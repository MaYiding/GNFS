#pragma once

#include "../core/relation.hpp"
#include "large_prime_key.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace gnfs::relation {

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

/// Immutable, vector-backed source corpus for the M2 sequential reference.
///
/// Each corpus row is one provenance atom and receives exactly one SourceId.
/// A row may already contain extra_ab_pairs; materialization expands its primary
/// pair followed by those extras, but the extras do not receive new source IDs.
/// Equal AB pairs from different source payloads remain separate contributions.
///
/// This type is deliberately not an OOC abstraction. Source IDs are assigned
/// from stable vector order. Raw callers de-duplicate before construction;
/// SourceCorpus itself never de-duplicates because merged atoms may share a
/// primary pair while carrying different provenance and payload.
class SourceCorpus final {
public:
    SourceCorpus(uint64_t generation, std::vector<core::Relation> relations);

    SourceCorpus(const SourceCorpus&) = delete;
    SourceCorpus& operator=(const SourceCorpus&) = delete;
    SourceCorpus(SourceCorpus&&) noexcept = default;
    SourceCorpus& operator=(SourceCorpus&&) noexcept = default;

    [[nodiscard]] uint64_t generation() const noexcept;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] SourceId source_id(size_t ordinal) const;
    [[nodiscard]] const core::Relation& at(SourceId source) const;
    [[nodiscard]] core::Relation materialize(const SourceCombination& combination) const;

private:
    uint64_t generation_ = 0;
    std::vector<core::Relation> relations_;
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

    PreparedTreeBasisMerge(TreeBasisMergePlan plan,
                           std::vector<core::Relation> materialized) noexcept;

    TreeBasisMergePlan plan_;
    std::vector<core::Relation> materialized_;
};

enum class StructuredReductionStopReason {
    NotStarted,
    NoCandidates,
    PersistenceLimit,
};

struct StructuredReductionStats final {
    size_t input_rows = 0;
    size_t singleton_rows_removed = 0;
    size_t two_way_merges = 0;
    size_t tree_basis_batches = 0;
    size_t tree_basis_rows_consumed = 0;
    size_t tree_basis_rows_emitted = 0;
    size_t persistence_limited_plans = 0;
    size_t output_rows = 0;
    StructuredReductionStopReason stop_reason = StructuredReductionStopReason::NotStarted;
};

/// Deterministic vector-backed sequential reference over the LP incidence matrix.
///
/// Logical rows retain only exact source and LP symmetric differences. Plans
/// are read-only, preparation validates and materializes without mutation, and
/// commit publishes only after every potentially throwing allocation succeeds.
/// Active SourceCombination rows are GF(2) transform vectors over the immutable
/// corpus and must have full row rank. Source IDs may overlap between active
/// rows; overlap alone is not an invariant violation.
/// Parallel batches and OOC persistence are later milestones with additional
/// planning and storage requirements.
class SequentialStructuredReducer final {
public:
    explicit SequentialStructuredReducer(SourceCorpus corpus);
    SequentialStructuredReducer(uint64_t generation, std::vector<core::Relation> relations);
    ~SequentialStructuredReducer();

    SequentialStructuredReducer(const SequentialStructuredReducer&) = delete;
    SequentialStructuredReducer& operator=(const SequentialStructuredReducer&) = delete;
    SequentialStructuredReducer(SequentialStructuredReducer&&) noexcept;
    SequentialStructuredReducer& operator=(SequentialStructuredReducer&&) noexcept;

    [[nodiscard]] const SourceCorpus& corpus() const noexcept;
    [[nodiscard]] size_t total_row_count() const noexcept;
    [[nodiscard]] size_t active_row_count() const noexcept;
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

    [[nodiscard]] core::Relation materialize(StructuredRowId row) const;
    /// Materialize active rows in the exact order returned by active_row_ids().
    /// The vector-only M2 reference does not persist SourceCombination metadata;
    /// OOC provenance and recovery remain a later promotion gate.
    [[nodiscard]] std::vector<core::Relation> materialize_active() const;
    [[nodiscard]] const StructuredReductionStats& stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gnfs::relation
