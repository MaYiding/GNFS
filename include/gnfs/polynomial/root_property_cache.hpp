#pragma once

/// RootPropertyCache — per-(prime, coeffs-mod-p) memo for alpha contribution.
///
/// Motivation:
/// Kleinjung selector Stage 2 evaluates Murphy E for thousands of candidate
/// polynomials. Each evaluation calls `compute_alpha(f)`, which sweeps ~78k
/// small primes and runs Cantor-Zassenhaus root finding per prime. The
/// rotation + translation loop yields polynomials that share `f(x) mod p`
/// for many primes (rotation `f + k*(x-m)` only shifts coefficients by
/// p-multiples for p larger than the rotation step magnitude). Caching the
/// per-prime alpha contribution keyed on the coefficient-mod-p tuple skips
/// the redundant root-finding work.
///
/// Cache key: (uint32_t p, uint64_t coeffs_hash) where coeffs_hash is a
/// stable FNV-1a digest over `[f[0] % p, f[1] % p, ..., f[d] % p, d]`.
///
/// Cache value: precomputed double alpha contribution at that prime,
/// including affine root, double-root bonus, and projective root term.
///
/// Concurrency: single std::mutex guards the map; hit/miss counters are
/// std::atomic so they can be polled without locking. Designed to work
/// concurrently with the W3 parallel compute_alpha sweep
/// (`GNFS_MURPHY_ALPHA_THREADS=N`).
///
/// Eviction policy: FIFO. Insertions append to a deque tracking order;
/// when size exceeds capacity, the oldest entry is evicted. Simpler than
/// LRU and adequate for the Kleinjung working-set access pattern.
///
/// Activation: ENV `GNFS_POLY_ROOT_CACHE_SIZE=N`. Default 0 = disabled.
/// Disabled mode: lookup/insert are no-ops; baseline path is bit-identical
/// to the pre-cache implementation.

#include "int_polynomial.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace gnfs::polynomial {

/// Composite key into the root-property cache.
struct RootCacheKey {
    uint32_t p;
    uint64_t coeffs_hash;

    bool operator==(const RootCacheKey& o) const noexcept {
        return p == o.p && coeffs_hash == o.coeffs_hash;
    }
};

/// Hash functor for RootCacheKey (folds p into the coeffs_hash with a
/// secondary mix; coeffs_hash already has high entropy so a single
/// multiply-rotate-xor suffices).
struct RootCacheKeyHash {
    size_t operator()(const RootCacheKey& k) const noexcept {
        // Mix p with coeffs_hash. Splittable: most cache misses on rotation
        // share coeffs_hash and differ on p; treat p as a secondary axis.
        uint64_t x = k.coeffs_hash ^ (static_cast<uint64_t>(k.p) * 0x9E3779B97F4A7C15ULL);
        x ^= (x >> 33);
        x *= 0xff51afd7ed558ccdULL;
        x ^= (x >> 33);
        return static_cast<size_t>(x);
    }
};

/// Thread-safe bounded cache of per-prime alpha contributions.
///
/// All lookup/insert operations are O(1) amortized under the cache mutex.
/// Hit/miss counters can be read concurrently via atomic loads.
class RootPropertyCache {
public:
    /// Construct a cache with capacity `cap`. cap=0 means disabled (no-op).
    explicit RootPropertyCache(size_t cap)
        : capacity_(cap) {
        if (capacity_ > 0) {
            // Reserve about 25% over capacity to keep load factor low and
            // limit rehashing churn during the steady-state.
            map_.reserve(capacity_ + capacity_ / 4 + 16);
        }
    }

    /// Non-copyable, non-movable: stable identity simplifies sharing.
    RootPropertyCache(const RootPropertyCache&) = delete;
    RootPropertyCache& operator=(const RootPropertyCache&) = delete;
    RootPropertyCache(RootPropertyCache&&) = delete;
    RootPropertyCache& operator=(RootPropertyCache&&) = delete;

    /// Returns true when the cache is active (capacity > 0).
    [[nodiscard]] bool enabled() const noexcept {
        return capacity_ > 0;
    }

    /// Capacity (zero means disabled).
    [[nodiscard]] size_t capacity() const noexcept {
        return capacity_;
    }

    /// Current number of cached entries.
    [[nodiscard]] size_t size() const noexcept {
        if (!enabled()) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        return map_.size();
    }

    /// Cache hit counter (atomic, lock-free read).
    [[nodiscard]] size_t hit_count() const noexcept {
        return hits_.load(std::memory_order_relaxed);
    }

    /// Cache miss counter (atomic, lock-free read).
    [[nodiscard]] size_t miss_count() const noexcept {
        return misses_.load(std::memory_order_relaxed);
    }

    /// Hit rate as a fraction in [0, 1]; zero accesses returns 0.0.
    [[nodiscard]] double hit_rate() const noexcept {
        size_t h = hit_count();
        size_t m = miss_count();
        size_t total = h + m;
        if (total == 0) return 0.0;
        return static_cast<double>(h) / static_cast<double>(total);
    }

    /// Reset statistics counters (does not clear the cache).
    void reset_stats() noexcept {
        hits_.store(0, std::memory_order_relaxed);
        misses_.store(0, std::memory_order_relaxed);
    }

    /// Lookup. Returns the cached alpha contribution on hit, std::nullopt on
    /// miss (and on disabled mode). Both branches update the hit/miss
    /// counters when the cache is enabled.
    [[nodiscard]] std::optional<double> lookup(uint32_t p, uint64_t coeffs_hash) noexcept {
        if (!enabled()) {
            return std::nullopt;
        }
        const RootCacheKey key{p, coeffs_hash};
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            hits_.fetch_add(1, std::memory_order_relaxed);
            return it->second;
        }
        misses_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    /// Insert. Updates the entry on duplicate key (idempotent; value should
    /// be deterministic per key). Evicts FIFO oldest if size would exceed
    /// capacity. No-op when disabled.
    void insert(uint32_t p, uint64_t coeffs_hash, double value) {
        if (!enabled()) {
            return;
        }
        const RootCacheKey key{p, coeffs_hash};
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = map_.find(key);
        if (it != map_.end()) {
            // Idempotent overwrite (same key should produce same value).
            it->second = value;
            return;
        }

        // Insert new entry; FIFO-evict if over capacity. Copy the
        // front key by value before erasing from the map and popping
        // the deque, so even if a future libstdc++ implementation
        // invalidates front() across pop, the lookup key remains valid.
        while (map_.size() >= capacity_ && !insertion_order_.empty()) {
            const RootCacheKey oldest = insertion_order_.front();
            map_.erase(oldest);
            insertion_order_.pop_front();
        }

        map_.emplace(key, value);
        insertion_order_.push_back(key);
    }

    /// Drop all entries and reset stats. Useful for tests.
    void clear() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        map_.clear();
        insertion_order_.clear();
        hits_.store(0, std::memory_order_relaxed);
        misses_.store(0, std::memory_order_relaxed);
    }

    /// Parse ENV `GNFS_POLY_ROOT_CACHE_SIZE`. Returns the requested capacity
    /// or 0 (disabled) when the variable is unset, empty, non-numeric, or
    /// non-positive.
    ///
    /// Cached once per process via std::call_once.
    [[nodiscard]] static size_t env_capacity() noexcept {
        std::call_once(env_init_flag_, [] {
            env_capacity_ = parse_env_capacity();
        });
        return env_capacity_;
    }

    /// Stable FNV-1a hash of `[f[0] mod p, f[1] mod p, ..., f[d] mod p, d]`.
    /// Used as the second component of the cache key. Independent of GMP's
    /// internal limb layout (only consumes uint64_t residues).
    [[nodiscard]] static uint64_t hash_coeffs_mod_p(const IntPolynomial& f, uint32_t p) noexcept {
        // FNV-1a 64-bit constants.
        constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
        constexpr uint64_t FNV_PRIME  = 1099511628211ULL;

        uint64_t h = FNV_OFFSET;
        const uint32_t d = f.degree();
        const auto& coeffs = f.coefficients();

        for (size_t i = 0; i <= d; ++i) {
            // Reuse mpz_fdiv_ui semantics for sign-aware mod.
            uint64_t c = (i < coeffs.size())
                ? static_cast<uint64_t>(mpz_fdiv_ui(coeffs[i].get_mpz(), p))
                : 0ULL;
            // Mix in 8 bytes of the residue.
            for (int byte_idx = 0; byte_idx < 8; ++byte_idx) {
                uint8_t b = static_cast<uint8_t>((c >> (byte_idx * 8)) & 0xFFULL);
                h ^= static_cast<uint64_t>(b);
                h *= FNV_PRIME;
            }
        }

        // Append degree as a tail seed so polynomials with trailing zero
        // coefficients in lower-degree truncations do not collide.
        h ^= static_cast<uint64_t>(d);
        h *= FNV_PRIME;
        return h;
    }

private:
    static size_t parse_env_capacity() noexcept {
        const char* e = std::getenv("GNFS_POLY_ROOT_CACHE_SIZE");
        if (e == nullptr || e[0] == '\0') {
            return 0;
        }
        long long v = std::atoll(e);
        if (v <= 0) {
            return 0;
        }
        // Clamp upper bound to prevent OOM (~ 64 bytes per entry, so
        // 1 << 26 = 67M entries = ~4 GiB; that is more than any realistic
        // Kleinjung sweep would ever benefit from).
        constexpr long long MAX_CAP = (1LL << 26);
        if (v > MAX_CAP) v = MAX_CAP;
        return static_cast<size_t>(v);
    }

    size_t capacity_;
    mutable std::mutex mutex_;
    std::unordered_map<RootCacheKey, double, RootCacheKeyHash> map_;
    std::deque<RootCacheKey> insertion_order_;
    std::atomic<size_t> hits_{0};
    std::atomic<size_t> misses_{0};

    inline static std::once_flag env_init_flag_{};
    inline static size_t env_capacity_ = 0;
};

} // namespace gnfs::polynomial
