#pragma once

#include "integer.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace gnfs::core {

/// PolynomialContext - 多项式上下文
/// 存储 GNFS 所需的多项式信息，被 factor_base 和 sieve 模块共用
class PolynomialContext {
public:
    /// 默认构造
    PolynomialContext() = default;

    /// 从系数构造
    /// @param n 待分解的数
    /// @param f_coeffs f(x) 的系数，f_coeffs[i] 是 x^i 的系数
    /// @param m 使得 f(m) = 0 (mod n)
    /// @param skewness skewness 参数
    PolynomialContext(Integer n,
                      std::vector<Integer> f_coeffs,
                      Integer m,
                      double skewness = 1.0)
        : n_(std::move(n))
        , f_coeffs_(std::move(f_coeffs))
        , m_(std::move(m))
        , skewness_(skewness) {
        if (f_coeffs_.empty()) {
            throw std::invalid_argument("Polynomial must have at least one coefficient");
        }
        degree_ = static_cast<uint32_t>(f_coeffs_.size() - 1);

        // 移除高次零系数
        while (degree_ > 0 && f_coeffs_[degree_].is_zero()) {
            --degree_;
        }
    }

    // 移动构造和赋值
    PolynomialContext(PolynomialContext&&) = default;
    PolynomialContext& operator=(PolynomialContext&&) = default;

    // 禁止拷贝
    PolynomialContext(const PolynomialContext&) = delete;
    PolynomialContext& operator=(const PolynomialContext&) = delete;

    /// 克隆
    [[nodiscard]] PolynomialContext clone() const {
        std::vector<Integer> coeffs_copy;
        coeffs_copy.reserve(f_coeffs_.size());
        for (const auto& c : f_coeffs_) {
            coeffs_copy.push_back(c.clone());
        }
        return PolynomialContext(n_.clone(), std::move(coeffs_copy), m_.clone(), skewness_);
    }

    // ==================== 访问器 ====================

    /// 多项式度数
    [[nodiscard]] uint32_t degree() const noexcept { return degree_; }

    /// 待分解的数 n
    [[nodiscard]] const Integer& n() const noexcept { return n_; }

    /// m 值，f(m) = 0 (mod n)
    [[nodiscard]] const Integer& m() const noexcept { return m_; }

    /// skewness 参数
    [[nodiscard]] double skewness() const noexcept { return skewness_; }

    /// 获取系数 f_i（x^i 的系数）
    [[nodiscard]] const Integer& coeff(uint32_t i) const {
        static const Integer zero(static_cast<int64_t>(0));
        if (i >= f_coeffs_.size()) return zero;
        return f_coeffs_[i];
    }

    /// 获取 leading coefficient
    [[nodiscard]] const Integer& leading_coeff() const {
        return f_coeffs_[degree_];
    }

    /// 获取所有系数
    [[nodiscard]] const std::vector<Integer>& coefficients() const noexcept {
        return f_coeffs_;
    }

    // ==================== 多项式运算 ====================

    /// 计算 f(x) 的值 (Horner 方法)
    [[nodiscard]] Integer evaluate(const Integer& x) const {
        if (f_coeffs_.empty()) return Integer(static_cast<int64_t>(0));

        // Horner 方法 (v22: result 直接 assign)
        Integer result;
        result = f_coeffs_[degree_];
        for (int i = static_cast<int>(degree_) - 1; i >= 0; --i) {
            result *= x;
            result += f_coeffs_[i];
        }
        return result;
    }

    /// 计算 f(x) mod p 的值 (always returns non-negative result in [0, p))
    [[nodiscard]] uint64_t evaluate_mod(uint64_t x, uint64_t p) const {
        if (f_coeffs_.empty()) return 0;

        // p_int hoist 出 lambda — degree+1 次调用避免重复 mpz_init+set+clear
        Integer p_int(static_cast<unsigned long long>(p));
        Integer tmp;
        auto get_coeff_mod_p = [&](const Integer& coeff) -> uint64_t {
            if (coeff.is_zero()) return 0;
            Integer::mod(tmp, coeff, p_int);
            if (tmp.is_negative()) {
                tmp += p_int;
            }
            return tmp.to_uint64();
        };

        uint64_t result = get_coeff_mod_p(f_coeffs_[degree_]);

        for (int i = static_cast<int>(degree_) - 1; i >= 0; --i) {
            result = static_cast<uint64_t>(
                (static_cast<__uint128_t>(result) * x) % p);
            uint64_t ci = get_coeff_mod_p(f_coeffs_[i]);
            result = (result + ci) % p;
        }
        return result;
    }

    /// 计算 f(x) 的 double 值（用于估算）
    [[nodiscard]] double evaluate_double(double x) const {
        if (f_coeffs_.empty()) return 0.0;

        double result = f_coeffs_[degree_].to_double();
        for (int i = static_cast<int>(degree_) - 1; i >= 0; --i) {
            result = result * x + f_coeffs_[i].to_double();
        }
        return result;
    }

    /// 计算代数侧的范数 (GNFS convention)
    /// N(a - b*α) = b^d * f(a/b)
    /// 展开形式: N = sum_{i=0}^{d} f_i * a^i * b^{d-i}
    [[nodiscard]] Integer algebraic_norm(int64_t a, uint64_t b) const {
        Integer result(static_cast<int64_t>(0));
        Integer a_power(static_cast<int64_t>(1));  // a^i

        // 计算 b^d, b^{d-1}, ..., b^0
        // Stack array avoids per-call heap allocation (degree ≤ 7 in practice).
        // Heap fallback when degree ≥ MAX_STACK_DEG — assert 在 Release 下 NDEBUG
        // 失效会导致 b_powers[i>7] 越界写。
        static constexpr uint32_t MAX_STACK_DEG = 8;
        std::array<Integer, MAX_STACK_DEG> b_powers_stack;
        std::vector<Integer> b_powers_heap;
        Integer* b_powers;
        if (degree_ < MAX_STACK_DEG) {
            b_powers = b_powers_stack.data();
        } else {
            b_powers_heap.resize(degree_ + 1);
            b_powers = b_powers_heap.data();
        }
        b_powers[0] = int64_t(1);  // mpz_set_si into default-init slot
        // v22: b_powers[i] = b_powers[i-1] (mpz_set into default-init / stack slot)
        for (uint32_t i = 1; i <= degree_; ++i) {
            b_powers[i] = b_powers[i-1];
            b_powers[i] *= static_cast<long long>(b);
        }

        // v22: 复用 term buffer (mpz_set 而非 mpz_init_set)
        Integer term;
        for (uint32_t i = 0; i <= degree_; ++i) {
            // term = f_i * a^i * b^{d-i}
            // Note: No sign alternation for N(a - bα) = b^d * f(a/b)
            term = f_coeffs_[i];
            term *= a_power;
            term *= b_powers[degree_ - i];

            result += term;

            // 更新 a_power
            if (i < degree_) {
                a_power *= a;
            }
        }

        return result;
    }

    /// 计算有理侧的值 (GNFS convention)
    /// R(a, b) = a - b*m
    [[nodiscard]] Integer rational_value(int64_t a, uint64_t b) const {
        // uint64 快路径: 当 m fits uint64 且结果 fits int64 时避免 GMP
        if (m_.fits_uint64()) {
            __int128 result = static_cast<__int128>(a) -
                              static_cast<__int128>(b) * static_cast<__int128>(m_.to_uint64());
            // int64 range: [-2^63, 2^63)
            if (result >= INT64_MIN && result <= INT64_MAX) {
                return Integer(static_cast<int64_t>(result));
            }
        }
        // v22: bm 直接 assign (mpz_set)
        Integer result(a);
        Integer bm;
        bm = m_;
        bm *= static_cast<long long>(b);
        result -= bm;
        return result;
    }

    /// 计算有理侧值的绝对值 (int64 快路径)
    /// 如果值 fits int64，返回 {abs_value, true}；否则返回 {0, false}
    [[nodiscard]] std::pair<uint64_t, bool> rational_value_abs_u64(int64_t a, uint64_t b) const {
        if (m_.fits_uint64()) {
            __int128 val = static_cast<__int128>(a) -
                           static_cast<__int128>(b) * static_cast<__int128>(m_.to_uint64());
            if (val < 0) val = -val;
            if (val <= static_cast<__int128>(UINT64_MAX)) {
                return {static_cast<uint64_t>(val), true};
            }
        }
        return {0, false};
    }

    /// 计算代数范数 (__int128 快路径)
    /// 当所有系数 fits int64 且中间乘积 fits __int128 时使用纯原生算术
    [[nodiscard]] std::pair<__int128, bool> algebraic_norm_i128(int64_t a, uint64_t b) const {
        if (b == 0) return {0, false};  // b^(d-i) division would be UB

        // 检查所有系数是否 fits int64
        for (uint32_t i = 0; i <= degree_; ++i) {
            if (!f_coeffs_[i].fits_int64()) return {0, false};
        }

        // Overflow guard: intermediate term = c_i * a^i * b^(d-i).
        // Worst case: max|c_i| * max(|a|,b)^d.
        // Need (d+1) * max|c_i| * max_val^d < 2^127 to be safe.
        // Use log2 to avoid overflow in the check itself.
        uint64_t abs_a = (a >= 0) ? static_cast<uint64_t>(a) : static_cast<uint64_t>(-(a + 1)) + 1;
        uint64_t max_val = std::max(abs_a, b);
        if (max_val > 1) {
            // Find max |coefficient|
            double max_coeff_log2 = 0;
            for (uint32_t i = 0; i <= degree_; ++i) {
                double c = std::abs(f_coeffs_[i].to_double());
                if (c > 0) max_coeff_log2 = std::max(max_coeff_log2, std::log2(c));
            }
            double max_term_log2 = max_coeff_log2 +
                static_cast<double>(degree_) * std::log2(static_cast<double>(max_val)) +
                std::log2(static_cast<double>(degree_ + 1));
            if (max_term_log2 > 126.0) {
                return {0, false};  // Would overflow __int128
            }
        }

        __int128 result = 0;
        __int128 a_power = 1;  // a^i
        __int128 b_val = static_cast<__int128>(b);

        // 计算 b^d
        __int128 b_pow_d = 1;
        for (uint32_t i = 0; i < degree_; ++i) b_pow_d *= b_val;

        __int128 b_pow = b_pow_d;  // starts at b^d, decreases
        for (uint32_t i = 0; i <= degree_; ++i) {
            __int128 ci = static_cast<__int128>(f_coeffs_[i].to_int64());
            __int128 term = ci * a_power * b_pow;
            result += term;
            a_power *= static_cast<__int128>(a);
            if (i < degree_ && b_val != 0) {
                b_pow /= b_val;  // b^(d-i) → b^(d-i-1)
            }
        }
        return {result, true};
    }

    // ==================== 验证 ====================

    /// 验证 f(m) = 0 (mod n)
    [[nodiscard]] bool verify() const {
        Integer fm = evaluate(m_);
        Integer r;
        Integer::mod(r, fm, n_);
        return r.is_zero();
    }

    /// n 的十进制位数
    [[nodiscard]] size_t n_digits() const {
        return n_.num_digits(10);
    }

private:
    Integer n_;                      // 待分解的数
    std::vector<Integer> f_coeffs_;  // f(x) 的系数
    Integer m_;                      // f(m) = 0 (mod n)
    uint32_t degree_ = 0;
    double skewness_ = 1.0;
};

} // namespace gnfs::core
