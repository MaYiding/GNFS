#pragma once

// Sieve apply-tile parallel dispatcher.
//
// Background
// ----------
// The lattice sieve `sieve_bucket_region` apply phase scans `sieve_array_`
// to extract candidate (a, b) pairs whose accumulated log-residual passes
// the threshold. The W6 helper `<gnfs/sieve/region_tile.hpp>` introduced
// `GNFS_SIEVE_REGION_TILE_BITS`, which partitions the row range into
// `2^N`-row tiles to keep each scan window inside L1/L2. That helper is
// **sequential cache-blocking**: the tiles are still scanned one after the
// other, just in a cache-friendly order.
//
// This helper is a strictly orthogonal axis: **parallel work distribution**
// across the tiles. Once a caller has decided how many tiles to produce
// (either via `region_tile_size_rows()` or any other partitioning scheme),
// the `parallel_apply_tiles` template lets the caller dispatch the tiles
// across multiple threads so that disjoint tiles can run concurrently. The
// caller-supplied `tile_fn(tile_index)` performs whatever work a tile
// needs (scan, gather, candidate emission, etc.) and the dispatcher
// collects per-tile results into a `std::vector<Result>` in input order.
//
//   `GNFS_SIEVE_APPLY_TILE_THREADS = N` (default 1, range
//   [1, hardware_concurrency * 2])
//
//   N = 1 keeps the sequential path bit-for-bit (no ThreadPool spawned,
//   zero overhead). N >= 2 spawns up to N pool workers and submits each
//   tile's work as an independent task. ENV unset / empty / "0" / negative
//   / non-numeric all collapse to the sequential default, matching the
//   W7 / W8 / W9 / W10 T4 / W11 T3 / W11 T4 parser semantics for a
//   consistent operator UX across the whole parallel-dispatcher family.
//
// Family position
// ---------------
// This helper is the sixth member of the parallel-dispatcher family that
// the W7 ... W11 T4 task waves landed:
//
//   * W7   `<gnfs/sqrt/hensel_parallel.hpp>`                  (Hensel slots)
//   * W8 T1 `<gnfs/cofactor/ecm_stage2_parallel.hpp>`         (ECM Stage 2)
//   * W9 T1 `<gnfs/cofactor/ecm_stage1_parallel.hpp>`         (ECM Stage 1)
//   * W10 T4 `<gnfs/relation/merger_parallel.hpp>`            (LP merger)
//   * W11 T3 `<gnfs/util/mpz_powm_parallel.hpp>`              (mpz_powm)
//   * W11 T4 `<gnfs/sieve/lattice_basis_parallel.hpp>`        (basis reduce)
//   * W12 T4 (this header)                                    (apply tiles)
//
// All seven share the same ENV-gate + ThreadPool dispatcher design. Each
// has its own ENV name so callers can independently tune the throughput
// vs latency trade-off at every hot site. Helpers do not interfere with
// one another and can be enabled simultaneously.
//
// Algorithmic equivalence (strict invariant)
// ------------------------------------------
//   * Per-tile work must be a pure function of `tile_index`. The caller-
//     supplied `tile_fn(tile_index)` must read only shared read-only state
//     (sieve_array_ snapshot, threshold, polynomial coefficients, etc.)
//     captured by reference; it must not write to shared mutable state.
//     If the caller needs to mutate per-tile data structures (e.g. emit
//     candidates into a per-tile buffer), each task must own its own
//     disjoint buffer (e.g. via a small per-tile struct stored in
//     `Result`) so that concurrent writes do not race.
//   * The dispatcher returns `std::vector<Result>` of per-tile outcomes
//     in input order: `Result[i] == tile_fn(i)` regardless of `threads`,
//     so callers see bit-for-bit identical output between the sequential
//     and parallel paths.
//
// Non-goals
// ---------
//   * We do NOT modify `src/sieve/lattice_sieve.cpp` or any existing
//     `sieve_bucket_region` call site. The dispatcher is helper-only
//     future-infra; an opt-in caller wiring it into the sieve apply loop
//     (alongside or instead of `region_tile_size_rows()`) is left for a
//     subsequent task. By keeping the wiring out of this header we
//     preserve the bit-for-bit baseline of the existing sieve while the
//     dispatcher is integrated into selected hot sites.
//   * We do NOT prescribe a tile size. The helper consumes a tile count
//     decided by the caller (typically `(rows + tile_size - 1) /
//     tile_size` with `tile_size = region_tile_size_rows()`). Selecting
//     a tile size is a separate cache-blocking concern handled by the
//     W6 region_tile helper.
//   * `Result` must be default-constructible (used to size the output
//     vector) and move- or copy-assignable. Common choices: a candidate
//     emit buffer struct, a small `(tile_index, candidate_count)` record,
//     `std::vector<Candidate>` per tile, or `std::optional<...>` for
//     "produced candidates / nothing this tile" semantics. Move-only
//     types are supported because the per-index assignment uses
//     `results[i] = std::move(...)`.

#include "../util/thread_pool.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <future>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace gnfs::sieve {

namespace detail {

/// Cached env-parsed thread count for the sieve apply-tile dispatcher.
/// Reset via `sieve_apply_tile_threads_reset_env_cache_for_testing()` so
/// unit tests can toggle `GNFS_SIEVE_APPLY_TILE_THREADS` between
/// assertions.
struct SieveApplyTileCache {
    std::once_flag once;
    std::size_t value = 1;
};

inline SieveApplyTileCache& sieve_apply_tile_cache() noexcept {
    static SieveApplyTileCache cache;
    return cache;
}

/// Parse `GNFS_SIEVE_APPLY_TILE_THREADS`. Returns 1 (sequential) on:
///   - ENV unset / empty / non-numeric / non-positive
/// Otherwise returns the parsed value, clamped to
/// [1, hardware_concurrency * 2] (fallback hw = 4 when
/// hardware_concurrency() reports 0 so the upper cap stays meaningful).
///
/// Parser semantics mirror `parse_lattice_basis_parallel_env()` and the
/// other W7 ... W11 T4 dispatcher helpers so callers that wire up
/// multiple knobs see consistent ENV behaviour: `std::atoi` accepts a
/// leading numeric prefix (so `"12abc"` parses to 12), empty / unset /
/// "0" / negative all collapse to the sequential default, and any
/// out-of-range value clamps to the cap rather than throwing.
inline std::size_t parse_sieve_apply_tile_env() noexcept {
    const char* env = std::getenv("GNFS_SIEVE_APPLY_TILE_THREADS");
    if (env == nullptr || env[0] == '\0') {
        return 1;  // default sequential
    }
    int parsed = std::atoi(env);
    if (parsed <= 0) {
        return 1;  // invalid / non-positive -> sequential
    }
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::size_t cap = static_cast<std::size_t>(hw) * 2;
    std::size_t v = static_cast<std::size_t>(parsed);
    if (v > cap) v = cap;
    return v;
}

}  // namespace detail

/// Read the `GNFS_SIEVE_APPLY_TILE_THREADS` env into a cached thread
/// count.
///
/// First call parses the env once (via `std::once_flag`); subsequent
/// calls return the cached value. Range:
/// [1, hardware_concurrency() * 2]. Default (unset / "" / non-numeric /
/// <= 0): 1 (sequential). Out-of-range high values clamp to the upper
/// cap.
[[nodiscard]] inline int sieve_apply_tile_threads() noexcept {
    auto& cache = detail::sieve_apply_tile_cache();
    std::call_once(cache.once, [&cache]() {
        cache.value = detail::parse_sieve_apply_tile_env();
    });
    return static_cast<int>(cache.value);
}

/// Resolve the effective worker count for a given tile_count.
///
/// Returns 1 when the cached env requests sequential (or asks for more
/// workers than there are tiles). Otherwise returns min(env, tile_count).
/// Callers that need to size their own scratch buffers before invoking
/// the dispatcher can use this helper.
[[nodiscard]] inline int
resolve_sieve_apply_tile_threads(std::size_t tile_count) noexcept {
    const int env = sieve_apply_tile_threads();
    if (tile_count == 0) return 0;
    if (env <= 1 || tile_count == 1) return 1;
    const std::size_t v = (static_cast<std::size_t>(env) < tile_count)
                              ? static_cast<std::size_t>(env)
                              : tile_count;
    return static_cast<int>(v);
}

/// Reset the cached thread count. Intended for unit tests that toggle
/// `GNFS_SIEVE_APPLY_TILE_THREADS` between assertions.
///
/// Not thread-safe; only call between test cases where no
/// `parallel_apply_tiles` invocation is in flight.
inline void sieve_apply_tile_threads_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::sieve_apply_tile_cache();
    // Reconstruct the once_flag + value in place. We avoid std::atomic
    // because the helper is meant to be called between test cases where
    // single-threaded use is guaranteed.
    cache.~SieveApplyTileCache();
    new (&cache) detail::SieveApplyTileCache();
}

/// Parallelize per-tile work across the sieve apply-phase tile range.
///
/// `tile_count` is the total number of tiles the caller has partitioned
/// the apply phase into (typically `ceil(rows / region_tile_size_rows())`
/// when the W6 cache-blocking gate is in play, but the helper takes any
/// tile count). `tile_fn(tile_index)` performs the work for a single
/// tile and returns its outcome as a value of caller-chosen type `Result`
/// (commonly a candidate-emit buffer, a small per-tile record, or a
/// `std::vector<Candidate>` containing the candidates discovered within
/// the tile).
///
/// `tile_fn` MUST be thread-safe over disjoint tile indices:
///   * It may read shared read-only state (sieve_array_ snapshot,
///     threshold, polynomial coefficients, factor base, etc.) captured
///     by reference.
///   * It MUST NOT write to any shared mutable state.
///   * Per-tile scratch must be allocated inside the lambda (per-call
///     local) or carried inside the returned `Result` so that two tasks
///     never collide on the same buffer.
///
/// Returns a `std::vector<Result>` of per-tile outcomes in input order.
/// `Result[i]` is exactly what `tile_fn(i)` returned, regardless of
/// `threads` - the sequential (N=1) and parallel (N>=2) paths are
/// bit-for-bit equivalent because `tile_fn` is a pure function of
/// `tile_index`.
///
/// Behavior:
///   - threads == 1 (default):  sequential for-loop, no ThreadPool created
///   - threads >= 2:            ThreadPool dispatch via `submit()` +
///                              `future.get()`
///   - tile_count == 0:         returns empty vector (no pool created,
///                              `tile_fn` never invoked)
///   - tile_count == 1:         always sequential (no ThreadPool overhead
///                              even when threads >= 2)
///
/// `Result` must be default-constructible (used to pre-size the output)
/// and move- or copy-assignable. Per-slot writes are race-free because
/// each task owns exactly one disjoint output index.
///
/// Exception propagation: any exception thrown by `tile_fn` propagates
/// to the caller via `future::get()`. The dispatcher does not swallow or
/// wrap exceptions. When multiple workers throw concurrently, only the
/// first exception reached by the synchronous `future.get()` loop is
/// observed; remaining futures are still waited on so the ThreadPool can
/// join cleanly.
template <typename Result, typename TileFn>
inline std::vector<Result>
parallel_apply_tiles(std::size_t tile_count, TileFn tile_fn) {
    std::vector<Result> results;
    if (tile_count == 0) return results;

    results.resize(tile_count);

    const int threads = sieve_apply_tile_threads();

    // Sequential path: zero overhead, preserves original behaviour
    // bit-for-bit (no pool spawn, no future overhead). Also exercised
    // when a caller asks for parallelism but only supplied a single
    // tile; one task is never worth a pool spin-up.
    if (threads <= 1 || tile_count == 1) {
        for (std::size_t i = 0; i < tile_count; ++i) {
            results[i] = tile_fn(i);
        }
        return results;
    }

    // Parallel path: bound pool size by min(threads, tile_count).
    // Spawning more workers than tiles wastes resources and adds futex
    // pressure for no throughput gain.
    const std::size_t pool_size =
        (static_cast<std::size_t>(threads) < tile_count)
            ? static_cast<std::size_t>(threads)
            : tile_count;
    gnfs::util::ThreadPool pool(static_cast<uint32_t>(pool_size));

    std::vector<std::future<void>> futures;
    futures.reserve(tile_count);
    for (std::size_t i = 0; i < tile_count; ++i) {
        // Each task captures the tile index plus references to the
        // output vector and the user tile functor. Per-tile output
        // slots are disjoint, so concurrent writes to results[i] are
        // race-free even though `results` itself is shared.
        futures.push_back(pool.submit([&results, &tile_fn, i]() {
            results[i] = tile_fn(i);
        }));
    }

    // Drain every future even when one rethrows: we want the pool to
    // join cleanly in its dtor (workers must finish their current task
    // before returning) and we do not want a thrown exception to
    // abandon other workers' results mid-flight. The first observed
    // exception propagates; any subsequent exceptions are swallowed
    // (matches std::async / typical future-chain semantics).
    std::exception_ptr first_exc;
    for (auto& f : futures) {
        try {
            f.get();
        } catch (...) {
            if (!first_exc) {
                first_exc = std::current_exception();
            }
        }
    }
    if (first_exc) {
        std::rethrow_exception(first_exc);
    }

    return results;
}

}  // namespace gnfs::sieve
