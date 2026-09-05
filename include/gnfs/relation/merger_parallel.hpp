#pragma once

// Partial relation merger worker-pool parallel dispatch.
//
// Background:
//   PartialRelationMerger (`include/gnfs/relation/filter.hpp`) and the V3
//   CliqueRelationMerger (`include/gnfs/relation/clique_merger.hpp`) both
//   group partial relations by shared large-prime ("LP") keys and then walk
//   each LP-key bucket independently to produce merged full relations.
//   Within one merge round, the work done for each bucket touches only its
//   own partial relations, so distinct buckets are embarrassingly parallel.
//
//   This helper centralises the env-gated dispatch so any merger variant
//   that already exposes a "per-bucket merge function" (typically a small
//   lambda over a per-LP-key partial list) can opt into worker-pool
//   parallelism without rewriting its core algorithm.
//
//   `GNFS_FILTER_MERGE_THREADS = N` (default 1, range [1, hw_concurrency * 2])
//
//   N = 1 keeps the sequential path bit-for-bit (no ThreadPool created, zero
//   overhead). N >= 2 spawns up to N pool workers and submits each bucket's
//   merge task as an independent unit of work.
//
// Algorithmic equivalence (strict invariant):
//   * Per-bucket merge is a pure function of the bucket's contents.
//     `merge_fn(bucket)` must read only the supplied bucket and may capture
//     read-only shared state by reference; it must not write to any shared
//     mutable state.
//   * GMP `mpz_*` operations are thread-safe when operands are disjoint per
//     call. Each bucket task owns its own `Relation` / `Integer` buffers
//     constructed inside the lambda, satisfying GMP's per-call disjoint-
//     operands requirement.
//   * The dispatcher returns a `std::vector<Result>` of per-bucket outcomes
//     in input order. `Result[i] == merge_fn(buckets[i])` regardless of
//     `threads`, so callers see bit-for-bit identical output between the
//     sequential and parallel paths.
//
// Non-goals:
//   * We do NOT modify `PartialRelationMerger::merge_all` or
//     `CliqueRelationMerger::merge_cliques`. The dispatcher is an opt-in
//     helper for callers that have already grouped their input by LP key.
//   * We do NOT inspect bucket contents. `Bucket` is a caller-supplied
//     type (typically `const std::vector<Relation>*` or a small descriptor
//     struct); the helper simply forwards each bucket to `merge_fn`.
//   * `Result` must be default-constructible (used to size the output
//     vector) and move-assignable. Common choices: `std::vector<Relation>`
//     (merged outputs of one bucket), a small aggregate carrying merge
//     stats, or `std::optional<Relation>` for "first eligible pair" pickers.

#include "../util/thread_pool.hpp"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <limits>
#include <mutex>
#include <new>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace gnfs::relation {

namespace detail {

/// Cached env-parsed thread count for partial-merger dispatch. Reset via
/// `filter_merge_threads_reset_env_cache_for_testing()` so unit tests can
/// toggle `GNFS_FILTER_MERGE_THREADS` between assertions.
struct FilterMergeThreadsCache {
    std::once_flag once;
    std::size_t value = 1;
};

inline FilterMergeThreadsCache& filter_merge_threads_cache() noexcept {
    static FilterMergeThreadsCache cache;
    return cache;
}

/// Parse `GNFS_FILTER_MERGE_THREADS`. Returns 1 (sequential) on:
///   - ENV unset / empty / non-numeric / non-positive
/// Otherwise returns the parsed value, clamped to
/// [1, hardware_concurrency * 2] (fallback hw = 4 when hardware_concurrency()
/// reports 0 so the upper cap stays meaningful).
///
/// Parser semantics mirror `parse_ecm_stage1_parallel_env()` /
/// `parse_ecm_stage2_parallel_env()` so callers that wire up multiple
/// dispatcher knobs see consistent ENV behaviour: `std::atoi` accepts a
/// leading numeric prefix (so `"12abc"` parses to 12), empty / unset / "0" /
/// negative all collapse to the sequential default, and any out-of-range
/// value clamps to the cap rather than throwing.
inline std::size_t parse_filter_merge_threads_env() noexcept {
    const char* env = std::getenv("GNFS_FILTER_MERGE_THREADS");
    if (env == nullptr || env[0] == '\0') {
        return 1;  // default sequential
    }

    // Preserve atoi's accepted leading whitespace and numeric-prefix behavior,
    // but avoid signed overflow when a deployment supplies a very large value.
    const char* first = env;
    while (*first != '\0' && std::isspace(static_cast<unsigned char>(*first)) != 0)
        ++first;
    if (*first == '-') {
        return 1;  // invalid / non-positive -> sequential
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(first, &end, 10);
    if (end == first || parsed == 0)
        return 1;  // invalid / non-positive -> sequential

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    const std::size_t hw_size = static_cast<std::size_t>(hw);
    const std::size_t cap =
        hw_size > std::numeric_limits<std::size_t>::max() / 2
            ? std::numeric_limits<std::size_t>::max()
            : hw_size * 2;
    if (errno == ERANGE || parsed > static_cast<unsigned long long>(cap))
        return cap;
    return static_cast<std::size_t>(parsed);
}

}  // namespace detail

/// Read the `GNFS_FILTER_MERGE_THREADS` env into a cached thread count.
///
/// First call parses the env once (via `std::once_flag`); subsequent calls
/// return the cached value. Range: [1, hardware_concurrency() * 2]. Default
/// (unset / "" / non-numeric / <= 0): 1 (sequential). Out-of-range high
/// values clamp to the upper cap.
[[nodiscard]] inline std::size_t filter_merge_threads() noexcept {
    auto& cache = detail::filter_merge_threads_cache();
    std::call_once(cache.once, [&cache]() {
        cache.value = detail::parse_filter_merge_threads_env();
    });
    return cache.value;
}

/// Reset the cached thread count. Intended for unit tests that toggle
/// `GNFS_FILTER_MERGE_THREADS` between assertions.
///
/// Not thread-safe; only call between test cases where no
/// `parallel_merge_partials` invocation is in flight.
inline void filter_merge_threads_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::filter_merge_threads_cache();
    cache.~FilterMergeThreadsCache();
    new (&cache) detail::FilterMergeThreadsCache();
}

/// Parallelize per-bucket merging across an independent set of partial-
/// relation buckets.
///
/// `buckets` is a span of per-LP-key buckets (any caller-supplied `Bucket`
/// type — typically `const std::vector<Relation>*`, a `std::span<const
/// Relation>`, or a small descriptor struct). `merge_fn(const Bucket&)`
/// returns the merge outcome for one bucket as a value of caller-chosen
/// type `Result` (commonly `std::vector<Relation>` for "full relations
/// produced", a stats aggregate, or `std::optional<Relation>` for "first
/// merged pair").
///
/// `merge_fn` MUST be thread-safe over disjoint buckets:
///   * It may read shared read-only state (`NumberField`, params, etc.)
///     captured by reference.
///   * It MUST NOT write to any shared mutable state.
///   * GMP / Integer scratch must be allocated inside the lambda (per-call
///     local) or borrowed via a thread-local mechanism such as
///     `IntegerScratchHandle`.
///
/// Returns a `std::vector<Result>` of per-bucket outcomes in input order.
/// `Result[i]` is exactly what `merge_fn(buckets[i])` returned, regardless
/// of `threads` — the sequential (N=1) and parallel (N>=2) paths are
/// bit-for-bit equivalent because `merge_fn` is a pure function of
/// `buckets[i]`.
///
/// Behavior:
///   - threads == 1 (default):  sequential for-loop, no ThreadPool created
///   - threads >= 2:            ThreadPool dispatch via `submit()` +
///                              `future.get()`
///   - empty buckets span:      returns empty vector (no pool created)
///   - single bucket:           always sequential (no ThreadPool overhead
///                              even when threads >= 2)
///
/// `Result` must be default-constructible (used to pre-size the output) and
/// move- or copy-assignable. Per-slot writes are race-free because each task
/// owns exactly one disjoint output index.
///
/// Exception propagation: any exception thrown by `merge_fn` propagates to
/// the caller via `future::get()`. The dispatcher does not swallow or wrap
/// exceptions. When multiple workers throw concurrently, only the first
/// exception reached by the synchronous `future.get()` loop is observed;
/// remaining futures are still waited on so the ThreadPool can join cleanly.
template <typename Result, typename Bucket, typename MergeFn>
inline std::vector<Result>
parallel_merge_partials(std::span<const Bucket> buckets, MergeFn&& merge_fn) {
    const std::size_t n = buckets.size();
    std::vector<Result> results;
    if (n == 0) return results;

    results.resize(n);

    const std::size_t threads = filter_merge_threads();

    // Sequential path: zero overhead, preserves original behaviour
    // bit-for-bit (no pool spawn, no future overhead). Also exercised when
    // a caller asks for parallelism but only supplied a single bucket; one
    // task is never worth a pool spin-up.
    if (threads <= 1 || n == 1) {
        for (std::size_t i = 0; i < n; ++i) {
            results[i] = merge_fn(buckets[i]);
        }
        return results;
    }

    // Parallel path: bound pool size by min(threads, buckets). Spawning more
    // workers than buckets wastes resources and adds futex pressure for no
    // throughput gain.
    const std::size_t pool_size = (threads < n) ? threads : n;
    gnfs::util::ThreadPool pool(static_cast<uint32_t>(pool_size));

    std::vector<std::future<void>> futures;
    futures.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        // Each task captures bucket index + references to the input span,
        // the output vector, and the user merge functor. Per-bucket output
        // slots are disjoint, so concurrent writes to results[i] are
        // race-free even though `results` itself is shared.
        futures.push_back(pool.submit([&buckets, &results, &merge_fn, i]() {
            results[i] = merge_fn(buckets[i]);
        }));
    }

    // Drain every future even when one rethrows: we want the pool to join
    // cleanly in its dtor (workers must finish their current task before
    // returning) and we do not want a thrown exception to abandon other
    // workers' results mid-flight. The first observed exception propagates;
    // any subsequent exceptions are swallowed (matches std::async / typical
    // future-chain semantics).
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

}  // namespace gnfs::relation
