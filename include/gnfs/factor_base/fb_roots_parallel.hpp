#pragma once

// Factor Base Cantor-Zassenhaus root-finding — per-prime parallel dispatcher.
//
// Background:
//   Factor base construction iterates over the algebraic factor base primes and
//   calls `find_roots_mod_p(ctx, p)` for each one. The per-prime CZ algorithm
//   is a pure function of (p, monic_polynomial_mod_p) and depends only on
//   immutable inputs, so the cross-prime work is embarrassingly parallel.
//
//   The current production path in `src/factor_base/builder.cpp` spawns a
//   `std::vector<std::thread>` sized by `std::thread::hardware_concurrency()`.
//   That is fine for typical bulk builds but offers no knob to:
//     * force sequential execution for `lldb` / `bisect` / sanitizer debugging,
//     * pin a smaller thread count under sandboxed CI runners (2-4 vCPU),
//     * experiment with thread-count sweeps for fine-tuning ROI.
//
// This header centralises an env-gated dispatcher:
//
//   GNFS_FB_ROOTS_THREADS = N (default 0, range [0, hardware_concurrency * 2])
//
//   N = 0 (unset / "0" / non-numeric / empty / negative):
//       "use the runtime default" — the helper falls back to
//       `std::thread::hardware_concurrency()`, matching the legacy production
//       path bit-for-bit. Zero-overhead default.
//
//   N = 1:
//       Force sequential. No ThreadPool created. Useful for `lldb`, bisecting
//       a regression, or running under sanitizers that hate concurrent GMP
//       traffic.
//
//   N >= 2:
//       Use exactly N worker threads via a dedicated `util::ThreadPool`.
//
//   N > hardware_concurrency() * 2 clamps to the upper cap (with a 16-worker
//   fallback when `hardware_concurrency()` reports 0). Non-numeric inputs
//   parse to 0 (same as unset).
//
// Bit-for-bit guarantee:
//   The dispatcher pre-sizes the output vector to `primes.size()` and each
//   worker writes exclusively to its own index. The per-prime `worker_fn(p)`
//   is required to be a pure function of `p` plus read-only state captured
//   by the lambda. With these invariants, output[i] equals
//   `worker_fn(primes[i])` regardless of thread count, so sequential and
//   parallel paths produce identical `std::vector<Result>` outputs.
//
// Non-goals:
//   * We do NOT modify `src/factor_base/builder.cpp` — this helper is opt-in
//     infrastructure for future wire-in. The production CZ loop keeps its
//     existing `std::thread::hardware_concurrency()` behaviour.
//   * We do NOT change the inner CZ algorithm. Only outer dispatch changes.
//   * We do NOT impose synchronisation requirements beyond "worker_fn writes
//     no shared mutable state" — the helper does not own GMP buffers and
//     leaves all per-prime arithmetic to the caller's lambda.

#include "../util/thread_pool.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace gnfs::factor_base {

namespace detail {

/// Cached env-parsed thread count. Reset via
/// `fb_roots_threads_reset_env_cache_for_testing()` for unit testing under
/// different env values.
struct FbRootsThreadsCache {
    std::once_flag once;
    int value = 0;
};

inline FbRootsThreadsCache& fb_roots_threads_cache() noexcept {
    static FbRootsThreadsCache cache;
    return cache;
}

/// Parse `GNFS_FB_ROOTS_THREADS`. Returns 0 (default) on:
///   - ENV unset / empty / non-numeric / negative
/// Otherwise returns the parsed value, clamped to
/// [0, hardware_concurrency() * 2] (with a fallback of 16 when
/// `hardware_concurrency()` reports 0).
///
/// Return value semantics (see header docblock for the full contract):
///   0  => "fall back to runtime default" (hardware_concurrency()).
///   1  => "force sequential".
///   >=2 => "use exactly N worker threads".
inline int parse_fb_roots_threads_env() noexcept {
    const char* env = std::getenv("GNFS_FB_ROOTS_THREADS");
    if (env == nullptr || env[0] == '\0') {
        return 0;  // default — use hardware_concurrency()
    }
    int parsed = 0;
    try {
        // Use std::stoi to handle leading whitespace and reject pure garbage.
        // std::atoi would silently return 0 for "garbage", which we want, but
        // std::stoi also throws std::invalid_argument for "abc" so we get the
        // same behaviour via the catch block.
        std::size_t consumed = 0;
        parsed = std::stoi(env, &consumed);
        // Guard against partial parses like "12abc" — treat as garbage.
        if (consumed == 0) {
            return 0;
        }
    } catch (...) {
        return 0;
    }
    if (parsed < 0) {
        return 0;
    }
    unsigned int hw = std::thread::hardware_concurrency();
    int hw_max = static_cast<int>(hw) * 2;
    if (hw_max <= 0) hw_max = 16;
    if (parsed > hw_max) parsed = hw_max;
    return parsed;
}

}  // namespace detail

/// Read the `GNFS_FB_ROOTS_THREADS` env into a cached thread count.
///
/// First call parses the env once (via `std::once_flag`); subsequent calls
/// return the cached value. Range: [0, hardware_concurrency() * 2] (with a
/// fallback cap of 16 when hardware_concurrency() reports 0).
///
/// Semantics (see header docblock):
///   - 0:  default — caller should use `std::thread::hardware_concurrency()`.
///   - 1:  force sequential, no ThreadPool spawn.
///   - >=2: use exactly that many worker threads.
[[nodiscard]] inline int fb_roots_threads() noexcept {
    auto& cache = detail::fb_roots_threads_cache();
    std::call_once(cache.once, [&cache]() {
        cache.value = detail::parse_fb_roots_threads_env();
    });
    return cache.value;
}

/// Reset the cached thread count. Intended for unit tests that toggle
/// `GNFS_FB_ROOTS_THREADS` between assertions.
///
/// Not thread-safe; only call when no `parallel_fb_roots` is in flight.
inline void fb_roots_threads_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::fb_roots_threads_cache();
    // Reconstruct the once_flag + value in place. Mirrors the pattern used by
    // `schirokauer_threads_reset_env_cache_for_testing()` and
    // `ecm_stage2_parallel_reset_env_cache_for_testing()`.
    cache.~FbRootsThreadsCache();
    new (&cache) detail::FbRootsThreadsCache();
}

/// Resolve the effective worker thread count for a `parallel_fb_roots` call.
///
/// Combines the env-cached value with the supplied prime count `n`:
///   * env == 0 (default)  -> hardware_concurrency() (fall-back 4 if 0)
///   * env == 1            -> 1 (sequential)
///   * env >= 2            -> min(env, n) once we know n
/// The returned value is then bound by `n` so we never spawn more workers
/// than there are tasks (avoids wasted thread spin-up).
[[nodiscard]] inline std::size_t
resolve_fb_roots_threads(std::size_t n) noexcept {
    if (n == 0) return 0;
    int env = fb_roots_threads();
    std::size_t effective;
    if (env == 0) {
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        effective = static_cast<std::size_t>(hw);
    } else {
        effective = static_cast<std::size_t>(env);
    }
    if (effective > n) effective = n;
    return effective;
}

/// Per-prime parallel dispatcher for factor-base root-finding.
///
/// Runs `worker_fn(p)` for every `p` in `primes` and returns the per-prime
/// results in input-position order. `worker_fn` MUST be a pure function over
/// `p` plus read-only state captured by reference; it MUST NOT write to any
/// shared mutable state. GMP `mpz_*` operations inside the lambda must
/// operate on per-task-local `Integer` buffers (the lambda's own scratch).
///
/// Behaviour:
///   - empty primes:               returns empty vector (no pool created)
///   - resolved threads <= 1:      sequential for-loop, zero overhead
///   - resolved threads >= 2:      ThreadPool dispatch via
///                                 `parallel_for_index`; each task writes
///                                 only to its own index in the output
///                                 vector
///
/// Bit-for-bit guarantee: `worker_fn(p)` is a pure function, output[i] is
/// written only by the task that owns index i, and the output container is
/// pre-sized so concurrent disjoint writes are race-free. Sequential and
/// parallel paths produce identical output vectors.
template <typename Result, typename WorkerFn>
[[nodiscard]] inline std::vector<Result>
parallel_fb_roots(const std::vector<uint32_t>& primes, WorkerFn worker_fn) {
    const std::size_t n = primes.size();
    std::vector<Result> results;
    if (n == 0) return results;

    results.resize(n);

    const std::size_t threads = resolve_fb_roots_threads(n);

    // Sequential path: zero overhead, no ThreadPool spawn. This covers
    //   * n == 1 (one prime — pool overhead always loses)
    //   * env == 1 explicitly forcing sequential
    //   * env == 0 on a hypothetical 1-core machine
    if (threads <= 1) {
        for (std::size_t i = 0; i < n; ++i) {
            results[i] = worker_fn(primes[i]);
        }
        return results;
    }

    // Parallel path: dedicated pool sized by `threads` (already capped by n).
    // We do NOT reuse a global pool because:
    //   1. Helper is reentrancy-safe (no static state beyond the env cache).
    //   2. Callers may run inside another pool already (composability).
    //   3. Spin-up cost for a fresh `util::ThreadPool` is small relative to
    //      thousands of CZ root-finds.
    gnfs::util::ThreadPool pool(static_cast<uint32_t>(threads));

    // parallel_for_index dispatches contiguous chunks to workers; each task
    // writes only `results[i]` for its assigned i. Reads of `primes[i]` are
    // const-ref, and writes to `results[i]` are disjoint per index.
    pool.parallel_for_index(0, n, [&](std::size_t i) {
        results[i] = worker_fn(primes[i]);
    });

    return results;
}

}  // namespace gnfs::factor_base
