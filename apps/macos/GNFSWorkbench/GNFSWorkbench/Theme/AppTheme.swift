import SwiftUI

enum AppTheme {
  static let canvas = Color(red: 0.055, green: 0.067, blue: 0.082)
  static let header = Color(red: 0.066, green: 0.078, blue: 0.094)
  static let elevated = Color(red: 0.075, green: 0.088, blue: 0.108)
  static let surface = Color(red: 0.092, green: 0.106, blue: 0.128)
  static let surfaceHover = Color(red: 0.115, green: 0.132, blue: 0.158)
  static let separator = Color.white.opacity(0.11)
  static let separatorStrong = Color.white.opacity(0.18)

  static let primaryText = Color(red: 0.91, green: 0.925, blue: 0.95)
  static let secondaryText = Color(red: 0.62, green: 0.66, blue: 0.72)
  static let tertiaryText = Color(red: 0.43, green: 0.47, blue: 0.54)

  static let indigo = Color(red: 0.31, green: 0.37, blue: 0.98)
  static let cyan = Color(red: 0.10, green: 0.72, blue: 0.94)
  static let success = Color(red: 0.24, green: 0.72, blue: 0.50)
  static let warning = Color(red: 0.95, green: 0.65, blue: 0.25)
  static let danger = Color(red: 0.95, green: 0.32, blue: 0.35)

  static let progressGradient = LinearGradient(
    colors: [indigo, cyan],
    startPoint: .leading,
    endPoint: .trailing
  )

  static let contentPadding: CGFloat = 38
  static let sectionSpacing: CGFloat = 22
  static let controlRadius: CGFloat = 8
  static let panelRadius: CGFloat = 10
  static let headerHeight: CGFloat = 80
  static let contentMaxWidth: CGFloat = 1_520
}

extension Font {
  static func gnfsMonospaced(size: CGFloat, weight: Weight = .regular) -> Font {
    .system(size: size, weight: weight, design: .monospaced)
  }
}
