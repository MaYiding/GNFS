#pragma once

#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"
#include "../util/thread_pool.hpp"
#include "int_polynomial.hpp"
#include "murphy_evaluator.hpp"
#include "polynomial_optimizer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace gnfs {
namespace polynomial {

using core::Integer;
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

        // Stage 2: 对每个候选进行根优化
        MurphyEvaluator evaluator(params_.murphy_params);
        std::optional<KleinjungResult> best_result;
        double best_log_score = -1e100;
        std::mutex result_mutex;

        auto process_candidate = [&](size_t idx) {
            if (is_cancelled()) return;

            const auto& [ad, m_init] = candidates[idx];
            auto candidate_result = stage2_root_optimization(n, ad, m_init, evaluator);

            if (candidate_result.has_value()) {
                std::lock_guard<std::mutex> lock(result_mutex);
                // 使用 log_e_score 比较以处理大数
                if (candidate_result->score.log_e_score > best_log_score) {
                    best_log_score = candidate_result->score.log_e_score;
                    best_result = std::move(candidate_result);
                }
            }

            // 报告进度
            {
                std::lock_guard<std::mutex> lock(result_mutex);
                report_progress(idx + 1, candidates.size(), best_log_score, "Stage 2: Optimizing");
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
            result.candidates_tested = candidates.size();
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
        std::vector<std::pair<Integer, Integer>> candidates;

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
            for (int32_t delta = -static_cast<int32_t>(params_.search_radius);
                 delta <= static_cast<int32_t>(params_.search_radius);
                 ++delta) {

                Integer m = m_est.clone();
                if (delta >= 0) {
                    m += delta;
                } else {
                    m -= (-delta);
                }

                if (m.is_zero() || m.is_negative()) continue;

                // 计算 n - a_d * m^d
                Integer m_pow_d = core::pow(m, d);
                Integer ad_md = ad.clone();
                ad_md *= m_pow_d;

                Integer remainder = n.clone();
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

                // 放宽条件：ad1 应该比 m 小或相近
                if (ad1_val <= m_val * 2.0) {
                    candidates.emplace_back(ad.clone(), m.clone());

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

    /// Stage 2: 根优化
    /// 使用牛顿法优化 m，构造完整多项式并评分
    [[nodiscard]] std::optional<KleinjungResult>
    stage2_root_optimization(
            const Integer& n,
            const Integer& ad,
            const Integer& m_init,
            const MurphyEvaluator& evaluator) {

        uint32_t d = params_.degree;

        // 构造初始多项式
        auto f_opt = construct_polynomial(n, ad, m_init, d);
        if (!f_opt.has_value()) {
            return std::nullopt;
        }

        IntPolynomial f = std::move(f_opt.value());
        Integer m = m_init.clone();

        // 牛顿法优化
        IntPolynomial df = f.derivative();
        auto m_refined = PolynomialOptimizer::newton_root(
            f, df, m, n,
            params_.root_opt_iterations,
            params_.root_opt_precision);

        if (m_refined.has_value()) {
            // 用优化后的 m 重新构造多项式
            auto f_new = construct_polynomial(n, ad, m_refined.value(), d);
            if (f_new.has_value()) {
                f = std::move(f_new.value());
                m = std::move(m_refined.value());
            }
        }

        // 验证多项式
        if (!is_valid_polynomial(f, n, m)) {
            return std::nullopt;
        }

        // 构造 g(x) = x - m
        std::vector<Integer> g_coeffs;
        Integer neg_m = m.clone();
        neg_m.negate();
        g_coeffs.push_back(std::move(neg_m));
        g_coeffs.push_back(Integer(static_cast<int64_t>(1)));
        IntPolynomial g(std::move(g_coeffs));

        // 计算 Murphy E-score
        MurphyScore score = evaluator.compute(f, g, n);

        // 构造结果
        KleinjungResult result;
        result.f = std::move(f);
        result.g = std::move(g);
        result.m = std::move(m);
        result.skewness = score.skewness;
        result.score = score;
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

        // n = a_d * m^d + a_{d-1} * m^{d-1} + ... + a_1 * m + a_0

        std::vector<Integer> coeffs(d + 1);
        coeffs[d] = ad.clone();

        // 计算 r = n - a_d * m^d
        Integer m_pow = core::pow(m, d);
        Integer remainder = n.clone();
        Integer ad_md = ad.clone();
        ad_md *= m_pow;
        remainder -= ad_md;

        // 逐次提取系数
        for (int i = static_cast<int>(d) - 1; i >= 0; --i) {
            if (i > 0) {
                m_pow = core::pow(m, static_cast<uint32_t>(i));
            } else {
                m_pow = Integer(static_cast<int64_t>(1));
            }

            Integer quotient;
            Integer rem;
            Integer::divmod(quotient, rem, remainder, m_pow);

            // 调整系数范围，使其尽可能小
            // 系数应该在 [-m/2, m/2] 范围内
            if (i > 0) {
                Integer half_m = m.clone();
                Integer two(2);
                half_m /= two;

                if (quotient > half_m) {
                    quotient -= m;
                    remainder += m_pow;
                    remainder *= m;
                } else if (quotient.is_negative()) {
                    Integer neg_half_m = half_m.clone();
                    neg_half_m.negate();
                    if (quotient < neg_half_m) {
                        quotient += m;
                        remainder -= m_pow;
                        remainder *= m;
                    }
                }
            }

            coeffs[i] = remainder.clone();

            // 更新 remainder
            Integer term = coeffs[i].clone();
            term *= m_pow;
            remainder -= term;

            // 重新计算 remainder
            if (i > 0) {
                Integer m_pow_prev = core::pow(m, static_cast<uint32_t>(i - 1));
                Integer::divmod(coeffs[i], remainder, remainder, m_pow_prev);
            }
        }

        // 使用 base-m 展开重新计算（更稳定）
        coeffs = base_m_expansion(n, m, d, ad);

        IntPolynomial f(std::move(coeffs));
        f.normalize();

        return f;
    }

    /// Base-m 展开（指定领导系数）
    [[nodiscard]] std::vector<Integer> base_m_expansion(
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
        half_m /= Integer(2);

        for (uint32_t i = 0; i < d; ++i) {
            Integer quotient;
            Integer::divmod(quotient, coeffs[i], remainder, m);

            // Center: if coeff > m/2, subtract m and carry +1
            if (coeffs[i] > half_m) {
                coeffs[i] -= m;
                quotient += Integer(static_cast<int64_t>(1));
            }

            remainder = std::move(quotient);
        }

        // 最高次系数
        coeffs[d] = ad.clone();

        // 如果有剩余，添加到次高次系数
        if (!remainder.is_zero()) {
            coeffs[d - 1] += remainder;
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
        result.push_back(Integer(static_cast<int64_t>(1)));

        for (uint32_t p : small_primes) {
            size_t current_size = result.size();
            for (size_t i = 0; i < current_size; ++i) {
                Integer val = result[i].clone();
                val *= p;
                while (val.fits_uint64() && val.to_uint64() <= bound) {
                    result.push_back(val.clone());
                    val *= p;
                }
            }
        }

        // 排序
        std::sort(result.begin(), result.end(),
            [](const Integer& a, const Integer& b) {
                return a < b;
            });

        return result;
    }
};

/// 从 Kleinjung 结果创建 PolynomialContext
[[nodiscard]] inline PolynomialContext create_context_from_kleinjung(
        const Integer& n, const KleinjungResult& result) {
    std::vector<Integer> coeffs;
    for (uint32_t i = 0; i <= result.f.degree(); ++i) {
        coeffs.push_back(result.f[i].clone());
    }
    return PolynomialContext(
        n.clone(),
        std::move(coeffs),
        result.m.clone(),
        result.skewness
    );
}

} // namespace polynomial
} // namespace gnfs
