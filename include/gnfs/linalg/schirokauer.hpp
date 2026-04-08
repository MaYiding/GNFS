#pragma once

#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"
#include "../sqrt/modular_poly.hpp"

#include <vector>
#include <cstdint>
#include <array>
#include <random>
#include <tuple>
#include <numeric>

namespace gnfs {
namespace linalg {

using core::Integer;
using core::PolynomialContext;
using sqrt::ModularPoly;

// ============================================================================
// GF(ℓ) Polynomial Arithmetic and Factorization
// For proper Schirokauer map split-case handling when f mod ℓ is reducible
// ============================================================================
struct GFPolyOps {
    using Poly = std::vector<uint64_t>;

    static Poly trim(Poly p) {
        while (p.size() > 1 && p.back() == 0) p.pop_back();
        return p;
    }

    static bool is_zero(const Poly& p) {
        return p.empty() || (p.size() == 1 && p[0] == 0);
    }

    static uint64_t inv_mod(uint64_t a, uint64_t p) {
        if (a == 0) return 0;
        int64_t t = 0, nt = 1;
        int64_t r = static_cast<int64_t>(p), nr = static_cast<int64_t>(a % p);
        while (nr != 0) {
            int64_t q = r / nr;
            t -= q * nt; std::swap(t, nt);
            r -= q * nr; std::swap(r, nr);
        }
        return static_cast<uint64_t>((t % static_cast<int64_t>(p) +
                                       static_cast<int64_t>(p)) % static_cast<int64_t>(p));
    }

    static Poly add(const Poly& a, const Poly& b, uint64_t p) {
        Poly r(std::max(a.size(), b.size()), 0);
        for (size_t i = 0; i < r.size(); ++i) {
            uint64_t ai = i < a.size() ? a[i] : 0;
            uint64_t bi = i < b.size() ? b[i] : 0;
            r[i] = (ai + bi) % p;
        }
        return trim(r);
    }

    static Poly sub(const Poly& a, const Poly& b, uint64_t p) {
        Poly r(std::max(a.size(), b.size()), 0);
        for (size_t i = 0; i < r.size(); ++i) {
            uint64_t ai = i < a.size() ? a[i] : 0;
            uint64_t bi = i < b.size() ? b[i] : 0;
            r[i] = (ai + p - bi) % p;
        }
        return trim(r);
    }

    static Poly mul(const Poly& a, const Poly& b, uint64_t p) {
        if (is_zero(a) || is_zero(b)) return {0};
        Poly r(a.size() + b.size() - 1, 0);
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i] == 0) continue;
            for (size_t j = 0; j < b.size(); ++j) {
                // Use __uint128_t to avoid overflow when coefficients approach p-1
                __uint128_t prod = static_cast<__uint128_t>(a[i]) * b[j];
                r[i + j] = static_cast<uint64_t>((r[i + j] + prod % p) % p);
            }
        }
        return trim(r);
    }

    /// Polynomial division: returns (quotient, remainder)
    static std::pair<Poly, Poly> divmod(Poly a, const Poly& b, uint64_t p) {
        a = trim(a);
        if (is_zero(b)) return {{0}, a};
        if (a.size() < b.size()) return {{0}, a};
        uint64_t b_inv = inv_mod(b.back(), p);
        Poly q(a.size() - b.size() + 1, 0);
        for (int i = static_cast<int>(a.size()) - 1;
             i >= static_cast<int>(b.size()) - 1; --i) {
            uint64_t c = static_cast<uint64_t>(static_cast<__uint128_t>(a[i]) * b_inv % p);
            q[i - (static_cast<int>(b.size()) - 1)] = c;
            for (size_t j = 0; j < b.size(); ++j) {
                uint64_t cb = static_cast<uint64_t>(static_cast<__uint128_t>(c) * b[j] % p);
                a[i - (static_cast<int>(b.size()) - 1) + j] =
                    (a[i - (static_cast<int>(b.size()) - 1) + j] + p - cb) % p;
            }
        }
        return {trim(q), trim(a)};
    }

    static Poly gcd(Poly a, Poly b, uint64_t p) {
        a = trim(a); b = trim(b);
        while (!is_zero(b)) {
            auto [q, r] = divmod(a, b, p);
            a = b; b = r;
        }
        // Make monic
        if (!a.empty() && a.back() != 0 && a.back() != 1) {
            uint64_t inv = inv_mod(a.back(), p);
            for (auto& c : a) c = c * inv % p;
        }
        return a;
    }

    /// Extended GCD: returns (gcd, s, t) with s*a + t*b = gcd
    static std::tuple<Poly, Poly, Poly> extended_gcd(Poly a, Poly b, uint64_t p) {
        Poly old_r = trim(a), r = trim(b);
        Poly old_s = {1}, s = {0};
        Poly old_t = {0}, t = {1};
        while (!is_zero(r)) {
            auto [q, rem] = divmod(old_r, r, p);
            old_r = r; r = rem;
            Poly ns = sub(old_s, mul(q, s, p), p);
            old_s = s; s = ns;
            Poly nt_new = sub(old_t, mul(q, t, p), p);
            old_t = t; t = nt_new;
        }
        // Make gcd monic
        if (!old_r.empty() && old_r.back() != 0 && old_r.back() != 1) {
            uint64_t inv = inv_mod(old_r.back(), p);
            for (auto& c : old_r) c = c * inv % p;
            for (auto& c : old_s) c = c * inv % p;
            for (auto& c : old_t) c = c * inv % p;
        }
        return {old_r, old_s, old_t};
    }

    /// base^exp mod (mod, p) using square-and-multiply
    static Poly powmod(Poly base, uint64_t exp, const Poly& mod, uint64_t p) {
        Poly result = {1};
        base = divmod(base, mod, p).second;
        while (exp > 0) {
            if (exp & 1) {
                result = mul(result, base, p);
                result = divmod(result, mod, p).second;
            }
            base = mul(base, base, p);
            base = divmod(base, mod, p).second;
            exp >>= 1;
        }
        return result;
    }

    /// Distinct-degree factorization of squarefree monic f over GF(p)
    /// Returns pairs (degree, product_of_factors_of_that_degree)
    static std::vector<std::pair<uint32_t, Poly>> ddf(Poly f, uint64_t p) {
        std::vector<std::pair<uint32_t, Poly>> result;
        f = trim(f);
        if (f.size() <= 1) return result;
        // Make monic
        if (f.back() != 1) {
            uint64_t inv = inv_mod(f.back(), p);
            for (auto& c : f) c = c * inv % p;
        }

        Poly h = {0, 1};  // h = x
        for (uint32_t d = 1; 2 * d <= static_cast<uint32_t>(f.size() - 1); ++d) {
            // h = h^p mod f (so h = x^{p^d} mod f at step d)
            h = powmod(h, p, f, p);

            // g_d = gcd(h - x, f)
            Poly h_minus_x = h;
            if (h_minus_x.size() < 2) h_minus_x.resize(2, 0);
            h_minus_x[1] = (h_minus_x[1] + p - 1) % p;
            h_minus_x = trim(h_minus_x);

            Poly g_d = gcd(h_minus_x, f, p);

            if (g_d.size() > 1) {
                result.push_back({d, g_d});
                f = divmod(f, g_d, p).first;
                f = trim(f);
                if (f.size() <= 1) break;
                h = divmod(h, f, p).second;
            }
        }
        if (f.size() > 1) {
            result.push_back({static_cast<uint32_t>(f.size() - 1), f});
        }
        return result;
    }

    /// Equal-degree factorization (Cantor-Zassenhaus) over GF(p)
    /// f is a product of distinct irreducible polys each of degree d
    static std::vector<Poly> edf(const Poly& f, uint32_t d, uint64_t p) {
        if (static_cast<uint32_t>(f.size() - 1) == d) return {f};
        if (f.size() <= 1) return {};

        std::mt19937 rng(12345 + d);
        for (int attempt = 0; attempt < 200; ++attempt) {
            Poly t(f.size() - 1, 0);
            for (size_t i = 0; i < t.size(); ++i) t[i] = rng() % p;
            t = trim(t);
            if (is_zero(t)) continue;

            Poly g;
            if (p == 2) {
                // Trace map: Tr(t) = t + t^2 + t^{2^2} + ... + t^{2^{d-1}}
                Poly trace = t;
                Poly ti = t;
                for (uint32_t i = 1; i < d; ++i) {
                    ti = mul(ti, ti, p);
                    ti = divmod(ti, f, p).second;
                    trace = add(trace, ti, p);
                }
                g = gcd(trace, f, p);
            } else {
                // g = gcd(t^{(p^d-1)/2} - 1, f)
                uint64_t exp = 1;
                for (uint32_t i = 0; i < d; ++i) exp *= p;
                exp = (exp - 1) / 2;
                Poly tp = powmod(t, exp, f, p);
                tp[0] = (tp[0] + p - 1) % p;
                tp = trim(tp);
                g = gcd(tp, f, p);
            }

            if (g.size() > 1 && g.size() < f.size()) {
                auto left = edf(g, d, p);
                auto right_poly = divmod(f, g, p).first;
                auto right = edf(right_poly, d, p);
                left.insert(left.end(), right.begin(), right.end());
                return left;
            }
        }
        return {f};  // failed to split
    }

    /// Full factorization of squarefree part of f over GF(p)
    /// Returns monic irreducible factors (multiplicities stripped)
    /// Returns empty if f has repeated factors and can't be squarefree-decomposed
    static std::vector<Poly> factor(Poly f, uint64_t p) {
        f = trim(f);
        if (f.size() <= 1) return {};

        // Make monic
        if (f.back() != 1) {
            uint64_t inv = inv_mod(f.back(), p);
            for (auto& c : f) c = c * inv % p;
        }

        // Square-free check: gcd(f, f')
        Poly fp(f.size() - 1, 0);
        for (size_t i = 1; i < f.size(); ++i) {
            fp[i - 1] = (i % p) * f[i] % p;
        }
        fp = trim(fp);

        Poly g = gcd(f, fp, p);
        Poly f_sqfree;
        if (g.size() <= 1) {
            f_sqfree = f;  // already squarefree
        } else {
            f_sqfree = divmod(f, g, p).first;
            f_sqfree = trim(f_sqfree);
        }

        if (f_sqfree.size() <= 1) {
            // f is a perfect power — can't easily extract factors
            return {};  // signal caller to use fallback
        }

        // DDF + EDF on squarefree part
        auto ddf_result = ddf(f_sqfree, p);

        std::vector<Poly> factors;
        for (auto& [d, h] : ddf_result) {
            auto parts = edf(h, d, p);
            for (auto& part : parts) {
                if (!part.empty() && part.back() != 1) {
                    uint64_t inv = inv_mod(part.back(), p);
                    for (auto& c : part) c = c * inv % p;
                }
                factors.push_back(part);
            }
        }
        return factors;
    }

    /// Hensel lift two coprime factors g, h (mod ℓ) to mod ℓ^k
    /// such that f ≡ g*h (mod ℓ^k)
    /// g0, h0: original mod-ℓ factors (kept for Bezout reference)
    /// t_bezout: h0^{-1} mod g0 in GF(ℓ) (from extended GCD: s*g0 + t*h0 = 1)
    static void hensel_lift_pair(
        const std::vector<uint64_t>& f_pk,
        std::vector<uint64_t>& g,
        std::vector<uint64_t>& h,
        const Poly& g0, const Poly& h0,
        const Poly& t_bezout,
        uint32_t ell, uint32_t k)
    {
        uint64_t target = 1;
        for (uint32_t i = 0; i < k; ++i) target *= ell;

        uint64_t modulus = ell;
        for (uint32_t step = 1; step < k; ++step) {
            // Compute g*h over Z (using modular arithmetic at target precision)
            size_t g_sz = g.size(), h_sz = h.size();
            std::vector<int64_t> prod(g_sz + h_sz - 1, 0);
            for (size_t i = 0; i < g_sz; ++i) {
                for (size_t j = 0; j < h_sz; ++j) {
                    int64_t p = static_cast<int64_t>(
                        static_cast<__uint128_t>(g[i]) * h[j] % target);
                    prod[i + j] = (prod[i + j] + p) % static_cast<int64_t>(target);
                }
            }

            // epsilon = (f - g*h) / modulus mod ℓ
            Poly eps(f_pk.size(), 0);
            for (size_t i = 0; i < f_pk.size(); ++i) {
                int64_t fi = static_cast<int64_t>(f_pk[i]);
                int64_t pi = (i < prod.size()) ?
                    ((prod[i] % static_cast<int64_t>(target)) + static_cast<int64_t>(target))
                        % static_cast<int64_t>(target) : 0;
                int64_t diff = fi - pi;
                if (diff < 0) diff += static_cast<int64_t>(target);
                // diff is divisible by modulus (by induction)
                int64_t ei = (diff / static_cast<int64_t>(modulus))
                             % static_cast<int64_t>(ell);
                if (ei < 0) ei += static_cast<int64_t>(ell);
                eps[i] = static_cast<uint64_t>(ei);
            }
            eps = trim(eps);

            // δg = (t_bezout * eps) mod g0 in GF(ℓ)
            auto t_eps = mul(t_bezout, eps, ell);
            auto delta_g = divmod(t_eps, g0, ell).second;

            // δh = (eps - h0 * delta_g) / g0 in GF(ℓ) (exact division)
            auto h0_dg = mul(h0, delta_g, ell);
            auto num = sub(eps, h0_dg, ell);
            auto delta_h = divmod(num, g0, ell).first;

            // Update g and h
            uint64_t new_mod = modulus * ell;
            for (size_t i = 0; i < g.size(); ++i) {
                uint64_t dg = i < delta_g.size() ? delta_g[i] : 0;
                g[i] = (g[i] + modulus * dg) % new_mod;
            }
            for (size_t i = 0; i < h.size(); ++i) {
                uint64_t dh = i < delta_h.size() ? delta_h[i] : 0;
                h[i] = (h[i] + modulus * dh) % new_mod;
            }
            modulus = new_mod;
        }
    }

    /// Recursively Hensel lift multiple coprime factors from mod ℓ to mod ℓ^k
    static std::vector<std::vector<uint64_t>> hensel_lift_all(
        const std::vector<uint64_t>& f_pk,
        const std::vector<Poly>& factors,
        uint32_t ell, uint32_t k)
    {
        if (factors.size() == 1) {
            return {f_pk};
        }

        if (factors.size() == 2) {
            auto& g0 = factors[0];
            auto& h0 = factors[1];
            auto [gcd_val, s, t] = extended_gcd(g0, h0, ell);
            std::vector<uint64_t> g(g0.begin(), g0.end());
            std::vector<uint64_t> h(h0.begin(), h0.end());
            hensel_lift_pair(f_pk, g, h, g0, h0, t, ell, k);
            return {g, h};
        }

        // Split into two groups and recurse
        size_t mid = factors.size() / 2;

        Poly product_a = {1};
        for (size_t i = 0; i < mid; ++i)
            product_a = mul(product_a, factors[i], ell);

        Poly product_b = {1};
        for (size_t i = mid; i < factors.size(); ++i)
            product_b = mul(product_b, factors[i], ell);

        auto [gcd_val, s, t] = extended_gcd(product_a, product_b, ell);
        std::vector<uint64_t> lifted_a(product_a.begin(), product_a.end());
        std::vector<uint64_t> lifted_b(product_b.begin(), product_b.end());
        hensel_lift_pair(f_pk, lifted_a, lifted_b, product_a, product_b, t, ell, k);

        std::vector<Poly> group_a(factors.begin(), factors.begin() + mid);
        std::vector<Poly> group_b(factors.begin() + mid, factors.end());

        auto result_a = hensel_lift_all(lifted_a, group_a, ell, k);
        auto result_b = hensel_lift_all(lifted_b, group_b, ell, k);

        result_a.insert(result_a.end(), result_b.begin(), result_b.end());
        return result_a;
    }
};

/// Schirokauer map configuration
struct SchirokaurConfig {
    std::vector<uint32_t> primes = {2};  // Primes for Schirokauer maps (usually just 2)
    uint32_t exponent_k = 8;              // Exponent k (use ℓ^k), larger = more accurate
                                          // k ≥ 8 needed for split case: non-unit elements
                                          // require ℓ-part stripping, reducing precision by v.
                                          // Formula (u^e-1)/ℓ mod ℓ needs ℓ^2 precision,
                                          // so k - v ≥ 2. k=8 handles v ≤ 6 (prob 99.2%)
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

        // FastPoly uses fixed-size arrays — degree must not exceed MAX_DEGREE
        if (degree_ > FastPoly::MAX_DEGREE) {
            config_.primes.clear();
            return;
        }

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
    ///   If γ ≡ 0 mod (fᵢ, ℓ): strip ℓ-part to get unit before computing map
    ///   Compute unit^(ℓ^{dᵢ} - 1) mod (fᵢ, ℓ^k)
    ///   Extract (result - 1) / ℓ mod ℓ → dᵢ columns
    [[nodiscard]] std::vector<uint32_t> compute_split(
            int64_t a, uint64_t b, const PrimeInfo& info) const {

        std::vector<uint32_t> result;
        result.reserve(degree_);

        for (const auto& fi : info.factors) {
            if (fi.degree == 1) {
                // Linear factor (x - r): γ mod (x - r, ℓ^k) = a - b·r mod ℓ^k
                uint64_t r = (info.ell_k - fi.f_mod[0]) % info.ell_k;  // root

                // Compute gamma = (a - b*r) mod ℓ^k using the full Hensel-lifted root
                int64_t a_mod = a % static_cast<int64_t>(info.ell_k);
                if (a_mod < 0) a_mod += static_cast<int64_t>(info.ell_k);
                uint64_t b_mod = b % info.ell_k;
                uint64_t gamma = (static_cast<uint64_t>(a_mod) + info.ell_k -
                    ((__uint128_t)b_mod * r) % info.ell_k) % info.ell_k;

                // Strip ℓ-part: the ideal valuation v_P(γ) is already captured by
                // the factor base column. The Schirokauer map should only capture
                // the unit part. For non-units (γ ≡ 0 mod ℓ), the formula
                // (γ^{ℓ-1} - 1)/ℓ is undefined, so we must strip ℓ first.
                if (gamma == 0) {
                    // γ ≡ 0 mod ℓ^k — degenerate, set Schirokauer to 0
                    result.push_back(0);
                    continue;
                }
                while (gamma % info.ell == 0) {
                    gamma /= info.ell;
                }
                // gamma is now the unit part (coprime to ℓ), known mod ℓ^{k-v}
                // where v was the stripped valuation. Need k-v ≥ 2 for formula.

                // γ^(ℓ-1) for degree-1 factor (exponent = ℓ^1 - 1 = ℓ - 1)
                // For ℓ=2: exponent = 1, so γ^1 = γ
                uint64_t g_pow = gamma;
                if (fi.exponent > 1) {
                    // Compute gamma^exponent mod ℓ^k
                    uint64_t base = gamma % info.ell_k;
                    uint64_t exp = fi.exponent;
                    g_pow = 1;
                    while (exp > 0) {
                        if (exp & 1) g_pow = ((__uint128_t)g_pow * base) % info.ell_k;
                        base = ((__uint128_t)base * base) % info.ell_k;
                        exp >>= 1;
                    }
                } else {
                    gamma %= info.ell_k;
                    g_pow = gamma;
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

                // Strip ℓ-part: if both coefficients are divisible by ℓ, divide out
                uint64_t c0 = static_cast<uint64_t>(a_mod);
                uint64_t c1 = neg_b;
                while (c0 % info.ell == 0 && c1 % info.ell == 0 &&
                       (c0 != 0 || c1 != 0)) {
                    c0 /= info.ell;
                    c1 /= info.ell;
                }
                // Ensure coefficients stay in [0, ell_k)
                c0 %= info.ell_k;
                c1 %= info.ell_k;

                FastPoly g(c0, c1);

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

                // Strip ℓ-part: if both coefficients are divisible by ℓ, divide out
                uint64_t c0 = static_cast<uint64_t>(a_mod);
                uint64_t c1 = neg_b;
                while (c0 % info.ell == 0 && c1 % info.ell == 0 &&
                       (c0 != 0 || c1 != 0)) {
                    c0 /= info.ell;
                    c1 /= info.ell;
                }
                c0 %= info.ell_k;
                c1 %= info.ell_k;

                FastPoly g(c0, c1);
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
            // f is reducible mod ℓ — proper factorization using DDF + EDF
            info.is_split = true;
            factorize_and_setup(info, ell, f_mod_ell);
        }

        prime_info_.push_back(info);
    }

    /// Factorize f mod ℓ using DDF+EDF and Hensel lift factors to mod ℓ^k.
    /// Handles three cases:
    ///   1. f is a perfect power mod ℓ → zero-fill all Schirokauer columns
    ///   2. f is squarefree mod ℓ → standard multi-factor Hensel lift
    ///   3. f has repeated roots mod ℓ → lift only multiplicity-1 factors, zero-pad rest
    void factorize_and_setup(PrimeInfo& info, uint32_t ell,
                             const std::vector<uint64_t>& f_mod_ell) {
        // Factor the squarefree part of f mod ℓ into irreducible factors
        auto irred_factors = GFPolyOps::factor(f_mod_ell, ell);

        uint32_t deg_sum = 0;
        for (const auto& fac : irred_factors) {
            deg_sum += static_cast<uint32_t>(fac.size()) - 1;
        }

        // Case 1: f is a perfect power mod ℓ — no squarefree factors found
        // Fall back to unsplit mode: use γ^(ℓ^d-1) mod (f, ℓ^k).
        // Zero-filling would remove all Schirokauer constraints from the matrix,
        // causing ALL dependencies to give trivial factors.
        if (irred_factors.empty()) {
            info.is_split = false;
            info.factors.clear();
            uint64_t q = 1;
            for (uint32_t i = 0; i < degree_; ++i) q *= ell;
            info.exponent = q - 1;
            return;  // compute_unsplit() will handle this
        }

        // Prepare f mod ℓ^k for Hensel lifting
        std::vector<uint64_t> f_pk(degree_ + 1);
        for (uint32_t i = 0; i <= degree_; ++i) {
            f_pk[i] = info.f_mod[i];
        }

        // Case 2: f is squarefree mod ℓ (factor degrees sum to polynomial degree)
        if (deg_sum == degree_ && irred_factors.size() >= 2) {
            auto lifted = GFPolyOps::hensel_lift_all(
                f_pk, irred_factors, ell, config_.exponent_k);

            info.factors.clear();
            for (size_t idx = 0; idx < irred_factors.size(); ++idx) {
                FactorInfo fi;
                fi.degree = static_cast<uint32_t>(irred_factors[idx].size()) - 1;
                uint64_t q = 1;
                for (uint32_t i = 0; i < fi.degree; ++i) q *= ell;
                fi.exponent = q - 1;
                fi.f_mod.fill(0);
                for (size_t i = 0; i < lifted[idx].size() && i <= FastPoly::MAX_DEGREE; ++i) {
                    fi.f_mod[i] = lifted[idx][i];
                }
                info.factors.push_back(fi);
            }
            return;
        }

        // Case 3: f has repeated roots mod ℓ (deg_sum < degree_ or single factor)
        // Only multiplicity-1 factors can be Hensel-lifted (coprime with cofactor).
        // Multiplicity > 1 factors share roots with their cofactor, making Hensel
        // lifting impossible. Those columns are zero-padded — fewer constraints
        // but still correct (just slightly less efficient).
        info.factors.clear();

        GFPolyOps::Poly f_ell = GFPolyOps::trim(
            GFPolyOps::Poly(f_mod_ell.begin(), f_mod_ell.end()));

        for (const auto& fac : irred_factors) {
            // Compute multiplicity of this factor in f mod ℓ
            uint32_t multiplicity = 0;
            GFPolyOps::Poly remaining = f_ell;
            while (remaining.size() >= fac.size()) {
                auto [quot, r] = GFPolyOps::divmod(remaining, fac, ell);
                if (GFPolyOps::is_zero(r)) {
                    multiplicity++;
                    remaining = quot;
                } else {
                    break;
                }
            }

            if (multiplicity != 1) continue;  // Can't Hensel-lift repeated factors

            // Cofactor = f / fac mod ℓ (coprime since multiplicity == 1)
            auto [cofactor, rem] = GFPolyOps::divmod(f_ell, fac, ell);

            // Verify coprimality (should hold for multiplicity-1 factors)
            auto gcd_check = GFPolyOps::gcd(fac, cofactor, ell);
            if (gcd_check.size() > 1) continue;  // Not coprime — skip

            // Extended GCD for Bezout coefficients needed by Hensel lifting
            auto [gcd_val, s_bezout, t_bezout] = GFPolyOps::extended_gcd(fac, cofactor, ell);

            // Hensel lift this factor individually against f
            std::vector<uint64_t> g_lift(fac.begin(), fac.end());
            std::vector<uint64_t> h_lift(cofactor.begin(), cofactor.end());
            GFPolyOps::hensel_lift_pair(
                f_pk, g_lift, h_lift, fac, cofactor, t_bezout, ell, config_.exponent_k);

            // Store the lifted factor
            FactorInfo fi;
            fi.degree = static_cast<uint32_t>(fac.size()) - 1;
            uint64_t q = 1;
            for (uint32_t i = 0; i < fi.degree; ++i) q *= ell;
            fi.exponent = q - 1;
            fi.f_mod.fill(0);
            for (size_t i = 0; i < g_lift.size() && i <= FastPoly::MAX_DEGREE; ++i) {
                fi.f_mod[i] = g_lift[i];
            }
            info.factors.push_back(fi);
        }

        // If no multiplicity-1 factors survived, fall back to unsplit mode.
        // Zero-padding all columns removes Schirokauer constraints entirely,
        // which makes ALL dependencies trivial (same failure as Case 1).
        if (info.factors.empty()) {
            info.is_split = false;
            uint64_t q = 1;
            for (uint32_t i = 0; i < degree_; ++i) q *= ell;
            info.exponent = q - 1;
            return;  // compute_unsplit() will handle this
        }
        // Otherwise, compute_split() will zero-pad remaining columns up to degree_
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
