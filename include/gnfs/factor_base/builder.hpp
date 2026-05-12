#pragma once

#include "gnfs/core/integer.hpp"
#include "gnfs/core/polynomial_context.hpp"
#include "gnfs/factor_base/factor_base.hpp"
#include <vector>
#include <cstdint>

// Forward declaration for ModularPoly
namespace gnfs::sqrt { class ModularPoly; }

namespace gnfs::factor_base {

using gnfs::core::Integer;
using gnfs::core::PolynomialContext;

/// Builder for factor base
class FactorBaseBuilder {
public:
    struct Options {
        uint32_t rational_bound = 1000;
        uint32_t algebraic_bound = 1000;
        uint32_t special_q_bound = 0;   // Special-Q 素数上界（0 = 同 algebraic_bound）
        uint64_t large_prime_bound = 0; // 大素数界（0 = rational_bound × 100 默认值）
        uint8_t log_scale = core::SIEVE_LOG_SCALE;  // Scale factor for log values
        bool parallel = true;

        Options() = default;
    };

    // Static build method
    static FactorBase build(const PolynomialContext& ctx, const Options& opts);

    // Instance-based method (for compatibility)
    explicit FactorBaseBuilder(const PolynomialContext& ctx);
    FactorBase build(uint32_t rational_bound, uint32_t algebraic_bound);

private:
    PolynomialContext ctx_;

    static void find_rational_primes(FactorBase& fb, const PolynomialContext& ctx, uint32_t bound, uint8_t log_scale);
    static void find_algebraic_primes(FactorBase& fb, const PolynomialContext& ctx, uint32_t bound, uint8_t log_scale);
    static void find_algebraic_primes_range(FactorBase& fb, const PolynomialContext& ctx,
                                             uint32_t min_p, uint32_t max_p, uint8_t log_scale);

    /// Find roots of f(x) ≡ 0 mod p using Cantor-Zassenhaus algorithm
    /// O(d^2 * log p) instead of O(p) brute force
    static std::vector<uint32_t> find_roots_mod_p(const PolynomialContext& ctx, uint32_t p);

    /// Extract roots from a polynomial known to be a product of distinct linear factors
    static std::vector<uint32_t> extract_roots_from_poly(
        const sqrt::ModularPoly& poly, const std::vector<uint64_t>& f_mod, uint32_t p);

    /// Exact polynomial division: a / b mod p
    static sqrt::ModularPoly poly_div_mod(
        const sqrt::ModularPoly& a, const sqrt::ModularPoly& b, uint32_t p);
};

} // namespace gnfs::factor_base
