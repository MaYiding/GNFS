#pragma once

// GMP `mpz_mod` batched parallel dispatcher (W13 T5).
//
// Sister helper of `mpz_powm_parallel.hpp` (W11 T3) and
// `mpz_invert_parallel.hpp` (W12 T3). Same parallel-dispatcher template -- the
// only differences are (a) the GMP primitive is `mpz_mod(result, dividend,
// modulus)` (Euclidean reduction into the canonical residue class) and (b)
// callers do NOT need to inspect a success vector: mpz_mod is total over its
// domain and never fails as long as `modulus > 0`, so a void-returning
// dispatch surface is sufficient.
//
// Background:
//   Modular reduction shows up across the GNFS pipeline whenever a batch of
//   independent dividends is reduced against a common modulus -- Schirokauer
//   maps batch normalisation, ECM Montgomery batch accumulators, Cantor-
//   Zassenhaus coefficient reductions, and the rational sqrt batch reduction
//   all match this shape. Each `mpz_mod` call is a deterministic function of
//   `(dividend, modulus)` with disjoint operand allocations, satisfying GMP's
//   documented per-call thread-safety contract.
//
//   This helper centralises the env-gated dispatch so any caller that already
//   has a vector of independent dividends plus a shared modulus can opt into
//   worker-pool parallelism without rewriting the reduction loop.
//
//   `GNFS_MPZ_MOD_BATCH_THREADS = N` (default 1, range [1, hw_concurrency * 2])
//
//   N = 1 keeps the sequential path bit-for-bit (no ThreadPool created, zero
//   overhead). N >= 2 spawns up to N pool workers and submits each dividend's
//   `mpz_mod` invocation as an independent unit of work.
//
// No failure semantics (real difference vs W12 T3 mpz_invert):
//   `mpz_mod(out, dividend, modulus)` is total -- given `modulus > 0` and any
//   `dividend`, the result is the unique canonical residue in `[0, modulus)`.
//   GMP itself never reports a failure for mpz_mod, so the dispatcher returns
//   `void` (no per-slot success bit). This is the key surface-area difference
//   vs W12 T3 `parallel_mpz_invert`, whose `std::vector<bool>` return value
//   exists because `mpz_invert` can fail when `gcd(dividend, modulus) > 1`.
//   Callers who want to interpret a negative dividend should know that GMP's
//   mpz_mod always produces a non-negative result (the canonical class
//   representative), which is also what tests assert.
//
// Algorithmic equivalence (strict invariant):
//   * `mpz_mod` is a deterministic function of `(dividend, modulus)`. The
//     dispatcher only changes scheduling; per-slot results must equal
//     `mpz_mod(out, dividends[i], modulus)` regardless of `threads`.
//   * GMP's documented thread-safety covers concurrent calls that touch
//     disjoint operands. Per-task writes go into `results[i]` (a per-index
//     disjoint `Integer`), and reads share an immutable reference to
//     `modulus`, so the per-call disjoint-operand contract holds.
//   * The `results` vector aliases caller-owned storage; the dispatcher
//     writes each slot exactly once. Both sequential and parallel paths
//     produce bit-for-bit identical contents.
//
// Family membership:
//   Member 9 of the W7-W13 parallel-dispatcher family
//   (W7 `GNFS_SQRT_HENSEL_THREADS`,
//    W8 T1 `GNFS_ECM_STAGE2_PARALLEL`,
//    W9 T1 `GNFS_ECM_STAGE1_PARALLEL_THREADS`,
//    W10 T4 `GNFS_FILTER_MERGE_THREADS`,
//    W11 T3 `GNFS_MPZ_POWM_BATCH_THREADS`,
//    W11 T4 `GNFS_LATTICE_BASIS_PARALLEL_THREADS`,
//    W12 T3 `GNFS_MPZ_INVERT_BATCH_THREADS`,
//    W12 T4 `GNFS_SIEVE_APPLY_TILE_THREADS`,
//    W13 T5 `GNFS_MPZ_MOD_BATCH_THREADS`). All nine share the same env
//   parsing semantics, ThreadPool ownership pattern, and exception-drain
//   contract. They are individually opt-in (default sequential) and remain
//   fully orthogonal -- enabling several at once causes no interference.
//
// Non-goals:
//   * We do NOT modify any of the existing reduction call-sites or any
//     `gnfs::core::Integer` arithmetic operator. This helper is opt-in
//     infrastructure; callers wire it in where they already have a contiguous
//     batch of independent dividends.
//   * We do NOT accept multiple moduli per call. A future variant can layer
//     on top of this template; the common case (one shared modulus, varying
//     dividend) is what this dispatcher targets.
//   * We do NOT pre-allocate or pool `Integer` results. Caller owns the
//     output vector; the dispatcher defensively resizes it to `dividends.size()`
//     when needed so a missized caller still gets a well-defined result.
//   * `dividends` and `results` must not alias the same storage (caller
//     contract; the dispatcher does not detect or handle aliasing).

#include "../core/integer.hpp"
#include "./thread_pool.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <gmp.h>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace gnfs::util {

namespace detail {

/// Cached env-parsed thread count for batched `mpz_mod` dispatch. Reset via
/// `mpz_mod_batch_threads_reset_env_cache_for_testing()` so unit tests can
/// toggle `GNFS_MPZ_MOD_BATCH_THREADS` between assertions.
struct MpzModBatchThreadsCache {
    std::once_flag once;
    int value = 1;
};

inline MpzModBatchThreadsCache& mpz_mod_batch_threads_cache() noexcept {
    static MpzModBatchThreadsCache cache;
    return cache;
}

/// Parse `GNFS_MPZ_MOD_BATCH_THREADS`. Returns 1 (sequential) on:
///   - ENV unset / empty / non-numeric / non-positive
/// Otherwise returns the parsed value, clamped to
/// [1, hardware_concurrency * 2] (fallback hw = 4 when hardware_concurrency()
/// reports 0 so the upper cap stays meaningful).
///
/// Parser semantics mirror `parse_mpz_powm_batch_threads_env()` /
/// `parse_mpz_invert_batch_threads_env()` / `parse_filter_merge_threads_env()`:
/// `std::atoi` accepts a leading numeric prefix (so `"12abc"` parses to 12),
/// empty / unset / "0" / negative all collapse to the sequential default, and
/// any out-of-range value clamps to the cap rather than throwing.
inline int parse_mpz_mod_batch_threads_env() noexcept {
    const char* env = std::getenv("GNFS_MPZ_MOD_BATCH_THREADS");
    if (env == nullptr || env[0] == '\0') {
        return 1;  // default sequential
    }
    int parsed = std::atoi(env);
    if (parsed <= 0) {
        return 1;  // invalid / non-positive -> sequential
    }
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    int cap = static_cast<int>(hw) * 2;
    if (parsed > cap) parsed = cap;
    return parsed;
}

}  // namespace detail

/// Read the `GNFS_MPZ_MOD_BATCH_THREADS` env into a cached thread count.
///
/// First call parses the env once (via `std::once_flag`); subsequent calls
/// return the cached value. Range: [1, hardware_concurrency() * 2]. Default
/// (unset / "" / non-numeric / <= 0): 1 (sequential). Out-of-range high
/// values clamp to the upper cap.
[[nodiscard]] inline int mpz_mod_batch_threads() noexcept {
    auto& cache = detail::mpz_mod_batch_threads_cache();
    std::call_once(cache.once, [&cache]() {
        cache.value = detail::parse_mpz_mod_batch_threads_env();
    });
    return cache.value;
}

/// Resolve the effective thread count given a specific batch size.
///
/// Returns `min(threads, batch_size)` so callers can decide upfront whether
/// they should bother building per-task scratch state. Useful for callers
/// that want to short-circuit further setup work when the dispatcher will
/// degrade to a single-task sequential run anyway.
[[nodiscard]] inline int resolve_mpz_mod_batch_threads(std::size_t batch_size) noexcept {
    int threads = mpz_mod_batch_threads();
    if (batch_size == 0) return 0;
    if (threads <= 1) return 1;
    auto b = static_cast<std::size_t>(threads);
    if (b > batch_size) b = batch_size;
    return static_cast<int>(b);
}

/// Reset the cached thread count. Intended for unit tests that toggle
/// `GNFS_MPZ_MOD_BATCH_THREADS` between assertions.
///
/// Not thread-safe; only call between test cases where no `parallel_mpz_mod`
/// invocation is in flight.
inline void mpz_mod_batch_threads_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::mpz_mod_batch_threads_cache();
    cache.~MpzModBatchThreadsCache();
    new (&cache) detail::MpzModBatchThreadsCache();
}

/// Compute `results[i] = dividends[i] mod modulus` for every i.
///
/// Resizes `results` to match `dividends.size()` if undersized (each new slot
/// is default-constructed `Integer(0)`). `modulus` is shared across the
/// entire batch and read concurrently by every worker (this is safe because
/// modulus is not mutated; GMP's per-call disjoint-operand contract only
/// forbids concurrent writes through aliasing `mpz_t`s, not concurrent
/// reads).
///
/// Behavior:
///   - threads == 1 (default): sequential for-loop, no ThreadPool created
///   - threads >= 2:           ThreadPool dispatch via `submit()` +
///                             `future.get()`
///   - empty dividends vector: no-op (no pool created, no writes)
///   - single dividend:        always sequential (no ThreadPool overhead
///                             even when threads >= 2)
///
/// Bit-for-bit guarantee: same `(dividends, modulus)` input produces the
/// same `results` content regardless of thread count, because `mpz_mod` is a
/// deterministic function and each result slot is written by exactly one
/// task. Tests `test_mpz_mod_parallel.cpp::test_n1_vs_n4_parity` and
/// `test_n1_vs_n_hw_parity` strictly enforce per-index equality.
///
/// Preconditions:
///   - `modulus > 0` (caller responsibility; `mpz_mod` requires a positive
///     modulus, matching the GMP documented contract).
///   - `dividends` and `results` must not alias the same storage (caller
///     contract; the dispatcher does not detect or handle aliasing).
///
/// Exception propagation: GMP itself does not throw on mpz_mod (the
/// operation is total over `(dividend, modulus > 0)`). The dispatcher does
/// not swallow or wrap exceptions thrown by the underlying Integer /
/// ThreadPool machinery (e.g., bad_alloc); if any worker throws, the first
/// observed exception rethrows after every other future has been drained so
/// the pool joins cleanly.
inline void parallel_mpz_mod(const std::vector<gnfs::core::Integer>& dividends,
                             const gnfs::core::Integer& modulus,
                             std::vector<gnfs::core::Integer>& results) {
    const std::size_t n = dividends.size();
    if (n == 0) {
        return;
    }

    // Defensive resize: keep the dispatcher robust when caller passed an
    // under-sized output buffer. This matches the contract used by
    // `parallel_mpz_invert` (W12 T3) and the SIMD batch helpers
    // (popcount_simd / and_popcnt_simd / xor_words_simd) -- never UB-write
    // past the output rather than abort.
    if (results.size() < n) {
        results.resize(n);
    }

    const int threads = mpz_mod_batch_threads();

    // Sequential path: zero overhead, preserves the bit-for-bit reference
    // behaviour (no pool spawn, no future overhead). Also taken when the
    // caller asked for parallelism but only supplied a single dividend; one
    // task is never worth a pool spin-up.
    if (threads <= 1 || n == 1) {
        for (std::size_t i = 0; i < n; ++i) {
            mpz_mod(results[i].get_mpz(),
                    dividends[i].get_mpz(),
                    modulus.get_mpz());
        }
        return;
    }

    // Parallel path: bound pool size by min(threads, n). Spawning more
    // workers than dividends wastes resources and adds futex pressure for no
    // throughput gain.
    const std::size_t pool_size =
        (static_cast<std::size_t>(threads) < n)
            ? static_cast<std::size_t>(threads)
            : n;
    gnfs::util::ThreadPool pool(static_cast<uint32_t>(pool_size));

    std::vector<std::future<void>> futures;
    futures.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        // Each task captures the batch index plus references to the input
        // vector, the shared modulus, and the output vector. Per-dividend
        // output slots are disjoint, so concurrent writes to `results[i]`
        // are race-free even though `results` itself is shared. `modulus`
        // is read-only and may be referenced from every worker.
        futures.push_back(pool.submit([&dividends, &modulus, &results, i]() {
            mpz_mod(results[i].get_mpz(),
                    dividends[i].get_mpz(),
                    modulus.get_mpz());
        }));
    }

    // Drain every future even when one rethrows: we want the pool to join
    // cleanly in its dtor (workers must finish their current task before
    // returning) and we do not want a thrown exception to abandon other
    // workers' results mid-flight. The first observed exception propagates;
    // any subsequent exceptions are swallowed (matches std::async / typical
    // future-chain semantics, consistent with parallel_merge_partials,
    // parallel_mpz_powm, parallel_mpz_invert).
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
}

}  // namespace gnfs::util
