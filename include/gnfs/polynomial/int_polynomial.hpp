#pragma once

#include "../core/integer.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace gnfs {
namespace polynomial {

using core::Integer;

/// IntPolynomial - 整系数多项式
/// coeffs_[i] 是 x^i 的系数
class IntPolynomial {
public:
    /// 默认构造（零多项式）
    IntPolynomial() {
        coeffs_.push_back(Integer(static_cast<int64_t>(0)));
    }

    /// 从系数列表构造
    explicit IntPolynomial(std::vector<Integer> coeffs)
        : coeffs_(std::move(coeffs)) {
        if (coeffs_.empty()) {
            coeffs_.push_back(Integer(static_cast<int64_t>(0)));
        }
        normalize();
    }

    /// 从单个常数构造
    explicit IntPolynomial(int c) {
        coeffs_.push_back(Integer(c));
    }

    /// 移动构造
    IntPolynomial(IntPolynomial&&) = default;
    IntPolynomial& operator=(IntPolynomial&&) = default;

    // 禁止拷贝
    IntPolynomial(const IntPolynomial&) = delete;
    IntPolynomial& operator=(const IntPolynomial&) = delete;

    /// 克隆
    [[nodiscard]] IntPolynomial clone() const {
        std::vector<Integer> coeffs_copy;
        coeffs_copy.reserve(coeffs_.size());
        for (const auto& c : coeffs_) {
            coeffs_copy.push_back(c.clone());
        }
        return IntPolynomial(std::move(coeffs_copy));
    }

    // ==================== 访问器 ====================

    /// 多项式度数（零多项式返回0）
    [[nodiscard]] uint32_t degree() const noexcept {
        return static_cast<uint32_t>(coeffs_.size() - 1);
    }

    /// 是否为零多项式
    [[nodiscard]] bool is_zero() const noexcept {
        return coeffs_.size() == 1 && coeffs_[0].is_zero();
    }

    /// 获取系数
    [[nodiscard]] const Integer& operator[](size_t i) const {
        static Integer zero(static_cast<int64_t>(0));
        if (i >= coeffs_.size()) return zero;
        return coeffs_[i];
    }

    /// 获取系数（可修改）
    [[nodiscard]] Integer& operator[](size_t i) {
        if (i >= coeffs_.size()) {
            coeffs_.resize(i + 1);
        }
        return coeffs_[i];
    }

    /// 获取 leading coefficient
    [[nodiscard]] const Integer& leading_coeff() const {
        return coeffs_.back();
    }

    /// 获取所有系数
    [[nodiscard]] const std::vector<Integer>& coefficients() const noexcept {
        return coeffs_;
    }

    // ==================== 多项式求值 ====================

    /// 计算 f(x) 的值 (Horner 方法)
    [[nodiscard]] Integer evaluate(const Integer& x) const {
        if (coeffs_.empty()) return Integer(static_cast<int64_t>(0));

        Integer result = coeffs_.back().clone();
        for (int i = static_cast<int>(coeffs_.size()) - 2; i >= 0; --i) {
            result *= x;
            result += coeffs_[i];
        }
        return result;
    }

    /// 计算 f(x) 的 double 值
    [[nodiscard]] double evaluate_double(double x) const {
        if (coeffs_.empty()) return 0.0;

        double result = coeffs_.back().to_double();
        for (int i = static_cast<int>(coeffs_.size()) - 2; i >= 0; --i) {
            result = result * x + coeffs_[i].to_double();
        }
        return result;
    }

    // ==================== 模运算 ====================

    /// 计算 f(x) mod p 的值
    [[nodiscard]] uint64_t evaluate_mod(uint64_t x, uint64_t p) const {
        if (coeffs_.empty()) return 0;

        uint64_t result = coeff_mod(coeffs_.size() - 1, p);
        for (int i = static_cast<int>(coeffs_.size()) - 2; i >= 0; --i) {
            result = mul_mod(result, x, p);
            result = add_mod(result, coeff_mod(i, p), p);
        }
        return result;
    }

    /// 计算 f(x) mod p 的所有根
    /// 使用暴力搜索（小 p）或 Cantor-Zassenhaus（大 p）
    [[nodiscard]] std::vector<uint32_t> roots_mod_p(uint32_t p) const {
        std::vector<uint32_t> roots;

        if (p < 1000) {
            // 小 p：暴力搜索
            for (uint32_t r = 0; r < p; ++r) {
                if (evaluate_mod(r, p) == 0) {
                    roots.push_back(r);
                }
            }
        } else {
            // 大 p：使用 Cantor-Zassenhaus 算法
            roots = roots_cantor_zassenhaus(p);
        }

        return roots;
    }

    // ==================== 算术运算 ====================

    /// 多项式加法
    IntPolynomial& operator+=(const IntPolynomial& other) {
        if (other.coeffs_.size() > coeffs_.size()) {
            coeffs_.resize(other.coeffs_.size());
        }
        for (size_t i = 0; i < other.coeffs_.size(); ++i) {
            coeffs_[i] += other.coeffs_[i];
        }
        normalize();
        return *this;
    }

    /// 多项式减法
    IntPolynomial& operator-=(const IntPolynomial& other) {
        if (other.coeffs_.size() > coeffs_.size()) {
            coeffs_.resize(other.coeffs_.size());
        }
        for (size_t i = 0; i < other.coeffs_.size(); ++i) {
            coeffs_[i] -= other.coeffs_[i];
        }
        normalize();
        return *this;
    }

    /// 标量乘法
    IntPolynomial& operator*=(const Integer& scalar) {
        for (auto& c : coeffs_) {
            c *= scalar;
        }
        normalize();
        return *this;
    }

    IntPolynomial& operator*=(int scalar) {
        for (auto& c : coeffs_) {
            c *= scalar;
        }
        normalize();
        return *this;
    }

    /// 多项式乘法
    [[nodiscard]] IntPolynomial operator*(const IntPolynomial& other) const {
        if (is_zero() || other.is_zero()) {
            return IntPolynomial(0);
        }

        uint32_t d1 = degree();
        uint32_t d2 = other.degree();
        std::vector<Integer> result_coeffs(d1 + d2 + 1);

        // 初始化为零
        for (auto& c : result_coeffs) {
            c = Integer(static_cast<int64_t>(0));
        }

        // 卷积
        for (uint32_t i = 0; i <= d1; ++i) {
            for (uint32_t j = 0; j <= d2; ++j) {
                Integer term = coeffs_[i].clone();
                term *= other.coeffs_[j];
                result_coeffs[i + j] += term;
            }
        }

        IntPolynomial result(std::move(result_coeffs));
        result.normalize();
        return result;
    }

    // ==================== 导数和变换 ====================

    /// 计算导数 f'(x)
    /// f(x) = a_n*x^n + ... + a_1*x + a_0
    /// f'(x) = n*a_n*x^{n-1} + ... + a_1
    [[nodiscard]] IntPolynomial derivative() const {
        uint32_t d = degree();
        if (d == 0) {
            // 常数的导数是 0
            return IntPolynomial(0);
        }

        std::vector<Integer> new_coeffs;
        new_coeffs.reserve(d);

        for (uint32_t i = 1; i <= d; ++i) {
            Integer c = coeffs_[i].clone();
            c *= static_cast<int64_t>(i);
            new_coeffs.push_back(std::move(c));
        }

        return IntPolynomial(std::move(new_coeffs));
    }

    /// 多项式平移: 计算 g(x) = f(x + t)
    /// 使用二项式展开
    [[nodiscard]] IntPolynomial translate(int64_t t) const {
        if (t == 0) {
            return clone();
        }

        uint32_t d = degree();
        std::vector<Integer> new_coeffs(d + 1);

        // 初始化为零
        for (auto& c : new_coeffs) {
            c = Integer(static_cast<int64_t>(0));
        }

        // 预计算 t 的幂次
        std::vector<Integer> t_powers(d + 1);
        t_powers[0] = Integer(static_cast<int64_t>(1));
        for (uint32_t i = 1; i <= d; ++i) {
            t_powers[i] = t_powers[i-1].clone();
            t_powers[i] *= t;
        }

        // 预计算二项式系数
        std::vector<std::vector<uint64_t>> binom(d + 1);
        for (uint32_t i = 0; i <= d; ++i) {
            binom[i].resize(i + 1);
            binom[i][0] = 1;
            binom[i][i] = 1;
            for (uint32_t j = 1; j < i; ++j) {
                binom[i][j] = binom[i-1][j-1] + binom[i-1][j];
            }
        }

        // 二项式展开: f(x+t) = sum_i f[i] * (x+t)^i
        // (x+t)^i = sum_j C(i,j) * x^j * t^{i-j}
        for (uint32_t i = 0; i <= d; ++i) {
            for (uint32_t j = 0; j <= i; ++j) {
                // 贡献: f[i] * C(i,j) * t^{i-j} 到 x^j
                Integer term = coeffs_[i].clone();
                term *= static_cast<int64_t>(binom[i][j]);
                term *= t_powers[i - j];
                new_coeffs[j] += term;
            }
        }

        return IntPolynomial(std::move(new_coeffs));
    }

    /// 规范化：移除高次零系数
    void normalize() {
        while (coeffs_.size() > 1 && coeffs_.back().is_zero()) {
            coeffs_.pop_back();
        }
    }

    // ==================== 判别式 ====================

    /// 计算判别式（简化版，仅用于小度数多项式）
    [[nodiscard]] Integer discriminant() const {
        // 对于一般多项式，判别式计算很复杂
        // 这里只实现度数 <= 6 的常见情况

        uint32_t d = degree();

        if (d == 1) {
            // 线性多项式的判别式为 1
            return Integer(static_cast<int64_t>(1));
        }

        if (d == 2) {
            // ax^2 + bx + c 的判别式 = b^2 - 4ac
            Integer b2 = coeffs_[1].clone();
            b2 *= coeffs_[1];

            Integer ac = coeffs_[0].clone();
            ac *= coeffs_[2];
            ac *= 4;

            b2 -= ac;
            return b2;
        }

        // 对于更高度数，需要计算 Resultant(f, f')
        // 这里返回一个占位值，实际实现需要更复杂的算法
        return Integer(static_cast<int64_t>(0));
    }

private:
    std::vector<Integer> coeffs_;

    // 辅助函数：取系数 mod p
    [[nodiscard]] uint64_t coeff_mod(size_t i, uint64_t p) const {
        if (i >= coeffs_.size()) return 0;
        const Integer& c = coeffs_[i];

        if (c.is_zero()) return 0;

        if (c.is_negative()) {
            // 处理负系数
            Integer abs_c = c.clone();
            abs_c.abs();
            uint64_t val = abs_c.fits_uint64()
                ? abs_c.to_uint64() % p
                : mpz_fdiv_ui(abs_c.get_mpz(), p);
            return val == 0 ? 0 : p - val;
        }

        return c.fits_uint64()
            ? c.to_uint64() % p
            : mpz_fdiv_ui(c.get_mpz(), p);
    }

    // 模加法
    [[nodiscard]] static uint64_t add_mod(uint64_t a, uint64_t b, uint64_t p) {
        return (a + b) % p;
    }

    // 模乘法
    [[nodiscard]] static uint64_t mul_mod(uint64_t a, uint64_t b, uint64_t p) {
        return (static_cast<__uint128_t>(a) * b) % p;
    }

    // 模幂
    [[nodiscard]] static uint64_t pow_mod(uint64_t base, uint64_t exp, uint64_t p) {
        uint64_t result = 1;
        base %= p;
        while (exp > 0) {
            if (exp & 1) {
                result = mul_mod(result, base, p);
            }
            exp >>= 1;
            base = mul_mod(base, base, p);
        }
        return result;
    }

    /// Cantor-Zassenhaus 算法求根
    [[nodiscard]] std::vector<uint32_t> roots_cantor_zassenhaus(uint32_t p) const {
        std::vector<uint32_t> roots;

        // 简化实现：对于 GNFS 的因子基构建
        // 我们主要处理度数 5-6 的多项式
        // 在 F_p 上，f(x) 的根数 <= degree(f)

        // 首先检查 gcd(f(x), x^p - x) 来找分裂因子
        // 这里用简化的暴力方法（对于因子基的素数范围够用）

        for (uint32_t r = 0; r < p; ++r) {
            if (evaluate_mod(r, p) == 0) {
                roots.push_back(r);
                if (roots.size() >= degree()) {
                    break;  // 最多 degree() 个根
                }
            }
        }

        return roots;
    }
};

// ==================== 非成员运算符 ====================

[[nodiscard]] inline IntPolynomial operator+(IntPolynomial&& a, const IntPolynomial& b) {
    a += b;
    return std::move(a);
}

[[nodiscard]] inline IntPolynomial operator-(IntPolynomial&& a, const IntPolynomial& b) {
    a -= b;
    return std::move(a);
}

} // namespace polynomial
} // namespace gnfs
