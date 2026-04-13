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

namespace gnfs::sieve {

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

    /// 设置最大线程数（0 = auto, 1 = single-threaded）
    void set_max_threads(size_t n) { max_threads_ = n; }

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

        // 4. 筛法：大 FB 用 bucket region 模式，小 FB 用原始 row-major 模式
        if (primes.size() >= BUCKET_REGION_FB_THRESHOLD) {
            sieve_bucket_region(primes);
        } else {
            sieve_row_major(primes);
        }

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
    size_t max_threads_ = 0;             // 0 = auto (hardware_concurrency)

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

    // ── Bucket sieve 数据结构 ──────────────────────────────────

    /// Bucket entry: 大素数在某行的命中记录（4 bytes，紧凑 cache-friendly）
    struct BucketEntry {
        uint16_t offset;   // 行内偏移 (0..width-1)
        uint16_t log_p;    // log 贡献
    };

    /// 预计算的 FB 素数条目
    /// 存储格坐标映射参数，避免 per-row 重复计算 mod_inverse
    struct PrimeEntry {
        uint32_t p;        // 素数
        uint16_t log_p;    // log 贡献
        uint16_t flags;    // 0=normal, 1=u_zero (整行命中), 2=uv_zero (全局命中)
        uint64_t u_inv;    // mod_inverse(u, p)，仅 flags==0 时有效
        int64_t v;         // 格参数 v (mod p)
        // Carry-forward 预计算字段
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
        // Use __int128_t for intermediate product to avoid overflow when p ~ 2^32
        int64_t u = (e0_mod - static_cast<int64_t>(static_cast<__int128_t>(f0_mod) * m64 % p64) + p64) % p64;
        int64_t v = (e1_mod - static_cast<int64_t>(static_cast<__int128_t>(f1_mod) * m64 % p64) + p64) % p64;
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
        // Use __int128_t for intermediate product to avoid overflow when p ~ 2^32
        int64_t u = (e0_mod - static_cast<int64_t>(static_cast<__int128_t>(f0_mod) * r64 % p64) + p64) % p64;
        int64_t v = (e1_mod - static_cast<int64_t>(static_cast<__int128_t>(f1_mod) * r64 % p64) + p64) % p64;
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

    // ── Bucket Region Sieve ───────────────────────────────────
    // CADO-NFS-style: divide sieve area into L1-friendly regions,
    // scatter all prime hits in one pass, apply per-region.

    /// Bucket region entry: 4 bytes (offset within region + log_p)
    /// With 64K regions, offset fits uint16_t. log_p fits uint16_t (SIEVE_LOG_SCALE).
    struct BucketRegionEntry {
        uint16_t offset;   // Position within region (0..region_size-1)
        uint16_t log_p;    // Log contribution
    };

    /// Compact bucket entry: 3 bytes (offset:16 + log_p:8)
    /// Saves 25% memory vs BucketRegionEntry for 80+ digit where bucket arrays are huge.
    /// log_p stored as uint8_t (max 255 after >>8 from uint16_t — suitable for SIEVE_LOG_SCALE ≤ 256).
    struct CompactBucketEntry {
        uint16_t offset;   // Position within region (0..region_size-1)
        uint8_t log_p;     // Truncated log contribution (>> 8 if needed)
    };

    /// Log2 of bucket region size. 2^16 = 65536 positions × 2B = 128KB ≈ L1 cache.
    static constexpr uint32_t LOG_BUCKET_REGION = 16;
    static constexpr uint32_t BUCKET_REGION_SIZE = 1u << LOG_BUCKET_REGION;

    /// Minimum total FB size to use bucket region sieve (below this, row-major is fine)
    static constexpr size_t BUCKET_REGION_FB_THRESHOLD = 5000;

    /// Bucket region sieve: scatter-then-apply for all primes.
    /// Phase 1 (scatter): iterate all primes once, compute hit positions, scatter to region buckets.
    /// Phase 2 (apply): for each region, apply all accumulated hits.
    /// Complexity: O(total_hits) instead of O(height × small_primes).
    void sieve_bucket_region(const std::vector<PrimeEntry>& primes) {
        const size_t total_area = sieve_array_.size();
        const size_t w = static_cast<size_t>(region_.i_width());
        const int32_t height = region_.j_height();
        const int32_t j_min = region_.j_min;

        // Number of bucket regions
        const size_t num_regions = (total_area + BUCKET_REGION_SIZE - 1) / BUCKET_REGION_SIZE;

        // Phase 0: global hits (flags==2, extremely rare)
        for (const auto& pe : primes) {
            if (pe.flags == 2) {
                uint16_t lp = pe.log_p;
                for (size_t idx = 0; idx < total_area; ++idx)
                    sieve_array_[idx] += lp;
            }
        }

        // Phase 1: scatter all prime hits to bucket regions
        // For each prime, compute all hit positions and assign to the correct region.
        // Use flat arrays with per-region counts for CSR-like access.

        // First pass: count hits per region
        std::vector<uint32_t> region_counts(num_regions, 0);

        // Tiny primes (p < 256): very high hit density, scatter is expensive.
        // Medium primes (256 <= p < total_area): one pass scatter.
        // Large primes (p >= total_area): at most 1 hit total, direct compute.
        constexpr uint32_t TINY_THRESHOLD = 256;

        // Count hits for medium+large primes
        for (const auto& pe : primes) {
            if (pe.flags != 0) continue;

            if (pe.p < TINY_THRESHOLD) {
                // Tiny primes: estimate hits per region
                size_t hits_per_row = w / pe.p + 1;
                size_t total_hits = hits_per_row * static_cast<size_t>(height);
                for (size_t r = 0; r < num_regions; ++r) {
                    size_t region_start = r * BUCKET_REGION_SIZE;
                    size_t region_end = std::min(region_start + BUCKET_REGION_SIZE, total_area);
                    size_t region_rows = (region_end - region_start + w - 1) / w;
                    region_counts[r] += static_cast<uint32_t>(
                        std::min(hits_per_row * region_rows, region_end - region_start));
                }
                (void)total_hits;
            } else {
                // Medium and large primes: exact count via row iteration
                int32_t p32 = static_cast<int32_t>(pe.p);
                int32_t i_mod = pe.i_mod_init;
                int32_t imin_mod = pe.i_min_mod;

                for (int32_t j = j_min; j <= j_min + height - 1; ++j) {
                    int32_t offset = i_mod - imin_mod;
                    if (offset < 0) offset += p32;

                    size_t row_base = static_cast<size_t>(j - j_min) * w;

                    for (size_t pos = row_base + static_cast<size_t>(offset);
                         pos < row_base + w;
                         pos += static_cast<size_t>(pe.p)) {
                        size_t region_idx = pos >> LOG_BUCKET_REGION;
                        if (region_idx < num_regions) region_counts[region_idx]++;
                    }

                    // advance carry-forward
                    i_mod += pe.delta;
                    if (i_mod >= p32) i_mod -= p32;
                }
            }
        }

        // Build CSR offsets
        std::vector<uint32_t> region_offsets(num_regions + 1, 0);
        for (size_t r = 0; r < num_regions; ++r) {
            region_offsets[r + 1] = region_offsets[r] + region_counts[r];
        }
        uint32_t total_entries = region_offsets[num_regions];

        // Allocate flat bucket array
        std::vector<BucketRegionEntry> bucket_entries(total_entries);
        std::vector<uint32_t> write_pos(num_regions);
        std::copy(region_offsets.begin(), region_offsets.begin() + static_cast<ptrdiff_t>(num_regions),
                  write_pos.begin());

        // Second pass: scatter hits (medium + large primes only)
        // Parallel: each thread processes a chunk of primes into thread-local
        // per-region vectors, then merge into the flat bucket array.
        //
        // This avoids write contention on shared write_pos counters.

        // Collect medium+large prime indices for parallel dispatch
        std::vector<size_t> medium_prime_indices;
        for (size_t pi = 0; pi < primes.size(); ++pi) {
            if (primes[pi].flags == 0 && primes[pi].p >= TINY_THRESHOLD)
                medium_prime_indices.push_back(pi);
        }

        size_t scatter_threads = (max_threads_ > 0) ? max_threads_ : std::thread::hardware_concurrency();
        if (scatter_threads == 0) scatter_threads = 4;
        if (medium_prime_indices.size() < 100) scatter_threads = 1;

        if (scatter_threads <= 1) {
            // Sequential scatter (same as before)
            for (size_t pi : medium_prime_indices) {
                const auto& pe = primes[pi];
                int32_t p32 = static_cast<int32_t>(pe.p);
                int32_t i_mod = pe.i_mod_init;
                int32_t imin_mod = pe.i_min_mod;
                for (int32_t j = j_min; j <= j_min + height - 1; ++j) {
                    int32_t offset = i_mod - imin_mod;
                    if (offset < 0) offset += p32;
                    size_t row_base = static_cast<size_t>(j - j_min) * w;
                    for (size_t pos = row_base + static_cast<size_t>(offset);
                         pos < row_base + w;
                         pos += static_cast<size_t>(pe.p)) {
                        size_t region_idx = pos >> LOG_BUCKET_REGION;
                        if (region_idx < num_regions) {
                            uint32_t wp = write_pos[region_idx]++;
                            bucket_entries[wp] = {
                                static_cast<uint16_t>(pos & (BUCKET_REGION_SIZE - 1)),
                                pe.log_p
                            };
                        }
                    }
                    i_mod += pe.delta;
                    if (i_mod >= p32) i_mod -= p32;
                }
            }
        } else {
            // Parallel scatter: thread-local bucket vectors, then merge
            struct ThreadBuckets {
                std::vector<std::vector<BucketRegionEntry>> per_region;
            };
            std::vector<ThreadBuckets> thread_buckets(scatter_threads);
            for (auto& tb : thread_buckets) {
                tb.per_region.resize(num_regions);
            }

            size_t chunk = (medium_prime_indices.size() + scatter_threads - 1) / scatter_threads;
            std::vector<std::thread> scatter_workers;
            scatter_workers.reserve(scatter_threads);

            for (size_t t = 0; t < scatter_threads; ++t) {
                size_t start = t * chunk;
                size_t end_idx = std::min(start + chunk, medium_prime_indices.size());
                if (start >= medium_prime_indices.size()) break;

                scatter_workers.emplace_back([&, t, start, end_idx]() {
                    auto& local = thread_buckets[t].per_region;
                    for (size_t pi_idx = start; pi_idx < end_idx; ++pi_idx) {
                        const auto& pe = primes[medium_prime_indices[pi_idx]];
                        int32_t p32 = static_cast<int32_t>(pe.p);
                        int32_t i_mod = pe.i_mod_init;
                        int32_t imin_mod = pe.i_min_mod;
                        for (int32_t j = j_min; j <= j_min + height - 1; ++j) {
                            int32_t offset = i_mod - imin_mod;
                            if (offset < 0) offset += p32;
                            size_t row_base = static_cast<size_t>(j - j_min) * w;
                            for (size_t pos = row_base + static_cast<size_t>(offset);
                                 pos < row_base + w;
                                 pos += static_cast<size_t>(pe.p)) {
                                size_t region_idx = pos >> LOG_BUCKET_REGION;
                                if (region_idx < num_regions) {
                                    local[region_idx].push_back({
                                        static_cast<uint16_t>(pos & (BUCKET_REGION_SIZE - 1)),
                                        pe.log_p
                                    });
                                }
                            }
                            i_mod += pe.delta;
                            if (i_mod >= p32) i_mod -= p32;
                        }
                    }
                });
            }
            for (auto& w_thread : scatter_workers) w_thread.join();

            // Merge thread-local buckets into the flat bucket_entries array
            for (size_t r = 0; r < num_regions; ++r) {
                for (size_t t = 0; t < scatter_threads; ++t) {
                    const auto& local = thread_buckets[t].per_region[r];
                    for (const auto& entry : local) {
                        uint32_t wp = write_pos[r]++;
                        if (wp < total_entries) {
                            bucket_entries[wp] = entry;
                        }
                    }
                }
            }
        }

        // Phase 2: apply bucket regions + tiny prime stride

        // Build tiny prime list (reuse CompactSmallPrime)
        std::vector<CompactSmallPrime> tiny_primes;
        for (const auto& pe : primes) {
            if (pe.flags == 0 && pe.p < TINY_THRESHOLD) {
                tiny_primes.push_back({
                    pe.p, pe.log_p,
                    static_cast<int16_t>(pe.delta),
                    static_cast<int16_t>(pe.i_min_mod),
                    static_cast<int16_t>(pe.i_mod_init)
                });
            }
        }

        // v-primes
        std::vector<VPrimeEntry> v_primes;
        for (const auto& pe : primes) {
            if (pe.flags == 1) v_primes.push_back({pe.p, pe.log_p});
        }

        // Apply per-region: bucket entries + tiny prime stride + v-primes
        // Each region writes to a non-overlapping sieve_array segment → thread-safe.
        auto apply_region = [&](size_t r) {
            size_t region_start = r * BUCKET_REGION_SIZE;
            size_t region_end = std::min(region_start + BUCKET_REGION_SIZE, total_area);

            // Apply scattered bucket entries for this region
            for (uint32_t bi = region_offsets[r]; bi < region_offsets[r + 1]; ++bi) {
                const auto& entry = bucket_entries[bi];
                size_t pos = region_start + entry.offset;
                if (pos < total_area) {
                    sieve_array_[pos] += entry.log_p;
                }
            }

            // Apply tiny primes via stride within this region
            int32_t region_j_start = static_cast<int32_t>(region_start / w) + j_min;
            int32_t region_j_end = static_cast<int32_t>((region_end - 1) / w) + j_min;
            region_j_end = std::min(region_j_end, j_min + height - 1);

            // Make working copies for this region's i_mod state
            auto tiny_copy = tiny_primes;
            for (auto& tp : tiny_copy) {
                // Advance i_mod to region_j_start
                int32_t p32 = static_cast<int32_t>(tp.p);
                int64_t row_offset = static_cast<int64_t>(region_j_start - j_min);
                int64_t advanced = static_cast<int64_t>(tp.i_mod) + row_offset * static_cast<int64_t>(tp.delta);
                int64_t mod = advanced % static_cast<int64_t>(p32);
                if (mod < 0) mod += p32;
                tp.i_mod = static_cast<int16_t>(mod);
            }

            for (int32_t j = region_j_start; j <= region_j_end; ++j) {
                size_t row_base = static_cast<size_t>(j - j_min) * w;
                size_t row_end = row_base + w;
                // Clamp to region bounds
                size_t eff_start = std::max(row_base, region_start);
                size_t eff_end = std::min(row_end, region_end);
                if (eff_start >= eff_end) continue;

                // v-primes: whole row
                for (const auto& vp : v_primes) {
                    if ((j % static_cast<int32_t>(vp.p)) == 0) {
                        for (size_t idx = eff_start; idx < eff_end; ++idx)
                            sieve_array_[idx] += vp.log_p;
                    }
                }

                // Tiny primes: stride within clamped range
                for (auto& tp : tiny_copy) {
                    int32_t off = static_cast<int32_t>(tp.i_mod) - static_cast<int32_t>(tp.i_min_mod);
                    if (off < 0) off += static_cast<int32_t>(tp.p);

                    size_t idx = row_base + static_cast<size_t>(off);
                    // Skip to eff_start if needed
                    if (idx < eff_start) {
                        size_t stride = static_cast<size_t>(tp.p);
                        size_t skip = (eff_start - idx + stride - 1) / stride;
                        idx += skip * stride;
                    }
                    size_t stride = static_cast<size_t>(tp.p);
                    for (; idx < eff_end; idx += stride) {
                        sieve_array_[idx] += tp.log_p;
                    }

                    int32_t new_mod = static_cast<int32_t>(tp.i_mod) + static_cast<int32_t>(tp.delta);
                    int32_t p32 = static_cast<int32_t>(tp.p);
                    if (new_mod >= p32) new_mod -= p32;
                    tp.i_mod = static_cast<int16_t>(new_mod);
                }
            }
        };

        // Dispatch regions in parallel using std::thread (no lock needed — disjoint writes)
        size_t num_threads = (max_threads_ > 0) ? max_threads_ : std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 4;
        if (num_regions < 4) num_threads = 1;  // Not enough regions to parallelize

        if (num_threads <= 1) {
            for (size_t r = 0; r < num_regions; ++r) apply_region(r);
        } else {
            std::vector<std::thread> threads;
            threads.reserve(num_threads);
            std::atomic<size_t> next_region{0};

            for (size_t t = 0; t < num_threads; ++t) {
                threads.emplace_back([&]() {
                    while (true) {
                        size_t r = next_region.fetch_add(1, std::memory_order_relaxed);
                        if (r >= num_regions) break;
                        apply_region(r);
                    }
                });
            }
            for (auto& t : threads) t.join();
        }
    }

    // ── Row-major Bucket Sieve (original) ─────────────────────

    /// 预填充大素数的 per-row buckets
    /// 对 flags==0 且 p >= width 的素数，计算其在每行的命中位置并写入 bucket。
    /// 复杂度: O(large_primes × total_rows)，但 total_rows 仅 ~1K，所以很快。
    [[nodiscard]] std::vector<std::vector<BucketEntry>> fill_buckets(
            const std::vector<PrimeEntry>& primes,
            uint32_t bucket_threshold) const {

        const int32_t j_min = region_.j_min;
        const int32_t j_max = region_.j_max;
        const int32_t total_rows = j_max - j_min + 1;
        const auto w = static_cast<int32_t>(region_.i_width());

        std::vector<std::vector<BucketEntry>> buckets(total_rows);

        // 预估每行平均条目数用于 reserve
        size_t large_count = 0;
        for (const auto& pe : primes) {
            if (pe.flags == 0 && pe.p >= bucket_threshold)
                ++large_count;
        }
        // 每个大素数平均每行命中 w/p ≈ 0.3-1.0 次
        size_t avg_per_row = large_count * 2 / static_cast<size_t>(std::max(total_rows, static_cast<int32_t>(1))) + 8;
        for (auto& b : buckets) b.reserve(avg_per_row);

        for (const auto& pe : primes) {
            if (pe.flags != 0 || pe.p < bucket_threshold) continue;

            int32_t i_mod = pe.i_mod_init;
            int32_t p32 = static_cast<int32_t>(pe.p);
            uint16_t lp = pe.log_p;

            for (int32_t row = 0; row < total_rows; ++row) {
                int32_t off = i_mod - pe.i_min_mod;
                if (off < 0) off += p32;

                // p >= width → at most 1 hit per row
                if (off < w) {
                    buckets[static_cast<size_t>(row)].push_back({static_cast<uint16_t>(off), lp});
                }

                // Advance carry-forward
                i_mod += pe.delta;
                if (i_mod >= p32) i_mod -= p32;
            }
        }

        return buckets;
    }

    /// Bucket sieve 主入口：分离小/大素数，预填充 bucket，多线程处理
    ///
    /// 小素数 (p < width): stride loop（多次命中/行，L1 cache 热驻留）
    /// 大素数 (p ≥ width): bucket apply（预排序命中，无空循环）
    void sieve_row_major(const std::vector<PrimeEntry>& primes) {
        const size_t w = static_cast<size_t>(region_.i_width());
        assert(w <= UINT16_MAX && "sieve width exceeds BucketEntry::offset uint16_t range");
        const uint32_t bucket_threshold = static_cast<uint32_t>(w);

        // Phase 0: 全局命中素数（u=0, v=0 → 每个位置都被整除，极罕见）
        for (const auto& pe : primes) {
            if (pe.flags == 2) {
                uint16_t lp = pe.log_p;
                for (size_t idx = 0; idx < sieve_array_.size(); ++idx)
                    sieve_array_[idx] += lp;
            }
        }

        // Phase 1: 预填充大素数 buckets
        auto buckets = fill_buckets(primes, bucket_threshold);

        // Pre-separate primes once (was per-thread-per-chunk)
        auto pre = preseparate_primes(primes, bucket_threshold);

        // Phase 2: 多线程行处理
        const int32_t j_min = region_.j_min;
        const int32_t j_max = region_.j_max;
        const int32_t total_rows = j_max - j_min + 1;
        const int32_t i_min = region_.i_min;

        size_t num_threads = (max_threads_ > 0) ? max_threads_ : std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 4;
        if (total_rows < 500) num_threads = 1;

        if (num_threads <= 1) {
            sieve_row_chunk(pre, primes, buckets, bucket_threshold,
                            j_min, j_max, w, i_min, 0);
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
                                 std::cref(pre), std::cref(primes),
                                 std::cref(buckets), bucket_threshold,
                                 chunk_start, chunk_end,
                                 w, i_min, row_offset);
            chunk_start = chunk_end + 1;
        }

        for (auto& t : threads) {
            t.join();
        }
    }

    /// 紧凑小素数条目（16 bytes vs PrimeEntry 36 bytes → 2.25× cache 效率）
    /// INVARIANT: bucket_threshold must be <= INT16_MAX (32767) for int16_t fields.
    struct CompactSmallPrime {
        uint32_t p;
        uint16_t log_p;
        int16_t delta;       // fits in int16: delta < p < bucket_threshold
        int16_t i_min_mod;   // fits in int16: i_min_mod < p
        int16_t i_mod;       // current carry-forward state, mutable per-thread
        // 12 bytes, padded to 12 (no need for 16)
    };

    /// v-prime (u=0: 整行命中) 条目
    struct VPrimeEntry {
        uint32_t p;
        uint16_t log_p;
    };

    /// Pre-separated prime lists (static fields only, built once per SQ)
    struct PreSeparatedPrimes {
        std::vector<CompactSmallPrime> small;  // flags==0 && p < bucket_threshold
        std::vector<VPrimeEntry> v;            // flags==1
    };

    /// Build pre-separated lists from full PrimeEntry vector (call once per SQ)
    [[nodiscard]] PreSeparatedPrimes preseparate_primes(
            const std::vector<PrimeEntry>& primes, uint32_t bucket_threshold) const {
        PreSeparatedPrimes result;
        result.small.reserve(primes.size());
        for (const auto& pe : primes) {
            if (pe.flags == 1) {
                result.v.push_back({pe.p, pe.log_p});
            } else if (pe.flags == 0 && pe.p < bucket_threshold) {
                result.small.push_back({
                    pe.p, pe.log_p,
                    static_cast<int16_t>(pe.delta),
                    static_cast<int16_t>(pe.i_min_mod),
                    0  // i_mod filled per-chunk
                });
            }
        }
        return result;
    }

    /// 处理一段行范围 [j_start, j_end]
    /// 小素数走 stride loop，大素数从 bucket 直接 apply
    void sieve_row_chunk(const PreSeparatedPrimes& pre,
                         const std::vector<PrimeEntry>& primes,
                         const std::vector<std::vector<BucketEntry>>& buckets,
                         uint32_t bucket_threshold,
                         int32_t j_start, int32_t j_end,
                         size_t w, [[maybe_unused]] int32_t i_min, int32_t row_offset) {

        // Copy pre-separated small primes, compute per-chunk i_mod
        assert(bucket_threshold <= INT16_MAX &&
               "bucket_threshold exceeds int16_t range for CompactSmallPrime");
        auto small_primes = pre.small;  // shallow copy (only i_mod differs)
        const auto& v_primes = pre.v;

        // Fill i_mod from initial values + row_offset * delta
        size_t si = 0;
        for (const auto& pe : primes) {
            if (pe.flags == 0 && pe.p < bucket_threshold) {
                int32_t p32 = static_cast<int32_t>(pe.p);
                int64_t advanced = static_cast<int64_t>(pe.i_mod_init) +
                                   static_cast<int64_t>(row_offset) * static_cast<int64_t>(pe.delta);
                int64_t mod = advanced % static_cast<int64_t>(p32);
                if (mod < 0) mod += p32;
                small_primes[si].i_mod = static_cast<int16_t>(mod);
                ++si;
            }
        }

        for (int32_t j = j_start; j <= j_end; ++j) {
            size_t row_base = static_cast<size_t>(j - region_.j_min) * w;
            size_t row_end = row_base + w;

            // ── v-primes: 整行命中 (稀少，通常 0-2 个) ──
            for (const auto& vp : v_primes) {
                if ((j % static_cast<int32_t>(vp.p)) == 0) {
                    uint16_t lp = vp.log_p;
                    for (size_t idx = row_base; idx < row_end; ++idx)
                        sieve_array_[idx] += lp;
                }
            }

            // ── 小素数: stride loop (紧凑数组, 无 flag 检查) ──
            for (auto& sp : small_primes) {
                int32_t offset = static_cast<int32_t>(sp.i_mod) - static_cast<int32_t>(sp.i_min_mod);
                if (offset < 0) offset += static_cast<int32_t>(sp.p);

                size_t idx = row_base + static_cast<size_t>(offset);
                uint16_t lp = sp.log_p;
                size_t stride = static_cast<size_t>(sp.p);
                for (; idx < row_end; idx += stride) {
                    sieve_array_[idx] += lp;
                }

                // Advance carry-forward
                int32_t new_mod = static_cast<int32_t>(sp.i_mod) + static_cast<int32_t>(sp.delta);
                int32_t p32 = static_cast<int32_t>(sp.p);
                if (new_mod >= p32) new_mod -= p32;
                sp.i_mod = static_cast<int16_t>(new_mod);
            }

            // ── 大素数: bucket apply (无分支，直接索引写入) ──
            size_t row_idx = static_cast<size_t>(j - region_.j_min);
            const auto& bucket = buckets[row_idx];
            for (const auto& entry : bucket) {
                sieve_array_[row_base + entry.offset] += entry.log_p;
            }
        }
    }

    /// 收集候选点（加法筛 + NEON 向量化扫描）
    /// 加法筛中：candidate iff accumulated >= (init_val - threshold)
    /// 等价于旧减法筛中：residual <= threshold
    [[nodiscard]] std::vector<SieveCandidate> collect_candidates(const LatticeBasis& basis) const {
        std::vector<SieveCandidate> candidates;

        uint16_t threshold = params_.combined_threshold();
        // effective_threshold = init_val - threshold (if init too low, no valid candidates)
        if (last_init_val_ <= threshold) {
            return {};  // Log estimate too small — all positions would pass, return empty
        }
        uint16_t eff_thresh = static_cast<uint16_t>(last_init_val_ - threshold);

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

} // namespace gnfs::sieve
