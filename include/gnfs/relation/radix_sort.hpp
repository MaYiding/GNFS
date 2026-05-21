#pragma once

// LSD byte-radix sort over Relation (a, b) keys for Phase 0 dedup-sort.
//
// Scope
// -----
// Relation Phase 0 sorts the freshly collected relation list by (b, a)
// lexicographic order before `filter_duplicates` removes identical (a, b)
// pairs. The legacy implementation in `collector.hpp::sort_relations` uses
// `std::sort` with a 2-key comparator: O(n log n) with poor cache locality
// because the comparator touches the Relation struct (≥ 500 bytes) for
// every probe. At 1M+ relations on 50d+ runs the sort visibly dominates
// the Phase 0 wall time.
//
// This header implements a stable least-significant-digit (LSD) byte
// radix sort that scans the relations linearly, builds a compact 16-byte
// `(b, a)` key per relation, then sorts the index permutation in two
// stages (8 byte-passes over `a` followed by 8 byte-passes over `b`).
// Because LSD radix is stable, sorting on the lower sub-key first and
// the higher sub-key second yields a final order with `b` as the primary
// and `a` as the secondary key — bit-for-bit identical to the std::sort
// path. The relation array is then permuted exactly once (move-only) at
// the end so the underlying Relation data is touched only twice in total
// (once for key build, once for the final permute).
//
// Correctness
// -----------
// LSD radix sort is exact for fixed-width integer keys. The signed `a`
// field is biased by 0x8000000000000000 before radixing so two's-complement
// negatives sort before non-negatives. Stability across the 8 + 8 byte
// passes means duplicate `(a, b)` pairs preserve their original insertion
// order, which is required for the subsequent `filter_duplicates` pass to
// pick the same representative as the std::sort path.
//
// ENV gate
// --------
//   GNFS_FILTER_RADIX_SORT=0  (default)  → std::sort path, zero behaviour change
//   GNFS_FILTER_RADIX_SORT=1             → radix sort path
//
// Anything else (unset, empty, "garbage", "2", "true") is treated as 0.
// The gate is cached on the first call via `std::once_flag`; tests can
// reload it through `filter_radix_sort_reset_env_cache_for_testing()`.
//
// Threading
// ---------
// Single-threaded by design. SIMD and multi-threaded radix variants are
// out of scope for this drop-in. Memory: O(n) scratch (one auxiliary
// uint64 key buffer + one auxiliary index buffer + one Relation move
// buffer used during the final permutation).

#include "../core/relation.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace gnfs::relation {

namespace detail {

/// Sentinel that flips the sign bit so signed int64 values radix-sort in
/// numeric order under unsigned byte comparison. Two's-complement
/// negatives (sign bit = 1) move below non-negatives (sign bit = 0) once
/// XORed with the bias.
inline constexpr std::uint64_t kSignBias = 0x8000000000000000ULL;

inline std::atomic<bool>& cached_gate_state() noexcept {
    static std::atomic<bool> state{false};
    return state;
}

inline std::once_flag& cached_gate_flag() noexcept {
    static std::once_flag flag;
    return flag;
}

inline bool resolve_gate_from_env() noexcept {
    const char* v = std::getenv("GNFS_FILTER_RADIX_SORT");
    if (v == nullptr) {
        return false;
    }
    // Strict parse: only literal "1" enables. Anything else (including
    // "true", "2", "garbage", empty string) keeps the default std::sort
    // path. This mirrors the conservative gate style used elsewhere
    // (sieve bucket prefetch / relation pool) where opt-in must be
    // explicit.
    if (std::strcmp(v, "1") == 0) {
        return true;
    }
    return false;
}

/// Single LSD byte-pass over an array of uint64 keys with a parallel
/// auxiliary array (here, the Relation index permutation). Reads from
/// `(keys_in, indices_in)` and writes to `(keys_out, indices_out)`.
inline void radix_byte_pass(const std::uint64_t* keys_in,
                            const std::uint32_t* indices_in,
                            std::uint64_t* keys_out,
                            std::uint32_t* indices_out,
                            std::size_t n,
                            unsigned shift) noexcept {
    std::array<std::size_t, 256> count{};
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t byte = static_cast<std::uint8_t>(keys_in[i] >> shift);
        ++count[byte];
    }
    // Exclusive prefix sum → bucket starting offsets.
    std::size_t sum = 0;
    for (std::size_t b = 0; b < 256; ++b) {
        const std::size_t c = count[b];
        count[b] = sum;
        sum += c;
    }
    // Scatter, preserving input order within each bucket (stability).
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t byte = static_cast<std::uint8_t>(keys_in[i] >> shift);
        const std::size_t dst = count[byte]++;
        keys_out[dst] = keys_in[i];
        indices_out[dst] = indices_in[i];
    }
}

}  // namespace detail

/// Returns whether `sort_relations` should dispatch to the radix path.
/// Result is cached on the first call so the dispatch site pays only a
/// relaxed atomic load. Tests may toggle the ENV and call
/// `filter_radix_sort_reset_env_cache_for_testing()` to re-resolve.
[[nodiscard]] inline bool filter_radix_sort_enabled() noexcept {
    std::call_once(detail::cached_gate_flag(), []() noexcept {
        detail::cached_gate_state().store(detail::resolve_gate_from_env(),
                                          std::memory_order_relaxed);
    });
    return detail::cached_gate_state().load(std::memory_order_relaxed);
}

/// Re-read GNFS_FILTER_RADIX_SORT and update the cached gate. Intended
/// solely for unit tests that need to flip the ENV between scenarios.
inline void filter_radix_sort_reset_env_cache_for_testing() noexcept {
    detail::cached_gate_state().store(detail::resolve_gate_from_env(),
                                      std::memory_order_relaxed);
    // Mark the once_flag as completed so a subsequent
    // `filter_radix_sort_enabled()` call does not overwrite us with the
    // value resolved at first-touch time.
    std::call_once(detail::cached_gate_flag(), []() noexcept {
        // No-op: state already set above.
    });
}

/// Stable LSD byte-radix sort of `relations` by `(b, a)` lex order.
/// Equivalent to the std::sort comparator at `collector.hpp:617`:
///     if (b != other.b) return b < other.b;
///     return a < other.a;
///
/// Implementation: 8 byte-passes over the biased `a` key (low to high)
/// then 8 byte-passes over the `b` key (low to high). Stability across
/// the 16 passes makes `b` dominate; `a` ties break in numeric order
/// thanks to the kSignBias XOR (which maps int64 to a strictly
/// monotone uint64 image).
inline void radix_sort_relations(std::vector<core::Relation>& relations) {
    const std::size_t n = relations.size();
    if (n < 2) {
        return;
    }

    // Build two parallel key arrays (a_biased, b) and the identity index
    // permutation. The relations array stays untouched until the final
    // permute step, so the (≥ 500 byte) Relation struct is read once
    // here and moved once at the end. All radix passes operate on the
    // 8-byte keys + 4-byte indices, which fit comfortably in L1/L2.
    std::vector<std::uint64_t> keys_a(n);
    std::vector<std::uint64_t> keys_b(n);
    std::vector<std::uint32_t> idx(n);
    for (std::size_t i = 0; i < n; ++i) {
        keys_a[i] = static_cast<std::uint64_t>(relations[i].a) ^ detail::kSignBias;
        keys_b[i] = relations[i].b;
        idx[i] = static_cast<std::uint32_t>(i);
    }

    // Ping-pong scratch buffers. Two key arrays (active key + auxiliary)
    // plus two index arrays. After 8 passes the active key flips back to
    // the input pointer; we keep the auxiliary index buffer separate so
    // the final permutation reads from the radix output.
    std::vector<std::uint64_t> keys_aux(n);
    std::vector<std::uint32_t> idx_aux(n);

    auto run_8_passes = [&](std::vector<std::uint64_t>& primary_keys) noexcept {
        std::uint64_t* k_in = primary_keys.data();
        std::uint64_t* k_out = keys_aux.data();
        std::uint32_t* i_in = idx.data();
        std::uint32_t* i_out = idx_aux.data();
        for (unsigned pass = 0; pass < 8; ++pass) {
            const unsigned shift = pass * 8;
            detail::radix_byte_pass(k_in, i_in, k_out, i_out, n, shift);
            std::swap(k_in, k_out);
            std::swap(i_in, i_out);
        }
        // After 8 passes (even number of swaps) the active key is back
        // in `primary_keys`, and the active index in `idx`. No copy
        // needed.
    };

    // Stage 1: sort by `a` (secondary). Index ends up in `idx`.
    run_8_passes(keys_a);

    // Stage 2: sort by `b` (primary). Stability preserves the `a` order
    // within each `b` bucket. We must walk `keys_b` through the current
    // permutation before stage 2, otherwise stage 2 would sort the
    // original (unpermuted) `b` array.
    {
        std::vector<std::uint64_t> keys_b_permuted(n);
        for (std::size_t i = 0; i < n; ++i) {
            keys_b_permuted[i] = keys_b[idx[i]];
        }
        keys_b.swap(keys_b_permuted);
    }
    run_8_passes(keys_b);

    // Apply the final permutation. The relations array is rebuilt from
    // moves so each Relation is touched exactly once.
    std::vector<core::Relation> sorted;
    sorted.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        sorted.push_back(std::move(relations[idx[i]]));
    }
    relations.swap(sorted);
}

}  // namespace gnfs::relation
