import XCTest

@MainActor
final class GNFSWorkbenchUITests: XCTestCase {
  private func launchApp(realEngine: Bool = false) -> XCUIApplication {
    let app = XCUIApplication()
    app.launchArguments = ["--ui-testing"]
    if realEngine { app.launchArguments.append("--ui-testing-real-engine") }
    app.launch()
    return app
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
    XCTAssertTrue(app.staticTexts["algorithmRoute"].exists)
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

}
