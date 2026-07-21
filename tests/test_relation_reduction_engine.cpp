#include "gnfs/relation/reduction_engine.hpp"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

using gnfs::core::Relation;
using gnfs::relation::RawRelationSnapshot;
using gnfs::relation::ReductionStrategy;
using gnfs::relation::RelationReductionConfig;
using gnfs::relation::RelationReductionEngine;
using gnfs::relation::RelationReductionResult;
using gnfs::relation::RelationSourceCombination;
using gnfs::relation::RelationSourceCombinationHash;

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

RelationReductionConfig lp_config(ReductionStrategy strategy) {
    RelationReductionConfig config;
    config.large_primes_enabled = true;
    config.merge_rounds = 10;
    config.strategy = strategy;
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

void check_common_stats(const RelationReductionResult& result) {
    CHECK(result.stats.input_relations == 6);
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
}

void test_generation_and_no_large_primes() {
    std::vector<Relation> input{make_full(1), make_full(2)};
    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(47, std::move(input)),
                                                  RelationReductionConfig{});

    CHECK(result.generation == 47);
    CHECK(result.relations.size() == 2);
    CHECK(result.stats.strategy == ReductionStrategy::NoLargePrimes);
    CHECK(result.stats.input_relations == 2);
    CHECK(result.stats.filter.input_relations == 2);
    CHECK(result.stats.filter.output_relations == 2);
    CHECK(result.stats.output_relations == 2);
    CHECK(result.stats.output_lp_columns == 0);
    CHECK(result.stats.merged_relations == 0);
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

} // namespace

int main() {
    // Keep unrelated optional merger policies out of this explicit-strategy test.
    unsetenv("GNFS_3LP");
    unsetenv("GNFS_V0_WEIGHT3");
    unsetenv("GNFS_WEIGHT_CUTOFF");
    unsetenv("GNFS_DROP_RESIDUAL");

    test_generation_and_no_large_primes();
    test_illegal_combinations_fail_closed();
    test_singleton_purge_precedes_merge();
    test_standard_v0_and_explicit_off_unset_equivalence();
    test_standard_v0_with_v3_exact_dedup();
    test_clique_v0();

    if (failures != 0) {
        std::cerr << failures << " relation reduction engine checks failed\n";
        return 1;
    }
    std::cout << "relation reduction engine checks passed\n";
    return 0;
}
