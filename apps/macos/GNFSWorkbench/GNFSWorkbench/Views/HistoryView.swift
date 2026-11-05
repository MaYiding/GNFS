import SwiftUI

struct HistoryView: View {
  @Bindable var model: AppModel
  var embedded = false
  @State private var isConfirmingClear = false
  @State private var query = ""
  @FocusState private var focusedRunID: UUID?

  private var matchingRuns: [RunRecord] {
    guard !query.isEmpty else { return model.history }
    return model.history.filter {
      $0.configuration.number.localizedCaseInsensitiveContains(query)
        || ($0.result?.factorExpression.localizedCaseInsensitiveContains(query) ?? false)
        || HistoryRowContent.statusLabel($0).localizedCaseInsensitiveContains(query)
        || ($0.result?.executionLabel.localizedCaseInsensitiveContains(query) ?? false)
        || ($0.selectedMethod ?? $0.configuration.method).displayName
          .localizedCaseInsensitiveContains(query)
    }
  }

  var body: some View {
    let runs = matchingRuns
    VStack(alignment: .leading, spacing: 0) {
      HStack {
        Text("运行历史").font(.system(size: 12, weight: .semibold))
        Spacer()
        Text("\(model.history.count)")
          .font(.system(size: 11).monospacedDigit()).foregroundStyle(.tertiary)
          .help("最多保留 50 条记录")
      }
      .padding(.horizontal, 16).padding(.top, 20).padding(.bottom, 14)
      WorkbenchSearchField(
        title: "搜索记录", text: $query,
        accessibilityLabel: "搜索运行历史", identifier: "historySearch"
      )
      .help("搜索整数、因子、算法或状态")
      .onSubmit { _ = selectBoundary(last: false, in: runs) }
      .onKeyPress(.downArrow) { selectBoundary(last: false, in: runs) }
      .padding(.horizontal, 12).padding(.bottom, 10)
      if model.history.isEmpty {
        VStack(alignment: .leading, spacing: 7) {
          Text("暂无记录").font(.system(size: 12, weight: .medium))
          Text("运行结果会自动保存在本机。")
            .font(.system(size: 11)).foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
          Spacer()
        }
        .padding(.horizontal, 16).padding(.top, 20)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
      } else if runs.isEmpty {
        VStack(alignment: .leading, spacing: 8) {
          Text("没有匹配的记录").font(.system(size: 12)).foregroundStyle(.secondary)
          Button("清除搜索") { query = "" }.controlSize(.small)
          Spacer()
        }
        .padding(.horizontal, 16).padding(.top, 20)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
      } else {
        ScrollViewReader { proxy in
          ScrollView {
            LazyVStack(spacing: 2) {
              ForEach(runs) { run in
                Button {
                  select(run.id)
                } label: {
                  HistoryRowContent(run: run).equatable()
                }
                .buttonStyle(
                  HistorySelectionStyle(
                    selected: model.displayedRun?.id == run.id,
                    focused: focusedRunID == run.id)
                )
                .focusable()
                .focused($focusedRunID, equals: run.id)
                .focusEffectDisabled()
                .onKeyPress(.upArrow) { moveSelection(from: run.id, offset: -1, in: runs) }
                .onKeyPress(.downArrow) { moveSelection(from: run.id, offset: 1, in: runs) }
                .onKeyPress(.home) { selectBoundary(last: false, in: runs) }
                .onKeyPress(.end) { selectBoundary(last: true, in: runs) }
                .accessibilityAddTraits(model.displayedRun?.id == run.id ? .isSelected : [])
                .accessibilityIdentifier("history-\(run.configuration.number)")
                .id(run.id)
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
            .padding(.horizontal, 8).padding(.bottom, 8)
          }
          .accessibilityElement(children: .contain)
          .accessibilityIdentifier("historyList")
          .onAppear { reveal(model.displayedRun?.id, in: runs, using: proxy) }
          .onChange(of: model.displayedRun?.id) { _, id in
            reveal(id, in: runs, using: proxy)
          }
          .onChange(of: focusedRunID) { _, id in reveal(id, in: runs, using: proxy) }
          .onChange(of: query) {
            focusedRunID = nil
            reveal(runs.first?.id, in: runs, using: proxy)
          }
        }
      }
      Divider()
      HStack {
        Text("本机记录").font(.system(size: 10))
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
      .foregroundStyle(.tertiary)
      .padding(.horizontal, 16).padding(.vertical, 12)
    }
    .frame(width: embedded ? nil : 390, height: embedded ? nil : 470)
    .background(AppTheme.header)
    .confirmationDialog("清除所有运行历史？", isPresented: $isConfirmingClear, titleVisibility: .visible) {
      Button("清除历史", role: .destructive) { Task { await model.clearHistory() } }
      Button("取消", role: .cancel) {}
    } message: {
      Text("历史记录及其临时工作目录将从这台 Mac 上删除。此操作无法撤销。")
    }
  }

  private func select(_ id: UUID) {
    focusedRunID = id
    model.selectHistory(id)
  }

  private func moveSelection(from id: UUID, offset: Int, in runs: [RunRecord]) -> KeyPress.Result {
    guard let index = runs.firstIndex(where: { $0.id == id }) else { return .ignored }
    select(runs[min(max(index + offset, 0), runs.count - 1)].id)
    return .handled
  }

  private func selectBoundary(last: Bool, in runs: [RunRecord]) -> KeyPress.Result {
    guard let run = last ? runs.last : runs.first else { return .ignored }
    select(run.id)
    return .handled
  }

  private func reveal(_ id: UUID?, in runs: [RunRecord], using proxy: ScrollViewProxy) {
    guard let id, runs.contains(where: { $0.id == id }) else { return }
    // No anchor means the smallest scroll needed; selecting a visible row never recenters the list.
    proxy.scrollTo(id)
  }
}

private struct HistoryRowContent: View, Equatable {
  let run: RunRecord

  var body: some View {
    let verified = run.hasVerifiedResult
    let needsVerification = run.status == .succeeded && !verified
    return VStack(alignment: .leading, spacing: 6) {
      HStack(spacing: 5) {
        Circle()
          .fill(needsVerification ? AppTheme.warning : Self.statusColor(run.status))
          .frame(width: 5, height: 5)
          .accessibilityHidden(true)
        Text(Self.statusLabel(run))
          .foregroundStyle(needsVerification ? AppTheme.warning : AppTheme.secondaryText)
        Spacer(minLength: 0)
        Text(run.createdAt.formatted(.dateTime.month(.twoDigits).day(.twoDigits).hour().minute()))
          .foregroundStyle(.tertiary)
          .lineLimit(1)
          .help(run.createdAt.formatted(date: .complete, time: .shortened))
      }
      .font(.system(size: 10))
      Text(run.displayNumber)
        .font(.system(size: 12, weight: .medium, design: .monospaced))
        .foregroundStyle(AppTheme.primaryText)
        .lineLimit(2, reservesSpace: true).multilineTextAlignment(.leading)
        .frame(maxWidth: .infinity, alignment: .leading)
      HStack(alignment: .firstTextBaseline, spacing: 8) {
        Text(
          verified
            ? run.result!.factorExpression
            : needsVerification
              ? "因子尚未验证"
              : AlgorithmDisplay.name(
                run.activeAlgorithm, fallback: run.status.isTerminal ? "未记录算法" : "等待引擎")
        )
        .lineLimit(1).truncationMode(.tail)
        Spacer(minLength: 0)
        Group {
          if run.status == .running || run.status == .cancelling {
            TimelineView(.periodic(from: .now, by: 1)) { context in
              Text(DisplayFormat.duration(run.elapsed(at: context.date)))
            }
          } else {
            Text(DisplayFormat.duration(run.elapsed))
          }
        }
        .monospacedDigit().lineLimit(1)
        .frame(width: 64, alignment: .trailing)
        .contentTransition(.identity)
      }
      .font(.system(size: 10)).foregroundStyle(.secondary)
    }
    .padding(.horizontal, 10).padding(.vertical, 10)
    .frame(maxWidth: .infinity, alignment: .leading)
    .contentShape(Rectangle())
    .transaction { $0.animation = nil }
  }

  private static func statusColor(_ status: RunStatus) -> Color {
    switch status {
    case .running: AppTheme.indigo
    case .cancelling: AppTheme.warning
    case .succeeded: AppTheme.secondaryText
    case .failed: AppTheme.danger
    case .cancelled, .ready: AppTheme.secondaryText
    }
  }
  static func statusLabel(_ run: RunRecord) -> String {
    if run.needsResultVerification { return "结果未验证" }
    return switch run.status {
    case .ready: "就绪"
    case .running: "运行中"
    case .cancelling: "正在停止"
    case .succeeded: "已完成"
    case .failed: "未完成"
    case .cancelled: "已取消"
    }
  }
}

private struct HistorySelectionStyle: ButtonStyle {
  let selected: Bool
  let focused: Bool

  func makeBody(configuration: Configuration) -> some View {
    SelectionBody(configuration: configuration, selected: selected, focused: focused)
  }

  private struct SelectionBody: View {
    let configuration: ButtonStyleConfiguration
    let selected: Bool
    let focused: Bool
    @Environment(\.workbenchReduceMotion) private var reduceMotion
    @State private var hovered = false

    var body: some View {
      configuration.label
        .background {
          RoundedRectangle(cornerRadius: 6)
            .fill(
              selected
                ? AppTheme.indigo.opacity(
                  configuration.isPressed ? 0.20 : hovered ? 0.16 : 0.12)
                : Color.primary.opacity(
                  configuration.isPressed ? 0.10 : hovered ? 0.045 : 0)
            )
            .animation(reduceMotion ? nil : WorkbenchMotion.feedback, value: hovered)
            .animation(
              reduceMotion ? nil : WorkbenchMotion.feedback, value: configuration.isPressed
            )
            .animation(reduceMotion ? nil : WorkbenchMotion.feedback, value: selected)
        }
        .overlay {
          RoundedRectangle(cornerRadius: 6)
            .strokeBorder(focused ? AppTheme.indigo.opacity(0.65) : .clear, lineWidth: 1.5)
        }
        .contentShape(RoundedRectangle(cornerRadius: 6))
        .onHover { hovered = $0 }
    }
  }
}
