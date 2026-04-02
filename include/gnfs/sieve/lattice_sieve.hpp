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
#include <cstring>
#include <cstdint>
#include <functional>
#include <numeric>
#include <thread>
#include <vector>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace gnfs {
namespace sieve {

using core::ABPair;
using core::PolynomialContext;
using core::Relation;
using factor_base::FactorBase;

/// 筛法参数
struct SieveParams {
    uint8_t log_scale = 16;                    // log 值缩放因子
    uint16_t rational_threshold = 70;          // 有理侧阈值 (uint16_t: LP 模式需 >255)
    uint16_t algebraic_threshold = 70;         // 代数侧阈值 (uint16_t: LP 模式需 >255)
    uint32_t large_prime_bound = 0;            // 大素数上界（0 = 使用因子基设置）
    bool enable_2lp = true;                    // 启用 2LP (two large primes)
    bool enable_3lp = false;                   // 启用 3LP (three large primes)

    /// 计算合并阈值
    [[nodiscard]] uint16_t combined_threshold() const noexcept {
        uint32_t sum = static_cast<uint32_t>(rational_threshold) + algebraic_threshold;
        return static_cast<uint16_t>(std::min(sum, static_cast<uint32_t>(UINT16_MAX)));
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
        std::atomic<size_t> next_sq{0};

        // Worker function - each thread gets its own LatticeSieve copy
        // No mutex needed: each thread writes to a unique all_results[idx]
        auto worker = [&]() {
            LatticeSieve local_sieve(ctx_, fb_, params_);
            local_sieve.set_region(region_);

            while (true) {
                size_t idx = next_sq.fetch_add(1, std::memory_order_relaxed);
                if (idx >= special_qs.size()) break;

                all_results[idx] = local_sieve.sieve_special_q(special_qs[idx]);
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

    std::vector<uint16_t> sieve_array_;  // 加法筛：累积 log_p 值
    uint16_t last_init_val_ = 0;         // 当前 SQ 的初始 log 估计值

    RelationCallback relation_callback_;
    ProgressCallback progress_callback_;

    /// 初始化筛数组（加法筛：零填充）
    /// 加法筛中数组从 0 开始，每个 FB 素数命中时 += log_p。
    /// 候选检测时比较 accumulated >= (init_val - threshold)。
    /// memset(0) 比 std::fill(init_val) 快：OS 级零页优化 + 无分支写入。
    void init_sieve_array(const LatticeBasis& basis) {
        last_init_val_ = estimate_initial_log(basis);
        std::memset(sieve_array_.data(), 0, sieve_array_.size() * sizeof(uint16_t));
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

        sieve_stride(u, v, p, log_p);
    }

    /// 代数侧筛
    /// 只使用筛选范围内的代数素数（≤ algebraic_bound），
    /// 跳过 special-Q 范围的素数（它们只供 SpecialQGenerator 使用）
    void sieve_algebraic_side(const LatticeBasis& basis, const SpecialQ& sq) {
        const auto& algebraics = fb_.algebraic();
        const size_t sieve_count = fb_.sieve_algebraic_count();

        for (size_t ai = 0; ai < sieve_count; ++ai) {
            const auto& ap = algebraics[ai];
            if (ap.is_projective()) continue;
            if (ap.p == sq.q && ap.r == sq.r) continue;

            // 对于代数侧 (GNFS convention)，p | (a - b*r)
            int64_t p64 = static_cast<int64_t>(ap.p);
            auto mod_reduce = [p64](int64_t val) -> int64_t {
                int64_t rem = val % p64;
                return rem < 0 ? rem + p64 : rem;
            };
            int64_t e0_mod = mod_reduce(basis.e0);
            int64_t f0_mod = mod_reduce(basis.f0);
            int64_t e1_mod = mod_reduce(basis.e1);
            int64_t f1_mod = mod_reduce(basis.f1);
            int64_t r64 = static_cast<int64_t>(ap.r);
            int64_t u = (e0_mod - f0_mod * r64 % p64 + p64) % p64;
            int64_t v = (e1_mod - f1_mod * r64 % p64 + p64) % p64;

            sieve_stride(u, v, ap.p, ap.log_p);
        }
    }

    /// 公共筛法内循环：对给定 (u, v, p) 在筛区域中步进 += log_p
    /// u, v: 格坐标到素数整除性的线性映射系数 (mod p)
    ///   i*u + j*v ≡ 0 (mod p) 时该位置被 p 整除
    /// 使用直接索引步进避免 per-hit ij_to_index 调用
    void sieve_stride(int64_t u, int64_t v, uint32_t p, uint32_t log_p) {
        const size_t w = static_cast<size_t>(region_.i_width());
        const uint16_t lp = static_cast<uint16_t>(log_p);

        if (u == 0 && v == 0) {
            for (size_t idx = 0; idx < sieve_array_.size(); ++idx) {
                sieve_array_[idx] += lp;
            }
            return;
        }

        if (u == 0) {
            // 只有 j ≡ 0 (mod p) 的行被整除 → 整行 += log_p
            int32_t p32 = static_cast<int32_t>(p);
            for (int32_t j = region_.j_min; j <= region_.j_max; ++j) {
                if ((j % p32) == 0) {
                    size_t row = static_cast<size_t>(j - region_.j_min) * w;
                    for (size_t idx = row; idx < row + w; ++idx) {
                        sieve_array_[idx] += lp;
                    }
                }
            }
            return;
        }

        uint64_t u_inv = mod_inverse(static_cast<uint64_t>(u), p);
        const size_t stride = static_cast<size_t>(p);

        for (int32_t j = region_.j_min; j <= region_.j_max; ++j) {
            int64_t p64 = static_cast<int64_t>(p);
            int64_t rhs = (-static_cast<int64_t>(j) * v) % p64;
            if (rhs < 0) rhs += p;
            int64_t i_mod = (rhs * static_cast<int64_t>(u_inv)) % p64;

            int32_t i_start = region_.i_min + static_cast<int32_t>(
                (i_mod - region_.i_min % p64 + p) % p);

            // 直接索引步进：row_base + offset, stride = p
            size_t row_base = static_cast<size_t>(j - region_.j_min) * w;
            size_t row_end = row_base + w;
            size_t idx = row_base + static_cast<size_t>(i_start - region_.i_min);
            for (; idx < row_end; idx += stride) {
                sieve_array_[idx] += lp;
            }
        }
    }

    /// 收集候选点（加法筛 + NEON 向量化扫描）
    /// 加法筛中：candidate iff accumulated >= (init_val - threshold)
    /// 等价于旧减法筛中：residual <= threshold
    [[nodiscard]] std::vector<SieveCandidate> collect_candidates(const LatticeBasis& basis) const {
        std::vector<SieveCandidate> candidates;

        uint16_t threshold = params_.combined_threshold();
        // effective_threshold = init_val - threshold (clamped to 0)
        uint16_t eff_thresh = (last_init_val_ > threshold) ?
            static_cast<uint16_t>(last_init_val_ - threshold) : uint16_t(0);

        auto process_hit = [&](size_t idx) {
            auto [i, j] = region_.index_to_ij(idx);
            auto [a, b] = basis.to_ab(i, j);
            if (b <= 0) return;
            if (std::gcd(util::safe_abs(a), b) != 1) return;

            SieveCandidate cand;
            cand.i = i;
            cand.j = j;
            cand.a = a;
            cand.b = static_cast<uint64_t>(b);
            // residual = init_val - accumulated (backwards compat)
            uint16_t acc = sieve_array_[idx];
            cand.residual = static_cast<uint8_t>(
                std::min(static_cast<uint16_t>(acc <= last_init_val_ ?
                    last_init_val_ - acc : 0), uint16_t(255)));
            candidates.push_back(cand);
        };

        const size_t n = sieve_array_.size();

#ifdef __ARM_NEON
        // NEON: scan 8 × uint16 per iteration, quick-reject blocks with no hits
        const uint16x8_t thresh_vec = vdupq_n_u16(eff_thresh);
        const size_t vec_end = n & ~size_t(7);

        for (size_t idx = 0; idx < vec_end; idx += 8) {
            uint16x8_t vals = vld1q_u16(&sieve_array_[idx]);
            uint16x8_t cmp = vcgeq_u16(vals, thresh_vec);
            // Quick reject: all 128 bits zero → no candidates in this block
            uint64x2_t cmp64 = vreinterpretq_u64_u16(cmp);
            if ((vgetq_lane_u64(cmp64, 0) | vgetq_lane_u64(cmp64, 1)) == 0)
                continue;
            // At least one hit — check individual lanes
            for (size_t k = 0; k < 8; ++k) {
                if (sieve_array_[idx + k] >= eff_thresh)
                    process_hit(idx + k);
            }
        }
        // Scalar tail
        for (size_t idx = vec_end; idx < n; ++idx) {
            if (sieve_array_[idx] >= eff_thresh)
                process_hit(idx);
        }
#else
        // Scalar fallback
        for (size_t idx = 0; idx < n; ++idx) {
            if (sieve_array_[idx] >= eff_thresh)
                process_hit(idx);
        }
#endif

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
