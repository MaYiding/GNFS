import SwiftUI

/// Values come from the selected backend result, never from requested overrides.
struct RunStatisticsView: View {
  let result: FactorizationResult
  @State private var selectedAttempt = 0

  private var attempts: [FactorizationResult] { result.attempts ?? [] }
  private var selected: FactorizationResult {
    guard selectedAttempt > 0, selectedAttempt <= attempts.count else { return result }
    return attempts[selectedAttempt - 1]
  }
  private var isAggregate: Bool { selectedAttempt == 0 && attempts.count > 1 }
  private var isSingleGroup: Bool {
    selected.statistics.gnfsParametersApplied != true && (selected.statistics.siqsWorkers ?? 0) == 0
  }
  private var statisticColumns: [GridItem] {
    if selected.statistics.gnfsParametersApplied != true {
      let count = (selected.statistics.siqsWorkers ?? 0) > 0 ? 2 : 1
      return Array(repeating: GridItem(.flexible(), spacing: 28, alignment: .top), count: count)
    }
    return [GridItem(.adaptive(minimum: 235), spacing: 28, alignment: .top)]
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 18) {
      HStack(alignment: .firstTextBaseline, spacing: 16) {
        Text("计算统计")
          .font(.headline)
        Spacer()
        if !attempts.isEmpty {
          Picker("统计范围", selection: $selectedAttempt) {
            Text("整次任务 · \(attempts.count) 次拆分").tag(0)
            ForEach(attempts.indices, id: \.self) { index in
              Text("第 \(index + 1) 次 · \(attempts[index].executionLabel)").tag(index + 1)
            }
          }
          .pickerStyle(.menu).controlSize(.small)
          .labelsHidden().frame(maxWidth: 260)
          .accessibilityIdentifier("statisticsScopePicker")
        }
      }
      if selectedAttempt > 0 {
        Text("本次整数  \(selected.number)")
          .font(.caption.monospaced()).textSelection(.enabled)
          .fixedSize(horizontal: false, vertical: true)
      }
      if let algorithms = selected.statistics.algorithmsExecuted, !algorithms.isEmpty {
        Text("执行过的算法  " + algorithms.map { AlgorithmDisplay.name($0) }.joined(separator: " · "))
          .font(.caption).foregroundStyle(.secondary)
          .fixedSize(horizontal: false, vertical: true)
      }
      LazyVGrid(
        columns: statisticColumns,
        alignment: .leading, spacing: 24
      ) {
        timingsPanel
        if selected.statistics.gnfsParametersApplied == true {
          gnfsPanel
          matrixPanel
        }
        if let workers = selected.statistics.siqsWorkers, workers > 0 {
          StatisticsPanel(title: "SIQS") {
            StatisticRow("保留关系", selected.statistics.siqsRelations)
            StatisticRow("处理多项式", selected.statistics.siqsPolynomials)
            StatisticRow(isAggregate ? "最大筛选线程数" : "筛选线程数", workers)
          }
        }
      }
      .frame(maxWidth: isSingleGroup ? 420 : .infinity, alignment: .leading)
      if selected.statistics.algorithmsExecuted?.contains("siqs") == true,
        selected.statistics.siqsWorkers == nil
      {
        Text("SIQS 已执行，但未提供结束统计。")
          .font(.caption).foregroundStyle(.secondary)
      }
      if selected.statistics.gnfsParametersApplied == true {
        if isAggregate {
          Text("关系与计数按拆分累加；矩阵取行数最大的单个矩阵。生效参数请切换到具体拆分查看。")
            .font(.caption).foregroundStyle(.secondary)
        } else {
          Divider()
          EffectiveParametersView(parameters: selected.statistics.effectiveParameters)
        }
      } else {
        Text(
          selected.statistics.gnfsParametersApplied == false
            ? "此次统计范围内未执行 GNFS，高级 GNFS 参数未应用。"
            : "这条历史记录未提供实际参数遥测。"
        )
        .font(.caption).foregroundStyle(.secondary)
      }
    }
    .accessibilityIdentifier("runStatistics")
  }

  private var timingsPanel: some View {
    StatisticsPanel(title: "耗时") {
      StatisticRow("总计", DisplayFormat.duration(selected.timings.total), emphasized: true)
      StatisticRow("预检查与其他算法", selected.timings.nonGNFS.map(DisplayFormat.duration) ?? "未提供")
      if selected.statistics.gnfsParametersApplied == true {
        StatisticRow("多项式选择", DisplayFormat.duration(selected.timings.polynomial))
        StatisticRow("因子基", DisplayFormat.duration(selected.timings.factorBase))
        StatisticRow("格筛", DisplayFormat.duration(selected.timings.sieving))
        StatisticRow("关系过滤", DisplayFormat.duration(selected.timings.filtering))
        StatisticRow("线性代数", DisplayFormat.duration(selected.timings.linearAlgebra))
        StatisticRow("平方根", DisplayFormat.duration(selected.timings.squareRoot))
        StatisticRow("GCD 提取", DisplayFormat.duration(selected.timings.extraction))
      }
    }
  }

  private var gnfsPanel: some View {
    StatisticsPanel(title: "GNFS 关系") {
      StatisticRow("收集关系", selected.statistics.relationsFound, emphasized: true)
      StatisticRow("完全光滑", selected.statistics.fullRelations)
      StatisticRow(
        "测试候选",
        selected.statistics.candidatesStatisticsComplete == true
          ? selected.statistics.candidatesTotal.map(DisplayFormat.count) ?? "未提供" : "未提供")
      StatisticRow(
        "1 LP / ≥2 LP",
        "\(DisplayFormat.count(selected.statistics.partial1LP)) / \(DisplayFormat.count(selected.statistics.partial2LP))"
      )
      StatisticRow("过滤后关系", selected.statistics.relationsAfterFilter)
      StatisticRow("移除单例", selected.statistics.singletonsRemoved)
      StatisticRow("合并关系", selected.statistics.mergedRelations)
      StatisticRow("Special-Q", selected.statistics.specialQProcessed)
      StatisticRow("筛选轮数", selected.statistics.sieveRounds)
    }
  }

  private var matrixPanel: some View {
    StatisticsPanel(title: isAggregate ? "最大矩阵与依赖" : "矩阵与依赖") {
      StatisticRow(
        "矩阵 行 × 列",
        "\(DisplayFormat.count(selected.statistics.matrixRows)) × \(DisplayFormat.count(selected.statistics.matrixCols))",
        emphasized: true)
      StatisticRow("非零元素", selected.statistics.matrixWeight)
      StatisticRow("行列差", String(selected.statistics.matrixExcess))
      StatisticRow("找到依赖", selected.statistics.dependenciesFound)
      StatisticRow("尝试依赖（含组合）", String(selected.statistics.dependenciesTried))
    }
  }
}

struct EffectiveParametersView: View {
  let parameters: EffectiveGNFSParameters?

  var body: some View {
    if let parameters {
      VStack(alignment: .leading, spacing: 10) {
        Text("实际生效的 GNFS 参数")
          .font(.callout.weight(.semibold))
        LazyVGrid(
          columns: [GridItem(.adaptive(minimum: 235), spacing: 28, alignment: .leading)],
          alignment: .leading, spacing: 0
        ) {
          StatisticRow("多项式次数", parameters.degree)
          StatisticRow("有理因子基上界", parameters.rationalBound)
          StatisticRow("代数因子基上界", parameters.algebraicBound)
          StatisticRow("大素数上界", parameters.largePrimeBound)
          StatisticRow("筛区 宽 × 高", "\(parameters.sieveWidth) × \(parameters.sieveHeight)")
          StatisticRow("Special-Q 预算", parameters.maxSpecialQ)
        }
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      .accessibilityIdentifier("effectiveParameters")
    } else {
      Text("此记录未提供完整的实际参数。")
        .font(.caption).foregroundStyle(.secondary)
    }
  }
}

private struct StatisticsPanel<Content: View>: View {
  let title: String
  @ViewBuilder let content: Content
  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text(title)
        .font(.callout.weight(.semibold))
        .foregroundStyle(AppTheme.primaryText)
      Divider()
      VStack(alignment: .leading, spacing: 0) {
        content
      }
    }
    .frame(maxWidth: .infinity, alignment: .topLeading)
  }
}

private struct StatisticRow: View {
  let label: String
  let value: String
  var emphasized = false
  init(_ label: String, _ value: String, emphasized: Bool = false) {
    self.label = label
    self.value = value
    self.emphasized = emphasized
  }
  init(_ label: String, _ value: UInt64?, emphasized: Bool = false) {
    self.init(label, value.map(DisplayFormat.count) ?? "未提供", emphasized: emphasized)
  }
  var body: some View {
    HStack(alignment: .firstTextBaseline, spacing: 12) {
      Text(label).foregroundStyle(.secondary)
        .fixedSize(horizontal: false, vertical: true)
      Spacer(minLength: 8)
      Text(value).fontWeight(emphasized ? .semibold : .regular)
        .monospacedDigit().textSelection(.enabled)
        .multilineTextAlignment(.trailing)
        .fixedSize(horizontal: false, vertical: true)
    }
    .font(.caption)
    .padding(.vertical, 4)
    .accessibilityElement(children: .combine)
    .accessibilityLabel(label)
    .accessibilityValue(value)
  }
}

extension FactorStatistics {
  var effectiveParameters: EffectiveGNFSParameters? {
    guard gnfsParametersApplied == true, let sieveWidth, let sieveHeight, let maxSpecialQ else {
      return nil
    }
    return EffectiveGNFSParameters(
      degree: degree, rationalBound: rationalBound, algebraicBound: algebraicBound,
      largePrimeBound: largePrimeBound, sieveWidth: sieveWidth, sieveHeight: sieveHeight,
      maxSpecialQ: maxSpecialQ)
  }
}
