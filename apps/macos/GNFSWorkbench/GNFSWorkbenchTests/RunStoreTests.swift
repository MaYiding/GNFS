import Foundation
import XCTest

@testable import GNFSWorkbench

final class RunStoreTests: XCTestCase {
  func testPersistsRecoveryAndCleansInterruptedLegacyAndOrphanArtifacts() async throws {
    let directory = temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }

    let store = RunStore(baseURL: directory)
    var run = RunRecord(configuration: RunConfiguration())
    run.status = .running
    try await store.save([run])
    let workspace = try await store.workspace(for: run.id)
    try Data("checkpoint".utf8).write(
      to: URL(fileURLWithPath: workspace.resumeBase.path + ".poly_ckpt")
    )

    let runsDirectory = directory.appendingPathComponent("Runs", isDirectory: true)
    let legacy = runsDirectory.appendingPathComponent(run.id.uuidString + ".sieve_ckpt")
    try Data("legacy".utf8).write(to: legacy)
    let orphan = runsDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
    try FileManager.default.createDirectory(at: orphan, withIntermediateDirectories: true)

    let restored = try await store.load()
    XCTAssertEqual(restored.count, 1)
    XCTAssertEqual(restored.first?.status, .cancelled)
    XCTAssertNotNil(restored.first?.completedAt)
    XCTAssertEqual(restored.first?.errorMessage, "上次退出时任务仍在运行。")
    XCTAssertFalse(FileManager.default.fileExists(atPath: workspace.directory.path))
    XCTAssertFalse(FileManager.default.fileExists(atPath: legacy.path))
    XCTAssertFalse(FileManager.default.fileExists(atPath: orphan.path))

    let persisted = try await store.load()
    XCTAssertEqual(persisted.first?.status, .cancelled)
    XCTAssertEqual(persisted.first?.errorMessage, "上次退出时任务仍在运行。")
  }

  func testCreatesPerRunWorkspaceWithStateResumeBase() async throws {
    let directory = temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }

    let runID = UUID()
    let store = RunStore(baseURL: directory)
    let workspace = try await store.workspace(for: runID)

    XCTAssertEqual(
      workspace.directory,
      directory
        .appendingPathComponent("Runs", isDirectory: true)
        .appendingPathComponent(runID.uuidString, isDirectory: true)
    )
    XCTAssertEqual(
      workspace.resumeBase,
      workspace.directory.appendingPathComponent("state", isDirectory: false)
    )
    XCTAssertTrue(FileManager.default.fileExists(atPath: workspace.directory.path))
    XCTAssertFalse(FileManager.default.fileExists(atPath: workspace.resumeBase.path))
  }

  func testFinalizePersistsTerminalRunAndRemovesItsEntireWorkspace() async throws {
    let directory = temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }

    let store = RunStore(baseURL: directory)
    var run = RunRecord(configuration: RunConfiguration())
    run.status = .succeeded
    let workspace = try await store.workspace(for: run.id)
    try Data("checkpoint".utf8).write(
      to: URL(fileURLWithPath: workspace.resumeBase.path + ".fb_ckpt")
    )
    let legacy = workspace.directory.deletingLastPathComponent()
      .appendingPathComponent(run.id.uuidString + ".poly_ckpt")
    try Data("legacy".utf8).write(to: legacy)

    try await store.finalize(run.id, runs: [run])

    XCTAssertFalse(FileManager.default.fileExists(atPath: workspace.directory.path))
    XCTAssertFalse(FileManager.default.fileExists(atPath: legacy.path))
    let restored = try await store.load()
    XCTAssertEqual(restored.map(\.id), [run.id])
    XCTAssertEqual(restored.first?.status, .succeeded)
  }

  func testSaveCapsHistoryAndRemovesEvictedWorkspacesButPreservesUnknownEntries() async throws {
    let directory = temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }

    let store = RunStore(baseURL: directory)
    let runs = (0..<51).map { index in
      RunRecord(configuration: RunConfiguration(number: String(index + 2)))
    }
    for run in runs {
      _ = try await store.workspace(for: run.id)
    }
    let unknown = directory.appendingPathComponent("Runs/unknown", isDirectory: true)
    try FileManager.default.createDirectory(at: unknown, withIntermediateDirectories: true)

    try await store.save(runs)

    XCTAssertTrue(
      FileManager.default.fileExists(
        atPath: directory.appendingPathComponent("Runs/\(runs[49].id.uuidString)").path
      ))
    XCTAssertFalse(
      FileManager.default.fileExists(
        atPath: directory.appendingPathComponent("Runs/\(runs[50].id.uuidString)").path
      ))
    XCTAssertTrue(FileManager.default.fileExists(atPath: unknown.path))
    let restored = try await store.load()
    XCTAssertEqual(restored.count, 50)
    XCTAssertEqual(restored.last?.id, runs[49].id)
  }

  func testClearHistoryRemovesHistoryAndAllManagedArtifacts() async throws {
    let directory = temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }

    let store = RunStore(baseURL: directory)
    let run = RunRecord(configuration: RunConfiguration())
    try await store.save([run])
    let workspace = try await store.workspace(for: run.id)
    let unknown = directory.appendingPathComponent("Runs/operator-note.txt")
    try Data("preserve".utf8).write(to: unknown)

    try await store.clearHistory()

    XCTAssertFalse(
      FileManager.default.fileExists(
        atPath: directory.appendingPathComponent("history.json").path
      ))
    XCTAssertFalse(FileManager.default.fileExists(atPath: workspace.directory.path))
    XCTAssertTrue(FileManager.default.fileExists(atPath: unknown.path))
    let restored = try await store.load()
    XCTAssertEqual(restored, [])
  }

  func testMalformedOrFutureHistoryIsAtomicallyQuarantined() async throws {
    let directory = temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let history = directory.appendingPathComponent("history.json")
    let original = Data(#"{"schema_version":2,"runs":[]}"#.utf8)
    try original.write(to: history)

    let store = RunStore(baseURL: directory)
    do {
      _ = try await store.load()
      XCTFail("malformed history should be rejected")
    } catch let error as RunStoreError {
      guard case .historyQuarantined(let filename) = error else {
        return XCTFail("unexpected RunStoreError: \(error)")
      }
      XCTAssertTrue(filename.hasPrefix("history.invalid-"))
      let quarantine = directory.appendingPathComponent(filename)
      XCTAssertEqual(try Data(contentsOf: quarantine), original)
    }

    XCTAssertFalse(FileManager.default.fileExists(atPath: history.path))
    let quarantines = try FileManager.default.contentsOfDirectory(atPath: directory.path)
      .filter { $0.hasPrefix("history.invalid-") }
    XCTAssertEqual(quarantines.count, 1)
  }

  func testRunsRootSymlinkFailsClosedWithoutTouchingExternalSentinel() async throws {
    let directory = temporaryDirectory()
    let external = temporaryDirectory()
    defer {
      try? FileManager.default.removeItem(at: directory)
      try? FileManager.default.removeItem(at: external)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try FileManager.default.createDirectory(at: external, withIntermediateDirectories: true)
    let sentinel = external.appendingPathComponent("sentinel.txt")
    try Data("outside".utf8).write(to: sentinel)

    let initialStore = RunStore(baseURL: directory)
    let run = RunRecord(configuration: RunConfiguration())
    try await initialStore.save([run])
    let history = directory.appendingPathComponent("history.json")
    let originalHistory = try Data(contentsOf: history)
    try FileManager.default.createSymbolicLink(
      at: directory.appendingPathComponent("Runs"),
      withDestinationURL: external
    )

    let store = RunStore(baseURL: directory)
    await expectUnsafe { _ = try await store.load() }
    let replacement = RunRecord(configuration: RunConfiguration(number: "143"))
    await expectUnsafe { try await store.save([replacement]) }
    await expectUnsafe { _ = try await store.workspace(for: run.id) }
    await expectUnsafe { try await store.finalize(run.id, runs: [run]) }
    await expectUnsafe { try await store.clearHistory() }
    XCTAssertEqual(try Data(contentsOf: history), originalHistory)
    XCTAssertEqual(try String(contentsOf: sentinel, encoding: .utf8), "outside")
  }

  func testPerRunSymlinkFailsClosedWithoutTouchingExternalSentinel() async throws {
    let directory = temporaryDirectory()
    let external = temporaryDirectory()
    defer {
      try? FileManager.default.removeItem(at: directory)
      try? FileManager.default.removeItem(at: external)
    }
    try FileManager.default.createDirectory(
      at: directory.appendingPathComponent("Runs"),
      withIntermediateDirectories: true
    )
    try FileManager.default.createDirectory(at: external, withIntermediateDirectories: true)
    let sentinel = external.appendingPathComponent("sentinel.txt")
    try Data("outside".utf8).write(to: sentinel)
    let run = RunRecord(configuration: RunConfiguration())
    let linkedWorkspace = directory.appendingPathComponent("Runs/\(run.id.uuidString)")
    try FileManager.default.createSymbolicLink(
      at: linkedWorkspace,
      withDestinationURL: external
    )

    let store = RunStore(baseURL: directory)
    await expectUnsafe { _ = try await store.workspace(for: run.id) }
    await expectUnsafe { try await store.save([run]) }
    XCTAssertFalse(
      FileManager.default.fileExists(
        atPath: directory.appendingPathComponent("history.json").path
      )
    )
    await expectUnsafe { try await store.finalize(run.id, runs: [run]) }
    await expectUnsafe { _ = try await store.load() }
    await expectUnsafe { try await store.clearHistory() }
    XCTAssertEqual(try String(contentsOf: sentinel, encoding: .utf8), "outside")
  }

  private func expectUnsafe(_ operation: () async throws -> Void) async {
    do {
      try await operation()
      XCTFail("unsafe managed path should be rejected")
    } catch let error as RunStoreError {
      guard case .unsafeManagedPath = error else {
        return XCTFail("unexpected RunStoreError: \(error)")
      }
    } catch {
      XCTFail("unexpected error: \(error)")
    }
  }

  private func temporaryDirectory() -> URL {
    FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
  }
}
