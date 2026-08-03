import Foundation
import XCTest

@testable import GNFSWorkbench

final class ProcessGNFSRunnerTests: XCTestCase {
  func testRealCLIStreamsVerifiedResultWithoutTerminalParsing() async throws {
    let executable = try XCTUnwrap(Bundle.main.url(forResource: "gnfs", withExtension: nil))
    XCTAssertTrue(FileManager.default.isExecutableFile(atPath: executable.path))

    let resolver = GNFSExecutableResolver(
      bundle: .main,
      environment: [:],
      workingDirectory: repositoryRoot()
    )
    let runner = ProcessGNFSRunner(resolver: resolver)
    let resumeDirectory = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: resumeDirectory) }

    let stream = try await runner.start(
      configuration: RunConfiguration(number: "96091"),
      resumeDirectory: resumeDirectory
    )
    var eventKinds: [CLIEvent.Kind] = []
    var result: FactorizationResult?
    for try await event in stream {
      eventKinds.append(event.type)
      if event.type == .started {
        XCTAssertEqual(event.completeFactorization, true)
      }
      if event.type == .result { result = event.result }
    }
    await runner.cancel()

    XCTAssertEqual(eventKinds.first, .started)
    XCTAssertTrue(eventKinds.contains(.log))
    XCTAssertEqual(eventKinds.last, .result)
    XCTAssertEqual(result?.factors, ["307", "313"])
    XCTAssertEqual(result?.factorizationComplete, true)
    XCTAssertEqual(result?.factorsPrime, true)
    XCTAssertEqual(result?.isVerified, true)
  }

  func testRealCLIProducesCompletePrimeFactorizationWithMultiplicity() async throws {
    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: [:],
        workingDirectory: repositoryRoot()
      ))
    let resumeDirectory = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: resumeDirectory) }

    let stream = try await runner.start(
      configuration: RunConfiguration(number: "360"),
      resumeDirectory: resumeDirectory
    )
    var result: FactorizationResult?
    for try await event in stream where event.type == .result {
      result = event.result
    }
    await runner.cancel()

    XCTAssertEqual(result?.factors, ["2", "2", "2", "3", "3", "5"])
    XCTAssertEqual(result?.distinctPrimeCount, 3)
    XCTAssertEqual(result?.factorExpression, "2^3 × 3^2 × 5")
    XCTAssertEqual(result?.factorizationComplete, true)
    XCTAssertEqual(result?.factorsPrime, true)
    XCTAssertEqual(result?.isVerified, true)
  }

  func testRealCLIAcceptsPrimeInputAsCompleteFactorization() async throws {
    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: [:],
        workingDirectory: repositoryRoot()
      ))
    let resumeDirectory = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: resumeDirectory) }

    let stream = try await runner.start(
      configuration: RunConfiguration(number: "127"),
      resumeDirectory: resumeDirectory
    )
    var result: FactorizationResult?
    for try await event in stream where event.type == .result {
      result = event.result
    }
    await runner.cancel()

    XCTAssertEqual(result?.factors, ["127"])
    XCTAssertEqual(result?.factorizationComplete, true)
    XCTAssertEqual(result?.factorsPrime, true)
    XCTAssertEqual(result?.isVerified, true)
  }

  func testRealCLIAcceptsValidatedHexadecimalInput() async throws {
    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: [:],
        workingDirectory: repositoryRoot()
      ))
    let resumeDirectory = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: resumeDirectory) }

    let stream = try await runner.start(
      configuration: RunConfiguration(number: "0x1775b"),
      resumeDirectory: resumeDirectory
    )
    var result: FactorizationResult?
    for try await event in stream where event.type == .result {
      result = event.result
    }
    await runner.cancel()

    XCTAssertEqual(result?.number, "96091")
    XCTAssertEqual(result?.factors, ["307", "313"])
    XCTAssertEqual(result?.isVerified, true)
  }

  @MainActor
  func testAppModelCancelsRunningBundledCLIWithoutReportingFailure() async throws {
    let storeDirectory = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: storeDirectory) }

    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: [:],
        workingDirectory: repositoryRoot()
      ))
    let model = AppModel(runner: runner, store: RunStore(baseURL: storeDirectory))
    model.draftConfiguration = RunConfiguration(
      number:
        "1522605027922533360535618378132637429718068114961380688657908494580122963258952897654000350692006139",
      method: .gnfs
    )

    await model.startRun()
    let didStart = await waitUntil { model.activeRun?.selectedMethod == .gnfs }
    XCTAssertTrue(didStart)
    model.cancelRun()

    let didCancel = await waitUntil { model.activeRun?.status == .cancelled }
    XCTAssertTrue(didCancel)
    XCTAssertEqual(model.activeRun?.errorMessage, "任务已由用户取消。")
  }

  private func repositoryRoot() -> URL {
    var directory = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    for _ in 0..<4 { directory.deleteLastPathComponent() }
    return directory
  }

  @MainActor
  private func waitUntil(
    timeout: Duration = .seconds(5),
    condition: @escaping @MainActor () -> Bool
  ) async -> Bool {
    let clock = ContinuousClock()
    let deadline = clock.now.advanced(by: timeout)
    while clock.now < deadline {
      if condition() { return true }
      try? await Task.sleep(for: .milliseconds(20))
    }
    return condition()
  }
}
