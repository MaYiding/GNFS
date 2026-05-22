#pragma once

// LP key splitmix64 hash mixing helper (W11 T5).
//
// Scope
// -----
// The GNFS filter Phase 4 frequently uses `std::unordered_set<uint64_t>`
// or `std::unordered_map<uint64_t, ...>` keyed by LP (large prime) keys.
// The LP keys are typically packed as `(prime_id << 1) | side` or similar
// simple bit-packings. These keys cluster:
//
//   * Small primes' bit patterns are dense in low bits.
//   * The side bit is fixed (0 = rational, 1 = algebraic).
//   * Sequential prime IDs differ only in low bits.
//
// On libstdc++ / libc++, `std::hash<uint64_t>` is mostly the identity
// function. Plugged into `std::unordered_set`, the identity hash maps
// these clustered keys to a small subset of buckets, producing chained
// collisions and elevated probe counts. The hash-set's amortised O(1)
// guarantees degrade.
//
// This helper provides a `mix_lp_key()` function based on the well-known
// splitmix64 bit mixer (used in Java's SplittableRandom, many random
// generators, and reference Bloom-filter implementations). One multiply
// + xorshift per round produces output that passes the BigCrush
// statistical test battery. The mixer is deterministic, stateless, and
// allocation-free; `constexpr` so callers can evaluate at compile time.
//
// The helper also provides:
//
//   * `maybe_mix_lp_key(key)` — a gate-aware wrapper. When the ENV
//     `GNFS_FILTER_LP_HASH_MIX` is set to "0"/"off" (ForceOff), returns
//     the input unchanged. Otherwise returns `mix_lp_key(key)`.
//   * `LpKeyHash` — a `std::hash`-compatible functor that internally
//     calls `maybe_mix_lp_key`. Suitable as the second template argument
//     to `std::unordered_set<uint64_t, LpKeyHash>` /
//     `std::unordered_map<uint64_t, V, LpKeyHash>`.
//
// Correctness invariant
// ---------------------
// `mix_lp_key` is a deterministic pure function of its input. Same input
// always yields same output. No two distinct uint64 inputs are
// guaranteed to produce distinct outputs (no perfect hash claim), but
// the splitmix64 mixer has been shown empirically to scatter input
// patterns very well — clustered LP keys land in well-spread hash
// buckets.
//
// This helper is **standalone**: existing callers in `filter.hpp` /
// `clique_merger.hpp` are NOT modified. Use is opt-in for future wire-in.
//
// ENV gate
// --------
//   GNFS_FILTER_LP_HASH_MIX
//
//   * unset / empty / "auto" / any unrecognized token → Auto (enabled)
//   * "0" / "off"                                     → ForceOff
//   * "1" / "on"                                      → ForceOn
//
// The cached result is resolved on the first call via `std::once_flag`;
// tests can reload it through
// `lp_hash_mix_reset_env_cache_for_testing()`.
//
// Note: ForceOn and Auto are observationally identical (both enable
// mixing). The distinction exists so callers / future wire-in code can
// distinguish "user explicitly opted in" from "default behaviour".
//
// Algorithmic note (splitmix64)
// -----------------------------
// One splitmix64 round:
//
//   uint64_t z = key + 0x9E3779B97F4A7C15ULL;  // golden ratio fract
//   z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
//   z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
//   z = z ^ (z >> 31);
//   return z;
//
// The three magic constants come from Stafford's `Mix13` variant
// (https://zimbry.blogspot.com/2011/09/better-bit-mixing-improving-on.html),
// chosen to maximise avalanche (each output bit depends on roughly half
// of all input bits) while keeping the routine to two 64-bit multiplies
// and three xorshifts. The golden-ratio addition steers identity inputs
// (`key == 0`) away from the all-zero fixed point of pure xorshift mixers.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace gnfs::relation {

/// Mixing mode for the LP key hash gate.
enum class LpHashMixMode {
    Auto,      ///< Unspecified or unrecognized ENV (mixing enabled).
    ForceOff,  ///< ENV = "0" / "off" (mixing disabled).
    ForceOn,   ///< ENV = "1" / "on" (mixing enabled).
};

namespace detail {

inline std::atomic<int>& cached_lp_hash_mix_mode() noexcept {
    // 0 = Auto, 1 = ForceOff, 2 = ForceOn
    static std::atomic<int> v{0};
    return v;
}

inline std::once_flag& cached_lp_hash_mix_flag() noexcept {
    static std::once_flag f;
    return f;
}

/// Strict ENV parser. Recognises a small token set; anything outside
/// that set falls back to Auto. Matches the helper-style "auto / 0 / 1"
/// triad used by SIMD and parallel helpers in this codebase.
[[nodiscard]] inline int resolve_lp_hash_mix_mode_from_env() noexcept {
    const char* v = std::getenv("GNFS_FILTER_LP_HASH_MIX");
    if (v == nullptr || v[0] == '\0') {
        return 0;  // Auto
    }
    // Token-based recognition. Lowercase comparison; any deviation
    // (whitespace, case mix beyond what we list, garbage) falls back to
    // Auto so a typo never silently changes behaviour.
    if (std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0) {
        return 1;  // ForceOff
    }
    if (std::strcmp(v, "1") == 0 || std::strcmp(v, "on") == 0) {
        return 2;  // ForceOn
    }
    // "auto" or anything else → Auto.
    return 0;
}

}  // namespace detail

/// Returns the cached parsed ENV gate mode for the LP hash mixer.
[[nodiscard]] inline LpHashMixMode lp_hash_mix_mode() noexcept {
    std::call_once(detail::cached_lp_hash_mix_flag(), []() noexcept {
        detail::cached_lp_hash_mix_mode().store(
            detail::resolve_lp_hash_mix_mode_from_env(),
            std::memory_order_relaxed);
    });
    const int v = detail::cached_lp_hash_mix_mode().load(
        std::memory_order_relaxed);
    switch (v) {
        case 1:
            return LpHashMixMode::ForceOff;
        case 2:
            return LpHashMixMode::ForceOn;
        default:
            return LpHashMixMode::Auto;
    }
}

/// True when mixing is active (Auto and ForceOn both enable mixing,
/// only ForceOff disables it).
[[nodiscard]] inline bool lp_hash_mix_enabled() noexcept {
    return lp_hash_mix_mode() != LpHashMixMode::ForceOff;
}

/// Reload the cached ENV gate. Intended solely for unit tests that
/// toggle the ENV between scenarios.
inline void lp_hash_mix_reset_env_cache_for_testing() noexcept {
    detail::cached_lp_hash_mix_mode().store(
        detail::resolve_lp_hash_mix_mode_from_env(),
        std::memory_order_relaxed);
    // Mark the once_flag as completed so a subsequent
    // `lp_hash_mix_mode()` call does not overwrite us.
    std::call_once(detail::cached_lp_hash_mix_flag(), []() noexcept {
        // No-op: state already set above.
    });
}

/// Single splitmix64 mixing round. Deterministic, branch-free,
/// allocation-free. `constexpr` so callers can evaluate at compile
/// time. `noexcept` because the body only does integer arithmetic.
///
/// The output of `mix_lp_key(key)` for a few canonical inputs:
///   mix_lp_key(0x0000000000000000) = 0xE220A8397B1DCDAF
///   mix_lp_key(0x0000000000000001) = 0x910A2DEC89025CC1
///   mix_lp_key(0x00000000DEADBEEF) = 0x4ADFB90F68C9EB9B
///   mix_lp_key(0xFFFFFFFFFFFFFFFF) = 0xE4D971771B652C20
[[nodiscard]] constexpr std::uint64_t mix_lp_key(std::uint64_t key) noexcept {
    std::uint64_t z = key + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return z;
}

/// Gate-aware wrapper. Caller sees `mix_lp_key(key)` unless the user
/// has set `GNFS_FILTER_LP_HASH_MIX=0` (or "off"), in which case the
/// input is returned unchanged.
[[nodiscard]] inline std::uint64_t
maybe_mix_lp_key(std::uint64_t key) noexcept {
    return lp_hash_mix_enabled() ? mix_lp_key(key) : key;
}

/// `std::hash`-compatible functor for uint64 keys that internally
/// routes through `maybe_mix_lp_key`. Suitable as the second template
/// argument of `std::unordered_set<uint64_t, LpKeyHash>` /
/// `std::unordered_map<uint64_t, V, LpKeyHash>`.
///
/// On a libstdc++ / libc++ implementation where `std::hash<uint64_t>`
/// is mostly identity, swapping the hash functor to `LpKeyHash` should
/// produce a more uniform bucket distribution for clustered LP-key
/// inputs (`(prime_id << 1) | side` etc.). The functor is `constexpr`-
/// callable through `mix_lp_key`, though the ENV gate makes the
/// actual lookup non-`constexpr`.
struct LpKeyHash {
    [[nodiscard]] std::size_t operator()(std::uint64_t key) const noexcept {
        return static_cast<std::size_t>(maybe_mix_lp_key(key));
    }
};

}  // namespace gnfs::relation
