#pragma once

#include "squfof_strategy_optimizer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace gnfs::tests::squfof_budget {

// Test-only model for exact SQUFOF per-slot budget experiments. The oracle
// test owns every production probe call. This file only validates, digests,
// replays, and optimizes already-collected observations.

using CellStatus = gnfs::tests::squfof_strategy::CellStatus;
using Digest = gnfs::tests::squfof_strategy::Digest;
using DigestBuilder = gnfs::tests::squfof_strategy::DigestBuilder;
using IterationCost = uint64_t;

inline constexpr uint32_t BASELINE_CAP_SENTINEL = std::numeric_limits<uint32_t>::max();
// The observation ladder includes the prospectively registered absolute
// 10056 cap. Exact training optimization intentionally remains on the four
// pre-registered search choices, while replay can evaluate all five levels.
inline constexpr std::array<uint32_t, 5> CAP_LADDER{{
    UINT32_C(1000),
    UINT32_C(2000),
    UINT32_C(5000),
    UINT32_C(10056),
    BASELINE_CAP_SENTINEL,
}};
inline constexpr size_t BASELINE_CAP_INDEX = CAP_LADDER.size() - 1;
inline constexpr std::array<uint32_t, 4> OPTIMIZATION_CAP_LADDER{{
    UINT32_C(1000),
    UINT32_C(2000),
    UINT32_C(5000),
    BASELINE_CAP_SENTINEL,
}};
static_assert(BASELINE_CAP_SENTINEL != 0);
static_assert(CAP_LADDER.back() == BASELINE_CAP_SENTINEL);
static_assert(OPTIMIZATION_CAP_LADDER.back() == BASELINE_CAP_SENTINEL);

[[nodiscard]] constexpr bool is_ladder_cap(uint32_t requested_cap) noexcept {
    for (const uint32_t cap : CAP_LADDER) {
        if (requested_cap == cap) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline size_t cap_ladder_index(uint32_t requested_cap) {
    for (size_t index = 0; index < CAP_LADDER.size(); ++index) {
        if (requested_cap == CAP_LADDER[index]) {
            return index;
        }
    }
    throw std::runtime_error("SQUFOF budget policy contains a cap outside the fixed ladder");
}

[[nodiscard]] constexpr uint32_t effective_cap(uint32_t baseline_cap,
                                               uint32_t requested_cap) noexcept {
    return requested_cap == BASELINE_CAP_SENTINEL ? baseline_cap
                                                  : std::min(baseline_cap, requested_cap);
}

[[nodiscard]] inline std::string cap_to_string(uint32_t requested_cap) {
    if (requested_cap == BASELINE_CAP_SENTINEL) {
        return "baseline";
    }
    if (!is_ladder_cap(requested_cap)) {
        throw std::runtime_error("cannot format a cap outside the fixed SQUFOF budget ladder");
    }
    return std::to_string(requested_cap);
}

[[nodiscard]] inline IterationCost checked_add(IterationCost left, IterationCost right) {
    if (right > std::numeric_limits<IterationCost>::max() - left) {
        throw std::overflow_error("SQUFOF budget iteration cost overflow");
    }
    return left + right;
}

[[nodiscard]] inline bool is_proper_factor(uint64_t n, uint64_t factor) noexcept {
    return gnfs::tests::squfof_strategy::is_proper_factor(n, factor);
}

struct BudgetCell final {
    uint32_t effective_cap = 0;
    CellStatus status = CellStatus::ineligible_input;
    uint64_t forward_iterations = 0;
    bool core_hit = false;
    uint64_t accepted_factor = 0;

    [[nodiscard]] bool operator==(const BudgetCell&) const noexcept = default;
};

struct SlotBudgetCells final {
    // This is an independently captured production-baseline observation. The
    // sentinel cell below must match it exactly, including effective_cap.
    BudgetCell baseline_cell;
    std::array<BudgetCell, CAP_LADDER.size()> cells{};

    [[nodiscard]] bool operator==(const SlotBudgetCells&) const noexcept = default;
};

struct BudgetRow final {
    uint64_t n = 0;
    uint32_t baseline_cap = 0;
    uint64_t reference_factor = 1;
    std::vector<SlotBudgetCells> slots;

    [[nodiscard]] bool operator==(const BudgetRow&) const noexcept = default;
};

struct BudgetMatrix final {
    Digest corpus_identity;
    Digest schedule_identity;
    std::vector<uint64_t> multipliers;
    std::vector<BudgetRow> rows;
};

[[nodiscard]] inline const BudgetCell& cell_for(const BudgetRow& row, size_t slot,
                                                uint32_t requested_cap) {
    if (slot >= row.slots.size()) {
        throw std::runtime_error("SQUFOF budget cell slot is out of range");
    }
    return row.slots[slot].cells[cap_ladder_index(requested_cap)];
}

namespace detail {

[[nodiscard]] inline bool valid_status(CellStatus status) noexcept {
    switch (status) {
    case CellStatus::ineligible_input:
    case CellStatus::invalid_slot:
    case CellStatus::overflow:
    case CellStatus::attempted:
        return true;
    }
    return false;
}

[[nodiscard]] inline bool same_observation(const BudgetCell& left,
                                           const BudgetCell& right) noexcept {
    return left.status == right.status && left.forward_iterations == right.forward_iterations &&
           left.core_hit == right.core_hit && left.accepted_factor == right.accepted_factor;
}

inline void validate_cell(const BudgetRow& row, size_t row_index, size_t slot, size_t cap_index,
                          const BudgetCell& cell) {
    const std::string location = " at row " + std::to_string(row_index) + " slot " +
                                 std::to_string(slot) + " cap " +
                                 cap_to_string(CAP_LADDER[cap_index]);
    const uint32_t expected_cap = effective_cap(row.baseline_cap, CAP_LADDER[cap_index]);
    if (cell.effective_cap != expected_cap) {
        throw std::runtime_error("SQUFOF budget cell has the wrong effective cap" + location);
    }
    if (!valid_status(cell.status)) {
        throw std::runtime_error("SQUFOF budget cell has an invalid status" + location);
    }
    if (cell.status != CellStatus::attempted) {
        if (cell.forward_iterations != 0 || cell.core_hit || cell.accepted_factor != 0) {
            throw std::runtime_error("non-attempted SQUFOF budget cell contains attempt results" +
                                     location);
        }
        return;
    }
    if (cell.forward_iterations > cell.effective_cap) {
        throw std::runtime_error("SQUFOF budget cell exceeds its effective cap" + location);
    }
    if (cell.accepted_factor != 0 && !is_proper_factor(row.n, cell.accepted_factor)) {
        throw std::runtime_error("SQUFOF budget cell has an invalid accepted factor" + location);
    }
    if (cell.accepted_factor != 0 && !cell.core_hit) {
        throw std::runtime_error("accepted SQUFOF budget factor lacks a core hit" + location);
    }
}

inline void append_cell(DigestBuilder& builder, const BudgetCell& cell) noexcept {
    builder.append_u32(cell.effective_cap);
    builder.append_byte(static_cast<uint8_t>(cell.status));
    builder.append_u64(cell.forward_iterations);
    builder.append_byte(cell.core_hit ? UINT8_C(1) : UINT8_C(0));
    builder.append_u64(cell.accepted_factor);
}

[[nodiscard]] inline bool contains_any(std::span<const uint64_t> left,
                                       std::span<const uint64_t> right) noexcept {
    for (size_t word = 0; word < left.size(); ++word) {
        if ((left[word] & right[word]) != 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool contains_outside(std::span<const uint64_t> values,
                                           std::span<const uint64_t> allowed) noexcept {
    for (size_t word = 0; word < values.size(); ++word) {
        if ((values[word] & ~allowed[word]) != 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool any_set(std::span<const uint64_t> values) noexcept {
    return std::any_of(values.begin(), values.end(), [](uint64_t value) { return value != 0; });
}

} // namespace detail

inline void validate_matrix(const BudgetMatrix& matrix) {
    if (matrix.multipliers.empty()) {
        throw std::runtime_error("SQUFOF budget matrix has no multiplier slots");
    }
    if (matrix.rows.empty()) {
        throw std::runtime_error("SQUFOF budget matrix has no rows");
    }

    for (size_t row_index = 0; row_index < matrix.rows.size(); ++row_index) {
        const BudgetRow& row = matrix.rows[row_index];
        if (row.n <= 1) {
            throw std::runtime_error("SQUFOF budget matrix contains n <= 1 at row " +
                                     std::to_string(row_index));
        }
        if (row.baseline_cap == 0) {
            throw std::runtime_error(
                "SQUFOF budget matrix requires an explicit baseline cap at row " +
                std::to_string(row_index));
        }
        if (row.reference_factor != 1 && !is_proper_factor(row.n, row.reference_factor)) {
            throw std::runtime_error(
                "SQUFOF budget matrix has an invalid reference factor at row " +
                std::to_string(row_index));
        }
        if (row.slots.size() != matrix.multipliers.size()) {
            throw std::runtime_error("SQUFOF budget matrix row has the wrong slot count at row " +
                                     std::to_string(row_index));
        }

        uint64_t baseline_replay_factor = 1;
        bool baseline_resolved = false;
        for (size_t slot = 0; slot < row.slots.size(); ++slot) {
            const SlotBudgetCells& slot_cells = row.slots[slot];
            if (slot_cells.baseline_cell.effective_cap != row.baseline_cap) {
                throw std::runtime_error(
                    "independent SQUFOF baseline cell has the wrong effective cap at row " +
                    std::to_string(row_index) + " slot " + std::to_string(slot));
            }
            detail::validate_cell(row, row_index, slot, BASELINE_CAP_INDEX,
                                  slot_cells.baseline_cell);
            for (size_t cap_index = 0; cap_index < CAP_LADDER.size(); ++cap_index) {
                detail::validate_cell(row, row_index, slot, cap_index, slot_cells.cells[cap_index]);
                if (slot_cells.cells[cap_index].status != slot_cells.baseline_cell.status) {
                    throw std::runtime_error(
                        "SQUFOF probe status changed with the iteration cap at row " +
                        std::to_string(row_index) + " slot " + std::to_string(slot));
                }
            }
            if (slot_cells.cells[BASELINE_CAP_INDEX] != slot_cells.baseline_cell) {
                throw std::runtime_error(
                    "SQUFOF baseline sentinel cell differs from the independent baseline at row " +
                    std::to_string(row_index) + " slot " + std::to_string(slot));
            }
            if (slot_cells.baseline_cell.status == CellStatus::ineligible_input ||
                slot_cells.baseline_cell.status == CellStatus::invalid_slot) {
                throw std::runtime_error(
                    "SQUFOF budget matrix contains an incomplete baseline cell at row " +
                    std::to_string(row_index) + " slot " + std::to_string(slot));
            }

            for (size_t lower_index = 0; lower_index < CAP_LADDER.size(); ++lower_index) {
                const BudgetCell& lower = slot_cells.cells[lower_index];
                for (size_t upper_index = lower_index + 1; upper_index < CAP_LADDER.size();
                     ++upper_index) {
                    const BudgetCell& upper = slot_cells.cells[upper_index];
                    if (lower.effective_cap == upper.effective_cap) {
                        if (lower != upper) {
                            throw std::runtime_error("repeated SQUFOF effective caps produced "
                                                     "different observations at row " +
                                                     std::to_string(row_index) + " slot " +
                                                     std::to_string(slot));
                        }
                        continue;
                    }
                    if (lower.effective_cap > upper.effective_cap) {
                        throw std::runtime_error(
                            "SQUFOF budget ladder is not monotone after cap clamping");
                    }
                    if (lower.status != CellStatus::attempted) {
                        // Overflow and preprocessing status are independent of
                        // the cap. Non-attempted cells carry no dynamic state.
                        continue;
                    }
                    if (upper.forward_iterations < lower.forward_iterations) {
                        throw std::runtime_error("larger SQUFOF cap does not preserve the observed "
                                                 "forward prefix at row " +
                                                 std::to_string(row_index) + " slot " +
                                                 std::to_string(slot));
                    }
                    const bool lower_is_terminal =
                        lower.core_hit || lower.forward_iterations < lower.effective_cap;
                    const bool upper_terminated_within_lower_prefix =
                        upper.forward_iterations <= lower.effective_cap;
                    if ((lower_is_terminal || upper_terminated_within_lower_prefix) &&
                        !detail::same_observation(lower, upper)) {
                        throw std::runtime_error(
                            "larger SQUFOF cap changed a terminal probe result at row " +
                            std::to_string(row_index) + " slot " + std::to_string(slot));
                    }
                }
            }

            if (!baseline_resolved) {
                const BudgetCell& baseline = slot_cells.baseline_cell;
                if (baseline.accepted_factor != 0) {
                    baseline_replay_factor = baseline.accepted_factor;
                    baseline_resolved = true;
                }
            }
        }
        if (baseline_replay_factor != row.reference_factor) {
            throw std::runtime_error(
                "SQUFOF budget baseline cells do not replay the reference factor at row " +
                std::to_string(row_index));
        }
    }
}

// Stable V3 encoding. The digest covers the frozen corpus and schedule IDs,
// observation and optimization ladders, independent baseline observations,
// every case-slot-cap cell, and the exact production reference factor.
[[nodiscard]] inline Digest matrix_digest(const BudgetMatrix& matrix) {
    validate_matrix(matrix);
    DigestBuilder builder("GNFS-SQUFOF-BUDGET-MATRIX-V3");
    builder.append_u32(3);
    builder.append_u64(matrix.corpus_identity.low);
    builder.append_u64(matrix.corpus_identity.high);
    builder.append_u64(matrix.schedule_identity.low);
    builder.append_u64(matrix.schedule_identity.high);
    builder.append_u64(static_cast<uint64_t>(matrix.rows.size()));
    builder.append_u64(static_cast<uint64_t>(matrix.multipliers.size()));
    for (size_t slot = 0; slot < matrix.multipliers.size(); ++slot) {
        builder.append_u64(static_cast<uint64_t>(slot));
        builder.append_u64(matrix.multipliers[slot]);
    }
    builder.append_u64(static_cast<uint64_t>(CAP_LADDER.size()));
    for (const uint32_t cap : CAP_LADDER) {
        builder.append_u32(cap);
    }
    builder.append_u64(static_cast<uint64_t>(OPTIMIZATION_CAP_LADDER.size()));
    for (const uint32_t cap : OPTIMIZATION_CAP_LADDER) {
        builder.append_u32(cap);
    }

    for (size_t row_index = 0; row_index < matrix.rows.size(); ++row_index) {
        const BudgetRow& row = matrix.rows[row_index];
        builder.append_u64(static_cast<uint64_t>(row_index));
        builder.append_u64(row.n);
        builder.append_u32(row.baseline_cap);
        builder.append_u64(row.reference_factor);
        for (size_t slot = 0; slot < row.slots.size(); ++slot) {
            const SlotBudgetCells& slot_cells = row.slots[slot];
            builder.append_u64(static_cast<uint64_t>(slot));
            builder.append_u64(matrix.multipliers[slot]);
            detail::append_cell(builder, slot_cells.baseline_cell);
            for (size_t cap_index = 0; cap_index < CAP_LADDER.size(); ++cap_index) {
                builder.append_u64(static_cast<uint64_t>(cap_index));
                builder.append_u32(CAP_LADDER[cap_index]);
                detail::append_cell(builder, slot_cells.cells[cap_index]);
            }
        }
    }
    return builder.finish();
}

using CapPolicy = std::vector<uint32_t>;

[[nodiscard]] inline CapPolicy uniform_policy(const BudgetMatrix& matrix, uint32_t requested_cap) {
    if (!is_ladder_cap(requested_cap)) {
        throw std::runtime_error(
            "uniform SQUFOF budget policy contains a cap outside the fixed ladder");
    }
    return CapPolicy(matrix.multipliers.size(), requested_cap);
}

[[nodiscard]] inline CapPolicy baseline_policy(const BudgetMatrix& matrix) {
    validate_matrix(matrix);
    return uniform_policy(matrix, BASELINE_CAP_SENTINEL);
}

inline void validate_policy(const BudgetMatrix& matrix, std::span<const uint32_t> slot_caps) {
    if (slot_caps.size() != matrix.multipliers.size()) {
        throw std::runtime_error("SQUFOF budget policy has the wrong slot count");
    }
    for (const uint32_t cap : slot_caps) {
        if (!is_ladder_cap(cap)) {
            throw std::runtime_error(
                "SQUFOF budget policy contains a cap outside the fixed ladder");
        }
    }
}

[[nodiscard]] inline std::vector<size_t> all_rows(const BudgetMatrix& matrix) {
    std::vector<size_t> rows(matrix.rows.size());
    for (size_t row = 0; row < rows.size(); ++row) {
        rows[row] = row;
    }
    return rows;
}

struct ReplayResult final {
    uint64_t factor = 1;
    IterationCost forward_iterations = 0;
    size_t slots_visited = 0;
};

namespace detail {

[[nodiscard]] inline ReplayResult replay_row_unchecked(const BudgetMatrix& matrix, size_t row_index,
                                                       std::span<const uint32_t> slot_caps) {
    const BudgetRow& row = matrix.rows[row_index];
    ReplayResult result;
    for (size_t slot = 0; slot < matrix.multipliers.size(); ++slot) {
        const BudgetCell& cell = cell_for(row, slot, slot_caps[slot]);
        ++result.slots_visited;
        if (cell.status == CellStatus::ineligible_input ||
            cell.status == CellStatus::invalid_slot) {
            throw std::runtime_error("SQUFOF budget replay encountered an incomplete cell");
        }
        result.forward_iterations = checked_add(result.forward_iterations, cell.forward_iterations);
        if (cell.accepted_factor != 0) {
            result.factor = cell.accepted_factor;
            break;
        }
    }
    return result;
}

} // namespace detail

[[nodiscard]] inline ReplayResult replay_row(const BudgetMatrix& matrix, size_t row_index,
                                             std::span<const uint32_t> slot_caps) {
    validate_matrix(matrix);
    validate_policy(matrix, slot_caps);
    if (row_index >= matrix.rows.size()) {
        throw std::runtime_error("SQUFOF budget replay row is out of range");
    }
    return detail::replay_row_unchecked(matrix, row_index, slot_caps);
}

struct BudgetEvaluation final {
    size_t row_count = 0;
    size_t reference_successes = 0;
    size_t reference_failures = 0;
    size_t candidate_successes = 0;
    size_t candidate_failures = 0;
    size_t new_failures = 0;
    size_t new_successes = 0;
    size_t changed_factors = 0;
    size_t factor_identity_mismatches = 0;
    // Costs are bucketed by the production reference result. A candidate that
    // creates a new failure therefore remains in success_forward_iterations.
    IterationCost success_forward_iterations = 0;
    IterationCost failure_forward_iterations = 0;
    IterationCost total_forward_iterations = 0;
};

[[nodiscard]] inline BudgetEvaluation evaluate_policy(const BudgetMatrix& matrix,
                                                      std::span<const size_t> row_indices,
                                                      std::span<const uint32_t> slot_caps) {
    validate_matrix(matrix);
    validate_policy(matrix, slot_caps);
    BudgetEvaluation evaluation;
    evaluation.row_count = row_indices.size();
    std::vector<bool> seen(matrix.rows.size(), false);
    for (const size_t row_index : row_indices) {
        if (row_index >= matrix.rows.size() || seen[row_index]) {
            throw std::runtime_error("SQUFOF budget evaluation has an invalid row selection");
        }
        seen[row_index] = true;
        const BudgetRow& row = matrix.rows[row_index];
        const ReplayResult replayed = detail::replay_row_unchecked(matrix, row_index, slot_caps);

        if (row.reference_factor == 1) {
            ++evaluation.reference_failures;
            evaluation.failure_forward_iterations =
                checked_add(evaluation.failure_forward_iterations, replayed.forward_iterations);
        } else {
            ++evaluation.reference_successes;
            evaluation.success_forward_iterations =
                checked_add(evaluation.success_forward_iterations, replayed.forward_iterations);
        }
        if (replayed.factor == 1) {
            ++evaluation.candidate_failures;
        } else {
            ++evaluation.candidate_successes;
        }

        if (replayed.factor != row.reference_factor) {
            ++evaluation.factor_identity_mismatches;
            if (row.reference_factor == 1) {
                ++evaluation.new_successes;
            } else if (replayed.factor == 1) {
                ++evaluation.new_failures;
            } else {
                ++evaluation.changed_factors;
            }
        }
    }
    evaluation.total_forward_iterations =
        checked_add(evaluation.success_forward_iterations, evaluation.failure_forward_iterations);
    return evaluation;
}

[[nodiscard]] inline BudgetEvaluation evaluate_policy(const BudgetMatrix& matrix,
                                                      std::span<const uint32_t> slot_caps) {
    const std::vector<size_t> rows = all_rows(matrix);
    return evaluate_policy(matrix, rows, slot_caps);
}

struct OptimizationResult final {
    bool feasible = false;
    IterationCost training_cost = 0;
    CapPolicy slot_caps;
};

// Exact fixed-order search over the four optimization cap choices for every multiplier
// slot. The search incrementally charges only unresolved reference-success
// rows, while reference-failure rows remain active through every slot. Bit
// masks reject incomplete and wrong-factor transitions. Byte lookup tables
// aggregate active-row costs without replaying all training rows for each of
// the 4^slot_count complete policies.
[[nodiscard]] inline OptimizationResult
optimize_training_rows(const BudgetMatrix& matrix, std::span<const size_t> training_rows) {
    validate_matrix(matrix);
    if (matrix.multipliers.size() > 11) {
        throw std::runtime_error("exact SQUFOF budget search is limited to 11 multiplier slots");
    }
    if (training_rows.empty()) {
        throw std::runtime_error("exact SQUFOF budget search requires training rows");
    }

    std::vector<bool> selected(matrix.rows.size(), false);
    std::vector<size_t> success_rows;
    std::vector<size_t> failure_rows;
    for (const size_t row_index : training_rows) {
        if (row_index >= matrix.rows.size() || selected[row_index]) {
            throw std::runtime_error(
                "SQUFOF budget optimizer has an invalid training row selection");
        }
        selected[row_index] = true;
        if (matrix.rows[row_index].reference_factor == 1) {
            failure_rows.push_back(row_index);
        } else {
            success_rows.push_back(row_index);
        }
    }

    const size_t success_word_count = (success_rows.size() + 63) / 64;
    const size_t success_byte_count = (success_rows.size() + 7) / 8;

    struct ChoiceTransition final {
        bool failure_compatible = true;
        IterationCost failure_cost = 0;
        std::vector<uint64_t> incomplete_mask;
        std::vector<uint64_t> wrong_factor_mask;
        std::vector<uint64_t> resolved_mask;
        std::vector<std::array<IterationCost, 256>> success_cost_by_byte;
    };

    std::vector<std::array<ChoiceTransition, OPTIMIZATION_CAP_LADDER.size()>> transitions(
        matrix.multipliers.size());
    for (size_t slot = 0; slot < matrix.multipliers.size(); ++slot) {
        for (size_t search_cap_index = 0; search_cap_index < OPTIMIZATION_CAP_LADDER.size();
             ++search_cap_index) {
            ChoiceTransition& transition = transitions[slot][search_cap_index];
            const size_t observation_cap_index =
                cap_ladder_index(OPTIMIZATION_CAP_LADDER[search_cap_index]);
            transition.incomplete_mask.assign(success_word_count, 0);
            transition.wrong_factor_mask.assign(success_word_count, 0);
            transition.resolved_mask.assign(success_word_count, 0);
            transition.success_cost_by_byte.resize(success_byte_count);

            for (const size_t row_index : failure_rows) {
                const BudgetCell& cell =
                    matrix.rows[row_index].slots[slot].cells[observation_cap_index];
                if (cell.status == CellStatus::ineligible_input ||
                    cell.status == CellStatus::invalid_slot || cell.accepted_factor != 0) {
                    transition.failure_compatible = false;
                }
                transition.failure_cost =
                    checked_add(transition.failure_cost, cell.forward_iterations);
            }

            std::vector<IterationCost> success_costs(success_rows.size(), 0);
            for (size_t success_index = 0; success_index < success_rows.size(); ++success_index) {
                const BudgetRow& row = matrix.rows[success_rows[success_index]];
                const BudgetCell& cell = row.slots[slot].cells[observation_cap_index];
                const size_t word = success_index / 64;
                const uint64_t bit = UINT64_C(1) << (success_index % 64);
                success_costs[success_index] = cell.forward_iterations;
                if (cell.status == CellStatus::ineligible_input ||
                    cell.status == CellStatus::invalid_slot) {
                    transition.incomplete_mask[word] |= bit;
                } else if (cell.accepted_factor == row.reference_factor) {
                    transition.resolved_mask[word] |= bit;
                } else if (cell.accepted_factor != 0) {
                    transition.wrong_factor_mask[word] |= bit;
                }
            }

            for (size_t byte_index = 0; byte_index < success_byte_count; ++byte_index) {
                auto& table = transition.success_cost_by_byte[byte_index];
                table.fill(0);
                for (size_t subset = 1; subset < table.size(); ++subset) {
                    const uint32_t subset_bits = static_cast<uint32_t>(subset);
                    const unsigned selected_bit =
                        static_cast<unsigned>(std::countr_zero(subset_bits));
                    const size_t previous = subset & (subset - 1);
                    const size_t success_index = byte_index * 8 + selected_bit;
                    const IterationCost added =
                        success_index < success_costs.size() ? success_costs[success_index] : 0;
                    table[subset] = checked_add(table[previous], added);
                }
            }
        }
    }

    std::vector<std::vector<uint64_t>> suffix_resolvable(
        matrix.multipliers.size() + 1, std::vector<uint64_t>(success_word_count, 0));
    for (size_t slot = matrix.multipliers.size(); slot-- > 0;) {
        suffix_resolvable[slot] = suffix_resolvable[slot + 1];
        for (const ChoiceTransition& transition : transitions[slot]) {
            for (size_t word = 0; word < success_word_count; ++word) {
                suffix_resolvable[slot][word] |= transition.resolved_mask[word];
            }
        }
    }

    std::vector<uint64_t> initial_active(success_word_count, 0);
    for (size_t success_index = 0; success_index < success_rows.size(); ++success_index) {
        initial_active[success_index / 64] |= UINT64_C(1) << (success_index % 64);
    }

    CapPolicy current_policy(matrix.multipliers.size(), OPTIMIZATION_CAP_LADDER.front());
    const CapPolicy baseline_caps(matrix.multipliers.size(), BASELINE_CAP_SENTINEL);
    const BudgetEvaluation baseline_evaluation =
        evaluate_policy(matrix, training_rows, baseline_caps);
    if (baseline_evaluation.factor_identity_mismatches != 0) {
        throw std::runtime_error(
            "SQUFOF budget baseline is not identity-preserving on the training rows");
    }
    OptimizationResult best{
        true,
        baseline_evaluation.total_forward_iterations,
        baseline_caps,
    };

    const auto active_cost = [](const ChoiceTransition& transition,
                                std::span<const uint64_t> active) {
        IterationCost cost = 0;
        for (size_t byte_index = 0; byte_index < transition.success_cost_by_byte.size();
             ++byte_index) {
            const size_t word = byte_index / 8;
            const unsigned shift = static_cast<unsigned>((byte_index % 8) * 8);
            const auto subset = static_cast<uint8_t>((active[word] >> shift) & UINT64_C(0xff));
            cost = checked_add(cost, transition.success_cost_by_byte[byte_index][subset]);
        }
        return cost;
    };

    const auto search = [&](auto&& self, size_t slot, const std::vector<uint64_t>& active,
                            IterationCost accumulated_cost) -> void {
        if (detail::contains_outside(active, suffix_resolvable[slot])) {
            return;
        }
        if (slot == matrix.multipliers.size()) {
            if (detail::any_set(active)) {
                return;
            }
            if (!best.feasible || accumulated_cost < best.training_cost ||
                (accumulated_cost == best.training_cost && current_policy < best.slot_caps)) {
                best.feasible = true;
                best.training_cost = accumulated_cost;
                best.slot_caps = current_policy;
            }
            return;
        }

        for (size_t search_cap_index = 0; search_cap_index < OPTIMIZATION_CAP_LADDER.size();
             ++search_cap_index) {
            const ChoiceTransition& transition = transitions[slot][search_cap_index];
            if (!transition.failure_compatible ||
                detail::contains_any(active, transition.incomplete_mask) ||
                detail::contains_any(active, transition.wrong_factor_mask)) {
                continue;
            }

            IterationCost candidate_cost = checked_add(accumulated_cost, transition.failure_cost);
            candidate_cost = checked_add(candidate_cost, active_cost(transition, active));
            if (best.feasible && candidate_cost > best.training_cost) {
                continue;
            }

            std::vector<uint64_t> next_active = active;
            for (size_t word = 0; word < next_active.size(); ++word) {
                next_active[word] &= ~transition.resolved_mask[word];
            }
            current_policy[slot] = OPTIMIZATION_CAP_LADDER[search_cap_index];
            self(self, slot + 1, next_active, candidate_cost);
        }
    };
    search(search, 0, initial_active, 0);
    return best;
}

} // namespace gnfs::tests::squfof_budget
