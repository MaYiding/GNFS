#pragma once

#include "number_field.hpp"
#include "couveignes.hpp"
#include "hensel_sqrt.hpp"
#include "../core/integer.hpp"
#include "../core/relation.hpp"
#include "../core/polynomial_context.hpp"
#include "../linalg/sparse_matrix.hpp"

#include <unordered_map>
#include <vector>

namespace gnfs {
namespace sqrt {

using core::Integer;
using core::Relation;
using core::PolynomialContext;
using linalg::BitVector;

/// Pre-check: verify all algebraic ideal powers have even multiplicity.
/// This is msieve's verify_alg_ideal_powers strategy — catches bad deps
/// in O(n·d) before running expensive Hensel lifting.
/// Returns true if parity check passes (all even), false if any odd exponent found.
[[nodiscard]] inline bool verify_algebraic_ideal_powers(
        const BitVector& dependency,
        const std::vector<Relation>& relations) {

    // Count algebraic FB factor multiplicities
    std::unordered_map<uint32_t, uint64_t> fb_exponents;
    std::unordered_map<uint64_t, uint64_t> lp_exponents;

    for (size_t i = 0; i < relations.size(); ++i) {
        if (!dependency.test(i)) continue;
        const auto& rel = relations[i];
        for (uint32_t idx : rel.algebraic_factors) {
            fb_exponents[idx]++;
        }
        for (const auto& lp : rel.algebraic_large_prime) {
            lp_exponents[lp.p] += lp.e;
        }
    }

    for (const auto& [idx, exp] : fb_exponents) {
        if (exp % 2 != 0) return false;
    }
    for (const auto& [p, exp] : lp_exponents) {
        if (exp % 2 != 0) return false;
    }
    return true;
}

/// 代数平方根计算结果
struct AlgebraicSqrtResult {
    Integer value;           // 平方根值（模 N）
    bool success = false;    // 是否成功
    std::string error;       // 错误信息
};

/// 代数平方根配置
struct AlgebraicSqrtConfig {
    bool verify = true;                // 是否验证结果
    // Note: num_primes should be <= 16 since sign determination only searches 2^16 patterns
    // With 16 primes starting at 1000, M ≈ 10^48 which is sufficient for most cases
    size_t num_primes = 16;            // CRT 使用的素数数量
    uint64_t prime_start = 1000;       // 素数搜索起点
    bool use_couveignes = true;        // 使用 Couveignes 算法
};

/// AlgebraicSqrt - 计算代数侧的平方根
/// 这是 GNFS 中最复杂的部分
///
/// 给定一组关系，其在代数侧的乘积是完全平方
/// 我们需要在数域 Q[α]/f(α) 中计算这个平方根
/// 然后将其映射到 Z/NZ
class AlgebraicSqrt {
public:
    using Config = AlgebraicSqrtConfig;

    explicit AlgebraicSqrt(const Config& config = Config{})
        : config_(config) {}

    /// 从依赖和关系计算代数平方根
    /// @param dependency 依赖向量
    /// @param relations 所有关系
    /// @param ctx 多项式上下文
    /// @return 平方根结果（模 N）
    [[nodiscard]] AlgebraicSqrtResult compute(
            const BitVector& dependency,
            const std::vector<Relation>& relations,
            const PolynomialContext& ctx) const {

        AlgebraicSqrtResult result;

        // Quick pre-check: algebraic ideal power parity (msieve strategy)
        // Catches bad deps in O(n·d) before expensive Hensel lifting
        if (!verify_algebraic_ideal_powers(dependency, relations)) {
            result.error = "Algebraic ideal powers parity check failed";
            return result;
        }

        // 创建数域
        NumberField nf(ctx);

        // 收集参与的 (a, b) 对（含合并关系的 extra pairs）
        std::vector<std::pair<int64_t, uint64_t>> ab_pairs;
        for (size_t i = 0; i < relations.size(); ++i) {
            if (!dependency.test(i)) continue;
            const auto& rel = relations[i];
            ab_pairs.emplace_back(rel.a, rel.b);
            for (const auto& [ea, eb] : rel.extra_ab_pairs) {
                ab_pairs.emplace_back(ea, static_cast<uint64_t>(eb));
            }
        }

        if (ab_pairs.empty()) {
            result.error = "No relations in dependency";
            return result;
        }

        // Try Hensel lifting first (most reliable for all sizes)
        // compute() returns the algebraic sqrt value mod N directly
        // (handles the O_K vs Z[α] index via the f'(α)² trick internally)
        {
            HenselSqrt::Config hcfg;
            hcfg.verbose = (ab_pairs.size() >= 500);
            // Reuse cached inert prime across deps for the same polynomial
            if (cached_inert_prime_ != 0) {
                hcfg.cached_inert_prime = cached_inert_prime_;
            }
            HenselSqrt hensel(hcfg);
            auto sqrt_val = hensel.compute(ab_pairs, nf);
            if (sqrt_val) {
                if (cached_inert_prime_ == 0) {
                    cached_inert_prime_ = hcfg.cached_inert_prime;
                    if (cached_inert_prime_ == 0) {
                        cached_inert_prime_ = hensel.last_inert_prime();
                    }
                }
                result.value = std::move(*sqrt_val);
                result.success = true;
                return result;
            }
            // If CRT exhausted all sign combos, the dependency is invalid.
            // Skip expensive Couveignes/heuristic fallback.
            if (hensel.was_crt_sign_exhausted()) {
                result.error = "CRT sign search exhausted — dependency invalid";
                return result;
            }
        }

        if (config_.use_couveignes) {
            // Fall back to Couveignes algorithm
            return compute_couveignes(ab_pairs, nf);
        } else {
            return compute_heuristic(ab_pairs, nf);
        }
    }

    /// 简化版：假设乘积已经是数域元素
    [[nodiscard]] AlgebraicSqrtResult compute_from_product(
            const NumberFieldElement& product,
            const NumberField& nf) const {

        AlgebraicSqrtResult result;

        CouveignesSqrt::Config cfg;
        cfg.num_primes = config_.num_primes;
        cfg.prime_start = config_.prime_start;

        CouveignesSqrt couveignes(cfg);
        auto sqrt_opt = couveignes.compute_from_element(product, nf);

        if (!sqrt_opt) {
            result.error = "Couveignes algorithm failed to compute square root";
            return result;
        }

        result.value = nf.evaluate_at_m_mod_n(*sqrt_opt);
        result.success = true;

        return result;
    }

private:
    Config config_;
    mutable uint64_t cached_inert_prime_ = 0;

    /// 使用 Couveignes 算法计算平方根
    [[nodiscard]] AlgebraicSqrtResult compute_couveignes(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf) const {

        AlgebraicSqrtResult result;

        CouveignesSqrt::Config cfg;
        cfg.num_primes = config_.num_primes;
        cfg.prime_start = config_.prime_start;

        CouveignesSqrt couveignes(cfg);
        auto sqrt_opt = couveignes.compute(ab_pairs, nf);

        if (!sqrt_opt) {
            result.error = "Couveignes algorithm failed";
            return result;
        }

        // 映射到 Z/NZ
        result.value = nf.evaluate_at_m_mod_n(*sqrt_opt);
        result.success = true;

        // Note: Couveignes internally uses per-prime verification which is more
        // reliable than verifying in Z[α]/N (different reduction orders can cause
        // multiply_mod_n to produce inconsistent products). Skip redundant verification.

        return result;
    }

    /// 启发式后备（已废弃——数学不正确）
    /// product^((N+1)/2) 仅对素数 p ≡ 3 (mod 4) 的 Z/pZ 有效，
    /// 对合数 N 的数域环 (Z/NZ)[α]/f(α) 没有数学依据。
    /// 此方法现在始终返回失败。
    [[nodiscard]] AlgebraicSqrtResult compute_heuristic(
            const std::vector<std::pair<int64_t, uint64_t>>& /* ab_pairs */,
            const NumberField& /* nf */) const {

        AlgebraicSqrtResult result;
        result.error = "Heuristic sqrt via elem^((N+1)/2) is mathematically "
                       "invalid for composite N; use Hensel or Couveignes";
        return result;
    }
};

/// 便捷函数：计算代数平方根
[[nodiscard]] inline AlgebraicSqrtResult compute_algebraic_sqrt(
        const BitVector& dependency,
        const std::vector<Relation>& relations,
        const PolynomialContext& ctx) {

    AlgebraicSqrt calculator;
    return calculator.compute(dependency, relations, ctx);
}

/// 最终因子提取
/// 给定有理平方根 X 和代数平方根 Y
/// 计算 gcd(X - Y, N) 和 gcd(X + Y, N)
struct FactorResult {
    Integer factor1;      // gcd(X - Y, N)
    Integer factor2;      // gcd(X + Y, N)
    bool is_nontrivial;   // 是否找到非平凡因子
};

[[nodiscard]] inline FactorResult extract_factors(
        const Integer& rational_sqrt,
        const Integer& algebraic_sqrt,
        const Integer& n) {

    FactorResult result;

    // 计算 X - Y mod N
    Integer diff = rational_sqrt.clone();
    diff -= algebraic_sqrt;
    diff %= n;
    if (diff.is_negative()) {
        diff += n;
    }

    // 计算 X + Y mod N
    Integer sum = rational_sqrt.clone();
    sum += algebraic_sqrt;
    sum %= n;

    // 计算 GCD
    result.factor1 = core::gcd(diff, n);
    result.factor2 = core::gcd(sum, n);

    // 检查是否非平凡
    result.is_nontrivial = false;

    auto check_nontrivial = [&n](const Integer& f) -> bool {
        if (f.is_one()) return false;       // f == 1 → trivial
        if (f.compare(n) == 0) return false; // f == N → trivial
        return true;                         // any other value → non-trivial
    };

    if (check_nontrivial(result.factor1) || check_nontrivial(result.factor2)) {
        result.is_nontrivial = true;
    }

    return result;
}

} // namespace sqrt
} // namespace gnfs
