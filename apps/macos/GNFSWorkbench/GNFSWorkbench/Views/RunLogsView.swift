import SwiftUI

struct RunLogsView: View {
  let entries: [RunLogEntry]
  @Binding var isExpanded: Bool
  let canClear: Bool
  let copy: (String) -> Void
  let clear: () -> Void
  @State private var query = ""
  @State private var severityFilter: LogSeverity?

  private var visibleEntries: [RunLogEntry] {
    return entries.filter {
      let severityMatches = severityFilter == nil || $0.severity == severityFilter
      let queryMatches =
        query.isEmpty
        || $0.message.localizedCaseInsensitiveContains(query)
        || $0.severity.rawValue.localizedCaseInsensitiveContains(query)
        || ($0.phase?.displayName.localizedCaseInsensitiveContains(query) ?? false)
      return severityMatches && queryMatches
    }
  }

  var body: some View {
    DisclosureGroup(isExpanded: $isExpanded) {
      VStack(spacing: 11) {
        if entries.isEmpty {
          ContentUnavailableView(
            "暂无运行日志",
            systemImage: "terminal",
            description: Text("任务开始后，结构化事件会实时显示在这里。")
          )
          .frame(maxWidth: .infinity, minHeight: 154)
        } else if visibleEntries.isEmpty {
          if query.isEmpty, let severityFilter {
            ContentUnavailableView(
              "没有 \(severityFilter.rawValue) 日志",
              systemImage: "line.3.horizontal.decrease.circle",
              description: Text("请选择其他日志级别或恢复显示全部级别。")
            )
            .frame(maxWidth: .infinity, minHeight: 154)
          } else {
            ContentUnavailableView.search(text: query)
              .frame(maxWidth: .infinity, minHeight: 154)
          }
        } else {
          ScrollViewReader { proxy in
            ScrollView {
              LazyVStack(alignment: .leading, spacing: 0) {
                ForEach(visibleEntries) { entry in
                  logRow(entry)
                    .id(entry.id)
                }
              }
              .padding(.vertical, 4)
            }
            .frame(height: 196)
            .background(
              AppTheme.canvas.opacity(0.48),
              in: RoundedRectangle(
                cornerRadius: 6,
                style: .continuous
              )
            )
            .onChange(of: entries.count) {
              guard query.isEmpty, severityFilter == nil, let last = entries.last else {
                return
              }
              proxy.scrollTo(last.id, anchor: .bottom)
            }
          }
        }
      }
      .padding(.top, 12)
    } label: {
      HStack(spacing: 11) {
        Image(systemName: "terminal")
          .foregroundStyle(AppTheme.cyan)
        Text("运行日志")
          .font(.system(size: 13, weight: .semibold))
        Text("\(entries.count)")
          .font(.gnfsMonospaced(size: 10, weight: .medium))
          .foregroundStyle(AppTheme.tertiaryText)

        Spacer()

        if isExpanded {
          Button("清除", action: clear)
            .buttonStyle(.borderless)
            .font(.system(size: 11.5, weight: .medium))
            .foregroundStyle(AppTheme.secondaryText)
            .disabled(entries.isEmpty || !canClear)

          Menu {
            Button("全部级别") { severityFilter = nil }
            Divider()
            ForEach(
              [LogSeverity.info, .warning, .error, .debug, .trace],
              id: \.rawValue
            ) { severity in
              Button(severity.rawValue) { severityFilter = severity }
            }
          } label: {
            Label(
              severityFilter?.rawValue ?? "全部",
              systemImage: "line.3.horizontal.decrease.circle"
            )
          }
          .menuStyle(.borderlessButton)
          .fixedSize()
          .font(.system(size: 11.5))

          TextField("筛选", text: $query)
            .textFieldStyle(.roundedBorder)
            .frame(width: 168)
            .font(.system(size: 11.5))
            .accessibilityIdentifier("logSearch")

          Button {
            copy(entries.map(Self.plainText).joined(separator: "\n"))
          } label: {
            Image(systemName: "doc.on.doc")
          }
          .buttonStyle(.borderless)
          .help("复制全部日志")
          .disabled(entries.isEmpty)
        }
      }
      .foregroundStyle(AppTheme.primaryText)
    }
    .workbenchPanel(padding: 18)
    .accessibilityIdentifier("runLogs")
  }

  private func logRow(_ entry: RunLogEntry) -> some View {
    HStack(alignment: .firstTextBaseline, spacing: 10) {
      Text(String(format: "%07.2f", entry.timestamp))
        .foregroundStyle(AppTheme.tertiaryText)
        .frame(width: 58, alignment: .trailing)
      Text(entry.severity.rawValue)
        .foregroundStyle(color(for: entry.severity))
        .frame(width: 42, alignment: .leading)
      Text(entry.phase?.rawValue.uppercased() ?? "—")
        .foregroundStyle(AppTheme.indigo)
        .frame(width: 52, alignment: .leading)
      Text(entry.message)
        .foregroundStyle(AppTheme.secondaryText)
        .textSelection(.enabled)
      Spacer(minLength: 0)
    }
    .font(.gnfsMonospaced(size: 12))
    .padding(.horizontal, 12)
    .padding(.vertical, 4)
  }

  private func color(for severity: LogSeverity) -> Color {
    switch severity {
    case .error: AppTheme.danger
    case .warning: AppTheme.warning
    case .info: AppTheme.cyan
    case .debug, .trace: AppTheme.secondaryText
    }
  }

  private static func plainText(_ entry: RunLogEntry) -> String {
    String(
      format: "%07.2f  %-5@  %-7@  %@",
      entry.timestamp,
      entry.severity.rawValue,
      entry.phase?.rawValue.uppercased() ?? "—",
      entry.message
    )
  }
}
