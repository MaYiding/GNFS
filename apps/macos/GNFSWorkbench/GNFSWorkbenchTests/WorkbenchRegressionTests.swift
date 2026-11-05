import Foundation
import XCTest

@testable import GNFSWorkbench

final class WorkbenchRegressionTests: XCTestCase {
  func testProtocolCountsPreserveUnsignedBoundariesAndUnknownSentinel() throws {
    let json =
      #"{"schema_version":1,"type":"progress","dependency_index":18446744073709551614,"dependencies_total":18446744073709551615,"attempt_index":18446744073709551615}"#
    let event = try JSONDecoder().decode(CLIEvent.self, from: Data(json.utf8))
    XCTAssertEqual(event.dependencyIndex, UInt64.max - 1)
    XCTAssertEqual(event.dependenciesTotal, UInt64.max)
    XCTAssertEqual(event.attemptIndex, UInt64.max)
    let sentinel = json.replacingOccurrences(of: "18446744073709551614", with: "-1")
    XCTAssertNil(try JSONDecoder().decode(CLIEvent.self, from: Data(sentinel.utf8)).dependencyIndex)
  }

  func testSieveCompletionDoesNotPretendRelationTargetWasReached() {
    var run = RunRecord(configuration: RunConfiguration())
    run.phase = .sieving
    run.phaseProgress = 1
    run.relationsFound = 100
    run.relationsTarget = 1000
    XCTAssertEqual(run.progressFraction, 0.1)
  }

  func testExportPreservesExactInputParametersAndLogs() throws {
    var run = RunRecord(
      configuration: RunConfiguration(number: "0xffffffffffffffffffffffff", method: .gnfs))
    run.configuration.parameters.rationalBound = 5000
    run.logs = [.init(timestamp: 1, severity: .warning, phase: .sieving, message: "diagnostic")]
    let document = try RunReportDocument(run: run)
    let decoder = JSONDecoder()
    decoder.dateDecodingStrategy = .iso8601
    let restored = try decoder.decode(RunRecord.self, from: document.data)
    XCTAssertEqual(restored.id, run.id)
    XCTAssertEqual(restored.configuration, run.configuration)
    XCTAssertEqual(restored.logs, run.logs)
  }

  func testUnknownLaterPhaseDoesNotReuseSievePercentage() {
    var run = RunRecord(configuration: RunConfiguration())
    run.relationsFound = 100
    run.relationsTarget = 100
    run.phase = .sieving
    XCTAssertEqual(run.progressFraction, 1)
    for phase in [PipelinePhase.filtering, .linearAlgebra, .squareRoot, .extraction] {
      run.phase = phase
      XCTAssertNil(run.progressFraction)
    }
    run.phaseProgress = 0.5
    XCTAssertEqual(run.progressFraction, 0.5)
    run.phaseProgress = .nan
    XCTAssertNil(run.progressFraction)
  }

  func testParameterTextRejectsMalformedOutOfRangeAndOverflowValues() {
    var form = ParameterForm(configuration: RunConfiguration())
    for text in ["abc", "-1", "1.5", "1e3", "２", "4294967296"] {
      form.values[.rationalBound] = text
      XCTAssertNotNil(form.error(for: .rationalBound), text)
      XCTAssertNil(form.configuration, text)
    }
    form.reset()
    form.values[.largePrimeBound] = "18446744073709551616"
    XCTAssertNil(form.configuration)
    form.values[.largePrimeBound] = "18446744073709551615"
    XCTAssertEqual(form.configuration?.parameters.largePrimeBound, UInt64.max)
    form.values[.sieveWidth] = "15"
    XCTAssertNil(form.configuration)
    form.values[.sieveWidth] = "16"
    XCTAssertEqual(form.configuration?.parameters.sieveWidth, 16)
    form.reset()
    XCTAssertEqual(form.configuration?.parameters, .automatic)
  }

  func testParameterEditingPreservesOriginalUntilApplied() {
    let original = RunConfiguration(number: "360", method: .rho, parameters: .automatic)
    var form = ParameterForm(configuration: original)
    form.method = .gnfs
    form.values[.rationalBound] = "5000"
    XCTAssertEqual(original.method, .rho)
    XCTAssertNil(original.parameters.rationalBound)
    XCTAssertEqual(form.configuration?.number, "360")
    XCTAssertEqual(form.configuration?.parameters.rationalBound, 5000)
  }

  func testCanonicalIntegerComparisonIsExactBeyondUInt64() {
    XCTAssertEqual(
      DecimalBigUInt.decimalValue("0xffffffffffffffffffffffffffffffff"),
      "340282366920938463463374607431768211455")
    XCTAssertEqual(DecimalBigUInt.decimalValue("000,360"), "360")
    XCTAssertNil(DecimalBigUInt.decimalValue("garbage"))
  }

  func testDurationHandlesUntrustedMagnitude() {
    XCTAssertEqual(DisplayFormat.duration(.infinity), "—")
    XCTAssertEqual(DisplayFormat.duration(-1), "—")
    XCTAssertEqual(DisplayFormat.duration(1e100), "1.00e+100 s")
    XCTAssertEqual(DisplayFormat.duration(3661), "1 h 1 m")
    XCTAssertEqual(DisplayFormat.duration(60), "1 m 0 s")
  }

  func testNonGNFSRunDoesNotClaimGNFSStages() {
    var run = RunRecord(configuration: RunConfiguration())
    for method in [FactorMethod.auto, .trial, .rho, .siqs] {
      run.selectedMethod = method
      XCTAssertFalse(run.usesGNFSPipeline)
    }
    run.selectedMethod = .gnfs
    XCTAssertFalse(run.usesGNFSPipeline)
    run.activeAlgorithm = "gnfs"
    XCTAssertTrue(run.usesGNFSPipeline)
  }

  func testDuplicateHistoryIDsAreQuarantined() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    defer { try? FileManager.default.removeItem(at: root) }
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    let run = RunRecord(configuration: RunConfiguration())
    let encoder = JSONEncoder()
    encoder.dateEncodingStrategy = .iso8601
    let data = try encoder.encode([run, run])
    try data.write(to: root.appendingPathComponent("history.json"))
    let store = RunStore(baseURL: root)
    do {
      _ = try await store.load()
      XCTFail("Duplicate IDs must be quarantined")
    } catch RunStoreError.historyQuarantined(let name) {
      XCTAssertEqual(try Data(contentsOf: root.appendingPathComponent(name)), data)
    }
    let recovered = try await store.load()
    XCTAssertTrue(recovered.isEmpty)
  }
}
