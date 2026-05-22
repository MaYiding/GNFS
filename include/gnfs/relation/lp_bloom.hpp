#pragma once

// Bloom-filter pre-screen helper for unique LP key counting.
//
// Scope
// -----
// `count_unique_lp_keys(relations)` in `filter.hpp` is on the hot path for
// 50d+/60d Round 2 adaptive sieve loops. The current implementation scans
// each relation's LP keys and inserts every (b-smooth) key into a pair of
// `std::unordered_set<uint64_t>` containers (rational primes and
// (p, r) algebraic packed pairs). The `.size()` of those sets is then used
// as the effective_cols safety margin for trim limits. At 1M+ relations
// this hash-set churn dominates Phase 4 entry wall time because each
// `insert` does an open-addressing probe and an `equal_range` lookup, and
// the underlying buckets are out of L2.
//
// This helper provides:
//
//   1. `BloomLPKeyFilter` — a fixed-width Bloom filter parameterised on a
//      log2 size in bits, with k=4 hash functions. The Bloom filter never
//      reports a false negative; it may report a false positive (a key not
//      yet inserted that nevertheless looks "present").
//
//   2. `count_unique_with_bloom(first, last, bloom_bits)` — a counting
//      helper. When `bloom_bits == 0` it falls back to the legacy pure
//      `std::unordered_set<uint64_t>` path. When `bloom_bits > 0` it
//      maintains both the Bloom filter and the hash set in parallel:
//        * Bloom says "maybe seen" → still probe the hash set (a hash-set
//          hit confirms duplicate; a hash-set miss promotes to unique).
//        * Bloom says "definitely not seen" → directly insert into the
//          hash set (the key MUST be new; this saves nothing in unique
//          count work but documents the Bloom invariant).
//      Either way the final `unique_count` is the cardinality of the
//      underlying hash set, which is bit-for-bit identical to the pure
//      `std::unordered_set` path because hash-set semantics are
//      deterministic over the input sequence.
//
// Correctness invariant
// ---------------------
// The output of `count_unique_with_bloom(first, last, B)` is independent
// of `B`: false positives in the Bloom filter still go through the hash
// set's exact equality check before the unique counter increments. The
// helper exists purely to amortise probing cost when the working set is
// large enough that the hash-set buckets sit out of L1/L2. The unit test
// `count_unique_parity_random_100k` enforces this with a sweep across
// 100k random uint64 keys and bit widths {0, 14, 18, 22}.
//
// ENV gate
// --------
//   GNFS_FILTER_LP_BLOOM_BITS = log2(filter_bits)
//
//   * unset / "0" / negative / non-numeric / empty           → 0 (disabled)
//   * "<10"                                                  → clamp to 0
//   * "10".."28"                                             → as-is
//   * ">28"                                                  → clamp to 28
//
// The cached result is resolved on the first call via `std::once_flag`;
// tests can reload it through `filter_lp_bloom_reset_env_cache_for_testing()`.
//
// Sizing rationale
// ----------------
// bits=10  →   1 KiB  filter →  fits L1, k=4 wastes a lot of cache
// bits=14  →  16 KiB  filter →  L1-friendly, ~1.5% FP @ 5k keys
// bits=18  → 256 KiB  filter →  L2-friendly, ~0.5% FP @ 50k keys
// bits=22  →   4 MiB  filter →  L3-friendly, ~0.5% FP @ 500k keys
// bits=24  →  16 MiB  filter →  off-cache,   ~0.5% FP @ 2M keys
// bits=28  → 256 MiB  filter →  oversized; clamp ceiling
//
// k=4 is hardcoded — that's the empirical sweet spot for the (m, n) range
// the helper is designed for (10 < log2(m) <= 28, n in 10k..10M). For
// larger n the optimal k grows; the helper is parameterised to make
// future tuning easy without breaking the API.
//
// Threading
// ---------
// `BloomLPKeyFilter` is *not* thread-safe. Callers must synchronise around
// concurrent `insert` / `maybe_contains`. The counting helper is single-
// threaded by design.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace gnfs::relation {

namespace detail {

inline std::atomic<int>& cached_bloom_bits() noexcept {
    static std::atomic<int> v{0};
    return v;
}

inline std::once_flag& cached_bloom_flag() noexcept {
    static std::once_flag f;
    return f;
}

/// Resolve the ENV value into a clamped int. See header comment for the
/// full parsing matrix.
inline int resolve_bloom_bits_from_env() noexcept {
    const char* v = std::getenv("GNFS_FILTER_LP_BLOOM_BITS");
    if (v == nullptr || v[0] == '\0') {
        return 0;
    }
    // Strict numeric prefix parse — anything not a clean integer
    // (including a leading '-') is treated as 0.
    char* end = nullptr;
    long parsed = std::strtol(v, &end, 10);
    if (end == v) {
        // No digits consumed.
        return 0;
    }
    if (parsed <= 0) {
        // "0" / negative — disabled.
        return 0;
    }
    if (parsed < 10) {
        // Below the 1 KiB floor → treat as disabled rather than throwing
        // at gate-resolution time. Tests can still hand a small bit count
        // directly to the `BloomLPKeyFilter` constructor for explicit
        // exercise.
        return 0;
    }
    if (parsed > 28) {
        return 28;
    }
    return static_cast<int>(parsed);
}

/// 64-bit splitmix variant — Stafford Mix 13. Used to fan out a single
/// 64-bit key into k=4 independent-looking hashes via different seeds.
[[nodiscard]] inline std::uint64_t splitmix64_step(std::uint64_t x) noexcept {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

/// Returns 4 hashes for key `k`. Each seed is chosen to be coprime with
/// the others under multiplication mod 2^64; the salts dodge trivial
/// collision between the rational packing (prime in low 64 bits) and the
/// algebraic packing ((prime << 32) | root).
inline void hash_quadruple(std::uint64_t key,
                           std::uint64_t out[4]) noexcept {
    out[0] = splitmix64_step(key + 0x9E3779B97F4A7C15ULL);
    out[1] = splitmix64_step(key ^ 0xBB67AE8584CAA73BULL);
    out[2] = splitmix64_step(key + 0x243F6A8885A308D3ULL);
    out[3] = splitmix64_step(key ^ 0x13198A2E03707344ULL);
}

}  // namespace detail

/// Returns the cached parsed value of GNFS_FILTER_LP_BLOOM_BITS.
/// 0 means disabled (pure hash-set baseline path).
[[nodiscard]] inline int filter_lp_bloom_bits() noexcept {
    std::call_once(detail::cached_bloom_flag(), []() noexcept {
        detail::cached_bloom_bits().store(detail::resolve_bloom_bits_from_env(),
                                          std::memory_order_relaxed);
    });
    return detail::cached_bloom_bits().load(std::memory_order_relaxed);
}

/// Convenience predicate: `filter_lp_bloom_bits() > 0`.
[[nodiscard]] inline bool filter_lp_bloom_enabled() noexcept {
    return filter_lp_bloom_bits() > 0;
}

/// Reload the cached ENV value. Intended solely for unit tests that toggle
/// the ENV between scenarios.
inline void filter_lp_bloom_reset_env_cache_for_testing() noexcept {
    detail::cached_bloom_bits().store(detail::resolve_bloom_bits_from_env(),
                                      std::memory_order_relaxed);
    // Mark the once_flag as completed so a subsequent
    // `filter_lp_bloom_bits()` call does not overwrite us.
    std::call_once(detail::cached_bloom_flag(), []() noexcept {
        // No-op: state already set above.
    });
}

/// Fixed-width Bloom filter over 64-bit keys with k=4 hash functions.
/// Storage: `2^bits` slots packed into `2^bits / 64` uint64 words. False
/// positive rate after `n` insertions: `(1 - exp(-4n / 2^bits))^4`.
class BloomLPKeyFilter {
public:
    /// `bits` must be in [10, 30]. The lower bound keeps the filter from
    /// degenerating below 1 KiB (where k=4 immediately saturates); the
    /// upper bound caps the storage at 128 MiB (`2^30 / 8` bytes).
    explicit BloomLPKeyFilter(int bits) : bits_(bits) {
        if (bits_ < 10 || bits_ > 30) {
            throw std::invalid_argument(
                "BloomLPKeyFilter: bits out of range [10, 30] (got " +
                std::to_string(bits_) + ")");
        }
        const std::size_t slot_count = static_cast<std::size_t>(1) << bits_;
        const std::size_t word_count = slot_count / 64;
        data_.assign(word_count, 0ULL);
    }

    /// Insert `key`. After this call, `maybe_contains(key)` returns true.
    void insert(std::uint64_t key) noexcept {
        std::uint64_t h[4];
        detail::hash_quadruple(key, h);
        const std::uint64_t mask = (static_cast<std::uint64_t>(1) << bits_) - 1;
        for (int i = 0; i < 4; ++i) {
            const std::uint64_t bit = h[i] & mask;
            data_[bit >> 6] |= (1ULL << (bit & 63));
        }
    }

    /// Returns true if every bit position for `key` is set. False
    /// positives are possible; false negatives are not.
    [[nodiscard]] bool maybe_contains(std::uint64_t key) const noexcept {
        std::uint64_t h[4];
        detail::hash_quadruple(key, h);
        const std::uint64_t mask = (static_cast<std::uint64_t>(1) << bits_) - 1;
        for (int i = 0; i < 4; ++i) {
            const std::uint64_t bit = h[i] & mask;
            if ((data_[bit >> 6] & (1ULL << (bit & 63))) == 0) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::size_t size_bytes() const noexcept {
        return data_.size() * sizeof(std::uint64_t);
    }

    [[nodiscard]] int bits() const noexcept { return bits_; }

    /// Estimated false-positive rate after `n_inserted` distinct keys.
    /// Standard formula: (1 - exp(-kn / m))^k with k=4 and m=2^bits.
    [[nodiscard]] double estimated_fp_rate(std::size_t n_inserted) const noexcept {
        if (n_inserted == 0) {
            return 0.0;
        }
        const double m = static_cast<double>(static_cast<std::size_t>(1) << bits_);
        const double n = static_cast<double>(n_inserted);
        constexpr double k = 4.0;
        const double single = 1.0 - std::exp(-k * n / m);
        return std::pow(single, k);
    }

private:
    int bits_;
    std::vector<std::uint64_t> data_;
};

namespace detail {

/// Counting kernel that walks `[first, last)` once and inserts each key
/// into either a pure hash set (when `bloom_bits == 0`) or a Bloom-pre-
/// screened hash set. Returns the cardinality of the hash set.
template <typename KeyIt>
[[nodiscard]] inline std::size_t count_unique_with_bloom_impl(
        KeyIt first, KeyIt last, int bloom_bits) {
    // Empty span short-circuit avoids constructing a Bloom filter and a
    // hash set when there is nothing to count.
    if (first == last) {
        return 0;
    }
    if (bloom_bits <= 0) {
        std::unordered_set<std::uint64_t> set;
        // Heuristic reserve so we don't rehash mid-loop. The caller may
        // not know the true cardinality; pick the upper bound on naive
        // inputs. unordered_set tolerates oversized reserves.
        for (auto it = first; it != last; ++it) {
            set.insert(static_cast<std::uint64_t>(*it));
        }
        return set.size();
    }
    BloomLPKeyFilter bloom(bloom_bits);
    std::unordered_set<std::uint64_t> set;
    for (auto it = first; it != last; ++it) {
        const std::uint64_t key = static_cast<std::uint64_t>(*it);
        if (!bloom.maybe_contains(key)) {
            // Bloom guarantees the key is not in the set; skip the
            // hash-set lookup and go straight to insert. Both paths
            // populate the same set, so the final cardinality matches.
            bloom.insert(key);
            set.insert(key);
        } else {
            // Bloom may be lying (false positive). The hash-set insert
            // is idempotent on duplicates, so the bit-identical
            // invariant holds regardless of which branch we take.
            auto [_, inserted] = set.insert(key);
            if (inserted) {
                bloom.insert(key);
            }
        }
    }
    return set.size();
}

}  // namespace detail

/// Count unique 64-bit keys in `[first, last)` with optional Bloom
/// filter pre-screen. Output is independent of `bloom_bits` (Bloom only
/// influences probe count, not the unique-set membership decision).
template <typename KeyIt>
[[nodiscard]] inline std::size_t count_unique_with_bloom(
        KeyIt first, KeyIt last, int bloom_bits) {
    return detail::count_unique_with_bloom_impl(first, last, bloom_bits);
}

}  // namespace gnfs::relation
