#pragma once

// OOC relation streaming policy decision (BACKLOG #1 size-aware default).
//
// Extracted from src/api/pipeline.cpp sieve_and_collect to make the decision
// testable in isolation. Behavior preserves commit 0fac325 (size-aware default
// enabling 50d+ Round 2+ OOM avoidance) exactly.

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace gnfs::relation {

struct OocPolicy {
    bool enabled;
    std::string_view reason;
};

// floor(log2) approximation for lp_bits estimate from large_prime_bound.
// Examples: 1<<20 -> 20, 1<<22 -> 22 (sweet spot), 1<<23 -> 23 (50d), 1<<26 -> 26 (60d).
inline size_t estimate_lp_bits(uint64_t large_prime_bound) noexcept {
    size_t bits = 0;
    for (uint64_t b = large_prime_bound; b > 1; b >>= 1) ++bits;
    return bits;
}

// Decide whether to enable OOC streaming for the sieve phase.
//   ooc_env: GNFS_OOC_RELATIONS env value (nullptr if unset)
//   large_prime_bound: params_.large_prime_bound
//
// Preserves pipeline.cpp commit 0fac325 std::atoi semantics:
//   ENV "0" → OFF (explicit opt-out)
//   ENV "1" → ON (explicit opt-in, bypasses size gate)
//   ENV unset OR ENV other → size-aware default
//     lp_bits >= 22: ON (50d+ Round 2 OOM avoidance)
//     lp_bits <  22: OFF (25d / 40-bit / 81-bit unchanged)
//   ENV "" or non-numeric: std::atoi returns 0 → treated as explicit OFF
inline OocPolicy decide_ooc_policy(const char* ooc_env,
                                    uint64_t large_prime_bound) noexcept {
    const size_t lp_bits = estimate_lp_bits(large_prime_bound);
    const bool env_set = (ooc_env != nullptr);
    const int env_int = env_set ? std::atoi(ooc_env) : 0;
    const bool env_explicit_off = env_set && env_int == 0;
    const bool env_explicit_on = env_set && env_int == 1;
    const bool size_aware_default = !env_explicit_off && lp_bits >= 22;
    const bool enabled = env_explicit_on || size_aware_default;

    std::string_view reason;
    if (env_explicit_on) {
        reason = "GNFS_OOC_RELATIONS=1";
    } else if (size_aware_default) {
        reason = "size-aware default (lp_bits>=22)";
    } else if (env_explicit_off) {
        reason = "GNFS_OOC_RELATIONS=0";
    } else {
        reason = "default off (lp_bits<22)";
    }

    return {enabled, reason};
}

}  // namespace gnfs::relation
