#pragma once

#include "../core/integer.hpp"
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace gnfs::sqrt {

using core::Integer;

/// ModularPoly - Polynomial arithmetic over F_p
/// Represents polynomials in F_p[x] and operations mod f(x)
class ModularPoly {
public:
    /// Construct zero polynomial
    ModularPoly() = default;

    /// Construct from coefficients (coeffs[i] is coefficient of x^i)
    explicit ModularPoly(std::vector<uint64_t> coeffs) : coeffs_(std::move(coeffs)) {
        normalize();
    }

    /// Construct constant polynomial
    explicit ModularPoly(uint64_t value) {
        if (value != 0) {
            coeffs_.push_back(value);
        }
    }

    /// Copy operations
    ModularPoly(const ModularPoly&) = default;
    ModularPoly& operator=(const ModularPoly&) = default;
    ModularPoly(ModularPoly&&) = default;
    ModularPoly& operator=(ModularPoly&&) = default;

    /// Get degree (-1 for zero polynomial)
    [[nodiscard]] int degree() const noexcept {
        return coeffs_.empty() ? -1 : static_cast<int>(coeffs_.size()) - 1;
    }

    /// Get coefficient
    [[nodiscard]] uint64_t coeff(size_t i) const noexcept {
        return i < coeffs_.size() ? coeffs_[i] : 0;
    }

    /// Set coefficient
    void set_coeff(size_t i, uint64_t value) {
        if (coeffs_.size() <= i) {
            coeffs_.resize(i + 1, 0);  // single resize replaces N push_back(0) loop
        }
        coeffs_[i] = value;
        normalize();
    }

    /// Check if zero
    [[nodiscard]] bool is_zero() const noexcept {
        return coeffs_.empty();
    }

    /// Check if one
    [[nodiscard]] bool is_one() const noexcept {
        return coeffs_.size() == 1 && coeffs_[0] == 1;
    }

    /// Get coefficients
    [[nodiscard]] const std::vector<uint64_t>& coefficients() const noexcept {
        return coeffs_;
    }

    /// Addition mod p
    [[nodiscard]] static ModularPoly add(const ModularPoly& a, const ModularPoly& b, uint64_t p) {
        size_t max_size = std::max(a.coeffs_.size(), b.coeffs_.size());
        std::vector<uint64_t> result(max_size);

        for (size_t i = 0; i < max_size; ++i) {
            uint64_t ai = a.coeff(i), bi = b.coeff(i);
            // Overflow-safe addition: avoid ai + bi > UINT64_MAX when p >= 2^63
            result[i] = (ai >= p - bi) ? ai - (p - bi) : ai + bi;
        }

        return ModularPoly(std::move(result));
    }

    /// Subtraction mod p
    [[nodiscard]] static ModularPoly sub(const ModularPoly& a, const ModularPoly& b, uint64_t p) {
        size_t max_size = std::max(a.coeffs_.size(), b.coeffs_.size());
        std::vector<uint64_t> result(max_size);

        for (size_t i = 0; i < max_size; ++i) {
            uint64_t ai = a.coeff(i);
            uint64_t bi = b.coeff(i);
            result[i] = ai >= bi ? ai - bi : p - (bi - ai);
        }

        return ModularPoly(std::move(result));
    }

    /// Scalar multiplication mod p
    [[nodiscard]] static ModularPoly scalar_mul(const ModularPoly& a, uint64_t c, uint64_t p) {
        std::vector<uint64_t> result(a.coeffs_.size());

        for (size_t i = 0; i < a.coeffs_.size(); ++i) {
            result[i] = mul_mod(a.coeffs_[i], c, p);
        }

        return ModularPoly(std::move(result));
    }

    /// Polynomial multiplication mod p (no reduction by f)
    [[nodiscard]] static ModularPoly mul_raw(const ModularPoly& a, const ModularPoly& b, uint64_t p) {
        if (a.is_zero() || b.is_zero()) {
            return ModularPoly();
        }

        size_t result_size = a.coeffs_.size() + b.coeffs_.size() - 1;
        std::vector<uint64_t> result(result_size, 0);

        for (size_t i = 0; i < a.coeffs_.size(); ++i) {
            if (a.coeffs_[i] == 0) continue;
            for (size_t j = 0; j < b.coeffs_.size(); ++j) {
                if (b.coeffs_[j] == 0) continue;
                uint64_t prod = mul_mod(a.coeffs_[i], b.coeffs_[j], p);
                result[i + j] = (result[i + j] + prod) % p;
            }
        }

        return ModularPoly(std::move(result));
    }

    /// Polynomial multiplication mod f(x) and mod p
    /// f need not be monic; leading coefficient must be invertible mod p
    [[nodiscard]] static ModularPoly mul(
            const ModularPoly& a,
            const ModularPoly& b,
            const std::vector<uint64_t>& f,
            uint64_t p) {

        auto product = mul_raw(a, b, p);
        return reduce(product, f, p);
    }

    /// Reduce polynomial mod f(x) and mod p
    /// Handles both monic and non-monic f via modular inverse of leading coeff
    [[nodiscard]] static ModularPoly reduce(
            const ModularPoly& a,
            const std::vector<uint64_t>& f,
            uint64_t p) {

        if (a.coeffs_.size() < f.size()) {
            return a;
        }

        std::vector<uint64_t> result = a.coeffs_;
        int f_deg = static_cast<int>(f.size()) - 1;

        // Inverse of leading coefficient: α^d = -(f[0]+...+f[d-1]α^{d-1}) / f[d]
        // f[f_deg] must be non-zero mod p (otherwise f degenerates)
        if (f[f_deg] % p == 0) {
            throw std::runtime_error("ModularPoly::reduce: leading coefficient ≡ 0 (mod p)");
        }
        uint64_t f_lead_inv = mod_inverse(f[f_deg], p);

        // Reduce from highest degree down
        while (static_cast<int>(result.size()) > f_deg) {
            uint64_t lead = result.back();
            result.pop_back();

            if (lead == 0) continue;

            // Scale by inverse of leading coefficient
            uint64_t lead_scaled = mul_mod(lead, f_lead_inv, p);

            for (int i = 0; i < f_deg; ++i) {
                uint64_t term = mul_mod(lead_scaled, f[i], p);
                int idx = static_cast<int>(result.size()) - f_deg + i;
                if (idx >= 0 && idx < static_cast<int>(result.size())) {
                    if (result[idx] >= term) {
                        result[idx] -= term;
                    } else {
                        result[idx] = p - (term - result[idx]);
                    }
                }
            }
        }

        // Normalize
        while (!result.empty() && result.back() == 0) {
            result.pop_back();
        }

        return ModularPoly(std::move(result));
    }

    /// Power mod f(x) and mod p using binary exponentiation
    [[nodiscard]] static ModularPoly power(
            const ModularPoly& base,
            const Integer& exp,
            const std::vector<uint64_t>& f,
            uint64_t p) {

        if (exp.is_zero()) {
            return ModularPoly(1);
        }

        ModularPoly result(1);
        ModularPoly b = base;
        Integer e = exp.clone();

        while (!e.is_zero()) {
            if (e.is_odd()) {
                result = mul(result, b, f, p);
            }
            b = mul(b, b, f, p);
            mpz_tdiv_q_2exp(e.get_mpz(), e.get_mpz(), 1);
        }

        return result;
    }

    /// GCD of two polynomials mod p
    [[nodiscard]] static ModularPoly gcd(
            const ModularPoly& a,
            const ModularPoly& b,
            uint64_t p) {

        ModularPoly x = a;
        ModularPoly y = b;

        while (!y.is_zero()) {
            auto [q, r] = divmod(x, y, p);
            x = std::move(y);
            y = std::move(r);
        }

        // Make monic
        if (!x.is_zero() && x.coeffs_.back() != 1) {
            uint64_t inv = mod_inverse(x.coeffs_.back(), p);
            x = scalar_mul(x, inv, p);
        }

        return x;
    }

    /// Division with remainder mod p
    [[nodiscard]] static std::pair<ModularPoly, ModularPoly> divmod(
            const ModularPoly& a,
            const ModularPoly& b,
            uint64_t p) {

        if (b.is_zero()) {
            throw std::runtime_error("Division by zero polynomial");
        }

        if (a.degree() < b.degree()) {
            return {ModularPoly(), a};
        }

        std::vector<uint64_t> rem = a.coeffs_;
        std::vector<uint64_t> quot(a.degree() - b.degree() + 1, 0);

        uint64_t b_lead_inv = mod_inverse(b.coeffs_.back(), p);

        for (int i = a.degree(); i >= b.degree(); --i) {
            if (rem[i] == 0) continue;

            uint64_t c = mul_mod(rem[i], b_lead_inv, p);
            quot[i - b.degree()] = c;

            for (int j = 0; j <= b.degree(); ++j) {
                uint64_t term = mul_mod(c, b.coeffs_[j], p);
                if (rem[i - b.degree() + j] >= term) {
                    rem[i - b.degree() + j] -= term;
                } else {
                    rem[i - b.degree() + j] = p - (term - rem[i - b.degree() + j]);
                }
            }
        }

        return {ModularPoly(std::move(quot)), ModularPoly(std::move(rem))};
    }

    /// Compute Frobenius map: a(x) -> a(x^p) mod f(x) mod p
    [[nodiscard]] static ModularPoly frobenius(
            const ModularPoly& a,
            const std::vector<uint64_t>& f,
            uint64_t p) {

        // x^p mod f(x) mod p
        ModularPoly x_to_p;
        x_to_p.set_coeff(1, 1);
        x_to_p = power(x_to_p, Integer(p), f, p);

        // Compute a(x^p) by substitution
        ModularPoly result(a.coeff(0));
        ModularPoly x_p_power(1);

        for (int i = 1; i <= a.degree(); ++i) {
            x_p_power = mul(x_p_power, x_to_p, f, p);
            auto term = scalar_mul(x_p_power, a.coeff(i), p);
            result = add(result, term, p);
        }

        return result;
    }

    /// Check if polynomial is a square in F_p[x]/f(x)
    /// Uses Euler's criterion: a^((p^d - 1)/2) = 1 iff a is square
    [[nodiscard]] static bool is_square(
            const ModularPoly& a,
            const std::vector<uint64_t>& f,
            uint64_t p) {

        if (a.is_zero()) return true;
        if (a.is_one()) return true;

        // Characteristic 2: Frobenius x→x² is a bijection on F_{2^d}
        // (mult group order 2^d-1 is odd → squaring is an automorphism).
        // Every element is a square.
        if (p == 2) return true;

        int d = static_cast<int>(f.size()) - 1;

        // Compute (p^d - 1) / 2
        Integer pd(1);
        for (int i = 0; i < d; ++i) {
            pd *= Integer(p);
        }
        pd -= Integer(static_cast<int64_t>(1));
        mpz_tdiv_q_2exp(pd.get_mpz(), pd.get_mpz(), 1);

        auto result = power(a, pd, f, p);
        return result.is_one();
    }

    /// Rabin irreducibility test: check if f(x) is irreducible over F_p.
    /// f is irreducible iff:
    ///   (1) for every prime factor q of deg(f): gcd(x^{p^{d/q}} - x, f) = 1
    ///   (2) x^{p^d} ≡ x mod f
    /// Cost: O(d^2 * d * log p) — d steps of modular exponentiation.
    [[nodiscard]] static bool is_irreducible(const std::vector<uint64_t>& f, uint64_t p) {
        int d = static_cast<int>(f.size()) - 1;
        if (d <= 0) return false;
        // Leading coeff ≡ 0 (mod p) → f degenerates (ramified prime), not irreducible at this degree
        if (f[d] % p == 0) return false;
        if (d == 1) return true;  // linear polynomials are always irreducible

        // Find distinct prime factors of d
        std::vector<int> prime_factors;
        {
            int temp = d;
            for (int q = 2; q * q <= temp; ++q) {
                if (temp % q == 0) {
                    prime_factors.push_back(q);
                    while (temp % q == 0) temp /= q;
                }
            }
            if (temp > 1) prime_factors.push_back(temp);
        }

        // Compute x^{p^k} mod f iteratively: x^{p^1}, x^{p^2}, ..., x^{p^d}
        // Each step: x^{p^{k}} = (x^{p^{k-1}})^p mod f
        ModularPoly x_poly;
        x_poly.set_coeff(1, 1);  // x

        std::vector<ModularPoly> x_pow_pk(d + 1);
        x_pow_pk[0] = x_poly;  // x^{p^0} = x
        for (int k = 1; k <= d; ++k) {
            x_pow_pk[k] = power(x_pow_pk[k - 1], Integer(p), f, p);
        }

        // Step 1: for each prime factor q of d, check gcd(x^{p^{d/q}} - x, f) = 1
        ModularPoly f_poly(f);
        for (int q : prime_factors) {
            int exp = d / q;
            auto diff = sub(x_pow_pk[exp], x_poly, p);
            auto g = gcd(diff, f_poly, p);
            if (g.degree() > 0) return false;
        }

        // Step 2: x^{p^d} ≡ x mod f
        auto diff_final = sub(x_pow_pk[d], x_poly, p);
        if (!diff_final.is_zero()) return false;

        return true;
    }

    /// Tonelli-Shanks square root in F_p[x]/f(x)
    /// Returns sqrt(a) if a is a square, zero polynomial otherwise
    [[nodiscard]] static ModularPoly sqrt_tonelli_shanks(
            const ModularPoly& a,
            const std::vector<uint64_t>& f,
            uint64_t p) {

        if (a.is_zero()) return ModularPoly();
        if (a.is_one()) return ModularPoly(1);

        int d = static_cast<int>(f.size()) - 1;

        // Characteristic 2: Frobenius x→x² is a bijection on F_{2^d}.
        // Inverse Frobenius gives sqrt: sqrt(a) = a^{2^{d-1}}.
        // Proof: (a^{2^{d-1}})² = a^{2^d} = a (by Fermat in F_{2^d}).
        if (p == 2) {
            assert(d >= 1 && "sqrt_tonelli_shanks: f must have degree >= 1");
            auto result = a;
            for (int i = 0; i < d - 1; ++i) {
                result = mul(result, result, f, p);
            }
            return result;
        }

        // Check if a is a square
        if (!is_square(a, f, p)) {
            return ModularPoly();  // Not a square
        }

        // Compute q and s where p^d - 1 = q * 2^s
        Integer pd(1);
        for (int i = 0; i < d; ++i) {
            pd *= Integer(p);
        }
        Integer q = pd.clone();
        q -= Integer(static_cast<int64_t>(1));

        uint64_t s = 0;
        while (!q.is_odd()) {
            mpz_tdiv_q_2exp(q.get_mpz(), q.get_mpz(), 1);
            s++;
        }

        // Find a non-square z in F_{p^d}.
        //
        // For EVEN d: ALL constants in F_p* are squares in F_{p^d}
        //   because (p-1) | (p^d-1)/2.  We must use polynomial non-squares.
        //   Bug history: the old loop reached i==p, creating ModularPoly(p)
        //   whose raw coefficient p is NOT detected as zero by is_zero()
        //   (which checks coeffs_.empty(), not reduction mod p).  This fake
        //   "non-square" is actually zero, making c = 0^q = 0 and the entire
        //   Tonelli-Shanks output collapse to zero.
        //
        // For ODD d: F_p non-squares remain non-squares in F_{p^d}, so the
        //   constant search z=2,3,... finds one in ~2 iterations.
        ModularPoly z;
        bool found_nonsq = false;

        if (d % 2 != 0) {
            // Odd d: search constants (cap at p-1 to avoid the zero-disguise bug)
            for (uint64_t i = 2; i < p; ++i) {
                z = ModularPoly(i);
                if (!is_square(z, f, p)) { found_nonsq = true; break; }
            }
        }

        if (!found_nonsq) {
            // Even d (or odd-d fallback): polynomial non-squares z = x + c.
            // ~50% of F_{p^d}* are non-squares, so expect ~2 trials.
            for (uint64_t c = 0; c < p; ++c) {
                z = ModularPoly(std::vector<uint64_t>{c, 1});
                if (!is_square(z, f, p)) { found_nonsq = true; break; }
            }
        }

        if (!found_nonsq) {
            return ModularPoly();  // Should not happen for irreducible f
        }

        // Initialize
        Integer exp_m = q.clone();
        exp_m += Integer(static_cast<int64_t>(1));
        mpz_tdiv_q_2exp(exp_m.get_mpz(), exp_m.get_mpz(), 1);  // (q+1)/2

        auto c = power(z, q, f, p);
        auto t = power(a, q, f, p);
        auto r = power(a, exp_m, f, p);
        uint64_t m = s;

        while (!t.is_one()) {
            // Find least i such that t^(2^i) = 1
            uint64_t i = 1;
            auto t_pow = mul(t, t, f, p);
            while (!t_pow.is_one() && i < m) {
                t_pow = mul(t_pow, t_pow, f, p);
                i++;
            }

            if (i >= m) {
                return ModularPoly();  // Shouldn't happen if a is a square
            }

            // b = c^(2^(m-i-1))
            auto b = c;
            for (uint64_t j = 0; j < m - i - 1; ++j) {
                b = mul(b, b, f, p);
            }

            c = mul(b, b, f, p);
            t = mul(t, c, f, p);
            r = mul(r, b, f, p);
            m = i;
        }

        // Self-verification: r^2 ≡ a mod (f, p)
        auto r_sq = mul(r, r, f, p);
        for (int i = 0; i < d; ++i) {
            if (r_sq.coeff(i) % p != a.coeff(i) % p) {
                return ModularPoly();  // Verification failed
            }
        }
        return r;
    }

private:
    std::vector<uint64_t> coeffs_;

    void normalize() {
        while (!coeffs_.empty() && coeffs_.back() == 0) {
            coeffs_.pop_back();
        }
    }

    /// Modular multiplication (handles overflow)
    [[nodiscard]] static uint64_t mul_mod(uint64_t a, uint64_t b, uint64_t p) {
        __uint128_t prod = static_cast<__uint128_t>(a) * b;
        return static_cast<uint64_t>(prod % p);
    }

    /// Modular inverse using extended Euclidean algorithm
    [[nodiscard]] static uint64_t mod_inverse(uint64_t a, uint64_t p) {
        // Use __int128_t to avoid overflow for p > INT64_MAX
        __int128_t t = 0, new_t = 1;
        __int128_t r = static_cast<__int128_t>(p), new_r = static_cast<__int128_t>(a);

        while (new_r != 0) {
            __int128_t quotient = r / new_r;

            __int128_t temp_t = new_t;
            new_t = t - quotient * new_t;
            t = temp_t;

            __int128_t temp_r = new_r;
            new_r = r - quotient * new_r;
            r = temp_r;
        }

        if (t < 0) {
            t += static_cast<__int128_t>(p);
        }

        return static_cast<uint64_t>(t);
    }
};

} // namespace gnfs::sqrt
