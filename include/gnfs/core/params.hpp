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
        // === 因子基界 — CADO-NFS 校准参数表 ===
        // =============================================================
        // 设计原则:
        //   1. 基于 L_N[1/3, c] 理论值 + CADO-NFS 实践修正
        //   2. 平滑过渡: 相邻区间界差 ≤ 3×, 避免参数跳跃
        //   3. B_alg = 2 × B_rat (CADO-NFS 标准 lim1 = 2 × lim0)
        //   4. LP multiplier 随 N 变化: 小 N 用更大倍率 (LP 覆盖少量大素数)
        //
        // CADO-NFS 参考 (https://cado-nfs.gitlabpages.inria.fr/cado-nfs/):
        //   C35: lim0=50K   C40: lim0=100K  C50: lim0=200K
        //   C60: lim0=400K  C70: lim0=800K  C80: lim0=2M
        //   C90: lim0=6M    C100: lim0=12M
        //
        double LP_MULTIPLIER;
        double B_rat, B_alg;
        bool enable_lp = true;

        if (p.digits <= 6) {
            B_rat = 500;     B_alg = 1000;     enable_lp = false;
            LP_MULTIPLIER = 1.0;
        } else if (p.digits <= 10) {
            B_rat = 2000;    B_alg = 4000;     enable_lp = false;
            LP_MULTIPLIER = 1.0;
        } else if (p.digits <= 15) {
            B_rat = 5000;    B_alg = 10000;
            LP_MULTIPLIER = 50.0;
        } else if (p.digits <= 20) {
            B_rat = 8000;    B_alg = 16000;
            LP_MULTIPLIER = 50.0;
        } else if (p.digits <= 25) {
            B_rat = 15000;   B_alg = 30000;
            LP_MULTIPLIER = 40.0;
        } else if (p.digits <= 30) {
            B_rat = 30000;   B_alg = 60000;
            LP_MULTIPLIER = 30.0;
        } else if (p.digits <= 35) {
            // CADO-NFS C35: lim0≈50K
            B_rat = 50000;   B_alg = 100000;
            LP_MULTIPLIER = 25.0;
        } else if (p.digits <= 40) {
            // CADO-NFS C40: lim0≈100K
            B_rat = 100000;  B_alg = 200000;
            LP_MULTIPLIER = 20.0;
        } else if (p.digits <= 45) {
            B_rat = 150000;  B_alg = 300000;
            LP_MULTIPLIER = 20.0;
        } else if (p.digits <= 50) {
            // CADO-NFS C50: lim0≈200K
            B_rat = 200000;  B_alg = 400000;
            LP_MULTIPLIER = 15.0;
        } else if (p.digits <= 55) {
            B_rat = 300000;  B_alg = 600000;
            LP_MULTIPLIER = 12.0;
        } else if (p.digits <= 60) {
            // CADO-NFS C60: lim0≈400K, lim1≈800K
            B_rat = 400000;  B_alg = 800000;
            LP_MULTIPLIER = 10.0;
        } else if (p.digits <= 65) {
            B_rat = 600000;  B_alg = 1200000;
            LP_MULTIPLIER = 10.0;
        } else if (p.digits <= 70) {
            // CADO-NFS C70: lim0≈800K, lim1≈1.5M
            B_rat = 800000;  B_alg = 1600000;
            LP_MULTIPLIER = 8.0;
        } else if (p.digits <= 80) {
            // CADO-NFS C80: lim0≈2M, lim1≈4M
            B_rat = 2000000; B_alg = 4000000;
            LP_MULTIPLIER = 6.0;
        } else if (p.digits <= 90) {
            // CADO-NFS C90: lim0≈6M, lim1≈12M
            B_rat = 6000000; B_alg = 12000000;
            LP_MULTIPLIER = 5.0;
        } else if (p.digits <= 100) {
            // CADO-NFS C100: lim0≈12M, lim1≈24M
            B_rat = 12000000; B_alg = 24000000;
            LP_MULTIPLIER = 4.0;
        } else {
            // >100 digits: L_N formula
            double c_B = 0.9;
            B_rat = std::exp(c_B * l_val);
            B_rat = std::max(B_rat, 30000000.0);
            B_rat = std::min(B_rat, 1e9);
            B_alg = std::min(B_rat * 2.0, 2e9);
            LP_MULTIPLIER = 3.0;
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
        // 原理: 小区域 → 小范数 → 高光滑率; 多 SQ 弥补单 SQ 位置少
        // CADO-NFS I 参数 (sieve line = 2^I, area = 2^(2I-1)):
        //   C35: I=11 (2K×1K), C50: I=12 (4K×2K), C60: I=13 (8K×4K)
        //   C80: I=14 (16K×8K), C100: I=15 (32K×16K)
        double sieve_width, sieve_height;
        if (p.digits <= 6) {
            sieve_width = 1000;   sieve_height = 400;
        } else if (p.digits <= 10) {
            sieve_width = 2000;   sieve_height = 800;
        } else if (p.digits <= 25) {
            sieve_width = 4096;   sieve_height = 1024;   // 4M positions
        } else if (p.digits <= 35) {
            sieve_width = 2048;   sieve_height = 1024;   // I=11, 2M positions
        } else if (p.digits <= 50) {
            sieve_width = 4096;   sieve_height = 2048;   // I=12, 8M positions
        } else if (p.digits <= 60) {
            sieve_width = 8192;   sieve_height = 4096;   // I=13, 33M positions
        } else if (p.digits <= 80) {
            sieve_width = 16384;  sieve_height = 8192;   // I=14, 134M positions
        } else {
            sieve_width = 32768;  sieve_height = 16384;  // I=15, 536M positions
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
            // Tighter threshold = fewer candidates = less cofactorization time
            // But too tight = miss smooth relations = need more SQs
            // Balance: 0.6-0.8× for most sizes
            double thresh_factor = (p.digits <= 30) ? 0.6 :
                                   (p.digits <= 60) ? 0.7 : 0.8;
            double slack = (p.digits <= 30) ? 2.0 : 3.0;
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
            // 下限：按位数平滑设置
            uint32_t min_sq = (p.digits < 15) ? 2000u :
                              (p.digits < 25) ? 10000u :
                              (p.digits < 35) ? 50000u :
                              (p.digits < 50) ? 200000u :
                              (p.digits < 70) ? 500000u :
                              (p.digits < 90) ? 2000000u : 5000000u;
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
        if (p.digits >= 50) {
            p.progress_interval = 100;
        } else if (p.digits >= 30) {
            p.progress_interval = 50;
        } else {
            p.progress_interval = 20;
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

    /// 计算初始原始关系目标
    /// @param matrix_columns 矩阵实际列数
    /// @return 初始目标（自适应循环会根据实际 merge rate 调整）
    ///
    /// 策略: 保守的初始目标 + 自适应循环自动扩容
    /// 不用 birthday 公式（对小 N 过高估计、对大 N 不够准确）
    /// 而是从 mc × 2.0 开始，让 adaptive loop 倍增直到够用
    [[nodiscard]] size_t raw_relation_target(size_t matrix_columns) const {
        if (large_prime_bits > 0 && large_prime_bound > algebraic_bound) {
            double mc = static_cast<double>(matrix_columns);
            // Start with mc × 3.0 — conservative but enough for first adaptive round.
            // The adaptive sieve-filter-merge loop doubles each round until enough.
            // For LP-heavy factorizations (30+ digit), typical survival rate is 5-30%,
            // so several adaptive rounds are expected.
            return static_cast<size_t>(mc * 3.0);
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
