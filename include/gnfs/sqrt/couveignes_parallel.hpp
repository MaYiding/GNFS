#pragma once

// Couveignes algebraic sqrt — sign-pattern search parallel dispatcher.
//
// The Couveignes algorithm enumerates up to 2^16 = 65536 sign patterns in
// Gray-code order to recover the global sign assignment for the K small-prime
// CRT factors of f'(α)·sqrt(P).  Each pattern verification is independent of
// other patterns — it computes a candidate signed root and checks against
// quadratic-character / numeric constraints.  The pattern-verify body is a
// pure function of (pattern_index, read-only weights / CRT state captured
// by the caller).
//
// This helper centralizes the env-gated dispatch:
//
//   GNFS_COUVEIGNES_PARALLEL_THREADS = N (default 1, range [1, hardware_concurrency * 2])
//
// N = 1 keeps the sequential path bit-for-bit (no ThreadPool created, zero
// overhead) — exactly the legacy per-pattern Gray-code loop.  N >= 2 spawns
// N pool workers and partitions the [start, end) pattern range into N
// approximately equal chunks; the first worker to find a valid pattern
// short-circuits all remaining workers via a shared atomic flag.
//
// "First valid pattern" semantics:
//   - Sequential (N=1): returns the *first* valid pattern in scan order (the
//     legacy Couveignes behavior).
//   - Parallel (N>=2): returns *one* of the valid patterns; due to the racy
//     short-circuit, the returned pattern is the smallest one observed before
//     any worker exited.  When the search space contains exactly one valid
//     pattern, parallel and sequential paths return the same value.  When
//     multiple valid patterns exist, the parallel path may return any of
//     them, but is guaranteed to return one that satisfies the verifier.
//   - The helper does NOT claim sequential bit-for-bit pattern identity in
//     the multi-valid case.  Callers requiring deterministic selection can
//     order their verifier so that exactly one pattern can match (e.g., by
//     selecting the *minimum* matching index via post-processing).
//
// Design notes:
//   - The helper is a standalone template; it does not wire into Couveignes'
//     main pipeline.  Couveignes' production sign-search remains the existing
//     sequential Gray-code path until a separate wire-in change selects this
//     dispatcher.  This mirrors the W7 T2 / T3 helper-only landings.
//   - The verify_fn callable must be thread-safe under concurrent invocation:
//     it may read shared immutable state (weights, base CRT, expected
//     residues, etc.) captured by reference, but must not write to shared
//     mutable state.  Per-thread scratch buffers must be created inside
//     verify_fn (e.g., via thread_local or per-call construction).
//   - The total pattern count [start, end) is uint64_t; in practice the
//     Couveignes caller would pass start=0 and end=65536, but the helper is
//     generic over any contiguous range.
//   - Empty range (start == end) returns std::nullopt immediately, no pool.
//   - Single-pattern range (end - start == 1) takes the sequential fast path
//     even when N >= 2 (no ThreadPool overhead for one item).

#include "../util/thread_pool.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace gnfs::sqrt {

namespace detail {

// Cached env-parsed thread count.  Reset via
// couveignes_parallel_threads_reset_env_cache_for_testing() for unit testing
// under different env values.
struct CouveignesParallelThreadsCache {
    std::once_flag once;
    int value = 1;
};

inline CouveignesParallelThreadsCache& couveignes_parallel_threads_cache() noexcept {
    static CouveignesParallelThreadsCache cache;
    return cache;
}

inline int parse_couveignes_parallel_threads_env() noexcept {
    const char* env = std::getenv("GNFS_COUVEIGNES_PARALLEL_THREADS");
    if (env == nullptr || env[0] == '\0') {
        return 1;  // default sequential
    }
    // Manual numeric prefix detection — std::stoi accepts leading whitespace
    // and partial parses ("12abc" → 12), but we treat any non-numeric value
    // (including partial-numeric like "4x" or "abc") as default 1 for
    // predictability under invalid env inputs.
    const char* p = env;
    if (*p == '+' || *p == '-') ++p;
    if (*p == '\0') return 1;
    for (const char* q = p; *q != '\0'; ++q) {
        if (*q < '0' || *q > '9') {
            return 1;  // any non-digit (after optional sign) → invalid → 1
        }
    }
    int parsed = 1;
    try {
        parsed = std::stoi(env);
    } catch (...) {
        return 1;  // out of int range or other parse failure → 1
    }
    if (parsed <= 0) {
        return 1;  // non-positive → sequential
    }
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    int cap = static_cast<int>(hw) * 2;
    if (cap < 1) cap = 16;  // unreasonable hw → 16 fallback (defensive)
    if (parsed > cap) parsed = cap;
    return parsed;
}

}  // namespace detail

/// Read the GNFS_COUVEIGNES_PARALLEL_THREADS env into a cached thread count.
///
/// First call parses the env once (via std::once_flag); subsequent calls
/// return the cached value.  Range: [1, hardware_concurrency() * 2].
/// Default (unset / "" / non-numeric / <= 0): 1 (sequential).  Out-of-range
/// high values clamp to the upper cap.
[[nodiscard]] inline int couveignes_parallel_threads() noexcept {
    auto& cache = detail::couveignes_parallel_threads_cache();
    std::call_once(cache.once, [&cache]() {
        cache.value = detail::parse_couveignes_parallel_threads_env();
    });
    return cache.value;
}

/// Reset the cached thread count.  Intended for unit tests that toggle
/// GNFS_COUVEIGNES_PARALLEL_THREADS between assertions.
///
/// Not thread-safe; only call when no parallel_pattern_search is in flight.
inline void couveignes_parallel_threads_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::couveignes_parallel_threads_cache();
    // Reconstruct the once_flag + value in place.  Mirrors
    // reset_sqrt_hensel_threads_cache() in include/gnfs/sqrt/hensel_parallel.hpp.
    cache.~CouveignesParallelThreadsCache();
    new (&cache) detail::CouveignesParallelThreadsCache();
}

/// Search the pattern range [start, end) for the first index that satisfies
/// verify_fn, with optional thread-pool parallelization.
///
/// @tparam VerifyFn  Callable with signature `bool(uint64_t pattern_idx)`.
///                   Must be thread-safe when invoked concurrently from
///                   multiple workers (see helper-level docs above).
///
/// @param start      Inclusive lower bound of pattern range.
/// @param end        Exclusive upper bound of pattern range.
/// @param verify_fn  Per-pattern validator; returns true when the pattern is
///                   accepted.  The first thread to return true short-circuits
///                   all other workers.
///
/// @return std::nullopt if no pattern in [start, end) is accepted.  Otherwise
///         the index of the first accepted pattern.  In the parallel path
///         (when env >= 2 and range > 1), "first" is defined as the smallest
///         pattern index observed by any worker before the first short-circuit
///         signal; when the search space contains exactly one accepted
///         pattern, this is equivalent to the sequential first-match.
///
/// Behavior:
///   - threads == 1 (default):      sequential for-loop, no ThreadPool
///                                  created (zero overhead) — bit-for-bit
///                                  matches legacy Couveignes Gray-code loop.
///   - threads >= 2 && range >= 2:  ThreadPool dispatch with shared
///                                  found_flag + first_match atomic.
///   - empty range (start == end):  returns std::nullopt immediately, no
///                                  ThreadPool created.
///   - single-pattern range:        sequential fast path even when N >= 2
///                                  (avoids ThreadPool overhead for one
///                                  pattern).
template <typename VerifyFn>
[[nodiscard]] inline std::optional<uint64_t> parallel_pattern_search(
        uint64_t start,
        uint64_t end,
        VerifyFn verify_fn) {

    if (end <= start) {
        return std::nullopt;
    }

    const uint64_t range = end - start;
    const int threads = couveignes_parallel_threads();

    // Sequential fast path: zero overhead, preserves legacy behavior
    // bit-for-bit.  Used when threads == 1 or range == 1 (single pattern).
    if (threads <= 1 || range == 1) {
        for (uint64_t i = start; i < end; ++i) {
            if (verify_fn(i)) {
                return i;
            }
        }
        return std::nullopt;
    }

    // Parallel path: partition [start, end) into N contiguous chunks.
    // pool_size = min(threads, range) to avoid wasting workers on a small
    // search range.
    const uint64_t pool_size_u64 = (static_cast<uint64_t>(threads) < range)
                                       ? static_cast<uint64_t>(threads)
                                       : range;
    const std::size_t pool_size = static_cast<std::size_t>(pool_size_u64);
    gnfs::util::ThreadPool pool(static_cast<uint32_t>(pool_size));

    // Shared short-circuit signal + first-match index.
    //   found_flag:   true once any worker has found a match (relaxed visibility
    //                 OK — the only effect of a missed update is one extra
    //                 inner iteration before noticing).
    //   first_match:  atomic min reduction of all observed matches across
    //                 workers.  UINT64_MAX sentinel == no match.
    std::atomic<bool> found_flag{false};
    std::atomic<uint64_t> first_match{std::numeric_limits<uint64_t>::max()};

    // Inner helper: atomic-min update on first_match.  Used by each worker
    // when it finds a match, to ensure we report the smallest pattern index
    // observed (deterministic when only one match exists).
    auto atomic_min_update = [&first_match](uint64_t candidate) noexcept {
        uint64_t expected = first_match.load(std::memory_order_relaxed);
        while (candidate < expected) {
            if (first_match.compare_exchange_weak(
                    expected, candidate,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                break;
            }
            // expected is now refreshed with the current value; loop retries
            // if candidate is still smaller.
        }
    };

    // Chunk allocation.  Distribute the range evenly across pool_size chunks
    // with the remainder spread across the first `range % pool_size` chunks
    // (giving them one extra pattern each).
    const uint64_t base_chunk = range / pool_size_u64;
    const uint64_t remainder = range % pool_size_u64;

    std::vector<std::future<void>> futures;
    futures.reserve(pool_size);

    uint64_t cursor = start;
    for (std::size_t w = 0; w < pool_size; ++w) {
        uint64_t chunk_len = base_chunk + (static_cast<uint64_t>(w) < remainder ? 1ULL : 0ULL);
        if (chunk_len == 0) {
            // Defensive: should not happen because pool_size <= range.
            continue;
        }
        const uint64_t chunk_start = cursor;
        const uint64_t chunk_end = cursor + chunk_len;
        cursor = chunk_end;

        futures.push_back(pool.submit(
            [chunk_start, chunk_end, &verify_fn, &found_flag, &atomic_min_update]() {
                for (uint64_t i = chunk_start; i < chunk_end; ++i) {
                    // Short-circuit early when another worker has already
                    // signaled a match.  We re-check every iteration to
                    // bound wasted work; the relaxed load is a cheap acquire
                    // hint that the smallest matching index has been seen.
                    if (found_flag.load(std::memory_order_acquire)) {
                        return;
                    }
                    if (verify_fn(i)) {
                        atomic_min_update(i);
                        found_flag.store(true, std::memory_order_release);
                        return;
                    }
                }
            }));
    }

    // Propagate any task exception to the caller via future::get().
    for (auto& f : futures) {
        f.get();
    }

    const uint64_t winner = first_match.load(std::memory_order_acquire);
    if (winner == std::numeric_limits<uint64_t>::max()) {
        return std::nullopt;
    }
    return winner;
}

}  // namespace gnfs::sqrt
