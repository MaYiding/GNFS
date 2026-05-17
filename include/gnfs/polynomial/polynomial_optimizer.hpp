#pragma once

#include "../core/integer.hpp"
#include "int_polynomial.hpp"

#include <cmath>
#include <optional>
#include <vector>

namespace gnfs::polynomial {

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

        // v22: diff buffer 复用 across Newton iterations
        Integer diff;
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

            prev_m = m;  // mpz_set (复用 prev_m buffer)
            m -= delta;

            // 检查收敛
            if (delta.is_zero()) {
                break;
            }

            // 检查相对变化 (diff 复用 buffer)
            diff = prev_m;
            diff -= m;
            diff.abs();

            // to_double() 对超过 ~2^1023 的 Integer 会返回 ±inf,rel_change=NaN<tol 永远 false,
            // 让循环跑满 max_iterations。改用 bit_length 比较:diff 的位长比 m 至少少 k 位时
            // 视为相对变化 ≤ 2^-k,默认 tolerance=1e-6 → 约 20 位。
            const size_t m_bits = m.bit_length();
            const size_t diff_bits = diff.bit_length();
            const int tol_bits = static_cast<int>(std::ceil(-std::log2(tolerance)));
            if (diff.is_zero() ||
                (m_bits > 0 && diff_bits + tol_bits <= m_bits)) {
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

    /// 计算多项式的导数 — 委托给 IntPolynomial::derivative()
    [[nodiscard]] static IntPolynomial derivative(const IntPolynomial& f) {
        return f.derivative();
    }

    /// 多项式平移: 计算 g(x) = f(x + t) — 委托给 IntPolynomial::translate()
    [[nodiscard]] static IntPolynomial translate(const IntPolynomial& f, int64_t t) {
        return f.translate(t);
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

        // g = f + k * h — mpz_mul_si writes h[i] * k directly into term (skip set)
        uint32_t max_deg = std::max(f.degree(), h.degree());
        Integer term;
        for (uint32_t i = 0; i <= max_deg; ++i) {
            mpz_mul_si(term.get_mpz(), h[i].get_mpz(), k);
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

        // cd 极小或溢出 inf 时 fallback s=1 是粗暴的 — Murphy 评估会严重偏离最优。
        // 当 cd 病态但其他系数可用时,尝试 (|c_{d-1}|/|c_d|) 作为近似;
        // c0=0 时改用相邻系数比。
        if (!std::isfinite(cd) || cd < 1e-10) {
            // c_d 病态 → fallback s=1
            return 1.0;
        }
        if (!std::isfinite(c0) || c0 < 1e-10) {
            // c_0 ≈ 0:用 |c_1|/|c_d| 作为替代估计(degree d≥1)
            if (d >= 1) {
                double c1 = std::abs(f[1].to_double());
                if (std::isfinite(c1) && c1 >= 1e-10) {
                    return std::pow(c1 / cd, 1.0 / (d > 1 ? (d - 1) : 1));
                }
            }
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
        result.reserve(max_count);  // exit cap
        result.emplace_back(1);

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

} // namespace gnfs::polynomial
