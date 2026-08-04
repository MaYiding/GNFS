import XCTest

@MainActor
final class GNFSWorkbenchUITests: XCTestCase {
  private func launchApp() -> XCUIApplication {
    let app = XCUIApplication()
    app.launchArguments = ["--ui-testing"]
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
}
