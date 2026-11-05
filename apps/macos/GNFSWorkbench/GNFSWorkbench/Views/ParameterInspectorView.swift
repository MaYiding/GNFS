import SwiftUI

/// Retains raw text until the user applies a complete, valid configuration.
struct ParameterForm: Equatable {
  enum Field: String, CaseIterable, Identifiable {
    case rationalBound, algebraicBound, largePrimeBound, sieveWidth, sieveHeight
    var id: String { rawValue }
    var title: String {
      switch self {
      case .rationalBound: "有理因子基上界"
      case .algebraicBound: "代数因子基上界"
      case .largePrimeBound: "大素数上界"
      case .sieveWidth: "筛区宽度"
      case .sieveHeight: "筛区高度"
      }
    }
    var range: ClosedRange<UInt64> {
      switch self {
      case .rationalBound, .algebraicBound: 2...UInt64(UInt32.max)
      case .largePrimeBound: 2...UInt64.max
      case .sieveWidth: 16...UInt64(Int32.max)
      case .sieveHeight: 1...UInt64(Int32.max)
      }
    }
  }
  var number: String
  var method: FactorMethod
  var degree: Int?
  var values: [Field: String]

  init(configuration: RunConfiguration) {
    number = configuration.number
    method = configuration.method
    degree = configuration.parameters.degree
    let p = configuration.parameters
    values = [
      .rationalBound: p.rationalBound.map(String.init) ?? "",
      .algebraicBound: p.algebraicBound.map(String.init) ?? "",
      .largePrimeBound: p.largePrimeBound.map(String.init) ?? "",
      .sieveWidth: p.sieveWidth.map(String.init) ?? "",
      .sieveHeight: p.sieveHeight.map(String.init) ?? "",
    ]
  }

  func error(for field: Field) -> String? {
    let text = values[field, default: ""].trimmingCharacters(in: .whitespacesAndNewlines)
    guard !text.isEmpty else { return nil }
    guard text.utf8.allSatisfy({ (48...57).contains($0) }), let value = UInt64(text) else {
      return "请输入有效的正整数，或留空使用自动值。"
    }
    guard field.range.contains(value) else {
      return "范围：\(field.range.lowerBound) 至 \(field.range.upperBound)"
    }
    return nil
  }

  var isValid: Bool {
    Field.allCases.allSatisfy { error(for: $0) == nil }
      && (degree == nil || (3...8).contains(degree!))
  }

  var configuration: RunConfiguration? {
    guard isValid else { return nil }
    func value(_ field: Field) -> UInt64? {
      UInt64(values[field, default: ""].trimmingCharacters(in: .whitespacesAndNewlines))
    }
    return RunConfiguration(
      number: number, method: method,
      parameters: AdvancedParameters(
        degree: degree,
        rationalBound: value(.rationalBound).map(Int.init),
        algebraicBound: value(.algebraicBound).map(Int.init),
        largePrimeBound: value(.largePrimeBound),
        sieveWidth: value(.sieveWidth).map(Int.init),
        sieveHeight: value(.sieveHeight).map(Int.init)
      ))
  }

  mutating func reset() {
    degree = nil
    values = [:]
  }
}

struct ParameterEditorSheet: View {
  @Bindable var model: AppModel
  @Environment(\.dismiss) private var dismiss
  @State private var form: ParameterForm
  @State private var isEditing: Bool
  private let isCopy: Bool

  init(model: AppModel) {
    self.model = model
    let run = model.displayedRun
    _form = State(
      initialValue: ParameterForm(configuration: run?.configuration ?? model.draftConfiguration))
    _isEditing = State(initialValue: run == nil)
    isCopy = run != nil
  }

  var body: some View {
    VStack(spacing: 0) {
      VStack(alignment: .leading, spacing: 8) {
        Text(isEditing ? (isCopy ? "编辑参数副本" : "运行参数") : "本次请求参数")
          .font(.title2.weight(.semibold))
        Text(isEditing ? "留空使用自动值。应用后，下次运行会请求这些覆盖值。" : "这是启动时请求的配置。实际算法与生效参数以计算统计为准；编辑副本可新建任务。")
          .font(.callout).foregroundStyle(.secondary)
          .fixedSize(horizontal: false, vertical: true)
      }
      .frame(maxWidth: .infinity, alignment: .leading).padding(24)
      Divider()
      ParameterInspectorView(form: $form).disabled(!isEditing)
      Divider()
      HStack {
        if isEditing {
          Button("恢复自动参数") { form.reset() }
            .accessibilityIdentifier("resetParametersButton")
        }
        Spacer()
        Button(isEditing ? "取消" : "关闭") { dismiss() }
          .keyboardShortcut(.cancelAction)
        if isEditing {
          Button(isCopy ? "应用并新建" : "应用参数") {
            guard let configuration = form.configuration else { return }
            Task {
              if isCopy { await model.useConfigurationFromDisplayedRun() }
              guard model.canEdit else { return }
              model.draftConfiguration = configuration
              dismiss()
            }
          }
          .buttonStyle(.borderedProminent)
          .keyboardShortcut(.defaultAction)
          .disabled(!form.isValid || !model.canEdit)
          .accessibilityIdentifier("applyParametersButton")
        } else {
          Button("编辑副本") { isEditing = true }
            .buttonStyle(.borderedProminent)
            .disabled(!model.canEdit)
            .accessibilityIdentifier("editParameterCopyButton")
        }
      }
      .padding(20)
    }
    .frame(width: 480, height: 580)
    .background(AppTheme.canvas)
    .tint(AppTheme.indigo)
  }
}

struct ParameterInspectorView: View {
  @Binding var form: ParameterForm

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 22) {
        VStack(alignment: .leading, spacing: 10) {
          Text("分解算法").font(.headline)
          Picker("算法", selection: $form.method) {
            ForEach(FactorMethod.allCases) { Text($0.displayName).tag($0) }
          }
          .accessibilityIdentifier("parameterMethodPicker")
          Text(form.method.explanation).font(.callout).foregroundStyle(.secondary)
        }
        Divider()
        VStack(alignment: .leading, spacing: 12) {
          HStack {
            Text("高级参数").font(.headline)
            Spacer()
            Text("可选").font(.caption).foregroundStyle(.secondary)
          }
          Text("这些覆盖值主要用于 GNFS。自动模式及其他算法可能不使用部分参数。")
            .font(.callout).foregroundStyle(.secondary)
          Picker("多项式次数", selection: $form.degree) {
            Text("自动").tag(nil as Int?)
            ForEach(3...8, id: \.self) { Text("\($0)").tag(Optional($0)) }
          }
          .accessibilityIdentifier("degreePicker")
          ForEach(ParameterForm.Field.allCases) { field in
            VStack(alignment: .leading, spacing: 5) {
              HStack {
                Text(field.title).font(.callout)
                Spacer()
                TextField(
                  "自动",
                  text: Binding(
                    get: { form.values[field, default: ""] },
                    set: { form.values[field] = $0 }
                  )
                )
                .textFieldStyle(.roundedBorder)
                .multilineTextAlignment(.trailing)
                .frame(width: 180)
                .accessibilityLabel(field.title)
                .accessibilityIdentifier("\(field.rawValue)Field")
              }
              if let error = form.error(for: field) {
                Label(error, systemImage: "exclamationmark.circle")
                  .font(.caption).foregroundStyle(AppTheme.danger)
                  .accessibilityIdentifier("\(field.rawValue)Error")
              }
            }
          }
        }
        Label("所有结果均检查完整性、因子素性与精确乘积。", systemImage: "checkmark.shield")
          .font(.caption).foregroundStyle(.secondary)
      }
      .padding(24)
    }
    .background(AppTheme.elevated)
    .accessibilityIdentifier("parameterInspector")
  }
}
