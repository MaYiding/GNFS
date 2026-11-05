import SwiftUI

struct HistoryView: View {
  @Bindable var model: AppModel
  var embedded = false
  @State private var isConfirmingClear = false
  @State private var query = ""

  private var matchingRuns: [RunRecord] {
    guard !query.isEmpty else { return model.history }
    return model.history.filter {
      $0.configuration.number.localizedCaseInsensitiveContains(query)
        || ($0.result?.factorExpression.localizedCaseInsensitiveContains(query) ?? false)
        || statusLabel($0.status).localizedCaseInsensitiveContains(query)
        || ($0.result?.executionLabel.localizedCaseInsensitiveContains(query) ?? false)
        || ($0.selectedMethod ?? $0.configuration.method).displayName
          .localizedCaseInsensitiveContains(query)
    }
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      HStack {
        Text("运行历史").font(.headline)
        Spacer()
        Text("\(model.history.count) / 50").font(.caption.monospacedDigit()).foregroundStyle(
          .secondary)
      }
      .padding(.horizontal, 16).padding(.top, 20).padding(.bottom, 12)
      TextField("搜索整数、因子或状态", text: $query)
        .textFieldStyle(.roundedBorder)
        .accessibilityLabel("搜索运行历史")
        .accessibilityIdentifier("historySearch")
        .padding(.horizontal, 12).padding(.bottom, 12)
      if model.history.isEmpty {
        VStack(spacing: 12) {
          Image(systemName: "clock.arrow.circlepath").font(.largeTitle).foregroundStyle(.tertiary)
          Text("还没有运行记录").font(.callout.weight(.medium))
          Text("开始一次分解，结果会自动保存在这台 Mac 上。")
            .font(.caption).foregroundStyle(.secondary).multilineTextAlignment(.center)
        }
        .padding(20).frame(maxWidth: .infinity, maxHeight: .infinity)
      } else if matchingRuns.isEmpty {
        VStack(spacing: 8) {
          Text("没有匹配的记录").font(.callout)
          Button("清除搜索") { query = "" }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
      } else {
        ScrollView {
          LazyVStack(spacing: 5) {
            ForEach(matchingRuns) { run in
              Button {
                model.selectHistory(run.id)
              } label: {
                historyRow(run)
              }
              .buttonStyle(.plain)
              .accessibilityIdentifier("history-\(run.configuration.number)")
              .contextMenu {
                Button("复制整数") { model.copy(run.configuration.number) }
                Button("使用参数新建") {
                  model.selectHistory(run.id)
                  Task { await model.useConfigurationFromDisplayedRun() }
                }
                .disabled(!model.canEdit)
                Divider()
                Button("删除记录", role: .destructive) {
                  guard let index = model.history.firstIndex(where: { $0.id == run.id }) else {
                    return
                  }
                  Task { await model.removeHistory(IndexSet(integer: index)) }
                }
                .disabled(!model.canEdit)
              }
            }
          }
          .padding(.horizontal, 8)
        }
      }
      Divider()
      HStack {
        Label("仅保存在本机", systemImage: "internaldrive").font(.caption)
        Spacer()
        Button {
          isConfirmingClear = true
        } label: {
          Image(systemName: "trash")
        }
        .buttonStyle(.borderless)
        .disabled(model.history.isEmpty || !model.canEdit)
        .help("清除所有历史")
        .accessibilityLabel("清除所有历史")
      }
      .foregroundStyle(.secondary).padding(14)
    }
    .frame(width: embedded ? nil : 390, height: embedded ? nil : 470)
    .background(AppTheme.canvas)
    .confirmationDialog("清除所有运行历史？", isPresented: $isConfirmingClear, titleVisibility: .visible) {
      Button("清除历史", role: .destructive) { Task { await model.clearHistory() } }
      Button("取消", role: .cancel) {}
    } message: {
      Text("历史记录及其临时工作目录将从这台 Mac 上删除。此操作无法撤销。")
    }
  }

  private func historyRow(_ run: RunRecord) -> some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack(spacing: 6) {
        Image(systemName: statusIcon(run.status)).foregroundStyle(statusColor(run.status))
        Text(statusLabel(run.status)).foregroundStyle(statusColor(run.status))
        Spacer(minLength: 0)
        Text(DisplayFormat.duration(run.elapsed)).foregroundStyle(.secondary)
      }
      .font(.caption.weight(.medium))
      Text(run.displayNumber)
        .font(.system(.callout, design: .monospaced).weight(.medium))
        .lineLimit(2).multilineTextAlignment(.leading)
      Text(
        run.result?.isVerified == true
          ? run.result!.factorExpression
          : AlgorithmDisplay.name(run.activeAlgorithm, fallback: "等待引擎")
      )
      .font(.caption).foregroundStyle(.secondary).lineLimit(1)
      Text(run.createdAt.formatted(date: .abbreviated, time: .shortened))
        .font(.caption2).foregroundStyle(.secondary)
    }
    .padding(12)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(
      model.displayedRun?.id == run.id ? AppTheme.indigo.opacity(0.10) : Color.clear,
      in: RoundedRectangle(cornerRadius: 10)
    )
    .overlay {
      RoundedRectangle(cornerRadius: 10)
        .stroke(model.displayedRun?.id == run.id ? AppTheme.indigo.opacity(0.25) : Color.clear)
    }
    .contentShape(Rectangle())
  }

  private func statusColor(_ status: RunStatus) -> Color {
    switch status {
    case .running: AppTheme.indigo
    case .cancelling: AppTheme.warning
    case .succeeded: AppTheme.success
    case .failed: AppTheme.danger
    case .cancelled, .ready: AppTheme.secondaryText
    }
  }
  private func statusLabel(_ status: RunStatus) -> String {
    switch status {
    case .ready: "就绪"
    case .running: "运行中"
    case .cancelling: "正在停止"
    case .succeeded: "已完成"
    case .failed: "未完成"
    case .cancelled: "已取消"
    }
  }
  private func statusIcon(_ status: RunStatus) -> String {
    switch status {
    case .ready: "circle"
    case .running: "waveform.path.ecg"
    case .cancelling, .cancelled: "stop.circle"
    case .succeeded: "checkmark.circle.fill"
    case .failed: "exclamationmark.circle.fill"
    }
  }
}
