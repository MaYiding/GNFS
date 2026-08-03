import AppKit
import SwiftUI
import XCTest

@testable import GNFSWorkbench

@MainActor
final class OffscreenRenderingTests: XCTestCase {
  func testLiveDashboardRendersOffscreenAtDesignViewport() throws {
    try render(
      makeLiveModel(),
      name: "01-live-dashboard-design-viewport",
      size: CGSize(width: 1_487, height: 1_058)
    )
  }

  func testReadyStateRendersOffscreenAtDefaultWindowSize() throws {
    try render(
      makeEmptyModel(),
      name: "02-ready-state-default-window",
      size: CGSize(width: 1_320, height: 940)
    )
  }

  func testCompletePrimeFactorizationRendersOffscreen() throws {
    try render(
      makeCompletedModel(),
      name: "03-complete-prime-factorization",
      size: CGSize(width: 1_320, height: 940)
    )
  }

  func testFailureStateAndLargeTextRenderAtMinimumWindowSize() throws {
    try render(
      makeFailureModel(),
      name: "04-failure-large-text-minimum-window",
      size: CGSize(width: 1_000, height: 740),
      dynamicTypeSize: .accessibility1
    )
  }

  func testHistoryAndParameterSurfacesRenderOffscreen() throws {
    let model = try makeCompletedModel()
    try renderView(
      HistoryView(model: model)
        .preferredColorScheme(.dark),
      name: "05-history-popover",
      size: CGSize(width: 390, height: 470)
    )

    try renderView(
      ParameterInspectorView(
        configuration: .constant(
          RunConfiguration(
            number: "1000036000099",
            method: .auto,
            parameters: .automatic
          )
        )
      )
      .preferredColorScheme(.dark),
      name: "06-parameter-inspector",
      size: CGSize(width: 340, height: 700)
    )
  }

  private func render(
    _ model: AppModel,
    name: String,
    size: CGSize,
    dynamicTypeSize: DynamicTypeSize = .large
  ) throws {
    try renderView(
      ContentView(model: model)
        .preferredColorScheme(.dark)
        .environment(\.dynamicTypeSize, dynamicTypeSize)
        .frame(width: size.width, height: size.height),
      name: name,
      size: size
    )
  }

  private func renderView<Content: View>(
    _ view: Content,
    name: String,
    size: CGSize
  ) throws {
    let hostingView = NSHostingView(rootView: view)
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

    let minimumEncodedBytes = max(20_000, Int(size.width * size.height / 10))
    XCTAssertGreaterThan(png.count, minimumEncodedBytes)
    let horizontalScale = Double(representation.pixelsWide) / size.width
    let verticalScale = Double(representation.pixelsHigh) / size.height
    XCTAssertGreaterThanOrEqual(horizontalScale, 1)
    XCTAssertEqual(horizontalScale, verticalScale, accuracy: 0.001)

    let attachment = XCTAttachment(data: png, uniformTypeIdentifier: "public.png")
    attachment.name = name
    attachment.lifetime = .keepAlways
    add(attachment)
  }

  private func makeEmptyModel() -> AppModel {
    AppModel(runner: PreviewIdleRunner(), store: makeStore())
  }

  private func makeLiveModel() -> AppModel {
    let model = makeEmptyModel()
    var run = RunRecord(
      configuration: RunConfiguration(
        number: "1000036000099",
        method: .gnfs,
        parameters: .automatic
      ))
    run.status = .running
    run.selectedMethod = .gnfs
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

  private func makeCompletedModel() throws -> AppModel {
    let model = makeEmptyModel()
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

  private func makeFailureModel() -> AppModel {
    let model = makeEmptyModel()
    var run = RunRecord(
      configuration: RunConfiguration(
        number:
          "1522605027922533360535618378132637429718068114961380688657908494580122963258952897654000350692006139",
        method: .gnfs,
        parameters: .automatic
      ))
    run.status = .failed
    run.selectedMethod = .gnfs
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
    #"{"success":true,"factorization_complete":true,"factors_prime":true,"n":"360","n_bits":9,"n_digits":3,"method":"trial","method_name":"Trial Division","method_reason":"complete prime factorization","factors":["2","2","2","3","3","5"],"timings":{"total_s":0.012,"poly_s":0,"fb_s":0,"sieve_s":0,"filter_s":0,"linalg_s":0,"sqrt_s":0,"extract_s":0},"stats":{"degree":0,"rational_bound":0,"algebraic_bound":0,"large_prime_bound":0,"rational_primes":0,"algebraic_primes":0,"special_q_processed":0,"candidates_total":0,"relations_found":0,"full_relations":0,"partial_1lp":0,"partial_2lp":0,"relations_after_filter":0,"singletons_removed":0,"merged_relations":0,"matrix_rows":0,"matrix_cols":0,"matrix_excess":0,"dependencies_found":0,"dependencies_tried":0}}"#
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
