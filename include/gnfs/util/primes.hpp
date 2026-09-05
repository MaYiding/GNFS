#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace gnfs::util {

[[nodiscard]] inline uint64_t add_mod_u64(uint64_t a, uint64_t b, uint64_t mod) {
    if (mod == 0) return 0;
    a %= mod;
    b %= mod;
    return (a >= mod - b) ? (a - (mod - b)) : (a + b);
}

/// Modular multiplication with a native 128-bit fast path where available.
[[nodiscard]] inline uint64_t mul_mod_u64(uint64_t a, uint64_t b, uint64_t mod) {
    if (mod == 0) return 0;
#if defined(__SIZEOF_INT128__)
    __uint128_t prod = static_cast<__uint128_t>(a) * b;
    return static_cast<uint64_t>(prod % mod);
#else
    uint64_t result = 0;
    a %= mod;
    while (b != 0) {
        if (b & 1U) result = add_mod_u64(result, a, mod);
        b >>= 1U;
        if (b != 0) a = add_mod_u64(a, a, mod);
    }
    return result;
#endif
}

[[nodiscard]] inline bool u64_gt_square(uint64_t value, uint64_t bound) noexcept {
    if (bound != 0 && bound > std::numeric_limits<uint64_t>::max() / bound) {
        return false;
    }
    return value > bound * bound;
}

[[nodiscard]] inline bool u64_gt_cube(uint64_t value, uint64_t bound) noexcept {
    if (bound != 0 && bound > std::numeric_limits<uint64_t>::max() / bound) {
        return false;
    }
    uint64_t square = bound * bound;
    if (bound != 0 && square > std::numeric_limits<uint64_t>::max() / bound) {
        return false;
    }
    return value > square * bound;
}

/// Modular exponentiation
[[nodiscard]] inline uint64_t pow_mod_u64(uint64_t base, uint64_t exp, uint64_t mod) {
    if (mod == 0) return 0;
    uint64_t result = 1;
    base %= mod;
    while (exp > 0) {
        if (exp & 1) result = mul_mod_u64(result, base, mod);
        base = mul_mod_u64(base, base, mod);
        exp >>= 1;
    }
    return result;
}

/// Miller-Rabin primality test, deterministic for n < 2^64.
/// 12 witnesses (Sinclair 2011) are sufficient for the full uint64_t range.
[[nodiscard]] inline bool is_prime_u64(uint64_t n) {
    if (n < 2) return false;
    if (n == 2 || n == 3) return true;
    if (n % 2 == 0) return false;

    uint64_t d = n - 1;
    uint64_t r = 0;
    while ((d & 1) == 0) {
        d >>= 1;
        ++r;
    }

    static const uint64_t witnesses[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    for (uint64_t a : witnesses) {
        if (a >= n) continue;
        uint64_t x = pow_mod_u64(a, d, n);
        if (x == 1 || x == n - 1) continue;

        bool composite = true;
        for (uint64_t i = 0; i < r - 1; ++i) {
            x = mul_mod_u64(x, x, n);
            if (x == n - 1) {
                composite = false;
                break;
            }
        }
        if (composite) return false;
    }
    return true;
}

/// Integer sqrt for uint32_t — robust at boundary (avoids std::sqrt double rounding).
[[nodiscard]] inline uint32_t isqrt_u32(uint32_t n) {
    if (n < 2) return n;
    uint32_t x = static_cast<uint32_t>(std::sqrt(static_cast<double>(n)));
    // Correct double rounding at boundary
    while (static_cast<uint64_t>(x + 1) * (x + 1) <= n) ++x;
    while (static_cast<uint64_t>(x) * x > n) --x;
    return x;
}

/// Simple primality for small uint32_t — Miller-Rabin overkill, trial division
/// with integer sqrt suffices and avoids the std::sqrt double-rounding pitfall.
[[nodiscard]] inline bool is_prime_u32(uint32_t n) {
    if (n < 2) return false;
    if (n < 4) return true;       // 2, 3
    if ((n & 1) == 0) return false;
    if (n % 3 == 0) return false;
    uint32_t s = isqrt_u32(n);
    for (uint32_t i = 5; i <= s; i += 6) {
        if (n % i == 0) return false;
        if (n % (i + 2) == 0) return false;
    }
    return true;
}

/// Next prime > n (uint64_t, with overflow guard returning 0).
[[nodiscard]] inline uint64_t next_prime_u64(uint64_t n) {
    if (n >= UINT64_MAX - 2) return 0;
    ++n;
    if (n <= 2) return 2;
    if ((n & 1) == 0) {
        if (n == UINT64_MAX) return 0;
        ++n;
    }
    while (!is_prime_u64(n)) {
        if (n > UINT64_MAX - 2) return 0;
        n += 2;
    }
    return n;
}

} // namespace gnfs::util
