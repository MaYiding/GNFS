import XCTest

@testable import GNFSWorkbench

final class GNFSInvocationTests: XCTestCase {
  func testBuildsStableMachineProtocolArguments() {
    let configuration = RunConfiguration(
      number: "1000036000099",
      method: .gnfs,
      parameters: AdvancedParameters(
        degree: 5,
        rationalBound: 50_000,
        algebraicBound: 70_000,
        largePrimeBound: 2_000_000,
        sieveWidth: 32_768,
        sieveHeight: 4_096
      )
    )

    XCTAssertEqual(
      GNFSInvocation.arguments(for: configuration),
      [
        "1000036000099", "--event-stream", "--complete", "--lang", "zh", "--method", "gnfs",
        "--degree", "5", "--fb-rational", "50000", "--fb-algebraic", "70000",
        "--lp-bound", "2000000", "--sieve-width", "32768", "--sieve-height", "4096",
      ])
  }

  func testAutomaticParametersDoNotCreateOverrides() {
    let arguments = GNFSInvocation.arguments(for: RunConfiguration())
    XCTAssertEqual(
      arguments,
      [
        "1000036000099", "--event-stream", "--complete", "--lang", "zh", "--method", "auto",
      ])
  }
}
