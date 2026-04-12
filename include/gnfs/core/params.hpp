#pragma once

#include "types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace gnfs::core {

/// GNFS 参数计算器
/// 根据 N 的大小自动计算最优的 GNFS 参数
/// 基于 L_N[1/3, c] = exp(c · (ln N)^{1/3} · (ln ln N)^{2/3})
///
/// 参数表经过 CADO-NFS / msieve 标准校准（Session 46 P0 修复）。
/// 旧代码因子基界对 15-40 位数过大 20-200×，导致矩阵/BL/Sqrt 时间爆炸。
struct GNFSParams {
    // === 基本信息 ===
    size_t bits = 0;           // N 的位数
    size_t digits = 0;         // N 的十进制位数

    // === 多项式选择 ===
    uint32_t degree = 3;                     // 多项式度数
    uint64_t leading_coeff_bound = 10000;    // 首项系数搜索上界
    uint64_t search_radius = 100;            // Newton 优化搜索半径
    uint32_t num_candidates = 1000;          // 候选多项式数量
    uint32_t skewness_steps = 100;           // 偏度搜索步数

    // === 因子基 ===
    uint32_t rational_bound = 5000;          // 有理侧因子基界
    uint32_t algebraic_bound = 5000;         // 代数侧因子基界
    uint64_t large_prime_bound = 500000;     // 大素数界
    uint32_t large_prime_bits = 0;           // 大素数位数 (0 = 不启用 LP)
    uint8_t  log_scale = SIEVE_LOG_SCALE;    // 对数缩放因子（动态调整）

    // === 筛法 ===
    int32_t sieve_i_min = -2000;
    int32_t sieve_i_max = 1999;
    int32_t sieve_j_min = 1;
    int32_t sieve_j_max = 500;
    uint16_t rational_threshold = 50;
    uint16_t algebraic_threshold = 50;

    // === Special-Q ===
    uint32_t special_q_min = 1000;
    uint32_t special_q_max = 5000;
    uint32_t max_special_q = 2000;           // 最大处理的 special-q 数量

    // === 线性代数 ===
    uint32_t num_qc_primes = 64;             // 二次特征素数数量
    uint32_t target_excess = 100;            // 目标关系过剩数

    // === Schirokauer ===
    // 注意: GF(2) 矩阵只能用 ℓ=2
    // schirokauer_primes 固定为 {2}

    // === 进度报告 ===
    uint32_t progress_interval = 50;         // 每 N 个 special-q 报告一次
    bool verbose = true;

    /// 根据 N 的位数计算所有参数
    static GNFSParams compute(size_t n_bits) {
        GNFSParams p;
        p.bits = n_bits;
        constexpr double LOG10_2 = 0.30103;  // log10(2)
        p.digits = static_cast<size_t>(n_bits * LOG10_2 + 1);

        // L_N 函数的核心值: (ln N)^{1/3} · (ln ln N)^{2/3}
        double ln_n = n_bits * std::log(2.0);
        double ln_ln_n = std::log(std::max(ln_n, 1.0));
        double l_val = std::pow(ln_n, 1.0/3.0) * std::pow(std::max(ln_ln_n, 1.0), 2.0/3.0);

        // === 多项式度数 ===
        // 标准选择: degree = round((3 ln N / ln ln N)^{1/3})
        // 小 N 用经验值，大 N 用解析公式
        if (n_bits <= 100) {
            p.degree = 3;
        } else {
            double d_opt = std::pow(3.0 * ln_n / ln_ln_n, 1.0 / 3.0);
            p.degree = std::max(4u, static_cast<uint32_t>(std::lround(d_opt)));
            p.degree = std::min(p.degree, 8u);  // 理论上限: degree 8
        }

        // =============================================================
        // === 因子基界 — 经验参数表 ===
        // =============================================================
        // 设计约束:
        //   1. FB 越大 → smoothness rate 越高 (u^{-u} 指数提升)
        //   2. FB 越大 → 矩阵列数越多 → BL 耗时 ∝ cols³
        //   3. 甜蜜点: 矩阵列数 < 7K → BL < 3s, 列数 < 10K → BL < 10s
        //
        // BL 瓶颈决定 FB 上限: π(B_rat) + π(B_alg) + extras < 7000
        //   ≤15: 8K/16K → ~3100 cols → BL ~0.5s
        //   ≤20: 15K/30K → ~5700 cols → BL ~1.5s
        //   ≤25: 20K/40K → ~6650 cols → BL ~2.5s
        //   ≤30: 50K/100K → ~15K cols → BL ~30s (needs BL optimization)
        //
        // LP/FB ratio: higher ratio = more LP range per FB bound
        // ≤25 digit uses smaller FB so LP_MULT compensates
        double LP_MULTIPLIER = 40.0;

        double B_rat, B_alg;
        bool enable_lp = true;

        if (p.digits <= 6) {
            B_rat = 500;     B_alg = 1000;     enable_lp = false;
        } else if (p.digits <= 10) {
            B_rat = 2000;    B_alg = 4000;     enable_lp = false;
        } else if (p.digits <= 15) {
            B_rat = 8000;    B_alg = 16000;
        } else if (p.digits <= 20) {
            B_rat = 15000;   B_alg = 30000;
        } else if (p.digits <= 25) {
            B_rat = 6000;    B_alg = 12000;
        } else if (p.digits <= 30) {
            B_rat = 50000;   B_alg = 100000;
        } else if (p.digits <= 40) {
            B_rat = 200000;  B_alg = 400000;
        } else if (p.digits <= 50) {
            B_rat = 300000;  B_alg = 600000;
        } else if (p.digits <= 60) {
            // CADO-NFS C60: lim0≈400K, lim1≈800K
            B_rat = 500000;  B_alg = 1000000;
        } else if (p.digits <= 70) {
            // CADO-NFS C70: lim0≈800K, lim1≈1.5M
            B_rat = 800000;  B_alg = 1600000;
        } else if (p.digits <= 80) {
            // CADO-NFS C80: lim0≈1M, lim1≈2M
            B_rat = 1000000; B_alg = 2000000;
        } else if (p.digits <= 90) {
            // CADO-NFS C90: lim0≈4M, lim1≈8M
            B_rat = 4000000; B_alg = 8000000;
        } else if (p.digits <= 100) {
            // CADO-NFS C100: lim0≈8M, lim1≈16M
            B_rat = 8000000; B_alg = 16000000;
        } else {
            // >100 digits: L_N formula with conservative c_B ≈ 0.8
            double c_B = 0.8;
            B_rat = std::exp(c_B * l_val);
            B_rat = std::max(B_rat, 30000000.0);
            B_rat = std::min(B_rat, 1e9);
            B_alg = std::min(B_rat * 2.0, 2e9);
        }

        // LP bits: LP_bound = B_alg × 30, lp_bits = floor(log2(LP_bound))
        uint32_t lp_bits = 0;
        if (enable_lp) {
            double lp_bound = B_alg * LP_MULTIPLIER;
            lp_bits = static_cast<uint32_t>(std::floor(std::log2(lp_bound)));
            lp_bits = std::min(lp_bits, 30u);  // 安全上限
        }

        p.rational_bound = static_cast<uint32_t>(std::min(B_rat, static_cast<double>(UINT32_MAX)));
        p.algebraic_bound = static_cast<uint32_t>(std::min(B_alg, static_cast<double>(UINT32_MAX)));
        p.large_prime_bits = lp_bits;

        // === 大素数界 ===
        // 使用 2^lp_bits（与 CADO-NFS 一致）
        if (lp_bits > 0) {
            p.large_prime_bound = 1ULL << lp_bits;
        } else {
            // 无 LP: 设为因子基界（cofactorizer 不接受 >B 的余因子）
            p.large_prime_bound = p.rational_bound;
        }

        // === 筛区域 ===
        // CADO-NFS 风格：固定小筛区域 + 大 FB = 高光滑概率 + 快筛
        // 筛区域大小与 FB 界解耦。原理：
        //   小区域 → 小范数 → 高光滑率 u^{-u}（u 更小）
        //   多 Special-Q 弥补单 SQ 位置少
        // CADO C80: I=2048, 我们稍大以兼顾实现效率。
        double sieve_width, sieve_height;
        if (p.digits <= 6) {
            sieve_width = 1000;   sieve_height = 400;    // tiny N
        } else if (p.digits <= 10) {
            sieve_width = 2000;   sieve_height = 1000;   // small N
        } else if (p.digits <= 25) {
            sieve_width = 4096;   sieve_height = 1024;   // 4M positions — smaller j → smaller norms → higher smoothness
        } else if (p.digits <= 30) {
            sieve_width = 4096;   sieve_height = 2048;   // 8M positions, ~16MB
        } else if (p.digits <= 80) {
            // 50-80 digit: 33M positions, ~67MB per SQ
            // Keeping area moderate to control norms; more SQs compensate
            sieve_width = 8192;   sieve_height = 4096;
        } else {
            // ≥81 digits: 134M positions for wider SQ coverage
            sieve_width = 16384;  sieve_height = 8192;
        }

        p.sieve_i_min = -static_cast<int32_t>(sieve_width / 2);
        p.sieve_i_max = static_cast<int32_t>(sieve_width / 2) - 1;
        p.sieve_j_min = 1;
        p.sieve_j_max = static_cast<int32_t>(sieve_height);

        // === 对数缩放因子 ===
        // 所有组件统一使用 core::SIEVE_LOG_SCALE (types.hpp)。
        // GNFSParams.log_scale 按 FB 规模动态调整（供诊断参考）。
        if (p.rational_bound > 100000)
            p.log_scale = 12;
        if (p.rational_bound > 1000000)
            p.log_scale = 14;
        if (p.rational_bound > 10000000)
            p.log_scale = 16;

        // === 筛阈值 ===
        // 使用 core::SIEVE_LOG_SCALE (types.hpp) — 全局唯一定义
        //
        // 阈值 = 基础裕度（sieve 精度误差）+ LP 允许量
        // - 无 LP: per_side = 3.5 × log_scale（允许 cofactor ≤ ~11）
        // - 有 LP: per_side = (lp_bits × factor + slack) × log_scale
        //
        // 优化: 对小 N (≤25 digit), 使用 0.6× 系数减少候选数量。
        // 较紧的阈值产生更少但质量更高的候选（更高光滑率），
        // 虽然需要更多 SQ 补偿，但 cofac 时间大幅下降（sieve 远快于 cofac）。
        // 实测: 25-digit Phase 3 从 1.4s 降至 1.0s。
        if (lp_bits > 0) {
            double thresh_factor = (p.digits <= 25) ? 0.6 : 1.0;
            double slack = (p.digits <= 25) ? 2.0 : 3.0;
            uint16_t per_side = static_cast<uint16_t>(
                std::min(1000.0, (lp_bits * thresh_factor + slack) * SIEVE_LOG_SCALE));
            p.rational_threshold = per_side;
            p.algebraic_threshold = per_side;
        } else {
            // 全光滑：只允许小的 sieve 精度误差
            p.rational_threshold = static_cast<uint16_t>(3.5 * SIEVE_LOG_SCALE);
            p.algebraic_threshold = p.rational_threshold;
        }

        // === Special-Q 范围 ===
        // Special-Q 应在因子基界以上，提供更好的格结构
        // CADO-NFS 典型用 ~10×B
        p.special_q_min = p.algebraic_bound + 1;
        p.special_q_max = static_cast<uint32_t>(
            std::min(static_cast<uint64_t>(p.algebraic_bound) * 10,
                     static_cast<uint64_t>(UINT32_MAX)));

        // 最大 special-q 数量：基于预估关系需求量
        // 每个 SQ 平均产出 1-5 个关系（取决于 B/LP），需要足够多的 SQ
        {
            size_t est_rels = p.estimated_relations_needed();
            // 保守假设每 SQ 平均 1 个关系（小 B 时命中率低），乘 3 安全余量
            uint32_t needed_sq = static_cast<uint32_t>(
                std::min(static_cast<size_t>(UINT32_MAX), est_rels * 3));
            // 下限：按位数设置
            uint32_t min_sq = (p.digits < 15) ? 2000u :
                              (p.digits < 30) ? 20000u :
                              (p.digits < 50) ? 100000u : 1000000u;
            p.max_special_q = std::max(min_sq, needed_sq);
        }

        // === 多项式选择参数 ===
        // 首项系数界需要随 N^{1/d} 缩放
        // m ≈ N^{1/d}, 首项系数应在 m^{1/3} 左右
        double log_m = ln_n / p.degree;
        double log_ad = log_m / 3.0;
        p.leading_coeff_bound = static_cast<uint64_t>(
            std::max(std::exp(log_ad), 10000.0));
        p.leading_coeff_bound = std::min(p.leading_coeff_bound, uint64_t(1e12));

        // 搜索半径随 m 缩放
        p.search_radius = static_cast<uint64_t>(
            std::max(100.0, std::exp(log_m * 0.3)));
        p.search_radius = std::min(p.search_radius, uint64_t(1e8));

        // 候选数量
        p.num_candidates = std::max(1000u, static_cast<uint32_t>(p.digits * 100));
        p.skewness_steps = std::max(100u, static_cast<uint32_t>(p.digits * 5));

        // === 线性代数 ===
        // QC primes ensure deps are squares in Z[α]. Need ≥ degree-1 columns.
        // For small N (≤30 digits), 20 QC primes is ample (saves 60% matrix build time).
        if (p.digits <= 30) {
            p.num_qc_primes = 20;
        } else {
            p.num_qc_primes = std::max(32u, std::min(128u, static_cast<uint32_t>(p.digits * 2)));
        }
        // target_excess: 固定基础 + 额外列数（QC + Schirokauer + sign）
        // 不再使用 0.15·π(B) 的比例（随 B 线性增长过快）
        uint32_t extra_cols = p.num_qc_primes + p.degree + 1;  // QC + Schirokauer + sign
        p.target_excess = std::max(200u, extra_cols + 100);

        // === 进度报告间隔 ===
        if (p.max_special_q > 10000) {
            p.progress_interval = 500;
        } else if (p.max_special_q > 1000) {
            p.progress_interval = 100;
        } else {
            p.progress_interval = 50;
        }

        return p;
    }

    /// 估算需要的关系数量（原始关系，过滤前）
    [[nodiscard]] size_t estimated_relations_needed() const {
        double pi_r = rational_bound / std::log(static_cast<double>(std::max(rational_bound, 2u)));
        double pi_a = algebraic_bound / std::log(static_cast<double>(std::max(algebraic_bound, 2u)));
        double matrix_cols = pi_r + pi_a + target_excess;

        if (large_prime_bits > 0 && large_prime_bound > algebraic_bound) {
            return raw_relation_target(static_cast<size_t>(matrix_cols));
        }
        return static_cast<size_t>(matrix_cols);
    }

    /// 计算 LP 感知的原始关系目标
    /// @param matrix_columns 矩阵实际列数
    /// @return 需要收集的原始关系数（过滤前）
    ///
    /// Birthday model: n ≥ √(2·K·M·s)
    ///   K = LP key space, M = matrix_columns, s = safety factor
    ///   safety=8: 补偿 2LP 占比 (~70-90% partial) + singleton 过滤损失
    [[nodiscard]] size_t raw_relation_target(size_t matrix_columns) const {
        if (large_prime_bits > 0 && large_prime_bound > algebraic_bound) {
            double lp_bound_d = static_cast<double>(large_prime_bound);
            double alg_bound_d = static_cast<double>(algebraic_bound);
            // Prime counting: π(x) ≈ x/ln(x), so LP primes ≈ π(lp) - π(alg)
            double lp_primes = lp_bound_d / std::log(std::max(lp_bound_d, 2.0))
                             - alg_bound_d / std::log(std::max(alg_bound_d, 2.0));
            double key_space = lp_primes * static_cast<double>(std::max(degree, 2u));
            double mc = static_cast<double>(matrix_columns);

            // Birthday: safety=25. Must account for LP columns that expand
            // the matrix beyond matrix_columns estimate. Run 12 showed 3.4M raw
            // → 75K usable but matrix had 77K cols (1981 LP cols).
            // safety=25 gives ~12% more raw, producing ~10% more usable.
            double n_min = std::sqrt(2.0 * key_space * mc * 25.0);
            // Floor: at least 5× matrix columns (covers 2LP merge overhead)
            return static_cast<size_t>(
                std::max(n_min, mc * 5.0));
        }
        return matrix_columns;
    }

    /// 估算筛区域大小 (位置数)
    [[nodiscard]] size_t sieve_region_size() const {
        return static_cast<size_t>(sieve_i_max - sieve_i_min + 1) *
               static_cast<size_t>(sieve_j_max - sieve_j_min + 1);
    }

    /// 估算筛区域内存使用 (bytes, uint16_t per position)
    [[nodiscard]] size_t sieve_memory_bytes() const {
        return sieve_region_size() * sizeof(uint16_t);
    }

};

} // namespace gnfs::core
