#pragma once

#include "../core/integer.hpp"
#include "../core/relation.hpp"
#include "../factor_base/factor_base.hpp"
#include "../linalg/sparse_matrix.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace gnfs::sqrt {

using core::Integer;
using core::Relation;
using factor_base::FactorBase;
using linalg::BitVector;

namespace detail {

/// Compute a modular power without narrowing a full-width exponent on LLP64.
/// GMP's mpz_powm_ui takes unsigned long, which is 32 bits on Windows while
/// uint64_t remains 64 bits. Keep the UI fast path where it is lossless and
/// use the arbitrary-precision API otherwise.
inline void powm_u64(mpz_t result, const mpz_t base, uint64_t exponent, const mpz_t mod) {
    if constexpr (std::numeric_limits<unsigned long>::digits >=
                  std::numeric_limits<uint64_t>::digits) {
        mpz_powm_ui(result, base, static_cast<unsigned long>(exponent), mod);
    } else {
        const Integer exponent_value(exponent);
        mpz_powm(result, base, exponent_value.get_mpz(), mod);
    }
}

/// Subtract m*b without narrowing a full-width relation parameter on LLP64.
inline void subtract_m_times_b(Integer& value, const Integer& m, uint64_t b) {
    if (b <= std::numeric_limits<unsigned long>::max()) {
        mpz_submul_ui(value.get_mpz(), m.get_mpz(), static_cast<unsigned long>(b));
        return;
    }

    const Integer b_value(b);
    mpz_submul(value.get_mpz(), m.get_mpz(), b_value.get_mpz());
}

} // namespace detail

/// 有理平方根计算结果
struct RationalSqrtResult {
    Integer value;        // 平方根值（模 N）
    bool success = false; // 是否成功
    std::string error;    // 错误信息
};

/// 有理平方根配置
struct RationalSqrtConfig {
    bool verify = true; // 是否验证结果
};

/// RationalSqrt - 计算有理侧的平方根
/// 给定一组关系，其乘积在有理侧是完全平方
class RationalSqrt {
public:
    using Config = RationalSqrtConfig;

    explicit RationalSqrt(const Config& config = Config{}) : config_(config) {}

    /// 从依赖和关系计算有理平方根
    /// @param dependency 依赖向量（指示哪些关系参与）
    /// @param relations 所有关系
    /// @param fb 因子基
    /// @param n 模数
    /// @param m 多项式根（用于计算 a - b*m 的符号，GNFS a - b·α 约定）
    /// @return 平方根结果（模 n）
    [[nodiscard]] RationalSqrtResult compute(const BitVector& dependency,
                                             const std::vector<Relation>& relations,
                                             const FactorBase& fb, const Integer& n,
                                             const Integer& m) const {

        RationalSqrtResult result;
        if (dependency.size() != relations.size()) {
            result.error = "Rational sqrt: dependency length does not match relation count";
            return result;
        }

        // 收集所有参与关系的有理因子
        // 累积每个素数的指数
        // Reserve基于 dependency.popcount() * 平均 factors per row:
        // FB ~10-15 / row, LP ~1-3 / row. popcount typical 64-256.
        // 上界 fallback 为 relations.size() (但只是上界, 实际 popcount * factor 多数情况下少
        // 1000×).
        const auto& rational_primes = fb.rational();
        const size_t pop = dependency.popcount();
        std::unordered_map<uint32_t, uint64_t> fb_exponents; // 因子基素数指数
        std::unordered_map<uint64_t, uint64_t> lp_exponents; // 大素数指数
        fb_exponents.reserve(std::min(pop * 15, relations.size()));
        lp_exponents.reserve(std::min(pop * 3, relations.size()));
        bool has_negative = false; // 是否有负数的 (a - b*m) 值

        for (size_t i = 0; i < relations.size(); ++i) {
            if (!dependency.test(i))
                continue;

            const auto& rel = relations[i];

            // 检查符号：(a - b*m) 是否为负 (GNFS convention)
            // For merged relations, check all constituent (a,b) pairs
            // a_minus_bm = a - m*b via mpz_submul_ui (fused FMS, drops bm)
            Integer a_minus_bm;
            auto check_sign = [&](int64_t a_val, uint64_t b_val) {
                a_minus_bm = a_val; // mpz_set_si direct
                detail::subtract_m_times_b(a_minus_bm, m, b_val);
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
                const uint32_t index = rel.rational_factors[j];
                if (index >= rational_primes.size()) {
                    result.error = "Rational sqrt: factor-base index out of range";
                    return result;
                }
                fb_exponents[index]++;
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

        // hoist p_int + contribution — 每个 dep 数千次迭代复用 buffer
        // powm_u64 keeps the exponent wide without giving up the LP64 fast path.
        Integer p_int, contribution;

        // 因子基素数贡献
        for (const auto& [idx, exp] : fb_exponents) {
            if (exp == 0)
                continue;

            uint32_t p = rational_primes[idx].p;
            uint64_t half_exp = exp / 2;

            // p^half_exp mod n — preserve the full uint64_t exponent on LLP64.
            p_int = uint64_t(p);
            detail::powm_u64(contribution.get_mpz(), p_int.get_mpz(), half_exp, n.get_mpz());

            sqrt_value *= contribution;
            sqrt_value %= n;
        }

        // 大素数贡献
        for (const auto& [p, exp] : lp_exponents) {
            if (exp == 0)
                continue;

            uint64_t half_exp = exp / 2;

            p_int = uint64_t(p);
            detail::powm_u64(contribution.get_mpz(), p_int.get_mpz(), half_exp, n.get_mpz());

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
            Integer val;
            auto multiply_ab = [&](int64_t a_val, uint64_t b_val) {
                val = a_val; // mpz_set_si direct
                // val -= m * b_val (fused FMS, b_val is unsigned)
                detail::subtract_m_times_b(val, m, b_val);
                val %= n;
                if (val.is_negative())
                    val += n;
                product *= val;
                product %= n;
            };
            for (size_t i = 0; i < relations.size(); ++i) {
                if (!dependency.test(i))
                    continue;
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
    [[nodiscard]] static Integer compute_from_exponents(const std::vector<uint64_t>& exponents,
                                                        const std::vector<uint32_t>& primes,
                                                        const Integer& n) {

        Integer result(1);
        // Hoist p + contribution; detail::powm_u64 keeps the UI fast path on
        // LP64 and avoids truncation on LLP64.
        Integer p, contribution;

        for (size_t i = 0; i < exponents.size() && i < primes.size(); ++i) {
            if (exponents[i] == 0)
                continue;

            // 指数应该是偶数
            uint64_t half_exp = exponents[i] / 2;

            p = uint64_t(primes[i]);
            detail::powm_u64(contribution.get_mpz(), p.get_mpz(), half_exp, n.get_mpz());

            result *= contribution;
            result %= n;
        }

        return result;
    }

private:
    Config config_;
};

/// 便捷函数：计算有理平方根
[[nodiscard]] inline RationalSqrtResult
compute_rational_sqrt(const BitVector& dependency, const std::vector<Relation>& relations,
                      const FactorBase& fb, const Integer& n, const Integer& m) {

    RationalSqrt calculator;
    return calculator.compute(dependency, relations, fb, n, m);
}

} // namespace gnfs::sqrt
