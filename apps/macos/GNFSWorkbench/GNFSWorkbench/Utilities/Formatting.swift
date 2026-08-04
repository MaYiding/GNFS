import Foundation

enum DisplayFormat {
  static func duration(_ seconds: Double) -> String {
    guard seconds.isFinite, seconds >= 0 else { return "—" }
    if seconds < 0.001 { return "< 1 ms" }
    if seconds < 1 { return String(format: "%.0f ms", seconds * 1_000) }
    if seconds < 60 { return String(format: "%.2f s", seconds) }
    let minutes = Int(seconds) / 60
    if minutes < 60 {
      return String(format: "%d m %.0f s", minutes, seconds.truncatingRemainder(dividingBy: 60))
    }
    let hours = minutes / 60
    return String(format: "%d h %d m", hours, minutes % 60)
  }

  static func count(_ value: UInt64) -> String {
    let formatter = NumberFormatter()
    formatter.numberStyle = .decimal
    formatter.maximumFractionDigits = 0
    return formatter.string(from: NSNumber(value: value)) ?? String(value)
  }

  static func rate(_ value: Double) -> String {
    guard value.isFinite, value > 0 else { return "—" }
    if value >= 1_000_000 { return String(format: "%.2f M/s", value / 1_000_000) }
    if value >= 1_000 { return String(format: "%.1f k/s", value / 1_000) }
    return String(format: "%.0f/s", value)
  }
}
