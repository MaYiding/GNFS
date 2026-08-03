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
    WindowGroup("GNFS Workbench") {
      ContentView(model: model)
        .preferredColorScheme(.dark)
        .frame(minWidth: 1_000, minHeight: 740)
        .onAppear { appDelegate.model = model }
    }
    .defaultSize(width: 1_320, height: 940)
    .windowStyle(.hiddenTitleBar)
    .commands {
      CommandGroup(replacing: .newItem) {
        Button("新建分解") { model.newRun() }
          .keyboardShortcut("n", modifiers: .command)
          .disabled(model.isRunTaskActive)
      }
      CommandMenu("运行") {
        Button("开始完整质因数分解") {
          Task { await model.startRun() }
        }
        .keyboardShortcut("r", modifiers: .command)
        .disabled(!model.canStart)

        Button("取消当前任务") { model.cancelRun() }
          .keyboardShortcut(".", modifiers: .command)
          .disabled(!model.isRunning)

        Divider()

        Button("参数设置") { model.isParametersPresented.toggle() }
          .keyboardShortcut(",", modifiers: [.command, .shift])
      }
    }
  }
}
