#pragma once

// GMP `mpz_gcd` batched parallel dispatcher (W14 T5).
//
// Sister helper of `mpz_powm_parallel.hpp` (W11 T3), `mpz_invert_parallel.hpp`
// (W12 T3), and `mpz_mod_parallel.hpp` (W13 T5). Same parallel-dispatcher
// template -- the difference is in the underlying GMP primitive and in the
// shape of the inputs: each call reduces an independent `(a, b)` pair down
// to `gcd(a, b)`, so two parallel input spans replace the single
// `dividends` / shared-`modulus` pattern of the previous three helpers.
//
// Background:
//   The GNFS pipeline has many sites where independent gcd computations
//   accumulate: relation filtering (`gcd(a - b*m, N) > 1` rejection over
//   millions of relations), Schirokauer / lattice-basis Bezout coefficients,
//   ECM "lucky factor" extraction in batched Montgomery inversion (when
//   `mpz_invert` returns 0 we fall back to scanning gcd(v_i, n)), and
//   Cantor-Zassenhaus root finding cross-coefficients. Each `mpz_gcd(out,
//   a, b)` call is a deterministic pure function of `(a, b)` with disjoint
//   operand allocations, satisfying GMP's documented per-call thread-safety
//   contract (concurrent reads are safe, only concurrent writes through
//   aliasing `mpz_t` are forbidden).
//
//   This helper centralises the env-gated dispatch so any caller that
//   already has a batch of independent `(a, b)` pairs can opt into
//   worker-pool parallelism without rewriting the reduction loop.
//
//   `GNFS_MPZ_GCD_BATCH_THREADS = N` (default 1, range [1, hw_concurrency * 2])
//
//   N = 1 keeps the sequential path bit-for-bit (no ThreadPool created, zero
//   overhead). N >= 2 spawns up to N pool workers and submits each gcd
//   invocation as an independent unit of work.
//
// No failure semantics (matches W13 T5 mpz_mod, differs from W12 T3 mpz_invert):
//   `mpz_gcd(out, a, b)` is total over its entire domain. Standard GMP
//   conventions apply: gcd(0, 0) = 0, gcd(a, 0) = |a|, gcd(0, b) = |b|, and
//   the result is always non-negative regardless of the signs of `a` and
//   `b`. The operation never fails, so the dispatcher returns `void` and
//   does not produce a per-slot success vector. (Compare with W12 T3
//   `parallel_mpz_invert`, whose `std::vector<bool>` exposes the
//   `gcd(base, modulus) > 1` "no inverse exists" branch.)
//
// Difference vs W13 T5 mpz_mod (sibling helper):
//   * `mpz_mod` consumes a vector of dividends plus a *shared* modulus. The
//     dispatcher reads the same `modulus` from every worker.
//   * `mpz_gcd` consumes two parallel vectors of operands -- both inputs
//     vary per-index. Workers still only read their own `(a_values[i],
//     b_values[i])` operands plus write their own `results[i]` slot, so
//     the GMP thread-safety contract is identical (per-call disjoint
//     operands), but the surface area exposes two input spans.
//   * `mpz_gcd` handles sign-extension differently from `mpz_mod`: it
//     ignores the signs of both operands and always produces a non-negative
//     result, whereas mpz_mod produces a non-negative residue specifically
//     in `[0, modulus)`.
//
// Algorithmic equivalence (strict invariant):
//   * `mpz_gcd` is a deterministic function of `(a, b)`. The dispatcher
//     only changes scheduling; per-slot results must equal
//     `mpz_gcd(out, a_values[i], b_values[i])` regardless of `threads`.
//   * GMP's documented thread-safety covers concurrent calls that touch
//     disjoint operands. Per-task writes go into `results[i]` (a per-index
//     disjoint `Integer`), and reads access two per-index disjoint slots
//     in `a_values` / `b_values`, so the per-call disjoint-operand
//     contract holds.
//   * The `results` vector aliases caller-owned storage; the dispatcher
//     writes each slot exactly once. Both sequential and parallel paths
//     produce bit-for-bit identical contents.
//
// Family membership:
//   Member 10 of the W7-W14 parallel-dispatcher family
//   (W7 `GNFS_SQRT_HENSEL_THREADS`,
//    W8 T1 `GNFS_ECM_STAGE2_PARALLEL`,
//    W9 T1 `GNFS_ECM_STAGE1_PARALLEL_THREADS`,
//    W10 T4 `GNFS_FILTER_MERGE_THREADS`,
//    W11 T3 `GNFS_MPZ_POWM_BATCH_THREADS`,
//    W11 T4 `GNFS_LATTICE_BASIS_PARALLEL_THREADS`,
//    W12 T3 `GNFS_MPZ_INVERT_BATCH_THREADS`,
//    W12 T4 `GNFS_SIEVE_APPLY_TILE_THREADS`,
//    W13 T5 `GNFS_MPZ_MOD_BATCH_THREADS`,
//    W14 T5 `GNFS_MPZ_GCD_BATCH_THREADS`). All ten share the same env
//   parsing semantics, ThreadPool ownership pattern, and exception-drain
//   contract. They are individually opt-in (default sequential) and remain
//   fully orthogonal -- enabling several at once causes no interference.
//
// Non-goals:
//   * We do NOT modify any of the existing gcd call-sites or any
//     `gnfs::core::Integer` arithmetic operator. This helper is opt-in
//     infrastructure; callers wire it in where they already have a
//     contiguous batch of independent `(a, b)` pairs.
//   * We do NOT pre-allocate or pool `Integer` results. Caller owns the
//     output vector; the dispatcher defensively resizes it to
//     `a_values.size()` when needed so a missized caller still gets a
//     well-defined result.
//   * `a_values`, `b_values`, and `results` must not alias the same
//     storage (caller contract; the dispatcher does not detect or handle
//     aliasing).
//   * We do NOT extend to extended-GCD (`mpz_gcdext`). That would require
//     two extra Bezout coefficient outputs per index, doubling the slot
//     count and complicating the dispatch shape. A future helper can layer
//     on top of this template if the need arises.

#include "../core/integer.hpp"
#include "./thread_pool.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <gmp.h>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace gnfs::util {

namespace detail {

/// Cached env-parsed thread count for batched `mpz_gcd` dispatch. Reset via
/// `mpz_gcd_batch_threads_reset_env_cache_for_testing()` so unit tests can
/// toggle `GNFS_MPZ_GCD_BATCH_THREADS` between assertions.
struct MpzGcdBatchThreadsCache {
    std::atomic<int> value{0};
};

inline MpzGcdBatchThreadsCache& mpz_gcd_batch_threads_cache() noexcept {
    static MpzGcdBatchThreadsCache cache;
    return cache;
}

/// Parse `GNFS_MPZ_GCD_BATCH_THREADS`. Returns 1 (sequential) on:
///   - ENV unset / empty / non-numeric / non-positive
/// Otherwise returns the parsed value, clamped to
/// [1, hardware_concurrency * 2] (fallback hw = 4 when hardware_concurrency()
/// reports 0 so the upper cap stays meaningful).
///
/// Parser semantics mirror the rest of the parallel-dispatcher family
/// (W11 T3 / W12 T3 / W13 T5): `std::atoi` accepts a leading numeric prefix
/// (so `"12abc"` parses to 12 and `"  4"` parses to 4 because atoi consumes
/// leading whitespace), empty / unset / "0" / negative all collapse to the
/// sequential default, and any out-of-range value clamps to the cap rather
/// than throwing.
inline int parse_mpz_gcd_batch_threads_env() noexcept {
    const char* env = std::getenv("GNFS_MPZ_GCD_BATCH_THREADS");
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

/// Read the `GNFS_MPZ_GCD_BATCH_THREADS` env into a cached thread count.
///
/// First call parses the env once (via `std::once_flag`); subsequent calls
/// return the cached value. Range: [1, hardware_concurrency() * 2]. Default
/// (unset / "" / non-numeric / <= 0): 1 (sequential). Out-of-range high
/// values clamp to the upper cap.
[[nodiscard]] inline int mpz_gcd_batch_threads() noexcept {
    auto& cache = detail::mpz_gcd_batch_threads_cache();
    int cached = cache.value.load(std::memory_order_acquire);
    if (cached != 0) return cached;

    const int parsed = detail::parse_mpz_gcd_batch_threads_env();
    int expected = 0;
    if (cache.value.compare_exchange_strong(expected, parsed,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
        return parsed;
    }
    return expected;
}

/// Resolve the effective thread count given a specific batch size.
///
/// Returns `min(threads, batch_size)` so callers can decide upfront whether
/// they should bother building per-task scratch state. Useful for callers
/// that want to short-circuit further setup work when the dispatcher will
/// degrade to a single-task sequential run anyway. Empty batch returns 0
/// (no workers needed at all).
[[nodiscard]] inline int resolve_mpz_gcd_batch_threads(std::size_t batch_size) noexcept {
    int threads = mpz_gcd_batch_threads();
    if (batch_size == 0) return 0;
    if (threads <= 1) return 1;
    auto b = static_cast<std::size_t>(threads);
    if (b > batch_size) b = batch_size;
    return static_cast<int>(b);
}

/// Reset the cached thread count. Intended for unit tests that toggle
/// `GNFS_MPZ_GCD_BATCH_THREADS` between assertions.
///
/// Not thread-safe; only call between test cases where no
/// `parallel_mpz_gcd` invocation is in flight.
inline void mpz_gcd_batch_threads_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::mpz_gcd_batch_threads_cache();
    cache.value.store(0, std::memory_order_release);
}

/// Compute `results[i] = gcd(a_values[i], b_values[i])` for every i.
///
/// Resizes `results` to match `a_values.size()` if undersized (each new slot
/// is default-constructed `Integer(0)`). When the caller's `results.size()`
/// is *less* than `a_values.size()`, the dispatcher defensively clamps to
/// `results.size()` to avoid UB-writing past the output -- the trailing
/// indices simply do not produce gcd output. This matches the contract used
/// by W12 T3 `parallel_mpz_invert` and the SIMD batch helpers.
///
/// `a_values.size()` must equal `b_values.size()`. A length mismatch throws
/// `std::invalid_argument` because the result vector cannot be sensibly
/// shaped otherwise (unlike `results`, which can be defensively clamped --
/// the two input spans encode a logical pairing where each index must have
/// both `a` and `b` defined).
///
/// Behavior:
///   - threads == 1 (default): sequential for-loop, no ThreadPool created
///   - threads >= 2:           ThreadPool dispatch via `submit()` +
///                             `future.get()`
///   - empty input vectors:    no-op (no pool created, no writes)
///   - single pair:            always sequential (no ThreadPool overhead
///                             even when threads >= 2)
///
/// Bit-for-bit guarantee: same `(a_values, b_values)` input produces the
/// same `results` content regardless of thread count, because `mpz_gcd` is a
/// deterministic function and each result slot is written by exactly one
/// task. Tests `test_mpz_gcd_parallel.cpp::test_n1_vs_n4_parity` and
/// `test_n1_vs_n_hw_parity` strictly enforce per-index equality.
///
/// Total semantics (no failure):
///   GMP `mpz_gcd` is total over its entire domain. Standard conventions:
///     - gcd(0, 0) = 0
///     - gcd(a, 0) = |a|, gcd(0, b) = |b|
///     - Result is always non-negative regardless of the signs of a and b
///   The operation never fails, so this dispatcher returns `void` (unlike
///   W12 T3 `parallel_mpz_invert`, which returns `std::vector<bool>` to
///   expose `mpz_invert`'s "no inverse exists" branch).
///
/// Preconditions:
///   - `a_values.size() == b_values.size()`. Mismatch -> throws
///     `std::invalid_argument`.
///   - `a_values`, `b_values`, and `results` must not alias the same
///     storage (caller contract; the dispatcher does not detect or handle
///     aliasing).
///
/// Exception propagation: GMP itself does not throw on mpz_gcd (the
/// operation is total). The dispatcher does not swallow or wrap exceptions
/// thrown by the underlying Integer / ThreadPool machinery (e.g.,
/// bad_alloc); if any worker throws, the first observed exception rethrows
/// after every other future has been drained so the pool joins cleanly.
inline void parallel_mpz_gcd(const std::vector<gnfs::core::Integer>& a_values,
                             const std::vector<gnfs::core::Integer>& b_values,
                             std::vector<gnfs::core::Integer>& results) {
    if (a_values.size() != b_values.size()) {
        throw std::invalid_argument(
            "parallel_mpz_gcd: a_values.size() must equal b_values.size()");
    }

    const std::size_t n = a_values.size();
    if (n == 0) {
        return;
    }

    // Defensive resize: keep the dispatcher robust when caller passed an
    // under-sized output buffer. The clamp below caps the workload at the
    // *smaller* of `n` and `results.size()` so we never UB-write past the
    // output. If the caller wants every input pair processed, they need to
    // pre-size `results` to at least `n`; otherwise the trailing inputs
    // are silently dropped (matches the contract used by the sister batch
    // helpers).
    const std::size_t effective_n =
        (results.size() < n) ? results.size() : n;
    if (effective_n == 0) {
        return;  // nothing to do (caller passed empty results too)
    }

    const int threads = mpz_gcd_batch_threads();

    // Sequential path: zero overhead, preserves the bit-for-bit reference
    // behaviour (no pool spawn, no future overhead). Also taken when the
    // caller asked for parallelism but only supplied a single pair; one
    // task is never worth a pool spin-up.
    if (threads <= 1 || effective_n == 1) {
        for (std::size_t i = 0; i < effective_n; ++i) {
            mpz_gcd(results[i].get_mpz(),
                    a_values[i].get_mpz(),
                    b_values[i].get_mpz());
        }
        return;
    }

    // Parallel path: bound pool size by min(threads, effective_n). Spawning
    // more workers than pairs wastes resources and adds futex pressure for
    // no throughput gain.
    const std::size_t pool_size =
        (static_cast<std::size_t>(threads) < effective_n)
            ? static_cast<std::size_t>(threads)
            : effective_n;
    gnfs::util::ThreadPool pool(static_cast<uint32_t>(pool_size));

    std::vector<std::future<void>> futures;
    futures.reserve(effective_n);
    for (std::size_t i = 0; i < effective_n; ++i) {
        // Each task captures the batch index plus references to the input
        // vectors and the output vector. Per-pair output slots are
        // disjoint, so concurrent writes to `results[i]` are race-free
        // even though `results` itself is shared. Inputs are read-only
        // from disjoint per-index slots, satisfying GMP's per-call
        // disjoint-operand contract.
        futures.push_back(pool.submit([&a_values, &b_values, &results, i]() {
            mpz_gcd(results[i].get_mpz(),
                    a_values[i].get_mpz(),
                    b_values[i].get_mpz());
        }));
    }

    // Drain every future even when one rethrows: we want the pool to join
    // cleanly in its dtor (workers must finish their current task before
    // returning) and we do not want a thrown exception to abandon other
    // workers' results mid-flight. The first observed exception
    // propagates; any subsequent exceptions are swallowed (matches
    // std::async / typical future-chain semantics, consistent with
    // parallel_merge_partials, parallel_mpz_powm, parallel_mpz_invert,
    // parallel_mpz_mod).
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
