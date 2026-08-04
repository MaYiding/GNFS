import Foundation

enum FactorMethod: String, Codable, CaseIterable, Identifiable, Sendable {
  case auto
  case trial
  case rho
  case siqs
  case gnfs

  var id: String { rawValue }

  var displayName: String {
    switch self {
    case .auto: "自动"
    case .trial: "试除"
    case .rho: "Pollard Rho"
    case .siqs: "SIQS"
    case .gnfs: "GNFS"
    }
  }

  var explanation: String {
    switch self {
    case .auto: "根据整数规模自动选择合适方法"
    case .trial: "适用于包含小因子的整数"
    case .rho: "适用于中小规模整数"
    case .siqs: "适用于约 25 至 100 位十进制整数"
    case .gnfs: "强制运行完整一般数域筛流水线"
    }
  }
}

enum PipelinePhase: String, Codable, CaseIterable, Identifiable, Sendable {
  case polynomial = "poly"
  case factorBase = "fb"
  case sieving = "sieve"
  case filtering = "filter"
  case linearAlgebra = "linalg"
  case squareRoot = "sqrt"
  case extraction = "extract"
  case done

  var id: String { rawValue }

  var order: Int {
    Self.allCases.firstIndex(of: self) ?? 0
  }

  var displayName: String {
    switch self {
    case .polynomial: "多项式选择"
    case .factorBase: "因子基"
    case .sieving: "格筛"
    case .filtering: "关系过滤"
    case .linearAlgebra: "线性代数"
    case .squareRoot: "平方根"
    case .extraction: "因子提取"
    case .done: "完成"
    }
  }
}

enum RunStatus: String, Codable, Sendable {
  case ready
  case running
  case cancelling
  case succeeded
  case failed
  case cancelled

  var isTerminal: Bool {
    self == .succeeded || self == .failed || self == .cancelled
  }
}

enum LogSeverity: String, Codable, Sendable {
  case trace = "TRACE"
  case debug = "DEBUG"
  case info = "INFO"
  case warning = "WARN"
  case error = "ERROR"

  init(protocolValue: String) {
    self = Self(rawValue: protocolValue) ?? .info
  }
}

struct AdvancedParameters: Codable, Equatable, Sendable {
  var degree: Int?
  var rationalBound: Int?
  var algebraicBound: Int?
  var largePrimeBound: UInt64?
  var sieveWidth: Int?
  var sieveHeight: Int?

  static let automatic = AdvancedParameters()

  var isAutomatic: Bool {
    degree == nil && rationalBound == nil && algebraicBound == nil && largePrimeBound == nil
      && sieveWidth == nil && sieveHeight == nil
  }

  var validationMessage: String? {
    if let degree, !(3...8).contains(degree) {
      return "多项式次数必须在 3 到 8 之间。"
    }
    if let rationalBound, !(2...Int(UInt32.max)).contains(rationalBound) {
      return "有理因子基上界必须在 2 到 \(UInt32.max) 之间。"
    }
    if let algebraicBound, !(2...Int(UInt32.max)).contains(algebraicBound) {
      return "代数因子基上界必须在 2 到 \(UInt32.max) 之间。"
    }
    if let largePrimeBound, largePrimeBound < 2 {
      return "大素数上界必须大于等于 2。"
    }
    if let sieveWidth, !(16...Int(Int32.max)).contains(sieveWidth) {
      return "筛区宽度必须在 16 到 \(Int32.max) 之间。"
    }
    if let sieveHeight, !(1...Int(Int32.max)).contains(sieveHeight) {
      return "筛区高度必须在 1 到 \(Int32.max) 之间。"
    }
    return nil
  }
}

struct RunConfiguration: Codable, Equatable, Sendable {
  var number: String = "1000036000099"
  var method: FactorMethod = .auto
  var parameters: AdvancedParameters = .automatic
}

struct ProgressSample: Identifiable, Codable, Equatable, Sendable {
  let id: UUID
  let elapsed: Double
  let relations: UInt64
  let rate: Double

  init(id: UUID = UUID(), elapsed: Double, relations: UInt64, rate: Double) {
    self.id = id
    self.elapsed = elapsed
    self.relations = relations
    self.rate = rate
  }
}

struct RunLogEntry: Identifiable, Codable, Equatable, Sendable {
  let id: UUID
  let timestamp: Double
  let severity: LogSeverity
  let phase: PipelinePhase?
  let message: String

  init(
    id: UUID = UUID(),
    timestamp: Double,
    severity: LogSeverity,
    phase: PipelinePhase?,
    message: String
  ) {
    self.id = id
    self.timestamp = timestamp
    self.severity = severity
    self.phase = phase
    self.message = message
  }
}

struct FactorTimings: Codable, Equatable, Sendable {
  let total: Double
  let polynomial: Double
  let factorBase: Double
  let sieving: Double
  let filtering: Double
  let linearAlgebra: Double
  let squareRoot: Double
  let extraction: Double

  enum CodingKeys: String, CodingKey {
    case total = "total_s"
    case polynomial = "poly_s"
    case factorBase = "fb_s"
    case sieving = "sieve_s"
    case filtering = "filter_s"
    case linearAlgebra = "linalg_s"
    case squareRoot = "sqrt_s"
    case extraction = "extract_s"
  }
}

struct FactorStatistics: Codable, Equatable, Sendable {
  let degree: UInt64
  let rationalBound: UInt64
  let algebraicBound: UInt64
  let largePrimeBound: UInt64
  let rationalPrimes: UInt64
  let algebraicPrimes: UInt64
  let specialQProcessed: UInt64
  let candidatesTotal: UInt64
  let relationsFound: UInt64
  let fullRelations: UInt64
  let partial1LP: UInt64
  let partial2LP: UInt64
  let relationsAfterFilter: UInt64
  let singletonsRemoved: UInt64
  let mergedRelations: UInt64
  let matrixRows: UInt64
  let matrixCols: UInt64
  let matrixExcess: Int64
  let dependenciesFound: UInt64
  let dependenciesTried: Int64

  enum CodingKeys: String, CodingKey {
    case degree
    case rationalBound = "rational_bound"
    case algebraicBound = "algebraic_bound"
    case largePrimeBound = "large_prime_bound"
    case rationalPrimes = "rational_primes"
    case algebraicPrimes = "algebraic_primes"
    case specialQProcessed = "special_q_processed"
    case candidatesTotal = "candidates_total"
    case relationsFound = "relations_found"
    case fullRelations = "full_relations"
    case partial1LP = "partial_1lp"
    case partial2LP = "partial_2lp"
    case relationsAfterFilter = "relations_after_filter"
    case singletonsRemoved = "singletons_removed"
    case mergedRelations = "merged_relations"
    case matrixRows = "matrix_rows"
    case matrixCols = "matrix_cols"
    case matrixExcess = "matrix_excess"
    case dependenciesFound = "dependencies_found"
    case dependenciesTried = "dependencies_tried"
  }
}

struct FactorizationResult: Codable, Equatable, Sendable {
  let success: Bool
  let factorizationComplete: Bool?
  let factorsPrime: Bool?
  let number: String
  let bitCount: UInt64
  let digitCount: UInt64
  let method: FactorMethod
  let methodName: String
  let methodReason: String?
  let factors: [String]
  let timings: FactorTimings
  let statistics: FactorStatistics

  enum CodingKeys: String, CodingKey {
    case success
    case factorizationComplete = "factorization_complete"
    case factorsPrime = "factors_prime"
    case number = "n"
    case bitCount = "n_bits"
    case digitCount = "n_digits"
    case method
    case methodName = "method_name"
    case methodReason = "method_reason"
    case factors
    case timings
    case statistics = "stats"
  }

  var hasVerifiedProduct: Bool {
    guard success, !factors.isEmpty else { return false }
    return factors.reduce("1") { DecimalBigUInt.multiply($0, $1) }
      == DecimalBigUInt.normalized(number)
  }

  var isVerified: Bool {
    factorizationComplete == true && factorsPrime == true && hasVerifiedProduct
  }

  var distinctPrimeCount: Int {
    Set(factors).count
  }

  var factorExpression: String {
    var order: [String] = []
    var exponents: [String: Int] = [:]
    for factor in factors {
      if exponents[factor] == nil {
        order.append(factor)
      }
      exponents[factor, default: 0] += 1
    }
    return order.map { factor in
      let exponent = exponents[factor, default: 1]
      return exponent == 1 ? factor : "\(factor)^\(exponent)"
    }
    .joined(separator: " × ")
  }
}

struct RunRecord: Identifiable, Codable, Equatable, Sendable {
  let id: UUID
  var configuration: RunConfiguration
  var status: RunStatus
  let createdAt: Date
  var completedAt: Date?
  var selectedMethod: FactorMethod?
  var methodReason: String?
  var phase: PipelinePhase
  var phaseProgress: Double?
  var elapsed: Double
  var relationsFound: UInt64
  var relationsTarget: UInt64
  var specialQDone: UInt64
  var matrixRows: UInt64
  var matrixCols: UInt64
  var logs: [RunLogEntry]
  var samples: [ProgressSample]
  var result: FactorizationResult?
  var errorMessage: String?

  init(id: UUID = UUID(), configuration: RunConfiguration, createdAt: Date = Date()) {
    self.id = id
    self.configuration = configuration
    self.status = .ready
    self.createdAt = createdAt
    self.completedAt = nil
    self.selectedMethod = nil
    self.methodReason = nil
    self.phase = .polynomial
    self.phaseProgress = nil
    self.elapsed = 0
    self.relationsFound = 0
    self.relationsTarget = 0
    self.specialQDone = 0
    self.matrixRows = 0
    self.matrixCols = 0
    self.logs = []
    self.samples = []
    self.result = nil
    self.errorMessage = nil
  }

  var displayNumber: String {
    configuration.number
  }

  var progressFraction: Double? {
    if let phaseProgress { return min(max(phaseProgress, 0), 1) }
    guard relationsTarget > 0 else { return nil }
    return min(Double(relationsFound) / Double(relationsTarget), 1)
  }
}
