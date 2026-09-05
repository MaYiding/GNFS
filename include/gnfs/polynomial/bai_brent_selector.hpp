#pragma once

#include "../core/integer.hpp"
#include "../core/params.hpp"
#include "../core/polynomial_context.hpp"
#include "../util/thread_pool.hpp"
#include "int_polynomial.hpp"
#include "kleinjung_selector.hpp" // BaiBrentResult = KleinjungResult, KleinjungProgressCallback
#include "murphy_evaluator.hpp"
#include "polynomial_optimizer.hpp"
#include "rotation_alpha.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace gnfs::polynomial {

using core::GNFSParams;
using core::Integer;
using core::PolynomialContext;
using util::ThreadPool;

/// Bai-Brent 非首一多项式选择参数 (Kleinjung 2008 + Bai 2011 thesis 扩展)
///
/// 与 Kleinjung 的关键差异:
///   - Kleinjung Stage 1: a_d 限于 smooth 数 (小素因子乘积 ≤ leading_coeff_bound)
///   - Bai-Brent  Stage 1: a_d 可以是 [ad_min, ad_max] 内任意整数, 仅要求 gcd(a_d, m) = 1
///                         + 可选 smooth 偏好 (smooth_preference=true 时优先 smooth, 但不限定)
///
/// 这扩大了 a_d 搜索空间, 允许 sqrt-skewness 更大的多项式, alpha 优化范围更广.
struct BaiBrentParams {
    uint32_t degree = 5;                // 多项式度数 (5 或 6)
    double skewness_min = 1e2;          // skewness 搜索下界 (比 Kleinjung 更宽)
    double skewness_max = 1e10;         // skewness 搜索上界
    uint64_t ad_min = 1;                // a_d 下界 (含)
    uint64_t ad_max = 10000;            // a_d 上界 (含)
    uint32_t num_candidates = 1000;     // Stage 1 候选数量
    uint32_t root_opt_iterations = 256; // 牛顿法迭代次数
    double root_opt_precision = 1e-6;   // 牛顿法收敛精度
    uint32_t search_radius = 100;       // m 搜索半径
    bool parallel = true;               // 启用并行
    uint32_t num_threads = 0;           // 线程数 (0 = 自动)
    bool smooth_preference = true;      // Stage 1 先枚举 smooth a_d, 再补非 smooth

    /// Murphy 评估参数
    MurphyParams murphy_params;

    /// 从 GNFSParams 自动推导 BaiBrent 参数 (与 Kleinjung 推导对齐)
    [[nodiscard]] static BaiBrentParams from_gnfs_params(const GNFSParams& gp) {
        BaiBrentParams bp;
        bp.degree = gp.degree;
        bp.ad_min = 1;
        bp.ad_max = gp.leading_coeff_bound;
        bp.search_radius =
            static_cast<uint32_t>(std::min(gp.search_radius, static_cast<uint64_t>(UINT32_MAX)));
        bp.num_candidates = gp.num_candidates;

        // Skewness 范围 — Bai-Brent 可探索更大 skewness
        if (gp.digits <= 40) {
            bp.skewness_min = 1e1;
            bp.skewness_max = 1e7;
        } else if (gp.digits <= 80) {
            bp.skewness_min = 1e2;
            bp.skewness_max = 1e9;
        } else {
            bp.skewness_min = 1e3;
            bp.skewness_max = 1e11;
        }

        bp.root_opt_iterations = (gp.digits <= 40) ? 128 : 256;

        bp.murphy_params.alpha_bound = std::min(static_cast<double>(gp.algebraic_bound), 1e6);
        bp.murphy_params.smoothness_bound = gp.algebraic_bound;

        double sp_cont = static_cast<double>(gp.digits) * 30.0 + 200.0;
        bp.murphy_params.sample_points = static_cast<uint32_t>(std::clamp(sp_cont, 100.0, 5000.0));
        bp.murphy_params.skewness_steps = gp.skewness_steps;

        return bp;
    }
};

/// Bai-Brent 结果 — 与 Kleinjung 结果同构, 复用类型方便 dispatch.
using BaiBrentResult = KleinjungResult;
using BaiBrentProgressCallback = KleinjungProgressCallback;

/// BaiBrentSelector — 非首一多项式选择 (Bai-Brent 风格)
///
/// 与 KleinjungSelector 同构, Stage 2 完全复用 (translation + closed-form rotation +
/// L²/cheap-alpha top-K + Murphy E re-ranking). Stage 1 改写为
/// "枚举 a_d ∈ [ad_min, ad_max], 要求 gcd(a_d, m) = 1, smooth 优先".
class BaiBrentSelector {
public:
    explicit BaiBrentSelector(const BaiBrentParams& params = BaiBrentParams{})
        : params_(params), cancelled_(false) {}

    BaiBrentSelector(BaiBrentSelector&& other) noexcept
        : params_(std::move(other.params_)),
          progress_callback_(std::move(other.progress_callback_)),
          cancelled_(other.cancelled_.load()) {}

    BaiBrentSelector& operator=(BaiBrentSelector&& other) noexcept {
        if (this != &other) {
            params_ = std::move(other.params_);
            progress_callback_ = std::move(other.progress_callback_);
            cancelled_.store(other.cancelled_.load());
        }
        return *this;
    }

    BaiBrentSelector(const BaiBrentSelector&) = delete;
    BaiBrentSelector& operator=(const BaiBrentSelector&) = delete;

    ~BaiBrentSelector() = default;

    void set_progress_callback(BaiBrentProgressCallback callback) {
        progress_callback_ = std::move(callback);
    }

    void cancel() noexcept {
        cancelled_.store(true);
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return cancelled_.load();
    }

    /// 选择最佳多项式 (主入口)
    [[nodiscard]] BaiBrentResult select(const Integer& n) {
        cancelled_.store(false);
        auto start_time = std::chrono::high_resolution_clock::now();

        BaiBrentResult result;
        result.success = false;

        if (n.is_zero() || n.is_negative() || params_.degree == 0) {
            return result;
        }

        report_progress(0, 1, 0.0, "BaiBrent Stage 1: Generating candidates");
        auto candidates = stage1_leading_coeff_search(n);

        if (candidates.empty() || is_cancelled()) {
            return result;
        }

        MurphyEvaluator evaluator(params_.murphy_params);
        std::optional<BaiBrentResult> best_result;
        double best_log_score = -1e100;
        std::mutex result_mutex;
        std::atomic<size_t> progress_count{0};

        auto process_candidate = [&](size_t idx) {
            if (is_cancelled())
                return;

            const auto& [ad, m_init] = candidates[idx];
            auto candidate_result = stage2_root_optimization(n, ad, m_init, evaluator);

            if (candidate_result.has_value()) {
                std::lock_guard<std::mutex> lock(result_mutex);
                if (candidate_result->score.log_e_score > best_log_score) {
                    best_log_score = candidate_result->score.log_e_score;
                    best_result = std::move(candidate_result);
                }
            }

            size_t current = progress_count.fetch_add(1, std::memory_order_relaxed) + 1;
            {
                std::lock_guard<std::mutex> lock(result_mutex);
                report_progress(current, candidates.size(), best_log_score,
                                "BaiBrent Stage 2: Optimizing");
            }
        };

        if (params_.parallel && candidates.size() > 1) {
            uint32_t num_threads =
                params_.num_threads > 0 ? params_.num_threads : std::thread::hardware_concurrency();

            ThreadPool pool(num_threads);
            pool.parallel_for_index(0, candidates.size(),
                                    [&](size_t idx) { process_candidate(idx); });
        } else {
            for (size_t idx = 0; idx < candidates.size(); ++idx) {
                process_candidate(idx);
                if (is_cancelled())
                    break;
            }
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(end_time - start_time).count();

        if (best_result.has_value()) {
            result = std::move(best_result.value());
            result.candidates_tested = progress_count.load(std::memory_order_relaxed);
            result.elapsed_seconds = elapsed;
        }

        return result;
    }

    [[nodiscard]] const BaiBrentParams& params() const noexcept {
        return params_;
    }

private:
    BaiBrentParams params_;
    BaiBrentProgressCallback progress_callback_;
    std::atomic<bool> cancelled_;

    void report_progress(size_t current, size_t total, double best_score, const char* stage) {
        if (progress_callback_) {
            progress_callback_(current, total, best_score, stage);
        }
    }

    /// Stage 1: a_d 枚举 + m 邻域搜索.
    /// Bai-Brent 关键: a_d 不再限于 smooth 数, 而是 [ad_min, ad_max] 全部整数,
    /// 仅要求 gcd(a_d, m) = 1 (Bai 2011 §2.3, 保证 (a, b·m) 与 (a, b·α) 在 NFS
    /// 关系格中 same Frobenius orbit, 不会破坏 sieve).
    /// smooth_preference=true 时, smooth a_d 先入候选池, 非 smooth 补齐到 num_candidates.
    [[nodiscard]] std::vector<std::pair<Integer, Integer>>
    stage1_leading_coeff_search(const Integer& n) {
        std::vector<std::pair<Integer, Integer>> candidates;
        const size_t requested_limit = static_cast<size_t>(params_.num_candidates);
        const size_t stage1_limit = requested_limit > (std::numeric_limits<size_t>::max)() / 2
                                        ? (std::numeric_limits<size_t>::max)()
                                        : requested_limit * 2;
        constexpr size_t STAGE1_RESERVE_CAP = 100000;
        candidates.reserve(std::min(stage1_limit, STAGE1_RESERVE_CAP));

        uint32_t d = params_.degree;

        auto ad_list = generate_ad_candidates(params_.ad_min, params_.ad_max);

        for (const auto& ad : ad_list) {
            if (is_cancelled())
                break;
            if (ad.is_zero())
                continue;

            Integer n_div_ad;
            Integer div_remainder;
            Integer::divmod(n_div_ad, div_remainder, n, ad);

            if (n_div_ad.is_zero())
                continue;

            Integer m_est;
            mpz_root(m_est.get_mpz(), n_div_ad.get_mpz(), d);

            Integer m;
            Integer remainder;
            const int64_t radius = static_cast<int64_t>(params_.search_radius);
            for (int64_t delta = -radius; delta <= radius; ++delta) {

                m = m_est;
                if (delta >= 0) {
                    m += delta;
                } else {
                    m -= (-delta);
                }

                if (m.is_zero() || m.is_negative())
                    continue;

                // Bai-Brent 核心约束: gcd(a_d, m) = 1
                Integer g = core::gcd(ad, m);
                if (!g.fits_uint64() || g.to_uint64() != 1)
                    continue;

                Integer m_pow_d = core::pow(m, d);
                remainder = n;
                mpz_submul(remainder.get_mpz(), ad.get_mpz(), m_pow_d.get_mpz());

                if (remainder.is_negative())
                    continue;

                Integer m_pow_d1 = core::pow(m, d - 1);
                Integer ad1;
                Integer ad1_remainder;
                Integer::divmod(ad1, ad1_remainder, remainder, m_pow_d1);

                double m_val = m.to_double();
                double ad1_val = std::abs(ad1.to_double());

                if (ad1_val <= m_val * 1.0) {
                    candidates.emplace_back(ad, m);
                    if (candidates.size() >= stage1_limit)
                        break;
                }
            }

            if (candidates.size() >= stage1_limit)
                break;
        }

        // 按 |a_d| 排序 (小优先 — coefficient quality 经验观察)
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return std::abs(a.first.to_double()) < std::abs(b.first.to_double());
        });

        if (candidates.size() > params_.num_candidates) {
            candidates.resize(params_.num_candidates);
        }

        return candidates;
    }

    /// 生成 a_d 候选: smooth 数优先 + 非 smooth 补齐.
    /// Bai-Brent 关键差异: 不限制 a_d 必须 smooth, 只是排序时 smooth 在前.
    [[nodiscard]] std::vector<Integer> generate_ad_candidates(uint64_t lo, uint64_t hi) const {

        std::vector<Integer> result;
        if (lo == 0)
            lo = 1;
        if (lo > hi)
            return result;

        const uint64_t span = hi - lo;
        const uint64_t count = span == UINT64_MAX ? UINT64_MAX : span + 1;
        const size_t cap = static_cast<size_t>(std::min<uint64_t>(count, 100000));
        result.reserve(cap);

        if (params_.smooth_preference) {
            // Step 1: smooth a_d 优先
            std::vector<uint32_t> small_primes = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31};
            std::vector<uint64_t> smooth;
            smooth.push_back(1);
            for (uint32_t p : small_primes) {
                size_t cur_size = smooth.size();
                for (size_t i = 0; i < cur_size; ++i) {
                    uint64_t v = smooth[i];
                    while (v <= hi / p) {
                        v *= p;
                        if (v >= lo && v <= hi)
                            smooth.push_back(v);
                    }
                }
            }
            std::sort(smooth.begin(), smooth.end());
            smooth.erase(std::unique(smooth.begin(), smooth.end()), smooth.end());

            for (uint64_t s : smooth) {
                if (s >= lo)
                    result.emplace_back(s);
                if (result.size() >= cap)
                    return result;
            }
        }

        // Step 2: 顺序枚举 [lo, hi] 补齐 (Bai-Brent 关键 — 任意 a_d)
        for (uint64_t v = lo;; ++v) {
            result.emplace_back(v);
            if (result.size() >= cap)
                break;
            if (v == hi)
                break;
        }

        // 去重 (smooth 已经在前面, 后续 enum 会有重复, 先稳定排序再 unique)
        // 但要保留原顺序优先级 — 改用 set-based dedup
        std::vector<Integer> dedup;
        dedup.reserve(result.size());
        std::vector<uint64_t> seen_uint;
        seen_uint.reserve(result.size());
        for (auto& v : result) {
            if (!v.fits_uint64())
                continue;
            uint64_t u = v.to_uint64();
            auto it = std::lower_bound(seen_uint.begin(), seen_uint.end(), u);
            if (it != seen_uint.end() && *it == u)
                continue;
            seen_uint.insert(it, u);
            dedup.emplace_back(std::move(v));
        }

        return dedup;
    }

    /// Stage 2: 与 Kleinjung 完全相同 (translation + rotation + top-K + Murphy E).
    /// 复用而不重写 — 算法逻辑相同, 仅 a_d 来源不同.
    [[nodiscard]] std::optional<BaiBrentResult>
    stage2_root_optimization(const Integer& n, const Integer& ad, const Integer& m_init,
                             const MurphyEvaluator& evaluator) {

        uint32_t d = params_.degree;

        auto f_init = construct_polynomial(n, ad, m_init, d);
        if (!f_init.has_value())
            return std::nullopt;
        if (!is_valid_polynomial(*f_init, n, m_init))
            return std::nullopt;

        struct Candidate {
            IntPolynomial f;
            Integer m;
            double norm;
            double rank_key;
        };
        constexpr size_t TOP_K = 3;
        std::vector<Candidate> top_k;
        top_k.reserve(TOP_K + 1);

        const RotationAlphaTracker tracker;
        const bool use_alpha_ranking = []() {
            const char* env = std::getenv("GNFS_TOPK_ALPHA");
            return env && env[0] == '1';
        }();

        const int t_range = static_cast<int>(std::min<uint64_t>(params_.search_radius, 50));
        for (int t = -t_range; t <= t_range; ++t) {
            IntPolynomial f_t;
            Integer m_t;

            if (t == 0) {
                f_t = f_init->clone();
                m_t = m_init;
            } else {
                f_t = PolynomialOptimizer::translate(*f_init, static_cast<int64_t>(t));
                m_t = m_init;
                m_t -= t;
                if (m_t.is_zero() || m_t.is_negative())
                    continue;
            }

            if (f_t.degree() != d)
                continue;

            for (int iter = 0; iter < 3; ++iter) {
                double s = PolynomialOptimizer::estimate_skewness(f_t);
                if (s < 1.0)
                    s = 1.0;
                double s_sq = s * s;
                double a0 = f_t[0].to_double();
                double a1 = f_t[1].to_double();
                double m_d = m_t.to_double();

                double denom = m_d * m_d + s_sq;
                if (denom < 1.0)
                    break;

                double k_d = (m_d * a0 - s_sq * a1) / denom;
                const double rounded_k = std::round(k_d);
                if (!std::isfinite(rounded_k))
                    break;

                constexpr int64_t K_MAX = 10000;
                constexpr double K_MAX_D = static_cast<double>(K_MAX);
                int64_t k;
                if (rounded_k >= K_MAX_D) {
                    k = K_MAX;
                } else if (rounded_k <= -K_MAX_D) {
                    k = -K_MAX;
                } else {
                    k = static_cast<int64_t>(rounded_k);
                }
                if (k == 0)
                    break;

                f_t = PolynomialOptimizer::rotate_linear(f_t, m_t, k);
            }

            double s = PolynomialOptimizer::estimate_skewness(f_t);
            double norm = PolynomialOptimizer::compute_size(f_t, s);

            const double rank_key = use_alpha_ranking ? tracker.score(f_t, norm) : norm;

            if (top_k.size() < TOP_K || rank_key < top_k.back().rank_key) {
                top_k.push_back({std::move(f_t), std::move(m_t), norm, rank_key});
                std::sort(top_k.begin(), top_k.end(), [](const Candidate& a, const Candidate& b) {
                    return a.rank_key < b.rank_key;
                });
                if (top_k.size() > TOP_K)
                    top_k.pop_back();
            }
        }

        if (top_k.empty())
            return std::nullopt;

        IntPolynomial best_f;
        Integer best_m;
        MurphyScore best_score;
        double best_log_e = -1e300;

        for (auto& cand : top_k) {
            if (!is_valid_polynomial(cand.f, n, cand.m))
                continue;

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
                best_f = cand.f.clone();
                best_m = cand.m;
                best_score = score;
            }
        }

        if (best_log_e <= -1e299)
            return std::nullopt;

        Integer neg_m = best_m;
        neg_m.negate();
        std::vector<Integer> g_coeffs;
        g_coeffs.reserve(2);
        g_coeffs.push_back(std::move(neg_m));
        g_coeffs.emplace_back(static_cast<int64_t>(1));
        IntPolynomial g(std::move(g_coeffs));

        BaiBrentResult result;
        result.f = std::move(best_f);
        result.g = std::move(g);
        result.m = std::move(best_m);
        result.skewness = best_score.skewness;
        result.score = best_score;
        result.success = true;

        return result;
    }

    [[nodiscard]] std::optional<IntPolynomial>
    construct_polynomial(const Integer& n, const Integer& ad, const Integer& m, uint32_t d) {

        auto coeffs_opt = base_m_expansion(n, m, d, ad);
        if (!coeffs_opt)
            return std::nullopt;

        IntPolynomial f(std::move(*coeffs_opt));
        f.normalize();
        return f;
    }

    [[nodiscard]] std::optional<std::vector<Integer>>
    base_m_expansion(const Integer& n, const Integer& m, uint32_t d, const Integer& ad) {

        std::vector<Integer> coeffs(static_cast<size_t>(d) + 1);

        Integer m_pow_d = core::pow(m, d);
        Integer n_prime = n;
        mpz_submul(n_prime.get_mpz(), ad.get_mpz(), m_pow_d.get_mpz());

        Integer remainder = std::move(n_prime);
        Integer half_m = m;
        half_m /= int64_t(2);

        for (uint32_t i = 0; i < d; ++i) {
            Integer quotient;
            Integer::divmod(quotient, coeffs[i], remainder, m);

            if (coeffs[i] > half_m) {
                coeffs[i] -= m;
                quotient += int64_t(1);
            }

            remainder = std::move(quotient);
        }

        coeffs[d] = ad;

        if (!remainder.is_zero()) {
            return std::nullopt;
        }

        return coeffs;
    }

    [[nodiscard]] bool is_valid_polynomial(const IntPolynomial& f, const Integer& n,
                                           const Integer& m) {

        if (f.degree() != params_.degree)
            return false;
        if (f.leading_coeff().is_zero())
            return false;

        Integer fm = f.evaluate(m);
        Integer remainder;
        Integer quotient;
        Integer::divmod(quotient, remainder, fm, n);
        return remainder.is_zero();
    }
};

/// 从 Bai-Brent 结果创建 PolynomialContext (与 Kleinjung 创建函数同构, 复用即可).
[[nodiscard]] inline PolynomialContext create_context_from_bai_brent(const Integer& n,
                                                                     const BaiBrentResult& result) {
    return create_context_from_kleinjung(n, result);
}

} // namespace gnfs::polynomial
