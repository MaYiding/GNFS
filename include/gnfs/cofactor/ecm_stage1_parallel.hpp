#pragma once

// ECM Stage 1 (Lucas-chain Montgomery ladder) multi-curve parallel dispatch.
//
// Background:
//   ECM Stage 1 (`ECM::stage1` / the post-curve-construction Montgomery ladder
//   inside `try_curve_with_pk`) performs the scalar-multiplication chain
//   `k * Q` for a Suyama-parametrised starting point Q, where `k` is the
//   product of small prime powers up to B1. The chain is purely arithmetic
//   on `(X : Z)` projective coordinates modulo N and depends on no shared
//   mutable state once the per-curve `(sigma, n, a24, Q)` tuple is fixed.
//   Distinct sigmas therefore yield embarrassingly-parallel work across
//   curves, just like Stage 2.
//
//   A companion helper (`ecm_stage2_parallel.hpp`, W8 T1) already exists for
//   Stage 2 BSGS. The Stage 1 helper mirrors its API and behaviour so callers
//   can dispatch both stages with identical patterns. The two helpers are
//   strictly orthogonal: enabling one does not affect the other, and a
//   caller may opt-in to Stage 1 parallelism while leaving Stage 2 sequential
//   (or vice-versa).
//
// This helper centralises the env-gated dispatch:
//
//   GNFS_ECM_STAGE1_PARALLEL_THREADS = N (default 1, range [1, hw * 2])
//
// N = 1 keeps the sequential path bit-for-bit (no ThreadPool created, zero
// overhead). N >= 2 spawns N pool workers and submits each curve's Stage 1
// run as an independent task.
//
// Algorithmic equivalence (strict invariant):
//   * Per-curve Stage 1 arithmetic is a pure function of (sigma, n, B1).
//     Distinct sigmas share no mutable state, so dispatch order cannot
//     change either curve's result.
//   * GMP `mpz_*` operations are thread-safe when operands are disjoint per
//     call. Each curve task owns its own Integer buffers, satisfying GMP's
//     per-call disjoint-operands requirement.
//   * The dispatcher returns a `std::vector<Result>` of per-curve outcomes
//     aligned with the input span. Callers may use any Result type
//     (typically `std::optional<Integer>` for "factor found / not found"
//     semantics, or a richer struct carrying post-Stage-1 Point state for
//     onward Stage 2 dispatch).
//
// Non-goals:
//   * We do NOT change Stage 2 behaviour. The Stage 2 helper
//     (`parallel_stage2_curves`) remains unchanged.
//   * We do NOT change the `ECM::factor` / `ECM::quick_factor` /
//     `ECM::factor_with_batch` public path. The dispatcher is an opt-in
//     helper for callers that explicitly want multi-curve Stage 1
//     concurrency.
//   * We do NOT touch the per-curve Stage 1 inner loops (Lucas chain /
//     Montgomery ladder). Those remain bit-identical; only outer dispatch
//     changes.

#include "../util/thread_pool.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <new>
#include <mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

namespace gnfs::cofactor {

namespace detail {

/// Cached env-parsed thread count for Stage 1 multi-curve dispatch. Reset via
/// `ecm_stage1_parallel_reset_env_cache_for_testing()` for unit testing under
/// different env values.
struct EcmStage1ParallelCache {
    std::once_flag once;
    std::size_t value = 1;
};

inline EcmStage1ParallelCache& ecm_stage1_parallel_cache() noexcept {
    static EcmStage1ParallelCache cache;
    return cache;
}

/// Parse `GNFS_ECM_STAGE1_PARALLEL_THREADS`. Returns 1 (sequential) on:
///   - ENV unset / empty / non-numeric / non-positive
/// Otherwise returns the parsed value, clamped to
/// [1, hardware_concurrency * 2] (with a fallback cap of 16 when
/// hardware_concurrency() reports 0 -> hw=4).
///
/// Parser semantics mirror `parse_ecm_stage2_parallel_env()` exactly to keep
/// W8 T1 / W9 T1 ENV behaviour consistent: uses `std::atoi` so a non-numeric
/// prefix returns 0 (-> 1), but a partial parse like "12abc" returns 12
/// (atoi accepts a leading numeric prefix). Empty / unset / "0" / negative
/// all collapse to the sequential default.
inline std::size_t parse_ecm_stage1_parallel_env() noexcept {
    const char* env = std::getenv("GNFS_ECM_STAGE1_PARALLEL_THREADS");
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

/// Read the `GNFS_ECM_STAGE1_PARALLEL_THREADS` env into a cached thread
/// count.
///
/// First call parses the env once (via `std::once_flag`); subsequent calls
/// return the cached value. Range: [1, hardware_concurrency() * 2]. Default
/// (unset / "" / non-numeric / <= 0): 1 (sequential). Out-of-range high
/// values clamp to the upper cap.
[[nodiscard]] inline std::size_t ecm_stage1_parallel_threads() noexcept {
    auto& cache = detail::ecm_stage1_parallel_cache();
    std::call_once(cache.once, [&cache]() {
        cache.value = detail::parse_ecm_stage1_parallel_env();
    });
    return cache.value;
}

/// Reset the cached thread count. Intended for unit tests that toggle
/// `GNFS_ECM_STAGE1_PARALLEL_THREADS` between assertions.
///
/// Not thread-safe; only call when no `parallel_stage1_curves` is in flight.
inline void ecm_stage1_parallel_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::ecm_stage1_parallel_cache();
    // Reconstruct the once_flag + value in place. We avoid std::atomic
    // because the helper is meant to be called between test cases where
    // single-threaded use is guaranteed.
    cache.~EcmStage1ParallelCache();
    new (&cache) detail::EcmStage1ParallelCache();
}

/// Parallelize Stage 1 work across an independent set of ECM curves.
///
/// `curves` is a span of per-curve setup tuples (typically `(sigma, n, B1)`
/// or a small struct holding everything Stage 1 needs to start). `run_stage1
/// (curve)` performs Stage 1 for one curve and returns its outcome of caller-
/// chosen type `Result` (commonly `std::optional<Integer>` for "factor
/// found / not found" semantics, or a richer struct carrying the post-
/// Stage-1 Point state for onward Stage 2 dispatch).
///
/// `run_stage1` MUST be a pure function over per-curve state: it may read
/// shared read-only inputs (`n`, B1, BatchContext primes_cache, etc.)
/// captured by reference, but MUST NOT write to any shared mutable state.
/// GMP `mpz_*` calls in the lambda must operate on per-curve `Integer`
/// buffers (the lambda's own local scratch).
///
/// Returns a `std::vector<Result>` of per-curve outcomes in input order.
/// Callers that want "first success wins" short-circuit semantics can sweep
/// the returned vector linearly.
///
/// Behavior:
///   - threads == 1 (default):  sequential for-loop, no ThreadPool created
///   - threads >= 2:            ThreadPool dispatch via `submit()` +
///                              `future.get()`
///   - empty curves span:       returns empty vector (no pool created)
///   - single curve:            always sequential (no ThreadPool overhead
///                              even when threads >= 2)
///
/// `Result` must be default-constructible and assignable; per-slot writes
/// are race-free because each task owns exactly one disjoint output index.
template <typename Result, typename Curve, typename Func>
inline std::vector<Result>
parallel_stage1_curves(std::span<const Curve> curves, Func&& run_stage1) {
    const std::size_t n = curves.size();
    std::vector<Result> results;
    if (n == 0) return results;

    results.resize(n);

    const std::size_t threads = ecm_stage1_parallel_threads();

    // Sequential path: zero overhead, preserves original behavior
    // bit-for-bit (no pool spawn, no future overhead).
    if (threads <= 1 || n == 1) {
        for (std::size_t i = 0; i < n; ++i) {
            results[i] = run_stage1(curves[i]);
        }
        return results;
    }

    // Parallel path: bound pool size by min(threads, curves). Spawning
    // more workers than curves wastes resources.
    const std::size_t pool_size = (threads < n) ? threads : n;
    gnfs::util::ThreadPool pool(static_cast<uint32_t>(pool_size));

    std::vector<std::future<void>> futures;
    futures.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        // Each task captures curve index + reference to the curves span
        // and the results vector. Per-curve slots in `results` are
        // disjoint, so concurrent writes to results[i] are race-free.
        futures.push_back(pool.submit([&curves, &results, &run_stage1, i]() {
            results[i] = run_stage1(curves[i]);
        }));
    }

    // Propagate any task exception to the caller via future::get().
    for (auto& f : futures) {
        f.get();
    }

    return results;
}

}  // namespace gnfs::cofactor
