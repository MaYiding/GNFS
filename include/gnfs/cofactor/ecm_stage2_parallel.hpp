#pragma once

// ECM Stage 2 (Baby-Step Giant-Step) multi-curve parallel dispatch.
//
// Background:
//   ECM Stage 1 multi-curve concurrency already exists via `EcmCurvePool`
//   (see `ecm_curve_pool.hpp`), which pre-builds Suyama-parametrised curves
//   under a small ThreadPool so the hot retry loop can pop ready-to-use
//   curves. Stage 2 (the Baby-Step Giant-Step phase, plus the optional
//   Brent-Suyama polynomial extension) currently runs sequentially per
//   curve inside `ECM::stage2()` / `ECM::stage2_brent_suyama()`. For
//   moderate B2 (50d+/60d cofactors) Stage 2 wall-time dominates Stage 1,
//   so dispatching distinct curves' Stage 2 work in parallel yields real
//   ROI without touching Stage 1 behaviour.
//
// This helper centralises the env-gated dispatch:
//
//   GNFS_ECM_STAGE2_PARALLEL = N (default 1, range [1, hardware_concurrency * 2])
//
// N = 1 keeps the sequential path bit-for-bit (no ThreadPool created, zero
// overhead). N >= 2 spawns N pool workers and submits each curve's Stage 2
// run as an independent task.
//
// Algorithmic equivalence (strict invariant):
//   * Per-curve Stage 2 arithmetic is a pure function of (sigma, n, B1, B2,
//     a24, post-Stage-1 point Q). When two curves use distinct sigmas they
//     share no mutable state, so dispatch order cannot change either
//     curve's result.
//   * GMP `mpz_*` operations are thread-safe when operands are disjoint
//     per call (which is satisfied here: each curve task owns its own
//     Integer buffers and point state).
//   * The dispatcher returns a `std::vector<std::optional<Integer>>` of
//     per-curve outcomes in input order. Callers that originally iterated
//     "first non-null wins" should sweep the returned vector in input
//     order and take the first non-null, which is identical to the
//     sequential short-circuit semantics for a fixed sigma list.
//
// Non-goals:
//   * We do NOT change Stage 1 behaviour. The Stage 1 helper
//     (`EcmCurvePool` + `try_curve_with_pk`) remains unchanged.
//   * We do NOT change the `ECM::factor` / `ECM::quick_factor` / `ECM::
//     factor_with_batch` public path. The dispatcher is an opt-in helper
//     for callers that explicitly want multi-curve Stage 2 concurrency.
//   * We do NOT touch the per-curve Stage 2 inner loops (BSGS, Brent-
//     Suyama). Those remain bit-identical; only outer dispatch changes.

#include "../util/thread_pool.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <new>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace gnfs::cofactor {

namespace detail {

/// Cached env-parsed thread count. Reset via
/// `ecm_stage2_parallel_reset_env_cache_for_testing()` for unit testing
/// under different env values.
struct EcmStage2ParallelCache {
    std::once_flag once;
    std::size_t value = 1;
};

inline EcmStage2ParallelCache& ecm_stage2_parallel_cache() noexcept {
    static EcmStage2ParallelCache cache;
    return cache;
}

/// Parse `GNFS_ECM_STAGE2_PARALLEL`. Returns 1 (sequential) on:
///   - ENV unset / empty / non-numeric / non-positive
/// Otherwise returns the parsed value, clamped to
/// [1, hardware_concurrency * 2] (with a fallback of 4 when
/// hardware_concurrency() reports 0).
inline std::size_t parse_ecm_stage2_parallel_env() noexcept {
    const char* env = std::getenv("GNFS_ECM_STAGE2_PARALLEL");
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

/// Read the `GNFS_ECM_STAGE2_PARALLEL` env into a cached thread count.
///
/// First call parses the env once (via `std::once_flag`); subsequent calls
/// return the cached value. Range: [1, hardware_concurrency() * 2].
/// Default (unset / "" / non-numeric / <= 0): 1 (sequential). Out-of-range
/// high values clamp to the upper cap.
[[nodiscard]] inline std::size_t ecm_stage2_parallel_threads() noexcept {
    auto& cache = detail::ecm_stage2_parallel_cache();
    std::call_once(cache.once, [&cache]() {
        cache.value = detail::parse_ecm_stage2_parallel_env();
    });
    return cache.value;
}

/// Reset the cached thread count. Intended for unit tests that toggle
/// `GNFS_ECM_STAGE2_PARALLEL` between assertions.
///
/// Not thread-safe; only call when no `parallel_stage2_curves` is in
/// flight.
inline void ecm_stage2_parallel_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::ecm_stage2_parallel_cache();
    // Reconstruct the once_flag + value in place. We avoid std::atomic
    // because the helper is meant to be called between test cases where
    // single-threaded use is guaranteed.
    cache.~EcmStage2ParallelCache();
    new (&cache) detail::EcmStage2ParallelCache();
}

/// Parallelize Stage 2 work across an independent set of ECM curves.
///
/// `curves` is a span of per-curve state (typically a small struct holding
/// the post-Stage-1 point + per-curve parameters). `run_stage2(curve, idx)`
/// performs Stage 2 for one curve and returns its outcome
/// (`std::optional<Result>`).
///
/// `run_stage2` MUST be a pure function over per-curve state: it may read
/// shared read-only inputs (`n`, BatchContext, etc.) captured by reference,
/// but MUST NOT write to any shared mutable state. GMP `mpz_*` calls in
/// the lambda must operate on per-curve `Integer` buffers (the lambda's
/// own local scratch).
///
/// Returns a `std::vector<std::optional<Result>>` of per-curve outcomes in
/// input order. Callers that want "first non-null wins" short-circuit
/// semantics can sweep the returned vector linearly.
///
/// Behavior:
///   - threads == 1 (default):  sequential for-loop, no ThreadPool created
///   - threads >= 2:            ThreadPool dispatch via `submit()` +
///                              `future.get()`
///   - empty curves span:       returns empty vector (no pool created)
///   - single curve:            always sequential (no ThreadPool overhead
///                              even when threads >= 2)
template <typename Result, typename Curve, typename Func>
inline std::vector<std::optional<Result>>
parallel_stage2_curves(std::span<Curve> curves, Func&& run_stage2) {
    const std::size_t n = curves.size();
    std::vector<std::optional<Result>> results;
    if (n == 0) return results;

    results.resize(n);

    const std::size_t threads = ecm_stage2_parallel_threads();

    // Sequential path: zero overhead, preserves original behavior
    // bit-for-bit (no pool spawn, no future overhead).
    if (threads <= 1 || n == 1) {
        for (std::size_t i = 0; i < n; ++i) {
            results[i] = run_stage2(curves[i], i);
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
        futures.push_back(pool.submit([&curves, &results, &run_stage2, i]() {
            results[i] = run_stage2(curves[i], i);
        }));
    }

    // Propagate any task exception to the caller via future::get().
    for (auto& f : futures) {
        f.get();
    }

    return results;
}

}  // namespace gnfs::cofactor
