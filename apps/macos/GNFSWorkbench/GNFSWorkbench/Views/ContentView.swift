import SwiftUI

struct ContentView: View {
  @Bindable var model: AppModel
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    VStack(spacing: 0) {
      WorkbenchHeaderView(model: model)
      HStack(spacing: 0) {
        if model.isHistoryPresented {
          HistoryView(model: model, embedded: true)
            .frame(width: 236)
          Divider()
        }
        ScrollView {
          VStack(spacing: AppTheme.sectionSpacing) {
            if model.historyRestorationState != .ready { historyRecovery }
            if model.isRunning, model.displayedRun?.id != model.activeRun?.id {
              HStack {
                Label("另一个任务正在运行", systemImage: "waveform.path.ecg")
                Spacer()
                Button("返回当前任务") { model.selectedRunID = model.activeRun?.id }
              }
              .font(.callout)
              .workbenchPanel(padding: 14)
            }
            NumberHeroView(
              configuration: $model.draftConfiguration,
              run: model.displayedRun,
              validationMessage: model.inputValidationMessage,
              canSubmit: model.canStart,
              submit: { Task { await model.startRun() } },
              copy: model.copy
            )
            if model.displayedRun?.status.isTerminal != true {
              PipelineRailView(run: model.displayedRun)
                .workbenchPanel(padding: 18)
            }
            RunStageView(
              run: model.displayedRun,
              finalizationMessage: model.displayedRun.map(model.finalizationMessage(for:)) ?? "",
              canCreateRun: model.canEdit,
              copy: model.copy,
              newRun: { Task { await model.newRun() } }
            )
            .workbenchPanel(padding: 20)
            .id(model.displayedRun?.id)
            RunLogsView(
              entries: model.displayedRun?.logs ?? [],
              isExpanded: $model.areLogsExpanded,
              canClear: model.canEdit,
              copy: model.copy,
              clear: { Task { await model.clearDisplayedLogs() } }
            )
            .id(model.displayedRun?.id)
            if let run = model.displayedRun, run.status.isTerminal {
              Text(model.finalizationMessage(for: run))
                .font(.caption).foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
          }
          .padding(AppTheme.contentPadding)
          .frame(maxWidth: AppTheme.contentMaxWidth)
          .frame(maxWidth: .infinity)
        }
        .scrollBounceBehavior(.basedOnSize)
        .background(AppTheme.canvas)
      }
    }
    .foregroundStyle(AppTheme.primaryText)
    .tint(AppTheme.indigo)
    .overlay(alignment: .bottom) {
      if let toast = model.toastMessage {
        Label(toast, systemImage: "checkmark.circle.fill")
          .font(.callout.weight(.medium))
          .padding(.horizontal, 18).padding(.vertical, 12)
          .background(.regularMaterial, in: Capsule())
          .shadow(color: .black.opacity(0.12), radius: 12, y: 4)
          .padding(.bottom, 20)
          .accessibilityLabel(toast)
      }
    }
    .animation(reduceMotion ? nil : .easeOut(duration: 0.18), value: model.toastMessage)
    .sheet(isPresented: $model.isParametersPresented) {
      ParameterEditorSheet(model: model)
    }
    .alert("无法继续", isPresented: alertBinding) {
      Button("好", role: .cancel) { model.alertMessage = nil }
    } message: {
      Text(model.alertMessage ?? "发生未知错误。")
    }
  }

  private var historyRecovery: some View {
    HStack(alignment: .top, spacing: 12) {
      if model.historyRestorationState == .loading {
        ProgressView().controlSize(.small)
        Text("正在读取本机历史…")
      } else {
        Image(systemName: "exclamationmark.triangle").foregroundStyle(AppTheme.warning)
        VStack(alignment: .leading, spacing: 5) {
          Text("运行历史暂时不可用").fontWeight(.semibold)
          Text("修复存储问题后可重试。已隔离的损坏文件会保留，不会覆盖。")
            .font(.caption).foregroundStyle(.secondary)
        }
        Spacer()
        Button("重试读取") { Task { await model.retryHistoryRestoration() } }
          .accessibilityIdentifier("retryHistoryButton")
      }
    }
    .frame(maxWidth: .infinity, alignment: .leading)
    .workbenchPanel(padding: 16)
  }

  private var alertBinding: Binding<Bool> {
    Binding(get: { model.alertMessage != nil }, set: { if !$0 { model.alertMessage = nil } })
  }
}
