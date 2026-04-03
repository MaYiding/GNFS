#pragma once

#include "types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace gnfs {
namespace core {

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
        p.digits = static_cast<size_t>(n_bits * 0.30103 + 1);  // log10(2) ≈ 0.30103

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
        // === 因子基界 — 经验参数表 (CADO-NFS / msieve 校准) ===
        // =============================================================
        // B_alg ≈ 2 × B_rat: 代数侧有 degree 个根，需更大因子基捕获
        // 参照: gnfs_optimization_guide.md §1.2-1.3
        //
        //   digits | B_rat   | B_alg   | lp_bits | LP/B  | 说明
        //   ≤6     | 500     | 1000    | 0       | —     | tiny N, 全光滑
        //   ≤10    | 1000    | 2000    | 0       | —     | small N
        //   ≤15    | 3000    | 6000    | auto    | ~30×  | LP 使 NFS 对小 N 可行
        //   ≤20    | 8000    | 16000   | auto    | ~30×  | 增大 FB 提高 1LP/full 比例
        //   ≤25    | 10000   | 20000   | auto    | ~30×  | 25-digit 关键区间
        //   ≤30    | 20000   | 40000   | auto    | ~30×  |
        //   ≤40    | 100000  | 200000  | auto    | ~30×  |
        //   ≤50    | 500000  | 1000000 | auto    | ~30×  |
        //   >50    | L_N     | 2×L_N   | auto    | ~30×  | 渐近公式
        //
        // LP_bound = B_alg × LP_MULTIPLIER (CADO-NFS 典型 LP/FB ≈ 30×)
        // lp_bits = floor(log2(LP_bound))
        constexpr double LP_MULTIPLIER = 30.0;

        double B_rat, B_alg;
        bool enable_lp = true;

        if (p.digits <= 6) {
            B_rat = 500;    B_alg = 1000;    enable_lp = false;
        } else if (p.digits <= 10) {
            B_rat = 1000;   B_alg = 2000;    enable_lp = false;
        } else if (p.digits <= 15) {
            B_rat = 3000;   B_alg = 6000;
        } else if (p.digits <= 20) {
            // 19-digit (61-bit): 代数范数 ~10^17, 需要更大 FB 提高 1LP 比例
            B_rat = 8000;   B_alg = 16000;
        } else if (p.digits <= 25) {
            // 21-25 digit: 维持 Session 47 校准值（增大导致矩阵过大+merge不足）
            B_rat = 10000;  B_alg = 20000;
        } else if (p.digits <= 30) {
            B_rat = 20000;  B_alg = 40000;
        } else if (p.digits <= 40) {
            B_rat = 100000; B_alg = 200000;
        } else if (p.digits <= 50) {
            B_rat = 500000; B_alg = 1000000;
        } else {
            // >50 digits: L_N formula with c_B ≈ 0.9
            double c_B = 0.9;
            B_rat = std::exp(c_B * l_val);
            B_rat = std::max(B_rat, 1000000.0);
            B_rat = std::min(B_rat, 4e9);
            B_alg = std::min(B_rat * 2.0, 4e9);
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
        // 与因子基界联动：sieve_width ≈ B_rat × 3，受 MAX_SIEVE_AREA 约束
        double sieve_width = std::max(static_cast<double>(p.rational_bound) * 3.0, 1000.0);
        double sieve_height = std::max(sieve_width / 4.0, 200.0);

        // Cap total sieve area to prevent catastrophic memory allocation.
        // 256M positions × sizeof(uint16_t) = 512 MB per sieve array.
        constexpr double MAX_SIEVE_AREA = 256.0 * 1024 * 1024;
        double sieve_area = sieve_width * sieve_height;
        if (sieve_area > MAX_SIEVE_AREA) {
            double scale = std::sqrt(MAX_SIEVE_AREA / sieve_area);
            sieve_width = std::floor(sieve_width * scale);
            sieve_height = std::floor(sieve_height * scale);
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
        // - 有 LP: per_side = (lp_bits + 3) × log_scale（允许 cofactor ≤ 2^lp_bits × 8）
        if (lp_bits > 0) {
            // 允许每侧有一个大素数级别的余因子
            uint16_t per_side = static_cast<uint16_t>(
                std::min(1000.0, (lp_bits + 3.0) * SIEVE_LOG_SCALE));
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
        p.num_qc_primes = std::max(32u, std::min(128u, static_cast<uint32_t>(p.digits * 2)));
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
            double lp_primes = (lp_bound_d - alg_bound_d) /
                               std::log(std::max(lp_bound_d, 2.0));
            double key_space = lp_primes * static_cast<double>(std::max(degree, 2u));
            double mc = static_cast<double>(matrix_columns);

            // Birthday: safety=8 (covers 2LP fraction + singleton filtering)
            double n_min = std::sqrt(2.0 * key_space * mc * 8.0);
            // Floor: at least 3× matrix columns
            return static_cast<size_t>(
                std::max(n_min, mc * 3.0));
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

    /// 打印参数摘要 (到 stdout)
    void print_summary() const {
        auto print_size = [](size_t bytes) {
            if (bytes < 1024) return std::to_string(bytes) + " B";
            if (bytes < 1024*1024) return std::to_string(bytes/1024) + " KB";
            if (bytes < 1024*1024*1024) return std::to_string(bytes/(1024*1024)) + " MB";
            return std::to_string(bytes/(1024*1024*1024)) + " GB";
        };
        (void)print_size;  // suppress unused warning in non-verbose builds
    }
};

} // namespace core
} // namespace gnfs
