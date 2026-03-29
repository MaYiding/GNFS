#pragma once

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
    uint8_t  log_scale = 10;                 // 对数缩放因子

    // === 筛法 ===
    int32_t sieve_i_min = -2000;
    int32_t sieve_i_max = 1999;
    int32_t sieve_j_min = 1;
    int32_t sieve_j_max = 500;
    uint8_t rational_threshold = 50;
    uint8_t algebraic_threshold = 50;

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
        double ln_ln_n = std::log(ln_n);
        double l_val = std::pow(ln_n, 1.0/3.0) * std::pow(ln_ln_n, 2.0/3.0);

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

        // === 因子基界 ===
        // L_N formula is asymptotic — useless for small N.
        // Use empirical values for small digits, L_N only for large.
        double B;
        if (p.digits <= 6) {
            B = 2000;
        } else if (p.digits <= 10) {
            B = 3000;
        } else if (p.digits <= 15) {
            B = 20000;
        } else if (p.digits <= 20) {
            B = 50000;
        } else if (p.digits <= 30) {
            B = 200000;
        } else if (p.digits <= 40) {
            B = 1000000;
        } else {
            // For 40+ digits, use L_N formula with c_B = 1.1
            double c_B = 1.1;
            double log_B = c_B * l_val;
            B = std::exp(log_B);
            B = std::max(B, 2000000.0);
            B = std::min(B, 4e9);  // uint32_t 上限 ~4.29e9；sieve 自动缩放内存
        }

        p.rational_bound = static_cast<uint32_t>(std::min(B, static_cast<double>(UINT32_MAX)));
        p.algebraic_bound = p.rational_bound;

        // === 大素数界 ===
        // 通常为 100B-1000B
        double lp_multiplier = 100.0;
        if (p.digits > 40) lp_multiplier = 200.0;
        if (p.digits > 60) lp_multiplier = 500.0;
        p.large_prime_bound = static_cast<uint64_t>(
            std::min(static_cast<double>(p.rational_bound) * lp_multiplier,
                     static_cast<double>(UINT64_MAX / 2)));

        // === 筛区域 ===
        // Scale with FB bound: need enough positions to find smooth relations
        // Larger sieve = more candidates per special-q, but more memory
        double sieve_width, sieve_height;
        if (p.digits <= 10) {
            sieve_width = 4000;  sieve_height = 1000;
        } else if (p.digits <= 15) {
            sieve_width = 8000;  sieve_height = 2000;
        } else if (p.digits <= 20) {
            sieve_width = 16000; sieve_height = 4000;
        } else if (p.digits <= 30) {
            sieve_width = 32000; sieve_height = 8000;
        } else {
            sieve_width = std::sqrt(static_cast<double>(p.rational_bound)) * 8.0;
            sieve_width = std::max(sieve_width, 32000.0);
            sieve_width = std::min(sieve_width, 4e6);
            sieve_height = sieve_width / 4.0;
        }

        // Cap total sieve area to prevent catastrophic memory allocation.
        // Without this cap, rational_bound=1e9 produces 16 billion positions
        // = 32 GB per sieve array (× num_threads in sieve_parallel).
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

        // === 筛阈值 ===
        // 合并筛残差 ≈ log2(rat_cofactor)*scale + log2(alg_cofactor)*scale。
        // 使用 3.5 * log_scale 作为每侧阈值。此值补偿了 compute_log_prime_precise
        // 相对旧 compute_log_prime 的精度提升（旧 log_p 系统性低估 ~(scale-1)/2
        // 每个素数，累积 ~40-80 偏差被旧阈值 50-80 隐含补偿）。
        p.rational_threshold = static_cast<uint8_t>(
            std::min(255.0, 3.5 * p.log_scale));
        p.algebraic_threshold = p.rational_threshold;

        // 对数缩放因子: 更大的因子基需要更大的缩放
        if (p.rational_bound > 100000)
            p.log_scale = 12;
        if (p.rational_bound > 1000000)
            p.log_scale = 14;
        if (p.rational_bound > 10000000)
            p.log_scale = 16;

        // === Special-Q 范围 ===
        // Special-Q 应在因子基界以上，提供更好的格结构
        // 在 FB 内的 SQ 会被筛选重复计算，浪费效率
        p.special_q_min = p.algebraic_bound + 1;
        p.special_q_max = static_cast<uint32_t>(
            std::min(static_cast<uint64_t>(p.algebraic_bound) * 3,
                     static_cast<uint64_t>(UINT32_MAX)));

        // 最大 special-q 数量随问题规模增长
        if (p.digits < 15) {
            p.max_special_q = 2000;
        } else if (p.digits < 30) {
            p.max_special_q = 10000;
        } else if (p.digits < 50) {
            p.max_special_q = 100000;
        } else {
            p.max_special_q = 1000000;
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
        // target_excess 需要覆盖矩阵额外列（QC + Schirokauer + sign + 大素数列）
        // 以及单例过滤带来的关系损失。经验公式: ~20% 的因子基大小。
        p.target_excess = std::max(200u,
            static_cast<uint32_t>(
                p.num_qc_primes + p.degree + 1 +       // QC + Schirokauer + sign
                (p.rational_bound / std::log(static_cast<double>(p.rational_bound))) * 0.15  // ~15% of FB for LP columns + filter loss
            ));

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

    /// 估算需要的关系数量
    [[nodiscard]] size_t estimated_relations_needed() const {
        // 大约需要 rational_count + algebraic_count + excess
        // rational_count ≈ π(B_r) ≈ B_r / ln(B_r)
        double pi_r = rational_bound / std::log(static_cast<double>(rational_bound));
        double pi_a = algebraic_bound / std::log(static_cast<double>(algebraic_bound)) * degree;
        return static_cast<size_t>(pi_r + pi_a + target_excess);
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
