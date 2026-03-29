#pragma once

#include "../core/polynomial_context.hpp"
#include "../core/relation.hpp"
#include "../core/types.hpp"
#include "../factor_base/factor_base.hpp"
#include "../util/safe_math.hpp"
#include "lattice_basis.hpp"
#include "special_q.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

namespace gnfs {
namespace sieve {

using core::ABPair;
using core::PolynomialContext;
using core::Relation;
using factor_base::FactorBase;

/// 筛法参数
struct SieveParams {
    uint8_t log_scale = 16;                    // log 值缩放因子
    uint8_t rational_threshold = 70;           // 有理侧阈值
    uint8_t algebraic_threshold = 70;          // 代数侧阈值
    uint32_t large_prime_bound = 0;            // 大素数上界（0 = 使用因子基设置）
    bool enable_2lp = true;                    // 启用 2LP (two large primes)
    bool enable_3lp = false;                   // 启用 3LP (three large primes)

    /// 计算合并阈值（返回 uint16_t 以避免两个 uint8_t 相加溢出）
    [[nodiscard]] uint16_t combined_threshold() const noexcept {
        return static_cast<uint16_t>(rational_threshold) + algebraic_threshold;
    }
};

/// 筛候选点
struct SieveCandidate {
    int32_t i;              // 格坐标 i
    int32_t j;              // 格坐标 j
    int64_t a;              // 原始坐标 a
    uint64_t b;             // 原始坐标 b
    uint8_t residual;       // 残余 log 值
};

/// 单个 special-q 的筛结果
struct SieveResult {
    SpecialQ special_q;                           // special-q
    std::vector<SieveCandidate> candidates;       // 候选点
    size_t sieved_positions = 0;                  // 筛过的位置数
    size_t smooth_count = 0;                      // 光滑数数量
};

/// 回调类型定义
using RelationCallback = std::function<void(Relation&&)>;
using ProgressCallback = std::function<void(size_t, size_t, const char*)>;

/// LatticeSieve - 格筛法主类
class LatticeSieve {
public:
    /// 构造函数
    /// @param ctx 多项式上下文
    /// @param fb 因子基
    /// @param params 筛法参数
    LatticeSieve(const PolynomialContext& ctx,
                 const FactorBase& fb,
                 const SieveParams& params = SieveParams{})
        : ctx_(ctx)
        , fb_(fb)
        , params_(params)
        , region_(default_sieve_region(ctx.skewness())) {

        // 初始化筛数组
        sieve_array_.resize(region_.size(), 0);
    }

    /// 设置筛区域
    void set_region(const SieveRegion& region) {
        region_ = region;
        sieve_array_.resize(region_.size(), 0);
    }

    /// 设置关系回调
    void set_relation_callback(RelationCallback callback) {
        relation_callback_ = std::move(callback);
    }

    /// 设置进度回调
    void set_progress_callback(ProgressCallback callback) {
        progress_callback_ = std::move(callback);
    }

    /// 对单个 special-q 进行筛法
    [[nodiscard]] SieveResult sieve_special_q(const SpecialQ& sq) {
        SieveResult result;
        result.special_q = sq;

        // 1. 计算格基
        LatticeBasis basis = compute_lattice_basis(sq);

        // 2. 初始化筛数组
        init_sieve_array(basis);

        // 3. 有理侧筛
        sieve_rational_side(basis);

        // 4. 代数侧筛
        sieve_algebraic_side(basis, sq);

        // 5. 收集候选点
        result.candidates = collect_candidates(basis);
        result.sieved_positions = region_.size();

        return result;
    }

    /// 并行处理多个 special-q
    /// @param special_qs 要处理的 special-q 列表
    /// @param num_threads 线程数 (0 = auto)
    /// @return 所有 special-q 的合并结果
    [[nodiscard]] std::vector<SieveResult> sieve_parallel(
            const std::vector<SpecialQ>& special_qs,
            size_t num_threads = 0) {

        if (num_threads == 0) {
            num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0) num_threads = 4;
        }

        std::vector<SieveResult> all_results(special_qs.size());
        std::mutex results_mutex;
        std::atomic<size_t> next_sq{0};

        // Worker function - each thread gets its own LatticeSieve copy
        auto worker = [&]() {
            // Each thread needs its own sieve array
            LatticeSieve local_sieve(ctx_, fb_, params_);
            local_sieve.set_region(region_);

            while (true) {
                size_t idx = next_sq.fetch_add(1, std::memory_order_relaxed);
                if (idx >= special_qs.size()) break;

                auto result = local_sieve.sieve_special_q(special_qs[idx]);

                std::lock_guard<std::mutex> lock(results_mutex);
                all_results[idx] = std::move(result);
            }
        };

        // Launch threads
        std::vector<std::thread> threads;
        for (size_t t = 0; t < num_threads; ++t) {
            threads.emplace_back(worker);
        }
        for (auto& t : threads) {
            t.join();
        }

        return all_results;
    }

private:
    const PolynomialContext& ctx_;
    const FactorBase& fb_;
    SieveParams params_;
    SieveRegion region_;

    std::vector<uint16_t> sieve_array_;  // 使用 uint16_t 以支持更大的 log 值

    RelationCallback relation_callback_;
    ProgressCallback progress_callback_;

    /// 初始化筛数组
    /// 设置初始 log 值（基于 (a, b) 的大小估计）
    void init_sieve_array(const LatticeBasis& basis) {
        std::fill(sieve_array_.begin(), sieve_array_.end(), 0);

        // 计算每个位置的初始 log 值
        // 这是基于 |a + b*m| 和 |N(a, b)| 的估计
        // 简化版本：使用均匀初始值
        uint16_t init_val = estimate_initial_log(basis);

        std::fill(sieve_array_.begin(), sieve_array_.end(), init_val);
    }

    /// 估计初始 log 值
    [[nodiscard]] uint16_t estimate_initial_log(const LatticeBasis& basis) const {
        // 估计 (a, b) 在区域中的典型大小
        // |a| ~ |i * e0 + j * e1|, |b| ~ |i * f0 + j * f1|
        // E[|i|] ≈ range/4 for symmetric distribution on [i_min, i_max]
        // E[j]   ≈ midpoint for [j_min, j_max] (j > 0)
        double typical_i = std::max(1.0, (region_.i_max - region_.i_min) / 4.0);
        double typical_j = std::max(1.0, (region_.j_max + region_.j_min) / 2.0);

        double typical_a = std::abs(typical_i * basis.e0 + typical_j * basis.e1);
        double typical_b = std::abs(typical_i * basis.f0 + typical_j * basis.f1);

        // 有理侧 (GNFS convention): |a - b*m|
        double m_val = ctx_.m().to_double();
        double rat_val = std::abs(typical_a - typical_b * m_val);

        // Guard: log2(0) = -Inf, static_cast<uint16_t>(-Inf) is UB
        if (rat_val < 1.0) rat_val = 1.0;
        double rat_log = std::log2(rat_val) * params_.log_scale;

        // 代数侧: |N(a,b)| ~ |a|^d * some_factor
        uint32_t d = ctx_.degree();
        double alg_val = std::pow(std::max(typical_a, 1.0), d);  // clamp to avoid log2(0)
        double alg_log = std::log2(alg_val) * params_.log_scale;

        // 返回合并值（final guard against NaN/Inf from edge cases）
        double combined = rat_log + alg_log;
        if (!std::isfinite(combined) || combined < 0.0) return 0;
        return static_cast<uint16_t>(std::min(combined,
                                               static_cast<double>(UINT16_MAX)));
    }

    /// 有理侧筛
    void sieve_rational_side(const LatticeBasis& basis) {
        const auto& rationals = fb_.rational();

        for (const auto& rp : rationals) {
            uint32_t p = rp.p;
            uint32_t log_p = rp.log_p;

            // 找到格中被 p 整除的位置
            // 对于有理侧 (GNFS convention)，p | (a - b*m)
            // 需要找到格点 (i, j) 使得 (i*e0 + j*e1) - (i*f0 + j*f1)*m ≡ 0 (mod p)
            sieve_prime_rational(basis, p, log_p);
        }
    }

    /// 对单个有理侧素数进行筛
    void sieve_prime_rational(const LatticeBasis& basis, uint32_t p, uint32_t log_p) {
        // 计算 m mod p
        uint64_t m_mod_p = 0;
        if (ctx_.m().fits_uint64()) {
            m_mod_p = ctx_.m().to_uint64() % p;
        } else {
            // 大数取模
            core::Integer p_int(static_cast<unsigned long long>(p));
            core::Integer m_mod;
            core::Integer::mod(m_mod, ctx_.m(), p_int);
            m_mod_p = m_mod.to_uint64();
        }

        // 对于格中的点 (a, b) = (i*e0 + j*e1, i*f0 + j*f1)
        // 有理侧值 (GNFS) = a - b*m = i*(e0 - f0*m) + j*(e1 - f1*m)
        // 设 u = (e0 - f0*m) mod p, v = (e1 - f1*m) mod p
        // 需要找 (i, j) 使得 i*u + j*v ≡ 0 (mod p)
        //
        // NOTE: 先对 p 取模再做乘法，防止 f0*m_mod_p 溢出 int64
        int64_t p64 = static_cast<int64_t>(p);
        auto mod_reduce = [p64](int64_t val) -> int64_t {
            int64_t r = val % p64;
            return r < 0 ? r + p64 : r;
        };
        int64_t e0_mod = mod_reduce(basis.e0);
        int64_t f0_mod = mod_reduce(basis.f0);
        int64_t e1_mod = mod_reduce(basis.e1);
        int64_t f1_mod = mod_reduce(basis.f1);
        int64_t m64 = static_cast<int64_t>(m_mod_p);
        // f_mod * m64 fits in int64: both < p < 2^32
        int64_t u = (e0_mod - f0_mod * m64 % p64 + p64) % p64;
        int64_t v = (e1_mod - f1_mod * m64 % p64 + p64) % p64;

        if (u < 0) u += p;
        if (v < 0) v += p;

        // 简化处理：遍历 j，计算对应的 i
        // i*u ≡ -j*v (mod p)
        // 如果 u ≠ 0, i ≡ -j*v*u^{-1} (mod p)
        if (u == 0 && v == 0) {
            // 所有点都被整除
            for (size_t idx = 0; idx < sieve_array_.size(); ++idx) {
                if (sieve_array_[idx] >= log_p) {
                    sieve_array_[idx] -= log_p;
                } else {
                    sieve_array_[idx] = 0;
                }
            }
            return;
        }

        if (u == 0) {
            // 只有 j ≡ 0 (mod p) 的行被整除
            for (int32_t j = region_.j_min; j <= region_.j_max; ++j) {
                if ((j % static_cast<int32_t>(p)) == 0) {
                    for (int32_t i = region_.i_min; i <= region_.i_max; ++i) {
                        size_t idx = region_.ij_to_index(i, j);
                        if (sieve_array_[idx] >= log_p) {
                            sieve_array_[idx] -= log_p;
                        }
                    }
                }
            }
            return;
        }

        // 计算 u 的模逆
        uint64_t u_inv = mod_inverse(static_cast<uint64_t>(u), p);

        // 对每个 j，计算 i_start，然后每隔 p 个位置筛一次
        for (int32_t j = region_.j_min; j <= region_.j_max; ++j) {
            // i ≡ -j*v*u^{-1} (mod p)
            int64_t rhs = (-static_cast<int64_t>(j) * v) % static_cast<int64_t>(p);
            if (rhs < 0) rhs += p;
            int64_t i_mod = (rhs * static_cast<int64_t>(u_inv)) % static_cast<int64_t>(p);

            // 找到范围内的第一个 i
            int32_t i_start = region_.i_min + static_cast<int32_t>(
                (static_cast<int64_t>(i_mod) - region_.i_min % static_cast<int64_t>(p) + p) % p);

            for (int32_t i = i_start; i <= region_.i_max; i += static_cast<int32_t>(p)) {
                size_t idx = region_.ij_to_index(i, j);
                if (idx < sieve_array_.size() && sieve_array_[idx] >= log_p) {
                    sieve_array_[idx] -= log_p;
                }
            }
        }
    }

    /// 代数侧筛
    /// 只使用筛选范围内的代数素数（≤ algebraic_bound），
    /// 跳过 special-Q 范围的素数（它们只供 SpecialQGenerator 使用）
    void sieve_algebraic_side(const LatticeBasis& basis, const SpecialQ& sq) {
        const auto& algebraics = fb_.algebraic();
        const size_t sieve_count = fb_.sieve_algebraic_count();

        for (size_t idx = 0; idx < sieve_count; ++idx) {
            const auto& ap = algebraics[idx];
            // 跳过 projective roots（r = UINT32_MAX 不是有效的模根）
            if (ap.is_projective()) {
                continue;
            }
            // 跳过 special-q 本身
            if (ap.p == sq.q && ap.r == sq.r) {
                continue;
            }

            sieve_prime_algebraic(basis, ap.p, ap.r, ap.log_p);
        }
    }

    /// 对单个代数侧素理想进行筛
    void sieve_prime_algebraic(const LatticeBasis& basis, uint32_t p, uint32_t r, uint32_t log_p) {
        // 对于代数侧 (GNFS convention)，p | N(a - bα) 当且仅当 p | (a - b*r)
        // Prime ideal P = (p, α - r) divides (a - bα) iff a - b*r ≡ 0 (mod p)
        //
        // NOTE: 先对 p 取模再做乘法，防止 f0*r 溢出 int64
        int64_t p64 = static_cast<int64_t>(p);
        auto mod_reduce = [p64](int64_t val) -> int64_t {
            int64_t rem = val % p64;
            return rem < 0 ? rem + p64 : rem;
        };
        int64_t e0_mod = mod_reduce(basis.e0);
        int64_t f0_mod = mod_reduce(basis.f0);
        int64_t e1_mod = mod_reduce(basis.e1);
        int64_t f1_mod = mod_reduce(basis.f1);
        int64_t r64 = static_cast<int64_t>(r);
        // f_mod * r64 fits in int64: both < p < 2^32
        int64_t u = (e0_mod - f0_mod * r64 % p64 + p64) % p64;
        int64_t v = (e1_mod - f1_mod * r64 % p64 + p64) % p64;

        if (u == 0 && v == 0) {
            for (size_t idx = 0; idx < sieve_array_.size(); ++idx) {
                if (sieve_array_[idx] >= log_p) {
                    sieve_array_[idx] -= log_p;
                }
            }
            return;
        }

        if (u == 0) {
            for (int32_t j = region_.j_min; j <= region_.j_max; ++j) {
                if ((j % static_cast<int32_t>(p)) == 0) {
                    for (int32_t i = region_.i_min; i <= region_.i_max; ++i) {
                        size_t idx = region_.ij_to_index(i, j);
                        if (sieve_array_[idx] >= log_p) {
                            sieve_array_[idx] -= log_p;
                        }
                    }
                }
            }
            return;
        }

        uint64_t u_inv = mod_inverse(static_cast<uint64_t>(u), p);

        for (int32_t j = region_.j_min; j <= region_.j_max; ++j) {
            int64_t rhs = (-static_cast<int64_t>(j) * v) % static_cast<int64_t>(p);
            if (rhs < 0) rhs += p;
            int64_t i_mod = (rhs * static_cast<int64_t>(u_inv)) % static_cast<int64_t>(p);

            int32_t i_start = region_.i_min + static_cast<int32_t>(
                (static_cast<int64_t>(i_mod) - region_.i_min % static_cast<int64_t>(p) + p) % p);

            for (int32_t i = i_start; i <= region_.i_max; i += static_cast<int32_t>(p)) {
                size_t idx = region_.ij_to_index(i, j);
                if (idx < sieve_array_.size() && sieve_array_[idx] >= log_p) {
                    sieve_array_[idx] -= log_p;
                }
            }
        }
    }

    /// 收集候选点
    [[nodiscard]] std::vector<SieveCandidate> collect_candidates(const LatticeBasis& basis) const {
        std::vector<SieveCandidate> candidates;

        uint16_t threshold = params_.combined_threshold();

        for (size_t idx = 0; idx < sieve_array_.size(); ++idx) {
            if (sieve_array_[idx] <= threshold) {
                auto [i, j] = region_.index_to_ij(idx);
                auto [a, b] = basis.to_ab(i, j);

                // 只保留 b > 0 的情况
                if (b <= 0) continue;

                // 确保 gcd(a, b) = 1
                if (std::gcd(util::safe_abs(a), b) != 1) continue;

                SieveCandidate cand;
                cand.i = i;
                cand.j = j;
                cand.a = a;
                cand.b = static_cast<uint64_t>(b);
                cand.residual = static_cast<uint8_t>(sieve_array_[idx]);

                candidates.push_back(cand);
            }
        }

        return candidates;
    }

    /// 计算模逆 (a^{-1} mod m)
    [[nodiscard]] static uint64_t mod_inverse(uint64_t a, uint64_t m) {
        // 扩展欧几里得算法
        int64_t t = 0, newt = 1;
        int64_t r = static_cast<int64_t>(m), newr = static_cast<int64_t>(a);

        while (newr != 0) {
            int64_t quotient = r / newr;
            t -= quotient * newt;
            std::swap(t, newt);
            r -= quotient * newr;
            std::swap(r, newr);
        }

        if (t < 0) t += static_cast<int64_t>(m);
        return static_cast<uint64_t>(t);
    }
};

} // namespace sieve
} // namespace gnfs
