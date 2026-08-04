import Darwin
import Foundation

struct RunWorkspace: Equatable, Sendable {
  let directory: URL
  let resumeBase: URL
}

protocol RunStoring: Sendable {
  func load() async throws -> [RunRecord]
  func save(_ runs: [RunRecord]) async throws
  func workspace(for runID: UUID) async throws -> RunWorkspace
  func finalize(_ runID: UUID, runs: [RunRecord]) async throws
  func clearHistory() async throws
}

enum RunStoreError: LocalizedError, Equatable {
  case unsafeManagedPath(String)
  case historyQuarantined(String)

  var errorDescription: String? {
    switch self {
    case .unsafeManagedPath(let path):
      "受管存储路径不是预期的普通文件或目录，已拒绝访问：\(path)"
    case .historyQuarantined(let filename):
      "运行历史格式损坏或来自不兼容版本；原文件已隔离为 \(filename)。"
    }
  }
}

actor RunStore: RunStoring {
  private enum NodeKind {
    case directory
    case regularFile
    case symbolicLink
    case other
  }

  private static let legacyArtifactSuffixes = [
    ".poly_ckpt",
    ".fb_ckpt",
    ".sieve_ckpt",
    ".sieve_ckpt.tmp",
    ".reldata",
    ".relidx",
  ]

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

  func load() async throws -> [RunRecord] {
    guard try ensureDirectory(baseURL, createIfMissing: false) else {
      return []
    }

    guard let historyKind = try nodeKind(at: historyURL) else {
      let cleanupPlan = try managedRunArtifactsToRemove(retaining: [])
      try removeManagedRunArtifacts(cleanupPlan)
      return []
    }
    guard historyKind == .regularFile else {
      throw RunStoreError.unsafeManagedPath(historyURL.path)
    }

    let data = try Data(contentsOf: historyURL)
    let decoded: [RunRecord]
    do {
      decoded = try decoder.decode([RunRecord].self, from: data)
    } catch {
      let quarantineURL = baseURL.appendingPathComponent(
        "history.invalid-\(UUID().uuidString).json",
        isDirectory: false
      )
      try FileManager.default.moveItem(at: historyURL, to: quarantineURL)
      throw RunStoreError.historyQuarantined(quarantineURL.lastPathComponent)
    }

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

    let cleanupPlan = try managedRunArtifactsToRemove(retaining: [])
    if didRecoverInterruptedRun || runs.count != decoded.count {
      try writeHistory(runs)
    }
    // No process survives an application relaunch. Exact UUID workspaces and
    // known UUID checkpoint files are stale; unrelated entries are preserved.
    try removeManagedRunArtifacts(cleanupPlan)
    return runs
  }

  func save(_ runs: [RunRecord]) async throws {
    let retained = Array(runs.prefix(50))
    let cleanupPlan = try managedRunArtifactsToRemove(retaining: Set(retained.map(\.id)))
    try removeManagedRunArtifacts(cleanupPlan)
    try writeHistory(retained)
  }

  func workspace(for runID: UUID) async throws -> RunWorkspace {
    try ensureRunsDirectory(createIfMissing: true)
    try cleanupLegacyArtifacts(for: runID)

    let directory = runsURL.appendingPathComponent(runID.uuidString, isDirectory: true)
    if let kind = try nodeKind(at: directory) {
      guard kind == .directory else {
        throw RunStoreError.unsafeManagedPath(directory.path)
      }
    } else {
      try FileManager.default.createDirectory(
        at: directory,
        withIntermediateDirectories: false
      )
      guard try nodeKind(at: directory) == .directory else {
        throw RunStoreError.unsafeManagedPath(directory.path)
      }
    }

    return RunWorkspace(
      directory: directory,
      resumeBase: directory.appendingPathComponent("state", isDirectory: false)
    )
  }

  func finalize(_ runID: UUID, runs: [RunRecord]) async throws {
    let retained = Array(runs.prefix(50))
    let activeRunIDs = Set(
      retained.compactMap { run in
        run.status == .running || run.status == .cancelling ? run.id : nil
      })
    let cleanupPlan = try managedRunArtifactsToRemove(retaining: activeRunIDs)
    if !activeRunIDs.contains(runID) {
      let expectedWorkspace = runsURL.appendingPathComponent(
        runID.uuidString,
        isDirectory: true
      )
      if let kind = try nodeKind(at: expectedWorkspace), kind != .directory {
        throw RunStoreError.unsafeManagedPath(expectedWorkspace.path)
      }
    }
    try writeHistory(retained)
    try removeManagedRunArtifacts(cleanupPlan)
  }

  func clearHistory() async throws {
    // Validate and remove only known artifacts before mutating history. If an
    // unsafe link is present, history remains untouched and the caller can
    // surface the failure without claiming that cleanup succeeded.
    let cleanupPlan = try managedRunArtifactsToRemove(retaining: [])
    try removeManagedRunArtifacts(cleanupPlan)
    if let kind = try nodeKind(at: historyURL) {
      guard kind == .regularFile else {
        throw RunStoreError.unsafeManagedPath(historyURL.path)
      }
      try FileManager.default.removeItem(at: historyURL)
    }
  }

  private func writeHistory(_ runs: [RunRecord]) throws {
    try ensureDirectory(baseURL, createIfMissing: true)
    if let kind = try nodeKind(at: historyURL), kind != .regularFile {
      throw RunStoreError.unsafeManagedPath(historyURL.path)
    }
    let data = try encoder.encode(runs)
    try data.write(to: historyURL, options: [.atomic])
    guard try nodeKind(at: historyURL) == .regularFile else {
      throw RunStoreError.unsafeManagedPath(historyURL.path)
    }
  }

  private func cleanupLegacyArtifacts(for runID: UUID) throws {
    guard try ensureRunsDirectory(createIfMissing: false) else { return }
    var cleanupPlan: [URL] = []
    for suffix in Self.legacyArtifactSuffixes {
      let artifact = runsURL.appendingPathComponent(runID.uuidString + suffix)
      guard let kind = try nodeKind(at: artifact) else { continue }
      guard kind == .regularFile else {
        throw RunStoreError.unsafeManagedPath(artifact.path)
      }
      cleanupPlan.append(artifact)
    }
    for artifact in cleanupPlan {
      try FileManager.default.removeItem(at: artifact)
    }
  }

  private func managedRunArtifactsToRemove(retaining runIDs: Set<UUID>) throws -> [URL] {
    guard try ensureRunsDirectory(createIfMissing: false) else { return [] }
    let fileManager = FileManager.default
    var cleanupPlan: [URL] = []
    for item in try fileManager.contentsOfDirectory(
      at: runsURL,
      includingPropertiesForKeys: nil,
      options: []
    ) {
      let name = item.lastPathComponent
      if let runID = UUID(uuidString: name) {
        guard try nodeKind(at: item) == .directory else {
          throw RunStoreError.unsafeManagedPath(item.path)
        }
        if !runIDs.contains(runID) {
          cleanupPlan.append(item)
        }
        continue
      }

      guard isKnownLegacyArtifact(name) else {
        // Unknown entries are not ours to delete.
        continue
      }
      guard try nodeKind(at: item) == .regularFile else {
        throw RunStoreError.unsafeManagedPath(item.path)
      }
      cleanupPlan.append(item)
    }
    return cleanupPlan
  }

  private func removeManagedRunArtifacts(_ cleanupPlan: [URL]) throws {
    let fileManager = FileManager.default
    for item in cleanupPlan {
      try fileManager.removeItem(at: item)
    }
    guard try ensureRunsDirectory(createIfMissing: false) else { return }
    if try fileManager.contentsOfDirectory(atPath: runsURL.path).isEmpty {
      try fileManager.removeItem(at: runsURL)
    }
  }

  private func isKnownLegacyArtifact(_ name: String) -> Bool {
    for suffix in Self.legacyArtifactSuffixes where name.hasSuffix(suffix) {
      let prefix = String(name.dropLast(suffix.count))
      if UUID(uuidString: prefix) != nil {
        return true
      }
    }
    return false
  }

  @discardableResult
  private func ensureRunsDirectory(createIfMissing: Bool) throws -> Bool {
    guard try ensureDirectory(baseURL, createIfMissing: createIfMissing) else {
      return false
    }
    return try ensureDirectory(runsURL, createIfMissing: createIfMissing)
  }

  @discardableResult
  private func ensureDirectory(_ url: URL, createIfMissing: Bool) throws -> Bool {
    if let kind = try nodeKind(at: url) {
      guard kind == .directory else {
        throw RunStoreError.unsafeManagedPath(url.path)
      }
      return true
    }
    guard createIfMissing else { return false }
    try FileManager.default.createDirectory(
      at: url,
      withIntermediateDirectories: true
    )
    guard try nodeKind(at: url) == .directory else {
      throw RunStoreError.unsafeManagedPath(url.path)
    }
    return true
  }

  private func nodeKind(at url: URL) throws -> NodeKind? {
    var information = stat()
    if lstat(url.path, &information) == 0 {
      switch information.st_mode & S_IFMT {
      case S_IFDIR:
        return .directory
      case S_IFREG:
        return .regularFile
      case S_IFLNK:
        return .symbolicLink
      default:
        return .other
      }
    }

    let code = errno
    if code == ENOENT { return nil }
    throw POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO)
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
