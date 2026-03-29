#pragma once

#include "../core/integer.hpp"
#include "int_polynomial.hpp"

#include <cmath>
#include <optional>
#include <vector>

namespace gnfs {
namespace polynomial {

using core::Integer;

/// PolynomialOptimizer - 多项式优化工具
/// 包含牛顿法求根、多项式变换等优化方法
class PolynomialOptimizer {
public:
    /// 牛顿法求根优化
    /// 给定初始估计，迭代优化找到更精确的根
    /// @param f 多项式
    /// @param df f 的导数
    /// @param initial 初始估计值
    /// @param n 模数（优化 f(m) ≡ 0 mod n）
    /// @param max_iterations 最大迭代次数
    /// @param tolerance 收敛容差（相对误差）
    /// @return 优化后的根，如果失败则返回 nullopt
    [[nodiscard]] static std::optional<Integer> newton_root(
            const IntPolynomial& f,
            const IntPolynomial& df,
            const Integer& initial,
            const Integer& n,
            uint32_t max_iterations = 256,
            double tolerance = 1e-6) {

        if (f.is_zero()) {
            return std::nullopt;
        }

        // 如果起始点已满足 f(m) ≡ 0 mod n，直接返回
        // （GNFS base-m 展开保证 f(m) = N，此检查短路常见情况）
        {
            Integer fm_init = f.evaluate(initial);
            Integer q_init, r_init;
            Integer::divmod(q_init, r_init, fm_init, n);
            if (r_init.is_zero()) {
                return initial.clone();
            }
        }

        Integer m = initial.clone();
        Integer prev_m = m.clone();

        for (uint32_t iter = 0; iter < max_iterations; ++iter) {
            // 计算 f(m) 和 f'(m)
            Integer fm = f.evaluate(m);
            Integer dfm = df.evaluate(m);

            // 如果导数为零，无法继续
            if (dfm.is_zero()) {
                // 尝试微小扰动
                m += 1;
                continue;
            }

            // 牛顿更新: m_new = m - f(m)/f'(m)
            // divmod(q, r, a, b): q = a/b, r = a%b
            Integer delta, rem;
            Integer::divmod(delta, rem, fm, dfm);

            prev_m = m.clone();
            m -= delta;

            // 检查收敛
            if (delta.is_zero()) {
                break;
            }

            // 检查相对变化
            Integer diff = prev_m.clone();
            diff -= m;
            diff.abs();

            double rel_change = diff.to_double() / (std::abs(m.to_double()) + 1e-10);
            if (rel_change < tolerance) {
                break;
            }
        }

        // 验证结果: f(m) ≡ 0 mod n?
        Integer fm_final = f.evaluate(m);
        Integer quotient, actual_rem;
        Integer::divmod(quotient, actual_rem, fm_final, n);

        if (actual_rem.is_zero()) {
            return m;
        }

        // 验证失败：f(m) 不是 n 的倍数
        return std::nullopt;
    }

    /// 简化版牛顿法（自动计算导数）
    [[nodiscard]] static std::optional<Integer> newton_root(
            const IntPolynomial& f,
            const Integer& initial,
            const Integer& n,
            uint32_t max_iterations = 256,
            double tolerance = 1e-6) {

        IntPolynomial df = derivative(f);
        return newton_root(f, df, initial, n, max_iterations, tolerance);
    }

    /// 计算多项式的导数
    /// f(x) = a_n*x^n + ... + a_1*x + a_0
    /// f'(x) = n*a_n*x^{n-1} + ... + a_1
    [[nodiscard]] static IntPolynomial derivative(const IntPolynomial& f) {
        uint32_t d = f.degree();
        if (d == 0) {
            // 常数的导数是 0
            return IntPolynomial(0);
        }

        std::vector<Integer> coeffs;
        coeffs.reserve(d);

        for (uint32_t i = 1; i <= d; ++i) {
            Integer c = f[i].clone();
            c *= static_cast<int64_t>(i);
            coeffs.push_back(std::move(c));
        }

        return IntPolynomial(std::move(coeffs));
    }

    /// 多项式平移: 计算 g(x) = f(x + t)
    /// 使用二项式展开
    [[nodiscard]] static IntPolynomial translate(const IntPolynomial& f, int64_t t) {
        if (t == 0) {
            return f.clone();
        }

        uint32_t d = f.degree();
        std::vector<Integer> new_coeffs(d + 1);

        // 初始化为零
        for (auto& c : new_coeffs) {
            c = Integer(static_cast<int64_t>(0));
        }

        // 二项式展开: f(x+t) = sum_i f[i] * (x+t)^i
        // (x+t)^i = sum_j C(i,j) * x^j * t^{i-j}

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

        // 计算新系数
        for (uint32_t i = 0; i <= d; ++i) {
            // f[i] * (x+t)^i 对 x^j 的贡献
            for (uint32_t j = 0; j <= i; ++j) {
                // 贡献: f[i] * C(i,j) * t^{i-j} 到 x^j
                Integer term = f[i].clone();
                term *= static_cast<int64_t>(binom[i][j]);
                term *= t_powers[i - j];
                new_coeffs[j] += term;
            }
        }

        return IntPolynomial(std::move(new_coeffs));
    }

    /// 多项式旋转: 计算 g(x) = f(x) + k * h(x)
    /// 常用于 h(x) = x - m，保持 f(m) = g(m)
    [[nodiscard]] static IntPolynomial rotate(
            const IntPolynomial& f,
            const IntPolynomial& h,
            int64_t k) {
        if (k == 0) {
            return f.clone();
        }

        IntPolynomial g = f.clone();

        // g = f + k * h
        uint32_t max_deg = std::max(f.degree(), h.degree());
        for (uint32_t i = 0; i <= max_deg; ++i) {
            Integer term = h[i].clone();
            term *= k;
            g[i] += term;
        }

        g.normalize();
        return g;
    }

    /// 简化的旋转: f(x) + k * (x - m)
    [[nodiscard]] static IntPolynomial rotate_linear(
            const IntPolynomial& f,
            const Integer& m,
            int64_t k) {
        if (k == 0) {
            return f.clone();
        }

        IntPolynomial g = f.clone();

        // g[0] -= k * m
        // g[1] += k
        Integer km = m.clone();
        km *= k;
        g[0] -= km;
        g[1] += k;

        g.normalize();
        return g;
    }

    /// 黄金分割法优化 skewness
    /// @param f 多项式
    /// @param min_skew 最小 skewness
    /// @param max_skew 最大 skewness
    /// @param scorer 评分函数 (skewness) -> score（越小越好）
    /// @param tolerance 收敛容差
    /// @return 最优 skewness
    template<typename Scorer>
    [[nodiscard]] static double golden_section_skewness(
            double min_skew,
            double max_skew,
            Scorer scorer,
            double tolerance = 1e-4) {

        const double phi = (1.0 + std::sqrt(5.0)) / 2.0;
        const double resphi = 2.0 - phi;

        double a = min_skew;
        double b = max_skew;
        double x1 = a + resphi * (b - a);
        double x2 = b - resphi * (b - a);
        double f1 = scorer(x1);
        double f2 = scorer(x2);

        while (std::abs(b - a) > tolerance * (std::abs(x1) + std::abs(x2) + 1e-10)) {
            if (f1 < f2) {
                // 最小值在 [a, x2]
                b = x2;
                x2 = x1;
                f2 = f1;
                x1 = a + resphi * (b - a);
                f1 = scorer(x1);
            } else {
                // 最小值在 [x1, b]
                a = x1;
                x1 = x2;
                f1 = f2;
                x2 = b - resphi * (b - a);
                f2 = scorer(x2);
            }
        }

        return (a + b) / 2.0;
    }

    /// 估计最优 skewness
    /// 基于系数大小: skewness ~ (c_0 / c_d)^{1/d}
    [[nodiscard]] static double estimate_skewness(const IntPolynomial& f) {
        uint32_t d = f.degree();
        if (d == 0) return 1.0;

        double c0 = std::abs(f[0].to_double());
        double cd = std::abs(f[d].to_double());

        if (cd < 1e-10 || c0 < 1e-10) {
            return 1.0;
        }

        return std::pow(c0 / cd, 1.0 / d);
    }

    /// 计算多项式在给定 skewness 下的"大小"
    /// size = sum_i |c_i| * s^{i - d/2}
    [[nodiscard]] static double compute_size(const IntPolynomial& f, double skewness) {
        uint32_t d = f.degree();
        double half_d = d / 2.0;
        double size = 0.0;

        for (uint32_t i = 0; i <= d; ++i) {
            double ci = std::abs(f[i].to_double());
            size += ci * std::pow(skewness, i - half_d);
        }

        return size;
    }

    /// 生成光滑数（只有小素因子的数）
    /// @param bound 上界
    /// @param small_primes 小素数列表
    /// @param max_count 最大生成数量
    [[nodiscard]] static std::vector<Integer> generate_smooth_numbers(
            uint64_t bound,
            const std::vector<uint32_t>& small_primes,
            size_t max_count = 10000) {

        std::vector<Integer> result;
        result.push_back(Integer(static_cast<int64_t>(1)));

        for (uint32_t p : small_primes) {
            size_t current_size = result.size();
            for (size_t i = 0; i < current_size && result.size() < max_count; ++i) {
                Integer val = result[i].clone();
                val *= p;
                while (val.fits_uint64() && val.to_uint64() <= bound) {
                    result.push_back(val.clone());
                    val *= p;
                }
            }
        }

        // 排序并去重
        std::sort(result.begin(), result.end(),
            [](const Integer& a, const Integer& b) {
                return a < b;
            });
        result.erase(
            std::unique(result.begin(), result.end(),
                [](const Integer& a, const Integer& b) { return a == b; }),
            result.end());

        // 限制数量
        if (result.size() > max_count) {
            result.resize(max_count);
        }

        return result;
    }
};

} // namespace polynomial
} // namespace gnfs
