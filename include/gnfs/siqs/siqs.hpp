#pragma once

/// @file siqs.hpp
/// @brief Self-Initializing Quadratic Sieve (SIQS) — Contini 1997
///
/// Efficient factorization for 40-95 digit semiprimes.
/// Uses polynomial self-initialization for fast switching between
/// sieve polynomials, large-prime variation, and GF(2) linear algebra.

#include <gnfs/core/integer.hpp>
#include <gnfs/linalg/sparse_matrix.hpp>
#include <gnfs/linalg/gauss.hpp>
#include <gnfs/linalg/block_lanczos.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <unordered_map>
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
inline SIQSParams select_params(size_t digits) {
    // Each row: {fb_size, sieve_half, lp_mult, a_factors, sieve_error, small_cutoff}
    // sieve_error: only covers log approximation + prime powers; LP subtracted separately
    if (digits <= 20) return {50,     8192,    20, 2,  8,  5};
    if (digits <= 25) return {80,     16384,   20, 3,  8,  5};
    if (digits <= 30) return {150,    16384,   25, 3,  9,  10};
    if (digits <= 34) return {250,    16384,   25, 4,  10, 10};
    if (digits <= 39) return {500,    32768,   25, 5,  10, 15};
    if (digits <= 44) return {1000,   32768,   30, 5,  11, 20};
    if (digits <= 49) return {1800,   65536,   30, 6,  12, 25};
    if (digits <= 54) return {3500,   65536,   30, 7,  12, 30};
    if (digits <= 59) return {6000,   65536,   35, 7,  13, 30};
    if (digits <= 64) return {10000,  65536,   35, 8,  13, 40};
    if (digits <= 69) return {18000,  131072,  40, 8,  14, 50};
    if (digits <= 74) return {30000,  131072,  40, 9,  14, 60};
    if (digits <= 79) return {55000,  131072,  45, 9,  15, 70};
    if (digits <= 84) return {90000,  262144,  45, 10, 15, 80};
    if (digits <= 89) return {150000, 262144,  50, 10, 16, 90};
    if (digits <= 95) return {250000, 524288,  50, 11, 16, 100};
    return                     {400000, 524288,  60, 12, 17, 120};
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
// Factor base
// ================================================================

struct FBPrime {
    uint32_t p;
    uint32_t sqrt_n;    // sqrt(N) mod p
    uint8_t  logp;      // floor(log2(p))
};

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
};

/// Choose A = product of num_factors primes from factor base
/// Target: A ≈ sqrt(2N) / M
inline void choose_A(const Integer& N, uint32_t M,
                     uint32_t num_factors,
                     const std::vector<FBPrime>& fb,
                     std::mt19937& rng,
                     std::vector<uint32_t>& a_indices, Integer& A) {
    // Target A value
    Integer two_n = N * Integer(2);
    Integer target_a = core::sqrt(two_n) / Integer(static_cast<uint64_t>(M));
    if (target_a < Integer(1)) target_a = Integer(1);

    // Pick primes from middle of FB to form A close to target
    // Strategy: find the prime whose individual value would give
    // target^(1/num_factors), then pick around that range
    double log_target = mpz_sizeinbase(target_a.get_mpz(), 2);
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
    A = Integer(1);
    for (uint32_t idx : a_indices) {
        A = A * Integer(static_cast<uint64_t>(fb[idx].p));
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

        // Compute A/qi
        Integer A_div_qi = poly.A / Integer(static_cast<uint64_t>(qi));

        // (A/qi)^{-1} mod qi
        uint32_t a_div_qi_mod = static_cast<uint32_t>(
            mpz_fdiv_ui(A_div_qi.get_mpz(), qi));
        uint32_t inv = mod_inv32(a_div_qi_mod, qi);

        // B_i = ti * inv * (A/qi) — but as Integer
        uint32_t coeff = mod_mul32(ti, inv, qi);
        poly.B_parts[i] = A_div_qi * Integer(static_cast<uint64_t>(coeff));
    }

    // B = sum(B_i) mod A, adjusted to |B| <= A/2
    poly.B = Integer(0);
    for (size_t i = 0; i < s; i++) {
        poly.B = poly.B + poly.B_parts[i];
    }
    // Reduce B mod A
    poly.B = poly.B % poly.A;
    // Center: if B > A/2, B = B - A
    Integer half_A = poly.A / Integer(2);
    if (poly.B > half_A) {
        poly.B = poly.B - poly.A;
    }

    // Verify: B^2 ≡ N (mod A)
    // (skip in release for performance)

    // Precompute A^{-1} mod p for each FB prime
    poly.a_inv_mod_p.resize(fb.size());
    for (size_t i = 1; i < fb.size(); i++) {
        uint32_t p = fb[i].p;
        uint32_t a_mod_p = static_cast<uint32_t>(
            mpz_fdiv_ui(poly.A.get_mpz(), p));
        if (a_mod_p == 0) {
            poly.a_inv_mod_p[i] = 0; // A prime divides A — skip
        } else {
            poly.a_inv_mod_p[i] = mod_inv32(a_mod_p, p);
        }
    }

    // Compute sieve start positions for each FB prime
    // Q(x) = (Ax + B)^2 - N ≡ 0 (mod p)
    // Ax + B ≡ ±sqrt(N) (mod p)
    // x ≡ (±sqrt(N) - B) * A^{-1} (mod p)
    poly.solns.resize(fb.size());
    uint32_t M = sieve_half;

    for (size_t i = 1; i < fb.size(); i++) {
        uint32_t p = fb[i].p;
        uint32_t ainv = poly.a_inv_mod_p[i];

        if (ainv == 0) {
            // This prime divides A — special handling
            // Q(x)/A factors, one root only
            // (Ax+B)^2 - N = A*(Ax^2 + 2Bx + (B^2-N)/A)
            // For p | A: Ax^2 + 2Bx ≡ 2Bx (mod p), so x ≡ -(B^2-N)/(2BA) mod p
            // Simpler: just skip. The A primes are already accounted for.
            poly.solns[i] = {UINT32_MAX, UINT32_MAX};
            continue;
        }

        uint32_t b_mod_p = static_cast<uint32_t>(
            mpz_fdiv_ui(poly.B.get_mpz(), p));
        // Handle negative B: B might be negative, need proper mod
        if (poly.B < Integer(0)) {
            Integer abs_b = Integer(0) - poly.B;
            uint32_t abs_b_mod = static_cast<uint32_t>(
                mpz_fdiv_ui(abs_b.get_mpz(), p));
            b_mod_p = (p - abs_b_mod) % p;
        }

        uint32_t sq = fb[i].sqrt_n;

        // soln1 = (sq - B) * A^{-1} mod p
        uint32_t diff1 = (sq >= b_mod_p) ? sq - b_mod_p : sq + p - b_mod_p;
        uint32_t x1 = mod_mul32(diff1, ainv, p);

        // soln2 = (-sq - B) * A^{-1} mod p = (p - sq - B) * A^{-1} mod p
        uint32_t neg_sq = p - sq;
        uint32_t diff2 = (neg_sq >= b_mod_p) ? neg_sq - b_mod_p : neg_sq + p - b_mod_p;
        uint32_t x2 = mod_mul32(diff2, ainv, p);

        // Adjust to sieve array coordinates: sieve[x + M] corresponds to x
        // We sieve x in [0, 2M), so starting position = x_val mod p
        // But x in the formula is the polynomial variable, ranging over [-M, M)
        // In array coords: pos = x + M, so x = pos - M
        // x ≡ x1 (mod p) → pos ≡ x1 + M (mod p)
        uint32_t s1 = (x1 + M) % p;
        uint32_t s2 = (x2 + M) % p;

        poly.solns[i] = {s1, s2};
    }
}

/// Self-initialization: switch to next B value using Gray code
/// gray_bit: which B_part to flip (0-indexed)
inline void next_poly_B(const std::vector<FBPrime>& fb,
                        uint32_t sieve_half,
                        SIQSPoly& poly,
                        size_t gray_bit, bool add) {
    // Update B: B_new = B_old ± 2 * B_parts[gray_bit]
    Integer two_B = poly.B_parts[gray_bit] * Integer(2);
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

        // Delta = 2 * B_part * A^{-1} mod p
        uint32_t bp_mod = static_cast<uint32_t>(
            mpz_fdiv_ui(poly.B_parts[gray_bit].get_mpz(), p));
        uint32_t delta = mod_mul32(mod_mul32(2, bp_mod, p), ainv, p);

        if (add) {
            // B increased → x = (sq - B)*Ainv decreased
            // soln1 -= delta, soln2 -= delta
            poly.solns[i].soln1 = (poly.solns[i].soln1 + p - delta) % p;
            poly.solns[i].soln2 = (poly.solns[i].soln2 + p + delta) % p;
        } else {
            poly.solns[i].soln1 = (poly.solns[i].soln1 + delta) % p;
            poly.solns[i].soln2 = (poly.solns[i].soln2 + p - delta) % p;
        }
    }
}

// ================================================================
// Relation
// ================================================================

struct SIQSRelation {
    Integer value;                     // Ax + B (the "square root" side)
    std::vector<uint32_t> fb_indices;  // which FB primes divide Q(x) (with multiplicity)
    std::vector<uint8_t>  exponents;   // exponent of each FB prime (for sqrt computation)
    uint64_t large_prime;              // 0 if fully smooth
    std::vector<uint64_t> merge_lps;   // LP values from merged partials (for Y computation)
    bool negative;                     // Q(x) < 0?
};

// ================================================================
// Sieve kernel
// ================================================================

/// Sieve one polynomial and collect smooth relations
inline void sieve_polynomial(
    const SIQSPoly& poly,
    const Integer& N,
    const std::vector<FBPrime>& fb,
    uint32_t sieve_half,
    uint8_t threshold,
    uint32_t small_cutoff,
    uint64_t lp_bound,
    std::vector<SIQSRelation>& out_relations,
    std::mutex& relations_mutex)
{
    uint32_t M = sieve_half;
    uint32_t sieve_size = 2 * M;

    // Allocate sieve array
    std::vector<uint8_t> sieve(sieve_size, 0);

    // Phase 1: Sieve — accumulate log(p) for each FB prime
    for (size_t i = 1; i < fb.size(); i++) {
        uint32_t p = fb[i].p;
        if (p < small_cutoff) continue;  // skip tiny primes (accounted in threshold)

        uint32_t s1 = poly.solns[i].soln1;
        uint32_t s2 = poly.solns[i].soln2;

        if (s1 == UINT32_MAX) continue; // A-prime, skip

        uint8_t logp = fb[i].logp;

        // Sieve with soln1
        for (uint32_t pos = s1; pos < sieve_size; pos += p) {
            sieve[pos] += logp;
        }
        // Sieve with soln2 (if different from soln1)
        if (s2 != s1) {
            for (uint32_t pos = s2; pos < sieve_size; pos += p) {
                sieve[pos] += logp;
            }
        }
    }

    // Phase 2: Identify candidates and trial divide
    for (uint32_t pos = 0; pos < sieve_size; pos++) {
        if (sieve[pos] < threshold) continue;

        // Candidate found — compute Q(x) and trial divide
        int64_t x = static_cast<int64_t>(pos) - static_cast<int64_t>(M);

        // value = Ax + B
        Integer ax = poly.A * Integer(static_cast<uint64_t>(std::abs(x)));
        Integer value = (x >= 0) ? (ax + poly.B) : (poly.B - ax);

        // Q(x) = value^2 - N
        Integer Q = value * value - N;
        bool negative = (Q < Integer(0));
        if (negative) Q = Integer(0) - Q;

        // Trial divide Q by factor base primes
        SIQSRelation rel;
        rel.value = std::move(value);
        rel.negative = negative;
        rel.large_prime = 0;
        rel.exponents.assign(fb.size(), 0);

        // Divide out A primes first (they always divide Q)
        mpz_t q_mpz;
        mpz_init(q_mpz);
        mpz_set(q_mpz, Q.get_mpz());

        for (uint32_t ai : poly.a_indices) {
            uint32_t p = fb[ai].p;
            while (mpz_divisible_ui_p(q_mpz, p)) {
                mpz_divexact_ui(q_mpz, q_mpz, p);
                rel.exponents[ai]++;
            }
        }

        // Divide by all FB primes
        for (size_t i = 1; i < fb.size(); i++) {
            uint32_t p = fb[i].p;
            while (mpz_divisible_ui_p(q_mpz, p)) {
                mpz_divexact_ui(q_mpz, q_mpz, p);
                rel.exponents[i]++;
            }
        }

        // Check remaining cofactor
        bool accept = false;
        if (mpz_cmp_ui(q_mpz, 1) == 0) {
            // Fully smooth
            accept = true;
        } else if (mpz_fits_ulong_p(q_mpz)) {
            uint64_t cofac = mpz_get_ui(q_mpz);
            if (cofac <= lp_bound && cofac > 1) {
                // Partial: one large prime
                rel.large_prime = cofac;
                accept = true;
            }
        }

        mpz_clear(q_mpz);

        if (accept) {
            // Build fb_indices list
            for (size_t i = 0; i < fb.size(); i++) {
                if (rel.exponents[i] > 0) {
                    rel.fb_indices.push_back(static_cast<uint32_t>(i));
                }
            }

            std::lock_guard<std::mutex> lock(relations_mutex);
            out_relations.push_back(std::move(rel));
        }
    }
}

// ================================================================
// Large prime merging
// ================================================================

/// Merge partial relations sharing the same large prime
/// Returns merged full relations + original full relations
inline std::vector<SIQSRelation> merge_partials(
    std::vector<SIQSRelation>& relations, size_t fb_size)
{
    std::vector<SIQSRelation> full;
    std::unordered_map<uint64_t, size_t> lp_map; // LP → index in partials
    std::vector<SIQSRelation> partials;

    for (auto& rel : relations) {
        if (rel.large_prime == 0) {
            full.push_back(std::move(rel));
        } else {
            auto it = lp_map.find(rel.large_prime);
            if (it != lp_map.end()) {
                // Found match — merge the two partials
                auto& other = partials[it->second];

                SIQSRelation merged;
                // value = val1 * val2 (the product gives LP^2 which is a square)
                merged.value = rel.value * other.value;
                merged.negative = (rel.negative != other.negative);
                merged.large_prime = 0; // now fully smooth
                merged.exponents.resize(fb_size, 0);

                // Add exponents from both
                for (size_t i = 0; i < fb_size; i++) {
                    merged.exponents[i] = rel.exponents[i] + other.exponents[i];
                }

                // LP appears twice → even exponent (doesn't affect matrix parity)
                // But we MUST track it for Y computation: Y includes LP^1
                merged.merge_lps.push_back(rel.large_prime);

                merged.fb_indices.clear();
                for (size_t i = 0; i < fb_size; i++) {
                    if (merged.exponents[i] > 0)
                        merged.fb_indices.push_back(static_cast<uint32_t>(i));
                }

                full.push_back(std::move(merged));
                lp_map.erase(it);
            } else {
                lp_map[rel.large_prime] = partials.size();
                partials.push_back(std::move(rel));
            }
        }
    }

    return full;
}

// ================================================================
// Matrix construction and linear algebra
// ================================================================

/// Build GF(2) matrix from relations and find null space
inline std::vector<std::vector<size_t>> solve_matrix(
    const std::vector<SIQSRelation>& relations,
    size_t fb_size)
{
    size_t nrows = relations.size();
    size_t ncols = fb_size; // columns = FB primes (including sign at index 0)

    linalg::SparseMatrix matrix(nrows, ncols);

    for (size_t r = 0; r < nrows; r++) {
        const auto& rel = relations[r];
        // Sign column
        if (rel.negative) {
            matrix.set(r, 0);
        }
        // FB prime exponents mod 2
        for (size_t c = 1; c < fb_size && c < rel.exponents.size(); c++) {
            if (rel.exponents[c] & 1) {
                matrix.set(r, c);
            }
        }
    }

    // Choose solver based on matrix size
    std::vector<std::vector<size_t>> deps;

    if (ncols <= 60000) {
        // Gaussian elimination (fast for moderate sizes)
        linalg::GaussianConfig cfg;
        cfg.compute_null_space = true;
        cfg.max_null_vectors = 64;

        // Transpose: we need column dependencies but Gaussian finds row deps
        // Actually, we need to find vectors v such that M^T * v = 0
        // Equivalently, find left null space of M
        linalg::SparseMatrix transposed = matrix.transpose();
        linalg::GaussianEliminator gauss(cfg);
        auto result = gauss.eliminate(transposed);

        for (auto& null_vec : result.null_space) {
            std::vector<size_t> dep;
            for (size_t i = 0; i < nrows; i++) {
                if (null_vec.test(i)) {
                    dep.push_back(i);
                }
            }
            if (!dep.empty()) {
                deps.push_back(std::move(dep));
            }
        }
    } else {
        // Block Lanczos for large matrices
        linalg::BlockLanczos bl;
        auto bl_deps = bl.find_dependencies(matrix, 64);

        for (auto& bv : bl_deps) {
            std::vector<size_t> dep;
            for (size_t i = 0; i < nrows; i++) {
                if (i < bv.size() && bv[i]) {
                    dep.push_back(i);
                }
            }
            if (!dep.empty()) {
                deps.push_back(std::move(dep));
            }
        }
    }

    return deps;
}

// ================================================================
// Factor extraction: X^2 ≡ Y^2 (mod N) → gcd(X-Y, N)
// ================================================================

inline std::optional<std::pair<Integer, Integer>> try_extract(
    const Integer& N,
    const std::vector<SIQSRelation>& relations,
    const std::vector<size_t>& dep,
    const std::vector<FBPrime>& fb)
{
    // Verify exponents are all even (matrix correctness check)
    std::vector<uint32_t> total_exp(fb.size(), 0);
    for (size_t idx : dep) {
        const auto& rel = relations[idx];
        for (size_t i = 0; i < fb.size() && i < rel.exponents.size(); i++) {
            total_exp[i] += rel.exponents[i];
        }
    }
    for (size_t i = 0; i < fb.size(); i++) {
        if (total_exp[i] & 1) return std::nullopt; // parity error → skip
    }

    // Compute X = product of value_i mod N
    Integer X(1);
    for (size_t idx : dep) {
        mpz_mul(X.get_mpz(), X.get_mpz(), relations[idx].value.get_mpz());
        mpz_mod(X.get_mpz(), X.get_mpz(), N.get_mpz());
    }

    // Compute Y = product of p_i^{exp/2} * LP_products mod N
    Integer Y(1);
    for (size_t i = 1; i < fb.size(); i++) {
        uint32_t half_exp = total_exp[i] / 2;
        if (half_exp > 0) {
            Integer pe;
            mpz_set_ui(pe.get_mpz(), fb[i].p);
            mpz_powm_ui(pe.get_mpz(), pe.get_mpz(), half_exp, N.get_mpz());
            mpz_mul(Y.get_mpz(), Y.get_mpz(), pe.get_mpz());
            mpz_mod(Y.get_mpz(), Y.get_mpz(), N.get_mpz());
        }
    }

    // Include LP factors from merged relations
    for (size_t idx : dep) {
        for (uint64_t lp : relations[idx].merge_lps) {
            Integer lp_int(lp);
            mpz_mul(Y.get_mpz(), Y.get_mpz(), lp_int.get_mpz());
            mpz_mod(Y.get_mpz(), Y.get_mpz(), N.get_mpz());
        }
    }

    // Try gcd(X - Y, N) and gcd(X + Y, N)
    Integer diff;
    mpz_sub(diff.get_mpz(), X.get_mpz(), Y.get_mpz());
    Integer g = core::gcd(diff, N);
    if (g > Integer(1) && g < N) {
        return std::make_pair(g.clone(), N / g);
    }
    mpz_add(diff.get_mpz(), X.get_mpz(), Y.get_mpz());
    g = core::gcd(diff, N);
    if (g > Integer(1) && g < N) {
        return std::make_pair(g.clone(), N / g);
    }
    return std::nullopt;
}

/// Try random XOR combinations of dependency vectors to increase success probability
inline std::optional<std::pair<Integer, Integer>> try_extract_with_combos(
    const Integer& N,
    const std::vector<SIQSRelation>& relations,
    const std::vector<std::vector<size_t>>& deps,
    const std::vector<FBPrime>& fb)
{
    // First try each dependency individually
    for (const auto& dep : deps) {
        auto result = try_extract(N, relations, dep, fb);
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

        auto result = try_extract(N, relations, combined, fb);
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
};

inline std::optional<SIQSResult> factor(
    const Integer& N,
    size_t max_seconds = 3600,
    bool verbose = true)
{
    auto start = std::chrono::steady_clock::now();
    auto elapsed = [&]() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
    };

    size_t digits = N.to_string().size();
    auto params = select_params(digits);

    if (verbose) {
        fprintf(stderr, "[SIQS] N=%zu digits, FB=%u, M=%u, A_factors=%u\n",
                digits, params.fb_size, params.sieve_half, params.num_a_factors);
    }

    // Build factor base
    auto fb = build_factor_base(N, params.fb_size);
    size_t fb_size = fb.size(); // includes sign at index 0

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
    size_t n_bits = mpz_sizeinbase(N.get_mpz(), 2);
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

    if (verbose) {
        fprintf(stderr, "[SIQS] log_Qmax=%.1f, lp_bits=%.1f, sieve_err=%u, "
                "small=%.1f, threshold=%u, LP_bound=%llu\n",
                log_Qmax_d, lp_bits, params.sieve_error,
                small_contrib, threshold, (unsigned long long)lp_bound);
    }

    // Target: enough usable relations to exceed FB size
    size_t target_usable = fb_size + 50;

    // Sieve loop — collect until estimated usable exceeds target
    // Estimate: merge rate ~10-20% of partials. So total needed ≈ target * 5
    std::vector<SIQSRelation> all_relations;
    all_relations.reserve(target_usable * 10);
    std::mutex relations_mutex;

    size_t num_polys = 0;
    std::mt19937 rng(42);

    auto estimate_usable = [&]() -> size_t {
        size_t full = 0, partial = 0;
        std::unordered_map<uint64_t, size_t> lp_count;
        for (const auto& r : all_relations) {
            if (r.large_prime == 0) full++;
            else {
                partial++;
                lp_count[r.large_prime]++;
            }
        }
        size_t merged = 0;
        for (auto& [lp, cnt] : lp_count) {
            merged += cnt / 2; // pairs
        }
        return full + merged;
    };

    // Multi-threaded sieve: each thread processes its own A values
    unsigned num_threads = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<size_t> atomic_polys{0};
    std::atomic<bool> enough{false};

    auto sieve_worker = [&](unsigned thread_id) {
        std::mt19937 local_rng(42 + thread_id * 1000);
        std::vector<SIQSRelation> local_relations;
        local_relations.reserve(target_usable);

        while (!enough.load(std::memory_order_relaxed) &&
               elapsed() < static_cast<double>(max_seconds))
        {
            // Choose new A (each thread picks independently)
            SIQSPoly poly;
            choose_A(N, params.sieve_half, params.num_a_factors, fb, local_rng,
                     poly.a_indices, poly.A);

            init_poly(N, fb, params.sieve_half, poly);

            size_t num_B = size_t(1) << (poly.a_indices.size() - 1);
            std::vector<bool> signs(poly.a_indices.size(), true);
            std::mutex dummy_mutex; // local, no contention

            for (size_t b_idx = 0; b_idx < num_B; b_idx++) {
                sieve_polynomial(poly, N, fb, params.sieve_half,
                               threshold, params.small_prime_cutoff,
                               lp_bound, local_relations, dummy_mutex);

                size_t polys_done = atomic_polys.fetch_add(1, std::memory_order_relaxed) + 1;

                // Periodically flush local relations to shared pool
                if (local_relations.size() > 200 || polys_done % 100 == 0) {
                    std::lock_guard<std::mutex> lock(relations_mutex);
                    for (auto& r : local_relations)
                        all_relations.push_back(std::move(r));
                    local_relations.clear();

                    // Check if enough
                    if (polys_done % 200 == 0) {
                        size_t est = estimate_usable();
                        if (est >= target_usable) {
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

    // Merge partials
    auto relations = merge_partials(all_relations, fb_size);

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

    // Try dependencies (individual + random XOR combos)
    auto result = try_extract_with_combos(N, relations, deps, fb);
    if (result) {
        SIQSResult sr;
        sr.factor1 = std::move(result->first);
        sr.factor2 = std::move(result->second);
        sr.time_seconds = elapsed();
        sr.relations_found = relations.size();
        sr.polynomials_used = num_polys;

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
