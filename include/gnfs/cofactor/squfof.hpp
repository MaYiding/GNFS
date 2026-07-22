#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gnfs::cofactor {

/// SQUFOF (SQUare FOrm Factorization) — Shanks 1975
///
/// Factors composites up to ~2^62 in O(N^{1/4}) time with tiny constant.
/// Reference: Jason Gower & Samuel Wagstaff Jr., "Square Form Factorization",
///            Mathematics of Computation 77(261), 2008.
///
/// Algorithm operates on the infrastructure of binary quadratic forms
/// with discriminant Δ = 4kN (for multiplier k). Uses continued fraction
/// expansion of √(kN) to find a proper square form, then extract factor.
class SQUFOF {
public:
    /// Factor n using SQUFOF. Returns a non-trivial factor, or 1 on failure.
    /// n must be > 1 and composite. Works for n up to ~2^62.
    [[nodiscard]] static uint64_t factor(uint64_t n, uint32_t max_iterations = 0) {
        if (n <= 1) return 1;
        if (n % 2 == 0) return 2;
        if (is_square(n)) return isqrt(n);

        // Try multipliers to avoid short-period cases. Keep k=1 first for the
        // common path, then try k=15: Gower-Wagstaff Table 5 gives it the best
        // expected work among the non-trivial multipliers in this set. Preserve
        // the prior order for the remaining fallbacks.
        static constexpr uint64_t multipliers[] = {
            1, 3*5, 3, 5, 7, 11, 3*7, 3*11, 5*7, 5*11, 7*11
        };

        for (uint64_t k : multipliers) {
            if (k > 1 && n > UINT64_MAX / k) continue;
            uint64_t D = k * n;
            if (D < 2) continue;
            // D ≡ 2,3 mod 4 时 SQUFOF 周期偏长;对 k=1 时如果 N ≢ 1 mod 4
            // 改用 D=4N 既避免 D≡2,3 又把 k 的搜索空间限制在奇 k。
            if (k == 1 && (n % 4) != 1) {
                if (n > UINT64_MAX / 4) continue;
                D = 4 * n;
            }

            uint64_t result = squfof_core(D, max_iterations);
            if (result > 1 && result < D) {
                uint64_t g = gcd(result, n);
                if (g > 1 && g < n) return g;
            }
        }
        return 1;
    }

private:
    static uint64_t gcd(uint64_t a, uint64_t b) {
        while (b) { uint64_t t = b; b = a % b; a = t; }
        return a;
    }

    static uint64_t isqrt(uint64_t n) {
        if (n == 0) return 0;
        uint64_t x = static_cast<uint64_t>(std::sqrt(static_cast<double>(n)));
        // double may round sqrt(UINT64_MAX - epsilon) to 2^32.  Clamp to
        // floor(sqrt(UINT64_MAX)) before using multiplication for correction.
        x = std::min<uint64_t>(x, UINT32_MAX);
        while (x > 0 && x * x > n) --x;
        while (x < UINT32_MAX && (x + 1) * (x + 1) <= n) ++x;
        return x;
    }

    static bool is_square(uint64_t n) {
        uint64_t s = isqrt(n);
        return s * s == n;
    }

    /// Core SQUFOF: factor D using continued fraction of √D.
    /// Returns a non-trivial factor of D, or 1 on failure.
    ///
    /// Uses the "reduced form" approach:
    ///   P_0 = floor(√D), Q_0 = 1, Q_1 = D - P_0²
    ///   b_i = floor((P_i + floor(√D)) / Q_{i+1})
    ///   P_{i+1} = b_i * Q_{i+1} - P_i
    ///   Q_{i+2} = Q_i + b_i * (P_i - P_{i+1})
    ///
    /// Look for Q_i that is a perfect square at even steps (i ≥ 2).
    /// Then do an inverse walk to extract the factor.
    [[nodiscard]] static uint64_t squfof_core(uint64_t D, uint32_t max_iter) {
        uint64_t sqrtD = isqrt(D);
        if (sqrtD * sqrtD == D) return sqrtD;

        if (max_iter == 0) {
            max_iter = static_cast<uint32_t>(std::min(
                4.0 * std::pow(static_cast<double>(D), 0.25) + 200,
                static_cast<double>(1u << 22)));
        }

        // Initialization
        uint64_t Pprev = sqrtD;
        uint64_t Qprev = 1;
        uint64_t Qcurr = D - sqrtD * sqrtD;
        if (Qcurr == 0) return sqrtD;

        // Forward cycle
        for (uint32_t i = 0; i < max_iter; ++i) {
            uint64_t b = (sqrtD + Pprev) / Qcurr;
            uint64_t Pnew = b * Qcurr - Pprev;
            uint64_t Qnew = Qprev + b * (Pprev - Pnew);

            // Check for perfect square Q at even iteration (i odd = step i+1 even since i starts 0)
            // Gower-Wagstaff: check Q_{i+1} at odd i (0-indexed), i.e., even steps
            if ((i & 1) == 1) {
                uint64_t sq = isqrt(Qcurr);
                if (sq > 0 && sq * sq == Qcurr) {
                    // Found proper square form. Do inverse walk.
                    uint64_t f = inverse_walk(D, sqrtD, Pprev, sq);
                    if (f > 1 && f < D) return f;
                }
            }

            Pprev = Pnew;
            Qprev = Qcurr;
            Qcurr = Qnew;

            if (Qcurr == 0) break;
        }

        return 1;
    }

    /// Inverse walk from a square form to extract a factor.
    /// Given that Q = sq², start a new CF expansion from the reduced form
    /// and walk until P doesn't change (P_{i+1} == P_i).
    static uint64_t inverse_walk(uint64_t D, uint64_t sqrtD, uint64_t P_at_sq, uint64_t sq) {
        // Start inverse walk
        uint64_t b = (sqrtD + P_at_sq) / sq;
        uint64_t P = b * sq - P_at_sq;
        uint64_t Qprev = sq;
        uint64_t Qcurr = (D - P * P) / Qprev;

        if (Qcurr == 0) return gcd(D, P);

        uint32_t max_rev = 1u << 20;
        for (uint32_t i = 0; i < max_rev; ++i) {
            b = (sqrtD + P) / Qcurr;
            uint64_t Pnew = b * Qcurr - P;

            if (Pnew == P) {
                // Ambiguous form found
                return gcd(D, P);
            }

            uint64_t Qnew = Qprev + b * (P - Pnew);
            Qprev = Qcurr;
            P = Pnew;
            Qcurr = Qnew;

            if (Qcurr == 0) return gcd(D, P);
        }

        return 1;
    }
};

} // namespace gnfs::cofactor
