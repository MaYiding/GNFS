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

  private func makeModel(runner: any GNFSRunning) -> AppModel {
    let storeURL = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    return AppModel(runner: runner, store: RunStore(baseURL: storeURL))
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

  private func decodeEvent(_ json: String) throws -> CLIEvent {
    try JSONDecoder().decode(CLIEvent.self, from: Data(json.utf8))
  }

  private static let resultEvent =
    #"{"schema_version":1,"type":"result","result":{"success":true,"factorization_complete":true,"factors_prime":true,"n":"1000036000099","n_bits":40,"n_digits":13,"method":"rho","method_name":"Pollard Rho","method_reason":"GMP rho fallback","factors":["1000003","1000033"],"timings":{"total_s":2.1,"poly_s":0,"fb_s":0,"sieve_s":2,"filter_s":0,"linalg_s":0,"sqrt_s":0,"extract_s":0.1},"stats":{"degree":3,"rational_bound":5000,"algebraic_bound":10000,"large_prime_bound":131072,"rational_primes":0,"algebraic_primes":0,"special_q_processed":90,"candidates_total":0,"relations_found":500,"full_relations":0,"partial_1lp":0,"partial_2lp":0,"relations_after_filter":0,"singletons_removed":0,"merged_relations":0,"matrix_rows":0,"matrix_cols":0,"matrix_excess":0,"dependencies_found":0,"dependencies_tried":0}}}"#
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
