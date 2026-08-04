import Foundation

enum GNFSExecutableResolverError: LocalizedError {
  case missingExecutable([URL])

  var errorDescription: String? {
    switch self {
    case .missingExecutable(let candidates):
      let searched = candidates.map(\.path).joined(separator: "\n")
      return "找不到 GNFS 运算引擎。已检查：\n\(searched)"
    }
  }
}

struct GNFSExecutableResolver: Sendable {
  var bundle: Bundle = .main
  var environment: [String: String] = ProcessInfo.processInfo.environment
  var workingDirectory: URL = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)

  func resolve() throws -> URL {
    var candidates: [URL] = []

    if let override = environment["GNFS_CLI_PATH"], !override.isEmpty {
      candidates.append(URL(fileURLWithPath: override))
    }
    if let bundled = bundle.url(forResource: "gnfs", withExtension: nil) {
      candidates.append(bundled)
    }

    var directory = workingDirectory.standardizedFileURL
    for _ in 0..<7 {
      candidates.append(directory.appendingPathComponent("build/gnfs"))
      let parent = directory.deletingLastPathComponent()
      if parent == directory { break }
      directory = parent
    }

    if let match = candidates.first(where: { candidate in
      FileManager.default.isExecutableFile(atPath: candidate.path)
    }) {
      return match
    }
    throw GNFSExecutableResolverError.missingExecutable(candidates)
  }
}
