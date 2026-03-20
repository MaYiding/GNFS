#pragma once

#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"
#include "../sqrt/modular_poly.hpp"

#include <vector>
#include <cstdint>
#include <array>

namespace gnfs {
namespace linalg {

using core::Integer;
using core::PolynomialContext;
using sqrt::ModularPoly;

/// Schirokauer map configuration
struct SchirokaurConfig {
    std::vector<uint32_t> primes = {2};  // Primes for Schirokauer maps (usually just 2)
    uint32_t exponent_k = 3;              // Exponent k (use ℓ^k), larger = more accurate
    bool verbose = false;
};

// ============================================================================
// Fast fixed-size polynomial arithmetic for Schirokauer maps
// Optimized for degree <= 8 polynomials with small moduli
// ============================================================================
class FastPoly {
public:
    static constexpr size_t MAX_DEGREE = 8;
    std::array<uint64_t, MAX_DEGREE + 1> coeffs{};
    uint32_t deg = 0;

    FastPoly() = default;

    explicit FastPoly(uint64_t c0) : deg(0) {
        coeffs[0] = c0;
    }

    FastPoly(uint64_t c0, uint64_t c1) : deg(1) {
        coeffs[0] = c0;
        coeffs[1] = c1;
    }

    [[nodiscard]] uint64_t coeff(size_t i) const noexcept {
        return i <= deg ? coeffs[i] : 0;
    }

    // Multiply two polynomials mod m (no reduction by f yet)
    [[nodiscard]] static FastPoly mul_raw(const FastPoly& a, const FastPoly& b, uint64_t m) {
        FastPoly result;
        result.deg = a.deg + b.deg;

        for (uint32_t i = 0; i <= a.deg; ++i) {
            if (a.coeffs[i] == 0) continue;
            for (uint32_t j = 0; j <= b.deg; ++j) {
                if (b.coeffs[j] == 0) continue;
                __uint128_t prod = static_cast<__uint128_t>(a.coeffs[i]) * b.coeffs[j];
                result.coeffs[i + j] = (result.coeffs[i + j] + static_cast<uint64_t>(prod % m)) % m;
            }
        }

        // Normalize degree
        while (result.deg > 0 && result.coeffs[result.deg] == 0) {
            result.deg--;
        }

        return result;
    }

    // Reduce polynomial mod f(x) and mod m
    // f is assumed monic with degree f_deg
    static void reduce_inplace(FastPoly& a, const uint64_t* f, uint32_t f_deg, uint64_t m) {
        while (a.deg >= f_deg) {
            uint64_t lead = a.coeffs[a.deg];
            if (lead == 0) {
                if (a.deg > 0) a.deg--;
                continue;
            }

            // Subtract lead * x^(deg-f_deg) * f
            for (uint32_t i = 0; i < f_deg; ++i) {
                __uint128_t term = static_cast<__uint128_t>(lead) * f[i];
                uint64_t t = static_cast<uint64_t>(term % m);
                uint32_t idx = a.deg - f_deg + i;
                if (a.coeffs[idx] >= t) {
                    a.coeffs[idx] -= t;
                } else {
                    a.coeffs[idx] = m - (t - a.coeffs[idx]);
                }
            }
            a.coeffs[a.deg] = 0;
            if (a.deg > 0) a.deg--;
        }

        // Final normalize
        while (a.deg > 0 && a.coeffs[a.deg] == 0) {
            a.deg--;
        }
    }

    // Multiply and reduce: result = a * b mod (f, m)
    [[nodiscard]] static FastPoly mul(const FastPoly& a, const FastPoly& b,
                                       const uint64_t* f, uint32_t f_deg, uint64_t m) {
        auto result = mul_raw(a, b, m);
        reduce_inplace(result, f, f_deg, m);
        return result;
    }

    // Power mod (f, m) using binary exponentiation with uint64_t exponent
    [[nodiscard]] static FastPoly power(const FastPoly& base, uint64_t exp,
                                         const uint64_t* f, uint32_t f_deg, uint64_t m) {
        if (exp == 0) return FastPoly(1);

        FastPoly result(1);
        FastPoly b = base;

        while (exp > 0) {
            if (exp & 1) {
                result = mul(result, b, f, f_deg, m);
            }
            b = mul(b, b, f, f_deg, m);
            exp >>= 1;
        }

        return result;
    }
};

/// Schirokauer map computation for GNFS
///
/// Schirokauer maps provide additional constraints to ensure the algebraic
/// product is a square in the number field. For each prime ℓ, we get deg(f)
/// additional columns in the matrix.
///
/// For an element γ = a - bα in the number field K = Q(α):
///   λ_ℓ(γ) = (γ^(ℓ^(k-1)(ℓ-1)) - 1) / ℓ^(k-1) mod ℓ
///
/// For a product Πγ_i to be an ℓ-th power, we need Σλ_ℓ(γ_i) ≡ 0 mod ℓ
class SchirokaurMap {
public:
    using Config = SchirokaurConfig;

    explicit SchirokaurMap(const PolynomialContext& ctx, const Config& config = Config{})
        : ctx_(ctx), config_(config), degree_(ctx.degree()) {

        // Pre-compute values for each prime
        for (uint32_t p : config_.primes) {
            precompute_for_prime(p);
        }
    }

    /// Get total number of Schirokauer columns
    [[nodiscard]] size_t num_columns() const noexcept {
        return config_.primes.size() * degree_;
    }

    /// Get the primes used
    [[nodiscard]] const std::vector<uint32_t>& primes() const noexcept {
        return config_.primes;
    }

    /// Compute Schirokauer map for element (a - bα)
    /// Returns a vector of deg(f) values in [0, ℓ) for each prime ℓ
    [[nodiscard]] std::vector<std::vector<uint32_t>> compute(int64_t a, uint64_t b) const {
        std::vector<std::vector<uint32_t>> result;
        result.reserve(config_.primes.size());

        for (size_t i = 0; i < config_.primes.size(); ++i) {
            result.push_back(compute_for_prime_fast(a, b, i));
        }

        return result;
    }

    /// Fast computation for a single prime using fixed-size arrays
    [[nodiscard]] std::vector<uint32_t> compute_for_prime_fast(
            int64_t a, uint64_t b, size_t prime_idx) const {

        if (prime_idx >= config_.primes.size()) {
            return std::vector<uint32_t>(degree_, 0);
        }

        const auto& info = prime_info_[prime_idx];

        if (!info.is_split) {
            // Standard case: f irreducible mod ℓ
            return compute_unsplit(a, b, info);
        } else {
            // Split case: f = ∏ fᵢ mod ℓ
            return compute_split(a, b, info);
        }
    }

    // Info for a single factor of f mod ℓ
    struct FactorInfo {
        uint32_t degree;
        uint64_t exponent;
        std::array<uint64_t, FastPoly::MAX_DEGREE + 1> f_mod;
    };

    // Pre-computed values for each prime
    struct PrimeInfo {
        uint32_t ell;
        uint64_t ell_k;
        uint64_t ell_k_minus_1;
        uint64_t exponent;
        std::array<uint64_t, FastPoly::MAX_DEGREE + 1> f_mod;
        bool is_split = false;
        std::vector<FactorInfo> factors;
    };

    std::vector<PrimeInfo> prime_info_;

    /// Standard Schirokauer (f irreducible mod ℓ)
    [[nodiscard]] std::vector<uint32_t> compute_unsplit(
            int64_t a, uint64_t b, const PrimeInfo& info) const {

        int64_t a_mod = a % static_cast<int64_t>(info.ell_k);
        if (a_mod < 0) a_mod += static_cast<int64_t>(info.ell_k);
        uint64_t b_mod = b % info.ell_k;
        uint64_t neg_b = (info.ell_k - b_mod) % info.ell_k;

        FastPoly g(static_cast<uint64_t>(a_mod), neg_b);
        auto g_pow = FastPoly::power(g, info.exponent, info.f_mod.data(), degree_, info.ell_k);

        std::vector<uint32_t> result(degree_, 0);
        for (uint32_t i = 0; i < degree_; ++i) {
            uint64_t coeff = g_pow.coeff(i);
            if (i == 0) coeff = (coeff >= 1) ? (coeff - 1) : (info.ell_k - 1);
            result[i] = static_cast<uint32_t>((coeff / info.ell) % info.ell);
        }
        return result;
    }

    /// Split Schirokauer (f reducible mod ℓ)
    /// For each irreducible factor fᵢ of degree dᵢ:
    ///   Reduce γ = (a - bα) mod (fᵢ, ℓ^k)
    ///   Compute γ^(ℓ^{dᵢ} - 1) mod (fᵢ, ℓ^k)
    ///   Extract (result - 1) / ℓ mod ℓ → dᵢ columns
    [[nodiscard]] std::vector<uint32_t> compute_split(
            int64_t a, uint64_t b, const PrimeInfo& info) const {

        std::vector<uint32_t> result;
        result.reserve(degree_);

        for (const auto& fi : info.factors) {
            if (fi.degree == 1) {
                // Linear factor (x - r): γ mod (x - r, ℓ^k) = a - b·r (scalar)
                uint64_t r = (info.ell_k - fi.f_mod[0]) % info.ell_k;  // root
                int64_t a_mod = a % static_cast<int64_t>(info.ell_k);
                if (a_mod < 0) a_mod += static_cast<int64_t>(info.ell_k);
                uint64_t gamma = (static_cast<uint64_t>(a_mod) + info.ell_k -
                                  ((__uint128_t)b * r) % info.ell_k) % info.ell_k;

                // γ^(ℓ-1) for degree-1 factor (exponent = ℓ^1 - 1 = ℓ - 1)
                // For ℓ=2: exponent = 1, so γ^1 = γ
                uint64_t g_pow = gamma;
                if (fi.exponent > 1) {
                    // Compute gamma^exponent mod ℓ^k
                    uint64_t base = gamma, exp = fi.exponent;
                    g_pow = 1;
                    while (exp > 0) {
                        if (exp & 1) g_pow = ((__uint128_t)g_pow * base) % info.ell_k;
                        base = ((__uint128_t)base * base) % info.ell_k;
                        exp >>= 1;
                    }
                }

                // (g_pow - 1) / ℓ mod ℓ
                uint64_t v = (g_pow >= 1) ? (g_pow - 1) : (info.ell_k - 1);
                result.push_back(static_cast<uint32_t>((v / info.ell) % info.ell));

            } else if (fi.degree == 2) {
                // Quadratic factor: γ mod (fᵢ, ℓ^k) is a degree-1 polynomial
                // Reduce (a - bx) mod (fᵢ, ℓ^k) — since deg(a-bx) = 1 < 2, no reduction needed
                int64_t a_mod = a % static_cast<int64_t>(info.ell_k);
                if (a_mod < 0) a_mod += static_cast<int64_t>(info.ell_k);
                uint64_t b_mod = b % info.ell_k;
                uint64_t neg_b = (info.ell_k - b_mod) % info.ell_k;

                FastPoly g(static_cast<uint64_t>(a_mod), neg_b);

                // Compute g^(ℓ^2 - 1) mod (fᵢ, ℓ^k)
                auto g_pow = FastPoly::power(g, fi.exponent, fi.f_mod.data(), fi.degree, info.ell_k);

                // Extract 2 values: (g_pow - 1) / ℓ mod ℓ
                for (uint32_t i = 0; i < fi.degree; ++i) {
                    uint64_t coeff = g_pow.coeff(i);
                    if (i == 0) coeff = (coeff >= 1) ? (coeff - 1) : (info.ell_k - 1);
                    result.push_back(static_cast<uint32_t>((coeff / info.ell) % info.ell));
                }

            } else {
                // Higher degree factor — general case using FastPoly
                int64_t a_mod = a % static_cast<int64_t>(info.ell_k);
                if (a_mod < 0) a_mod += static_cast<int64_t>(info.ell_k);
                uint64_t b_mod = b % info.ell_k;
                uint64_t neg_b = (info.ell_k - b_mod) % info.ell_k;

                FastPoly g(static_cast<uint64_t>(a_mod), neg_b);
                auto g_pow = FastPoly::power(g, fi.exponent, fi.f_mod.data(), fi.degree, info.ell_k);

                for (uint32_t i = 0; i < fi.degree; ++i) {
                    uint64_t coeff = g_pow.coeff(i);
                    if (i == 0) coeff = (coeff >= 1) ? (coeff - 1) : (info.ell_k - 1);
                    result.push_back(static_cast<uint32_t>((coeff / info.ell) % info.ell));
                }
            }
        }

        // Pad to degree_ if needed (shouldn't happen if factorization is correct)
        while (result.size() < degree_) result.push_back(0);

        return result;
    }

    /// Compute Schirokauer map for a single prime (by index) - legacy interface
    [[nodiscard]] std::vector<uint32_t> compute_for_prime(
            int64_t a, uint64_t b, size_t prime_idx) const {
        return compute_for_prime_fast(a, b, prime_idx);
    }

    /// Compute Schirokauer map values as a flat vector (for matrix construction)
    /// Returns values for all primes concatenated
    [[nodiscard]] std::vector<uint32_t> compute_flat(int64_t a, uint64_t b) const {
        std::vector<uint32_t> result;
        result.reserve(num_columns());

        for (size_t i = 0; i < config_.primes.size(); ++i) {
            auto map = compute_for_prime_fast(a, b, i);
            for (uint32_t v : map) {
                result.push_back(v);
            }
        }

        return result;
    }

private:
    const PolynomialContext& ctx_;
    Config config_;
    uint32_t degree_;

    /// Pre-compute values for a prime
    void precompute_for_prime(uint32_t ell) {
        uint32_t k = config_.exponent_k;

        PrimeInfo info;
        info.ell = ell;

        // Compute ℓ^k
        info.ell_k = 1;
        for (uint32_t i = 0; i < k; ++i) {
            info.ell_k *= ell;
        }
        info.ell_k_minus_1 = info.ell_k / ell;

        // Compute f(x) mod ℓ^k
        info.f_mod.fill(0);
        for (uint32_t i = 0; i <= degree_; ++i) {
            Integer c = ctx_.coeff(i).clone();
            c %= Integer(info.ell_k);
            if (c.is_negative()) c += Integer(info.ell_k);
            info.f_mod[i] = c.to_uint64();
        }

        // Full Rabin irreducibility test (not just "no roots")
        // For degree > 3, "no roots" ≠ "irreducible"
        std::vector<uint64_t> f_mod_ell(degree_ + 1);
        for (uint32_t i = 0; i <= degree_; ++i) {
            f_mod_ell[i] = info.f_mod[i] % ell;
        }
        bool is_irred = ModularPoly::is_irreducible(f_mod_ell, ell);

        if (is_irred) {
            // f is irreducible mod ℓ — standard case
            info.is_split = false;
            uint64_t q = 1;
            for (uint32_t i = 0; i < degree_; ++i) q *= ell;
            info.exponent = q - 1;
        } else {
            // f is reducible mod ℓ — find roots for split Schirokauer
            std::vector<uint32_t> roots;
            for (uint32_t x = 0; x < ell; ++x) {
                uint64_t val = 0, xp = 1;
                for (uint32_t i = 0; i <= degree_; ++i) {
                    val = (val + f_mod_ell[i] * (xp % ell)) % ell;
                    xp = (xp * x) % ell;
                }
                if (val == 0) roots.push_back(x);
            }
            info.is_split = true;
            factorize_and_setup(info, ell, roots);
        }

        prime_info_.push_back(info);
    }

    /// Factorize f mod ℓ and set up split Schirokauer
    void factorize_and_setup(PrimeInfo& info, uint32_t ell, const std::vector<uint32_t>& roots) {
        // For degree 3 polynomial with roots mod ℓ:
        // - 1 root: f = (x - r) · g(x) where g is irreducible degree-2
        // - 3 roots: f = (x - r1)(x - r2)(x - r3)
        // For degree > 3, more complex factorization needed

        // Polynomial division: divide f by (x - r) for each root
        std::vector<uint64_t> f_coeffs(degree_ + 1);
        for (uint32_t i = 0; i <= degree_; ++i) {
            f_coeffs[i] = info.f_mod[i] % ell;
        }

        std::vector<std::vector<uint64_t>> irred_factors;

        // Divide out linear factors
        auto remaining = f_coeffs;
        for (uint32_t r : roots) {
            // Divide remaining by (x - r) mod ℓ
            // (x - r) = [ell - r, 1]
            uint32_t rem_deg = 0;
            for (int i = static_cast<int>(remaining.size()) - 1; i >= 0; --i) {
                if (remaining[i] != 0) { rem_deg = static_cast<uint32_t>(i); break; }
            }
            if (rem_deg == 0) break;

            std::vector<uint64_t> quotient(rem_deg, 0);
            uint64_t carry = 0;
            for (int i = static_cast<int>(rem_deg); i >= 1; --i) {
                uint64_t coeff = (remaining[i] + carry) % ell;
                quotient[i - 1] = coeff;
                carry = (coeff * r) % ell;
            }

            // Linear factor: (x - r) mod ℓ^k → stored as [ℓ^k - r_lifted, 1]
            std::vector<uint64_t> lin_factor = {(ell - r) % ell, 1};
            irred_factors.push_back(lin_factor);
            remaining = quotient;
        }

        // Whatever remains is the cofactor (should be irreducible if degree > 0)
        uint32_t rem_deg = 0;
        for (int i = static_cast<int>(remaining.size()) - 1; i >= 0; --i) {
            if (remaining[i] != 0) { rem_deg = static_cast<uint32_t>(i); break; }
        }
        if (rem_deg > 0) {
            remaining.resize(rem_deg + 1);
            irred_factors.push_back(remaining);
        }

        // Set up FactorInfo for each irreducible factor
        for (const auto& factor : irred_factors) {
            FactorInfo fi;
            fi.degree = static_cast<uint32_t>(factor.size()) - 1;

            // Exponent = ℓ^{deg} - 1
            uint64_t q = 1;
            for (uint32_t i = 0; i < fi.degree; ++i) q *= ell;
            fi.exponent = q - 1;

            // Lift factor to mod ℓ^k using Hensel lifting
            // For now, use the mod-ℓ factor directly mod ℓ^k
            // (simple lift: just use the factor as-is, with coefficients in [0, ℓ))
            fi.f_mod.fill(0);
            for (size_t i = 0; i < factor.size(); ++i) {
                // Lift coefficient: find factor mod ℓ^k that divides f mod ℓ^k
                fi.f_mod[i] = factor[i];  // Start with mod-ℓ values
            }

            // Hensel lift the factor to mod ℓ^k
            hensel_lift_factor(fi, info, ell);

            info.factors.push_back(fi);
        }
    }

    /// Hensel lift a factor of f from mod ℓ to mod ℓ^k
    void hensel_lift_factor(FactorInfo& fi, const PrimeInfo& info, uint32_t ell) {
        // For a linear factor (x - r) mod ℓ:
        // Lift to (x - r_lifted) mod ℓ^k where f(r_lifted) ≡ 0 mod ℓ^k
        if (fi.degree == 1) {
            // Linear factor: root r = (ℓ - fi.f_mod[0]) % ℓ
            uint64_t r = (ell - fi.f_mod[0]) % ell;
            // Hensel lift root: Newton's method
            // r_{new} = r - f(r)/f'(r) mod ℓ^k
            uint64_t modulus = ell;
            for (uint32_t step = 0; step < 20 && modulus < info.ell_k; ++step) {
                uint64_t new_mod = std::min(modulus * modulus, info.ell_k);
                // Evaluate f(r) mod new_mod
                uint64_t fr = 0, rp = 1;
                for (uint32_t i = 0; i <= degree_; ++i) {
                    fr = (fr + (__uint128_t)info.f_mod[i] * rp) % new_mod;
                    if (i < degree_) rp = ((__uint128_t)rp * r) % new_mod;
                }
                // Evaluate f'(r) mod new_mod
                uint64_t fpr = 0;
                rp = 1;
                for (uint32_t i = 1; i <= degree_; ++i) {
                    fpr = (fpr + (__uint128_t)i * ((__uint128_t)info.f_mod[i] * rp % new_mod)) % new_mod;
                    if (i < degree_) rp = ((__uint128_t)rp * r) % new_mod;
                }
                // Newton step: r = r - f(r) * f'(r)^{-1} mod new_mod
                // Need inverse of f'(r) mod new_mod
                // Use extended GCD
                int64_t t = 0, nt = 1, rv = static_cast<int64_t>(new_mod), nr = static_cast<int64_t>(fpr);
                while (nr != 0) {
                    int64_t q = rv / nr;
                    t -= q * nt; std::swap(t, nt);
                    rv -= q * nr; std::swap(rv, nr);
                }
                if (t < 0) t += static_cast<int64_t>(new_mod);
                uint64_t fpr_inv = static_cast<uint64_t>(t);
                r = (r + new_mod - (__uint128_t)fr * fpr_inv % new_mod) % new_mod;
                modulus = new_mod;
            }
            fi.f_mod[0] = (info.ell_k - r) % info.ell_k;
            fi.f_mod[1] = 1;
            return;
        }

        // For higher-degree factors: compute lifted factor via polynomial division.
        // All linear factors have already been Hensel-lifted and appear earlier in
        // info.factors. We divide f(x) mod ℓ^k by each lifted linear factor
        // (x - r_lifted) to obtain the exact lifted cofactor. The division is exact
        // because f(r_lifted) ≡ 0 (mod ℓ^k) by construction.
        {
            // Start with f mod ℓ^k
            std::vector<uint64_t> poly(degree_ + 1);
            for (uint32_t i = 0; i <= degree_; ++i) {
                poly[i] = info.f_mod[i];
            }
            uint32_t poly_deg = degree_;

            // Divide out each already-lifted linear factor
            for (const auto& other_fi : info.factors) {
                if (other_fi.degree != 1) continue;
                // Root r from factor (x - r) stored as [ℓ^k - r, 1]
                uint64_t r = (info.ell_k - other_fi.f_mod[0]) % info.ell_k;
                // Synthetic division by (x - r) mod ℓ^k
                for (int i = static_cast<int>(poly_deg) - 1; i >= 0; --i) {
                    __uint128_t prod = static_cast<__uint128_t>(r) * poly[i + 1] % info.ell_k;
                    poly[i] = static_cast<uint64_t>((poly[i] + prod) % info.ell_k);
                }
                // poly[0] is remainder (should be ≡ 0 mod ℓ^k), shift quotient down
                for (uint32_t i = 0; i < poly_deg; ++i) {
                    poly[i] = poly[i + 1];
                }
                poly[poly_deg] = 0;
                poly_deg--;
            }

            // Store the lifted cofactor
            fi.f_mod.fill(0);
            for (uint32_t i = 0; i <= fi.degree && i <= poly_deg; ++i) {
                fi.f_mod[i] = poly[i];
            }
        }
    }
};

/// Compute the number of Schirokauer columns for given configuration
[[nodiscard]] inline size_t schirokauer_num_columns(
        uint32_t degree,
        const std::vector<uint32_t>& primes) {
    return primes.size() * degree;
}

} // namespace linalg
} // namespace gnfs
