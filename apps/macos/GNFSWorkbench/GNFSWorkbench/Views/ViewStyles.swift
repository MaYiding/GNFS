import SwiftUI

private struct ReducedMotionOverrideKey: EnvironmentKey {
  static let defaultValue = false
}

extension EnvironmentValues {
  /// Callers may reduce motion further, but can never override the system preference off.
  var workbenchReduceMotion: Bool {
    get { accessibilityReduceMotion || self[ReducedMotionOverrideKey.self] }
    set { self[ReducedMotionOverrideKey.self] = newValue }
  }
}

/// Motion is scoped to the control that changed. Telemetry is never interpolated.
enum WorkbenchMotion {
  static let feedback = Animation.easeOut(duration: 0.12)
  static let disclosure = Animation.smooth(duration: 0.24, extraBounce: 0)
  static let page = Animation.easeInOut(duration: 0.18)
}

struct WorkbenchActionStyle: ButtonStyle {
  enum Emphasis { case quiet, secondary, primary }
  var emphasis: Emphasis

  init(_ emphasis: Emphasis = .secondary) { self.emphasis = emphasis }

  func makeBody(configuration: Configuration) -> some View {
    ActionBody(configuration: configuration, emphasis: emphasis)
  }

  private struct ActionBody: View {
    let configuration: ButtonStyleConfiguration
    let emphasis: Emphasis
    @Environment(\.isEnabled) private var isEnabled
    @Environment(\.workbenchReduceMotion) private var reduceMotion
    @Environment(\.colorSchemeContrast) private var contrast
    @State private var isHovered = false

    private var background: Color {
      guard isEnabled else { return AppTheme.surface.opacity(0.5) }
      if emphasis == .primary {
        return AppTheme.indigo.opacity(configuration.isPressed ? 0.75 : (isHovered ? 0.9 : 1))
      }
      if configuration.isPressed { return Color.primary.opacity(0.12) }
      if isHovered { return Color.primary.opacity(0.07) }
      return emphasis == .quiet ? .clear : AppTheme.surface
    }

    var body: some View {
      configuration.label
        .font(.system(size: 12, weight: .medium))
        .padding(.horizontal, 10).padding(.vertical, 6)
        .foregroundStyle(
          isEnabled ? (emphasis == .primary ? AppTheme.onAccent : AppTheme.primaryText) : .secondary
        )
        .background(background, in: RoundedRectangle(cornerRadius: 7, style: .continuous))
        .overlay {
          RoundedRectangle(cornerRadius: 7, style: .continuous)
            .strokeBorder(contrast == .increased ? AppTheme.separatorStrong : .clear, lineWidth: 1)
        }
        .contentShape(RoundedRectangle(cornerRadius: 7, style: .continuous))
        .onHover { isHovered = $0 }
        .animation(reduceMotion ? nil : WorkbenchMotion.feedback, value: isHovered)
        .animation(reduceMotion ? nil : WorkbenchMotion.feedback, value: configuration.isPressed)
    }
  }
}

struct WorkbenchSearchField: View {
  let title: String
  @Binding var text: String
  let accessibilityLabel: String
  let identifier: String
  @FocusState private var isFocused: Bool
  @Environment(\.workbenchReduceMotion) private var reduceMotion

  var body: some View {
    HStack(spacing: 7) {
      Image(systemName: "magnifyingglass")
        .foregroundStyle(.secondary).accessibilityHidden(true)
      TextField(title, text: $text)
        .textFieldStyle(.plain)
        .focused($isFocused)
        .accessibilityLabel(accessibilityLabel)
        .accessibilityIdentifier(identifier)
      Button {
        text = ""
        isFocused = true
      } label: {
        Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary)
      }
      .buttonStyle(.plain)
      .opacity(text.isEmpty ? 0 : 1)
      .disabled(text.isEmpty)
      .accessibilityHidden(text.isEmpty)
      .accessibilityLabel("清除搜索")
      .help("清除搜索")
    }
    .font(.system(size: 12))
    .padding(.horizontal, 9).frame(height: 30)
    .background(AppTheme.inputSurface, in: RoundedRectangle(cornerRadius: 7))
    .overlay {
      RoundedRectangle(cornerRadius: 7)
        .strokeBorder(
          isFocused ? AppTheme.indigo.opacity(0.65) : AppTheme.separatorStrong,
          lineWidth: isFocused ? 2 : 1)
    }
    .animation(reduceMotion ? nil : WorkbenchMotion.feedback, value: isFocused)
  }
}

struct WorkbenchPanel: ViewModifier {
  var padding: CGFloat = 22

  func body(content: Content) -> some View {
    content
      .padding(padding)
      .background(
        AppTheme.surface.opacity(0.72),
        in: RoundedRectangle(
          cornerRadius: AppTheme.panelRadius,
          style: .continuous
        )
      )
      .overlay {
        RoundedRectangle(cornerRadius: AppTheme.panelRadius, style: .continuous)
          .stroke(AppTheme.separator, lineWidth: 1)
      }
  }
}

extension View {
  func workbenchPanel(padding: CGFloat = 22) -> some View {
    modifier(WorkbenchPanel(padding: padding))
  }
}
