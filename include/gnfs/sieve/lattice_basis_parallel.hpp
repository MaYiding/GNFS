#pragma once

// Lattice basis reduction multi-basis parallel dispatch.
//
// Background:
//   GNFS lattice sieving (see `include/gnfs/sieve/lattice_basis.hpp`) reduces
//   the 2-vector basis (b1, b2) of each Special-Q sub-lattice before the sieve
//   region is established. The chosen reduction method (legacy Gauss / 2D LLL /
//   skew-LLL) is a pure function of the incoming basis and produces a reduced
//   pair (b1', b2') plus optional region-setup metadata. Distinct Special-Qs
//   never share state during this reduction, so a batch of K basis inputs is
//   embarrassingly parallel — much like Stage 1 of ECM (`ecm_stage1_parallel
//   .hpp`, W9 T1), Stage 2 of ECM (`ecm_stage2_parallel.hpp`, W8 T1), Hensel
//   lift slot dispatch (`hensel_parallel.hpp`, W7), partial-relation merging
//   (`merger_parallel.hpp`, W10 T4) and the in-flight W11 T3 mpz_powm
//   dispatcher. This helper is the fifth member of that family.
//
//   The actual sieve loop today calls the reduction inline, one Special-Q at
//   a time. This helper provides the API that a future wire-in can use to
//   pre-reduce a batch of Special-Q bases in parallel before the per-region
//   sieve work begins. The helper is opt-in and stand-alone: it does NOT
//   modify `lattice_sieve.cpp` or any existing sieve entry point.
//
//   `GNFS_LATTICE_BASIS_PARALLEL_THREADS = N` (default 1, range
//   [1, hw_concurrency * 2])
//
//   N = 1 keeps the sequential path bit-for-bit (no ThreadPool spawned, zero
//   overhead). N >= 2 spawns up to N pool workers and submits each basis's
//   reduction as an independent task. ENV unset / empty / "0" / negative / non-
//   numeric all collapse to the sequential default (matches the W7 / W8 /
//   W9 / W10 T4 parser semantics for consistent operator UX across the
//   parallel-dispatcher family).
//
// Algorithmic equivalence (strict invariant):
//   * Per-basis reduction is a pure function of the input basis. The caller-
//     supplied `reduce_fn(basis_inputs[i])` must read only its own argument
//     and may capture read-only shared state (skew constant, NumberField,
//     params) by reference; it must not write to any shared mutable state.
//   * GMP `mpz_*` operations are thread-safe when operands are disjoint per
//     call. Each basis task owns its own `Integer` / `Result` buffers
//     constructed inside the lambda, satisfying GMP's per-call disjoint-
//     operands requirement.
//   * The dispatcher returns a `std::vector<Result>` of per-basis outcomes
//     in input order. `Result[i] == reduce_fn(basis_inputs[i])` regardless
//     of `threads`, so callers see bit-for-bit identical output between the
//     sequential and parallel paths.
//
// Non-goals:
//   * We do NOT modify the inline sieve loop or any existing `LatticeBasis`
//     reduction call site. The dispatcher is helper-only future-infra; an
//     opt-in caller wiring it into a Special-Q batch driver is left for a
//     subsequent task.
//   * We do NOT inspect basis contents. `Basis` is a caller-supplied type
//     (typically a small struct carrying (q, root) or the explicit (b1, b2)
//     integer pair); the helper simply forwards each entry to `reduce_fn`.
//   * `Result` must be default-constructible (used to size the output vector)
//     and move- or copy-assignable. Common choices: a reduced-basis struct
//     ({Integer b1, Integer b2}), a sieve-region setup struct, or
//     `std::optional<...>` for "valid reduction / skip" semantics.

#include "../util/thread_pool.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <future>
#include <mutex>
#include <new>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace gnfs::sieve {

namespace detail {

/// Cached env-parsed thread count for lattice-basis-reduction multi-basis
/// dispatch. Reset via
/// `lattice_basis_parallel_threads_reset_env_cache_for_testing()` so unit
/// tests can toggle `GNFS_LATTICE_BASIS_PARALLEL_THREADS` between
/// assertions.
struct LatticeBasisParallelCache {
    std::once_flag once;
    std::size_t value = 1;
};

inline LatticeBasisParallelCache& lattice_basis_parallel_cache() noexcept {
    static LatticeBasisParallelCache cache;
    return cache;
}

/// Parse `GNFS_LATTICE_BASIS_PARALLEL_THREADS`. Returns 1 (sequential) on:
///   - ENV unset / empty / non-numeric / non-positive
/// Otherwise returns the parsed value, clamped to
/// [1, hardware_concurrency * 2] (fallback hw = 4 when
/// hardware_concurrency() reports 0 so the upper cap stays meaningful).
///
/// Parser semantics mirror `parse_filter_merge_threads_env()` and
/// `parse_ecm_stage1_parallel_env()` so callers that wire up multiple
/// dispatcher knobs see consistent ENV behaviour: `std::atoi` accepts a
/// leading numeric prefix (so `"12abc"` parses to 12), empty / unset / "0" /
/// negative all collapse to the sequential default, and any out-of-range
/// value clamps to the cap rather than throwing.
inline std::size_t parse_lattice_basis_parallel_env() noexcept {
    const char* env = std::getenv("GNFS_LATTICE_BASIS_PARALLEL_THREADS");
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
    std::size_t cap = static_cast<std::size_t>(hw) * 2;
    std::size_t v = static_cast<std::size_t>(parsed);
    if (v > cap)
        v = cap;
    return v;
}

} // namespace detail

/// Read the `GNFS_LATTICE_BASIS_PARALLEL_THREADS` env into a cached thread
/// count.
///
/// First call parses the env once (via `std::once_flag`); subsequent calls
/// return the cached value. Range: [1, hardware_concurrency() * 2]. Default
/// (unset / "" / non-numeric / <= 0): 1 (sequential). Out-of-range high
/// values clamp to the upper cap.
[[nodiscard]] inline std::size_t lattice_basis_parallel_threads() noexcept {
    auto& cache = detail::lattice_basis_parallel_cache();
    std::call_once(cache.once,
                   [&cache]() { cache.value = detail::parse_lattice_basis_parallel_env(); });
    return cache.value;
}

/// Reset the cached thread count. Intended for unit tests that toggle
/// `GNFS_LATTICE_BASIS_PARALLEL_THREADS` between assertions.
///
/// Not thread-safe; only call between test cases where no
/// `parallel_lattice_basis_reduce` invocation is in flight.
inline void lattice_basis_parallel_threads_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::lattice_basis_parallel_cache();
    // Reconstruct the once_flag + value in place. We avoid std::atomic
    // because the helper is meant to be called between test cases where
    // single-threaded use is guaranteed.
    cache.~LatticeBasisParallelCache();
    new (&cache) detail::LatticeBasisParallelCache();
}

/// Parallelize per-basis reduction across an independent set of Special-Q
/// lattice basis inputs.
///
/// `basis_inputs` is a span of per-basis input tuples (any caller-supplied
/// `Basis` type — typically a small struct holding `(q, root)` or the
/// explicit `(b1, b2)` integer pair, or a fuller `(q, root, skew, params)`
/// descriptor). `reduce_fn(basis_inputs[i])` performs reduction for one
/// basis and returns its outcome as a value of caller-chosen type `Result`
/// (commonly a reduced-basis struct `{Integer b1, Integer b2}`, a sieve-
/// region setup struct, or `std::optional<...>` for "valid reduction / skip"
/// semantics).
///
/// `reduce_fn` MUST be thread-safe over disjoint basis inputs:
///   * It may read shared read-only state (skew constant, polynomial,
///     params, NumberField, etc.) captured by reference.
///   * It MUST NOT write to any shared mutable state.
///   * GMP / Integer scratch must be allocated inside the lambda (per-call
///     local) or borrowed via a thread-local mechanism such as
///     `IntegerScratchHandle`.
///
/// Returns a `std::vector<Result>` of per-basis outcomes in input order.
/// `Result[i]` is exactly what `reduce_fn(basis_inputs[i])` returned,
/// regardless of `threads` — the sequential (N=1) and parallel (N>=2) paths
/// are bit-for-bit equivalent because `reduce_fn` is a pure function of
/// `basis_inputs[i]`.
///
/// Behavior:
///   - threads <= 1:            sequential for-loop, no ThreadPool created
///   - threads >= 2:            ThreadPool dispatch via `submit()` +
///                              `future.get()`
///   - empty inputs span:       returns empty vector (no pool created)
///   - single basis:            always sequential (no ThreadPool overhead
///                              even when threads >= 2)
///
/// `Result` must be default-constructible (used to pre-size the output) and
/// move- or copy-assignable. Per-slot writes are race-free because each
/// task owns exactly one disjoint output index.
///
/// Exception propagation: any exception thrown by `reduce_fn` propagates to
/// the caller via `future::get()`. The dispatcher does not swallow or wrap
/// exceptions. When multiple workers throw concurrently, only the first
/// exception reached by the synchronous `future.get()` loop is observed;
/// remaining futures are still waited on so the ThreadPool can join
/// cleanly.
template <typename Result, typename Basis, typename ReduceFn>
inline std::vector<Result> parallel_lattice_basis_reduce(std::span<const Basis> basis_inputs,
                                                         ReduceFn&& reduce_fn,
                                                         std::size_t threads) {
    const std::size_t n = basis_inputs.size();
    std::vector<Result> results;
    if (n == 0)
        return results;

    results.resize(n);

    // Sequential path: zero overhead, preserves original behaviour
    // bit-for-bit (no pool spawn, no future overhead). Also exercised when
    // a caller asks for parallelism but only supplied a single basis; one
    // task is never worth a pool spin-up.
    if (threads <= 1 || n == 1) {
        for (std::size_t i = 0; i < n; ++i) {
            results[i] = reduce_fn(basis_inputs[i]);
        }
        return results;
    }

    // Parallel path: bound pool size by min(threads, basis_inputs).
    // Spawning more workers than basis entries wastes resources and adds
    // futex pressure for no throughput gain.
    const std::size_t pool_size = (threads < n) ? threads : n;
    gnfs::util::ThreadPool pool(static_cast<uint32_t>(pool_size));

    std::vector<std::future<void>> futures;
    futures.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        // Each task captures basis index + references to the input span,
        // the output vector, and the user reduce functor. Per-basis output
        // slots are disjoint, so concurrent writes to results[i] are
        // race-free even though `results` itself is shared.
        futures.push_back(pool.submit([&basis_inputs, &results, &reduce_fn, i]() {
            results[i] = reduce_fn(basis_inputs[i]);
        }));
    }

    // Drain every future even when one rethrows: we want the pool to join
    // cleanly in its dtor (workers must finish their current task before
    // returning) and we do not want a thrown exception to abandon other
    // workers' results mid-flight. The first observed exception
    // propagates; any subsequent exceptions are swallowed (matches
    // std::async / typical future-chain semantics).
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

/// Legacy environment-driven wrapper.
///
/// Empty input preserves the historical no-op behavior without parsing or
/// seeding the cached environment value. Non-empty input resolves the cached
/// legacy thread count once and delegates to the explicit overload above.
/// The callable is forwarded exactly once so move-sensitive reducers are not
/// consumed before the selected algorithm path uses them.
template <typename Result, typename Basis, typename ReduceFn>
inline std::vector<Result> parallel_lattice_basis_reduce(std::span<const Basis> basis_inputs,
                                                         ReduceFn&& reduce_fn) {
    if (basis_inputs.empty()) {
        return {};
    }
    const std::size_t threads = lattice_basis_parallel_threads();
    return parallel_lattice_basis_reduce<Result, Basis>(basis_inputs,
                                                        std::forward<ReduceFn>(reduce_fn), threads);
}

} // namespace gnfs::sieve
