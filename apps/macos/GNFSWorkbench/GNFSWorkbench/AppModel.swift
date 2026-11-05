import AppKit
import Foundation
import Observation

enum HistoryRestorationState: Equatable {
  case loading
  case ready
  case failed
}

enum RunFinalizationState: Equatable {
  case pending
  case complete
  case failed(String)
}

@MainActor
@Observable
final class AppModel {
  var draftConfiguration = RunConfiguration()
  var activeRun: RunRecord?
  var history: [RunRecord] = []
  var selectedRunID: UUID?
  var isParametersPresented = false
  var isHistoryPresented = true
  var areLogsExpanded = true
  var alertMessage: String?
  var toastMessage: String?
  private(set) var isRunTaskActive = false
  private(set) var isHistoryMutationActive = false
  private(set) var historyRestorationState: HistoryRestorationState = .loading
  private(set) var runFinalizationStates: [UUID: RunFinalizationState] = [:]

  @ObservationIgnored private let runner: any GNFSRunning
  @ObservationIgnored private let store: any RunStoring
  @ObservationIgnored private var restorationTask: Task<Bool, Never>?
  @ObservationIgnored private var runTask: Task<Void, Never>?
  @ObservationIgnored private var saveTask: Task<Void, Never>?
  @ObservationIgnored private var toastTask: Task<Void, Never>?

  init(
    runner: (any GNFSRunning)? = nil,
    store: (any RunStoring)? = nil
  ) {
    let isDemo =
      ProcessInfo.processInfo.arguments.contains("--ui-testing")
      || ProcessInfo.processInfo.arguments.contains("--design-preview")
    let usesRealEngine = ProcessInfo.processInfo.arguments.contains("--ui-testing-real-engine")
    self.runner = runner ?? (isDemo && !usesRealEngine ? DemoGNFSRunner() : ProcessGNFSRunner())

    if let store {
      self.store = store
    } else if isDemo {
      let temporary = URL(fileURLWithPath: NSTemporaryDirectory())
        .appendingPathComponent(
          "GNFSWorkbench-Demo-\(UUID().uuidString)",
          isDirectory: true
        )
      self.store = RunStore(baseURL: temporary)
    } else {
      self.store = RunStore()
    }

    let restorationTask = Task { [weak self] in
      await self?.restoreHistory() ?? false
    }
    self.restorationTask = restorationTask
    if ProcessInfo.processInfo.arguments.contains("--design-preview") {
      Task { [weak self] in
        guard await restorationTask.value else { return }
        await self?.startRun()
      }
    }
  }

  var displayedRun: RunRecord? {
    if let selectedRunID {
      if activeRun?.id == selectedRunID { return activeRun }
      return history.first(where: { $0.id == selectedRunID })
    }
    return activeRun
  }

  var isRunning: Bool {
    activeRun?.status == .running || activeRun?.status == .cancelling
  }

  var inputValidationMessage: String? {
    if case .invalid(let message) = IntegerInputValidator.validate(draftConfiguration.number) {
      return message
    }
    return draftConfiguration.parameters.validationMessage
  }

  var canStart: Bool {
    inputValidationMessage == nil && historyRestorationState == .ready
      && !isRunTaskActive && !isHistoryMutationActive
  }

  var canEdit: Bool {
    historyRestorationState == .ready && !isRunTaskActive && !isHistoryMutationActive && !isRunning
  }

  func retryHistoryRestoration() async {
    guard historyRestorationState == .failed, !isRunTaskActive else { return }
    historyRestorationState = .loading
    alertMessage = nil
    let task = Task { [weak self] in await self?.restoreHistory() ?? false }
    restorationTask = task
    _ = await task.value
  }

  func runOrRepeat() async {
    if displayedRun != nil { await useConfigurationFromDisplayedRun() }
    await startRun()
  }

  func startRun() async {
    guard await waitForHistoryRestoration() else { return }
    guard !isRunTaskActive, !isHistoryMutationActive else { return }
    if let message = inputValidationMessage {
      alertMessage = message
      return
    }
    guard
      case .valid(let normalized) = IntegerInputValidator.validate(
        draftConfiguration.number
      )
    else {
      alertMessage = inputValidationMessage
      return
    }

    alertMessage = nil
    draftConfiguration.number = normalized
    var run = RunRecord(configuration: draftConfiguration)
    run.status = .running
    activeRun = run
    selectedRunID = run.id
    history.removeAll(where: { $0.id == run.id })
    history.insert(run, at: 0)
    history = Array(history.prefix(50))
    let retainedRunIDs = Set(history.map(\.id))
    runFinalizationStates = runFinalizationStates.filter { retainedRunIDs.contains($0.key) }
    runFinalizationStates[run.id] = .pending
    persistNow()
    let initialSaveTask = saveTask
    isRunTaskActive = true

    let runner = self.runner
    let store = self.store
    runTask = Task { [weak self] in
      guard let self else { return }
      do {
        await initialSaveTask?.value
        try Task.checkCancellation()
        let workspace = try await store.workspace(for: run.id)
        try Task.checkCancellation()
        let events = try await runner.start(
          configuration: run.configuration,
          workspace: workspace
        )
        for try await event in events {
          guard !Task.isCancelled else { break }
          self.consume(event)
        }
        await runner.cancel()
        self.finishIfNeeded()
      } catch is CancellationError {
        await runner.cancel()
        self.markCancelled()
      } catch {
        await runner.cancel()
        if self.activeRun?.status == .cancelling {
          self.markCancelled()
        } else {
          self.markFailed(error.localizedDescription)
        }
      }
      await self.finalizeRun(run.id)
      self.runTask = nil
      self.isRunTaskActive = false
    }
  }

  func cancelRun() {
    guard isRunning else { return }
    activeRun?.status = .cancelling
    syncActiveRun()
    let currentRunTask = runTask
    currentRunTask?.cancel()
    Task { [runner] in
      await runner.cancel()
      await currentRunTask?.value
    }
  }

  func prepareForTermination() async {
    if isRunTaskActive {
      if isRunning {
        activeRun?.status = .cancelling
        syncActiveRun()
      }
      let currentRunTask = runTask
      currentRunTask?.cancel()
      await runner.cancel()
      await currentRunTask?.value
      if activeRun?.status.isTerminal == false {
        markCancelled()
      }
    }
    if await waitForHistoryRestoration() {
      await flushHistory()
    }
  }

  func newRun() async {
    guard await waitForHistoryRestoration() else { return }
    guard !isRunTaskActive, !isHistoryMutationActive else { return }
    if let displayedRun {
      draftConfiguration = displayedRun.configuration
    }
    activeRun = nil
    selectedRunID = nil
    alertMessage = nil
  }

  func useConfigurationFromDisplayedRun() async {
    guard await waitForHistoryRestoration() else { return }
    guard !isRunTaskActive, !isHistoryMutationActive, let displayedRun else { return }
    draftConfiguration = displayedRun.configuration
    activeRun = nil
    selectedRunID = nil
    alertMessage = nil
  }

  func selectHistory(_ id: UUID) {
    guard historyRestorationState == .ready else { return }
    selectedRunID = id
  }

  func removeHistory(_ offsets: IndexSet) async {
    guard await waitForHistoryRestoration() else { return }
    guard !isRunTaskActive, !isHistoryMutationActive else { return }
    guard offsets.allSatisfy({ history.indices.contains($0) }) else { return }
    isHistoryMutationActive = true
    defer { isHistoryMutationActive = false }

    let removedIDs = offsets.map { history[$0].id }
    var candidate = history
    for index in offsets.sorted(by: >) {
      candidate.remove(at: index)
    }
    await cancelAndDrainPendingSave()
    do {
      try await store.save(candidate)
      history = candidate
      removedIDs.forEach { runFinalizationStates.removeValue(forKey: $0) }
      if let selectedRunID, removedIDs.contains(selectedRunID) {
        self.selectedRunID = nil
      }
      if let activeRun, removedIDs.contains(activeRun.id), activeRun.status.isTerminal {
        self.activeRun = nil
      }
    } catch {
      alertMessage = "无法删除运行历史：\(error.localizedDescription)"
    }
  }

  func clearHistory() async {
    guard await waitForHistoryRestoration() else { return }
    guard !isRunTaskActive, !isHistoryMutationActive else { return }
    isHistoryMutationActive = true
    defer { isHistoryMutationActive = false }

    await cancelAndDrainPendingSave()
    do {
      try await store.clearHistory()
      history.removeAll()
      activeRun = nil
      selectedRunID = nil
      runFinalizationStates.removeAll()
    } catch {
      alertMessage = "无法清除运行历史：\(error.localizedDescription)"
    }
  }

  func clearDisplayedLogs() async {
    guard await waitForHistoryRestoration() else { return }
    guard !isRunTaskActive, !isHistoryMutationActive else { return }
    guard let displayedRun else { return }
    isHistoryMutationActive = true
    defer { isHistoryMutationActive = false }

    var candidate = history
    if activeRun?.id == displayedRun.id {
      guard var updated = activeRun else { return }
      updated.logs.removeAll()
      if let index = candidate.firstIndex(where: { $0.id == updated.id }) {
        candidate[index] = updated
      }
      await cancelAndDrainPendingSave()
      do {
        try await store.save(candidate)
        activeRun = updated
        history = candidate
      } catch {
        alertMessage = "无法清除运行日志：\(error.localizedDescription)"
      }
    } else {
      guard let index = candidate.firstIndex(where: { $0.id == displayedRun.id }) else { return }
      candidate[index].logs.removeAll()
      await cancelAndDrainPendingSave()
      do {
        try await store.save(candidate)
        history = candidate
      } catch {
        alertMessage = "无法清除运行日志：\(error.localizedDescription)"
      }
    }
  }

  func finalizationMessage(for run: RunRecord) -> String {
    switch runFinalizationStates[run.id] {
    case .pending:
      "正在保存历史并清理本次临时工作目录。"
    case .complete:
      "历史记录已保存，本次临时工作目录已清理。"
    case .failed(let message):
      "历史保存或临时目录清理未完成：\(message)"
    case .none:
      "历史保存和临时目录清理状态尚未确认。"
    }
  }

  func copy(_ text: String) {
    NSPasteboard.general.clearContents()
    NSPasteboard.general.setString(text, forType: .string)
    showToast("已复制到剪贴板")
  }

  private func showToast(_ message: String) {
    toastTask?.cancel()
    toastMessage = message
    toastTask = Task { [weak self] in
      try? await Task.sleep(for: .seconds(1.6))
      guard !Task.isCancelled else { return }
      self?.toastMessage = nil
    }
  }

  private func restoreHistory() async -> Bool {
    do {
      let loaded = try await store.load()
      var seen: Set<UUID> = []
      let restored = loaded.filter { seen.insert($0.id).inserted }
      guard activeRun == nil else { return false }
      history = restored
      selectedRunID = restored.first?.id
      runFinalizationStates = Dictionary(
        uniqueKeysWithValues: restored.map { ($0.id, RunFinalizationState.complete) }
      )
      historyRestorationState = .ready
      return true
    } catch {
      historyRestorationState = .failed
      alertMessage = "无法恢复运行历史：\(error.localizedDescription)"
      return false
    }
  }

  private func waitForHistoryRestoration() async -> Bool {
    switch historyRestorationState {
    case .ready:
      return true
    case .failed:
      return false
    case .loading:
      return await restorationTask?.value ?? false
    }
  }

  private func consume(_ event: CLIEvent) {
    guard activeRun?.status == .running || activeRun?.status == .cancelling else { return }
    switch event.type {
    case .started:
      activeRun?.selectedMethod = event.method
      activeRun?.methodReason = event.methodReason
    case .progress:
      consumeProgress(event)
    case .log:
      let entry = RunLogEntry(
        timestamp: event.timestamp ?? event.elapsed ?? activeRun?.elapsed ?? 0,
        severity: LogSeverity(protocolValue: event.level ?? "INFO"),
        phase: event.phase,
        message: event.message ?? ""
      )
      activeRun?.logs.append(entry)
      if let count = activeRun?.logs.count, count > 600 {
        activeRun?.logs.removeFirst(count - 600)
      }
    case .result:
      guard let result = event.result else {
        markFailed("结果事件缺少结果数据。")
        return
      }
      guard let input = activeRun?.configuration.number,
        DecimalBigUInt.decimalValue(input) == DecimalBigUInt.decimalValue(result.number),
        DecimalBigUInt.decimalValue(result.number) != nil
      else {
        markFailed("结果整数与本次输入不一致；结果未被接受。")
        return
      }
      activeRun?.selectedMethod = result.method
      activeRun?.methodReason = result.methodReason
      activeRun?.result = result
      activeRun?.completedAt = Date()
      activeRun?.elapsed = result.timings.total
      activeRun?.activeAlgorithm = result.statistics.executedAlgorithm
      activeRun?.attemptIndex = result.attempts.map { UInt64($0.count) }
      activeRun?.currentNumber = result.number
      activeRun?.resolvedParameters =
        result.attempts?.last?.statistics.effectiveParameters
        ?? result.statistics.effectiveParameters
      activeRun?.relationsFound = result.statistics.relationsFound
      activeRun?.relationsTarget = 0
      activeRun?.specialQDone = result.statistics.specialQProcessed
      activeRun?.matrixRows = result.statistics.matrixRows
      activeRun?.matrixCols = result.statistics.matrixCols
      if result.success && result.isVerified {
        activeRun?.phase = .done
        activeRun?.phaseProgress = 1
        activeRun?.status = .succeeded
      } else if result.success {
        activeRun?.status = .failed
        if result.factorizationComplete != true || result.factorsPrime != true {
          activeRun?.errorMessage = "结果不是完整且通过素性检查的质因数分解；结果未被接受。"
        } else {
          activeRun?.errorMessage = "因子乘积校验失败；结果未被接受。"
        }
      } else {
        activeRun?.status = .failed
        activeRun?.errorMessage = "运算完成，但未找到非平凡因子。"
      }
    case .error:
      markFailed(event.message ?? "运算引擎报告了未知错误。")
    }
    syncActiveRun()
  }

  private func consumeProgress(_ event: CLIEvent) {
    guard var run = activeRun else { return }
    let newAttempt = event.attemptIndex.map { $0 != run.attemptIndex && $0 > 0 } ?? false
    if newAttempt {
      run.relationsFound = 0
      run.relationsTarget = 0
      run.specialQDone = 0
      run.matrixRows = 0
      run.matrixCols = 0
      run.samples = []
      run.rateBaseline = nil
      run.resolvedParameters = nil
      run.dependenciesTried = nil
      run.dependenciesTotal = nil
    }
    let enteringSieve = event.phase == .sieving && (run.phase != .sieving || newAttempt)
    if let index = event.attemptIndex { run.attemptIndex = index }
    if let number = event.currentNumber, !number.isEmpty { run.currentNumber = number }
    if let algorithm = event.algorithm, !algorithm.isEmpty { run.activeAlgorithm = algorithm }
    if let method = event.method { run.selectedMethod = method }
    if let parameters = event.effectiveParameters { run.resolvedParameters = parameters }
    if event.phase != .done {
      if let phase = event.phase { run.phase = phase }
      run.phaseProgress = event.phaseProgress
    }
    if let elapsed = event.elapsed, elapsed.isFinite { run.elapsed = max(run.elapsed, elapsed) }
    if event.phase == .sieving {
      if let relations = event.relationsFound { run.relationsFound = relations }
      if let target = event.relationsTarget { run.relationsTarget = target }
      if let specialQ = event.specialQDone { run.specialQDone = specialQ }
      if let elapsed = event.elapsed, elapsed.isFinite, let relations = event.relationsFound {
        let previous = run.rateBaseline
        if enteringSieve || previous == nil || relations < (previous?.relations ?? 0) {
          if relations < (previous?.relations ?? 0) { run.samples = [] }
          run.rateBaseline = ProgressSample(elapsed: elapsed, relations: relations, rate: 0)
        } else if let previous, elapsed - previous.elapsed >= 0.15 {
          let instant = Double(relations - previous.relations) / (elapsed - previous.elapsed)
          let smoothed = run.samples.last.map { $0.rate * 0.55 + instant * 0.45 } ?? instant
          let sample = ProgressSample(elapsed: elapsed, relations: relations, rate: smoothed)
          run.samples.append(sample)
          run.rateBaseline = sample
          if run.samples.count > 240 { run.samples.removeFirst(run.samples.count - 240) }
        }
      }
    }
    if event.phase == .squareRoot || event.phase == .extraction {
      if let tried = event.dependencyIndex { run.dependenciesTried = tried }
      if let total = event.dependenciesTotal, total > 0 || event.phase == .squareRoot {
        run.dependenciesTotal = total
      }
    }
    if event.phase == .linearAlgebra {
      if let rows = event.matrixRows { run.matrixRows = rows }
      if let columns = event.matrixCols { run.matrixCols = columns }
    }
    activeRun = run
  }

  private func finishIfNeeded() {
    guard let status = activeRun?.status, !status.isTerminal else {
      syncActiveRun()
      return
    }
    if status == .cancelling {
      markCancelled()
    } else {
      markFailed("运算引擎在返回最终结果前结束。")
    }
  }

  private func markCancelled() {
    let terminalElapsed = activeRun?.elapsed(at: Date()) ?? 0
    activeRun?.elapsed = terminalElapsed
    activeRun?.status = .cancelled
    activeRun?.completedAt = Date()
    activeRun?.errorMessage = "任务已由用户取消。"
    syncActiveRun()
  }

  private func markFailed(_ message: String) {
    let terminalElapsed = activeRun?.elapsed(at: Date()) ?? 0
    activeRun?.elapsed = terminalElapsed
    activeRun?.status = .failed
    activeRun?.completedAt = Date()
    activeRun?.errorMessage = message
    alertMessage = message
    syncActiveRun()
  }

  private func syncActiveRun() {
    guard let activeRun else { return }
    if let index = history.firstIndex(where: { $0.id == activeRun.id }) {
      history[index] = activeRun
    } else {
      history.insert(activeRun, at: 0)
      history = Array(history.prefix(50))
    }
    if activeRun.status.isTerminal {
      persistNow()
    } else {
      persistSoon()
    }
  }

  private func persistSoon() {
    guard historyRestorationState == .ready else { return }
    let pendingSave = saveTask
    pendingSave?.cancel()
    let snapshot = history
    saveTask = Task { [weak self, store] in
      await pendingSave?.value
      try? await Task.sleep(for: .milliseconds(300))
      guard !Task.isCancelled else { return }
      do {
        try await store.save(snapshot)
      } catch {
        self?.alertMessage = "无法保存运行历史：\(error.localizedDescription)"
      }
    }
  }

  private func persistNow() {
    guard historyRestorationState == .ready else { return }
    let pendingSave = saveTask
    pendingSave?.cancel()
    let snapshot = history
    saveTask = Task { [weak self, store] in
      await pendingSave?.value
      guard !Task.isCancelled else { return }
      do {
        try await store.save(snapshot)
      } catch {
        self?.alertMessage = "无法保存运行历史：\(error.localizedDescription)"
      }
    }
  }

  private func finalizeRun(_ runID: UUID) async {
    let pendingSave = saveTask
    pendingSave?.cancel()
    saveTask = nil
    await pendingSave?.value
    do {
      try await store.finalize(runID, runs: history)
      runFinalizationStates[runID] = .complete
    } catch {
      runFinalizationStates[runID] = .failed(error.localizedDescription)
      alertMessage = "无法完成运行清理：\(error.localizedDescription)"
    }
  }

  private func flushHistory() async {
    guard historyRestorationState == .ready else { return }
    let pendingSave = saveTask
    pendingSave?.cancel()
    saveTask = nil
    await pendingSave?.value
    do {
      try await store.save(history)
    } catch {
      alertMessage = "无法保存运行历史：\(error.localizedDescription)"
    }
  }

  private func cancelAndDrainPendingSave() async {
    let pendingSave = saveTask
    pendingSave?.cancel()
    saveTask = nil
    await pendingSave?.value
  }
}
