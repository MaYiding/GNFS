#pragma once

#include "../core/relation.hpp"
#include "large_prime_key.hpp"
#include "relation_corpus.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace gnfs::relation {

class SequentialStructuredReducer;
class RelationSink;
struct StructuredIncidenceBuildOptions;
struct StructuredIncidenceBuildStats;
struct StructuredConflictFreeBatchPlan;
class StructuredPreparedBatch;
struct StructuredBatchCommitResult;

[[nodiscard]] StructuredPreparedBatch
prepare_conflict_free_batch(const SequentialStructuredReducer& reducer,
                            const StructuredConflictFreeBatchPlan& batch, uint32_t worker_count);

enum class StructuredReductionErrorCode {
    InvalidGeneration,
    InvalidSourceCombination,
    InvalidInput,
    ExponentOverflow,
    PersistenceLimit,
    ResourceLimit,
    InvalidPlan,
    StalePlan,
    InvariantViolation,
};

class StructuredReductionError final : public std::runtime_error {
public:
    StructuredReductionError(StructuredReductionErrorCode code, std::string message);

    [[nodiscard]] StructuredReductionErrorCode code() const noexcept;

private:
    StructuredReductionErrorCode code_;
};

/// Immutable identity of one relation in a validated raw corpus.
struct SourceId final {
    uint64_t generation = 0;
    uint64_t ordinal = 0;

    [[nodiscard]] constexpr bool operator==(const SourceId&) const noexcept = default;
    [[nodiscard]] constexpr bool operator<(const SourceId& other) const noexcept {
        if (generation != other.generation) {
            return generation < other.generation;
        }
        return ordinal < other.ordinal;
    }
};

/// Canonical GF(2) combination of immutable source relations.
///
/// IDs are strictly increasing and belong to one nonzero generation. The empty
/// combination represents the GF(2) zero only; logical rows reject it.
class SourceCombination final {
public:
    SourceCombination() = default;

    [[nodiscard]] static SourceCombination canonical(uint64_t generation,
                                                     std::vector<SourceId> sources);
    [[nodiscard]] static SourceCombination singleton(SourceId source);
    [[nodiscard]] static SourceCombination symmetric_difference(const SourceCombination& lhs,
                                                                const SourceCombination& rhs);

    [[nodiscard]] uint64_t generation() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] std::span<const SourceId> sources() const noexcept;
    [[nodiscard]] bool operator==(const SourceCombination&) const noexcept = default;

private:
    SourceCombination(uint64_t generation, std::vector<SourceId> sources) noexcept;

    uint64_t generation_ = 0;
    std::vector<SourceId> sources_;
};

/// Immutable owning source corpus for structured reduction.
///
/// Each corpus row is one provenance atom and receives exactly one SourceId.
/// A row may already contain extra_ab_pairs; materialization expands its primary
/// pair followed by those extras, but the extras do not receive new source IDs.
/// Equal AB pairs from different source payloads remain separate contributions.
///
/// The underlying RelationCorpus may own either a vector or a finalized OOC
/// store. Source IDs are assigned from its stable ordinal order. Raw callers
/// de-duplicate before construction; SourceCorpus itself never de-duplicates
/// because merged atoms may share a primary pair while carrying different
/// provenance and payload.
class SourceCorpus final {
public:
    explicit SourceCorpus(RelationCorpus corpus);
    SourceCorpus(uint64_t generation, std::vector<core::Relation> relations);

    SourceCorpus(const SourceCorpus&) = delete;
    SourceCorpus& operator=(const SourceCorpus&) = delete;
    SourceCorpus(SourceCorpus&&) noexcept = default;
    SourceCorpus& operator=(SourceCorpus&&) noexcept = default;

    [[nodiscard]] uint64_t generation() const noexcept;
    [[nodiscard]] size_t size() const;
    [[nodiscard]] SourceId source_id(size_t ordinal) const;
    /// Borrow an in-memory atom, or return nullptr for an OOC corpus.
    /// The pointer is tied to this SourceCorpus lifetime.
    [[nodiscard]] const core::Relation* try_borrow(SourceId source) const;
    [[nodiscard]] core::Relation at(SourceId source) const;
    [[nodiscard]] core::Relation materialize(const SourceCombination& combination) const;

private:
    uint64_t generation_ = 0;
    RelationCorpus corpus_;
};

struct StructuredRowId final {
    uint64_t value = 0;

    [[nodiscard]] constexpr bool operator==(const StructuredRowId&) const noexcept = default;
    [[nodiscard]] constexpr bool operator<(const StructuredRowId& other) const noexcept {
        return value < other.value;
    }
};

struct TwoWayMergePlan final {
    uint64_t generation = 0;
    uint64_t incidence_epoch = 0;
    std::array<StructuredRowId, 2> members{};
    LargePrimeKey witness{};
    SourceCombination expected_sources{};
    std::vector<LargePrimeKey> expected_lp_keys;

    [[nodiscard]] bool operator==(const TwoWayMergePlan&) const noexcept = default;
};

class PreparedTwoWayMerge final {
public:
    PreparedTwoWayMerge(const PreparedTwoWayMerge&) = delete;
    PreparedTwoWayMerge& operator=(const PreparedTwoWayMerge&) = delete;
    PreparedTwoWayMerge(PreparedTwoWayMerge&&) noexcept = default;
    PreparedTwoWayMerge& operator=(PreparedTwoWayMerge&&) noexcept = default;

    [[nodiscard]] const TwoWayMergePlan& plan() const noexcept;
    [[nodiscard]] const core::Relation& materialized_relation() const noexcept;

private:
    friend class SequentialStructuredReducer;
    friend StructuredPreparedBatch
    prepare_conflict_free_batch(const SequentialStructuredReducer& reducer,
                                const StructuredConflictFreeBatchPlan& batch,
                                uint32_t worker_count);

    PreparedTwoWayMerge(TwoWayMergePlan plan, core::Relation materialized) noexcept;

    TwoWayMergePlan plan_;
    core::Relation materialized_;
};

enum class TreeBasisPlanner {
    ReferenceStar,
    DeterministicMst,
};

struct TreeBasisEdgePlan final {
    std::array<StructuredRowId, 2> endpoints{};
    SourceCombination expected_sources{};
    std::vector<LargePrimeKey> expected_lp_keys;

    [[nodiscard]] bool operator==(const TreeBasisEdgePlan&) const noexcept = default;
};

struct TreeBasisMergePlan final {
    uint64_t generation = 0;
    uint64_t incidence_epoch = 0;
    TreeBasisPlanner planner = TreeBasisPlanner::DeterministicMst;
    LargePrimeKey pivot{};
    std::vector<StructuredRowId> members;
    std::vector<TreeBasisEdgePlan> edges;
    size_t input_nonpivot_lp_nnz = 0;
    size_t output_lp_nnz = 0;
    size_t lp_fill_growth = 0;

    [[nodiscard]] bool operator==(const TreeBasisMergePlan&) const noexcept = default;
};

class PreparedTreeBasisMerge final {
public:
    PreparedTreeBasisMerge(const PreparedTreeBasisMerge&) = delete;
    PreparedTreeBasisMerge& operator=(const PreparedTreeBasisMerge&) = delete;
    PreparedTreeBasisMerge(PreparedTreeBasisMerge&&) noexcept = default;
    PreparedTreeBasisMerge& operator=(PreparedTreeBasisMerge&&) noexcept = default;

    [[nodiscard]] const TreeBasisMergePlan& plan() const noexcept;
    [[nodiscard]] std::span<const core::Relation> materialized_relations() const noexcept;

private:
    friend class SequentialStructuredReducer;
    friend StructuredPreparedBatch
    prepare_conflict_free_batch(const SequentialStructuredReducer& reducer,
                                const StructuredConflictFreeBatchPlan& batch,
                                uint32_t worker_count);

    PreparedTreeBasisMerge(TreeBasisMergePlan plan,
                           std::vector<core::Relation> materialized) noexcept;

    TreeBasisMergePlan plan_;
    std::vector<core::Relation> materialized_;
};

/// Per-invocation policy limits for the vector-backed sequential reference.
///
/// The constructor requires the limits that have no evidence-based project
/// default. Other fields default to the current relation-format boundary or
/// the exact weight-eight structural maximum and may be tightened by callers.
struct StructuredReductionBudget final {
    StructuredReductionBudget(size_t max_candidate_examinations_per_pass, size_t max_emitted_rows,
                              size_t max_total_lp_fill_growth,
                              size_t max_accepted_materialized_payload_entries_per_commit) noexcept;

    size_t max_candidate_examinations_per_pass;
    size_t max_emitted_rows;
    size_t max_total_lp_fill_growth;
    /// Post-prepare policy acceptance bound over the five persisted payload
    /// categories, counting the primary AB pair. This is not an allocation,
    /// resident-memory, or peak-RSS bound.
    size_t max_accepted_materialized_payload_entries_per_commit;
    /// Defaults to max_emitted_rows because every commit emits at least one
    /// row. Callers may tighten it independently.
    size_t max_commits;

    size_t max_pivot_weight = 8;
    size_t max_source_atoms_per_output =
        static_cast<size_t>(core::Relation::MAX_SERIALIZED_EXTRA_AB_PAIRS) + 1;
    size_t max_odd_lp_keys_per_output =
        static_cast<size_t>(core::Relation::MAX_SERIALIZED_LARGE_PRIMES) * 2;
    size_t max_output_lp_nnz_per_commit =
        7 * static_cast<size_t>(core::Relation::MAX_SERIALIZED_LARGE_PRIMES) * 2;
    size_t max_materialized_pairs_per_output =
        static_cast<size_t>(core::Relation::MAX_SERIALIZED_EXTRA_AB_PAIRS) + 1;
    size_t max_factor_entries_per_side = core::Relation::MAX_SERIALIZED_FACTORS;
    size_t max_persisted_lp_entries_per_side = core::Relation::MAX_SERIALIZED_LARGE_PRIMES;
};

/// Validate every format-bound field of a structured reduction budget.
///
/// Callers that own a move-only source snapshot use this preflight before
/// transferring the corpus into a reducer. The reducer repeats the same check
/// defensively at execution time.
void validate_structured_reduction_budget(const StructuredReductionBudget& budget);

enum class StructuredReductionStopReason {
    NotStarted,
    NoCandidates,
    BudgetLimit,
    PersistenceLimit,
};

struct StructuredReductionRejectionStats final {
    size_t pivot_weight_limit = 0;
    size_t source_limit = 0;
    size_t output_lp_limit = 0;
    size_t fill_limit = 0;
    size_t emitted_row_limit = 0;
    size_t materialization_limit = 0;
};

struct StructuredReductionStats final {
    size_t input_rows = 0;
    size_t singleton_rows_removed = 0;
    size_t two_way_merges = 0;
    size_t tree_basis_batches = 0;
    size_t tree_basis_rows_consumed = 0;
    size_t tree_basis_rows_emitted = 0;
    size_t persistence_limited_plans = 0;
    size_t persistence_cache_hits = 0;
    size_t budgeted_runs = 0;
    size_t planning_passes = 0;
    size_t candidate_plans_considered = 0;
    size_t budget_limited_plans = 0;
    size_t candidate_limit_stops = 0;
    size_t commit_limit_stops = 0;
    size_t budget_limit_stops = 0;
    size_t peak_prepared_payload_entries = 0;
    size_t accepted_lp_fill_growth = 0;
    StructuredReductionRejectionStats budget_rejections;
    size_t output_rows = 0;
    StructuredReductionStopReason stop_reason = StructuredReductionStopReason::NotStarted;
};

struct StructuredReductionRunResult final {
    size_t singleton_rows_removed = 0;
    size_t commits = 0;
    size_t emitted_rows = 0;
    size_t lp_fill_growth = 0;
    StructuredReductionStopReason stop_reason = StructuredReductionStopReason::NotStarted;

    [[nodiscard]] bool operator==(const StructuredReductionRunResult&) const noexcept = default;
};

/// Execution shape for the vector-backed deterministic parallel scheduler.
/// Both fields must be nonzero. Batch width bounds selected candidates, while
/// worker count bounds concurrent candidate materialization.
struct StructuredParallelReductionOptions final {
    size_t max_batch_candidates = 1;
    uint32_t worker_count = 1;

    [[nodiscard]] bool
    operator==(const StructuredParallelReductionOptions&) const noexcept = default;
};

/// Deterministic vector-backed sequential reference over the LP incidence matrix.
///
/// Logical rows retain only exact source and LP symmetric differences. Plans
/// are read-only, preparation validates and materializes without mutation, and
/// commit publishes only after every potentially throwing allocation succeeds.
/// Active SourceCombination rows are GF(2) transform vectors over the immutable
/// corpus and must have full row rank. Source IDs may overlap between active
/// rows; overlap alone is not an invariant violation.
/// Initial incidence construction uses bounded transient row shards. Parallel
/// batches still retain the vector-backed corpus, logical rows, final bucket
/// adjacency, and append-only incidence history. Bounded-memory OOC execution
/// remains a later milestone.
class SequentialStructuredReducer final {
public:
    explicit SequentialStructuredReducer(SourceCorpus corpus);
    SequentialStructuredReducer(SourceCorpus corpus,
                                const StructuredIncidenceBuildOptions& build_options);
    SequentialStructuredReducer(uint64_t generation, std::vector<core::Relation> relations);
    SequentialStructuredReducer(uint64_t generation, std::vector<core::Relation> relations,
                                const StructuredIncidenceBuildOptions& build_options);
    ~SequentialStructuredReducer();

    SequentialStructuredReducer(const SequentialStructuredReducer&) = delete;
    SequentialStructuredReducer& operator=(const SequentialStructuredReducer&) = delete;
    SequentialStructuredReducer(SequentialStructuredReducer&&) noexcept;
    SequentialStructuredReducer& operator=(SequentialStructuredReducer&&) noexcept;

    [[nodiscard]] const SourceCorpus& corpus() const noexcept;
    /// Read-only identity of the current incidence snapshot. Any logical
    /// incidence mutation advances this epoch and invalidates older plans.
    [[nodiscard]] uint64_t incidence_epoch() const noexcept;
    [[nodiscard]] size_t total_row_count() const noexcept;
    [[nodiscard]] size_t active_row_count() const noexcept;
    /// Number of distinct odd large-prime columns referenced by active rows.
    /// This scans existing bucket metadata without building a duplicate set.
    [[nodiscard]] size_t active_lp_column_count() const noexcept;
    [[nodiscard]] bool is_active(StructuredRowId row) const;
    /// Borrowed views remain valid only until the next mutating reducer call.
    [[nodiscard]] const SourceCombination& sources(StructuredRowId row) const;
    [[nodiscard]] std::span<const LargePrimeKey> lp_keys(StructuredRowId row) const;
    [[nodiscard]] std::vector<StructuredRowId> active_row_ids() const;

    size_t peel_singletons();
    [[nodiscard]] std::vector<TwoWayMergePlan> plan_two_way_merges() const;
    [[nodiscard]] PreparedTwoWayMerge prepare(const TwoWayMergePlan& plan) const;
    [[nodiscard]] StructuredRowId commit(PreparedTwoWayMerge&& prepared);
    void reduce_two_way();

    [[nodiscard]] std::vector<TreeBasisMergePlan>
    plan_tree_basis_merges(TreeBasisPlanner planner = TreeBasisPlanner::DeterministicMst) const;
    [[nodiscard]] PreparedTreeBasisMerge prepare(const TreeBasisMergePlan& plan) const;
    [[nodiscard]] std::vector<StructuredRowId> commit(PreparedTreeBasisMerge&& prepared);

    /// Atomically publish every prepared success slot in candidate order.
    /// Persistence-limit slots remain unpublished. The batch advances the
    /// incidence epoch exactly once when at least one candidate is committed
    /// and never performs singleton peeling or scheduler-level accounting.
    /// The handle is consumed by value. Any failure before the no-throw publish
    /// boundary leaves the reducer's logical state unchanged.
    [[nodiscard]] StructuredBatchCommitResult commit(StructuredPreparedBatch prepared);

    /// Run deterministic singleton, 2-way, and tree-basis reduction under
    /// explicit examination, output, fill, and materialization limits.
    ///
    /// This method bounds candidate examinations and newly prepared/committed
    /// output for one invocation. The current reference still constructs every
    /// candidate plan for an epoch and retains the corpus, tombstones, and
    /// incidence history in memory. It is not a bounded-memory, streaming, OOC,
    /// or parallel reducer.
    ///
    /// Metadata policy rejection order is pivot weight, source atoms per
    /// output, odd LP keys per output, output LP NNZ per commit, cumulative LP
    /// fill growth, then cumulative emitted rows. Policy-admissible persistence
    /// cache hits do not consume the candidate-examination limit. Preparation
    /// then checks each materialized output's AB-pair, factor-side, and
    /// persisted-LP-side limits before applying the accepted whole-commit
    /// payload bound.
    ///
    /// A pass with no raw candidates stops with NoCandidates. With raw
    /// candidates remaining, a reached commit or candidate-examination limit
    /// takes precedence and stops with BudgetLimit. Otherwise an intrinsic
    /// PersistenceLimit from any policy-admissible plan takes precedence over
    /// policy rejections; a pass containing only policy rejections stops with
    /// BudgetLimit.
    ///
    /// The invocation is commit-granular, not transactional as a whole:
    /// singleton peels and completed commits remain published if a later pass
    /// stops or throws. Each individual prepared commit retains its existing
    /// all-or-nothing mutation contract.
    [[nodiscard]] StructuredReductionRunResult
    reduce_budgeted(const StructuredReductionBudget& budget,
                    TreeBasisPlanner planner = TreeBasisPlanner::DeterministicMst);

    /// Run the same reduction policy through deterministic conflict-free
    /// batches and a drain-all parallel preparation barrier. Scheduler and
    /// persistence accounting are folded in slot order only after an atomic
    /// batch publication succeeds. The sequential driver remains the reference
    /// oracle; this first parallel scheduler is still vector-backed.
    [[nodiscard]] StructuredReductionRunResult
    reduce_budgeted_parallel(const StructuredReductionBudget& budget,
                             const StructuredParallelReductionOptions& options,
                             TreeBasisPlanner planner = TreeBasisPlanner::DeterministicMst);

    [[nodiscard]] core::Relation materialize(StructuredRowId row) const;
    /// Materialize active rows in the exact order returned by active_row_ids().
    /// This compatibility API collects every result in memory; use
    /// materialize_active_to() for bounded-memory output publication.
    [[nodiscard]] std::vector<core::Relation> materialize_active() const;
    /// Materialize and append active rows one at a time in the same deterministic
    /// order as materialize_active(), without constructing an active-row vector.
    /// The optional observer runs before each append, allowing validation and
    /// metrics to finish before the sink is finalized. Any materialization,
    /// observer, or append failure aborts the sink transaction.
    [[nodiscard]] size_t
    materialize_active_to(RelationSink& sink,
                          const std::function<void(const core::Relation&)>& observer = {}) const;
    [[nodiscard]] const StructuredReductionStats& stats() const noexcept;
    /// Resource evidence snapshot for immutable initial-incidence construction.
    [[nodiscard]] StructuredIncidenceBuildStats incidence_build_stats() const noexcept;

private:
    friend StructuredPreparedBatch
    prepare_conflict_free_batch(const SequentialStructuredReducer& reducer,
                                const StructuredConflictFreeBatchPlan& batch,
                                uint32_t worker_count);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#if defined(GNFS_STRUCTURED_REDUCTION_TEST_HOOKS)
/// Test-only deterministic events for exercising parallel publication boundaries.
///
/// This API and every corresponding call site are compiled out unless the
/// dedicated test macro is enabled. Prepare-slot callbacks may run concurrently
/// and therefore must synchronize any shared observation state themselves.
namespace structured_reduction_testing {

enum class Event : uint8_t {
    ParallelPrepareSlotStarted,
    ParallelPrepareSlotCompleted,
    MaskedBatchCommitBeforePublish,
    ParallelDriverBeforePostCommitPeel,
};

inline constexpr size_t no_slot = static_cast<size_t>(-1);

using HookCallback = void (*)(Event event, size_t slot, void* context);

struct Hook final {
    HookCallback callback = nullptr;
    void* context = nullptr;
};

[[nodiscard]] StructuredReductionRunResult reduce_budgeted_parallel_with_hook(
    SequentialStructuredReducer& reducer, const StructuredReductionBudget& budget,
    const StructuredParallelReductionOptions& options, const Hook& hook,
    TreeBasisPlanner planner = TreeBasisPlanner::DeterministicMst);

[[nodiscard]] StructuredBatchCommitResult
commit_prepared_batch_with_hook(SequentialStructuredReducer& reducer,
                                StructuredPreparedBatch prepared, const Hook& hook);

} // namespace structured_reduction_testing
#endif

} // namespace gnfs::relation
