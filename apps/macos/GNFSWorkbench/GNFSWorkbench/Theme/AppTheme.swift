import AppKit
import SwiftUI

enum AppTheme {
  static let canvas = adaptive(light: 0xFFFFFF, dark: 0x1D1D1F)
  static let header = adaptive(light: 0xF3F3F5, dark: 0x252527)
  static let surface = adaptive(light: 0xF5F6F8, dark: 0x27272A)
  static let elevated = adaptive(light: 0xFAFAFC, dark: 0x303033)
  static let surfaceHover = Color.primary.opacity(0.055)
  static let separator = Color.primary.opacity(0.08)
  static let separatorStrong = Color.primary.opacity(0.14)

  static let primaryText = Color.primary
  static let secondaryText = Color.secondary
  static let tertiaryText = Color.secondary
  static let indigo = adaptive(light: 0x3258B8, dark: 0x91ADFF)
  static let cyan = adaptive(light: 0x3258B8, dark: 0x91ADFF)
  static let success = adaptive(light: 0x24734E, dark: 0x7AC69B)
  static let warning = adaptive(light: 0x9E590B, dark: 0xF2BA62)
  static let danger = adaptive(light: 0xBC2D3A, dark: 0xFF7B86)

  static let progressGradient = LinearGradient(
    colors: [indigo, cyan], startPoint: .leading, endPoint: .trailing
  )
  static let contentPadding: CGFloat = 32
  static let sectionSpacing: CGFloat = 28
  static let controlRadius: CGFloat = 8
  static let panelRadius: CGFloat = 10
  static let contentMaxWidth: CGFloat = 1_040

  private static func adaptive(light: UInt32, dark: UInt32) -> Color {
    Color(
      nsColor: NSColor(name: nil) { appearance in
        let value = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua ? dark : light
        return NSColor(
          srgbRed: Double((value >> 16) & 0xFF) / 255,
          green: Double((value >> 8) & 0xFF) / 255,
          blue: Double(value & 0xFF) / 255,
          alpha: 1
        )
      })
  }
}

extension Font {
  static func gnfsMonospaced(size: CGFloat, weight: Weight = .regular) -> Font {
    .system(size: size, weight: weight, design: .monospaced)
  }
}
