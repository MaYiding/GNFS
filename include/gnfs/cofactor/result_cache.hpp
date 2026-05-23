#pragma once

// Cofactor classification LRU result cache (W14 T3).
//
// `classify_cofactor` is a deterministic pure function of
// `(cofactor, smoothness_bound, large_prime_bound)`:
//
//   classify_cofactor(Integer c, B, lpb) -> CofactorClassification
//
// In real GNFS sieve loops the same (cofactor, B, lpb) tuple is queried
// multiple times — adaptive sieve rounds revisit identical small-cofactor
// candidates produced by repeated Special-Q windows; bucket merges flush
// the same residual cofactor through `classify_cofactor` again. The full
// pipeline (trial division -> SQUFOF -> Brent rho -> Pollard rho -> ECM)
// takes microseconds (cheap path) up to milliseconds (ECM Stage 2). An
// LRU cache lets repeat queries return in O(1) hash-lookup time.
//
// What this helper actually delivers:
//   * Standard textbook LRU: `std::list<(Key, Value)>` (front = MRU,
//     back = LRU) + `std::unordered_map<Key, list-iterator>` for O(1)
//     lookup. `splice` on hit promotes to front. `pop_back` on insert
//     when full evicts the LRU entry.
//   * Mutex-protected get/put for thread safety.
//   * Single overflow-free policy: cache is bounded by capacity; any new
//     entry evicts the LRU entry to make room (true LRU semantics, unlike
//     W13 T3 ECM B1 prime cache which is insert-only with overflow slot).
//   * Process-singleton accessor `shared_cofactor_result_cache()`.
//   * ENV-cached capacity reader with documented parse rules.
//
// What this helper does NOT do:
//   * It does NOT modify any cofactor algorithm. Helper-only future-infra.
//   * It does NOT auto-instrument `classify_cofactor`. The current
//     production cofactor pipeline path is unchanged. Callers wishing to
//     skip the pipeline on cache hit must explicitly `get(...)` first and
//     call `classify_cofactor` only on cache miss, then `put(...)` the
//     result.
//   * It does NOT cache failed-but-retried results differently. A
//     deterministic `CofactorClassification` is cached regardless of
//     `CofactorClass` (Smooth / Composite / TooLarge / Unknown). Subsequent
//     lookups with the same key return whichever classification was first
//     observed and persisted.
//
// Key composition (3-tuple):
//   * `cofactor`: uint64 (real `Integer` cofactors that overflow uint64
//     are out of scope — pipeline already routes those to a Composite/
//     TooLarge branch without invoking expensive subfactor probes).
//   * `B`: smoothness_bound (uint32). Caller's sieve-parameter "B" passed
//     to `classify_cofactor` for survival predictor. Two cofactor values
//     under different B may classify differently (TooLarge boundary
//     differs by B).
//   * `lp_bound`: large_prime_bound (uint32). Caller's "LP bound" passed
//     to `classify_cofactor`. Two cofactor values under different lp_bound
//     may classify differently (Prime vs TooLarge boundary).
//
// Why these three uint32/uint64 fields suffice as the cache key:
// `classify_cofactor` reads only these three numeric inputs from its
// argument list. All other behavior (`survival_predictor` thresholds, ECM
// curve-pool sigma generation seed, Brent rho seed) is process-global
// state shared across queries; mutating those between two queries with the
// same `(cofactor, B, lp_bound)` could in principle yield different
// outputs, but in stable steady-state pipeline runs those globals are
// fixed. Cache hits therefore return the deterministic classification.
//
// Bit-for-bit guarantee:
//   * `put(K, V)` followed by `get(K)` returns V's fields exactly:
//     `type`, `factor1`, `factor2`, `factor3`, `power` are stored by
//     value. The cache returns a copy on `get` to avoid lifetime aliasing
//     across concurrent calls; the copy is byte-identical to the put-time
//     argument.
//
// LRU eviction strategy (textbook):
//   * On `get` hit: `splice` the hit node to `list.begin()` (front = MRU).
//   * On `put` of existing key: update value AND `splice` node to front.
//   * On `put` of new key + cache not full: `push_front` + insert iter.
//   * On `put` of new key + cache full: evict `list.back()` (LRU node) —
//     erase its hash-map entry first to keep the map / list in sync —
//     then `push_front` new node.
//
// Thread safety:
//   * All public methods take an internal `std::mutex`. Concurrent get/put
//     from multiple threads serialise on the mutex. The mutex is held
//     across the full operation (including the optional `splice`), so the
//     LRU list and the hash map cannot diverge under concurrent access.
//   * The returned `std::optional<CofactorClassification>` from `get(K)`
//     is a value-copy of the cached entry — safe to use across subsequent
//     cache mutations without holding the lock.
//
// ENV control:
//   * GNFS_COFACTOR_RESULT_CACHE_SIZE=N  → cache enabled with capacity N
//     - N in [1, 1048576]  enables the shared singleton cache
//     - N > 1048576        clamps to 1048576 (hard cap; 1M entries ~
//                          tens of MB depending on value padding)
//     - N == 0 / unset     → cache disabled (default, zero overhead)
//     - Negative / non-numeric / empty / leading whitespace → 0
//     - Partial-parse: "12abc" → 12 (std::stoi accepts numeric prefix).
//       Documented; users should pass clean integer values.
//
// Process-singleton storage strategy:
//   * `shared_cofactor_result_cache()` returns a reference to a
//     function-local static `CofactorResultCache`. This matches the
//     idiom used by `survival_stats()` (W5 T5 survival_predictor.hpp),
//     `cofactor_timing_stats()` (W12 T5 stage_timing.hpp), and
//     `shared_ecm_b1_cache()` (W13 T3 ecm_prime_cache.hpp). Function-local
//     static gives us guaranteed C++11 thread-safe one-time initialization
//     and ODR safety across translation units.

#include "smooth_check.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace gnfs::cofactor {

namespace detail {

/// 3-tuple cache key: (cofactor, smoothness_bound, lp_bound).
///
/// All fields fit in uint64/uint32; total key size 16 bytes (with
/// padding). Comparison and hashing are bit-level over the three fields,
/// so two semantically distinct B / lp_bound values never collide on the
/// same cofactor entry.
struct CofactorCacheKey {
    uint64_t cofactor = 0;
    uint32_t B = 0;
    uint32_t lp = 0;

    [[nodiscard]] bool operator==(const CofactorCacheKey& other) const noexcept {
        return cofactor == other.cofactor && B == other.B && lp == other.lp;
    }
};

/// splitmix64-style hash over the three key fields.
///
/// We use the same Stafford Mix 13 round as W11 T5 lp_key_hash to avoid
/// `std::hash<uint64_t>` near-identity behavior. Inputs are folded into a
/// single uint64 before mixing.
struct CofactorCacheKeyHash {
    [[nodiscard]] std::size_t operator()(const CofactorCacheKey& k) const noexcept {
        // Combine three fields into one uint64. Distribute B and lp into
        // disjoint bit positions so distinct (B, lp) pairs produce
        // distinct combined values before mixing:
        //   cofactor (64 bits, full)
        //   ^ (B << 32) (B occupies top 32 bits)
        //   ^ (lp << 16) (lp occupies bits 16..47)
        // Then splitmix64 mix.
        uint64_t z = k.cofactor
                   ^ (static_cast<uint64_t>(k.B) << 32)
                   ^ (static_cast<uint64_t>(k.lp) << 16);
        z += 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31);
        return static_cast<std::size_t>(z);
    }
};

/// Cached parse result for GNFS_COFACTOR_RESULT_CACHE_SIZE.
struct ResultCacheSizeEnvCache {
    std::once_flag once;
    std::size_t value = 0;  // 0 = disabled (default)
};

inline ResultCacheSizeEnvCache& result_cache_size_env_cache() noexcept {
    static ResultCacheSizeEnvCache cache;
    return cache;
}

/// Parse GNFS_COFACTOR_RESULT_CACHE_SIZE env variable.
///
/// Returns capacity in [0, 1048576]. 0 disables the shared singleton cache.
///   * unset / empty / "0" / negative / leading non-numeric / leading
///     whitespace → 0
///   * N in [1, 1048576]  → N
///   * N > 1048576        → 1048576 (clamped)
///   * Partial-parse: "12abc" → 12 (std::stoi accepts numeric prefix).
///     Documented behavior; users should pass clean values.
[[nodiscard]] inline std::size_t parse_result_cache_size_env() noexcept {
    const char* env = std::getenv("GNFS_COFACTOR_RESULT_CACHE_SIZE");
    if (env == nullptr || env[0] == '\0') {
        return 0;
    }
    // Reject leading whitespace explicitly. std::stoi otherwise silently
    // skips leading whitespace via std::strtol.
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
    constexpr int kMaxCacheCapacity = 1 << 20;  // 1,048,576
    if (parsed > kMaxCacheCapacity) parsed = kMaxCacheCapacity;
    return static_cast<std::size_t>(parsed);
}

}  // namespace detail

/// Thread-safe LRU cache mapping cofactor query key -> CofactorClassification.
///
/// Capacity is fixed at construction. Use `capacity == 0` to construct a
/// disabled cache (all `get` return nullopt, `put` is a no-op). This allows
/// the process-singleton to be constructed even when the user has not
/// enabled the cache via ENV.
class CofactorResultCache {
public:
    using Key = detail::CofactorCacheKey;
    using Value = CofactorClassification;

    explicit CofactorResultCache(std::size_t capacity)
        : capacity_(capacity), entries_(), order_() {
        if (capacity_ > 0) {
            // Reserve buckets to avoid frequent rehashes during the warm-up
            // phase. The unordered_map default starts with very few buckets.
            entries_.reserve(capacity_ + 16);
        }
    }

    /// Lookup `(cofactor, B, lp_bound)`. Returns the cached classification
    /// on hit (and promotes the entry to MRU), or `std::nullopt` on miss.
    ///
    /// Disabled cache (capacity_ == 0) always returns nullopt.
    [[nodiscard]] std::optional<Value> get(
            uint64_t cofactor, uint32_t B, uint32_t lp_bound) {
        if (capacity_ == 0) return std::nullopt;
        std::lock_guard<std::mutex> guard(mutex_);
        const Key key{cofactor, B, lp_bound};
        auto it = entries_.find(key);
        if (it == entries_.end()) {
            return std::nullopt;
        }
        // Hit: splice to front to mark MRU.
        order_.splice(order_.begin(), order_, it->second);
        // Return a value-copy so caller can use result safely after the
        // lock is released, even if subsequent put() evicts this entry.
        return it->second->second;
    }

    /// Insert / update `(cofactor, B, lp_bound) -> result` in the cache.
    ///
    /// On existing key: overwrites value AND promotes to MRU.
    /// On new key + cache not full: insert at front (MRU).
    /// On new key + cache full: evict LRU (`order_.back()`) first, then
    /// insert at front.
    ///
    /// Disabled cache (capacity_ == 0) is a no-op.
    void put(uint64_t cofactor, uint32_t B, uint32_t lp_bound,
             const Value& result) {
        if (capacity_ == 0) return;
        std::lock_guard<std::mutex> guard(mutex_);
        const Key key{cofactor, B, lp_bound};
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            // Existing key: update value and promote.
            it->second->second = result;
            order_.splice(order_.begin(), order_, it->second);
            return;
        }
        // New key. Evict if at capacity.
        if (order_.size() >= capacity_) {
            // Evict LRU (back of list).
            const Key& lru_key = order_.back().first;
            // Erase hash-map entry FIRST (uses reference to lru_key).
            entries_.erase(lru_key);
            order_.pop_back();
        }
        // Insert new entry at front (MRU).
        order_.emplace_front(key, result);
        entries_[key] = order_.begin();
    }

    /// Current number of cached entries.
    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return order_.size();
    }

    /// Configured capacity (immutable post-construction).
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    /// Empty the cache. Thread-safe.
    void clear() noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        entries_.clear();
        order_.clear();
    }

    CofactorResultCache(const CofactorResultCache&) = delete;
    CofactorResultCache& operator=(const CofactorResultCache&) = delete;

private:
    mutable std::mutex mutex_;
    std::size_t capacity_;
    // `order_` is the MRU-to-LRU linked list. We store (Key, Value) pairs
    // so the iterator stored in `entries_` can dereference to both fields.
    // We use std::list because its iterators are stable across insert/
    // erase of other elements — required for the iterator stored in the
    // hash map.
    using ListNode = std::pair<Key, Value>;
    using ListIter = typename std::list<ListNode>::iterator;
    std::unordered_map<Key, ListIter, detail::CofactorCacheKeyHash> entries_;
    std::list<ListNode> order_;
};

/// Read GNFS_COFACTOR_RESULT_CACHE_SIZE and return cached capacity in [0, 1048576].
///
/// Cached via std::call_once. First invocation parses the environment;
/// subsequent invocations return the cached value with no getenv on the
/// hot path.
[[nodiscard]] inline std::size_t cofactor_result_cache_size() noexcept {
    auto& cache = detail::result_cache_size_env_cache();
    std::call_once(cache.once, [&cache]() {
        cache.value = detail::parse_result_cache_size_env();
    });
    return cache.value;
}

/// Convenience predicate: true iff `cofactor_result_cache_size() > 0`.
[[nodiscard]] inline bool cofactor_result_cache_enabled() noexcept {
    return cofactor_result_cache_size() > 0;
}

/// Test-only: re-parse GNFS_COFACTOR_RESULT_CACHE_SIZE.
///
/// NOT thread-safe — call only from single-threaded test setup. The
/// cached once_flag is not reset; this helper directly overwrites the
/// cached value so a subsequent call to `cofactor_result_cache_size()`
/// returns the freshly parsed value without re-invoking the once_flag
/// initializer.
inline void cofactor_result_cache_reset_env_cache_for_testing() noexcept {
    detail::result_cache_size_env_cache().value =
        detail::parse_result_cache_size_env();
}

/// Process-singleton accessor for the shared cofactor result cache.
///
/// Returns a reference to a function-local static `CofactorResultCache`
/// whose capacity is determined at first call from
/// `cofactor_result_cache_size()`. Capacity is fixed for the process
/// lifetime once initialised.
///
/// Note: if the user wants a different capacity, they must set
/// GNFS_COFACTOR_RESULT_CACHE_SIZE BEFORE the first call to
/// `shared_cofactor_result_cache()`. Calling
/// `cofactor_result_cache_reset_env_cache_for_testing()` after first use
/// updates the *parsed* value but does NOT resize the singleton, so the
/// singleton retains its initial capacity. Tests that want to verify
/// different capacities should construct their own `CofactorResultCache`
/// instances locally.
inline CofactorResultCache& shared_cofactor_result_cache() {
    static CofactorResultCache cache(cofactor_result_cache_size());
    return cache;
}

}  // namespace gnfs::cofactor

// std::hash specialization for cache key (used inside detail::
// CofactorCacheKeyHash for compositional cleanliness, but exposed here so
// users wanting unordered_map<CofactorCacheKey, ...> can compose without
// importing the detail functor).
namespace std {
template <>
struct hash<gnfs::cofactor::detail::CofactorCacheKey> {
    [[nodiscard]] std::size_t operator()(
            const gnfs::cofactor::detail::CofactorCacheKey& k) const noexcept {
        return gnfs::cofactor::detail::CofactorCacheKeyHash{}(k);
    }
};
}  // namespace std
