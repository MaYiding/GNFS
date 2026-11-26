import XCTest

@MainActor
final class GNFSWorkbenchUITests: XCTestCase {
  override func setUpWithError() throws {
    continueAfterFailure = false
    // Diagnostic repetition only. Drop what the previous launch saved for the app under
    // test, so every repetition starts from the state a fresh runner gives it: a restored
    // window that is wider than the screen pushed the toolbar button off screen.
    let defaults = Process()
    defaults.executableURL = URL(fileURLWithPath: "/usr/bin/defaults")
    defaults.arguments = ["delete", "org.gnfs.workbench"]
    if (try? defaults.run()) != nil { defaults.waitUntilExit() }
    try? FileManager.default.removeItem(
      atPath: NSHomeDirectory() + "/Library/Saved Application State/org.gnfs.workbench.savedState")
  }

  private func launchApp(realEngine: Bool = false, streamLogs: Bool = false) -> XCUIApplication {
    let app = XCUIApplication()
    app.launchArguments = ["--ui-testing"]
    if realEngine { app.launchArguments.append("--ui-testing-real-engine") }
    if streamLogs { app.launchArguments.append("--ui-testing-stream-logs") }
    app.launch()
    return app
  }

  func testMainWindowAppearsAfterTerminationAndRelaunch() {
    continueAfterFailure = false
    let app = launchApp()
    XCTAssertTrue(app.textFields["numberInput"].waitForExistence(timeout: 5))
    app.terminate()
    app.launch()
    XCTAssertTrue(app.textFields["numberInput"].waitForExistence(timeout: 5))
    XCTAssertEqual(app.windows.count, 1)
    app.typeKey("w", modifierFlags: .command)
    XCTAssertTrue(app.wait(for: .notRunning, timeout: 5))
    app.launch()
    XCTAssertTrue(app.textFields["numberInput"].waitForExistence(timeout: 5))
    XCTAssertEqual(app.windows.count, 1)
  }

  func testPrimaryRunAndCancellationFlow() {
    continueAfterFailure = false
    let app = launchApp()
    let input = app.textFields["numberInput"]
    XCTAssertTrue(input.waitForExistence(timeout: 4))

    let start = app.buttons["startRunButton"]
    XCTAssertTrue(start.isEnabled)
    start.click()

    XCTAssertTrue(app.staticTexts["numberDisplay"].waitForExistence(timeout: 3))
    XCTAssertTrue(app.staticTexts["relationsMetric"].waitForExistence(timeout: 4))

    let cancel = app.buttons["cancelRunButton"]
    XCTAssertTrue(cancel.exists)
    cancel.click()
    XCTAssertTrue(app.staticTexts["任务已取消"].waitForExistence(timeout: 3))
  }

  func testInvalidInputDisablesStart() {
    continueAfterFailure = false
    let app = launchApp()
    let input = app.textFields["numberInput"]
    XCTAssertTrue(input.waitForExistence(timeout: 4))
    input.click()
    input.typeKey("a", modifierFlags: .command)
    input.typeText("1")

    XCTAssertTrue(app.staticTexts["validationMessage"].waitForExistence(timeout: 2))
    XCTAssertFalse(app.buttons["startRunButton"].isEnabled)
  }

  func testParameterInspectorOpens() {
    continueAfterFailure = false
    let app = launchApp()
    let button = app.buttons["parametersButton"]
    XCTAssertTrue(button.waitForExistence(timeout: 4))
    button.click()
    XCTAssertTrue(app.scrollViews["parameterInspector"].waitForExistence(timeout: 3))
  }
  func testEscapeDismissesParametersWithoutCancellingRun() {
    continueAfterFailure = false
    let app = launchApp()
    XCTAssertTrue(app.buttons["startRunButton"].waitForExistence(timeout: 4))
    app.buttons["startRunButton"].click()
    XCTAssertTrue(app.buttons["cancelRunButton"].waitForExistence(timeout: 4))
    app.buttons["parametersButton"].click()
    XCTAssertTrue(app.scrollViews["parameterInspector"].waitForExistence(timeout: 3))
    app.typeKey(.escape, modifierFlags: [])
    XCTAssertTrue(app.buttons["cancelRunButton"].exists)
    XCTAssertTrue(app.buttons["cancelRunButton"].isEnabled)
    app.buttons["cancelRunButton"].click()
  }

  func testInvalidParameterTextBlocksApplyAndCancelPreservesDraft() {
    continueAfterFailure = false
    let app = launchApp()
    XCTAssertTrue(app.buttons["parametersButton"].waitForExistence(timeout: 4))
    app.buttons["parametersButton"].click()
    let field = app.textFields["rationalBoundField"]
    XCTAssertTrue(field.waitForExistence(timeout: 3))
    field.click()
    field.typeText("not-a-number")
    XCTAssertTrue(app.staticTexts["rationalBoundError"].waitForExistence(timeout: 2))
    XCTAssertFalse(app.buttons["applyParametersButton"].isEnabled)
    app.typeKey(.return, modifierFlags: [])
    XCTAssertTrue(app.scrollViews["parameterInspector"].exists)
    app.typeKey(.escape, modifierFlags: [])
    XCTAssertTrue(app.buttons["startRunButton"].isEnabled)
    app.buttons["parametersButton"].click()
    XCTAssertEqual(app.textFields["rationalBoundField"].value as? String, "")
    app.typeKey(.escape, modifierFlags: [])
  }

  func testRealEngineResultHistoryAndParameterCopy() {
    continueAfterFailure = false
    let app = launchApp(realEngine: true)
    XCTAssertTrue(app.buttons["example-360"].waitForExistence(timeout: 4))
    app.buttons["example-360"].click()
    app.buttons["startRunButton"].click()
    XCTAssertTrue(app.staticTexts["完整质因数分解完成"].waitForExistence(timeout: 10))
    XCTAssertTrue(app.staticTexts["完成算法"].exists)
    XCTAssertFalse(app.staticTexts["GNFS 流水线"].exists)
    app.buttons["parametersButton"].click()
    XCTAssertTrue(app.buttons["editParameterCopyButton"].waitForExistence(timeout: 3))
    app.buttons["editParameterCopyButton"].click()
    let field = app.textFields["rationalBoundField"]
    field.click()
    field.typeText("5000")
    app.buttons["applyParametersButton"].click()
    XCTAssertTrue(app.textFields["numberInput"].waitForExistence(timeout: 3))
    XCTAssertEqual(app.textFields["numberInput"].value as? String, "360")
    app.buttons["parametersButton"].click()
    XCTAssertEqual(app.textFields["rationalBoundField"].value as? String, "5000")
    app.typeKey(.escape, modifierFlags: [])
    app.buttons["startRunButton"].click()
    XCTAssertTrue(app.staticTexts["完整质因数分解完成"].waitForExistence(timeout: 10))
    let search = app.textFields["historySearch"]
    search.click()
    search.typeText("no-match")
    XCTAssertTrue(app.staticTexts["没有匹配的记录"].waitForExistence(timeout: 2))
    search.typeKey("a", modifierFlags: .command)
    search.typeText("360")
    XCTAssertTrue(app.buttons.matching(identifier: "history-360").firstMatch.exists)
  }

  func testSidebarDisclosurePreservesDraftAndSearch() {
    continueAfterFailure = false
    let app = launchApp()
    let input = app.textFields["numberInput"]
    XCTAssertTrue(input.waitForExistence(timeout: 4))
    input.click()
    input.typeKey("a", modifierFlags: .command)
    input.typeText("10403")
    let search = app.textFields["historySearch"]
    search.click()
    search.typeText("retained search")
    for _ in 0..<3 {
      app.buttons["historyButton"].click()
      XCTAssertTrue(input.isHittable)
      XCTAssertEqual(input.value as? String, "10403")
      app.buttons["historyButton"].click()
      XCTAssertTrue(search.waitForExistence(timeout: 3))
      XCTAssertTrue(search.isHittable)
      XCTAssertEqual(search.value as? String, "retained search")
    }
    XCTAssertEqual(app.windows.count, 1)
  }

  func testIndividualAutomaticParameterAndReturnApply() {
    continueAfterFailure = false
    let app = launchApp()
    XCTAssertTrue(app.buttons["parametersButton"].waitForExistence(timeout: 4))
    app.buttons["parametersButton"].click()
    let rational = app.textFields["rationalBoundField"]
    let algebraic = app.textFields["algebraicBoundField"]
    XCTAssertTrue(rational.waitForExistence(timeout: 3))
    rational.click()
    rational.typeText("5000")
    algebraic.click()
    algebraic.typeText("10000")
    app.buttons["rationalBoundAutomaticButton"].click()
    XCTAssertEqual(rational.value as? String, "")
    XCTAssertEqual(algebraic.value as? String, "10000")
    algebraic.click()
    app.typeKey(.return, modifierFlags: [])
    XCTAssertTrue(app.scrollViews["parameterInspector"].waitForNonExistence(timeout: 3))
    XCTAssertTrue(app.textFields["numberInput"].isHittable)
    app.buttons["parametersButton"].click()
    XCTAssertEqual(rational.value as? String, "")
    XCTAssertEqual(algebraic.value as? String, "10000")
    app.typeKey(.escape, modifierFlags: [])
  }

  func testReturnSubmitsAndLogDisclosureRetainsFilter() {
    continueAfterFailure = false
    let app = launchApp()
    let input = app.textFields["numberInput"]
    XCTAssertTrue(input.waitForExistence(timeout: 4))
    input.click()
    input.typeKey(.return, modifierFlags: [])
    XCTAssertTrue(app.staticTexts["relationsMetric"].waitForExistence(timeout: 5))
    app.buttons["cancelRunButton"].click()
    XCTAssertTrue(app.staticTexts["任务已取消"].waitForExistence(timeout: 4))
    let disclosure = app.buttons["toggleLogsButton"]
    XCTAssertTrue(disclosure.waitForExistence(timeout: 3))
    disclosure.click()
    let search = app.textFields["logSearch"]
    XCTAssertTrue(search.waitForExistence(timeout: 3))
    search.click()
    search.typeText("no matching diagnostic")
    XCTAssertTrue(app.staticTexts["没有匹配的日志"].waitForExistence(timeout: 3))
    disclosure.click()
    XCTAssertFalse(search.isHittable)
    disclosure.click()
    XCTAssertTrue(search.isHittable)
    XCTAssertEqual(search.value as? String, "no matching diagnostic")
    app.buttons["重置筛选"].click()
    XCTAssertEqual(search.value as? String, "")
    XCTAssertTrue(app.buttons["copyLogsButton"].isEnabled)
  }

  func testHistoryNavigationRestoresSelectedResultAndKeyboardFocus() {
    continueAfterFailure = false
    let app = launchApp(realEngine: true)
    XCTAssertTrue(app.buttons["example-360"].waitForExistence(timeout: 4))
    app.buttons["example-360"].click()
    app.buttons["startRunButton"].click()
    XCTAssertTrue(app.staticTexts["factorResult"].waitForExistence(timeout: 10))
    app.buttons["newRunButton"].click()
    XCTAssertTrue(app.buttons["example-10403"].waitForExistence(timeout: 3))
    app.buttons["example-10403"].click()
    app.buttons["startRunButton"].click()
    XCTAssertTrue(app.staticTexts["factorResult"].waitForExistence(timeout: 10))
    let first = app.buttons["history-360"]
    first.click()
    XCTAssertTrue(app.staticTexts["numberDisplay"].isHittable)
    XCTAssertTrue(app.staticTexts["factorResult"].isHittable)
    XCTAssertTrue((app.staticTexts["factorResult"].value as? String ?? "").contains("2^3"))
    app.typeKey(.upArrow, modifierFlags: [])
    XCTAssertTrue((app.staticTexts["factorResult"].value as? String ?? "").contains("101"))
    app.typeKey(.downArrow, modifierFlags: [])
    XCTAssertTrue((app.staticTexts["factorResult"].value as? String ?? "").contains("2^3"))
  }

  func testReadingOlderLogsPausesFollowingAndLatestResumesIt() {
    continueAfterFailure = false
    let app = launchApp(streamLogs: true)
    XCTAssertTrue(app.buttons["startRunButton"].waitForExistence(timeout: 4))
    app.buttons["startRunButton"].click()
    XCTAssertTrue(app.staticTexts["relationsMetric"].waitForExistence(timeout: 5))
    let disclosure = app.buttons["toggleLogsButton"]
    disclosure.click()
    let console = app.scrollViews["logConsole"]
    XCTAssertTrue(console.waitForExistence(timeout: 3))
    app.scrollViews["workbenchDocument"].scroll(byDeltaX: 0, deltaY: -600)
    let following = app.checkBoxes["followLogsToggle"]
    XCTAssertTrue(following.exists)
    // Wait for actual rows rather than an arbitrary sleep before scrolling.
    XCTAssertTrue(
      console.staticTexts.containing(
        NSPredicate(format: "value CONTAINS 'Console scrolling fixture'")
      ).firstMatch.waitForExistence(timeout: 8))
    console.scroll(byDeltaX: 0, deltaY: 300)
    // Reading the toggle asks the app for its accessibility state, which can take
    // seconds on a busy runner. Both waits below allow for that delay.
    let paused = XCTNSPredicateExpectation(
      predicate: NSPredicate { _, _ in String(describing: following.value ?? "") == "0" },
      object: following)
    XCTAssertEqual(XCTWaiter.wait(for: [paused], timeout: 10), .completed)
    XCTAssertTrue(app.buttons["jumpToLatestLogsButton"].isHittable)
    disclosure.click()
    disclosure.click()
    XCTAssertEqual(String(describing: following.value ?? ""), "0")
    app.buttons["jumpToLatestLogsButton"].click()
    let resumed = XCTNSPredicateExpectation(
      predicate: NSPredicate { _, _ in String(describing: following.value ?? "") == "1" },
      object: following)
    XCTAssertEqual(XCTWaiter.wait(for: [resumed], timeout: 10), .completed)
    app.buttons["cancelRunButton"].click()
    XCTAssertTrue(app.staticTexts["任务已取消"].waitForExistence(timeout: 4))
  }

}
