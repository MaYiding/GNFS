#include "gnfs/api/detail/solver_handoff.hpp"
#include "gnfs/relation/reduction_engine.hpp"
#include "gnfs/relation/structured_filter_profile.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

using gnfs::api::detail::handoff_after_collection;
using gnfs::api::detail::SolverHandoffInfo;
using gnfs::core::Relation;
using gnfs::relation::corpus_digest;
using gnfs::relation::CorpusDigest;
using gnfs::relation::RawRelationSnapshot;
using gnfs::relation::ReductionStrategy;
using gnfs::relation::RelationReductionConfig;
using gnfs::relation::RelationReductionEngine;
using gnfs::relation::RelationReductionResult;
using gnfs::relation::RelationReductionStats;
using gnfs::relation::RelationSourceCombination;
using gnfs::relation::RelationSourceCombinationHash;
using gnfs::relation::select_reduction_strategy;
using gnfs::relation::StructuredFilterSelection;
using gnfs::relation::StructuredReductionBudget;
using gnfs::relation::StructuredReductionError;
using gnfs::relation::StructuredReductionErrorCode;
using gnfs::relation::StructuredReductionStopReason;

static_assert(!std::is_copy_constructible_v<RawRelationSnapshot>);
static_assert(!std::is_copy_assignable_v<RawRelationSnapshot>);
static_assert(std::is_nothrow_move_constructible_v<RawRelationSnapshot>);
static_assert(!std::is_copy_constructible_v<RelationReductionResult>);
static_assert(!std::is_copy_assignable_v<RelationReductionResult>);
static_assert(std::is_nothrow_move_constructible_v<RelationReductionResult>);
static_assert(!std::is_same_v<RawRelationSnapshot, RelationReductionResult>);

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " << #condition   \
                      << '\n';                                                                     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

Relation make_full(int64_t a) {
    Relation relation(a, 1);
    relation.rational_factors.push_back(static_cast<uint32_t>(a));
    return relation;
}

Relation make_partial(int64_t a, std::initializer_list<uint64_t> large_primes) {
    Relation relation(a, 1);
    relation.rational_factors.push_back(static_cast<uint32_t>(a));
    for (uint64_t prime : large_primes) {
        relation.rational_large_prime.emplace_back(prime, uint8_t{1});
    }
    return relation;
}

std::vector<Relation> make_shared_primary_corpus() {
    // h is shared by A/B/C, while u1/u2/u3 connect each hub row to a leaf.
    // Standard V0 materializes A+D, B+E and C+F. V3 also materializes the
    // distinct A+B+D+E source combination with the same primary A.
    constexpr uint64_t h = 101;
    constexpr uint64_t u1 = 103;
    constexpr uint64_t u2 = 107;
    constexpr uint64_t u3 = 109;
    return {
        make_partial(10, {h, u1}), make_partial(20, {h, u2}), make_partial(30, {h, u3}),
        make_partial(40, {u1}),    make_partial(50, {u2}),    make_partial(60, {u3}),
    };
}

std::vector<Relation> make_rich_digest_corpus() {
    Relation first(-17, 19);
    first.rational_factors = {2, 3};
    first.algebraic_factors = {5, 7};
    first.rational_large_prime = {
        gnfs::core::PrimePower(101, 11, uint8_t{1}),
        gnfs::core::PrimePower(103, 13, uint8_t{2}),
    };
    first.algebraic_large_prime = {
        gnfs::core::PrimePower(107, 17, uint8_t{3}),
        gnfs::core::PrimePower(109, 19, uint8_t{4}),
    };
    first.extra_ab_pairs = {{-23, 29}, {31, 37}};

    Relation second(41, 43);
    second.rational_factors = {11, 13};
    second.algebraic_factors = {17, 19};
    second.rational_large_prime = {gnfs::core::PrimePower(127, 23, uint8_t{5})};
    second.algebraic_large_prime = {gnfs::core::PrimePower(131, 29, uint8_t{6})};
    second.extra_ab_pairs = {{47, 53}};
    return {std::move(first), std::move(second)};
}

RelationReductionConfig lp_config(ReductionStrategy strategy) {
    RelationReductionConfig config;
    config.large_primes_enabled = true;
    config.merge_rounds = 10;
    config.strategy = strategy;
    return config;
}

RelationReductionConfig structured_config(uint32_t workers = 1, size_t batch_width = 4) {
    RelationReductionConfig config;
    config.large_primes_enabled = true;
    config.merge_rounds = 0;
    config.strategy = ReductionStrategy::Structured;

    StructuredReductionBudget budget(128, 128, 0, 2048);
    budget.max_commits = 128;
    budget.max_source_atoms_per_output = 64;
    budget.max_odd_lp_keys_per_output =
        static_cast<size_t>(Relation::MAX_SERIALIZED_LARGE_PRIMES) * 2;
    budget.max_materialized_pairs_per_output = 64;

    RelationReductionConfig::StructuredExecutionConfig structured{
        std::move(budget),
        {.max_batch_candidates = batch_width, .worker_count = workers},
        {.max_rows_per_shard = 3, .worker_count = workers},
        gnfs::relation::TreeBasisPlanner::DeterministicMst,
    };
    config.structured = std::move(structured);
    return config;
}

RelationReductionConfig experimental_profile_config(size_t input_rows, uint32_t workers) {
    RelationReductionConfig config;
    config.large_primes_enabled = true;
    config.merge_rounds = 0;
    config.strategy = ReductionStrategy::Structured;
    config.structured =
        gnfs::relation::make_structured_filter_experimental_config(input_rows, workers);
    return config;
}

bool equal_relation(const Relation& lhs, const Relation& rhs) {
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.rational_factors == rhs.rational_factors &&
           lhs.algebraic_factors == rhs.algebraic_factors &&
           lhs.rational_large_prime == rhs.rational_large_prime &&
           lhs.algebraic_large_prime == rhs.algebraic_large_prime &&
           lhs.extra_ab_pairs == rhs.extra_ab_pairs;
}

bool equal_corpus(const std::vector<Relation>& lhs, const std::vector<Relation>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!equal_relation(lhs[i], rhs[i])) {
            return false;
        }
    }
    return true;
}

template <typename Fn> bool throws_invalid_argument(Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

template <typename Fn>
bool throws_structured_error(StructuredReductionErrorCode expected, Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const StructuredReductionError& error) {
        return error.code() == expected;
    } catch (...) {
        return false;
    }
    return false;
}

void check_common_stats(const RelationReductionResult& result) {
    CHECK(result.stats.input_relations == 6);
    CHECK(result.stats.raw_duplicates_removed == 0);
    CHECK(result.stats.filter.input_relations == 6);
    CHECK(result.stats.filter.output_relations == 6);
    CHECK(result.stats.filter.singletons_removed == 0);
    CHECK(result.stats.pre_merge_lp_histogram.unique_keys == 4);
    CHECK(result.stats.pre_merge_lp_histogram.weight_1 == 0);
    CHECK(result.stats.pre_merge_lp_histogram.weight_2 == 3);
    CHECK(result.stats.pre_merge_lp_histogram.weight_3 == 1);
    CHECK(result.stats.pre_merge_lp_histogram.weight_4plus == 0);
    CHECK(result.stats.separated_full_relations == 0);
    CHECK(result.stats.separated_partial_relations == 6);
    CHECK(result.stats.output_relations == result.relations.size());
    CHECK(result.stats.output_lp_columns == gnfs::relation::count_unique_lp_keys(result.relations));
    CHECK(result.stats.output_digest == corpus_digest(result.relations));
}

void test_generation_and_no_large_primes() {
    std::vector<Relation> input{make_full(1), make_full(2)};
    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(47, std::move(input)),
                                                  RelationReductionConfig{});

    CHECK(result.generation == 47);
    CHECK(result.relations.size() == 2);
    CHECK(result.stats.strategy == ReductionStrategy::NoLargePrimes);
    CHECK(result.stats.input_relations == 2);
    CHECK(result.stats.raw_duplicates_removed == 0);
    CHECK(result.stats.filter.input_relations == 2);
    CHECK(result.stats.filter.output_relations == 2);
    CHECK(result.stats.output_relations == 2);
    CHECK(result.stats.output_lp_columns == 0);
    CHECK(result.stats.merged_relations == 0);
    CHECK(result.stats.output_digest == corpus_digest(result.relations));
}

void test_structured_policy_maps_to_named_legacy_strategy() {
    constexpr std::array legacy_strategies{
        ReductionStrategy::NoLargePrimes, ReductionStrategy::FilterOnly,
        ReductionStrategy::StandardV0,    ReductionStrategy::StandardV0WithV3,
        ReductionStrategy::CliqueV0,
    };
    const auto unset = gnfs::relation::decide_structured_filter_policy(
        gnfs::relation::parse_structured_filter_mode(nullptr), true, false);
    const auto explicit_off = gnfs::relation::decide_structured_filter_policy(
        gnfs::relation::parse_structured_filter_mode("0"), true, false);
    for (const ReductionStrategy legacy : legacy_strategies) {
        CHECK(select_reduction_strategy(unset, legacy) == legacy);
        CHECK(select_reduction_strategy(explicit_off, legacy) == legacy);
    }

    const auto forced_on = gnfs::relation::decide_structured_filter_policy(
        gnfs::relation::parse_structured_filter_mode("1"), true, false);
    CHECK(forced_on.selection == StructuredFilterSelection::Structured);
    CHECK(select_reduction_strategy(forced_on, ReductionStrategy::StandardV0) ==
          ReductionStrategy::Structured);
    CHECK(throws_invalid_argument(
        [&] { (void)select_reduction_strategy(forced_on, ReductionStrategy::Structured); }));
}

void test_illegal_combinations_fail_closed() {
    CHECK(throws_invalid_argument([] {
        RawRelationSnapshot invalid(0, {});
        (void)invalid;
    }));

    auto no_lp_with_standard = lp_config(ReductionStrategy::StandardV0);
    no_lp_with_standard.large_primes_enabled = false;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(1, {}), no_lp_with_standard);
    }));

    auto no_lp_with_filter_only = lp_config(ReductionStrategy::FilterOnly);
    no_lp_with_filter_only.large_primes_enabled = false;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(1, {}), no_lp_with_filter_only);
    }));

    RelationReductionConfig lp_with_no_strategy;
    lp_with_no_strategy.large_primes_enabled = true;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(1, {}), lp_with_no_strategy);
    }));

    auto zero_rounds = lp_config(ReductionStrategy::CliqueV0);
    zero_rounds.merge_rounds = 0;
    CHECK(throws_invalid_argument(
        [&] { (void)RelationReductionEngine::reduce(RawRelationSnapshot(1, {}), zero_rounds); }));

    auto unknown_strategy = lp_config(static_cast<ReductionStrategy>(255));
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(1, {}), unknown_strategy);
    }));

    RawRelationSnapshot mutated_generation(1, {});
    mutated_generation.generation = 0;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(std::move(mutated_generation),
                                              RelationReductionConfig{});
    }));

    auto missing_structured_config = lp_config(ReductionStrategy::Structured);
    missing_structured_config.merge_rounds = 0;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(2, {}),
                                              missing_structured_config);
    }));

    auto structured_without_lp = structured_config();
    structured_without_lp.large_primes_enabled = false;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(3, {}), structured_without_lp);
    }));

    auto legacy_with_structured_config = structured_config();
    legacy_with_structured_config.strategy = ReductionStrategy::StandardV0;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(4, {}),
                                              legacy_with_structured_config);
    }));

    auto invalid_parallel = structured_config();
    invalid_parallel.structured->parallel.worker_count = 0;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(5, {}), invalid_parallel);
    }));

    auto invalid_incidence = structured_config();
    invalid_incidence.structured->incidence.max_rows_per_shard = 0;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(6, {}), invalid_incidence);
    }));

    auto invalid_budget = structured_config();
    invalid_budget.structured->budget.max_pivot_weight = 9;
    RawRelationSnapshot preserved_snapshot(7, {make_partial(1, {101})});
    CHECK(throws_structured_error(StructuredReductionErrorCode::InvalidInput, [&] {
        (void)RelationReductionEngine::reduce(std::move(preserved_snapshot), invalid_budget);
    }));
    CHECK(preserved_snapshot.relations.size() == 1);
    CHECK(preserved_snapshot.relations[0].a == 1);
}

void test_exact_abpair_dedup_preserves_old_collision() {
    Relation first(0, 1);
    first.rational_factors = {101};
    Relation second(static_cast<int64_t>(UINT64_C(3) << 32U), 2);
    second.rational_factors = {103};

    std::vector<Relation> input{first, second};
    const CorpusDigest raw_digest = corpus_digest(input);
    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(501, std::move(input)),
                                                  RelationReductionConfig{});

    CHECK(result.stats.input_relations == 2);
    CHECK(result.stats.raw_duplicates_removed == 0);
    CHECK(result.stats.raw_input_digest == raw_digest);
    CHECK(result.relations.size() == 2);
    CHECK(result.relations[0].ab() == first.ab());
    CHECK(result.relations[1].ab() == second.ab());
}

void test_exact_abpair_dedup_keeps_first_occurrence() {
    Relation first(17, 19);
    first.rational_factors = {101};
    Relation duplicate(17, 19);
    duplicate.rational_factors = {103};
    Relation tail(23, 29);
    tail.rational_factors = {107};

    std::vector<Relation> input{first, duplicate, tail};
    const CorpusDigest raw_digest = corpus_digest(input);
    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(502, std::move(input)),
                                                  RelationReductionConfig{});

    CHECK(result.stats.input_relations == 3);
    CHECK(result.stats.raw_duplicates_removed == 1);
    CHECK(result.stats.raw_input_digest == raw_digest);
    CHECK(result.stats.filter.input_relations == 2);
    CHECK(result.relations.size() == 2);
    CHECK(result.relations[0].ab() == first.ab());
    CHECK(result.relations[0].rational_factors == first.rational_factors);
    CHECK(result.relations[1].ab() == tail.ab());
}

void test_merged_input_fails_before_dedup() {
    Relation raw(31, 37);
    Relation merged_duplicate(31, 37);
    merged_duplicate.extra_ab_pairs.emplace_back(41, 43);

    std::vector<Relation> input{raw, merged_duplicate};
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(503, std::move(input)),
                                              RelationReductionConfig{});
    }));
}

void test_digest_covers_every_field_and_order() {
    const auto baseline = make_rich_digest_corpus();
    const CorpusDigest expected = corpus_digest(baseline);

    auto expect_change = [&](auto mutate) {
        auto changed = baseline;
        mutate(changed);
        CHECK(!(corpus_digest(changed) == expected));
    };

    expect_change([](auto& corpus) { ++corpus[0].a; });
    expect_change([](auto& corpus) { ++corpus[0].b; });
    expect_change([](auto& corpus) { ++corpus[0].rational_factors[0]; });
    expect_change([](auto& corpus) {
        std::swap(corpus[0].rational_factors[0], corpus[0].rational_factors[1]);
    });
    expect_change([](auto& corpus) { ++corpus[0].algebraic_factors[0]; });
    expect_change([](auto& corpus) {
        std::swap(corpus[0].algebraic_factors[0], corpus[0].algebraic_factors[1]);
    });
    expect_change([](auto& corpus) { ++corpus[0].rational_large_prime[0].p; });
    expect_change([](auto& corpus) { ++corpus[0].rational_large_prime[0].r; });
    expect_change([](auto& corpus) { ++corpus[0].rational_large_prime[0].e; });
    expect_change([](auto& corpus) {
        std::swap(corpus[0].rational_large_prime[0], corpus[0].rational_large_prime[1]);
    });
    expect_change([](auto& corpus) { ++corpus[0].algebraic_large_prime[0].p; });
    expect_change([](auto& corpus) { ++corpus[0].algebraic_large_prime[0].r; });
    expect_change([](auto& corpus) { ++corpus[0].algebraic_large_prime[0].e; });
    expect_change([](auto& corpus) {
        std::swap(corpus[0].algebraic_large_prime[0], corpus[0].algebraic_large_prime[1]);
    });
    expect_change([](auto& corpus) { ++corpus[0].extra_ab_pairs[0].first; });
    expect_change([](auto& corpus) { ++corpus[0].extra_ab_pairs[0].second; });
    expect_change(
        [](auto& corpus) { std::swap(corpus[0].extra_ab_pairs[0], corpus[0].extra_ab_pairs[1]); });
    expect_change([](auto& corpus) { corpus[0].rational_factors.push_back(23); });
    expect_change([](auto& corpus) { std::swap(corpus[0], corpus[1]); });
}

void test_filter_only_preserves_filtered_partials() {
    std::vector<Relation> input{
        make_partial(1, {101}),
        make_partial(2, {101}),
        make_partial(3, {103}),
    };
    auto config = lp_config(ReductionStrategy::FilterOnly);
    config.merge_rounds = 0;
    auto result =
        RelationReductionEngine::reduce(RawRelationSnapshot(504, std::move(input)), config);

    CHECK(result.stats.strategy == ReductionStrategy::FilterOnly);
    CHECK(result.stats.filter.input_relations == 3);
    CHECK(result.stats.filter.singletons_removed == 1);
    CHECK(result.stats.pre_merge_lp_histogram.unique_keys == 1);
    CHECK(result.stats.pre_merge_lp_histogram.weight_2 == 1);
    CHECK(result.stats.separated_full_relations == 0);
    CHECK(result.stats.separated_partial_relations == 0);
    CHECK(result.stats.merged_relations == 0);
    CHECK(result.stats.standard_v0.output_relations == 0);
    CHECK(result.stats.clique_v0.input_relations == 0);
    CHECK(result.stats.output_relations == 2);
    CHECK(result.stats.output_lp_columns == 1);
    CHECK(result.relations.size() == 2);
    CHECK(!result.relations[0].is_full());
    CHECK(!result.relations[1].is_full());
    CHECK(result.relations[0].a == 1);
    CHECK(result.relations[1].a == 2);
}

void test_fixed_digest_golden() {
    std::vector<Relation> input{
        make_partial(1, {101}),
        make_partial(2, {101}),
        make_full(3),
    };
    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(505, std::move(input)),
                                                  lp_config(ReductionStrategy::StandardV0));

    constexpr CorpusDigest expected_raw{0xc164e37a1f9065fdULL, 0x50d03aafd1a8c057ULL};
    constexpr CorpusDigest expected_output{0x1b01daeb3e04f2dbULL, 0x704af9c872e1e7e7ULL};
    CHECK(result.stats.raw_input_digest == expected_raw);
    CHECK(result.stats.output_digest == expected_output);
}

void test_singleton_purge_precedes_merge() {
    std::vector<Relation> input{
        make_partial(1, {101}),
        make_partial(2, {101}),
        make_partial(3, {103}),
    };
    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(88, std::move(input)),
                                                  lp_config(ReductionStrategy::StandardV0));

    CHECK(result.generation == 88);
    CHECK(result.stats.input_relations == 3);
    CHECK(result.stats.filter.input_relations == 3);
    CHECK(result.stats.filter.output_relations == 2);
    CHECK(result.stats.filter.singletons_removed == 1);
    CHECK(result.stats.pre_merge_lp_histogram.unique_keys == 1);
    CHECK(result.stats.pre_merge_lp_histogram.weight_2 == 1);
    CHECK(result.stats.separated_full_relations == 0);
    CHECK(result.stats.separated_partial_relations == 2);
    CHECK(result.stats.standard_v0.input_1lp == 2);
    CHECK(result.stats.standard_v0.full_produced == 1);
    CHECK(result.stats.merged_relations == 1);
    CHECK(result.stats.output_relations == 1);
    CHECK(result.relations.size() == 1);
    CHECK(result.stats.output_lp_columns == 0);
}

void test_standard_v0_and_explicit_off_unset_equivalence() {
    const auto config = lp_config(ReductionStrategy::StandardV0);

    unsetenv("GNFS_V0_BFS");
    unsetenv("GNFS_CASCADE_V3");
    auto unset_result = RelationReductionEngine::reduce(
        RawRelationSnapshot(101, make_shared_primary_corpus()), config);

    setenv("GNFS_V0_BFS", "0", 1);
    setenv("GNFS_CASCADE_V3", "0", 1);
    auto off_result = RelationReductionEngine::reduce(
        RawRelationSnapshot(101, make_shared_primary_corpus()), config);
    unsetenv("GNFS_V0_BFS");
    unsetenv("GNFS_CASCADE_V3");

    CHECK(equal_corpus(unset_result.relations, off_result.relations));
    CHECK(unset_result.generation == 101);
    CHECK(unset_result.stats.strategy == ReductionStrategy::StandardV0);
    CHECK(unset_result.stats.standard_v0.output_relations == unset_result.stats.merged_relations);
    CHECK(unset_result.stats.v3_relations_added == 0);
    CHECK(unset_result.stats.clique_v0.input_relations == 0);
    check_common_stats(unset_result);
}

void test_standard_v0_with_v3_exact_dedup() {
    auto result =
        RelationReductionEngine::reduce(RawRelationSnapshot(202, make_shared_primary_corpus()),
                                        lp_config(ReductionStrategy::StandardV0WithV3));

    CHECK(result.generation == 202);
    CHECK(result.stats.strategy == ReductionStrategy::StandardV0WithV3);
    CHECK(result.stats.v3.input_relations == 6);
    CHECK(result.stats.v3_relations_added > 0);
    CHECK(result.stats.v3_duplicates_skipped > 0);
    CHECK(result.stats.merged_relations ==
          result.stats.standard_v0.output_relations + result.stats.v3_relations_added);
    check_common_stats(result);

    std::unordered_set<RelationSourceCombination, RelationSourceCombinationHash>
        primary_a_combinations;
    for (const auto& relation : result.relations) {
        if (relation.a == 10 && relation.is_merged()) {
            primary_a_combinations.insert(gnfs::relation::relation_source_combination(relation));
        }
    }
    CHECK(primary_a_combinations.size() >= 2);
}

void test_clique_v0() {
    auto result =
        RelationReductionEngine::reduce(RawRelationSnapshot(303, make_shared_primary_corpus()),
                                        lp_config(ReductionStrategy::CliqueV0));

    CHECK(result.generation == 303);
    CHECK(result.stats.strategy == ReductionStrategy::CliqueV0);
    CHECK(result.stats.clique_v0.input_relations == 6);
    CHECK(result.stats.standard_v0.output_relations == 0);
    CHECK(result.stats.v3.input_relations == 0);
    CHECK(result.stats.merged_relations == result.relations.size());
    check_common_stats(result);
}

void test_structured_strategy_runs_exactly_once() {
    std::vector<Relation> input{
        make_partial(1, {101}),
        make_partial(2, {101}),
        make_full(3),
    };
    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(701, std::move(input)),
                                                  structured_config(2, 4));

    CHECK(result.generation == 701);
    CHECK(result.stats.strategy == ReductionStrategy::Structured);
    CHECK(result.stats.input_relations == 3);
    CHECK(result.stats.raw_duplicates_removed == 0);
    CHECK(result.stats.filter.input_relations == 0);
    CHECK(result.stats.filter.output_relations == 0);
    CHECK(result.stats.standard_v0.input_1lp == 0);
    CHECK(result.stats.standard_v0.input_2lp == 0);
    CHECK(result.stats.clique_v0.input_relations == 0);
    CHECK(result.stats.v3.input_relations == 0);
    CHECK(result.stats.structured.budgeted_runs == 1);
    CHECK(result.stats.structured.input_rows == 3);
    CHECK(result.stats.structured_run.commits == 1);
    CHECK(result.stats.structured_run.emitted_rows == 1);
    CHECK(result.stats.structured_run.stop_reason == StructuredReductionStopReason::NoCandidates);
    CHECK(result.stats.singleton_rows_removed == 0);
    CHECK(result.stats.merged_relations == 1);
    CHECK(result.stats.deduplicated_input_lp_histogram.unique_keys == 1);
    CHECK(result.stats.pre_merge_lp_histogram.unique_keys == 0);
    CHECK(result.stats.output_relations == 2);
    CHECK(result.stats.output_lp_columns == 0);
    CHECK(result.stats.output_digest == corpus_digest(result.relations));
    CHECK(result.relations.size() == 2);
    CHECK(result.relations[0].a == 3);
    CHECK(result.relations[1].is_merged());
}

void test_structured_no_candidates_is_success() {
    const std::vector<Relation> input{make_full(11), make_full(13)};
    auto result =
        RelationReductionEngine::reduce(RawRelationSnapshot(702, input), structured_config(1, 1));

    CHECK(result.stats.structured.budgeted_runs == 1);
    CHECK(result.stats.structured_run.commits == 0);
    CHECK(result.stats.structured_run.stop_reason == StructuredReductionStopReason::NoCandidates);
    CHECK(result.stats.merged_relations == 0);
    CHECK(result.stats.output_relations == input.size());
    CHECK(equal_corpus(result.relations, input));
}

void test_structured_owns_singleton_policy() {
    std::vector<Relation> input{
        make_partial(21, {101}),
        make_full(23),
    };
    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(703, std::move(input)),
                                                  structured_config(1, 2));

    CHECK(result.stats.filter.input_relations == 0);
    CHECK(result.stats.filter.singletons_removed == 0);
    CHECK(result.stats.structured_run.singleton_rows_removed == 1);
    CHECK(result.stats.singleton_rows_removed == 1);
    CHECK(result.stats.output_relations == 1);
    CHECK(result.relations[0].a == 23);
}

void test_structured_deduplicates_before_source_ids() {
    Relation first = make_partial(31, {101});
    Relation duplicate = first;
    duplicate.rational_factors = {999};
    std::vector<Relation> input{first, duplicate, make_partial(37, {101})};

    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(704, std::move(input)),
                                                  structured_config(2, 2));
    CHECK(result.stats.input_relations == 3);
    CHECK(result.stats.raw_duplicates_removed == 1);
    CHECK(result.stats.structured.input_rows == 2);
    CHECK(result.stats.output_relations == 1);
    CHECK(result.relations[0].rational_factors.front() == first.rational_factors.front());
}

void test_structured_final_merge_count_excludes_consumed_intermediates() {
    std::vector<Relation> input{
        make_partial(51, {101}),
        make_partial(53, {101, 103}),
        make_partial(59, {103}),
    };
    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(707, std::move(input)),
                                                  structured_config(2, 2));

    CHECK(result.stats.structured_run.emitted_rows == 2);
    CHECK(result.stats.merged_relations == 1);
    CHECK(result.stats.output_relations == 1);
    CHECK(result.relations[0].is_merged());
}

void test_structured_thread_equivalence() {
    std::optional<RelationReductionResult> baseline;
    for (uint32_t workers : {1U, 2U, 4U}) {
        auto result = RelationReductionEngine::reduce(
            RawRelationSnapshot(705, make_shared_primary_corpus()), structured_config(workers, 3));
        CHECK(result.stats.structured.budgeted_runs == 1);
        CHECK(result.stats.structured_incidence.requested_worker_count == workers);
        CHECK(result.stats.output_digest == corpus_digest(result.relations));
        if (!baseline.has_value()) {
            baseline.emplace(std::move(result));
            continue;
        }
        CHECK(equal_corpus(result.relations, baseline->relations));
        CHECK(result.stats.output_digest == baseline->stats.output_digest);
        CHECK(result.stats.output_relations == baseline->stats.output_relations);
        CHECK(result.stats.output_lp_columns == baseline->stats.output_lp_columns);
        CHECK(result.stats.singleton_rows_removed == baseline->stats.singleton_rows_removed);
        CHECK(result.stats.merged_relations == baseline->stats.merged_relations);
        CHECK(result.stats.structured_run == baseline->stats.structured_run);
        CHECK(result.stats.structured.two_way_merges == baseline->stats.structured.two_way_merges);
        CHECK(result.stats.structured.tree_basis_batches ==
              baseline->stats.structured.tree_basis_batches);
        CHECK(result.stats.structured.output_rows == baseline->stats.structured.output_rows);
        CHECK(result.stats.structured.stop_reason == baseline->stats.structured.stop_reason);
    }
}

void test_structured_experimental_profile_thread_equivalence() {
    std::optional<RelationReductionResult> baseline;
    for (uint32_t workers : {1U, 2U, 4U}) {
        auto corpus = make_shared_primary_corpus();
        const size_t input_rows = corpus.size();
        auto result =
            RelationReductionEngine::reduce(RawRelationSnapshot(708, std::move(corpus)),
                                            experimental_profile_config(input_rows, workers));
        CHECK(result.stats.structured.budgeted_runs == 1);
        CHECK(result.stats.structured_incidence.requested_worker_count == workers);
        if (!baseline) {
            baseline.emplace(std::move(result));
            continue;
        }
        CHECK(equal_corpus(result.relations, baseline->relations));
        CHECK(result.stats.output_digest == baseline->stats.output_digest);
        CHECK(result.stats.structured_run == baseline->stats.structured_run);
        CHECK(result.stats.merged_relations == baseline->stats.merged_relations);
    }
}

void test_structured_invariant_error_never_falls_back() {
    Relation invalid(41, 0);
    invalid.rational_large_prime.emplace_back(101, uint8_t{1});
    std::vector<Relation> input{invalid, make_partial(43, {101})};

    CHECK(throws_structured_error(StructuredReductionErrorCode::InvalidInput, [&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(706, std::move(input)),
                                              structured_config(2, 2));
    }));
}

void test_solver_handoff_exactly_once() {
    {
        std::vector<Relation> relations{make_full(71), make_full(73)};
        RelationReductionStats stats;
        stats.output_relations = relations.size();
        RelationReductionResult reduction(601, std::move(relations), std::move(stats));

        size_t diagnostic_calls = 0;
        size_t solver_calls = 0;
        const uint64_t generation = handoff_after_collection(
            std::move(reduction), 2,
            [&](const SolverHandoffInfo& info) {
                ++diagnostic_calls;
                CHECK(info.relation_rows == 2);
                CHECK(info.estimated_effective_columns == 2);
                CHECK(info.estimated_underbuilt);
            },
            [&](RelationReductionResult handed_off) {
                ++solver_calls;
                CHECK(handed_off.generation == 601);
                CHECK(handed_off.size() == 2);
                return handed_off.generation;
            });

        CHECK(generation == 601);
        CHECK(diagnostic_calls == 1);
        CHECK(solver_calls == 1);
    }

    {
        std::vector<Relation> relations{make_full(79), make_full(83)};
        RelationReductionStats stats;
        stats.output_relations = relations.size();
        RelationReductionResult reduction(602, std::move(relations), std::move(stats));

        size_t diagnostic_calls = 0;
        size_t solver_calls = 0;
        const uint64_t generation = handoff_after_collection(
            std::move(reduction), 1, [&](const SolverHandoffInfo&) { ++diagnostic_calls; },
            [&](RelationReductionResult handed_off) {
                ++solver_calls;
                CHECK(handed_off.generation == 602);
                CHECK(handed_off.size() == 2);
                return handed_off.generation;
            });

        CHECK(generation == 602);
        CHECK(diagnostic_calls == 0);
        CHECK(solver_calls == 1);
    }
}

} // namespace

int main() {
    // Keep unrelated optional merger policies out of this explicit-strategy test.
    unsetenv("GNFS_3LP");
    unsetenv("GNFS_V0_WEIGHT3");
    unsetenv("GNFS_WEIGHT_CUTOFF");
    unsetenv("GNFS_DROP_RESIDUAL");

    test_generation_and_no_large_primes();
    test_structured_policy_maps_to_named_legacy_strategy();
    test_illegal_combinations_fail_closed();
    test_exact_abpair_dedup_preserves_old_collision();
    test_exact_abpair_dedup_keeps_first_occurrence();
    test_merged_input_fails_before_dedup();
    test_digest_covers_every_field_and_order();
    test_filter_only_preserves_filtered_partials();
    test_fixed_digest_golden();
    test_singleton_purge_precedes_merge();
    test_standard_v0_and_explicit_off_unset_equivalence();
    test_standard_v0_with_v3_exact_dedup();
    test_clique_v0();
    test_structured_strategy_runs_exactly_once();
    test_structured_no_candidates_is_success();
    test_structured_owns_singleton_policy();
    test_structured_deduplicates_before_source_ids();
    test_structured_final_merge_count_excludes_consumed_intermediates();
    test_structured_thread_equivalence();
    test_structured_experimental_profile_thread_equivalence();
    test_structured_invariant_error_never_falls_back();
    test_solver_handoff_exactly_once();

    if (failures != 0) {
        std::cerr << failures << " relation reduction engine checks failed\n";
        return 1;
    }
    std::cout << "relation reduction engine checks passed\n";
    return 0;
}
