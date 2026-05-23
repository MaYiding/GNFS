#pragma once

// GMP `mpz_mul` batched parallel dispatcher (W15 T5).
//
// Sister helper of `mpz_powm_parallel.hpp` (W11 T3),
// `mpz_invert_parallel.hpp` (W12 T3), `mpz_mod_parallel.hpp` (W13 T5),
// and `mpz_gcd_parallel.hpp` (W14 T5). Same parallel-dispatcher template --
// the difference is in the underlying GMP primitive and in the shape of
// the inputs: each call multiplies an independent `(a, b)` pair to produce
// `a * b`, so two parallel input spans (matching W14 T5 gcd's shape)
// replace the single `dividends` / shared-`modulus` pattern of W11 T3 /
// W12 T3 / W13 T5.
//
// Background:
//   The GNFS pipeline has many sites where independent multiplications
//   accumulate: large-integer accumulators in Schirokauer maps, batched
//   product chains in Cantor-Zassenhaus root finding (cross-coefficient
//   products), prefix-product computation in ECM Montgomery batch
//   inversion (before the inverse, the helper builds p_i = prod(v_0..v_i)
//   one mul at a time), Bezout coefficient multiplications, and lattice
//   basis updates. Each `mpz_mul(out, a, b)` call is a deterministic pure
//   function of `(a, b)` with disjoint operand allocations, satisfying
//   GMP's documented per-call thread-safety contract (concurrent reads are
//   safe, only concurrent writes through aliasing `mpz_t` are forbidden).
//
//   This helper centralises the env-gated dispatch so any caller that
//   already has a batch of independent `(a, b)` pairs can opt into
//   worker-pool parallelism without rewriting the multiplication loop.
//
//   `GNFS_MPZ_MUL_BATCH_THREADS = N` (default 1, range [1, hw_concurrency * 2])
//
//   N = 1 keeps the sequential path bit-for-bit (no ThreadPool created, zero
//   overhead). N >= 2 spawns up to N pool workers and submits each mul
//   invocation as an independent unit of work.
//
// No failure semantics (matches W13 T5 mpz_mod and W14 T5 mpz_gcd,
// differs from W12 T3 mpz_invert):
//   `mpz_mul(out, a, b)` is total over its entire domain. For any pair of
//   integers `(a, b)`, the product `a * b` is well-defined; standard GMP
//   conventions apply (zero handling, sign propagation -- the result's
//   sign is positive iff the two operands' signs agree, zero iff either
//   operand is zero). The operation never fails, so the dispatcher
//   returns `void` and does not produce a per-slot success vector.
//   (Compare with W12 T3 `parallel_mpz_invert`, whose `std::vector<bool>`
//   exposes the `gcd(base, modulus) > 1` "no inverse exists" branch.)
//
// Difference vs W13 T5 mpz_mod (sibling helper, input-shape comparison):
//   * `mpz_mod` consumes a vector of dividends plus a *shared* modulus.
//     The dispatcher reads the same `modulus` from every worker.
//   * `mpz_mul` consumes two parallel vectors of operands -- both inputs
//     vary per-index. Workers still only read their own `(a_values[i],
//     b_values[i])` operands plus write their own `results[i]` slot, so
//     the GMP thread-safety contract is identical (per-call disjoint
//     operands), but the surface area exposes two input spans.
//
// Difference vs W14 T5 mpz_gcd (closest sibling, same input shape):
//   * Input shape is identical (two parallel arrays of `Integer` paired
//     per-index, void return, no failure mode).
//   * The underlying GMP primitive differs: `mpz_gcd` computes the
//     greatest common divisor (ignores signs, always non-negative),
//     while `mpz_mul` computes the algebraic product (preserves sign,
//     can be negative if exactly one operand is negative, zero if either
//     operand is zero).
//   * Typical caller workloads differ: gcd is used for "lucky factor"
//     scans, Bezout, and relation-filter rejection; mul is used for
//     accumulator chains, prefix products, and batched products that
//     would otherwise serialise on a single `mpz_mul` per iteration.
//
// Algorithmic equivalence (strict invariant):
//   * `mpz_mul` is a deterministic function of `(a, b)`. The dispatcher
//     only changes scheduling; per-slot results must equal
//     `mpz_mul(out, a_values[i], b_values[i])` regardless of `threads`.
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
//   Member 11 of the W7-W15 parallel-dispatcher family
//   (W7 `GNFS_SQRT_HENSEL_THREADS`,
//    W8 T1 `GNFS_ECM_STAGE2_PARALLEL`,
//    W9 T1 `GNFS_ECM_STAGE1_PARALLEL_THREADS`,
//    W10 T4 `GNFS_FILTER_MERGE_THREADS`,
//    W11 T3 `GNFS_MPZ_POWM_BATCH_THREADS`,
//    W11 T4 `GNFS_LATTICE_BASIS_PARALLEL_THREADS`,
//    W12 T3 `GNFS_MPZ_INVERT_BATCH_THREADS`,
//    W12 T4 `GNFS_SIEVE_APPLY_TILE_THREADS`,
//    W13 T5 `GNFS_MPZ_MOD_BATCH_THREADS`,
//    W14 T5 `GNFS_MPZ_GCD_BATCH_THREADS`,
//    W15 T5 `GNFS_MPZ_MUL_BATCH_THREADS`). All eleven share the same env
//   parsing semantics, ThreadPool ownership pattern, and exception-drain
//   contract. They are individually opt-in (default sequential) and remain
//   fully orthogonal -- enabling several at once causes no interference.
//
// Non-goals:
//   * We do NOT modify any of the existing mul call-sites or any
//     `gnfs::core::Integer` arithmetic operator. This helper is opt-in
//     infrastructure; callers wire it in where they already have a
//     contiguous batch of independent `(a, b)` pairs.
//   * We do NOT pre-allocate or pool `Integer` results. Caller owns the
//     output vector; the dispatcher defensively clamps to
//     `min(a_values.size(), results.size())` when needed so a missized
//     caller still gets a well-defined result for the prefix it sized.
//   * `a_values`, `b_values`, and `results` must not alias the same
//     storage (caller contract; the dispatcher does not detect or handle
//     aliasing).
//   * We do NOT add a separate squaring variant (`mpz_mul(out, a, a)`).
//     Callers wishing to square should pass `a_values[i] == b_values[i]`
//     explicitly; the GMP runtime decides whether to use a fused squaring
//     kernel internally.

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

/// Cached env-parsed thread count for batched `mpz_mul` dispatch. Reset via
/// `mpz_mul_batch_threads_reset_env_cache_for_testing()` so unit tests can
/// toggle `GNFS_MPZ_MUL_BATCH_THREADS` between assertions.
struct MpzMulBatchThreadsCache {
    std::once_flag once;
    int value = 1;
};

inline MpzMulBatchThreadsCache& mpz_mul_batch_threads_cache() noexcept {
    static MpzMulBatchThreadsCache cache;
    return cache;
}

/// Parse `GNFS_MPZ_MUL_BATCH_THREADS`. Returns 1 (sequential) on:
///   - ENV unset / empty / non-numeric / non-positive
/// Otherwise returns the parsed value, clamped to
/// [1, hardware_concurrency * 2] (fallback hw = 4 when hardware_concurrency()
/// reports 0 so the upper cap stays meaningful).
///
/// Parser semantics mirror the rest of the parallel-dispatcher family
/// (W11 T3 / W12 T3 / W13 T5 / W14 T5): `std::atoi` accepts a leading
/// numeric prefix (so `"12abc"` parses to 12 and `"  4"` parses to 4
/// because atoi consumes leading whitespace), empty / unset / "0" /
/// negative all collapse to the sequential default, and any out-of-range
/// value clamps to the cap rather than throwing.
inline int parse_mpz_mul_batch_threads_env() noexcept {
    const char* env = std::getenv("GNFS_MPZ_MUL_BATCH_THREADS");
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

/// Read the `GNFS_MPZ_MUL_BATCH_THREADS` env into a cached thread count.
///
/// First call parses the env once (via `std::once_flag`); subsequent calls
/// return the cached value. Range: [1, hardware_concurrency() * 2]. Default
/// (unset / "" / non-numeric / <= 0): 1 (sequential). Out-of-range high
/// values clamp to the upper cap.
[[nodiscard]] inline int mpz_mul_batch_threads() noexcept {
    auto& cache = detail::mpz_mul_batch_threads_cache();
    std::call_once(cache.once, [&cache]() {
        cache.value = detail::parse_mpz_mul_batch_threads_env();
    });
    return cache.value;
}

/// Resolve the effective thread count given a specific batch size.
///
/// Returns `min(threads, batch_size)` so callers can decide upfront whether
/// they should bother building per-task scratch state. Useful for callers
/// that want to short-circuit further setup work when the dispatcher will
/// degrade to a single-task sequential run anyway. Empty batch returns 0
/// (no workers needed at all).
[[nodiscard]] inline int resolve_mpz_mul_batch_threads(std::size_t batch_size) noexcept {
    int threads = mpz_mul_batch_threads();
    if (batch_size == 0) return 0;
    if (threads <= 1) return 1;
    auto b = static_cast<std::size_t>(threads);
    if (b > batch_size) b = batch_size;
    return static_cast<int>(b);
}

/// Reset the cached thread count. Intended for unit tests that toggle
/// `GNFS_MPZ_MUL_BATCH_THREADS` between assertions.
///
/// Not thread-safe; only call between test cases where no
/// `parallel_mpz_mul` invocation is in flight.
inline void mpz_mul_batch_threads_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::mpz_mul_batch_threads_cache();
    cache.~MpzMulBatchThreadsCache();
    new (&cache) detail::MpzMulBatchThreadsCache();
}

/// Compute `results[i] = a_values[i] * b_values[i]` for every i.
///
/// `a_values.size()` must equal `b_values.size()`. A length mismatch throws
/// `std::invalid_argument` because the result vector cannot be sensibly
/// shaped otherwise (unlike `results`, which can be defensively clamped --
/// the two input spans encode a logical pairing where each index must have
/// both `a` and `b` defined).
///
/// When the caller's `results.size()` is *less* than `a_values.size()`,
/// the dispatcher defensively clamps to `results.size()` to avoid
/// UB-writing past the output -- the trailing indices simply do not
/// produce mul output. This matches the contract used by W12 T3
/// `parallel_mpz_invert`, W13 T5 `parallel_mpz_mod`, and W14 T5
/// `parallel_mpz_gcd`.
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
/// same `results` content regardless of thread count, because `mpz_mul` is
/// a deterministic function and each result slot is written by exactly
/// one task. Tests `test_mpz_mul_parallel.cpp::test_n1_vs_n4_parity` and
/// `test_n1_vs_n_hw_parity` strictly enforce per-index equality.
///
/// Total semantics (no failure):
///   GMP `mpz_mul` is total over its entire domain. The result is
///   well-defined for any operand pair (positive, negative, zero, or
///   multi-limb). The operation never fails, so this dispatcher returns
///   `void` (unlike W12 T3 `parallel_mpz_invert`, which returns
///   `std::vector<bool>` to expose `mpz_invert`'s "no inverse exists"
///   branch).
///
/// Preconditions:
///   - `a_values.size() == b_values.size()`. Mismatch -> throws
///     `std::invalid_argument`.
///   - `a_values`, `b_values`, and `results` must not alias the same
///     storage (caller contract; the dispatcher does not detect or handle
///     aliasing).
///
/// Exception propagation: GMP itself does not throw on mpz_mul (the
/// operation is total). The dispatcher does not swallow or wrap exceptions
/// thrown by the underlying Integer / ThreadPool machinery (e.g.,
/// bad_alloc); if any worker throws, the first observed exception rethrows
/// after every other future has been drained so the pool joins cleanly.
inline void parallel_mpz_mul(const std::vector<gnfs::core::Integer>& a_values,
                             const std::vector<gnfs::core::Integer>& b_values,
                             std::vector<gnfs::core::Integer>& results) {
    if (a_values.size() != b_values.size()) {
        throw std::invalid_argument(
            "parallel_mpz_mul: a_values.size() must equal b_values.size()");
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
    // helpers W12 T3 / W13 T5 / W14 T5).
    const std::size_t effective_n =
        (results.size() < n) ? results.size() : n;
    if (effective_n == 0) {
        return;  // nothing to do (caller passed empty results too)
    }

    const int threads = mpz_mul_batch_threads();

    // Sequential path: zero overhead, preserves the bit-for-bit reference
    // behaviour (no pool spawn, no future overhead). Also taken when the
    // caller asked for parallelism but only supplied a single pair; one
    // task is never worth a pool spin-up.
    if (threads <= 1 || effective_n == 1) {
        for (std::size_t i = 0; i < effective_n; ++i) {
            mpz_mul(results[i].get_mpz(),
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
            mpz_mul(results[i].get_mpz(),
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
    // parallel_mpz_mod, parallel_mpz_gcd).
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
