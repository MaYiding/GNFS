// Exact counterfactual strategy oracle for the fixed SQUFOF V1 corpus.

#include <gnfs/cofactor/squfof.hpp>

#include "fixtures/squfof_strategy_corpus_v1.hpp"
#include "support/squfof_strategy_optimizer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using gnfs::cofactor::SQUFOF;
using gnfs::tests::fixtures::FIXED_50D_SQUFOF_STRATEGY_V1;
using gnfs::tests::fixtures::FIXED_50D_SQUFOF_STRATEGY_V1_DIGEST_HIGH;
using gnfs::tests::fixtures::FIXED_50D_SQUFOF_STRATEGY_V1_DIGEST_LOW;
using gnfs::tests::squfof_strategy::BitBand;
using gnfs::tests::squfof_strategy::CellStatus;
using gnfs::tests::squfof_strategy::DataSplit;
using gnfs::tests::squfof_strategy::Digest;
using gnfs::tests::squfof_strategy::MatrixCell;
using gnfs::tests::squfof_strategy::MatrixRow;
using gnfs::tests::squfof_strategy::OptimizationResult;
using gnfs::tests::squfof_strategy::SplitAssignment;
using gnfs::tests::squfof_strategy::StrategyEvaluation;
using gnfs::tests::squfof_strategy::StrategyMatrix;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
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

[[nodiscard]] StrategyMatrix build_complete_matrix() {
    StrategyMatrix matrix;
    matrix.corpus_identity = {FIXED_50D_SQUFOF_STRATEGY_V1_DIGEST_LOW,
                              FIXED_50D_SQUFOF_STRATEGY_V1_DIGEST_HIGH};
    const auto& production_schedule = SQUFOF::multiplier_schedule();
    matrix.multipliers.assign(production_schedule.begin(), production_schedule.end());
    matrix.schedule_identity = gnfs::tests::squfof_strategy::schedule_digest(matrix.multipliers);
    matrix.rows.reserve(FIXED_50D_SQUFOF_STRATEGY_V1.size());

    for (size_t row_index = 0; row_index < FIXED_50D_SQUFOF_STRATEGY_V1.size(); ++row_index) {
        const auto& test_case = FIXED_50D_SQUFOF_STRATEGY_V1[row_index];
        MatrixRow row;
        row.n = test_case.n;
        row.max_iterations = test_case.max_iterations;
        row.reference_factor = SQUFOF::factor(test_case.n, test_case.max_iterations);
        row.cells.reserve(matrix.multipliers.size());

        for (size_t slot = 0; slot < matrix.multipliers.size(); ++slot) {
            const SQUFOF::MultiplierProbeResult probe =
                SQUFOF::probe_multiplier(test_case.n, slot, test_case.max_iterations);
            row.cells.push_back(MatrixCell{
                .status = translate_status(probe.status),
                .forward_iterations = probe.forward_iterations,
                .core_hit = probe.core_hit,
                .accepted_factor = probe.accepted_factor,
            });
        }
        matrix.rows.push_back(std::move(row));
    }
    return matrix;
}

[[nodiscard]] MatrixCell attempted(uint64_t iterations, uint64_t factor = 0) {
    return MatrixCell{
        .status = CellStatus::attempted,
        .forward_iterations = iterations,
        .core_hit = factor > 1,
        .accepted_factor = factor,
    };
}

void validate_optimizer_mechanics() {
    StrategyMatrix constrained;
    constrained.multipliers = {1, 3, 5};
    constrained.schedule_identity =
        gnfs::tests::squfof_strategy::schedule_digest(constrained.multipliers);
    constrained.rows = {
        MatrixRow{
            .n = 15,
            .max_iterations = 10,
            .reference_factor = 3,
            .cells = {attempted(5), attempted(2, 3), attempted(1, 5)},
        },
        MatrixRow{
            .n = 35,
            .max_iterations = 10,
            .reference_factor = 5,
            .cells = {attempted(3, 5), attempted(1), attempted(4)},
        },
        MatrixRow{
            .n = 77,
            .max_iterations = 10,
            .reference_factor = 1,
            .cells = {attempted(2), attempted(4), attempted(6)},
        },
    };
    const std::vector<size_t> constrained_rows =
        gnfs::tests::squfof_strategy::all_rows(constrained);
    const SplitAssignment constrained_splits(constrained.rows.size(), DataSplit::train);
    const OptimizationResult constrained_result =
        gnfs::tests::squfof_strategy::optimize_training_rows(constrained, constrained_rows,
                                                             constrained_splits);
    require(constrained_result.feasible &&
                constrained_result.order == std::vector<size_t>({1, 0, 2}),
            "subset DP ignored exact-factor transition constraints or cost ordering");
    require(constrained_result.training_cost == 18,
            "subset DP did not charge every unresolved row, including failures");
    const StrategyEvaluation constrained_replay = gnfs::tests::squfof_strategy::evaluate_strategy(
        constrained, constrained_rows, constrained_result.order);
    require(constrained_replay.factor_identity_mismatches == 0 &&
                constrained_replay.forward_iterations == constrained_result.training_cost,
            "synthetic optimized order does not replay its DP result exactly");

    StrategyMatrix wrong_factor_first;
    wrong_factor_first.multipliers = {1, 3};
    wrong_factor_first.rows = {MatrixRow{
        .n = 15,
        .max_iterations = 100,
        .reference_factor = 3,
        .cells = {attempted(0, 5), attempted(100, 3)},
    }};
    const std::vector<size_t> wrong_factor_rows =
        gnfs::tests::squfof_strategy::all_rows(wrong_factor_first);
    const SplitAssignment wrong_factor_splits(wrong_factor_first.rows.size(), DataSplit::train);
    const OptimizationResult wrong_factor_result =
        gnfs::tests::squfof_strategy::optimize_training_rows(wrong_factor_first, wrong_factor_rows,
                                                             wrong_factor_splits);
    require(wrong_factor_result.feasible &&
                wrong_factor_result.order == std::vector<size_t>({1, 0}) &&
                wrong_factor_result.training_cost == 100,
            "subset DP allowed a cheaper complementary factor to win first");

    StrategyMatrix tied;
    tied.multipliers = {1, 3, 5};
    tied.schedule_identity = gnfs::tests::squfof_strategy::schedule_digest(tied.multipliers);
    tied.rows = {MatrixRow{
        .n = 77,
        .max_iterations = 10,
        .reference_factor = 1,
        .cells = {attempted(2), attempted(4), attempted(6)},
    }};
    const std::vector<size_t> tied_rows = gnfs::tests::squfof_strategy::all_rows(tied);
    const SplitAssignment tied_splits(tied.rows.size(), DataSplit::train);
    const OptimizationResult tied_result =
        gnfs::tests::squfof_strategy::optimize_training_rows(tied, tied_rows, tied_splits);
    require(tied_result.feasible && tied_result.order == std::vector<size_t>({0, 1, 2}),
            "subset DP tie-break is not lexicographically deterministic");

    StrategyMatrix unreachable;
    unreachable.multipliers = {1, 3};
    unreachable.rows = {MatrixRow{
        .n = 91,
        .max_iterations = 10,
        .reference_factor = 7,
        .cells = {attempted(2), attempted(3)},
    }};
    bool unreachable_rejected = false;
    try {
        gnfs::tests::squfof_strategy::validate_matrix(unreachable);
    } catch (const std::runtime_error&) {
        unreachable_rejected = true;
    }
    require(unreachable_rejected,
            "strategy matrix accepted an unreachable production reference factor");

    bool overflow_rejected = false;
    try {
        (void)gnfs::tests::squfof_strategy::checked_add(std::numeric_limits<uint64_t>::max(),
                                                        UINT64_C(1));
    } catch (const std::overflow_error&) {
        overflow_rejected = true;
    }
    require(overflow_rejected, "strategy iteration aggregation silently overflowed");
    overflow_rejected = false;
    try {
        (void)gnfs::tests::squfof_strategy::checked_multiply(std::numeric_limits<uint64_t>::max(),
                                                             UINT64_C(2));
    } catch (const std::overflow_error&) {
        overflow_rejected = true;
    }
    require(overflow_rejected, "strategy percentage comparison silently overflowed");

    require(
        gnfs::tests::squfof_strategy::bit_band_for_n(UINT64_C(1) << 39) == BitBand::through_40 &&
            gnfs::tests::squfof_strategy::bit_band_for_n(UINT64_C(1) << 40) ==
                BitBand::bits_41_to_43 &&
            gnfs::tests::squfof_strategy::bit_band_for_n(UINT64_C(1) << 43) ==
                BitBand::bits_44_to_46 &&
            gnfs::tests::squfof_strategy::bit_band_for_n(UINT64_C(1) << 46) == BitBand::above_46,
        "SQUFOF strategy bit-band boundaries changed");
}

void validate_complete_counterfactuals(const StrategyMatrix& matrix) {
    require(matrix.rows.size() == FIXED_50D_SQUFOF_STRATEGY_V1.size(),
            "strategy matrix does not cover all fixed corpus rows");
    require(matrix.multipliers.size() == SQUFOF::diagnostic_slot_count,
            "strategy matrix does not cover all production multiplier slots");

    size_t cell_count = 0;
    for (size_t row_index = 0; row_index < matrix.rows.size(); ++row_index) {
        const MatrixRow& row = matrix.rows[row_index];
        require(row.cells.size() == matrix.multipliers.size(),
                "strategy matrix row has incomplete multiplier coverage");
        for (size_t slot = 0; slot < row.cells.size(); ++slot) {
            const MatrixCell& cell = row.cells[slot];
            require(
                cell.status == CellStatus::attempted || cell.status == CellStatus::overflow,
                "valid corpus/slot pair unexpectedly produced an ineligible probe status at row " +
                    std::to_string(row_index) + " slot " + std::to_string(slot));
            require(
                cell.status == CellStatus::attempted ||
                    (cell.forward_iterations == 0 && !cell.core_hit && cell.accepted_factor == 0),
                "overflow cell contains attempted work or a hit");
            require(cell.accepted_factor == 0 ||
                        gnfs::tests::squfof_strategy::is_proper_factor(row.n, cell.accepted_factor),
                    "probe returned an invalid accepted factor");
            ++cell_count;
        }
    }
    require(cell_count == matrix.rows.size() * matrix.multipliers.size(),
            "strategy matrix cell count is incomplete");
}

void validate_split_isolation(const StrategyMatrix& matrix, std::span<const DataSplit> assignment) {
    gnfs::tests::squfof_strategy::validate_split_assignment(matrix, assignment);
    std::unordered_map<uint64_t, DataSplit> split_by_n;
    for (size_t row_index = 0; row_index < matrix.rows.size(); ++row_index) {
        const MatrixRow& row = matrix.rows[row_index];
        const DataSplit split = assignment[row_index];
        const auto [position, inserted] = split_by_n.emplace(row.n, split);
        require(inserted || position->second == split,
                "identical n values leaked across strategy data splits");
    }
}

[[nodiscard]] std::string order_string(const StrategyMatrix& matrix,
                                       std::span<const size_t> order) {
    std::string text;
    for (size_t index = 0; index < order.size(); ++index) {
        if (index != 0) {
            text.push_back(',');
        }
        text += std::to_string(matrix.multipliers[order[index]]);
    }
    return text;
}

[[nodiscard]] bool meets_minimum_gain(uint64_t baseline, uint64_t candidate,
                                      uint64_t minimum_percent) {
    require(minimum_percent <= 100, "invalid minimum strategy gain");
    if (baseline == 0) {
        return false;
    }
    return gnfs::tests::squfof_strategy::checked_multiply(candidate, 100) <=
           gnfs::tests::squfof_strategy::checked_multiply(baseline, 100 - minimum_percent);
}

[[nodiscard]] bool stays_within_regression(uint64_t baseline, uint64_t candidate,
                                           uint64_t maximum_percent) {
    require(maximum_percent <= 100, "invalid maximum strategy regression");
    return gnfs::tests::squfof_strategy::checked_multiply(candidate, 100) <=
           gnfs::tests::squfof_strategy::checked_multiply(baseline, 100 + maximum_percent);
}

struct EvaluationRecord final {
    std::string_view split;
    std::string_view band;
    std::string_view policy;
    StrategyEvaluation evaluation;
};

void emit_evaluation(const EvaluationRecord& record) {
    std::cout << "GNFS_SQUFOF_STRATEGY_EVAL_V2" << " status=pass" << " identity_preserved="
              << (record.evaluation.factor_identity_mismatches == 0 ? "true" : "false")
              << " split=" << record.split << " bit_band=" << record.band
              << " policy=" << record.policy << " rows=" << record.evaluation.row_count
              << " reference_successes=" << record.evaluation.reference_successes
              << " reference_failures=" << record.evaluation.reference_failures
              << " factor_identity_mismatches=" << record.evaluation.factor_identity_mismatches
              << " forward_iterations="
              << gnfs::tests::squfof_strategy::cost_to_string(record.evaluation.forward_iterations)
              << '\n';
}

void append_evaluations(std::vector<EvaluationRecord>& records, const StrategyMatrix& matrix,
                        std::string_view split, std::string_view band, std::span<const size_t> rows,
                        std::span<const size_t> current, std::span<const size_t> optimized) {
    const StrategyEvaluation current_evaluation =
        gnfs::tests::squfof_strategy::evaluate_strategy(matrix, rows, current);
    const StrategyEvaluation optimized_evaluation =
        gnfs::tests::squfof_strategy::evaluate_strategy(matrix, rows, optimized);
    require(current_evaluation.factor_identity_mismatches == 0,
            "current strategy replay changed a production factor identity");
    records.push_back({split, band, "current", current_evaluation});
    records.push_back({split, band, "train_optimal", optimized_evaluation});
}

[[nodiscard]] std::string sanitize_token(std::string_view input) {
    std::string output;
    output.reserve(input.size() < 200 ? input.size() : 200);
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
    const StrategyMatrix matrix = build_complete_matrix();
    gnfs::tests::squfof_strategy::validate_matrix(matrix);
    validate_complete_counterfactuals(matrix);

    constexpr Digest EXPECTED_SCHEDULE_ID{UINT64_C(12597619067809015512),
                                          UINT64_C(9409246306371504405)};
    require(matrix.schedule_identity == EXPECTED_SCHEDULE_ID,
            "production multiplier schedule differs from the frozen V1 identity");

    const Digest matrix_id = gnfs::tests::squfof_strategy::matrix_digest(matrix);
    constexpr Digest EXPECTED_MATRIX_ID{UINT64_C(13897133093001924776),
                                        UINT64_C(2379028879204942237)};
    require(matrix_id == EXPECTED_MATRIX_ID,
            "counterfactual SQUFOF matrix differs from the frozen V2 identity");
    require(matrix_id == gnfs::tests::squfof_strategy::matrix_digest(matrix),
            "strategy matrix digest is not deterministic");
    StrategyMatrix changed_matrix = matrix;
    ++changed_matrix.rows.front().cells.front().forward_iterations;
    require(matrix_id != gnfs::tests::squfof_strategy::matrix_digest(changed_matrix),
            "strategy matrix digest omitted a counterfactual cell field");

    const std::vector<size_t> current = gnfs::tests::squfof_strategy::current_order(matrix);
    const std::vector<size_t> all = gnfs::tests::squfof_strategy::all_rows(matrix);
    const SplitAssignment splits =
        gnfs::tests::squfof_strategy::stratified_split_assignment(matrix);
    validate_split_isolation(matrix, splits);
    const Digest split_id = gnfs::tests::squfof_strategy::split_assignment_digest(matrix, splits);
    constexpr Digest EXPECTED_SPLIT_ID{UINT64_C(11247766345367590209),
                                       UINT64_C(16940138875728727164)};
    require(split_id == EXPECTED_SPLIT_ID,
            "stratified SQUFOF train/validation/holdout split changed");
    const std::vector<size_t> training =
        gnfs::tests::squfof_strategy::rows_for_split(matrix, splits, DataSplit::train);
    const std::vector<size_t> validation =
        gnfs::tests::squfof_strategy::rows_for_split(matrix, splits, DataSplit::validation);
    const std::vector<size_t> holdout =
        gnfs::tests::squfof_strategy::rows_for_split(matrix, splits, DataSplit::holdout);
    require(!training.empty() && !validation.empty() && !holdout.empty(),
            "stable 50/25/25 strategy split produced an empty partition");

    const OptimizationResult optimized =
        gnfs::tests::squfof_strategy::optimize_training_rows(matrix, training, splits);
    require(optimized.feasible, "exact subset DP found no factor-identity-preserving order");
    require(optimized.order.size() == matrix.multipliers.size(),
            "exact subset DP returned an incomplete multiplier order");
    const std::vector<size_t> expected_optimized_order{0, 1, 2, 7, 6, 3, 4, 5, 8, 9, 10};
    require(optimized.order == expected_optimized_order &&
                optimized.training_cost == UINT64_C(1588918),
            "train-only SQUFOF strategy optimizer differs from the frozen V2 result");

    const StrategyEvaluation current_all =
        gnfs::tests::squfof_strategy::evaluate_strategy(matrix, all, current);
    const StrategyEvaluation optimized_all =
        gnfs::tests::squfof_strategy::evaluate_strategy(matrix, all, optimized.order);
    const StrategyEvaluation current_training =
        gnfs::tests::squfof_strategy::evaluate_strategy(matrix, training, current);
    const StrategyEvaluation training_optimal =
        gnfs::tests::squfof_strategy::evaluate_strategy(matrix, training, optimized.order);
    const StrategyEvaluation current_validation =
        gnfs::tests::squfof_strategy::evaluate_strategy(matrix, validation, current);
    const StrategyEvaluation optimized_validation =
        gnfs::tests::squfof_strategy::evaluate_strategy(matrix, validation, optimized.order);
    const StrategyEvaluation current_holdout =
        gnfs::tests::squfof_strategy::evaluate_strategy(matrix, holdout, current);
    const StrategyEvaluation optimized_holdout =
        gnfs::tests::squfof_strategy::evaluate_strategy(matrix, holdout, optimized.order);
    require(current_all.row_count == 192 && current_all.reference_successes == 166 &&
                current_all.reference_failures == 26 &&
                current_all.factor_identity_mismatches == 0 &&
                current_all.forward_iterations == UINT64_C(2584580),
            "current-order replay differs from the frozen V1 production baseline");
    require(training_optimal.factor_identity_mismatches == 0,
            "exact subset DP did not preserve training factor identities");
    require(training_optimal.forward_iterations == optimized.training_cost,
            "exact subset DP cost differs from an independent strategy replay");

    constexpr std::array<BitBand, 4> BANDS{{
        BitBand::through_40,
        BitBand::bits_41_to_43,
        BitBand::bits_44_to_46,
        BitBand::above_46,
    }};
    constexpr std::array<DataSplit, 3> SPLITS{{
        DataSplit::train,
        DataSplit::validation,
        DataSplit::holdout,
    }};
    const bool identity_gate = optimized_all.factor_identity_mismatches == 0;
    const bool train_gate = meets_minimum_gain(current_training.forward_iterations,
                                               training_optimal.forward_iterations, 5);
    const bool validation_gate = optimized_validation.factor_identity_mismatches == 0 &&
                                 meets_minimum_gain(current_validation.forward_iterations,
                                                    optimized_validation.forward_iterations, 3);
    const bool holdout_gate = optimized_holdout.factor_identity_mismatches == 0 &&
                              meets_minimum_gain(current_holdout.forward_iterations,
                                                 optimized_holdout.forward_iterations, 3);
    bool bit_band_gate = true;
    for (const BitBand band : BANDS) {
        const std::vector<size_t> rows = gnfs::tests::squfof_strategy::rows_for_band(matrix, band);
        const StrategyEvaluation baseline =
            gnfs::tests::squfof_strategy::evaluate_strategy(matrix, rows, current);
        const StrategyEvaluation candidate =
            gnfs::tests::squfof_strategy::evaluate_strategy(matrix, rows, optimized.order);
        bit_band_gate =
            bit_band_gate && candidate.factor_identity_mismatches == 0 &&
            stays_within_regression(baseline.forward_iterations, candidate.forward_iterations, 1);
    }
    const bool promotion_ready =
        identity_gate && train_gate && validation_gate && holdout_gate && bit_band_gate;
    require(!promotion_ready,
            "frozen V2 candidate unexpectedly passed; review it before production promotion");

    std::vector<EvaluationRecord> evaluation_records;
    evaluation_records.reserve(40);
    append_evaluations(evaluation_records, matrix, "all", "all", all, current, optimized.order);
    append_evaluations(evaluation_records, matrix, "train", "all", training, current,
                       optimized.order);
    append_evaluations(evaluation_records, matrix, "validation", "all", validation, current,
                       optimized.order);
    append_evaluations(evaluation_records, matrix, "holdout", "all", holdout, current,
                       optimized.order);

    for (const BitBand band : BANDS) {
        const std::vector<size_t> band_rows =
            gnfs::tests::squfof_strategy::rows_for_band(matrix, band);
        require(!band_rows.empty(), "SQUFOF strategy bit band is empty");
        append_evaluations(evaluation_records, matrix, "all",
                           gnfs::tests::squfof_strategy::bit_band_name(band), band_rows, current,
                           optimized.order);
        for (const DataSplit split : SPLITS) {
            const std::vector<size_t> rows =
                gnfs::tests::squfof_strategy::rows_for_split_and_band(matrix, splits, split, band);
            require(!rows.empty(), "stratified SQUFOF split contains an empty bit band");
            append_evaluations(
                evaluation_records, matrix, gnfs::tests::squfof_strategy::split_name(split),
                gnfs::tests::squfof_strategy::bit_band_name(band), rows, current, optimized.order);
        }
    }

    for (const EvaluationRecord& record : evaluation_records) {
        emit_evaluation(record);
    }
    std::cout << "GNFS_SQUFOF_STRATEGY_ORACLE_SUMMARY_V2" << " status=pass schema=2"
              << " promotion=no_go" << " identity_gate=" << (identity_gate ? "pass" : "fail")
              << " train_gate=" << (train_gate ? "pass" : "fail")
              << " validation_gate=" << (validation_gate ? "pass" : "fail")
              << " holdout_gate=" << (holdout_gate ? "pass" : "fail")
              << " bit_band_gate=" << (bit_band_gate ? "pass" : "fail")
              << " rows=" << matrix.rows.size() << " slots=" << matrix.multipliers.size()
              << " cells=" << matrix.rows.size() * matrix.multipliers.size()
              << " train_rows=" << training.size() << " validation_rows=" << validation.size()
              << " holdout_rows=" << holdout.size()
              << " corpus_digest_low=" << matrix.corpus_identity.low
              << " corpus_digest_high=" << matrix.corpus_identity.high
              << " schedule_digest_low=" << matrix.schedule_identity.low
              << " schedule_digest_high=" << matrix.schedule_identity.high
              << " matrix_digest_low=" << matrix_id.low << " matrix_digest_high=" << matrix_id.high
              << " split_digest_low=" << split_id.low << " split_digest_high=" << split_id.high
              << " current_order=" << order_string(matrix, current)
              << " train_optimal_order=" << order_string(matrix, optimized.order)
              << " train_optimal_cost="
              << gnfs::tests::squfof_strategy::cost_to_string(optimized.training_cost) << '\n';
}

} // namespace

int main() {
    try {
        run_oracle();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GNFS_SQUFOF_STRATEGY_ORACLE_SUMMARY_V2 status=fail error="
                  << sanitize_token(error.what()) << '\n';
        return 1;
    } catch (...) {
        std::cerr << "GNFS_SQUFOF_STRATEGY_ORACLE_SUMMARY_V2"
                     " status=fail error=unknown_exception\n";
        return 1;
    }
}
