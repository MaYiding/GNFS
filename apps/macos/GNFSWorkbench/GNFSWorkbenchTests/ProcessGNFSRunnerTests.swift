import Darwin
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
    let workspace = temporaryWorkspace()
    defer { try? FileManager.default.removeItem(at: workspace.directory) }

    let stream = try await runner.start(
      configuration: RunConfiguration(number: "96091"),
      workspace: workspace
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
    let workspace = temporaryWorkspace()
    defer { try? FileManager.default.removeItem(at: workspace.directory) }

    let stream = try await runner.start(
      configuration: RunConfiguration(number: "360"),
      workspace: workspace
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
    let workspace = temporaryWorkspace()
    defer { try? FileManager.default.removeItem(at: workspace.directory) }

    let stream = try await runner.start(
      configuration: RunConfiguration(number: "127"),
      workspace: workspace
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
    let workspace = temporaryWorkspace()
    defer { try? FileManager.default.removeItem(at: workspace.directory) }

    let stream = try await runner.start(
      configuration: RunConfiguration(number: "0x1775b"),
      workspace: workspace
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

  func testRunnerUsesWorkspaceDirectoryAndStateResumeBase() async throws {
    let root = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: root) }
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    let executable = root.appendingPathComponent("workspace-probe.zsh")
    let script = #"""
      #!/bin/zsh
      print -r -- "$PWD" > cwd.txt
      print -r -- "$GNFS_RESUME" > resume.txt
      print -r -- '{"schema_version":1,"type":"error","message":"probe complete"}'
      """#
    try Data(script.utf8).write(to: executable)
    try FileManager.default.setAttributes(
      [.posixPermissions: 0o755],
      ofItemAtPath: executable.path
    )

    let workspace = RunWorkspace(
      directory: root.appendingPathComponent("Runs/probe", isDirectory: true),
      resumeBase: root.appendingPathComponent("Runs/probe/state")
    )
    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: ["GNFS_CLI_PATH": executable.path],
        workingDirectory: root
      ))
    let stream = try await runner.start(
      configuration: RunConfiguration(number: "360"),
      workspace: workspace
    )
    for try await _ in stream {}
    await runner.cancel()

    let cwd = try String(
      contentsOf: workspace.directory.appendingPathComponent("cwd.txt"),
      encoding: .utf8
    ).trimmingCharacters(in: .whitespacesAndNewlines)
    let resume = try String(
      contentsOf: workspace.directory.appendingPathComponent("resume.txt"),
      encoding: .utf8
    ).trimmingCharacters(in: .whitespacesAndNewlines)
    XCTAssertEqual(
      URL(fileURLWithPath: cwd).standardizedFileURL, workspace.directory.standardizedFileURL)
    XCTAssertEqual(resume, workspace.resumeBase.path)
  }

  func testCancelReturnsAfterProcessAndReadersHaveStopped() async throws {
    let root = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: root) }
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    let executable = root.appendingPathComponent("cancellation-probe.zsh")
    let script = #"""
      #!/bin/zsh
      print -r -- "$$" > pid.txt
      trap 'exit 0' INT TERM
      while true; do
        print -r -- '{"schema_version":1,"type":"log","level":"INFO","message":"draining"}'
        print -r -- 'diagnostic' >&2
        sleep 0.01
      done
      """#
    try Data(script.utf8).write(to: executable)
    try FileManager.default.setAttributes(
      [.posixPermissions: 0o755],
      ofItemAtPath: executable.path
    )

    let workspace = RunWorkspace(
      directory: root.appendingPathComponent("Runs/cancel", isDirectory: true),
      resumeBase: root.appendingPathComponent("Runs/cancel/state")
    )
    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: ["GNFS_CLI_PATH": executable.path],
        workingDirectory: root
      ))
    let stream = try await runner.start(
      configuration: RunConfiguration(number: "360"),
      workspace: workspace
    )
    let consumer = Task {
      for try await _ in stream {}
    }
    let pidURL = workspace.directory.appendingPathComponent("pid.txt")
    let didLaunch = await waitUntilFileExists(pidURL)
    XCTAssertTrue(didLaunch)

    await runner.cancel()
    try await consumer.value

    let pidText = try String(contentsOf: pidURL, encoding: .utf8)
      .trimmingCharacters(in: .whitespacesAndNewlines)
    let pid = try XCTUnwrap(Int32(pidText))
    XCTAssertEqual(Darwin.kill(pid, 0), -1)
    XCTAssertEqual(errno, ESRCH)
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

  private func temporaryWorkspace() -> RunWorkspace {
    let directory = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    return RunWorkspace(
      directory: directory,
      resumeBase: directory.appendingPathComponent("state")
    )
  }

  private func waitUntilFileExists(
    _ url: URL,
    timeout: Duration = .seconds(2)
  ) async -> Bool {
    let clock = ContinuousClock()
    let deadline = clock.now.advanced(by: timeout)
    while clock.now < deadline {
      if FileManager.default.fileExists(atPath: url.path) { return true }
      try? await Task.sleep(for: .milliseconds(10))
    }
    return FileManager.default.fileExists(atPath: url.path)
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
