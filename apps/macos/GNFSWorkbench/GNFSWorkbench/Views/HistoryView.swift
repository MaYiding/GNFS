import SwiftUI

struct HistoryView: View {
  @Bindable var model: AppModel
  @State private var isConfirmingClear = false

  var body: some View {
    VStack(spacing: 0) {
      HStack {
        VStack(alignment: .leading, spacing: 2) {
          Text("运行历史")
            .font(.system(size: 16, weight: .semibold))
          Text("保存在这台 Mac 上")
            .font(.caption)
            .foregroundStyle(AppTheme.secondaryText)
        }
        Spacer()
        Button {
          isConfirmingClear = true
        } label: {
          Image(systemName: "trash")
        }
        .buttonStyle(.borderless)
        .disabled(model.history.isEmpty || model.isRunTaskActive)
        .help("清除历史")
      }
      .padding(16)

      Divider().overlay(AppTheme.separator)

      if model.history.isEmpty {
        ContentUnavailableView(
          "暂无运行记录",
          systemImage: "clock",
          description: Text("完成、失败或取消的任务会出现在这里。")
        )
        .frame(maxWidth: .infinity, maxHeight: .infinity)
      } else {
        ScrollView {
          LazyVStack(spacing: 3) {
            ForEach(Array(model.history.enumerated()), id: \.element.id) { index, run in
              Button {
                model.selectHistory(run.id)
              } label: {
                historyRow(run)
              }
              .buttonStyle(.plain)
              .disabled(model.isRunTaskActive && run.id != model.activeRun?.id)
              .contextMenu {
                Button("删除", role: .destructive) {
                  model.removeHistory(IndexSet(integer: index))
                }
                .disabled(model.isRunTaskActive)
              }
            }
          }
          .padding(8)
        }
      }
    }
    .frame(width: 390, height: 470)
    .background(AppTheme.elevated)
    .confirmationDialog(
      "清除所有运行历史？",
      isPresented: $isConfirmingClear,
      titleVisibility: .visible
    ) {
      Button("清除历史", role: .destructive) { model.clearHistory() }
      Button("取消", role: .cancel) {}
    } message: {
      Text("历史记录及其临时工作目录将从这台 Mac 上删除。")
    }
  }

  private func historyRow(_ run: RunRecord) -> some View {
    HStack(spacing: 12) {
      Circle()
        .fill(statusColor(run.status))
        .frame(width: 7, height: 7)

      VStack(alignment: .leading, spacing: 5) {
        Text(run.displayNumber)
          .font(.gnfsMonospaced(size: 12, weight: .medium))
          .foregroundStyle(AppTheme.primaryText)
          .lineLimit(1)
        HStack(spacing: 7) {
          if let result = run.result, result.isVerified {
            Text(result.factorExpression)
              .lineLimit(1)
          } else {
            Text(run.selectedMethod?.displayName ?? run.configuration.method.displayName)
          }
          Text("·")
          Text(DisplayFormat.duration(run.elapsed))
        }
        .font(.caption)
        .foregroundStyle(AppTheme.secondaryText)
      }

      Spacer()

      VStack(alignment: .trailing, spacing: 5) {
        Text(statusLabel(run.status))
          .font(.caption.weight(.medium))
          .foregroundStyle(statusColor(run.status))
        Text(run.createdAt.formatted(date: .abbreviated, time: .shortened))
          .font(.system(size: 9))
          .foregroundStyle(AppTheme.tertiaryText)
      }
    }
    .padding(.horizontal, 10)
    .padding(.vertical, 9)
    .background(
      model.selectedRunID == run.id ? AppTheme.indigo.opacity(0.12) : Color.clear,
      in: RoundedRectangle(cornerRadius: 7, style: .continuous)
    )
    .contentShape(Rectangle())
  }

  private func statusColor(_ status: RunStatus) -> Color {
    switch status {
    case .running: AppTheme.cyan
    case .cancelling: AppTheme.warning
    case .succeeded: AppTheme.success
    case .failed: AppTheme.danger
    case .cancelled: AppTheme.secondaryText
    case .ready: AppTheme.indigo
    }
  }

  private func statusLabel(_ status: RunStatus) -> String {
    switch status {
    case .ready: "就绪"
    case .running: "运行中"
    case .cancelling: "正在取消"
    case .succeeded: "成功"
    case .failed: "失败"
    case .cancelled: "已取消"
    }
  }
}
