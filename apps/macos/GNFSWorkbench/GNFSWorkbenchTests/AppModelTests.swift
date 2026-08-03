import Foundation
import XCTest

@testable import GNFSWorkbench

@MainActor
final class AppModelTests: XCTestCase {
  func testConsumesScriptedRunAndAcceptsVerifiedResult() async throws {
    let runner = ScriptedRunner(
      events: try [
        decodeEvent(
          #"{"schema_version":1,"type":"started","n":"1000036000099","n_bits":40,"n_digits":13,"method":"rho","method_name":"Pollard Rho","method_reason":"automatic"}"#
        ),
        decodeEvent(
          #"{"schema_version":1,"type":"progress","phase":"sieve","phase_progress":0.5,"elapsed_s":2,"message":"sieving","relations_found":500,"relations_target":1000,"special_q_done":90,"matrix_rows":0,"matrix_cols":0,"dependency_index":-1,"dependencies_total":0}"#
        ),
        decodeEvent(
          #"{"schema_version":1,"type":"log","level":"INFO","phase":"sieve","timestamp_s":2,"message":"halfway"}"#
        ),
        decodeEvent(Self.resultEvent),
      ])
    let model = makeModel(runner: runner)

    await model.startRun()
    let didSucceed = await waitUntil { model.activeRun?.status == .succeeded }
    XCTAssertTrue(didSucceed)

    XCTAssertEqual(model.activeRun?.selectedMethod, .rho)
    XCTAssertEqual(model.activeRun?.relationsFound, 500)
    XCTAssertEqual(model.activeRun?.logs.last?.message, "halfway")
    XCTAssertEqual(model.activeRun?.result?.factors, ["1000003", "1000033"])
    XCTAssertEqual(model.history.first?.status, .succeeded)
  }

  func testResultRestoresMethodMetadataWhenStartedEventWasDropped() async throws {
    let model = makeModel(
      runner: ScriptedRunner(events: [try decodeEvent(Self.resultEvent)])
    )

    await model.startRun()
    let didSucceed = await waitUntil { model.activeRun?.status == .succeeded }

    XCTAssertTrue(didSucceed)
    XCTAssertEqual(model.activeRun?.selectedMethod, .rho)
    XCTAssertEqual(model.activeRun?.methodReason, "GMP rho fallback")
  }

  func testFinalizationStateRetentionMatchesFiftyRunHistoryLimit() async throws {
    let model = makeModel(
      runner: ScriptedRunner(events: [try decodeEvent(Self.resultEvent)])
    )

    for index in 0..<51 {
      model.draftConfiguration.number = String(index + 2)
      await model.startRun()
      let didFinalize = await waitUntil { !model.isRunTaskActive }
      XCTAssertTrue(didFinalize)
      if index < 50 { await model.newRun() }
    }

    XCTAssertEqual(model.history.count, 50)
    XCTAssertEqual(model.runFinalizationStates.count, 50)
    XCTAssertEqual(Set(model.runFinalizationStates.keys), Set(model.history.map(\.id)))
  }

  func testTerminalRunPersistsBeforeWorkspaceCleanup() async throws {
    let storeURL = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: storeURL) }
    let store = RunStore(baseURL: storeURL)
    let model = AppModel(
      runner: ScriptedRunner(events: [try decodeEvent(Self.resultEvent)]),
      store: store
    )

    await model.startRun()
    let didFinalize = await waitUntil {
      model.activeRun?.status == .succeeded && !model.isRunTaskActive
    }
    XCTAssertTrue(didFinalize)
    let runID = try XCTUnwrap(model.activeRun?.id)
    XCTAssertFalse(
      FileManager.default.fileExists(
        atPath: storeURL.appendingPathComponent("Runs/\(runID.uuidString)").path
      ))
    let restored = try await store.load()
    XCTAssertEqual(restored.first?.id, runID)
    XCTAssertEqual(restored.first?.status, .succeeded)
  }

  func testCancellationTransitionsThroughRunnerToTerminalState() async throws {
    let runner = HangingRunner(
      startedEvent: try decodeEvent(
        #"{"schema_version":1,"type":"started","n":"1000036000099","n_bits":40,"n_digits":13,"method":"gnfs","method_name":"GNFS","method_reason":"forced"}"#
      ))
    let model = makeModel(runner: runner)

    await model.startRun()
    let didStart = await waitUntil { model.activeRun?.selectedMethod == .gnfs }
    XCTAssertTrue(didStart)
    model.cancelRun()
    let didCancel = await waitUntil { model.activeRun?.status == .cancelled }
    XCTAssertTrue(didCancel)
    XCTAssertEqual(model.activeRun?.errorMessage, "任务已由用户取消。")
    let runnerCancelled = await runner.didCancel()
    XCTAssertTrue(runnerCancelled)
  }

  func testClearLogsIsRejectedWhileRunCanStillEmitEvents() async throws {
    let runner = HangingRunner(
      startedEvent: try decodeEvent(
        #"{"schema_version":1,"type":"started","n":"1000036000099","n_bits":40,"n_digits":13,"method":"gnfs","method_name":"GNFS","method_reason":"forced"}"#
      ))
    let model = makeModel(runner: runner)

    await model.startRun()
    let didStart = await waitUntil { model.activeRun?.selectedMethod == .gnfs }
    XCTAssertTrue(didStart)
    model.activeRun?.logs.append(
      RunLogEntry(timestamp: 0, severity: .info, phase: .sieving, message: "still running")
    )

    await model.clearDisplayedLogs()

    XCTAssertEqual(model.activeRun?.logs.map(\.message), ["still running"])
    model.cancelRun()
    let didCancel = await waitUntil { model.activeRun?.status == .cancelled }
    XCTAssertTrue(didCancel)
  }

  func testRejectsInvalidInputBeforeStartingRunner() async {
    let runner = ScriptedRunner(events: [])
    let model = makeModel(runner: runner)
    model.draftConfiguration.number = "1"

    await model.startRun()

    XCTAssertNil(model.activeRun)
    XCTAssertEqual(model.alertMessage, "整数 N 必须大于 1。")
    let startCount = await runner.startCount()
    XCTAssertEqual(startCount, 0)
  }

  func testRejectsProductCorrectButIncompleteSplit() async throws {
    let incomplete = try decodeEvent(
      #"{"schema_version":1,"type":"result","result":{"success":true,"factorization_complete":false,"factors_prime":false,"n":"60","n_bits":6,"n_digits":2,"method":"trial","method_name":"Trial Division","method_reason":"single split","factors":["6","10"],"timings":{"total_s":0.01,"poly_s":0,"fb_s":0,"sieve_s":0,"filter_s":0,"linalg_s":0,"sqrt_s":0,"extract_s":0},"stats":{"degree":0,"rational_bound":0,"algebraic_bound":0,"large_prime_bound":0,"rational_primes":0,"algebraic_primes":0,"special_q_processed":0,"candidates_total":0,"relations_found":0,"full_relations":0,"partial_1lp":0,"partial_2lp":0,"relations_after_filter":0,"singletons_removed":0,"merged_relations":0,"matrix_rows":0,"matrix_cols":0,"matrix_excess":0,"dependencies_found":0,"dependencies_tried":0}}}"#
    )
    let model = makeModel(runner: ScriptedRunner(events: [incomplete]))
    model.draftConfiguration.number = "60"

    await model.startRun()
    let didFail = await waitUntil { model.activeRun?.status == .failed }

    XCTAssertTrue(didFail)
    XCTAssertEqual(
      model.activeRun?.errorMessage,
      "结果不是完整且通过素性检查的质因数分解；结果未被接受。"
    )
  }

  func testAcceptsPrimeInputAsOneFactorCompleteResult() async throws {
    let prime = try decodeEvent(
      #"{"schema_version":1,"type":"result","result":{"success":true,"factorization_complete":true,"factors_prime":true,"n":"127","n_bits":7,"n_digits":3,"method":"auto","method_name":"Auto","method_reason":"input is prime","factors":["127"],"timings":{"total_s":0.001,"poly_s":0,"fb_s":0,"sieve_s":0,"filter_s":0,"linalg_s":0,"sqrt_s":0,"extract_s":0},"stats":{"degree":0,"rational_bound":0,"algebraic_bound":0,"large_prime_bound":0,"rational_primes":0,"algebraic_primes":0,"special_q_processed":0,"candidates_total":0,"relations_found":0,"full_relations":0,"partial_1lp":0,"partial_2lp":0,"relations_after_filter":0,"singletons_removed":0,"merged_relations":0,"matrix_rows":0,"matrix_cols":0,"matrix_excess":0,"dependencies_found":0,"dependencies_tried":0}}}"#
    )
    let model = makeModel(runner: ScriptedRunner(events: [prime]))
    model.draftConfiguration.number = "127"

    await model.startRun()
    let didSucceed = await waitUntil { model.activeRun?.status == .succeeded }

    XCTAssertTrue(didSucceed)
    XCTAssertEqual(model.activeRun?.result?.factors, ["127"])
    XCTAssertEqual(model.activeRun?.result?.isVerified, true)
  }

  func testTerminationFlushesPendingHistoryWhileIdle() async throws {
    let storeURL = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: storeURL) }

    let store = RunStore(baseURL: storeURL)
    let first = RunRecord(configuration: RunConfiguration(number: "96091"))
    try await store.save([first])
    let model = AppModel(runner: ScriptedRunner(events: []), store: store)
    let didRestore = await waitUntil { model.history.count == 1 }
    XCTAssertTrue(didRestore)

    let second = RunRecord(configuration: RunConfiguration(number: "143"))
    model.history.insert(second, at: 0)
    await model.prepareForTermination()

    let restored = try await store.load()
    XCTAssertEqual(restored.map(\.id), [second.id, first.id])
  }

  func testImmediateStartWaitsForRestoreAndPreservesExistingHistory() async throws {
    let existing = RunRecord(configuration: RunConfiguration(number: "96091"))
    let store = DelayedRunStore(initialRuns: [existing])
    let runner = ScriptedRunner(events: [try decodeEvent(Self.resultEvent)])
    let model = AppModel(runner: runner, store: store)

    let startTask = Task { await model.startRun() }
    let didBeginLoading = await waitUntilAsync { await store.didBeginLoading() }
    XCTAssertTrue(didBeginLoading)
    XCTAssertEqual(model.historyRestorationState, .loading)
    XCTAssertNil(model.activeRun)
    let startsBeforeRestore = await runner.startCount()
    XCTAssertEqual(startsBeforeRestore, 0)

    await store.releaseLoad()
    await startTask.value
    let didFinalize = await waitUntil { !model.isRunTaskActive && model.history.count == 2 }
    XCTAssertTrue(didFinalize)
    XCTAssertTrue(model.history.contains(where: { $0.id == existing.id }))
    let persisted = await store.latestRuns()
    XCTAssertEqual(Set(persisted.map(\.id)), Set(model.history.map(\.id)))
  }

  func testClearHistoryWaitsForRestoreBeforeMutatingOrDeleting() async {
    let existing = RunRecord(configuration: RunConfiguration(number: "143"))
    let store = DelayedRunStore(initialRuns: [existing])
    let model = AppModel(runner: ScriptedRunner(events: []), store: store)

    let clearTask = Task { await model.clearHistory() }
    let didBeginLoading = await waitUntilAsync { await store.didBeginLoading() }
    XCTAssertTrue(didBeginLoading)
    let clearCountBeforeRestore = await store.clearCount()
    XCTAssertEqual(clearCountBeforeRestore, 0)
    await store.releaseLoad()
    await clearTask.value

    XCTAssertEqual(model.historyRestorationState, .ready)
    XCTAssertTrue(model.history.isEmpty)
    let clearCountAfterRestore = await store.clearCount()
    XCTAssertEqual(clearCountAfterRestore, 1)
  }

  func testMalformedHistoryBlocksStartAndPreservesQuarantine() async throws {
    let storeURL = temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: storeURL) }
    try FileManager.default.createDirectory(at: storeURL, withIntermediateDirectories: true)
    let historyURL = storeURL.appendingPathComponent("history.json")
    let malformed = Data(#"{"schema_version":999,"runs":[]}"#.utf8)
    try malformed.write(to: historyURL)
    let runner = ScriptedRunner(events: [try decodeEvent(Self.resultEvent)])
    let model = AppModel(runner: runner, store: RunStore(baseURL: storeURL))

    let didFailRestore = await waitUntil { model.historyRestorationState == .failed }
    XCTAssertTrue(didFailRestore)
    await model.startRun()

    XCTAssertNil(model.activeRun)
    let startCount = await runner.startCount()
    XCTAssertEqual(startCount, 0)
    XCTAssertFalse(FileManager.default.fileExists(atPath: historyURL.path))
    let quarantines = try FileManager.default.contentsOfDirectory(at: storeURL, includingPropertiesForKeys: nil)
      .filter { $0.lastPathComponent.hasPrefix("history.invalid-") }
    XCTAssertEqual(quarantines.count, 1)
    XCTAssertEqual(try Data(contentsOf: XCTUnwrap(quarantines.first)), malformed)
  }

  func testFinalizationFailureIsExposedWithoutSuccessClaim() async throws {
    let store = FinalizationFailingRunStore()
    let model = AppModel(
      runner: ScriptedRunner(events: [try decodeEvent(Self.resultEvent)]),
      store: store
    )

    await model.startRun()
    let didFinish = await waitUntil { !model.isRunTaskActive }
    XCTAssertTrue(didFinish)
    let run = try XCTUnwrap(model.activeRun)
    guard case .failed(let message) = model.runFinalizationStates[run.id] else {
      return XCTFail("finalization failure state was not retained")
    }
    XCTAssertEqual(message, "injected finalization failure")
    XCTAssertTrue(model.finalizationMessage(for: run).contains("未完成"))
    XCTAssertFalse(model.finalizationMessage(for: run).contains("已清理"))
  }

  private func makeModel(runner: any GNFSRunning) -> AppModel {
    let storeURL = temporaryDirectory()
    addTeardownBlock {
      try? FileManager.default.removeItem(at: storeURL)
    }
    return AppModel(runner: runner, store: RunStore(baseURL: storeURL))
  }

  private func temporaryDirectory() -> URL {
    FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
  }

  private func waitUntil(
    timeout: Duration = .seconds(2),
    condition: @escaping @MainActor () -> Bool
  ) async -> Bool {
    let clock = ContinuousClock()
    let deadline = clock.now.advanced(by: timeout)
    while clock.now < deadline {
      if condition() { return true }
      try? await Task.sleep(for: .milliseconds(15))
    }
    return condition()
  }

  private func waitUntilAsync(
    timeout: Duration = .seconds(2),
    condition: @escaping () async -> Bool
  ) async -> Bool {
    let clock = ContinuousClock()
    let deadline = clock.now.advanced(by: timeout)
    while clock.now < deadline {
      if await condition() { return true }
      try? await Task.sleep(for: .milliseconds(15))
    }
    return await condition()
  }

  private func decodeEvent(_ json: String) throws -> CLIEvent {
    try JSONDecoder().decode(CLIEvent.self, from: Data(json.utf8))
  }

  private static let resultEvent =
    #"{"schema_version":1,"type":"result","result":{"success":true,"factorization_complete":true,"factors_prime":true,"n":"1000036000099","n_bits":40,"n_digits":13,"method":"rho","method_name":"Pollard Rho","method_reason":"GMP rho fallback","factors":["1000003","1000033"],"timings":{"total_s":2.1,"poly_s":0,"fb_s":0,"sieve_s":2,"filter_s":0,"linalg_s":0,"sqrt_s":0,"extract_s":0.1},"stats":{"degree":3,"rational_bound":5000,"algebraic_bound":10000,"large_prime_bound":131072,"rational_primes":0,"algebraic_primes":0,"special_q_processed":90,"candidates_total":0,"relations_found":500,"full_relations":0,"partial_1lp":0,"partial_2lp":0,"relations_after_filter":0,"singletons_removed":0,"merged_relations":0,"matrix_rows":0,"matrix_cols":0,"matrix_excess":0,"dependencies_found":0,"dependencies_tried":0}}}"#
}

private actor DelayedRunStore: RunStoring {
  private let initialRuns: [RunRecord]
  private let workspaceRoot = FileManager.default.temporaryDirectory
    .appendingPathComponent(UUID().uuidString, isDirectory: true)
  private var loadContinuation: CheckedContinuation<Void, Never>?
  private var loadStarted = false
  private var savedRuns: [RunRecord] = []
  private var clears = 0

  init(initialRuns: [RunRecord]) {
    self.initialRuns = initialRuns
  }

  func load() async throws -> [RunRecord] {
    loadStarted = true
    await withCheckedContinuation { continuation in
      loadContinuation = continuation
    }
    return initialRuns
  }

  func save(_ runs: [RunRecord]) async throws {
    savedRuns = runs
  }

  func workspace(for runID: UUID) async throws -> RunWorkspace {
    let directory = workspaceRoot.appendingPathComponent(runID.uuidString, isDirectory: true)
    return RunWorkspace(directory: directory, resumeBase: directory.appendingPathComponent("state"))
  }

  func finalize(_ runID: UUID, runs: [RunRecord]) async throws {
    savedRuns = runs
  }

  func clearHistory() async throws {
    clears += 1
    savedRuns = []
  }

  func didBeginLoading() -> Bool { loadStarted }
  func latestRuns() -> [RunRecord] { savedRuns }
  func clearCount() -> Int { clears }

  func releaseLoad() {
    loadContinuation?.resume()
    loadContinuation = nil
  }
}

private actor FinalizationFailingRunStore: RunStoring {
  private struct InjectedError: LocalizedError {
    var errorDescription: String? { "injected finalization failure" }
  }

  func load() async throws -> [RunRecord] { [] }
  func save(_ runs: [RunRecord]) async throws {}

  func workspace(for runID: UUID) async throws -> RunWorkspace {
    let directory = FileManager.default.temporaryDirectory
      .appendingPathComponent(runID.uuidString, isDirectory: true)
    return RunWorkspace(directory: directory, resumeBase: directory.appendingPathComponent("state"))
  }

  func finalize(_ runID: UUID, runs: [RunRecord]) async throws {
    throw InjectedError()
  }

  func clearHistory() async throws {}
}

private actor ScriptedRunner: GNFSRunning {
  private let events: [CLIEvent]
  private var starts = 0

  init(events: [CLIEvent]) {
    self.events = events
  }

  func start(
    configuration: RunConfiguration,
    workspace: RunWorkspace
  ) async throws -> AsyncThrowingStream<CLIEvent, Error> {
    starts += 1
    return AsyncThrowingStream { continuation in
      for event in events { continuation.yield(event) }
      continuation.finish()
    }
  }

  func cancel() async {}
  func startCount() -> Int { starts }
}

private actor HangingRunner: GNFSRunning {
  private let startedEvent: CLIEvent
  private var continuation: AsyncThrowingStream<CLIEvent, Error>.Continuation?
  private var cancelled = false

  init(startedEvent: CLIEvent) {
    self.startedEvent = startedEvent
  }

  func start(
    configuration: RunConfiguration,
    workspace: RunWorkspace
  ) async throws -> AsyncThrowingStream<CLIEvent, Error> {
    AsyncThrowingStream { continuation in
      self.continuation = continuation
      continuation.yield(startedEvent)
    }
  }

  func cancel() async {
    cancelled = true
    continuation?.finish()
    continuation = nil
  }

  func didCancel() -> Bool { cancelled }
}
