#pragma once

/// @file siqs.hpp
/// @brief Self-Initializing Quadratic Sieve (SIQS) — Contini 1997
///
/// Efficient factorization for medium composites (optimal 40-95 digits;
/// auto-selected by pipeline for 25-100 digits).
/// Uses polynomial self-initialization for fast switching between
/// sieve polynomials, large-prime variation, and GF(2) linear algebra.

#include <gnfs/cofactor/squfof.hpp>
#include <gnfs/core/integer.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <gnfs/linalg/gauss.hpp>
#include <gnfs/linalg/sparse_matrix.hpp>
#include <gnfs/siqs/congruence.hpp>
#include <gnfs/siqs/live_sieve_capture.hpp>
#include <gnfs/siqs/relation.hpp>
#include <gnfs/siqs/runtime_facts.hpp>
#include <gnfs/siqs/shadow_proof_observe.hpp>
#include <gnfs/siqs/shadow_proof_prefer.hpp>
#include <gnfs/util/bit_intrin.hpp>
#include <gnfs/util/primes.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gnfs::siqs {

using core::Integer;

// ================================================================
// Parameters
// ================================================================

struct SIQSParams {
    uint32_t fb_size;           // factor base size (# primes)
    uint32_t sieve_half;        // half-width M of sieve interval [-M, M]
    uint32_t lp_multiplier;     // large prime bound = fb[last].p * mult
    uint32_t num_a_factors;     // # primes composing A
    uint32_t sieve_error;       // sieve approximation error (typically 8-15)
    uint32_t small_prime_cutoff; // skip sieving primes < this (account in threshold)
};

/// Parameter table calibrated from msieve/CADO-NFS/yafu
/// Parameter table calibrated from msieve/CADO-NFS/yafu.
/// LP multiplier: controls large prime bound = fb.back().p × mult.
/// 2LP is only effective when LP_space is small (≤ ~500K). For ≤49d,
/// graph-based 2LP merge finds many cycles. For ≥50d, LP space is too
/// large → 2LP graph density is insufficient → 0 cycles. Keep mult ≤ 100.
inline SIQSParams select_params(size_t digits) {
    // Each row: {fb_size, sieve_half, lp_mult, a_factors, sieve_error, small_cutoff}
    // sieve_error: only covers log approximation + prime powers; LP subtracted separately
    // NOTE: FB sizes must stay moderate until block sieve is implemented.
    // Without block sieve, per-poly cost scales linearly with FB — larger FB is slower.
    if (digits <= 20) return {50,     8192,    40,  2,  8,  5};
    if (digits <= 25) return {80,     16384,   40,  3,  8,  5};
    if (digits <= 34) return {250,    16384,   50,  4,  10, 10};
    if (digits <= 39) return {500,    32768,   60,  5,  10, 15};
    if (digits <= 44) return {1000,   32768,   80,  5,  11, 20};
    if (digits <= 49) return {1200,   65536,   100, 5,  11, 20};
    if (digits <= 54) return {1600,   65536,   120, 6,  12, 25};   // smaller FB → faster LA
    if (digits <= 59) return {1700,   32768,   200, 6,  12, 20};   // FB=1700, M=32768(L1), LP=200
    if (digits <= 62) return {2500,   32768,   200, 7,  13, 30};   // 60d: FB=2500 M=32768(L1) LP=200
    if (digits <= 66) return {4000,   32768,   200, 8,  14, 35};   // 65d: FB=4000 M=32768(L1) LP=200
    if (digits <= 69) return {5500,   65536,   150, 8,  14, 40};   // 68-69d optimal
    if (digits <= 74) return {15000,  131072,  120, 9,  14, 60};
    if (digits <= 79) return {25000,  131072,  150, 9,  15, 70};
    if (digits <= 84) return {30000,  131072,  150, 10, 15, 80};
    if (digits <= 89) return {80000,  262144,  200, 10, 16, 90};
    if (digits <= 95) return {130000, 524288,  200, 11, 16, 100};
    return                     {400000, 1048576, 200, 12, 17, 120};
}

// ================================================================
// Modular arithmetic (32-bit, for factor base primes)
// ================================================================

inline uint32_t mod_mul32(uint32_t a, uint32_t b, uint32_t m) {
    return static_cast<uint32_t>(static_cast<uint64_t>(a) * b % m);
}

inline uint32_t mod_pow32(uint32_t base, uint32_t exp, uint32_t mod) {
    uint64_t result = 1, b = base % mod;
    while (exp > 0) {
        if (exp & 1) result = result * b % mod;
        b = b * b % mod;
        exp >>= 1;
    }
    return static_cast<uint32_t>(result);
}

inline uint32_t mod_inv32(uint32_t a, uint32_t p) {
    return mod_pow32(a, p - 2, p); // Fermat
}

/// Tonelli-Shanks: compute sqrt(n) mod p, returns 0 if n is not QR
inline uint32_t tonelli_shanks(uint64_t n_u64, uint32_t p) {
    uint32_t n = static_cast<uint32_t>(n_u64 % p);
    if (n == 0) return 0;
    if (p == 2) return n & 1;

    // Euler criterion
    if (mod_pow32(n, (p - 1) / 2, p) != 1) return 0;

    // Factor p - 1 = Q * 2^S
    uint32_t Q = p - 1, S = 0;
    while ((Q & 1) == 0) { Q >>= 1; S++; }

    if (S == 1) { // p ≡ 3 (mod 4)
        return mod_pow32(n, (p + 1) / 4, p);
    }

    // Find quadratic non-residue z
    uint32_t z = 2;
    while (mod_pow32(z, (p - 1) / 2, p) != p - 1) z++;

    uint32_t M = S;
    uint32_t c = mod_pow32(z, Q, p);
    uint32_t t = mod_pow32(n, Q, p);
    uint32_t R = mod_pow32(n, (Q + 1) / 2, p);

    for (;;) {
        if (t == 1) return R;
        uint32_t i = 0, tmp = t;
        while (tmp != 1) { tmp = mod_mul32(tmp, tmp, p); i++; }

        uint32_t b = c;
        for (uint32_t j = 0; j + i + 1 < M; j++)
            b = mod_mul32(b, b, p);

        M = i;
        c = mod_mul32(b, b, p);
        t = mod_mul32(t, c, p);
        R = mod_mul32(R, b, p);
    }
}

// ================================================================
// Fast cofactor splitting for 2LP (64-bit composites)
// ================================================================

/// Pollard rho with Brent's cycle detection for 64-bit composites.
/// Uses portable modular multiplication. Very fast for N ≤ 2^48.
/// Expected O(N^{1/4}) iterations ≈ ~4K for 48-bit numbers.
inline uint64_t pollard_rho_64(uint64_t n) {
    if (n % 2 == 0) return 2;
    if (n % 3 == 0) return 3;
    if (n % 5 == 0) return 5;

    auto mul_mod = [n](uint64_t a, uint64_t b) -> uint64_t {
        return gnfs::util::mul_mod_u64(a, b, n);
    };
    auto f = [&](uint64_t x, uint64_t c) -> uint64_t {
        return gnfs::util::add_mod_u64(mul_mod(x, x), c, n);
    };

    for (uint64_t c = 1; c < 256; c++) {
        uint64_t y = 2, x = 2, q = 1;
        uint64_t ys = 0, d = 1;
        uint64_t range = 1;

        // Brent's algorithm with GCD batching
        while (d == 1) {
            x = y;
            for (uint64_t i = 0; i < range; i++)
                y = f(y, c);

            uint64_t k = 0;
            while (k < range && d == 1) {
                ys = y;
                uint64_t batch = std::min(uint64_t(128), range - k);
                for (uint64_t i = 0; i < batch; i++) {
                    y = f(y, c);
                    uint64_t diff = (x > y) ? x - y : y - x;
                    if (diff > 0) q = mul_mod(q, diff);
                }
                d = std::gcd(q, n);
                k += batch;
            }
            range *= 2;
            if (range > 2000000) break; // safety limit for large composites
        }

        if (d == n) {
            // Batched GCD overshot — retry step by step from saved ys
            d = 1;
            while (d == 1) {
                ys = f(ys, c);
                uint64_t diff = (x > ys) ? x - ys : ys - x;
                d = std::gcd(diff, n);
            }
        }
        if (d > 1 && d < n) return d;
    }
    return 1;
}

/// Split a 64-bit composite into two factors.
/// Uses trial division for small factors, then Pollard rho.
/// Returns {p1, p2} with p1 ≤ p2, or {0, 0} on failure.
inline std::pair<uint64_t, uint64_t> split_cofactor_64(uint64_t n) {
    if (n <= 1) return {0, 0};

    // Quick trial division for small factors
    for (uint64_t p : {2ULL,3ULL,5ULL,7ULL,11ULL,13ULL,17ULL,19ULL,23ULL,29ULL,
                       31ULL,37ULL,41ULL,43ULL,47ULL,53ULL,59ULL,61ULL,67ULL,71ULL,
                       73ULL,79ULL,83ULL,89ULL,97ULL}) {
        if (p * p > n) break;
        if (n % p == 0) return {p, n / p};
    }
    // Extended trial division with 6k±1 wheel to ~1000
    for (uint64_t p = 101; p < 1000; p += 2) {
        if (p * p > n) break;
        if (n % p == 0) return {p, n / p};
    }

    if (gnfs::util::is_prime_u64(n)) {
        return {0, 0};
    }

    // Pollard rho for larger composites (~1-10μs per number for ≤48 bit)
    uint64_t f = pollard_rho_64(n);
    if (f > 1 && f < n) {
        uint64_t q = n / f;
        return {std::min(f, q), std::max(f, q)};
    }

    // SQUFOF as last resort (handles edge cases Pollard rho misses)
    f = cofactor::SQUFOF::factor(n, 50000);
    if (f > 1 && f < n) {
        uint64_t q = n / f;
        return {std::min(f, q), std::max(f, q)};
    }
    return {0, 0};
}

// ================================================================
// Factor base
// ================================================================

struct FBPrime {
    uint32_t p;
    uint32_t sqrt_n;    // sqrt(N) mod p
    uint8_t  logp;      // floor(log2(p))
};

// ================================================================
// Knuth-Schroeppel multiplier selection
// ================================================================

/// Select optimal multiplier k for N. Returns k such that kN has a dense factor base.
/// Score = -0.5*log(kN) + Σ_{p small, kN is QR mod p} log(p)/(p-1)
inline uint32_t select_multiplier(const Integer& N) {
    static const uint32_t candidates[] = {
        1, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73
    };
    // Small primes for scoring
    static const uint32_t test_primes[] = {
        2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71,
        73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127, 131, 137, 139, 149, 151,
        157, 163, 167, 173, 179, 181, 191, 193, 197, 199
    };

    double best_score = -1e30;
    uint32_t best_k = 1;

    for (uint32_t k : candidates) {
        Integer kN;
        mpz_mul_ui(kN.get_mpz(), N.get_mpz(), k);  // kN = N * k (skip source copy)
        double log_kN = static_cast<double>(mpz_sizeinbase(kN.get_mpz(), 2)) * 0.6931; // log(2) * bits
        double score = -0.5 * log_kN;

        // Special handling for p=2
        uint32_t kN_mod8 = static_cast<uint32_t>(mpz_fdiv_ui(kN.get_mpz(), 8));
        if (kN_mod8 == 1) score += 2.0 * std::log(2.0); // both roots
        else if (kN_mod8 == 5) score += std::log(2.0); // one root

        for (size_t i = 1; i < sizeof(test_primes)/sizeof(test_primes[0]); i++) {
            uint32_t p = test_primes[i];
            // 当 v_p(kN) = 1(典型情况:k 候选都是 squarefree、gcd(k,N)=1),
            // x² ≡ kN mod p 只有 x≡0 mod p 单根,且 **不能** Hensel lift 到
            // mod p²(geometric sum 不适用)。贡献为 log(p)/p,而非
            // log(p)/(p-1)。msieve 与 Silverman 标准实现采用此公式。
            //
            // 边界:若 p² | kN(罕见,N 含平方因子),x² ≡ kN/p² mod p 又是 QR
            // 时会有额外二次贡献 ≈ 2 log(p)/(p²·(p-1)),数量级可忽略。
            if (k % p == 0) {
                score += std::log(static_cast<double>(p)) / p;
                continue;
            }
            uint32_t kN_mod_p = static_cast<uint32_t>(mpz_fdiv_ui(kN.get_mpz(), p));
            if (kN_mod_p == 0) {
                // p | N 但 p ∤ k:同上,v_p(kN)=v_p(N) 时根不 lift。
                score += std::log(static_cast<double>(p)) / p;
            } else if (mod_pow32(kN_mod_p, (p - 1) / 2, p) == 1) {
                // p ∤ kN 且 kN is QR mod p — 2 个根,Hensel lift 几何和:
                // 2 log(p)/(p-1)
                score += 2.0 * std::log(static_cast<double>(p)) / (p - 1);
            }
        }

        if (score > best_score) {
            best_score = score;
            best_k = k;
        }
    }
    return best_k;
}

/// Build factor base: primes p where Legendre(N, p) = 1
inline std::vector<FBPrime> build_factor_base(const Integer& N, size_t count) {
    std::vector<FBPrime> fb;
    fb.reserve(count + 1);

    // Element 0: "-1" for sign tracking
    fb.push_back({0, 0, 0}); // sentinel for negative Q(x)

    // N mod p for small primes — use GMP
    mpz_srcptr nz = N.get_mpz();

    auto is_prime = [](uint32_t n) -> bool {
        if (n < 2) return false;
        if (n < 4) return true;
        if (n % 2 == 0 || n % 3 == 0) return false;
        for (uint32_t i = 5; i * i <= n; i += 6)
            if (n % i == 0 || n % (i + 2) == 0) return false;
        return true;
    };

    // Always include p=2
    {
        uint32_t n_mod2 = static_cast<uint32_t>(mpz_fdiv_ui(nz, 2));
        if (n_mod2 == 1) { // N is odd, 2 is always in FB for QS
            fb.push_back({2, 1, 1});
        }
    }

    for (uint32_t p = 3; fb.size() <= count; p += 2) {
        if (!is_prime(p)) continue;

        uint64_t n_mod_p = mpz_fdiv_ui(nz, p);
        uint32_t sq = tonelli_shanks(n_mod_p, p);
        if (sq == 0 && n_mod_p != 0) continue; // not QR

        uint8_t logp = 0;
        { uint32_t tmp = p; while (tmp >>= 1) logp++; }

        fb.push_back({p, sq, logp});
    }

    return fb;
}

// ================================================================
// SIQS Polynomial: Q(x) = (Ax + B)^2 - N
// ================================================================

struct SIQSPoly {
    Integer A;
    Integer B;

    // Self-initialization data
    std::vector<uint32_t> a_indices;   // FB indices of primes composing A
    std::vector<Integer>  B_parts;     // B_i components for CRT

    // Per-FB-prime sieve start positions
    struct PrimeSolns {
        uint32_t soln1, soln2;
    };
    std::vector<PrimeSolns> solns;     // for each FB prime (skip index 0 = sign)

    // A^{-1} mod p for each FB prime (precomputed for self-init)
    std::vector<uint32_t> a_inv_mod_p;

    // Precomputed: B_parts[i] mod p for each (i, FB prime)
    // Layout: bp_mod_p[i * fb_size + j] = B_parts[i] mod fb[j].p
    // This avoids expensive GMP division in next_poly_B
    std::vector<uint32_t> bp_mod_p;
    size_t bp_fb_size = 0;  // fb.size() at time of precomputation

    // CRT coefficients for B_parts (needed for 32-bit bp_mod_p computation)
    std::vector<uint32_t> coeffs;  // coeff_i from B_parts computation
};

/// Choose A = product of num_factors primes from factor base
/// Target: A ≈ sqrt(2N) / M
inline void choose_A(const Integer& N, uint32_t M,
                     uint32_t num_factors,
                     const std::vector<FBPrime>& fb,
                     std::mt19937& rng,
                     std::vector<uint32_t>& a_indices, Integer& A) {
    // Target A value: target_a = sqrt(2N) / M
    Integer two_n;
    mpz_mul_2exp(two_n.get_mpz(), N.get_mpz(), 1);  // 2*N via bit shift
    Integer target_a = core::sqrt(two_n);
    target_a /= int64_t(M);
    if (target_a.is_zero() || target_a.is_negative()) target_a = int64_t(1);

    // Pick primes from middle of FB to form A close to target
    // Strategy: find the prime whose individual value would give
    // target^(1/num_factors), then pick around that range
    double log_target = static_cast<double>(mpz_sizeinbase(target_a.get_mpz(), 2));
    double log_per_factor = log_target / num_factors;
    uint32_t ideal_p = static_cast<uint32_t>(std::pow(2.0, log_per_factor));

    // Find the FB index closest to ideal_p
    size_t center = 1; // skip index 0 (sign)
    for (size_t i = 1; i < fb.size(); i++) {
        if (fb[i].p >= ideal_p) { center = i; break; }
        center = i;
    }

    // Pick num_factors primes around center, with some randomness
    a_indices.clear();
    size_t range_start = (center > num_factors * 2) ? center - num_factors * 2 : 1;
    size_t range_end = std::min(center + num_factors * 2, fb.size() - 1);

    // Generate candidate indices in range
    std::vector<size_t> candidates;
    candidates.reserve(range_end - range_start + 1);
    for (size_t i = range_start; i <= range_end; i++) {
        if (fb[i].p > 2) { // skip p=2 for A (simplifies self-init)
            candidates.push_back(i);
        }
    }

    // Shuffle and pick
    std::shuffle(candidates.begin(), candidates.end(), rng);
    size_t pick_count = std::min(static_cast<size_t>(num_factors), candidates.size());
    for (size_t i = 0; i < pick_count; i++) {
        a_indices.push_back(static_cast<uint32_t>(candidates[i]));
    }
    std::sort(a_indices.begin(), a_indices.end());

    // Compute A = product of chosen primes
    A = int64_t(1);
    for (uint32_t idx : a_indices) {
        A *= int64_t(fb[idx].p);
    }
}

/// Initialize polynomial for given A: compute first B and sieve start positions
inline void init_poly(const Integer& /*N*/, const std::vector<FBPrime>& fb,
                      uint32_t sieve_half, SIQSPoly& poly) {
    const size_t s = poly.a_indices.size();

    // Compute B_i parts using CRT
    // B_i = sqrt(N) mod q_i * (A/q_i)^{-1} mod q_i * (A/q_i)
    poly.B_parts.resize(s);

    for (size_t i = 0; i < s; i++) {
        uint32_t qi = fb[poly.a_indices[i]].p;
        uint32_t ti = fb[poly.a_indices[i]].sqrt_n; // sqrt(N) mod qi

        // Compute A/qi via mpz_tdiv_q_ui (skip clone+ /=)
        Integer A_div_qi;
        mpz_tdiv_q_ui(A_div_qi.get_mpz(), poly.A.get_mpz(), qi);

        // (A/qi)^{-1} mod qi
        uint32_t a_div_qi_mod = static_cast<uint32_t>(
            mpz_fdiv_ui(A_div_qi.get_mpz(), qi));
        uint32_t inv = mod_inv32(a_div_qi_mod, qi);

        // B_i = (A/qi) * coeff where coeff = ti * inv mod qi
        uint32_t coeff = mod_mul32(ti, inv, qi);
        mpz_mul_ui(poly.B_parts[i].get_mpz(), A_div_qi.get_mpz(), coeff);
    }

    // B = sum(B_i) mod A, adjusted to |B| <= A/2
    poly.B = int64_t(0);
    for (size_t i = 0; i < s; i++) {
        poly.B = poly.B + poly.B_parts[i];
    }
    // Reduce B mod A
    poly.B = poly.B % poly.A;
    // Center: if B > A/2, B = B - A (mpz_tdiv_q_2exp = bit shift)
    Integer half_A;
    mpz_tdiv_q_2exp(half_A.get_mpz(), poly.A.get_mpz(), 1);
    if (poly.B > half_A) {
        poly.B = poly.B - poly.A;
    }

    // Verify: B^2 ≡ N (mod A)
    // (skip in release for performance)

    // ── Fast 32-bit init: compute A mod p, A^{-1} mod p, bp_mod_p ──
    // A = product of s FB primes (all 32-bit), so A mod p = ∏(q_i mod p) mod p
    // using 64-bit arithmetic. Avoids all mpz_fdiv_ui calls (~20× faster).
    poly.a_inv_mod_p.resize(fb.size());
    poly.solns.resize(fb.size());
    poly.bp_fb_size = fb.size();
    poly.bp_mod_p.resize(s * fb.size());
    uint32_t M = sieve_half;

    // Store coefficients for bp_mod_p computation
    poly.coeffs.resize(s);
    Integer A_div_qi_tmp;
    for (size_t i = 0; i < s; i++) {
        uint32_t qi = fb[poly.a_indices[i]].p;
        uint32_t ti = fb[poly.a_indices[i]].sqrt_n;
        A_div_qi_tmp = poly.A;
        A_div_qi_tmp /= int64_t(qi);
        uint32_t a_div_qi_mod_qi = static_cast<uint32_t>(
            mpz_fdiv_ui(A_div_qi_tmp.get_mpz(), qi));
        poly.coeffs[i] = mod_mul32(ti, mod_inv32(a_div_qi_mod_qi, qi), qi);
    }

    // v22: hoist abs(B) out of FB loop — only depends on poly.B, not FB prime.
    // Per A switch we recompute B once, not per FB prime.
    const bool B_is_neg = poly.B.is_negative();
    Integer abs_B_hoisted;
    if (B_is_neg) {
        mpz_abs(abs_B_hoisted.get_mpz(), poly.B.get_mpz());  // abs of B (skip clone+abs)
    }
    const mpz_srcptr B_for_mod = B_is_neg ? abs_B_hoisted.get_mpz() : poly.B.get_mpz();

    for (size_t j = 1; j < fb.size(); j++) {
        uint32_t p = fb[j].p;

        // Compute A mod p using 32-bit arithmetic: ∏(q_i mod p)
        uint64_t a_mod_p64 = 1;
        for (size_t k = 0; k < s; k++) {
            a_mod_p64 = a_mod_p64 * (fb[poly.a_indices[k]].p % p) % p;
        }
        uint32_t a_mod_p = static_cast<uint32_t>(a_mod_p64);

        if (a_mod_p == 0) {
            poly.a_inv_mod_p[j] = 0;
            poly.solns[j] = {UINT32_MAX, UINT32_MAX};
            for (size_t i = 0; i < s; i++)
                poly.bp_mod_p[i * fb.size() + j] = 0;
            continue;
        }

        poly.a_inv_mod_p[j] = mod_inv32(a_mod_p, p);

        // Compute (A/q_i) mod p using prefix/suffix product trick (no inversions)
        // A/q_i mod p = ∏_{k≠i} (q_k mod p) mod p
        // 100-digit config uses num_a_factors=12,刚好顶到旧上限。
        // 升到 16 留余量,assert 防越界。
        constexpr size_t MAX_A_FACTORS = 16;
        assert(s <= MAX_A_FACTORS && "SIQS num_a_factors > MAX_A_FACTORS");
        uint32_t qi_mod_p[MAX_A_FACTORS];
        for (size_t k = 0; k < s; k++)
            qi_mod_p[k] = fb[poly.a_indices[k]].p % p;

        for (size_t i = 0; i < s; i++) {
            uint64_t a_div_qi = 1;
            for (size_t k = 0; k < s; k++) {
                if (k != i) a_div_qi = a_div_qi * qi_mod_p[k] % p;
            }
            // bp_mod_p = (A/q_i mod p) × coeff_i mod p
            poly.bp_mod_p[i * fb.size() + j] = static_cast<uint32_t>(
                a_div_qi * poly.coeffs[i] % p);
        }

        // Compute B mod p using GMP (needed because B = B_raw mod A is not
        // computable purely from B_raw mod p). This is O(FB_size) GMP calls
        // per init but only once per A (not per B update).
        uint32_t b_mod_p;
        if (B_is_neg) {
            uint32_t abs_b_mod = static_cast<uint32_t>(
                mpz_fdiv_ui(B_for_mod, p));
            b_mod_p = (p - abs_b_mod) % p;
        } else {
            b_mod_p = static_cast<uint32_t>(
                mpz_fdiv_ui(B_for_mod, p));
        }

        uint32_t ainv = poly.a_inv_mod_p[j];
        uint32_t sq = fb[j].sqrt_n;

        uint32_t diff1 = (sq >= b_mod_p) ? sq - b_mod_p : sq + p - b_mod_p;
        uint32_t x1 = mod_mul32(diff1, ainv, p);

        uint32_t neg_sq = p - sq;
        uint32_t diff2 = (neg_sq >= b_mod_p) ? neg_sq - b_mod_p : neg_sq + p - b_mod_p;
        uint32_t x2 = mod_mul32(diff2, ainv, p);

        uint32_t m_mod = M % p;
        uint32_t s1 = x1 + m_mod;
        if (s1 >= p) s1 -= p;
        uint32_t s2 = x2 + m_mod;
        if (s2 >= p) s2 -= p;

        poly.solns[j] = {s1, s2};
    }
}

/// Self-initialization: switch to next B value using Gray code
/// gray_bit: which B_part to flip (0-indexed)
inline void next_poly_B(const std::vector<FBPrime>& fb,
                        uint32_t sieve_half,
                        SIQSPoly& poly,
                        size_t gray_bit, bool add) {
    // Update B: B_new = B_old ± 2 * B_parts[gray_bit] (mpz_mul_2exp = bit shift)
    Integer two_B;
    mpz_mul_2exp(two_B.get_mpz(), poly.B_parts[gray_bit].get_mpz(), 1);
    if (add) {
        poly.B = poly.B + two_B;
    } else {
        poly.B = poly.B - two_B;
    }

    // Update sieve positions for each FB prime
    (void)sieve_half;
    for (size_t i = 1; i < fb.size(); i++) {
        uint32_t ainv = poly.a_inv_mod_p[i];
        if (ainv == 0) continue;

        uint32_t p = fb[i].p;

        // Delta = 2 * B_part * A^{-1} mod p (using precomputed B_parts mod p)
        // Single mod: 2 × bp_mod × ainv ≤ 2 × p² < 2^65 fits uint64
        uint32_t bp_mod = poly.bp_mod_p[gray_bit * poly.bp_fb_size + i];
        uint32_t delta = static_cast<uint32_t>(
            (static_cast<uint64_t>(2) * bp_mod % p * ainv) % p);

        if (add) {
            // soln1 -= delta, soln2 += delta (mod p)
            uint32_t s1 = poly.solns[i].soln1 + (p - delta); // ∈ [0, 2p)
            if (s1 >= p) s1 -= p;
            poly.solns[i].soln1 = s1;
            uint32_t s2 = poly.solns[i].soln2 + delta; // ∈ [0, 2p)
            if (s2 >= p) s2 -= p;
            poly.solns[i].soln2 = s2;
        } else {
            uint32_t s1 = poly.solns[i].soln1 + delta;
            if (s1 >= p) s1 -= p;
            poly.solns[i].soln1 = s1;
            uint32_t s2 = poly.solns[i].soln2 + (p - delta);
            if (s2 >= p) s2 -= p;
            poly.solns[i].soln2 = s2;
        }
    }
}

// ================================================================
// Relation
// ================================================================

/// Exact admission predicate for a one-large-prime SIQS residual.
///
/// A composite residual cannot be represented by one LP column: accepting it
/// would leave untracked prime factors and can invalidate X^2 = Y^2 during
/// extraction. Keep this boundary named and directly testable so every sieve
/// arithmetic backend applies the same contract.
[[nodiscard]] inline bool is_valid_one_large_prime(uint64_t n) noexcept {
    return gnfs::util::is_prime_u64(n);
}

struct SIQSResidualAdmission {
    SIQSLiveSieveRelationKind kind;
    uint64_t large_prime;
    uint64_t large_prime2;
};

/// Classify one exact uint64 residual at the raw SIQS relation boundary.
///
/// The two-large-prime result remains an unresolved sentinel cofactor. It must
/// pass split_cofactor_64() and normalize_two_large_prime() before entering the
/// staged graph. A zero two-large-prime bound preserves the production 1LP-only
/// contract.
[[nodiscard]] inline std::optional<SIQSResidualAdmission>
classify_siqs_residual(uint64_t residual,
                       uint64_t large_prime_bound,
                       uint64_t two_large_prime_cofactor_bound) noexcept {
    if (residual <= 1) {
        return std::nullopt;
    }

    if (residual <= large_prime_bound) {
        if (is_valid_one_large_prime(residual)) {
            return SIQSResidualAdmission{
                SIQSLiveSieveRelationKind::one_lp, residual, 0};
        }
        if (two_large_prime_cofactor_bound > 0 &&
            residual <= two_large_prime_cofactor_bound) {
            return SIQSResidualAdmission{
                SIQSLiveSieveRelationKind::two_lp_candidate, residual, 1};
        }
        return std::nullopt;
    }
    if (two_large_prime_cofactor_bound > 0 &&
        residual <= two_large_prime_cofactor_bound &&
        !is_valid_one_large_prime(residual)) {
        return SIQSResidualAdmission{
            SIQSLiveSieveRelationKind::two_lp_candidate, residual, 1};
    }
    return std::nullopt;
}

/// Convert a nonnegative GMP integer to an exact 64-bit unsigned value.
///
/// GMP's mpz_fits_ulong_p()/mpz_get_ui() pair follows the platform width of
/// unsigned long, which is only 32 bits on Windows LLP64. Exporting one native
/// uint64_t word keeps the SIQS cofactor boundary identical on every platform.
/// The caller-provided destination also avoids an allocation inside mpz_export.
[[nodiscard]] inline std::optional<uint64_t>
nonnegative_mpz_to_uint64_checked(mpz_srcptr value) noexcept {
    if (mpz_sgn(value) < 0 || mpz_sizeinbase(value, 2) > 64) {
        return std::nullopt;
    }

    uint64_t result = 0;
    size_t exported_words = 0;
    mpz_export(&result, &exported_words, -1, sizeof(result), 0, 0, value);
    if (exported_words > 1) {
        return std::nullopt;
    }
    return result;
}

// ================================================================
// Sieve kernel
// ================================================================

/// Sieve one polynomial and collect smooth relations.
/// sieve_buf: caller-owned buffer (avoids reallocation per polynomial)
/// @param lp_bound_sq  Upper bound for 2LP cofactors (set to 0 to disable 2LP)
inline void sieve_polynomial(
    const SIQSPoly& poly,
    const Integer& N,
    const std::vector<FBPrime>& fb,
    uint32_t sieve_half,
    uint8_t threshold,
    uint32_t small_cutoff,
    uint64_t lp_bound,
    uint64_t lp_bound_sq,
    std::vector<SIQSRelation>& out_relations,
    std::mutex& relations_mutex,
    std::vector<uint8_t>& sieve_buf,
    std::vector<uint8_t>& exp_buf,
    SIQSLiveSieveCaptureController* live_capture = nullptr)
{
    if (live_capture != nullptr && live_capture->stopped()) {
        return;
    }

    uint32_t M = sieve_half;
    uint32_t sieve_size = 2 * M;

    // Reuse caller's buffer
    sieve_buf.resize(sieve_size);
    std::memset(sieve_buf.data(), 0, sieve_size);
    uint8_t* sieve = sieve_buf.data();

    // Find first FB index with p >= small_cutoff (avoid branch in hot loop)
    size_t fb_start = 1;
    while (fb_start < fb.size() && fb[fb_start].p < small_cutoff) fb_start++;

    // Phase 1: Block sieve — process sieve in L1-cache-sized blocks
    // Each block stays in L1 cache (~128KB on M-series), improving hit rate for
    // primes with stride < block_size. Block overhead is amortized by cache benefit.
    constexpr uint32_t BLOCK_SIZE = 32768; // 32KB — fits comfortably in L1

    if (sieve_size <= BLOCK_SIZE) {
        // Small sieve: no blocking needed
        for (size_t i = fb_start; i < fb.size(); i++) {
            uint32_t p = fb[i].p;
            uint32_t s1 = poly.solns[i].soln1;
            uint32_t s2 = poly.solns[i].soln2;
            if (s1 == UINT32_MAX) continue;
            uint8_t logp = fb[i].logp;
            if (s1 == s2) {
                for (uint32_t pos = s1; pos < sieve_size; pos += p)
                    sieve[pos] += logp;
            } else {
                uint32_t pos1 = s1, pos2 = s2;
                if (pos1 > pos2) std::swap(pos1, pos2);
                while (pos2 < sieve_size) {
                    sieve[pos1] += logp;
                    sieve[pos2] += logp;
                    pos1 += p; pos2 += p;
                }
                if (pos1 < sieve_size) sieve[pos1] += logp;
            }
        }
    } else {
        // Large sieve: standard linear sieve
        // NOTE: Block sieve was tested (32KB blocks, hybrid small/large split) but
        // showed no benefit on Apple M-series (128KB L1D means 262KB sieve only
        // exceeds L1 by 2×, insufficient cache pressure). Sieve throughput is already
        // near hardware-limited (~12B writes/sec approaching M1 peak).
        for (size_t i = fb_start; i < fb.size(); i++) {
            uint32_t p = fb[i].p;
            uint32_t s1 = poly.solns[i].soln1;
            uint32_t s2 = poly.solns[i].soln2;
            if (s1 == UINT32_MAX) continue;
            uint8_t logp = fb[i].logp;
            if (s1 == s2) {
                for (uint32_t pos = s1; pos < sieve_size; pos += p)
                    sieve[pos] += logp;
            } else {
                uint32_t pos1 = s1, pos2 = s2;
                if (pos1 > pos2) std::swap(pos1, pos2);
                while (pos2 < sieve_size) {
                    sieve[pos1] += logp;
                    sieve[pos2] += logp;
                    pos1 += p; pos2 += p;
                }
                if (pos1 < sieve_size) sieve[pos1] += logp;
            }
        }
    }

    // Phase 2: Identify candidates and trial divide. Keep raw GMP handles for
    // the hot operations, but let Integer own their lifetime so an allocation
    // or capture-sink exception cannot bypass mpz_clear().
    Integer ax_scratch, value_scratch, q_scratch;
    mpz_ptr ax_mpz = ax_scratch.get_mpz();
    mpz_ptr val_mpz = value_scratch.get_mpz();
    mpz_ptr Q_mpz = q_scratch.get_mpz();

    // Heap-allocated touched buffer (reused across candidates).
    // Old uint32_t touched[256] on stack overflowed for fb_size > 256 when a Q(x)
    // was hit by many small primes (each prime power contributes one touched slot).
    std::vector<uint32_t> touched_buf;
    touched_buf.reserve(fb.size());

    for (uint32_t cand_pos = 0; cand_pos < sieve_size; cand_pos++) {
        if (sieve[cand_pos] < threshold) continue;

        if (live_capture != nullptr) {
            live_capture->observe_threshold_candidate();
            if (live_capture->stopped()) {
                break;
            }
        }

        // Candidate found — compute Q(x) and trial divide
        int64_t x = static_cast<int64_t>(cand_pos) - static_cast<int64_t>(M);

        // value = Ax + B (using raw GMP for speed)
        mpz_mul_ui(ax_mpz, poly.A.get_mpz(), static_cast<uint64_t>(std::abs(x)));
        if (x >= 0)
            mpz_add(val_mpz, ax_mpz, poly.B.get_mpz());
        else
            mpz_sub(val_mpz, poly.B.get_mpz(), ax_mpz);

        // Q(x) = value^2 - N
        mpz_mul(Q_mpz, val_mpz, val_mpz);
        mpz_sub(Q_mpz, Q_mpz, N.get_mpz());
        bool negative = (mpz_sgn(Q_mpz) < 0);
        if (negative) mpz_neg(Q_mpz, Q_mpz);

        Integer value; mpz_set(value.get_mpz(), val_mpz);
        Integer Q; mpz_set(Q.get_mpz(), Q_mpz);

        // Trial divide Q by factor base primes using reusable exponent buffer
        // (avoids per-candidate heap allocation of fb.size() bytes)
        uint8_t* exp = exp_buf.data();
        // Track which indices were touched for selective clear.
        // Previous code used uint32_t touched[256] on stack — for 100-digit configs
        // with fb_size=400000, a single Q can be hit by far more than 256 FB primes.
        touched_buf.clear();
        struct ExponentResetGuard {
            uint8_t* exponents;
            const std::vector<uint32_t>& touched;

            ~ExponentResetGuard() noexcept {
                for (uint32_t index : touched) {
                    exponents[index] = 0;
                }
            }
        } exponent_reset{exp, touched_buf};
        auto record_touched = [&](uint32_t idx) { touched_buf.push_back(idx); };

        bool accept = false;
        uint64_t large_prime = 0;
        uint64_t large_prime2 = 0;
        std::optional<SIQSLiveSieveRelationKind> relation_kind;
        bool residual_unrepresentable = false;

#if defined(__SIZEOF_INT128__)
        if (Q.bit_length() <= 127) {
            // Native 128-bit trial division (no GMP) — zero-alloc limb access
            uint64_t lo = mpz_getlimbn(Q.get_mpz(), 0);
            uint64_t hi = mpz_getlimbn(Q.get_mpz(), 1);
            __uint128_t q128 = (static_cast<__uint128_t>(hi) << 64) | lo;

            // Divide out A primes first
            for (uint32_t ai : poly.a_indices) {
                uint32_t p = fb[ai].p;
                while (q128 % p == 0) {
                    q128 /= p;
                    if (exp[ai] == 0) record_touched(ai);
                    exp[ai]++;
                }
            }

            // Trial divide by all FB primes (early exit when fully smooth)
            for (size_t i = 1; i < fb.size() && q128 > 1; i++) {
                uint32_t p = fb[i].p;
                if (q128 % p == 0) {
                    record_touched(static_cast<uint32_t>(i));
                    do { q128 /= p; exp[i]++; } while (q128 % p == 0);
                }
            }

            // Check cofactor
            if (q128 == 1) {
                accept = true;
                relation_kind = SIQSLiveSieveRelationKind::full;
            } else if (q128 <= UINT64_MAX) {
                const auto admission = classify_siqs_residual(
                    static_cast<uint64_t>(q128), lp_bound, lp_bound_sq);
                if (admission) {
                    accept = true;
                    relation_kind = admission->kind;
                    large_prime = admission->large_prime;
                    large_prime2 = admission->large_prime2;
                }
            } else {
                residual_unrepresentable = true;
            }
        } else
#endif
        {
            // GMP fallback for very large Q (>127 bits, rare for ≤65 digits)
            Integer residual_scratch;
            mpz_ptr q_mpz = residual_scratch.get_mpz();
            mpz_set(q_mpz, Q.get_mpz());

            for (uint32_t ai : poly.a_indices) {
                uint32_t p = fb[ai].p;
                if (mpz_divisible_ui_p(q_mpz, p)) {
                    if (exp[ai] == 0) record_touched(ai);
                    do { mpz_divexact_ui(q_mpz, q_mpz, p); exp[ai]++; }
                    while (mpz_divisible_ui_p(q_mpz, p));
                }
            }
            for (size_t i = 1; i < fb.size() && mpz_cmp_ui(q_mpz, 1) > 0; i++) {
                uint32_t p = fb[i].p;
                if (mpz_divisible_ui_p(q_mpz, p)) {
                    record_touched(static_cast<uint32_t>(i));
                    do { mpz_divexact_ui(q_mpz, q_mpz, p); exp[i]++; }
                    while (mpz_divisible_ui_p(q_mpz, p));
                }
            }

            if (mpz_cmp_ui(q_mpz, 1) == 0) {
                accept = true;
                relation_kind = SIQSLiveSieveRelationKind::full;
            } else if (auto cofactor = nonnegative_mpz_to_uint64_checked(q_mpz)) {
                const auto admission =
                    classify_siqs_residual(*cofactor, lp_bound, lp_bound_sq);
                if (admission) {
                    accept = true;
                    relation_kind = admission->kind;
                    large_prime = admission->large_prime;
                    large_prime2 = admission->large_prime2;
                }
            } else {
                residual_unrepresentable = true;
            }
        }

        if (!accept && live_capture != nullptr) {
            if (residual_unrepresentable) {
                live_capture->observe_unrepresentable_residual();
            } else {
                live_capture->observe_rejected_residual();
            }
            if (live_capture->stopped()) {
                break;
            }
        }

        if (accept) {
            bool capture_reserved = false;
            if (live_capture != nullptr) {
                if (!relation_kind) {
                    live_capture->observe_rejected_residual();
                    break;
                }
                const size_t value_bits = value.bit_length();
                const size_t value_bytes =
                    value_bits / 8 + static_cast<size_t>(value_bits % 8 != 0);
                const SIQSLiveSieveRelationPayloadShape payload{
                    value_bytes, fb.size(), touched_buf.size(), 0};
                if (!live_capture->try_reserve_relation(*relation_kind, payload)) {
                    break;
                }
                capture_reserved = true;
            }

            try {
                SIQSRelation rel;
                rel.value = std::move(value);
                rel.negative = negative;
                rel.large_prime = large_prime;
                rel.large_prime2 = large_prime2;
                // Copy only touched exponents (sparse → dense)
                rel.exponents.assign(fb.size(), 0);
                rel.fb_indices.reserve(touched_buf.size());
                for (uint32_t idx : touched_buf) {
                    rel.exponents[idx] = exp[idx];
                    rel.fb_indices.push_back(idx);
                }

                std::lock_guard<std::mutex> lock(relations_mutex);
                out_relations.push_back(std::move(rel));
            } catch (...) {
                if (capture_reserved) {
                    (void)live_capture->cancel_reserved_relation();
                }
                throw;
            }
            if (capture_reserved && !live_capture->commit_reserved_relation()) {
                break;
            }
        }
        if (live_capture != nullptr && live_capture->stopped()) {
            break;
        }
    }
}

// ================================================================
// Large prime merging
// ================================================================

/// Merge two relations: combine their values and exponents
inline SIQSRelation merge_two(const SIQSRelation& a, const SIQSRelation& b,
                               size_t fb_size) {
    SIQSRelation merged;
    merged.value = a.value * b.value;
    merged.negative = (a.negative != b.negative);
    merged.large_prime = 0;
    merged.large_prime2 = 0;
    merged.exponents.resize(fb_size, 0);
    for (size_t i = 0; i < fb_size; i++) {
        uint8_t ea = (i < a.exponents.size()) ? a.exponents[i] : 0;
        uint8_t eb = (i < b.exponents.size()) ? b.exponents[i] : 0;
        merged.exponents[i] = ea + eb;
    }
    // Copy existing merge_lps from both sides
    merged.merge_lps = a.merge_lps;
    merged.merge_lps.reserve(a.merge_lps.size() + b.merge_lps.size());
    merged.merge_lps.insert(merged.merge_lps.end(), b.merge_lps.begin(), b.merge_lps.end());
    // Build fb_indices — likely union of a.fb_indices and b.fb_indices.
    merged.fb_indices.reserve(a.fb_indices.size() + b.fb_indices.size());
    for (size_t i = 0; i < fb_size; i++) {
        if (merged.exponents[i] > 0)
            merged.fb_indices.push_back(static_cast<uint32_t>(i));
    }
    return merged;
}

/// Graph-based LP merge: 1LP pair matching + 2LP cycle finding.
///
/// Algorithm:
/// 1. Factor all 2LP cofactors into (p1, p2) using trial div + SQUFOF
/// 2. Build LP graph: each relation with LP(s) is an edge
/// 3. Iterative greedy merge: process LPs that appear in ≥2 relations
///    - Merge pairs to eliminate the shared LP
///    - Newly created 1LP relations feed back into the graph
///    - Repeat until no more merges possible
inline std::vector<SIQSRelation> merge_partials(
    std::vector<SIQSRelation>& relations, size_t fb_size, bool verbose = false)
{
    std::vector<SIQSRelation> full;
    // SIQS typical 10-30% relations are fully smooth (large_prime==0).
    full.reserve(relations.size() / 4);

    // Pool of all LP relations (grows as merges create new ones)
    std::vector<SIQSRelation> pool;
    pool.reserve(relations.size());
    size_t factored_2lp = 0, failed_2lp = 0, raw_1lp = 0, raw_2lp = 0;

    for (auto& rel : relations) {
        if (rel.large_prime == 0) {
            full.push_back(std::move(rel));
            continue;
        }
        // Factor unfactored 2LP cofactors
        if (rel.large_prime2 == 1) {
            auto [p1, p2] = split_cofactor_64(rel.large_prime);
            if (p1 > 0 && p2 > 0) {
                rel.large_prime = p1;
                rel.large_prime2 = p2;
                factored_2lp++;
                raw_2lp++;
            } else {
                failed_2lp++;
                continue; // can't use this relation
            }
        } else if (rel.large_prime2 == 0) {
            raw_1lp++;
        } else {
            raw_2lp++;
        }
        pool.push_back(std::move(rel));
    }

    if (verbose) {
        fprintf(stderr, "[SIQS] Merge input: %zu full, %zu 1LP, %zu 2LP (factored=%zu, failed=%zu)\n",
                full.size(), raw_1lp, raw_2lp, factored_2lp, failed_2lp);
    }

    // Iterative greedy merge (up to 10 rounds for convergence)
    std::vector<bool> consumed(pool.size(), false);
    size_t merged_1lp_pairs = 0, merged_2lp_cycles = 0;

    for (int round = 0; round < 10; round++) {
        // Build LP → relation index mapping
        std::unordered_map<uint64_t, std::vector<size_t>> lp_index;
        lp_index.reserve(pool.size() * 2);  // each rel contributes 1-2 LP keys
        for (size_t i = 0; i < pool.size(); i++) {
            if (consumed[i]) continue;
            lp_index[pool[i].large_prime].push_back(i);
            if (pool[i].large_prime2 > 0)
                lp_index[pool[i].large_prime2].push_back(i);
        }

        bool any_merge = false;

        // Process each LP: merge pairs that share this LP
        for (auto& [lp, indices] : lp_index) {
            // Remove consumed entries
            size_t write = 0;
            for (size_t r = 0; r < indices.size(); r++) {
                if (!consumed[indices[r]])
                    indices[write++] = indices[r];
            }
            indices.resize(write);

            while (indices.size() >= 2) {
                size_t ai = indices.back(); indices.pop_back();
                if (consumed[ai]) continue;
                size_t bi = SIZE_MAX;
                while (!indices.empty()) {
                    bi = indices.back(); indices.pop_back();
                    if (!consumed[bi]) break;
                    bi = SIZE_MAX;
                }
                if (bi == SIZE_MAX) break;

                consumed[ai] = consumed[bi] = true;
                any_merge = true;

                auto& a = pool[ai];
                auto& b = pool[bi];

                // Determine other LPs after eliminating 'lp'
                uint64_t other_a = (a.large_prime == lp) ? a.large_prime2 : a.large_prime;
                uint64_t other_b = (b.large_prime == lp) ? b.large_prime2 : b.large_prime;

                SIQSRelation merged = merge_two(a, b, fb_size);
                merged.merge_lps.push_back(lp);

                if ((other_a == 0 && other_b == 0) ||
                    (other_a == other_b && other_a > 0)) {
                    // Fully merged: both other LPs are zero, or they're the same and cancel
                    merged.large_prime = 0;
                    merged.large_prime2 = 0;
                    if (other_a > 0) merged.merge_lps.push_back(other_a);
                    full.push_back(std::move(merged));
                    if (other_a == 0) merged_1lp_pairs++;
                    else merged_2lp_cycles++;
                } else if (other_a == 0 || other_b == 0) {
                    // One remaining LP → new 1LP relation
                    merged.large_prime = std::max(other_a, other_b);
                    merged.large_prime2 = 0;
                    consumed.push_back(false);
                    pool.push_back(std::move(merged));
                } else {
                    // Two remaining LPs → new 2LP relation
                    merged.large_prime = std::min(other_a, other_b);
                    merged.large_prime2 = std::max(other_a, other_b);
                    consumed.push_back(false);
                    pool.push_back(std::move(merged));
                }
            }
        }

        if (!any_merge) break;
    }

    if (verbose) {
        fprintf(stderr, "[SIQS] Merge result: %zu usable (%zu 1LP-pair, %zu 2LP-cycle)\n",
                full.size(), merged_1lp_pairs, merged_2lp_cycles);
    }

    return full;
}

// ================================================================
// Matrix construction and linear algebra
// ================================================================

/// Dense GF(2) Gaussian elimination — much faster than sparse for SIQS matrices
/// Finds the left null space of M (row dependencies).
/// Matrix layout: ncols rows × nrows cols (transposed), packed in 64-bit words
inline std::vector<std::vector<size_t>> dense_gauss_left_nullspace(
    const std::vector<SIQSRelation>& relations, size_t fb_size, size_t max_deps = 64)
{
    size_t nrows = relations.size();
    size_t ncols = fb_size;

    // Build M^T as dense packed matrix (ncols rows × nrows columns)
    // Each row is a word-packed bit vector of nrows bits
    size_t words_per_row = (nrows + 63) / 64;
    std::vector<std::vector<uint64_t>> M(ncols, std::vector<uint64_t>(words_per_row, 0));

    for (size_t r = 0; r < nrows; r++) {
        const auto& rel = relations[r];
        size_t word = r / 64, bit = r % 64;
        if (rel.negative) M[0][word] |= (1ULL << bit);
        for (size_t c = 1; c < fb_size && c < rel.exponents.size(); c++) {
            if (rel.exponents[c] & 1) {
                M[c][word] |= (1ULL << bit);
            }
        }
    }

    // Gaussian elimination on M^T (ncols × nrows)
    // Find pivot in each row (column of M^T = relation index)
    std::vector<size_t> pivot_col(ncols, SIZE_MAX);
    std::vector<bool> is_pivot(nrows, false);

    for (size_t row = 0; row < ncols; row++) {
        // Find leftmost set bit
        size_t pc = SIZE_MAX;
        for (size_t w = 0; w < words_per_row; w++) {
            if (M[row][w]) {
                pc = w * 64 + static_cast<size_t>(gnfs::util::ctz64(M[row][w]));
                break;
            }
        }
        if (pc == SIZE_MAX || pc >= nrows) continue;
        pivot_col[row] = pc;
        is_pivot[pc] = true;

        // Eliminate this column from all other rows (parallel for large matrices)
        size_t w_pc = pc / 64, b_pc = pc % 64;
        uint64_t mask = 1ULL << b_pc;
        const uint64_t* pivot_row_data = M[row].data();

        if (ncols > 20000) {
            // Parallel elimination only for very large matrices
            // (thread creation per pivot: ~70μs overhead vs ~10μs work for ncols<10K)
            unsigned nt = std::max(1u, std::thread::hardware_concurrency());
            auto elim_chunk = [&](size_t start, size_t end) {
                for (size_t other = start; other < end; other++) {
                    if (other == row) continue;
                    if (M[other][w_pc] & mask) {
                        uint64_t* dst = M[other].data();
                        for (size_t k = 0; k < words_per_row; k++)
                            dst[k] ^= pivot_row_data[k];
                    }
                }
            };
            size_t chunk = (ncols + nt - 1) / nt;
            std::vector<std::thread> threads;
            for (unsigned t = 0; t < nt; t++) {
                size_t s = t * chunk, e = std::min(s + chunk, ncols);
                if (s < e) threads.emplace_back(elim_chunk, s, e);
            }
            for (auto& t : threads) t.join();
        } else {
            for (size_t other = 0; other < ncols; other++) {
                if (other == row) continue;
                if (M[other][w_pc] & mask) {
                    uint64_t* dst = M[other].data();
                    for (size_t k = 0; k < words_per_row; k++)
                        dst[k] ^= pivot_row_data[k];
                }
            }
        }
    }

    // Extract null space: free variables (non-pivot columns)
    std::vector<std::vector<size_t>> deps;
    for (size_t col = 0; col < nrows && deps.size() < max_deps; col++) {
        if (is_pivot[col]) continue;
        // Free variable col → null vector
        std::vector<size_t> dep;
        dep.push_back(col);
        // Find pivot rows that depend on this free variable
        for (size_t row = 0; row < ncols; row++) {
            if (pivot_col[row] == SIZE_MAX) continue;
            size_t w = col / 64, b = col % 64;
            if (M[row][w] & (1ULL << b)) {
                dep.push_back(pivot_col[row]);
            }
        }
        if (dep.size() > 1) {
            deps.push_back(std::move(dep));
        }
    }
    return deps;
}

/// Build GF(2) matrix from relations and find null space
inline std::vector<std::vector<size_t>> solve_matrix(
    const std::vector<SIQSRelation>& relations, size_t fb_size)
{
    size_t nrows = relations.size();
    size_t ncols = fb_size;

    if (ncols <= 100000) {
        // Dense Gaussian — O(ncols × nrows² / 64), fast for SIQS
        return dense_gauss_left_nullspace(relations, fb_size, 64);
    }

    // Block Lanczos for very large matrices
    linalg::SparseMatrix matrix(nrows, ncols);
    for (size_t r = 0; r < nrows; r++) {
        const auto& rel = relations[r];
        if (rel.negative) matrix.set(r, 0);
        for (size_t c = 1; c < fb_size && c < rel.exponents.size(); c++) {
            if (rel.exponents[c] & 1) matrix.set(r, c);
        }
    }
    linalg::BlockLanczos bl;
    auto bl_deps = bl.find_dependencies(matrix, 64);
    std::vector<std::vector<size_t>> deps;
    for (auto& bv : bl_deps) {
        std::vector<size_t> dep;
        for (size_t i = 0; i < nrows; i++) {
            if (i < bv.size() && bv[i]) dep.push_back(i);
        }
        if (!dep.empty()) deps.push_back(std::move(dep));
    }
    return deps;
}

// ================================================================
// Factor extraction: X^2 ≡ Y^2 (mod N) → gcd(X-Y, N)
// ================================================================

/// @param mod_N: modulus for X,Y computation (kN or N)
/// @param gcd_N: target for GCD (always original N)
inline std::optional<std::pair<Integer, Integer>> try_extract(
    const Integer& mod_N, const Integer& gcd_N,
    const std::vector<SIQSRelation>& relations,
    const std::vector<size_t>& dep,
    const std::vector<FBPrime>& fb)
{
    // Verify exponents are all even (matrix correctness check)
    std::vector<uint32_t> total_exp(fb.size(), 0);
    // Track sign parity: M[0] encodes the sign of the factorized Q side.
    // XOR of `negative` across the dependency must be zero before that side
    // can be represented by an ordinary modular square Y².
    uint32_t sign_parity = 0;
    for (size_t idx : dep) {
        const auto& rel = relations[idx];
        if (rel.negative) sign_parity ^= 1;
        for (size_t i = 0; i < fb.size() && i < rel.exponents.size(); i++) {
            total_exp[i] += rel.exponents[i];
        }
    }
    if (sign_parity) return std::nullopt; // odd number of negative values → skip
    for (size_t i = 0; i < fb.size(); i++) {
        if (total_exp[i] & 1) return std::nullopt; // parity error → skip
    }

    // Compute X = product of value_i mod mod_N
    Integer X(1);
    for (size_t idx : dep) {
        mpz_mul(X.get_mpz(), X.get_mpz(), relations[idx].value.get_mpz());
        mpz_mod(X.get_mpz(), X.get_mpz(), mod_N.get_mpz());
    }

    // Compute Y = product of p_i^{exp/2} * LP_products mod mod_N
    Integer Y(1);
    Integer pe;  // hoist — reused per FB prime
    for (size_t i = 1; i < fb.size(); i++) {
        uint32_t half_exp = total_exp[i] / 2;
        if (half_exp > 0) {
            mpz_set_ui(pe.get_mpz(), fb[i].p);
            mpz_powm_ui(pe.get_mpz(), pe.get_mpz(), half_exp, mod_N.get_mpz());
            mpz_mul(Y.get_mpz(), Y.get_mpz(), pe.get_mpz());
            mpz_mod(Y.get_mpz(), Y.get_mpz(), mod_N.get_mpz());
        }
    }

    // Include LP factors from merged relations
    Integer lp_int;  // hoist — reused per LP
    for (size_t idx : dep) {
        for (uint64_t lp : relations[idx].merge_lps) {
            // Integer's uint64_t assignment is lossless on Windows LLP64,
            // where GMP's mpz_set_ui accepts only a 32-bit unsigned long.
            lp_int = lp;
            mpz_mul(Y.get_mpz(), Y.get_mpz(), lp_int.get_mpz());
            mpz_mod(Y.get_mpz(), Y.get_mpz(), mod_N.get_mpz());
        }
    }

    // A dependency is not allowed to reach GCD merely because X +/- Y happens
    // to share a factor with N. It must first prove the complete SIQS square
    // congruence over the relation modulus kN.
    if (!are_congruent_squares(X, Y, mod_N)) {
        return std::nullopt;
    }

    // GCD against gcd_N (original N, not kN) to find factors
    Integer diff;
    mpz_sub(diff.get_mpz(), X.get_mpz(), Y.get_mpz());
    Integer g = core::gcd(diff, gcd_N);
    if (mpz_cmp_si(g.get_mpz(), 1) > 0 && g < gcd_N) {
        Integer other = gcd_N / g;
        return std::make_pair(std::move(g), std::move(other));
    }
    mpz_add(diff.get_mpz(), X.get_mpz(), Y.get_mpz());
    g = core::gcd(diff, gcd_N);
    if (mpz_cmp_si(g.get_mpz(), 1) > 0 && g < gcd_N) {
        Integer other = gcd_N / g;
        return std::make_pair(std::move(g), std::move(other));
    }
    return std::nullopt;
}

/// Try random XOR combinations of dependency vectors to increase success probability
inline std::optional<std::pair<Integer, Integer>> try_extract_with_combos(
    const Integer& mod_N, const Integer& gcd_N,
    const std::vector<SIQSRelation>& relations,
    const std::vector<std::vector<size_t>>& deps,
    const std::vector<FBPrime>& fb)
{
    // First try each dependency individually
    for (const auto& dep : deps) {
        auto result = try_extract(mod_N, gcd_N, relations, dep, fb);
        if (result) return result;
    }

    // Then try random XOR combinations of pairs
    std::mt19937 rng(12345);
    size_t max_combos = std::min(deps.size() * 3, size_t(200));

    for (size_t attempt = 0; attempt < max_combos; attempt++) {
        size_t i = rng() % deps.size();
        size_t j = rng() % deps.size();
        if (i == j) continue;

        // XOR two dependencies: symmetric difference of relation sets
        std::vector<bool> in_dep(relations.size(), false);
        for (size_t idx : deps[i]) in_dep[idx] = !in_dep[idx];
        for (size_t idx : deps[j]) in_dep[idx] = !in_dep[idx];

        std::vector<size_t> combined;
        for (size_t k = 0; k < relations.size(); k++) {
            if (in_dep[k]) combined.push_back(k);
        }
        if (combined.empty()) continue;

        auto result = try_extract(mod_N, gcd_N, relations, combined, fb);
        if (result) return result;
    }

    return std::nullopt;
}

// ================================================================
// Main SIQS entry point
// ================================================================

struct SIQSResult {
    Integer factor1, factor2;
    double  time_seconds;
    size_t  relations_found;
    size_t  polynomials_used;
    unsigned resolved_sieve_workers = 0;
    // True only after the observe record is written, flushed, and
    // stream-error free.
    bool shadow_proof_observe_record_committed = false;
};

namespace siqs_factor_detail {

/// Ephemeral route authority: no draft, decision, result, or telemetry object
/// retains this bit after factor() evaluates the three gates.
[[nodiscard]] constexpr bool
prefer_shadow_return_authorized(bool decision_is_shadow_candidate, bool prepared_candidate_matches,
                                bool prefer_decision_committed) noexcept {
    return decision_is_shadow_candidate && prepared_candidate_matches && prefer_decision_committed;
}

/// Revalidate the fully prepared result, emit the V2 pre-route decision, and
/// return a result only when all three ephemeral route gates pass. This helper
/// owns no callback or persistent authority and is the sole prefer route
/// adapter used by factor().
[[nodiscard]] inline std::optional<SIQSResult>
commit_prefer_route(std::FILE* output, const Integer& original_n,
                    const SIQSShadowProofPreferDecision& decision,
                    std::optional<SIQSResult> prepared_result) noexcept {
    const bool decision_is_shadow_candidate = decision.is_shadow_candidate();
    const bool prepared_candidate_matches =
        decision_is_shadow_candidate && prepared_result.has_value() &&
        prepared_result->factor1 == decision.candidate->factorization.factor &&
        prepared_result->factor2 == decision.candidate->factorization.cofactor &&
        prepared_result->time_seconds == 0.0 &&
        prepared_result->relations_found == decision.candidate->relations_found &&
        prepared_result->polynomials_used == decision.candidate->polynomials_used &&
        prepared_result->resolved_sieve_workers > 0 &&
        !prepared_result->shadow_proof_observe_record_committed;
    if (prepared_candidate_matches) {
        prepared_result->time_seconds = decision.candidate->time_seconds;
    }

    // A fallback decision may be recorded for audit, but it can never
    // authorize a shadow return even if a pre-finalization candidate was
    // prepared. A candidate/prepared mismatch emits nothing.
    if (!decision_is_shadow_candidate || prepared_candidate_matches) {
        const bool prefer_decision_committed =
            emit_siqs_shadow_proof_prefer_decision(output, original_n, decision);
        if (prefer_shadow_return_authorized(decision_is_shadow_candidate,
                                            prepared_candidate_matches,
                                            prefer_decision_committed)) {
            return prepared_result;
        }
    }
    return std::nullopt;
}

} // namespace siqs_factor_detail

static_assert(std::is_nothrow_move_constructible_v<std::optional<SIQSResult>>);
static_assert(noexcept(siqs_factor_detail::prefer_shadow_return_authorized(false, false, false)));
static_assert(noexcept(siqs_factor_detail::commit_prefer_route(
    nullptr, std::declval<const Integer&>(), std::declval<const SIQSShadowProofPreferDecision&>(),
    std::nullopt)));

inline std::optional<SIQSResult> factor(
    const Integer& N,
    size_t max_seconds = 3600,
    bool verbose = true)
{
    const SIQSShadowProofMode shadow_proof_mode =
        parse_siqs_shadow_proof_mode(std::getenv(SIQS_SHADOW_PROOF_ENV));

    auto start = std::chrono::steady_clock::now();
    auto elapsed = [&]() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
    };

    size_t digits = N.to_string().size();
    auto params = select_params(digits);

    // Knuth-Schroeppel multiplier selection
    uint32_t multiplier = select_multiplier(N);
    Integer kN;
    if (multiplier > 1) {
        mpz_mul_ui(kN.get_mpz(), N.get_mpz(), multiplier);  // kN = N * k (skip source copy)
    } else {
        kN = N;  // copy ctor
    }

    if (verbose) {
        fprintf(stderr, "[SIQS] N=%zu digits, k=%u, FB=%u, M=%u, A_factors=%u\n",
                digits, multiplier, params.fb_size, params.sieve_half, params.num_a_factors);
    }

    // Build factor base for kN (not N)
    auto fb = build_factor_base(kN, params.fb_size);
    size_t fb_size = fb.size();

    if (verbose) {
        fprintf(stderr, "[SIQS] Factor base built: %zu primes (%.3fs)\n",
                fb_size, elapsed());
    }

    // Large prime bound
    uint64_t lp_bound = static_cast<uint64_t>(fb.back().p) * params.lp_multiplier;

    // Sieve threshold computation:
    // We sieve Q(x)/A, where |Q(x)/A| ≈ M * sqrt(2N) at sieve boundary
    // log2(|Q(x)/A|) ≈ n_bits/2 + log2(M) + 0.5
    // Threshold = log2(target) - log2(LP_bound) - sieve_error - small_contrib
    size_t n_bits = mpz_sizeinbase(kN.get_mpz(), 2);
    double log_Qmax_d = static_cast<double>(n_bits) / 2.0 + std::log2(params.sieve_half) + 0.5;
    double lp_bits = std::log2(static_cast<double>(lp_bound));

    // Account for small primes we skip sieving
    double small_contrib = 0.0;
    for (size_t i = 1; i < fb.size(); i++) {
        if (fb[i].p >= params.small_prime_cutoff) break;
        small_contrib += 2.0 * fb[i].logp / fb[i].p;
    }

    double thr_d = log_Qmax_d - lp_bits - params.sieve_error - small_contrib;
    uint8_t threshold = (thr_d > 10.0) ? static_cast<uint8_t>(thr_d) : 10;

    // 2LP: DISABLED entirely. 2LP cycles cause extraction failures for certain
    // multipliers (all 64 deps fail). 1LP merge provides enough usable relations.
    uint64_t lp_bound_sq = 0;

    if (verbose) {
        fprintf(stderr, "[SIQS] log_Qmax=%.1f, lp_bits=%.1f, sieve_err=%u, "
                "small=%.1f, threshold=%u, LP_bound=%llu, 2LP=%s\n",
                log_Qmax_d, lp_bits, params.sieve_error,
                small_contrib, threshold, (unsigned long long)lp_bound,
                lp_bound_sq > 0 ? "on" : "off");
    }

    // Target: enough usable relations to exceed FB size
    size_t target_usable = fb_size + 50;

    std::vector<SIQSRelation> all_relations;
    all_relations.reserve(target_usable * 10);
    std::mutex relations_mutex;

    // Use atomic counters to avoid expensive estimate_usable under mutex
    std::atomic<size_t> atomic_full{0};
    std::atomic<size_t> atomic_1lp{0};
    std::atomic<size_t> atomic_2lp{0};

    size_t num_polys = 0;
    std::mt19937 rng(42);

    // Rough estimate: usable ≈ full + 1lp_partials * 1lp_merge_rate
    // 1LP merge rate is ~3-5% (pair matching). 2LP merge rate is ~0% for ≥50d
    // (LP space too large for graph cycles), so we only count 1LP.
    // Safety margin: +10% of target.
    // Safety margin: balance between overshoot cost and risk of falling short.
    // Small FB (<3000): merge rate ~8%, use +5% margin
    // Large FB (≥3000): merge rate ~5%, use +10% margin (conservative)
    size_t margin = (fb_size < 3000)
        ? std::max(size_t(30), target_usable / 20)   // +5% for small FB
        : std::max(size_t(100), target_usable / 10);  // +10% for large FB
    size_t safe_target = target_usable + margin;
    auto quick_estimate = [&]() -> size_t {
        size_t f = atomic_full.load(std::memory_order_relaxed);
        size_t p1 = atomic_1lp.load(std::memory_order_relaxed);
        // Empirical merge rate: small FB → high collision rate (~8%), large FB → low (~5%).
        // Use conservative estimate to avoid stopping too early.
        size_t divisor = (fb_size < 3000) ? 14 : 20;
        return f + p1 / divisor;
    };

    // Multi-threaded sieve: each thread processes its own A values
    const unsigned num_threads =
        resolve_siqs_sieve_workers(std::thread::hardware_concurrency());
    std::atomic<size_t> atomic_polys{0};
    std::atomic<bool> enough{false};

    auto sieve_worker = [&](unsigned thread_id) {
        std::mt19937 local_rng(42 + thread_id * 1000);
        std::vector<SIQSRelation> local_relations;
        local_relations.reserve(target_usable);
        std::vector<uint8_t> sieve_buf; // reuse across polynomials
        sieve_buf.reserve(params.sieve_half * 2);
        std::vector<uint8_t> exp_buf(params.fb_size + 100, 0); // reuse across candidates
        size_t local_full = 0, local_1lp = 0, local_2lp = 0;

        while (!enough.load(std::memory_order_relaxed) &&
               elapsed() < static_cast<double>(max_seconds))
        {
            SIQSPoly poly;
            choose_A(kN, params.sieve_half, params.num_a_factors, fb, local_rng,
                     poly.a_indices, poly.A);

            init_poly(kN, fb, params.sieve_half, poly);

            size_t num_B = size_t(1) << (poly.a_indices.size() - 1);
            std::vector<bool> signs(poly.a_indices.size(), true);
            std::mutex dummy_mutex;

            for (size_t b_idx = 0; b_idx < num_B; b_idx++) {
                size_t before = local_relations.size();
                sieve_polynomial(poly, kN, fb, params.sieve_half,
                               threshold, params.small_prime_cutoff,
                               lp_bound, lp_bound_sq,
                               local_relations, dummy_mutex, sieve_buf, exp_buf);

                // Incrementally count new relations by type
                for (size_t ri = before; ri < local_relations.size(); ri++) {
                    const auto& r = local_relations[ri];
                    if (r.large_prime == 0) local_full++;
                    else if (r.large_prime2 == 0) local_1lp++;
                    else local_2lp++;
                }

                size_t polys_done = atomic_polys.fetch_add(1, std::memory_order_relaxed) + 1;

                // Flush every 200 relations or 10 polys (frequent for fast early-stop)
                if (local_relations.size() > 200 || polys_done % 10 == 0) {
                    atomic_full.fetch_add(local_full, std::memory_order_relaxed);
                    atomic_1lp.fetch_add(local_1lp, std::memory_order_relaxed);
                    atomic_2lp.fetch_add(local_2lp, std::memory_order_relaxed);
                    local_full = local_1lp = local_2lp = 0;

                    {
                        std::lock_guard<std::mutex> lock(relations_mutex);
                        for (auto& r : local_relations)
                            all_relations.push_back(std::move(r));
                    }
                    local_relations.clear();

                    // Quick estimate without mutex — check every 10 polys
                    if (polys_done % 10 == 0) {
                        if (quick_estimate() >= safe_target) {
                            enough.store(true, std::memory_order_relaxed);
                            break;
                        }
                    }
                }

                if (enough.load(std::memory_order_relaxed)) break;
                if (elapsed() >= static_cast<double>(max_seconds)) break;

                // Gray code switch to next B
                if (b_idx + 1 < num_B) {
                    size_t change = 0;
                    size_t temp = b_idx + 1;
                    while ((temp & 1) == 0) { change++; temp >>= 1; }
                    if (change < signs.size()) {
                        bool add = !signs[change];
                        signs[change] = !signs[change];
                        next_poly_B(fb, params.sieve_half, poly, change, add);
                    }
                }
            }
        }

        // Final flush
        if (!local_relations.empty()) {
            std::lock_guard<std::mutex> lock(relations_mutex);
            for (auto& r : local_relations)
                all_relations.push_back(std::move(r));
        }
    };

    // Launch threads
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < num_threads; t++) {
        threads.emplace_back(sieve_worker, t);
    }
    for (auto& t : threads) t.join();
    num_polys = atomic_polys.load();

    if (verbose) {
        size_t full = 0, partial = 0;
        for (const auto& r : all_relations) {
            if (r.large_prime == 0) full++;
            else partial++;
        }
        fprintf(stderr, "[SIQS] Sieve done: %zu full + %zu partial in %zu polys (%.3fs)\n",
                full, partial, num_polys, elapsed());
    }

    // Both explicit shadow modes read the post-join raw corpus before legacy
    // merge mutation. Observe always continues. Prefer may return a fully
    // revalidated candidate only after its V2 pre-route decision is written,
    // flushed, and stream-error free on the caller's stderr stream.
    bool shadow_proof_observe_record_committed = false;
    const SIQSShadowProofOptions shadow_options{};
    const auto run_shadow_proof = [&]() {
        std::vector<uint32_t> factor_base_primes;
        factor_base_primes.reserve(fb.size());
        for (const FBPrime& prime : fb) {
            factor_base_primes.push_back(prime.p);
        }

        auto shadow_splitter = split_cofactor_64;
        return run_siqs_shadow_proof(
            std::span<const SIQSRelation>(all_relations.data(), all_relations.size()),
            std::span<const uint32_t>(factor_base_primes.data(), factor_base_primes.size()), kN, N,
            lp_bound, shadow_splitter, shadow_options);
    };
    if (shadow_proof_mode == SIQSShadowProofMode::observe) {
        const SIQSShadowProofObserveRecord shadow_record = observe_siqs_shadow_proof(
            all_relations.size(), fb.size(), lp_bound, shadow_options, run_shadow_proof);
        shadow_proof_observe_record_committed =
            emit_siqs_shadow_proof_observe_record(shadow_record);
    } else if (shadow_proof_mode == SIQSShadowProofMode::prefer) {
        try {
            SIQSShadowProofResult shadow_result = run_shadow_proof();
            SIQSShadowProofPreferDraft draft =
                evaluate_siqs_shadow_proof_prefer(shadow_result, N, num_polys);

            std::optional<SIQSResult> prepared_shadow_result;
            if (draft.decision == SIQSShadowProofPreferDecisionKind::shadow_candidate &&
                draft.accepted.has_value()) {
                SIQSResult candidate;
                candidate.factor1 = draft.accepted->factorization.factor;
                candidate.factor2 = draft.accepted->factorization.cofactor;
                candidate.time_seconds = 0.0;
                candidate.relations_found = draft.accepted->relations_found;
                candidate.polynomials_used = draft.accepted->polynomials_used;
                candidate.resolved_sieve_workers = num_threads;
                prepared_shadow_result.emplace(std::move(candidate));
            }

            const auto decision_wall_end = std::chrono::steady_clock::now();
            const auto decision_wall_count =
                std::chrono::duration_cast<std::chrono::nanoseconds>(decision_wall_end - start)
                    .count();
            const uint64_t decision_wall_ns =
                decision_wall_count > 0 && std::in_range<uint64_t>(decision_wall_count)
                    ? static_cast<uint64_t>(decision_wall_count)
                    : 0;
            SIQSShadowProofPreferDecision decision =
                finalize_siqs_shadow_proof_prefer(std::move(draft), decision_wall_ns);

            std::optional<SIQSResult> routed_shadow_result =
                siqs_factor_detail::commit_prefer_route(stderr, N, decision,
                                                        std::move(prepared_shadow_result));
            if (routed_shadow_result.has_value()) {
                return routed_shadow_result;
            }
        } catch (...) {
            // Explicit prefer never prevents the untouched legacy path.
        }
    }

    // Merge partials
    auto relations = merge_partials(all_relations, fb_size, verbose);

    if (verbose) {
        fprintf(stderr, "[SIQS] After merge: %zu usable relations (target=%zu, %.3fs)\n",
                relations.size(), fb_size, elapsed());
    }

    if (relations.size() <= fb_size) {
        if (verbose) {
            fprintf(stderr, "[SIQS] Not enough relations (%zu <= %zu), giving up\n",
                    relations.size(), fb_size);
        }
        return std::nullopt;
    }

    // Trim excess relations to reduce LA cost.
    // O(n²) Gaussian: halving rows gives ~4× speedup.
    // Keep fb_size + 100 relations (enough for ~64 dependencies).
    size_t max_rels = fb_size + 100;
    if (relations.size() > max_rels) {
        // Prefer full relations (more reliable) — they're at the front
        relations.resize(max_rels);
    }

    // Linear algebra
    if (verbose) {
        fprintf(stderr, "[SIQS] Starting linear algebra (%zux%zu)...\n",
                relations.size(), fb_size);
    }

    auto deps = solve_matrix(relations, fb_size);

    if (verbose) {
        fprintf(stderr, "[SIQS] Found %zu dependencies (%.3fs)\n",
                deps.size(), elapsed());
    }

    // Try dependencies — use ORIGINAL N for GCD (not kN)
    // Since kN | (X²-Y²), we have N | (X²-Y²), so gcd(X-Y, N) works directly.
    // Compute X,Y mod kN (for correct arithmetic), but gcd against N.
    auto result = try_extract_with_combos(kN, N, relations, deps, fb);
    if (result) {
        SIQSResult sr;
        sr.factor1 = std::move(result->first);
        sr.factor2 = std::move(result->second);
        sr.time_seconds = elapsed();
        sr.relations_found = relations.size();
        sr.polynomials_used = num_polys;
        sr.resolved_sieve_workers = num_threads;
        sr.shadow_proof_observe_record_committed = shadow_proof_observe_record_committed;

        if (verbose) {
            fprintf(stderr, "[SIQS] SUCCESS: %s * %s (%.3fs, %zu polys)\n",
                    sr.factor1.to_string().c_str(),
                    sr.factor2.to_string().c_str(),
                    sr.time_seconds, num_polys);
        }
        return sr;
    }

    if (verbose) {
        fprintf(stderr, "[SIQS] All dependencies + combos failed\n");
    }
    return std::nullopt;
}

} // namespace gnfs::siqs
