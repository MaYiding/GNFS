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
  @Environment(\.workbenchReduceMotion) private var reduceMotion
  @State private var form: ParameterForm
  @State private var isEditing: Bool
  @State private var isApplying = false
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
      VStack(alignment: .leading, spacing: 9) {
        HStack(alignment: .firstTextBaseline) {
          Text(isEditing ? (isCopy ? "编辑参数副本" : "运行参数") : "本次请求参数")
            .font(.title2.weight(.semibold))
          Spacer()
          if !isEditing {
            Label("只读", systemImage: "lock")
              .font(.caption).foregroundStyle(.secondary)
          }
        }
        Text(
          isEditing
            ? "留空使用自动值。更改仅在应用后用于下一次运行。"
            : "这是启动时请求的配置，实际生效值请查看计算统计。"
        )
        .font(.callout).foregroundStyle(.secondary)
        .fixedSize(horizontal: false, vertical: true)
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      .padding(24)
      Divider()
      ParameterInspectorView(form: $form, isEditing: isEditing && !isApplying)
        .onSubmit(applyParameters)
      Divider()
      VStack(alignment: .leading, spacing: 12) {
        HStack(spacing: 12) {
          if isEditing {
            Button("恢复自动参数") { form.reset() }
              .buttonStyle(WorkbenchActionStyle(.quiet))
              .disabled(isApplying)
              .accessibilityIdentifier("resetParametersButton")
          } else if !model.canEdit {
            Text("当前任务结束后可编辑副本。")
              .font(.caption).foregroundStyle(.secondary)
          }
          Spacer(minLength: 8)
          Button(isEditing ? "取消" : "关闭") { dismiss() }
            .keyboardShortcut(.cancelAction)
            .disabled(isApplying)
          if isEditing {
            Button(isApplying ? "正在应用…" : (isCopy ? "应用并新建" : "应用参数")) {
              applyParameters()
            }
            .buttonStyle(.borderedProminent)
            .keyboardShortcut(.defaultAction)
            .disabled(!form.isValid || !model.canEdit || isApplying)
            .accessibilityIdentifier("applyParametersButton")
          } else {
            Button("编辑副本") {
              withAnimation(reduceMotion ? nil : WorkbenchMotion.disclosure) {
                isEditing = true
              }
            }
            .buttonStyle(.borderedProminent)
            .disabled(!model.canEdit)
            .accessibilityIdentifier("editParameterCopyButton")
          }
        }
        if isEditing && !form.isValid {
          Label("请先修正标记的参数。", systemImage: "exclamationmark.circle")
            .font(.caption).foregroundStyle(AppTheme.danger)
        }
      }
      .padding(.horizontal, 24).padding(.vertical, 18)
    }
    .frame(width: 560, height: 580)
    .background(AppTheme.canvas)
    .tint(AppTheme.indigo)
  }

  private func applyParameters() {
    guard isEditing, !isApplying, model.canEdit, let configuration = form.configuration else {
      return
    }
    isApplying = true
    Task {
      if isCopy { await model.useConfigurationFromDisplayedRun() }
      guard model.canEdit else {
        isApplying = false
        return
      }
      model.draftConfiguration = configuration
      dismiss()
    }
  }
}

struct ParameterInspectorView: View {
  @Binding var form: ParameterForm
  var isEditing = true
  @Environment(\.workbenchReduceMotion) private var reduceMotion
  @FocusState private var focusedField: ParameterForm.Field?

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 24) {
        VStack(alignment: .leading, spacing: 10) {
          HStack(alignment: .firstTextBaseline, spacing: 24) {
            Text("分解算法").font(.callout.weight(.semibold))
            Spacer()
            if isEditing {
              Picker("算法", selection: $form.method) {
                ForEach(FactorMethod.allCases) { Text($0.displayName).tag($0) }
              }
              .labelsHidden().pickerStyle(.menu).frame(width: 224)
              .accessibilityIdentifier("parameterMethodPicker")
            } else {
              Text(form.method.displayName)
                .font(.callout).textSelection(.enabled)
                .accessibilityIdentifier("parameterMethodPicker")
            }
          }
          Text(form.method.explanation)
            .font(.caption).foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        }
        Divider()
        VStack(alignment: .leading, spacing: 16) {
          HStack(alignment: .firstTextBaseline) {
            Text("GNFS 高级参数").font(.callout.weight(.semibold))
            Spacer()
            Text(overrideCount == 0 ? "全部自动" : "\(overrideCount) 项自定义")
              .font(.caption).foregroundStyle(.secondary)
          }
          Text("仅在 GNFS 路径生效。留空由引擎选择，不代表实际生效值。")
            .font(.caption).foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
          HStack(alignment: .firstTextBaseline, spacing: 24) {
            Text("多项式次数").font(.callout)
            Spacer()
            if isEditing {
              Picker("多项式次数", selection: $form.degree) {
                Text("自动").tag(nil as Int?)
                ForEach(3...8, id: \.self) { Text("\($0)").tag(Optional($0)) }
              }
              .labelsHidden().pickerStyle(.menu).frame(width: 224)
              .accessibilityIdentifier("degreePicker")
            } else {
              Text(form.degree.map(String.init) ?? "自动")
                .font(.callout.monospacedDigit())
                .foregroundStyle(form.degree == nil ? .secondary : .primary)
                .textSelection(.enabled)
                .accessibilityIdentifier("degreePicker")
            }
          }
          VStack(spacing: 14) {
            ForEach(ParameterForm.Field.allCases) { field in
              parameterRow(field)
            }
          }
        }
      }
      .padding(24)
    }
    .background(AppTheme.canvas)
    .accessibilityIdentifier("parameterInspector")
    .onChange(of: isEditing) {
      focusedField = isEditing ? .rationalBound : nil
    }
  }

  private var overrideCount: Int {
    (form.degree == nil ? 0 : 1) + ParameterForm.Field.allCases.filter {
      !form.values[$0, default: ""].trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }.count
  }

  private func parameterRow(_ field: ParameterForm.Field) -> some View {
    let rawValue = form.values[field, default: ""]
    let isAutomatic = rawValue.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    let error = form.error(for: field)
    return VStack(alignment: .leading, spacing: 5) {
      HStack(alignment: .firstTextBaseline, spacing: 24) {
        Text(field.title).font(.callout)
        Spacer(minLength: 0)
        if isEditing {
          HStack(spacing: 6) {
            TextField(
              "自动",
              text: Binding(
                get: { form.values[field, default: ""] },
                set: { form.values[field] = $0 }
              )
            )
            .textFieldStyle(.plain)
            .font(.system(size: 12, design: .monospaced))
            .multilineTextAlignment(.trailing)
            .padding(.horizontal, 9).padding(.vertical, 7)
            .background(AppTheme.inputSurface, in: RoundedRectangle(cornerRadius: 6))
            .overlay {
              RoundedRectangle(cornerRadius: 6)
                .stroke(
                  error != nil ? AppTheme.danger.opacity(0.7)
                    : (focusedField == field ? AppTheme.indigo.opacity(0.75) : AppTheme.separatorStrong),
                  lineWidth: focusedField == field ? 1.5 : 1
                )
                .animation(reduceMotion ? nil : WorkbenchMotion.feedback, value: focusedField)
            }
            .focused($focusedField, equals: field)
            .accessibilityLabel(field.title)
            .accessibilityHint("留空使用自动值，输入正整数可覆盖默认值")
            .accessibilityIdentifier("\(field.rawValue)Field")
            Button("自动") {
              form.values[field] = ""
              focusedField = field
            }
            .buttonStyle(WorkbenchActionStyle(.quiet))
            .disabled(isAutomatic)
            .help("将\(field.title)恢复为自动")
            .accessibilityLabel("将\(field.title)恢复自动")
            .accessibilityIdentifier("\(field.rawValue)AutomaticButton")
          }
          .frame(width: 224)
        } else {
          Text(isAutomatic ? "自动" : rawValue)
            .font(.callout.monospacedDigit())
            .foregroundStyle(isAutomatic ? .secondary : .primary)
            .textSelection(.enabled)
            .accessibilityIdentifier("\(field.rawValue)Field")
        }
      }
      Group {
        if isEditing, let error {
          Label(error, systemImage: "exclamationmark.circle")
            .foregroundStyle(AppTheme.danger)
            .accessibilityIdentifier("\(field.rawValue)Error")
        } else {
          Text(rangeHint(field)).foregroundStyle(.secondary)
        }
      }
      .font(.caption)
      .fixedSize(horizontal: false, vertical: true)
      .frame(minHeight: 16, alignment: .leading)
    }
  }

  private func rangeHint(_ field: ParameterForm.Field) -> String {
    let lower = field.range.lowerBound.formatted(.number.grouping(.never))
    let upper = field.range.upperBound.formatted(.number.grouping(.never))
    return "范围 \(lower)–\(upper)"
  }
}
