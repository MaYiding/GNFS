import Foundation
import XCTest

@testable import GNFSWorkbench

final class CLIEventTests: XCTestCase {
  func testDecodesProgressEvent() throws {
    let json =
      #"{"schema_version":1,"type":"progress","phase":"sieve","phase_progress":0.63,"elapsed_s":3.37,"message":"sieving","relations_found":11592,"relations_target":18400,"special_q_done":4204,"matrix_rows":0,"matrix_cols":0,"dependency_index":-1,"dependencies_total":0}"#
    let event = try JSONDecoder().decode(CLIEvent.self, from: Data(json.utf8))

    XCTAssertEqual(event.schemaVersion, 1)
    XCTAssertEqual(event.type, .progress)
    XCTAssertEqual(event.phase, .sieving)
    XCTAssertEqual(event.relationsFound, 11_592)
    XCTAssertEqual(event.relationsTarget, 18_400)
  }

  func testDecodesAndVerifiesResultEvent() throws {
    let json =
      #"{"schema_version":1,"type":"result","result":{"success":true,"factorization_complete":true,"factors_prime":true,"n":"1000036000099","n_bits":40,"n_digits":13,"method":"rho","method_name":"Pollard Rho","method_reason":"GMP rho fallback","factors":["1000003","1000033"],"timings":{"total_s":0.007,"poly_s":0,"fb_s":0,"sieve_s":0,"filter_s":0,"linalg_s":0,"sqrt_s":0,"extract_s":0},"stats":{"degree":3,"rational_bound":5000,"algebraic_bound":10000,"large_prime_bound":131072,"rational_primes":0,"algebraic_primes":0,"special_q_processed":0,"candidates_total":0,"relations_found":0,"full_relations":0,"partial_1lp":0,"partial_2lp":0,"relations_after_filter":0,"singletons_removed":0,"merged_relations":0,"matrix_rows":0,"matrix_cols":0,"matrix_excess":0,"dependencies_found":0,"dependencies_tried":0}}}"#
    let event = try JSONDecoder().decode(CLIEvent.self, from: Data(json.utf8))

    XCTAssertEqual(event.type, .result)
    XCTAssertEqual(event.result?.factors, ["1000003", "1000033"])
    XCTAssertEqual(event.result?.isVerified, true)
  }
}
