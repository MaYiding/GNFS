import SwiftUI

struct RunLogsView: View {
  let entries: [RunLogEntry]
  @Binding var isExpanded: Bool
  let canClear: Bool
  let copy: (String) -> Void
  let clear: () -> Void
  @State private var query = ""
  @State private var severityFilter: LogSeverity?
  @State private var followsLatest = true

  private var visibleEntries: [RunLogEntry] {
    entries.filter {
      (severityFilter == nil || $0.severity == severityFilter)
        && (query.isEmpty || $0.message.localizedCaseInsensitiveContains(query)
          || $0.severity.rawValue.localizedCaseInsensitiveContains(query)
          || ($0.phase?.displayName.localizedCaseInsensitiveContains(query) ?? false))
    }
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 14) {
      Button {
        isExpanded.toggle()
      } label: {
        HStack {
          Label("运行日志", systemImage: "terminal").font(.headline)
          Text("\(entries.count)").font(.caption.monospacedDigit()).foregroundStyle(.secondary)
          Spacer()
          Image(systemName: isExpanded ? "chevron.up" : "chevron.down").font(.caption)
        }
        .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .accessibilityIdentifier("toggleLogsButton")
      if isExpanded {
        HStack(spacing: 12) {
          TextField("搜索日志", text: $query)
            .textFieldStyle(.roundedBorder)
            .accessibilityLabel("搜索运行日志")
            .accessibilityIdentifier("logSearch")
          Picker("级别", selection: $severityFilter) {
            Text("全部级别").tag(nil as LogSeverity?)
            ForEach([LogSeverity.info, .warning, .error, .debug, .trace], id: \.rawValue) {
              Text($0.rawValue).tag(Optional($0))
            }
          }
          .labelsHidden().frame(width: 112)
          Menu {
            Button("复制筛选结果") { copy(visibleEntries.map(Self.plainText).joined(separator: "\n")) }
              .disabled(visibleEntries.isEmpty)
            Button("复制全部日志") { copy(entries.map(Self.plainText).joined(separator: "\n")) }
              .disabled(entries.isEmpty)
            Divider()
            Button("清除日志", role: .destructive, action: clear)
              .disabled(entries.isEmpty || !canClear)
          } label: {
            Image(systemName: "ellipsis.circle")
          }
          .menuIndicator(.hidden).menuStyle(.borderlessButton).fixedSize()
          .accessibilityLabel("日志操作")
        }
        HStack {
          Text("显示 \(visibleEntries.count) 条 · 最多保留最近 600 条")
            .font(.caption).foregroundStyle(.secondary)
          Spacer()
          Toggle("跟随最新", isOn: $followsLatest)
            .toggleStyle(.checkbox).font(.caption)
            .accessibilityIdentifier("followLogsToggle")
        }
        if entries.isEmpty {
          emptyState("暂无日志", detail: "开始运行后，事件和诊断信息将显示在这里。")
        } else if visibleEntries.isEmpty {
          VStack(spacing: 10) {
            emptyState("没有匹配的日志", detail: "试试其他关键词或级别。")
            Button("重置筛选") {
              query = ""
              severityFilter = nil
            }
          }
        } else {
          ScrollViewReader { proxy in
            ScrollView {
              LazyVStack(alignment: .leading, spacing: 0) {
                ForEach(visibleEntries) { entry in logRow(entry).id(entry.id) }
              }
              .padding(.vertical, 6)
            }
            .frame(height: 220)
            .background(AppTheme.canvas, in: RoundedRectangle(cornerRadius: 8))
            .onChange(of: visibleEntries.last?.id) {
              if followsLatest, let last = visibleEntries.last {
                proxy.scrollTo(last.id, anchor: .bottom)
              }
            }
            .onChange(of: followsLatest) {
              if followsLatest, let last = visibleEntries.last {
                proxy.scrollTo(last.id, anchor: .bottom)
              }
            }
            .onAppear {
              if followsLatest, let last = visibleEntries.last {
                proxy.scrollTo(last.id, anchor: .bottom)
              }
            }
          }
        }
      }
    }
    .workbenchPanel(padding: 18)
    .accessibilityIdentifier("runLogs")
  }

  private func emptyState(_ title: String, detail: String) -> some View {
    VStack(spacing: 7) {
      Text(title).font(.callout.weight(.medium))
      Text(detail).font(.caption).foregroundStyle(.secondary)
    }
    .frame(maxWidth: .infinity, minHeight: 85)
  }

  private func logRow(_ entry: RunLogEntry) -> some View {
    HStack(alignment: .firstTextBaseline, spacing: 10) {
      Text(String(format: "%07.2f", entry.timestamp))
        .foregroundStyle(.secondary).frame(width: 64, alignment: .trailing)
      Text(entry.severity.rawValue)
        .foregroundStyle(color(for: entry.severity)).frame(width: 42, alignment: .leading)
      Text(entry.message)
        .foregroundStyle(AppTheme.primaryText)
        .textSelection(.enabled)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
    .font(.system(size: 12, design: .monospaced))
    .padding(.horizontal, 12).padding(.vertical, 6)
    .help(entry.phase?.displayName ?? "运行事件")
  }

  private func color(for severity: LogSeverity) -> Color {
    switch severity {
    case .error: AppTheme.danger
    case .warning: AppTheme.warning
    case .info: AppTheme.indigo
    case .debug, .trace: AppTheme.secondaryText
    }
  }

  private static func plainText(_ entry: RunLogEntry) -> String {
    "\(String(format: "%.2f", entry.timestamp))  \(entry.severity.rawValue)  \(entry.phase?.rawValue ?? "—")  \(entry.message)"
  }
}
