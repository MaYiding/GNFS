#pragma once

#include "../core/integer.hpp"
#include "int_polynomial.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace gnfs {
namespace polynomial {

using core::Integer;

/// Murphy E-score 的各个组成部分
struct MurphyScore {
    double e_score = 0.0;       // 综合 Murphy E 值（线性尺度，可能为0）
    double log_e_score = -1e100; // log(E-score)，用于大数比较（越大越好）
    double alpha_f = 0.0;       // f 的 alpha 值（越负越好）
    double alpha_g = 0.0;       // g 的 alpha 值（越负越好）
    double size_score = 0.0;    // 大小贡献
    double root_score = 0.0;    // 根属性贡献
    double skewness = 1.0;      // 使用的 skewness

    /// 比较运算符（用于排序多项式）
    /// 使用 log_e_score 比较以处理大数
    bool operator<(const MurphyScore& other) const noexcept {
        return log_e_score < other.log_e_score;
    }

    bool operator>(const MurphyScore& other) const noexcept {
        return log_e_score > other.log_e_score;
    }
};

/// Murphy 评估参数
struct MurphyParams {
    uint32_t sample_points = 2000;          // 采样点数
    double alpha_bound = 1e7;               // alpha 计算的素数上界
    uint64_t smoothness_bound = 1000000;    // 光滑性界
    double skewness_min = 1e2;              // skewness 搜索下界
    double skewness_max = 1e10;             // skewness 搜索上界
    uint32_t skewness_steps = 100;          // skewness 网格搜索步数
    uint32_t seed = 42;                     // 随机种子
};

/// MurphyEvaluator - Murphy E-score 评估器
/// 用于评估 GNFS 多项式对 (f, g) 的质量
/// E(f, g) 估计期望的光滑关系产量
class MurphyEvaluator {
public:
    /// 构造评估器
    explicit MurphyEvaluator(const MurphyParams& params = MurphyParams{})
        : params_(params)
        , rng_(params.seed) {
        init_primes();
        init_dickman_table();
    }

    /// Move-only 语义
    MurphyEvaluator(MurphyEvaluator&&) = default;
    MurphyEvaluator& operator=(MurphyEvaluator&&) = default;
    MurphyEvaluator(const MurphyEvaluator&) = delete;
    MurphyEvaluator& operator=(const MurphyEvaluator&) = delete;

    ~MurphyEvaluator() = default;

    /// 计算 Murphy E-score（自动优化 skewness）
    /// @param f 代数侧多项式
    /// @param g 有理侧多项式 (通常是 g(x) = x - m)
    /// @param n 待分解的数
    /// @return Murphy 评分结构
    [[nodiscard]] MurphyScore compute(
            const IntPolynomial& f,
            const IntPolynomial& g,
            const Integer& n) {

        double best_skewness = optimize_skewness(f, g, n);
        return compute(f, g, n, best_skewness);
    }

    /// 计算 Murphy E-score（指定 skewness）
    [[nodiscard]] MurphyScore compute(
            const IntPolynomial& f,
            const IntPolynomial& g,
            const Integer& n,
            double skewness) {

        MurphyScore score;
        score.skewness = skewness;

        // 计算 alpha 值
        score.alpha_f = compute_alpha(f);
        score.alpha_g = compute_alpha(g);

        // 计算大小得分
        score.size_score = compute_size_score(f, g, skewness);

        // 计算根得分
        score.root_score = compute_root_score(f);

        // 基于采样计算 E-score（使用对数尺度）
        auto [log_e, linear_e] = sample_e_score_log(f, g, n, skewness);

        // 综合 alpha 贡献到 e_score（在对数空间）
        // alpha 越负，表示小素数整除的概率越高，多项式越好
        double alpha_contribution = -(score.alpha_f + score.alpha_g) / 10.0;

        score.log_e_score = log_e + alpha_contribution;
        score.e_score = linear_e * std::exp(alpha_contribution);

        return score;
    }

    /// 计算多项式的 alpha 值
    /// alpha 衡量多项式值被小素数整除的期望贡献
    /// 负的 alpha 表示更容易被小素数整除（更好）
    [[nodiscard]] double compute_alpha(const IntPolynomial& f) {
        return compute_alpha(f, params_.alpha_bound);
    }

    /// 计算 alpha 值（指定素数上界）
    [[nodiscard]] double compute_alpha(const IntPolynomial& f, double prime_bound) {
        double alpha = 0.0;

        for (uint32_t p : small_primes_) {
            if (p > prime_bound) break;

            // 计算 f mod p 的根数
            auto roots = f.roots_mod_p(p);
            uint32_t r = static_cast<uint32_t>(roots.size());

            // 贡献公式:
            // contribution = (r/p - 1/(p-1)) * log(p)
            // r/p 是被 p 整除的概率
            // 1/(p-1) 是随机数被 p 整除的期望
            double contribution = (static_cast<double>(r) / p
                                 - 1.0 / (p - 1)) * std::log(static_cast<double>(p));

            // 投影根的额外贡献
            // 如果 p | leading_coeff(f)，则有投影根
            if (f.leading_coeff().fits_uint64()) {
                if (f.leading_coeff().to_uint64() % p == 0) {
                    contribution += std::log(static_cast<double>(p)) / p;
                }
            }

            alpha += contribution;
        }

        return alpha;
    }

    /// 优化 skewness
    /// 在给定范围内搜索最优 skewness
    [[nodiscard]] double optimize_skewness(
            const IntPolynomial& f,
            const IntPolynomial& g,
            const Integer& n) {

        // 首先估计初始 skewness
        double init_skew = estimate_initial_skewness(f);

        // 在估计值附近搜索
        double min_skew = std::max(params_.skewness_min, init_skew / 100.0);
        double max_skew = std::min(params_.skewness_max, init_skew * 100.0);

        double best_skew = init_skew;
        double best_log_score = -1e100;

        // 对数尺度网格搜索
        double log_min = std::log(min_skew);
        double log_max = std::log(max_skew);
        double step = (log_max - log_min) / params_.skewness_steps;

        for (uint32_t i = 0; i <= params_.skewness_steps; ++i) {
            double s = std::exp(log_min + i * step);
            auto [log_score, linear_score] = sample_e_score_log(f, g, n, s);
            (void)linear_score;  // 不使用线性值

            if (log_score > best_log_score) {
                best_log_score = log_score;
                best_skew = s;
            }
        }

        return best_skew;
    }

    /// 获取参数
    [[nodiscard]] const MurphyParams& params() const noexcept {
        return params_;
    }

private:
    MurphyParams params_;
    std::mt19937_64 rng_;
    std::vector<uint32_t> small_primes_;
    std::vector<double> dickman_table_;  // Dickman rho 查找表

    /// 初始化小素数列表
    void init_primes() {
        // 埃拉托斯特尼筛法
        uint64_t bound = static_cast<uint64_t>(params_.alpha_bound);
        std::vector<bool> is_prime(bound + 1, true);
        is_prime[0] = is_prime[1] = false;

        for (uint64_t i = 2; i * i <= bound; ++i) {
            if (is_prime[i]) {
                for (uint64_t j = i * i; j <= bound; j += i) {
                    is_prime[j] = false;
                }
            }
        }

        small_primes_.clear();
        for (uint64_t i = 2; i <= bound; ++i) {
            if (is_prime[i]) {
                small_primes_.push_back(static_cast<uint32_t>(i));
            }
        }
    }

    /// 初始化 Dickman rho 查找表
    void init_dickman_table() {
        // 预计算 rho(u) for u = 0, 0.1, 0.2, ..., 20.0
        // 使用数值积分或已知近似值
        dickman_table_.clear();
        dickman_table_.reserve(201);

        for (int i = 0; i <= 200; ++i) {
            double u = i * 0.1;
            dickman_table_.push_back(dickman_rho_exact(u));
        }
    }

    /// Dickman rho 函数的精确计算
    /// rho(u) 是 [0,1] 上均匀分布的 u 个独立随机变量乘积小于 1 的概率
    [[nodiscard]] static double dickman_rho_exact(double u) {
        if (u <= 0.0) return 1.0;
        if (u <= 1.0) return 1.0;
        if (u <= 2.0) return 1.0 - std::log(u);

        // 对于 u > 2，使用递推和数值积分
        // rho(u) = (1/u) * integral_{u-1}^{u} rho(t) dt

        // 使用近似公式: rho(u) ~ exp(-u * (log(u) + log(log(u)) - 1 + ...))
        // 或使用查表插值

        // 简化近似（Knuth-Trabb Pardo 近似）
        double log_u = std::log(u);
        double log_log_u = std::log(log_u);

        // 更精确的 Hildebrand 近似
        double xi = log_u + log_log_u - 1.0
                  + (log_log_u - 2.0) / log_u
                  - (log_log_u * log_log_u - 6.0 * log_log_u + 11.0) / (2.0 * log_u * log_u);

        return std::exp(-u * xi + 0.5 * std::log(2.0 * M_PI * u) - u);
    }

    /// 计算 log(rho(u)) - 对数尺度避免下溢
    [[nodiscard]] static double log_dickman_rho(double u) {
        if (u <= 0.0) return 0.0;  // log(1) = 0
        if (u <= 1.0) return 0.0;  // log(1) = 0
        if (u <= 2.0) return std::log(1.0 - std::log(u));

        // 对于 u > 2，直接在对数空间计算
        double log_u = std::log(u);
        double log_log_u = std::log(log_u);

        // Hildebrand 近似的对数形式
        double xi = log_u + log_log_u - 1.0
                  + (log_log_u - 2.0) / log_u
                  - (log_log_u * log_log_u - 6.0 * log_log_u + 11.0) / (2.0 * log_u * log_u);

        // log(rho) ≈ -u * xi + 0.5 * log(2*pi*u) - u
        return -u * xi + 0.5 * std::log(2.0 * M_PI * u) - u;
    }

    /// 从查找表获取 Dickman rho（带插值）
    [[nodiscard]] double dickman_rho(double u) const {
        if (u <= 0.0) return 1.0;
        if (u >= 20.0) return dickman_rho_exact(u);

        // 线性插值
        double idx = u * 10.0;
        size_t i = static_cast<size_t>(idx);
        double frac = idx - i;

        if (i + 1 >= dickman_table_.size()) {
            return dickman_table_.back();
        }

        return dickman_table_[i] * (1.0 - frac) + dickman_table_[i + 1] * frac;
    }

    /// 计算光滑概率的对数
    /// log(P(n is B-smooth)) ≈ log(rho(log(n) / log(B)))
    [[nodiscard]] double log_smooth_probability(double log_value) const {
        double log_bound = std::log(static_cast<double>(params_.smoothness_bound));
        double u = log_value / log_bound;
        return log_dickman_rho(u);
    }

    /// 计算光滑概率（线性尺度，可能下溢）
    [[nodiscard]] double smooth_probability(double log_value) const {
        double log_bound = std::log(static_cast<double>(params_.smoothness_bound));
        double u = log_value / log_bound;
        return dickman_rho(u);
    }

    /// 基于采样计算 E-score（返回 log(E-score) 和线性 E-score）
    [[nodiscard]] std::pair<double, double> sample_e_score_log(
            const IntPolynomial& f,
            const IntPolynomial& g,
            const Integer& n,
            double skewness) {

        uint32_t d = f.degree();
        uint32_t valid_samples = 0;

        // 使用 log-sum-exp 技巧避免下溢
        // log(sum(exp(x_i))) = max(x) + log(sum(exp(x_i - max(x))))
        std::vector<double> log_probs;
        log_probs.reserve(params_.sample_points);

        // 计算采样区域大小
        double A = skewness;  // a 的范围: [-A, A]
        double B_range = std::sqrt(n.to_double()) / skewness;  // b 的范围

        std::uniform_real_distribution<double> dist_a(-A, A);
        std::uniform_real_distribution<double> dist_b(1.0, std::max(2.0, B_range));

        for (uint32_t i = 0; i < params_.sample_points; ++i) {
            double a = dist_a(rng_);
            double b = dist_b(rng_);

            // 跳过 gcd(a,b) != 1 的情况（简化处理）
            if (std::abs(a) < 1.0 || b < 1.0) continue;

            // 计算代数侧范数: F(a,b) = b^d * f(a/b)
            double ratio = a / b;
            double F_val = std::pow(b, d) * std::abs(f.evaluate_double(ratio));

            // 计算有理侧值: G(a,b) = g(a/b) * b^{deg(g)}
            double G_val = std::abs(g.evaluate_double(ratio)) * b;

            if (F_val < 1.0 || G_val < 1.0) continue;

            // 在对数空间计算
            double log_F = std::log(F_val);
            double log_G = std::log(G_val);

            double log_p_f = log_smooth_probability(log_F);
            double log_p_g = log_smooth_probability(log_G);

            // log(p_f * p_g) = log(p_f) + log(p_g)
            log_probs.push_back(log_p_f + log_p_g);
            ++valid_samples;
        }

        if (valid_samples == 0) {
            return {-1e100, 0.0};
        }

        // Log-sum-exp 计算平均值的对数
        // log(mean) = log(sum/n) = log(sum) - log(n)
        double max_log = *std::max_element(log_probs.begin(), log_probs.end());
        double sum_exp = 0.0;
        for (double lp : log_probs) {
            sum_exp += std::exp(lp - max_log);
        }

        double log_sum = max_log + std::log(sum_exp);
        double log_mean = log_sum - std::log(static_cast<double>(valid_samples));

        // 线性尺度（如果可能）
        double linear_e_score = 0.0;
        if (log_mean > -700) {  // 避免 exp 下溢
            linear_e_score = std::exp(log_mean);
        }

        return {log_mean, linear_e_score};
    }

    /// 基于采样计算 E-score（保留旧接口）
    [[nodiscard]] double sample_e_score(
            const IntPolynomial& f,
            const IntPolynomial& g,
            const Integer& n,
            double skewness) {
        auto [log_score, linear_score] = sample_e_score_log(f, g, n, skewness);
        return linear_score;
    }

    /// 计算大小得分
    [[nodiscard]] double compute_size_score(
            const IntPolynomial& f,
            const IntPolynomial& g,
            double skewness) const {

        // 大小得分基于多项式系数在给定 skewness 下的"平衡"程度
        // 较小的系数意味着较小的范数值

        uint32_t d_f = f.degree();
        uint32_t d_g = g.degree();

        // 计算 f 的加权系数和
        double size_f = 0.0;
        for (uint32_t i = 0; i <= d_f; ++i) {
            double ci = std::abs(f[i].to_double());
            // 权重使各项在 skewness 下贡献相当
            double weight = std::pow(skewness, static_cast<double>(i) - d_f / 2.0);
            size_f += ci * weight;
        }

        // 计算 g 的加权系数和
        double size_g = 0.0;
        for (uint32_t i = 0; i <= d_g; ++i) {
            double ci = std::abs(g[i].to_double());
            double weight = std::pow(skewness, static_cast<double>(i) - d_g / 2.0);
            size_g += ci * weight;
        }

        // 大小得分：越小越好，取倒数并归一化
        return 1.0 / (std::log(size_f + 1.0) * std::log(size_g + 1.0) + 1.0);
    }

    /// 计算根得分
    /// 基于多项式在小素数处根的分布
    [[nodiscard]] double compute_root_score(const IntPolynomial& f) const {
        double total_roots = 0.0;
        uint32_t d = f.degree();

        // 只检查前几个小素数
        size_t check_count = std::min(static_cast<size_t>(50), small_primes_.size());

        for (size_t i = 0; i < check_count; ++i) {
            uint32_t p = small_primes_[i];
            auto roots = f.roots_mod_p(p);
            total_roots += static_cast<double>(roots.size());
        }

        // 根得分: 根数越多越好（更容易找到光滑数）
        // 期望根数约为 degree
        double expected = d * check_count;
        return total_roots / expected;
    }

    /// 估计初始 skewness
    [[nodiscard]] double estimate_initial_skewness(const IntPolynomial& f) const {
        uint32_t d = f.degree();
        if (d == 0) return 1.0;

        double c0 = std::abs(f[0].to_double());
        double cd = std::abs(f[d].to_double());

        if (cd < 1e-10 || c0 < 1e-10) {
            return 1.0;
        }

        return std::pow(c0 / cd, 1.0 / d);
    }
};

/// 快速多项式比较（不进行完整的 Murphy 计算）
/// 用于初步筛选候选多项式
[[nodiscard]] inline bool quick_polynomial_compare(
        const IntPolynomial& f1, double skew1,
        const IntPolynomial& f2, double skew2) {

    // 启发式：系数较小的通常更好
    uint32_t d = f1.degree();
    double score1 = 0.0, score2 = 0.0;

    for (uint32_t i = 0; i <= d; ++i) {
        double c1 = std::abs(f1[i].to_double());
        double c2 = std::abs(f2[i].to_double());
        double expected_power1 = std::pow(skew1, static_cast<double>(d - i) / d);
        double expected_power2 = std::pow(skew2, static_cast<double>(d - i) / d);
        score1 += std::log(c1 / expected_power1 + 1.0);
        score2 += std::log(c2 / expected_power2 + 1.0);
    }

    return score1 < score2;  // 较小的得分更好
}

} // namespace polynomial
} // namespace gnfs
