import AppKit
import SwiftUI

@MainActor
final class WorkbenchAppDelegate: NSObject, NSApplicationDelegate {
  weak var model: AppModel?

  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
    true
  }

  func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
    guard let model else { return .terminateNow }
    Task {
      await model.prepareForTermination()
      sender.reply(toApplicationShouldTerminate: true)
    }
    return .terminateLater
  }
}

@main
struct GNFSWorkbenchApp: App {
  @NSApplicationDelegateAdaptor(WorkbenchAppDelegate.self) private var appDelegate
  @State private var model = AppModel()

  var body: some Scene {
    Window("GNFS Workbench", id: "workbench") {
      ContentView(model: model)
        .frame(minWidth: 900, minHeight: 640)
        .onAppear { appDelegate.model = model }
    }
    .defaultSize(width: 1_240, height: 860)
    .commands {
      CommandGroup(replacing: .newItem) {
        Button("新建分解") { Task { await model.newRun() } }
          .keyboardShortcut("n", modifiers: .command)
          .disabled(!model.canEdit || model.isParametersPresented)
      }
      CommandMenu("运行") {
        Button("开始完整质因数分解") {
          Task { await model.runOrRepeat() }
        }
        .keyboardShortcut("r", modifiers: .command)
        .disabled(
          !model.canEdit || (model.displayedRun == nil && !model.canStart)
            || model.isParametersPresented)

        Button("取消当前任务") { model.cancelRun() }
          .keyboardShortcut(".", modifiers: .command)
          .disabled(!model.isRunning)

        Divider()

        Button("参数设置") { model.isParametersPresented.toggle() }
          .keyboardShortcut(",", modifiers: [.command, .shift])
        Button("显示运行历史") { model.isHistoryPresented.toggle() }
          .keyboardShortcut("s", modifiers: [.command, .control])
      }
    }
  }
}
