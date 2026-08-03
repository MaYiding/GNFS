import Foundation

struct RunWorkspace: Equatable, Sendable {
  let directory: URL
  let resumeBase: URL
}

actor RunStore {
  private let baseURL: URL
  private let historyURL: URL
  private let runsURL: URL
  private let encoder: JSONEncoder
  private let decoder: JSONDecoder

  init(baseURL: URL? = nil) {
    let root = baseURL ?? Self.defaultBaseURL()
    self.baseURL = root
    self.historyURL = root.appendingPathComponent("history.json")
    self.runsURL = root.appendingPathComponent("Runs", isDirectory: true)

    let encoder = JSONEncoder()
    encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
    encoder.dateEncodingStrategy = .iso8601
    self.encoder = encoder

    let decoder = JSONDecoder()
    decoder.dateDecodingStrategy = .iso8601
    self.decoder = decoder
  }

  func load() throws -> [RunRecord] {
    guard FileManager.default.fileExists(atPath: historyURL.path) else {
      try cleanupManagedRunArtifacts(retaining: [])
      return []
    }

    let data = try Data(contentsOf: historyURL)
    let decoded = try decoder.decode([RunRecord].self, from: data)
    var didRecoverInterruptedRun = false
    let runs = Array(decoded.prefix(50)).map { record in
      var recovered = record
      if recovered.status == .running || recovered.status == .cancelling {
        recovered.status = .cancelled
        recovered.completedAt = recovered.completedAt ?? Date()
        recovered.errorMessage = "上次退出时任务仍在运行。"
        didRecoverInterruptedRun = true
      }
      return recovered
    }

    if didRecoverInterruptedRun || runs.count != decoded.count {
      try writeHistory(runs)
    }
    // No process survives an application relaunch. All old workspaces,
    // including the UUID.* layout used by the first prototype, are stale.
    try cleanupManagedRunArtifacts(retaining: [])
    return runs
  }

  func save(_ runs: [RunRecord]) throws {
    let retained = Array(runs.prefix(50))
    try writeHistory(retained)
    try cleanupManagedRunArtifacts(retaining: Set(retained.map(\.id)))
  }

  func workspace(for runID: UUID) throws -> RunWorkspace {
    let directory =
      runsURL
      .appendingPathComponent(runID.uuidString, isDirectory: true)
    try cleanupLegacyArtifacts(for: runID)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    return RunWorkspace(
      directory: directory,
      resumeBase: directory.appendingPathComponent("state", isDirectory: false)
    )
  }

  func finalize(_ runID: UUID, runs: [RunRecord]) throws {
    let retained = Array(runs.prefix(50))
    try writeHistory(retained)
    try cleanupWorkspace(for: runID)
    let activeRunIDs = Set(
      retained.compactMap { run in
        run.status == .running || run.status == .cancelling ? run.id : nil
      })
    try cleanupManagedRunArtifacts(retaining: activeRunIDs)
  }

  func clearHistory() throws {
    if FileManager.default.fileExists(atPath: historyURL.path) {
      try FileManager.default.removeItem(at: historyURL)
    }
    if FileManager.default.fileExists(atPath: runsURL.path) {
      try FileManager.default.removeItem(at: runsURL)
    }
  }

  private func writeHistory(_ runs: [RunRecord]) throws {
    try FileManager.default.createDirectory(at: baseURL, withIntermediateDirectories: true)
    let data = try encoder.encode(runs)
    try data.write(to: historyURL, options: [.atomic])
  }

  private func cleanupWorkspace(for runID: UUID) throws {
    let fileManager = FileManager.default
    let workspace = runsURL.appendingPathComponent(runID.uuidString, isDirectory: true)
    if fileManager.fileExists(atPath: workspace.path) {
      try fileManager.removeItem(at: workspace)
    }
    try cleanupLegacyArtifacts(for: runID)
  }

  private func cleanupLegacyArtifacts(for runID: UUID) throws {
    guard FileManager.default.fileExists(atPath: runsURL.path) else { return }
    let legacyPrefix = runID.uuidString + "."
    for item in try FileManager.default.contentsOfDirectory(
      at: runsURL,
      includingPropertiesForKeys: nil,
      options: [.skipsHiddenFiles]
    ) where item.lastPathComponent.hasPrefix(legacyPrefix) {
      try FileManager.default.removeItem(at: item)
    }
  }

  private func cleanupManagedRunArtifacts(retaining runIDs: Set<UUID>) throws {
    guard FileManager.default.fileExists(atPath: runsURL.path) else { return }
    let fileManager = FileManager.default
    for item in try fileManager.contentsOfDirectory(
      at: runsURL,
      includingPropertiesForKeys: nil,
      options: []
    ) {
      let name = item.lastPathComponent
      let exactRunID = UUID(uuidString: name)
      if let exactRunID, runIDs.contains(exactRunID) {
        continue
      }
      // A name beginning with UUID plus a suffix is a legacy checkpoint.
      // Unknown entries inside the private Runs directory are also orphans.
      try fileManager.removeItem(at: item)
    }

    if (try fileManager.contentsOfDirectory(atPath: runsURL.path)).isEmpty {
      try fileManager.removeItem(at: runsURL)
    }
  }

  private static func defaultBaseURL() -> URL {
    let support =
      FileManager.default.urls(
        for: .applicationSupportDirectory,
        in: .userDomainMask
      ).first ?? URL(fileURLWithPath: NSTemporaryDirectory())
    return support.appendingPathComponent("GNFS Workbench", isDirectory: true)
  }
}
