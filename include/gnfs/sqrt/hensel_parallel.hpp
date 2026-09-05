#pragma once

// Nguyen Hybrid algebraic sqrt — Hensel lift across prime slots parallelization.
//
// The Nguyen Hybrid algebraic sqrt phase computes sqrt(P · f'(α)²) mod a
// product of K small inert primes via independent Hensel lifts (one per prime),
// then CRT-combines the lifts and resolves signs.  The K lifts are
// embarrassingly parallel: each consumes the same ab_pairs/number-field inputs
// but writes into a distinct LiftResult slot, and the inputs are read-only.
//
// This helper centralizes the env-gated dispatch:
//
//   GNFS_SQRT_HENSEL_THREADS = N (default 1, range [1, hardware_concurrency * 2])
//
// N = 1 keeps the sequential path bit-for-bit (no ThreadPool created, zero
// overhead).  N >= 2 spawns N pool workers and submits each slot's lift as an
// independent task.  Slot state must be per-slot (LiftResult written by index);
// the lift body must not touch shared mutable state.  GMP mpz operations are
// thread-safe when the operands are disjoint per call, which is satisfied here
// because each slot owns its LiftResult/coefficients buffers.
//
// Bit-for-bit guarantee: when each slot's lift_one() is a pure function of the
// slot index plus shared read-only inputs, the K LiftResults produced are
// identical between sequential and parallel paths.  Downstream CRT combination
// only depends on per-slot results, so the final sqrt is identical.

#include "../util/thread_pool.hpp"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace gnfs::sqrt {

namespace detail {

// Cached env-parsed thread count.  Reset via reset_sqrt_hensel_threads_cache()
// for unit testing under different env values.
struct SqrtHenselThreadsCache {
    std::once_flag once;
    std::size_t value = 1;
};

inline SqrtHenselThreadsCache& sqrt_hensel_threads_cache() noexcept {
    static SqrtHenselThreadsCache cache;
    return cache;
}

inline std::size_t sqrt_hensel_thread_cap() noexcept {
    const unsigned int hw_count = std::thread::hardware_concurrency();
    const uint64_t hw = hw_count == 0 ? 4ULL : static_cast<uint64_t>(hw_count);
    constexpr uint64_t max_pool_size = std::numeric_limits<uint32_t>::max();
    const uint64_t cap = hw > max_pool_size / 2 ? max_pool_size : hw * 2;
    return static_cast<std::size_t>(cap);
}

inline std::size_t parse_sqrt_hensel_threads_env() noexcept {
    const char* env = std::getenv("GNFS_SQRT_HENSEL_THREADS");
    if (env == nullptr || env[0] == '\0') {
        return 1; // default sequential
    }

    // Match atoi's accepted decimal-prefix syntax without converting through
    // int first.  The latter can wrap for a positive value above INT_MAX and
    // incorrectly select the sequential path instead of the high-value cap.
    const char* first = env;
    while (*first != '\0' && std::isspace(static_cast<unsigned char>(*first))) {
        ++first;
    }
    if (*first == '-') {
        return 1; // invalid / non-positive → sequential
    }
    if (*first == '+') {
        ++first;
    }
    if (*first < '0' || *first > '9') {
        return 1;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(first, &end, 10);
    if (end == first || parsed == 0) {
        return 1; // invalid / non-positive → sequential
    }

    const std::size_t cap = sqrt_hensel_thread_cap();
    if (errno == ERANGE || parsed > static_cast<unsigned long long>(cap)) {
        return cap;
    }
    return static_cast<std::size_t>(parsed);
}

} // namespace detail

/// Read the GNFS_SQRT_HENSEL_THREADS env into a cached thread count.
///
/// First call parses the env once (via std::once_flag); subsequent calls
/// return the cached value.  Range: [1, hardware_concurrency() * 2].  Default
/// (unset / "" / non-numeric / <= 0): 1 (sequential).  Out-of-range high
/// values clamp to the upper cap.
[[nodiscard]] inline std::size_t sqrt_hensel_threads() noexcept {
    auto& cache = detail::sqrt_hensel_threads_cache();
    std::call_once(cache.once,
                   [&cache]() { cache.value = detail::parse_sqrt_hensel_threads_env(); });
    return cache.value;
}

/// Reset the cached thread count.  Intended for unit tests that toggle
/// GNFS_SQRT_HENSEL_THREADS between assertions.
///
/// Not thread-safe; only call when no parallel_hensel_lift is in flight.
inline void reset_sqrt_hensel_threads_cache() noexcept {
    auto& cache = detail::sqrt_hensel_threads_cache();
    // Reconstruct the once_flag + value in place.  We avoid std::atomic
    // because the helper is meant to be called between test cases where
    // single-threaded use is guaranteed.
    cache.~SqrtHenselThreadsCache();
    new (&cache) detail::SqrtHenselThreadsCache();
}

/// Parallelize the K Hensel lifts of the Nguyen Hybrid algebraic sqrt phase.
///
/// `slots` is a span of per-slot state (typically LiftResult written by
/// index).  `lift_one(slot, index)` performs the lift for one slot in-place.
/// `lift_one` MUST be a pure function over per-slot state: it may read shared
/// read-only inputs (ab_pairs, NumberField, etc.) captured by reference, but
/// must not write to any shared mutable state.
///
/// Behavior:
///   - threads == 1 (default):  sequential for-loop, no ThreadPool created
///   - threads >= 2:            ThreadPool dispatch via submit() + future.get()
///   - empty slots span:        no-op (no pool created)
///   - single slot:             always sequential (no ThreadPool overhead even
///                              when threads >= 2)
template <typename Slot, typename Func>
inline void parallel_hensel_lift(std::span<Slot> slots, Func&& lift_one) {
    const std::size_t n = slots.size();
    if (n == 0)
        return;

    const std::size_t threads = sqrt_hensel_threads();

    // Sequential path: zero overhead, preserves original behavior bit-for-bit.
    if (threads <= 1 || n == 1) {
        for (std::size_t i = 0; i < n; ++i) {
            lift_one(slots[i], i);
        }
        return;
    }

    // Parallel path: bound pool size by min(threads, slots).  Spawning more
    // workers than slots wastes resources.
    const std::size_t pool_size = (threads < n) ? threads : n;
    gnfs::util::ThreadPool pool(static_cast<uint32_t>(pool_size));

    std::vector<std::future<void>> futures;
    futures.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        // Each task captures slot index + reference to the slots span.
        // The slot pointer is stable because std::span aliases the caller's
        // contiguous buffer; the caller guarantees slots[] is not resized
        // during the parallel section.
        futures.push_back(pool.submit([&slots, &lift_one, i]() { lift_one(slots[i], i); }));
    }

    // Propagate any task exception to the caller via future::get().
    for (auto& f : futures) {
        f.get();
    }
}

} // namespace gnfs::sqrt
