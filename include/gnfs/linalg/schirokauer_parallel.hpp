#pragma once

// Schirokauer map computation — per-relation parallel dispatcher.
//
// SchirokaurMap::compute_flat(a, b) is a pure const member: it only reads
// prime_info_[i] (immutable after the ctor finishes precompute_for_prime
// for every configured ell), plus the scalar inputs (a, b).  No shared
// mutable state is touched between calls.  Per-relation parallelism is
// therefore trivially safe — each output row depends only on its own
// (a, b) and read-only prime_info_.
//
// This header centralizes the env-gated dispatch:
//
//   GNFS_SCHIROKAUER_THREADS = N (default 1, range [1, hardware_concurrency * 2])
//
// N = 1 keeps the sequential path bit-for-bit (no ThreadPool created, zero
// overhead).  N >= 2 spawns N pool workers and computes Schirokauer maps
// for each relation in parallel.
//
// Bit-for-bit guarantee: compute_flat(a_i, b_i) is a deterministic pure
// function of its inputs and the read-only SchirokaurMap state, so the
// per-relation output vector at index i is identical between sequential
// and parallel paths.  The output container (vector<vector<uint32_t>>) is
// always indexed by input position, so per-row order matches input order.

#include "schirokauer.hpp"
#include "../util/thread_pool.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace gnfs::linalg {

namespace detail {

// Cached env-parsed thread count.  Reset via
// schirokauer_threads_reset_env_cache_for_testing() for unit testing under
// different env values.
struct SchirokauerThreadsCache {
    std::once_flag once;
    std::size_t value = 1;
};

inline SchirokauerThreadsCache& schirokauer_threads_cache() noexcept {
    static SchirokauerThreadsCache cache;
    return cache;
}

inline std::size_t parse_schirokauer_threads_env() noexcept {
    const char* env = std::getenv("GNFS_SCHIROKAUER_THREADS");
    if (env == nullptr || env[0] == '\0') {
        return 1;  // default sequential
    }
    int parsed = std::atoi(env);
    if (parsed <= 0) {
        return 1;  // invalid / non-positive / non-numeric → sequential
    }
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::size_t cap = static_cast<std::size_t>(hw) * 2;
    std::size_t v = static_cast<std::size_t>(parsed);
    if (v > cap) v = cap;
    return v;
}

}  // namespace detail

/// Read the GNFS_SCHIROKAUER_THREADS env into a cached thread count.
///
/// First call parses the env once (via std::once_flag); subsequent calls
/// return the cached value.  Range: [1, hardware_concurrency() * 2].
/// Default (unset / "" / non-numeric / <= 0): 1 (sequential).  Out-of-range
/// high values clamp to the upper cap.
[[nodiscard]] inline std::size_t schirokauer_threads() noexcept {
    auto& cache = detail::schirokauer_threads_cache();
    std::call_once(cache.once, [&cache]() {
        cache.value = detail::parse_schirokauer_threads_env();
    });
    return cache.value;
}

/// Reset the cached thread count.  Intended for unit tests that toggle
/// GNFS_SCHIROKAUER_THREADS between assertions.
///
/// Not thread-safe; only call when no compute_schirokauer_flat_batch is in
/// flight.
inline void schirokauer_threads_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::schirokauer_threads_cache();
    // Reconstruct the once_flag + value in place.  Mirrors
    // reset_sqrt_hensel_threads_cache() in include/gnfs/sqrt/hensel_parallel.hpp.
    cache.~SchirokauerThreadsCache();
    new (&cache) detail::SchirokauerThreadsCache();
}

/// Per-relation parallel dispatcher for SchirokaurMap::compute_flat.
///
/// Computes Schirokauer map flat vectors for a batch of (a, b) pairs.
/// Returns a vector indexed by input position; output[i] is exactly
/// `map.compute_flat(ab_pairs[i].first, ab_pairs[i].second)` regardless of
/// the thread count.
///
/// Behavior:
///   - threads == 1 (default):      sequential for-loop, no ThreadPool
///                                  created (zero overhead)
///   - threads >= 2 && n >= 2:      ThreadPool dispatch via
///                                  parallel_for_index
///   - empty ab_pairs:              returns empty vector (no pool created)
///   - single relation (n == 1):    sequential even when threads >= 2
///                                  (no ThreadPool overhead for one item)
///
/// Bit-for-bit guarantee: SchirokaurMap::compute_flat is a const pure
/// function over (a, b) plus the read-only prime_info_; identical inputs
/// produce identical outputs in either path.  The output container is
/// pre-sized to ab_pairs.size() and each parallel task writes only to its
/// own index, so per-position results match the sequential path exactly.
template <typename ABPair>
[[nodiscard]] inline std::vector<std::vector<uint32_t>>
compute_schirokauer_flat_batch(
        const SchirokaurMap& map,
        const std::vector<ABPair>& ab_pairs) {

    std::vector<std::vector<uint32_t>> out;
    const std::size_t n = ab_pairs.size();
    out.resize(n);
    if (n == 0) return out;

    const std::size_t threads = schirokauer_threads();

    // Sequential path: zero overhead, preserves original behavior bit-for-bit.
    if (threads <= 1 || n == 1) {
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = map.compute_flat(
                static_cast<int64_t>(ab_pairs[i].first),
                static_cast<uint64_t>(ab_pairs[i].second));
        }
        return out;
    }

    // Parallel path: bound pool size by min(threads, n).  Spawning more
    // workers than relations wastes resources.
    const std::size_t pool_size = (threads < n) ? threads : n;
    gnfs::util::ThreadPool pool(static_cast<uint32_t>(pool_size));

    // parallel_for_index dispatches contiguous chunks to workers; each task
    // writes only out[i] for its assigned indices.  Since SchirokaurMap is
    // const and prime_info_ is immutable post-ctor, no synchronization is
    // required for the reads.  out[i] writes are disjoint per index.
    pool.parallel_for_index(0, n, [&](std::size_t i) {
        out[i] = map.compute_flat(
            static_cast<int64_t>(ab_pairs[i].first),
            static_cast<uint64_t>(ab_pairs[i].second));
    });

    return out;
}

}  // namespace gnfs::linalg
