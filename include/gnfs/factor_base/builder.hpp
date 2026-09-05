#pragma once

#include "gnfs/core/integer.hpp"
#include "gnfs/core/polynomial_context.hpp"
#include "gnfs/factor_base/factor_base.hpp"
#include <cstdint>
#include <vector>

// Forward declaration for ModularPoly
namespace gnfs::sqrt {
class ModularPoly;
}

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
        uint8_t log_scale = core::SIEVE_LOG_SCALE; // Scale factor for log values
        bool parallel = true;

        Options() = default;
    };

    // Static build method
    static FactorBase build(const PolynomialContext& ctx, const Options& opts);

    // Instance-based method (for compatibility)
    explicit FactorBaseBuilder(const PolynomialContext& ctx);
    FactorBase build(uint32_t rational_bound, uint32_t algebraic_bound);

public:
    // Find roots of f(x) ≡ 0 mod p using Cantor-Zassenhaus algorithm.
    // Throws std::invalid_argument when p is not prime.
    // (公开为静态工具,主要给测试用 — p<64 走 brute-force,p≥64 走 CZ
    // 含 random splitting 多根分离)。
    static std::vector<uint32_t> find_roots_mod_p(const PolynomialContext& ctx, uint32_t p);

    /// 构建 Eratosthenes 筛(从 p*p 起标),返回 is_prime[0..bound]。
    /// 拆出避免 find_rational + find_algebraic 在同 bound 下重复构建(1e8 ≈ 12.5 MB)。
    /// bound ≥ 5e6 走分段并行筛,小 bound 走简单单线程。
    /// 公开以便测试覆盖分段路径正确性。
    static std::vector<bool> build_eratosthenes_sieve(uint32_t bound);

private:
    PolynomialContext ctx_;

    static void find_rational_primes(FactorBase& fb, const PolynomialContext& ctx, uint32_t bound,
                                     uint8_t log_scale,
                                     const std::vector<bool>* shared_sieve = nullptr);
    static void find_algebraic_primes(FactorBase& fb, const PolynomialContext& ctx, uint32_t bound,
                                      uint8_t log_scale,
                                      const std::vector<bool>* shared_sieve = nullptr);
    static void find_algebraic_primes_range(FactorBase& fb, const PolynomialContext& ctx,
                                            uint32_t min_p, uint32_t max_p, uint8_t log_scale);

    /// Extract roots from a polynomial known to be a product of distinct linear factors
    static std::vector<uint32_t> extract_roots_from_poly(const sqrt::ModularPoly& poly,
                                                         const std::vector<uint64_t>& f_mod,
                                                         uint32_t p);

    /// Root finder implementation for callers that already validated p as prime.
    static std::vector<uint32_t> find_roots_mod_p_unchecked(const PolynomialContext& ctx,
                                                            uint32_t p);

    /// Exact polynomial division: a / b mod p
    static sqrt::ModularPoly poly_div_mod(const sqrt::ModularPoly& a, const sqrt::ModularPoly& b,
                                          uint32_t p);
};

} // namespace gnfs::factor_base
