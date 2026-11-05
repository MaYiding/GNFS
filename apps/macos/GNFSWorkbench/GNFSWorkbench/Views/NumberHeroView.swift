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
    VStack(alignment: .leading, spacing: run == nil ? 24 : 14) {
      if let run {
        HStack {
          Text("输入整数").font(.system(size: 11, weight: .medium)).foregroundStyle(.secondary)
          Text(inputDescription).font(.system(size: 11).monospacedDigit()).foregroundStyle(
            .tertiary)
          Spacer()
          Text(run.createdAt.formatted(date: .abbreviated, time: .shortened))
            .font(.system(size: 11)).foregroundStyle(.secondary)
          copyButton
        }
        Text(run.displayNumber)
          .font(.system(size: 18, weight: .medium, design: .monospaced))
          .textSelection(.enabled)
          .fixedSize(horizontal: false, vertical: true)
          .accessibilityIdentifier("numberDisplay")
        HStack(spacing: 16) {
          Label("请求 \(run.configuration.method.displayName)", systemImage: "cpu")
          Text(run.configuration.parameters.isAutomatic ? "自动参数" : "自定义参数")
        }
        .font(.system(size: 11)).foregroundStyle(.secondary)
      } else {
        VStack(alignment: .leading, spacing: 8) {
          Text("分解一个整数")
            .font(.system(size: 25, weight: .semibold)).tracking(-0.5)
          Text("完整质因数分解，在本机计算并精确校验。")
            .font(.system(size: 13)).foregroundStyle(.secondary)
        }
        .padding(.top, 14)

        VStack(alignment: .leading, spacing: 12) {
          HStack {
            Text("整数 N").font(.system(size: 12, weight: .medium))
            Spacer()
            Text(inputDescription).font(.system(size: 11).monospacedDigit()).foregroundStyle(
              .secondary)
            copyButton
          }
          TextField("例如 1000036000099", text: $configuration.number, axis: .vertical)
            .textFieldStyle(.plain)
            .font(.system(size: 22, weight: .regular, design: .monospaced))
            .lineLimit(1...5)
            .padding(18)
            .background(AppTheme.surface, in: RoundedRectangle(cornerRadius: 8))
            .overlay {
              RoundedRectangle(cornerRadius: 8)
                .stroke(
                  isNumberFocused ? AppTheme.indigo.opacity(0.65) : AppTheme.separatorStrong,
                  lineWidth: isNumberFocused ? 1.5 : 1)
            }
            .focused($isNumberFocused)
            .accessibilityLabel("待分解整数")
            .accessibilityHint("支持十进制与 0x 十六进制，按回车开始")
            .accessibilityIdentifier("numberInput")
            .onSubmit { if canSubmit { submit() } }
          if let validationMessage {
            Label(validationMessage, systemImage: "exclamationmark.circle")
              .font(.system(size: 12)).foregroundStyle(AppTheme.danger)
              .accessibilityIdentifier("validationMessage")
          } else {
            HStack {
              Text("十进制或 0x 十六进制，可粘贴含空格、逗号的数字。")
              Spacer()
              Text("⌘R 开始").monospaced()
            }
            .font(.system(size: 11)).foregroundStyle(.secondary)
          }
        }

        HStack(spacing: 8) {
          Text("示例").font(.system(size: 11)).foregroundStyle(.secondary)
          example("360", caption: "360 · 重复因子")
          example("10403", caption: "10403 · 半素数")
          example("1000036000099", caption: "较大整数")
          Spacer(minLength: 0)
        }
        Divider().padding(.vertical, 2)
        HStack(alignment: .top, spacing: 18) {
          VStack(alignment: .leading, spacing: 9) {
            Text("计算方法").font(.system(size: 12, weight: .medium))
            Picker("算法", selection: $configuration.method) {
              ForEach(FactorMethod.allCases) { Text($0.displayName).tag($0) }
            }
            .labelsHidden().frame(width: 150)
            .accessibilityIdentifier("methodPicker")
          }
          VStack(alignment: .leading, spacing: 9) {
            Text(configuration.parameters.isAutomatic ? "自动参数" : "自定义参数")
              .font(.system(size: 12, weight: .medium))
            Text(configuration.method.explanation)
              .font(.system(size: 12)).foregroundStyle(.secondary)
              .fixedSize(horizontal: false, vertical: true)
          }
          .frame(maxWidth: .infinity, alignment: .leading)
        }
      }
    }
    .frame(maxWidth: .infinity, alignment: .leading)
    .onAppear { isNumberFocused = run == nil }
    .onChange(of: run?.id) { isNumberFocused = run == nil }
  }

  private var copyButton: some View {
    Button {
      copy(run?.configuration.number ?? configuration.number)
    } label: {
      Image(systemName: "doc.on.doc").font(.system(size: 11))
    }
    .buttonStyle(.borderless).foregroundStyle(.secondary)
    .help("复制整数").accessibilityLabel("复制待分解整数")
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
    .buttonStyle(.bordered).controlSize(.small)
    .help(number).accessibilityIdentifier("example-\(number)")
  }
}
