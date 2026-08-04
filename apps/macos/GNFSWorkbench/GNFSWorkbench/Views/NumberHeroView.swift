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
    VStack(alignment: .leading, spacing: 15) {
      Text("要分解的整数 N")
        .font(.system(size: 13, weight: .semibold))
        .tracking(0.7)
        .foregroundStyle(AppTheme.secondaryText)

      if let run {
        HStack(spacing: 12) {
          Text(run.displayNumber)
            .font(.gnfsMonospaced(size: 50, weight: .medium))
            .foregroundStyle(AppTheme.primaryText)
            .lineLimit(1)
            .minimumScaleFactor(0.42)
            .textSelection(.enabled)
            .accessibilityIdentifier("numberDisplay")
          Spacer(minLength: 8)
          copyButton(run.configuration.number)
        }
      } else {
        HStack(spacing: 12) {
          TextField("例如 1000036000099", text: $configuration.number)
            .textFieldStyle(.plain)
            .font(.gnfsMonospaced(size: 50, weight: .medium))
            .foregroundStyle(AppTheme.primaryText)
            .focused($isNumberFocused)
            .accessibilityLabel("待分解整数")
            .accessibilityHint("支持十进制或以 0x 开头的十六进制整数，按回车开始")
            .accessibilityIdentifier("numberInput")
            .onSubmit {
              if canSubmit { submit() }
            }
          copyButton(configuration.number)
        }
      }

      Rectangle()
        .fill(AppTheme.separatorStrong)
        .frame(height: 1)

      HStack(spacing: 11) {
        Image(systemName: "checkmark.circle.fill")
          .font(.system(size: 16, weight: .semibold))
          .foregroundStyle(AppTheme.indigo)

        Text("完整质因数分解")
          .font(.system(size: 13, weight: .semibold))
          .foregroundStyle(AppTheme.primaryText)

        Rectangle()
          .fill(AppTheme.separatorStrong)
          .frame(width: 1, height: 16)

        if let run {
          Text(run.selectedMethod?.displayName ?? run.configuration.method.displayName)
            .font(.system(size: 13, weight: .semibold))
          Text(run.methodReason ?? run.configuration.method.explanation)
            .foregroundStyle(AppTheme.secondaryText)
            .lineLimit(1)
        } else {
          Picker("方法", selection: $configuration.method) {
            ForEach(FactorMethod.allCases) { method in
              Text(method.displayName).tag(method)
            }
          }
          .labelsHidden()
          .pickerStyle(.menu)
          .fixedSize()
          .accessibilityIdentifier("methodPicker")

          Text(configuration.method.explanation)
            .font(.system(size: 12.5))
            .foregroundStyle(AppTheme.secondaryText)
            .lineLimit(1)
        }

        Spacer()

        if run == nil, let validationMessage {
          Label(validationMessage, systemImage: "exclamationmark.triangle.fill")
            .font(.system(size: 11.5, weight: .medium))
            .foregroundStyle(AppTheme.danger)
            .fixedSize(horizontal: false, vertical: true)
            .accessibilityIdentifier("validationMessage")
        } else if run == nil {
          Label("回车开始", systemImage: "return")
            .font(.system(size: 11, weight: .medium))
            .foregroundStyle(AppTheme.tertiaryText)
        }
      }
      .frame(minHeight: 28)
    }
    .padding(.horizontal, 1)
    .onAppear {
      if run == nil { isNumberFocused = true }
    }
  }

  private func copyButton(_ value: String) -> some View {
    Button {
      copy(value)
    } label: {
      Image(systemName: "doc.on.doc")
        .font(.system(size: 14, weight: .medium))
        .frame(width: 36, height: 36)
    }
    .buttonStyle(.borderless)
    .foregroundStyle(AppTheme.secondaryText)
    .background(
      AppTheme.surface.opacity(0.7),
      in: RoundedRectangle(
        cornerRadius: 8,
        style: .continuous
      )
    )
    .help("复制整数")
    .accessibilityLabel("复制待分解整数")
    .disabled(value.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
  }
}
