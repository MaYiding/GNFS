import Charts
import SwiftUI

struct RunStageView: View {
  let run: RunRecord?
  let finalizationMessage: String
  var canCreateRun = true
  let copy: (String) -> Void
  let newRun: () -> Void

  var body: some View {
    Group {
      if let run, run.status == .succeeded, run.hasVerifiedResult, let result = run.result {
        ResultSummaryView(result: result, canCreateRun: canCreateRun, copy: copy, newRun: newRun)
      } else if let run, run.needsResultVerification {
        VStack(alignment: .leading, spacing: 12) {
          Label("历史结果未验证", systemImage: "exclamationmark.shield")
            .font(.headline).foregroundStyle(AppTheme.warning)
          Text("这条记录未通过当前完整结果校验，不能确认质因数分解已完成。原记录已保留，可使用原参数重新运行。")
            .font(.callout).foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
          if let result = run.result, !result.factors.isEmpty {
            Text("记录中的因子（未验证）：\(result.factorExpression)")
              .font(.callout.monospaced()).textSelection(.enabled)
              .fixedSize(horizontal: false, vertical: true)
          }
          Button("使用原参数新建", action: newRun)
            .disabled(!canCreateRun).buttonStyle(.borderedProminent)
        }
      } else if let run, run.status == .failed || run.status == .cancelled {
        FailureStateView(
          run: run,
          finalizationMessage: finalizationMessage,
          canCreateRun: canCreateRun,
          newRun: newRun
        )
      } else if let run {
        LiveMetricsView(run: run)
      } else {
        ReadyStateView()
      }
    }
    .frame(maxWidth: .infinity, alignment: .topLeading)
  }
}

private struct ReadyStateView: View {
  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("计算说明")
        .font(.callout.weight(.semibold))
      Text("运行时可查看实际算法、阶段与日志。完成后，结果会经过素性与精确乘积校验，并保存在本机历史中。")
        .font(.caption).foregroundStyle(.secondary)
        .fixedSize(horizontal: false, vertical: true)
      LazyVGrid(
        columns: [GridItem(.adaptive(minimum: 150), alignment: .leading)], alignment: .leading,
        spacing: 8
      ) {
        Label("完整质因数分解", systemImage: "function")
        Label("精确乘积校验", systemImage: "equal.circle")
        Label("本机保留历史", systemImage: "clock")
      }
      .font(.caption).foregroundStyle(.secondary)
    }
  }
}

private struct LiveMetricsView: View {
  let run: RunRecord

  private var latestRate: Double {
    run.samples.last?.rate ?? 0
  }

  private var progress: Double {
    run.progressFraction ?? 0
  }

  private var remainingRelations: UInt64 {
    guard run.relationsTarget > run.relationsFound else { return 0 }
    return run.relationsTarget - run.relationsFound
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 18) {
      HStack(alignment: .firstTextBaseline, spacing: 16) {
        Label(run.activityTitle, systemImage: "waveform.path.ecg")
          .font(.headline)
          .foregroundStyle(AppTheme.primaryText)
        Spacer()
        TimelineView(.periodic(from: .now, by: 1)) { context in
          Text("已运行 \(DisplayFormat.duration(run.elapsed(at: context.date)))")
            .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            .fixedSize()
        }
      }

      if let number = run.currentNumber, let attempt = run.attemptIndex, attempt > 0 {
        Text("第 \(attempt) 次拆分 · N = \(number)")
          .font(.caption.monospaced()).foregroundStyle(.secondary)
          .textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
      }

      if !run.usesGNFSPipeline {
        HStack(alignment: .top, spacing: 12) {
          ProgressView().controlSize(.small)
          VStack(alignment: .leading, spacing: 6) {
            Text(run.status == .cancelling ? "正在停止运算…" : "正在搜索质因数")
              .font(.callout.weight(.medium))
            Text("此算法无法可靠预估剩余时间。完成后会校验全部因子及其乘积。")
              .font(.caption).foregroundStyle(.secondary)
              .fixedSize(horizontal: false, vertical: true)
          }
        }
        .padding(.vertical, 6)
      } else if run.phase == .sieving {
        ViewThatFits(in: .horizontal) {
          HStack(alignment: .top, spacing: 28) {
            relationPane
              .frame(maxWidth: .infinity, alignment: .leading)

            chartPane
              .frame(maxWidth: .infinity, alignment: .leading)
          }
          .frame(minWidth: 740)

          VStack(alignment: .leading, spacing: 20) {
            relationPane
            chartPane
          }
        }

        if run.matrixRows > 0 || run.matrixCols > 0 {
          Label("矩阵 \(run.matrixRows) × \(run.matrixCols)", systemImage: "square.grid.3x3")
            .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
        }
      } else {
        HStack(spacing: 12) {
          ProgressView().controlSize(.small)
          VStack(alignment: .leading, spacing: 6) {
            Text(run.progressFraction.map { "当前阶段 \(Int($0 * 100))%" } ?? "当前阶段未提供可预估进度")
              .font(.callout)
            if run.matrixRows > 0 || run.matrixCols > 0 {
              Text("矩阵 \(run.matrixRows) 行 × \(run.matrixCols) 列")
                .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            }
            if let tried = run.dependenciesTried, let total = run.dependenciesTotal, total > 0 {
              Text("已尝试依赖 \(tried) · 基础依赖 \(total)（可能继续尝试组合）")
                .font(.caption).foregroundStyle(.secondary)
            }
          }
        }.padding(.vertical, 6)
      }
      if let parameters = run.resolvedParameters {
        Divider()
        EffectiveParametersView(parameters: parameters)
      }
    }
  }

  private var relationPane: some View {
    VStack(alignment: .leading, spacing: 10) {
      Text(run.phase == .sieving ? "当前关系目标" : "当前阶段进度")
        .font(.caption)
        .foregroundStyle(AppTheme.secondaryText)

      if run.phase == .sieving {
        HStack(alignment: .firstTextBaseline, spacing: 9) {
          Text(DisplayFormat.count(run.relationsFound))
            .font(.gnfsMonospaced(size: 30, weight: .medium))
            .foregroundStyle(AppTheme.primaryText)
            .accessibilityIdentifier("relationsMetric")
          if run.relationsTarget > 0 {
            Text("/ \(DisplayFormat.count(run.relationsTarget))")
              .font(.gnfsMonospaced(size: 17))
              .foregroundStyle(AppTheme.secondaryText)
          }
        }
      } else {
        Text(run.progressFraction.map { String(format: "%.0f%%", $0 * 100) } ?? "正在计算")
          .font(.gnfsMonospaced(size: 30, weight: .medium))
          .foregroundStyle(AppTheme.primaryText)
      }

      if run.progressFraction != nil {
        HStack(spacing: 12) {
          ProgressView(value: progress)
            .progressViewStyle(.linear)
            .tint(AppTheme.indigo)
            .accessibilityLabel("进度")
          Text(String(format: "%.2f%%", progress * 100))
            .font(.caption.monospacedDigit())
            .foregroundStyle(AppTheme.secondaryText)
            .frame(width: 58, alignment: .trailing)
        }

        if run.phase == .sieving, run.relationsTarget > 0 {
          Text("距当前目标 \(DisplayFormat.count(remainingRelations)) 条；过滤后目标可能调整。")
            .font(.caption)
            .foregroundStyle(AppTheme.secondaryText)
            .fixedSize(horizontal: false, vertical: true)
        }
      } else {
        ProgressView()
          .controlSize(.small)
          .tint(AppTheme.indigo)
          .accessibilityLabel("当前阶段正在计算")
      }

      HStack(alignment: .top, spacing: 24) {
        metricBlock(
          label: "关系发现速率",
          value: run.samples.isEmpty ? "采样中" : DisplayFormat.rate(latestRate),
          detail: "本段格筛的平滑速率"
        )
        metricBlock(
          label: "Special-Q 数量",
          value: DisplayFormat.count(run.specialQDone),
          detail: run.relationsFound > 0 ? "已处理数量" : "等待格筛"
        )
      }
      .padding(.top, 4)
    }
  }

  private var chartPane: some View {
    VStack(alignment: .leading, spacing: 10) {
      HStack {
        Text("关系发现速率 · 当前拆分")
          .font(.caption)
          .foregroundStyle(AppTheme.secondaryText)
        Spacer()
        Text(run.samples.isEmpty ? "采样中" : DisplayFormat.rate(latestRate))
          .font(.caption.monospacedDigit())
          .foregroundStyle(AppTheme.secondaryText)
      }

      if run.samples.count >= 2 {
        Chart(run.samples) { sample in
          AreaMark(
            x: .value("时间", sample.elapsed),
            y: .value("关系每秒", sample.rate)
          )
          .interpolationMethod(.linear)
          .foregroundStyle(
            LinearGradient(
              colors: [AppTheme.indigo.opacity(0.12), AppTheme.indigo.opacity(0.01)],
              startPoint: .top,
              endPoint: .bottom
            ))

          LineMark(
            x: .value("时间", sample.elapsed),
            y: .value("关系每秒", sample.rate)
          )
          .interpolationMethod(.linear)
          .foregroundStyle(AppTheme.indigo)
          .lineStyle(StrokeStyle(lineWidth: 1.6, lineCap: .round, lineJoin: .round))
        }
        .chartXAxis {
          AxisMarks(values: .automatic(desiredCount: 5)) { _ in
            AxisGridLine().foregroundStyle(AppTheme.separator)
            AxisValueLabel(format: Decimal.FormatStyle().precision(.fractionLength(0)))
              .foregroundStyle(AppTheme.tertiaryText)
          }
        }
        .chartYAxis {
          AxisMarks(position: .leading, values: .automatic(desiredCount: 4)) { _ in
            AxisGridLine().foregroundStyle(AppTheme.separator)
            AxisValueLabel().foregroundStyle(AppTheme.tertiaryText)
          }
        }
        .chartXAxisLabel("任务用时（秒）")
        .chartYAxisLabel("关系/秒")
        .chartYScale(domain: .automatic(includesZero: true))
        .frame(height: 150)
        .accessibilityLabel("关系收集速率图")
      } else {
        VStack(spacing: 8) {
          Image(systemName: "chart.xyaxis.line").font(.title3)
          Text("等待速率样本").font(.caption.weight(.medium))
          Text("收集到关系后显示实时曲线。")
            .font(.caption).foregroundStyle(.tertiary)
        }
        .foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, minHeight: 150)
      }
    }
  }

  private func metricBlock(label: String, value: String, detail: String) -> some View {
    VStack(alignment: .leading, spacing: 4) {
      Text(label)
        .font(.caption)
        .foregroundStyle(AppTheme.tertiaryText)
      Text(value)
        .font(.gnfsMonospaced(size: 16, weight: .medium))
        .foregroundStyle(AppTheme.primaryText)
      Text(detail)
        .font(.caption2)
        .foregroundStyle(AppTheme.tertiaryText)
        .fixedSize(horizontal: false, vertical: true)
    }
    .frame(maxWidth: .infinity, alignment: .leading)
  }
}

private struct ResultSummaryView: View {
  let result: FactorizationResult
  let canCreateRun: Bool
  let copy: (String) -> Void
  let newRun: () -> Void

  private struct PrimePower {
    let factor: String
    let exponent: Int
  }

  private var primePowers: [PrimePower] {
    var order: [String] = []
    var powers: [String: Int] = [:]
    for factor in result.factors {
      if powers[factor] == nil { order.append(factor) }
      powers[factor, default: 0] += 1
    }
    return order.map { PrimePower(factor: $0, exponent: powers[$0, default: 1]) }
  }

  private var displayedExpression: String {
    let superscripts = Array("⁰¹²³⁴⁵⁶⁷⁸⁹")
    return primePowers.map { power in
      let exponent =
        power.exponent > 1
        ? String(power.exponent).compactMap { $0.wholeNumberValue }.map { String(superscripts[$0]) }
          .joined()
        : ""
      return DecimalBigUInt.grouped(power.factor) + exponent
    }.joined(separator: " × ")
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 20) {
      HStack(alignment: .top, spacing: 10) {
        Image(systemName: "checkmark.circle.fill")
          .font(.system(size: 18))
          .foregroundStyle(AppTheme.indigo)
          .accessibilityHidden(true)
        VStack(alignment: .leading, spacing: 5) {
          Text(result.factors.count == 1 ? "N 本身是素数" : "完整质因数分解完成")
            .font(.headline)
          Text("乘积精确校验 · 全部因子通过素性检查")
            .font(.caption).foregroundStyle(.secondary)
        }
        Spacer(minLength: 12)
        Button("分解另一个整数", action: newRun)
          .disabled(!canCreateRun)
          .controlSize(.small)
          .accessibilityIdentifier("resultNewRunButton")
      }

      VStack(alignment: .leading, spacing: 12) {
        HStack {
          Text("验证后的分解式")
            .font(.caption).foregroundStyle(.secondary)
          Spacer()
          Button {
            copy("\(result.number) = \(result.factorExpression)")
          } label: {
            Label("复制分解式", systemImage: "doc.on.doc")
          }
          .buttonStyle(.borderless).controlSize(.small)
          .accessibilityLabel("复制完整分解式")
        }
        Text("N = " + displayedExpression)
          .font(.gnfsMonospaced(size: 25, weight: .regular))
          .foregroundStyle(AppTheme.primaryText)
          .lineSpacing(6)
          .fixedSize(horizontal: false, vertical: true)
          .frame(maxWidth: .infinity, alignment: .leading)
          .textSelection(.enabled)
          .accessibilityIdentifier("factorResult")
          .accessibilityLabel("验证后的分解式")
          .accessibilityValue("N = \(result.factorExpression)")
      }

      LazyVGrid(
        columns: [GridItem(.flexible(), spacing: 28), GridItem(.flexible())],
        alignment: .leading, spacing: 12
      ) {
        resultMetric("质因数", "\(result.factors.count) 个 · \(result.distinctPrimeCount) 种")
        resultMetric("完成算法", result.executionLabel)
        resultMetric("总用时", DisplayFormat.duration(result.timings.total))
        resultMetric("整数规模", "\(result.bitCount) bits · \(result.digitCount) 位")
      }
      .frame(maxWidth: 680, alignment: .leading)

      Divider()
      RunStatisticsView(result: result)
    }
  }

  private func resultMetric(_ label: String, _ value: String) -> some View {
    HStack(alignment: .firstTextBaseline, spacing: 12) {
      Text(label)
        .foregroundStyle(.secondary)
      Spacer(minLength: 4)
      Text(value)
        .fontWeight(.medium).monospacedDigit()
        .multilineTextAlignment(.trailing)
        .fixedSize(horizontal: false, vertical: true)
    }
    .font(.caption)
    .frame(maxWidth: .infinity)
  }
}

private struct FailureStateView: View {
  let run: RunRecord
  let finalizationMessage: String
  let canCreateRun: Bool
  let newRun: () -> Void

  var body: some View {
    VStack(alignment: .leading, spacing: 16) {
      VStack(alignment: .leading, spacing: 10) {
        HStack(spacing: 9) {
          Image(systemName: run.status == .cancelled ? "stop.circle" : "exclamationmark.triangle")
            .foregroundStyle(run.status == .cancelled ? AppTheme.secondaryText : AppTheme.danger)
            .accessibilityHidden(true)
          Text(run.status == .cancelled ? "任务已取消" : "完整质因数分解未完成")
            .foregroundStyle(AppTheme.primaryText)
        }
        .font(.headline)
        Text(run.errorMessage ?? "运算引擎没有返回可验证的完整质因数结果。")
          .font(.callout)
          .foregroundStyle(AppTheme.secondaryText)
          .textSelection(.enabled)
          .fixedSize(horizontal: false, vertical: true)
        Text(finalizationMessage + " 可使用当前参数新建任务。")
          .font(.caption)
          .foregroundStyle(AppTheme.tertiaryText)
          .fixedSize(horizontal: false, vertical: true)
      }

      HStack {
        Text("已运行 \(DisplayFormat.duration(run.elapsed)) · \(run.activityTitle)")
          .font(.caption).foregroundStyle(.secondary)
        Spacer(minLength: 12)
        Button("使用当前参数新建", action: newRun)
          .disabled(!canCreateRun)
          .controlSize(.small)
          .accessibilityIdentifier("failureNewRunButton")
      }
      if let result = run.result {
        Divider()
        RunStatisticsView(result: result)
      } else if let parameters = run.resolvedParameters {
        Divider()
        EffectiveParametersView(parameters: parameters)
      }
    }
    .frame(maxWidth: .infinity, alignment: .leading)
  }
}
