#include "gnfs/relation/structured_reduction.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using gnfs::core::Relation;
using gnfs::relation::LargePrimeKey;
using gnfs::relation::SequentialStructuredReducer;
using gnfs::relation::SourceCombination;
using gnfs::relation::SourceCorpus;
using gnfs::relation::SourceId;
using gnfs::relation::StructuredParallelReductionOptions;
using gnfs::relation::StructuredReductionBudget;
using gnfs::relation::StructuredReductionError;
using gnfs::relation::StructuredReductionErrorCode;
using gnfs::relation::StructuredReductionRunResult;
using gnfs::relation::StructuredReductionStats;
using gnfs::relation::StructuredReductionStopReason;
using gnfs::relation::StructuredRowId;

namespace {

using SourceMask = uint64_t;

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

const std::vector<uint32_t> worker_counts = [] {
    std::vector<uint32_t> counts{1, 2, 4};
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware != 0) {
        const auto value = static_cast<uint32_t>(hardware);
        if (std::find(counts.begin(), counts.end(), value) == counts.end())
            counts.push_back(value);
    }
    return counts;
}();
constexpr std::array<size_t, 3> batch_widths{1, 2, 3};

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

[[nodiscard]] bool relations_equal(std::span<const Relation> lhs, std::span<const Relation> rhs) {
    if (lhs.size() != rhs.size())
        return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!relation_equal(lhs[i], rhs[i]))
            return false;
    }
    return true;
}

[[nodiscard]] SourceMask source_mask(const SourceCombination& combination,
                                     const SourceCorpus& corpus) {
    CHECK(corpus.size() <= 64);
    CHECK(combination.generation() == corpus.generation());
    SourceMask result = 0;
    for (const SourceId source : combination.sources()) {
        CHECK(source.ordinal < 64);
        const SourceMask bit = SourceMask{1} << static_cast<unsigned>(source.ordinal);
        CHECK((result & bit) == 0);
        result |= bit;
    }
    return result;
}

[[nodiscard]] SourceCombination combination_from_mask(SourceMask mask, const SourceCorpus& corpus) {
    CHECK(mask != 0);
    CHECK(corpus.size() <= 64);
    std::vector<SourceId> sources;
    for (size_t ordinal = 0; ordinal < corpus.size(); ++ordinal) {
        const SourceMask bit = SourceMask{1} << static_cast<unsigned>(ordinal);
        if ((mask & bit) != 0)
            sources.push_back(corpus.source_id(ordinal));
    }
    return SourceCombination::canonical(corpus.generation(), std::move(sources));
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

[[nodiscard]] StatsSnapshot stats_snapshot(const StructuredReductionStats& stats) {
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
        RejectionSnapshot{
            stats.budget_rejections.pivot_weight_limit, stats.budget_rejections.source_limit,
            stats.budget_rejections.output_lp_limit, stats.budget_rejections.fill_limit,
            stats.budget_rejections.emitted_row_limit,
            stats.budget_rejections.materialization_limit},
        stats.output_rows,
        stats.stop_reason,
    };
}

struct ReducerSnapshot final {
    uint64_t generation = 0;
    uint64_t incidence_epoch = 0;
    size_t total_rows = 0;
    size_t active_rows = 0;
    std::vector<StructuredRowId> active_ids;
    std::vector<SourceMask> source_masks;
    std::vector<std::vector<LargePrimeKey>> lp_keys;
    std::vector<Relation> materialized;
    StatsSnapshot stats;
};

[[nodiscard]] ReducerSnapshot capture_state(const SequentialStructuredReducer& reducer) {
    ReducerSnapshot result;
    result.generation = reducer.corpus().generation();
    result.incidence_epoch = reducer.incidence_epoch();
    result.total_rows = reducer.total_row_count();
    result.active_rows = reducer.active_row_count();
    result.active_ids = reducer.active_row_ids();
    for (const StructuredRowId row : result.active_ids) {
        result.source_masks.push_back(source_mask(reducer.sources(row), reducer.corpus()));
        const auto keys = reducer.lp_keys(row);
        result.lp_keys.emplace_back(keys.begin(), keys.end());
    }
    result.materialized = reducer.materialize_active();
    result.stats = stats_snapshot(reducer.stats());
    return result;
}

[[nodiscard]] bool logical_equal(const ReducerSnapshot& lhs, const ReducerSnapshot& rhs) {
    return lhs.generation == rhs.generation && lhs.incidence_epoch == rhs.incidence_epoch &&
           lhs.total_rows == rhs.total_rows && lhs.active_rows == rhs.active_rows &&
           lhs.active_ids == rhs.active_ids && lhs.source_masks == rhs.source_masks &&
           lhs.lp_keys == rhs.lp_keys && relations_equal(lhs.materialized, rhs.materialized);
}

[[nodiscard]] bool state_equal(const ReducerSnapshot& lhs, const ReducerSnapshot& rhs) {
    return logical_equal(lhs, rhs) && lhs.stats == rhs.stats;
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

[[nodiscard]] StructuredParallelReductionOptions parallel_options(size_t width, uint32_t workers) {
    StructuredParallelReductionOptions options;
    options.max_batch_candidates = width;
    options.worker_count = workers;
    return options;
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

enum class RandomBudgetProfile : uint8_t {
    Normal,
    Commit,
    Candidate,
    Emitted,
    Fill,
    Materialization,
    Persistence,
};

constexpr std::array<RandomBudgetProfile, 7> random_budget_profiles{
    RandomBudgetProfile::Normal,      RandomBudgetProfile::Commit,
    RandomBudgetProfile::Candidate,   RandomBudgetProfile::Emitted,
    RandomBudgetProfile::Fill,        RandomBudgetProfile::Materialization,
    RandomBudgetProfile::Persistence,
};
constexpr std::array<uint64_t, 3> random_property_seeds{
    0x0000'0000'00c0'ffeeULL,
    0x0000'0000'4d33'6332ULL,
    0x9e37'79b9'7f4a'7c15ULL,
};
constexpr std::array<size_t, 2> random_property_widths{1, 3};

template <typename T> void deterministic_shuffle(std::vector<T>& values, std::mt19937_64& rng) {
    for (size_t upper = values.size(); upper > 1; --upper) {
        const size_t selected = static_cast<size_t>(rng() % upper);
        std::swap(values[upper - 1], values[selected]);
    }
}

[[nodiscard]] Relation make_relation_from_support(int64_t a, std::span<const LargePrimeKey> lp_keys,
                                                  std::span<const size_t> support) {
    Relation relation(a, 1);
    for (const size_t index : support)
        relation.rational_large_prime.emplace_back(lp_keys[index].prime, uint8_t{1});
    return relation;
}

[[nodiscard]] std::vector<Relation> randomized_overlapping_fixture(uint64_t seed,
                                                                   RandomBudgetProfile profile) {
    std::mt19937_64 rng(seed);
    std::vector<LargePrimeKey> keys{
        rational_key(101), rational_key(103), rational_key(107), rational_key(109),
        rational_key(113), rational_key(127), rational_key(131), rational_key(137),
    };
    deterministic_shuffle(keys, rng);

    // A weight-three fan overlaps a cycle through three bridges. Every key has
    // degree at least two, so construction does not erase the randomized graph
    // through singleton peeling before the scheduler sees it.
    std::vector<std::vector<size_t>> supports{
        {0, 1}, {0, 2}, {0, 3}, {1, 2, 3}, {1, 2, 3}, {4, 5},
        {5, 6}, {6, 7}, {7, 4}, {1, 4},    {2, 5},    {3, 6},
    };
    for (size_t extra = 0; extra < 4; ++extra) {
        auto& support = supports[static_cast<size_t>(rng() % supports.size())];
        const size_t key = static_cast<size_t>(rng() % keys.size());
        if (std::find(support.begin(), support.end(), key) == support.end())
            support.push_back(key);
    }
    for (auto& support : supports)
        std::sort(support.begin(), support.end());
    deterministic_shuffle(supports, rng);

    std::vector<Relation> relations;
    relations.reserve(supports.size() + (profile == RandomBudgetProfile::Persistence ? 2U : 0U));
    for (size_t index = 0; index < supports.size(); ++index) {
        Relation relation =
            make_relation_from_support(1'000 + static_cast<int64_t>(index), keys, supports[index]);
        if (profile == RandomBudgetProfile::Materialization)
            relation.rational_factors.push_back(0);
        relations.push_back(std::move(relation));
    }

    if (profile == RandomBudgetProfile::Persistence) {
        const auto persistence_pivot = rational_key(83);
        Relation lhs = persistence_heavy_relation(2'000, persistence_pivot);
        lhs.rational_large_prime.emplace_back(keys[1].prime, uint8_t{1});
        Relation rhs = persistence_heavy_relation(2'001, persistence_pivot);
        rhs.rational_large_prime.emplace_back(keys[5].prime, uint8_t{1});
        relations.push_back(std::move(lhs));
        relations.push_back(std::move(rhs));
    }
    return relations;
}

[[nodiscard]] StructuredReductionBudget random_budget(RandomBudgetProfile profile) {
    auto budget = generous_budget();
    switch (profile) {
    case RandomBudgetProfile::Normal:
    case RandomBudgetProfile::Persistence:
        break;
    case RandomBudgetProfile::Commit:
        budget.max_commits = 1;
        break;
    case RandomBudgetProfile::Candidate:
        budget.max_candidate_examinations_per_pass = 1;
        budget.max_source_atoms_per_output = 1;
        break;
    case RandomBudgetProfile::Emitted:
        budget.max_emitted_rows = 1;
        break;
    case RandomBudgetProfile::Fill:
        budget.max_total_lp_fill_growth = 0;
        break;
    case RandomBudgetProfile::Materialization:
        budget.max_factor_entries_per_side = 0;
        break;
    }
    return budget;
}

[[nodiscard]] bool random_profile_exercised(RandomBudgetProfile profile,
                                            const StructuredReductionRunResult& run,
                                            const StatsSnapshot& stats) {
    switch (profile) {
    case RandomBudgetProfile::Normal:
        return run.commits > 0;
    case RandomBudgetProfile::Commit:
        return stats.commit_limit_stops > 0;
    case RandomBudgetProfile::Candidate:
        return stats.candidate_limit_stops > 0 && stats.budget_rejections.source_limit > 0;
    case RandomBudgetProfile::Emitted:
        return stats.budget_rejections.emitted_row_limit > 0;
    case RandomBudgetProfile::Fill:
        return stats.budget_rejections.fill_limit > 0;
    case RandomBudgetProfile::Materialization:
        return stats.budget_rejections.materialization_limit > 0;
    case RandomBudgetProfile::Persistence:
        return stats.persistence_limited_plans > 0;
    }
    return false;
}

void test_randomized_worker_and_sequential_oracles() {
    for (const RandomBudgetProfile profile : random_budget_profiles) {
        bool exercised = false;
        for (size_t seed_index = 0; seed_index < random_property_seeds.size(); ++seed_index) {
            const uint64_t seed = random_property_seeds[seed_index];
            const auto relations = randomized_overlapping_fixture(seed, profile);
            const auto budget = random_budget(profile);

            for (const size_t width : random_property_widths) {
                const uint64_t generation = 60'000 + static_cast<uint64_t>(profile) * 1'000 +
                                            static_cast<uint64_t>(seed_index) * 10 +
                                            static_cast<uint64_t>(width);
                StructuredReductionRunResult baseline_run;
                ReducerSnapshot baseline_state;
                bool have_baseline = false;

                for (const uint32_t workers : worker_counts) {
                    SequentialStructuredReducer reducer(generation, relations);
                    const auto run =
                        reducer.reduce_budgeted_parallel(budget, parallel_options(width, workers));
                    const auto state = capture_state(reducer);
                    CHECK(run.stop_reason == state.stats.stop_reason);
                    if (!have_baseline) {
                        baseline_run = run;
                        baseline_state = state;
                        have_baseline = true;
                    } else {
                        CHECK(run == baseline_run);
                        CHECK(state_equal(state, baseline_state));
                    }
                }

                exercised = exercised ||
                            random_profile_exercised(profile, baseline_run, baseline_state.stats);

                if (width == 1) {
                    SequentialStructuredReducer sequential(generation, relations);
                    const auto sequential_run = sequential.reduce_budgeted(budget);
                    const auto sequential_state = capture_state(sequential);
                    CHECK(sequential_run.stop_reason == sequential_state.stats.stop_reason);
                    CHECK(sequential_run == baseline_run);
                    CHECK(state_equal(sequential_state, baseline_state));
                }
            }
        }
        CHECK(exercised);
    }
}

[[nodiscard]] std::vector<Relation> mixed_fixture() {
    const auto y = rational_key(101);
    const auto x = rational_key(103);
    const auto z = rational_key(107);
    const auto u = rational_key(109);
    const auto t = rational_key(113);
    return {
        make_relation(10, {x, t}), make_relation(11, {x, y}), make_relation(12, {y, u}),
        make_relation(13, {z}),    make_relation(14, {z}),    make_relation(15, {t}),
        make_relation(16, {t}),    make_relation(17, {u}),    make_relation(18, {u}),
    };
}

struct ExpectedActiveRow final {
    StructuredRowId row{};
    SourceMask sources = 0;
    std::vector<LargePrimeKey> lp_keys;
};

[[nodiscard]] std::vector<ExpectedActiveRow> expected_mixed_rows(size_t width) {
    if (width <= 2) {
        return {
            ExpectedActiveRow{StructuredRowId{9}, SourceMask{24}, {}},
            ExpectedActiveRow{StructuredRowId{12}, SourceMask{384}, {}},
            ExpectedActiveRow{StructuredRowId{14}, SourceMask{96}, {}},
            ExpectedActiveRow{StructuredRowId{15}, SourceMask{167}, {}},
        };
    }
    return {
        ExpectedActiveRow{StructuredRowId{9}, SourceMask{24}, {}},
        ExpectedActiveRow{StructuredRowId{11}, SourceMask{96}, {}},
        ExpectedActiveRow{StructuredRowId{14}, SourceMask{384}, {}},
        ExpectedActiveRow{StructuredRowId{15}, SourceMask{167}, {}},
    };
}

void check_mixed_result(const SequentialStructuredReducer& reducer, size_t width,
                        const StructuredReductionRunResult& run) {
    CHECK((run ==
           StructuredReductionRunResult{0, 5, 7, 0, StructuredReductionStopReason::NoCandidates}));
    CHECK(reducer.incidence_epoch() == static_cast<uint64_t>(7 - width));
    CHECK(reducer.total_row_count() == 16);
    CHECK(reducer.active_row_count() == 4);

    const auto expected = expected_mixed_rows(width);
    std::vector<StructuredRowId> expected_ids;
    std::vector<SourceMask> expected_masks;
    std::vector<Relation> expected_materialized;
    for (const auto& row : expected) {
        expected_ids.push_back(row.row);
        expected_masks.push_back(row.sources);
        CHECK(reducer.sources(row.row) == combination_from_mask(row.sources, reducer.corpus()));
        const auto keys = reducer.lp_keys(row.row);
        CHECK(std::vector<LargePrimeKey>(keys.begin(), keys.end()) == row.lp_keys);
        const Relation materialized =
            reducer.corpus().materialize(combination_from_mask(row.sources, reducer.corpus()));
        CHECK(relation_equal(reducer.materialize(row.row), materialized));
        expected_materialized.push_back(materialized);
    }
    CHECK(reducer.active_row_ids() == expected_ids);
    CHECK(relations_equal(reducer.materialize_active(), expected_materialized));
    CHECK(source_rank(expected_masks) == expected.size());

    StatsSnapshot expected_stats;
    expected_stats.input_rows = 9;
    expected_stats.two_way_merges = 3;
    expected_stats.tree_basis_batches = 2;
    expected_stats.tree_basis_rows_consumed = 6;
    expected_stats.tree_basis_rows_emitted = 4;
    expected_stats.budgeted_runs = 1;
    expected_stats.planning_passes = 7 - width;
    expected_stats.candidate_plans_considered = 5;
    expected_stats.peak_prepared_payload_entries = 12;
    expected_stats.output_rows = 4;
    expected_stats.stop_reason = StructuredReductionStopReason::NoCandidates;
    CHECK(stats_snapshot(reducer.stats()) == expected_stats);
}

void test_mixed_width_and_worker_determinism() {
    for (const size_t width : batch_widths) {
        StructuredReductionRunResult baseline_run;
        ReducerSnapshot baseline_state;
        bool have_baseline = false;
        const uint64_t generation = 41'000 + static_cast<uint64_t>(width);

        for (const uint32_t workers : worker_counts) {
            SequentialStructuredReducer reducer(generation, mixed_fixture());
            const auto run = reducer.reduce_budgeted_parallel(generous_budget(),
                                                              parallel_options(width, workers));
            check_mixed_result(reducer, width, run);
            const auto state = capture_state(reducer);
            if (!have_baseline) {
                baseline_run = run;
                baseline_state = state;
                have_baseline = true;
            } else {
                CHECK(run == baseline_run);
                CHECK(state_equal(state, baseline_state));
            }
        }

        if (width == 1) {
            SequentialStructuredReducer sequential(generation, mixed_fixture());
            const auto sequential_run = sequential.reduce_budgeted(generous_budget());
            CHECK(sequential_run == baseline_run);
            CHECK(state_equal(capture_state(sequential), baseline_state));
        }
    }
}

[[nodiscard]] std::vector<Relation> tree_boundary_fixture() {
    const auto p = rational_key(307);
    const auto a = rational_key(311);
    const auto b = rational_key(313);
    const auto c = rational_key(317);
    std::vector<Relation> relations{
        make_relation(1, {p, a}),
        make_relation(2, {p, b}),
        make_relation(3, {p, c}),
    };
    for (int64_t row = 0; row < 8; ++row)
        relations.push_back(make_relation(10 + row, {a, b, c}));
    return relations;
}

enum class BoundaryKind : uint8_t {
    Commits,
    EmittedRows,
    Fill,
};

void set_boundary(StructuredReductionBudget& budget, BoundaryKind kind, size_t value) {
    switch (kind) {
    case BoundaryKind::Commits:
        budget.max_commits = value;
        break;
    case BoundaryKind::EmittedRows:
        budget.max_emitted_rows = value;
        break;
    case BoundaryKind::Fill:
        budget.max_total_lp_fill_growth = value;
        break;
    }
}

void check_tree_boundary(BoundaryKind kind, size_t limit, bool accepted, uint64_t generation) {
    auto budget = generous_budget();
    set_boundary(budget, kind, limit);

    SequentialStructuredReducer parallel(generation, tree_boundary_fixture());
    const auto before = capture_state(parallel);
    const auto parallel_run = parallel.reduce_budgeted_parallel(budget, parallel_options(1, 4));

    SequentialStructuredReducer sequential(generation, tree_boundary_fixture());
    const auto sequential_run = sequential.reduce_budgeted(budget);
    CHECK(parallel_run == sequential_run);
    CHECK(state_equal(capture_state(parallel), capture_state(sequential)));

    if (accepted) {
        CHECK((parallel_run == StructuredReductionRunResult{
                                   0, 1, 2, 1, StructuredReductionStopReason::NoCandidates}));
        CHECK(parallel.incidence_epoch() == before.incidence_epoch + 1);
        CHECK(parallel.total_row_count() == 13);
        CHECK(parallel.active_row_count() == 10);
        CHECK(parallel.stats().peak_prepared_payload_entries == 10);
        CHECK(parallel.stats().budget_limited_plans == 0);
        CHECK(parallel.stats().budget_limit_stops == 0);
        return;
    }

    CHECK((parallel_run ==
           StructuredReductionRunResult{0, 0, 0, 0, StructuredReductionStopReason::BudgetLimit}));
    CHECK(logical_equal(capture_state(parallel), before));
    CHECK(parallel.stats().budget_limit_stops == 1);
    if (kind == BoundaryKind::Commits) {
        CHECK(parallel.stats().candidate_plans_considered == 0);
        CHECK(parallel.stats().commit_limit_stops == 1);
        CHECK(parallel.stats().budget_limited_plans == 0);
    } else {
        CHECK(parallel.stats().candidate_plans_considered == 1);
        CHECK(parallel.stats().commit_limit_stops == 0);
        CHECK(parallel.stats().budget_limited_plans == 1);
        CHECK(parallel.stats().budget_rejections.emitted_row_limit ==
              (kind == BoundaryKind::EmittedRows ? 1 : 0));
        CHECK(parallel.stats().budget_rejections.fill_limit ==
              (kind == BoundaryKind::Fill ? 1 : 0));
    }
}

void test_exact_and_one_over_budget_boundaries() {
    check_tree_boundary(BoundaryKind::Commits, 1, true, 42'001);
    check_tree_boundary(BoundaryKind::Commits, 0, false, 42'002);
    check_tree_boundary(BoundaryKind::EmittedRows, 2, true, 42'003);
    check_tree_boundary(BoundaryKind::EmittedRows, 1, false, 42'004);
    check_tree_boundary(BoundaryKind::Fill, 1, true, 42'005);
    check_tree_boundary(BoundaryKind::Fill, 0, false, 42'006);
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

void test_interleaved_persistence_publish_and_cache_folding() {
    SequentialStructuredReducer reducer(42'101, interleaved_persistence_fixture());
    const uint64_t initial_epoch = reducer.incidence_epoch();
    auto budget = generous_budget();
    budget.max_candidate_examinations_per_pass = 4;

    const auto first_run = reducer.reduce_budgeted_parallel(budget, parallel_options(4, 4));
    CHECK((first_run == StructuredReductionRunResult{
                            0, 2, 2, 0, StructuredReductionStopReason::PersistenceLimit}));
    CHECK(reducer.incidence_epoch() == initial_epoch + 1);
    CHECK(reducer.total_row_count() == 10);
    CHECK(
        reducer.active_row_ids() ==
        std::vector<StructuredRowId>({StructuredRowId{2}, StructuredRowId{3}, StructuredRowId{6},
                                      StructuredRowId{7}, StructuredRowId{8}, StructuredRowId{9}}));
    CHECK(source_mask(reducer.sources(StructuredRowId{8}), reducer.corpus()) == SourceMask{3});
    CHECK(source_mask(reducer.sources(StructuredRowId{9}), reducer.corpus()) == SourceMask{48});
    CHECK(reducer.lp_keys(StructuredRowId{8}).empty());
    CHECK(reducer.lp_keys(StructuredRowId{9}).empty());
    std::vector<SourceMask> active_masks;
    for (const StructuredRowId row : reducer.active_row_ids())
        active_masks.push_back(source_mask(reducer.sources(row), reducer.corpus()));
    CHECK(source_rank(active_masks) == reducer.active_row_count());

    const auto& first_stats = reducer.stats();
    CHECK(first_stats.two_way_merges == 2);
    CHECK(first_stats.persistence_limited_plans == 2);
    CHECK(first_stats.persistence_cache_hits == 2);
    CHECK(first_stats.budgeted_runs == 1);
    CHECK(first_stats.planning_passes == 2);
    CHECK(first_stats.candidate_plans_considered == 4);
    CHECK(first_stats.budget_limited_plans == 0);
    CHECK(first_stats.candidate_limit_stops == 0);
    CHECK(first_stats.peak_prepared_payload_entries == 3);
    CHECK(first_stats.output_rows == 6);
    CHECK(first_stats.stop_reason == StructuredReductionStopReason::PersistenceLimit);

    const auto before_retry = capture_state(reducer);
    auto cache_only_budget = generous_budget();
    cache_only_budget.max_candidate_examinations_per_pass = 0;
    const auto retry_run =
        reducer.reduce_budgeted_parallel(cache_only_budget, parallel_options(4, 4));
    CHECK((retry_run == StructuredReductionRunResult{
                            0, 0, 0, 0, StructuredReductionStopReason::PersistenceLimit}));
    CHECK(logical_equal(capture_state(reducer), before_retry));
    const auto& retry_stats = reducer.stats();
    CHECK(retry_stats.persistence_limited_plans == 2);
    CHECK(retry_stats.persistence_cache_hits == 4);
    CHECK(retry_stats.budgeted_runs == 2);
    CHECK(retry_stats.planning_passes == 3);
    CHECK(retry_stats.candidate_plans_considered == 4);
    CHECK(retry_stats.candidate_limit_stops == 0);
    CHECK(retry_stats.budget_limit_stops == 0);
    CHECK(retry_stats.stop_reason == StructuredReductionStopReason::PersistenceLimit);
}

void test_cumulative_budget_precedes_later_persistence_slots() {
    auto budget = generous_budget();
    budget.max_emitted_rows = 1;

    SequentialStructuredReducer sequential(42'102, interleaved_persistence_fixture());
    const auto sequential_run = sequential.reduce_budgeted(budget);
    const auto sequential_state = capture_state(sequential);
    CHECK((sequential_run ==
           StructuredReductionRunResult{0, 1, 1, 0, StructuredReductionStopReason::BudgetLimit}));
    CHECK(sequential.stats().planning_passes == 2);
    CHECK(sequential.stats().candidate_plans_considered == 4);
    CHECK(sequential.stats().budget_limited_plans == 3);
    CHECK(sequential.stats().budget_rejections.emitted_row_limit == 3);
    CHECK(sequential.stats().persistence_limited_plans == 0);
    CHECK(sequential.stats().persistence_cache_hits == 0);

    for (const size_t width : std::array<size_t, 2>{1, 4}) {
        SequentialStructuredReducer parallel(42'102, interleaved_persistence_fixture());
        const auto parallel_run =
            parallel.reduce_budgeted_parallel(budget, parallel_options(width, 4));
        CHECK(parallel_run == sequential_run);
        CHECK(state_equal(capture_state(parallel), sequential_state));
    }
}

[[nodiscard]] std::vector<Relation> materialization_backfill_fixture() {
    const auto limited_pivot = rational_key(653);
    const auto ready_pivot = rational_key(659);
    Relation limited = make_relation(30, {limited_pivot});
    limited.rational_factors.push_back(0);
    return {
        std::move(limited),
        make_relation(31, {limited_pivot}),
        make_relation(32, {ready_pivot}),
        make_relation(33, {ready_pivot}),
    };
}

void test_materialization_mask_and_ready_backfill() {
    ReducerSnapshot baseline;
    StructuredReductionRunResult baseline_run;
    bool have_baseline = false;
    for (const size_t width : std::array<size_t, 2>{1, 2}) {
        SequentialStructuredReducer reducer(42'201, materialization_backfill_fixture());
        auto budget = generous_budget();
        budget.max_factor_entries_per_side = 0;
        const auto run = reducer.reduce_budgeted_parallel(budget, parallel_options(width, 4));

        CHECK((run == StructuredReductionRunResult{0, 1, 1, 0,
                                                   StructuredReductionStopReason::BudgetLimit}));
        CHECK(reducer.incidence_epoch() == 2);
        CHECK(reducer.total_row_count() == 5);
        CHECK(reducer.active_row_ids() ==
              std::vector<StructuredRowId>(
                  {StructuredRowId{0}, StructuredRowId{1}, StructuredRowId{4}}));
        CHECK(source_mask(reducer.sources(StructuredRowId{4}), reducer.corpus()) == SourceMask{12});
        CHECK(reducer.lp_keys(StructuredRowId{4}).empty());
        const auto& stats = reducer.stats();
        CHECK(stats.two_way_merges == 1);
        CHECK(stats.budgeted_runs == 1);
        CHECK(stats.planning_passes == 2);
        CHECK(stats.candidate_plans_considered == 3);
        CHECK(stats.budget_limited_plans == 2);
        CHECK(stats.budget_rejections.materialization_limit == 2);
        CHECK(stats.peak_prepared_payload_entries == 4);
        CHECK(stats.accepted_lp_fill_growth == 0);
        CHECK(stats.output_rows == 3);
        CHECK(stats.stop_reason == StructuredReductionStopReason::BudgetLimit);

        const auto state = capture_state(reducer);
        if (!have_baseline) {
            baseline = state;
            baseline_run = run;
            have_baseline = true;
        } else {
            CHECK(run == baseline_run);
            CHECK(state_equal(state, baseline));
        }
    }
}

[[nodiscard]] std::vector<Relation> three_ready_pairs_fixture() {
    const auto p = rational_key(751);
    const auto q = rational_key(757);
    const auto r = rational_key(761);
    return {
        make_relation(40, {p}), make_relation(41, {p}), make_relation(42, {q}),
        make_relation(43, {q}), make_relation(44, {r}), make_relation(45, {r}),
    };
}

void test_candidate_examination_mid_wave() {
    SequentialStructuredReducer prefix(42'301, three_ready_pairs_fixture());
    auto prefix_budget = generous_budget();
    prefix_budget.max_candidate_examinations_per_pass = 1;
    const auto prefix_run = prefix.reduce_budgeted_parallel(prefix_budget, parallel_options(3, 4));
    CHECK((prefix_run ==
           StructuredReductionRunResult{0, 3, 3, 0, StructuredReductionStopReason::NoCandidates}));
    CHECK(prefix.incidence_epoch() == 4);
    CHECK(prefix.total_row_count() == 9);
    CHECK(
        prefix.active_row_ids() ==
        std::vector<StructuredRowId>({StructuredRowId{6}, StructuredRowId{7}, StructuredRowId{8}}));
    CHECK(source_mask(prefix.sources(StructuredRowId{6}), prefix.corpus()) == SourceMask{3});
    CHECK(source_mask(prefix.sources(StructuredRowId{7}), prefix.corpus()) == SourceMask{12});
    CHECK(source_mask(prefix.sources(StructuredRowId{8}), prefix.corpus()) == SourceMask{48});
    CHECK(prefix.stats().planning_passes == 4);
    CHECK(prefix.stats().candidate_plans_considered == 3);
    CHECK(prefix.stats().candidate_limit_stops == 0);
    CHECK(prefix.stats().budget_limit_stops == 0);

    const auto p = rational_key(769);
    const auto q = rational_key(773);
    SequentialStructuredReducer rejected(42'302, {make_relation(50, {p}), make_relation(51, {p}),
                                                  make_relation(52, {q}), make_relation(53, {q})});
    const auto rejected_before = capture_state(rejected);
    auto rejected_budget = generous_budget();
    rejected_budget.max_candidate_examinations_per_pass = 1;
    rejected_budget.max_source_atoms_per_output = 1;
    const auto rejected_run =
        rejected.reduce_budgeted_parallel(rejected_budget, parallel_options(2, 4));
    CHECK((rejected_run ==
           StructuredReductionRunResult{0, 0, 0, 0, StructuredReductionStopReason::BudgetLimit}));
    CHECK(logical_equal(capture_state(rejected), rejected_before));
    CHECK(rejected.stats().planning_passes == 1);
    CHECK(rejected.stats().candidate_plans_considered == 1);
    CHECK(rejected.stats().budget_limited_plans == 1);
    CHECK(rejected.stats().budget_rejections.source_limit == 1);
    CHECK(rejected.stats().candidate_limit_stops == 1);
    CHECK(rejected.stats().budget_limit_stops == 1);
    CHECK(rejected.stats().peak_prepared_payload_entries == 0);
}

void test_all_persistence_and_empty_plan_preserve_epoch() {
    const auto p = rational_key(701);
    const auto q = rational_key(709);
    SequentialStructuredReducer limited(
        43'001, {persistence_heavy_relation(30, p), persistence_heavy_relation(31, p),
                 persistence_heavy_relation(32, q), persistence_heavy_relation(33, q)});
    const auto limited_before = capture_state(limited);
    const auto limited_run =
        limited.reduce_budgeted_parallel(generous_budget(), parallel_options(2, 4));
    CHECK((limited_run == StructuredReductionRunResult{
                              0, 0, 0, 0, StructuredReductionStopReason::PersistenceLimit}));
    const auto limited_after = capture_state(limited);
    CHECK(logical_equal(limited_after, limited_before));
    CHECK(limited_after.stats.persistence_limited_plans == 2);
    CHECK(limited_after.stats.persistence_cache_hits == 0);
    CHECK(limited_after.stats.budgeted_runs == 1);
    CHECK(limited_after.stats.planning_passes == 1);
    CHECK(limited_after.stats.candidate_plans_considered == 2);
    CHECK(limited_after.stats.peak_prepared_payload_entries == 0);
    CHECK(limited_after.stats.output_rows == 4);
    CHECK(limited_after.stats.stop_reason == StructuredReductionStopReason::PersistenceLimit);

    SequentialStructuredReducer empty(43'002, {make_relation(40, {})});
    const auto empty_before = capture_state(empty);
    const auto empty_run =
        empty.reduce_budgeted_parallel(generous_budget(), parallel_options(3, 4));
    CHECK((empty_run ==
           StructuredReductionRunResult{0, 0, 0, 0, StructuredReductionStopReason::NoCandidates}));
    const auto empty_after = capture_state(empty);
    CHECK(logical_equal(empty_after, empty_before));
    CHECK(empty_after.stats.budgeted_runs == 1);
    CHECK(empty_after.stats.planning_passes == 1);
    CHECK(empty_after.stats.candidate_plans_considered == 0);
    CHECK(empty_after.stats.output_rows == 1);
    CHECK(empty_after.stats.stop_reason == StructuredReductionStopReason::NoCandidates);
}

void test_successful_batch_then_peel_advances_epoch_twice() {
    const auto p = rational_key(809);
    const auto q = rational_key(811);
    SequentialStructuredReducer reducer(
        44'001, {make_relation(50, {p, q}), make_relation(51, {p, q}), make_relation(52, {q})});
    const uint64_t initial_epoch = reducer.incidence_epoch();
    const auto run = reducer.reduce_budgeted_parallel(generous_budget(), parallel_options(3, 4));

    CHECK((run ==
           StructuredReductionRunResult{1, 1, 1, 0, StructuredReductionStopReason::NoCandidates}));
    CHECK(reducer.incidence_epoch() == initial_epoch + 2);
    CHECK(reducer.total_row_count() == 4);
    CHECK(reducer.active_row_ids() == std::vector<StructuredRowId>({StructuredRowId{3}}));
    CHECK(source_mask(reducer.sources(StructuredRowId{3}), reducer.corpus()) == SourceMask{3});
    CHECK(reducer.lp_keys(StructuredRowId{3}).empty());
    CHECK(reducer.stats().singleton_rows_removed == 1);
    CHECK(reducer.stats().two_way_merges == 1);
    CHECK(reducer.stats().planning_passes == 2);
    CHECK(reducer.stats().candidate_plans_considered == 1);
    CHECK(reducer.stats().output_rows == 1);
}

void test_zero_width_and_worker_fail_without_mutation() {
    const auto p = rational_key(907);
    SequentialStructuredReducer reducer(45'001, {make_relation(60, {p}), make_relation(61, {p})});
    const auto before = capture_state(reducer);

    expect_error(StructuredReductionErrorCode::InvalidInput, [&] {
        (void)reducer.reduce_budgeted_parallel(generous_budget(), parallel_options(0, 1));
    });
    CHECK(state_equal(capture_state(reducer), before));

    expect_error(StructuredReductionErrorCode::InvalidInput, [&] {
        (void)reducer.reduce_budgeted_parallel(generous_budget(), parallel_options(1, 0));
    });
    CHECK(state_equal(capture_state(reducer), before));
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
    run_test("randomized worker and sequential oracles",
             test_randomized_worker_and_sequential_oracles);
    run_test("mixed width and worker determinism", test_mixed_width_and_worker_determinism);
    run_test("exact and one-over budget boundaries", test_exact_and_one_over_budget_boundaries);
    run_test("interleaved persistence publish and cache",
             test_interleaved_persistence_publish_and_cache_folding);
    run_test("cumulative budget precedes later persistence slots",
             test_cumulative_budget_precedes_later_persistence_slots);
    run_test("materialization mask and ready backfill",
             test_materialization_mask_and_ready_backfill);
    run_test("candidate examination mid-wave", test_candidate_examination_mid_wave);
    run_test("all persistence and empty plan epoch",
             test_all_persistence_and_empty_plan_preserve_epoch);
    run_test("successful batch then peel epoch",
             test_successful_batch_then_peel_advances_epoch_twice);
    run_test("invalid zero width and worker", test_zero_width_and_worker_fail_without_mutation);

    if (failures != 0) {
        std::cerr << failures << " tests failed after " << checks << " checks\n";
        return 1;
    }
    std::cout << "structured parallel driver: " << checks << " checks passed\n";
    return 0;
}
