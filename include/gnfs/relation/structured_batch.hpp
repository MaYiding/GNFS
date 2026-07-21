#pragma once

#include "structured_reduction.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace gnfs::relation {

/// Identity of one immutable structured-incidence snapshot.
struct StructuredIncidenceSnapshotId final {
    uint64_t generation = 0;
    uint64_t incidence_epoch = 0;

    [[nodiscard]] bool operator==(const StructuredIncidenceSnapshotId&) const noexcept = default;
};

using StructuredBatchCandidate = std::variant<TwoWayMergePlan, TreeBasisMergePlan>;

/// Deterministic, conflict-free plan over one immutable incidence snapshot.
///
/// `candidates` owns the selected plans in their frozen global order. Duplicate
/// input transformations are removed before conflicts are evaluated. The
/// result is greedy maximal when the capacity is not reached, and only
/// capacity-maximal otherwise; it is not a maximum-cardinality solution.
///
/// This is a vector-backed planning result. Selection first constructs and
/// validates every candidate. It does not bound planning memory and makes no
/// budget, persistence, parallel-execution, materialization, or commit
/// guarantee. Any incidence mutation makes the entire snapshot stale.
struct StructuredConflictFreeBatchPlan final {
    StructuredIncidenceSnapshotId snapshot;
    std::vector<StructuredBatchCandidate> candidates;
    size_t raw_candidate_count = 0;
    size_t duplicate_candidate_count = 0;
    /// Conflicts encountered while scanning before the width cap is reached.
    size_t conflict_deferred_count = 0;
    /// Every unique candidate left unexamined after the width cap is reached.
    size_t capacity_deferred_count = 0;

    [[nodiscard]] bool operator==(const StructuredConflictFreeBatchPlan&) const noexcept = default;
};

/// One candidate whose exact plan was valid but whose materialized payload
/// exceeded the shared persistence format boundary. The candidate is retained
/// so the later scheduler can fold cache/statistics updates in slot order.
struct StructuredBatchPersistenceLimit final {
    StructuredBatchCandidate candidate;
};

using StructuredBatchPrepareOutcome =
    std::variant<PreparedTwoWayMerge, PreparedTreeBasisMerge, StructuredBatchPersistenceLimit>;

/// Opaque output of the M3a.2 prepare barrier.
///
/// Outcomes are in the exact selected-candidate order. The object deliberately
/// exposes only a const view: publishing one prepared slot through the legacy
/// single-candidate commit API would stale every remaining slot. M3b will add
/// the sole atomic batch consumer.
class StructuredPreparedBatch final {
public:
    StructuredPreparedBatch(const StructuredPreparedBatch&) = delete;
    StructuredPreparedBatch& operator=(const StructuredPreparedBatch&) = delete;
    StructuredPreparedBatch(StructuredPreparedBatch&&) noexcept = default;
    StructuredPreparedBatch& operator=(StructuredPreparedBatch&&) noexcept = default;

    [[nodiscard]] StructuredIncidenceSnapshotId snapshot() const noexcept;
    [[nodiscard]] std::span<const StructuredBatchPrepareOutcome> outcomes() const& noexcept;
    [[nodiscard]] std::span<const StructuredBatchPrepareOutcome> outcomes() const&& = delete;
    [[nodiscard]] size_t prepared_candidate_count() const noexcept;
    [[nodiscard]] size_t persistence_limited_candidate_count() const noexcept;

private:
    friend StructuredPreparedBatch
    prepare_conflict_free_batch(const SequentialStructuredReducer& reducer,
                                const StructuredConflictFreeBatchPlan& batch,
                                uint32_t worker_count);

    StructuredPreparedBatch(StructuredIncidenceSnapshotId snapshot,
                            std::vector<StructuredBatchPrepareOutcome> outcomes) noexcept;

    StructuredIncidenceSnapshotId snapshot_;
    std::vector<StructuredBatchPrepareOutcome> outcomes_;
    size_t prepared_candidate_count_ = 0;
    size_t persistence_limited_candidate_count_ = 0;
};

/// Validate snapshot-bound scheduling metadata, canonicalize, de-duplicate,
/// globally order, and greedily select a conflict-free batch. Exact reducer
/// state and corpus validation remain mandatory before preparation or commit.
/// Input order has no effect on the result.
[[nodiscard]] StructuredConflictFreeBatchPlan
select_conflict_free_batch(StructuredIncidenceSnapshotId snapshot, size_t total_row_count,
                           std::vector<StructuredBatchCandidate> candidates,
                           size_t max_batch_candidates);

/// Construct all current 2-way and tree-basis plans, then select a deterministic
/// conflict-free batch. This function is read-only and does not prepare or
/// commit any candidate.
[[nodiscard]] StructuredConflictFreeBatchPlan
plan_conflict_free_batch(const SequentialStructuredReducer& reducer, size_t max_batch_candidates,
                         TreeBasisPlanner planner = TreeBasisPlanner::DeterministicMst);

/// Validate one selected conflict-free batch against the current immutable
/// reducer snapshot, then materialize its candidates through a drain-all
/// parallel barrier. `worker_count` must be nonzero; the effective pool size is
/// bounded by the number of candidates and an empty batch creates no pool.
///
/// PersistenceLimit is retained as an ordered per-slot outcome. Every other
/// exception is rethrown only after all successfully submitted work completes;
/// if multiple indexed candidates fail, the lowest candidate index wins. This
/// function never commits, peels, updates statistics, or mutates the reducer.
/// The caller must exclude concurrent reducer mutation for the entire call.
[[nodiscard]] StructuredPreparedBatch
prepare_conflict_free_batch(const SequentialStructuredReducer& reducer,
                            const StructuredConflictFreeBatchPlan& batch, uint32_t worker_count);

} // namespace gnfs::relation
