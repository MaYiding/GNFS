#pragma once

#include "number_field.hpp"
#include "modular_poly.hpp"
#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"

#include <vector>
#include <optional>
#include <iostream>
#include <thread>

namespace gnfs {
namespace sqrt {

using core::Integer;
using core::PolynomialContext;

/// Hensel lifting algebraic square root
///
/// Computes algebraic sqrt by:
/// 1. Finding sqrt mod (f, p) for a single inert prime p
/// 2. Hensel lifting to mod (f, p^{2^k}) until precision suffices
/// 3. Evaluating at m mod N to get the integer result
///
/// This avoids the CRT sign determination problem entirely.
class HenselSqrt {
public:
    struct Config {
        uint64_t prime_start = 1000;   // Starting prime search
        size_t extra_precision = 200;  // Extra bits of precision beyond estimate
        bool verbose = false;
    };

    HenselSqrt() = default;
    explicit HenselSqrt(const Config& config) : config_(config) {}

    /// Compute algebraic square root
    [[nodiscard]] std::optional<NumberFieldElement> compute(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf) const {

        if (ab_pairs.empty()) return nf.one();

        uint32_t d = nf.degree();
        const Integer& n = nf.n();

        // Step 1: Find a suitable prime p where f is irreducible mod p
        uint64_t p = find_inert_prime(nf);
        if (p == 0) return std::nullopt;

        // Step 2: Compute product mod (f, p)
        auto f_mod_p = get_f_mod_p(nf, p);
        ModularPoly product_mod_p(1);
        for (const auto& [a, b] : ab_pairs) {
            std::vector<uint64_t> cs(2);
            int64_t am = a % static_cast<int64_t>(p);
            if (am < 0) am += static_cast<int64_t>(p);
            cs[0] = static_cast<uint64_t>(am);
            cs[1] = (p - (b % p)) % p;
            product_mod_p = ModularPoly::mul(product_mod_p, ModularPoly(std::move(cs)), f_mod_p, p);
        }

        if (product_mod_p.is_zero()) return std::nullopt;

        // Step 3: Compute sqrt mod (f, p)
        if (!ModularPoly::is_square(product_mod_p, f_mod_p, p)) {
            return std::nullopt;
        }
        auto sqrt_mod_p = ModularPoly::sqrt_tonelli_shanks(product_mod_p, f_mod_p, p);

        // Step 4: Estimate required precision
        // Product of |a - b·α| for each relation; sqrt is half the log
        double log_bound = 0;
        for (const auto& [a, b] : ab_pairs) {
            double val = std::abs(static_cast<double>(a)) +
                         static_cast<double>(b) * std::abs(nf.m().to_double());
            log_bound += std::log2(std::max(val, 1.0));
        }
        double sqrt_log_bound = log_bound / 2.0 + config_.extra_precision;
        double log_p = std::log2(static_cast<double>(p));
        size_t num_lifts = 0;
        double current_precision = log_p;
        while (current_precision < sqrt_log_bound) {
            current_precision *= 2;
            ++num_lifts;
        }

        if (config_.verbose) {
            std::cerr << "[Hensel] p=" << p << " lifts=" << num_lifts
                      << " target_bits=" << static_cast<size_t>(sqrt_log_bound) << "\n";
        }

        // Step 5: Convert to Integer polynomial and Hensel lift
        std::vector<Integer> S(d);
        for (uint32_t i = 0; i < d; ++i) {
            S[i] = Integer(static_cast<int64_t>(
                (i <= static_cast<uint32_t>(sqrt_mod_p.degree())) ? sqrt_mod_p.coeff(i) : 0));
        }

        // Get f polynomial as Integer vector
        std::vector<Integer> f_int(d + 1);
        for (uint32_t i = 0; i <= d; ++i) {
            f_int[i] = nf.coeff(i).clone();
        }

        Integer modulus(static_cast<int64_t>(p));

        // Compute T₀ = (2·S₀)^{-1} mod (f, p) using Fermat
        // In F_{p^d}: a^{-1} = a^{p^d - 2}
        std::vector<Integer> T(d);
        {
            std::vector<uint64_t> two_s_mod(d), f_mod_p_vec(d + 1);
            for (uint32_t i = 0; i < d; ++i) {
                uint64_t si = S[i].to_uint64();
                two_s_mod[i] = (2 * si) % p;
            }
            for (uint32_t i = 0; i <= d; ++i) {
                Integer c = nf.coeff(i).clone();
                c %= Integer(p);
                if (c.is_negative()) c += Integer(p);
                f_mod_p_vec[i] = c.to_uint64();
            }
            // Compute p^d - 2 using Integer to avoid uint64 overflow for large p/d
            Integer q_minus_2(int64_t(1));
            for (uint32_t i = 0; i < d; ++i) q_minus_2 *= Integer(p);
            q_minus_2 -= Integer(int64_t(2));
            auto inv_mp = ModularPoly::power(ModularPoly(two_s_mod), q_minus_2, f_mod_p_vec, p);
            for (uint32_t i = 0; i < d; ++i) {
                uint64_t coeff_val = (i <= static_cast<uint32_t>(inv_mp.degree())) ? inv_mp.coeff(i) : 0;
                T[i] = Integer(coeff_val);
            }
        }

        // Pre-compute product at final precision ONCE.
        // Key optimization: instead of recomputing ∏(a_i - b_i·x) at every
        // lift level (O(num_lifts × n) poly muls), compute it once at the
        // maximum precision, then reduce coefficients for each lift level.
        std::vector<Integer> P_final;
        if (num_lifts > 0) {
            Integer final_mod(static_cast<int64_t>(p));
            for (size_t i = 0; i < num_lifts; ++i) {
                Integer temp = final_mod.clone();
                final_mod *= temp;
            }

            if (config_.verbose) {
                std::cerr << "[Hensel] Pre-computing product (" << ab_pairs.size()
                          << " factors, " << final_mod.bit_length() << "-bit modulus)...\n";
            }

            P_final = compute_product_mod_parallel(
                ab_pairs, f_int, d, final_mod, config_.verbose);

            if (config_.verbose) {
                std::cerr << "[Hensel] Product pre-computed\n";
            }
        }

        // Hensel lifting: maintain S and T = (2S)^{-1} in parallel
        // At each step, modulus → modulus², and:
        //   S' = S + T · (P - S²) mod (f, modulus²)
        //   T' = T · (2 - 2S'·T) mod (f, modulus²)
        for (size_t lift = 0; lift < num_lifts; ++lift) {
            Integer new_modulus = modulus.clone();
            new_modulus *= modulus;  // modulus²

            // Reduce pre-computed product to current precision
            std::vector<Integer> P(d);
            for (uint32_t i = 0; i < d; ++i) {
                P[i] = P_final[i].clone();
                P[i] %= new_modulus;
            }

            // S² mod (f, new_modulus)
            auto S2 = poly_mul_mod(S, S, f_int, d, new_modulus);

            // residual = P - S²
            auto residual = poly_sub_mod(P, S2, new_modulus);

            // correction = T · residual mod (f, new_modulus)
            auto correction = poly_mul_mod(T, residual, f_int, d, new_modulus);

            // S' = S + correction
            for (uint32_t i = 0; i < d; ++i) {
                S[i] += correction[i];
                S[i] %= new_modulus;
                if (S[i].is_negative()) S[i] += new_modulus;
            }

            // Update T: T' = T · (2 - 2S'·T) mod (f, new_modulus)
            // First compute 2S'·T
            std::vector<Integer> two_S_prime(d);
            for (uint32_t i = 0; i < d; ++i) {
                two_S_prime[i] = S[i].clone();
                two_S_prime[i] *= Integer(int64_t(2));
                two_S_prime[i] %= new_modulus;
            }
            auto two_S_T = poly_mul_mod(two_S_prime, T, f_int, d, new_modulus);

            // 2 - 2S'·T
            std::vector<Integer> factor(d);
            factor[0] = Integer(int64_t(2));
            factor[0] -= two_S_T[0];
            factor[0] %= new_modulus;
            if (factor[0].is_negative()) factor[0] += new_modulus;
            for (uint32_t i = 1; i < d; ++i) {
                factor[i] = two_S_T[i].clone();
                factor[i].negate();
                factor[i] %= new_modulus;
                if (factor[i].is_negative()) factor[i] += new_modulus;
            }

            // T' = T · factor
            T = poly_mul_mod(T, factor, f_int, d, new_modulus);

            modulus = std::move(new_modulus);

            if (config_.verbose && (lift == 0 || lift == num_lifts - 1 ||
                                    (num_lifts > 10 && lift % (num_lifts / 4) == 0))) {
                std::cerr << "[Hensel] lift " << lift << "/" << num_lifts
                          << " modulus_bits=" << modulus.bit_length() << "\n";
            }
        }

        // Step 6: Center coefficients and reduce mod N
        Integer half_mod = modulus.clone();
        mpz_tdiv_q_2exp(half_mod.get_mpz(), half_mod.get_mpz(), 1);

        std::vector<Integer> result_coeffs(d);
        for (uint32_t i = 0; i < d; ++i) {
            result_coeffs[i] = S[i].clone();
            if (result_coeffs[i].compare(half_mod) > 0) {
                result_coeffs[i] -= modulus;
            }
            result_coeffs[i] %= n;
            if (result_coeffs[i].is_negative()) result_coeffs[i] += n;
        }

        return NumberFieldElement(std::move(result_coeffs));
    }

private:
    Config config_;

    /// Find a prime p where f(x) is irreducible mod p
    [[nodiscard]] uint64_t find_inert_prime(const NumberField& nf) const {
        uint64_t p = config_.prime_start;

        for (size_t attempts = 0; attempts < 100000; ++attempts) {
            p = next_prime(p);

            // Check f irreducible mod p (full Rabin test)
            auto f_mod = get_f_mod_p(nf, p);
            if (f_mod.back() == 0) continue;

            if (ModularPoly::is_irreducible(f_mod, p)) {
                return p;
            }
        }
        return 0;
    }

    /// Get f(x) coefficients mod p
    [[nodiscard]] static std::vector<uint64_t> get_f_mod_p(const NumberField& nf, uint64_t p) {
        uint32_t d = nf.degree();
        std::vector<uint64_t> f(d + 1);
        for (uint32_t i = 0; i <= d; ++i) {
            Integer c = nf.coeff(i).clone();
            c %= Integer(p);
            if (c.is_negative()) c += Integer(p);
            f[i] = c.to_uint64();
        }
        return f;
    }

    /// Compute ∏(a_i - b_i·x) mod (f, modulus) using Integer polynomial arithmetic
    [[nodiscard]] static std::vector<Integer> compute_product_mod(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const std::vector<Integer>& f,
            uint32_t d,
            const Integer& modulus) {

        // Start with 1
        std::vector<Integer> product(d);
        product[0] = Integer(int64_t(1));
        for (uint32_t i = 1; i < d; ++i) product[i] = Integer(int64_t(0));

        for (const auto& [a, b] : ab_pairs) {
            // Factor = a - b·x
            std::vector<Integer> factor(d);
            Integer a_mod(a);
            a_mod %= modulus;
            if (a_mod.is_negative()) a_mod += modulus;
            factor[0] = std::move(a_mod);

            if (d > 1) {
                Integer neg_b(static_cast<int64_t>(b));
                neg_b.negate();
                neg_b %= modulus;
                if (neg_b.is_negative()) neg_b += modulus;
                factor[1] = std::move(neg_b);
            }
            for (uint32_t i = 2; i < d; ++i) factor[i] = Integer(int64_t(0));

            product = poly_mul_mod(product, factor, f, d, modulus);
        }

        return product;
    }

    /// Parallel product computation: splits factors across threads
    /// then combines partial products. Falls back to sequential for small n.
    [[nodiscard]] static std::vector<Integer> compute_product_mod_parallel(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const std::vector<Integer>& f,
            uint32_t d,
            const Integer& modulus,
            bool verbose = false) {

        size_t n = ab_pairs.size();

        // Determine thread count
        unsigned hw = std::thread::hardware_concurrency();
        size_t num_threads = (hw > 0) ? hw : 4;
        if (n < num_threads * 100) num_threads = 1;

        if (num_threads <= 1) {
            return compute_product_mod(ab_pairs, f, d, modulus);
        }

        if (verbose) {
            std::cerr << "[Hensel] Parallel product: " << n << " factors, "
                      << num_threads << " threads\n";
        }

        size_t chunk = (n + num_threads - 1) / num_threads;
        size_t actual_threads = (n + chunk - 1) / chunk;

        std::vector<std::vector<Integer>> partials(actual_threads);
        std::vector<std::thread> threads;
        threads.reserve(actual_threads);

        for (size_t t = 0; t < actual_threads; ++t) {
            size_t start = t * chunk;
            size_t end = std::min(start + chunk, n);

            threads.emplace_back([&partials, &ab_pairs, &f, &modulus, d, t, start, end]() {
                std::vector<Integer> product(d);
                product[0] = Integer(int64_t(1));
                for (uint32_t i = 1; i < d; ++i) product[i] = Integer(int64_t(0));

                for (size_t j = start; j < end; ++j) {
                    auto [a, b] = ab_pairs[j];
                    std::vector<Integer> factor(d);
                    Integer a_mod(a);
                    a_mod %= modulus;
                    if (a_mod.is_negative()) a_mod += modulus;
                    factor[0] = std::move(a_mod);

                    if (d > 1) {
                        Integer neg_b(static_cast<int64_t>(b));
                        neg_b.negate();
                        neg_b %= modulus;
                        if (neg_b.is_negative()) neg_b += modulus;
                        factor[1] = std::move(neg_b);
                    }
                    for (uint32_t i = 2; i < d; ++i) factor[i] = Integer(int64_t(0));

                    product = poly_mul_mod(product, factor, f, d, modulus);
                }

                partials[t] = std::move(product);
            });
        }

        for (auto& th : threads) th.join();

        // Combine partial products sequentially
        auto result = std::move(partials[0]);
        for (size_t t = 1; t < actual_threads; ++t) {
            result = poly_mul_mod(result, partials[t], f, d, modulus);
        }

        return result;
    }

    /// Polynomial multiplication mod (f, modulus)
    /// Both inputs have degree < d, result has degree < d
    [[nodiscard]] static std::vector<Integer> poly_mul_mod(
            const std::vector<Integer>& a,
            const std::vector<Integer>& b,
            const std::vector<Integer>& f,
            uint32_t d,
            const Integer& modulus) {

        // Multiply: result has degree up to 2d-2
        std::vector<Integer> result(2 * d - 1);
        for (size_t i = 0; i < result.size(); ++i) result[i] = Integer(int64_t(0));

        for (uint32_t i = 0; i < d; ++i) {
            if (a[i].is_zero()) continue;
            for (uint32_t j = 0; j < d; ++j) {
                if (b[j].is_zero()) continue;
                Integer term = a[i].clone();
                term *= b[j];
                term %= modulus;
                result[i + j] += term;
                result[i + j] %= modulus;
            }
        }

        // Reduce mod f: for each degree >= d, subtract (lead/f[d]) * f[i]
        // Handles non-monic f via modular inverse of leading coefficient
        Integer f_lead_inv(int64_t(1));
        {
            Integer f_d = f[d].clone();
            f_d %= modulus;
            if (f_d.is_negative()) f_d += modulus;
            if (!f_d.is_one()) {
                int ok = mpz_invert(f_lead_inv.get_mpz(), f_d.get_mpz(),
                                    modulus.get_mpz());
                // f_d must be invertible mod modulus for Hensel lifting.
                // For Hensel, modulus = p^k where p doesn't divide f_d
                // (ensured by find_inert_prime checking f_mod.back() != 0).
                assert(ok && "f[d] must be invertible mod modulus in poly_mul_mod");
                (void)ok;
            }
        }

        for (int k = static_cast<int>(2 * d - 2); k >= static_cast<int>(d); --k) {
            Integer lead = std::move(result[k]);
            result[k] = Integer(int64_t(0));
            if (lead.is_zero()) continue;

            // Scale by inverse of leading coefficient
            Integer lead_scaled = lead.clone();
            if (!f_lead_inv.is_one()) {
                lead_scaled *= f_lead_inv;
                lead_scaled %= modulus;
            }

            // Subtract lead_scaled * f[0..d-1] from result[k-d..k-1]
            for (uint32_t i = 0; i < d; ++i) {
                Integer sub = lead_scaled.clone();
                sub *= f[i];
                sub %= modulus;
                result[k - d + i] -= sub;
                result[k - d + i] %= modulus;
                if (result[k - d + i].is_negative()) result[k - d + i] += modulus;
            }
        }

        // Trim to d coefficients
        result.resize(d);
        return result;
    }

    /// Polynomial subtraction mod modulus
    [[nodiscard]] static std::vector<Integer> poly_sub_mod(
            const std::vector<Integer>& a,
            const std::vector<Integer>& b,
            const Integer& modulus) {

        size_t n = std::max(a.size(), b.size());
        std::vector<Integer> result(n);
        for (size_t i = 0; i < n; ++i) {
            result[i] = (i < a.size()) ? a[i].clone() : Integer(int64_t(0));
            if (i < b.size()) {
                result[i] -= b[i];
                result[i] %= modulus;
                if (result[i].is_negative()) result[i] += modulus;
            }
        }
        return result;
    }

    /// Compute polynomial inverse mod (f, new_modulus)
    /// Given that we know the inverse mod (f, old_modulus), lift it
    [[nodiscard]] static std::vector<Integer> poly_inverse_mod(
            const std::vector<Integer>& a,
            const std::vector<Integer>& f,
            uint32_t d,
            const Integer& old_modulus,
            const Integer& new_modulus) {

        // First compute inverse mod old_modulus using extended GCD
        // For small modulus, use brute force or direct computation
        auto inv = poly_inverse_mod_direct(a, f, d, old_modulus);
        if (inv.empty()) return inv;

        // Newton lifting: inv' = 2·inv - a·inv² mod (f, new_modulus)
        auto a_inv2 = poly_mul_mod(a, inv, f, d, new_modulus);
        // a·inv should be ≡ 1 mod old_modulus
        // Compute 2·inv - a·inv·inv = inv·(2 - a·inv) mod (f, new_modulus)

        // t = a · inv mod (f, new_modulus)
        auto t = poly_mul_mod(a, inv, f, d, new_modulus);

        // 2 - t
        std::vector<Integer> two_minus_t(d);
        two_minus_t[0] = Integer(int64_t(2));
        two_minus_t[0] -= t[0];
        two_minus_t[0] %= new_modulus;
        if (two_minus_t[0].is_negative()) two_minus_t[0] += new_modulus;
        for (uint32_t i = 1; i < d; ++i) {
            two_minus_t[i] = t[i].clone();
            two_minus_t[i].negate();
            two_minus_t[i] %= new_modulus;
            if (two_minus_t[i].is_negative()) two_minus_t[i] += new_modulus;
        }

        // result = inv · (2 - t) mod (f, new_modulus)
        return poly_mul_mod(inv, two_minus_t, f, d, new_modulus);
    }

    /// Compute polynomial inverse mod (f, modulus) directly
    /// Uses the fact that (Z/mZ)[x]/(f) is a ring and we need a^{-1}
    [[nodiscard]] static std::vector<Integer> poly_inverse_mod_direct(
            const std::vector<Integer>& a,
            const std::vector<Integer>& f,
            uint32_t d,
            const Integer& modulus) {

        // For small modulus (fits uint64), use ModularPoly inverse
        if (modulus.fits_uint64()) {
            uint64_t mod = modulus.to_uint64();
            std::vector<uint64_t> a_mod(d), f_mod(d + 1);
            for (uint32_t i = 0; i < d; ++i) {
                Integer c = a[i].clone();
                c %= modulus;
                if (c.is_negative()) c += modulus;
                a_mod[i] = c.to_uint64();
            }
            for (uint32_t i = 0; i <= d; ++i) {
                Integer c = f[i].clone();
                c %= modulus;
                if (c.is_negative()) c += modulus;
                f_mod[i] = c.to_uint64();
            }

            // Compute a^{q-2} mod (f, p) where q = p^d (Fermat's little theorem in F_{p^d})
            // For the inverse: a^{-1} = a^{p^d - 2} mod (f, p)
            // Use Integer to avoid uint64_t overflow when p^d > 2^64 (e.g. p=2000, d=6)
            Integer q_minus_2(1);
            for (uint32_t i = 0; i < d; ++i) q_minus_2 *= Integer(static_cast<uint64_t>(mod));
            q_minus_2 -= Integer(2);

            ModularPoly ap(a_mod);
            auto inv_mp = ModularPoly::power(ap, q_minus_2, f_mod, mod);

            std::vector<Integer> result(d);
            for (uint32_t i = 0; i < d; ++i) {
                result[i] = Integer(static_cast<int64_t>(
                    (i <= static_cast<uint32_t>(inv_mp.degree())) ? inv_mp.coeff(i) : 0));
            }
            return result;
        }

        // For large modulus, we'd need extended polynomial GCD
        // For now, return empty (failure)
        return {};
    }

    /// Find next prime
    [[nodiscard]] static uint64_t next_prime(uint64_t n) {
        n++;
        if (n <= 2) return 2;
        if (n % 2 == 0) n++;
        while (true) {
            bool is_p = true;
            for (uint64_t i = 3; i * i <= n; ++i) {
                if (n % i == 0) { is_p = false; break; }
            }
            if (is_p) return n;
            n += 2;
        }
    }
};

} // namespace sqrt
} // namespace gnfs
