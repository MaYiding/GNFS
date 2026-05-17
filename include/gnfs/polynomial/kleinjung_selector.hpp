#pragma once

#include "../core/integer.hpp"
#include "../core/params.hpp"
#include "../core/polynomial_context.hpp"
#include "../util/thread_pool.hpp"
#include "int_polynomial.hpp"
#include "murphy_evaluator.hpp"
#include "polynomial_optimizer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace gnfs::polynomial {

using core::Integer;
using core::GNFSParams;
using core::PolynomialContext;
using util::ThreadPool;

/// Kleinjung 多项式选择参数
struct KleinjungParams {
    uint32_t degree = 5;                      // 多项式度数 (5 或 6)
    double skewness_min = 1e4;                // skewness 搜索下界
    double skewness_max = 1e7;                // skewness 搜索上界
    uint64_t leading_coeff_bound = 10000;     // 领导系数 |a_d| 上界
    uint32_t num_candidates = 1000;           // Stage 1 候选数量
    uint32_t root_opt_iterations = 256;       // 牛顿法迭代次数
    double root_opt_precision = 1e-6;         // 牛顿法收敛精度
    uint32_t search_radius = 100;             // m 搜索半径
    bool parallel = true;                     // 启用并行
    uint32_t num_threads = 0;                 // 线程数 (0 = 自动)

    // Murphy 评估参数
    MurphyParams murphy_params;

    /// 从 GNFSParams 自动推导 Kleinjung 参数
    ///
    /// GNFSParams::compute() 已根据 L_N 理论和 CADO-NFS 校准计算了
    /// leading_coeff_bound, search_radius, num_candidates 等参数。
    /// 此方法将这些值映射到 KleinjungParams，保证参数一致性。
    [[nodiscard]] static KleinjungParams from_gnfs_params(const GNFSParams& gp) {
        KleinjungParams kp;
        kp.degree = gp.degree;
        kp.leading_coeff_bound = gp.leading_coeff_bound;
        kp.search_radius = static_cast<uint32_t>(
            std::min(gp.search_radius, static_cast<uint64_t>(UINT32_MAX)));
        kp.num_candidates = gp.num_candidates;

        // Skewness 范围随 N 缩放:
        // 小 N: skewness ~ 1e2-1e5, 大 N: skewness ~ 1e4-1e10
        if (gp.digits <= 40) {
            kp.skewness_min = 1e2;
            kp.skewness_max = 1e6;
        } else if (gp.digits <= 80) {
            kp.skewness_min = 1e3;
            kp.skewness_max = 1e8;
        } else {
            kp.skewness_min = 1e4;
            kp.skewness_max = 1e10;
        }

        // 牛顿法参数: 大 N 需更多迭代
        kp.root_opt_iterations = (gp.digits <= 40) ? 128 : 256;

        // Murphy 参数: alpha_bound 随 FB 缩放但上限 1e6 (够用)
        kp.murphy_params.alpha_bound = std::min(
            static_cast<double>(gp.algebraic_bound), 1e6);
        kp.murphy_params.smoothness_bound = gp.algebraic_bound;

        // sample_points 随 digits 连续缩放:digits·30 + 200,clamp [100, 5000]。
        // 原公式 max(500, min(5000, digits·50)) 在 digits∈[8,10] 处都 clamp 到 500,
        // digits=11 跳到 550,人为台阶。连续公式让小 N 也按比例少采样。
        double sp_cont = static_cast<double>(gp.digits) * 30.0 + 200.0;
        uint32_t sp = static_cast<uint32_t>(std::clamp(sp_cont, 100.0, 5000.0));
        kp.murphy_params.sample_points = sp;

        // skewness 搜索步数
        kp.murphy_params.skewness_steps = gp.skewness_steps;

        return kp;
    }
};

/// 进度回调函数类型
/// (当前进度, 总数, 当前最佳分数, 阶段名称)
using KleinjungProgressCallback = std::function<void(
    size_t current,
    size_t total,
    double best_score,
    const char* stage)>;

/// Kleinjung 选择结果
struct KleinjungResult {
    IntPolynomial f;              // 代数侧多项式
    IntPolynomial g;              // 有理侧多项式 g(x) = x - m
    Integer m;                    // f(m) ≡ 0 (mod n)
    double skewness = 1.0;        // 最优 skewness
    MurphyScore score;            // Murphy E-score
    bool success = false;         // 是否成功
    size_t candidates_tested = 0; // 测试的候选数
    double elapsed_seconds = 0.0; // 耗时
};

/// KleinjungSelector - Kleinjung 两阶段多项式选择
/// Stage 1: 搜索光滑的领导系数
/// Stage 2: 牛顿法优化根
class KleinjungSelector {
public:
    /// 构造选择器
    explicit KleinjungSelector(const KleinjungParams& params = KleinjungParams{})
        : params_(params)
        , cancelled_(false) {
    }

    /// Move-only 语义
    KleinjungSelector(KleinjungSelector&& other) noexcept
        : params_(std::move(other.params_))
        , progress_callback_(std::move(other.progress_callback_))
        , cancelled_(other.cancelled_.load()) {
    }

    KleinjungSelector& operator=(KleinjungSelector&& other) noexcept {
        if (this != &other) {
            params_ = std::move(other.params_);
            progress_callback_ = std::move(other.progress_callback_);
            cancelled_.store(other.cancelled_.load());
        }
        return *this;
    }

    KleinjungSelector(const KleinjungSelector&) = delete;
    KleinjungSelector& operator=(const KleinjungSelector&) = delete;

    ~KleinjungSelector() = default;

    /// 设置进度回调
    void set_progress_callback(KleinjungProgressCallback callback) {
        progress_callback_ = std::move(callback);
    }

    /// 取消搜索（线程安全）
    void cancel() noexcept {
        cancelled_.store(true);
    }

    /// 检查是否已取消
    [[nodiscard]] bool is_cancelled() const noexcept {
        return cancelled_.load();
    }

    /// 选择最佳多项式
    /// @param n 待分解的数
    /// @return 选择结果
    [[nodiscard]] KleinjungResult select(const Integer& n) {
        cancelled_.store(false);
        auto start_time = std::chrono::high_resolution_clock::now();

        KleinjungResult result;
        result.success = false;

        // 验证输入
        if (n.is_zero() || n.is_negative()) {
            return result;
        }

        // Stage 1: 生成候选领导系数
        report_progress(0, 1, 0.0, "Stage 1: Generating candidates");
        auto candidates = stage1_leading_coeff_search(n);

        if (candidates.empty() || is_cancelled()) {
            return result;
        }

        // Stage 2: 对每个候选进行旋转 + 平移优化
        MurphyEvaluator evaluator(params_.murphy_params);
        std::optional<KleinjungResult> best_result;
        double best_log_score = -1e100;
        std::mutex result_mutex;
        std::atomic<size_t> progress_count{0};

        auto process_candidate = [&](size_t idx) {
            if (is_cancelled()) return;

            const auto& [ad, m_init] = candidates[idx];
            auto candidate_result = stage2_root_optimization(n, ad, m_init, evaluator);

            if (candidate_result.has_value()) {
                std::lock_guard<std::mutex> lock(result_mutex);
                if (candidate_result->score.log_e_score > best_log_score) {
                    best_log_score = candidate_result->score.log_e_score;
                    best_result = std::move(candidate_result);
                }
            }

            // 原子计数器确保并行下进度单调递增
            size_t current = progress_count.fetch_add(1, std::memory_order_relaxed) + 1;
            {
                std::lock_guard<std::mutex> lock(result_mutex);
                report_progress(current, candidates.size(), best_log_score, "Stage 2: Optimizing");
            }
        };

        if (params_.parallel && candidates.size() > 1) {
            // 并行处理
            uint32_t num_threads = params_.num_threads > 0
                ? params_.num_threads
                : std::thread::hardware_concurrency();

            ThreadPool pool(num_threads);
            pool.parallel_for_index(0, candidates.size(),
                [&](size_t idx) { process_candidate(idx); });
        } else {
            // 串行处理
            for (size_t idx = 0; idx < candidates.size(); ++idx) {
                process_candidate(idx);
                if (is_cancelled()) break;
            }
        }

        // 计算耗时
        auto end_time = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(end_time - start_time).count();

        if (best_result.has_value()) {
            result = std::move(best_result.value());
            result.candidates_tested = progress_count.load(std::memory_order_relaxed);
            result.elapsed_seconds = elapsed;
        }

        return result;
    }

    /// 获取参数
    [[nodiscard]] const KleinjungParams& params() const noexcept {
        return params_;
    }

private:
    KleinjungParams params_;
    KleinjungProgressCallback progress_callback_;
    std::atomic<bool> cancelled_;

    /// 报告进度
    void report_progress(size_t current, size_t total, double best_score, const char* stage) {
        if (progress_callback_) {
            progress_callback_(current, total, best_score, stage);
        }
    }

    /// Stage 1: 领导系数搜索
    /// 生成光滑的 a_d 和对应的初始 m 值
    [[nodiscard]] std::vector<std::pair<Integer, Integer>>
    stage1_leading_coeff_search(const Integer& n) {
        // Reserve: loop caps at num_candidates * 2 (line 329), reserve up to that.
        std::vector<std::pair<Integer, Integer>> candidates;
        candidates.reserve(params_.num_candidates * 2);

        uint32_t d = params_.degree;

        // 计算 m_base = floor(n^{1/d})
        Integer m_base;
        mpz_root(m_base.get_mpz(), n.get_mpz(), d);

        // 生成光滑系数
        auto smooth_coeffs = generate_smooth_coefficients(params_.leading_coeff_bound);

        for (const auto& ad : smooth_coeffs) {
            if (is_cancelled()) break;
            if (ad.is_zero()) continue;

            // 对于给定的 a_d，寻找最佳的 m
            // 使得 n = a_d * m^d + a_{d-1} * m^{d-1} + ...
            // 其中 |a_{d-1}| 尽可能小

            // 估计 m = (n / a_d)^{1/d}
            Integer n_div_ad;
            Integer div_remainder;
            Integer::divmod(n_div_ad, div_remainder, n, ad);
            Integer m_est;
            mpz_root(m_est.get_mpz(), n_div_ad.get_mpz(), d);

            // 在 m_est 附近搜索
            // v22: m/ad_md/remainder buffer 复用 across delta iterations
            Integer m;
            Integer ad_md;
            Integer remainder;
            for (int32_t delta = -static_cast<int32_t>(params_.search_radius);
                 delta <= static_cast<int32_t>(params_.search_radius);
                 ++delta) {

                m = m_est;
                if (delta >= 0) {
                    m += delta;
                } else {
                    m -= (-delta);
                }

                if (m.is_zero() || m.is_negative()) continue;

                // 计算 n - a_d * m^d
                Integer m_pow_d = core::pow(m, d);
                ad_md = ad;
                ad_md *= m_pow_d;

                remainder = n;
                remainder -= ad_md;

                // 检查 remainder 的符号和大小
                // 如果 remainder 为负，说明 a_d * m^d > n，跳过
                if (remainder.is_negative()) continue;

                // 计算 a_{d-1} = remainder / m^{d-1}
                Integer m_pow_d1 = core::pow(m, d - 1);
                Integer ad1;
                Integer ad1_remainder;
                Integer::divmod(ad1, ad1_remainder, remainder, m_pow_d1);

                // 使用 m 作为系数大小参考
                // 系数应该和 m 在同一数量级或更小
                double m_val = m.to_double();
                double ad1_val = std::abs(ad1.to_double());

                // Stage 2 旋转优化只改 a₀/a₁，a_{d-1} 须在合理范围
                if (ad1_val <= m_val * 1.0) {
                    candidates.emplace_back(ad, m);  // Integer copy ctors

                    // 限制候选数量
                    if (candidates.size() >= params_.num_candidates * 2) {
                        break;
                    }
                }
            }

            if (candidates.size() >= params_.num_candidates * 2) {
                break;
            }
        }

        // 按 |a_d| 排序（小的优先）
        std::sort(candidates.begin(), candidates.end(),
            [](const auto& a, const auto& b) {
                double va = std::abs(a.first.to_double());
                double vb = std::abs(b.first.to_double());
                return va < vb;
            });

        // 限制数量
        if (candidates.size() > params_.num_candidates) {
            candidates.resize(params_.num_candidates);
        }

        return candidates;
    }

    /// Stage 2: 旋转 + 平移优化
    /// 对每个 (a_d, m) 候选进行：
    ///   1. 平移搜索 t ∈ [-5, +5]，产生 f(x+t) 与 m' = m - t
    ///   2. 闭合形式旋转优化：k = round((m·a₀ - s²·a₁) / (m² + s²))
    ///   3. L² norm 预筛 → Murphy E 精确评分
    [[nodiscard]] std::optional<KleinjungResult>
    stage2_root_optimization(
            const Integer& n,
            const Integer& ad,
            const Integer& m_init,
            const MurphyEvaluator& evaluator) {

        uint32_t d = params_.degree;

        // 构造初始多项式 via base-m 展开
        auto f_init = construct_polynomial(n, ad, m_init, d);
        if (!f_init.has_value()) return std::nullopt;
        if (!is_valid_polynomial(*f_init, n, m_init)) return std::nullopt;

        // 保留 top-K (K=3) L² 最小的候选,然后用 Murphy E 二级筛。
        // 原代码只取 L² min — 但 L² 不含 α,L² 最小 不等于 Murphy 最佳。
        // 例: α 极负但 L² 略大的多项式实际上更优 (CADO-NFS 经验)。
        struct Candidate {
            IntPolynomial f;
            Integer m;
            double norm;
        };
        constexpr size_t TOP_K = 3;
        std::vector<Candidate> top_k;
        top_k.reserve(TOP_K + 1);

        // 平移 + 旋转网格搜索。t_range 之前硬编码 ±5(11 点),与 KleinjungParams
        // 的 search_radius 无关,大 N 下平移空间被严重压缩。改为按
        // search_radius 缩放(默认 50),提供更充裕的平移采样。
        const int t_range = static_cast<int>(
            std::min<uint64_t>(params_.search_radius, 50));
        for (int t = -t_range; t <= t_range; ++t) {
            IntPolynomial f_t;
            Integer m_t;

            if (t == 0) {
                f_t = f_init->clone();
                m_t = m_init;  // mpz_set into default-init buffer (no tmp alloc)
            } else {
                // f(x+t) 满足 f(x+t)|_{x=m-t} = f(m) ≡ 0 (mod N)
                f_t = PolynomialOptimizer::translate(*f_init, static_cast<int64_t>(t));
                m_t = m_init;  // mpz_set, then mutate in place
                m_t -= t;
                if (m_t.is_zero() || m_t.is_negative()) continue;
            }

            // 度数可能被 normalize() 改变（极罕见：平移导致领导系数抵消）
            if (f_t.degree() != d) continue;

            // 迭代旋转优化 (3 轮)
            // f_new = f + k·(x - m)，只改 a₀ 和 a₁
            // 最优 k 最小化 L² norm: k = (m·a₀ - s²·a₁) / (m² + s²)
            for (int iter = 0; iter < 3; ++iter) {
                double s = PolynomialOptimizer::estimate_skewness(f_t);
                if (s < 1.0) s = 1.0;
                double s_sq = s * s;
                double a0 = f_t[0].to_double();
                double a1 = f_t[1].to_double();
                double m_d = m_t.to_double();

                double denom = m_d * m_d + s_sq;
                if (denom < 1.0) break;

                double k_d = (m_d * a0 - s_sq * a1) / denom;
                int64_t k = static_cast<int64_t>(std::round(k_d));
                if (k == 0) break;  // 已在最优点

                // 限制 k 的幅度防止极端旋转
                constexpr int64_t K_MAX = 10000;
                if (k > K_MAX) k = K_MAX;
                if (k < -K_MAX) k = -K_MAX;

                f_t = PolynomialOptimizer::rotate_linear(f_t, m_t, k);
            }

            // L² norm 预筛 — 维护 top-K
            double s = PolynomialOptimizer::estimate_skewness(f_t);
            double norm = PolynomialOptimizer::compute_size(f_t, s);

            // 插入并保持按 norm 升序的 top-K 列表
            if (top_k.size() < TOP_K ||
                norm < top_k.back().norm) {
                top_k.push_back({std::move(f_t), std::move(m_t), norm});
                std::sort(top_k.begin(), top_k.end(),
                          [](const Candidate& a, const Candidate& b) {
                              return a.norm < b.norm;
                          });
                if (top_k.size() > TOP_K) top_k.pop_back();
            }
        }

        if (top_k.empty()) return std::nullopt;

        // Murphy E 二级评估: 对 top-K L² 候选跑完整 Murphy,挑 log_e_score 最高的。
        // 与之前的差别: 之前 K=1, 这里 K=3。Murphy 单次成本 ~2 次 compute_alpha
        // (v12 已去重),top-K=3 总成本约旧 K=1 的 3x,但能发现 α 优势候选。
        IntPolynomial best_f;
        Integer best_m;
        MurphyScore best_score;
        double best_log_e = -1e300;

        for (auto& cand : top_k) {
            if (!is_valid_polynomial(cand.f, n, cand.m)) continue;

            // 构造 g(x) = x - m (negate copy of m)
            Integer neg_m = cand.m;
            neg_m.negate();
            std::vector<Integer> g_coeffs;
            g_coeffs.reserve(2);
            g_coeffs.push_back(std::move(neg_m));
            g_coeffs.emplace_back(static_cast<int64_t>(1));
            IntPolynomial g_cand(std::move(g_coeffs));

            MurphyScore score = evaluator.compute(cand.f, g_cand, n);

            if (score.log_e_score > best_log_e) {
                best_log_e = score.log_e_score;
                best_f = cand.f.clone();  // best_f keeps copy semantics for IntPolynomial
                best_m = cand.m;          // mpz_set into existing best_m buffer
                best_score = score;
            }
        }

        if (best_log_e <= -1e299) return std::nullopt;

        // 构造最终 g(x) = x - m
        Integer neg_m = best_m.clone();
        neg_m.negate();
        std::vector<Integer> g_coeffs;
        g_coeffs.reserve(2);
        g_coeffs.push_back(std::move(neg_m));
        g_coeffs.emplace_back(static_cast<int64_t>(1));
        IntPolynomial g(std::move(g_coeffs));

        KleinjungResult result;
        result.f = std::move(best_f);
        result.g = std::move(g);
        result.m = std::move(best_m);
        result.skewness = best_score.skewness;
        result.score = best_score;
        result.success = true;

        return result;
    }

    /// 构造多项式
    /// 给定 n, a_d, m，构造 f(x) 使得 f(m) ≡ 0 (mod n)
    [[nodiscard]] std::optional<IntPolynomial> construct_polynomial(
            const Integer& n,
            const Integer& ad,
            const Integer& m,
            uint32_t d) {

        auto coeffs_opt = base_m_expansion(n, m, d, ad);
        if (!coeffs_opt) return std::nullopt;

        IntPolynomial f(std::move(*coeffs_opt));
        f.normalize();

        return f;
    }

    /// Base-m 展开（指定领导系数）。
    /// 返回 nullopt 当展开后仍有非零余数 —— 表示 a_d 选择让 n - a_d·m^d 无法
    /// 在 [-m/2, m/2]^d 内表示;静默把 carry 注入 a_{d-1} 会让 a_{d-1} 远超 m
    /// (违反 Stage 1 的 |a_{d-1}| ≤ m 选择标准),Stage 1 应改选下一个 m。
    [[nodiscard]] std::optional<std::vector<Integer>> base_m_expansion(
            const Integer& n,
            const Integer& m,
            uint32_t d,
            const Integer& ad) {

        std::vector<Integer> coeffs(d + 1);

        // 计算 n' = n - a_d * m^d
        Integer m_pow_d = core::pow(m, d);
        Integer n_prime = n.clone();
        Integer ad_md = ad.clone();
        ad_md *= m_pow_d;
        n_prime -= ad_md;

        // 对 n' 进行平衡 base-m 展开
        // 标准展开产生 [0, m) 系数；平衡展开 centering 到 [-m/2, m/2]
        Integer remainder = std::move(n_prime);
        Integer half_m = m.clone();
        half_m /= int64_t(2);

        for (uint32_t i = 0; i < d; ++i) {
            Integer quotient;
            Integer::divmod(quotient, coeffs[i], remainder, m);

            // Center: if coeff > m/2, subtract m and carry +1
            if (coeffs[i] > half_m) {
                coeffs[i] -= m;
                quotient += int64_t(1);  // mpz_add_ui direct
            }

            remainder = std::move(quotient);
        }

        // 最高次系数
        coeffs[d] = ad.clone();

        // Reject the (a_d, m) pair if expansion didn't terminate cleanly.
        // The historical fix silently merged remainder into coeffs[d-1] which
        // can produce a_{d-1} with magnitude many times larger than m.
        if (!remainder.is_zero()) {
            return std::nullopt;
        }

        return coeffs;
    }

    /// 验证多项式是否有效
    [[nodiscard]] bool is_valid_polynomial(
            const IntPolynomial& f,
            const Integer& n,
            const Integer& m) {

        // 检查度数
        if (f.degree() != params_.degree) {
            return false;
        }

        // 检查领导系数非零
        if (f.leading_coeff().is_zero()) {
            return false;
        }

        // 检查 f(m) ≡ 0 (mod n)（使用 Integer 精确验证）
        Integer fm = f.evaluate(m);
        Integer remainder;
        Integer quotient;
        Integer::divmod(quotient, remainder, fm, n);

        return remainder.is_zero();
    }

    /// 生成光滑系数
    /// 只包含小素因子的正整数
    [[nodiscard]] std::vector<Integer> generate_smooth_coefficients(uint64_t bound) {
        // 小素数列表
        std::vector<uint32_t> small_primes = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31};

        std::vector<Integer> result;
        result.emplace_back(1);

        for (uint32_t p : small_primes) {
            size_t current_size = result.size();
            for (size_t i = 0; i < current_size; ++i) {
                Integer val = result[i];  // copy ctor
                val *= p;
                while (val.fits_uint64() && val.to_uint64() <= bound) {
                    result.emplace_back(val);  // Integer copy ctor
                    val *= p;
                }
            }
        }

        // 排序 + 去重
        std::sort(result.begin(), result.end(),
            [](const Integer& a, const Integer& b) {
                return a < b;
            });
        result.erase(std::unique(result.begin(), result.end(),
            [](const Integer& a, const Integer& b) {
                return a == b;
            }), result.end());

        return result;
    }
};

/// 从 Kleinjung 结果创建 PolynomialContext
[[nodiscard]] inline PolynomialContext create_context_from_kleinjung(
        const Integer& n, const KleinjungResult& result) {
    std::vector<Integer> coeffs;
    coeffs.reserve(result.f.degree() + 1);
    for (uint32_t i = 0; i <= result.f.degree(); ++i) {
        coeffs.emplace_back(result.f[i]);  // Integer copy ctor
    }
    return PolynomialContext(
        n,  // copy ctor via op=
        std::move(coeffs),
        result.m.clone(),
        result.skewness
    );
}

} // namespace gnfs::polynomial
