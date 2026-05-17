#pragma once

#include "../core/integer.hpp"
#include "../core/relation.hpp"
#include "../factor_base/factor_base.hpp"
#include "../linalg/sparse_matrix.hpp"

#include <unordered_map>
#include <vector>

namespace gnfs::sqrt {

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
    /// @param m 多项式根（用于计算 a - b*m 的符号，GNFS a - b·α 约定）
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
        // Reserve基于 dependency.popcount() * 平均 factors per row:
        // FB ~10-15 / row, LP ~1-3 / row. popcount typical 64-256.
        // 上界 fallback 为 relations.size() (但只是上界, 实际 popcount * factor 多数情况下少 1000×).
        const size_t pop = dependency.popcount();
        std::unordered_map<uint32_t, uint64_t> fb_exponents;    // 因子基素数指数
        std::unordered_map<uint64_t, uint64_t> lp_exponents;    // 大素数指数
        fb_exponents.reserve(std::min(pop * 15, relations.size()));
        lp_exponents.reserve(std::min(pop * 3, relations.size()));
        bool has_negative = false;  // 是否有负数的 (a - b*m) 值

        for (size_t i = 0; i < relations.size(); ++i) {
            if (!dependency.test(i)) continue;

            const auto& rel = relations[i];

            // 检查符号：(a - b*m) 是否为负 (GNFS convention)
            // For merged relations, check all constituent (a,b) pairs
            // v22: check_sign hot lambda - hoist a_minus_bm/bm buffers (mpz_set)
            Integer a_minus_bm, bm;
            auto check_sign = [&](int64_t a_val, uint64_t b_val) {
                a_minus_bm = a_val;  // mpz_set_si direct
                bm = m;
                bm *= static_cast<int64_t>(b_val);  // mpz_mul_si direct (b ≤ sieve bound)
                a_minus_bm -= bm;
                if (a_minus_bm.is_negative()) {
                    has_negative = !has_negative;
                }
            };
            check_sign(rel.a, rel.b);
            for (const auto& [ea, eb] : rel.extra_ab_pairs) {
                check_sign(ea, eb);
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

        // hoist p_int + half_exp_int — 每个 dep 数千次迭代复用 buffer
        Integer p_int, half_exp_int;

        // 因子基素数贡献
        for (const auto& [idx, exp] : fb_exponents) {
            if (exp == 0) continue;

            uint32_t p = fb.rational()[idx].p;
            uint64_t half_exp = exp / 2;

            // p^half_exp mod n
            p_int = static_cast<uint64_t>(p);
            half_exp_int = static_cast<uint64_t>(half_exp);
            Integer contribution = core::powmod(p_int, half_exp_int, n);

            sqrt_value *= contribution;
            sqrt_value %= n;
        }

        // 大素数贡献
        for (const auto& [p, exp] : lp_exponents) {
            if (exp == 0) continue;

            uint64_t half_exp = exp / 2;

            p_int = static_cast<uint64_t>(p);
            half_exp_int = static_cast<uint64_t>(half_exp);
            Integer contribution = core::powmod(p_int, half_exp_int, n);

            sqrt_value *= contribution;
            sqrt_value %= n;
        }

        // 如果有奇数个负数，平方根是负的
        // 在模算术中，-x ≡ n - x
        if (has_negative) {
            sqrt_value = n - sqrt_value;
        }

        // 验证: X² mod N == ∏|a_i - b_i·m| mod N
        // 注意: FB/LP 因子分解的是绝对值 |a - b·m|，所以 X² = ∏|a_i - b_i·m|。
        // 而 ∏(a_i - b_i·m) = (-1)^k × ∏|a_i - b_i·m|（k = 负因子个数）。
        // 当 k 为奇数时 product mod N = N - |product| mod N ≠ X²。
        // 因此比较时需要使用绝对值乘积，或者在 k 为奇数时翻转 product。
        if (config_.verify) {
            // v22: squared 直接 assign; verify_ab buffers hoisted
            Integer squared;
            squared = sqrt_value;
            squared *= sqrt_value;
            squared %= n;

            Integer product(1);
            Integer val, bm_v;
            auto multiply_ab = [&](int64_t a_val, uint64_t b_val) {
                val = a_val;  // mpz_set_si direct
                bm_v = m;
                bm_v *= static_cast<int64_t>(b_val);  // mpz_mul_si direct
                val -= bm_v;
                val %= n;
                if (val.is_negative()) val += n;
                product *= val;
                product %= n;
            };
            for (size_t i = 0; i < relations.size(); ++i) {
                if (!dependency.test(i)) continue;
                const auto& rel = relations[i];
                multiply_ab(rel.a, rel.b);
                for (const auto& [ea, eb] : rel.extra_ab_pairs) {
                    multiply_ab(ea, eb);
                }
            }

            // 当负因子个数为奇数时，product = N - |product|
            // X² = |product|，所以需要翻转 product 再比较
            if (has_negative) {
                product = n - product;
                if (product.compare(n) == 0) {
                    product = int64_t(0);
                }
            }

            // 退化乘积 ≡ 0 (mod N) 表示某个 (a - b·m) 与 N 不互素。
            // 让它沉默通过会让上层算出 X=0,gcd(X-Y, N)=N 平凡。
            // RelationCollector 应在入口拒绝 gcd(a-bm, N)>1 的关系,但 bypass
            // (load()/merge()/直接 add()) 可能跳过校验,这里加最后一道防线。
            if (product.is_zero()) {
                result.error = "Rational sqrt: degenerate product ≡ 0 (mod N), "
                               "relation set contains a - b·m divisible by N";
                return result;
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
        // hoist p, e buffers — reused across iterations
        Integer p, e;

        for (size_t i = 0; i < exponents.size() && i < primes.size(); ++i) {
            if (exponents[i] == 0) continue;

            // 指数应该是偶数
            uint64_t half_exp = exponents[i] / 2;

            p = static_cast<uint64_t>(primes[i]);
            e = static_cast<uint64_t>(half_exp);
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

} // namespace gnfs::sqrt
