#pragma once

#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace gnfs {
namespace sqrt {

using core::Integer;
using core::PolynomialContext;

/// 数域元素 - 表示 Q[α]/f(α) 中的元素
/// 其中 α 是 f(x) 的根
/// 元素表示为多项式 a_0 + a_1*α + ... + a_{d-1}*α^{d-1}
class NumberFieldElement {
public:
    NumberFieldElement() = default;

    /// 从系数构造（coeffs[i] 是 α^i 的系数）
    explicit NumberFieldElement(std::vector<Integer> coeffs)
        : coeffs_(std::move(coeffs)) {
        normalize();
    }

    /// 从单个整数构造（常数元素）
    explicit NumberFieldElement(const Integer& value) {
        coeffs_.push_back(value.clone());
    }

    explicit NumberFieldElement(int64_t value) {
        coeffs_.push_back(Integer(value));
    }

    /// 移动构造
    NumberFieldElement(NumberFieldElement&&) = default;
    NumberFieldElement& operator=(NumberFieldElement&&) = default;

    /// 禁止拷贝
    NumberFieldElement(const NumberFieldElement&) = delete;
    NumberFieldElement& operator=(const NumberFieldElement&) = delete;

    /// 克隆
    [[nodiscard]] NumberFieldElement clone() const {
        std::vector<Integer> new_coeffs;
        new_coeffs.reserve(coeffs_.size());
        for (const auto& c : coeffs_) {
            new_coeffs.push_back(c.clone());
        }
        return NumberFieldElement(std::move(new_coeffs));
    }

    /// 获取度数（最高非零项的次数）
    [[nodiscard]] size_t degree() const noexcept {
        if (coeffs_.empty()) return 0;
        return coeffs_.size() - 1;
    }

    /// 获取系数
    [[nodiscard]] const Integer& coeff(size_t i) const {
        static Integer zero(static_cast<int64_t>(0));
        if (i >= coeffs_.size()) return zero;
        return coeffs_[i];
    }

    /// 获取所有系数
    [[nodiscard]] const std::vector<Integer>& coefficients() const noexcept {
        return coeffs_;
    }

    /// 是否为零
    [[nodiscard]] bool is_zero() const noexcept {
        for (const auto& c : coeffs_) {
            if (!c.is_zero()) return false;
        }
        return true;
    }

    /// 是否为单位元（常数1）
    [[nodiscard]] bool is_one() const noexcept {
        if (coeffs_.empty()) return false;
        if (coeffs_.size() > 1) {
            for (size_t i = 1; i < coeffs_.size(); ++i) {
                if (!coeffs_[i].is_zero()) return false;
            }
        }
        return coeffs_[0].fits_uint64() && coeffs_[0].to_uint64() == 1;
    }

    /// 加法
    void add(const NumberFieldElement& other) {
        size_t max_deg = std::max(coeffs_.size(), other.coeffs_.size());
        while (coeffs_.size() < max_deg) {
            coeffs_.push_back(Integer(static_cast<int64_t>(0)));
        }

        for (size_t i = 0; i < other.coeffs_.size(); ++i) {
            coeffs_[i] += other.coeffs_[i];
        }

        normalize();
    }

    /// 减法
    void subtract(const NumberFieldElement& other) {
        size_t max_deg = std::max(coeffs_.size(), other.coeffs_.size());
        while (coeffs_.size() < max_deg) {
            coeffs_.push_back(Integer(static_cast<int64_t>(0)));
        }

        for (size_t i = 0; i < other.coeffs_.size(); ++i) {
            coeffs_[i] -= other.coeffs_[i];
        }

        normalize();
    }

    /// 标量乘法
    void multiply_scalar(const Integer& scalar) {
        for (auto& c : coeffs_) {
            c *= scalar;
        }
        normalize();
    }

    /// 取负
    void negate() {
        for (auto& c : coeffs_) {
            c.negate();
        }
    }

    /// 模 n 归约（所有系数取模）
    void mod(const Integer& n) {
        for (auto& c : coeffs_) {
            c %= n;
            // 确保非负
            if (c.is_negative()) {
                c += n;
            }
        }
        normalize();
    }

private:
    std::vector<Integer> coeffs_;  // coeffs_[i] 是 α^i 的系数

    /// 移除高次零系数
    void normalize() {
        while (!coeffs_.empty() && coeffs_.back().is_zero()) {
            coeffs_.pop_back();
        }
    }
};

/// 数域 - Q[α]/f(α)
/// 提供数域中的算术运算
class NumberField {
public:
    /// 从多项式上下文构造
    explicit NumberField(const PolynomialContext& ctx)
        : degree_(ctx.degree()) {

        // 复制多项式系数
        for (uint32_t i = 0; i <= ctx.degree(); ++i) {
            f_coeffs_.push_back(ctx.coeff(i).clone());
        }

        // 存储 N 和 m
        n_ = ctx.n().clone();
        m_ = ctx.m().clone();
    }

    /// 获取多项式度数
    [[nodiscard]] uint32_t degree() const noexcept {
        return degree_;
    }

    /// 获取 N
    [[nodiscard]] const Integer& n() const noexcept {
        return n_;
    }

    /// 获取 m
    [[nodiscard]] const Integer& m() const noexcept {
        return m_;
    }

    /// 获取多项式系数 f_i
    [[nodiscard]] const Integer& coeff(uint32_t i) const {
        static Integer zero(static_cast<int64_t>(0));
        if (i >= f_coeffs_.size()) return zero;
        return f_coeffs_[i];
    }

    /// 获取所有多项式系数
    [[nodiscard]] const std::vector<Integer>& polynomial_coeffs() const noexcept {
        return f_coeffs_;
    }

    /// 创建零元素
    [[nodiscard]] NumberFieldElement zero() const {
        return NumberFieldElement(Integer(static_cast<int64_t>(0)));
    }

    /// 创建单位元素
    [[nodiscard]] NumberFieldElement one() const {
        return NumberFieldElement(Integer(static_cast<int64_t>(1)));
    }

    /// 创建 α 元素
    [[nodiscard]] NumberFieldElement alpha() const {
        std::vector<Integer> coeffs;
        coeffs.push_back(Integer(static_cast<int64_t>(0)));
        coeffs.push_back(Integer(static_cast<int64_t>(1)));
        return NumberFieldElement(std::move(coeffs));
    }

    /// 创建 (a - b*α) 元素
    /// This matches the GNFS convention where N(a - bα) = (-b)^d * f(-a/b)
    [[nodiscard]] NumberFieldElement from_ab(int64_t a, uint64_t b) const {
        std::vector<Integer> coeffs;
        coeffs.push_back(Integer(a));
        if (b != 0) {
            // Note: coefficient of α is -b (negative)
            coeffs.push_back(Integer(-static_cast<long long>(b)));
        }
        return NumberFieldElement(std::move(coeffs));
    }

    /// 数域乘法（模 f(α)）
    [[nodiscard]] NumberFieldElement multiply(
            const NumberFieldElement& x,
            const NumberFieldElement& y) const {

        if (x.is_zero() || y.is_zero()) {
            return zero();
        }

        // 多项式乘法
        size_t result_deg = x.degree() + y.degree();
        std::vector<Integer> result;
        result.reserve(result_deg + 1);
        for (size_t i = 0; i <= result_deg; ++i) {
            result.push_back(Integer(static_cast<int64_t>(0)));
        }

        for (size_t i = 0; i <= x.degree(); ++i) {
            for (size_t j = 0; j <= y.degree(); ++j) {
                Integer term = x.coeff(i).clone();
                term *= y.coeff(j);
                result[i + j] += term;
            }
        }

        // 模 f(α) 归约
        reduce(result);

        return NumberFieldElement(std::move(result));
    }

    /// 数域乘法（模 f(α) 和模 n）
    /// 支持非 monic 多项式 f（通过 f_d 的模逆来约化）
    [[nodiscard]] NumberFieldElement multiply_mod_n(
            const NumberFieldElement& x,
            const NumberFieldElement& y) const {

        if (x.is_zero() || y.is_zero()) {
            return zero();
        }

        // 多项式乘法
        size_t result_deg = x.degree() + y.degree();
        std::vector<Integer> result;
        result.reserve(result_deg + 1);
        for (size_t i = 0; i <= result_deg; ++i) {
            result.push_back(Integer(static_cast<int64_t>(0)));
        }

        for (size_t i = 0; i <= x.degree(); ++i) {
            for (size_t j = 0; j <= y.degree(); ++j) {
                Integer term = x.coeff(i).clone();
                term *= y.coeff(j);
                result[i + j] += term;
            }
        }

        // 模 f(α) 和模 n_ 归约（支持非 monic f）
        reduce_mod(result, n_);

        return NumberFieldElement(std::move(result));
    }

    /// 数域幂运算
    [[nodiscard]] NumberFieldElement power(
            const NumberFieldElement& base,
            const Integer& exp) const {

        if (exp.is_zero()) {
            return one();
        }

        NumberFieldElement result = one();
        NumberFieldElement b = base.clone();
        Integer e = exp.clone();

        while (!e.is_zero()) {
            if (e.is_odd()) {
                result = multiply(result, b);
            }
            b = multiply(b, b);
            // e >>= 1
            mpz_tdiv_q_2exp(e.get_mpz(), e.get_mpz(), 1);
        }

        return result;
    }

    /// 数域幂运算（模 n）
    [[nodiscard]] NumberFieldElement power_mod_n(
            const NumberFieldElement& base,
            const Integer& exp) const {

        if (exp.is_zero()) {
            return one();
        }

        NumberFieldElement result = one();
        NumberFieldElement b = base.clone();
        b.mod(n_);
        Integer e = exp.clone();

        while (!e.is_zero()) {
            if (e.is_odd()) {
                result = multiply_mod_n(result, b);
            }
            b = multiply_mod_n(b, b);
            // e >>= 1
            mpz_tdiv_q_2exp(e.get_mpz(), e.get_mpz(), 1);
        }

        return result;
    }

    /// 计算元素在 x = m 处的值（映射到 Z）
    [[nodiscard]] Integer evaluate_at_m(const NumberFieldElement& elem) const {
        Integer result(static_cast<int64_t>(0));
        Integer m_power(1);

        for (size_t i = 0; i <= elem.degree(); ++i) {
            Integer term = elem.coeff(i).clone();
            term *= m_power;
            result += term;

            if (i < elem.degree()) {
                m_power *= m_;
            }
        }

        return result;
    }

    /// 计算元素在 x = m 处的值（模 n）
    [[nodiscard]] Integer evaluate_at_m_mod_n(const NumberFieldElement& elem) const {
        Integer result(static_cast<int64_t>(0));
        Integer m_power(1);

        for (size_t i = 0; i <= elem.degree(); ++i) {
            Integer term = elem.coeff(i).clone();
            term *= m_power;
            term %= n_;
            result += term;
            result %= n_;

            if (i < elem.degree()) {
                m_power *= m_;
                m_power %= n_;
            }
        }

        // 确保非负
        if (result.is_negative()) {
            result += n_;
        }

        return result;
    }

    /// 计算范数 N(a - b*α) (GNFS convention)
    /// N(a - b*α) = b^d * f(a/b)
    /// 展开形式: N = sum_{i=0}^{d} f_i * a^i * b^{d-i}
    [[nodiscard]] Integer norm_linear(int64_t a, uint64_t b) const {
        Integer result(static_cast<int64_t>(0));
        Integer a_power(1);

        // 计算 b^d, b^{d-1}, ..., b^0
        std::vector<Integer> b_powers(degree_ + 1);
        b_powers[0] = Integer(static_cast<int64_t>(1));
        for (uint32_t i = 1; i <= degree_; ++i) {
            b_powers[i] = b_powers[i-1].clone();
            b_powers[i] *= static_cast<long long>(b);
        }

        for (uint32_t i = 0; i <= degree_; ++i) {
            // term = f_i * a^i * b^{d-i}
            Integer term = f_coeffs_[i].clone();
            term *= a_power;
            term *= b_powers[degree_ - i];

            result += term;

            // 更新 a^i
            if (i < degree_) {
                a_power *= a;
            }
        }

        // 取绝对值
        if (result.is_negative()) {
            result.negate();
        }

        return result;
    }

private:
    uint32_t degree_;
    std::vector<Integer> f_coeffs_;  // f(x) 的系数
    Integer n_;
    Integer m_;

    /// 模 f(x) 归约（纯整数，要求 f 是 monic）
    /// α^d = -(f_0 + f_1*α + ... + f_{d-1}*α^{d-1})
    void reduce(std::vector<Integer>& coeffs) const {
        // 从最高次项开始归约
        while (coeffs.size() > degree_) {
            size_t high_deg = coeffs.size() - 1;
            Integer high_coeff = std::move(coeffs.back());
            coeffs.pop_back();

            if (high_coeff.is_zero()) {
                continue;
            }

            size_t shift = high_deg - degree_;

            for (uint32_t i = 0; i < degree_; ++i) {
                Integer term = high_coeff.clone();
                term *= f_coeffs_[i];

                while (coeffs.size() <= shift + i) {
                    coeffs.push_back(Integer(static_cast<int64_t>(0)));
                }

                coeffs[shift + i] -= term;
            }
        }

        while (!coeffs.empty() && coeffs.back().is_zero()) {
            coeffs.pop_back();
        }
    }

    /// 模 f(x) 和模 modulus 归约（支持非 monic f）
    /// α^d = -(f_0 + ... + f_{d-1}*α^{d-1}) / f_d
    /// 通过 f_d 的模逆实现除法
    void reduce_mod(std::vector<Integer>& coeffs, const Integer& modulus) const {
        // 计算 f_d 的模逆
        Integer f_d_inv(int64_t(1));
        {
            Integer f_d = f_coeffs_[degree_].clone();
            f_d %= modulus;
            if (f_d.is_negative()) f_d += modulus;
            if (!f_d.is_one()) {
                int ok = mpz_invert(f_d_inv.get_mpz(), f_d.get_mpz(),
                                    modulus.get_mpz());
                if (!ok) {
                    // f_d 不可逆 — gcd(f_d, modulus) > 1
                    // 在 GNFS 中 modulus = N，这意味着找到了非平凡因子！
                    // 抛出异常让调用者处理（或捕获因子）
                    throw std::runtime_error(
                        "reduce_mod: f_d not invertible mod N — "
                        "gcd(f_d, N) > 1, nontrivial factor found");
                }
            }
        }

        bool need_scale = !f_d_inv.is_one();

        while (coeffs.size() > degree_) {
            size_t high_deg = coeffs.size() - 1;
            Integer high_coeff = std::move(coeffs.back());
            coeffs.pop_back();

            if (high_coeff.is_zero()) {
                continue;
            }

            // 除以 f_d
            Integer scaled = high_coeff.clone();
            if (need_scale) {
                scaled *= f_d_inv;
                scaled %= modulus;
            }

            size_t shift = high_deg - degree_;

            for (uint32_t i = 0; i < degree_; ++i) {
                Integer term = scaled.clone();
                term *= f_coeffs_[i];
                term %= modulus;

                while (coeffs.size() <= shift + i) {
                    coeffs.push_back(Integer(static_cast<int64_t>(0)));
                }

                coeffs[shift + i] -= term;
                coeffs[shift + i] %= modulus;
                if (coeffs[shift + i].is_negative()) {
                    coeffs[shift + i] += modulus;
                }
            }
        }

        // 最终 mod 归约 + 移除零高次项
        for (auto& c : coeffs) {
            c %= modulus;
            if (c.is_negative()) c += modulus;
        }
        while (!coeffs.empty() && coeffs.back().is_zero()) {
            coeffs.pop_back();
        }
    }
};

} // namespace sqrt
} // namespace gnfs
