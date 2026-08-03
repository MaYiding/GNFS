#include "gnfs/relation/structured_batch.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
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
using gnfs::relation::StructuredBatchCandidate;
using gnfs::relation::StructuredBatchPersistenceLimit;
using gnfs::relation::StructuredBatchPrepareOutcome;
using gnfs::relation::StructuredConflictFreeBatchPlan;
using gnfs::relation::StructuredReductionError;
using gnfs::relation::StructuredReductionErrorCode;
using gnfs::relation::StructuredReductionRejectionStats;
using gnfs::relation::StructuredReductionStats;
using gnfs::relation::StructuredRowId;
using gnfs::relation::TreeBasisMergePlan;
using gnfs::relation::TwoWayMergePlan;

namespace {

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

[[nodiscard]] bool relation_vectors_equal(const std::vector<Relation>& lhs,
                                          const std::vector<Relation>& rhs) {
    if (lhs.size() != rhs.size())
        return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!relation_equal(lhs[i], rhs[i]))
            return false;
    }
    return true;
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
        result.rows.push_back(RowSnapshot{row, reducer.sources(row),
                                          std::vector<LargePrimeKey>(keys.begin(), keys.end()),
                                          reducer.materialize(row)});
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
            left.lp_keys != right.lp_keys ||
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

[[nodiscard]] CandidateSignature signature(const StructuredBatchCandidate& candidate) {
    CandidateSignature result;
    if (const auto* two_way = std::get_if<TwoWayMergePlan>(&candidate)) {
        result.kind = CandidateKind::TwoWay;
        result.pivot_prime = two_way->witness.prime;
        for (const StructuredRowId member : two_way->members)
            result.members.push_back(member.value);
    } else {
        const auto& tree = std::get<TreeBasisMergePlan>(candidate);
        result.kind = CandidateKind::Tree;
        result.pivot_prime = tree.pivot.prime;
        for (const StructuredRowId member : tree.members)
            result.members.push_back(member.value);
    }
    return result;
}

[[nodiscard]] std::vector<CandidateSignature>
signatures(const std::vector<StructuredBatchCandidate>& candidates) {
    std::vector<CandidateSignature> result;
    result.reserve(candidates.size());
    for (const auto& candidate : candidates)
        result.push_back(signature(candidate));
    return result;
}

enum class OutcomeKind : uint8_t {
    PreparedTwoWay,
    PreparedTree,
    PersistenceLimit,
};

struct OutcomeSnapshot final {
    OutcomeKind kind = OutcomeKind::PersistenceLimit;
    StructuredBatchCandidate candidate;
    std::vector<Relation> materialized;
};

[[nodiscard]] OutcomeSnapshot scalar_prepare_one(const SequentialStructuredReducer& reducer,
                                                 const StructuredBatchCandidate& candidate) {
    try {
        if (const auto* two_way = std::get_if<TwoWayMergePlan>(&candidate)) {
            auto prepared = reducer.prepare(*two_way);
            return OutcomeSnapshot{
                OutcomeKind::PreparedTwoWay, prepared.plan(), {prepared.materialized_relation()}};
        }

        const auto& tree = std::get<TreeBasisMergePlan>(candidate);
        auto prepared = reducer.prepare(tree);
        const auto materialized = prepared.materialized_relations();
        return OutcomeSnapshot{OutcomeKind::PreparedTree, prepared.plan(),
                               std::vector<Relation>(materialized.begin(), materialized.end())};
    } catch (const StructuredReductionError& error) {
        if (error.code() != StructuredReductionErrorCode::PersistenceLimit)
            throw;
        return OutcomeSnapshot{OutcomeKind::PersistenceLimit, candidate, {}};
    }
}

[[nodiscard]] std::vector<OutcomeSnapshot>
scalar_prepare(const SequentialStructuredReducer& reducer,
               const StructuredConflictFreeBatchPlan& batch) {
    std::vector<OutcomeSnapshot> result;
    result.reserve(batch.candidates.size());
    for (const auto& candidate : batch.candidates)
        result.push_back(scalar_prepare_one(reducer, candidate));
    return result;
}

[[nodiscard]] OutcomeSnapshot normalize_outcome(const StructuredBatchPrepareOutcome& outcome) {
    if (const auto* two_way = std::get_if<PreparedTwoWayMerge>(&outcome)) {
        return OutcomeSnapshot{
            OutcomeKind::PreparedTwoWay, two_way->plan(), {two_way->materialized_relation()}};
    }
    if (const auto* tree = std::get_if<PreparedTreeBasisMerge>(&outcome)) {
        const auto materialized = tree->materialized_relations();
        return OutcomeSnapshot{OutcomeKind::PreparedTree, tree->plan(),
                               std::vector<Relation>(materialized.begin(), materialized.end())};
    }
    const auto& limited = std::get<StructuredBatchPersistenceLimit>(outcome);
    return OutcomeSnapshot{OutcomeKind::PersistenceLimit, limited.candidate, {}};
}

[[nodiscard]] bool outcome_equal(const OutcomeSnapshot& lhs, const OutcomeSnapshot& rhs) {
    return lhs.kind == rhs.kind && lhs.candidate == rhs.candidate &&
           relation_vectors_equal(lhs.materialized, rhs.materialized);
}

void check_outcomes_equal(const std::vector<OutcomeSnapshot>& expected,
                          const std::vector<OutcomeSnapshot>& actual) {
    CHECK(actual.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        CHECK(outcome_equal(actual[i], expected[i]));
}

void check_parallel_matches_scalar(const SequentialStructuredReducer& reducer,
                                   const StructuredConflictFreeBatchPlan& batch,
                                   uint32_t worker_count) {
    const ReducerSnapshot before = capture_state(reducer);
    const auto expected = scalar_prepare(reducer, batch);
    CHECK(state_equal(before, capture_state(reducer)));

    const auto prepared = prepare_conflict_free_batch(reducer, batch, worker_count);
    CHECK(prepared.snapshot() == batch.snapshot);

    size_t expected_persistence = 0;
    for (const auto& outcome : expected) {
        if (outcome.kind == OutcomeKind::PersistenceLimit)
            ++expected_persistence;
    }
    CHECK(prepared.persistence_limited_candidate_count() == expected_persistence);
    CHECK(prepared.prepared_candidate_count() == expected.size() - expected_persistence);

    const auto outcome_span = prepared.outcomes();
    std::vector<OutcomeSnapshot> actual;
    actual.reserve(outcome_span.size());
    for (const auto& outcome : outcome_span)
        actual.push_back(normalize_outcome(outcome));
    check_outcomes_equal(expected, actual);
    CHECK(state_equal(before, capture_state(reducer)));
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

[[nodiscard]] std::vector<Relation> main_fixture(bool include_singleton = false) {
    const auto y = rational_key(101);
    const auto x = rational_key(103);
    const auto z = rational_key(107);
    const auto u = rational_key(109);
    const auto t = rational_key(113);
    std::vector<Relation> relations{
        make_relation(10, {x, t}), make_relation(11, {x, y}), make_relation(12, {y, u}),
        make_relation(13, {z}),    make_relation(14, {z}),    make_relation(15, {t}),
        make_relation(16, {t}),    make_relation(17, {u}),    make_relation(18, {u}),
    };
    if (include_singleton)
        relations.push_back(make_relation(19, {rational_key(127)}));
    return relations;
}

void test_empty_and_single_candidate() {
    for (const uint32_t worker_count : worker_counts) {
        SequentialStructuredReducer empty_reducer(81, {make_relation(1, {})});
        const auto empty_batch = plan_conflict_free_batch(empty_reducer, 4);
        CHECK(empty_batch.candidates.empty());
        check_parallel_matches_scalar(empty_reducer, empty_batch, worker_count);

        const auto p = rational_key(211);
        SequentialStructuredReducer single_reducer(82,
                                                   {make_relation(2, {p}), make_relation(3, {p})});
        const auto single_batch = plan_conflict_free_batch(single_reducer, 1);
        CHECK((signatures(single_batch.candidates) ==
               std::vector<CandidateSignature>{
                   CandidateSignature{CandidateKind::TwoWay, 211, {0, 1}}}));
        check_parallel_matches_scalar(single_reducer, single_batch, worker_count);
    }
}

void test_mixed_two_way_and_tree_order() {
    const std::vector<CandidateSignature> expected_signatures{
        CandidateSignature{CandidateKind::TwoWay, 107, {3, 4}},
        CandidateSignature{CandidateKind::TwoWay, 101, {1, 2}},
        CandidateSignature{CandidateKind::Tree, 113, {0, 5, 6}},
    };

    for (const uint32_t worker_count : worker_counts) {
        SequentialStructuredReducer reducer(83, main_fixture());
        const auto batch = plan_conflict_free_batch(reducer, 3);
        CHECK(signatures(batch.candidates) == expected_signatures);

        const auto expected = scalar_prepare(reducer, batch);
        CHECK(expected.size() == 3);
        CHECK(expected[0].kind == OutcomeKind::PreparedTwoWay);
        CHECK(expected[1].kind == OutcomeKind::PreparedTwoWay);
        CHECK(expected[2].kind == OutcomeKind::PreparedTree);
        CHECK(expected[0].materialized.size() == 1);
        CHECK(expected[1].materialized.size() == 1);
        CHECK(expected[2].materialized.size() == 2);

        check_parallel_matches_scalar(reducer, batch, worker_count);
    }
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

void test_interleaved_persistence_outcomes_keep_slot_order() {
    const std::vector<CandidateSignature> expected_signatures{
        CandidateSignature{CandidateKind::TwoWay, 601, {0, 1}},
        CandidateSignature{CandidateKind::TwoWay, 607, {2, 3}},
        CandidateSignature{CandidateKind::TwoWay, 613, {4, 5}},
        CandidateSignature{CandidateKind::TwoWay, 617, {6, 7}},
    };
    constexpr std::array expected_kinds{
        OutcomeKind::PreparedTwoWay,
        OutcomeKind::PersistenceLimit,
        OutcomeKind::PreparedTwoWay,
        OutcomeKind::PersistenceLimit,
    };

    for (const uint32_t worker_count : worker_counts) {
        SequentialStructuredReducer reducer(84, interleaved_persistence_fixture());
        const auto batch = plan_conflict_free_batch(reducer, 4);
        CHECK(signatures(batch.candidates) == expected_signatures);

        const auto scalar = scalar_prepare(reducer, batch);
        CHECK(scalar.size() == expected_kinds.size());
        for (size_t i = 0; i < scalar.size(); ++i)
            CHECK(scalar[i].kind == expected_kinds[i]);

        check_parallel_matches_scalar(reducer, batch, worker_count);
    }
}

void test_stale_batch_fails_without_mutation() {
    for (const uint32_t worker_count : worker_counts) {
        SequentialStructuredReducer reducer(85, main_fixture(true));
        const auto stale_batch = plan_conflict_free_batch(reducer, 3);
        const auto stale_empty_batch = plan_conflict_free_batch(reducer, 0);
        CHECK(stale_empty_batch.candidates.empty());
        const uint64_t old_epoch = reducer.incidence_epoch();
        CHECK(reducer.peel_singletons() == 1);
        CHECK(reducer.incidence_epoch() == old_epoch + 1);
        const ReducerSnapshot before = capture_state(reducer);

        expect_error(StructuredReductionErrorCode::StalePlan, [&] {
            (void)prepare_conflict_free_batch(reducer, stale_batch, worker_count);
        });
        expect_error(StructuredReductionErrorCode::StalePlan, [&] {
            (void)prepare_conflict_free_batch(reducer, stale_empty_batch, worker_count);
        });
        CHECK(state_equal(before, capture_state(reducer)));
    }
}

void test_invalid_worker_and_batch_accounting_fail_without_mutation() {
    SequentialStructuredReducer reducer(87, main_fixture());
    const auto batch = plan_conflict_free_batch(reducer, 3);
    const ReducerSnapshot before = capture_state(reducer);

    expect_error(StructuredReductionErrorCode::InvalidInput,
                 [&] { (void)prepare_conflict_free_batch(reducer, batch, 0); });
    CHECK(state_equal(before, capture_state(reducer)));

    auto forged_accounting = batch;
    ++forged_accounting.raw_candidate_count;
    expect_error(StructuredReductionErrorCode::InvalidPlan,
                 [&] { (void)prepare_conflict_free_batch(reducer, forged_accounting, 2); });
    CHECK(state_equal(before, capture_state(reducer)));
}

void test_forged_exact_plan_fails_without_mutation() {
    for (const uint32_t worker_count : worker_counts) {
        const auto p = rational_key(701);
        SequentialStructuredReducer reducer(86, {make_relation(30, {p}), make_relation(31, {p})});
        auto forged_batch = plan_conflict_free_batch(reducer, 1);
        CHECK(forged_batch.candidates.size() == 1);
        auto* plan = std::get_if<TwoWayMergePlan>(&forged_batch.candidates[0]);
        CHECK(plan != nullptr);
        plan->witness = rational_key(709);
        const ReducerSnapshot before = capture_state(reducer);

        expect_error(StructuredReductionErrorCode::InvalidPlan, [&] {
            (void)prepare_conflict_free_batch(reducer, forged_batch, worker_count);
        });
        CHECK(state_equal(before, capture_state(reducer)));
    }
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
    run_test("empty and single candidate", test_empty_and_single_candidate);
    run_test("mixed two-way and tree order", test_mixed_two_way_and_tree_order);
    run_test("interleaved persistence order",
             test_interleaved_persistence_outcomes_keep_slot_order);
    run_test("stale batch no mutation", test_stale_batch_fails_without_mutation);
    run_test("invalid worker and accounting no mutation",
             test_invalid_worker_and_batch_accounting_fail_without_mutation);
    run_test("forged exact plan no mutation", test_forged_exact_plan_fails_without_mutation);

    if (failures != 0) {
        std::cerr << failures << " tests failed after " << checks << " checks\n";
        return 1;
    }
    std::cout << "structured parallel prepare: " << checks << " checks passed\n";
    return 0;
}
