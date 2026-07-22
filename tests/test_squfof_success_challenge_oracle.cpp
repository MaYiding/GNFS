// Deterministic two-cap oracle for the sealed SQUFOF success challenge.

#include <gnfs/cofactor/squfof.hpp>

#include "fixtures/squfof_success_challenge_v1.hpp"
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
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using gnfs::cofactor::SQUFOF;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_BUDGET;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_CASES_PER_PROFILE;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_HIGH;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_LOW;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_COUNT;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_ROW_COUNT;
using gnfs::tests::fixtures::SqufofSuccessChallengeProfile;
using gnfs::tests::squfof_strategy::CellStatus;
using gnfs::tests::squfof_strategy::Digest;
using gnfs::tests::squfof_strategy::DigestBuilder;

constexpr uint32_t BASELINE_CAP = 20000;
constexpr uint32_t CANDIDATE_CAP = 10056;
constexpr size_t POLICY_COUNT = 2;
constexpr size_t MAX_WORKERS = 8;
constexpr size_t MINIMUM_SUCCESSES = 8;
constexpr size_t MINIMUM_VALIDATION_SUCCESSES = 1;
constexpr size_t MINIMUM_CONFIRMATION_SUCCESSES = 1;
constexpr uint32_t MINIMUM_WORK_GAIN_PERCENT = 3;
constexpr std::array<uint32_t, POLICY_COUNT> POLICY_CAPS{{BASELINE_CAP, CANDIDATE_CAP}};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

[[nodiscard]] uint64_t checked_add(uint64_t left, uint64_t right) {
    if (right > std::numeric_limits<uint64_t>::max() - left) {
        throw std::overflow_error("success-challenge iteration sum overflowed");
    }
    return left + right;
}

[[nodiscard]] bool is_proper_factor(uint64_t n, uint64_t factor) noexcept {
    return factor > 1 && factor < n && n % factor == 0;
}

enum class DataSplit : uint8_t {
    train = 0,
    validation = 1,
    confirmation = 2,
};

enum class MismatchClass : uint8_t {
    same = 0,
    new_failure = 1,
    new_success = 2,
    changed_factor = 3,
};

enum class Decision : uint8_t {
    insufficient_evidence = 0,
    no_go = 1,
    eligible = 2,
};

[[nodiscard]] std::string_view split_name(DataSplit split) noexcept {
    switch (split) {
    case DataSplit::train:
        return "train";
    case DataSplit::validation:
        return "validation";
    case DataSplit::confirmation:
        return "confirmation";
    }
    return "unknown";
}

[[nodiscard]] std::string_view profile_name(SqufofSuccessChallengeProfile profile) noexcept {
    switch (profile) {
    case SqufofSuccessChallengeProfile::close_balanced:
        return "close_balanced";
    case SqufofSuccessChallengeProfile::mildly_skewed:
        return "mildly_skewed";
    case SqufofSuccessChallengeProfile::moderately_skewed:
        return "moderately_skewed";
    }
    return "unknown";
}

[[nodiscard]] std::string_view decision_name(Decision decision) noexcept {
    switch (decision) {
    case Decision::insufficient_evidence:
        return "insufficient_evidence";
    case Decision::no_go:
        return "no_go";
    case Decision::eligible:
        return "eligible";
    }
    return "unknown";
}

[[nodiscard]] constexpr uint64_t stable_n_hash(uint64_t n) noexcept {
    n ^= n >> 30;
    n *= UINT64_C(0xbf58476d1ce4e5b9);
    n ^= n >> 27;
    n *= UINT64_C(0x94d049bb133111eb);
    n ^= n >> 31;
    return n;
}

using SplitAssignment = std::array<DataSplit, SQUFOF_SUCCESS_CHALLENGE_V1_ROW_COUNT>;

[[nodiscard]] SplitAssignment make_split_assignment() {
    SplitAssignment assignment{};
    constexpr std::array<DataSplit, 4> CYCLE{{
        DataSplit::train,
        DataSplit::train,
        DataSplit::validation,
        DataSplit::confirmation,
    }};
    for (size_t profile = 0; profile < SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_COUNT; ++profile) {
        std::array<size_t, SQUFOF_SUCCESS_CHALLENGE_V1_CASES_PER_PROFILE> indices{};
        for (size_t offset = 0; offset < indices.size(); ++offset) {
            indices[offset] = profile * indices.size() + offset;
        }
        std::sort(indices.begin(), indices.end(), [](size_t left, size_t right) {
            const uint64_t left_n = SQUFOF_SUCCESS_CHALLENGE_V1[left].n;
            const uint64_t right_n = SQUFOF_SUCCESS_CHALLENGE_V1[right].n;
            const uint64_t left_hash = stable_n_hash(left_n);
            const uint64_t right_hash = stable_n_hash(right_n);
            return left_hash != right_hash ? left_hash < right_hash : left_n < right_n;
        });
        for (size_t rank = 0; rank < indices.size(); ++rank) {
            assignment[indices[rank]] = CYCLE[rank % CYCLE.size()];
        }
    }
    return assignment;
}

[[nodiscard]] Digest split_digest(const SplitAssignment& assignment) {
    DigestBuilder builder("GNFS-SQUFOF-SUCCESS-CHALLENGE-SPLIT-V1");
    builder.append_u32(1);
    builder.append_u64(SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_LOW);
    builder.append_u64(SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_HIGH);
    builder.append_u64(static_cast<uint64_t>(SQUFOF_SUCCESS_CHALLENGE_V1.size()));
    for (size_t row = 0; row < SQUFOF_SUCCESS_CHALLENGE_V1.size(); ++row) {
        const auto& source = SQUFOF_SUCCESS_CHALLENGE_V1[row];
        builder.append_u64(static_cast<uint64_t>(row));
        builder.append_u64(source.n);
        builder.append_byte(static_cast<uint8_t>(source.caller));
        builder.append_byte(static_cast<uint8_t>(source.profile));
        builder.append_u32(source.budget);
        builder.append_byte(static_cast<uint8_t>(assignment[row]));
    }
    return builder.finish();
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

struct Cell final {
    uint32_t cap = 0;
    CellStatus status = CellStatus::ineligible_input;
    uint64_t forward_iterations = 0;
    bool core_hit = false;
    uint64_t accepted_factor = 0;

    [[nodiscard]] bool same_observation(const Cell& other) const noexcept {
        return status == other.status && forward_iterations == other.forward_iterations &&
               core_hit == other.core_hit && accepted_factor == other.accepted_factor;
    }
};

struct SlotObservation final {
    std::array<Cell, POLICY_COUNT> policies{};
};

struct RowObservation final {
    uint64_t n = 0;
    SqufofSuccessChallengeProfile profile = SqufofSuccessChallengeProfile::close_balanced;
    std::array<uint64_t, POLICY_COUNT> raw_factors{{1, 1}};
    std::vector<SlotObservation> slots;
};

struct ObservationMatrix final {
    Digest schedule_identity;
    std::vector<uint64_t> multipliers;
    std::vector<RowObservation> rows;
};

[[nodiscard]] Cell observe_cell(uint64_t n, size_t slot, uint32_t cap) {
    const auto probe = SQUFOF::probe_multiplier(n, slot, cap);
    return {
        .cap = cap,
        .status = translate_status(probe.status),
        .forward_iterations = probe.forward_iterations,
        .core_hit = probe.core_hit,
        .accepted_factor = probe.accepted_factor,
    };
}

[[nodiscard]] RowObservation observe_row(size_t row_index, size_t slot_count) {
    const auto& source = SQUFOF_SUCCESS_CHALLENGE_V1[row_index];
    RowObservation row;
    row.n = source.n;
    row.profile = source.profile;
    for (size_t policy = 0; policy < POLICY_COUNT; ++policy) {
        row.raw_factors[policy] = SQUFOF::factor(row.n, POLICY_CAPS[policy]);
    }
    row.slots.resize(slot_count);
    for (size_t slot = 0; slot < slot_count; ++slot) {
        for (size_t policy = 0; policy < POLICY_COUNT; ++policy) {
            row.slots[slot].policies[policy] = observe_cell(row.n, slot, POLICY_CAPS[policy]);
        }
    }
    return row;
}

[[nodiscard]] ObservationMatrix build_matrix() {
    ObservationMatrix matrix;
    const auto& schedule = SQUFOF::multiplier_schedule();
    matrix.multipliers.assign(schedule.begin(), schedule.end());
    matrix.schedule_identity = gnfs::tests::squfof_strategy::schedule_digest(matrix.multipliers);
    matrix.rows.resize(SQUFOF_SUCCESS_CHALLENGE_V1.size());

    const unsigned reported_threads = std::max(1U, std::thread::hardware_concurrency());
    const size_t workers =
        std::min<size_t>({matrix.rows.size(), static_cast<size_t>(reported_threads), MAX_WORKERS});
    std::atomic<size_t> next_row{0};
    std::vector<std::future<void>> futures;
    futures.reserve(workers);
    for (size_t worker = 0; worker < workers; ++worker) {
        futures.push_back(std::async(std::launch::async, [&]() {
            while (true) {
                const size_t row = next_row.fetch_add(1, std::memory_order_relaxed);
                if (row >= matrix.rows.size()) {
                    return;
                }
                matrix.rows[row] = observe_row(row, matrix.multipliers.size());
            }
        }));
    }
    for (auto& future : futures) {
        future.get();
    }
    return matrix;
}

struct Replay final {
    uint64_t factor = 1;
    uint64_t forward_iterations = 0;
    uint64_t slots_visited = 0;
};

[[nodiscard]] Replay replay_row(const RowObservation& row, size_t policy) {
    Replay replay;
    for (const SlotObservation& slot : row.slots) {
        const Cell& cell = slot.policies[policy];
        ++replay.slots_visited;
        if (cell.status == CellStatus::attempted) {
            replay.forward_iterations =
                checked_add(replay.forward_iterations, cell.forward_iterations);
        }
        if (cell.accepted_factor != 0) {
            replay.factor = cell.accepted_factor;
            return replay;
        }
    }
    return replay;
}

void validate_cell(const RowObservation& row, const Cell& cell, size_t policy) {
    require(cell.cap == POLICY_CAPS[policy], "success-challenge cell has the wrong cap");
    if (cell.status != CellStatus::attempted) {
        require(cell.forward_iterations == 0 && !cell.core_hit && cell.accepted_factor == 0,
                "non-attempted success-challenge cell contains dynamic results");
        return;
    }
    require(cell.forward_iterations <= cell.cap,
            "success-challenge cell exceeded its iteration cap");
    require(cell.accepted_factor == 0 || is_proper_factor(row.n, cell.accepted_factor),
            "success-challenge cell contains an invalid factor");
    require(cell.accepted_factor == 0 || cell.core_hit,
            "accepted success-challenge factor lacks a core hit");
}

void validate_matrix(const ObservationMatrix& matrix) {
    constexpr std::array<uint64_t, 11> EXPECTED_SCHEDULE{{1, 15, 3, 5, 7, 11, 21, 33, 35, 55, 77}};
    constexpr Digest EXPECTED_SCHEDULE_DIGEST{UINT64_C(12597619067809015512),
                                              UINT64_C(9409246306371504405)};
    require(matrix.multipliers.size() == EXPECTED_SCHEDULE.size() &&
                std::equal(matrix.multipliers.begin(), matrix.multipliers.end(),
                           EXPECTED_SCHEDULE.begin()) &&
                matrix.schedule_identity == EXPECTED_SCHEDULE_DIGEST,
            "production SQUFOF multiplier schedule changed");
    require(matrix.rows.size() == SQUFOF_SUCCESS_CHALLENGE_V1.size(),
            "success-challenge matrix has the wrong row count");

    for (size_t row_index = 0; row_index < matrix.rows.size(); ++row_index) {
        const RowObservation& row = matrix.rows[row_index];
        const auto& source = SQUFOF_SUCCESS_CHALLENGE_V1[row_index];
        require(row.n == source.n && row.profile == source.profile && source.budget == BASELINE_CAP,
                "success-challenge matrix row differs from the sealed corpus");
        require(row.slots.size() == matrix.multipliers.size(),
                "success-challenge matrix row has the wrong slot count");
        for (const uint64_t factor : row.raw_factors) {
            require(factor == 1 || is_proper_factor(row.n, factor),
                    "success-challenge factor() returned an invalid factor");
        }

        for (const SlotObservation& slot : row.slots) {
            const Cell& baseline = slot.policies[0];
            const Cell& candidate = slot.policies[1];
            validate_cell(row, baseline, 0);
            validate_cell(row, candidate, 1);
            require(baseline.status == candidate.status,
                    "success-challenge probe status changed with the cap");
            require(baseline.status != CellStatus::ineligible_input &&
                        baseline.status != CellStatus::invalid_slot,
                    "success-challenge matrix contains an incomplete slot");
            if (baseline.status != CellStatus::attempted) {
                require(baseline.same_observation(candidate),
                        "non-attempted success-challenge observations differ by cap");
                continue;
            }
            require(candidate.forward_iterations <= baseline.forward_iterations,
                    "candidate observation is not a baseline prefix");
            const bool candidate_terminated =
                candidate.forward_iterations < CANDIDATE_CAP || candidate.core_hit;
            if (candidate_terminated || baseline.forward_iterations <= CANDIDATE_CAP) {
                require(baseline.same_observation(candidate),
                        "completed candidate observation differs from baseline");
            } else {
                require(candidate.forward_iterations == CANDIDATE_CAP && !candidate.core_hit &&
                            candidate.accepted_factor == 0,
                        "capped candidate observation has an invalid terminal state");
            }
        }

        for (size_t policy = 0; policy < POLICY_COUNT; ++policy) {
            const Replay replay = replay_row(row, policy);
            require(replay.factor == row.raw_factors[policy],
                    "success-challenge slot replay differs from factor()");
        }
    }
}

[[nodiscard]] Digest matrix_digest(const ObservationMatrix& matrix) {
    DigestBuilder builder("GNFS-SQUFOF-SUCCESS-CHALLENGE-MATRIX-V1");
    builder.append_u32(1);
    builder.append_u64(SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_LOW);
    builder.append_u64(SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_HIGH);
    builder.append_u64(matrix.schedule_identity.low);
    builder.append_u64(matrix.schedule_identity.high);
    builder.append_u64(static_cast<uint64_t>(matrix.rows.size()));
    builder.append_u64(static_cast<uint64_t>(matrix.multipliers.size()));
    builder.append_u64(POLICY_COUNT);
    for (size_t policy = 0; policy < POLICY_COUNT; ++policy) {
        builder.append_byte(static_cast<uint8_t>(policy));
        builder.append_u32(POLICY_CAPS[policy]);
    }
    for (size_t slot = 0; slot < matrix.multipliers.size(); ++slot) {
        builder.append_u64(static_cast<uint64_t>(slot));
        builder.append_u64(matrix.multipliers[slot]);
    }
    for (size_t row_index = 0; row_index < matrix.rows.size(); ++row_index) {
        const RowObservation& row = matrix.rows[row_index];
        const auto& source = SQUFOF_SUCCESS_CHALLENGE_V1[row_index];
        builder.append_u64(static_cast<uint64_t>(row_index));
        builder.append_u64(row.n);
        builder.append_byte(static_cast<uint8_t>(source.caller));
        builder.append_byte(static_cast<uint8_t>(row.profile));
        builder.append_u32(source.budget);
        for (size_t policy = 0; policy < POLICY_COUNT; ++policy) {
            builder.append_byte(static_cast<uint8_t>(policy));
            builder.append_u64(row.raw_factors[policy]);
        }
        for (size_t slot = 0; slot < row.slots.size(); ++slot) {
            builder.append_u64(static_cast<uint64_t>(slot));
            builder.append_u64(matrix.multipliers[slot]);
            for (size_t policy = 0; policy < POLICY_COUNT; ++policy) {
                const Cell& cell = row.slots[slot].policies[policy];
                builder.append_byte(static_cast<uint8_t>(policy));
                builder.append_u32(cell.cap);
                builder.append_byte(static_cast<uint8_t>(cell.status));
                builder.append_u64(cell.forward_iterations);
                builder.append_byte(cell.core_hit ? UINT8_C(1) : UINT8_C(0));
                builder.append_u64(cell.accepted_factor);
            }
        }
    }
    return builder.finish();
}

[[nodiscard]] MismatchClass mismatch_class(const RowObservation& row) noexcept {
    const uint64_t baseline = row.raw_factors[0];
    const uint64_t candidate = row.raw_factors[1];
    if (candidate == baseline) {
        return MismatchClass::same;
    }
    if (baseline != 1 && candidate == 1) {
        return MismatchClass::new_failure;
    }
    if (baseline == 1 && candidate != 1) {
        return MismatchClass::new_success;
    }
    return MismatchClass::changed_factor;
}

struct Aggregate final {
    uint64_t rows = 0;
    uint64_t baseline_successes = 0;
    uint64_t baseline_failures = 0;
    uint64_t candidate_successes = 0;
    uint64_t candidate_failures = 0;
    uint64_t factor_identity_mismatches = 0;
    uint64_t new_failures = 0;
    uint64_t new_successes = 0;
    uint64_t changed_factors = 0;
    uint64_t baseline_success_iterations = 0;
    uint64_t baseline_failure_iterations = 0;
    uint64_t baseline_total_iterations = 0;
    uint64_t candidate_success_iterations = 0;
    uint64_t candidate_failure_iterations = 0;
    uint64_t candidate_total_iterations = 0;
};

[[nodiscard]] Aggregate aggregate_rows(const ObservationMatrix& matrix,
                                       std::span<const size_t> rows) {
    Aggregate aggregate;
    aggregate.rows = static_cast<uint64_t>(rows.size());
    for (const size_t row_index : rows) {
        const RowObservation& row = matrix.rows[row_index];
        const Replay baseline = replay_row(row, 0);
        const Replay candidate = replay_row(row, 1);
        const bool baseline_success = is_proper_factor(row.n, row.raw_factors[0]);
        const bool candidate_success = is_proper_factor(row.n, row.raw_factors[1]);
        if (baseline_success) {
            ++aggregate.baseline_successes;
            aggregate.baseline_success_iterations =
                checked_add(aggregate.baseline_success_iterations, baseline.forward_iterations);
        } else {
            ++aggregate.baseline_failures;
            aggregate.baseline_failure_iterations =
                checked_add(aggregate.baseline_failure_iterations, baseline.forward_iterations);
        }
        if (candidate_success) {
            ++aggregate.candidate_successes;
            aggregate.candidate_success_iterations =
                checked_add(aggregate.candidate_success_iterations, candidate.forward_iterations);
        } else {
            ++aggregate.candidate_failures;
            aggregate.candidate_failure_iterations =
                checked_add(aggregate.candidate_failure_iterations, candidate.forward_iterations);
        }
        aggregate.baseline_total_iterations =
            checked_add(aggregate.baseline_total_iterations, baseline.forward_iterations);
        aggregate.candidate_total_iterations =
            checked_add(aggregate.candidate_total_iterations, candidate.forward_iterations);
        switch (mismatch_class(row)) {
        case MismatchClass::same:
            break;
        case MismatchClass::new_failure:
            ++aggregate.factor_identity_mismatches;
            ++aggregate.new_failures;
            break;
        case MismatchClass::new_success:
            ++aggregate.factor_identity_mismatches;
            ++aggregate.new_successes;
            break;
        case MismatchClass::changed_factor:
            ++aggregate.factor_identity_mismatches;
            ++aggregate.changed_factors;
            break;
        }
    }
    return aggregate;
}

[[nodiscard]] std::vector<size_t> select_rows(const SplitAssignment& assignment, DataSplit split,
                                              SqufofSuccessChallengeProfile profile) {
    std::vector<size_t> rows;
    for (size_t row = 0; row < SQUFOF_SUCCESS_CHALLENGE_V1.size(); ++row) {
        if (assignment[row] == split && SQUFOF_SUCCESS_CHALLENGE_V1[row].profile == profile) {
            rows.push_back(row);
        }
    }
    return rows;
}

[[nodiscard]] std::vector<size_t> select_split_rows(const SplitAssignment& assignment,
                                                    DataSplit split) {
    std::vector<size_t> rows;
    for (size_t row = 0; row < assignment.size(); ++row) {
        if (assignment[row] == split) {
            rows.push_back(row);
        }
    }
    return rows;
}

struct EvaluationRecord final {
    DataSplit split;
    SqufofSuccessChallengeProfile profile;
    Aggregate aggregate;
};

[[nodiscard]] std::vector<EvaluationRecord> build_records(const ObservationMatrix& matrix,
                                                          const SplitAssignment& assignment) {
    std::vector<EvaluationRecord> records;
    records.reserve(9);
    for (size_t split_index = 0; split_index < 3; ++split_index) {
        const auto split = static_cast<DataSplit>(split_index);
        for (size_t profile_index = 0; profile_index < 3; ++profile_index) {
            const auto profile = static_cast<SqufofSuccessChallengeProfile>(profile_index);
            const std::vector<size_t> rows = select_rows(assignment, split, profile);
            records.push_back({split, profile, aggregate_rows(matrix, rows)});
        }
    }
    return records;
}

void append_aggregate(DigestBuilder& builder, const Aggregate& value) {
    builder.append_u64(value.rows);
    builder.append_u64(value.baseline_successes);
    builder.append_u64(value.baseline_failures);
    builder.append_u64(value.candidate_successes);
    builder.append_u64(value.candidate_failures);
    builder.append_u64(value.factor_identity_mismatches);
    builder.append_u64(value.new_failures);
    builder.append_u64(value.new_successes);
    builder.append_u64(value.changed_factors);
    builder.append_u64(value.baseline_success_iterations);
    builder.append_u64(value.baseline_failure_iterations);
    builder.append_u64(value.baseline_total_iterations);
    builder.append_u64(value.candidate_success_iterations);
    builder.append_u64(value.candidate_failure_iterations);
    builder.append_u64(value.candidate_total_iterations);
}

struct Gates final {
    bool total_coverage = false;
    bool validation_coverage = false;
    bool confirmation_coverage = false;
    bool coverage = false;
    bool identity = false;
    bool new_failure = false;
    bool work = false;
    Decision decision = Decision::insufficient_evidence;
};

[[nodiscard]] bool minimum_gain(uint64_t baseline, uint64_t candidate, uint32_t percent) {
    if (percent > 100 || baseline > std::numeric_limits<uint64_t>::max() / 100 ||
        candidate > std::numeric_limits<uint64_t>::max() / 100) {
        throw std::overflow_error("success-challenge work comparison overflowed");
    }
    return candidate * 100 <= baseline * (100 - percent);
}

[[nodiscard]] Gates evaluate_gates(const Aggregate& overall, const Aggregate& validation,
                                   const Aggregate& confirmation) {
    Gates gates;
    gates.total_coverage = overall.baseline_successes >= MINIMUM_SUCCESSES;
    gates.validation_coverage = validation.baseline_successes >= MINIMUM_VALIDATION_SUCCESSES;
    gates.confirmation_coverage = confirmation.baseline_successes >= MINIMUM_CONFIRMATION_SUCCESSES;
    gates.coverage =
        gates.total_coverage && gates.validation_coverage && gates.confirmation_coverage;
    gates.identity = overall.factor_identity_mismatches == 0;
    gates.new_failure = overall.new_failures == 0;
    gates.work = minimum_gain(overall.baseline_total_iterations, overall.candidate_total_iterations,
                              MINIMUM_WORK_GAIN_PERCENT);
    if (!gates.identity || !gates.new_failure) {
        gates.decision = Decision::no_go;
    } else if (!gates.coverage) {
        gates.decision = Decision::insufficient_evidence;
    } else if (!gates.work) {
        gates.decision = Decision::no_go;
    } else {
        gates.decision = Decision::eligible;
    }
    return gates;
}

void validate_gate_precedence() {
    Aggregate overall;
    overall.baseline_successes = MINIMUM_SUCCESSES;
    overall.baseline_total_iterations = 100;
    overall.candidate_total_iterations = 90;
    Aggregate validation;
    validation.baseline_successes = MINIMUM_VALIDATION_SUCCESSES;
    Aggregate confirmation;
    confirmation.baseline_successes = MINIMUM_CONFIRMATION_SUCCESSES;

    require(evaluate_gates(overall, validation, confirmation).decision == Decision::eligible,
            "success-challenge gates rejected an eligible synthetic result");
    overall.factor_identity_mismatches = 1;
    overall.baseline_successes = 1;
    require(evaluate_gates(overall, validation, confirmation).decision == Decision::no_go,
            "success-challenge identity failure did not take precedence over coverage");
    overall.factor_identity_mismatches = 0;
    overall.new_failures = 1;
    overall.baseline_successes = MINIMUM_SUCCESSES;
    require(evaluate_gates(overall, validation, confirmation).decision == Decision::no_go,
            "success-challenge new-failure gate was not independently enforced");
    overall.new_failures = 0;
    overall.baseline_successes = 1;
    require(evaluate_gates(overall, validation, confirmation).decision ==
                Decision::insufficient_evidence,
            "success-challenge coverage shortfall did not remain insufficient evidence");
    overall.baseline_successes = MINIMUM_SUCCESSES;
    overall.candidate_total_iterations = 100;
    require(evaluate_gates(overall, validation, confirmation).decision == Decision::no_go,
            "success-challenge work regression did not produce no-go");
}

[[nodiscard]] Digest result_digest(const ObservationMatrix& matrix,
                                   const SplitAssignment& assignment, Digest matrix_identity,
                                   Digest split_identity, std::span<const EvaluationRecord> records,
                                   const Aggregate& overall, const Gates& gates) {
    DigestBuilder builder("GNFS-SQUFOF-SUCCESS-CHALLENGE-RESULT-V1");
    builder.append_u32(1);
    builder.append_u64(matrix_identity.low);
    builder.append_u64(matrix_identity.high);
    builder.append_u64(split_identity.low);
    builder.append_u64(split_identity.high);
    builder.append_u64(static_cast<uint64_t>(matrix.rows.size()));
    for (size_t row_index = 0; row_index < matrix.rows.size(); ++row_index) {
        const RowObservation& row = matrix.rows[row_index];
        builder.append_u64(static_cast<uint64_t>(row_index));
        builder.append_byte(static_cast<uint8_t>(assignment[row_index]));
        builder.append_byte(static_cast<uint8_t>(row.profile));
        for (size_t policy = 0; policy < POLICY_COUNT; ++policy) {
            const Replay replay = replay_row(row, policy);
            builder.append_byte(static_cast<uint8_t>(policy));
            builder.append_u64(row.raw_factors[policy]);
            builder.append_u64(replay.factor);
            builder.append_u64(replay.forward_iterations);
            builder.append_u64(replay.slots_visited);
        }
        builder.append_byte(static_cast<uint8_t>(mismatch_class(row)));
    }
    for (const EvaluationRecord& record : records) {
        builder.append_byte(static_cast<uint8_t>(record.split));
        builder.append_byte(static_cast<uint8_t>(record.profile));
        append_aggregate(builder, record.aggregate);
    }
    append_aggregate(builder, overall);
    builder.append_u64(MINIMUM_SUCCESSES);
    builder.append_u64(MINIMUM_VALIDATION_SUCCESSES);
    builder.append_u64(MINIMUM_CONFIRMATION_SUCCESSES);
    builder.append_u32(MINIMUM_WORK_GAIN_PERCENT);
    builder.append_byte(gates.total_coverage ? UINT8_C(1) : UINT8_C(0));
    builder.append_byte(gates.validation_coverage ? UINT8_C(1) : UINT8_C(0));
    builder.append_byte(gates.confirmation_coverage ? UINT8_C(1) : UINT8_C(0));
    builder.append_byte(gates.coverage ? UINT8_C(1) : UINT8_C(0));
    builder.append_byte(gates.identity ? UINT8_C(1) : UINT8_C(0));
    builder.append_byte(gates.new_failure ? UINT8_C(1) : UINT8_C(0));
    builder.append_byte(gates.work ? UINT8_C(1) : UINT8_C(0));
    builder.append_byte(static_cast<uint8_t>(gates.decision));
    return builder.finish();
}

void emit_record(const EvaluationRecord& record) {
    const Aggregate& value = record.aggregate;
    std::cout << "GNFS_SQUFOF_SUCCESS_CHALLENGE_EVAL_V1"
              << " status=pass schema=1 split=" << split_name(record.split)
              << " profile=" << profile_name(record.profile) << " rows=" << value.rows
              << " baseline_successes=" << value.baseline_successes
              << " baseline_failures=" << value.baseline_failures
              << " candidate_successes=" << value.candidate_successes
              << " candidate_failures=" << value.candidate_failures
              << " factor_identity_mismatches=" << value.factor_identity_mismatches
              << " new_failures=" << value.new_failures << " new_successes=" << value.new_successes
              << " changed_factors=" << value.changed_factors
              << " baseline_success_iterations=" << value.baseline_success_iterations
              << " baseline_failure_iterations=" << value.baseline_failure_iterations
              << " baseline_iterations=" << value.baseline_total_iterations
              << " candidate_success_iterations=" << value.candidate_success_iterations
              << " candidate_failure_iterations=" << value.candidate_failure_iterations
              << " candidate_iterations=" << value.candidate_total_iterations << '\n';
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
    validate_gate_precedence();
    require(SQUFOF_SUCCESS_CHALLENGE_V1_BUDGET == BASELINE_CAP,
            "sealed challenge budget differs from the production baseline");
    const SplitAssignment assignment = make_split_assignment();
    const Digest observed_split = split_digest(assignment);
    constexpr Digest EXPECTED_SPLIT{UINT64_C(5936611983363779581), UINT64_C(6396469101558652297)};
    require(observed_split == EXPECTED_SPLIT,
            "success-challenge split differs from the sealed contract");

    const ObservationMatrix matrix = build_matrix();
    validate_matrix(matrix);
    const Digest observed_matrix = matrix_digest(matrix);
    const std::vector<EvaluationRecord> records = build_records(matrix, assignment);

    std::vector<size_t> all_rows(matrix.rows.size());
    for (size_t row = 0; row < all_rows.size(); ++row) {
        all_rows[row] = row;
    }
    const Aggregate overall = aggregate_rows(matrix, all_rows);
    const std::vector<size_t> validation_rows =
        select_split_rows(assignment, DataSplit::validation);
    const std::vector<size_t> confirmation_rows =
        select_split_rows(assignment, DataSplit::confirmation);
    const Aggregate validation = aggregate_rows(matrix, validation_rows);
    const Aggregate confirmation = aggregate_rows(matrix, confirmation_rows);
    const Gates gates = evaluate_gates(overall, validation, confirmation);
    const Digest observed_result =
        result_digest(matrix, assignment, observed_matrix, observed_split, records, overall, gates);

    require(records.size() == 9 && overall.rows == 192 && overall.baseline_successes == 5 &&
                overall.baseline_failures == 187 && overall.candidate_successes == 3 &&
                overall.candidate_failures == 189 && overall.factor_identity_mismatches == 2 &&
                overall.new_failures == 2 && overall.new_successes == 0 &&
                overall.changed_factors == 0 &&
                overall.baseline_total_iterations == UINT64_C(41526608) &&
                overall.candidate_total_iterations == UINT64_C(20976188),
            "success-challenge aggregate differs from its sealed first observation");
    require(validation.baseline_successes == 1 && validation.candidate_successes == 0 &&
                validation.new_failures == 1 && confirmation.baseline_successes == 3 &&
                confirmation.candidate_successes == 2 && confirmation.new_failures == 1,
            "success-challenge split result differs from its sealed first observation");
    require(!gates.total_coverage && gates.validation_coverage && gates.confirmation_coverage &&
                !gates.coverage && !gates.identity && !gates.new_failure && gates.work &&
                gates.decision == Decision::no_go,
            "success-challenge gate result differs from its sealed first observation");

    // Frozen from the first sealed observation; no bless/update mode is provided.
    constexpr Digest EXPECTED_MATRIX{UINT64_C(66321629368464418), UINT64_C(14097916444529299682)};
    constexpr Digest EXPECTED_RESULT{UINT64_C(8167599806903207849), UINT64_C(12264992225924573372)};
    if (!(observed_matrix == EXPECTED_MATRIX) || !(observed_result == EXPECTED_RESULT)) {
        fail("sealed success-challenge matrix or result identity changed");
    }

    for (const EvaluationRecord& record : records) {
        emit_record(record);
    }
    std::cout << "GNFS_SQUFOF_SUCCESS_CHALLENGE_ORACLE_SUMMARY_V1"
              << " status=pass schema=1 offline_decision=" << decision_name(gates.decision)
              << " rows=" << overall.rows << " slots=" << matrix.multipliers.size()
              << " policies=" << POLICY_COUNT << " max_workers=" << MAX_WORKERS
              << " production_cap=" << BASELINE_CAP << " candidate_cap=" << CANDIDATE_CAP
              << " minimum_successes=" << MINIMUM_SUCCESSES
              << " minimum_work_gain_percent=" << MINIMUM_WORK_GAIN_PERCENT
              << " coverage_gate=" << (gates.coverage ? "pass" : "fail")
              << " validation_coverage_gate=" << (gates.validation_coverage ? "pass" : "fail")
              << " confirmation_coverage_gate=" << (gates.confirmation_coverage ? "pass" : "fail")
              << " identity_gate=" << (gates.identity ? "pass" : "fail")
              << " new_failure_gate=" << (gates.new_failure ? "pass" : "fail")
              << " work_gate=" << (gates.work ? "pass" : "fail")
              << " baseline_successes=" << overall.baseline_successes
              << " candidate_successes=" << overall.candidate_successes
              << " factor_identity_mismatches=" << overall.factor_identity_mismatches
              << " new_failures=" << overall.new_failures
              << " new_successes=" << overall.new_successes
              << " changed_factors=" << overall.changed_factors
              << " baseline_iterations=" << overall.baseline_total_iterations
              << " candidate_iterations=" << overall.candidate_total_iterations
              << " corpus_digest_low=" << SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_LOW
              << " corpus_digest_high=" << SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_HIGH
              << " split_digest_low=" << observed_split.low
              << " split_digest_high=" << observed_split.high
              << " schedule_digest_low=" << matrix.schedule_identity.low
              << " schedule_digest_high=" << matrix.schedule_identity.high
              << " matrix_digest_low=" << observed_matrix.low
              << " matrix_digest_high=" << observed_matrix.high
              << " result_digest_low=" << observed_result.low
              << " result_digest_high=" << observed_result.high
              << " policy_fitted=false timing_asserted=false\n";
}

} // namespace

int main() {
    try {
        run_oracle();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GNFS_SQUFOF_SUCCESS_CHALLENGE_ORACLE_SUMMARY_V1 status=fail error="
                  << sanitize_token(error.what()) << '\n';
        return 1;
    } catch (...) {
        std::cerr << "GNFS_SQUFOF_SUCCESS_CHALLENGE_ORACLE_SUMMARY_V1"
                     " status=fail error=unknown_exception\n";
        return 1;
    }
}
