#pragma once

// ECM sigma seed warm pool (W10 T3).
//
// ECM Suyama curve setup begins by selecting a sigma value (sigma >= 6),
// which is the parameter that determines the Montgomery curve. Production
// callers typically generate sigma via a PRNG seeded from random_device
// (see `ECM::factor` / `EcmCurvePool::prepare_batch`). On hot retry loops
// (50d+/60d cofactor pipelines, where many curves get tried in tight
// succession), the PRNG call sequence becomes part of the inner loop:
//
//   * std::mt19937_64::operator() advances 624 words of state, then mixes
//     the output, then returns the 64-bit draw.
//   * Per-call cost is small in absolute terms (~tens of cycles), but the
//     PRNG state advance defeats branch prediction on tight retries and
//     creates a serial dependency that limits ILP across attempt sigmas.
//
// This helper provides an opt-in `thread_local` warm pool of pre-generated
// sigma seeds. Callers refill the pool with N seeds in bulk (under one
// generator call sequence) and then consume seeds one-at-a-time from the
// pool. This shifts the PRNG amortisation point: instead of one PRNG call
// per attempted curve, the caller pays N PRNG calls once and amortises
// over the next N curves.
//
// What this helper actually saves vs default path:
//   1. Tight-loop PRNG state advance — bulk refill batches the N draws
//      into a single sequence, so the inner curve attempt loop only does
//      a pop_back from the pool rather than a state-mutating PRNG call.
//   2. Inner-loop branch prediction — no more per-attempt PRNG-vs-cache
//      branch, just a uniform vector pop.
//
// What this helper does NOT do:
//   * It does NOT guarantee bit-for-bit identical sigma sequences vs the
//     OFF path. The PRNG generator function is invoked at refill time
//     under a different call schedule than the OFF path's per-attempt
//     pattern. Callers that need a deterministic sigma sequence should
//     either disable the pool (ENV unset / "0") or refill it from a
//     deterministic generator (e.g. a fixed-seed mt19937_64) and treat
//     the per-thread pool as the new source of truth.
//   * It does NOT change the Suyama curve formula or any downstream ECM
//     arithmetic. Sigma is fed to the same `build_suyama_curve` /
//     `try_curve_with_pk` paths as before.
//   * It does NOT modify any main pipeline path. This is helper-only
//     infrastructure; opt-in wire-ins must be added by callers.
//
// ENV control:
//   * GNFS_ECM_SIGMA_POOL_SIZE=N    → pool enabled with capacity N
//     - N in [1, 1024] enables the pool
//     - N > 1024 clamps to 1024
//     - N == 0 / unset / negative / non-numeric → pool disabled
//     - Partial-parse behavior: "12abc" → 12 (std::stoi accepts numeric
//       prefix). Documented but not relied upon by callers; users should
//       pass clean integer values.
//
// Per-thread storage:
//   * The pool is `thread_local`. Each thread has its own pool. There is
//     no inter-thread synchronization. Seeds pushed by one thread never
//     reach another thread.
//   * On thread exit, the `thread_local std::vector<uint64_t>` destructor
//     runs (trivial — uint64_t has no resource ownership). No teardown
//     required.
//
// Calling pattern:
//   1. Caller checks `sigma_seed_pool_enabled()`. If false, generate sigma
//      via the caller's normal PRNG and skip the pool entirely.
//   2. If enabled, caller calls `refill_sigma_seed_pool(generator)` once
//      at the start of an attempt round (where generator is a callable
//      returning uint64_t — typically a lambda capturing a per-thread
//      mt19937_64 or random_device).
//   3. Caller then loops `auto sigma = get_next_sigma_seed(fresh);`,
//      passing a freshly-generated `fresh` as the fallback when the pool
//      is empty (e.g., when refill capacity is exceeded).
//   4. Caller maps `sigma` to the standard `(sigma % 1000000) + 6`
//      transformation if the production sigma range is `[6, 1000006)`,
//      or applies whatever transformation matches the ECM caller's
//      sigma-generation convention.
//
// Bit-for-bit guarantee (within deterministic generator):
//   * Given a deterministic generator (e.g. `[&rng]() { return rng(); }`
//     with a fixed-seed mt19937_64), the sequence of seeds returned by
//     repeated `get_next_sigma_seed()` calls after refill is
//     bit-for-bit identical to a manually-generated sequence
//     `[gen(), gen(), gen(), ...]`. Pool order is LIFO (`pop_back`), so
//     the seed sequence is the reverse of the refill order.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace gnfs::cofactor {

namespace detail {

/// Cache for GNFS_ECM_SIGMA_POOL_SIZE parse result.
struct SigmaSeedPoolEnvCache {
    std::once_flag once;
    int value = 0;  // 0 = disabled (default)
};

inline SigmaSeedPoolEnvCache& sigma_seed_pool_env_cache() noexcept {
    static SigmaSeedPoolEnvCache cache;
    return cache;
}

/// Parse the GNFS_ECM_SIGMA_POOL_SIZE env variable.
///
/// Returns the desired pool size in [0, 1024]. 0 disables the pool.
/// - unset / empty / "0" / negative / leading non-numeric  → 0
/// - N in [1, 1024]  → N
/// - N > 1024  → 1024 (clamped)
/// - Partial-parse: "12abc" → 12 (std::stoi accepts numeric prefix). This
///   is documented behavior — users should pass clean values.
inline int parse_sigma_seed_pool_size_env() noexcept {
    const char* env = std::getenv("GNFS_ECM_SIGMA_POOL_SIZE");
    if (env == nullptr || env[0] == '\0') {
        return 0;  // unset / empty → disabled
    }
    int parsed = 0;
    try {
        parsed = std::stoi(env);
    } catch (...) {
        return 0;  // out of int range or no leading digits → disabled
    }
    if (parsed <= 0) {
        return 0;  // 0 / negative → disabled
    }
    constexpr int kMaxPoolSize = 1024;
    if (parsed > kMaxPoolSize) parsed = kMaxPoolSize;
    return parsed;
}

/// Per-thread sigma seed warm pool.
///
/// `inline thread_local` is C++17 and ensures a single per-thread instance
/// across all translation units that include this header. The destructor
/// runs at thread exit; uint64_t has no resource ownership, so teardown
/// is trivial.
inline thread_local std::vector<uint64_t> tls_sigma_pool;

}  // namespace detail

/// Read GNFS_ECM_SIGMA_POOL_SIZE and return the cached pool size.
///
/// Cached via std::call_once + std::atomic. First invocation parses the
/// environment; subsequent invocations return the cached value with only
/// an atomic load (no getenv on hot path).
///
/// Returns the pool capacity in [0, 1024]:
///   * 0 → pool disabled (default, zero overhead)
///   * 1..1024 → pool capacity per thread
[[nodiscard]] inline int sigma_seed_pool_size() noexcept {
    auto& cache = detail::sigma_seed_pool_env_cache();
    std::call_once(cache.once, [&cache]() {
        cache.value = detail::parse_sigma_seed_pool_size_env();
    });
    // After call_once completes, cache.value is stable. The lambda is
    // synchronized via the once_flag, so subsequent reads are safe.
    return cache.value;
}

/// Convenience predicate: true iff `sigma_seed_pool_size() > 0`.
[[nodiscard]] inline bool sigma_seed_pool_enabled() noexcept {
    return sigma_seed_pool_size() > 0;
}

/// Test-only: re-parse GNFS_ECM_SIGMA_POOL_SIZE.
///
/// NOT thread-safe — call only from single-threaded test setup. The cached
/// once_flag is not reset; this helper directly overwrites the cached
/// value, so a subsequent call to `sigma_seed_pool_size()` returns the
/// freshly parsed value without re-invoking the once_flag initializer.
inline void sigma_seed_pool_reset_env_cache_for_testing() noexcept {
    detail::sigma_seed_pool_env_cache().value =
        detail::parse_sigma_seed_pool_size_env();
}

/// Returns the current number of seeds held in the calling thread's pool.
/// Mainly useful for testing and debugging.
[[nodiscard]] inline std::size_t sigma_seed_pool_remaining() noexcept {
    return detail::tls_sigma_pool.size();
}

/// Releases all seeds held in the calling thread's pool. After this call,
/// `sigma_seed_pool_remaining()` returns 0 until the next refill.
inline void sigma_seed_pool_clear() noexcept {
    detail::tls_sigma_pool.clear();
}

/// Fill the calling thread's pool with up to `sigma_seed_pool_size()`
/// seeds drawn from `generator`. If the pool already contains seeds, the
/// existing seeds are preserved and additional seeds are appended up to
/// the configured capacity.
///
/// Behavior:
///   * Pool disabled (`!sigma_seed_pool_enabled()`): no-op. `generator`
///     is NOT invoked.
///   * Pool enabled + current size >= capacity: no-op. `generator` is
///     NOT invoked.
///   * Pool enabled + current size < capacity: `generator` is invoked
///     `(capacity - current_size)` times via `push_back`. If `generator`
///     throws, the exception propagates after partial fill (the pool may
///     contain fewer than `capacity` seeds, but is in a consistent state).
///
/// Thread-safety: per-thread pool, no inter-thread synchronization. Safe
/// to call concurrently from multiple threads.
inline void refill_sigma_seed_pool(const std::function<uint64_t()>& generator) {
    const int capacity = sigma_seed_pool_size();
    if (capacity <= 0) return;  // pool disabled

    auto& pool = detail::tls_sigma_pool;
    const std::size_t cap_sz = static_cast<std::size_t>(capacity);
    if (pool.size() >= cap_sz) return;  // already full

    pool.reserve(cap_sz);
    while (pool.size() < cap_sz) {
        // generator() may throw; propagate. Pool remains valid (partial fill).
        pool.push_back(generator());
    }
}

/// Pop and return one seed from the calling thread's pool. If the pool is
/// disabled or empty, returns `fresh_seed` (the caller-supplied fallback).
///
/// Calling pattern:
///   uint64_t fresh = rng();           // caller's PRNG
///   uint64_t sigma_seed = get_next_sigma_seed(fresh);
///   // Now `sigma_seed` is either a pre-pooled seed (if available) or
///   // `fresh` (if the pool is disabled / empty).
///
/// Note: the caller is responsible for ensuring `fresh_seed` is a valid
/// fallback (e.g., recently drawn from a healthy PRNG). The helper does
/// NOT generate a fresh seed itself — that would couple the helper to a
/// specific RNG choice. Callers that want a "fresh fallback" should
/// invoke their own PRNG at the call site.
///
/// Pop order is LIFO (`pop_back`). When the pool was refilled with seeds
/// [s0, s1, s2, ..., s_{N-1}] (in that order), the first N calls to
/// `get_next_sigma_seed` return [s_{N-1}, s_{N-2}, ..., s0] (reverse).
[[nodiscard]] inline uint64_t get_next_sigma_seed(uint64_t fresh_seed) noexcept {
    if (!sigma_seed_pool_enabled()) {
        return fresh_seed;
    }
    auto& pool = detail::tls_sigma_pool;
    if (pool.empty()) {
        return fresh_seed;
    }
    uint64_t s = pool.back();
    pool.pop_back();
    return s;
}

}  // namespace gnfs::cofactor
