#pragma once

#include "../core/integer.hpp"
#include "../core/types.hpp"
#include "../factor_base/factor_base.hpp"

#include <cstdint>
#include <vector>

namespace gnfs::cofactor {

using core::Integer;
using core::PrimePower;
using factor_base::FactorBase;

/// 试除结果
struct TrialDivisionResult {
    std::vector<uint32_t> factor_indices;   // 因子基中的索引
    std::vector<uint8_t> exponents;         // 对应的指数
    Integer cofactor;                        // 剩余的未分解部分
    bool is_smooth = false;                  // cofactor == 1?

    /// 是否完全分解
    [[nodiscard]] bool fully_factored() const noexcept {
        return is_smooth || (cofactor.fits_uint64() && cofactor.to_uint64() == 1);
    }
};

/// TrialDivider - 试除法分解器
/// 使用因子基中的素数进行试除
class TrialDivider {
public:
    /// 构造函数
    /// @param fb 因子基
    explicit TrialDivider(const FactorBase& fb) : fb_(fb) {}

    /// 对有理侧值进行试除
    /// @param value 有理侧值 |a - b*m|
    /// @return 试除结果
    [[nodiscard]] TrialDivisionResult divide_rational(Integer value) const {
        // uint64 快路径：小值避免 GMP 开销
        if (value.fits_uint64()) {
            return divide_rational_u64(value.to_uint64());
        }

        TrialDivisionResult result;
        if (value.is_negative()) value.negate();

        if (value.is_zero()) {
            result.is_smooth = true;
            result.cofactor = Integer(static_cast<int64_t>(1));
            return result;
        }

        const auto& rationals = fb_.rational();
        for (uint32_t idx = 0; idx < rationals.size(); ++idx) {
            uint32_t p = rationals[idx].p;
            uint8_t exp = 0;
            while (divisible_by(value, p) && exp < 255) {
                divide_exact(value, p);
                ++exp;
            }
            if (exp > 0) {
                result.factor_indices.push_back(idx);
                result.exponents.push_back(exp);
            }
            if (value.fits_uint64() && value.to_uint64() == 1) {
                result.is_smooth = true;
                break;
            }
            // 切换到 uint64 快路径
            if (value.fits_uint64()) {
                auto tail = divide_rational_u64_from(value.to_uint64(), idx + 1);
                result.factor_indices.insert(result.factor_indices.end(),
                    tail.factor_indices.begin(), tail.factor_indices.end());
                result.exponents.insert(result.exponents.end(),
                    tail.exponents.begin(), tail.exponents.end());
                result.cofactor = std::move(tail.cofactor);
                result.is_smooth = tail.is_smooth;
                return result;
            }
        }

        result.cofactor = std::move(value);
        if (result.cofactor.fits_uint64() && result.cofactor.to_uint64() == 1) {
            result.is_smooth = true;
        }
        return result;
    }

    /// 纯 uint64 有理试除（零 GMP 分配）
    [[nodiscard]] TrialDivisionResult divide_rational_u64(uint64_t value) const {
        return divide_rational_u64_from(value, 0);
    }

    /// 对代数侧范数进行试除
    /// @param norm 代数侧范数 |N(a, b)|
    /// @param a, b 用于确定哪个根 r 整除
    /// @param max_entries 最大遍历条目数 (0 = all)。传 sieve_algebraic_count()
    ///   可跳过 SQ 范围条目，让 classify_cofactor 处理它们。
    /// @return 试除结果
    [[nodiscard]] TrialDivisionResult divide_algebraic(
            Integer norm, int64_t a, uint64_t b, size_t max_entries = 0) const {

        TrialDivisionResult result;

        // 处理符号
        if (norm.is_negative()) {
            norm.negate();
        }

        if (norm.is_zero()) {
            result.is_smooth = true;
            result.cofactor = Integer(static_cast<int64_t>(1));
            return result;
        }

        // 对每个代数因子基素理想进行试除
        const auto& algebraics = fb_.algebraic();
        size_t alg_limit = (max_entries > 0 && max_entries < algebraics.size())
                         ? max_entries : algebraics.size();

        // Three-tier fast path: uint64 → uint128 → GMP
        bool use_u64 = norm.fits_uint64();
        uint64_t norm_u64 = use_u64 ? norm.to_uint64() : 0;
        bool use_u128 = false;
        __uint128_t norm_u128 = 0;
        if (!use_u64 && norm.bit_length() <= 127) {
            use_u128 = true;
            mpz_t tmp;
            mpz_init(tmp);
            mpz_tdiv_r_2exp(tmp, norm.get_mpz(), 64);
            uint64_t lo = mpz_get_ui(tmp);
            mpz_tdiv_q_2exp(tmp, norm.get_mpz(), 64);
            uint64_t hi = mpz_get_ui(tmp);
            mpz_clear(tmp);
            norm_u128 = (static_cast<__uint128_t>(hi) << 64) | lo;
        }

        for (uint32_t idx = 0; idx < alg_limit; ++idx) {
            uint32_t p = algebraics[idx].p;
            uint32_t r = algebraics[idx].r;

            // Fast pre-check: skip primes that don't divide the norm.
            // Three-tier: uint64 % p, uint128 % p, or GMP mpz_divisible_ui_p.
            if (use_u64) {
                if (norm_u64 % p != 0) continue;
            } else if (use_u128) {
                if (norm_u128 % p != 0) continue;
            } else {
                if (mpz_divisible_ui_p(norm.get_mpz(), p) == 0) continue;
            }

            // 检查 P | (a - bα):
            // Normal root: P = (p, α - r), condition: a - b*r ≡ 0 (mod p)
            // Projective root: P = (p, ∞), condition: b ≡ 0 (mod p)
            if (r == core::AlgebraicPrime::PROJECTIVE_ROOT) {
                if (b % p != 0) continue;
            } else {
                // Compute a ≡ b·r (mod p) using mod-first to avoid int64 overflow
                // when b > 2^31 and r is large.
                uint64_t p64 = p;
                uint64_t b_mod = b % p64;
                uint64_t br_mod = (b_mod * static_cast<uint64_t>(r)) % p64;
                // a mod p (handle negative a)
                int64_t a_mod = a % static_cast<int64_t>(p);
                if (a_mod < 0) a_mod += p;
                if (static_cast<uint64_t>(a_mod) != br_mod) continue;
            }

            // 试除 — uint64 / uint128 / GMP 快路径
            uint8_t exp = 0;
            if (use_u64) {
                while (norm_u64 % p == 0 && exp < 255) {
                    norm_u64 /= p;
                    ++exp;
                }
            } else if (use_u128) {
                while (norm_u128 % p == 0 && exp < 255) {
                    norm_u128 /= p;
                    ++exp;
                }
                // Check if we can downgrade to uint64
                if (norm_u128 <= UINT64_MAX) {
                    use_u64 = true;
                    use_u128 = false;
                    norm_u64 = static_cast<uint64_t>(norm_u128);
                }
            } else {
                while (divisible_by(norm, p) && exp < 255) {
                    divide_exact(norm, p);
                    ++exp;
                }
                // Check for upgrade to uint128 or uint64
                if (norm.fits_uint64()) {
                    use_u64 = true;
                    norm_u64 = norm.to_uint64();
                } else if (norm.bit_length() <= 127) {
                    use_u128 = true;
                    mpz_t tmp;
                    mpz_init(tmp);
                    mpz_tdiv_r_2exp(tmp, norm.get_mpz(), 64);
                    uint64_t lo = mpz_get_ui(tmp);
                    mpz_tdiv_q_2exp(tmp, norm.get_mpz(), 64);
                    uint64_t hi = mpz_get_ui(tmp);
                    mpz_clear(tmp);
                    norm_u128 = (static_cast<__uint128_t>(hi) << 64) | lo;
                }
            }

            if (exp > 0) {
                result.factor_indices.push_back(idx);
                result.exponents.push_back(exp);
            }

            // 早期退出: cofactor == 1 → fully smooth
            if (use_u64 && norm_u64 == 1) {
                result.is_smooth = true;
                break;
            }
            if (use_u128 && norm_u128 == 1) {
                result.is_smooth = true;
                break;
            }
            // 早期退出: cofactor < p → can't be divided by any remaining FB prime
            if (use_u64 && norm_u64 > 0 && norm_u64 < p) {
                break;
            }
            // p² 早停: cofactor < p² 且 cofactor > algebraic_bound
            // cofactor 是素数且不在 FB 中 → 安全退出
            if (use_u64 && norm_u64 > fb_.params().algebraic_bound &&
                norm_u64 < static_cast<uint64_t>(p) * p) {
                break;
            }
        }

        if (use_u64) {
            result.cofactor = Integer(norm_u64);
            if (norm_u64 == 1) result.is_smooth = true;
        } else if (use_u128) {
            // Convert uint128 back to Integer
            if (norm_u128 <= UINT64_MAX) {
                result.cofactor = Integer(static_cast<uint64_t>(norm_u128));
            } else {
                // Construct Integer from uint128
                uint64_t hi = static_cast<uint64_t>(norm_u128 >> 64);
                uint64_t lo = static_cast<uint64_t>(norm_u128);
                mpz_t tmp;
                mpz_init(tmp);
                mpz_set_ui(tmp, hi);
                mpz_mul_2exp(tmp, tmp, 64);
                mpz_add_ui(tmp, tmp, lo);
                result.cofactor = Integer(0);
                mpz_set(result.cofactor.get_mpz(), tmp);
                mpz_clear(tmp);
            }
            if (norm_u128 == 1) result.is_smooth = true;
        } else {
            result.cofactor = std::move(norm);
            if (result.cofactor.fits_uint64() && result.cofactor.to_uint64() == 1) {
                result.is_smooth = true;
            }
        }

        return result;
    }

    /// 快速检查是否可能光滑（不完全分解，只检查小素数）
    /// @param value 要检查的值
    /// @param limit 检查的素数数量限制
    /// @return 剩余的 cofactor
    [[nodiscard]] Integer quick_divide(Integer value, size_t limit = 100) const {
        if (value.is_negative()) {
            value.negate();
        }

        const auto& rationals = fb_.rational();
        size_t count = std::min(limit, rationals.size());

        for (size_t i = 0; i < count; ++i) {
            uint32_t p = rationals[i].p;

            while (divisible_by(value, p)) {
                divide_exact(value, p);
            }

            if (value.fits_uint64() && value.to_uint64() == 1) {
                break;
            }
        }

        return value;
    }

private:
    const FactorBase& fb_;

    /// 检查 value 是否被 p 整除
    [[nodiscard]] static bool divisible_by(const Integer& value, uint32_t p) {
        if (value.fits_uint64()) {
            return value.to_uint64() % p == 0;
        }
        // 使用 GMP 的模运算 - get_mpz() returns mpz_t& which decays to mpz_ptr
        return mpz_divisible_ui_p(value.get_mpz(), p) != 0;
    }

    /// value /= p（原地除法）
    static void divide_exact(Integer& value, uint32_t p) {
        if (value.fits_uint64()) {
            uint64_t v = value.to_uint64() / p;
            value = Integer(v);
        } else {
            // get_mpz() returns mpz_t& which decays to mpz_ptr
            mpz_divexact_ui(value.get_mpz(), value.get_mpz(), p);
        }
    }

    /// 纯 uint64 有理试除（从指定 FB 索引开始）
    /// 零 GMP 分配——整个流程在原生 uint64 算术内完成
    [[nodiscard]] TrialDivisionResult divide_rational_u64_from(uint64_t value, uint32_t start_idx) const {
        TrialDivisionResult result;

        if (value == 0) {
            result.is_smooth = true;
            result.cofactor = Integer(static_cast<int64_t>(1));
            return result;
        }

        const auto& rationals = fb_.rational();
        for (uint32_t idx = start_idx; idx < rationals.size(); ++idx) {
            uint32_t p = rationals[idx].p;

            uint8_t exp = 0;
            while (value % p == 0 && exp < 255) {
                value /= p;
                ++exp;
            }
            if (exp > 0) {
                result.factor_indices.push_back(idx);
                result.exponents.push_back(exp);
            }
            if (value == 1) {
                result.is_smooth = true;
                break;
            }
            // 早期退出: cofactor < next_p → 不可能被任何剩余 FB 素数整除
            if (idx + 1 < rationals.size()) {
                uint64_t next_p = rationals[idx + 1].p;
                if (value < next_p) {
                    break;
                }
            }
            // p² 早停: cofactor < p² 且 cofactor > rational_bound
            // cofactor 是素数且不在 FB 中 → 安全退出
            if (value > fb_.params().rational_bound &&
                value < static_cast<uint64_t>(p) * p) {
                break;
            }
        }

        result.cofactor = Integer(value);
        if (value == 1) result.is_smooth = true;
        return result;
    }
};

/// 批量试除器 - 对多个候选同时进行试除
class BatchTrialDivider {
public:
    /// 构造函数
    explicit BatchTrialDivider(const FactorBase& fb) : divider_(fb) {}

    /// 批量处理
    template <typename Iter, typename OutputIter>
    void divide_batch(Iter begin, Iter end, OutputIter out) const {
        for (auto it = begin; it != end; ++it) {
            *out++ = divider_.divide_rational(*it);
        }
    }

private:
    TrialDivider divider_;
};

/// 计算有理侧值 |a - b*m| (GNFS convention)
[[nodiscard]] inline Integer compute_rational_value(
        int64_t a, uint64_t b, const Integer& m) {

    Integer result(a);
    Integer bm = m.clone();
    bm *= static_cast<long long>(b);
    result -= bm;

    if (result.is_negative()) {
        result.negate();
    }

    return result;
}

/// 计算代数侧范数 |N(a - bα)| (GNFS convention)
/// N(a - bα) = b^d * f(a/b)
/// 展开形式: N = |sum_{i=0}^{d} f_i * a^i * b^{d-i}|
[[nodiscard]] inline Integer compute_algebraic_norm(
        int64_t a, uint64_t b,
        const std::vector<Integer>& coeffs,
        uint32_t degree) {

    Integer result(static_cast<int64_t>(0));
    Integer a_power(static_cast<int64_t>(1));  // a^i

    // 计算 b^d, b^{d-1}, ..., b^0
    std::vector<Integer> b_powers(degree + 1);
    b_powers[0] = Integer(static_cast<int64_t>(1));
    for (uint32_t i = 1; i <= degree; ++i) {
        b_powers[i] = b_powers[i - 1].clone();
        b_powers[i] *= static_cast<long long>(b);
    }

    for (uint32_t i = 0; i <= degree; ++i) {
        // term = f_i * a^i * b^{d-i}
        // Note: No sign alternation for N(a - bα) = b^d * f(a/b)
        Integer term = coeffs[i].clone();
        term *= a_power;
        term *= b_powers[degree - i];

        result += term;

        // 更新 a^i
        if (i < degree) {
            a_power *= a;
        }
    }

    if (result.is_negative()) {
        result.negate();
    }

    return result;
}

} // namespace gnfs::cofactor
