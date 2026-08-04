import Foundation

enum DecimalBigUInt {
  private static func isASCIIDigit(_ character: Character) -> Bool {
    character >= "0" && character <= "9"
  }

  static func normalized(_ value: String) -> String {
    let digits = value.drop(while: { $0 == "0" })
    return digits.isEmpty ? "0" : String(digits)
  }

  static func multiply(_ lhs: String, _ rhs: String) -> String {
    let left = normalized(lhs)
    let right = normalized(rhs)
    guard left != "0", right != "0" else { return "0" }
    guard left.allSatisfy(isASCIIDigit), right.allSatisfy(isASCIIDigit) else { return "" }

    let a = left.reversed().compactMap(\.wholeNumberValue)
    let b = right.reversed().compactMap(\.wholeNumberValue)
    var product = Array(repeating: 0, count: a.count + b.count)

    for i in a.indices {
      for j in b.indices {
        product[i + j] += a[i] * b[j]
      }
    }

    for index in 0..<(product.count - 1) {
      product[index + 1] += product[index] / 10
      product[index] %= 10
    }
    while product.count > 1, product.last == 0 {
      product.removeLast()
    }
    return product.reversed().map(String.init).joined()
  }

  static func grouped(_ value: String) -> String {
    let normalized = normalized(value)
    guard normalized.allSatisfy(isASCIIDigit) else { return value }

    var result: [Character] = []
    result.reserveCapacity(normalized.count + normalized.count / 3)
    for (offset, character) in normalized.reversed().enumerated() {
      if offset > 0, offset.isMultiple(of: 3) {
        result.append(" ")
      }
      result.append(character)
    }
    return String(result.reversed())
  }
}

enum IntegerInputValidation: Equatable, Sendable {
  case valid(String)
  case invalid(String)
}

enum IntegerInputValidator {
  private static func isASCIIDigit(_ character: Character) -> Bool {
    character >= "0" && character <= "9"
  }

  private static func isASCIIHexDigit(_ character: Character) -> Bool {
    isASCIIDigit(character) || (character >= "a" && character <= "f")
      || (character >= "A" && character <= "F")
  }

  static func validate(_ rawValue: String) -> IntegerInputValidation {
    let separators = CharacterSet.whitespacesAndNewlines.union(
      CharacterSet(charactersIn: "_,， ")
    )
    let compact = rawValue.unicodeScalars
      .filter { !separators.contains($0) }
      .map(String.init)
      .joined()

    guard !compact.isEmpty else {
      return .invalid("请输入待分解的整数。")
    }

    if compact.lowercased().hasPrefix("0x") {
      let hex = compact.dropFirst(2)
      guard !hex.isEmpty, hex.allSatisfy(isASCIIHexDigit) else {
        return .invalid("十六进制整数需要使用 0x 前缀和有效数字。")
      }
      let significant = hex.drop(while: { $0 == "0" })
      guard significant.count > 1 || significant.first.map({ $0 != "0" && $0 != "1" }) == true
      else {
        return .invalid("整数 N 必须大于 1。")
      }
      return .valid("0x" + (significant.isEmpty ? "0" : String(significant).lowercased()))
    }

    guard compact.allSatisfy(isASCIIDigit) else {
      return .invalid("仅支持十进制数字，或带 0x 前缀的十六进制整数。")
    }
    let normalized = DecimalBigUInt.normalized(compact)
    guard normalized.count > 1 || normalized > "1" else {
      return .invalid("整数 N 必须大于 1。")
    }
    return .valid(normalized)
  }
}
