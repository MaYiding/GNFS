import Foundation
import XCTest

@testable import GNFSWorkbench

final class RunStoreTests: XCTestCase {
  func testPersistsHistoryAndRecoversInterruptedRun() async throws {
    let directory = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: directory) }

    let store = RunStore(baseURL: directory)
    var run = RunRecord(configuration: RunConfiguration())
    run.status = .running
    try await store.save([run])

    let restored = await store.load()
    XCTAssertEqual(restored.count, 1)
    XCTAssertEqual(restored.first?.status, .cancelled)
    XCTAssertNotNil(restored.first?.completedAt)
  }

  func testCreatesPerRunResumeDirectory() async throws {
    let directory = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: directory) }

    let runID = UUID()
    let store = RunStore(baseURL: directory)
    let resume = try await store.resumeDirectory(for: runID)

    XCTAssertTrue(FileManager.default.fileExists(atPath: resume.path))
    XCTAssertTrue(resume.path.hasSuffix(runID.uuidString))
  }
}
