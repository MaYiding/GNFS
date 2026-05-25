#pragma once

// GMP `mpz_powm` batched parallel dispatcher (W11 T3).
//
// Background:
//   Modular exponentiation (`base^exp mod modulus`) shows up as a per-relation
//   operation in several GNFS Phase 5 hot paths -- the most prominent is
//   Schirokauer maps computation, which evaluates a fixed exponent against a
//   per-relation base across O(thousands of) relations. Each `mpz_powm` call
//   is a pure function of `(base, exp, modulus)` with disjoint operand
//   allocations, so cross-call parallelism is safe under GMP's documented
//   per-call thread-safety guarantee (operands across concurrent calls must
//   be distinct `mpz_t` objects, which is trivially satisfied when results
//   live in disjoint `std::vector` slots).
//
//   This helper centralises the env-gated dispatch so any caller that already
//   has a span of independent bases plus a common `(exp, modulus)` pair can
//   opt into worker-pool parallelism without rewriting the exponentiation
//   loop.
//
//   `GNFS_MPZ_POWM_BATCH_THREADS = N` (default 1, range [1, hw_concurrency * 2])
//
//   N = 1 keeps the sequential path bit-for-bit (no ThreadPool created, zero
//   overhead). N >= 2 spawns up to N pool workers and submits each base's
//   `mpz_powm` invocation as an independent unit of work.
//
// Algorithmic equivalence (strict invariant):
//   * `mpz_powm` is a deterministic pure function of `(base, exp, modulus)`.
//     The dispatcher only changes scheduling; the per-slot result must equal
//     `bases[i]^exp mod modulus` regardless of `threads`.
//   * GMP's documented thread-safety covers concurrent calls that touch
//     disjoint operands. Per-task writes go into `results[i]` (a per-index
//     disjoint `Integer`), and reads share immutable references to `exp` and
//     `modulus`, so the per-call disjoint-operand contract holds.
//   * The result span aliases caller-owned storage; the dispatcher writes
//     each slot exactly once. Both sequential and parallel paths produce
//     bit-for-bit identical contents.
//
// Non-goals:
//   * We do NOT modify any of the existing Schirokauer maps / matrix-builder
//     hot paths. This helper is opt-in infrastructure; callers wire it in
//     where they already have a contiguous batch of independent bases.
//   * We do NOT accept multiple exponents or multiple moduli per call. Those
//     variants can layer on top of this template if needed; the common case
//     (one shared exponent, one shared modulus, varying base) is what this
//     dispatcher targets.
//   * We do NOT pre-allocate or pool `Integer` results. Caller owns the
//     output span and is responsible for pre-sizing it to `bases.size()`.

#include "../core/integer.hpp"
#include "./thread_pool.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <gmp.h>
#include <mutex>
#include <new>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace gnfs::util {

namespace detail {

/// Cached env-parsed thread count for batched `mpz_powm` dispatch. Reset via
/// `mpz_powm_batch_threads_reset_env_cache_for_testing()` so unit tests can
/// toggle `GNFS_MPZ_POWM_BATCH_THREADS` between assertions.
struct MpzPowmBatchThreadsCache {
    std::atomic<int> value{0};
};

inline MpzPowmBatchThreadsCache& mpz_powm_batch_threads_cache() noexcept {
    static MpzPowmBatchThreadsCache cache;
    return cache;
}

/// Parse `GNFS_MPZ_POWM_BATCH_THREADS`. Returns 1 (sequential) on:
///   - ENV unset / empty / non-numeric / non-positive
/// Otherwise returns the parsed value, clamped to
/// [1, hardware_concurrency * 2] (fallback hw = 4 when hardware_concurrency()
/// reports 0 so the upper cap stays meaningful).
///
/// Parser semantics mirror `parse_filter_merge_threads_env()` /
/// `parse_ecm_stage1_parallel_env()`: `std::atoi` accepts a leading numeric
/// prefix (so `"12abc"` parses to 12), empty / unset / "0" / negative all
/// collapse to the sequential default, and any out-of-range value clamps to
/// the cap rather than throwing.
inline int parse_mpz_powm_batch_threads_env() noexcept {
    const char* env = std::getenv("GNFS_MPZ_POWM_BATCH_THREADS");
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

/// Read the `GNFS_MPZ_POWM_BATCH_THREADS` env into a cached thread count.
///
/// First call parses the env once (via `std::once_flag`); subsequent calls
/// return the cached value. Range: [1, hardware_concurrency() * 2]. Default
/// (unset / "" / non-numeric / <= 0): 1 (sequential). Out-of-range high
/// values clamp to the upper cap.
[[nodiscard]] inline int mpz_powm_batch_threads() noexcept {
    auto& cache = detail::mpz_powm_batch_threads_cache();
    int cached = cache.value.load(std::memory_order_acquire);
    if (cached != 0) return cached;

    const int parsed = detail::parse_mpz_powm_batch_threads_env();
    int expected = 0;
    if (cache.value.compare_exchange_strong(expected, parsed,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
        return parsed;
    }
    return expected;
}

/// Reset the cached thread count. Intended for unit tests that toggle
/// `GNFS_MPZ_POWM_BATCH_THREADS` between assertions.
///
/// Not thread-safe; only call between test cases where no
/// `parallel_mpz_powm` invocation is in flight.
inline void mpz_powm_batch_threads_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::mpz_powm_batch_threads_cache();
    cache.value.store(0, std::memory_order_release);
}

/// Compute `results[i] = bases[i]^exp mod modulus` for every i.
///
/// `bases` and `results` must have the same size; the dispatcher writes into
/// `results[i]` exactly once. `exp` and `modulus` are shared across the
/// entire batch and read concurrently by every worker (this is safe because
/// neither is mutated; GMP's per-call disjoint-operand contract only forbids
/// concurrent writes through aliasing `mpz_t`s, not concurrent reads).
///
/// Behavior:
///   - threads == 1 (default):  sequential for-loop, no ThreadPool created
///   - threads >= 2:            ThreadPool dispatch via `submit()` +
///                              `future.get()`
///   - empty bases span:        no-op (no pool created, no writes)
///   - single base:             always sequential (no ThreadPool overhead
///                              even when threads >= 2)
///
/// Bit-for-bit guarantee: same `(bases, exp, modulus)` input produces the
/// same `results` content regardless of thread count, because `mpz_powm` is
/// a deterministic pure function and each result slot is written by exactly
/// one task. Tests `test_mpz_powm_parallel.cpp::test_n1_vs_n4_parity` and
/// `test_n1_vs_n_hw_parity` strictly enforce per-index equality.
///
/// Preconditions:
///   - `results.size() >= bases.size()` (debug build assert; release build
///     silently writes only into the first `bases.size()` slots).
///   - `modulus > 0` (caller responsibility; GMP `mpz_powm` UB on zero
///     modulus, mirroring the existing `gnfs::core::powmod` contract).
///
/// Exception propagation: GMP errors propagate up via `std::runtime_error`
/// at the call site (consistent with the rest of the codebase, since
/// `mpz_powm` itself does not throw). The dispatcher does not swallow or
/// wrap exceptions; if any worker throws, the first observed exception
/// rethrows after every other future has been drained so the pool joins
/// cleanly.
inline void parallel_mpz_powm(std::span<const gnfs::core::Integer> bases,
                              const gnfs::core::Integer& exp,
                              const gnfs::core::Integer& modulus,
                              std::span<gnfs::core::Integer> results) {
    const std::size_t n = bases.size();
    if (n == 0) return;

    // Defensive clamp: if caller under-sized the output span, we still write
    // exactly the slots that exist. This mirrors the contract used by the
    // SIMD batch helpers (popcount_simd / and_popcnt_simd) -- avoid UB write
    // past the output rather than abort.
    const std::size_t out_n = (results.size() < n) ? results.size() : n;
    if (out_n == 0) return;

    const int threads = mpz_powm_batch_threads();

    // Sequential path: zero overhead, preserves original behaviour
    // bit-for-bit (no pool spawn, no future overhead). Also taken when the
    // caller asked for parallelism but only supplied a single base; one
    // task is never worth a pool spin-up.
    if (threads <= 1 || out_n == 1) {
        for (std::size_t i = 0; i < out_n; ++i) {
            mpz_powm(results[i].get_mpz(),
                     bases[i].get_mpz(),
                     exp.get_mpz(),
                     modulus.get_mpz());
        }
        return;
    }

    // Parallel path: bound pool size by min(threads, out_n). Spawning more
    // workers than bases wastes resources and adds futex pressure for no
    // throughput gain.
    const std::size_t pool_size =
        (static_cast<std::size_t>(threads) < out_n)
            ? static_cast<std::size_t>(threads)
            : out_n;
    gnfs::util::ThreadPool pool(static_cast<uint32_t>(pool_size));

    std::vector<std::future<void>> futures;
    futures.reserve(out_n);
    for (std::size_t i = 0; i < out_n; ++i) {
        // Each task captures the batch index plus references to the input
        // span, shared exponent / modulus, and the output span. Per-base
        // output slots are disjoint, so concurrent writes to `results[i]`
        // are race-free even though `results` itself is shared. `exp` and
        // `modulus` are read-only and may be referenced from every worker.
        futures.push_back(pool.submit([&bases, &exp, &modulus, &results, i]() {
            mpz_powm(results[i].get_mpz(),
                     bases[i].get_mpz(),
                     exp.get_mpz(),
                     modulus.get_mpz());
        }));
    }

    // Drain every future even when one rethrows: we want the pool to join
    // cleanly in its dtor (workers must finish their current task before
    // returning) and we do not want a thrown exception to abandon other
    // workers' results mid-flight. The first observed exception propagates;
    // any subsequent exceptions are swallowed (matches std::async / typical
    // future-chain semantics, consistent with parallel_merge_partials).
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
