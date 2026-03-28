#pragma once

#include "integer.hpp"

#include <cmath>
#include <stdexcept>
#include <vector>

namespace gnfs {
namespace core {

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

        // Horner 方法
        Integer result = f_coeffs_[degree_].clone();
        for (int i = static_cast<int>(degree_) - 1; i >= 0; --i) {
            result *= x;
            result += f_coeffs_[i];
        }
        return result;
    }

    /// 计算 f(x) mod p 的值 (always returns non-negative result in [0, p))
    [[nodiscard]] uint64_t evaluate_mod(uint64_t x, uint64_t p) const {
        if (f_coeffs_.empty()) return 0;

        // Helper to get coefficient mod p (always non-negative)
        auto get_coeff_mod_p = [p](const Integer& coeff) -> uint64_t {
            if (coeff.is_zero()) return 0;

            Integer tmp;
            Integer p_int(static_cast<unsigned long long>(p));
            Integer::mod(tmp, coeff, p_int);

            // Handle negative remainders: convert to [0, p)
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
        std::vector<Integer> b_powers(degree_ + 1);
        b_powers[0] = Integer(int64_t(1));
        for (uint32_t i = 1; i <= degree_; ++i) {
            b_powers[i] = b_powers[i-1].clone();
            b_powers[i] *= static_cast<long long>(b);
        }

        for (uint32_t i = 0; i <= degree_; ++i) {
            // term = f_i * a^i * b^{d-i}
            // Note: No sign alternation for N(a - bα) = b^d * f(a/b)
            Integer term = f_coeffs_[i].clone();
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
        Integer result(a);
        Integer bm = m_.clone();
        bm *= static_cast<long long>(b);
        result -= bm;
        return result;
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

} // namespace core
} // namespace gnfs
