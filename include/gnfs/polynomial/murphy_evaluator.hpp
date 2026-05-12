#pragma once

#include "../core/integer.hpp"
#include "int_polynomial.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <vector>

namespace gnfs::polynomial {

using core::Integer;

/// Murphy E-score 的各个组成部分
struct MurphyScore {
    double e_score = 0.0;       // 综合 Murphy E 值（线性尺度，可能为0）
    double log_e_score = -1e100; // log(E-score)，用于大数比较（越大越好）
    double alpha_f = 0.0;       // f 的 alpha 值（越负越好）
    double alpha_g = 0.0;       // g 的 alpha 值（越负越好）
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
    uint32_t sample_points = 2000;          // 积分采样点数
    double alpha_bound = 1e6;               // alpha 计算的素数上界 (was 1e7; 10× smaller sieve)
    uint64_t smoothness_bound = 1000000;    // 光滑性界
    double skewness_min = 1e2;              // skewness 搜索下界
    double skewness_max = 1e10;             // skewness 搜索上界
    uint32_t skewness_steps = 100;          // skewness 网格搜索步数
};

/// MurphyEvaluator - Murphy E-score 评估器
/// 用于评估 GNFS 多项式对 (f, g) 的质量
///
/// 实现标准 Murphy E 公式（Murphy 1999, CADO-NFS）:
///   E(f,g,s) = (1/π) ∫₀^π ρ(u_f(θ)) · ρ(u_g(θ)) dθ
/// 其中 u_f(θ) = (log|F_s(cosθ, sinθ)| - α_f) / log(B)
///
/// 三处修正（相对于旧实现）:
/// 1. Alpha 集成到 Dickman rho 参数中（非 post-hoc `/10.0` 校正）
/// 2. 角度积分替代随机 (a,b) 采样（消除 sqrt(N) 采样区域错误）
/// 3. Dickman rho 表使用 ODE 数值积分（非错误渐近公式）
class MurphyEvaluator {
public:
    /// 构造评估器
    explicit MurphyEvaluator(const MurphyParams& params = MurphyParams{})
        : params_(params) {
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
    /// @param n 待分解的数（保留参数兼容性，不再内部使用）
    /// @return Murphy 评分结构
    [[nodiscard]] MurphyScore compute(
            const IntPolynomial& f,
            const IntPolynomial& g,
            const Integer& n) const {

        (void)n;

        // alpha 计算极慢 (compute_alpha 扫 ~78k 素数 + Cantor-Zassenhaus 求根)。
        // 原代码: compute() → optimize_skewness() 算 2 次 + 内部 compute(…,skew)
        // 又算 2 次 = 共 4 次 alpha。alpha 不依赖 skewness,只算一次然后传入。
        double alpha_f = compute_alpha(f);
        double alpha_g = compute_alpha(g);

        double best_skewness = optimize_skewness_with_alphas(f, g, alpha_f, alpha_g);
        return compute_with_alphas(f, g, best_skewness, alpha_f, alpha_g);
    }

    /// 计算 Murphy E-score（指定 skewness,内部入口,重算 alpha)
    [[nodiscard]] MurphyScore compute(
            const IntPolynomial& f,
            const IntPolynomial& g,
            const Integer& n,
            double skewness) const {

        (void)n;
        return compute_with_alphas(f, g, skewness,
                                    compute_alpha(f), compute_alpha(g));
    }

private:
    /// 内部:已知 alpha 的 score 构造,避免重复计算。
    [[nodiscard]] MurphyScore compute_with_alphas(
            const IntPolynomial& f,
            const IntPolynomial& g,
            double skewness,
            double alpha_f,
            double alpha_g) const {

        MurphyScore score;
        score.skewness = skewness;
        score.alpha_f = alpha_f;
        score.alpha_g = alpha_g;

        // 角度积分计算 E-score，alpha 已集成到 Dickman rho 参数中
        auto [log_e, linear_e] = compute_e_score_log(
            f, g, skewness, alpha_f, alpha_g);

        score.log_e_score = log_e;
        score.e_score = linear_e;

        return score;
    }

public:
    /// 计算多项式的 alpha 值
    /// alpha = Σ_p (r_p/p - 1/(p-1)) · log(p)
    /// 正 alpha = 多根，值更易被小素数整除（好）
    /// 负 alpha = 少根，值不易被小素数整除（差）
    [[nodiscard]] double compute_alpha(const IntPolynomial& f) const {
        return compute_alpha(f, params_.alpha_bound);
    }

    /// 计算 alpha 值（指定素数上界）
    ///
    /// alpha = Σ_p [ (effective_r_p / p - 1/(p-1)) + double_root_bonus ] · log(p)
    /// 其中 double_root_bonus = 1/p² per double root（f'(r) ≡ 0 mod p）
    [[nodiscard]] double compute_alpha(const IntPolynomial& f, double prime_bound) const {
        double alpha = 0.0;

        // 预计算 f' 用于双根检测
        IntPolynomial df = f.derivative();

        for (uint32_t p : small_primes_) {
            if (p > prime_bound) break;

            // 计算 f mod p 的根（不含重数）
            auto roots = f.roots_mod_p(p);
            uint32_t r = static_cast<uint32_t>(roots.size());

            // 基础贡献:
            // contribution = (r/p - 1/(p-1)) * log(p)
            // r/p 是被 p 整除的概率
            // 1/(p-1) 是随机数被 p 整除的期望
            double log_p = std::log(static_cast<double>(p));
            double contribution = (static_cast<double>(r) / p
                                 - 1.0 / (p - 1)) * log_p;

            // 双根额外贡献 (p | disc(f))
            // 若 f(r) ≡ 0 且 f'(r) ≡ 0 mod p，则 r 是双根
            // 双根使 p² | f(a - bα) 的概率增加 1/p²
            for (uint32_t root : roots) {
                if (df.evaluate_mod(root, p) == 0) {
                    contribution += log_p / (static_cast<double>(p) * p);
                }
            }

            // 投影根的额外贡献
            // 如果 p | leading_coeff(f)，则有投影根
            // ─── [VERIFY] BACKLOG P1-OPT ─────────────────────────────────────
            // CADO-NFS polyselect/alpha.c special_val0 中投影根贡献的精确
            // 表达可能不同 (常见为 log_p/(p*(p-1)) 或类似量级)。当前 +log_p/p
            // 将投影根视为额外 1 个 affine root,在大多数 Murphy E 排序场景
            // 给出合理结果,但严格 Murphy 1999 形式需 paper 核对。
            // 改动前需先确立新公式下的 test_murphy 黄金值。
            // ─────────────────────────────────────────────────────────────────
            if (f.leading_coeff().fits_uint64()) {
                if (f.leading_coeff().to_uint64() % p == 0) {
                    contribution += log_p / p;
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
            const Integer& n) const {

        (void)n;
        return optimize_skewness_with_alphas(
            f, g, compute_alpha(f), compute_alpha(g));
    }

    /// 已知 alpha 的 skewness 优化(避免外部已算 alpha 后再次重算)
    [[nodiscard]] double optimize_skewness_with_alphas(
            const IntPolynomial& f,
            const IntPolynomial& g,
            double alpha_f,
            double alpha_g) const {

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
            auto [log_score, linear_score] = compute_e_score_log(
                f, g, s, alpha_f, alpha_g);
            (void)linear_score;

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
    std::vector<uint32_t> small_primes_;
    std::vector<double> dickman_table_;  // Dickman rho 查找表 (u=0,0.1,...,20.0)

    /// 初始化小素数列表
    void init_primes() {
        // 埃拉托斯特尼筛法。alpha_bound 是 double — 用户可能误传 1e18 之类
        // 巨大值导致 bitset 试图分配 EB 级内存。clamp 到 1e7(~620k primes,
        // ~10MB sieve)足以满足 Murphy alpha 精度需求。
        constexpr double ALPHA_BOUND_MAX = 1e7;
        double bound_d = std::min(params_.alpha_bound, ALPHA_BOUND_MAX);
        if (bound_d < 2.0) bound_d = 2.0;
        uint64_t bound = static_cast<uint64_t>(bound_d);
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

    /// 初始化 Dickman rho 查找表（积分关系 + 梯形法）
    /// 使用 u·ρ(u) = ∫_{u-1}^{u} ρ(t) dt, h=0.001, 相对误差 < 5e-6
    void init_dickman_table() {
        constexpr double h = 0.001;
        constexpr int N_fine = 20001;   // u ∈ [0, 20.0]
        constexpr int lag = 1000;       // 1.0 / h

        std::vector<double> rho(N_fine, 0.0);

        // Exact values for u ∈ [0, 2]
        for (int i = 0; i < N_fine; ++i) {
            double u = i * h;
            if (u <= 1.0) {
                rho[i] = 1.0;
            } else if (u <= 2.0) {
                rho[i] = 1.0 - std::log(u);
            } else {
                break;
            }
        }

        // For u > 2: trapezoidal approximation of the integral relation
        // ρ[k] = (ρ[k-lag]/2 + Σ_{j=k-lag+1}^{k-1} ρ[j]) / (k - 0.5)
        int start = static_cast<int>(2.0 / h) + 1;

        // Initialize running interior sum: Σ_{j=start-lag+1}^{start-1} ρ[j]
        double interior_sum = 0.0;
        for (int j = start - lag + 1; j <= start - 1; ++j) {
            interior_sum += rho[j];
        }

        for (int k = start; k < N_fine; ++k) {
            rho[k] = (rho[k - lag] * 0.5 + interior_sum) / (k - 0.5);
            // Update running sum for next step
            interior_sum = interior_sum - rho[k - lag + 1] + rho[k];
        }

        // Downsample to 0.1 resolution for lookup table
        dickman_table_.clear();
        dickman_table_.reserve(201);
        constexpr int ratio = 100;  // 0.1 / 0.001
        for (int i = 0; i <= 200; ++i) {
            int fine_idx = std::min(i * ratio, N_fine - 1);
            dickman_table_.push_back(rho[fine_idx]);
        }
    }

    /// 从查找表获取 Dickman rho（带线性插值）
    [[nodiscard]] double dickman_rho(double u) const {
        if (u <= 0.0) return 1.0;

        if (u >= 20.0) {
            // Hildebrand 渐近: ρ(u) ≈ exp(-u·ξ)
            double log_u = std::log(u);
            double log_log_u = std::log(log_u);
            double xi = log_u + log_log_u - 1.0
                      + (log_log_u - 2.0) / log_u
                      - (log_log_u * log_log_u - 6.0 * log_log_u + 11.0)
                        / (2.0 * log_u * log_u);
            return std::exp(-u * xi);
        }

        // 线性插值
        double idx = u * 10.0;
        size_t i = static_cast<size_t>(idx);
        double frac = idx - static_cast<double>(i);

        if (i + 1 >= dickman_table_.size()) {
            return dickman_table_.back();
        }

        return dickman_table_[i] * (1.0 - frac) + dickman_table_[i + 1] * frac;
    }

    /// 计算 log(ρ(u))（使用表避免下溢）
    [[nodiscard]] double log_dickman_rho(double u) const {
        if (u <= 0.0) return 0.0;
        if (u <= 1.0) return 0.0;

        if (u >= 20.0) {
            // Hildebrand 渐近: log ρ(u) ≈ -u·ξ
            double log_u = std::log(u);
            double log_log_u = std::log(log_u);
            double xi = log_u + log_log_u - 1.0
                      + (log_log_u - 2.0) / log_u
                      - (log_log_u * log_log_u - 6.0 * log_log_u + 11.0)
                        / (2.0 * log_u * log_u);
            return -u * xi;
        }

        // 从 ODE 表获取并取对数
        double val = dickman_rho(u);
        if (val <= 0.0) return -1e100;
        return std::log(val);
    }

    /// 角度积分计算 Murphy E-score
    /// E = (1/π) ∫₀^π ρ(u_f(θ)) · ρ(u_g(θ)) dθ
    /// 其中 u_f = (log|F_s(cosθ, sinθ)| - α_f) / log(B)
    ///
    /// F_s(x,y) = Σ f_i · s^i · x^i · y^(d-i)  是 skewed 齐次多项式
    [[nodiscard]] std::pair<double, double> compute_e_score_log(
            const IntPolynomial& f,
            const IntPolynomial& g,
            double skewness,
            double alpha_f,
            double alpha_g) const {

        uint32_t d_f = f.degree();
        uint32_t d_g = g.degree();
        double log_bound = std::log(static_cast<double>(params_.smoothness_bound));

        if (log_bound <= 0) return {-1e100, 0.0};

        const uint32_t num_points = params_.sample_points;

        // Thread-local buffers — sample_points=2000 时原代码每次 alloc 3 个
        // ~2000 向量 + 内部 2 个 (max_deg+1)。Kleinjung Stage2 调 33000 次,
        // 几百 MB/s alloc/free 浪费。GNFS degree ≤ 6,ct_pow/st_pow 用
        // std::array 完全栈分配。
        thread_local std::vector<double> log_probs;
        log_probs.clear();
        log_probs.reserve(num_points);

        // Precompute skewness powers (was: 3 × std::pow per (i,j) pair)
        uint32_t max_deg = std::max(d_f, d_g);
        constexpr uint32_t MAX_DEG_STACK = 16;  // GNFS degree ≤ 6,余量到 16
        assert(max_deg <= MAX_DEG_STACK && "Murphy compute_e_score_log: degree too high");
        std::array<double, MAX_DEG_STACK + 1> skew_pow{};
        skew_pow[0] = 1.0;
        for (uint32_t j = 1; j <= max_deg; ++j)
            skew_pow[j] = skew_pow[j - 1] * skewness;

        for (uint32_t i = 0; i < num_points; ++i) {
            // Midpoint rule: θ = π(i + 0.5) / N
            double theta = M_PI * (i + 0.5) / num_points;
            double ct = std::cos(theta);
            double st = std::sin(theta);

            // Precompute cos/sin powers for this angle — 栈分配,零堆压力
            std::array<double, MAX_DEG_STACK + 1> ct_pow{}, st_pow{};
            ct_pow[0] = st_pow[0] = 1.0;
            for (uint32_t j = 1; j <= max_deg; ++j) {
                ct_pow[j] = ct_pow[j - 1] * ct;
                st_pow[j] = st_pow[j - 1] * st;
            }

            // F_s(cosθ, sinθ) = Σ f_i · s^i · cos^i(θ) · sin^(d_f-i)(θ)
            double F_val = 0.0;
            for (uint32_t j = 0; j <= d_f; ++j) {
                double ci = f[j].to_double();
                F_val += ci * skew_pow[j] * ct_pow[j] * st_pow[d_f - j];
            }
            F_val = std::abs(F_val);

            // G_s(cosθ, sinθ)
            double G_val = 0.0;
            for (uint32_t j = 0; j <= d_g; ++j) {
                double ci = g[j].to_double();
                G_val += ci * skew_pow[j] * ct_pow[j] * st_pow[d_g - j];
            }
            G_val = std::abs(G_val);

            if (F_val < 1e-300 || G_val < 1e-300) continue;

            double log_F = std::log(F_val);
            double log_G = std::log(G_val);

            // Murphy E 公式: u = (log|norm| - alpha) / log(B)
            // alpha > 0 (多根) → u 减小 → ρ 增大 → 更好
            // alpha < 0 (少根) → u 增大 → ρ 减小 → 更差
            double u_f = (log_F - alpha_f) / log_bound;
            double u_g = (log_G - alpha_g) / log_bound;

            // u ≤ 0 时 ρ(u) = 1 (必然光滑)。旧代码把 u_f 抬到 0.01 让 ρ(0.01)≈1
            // 同时 ρ(其他 u_f) 范围被压缩,导致大负 u_f 之间无法区分,排序失效。
            // 改为:u ≤ 0 直接 log_p = 0,避免数值病态。
            double log_p_f = (u_f <= 0.0) ? 0.0 : log_dickman_rho(u_f);
            double log_p_g = (u_g <= 0.0) ? 0.0 : log_dickman_rho(u_g);

            log_probs.push_back(log_p_f + log_p_g);
        }

        if (log_probs.empty()) {
            return {-1e100, 0.0};
        }

        // Log-sum-exp 计算平均值的对数
        double max_log = *std::max_element(log_probs.begin(), log_probs.end());
        double sum_exp = 0.0;
        for (double lp : log_probs) {
            sum_exp += std::exp(lp - max_log);
        }

        double log_sum = max_log + std::log(sum_exp);
        double log_mean = log_sum - std::log(static_cast<double>(log_probs.size()));

        double linear_e_score = 0.0;
        if (log_mean > -700) {
            linear_e_score = std::exp(log_mean);
        }

        return {log_mean, linear_e_score};
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

} // namespace gnfs::polynomial
