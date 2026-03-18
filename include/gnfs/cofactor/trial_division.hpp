#pragma once

#include "../core/integer.hpp"
#include "../core/types.hpp"
#include "../factor_base/factor_base.hpp"

#include <cstdint>
#include <vector>

namespace gnfs {
namespace cofactor {

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
    /// @param value 有理侧值 |a + b*m|
    /// @return 试除结果
    [[nodiscard]] TrialDivisionResult divide_rational(Integer value) const {
        TrialDivisionResult result;

        // 处理符号
        if (value.is_negative()) {
            value.negate();
        }

        if (value.is_zero()) {
            result.is_smooth = true;
            result.cofactor = Integer(static_cast<int64_t>(1));
            return result;
        }

        // 对每个有理因子基素数进行试除
        const auto& rationals = fb_.rational();

        for (uint32_t idx = 0; idx < rationals.size(); ++idx) {
            uint32_t p = rationals[idx].p;

            uint8_t exp = 0;
            while (divisible_by(value, p)) {
                divide_exact(value, p);
                ++exp;
            }

            if (exp > 0) {
                result.factor_indices.push_back(idx);
                result.exponents.push_back(exp);
            }

            // 早期退出：如果 value 变成 1
            if (value.fits_uint64() && value.to_uint64() == 1) {
                result.is_smooth = true;
                break;
            }

            // 早期退出：如果 value < next_p^2，则 value 是素数
            // 但只有当下一个素数存在且 value < next_p 时才退出
            // 这样可以确保如果 value 本身是因子基中的素数，我们仍会尝试它
            if (idx + 1 < rationals.size() && value.fits_uint64()) {
                uint64_t v = value.to_uint64();
                uint32_t next_p = rationals[idx + 1].p;
                if (v < next_p) {
                    // value 比下一个因子基素数小，不可能被它整除
                    break;
                }
            }
        }

        result.cofactor = std::move(value);
        if (result.cofactor.fits_uint64() && result.cofactor.to_uint64() == 1) {
            result.is_smooth = true;
        }

        return result;
    }

    /// 对代数侧范数进行试除
    /// @param norm 代数侧范数 |N(a, b)|
    /// @param a, b 用于确定哪个根 r 整除
    /// @return 试除结果
    [[nodiscard]] TrialDivisionResult divide_algebraic(
            Integer norm, int64_t a, uint64_t b) const {

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

        for (uint32_t idx = 0; idx < algebraics.size(); ++idx) {
            uint32_t p = algebraics[idx].p;
            uint32_t r = algebraics[idx].r;

            // 检查 P | (a - bα) where P = (p, α - r)
            // Condition: a - b*r ≡ 0 (mod p)
            int64_t check = a - static_cast<int64_t>(b) * static_cast<int64_t>(r);
            int64_t mod = check % static_cast<int64_t>(p);
            if (mod < 0) mod += p;

            if (mod != 0) {
                continue;  // 这个素理想不整除
            }

            // 试除
            uint8_t exp = 0;
            while (divisible_by(norm, p)) {
                divide_exact(norm, p);
                ++exp;
            }

            if (exp > 0) {
                result.factor_indices.push_back(idx);
                result.exponents.push_back(exp);
            }

            // 早期退出
            if (norm.fits_uint64() && norm.to_uint64() == 1) {
                result.is_smooth = true;
                break;
            }
        }

        result.cofactor = std::move(norm);
        if (result.cofactor.fits_uint64() && result.cofactor.to_uint64() == 1) {
            result.is_smooth = true;
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

} // namespace cofactor
} // namespace gnfs
