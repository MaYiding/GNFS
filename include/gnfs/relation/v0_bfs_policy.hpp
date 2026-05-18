#pragma once

// V0 BFS chain-merge policy decision (BACKLOG #1 size-aware default).
//
// Extracted from src/api/pipeline.cpp Pipeline::filter() to make the V0_BFS
// activation decision testable in isolation. Behavior preserves and extends
// the original env-only gate (commits 086afb2 + d50fd61) with a size-aware
// default-ON for lp_bits ≥ 22.
//
// Motivation (BACKLOG #1 step 11 empirical, 2026-05-18):
//   50d Round 1 LP-key weight histogram showed 49% of LP keys are weight≥3
//   (chain merge territory). V0 PartialRelationMerger handles only weight=2
//   simple matches — it misses half the LP graph. V0_BFS (CliqueRelationMerger
//   BFS spanning tree) handles weight≥3 chains correctly.
//
// Size-aware fallback (CLAUDE.md 跨 bit-size 验证 铁律):
//   Small LP spaces (lp_bits ≤ 20, 25d/81-bit) have BFS chain merge produce
//   ~87% residual partials → matrix LP cols 大幅增加 → BL/BW 找不到 deps.
//   实测 test_regression_gate Level 4 (81-bit) V0_BFS=1 FAIL "no deps found".
//   故 small LP space 永 fallback to V0 standard, 即使 ENV explicit force-on.

#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "ooc_policy.hpp"  // For estimate_lp_bits

namespace gnfs::relation {

struct V0BfsPolicy {
    bool enabled;            // V0_BFS active (CliqueRelationMerger as main V0 path)
    bool env_force_failed;   // ENV=1 but lp_bits<22 → fallback warn emitted
    std::string_view reason;
};

// Decide whether V0 main path uses BFS chain merge (CliqueRelationMerger)
// instead of standard PartialRelationMerger.
//
//   v0_bfs_env: GNFS_V0_BFS env value (nullptr if unset)
//   large_prime_bound: params_.large_prime_bound
//
// Semantics:
//   ENV "0" → OFF (explicit opt-out, V0 standard always)
//   ENV "1" → ON if lp_bits≥22, else env_force_failed=true (fallback to V0 standard)
//   ENV unset → size-aware default:
//     lp_bits ≥ 22: ON (50d+ 49% w3+ chain merge territory, BACKLOG #1 step 11)
//     lp_bits <  22: OFF (25d/40-bit/81-bit unchanged, gate test_regression_gate Level 4 PASS guaranteed)
//   ENV "" or non-numeric: std::atoi returns 0 → treated as explicit OFF
inline V0BfsPolicy decide_v0_bfs_policy(const char* v0_bfs_env,
                                         uint64_t large_prime_bound) noexcept {
    const size_t lp_bits = estimate_lp_bits(large_prime_bound);
    const bool env_set = (v0_bfs_env != nullptr);
    const int env_int = env_set ? std::atoi(v0_bfs_env) : 0;
    const bool env_explicit_off = env_set && env_int == 0;
    const bool env_explicit_on = env_set && env_int == 1;
    const bool large_lp = lp_bits >= 22;

    // Size-aware default: ON when lp_bits≥22 AND not explicit opt-out.
    const bool size_aware_default = !env_explicit_off && large_lp;
    // Explicit force-on takes effect only if size gate passes.
    const bool enabled = (env_explicit_on && large_lp) || size_aware_default;
    // ENV=1 but small LP — record fallback warning condition.
    const bool env_force_failed = env_explicit_on && !large_lp;

    std::string_view reason;
    if (env_force_failed) {
        reason = "GNFS_V0_BFS=1 but lp_bits<22 (fallback to V0 standard)";
    } else if (env_explicit_on) {
        reason = "GNFS_V0_BFS=1";
    } else if (size_aware_default) {
        reason = "size-aware default (lp_bits>=22)";
    } else if (env_explicit_off) {
        reason = "GNFS_V0_BFS=0";
    } else {
        reason = "default off (lp_bits<22)";
    }

    return {enabled, env_force_failed, reason};
}

}  // namespace gnfs::relation
