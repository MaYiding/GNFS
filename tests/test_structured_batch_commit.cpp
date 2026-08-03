#include "gnfs/relation/structured_batch.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using gnfs::core::Relation;
using gnfs::relation::LargePrimeKey;
using gnfs::relation::plan_conflict_free_batch;
using gnfs::relation::prepare_conflict_free_batch;
using gnfs::relation::PreparedTreeBasisMerge;
using gnfs::relation::PreparedTwoWayMerge;
using gnfs::relation::SequentialStructuredReducer;
using gnfs::relation::SourceCombination;
using gnfs::relation::SourceCorpus;
using gnfs::relation::StructuredBatchCommitResult;
using gnfs::relation::StructuredBatchPersistenceLimit;
using gnfs::relation::StructuredBatchPrepareOutcome;
using gnfs::relation::StructuredConflictFreeBatchPlan;
using gnfs::relation::StructuredPreparedBatch;
using gnfs::relation::StructuredReductionError;
using gnfs::relation::StructuredReductionErrorCode;
using gnfs::relation::StructuredReductionRejectionStats;
using gnfs::relation::StructuredReductionStats;
using gnfs::relation::StructuredRowId;
using gnfs::relation::TreeBasisMergePlan;
using gnfs::relation::TwoWayMergePlan;

template <typename Batch>
concept HasOutcomeView = requires(Batch&& batch) { std::forward<Batch>(batch).outcomes(); };

template <typename Batch>
concept CanLegacyCommitTwoWaySlot = requires(SequentialStructuredReducer& reducer, Batch&& batch) {
    reducer.commit(
        std::move(std::get<PreparedTwoWayMerge>(std::forward<Batch>(batch).outcomes().front())));
};

template <typename Batch>
concept CanLegacyCommitTreeSlot = requires(SequentialStructuredReducer& reducer, Batch&& batch) {
    reducer.commit(
        std::move(std::get<PreparedTreeBasisMerge>(std::forward<Batch>(batch).outcomes().front())));
};

static_assert(!std::is_copy_constructible_v<StructuredPreparedBatch>);
static_assert(!std::is_copy_assignable_v<StructuredPreparedBatch>);
static_assert(std::is_nothrow_move_constructible_v<StructuredPreparedBatch>);
static_assert(std::same_as<decltype(std::declval<StructuredPreparedBatch&>().outcomes()),
                           std::span<const StructuredBatchPrepareOutcome>>);
static_assert(HasOutcomeView<StructuredPreparedBatch&>);
static_assert(!HasOutcomeView<StructuredPreparedBatch>);
static_assert(!CanLegacyCommitTwoWaySlot<StructuredPreparedBatch&>);
static_assert(!CanLegacyCommitTwoWaySlot<StructuredPreparedBatch>);
static_assert(!CanLegacyCommitTreeSlot<StructuredPreparedBatch&>);
static_assert(!CanLegacyCommitTreeSlot<StructuredPreparedBatch>);

namespace {

using SourceMask = uint64_t;
using BucketMap = std::map<LargePrimeKey, std::vector<StructuredRowId>>;

size_t checks = 0;
size_t failures = 0;
std::string_view current_test;

[[noreturn]] void check_failed(const char* expression, int line) {
    throw std::runtime_error(std::string("CHECK failed in ") + std::string(current_test) +
                             " at line " + std::to_string(line) + ": " + expression);
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(condition))                                                                          \
            check_failed(#condition, __LINE__);                                                    \
    } while (false)

constexpr std::array<uint32_t, 3> worker_counts{1, 2, 4};

[[nodiscard]] constexpr LargePrimeKey rational_key(uint64_t prime) noexcept {
    return LargePrimeKey{prime, 0, false};
}

[[nodiscard]] Relation make_relation(int64_t a, std::initializer_list<LargePrimeKey> lp_keys) {
    Relation relation(a, 1);
    for (const auto& key : lp_keys)
        relation.rational_large_prime.emplace_back(key.prime, uint8_t{1});
    return relation;
}

[[nodiscard]] Relation persistence_heavy_relation(int64_t a, const LargePrimeKey& pivot) {
    Relation relation(a, 1);
    for (size_t i = 0; i < 9; ++i)
        relation.rational_large_prime.emplace_back(pivot.prime, 0, uint8_t{255});
    return relation;
}

[[nodiscard]] bool relation_equal(const Relation& lhs, const Relation& rhs) {
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.rational_factors == rhs.rational_factors &&
           lhs.algebraic_factors == rhs.algebraic_factors &&
           lhs.rational_large_prime == rhs.rational_large_prime &&
           lhs.algebraic_large_prime == rhs.algebraic_large_prime &&
           lhs.extra_ab_pairs == rhs.extra_ab_pairs;
}

[[nodiscard]] SourceMask source_mask(const SourceCombination& combination,
                                     const SourceCorpus& corpus) {
    CHECK(corpus.size() <= 64);
    CHECK(combination.generation() == corpus.generation());
    SourceMask result = 0;
    for (const auto source : combination.sources()) {
        CHECK(source.ordinal < 64);
        const SourceMask bit = SourceMask{1} << static_cast<unsigned>(source.ordinal);
        CHECK((result & bit) == 0);
        result |= bit;
    }
    return result;
}

[[nodiscard]] std::vector<LargePrimeKey> xor_lp_support(std::span<const LargePrimeKey> lhs,
                                                        std::span<const LargePrimeKey> rhs) {
    std::map<LargePrimeKey, bool> parity;
    for (const auto& key : lhs)
        parity[key] = !parity[key];
    for (const auto& key : rhs)
        parity[key] = !parity[key];

    std::vector<LargePrimeKey> result;
    for (const auto& [key, odd] : parity) {
        if (odd)
            result.push_back(key);
    }
    return result;
}

[[nodiscard]] size_t source_rank(std::vector<SourceMask> rows) {
    std::array<SourceMask, 64> basis{};
    size_t rank = 0;
    for (SourceMask row : rows) {
        for (size_t bit = 64; bit-- > 0;) {
            const SourceMask pivot = SourceMask{1} << static_cast<unsigned>(bit);
            if ((row & pivot) == 0)
                continue;
            if (basis[bit] == 0) {
                basis[bit] = row;
                ++rank;
                row = 0;
                break;
            }
            row ^= basis[bit];
        }
        CHECK(row == 0);
    }
    return rank;
}

[[nodiscard]] bool rejection_stats_equal(const StructuredReductionRejectionStats& lhs,
                                         const StructuredReductionRejectionStats& rhs) noexcept {
    return lhs.pivot_weight_limit == rhs.pivot_weight_limit &&
           lhs.source_limit == rhs.source_limit && lhs.output_lp_limit == rhs.output_lp_limit &&
           lhs.fill_limit == rhs.fill_limit && lhs.emitted_row_limit == rhs.emitted_row_limit &&
           lhs.materialization_limit == rhs.materialization_limit;
}

[[nodiscard]] bool stats_equal(const StructuredReductionStats& lhs,
                               const StructuredReductionStats& rhs) noexcept {
    return lhs.input_rows == rhs.input_rows &&
           lhs.singleton_rows_removed == rhs.singleton_rows_removed &&
           lhs.two_way_merges == rhs.two_way_merges &&
           lhs.tree_basis_batches == rhs.tree_basis_batches &&
           lhs.tree_basis_rows_consumed == rhs.tree_basis_rows_consumed &&
           lhs.tree_basis_rows_emitted == rhs.tree_basis_rows_emitted &&
           lhs.persistence_limited_plans == rhs.persistence_limited_plans &&
           lhs.persistence_cache_hits == rhs.persistence_cache_hits &&
           lhs.budgeted_runs == rhs.budgeted_runs && lhs.planning_passes == rhs.planning_passes &&
           lhs.candidate_plans_considered == rhs.candidate_plans_considered &&
           lhs.budget_limited_plans == rhs.budget_limited_plans &&
           lhs.candidate_limit_stops == rhs.candidate_limit_stops &&
           lhs.commit_limit_stops == rhs.commit_limit_stops &&
           lhs.budget_limit_stops == rhs.budget_limit_stops &&
           lhs.peak_prepared_payload_entries == rhs.peak_prepared_payload_entries &&
           lhs.accepted_lp_fill_growth == rhs.accepted_lp_fill_growth &&
           rejection_stats_equal(lhs.budget_rejections, rhs.budget_rejections) &&
           lhs.output_rows == rhs.output_rows && lhs.stop_reason == rhs.stop_reason;
}

struct RowSnapshot final {
    StructuredRowId row{};
    SourceCombination sources;
    SourceMask source_bits = 0;
    std::vector<LargePrimeKey> lp_keys;
    Relation materialized;
};

struct ReducerSnapshot final {
    uint64_t generation = 0;
    uint64_t incidence_epoch = 0;
    size_t total_rows = 0;
    size_t active_rows = 0;
    std::vector<RowSnapshot> rows;
    StructuredReductionStats stats;
};

[[nodiscard]] ReducerSnapshot capture_state(const SequentialStructuredReducer& reducer) {
    ReducerSnapshot result;
    result.generation = reducer.corpus().generation();
    result.incidence_epoch = reducer.incidence_epoch();
    result.total_rows = reducer.total_row_count();
    result.active_rows = reducer.active_row_count();
    for (const StructuredRowId row : reducer.active_row_ids()) {
        const auto keys = reducer.lp_keys(row);
        result.rows.push_back(RowSnapshot{
            row,
            reducer.sources(row),
            source_mask(reducer.sources(row), reducer.corpus()),
            std::vector<LargePrimeKey>(keys.begin(), keys.end()),
            reducer.materialize(row),
        });
    }
    result.stats = reducer.stats();
    return result;
}

[[nodiscard]] bool state_equal(const ReducerSnapshot& lhs, const ReducerSnapshot& rhs) {
    if (lhs.generation != rhs.generation || lhs.incidence_epoch != rhs.incidence_epoch ||
        lhs.total_rows != rhs.total_rows || lhs.active_rows != rhs.active_rows ||
        lhs.rows.size() != rhs.rows.size() || !stats_equal(lhs.stats, rhs.stats)) {
        return false;
    }
    for (size_t i = 0; i < lhs.rows.size(); ++i) {
        const auto& left = lhs.rows[i];
        const auto& right = rhs.rows[i];
        if (left.row != right.row || left.sources != right.sources ||
            left.source_bits != right.source_bits || left.lp_keys != right.lp_keys ||
            !relation_equal(left.materialized, right.materialized)) {
            return false;
        }
    }
    return true;
}

enum class CandidateKind : uint8_t {
    TwoWay,
    Tree,
};

struct CandidateSignature final {
    CandidateKind kind = CandidateKind::TwoWay;
    uint64_t pivot_prime = 0;
    std::vector<uint64_t> members;

    [[nodiscard]] bool operator==(const CandidateSignature&) const noexcept = default;
};

template <typename Plan> [[nodiscard]] CandidateSignature signature(const Plan& plan) {
    CandidateSignature result;
    if constexpr (std::is_same_v<Plan, TwoWayMergePlan>) {
        result.kind = CandidateKind::TwoWay;
        result.pivot_prime = plan.witness.prime;
    } else {
        static_assert(std::is_same_v<Plan, TreeBasisMergePlan>);
        result.kind = CandidateKind::Tree;
        result.pivot_prime = plan.pivot.prime;
    }
    for (const StructuredRowId member : plan.members)
        result.members.push_back(member.value);
    return result;
}

[[nodiscard]] std::vector<CandidateSignature>
batch_signatures(const StructuredConflictFreeBatchPlan& batch) {
    std::vector<CandidateSignature> result;
    for (const auto& candidate : batch.candidates) {
        if (const auto* two_way = std::get_if<TwoWayMergePlan>(&candidate))
            result.push_back(signature(*two_way));
        else
            result.push_back(signature(std::get<TreeBasisMergePlan>(candidate)));
    }
    return result;
}

struct ExpectedRow final {
    StructuredRowId row{};
    SourceCombination sources;
    SourceMask source_bits = 0;
    std::vector<LargePrimeKey> lp_keys;
};

struct CommitOracle final {
    StructuredBatchCommitResult result;
    size_t prepared_slots = 0;
    uint64_t first_output = 0;
    uint64_t expected_epoch = 0;
    size_t expected_total_rows = 0;
    size_t expected_active_rows = 0;
    std::vector<StructuredRowId> consumed_rows;
    std::vector<ExpectedRow> active_rows;
    StructuredReductionStats expected_stats;
};

[[nodiscard]] const RowSnapshot& find_row(const ReducerSnapshot& before, StructuredRowId row) {
    const auto found = std::find_if(before.rows.begin(), before.rows.end(),
                                    [&](const auto& item) { return item.row == row; });
    CHECK(found != before.rows.end());
    return *found;
}

void mark_consumed(std::vector<bool>& consumed, StructuredRowId row,
                   std::vector<StructuredRowId>& consumed_rows) {
    CHECK(row.value < consumed.size());
    const size_t index = static_cast<size_t>(row.value);
    CHECK(!consumed[index]);
    consumed[index] = true;
    consumed_rows.push_back(row);
}

[[nodiscard]] CommitOracle build_commit_oracle(const SequentialStructuredReducer& reducer,
                                               const StructuredPreparedBatch& prepared) {
    const ReducerSnapshot before = capture_state(reducer);
    CommitOracle oracle;
    oracle.prepared_slots = prepared.outcomes().size();
    oracle.first_output = static_cast<uint64_t>(before.total_rows);
    oracle.expected_epoch = before.incidence_epoch;
    oracle.expected_stats = before.stats;
    oracle.result.output_offsets.push_back(0);

    std::vector<bool> consumed(before.total_rows, false);
    std::vector<ExpectedRow> outputs;

    auto append_output = [&](const SourceCombination& expected_sources,
                             std::span<const LargePrimeKey> expected_lp_keys, StructuredRowId lhs,
                             StructuredRowId rhs) {
        const auto& left = find_row(before, lhs);
        const auto& right = find_row(before, rhs);
        const SourceMask expected_mask = left.source_bits ^ right.source_bits;
        const auto expected_lp = xor_lp_support(left.lp_keys, right.lp_keys);
        CHECK(source_mask(expected_sources, reducer.corpus()) == expected_mask);
        CHECK(std::vector<LargePrimeKey>(expected_lp_keys.begin(), expected_lp_keys.end()) ==
              expected_lp);

        const uint64_t output_value = oracle.first_output + static_cast<uint64_t>(outputs.size());
        const StructuredRowId output{output_value};
        outputs.push_back(ExpectedRow{output, expected_sources, expected_mask, expected_lp});
        oracle.result.output_rows.push_back(output);
    };

    for (const auto& outcome : prepared.outcomes()) {
        if (std::holds_alternative<StructuredBatchPersistenceLimit>(outcome)) {
            ++oracle.result.persistence_limited_candidates;
            oracle.result.output_offsets.push_back(oracle.result.output_rows.size());
            continue;
        }

        if (const auto* two_way = std::get_if<PreparedTwoWayMerge>(&outcome)) {
            const auto& plan = two_way->plan();
            mark_consumed(consumed, plan.members[0], oracle.consumed_rows);
            mark_consumed(consumed, plan.members[1], oracle.consumed_rows);
            append_output(plan.expected_sources, plan.expected_lp_keys, plan.members[0],
                          plan.members[1]);
            ++oracle.result.committed_candidates;
            ++oracle.expected_stats.two_way_merges;
            oracle.result.output_offsets.push_back(oracle.result.output_rows.size());
            continue;
        }

        const auto& tree = std::get<PreparedTreeBasisMerge>(outcome).plan();
        for (const StructuredRowId member : tree.members)
            mark_consumed(consumed, member, oracle.consumed_rows);
        for (const auto& edge : tree.edges) {
            append_output(edge.expected_sources, edge.expected_lp_keys, edge.endpoints[0],
                          edge.endpoints[1]);
        }
        ++oracle.result.committed_candidates;
        oracle.result.lp_fill_growth += tree.lp_fill_growth;
        ++oracle.expected_stats.tree_basis_batches;
        oracle.expected_stats.tree_basis_rows_consumed += tree.members.size();
        oracle.expected_stats.tree_basis_rows_emitted += tree.edges.size();
        oracle.result.output_offsets.push_back(oracle.result.output_rows.size());
    }

    oracle.result.emitted_rows = oracle.result.output_rows.size();
    oracle.expected_total_rows = before.total_rows + oracle.result.emitted_rows;
    CHECK(before.active_rows >= oracle.result.committed_candidates);
    oracle.expected_active_rows = before.active_rows - oracle.result.committed_candidates;
    oracle.expected_stats.output_rows = oracle.expected_active_rows;
    if (oracle.result.committed_candidates != 0)
        ++oracle.expected_epoch;

    for (const auto& row : before.rows) {
        const size_t index = static_cast<size_t>(row.row.value);
        if (!consumed[index]) {
            oracle.active_rows.push_back(
                ExpectedRow{row.row, row.sources, row.source_bits, row.lp_keys});
        }
    }
    oracle.active_rows.insert(oracle.active_rows.end(), outputs.begin(), outputs.end());
    CHECK(oracle.active_rows.size() == oracle.expected_active_rows);
    return oracle;
}

[[nodiscard]] BucketMap active_buckets(const SequentialStructuredReducer& reducer) {
    BucketMap result;
    for (const StructuredRowId row : reducer.active_row_ids()) {
        for (const auto& key : reducer.lp_keys(row))
            result[key].push_back(row);
    }
    return result;
}

void verify_commit_state(const SequentialStructuredReducer& reducer, const CommitOracle& oracle,
                         const StructuredBatchCommitResult& actual) {
    CHECK(actual == oracle.result);
    CHECK(actual.output_offsets.size() == oracle.prepared_slots + 1);
    CHECK(actual.committed_candidates + actual.persistence_limited_candidates ==
          oracle.prepared_slots);
    CHECK(!actual.output_offsets.empty());
    CHECK(actual.output_offsets.front() == 0);
    CHECK(actual.output_offsets.back() == actual.output_rows.size());
    CHECK(std::is_sorted(actual.output_offsets.begin(), actual.output_offsets.end()));
    CHECK(actual.emitted_rows == actual.output_rows.size());
    for (size_t i = 0; i < actual.output_rows.size(); ++i) {
        CHECK(actual.output_rows[i].value == oracle.first_output + static_cast<uint64_t>(i));
    }

    CHECK(reducer.incidence_epoch() == oracle.expected_epoch);
    CHECK(reducer.total_row_count() == oracle.expected_total_rows);
    CHECK(reducer.active_row_count() == oracle.expected_active_rows);
    CHECK(stats_equal(reducer.stats(), oracle.expected_stats));

    std::vector<StructuredRowId> expected_active_ids;
    std::vector<SourceMask> active_source_masks;
    for (const auto& expected : oracle.active_rows) {
        expected_active_ids.push_back(expected.row);
        active_source_masks.push_back(expected.source_bits);
        CHECK(reducer.is_active(expected.row));
        CHECK(reducer.sources(expected.row) == expected.sources);
        CHECK(source_mask(reducer.sources(expected.row), reducer.corpus()) == expected.source_bits);
        const auto actual_lp = reducer.lp_keys(expected.row);
        CHECK(std::vector<LargePrimeKey>(actual_lp.begin(), actual_lp.end()) == expected.lp_keys);
        CHECK(relation_equal(reducer.materialize(expected.row),
                             reducer.corpus().materialize(expected.sources)));
    }
    CHECK(reducer.active_row_ids() == expected_active_ids);
    CHECK(source_rank(active_source_masks) == oracle.expected_active_rows);
    for (const StructuredRowId consumed : oracle.consumed_rows)
        CHECK(!reducer.is_active(consumed));

    // Both planners start with validate_state(), so these calls also verify the
    // internal bucket adjacency and active-degree accounting after publication.
    (void)reducer.plan_two_way_merges();
    (void)reducer.plan_tree_basis_merges();
}

template <typename Action>
void expect_error(StructuredReductionErrorCode expected, Action&& action) {
    bool caught = false;
    try {
        std::forward<Action>(action)();
    } catch (const StructuredReductionError& error) {
        caught = true;
        CHECK(error.code() == expected);
    }
    CHECK(caught);
}

[[nodiscard]] std::vector<Relation> mixed_fixture() {
    const auto y = rational_key(101);
    const auto x = rational_key(103);
    const auto z = rational_key(107);
    const auto u = rational_key(109);
    const auto t = rational_key(113);
    const auto a = rational_key(127);
    const auto b = rational_key(131);
    return {
        make_relation(10, {x, t}), make_relation(11, {x, y}), make_relation(12, {y, u}),
        make_relation(13, {z}),    make_relation(14, {z}),    make_relation(15, {t, a}),
        make_relation(16, {t, b}), make_relation(17, {u}),    make_relation(18, {u}),
    };
}

[[nodiscard]] std::vector<Relation> interleaved_persistence_fixture() {
    const auto success_a = rational_key(601);
    const auto limited_a = rational_key(607);
    const auto success_b = rational_key(613);
    const auto limited_b = rational_key(617);
    return {
        make_relation(20, {success_a}),
        make_relation(21, {success_a}),
        persistence_heavy_relation(22, limited_a),
        persistence_heavy_relation(23, limited_a),
        make_relation(24, {success_b}),
        make_relation(25, {success_b}),
        persistence_heavy_relation(26, limited_b),
        persistence_heavy_relation(27, limited_b),
    };
}

void test_mixed_atomic_commit() {
    const std::vector<CandidateSignature> expected_batch{
        CandidateSignature{CandidateKind::TwoWay, 107, {3, 4}},
        CandidateSignature{CandidateKind::TwoWay, 101, {1, 2}},
        CandidateSignature{CandidateKind::Tree, 113, {0, 5, 6}},
    };

    for (const uint32_t worker_count : worker_counts) {
        SequentialStructuredReducer reducer(91, mixed_fixture());
        const auto plan = plan_conflict_free_batch(reducer, 3);
        CHECK(batch_signatures(plan) == expected_batch);
        const auto& tree = std::get<TreeBasisMergePlan>(plan.candidates[2]);
        CHECK(tree.edges.size() == 2);
        CHECK((tree.edges[0].endpoints ==
               std::array<StructuredRowId, 2>{StructuredRowId{0}, StructuredRowId{5}}));
        CHECK((tree.edges[1].endpoints ==
               std::array<StructuredRowId, 2>{StructuredRowId{0}, StructuredRowId{6}}));
        CHECK(tree.lp_fill_growth == 1);

        auto prepared = prepare_conflict_free_batch(reducer, plan, worker_count);
        const auto oracle = build_commit_oracle(reducer, prepared);
        CHECK(oracle.result.output_rows ==
              std::vector<StructuredRowId>({StructuredRowId{9}, StructuredRowId{10},
                                            StructuredRowId{11}, StructuredRowId{12}}));
        CHECK(oracle.result.output_offsets == std::vector<size_t>({0, 1, 2, 4}));
        CHECK(oracle.result.lp_fill_growth == 1);

        const auto result = reducer.commit(std::move(prepared));
        verify_commit_state(reducer, oracle, result);
        CHECK(reducer.active_row_ids() ==
              std::vector<StructuredRowId>({StructuredRowId{7}, StructuredRowId{8},
                                            StructuredRowId{9}, StructuredRowId{10},
                                            StructuredRowId{11}, StructuredRowId{12}}));

        const auto buckets = active_buckets(reducer);
        CHECK(buckets.size() == 4);
        CHECK(buckets.at(rational_key(103)) ==
              std::vector<StructuredRowId>(
                  {StructuredRowId{10}, StructuredRowId{11}, StructuredRowId{12}}));
        CHECK(buckets.at(rational_key(109)) ==
              std::vector<StructuredRowId>(
                  {StructuredRowId{7}, StructuredRowId{8}, StructuredRowId{10}}));
        CHECK(buckets.at(rational_key(127)) == std::vector<StructuredRowId>({StructuredRowId{11}}));
        CHECK(buckets.at(rational_key(131)) == std::vector<StructuredRowId>({StructuredRowId{12}}));
        CHECK(reducer.plan_two_way_merges().empty());
        const auto trees = reducer.plan_tree_basis_merges();
        CHECK(trees.size() == 2);
        std::vector<uint64_t> pivots;
        for (const auto& candidate : trees)
            pivots.push_back(candidate.pivot.prime);
        std::sort(pivots.begin(), pivots.end());
        CHECK(pivots == std::vector<uint64_t>({103, 109}));
    }
}

void test_interleaved_persistence_offsets() {
    for (const uint32_t worker_count : worker_counts) {
        SequentialStructuredReducer reducer(92, interleaved_persistence_fixture());
        const auto plan = plan_conflict_free_batch(reducer, 4);
        auto prepared = prepare_conflict_free_batch(reducer, plan, worker_count);
        const auto oracle = build_commit_oracle(reducer, prepared);
        CHECK(oracle.result.output_rows ==
              std::vector<StructuredRowId>({StructuredRowId{8}, StructuredRowId{9}}));
        CHECK(oracle.result.output_offsets == std::vector<size_t>({0, 1, 1, 2, 2}));
        CHECK(oracle.result.committed_candidates == 2);
        CHECK(oracle.result.persistence_limited_candidates == 2);

        const auto result = reducer.commit(std::move(prepared));
        verify_commit_state(reducer, oracle, result);
        CHECK(reducer.active_row_ids() ==
              std::vector<StructuredRowId>({StructuredRowId{2}, StructuredRowId{3},
                                            StructuredRowId{6}, StructuredRowId{7},
                                            StructuredRowId{8}, StructuredRowId{9}}));
        const auto buckets = active_buckets(reducer);
        CHECK(buckets.size() == 2);
        CHECK(buckets.at(rational_key(607)) ==
              std::vector<StructuredRowId>({StructuredRowId{2}, StructuredRowId{3}}));
        CHECK(buckets.at(rational_key(617)) ==
              std::vector<StructuredRowId>({StructuredRowId{6}, StructuredRowId{7}}));
        const auto two_way = reducer.plan_two_way_merges();
        CHECK(two_way.size() == 2);
        CHECK(two_way[0].witness == rational_key(607));
        CHECK(two_way[1].witness == rational_key(617));
    }
}

void test_all_persistence_and_empty_are_noops() {
    const auto p = rational_key(701);
    const auto q = rational_key(709);
    SequentialStructuredReducer all_limited(
        93, {persistence_heavy_relation(30, p), persistence_heavy_relation(31, p),
             persistence_heavy_relation(32, q), persistence_heavy_relation(33, q)});
    const auto limited_plan = plan_conflict_free_batch(all_limited, 2);
    auto limited_prepared = prepare_conflict_free_batch(all_limited, limited_plan, 4);
    const auto limited_before = capture_state(all_limited);
    const auto limited_oracle = build_commit_oracle(all_limited, limited_prepared);
    CHECK(limited_oracle.result.output_offsets == std::vector<size_t>({0, 0, 0}));
    CHECK(limited_oracle.result.persistence_limited_candidates == 2);
    const auto limited_result = all_limited.commit(std::move(limited_prepared));
    verify_commit_state(all_limited, limited_oracle, limited_result);
    CHECK(state_equal(limited_before, capture_state(all_limited)));
    expect_error(StructuredReductionErrorCode::InvalidPlan,
                 [&] { (void)all_limited.commit(std::move(limited_prepared)); });
    CHECK(state_equal(limited_before, capture_state(all_limited)));

    SequentialStructuredReducer empty(94, {make_relation(40, {})});
    const auto empty_plan = plan_conflict_free_batch(empty, 4);
    auto empty_prepared = prepare_conflict_free_batch(empty, empty_plan, 4);
    const auto empty_before = capture_state(empty);
    const auto empty_oracle = build_commit_oracle(empty, empty_prepared);
    CHECK(empty_oracle.result.output_offsets == std::vector<size_t>({0}));
    const auto empty_result = empty.commit(std::move(empty_prepared));
    verify_commit_state(empty, empty_oracle, empty_result);
    CHECK(state_equal(empty_before, capture_state(empty)));
    expect_error(StructuredReductionErrorCode::InvalidPlan,
                 [&] { (void)empty.commit(std::move(empty_prepared)); });
    CHECK(state_equal(empty_before, capture_state(empty)));
}

void test_stale_batch_fails_without_mutation() {
    const auto p = rational_key(809);
    const auto q = rational_key(811);
    const auto singleton = rational_key(821);
    SequentialStructuredReducer reducer(95, {make_relation(50, {p}), make_relation(51, {p}),
                                             make_relation(52, {q}), make_relation(53, {q}),
                                             make_relation(54, {singleton})});
    const auto plan = plan_conflict_free_batch(reducer, 2);
    auto prepared = prepare_conflict_free_batch(reducer, plan, 4);
    CHECK(reducer.peel_singletons() == 1);
    const auto before = capture_state(reducer);

    expect_error(StructuredReductionErrorCode::StalePlan,
                 [&] { (void)reducer.commit(std::move(prepared)); });
    CHECK(state_equal(before, capture_state(reducer)));
    (void)reducer.plan_two_way_merges();
    (void)reducer.plan_tree_basis_merges();
}

void test_foreign_payload_fails_without_mutation() {
    const auto p = rational_key(907);
    constexpr uint64_t generation = 96;
    SequentialStructuredReducer source(generation,
                                       {make_relation(60, {p}), make_relation(61, {p})});
    SequentialStructuredReducer target(generation,
                                       {make_relation(160, {p}), make_relation(161, {p})});
    const auto plan = plan_conflict_free_batch(source, 1);
    auto foreign = prepare_conflict_free_batch(source, plan, 2);
    const auto before = capture_state(target);

    expect_error(StructuredReductionErrorCode::InvalidPlan,
                 [&] { (void)target.commit(std::move(foreign)); });
    CHECK(state_equal(before, capture_state(target)));
    (void)target.plan_two_way_merges();
    (void)target.plan_tree_basis_merges();
}

void test_legacy_commit_stales_entire_prepared_batch() {
    const auto p = rational_key(1009);
    const auto q = rational_key(1013);
    SequentialStructuredReducer reducer(97, {make_relation(70, {p}), make_relation(71, {p}),
                                             make_relation(72, {q}), make_relation(73, {q})});
    const auto plan = plan_conflict_free_batch(reducer, 2);
    auto batch = prepare_conflict_free_batch(reducer, plan, 4);
    const auto& first_plan = std::get<TwoWayMergePlan>(plan.candidates.front());
    auto legacy = reducer.prepare(first_plan);
    (void)reducer.commit(std::move(legacy));
    const auto after_legacy = capture_state(reducer);

    expect_error(StructuredReductionErrorCode::StalePlan,
                 [&] { (void)reducer.commit(std::move(batch)); });
    CHECK(state_equal(after_legacy, capture_state(reducer)));
    (void)reducer.plan_two_way_merges();
    (void)reducer.plan_tree_basis_merges();
}

template <typename Action> void run_test(std::string_view name, Action&& action) {
    current_test = name;
    try {
        std::forward<Action>(action)();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "test failed: " << current_test << ": " << error.what() << '\n';
    } catch (...) {
        ++failures;
        std::cerr << "test failed with non-standard exception: " << current_test << '\n';
    }
}

} // namespace

int main() {
    run_test("mixed atomic commit", test_mixed_atomic_commit);
    run_test("interleaved persistence offsets", test_interleaved_persistence_offsets);
    run_test("all persistence and empty no-op", test_all_persistence_and_empty_are_noops);
    run_test("stale batch no mutation", test_stale_batch_fails_without_mutation);
    run_test("foreign payload no mutation", test_foreign_payload_fails_without_mutation);
    run_test("legacy commit stales prepared batch",
             test_legacy_commit_stales_entire_prepared_batch);

    if (failures != 0) {
        std::cerr << failures << " tests failed after " << checks << " checks\n";
        return 1;
    }
    std::cout << "structured batch commit: " << checks << " checks passed\n";
    return 0;
}
