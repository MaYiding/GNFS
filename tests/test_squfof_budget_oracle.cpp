// Deterministic SQUFOF budget matrix and offline policy oracle.

#include <gnfs/cofactor/squfof.hpp>

#include "fixtures/squfof_budget_corpus_v1.hpp"
#include "fixtures/squfof_strategy_corpus_v1.hpp"
#include "support/squfof_budget_optimizer.hpp"
#include "support/squfof_strategy_optimizer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using gnfs::cofactor::SQUFOF;
using gnfs::tests::fixtures::FIXED_50D_SQUFOF_STRATEGY_V1;
using gnfs::tests::fixtures::FIXED_50D_SQUFOF_STRATEGY_V1_DIGEST_HIGH;
using gnfs::tests::fixtures::FIXED_50D_SQUFOF_STRATEGY_V1_DIGEST_LOW;
using gnfs::tests::fixtures::FIXED_50D_SQUFOF_STRATEGY_V1_HARD_3LP_COUNT;
using gnfs::tests::fixtures::FIXED_50D_SQUFOF_STRATEGY_V1_SERIAL_ORACLE_COUNT;
using gnfs::tests::fixtures::SQUFOF_BUDGET_CORPUS_V1;
using gnfs::tests::fixtures::SQUFOF_BUDGET_CORPUS_V1_DIGEST_HIGH;
using gnfs::tests::fixtures::SQUFOF_BUDGET_CORPUS_V1_DIGEST_LOW;
using gnfs::tests::fixtures::SQUFOF_BUDGET_CORPUS_V1_UNIQUE_CASES_PER_BAND;
using gnfs::tests::fixtures::SqufofBudgetBitBand;
using gnfs::tests::fixtures::SqufofBudgetCallerPath;
using gnfs::tests::squfof_budget::BASELINE_CAP_SENTINEL;
using gnfs::tests::squfof_budget::BudgetCell;
using gnfs::tests::squfof_budget::BudgetEvaluation;
using gnfs::tests::squfof_budget::BudgetMatrix;
using gnfs::tests::squfof_budget::BudgetRow;
using gnfs::tests::squfof_budget::CapPolicy;
using gnfs::tests::squfof_budget::Digest;
using gnfs::tests::squfof_budget::DigestBuilder;
using gnfs::tests::squfof_budget::OptimizationResult;
using gnfs::tests::squfof_budget::SlotBudgetCells;
using gnfs::tests::squfof_strategy::CellStatus;
using gnfs::tests::squfof_strategy::DataSplit;
using gnfs::tests::squfof_strategy::MatrixCell;
using gnfs::tests::squfof_strategy::MatrixRow;
using gnfs::tests::squfof_strategy::SplitAssignment;
using gnfs::tests::squfof_strategy::StrategyMatrix;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

enum class EvidenceEpoch : uint8_t {
    legacy_public,
    prospective_locked,
};

enum class CallerPath : uint8_t {
    three_lp,
    normal_2lp,
};

enum class AnalysisBand : uint8_t {
    low,
    mid,
    high,
};

enum class OracleSplit : uint8_t {
    train,
    validation,
    holdout,
};

struct RowMetadata final {
    EvidenceEpoch epoch = EvidenceEpoch::legacy_public;
    CallerPath caller_path = CallerPath::normal_2lp;
    AnalysisBand bit_band = AnalysisBand::low;
    OracleSplit split = OracleSplit::train;
    uint32_t budget = 0;
};

[[nodiscard]] std::string_view epoch_name(EvidenceEpoch epoch) noexcept {
    return epoch == EvidenceEpoch::legacy_public ? "legacy_public" : "prospective_locked";
}

[[nodiscard]] std::string_view caller_name(CallerPath path) noexcept {
    return path == CallerPath::three_lp ? "three_lp" : "normal_2lp";
}

[[nodiscard]] std::string_view band_name(AnalysisBand band) noexcept {
    switch (band) {
    case AnalysisBand::low:
        return "lt_2p40";
    case AnalysisBand::mid:
        return "2p40_to_2p50";
    case AnalysisBand::high:
        return "2p50_to_2p62";
    }
    return "unknown";
}

[[nodiscard]] std::string_view split_name(EvidenceEpoch epoch, OracleSplit split) noexcept {
    switch (split) {
    case OracleSplit::train:
        return "train";
    case OracleSplit::validation:
        return "validation";
    case OracleSplit::holdout:
        return epoch == EvidenceEpoch::legacy_public ? "published_holdout" : "confirmation";
    }
    return "unknown";
}

[[nodiscard]] AnalysisBand analysis_band_for_n(uint64_t n) {
    if (n < (UINT64_C(1) << 40)) {
        return AnalysisBand::low;
    }
    if (n < (UINT64_C(1) << 50)) {
        return AnalysisBand::mid;
    }
    require(n < (UINT64_C(1) << 62), "budget oracle input exceeds the supported band");
    return AnalysisBand::high;
}

[[nodiscard]] uint32_t production_budget(CallerPath path, AnalysisBand band) noexcept {
    if (path == CallerPath::three_lp) {
        constexpr std::array<uint32_t, 3> BUDGETS{{1000, 2000, 5000}};
        return BUDGETS[static_cast<size_t>(band)];
    }
    constexpr std::array<uint32_t, 3> BUDGETS{{2000, 5000, 20000}};
    return BUDGETS[static_cast<size_t>(band)];
}

[[nodiscard]] CellStatus translate_status(SQUFOF::MultiplierProbeStatus status) {
    switch (status) {
    case SQUFOF::MultiplierProbeStatus::ineligible_input:
        return CellStatus::ineligible_input;
    case SQUFOF::MultiplierProbeStatus::invalid_slot:
        return CellStatus::invalid_slot;
    case SQUFOF::MultiplierProbeStatus::overflow:
        return CellStatus::overflow;
    case SQUFOF::MultiplierProbeStatus::attempted:
        return CellStatus::attempted;
    }
    fail("production SQUFOF probe returned an unknown status");
}

[[nodiscard]] BudgetCell make_cell(const SQUFOF::MultiplierProbeResult& probe,
                                   uint32_t effective_cap) {
    return BudgetCell{
        .effective_cap = effective_cap,
        .status = translate_status(probe.status),
        .forward_iterations = probe.forward_iterations,
        .core_hit = probe.core_hit,
        .accepted_factor = probe.accepted_factor,
    };
}

[[nodiscard]] BudgetRow observe_row(std::span<const uint64_t> multipliers, uint64_t n,
                                    uint32_t baseline_cap) {
    BudgetRow row;
    row.n = n;
    row.baseline_cap = baseline_cap;
    row.reference_factor = SQUFOF::factor(n, baseline_cap);
    row.slots.reserve(multipliers.size());

    for (size_t slot = 0; slot < multipliers.size(); ++slot) {
        SlotBudgetCells observations;
        observations.baseline_cell =
            make_cell(SQUFOF::probe_multiplier(n, slot, baseline_cap), baseline_cap);
        for (size_t cap_index = 0; cap_index < gnfs::tests::squfof_budget::CAP_LADDER.size();
             ++cap_index) {
            const uint32_t requested = gnfs::tests::squfof_budget::CAP_LADDER[cap_index];
            const uint32_t effective =
                gnfs::tests::squfof_budget::effective_cap(baseline_cap, requested);
            observations.cells[cap_index] =
                make_cell(SQUFOF::probe_multiplier(n, slot, effective), effective);
        }
        row.slots.push_back(std::move(observations));
    }
    return row;
}

template <typename CaseAccessor>
[[nodiscard]] BudgetMatrix build_matrix(Digest corpus_identity, size_t row_count,
                                        CaseAccessor&& case_at) {
    BudgetMatrix matrix;
    matrix.corpus_identity = corpus_identity;
    const auto& schedule = SQUFOF::multiplier_schedule();
    matrix.multipliers.assign(schedule.begin(), schedule.end());
    matrix.schedule_identity = gnfs::tests::squfof_strategy::schedule_digest(matrix.multipliers);
    matrix.rows.resize(row_count);

    const unsigned reported_threads = std::max(1U, std::thread::hardware_concurrency());
    const size_t worker_count = std::min<size_t>({row_count, reported_threads, 8});
    std::atomic<size_t> next_row{0};
    std::vector<std::future<void>> workers;
    workers.reserve(worker_count);
    for (size_t worker = 0; worker < worker_count; ++worker) {
        workers.push_back(std::async(std::launch::async, [&]() {
            while (true) {
                const size_t row_index = next_row.fetch_add(1, std::memory_order_relaxed);
                if (row_index >= row_count) {
                    return;
                }
                const auto [n, cap] = case_at(row_index);
                matrix.rows[row_index] = observe_row(matrix.multipliers, n, cap);
            }
        }));
    }
    for (auto& worker : workers) {
        worker.get();
    }
    return matrix;
}

[[nodiscard]] BudgetMatrix build_legacy_matrix() {
    return build_matrix(
        {FIXED_50D_SQUFOF_STRATEGY_V1_DIGEST_LOW, FIXED_50D_SQUFOF_STRATEGY_V1_DIGEST_HIGH},
        FIXED_50D_SQUFOF_STRATEGY_V1.size(), [](size_t row) {
            const auto& test_case = FIXED_50D_SQUFOF_STRATEGY_V1[row];
            return std::pair{test_case.n, test_case.max_iterations};
        });
}

[[nodiscard]] BudgetMatrix build_prospective_matrix() {
    return build_matrix({SQUFOF_BUDGET_CORPUS_V1_DIGEST_LOW, SQUFOF_BUDGET_CORPUS_V1_DIGEST_HIGH},
                        SQUFOF_BUDGET_CORPUS_V1.size(), [](size_t row) {
                            const auto& test_case = SQUFOF_BUDGET_CORPUS_V1[row];
                            return std::pair{test_case.n, test_case.budget};
                        });
}

[[nodiscard]] StrategyMatrix baseline_strategy_projection(const BudgetMatrix& matrix) {
    StrategyMatrix projection;
    projection.corpus_identity = matrix.corpus_identity;
    projection.schedule_identity = matrix.schedule_identity;
    projection.multipliers = matrix.multipliers;
    projection.rows.reserve(matrix.rows.size());
    for (const BudgetRow& source : matrix.rows) {
        MatrixRow row;
        row.n = source.n;
        row.max_iterations = source.baseline_cap;
        row.reference_factor = source.reference_factor;
        row.cells.reserve(source.slots.size());
        for (const SlotBudgetCells& slot : source.slots) {
            row.cells.push_back(MatrixCell{
                .status = slot.baseline_cell.status,
                .forward_iterations = slot.baseline_cell.forward_iterations,
                .core_hit = slot.baseline_cell.core_hit,
                .accepted_factor = slot.baseline_cell.accepted_factor,
            });
        }
        projection.rows.push_back(std::move(row));
    }
    return projection;
}

[[nodiscard]] CallerPath legacy_caller_path(size_t row_index) {
    const size_t hard_begin = FIXED_50D_SQUFOF_STRATEGY_V1_SERIAL_ORACLE_COUNT;
    const size_t supplement_begin = hard_begin + FIXED_50D_SQUFOF_STRATEGY_V1_HARD_3LP_COUNT;
    if (row_index < hard_begin) {
        return CallerPath::normal_2lp;
    }
    if (row_index < supplement_begin) {
        return CallerPath::three_lp;
    }
    return FIXED_50D_SQUFOF_STRATEGY_V1[row_index].max_iterations == 5000 ? CallerPath::three_lp
                                                                          : CallerPath::normal_2lp;
}

[[nodiscard]] std::vector<RowMetadata> legacy_metadata(const BudgetMatrix& matrix,
                                                       SplitAssignment& v2_splits) {
    const StrategyMatrix projection = baseline_strategy_projection(matrix);
    gnfs::tests::squfof_strategy::validate_matrix(projection);
    const Digest projection_id = gnfs::tests::squfof_strategy::matrix_digest(projection);
    constexpr Digest EXPECTED_V2_MATRIX{UINT64_C(13897133093001924776),
                                        UINT64_C(2379028879204942237)};
    require(projection_id == EXPECTED_V2_MATRIX,
            "legacy production projection differs from the frozen V2 matrix");
    v2_splits = gnfs::tests::squfof_strategy::stratified_split_assignment(projection);
    const Digest split_id =
        gnfs::tests::squfof_strategy::split_assignment_digest(projection, v2_splits);
    constexpr Digest EXPECTED_V2_SPLIT{UINT64_C(11247766345367590209),
                                       UINT64_C(16940138875728727164)};
    require(split_id == EXPECTED_V2_SPLIT,
            "legacy published split differs from the frozen V2 identity");

    std::vector<RowMetadata> metadata;
    metadata.reserve(matrix.rows.size());
    for (size_t row = 0; row < matrix.rows.size(); ++row) {
        const CallerPath caller = legacy_caller_path(row);
        const AnalysisBand band = analysis_band_for_n(matrix.rows[row].n);
        require(matrix.rows[row].baseline_cap == production_budget(caller, band),
                "legacy row does not match its production caller budget");
        OracleSplit split = OracleSplit::train;
        if (v2_splits[row] == DataSplit::validation) {
            split = OracleSplit::validation;
        } else if (v2_splits[row] == DataSplit::holdout) {
            split = OracleSplit::holdout;
        }
        metadata.push_back(
            {EvidenceEpoch::legacy_public, caller, band, split, matrix.rows[row].baseline_cap});
    }
    return metadata;
}

[[nodiscard]] std::vector<RowMetadata> prospective_metadata(const BudgetMatrix& matrix) {
    std::vector<RowMetadata> metadata(matrix.rows.size());
    constexpr std::array<OracleSplit, 4> CYCLE{{
        OracleSplit::train,
        OracleSplit::train,
        OracleSplit::validation,
        OracleSplit::holdout,
    }};

    for (size_t band = 0; band < 3; ++band) {
        std::array<size_t, SQUFOF_BUDGET_CORPUS_V1_UNIQUE_CASES_PER_BAND> groups{};
        for (size_t offset = 0; offset < groups.size(); ++offset) {
            groups[offset] = band * groups.size() + offset;
        }
        std::sort(groups.begin(), groups.end(), [](size_t left, size_t right) {
            const uint64_t left_n = SQUFOF_BUDGET_CORPUS_V1[left * 2].n;
            const uint64_t right_n = SQUFOF_BUDGET_CORPUS_V1[right * 2].n;
            const uint64_t left_hash = gnfs::tests::squfof_strategy::stable_n_hash(left_n);
            const uint64_t right_hash = gnfs::tests::squfof_strategy::stable_n_hash(right_n);
            return left_hash != right_hash ? left_hash < right_hash : left_n < right_n;
        });
        for (size_t rank = 0; rank < groups.size(); ++rank) {
            const size_t first_row = groups[rank] * 2;
            for (size_t path_offset = 0; path_offset < 2; ++path_offset) {
                const size_t row = first_row + path_offset;
                const auto& source = SQUFOF_BUDGET_CORPUS_V1[row];
                const CallerPath caller = source.caller_path == SqufofBudgetCallerPath::three_lp
                                              ? CallerPath::three_lp
                                              : CallerPath::normal_2lp;
                const AnalysisBand analysis_band = analysis_band_for_n(source.n);
                require(static_cast<size_t>(source.bit_band) == static_cast<size_t>(analysis_band),
                        "prospective fixture band differs from production thresholds");
                require(source.budget == production_budget(caller, analysis_band),
                        "prospective row does not match its production caller budget");
                metadata[row] = {EvidenceEpoch::prospective_locked, caller, analysis_band,
                                 CYCLE[rank % CYCLE.size()], source.budget};
            }
            require(metadata[first_row].split == metadata[first_row + 1].split,
                    "same prospective n leaked across data splits");
        }
    }
    std::array<size_t, 3> unique_by_split{};
    for (size_t group = 0; group < SQUFOF_BUDGET_CORPUS_V1.size() / 2; ++group) {
        ++unique_by_split[static_cast<size_t>(metadata[group * 2].split)];
    }
    require(unique_by_split == std::array<size_t, 3>{{48, 24, 24}},
            "prospective grouped split balance changed");
    return metadata;
}

[[nodiscard]] Digest prospective_split_digest(std::span<const RowMetadata> metadata) {
    require(metadata.size() == SQUFOF_BUDGET_CORPUS_V1.size(),
            "prospective split digest has the wrong row count");
    DigestBuilder builder("GNFS-SQUFOF-BUDGET-CORPUS-SPLIT-V1");
    builder.append_u32(1);
    builder.append_u64(SQUFOF_BUDGET_CORPUS_V1_DIGEST_LOW);
    builder.append_u64(SQUFOF_BUDGET_CORPUS_V1_DIGEST_HIGH);
    builder.append_u64(SQUFOF_BUDGET_CORPUS_V1.size());
    for (size_t row = 0; row < SQUFOF_BUDGET_CORPUS_V1.size(); ++row) {
        const auto& test_case = SQUFOF_BUDGET_CORPUS_V1[row];
        builder.append_u64(row);
        builder.append_u64(test_case.n);
        builder.append_byte(static_cast<uint8_t>(test_case.caller_path));
        builder.append_byte(static_cast<uint8_t>(test_case.bit_band));
        builder.append_u32(test_case.budget);
        builder.append_byte(static_cast<uint8_t>(metadata[row].split));
    }
    return builder.finish();
}

[[nodiscard]] std::vector<size_t> select_rows(std::span<const RowMetadata> metadata,
                                              std::optional<OracleSplit> split = std::nullopt,
                                              std::optional<CallerPath> caller = std::nullopt,
                                              std::optional<AnalysisBand> band = std::nullopt) {
    std::vector<size_t> rows;
    for (size_t row = 0; row < metadata.size(); ++row) {
        if (split.has_value() && metadata[row].split != *split) {
            continue;
        }
        if (caller.has_value() && metadata[row].caller_path != *caller) {
            continue;
        }
        if (band.has_value() && metadata[row].bit_band != *band) {
            continue;
        }
        rows.push_back(row);
    }
    return rows;
}

[[nodiscard]] size_t unique_n_count(const BudgetMatrix& matrix, std::span<const size_t> rows) {
    std::vector<uint64_t> inputs;
    inputs.reserve(rows.size());
    for (const size_t row : rows) {
        if (std::find(inputs.begin(), inputs.end(), matrix.rows[row].n) == inputs.end()) {
            inputs.push_back(matrix.rows[row].n);
        }
    }
    return inputs.size();
}

struct EvaluationRecord final {
    EvidenceEpoch epoch;
    std::string_view split;
    std::string_view caller;
    std::string_view band;
    uint32_t budget;
    std::string_view policy;
    size_t unique_inputs;
    BudgetEvaluation evaluation;
};

void emit_evaluation(const EvaluationRecord& record) {
    const BudgetEvaluation& value = record.evaluation;
    std::cout << "GNFS_SQUFOF_BUDGET_EVAL_V1"
              << " status=pass epoch=" << epoch_name(record.epoch) << " split=" << record.split
              << " caller_path=" << record.caller << " budget=" << record.budget
              << " bit_band=" << record.band << " policy=" << record.policy
              << " rows=" << value.row_count << " unique_n=" << record.unique_inputs
              << " reference_successes=" << value.reference_successes
              << " reference_failures=" << value.reference_failures
              << " candidate_successes=" << value.candidate_successes
              << " candidate_failures=" << value.candidate_failures
              << " factor_identity_mismatches=" << value.factor_identity_mismatches
              << " new_failures=" << value.new_failures << " new_successes=" << value.new_successes
              << " changed_factors=" << value.changed_factors
              << " success_forward_iterations=" << value.success_forward_iterations
              << " failure_forward_iterations=" << value.failure_forward_iterations
              << " forward_iterations=" << value.total_forward_iterations << '\n';
}

void append_records(std::vector<EvaluationRecord>& records, const BudgetMatrix& matrix,
                    std::span<const RowMetadata> metadata, EvidenceEpoch epoch,
                    std::span<const uint32_t> baseline, std::span<const uint32_t> candidate) {
    constexpr std::array<OracleSplit, 3> SPLITS{{
        OracleSplit::train,
        OracleSplit::validation,
        OracleSplit::holdout,
    }};
    constexpr std::array<CallerPath, 2> CALLERS{{
        CallerPath::three_lp,
        CallerPath::normal_2lp,
    }};
    constexpr std::array<AnalysisBand, 3> BANDS{{
        AnalysisBand::low,
        AnalysisBand::mid,
        AnalysisBand::high,
    }};

    for (const OracleSplit split : SPLITS) {
        for (const CallerPath caller : CALLERS) {
            for (const AnalysisBand band : BANDS) {
                const std::vector<size_t> rows = select_rows(metadata, split, caller, band);
                if (rows.empty()) {
                    continue;
                }
                const uint32_t budget = production_budget(caller, band);
                records.push_back(
                    {epoch, split_name(epoch, split), caller_name(caller), band_name(band), budget,
                     "baseline", unique_n_count(matrix, rows),
                     gnfs::tests::squfof_budget::evaluate_policy(matrix, rows, baseline)});
                records.push_back(
                    {epoch, split_name(epoch, split), caller_name(caller), band_name(band), budget,
                     "uniform_10056", unique_n_count(matrix, rows),
                     gnfs::tests::squfof_budget::evaluate_policy(matrix, rows, candidate)});
            }
        }
    }
}

[[nodiscard]] BudgetCell synthetic_cell(uint32_t baseline_cap, uint32_t requested_cap,
                                        uint64_t hit_iteration = 0, uint64_t factor = 0) {
    const uint32_t effective =
        gnfs::tests::squfof_budget::effective_cap(baseline_cap, requested_cap);
    const bool hit = hit_iteration != 0 && hit_iteration <= effective;
    return BudgetCell{
        .effective_cap = effective,
        .status = CellStatus::attempted,
        .forward_iterations = hit ? hit_iteration : effective,
        .core_hit = hit,
        .accepted_factor = hit ? factor : 0,
    };
}

[[nodiscard]] SlotBudgetCells synthetic_slot(uint32_t baseline_cap, uint64_t hit_iteration = 0,
                                             uint64_t factor = 0) {
    SlotBudgetCells slot;
    slot.baseline_cell = synthetic_cell(baseline_cap, BASELINE_CAP_SENTINEL, hit_iteration, factor);
    for (size_t cap_index = 0; cap_index < gnfs::tests::squfof_budget::CAP_LADDER.size();
         ++cap_index) {
        slot.cells[cap_index] = synthetic_cell(
            baseline_cap, gnfs::tests::squfof_budget::CAP_LADDER[cap_index], hit_iteration, factor);
    }
    return slot;
}

void validate_optimizer_mechanics() {
    BudgetMatrix matrix;
    matrix.corpus_identity = {1, 2};
    matrix.schedule_identity = {3, 4};
    matrix.multipliers = {1, 3};
    matrix.rows = {
        BudgetRow{.n = 15,
                  .baseline_cap = 5000,
                  .reference_factor = 3,
                  .slots = {synthetic_slot(5000, 1500, 3), synthetic_slot(5000, 500, 5)}},
        BudgetRow{.n = 77,
                  .baseline_cap = 5000,
                  .reference_factor = 1,
                  .slots = {synthetic_slot(5000), synthetic_slot(5000)}},
    };
    gnfs::tests::squfof_budget::validate_matrix(matrix);
    const std::array<size_t, 2> rows{{0, 1}};
    const OptimizationResult result =
        gnfs::tests::squfof_budget::optimize_training_rows(matrix, rows);
    require(result.feasible && result.training_cost == 4500 &&
                result.slot_caps == CapPolicy({2000, 1000}),
            "fixed-order budget optimizer ignored failure work or wrong-factor constraints");

    const CapPolicy uniform = gnfs::tests::squfof_budget::uniform_policy(matrix, 1000);
    const BudgetEvaluation evaluation =
        gnfs::tests::squfof_budget::evaluate_policy(matrix, rows, uniform);
    require(evaluation.factor_identity_mismatches == 1 && evaluation.new_failures == 0 &&
                evaluation.changed_factors == 1,
            "budget policy evaluation did not classify a changed raw factor");

    bool empty_training_rejected = false;
    try {
        const std::span<const size_t> empty;
        (void)gnfs::tests::squfof_budget::optimize_training_rows(matrix, empty);
    } catch (const std::runtime_error&) {
        empty_training_rejected = true;
    }
    require(empty_training_rejected, "budget optimizer accepted an empty training set");

    BudgetMatrix changed_schedule = matrix;
    changed_schedule.multipliers[1] = 5;
    require(gnfs::tests::squfof_budget::matrix_digest(matrix) !=
                gnfs::tests::squfof_budget::matrix_digest(changed_schedule),
            "budget matrix digest omitted multiplier values");

    bool overflow_rejected = false;
    try {
        (void)gnfs::tests::squfof_budget::checked_add(std::numeric_limits<uint64_t>::max(),
                                                      UINT64_C(1));
    } catch (const std::overflow_error&) {
        overflow_rejected = true;
    }
    require(overflow_rejected, "budget iteration aggregation silently overflowed");
}

[[nodiscard]] std::string policy_string(std::span<const uint32_t> policy) {
    std::string output;
    for (size_t slot = 0; slot < policy.size(); ++slot) {
        if (slot != 0) {
            output.push_back(',');
        }
        output += gnfs::tests::squfof_budget::cap_to_string(policy[slot]);
    }
    return output;
}

[[nodiscard]] bool minimum_gain(uint64_t baseline, uint64_t candidate, uint64_t percent) {
    require(percent <= 100 && baseline <= std::numeric_limits<uint64_t>::max() / 100 &&
                candidate <= std::numeric_limits<uint64_t>::max() / 100,
            "budget gain comparison is out of range");
    return candidate * 100 <= baseline * (100 - percent);
}

[[nodiscard]] std::string sanitize_token(std::string_view input) {
    std::string output;
    output.reserve(std::min<size_t>(input.size(), 200));
    for (const char character : input) {
        const auto byte = static_cast<unsigned char>(character);
        const bool safe = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                          (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte == '.';
        output.push_back(safe ? static_cast<char>(byte) : '_');
        if (output.size() == 200) {
            break;
        }
    }
    return output.empty() ? "unknown" : output;
}

void run_oracle() {
    validate_optimizer_mechanics();

    const BudgetMatrix legacy = build_legacy_matrix();
    const BudgetMatrix prospective = build_prospective_matrix();
    gnfs::tests::squfof_budget::validate_matrix(legacy);
    gnfs::tests::squfof_budget::validate_matrix(prospective);
    constexpr Digest EXPECTED_SCHEDULE{UINT64_C(12597619067809015512),
                                       UINT64_C(9409246306371504405)};
    require(legacy.schedule_identity == EXPECTED_SCHEDULE &&
                prospective.schedule_identity == EXPECTED_SCHEDULE,
            "production SQUFOF multiplier schedule changed");

    const Digest legacy_matrix_id = gnfs::tests::squfof_budget::matrix_digest(legacy);
    const Digest prospective_matrix_id = gnfs::tests::squfof_budget::matrix_digest(prospective);
    constexpr Digest EXPECTED_LEGACY_MATRIX{UINT64_C(17887356378357031550),
                                            UINT64_C(10510892644681870966)};
    constexpr Digest EXPECTED_PROSPECTIVE_MATRIX{UINT64_C(10761457262857602067),
                                                 UINT64_C(5850380446892829808)};
    require(legacy_matrix_id == EXPECTED_LEGACY_MATRIX &&
                prospective_matrix_id == EXPECTED_PROSPECTIVE_MATRIX,
            "SQUFOF budget matrix identity changed");

    SplitAssignment v2_splits;
    const std::vector<RowMetadata> legacy_meta = legacy_metadata(legacy, v2_splits);
    const std::vector<RowMetadata> prospective_meta = prospective_metadata(prospective);
    constexpr Digest EXPECTED_PROSPECTIVE_SPLIT{UINT64_C(17722147925989565997),
                                                UINT64_C(4435973663510799258)};
    require(prospective_split_digest(prospective_meta) == EXPECTED_PROSPECTIVE_SPLIT,
            "prospective oracle split differs from the sealed corpus contract");
    const CapPolicy legacy_baseline = gnfs::tests::squfof_budget::baseline_policy(legacy);
    const CapPolicy prospective_baseline = gnfs::tests::squfof_budget::baseline_policy(prospective);
    const CapPolicy legacy_candidate = gnfs::tests::squfof_budget::uniform_policy(legacy, 10056);
    const CapPolicy prospective_candidate =
        gnfs::tests::squfof_budget::uniform_policy(prospective, 10056);

    const BudgetEvaluation legacy_base =
        gnfs::tests::squfof_budget::evaluate_policy(legacy, legacy_baseline);
    const BudgetEvaluation legacy_uniform =
        gnfs::tests::squfof_budget::evaluate_policy(legacy, legacy_candidate);
    const BudgetEvaluation prospective_base =
        gnfs::tests::squfof_budget::evaluate_policy(prospective, prospective_baseline);
    const BudgetEvaluation prospective_uniform =
        gnfs::tests::squfof_budget::evaluate_policy(prospective, prospective_candidate);
    require(legacy_base.row_count == 192 && legacy_base.reference_successes == 166 &&
                legacy_base.reference_failures == 26 &&
                legacy_base.factor_identity_mismatches == 0 &&
                legacy_base.total_forward_iterations == UINT64_C(2584580),
            "legacy budget baseline differs from the frozen production result");
    require(legacy_uniform.factor_identity_mismatches == 0 &&
                legacy_uniform.total_forward_iterations == UINT64_C(1888500),
            "preregistered cap differs from its legacy screening result");
    require(prospective_base.row_count == 192 && prospective_base.reference_successes == 117 &&
                prospective_base.reference_failures == 75 &&
                prospective_base.factor_identity_mismatches == 0 &&
                prospective_base.total_forward_iterations == UINT64_C(6938458),
            "prospective budget baseline differs from its sealed first observation");
    require(prospective_uniform.factor_identity_mismatches == 0 &&
                prospective_uniform.total_forward_iterations == UINT64_C(4512122),
            "preregistered cap differs from its prospective first observation");

    const std::vector<size_t> target_rows =
        select_rows(prospective_meta, std::nullopt, CallerPath::normal_2lp, AnalysisBand::high);
    const BudgetEvaluation target_base =
        gnfs::tests::squfof_budget::evaluate_policy(prospective, target_rows, prospective_baseline);
    const BudgetEvaluation target_candidate = gnfs::tests::squfof_budget::evaluate_policy(
        prospective, target_rows, prospective_candidate);
    require(target_base.row_count == 32 && target_base.reference_successes == 0 &&
                target_base.reference_failures == 32 &&
                target_base.total_forward_iterations == UINT64_C(4880000),
            "prospective high normal-2LP coverage changed");
    require(target_candidate.factor_identity_mismatches == 0 &&
                target_candidate.total_forward_iterations == UINT64_C(2453664),
            "prospective high normal-2LP candidate replay changed");

    const std::vector<size_t> legacy_training = select_rows(legacy_meta, OracleSplit::train);
    const std::vector<size_t> prospective_training =
        select_rows(prospective_meta, OracleSplit::train);
    const OptimizationResult legacy_optimal =
        gnfs::tests::squfof_budget::optimize_training_rows(legacy, legacy_training);
    const OptimizationResult prospective_optimal =
        gnfs::tests::squfof_budget::optimize_training_rows(prospective, prospective_training);
    require(legacy_optimal.feasible && prospective_optimal.feasible,
            "exact fixed-order budget optimizer found no feasible policy");
    const CapPolicy expected_legacy_optimal{
        5000, 5000, 2000, 5000, 1000, 1000, BASELINE_CAP_SENTINEL, 1000, 1000, 1000, 1000};
    const CapPolicy expected_prospective_optimal{2000, 5000, 5000, 5000, 2000, 5000,
                                                 1000, 1000, 1000, 5000, 1000};
    require(legacy_optimal.slot_caps == expected_legacy_optimal &&
                legacy_optimal.training_cost == UINT64_C(775188) &&
                prospective_optimal.slot_caps == expected_prospective_optimal &&
                prospective_optimal.training_cost == UINT64_C(1194736),
            "exact fixed-order budget optimizer changed its frozen result");
    const BudgetEvaluation legacy_optimal_train = gnfs::tests::squfof_budget::evaluate_policy(
        legacy, legacy_training, legacy_optimal.slot_caps);
    const BudgetEvaluation prospective_optimal_train = gnfs::tests::squfof_budget::evaluate_policy(
        prospective, prospective_training, prospective_optimal.slot_caps);
    require(legacy_optimal_train.factor_identity_mismatches == 0 &&
                legacy_optimal_train.total_forward_iterations == legacy_optimal.training_cost &&
                prospective_optimal_train.factor_identity_mismatches == 0 &&
                prospective_optimal_train.total_forward_iterations ==
                    prospective_optimal.training_cost,
            "exact budget optimizer does not replay its training result");
    const std::vector<size_t> legacy_validation = select_rows(legacy_meta, OracleSplit::validation);
    const std::vector<size_t> legacy_holdout = select_rows(legacy_meta, OracleSplit::holdout);
    const std::vector<size_t> prospective_validation =
        select_rows(prospective_meta, OracleSplit::validation);
    const std::vector<size_t> prospective_holdout =
        select_rows(prospective_meta, OracleSplit::holdout);
    const BudgetEvaluation legacy_optimal_validation = gnfs::tests::squfof_budget::evaluate_policy(
        legacy, legacy_validation, legacy_optimal.slot_caps);
    const BudgetEvaluation legacy_optimal_holdout = gnfs::tests::squfof_budget::evaluate_policy(
        legacy, legacy_holdout, legacy_optimal.slot_caps);
    const BudgetEvaluation prospective_optimal_validation =
        gnfs::tests::squfof_budget::evaluate_policy(prospective, prospective_validation,
                                                    prospective_optimal.slot_caps);
    const BudgetEvaluation prospective_optimal_holdout =
        gnfs::tests::squfof_budget::evaluate_policy(prospective, prospective_holdout,
                                                    prospective_optimal.slot_caps);
    require(legacy_optimal_validation.factor_identity_mismatches == 2 &&
                legacy_optimal_holdout.factor_identity_mismatches == 1 &&
                prospective_optimal_validation.factor_identity_mismatches == 0 &&
                prospective_optimal_holdout.factor_identity_mismatches == 3,
            "train-only budget policy generalization result changed");

    const bool identity_gate = legacy_uniform.factor_identity_mismatches == 0 &&
                               prospective_uniform.factor_identity_mismatches == 0;
    constexpr size_t MINIMUM_PROSPECTIVE_TARGET_SUCCESSES = 8;
    const bool coverage_gate =
        target_base.reference_successes >= MINIMUM_PROSPECTIVE_TARGET_SUCCESSES;
    const bool work_gate = minimum_gain(target_base.total_forward_iterations,
                                        target_candidate.total_forward_iterations, 3);
    const std::string_view decision = !identity_gate   ? "no_go"
                                      : !coverage_gate ? "insufficient_evidence"
                                      : !work_gate     ? "no_go"
                                                       : "eligible";
    require(decision == "insufficient_evidence",
            "sealed V1 budget evidence unexpectedly changed promotion state");

    std::vector<EvaluationRecord> records;
    append_records(records, legacy, legacy_meta, EvidenceEpoch::legacy_public, legacy_baseline,
                   legacy_candidate);
    append_records(records, prospective, prospective_meta, EvidenceEpoch::prospective_locked,
                   prospective_baseline, prospective_candidate);
    for (const EvaluationRecord& record : records) {
        emit_evaluation(record);
    }
    std::cout << "GNFS_SQUFOF_BUDGET_ORACLE_SUMMARY_V1"
              << " status=pass schema=1 offline_decision=" << decision
              << " identity_gate=" << (identity_gate ? "pass" : "fail")
              << " prospective_success_coverage_gate=" << (coverage_gate ? "pass" : "fail")
              << " prospective_work_gate=" << (work_gate ? "pass" : "fail")
              << " v2_holdout_role=published_retrospective"
              << " confirmation_role=prospective_locked"
              << " timing_asserted=false"
              << " target_rows=" << target_base.row_count
              << " target_baseline_successes=" << target_base.reference_successes
              << " target_minimum_successes=" << MINIMUM_PROSPECTIVE_TARGET_SUCCESSES
              << " target_baseline_iterations=" << target_base.total_forward_iterations
              << " target_candidate_iterations=" << target_candidate.total_forward_iterations
              << " legacy_matrix_digest_low=" << legacy_matrix_id.low
              << " legacy_matrix_digest_high=" << legacy_matrix_id.high
              << " prospective_matrix_digest_low=" << prospective_matrix_id.low
              << " prospective_matrix_digest_high=" << prospective_matrix_id.high
              << " candidate_policy=" << policy_string(prospective_candidate)
              << " legacy_train_optimal_policy=" << policy_string(legacy_optimal.slot_caps)
              << " legacy_train_optimal_cost=" << legacy_optimal.training_cost
              << " legacy_optimal_validation_mismatches="
              << legacy_optimal_validation.factor_identity_mismatches
              << " legacy_optimal_published_holdout_mismatches="
              << legacy_optimal_holdout.factor_identity_mismatches
              << " prospective_train_optimal_policy="
              << policy_string(prospective_optimal.slot_caps)
              << " prospective_train_optimal_cost=" << prospective_optimal.training_cost
              << " prospective_optimal_validation_mismatches="
              << prospective_optimal_validation.factor_identity_mismatches
              << " prospective_optimal_confirmation_mismatches="
              << prospective_optimal_holdout.factor_identity_mismatches << '\n';
}

} // namespace

int main() {
    try {
        run_oracle();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GNFS_SQUFOF_BUDGET_ORACLE_SUMMARY_V1 status=fail error="
                  << sanitize_token(error.what()) << '\n';
        return 1;
    } catch (...) {
        std::cerr << "GNFS_SQUFOF_BUDGET_ORACLE_SUMMARY_V1"
                     " status=fail error=unknown_exception\n";
        return 1;
    }
}
