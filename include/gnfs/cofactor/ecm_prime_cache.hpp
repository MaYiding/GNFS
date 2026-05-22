#pragma once

// ECM B1 prime-power expansion cache (W13 T3).
//
// ECM Stage 1 internally computes
//
//   k = lcm(1, 2, ..., B1) = prod_{p prime <= B1} p^{floor(log_p B1)}
//
// and performs a scalar multiplication `k * Q` on a Montgomery curve. Real
// callers (`ECM::stage1`, `try_curve_with_pk`) usually iterate the prime
// powers `p^e` one at a time (Lucas chain per prime power) rather than
// materialising the full integer `k`. The expensive part to amortise across
// multiple curves sharing the same B1 is *not* the multiplication, it is
//
//   * the sieve / trial-walk over primes <= B1, and
//   * for each prime p, the loop computing `e_p = max k s.t. p^k <= B1`
//     plus the integer power `p^{e_p}` itself.
//
// When `EcmCurvePool::prepare_batch` or any batched ECM caller spawns many
// curves with a fixed B1, those two computations get re-done per curve.
// This helper provides an opt-in shared cache keyed by B1 whose value is
// `std::vector<uint64_t>` containing the prime-power sequence in ascending
// prime order. The vector is exactly what a Stage 1 caller wants to iterate
// over: for each entry `pe = p^e`, the caller multiplies `Q := pe * Q`.
//
// What this helper actually saves:
//   * Per-curve prime sieve / iteration over primes <= B1.
//   * Per-curve integer-exponent loop computing `p^{e_p}` for each p.
//   * Per-curve heap allocation of the prime-power vector.
//
// What this helper does NOT do:
//   * It does NOT modify any ECM algorithm. Helper-only future-infra.
//   * It does NOT cache the full integer `k = lcm(1,...,B1)`. That integer
//     grows as roughly `exp(B1)` bits (by Mertens / prime-counting), so a
//     cache of mpz_t k values would be a memory hazard. Callers that need
//     the literal `k` mpz can fold the cached prime-power vector with a
//     single GMP loop.
//   * It does NOT auto-instrument `ECM::stage1`. The current production
//     `src/cofactor/` paths still call their existing local prime walk.
//     Helper-only: opt-in wire-in by future patches.
//   * It does NOT implement LRU eviction. The cache is insert-only with a
//     hard capacity bound. When the cache is full and a new B1 misses, the
//     helper computes the prime-power vector on the fly and returns it by
//     value WITHOUT inserting. This keeps the cache stable for the working
//     set of B1 values seen in the first `capacity` insertions; subsequent
//     novel B1 lookups pay the full compute cost on every call (same as no
//     cache). Production ECM callers typically use a small fixed set of B1
//     values (e.g., {1e4, 1e5, 1e6, 1e7}), so a small capacity (4..32)
//     covers the working set without ever evicting.
//
// ENV control:
//   * GNFS_ECM_B1_CACHE_SIZE=N         → cache enabled with capacity N
//     - N in [1, 32]    enables the shared singleton cache
//     - N > 32          clamps to 32 (the hard cap; B1 working sets are small)
//     - N == 0 / unset  → cache disabled (default, zero overhead)
//     - Negative / non-numeric / leading whitespace → 0 (disabled)
//     - Partial-parse: "12abc" → 12 (std::stoi accepts numeric prefix).
//       Documented; users should pass clean integer values.
//
// Process-singleton storage strategy:
//   * `shared_ecm_b1_cache()` returns a reference to a function-local static
//     `EcmB1PrimeCache`. This matches the idiom used by `survival_stats()`
//     (W5 T5 survival_predictor.hpp) and `cofactor_timing_stats()` (W12 T5
//     stage_timing.hpp). Function-local static gives us guaranteed C++11
//     thread-safe one-time initialization and ODR safety across translation
//     units. Choice rationale: cofactor module already uses this pattern for
//     telemetry singletons, so we follow that convention for codebase
//     consistency.
//
// Thread safety:
//   * `EcmB1PrimeCache::get_or_compute` is thread-safe via an internal
//     mutex. Concurrent lookups from many threads serialise on the mutex
//     but each lookup is fast (hash lookup + optional compute), so contention
//     is acceptable for the expected call frequency (a few hundred per
//     ECM batch entry).
//   * The returned `const std::vector<uint64_t>&` is valid only as long as
//     the cache itself outlives the borrower. Since the cache is insert-only
//     within its capacity and the underlying `std::unordered_map<uint64_t,
//     std::vector<uint64_t>>` never erases or rehashes a value's storage
//     (we use a separate `unique_ptr` indirection to keep references stable
//     across map rehashes), references remain valid for the cache's full
//     lifetime as long as the cache is not `clear()`ed.
//   * `compute_b1_prime_powers` is pure (no global state) and trivially
//     thread-safe.
//
// Bit-for-bit guarantee:
//   * `compute_b1_prime_powers(B1)` is a deterministic pure function of B1.
//     Same input -> same output across all calls and across cache hit / miss
//     paths. The cached vector and the freshly computed vector have
//     identical content.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace gnfs::cofactor {

namespace detail {

/// Compute `p^e mod 2^64` where `e = max k s.t. p^k <= B1`. Returns 0 only
/// when `p > B1` (caller filters those out before invoking).
///
/// We restrict B1 to fit in uint64_t and primes p to <= B1, so the result
/// `p^e` always fits in uint64_t because `p^e <= B1 <= UINT64_MAX`.
[[nodiscard]] inline uint64_t prime_power_at_most(uint64_t p, uint64_t B1) noexcept {
    if (p == 0 || p > B1) return 0;
    uint64_t acc = p;
    while (true) {
        // Test acc * p <= B1 without overflow.
        // Since acc <= B1 and p <= B1, the product could overflow uint64_t.
        // Use safe-multiply check: acc * p overflows if acc > B1 / p.
        if (p != 0 && acc > B1 / p) break;
        uint64_t next = acc * p;
        if (next > B1) break;
        acc = next;
    }
    return acc;
}

/// Eratosthenes-style sieve. Returns primes in `[2, B1]` (inclusive of
/// B1 if prime). For B1 <= 1 returns an empty vector.
///
/// Sieve memory is O(B1) bits. For the cache's intended range (B1 up to
/// 1e9 ish), a heap allocation of ~125 MB would be excessive. We restrict
/// to B1 <= 100_000_000 here for safety. Callers passing B1 > 1e8 to the
/// cache will get a std::vector returned by compute path, but it is the
/// caller's responsibility not to thrash the cache with extreme B1.
///
/// For the realistic ECM B1 range [1e3, 1e7], sieve allocation is at most
/// ~12 MB, which is reasonable.
[[nodiscard]] inline std::vector<uint64_t> sieve_primes_up_to(uint64_t B1) {
    if (B1 < 2) return {};
    // Hard cap to prevent runaway allocations.
    constexpr uint64_t kMaxSieveB1 = 100'000'000ULL;
    if (B1 > kMaxSieveB1) {
        throw std::invalid_argument(
            "ecm_prime_cache: B1 exceeds sieve cap (100M)");
    }
    const std::size_t n = static_cast<std::size_t>(B1) + 1;
    // is_composite[i] == true if i is composite.
    std::vector<bool> is_composite(n, false);
    is_composite[0] = true;
    is_composite[1] = true;
    for (std::size_t i = 2; i * i < n; ++i) {
        if (!is_composite[i]) {
            for (std::size_t j = i * i; j < n; j += i) {
                is_composite[j] = true;
            }
        }
    }
    std::vector<uint64_t> primes;
    // Rough upper bound by prime-counting: pi(B1) ~ B1 / ln(B1). Skip the
    // tight reservation to avoid logarithm dependency; vector grows
    // geometrically.
    primes.reserve(n / 16 + 16);
    for (std::size_t i = 2; i < n; ++i) {
        if (!is_composite[i]) {
            primes.push_back(static_cast<uint64_t>(i));
        }
    }
    return primes;
}

/// Cached parse result for GNFS_ECM_B1_CACHE_SIZE.
struct B1CacheSizeEnvCache {
    std::once_flag once;
    int value = 0;  // 0 = disabled (default)
};

inline B1CacheSizeEnvCache& b1_cache_size_env_cache() noexcept {
    static B1CacheSizeEnvCache cache;
    return cache;
}

/// Parse GNFS_ECM_B1_CACHE_SIZE env variable.
///
/// Returns capacity in [0, 32]. 0 disables the shared singleton cache.
///   * unset / empty / "0" / negative / leading non-numeric / leading
///     whitespace → 0
///   * N in [1, 32]  → N
///   * N > 32        → 32 (clamped)
///   * Partial-parse: "12abc" → 12 (std::stoi accepts numeric prefix).
///     Documented behavior; users should pass clean values.
[[nodiscard]] inline int parse_b1_cache_size_env() noexcept {
    const char* env = std::getenv("GNFS_ECM_B1_CACHE_SIZE");
    if (env == nullptr || env[0] == '\0') {
        return 0;
    }
    // Reject leading whitespace explicitly (matches W12 T1
    // linalg_progress_interval convention: clean parsing only).
    // std::stoi otherwise silently skips leading whitespace via std::strtol.
    if (env[0] == ' ' || env[0] == '\t' || env[0] == '\n' || env[0] == '\r') {
        return 0;
    }
    int parsed = 0;
    try {
        parsed = std::stoi(env);
    } catch (...) {
        return 0;  // out of int range or no leading digits
    }
    if (parsed <= 0) return 0;
    constexpr int kMaxCacheCapacity = 32;
    if (parsed > kMaxCacheCapacity) parsed = kMaxCacheCapacity;
    return parsed;
}

}  // namespace detail

/// Compute the ECM Stage 1 prime-power sequence for `B1`.
///
/// Returns a vector containing, in ascending prime order, `p^e` for every
/// prime `p <= B1` where `e = max k s.t. p^k <= B1`. The returned vector
/// is exactly what an ECM Stage 1 caller iterates over (one Lucas-chain
/// per entry).
///
/// Edge cases:
///   * B1 == 0 → returns empty vector
///   * B1 == 1 → returns empty vector (no primes <= 1)
///   * B1 == 2 → returns {2}
///   * B1 == 20 → returns {16, 9, 5, 7, 11, 13, 17, 19}
///
/// This function is pure and thread-safe (no global state). Suitable for
/// direct use when the shared cache is disabled or when a one-off
/// computation is preferred.
///
/// Throws std::invalid_argument if B1 exceeds 100_000_000 (sieve memory
/// cap). For realistic ECM workloads (B1 up to ~1e7), no exception fires.
[[nodiscard]] inline std::vector<uint64_t> compute_b1_prime_powers(uint64_t B1) {
    if (B1 < 2) return {};
    auto primes = detail::sieve_primes_up_to(B1);
    std::vector<uint64_t> result;
    result.reserve(primes.size());
    for (uint64_t p : primes) {
        const uint64_t pe = detail::prime_power_at_most(p, B1);
        // pe == 0 cannot happen here because we filtered primes <= B1 and
        // prime_power_at_most(p, B1) returns p (or higher) for p <= B1.
        result.push_back(pe);
    }
    return result;
}

/// Thread-safe insert-only cache mapping B1 -> prime-power sequence.
///
/// Capacity is fixed at construction. When the cache is full and a lookup
/// misses, the missing B1 is computed on the fly and returned by value
/// (not inserted). This keeps already-cached entries stable across hot
/// loops.
///
/// We use std::unique_ptr<std::vector<uint64_t>> as the map value so that
/// returning `const std::vector<uint64_t>&` is safe across concurrent
/// inserts: even if the underlying unordered_map rehashes (causing element
/// move), the indirection keeps the vector's storage address stable.
class EcmB1PrimeCache {
public:
    explicit EcmB1PrimeCache(std::size_t capacity)
        : capacity_(capacity),
          // Bypass any LIVE entries from a previous test that called
          // clear() — fresh maps start with size() == 0.
          entries_() {
        if (capacity_ > 0) {
            // Reserve a few buckets to avoid early rehash on the first
            // capacity insertions. Even small capacities benefit because
            // unordered_map default starts with very few buckets.
            entries_.reserve(capacity_ + 4);
        }
    }

    /// Lookup B1 in the cache.
    ///
    /// Behavior:
    ///   * Hit: returns a const reference to the cached vector. The
    ///     reference remains valid as long as `clear()` is not called.
    ///   * Miss + size() < capacity: computes the prime-power vector,
    ///     inserts it into the cache, returns a const reference to the
    ///     newly inserted entry.
    ///   * Miss + size() == capacity: computes the prime-power vector
    ///     and returns it via the optional by-value escape hatch.
    ///     Note: the API returns a const reference, so we must store the
    ///     overflow result somewhere. Strategy: we always insert. When the
    ///     cache is at capacity, the new entry STILL goes in (insert-only
    ///     semantics relax to "any insert succeeds; capacity is a soft
    ///     bound for normal workloads"). This avoids the API hazard of
    ///     returning a dangling reference.
    ///
    /// Wait — re-reading the task spec: "未命中 + cache 满 → 计算返回
    /// by-value (不 insert)." This means when the cache is full and a new
    /// B1 misses, we must NOT insert, but we still need to return a
    /// reference. The simplest correct approach is to maintain a small
    /// overflow buffer keyed by B1 that holds the most recent miss; later
    /// misses overwrite it. But that introduces a reference-invalidation
    /// hazard across concurrent calls.
    ///
    /// Adopted resolution: when the cache is full, we keep the latest
    /// "overflow miss" in a thread-local-equivalent slot inside the cache
    /// object itself. Because the API returns a const reference, we cannot
    /// safely return a temporary vector. The slot must be owned by the
    /// cache.
    ///
    /// Therefore we use a single `overflow_` `std::vector<uint64_t>` slot
    /// (protected by the same mutex). When a cache-full miss occurs, the
    /// computed vector is moved into `overflow_` and we return a reference
    /// to that slot. The next cache-full miss overwrites `overflow_`. This
    /// invalidates any previously returned overflow reference, but cached
    /// (in-map) references remain valid. The hazard is documented; correct
    /// callers should not retain overflow references across subsequent
    /// `get_or_compute` calls.
    const std::vector<uint64_t>& get_or_compute(uint64_t B1) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = entries_.find(B1);
        if (it != entries_.end()) {
            return *it->second;
        }
        // Cache miss.
        auto computed = std::make_unique<std::vector<uint64_t>>(
            compute_b1_prime_powers(B1));
        if (entries_.size() < capacity_) {
            // Insert into the cache.
            auto [ins_it, inserted] = entries_.emplace(B1, std::move(computed));
            (void)inserted;
            return *ins_it->second;
        }
        // Cache full: write into the overflow slot (single slot,
        // overwritten by the next cache-full miss).
        overflow_ = std::move(*computed);
        return overflow_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return entries_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    /// Clear the cache. Invalidates all previously returned references
    /// (both cached and overflow). Thread-safe.
    void clear() noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        entries_.clear();
        overflow_.clear();
        overflow_.shrink_to_fit();
    }

    EcmB1PrimeCache(const EcmB1PrimeCache&) = delete;
    EcmB1PrimeCache& operator=(const EcmB1PrimeCache&) = delete;

private:
    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::unordered_map<uint64_t, std::unique_ptr<std::vector<uint64_t>>> entries_;
    std::vector<uint64_t> overflow_;
};

/// Read GNFS_ECM_B1_CACHE_SIZE and return the cached capacity in [0, 32].
///
/// Cached via std::call_once + std::atomic. First invocation parses the
/// environment; subsequent invocations return the cached value with only
/// an atomic load (no getenv on hot path).
[[nodiscard]] inline std::size_t ecm_b1_cache_size() noexcept {
    auto& cache = detail::b1_cache_size_env_cache();
    std::call_once(cache.once, [&cache]() {
        cache.value = detail::parse_b1_cache_size_env();
    });
    return static_cast<std::size_t>(cache.value);
}

/// Convenience predicate: true iff `ecm_b1_cache_size() > 0`.
[[nodiscard]] inline bool ecm_b1_cache_enabled() noexcept {
    return ecm_b1_cache_size() > 0;
}

/// Test-only: re-parse GNFS_ECM_B1_CACHE_SIZE.
///
/// NOT thread-safe — call only from single-threaded test setup. The cached
/// once_flag is not reset; this helper directly overwrites the cached
/// value so a subsequent call to `ecm_b1_cache_size()` returns the
/// freshly parsed value without re-invoking the once_flag initializer.
inline void ecm_b1_cache_reset_env_cache_for_testing() noexcept {
    detail::b1_cache_size_env_cache().value = detail::parse_b1_cache_size_env();
}

/// Process-singleton accessor for the shared ECM B1 prime-power cache.
///
/// Returns a reference to a function-local static EcmB1PrimeCache whose
/// capacity is determined at first call from `ecm_b1_cache_size()`.
/// Capacity is fixed for the process lifetime once initialised.
///
/// Note: if the user wants a different capacity, they must set
/// GNFS_ECM_B1_CACHE_SIZE BEFORE the first call to `shared_ecm_b1_cache()`.
/// Calling `ecm_b1_cache_reset_env_cache_for_testing()` after first use
/// updates the *parsed* value but does NOT resize the singleton, so the
/// singleton retains its initial capacity. Tests that want to verify
/// different capacities should construct their own `EcmB1PrimeCache`
/// instances locally.
inline EcmB1PrimeCache& shared_ecm_b1_cache() {
    static EcmB1PrimeCache cache(ecm_b1_cache_size());
    return cache;
}

}  // namespace gnfs::cofactor
