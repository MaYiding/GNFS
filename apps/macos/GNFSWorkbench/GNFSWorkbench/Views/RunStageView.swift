import Charts
import SwiftUI

struct RunStageView: View {
  let run: RunRecord?
  let finalizationMessage: String
  let copy: (String) -> Void
  let newRun: () -> Void

  var body: some View {
    Group {
      if let run, run.status == .succeeded, let result = run.result {
        ResultSummaryView(result: result, copy: copy, newRun: newRun)
      } else if let run, run.status == .failed || run.status == .cancelled {
        FailureStateView(
          run: run,
          finalizationMessage: finalizationMessage,
          newRun: newRun
        )
      } else if let run {
        LiveMetricsView(run: run)
      } else {
        ReadyStateView()
      }
    }
    .frame(maxWidth: .infinity, minHeight: 304, alignment: .topLeading)
    .padding(.horizontal, 10)
    .padding(.vertical, 2)
  }
}

private struct ReadyStateView: View {
  var body: some View {
    HStack(spacing: 32) {
      ZStack {
        RoundedRectangle(cornerRadius: 18, style: .continuous)
          .fill(AppTheme.indigo.opacity(0.11))
          .frame(width: 94, height: 94)
        Image(systemName: "function")
          .font(.system(size: 35, weight: .medium))
          .foregroundStyle(AppTheme.progressGradient)
      }
      .accessibilityHidden(true)

      VStack(alignment: .leading, spacing: 11) {
        Text("准备进行完整质因数分解")
          .font(.system(size: 23, weight: .semibold))
          .foregroundStyle(AppTheme.primaryText)

        Text("自动模式会从低成本算法开始，需要时再进入 SIQS 或 GNFS；只有每个因子都通过素性检查且乘积精确等于 N，结果才会被接受。")
          .font(.system(size: 13.5))
          .foregroundStyle(AppTheme.secondaryText)
          .fixedSize(horizontal: false, vertical: true)
          .frame(maxWidth: 720, alignment: .leading)

        HStack(spacing: 22) {
          feature("逐个验证质因数", icon: "checkmark.shield")
          feature("可安全取消", icon: "stop.circle")
          feature("本机保存历史", icon: "clock.arrow.circlepath")
        }
        .padding(.top, 7)
      }

      Spacer(minLength: 12)
    }
    .frame(maxWidth: .infinity, minHeight: 282, alignment: .leading)
    .padding(.horizontal, 28)
  }

  private func feature(_ label: String, icon: String) -> some View {
    Label(label, systemImage: icon)
      .font(.system(size: 11.5, weight: .medium))
      .foregroundStyle(AppTheme.secondaryText)
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
        Label(run.phase.displayName, systemImage: "waveform.path.ecg")
          .font(.system(size: 14, weight: .semibold))
          .foregroundStyle(AppTheme.cyan)
        Spacer()
        Text("已运行 \(DisplayFormat.duration(run.elapsed))")
          .font(.gnfsMonospaced(size: 12, weight: .medium))
          .foregroundStyle(AppTheme.secondaryText)
      }

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

        VStack(alignment: .leading, spacing: 22) {
          relationPane
          Divider().overlay(AppTheme.separator)
          chartPane
        }
      }

      Divider().overlay(AppTheme.separator)

      HStack(spacing: 28) {
        Label("进程隔离", systemImage: "shield.checkered")
        Label("JSONL 实时事件", systemImage: "arrow.left.arrow.right")
        Label("完整质因数模式", systemImage: "checkmark.seal")
        if run.matrixRows > 0 || run.matrixCols > 0 {
          Label(
            "矩阵 \(run.matrixRows) × \(run.matrixCols)",
            systemImage: "square.grid.3x3"
          )
        }
        Spacer()
      }
      .font(.system(size: 11.5, weight: .medium))
      .foregroundStyle(AppTheme.secondaryText)
    }
  }

  private var relationPane: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text(run.phase == .sieving ? "关系数（找到 / 目标）" : "当前阶段进度")
        .font(.system(size: 12, weight: .semibold))
        .tracking(0.55)
        .foregroundStyle(AppTheme.secondaryText)

      if run.phase == .sieving || run.relationsFound > 0 {
        HStack(alignment: .firstTextBaseline, spacing: 9) {
          Text(DisplayFormat.count(run.relationsFound))
            .font(.gnfsMonospaced(size: 50, weight: .semibold))
            .foregroundStyle(AppTheme.indigo)
            .accessibilityIdentifier("relationsMetric")
          if run.relationsTarget > 0 {
            Text("/ \(DisplayFormat.count(run.relationsTarget))")
              .font(.gnfsMonospaced(size: 27, weight: .medium))
              .foregroundStyle(AppTheme.secondaryText)
          }
        }
      } else {
        Text(run.phaseProgress.map { String(format: "%.0f%%", $0 * 100) } ?? "正在计算")
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

        if run.relationsTarget > 0 {
          Text("还需 \(DisplayFormat.count(remainingRelations)) 个关系")
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
          label: "已用时间",
          value: DisplayFormat.duration(run.elapsed),
          detail: "自 \(run.createdAt.formatted(date: .omitted, time: .shortened))"
        )
        metricDivider
        metricBlock(
          label: "关系发现速率",
          value: DisplayFormat.rate(latestRate),
          detail: "实时平滑值"
        )
        metricDivider
        metricBlock(
          label: "Special-Q 数量",
          value: DisplayFormat.count(run.specialQDone),
          detail: run.relationsFound > 0 ? "已处理队列" : "等待格筛"
        )
      }
      .padding(.top, 3)
    }
  }

  private var chartPane: some View {
    VStack(alignment: .leading, spacing: 9) {
      HStack {
        Text("关系发现速率（关系 / 秒）")
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
          .interpolationMethod(.catmullRom)
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
          .interpolationMethod(.catmullRom)
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
        .frame(minWidth: 360, minHeight: 190)
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
  let copy: (String) -> Void
  let newRun: () -> Void

  private struct PrimePower: Identifiable {
    let factor: String
    let exponent: Int
    var id: String { factor }
  }

  private var primePowers: [PrimePower] {
    var powers: [PrimePower] = []
    for factor in result.factors {
      if let last = powers.last, last.factor == factor {
        powers[powers.count - 1] = PrimePower(factor: factor, exponent: last.exponent + 1)
      } else {
        powers.append(PrimePower(factor: factor, exponent: 1))
      }
    }
    return powers
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 19) {
      HStack {
        Label(
          result.factors.count == 1 ? "N 本身是素数" : "完整质因数分解完成",
          systemImage: "checkmark.seal.fill"
        )
        .font(.system(size: 18, weight: .semibold))
        .foregroundStyle(AppTheme.success)

        Spacer()

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

      HStack(spacing: 0) {
        resultMetric("质因数总数", "\(result.factors.count)")
        metricDivider
        resultMetric("不同质因数", "\(result.distinctPrimeCount)")
        metricDivider
        resultMetric("主要算法", result.methodName)
        metricDivider
        resultMetric("总用时", DisplayFormat.duration(result.timings.total))
        metricDivider
        resultMetric("整数规模", "\(result.bitCount) bits · \(result.digitCount) 位")
      }

      Divider().overlay(AppTheme.separator)

      HStack(spacing: 16) {
        VStack(alignment: .leading, spacing: 4) {
          Text("验证后的分解式")
            .font(.system(size: 10.5, weight: .semibold))
            .foregroundStyle(AppTheme.tertiaryText)
          Text("N = \(result.factorExpression)")
            .font(.gnfsMonospaced(size: 12.5, weight: .medium))
            .foregroundStyle(AppTheme.secondaryText)
            .lineLimit(1)
            .textSelection(.enabled)
        }

        Spacer()

        Button("复制完整分解式") { copy(result.factorExpression) }
        Button("分解另一个整数", action: newRun)
          .buttonStyle(.borderedProminent)
          .tint(AppTheme.indigo)
          .keyboardShortcut(.defaultAction)
      }
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
  let newRun: () -> Void

  var body: some View {
    HStack(spacing: 28) {
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

      Spacer()

      Button("使用当前参数新建", action: newRun)
        .buttonStyle(.borderedProminent)
        .tint(AppTheme.indigo)
        .keyboardShortcut(.defaultAction)
    }
    .padding(.horizontal, 28)
    .frame(maxWidth: .infinity, minHeight: 282)
  }
}
