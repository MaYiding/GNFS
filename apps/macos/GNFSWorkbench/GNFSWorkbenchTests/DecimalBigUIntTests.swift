import XCTest

@testable import GNFSWorkbench

final class DecimalBigUIntTests: XCTestCase {
  func testMultipliesValuesBeyondNativeIntegerWidth() {
    XCTAssertEqual(
      DecimalBigUInt.multiply("12345678901234567890", "98765432109876543210"),
      "1219326311370217952237463801111263526900"
    )
  }

  func testNormalizesAndGroupsDecimalInput() {
    XCTAssertEqual(DecimalBigUInt.normalized("00000123456"), "123456")
    XCTAssertEqual(DecimalBigUInt.grouped("1000036000099"), "1 000 036 000 099")
  }

  func testRejectsUnicodeDigitsAndSmallValues() {
    XCTAssertEqual(
      IntegerInputValidator.validate("１２３"),
      .invalid("仅支持十进制数字，或带 0x 前缀的十六进制整数。")
    )
    XCTAssertEqual(
      IntegerInputValidator.validate("0x1"),
      .invalid("整数 N 必须大于 1。")
    )
    XCTAssertEqual(IntegerInputValidator.validate("0x00Ff"), .valid("0xff"))
    XCTAssertEqual(IntegerInputValidator.validate("1_000 033"), .valid("1000033"))
  }
}
