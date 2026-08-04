import SwiftUI

struct ParameterInspectorView: View {
  @Binding var configuration: RunConfiguration

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 22) {
        header
        resultScopeSection
        methodSection
        Divider().overlay(AppTheme.separator)
        polynomialSection
        factorBaseSection
        sieveSection

        if let message = configuration.parameters.validationMessage {
          Label(message, systemImage: "exclamationmark.triangle.fill")
            .font(.caption)
            .foregroundStyle(AppTheme.danger)
            .fixedSize(horizontal: false, vertical: true)
            .accessibilityIdentifier("parameterValidationMessage")
        }

        Text("留空的字段会由 GNFS 根据整数规模自动计算。自定义参数可能显著改变运行时间和内存占用。")
          .font(.caption)
          .foregroundStyle(AppTheme.tertiaryText)
          .fixedSize(horizontal: false, vertical: true)
      }
      .padding(20)
    }
    .background(AppTheme.elevated)
    .accessibilityIdentifier("parameterInspector")
  }

  private var resultScopeSection: some View {
    parameterGroup("结果范围") {
      HStack(alignment: .top, spacing: 10) {
        Image(systemName: "checkmark.shield.fill")
          .foregroundStyle(AppTheme.success)
          .padding(.top, 1)
        VStack(alignment: .leading, spacing: 4) {
          Text("完整质因数分解")
            .font(.system(size: 12, weight: .semibold))
          Text("递归分解所有合数余因子，直到每个结果均通过素性检查。")
            .font(.caption)
            .foregroundStyle(AppTheme.secondaryText)
            .fixedSize(horizontal: false, vertical: true)
        }
      }
      .padding(11)
      .frame(maxWidth: .infinity, alignment: .leading)
      .background(
        AppTheme.success.opacity(0.08),
        in: RoundedRectangle(cornerRadius: 8, style: .continuous)
      )
      .overlay {
        RoundedRectangle(cornerRadius: 8, style: .continuous)
          .stroke(AppTheme.success.opacity(0.22), lineWidth: 1)
      }
    }
  }

  private var header: some View {
    HStack {
      VStack(alignment: .leading, spacing: 4) {
        Text("运行参数")
          .font(.system(size: 17, weight: .semibold))
        Text(configuration.parameters.isAutomatic ? "当前使用自动配置" : "当前包含手动覆盖")
          .font(.caption)
          .foregroundStyle(AppTheme.secondaryText)
      }
      Spacer()
      Button("恢复自动") {
        configuration.parameters = .automatic
      }
      .disabled(configuration.parameters.isAutomatic)
    }
  }

  private var methodSection: some View {
    parameterGroup("算法") {
      Picker("分解方法", selection: $configuration.method) {
        ForEach(FactorMethod.allCases) { method in
          VStack(alignment: .leading) {
            Text(method.displayName)
          }
          .tag(method)
        }
      }
      .pickerStyle(.menu)

      Text(configuration.method.explanation)
        .font(.caption)
        .foregroundStyle(AppTheme.secondaryText)
    }
  }

  private var polynomialSection: some View {
    parameterGroup("多项式") {
      Picker("次数", selection: $configuration.parameters.degree) {
        Text("自动").tag(nil as Int?)
        ForEach(3...8, id: \.self) { value in
          Text("\(value)").tag(Optional(value))
        }
      }
      .pickerStyle(.menu)
    }
  }

  private var factorBaseSection: some View {
    parameterGroup("因子基") {
      OptionalIntField(
        title: "有理上界",
        value: $configuration.parameters.rationalBound,
        accessibilityID: "rationalBoundField"
      )
      OptionalIntField(
        title: "代数上界",
        value: $configuration.parameters.algebraicBound,
        accessibilityID: "algebraicBoundField"
      )
      OptionalUInt64Field(
        title: "大素数上界",
        value: $configuration.parameters.largePrimeBound,
        accessibilityID: "largePrimeBoundField"
      )
    }
  }

  private var sieveSection: some View {
    parameterGroup("筛区") {
      OptionalIntField(
        title: "宽度",
        value: $configuration.parameters.sieveWidth,
        accessibilityID: "sieveWidthField"
      )
      OptionalIntField(
        title: "高度",
        value: $configuration.parameters.sieveHeight,
        accessibilityID: "sieveHeightField"
      )
    }
  }

  private func parameterGroup<Content: View>(
    _ title: String,
    @ViewBuilder content: () -> Content
  ) -> some View {
    VStack(alignment: .leading, spacing: 12) {
      Text(title.uppercased())
        .font(.system(size: 10, weight: .semibold))
        .tracking(1.1)
        .foregroundStyle(AppTheme.tertiaryText)
      content()
    }
  }
}

private struct OptionalIntField: View {
  let title: String
  @Binding var value: Int?
  let accessibilityID: String

  var body: some View {
    HStack {
      Text(title)
      Spacer()
      TextField("自动", value: $value, format: .number.grouping(.never))
        .multilineTextAlignment(.trailing)
        .textFieldStyle(.roundedBorder)
        .frame(width: 132)
        .accessibilityIdentifier(accessibilityID)
    }
    .font(.system(size: 12))
  }
}

private struct OptionalUInt64Field: View {
  let title: String
  @Binding var value: UInt64?
  let accessibilityID: String

  var body: some View {
    HStack {
      Text(title)
      Spacer()
      TextField("自动", value: $value, format: .number.grouping(.never))
        .multilineTextAlignment(.trailing)
        .textFieldStyle(.roundedBorder)
        .frame(width: 132)
        .accessibilityIdentifier(accessibilityID)
    }
    .font(.system(size: 12))
  }
}
