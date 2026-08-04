import Darwin
@preconcurrency import Foundation

enum GNFSRunnerError: LocalizedError, Equatable {
  case alreadyRunning
  case launchFailed(String)
  case malformedEvent(String)
  case unsupportedSchema(Int)
  case protocolViolation(String)
  case terminalStatusMismatch(expected: Int32, actual: Int32, stderr: String)
  case unexpectedTermination(Int32, String)

  var errorDescription: String? {
    switch self {
    case .alreadyRunning:
      "已有分解任务正在运行。"
    case .launchFailed(let message):
      "无法启动 GNFS 运算引擎：\(message)"
    case .malformedEvent(let line):
      "运算引擎返回了无法识别的数据：\(line.prefix(160))"
    case .unsupportedSchema(let version):
      "运算引擎事件协议版本不兼容（版本 \(version)）。"
    case .protocolViolation(let message):
      "运算引擎违反事件协议：\(message)"
    case .terminalStatusMismatch(let expected, let actual, let stderr):
      stderr.isEmpty
        ? "运算引擎终态与退出状态不匹配（预期 \(expected)，实际 \(actual)）。"
        : "运算引擎终态与退出状态不匹配（预期 \(expected)，实际 \(actual)）：\(stderr)"
    case .unexpectedTermination(let status, let stderr):
      stderr.isEmpty
        ? "运算引擎意外退出（状态 \(status)）。"
        : "运算引擎意外退出（状态 \(status)）：\(stderr)"
    }
  }
}

protocol GNFSRunning: Sendable {
  func start(
    configuration: RunConfiguration,
    workspace: RunWorkspace
  ) async throws -> AsyncThrowingStream<CLIEvent, Error>

  func cancel() async
}

actor ProcessGNFSRunner: GNFSRunning {
  nonisolated static let stdoutEventBufferLimit = 512
  nonisolated static let maxEventLineBytes = 1_024 * 1_024
  nonisolated static let diagnosticTailLimit = 64 * 1_024

  private struct ByteRingBuffer {
    private var storage: [UInt8]
    private var count = 0
    private var nextIndex = 0
    private(set) var didTruncate = false

    init(capacity: Int) {
      storage = Array(repeating: 0, count: capacity)
    }

    mutating func append(_ byte: UInt8) {
      guard !storage.isEmpty else {
        didTruncate = true
        return
      }
      if count < storage.count {
        storage[count] = byte
        count += 1
        return
      }
      storage[nextIndex] = byte
      nextIndex = (nextIndex + 1) % storage.count
      didTruncate = true
    }

    func data() -> Data {
      guard didTruncate else { return Data(storage.prefix(count)) }
      return Data(storage[nextIndex...] + storage[..<nextIndex])
    }
  }

  private struct ManagedProcessIdentity: Equatable, Sendable {
    let processID: Int32
    let startSeconds: UInt64
    let startMicroseconds: UInt64
  }

  private struct ManagedProcessNode: Sendable {
    let identity: ManagedProcessIdentity
    let depth: Int
  }

  private struct ChildProcessSnapshot: Sendable {
    let processIDs: [Int32]
    let isComplete: Bool
  }

  private typealias ManagedProcessTree = [Int32: ManagedProcessNode]

  private struct LaunchedProcess: Sendable {
    let processID: Int32
    let stdoutDescriptor: Int32
    let stderrDescriptor: Int32
  }

  private struct StdoutTerminal: Sendable {
    let event: CLIEvent?
    let expectedExitStatus: Int32?
  }

  struct PipeDuplicateAction: Equatable, Sendable {
    let source: Int32
    let destination: Int32
  }

  struct PipeFileActionPlan: Equatable, Sendable {
    let duplicates: [PipeDuplicateAction]
    let closures: [Int32]
  }

  private actor ProcessExitLatch {
    private let processID: Int32
    private var waiter: Task<Int32, Never>?

    init(processID: Int32) {
      self.processID = processID
    }

    func status() async -> Int32 {
      if let waiter { return await waiter.value }
      let processID = processID
      let waiter = Task {
        await ProcessGNFSRunner.waitForProcessAsync(processID)
      }
      self.waiter = waiter
      return await waiter.value
    }
  }

  private actor ManagedProcessTracker {
    private let root: ManagedProcessNode
    private let sessionID: Int32
    private var processTree: ManagedProcessTree
    private var terminationTask: Task<ManagedProcessTree, Never>?
    private var hasTerminated = false

    init(rootIdentity: ManagedProcessIdentity) {
      let root = ManagedProcessNode(identity: rootIdentity, depth: 0)
      self.root = root
      self.sessionID = rootIdentity.processID
      self.processTree = [rootIdentity.processID: root]
    }

    func monitor(pollInterval: Duration) async {
      while !Task.isCancelled {
        do {
          try await Task.sleep(for: pollInterval)
        } catch {
          return
        }
        guard !Task.isCancelled else { return }
        if !ProcessGNFSRunner.isProcessAlive(root) {
          await terminate()
          return
        }
      }
    }

    func terminate() async {
      if let terminationTask {
        processTree = await terminationTask.value
        hasTerminated = true
        return
      }
      guard !hasTerminated else { return }

      let knownProcesses = processTree
      let sessionID = sessionID
      let task = Task.detached(priority: .userInitiated) {
        await ProcessGNFSRunner.terminateProcessTree(
          knownProcesses,
          sessionID: sessionID
        )
      }
      terminationTask = task
      processTree = await task.value
      hasTerminated = true
      terminationTask = nil
    }
  }

  private let resolver: GNFSExecutableResolver
  private let processTrackingPollInterval: Duration
  private var processID: Int32?
  private var rootProcessIdentity: ManagedProcessIdentity?
  private var processTracker: ManagedProcessTracker?
  private var processTrackerTask: Task<Void, Never>?
  private var readerTask: Task<Void, Never>?
  private var stderrTask: Task<String, Never>?
  private var teardownTask: Task<Void, Never>?

  init(
    resolver: GNFSExecutableResolver = GNFSExecutableResolver(),
    processTrackingPollInterval: Duration = .milliseconds(100)
  ) {
    self.resolver = resolver
    self.processTrackingPollInterval = processTrackingPollInterval
  }

  func start(
    configuration: RunConfiguration,
    workspace: RunWorkspace
  ) async throws -> AsyncThrowingStream<CLIEvent, Error> {
    try Task.checkCancellation()
    guard processID == nil else { throw GNFSRunnerError.alreadyRunning }

    let executable = try resolver.resolve()
    try FileManager.default.createDirectory(
      at: workspace.directory,
      withIntermediateDirectories: true
    )
    try Task.checkCancellation()

    var environment = ProcessInfo.processInfo.environment
    environment["GNFS_RESUME"] = workspace.resumeBase.path
    environment["LC_ALL"] = "en_US.UTF-8"
    let launchedProcess = try Self.launchProcess(
      executable: executable,
      arguments: GNFSInvocation.arguments(for: configuration),
      environment: environment,
      currentDirectory: workspace.directory
    )
    let exitLatch = ProcessExitLatch(processID: launchedProcess.processID)
    guard let rootProcessIdentity = Self.managedProcessIdentity(for: launchedProcess.processID),
      Darwin.getsid(launchedProcess.processID) == launchedProcess.processID,
      Darwin.getpgid(launchedProcess.processID) == launchedProcess.processID
    else {
      _ = Darwin.kill(launchedProcess.processID, SIGKILL)
      _ = await exitLatch.status()
      Darwin.close(launchedProcess.stdoutDescriptor)
      Darwin.close(launchedProcess.stderrDescriptor)
      throw GNFSRunnerError.launchFailed("无法绑定运算引擎的独立会话身份。")
    }
    self.processID = launchedProcess.processID
    self.rootProcessIdentity = rootProcessIdentity
    let processTracker = ManagedProcessTracker(rootIdentity: rootProcessIdentity)
    let processTrackingPollInterval = processTrackingPollInterval
    let processTrackerTask = Task.detached(priority: .userInitiated) {
      await processTracker.monitor(pollInterval: processTrackingPollInterval)
    }
    let rootProcess = ManagedProcessNode(identity: rootProcessIdentity, depth: 0)
    self.processTracker = processTracker
    self.processTrackerTask = processTrackerTask
    guard Darwin.kill(launchedProcess.processID, SIGCONT) == 0 else {
      let code = errno
      await processTracker.terminate()
      processTrackerTask.cancel()
      await processTrackerTask.value
      _ = await exitLatch.status()
      Darwin.close(launchedProcess.stdoutDescriptor)
      Darwin.close(launchedProcess.stderrDescriptor)
      self.processID = nil
      self.rootProcessIdentity = nil
      self.processTracker = nil
      self.processTrackerTask = nil
      throw GNFSRunnerError.launchFailed(String(cString: strerror(code)))
    }

    let (stream, continuation) = AsyncThrowingStream<CLIEvent, Error>.makeStream(
      bufferingPolicy: .bufferingNewest(Self.stdoutEventBufferLimit)
    )
    let stderrTask = Task.detached(priority: .utility) {
      await Self.readDiagnosticTail(
        from: launchedProcess.stderrDescriptor,
        limit: Self.diagnosticTailLimit
      )
    }
    let task = Task.detached(priority: .userInitiated) {
      do {
        let stdoutTerminal = try await Self.readStdoutEvents(
          from: launchedProcess.stdoutDescriptor,
          continuation: continuation
        )
        await Self.waitForProcessExitWithoutReaping(rootProcess)
        await processTracker.terminate()
        let terminationStatus = await exitLatch.status()
        processTrackerTask.cancel()
        await processTrackerTask.value
        let stderr = await stderrTask.value
        if Task.isCancelled {
          continuation.finish()
          return
        }
        guard let terminalEvent = stdoutTerminal.event,
          let expectedExitStatus = stdoutTerminal.expectedExitStatus
        else {
          continuation.finish(
            throwing: GNFSRunnerError.unexpectedTermination(
              terminationStatus,
              stderr
            ))
          return
        }
        guard terminationStatus == expectedExitStatus else {
          continuation.finish(
            throwing: GNFSRunnerError.terminalStatusMismatch(
              expected: expectedExitStatus,
              actual: terminationStatus,
              stderr: stderr
            ))
          return
        }
        continuation.yield(terminalEvent)
        continuation.finish()
      } catch is CancellationError {
        await processTracker.terminate()
        _ = await exitLatch.status()
        processTrackerTask.cancel()
        await processTrackerTask.value
        stderrTask.cancel()
        _ = await stderrTask.value
        continuation.finish()
      } catch {
        await processTracker.terminate()
        _ = await exitLatch.status()
        processTrackerTask.cancel()
        await processTrackerTask.value
        stderrTask.cancel()
        _ = await stderrTask.value
        continuation.finish(throwing: error)
      }
    }
    continuation.onTermination = { @Sendable _ in
      task.cancel()
      stderrTask.cancel()
      Task.detached(priority: .userInitiated) {
        await processTracker.terminate()
      }
    }
    readerTask = task
    self.stderrTask = stderrTask
    return stream
  }

  func cancel() async {
    if let teardownTask {
      await teardownTask.value
      return
    }

    guard processID != nil || readerTask != nil || stderrTask != nil else { return }
    let rootProcessIdentity = rootProcessIdentity
    let processTracker = processTracker
    let processTrackerTask = processTrackerTask
    let outputReader = readerTask
    let errorReader = stderrTask
    // Establish cancellation intent before signals can make the root exit and
    // allow the reader to report a protocol outcome.
    outputReader?.cancel()
    errorReader?.cancel()
    let teardown = Task {
      if let processTracker {
        await processTracker.terminate()
      } else if let rootProcessIdentity {
        _ = await Self.terminateProcessTree([
          rootProcessIdentity.processID: ManagedProcessNode(
            identity: rootProcessIdentity,
            depth: 0
          )
        ], sessionID: rootProcessIdentity.processID)
      }
      processTrackerTask?.cancel()
      await processTrackerTask?.value

      await outputReader?.value
      _ = await errorReader?.value
    }
    teardownTask = teardown
    await teardown.value
    self.processID = nil
    self.rootProcessIdentity = nil
    self.processTracker = nil
    self.processTrackerTask = nil
    readerTask = nil
    stderrTask = nil
    teardownTask = nil
  }

  private static func terminateProcessTree(
    _ knownProcesses: ManagedProcessTree,
    sessionID: Int32
  ) async -> ManagedProcessTree {
    var processTree = await freezeProcessTree(knownProcesses, sessionID: sessionID)
    signalProcessTree(processTree, signal: SIGINT, resumeAfterSignal: true)
    processTree = await waitForProcessTreeToStop(
      processTree,
      for: .milliseconds(350),
      sessionID: sessionID
    )
    processTree = await freezeProcessTree(processTree, sessionID: sessionID)

    if hasLivingProcesses(processTree) {
      signalProcessTree(processTree, signal: SIGTERM, resumeAfterSignal: true)
      processTree = await waitForProcessTreeToStop(
        processTree,
        for: .seconds(1),
        sessionID: sessionID
      )
      processTree = await freezeProcessTree(processTree, sessionID: sessionID)
    }

    if hasLivingProcesses(processTree) {
      // A TERM-resistant process can spawn again during the grace period.
      // The preceding phase closure has frozen every surviving session member.
      signalProcessTree(processTree, signal: SIGKILL, resumeAfterSignal: false)
      processTree = await waitForProcessTreeToStop(
        processTree,
        for: .seconds(1),
        sessionID: sessionID
      )
    }
    return processTree
  }

  private static func freezeProcessTree(
    _ knownProcesses: ManagedProcessTree,
    sessionID: Int32
  ) async -> ManagedProcessTree {
    var processTree = discoverSessionProcesses(
      sessionID: sessionID,
      from: knownProcesses.filter { isSafeManagedProcessID($0.key) }
    )
    var stablePasses = 0

    // Stopping each discovered node prevents it from extending the tree while
    // the next closure pass is being collected. Two stable passes cover signal
    // delivery latency without placing an unbounded wait on cancellation.
    for _ in 0..<64 {
      let countBeforeDiscovery = processTree.count
      processTree = discoverSessionProcesses(sessionID: sessionID, from: processTree)
      for node in processTree.values where isProcessAlive(node) {
        signalProcessOrOwnedGroup(node, signal: SIGSTOP)
      }

      var discoveredProcess = processTree.count != countBeforeDiscovery
      var childSnapshotWasComplete = true
      for parent in processTree.values.sorted(by: { $0.depth < $1.depth })
      where isProcessAlive(parent) {
        let children = directChildProcessIDs(of: parent.identity.processID)
        childSnapshotWasComplete = childSnapshotWasComplete && children.isComplete
        for childID in children.processIDs where isSafeManagedProcessID(childID) {
          guard let childIdentity = managedProcessIdentity(
            for: childID,
            expectedParentID: parent.identity.processID
          ) else { continue }
          let existingIdentity = processTree[childID]?.identity
          if existingIdentity != childIdentity {
            processTree[childID] = ManagedProcessNode(
              identity: childIdentity,
              depth: parent.depth + 1
            )
            discoveredProcess = true
          }
          if let child = processTree[childID] {
            signalProcessOrOwnedGroup(child, signal: SIGSTOP)
          }
        }
      }

      let countBeforeSessionClosure = processTree.count
      processTree = discoverSessionProcesses(sessionID: sessionID, from: processTree)
      discoveredProcess = discoveredProcess || processTree.count != countBeforeSessionClosure
      for node in processTree.values where isProcessAlive(node) {
        signalProcessOrOwnedGroup(node, signal: SIGSTOP)
      }

      stablePasses = discoveredProcess || !childSnapshotWasComplete ? 0 : stablePasses + 1
      if stablePasses >= 2 { break }
      try? await Task.sleep(for: .milliseconds(5))
    }
    return processTree
  }

  private static func waitForProcessTreeToStop(
    _ knownProcesses: ManagedProcessTree,
    for timeout: Duration,
    sessionID: Int32
  ) async -> ManagedProcessTree {
    var processTree = knownProcesses
    let clock = ContinuousClock()
    let deadline = clock.now.advanced(by: timeout)
    var stableEmptyPasses = 0
    while clock.now < deadline {
      processTree = discoverSessionProcesses(sessionID: sessionID, from: processTree)
      if hasLivingProcesses(processTree) {
        stableEmptyPasses = 0
      } else {
        stableEmptyPasses += 1
        if stableEmptyPasses >= 2 {
          let finalSnapshot = discoverSessionProcesses(
            sessionID: sessionID,
            from: processTree
          )
          if !hasLivingProcesses(finalSnapshot) { return finalSnapshot }
          processTree = finalSnapshot
          stableEmptyPasses = 0
        }
      }
      try? await Task.sleep(for: .milliseconds(20))
    }
    return discoverSessionProcesses(sessionID: sessionID, from: processTree)
  }

  private nonisolated static func discoverProcessTree(
    from knownProcesses: ManagedProcessTree
  ) -> ManagedProcessTree {
    var processTree = knownProcesses
    var pending = processTree.values.sorted(by: { $0.depth < $1.depth })
    var nextIndex = 0
    while nextIndex < pending.count {
      let parent = pending[nextIndex]
      nextIndex += 1
      guard isProcessAlive(parent) else { continue }
      let children = directChildProcessIDs(of: parent.identity.processID)
      for childID in children.processIDs where isSafeManagedProcessID(childID) {
        guard let childIdentity = managedProcessIdentity(
          for: childID,
          expectedParentID: parent.identity.processID
        ) else { continue }
        if processTree[childID]?.identity == childIdentity { continue }
        let child = ManagedProcessNode(identity: childIdentity, depth: parent.depth + 1)
        processTree[childID] = child
        pending.append(child)
      }
    }
    return processTree
  }

  private nonisolated static func discoverSessionProcesses(
    sessionID: Int32,
    from knownProcesses: ManagedProcessTree
  ) -> ManagedProcessTree {
    var processTree = discoverProcessTree(from: knownProcesses)
    for processID in allProcessIDs() where isSafeManagedProcessID(processID) {
      guard let identityBefore = managedProcessIdentity(for: processID),
        Darwin.getsid(processID) == sessionID,
        let identityAfter = managedProcessIdentity(for: processID),
        identityBefore == identityAfter,
        Darwin.getsid(processID) == sessionID
      else { continue }
      if processTree[processID]?.identity == identityAfter { continue }
      processTree[processID] = ManagedProcessNode(
        identity: identityAfter,
        depth: processID == sessionID ? 0 : 1
      )
    }
    return discoverProcessTree(from: processTree)
  }

  private nonisolated static func allProcessIDs() -> [Int32] {
    let maximumCapacity = 1_048_576
    let capacityHint = max(0, Int(proc_listallpids(nil, 0)))
    var capacity = min(maximumCapacity, max(1_024, capacityHint + 128))
    while true {
      var processIDs = [Int32](repeating: 0, count: capacity)
      let count = processIDs.withUnsafeMutableBytes { buffer in
        proc_listallpids(buffer.baseAddress, Int32(buffer.count))
      }
      guard count >= 0 else { return [] }
      let returnedCount = min(Int(count), capacity)
      let safeProcessIDs = processIDs.prefix(returnedCount).filter(isSafeManagedProcessID)
      if count < capacity || capacity == maximumCapacity {
        return safeProcessIDs
      }
      capacity = min(maximumCapacity, capacity * 2)
    }
  }

  private nonisolated static func signalProcessTree(
    _ processTree: ManagedProcessTree,
    signal: Int32,
    resumeAfterSignal: Bool
  ) {
    let leafFirst = processTree.values.sorted {
      if $0.depth != $1.depth { return $0.depth > $1.depth }
      return $0.identity.processID > $1.identity.processID
    }
    for node in leafFirst where isProcessAlive(node) {
      signalProcessOrOwnedGroup(node, signal: signal)
    }
    guard resumeAfterSignal else { return }
    for node in leafFirst where isProcessAlive(node) {
      signalProcessOrOwnedGroup(node, signal: SIGCONT)
    }
  }

  private nonisolated static func signalProcessOrOwnedGroup(
    _ node: ManagedProcessNode,
    signal: Int32
  ) {
    guard isProcessAlive(node) else { return }
    let processID = node.identity.processID
    let processGroup = Darwin.getpgid(processID)
    guard isProcessAlive(node) else { return }
    if processGroup == processID {
      _ = Darwin.kill(-processGroup, signal)
    } else {
      _ = Darwin.kill(processID, signal)
    }
  }

  private nonisolated static func directChildProcessIDs(
    of processID: Int32
  ) -> ChildProcessSnapshot {
    let maximumCapacity = 1_048_576
    let capacityHint = max(0, Int(proc_listchildpids(processID, nil, 0)))
    var capacity = min(maximumCapacity, max(16, capacityHint + 16))
    while true {
      var childProcessIDs = [Int32](repeating: 0, count: capacity)
      let count = childProcessIDs.withUnsafeMutableBytes { buffer in
        proc_listchildpids(processID, buffer.baseAddress, Int32(buffer.count))
      }
      guard count >= 0 else {
        return ChildProcessSnapshot(processIDs: [], isComplete: false)
      }
      let returnedCount = min(Int(count), capacity)
      let safeProcessIDs = childProcessIDs.prefix(returnedCount)
        .filter(isSafeManagedProcessID)
      if count < capacity {
        return ChildProcessSnapshot(processIDs: safeProcessIDs, isComplete: true)
      }
      if capacity == maximumCapacity {
        return ChildProcessSnapshot(processIDs: safeProcessIDs, isComplete: false)
      }
      capacity = min(maximumCapacity, capacity * 2)
    }
  }

  private nonisolated static func hasLivingProcesses(
    _ processTree: ManagedProcessTree
  ) -> Bool {
    processTree.values.contains(where: isProcessAlive)
  }

  private nonisolated static func isProcessAlive(_ node: ManagedProcessNode) -> Bool {
    guard let processInfo = managedProcessInfo(for: node.identity.processID),
      processInfo.pbi_status != UInt32(SZOMB)
    else { return false }
    return managedProcessIdentity(from: processInfo) == node.identity
  }

  private nonisolated static func waitForProcessExitWithoutReaping(
    _ node: ManagedProcessNode
  ) async {
    while isProcessAlive(node) {
      do {
        try await Task.sleep(for: .milliseconds(10))
      } catch {
        return
      }
    }
  }

  private nonisolated static func managedProcessIdentity(
    for processID: Int32,
    expectedParentID: Int32? = nil
  ) -> ManagedProcessIdentity? {
    guard let processInfo = managedProcessInfo(for: processID) else { return nil }
    if let expectedParentID, processInfo.pbi_ppid != UInt32(expectedParentID) {
      return nil
    }
    return managedProcessIdentity(from: processInfo)
  }

  private nonisolated static func managedProcessInfo(
    for processID: Int32
  ) -> proc_bsdinfo? {
    guard isSafeManagedProcessID(processID) else { return nil }
    var processInfo = proc_bsdinfo()
    let expectedSize = Int32(MemoryLayout<proc_bsdinfo>.size)
    let result = withUnsafeMutablePointer(to: &processInfo) { pointer in
      proc_pidinfo(processID, PROC_PIDTBSDINFO, 0, pointer, expectedSize)
    }
    guard result == expectedSize, processInfo.pbi_pid == UInt32(processID) else {
      return nil
    }
    return processInfo
  }

  private nonisolated static func managedProcessIdentity(
    from processInfo: proc_bsdinfo
  ) -> ManagedProcessIdentity {
    return ManagedProcessIdentity(
      processID: Int32(processInfo.pbi_pid),
      startSeconds: processInfo.pbi_start_tvsec,
      startMicroseconds: processInfo.pbi_start_tvusec
    )
  }

  private nonisolated static func isSafeManagedProcessID(_ processID: Int32) -> Bool {
    processID > 1 && processID != Darwin.getpid()
  }

  private nonisolated static func launchProcess(
    executable: URL,
    arguments: [String],
    environment: [String: String],
    currentDirectory: URL
  ) throws -> LaunchedProcess {
    var stdoutPipe = [Int32](repeating: -1, count: 2)
    var stderrPipe = [Int32](repeating: -1, count: 2)
    guard Darwin.pipe(&stdoutPipe) == 0 else {
      throw GNFSRunnerError.launchFailed(String(cString: strerror(errno)))
    }
    guard Darwin.pipe(&stderrPipe) == 0 else {
      let code = errno
      Darwin.close(stdoutPipe[0])
      Darwin.close(stdoutPipe[1])
      throw GNFSRunnerError.launchFailed(String(cString: strerror(code)))
    }
    do {
      try normalizePipeDescriptors(&stdoutPipe)
      try normalizePipeDescriptors(&stderrPipe)
    } catch {
      closeDescriptors(stdoutPipe + stderrPipe)
      throw error
    }

    var fileActions: posix_spawn_file_actions_t?
    var attributes: posix_spawnattr_t?
    var fileActionsInitialized = false
    var attributesInitialized = false
    var didSpawn = false
    defer {
      if fileActionsInitialized { posix_spawn_file_actions_destroy(&fileActions) }
      if attributesInitialized { posix_spawnattr_destroy(&attributes) }
      Darwin.close(stdoutPipe[1])
      Darwin.close(stderrPipe[1])
      if !didSpawn {
        Darwin.close(stdoutPipe[0])
        Darwin.close(stderrPipe[0])
      }
    }

    var result = posix_spawn_file_actions_init(&fileActions)
    guard result == 0 else {
      throw GNFSRunnerError.launchFailed(String(cString: strerror(result)))
    }
    fileActionsInitialized = true
    result = posix_spawnattr_init(&attributes)
    guard result == 0 else {
      throw GNFSRunnerError.launchFailed(String(cString: strerror(result)))
    }
    attributesInitialized = true

    let pipeFileActionPlan = pipeFileActionPlan(
      stdoutPipe: stdoutPipe,
      stderrPipe: stderrPipe
    )
    for action in pipeFileActionPlan.duplicates {
      result = posix_spawn_file_actions_adddup2(
        &fileActions,
        action.source,
        action.destination
      )
      guard result == 0 else {
        throw GNFSRunnerError.launchFailed(String(cString: strerror(result)))
      }
    }
    for descriptor in pipeFileActionPlan.closures {
      result = posix_spawn_file_actions_addclose(&fileActions, descriptor)
      guard result == 0 else {
        throw GNFSRunnerError.launchFailed(String(cString: strerror(result)))
      }
    }
    result = currentDirectory.path.withCString { path in
      posix_spawn_file_actions_addchdir(&fileActions, path)
    }
    guard result == 0 else {
      throw GNFSRunnerError.launchFailed(String(cString: strerror(result)))
    }

    var emptySignalMask = sigset_t()
    var defaultSignals = sigset_t()
    guard Darwin.sigemptyset(&emptySignalMask) == 0,
      Darwin.sigemptyset(&defaultSignals) == 0
    else {
      throw GNFSRunnerError.launchFailed(String(cString: strerror(errno)))
    }
    for signalNumber in 1..<Int(NSIG) {
      let signal = Int32(signalNumber)
      if signal == SIGKILL || signal == SIGSTOP { continue }
      if Darwin.sigaddset(&defaultSignals, signal) != 0, errno != EINVAL {
        throw GNFSRunnerError.launchFailed(String(cString: strerror(errno)))
      }
    }
    result = posix_spawnattr_setsigmask(&attributes, &emptySignalMask)
    guard result == 0 else {
      throw GNFSRunnerError.launchFailed(String(cString: strerror(result)))
    }
    result = posix_spawnattr_setsigdefault(&attributes, &defaultSignals)
    guard result == 0 else {
      throw GNFSRunnerError.launchFailed(String(cString: strerror(result)))
    }

    let flags = Int16(
      POSIX_SPAWN_SETSID | POSIX_SPAWN_START_SUSPENDED | POSIX_SPAWN_CLOEXEC_DEFAULT
        | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF
    )
    result = posix_spawnattr_setflags(&attributes, flags)
    guard result == 0 else {
      throw GNFSRunnerError.launchFailed(String(cString: strerror(result)))
    }

    let argumentStrings = [executable.path] + arguments
    let environmentStrings = environment.keys.sorted().compactMap { key in
      environment[key].map { "\(key)=\($0)" }
    }
    let argumentPointers = argumentStrings.map { strdup($0) }
    let environmentPointers = environmentStrings.map { strdup($0) }
    guard argumentPointers.allSatisfy({ $0 != nil }),
      environmentPointers.allSatisfy({ $0 != nil })
    else {
      argumentPointers.forEach { free($0) }
      environmentPointers.forEach { free($0) }
      throw GNFSRunnerError.launchFailed("无法分配进程启动参数。")
    }
    defer {
      argumentPointers.forEach { free($0) }
      environmentPointers.forEach { free($0) }
    }
    var argv = argumentPointers + [nil]
    var envp = environmentPointers + [nil]
    var processID: Int32 = 0
    result = executable.path.withCString { path in
      argv.withUnsafeMutableBufferPointer { argumentsBuffer in
        envp.withUnsafeMutableBufferPointer { environmentBuffer in
          posix_spawn(
            &processID,
            path,
            &fileActions,
            &attributes,
            argumentsBuffer.baseAddress,
            environmentBuffer.baseAddress
          )
        }
      }
    }
    guard result == 0 else {
      throw GNFSRunnerError.launchFailed(String(cString: strerror(result)))
    }
    didSpawn = true
    return LaunchedProcess(
      processID: processID,
      stdoutDescriptor: stdoutPipe[0],
      stderrDescriptor: stderrPipe[0]
    )
  }

  static func pipeFileActionPlan(
    stdoutPipe: [Int32],
    stderrPipe: [Int32]
  ) -> PipeFileActionPlan {
    precondition(stdoutPipe.count == 2 && stderrPipe.count == 2)
    precondition((stdoutPipe + stderrPipe).allSatisfy { $0 > STDERR_FILENO })
    return PipeFileActionPlan(
      duplicates: [
        PipeDuplicateAction(source: stdoutPipe[1], destination: STDOUT_FILENO),
        PipeDuplicateAction(source: stderrPipe[1], destination: STDERR_FILENO),
      ],
      closures: stdoutPipe + stderrPipe
    )
  }

  private nonisolated static func normalizePipeDescriptors(
    _ descriptors: inout [Int32]
  ) throws {
    for index in descriptors.indices where descriptors[index] <= STDERR_FILENO {
      let descriptor = descriptors[index]
      let duplicate = Darwin.fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1)
      guard duplicate >= 0 else {
        throw GNFSRunnerError.launchFailed(String(cString: strerror(errno)))
      }
      descriptors[index] = duplicate
      Darwin.close(descriptor)
    }
  }

  private nonisolated static func closeDescriptors(_ descriptors: [Int32]) {
    for descriptor in Set(descriptors) where descriptor >= 0 {
      Darwin.close(descriptor)
    }
  }

  private nonisolated static func waitForProcess(_ processID: Int32) -> Int32 {
    var status: Int32 = 0
    while true {
      let result = Darwin.waitpid(processID, &status, 0)
      if result == processID { break }
      if result < 0, errno == EINTR { continue }
      return -1
    }
    let terminationSignal = status & 0x7f
    if terminationSignal == 0 {
      return (status >> 8) & 0xff
    }
    return terminationSignal
  }

  private nonisolated static func waitForProcessAsync(_ processID: Int32) async -> Int32 {
    await withCheckedContinuation { continuation in
      DispatchQueue.global(qos: .utility).async {
        continuation.resume(returning: waitForProcess(processID))
      }
    }
  }

  private nonisolated static func readStdoutEvents(
    from descriptor: Int32,
    continuation: AsyncThrowingStream<CLIEvent, Error>.Continuation
  ) async throws -> StdoutTerminal {
    try await withCheckedThrowingContinuation { checkedContinuation in
      DispatchQueue.global(qos: .userInitiated).async {
        do {
          checkedContinuation.resume(
            returning: try readStdoutEventsSynchronously(
              from: descriptor,
              continuation: continuation
            ))
        } catch {
          checkedContinuation.resume(throwing: error)
        }
      }
    }
  }

  private nonisolated static func readStdoutEventsSynchronously(
    from descriptor: Int32,
    continuation: AsyncThrowingStream<CLIEvent, Error>.Continuation
  ) throws -> StdoutTerminal {
    defer { Darwin.close(descriptor) }
    let decoder = JSONDecoder()
    var terminalEvent: CLIEvent?
    var expectedExitStatus: Int32?
    var lineBytes: [UInt8] = []
    lineBytes.reserveCapacity(4_096)
    var readBuffer = [UInt8](repeating: 0, count: 64 * 1_024)

    stdoutLoop: while true {
      let bytesRead = try readChunk(from: descriptor, into: &readBuffer)
      guard bytesRead > 0 else { break stdoutLoop }
      for byte in readBuffer.prefix(bytesRead) {
        guard terminalEvent == nil else {
          throw GNFSRunnerError.protocolViolation("终态事件之后仍有标准输出。")
        }
        if byte != 0x0A {
          guard lineBytes.count < maxEventLineBytes else {
            throw GNFSRunnerError.protocolViolation(
              "单个 JSONL 事件超过 \(maxEventLineBytes) 字节上限。"
            )
          }
          lineBytes.append(byte)
          continue
        }

        let data = Data(lineBytes)
        let diagnosticLine = String(decoding: lineBytes.prefix(160), as: UTF8.self)
        lineBytes.removeAll(keepingCapacity: true)
        let event: CLIEvent
        do {
          event = try decoder.decode(CLIEvent.self, from: data)
        } catch {
          throw GNFSRunnerError.malformedEvent(diagnosticLine)
        }
        guard event.schemaVersion == 1 else {
          throw GNFSRunnerError.unsupportedSchema(event.schemaVersion)
        }
        switch event.type {
        case .result:
          guard let result = event.result else {
            throw GNFSRunnerError.protocolViolation("result 事件缺少 result 字段。")
          }
          terminalEvent = event
          expectedExitStatus = result.success ? 0 : 1
        case .error:
          terminalEvent = event
          expectedExitStatus = 1
        case .started, .progress, .log:
          continuation.yield(event)
        }
      }
    }

    if !lineBytes.isEmpty {
      throw GNFSRunnerError.protocolViolation("JSONL 事件未以换行符结束。")
    }
    return StdoutTerminal(event: terminalEvent, expectedExitStatus: expectedExitStatus)
  }

  private nonisolated static func readDiagnosticTail(
    from descriptor: Int32,
    limit: Int
  ) async -> String {
    await withCheckedContinuation { continuation in
      DispatchQueue.global(qos: .utility).async {
        continuation.resume(returning: readDiagnosticTailSynchronously(from: descriptor, limit: limit))
      }
    }
  }

  private nonisolated static func readDiagnosticTailSynchronously(
    from descriptor: Int32,
    limit: Int
  ) -> String {
    defer { Darwin.close(descriptor) }
    var bytes = ByteRingBuffer(capacity: limit)
    var readBuffer = [UInt8](repeating: 0, count: 64 * 1_024)
    do {
      while true {
        let bytesRead = try readChunk(from: descriptor, into: &readBuffer)
        guard bytesRead > 0 else { break }
        for byte in readBuffer.prefix(bytesRead) {
          bytes.append(byte)
        }
      }
    } catch {
      // Diagnostic capture must never mask the primary process outcome.
    }
    let tail = String(decoding: bytes.data(), as: UTF8.self)
      .trimmingCharacters(in: .whitespacesAndNewlines)
    guard bytes.didTruncate else { return tail }
    return "[stderr truncated; showing last \(limit) bytes]\n" + tail
  }

  private nonisolated static func readChunk(
    from descriptor: Int32,
    into buffer: inout [UInt8]
  ) throws -> Int {
    while true {
      var pollDescriptor = pollfd(
        fd: descriptor,
        events: Int16(POLLIN | POLLHUP),
        revents: 0
      )
      let pollResult = Darwin.poll(&pollDescriptor, 1, 100)
      if pollResult == 0 { continue }
      if pollResult < 0 {
        if errno == EINTR { continue }
        throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
      }
      if pollDescriptor.revents & Int16(POLLNVAL) != 0 {
        throw NSError(domain: NSPOSIXErrorDomain, code: Int(EBADF))
      }
      let bytesRead = buffer.withUnsafeMutableBytes { rawBuffer in
        Darwin.read(descriptor, rawBuffer.baseAddress, rawBuffer.count)
      }
      if bytesRead >= 0 { return bytesRead }
      if errno == EINTR { continue }
      throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
    }
  }
}
