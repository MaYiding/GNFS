#pragma once

#include "../core/relation.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gnfs::relation {

/// Identity of one large-prime column in the GF(2) relation matrix.
///
/// Both numeric fields are deliberately kept at their full uint64_t width.
/// In particular, algebraic ideals must not be packed into a single uint64_t:
/// `(p << 32) | low32(r)` loses the high 32 bits of both p and r. Rational and
/// algebraic keys live in separate namespaces even when p and r are both 0.
struct LargePrimeKey {
    uint64_t prime = 0;
    uint64_t root = 0;      // Algebraic root modulo p; always 0 on rational side.
    bool is_algebraic = false;

    [[nodiscard]] constexpr bool operator==(
            const LargePrimeKey& other) const noexcept {
        return prime == other.prime && root == other.root &&
               is_algebraic == other.is_algebraic;
    }

    /// Stable ordering used by canonical per-relation LP incidence lists.
    [[nodiscard]] constexpr bool operator<(
            const LargePrimeKey& other) const noexcept {
        if (prime != other.prime) return prime < other.prime;
        if (root != other.root) return root < other.root;
        return is_algebraic < other.is_algebraic;
    }
};

/// Hash the structural key without narrowing either numeric field.
struct LargePrimeKeyHash {
    [[nodiscard]] size_t operator()(const LargePrimeKey& key) const noexcept {
        uint64_t h = 14695981039346656037ULL;
        h ^= key.prime;
        h *= 1099511628211ULL;
        h ^= key.root;
        h *= 1099511628211ULL;
        h ^= static_cast<uint64_t>(key.is_algebraic);
        h *= 1099511628211ULL;

        if constexpr (sizeof(size_t) >= sizeof(uint64_t)) {
            return static_cast<size_t>(h);
        } else {
            return static_cast<size_t>(h ^ (h >> 32));
        }
    }
};

[[nodiscard]] constexpr LargePrimeKey rational_large_prime_key(
        const core::PrimePower& lp) noexcept {
    return LargePrimeKey{lp.p, 0, false};
}

[[nodiscard]] constexpr LargePrimeKey algebraic_large_prime_key(
        const core::PrimePower& lp) noexcept {
    return LargePrimeKey{lp.p, lp.r, true};
}

namespace detail {

template <typename Toggle>
inline void for_each_raw_large_prime(
        const core::Relation& relation,
        Toggle&& toggle) {
    for (const auto& lp : relation.rational_large_prime) {
        if ((lp.e & 1u) != 0) {
            std::invoke(toggle, rational_large_prime_key(lp));
        }
    }
    for (const auto& lp : relation.algebraic_large_prime) {
        if ((lp.e & 1u) != 0) {
            std::invoke(toggle, algebraic_large_prime_key(lp));
        }
    }
}

}  // namespace detail

/// Visit the canonical LP support of one relation.
///
/// The raw PrimePower vectors are persistence/square-root data and are not
/// mutated. This function instead exposes the effective GF(2) view:
///
/// - each PrimePower contributes `e & 1`, not one bit per vector entry;
/// - repeated keys are XORed;
/// - p and r retain all 64 bits and the rational/algebraic side is part of the
///   key;
/// - the visitor receives sorted, unique keys.
///
/// Consequently, Relation::is_full() and Relation::num_large_primes() retain
/// their raw-storage meaning. Filtering, merging, metrics, and matrix code must
/// use this canonical view when they mean effective LP columns.
template <typename Visitor>
inline void for_each_odd_large_prime_key(
        const core::Relation& relation,
        Visitor&& visitor) {
    const size_t raw_count = relation.rational_large_prime.size() +
                             relation.algebraic_large_prime.size();

    // The common 1LP/2LP/3LP path stays allocation-free.
    if (raw_count <= 8) {
        std::array<LargePrimeKey, 8> keys{};
        std::array<bool, 8> odd{};
        size_t unique_count = 0;

        detail::for_each_raw_large_prime(relation, [&](LargePrimeKey key) {
            size_t i = 0;
            for (; i < unique_count; ++i) {
                if (keys[i] == key) {
                    odd[i] = !odd[i];
                    return;
                }
            }
            keys[unique_count] = key;
            odd[unique_count] = true;
            ++unique_count;
        });

        std::array<LargePrimeKey, 8> canonical{};
        size_t canonical_count = 0;
        for (size_t i = 0; i < unique_count; ++i) {
            if (odd[i]) canonical[canonical_count++] = keys[i];
        }
        std::sort(canonical.begin(), canonical.begin() + canonical_count);
        for (size_t i = 0; i < canonical_count; ++i) {
            std::invoke(visitor, canonical[i]);
        }
        return;
    }

    // Deep merged relations may carry many raw entries. Maintain their exact
    // symmetric difference in a structural-key set, then sort before exposing
    // it so hash iteration order cannot affect callers.
    std::unordered_set<LargePrimeKey, LargePrimeKeyHash> odd_keys;
    odd_keys.reserve(raw_count);
    detail::for_each_raw_large_prime(relation, [&](LargePrimeKey key) {
        auto [it, inserted] = odd_keys.insert(key);
        if (!inserted) odd_keys.erase(it);
    });

    std::vector<LargePrimeKey> canonical;
    canonical.reserve(odd_keys.size());
    canonical.insert(canonical.end(), odd_keys.begin(), odd_keys.end());
    std::sort(canonical.begin(), canonical.end());
    for (const auto& key : canonical) {
        std::invoke(visitor, key);
    }
}

[[nodiscard]] inline std::vector<LargePrimeKey> odd_large_prime_keys(
        const core::Relation& relation) {
    std::vector<LargePrimeKey> keys;
    keys.reserve(relation.rational_large_prime.size() +
                 relation.algebraic_large_prime.size());
    for_each_odd_large_prime_key(relation, [&](const LargePrimeKey& key) {
        keys.push_back(key);
    });
    return keys;
}

[[nodiscard]] inline size_t count_odd_large_prime_keys(
        const core::Relation& relation) {
    size_t count = 0;
    for_each_odd_large_prime_key(relation, [&](const LargePrimeKey&) {
        ++count;
    });
    return count;
}

[[nodiscard]] inline bool odd_large_prime_keys_empty(
        const core::Relation& relation) {
    return count_odd_large_prime_keys(relation) == 0;
}

[[nodiscard]] inline bool has_odd_large_prime_keys(
        const core::Relation& relation) {
    return !odd_large_prime_keys_empty(relation);
}

}  // namespace gnfs::relation
