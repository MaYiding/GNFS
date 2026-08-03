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

  private let resolver: GNFSExecutableResolver
  private var process: Process?
  private var rootProcessIdentity: ManagedProcessIdentity?
  private var readerTask: Task<Void, Never>?
  private var stderrTask: Task<String, Never>?
  private var teardownTask: Task<Void, Never>?

  init(resolver: GNFSExecutableResolver = GNFSExecutableResolver()) {
    self.resolver = resolver
  }

  func start(
    configuration: RunConfiguration,
    workspace: RunWorkspace
  ) async throws -> AsyncThrowingStream<CLIEvent, Error> {
    try Task.checkCancellation()
    guard process == nil else { throw GNFSRunnerError.alreadyRunning }

    let executable = try resolver.resolve()
    try FileManager.default.createDirectory(
      at: workspace.directory,
      withIntermediateDirectories: true
    )
    try Task.checkCancellation()

    let process = Process()
    let standardOutput = Pipe()
    let standardError = Pipe()
    process.executableURL = executable
    process.arguments = GNFSInvocation.arguments(for: configuration)
    process.standardOutput = standardOutput
    process.standardError = standardError
    process.currentDirectoryURL = workspace.directory
    var environment = ProcessInfo.processInfo.environment
    environment["GNFS_RESUME"] = workspace.resumeBase.path
    environment["LC_ALL"] = "en_US.UTF-8"
    process.environment = environment

    do {
      try process.run()
    } catch {
      throw GNFSRunnerError.launchFailed(error.localizedDescription)
    }
    guard let rootProcessIdentity = Self.managedProcessIdentity(
      for: process.processIdentifier
    ) else {
      process.terminate()
      process.waitUntilExit()
      throw GNFSRunnerError.launchFailed("无法绑定运算引擎进程身份。")
    }
    self.process = process
    self.rootProcessIdentity = rootProcessIdentity

    let (stream, continuation) = AsyncThrowingStream<CLIEvent, Error>.makeStream(
      bufferingPolicy: .bufferingNewest(Self.stdoutEventBufferLimit)
    )
    let stderrTask = Task.detached(priority: .utility) {
      await Self.readDiagnosticTail(
        from: standardError.fileHandleForReading.fileDescriptor,
        limit: Self.diagnosticTailLimit
      )
    }
    let task = Task.detached(priority: .userInitiated) {
      let decoder = JSONDecoder()
      var terminalEvent: CLIEvent?
      var expectedExitStatus: Int32?
      var lineBytes: [UInt8] = []
      lineBytes.reserveCapacity(4_096)
      var readBuffer = [UInt8](repeating: 0, count: 64 * 1_024)
      let stdoutDescriptor = standardOutput.fileHandleForReading.fileDescriptor
      do {
        stdoutLoop: while !Task.isCancelled {
          let bytesRead = try Self.readChunk(from: stdoutDescriptor, into: &readBuffer)
          guard bytesRead > 0 else { break stdoutLoop }
          for byte in readBuffer.prefix(bytesRead) {
            guard !Task.isCancelled else { break }
            guard terminalEvent == nil else {
              throw GNFSRunnerError.protocolViolation("终态事件之后仍有标准输出。")
            }
            if byte != 0x0A {
              guard lineBytes.count < Self.maxEventLineBytes else {
                throw GNFSRunnerError.protocolViolation(
                  "单个 JSONL 事件超过 \(Self.maxEventLineBytes) 字节上限。"
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

        if !Task.isCancelled && !lineBytes.isEmpty {
          throw GNFSRunnerError.protocolViolation("JSONL 事件未以换行符结束。")
        }

        process.waitUntilExit()
        let stderr = await stderrTask.value
        if Task.isCancelled {
          continuation.finish()
          return
        }
        guard let terminalEvent, let expectedExitStatus else {
          continuation.finish(
            throwing: GNFSRunnerError.unexpectedTermination(
              process.terminationStatus,
              stderr
            ))
          return
        }
        guard process.terminationStatus == expectedExitStatus else {
          continuation.finish(
            throwing: GNFSRunnerError.terminalStatusMismatch(
              expected: expectedExitStatus,
              actual: process.terminationStatus,
              stderr: stderr
            ))
          return
        }
        continuation.yield(terminalEvent)
        continuation.finish()
      } catch is CancellationError {
        continuation.finish()
      } catch {
        continuation.finish(throwing: error)
      }
    }
    continuation.onTermination = { @Sendable _ in
      task.cancel()
      stderrTask.cancel()
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

    guard process != nil || readerTask != nil || stderrTask != nil else { return }
    let rootProcessIdentity = rootProcessIdentity
    let outputReader = readerTask
    let errorReader = stderrTask
    let teardown = Task {
      if let rootProcessIdentity {
        var processTree = await Self.freezeProcessTree(
          rootedAt: rootProcessIdentity
        )
        Self.signalProcessTree(processTree, signal: SIGINT, resumeAfterSignal: true)
        processTree = await Self.waitForProcessTreeToStop(
          processTree,
          for: .milliseconds(350)
        )

        if Self.hasLivingProcesses(processTree) {
          processTree = await Self.freezeProcessTree(processTree)
          Self.signalProcessTree(processTree, signal: SIGTERM, resumeAfterSignal: true)
          processTree = await Self.waitForProcessTreeToStop(
            processTree,
            for: .seconds(1)
          )
        }

        if Self.hasLivingProcesses(processTree) {
          // A TERM-resistant process can spawn again during the grace period.
          // Freeze and close the descendant set one final time before SIGKILL.
          processTree = await Self.freezeProcessTree(processTree)
          Self.signalProcessTree(processTree, signal: SIGKILL, resumeAfterSignal: false)
          _ = await Self.waitForProcessTreeToStop(processTree, for: .seconds(1))
        }
      }

      outputReader?.cancel()
      errorReader?.cancel()
      await outputReader?.value
      _ = await errorReader?.value
    }
    teardownTask = teardown
    await teardown.value
    self.process = nil
    self.rootProcessIdentity = nil
    readerTask = nil
    stderrTask = nil
    teardownTask = nil
  }

  private static func freezeProcessTree(
    rootedAt rootIdentity: ManagedProcessIdentity
  ) async -> ManagedProcessTree {
    await freezeProcessTree([
      rootIdentity.processID: ManagedProcessNode(identity: rootIdentity, depth: 0)
    ])
  }

  private static func freezeProcessTree(
    _ knownProcesses: ManagedProcessTree
  ) async -> ManagedProcessTree {
    var processTree = knownProcesses.filter { isSafeManagedProcessID($0.key) }
    var stablePasses = 0

    // Stopping each discovered node prevents it from extending the tree while
    // the next closure pass is being collected. Two stable passes cover signal
    // delivery latency without placing an unbounded wait on cancellation.
    for _ in 0..<64 {
      for node in processTree.values where isProcessAlive(node) {
        signalProcessOrOwnedGroup(node, signal: SIGSTOP)
      }

      var discoveredProcess = false
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

      stablePasses = discoveredProcess || !childSnapshotWasComplete ? 0 : stablePasses + 1
      if stablePasses >= 2 { break }
      try? await Task.sleep(for: .milliseconds(5))
    }
    return processTree
  }

  private static func waitForProcessTreeToStop(
    _ knownProcesses: ManagedProcessTree,
    for timeout: Duration
  ) async -> ManagedProcessTree {
    var processTree = knownProcesses
    let clock = ContinuousClock()
    let deadline = clock.now.advanced(by: timeout)
    while clock.now < deadline {
      processTree = discoverProcessTree(from: processTree)
      if !hasLivingProcesses(processTree) { return processTree }
      try? await Task.sleep(for: .milliseconds(20))
    }
    return discoverProcessTree(from: processTree)
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
    managedProcessIdentity(for: node.identity.processID) == node.identity
  }

  private nonisolated static func managedProcessIdentity(
    for processID: Int32,
    expectedParentID: Int32? = nil
  ) -> ManagedProcessIdentity? {
    guard isSafeManagedProcessID(processID) else { return nil }
    var processInfo = proc_bsdinfo()
    let expectedSize = Int32(MemoryLayout<proc_bsdinfo>.size)
    let result = withUnsafeMutablePointer(to: &processInfo) { pointer in
      proc_pidinfo(processID, PROC_PIDTBSDINFO, 0, pointer, expectedSize)
    }
    guard result == expectedSize, processInfo.pbi_pid == UInt32(processID) else {
      return nil
    }
    if let expectedParentID, processInfo.pbi_ppid != UInt32(expectedParentID) {
      return nil
    }
    return ManagedProcessIdentity(
      processID: processID,
      startSeconds: processInfo.pbi_start_tvsec,
      startMicroseconds: processInfo.pbi_start_tvusec
    )
  }

  private nonisolated static func isSafeManagedProcessID(_ processID: Int32) -> Bool {
    processID > 1 && processID != Darwin.getpid()
  }

  private nonisolated static func readDiagnosticTail(
    from descriptor: Int32,
    limit: Int
  ) async -> String {
    var bytes = ByteRingBuffer(capacity: limit)
    var readBuffer = [UInt8](repeating: 0, count: 64 * 1_024)
    do {
      while !Task.isCancelled {
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
      try Task.checkCancellation()
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
