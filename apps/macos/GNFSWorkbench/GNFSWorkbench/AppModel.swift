import AppKit
import Foundation
import Observation

@MainActor
@Observable
final class AppModel {
  var draftConfiguration = RunConfiguration()
  var activeRun: RunRecord?
  var history: [RunRecord] = []
  var selectedRunID: UUID?
  var isParametersPresented = false
  var isHistoryPresented = false
  var areLogsExpanded = true
  var alertMessage: String?
  var toastMessage: String?
  private(set) var isRunTaskActive = false

  @ObservationIgnored private let runner: any GNFSRunning
  @ObservationIgnored private let store: RunStore
  @ObservationIgnored private var runTask: Task<Void, Never>?
  @ObservationIgnored private var saveTask: Task<Void, Never>?
  @ObservationIgnored private var toastTask: Task<Void, Never>?

  init(
    runner: (any GNFSRunning)? = nil,
    store: RunStore? = nil
  ) {
    let isDemo =
      ProcessInfo.processInfo.arguments.contains("--ui-testing")
      || ProcessInfo.processInfo.arguments.contains("--design-preview")
    self.runner = runner ?? (isDemo ? DemoGNFSRunner() : ProcessGNFSRunner())

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

    Task { [weak self] in
      await self?.restoreHistory()
      if ProcessInfo.processInfo.arguments.contains("--design-preview") {
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
    inputValidationMessage == nil && !isRunTaskActive
  }

  func startRun() async {
    guard !isRunTaskActive else { return }
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
    await flushHistory()
  }

  func newRun() {
    guard !isRunTaskActive else { return }
    if let displayedRun {
      draftConfiguration = displayedRun.configuration
    }
    activeRun = nil
    selectedRunID = nil
    alertMessage = nil
  }

  func useConfigurationFromDisplayedRun() {
    guard !isRunTaskActive, let displayedRun else { return }
    draftConfiguration = displayedRun.configuration
    activeRun = nil
    selectedRunID = nil
    alertMessage = nil
  }

  func selectHistory(_ id: UUID) {
    selectedRunID = id
    isHistoryPresented = false
  }

  func removeHistory(_ offsets: IndexSet) {
    guard !isRunTaskActive else { return }
    let removedIDs = offsets.map { history[$0].id }
    for index in offsets.sorted(by: >) {
      history.remove(at: index)
    }
    if let selectedRunID, removedIDs.contains(selectedRunID) {
      self.selectedRunID = nil
    }
    if let activeRun, removedIDs.contains(activeRun.id), activeRun.status.isTerminal {
      self.activeRun = nil
    }
    persistNow()
  }

  func clearHistory() {
    guard !isRunTaskActive else { return }
    history.removeAll()
    activeRun = nil
    selectedRunID = nil
    let pendingSave = saveTask
    pendingSave?.cancel()
    saveTask = Task { [weak self, store] in
      await pendingSave?.value
      do {
        try await store.clearHistory()
      } catch {
        self?.alertMessage = error.localizedDescription
      }
    }
  }

  func clearDisplayedLogs() {
    guard let displayedRun else { return }
    if activeRun?.id == displayedRun.id {
      activeRun?.logs.removeAll()
      syncActiveRun()
      return
    }
    guard let index = history.firstIndex(where: { $0.id == displayedRun.id }) else { return }
    history[index].logs.removeAll()
    persistSoon()
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

  private func restoreHistory() async {
    do {
      let restored = try await store.load()
      guard activeRun == nil else { return }
      history = restored
      selectedRunID = restored.first?.id
    } catch {
      alertMessage = "无法恢复运行历史：\(error.localizedDescription)"
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
      activeRun?.result = result
      activeRun?.completedAt = Date()
      activeRun?.phase = .done
      activeRun?.phaseProgress = 1
      activeRun?.elapsed = result.timings.total
      if result.success && result.isVerified {
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
    if let phase = event.phase { activeRun?.phase = phase }
    activeRun?.phaseProgress = event.phaseProgress
    if let elapsed = event.elapsed { activeRun?.elapsed = elapsed }
    if let relations = event.relationsFound { activeRun?.relationsFound = relations }
    if let target = event.relationsTarget { activeRun?.relationsTarget = target }
    if let specialQ = event.specialQDone { activeRun?.specialQDone = specialQ }
    if let rows = event.matrixRows { activeRun?.matrixRows = rows }
    if let columns = event.matrixCols { activeRun?.matrixCols = columns }

    guard let elapsed = event.elapsed,
      let relations = event.relationsFound,
      relations > 0
    else { return }

    let previous = activeRun?.samples.last
    guard previous == nil || elapsed - (previous?.elapsed ?? 0) >= 0.15 else { return }
    let deltaTime = max(elapsed - (previous?.elapsed ?? 0), 0.001)
    let deltaRelations =
      relations >= (previous?.relations ?? 0)
      ? relations - (previous?.relations ?? 0)
      : 0
    let instantRate = Double(deltaRelations) / deltaTime
    let smoothedRate = previous.map { $0.rate * 0.55 + instantRate * 0.45 } ?? instantRate
    activeRun?.samples.append(
      ProgressSample(
        elapsed: elapsed,
        relations: relations,
        rate: smoothedRate
      ))
    if let count = activeRun?.samples.count, count > 240 {
      activeRun?.samples.removeFirst(count - 240)
    }
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
    activeRun?.status = .cancelled
    activeRun?.completedAt = Date()
    activeRun?.errorMessage = "任务已由用户取消。"
    syncActiveRun()
  }

  private func markFailed(_ message: String) {
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
    } catch {
      alertMessage = "无法完成运行清理：\(error.localizedDescription)"
    }
  }

  private func flushHistory() async {
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
}
