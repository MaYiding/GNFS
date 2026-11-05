import SwiftUI

struct NumberHeroView: View {
  @Binding var configuration: RunConfiguration
  let run: RunRecord?
  let validationMessage: String?
  let canSubmit: Bool
  let submit: () -> Void
  let copy: (String) -> Void
  @FocusState private var isNumberFocused: Bool

  var body: some View {
    VStack(alignment: .leading, spacing: run == nil ? 18 : 10) {
      HStack {
        VStack(alignment: .leading, spacing: 6) {
          Text(run == nil ? "从一个整数开始" : "分解任务")
            .font(run == nil ? .title2.weight(.semibold) : .headline)
          if run == nil {
            Text("输入 N，得到经过校验的完整质因数分解。")
              .font(.subheadline).foregroundStyle(.secondary)
          }
        }
        Spacer()
        if let run {
          Text(run.createdAt.formatted(date: .abbreviated, time: .shortened))
            .font(.caption).foregroundStyle(.secondary)
        }
      }
      VStack(alignment: .leading, spacing: 10) {
        HStack {
          Text("待分解整数 N").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
          Spacer()
          Text(inputDescription).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
          Button {
            copy(run?.configuration.number ?? configuration.number)
          } label: {
            Image(systemName: "doc.on.doc")
          }
          .buttonStyle(.borderless).help("复制整数")
          .accessibilityLabel("复制待分解整数")
        }
        if let run {
          Text(run.displayNumber)
            .font(.system(size: 20, weight: .medium, design: .monospaced))
            .textSelection(.enabled)
            .fixedSize(horizontal: false, vertical: true)
            .accessibilityIdentifier("numberDisplay")
        } else {
          TextField("例如 1000036000099", text: $configuration.number, axis: .vertical)
            .textFieldStyle(.plain)
            .font(.system(size: 28, weight: .medium, design: .monospaced))
            .lineLimit(2...5)
            .focused($isNumberFocused)
            .accessibilityLabel("待分解整数")
            .accessibilityHint("支持十进制与 0x 十六进制，按回车开始")
            .accessibilityIdentifier("numberInput")
            .onSubmit { if canSubmit { submit() } }
        }
      }
      .padding(run == nil ? 18 : 10)
      .frame(maxWidth: .infinity, alignment: .leading)
      .background(AppTheme.canvas, in: RoundedRectangle(cornerRadius: 10))
      .overlay {
        RoundedRectangle(cornerRadius: 10)
          .stroke(isNumberFocused ? AppTheme.indigo : AppTheme.separatorStrong, lineWidth: 1)
      }

      if run == nil {
        HStack(spacing: 10) {
          Text("试一试").font(.caption).foregroundStyle(.secondary)
          example("360", caption: "重复因子")
          example("10403", caption: "小半素数")
          example("1000036000099", caption: "较大整数")
          Spacer(minLength: 0)
        }
        HStack(alignment: .top, spacing: 12) {
          Picker("算法", selection: $configuration.method) {
            ForEach(FactorMethod.allCases) { Text($0.displayName).tag($0) }
          }
          .frame(width: 180)
          .accessibilityIdentifier("methodPicker")
          Text(configuration.method.explanation)
            .font(.callout).foregroundStyle(.secondary)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        if let validationMessage {
          Label(validationMessage, systemImage: "exclamationmark.circle")
            .font(.callout).foregroundStyle(AppTheme.danger)
            .accessibilityIdentifier("validationMessage")
        } else {
          Text("支持十进制、0x 十六进制，以及带空格或逗号的数字。回车或 ⌘R 开始。")
            .font(.caption).foregroundStyle(.secondary)
        }
      } else if let run {
        HStack(spacing: 12) {
          Label(
            run.result?.executionLabel
              ?? AlgorithmDisplay.name(run.activeAlgorithm, fallback: "准备计算"),
            systemImage: "cpu")
          Text(run.configuration.parameters.isAutomatic ? "请求自动参数" : "请求自定义参数")
          Spacer()
          Label("完整分解", systemImage: "checkmark.shield")
        }
        .font(.callout).foregroundStyle(.secondary)
      }
    }
    .workbenchPanel()
    .onAppear { isNumberFocused = run == nil }
    .onChange(of: run?.id) { isNumberFocused = run == nil }
  }

  private var inputDescription: String {
    let input = run?.configuration.number ?? configuration.number
    guard case .valid(let value) = IntegerInputValidator.validate(input) else { return "" }
    return value.hasPrefix("0x") ? "\(value.count - 2) 位十六进制" : "\(value.count) 位十进制"
  }

  private func example(_ number: String, caption: String) -> some View {
    Button(caption) {
      configuration.number = number
      isNumberFocused = true
    }
    .buttonStyle(.bordered)
    .controlSize(.small)
    .help(number)
    .accessibilityIdentifier("example-\(number)")
  }
}
