#pragma once

#include "../core/integer.hpp"
#include "../util/thread_pool.hpp"
#include "int_polynomial.hpp"
#include "root_property_cache.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
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
        : params_(params),
          root_cache_(std::make_unique<RootPropertyCache>(
              RootPropertyCache::env_capacity())) {
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
    ///
    /// ENV `GNFS_MURPHY_ALPHA_THREADS=N`: ThreadPool size for parallel sweep
    /// (default = hardware concurrency, 0 = sequential). Each thread accumulates
    /// a partial sum over an index chunk of small_primes_; final reduction is
    /// a serial sum of partials (avoids atomic double overhead).
    [[nodiscard]] double compute_alpha(const IntPolynomial& f, double prime_bound) const {
        // 找 prime_bound 对应的 small_primes_ 截止索引
        size_t prime_end = small_primes_.size();
        for (size_t i = 0; i < small_primes_.size(); ++i) {
            if (small_primes_[i] > prime_bound) {
                prime_end = i;
                break;
            }
        }
        if (prime_end == 0) return 0.0;

        // Linear-polynomial short-circuit (CADO-NFS get_alpha line:
        //   if (f->deg == 1) return 0.569959993064325;).
        // For monic-linear g(x) = x + c0: every prime p has exactly one affine
        // root, no double root (g'=1), no projective root (lc=1).
        // Per-prime contribution: (1/p - 1/(p-1)) * log(p).
        // Sum is independent of c0; precomputed lazily per evaluator.
        // Note: Kleinjung's g(x) = x - m always has lc=1, so this fast path
        // covers every Murphy E call where g is the rational-side polynomial.
        //
        // ENV `GNFS_NO_LINEAR_SHORTCUT=1` disables short-circuit for benchmark
        // comparison (forces general path even for degree-1 monic).
        if (f.degree() == 1 && f.leading_coeff().is_one()) {
            const char* env = std::getenv("GNFS_NO_LINEAR_SHORTCUT");
            if (!(env && env[0] == '1')) {
                return linear_alpha_cached(prime_end);
            }
        }

        // 预计算 f' 用于双根检测 (read-only across threads)
        IntPolynomial df = f.derivative();

        auto* pool = get_alpha_pool();
        const size_t num_threads = pool ? pool->num_threads() : 1;

        if (num_threads <= 1 || prime_end < 256) {
            // 序列路径: 小 prime_end 或 ENV=0 时跳过并行 overhead
            double alpha = 0.0;
            for (size_t i = 0; i < prime_end; ++i) {
                alpha += alpha_contribution(f, df, small_primes_[i]);
            }
            return alpha;
        }

        // 并行: chunk by index, per-thread accumulator
        const size_t chunk_size = (prime_end + num_threads - 1) / num_threads;
        std::vector<double> partials(num_threads, 0.0);

        pool->parallel_for_index(0, num_threads, [&](size_t tid) {
            size_t lo = tid * chunk_size;
            size_t hi = std::min(lo + chunk_size, prime_end);
            double acc = 0.0;
            for (size_t i = lo; i < hi; ++i) {
                acc += alpha_contribution(f, df, small_primes_[i]);
            }
            partials[tid] = acc;
        });

        double alpha = 0.0;
        for (double p : partials) alpha += p;
        return alpha;
    }

public:
    /// Read-only access to the underlying root-property cache (for tests
    /// and diagnostics; disabled mode returns a non-null cache with
    /// capacity()==0). Returns a reference, so calling this on a moved-from
    /// evaluator is UB — same contract as STL container accessors.
    [[nodiscard]] const RootPropertyCache& root_cache() const noexcept {
        assert(root_cache_ && "root_cache_ is null (moved-from evaluator?)");
        return *root_cache_;
    }

private:
    /// Per-prime alpha contribution. Pure function of (f, df, p), thread-safe.
    /// When the root-property cache is enabled (env GNFS_POLY_ROOT_CACHE_SIZE),
    /// looks up (p, hash(f mod p)) before falling back to the uncached path.
    /// On a miss, computes the value and inserts it.
    [[nodiscard]] double alpha_contribution(
            const IntPolynomial& f,
            const IntPolynomial& df,
            uint32_t p) const {
        if (root_cache_->enabled()) {
            const uint64_t coeffs_hash = RootPropertyCache::hash_coeffs_mod_p(f, p);
            if (auto cached = root_cache_->lookup(p, coeffs_hash); cached.has_value()) {
                return cached.value();
            }
            const double value = alpha_contribution_uncached(f, df, p);
            root_cache_->insert(p, coeffs_hash, value);
            return value;
        }
        return alpha_contribution_uncached(f, df, p);
    }

    /// Cache-free per-prime alpha contribution. Pure function of (f, df, p).
    /// Extracted from alpha_contribution() so the cache wrapper can call it
    /// on a miss without recursion.
    [[nodiscard]] double alpha_contribution_uncached(
            const IntPolynomial& f,
            const IntPolynomial& df,
            uint32_t p) const {

        // 计算 f mod p 的根（不含重数）
        auto roots = f.roots_mod_p(p);
        uint32_t r = static_cast<uint32_t>(roots.size());

        double log_p = std::log(static_cast<double>(p));
        double contribution = (static_cast<double>(r) / p
                             - 1.0 / (p - 1)) * log_p;

        // 双根额外贡献 (p | disc(f))
        for (uint32_t root : roots) {
            if (df.evaluate_mod(root, p) == 0) {
                contribution += log_p / (static_cast<double>(p) * p);
            }
        }

        // 投影根的额外贡献 (p | leading_coeff(f))
        // 参考 Guillevic & Singh (2021), Eq 4.7 + Prop 1.
        if (f.leading_coeff().fits_uint64()) {
            if (f.leading_coeff().to_uint64() % p == 0) {
                contribution += log_p / p;
            }
        }

        return contribution;
    }

    /// Linear-poly cache: sum_{p ≤ small_primes_[prime_end-1]} (1/p - 1/(p-1)) · log(p).
    /// Lazy-computed prefix-sum of per-prime contributions for monic-linear polys.
    /// prefix_sum[i] = Σ_{j<i} contribution_at_prime(small_primes_[j]).
    /// Returns prefix_sum[prime_end] for the requested cutoff index.
    [[nodiscard]] double linear_alpha_cached(size_t prime_end) const {
        std::call_once(linear_alpha_init_, [this]() {
            linear_alpha_prefix_.resize(small_primes_.size() + 1, 0.0);
            double acc = 0.0;
            for (size_t i = 0; i < small_primes_.size(); ++i) {
                const double p = static_cast<double>(small_primes_[i]);
                const double log_p = std::log(p);
                // Monic linear: exactly 1 affine root per prime, no double root
                // (since g' = 1), no projective root (since lc = 1). Per-prime
                // contribution matches alpha_contribution() for r=1, leading=1, df=1.
                acc += (1.0 / p - 1.0 / (p - 1.0)) * log_p;
                linear_alpha_prefix_[i + 1] = acc;
            }
        });
        if (prime_end >= linear_alpha_prefix_.size()) {
            prime_end = linear_alpha_prefix_.size() - 1;
        }
        return linear_alpha_prefix_[prime_end];
    }

    /// Lazy ThreadPool init (per-evaluator instance, shared across compute_alpha calls).
    /// ENV GNFS_MURPHY_ALPHA_THREADS overrides hardware concurrency default.
    /// Returns nullptr if user set threads=0 (forces sequential).
    [[nodiscard]] gnfs::util::ThreadPool* get_alpha_pool() const {
        std::call_once(pool_init_, [this]() {
            uint32_t requested = 0;
            const char* env = std::getenv("GNFS_MURPHY_ALPHA_THREADS");
            if (env && env[0] != '\0') {
                int val = std::atoi(env);
                if (val < 0) val = 0;
                requested = static_cast<uint32_t>(val);
            } else {
                requested = std::thread::hardware_concurrency();
                if (requested == 0) requested = 4;
            }
            if (requested == 0) {
                alpha_pool_ = nullptr;
            } else {
                alpha_pool_ = std::make_unique<gnfs::util::ThreadPool>(requested);
            }
        });
        return alpha_pool_.get();
    }

public:

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

    // Root-property cache. Always non-null (unique_ptr); disabled iff
    // capacity()==0 (ENV GNFS_POLY_ROOT_CACHE_SIZE unset or 0). Mutable so
    // const compute_alpha can update hit/miss counters and insert misses.
    mutable std::unique_ptr<RootPropertyCache> root_cache_;

    // ThreadPool for parallel compute_alpha (BACKLOG #2 lightweight optimization).
    // Mutable + once_flag for lazy init in const get_alpha_pool().
    mutable std::once_flag pool_init_;
    mutable std::unique_ptr<gnfs::util::ThreadPool> alpha_pool_;

    // Linear-polynomial alpha cache (CADO get_alpha deg==1 fast path).
    // Precomputed prefix-sum across small_primes_ enables O(1) alpha lookup
    // for any monic linear polynomial with any alpha_bound. Initialized lazily
    // on first call. linear_alpha_prefix_[i] = Σ_{j<i} contribution_at(small_primes_[j]).
    mutable std::once_flag linear_alpha_init_;
    mutable std::vector<double> linear_alpha_prefix_;

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
        // π(bound) ≈ bound / ln(bound) — reserve to avoid log(n) reallocations.
        small_primes_.reserve(static_cast<size_t>(bound / std::max(std::log(static_cast<double>(bound)), 1.0)));
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
