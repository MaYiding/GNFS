import SwiftUI

struct PipelineRailView: View {
  let run: RunRecord?
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    VStack(alignment: .leading, spacing: 19) {
      HStack {
        Text("GNFS 流水线")
          .font(.system(size: 13, weight: .semibold))
          .tracking(0.9)
          .foregroundStyle(AppTheme.secondaryText)
        Spacer()
        if let run, run.status == .running {
          Text(run.phase.displayName)
            .font(.system(size: 12, weight: .semibold))
            .foregroundStyle(AppTheme.cyan)
        }
      }

      HStack(spacing: 0) {
        ForEach(Array(PipelinePhase.allCases.enumerated()), id: \.element.id) { index, phase in
          phaseNode(phase)
          if index < PipelinePhase.allCases.count - 1 {
            connector(after: phase)
          }
        }
      }
    }
    .padding(.horizontal, 2)
    .accessibilityElement(children: .contain)
    .accessibilityLabel("GNFS 流水线进度")
  }

  private func phaseNode(_ phase: PipelinePhase) -> some View {
    let state = state(for: phase)
    return VStack(spacing: 8) {
      ZStack {
        Circle()
          .fill(state.background)
          .frame(width: 36, height: 36)
        Circle()
          .stroke(state.stroke, lineWidth: state == .active ? 2 : 1)
          .frame(width: 36, height: 36)
        if state == .complete {
          Image(systemName: "checkmark")
            .font(.system(size: 11, weight: .bold))
            .foregroundStyle(state.foreground)
        } else if state == .failed {
          Image(systemName: "xmark")
            .font(.system(size: 10, weight: .bold))
            .foregroundStyle(state.foreground)
        } else {
          Text("\(phase.order + 1)")
            .font(.gnfsMonospaced(size: 12, weight: .semibold))
            .foregroundStyle(state.foreground)
        }
      }
      .shadow(color: state == .active ? AppTheme.cyan.opacity(0.35) : .clear, radius: 8)

      Text(phase.displayName)
        .font(.system(size: 12.5, weight: state == .active ? .semibold : .medium))
        .foregroundStyle(state == .inactive ? AppTheme.tertiaryText : state.foreground)
        .lineLimit(1)
        .fixedSize(horizontal: true, vertical: false)
    }
    .frame(minWidth: 76)
    .animation(reduceMotion ? nil : .easeInOut(duration: 0.22), value: state)
    .accessibilityElement(children: .ignore)
    .accessibilityLabel("\(phase.displayName)：\(state.accessibilityLabel)")
  }

  private func connector(after phase: PipelinePhase) -> some View {
    let isComplete =
      run.map { record in
        record.status == .succeeded || phase.order < record.phase.order
      } ?? false
    return Rectangle()
      .fill(
        isComplete
          ? AnyShapeStyle(AppTheme.progressGradient) : AnyShapeStyle(AppTheme.separatorStrong)
      )
      .frame(maxWidth: .infinity)
      .frame(height: 2.5)
      .offset(y: -10)
  }

  private func state(for phase: PipelinePhase) -> PhaseVisualState {
    guard let run else { return .inactive }
    if run.status == .succeeded { return .complete }
    if phase.order < run.phase.order { return .complete }
    if phase == run.phase {
      switch run.status {
      case .failed: return .failed
      case .cancelled, .cancelling: return .cancelled
      default: return .active
      }
    }
    return .inactive
  }
}

private enum PhaseVisualState: Equatable {
  case inactive
  case active
  case complete
  case failed
  case cancelled

  var foreground: Color {
    switch self {
    case .active: AppTheme.cyan
    case .complete: .white
    case .failed: AppTheme.danger
    case .cancelled: AppTheme.secondaryText
    case .inactive: AppTheme.tertiaryText
    }
  }

  var background: Color {
    switch self {
    case .complete: AppTheme.indigo
    case .active: AppTheme.cyan.opacity(0.12)
    case .failed: AppTheme.danger.opacity(0.12)
    case .cancelled: AppTheme.secondaryText.opacity(0.10)
    case .inactive: AppTheme.surface
    }
  }

  var stroke: Color {
    switch self {
    case .complete: AppTheme.indigo
    case .active: AppTheme.cyan
    case .failed: AppTheme.danger
    case .cancelled: AppTheme.secondaryText
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
    }
  }
}
