#pragma once

// Row-tiled scan helper for the lattice sieve bucket region apply phase.
//
// Scope
// -----
// During `sieve_bucket_region` the bucket gather phase touches every row of
// `sieve_array_` to accumulate the per-prime log contributions and then
// scans the same array for entries whose accumulated weight clears the
// candidate threshold. On a 50d+ region (`(i_max - i_min) * j_count` of
// tens of thousands of bytes) the second pass starts cold against the L1
// even though the gather pass just touched the data — the working set
// exceeds L1 and the apply scan races the next bucket's gather, evicting
// the rows we want to read back. Tiling the apply pass by 2^N rows keeps
// each scan window comfortably inside L1/L2 (16-row tile = 1 KiB at most
// realistic j widths) and lets the scan finish before the next tile pays
// the eviction cost.
//
// Two paths exist for any caller that opts in:
//
// 1. Untiled scan (default, `GNFS_SIEVE_REGION_TILE_BITS=0` or unset):
//    the existing apply loop runs unchanged. No additional code path,
//    no behaviour change — the gate predicate `region_tile_enabled()`
//    short-circuits to `false` and the helper returns a tile size of 0.
//
// 2. Tiled scan (`GNFS_SIEVE_REGION_TILE_BITS=N` for N in [1, 8]):
//    callers partition the row range into floor(rows / 2^N) tiles of
//    2^N rows each (with a tail tile when rows is not a multiple of 2^N)
//    and scan each tile to completion before advancing to the next.
//    The tile size is fixed at 2^N rows because the GF(2) sieve region
//    is naturally row-major and 2^N row strides align with the cache
//    line granularity. N is clamped to [0, 8] so the worst-case tile is
//    256 rows; anything wider stops fitting in L2 for realistic j widths
//    and the apply scan starts paying the eviction cost the tiling was
//    meant to amortise.
//
// Correctness
// -----------
// The tile size is the *only* state this helper exposes. The sieve_array_
// contents and the candidate list remain bit-for-bit identical between
// the tiled and untiled paths because the gather pass is unchanged — the
// only difference is the iteration order of the candidate threshold scan,
// and the candidate check is a pure function of (residual, threshold)
// that does not depend on the scan order. Each callsite that opts in
// MUST verify on its own fixtures that the candidate output matches the
// untiled baseline (the regression suite for the wired callsite covers
// this).
//
// ENV gate
// --------
//
//   GNFS_SIEVE_REGION_TILE_BITS=0       disabled (default; untiled scan)
//   GNFS_SIEVE_REGION_TILE_BITS=1..8    enabled with 2^N rows per tile
//   GNFS_SIEVE_REGION_TILE_BITS=9+      clamped to 8 (256-row tile cap)
//   (unset)                             same as 0
//   empty / non-numeric                 same as 0
//
// The runtime decision is cached after the first call so the hot path
// only pays for an atomic-relaxed load. Tests that toggle the env mid
// process call `region_tile_reset_env_cache_for_testing()` to flush
// the cache.
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

/// Maximum value of N accepted from GNFS_SIEVE_REGION_TILE_BITS. The
/// resulting tile is 2^8 = 256 rows. Tile sizes beyond this stop fitting
/// in L2 for realistic sieve region widths and the apply scan begins
/// paying the cache-eviction cost the tiling was meant to avoid.
inline constexpr int kRegionTileMaxBits = 8;

namespace region_tile_detail {

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

/// Parse the environment variable value and clamp to [0, kRegionTileMaxBits].
/// Treats unset / empty / non-numeric values as 0 (disabled). Numeric
/// values are parsed with `std::strtol` so leading whitespace and a
/// leading sign are tolerated but trailing garbage rejects the value
/// (returns 0).
inline int resolve_bits_from_env() noexcept {
    const char* v = std::getenv("GNFS_SIEVE_REGION_TILE_BITS");
    if (v == nullptr || v[0] == '\0') return 0;
    char* end = nullptr;
    const long parsed = std::strtol(v, &end, 10);
    if (end == nullptr || end == v || *end != '\0') return 0;
    if (parsed <= 0) return 0;
    if (parsed >= static_cast<long>(kRegionTileMaxBits)) {
        return kRegionTileMaxBits;
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

}  // namespace region_tile_detail

/// Returns the configured number of bits N from GNFS_SIEVE_REGION_TILE_BITS.
/// Values are clamped to [0, kRegionTileMaxBits]. Returns 0 when the
/// environment variable is unset, empty, or non-numeric — callers should
/// treat 0 as "use the existing untiled scan path".
[[nodiscard]] inline int region_tile_bits() noexcept {
    return region_tile_detail::load_bits();
}

/// Returns true when tiling should be applied (i.e., bits > 0). Callers
/// at apply-phase entry can branch on this single predicate to avoid
/// per-iteration overhead in the untiled path.
[[nodiscard]] inline bool region_tile_enabled() noexcept {
    return region_tile_detail::load_bits() > 0;
}

/// Returns the tile size in rows: 2^N when enabled, 0 when disabled.
/// Callers should use this to size their tile-scan inner loop. A return
/// of 0 indicates the gate is disabled and the caller should fall back
/// to the existing untiled scan path.
///
/// The maximum returned value is `1 << kRegionTileMaxBits` (256 rows).
[[nodiscard]] inline std::size_t region_tile_size_rows() noexcept {
    const int bits = region_tile_detail::load_bits();
    if (bits <= 0) return 0;
    return static_cast<std::size_t>(1) << static_cast<unsigned>(bits);
}

/// Re-read GNFS_SIEVE_REGION_TILE_BITS from the environment and refresh
/// the cached value. Intended for unit tests that flip the env between
/// scenarios. Not called from production hot paths — production resolves
/// the gate exactly once via `std::call_once`.
inline void region_tile_reset_env_cache_for_testing() noexcept {
    region_tile_detail::cached_bits().store(
        region_tile_detail::resolve_bits_from_env(),
        std::memory_order_relaxed);
    // Ensure `call_once` records completion even if it has never run
    // before, so a later production call does not overwrite our state.
    std::call_once(region_tile_detail::cached_flag(), []() noexcept {});
}

}  // namespace gnfs::sieve
