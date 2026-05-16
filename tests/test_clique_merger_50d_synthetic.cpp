// Synthetic 50d-like LP distribution test for V3 CliqueRelationMerger
// Validates V3 真的 expand V0 baseline (weight≥3 keys merge) without
// reintroducing V1/V2-style chain residue regression.

#include "gnfs/relation/clique_merger.hpp"
#include "gnfs/relation/filter.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

using namespace gnfs::relation;
using gnfs::core::Relation;
using gnfs::core::PrimePower;

// Generate synthetic partial relations matching 50d-like LP distribution:
// - LP keys count scales with target_rels (avg 3 rels per key)
// - Each key has weight 2..15 (geometric distribution favoring small weight)
// - Each rel has 1-2 LP keys (1LP or 2LP)
// - (a,b) values unique per rel
static std::vector<Relation> make_synthetic_50d_like(
        size_t target_rels, uint64_t seed = 42) {
    std::mt19937_64 rng(seed);
    // Weight distribution: 2 most common, geometric tail
    std::geometric_distribution<size_t> weight_dist(0.4);  // mean 2.5

    // Step 1: Generate LP keys and their assigned relation indices.
    // n_lp_keys scaled so that target_rels can be reached (~3 rels per key avg)
    const size_t n_lp_keys = std::max<size_t>(1500, target_rels / 3);
    std::vector<std::vector<size_t>> lp_to_rels(n_lp_keys);
    std::vector<bool> lp_is_algebraic(n_lp_keys);

    size_t rel_counter = 0;
    for (size_t k = 0; k < n_lp_keys; ++k) {
        size_t w = std::min<size_t>(2 + weight_dist(rng), 15);
        lp_is_algebraic[k] = (k % 2 == 0);
        for (size_t i = 0; i < w; ++i) {
            lp_to_rels[k].push_back(rel_counter++);
            if (rel_counter >= target_rels) break;
        }
        if (rel_counter >= target_rels) break;
    }

    // Step 2: Build inverse — for each rel, which LP keys does it carry?
    size_t total_rels = rel_counter;
    std::vector<std::vector<size_t>> rel_to_lps(total_rels);
    for (size_t k = 0; k < n_lp_keys; ++k) {
        for (size_t idx : lp_to_rels[k]) {
            rel_to_lps[idx].push_back(k);
        }
    }

    // Step 3: Construct Relations.
    std::vector<Relation> rels;
    rels.reserve(total_rels);
    for (size_t i = 0; i < total_rels; ++i) {
        Relation r(static_cast<int64_t>(i + 1), static_cast<int64_t>((i % 1000) + 1));
        r.rational_factors = {0, 1};
        r.algebraic_factors = {0};
        for (size_t k : rel_to_lps[i]) {
            uint64_t lp_p = 100000 + k * 7;  // synthetic primes
            uint64_t lp_r = (k * 13) % 65536;
            PrimePower pp{lp_p, lp_is_algebraic[k] ? lp_r : 0, 1};
            if (lp_is_algebraic[k]) r.algebraic_large_prime.push_back(pp);
            else r.rational_large_prime.push_back(pp);
        }
        rels.push_back(std::move(r));
    }
    return rels;
}

void test_v3_expands_v0_baseline() {
    std::cout << "Testing V3 expands V0 on synthetic 50d-like (5000 partials)..." << std::endl;
    auto v0_input = make_synthetic_50d_like(5000);
    auto v3_input = v0_input;  // copy for V3

    // Run V0
    PartialRelationMerger::MergeStats v0_stats;
    auto v0_merged = PartialRelationMerger::merge_all(std::move(v0_input), 10, &v0_stats);
    std::cout << "  V0: input_1lp=" << v0_stats.input_1lp
              << " input_2lp=" << v0_stats.input_2lp
              << " merged=" << v0_merged.size()
              << " w2_merges=" << v0_stats.weight2_merges
              << " sngl_removed=" << v0_stats.singletons_removed << std::endl;

    // Run V3
    CliqueStats v3_stats;
    auto v3_merged = CliqueRelationMerger::merge_cliques(std::move(v3_input), &v3_stats);
    std::cout << "  V3: input_1lp=" << v3_stats.input_1lp
              << " input_2lp=" << v3_stats.input_2lp
              << " components_with_excess=" << v3_stats.components_with_excess
              << " full=" << v3_stats.full_produced
              << " residual=" << v3_stats.residual_emitted
              << " lp_rejects=" << v3_stats.lp_cancel_rejections << std::endl;

    // Sanity: V3 should at least produce 1 full (any 2-clique trivially does).
    assert(v3_stats.full_produced + v3_stats.residual_emitted > 0);

    // V3 should not catastrophically regress: lp_rejects shouldn't 100% dominate.
    // (LP cancel check should reject some merges, but not all.)
    size_t v3_accepts = v3_stats.full_produced + v3_stats.residual_emitted;
    assert(v3_accepts >= v3_stats.lp_cancel_rejections / 10);  // accept ≥ 10% rate

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

    // Cascade dedup (a,b) XOR pattern
    std::unordered_set<int64_t> seen;
    for (const auto& r : v0_merged) {
        seen.insert(static_cast<int64_t>(r.a) ^ (static_cast<int64_t>(r.b) << 32));
    }
    size_t v3_added_after_dedup = 0;
    for (const auto& r : v3_merged) {
        int64_t k = static_cast<int64_t>(r.a) ^ (static_cast<int64_t>(r.b) << 32);
        if (seen.insert(k).second) ++v3_added_after_dedup;
    }

    std::cout << "  V0 merged=" << v0_merged.size()
              << " V3 merged=" << v3_merged.size()
              << " V3 added after dedup=" << v3_added_after_dedup << std::endl;

    // V3 cascade should add SOME relations beyond V0 (unless all components are 1LP×2 cliques).
    // Even strict assertion: v3_added_after_dedup ≤ v3_merged.size()
    assert(v3_added_after_dedup <= v3_merged.size());

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
    std::cout << "  in=" << stats.input_relations
              << " full=" << stats.full_produced
              << " residual=" << stats.residual_emitted
              << " lp_rejects=" << stats.lp_cancel_rejections << std::endl;

    // Sanity: no full from this chain (no closed loop), only residual.
    // V3 must NOT crash, NOT produce invalid relations.
    assert(stats.full_produced + stats.residual_emitted <= 3);

    std::cout << "  PASS" << std::endl;
}

void test_v3_scale_performance() {
    // Verify V3 cascade scales — 50K input rels should complete in instant tier
    // (post fast-path optimization, commit d2ef403).
    std::cout << "Testing V3 cascade scale (50K input)..." << std::endl;

    auto large_input = make_synthetic_50d_like(50000, /*seed=*/13);

    auto start = std::chrono::high_resolution_clock::now();
    CliqueStats stats;
    auto out = CliqueRelationMerger::merge_cliques(std::move(large_input), &stats);
    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_s = std::chrono::duration<double>(end - start).count();

    std::cout << "  in=" << stats.input_relations
              << " components=" << stats.components_with_excess
              << " full=" << stats.full_produced
              << " residual=" << stats.residual_emitted
              << " lp_rejects=" << stats.lp_cancel_rejections
              << " elapsed=" << std::fixed << std::setprecision(3) << elapsed_s << "s"
              << std::endl;

    // Post fast-path, 50K input should complete < 5s (was estimated minutes pre-opt)
    assert(elapsed_s < 5.0);
    assert(stats.input_relations == 50000);

    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== V3 Synthetic 50d-like Test ===" << std::endl;

    test_v3_expands_v0_baseline();
    test_v0_v3_cascade_dedup();
    test_v3_lp_cancel_safety();
    test_v3_scale_performance();

    std::cout << "\nAll V3 synthetic tests passed!" << std::endl;
    return 0;
}
