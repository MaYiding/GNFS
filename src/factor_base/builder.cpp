#include "gnfs/factor_base/builder.hpp"
#include "gnfs/sqrt/modular_poly.hpp"
#include <algorithm>
#include <cmath>
#include <random>

namespace gnfs::factor_base {

// ============================================================
// FactorBase methods (save/load - inline methods are in header)
// ============================================================

void FactorBase::save(std::ostream& /* os */) const {
    // TODO: Implement serialization
}

FactorBase FactorBase::load(std::istream& /* is */) {
    // TODO: Implement deserialization
    return FactorBase();
}

// ============================================================
// FactorBaseBuilder methods
// ============================================================

// Note: Instance-based constructor is removed because PolynomialContext
// doesn't support copying. Use the static build method instead.

FactorBaseBuilder::FactorBaseBuilder(const PolynomialContext& /* ctx */)
    : ctx_() {
    // Cannot copy PolynomialContext, this constructor is deprecated
    // Use the static build() method instead
}

FactorBase FactorBaseBuilder::build(const PolynomialContext& ctx, const Options& opts) {
    FactorBase fb;

    // Set parameters
    core::FactorBaseParams params(
        opts.rational_bound,
        opts.algebraic_bound,
        opts.rational_bound * 100,  // large prime bound
        opts.log_scale
    );
    fb.set_params(params);

    // 实际代数素数上界：含 special-Q 范围
    uint32_t effective_alg_bound = opts.algebraic_bound;
    if (opts.special_q_bound > opts.algebraic_bound) {
        effective_alg_bound = opts.special_q_bound;
    }

    // Estimate sizes for preallocation
    // Prime counting function: π(n) ≈ n / ln(n)
    size_t estimated_rational = static_cast<size_t>(
        opts.rational_bound / std::log(static_cast<double>(opts.rational_bound + 1)) * 1.2
    );
    size_t estimated_algebraic = static_cast<size_t>(
        effective_alg_bound / std::log(static_cast<double>(effective_alg_bound + 1)) * 1.2
    ) * ctx.degree();
    fb.reserve(estimated_rational, estimated_algebraic);

    find_rational_primes(fb, ctx, opts.rational_bound, opts.log_scale);
    find_algebraic_primes(fb, ctx, opts.algebraic_bound, opts.log_scale);

    // 记录筛选用的代数素数数量（≤ algebraic_bound 的部分）
    fb.set_sieve_algebraic_count(fb.algebraic_count());

    // 如果 special_q_bound > algebraic_bound，继续构建 SQ 范围的代数素数
    // 注意: algebraic_bound + 1 在 UINT32_MAX 时会溢出，但 params.hpp 将 B 限制在 1e9
    if (opts.special_q_bound > opts.algebraic_bound &&
        opts.algebraic_bound < UINT32_MAX) {
        find_algebraic_primes_range(fb, ctx,
            opts.algebraic_bound + 1, opts.special_q_bound, opts.log_scale);
    }

    fb.build_index();
    return fb;
}

FactorBase FactorBaseBuilder::build(uint32_t rational_bound, uint32_t algebraic_bound) {
    // This method requires a valid ctx_, which we can't have with deleted copy
    // Return empty factor base
    Options opts;
    opts.rational_bound = rational_bound;
    opts.algebraic_bound = algebraic_bound;
    return FactorBase();
}

void FactorBaseBuilder::find_rational_primes(FactorBase& fb, uint32_t bound, uint8_t log_scale) {
    // Sieve of Eratosthenes
    std::vector<bool> is_prime(bound + 1, true);
    is_prime[0] = is_prime[1] = false;

    for (uint32_t p = 2; p <= bound; ++p) {
        if (!is_prime[p]) continue;

        // Mark multiples
        for (uint32_t k = p * 2; k <= bound; k += p) {
            is_prime[k] = false;
        }

        // Compute log value
        uint32_t log_p = compute_log_prime_precise(p, log_scale);

        // Add to factor base
        fb.add_rational(p, log_p);
    }
}

void FactorBaseBuilder::find_rational_primes(FactorBase& fb, const PolynomialContext& ctx,
                                              uint32_t bound, uint8_t log_scale) {
    // Sieve of Eratosthenes
    std::vector<bool> is_prime(bound + 1, true);
    is_prime[0] = is_prime[1] = false;

    for (uint32_t p = 2; p <= bound; ++p) {
        if (!is_prime[p]) continue;

        // Mark multiples
        for (uint32_t k = p * 2; k <= bound; k += p) {
            is_prime[k] = false;
        }

        // Skip primes that divide N (these are factors we're trying to find!)
        core::Integer p_int(static_cast<unsigned long long>(p));
        core::Integer gcd_result = core::gcd(p_int, ctx.n());
        if (!gcd_result.is_one()) {
            continue;  // p divides N, skip it
        }

        // Compute log value
        uint32_t log_p = compute_log_prime_precise(p, log_scale);

        // Add to factor base
        fb.add_rational(p, log_p);
    }
}

void FactorBaseBuilder::find_algebraic_primes(FactorBase& fb, const PolynomialContext& ctx,
                                               uint32_t bound, uint8_t log_scale) {
    // Sieve of Eratosthenes
    std::vector<bool> is_prime(bound + 1, true);
    is_prime[0] = is_prime[1] = false;

    for (uint32_t p = 2; p <= bound; ++p) {
        if (!is_prime[p]) continue;

        // Mark multiples
        for (uint32_t k = p * 2; k <= bound; k += p) {
            is_prime[k] = false;
        }

        // Skip primes that divide N (these are factors we're trying to find!)
        core::Integer p_int(static_cast<unsigned long long>(p));
        core::Integer gcd_result = core::gcd(p_int, ctx.n());
        if (!gcd_result.is_one()) {
            continue;  // p divides N, skip it
        }

        // Find roots of f(x) ≡ 0 (mod p)
        auto roots = find_roots_mod_p(ctx, p);

        // Compute log value
        uint32_t log_p = compute_log_prime_precise(p, log_scale);

        for (uint32_t root : roots) {
            fb.add_algebraic(p, root, log_p, 1);
        }
    }
}

std::vector<uint32_t> FactorBaseBuilder::find_roots_mod_p(const PolynomialContext& ctx, uint32_t p) {
    // For very small primes, brute force is faster than the overhead of polynomial GCD
    if (p < 64) {
        std::vector<uint32_t> roots;
        for (uint32_t r = 0; r < p; ++r) {
            if (ctx.evaluate_mod(r, p) == 0) {
                roots.push_back(r);
            }
        }
        return roots;
    }

    // Cantor-Zassenhaus algorithm: O(d^2 * log p) instead of O(p)
    // Step 1: Get f(x) mod p
    uint32_t d = ctx.degree();
    std::vector<uint64_t> f_mod(d + 1);
    for (uint32_t i = 0; i <= d; ++i) {
        core::Integer c = ctx.coeff(i).clone();
        c %= core::Integer(static_cast<int64_t>(p));
        if (c.is_negative()) c += core::Integer(static_cast<int64_t>(p));
        f_mod[i] = c.to_uint64();
    }

    // Step 2: Compute g = gcd(x^p - x, f) mod p
    // This gives the product of all distinct linear factors of f
    sqrt::ModularPoly x_poly;
    x_poly.set_coeff(1, 1);  // x

    // x^p mod f mod p
    auto x_to_p = sqrt::ModularPoly::power(x_poly, core::Integer(static_cast<int64_t>(p)), f_mod, p);

    // x^p - x mod p
    auto x_p_minus_x = sqrt::ModularPoly::sub(x_to_p, x_poly, p);

    // gcd(x^p - x, f) mod p
    sqrt::ModularPoly f_poly(f_mod);
    auto g = sqrt::ModularPoly::gcd(x_p_minus_x, f_poly, p);

    int g_deg = g.degree();
    if (g_deg <= 0) {
        // No roots
        return {};
    }

    // Step 3: Extract roots from g (the split-free part)
    // g is a product of distinct linear factors (x - r_1)(x - r_2)...
    return extract_roots_from_poly(g, f_mod, p);
}

/// Extract roots from a polynomial that is a product of distinct linear factors mod p
/// Uses Cantor-Zassenhaus splitting when degree > 1
std::vector<uint32_t> FactorBaseBuilder::extract_roots_from_poly(
        const sqrt::ModularPoly& poly, const std::vector<uint64_t>& f_mod, uint32_t p) {

    int deg = poly.degree();
    if (deg <= 0) return {};

    if (deg == 1) {
        // Linear: ax + b = 0 → x = -b/a mod p
        uint64_t a = poly.coeff(1);
        uint64_t b = poly.coeff(0);
        // x = -b * a^{-1} mod p
        // a^{-1} = a^{p-2} mod p (Fermat)
        uint64_t a_inv = 1;
        {
            uint64_t base = a % p, exp = p - 2;
            uint64_t result = 1;
            while (exp > 0) {
                if (exp & 1) result = static_cast<uint64_t>(
                    (static_cast<__uint128_t>(result) * base) % p);
                base = static_cast<uint64_t>(
                    (static_cast<__uint128_t>(base) * base) % p);
                exp >>= 1;
            }
            a_inv = result;
        }
        uint64_t root = static_cast<uint64_t>(
            (static_cast<__uint128_t>(p - b) * a_inv) % p);
        return {static_cast<uint32_t>(root)};
    }

    // Degree > 1: use Cantor-Zassenhaus random splitting
    std::mt19937_64 rng(p);  // deterministic seed per prime
    std::vector<uint32_t> roots;

    // Try random splits
    for (int attempt = 0; attempt < 100 && static_cast<int>(roots.size()) < deg; ++attempt) {
        uint64_t a = rng() % p;

        // Compute gcd(poly, (x + a)^{(p-1)/2} - 1) mod p
        // This splits poly roughly in half (probabilistically)
        sqrt::ModularPoly x_plus_a;
        x_plus_a.set_coeff(0, a);
        x_plus_a.set_coeff(1, 1);

        // Get coefficients of current poly for reduction
        std::vector<uint64_t> poly_coeffs;
        for (int i = 0; i <= poly.degree(); ++i) {
            poly_coeffs.push_back(poly.coeff(i));
        }

        // (x+a)^{(p-1)/2} mod poly mod p
        core::Integer exp_val(static_cast<int64_t>((p - 1) / 2));
        auto power_result = sqrt::ModularPoly::power(x_plus_a, exp_val, poly_coeffs, p);

        // Subtract 1
        uint64_t c0 = power_result.coeff(0);
        c0 = (c0 + p - 1) % p;
        power_result.set_coeff(0, c0);

        // gcd with poly
        auto factor = sqrt::ModularPoly::gcd(power_result, poly, p);

        int factor_deg = factor.degree();
        if (factor_deg > 0 && factor_deg < deg) {
            // Successfully split! Recurse on both factors
            auto roots1 = extract_roots_from_poly(factor, f_mod, p);
            roots.insert(roots.end(), roots1.begin(), roots1.end());

            // Compute the other factor: poly / factor mod p
            // Simple polynomial division since factor divides poly exactly
            auto other = poly_div_mod(poly, factor, p);
            auto roots2 = extract_roots_from_poly(other, f_mod, p);
            roots.insert(roots.end(), roots2.begin(), roots2.end());
            return roots;
        }
    }

    // Fallback: brute force for this specific polynomial (shouldn't happen often)
    for (uint32_t r = 0; r < p && static_cast<int>(roots.size()) < deg; ++r) {
        uint64_t val = 0, rp = 1;
        for (int i = 0; i <= poly.degree(); ++i) {
            val = (val + static_cast<uint64_t>(
                (static_cast<__uint128_t>(poly.coeff(i)) * rp) % p)) % p;
            rp = static_cast<uint64_t>((static_cast<__uint128_t>(rp) * r) % p);
        }
        if (val == 0) roots.push_back(r);
    }
    return roots;
}

/// Polynomial division: compute a / b mod p (exact division)
sqrt::ModularPoly FactorBaseBuilder::poly_div_mod(
        const sqrt::ModularPoly& a, const sqrt::ModularPoly& b, uint32_t p) {

    if (b.degree() < 0 || (b.degree() == 0 && b.coeff(0) == 0)) {
        return sqrt::ModularPoly();  // division by zero
    }

    // Copy a's coefficients
    std::vector<uint64_t> rem;
    for (int i = 0; i <= a.degree(); ++i) {
        rem.push_back(a.coeff(i));
    }

    int a_deg = a.degree();
    int b_deg = b.degree();
    int q_deg = a_deg - b_deg;
    if (q_deg < 0) return sqrt::ModularPoly();

    // Inverse of leading coefficient of b
    uint64_t b_lead = b.coeff(b_deg);
    uint64_t b_lead_inv = 1;
    {
        uint64_t base = b_lead % p, exp = p - 2, result = 1;
        while (exp > 0) {
            if (exp & 1) result = static_cast<uint64_t>(
                (static_cast<__uint128_t>(result) * base) % p);
            base = static_cast<uint64_t>(
                (static_cast<__uint128_t>(base) * base) % p);
            exp >>= 1;
        }
        b_lead_inv = result;
    }

    std::vector<uint64_t> quotient(q_deg + 1, 0);

    for (int i = q_deg; i >= 0; --i) {
        uint64_t coeff = static_cast<uint64_t>(
            (static_cast<__uint128_t>(rem[i + b_deg]) * b_lead_inv) % p);
        quotient[i] = coeff;

        for (int j = 0; j <= b_deg; ++j) {
            uint64_t sub = static_cast<uint64_t>(
                (static_cast<__uint128_t>(coeff) * b.coeff(j)) % p);
            rem[i + j] = (rem[i + j] + p - sub) % p;
        }
    }

    return sqrt::ModularPoly(quotient);
}

void FactorBaseBuilder::find_algebraic_primes_range(FactorBase& fb, const PolynomialContext& ctx,
                                                     uint32_t min_p, uint32_t max_p, uint8_t log_scale) {
    // 对 [min_p, max_p] 范围内的素数求根并加入因子基
    if (min_p > max_p || min_p < 2) return;  // 防止溢出或无效范围

    // 使用 Sieve of Eratosthenes 筛出范围内的素数
    std::vector<bool> is_prime(static_cast<size_t>(max_p) + 1, true);
    is_prime[0] = is_prime[1] = false;

    for (uint64_t p = 2; p * p <= max_p; ++p) {
        if (!is_prime[static_cast<size_t>(p)]) continue;
        for (uint64_t k = p * 2; k <= max_p; k += p) {
            is_prime[static_cast<size_t>(k)] = false;
        }
    }

    for (uint64_t p = min_p; p <= max_p; ++p) {
        if (!is_prime[static_cast<size_t>(p)]) continue;
        uint32_t p32 = static_cast<uint32_t>(p);

        // Skip primes that divide N
        core::Integer p_int(static_cast<unsigned long long>(p32));
        core::Integer gcd_result = core::gcd(p_int, ctx.n());
        if (!gcd_result.is_one()) {
            continue;
        }

        auto roots = find_roots_mod_p(ctx, p32);
        uint32_t log_p = compute_log_prime_precise(p32, log_scale);

        for (uint32_t root : roots) {
            fb.add_algebraic(p32, root, log_p, 1);
        }
    }
}

} // namespace gnfs::factor_base
