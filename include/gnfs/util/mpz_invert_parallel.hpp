#pragma once

// GMP `mpz_invert` batched parallel dispatcher (W12 T3).
//
// Sister helper of `mpz_powm_parallel.hpp` (W11 T3). Same parallel-dispatcher
// template — the only differences are (a) the GMP primitive is
// `mpz_invert(result, base, modulus)` (modular inverse) and (b) callers must
// inspect a parallel `std::vector<bool>` to learn whether each slot was
// computed (mpz_invert returns 0 when gcd(base, modulus) != 1, exposing a
// nontrivial factor rather than producing an inverse).
//
// Background:
//   Modular inverse pops up across the GNFS pipeline whenever a batch of
//   independent bases is reduced against a common modulus -- Cantor-Zassenhaus
//   root finding (per-prime modulus reused across coefficient inversions),
//   Schirokauer maps batch normalisation, the ECM Montgomery batch-inversion
//   fallback path when the prefix product collides with the modulus, and the
//   2x2 Bezout step inside `LatticeBasis` reduction all match this shape.
//   Each `mpz_invert` call is a deterministic function of `(base, modulus)`
//   with disjoint operand allocations, satisfying GMP's documented per-call
//   thread-safety contract.
//
//   This helper centralises the env-gated dispatch so any caller that already
//   has a vector of independent bases plus a shared modulus can opt into
//   worker-pool parallelism without rewriting the inversion loop.
//
//   `GNFS_MPZ_INVERT_BATCH_THREADS = N` (default 1, range [1, hw_concurrency * 2])
//
//   N = 1 keeps the sequential path bit-for-bit (no ThreadPool created, zero
//   overhead). N >= 2 spawns up to N pool workers and submits each base's
//   `mpz_invert` invocation as an independent unit of work.
//
// Failure semantics (real -- not "this never happens"):
//   `mpz_invert(out, base, modulus)` returns non-zero on success and 0 when
//   gcd(base, modulus) != 1, i.e., when no inverse exists in (Z/modulusZ)^*.
//   The dispatcher mirrors this directly via the returned
//   `std::vector<bool> success` whose `i`-th entry says whether `results[i]`
//   holds a valid inverse:
//     * success[i] == true   : results[i] is the canonical inverse mod modulus.
//     * success[i] == false  : gcd(bases[i], modulus) was nontrivial; the
//                              caller may extract a factor via
//                              gcd(bases[i], modulus) (this is the standard
//                              ECM "lucky factor" idiom). results[i] is left
//                              untouched (no spurious write) so caller-side
//                              pre-initialised buffers stay consistent.
//   This is a real branch -- 50d+/60d cofactor pipelines hit it whenever a
//   prefix accumulator or Schirokauer base happens to share a factor with the
//   modulus. Tests `tests/test_mpz_invert_parallel.cpp::
//   test_failure_case_gcd_nontrivial` exercise the gcd-collision path with
//   both N=1 sequential and N=4 parallel to ensure both code paths report the
//   failure bit at the same indices.
//
// Algorithmic equivalence (strict invariant):
//   * `mpz_invert` is a deterministic function of `(base, modulus)`. The
//     dispatcher only changes scheduling; per-slot results (and per-slot
//     success bits) must equal `mpz_invert(out, bases[i], modulus)` regardless
//     of `threads`.
//   * GMP's documented thread-safety covers concurrent calls that touch
//     disjoint operands. Per-task writes go into `results[i]` (a per-index
//     disjoint `Integer`), and reads share an immutable reference to
//     `modulus`, so the per-call disjoint-operand contract holds.
//   * The `results` vector aliases caller-owned storage; the dispatcher
//     writes each slot at most once. Both sequential and parallel paths
//     produce bit-for-bit identical `(results[i], success[i])` pairs.
//
// Family membership:
//   Member 6 of the W7-W12 parallel-dispatcher family
//   (W7 `GNFS_SQRT_HENSEL_THREADS`,
//    W8 T1 `GNFS_ECM_STAGE2_PARALLEL`,
//    W9 T1 `GNFS_ECM_STAGE1_PARALLEL_THREADS`,
//    W10 T4 `GNFS_FILTER_MERGE_THREADS`,
//    W11 T3 `GNFS_MPZ_POWM_BATCH_THREADS`,
//    W11 T4 `GNFS_LATTICE_BASIS_PARALLEL_THREADS`,
//    W12 T3 `GNFS_MPZ_INVERT_BATCH_THREADS`). All six share the same env
//   parsing semantics, ThreadPool ownership pattern, and exception-drain
//   contract. They are individually opt-in (default sequential) and remain
//   fully orthogonal -- enabling several at once causes no interference.
//
// Non-goals:
//   * We do NOT modify `gnfs::core::modinv` (the existing free-function
//     wrapper around mpz_invert in `core/integer.hpp`) or any of the existing
//     batch-inversion / lattice-basis hot paths. This helper is opt-in
//     infrastructure; callers wire it in where they already have a contiguous
//     batch of independent bases.
//   * We do NOT accept multiple moduli per call. A future variant can layer
//     on top of this template; the common case (one shared modulus, varying
//     base) is what this dispatcher targets.
//   * We do NOT pre-allocate or pool `Integer` results. Caller owns the
//     output vector and is responsible for pre-sizing it to `bases.size()`
//     (a defensive resize happens inside the dispatcher when needed so a
//     missized caller still gets a well-defined result).

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

/// Cached env-parsed thread count for batched `mpz_invert` dispatch. Reset
/// via `mpz_invert_batch_threads_reset_env_cache_for_testing()` so unit tests
/// can toggle `GNFS_MPZ_INVERT_BATCH_THREADS` between assertions.
struct MpzInvertBatchThreadsCache {
    std::once_flag once;
    int value = 1;
};

inline MpzInvertBatchThreadsCache& mpz_invert_batch_threads_cache() noexcept {
    static MpzInvertBatchThreadsCache cache;
    return cache;
}

/// Parse `GNFS_MPZ_INVERT_BATCH_THREADS`. Returns 1 (sequential) on:
///   - ENV unset / empty / non-numeric / non-positive
/// Otherwise returns the parsed value, clamped to
/// [1, hardware_concurrency * 2] (fallback hw = 4 when hardware_concurrency()
/// reports 0 so the upper cap stays meaningful).
///
/// Parser semantics mirror `parse_mpz_powm_batch_threads_env()` /
/// `parse_filter_merge_threads_env()`: `std::atoi` accepts a leading numeric
/// prefix (so `"12abc"` parses to 12), empty / unset / "0" / negative all
/// collapse to the sequential default, and any out-of-range value clamps to
/// the cap rather than throwing.
inline int parse_mpz_invert_batch_threads_env() noexcept {
    const char* env = std::getenv("GNFS_MPZ_INVERT_BATCH_THREADS");
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

/// Read the `GNFS_MPZ_INVERT_BATCH_THREADS` env into a cached thread count.
///
/// First call parses the env once (via `std::once_flag`); subsequent calls
/// return the cached value. Range: [1, hardware_concurrency() * 2]. Default
/// (unset / "" / non-numeric / <= 0): 1 (sequential). Out-of-range high
/// values clamp to the upper cap.
[[nodiscard]] inline int mpz_invert_batch_threads() noexcept {
    auto& cache = detail::mpz_invert_batch_threads_cache();
    std::call_once(cache.once, [&cache]() {
        cache.value = detail::parse_mpz_invert_batch_threads_env();
    });
    return cache.value;
}

/// Resolve the effective thread count given a specific batch size.
///
/// Returns `min(threads, batch_size)` so callers can decide upfront whether
/// they should bother building per-task scratch state. Useful for callers
/// that want to short-circuit further setup work when the dispatcher will
/// degrade to a single-task sequential run anyway.
[[nodiscard]] inline int resolve_mpz_invert_batch_threads(std::size_t batch_size) noexcept {
    int threads = mpz_invert_batch_threads();
    if (batch_size == 0) return 0;
    if (threads <= 1) return 1;
    auto b = static_cast<std::size_t>(threads);
    if (b > batch_size) b = batch_size;
    return static_cast<int>(b);
}

/// Reset the cached thread count. Intended for unit tests that toggle
/// `GNFS_MPZ_INVERT_BATCH_THREADS` between assertions.
///
/// Not thread-safe; only call between test cases where no
/// `parallel_mpz_invert` invocation is in flight.
inline void mpz_invert_batch_threads_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::mpz_invert_batch_threads_cache();
    cache.~MpzInvertBatchThreadsCache();
    new (&cache) detail::MpzInvertBatchThreadsCache();
}

/// Compute `results[i] = bases[i]^{-1} mod modulus` for every i and report
/// which slots succeeded.
///
/// Resizes `results` to match `bases.size()` if undersized (each new slot
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
///   - empty bases vector:     no-op (no pool created, no writes); returns
///                             empty success vector
///   - single base:            always sequential (no ThreadPool overhead
///                             even when threads >= 2)
///
/// Return value:
///   `std::vector<bool>` of the same length as `bases`. `success[i] == true`
///   indicates `results[i]` holds a valid inverse; `success[i] == false`
///   indicates `mpz_invert` returned 0 because `gcd(bases[i], modulus) > 1`.
///   In the failure case, `results[i]` is left untouched (caller may inspect
///   its pre-call contents or extract a factor via `gcd(bases[i], modulus)`).
///
/// Bit-for-bit guarantee: same `(bases, modulus)` input produces the same
/// `(results, success)` content regardless of thread count, because
/// `mpz_invert` is a deterministic function and each result slot is written
/// by exactly one task. Tests
/// `test_mpz_invert_parallel.cpp::test_n1_vs_n4_parity` and
/// `test_n1_vs_n_hw_parity` strictly enforce per-index equality.
///
/// Preconditions:
///   - `modulus > 0` (caller responsibility; `mpz_invert` requires a
///     positive modulus, mirroring the existing `gnfs::core::modinv`
///     contract).
///
/// Exception propagation: GMP itself does not throw on inversion failure --
/// it reports the failure via its int return value, which is captured into
/// `success[i]`. The dispatcher does not swallow or wrap exceptions thrown
/// by the underlying Integer / ThreadPool machinery (e.g., bad_alloc); if
/// any worker throws, the first observed exception rethrows after every
/// other future has been drained so the pool joins cleanly.
inline std::vector<bool> parallel_mpz_invert(const std::vector<gnfs::core::Integer>& bases,
                                             const gnfs::core::Integer& modulus,
                                             std::vector<gnfs::core::Integer>& results) {
    const std::size_t n = bases.size();
    if (n == 0) {
        return {};
    }

    // Defensive resize: keep the dispatcher robust when caller passed an
    // under-sized output buffer. This matches the contract used by the
    // SIMD batch helpers (popcount_simd / and_popcnt_simd / xor_words_simd)
    // -- never UB-write past the output rather than abort.
    if (results.size() < n) {
        results.resize(n);
    }

    std::vector<bool> success(n, false);

    const int threads = mpz_invert_batch_threads();

    // Sequential path: zero overhead, preserves the bit-for-bit reference
    // behaviour (no pool spawn, no future overhead). Also taken when the
    // caller asked for parallelism but only supplied a single base; one
    // task is never worth a pool spin-up.
    if (threads <= 1 || n == 1) {
        for (std::size_t i = 0; i < n; ++i) {
            int rc = mpz_invert(results[i].get_mpz(),
                                bases[i].get_mpz(),
                                modulus.get_mpz());
            // mpz_invert returns 0 when gcd(base, modulus) > 1 (no inverse).
            // On success it returns non-zero; we surface that as success[i].
            success[i] = (rc != 0);
        }
        return success;
    }

    // Parallel path: bound pool size by min(threads, n). Spawning more
    // workers than bases wastes resources and adds futex pressure for no
    // throughput gain.
    const std::size_t pool_size =
        (static_cast<std::size_t>(threads) < n)
            ? static_cast<std::size_t>(threads)
            : n;
    gnfs::util::ThreadPool pool(static_cast<uint32_t>(pool_size));

    // success[i] is `std::vector<bool>` which packs bits and cannot have
    // disjoint per-bit references taken concurrently. We stage successes in
    // a temporary `std::vector<char>` (one byte per slot, naturally disjoint)
    // and copy back after the join. char writes are atomic at the byte level
    // on every supported platform; per-task writes to `success_byte[i]` are
    // disjoint by construction.
    std::vector<char> success_byte(n, 0);

    std::vector<std::future<void>> futures;
    futures.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        // Each task captures the batch index plus references to the input
        // vector, the shared modulus, and the output vector. Per-base output
        // slots are disjoint, so concurrent writes to `results[i]` are
        // race-free even though `results` itself is shared. `modulus` is
        // read-only and may be referenced from every worker.
        futures.push_back(pool.submit([&bases, &modulus, &results, &success_byte, i]() {
            int rc = mpz_invert(results[i].get_mpz(),
                                bases[i].get_mpz(),
                                modulus.get_mpz());
            success_byte[i] = (rc != 0) ? 1 : 0;
        }));
    }

    // Drain every future even when one rethrows: we want the pool to join
    // cleanly in its dtor (workers must finish their current task before
    // returning) and we do not want a thrown exception to abandon other
    // workers' results mid-flight. The first observed exception propagates;
    // any subsequent exceptions are swallowed (matches std::async / typical
    // future-chain semantics, consistent with parallel_merge_partials and
    // parallel_mpz_powm).
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

    // Copy the byte-packed success flags into the bool vector after the
    // join so the only `std::vector<bool>` writes happen single-threaded.
    for (std::size_t i = 0; i < n; ++i) {
        success[i] = (success_byte[i] != 0);
    }

    if (first_exc) {
        std::rethrow_exception(first_exc);
    }
    return success;
}

}  // namespace gnfs::util
