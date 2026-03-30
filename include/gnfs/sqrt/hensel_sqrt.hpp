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
        uint64_t cached_inert_prime = 0; // Pre-found inert prime (0 = auto-find)
        bool verbose = false;
    };

    HenselSqrt() = default;
    explicit HenselSqrt(const Config& config) : config_(config) {}

    /// Get the inert prime found/used during the last compute() call
    [[nodiscard]] uint64_t last_inert_prime() const noexcept { return last_inert_prime_; }

    /// Compute algebraic square root value (mod N)
    ///
    /// Uses multi-prime CRT (Nguyen 2004) for large inputs:
    /// compute sqrt(P·f'(α)²) independently mod many small primes,
    /// then reconstruct via CRT. Falls back to Hensel lifting for small inputs
    /// or if CRT fails.
    [[nodiscard]] std::optional<Integer> compute(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf) const {

        if (ab_pairs.empty()) return Integer(int64_t(1));

        const Integer& n = nf.n();

        // Estimate required precision for the sqrt coefficients
        double target_bits = estimate_target_bits(ab_pairs, nf);

        // Pre-compute verification product P(m) = ∏(a_i - b_i*m) mod N
        Integer product_at_m = compute_product_at_m(ab_pairs, nf);

        // Compute f'(m) mod N and its inverse
        Integer f_prime_m = evaluate_f_derivative_at_m(nf);
        Integer f_prime_m_inv;
        {
            int ok = mpz_invert(f_prime_m_inv.get_mpz(), f_prime_m.get_mpz(), n.get_mpz());
            if (!ok) {
                if (config_.verbose) {
                    std::cerr << "[Hensel] f'(m) not invertible mod N\n";
                }
                return std::nullopt;
            }
        }

        // Use multi-prime CRT for large inputs (≥100 factors)
        // For small inputs, Hensel lifting is fast regardless of precision.
        if (ab_pairs.size() >= 100) {
            auto result = compute_multi_prime_crt(
                ab_pairs, nf, target_bits, product_at_m, f_prime_m, f_prime_m_inv);
            if (result) return result;
            if (config_.verbose) {
                std::cerr << "[Hensel] Multi-prime CRT failed, falling back to Hensel lifting\n";
            }
        }

        // Fallback: classic Hensel lifting
        return compute_hensel_lifting(
            ab_pairs, nf, target_bits, product_at_m, f_prime_m, f_prime_m_inv);
    }

private:
    Config config_;
    mutable uint64_t last_inert_prime_ = 0;

    /// Estimate target bits for sqrt coefficient recovery
    [[nodiscard]] double estimate_target_bits(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf) const {
        uint32_t d = nf.degree();
        double max_root = std::abs(nf.m().to_double());
        {
            double c_d_abs = std::abs(nf.coeff(d).to_double());
            if (c_d_abs > 0) {
                for (uint32_t i = 0; i < d; ++i) {
                    double ratio = std::abs(nf.coeff(i).to_double()) / c_d_abs;
                    max_root = std::max(max_root, 1.0 + ratio);
                }
            }
        }
        double log_bound = 0;
        for (const auto& [a, b] : ab_pairs) {
            double val = std::abs(static_cast<double>(a)) +
                         static_cast<double>(b) * max_root;
            log_bound += std::log2(std::max(val, 1.0));
        }
        double log_f_prime_bound = std::log2(static_cast<double>(d));
        if (d > 1) log_f_prime_bound += static_cast<double>(d) * std::log2(max_root + 1.0);

        return log_bound / 2.0 + log_f_prime_bound
               + std::log2(static_cast<double>(d))
               + config_.extra_precision;
    }

    /// Compute ∏(a_i - b_i*m) mod N
    [[nodiscard]] static Integer compute_product_at_m(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf) {
        const Integer& n = nf.n();
        Integer product(int64_t(1));
        for (const auto& [a, b] : ab_pairs) {
            Integer factor(a);
            Integer bm = nf.m().clone();
            bm *= Integer(static_cast<int64_t>(b));
            factor -= bm;
            factor %= n;
            if (factor.is_negative()) factor += n;
            product *= factor;
            product %= n;
        }
        return product;
    }

    /// Verify Y and return it or -Y if verification passes
    [[nodiscard]] static std::optional<Integer> verify_and_return(
            const Integer& Y, const Integer& product_at_m, const Integer& n) {
        Integer Y2 = Y.clone();
        Y2 *= Y;
        Y2 %= n;
        if (Y2.is_negative()) Y2 += n;

        Integer pm_pos = product_at_m.clone();
        if (pm_pos.is_negative()) pm_pos += n;

        if (Y2.compare(pm_pos) == 0) return Y.clone();

        // Check -Y
        Integer neg_Y = n.clone();
        neg_Y -= Y;
        Integer neg_Y2 = neg_Y.clone();
        neg_Y2 *= neg_Y;
        neg_Y2 %= n;
        if (neg_Y2.is_negative()) neg_Y2 += n;

        if (neg_Y2.compare(pm_pos) == 0) return neg_Y;

        return std::nullopt;
    }

    // ========================================================================
    // Multi-prime CRT approach (Nguyen 2004)
    // ========================================================================

    /// Compute sqrt via multi-prime CRT:
    /// 1. Find k inert primes p_1,...,p_k with ∏p_i > 2·coeff_bound
    /// 2. For each p_i: compute sqrt(P·f'(α)²) mod (f, p_i)
    /// 3. CRT-combine each coefficient index across all primes
    /// 4. Center and evaluate at m mod N
    [[nodiscard]] std::optional<Integer> compute_multi_prime_crt(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf,
            double target_bits,
            const Integer& product_at_m,
            const Integer& /* f_prime_m */,
            const Integer& f_prime_m_inv) const {

        uint32_t d = nf.degree();
        const Integer& n = nf.n();

        // We need ∏p_i > 2^{target_bits + 1} (factor 2 for centering)
        double required_bits = target_bits + 1;
        // Use primes starting from 2^20 (~1M) to balance count vs per-prime work.
        // Larger primes = fewer needed but Tonelli-Shanks slightly slower.
        // 20-bit primes: need ~required_bits/20 primes.
        const uint64_t prime_start = (1ULL << 20); // ~1M
        const double bits_per_prime = 20.0;
        size_t num_primes_needed = static_cast<size_t>(std::ceil(required_bits / bits_per_prime)) + 10;

        if (config_.verbose) {
            std::cerr << "[CRT] target=" << static_cast<size_t>(target_bits) << " bits, need ~"
                      << num_primes_needed << " primes\n";
        }

        // Find inert primes
        std::vector<uint64_t> inert_primes;
        inert_primes.reserve(num_primes_needed);
        {
            uint64_t p = prime_start;
            while (inert_primes.size() < num_primes_needed) {
                p = next_prime(p);
                if (p == 0) break;
                auto f_mod = get_f_mod_p(nf, p);
                if (f_mod.back() == 0) continue;
                if (ModularPoly::is_irreducible(f_mod, p)) {
                    inert_primes.push_back(p);
                }
            }
        }

        if (inert_primes.size() < num_primes_needed) {
            if (config_.verbose) {
                std::cerr << "[CRT] Only found " << inert_primes.size()
                          << "/" << num_primes_needed << " inert primes\n";
            }
            return std::nullopt;
        }

        if (!inert_primes.empty()) {
            last_inert_prime_ = inert_primes[0];
        }

        if (config_.verbose) {
            std::cerr << "[CRT] Found " << inert_primes.size() << " inert primes in ["
                      << inert_primes.front() << ", " << inert_primes.back() << "]\n";
        }

        // Per-prime computation: product + f'(α)² + sqrt, all in F_p[x]/(f)
        // Store d residues per prime
        struct PrimeResult {
            uint64_t p;
            std::vector<uint64_t> sqrt_coeffs; // d values, each < p
        };
        std::vector<PrimeResult> results(inert_primes.size());

        // Parallel computation across primes
        unsigned hw = std::thread::hardware_concurrency();
        size_t num_threads = (hw > 0) ? hw : 4;
        if (inert_primes.size() < num_threads * 4) num_threads = 1;

        std::atomic<size_t> failures{0};
        std::vector<std::thread> threads;
        size_t chunk = (inert_primes.size() + num_threads - 1) / num_threads;

        for (size_t t = 0; t < num_threads; ++t) {
            size_t start = t * chunk;
            size_t end = std::min(start + chunk, inert_primes.size());
            if (start >= end) break;

            threads.emplace_back([&, start, end]() {
                for (size_t idx = start; idx < end; ++idx) {
                    uint64_t p = inert_primes[idx];
                    auto res = compute_sqrt_at_prime(p, ab_pairs, nf, d);
                    if (res.empty()) {
                        failures.fetch_add(1);
                        results[idx].p = 0; // mark failed
                    } else {
                        results[idx].p = p;
                        results[idx].sqrt_coeffs = std::move(res);
                    }
                }
            });
        }
        for (auto& th : threads) th.join();

        // Collect successful results
        std::vector<PrimeResult> good_results;
        good_results.reserve(results.size());
        for (auto& r : results) {
            if (r.p != 0) good_results.push_back(std::move(r));
        }

        if (config_.verbose) {
            std::cerr << "[CRT] " << good_results.size() << " successful, "
                      << failures.load() << " failed\n";
        }

        // Check if we have enough bits
        double total_bits = 0;
        for (const auto& r : good_results) {
            total_bits += std::log2(static_cast<double>(r.p));
        }
        if (total_bits < required_bits) {
            if (config_.verbose) {
                std::cerr << "[CRT] Insufficient precision: " << static_cast<size_t>(total_bits)
                          << " < " << static_cast<size_t>(required_bits) << " bits\n";
            }
            return std::nullopt;
        }

        // CRT combine for each coefficient
        std::vector<Integer> S_prime(d); // S' = f'(α)·sqrt(P) coefficients
        for (uint32_t ci = 0; ci < d; ++ci) {
            // Incremental CRT: accumulate residues
            Integer combined(static_cast<int64_t>(good_results[0].sqrt_coeffs[ci]));
            Integer modulus(static_cast<uint64_t>(good_results[0].p));

            for (size_t i = 1; i < good_results.size(); ++i) {
                uint64_t p = good_results[i].p;
                uint64_t r_i = good_results[i].sqrt_coeffs[ci];

                // CRT step: find x ≡ combined (mod modulus), x ≡ r_i (mod p)
                // x = combined + modulus · t, where t = (r_i - combined) · modulus^{-1} mod p
                Integer c_mod_p = combined.clone();
                c_mod_p %= Integer(p);
                if (c_mod_p.is_negative()) c_mod_p += Integer(p);

                int64_t diff = static_cast<int64_t>(r_i) - static_cast<int64_t>(c_mod_p.to_uint64());
                if (diff < 0) diff += static_cast<int64_t>(p);

                // modulus^{-1} mod p (using uint64 arithmetic)
                Integer m_mod_p = modulus.clone();
                m_mod_p %= Integer(p);
                if (m_mod_p.is_negative()) m_mod_p += Integer(p);

                Integer m_inv(int64_t(0));
                mpz_invert(m_inv.get_mpz(), m_mod_p.get_mpz(), Integer(p).get_mpz());

                uint64_t t_val = 0;
                {
                    Integer t = Integer(static_cast<int64_t>(diff));
                    t *= m_inv;
                    t %= Integer(p);
                    if (t.is_negative()) t += Integer(p);
                    t_val = t.to_uint64();
                }

                // combined += modulus * t
                if (t_val != 0) {
                    Integer step = modulus.clone();
                    step *= Integer(t_val);
                    combined += step;
                }
                modulus *= Integer(p);
            }

            // Center: if combined > modulus/2, subtract modulus
            Integer half = modulus.clone();
            mpz_tdiv_q_2exp(half.get_mpz(), half.get_mpz(), 1);
            if (combined.compare(half) > 0) {
                combined -= modulus;
            }

            S_prime[ci] = std::move(combined);
        }

        if (config_.verbose) {
            size_t max_bits = 0;
            for (uint32_t i = 0; i < d; ++i) {
                size_t b = S_prime[i].bit_length();
                max_bits = std::max(max_bits, b);
            }
            std::cerr << "[CRT] Recovered S' coefficients, max bits=" << max_bits << "\n";
        }

        // Evaluate S'(m) mod N
        NumberFieldElement result_elem(std::move(S_prime));
        Integer Y_prime = nf.evaluate_at_m_mod_n(result_elem);

        // Divide by f'(m) to recover Y = sqrt(P)(m) mod N
        Integer Y = Y_prime.clone();
        Y *= f_prime_m_inv;
        Y %= n;
        if (Y.is_negative()) Y += n;

        // Verify
        auto verified = verify_and_return(Y, product_at_m, n);
        if (verified) {
            if (config_.verbose) {
                std::cerr << "[CRT] Verification PASSED\n";
            }
            return verified;
        }

        if (config_.verbose) {
            std::cerr << "[CRT] Verification FAILED\n";
        }
        return std::nullopt;
    }

    /// Compute sqrt(P·f'(α)²) mod (f, p) for a single prime, all in F_p[x]/(f)
    /// Returns d coefficients or empty on failure
    [[nodiscard]] static std::vector<uint64_t> compute_sqrt_at_prime(
            uint64_t p,
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf,
            uint32_t d) {

        auto f_mod_p = get_f_mod_p(nf, p);

        // Product: ∏(a - b·α) mod (f, p)
        ModularPoly product(1);
        for (const auto& [a, b] : ab_pairs) {
            std::vector<uint64_t> cs(2);
            int64_t am = a % static_cast<int64_t>(p);
            if (am < 0) am += static_cast<int64_t>(p);
            cs[0] = static_cast<uint64_t>(am);
            cs[1] = (p - (b % p)) % p;
            product = ModularPoly::mul(product, ModularPoly(std::move(cs)), f_mod_p, p);
        }

        if (product.is_zero()) return {};

        // Multiply by f'(α)²
        auto f_prime = compute_f_derivative_mod_p(nf, p);
        auto f_prime_sq = ModularPoly::mul(
            ModularPoly(f_prime), ModularPoly(f_prime), f_mod_p, p);
        product = ModularPoly::mul(product, f_prime_sq, f_mod_p, p);

        if (product.is_zero()) return {};

        // Check if it's a square
        if (!ModularPoly::is_square(product, f_mod_p, p)) {
            return {};
        }

        // Compute sqrt
        auto sq = ModularPoly::sqrt_tonelli_shanks(product, f_mod_p, p);

        // Extract d coefficients
        std::vector<uint64_t> coeffs(d);
        for (uint32_t i = 0; i < d; ++i) {
            coeffs[i] = sq.coeff(i);
        }
        return coeffs;
    }

    // ========================================================================
    // Classic Hensel lifting fallback
    // ========================================================================

    /// Classic Hensel lifting approach (fallback for small inputs or CRT failure)
    [[nodiscard]] std::optional<Integer> compute_hensel_lifting(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf,
            double target_bits,
            const Integer& product_at_m,
            const Integer& /* f_prime_m */,
            const Integer& f_prime_m_inv) const {

        uint32_t d = nf.degree();
        const Integer& n = nf.n();

        // Find inert prime
        uint64_t p = config_.cached_inert_prime;
        if (p == 0) p = find_inert_prime(nf);
        if (p == 0) return std::nullopt;
        last_inert_prime_ = p;

        // Compute product and sqrt mod (f, p)
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

        if (!ModularPoly::is_square(product_mod_p, f_mod_p, p)) return std::nullopt;
        auto sqrt_mod_p = ModularPoly::sqrt_tonelli_shanks(product_mod_p, f_mod_p, p);

        // Multiply by f'(α)
        auto f_prime_mod_p = compute_f_derivative_mod_p(nf, p);
        sqrt_mod_p = ModularPoly::mul(
            sqrt_mod_p, ModularPoly(f_prime_mod_p), f_mod_p, p);

        // Compute lifts needed
        double log_p = std::log2(static_cast<double>(p));
        size_t base_lifts = 0;
        {
            double cur = log_p;
            while (cur < target_bits) { cur *= 2; ++base_lifts; }
        }

        for (int attempt = 0; attempt < 4; ++attempt) {
            size_t num_lifts = base_lifts + static_cast<size_t>(attempt);

            if (config_.verbose) {
                double mod_bits = log_p;
                for (size_t l = 0; l < num_lifts; ++l) mod_bits *= 2;
                std::cerr << "[Hensel-lift] attempt=" << attempt << " p=" << p
                          << " lifts=" << num_lifts
                          << " modulus_bits~=" << static_cast<size_t>(mod_bits) << "\n";
            }

            auto result_elem = hensel_lift_and_extract(
                sqrt_mod_p, ab_pairs, nf, p, num_lifts, d);
            if (!result_elem) continue;

            Integer Y_prime = nf.evaluate_at_m_mod_n(*result_elem);
            Integer Y = Y_prime.clone();
            Y *= f_prime_m_inv;
            Y %= n;
            if (Y.is_negative()) Y += n;

            auto verified = verify_and_return(Y, product_at_m, n);
            if (verified) return verified;

            if (config_.verbose) {
                std::cerr << "[Hensel-lift] Verification FAILED (attempt " << attempt << ")\n";
            }
        }

        return std::nullopt;
    }

    /// Core Hensel lifting: given sqrt mod p, lift to target precision and extract result
    [[nodiscard]] std::optional<NumberFieldElement> hensel_lift_and_extract(
            const ModularPoly& sqrt_mod_p,
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf,
            uint64_t p,
            size_t num_lifts,
            uint32_t d) const {

        const Integer& n = nf.n();

        // Convert to Integer polynomial
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

            // Multiply P by f'(x)^2 to ensure sqrt ∈ Z[α] after lifting.
            auto f_prime_int = compute_f_derivative_int(f_int, d);
            auto fli_final = compute_f_lead_inv(f_int, d, final_mod);
            auto f_prime_sq = poly_mul_mod(f_prime_int, f_prime_int, f_int, d, final_mod, fli_final);
            P_final = poly_mul_mod(P_final, f_prime_sq, f_int, d, final_mod, fli_final);

            if (config_.verbose) {
                std::cerr << "[Hensel] Product pre-computed (with f'(α)^2 factor)\n";
            }
        }

        // Hensel lifting: maintain S and T = (2S)^{-1} in parallel
        for (size_t lift = 0; lift < num_lifts; ++lift) {
            Integer new_modulus = modulus.clone();
            new_modulus *= modulus;  // modulus²

            // Pre-compute f_lead_inv for this round (all 5 poly_mul_mod share it)
            auto fli = compute_f_lead_inv(f_int, d, new_modulus);

            // Reduce pre-computed product to current precision
            std::vector<Integer> P(d);
            for (uint32_t i = 0; i < d; ++i) {
                P[i] = P_final[i].clone();
                P[i] %= new_modulus;
            }

            // S² mod (f, new_modulus)
            auto S2 = poly_mul_mod(S, S, f_int, d, new_modulus, fli);

            // residual = P - S²
            auto residual = poly_sub_mod(P, S2, new_modulus);

            // correction = T · residual mod (f, new_modulus)
            auto correction = poly_mul_mod(T, residual, f_int, d, new_modulus, fli);

            // S' = S + correction
            for (uint32_t i = 0; i < d; ++i) {
                S[i] += correction[i];
                S[i] %= new_modulus;
                if (S[i].is_negative()) S[i] += new_modulus;
            }

            // Update T: T' = T · (2 - 2S'·T) mod (f, new_modulus)
            std::vector<Integer> two_S_prime(d);
            for (uint32_t i = 0; i < d; ++i) {
                two_S_prime[i] = S[i].clone();
                two_S_prime[i] *= Integer(int64_t(2));
                two_S_prime[i] %= new_modulus;
            }
            auto two_S_T = poly_mul_mod(two_S_prime, T, f_int, d, new_modulus, fli);

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
            T = poly_mul_mod(T, factor, f_int, d, new_modulus, fli);

            modulus = std::move(new_modulus);

            if (config_.verbose && (lift == 0 || lift == num_lifts - 1 ||
                                    (num_lifts > 10 && lift % (num_lifts / 4) == 0))) {
                std::cerr << "[Hensel] lift " << lift << "/" << num_lifts
                          << " modulus_bits=" << modulus.bit_length() << "\n";
            }
        }

        // Verify Hensel invariant: S^2 ≡ P mod (f, modulus)
        if (config_.verbose && num_lifts > 0) {
            auto S2_check = poly_mul_mod(S, S, f_int, d, modulus);
            bool lift_ok = true;
            for (uint32_t i = 0; i < d; ++i) {
                Integer p_i = P_final[i].clone();
                p_i %= modulus;
                if (S2_check[i].compare(p_i) != 0) {
                    lift_ok = false;
                    std::cerr << "[Hensel] INVARIANT VIOLATION: S^2[" << i
                              << "] != P[" << i << "] mod p^k\n";
                    std::cerr << "  S^2[" << i << "] bits=" << S2_check[i].bit_length()
                              << " P[" << i << "] bits=" << p_i.bit_length() << "\n";
                    break;
                }
            }
            if (lift_ok) {
                std::cerr << "[Hensel] Lift invariant OK: S^2 ≡ P mod (f, p^k)\n";
            }
        }

        // Center coefficients and reduce mod N
        Integer half_mod = modulus.clone();
        mpz_tdiv_q_2exp(half_mod.get_mpz(), half_mod.get_mpz(), 1);

        // Diagnostic: print pre-centering and pre-mod-N coefficient sizes
        if (config_.verbose) {
            size_t max_pre_center = 0, max_post_center = 0;
            bool any_centered = false;
            for (uint32_t i = 0; i < d; ++i) {
                max_pre_center = std::max(max_pre_center, S[i].bit_length());
                Integer centered = S[i].clone();
                if (centered.compare(half_mod) > 0) {
                    centered -= modulus;
                    any_centered = true;
                }
                max_post_center = std::max(max_post_center, centered.bit_length());
            }
            std::cerr << "[Hensel] pre-center max bits=" << max_pre_center
                      << " post-center max bits=" << max_post_center
                      << " (centered=" << (any_centered ? "yes" : "no") << ")\n";

            // Evaluate S(m) mod N using Hensel coefficients (before centering)
            const Integer& nn = nf.n();
            const Integer& mm = nf.m();
            Integer s_at_m(int64_t(0));
            for (int i = static_cast<int>(d) - 1; i >= 0; --i) {
                s_at_m *= mm;
                s_at_m += S[i];
                s_at_m %= nn;
            }
            if (s_at_m.is_negative()) s_at_m += nn;
            Integer s2_at_m = s_at_m.clone();
            s2_at_m *= s_at_m;
            s2_at_m %= nn;
            if (s2_at_m.is_negative()) s2_at_m += nn;

            // Evaluate P_final(m) mod N
            Integer p_at_m(int64_t(0));
            for (int i = static_cast<int>(d) - 1; i >= 0; --i) {
                p_at_m *= mm;
                p_at_m += P_final[i];
                p_at_m %= nn;
            }
            if (p_at_m.is_negative()) p_at_m += nn;

            std::cerr << "[Hensel] φ(S)^2 mod N == φ(P_final) mod N ? "
                      << (s2_at_m.compare(p_at_m) == 0 ? "YES" : "NO") << "\n";
        }

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

    /// Compute f'(x) mod p (derivative of the defining polynomial)
    [[nodiscard]] static std::vector<uint64_t> compute_f_derivative_mod_p(
            const NumberField& nf, uint64_t p) {
        uint32_t d = nf.degree();
        std::vector<uint64_t> f_prime(d);
        for (uint32_t i = 0; i < d; ++i) {
            Integer c = nf.coeff(i + 1).clone();
            c *= Integer(static_cast<int64_t>(i + 1));
            c %= Integer(p);
            if (c.is_negative()) c += Integer(p);
            f_prime[i] = c.to_uint64();
        }
        return f_prime;
    }

    /// Compute f'(x) as Integer polynomial (d coefficients, degree d-1)
    [[nodiscard]] static std::vector<Integer> compute_f_derivative_int(
            const std::vector<Integer>& f, uint32_t d) {
        std::vector<Integer> f_prime(d);
        for (uint32_t i = 0; i < d; ++i) {
            f_prime[i] = f[i + 1].clone();
            f_prime[i] *= Integer(static_cast<int64_t>(i + 1));
        }
        return f_prime;
    }

    /// Evaluate f'(m) mod N via Horner's method
    [[nodiscard]] static Integer evaluate_f_derivative_at_m(const NumberField& nf) {
        uint32_t d = nf.degree();
        const Integer& m = nf.m();
        const Integer& n = nf.n();

        // f'(x) = d·c_d·x^{d-1} + (d-1)·c_{d-1}·x^{d-2} + ... + c_1
        Integer result = nf.coeff(d).clone();
        result *= Integer(static_cast<int64_t>(d));
        result %= n;

        for (int i = static_cast<int>(d) - 1; i >= 1; --i) {
            result *= m;
            Integer term = nf.coeff(i).clone();
            term *= Integer(static_cast<int64_t>(i));
            result += term;
            result %= n;
        }
        if (result.is_negative()) result += n;
        return result;
    }

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

    /// Compute f_lead_inv = f[d]^{-1} mod modulus (for poly_mul_mod)
    [[nodiscard]] static Integer compute_f_lead_inv(
            const std::vector<Integer>& f,
            uint32_t d,
            const Integer& modulus) {
        Integer f_lead_inv(int64_t(1));
        Integer f_d = f[d].clone();
        f_d %= modulus;
        if (f_d.is_negative()) f_d += modulus;
        if (!f_d.is_one()) {
            int ok = mpz_invert(f_lead_inv.get_mpz(), f_d.get_mpz(),
                                modulus.get_mpz());
            assert(ok && "f[d] must be invertible mod modulus");
            (void)ok;
        }
        return f_lead_inv;
    }

    /// Polynomial multiplication mod (f, modulus) — convenience overload
    [[nodiscard]] static std::vector<Integer> poly_mul_mod(
            const std::vector<Integer>& a,
            const std::vector<Integer>& b,
            const std::vector<Integer>& f,
            uint32_t d,
            const Integer& modulus) {
        auto fli = compute_f_lead_inv(f, d, modulus);
        return poly_mul_mod(a, b, f, d, modulus, fli);
    }

    /// Polynomial multiplication mod (f, modulus) with pre-computed f_lead_inv
    /// Both inputs have degree < d, result has degree < d
    [[nodiscard]] static std::vector<Integer> poly_mul_mod(
            const std::vector<Integer>& a,
            const std::vector<Integer>& b,
            const std::vector<Integer>& f,
            uint32_t d,
            const Integer& modulus,
            const Integer& f_lead_inv) {

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

        // Reduce mod f using pre-computed f_lead_inv
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

    /// Find next prime (with overflow guard)
    [[nodiscard]] static uint64_t next_prime(uint64_t n) {
        if (n >= UINT64_MAX - 2) return 0;
        n++;
        if (n <= 2) return 2;
        if (n % 2 == 0) {
            if (n == UINT64_MAX) return 0;
            n++;
        }
        while (true) {
            bool is_p = true;
            for (uint64_t i = 3; i * i <= n; ++i) {
                if (n % i == 0) { is_p = false; break; }
            }
            if (is_p) return n;
            if (n > UINT64_MAX - 2) return 0;
            n += 2;
        }
    }
};

} // namespace sqrt
} // namespace gnfs
