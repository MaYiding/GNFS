#pragma once

// GMP `mpz_invert` batched parallel dispatcher (W12 T3).
//
// Sister helper of `mpz_powm_parallel.hpp` (W11 T3). Same parallel-dispatcher
// template — the only differences are (a) the GMP primitive is
// `mpz_invert(result, base, modulus)` (modular inverse) and (b) callers must
// inspect a parallel `std::vector<bool>` to learn whether each slot was
// computed (mpz_invert returns 0 when gcd(base, modulus) != 1).
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
//   N = 1 processes the batch sequentially without creating a ThreadPool.
//   N >= 2 spawns up to N pool workers and submits each base's
//   `mpz_invert` invocation as an independent unit of work.
//
// Failure semantics (real -- not "this never happens"):
//   `mpz_invert(out, base, modulus)` returns non-zero on success and 0 when
//   gcd(base, modulus) != 1, i.e., when no inverse exists in (Z/modulusZ)^*.
//   The dispatcher reports this through the returned
//   `std::vector<bool> success` whose `i`-th entry says whether `results[i]`
//   holds a valid inverse:
//     * success[i] == true   : results[i] is the canonical inverse mod modulus.
//     * success[i] == false  : no inverse exists. results[i] retains its
//                              previous value; callers may inspect the gcd
//                              using the original base and modulus.
//   GMP leaves its output unspecified on failure. The dispatcher computes
//   into a local candidate and moves it into results[i] only on success.
//   Noninvertibility is normal for composite moduli. Tests
//   `tests/test_mpz_invert_parallel.cpp::
//   test_failure_case_gcd_nontrivial` exercise the gcd-collision path with
//   both N=1 sequential and N=4 parallel to ensure both code paths report the
//   failure bit at the same indices.
//
// Algorithmic equivalence (strict invariant):
//   * `mpz_invert` is a deterministic function of `(base, modulus)`. The
//     successful results and success bits match scalar inversion using the
//     original modulus regardless of `threads`. Failed slots retain their
//     prior values rather than publishing GMP's unspecified failure output.
//   * GMP's documented thread-safety covers concurrent calls that touch
//     disjoint operands. Each task owns its candidate and commits only its
//     result slot. Reads share an owned, immutable snapshot of the modulus.
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
//   * We do NOT modify `gnfs::core::mod_inverse` (the existing free-function
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
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace gnfs::util {

namespace detail {

/// Cached env-parsed thread count for batched `mpz_invert` dispatch. Reset
/// via `mpz_invert_batch_threads_reset_env_cache_for_testing()` so unit tests
/// can toggle `GNFS_MPZ_INVERT_BATCH_THREADS` between assertions.
struct MpzInvertBatchThreadsCache {
    std::atomic<int> value{0};
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
        return 1; // default sequential
    }
    int parsed = std::atoi(env);
    if (parsed <= 0) {
        return 1; // invalid / non-positive -> sequential
    }
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    int cap = static_cast<int>(hw) * 2;
    if (parsed > cap)
        parsed = cap;
    return parsed;
}

} // namespace detail

/// Read the `GNFS_MPZ_INVERT_BATCH_THREADS` env into a cached thread count.
///
/// First call parses the env once (via `std::once_flag`); subsequent calls
/// return the cached value. Range: [1, hardware_concurrency() * 2]. Default
/// (unset / "" / non-numeric / <= 0): 1 (sequential). Out-of-range high
/// values clamp to the upper cap.
[[nodiscard]] inline int mpz_invert_batch_threads() noexcept {
    auto& cache = detail::mpz_invert_batch_threads_cache();
    int cached = cache.value.load(std::memory_order_acquire);
    if (cached != 0)
        return cached;

    const int parsed = detail::parse_mpz_invert_batch_threads_env();
    int expected = 0;
    if (cache.value.compare_exchange_strong(expected, parsed, std::memory_order_acq_rel,
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
/// degrade to a single-task sequential run anyway.
[[nodiscard]] inline int resolve_mpz_invert_batch_threads(std::size_t batch_size) noexcept {
    int threads = mpz_invert_batch_threads();
    if (batch_size == 0)
        return 0;
    if (threads <= 1)
        return 1;
    auto b = static_cast<std::size_t>(threads);
    if (b > batch_size)
        b = batch_size;
    return static_cast<int>(b);
}

/// Reset the cached thread count. Intended for unit tests that toggle
/// `GNFS_MPZ_INVERT_BATCH_THREADS` between assertions.
///
/// Not thread-safe; only call between test cases where no
/// `parallel_mpz_invert` invocation is in flight.
inline void mpz_invert_batch_threads_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::mpz_invert_batch_threads_cache();
    cache.value.store(0, std::memory_order_release);
}

/// Compute `results[i] = bases[i]^{-1} mod modulus` for every i and report
/// which slots succeeded.
///
/// Resizes `results` to match `bases.size()` if undersized (each new slot
/// is default-constructed `Integer(0)`). An owned modulus snapshot is taken
/// before resizing or writing outputs. `modulus` may refer to an element
/// of `results`, and `bases` and `results` may be the same vector. Every
/// inverse uses the original modulus even when an output replaces the
/// object to which the caller's modulus reference points.
///
/// Behavior:
///   - threads == 1 (default): sequential for-loop, no ThreadPool created
///   - threads >= 2:           ThreadPool dispatch via `submit()` +
///                             `future.get()`
///   - empty bases vector:     no-op for any modulus (no pool created,
///                             no writes); returns empty success vector
///   - single base:            always sequential (no ThreadPool overhead
///                             even when threads >= 2)
///
/// Return value:
///   `std::vector<bool>` of the same length as `bases`. `success[i] == true`
///   indicates `results[i]` holds a valid inverse; `success[i] == false`
///   indicates `mpz_invert` returned 0 because `gcd(bases[i], modulus) > 1`.
///   Each inverse is computed into a local candidate and moved to its
///   result slot only on success. Failure preserves an existing result slot;
///   a slot added by resizing remains zero. GMP itself does not guarantee
///   that its output is preserved on failure.
///
/// Bit-for-bit guarantee: same `(bases, modulus)` input produces the same
/// `(results, success)` content regardless of thread count, because
/// `mpz_invert` is a deterministic function and each result slot is written
/// by exactly one task. Tests
/// `test_mpz_invert_parallel.cpp::test_n1_vs_n4_parity` and
/// `test_n1_vs_n_hw_parity` strictly enforce per-index equality.
///
/// Domain handling:
///   - A nonempty batch with zero modulus throws `std::domain_error`
///     before resizing or writing `results`.
///   - Nonzero moduli of either sign are supported; successful inverses
///     are canonical residues modulo `|modulus|`.
///
/// Exception propagation: GMP itself does not throw on inversion failure --
/// it reports the failure via its int return value, which is captured into
/// `success[i]`. The dispatcher does not swallow or wrap exceptions thrown
/// by the underlying Integer / ThreadPool machinery (e.g., bad_alloc); if
/// any worker throws, the first observed exception rethrows after every
/// other future has been drained so the pool joins cleanly. A submission
/// failure also joins queued tasks before destroying captured local state.
/// Already committed results are not rolled back on allocation or task errors.
inline std::vector<bool> parallel_mpz_invert(const std::vector<gnfs::core::Integer>& bases,
                                             const gnfs::core::Integer& modulus,
                                             std::vector<gnfs::core::Integer>& results) {
    const std::size_t n = bases.size();
    if (n == 0) {
        return {};
    }
    if (modulus.is_zero()) {
        throw std::domain_error("parallel_mpz_invert requires a nonzero modulus");
    }
    const gnfs::core::Integer modulus_snapshot(modulus);

    // Defensive resize: keep the dispatcher robust when caller passed an
    // under-sized output buffer. This matches the contract used by the
    // SIMD batch helpers (popcount_simd / and_popcnt_simd / xor_words_simd)
    // -- never UB-write past the output rather than abort.
    if (results.size() < n) {
        results.resize(n);
    }

    std::vector<bool> success(n, false);

    const int threads = mpz_invert_batch_threads();

    // Sequential path: no pool spawn or future overhead. Also taken when the
    // caller asked for parallelism but only supplied a single base; one
    // task is never worth a pool spin-up.
    if (threads <= 1 || n == 1) {
        gnfs::core::Integer candidate;
        for (std::size_t i = 0; i < n; ++i) {
            const int rc =
                mpz_invert(candidate.get_mpz(), bases[i].get_mpz(), modulus_snapshot.get_mpz());
            if (rc != 0) {
                results[i] = std::move(candidate);
            }
            success[i] = (rc != 0);
        }
        return success;
    }

    // Parallel path: bound pool size by min(threads, n). Spawning more
    // workers than bases wastes resources and adds futex pressure for no
    // throughput gain.
    const std::size_t pool_size =
        (static_cast<std::size_t>(threads) < n) ? static_cast<std::size_t>(threads) : n;

    // success[i] is `std::vector<bool>` which packs bits and cannot have
    // disjoint per-bit references taken concurrently. We stage successes in
    // a temporary `std::vector<char>` (one byte per slot, naturally disjoint)
    // and copy back after the join. char writes are atomic at the byte level
    // on every supported platform; per-task writes to `success_byte[i]` are
    // disjoint by construction.
    std::vector<char> success_byte(n, 0);

    std::vector<std::future<void>> futures;
    futures.reserve(n);
    // Captured local storage must outlive the pool. If submit() throws,
    // destruction joins queued tasks before releasing their referenced state.
    gnfs::util::ThreadPool pool(static_cast<uint32_t>(pool_size));
    for (std::size_t i = 0; i < n; ++i) {
        // The candidate is task-local, and an in-place base is read before
        // committing to its own result slot. All tasks share the owned modulus.
        futures.push_back(pool.submit([&bases, &modulus_snapshot, &results, &success_byte, i]() {
            gnfs::core::Integer candidate;
            const int rc =
                mpz_invert(candidate.get_mpz(), bases[i].get_mpz(), modulus_snapshot.get_mpz());
            if (rc != 0) {
                results[i] = std::move(candidate);
            }
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

} // namespace gnfs::util
