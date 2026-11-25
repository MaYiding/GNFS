#pragma once

// ECM B1 prime-power expansion cache.
//
// For each prime p <= B1, store the largest p^e <= B1, in ascending prime
// order. This is the sequence used to form the Stage 1 multiplier
// lcm(1, ..., B1), without materializing that large integer.
//
// This helper is not integrated into production ECM. The optional shared
// instance reads GNFS_ECM_B1_CACHE_SIZE once: unset/zero disables it,
// positive values are capped at 32. See docs/env-flags/cofactor.md for parsing.
//
// Lookups return shared ownership of immutable vectors. Hits share the cached
// object; misses at capacity return an independent object without insertion.
// Retain the returned handle while reading its vector. Later lookups, clear(),
// and destruction of the cache do not invalidate retained results.
//
// The mutex serializes lookups, computation, and clear(). Capacity bounds
// entries retained by the cache, not results retained by callers. As with
// other C++ objects, the cache must outlive calls to its member functions.
// All paths produce the same deterministic prime-power sequence for a B1.

#include <atomic>
#include <cerrno>
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
    if (p == 0 || p > B1)
        return 0;
    uint64_t acc = p;
    while (true) {
        // Test acc * p <= B1 without overflow.
        // Since acc <= B1 and p <= B1, the product could overflow uint64_t.
        // Use safe-multiply check: acc * p overflows if acc > B1 / p.
        if (p != 0 && acc > B1 / p)
            break;
        uint64_t next = acc * p;
        if (next > B1)
            break;
        acc = next;
    }
    return acc;
}

/// Eratosthenes-style sieve. Returns primes in `[2, B1]` (inclusive of
/// B1 if prime). For B1 <= 1 returns an empty vector.
///
/// The sieve bitmap uses O(B1) bits, in addition to the returned prime
/// vector. Bounds above 100,000,000 throw std::invalid_argument before
/// allocation, including lookups through the cache.
[[nodiscard]] inline std::vector<uint64_t> sieve_primes_up_to(uint64_t B1) {
    if (B1 < 2)
        return {};
    // Hard cap to prevent runaway allocations.
    constexpr uint64_t kMaxSieveB1 = 100'000'000ULL;
    if (B1 > kMaxSieveB1) {
        throw std::invalid_argument("ecm_prime_cache: B1 exceeds sieve cap (100M)");
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
    int value = 0; // 0 = disabled (default)
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
///   * Partial-parse: "12abc" → 12 (strtoull accepts numeric prefix).
///     Documented behavior; users should pass clean values.
[[nodiscard]] inline int parse_b1_cache_size_env() noexcept {
    const char* env = std::getenv("GNFS_ECM_B1_CACHE_SIZE");
    if (env == nullptr || env[0] == '\0') {
        return 0;
    }
    // Reject leading whitespace explicitly (matches W12 T1
    // linalg_progress_interval convention: clean parsing only).
    // strtoull otherwise silently skips leading whitespace.
    if (env[0] == ' ' || env[0] == '\t' || env[0] == '\n' || env[0] == '\r') {
        return 0;
    }
    // Reject a leading minus explicitly: strtoull accepts it and would
    // otherwise turn a negative value into a large unsigned result.
    if (env[0] == '-') {
        return 0;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(env, &end, 10);
    if (end == env) {
        return 0; // no leading digits
    }

    constexpr int kMaxCacheCapacity = 32;
    // A numeric value beyond the conversion type's range is still above our
    // much smaller cache cap, so saturate instead of disabling the cache.
    if (errno == ERANGE || parsed > static_cast<unsigned long long>(kMaxCacheCapacity)) {
        return kMaxCacheCapacity;
    }
    if (parsed == 0) {
        return 0;
    }
    return static_cast<int>(parsed);
}

} // namespace detail

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
    if (B1 < 2)
        return {};
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

/// A bounded cache of immutable prime-power sequences with owned results.
class EcmB1PrimeCache {
public:
    using Result = std::shared_ptr<const std::vector<uint64_t>>;

    explicit EcmB1PrimeCache(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ > 0) {
            entries_.reserve(capacity_);
        }
    }

    /// Keep this handle alive while using the sequence, including on a miss.
    /// Hits share the cached object; full-cache misses do not change the map.
    /// Results remain valid across clear() and destruction of the cache.
    [[nodiscard]] Result get_or_compute(uint64_t B1) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = entries_.find(B1);
        if (it != entries_.end()) {
            return it->second;
        }
        auto computed = std::make_shared<const std::vector<uint64_t>>(compute_b1_prime_powers(B1));
        if (entries_.size() < capacity_) {
            entries_.emplace(B1, computed);
        }
        return computed;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return entries_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    /// Release the cache's ownership; callers can still read retained results.
    void clear() noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        entries_.clear();
    }

    EcmB1PrimeCache(const EcmB1PrimeCache&) = delete;
    EcmB1PrimeCache& operator=(const EcmB1PrimeCache&) = delete;

private:
    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::unordered_map<uint64_t, Result> entries_;
};

/// Read GNFS_ECM_B1_CACHE_SIZE and return the cached capacity in [0, 32].
///
/// std::call_once publishes the parsed value. Subsequent invocations reuse
/// that value without reading the environment again.
[[nodiscard]] inline std::size_t ecm_b1_cache_size() noexcept {
    auto& cache = detail::b1_cache_size_env_cache();
    std::call_once(cache.once, [&cache]() { cache.value = detail::parse_b1_cache_size_env(); });
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

} // namespace gnfs::cofactor
