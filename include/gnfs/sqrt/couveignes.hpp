#pragma once

#include "number_field.hpp"
#include "modular_poly.hpp"
#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"

#include <vector>
#include <optional>
#include <iostream>

namespace gnfs {
namespace sqrt {

using core::Integer;
using core::PolynomialContext;

/// Couveignes square root configuration
struct CouveignesSqrtConfig {
    // Sign determination searches 2^num_primes patterns (up to 2^20 ≈ 1M)
    // More primes = larger CRT modulus = handles larger sqrt coefficients
    size_t num_primes = 16;        // Number of primes for CRT
    uint64_t prime_start = 1000;   // Starting point for prime search (larger = better)
    size_t max_attempts = 100;     // Max attempts for sign resolution
};

/// CouveignesSqrt - Compute algebraic square root using Couveignes method
///
/// The algorithm works by:
/// 1. Computing square roots modulo several small primes p
/// 2. Using CRT to lift the result
/// 3. Recognizing the coefficients as rational numbers
class CouveignesSqrt {
public:
    using Config = CouveignesSqrtConfig;

    CouveignesSqrt() : config_() {}

    explicit CouveignesSqrt(const Config& config)
        : config_(config) {}

    /// Compute square root of product of (a_i + b_i * alpha) elements
    /// @param ab_pairs Vector of (a, b) pairs whose product's sqrt we want
    /// @param nf Number field
    /// @return Square root as number field element, or empty if failed
    [[nodiscard]] std::optional<NumberFieldElement> compute(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf) const {

        if (ab_pairs.empty()) {
            return nf.one();
        }

        uint32_t d = nf.degree();
        const Integer& n = nf.n();

        // Get polynomial coefficients mod p
        auto get_f_mod_p = [&nf, d](uint64_t p) -> std::vector<uint64_t> {
            std::vector<uint64_t> f(d + 1);
            for (uint32_t i = 0; i <= d; ++i) {
                Integer coeff = nf.coeff(i).clone();
                coeff %= Integer(p);
                if (coeff.is_negative()) {
                    coeff += Integer(p);
                }
                f[i] = coeff.to_uint64();
            }
            return f;
        };

        // Collect suitable primes
        std::vector<uint64_t> primes;
        std::vector<std::vector<uint64_t>> sqrt_coeffs;  // sqrt coeffs mod each prime

        // Debug counters
        size_t primes_checked = 0;
        size_t primes_dividing_n = 0;
        size_t primes_bad_leading = 0;
        size_t primes_reducible = 0;
        size_t primes_zero_product = 0;
        size_t primes_no_sqrt = 0;

        uint64_t p = config_.prime_start;
        while (primes.size() < config_.num_primes && primes_checked < 100000) {
            p = next_prime(p);
            primes_checked++;

            // Skip primes that divide N
            Integer n_mod_p = n.clone();
            n_mod_p %= Integer(p);
            if (n_mod_p.is_zero()) {
                primes_dividing_n++;
                continue;
            }

            // Get f mod p
            auto f_mod_p = get_f_mod_p(p);

            // Check that f doesn't degenerate (leading coeff nonzero)
            if (f_mod_p.back() == 0) {
                primes_bad_leading++;
                continue;
            }

            // Full Rabin irreducibility test (not just "no roots")
            // For degree > 3, "no roots" ≠ "irreducible"
            if (!ModularPoly::is_irreducible(f_mod_p, p)) {
                primes_reducible++;
                continue;
            }

            // Compute product mod p
            auto product = compute_product_mod_p(ab_pairs, f_mod_p, p);

            if (product.is_zero()) {
                // Product is zero mod p, skip this prime
                primes_zero_product++;
                continue;
            }

            // Check if product is a square
            if (!ModularPoly::is_square(product, f_mod_p, p)) {
                primes_no_sqrt++;
                continue;
            }

            // Compute square root
            auto sqrt_p = ModularPoly::sqrt_tonelli_shanks(product, f_mod_p, p);

            if (sqrt_p.is_zero() && !product.is_zero()) {
                // Square root doesn't exist mod p (shouldn't happen for valid dependency)
                primes_no_sqrt++;
                continue;
            }

            // Verify: sqrt^2 should equal product mod p
            auto sqrt_squared_p = ModularPoly::mul(sqrt_p, sqrt_p, f_mod_p, p);
            bool sqrt_valid = true;
            for (size_t i = 0; i <= std::max(static_cast<size_t>(sqrt_squared_p.degree()),
                                             static_cast<size_t>(product.degree())); ++i) {
                if (sqrt_squared_p.coeff(i) != product.coeff(i)) {
                    sqrt_valid = false;
                    break;
                }
            }
            if (!sqrt_valid) {
                primes_no_sqrt++;
                continue;
            }

            // Store this prime and its square root coefficients (without sign normalization)
            // We'll handle sign consistency during CRT phase
            primes.push_back(p);
            std::vector<uint64_t> coeffs(d, 0);
            for (size_t i = 0; i < d && i <= static_cast<size_t>(sqrt_p.degree()); ++i) {
                coeffs[i] = sqrt_p.coeff(i);
            }
            sqrt_coeffs.push_back(std::move(coeffs));

            if (primes.size() >= config_.num_primes) {
                break;
            }
        }

        if (primes.size() < 2) {
            return std::nullopt;  // Not enough primes found
        }

        // Debug can be enabled via COUVEIGNES_DEBUG
        #ifdef COUVEIGNES_DEBUG
        std::cerr << "[Couveignes] primes=" << primes.size()
                  << " checked=" << primes_checked
                  << " reducible=" << primes_reducible
                  << " no_sqrt=" << primes_no_sqrt
                  << " zero_prod=" << primes_zero_product << "\n";
        #endif

        // Sign determination using subset enumeration
        // Enumerate all 2^k sign combinations for first k primes

        // Compute CRT modulus for all primes
        Integer M(1);
        for (uint64_t prime : primes) {
            M *= Integer(prime);
        }

        // Suppress unused variable warnings
        (void)primes_checked;
        (void)primes_dividing_n;
        (void)primes_bad_leading;
        (void)primes_reducible;
        (void)primes_zero_product;
        (void)primes_no_sqrt;

        // Note: old compute_crt_with_signs lambda removed — replaced by
        // precomputed weights + Gray code incremental update below

        // === OPTIMIZED sign pattern search ===
        // Key optimizations:
        // 1. Precompute CRT weights once (M_j * M_j_inv for each prime j)
        // 2. Compute base CRT (all-positive signs), then use Gray code enumeration
        //    so each step only flips one sign → incremental CRT update in O(d)
        // 3. Early rejection: check first coefficient before full d² verification

        // Exhaustive search over first 16 primes (2^16 = 65536 patterns)
        // Remaining primes contribute to CRT modulus for precision but
        // their sign is determined by the matching pattern of the first 16
        size_t num_to_search = std::min(primes.size(), static_cast<size_t>(16));
        // Extra primes beyond 16 are always kept positive (arbitrary but consistent)
        uint64_t max_patterns = 1ULL << num_to_search;

        // Precompute expected product ONCE
        NumberFieldElement expected_product = nf.one();
        for (const auto& [a, b] : ab_pairs) {
            auto factor = nf.from_ab(a, b);
            expected_product = nf.multiply_mod_n(expected_product, factor);
        }

        // --- Step 1: Precompute CRT weights ---
        // weight[j][i] = c_ij * M_j * M_j_inv mod M  (for each prime j, coefficient i)
        // When sign[j] is flipped, we subtract 2 * weight[j][i] from coeff[i]
        std::vector<std::vector<Integer>> weights(primes.size());

        for (size_t j = 0; j < primes.size(); ++j) {
            uint64_t p_j = primes[j];
            Integer M_j = M.clone();
            M_j /= Integer(p_j);
            Integer M_j_mod_pj = M_j.clone();
            M_j_mod_pj %= Integer(p_j);
            uint64_t M_j_inv = mod_inverse_u64(M_j_mod_pj.to_uint64(), p_j);

            weights[j].resize(d);
            for (uint32_t i = 0; i < d; ++i) {
                uint64_t c_ij = sqrt_coeffs[j][i];
                Integer w = Integer(c_ij);
                w *= M_j;
                w *= Integer(M_j_inv);
                w %= M;
                weights[j][i] = std::move(w);
            }
        }

        // --- Step 2: Compute base CRT (all signs = +) ---
        std::vector<Integer> base_coeffs(d);
        for (uint32_t i = 0; i < d; ++i) {
            Integer coeff_i(static_cast<int64_t>(0));
            for (size_t j = 0; j < primes.size(); ++j) {
                coeff_i += weights[j][i];
            }
            coeff_i %= M;
            base_coeffs[i] = std::move(coeff_i);
        }

        // Center around 0
        Integer half_M = M.clone();
        mpz_tdiv_q_2exp(half_M.get_mpz(), half_M.get_mpz(), 1);
        for (auto& c : base_coeffs) {
            if (c.compare(half_M) > 0) c -= M;
        }

        // Precompute 2 * weight[j][i] for incremental updates
        std::vector<std::vector<Integer>> two_weights(primes.size());
        for (size_t j = 0; j < primes.size(); ++j) {
            two_weights[j].resize(d);
            for (uint32_t i = 0; i < d; ++i) {
                two_weights[j][i] = weights[j][i].clone();
                two_weights[j][i] *= Integer(static_cast<int64_t>(2));
            }
        }

        // --- Step 3: Gray code enumeration ---
        // current_coeffs starts as base (all-positive)
        // Each Gray code step flips exactly one bit
        std::vector<Integer> current_coeffs(d);
        for (uint32_t i = 0; i < d; ++i) {
            current_coeffs[i] = base_coeffs[i].clone();
        }

        // Precompute rational product sqrt for GCD-based verification
        // Instead of verifying S^2 == product (which has reduction order issues),
        // compute rational sqrt X and check if gcd(S(m) - X, N) is non-trivial.
        // The correct sign pattern will give a non-trivial gcd ~50% of the time.
        //
        // We compute X^2 = ∏(a_i - b_i·m) mod N via direct multiplication
        Integer rat_product(int64_t(1));
        for (const auto& [a, b] : ab_pairs) {
            Integer term(a);
            Integer bm = nf.m().clone();
            bm *= Integer(b);
            term -= bm;
            term %= n;
            if (term.is_negative()) term += n;
            rat_product *= term;
            rat_product %= n;
        }
        // Compute rational sqrt via X = rat_product^((N+1)/4) if N ≡ 3 mod 4
        // or use Tonelli-Shanks otherwise
        Integer rat_sqrt;
        {
            Integer exp = n.clone();
            exp += Integer(int64_t(1));
            mpz_tdiv_q_2exp(exp.get_mpz(), exp.get_mpz(), 2);  // (N+1)/4 for N ≡ 3 mod 4
            rat_sqrt = core::powmod(rat_product, exp, n);
            // Verify
            Integer check = rat_sqrt.clone();
            check *= rat_sqrt;
            check %= n;
            if (check.compare(rat_product) != 0) {
                // Try (N+1)/2 instead (general case)
                exp = n.clone();
                exp += Integer(int64_t(1));
                mpz_tdiv_q_2exp(exp.get_mpz(), exp.get_mpz(), 1);
                rat_sqrt = core::powmod(rat_product, exp, n);
            }
        }

        auto verify_current = [&]() -> bool {
            // GCD-based verification: compute Y = candidate(m) mod N,
            // then check if gcd(Y - rat_sqrt, N) is non-trivial
            std::vector<Integer> cand_mod_n(d);
            for (uint32_t i = 0; i < d; ++i) {
                cand_mod_n[i] = current_coeffs[i].clone();
                cand_mod_n[i] %= n;
                if (cand_mod_n[i].is_negative()) cand_mod_n[i] += n;
            }
            NumberFieldElement candidate(std::move(cand_mod_n));
            Integer Y = nf.evaluate_at_m_mod_n(candidate);

            // Check gcd(Y - rat_sqrt, N) and gcd(Y + rat_sqrt, N)
            Integer diff = Y.clone();
            diff -= rat_sqrt;
            diff %= n;
            if (diff.is_negative()) diff += n;
            Integer g1 = core::gcd(diff, n);

            if (!g1.is_one() && g1.compare(n) != 0) return true;

            Integer sum = Y.clone();
            sum += rat_sqrt;
            sum %= n;
            Integer g2 = core::gcd(sum, n);

            if (!g2.is_one() && g2.compare(n) != 0) return true;

            // Also try with -Y
            Integer neg_Y = n.clone();
            neg_Y -= Y;

            diff = neg_Y.clone();
            diff -= rat_sqrt;
            diff %= n;
            if (diff.is_negative()) diff += n;
            g1 = core::gcd(diff, n);
            if (!g1.is_one() && g1.compare(n) != 0) return true;

            sum = neg_Y.clone();
            sum += rat_sqrt;
            sum %= n;
            g2 = core::gcd(sum, n);
            if (!g2.is_one() && g2.compare(n) != 0) return true;

            return false;  // Trivial gcd — this pattern doesn't work
        };

        auto extract_result = [&]() -> std::vector<Integer> {
            // CRT gives coefficients that represent an element of Z[α]/(f) already
            // (degree < d), so no polynomial reduction is needed.
            // Reduce each coefficient mod N for the number field ring Z[α]/(f, N).
            std::vector<Integer> r(d);
            for (uint32_t i = 0; i < d; ++i) {
                r[i] = current_coeffs[i].clone();
                r[i] %= n;
                if (r[i].is_negative()) r[i] += n;
            }
            return r;
        };

        // Also precompute: expected_product_at_m = ∏(a_i - b_i·m) mod N
        // This is what Y^2 should equal if everything is correct.
        Integer expected_X2(int64_t(1));
        for (const auto& [a, b] : ab_pairs) {
            Integer term(a);
            Integer bm = nf.m().clone();
            bm *= Integer(b);
            term -= bm;
            term %= n;
            if (term.is_negative()) term += n;
            expected_X2 *= term;
            expected_X2 %= n;
        }

        // Check pattern 0 (all positive)
        if (verify_current()) {
            return NumberFieldElement(extract_result());
        }

        // Gray code iteration: pattern g = i ^ (i >> 1)
        // Bit that flips: trailing zeros of i gives the position
        uint64_t prev_gray = 0;
        for (uint64_t i = 1; i < max_patterns; ++i) {
            uint64_t gray = i ^ (i >> 1);
            uint64_t changed_bit = prev_gray ^ gray;
            size_t bit_pos = __builtin_ctzll(changed_bit);
            bool new_sign = (gray >> bit_pos) & 1;  // 1 = negative

            // Incremental CRT update: flip sign of prime[bit_pos]
            // If going positive → negative: subtract 2*weight
            // If going negative → positive: add 2*weight
            for (uint32_t ci = 0; ci < d; ++ci) {
                if (new_sign) {
                    current_coeffs[ci] -= two_weights[bit_pos][ci];
                } else {
                    current_coeffs[ci] += two_weights[bit_pos][ci];
                }
            }

            prev_gray = gray;

            if (verify_current()) {
                return NumberFieldElement(extract_result());
            }
        }

        return std::nullopt;  // No valid sign pattern found
    }

    /// Compute square root directly from NumberFieldElement
    [[nodiscard]] std::optional<NumberFieldElement> compute_from_element(
            const NumberFieldElement& elem,
            const NumberField& nf) const {

        uint32_t d = nf.degree();
        const Integer& n = nf.n();

        // Get polynomial coefficients
        auto get_f_mod_p = [&nf, d](uint64_t p) -> std::vector<uint64_t> {
            std::vector<uint64_t> f(d + 1);
            for (uint32_t i = 0; i <= d; ++i) {
                Integer coeff = nf.coeff(i).clone();
                coeff %= Integer(p);
                if (coeff.is_negative()) {
                    coeff += Integer(p);
                }
                f[i] = coeff.to_uint64();
            }
            return f;
        };

        // Convert element to ModularPoly mod p
        auto elem_to_mod_p = [&elem, d](uint64_t p) -> ModularPoly {
            std::vector<uint64_t> coeffs(d);
            for (uint32_t i = 0; i < d && i <= elem.degree(); ++i) {
                Integer c = elem.coeff(i).clone();
                c %= Integer(p);
                if (c.is_negative()) {
                    c += Integer(p);
                }
                coeffs[i] = c.to_uint64();
            }
            return ModularPoly(std::move(coeffs));
        };

        // Collect suitable primes
        std::vector<uint64_t> primes;
        std::vector<std::vector<uint64_t>> sqrt_coeffs;

        uint64_t p = config_.prime_start;
        while (primes.size() < config_.num_primes) {
            p = next_prime(p);

            // Skip primes that divide N
            Integer n_mod_p = n.clone();
            n_mod_p %= Integer(p);
            if (n_mod_p.is_zero()) {
                continue;
            }

            auto f_mod_p = get_f_mod_p(p);
            if (f_mod_p.back() == 0) {
                continue;
            }

            auto elem_mod_p = elem_to_mod_p(p);

            if (elem_mod_p.is_zero()) {
                continue;
            }

            auto sqrt_p = ModularPoly::sqrt_tonelli_shanks(elem_mod_p, f_mod_p, p);

            if (sqrt_p.is_zero() && !elem_mod_p.is_zero()) {
                continue;
            }

            // Sign normalization: evaluate sqrt at t=1 and pick sign such that result < p/2
            uint64_t eval_at_1 = 0;
            for (int i = 0; i <= sqrt_p.degree(); ++i) {
                eval_at_1 = (eval_at_1 + sqrt_p.coeff(i)) % p;
            }
            bool needs_negate = (eval_at_1 > p / 2);

            primes.push_back(p);
            std::vector<uint64_t> coeffs(d, 0);
            for (size_t i = 0; i < d && i <= static_cast<size_t>(sqrt_p.degree()); ++i) {
                uint64_t c = sqrt_p.coeff(i);
                if (needs_negate) {
                    c = (c == 0) ? 0 : (p - c);
                }
                coeffs[i] = c;
            }
            sqrt_coeffs.push_back(std::move(coeffs));
        }

        if (primes.size() < 2) {
            return std::nullopt;
        }

        // CRT reconstruction
        std::vector<Integer> result_coeffs(d);
        Integer M(1);
        for (uint64_t prime : primes) {
            M *= Integer(prime);
        }

        for (uint32_t i = 0; i < d; ++i) {
            Integer coeff_i(static_cast<int64_t>(0));

            for (size_t j = 0; j < primes.size(); ++j) {
                uint64_t p_j = primes[j];
                uint64_t c_ij = sqrt_coeffs[j][i];

                Integer M_j = M.clone();
                M_j /= Integer(p_j);

                Integer M_j_mod_pj = M_j.clone();
                M_j_mod_pj %= Integer(p_j);
                uint64_t M_j_inv = mod_inverse_u64(M_j_mod_pj.to_uint64(), p_j);

                Integer term = Integer(c_ij);
                term *= M_j;
                term *= Integer(M_j_inv);

                coeff_i += term;
            }

            coeff_i %= M;
            result_coeffs[i] = std::move(coeff_i);
        }

        // Center coefficients
        Integer half_M = M.clone();
        mpz_tdiv_q_2exp(half_M.get_mpz(), half_M.get_mpz(), 1);

        for (auto& c : result_coeffs) {
            if (c.compare(half_M) > 0) {
                c -= M;
            }
        }

        // Reduce mod N
        for (auto& c : result_coeffs) {
            c %= n;
            if (c.is_negative()) {
                c += n;
            }
        }

        return NumberFieldElement(std::move(result_coeffs));
    }

private:
    Config config_;

    /// Compute product of (a_i + b_i * x) mod f(x) mod p
    [[nodiscard]] ModularPoly compute_product_mod_p(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const std::vector<uint64_t>& f,
            uint64_t p) const {

        ModularPoly product(1);

        for (const auto& [a, b] : ab_pairs) {
            // Create (a - b*x) mod p (GNFS convention)
            std::vector<uint64_t> coeffs(2);

            int64_t a_mod = a % static_cast<int64_t>(p);
            if (a_mod < 0) a_mod += static_cast<int64_t>(p);
            coeffs[0] = static_cast<uint64_t>(a_mod);

            // Coefficient of x is -b mod p
            uint64_t b_mod = b % p;
            coeffs[1] = (p - b_mod) % p;

            ModularPoly factor(std::move(coeffs));

            // Multiply into product
            product = ModularPoly::mul(product, factor, f, p);
        }

        return product;
    }

    /// Find next prime after n
    [[nodiscard]] static uint64_t next_prime(uint64_t n) {
        n++;
        if (n <= 2) return 2;
        if (n % 2 == 0) n++;

        while (!is_prime_u64(n)) {
            n += 2;
        }
        return n;
    }

    /// Miller-Rabin primality test
    [[nodiscard]] static bool is_prime_u64(uint64_t n) {
        if (n < 2) return false;
        if (n == 2 || n == 3) return true;
        if (n % 2 == 0) return false;

        // Write n-1 as d * 2^r
        uint64_t d = n - 1;
        uint64_t r = 0;
        while ((d & 1) == 0) {
            d >>= 1;
            r++;
        }

        // Witnesses for n < 2^64
        static const uint64_t witnesses[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};

        for (uint64_t a : witnesses) {
            if (a >= n) continue;

            uint64_t x = pow_mod_u64(a, d, n);
            if (x == 1 || x == n - 1) continue;

            bool composite = true;
            for (uint64_t i = 0; i < r - 1; i++) {
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

    /// Modular exponentiation
    [[nodiscard]] static uint64_t pow_mod_u64(uint64_t base, uint64_t exp, uint64_t mod) {
        uint64_t result = 1;
        base %= mod;

        while (exp > 0) {
            if (exp & 1) {
                result = mul_mod_u64(result, base, mod);
            }
            base = mul_mod_u64(base, base, mod);
            exp >>= 1;
        }

        return result;
    }

    /// Modular multiplication (handles overflow)
    [[nodiscard]] static uint64_t mul_mod_u64(uint64_t a, uint64_t b, uint64_t mod) {
        __uint128_t prod = static_cast<__uint128_t>(a) * b;
        return static_cast<uint64_t>(prod % mod);
    }

    /// Modular inverse
    [[nodiscard]] static uint64_t mod_inverse_u64(uint64_t a, uint64_t p) {
        int64_t t = 0, new_t = 1;
        int64_t r = static_cast<int64_t>(p), new_r = static_cast<int64_t>(a);

        while (new_r != 0) {
            int64_t quotient = r / new_r;

            int64_t temp_t = new_t;
            new_t = t - quotient * new_t;
            t = temp_t;

            int64_t temp_r = new_r;
            new_r = r - quotient * new_r;
            r = temp_r;
        }

        if (t < 0) {
            t += static_cast<int64_t>(p);
        }

        return static_cast<uint64_t>(t);
    }
};

} // namespace sqrt
} // namespace gnfs
