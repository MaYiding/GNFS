import Foundation

actor DemoGNFSRunner: GNFSRunning {
  private var task: Task<Void, Never>?

  func start(
    configuration: RunConfiguration,
    resumeDirectory: URL
  ) async throws -> AsyncThrowingStream<CLIEvent, Error> {
    task?.cancel()
    let number = configuration.number

    return AsyncThrowingStream { continuation in
      let task = Task {
        let lines = Self.demoLines(number: number)
        let delays: [Duration] = [
          .milliseconds(80), .milliseconds(130), .milliseconds(130),
          .milliseconds(130), .milliseconds(130), .milliseconds(130),
          .milliseconds(130), .milliseconds(130), .milliseconds(130),
          .milliseconds(130), .milliseconds(180),
        ]

        do {
          for (index, line) in lines.enumerated() {
            try Task.checkCancellation()
            try await Task.sleep(for: delays[min(index, delays.count - 1)])
            guard let data = line.data(using: .utf8) else { continue }
            continuation.yield(try JSONDecoder().decode(CLIEvent.self, from: data))
          }
          // Keep the design-preview run alive so the live dashboard
          // can be inspected and its cancellation path remains real.
          try await Task.sleep(for: .seconds(3_600))
          continuation.finish()
        } catch is CancellationError {
          continuation.finish()
        } catch {
          continuation.finish(throwing: error)
        }
      }
      continuation.onTermination = { @Sendable _ in task.cancel() }
      Task { self.store(task) }
    }
  }

  func cancel() async {
    task?.cancel()
    task = nil
  }

  private func store(_ task: Task<Void, Never>) {
    self.task = task
  }

  private static func demoLines(number: String) -> [String] {
    let safeNumber = number == "1000036000099" ? number : "1000036000099"
    return [
      #"{"schema_version":1,"type":"started","n":"\#(safeNumber)","n_bits":40,"n_digits":13,"method":"gnfs","method_name":"GNFS","method_reason":"界面演示：强制完整流水线","complete_factorization":true}"#,
      #"{"schema_version":1,"type":"log","level":"INFO","phase":"poly","timestamp_s":0.08,"message":"正在搜索低范数多项式"}"#,
      #"{"schema_version":1,"type":"progress","phase":"poly","phase_progress":1,"elapsed_s":0.42,"message":"多项式选择完成","relations_found":0,"relations_target":0,"special_q_done":0,"matrix_rows":0,"matrix_cols":0,"dependency_index":-1,"dependencies_total":0}"#,
      #"{"schema_version":1,"type":"progress","phase":"fb","phase_progress":1,"elapsed_s":0.76,"message":"因子基构建完成","relations_found":0,"relations_target":0,"special_q_done":0,"matrix_rows":0,"matrix_cols":0,"dependency_index":-1,"dependencies_total":0}"#,
      #"{"schema_version":1,"type":"log","level":"INFO","phase":"sieve","timestamp_s":0.88,"message":"格筛已启动，目标 18,400 条关系"}"#,
      #"{"schema_version":1,"type":"progress","phase":"sieve","phase_progress":0.18,"elapsed_s":1.42,"message":"正在处理 special-q 1201","relations_found":3312,"relations_target":18400,"special_q_done":1201,"matrix_rows":0,"matrix_cols":0,"dependency_index":-1,"dependencies_total":0}"#,
      #"{"schema_version":1,"type":"progress","phase":"sieve","phase_progress":0.31,"elapsed_s":2.07,"message":"正在处理 special-q 2078","relations_found":5704,"relations_target":18400,"special_q_done":2078,"matrix_rows":0,"matrix_cols":0,"dependency_index":-1,"dependencies_total":0}"#,
      #"{"schema_version":1,"type":"progress","phase":"sieve","phase_progress":0.47,"elapsed_s":2.72,"message":"正在处理 special-q 3136","relations_found":8648,"relations_target":18400,"special_q_done":3136,"matrix_rows":0,"matrix_cols":0,"dependency_index":-1,"dependencies_total":0}"#,
      #"{"schema_version":1,"type":"progress","phase":"sieve","phase_progress":0.63,"elapsed_s":3.37,"message":"正在处理 special-q 4204","relations_found":11592,"relations_target":18400,"special_q_done":4204,"matrix_rows":0,"matrix_cols":0,"dependency_index":-1,"dependencies_total":0}"#,
      #"{"schema_version":1,"type":"progress","phase":"sieve","phase_progress":0.78,"elapsed_s":4.02,"message":"正在处理 special-q 5211","relations_found":14352,"relations_target":18400,"special_q_done":5211,"matrix_rows":0,"matrix_cols":0,"dependency_index":-1,"dependencies_total":0}"#,
      #"{"schema_version":1,"type":"progress","phase":"sieve","phase_progress":0.86,"elapsed_s":4.67,"message":"正在处理 special-q 5748","relations_found":15824,"relations_target":18400,"special_q_done":5748,"matrix_rows":0,"matrix_cols":0,"dependency_index":-1,"dependencies_total":0}"#,
    ]
  }
}
