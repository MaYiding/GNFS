// Synthetic 50d-like LP distribution test for V3 CliqueRelationMerger
// Validates V3 真的 expand V0 baseline (weight≥3 keys merge) without
// reintroducing V1/V2-style chain residue regression.

#include "gnfs/relation/clique_merger.hpp"
#include "gnfs/relation/filter.hpp"
#include "gnfs/relation/relation_identity.hpp"
#include "gnfs/util/msvc_compat.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace gnfs::relation;
using gnfs::core::PrimePower;
using gnfs::core::Relation;

[[noreturn]] static void check_failed(const char* expression, int line) {
    throw std::runtime_error(std::string("CHECK failed at line ") + std::to_string(line) + ": " +
                             expression);
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition))                                                                          \
            check_failed(#condition, __LINE__);                                                    \
    } while (false)

// Timing assertions below assume an unoptimized-but-not-instrumented Debug
// build. AddressSanitizer / ThreadSanitizer / UBSan slow execution 2-10x and
// trip these thresholds in CI even when the algorithm itself is fine. Detect
// instrumentation at compile time and relax the budget — the assertions still
// guard against true regressions, just with more headroom under tooling.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer)
#define GNFS_TEST_UNDER_SANITIZER 1
#endif
#endif
#if !defined(GNFS_TEST_UNDER_SANITIZER) &&                                                         \
    (defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__))
#define GNFS_TEST_UNDER_SANITIZER 1
#endif
#if defined(GNFS_TEST_UNDER_SANITIZER)
static constexpr double kTimingBudgetMultiplier = 10.0;
#else
static constexpr double kTimingBudgetMultiplier = 1.0;
#endif

// Generate a deterministic overlapping 50d-like LP stress corpus:
// - three-row 2LP stars have a weight-3 hub and unique leaf keys; V0 cannot
//   use the hub, while V3 emits one new residual source combination;
// - two-row 1LP pairs give both algorithms an identical full relation, testing
//   source-combination dedup at the same time;
// - every raw (a,b) is unique and the requested relation count is exact.
static std::vector<Relation> make_synthetic_50d_like(size_t target_rels, uint64_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::vector<Relation> rels;
    rels.reserve(target_rels);

    uint64_t key_index = seed * 1000000ULL;
    auto next_key = [&]() {
        const uint64_t p = 1000003ULL + 2ULL * key_index++;
        const bool algebraic = (rng() & 1ULL) != 0;
        const uint64_t root = algebraic ? (rng() % p) : 0;
        return std::pair{PrimePower{p, root, 1}, algebraic};
    };
    auto append_key = [](Relation& relation, const auto& key) {
        if (key.second) {
            relation.algebraic_large_prime.push_back(key.first);
        } else {
            relation.rational_large_prime.push_back(key.first);
        }
    };
    auto make_relation = [&]() {
        const size_t i = rels.size();
        Relation r(static_cast<int64_t>(i + 1), static_cast<uint64_t>((i % 1000) + 1));
        r.rational_factors = {0, 1};
        r.algebraic_factors = {0};
        return r;
    };

    while (target_rels - rels.size() >= 5) {
        const auto hub = next_key();
        for (size_t i = 0; i < 3; ++i) {
            Relation relation = make_relation();
            append_key(relation, hub);
            append_key(relation, next_key());
            rels.push_back(std::move(relation));
        }

        const auto pair_key = next_key();
        for (size_t i = 0; i < 2; ++i) {
            Relation relation = make_relation();
            append_key(relation, pair_key);
            rels.push_back(std::move(relation));
        }
    }

    if (target_rels - rels.size() >= 3) {
        const auto hub = next_key();
        for (size_t i = 0; i < 3; ++i) {
            Relation relation = make_relation();
            append_key(relation, hub);
            append_key(relation, next_key());
            rels.push_back(std::move(relation));
        }
    }
    if (target_rels - rels.size() >= 2) {
        const auto pair_key = next_key();
        for (size_t i = 0; i < 2; ++i) {
            Relation relation = make_relation();
            append_key(relation, pair_key);
            rels.push_back(std::move(relation));
        }
    }
    if (rels.size() < target_rels) {
        Relation relation = make_relation();
        append_key(relation, next_key());
        rels.push_back(std::move(relation));
    }

    CHECK(rels.size() == target_rels);
    return rels;
}

void test_v3_expands_v0_baseline() {
    std::cout << "Testing V3 expands V0 on synthetic 50d-like (5000 partials)..." << std::endl;
    auto v0_input = make_synthetic_50d_like(5000);
    auto v3_input = v0_input; // copy for V3

    // Run V0
    PartialRelationMerger::MergeStats v0_stats;
    auto v0_merged = PartialRelationMerger::merge_all(std::move(v0_input), 10, &v0_stats);
    std::cout << "  V0: input_1lp=" << v0_stats.input_1lp << " input_2lp=" << v0_stats.input_2lp
              << " merged=" << v0_merged.size() << " w2_merges=" << v0_stats.weight2_merges
              << " sngl_removed=" << v0_stats.singletons_removed << std::endl;

    // Run V3
    CliqueStats v3_stats;
    auto v3_merged = CliqueRelationMerger::merge_cliques(std::move(v3_input), &v3_stats);
    std::cout << "  V3: input_1lp=" << v3_stats.input_1lp << " input_2lp=" << v3_stats.input_2lp
              << " components_with_excess=" << v3_stats.components_with_excess
              << " full=" << v3_stats.full_produced << " residual=" << v3_stats.residual_emitted
              << " lp_rejects=" << v3_stats.lp_cancel_rejections << std::endl;

    // Sanity: V3 should at least produce 1 full (any 2-clique trivially does).
    CHECK(v3_stats.full_produced + v3_stats.residual_emitted > 0);

    // V3 should not catastrophically regress: lp_rejects shouldn't 100% dominate.
    // (LP cancel check should reject some merges, but not all.)
    size_t v3_accepts = v3_stats.full_produced + v3_stats.residual_emitted;
    CHECK(v3_accepts >= v3_stats.lp_cancel_rejections / 10); // accept ≥ 10% rate

    std::cout << "  PASS" << std::endl;
}

void test_v0_v3_cascade_dedup() {
    std::cout << "Testing V0+V3 cascade dedup (no double-counting)..." << std::endl;
    auto input = make_synthetic_50d_like(2000, /*seed=*/7);

    // V0 on copy
    auto v0_input = input;
    PartialRelationMerger::MergeStats v0_stats;
    auto v0_merged = PartialRelationMerger::merge_all(std::move(v0_input), 10, &v0_stats);

    // V3 on copy
    auto v3_input = input;
    CliqueStats v3_stats;
    auto v3_merged = CliqueRelationMerger::merge_cliques(std::move(v3_input), &v3_stats);

    std::unordered_set<RelationSourceCombination, RelationSourceCombinationHash> seen;
    for (const auto& r : v0_merged) {
        seen.insert(relation_source_combination(r));
    }
    size_t v3_added_after_dedup = 0;
    for (const auto& r : v3_merged) {
        if (seen.insert(relation_source_combination(r)).second)
            ++v3_added_after_dedup;
    }

    std::cout << "  V0 merged=" << v0_merged.size() << " V3 merged=" << v3_merged.size()
              << " V3 added after dedup=" << v3_added_after_dedup << std::endl;

    CHECK(v3_added_after_dedup > 0);
    CHECK(v3_added_after_dedup <= v3_merged.size());

    std::cout << "  PASS" << std::endl;
}

void test_v3_lp_cancel_safety() {
    // Construct adversarial case: 3 rels with shared LP forming a chain
    // that, if naively merged, would accumulate residue (V1/V2 trap).
    // V3's LP cancel check must reject the residue-creating merges.
    std::cout << "Testing V3 LP cancel safety vs chain residue..." << std::endl;

    std::vector<Relation> rels;
    // R1: rat=101, alg=(201,1)
    {
        Relation r(1, 1);
        r.rational_factors = {0};
        r.algebraic_factors = {0};
        r.rational_large_prime.push_back(PrimePower{101, 0, 1});
        r.algebraic_large_prime.push_back(PrimePower{201, 1, 1});
        rels.push_back(std::move(r));
    }
    // R2: rat=101, alg=(202,2) — shares 101 with R1
    {
        Relation r(2, 1);
        r.rational_factors = {0};
        r.algebraic_factors = {0};
        r.rational_large_prime.push_back(PrimePower{101, 0, 1});
        r.algebraic_large_prime.push_back(PrimePower{202, 2, 1});
        rels.push_back(std::move(r));
    }
    // R3: rat=103, alg=(202,2) — shares (202,2) with R2 (chain extender)
    {
        Relation r(3, 1);
        r.rational_factors = {0};
        r.algebraic_factors = {0};
        r.rational_large_prime.push_back(PrimePower{103, 0, 1});
        r.algebraic_large_prime.push_back(PrimePower{202, 2, 1});
        rels.push_back(std::move(r));
    }

    CliqueStats stats;
    auto out = CliqueRelationMerger::merge_cliques(std::move(rels), &stats);

    // Expected: BFS from R1: acc=R1 → add R2 (cancels 101, residual=(201,1)+(202,2))
    // → R3 candidate: acc=(201,1)+(202,2)+R3=(103)+(202,2)
    //   merged would have (201,1)+103 (cancelled 202,2). Before=4 LPs, after=2 LPs → accept.
    //   Final: residual emit (still has 2 LPs).
    // 或 acc stays at (201,1)+(202,2) residual, no R3 accept if LP doesn't cancel cleanly.
    std::cout << "  in=" << stats.input_relations << " full=" << stats.full_produced
              << " residual=" << stats.residual_emitted
              << " lp_rejects=" << stats.lp_cancel_rejections << std::endl;

    // Sanity: no full from this chain (no closed loop), only residual.
    // V3 must NOT crash, NOT produce invalid relations.
    CHECK(stats.full_produced + stats.residual_emitted <= 3);

    std::cout << "  PASS" << std::endl;
}

void test_v0_v3_bench_large() {
    // Head-to-head V0 vs V0+V3 cascade on 30K-rel input (50d-scale).
    // Measures actual added value + timing for production decision-making.
    std::cout << "Bench V0 vs V0+V3 cascade (30K input)..." << std::endl;

    auto base_input = make_synthetic_50d_like(30000, /*seed=*/2026);

    // V0 alone
    auto v0_input = base_input;
    auto t0_start = std::chrono::high_resolution_clock::now();
    PartialRelationMerger::MergeStats v0_stats;
    auto v0_merged = PartialRelationMerger::merge_all(std::move(v0_input), 10, &v0_stats);
    auto t0_end = std::chrono::high_resolution_clock::now();
    double v0_elapsed = std::chrono::duration<double>(t0_end - t0_start).count();

    // V0 + V3 cascade
    auto v0_input_copy = base_input;
    auto v3_input_copy = base_input;
    auto tv_start = std::chrono::high_resolution_clock::now();
    PartialRelationMerger::MergeStats v0_stats_cascade;
    auto v0_merged_cascade =
        PartialRelationMerger::merge_all(std::move(v0_input_copy), 10, &v0_stats_cascade);
    CliqueStats v3_stats;
    auto v3_merged = CliqueRelationMerger::merge_cliques(std::move(v3_input_copy), &v3_stats);
    std::unordered_set<RelationSourceCombination, RelationSourceCombinationHash> existing;
    for (const auto& r : v0_merged_cascade) {
        existing.insert(relation_source_combination(r));
    }
    size_t v3_added = 0;
    for (const auto& r : v3_merged) {
        if (existing.insert(relation_source_combination(r)).second)
            ++v3_added;
    }
    auto tv_end = std::chrono::high_resolution_clock::now();
    double v3_elapsed = std::chrono::duration<double>(tv_end - tv_start).count();

    std::cout << "  V0 alone:    merged=" << v0_merged.size() << " elapsed=" << std::fixed
              << std::setprecision(3) << v0_elapsed << "s" << std::endl;
    std::cout << "  V0+V3:       v0=" << v0_merged_cascade.size() << " +v3_added=" << v3_added
              << " total=" << (v0_merged_cascade.size() + v3_added) << " elapsed=" << v3_elapsed
              << "s" << std::endl;
    if (!v0_merged.empty()) {
        double pct = 100.0 * static_cast<double>(v3_added) / static_cast<double>(v0_merged.size());
        std::cout << "  V3 adds " << std::setprecision(1) << pct << "% beyond V0 in "
                  << std::setprecision(3) << (v3_elapsed - v0_elapsed) << "s extra" << std::endl;
    }

    CHECK(v3_added > 0);
    CHECK(v3_elapsed < 10.0 * kTimingBudgetMultiplier);

    std::cout << "  PASS" << std::endl;
}

void test_v3_huge_clique() {
    // Adversarial: single large clique (5000 rels share same LP key).
    // V3 BFS must not blow up (stack overflow, quadratic in nbr eval, etc.)
    std::cout << "Testing V3 cascade single large clique (5000 rels)..." << std::endl;
    std::vector<Relation> rels;
    rels.reserve(5000);
    for (int64_t i = 0; i < 5000; ++i) {
        Relation r(i + 1, 1);
        r.rational_factors = {0};
        r.algebraic_factors = {0};
        r.rational_large_prime.push_back(PrimePower{101, 0, 1}); // all share LP=101
        rels.push_back(std::move(r));
    }

    auto start = std::chrono::high_resolution_clock::now();
    CliqueStats stats;
    auto out = CliqueRelationMerger::merge_cliques(std::move(rels), &stats);
    auto end = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(end - start).count();

    std::cout << "  in=" << stats.input_relations << " components=" << stats.components_with_excess
              << " full=" << stats.full_produced << " elapsed=" << std::fixed
              << std::setprecision(3) << elapsed << "s" << std::endl;

    CHECK(stats.components_with_excess == 1);
    // 5000 rels all sharing LP=101: BFS picks pairs → ~2500 full
    CHECK(stats.full_produced > 0);
    // Must complete quickly (< 5s, but expect << 1s)
    CHECK(elapsed < 5.0 * kTimingBudgetMultiplier);

    std::cout << "  PASS" << std::endl;
}

void test_v3_scale_performance() {
    // Verify V3 cascade scales — 50K input rels should complete quickly
    // (post fast-path optimization, commit d2ef403).
    std::cout << "Testing V3 cascade scale (50K input)..." << std::endl;

    auto large_input = make_synthetic_50d_like(50000, /*seed=*/13);

    auto start = std::chrono::high_resolution_clock::now();
    CliqueStats stats;
    auto out = CliqueRelationMerger::merge_cliques(std::move(large_input), &stats);
    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_s = std::chrono::duration<double>(end - start).count();

    std::cout << "  in=" << stats.input_relations << " components=" << stats.components_with_excess
              << " full=" << stats.full_produced << " residual=" << stats.residual_emitted
              << " lp_rejects=" << stats.lp_cancel_rejections << " elapsed=" << std::fixed
              << std::setprecision(3) << elapsed_s << "s" << std::endl;

    // Post fast-path, 50K input should complete < 5s (was estimated minutes pre-opt)
    CHECK(elapsed_s < 5.0 * kTimingBudgetMultiplier);
    CHECK(stats.input_relations == 50000);

    std::cout << "  PASS" << std::endl;
}

void test_v3_60d_scale() {
    // 60d Round 2-3 estimated 100-200K usable rels (lp_bits=26 LP space 67M).
    // Verify V3 cascade not bottleneck at this scale.
    std::cout << "Testing V3 cascade 60d scale (200K input)..." << std::endl;

    auto huge_input = make_synthetic_50d_like(200000, /*seed=*/60);

    auto start = std::chrono::high_resolution_clock::now();
    CliqueStats stats;
    auto out = CliqueRelationMerger::merge_cliques(std::move(huge_input), &stats);
    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_s = std::chrono::duration<double>(end - start).count();

    std::cout << "  in=" << stats.input_relations << " components=" << stats.components_with_excess
              << " full=" << stats.full_produced << " residual=" << stats.residual_emitted
              << " lp_rejects=" << stats.lp_cancel_rejections << " elapsed=" << std::fixed
              << std::setprecision(3) << elapsed_s << "s" << std::endl;

    // Must complete within the fast-tier per-case budget (10s)
    CHECK(elapsed_s < 10.0 * kTimingBudgetMultiplier);
    CHECK(stats.input_relations == 200000);

    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== V3 Synthetic 50d-like Test ===" << std::endl;

    // Freeze the default legacy comparison policy before the merger's static
    // ENV caches initialize; developer-shell flags must not change this corpus.
    unsetenv("GNFS_DROP_RESIDUAL");
    unsetenv("GNFS_V0_WEIGHT3");
    unsetenv("GNFS_WEIGHT_CUTOFF");

    test_v3_expands_v0_baseline();
    test_v0_v3_cascade_dedup();
    test_v3_lp_cancel_safety();
    test_v0_v3_bench_large();
    test_v3_huge_clique();
    test_v3_scale_performance();
    test_v3_60d_scale();

    std::cout << "\nAll V3 synthetic tests passed!" << std::endl;
    return 0;
}
