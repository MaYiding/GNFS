#pragma once

// Wheel-2-3-5 fast strip helpers for trial division.
//
// Background: many sieve cofactors have several factors of 2, 3, and 5.
// Stripping these upfront (before iterating the rational factor base) reduces
// the inner-loop divisions for the common case where the cofactor still has
// significant low-prime mass.
//
// The helpers are bit-for-bit equivalent to the naive
//     while (v % p == 0 && exp < 255) { v /= p; ++exp; }
// pattern. Specifically, strip_2 uses __builtin_ctzll on uint64_t, and the
// 128-bit variant strips the low limb first.
//
// All helpers are noexcept and pure: they only mutate a local copy of v plus
// the supplied exponent counters. Exponent values are clamped to 255 to match
// the existing TrialDivisionResult::exponents element type (uint8_t).
//
// Cross-platform notes:
//   - __builtin_ctzll: available on clang (macOS) and gcc (Linux). Undefined
//     when the argument is zero, so callers guard with v != 0.
//   - __uint128_t: already used elsewhere in cofactor/, so no portability
//     concern beyond the existing project baseline.

#include <cstdint>

namespace gnfs::cofactor::wheel {

// ----- uint64_t variants -----

/// Strip all factors of 2 from v; record exponent (capped at 255).
[[nodiscard]] inline uint64_t strip_2(uint64_t v, uint8_t& exp) noexcept {
    exp = 0;
    if (v == 0) return v;
    // ctz returns the count of trailing zero bits — exactly the 2-adic valuation.
    unsigned cnt = static_cast<unsigned>(__builtin_ctzll(v));
    if (cnt > 255u) cnt = 255u;
    v >>= cnt;
    exp = static_cast<uint8_t>(cnt);
    // If we had to clamp at 255, the original loop also stops at 255 with the
    // remaining factors-of-2 still inside v, so behavior is identical.
    return v;
}

/// Strip all factors of 3 from v; record exponent (capped at 255).
[[nodiscard]] inline uint64_t strip_3(uint64_t v, uint8_t& exp) noexcept {
    exp = 0;
    while (v != 0 && v % 3u == 0 && exp < 255u) {
        v /= 3u;
        ++exp;
    }
    return v;
}

/// Strip all factors of 5 from v; record exponent (capped at 255).
[[nodiscard]] inline uint64_t strip_5(uint64_t v, uint8_t& exp) noexcept {
    exp = 0;
    while (v != 0 && v % 5u == 0 && exp < 255u) {
        v /= 5u;
        ++exp;
    }
    return v;
}

// ----- __uint128_t variants -----

/// Strip all factors of 2 from a 128-bit value.
[[nodiscard]] inline __uint128_t strip_2(__uint128_t v, uint8_t& exp) noexcept {
    exp = 0;
    if (v == 0) return v;
    // Process low limb first; if it is all zero, dive into the high limb.
    uint64_t lo = static_cast<uint64_t>(v);
    unsigned cnt = 0;
    if (lo != 0) {
        cnt = static_cast<unsigned>(__builtin_ctzll(lo));
    } else {
        uint64_t hi = static_cast<uint64_t>(v >> 64);
        // hi cannot be zero because v != 0 and lo == 0
        cnt = 64u + static_cast<unsigned>(__builtin_ctzll(hi));
    }
    if (cnt > 255u) cnt = 255u;
    v >>= cnt;
    exp = static_cast<uint8_t>(cnt);
    return v;
}

/// Strip all factors of 3 from a 128-bit value.
[[nodiscard]] inline __uint128_t strip_3(__uint128_t v, uint8_t& exp) noexcept {
    exp = 0;
    while (v != 0 && v % 3u == 0 && exp < 255u) {
        v /= 3u;
        ++exp;
    }
    return v;
}

/// Strip all factors of 5 from a 128-bit value.
[[nodiscard]] inline __uint128_t strip_5(__uint128_t v, uint8_t& exp) noexcept {
    exp = 0;
    while (v != 0 && v % 5u == 0 && exp < 255u) {
        v /= 5u;
        ++exp;
    }
    return v;
}

// ----- Combined helper -----

/// Combined wheel-2-3-5 strip. Equivalent to applying strip_2 → strip_3 → strip_5
/// in order. Order matters when these are used to populate factor_indices in a
/// TrialDivisionResult so that 2 appears before 3 before 5 (matching FB order).
template <typename T>
[[nodiscard]] inline T strip_235(T v, uint8_t& exp_2, uint8_t& exp_3, uint8_t& exp_5) noexcept {
    v = strip_2(v, exp_2);
    v = strip_3(v, exp_3);
    v = strip_5(v, exp_5);
    return v;
}

} // namespace gnfs::cofactor::wheel
