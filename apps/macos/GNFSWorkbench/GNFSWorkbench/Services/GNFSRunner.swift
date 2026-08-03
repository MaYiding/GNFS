@preconcurrency import Foundation

enum GNFSRunnerError: LocalizedError, Equatable {
  case alreadyRunning
  case launchFailed(String)
  case malformedEvent(String)
  case unsupportedSchema(Int)
  case unexpectedTermination(Int32, String)

  var errorDescription: String? {
    switch self {
    case .alreadyRunning:
      "已有分解任务正在运行。"
    case .launchFailed(let message):
      "无法启动 GNFS 运算引擎：\(message)"
    case .malformedEvent(let line):
      "运算引擎返回了无法识别的数据：\(line.prefix(160))"
    case .unsupportedSchema(let version):
      "运算引擎事件协议版本不兼容（版本 \(version)）。"
    case .unexpectedTermination(let status, let stderr):
      stderr.isEmpty
        ? "运算引擎意外退出（状态 \(status)）。"
        : "运算引擎意外退出（状态 \(status)）：\(stderr)"
    }
  }
}

protocol GNFSRunning: Sendable {
  func start(
    configuration: RunConfiguration,
    resumeDirectory: URL
  ) async throws -> AsyncThrowingStream<CLIEvent, Error>

  func cancel() async
}

actor ProcessGNFSRunner: GNFSRunning {
  private let resolver: GNFSExecutableResolver
  private var process: Process?
  private var readerTask: Task<Void, Never>?

  init(resolver: GNFSExecutableResolver = GNFSExecutableResolver()) {
    self.resolver = resolver
  }

  func start(
    configuration: RunConfiguration,
    resumeDirectory: URL
  ) async throws -> AsyncThrowingStream<CLIEvent, Error> {
    try Task.checkCancellation()
    guard process == nil else { throw GNFSRunnerError.alreadyRunning }

    let executable = try resolver.resolve()
    try FileManager.default.createDirectory(
      at: resumeDirectory,
      withIntermediateDirectories: true
    )
    try Task.checkCancellation()

    let process = Process()
    let standardOutput = Pipe()
    let standardError = Pipe()
    process.executableURL = executable
    process.arguments = GNFSInvocation.arguments(for: configuration)
    process.standardOutput = standardOutput
    process.standardError = standardError
    process.currentDirectoryURL = resumeDirectory
    var environment = ProcessInfo.processInfo.environment
    environment["GNFS_RESUME"] = resumeDirectory.path
    environment["LC_ALL"] = "en_US.UTF-8"
    process.environment = environment

    do {
      try process.run()
    } catch {
      throw GNFSRunnerError.launchFailed(error.localizedDescription)
    }
    self.process = process

    let decoder = JSONDecoder()
    let (stream, continuation) = AsyncThrowingStream<CLIEvent, Error>.makeStream()
    let stderrTask = Task.detached(priority: .utility) {
      standardError.fileHandleForReading.readDataToEndOfFile()
    }
    let task = Task.detached(priority: .userInitiated) {
      var terminalEventReceived = false
      do {
        for try await line in standardOutput.fileHandleForReading.bytes.lines {
          guard !Task.isCancelled else { break }
          guard let data = line.data(using: .utf8) else {
            throw GNFSRunnerError.malformedEvent(line)
          }
          let event: CLIEvent
          do {
            event = try decoder.decode(CLIEvent.self, from: data)
          } catch {
            throw GNFSRunnerError.malformedEvent(line)
          }
          guard event.schemaVersion == 1 else {
            throw GNFSRunnerError.unsupportedSchema(event.schemaVersion)
          }
          if event.type == .result || event.type == .error {
            terminalEventReceived = true
          }
          continuation.yield(event)
        }

        process.waitUntilExit()
        let stderrData = await stderrTask.value
        let stderr = String(decoding: stderrData, as: UTF8.self)
          .trimmingCharacters(in: .whitespacesAndNewlines)
        if process.terminationStatus != 0 && !terminalEventReceived {
          continuation.finish(
            throwing: GNFSRunnerError.unexpectedTermination(
              process.terminationStatus,
              stderr
            ))
        } else {
          continuation.finish()
        }
      } catch is CancellationError {
        continuation.finish()
      } catch {
        continuation.finish(throwing: error)
      }
    }
    continuation.onTermination = { @Sendable _ in
      task.cancel()
      stderrTask.cancel()
    }
    readerTask = task
    return stream
  }

  func cancel() async {
    guard let process else { return }
    if process.isRunning {
      process.interrupt()
      try? await Task.sleep(for: .milliseconds(350))
    }
    if process.isRunning {
      process.terminate()
    }
    readerTask?.cancel()
    self.process = nil
    readerTask = nil
  }

}
