import Foundation

actor RunStore {
  private let baseURL: URL
  private let historyURL: URL
  private let encoder: JSONEncoder
  private let decoder: JSONDecoder

  init(baseURL: URL? = nil) {
    let root = baseURL ?? Self.defaultBaseURL()
    self.baseURL = root
    self.historyURL = root.appendingPathComponent("history.json")

    let encoder = JSONEncoder()
    encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
    encoder.dateEncodingStrategy = .iso8601
    self.encoder = encoder

    let decoder = JSONDecoder()
    decoder.dateDecodingStrategy = .iso8601
    self.decoder = decoder
  }

  func load() -> [RunRecord] {
    guard let data = try? Data(contentsOf: historyURL),
      let runs = try? decoder.decode([RunRecord].self, from: data)
    else {
      return []
    }
    return runs.map { record in
      var recovered = record
      if recovered.status == .running || recovered.status == .cancelling {
        recovered.status = .cancelled
        recovered.completedAt = recovered.completedAt ?? Date()
        recovered.errorMessage = "上次退出时任务仍在运行。"
      }
      return recovered
    }
  }

  func save(_ runs: [RunRecord]) throws {
    try FileManager.default.createDirectory(at: baseURL, withIntermediateDirectories: true)
    let data = try encoder.encode(Array(runs.prefix(50)))
    try data.write(to: historyURL, options: [.atomic])
  }

  func resumeDirectory(for runID: UUID) throws -> URL {
    let url =
      baseURL
      .appendingPathComponent("Runs", isDirectory: true)
      .appendingPathComponent(runID.uuidString, isDirectory: true)
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    return url
  }

  func clearHistory() throws {
    guard FileManager.default.fileExists(atPath: historyURL.path) else { return }
    try FileManager.default.removeItem(at: historyURL)
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
