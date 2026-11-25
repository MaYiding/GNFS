import SwiftUI

struct ContentView: View {
  @Bindable var model: AppModel
  @Environment(\.workbenchReduceMotion) private var reduceMotion

  var body: some View {
    NavigationSplitView(columnVisibility: sidebarVisibility) {
      HistoryView(model: model, embedded: true)
        .frame(minWidth: 220, idealWidth: 250, maxWidth: 320)
        .navigationSplitViewColumnWidth(min: 220, ideal: 250, max: 320)
        .toolbar(removing: .sidebarToggle)
    } detail: {
      ScrollViewReader { proxy in
        ScrollView {
          ZStack(alignment: .topLeading) {
            document
              .id(model.displayedRun?.id)
              .transition(.opacity)
          }
          .animation(reduceMotion ? nil : WorkbenchMotion.page, value: model.displayedRun?.id)
          .padding(AppTheme.contentPadding)
          .frame(maxWidth: AppTheme.contentMaxWidth)
          .frame(maxWidth: .infinity, alignment: .top)
          .id("documentTop")
        }
        .scrollBounceBehavior(.basedOnSize)
        .background(AppTheme.canvas)
        .accessibilityIdentifier("workbenchDocument")
        .onChange(of: model.displayedRun?.id) {
          var transaction = Transaction()
          transaction.disablesAnimations = true
          withTransaction(transaction) { proxy.scrollTo("documentTop", anchor: .top) }
        }
      }
      .frame(minWidth: 0, maxWidth: .infinity, minHeight: 0, maxHeight: .infinity)
    }
    .navigationSplitViewStyle(.balanced)
    .modifier(WorkbenchToolbar(model: model))
    .foregroundStyle(AppTheme.primaryText)
    .tint(AppTheme.indigo)
    .overlay(alignment: .bottom) {
      ZStack {
        if let toast = model.toastMessage {
          Label(toast, systemImage: "checkmark.circle.fill")
            .font(.callout.weight(.medium))
            .padding(.horizontal, 16).padding(.vertical, 10)
            .background(.regularMaterial, in: Capsule())
            .overlay { Capsule().strokeBorder(AppTheme.separator, lineWidth: 1) }
            .shadow(color: .black.opacity(0.08), radius: 8, y: 3)
            .accessibilityLabel(toast)
            .accessibilityIdentifier("copyConfirmation")
            .transition(reduceMotion ? .identity : .opacity.combined(with: .offset(y: 5)))
        }
      }
      .padding(.bottom, 18)
      .allowsHitTesting(false)
      .animation(reduceMotion ? nil : WorkbenchMotion.feedback, value: model.toastMessage)
    }
    .sheet(isPresented: $model.isParametersPresented) {
      ParameterEditorSheet(model: model)
    }
    .alert("无法继续", isPresented: alertBinding) {
      Button("好", role: .cancel) { model.alertMessage = nil }
    } message: {
      Text(model.alertMessage ?? "发生未知错误。")
    }
  }

  private var sidebarVisibility: Binding<NavigationSplitViewVisibility> {
    Binding(
      get: { model.isHistoryPresented ? .all : .detailOnly },
      set: { model.isHistoryPresented = $0 != .detailOnly }
    )
  }

  private var document: some View {
    VStack(spacing: AppTheme.sectionSpacing) {
      if model.historyRestorationState != .ready { historyRecovery }
      if model.isRunning, model.displayedRun?.id != model.activeRun?.id {
        HStack {
          Label("另一个任务正在运行", systemImage: "waveform.path.ecg")
          Spacer()
          Button("返回当前任务") { model.selectedRunID = model.activeRun?.id }
            .buttonStyle(WorkbenchActionStyle(.secondary))
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
      if let run = model.displayedRun, !run.status.isTerminal, run.usesGNFSPipeline {
        PipelineRailView(run: model.displayedRun)
          .padding(.vertical, 8)
      }
      if model.displayedRun != nil {
        Divider()
        RunStageView(
          run: model.displayedRun,
          finalizationMessage: model.displayedRun.map(model.finalizationMessage(for:)) ?? "",
          canCreateRun: model.canEdit,
          copy: model.copy,
          newRun: { Task { await model.newRun() } }
        )
        .id(model.displayedRun?.id)
        RunLogsView(
          entries: model.displayedRun?.logs ?? [],
          isExpanded: $model.areLogsExpanded,
          canClear: model.canEdit,
          copy: model.copy,
          clear: { Task { await model.clearDisplayedLogs() } }
        )
        .id(model.displayedRun?.id)
      }
      if let run = model.displayedRun, run.status.isTerminal {
        Text(model.finalizationMessage(for: run))
          .font(.caption).foregroundStyle(.secondary)
          .frame(maxWidth: .infinity, alignment: .leading)
      }
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
