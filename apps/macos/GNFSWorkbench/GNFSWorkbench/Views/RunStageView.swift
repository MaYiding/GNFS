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
        VStack(alignment: .leading, spacing: 16) {
          Label("历史结果未验证", systemImage: "exclamationmark.shield")
            .font(.title3.weight(.semibold)).foregroundStyle(AppTheme.warning)
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
    VStack(alignment: .leading, spacing: 16) {
      Label("每一步都有可核查的结果", systemImage: "checkmark.shield")
        .font(.headline)
      Text("从上方示例开始，或粘贴自己的整数。运行时可查看阶段和日志，完成后可复制分解式或修改参数重试。")
        .font(.callout).foregroundStyle(.secondary)
        .fixedSize(horizontal: false, vertical: true)
      LazyVGrid(
        columns: [GridItem(.adaptive(minimum: 150), alignment: .leading)], alignment: .leading,
        spacing: 12
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
    VStack(alignment: .leading, spacing: 17) {
      HStack {
        Label(run.activityTitle, systemImage: "waveform.path.ecg")
          .font(.system(size: 14, weight: .semibold))
          .foregroundStyle(AppTheme.cyan)
        Spacer()
        TimelineView(.periodic(from: .now, by: 1)) { context in
          Text("已运行 \(DisplayFormat.duration(run.elapsed(at: context.date)))")
            .font(.callout.monospacedDigit()).foregroundStyle(.secondary)
        }
      }

      if let number = run.currentNumber, let attempt = run.attemptIndex, attempt > 0 {
        Text("第 \(attempt) 次拆分 · N = \(number)")
          .font(.caption.monospaced()).foregroundStyle(.secondary)
          .textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
      }

      if !run.usesGNFSPipeline {
        HStack(alignment: .top, spacing: 16) {
          ProgressView().controlSize(.small)
          VStack(alignment: .leading, spacing: 8) {
            Text(run.status == .cancelling ? "正在停止运算…" : "正在搜索质因数")
              .font(.title3.weight(.medium))
            Text("此算法无法可靠预估剩余时间。完成后会校验全部因子及其乘积。")
              .font(.callout).foregroundStyle(.secondary)
          }
        }
        .padding(.vertical, 20)
      } else if run.phase == .sieving {
        ViewThatFits(in: .horizontal) {
          HStack(alignment: .top, spacing: 32) {
            relationPane
              .frame(maxWidth: .infinity, alignment: .leading)

            Rectangle()
              .fill(AppTheme.separatorStrong)
              .frame(width: 1, height: 210)

            chartPane
              .frame(maxWidth: .infinity, alignment: .leading)
          }
          .frame(minWidth: 780)

          VStack(alignment: .leading, spacing: 22) {
            relationPane
            Divider().overlay(AppTheme.separator)
            chartPane
          }
        }

        if run.matrixRows > 0 || run.matrixCols > 0 {
          Divider().overlay(AppTheme.separator)
          Label("矩阵 \(run.matrixRows) × \(run.matrixCols)", systemImage: "square.grid.3x3")
            .font(.callout).foregroundStyle(.secondary)
        }
      } else {
        HStack(spacing: 16) {
          ProgressView().controlSize(.small)
          VStack(alignment: .leading, spacing: 8) {
            Text(run.progressFraction.map { "当前阶段 \(Int($0 * 100))%" } ?? "当前阶段未提供可预估进度")
              .font(.callout)
            if run.matrixRows > 0 || run.matrixCols > 0 {
              Text("矩阵 \(run.matrixRows) 行 × \(run.matrixCols) 列")
                .font(.callout.monospacedDigit()).foregroundStyle(.secondary)
            }
            if let tried = run.dependenciesTried, let total = run.dependenciesTotal, total > 0 {
              Text("已尝试依赖 \(tried) · 基础依赖 \(total)（可能继续尝试组合）")
                .font(.caption).foregroundStyle(.secondary)
            }
          }
        }.padding(.vertical, 12)
      }
      if let parameters = run.resolvedParameters {
        EffectiveParametersView(parameters: parameters)
      }
    }
  }

  private var relationPane: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text(run.phase == .sieving ? "当前关系目标" : "当前阶段进度")
        .font(.system(size: 12, weight: .semibold))
        .tracking(0.55)
        .foregroundStyle(AppTheme.secondaryText)

      if run.phase == .sieving {
        HStack(alignment: .firstTextBaseline, spacing: 9) {
          Text(DisplayFormat.count(run.relationsFound))
            .font(.gnfsMonospaced(size: 38, weight: .semibold))
            .foregroundStyle(AppTheme.indigo)
            .accessibilityIdentifier("relationsMetric")
          if run.relationsTarget > 0 {
            Text("/ \(DisplayFormat.count(run.relationsTarget))")
              .font(.gnfsMonospaced(size: 22, weight: .medium))
              .foregroundStyle(AppTheme.secondaryText)
          }
        }
      } else {
        Text(run.progressFraction.map { String(format: "%.0f%%", $0 * 100) } ?? "正在计算")
          .font(.gnfsMonospaced(size: 38, weight: .semibold))
          .foregroundStyle(AppTheme.primaryText)
      }

      if run.progressFraction != nil {
        HStack(spacing: 12) {
          GradientProgressBar(value: progress)
            .frame(height: 14)
          Text(String(format: "%.2f%%", progress * 100))
            .font(.gnfsMonospaced(size: 12, weight: .semibold))
            .foregroundStyle(AppTheme.indigo)
            .frame(width: 66, alignment: .trailing)
        }

        if run.phase == .sieving, run.relationsTarget > 0 {
          Text("距当前目标 \(DisplayFormat.count(remainingRelations)) 条；过滤后目标可能调整。")
            .font(.system(size: 12))
            .foregroundStyle(AppTheme.secondaryText)
        }
      } else {
        ProgressView()
          .controlSize(.small)
          .tint(AppTheme.cyan)
          .accessibilityLabel("当前阶段正在计算")
      }

      HStack(spacing: 0) {
        metricBlock(
          label: "关系发现速率",
          value: run.samples.isEmpty ? "采样中" : DisplayFormat.rate(latestRate),
          detail: "本段格筛的平滑速率"
        )
        metricDivider
        metricBlock(
          label: "Special-Q 数量",
          value: DisplayFormat.count(run.specialQDone),
          detail: run.relationsFound > 0 ? "已处理数量" : "等待格筛"
        )
      }
      .padding(.top, 3)
    }
  }

  private var chartPane: some View {
    VStack(alignment: .leading, spacing: 9) {
      HStack {
        Text("关系发现速率 · 当前拆分")
          .font(.system(size: 12, weight: .semibold))
          .foregroundStyle(AppTheme.secondaryText)
        Spacer()
        Text(DisplayFormat.rate(latestRate))
          .font(.gnfsMonospaced(size: 12, weight: .semibold))
          .foregroundStyle(AppTheme.indigo)
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
              colors: [AppTheme.indigo.opacity(0.30), AppTheme.cyan.opacity(0.02)],
              startPoint: .top,
              endPoint: .bottom
            ))

          LineMark(
            x: .value("时间", sample.elapsed),
            y: .value("关系每秒", sample.rate)
          )
          .interpolationMethod(.linear)
          .foregroundStyle(AppTheme.progressGradient)
          .lineStyle(StrokeStyle(lineWidth: 2.2, lineCap: .round, lineJoin: .round))
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
        .chartPlotStyle { plot in
          plot.background(AppTheme.canvas.opacity(0.23))
        }
        .chartYScale(domain: .automatic(includesZero: true))
        .frame(minHeight: 190)
        .accessibilityLabel("关系收集速率图")
      } else {
        ContentUnavailableView(
          "等待速率样本",
          systemImage: "chart.xyaxis.line",
          description: Text("进入格筛并收集到关系后会显示实时曲线。")
        )
        .frame(maxWidth: .infinity, minHeight: 190)
      }
    }
  }

  private var metricDivider: some View {
    Rectangle()
      .fill(AppTheme.separatorStrong)
      .frame(width: 1, height: 58)
      .padding(.horizontal, 20)
  }

  private func metricBlock(label: String, value: String, detail: String) -> some View {
    VStack(alignment: .leading, spacing: 4) {
      Text(label)
        .font(.system(size: 10.5, weight: .medium))
        .foregroundStyle(AppTheme.tertiaryText)
      Text(value)
        .font(.gnfsMonospaced(size: 17, weight: .semibold))
        .foregroundStyle(AppTheme.primaryText)
      Text(detail)
        .font(.system(size: 9.5))
        .foregroundStyle(AppTheme.tertiaryText)
        .lineLimit(1)
    }
    .frame(maxWidth: .infinity, alignment: .leading)
  }
}

private struct GradientProgressBar: View {
  let value: Double

  var body: some View {
    GeometryReader { geometry in
      ZStack(alignment: .leading) {
        Capsule()
          .fill(AppTheme.surfaceHover)
        Capsule()
          .fill(AppTheme.progressGradient)
          .frame(width: max(0, geometry.size.width * min(max(value, 0), 1)))
      }
      .overlay { Capsule().stroke(AppTheme.separatorStrong, lineWidth: 1) }
    }
    .accessibilityElement(children: .ignore)
    .accessibilityLabel("进度")
    .accessibilityValue("\(Int(min(max(value, 0), 1) * 100))%")
  }
}

private struct ResultSummaryView: View {
  let result: FactorizationResult
  let canCreateRun: Bool
  let copy: (String) -> Void
  let newRun: () -> Void

  private struct PrimePower: Identifiable {
    let factor: String
    let exponent: Int
    var id: String { factor }
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

  var body: some View {
    VStack(alignment: .leading, spacing: 19) {
      VStack(alignment: .leading, spacing: 10) {
        Label(
          result.factors.count == 1 ? "N 本身是素数" : "完整质因数分解完成",
          systemImage: "checkmark.seal.fill"
        )
        .font(.system(size: 18, weight: .semibold))
        .foregroundStyle(AppTheme.success)

        HStack(spacing: 14) {
          Label("乘积精确校验", systemImage: "equal.circle.fill")
          Label("全部因子通过素性检查", systemImage: "checkmark.shield.fill")
        }
        .font(.system(size: 11.5, weight: .semibold))
        .foregroundStyle(AppTheme.success)
      }

      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 13) {
          ForEach(Array(primePowers.enumerated()), id: \.element.id) { index, power in
            if index > 0 {
              Text("×")
                .font(.system(size: 22, weight: .light))
                .foregroundStyle(AppTheme.tertiaryText)
            }

            HStack(alignment: .top, spacing: 4) {
              Text(DecimalBigUInt.grouped(power.factor))
                .font(.gnfsMonospaced(size: 27, weight: .semibold))
              if power.exponent > 1 {
                Text("\(power.exponent)")
                  .font(.gnfsMonospaced(size: 13, weight: .bold))
                  .baselineOffset(10)
              }
            }
            .foregroundStyle(index.isMultiple(of: 2) ? AppTheme.indigo : AppTheme.cyan)
            .padding(.horizontal, 16)
            .frame(height: 58)
            .background(
              AppTheme.surface.opacity(0.68),
              in: RoundedRectangle(cornerRadius: 9, style: .continuous)
            )
            .overlay {
              RoundedRectangle(cornerRadius: 9, style: .continuous)
                .stroke(AppTheme.separator, lineWidth: 1)
            }
            .textSelection(.enabled)
          }
        }
      }
      .accessibilityIdentifier("factorResult")

      LazyVGrid(
        columns: [GridItem(.adaptive(minimum: 130), alignment: .leading)], alignment: .leading,
        spacing: 18
      ) {
        resultMetric("质因数总数", "\(result.factors.count)")
        resultMetric("不同质因数", "\(result.distinctPrimeCount)")
        resultMetric("完成算法", result.executionLabel)
        resultMetric("总用时", DisplayFormat.duration(result.timings.total))
        resultMetric("整数规模", "\(result.bitCount) bits · \(result.digitCount) 位")
      }

      Divider().overlay(AppTheme.separator)

      VStack(alignment: .leading, spacing: 16) {
        VStack(alignment: .leading, spacing: 4) {
          Text("验证后的分解式")
            .font(.system(size: 10.5, weight: .semibold))
            .foregroundStyle(AppTheme.tertiaryText)
          Text("N = \(result.factorExpression)")
            .font(.gnfsMonospaced(size: 12.5, weight: .medium))
            .foregroundStyle(AppTheme.secondaryText)
            .fixedSize(horizontal: false, vertical: true)
            .textSelection(.enabled)
        }

        HStack {
          Button("复制完整分解式") { copy("\(result.number) = \(result.factorExpression)") }
          Button("分解另一个整数", action: newRun)
            .disabled(!canCreateRun)
            .buttonStyle(.borderedProminent)
            .tint(AppTheme.indigo)
            .accessibilityIdentifier("resultNewRunButton")
        }
      }
      Divider()
      RunStatisticsView(result: result)
    }
  }

  private var metricDivider: some View {
    Rectangle()
      .fill(AppTheme.separatorStrong)
      .frame(width: 1, height: 48)
      .padding(.horizontal, 19)
  }

  private func resultMetric(_ label: String, _ value: String) -> some View {
    VStack(alignment: .leading, spacing: 5) {
      Text(label)
        .font(.system(size: 10.5, weight: .medium))
        .foregroundStyle(AppTheme.tertiaryText)
      Text(value)
        .font(.system(size: 13, weight: .semibold))
        .foregroundStyle(AppTheme.primaryText)
        .lineLimit(1)
    }
    .frame(maxWidth: .infinity, alignment: .leading)
  }
}

private struct FailureStateView: View {
  let run: RunRecord
  let finalizationMessage: String
  let canCreateRun: Bool
  let newRun: () -> Void

  var body: some View {
    VStack(alignment: .leading, spacing: 18) {
      Image(systemName: run.status == .cancelled ? "stop.circle" : "exclamationmark.triangle")
        .font(.system(size: 42, weight: .light))
        .foregroundStyle(run.status == .cancelled ? AppTheme.secondaryText : AppTheme.danger)
        .accessibilityHidden(true)

      VStack(alignment: .leading, spacing: 9) {
        Text(run.status == .cancelled ? "任务已取消" : "完整质因数分解未完成")
          .font(.system(size: 22, weight: .semibold))
          .foregroundStyle(AppTheme.primaryText)
        Text(run.errorMessage ?? "运算引擎没有返回可验证的完整质因数结果。")
          .font(.system(size: 13.5))
          .foregroundStyle(AppTheme.secondaryText)
          .textSelection(.enabled)
          .fixedSize(horizontal: false, vertical: true)
        Text(finalizationMessage + " 可使用当前参数新建任务。")
          .font(.system(size: 11.5))
          .foregroundStyle(AppTheme.tertiaryText)
      }

      Text("已运行 \(DisplayFormat.duration(run.elapsed)) · \(run.activityTitle)")
        .font(.caption).foregroundStyle(.secondary)
      if let result = run.result {
        RunStatisticsView(result: result)
      } else if let parameters = run.resolvedParameters {
        EffectiveParametersView(parameters: parameters)
      }

      Button("使用当前参数新建", action: newRun)
        .disabled(!canCreateRun)
        .buttonStyle(.borderedProminent)
        .tint(AppTheme.indigo)
        .accessibilityIdentifier("failureNewRunButton")
    }
    .frame(maxWidth: .infinity, alignment: .leading)
  }
}
