#pragma once

#include "structured_reduction.hpp"

#include <cstddef>
#include <cstdint>
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
    size_t conflict_deferred_count = 0;
    size_t capacity_deferred_count = 0;

    [[nodiscard]] bool operator==(const StructuredConflictFreeBatchPlan&) const noexcept = default;
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

} // namespace gnfs::relation
