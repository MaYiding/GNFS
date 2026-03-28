#pragma once

#include "../core/integer.hpp"
#include "../core/relation.hpp"
#include "../factor_base/factor_base.hpp"
#include "../linalg/sparse_matrix.hpp"

#include <unordered_map>
#include <vector>

namespace gnfs {
namespace sqrt {

using core::Integer;
using core::Relation;
using factor_base::FactorBase;
using linalg::BitVector;

/// 有理平方根计算结果
struct RationalSqrtResult {
    Integer value;           // 平方根值（模 N）
    bool success = false;    // 是否成功
    std::string error;       // 错误信息
};

/// 有理平方根配置
struct RationalSqrtConfig {
    bool verify = true;      // 是否验证结果
};

/// RationalSqrt - 计算有理侧的平方根
/// 给定一组关系，其乘积在有理侧是完全平方
class RationalSqrt {
public:
    using Config = RationalSqrtConfig;

    explicit RationalSqrt(const Config& config = Config{})
        : config_(config) {}

    /// 从依赖和关系计算有理平方根
    /// @param dependency 依赖向量（指示哪些关系参与）
    /// @param relations 所有关系
    /// @param fb 因子基
    /// @param n 模数
    /// @param m 多项式根（用于计算 a + b*m 的符号）
    /// @return 平方根结果（模 n）
    [[nodiscard]] RationalSqrtResult compute(
            const BitVector& dependency,
            const std::vector<Relation>& relations,
            const FactorBase& fb,
            const Integer& n,
            const Integer& m) const {

        RationalSqrtResult result;

        // 收集所有参与关系的有理因子
        // 累积每个素数的指数
        std::unordered_map<uint32_t, uint64_t> fb_exponents;    // 因子基素数指数
        std::unordered_map<uint64_t, uint64_t> lp_exponents;    // 大素数指数
        bool has_negative = false;  // 是否有负数的 (a + b*m) 值

        for (size_t i = 0; i < relations.size(); ++i) {
            if (!dependency.test(i)) continue;

            const auto& rel = relations[i];

            // 检查符号：(a - b*m) 是否为负 (GNFS convention)
            Integer a_minus_bm = Integer(rel.a);
            Integer bm = m.clone();
            bm *= Integer(static_cast<int64_t>(rel.b));
            a_minus_bm -= bm;
            if (a_minus_bm.is_negative()) {
                has_negative = !has_negative;
            }

            // 因子基素数
            for (size_t j = 0; j < rel.rational_factors.size(); ++j) {
                fb_exponents[rel.rational_factors[j]]++;
            }

            // 大素数
            for (size_t j = 0; j < rel.rational_large_prime.size(); ++j) {
                lp_exponents[rel.rational_large_prime[j].p] += rel.rational_large_prime[j].e;
            }
        }

        // 检查所有指数是否为偶数
        for (const auto& [idx, exp] : fb_exponents) {
            if (exp % 2 != 0) {
                result.error = "Factor base exponent not even";
                return result;
            }
        }

        for (const auto& [p, exp] : lp_exponents) {
            if (exp % 2 != 0) {
                result.error = "Large prime exponent not even";
                return result;
            }
        }

        // 计算平方根（模 n）
        // sqrt = product of p^(exp/2) mod n
        Integer sqrt_value(1);

        // 因子基素数贡献
        for (const auto& [idx, exp] : fb_exponents) {
            if (exp == 0) continue;

            uint32_t p = fb.rational()[idx].p;
            uint64_t half_exp = exp / 2;

            // p^half_exp mod n
            Integer p_int(static_cast<unsigned long long>(p));
            Integer half_exp_int(static_cast<unsigned long long>(half_exp));
            Integer contribution = core::powmod(p_int, half_exp_int, n);

            sqrt_value *= contribution;
            sqrt_value %= n;
        }

        // 大素数贡献
        for (const auto& [p, exp] : lp_exponents) {
            if (exp == 0) continue;

            uint64_t half_exp = exp / 2;

            Integer p_int(static_cast<unsigned long long>(p));
            Integer half_exp_int(static_cast<unsigned long long>(half_exp));
            Integer contribution = core::powmod(p_int, half_exp_int, n);

            sqrt_value *= contribution;
            sqrt_value %= n;
        }

        // 如果有奇数个负数，平方根是负的
        // 在模算术中，-x ≡ n - x
        if (has_negative) {
            sqrt_value = n - sqrt_value;
        }

        // 验证: X² mod N == ∏(a_i - b_i·m) mod N
        if (config_.verify) {
            Integer squared = sqrt_value.clone();
            squared *= sqrt_value;
            squared %= n;

            Integer product(1);
            for (size_t i = 0; i < relations.size(); ++i) {
                if (!dependency.test(i)) continue;

                const auto& rel = relations[i];

                // 有理侧范数 = a - b*m (GNFS convention: elements are a - b·α)
                Integer val = Integer(rel.a);
                Integer bm = m.clone();
                bm *= Integer(static_cast<int64_t>(rel.b));
                val -= bm;
                val %= n;
                if (val.is_negative()) val += n;

                product *= val;
                product %= n;
            }

            if (squared.compare(product) != 0) {
                result.error = "Rational sqrt verification failed: X^2 != product mod N";
                return result;
            }
        }

        result.value = std::move(sqrt_value);
        result.success = true;

        return result;
    }

    /// 简化版：直接从指数向量计算
    /// @param exponents 每个素数的指数（应该都是偶数）
    /// @param primes 对应的素数
    /// @param n 模数
    [[nodiscard]] static Integer compute_from_exponents(
            const std::vector<uint64_t>& exponents,
            const std::vector<uint32_t>& primes,
            const Integer& n) {

        Integer result(1);

        for (size_t i = 0; i < exponents.size() && i < primes.size(); ++i) {
            if (exponents[i] == 0) continue;

            // 指数应该是偶数
            uint64_t half_exp = exponents[i] / 2;

            Integer p(static_cast<unsigned long long>(primes[i]));
            Integer e(static_cast<unsigned long long>(half_exp));
            Integer contribution = core::powmod(p, e, n);

            result *= contribution;
            result %= n;
        }

        return result;
    }

private:
    Config config_;
};

/// 便捷函数：计算有理平方根
[[nodiscard]] inline RationalSqrtResult compute_rational_sqrt(
        const BitVector& dependency,
        const std::vector<Relation>& relations,
        const FactorBase& fb,
        const Integer& n,
        const Integer& m) {

    RationalSqrt calculator;
    return calculator.compute(dependency, relations, fb, n, m);
}

} // namespace sqrt
} // namespace gnfs
