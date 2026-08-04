import SwiftUI

struct ContentView: View {
  @Bindable var model: AppModel

  var body: some View {
    ZStack {
      LinearGradient(
        colors: [AppTheme.elevated.opacity(0.88), AppTheme.canvas],
        startPoint: .top,
        endPoint: .bottom
      )
      .ignoresSafeArea()

      VStack(spacing: 0) {
        WorkbenchHeaderView(model: model)

        GeometryReader { geometry in
          ScrollView {
            VStack(spacing: AppTheme.sectionSpacing) {
              NumberHeroView(
                configuration: $model.draftConfiguration,
                run: model.displayedRun,
                validationMessage: model.inputValidationMessage,
                canSubmit: model.canStart,
                submit: { Task { await model.startRun() } },
                copy: model.copy
              )

              PipelineRailView(run: model.displayedRun)
                .workbenchPanel(padding: 20)

              RunStageView(
                run: model.displayedRun,
                finalizationMessage: model.displayedRun.map(model.finalizationMessage(for:))
                  ?? "历史保存和临时目录清理状态尚未确认。"
              ) {
                model.copy($0)
              } newRun: {
                Task { await model.newRun() }
              }

              RunLogsView(
                entries: model.displayedRun?.logs ?? [],
                isExpanded: $model.areLogsExpanded,
                canClear: !model.isRunTaskActive && !model.isHistoryMutationActive,
                copy: model.copy,
                clear: { Task { await model.clearDisplayedLogs() } }
              )
            }
            .padding(.horizontal, AppTheme.contentPadding)
            .padding(.top, 24)
            .padding(.bottom, 26)
            .frame(maxWidth: AppTheme.contentMaxWidth)
            .frame(
              maxWidth: .infinity,
              minHeight: geometry.size.height,
              alignment: .top
            )
          }
          .scrollBounceBehavior(.basedOnSize)
        }
      }

      if let toast = model.toastMessage {
        VStack {
          Spacer()
          Label(toast, systemImage: "checkmark")
            .font(.system(size: 12, weight: .semibold))
            .foregroundStyle(AppTheme.primaryText)
            .padding(.horizontal, 14)
            .frame(height: 38)
            .background(.ultraThinMaterial, in: Capsule())
            .overlay { Capsule().stroke(AppTheme.separatorStrong, lineWidth: 1) }
            .shadow(color: .black.opacity(0.28), radius: 18, y: 8)
            .padding(.bottom, 24)
        }
        .transition(.move(edge: .bottom).combined(with: .opacity))
        .accessibilityLabel(toast)
      }
    }
    .animation(.easeOut(duration: 0.18), value: model.toastMessage)
    .inspector(isPresented: $model.isParametersPresented) {
      ParameterInspectorView(configuration: $model.draftConfiguration)
        .inspectorColumnWidth(min: 300, ideal: 340, max: 400)
    }
    .alert("无法继续", isPresented: alertBinding) {
      Button("好", role: .cancel) { model.alertMessage = nil }
    } message: {
      Text(model.alertMessage ?? "发生未知错误。")
    }
    .onExitCommand {
      if model.isRunning { model.cancelRun() }
    }
  }

  private var alertBinding: Binding<Bool> {
    Binding(
      get: { model.alertMessage != nil },
      set: { if !$0 { model.alertMessage = nil } }
    )
  }
}
