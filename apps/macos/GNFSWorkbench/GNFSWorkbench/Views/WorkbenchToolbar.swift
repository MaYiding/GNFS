import SwiftUI

struct WorkbenchToolbar: ViewModifier {
  @Bindable var model: AppModel
  @Environment(\.workbenchReduceMotion) private var reduceMotion
  @State private var report: RunReportDocument?
  @State private var isExportPresented = false

  func body(content: Content) -> some View {
    content
      .toolbar {
        ToolbarItem(placement: .navigation) {
          Button {
            withAnimation(reduceMotion ? nil : WorkbenchMotion.disclosure) {
              model.isHistoryPresented.toggle()
            }
          } label: {
            Image(systemName: "sidebar.left")
          }
          .help("显示或隐藏运行历史")
          .accessibilityLabel("运行历史")
          .accessibilityIdentifier("historyButton")
        }
        ToolbarItem(placement: .principal) {
          HStack(spacing: 8) {
            Image(systemName: "function").foregroundStyle(AppTheme.indigo)
            Text("GNFS Workbench").font(.system(size: 13, weight: .semibold))
            Circle().fill(AppTheme.indigo).frame(width: 5, height: 5)
              .opacity(model.isRunning ? 1 : 0)
              .accessibilityLabel("正在分解")
              .accessibilityHidden(!model.isRunning)
          }
        }
        ToolbarItemGroup(placement: .primaryAction) {
          Button {
            Task { await model.newRun() }
          } label: {
            Image(systemName: "plus")
          }
          .disabled(!model.canEdit)
          .help("新建分解 ⌘N").accessibilityLabel("新建分解")
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
        }
        ToolbarItem(placement: .primaryAction) {
          Button {
            if model.isRunning { model.cancelRun() } else { Task { await model.runOrRepeat() } }
          } label: {
            HStack(spacing: 6) {
              Image(systemName: model.isRunning ? "stop.fill" : "play.fill")
                .frame(width: 12)
                .contentTransition(.identity)
              Text(primaryLabel)
            }
            .font(.system(size: 12, weight: .medium))
            .frame(minWidth: 86)
          }
          .buttonStyle(.borderedProminent)
          .tint(model.isRunning ? AppTheme.danger : AppTheme.indigo)
          .disabled(primaryDisabled)
          .help(model.isRunning ? "停止当前任务 ⌘." : "开始分解 ⌘R")
          .accessibilityIdentifier(model.isRunning ? "cancelRunButton" : "startRunButton")
        }
      }
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
