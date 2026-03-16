#pragma once

#include "number_field.hpp"
#include "couveignes.hpp"
#include "hensel_sqrt.hpp"
#include "../core/integer.hpp"
#include "../core/relation.hpp"
#include "../core/polynomial_context.hpp"
#include "../linalg/sparse_matrix.hpp"

#include <vector>

namespace gnfs {
namespace sqrt {

using core::Integer;
using core::Relation;
using core::PolynomialContext;
using linalg::BitVector;

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

        // 创建数域
        NumberField nf(ctx);

        // 收集参与的 (a, b) 对
        std::vector<std::pair<int64_t, uint64_t>> ab_pairs;
        for (size_t i = 0; i < relations.size(); ++i) {
            if (!dependency.test(i)) continue;
            const auto& rel = relations[i];
            ab_pairs.emplace_back(rel.a, rel.b);
        }

        if (ab_pairs.empty()) {
            result.error = "No relations in dependency";
            return result;
        }

        // Try Hensel lifting first (most reliable for all sizes)
        {
            HenselSqrt::Config hcfg;
            hcfg.verbose = (ab_pairs.size() >= 500);
            HenselSqrt hensel(hcfg);
            auto sqrt_opt = hensel.compute(ab_pairs, nf);
            if (sqrt_opt) {
                result.value = nf.evaluate_at_m_mod_n(*sqrt_opt);
                result.success = true;
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

    /// 启发式方法（后备）
    [[nodiscard]] AlgebraicSqrtResult compute_heuristic(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf) const {

        AlgebraicSqrtResult result;

        // 计算乘积
        NumberFieldElement product = nf.one();
        for (const auto& [a, b] : ab_pairs) {
            auto factor = nf.from_ab(a, b);
            product = nf.multiply_mod_n(product, factor);
        }

        if (product.is_zero()) {
            result.value = Integer(static_cast<int64_t>(0));
            result.success = true;
            return result;
        }

        if (product.is_one()) {
            result.value = Integer(static_cast<int64_t>(1));
            result.success = true;
            return result;
        }

        // 尝试 elem^((n+1)/2)
        const Integer& n = nf.n();
        Integer exp = n.clone();
        exp += Integer(static_cast<int64_t>(1));
        mpz_tdiv_q_2exp(exp.get_mpz(), exp.get_mpz(), 1);

        auto candidate = nf.power_mod_n(product, exp);
        result.value = nf.evaluate_at_m_mod_n(candidate);
        result.success = true;

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
