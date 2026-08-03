import AppKit
import SwiftUI

struct WorkbenchHeaderView: View {
  @Bindable var model: AppModel

  var body: some View {
    GeometryReader { geometry in
      HStack(spacing: 0) {
        Color.clear
          .frame(width: geometry.size.width >= 1_080 ? 124 : 82)
          .accessibilityHidden(true)

        brand

        headerDivider
          .padding(.horizontal, geometry.size.width >= 1_180 ? 18 : 12)

        historyButton

        Spacer(minLength: 18)

        runSummary(compact: geometry.size.width < 1_180)

        Spacer(minLength: 18)

        parameterButton(compact: geometry.size.width < 1_040)

        overflowMenu
          .padding(.leading, 12)
          .padding(.trailing, 22)
      }
    }
    .frame(height: AppTheme.headerHeight)
    .background(AppTheme.header.opacity(0.98))
    .overlay(alignment: .bottom) {
      Rectangle()
        .fill(AppTheme.separatorStrong)
        .frame(height: 1)
    }
    .accessibilityElement(children: .contain)
  }

  private var brand: some View {
    HStack(spacing: 11) {
      Image(nsImage: NSApplication.shared.applicationIconImage)
        .resizable()
        .interpolation(.high)
        .frame(width: 36, height: 36)
        .accessibilityHidden(true)

      VStack(alignment: .leading, spacing: 2) {
        Text("Factor Canvas")
          .font(.system(size: 16, weight: .semibold))
          .foregroundStyle(AppTheme.primaryText)
        Text("GNFS 工作台")
          .font(.system(size: 11, weight: .medium))
          .foregroundStyle(AppTheme.secondaryText)
      }
    }
    .frame(width: 226, alignment: .leading)
  }

  private var headerDivider: some View {
    Rectangle()
      .fill(AppTheme.separator)
      .frame(width: 1, height: 44)
  }

  private var historyButton: some View {
    Button {
      model.isHistoryPresented.toggle()
    } label: {
      HStack(spacing: 8) {
        Image(systemName: "clock.arrow.circlepath")
          .font(.system(size: 15, weight: .medium))
        Text("历史记录")
          .font(.system(size: 13, weight: .medium))
        Image(systemName: "chevron.down")
          .font(.system(size: 9, weight: .bold))
          .foregroundStyle(AppTheme.tertiaryText)
      }
      .frame(height: 34)
      .padding(.horizontal, 11)
    }
    .buttonStyle(WorkbenchHeaderButtonStyle())
    .accessibilityIdentifier("historyButton")
    .popover(isPresented: $model.isHistoryPresented, arrowEdge: .top) {
      HistoryView(model: model)
    }
  }

  private func runSummary(compact: Bool) -> some View {
    HStack(spacing: 13) {
      VStack(alignment: .leading, spacing: 3) {
        HStack(spacing: 8) {
          Circle()
            .fill(summaryColor)
            .frame(width: 8, height: 8)
          Text(summaryTitle(compact: compact))
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(AppTheme.primaryText)
            .lineLimit(1)
        }

        Text(summarySubtitle)
          .font(.gnfsMonospaced(size: 10.5, weight: .medium))
          .foregroundStyle(AppTheme.secondaryText)
          .lineLimit(1)
      }

      headerPrimaryButton
    }
    .padding(.leading, 13)
    .padding(.trailing, 10)
    .frame(width: compact ? 330 : 410, height: 52)
    .background(
      AppTheme.surface.opacity(0.86),
      in: RoundedRectangle(cornerRadius: 9, style: .continuous)
    )
    .overlay {
      RoundedRectangle(cornerRadius: 9, style: .continuous)
        .stroke(AppTheme.separatorStrong, lineWidth: 1)
    }
  }

  private var headerPrimaryButton: some View {
    Button(action: primaryAction) {
      HStack(spacing: 7) {
        Image(systemName: primaryIcon)
          .font(.system(size: 10, weight: .bold))
        Text(primaryLabel)
          .font(.system(size: 12, weight: .semibold))
      }
      .foregroundStyle(.white)
      .padding(.horizontal, 13)
      .frame(height: 32)
      .background(
        model.isRunning
          ? AnyShapeStyle(AppTheme.danger)
          : AnyShapeStyle(AppTheme.progressGradient),
        in: RoundedRectangle(cornerRadius: 7, style: .continuous)
      )
    }
    .buttonStyle(.plain)
    .disabled(primaryActionDisabled)
    .opacity(primaryActionDisabled ? 0.48 : 1)
    .accessibilityIdentifier(model.isRunning ? "cancelRunButton" : "startRunButton")
  }

  private func parameterButton(compact: Bool) -> some View {
    Button {
      model.isParametersPresented.toggle()
    } label: {
      HStack(spacing: 8) {
        Image(systemName: "slider.horizontal.3")
        if !compact {
          Text("参数")
            .font(.system(size: 13, weight: .medium))
        }
      }
      .frame(height: 34)
      .padding(.horizontal, compact ? 10 : 14)
    }
    .buttonStyle(WorkbenchHeaderButtonStyle())
    .accessibilityLabel("运行参数")
    .accessibilityIdentifier("parametersButton")
  }

  private var overflowMenu: some View {
    Menu {
      Button("新建分解", systemImage: "plus") {
        model.newRun()
      }
      .disabled(model.isRunTaskActive)

      if let run = model.displayedRun {
        Button("复制整数", systemImage: "doc.on.doc") {
          model.copy(run.configuration.number)
        }
        if let result = run.result, result.isVerified {
          Button("复制完整质因数式", systemImage: "function") {
            model.copy(result.factorExpression)
          }
        }
        Divider()
        Button("清除当前日志", systemImage: "text.badge.minus") {
          model.clearDisplayedLogs()
        }
        .disabled(run.logs.isEmpty)
      }
    } label: {
      Image(systemName: "ellipsis")
        .font(.system(size: 14, weight: .semibold))
        .frame(width: 34, height: 34)
    }
    .menuStyle(.borderlessButton)
    .menuIndicator(.hidden)
    .fixedSize()
    .background(
      AppTheme.surface.opacity(0.76),
      in: Circle()
    )
    .overlay {
      Circle().stroke(AppTheme.separatorStrong, lineWidth: 1)
    }
    .padding(.trailing, 66)
    .help("更多操作")
    .accessibilityLabel("更多操作")
  }

  private var summaryColor: Color {
    switch model.displayedRun?.status {
    case .running: AppTheme.indigo
    case .cancelling: AppTheme.warning
    case .succeeded: AppTheme.success
    case .failed: AppTheme.danger
    case .cancelled: AppTheme.secondaryText
    case .ready, .none: AppTheme.cyan
    }
  }

  private func summaryTitle(compact: Bool) -> String {
    guard let run = model.displayedRun else {
      return compact ? "完整质因数分解" : "准备进行完整质因数分解"
    }
    switch run.status {
    case .running:
      return compact ? "正在运行" : "正在运行 · \(run.displayNumber)"
    case .cancelling:
      return "正在安全取消"
    case .succeeded:
      return compact ? "分解完成" : "完整质因数分解完成"
    case .failed:
      return "分解未完成"
    case .cancelled:
      return "任务已取消"
    case .ready:
      return "准备开始"
    }
  }

  private var summarySubtitle: String {
    guard let run = model.displayedRun else {
      return "自动选择算法 · 逐个验证全部质因数"
    }
    switch run.status {
    case .running:
      return "已用时 \(DisplayFormat.duration(run.elapsed)) · \(run.phase.displayName)"
    case .cancelling:
      return "正在终止运算引擎并保存历史"
    case .succeeded:
      return "\(run.result?.factors.count ?? 0) 个质因数 · \(DisplayFormat.duration(run.elapsed))"
    case .failed:
      return run.errorMessage ?? "请检查日志"
    case .cancelled:
      return "历史已保存 · 临时工作目录已清理"
    case .ready:
      return "等待输入"
    }
  }

  private var primaryLabel: String {
    if model.activeRun?.status == .cancelling { return "取消中…" }
    if model.isRunning { return "停止" }
    if model.displayedRun != nil { return "再次分解" }
    return "开始"
  }

  private var primaryIcon: String {
    if model.isRunning { return "stop.fill" }
    if model.displayedRun != nil { return "arrow.clockwise" }
    return "play.fill"
  }

  private var primaryActionDisabled: Bool {
    if model.activeRun?.status == .cancelling { return true }
    if model.isRunTaskActive && !model.isRunning { return true }
    if model.isRunning || model.displayedRun != nil { return false }
    return !model.canStart
  }

  private func primaryAction() {
    if model.isRunning {
      model.cancelRun()
    } else if model.displayedRun != nil {
      model.useConfigurationFromDisplayedRun()
      Task { await model.startRun() }
    } else {
      Task { await model.startRun() }
    }
  }
}

private struct WorkbenchHeaderButtonStyle: ButtonStyle {
  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .foregroundStyle(
        configuration.isPressed ? AppTheme.primaryText : AppTheme.secondaryText
      )
      .background(
        configuration.isPressed
          ? AppTheme.surfaceHover
          : AppTheme.surface.opacity(0.72),
        in: RoundedRectangle(cornerRadius: 8, style: .continuous)
      )
      .overlay {
        RoundedRectangle(cornerRadius: 8, style: .continuous)
          .stroke(AppTheme.separatorStrong, lineWidth: 1)
      }
  }
}
