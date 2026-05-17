#pragma once

#include "../core/integer.hpp"
#include "../sqrt/modular_poly.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

namespace gnfs::polynomial {

using core::Integer;

/// IntPolynomial - 整系数多项式
/// coeffs_[i] 是 x^i 的系数
class IntPolynomial {
public:
    /// 默认构造（零多项式）
    IntPolynomial() {
        coeffs_.emplace_back(static_cast<int64_t>(0));
    }

    /// 从系数列表构造
    explicit IntPolynomial(std::vector<Integer> coeffs)
        : coeffs_(std::move(coeffs)) {
        if (coeffs_.empty()) {
            coeffs_.emplace_back(static_cast<int64_t>(0));
        }
        normalize();
    }

    /// 从单个常数构造
    explicit IntPolynomial(int c) {
        coeffs_.emplace_back(c);
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
        static const Integer zero(static_cast<int64_t>(0));
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
        roots.reserve(static_cast<size_t>(degree()));  // bounded by f degree

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
        // std::vector<Integer>(N) default-inits Integers to 0 (via Integer default ctor).
        // No need to re-init in loop.
        std::vector<Integer> result_coeffs(d1 + d2 + 1);

        // 卷积
        // v22: term buffer 复用 (mpz_set 替代 mpz_init_set), 节省 d² - 1 allocs/multiply
        Integer term;
        for (uint32_t i = 0; i <= d1; ++i) {
            for (uint32_t j = 0; j <= d2; ++j) {
                term = coeffs_[i];
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
        // std::vector<Integer>(d+1) default-inits Integer to 0 — no explicit zero loop needed.
        std::vector<Integer> new_coeffs(d + 1);

        // 预计算 t 的幂次
        std::vector<Integer> t_powers(d + 1);
        t_powers[0] = Integer(static_cast<int64_t>(1));
        for (uint32_t i = 1; i <= d; ++i) {
            t_powers[i] = t_powers[i-1].clone();
            t_powers[i] *= t;
        }

        // 二项式系数表(静态初始化,线程安全)。GNFS degree 上限 6,
        // 预表覆盖到 16 仍极小(<2KB),C(16,8)=12870 < 2^14。
        // 之前每次 translate 调用都重建,Stage2 平移循环 ×11 重复浪费。
        constexpr uint32_t BINOM_MAX = 16;
        static const auto binom = []() {
            std::array<std::array<uint64_t, BINOM_MAX + 1>, BINOM_MAX + 1> b{};
            for (uint32_t i = 0; i <= BINOM_MAX; ++i) {
                b[i][0] = 1;
                b[i][i] = 1;
                for (uint32_t j = 1; j < i; ++j) {
                    b[i][j] = b[i-1][j-1] + b[i-1][j];
                }
            }
            return b;
        }();
        assert(d <= BINOM_MAX && "IntPolynomial::translate degree exceeds binom table");

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
        throw std::logic_error("discriminant() not implemented for degree > 2");
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

    /// Cantor-Zassenhaus 算法求根 — O(d² log p)
    /// 1. 计算 h = gcd(f, x^p - x) mod p 得到所有线性因子的乘积
    /// 2. 从 h 中提取根（递归分裂）
    [[nodiscard]] std::vector<uint32_t> roots_cantor_zassenhaus(uint32_t p) const {
        using MP = sqrt::ModularPoly;

        // 获取 f(x) mod p
        uint32_t d = degree();
        std::vector<uint64_t> f_mod(d + 1);
        for (uint32_t i = 0; i <= d; ++i) {
            f_mod[i] = coeff_mod(i, p);
        }
        // f_mod 可能 leading coeff == 0 (mod p)，跳过
        MP f_poly(f_mod);
        if (f_poly.degree() <= 0) return {};

        // Step 1: h = gcd(f, x^p - x) mod p
        MP x_poly;
        x_poly.set_coeff(1, 1);

        // x^p mod f mod p (repeated squaring, O(d² log p))
        auto x_to_p = MP::power(x_poly, Integer(static_cast<int64_t>(p)), f_mod, p);
        auto x_p_minus_x = MP::sub(x_to_p, x_poly, p);
        auto h = MP::gcd(x_p_minus_x, f_poly, p);

        int h_deg = h.degree();
        if (h_deg <= 0) return {};

        // Step 2: 从 h 中提取根
        return cz_extract_roots(h, p);
    }

    /// 从度数为 deg 的 split-free 多项式中提取所有根
    [[nodiscard]] static std::vector<uint32_t> cz_extract_roots(
            const sqrt::ModularPoly& poly, uint32_t p) {
        using MP = sqrt::ModularPoly;

        int deg = poly.degree();
        if (deg <= 0) return {};

        if (deg == 1) {
            // ax + b = 0 → x = -b · a^{-1} mod p
            uint64_t a = poly.coeff(1), b = poly.coeff(0);
            uint64_t a_inv = pow_mod(a, p - 2, p);
            uint64_t root = static_cast<uint64_t>(
                (static_cast<__uint128_t>(p - b) * a_inv) % p);
            return {static_cast<uint32_t>(root)};
        }

        // Cantor-Zassenhaus splitting: pick random a, compute gcd(poly, (x+a)^{(p-1)/2} - 1).
        // Deterministic seed (reproducibility) but using SplitMix-style mixing of p and the
        // polynomial leading coefficient so two different polynomials over the same p don't
        // share the same random sequence. p*31+17 fed mt19937_64 well enough, but two distinct
        // CZ calls with the same p (different polys) would attempt identical a values 1-by-1.
        uint64_t seed = static_cast<uint64_t>(p);
        seed ^= poly.coeff(static_cast<int>(deg)) + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2);
        seed ^= static_cast<uint64_t>(deg) + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2);
        std::mt19937_64 rng(seed);
        std::vector<uint64_t> poly_coeffs;
        poly_coeffs.reserve(static_cast<size_t>(deg + 1));
        for (int i = 0; i <= deg; ++i) poly_coeffs.push_back(poly.coeff(i));

        for (int attempt = 0; attempt < 100; ++attempt) {
            uint64_t a = rng() % p;
            MP x_plus_a;
            x_plus_a.set_coeff(0, a);
            x_plus_a.set_coeff(1, 1);

            // (x+a)^{(p-1)/2} mod poly mod p
            Integer exp_val(static_cast<int64_t>((p - 1) / 2));
            auto power_result = MP::power(x_plus_a, exp_val, poly_coeffs, p);

            // subtract 1
            uint64_t c0 = power_result.coeff(0);
            c0 = (c0 + p - 1) % p;
            power_result.set_coeff(0, c0);

            auto factor = MP::gcd(power_result, poly, p);
            int f_deg = factor.degree();

            if (f_deg > 0 && f_deg < deg) {
                // 成功分裂，递归两半
                auto roots1 = cz_extract_roots(factor, p);
                // poly / factor
                auto [quotient, rem] = MP::divmod(poly, factor, p);
                auto roots2 = cz_extract_roots(quotient, p);
                roots1.reserve(roots1.size() + roots2.size());
                roots1.insert(roots1.end(), roots2.begin(), roots2.end());
                return roots1;
            }
        }

        // 极少数情况回退暴力
        std::vector<uint32_t> roots;
        roots.reserve(static_cast<size_t>(deg));  // bounded by f degree
        for (uint32_t r = 0; r < p && static_cast<int>(roots.size()) < deg; ++r) {
            uint64_t val = 0, rp = 1;
            for (int i = 0; i <= deg; ++i) {
                val = (val + static_cast<uint64_t>(
                    (static_cast<__uint128_t>(poly.coeff(i)) * rp) % p)) % p;
                rp = static_cast<uint64_t>((static_cast<__uint128_t>(rp) * r) % p);
            }
            if (val == 0) roots.push_back(r);
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

} // namespace gnfs::polynomial
