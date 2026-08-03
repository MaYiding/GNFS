#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gnfs::tests::squfof_strategy {

// Test-only model for evaluating complete SQUFOF case-by-multiplier matrices.
// It deliberately does not call SQUFOF itself: the oracle test owns the bridge
// from the production probe API, while this file keeps replay and optimization
// deterministic and independently testable.

struct Digest final {
    uint64_t low = 0;
    uint64_t high = 0;

    [[nodiscard]] bool operator==(const Digest&) const noexcept = default;
};

class DigestBuilder final {
public:
    DigestBuilder() = default;

    explicit DigestBuilder(std::string_view domain) {
        append_u64(static_cast<uint64_t>(domain.size()));
        for (const char character : domain) {
            append_byte(static_cast<uint8_t>(character));
        }
    }

    void append_byte(uint8_t value) noexcept {
        low_ ^= static_cast<uint64_t>(value);
        low_ *= UINT64_C(1099511628211);

        high_ ^= static_cast<uint64_t>(value) + byte_index_ * UINT64_C(0x9e3779b97f4a7c15);
        high_ = std::rotl(high_, 27);
        high_ *= UINT64_C(0x94d049bb133111eb);
        high_ += UINT64_C(0x2545f4914f6cdd1d);
        ++byte_index_;
    }

    void append_u32(uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    void append_u64(uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    [[nodiscard]] Digest finish() const noexcept {
        return {avalanche(low_ ^ byte_index_), avalanche(high_ ^ std::rotl(byte_index_, 17))};
    }

private:
    [[nodiscard]] static uint64_t avalanche(uint64_t value) noexcept {
        value ^= value >> 30;
        value *= UINT64_C(0xbf58476d1ce4e5b9);
        value ^= value >> 27;
        value *= UINT64_C(0x94d049bb133111eb);
        value ^= value >> 31;
        return value;
    }

    uint64_t low_ = UINT64_C(14695981039346656037);
    uint64_t high_ = UINT64_C(0x243f6a8885a308d3);
    uint64_t byte_index_ = 0;
};

enum class CellStatus : uint8_t {
    ineligible_input = 0,
    invalid_slot = 1,
    overflow = 2,
    attempted = 3,
};

struct MatrixCell final {
    CellStatus status = CellStatus::ineligible_input;
    uint64_t forward_iterations = 0;
    bool core_hit = false;
    uint64_t accepted_factor = 0;

    [[nodiscard]] bool operator==(const MatrixCell&) const noexcept = default;
};

struct MatrixRow final {
    uint64_t n = 0;
    uint32_t max_iterations = 0;
    uint64_t reference_factor = 1;
    std::vector<MatrixCell> cells;

    [[nodiscard]] bool operator==(const MatrixRow&) const noexcept = default;
};

struct StrategyMatrix final {
    Digest corpus_identity;
    Digest schedule_identity;
    std::vector<uint64_t> multipliers;
    std::vector<MatrixRow> rows;
};

using IterationCost = uint64_t;

[[nodiscard]] inline std::string cost_to_string(IterationCost value) {
    return std::to_string(value);
}

[[nodiscard]] inline IterationCost checked_add(IterationCost left, IterationCost right) {
    if (right > std::numeric_limits<IterationCost>::max() - left) {
        throw std::overflow_error("SQUFOF strategy iteration cost overflow");
    }
    return left + right;
}

[[nodiscard]] inline IterationCost checked_multiply(IterationCost value, uint64_t multiplier) {
    if (multiplier != 0 && value > std::numeric_limits<IterationCost>::max() / multiplier) {
        throw std::overflow_error("SQUFOF strategy iteration cost multiplication overflow");
    }
    return value * multiplier;
}

[[nodiscard]] inline bool is_proper_factor(uint64_t n, uint64_t factor) noexcept {
    return factor > 1 && factor < n && n % factor == 0;
}

inline void validate_matrix(const StrategyMatrix& matrix) {
    if (matrix.multipliers.empty()) {
        throw std::runtime_error("SQUFOF strategy matrix has no multiplier slots");
    }
    if (matrix.multipliers.size() >= 8 * sizeof(size_t)) {
        throw std::runtime_error("SQUFOF strategy matrix has too many slots for subset DP");
    }
    for (size_t row_index = 0; row_index < matrix.rows.size(); ++row_index) {
        const MatrixRow& row = matrix.rows[row_index];
        if (row.n <= 1) {
            throw std::runtime_error("SQUFOF strategy matrix contains n <= 1 at row " +
                                     std::to_string(row_index));
        }
        if (row.cells.size() != matrix.multipliers.size()) {
            throw std::runtime_error("SQUFOF strategy matrix row has the wrong slot count at row " +
                                     std::to_string(row_index));
        }
        if (row.reference_factor != 1 && !is_proper_factor(row.n, row.reference_factor)) {
            throw std::runtime_error(
                "SQUFOF strategy matrix has an invalid reference factor at row " +
                std::to_string(row_index));
        }
        bool reference_reachable = row.reference_factor == 1;
        bool any_factor_accepted = false;
        for (size_t slot = 0; slot < row.cells.size(); ++slot) {
            const MatrixCell& cell = row.cells[slot];
            switch (cell.status) {
            case CellStatus::ineligible_input:
            case CellStatus::invalid_slot:
            case CellStatus::overflow:
            case CellStatus::attempted:
                break;
            default:
                throw std::runtime_error(
                    "SQUFOF strategy matrix has an invalid cell status at row " +
                    std::to_string(row_index) + " slot " + std::to_string(slot));
            }
            if (cell.accepted_factor != 0 && !is_proper_factor(row.n, cell.accepted_factor)) {
                throw std::runtime_error(
                    "SQUFOF strategy matrix has an invalid accepted factor at row " +
                    std::to_string(row_index) + " slot " + std::to_string(slot));
            }
            if (cell.accepted_factor > 1 && !cell.core_hit) {
                throw std::runtime_error("accepted SQUFOF factor lacks a core hit at row " +
                                         std::to_string(row_index) + " slot " +
                                         std::to_string(slot));
            }
            if (cell.status == CellStatus::attempted && row.max_iterations != 0 &&
                cell.forward_iterations > row.max_iterations) {
                throw std::runtime_error("SQUFOF strategy cell exceeds its iteration cap at row " +
                                         std::to_string(row_index) + " slot " +
                                         std::to_string(slot));
            }
            if (cell.accepted_factor > 1) {
                any_factor_accepted = true;
                reference_reachable =
                    reference_reachable || cell.accepted_factor == row.reference_factor;
            }
            if (cell.status != CellStatus::attempted &&
                (cell.forward_iterations != 0 || cell.core_hit || cell.accepted_factor != 0)) {
                throw std::runtime_error(
                    "non-attempted SQUFOF cell contains attempt results at row " +
                    std::to_string(row_index) + " slot " + std::to_string(slot));
            }
        }
        if (!reference_reachable || (row.reference_factor == 1 && any_factor_accepted)) {
            throw std::runtime_error(
                "SQUFOF strategy matrix cannot replay its reference factor at row " +
                std::to_string(row_index));
        }
    }
}

[[nodiscard]] inline Digest schedule_digest(std::span<const uint64_t> multipliers) {
    DigestBuilder builder("GNFS-SQUFOF-MULTIPLIER-SCHEDULE-V1");
    builder.append_u64(static_cast<uint64_t>(multipliers.size()));
    for (size_t slot = 0; slot < multipliers.size(); ++slot) {
        builder.append_u64(static_cast<uint64_t>(slot));
        builder.append_u64(multipliers[slot]);
    }
    return builder.finish();
}

// Stable V2 encoding. All integers are little-endian. The prefix consists of
// the domain string (length then bytes), schema version, frozen corpus digest,
// frozen schedule digest, row count, and slot count. Every counterfactual cell
// then encodes row, slot, n, iteration budget, multiplier, status, iterations,
// core-hit bit, accepted factor, and the production reference factor.
[[nodiscard]] inline Digest matrix_digest(const StrategyMatrix& matrix) {
    validate_matrix(matrix);
    DigestBuilder builder("GNFS-SQUFOF-STRATEGY-MATRIX-V2");
    builder.append_u32(2);
    builder.append_u64(matrix.corpus_identity.low);
    builder.append_u64(matrix.corpus_identity.high);
    builder.append_u64(matrix.schedule_identity.low);
    builder.append_u64(matrix.schedule_identity.high);
    builder.append_u64(static_cast<uint64_t>(matrix.rows.size()));
    builder.append_u64(static_cast<uint64_t>(matrix.multipliers.size()));

    for (size_t row_index = 0; row_index < matrix.rows.size(); ++row_index) {
        const MatrixRow& row = matrix.rows[row_index];
        for (size_t slot = 0; slot < row.cells.size(); ++slot) {
            const MatrixCell& cell = row.cells[slot];
            builder.append_u64(static_cast<uint64_t>(row_index));
            builder.append_u64(static_cast<uint64_t>(slot));
            builder.append_u64(row.n);
            builder.append_u32(row.max_iterations);
            builder.append_u64(matrix.multipliers[slot]);
            builder.append_byte(static_cast<uint8_t>(cell.status));
            builder.append_u64(cell.forward_iterations);
            builder.append_byte(cell.core_hit ? UINT8_C(1) : UINT8_C(0));
            builder.append_u64(cell.accepted_factor);
            builder.append_u64(row.reference_factor);
        }
    }
    return builder.finish();
}

enum class DataSplit : uint8_t {
    train,
    validation,
    holdout,
};

[[nodiscard]] constexpr uint64_t stable_n_hash(uint64_t n) noexcept {
    // SplitMix64's finalizer provides a fixed, platform-independent mapping.
    n ^= n >> 30;
    n *= UINT64_C(0xbf58476d1ce4e5b9);
    n ^= n >> 27;
    n *= UINT64_C(0x94d049bb133111eb);
    n ^= n >> 31;
    return n;
}

enum class BitBand : uint8_t {
    through_40,
    bits_41_to_43,
    bits_44_to_46,
    above_46,
};

[[nodiscard]] constexpr BitBand bit_band_for_n(uint64_t n) noexcept {
    const int bits = std::bit_width(n);
    if (bits <= 40) {
        return BitBand::through_40;
    }
    if (bits <= 43) {
        return BitBand::bits_41_to_43;
    }
    if (bits <= 46) {
        return BitBand::bits_44_to_46;
    }
    return BitBand::above_46;
}

[[nodiscard]] inline std::string_view split_name(DataSplit split) noexcept {
    switch (split) {
    case DataSplit::train:
        return "train";
    case DataSplit::validation:
        return "validation";
    case DataSplit::holdout:
        return "holdout";
    }
    return "unknown";
}

[[nodiscard]] inline std::string_view bit_band_name(BitBand band) noexcept {
    switch (band) {
    case BitBand::through_40:
        return "le40";
    case BitBand::bits_41_to_43:
        return "41_43";
    case BitBand::bits_44_to_46:
        return "44_46";
    case BitBand::above_46:
        return "gt46";
    }
    return "unknown";
}

using SplitAssignment = std::vector<DataSplit>;

inline void validate_split_assignment(const StrategyMatrix& matrix,
                                      std::span<const DataSplit> assignment) {
    if (assignment.size() != matrix.rows.size()) {
        throw std::runtime_error("SQUFOF strategy split assignment has the wrong row count");
    }
    for (size_t row = 0; row < matrix.rows.size(); ++row) {
        switch (assignment[row]) {
        case DataSplit::train:
        case DataSplit::validation:
        case DataSplit::holdout:
            break;
        default:
            throw std::runtime_error("SQUFOF strategy split assignment has an invalid value");
        }
        for (size_t previous = 0; previous < row; ++previous) {
            if (matrix.rows[previous].n == matrix.rows[row].n &&
                assignment[previous] != assignment[row]) {
                throw std::runtime_error("identical SQUFOF inputs cross strategy data splits");
            }
        }
    }
}

[[nodiscard]] inline SplitAssignment stratified_split_assignment(const StrategyMatrix& matrix) {
    validate_matrix(matrix);
    SplitAssignment assignment(matrix.rows.size(), DataSplit::train);
    constexpr std::array BANDS{
        BitBand::through_40,
        BitBand::bits_41_to_43,
        BitBand::bits_44_to_46,
        BitBand::above_46,
    };
    constexpr std::array CYCLE{
        DataSplit::train,
        DataSplit::train,
        DataSplit::validation,
        DataSplit::holdout,
    };

    for (const BitBand band : BANDS) {
        std::vector<uint64_t> unique_inputs;
        for (const MatrixRow& row : matrix.rows) {
            if (bit_band_for_n(row.n) == band &&
                std::find(unique_inputs.begin(), unique_inputs.end(), row.n) ==
                    unique_inputs.end()) {
                unique_inputs.push_back(row.n);
            }
        }
        std::sort(unique_inputs.begin(), unique_inputs.end(), [](uint64_t left, uint64_t right) {
            const uint64_t left_hash = stable_n_hash(left);
            const uint64_t right_hash = stable_n_hash(right);
            return left_hash != right_hash ? left_hash < right_hash : left < right;
        });

        for (size_t group = 0; group < unique_inputs.size(); ++group) {
            for (size_t row = 0; row < matrix.rows.size(); ++row) {
                if (matrix.rows[row].n == unique_inputs[group]) {
                    assignment[row] = CYCLE[group % CYCLE.size()];
                }
            }
        }
    }
    validate_split_assignment(matrix, assignment);
    return assignment;
}

[[nodiscard]] inline Digest split_assignment_digest(const StrategyMatrix& matrix,
                                                    std::span<const DataSplit> assignment) {
    validate_split_assignment(matrix, assignment);
    const Digest matrix_id = matrix_digest(matrix);
    DigestBuilder builder("GNFS-SQUFOF-STRATEGY-SPLIT-V2");
    builder.append_u32(2);
    builder.append_u64(matrix_id.low);
    builder.append_u64(matrix_id.high);
    builder.append_u64(static_cast<uint64_t>(matrix.rows.size()));
    for (size_t row = 0; row < matrix.rows.size(); ++row) {
        builder.append_u64(static_cast<uint64_t>(row));
        builder.append_u64(matrix.rows[row].n);
        builder.append_byte(static_cast<uint8_t>(bit_band_for_n(matrix.rows[row].n)));
        builder.append_byte(static_cast<uint8_t>(assignment[row]));
    }
    return builder.finish();
}

[[nodiscard]] inline std::vector<size_t> current_order(const StrategyMatrix& matrix) {
    std::vector<size_t> order(matrix.multipliers.size());
    for (size_t slot = 0; slot < order.size(); ++slot) {
        order[slot] = slot;
    }
    return order;
}

[[nodiscard]] inline std::vector<size_t> all_rows(const StrategyMatrix& matrix) {
    std::vector<size_t> rows(matrix.rows.size());
    for (size_t row = 0; row < rows.size(); ++row) {
        rows[row] = row;
    }
    return rows;
}

[[nodiscard]] inline std::vector<size_t> rows_for_split(const StrategyMatrix& matrix,
                                                        std::span<const DataSplit> assignment,
                                                        DataSplit split) {
    validate_split_assignment(matrix, assignment);
    std::vector<size_t> rows;
    for (size_t row = 0; row < matrix.rows.size(); ++row) {
        if (assignment[row] == split) {
            rows.push_back(row);
        }
    }
    return rows;
}

[[nodiscard]] inline std::vector<size_t> rows_for_band(const StrategyMatrix& matrix, BitBand band) {
    std::vector<size_t> rows;
    for (size_t row = 0; row < matrix.rows.size(); ++row) {
        if (bit_band_for_n(matrix.rows[row].n) == band) {
            rows.push_back(row);
        }
    }
    return rows;
}

[[nodiscard]] inline std::vector<size_t>
rows_for_split_and_band(const StrategyMatrix& matrix, std::span<const DataSplit> assignment,
                        DataSplit split, BitBand band) {
    validate_split_assignment(matrix, assignment);
    std::vector<size_t> rows;
    for (size_t row = 0; row < matrix.rows.size(); ++row) {
        if (assignment[row] == split && bit_band_for_n(matrix.rows[row].n) == band) {
            rows.push_back(row);
        }
    }
    return rows;
}

inline void validate_order(const StrategyMatrix& matrix, std::span<const size_t> order) {
    if (order.size() != matrix.multipliers.size()) {
        throw std::runtime_error("SQUFOF strategy order has the wrong slot count");
    }
    std::vector<bool> seen(matrix.multipliers.size(), false);
    for (const size_t slot : order) {
        if (slot >= matrix.multipliers.size() || seen[slot]) {
            throw std::runtime_error("SQUFOF strategy order is not a slot permutation");
        }
        seen[slot] = true;
    }
}

struct ReplayResult final {
    uint64_t factor = 1;
    IterationCost forward_iterations = 0;
};

[[nodiscard]] inline ReplayResult replay_row(const StrategyMatrix& matrix, size_t row_index,
                                             std::span<const size_t> order) {
    if (row_index >= matrix.rows.size()) {
        throw std::runtime_error("SQUFOF strategy replay row is out of range");
    }
    validate_order(matrix, order);
    const MatrixRow& row = matrix.rows[row_index];
    ReplayResult result;
    for (const size_t slot : order) {
        const MatrixCell& cell = row.cells[slot];
        if (cell.status == CellStatus::ineligible_input ||
            cell.status == CellStatus::invalid_slot) {
            throw std::runtime_error("SQUFOF strategy replay encountered an incomplete cell");
        }
        result.forward_iterations = checked_add(result.forward_iterations, cell.forward_iterations);
        if (cell.accepted_factor > 1) {
            result.factor = cell.accepted_factor;
            break;
        }
    }
    return result;
}

struct StrategyEvaluation final {
    size_t row_count = 0;
    size_t reference_successes = 0;
    size_t reference_failures = 0;
    size_t factor_identity_mismatches = 0;
    IterationCost forward_iterations = 0;
};

[[nodiscard]] inline StrategyEvaluation evaluate_strategy(const StrategyMatrix& matrix,
                                                          std::span<const size_t> row_indices,
                                                          std::span<const size_t> order) {
    validate_matrix(matrix);
    validate_order(matrix, order);
    StrategyEvaluation evaluation;
    evaluation.row_count = row_indices.size();
    std::vector<bool> seen(matrix.rows.size(), false);
    for (const size_t row_index : row_indices) {
        if (row_index >= matrix.rows.size() || seen[row_index]) {
            throw std::runtime_error("SQUFOF strategy evaluation has an invalid row selection");
        }
        seen[row_index] = true;
        const MatrixRow& row = matrix.rows[row_index];
        if (row.reference_factor == 1) {
            ++evaluation.reference_failures;
        } else {
            ++evaluation.reference_successes;
        }
        const ReplayResult replayed = replay_row(matrix, row_index, order);
        evaluation.forward_iterations =
            checked_add(evaluation.forward_iterations, replayed.forward_iterations);
        if (replayed.factor != row.reference_factor) {
            ++evaluation.factor_identity_mismatches;
        }
    }
    return evaluation;
}

struct OptimizationResult final {
    bool feasible = false;
    IterationCost training_cost = 0;
    std::vector<size_t> order;
};

[[nodiscard]] inline OptimizationResult
optimize_training_rows(const StrategyMatrix& matrix, std::span<const size_t> training_rows,
                       std::span<const DataSplit> assignment) {
    validate_matrix(matrix);
    validate_split_assignment(matrix, assignment);
    // The fixed production schedule has 11 slots. Keep one growth slot while
    // preventing an accidental exponential blow-up in routine test lanes.
    if (matrix.multipliers.size() > 12) {
        throw std::runtime_error("exact SQUFOF subset DP is limited to 12 multiplier slots");
    }

    std::vector<bool> selected(matrix.rows.size(), false);
    for (const size_t row_index : training_rows) {
        if (row_index >= matrix.rows.size() || selected[row_index]) {
            throw std::runtime_error("SQUFOF optimizer has an invalid training row selection");
        }
        if (assignment[row_index] != DataSplit::train) {
            throw std::runtime_error("SQUFOF optimizer received a non-training row");
        }
        selected[row_index] = true;
    }

    const size_t slot_count = matrix.multipliers.size();
    const size_t state_count = size_t{1} << slot_count;
    std::vector<IterationCost> best_cost(state_count, 0);
    std::vector<bool> reached(state_count, false);
    std::vector<std::vector<size_t>> best_order(state_count);
    reached[0] = true;
    best_cost[0] = 0;

    for (size_t used_mask = 0; used_mask < state_count; ++used_mask) {
        if (!reached[used_mask]) {
            continue;
        }
        for (size_t next_slot = 0; next_slot < slot_count; ++next_slot) {
            const size_t next_bit = size_t{1} << next_slot;
            if ((used_mask & next_bit) != 0) {
                continue;
            }

            bool transition_valid = true;
            IterationCost added_cost = 0;
            for (const size_t row_index : training_rows) {
                const MatrixRow& row = matrix.rows[row_index];
                bool already_resolved = false;
                if (row.reference_factor != 1) {
                    for (size_t slot = 0; slot < slot_count; ++slot) {
                        if ((used_mask & (size_t{1} << slot)) != 0 &&
                            row.cells[slot].accepted_factor == row.reference_factor) {
                            already_resolved = true;
                            break;
                        }
                    }
                }
                if (already_resolved) {
                    continue;
                }

                const MatrixCell& cell = row.cells[next_slot];
                if (cell.status == CellStatus::ineligible_input ||
                    cell.status == CellStatus::invalid_slot) {
                    transition_valid = false;
                    break;
                }
                added_cost = checked_add(added_cost, cell.forward_iterations);
                if (cell.accepted_factor > 1 && cell.accepted_factor != row.reference_factor) {
                    // A reordered first hit must preserve the exact production
                    // factor, not merely return its complementary proper factor.
                    transition_valid = false;
                    break;
                }
            }
            if (!transition_valid) {
                continue;
            }

            const size_t next_mask = used_mask | next_bit;
            const IterationCost candidate_cost = checked_add(best_cost[used_mask], added_cost);
            std::vector<size_t> candidate_order = best_order[used_mask];
            candidate_order.push_back(next_slot);
            if (!reached[next_mask] || candidate_cost < best_cost[next_mask] ||
                (candidate_cost == best_cost[next_mask] &&
                 (best_order[next_mask].empty() || candidate_order < best_order[next_mask]))) {
                reached[next_mask] = true;
                best_cost[next_mask] = candidate_cost;
                best_order[next_mask] = std::move(candidate_order);
            }
        }
    }

    const size_t full_mask = state_count - 1;
    if (!reached[full_mask]) {
        return {};
    }
    return {true, best_cost[full_mask], std::move(best_order[full_mask])};
}

} // namespace gnfs::tests::squfof_strategy
