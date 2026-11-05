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
  @State private var readingAnchor: UUID?
  @State private var rememberedReadingAnchor: UUID?
  @State private var isAtBottom = true
  @State private var isUserScrolling = false
  @State private var pendingTailID: UUID?
  @State private var followTask: Task<Void, Never>?
  @Environment(\.workbenchReduceMotion) private var reduceMotion

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
        if isExpanded { rememberReadingPosition() }
        withAnimation(reduceMotion ? nil : WorkbenchMotion.disclosure) {
          isExpanded.toggle()
        }
      } label: {
        HStack(spacing: 8) {
          Image(systemName: "chevron.right")
            .font(.system(size: 9, weight: .semibold)).foregroundStyle(.tertiary)
            .frame(width: 10)
            .rotationEffect(.degrees(isExpanded ? 90 : 0))
          Text("运行日志").font(.system(size: 12, weight: .medium))
          Text("\(entries.count)")
            .font(.system(size: 11).monospacedDigit()).foregroundStyle(.tertiary)
            .frame(minWidth: 24, alignment: .leading)
            .contentTransition(.identity)
          Spacer()
          Text(isExpanded ? "收起" : "展开")
            .font(.system(size: 10)).foregroundStyle(.tertiary)
        }
        .padding(.vertical, 8)
        .contentShape(Rectangle())
      }
      .buttonStyle(WorkbenchActionStyle(.quiet))
      .accessibilityValue(isExpanded ? "已展开" : "已收起")
      .accessibilityIdentifier("toggleLogsButton")
      if isExpanded {
        expandedContent
          .transition(.opacity)
      }
    }
    .accessibilityElement(children: .contain)
    .accessibilityIdentifier("runLogs")
    .onChange(of: isExpanded) {
      if !isExpanded {
        isUserScrolling = false
        stopFollowingTask()
      }
    }
    .onChange(of: readingAnchor) {
      // Initial layout and removing the ScrollView can reset its binding.
      // Only user scrolling advances the remembered position between disclosures.
      if isExpanded && isUserScrolling { rememberReadingPosition() }
    }
    .onDisappear {
      isUserScrolling = false
      stopFollowingTask()
    }
  }

  private var expandedContent: some View {
    let filtered = visibleEntries
    return ScrollViewReader { proxy in
      VStack(alignment: .leading, spacing: 0) {
        HStack(spacing: 10) {
          WorkbenchSearchField(
            title: "搜索日志", text: $query,
            accessibilityLabel: "搜索运行日志", identifier: "logSearch"
          )
          .frame(minWidth: 140, maxWidth: 280)
          Picker("级别", selection: $severityFilter) {
            Text("全部级别").tag(nil as LogSeverity?)
            ForEach([LogSeverity.info, .warning, .error, .debug, .trace], id: \.rawValue) {
              Text($0.rawValue).tag(Optional($0))
            }
          }
          .labelsHidden().frame(width: 104)
          Spacer(minLength: 0)
          Button {
            copy(filtered.map(Self.plainText).joined(separator: "\n"))
          } label: {
            Image(systemName: "doc.on.doc")
          }
          .buttonStyle(WorkbenchActionStyle(.quiet))
          .disabled(filtered.isEmpty)
          .help("复制筛选结果")
          .accessibilityLabel("复制筛选日志")
          .accessibilityIdentifier("copyLogsButton")
          Menu {
            Button("复制筛选结果") { copy(filtered.map(Self.plainText).joined(separator: "\n")) }
              .disabled(filtered.isEmpty)
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
        ScrollView {
          LazyVStack(alignment: .leading, spacing: 0) {
            ForEach(filtered) { entry in logRow(entry).id(entry.id) }
          }
          .scrollTargetLayout()
          .padding(.vertical, 8)
        }
        .scrollPosition(id: $readingAnchor, anchor: .top)
        .frame(height: 180)
        .background(AppTheme.canvas.opacity(0.6))
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("logConsole")
        .onScrollGeometryChange(for: Bool.self) { geometry in
          geometry.contentSize.height <= geometry.containerSize.height
            || geometry.visibleRect.maxY >= geometry.contentSize.height - 20
        } action: { _, atBottom in
          isAtBottom = atBottom
          if isUserScrolling && !atBottom { followsLatest = false }
        }
        .onScrollPhaseChange { oldPhase, newPhase in
          isUserScrolling =
            newPhase == .tracking || newPhase == .interacting || newPhase == .decelerating
          if isUserScrolling {
            stopFollowingTask()
            if !isAtBottom { followsLatest = false }
          } else if newPhase == .idle,
            oldPhase == .tracking || oldPhase == .interacting || oldPhase == .decelerating,
            isAtBottom
          {
            followsLatest = true
          }
        }
        .overlay {
          if entries.isEmpty {
            emptyState("暂无日志", detail: "运行事件与诊断信息会显示在这里。")
          } else if filtered.isEmpty {
            VStack(spacing: 0) {
              emptyState("没有匹配的日志", detail: "试试其他关键词或级别。")
              Button("重置筛选") {
                query = ""
                severityFilter = nil
              }
              .buttonStyle(WorkbenchActionStyle(.secondary))
            }
          }
        }
        Divider()
        HStack {
          Text("\(filtered.count) 条 · 保留最近 600 条")
            .font(.system(size: 10).monospacedDigit()).foregroundStyle(.tertiary)
            .contentTransition(.identity)
          Spacer()
          if !followsLatest && !filtered.isEmpty {
            Button("最新日志", systemImage: "arrow.down.to.line") { followsLatest = true }
              .buttonStyle(WorkbenchActionStyle(.quiet))
              .accessibilityIdentifier("jumpToLatestLogsButton")
          }
          Toggle("跟随最新", isOn: $followsLatest)
            .toggleStyle(.checkbox).font(.system(size: 10))
            .controlSize(.mini)
            .accessibilityIdentifier("followLogsToggle")
        }
        .padding(.vertical, 9)
      }
      .task {
        if followsLatest {
          scheduleFollow(to: filtered.last?.id, using: proxy)
        } else {
          let remembered = rememberedReadingAnchor ?? readingAnchor
          let target = filtered.first(where: { $0.id == remembered })?.id ?? filtered.first?.id
          // Wait for the new scroll host to register its row targets. This task is
          // cancelled automatically if the disclosure closes again before layout.
          await Task.yield()
          guard !Task.isCancelled, isExpanded, !followsLatest, !isUserScrolling,
            let target
          else { return }
          readingAnchor = target
          rememberedReadingAnchor = target
          var transaction = Transaction()
          transaction.disablesAnimations = true
          withTransaction(transaction) { proxy.scrollTo(target, anchor: .top) }
        }
      }
      .onChange(of: filtered.last?.id) {
        scheduleFollow(to: filtered.last?.id, using: proxy)
      }
      .onChange(of: followsLatest) {
        if followsLatest {
          scheduleFollow(to: filtered.last?.id, using: proxy)
        } else {
          rememberReadingPosition()
          stopFollowingTask()
        }
      }
      .onChange(of: query) { showFirstResult(filtered, using: proxy) }
      .onChange(of: severityFilter) { showFirstResult(filtered, using: proxy) }
      .onChange(of: entries.first?.id) {
        // Retention can remove the currently read row. Prefer the oldest remaining match
        // rather than jumping to the newest entry while following is paused.
        if !followsLatest, let anchor = readingAnchor ?? rememberedReadingAnchor,
          !filtered.contains(where: { $0.id == anchor })
        {
          self.readingAnchor = filtered.first?.id
          rememberedReadingAnchor = filtered.first?.id
        }
      }
    }
  }

  private func showFirstResult(_ filtered: [RunLogEntry], using proxy: ScrollViewProxy) {
    followsLatest = false
    stopFollowingTask()
    readingAnchor = filtered.first?.id
    rememberedReadingAnchor = filtered.first?.id
    if let first = filtered.first { proxy.scrollTo(first.id, anchor: .top) }
  }

  private func rememberReadingPosition() {
    guard !followsLatest, let readingAnchor else { return }
    rememberedReadingAnchor = readingAnchor
  }

  private func scheduleFollow(to id: UUID?, using proxy: ScrollViewProxy) {
    pendingTailID = id
    guard isExpanded, followsLatest, !isUserScrolling, id != nil, followTask == nil else { return }
    // Coalesce incoming events without a debounce that can starve during continuous output.
    followTask = Task { @MainActor in
      do { try await Task.sleep(for: .milliseconds(100)) } catch { return }
      // A cancelled predecessor must not clear a new task scheduled after reopening.
      guard !Task.isCancelled else { return }
      guard isExpanded, followsLatest, !isUserScrolling else {
        followTask = nil
        return
      }
      if let pendingTailID {
        var transaction = Transaction()
        transaction.disablesAnimations = true
        withTransaction(transaction) { proxy.scrollTo(pendingTailID, anchor: .bottom) }
      }
      followTask = nil
    }
  }

  private func stopFollowingTask() {
    followTask?.cancel()
    followTask = nil
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
        .foregroundStyle(.tertiary).lineLimit(1).frame(width: 64, alignment: .trailing)
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
    .transaction { $0.animation = nil }
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
