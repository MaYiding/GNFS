import SwiftUI

struct WorkbenchHeaderView: View {
  @Bindable var model: AppModel
  @State private var report: RunReportDocument?
  @State private var isExportPresented = false

  var body: some View {
    HStack(spacing: 12) {
      Button {
        model.isHistoryPresented.toggle()
      } label: {
        Image(systemName: "sidebar.left")
      }
      .help("显示或隐藏运行历史")
      .accessibilityLabel("运行历史")
      .accessibilityIdentifier("historyButton")

      VStack(alignment: .leading, spacing: 3) {
        Text("GNFS Workbench").font(.headline)
        Text(model.isRunning ? "正在分解 · 可随时停止" : "整数分解工作台")
          .font(.caption).foregroundStyle(.secondary)
      }
      Spacer(minLength: 8)
      Button {
        Task { await model.newRun() }
      } label: {
        Label("新建", systemImage: "plus")
      }
      .disabled(!model.canEdit)
      .help("新建分解 ⌘N")
      .accessibilityIdentifier("newRunButton")
      Button {
        model.isParametersPresented = true
      } label: {
        Label("参数", systemImage: "slider.horizontal.3")
      }
      .help(model.displayedRun == nil ? "编辑本次运行参数" : "查看此记录的运行参数")
      .accessibilityIdentifier("parametersButton")
      Button {
        guard let run = model.displayedRun else { return }
        do {
          report = try RunReportDocument(run: run)
          isExportPresented = true
        } catch {
          model.alertMessage = "无法生成运行记录：\(error.localizedDescription)"
        }
      } label: {
        Image(systemName: "square.and.arrow.up")
      }
      .disabled(model.displayedRun == nil)
      .help("导出此任务的输入、参数、结果与日志（JSON）")
      .accessibilityLabel("导出运行记录")
      .accessibilityIdentifier("exportRunButton")
      Button {
        if model.isRunning { model.cancelRun() } else { Task { await model.runOrRepeat() } }
      } label: {
        Label(primaryLabel, systemImage: model.isRunning ? "stop.fill" : "play.fill")
          .frame(minWidth: 70)
      }
      .buttonStyle(.borderedProminent)
      .tint(model.isRunning ? AppTheme.danger : AppTheme.indigo)
      .disabled(primaryDisabled)
      .help(model.isRunning ? "停止当前任务 ⌘." : "开始分解 ⌘R")
      .accessibilityIdentifier(model.isRunning ? "cancelRunButton" : "startRunButton")
    }
    .controlSize(.large)
    .padding(.horizontal, 20)
    .frame(height: AppTheme.headerHeight)
    .background(AppTheme.header)
    .overlay(alignment: .bottom) { Divider() }
    .fileExporter(
      isPresented: $isExportPresented,
      document: report,
      contentType: .json,
      defaultFilename: "GNFS-\(model.displayedRun?.id.uuidString.prefix(8) ?? "run")"
    ) { result in
      if case .failure(let error) = result {
        model.alertMessage = "无法导出运行记录：\(error.localizedDescription)"
      }
    }
  }

  private var primaryLabel: String {
    if model.activeRun?.status == .cancelling { return "停止中…" }
    if model.isRunning { return "停止" }
    if model.isRunTaskActive { return "保存中…" }
    return model.displayedRun == nil ? "开始分解" : "再次分解"
  }

  private var primaryDisabled: Bool {
    if model.activeRun?.status == .cancelling { return true }
    if model.isRunning { return false }
    if model.displayedRun != nil { return !model.canEdit }
    return !model.canStart
  }
}
