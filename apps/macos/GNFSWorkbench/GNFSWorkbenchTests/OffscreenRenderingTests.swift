import AppKit
import SwiftUI
import XCTest

@testable import GNFSWorkbench

@MainActor
final class OffscreenRenderingTests: XCTestCase {
  func testLiveDashboardRendersOffscreenAtDesignViewport() async throws {
    try render(
      await makeLiveModel(),
      name: "01-live-dashboard-design-viewport",
      size: CGSize(width: 1_487, height: 1_058)
    )
  }

  func testReadyStateRendersOffscreenAtDefaultWindowSize() async throws {
    try render(
      await makeEmptyModel(),
      name: "02-ready-state-default-window",
      size: CGSize(width: 1_320, height: 940)
    )
  }

  func testCompletePrimeFactorizationRendersOffscreen() async throws {
    try render(
      await makeCompletedModel(),
      name: "03-complete-prime-factorization",
      size: CGSize(width: 1_320, height: 940)
    )
  }

  func testFailureStateAndLargeTextRenderAtMinimumWindowSize() async throws {
    try render(
      await makeFailureModel(),
      name: "04-failure-large-text-minimum-window",
      size: CGSize(width: 1_000, height: 740),
      dynamicTypeSize: .accessibility1
    )
  }

  func testHistoryAndParameterSurfacesRenderOffscreen() async throws {
    let model = try await makeCompletedModel()
    try renderView(
      HistoryView(model: model)
        .preferredColorScheme(.dark),
      name: "05-history-popover",
      size: CGSize(width: 390, height: 470)
    )

    try renderView(
      ParameterInspectorView(form: .constant(ParameterForm(configuration: RunConfiguration())))
        .preferredColorScheme(.dark),
      name: "06-parameter-inspector",
      size: CGSize(width: 480, height: 560)
    )
  }

  func testLightAppearanceAtMinimumWindowSize() async throws {
    try render(
      await makeEmptyModel(), name: "07-ready-light-minimum", size: CGSize(width: 900, height: 640),
      colorScheme: .light)
    try render(
      await makeLiveModel(), name: "08-live-light-minimum", size: CGSize(width: 900, height: 640),
      colorScheme: .light)
    try render(
      await makeCompletedModel(), name: "09-result-light-minimum",
      size: CGSize(width: 900, height: 900), colorScheme: .light)
  }

  func testLongInputAndInvalidParameterFormRender() async throws {
    let model = await makeEmptyModel()
    model.draftConfiguration.number = String(repeating: "1234567890", count: 10)
    try render(model, name: "10-long-integer", size: CGSize(width: 900, height: 740))
    var form = ParameterForm(configuration: RunConfiguration())
    form.values[.rationalBound] = "abc"
    form.values[.largePrimeBound] = "18446744073709551616"
    try renderView(
      ParameterInspectorView(form: .constant(form)).preferredColorScheme(.light),
      name: "11-invalid-parameters", size: CGSize(width: 480, height: 560), colorScheme: .light)
  }

  func testCompleteParameterSheetFitsShortWindow() async throws {
    let model = await makeEmptyModel()
    try renderView(
      ParameterEditorSheet(model: model), name: "12-parameter-sheet",
      size: CGSize(width: 480, height: 580), colorScheme: .light)
  }

  func testGNFSStatisticsAndEffectiveParametersRender() async throws {
    let model = try await makeCompletedModel()
    let baseline = try XCTUnwrap(model.activeRun?.result)
    var object = try XCTUnwrap(
      JSONSerialization.jsonObject(with: JSONEncoder().encode(baseline)) as? [String: Any])
    var stats = try XCTUnwrap(object["stats"] as? [String: Any])
    stats.merge([
      "executed_algorithm": "gnfs", "algorithms_executed": ["trial", "rho", "gnfs"],
      "gnfs_parameters_applied": true, "degree": 3, "rational_bound": 5000,
      "algebraic_bound": 10000, "large_prime_bound": 131072, "sieve_width": 513,
      "sieve_height": 128, "max_special_q": 10000, "relations_found": 15824,
      "full_relations": 15000, "partial_1lp": 800, "partial_2lp": 24,
      "relations_after_filter": 14000, "matrix_rows": 14000, "matrix_cols": 13600,
      "matrix_weight": 82540, "matrix_excess": 400, "dependencies_found": 12,
      "dependencies_tried": 3, "sieve_rounds_completed": 2,
    ]) { _, new in new }
    object["stats"] = stats
    object["method"] = "gnfs"
    let result = try JSONDecoder().decode(
      FactorizationResult.self, from: JSONSerialization.data(withJSONObject: object))
    try renderView(
      RunStatisticsView(result: result).padding(20)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading).background(
          AppTheme.canvas),
      name: "13-gnfs-statistics-light", size: CGSize(width: 1000, height: 600), colorScheme: .light)
    try renderView(
      RunStatisticsView(result: result).padding(20)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading).background(
          AppTheme.canvas),
      name: "14-gnfs-statistics-dark", size: CGSize(width: 640, height: 850), colorScheme: .dark)
  }

  private func render(
    _ model: AppModel,
    name: String,
    size: CGSize,
    dynamicTypeSize: DynamicTypeSize = .large,
    colorScheme: ColorScheme = .dark
  ) throws {
    try renderView(
      ContentView(model: model)
        .environment(\.colorScheme, colorScheme)
        .preferredColorScheme(colorScheme)
        .environment(\.dynamicTypeSize, dynamicTypeSize)
        .frame(width: size.width, height: size.height),
      name: name,
      size: size,
      colorScheme: colorScheme
    )
  }

  private func renderView<Content: View>(
    _ view: Content,
    name: String,
    size: CGSize,
    colorScheme: ColorScheme = .dark
  ) throws {
    let hostingView = NSHostingView(rootView: view.environment(\.colorScheme, colorScheme))
    hostingView.appearance = NSAppearance(named: colorScheme == .dark ? .darkAqua : .aqua)
    hostingView.frame = NSRect(origin: .zero, size: size)
    hostingView.layoutSubtreeIfNeeded()

    guard let representation = hostingView.bitmapImageRepForCachingDisplay(in: hostingView.bounds)
    else {
      XCTFail("Unable to allocate offscreen bitmap")
      return
    }
    hostingView.cacheDisplay(in: hostingView.bounds, to: representation)
    guard let png = representation.representation(using: .png, properties: [:]) else {
      XCTFail("Unable to encode offscreen bitmap")
      return
    }

    let horizontalScale = Double(representation.pixelsWide) / size.width
    let verticalScale = Double(representation.pixelsHigh) / size.height
    XCTAssertGreaterThanOrEqual(horizontalScale, 1)
    XCTAssertEqual(horizontalScale, verticalScale, accuracy: 0.001)
    assertRenderedPixelsAreMeaningful(representation)

    let attachment = XCTAttachment(data: png, uniformTypeIdentifier: "public.png")
    attachment.name = name
    attachment.lifetime = .keepAlways
    add(attachment)
  }

  private func assertRenderedPixelsAreMeaningful(_ representation: NSBitmapImageRep) {
    let sampleColumns = 32
    let sampleRows = 24
    var sampledColors: Set<UInt32> = []
    var visibleSamples = 0
    var minimumLuminance = Double.greatestFiniteMagnitude
    var maximumLuminance = -Double.greatestFiniteMagnitude

    for row in 0..<sampleRows {
      let y = min(
        representation.pixelsHigh - 1,
        (row * representation.pixelsHigh + representation.pixelsHigh / 2) / sampleRows
      )
      for column in 0..<sampleColumns {
        let x = min(
          representation.pixelsWide - 1,
          (column * representation.pixelsWide + representation.pixelsWide / 2) / sampleColumns
        )
        guard let color = representation.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB)
        else { continue }
        if color.alphaComponent > 0.05 { visibleSamples += 1 }
        let red = min(max(color.redComponent, 0), 1)
        let green = min(max(color.greenComponent, 0), 1)
        let blue = min(max(color.blueComponent, 0), 1)
        let quantized = UInt32(red * 15) << 8 | UInt32(green * 15) << 4 | UInt32(blue * 15)
        sampledColors.insert(quantized)
        let luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue
        minimumLuminance = min(minimumLuminance, luminance)
        maximumLuminance = max(maximumLuminance, luminance)
      }
    }

    let sampleCount = sampleColumns * sampleRows
    XCTAssertGreaterThan(visibleSamples, sampleCount * 9 / 10)
    // A monochrome statistics panel is valid; require foreground structure, not a fixed palette.
    XCTAssertGreaterThan(sampledColors.count, 2)
    XCTAssertGreaterThan(maximumLuminance - minimumLuminance, 0.08)
  }

  private func makeEmptyModel() async -> AppModel {
    let model = AppModel(runner: PreviewIdleRunner(), store: makeStore())
    let deadline = ContinuousClock.now.advanced(by: .seconds(2))
    while model.historyRestorationState == .loading, ContinuousClock.now < deadline {
      try? await Task.sleep(for: .milliseconds(5))
    }
    XCTAssertEqual(model.historyRestorationState, .ready)
    return model
  }

  private func makeLiveModel() async -> AppModel {
    let model = await makeEmptyModel()
    var run = RunRecord(
      configuration: RunConfiguration(
        number: "1000036000099",
        method: .gnfs,
        parameters: .automatic
      ))
    run.status = .running
    run.selectedMethod = .gnfs
    run.activeAlgorithm = "gnfs"
    run.attemptIndex = 1
    run.currentNumber = run.configuration.number
    run.resolvedParameters = EffectiveGNFSParameters(
      degree: 3, rationalBound: 5000,
      algebraicBound: 10000, largePrimeBound: 131072, sieveWidth: 512, sieveHeight: 128,
      maxSpecialQ: 10000)
    run.methodReason = "完整一般数域筛流水线"
    run.phase = .sieving
    run.phaseProgress = 0.86
    run.elapsed = 4.67
    run.relationsFound = 15_824
    run.relationsTarget = 18_400
    run.specialQDone = 5_748
    run.samples = [
      .init(elapsed: 1.42, relations: 3_312, rate: 2_332),
      .init(elapsed: 2.07, relations: 5_704, rate: 2_938),
      .init(elapsed: 2.72, relations: 8_648, rate: 3_655),
      .init(elapsed: 3.37, relations: 11_592, rate: 4_049),
      .init(elapsed: 4.02, relations: 14_352, rate: 4_137),
      .init(elapsed: 4.67, relations: 15_824, rate: 3_297),
    ]
    run.logs = previewLogs
    install(run, in: model)
    return model
  }

  private func makeCompletedModel() async throws -> AppModel {
    let model = await makeEmptyModel()
    var run = RunRecord(
      configuration: RunConfiguration(
        number: "360",
        method: .auto,
        parameters: .automatic
      ))
    run.status = .succeeded
    run.selectedMethod = .trial
    run.methodReason = "完整质因数分解"
    run.phase = .done
    run.phaseProgress = 1
    run.elapsed = 0.012
    run.logs =
      previewLogs + [
        .init(
          timestamp: 0.012,
          severity: .info,
          phase: .done,
          message: "完整质因数分解已验证，共 6 个质因数"
        )
      ]
    run.result = try JSONDecoder().decode(
      FactorizationResult.self,
      from: Data(Self.completedResultJSON.utf8)
    )
    install(run, in: model)
    return model
  }

  private func makeFailureModel() async -> AppModel {
    let model = await makeEmptyModel()
    var run = RunRecord(
      configuration: RunConfiguration(
        number:
          "1522605027922533360535618378132637429718068114961380688657908494580122963258952897654000350692006139",
        method: .gnfs,
        parameters: .automatic
      ))
    run.status = .failed
    run.selectedMethod = .gnfs
    run.activeAlgorithm = "gnfs"
    run.phase = .sieving
    run.elapsed = 83.42
    run.errorMessage = "关系数量不足，无法构造可求解矩阵。"
    run.logs =
      previewLogs + [
        .init(
          timestamp: 83.42,
          severity: .error,
          phase: .sieving,
          message: "可用关系不足；请调整参数后重试"
        )
      ]
    install(run, in: model)
    return model
  }

  private func install(_ run: RunRecord, in model: AppModel) {
    model.activeRun = run
    model.history = [run]
    model.selectedRunID = run.id
  }

  private var previewLogs: [RunLogEntry] {
    [
      .init(timestamp: 0.08, severity: .info, phase: .polynomial, message: "正在搜索低范数多项式"),
      .init(timestamp: 0.88, severity: .info, phase: .sieving, message: "格筛已启动，目标 18,400 条关系"),
      .init(timestamp: 4.31, severity: .debug, phase: .sieving, message: "已提交第 5,600 个 special-q"),
    ]
  }

  private func makeStore() -> RunStore {
    RunStore(
      baseURL: FileManager.default.temporaryDirectory
        .appendingPathComponent(UUID().uuidString, isDirectory: true)
    )
  }

  private static let completedResultJSON =
    #"{"success":true,"factorization_complete":true,"factors_prime":true,"n":"360","n_bits":9,"n_digits":3,"method":"trial","method_name":"Trial Division","method_reason":"complete prime factorization","factors":["2","2","2","3","3","5"],"timings":{"non_gnfs_s":0.012,"total_s":0.012,"poly_s":0,"fb_s":0,"sieve_s":0,"filter_s":0,"linalg_s":0,"sqrt_s":0,"extract_s":0},"stats":{"executed_algorithm":"trial","algorithms_executed":["trial"],"gnfs_parameters_applied":false,"degree":0,"rational_bound":0,"algebraic_bound":0,"large_prime_bound":0,"rational_primes":0,"algebraic_primes":0,"special_q_processed":0,"candidates_total":0,"relations_found":0,"full_relations":0,"partial_1lp":0,"partial_2lp":0,"relations_after_filter":0,"singletons_removed":0,"merged_relations":0,"matrix_rows":0,"matrix_cols":0,"matrix_excess":0,"dependencies_found":0,"dependencies_tried":0}}"#
}

private actor PreviewIdleRunner: GNFSRunning {
  func start(
    configuration: RunConfiguration,
    workspace: RunWorkspace
  ) async throws -> AsyncThrowingStream<CLIEvent, Error> {
    AsyncThrowingStream { _ in }
  }

  func cancel() async {}
}
