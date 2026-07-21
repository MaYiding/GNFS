#include "gnfs/relation/structured_incidence_builder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using gnfs::core::Relation;
using gnfs::relation::build_structured_incidence_shards;
using gnfs::relation::LargePrimeKey;
using gnfs::relation::odd_large_prime_keys;
using gnfs::relation::SequentialStructuredReducer;
using gnfs::relation::SourceCorpus;
using gnfs::relation::StructuredIncidenceBucket;
using gnfs::relation::StructuredIncidenceBuildOptions;
using gnfs::relation::StructuredIncidenceBuildResult;
using gnfs::relation::StructuredReductionBudget;
using gnfs::relation::StructuredReductionError;
using gnfs::relation::StructuredReductionErrorCode;
using gnfs::relation::StructuredReductionStats;
using gnfs::relation::StructuredRowId;

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

[[nodiscard]] constexpr LargePrimeKey rational_key(uint64_t prime) noexcept {
    return LargePrimeKey{prime, 0, false};
}

[[nodiscard]] constexpr LargePrimeKey algebraic_key(uint64_t prime, uint64_t root) noexcept {
    return LargePrimeKey{prime, root, true};
}

[[nodiscard]] Relation make_relation(int64_t a, std::initializer_list<LargePrimeKey> keys) {
    Relation relation(a, 1);
    for (const LargePrimeKey& key : keys) {
        if (key.is_algebraic) {
            relation.algebraic_large_prime.emplace_back(key.prime, key.root, uint8_t{1});
        } else {
            relation.rational_large_prime.emplace_back(key.prime, uint8_t{1});
        }
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

[[nodiscard]] bool relations_equal(const std::vector<Relation>& lhs,
                                   const std::vector<Relation>& rhs) {
    if (lhs.size() != rhs.size())
        return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!relation_equal(lhs[i], rhs[i]))
            return false;
    }
    return true;
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
           lhs.budget_rejections.pivot_weight_limit == rhs.budget_rejections.pivot_weight_limit &&
           lhs.budget_rejections.source_limit == rhs.budget_rejections.source_limit &&
           lhs.budget_rejections.output_lp_limit == rhs.budget_rejections.output_lp_limit &&
           lhs.budget_rejections.fill_limit == rhs.budget_rejections.fill_limit &&
           lhs.budget_rejections.emitted_row_limit == rhs.budget_rejections.emitted_row_limit &&
           lhs.budget_rejections.materialization_limit ==
               rhs.budget_rejections.materialization_limit &&
           lhs.output_rows == rhs.output_rows && lhs.stop_reason == rhs.stop_reason;
}

void check_reducer_state_equal(const SequentialStructuredReducer& lhs,
                               const SequentialStructuredReducer& rhs) {
    CHECK(lhs.corpus().generation() == rhs.corpus().generation());
    CHECK(lhs.incidence_epoch() == rhs.incidence_epoch());
    CHECK(lhs.total_row_count() == rhs.total_row_count());
    CHECK(lhs.active_row_count() == rhs.active_row_count());
    const auto lhs_ids = lhs.active_row_ids();
    const auto rhs_ids = rhs.active_row_ids();
    CHECK(lhs_ids == rhs_ids);
    for (size_t i = 0; i < lhs_ids.size(); ++i) {
        CHECK(lhs.sources(lhs_ids[i]) == rhs.sources(rhs_ids[i]));
        const auto lhs_keys = lhs.lp_keys(lhs_ids[i]);
        const auto rhs_keys = rhs.lp_keys(rhs_ids[i]);
        CHECK(std::vector<LargePrimeKey>(lhs_keys.begin(), lhs_keys.end()) ==
              std::vector<LargePrimeKey>(rhs_keys.begin(), rhs_keys.end()));
        CHECK(relation_equal(lhs.materialize(lhs_ids[i]), rhs.materialize(rhs_ids[i])));
    }
    CHECK(relations_equal(lhs.materialize_active(), rhs.materialize_active()));
    CHECK(stats_equal(lhs.stats(), rhs.stats()));
}

[[nodiscard]] StructuredIncidenceBuildResult independent_build(const SourceCorpus& corpus) {
    StructuredIncidenceBuildResult result;
    result.row_lp_keys.reserve(corpus.size());
    std::map<LargePrimeKey, std::vector<StructuredRowId>> buckets;
    for (size_t ordinal = 0; ordinal < corpus.size(); ++ordinal) {
        auto keys = odd_large_prime_keys(corpus.at(corpus.source_id(ordinal)));
        for (const LargePrimeKey& key : keys)
            buckets[key].push_back(StructuredRowId{static_cast<uint64_t>(ordinal)});
        result.stats.total_incidence_entries += keys.size();
        result.row_lp_keys.push_back(std::move(keys));
    }
    for (auto& [key, adjacency] : buckets)
        result.buckets.push_back(StructuredIncidenceBucket{key, std::move(adjacency)});
    return result;
}

void check_structural_result(const StructuredIncidenceBuildResult& result) {
    for (size_t row = 0; row < result.row_lp_keys.size(); ++row) {
        const auto& keys = result.row_lp_keys[row];
        CHECK(std::is_sorted(keys.begin(), keys.end()));
        CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
    }
    for (size_t bucket = 0; bucket < result.buckets.size(); ++bucket) {
        if (bucket != 0)
            CHECK(result.buckets[bucket - 1].key < result.buckets[bucket].key);
        const auto& adjacency = result.buckets[bucket].adjacency;
        CHECK(std::is_sorted(adjacency.begin(), adjacency.end()));
        CHECK(std::adjacent_find(adjacency.begin(), adjacency.end()) == adjacency.end());
        for (const StructuredRowId row : adjacency) {
            CHECK(row.value < result.row_lp_keys.size());
            const auto& keys = result.row_lp_keys[static_cast<size_t>(row.value)];
            CHECK(std::binary_search(keys.begin(), keys.end(), result.buckets[bucket].key));
        }
    }
}

void test_invalid_options_fail_before_construction() {
    SourceCorpus corpus(51'001, {make_relation(1, {rational_key(101)})});
    for (const StructuredIncidenceBuildOptions options :
         std::array<StructuredIncidenceBuildOptions, 2>{{{0, 1}, {1, 0}}}) {
        bool caught = false;
        try {
            (void)build_structured_incidence_shards(corpus, options);
        } catch (const StructuredReductionError& error) {
            caught = true;
            CHECK(error.code() == StructuredReductionErrorCode::InvalidInput);
        }
        CHECK(caught);
    }
}

void test_empty_corpus_has_no_shards() {
    SourceCorpus corpus(51'002, {});
    const auto result =
        build_structured_incidence_shards(corpus, StructuredIncidenceBuildOptions{7, 4});
    CHECK(result.row_lp_keys.empty());
    CHECK(result.buckets.empty());
    CHECK(result.stats.shard_count == 0);
    CHECK(result.stats.peak_shard_rows == 0);
    CHECK(result.stats.peak_shard_incidence_entries == 0);
    CHECK(result.stats.total_incidence_entries == 0);
    CHECK(result.stats.requested_worker_count == 4);
    CHECK(result.stats.peak_worker_count == 0);
}

void test_hand_built_parity_and_full_width_keys() {
    const auto p = rational_key((uint64_t{1} << 40) + 87);
    const auto q = algebraic_key((uint64_t{1} << 41) + 21, (uint64_t{1} << 39) + 9);
    Relation cancelled = make_relation(3, {p, p, q});
    Relation even_exponent = make_relation(4, {});
    even_exponent.rational_large_prime.emplace_back(p.prime, uint8_t{2});
    SourceCorpus corpus(51'003, {make_relation(1, {p}), make_relation(2, {p, q}),
                                 std::move(cancelled), std::move(even_exponent)});

    const auto expected = independent_build(corpus);
    for (const size_t shard_rows : std::array<size_t, 4>{1, 2, 3, 9}) {
        for (const uint32_t workers : std::array<uint32_t, 3>{1, 2, 4}) {
            const auto actual = build_structured_incidence_shards(
                corpus, StructuredIncidenceBuildOptions{shard_rows, workers});
            CHECK(actual.row_lp_keys == expected.row_lp_keys);
            CHECK(actual.buckets == expected.buckets);
            CHECK(actual.stats.shard_count == (corpus.size() + shard_rows - 1) / shard_rows);
            CHECK(actual.stats.peak_shard_rows == std::min(shard_rows, corpus.size()));
            CHECK(actual.stats.total_incidence_entries == expected.stats.total_incidence_entries);
            CHECK(actual.stats.requested_worker_count == workers);
            CHECK(actual.stats.peak_worker_count ==
                  std::min<uint32_t>(workers, static_cast<uint32_t>(actual.stats.peak_shard_rows)));
            check_structural_result(actual);
        }
    }
}

[[nodiscard]] std::vector<Relation> overlapping_fixture(size_t row_count) {
    std::vector<Relation> relations;
    relations.reserve(row_count);
    for (size_t row = 0; row < row_count; ++row) {
        Relation relation(static_cast<int64_t>(row + 10), 1);
        const uint64_t a = 10'007 + static_cast<uint64_t>(row % 31) * 2;
        const uint64_t b = 20'011 + static_cast<uint64_t>((row * 7) % 47) * 2;
        relation.rational_large_prime.emplace_back(a, uint8_t{1});
        relation.rational_large_prime.emplace_back(b, uint8_t{1});
        if (row % 3 == 0) {
            relation.algebraic_large_prime.emplace_back(
                30'011 + static_cast<uint64_t>(row % 19) * 2, 3 + static_cast<uint64_t>(row % 11),
                uint8_t{1});
        }
        if (row % 5 == 0)
            relation.rational_large_prime.emplace_back(a, uint8_t{1});
        relations.push_back(std::move(relation));
    }
    return relations;
}

[[nodiscard]] std::vector<Relation> synthetic_50d_incidence_fixture(size_t row_count) {
    constexpr uint64_t base = (uint64_t{1} << 48) + 1;
    std::vector<Relation> relations;
    relations.reserve(row_count);
    for (size_t row = 0; row < row_count; ++row) {
        Relation relation(static_cast<int64_t>(row + 1), 1);
        const uint64_t group = static_cast<uint64_t>(row / 5);
        const size_t within_group = row % 5;
        if (within_group < 3) {
            relation.rational_large_prime.emplace_back(base + group * 2, uint8_t{1});
            relation.rational_large_prime.emplace_back(
                base + 10'000 + static_cast<uint64_t>(row) * 2, uint8_t{1});
        } else {
            relation.rational_large_prime.emplace_back(base + 20'000 + group * 2, uint8_t{1});
        }
        relations.push_back(std::move(relation));
    }
    return relations;
}

void test_large_overlapping_fixture_is_shard_and_worker_equivalent() {
    SourceCorpus corpus(51'004, overlapping_fixture(257));
    const auto expected = independent_build(corpus);
    for (const size_t shard_rows : std::array<size_t, 5>{1, 7, 32, 64, 512}) {
        StructuredIncidenceBuildResult baseline;
        bool have_baseline = false;
        for (const uint32_t workers : std::array<uint32_t, 3>{1, 2, 4}) {
            const auto actual = build_structured_incidence_shards(
                corpus, StructuredIncidenceBuildOptions{shard_rows, workers});
            CHECK(actual.row_lp_keys == expected.row_lp_keys);
            CHECK(actual.buckets == expected.buckets);
            CHECK(actual.stats.shard_count == (corpus.size() + shard_rows - 1) / shard_rows);
            CHECK(actual.stats.peak_shard_rows == std::min(shard_rows, corpus.size()));
            CHECK(actual.stats.peak_shard_rows <= shard_rows);
            CHECK(actual.stats.total_incidence_entries == expected.stats.total_incidence_entries);
            CHECK(actual.stats.peak_shard_incidence_entries <=
                  actual.stats.total_incidence_entries);
            CHECK(actual.stats.requested_worker_count == workers);
            CHECK(actual.stats.peak_worker_count ==
                  std::min<uint32_t>(workers, static_cast<uint32_t>(actual.stats.peak_shard_rows)));
            check_structural_result(actual);
            if (!have_baseline) {
                baseline = actual;
                have_baseline = true;
            } else {
                CHECK(actual.row_lp_keys == baseline.row_lp_keys);
                CHECK(actual.buckets == baseline.buckets);
                CHECK(actual.stats.shard_count == baseline.stats.shard_count);
                CHECK(actual.stats.peak_shard_rows == baseline.stats.peak_shard_rows);
                CHECK(actual.stats.peak_shard_incidence_entries ==
                      baseline.stats.peak_shard_incidence_entries);
                CHECK(actual.stats.total_incidence_entries ==
                      baseline.stats.total_incidence_entries);
            }
        }
    }
}

void test_hardware_worker_request_preserves_canonical_output() {
    const unsigned hardware = std::thread::hardware_concurrency();
    const uint32_t workers = hardware == 0 ? 1 : static_cast<uint32_t>(hardware);
    SourceCorpus corpus(51'006, overlapping_fixture(17));
    const auto expected = independent_build(corpus);
    const auto actual =
        build_structured_incidence_shards(corpus, StructuredIncidenceBuildOptions{5, workers});
    CHECK(actual.row_lp_keys == expected.row_lp_keys);
    CHECK(actual.buckets == expected.buckets);
    CHECK(actual.stats.shard_count == 4);
    CHECK(actual.stats.peak_shard_rows == 5);
    CHECK(actual.stats.total_incidence_entries == expected.stats.total_incidence_entries);
    CHECK(actual.stats.requested_worker_count == workers);
    CHECK(actual.stats.peak_worker_count ==
          std::min<uint32_t>(workers, static_cast<uint32_t>(actual.stats.peak_shard_rows)));
    check_structural_result(actual);
}

void test_50d_like_first_band_preserves_shard_bound_and_output() {
    constexpr size_t row_count = 5'000;
    constexpr size_t shard_rows = 257;
    SourceCorpus corpus(51'007, synthetic_50d_incidence_fixture(row_count));
    const auto expected = independent_build(corpus);
    for (const uint32_t workers : std::array<uint32_t, 2>{1, 4}) {
        const auto actual = build_structured_incidence_shards(
            corpus, StructuredIncidenceBuildOptions{shard_rows, workers});
        CHECK(actual.row_lp_keys == expected.row_lp_keys);
        CHECK(actual.buckets == expected.buckets);
        CHECK(actual.stats.shard_count == (row_count + shard_rows - 1) / shard_rows);
        CHECK(actual.stats.peak_shard_rows == shard_rows);
        CHECK(actual.stats.peak_shard_incidence_entries <= shard_rows * 2);
        CHECK(actual.stats.total_incidence_entries == expected.stats.total_incidence_entries);
        CHECK(actual.stats.requested_worker_count == workers);
        CHECK(actual.stats.peak_worker_count == workers);
        check_structural_result(actual);
    }
}

[[nodiscard]] std::vector<Relation> reducer_fixture() {
    const auto p = rational_key(40'009);
    const auto q = rational_key(40'013);
    const auto r = algebraic_key(40'019, 17);
    return {
        make_relation(100, {p, q}), make_relation(101, {p, q}), make_relation(102, {q}),
        make_relation(103, {r}),    make_relation(104, {r}),    make_relation(105, {r}),
    };
}

[[nodiscard]] StructuredReductionBudget reducer_budget() {
    StructuredReductionBudget budget(64, 64, 64, 512);
    budget.max_commits = 64;
    return budget;
}

void test_reducer_construction_and_reduction_are_shard_worker_equivalent() {
    constexpr uint64_t generation = 51'005;
    for (const size_t shard_rows : std::array<size_t, 4>{1, 2, 4, 64}) {
        for (const uint32_t workers : std::array<uint32_t, 3>{1, 2, 4}) {
            SequentialStructuredReducer reference(generation, reducer_fixture());
            SequentialStructuredReducer sharded(
                generation, reducer_fixture(),
                StructuredIncidenceBuildOptions{shard_rows, workers});

            check_reducer_state_equal(reference, sharded);
            const auto build_stats = sharded.incidence_build_stats();
            CHECK(build_stats.shard_count ==
                  (sharded.corpus().size() + shard_rows - 1) / shard_rows);
            CHECK(build_stats.peak_shard_rows == std::min(shard_rows, sharded.corpus().size()));
            CHECK(build_stats.peak_shard_rows <= shard_rows);
            CHECK(build_stats.peak_shard_incidence_entries <= build_stats.total_incidence_entries);
            CHECK(build_stats.requested_worker_count == workers);
            CHECK(build_stats.peak_worker_count ==
                  std::min<uint32_t>(workers, static_cast<uint32_t>(build_stats.peak_shard_rows)));

            const auto reference_run = reference.reduce_budgeted(reducer_budget());
            const auto sharded_run = sharded.reduce_budgeted(reducer_budget());
            CHECK(reference_run == sharded_run);
            check_reducer_state_equal(reference, sharded);
        }
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
    run_test("invalid options", test_invalid_options_fail_before_construction);
    run_test("empty corpus", test_empty_corpus_has_no_shards);
    run_test("hand-built parity and full-width keys", test_hand_built_parity_and_full_width_keys);
    run_test("large shard and worker equivalence",
             test_large_overlapping_fixture_is_shard_and_worker_equivalent);
    run_test("hardware worker request", test_hardware_worker_request_preserves_canonical_output);
    run_test("50d-like first scale band",
             test_50d_like_first_band_preserves_shard_bound_and_output);
    run_test("reducer shard and worker equivalence",
             test_reducer_construction_and_reduction_are_shard_worker_equivalent);

    if (failures != 0) {
        std::cerr << failures << " tests failed after " << checks << " checks\n";
        return 1;
    }
    std::cout << "structured incidence builder: " << checks << " checks passed\n";
    return 0;
}
