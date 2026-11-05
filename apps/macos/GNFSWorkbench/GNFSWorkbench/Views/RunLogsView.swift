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
    VStack(alignment: .leading, spacing: 0) {
      Divider()
      Button {
        isExpanded.toggle()
      } label: {
        HStack(spacing: 8) {
          Image(systemName: isExpanded ? "chevron.down" : "chevron.right")
            .font(.system(size: 9, weight: .semibold)).foregroundStyle(.tertiary)
            .frame(width: 10)
          Text("运行日志").font(.system(size: 12, weight: .medium))
          Text("\(entries.count)")
            .font(.system(size: 11).monospacedDigit()).foregroundStyle(.tertiary)
          Spacer()
          Text(isExpanded ? "收起" : "展开")
            .font(.system(size: 10)).foregroundStyle(.tertiary)
        }
        .padding(.vertical, 14)
        .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .accessibilityIdentifier("toggleLogsButton")
      if isExpanded {
        HStack(spacing: 10) {
          TextField("搜索日志", text: $query)
            .textFieldStyle(.roundedBorder)
            .frame(minWidth: 140, maxWidth: 280)
            .accessibilityLabel("搜索运行日志")
            .accessibilityIdentifier("logSearch")
          Picker("级别", selection: $severityFilter) {
            Text("全部级别").tag(nil as LogSeverity?)
            ForEach([LogSeverity.info, .warning, .error, .debug, .trace], id: \.rawValue) {
              Text($0.rawValue).tag(Optional($0))
            }
          }
          .labelsHidden().frame(width: 104)
          Spacer(minLength: 0)
          Button {
            copy(visibleEntries.map(Self.plainText).joined(separator: "\n"))
          } label: {
            Image(systemName: "doc.on.doc")
          }
          .buttonStyle(.borderless)
          .disabled(visibleEntries.isEmpty)
          .help("复制筛选结果")
          .accessibilityLabel("复制筛选日志")
          .accessibilityIdentifier("copyLogsButton")
          Menu {
            Button("复制筛选结果") { copy(visibleEntries.map(Self.plainText).joined(separator: "\n")) }
              .disabled(visibleEntries.isEmpty)
            Button("复制全部日志") { copy(entries.map(Self.plainText).joined(separator: "\n")) }
              .disabled(entries.isEmpty)
            Divider()
            Button("清除日志", role: .destructive, action: clear)
              .disabled(entries.isEmpty || !canClear)
          } label: {
            Image(systemName: "ellipsis")
          }
          .menuIndicator(.hidden).menuStyle(.borderlessButton).fixedSize()
          .accessibilityLabel("日志操作")
        }
        .controlSize(.small)
        .padding(.bottom, 10)
        Divider()
        if entries.isEmpty {
          emptyState("暂无日志", detail: "运行事件与诊断信息会显示在这里。")
        } else if visibleEntries.isEmpty {
          VStack(spacing: 0) {
            emptyState("没有匹配的日志", detail: "试试其他关键词或级别。")
            Button("重置筛选") {
              query = ""
              severityFilter = nil
            }
            .controlSize(.small).padding(.bottom, 12)
          }
        } else {
          ScrollViewReader { proxy in
            ScrollView {
              LazyVStack(alignment: .leading, spacing: 0) {
                ForEach(visibleEntries) { entry in logRow(entry).id(entry.id) }
              }
              .padding(.vertical, 8)
            }
            .frame(height: 180)
            .background(AppTheme.canvas.opacity(0.6))
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
        Divider()
        HStack {
          Text("\(visibleEntries.count) 条 · 保留最近 600 条")
            .font(.system(size: 10)).foregroundStyle(.tertiary)
          Spacer()
          Toggle("跟随最新", isOn: $followsLatest)
            .toggleStyle(.checkbox).font(.system(size: 10))
            .controlSize(.mini)
            .accessibilityIdentifier("followLogsToggle")
        }
        .padding(.vertical, 9)
      }
    }
    .accessibilityIdentifier("runLogs")
  }

  private func emptyState(_ title: String, detail: String) -> some View {
    VStack(spacing: 5) {
      Text(title).font(.system(size: 11, weight: .medium)).foregroundStyle(.secondary)
      Text(detail).font(.system(size: 10)).foregroundStyle(.tertiary)
    }
    .frame(maxWidth: .infinity, minHeight: 72)
  }

  private func logRow(_ entry: RunLogEntry) -> some View {
    HStack(alignment: .firstTextBaseline, spacing: 10) {
      Text(String(format: "%07.2f", entry.timestamp))
        .foregroundStyle(.tertiary).frame(width: 58, alignment: .trailing)
      Text(entry.severity.rawValue)
        .foregroundStyle(color(for: entry.severity)).frame(width: 38, alignment: .leading)
      Text(entry.message)
        .foregroundStyle(AppTheme.primaryText)
        .textSelection(.enabled)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
    .font(.system(size: 11, design: .monospaced))
    .padding(.horizontal, 10).padding(.vertical, 4)
    .help(entry.phase?.displayName ?? "运行事件")
  }

  private func color(for severity: LogSeverity) -> Color {
    switch severity {
    case .error: AppTheme.danger
    case .warning: AppTheme.warning
    case .info, .debug, .trace: AppTheme.secondaryText
    }
  }

  private static func plainText(_ entry: RunLogEntry) -> String {
    "\(String(format: "%.2f", entry.timestamp))  \(entry.severity.rawValue)  \(entry.phase?.rawValue ?? "—")  \(entry.message)"
  }
}
