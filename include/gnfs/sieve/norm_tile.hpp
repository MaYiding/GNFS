#pragma once

// Row-tiled norm precomputation cache helper for the lattice sieve.
//
// Scope
// -----
// The lattice sieve evaluates the algebraic and rational norms |F(a, b)| /
// |G(a, b)| over every (i, j) lattice point of the current Special-Q region
// to seed the per-cell log-residual that subsequent prime updates subtract
// from. On a 50d+/60d region (`(i_max - i_min) * j_count` of tens of
// thousands of cells) the natural row-major evaluation order touches each
// cache line exactly once but pays the full DRAM round-trip on every
// `Polynomial::evaluate(a, b)` call because the polynomial coefficients
// and the per-row scratch buffers compete for the same L1 lines as the
// gather pass that immediately follows. Tiling the norm precomputation
// pass by 2^N rows keeps the polynomial coefficients hot in L1 for the
// duration of each tile, lets the per-row residual buffer stay in the
// nearest cache, and overlaps the next tile's coefficient prefetch with
// the current tile's evaluation.
//
// Two paths exist for any caller that opts in:
//
// 1. Untiled precomputation (default, `GNFS_SIEVE_NORM_TILE_BITS=0` or
//    unset): the existing row-major norm loop runs unchanged. No
//    additional code path, no behaviour change - the gate predicate
//    `norm_tile_enabled()` short-circuits to `false` and the helper
//    returns a tile size of 0.
//
// 2. Tiled precomputation (`GNFS_SIEVE_NORM_TILE_BITS=N` for N in [1, 8]):
//    callers partition the row range into floor(rows / 2^N) tiles of
//    2^N rows each (with a tail tile when rows is not a multiple of 2^N)
//    and run the polynomial evaluation for each tile to completion before
//    advancing to the next. The tile size is fixed at 2^N rows because
//    norm scratch buffers are naturally row-major and 2^N row strides
//    align with the cache line granularity. N is clamped to [0, 8] so the
//    worst-case tile is 256 rows; anything wider stops fitting comfortably
//    in L2 for realistic j widths and the precomputation pass starts
//    paying the cache-eviction cost the tiling was meant to amortise.
//
// Relation to GNFS_SIEVE_REGION_TILE_BITS
// ---------------------------------------
// This helper is *complementary but distinct* from the W6
// `GNFS_SIEVE_REGION_TILE_BITS` (`<gnfs/sieve/region_tile.hpp>`) gate.
// `region_tile_bits` tiles the *apply scan* phase (the second pass over
// `sieve_array_` that emits candidates) so the scan window stays in L1
// while it races the next bucket's gather. `norm_tile_bits` tiles the
// *norm precomputation* phase (the polynomial evaluation that seeds
// `sieve_array_` to begin with) so the polynomial coefficients stay in
// L1 across each tile's evaluation. The two gates have entirely separate
// caches, ENV names, and tunings; callers may set one or both depending
// on which phase dominates a given sieve fixture. Setting them to the
// same value is reasonable but not required.
//
// Correctness
// -----------
// The tile size is the *only* state this helper exposes. The norm scratch
// buffer contents (and therefore the seeded `sieve_array_` residuals)
// remain bit-for-bit identical between the tiled and untiled paths
// because `Polynomial::evaluate(a, b)` is a pure function of (a, b) and
// the polynomial coefficients - the only difference is the iteration
// order over rows, and the per-cell write is to a disjoint memory
// location for each (i, j). Each callsite that opts in MUST verify on
// its own fixtures that the seeded residuals (and any downstream candidate
// list) match the untiled baseline (the regression suite for the wired
// callsite covers this).
//
// ENV gate
// --------
//
//   GNFS_SIEVE_NORM_TILE_BITS=0       disabled (default; untiled precompute)
//   GNFS_SIEVE_NORM_TILE_BITS=1..8    enabled with 2^N rows per tile
//   GNFS_SIEVE_NORM_TILE_BITS=9+      clamped to 8 (256-row tile cap)
//   (unset)                           same as 0
//   empty / non-numeric               same as 0
//
// The runtime decision is cached after the first call so the hot path
// only pays for an atomic-relaxed load. Tests that toggle the env mid
// process call `norm_tile_reset_env_cache_for_testing()` to flush the
// cache.
//
// Build-time guards
// -----------------
// Header-only; depends only on the standard library. No external link
// dependencies. The cached state uses `std::call_once` + `std::atomic`
// so the first probe is thread-safe but inexpensive on subsequent calls.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace gnfs::sieve {

/// Maximum value of N accepted from GNFS_SIEVE_NORM_TILE_BITS. The
/// resulting tile is 2^8 = 256 rows. Tile sizes beyond this stop fitting
/// in L2 for realistic sieve region widths and the precomputation pass
/// begins paying the cache-eviction cost the tiling was meant to avoid.
inline constexpr int kNormTileMaxBits = 8;

namespace norm_tile_detail {

/// Cached parsed bit count. The first call resolves the cached value
/// from the environment; subsequent calls perform a relaxed atomic load.
inline std::atomic<int>& cached_bits() noexcept {
    static std::atomic<int> bits{0};
    return bits;
}

inline std::once_flag& cached_flag() noexcept {
    static std::once_flag flag;
    return flag;
}

/// Parse the environment variable value and clamp to [0, kNormTileMaxBits].
/// Treats unset / empty / non-numeric values as 0 (disabled). Numeric
/// values are parsed with `std::strtol` so leading whitespace and a
/// leading sign are tolerated but trailing garbage rejects the value
/// (returns 0).
inline int resolve_bits_from_env() noexcept {
    const char* v = std::getenv("GNFS_SIEVE_NORM_TILE_BITS");
    if (v == nullptr || v[0] == '\0') return 0;
    char* end = nullptr;
    const long parsed = std::strtol(v, &end, 10);
    if (end == nullptr || end == v || *end != '\0') return 0;
    if (parsed <= 0) return 0;
    if (parsed >= static_cast<long>(kNormTileMaxBits)) {
        return kNormTileMaxBits;
    }
    return static_cast<int>(parsed);
}

inline int load_bits() noexcept {
    std::call_once(cached_flag(), []() noexcept {
        cached_bits().store(resolve_bits_from_env(),
                            std::memory_order_relaxed);
    });
    return cached_bits().load(std::memory_order_relaxed);
}

}  // namespace norm_tile_detail

/// Returns the configured number of bits N from GNFS_SIEVE_NORM_TILE_BITS.
/// Values are clamped to [0, kNormTileMaxBits]. Returns 0 when the
/// environment variable is unset, empty, or non-numeric - callers should
/// treat 0 as "use the existing untiled precompute path".
[[nodiscard]] inline int norm_tile_bits() noexcept {
    return norm_tile_detail::load_bits();
}

/// Returns true when tiling should be applied (i.e., bits > 0). Callers
/// at precompute-phase entry can branch on this single predicate to avoid
/// per-iteration overhead in the untiled path.
[[nodiscard]] inline bool norm_tile_enabled() noexcept {
    return norm_tile_detail::load_bits() > 0;
}

/// Returns the tile size in rows: 2^N when enabled, 0 when disabled.
/// Callers should use this to size their tile-precompute inner loop. A
/// return of 0 indicates the gate is disabled and the caller should fall
/// back to the existing untiled precompute path.
///
/// The maximum returned value is `1 << kNormTileMaxBits` (256 rows).
[[nodiscard]] inline std::size_t norm_tile_size_rows() noexcept {
    const int bits = norm_tile_detail::load_bits();
    if (bits <= 0) return 0;
    return static_cast<std::size_t>(1) << static_cast<unsigned>(bits);
}

/// Re-read GNFS_SIEVE_NORM_TILE_BITS from the environment and refresh
/// the cached value. Intended for unit tests that flip the env between
/// scenarios. Not called from production hot paths - production resolves
/// the gate exactly once via `std::call_once`.
inline void norm_tile_reset_env_cache_for_testing() noexcept {
    norm_tile_detail::cached_bits().store(
        norm_tile_detail::resolve_bits_from_env(),
        std::memory_order_relaxed);
    // Ensure `call_once` records completion even if it has never run
    // before, so a later production call does not overwrite our state.
    std::call_once(norm_tile_detail::cached_flag(), []() noexcept {});
}

}  // namespace gnfs::sieve
