#pragma once

// Brent-Pollard rho multi-config parallel dispatcher (W15 T3).
//
// Sibling of the ECM Stage 1 / Stage 2 dispatchers
// (`ecm_stage1_parallel.hpp` W9 T1, `ecm_stage2_parallel.hpp` W8 T1) and the
// GMP-primitive batchers (`mpz_powm_parallel.hpp` W11 T3,
// `mpz_invert_parallel.hpp` W12 T3, `mpz_mod_parallel.hpp` W13 T5,
// `mpz_gcd_parallel.hpp` W14 T5). Member 11 of the W7-W14 parallel-dispatcher
// family.
//
// Background:
//   Brent's variant of Pollard rho (`include/gnfs/cofactor/brent_pollard_rho.hpp`)
//   is one stage of the cofactor pipeline that the user toggles on via
//   `GNFS_COFACTOR_BRENT=1`. Each rho run is parametrised by a polynomial
//   constant `c` (the constant in `f(x) = x^2 + c mod n`) and a starting
//   point `x0`. The Floyd / Brent cycle search that follows is independent
//   across distinct `(c, x0)` configurations: distinct rho runs share no
//   mutable state, depend only on the same modulus `n`, and produce
//   deterministic per-config outputs (`std::optional<Integer>` carrying a
//   non-trivial factor when found, `nullopt` when budget exhausted).
//
//   This makes "try K different rho configurations" an embarrassingly
//   parallel batch. The legacy sequential path tries up to three c values
//   inside `BrentPollardRho::split` itself; this dispatcher lets callers
//   batch a larger fan-out (e.g. (c, x0) pairs sampled from a larger seed
//   pool, or a per-cofactor schedule that pre-builds K configurations).
//
//   Relationship with `GNFS_COFACTOR_BRENT`:
//     * `GNFS_COFACTOR_BRENT=1` toggles whether the Brent variant is used at
//       all (vs falling through to legacy Pollard rho / ECM in the dispatch
//       chain inside `cofactorizer.hpp`).
//     * `GNFS_BRENT_POLLARD_RHO_THREADS=N` controls the concurrency of a
//       batched outer dispatcher built on top of `BrentPollardRho::split`.
//     The two env vars are orthogonal: the dispatcher is opt-in
//     infrastructure for callers that already produce a vector of
//     `(c, x0)` pairs and want to fan them out across worker threads.
//
//   `GNFS_BRENT_POLLARD_RHO_THREADS = N` (default 1, range [1, hw * 2])
//
//   N = 1 keeps the sequential path bit-for-bit (no ThreadPool created, zero
//   overhead). N >= 2 spawns up to N pool workers and submits each
//   configuration as an independent task.
//
// Algorithmic equivalence (strict invariant):
//   * Per-configuration rho work is a pure function of `(c, x0)` plus
//     immutable shared state (modulus `n`, iteration budget). Distinct
//     `(c, x0)` pairs share no mutable state.
//   * GMP `mpz_*` operations are thread-safe when operands are disjoint per
//     call. Each task owns its own per-config Integer buffers (the worker
//     lambda's local scratch), so the per-call disjoint-operand contract
//     holds.
//   * The dispatcher returns a `std::vector<Result>` of per-config outcomes
//     aligned with the input spans. The caller chooses `Result` (typically
//     `std::optional<std::pair<Integer, Integer>>` matching
//     `BrentPollardRho::split`'s shape, or `std::optional<Integer>` carrying
//     a single factor).
//
// Two-span input shape:
//   Unlike the Stage 1 / Stage 2 dispatchers (which consume a single span
//   of "curve" descriptors), this helper consumes two parallel spans
//   `cs[]` and `x0s[]` of equal length. This matches the W14 T5 mpz_gcd
//   surface: each per-index input is a logical pair, and the dispatcher
//   enforces `cs.size() == x0s.size()` via `std::invalid_argument`.
//
// Non-goals:
//   * We do NOT modify `BrentPollardRho::split` or any other Brent rho
//     internals. The dispatcher is an opt-in helper that wraps a
//     caller-supplied worker; the worker decides how to invoke the
//     underlying algorithm (which budget, which seed, which c value).
//   * We do NOT change the dispatch order inside `cofactorizer.hpp`. The
//     existing trial / SQUFOF / Brent / Pollard / ECM chain is untouched.
//   * We do NOT touch `GNFS_COFACTOR_BRENT` behaviour. That env continues
//     to gate whether Brent rho is tried at all; this helper only matters
//     once a caller decides to batch up multiple configurations.

#include "../util/thread_pool.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <future>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace gnfs::cofactor {

namespace detail {

/// Cached env-parsed thread count for batched Brent-Pollard rho dispatch.
/// Reset via `brent_pollard_rho_threads_reset_env_cache_for_testing()` for
/// unit tests that toggle `GNFS_BRENT_POLLARD_RHO_THREADS` between
/// assertions.
struct BrentPollardRhoThreadsCache {
    std::once_flag once;
    int value = 1;
};

inline BrentPollardRhoThreadsCache& brent_pollard_rho_threads_cache() noexcept {
    static BrentPollardRhoThreadsCache cache;
    return cache;
}

/// Parse `GNFS_BRENT_POLLARD_RHO_THREADS`. Returns 1 (sequential) on:
///   - ENV unset / empty / non-numeric / non-positive
/// Otherwise returns the parsed value, clamped to
/// [1, hardware_concurrency * 2] (fallback hw = 4 when hardware_concurrency()
/// reports 0 so the upper cap stays meaningful).
///
/// Parser semantics mirror the rest of the parallel-dispatcher family
/// (W9 T1 / W11 T3 / W12 T3 / W13 T5 / W14 T5): `std::atoi` accepts a
/// leading numeric prefix (so `"12abc"` parses to 12 and `"  4"` parses to 4
/// because atoi consumes leading whitespace), empty / unset / "0" / negative
/// all collapse to the sequential default, and any out-of-range value
/// clamps to the cap rather than throwing.
inline int parse_brent_pollard_rho_threads_env() noexcept {
    const char* env = std::getenv("GNFS_BRENT_POLLARD_RHO_THREADS");
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

/// Read the `GNFS_BRENT_POLLARD_RHO_THREADS` env into a cached thread count.
///
/// First call parses the env once (via `std::once_flag`); subsequent calls
/// return the cached value. Range: [1, hardware_concurrency() * 2]. Default
/// (unset / "" / non-numeric / <= 0): 1 (sequential). Out-of-range high
/// values clamp to the upper cap.
[[nodiscard]] inline int brent_pollard_rho_threads() noexcept {
    auto& cache = detail::brent_pollard_rho_threads_cache();
    std::call_once(cache.once, [&cache]() {
        cache.value = detail::parse_brent_pollard_rho_threads_env();
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
[[nodiscard]] inline int
resolve_brent_pollard_rho_threads(std::size_t batch_size) noexcept {
    int threads = brent_pollard_rho_threads();
    if (batch_size == 0) return 0;
    if (threads <= 1) return 1;
    auto b = static_cast<std::size_t>(threads);
    if (b > batch_size) b = batch_size;
    return static_cast<int>(b);
}

/// Reset the cached thread count. Intended for unit tests that toggle
/// `GNFS_BRENT_POLLARD_RHO_THREADS` between assertions.
///
/// Not thread-safe; only call between test cases where no
/// `parallel_brent_pollard_rho` invocation is in flight.
inline void brent_pollard_rho_threads_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::brent_pollard_rho_threads_cache();
    cache.~BrentPollardRhoThreadsCache();
    new (&cache) detail::BrentPollardRhoThreadsCache();
}

/// Parallelize Brent-Pollard rho across an independent set of
/// configurations.
///
/// `cs[i]` and `x0s[i]` together describe configuration `i`: the Brent rho
/// polynomial constant `c` and the starting point `x0`. The caller-supplied
/// `worker_fn(c, x0)` returns the rho outcome for that single configuration
/// as caller-chosen type `Result` (commonly
/// `std::optional<std::pair<Integer, Integer>>` matching the
/// `BrentPollardRho::split` shape, or `std::optional<Integer>` for a single
/// factor).
///
/// `worker_fn` MUST be a pure function of `(c, x0)`: it may read shared
/// read-only state (modulus `n`, per-cofactor metadata) captured by
/// reference, but MUST NOT write to any shared mutable state. GMP `mpz_*`
/// calls in the lambda must operate on per-task `Integer` buffers (the
/// lambda's own local scratch) so the per-call disjoint-operand contract
/// holds.
///
/// Returns a `std::vector<Result>` of per-config outcomes in input order.
/// Callers that want "first success wins" short-circuit semantics can sweep
/// the returned vector linearly (or build a richer worker that records a
/// shared atomic and bails early; the dispatcher itself does not enforce
/// short-circuit because that would couple worker semantics to the
/// dispatch loop and break the bit-for-bit invariant).
///
/// Behaviour:
///   - threads == 1 (default):  sequential for-loop, no ThreadPool created
///   - threads >= 2:            ThreadPool dispatch via `submit()` +
///                              `future.get()`
///   - empty input span:        returns empty vector (no pool created)
///   - single config:           always sequential (no ThreadPool overhead
///                              even when threads >= 2)
///
/// Bit-for-bit guarantee: same `(cs, x0s)` inputs produce the same
/// `std::vector<Result>` content regardless of thread count, because
/// `worker_fn` is required to be a pure function and each result slot is
/// written by exactly one task. Tests in `test_brent_pollard_rho_parallel.cpp`
/// strictly enforce per-index equality across N=1 / N=4 /
/// N=hardware_concurrency.
///
/// Preconditions:
///   - `cs.size() == x0s.size()`. Mismatch -> throws
///     `std::invalid_argument`.
///   - `cs` and `x0s` must not alias each other or any state used by
///     `worker_fn` for writes (caller contract; the dispatcher does not
///     detect aliasing).
///
/// `Result` must be default-constructible and move-assignable. Per-slot
/// writes are race-free because each task owns exactly one disjoint output
/// index.
///
/// Exception propagation: the dispatcher does not swallow or wrap
/// exceptions thrown by `worker_fn`. If any worker throws, the first
/// observed exception rethrows after every other future has been drained so
/// the pool joins cleanly. Subsequent exceptions are swallowed (matches
/// std::async / typical future-chain semantics, consistent with
/// parallel_mpz_gcd, parallel_mpz_mod, parallel_mpz_invert,
/// parallel_mpz_powm, parallel_stage1_curves, parallel_stage2_curves).
template <typename Result, typename WorkerFn>
inline std::vector<Result>
parallel_brent_pollard_rho(std::span<const uint64_t> cs,
                           std::span<const uint64_t> x0s,
                           WorkerFn worker_fn) {
    if (cs.size() != x0s.size()) {
        throw std::invalid_argument(
            "parallel_brent_pollard_rho: cs.size() must equal x0s.size()");
    }

    const std::size_t n = cs.size();
    std::vector<Result> results;
    if (n == 0) return results;

    results.resize(n);

    const int threads = brent_pollard_rho_threads();

    // Sequential path: zero overhead, preserves the bit-for-bit reference
    // behaviour (no pool spawn, no future overhead). Also taken when the
    // caller asked for parallelism but only supplied a single
    // configuration; one task is never worth a pool spin-up.
    if (threads <= 1 || n == 1) {
        for (std::size_t i = 0; i < n; ++i) {
            results[i] = worker_fn(cs[i], x0s[i]);
        }
        return results;
    }

    // Parallel path: bound pool size by min(threads, n). Spawning more
    // workers than configurations wastes resources and adds futex
    // pressure for no throughput gain.
    const std::size_t pool_size =
        (static_cast<std::size_t>(threads) < n) ? static_cast<std::size_t>(threads) : n;
    gnfs::util::ThreadPool pool(static_cast<uint32_t>(pool_size));

    std::vector<std::future<void>> futures;
    futures.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        // Each task captures the per-config index plus references to the
        // input spans and the results vector. Per-config slots in
        // `results` are disjoint, so concurrent writes to `results[i]`
        // are race-free even though `results` itself is shared. Inputs
        // are read-only from disjoint per-index slots, satisfying the
        // GMP per-call disjoint-operand contract inside whichever GMP
        // calls `worker_fn` chooses to make.
        futures.push_back(pool.submit([&cs, &x0s, &results, &worker_fn, i]() {
            results[i] = worker_fn(cs[i], x0s[i]);
        }));
    }

    // Drain every future even when one rethrows: we want the pool to join
    // cleanly in its dtor (workers must finish their current task before
    // returning) and we do not want a thrown exception to abandon other
    // workers' results mid-flight. The first observed exception
    // propagates; any subsequent exceptions are swallowed (matches
    // std::async / typical future-chain semantics, consistent with the
    // rest of the parallel-dispatcher family).
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

    return results;
}

}  // namespace gnfs::cofactor
