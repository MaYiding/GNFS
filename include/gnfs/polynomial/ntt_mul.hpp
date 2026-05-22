#pragma once

// Polynomial multiplication helper over F_p[x] (with p < 2^32) backed by a
// Number Theoretic Transform (NTT) and 3-prime CRT reconstruction.
//
// Background:
//   Classical schoolbook polynomial multiplication of two operands of size
//   n costs O(n^2). Karatsuba (see karatsuba_mul.hpp) reduces this to
//   O(n^1.585). The asymptotic optimum for polynomial multiplication is
//   O(n log n) via fast convolution. This helper provides such a primitive
//   using a Number Theoretic Transform over three small "Schönhage"
//   NTT-friendly primes followed by Chinese Remainder Theorem (CRT)
//   reconstruction back to the user-supplied modulus p.
//
//   The NTT is a Cooley-Tukey FFT-style fast convolution where every
//   complex root of unity is replaced by a primitive 2^k-th root of unity
//   modulo a "Schönhage prime" q of shape q = c · 2^k + 1. Working modulo
//   such a prime lets us do all transform arithmetic in integer modular
//   form, with no floating-point rounding errors.
//
// Why 3 primes:
//   The convolution of two polynomials with coefficients in [0, p) and
//   transform size n produces output coefficients bounded by (p-1)^2 · n.
//   For p < 2^32 and n < 2^24 this bound is < 2^88, which exceeds the
//   capacity of any single 64-bit prime. Working modulo three < 2^30
//   primes gives ≈ 90-bit CRT capacity, vastly exceeding the bound.
//
//   Each of the three primes is chosen so the inner-loop butterfly
//   arithmetic uint64_t * uint64_t fits in uint64_t (max product
//   < 2^60), which is the entire point: 3-prime NTT lets us avoid
//   __uint128_t in the hot loop, which would otherwise dominate the
//   constant factor on platforms without a fast 128-bit multiplier.
//
// Chosen primes and roots (cmake-built-in):
//   q1 = 998244353  = 119 · 2^23 + 1, primitive root g1 = 3
//   q2 = 985661441  = 235 · 2^22 + 1, primitive root g2 = 3
//   q3 = 754974721  = 45  · 2^24 + 1, primitive root g3 = 11
//
//   Smallest 2^k support across the three primes is q2's 2^22 = 4 194 304
//   coefficients — far above any practical GNFS polynomial size. Each
//   omega for transform size n = 2^m is computed as g^{(q-1) / n} mod q.
//
//   For each prime q, "the primitive 2^k-th root of unity" is computed as
//   g^c mod q (where q-1 = c · 2^k). To obtain a primitive n = 2^m root
//   for m ≤ k, square the 2^k root (k - m) times.
//
// ENV (GNFS_POLY_NTT):
//   unset / empty / "auto"       → Auto (default; NTT above threshold,
//                                  schoolbook below).
//   "0" / "off"                  → ForceOff (always schoolbook).
//   "1" / "on"                   → ForceOn  (NTT for any non-trivial size).
//   Anything else                → Auto.
//
// Threshold rationale (kNttAutoThreshold = 256):
//   NTT has high per-call overhead: three forward transforms + three
//   inverse transforms + three pointwise multiplies + Garner-style CRT
//   reconstruction per output coefficient + the cost of zero-padding both
//   operands to the next power of two ≥ deg_a + deg_b + 1. For small
//   inputs schoolbook's tight inner loop dominates. Empirically the
//   crossover for uint64-coefficient F_p[x] multiplication on modern CPUs
//   is in the 128 - 512 range; we pick 256 as a conservative midpoint.
//
//   Above threshold, NTT is asymptotically O(n log n) versus schoolbook's
//   O(n^2). ForceOn lets callers exercise the NTT path even on small
//   inputs (useful for test parity coverage). ForceOff disables NTT
//   entirely.
//
// Modulus precondition:
//   p prime, p < 2^32 (so uint64_t * uint64_t fits without overflow in
//   the CRT reduction step). Caller must ensure all coefficients of `a`
//   and `b` are reduced mod p. The helper does not validate primality of
//   p; that is the caller's responsibility.
//
// Bit-for-bit guarantee:
//   For any (a, b, p) with p prime, p < 2^32, coefficients in [0, p),
//       ntt_mul_mod(a, b, p, out)
//   produces the same `out` vector (length, trailing-zero trim, coefficient
//   values) as
//       schoolbook_mul_mod(a, b, p, out)
//   bit-for-bit. The gate value affects only which kernel computes the
//   result, not the mathematical answer.
//
// Integration status:
//   Future-infrastructure helper. The main `ModularPoly::mul_raw` entry
//   continues to use schoolbook. Callers (e.g. a future Half-GCD rewrite
//   or sub-quadratic divrem wired end-to-end) call this helper directly.
//   No behavior change for existing callers.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace gnfs::polynomial {

/// Three-state dispatch decision for the NTT helper.
/// Auto routes to NTT when input size >= kNttAutoThreshold, schoolbook
/// otherwise. ForceOff always uses schoolbook. ForceOn always uses NTT
/// (for any non-trivial size).
enum class PolyNttMode {
    Auto,
    ForceOff,
    ForceOn,
};

/// NTT autodispatch threshold. When `max(deg_a, deg_b) < kNttAutoThreshold`
/// under Auto, the helper falls back to schoolbook. Below this size NTT's
/// O(n log n) cost is dominated by its high constant factor.
inline constexpr size_t kNttAutoThreshold = 256;

namespace detail_ntt {

/// First Schönhage NTT prime: 998244353 = 119 · 2^23 + 1.
inline constexpr uint64_t kNttPrime1 = 998244353ULL;
inline constexpr uint64_t kNttRoot1  = 3ULL;
inline constexpr int      kNttMaxLog1 = 23;

/// Second Schönhage NTT prime: 985661441 = 235 · 2^22 + 1.
inline constexpr uint64_t kNttPrime2 = 985661441ULL;
inline constexpr uint64_t kNttRoot2  = 3ULL;
inline constexpr int      kNttMaxLog2 = 22;

/// Third Schönhage NTT prime: 754974721 = 45 · 2^24 + 1.
inline constexpr uint64_t kNttPrime3 = 754974721ULL;
inline constexpr uint64_t kNttRoot3  = 11ULL;
inline constexpr int      kNttMaxLog3 = 24;

/// Maximum transform size supported (governed by smallest prime's k).
inline constexpr size_t kNttMaxSize = 1ULL << kNttMaxLog2;  // 4 194 304

/// Modular exponentiation a^e mod m, with a < m < 2^31 (so a*a fits 62
/// bits and the `%` reduction stays in uint64_t). Square-and-multiply
/// with right-to-left bit scan.
[[nodiscard]] inline uint64_t pow_mod_u64(uint64_t a, uint64_t e,
                                          uint64_t m) noexcept {
    uint64_t result = 1 % m;
    uint64_t base = a % m;
    while (e > 0) {
        if (e & 1ULL) {
            result = (result * base) % m;
        }
        base = (base * base) % m;
        e >>= 1;
    }
    return result;
}

/// Modular inverse via Fermat's little theorem: a^{-1} = a^{m-2} mod m
/// when m is prime and gcd(a, m) = 1. Valid for our NTT primes.
[[nodiscard]] inline uint64_t inv_mod_u64(uint64_t a, uint64_t m) noexcept {
    return pow_mod_u64(a, m - 2, m);
}

/// Round n up to the next power of two, or 1 if n == 0.
[[nodiscard]] inline size_t next_pow2(size_t n) noexcept {
    if (n <= 1) return 1;
    size_t r = 1;
    while (r < n) r <<= 1;
    return r;
}

/// Bit-reverse permutation in place. `n` must be a power of two.
/// Used as the standard pre-pass for iterative Cooley-Tukey NTT.
inline void bit_reverse_permute(std::vector<uint64_t>& a) noexcept {
    const size_t n = a.size();
    if (n <= 1) return;
    size_t j = 0;
    for (size_t i = 1; i < n; ++i) {
        size_t bit = n >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
}

/// In-place iterative Cooley-Tukey NTT modulo prime q with primitive
/// 2^L-th root `omega_full`, where L = log2(a.size()). All butterfly
/// multiplications are uint64 * uint64 (max product < (q-1)^2 < 2^60).
///
/// `a.size()` must be a power of two and ≤ 2^k_q (the prime's transform
/// capacity).
///
/// When `inverse == false`: forward NTT with the supplied omega.
/// When `inverse == true`: inverse NTT (with omega's inverse) followed
/// by per-element multiplication by n^{-1} mod q.
inline void ntt_inplace(std::vector<uint64_t>& a,
                        uint64_t omega_full,
                        uint64_t q,
                        bool inverse) noexcept {
    const size_t n = a.size();
    assert(n > 0 && (n & (n - 1)) == 0 && "NTT size must be power of two");
    if (n == 1) return;

    if (inverse) {
        omega_full = inv_mod_u64(omega_full, q);
    }

    bit_reverse_permute(a);

    // Iterative Cooley-Tukey: butterfly groups of size 2, 4, 8, ..., n.
    for (size_t len = 2; len <= n; len <<= 1) {
        // omega for this stage: omega_full raised to (n / len). Each
        // outer doubling halves the exponent, so we may also stride.
        const uint64_t w_len =
            pow_mod_u64(omega_full,
                        static_cast<uint64_t>(n / len), q);
        for (size_t i = 0; i < n; i += len) {
            uint64_t w = 1;
            const size_t half = len >> 1;
            for (size_t k = 0; k < half; ++k) {
                const uint64_t u = a[i + k];
                const uint64_t v = (a[i + k + half] * w) % q;
                uint64_t s = u + v;
                if (s >= q) s -= q;
                a[i + k] = s;
                uint64_t d = (u >= v) ? (u - v) : (u + q - v);
                a[i + k + half] = d;
                w = (w * w_len) % q;
            }
        }
    }

    if (inverse) {
        const uint64_t n_inv =
            inv_mod_u64(static_cast<uint64_t>(n) % q, q);
        for (size_t i = 0; i < n; ++i) {
            a[i] = (a[i] * n_inv) % q;
        }
    }
}

/// Multiply two polynomials modulo a single NTT prime q. The output
/// `out` is the convolution `a * b` modulo q (NOT reduced modulo any
/// user-facing p — that happens in the CRT reduction step). Output
/// length is `a.size() + b.size() - 1` (or 0 if either input is empty).
inline void ntt_mul_single_prime(const std::vector<uint64_t>& a,
                                 const std::vector<uint64_t>& b,
                                 uint64_t q,
                                 uint64_t prim_root,
                                 int max_log,
                                 std::vector<uint64_t>& out) {
    if (a.empty() || b.empty()) {
        out.clear();
        return;
    }
    const size_t result_size = a.size() + b.size() - 1;
    const size_t m = next_pow2(result_size);
    assert(m <= (1ULL << max_log) &&
           "NTT size exceeds Schönhage prime capacity");

    // Reduce input coefficients into [0, q) and zero-pad to m.
    std::vector<uint64_t> A(m, 0), B(m, 0);
    for (size_t i = 0; i < a.size(); ++i) A[i] = a[i] % q;
    for (size_t i = 0; i < b.size(); ++i) B[i] = b[i] % q;

    // omega is a primitive m-th root of unity mod q.
    // Per the prime's definition q - 1 = c * 2^max_log; therefore
    // omega_full = g^{(q-1) / m} mod q is a primitive m-th root for any
    // m = 2^L with L ≤ max_log.
    const uint64_t omega_full =
        pow_mod_u64(prim_root,
                    (q - 1) / static_cast<uint64_t>(m), q);

    ntt_inplace(A, omega_full, q, /*inverse=*/false);
    ntt_inplace(B, omega_full, q, /*inverse=*/false);

    for (size_t i = 0; i < m; ++i) {
        A[i] = (A[i] * B[i]) % q;
    }

    ntt_inplace(A, omega_full, q, /*inverse=*/true);

    out.assign(result_size, 0);
    for (size_t i = 0; i < result_size; ++i) {
        out[i] = A[i];
    }
}

/// Garner-style 3-prime CRT reconstruction with simultaneous reduction
/// mod p. Inputs r1, r2, r3 are the residues of a single output
/// coefficient modulo (q1, q2, q3). Output is the unique value
/// x ∈ [0, q1*q2*q3) satisfying x ≡ r_i (mod q_i), reduced mod p.
///
/// Garner's reconstruction:
///   u1 = r1
///   u2 = ((r2 - r1) * q1^{-1} mod q2) mod q2
///   u3 = ((r3 - r1 - q1*u2) * (q1*q2)^{-1} mod q3) mod q3
///   x  = u1 + q1*u2 + q1*q2*u3
///
/// Reduction mod p: we never materialize x directly (it can be 90 bits).
/// Instead we compute
///   x mod p = (u1 + (q1 mod p)*u2 + (q1*q2 mod p)*u3) mod p
/// where each q_i (and thus q1*q2 mod p, precomputed by caller) is
/// < 2^32 and each u_i is < 2^30, so each product is < 2^62 and fits
/// uint64_t.
[[nodiscard]] inline uint64_t crt_combine_3(
        uint64_t r1, uint64_t r2, uint64_t r3,
        uint64_t q1, uint64_t q2, uint64_t q3,
        uint64_t inv_q1_mod_q2, uint64_t inv_q1q2_mod_q3,
        uint64_t q1_mod_p, uint64_t q1q2_mod_p,
        uint64_t p) noexcept {
    // u2 = (r2 - r1) * inv(q1) mod q2.
    uint64_t diff2 = (r2 >= r1) ? (r2 - r1) : (r2 + q2 - r1);
    // r1 < q1 < q2, so r1 < q2. If r2 < r1 (which can happen), the
    // above adjusts correctly without overflow.
    uint64_t u2 = (diff2 * inv_q1_mod_q2) % q2;

    // u3 = (r3 - r1 - q1*u2) * inv(q1*q2) mod q3.
    // Compute q1 * u2 first (< 2^60), reduce mod q3.
    uint64_t q1_u2_mod_q3 = ((q1 % q3) * u2) % q3;
    // r3 - r1 - q1*u2, all mod q3.
    uint64_t s = r3;
    uint64_t r1_mod_q3 = r1 % q3;
    s = (s >= r1_mod_q3) ? (s - r1_mod_q3) : (s + q3 - r1_mod_q3);
    s = (s >= q1_u2_mod_q3) ? (s - q1_u2_mod_q3) : (s + q3 - q1_u2_mod_q3);
    uint64_t u3 = (s * inv_q1q2_mod_q3) % q3;

    // x mod p = (r1 + q1*u2 + q1*q2*u3) mod p, computed without
    // overflowing uint64_t.
    uint64_t r1_mod_p = r1 % p;
    uint64_t u2_mod_p = u2 % p;
    uint64_t u3_mod_p = u3 % p;
    uint64_t t1 = (q1_mod_p * u2_mod_p) % p;
    uint64_t t2 = (q1q2_mod_p * u3_mod_p) % p;
    uint64_t x = r1_mod_p + t1;
    if (x >= p) x -= p;
    x += t2;
    if (x >= p) x -= p;
    return x;
}

/// Cached ENV reader for GNFS_POLY_NTT.
/// Three-state: Auto / ForceOff / ForceOn. Anything unrecognized → Auto.
struct NttGateCache {
    std::once_flag once;
    std::atomic<int> mode{static_cast<int>(PolyNttMode::Auto)};
};

[[nodiscard]] inline NttGateCache& ntt_gate_cache() noexcept {
    static NttGateCache cache;
    return cache;
}

[[nodiscard]] inline PolyNttMode parse_ntt_gate_env() noexcept {
    const char* env = std::getenv("GNFS_POLY_NTT");
    if (env == nullptr || env[0] == '\0') {
        return PolyNttMode::Auto;
    }
    if (std::strcmp(env, "0") == 0 || std::strcmp(env, "off") == 0) {
        return PolyNttMode::ForceOff;
    }
    if (std::strcmp(env, "1") == 0 || std::strcmp(env, "on") == 0) {
        return PolyNttMode::ForceOn;
    }
    if (std::strcmp(env, "auto") == 0) {
        return PolyNttMode::Auto;
    }
    return PolyNttMode::Auto;
}

}  // namespace detail_ntt

/// Returns the current process-wide NTT gate mode.
///
/// First call: parses GNFS_POLY_NTT (see header docstring).
/// Subsequent calls: returns cached value. Test-only reset available via
/// `poly_ntt_reset_env_cache_for_testing()`.
[[nodiscard]] inline PolyNttMode poly_ntt_mode() noexcept {
    auto& cache = detail_ntt::ntt_gate_cache();
    std::call_once(cache.once, []() {
        detail_ntt::ntt_gate_cache().mode.store(
            static_cast<int>(detail_ntt::parse_ntt_gate_env()),
            std::memory_order_relaxed);
    });
    return static_cast<PolyNttMode>(
        cache.mode.load(std::memory_order_relaxed));
}

/// Test-only: reset the ENV cache. After calling this, the next
/// `poly_ntt_mode()` will re-read the env. Not thread-safe against
/// concurrent readers; must be called from test setup only.
inline void poly_ntt_reset_env_cache_for_testing() noexcept {
    auto& cache = detail_ntt::ntt_gate_cache();
    cache.once.~once_flag();
    new (&cache.once) std::once_flag();
    cache.mode.store(static_cast<int>(PolyNttMode::Auto),
                     std::memory_order_relaxed);
}

/// Dispatcher decision: returns true iff NTT should be used for the
/// given (deg_a, deg_b). Routing rules per gate:
///   - either input empty → false (caller short-circuits to empty output)
///   - max(deg_a, deg_b) <= 1 → false (size-0 or size-1 multiply is trivial)
///   - ForceOff → false
///   - ForceOn → true (for any non-trivial size ≥ 2)
///   - Auto → true iff max(deg_a, deg_b) >= kNttAutoThreshold
[[nodiscard]] inline bool poly_ntt_enabled_for_size(size_t deg_a,
                                                    size_t deg_b) noexcept {
    if (deg_a == 0 || deg_b == 0) return false;
    const size_t nmax = (deg_a > deg_b) ? deg_a : deg_b;
    if (nmax <= 1) return false;
    const PolyNttMode mode = poly_ntt_mode();
    if (mode == PolyNttMode::ForceOff) return false;
    if (mode == PolyNttMode::ForceOn)  return true;
    // Auto
    return nmax >= kNttAutoThreshold;
}

/// Schoolbook polynomial multiplication mod p. Matches the semantics of
/// `ModularPoly::mul_raw` and serves as the test golden / dispatch
/// fallback for tiny inputs / ForceOff path.
///
/// Precondition: p prime, p < 2^32, all coefficients of `a` and `b` are
/// in [0, p).
inline void schoolbook_mul_mod(const std::vector<uint64_t>& a,
                               const std::vector<uint64_t>& b,
                               uint64_t p,
                               std::vector<uint64_t>& out) {
    if (a.empty() || b.empty()) {
        out.clear();
        return;
    }
    const size_t result_size = a.size() + b.size() - 1;
    out.assign(result_size, 0);
    for (size_t i = 0; i < a.size(); ++i) {
        const uint64_t ai = a[i];
        if (ai == 0) continue;
        for (size_t j = 0; j < b.size(); ++j) {
            const uint64_t bj = b[j];
            if (bj == 0) continue;
            // ai, bj < 2^32 so ai * bj < 2^64 fits in uint64_t.
            const uint64_t prod = (ai * bj) % p;
            uint64_t sum = out[i + j] + prod;
            if (sum >= p) sum -= p;
            out[i + j] = sum;
        }
    }
    // Trim trailing zeros to canonical form, matching schoolbook_mul_mod
    // contract used by tests and helper parity comparisons.
    while (!out.empty() && out.back() == 0) {
        out.pop_back();
    }
}

/// Main NTT multiplication entry. Dispatches between schoolbook and
/// 3-prime NTT according to the runtime gate (`poly_ntt_mode`) and the
/// compile-time size threshold (`kNttAutoThreshold`).
///
/// Output `out` has length `a.size() + b.size() - 1` (before trimming)
/// or 0 if either input is empty. Trailing zeros are trimmed to give a
/// canonical polynomial representation matching `schoolbook_mul_mod`.
///
/// Routing rules (delegated to `poly_ntt_enabled_for_size`):
///   - empty input or size-1 multiply → schoolbook short-circuit
///   - gate == ForceOff → schoolbook
///   - gate == ForceOn  → NTT (for any non-trivial size ≥ 2)
///   - gate == Auto and max(|a|, |b|) >= kNttAutoThreshold → NTT
///   - gate == Auto and max(|a|, |b|) <  kNttAutoThreshold → schoolbook
inline void ntt_mul_mod(const std::vector<uint64_t>& a,
                        const std::vector<uint64_t>& b,
                        uint64_t p,
                        std::vector<uint64_t>& out) {
    if (a.empty() || b.empty()) {
        out.clear();
        return;
    }

    if (!poly_ntt_enabled_for_size(a.size(), b.size())) {
        schoolbook_mul_mod(a, b, p, out);
        return;
    }

    // 3-prime NTT path. Compute per-prime convolution residues then
    // CRT-recombine each output coefficient back into F_p.
    using namespace detail_ntt;

    std::vector<uint64_t> r1, r2, r3;
    ntt_mul_single_prime(a, b, kNttPrime1, kNttRoot1, kNttMaxLog1, r1);
    ntt_mul_single_prime(a, b, kNttPrime2, kNttRoot2, kNttMaxLog2, r2);
    ntt_mul_single_prime(a, b, kNttPrime3, kNttRoot3, kNttMaxLog3, r3);

    // CRT-precomputed scalars (constant across all output coefficients).
    static const uint64_t inv_q1_mod_q2 =
        inv_mod_u64(kNttPrime1 % kNttPrime2, kNttPrime2);
    static const uint64_t inv_q1q2_mod_q3 =
        inv_mod_u64((kNttPrime1 % kNttPrime3) * (kNttPrime2 % kNttPrime3) % kNttPrime3,
                    kNttPrime3);

    // Per-call p-dependent reductions.
    const uint64_t q1_mod_p   = kNttPrime1 % p;
    const uint64_t q1q2_mod_p =
        (q1_mod_p * (kNttPrime2 % p)) % p;

    const size_t result_size = a.size() + b.size() - 1;
    out.assign(result_size, 0);
    for (size_t i = 0; i < result_size; ++i) {
        out[i] = crt_combine_3(r1[i], r2[i], r3[i],
                               kNttPrime1, kNttPrime2, kNttPrime3,
                               inv_q1_mod_q2, inv_q1q2_mod_q3,
                               q1_mod_p, q1q2_mod_p, p);
    }

    // Trim trailing zeros to canonical form.
    while (!out.empty() && out.back() == 0) {
        out.pop_back();
    }
}

}  // namespace gnfs::polynomial
