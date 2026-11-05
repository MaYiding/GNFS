import AppKit
import SwiftUI

@MainActor
final class WorkbenchAppDelegate: NSObject, NSApplicationDelegate {
  weak var model: AppModel?
  var openWorkbench: (() -> Void)?

  func applicationDidFinishLaunching(_ notification: Notification) {
    // Older releases used a type-derived WindowGroup restoration identifier.
    // AppKit can try to restore that obsolete scene, receive no window, and
    // still suppress the normal initial window. Keep the user's saved state;
    // open the stable, unique scene after restoration has finished instead.
    DispatchQueue.main.async { [weak self] in
      guard !NSApp.windows.contains(where: { $0.canBecomeMain }) else { return }
      self?.openWorkbench?()
    }
  }

  func applicationShouldHandleReopen(
    _ sender: NSApplication, hasVisibleWindows flag: Bool
  ) -> Bool {
    if !flag { openWorkbench?() }
    return true
  }

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
  @Environment(\.openWindow) private var openWindow
  @State private var model = AppModel()

  var body: some Scene {
    let _ = configureDelegate()
    Window("GNFS Workbench", id: "workbench") {
      ContentView(model: model)
        .frame(minWidth: 900, minHeight: 640)
    }
    .defaultSize(width: 1_240, height: 860)
    .windowToolbarStyle(.unifiedCompact(showsTitle: false))
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

  private func configureDelegate() {
    appDelegate.model = model
    appDelegate.openWorkbench = { openWindow(id: "workbench") }
  }
}
