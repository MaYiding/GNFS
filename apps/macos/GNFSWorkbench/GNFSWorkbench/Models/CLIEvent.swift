import Foundation

struct CLIEvent: Decodable, Sendable {
  enum Kind: String, Decodable, Sendable {
    case started
    case progress
    case log
    case result
    case error
  }

  let schemaVersion: Int
  let type: Kind
  let number: String?
  let bitCount: UInt64?
  let digitCount: UInt64?
  let method: FactorMethod?
  let methodName: String?
  let methodReason: String?
  let completeFactorization: Bool?
  let phase: PipelinePhase?
  let phaseProgress: Double?
  let elapsed: Double?
  let message: String?
  let relationsFound: UInt64?
  let relationsTarget: UInt64?
  let specialQDone: UInt64?
  let matrixRows: UInt64?
  let matrixCols: UInt64?
  let dependencyIndex: Int?
  let dependenciesTotal: Int?
  let level: String?
  let timestamp: Double?
  let result: FactorizationResult?
  let code: String?

  enum CodingKeys: String, CodingKey {
    case schemaVersion = "schema_version"
    case type
    case number = "n"
    case bitCount = "n_bits"
    case digitCount = "n_digits"
    case method
    case methodName = "method_name"
    case methodReason = "method_reason"
    case completeFactorization = "complete_factorization"
    case phase
    case phaseProgress = "phase_progress"
    case elapsed = "elapsed_s"
    case message
    case relationsFound = "relations_found"
    case relationsTarget = "relations_target"
    case specialQDone = "special_q_done"
    case matrixRows = "matrix_rows"
    case matrixCols = "matrix_cols"
    case dependencyIndex = "dependency_index"
    case dependenciesTotal = "dependencies_total"
    case level
    case timestamp = "timestamp_s"
    case result
    case code
  }
}
