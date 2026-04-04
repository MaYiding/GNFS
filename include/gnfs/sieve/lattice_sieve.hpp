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
    uint8_t log_scale = core::SIEVE_LOG_SCALE;  // log 值缩放因子
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
    /// 使用 row-major bucket sieve：预计算所有 FB 素数的格参数，
    /// 然后逐行处理（每行在 L1 cache 中热驻留，所有素数贡献完成后才移到下一行）。
    /// 相比旧的 per-prime 遍历，L1 miss 从 O(FB_size × j_height) 降到 O(j_height)。
    [[nodiscard]] SieveResult sieve_special_q(const SpecialQ& sq) {
        SieveResult result;
        result.special_q = sq;

        // 1. 计算格基
        LatticeBasis basis = compute_lattice_basis(sq);

        // 2. 初始化筛数组（memset(0)，加法筛）
        init_sieve_array(basis);

        // 3. 预计算所有 FB 素数的格参数
        auto primes = build_prime_entries(basis, sq);

        // 4. Row-major 筛法：逐行处理，每行在 L1 中热驻留（多线程并行）
        sieve_row_major(primes);

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
        if (alg_val < 1.0) alg_val = 1.0;
        double alg_log = std::log2(alg_val) * params_.log_scale;

        // 返回合并值（final guard against NaN/Inf from edge cases）
        double combined = rat_log + alg_log;
        if (!std::isfinite(combined) || combined < 0.0) return 0;
        return static_cast<uint16_t>(std::min(combined,
                                               static_cast<double>(UINT16_MAX)));
    }

    // ── Row-major bucket sieve 数据结构 ──────────────────────

    /// 预计算的 FB 素数条目
    /// 存储格坐标映射参数，避免 per-row 重复计算 mod_inverse
    struct PrimeEntry {
        uint32_t p;        // 素数
        uint16_t log_p;    // log 贡献
        uint16_t flags;    // 0=normal, 1=u_zero (整行命中), 2=uv_zero (全局命中)
        uint64_t u_inv;    // mod_inverse(u, p)，仅 flags==0 时有效
        int64_t v;         // 格参数 v (mod p)
        // Carry-forward 预计算字段 (Phase 1.1 优化)
        int32_t delta;     // 行间增量 = (-v * u_inv) mod p
        int32_t i_mod_init; // j=j_min 时的 i_mod 初始值
        int32_t i_min_mod; // i_min mod p (预计算避免 per-row 除法)
    };

    // ── 格参数计算 ──────────────────────────────────────────

    /// 计算有理侧 (u, v) 参数
    /// u = (e0 - f0·m) mod p, v = (e1 - f1·m) mod p
    /// 格点 (i,j) 满足 p | (a - b·m) iff i·u + j·v ≡ 0 (mod p)
    [[nodiscard]] std::pair<int64_t, int64_t> compute_rational_uv(
            const LatticeBasis& basis, uint32_t p) const {
        uint64_t m_mod_p = 0;
        if (ctx_.m().fits_uint64()) {
            m_mod_p = ctx_.m().to_uint64() % p;
        } else {
            core::Integer p_int(static_cast<unsigned long long>(p));
            core::Integer m_mod;
            core::Integer::mod(m_mod, ctx_.m(), p_int);
            m_mod_p = m_mod.to_uint64();
        }

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
        return {u, v};
    }

    /// 计算代数侧 (u, v) 参数
    /// u = (e0 - f0·r) mod p, v = (e1 - f1·r) mod p
    /// 格点 (i,j) 满足 p | (a - b·r) iff i·u + j·v ≡ 0 (mod p)
    [[nodiscard]] std::pair<int64_t, int64_t> compute_algebraic_uv(
            const LatticeBasis& basis, uint32_t p, uint32_t r) const {
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
        int64_t u = (e0_mod - f0_mod * r64 % p64 + p64) % p64;
        int64_t v = (e1_mod - f1_mod * r64 % p64 + p64) % p64;
        return {u, v};
    }

    // ── 素数预计算 ──────────────────────────────────────────

    /// 为所有 FB 素数预计算格参数（mod_inverse + carry-forward delta）
    /// 每个 SQ 调用一次，结果供 sieve_row_major() 使用
    [[nodiscard]] std::vector<PrimeEntry> build_prime_entries(
            const LatticeBasis& basis, const SpecialQ& sq) const {

        std::vector<PrimeEntry> entries;
        entries.reserve(fb_.rational_count() + fb_.sieve_algebraic_count());

        const int32_t j_min = region_.j_min;
        const int32_t i_min = region_.i_min;

        auto make_entry = [j_min, i_min](int64_t u, int64_t v, uint32_t p, uint32_t log_p) -> PrimeEntry {
            PrimeEntry pe;
            pe.p = p;
            pe.log_p = static_cast<uint16_t>(log_p);
            pe.delta = 0;
            pe.i_mod_init = 0;
            pe.i_min_mod = 0;
            if (u == 0 && v == 0) {
                pe.flags = 2; pe.u_inv = 0; pe.v = 0;
            } else if (u == 0) {
                pe.flags = 1; pe.u_inv = 0; pe.v = v;
            } else {
                pe.flags = 0;
                pe.u_inv = mod_inverse(static_cast<uint64_t>(u), p);
                pe.v = v;

                // Carry-forward 预计算:
                // delta = (-v * u_inv) mod p — 每行 i_mod 的增量
                int64_t p64 = static_cast<int64_t>(p);
                int64_t neg_v = (-v % p64 + p64) % p64;
                int64_t d = (neg_v * static_cast<int64_t>(pe.u_inv)) % p64;
                pe.delta = static_cast<int32_t>(d);

                // j=j_min 时的 i_mod 初始值
                int64_t rhs = (-static_cast<int64_t>(j_min) * v) % p64;
                if (rhs < 0) rhs += p64;
                int64_t im = (rhs * static_cast<int64_t>(pe.u_inv)) % p64;
                pe.i_mod_init = static_cast<int32_t>(im);

                // i_min mod p (预计算)
                int64_t imin_mod = static_cast<int64_t>(i_min) % p64;
                if (imin_mod < 0) imin_mod += p64;
                pe.i_min_mod = static_cast<int32_t>(imin_mod);
            }
            return pe;
        };

        // 有理侧 FB
        for (const auto& rp : fb_.rational()) {
            auto [u, v] = compute_rational_uv(basis, rp.p);
            entries.push_back(make_entry(u, v, rp.p, rp.log_p));
        }

        // 代数侧 FB（仅筛选范围，跳过投影根和 SQ 本身）
        const auto& algebraics = fb_.algebraic();
        const size_t sieve_count = fb_.sieve_algebraic_count();
        for (size_t ai = 0; ai < sieve_count; ++ai) {
            const auto& ap = algebraics[ai];
            if (ap.is_projective()) continue;
            if (ap.p == sq.q && ap.r == sq.r) continue;
            auto [u, v] = compute_algebraic_uv(basis, ap.p, ap.r);
            entries.push_back(make_entry(u, v, ap.p, ap.log_p));
        }

        return entries;
    }

    // ── Row-major 筛法核心 ──────────────────────────────────

    /// Row-major 筛法：逐行处理所有素数贡献（carry-forward + 多线程）
    ///
    /// 关键优化 1: row-major 循环顺序确保每行在 L1 cache 中热驻留
    /// 关键优化 2: carry-forward 消除 per-row per-prime 的 2 次 idiv
    /// 关键优化 3: 行范围分块多线程并行（线性递推允许任意行跳转）
    void sieve_row_major(const std::vector<PrimeEntry>& primes) {
        const size_t w = static_cast<size_t>(region_.i_width());

        // Phase 0: 全局命中素数（u=0, v=0 → 每个位置都被整除，极罕见）
        for (const auto& pe : primes) {
            if (pe.flags == 2) {
                uint16_t lp = pe.log_p;
                for (size_t idx = 0; idx < sieve_array_.size(); ++idx)
                    sieve_array_[idx] += lp;
            }
        }

        // Phase 1: 多线程行处理
        const int32_t j_min = region_.j_min;
        const int32_t j_max = region_.j_max;
        const int32_t total_rows = j_max - j_min + 1;
        const int32_t i_min = region_.i_min;

        size_t num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 4;
        // 少于 500 行不值得开多线程
        if (total_rows < 500) num_threads = 1;

        if (num_threads <= 1) {
            // 单线程快路径
            sieve_row_chunk(primes, j_min, j_max, w, i_min, 0);
            return;
        }

        // 分块并行
        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        int32_t rows_per_thread = total_rows / static_cast<int32_t>(num_threads);
        int32_t remainder = total_rows % static_cast<int32_t>(num_threads);

        int32_t chunk_start = j_min;
        for (size_t t = 0; t < num_threads; ++t) {
            int32_t chunk_rows = rows_per_thread + (static_cast<int32_t>(t) < remainder ? 1 : 0);
            int32_t chunk_end = chunk_start + chunk_rows - 1;
            int32_t row_offset = chunk_start - j_min;

            threads.emplace_back(&LatticeSieve::sieve_row_chunk, this,
                                 std::cref(primes), chunk_start, chunk_end,
                                 w, i_min, row_offset);
            chunk_start = chunk_end + 1;
        }

        for (auto& t : threads) {
            t.join();
        }
    }

    /// 处理一段行范围 [j_start, j_end]
    /// row_offset = j_start - region_.j_min，用于跳转 carry-forward 状态
    void sieve_row_chunk(const std::vector<PrimeEntry>& primes,
                         int32_t j_start, int32_t j_end,
                         size_t w, int32_t i_min, int32_t row_offset) {

        // 计算本 chunk 的 carry-forward 起始状态
        // i_mod 在行 k 时 = (i_mod_init + k * delta) mod p
        std::vector<int32_t> cur_i_mod(primes.size());
        for (size_t pi = 0; pi < primes.size(); ++pi) {
            const auto& pe = primes[pi];
            if (pe.flags != 0) continue;
            int32_t p32 = static_cast<int32_t>(pe.p);
            // 跳转 row_offset 行: i_mod = (init + offset * delta) mod p
            int64_t advanced = static_cast<int64_t>(pe.i_mod_init) +
                               static_cast<int64_t>(row_offset) * static_cast<int64_t>(pe.delta);
            int64_t mod = advanced % static_cast<int64_t>(p32);
            if (mod < 0) mod += p32;
            cur_i_mod[pi] = static_cast<int32_t>(mod);
        }

        for (int32_t j = j_start; j <= j_end; ++j) {
            size_t row_base = static_cast<size_t>(j - region_.j_min) * w;
            size_t row_end = row_base + w;

            for (size_t pi = 0; pi < primes.size(); ++pi) {
                const auto& pe = primes[pi];
                if (pe.flags == 2) continue;

                if (pe.flags == 1) {
                    if ((j % static_cast<int32_t>(pe.p)) == 0) {
                        uint16_t lp = pe.log_p;
                        for (size_t idx = row_base; idx < row_end; ++idx)
                            sieve_array_[idx] += lp;
                    }
                    continue;
                }

                int32_t i_mod = cur_i_mod[pi];
                int32_t p32 = static_cast<int32_t>(pe.p);

                int32_t offset = i_mod - pe.i_min_mod;
                if (offset < 0) offset += p32;
                int32_t i_start = i_min + offset;

                size_t idx = row_base + static_cast<size_t>(i_start - i_min);
                uint16_t lp = pe.log_p;
                size_t stride = static_cast<size_t>(pe.p);
                for (; idx < row_end; idx += stride) {
                    sieve_array_[idx] += lp;
                }

                i_mod += pe.delta;
                if (i_mod >= p32) i_mod -= p32;
                cur_i_mod[pi] = i_mod;
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
