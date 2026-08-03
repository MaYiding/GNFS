import SwiftUI

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
