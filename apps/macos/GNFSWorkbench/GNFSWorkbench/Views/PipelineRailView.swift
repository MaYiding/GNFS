import SwiftUI

struct PipelineRailView: View {
  let run: RunRecord?
  @Environment(\.workbenchReduceMotion) private var reduceMotion

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack {
        Label(
          run?.usesGNFSPipeline == true ? "GNFS 流水线" : "算法路线",
          systemImage: "point.3.connected.trianglepath.dotted"
        )
        .font(.callout.weight(.semibold))
        Spacer()
        if let run {
          Text(
            (run.result?.executionLabel
              ?? AlgorithmDisplay.name(run.activeAlgorithm, fallback: "准备计算"))
              + (run.status == .cancelling ? " · 正在停止" : "")
          )
          .font(.caption.weight(.medium)).foregroundStyle(AppTheme.indigo)
        }
      }
      if run?.usesGNFSPipeline == true {
        ViewThatFits(in: .horizontal) {
          HStack(spacing: 4) {
            ForEach(PipelinePhase.allCases) { phaseNode($0) }
          }
          .frame(minWidth: 800)

          LazyVGrid(
            columns: Array(repeating: GridItem(.flexible(), spacing: 6), count: 4),
            alignment: .leading, spacing: 6
          ) {
            ForEach(PipelinePhase.allCases) { phaseNode($0) }
          }
        }
      } else {
        Text(routeDescription)
          .font(.callout).foregroundStyle(.secondary)
          .fixedSize(horizontal: false, vertical: true)
          .accessibilityIdentifier("algorithmRoute")
      }
    }
    .accessibilityElement(children: .contain)
  }

  private var routeDescription: String {
    guard let run else {
      return "自动模式按整数规模选择试除、Pollard Rho、SIQS 或 GNFS。运行后显示实际使用的算法。"
    }
    if run.status == .succeeded {
      if let count = run.result?.attempts?.count, count > 1 {
        return "已完成 \(count) 次拆分与最终校验。各次的算法、生效参数和统计可在下方分别查看。"
      }
      if run.result?.statistics.gnfsParametersApplied == false {
        return "已完成因子搜索与结果校验。本次没有进入 GNFS 流水线。"
      }
      return "因子搜索与结果校验已完成。"
    }
    return "请求路线：\(run.configuration.method.displayName)。当前实际执行：\(run.activityTitle)。"
  }

  private func phaseNode(_ phase: PipelinePhase) -> some View {
    let state = state(for: phase)
    return HStack(spacing: 7) {
      PhaseIndicator(number: phase.order + 1, state: state)
      Text(phase.displayName)
        .font(.system(size: 11, weight: .medium))
        .foregroundStyle(state == .active ? AppTheme.indigo : AppTheme.secondaryText)
        .lineLimit(1)
    }
    .frame(maxWidth: .infinity, alignment: .leading)
    .padding(.horizontal, 7)
    .frame(height: 32)
    .background {
      RoundedRectangle(cornerRadius: 6, style: .continuous)
        .fill(AppTheme.indigo.opacity(state == .active ? 0.08 : 0))
        .animation(reduceMotion ? nil : WorkbenchMotion.feedback, value: state)
    }
    .accessibilityElement(children: .ignore)
    .accessibilityLabel("\(phase.displayName)：\(state.accessibilityLabel)")
    .accessibilityIdentifier("pipelinePhase-\(phase.order)")
  }

  private func state(for phase: PipelinePhase) -> PhaseVisualState {
    guard let run else { return .inactive }
    if run.status == .succeeded { return .complete }
    if phase.order < run.phase.order { return .complete }
    if phase == run.phase {
      switch run.status {
      case .failed: return .failed
      case .cancelled: return .cancelled
      case .cancelling: return .stopping
      default: return .active
      }
    }
    return .inactive
  }
}

private struct PhaseIndicator: View {
  let number: Int
  let state: PhaseVisualState
  @Environment(\.workbenchReduceMotion) private var reduceMotion

  var body: some View {
    ZStack {
      Circle().fill(state.background)
      Circle().stroke(state.stroke, lineWidth: state == .active ? 1.5 : 1)
      if let symbol = state.symbol {
        Image(systemName: symbol)
          .font(.system(size: 8, weight: .semibold))
          .transition(.opacity)
      } else {
        Text("\(number)")
          .font(.gnfsMonospaced(size: 9, weight: .medium))
          .transition(.opacity)
      }
    }
    .foregroundStyle(state.foreground)
    .frame(width: 17, height: 17)
    .animation(reduceMotion ? nil : WorkbenchMotion.feedback, value: state)
    .accessibilityHidden(true)
  }
}

private enum PhaseVisualState: Equatable {
  case inactive
  case active
  case complete
  case failed
  case cancelled
  case stopping

  var symbol: String? {
    switch self {
    case .complete: "checkmark"
    case .failed: "xmark"
    case .cancelled, .stopping: "stop.fill"
    case .active, .inactive: nil
    }
  }

  var foreground: Color {
    switch self {
    case .active: AppTheme.indigo
    case .complete: AppTheme.onAccent
    case .failed: AppTheme.danger
    case .cancelled, .stopping: AppTheme.secondaryText
    case .inactive: AppTheme.tertiaryText
    }
  }

  var background: Color {
    switch self {
    case .complete: AppTheme.indigo
    case .active: AppTheme.indigo.opacity(0.12)
    case .failed: AppTheme.danger.opacity(0.12)
    case .cancelled, .stopping: AppTheme.secondaryText.opacity(0.10)
    case .inactive: AppTheme.surface
    }
  }

  var stroke: Color {
    switch self {
    case .complete: AppTheme.indigo
    case .active: AppTheme.indigo
    case .failed: AppTheme.danger
    case .cancelled, .stopping: AppTheme.secondaryText
    case .inactive: AppTheme.separatorStrong
    }
  }

  var accessibilityLabel: String {
    switch self {
    case .inactive: "未开始"
    case .active: "进行中"
    case .complete: "已完成"
    case .failed: "失败"
    case .cancelled: "已取消"
    case .stopping: "正在停止"
    }
  }
}
