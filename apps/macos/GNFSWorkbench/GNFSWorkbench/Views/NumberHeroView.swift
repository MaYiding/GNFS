import AppKit
import SwiftUI

struct NumberHeroView: View {
  @Binding var configuration: RunConfiguration
  let run: RunRecord?
  let validationMessage: String?
  let canSubmit: Bool
  let submit: () -> Void
  let copy: (String) -> Void
  @Environment(\.workbenchReduceMotion) private var reduceMotion
  @FocusState private var isNumberFocused: Bool

  var body: some View {
    VStack(alignment: .leading, spacing: run == nil ? 24 : 14) {
      if let run {
        recordedNumber(run)
      } else {
        VStack(alignment: .leading, spacing: 8) {
          Text("分解一个整数")
            .font(.system(size: 25, weight: .semibold)).tracking(-0.5)
          Text("完整质因数分解，在本机计算并精确校验。")
            .font(.system(size: 13)).foregroundStyle(.secondary)
        }
        .padding(.top, 14)
        numberEditor
        ViewThatFits(in: .horizontal) {
          HStack(spacing: 8) {
            Text("示例").font(.caption).foregroundStyle(.secondary)
            examples
          }
          VStack(alignment: .leading, spacing: 8) {
            Text("示例").font(.caption).foregroundStyle(.secondary)
            HStack(spacing: 8) { examples }
          }
        }
        Divider()
        HStack(alignment: .top, spacing: 24) {
          VStack(alignment: .leading, spacing: 10) {
            Text("计算方法").font(.callout.weight(.medium))
            Picker("算法", selection: $configuration.method) {
              ForEach(FactorMethod.allCases) { Text($0.displayName).tag($0) }
            }
            .labelsHidden().pickerStyle(.menu).frame(width: 150)
            .accessibilityIdentifier("methodPicker")
          }
          VStack(alignment: .leading, spacing: 10) {
            Text(configuration.parameters.isAutomatic ? "自动参数" : "自定义参数")
              .font(.callout.weight(.medium))
            Text(configuration.method.explanation)
              .font(.callout).foregroundStyle(.secondary)
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

  private var numberEditor: some View {
    VStack(alignment: .leading, spacing: 10) {
      HStack(alignment: .firstTextBaseline) {
        Text("整数 N").font(.callout.weight(.medium))
        Spacer()
        Text(inputDescription).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
        copyButton
        Button {
          configuration.number = ""
          isNumberFocused = true
        } label: {
          Image(systemName: "xmark.circle")
        }
        .buttonStyle(WorkbenchActionStyle(.quiet))
        .disabled(configuration.number.isEmpty)
        .help("清空输入").accessibilityLabel("清空整数")
        .accessibilityIdentifier("clearNumberButton")
      }
      TextField("例如 1000036000099", text: $configuration.number, axis: .vertical)
        .textFieldStyle(.plain)
        .font(.system(size: 22, weight: .regular, design: .monospaced))
        .lineLimit(1...5)
        .padding(.horizontal, 16).padding(.vertical, 14)
        .background(AppTheme.inputSurface, in: RoundedRectangle(cornerRadius: 8))
        .overlay {
          RoundedRectangle(cornerRadius: 8)
            .stroke(inputBorder, lineWidth: isNumberFocused ? 1.5 : 1)
            .animation(reduceMotion ? nil : WorkbenchMotion.feedback, value: isNumberFocused)
        }
        .focused($isNumberFocused)
        .accessibilityLabel("待分解整数")
        .accessibilityHint("支持十进制与 0x 十六进制，按回车开始，Shift 回车换行")
        .accessibilityIdentifier("numberInput")
        .onSubmit(submitIfValid)
        .onKeyPress(.return, phases: .down) { key in
          // Text composition and modified Return retain their native behavior.
          guard key.modifiers.isEmpty else { return .ignored }
          if let editor = NSApp.keyWindow?.firstResponder as? NSTextView,
            editor.hasMarkedText()
          {
            return .ignored
          }
          submitIfValid()
          return .handled
        }
      HStack(alignment: .firstTextBaseline, spacing: 12) {
        if let validationMessage {
          Label(validationMessage, systemImage: "exclamationmark.circle")
            .foregroundStyle(AppTheme.danger)
            .fixedSize(horizontal: false, vertical: true)
            .accessibilityIdentifier("validationMessage")
        } else {
          Text("十进制或 0x 十六进制，可含空格、逗号。")
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        }
        Spacer(minLength: 0)
        Text("↵ 开始 · ⌘R")
          .monospaced().foregroundStyle(canSubmit ? .secondary : .tertiary)
          .fixedSize()
          .accessibilityLabel("按回车或 Command R 开始")
      }
      .font(.caption)
      .frame(minHeight: 20, alignment: .topLeading)
    }
  }

  private var inputBorder: Color {
    if validationMessage != nil { return AppTheme.danger.opacity(0.7) }
    return isNumberFocused ? AppTheme.indigo.opacity(0.75) : AppTheme.separatorStrong
  }

  private func recordedNumber(_ run: RunRecord) -> some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack(alignment: .firstTextBaseline, spacing: 12) {
        Text("输入整数").font(.caption.weight(.medium)).foregroundStyle(.secondary)
        Text(inputDescription).font(.caption.monospacedDigit()).foregroundStyle(.tertiary)
        Spacer()
        Text(run.createdAt.formatted(date: .abbreviated, time: .shortened))
          .font(.caption).foregroundStyle(.secondary)
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
        Spacer(minLength: 0)
        Label("只读记录", systemImage: "lock")
      }
      .font(.caption).foregroundStyle(.secondary)
    }
  }

  private var copyButton: some View {
    Button {
      copy(run?.configuration.number ?? configuration.number)
    } label: {
      Image(systemName: "doc.on.doc")
    }
    .buttonStyle(WorkbenchActionStyle(.quiet))
    .disabled((run?.configuration.number ?? configuration.number).isEmpty)
    .help("复制整数").accessibilityLabel("复制待分解整数")
  }

  private var inputDescription: String {
    let input = run?.configuration.number ?? configuration.number
    guard case .valid(let value) = IntegerInputValidator.validate(input) else { return "" }
    return value.hasPrefix("0x") ? "\(value.count - 2) 位十六进制" : "\(value.count) 位十进制"
  }

  @ViewBuilder private var examples: some View {
    example("360", caption: "360 · 重复因子")
    example("10403", caption: "10403 · 半素数")
    example("1000036000099", caption: "较大整数")
  }

  private func example(_ number: String, caption: String) -> some View {
    Button(caption) {
      configuration.number = number
      isNumberFocused = true
    }
    .buttonStyle(WorkbenchActionStyle(.secondary))
    .help(number).accessibilityIdentifier("example-\(number)")
  }

  private func submitIfValid() {
    guard canSubmit else { return }
    submit()
  }
}
