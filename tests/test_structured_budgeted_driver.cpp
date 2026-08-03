#include "gnfs/relation/structured_reduction.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

using gnfs::core::Relation;
using gnfs::relation::LargePrimeKey;
using gnfs::relation::SequentialStructuredReducer;
using gnfs::relation::SourceCombination;
using gnfs::relation::SourceCorpus;
using gnfs::relation::SourceId;
using gnfs::relation::StructuredReductionBudget;
using gnfs::relation::StructuredReductionError;
using gnfs::relation::StructuredReductionErrorCode;
using gnfs::relation::StructuredReductionRejectionStats;
using gnfs::relation::StructuredReductionRunResult;
using gnfs::relation::StructuredReductionStats;
using gnfs::relation::StructuredReductionStopReason;
using gnfs::relation::StructuredRowId;
using gnfs::relation::TreeBasisPlanner;

namespace {

using Mask = uint64_t;

constexpr size_t kMaxOracleRows = 16;
constexpr size_t kFactorColumns = 8;
constexpr size_t kMaskBits = std::numeric_limits<Mask>::digits;

int checks = 0;
int failures = 0;
std::string_view current_case = "startup";

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(condition)) {                                                                        \
            ++failures;                                                                            \
            std::cerr << "CHECK failed [" << current_case << "] at " << __FILE__ << ':'            \
                      << __LINE__ << ": " << #condition << '\n';                                   \
        }                                                                                          \
    } while (false)

void oracle_require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

[[nodiscard]] constexpr LargePrimeKey rational_key(uint64_t prime) noexcept {
    return LargePrimeKey{prime, 0, false};
}

void add_large_prime(Relation& relation, const LargePrimeKey& key) {
    oracle_require(!key.is_algebraic, "budget fixture unexpectedly uses an algebraic LP");
    relation.rational_large_prime.emplace_back(key.prime, uint8_t{1});
}

[[nodiscard]] Relation make_relation(int64_t a, std::initializer_list<LargePrimeKey> lp_keys,
                                     std::initializer_list<uint32_t> factors = {}) {
    Relation relation(a, 1);
    relation.rational_factors.assign(factors.begin(), factors.end());
    for (const auto& key : lp_keys) {
        add_large_prime(relation, key);
    }
    return relation;
}

[[nodiscard]] bool relation_equal(const Relation& lhs, const Relation& rhs) {
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.rational_factors == rhs.rational_factors &&
           lhs.algebraic_factors == rhs.algebraic_factors &&
           lhs.rational_large_prime == rhs.rational_large_prime &&
           lhs.algebraic_large_prime == rhs.algebraic_large_prime &&
           lhs.extra_ab_pairs == rhs.extra_ab_pairs;
}

[[nodiscard]] bool corpus_equal(std::span<const Relation> lhs, std::span<const Relation> rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!relation_equal(lhs[i], rhs[i])) {
            return false;
        }
    }
    return true;
}

/// Independent parity fold: do not share odd_large_prime_keys() with the reducer.
[[nodiscard]] std::vector<LargePrimeKey> exact_lp_support(const Relation& relation) {
    std::vector<LargePrimeKey> contributions;
    for (const auto& prime_power : relation.rational_large_prime) {
        if ((prime_power.e & 1U) != 0U) {
            contributions.push_back(rational_key(prime_power.p));
        }
    }
    for (const auto& prime_power : relation.algebraic_large_prime) {
        if ((prime_power.e & 1U) != 0U) {
            contributions.push_back(LargePrimeKey{prime_power.p, prime_power.r, true});
        }
    }
    std::sort(contributions.begin(), contributions.end());

    std::vector<LargePrimeKey> support;
    for (size_t begin = 0; begin < contributions.size();) {
        size_t end = begin + 1;
        while (end < contributions.size() && contributions[end] == contributions[begin]) {
            ++end;
        }
        if (((end - begin) & 1U) != 0U) {
            support.push_back(contributions[begin]);
        }
        begin = end;
    }
    return support;
}

struct SemanticUniverse final {
    std::vector<LargePrimeKey> lp_keys;
};

[[nodiscard]] SemanticUniverse build_universe(const SourceCorpus& corpus) {
    SemanticUniverse universe;
    for (size_t ordinal = 0; ordinal < corpus.size(); ++ordinal) {
        const auto support = exact_lp_support(corpus.at(corpus.source_id(ordinal)));
        universe.lp_keys.insert(universe.lp_keys.end(), support.begin(), support.end());
    }
    std::sort(universe.lp_keys.begin(), universe.lp_keys.end());
    universe.lp_keys.erase(std::unique(universe.lp_keys.begin(), universe.lp_keys.end()),
                           universe.lp_keys.end());
    oracle_require(universe.lp_keys.size() <= kMaskBits - kFactorColumns,
                   "semantic universe exceeds oracle mask width");
    return universe;
}

[[nodiscard]] Mask semantic_row(const Relation& relation, const SemanticUniverse& universe) {
    Mask result = 0;
    for (const uint32_t factor : relation.rational_factors) {
        oracle_require(factor < kFactorColumns, "factor exceeds oracle fixture width");
        result ^= Mask{1} << factor;
    }
    for (const auto& key : exact_lp_support(relation)) {
        const auto it = std::lower_bound(universe.lp_keys.begin(), universe.lp_keys.end(), key);
        oracle_require(it != universe.lp_keys.end() && *it == key,
                       "materialized LP is absent from the source universe");
        const size_t index = static_cast<size_t>(it - universe.lp_keys.begin());
        result ^= Mask{1} << (kFactorColumns + index);
    }
    return result;
}

[[nodiscard]] Mask source_mask(const SourceCombination& combination, const SourceCorpus& corpus) {
    oracle_require(combination.generation() == corpus.generation(),
                   "source combination generation mismatch");
    oracle_require(corpus.size() <= kMaxOracleRows, "source corpus exceeds oracle bound");
    Mask result = 0;
    SourceId previous{};
    bool have_previous = false;
    for (const SourceId source : combination.sources()) {
        oracle_require(source.generation == corpus.generation(), "source ID generation mismatch");
        oracle_require(source.ordinal < corpus.size(), "source ID is outside the corpus");
        oracle_require(!have_previous || previous < source, "source combination is not canonical");
        result |= Mask{1} << source.ordinal;
        previous = source;
        have_previous = true;
    }
    oracle_require(result != 0, "active source transform is empty");
    return result;
}

[[nodiscard]] Mask subset_count(size_t rows) {
    oracle_require(rows <= kMaxOracleRows && rows < kMaskBits,
                   "exhaustive oracle row bound exceeded");
    return Mask{1} << rows;
}

[[nodiscard]] Mask xor_selected(Mask selection, std::span<const Mask> rows) {
    Mask value = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (((selection >> i) & Mask{1}) != 0) {
            value ^= rows[i];
        }
    }
    return value;
}

[[nodiscard]] std::vector<Mask> exact_span(std::span<const Mask> rows) {
    std::vector<Mask> span;
    for (Mask selection = 0; selection < subset_count(rows.size()); ++selection) {
        span.push_back(xor_selected(selection, rows));
    }
    std::sort(span.begin(), span.end());
    span.erase(std::unique(span.begin(), span.end()), span.end());
    return span;
}

[[nodiscard]] std::vector<Mask> exact_left_kernel(std::span<const Mask> rows) {
    std::vector<Mask> kernel;
    for (Mask selection = 0; selection < subset_count(rows.size()); ++selection) {
        if (xor_selected(selection, rows) == 0) {
            kernel.push_back(selection);
        }
    }
    return kernel;
}

void check_exact_mapping(const SequentialStructuredReducer& reducer) {
    const auto& corpus = reducer.corpus();
    const auto active = reducer.active_row_ids();
    oracle_require(corpus.size() <= kMaxOracleRows && active.size() <= kMaxOracleRows,
                   "driver fixture exceeds exhaustive oracle bound");
    const auto universe = build_universe(corpus);

    std::vector<Mask> original_rows;
    for (size_t ordinal = 0; ordinal < corpus.size(); ++ordinal) {
        original_rows.push_back(semantic_row(corpus.at(corpus.source_id(ordinal)), universe));
    }

    std::vector<Mask> reduced_rows;
    std::vector<Mask> transforms;
    for (const auto row : active) {
        reduced_rows.push_back(semantic_row(reducer.materialize(row), universe));
        transforms.push_back(source_mask(reducer.sources(row), corpus));
    }
    CHECK(reduced_rows.size() == transforms.size());
    for (size_t i = 0; i < reduced_rows.size(); ++i) {
        CHECK(reduced_rows[i] == xor_selected(transforms[i], original_rows));
    }
    CHECK(exact_span(transforms).size() == static_cast<size_t>(subset_count(transforms.size())));

    std::vector<Mask> mapped_kernel;
    for (const Mask dependency : exact_left_kernel(reduced_rows)) {
        mapped_kernel.push_back(xor_selected(dependency, transforms));
    }
    std::sort(mapped_kernel.begin(), mapped_kernel.end());
    CHECK(std::adjacent_find(mapped_kernel.begin(), mapped_kernel.end()) == mapped_kernel.end());
    CHECK(mapped_kernel == exact_left_kernel(original_rows));
}

struct RejectionSnapshot final {
    size_t pivot_weight_limit = 0;
    size_t source_limit = 0;
    size_t output_lp_limit = 0;
    size_t fill_limit = 0;
    size_t emitted_row_limit = 0;
    size_t materialization_limit = 0;

    [[nodiscard]] bool operator==(const RejectionSnapshot&) const noexcept = default;
};

struct StatsSnapshot final {
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
    RejectionSnapshot budget_rejections;
    size_t output_rows = 0;
    StructuredReductionStopReason stop_reason = StructuredReductionStopReason::NotStarted;

    [[nodiscard]] bool operator==(const StatsSnapshot&) const noexcept = default;
};

[[nodiscard]] StatsSnapshot stats_snapshot(const SequentialStructuredReducer& reducer) {
    const StructuredReductionStats& stats = reducer.stats();
    const StructuredReductionRejectionStats& rejects = stats.budget_rejections;
    return StatsSnapshot{
        stats.input_rows,
        stats.singleton_rows_removed,
        stats.two_way_merges,
        stats.tree_basis_batches,
        stats.tree_basis_rows_consumed,
        stats.tree_basis_rows_emitted,
        stats.persistence_limited_plans,
        stats.persistence_cache_hits,
        stats.budgeted_runs,
        stats.planning_passes,
        stats.candidate_plans_considered,
        stats.budget_limited_plans,
        stats.candidate_limit_stops,
        stats.commit_limit_stops,
        stats.budget_limit_stops,
        stats.peak_prepared_payload_entries,
        stats.accepted_lp_fill_growth,
        RejectionSnapshot{rejects.pivot_weight_limit, rejects.source_limit, rejects.output_lp_limit,
                          rejects.fill_limit, rejects.emitted_row_limit,
                          rejects.materialization_limit},
        stats.output_rows,
        stats.stop_reason,
    };
}

struct LogicalSnapshot final {
    size_t total_rows = 0;
    size_t active_rows = 0;
    std::vector<StructuredRowId> active_ids;
    std::vector<Mask> source_masks;
    std::vector<std::vector<LargePrimeKey>> lp_keys;
    std::vector<Relation> materialized;
};

[[nodiscard]] LogicalSnapshot logical_snapshot(const SequentialStructuredReducer& reducer) {
    LogicalSnapshot result;
    result.total_rows = reducer.total_row_count();
    result.active_rows = reducer.active_row_count();
    result.active_ids = reducer.active_row_ids();
    for (const auto row : result.active_ids) {
        result.source_masks.push_back(source_mask(reducer.sources(row), reducer.corpus()));
        const auto keys = reducer.lp_keys(row);
        result.lp_keys.emplace_back(keys.begin(), keys.end());
    }
    result.materialized = reducer.materialize_active();
    return result;
}

[[nodiscard]] bool logical_equal(const LogicalSnapshot& lhs, const LogicalSnapshot& rhs) {
    return lhs.total_rows == rhs.total_rows && lhs.active_rows == rhs.active_rows &&
           lhs.active_ids == rhs.active_ids && lhs.source_masks == rhs.source_masks &&
           lhs.lp_keys == rhs.lp_keys && corpus_equal(lhs.materialized, rhs.materialized);
}

[[nodiscard]] StructuredReductionBudget generous_budget() {
    StructuredReductionBudget budget(64, 64, 64, 1024);
    budget.max_commits = 64;
    budget.max_pivot_weight = 8;
    budget.max_source_atoms_per_output = 16;
    budget.max_odd_lp_keys_per_output = 32;
    budget.max_output_lp_nnz_per_commit =
        7 * static_cast<size_t>(Relation::MAX_SERIALIZED_LARGE_PRIMES) * 2;
    budget.max_materialized_pairs_per_output = 32;
    budget.max_factor_entries_per_side = 32;
    budget.max_persisted_lp_entries_per_side = Relation::MAX_SERIALIZED_LARGE_PRIMES;
    return budget;
}

[[nodiscard]] bool all_zero(const RejectionSnapshot& stats) {
    return stats == RejectionSnapshot{};
}

void test_invalid_policy_is_rejected_before_mutation() {
    current_case = "F0 invalid policy";
    const auto x = rational_key(97);
    SequentialStructuredReducer reducer(30'000, {make_relation(1, {x}), make_relation(2, {})});
    const LogicalSnapshot before = logical_snapshot(reducer);
    const StatsSnapshot stats_before = stats_snapshot(reducer);

    auto invalid_budget = generous_budget();
    invalid_budget.max_pivot_weight = 9;
    bool rejected_budget = false;
    try {
        (void)reducer.reduce_budgeted(invalid_budget);
    } catch (const StructuredReductionError& error) {
        rejected_budget = error.code() == StructuredReductionErrorCode::InvalidInput;
    }
    CHECK(rejected_budget);
    CHECK(logical_equal(before, logical_snapshot(reducer)));
    CHECK(stats_snapshot(reducer) == stats_before);

    bool rejected_planner = false;
    try {
        (void)reducer.reduce_budgeted(generous_budget(), static_cast<TreeBasisPlanner>(255));
    } catch (const StructuredReductionError& error) {
        rejected_planner = error.code() == StructuredReductionErrorCode::InvalidPlan;
    }
    CHECK(rejected_planner);
    CHECK(logical_equal(before, logical_snapshot(reducer)));
    CHECK(stats_snapshot(reducer) == stats_before);
}

void test_no_candidates_after_initial_peel() {
    current_case = "F0 initial peel";
    const auto x = rational_key(101);
    SequentialStructuredReducer reducer(30'001, {make_relation(1, {x}), make_relation(2, {})});
    const StructuredReductionRunResult run = reducer.reduce_budgeted(generous_budget());

    CHECK((run ==
           StructuredReductionRunResult{1, 0, 0, 0, StructuredReductionStopReason::NoCandidates}));
    const auto stats = stats_snapshot(reducer);
    CHECK(stats.input_rows == 2);
    CHECK(stats.singleton_rows_removed == 1);
    CHECK(stats.two_way_merges == 0);
    CHECK(stats.tree_basis_batches == 0);
    CHECK(stats.budgeted_runs == 1);
    CHECK(stats.planning_passes == 1);
    CHECK(stats.candidate_plans_considered == 0);
    CHECK(stats.persistence_cache_hits == 0);
    CHECK(stats.commit_limit_stops == 0);
    CHECK(stats.output_rows == 1);
    CHECK(stats.stop_reason == StructuredReductionStopReason::NoCandidates);
    CHECK(all_zero(stats.budget_rejections));
    check_exact_mapping(reducer);
}

void test_after_commit_peel_and_known_kernel() {
    current_case = "F1 after-commit peel";
    const auto p = rational_key(211);
    const auto q = rational_key(223);
    SequentialStructuredReducer reducer(
        30'002, {make_relation(1, {p, q}), make_relation(2, {p, q}), make_relation(3, {q})});
    const StructuredReductionRunResult run = reducer.reduce_budgeted(generous_budget());

    CHECK((run ==
           StructuredReductionRunResult{1, 1, 1, 0, StructuredReductionStopReason::NoCandidates}));
    CHECK(reducer.total_row_count() == 4);
    CHECK(reducer.active_row_ids() == std::vector<StructuredRowId>({StructuredRowId{3}}));
    CHECK(source_mask(reducer.sources(StructuredRowId{3}), reducer.corpus()) == Mask{0b011});
    CHECK(reducer.lp_keys(StructuredRowId{3}).empty());
    const auto stats = stats_snapshot(reducer);
    CHECK(stats.singleton_rows_removed == 1);
    CHECK(stats.two_way_merges == 1);
    CHECK(stats.planning_passes == 2);
    CHECK(stats.candidate_plans_considered == 1);
    CHECK(stats.persistence_cache_hits == 0);
    CHECK(stats.commit_limit_stops == 0);
    CHECK(stats.peak_prepared_payload_entries == 4);
    CHECK(stats.output_rows == 1);
    CHECK(stats.stop_reason == StructuredReductionStopReason::NoCandidates);
    check_exact_mapping(reducer);
}

[[nodiscard]] std::vector<Relation> tree_budget_fixture(bool with_factors = false) {
    const auto p = rational_key(307);
    const auto a = rational_key(311);
    const auto b = rational_key(313);
    const auto c = rational_key(317);
    std::vector<Relation> relations;
    relations.push_back(with_factors ? make_relation(1, {p, a}, {0}) : make_relation(1, {p, a}));
    relations.push_back(with_factors ? make_relation(2, {p, b}, {1}) : make_relation(2, {p, b}));
    relations.push_back(with_factors ? make_relation(3, {p, c}, {2}) : make_relation(3, {p, c}));
    for (int64_t row = 0; row < 8; ++row) {
        relations.push_back(make_relation(10 + row, {a, b, c}));
    }
    return relations;
}

[[nodiscard]] size_t payload_entries(const Relation& relation) {
    return 1 + relation.extra_ab_pairs.size() + relation.rational_factors.size() +
           relation.algebraic_factors.size() + relation.rational_large_prime.size() +
           relation.algebraic_large_prime.size();
}

void check_tree_plan_metrics(SequentialStructuredReducer& reducer, size_t expected_payload,
                             bool with_factors) {
    CHECK(reducer.plan_two_way_merges().empty());
    const auto plans = reducer.plan_tree_basis_merges(TreeBasisPlanner::DeterministicMst);
    CHECK(plans.size() == 1);
    if (plans.size() != 1) {
        return;
    }
    const auto& plan = plans.front();
    CHECK(plan.members.size() == 3);
    CHECK(plan.edges.size() == 2);
    CHECK(plan.input_nonpivot_lp_nnz == 3);
    CHECK(plan.output_lp_nnz == 4);
    CHECK(plan.lp_fill_growth == 1);
    for (const auto& edge : plan.edges) {
        CHECK(edge.expected_sources.size() == 2);
        CHECK(edge.expected_lp_keys.size() == 2);
    }

    const auto prepared = reducer.prepare(plan);
    size_t batch_payload = 0;
    for (const auto& relation : prepared.materialized_relations()) {
        CHECK(1 + relation.extra_ab_pairs.size() == 2);
        CHECK(relation.rational_large_prime.size() == 3);
        CHECK(relation.algebraic_large_prime.empty());
        CHECK(relation.rational_factors.size() == (with_factors ? 2 : 0));
        batch_payload += payload_entries(relation);
    }
    CHECK(batch_payload == expected_payload);
}

enum class BoundaryKind {
    CandidateExaminations,
    CommitCount,
    EmittedRows,
    Fill,
    BatchPayload,
    PivotWeight,
    SourceAtoms,
    OddLpPerOutput,
    OutputLpPerCommit,
    MaterializedPairs,
    PersistedLpPerSide,
    FactorEntriesPerSide,
};

struct BoundaryCase final {
    std::string_view name;
    BoundaryKind kind;
    size_t accepted;
    size_t rejected;
    bool with_factors = false;
};

void set_boundary(StructuredReductionBudget& budget, BoundaryKind kind, size_t value) {
    switch (kind) {
    case BoundaryKind::CandidateExaminations:
        budget.max_candidate_examinations_per_pass = value;
        break;
    case BoundaryKind::CommitCount:
        budget.max_commits = value;
        break;
    case BoundaryKind::EmittedRows:
        budget.max_emitted_rows = value;
        break;
    case BoundaryKind::Fill:
        budget.max_total_lp_fill_growth = value;
        break;
    case BoundaryKind::BatchPayload:
        budget.max_accepted_materialized_payload_entries_per_commit = value;
        break;
    case BoundaryKind::PivotWeight:
        budget.max_pivot_weight = value;
        break;
    case BoundaryKind::SourceAtoms:
        budget.max_source_atoms_per_output = value;
        break;
    case BoundaryKind::OddLpPerOutput:
        budget.max_odd_lp_keys_per_output = value;
        break;
    case BoundaryKind::OutputLpPerCommit:
        budget.max_output_lp_nnz_per_commit = value;
        break;
    case BoundaryKind::MaterializedPairs:
        budget.max_materialized_pairs_per_output = value;
        break;
    case BoundaryKind::PersistedLpPerSide:
        budget.max_persisted_lp_entries_per_side = value;
        break;
    case BoundaryKind::FactorEntriesPerSide:
        budget.max_factor_entries_per_side = value;
        break;
    }
}

[[nodiscard]] RejectionSnapshot expected_rejection(BoundaryKind kind) {
    RejectionSnapshot result;
    switch (kind) {
    case BoundaryKind::CandidateExaminations:
    case BoundaryKind::CommitCount:
        break;
    case BoundaryKind::EmittedRows:
        result.emitted_row_limit = 1;
        break;
    case BoundaryKind::Fill:
        result.fill_limit = 1;
        break;
    case BoundaryKind::PivotWeight:
        result.pivot_weight_limit = 1;
        break;
    case BoundaryKind::SourceAtoms:
        result.source_limit = 1;
        break;
    case BoundaryKind::OddLpPerOutput:
    case BoundaryKind::OutputLpPerCommit:
        result.output_lp_limit = 1;
        break;
    case BoundaryKind::BatchPayload:
    case BoundaryKind::MaterializedPairs:
    case BoundaryKind::PersistedLpPerSide:
    case BoundaryKind::FactorEntriesPerSide:
        result.materialization_limit = 1;
        break;
    }
    return result;
}

void check_accepted_tree_boundary(const BoundaryCase& boundary) {
    current_case = boundary.name;
    SequentialStructuredReducer reducer(31'000 + static_cast<uint64_t>(boundary.kind),
                                        tree_budget_fixture(boundary.with_factors));
    const size_t expected_payload = boundary.with_factors ? 14 : 10;
    check_tree_plan_metrics(reducer, expected_payload, boundary.with_factors);
    auto budget = generous_budget();
    set_boundary(budget, boundary.kind, boundary.accepted);
    const auto run = reducer.reduce_budgeted(budget);

    CHECK((run ==
           StructuredReductionRunResult{0, 1, 2, 1, StructuredReductionStopReason::NoCandidates}));
    CHECK(reducer.total_row_count() == 13);
    CHECK(reducer.active_row_count() == 10);
    const auto stats = stats_snapshot(reducer);
    CHECK(stats.input_rows == 11);
    CHECK(stats.singleton_rows_removed == 0);
    CHECK(stats.two_way_merges == 0);
    CHECK(stats.tree_basis_batches == 1);
    CHECK(stats.tree_basis_rows_consumed == 3);
    CHECK(stats.tree_basis_rows_emitted == 2);
    CHECK(stats.persistence_limited_plans == 0);
    CHECK(stats.persistence_cache_hits == 0);
    CHECK(stats.budgeted_runs == 1);
    CHECK(stats.planning_passes == 2);
    CHECK(stats.candidate_plans_considered == 1);
    CHECK(stats.budget_limited_plans == 0);
    CHECK(stats.candidate_limit_stops == 0);
    CHECK(stats.commit_limit_stops == 0);
    CHECK(stats.budget_limit_stops == 0);
    CHECK(stats.peak_prepared_payload_entries == expected_payload);
    CHECK(stats.accepted_lp_fill_growth == 1);
    CHECK(all_zero(stats.budget_rejections));
    CHECK(stats.output_rows == 10);
    CHECK(stats.stop_reason == StructuredReductionStopReason::NoCandidates);
    check_exact_mapping(reducer);
}

void check_rejected_tree_boundary(const BoundaryCase& boundary) {
    current_case = boundary.name;
    SequentialStructuredReducer reducer(32'000 + static_cast<uint64_t>(boundary.kind),
                                        tree_budget_fixture(boundary.with_factors));
    const LogicalSnapshot before = logical_snapshot(reducer);
    const auto two_way_before = reducer.plan_two_way_merges();
    const auto tree_before = reducer.plan_tree_basis_merges();
    auto budget = generous_budget();
    set_boundary(budget, boundary.kind, boundary.rejected);
    const auto run = reducer.reduce_budgeted(budget);

    CHECK((run ==
           StructuredReductionRunResult{0, 0, 0, 0, StructuredReductionStopReason::BudgetLimit}));
    CHECK(logical_equal(before, logical_snapshot(reducer)));
    CHECK(reducer.plan_two_way_merges() == two_way_before);
    CHECK(reducer.plan_tree_basis_merges() == tree_before);
    const auto stats = stats_snapshot(reducer);
    const bool candidate_limit = boundary.kind == BoundaryKind::CandidateExaminations;
    const bool commit_limit = boundary.kind == BoundaryKind::CommitCount;
    const bool pre_examination_stop = candidate_limit || commit_limit;
    CHECK(stats.input_rows == 11);
    CHECK(stats.singleton_rows_removed == 0);
    CHECK(stats.two_way_merges == 0);
    CHECK(stats.tree_basis_batches == 0);
    CHECK(stats.persistence_limited_plans == 0);
    CHECK(stats.persistence_cache_hits == 0);
    CHECK(stats.budgeted_runs == 1);
    CHECK(stats.planning_passes == 1);
    CHECK(stats.candidate_plans_considered == (pre_examination_stop ? 0 : 1));
    CHECK(stats.budget_limited_plans == (pre_examination_stop ? 0 : 1));
    CHECK(stats.candidate_limit_stops == (candidate_limit ? 1 : 0));
    CHECK(stats.commit_limit_stops == (commit_limit ? 1 : 0));
    CHECK(stats.budget_limit_stops == 1);
    CHECK(stats.accepted_lp_fill_growth == 0);
    CHECK(stats.budget_rejections == expected_rejection(boundary.kind));
    CHECK(stats.output_rows == 11);
    CHECK(stats.stop_reason == StructuredReductionStopReason::BudgetLimit);
}

void test_tree_budget_boundaries_and_atomicity() {
    const std::vector<BoundaryCase> boundaries{
        {"F2 examination cap 1", BoundaryKind::CandidateExaminations, 1, 0},
        {"F2 commit cap 1", BoundaryKind::CommitCount, 1, 0},
        {"F2 emitted rows 2", BoundaryKind::EmittedRows, 2, 1},
        {"F2 total fill 1", BoundaryKind::Fill, 1, 0},
        {"F2 batch payload 10", BoundaryKind::BatchPayload, 10, 9},
        {"F2 pivot weight 3", BoundaryKind::PivotWeight, 3, 2},
        {"F2 source atoms 2", BoundaryKind::SourceAtoms, 2, 1},
        {"F2 odd LP output 2", BoundaryKind::OddLpPerOutput, 2, 1},
        {"F2 output LP commit 4", BoundaryKind::OutputLpPerCommit, 4, 3},
        {"F2 materialized pairs 2", BoundaryKind::MaterializedPairs, 2, 1},
        {"F2 persisted LP per side 3", BoundaryKind::PersistedLpPerSide, 3, 2},
        {"F2 factor entries per side 2", BoundaryKind::FactorEntriesPerSide, 2, 1, true},
    };
    for (const auto& boundary : boundaries) {
        check_accepted_tree_boundary(boundary);
        check_rejected_tree_boundary(boundary);
    }
}

void test_overlapping_metadata_limits_report_pivot_first() {
    current_case = "F2 overlapping metadata limits";
    SequentialStructuredReducer reducer(32'100, tree_budget_fixture());
    const LogicalSnapshot before = logical_snapshot(reducer);
    auto budget = generous_budget();
    budget.max_pivot_weight = 2;
    budget.max_source_atoms_per_output = 1;

    const auto run = reducer.reduce_budgeted(budget);
    CHECK((run ==
           StructuredReductionRunResult{0, 0, 0, 0, StructuredReductionStopReason::BudgetLimit}));
    CHECK(logical_equal(before, logical_snapshot(reducer)));
    const auto stats = stats_snapshot(reducer);
    CHECK(stats.candidate_plans_considered == 1);
    CHECK(stats.budget_limited_plans == 1);
    CHECK((stats.budget_rejections == RejectionSnapshot{1, 0, 0, 0, 0, 0}));
    CHECK(stats.persistence_cache_hits == 0);
    CHECK(stats.candidate_limit_stops == 0);
    CHECK(stats.commit_limit_stops == 0);
    CHECK(stats.budget_limit_stops == 1);
    CHECK(stats.output_rows == 11);
    CHECK(stats.stop_reason == StructuredReductionStopReason::BudgetLimit);
}

[[nodiscard]] Relation persistence_heavy_relation(int64_t a, const LargePrimeKey& pivot);

void test_exact_examination_cap_does_not_report_truncation() {
    current_case = "F2 exact examination cap policy";
    SequentialStructuredReducer policy_reducer(32'101, tree_budget_fixture());
    auto policy_budget = generous_budget();
    policy_budget.max_candidate_examinations_per_pass = 1;
    policy_budget.max_pivot_weight = 2;

    const auto policy_run = policy_reducer.reduce_budgeted(policy_budget);
    CHECK((policy_run ==
           StructuredReductionRunResult{0, 0, 0, 0, StructuredReductionStopReason::BudgetLimit}));
    const auto policy_stats = stats_snapshot(policy_reducer);
    CHECK(policy_stats.candidate_plans_considered == 1);
    CHECK(policy_stats.candidate_limit_stops == 0);
    CHECK((policy_stats.budget_rejections == RejectionSnapshot{1, 0, 0, 0, 0, 0}));

    current_case = "F2 exact examination cap persistence";
    const auto p = rational_key(331);
    SequentialStructuredReducer persistence_reducer(
        32'102, {persistence_heavy_relation(1, p), persistence_heavy_relation(2, p)});
    auto persistence_budget = generous_budget();
    persistence_budget.max_candidate_examinations_per_pass = 1;

    const auto persistence_run = persistence_reducer.reduce_budgeted(persistence_budget);
    CHECK((persistence_run == StructuredReductionRunResult{
                                  0, 0, 0, 0, StructuredReductionStopReason::PersistenceLimit}));
    const auto persistence_stats = stats_snapshot(persistence_reducer);
    CHECK(persistence_stats.candidate_plans_considered == 1);
    CHECK(persistence_stats.persistence_limited_plans == 1);
    CHECK(persistence_stats.candidate_limit_stops == 0);
}

[[nodiscard]] std::vector<Relation> policy_skip_fixture() {
    const auto s = rational_key(401);
    const auto p = rational_key(409);
    const auto q = rational_key(419);
    const auto a = rational_key(421);
    const auto b = rational_key(431);
    std::vector<Relation> relations{
        make_relation(1, {s, p}),
        make_relation(2, {s}),
        make_relation(3, {p, q}),
        make_relation(4, {q, a, b}),
    };
    for (int64_t row = 0; row < 8; ++row) {
        relations.push_back(make_relation(20 + row, {a, b}));
    }
    return relations;
}

void test_policy_skip_and_determinism() {
    current_case = "F3 policy skip and determinism";
    auto budget = generous_budget();
    budget.max_source_atoms_per_output = 2;
    SequentialStructuredReducer first(30'003, policy_skip_fixture());
    SequentialStructuredReducer second(30'003, policy_skip_fixture());
    const auto first_run = first.reduce_budgeted(budget);
    const auto second_run = second.reduce_budgeted(budget);

    CHECK((first_run ==
           StructuredReductionRunResult{0, 2, 2, 0, StructuredReductionStopReason::BudgetLimit}));
    CHECK(second_run == first_run);
    CHECK(logical_equal(logical_snapshot(first), logical_snapshot(second)));
    CHECK(stats_snapshot(first) == stats_snapshot(second));
    CHECK(first.total_row_count() == 14);
    CHECK(first.active_row_count() == 10);
    const std::vector<StructuredRowId> expected_active{
        StructuredRowId{4},  StructuredRowId{5},  StructuredRowId{6},  StructuredRowId{7},
        StructuredRowId{8},  StructuredRowId{9},  StructuredRowId{10}, StructuredRowId{11},
        StructuredRowId{12}, StructuredRowId{13},
    };
    CHECK(first.active_row_ids() == expected_active);
    CHECK(source_mask(first.sources(StructuredRowId{12}), first.corpus()) == Mask{0b0011});
    CHECK(source_mask(first.sources(StructuredRowId{13}), first.corpus()) == Mask{0b1100});

    const auto stats = stats_snapshot(first);
    CHECK(stats.input_rows == 12);
    CHECK(stats.two_way_merges == 2);
    CHECK(stats.tree_basis_batches == 0);
    CHECK(stats.budgeted_runs == 1);
    CHECK(stats.planning_passes == 3);
    CHECK(stats.candidate_plans_considered == 4);
    CHECK(stats.budget_limited_plans == 2);
    CHECK(stats.persistence_cache_hits == 0);
    CHECK(stats.commit_limit_stops == 0);
    CHECK(stats.budget_limit_stops == 1);
    CHECK(stats.peak_prepared_payload_entries == 6);
    CHECK((stats.budget_rejections == RejectionSnapshot{0, 2, 0, 0, 0, 0}));
    CHECK(stats.output_rows == 10);
    CHECK(stats.stop_reason == StructuredReductionStopReason::BudgetLimit);
    check_exact_mapping(first);
    check_exact_mapping(second);
}

[[nodiscard]] Relation persistence_heavy_relation(int64_t a, const LargePrimeKey& pivot) {
    Relation relation(a, 1);
    for (size_t i = 0; i < 9; ++i) {
        relation.rational_large_prime.emplace_back(pivot.prime, 0, uint8_t{255});
    }
    return relation;
}

void test_persistence_skip_continues_to_later_candidate() {
    current_case = "F4 persistence skip";
    const auto p = rational_key(503);
    const auto q = rational_key(509);
    SequentialStructuredReducer reducer(30'004, {persistence_heavy_relation(1, p),
                                                 persistence_heavy_relation(2, p),
                                                 make_relation(3, {q}), make_relation(4, {q})});
    const auto initial = reducer.plan_two_way_merges();
    CHECK(initial.size() == 2);
    if (initial.size() == 2) {
        CHECK(initial[0].witness == p);
        CHECK(initial[1].witness == q);
    }

    const auto run = reducer.reduce_budgeted(generous_budget());
    CHECK((run == StructuredReductionRunResult{0, 1, 1, 0,
                                               StructuredReductionStopReason::PersistenceLimit}));
    CHECK(reducer.total_row_count() == 5);
    CHECK(
        reducer.active_row_ids() ==
        std::vector<StructuredRowId>({StructuredRowId{0}, StructuredRowId{1}, StructuredRowId{4}}));
    const auto stats = stats_snapshot(reducer);
    CHECK(stats.input_rows == 4);
    CHECK(stats.two_way_merges == 1);
    CHECK(stats.persistence_limited_plans == 1);
    CHECK(stats.persistence_cache_hits == 1);
    CHECK(stats.budgeted_runs == 1);
    CHECK(stats.planning_passes == 2);
    CHECK(stats.candidate_plans_considered == 2);
    CHECK(stats.budget_limited_plans == 0);
    CHECK(stats.commit_limit_stops == 0);
    CHECK(stats.peak_prepared_payload_entries == 3);
    CHECK(stats.output_rows == 3);
    CHECK(stats.stop_reason == StructuredReductionStopReason::PersistenceLimit);
    CHECK(all_zero(stats.budget_rejections));
    check_exact_mapping(reducer);
}

void test_persistence_cache_hits_do_not_starve_later_candidates() {
    current_case = "F4 examination cache fairness";
    const auto p = rational_key(557);
    const auto q = rational_key(563);
    SequentialStructuredReducer reducer(30'104, {persistence_heavy_relation(1, p),
                                                 persistence_heavy_relation(2, p),
                                                 make_relation(3, {q}), make_relation(4, {q})});
    auto budget = generous_budget();
    budget.max_candidate_examinations_per_pass = 1;
    const LogicalSnapshot initial = logical_snapshot(reducer);

    const auto first_run = reducer.reduce_budgeted(budget);
    CHECK((first_run ==
           StructuredReductionRunResult{0, 0, 0, 0, StructuredReductionStopReason::BudgetLimit}));
    CHECK(logical_equal(initial, logical_snapshot(reducer)));
    const auto first_stats = stats_snapshot(reducer);
    CHECK(first_stats.persistence_limited_plans == 1);
    CHECK(first_stats.persistence_cache_hits == 0);
    CHECK(first_stats.budgeted_runs == 1);
    CHECK(first_stats.planning_passes == 1);
    CHECK(first_stats.candidate_plans_considered == 1);
    CHECK(first_stats.budget_limited_plans == 0);
    CHECK(first_stats.candidate_limit_stops == 1);
    CHECK(first_stats.commit_limit_stops == 0);
    CHECK(first_stats.budget_limit_stops == 1);
    CHECK(first_stats.peak_prepared_payload_entries == 0);
    CHECK(all_zero(first_stats.budget_rejections));
    CHECK(first_stats.output_rows == 4);
    CHECK(first_stats.stop_reason == StructuredReductionStopReason::BudgetLimit);

    const auto second_run = reducer.reduce_budgeted(budget);
    CHECK((second_run == StructuredReductionRunResult{
                             0, 1, 1, 0, StructuredReductionStopReason::PersistenceLimit}));
    CHECK(reducer.total_row_count() == 5);
    CHECK(
        reducer.active_row_ids() ==
        std::vector<StructuredRowId>({StructuredRowId{0}, StructuredRowId{1}, StructuredRowId{4}}));
    const auto second_stats = stats_snapshot(reducer);
    CHECK(second_stats.two_way_merges == 1);
    CHECK(second_stats.persistence_limited_plans == 1);
    CHECK(second_stats.persistence_cache_hits == 2);
    CHECK(second_stats.budgeted_runs == 2);
    CHECK(second_stats.planning_passes == 3);
    CHECK(second_stats.candidate_plans_considered == 2);
    CHECK(second_stats.budget_limited_plans == 0);
    CHECK(second_stats.candidate_limit_stops == 1);
    CHECK(second_stats.commit_limit_stops == 0);
    CHECK(second_stats.budget_limit_stops == 1);
    CHECK(second_stats.peak_prepared_payload_entries == 3);
    CHECK(all_zero(second_stats.budget_rejections));
    CHECK(second_stats.output_rows == 3);
    CHECK(second_stats.stop_reason == StructuredReductionStopReason::PersistenceLimit);
    check_exact_mapping(reducer);
}

void test_stricter_policy_precedes_persistence_cache() {
    current_case = "F4 stricter policy precedes cache";
    const auto p = rational_key(571);
    SequentialStructuredReducer reducer(
        30'204, {persistence_heavy_relation(1, p), persistence_heavy_relation(2, p)});

    const auto first_run = reducer.reduce_budgeted(generous_budget());
    CHECK((first_run == StructuredReductionRunResult{
                            0, 0, 0, 0, StructuredReductionStopReason::PersistenceLimit}));
    const LogicalSnapshot after_first = logical_snapshot(reducer);
    const auto first_stats = stats_snapshot(reducer);
    CHECK(first_stats.persistence_limited_plans == 1);
    CHECK(first_stats.persistence_cache_hits == 0);

    auto stricter_budget = generous_budget();
    stricter_budget.max_pivot_weight = 1;
    const auto second_run = reducer.reduce_budgeted(stricter_budget);
    CHECK((second_run ==
           StructuredReductionRunResult{0, 0, 0, 0, StructuredReductionStopReason::BudgetLimit}));
    CHECK(logical_equal(after_first, logical_snapshot(reducer)));
    const auto second_stats = stats_snapshot(reducer);
    CHECK(second_stats.persistence_limited_plans == 1);
    CHECK(second_stats.persistence_cache_hits == 0);
    CHECK(second_stats.candidate_plans_considered == 2);
    CHECK(second_stats.budget_limited_plans == 1);
    CHECK((second_stats.budget_rejections == RejectionSnapshot{1, 0, 0, 0, 0, 0}));
    CHECK(second_stats.stop_reason == StructuredReductionStopReason::BudgetLimit);
}

void test_commit_limit_stops_with_candidates_and_resets_per_invocation() {
    current_case = "F4 commit limit reset";
    const auto p = rational_key(577);
    const auto q = rational_key(587);
    SequentialStructuredReducer reducer(30'304, {make_relation(1, {p}), make_relation(2, {p}),
                                                 make_relation(3, {q}), make_relation(4, {q})});
    auto budget = generous_budget();
    budget.max_commits = 1;

    const auto first_run = reducer.reduce_budgeted(budget);
    CHECK((first_run ==
           StructuredReductionRunResult{0, 1, 1, 0, StructuredReductionStopReason::BudgetLimit}));
    CHECK(reducer.active_row_count() == 3);
    const auto first_stats = stats_snapshot(reducer);
    CHECK(first_stats.two_way_merges == 1);
    CHECK(first_stats.planning_passes == 2);
    CHECK(first_stats.candidate_plans_considered == 1);
    CHECK(first_stats.commit_limit_stops == 1);
    CHECK(first_stats.budget_limit_stops == 1);
    check_exact_mapping(reducer);

    const auto second_run = reducer.reduce_budgeted(budget);
    CHECK((second_run ==
           StructuredReductionRunResult{0, 1, 1, 0, StructuredReductionStopReason::NoCandidates}));
    CHECK(reducer.active_row_count() == 2);
    const auto second_stats = stats_snapshot(reducer);
    CHECK(second_stats.two_way_merges == 2);
    CHECK(second_stats.budgeted_runs == 2);
    CHECK(second_stats.planning_passes == 4);
    CHECK(second_stats.candidate_plans_considered == 2);
    CHECK(second_stats.commit_limit_stops == 1);
    CHECK(second_stats.budget_limit_stops == 1);
    CHECK(second_stats.stop_reason == StructuredReductionStopReason::NoCandidates);
    check_exact_mapping(reducer);
}

void test_mixed_terminal_precedence_and_atomicity() {
    current_case = "F5 mixed terminal precedence";
    const auto p = rational_key(601);
    const auto q = rational_key(607);
    SequentialStructuredReducer reducer(
        30'005, {persistence_heavy_relation(1, p), persistence_heavy_relation(2, p),
                 make_relation(3, {q}), make_relation(4, {q}), make_relation(5, {q})});
    CHECK(reducer.plan_two_way_merges().size() == 1);
    CHECK(reducer.plan_tree_basis_merges().size() == 1);
    const LogicalSnapshot before = logical_snapshot(reducer);
    auto budget = generous_budget();
    budget.max_pivot_weight = 2;
    const auto run = reducer.reduce_budgeted(budget);

    CHECK((run == StructuredReductionRunResult{0, 0, 0, 0,
                                               StructuredReductionStopReason::PersistenceLimit}));
    CHECK(logical_equal(before, logical_snapshot(reducer)));
    const auto stats = stats_snapshot(reducer);
    CHECK(stats.input_rows == 5);
    CHECK(stats.persistence_limited_plans == 1);
    CHECK(stats.persistence_cache_hits == 0);
    CHECK(stats.budgeted_runs == 1);
    CHECK(stats.planning_passes == 1);
    CHECK(stats.candidate_plans_considered == 2);
    CHECK(stats.budget_limited_plans == 1);
    CHECK(stats.candidate_limit_stops == 0);
    CHECK(stats.commit_limit_stops == 0);
    CHECK(stats.budget_limit_stops == 0);
    CHECK(stats.peak_prepared_payload_entries == 0);
    CHECK((stats.budget_rejections == RejectionSnapshot{1, 0, 0, 0, 0, 0}));
    CHECK(stats.output_rows == 5);
    CHECK(stats.stop_reason == StructuredReductionStopReason::PersistenceLimit);
}

} // namespace

int main() {
    try {
        test_invalid_policy_is_rejected_before_mutation();
        test_no_candidates_after_initial_peel();
        test_after_commit_peel_and_known_kernel();
        test_tree_budget_boundaries_and_atomicity();
        test_overlapping_metadata_limits_report_pivot_first();
        test_exact_examination_cap_does_not_report_truncation();
        test_policy_skip_and_determinism();
        test_persistence_skip_continues_to_later_candidate();
        test_persistence_cache_hits_do_not_starve_later_candidates();
        test_stricter_policy_precedes_persistence_cache();
        test_commit_limit_stops_with_candidates_and_resets_per_invocation();
        test_mixed_terminal_precedence_and_atomicity();
    } catch (const std::exception& error) {
        std::cerr << "fatal structured budgeted-driver test error [" << current_case
                  << "]: " << error.what() << '\n';
        return 1;
    }

    if (failures != 0) {
        std::cerr << failures << " of " << checks << " structured budgeted-driver checks failed\n";
        return 1;
    }
    std::cout << "All " << checks << " structured budgeted-driver checks passed\n";
    return 0;
}
