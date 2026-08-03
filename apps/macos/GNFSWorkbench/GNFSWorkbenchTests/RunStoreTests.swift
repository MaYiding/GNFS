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

  func testSaveCapsHistoryAndRemovesEvictedAndUnknownWorkspaces() async throws {
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
    XCTAssertFalse(FileManager.default.fileExists(atPath: unknown.path))
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

    try await store.clearHistory()

    XCTAssertFalse(
      FileManager.default.fileExists(
        atPath: directory.appendingPathComponent("history.json").path
      ))
    XCTAssertFalse(FileManager.default.fileExists(atPath: workspace.directory.path))
    let restored = try await store.load()
    XCTAssertEqual(restored, [])
  }

  private func temporaryDirectory() -> URL {
    FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
  }
}
