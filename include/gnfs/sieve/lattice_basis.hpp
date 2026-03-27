#pragma once

#include "../core/integer.hpp"
#include "special_q.hpp"

#include <array>
#include <cmath>
#include <cstdint>

namespace gnfs {
namespace sieve {

using core::Integer;

/// LatticeBasis - 格基
/// 格 L_q = {(a, b) : a - b*r ≡ 0 (mod q)} (GNFS convention)
/// For prime ideal P = (q, α - r), P | (a - bα) iff a - b*r ≡ 0 (mod q)
/// 由两个向量 (e0, f0) 和 (e1, f1) 生成
struct LatticeBasis {
    // 第一个基向量
    int64_t e0 = 0;
    int64_t f0 = 0;

    // 第二个基向量
    int64_t e1 = 0;
    int64_t f1 = 0;

    // special-q 的参数
    uint32_t q = 0;
    uint32_t r = 0;

    /// 从格坐标 (i, j) 转换为原始坐标 (a, b)
    /// a = i * e0 + j * e1
    /// b = i * f0 + j * f1
    [[nodiscard]] std::pair<int64_t, int64_t> to_ab(int32_t i, int32_t j) const noexcept {
        int64_t a = static_cast<int64_t>(i) * e0 + static_cast<int64_t>(j) * e1;
        int64_t b = static_cast<int64_t>(i) * f0 + static_cast<int64_t>(j) * f1;
        return {a, b};
    }

    /// 检查 (a, b) 是否满足 a - b*r ≡ 0 (mod q) (GNFS convention)
    [[nodiscard]] bool verify_ab(int64_t a, int64_t b) const noexcept {
        // (a - b*r) mod q should be 0
        int64_t val = a - static_cast<int64_t>(b) * r;
        int64_t mod = val % static_cast<int64_t>(q);
        if (mod < 0) mod += q;
        return mod == 0;
    }

    /// 格的行列式（应该等于 q）
    [[nodiscard]] int64_t determinant() const noexcept {
        return e0 * f1 - e1 * f0;
    }
};

/// 计算格基
/// 给定 special-q = (q, r)，计算满足 a - b*r ≡ 0 (mod q) 的格基 (GNFS convention)
/// For prime ideal P = (q, α - r), P | (a - bα) iff a - b*r ≡ 0 (mod q)
/// 使用扩展欧几里得算法
[[nodiscard]] inline LatticeBasis compute_lattice_basis(const SpecialQ& sq) {
    LatticeBasis basis;
    basis.q = sq.q;
    basis.r = sq.r;

    // 格 L = {(a, b) : a - b*r ≡ 0 (mod q)} (GNFS convention)
    // 等价于 a ≡ b*r (mod q)
    //
    // 初始基向量:
    //   (q, 0) - 因为 a = q, b = 0 满足 q - 0*r = q ≡ 0 (mod q)
    //   (r, 1) - 因为 a = r, b = 1 满足 r - 1*r = 0 ≡ 0 (mod q)
    //
    // 使用 LLL 简化可以得到更短的基向量，但这里先用简单版本

    int64_t q64 = static_cast<int64_t>(sq.q);
    int64_t r64 = static_cast<int64_t>(sq.r);

    // 简单的高斯格基规约
    // 初始: v0 = (q, 0), v1 = (r, 1)
    int64_t v0_a = q64, v0_b = 0;
    int64_t v1_a = r64, v1_b = 1;

    // 确保 |v0| >= |v1|（按长度）
    auto norm_sq = [](int64_t a, int64_t b) -> double {
        return static_cast<double>(a) * a + static_cast<double>(b) * b;
    };

    // 高斯规约
    bool changed = true;
    while (changed) {
        changed = false;

        // 确保 v0 是较长的
        if (norm_sq(v0_a, v0_b) < norm_sq(v1_a, v1_b)) {
            std::swap(v0_a, v1_a);
            std::swap(v0_b, v1_b);
        }

        // v0 = v0 - round(v0·v1 / v1·v1) * v1
        double dot = static_cast<double>(v0_a) * v1_a + static_cast<double>(v0_b) * v1_b;
        double n1 = norm_sq(v1_a, v1_b);
        if (n1 > 0) {
            int64_t mu = static_cast<int64_t>(std::round(dot / n1));
            if (mu != 0) {
                v0_a -= mu * v1_a;
                v0_b -= mu * v1_b;
                changed = true;
            }
        }
    }

    // 最终确保 v1 是较短的（用作主要遍历方向）
    if (norm_sq(v0_a, v0_b) < norm_sq(v1_a, v1_b)) {
        std::swap(v0_a, v1_a);
        std::swap(v0_b, v1_b);
    }

    basis.e0 = v1_a;  // 较短的向量
    basis.f0 = v1_b;
    basis.e1 = v0_a;  // 较长的向量
    basis.f1 = v0_b;

    return basis;
}

/// SieveRegion - 筛区域
/// 定义在格坐标 (i, j) 空间中的筛区域
struct SieveRegion {
    int32_t i_min = -16384;     // i 的最小值
    int32_t i_max = 16383;      // i 的最大值
    int32_t j_min = 1;          // j 的最小值（通常 > 0 以保证 b > 0）
    int32_t j_max = 16384;      // j 的最大值

    /// 区域大小
    [[nodiscard]] size_t size() const noexcept {
        return static_cast<size_t>(i_max - i_min + 1) *
               static_cast<size_t>(j_max - j_min + 1);
    }

    /// i 方向宽度
    [[nodiscard]] int32_t i_width() const noexcept {
        return i_max - i_min + 1;
    }

    /// j 方向高度
    [[nodiscard]] int32_t j_height() const noexcept {
        return j_max - j_min + 1;
    }

    /// 从线性索引转换为 (i, j)
    [[nodiscard]] std::pair<int32_t, int32_t> index_to_ij(size_t idx) const noexcept {
        int32_t w = i_width();
        int32_t j = static_cast<int32_t>(idx / w) + j_min;
        int32_t i = static_cast<int32_t>(idx % w) + i_min;
        return {i, j};
    }

    /// 从 (i, j) 转换为线性索引
    [[nodiscard]] size_t ij_to_index(int32_t i, int32_t j) const noexcept {
        return static_cast<size_t>(j - j_min) * i_width() +
               static_cast<size_t>(i - i_min);
    }
};

/// 默认筛区域（基于 skewness 调整）
[[nodiscard]] inline SieveRegion default_sieve_region(double skewness) {
    SieveRegion region;

    // 根据 skewness 调整 i/j 的范围
    // skewness > 1 意味着 |a| 通常比 |b| 大
    int32_t base_size = 16384;  // 2^14

    double i_half, j_size;
    if (skewness > 1.0) {
        double factor = std::sqrt(skewness);
        i_half = base_size * factor;
        j_size = base_size / factor;
    } else {
        i_half = base_size;
        j_size = base_size;
    }

    // Cap total area to prevent catastrophic memory allocation
    constexpr double MAX_SIEVE_AREA = 256.0 * 1024 * 1024;
    double area = (2.0 * i_half) * j_size;
    if (area > MAX_SIEVE_AREA) {
        double scale = std::sqrt(MAX_SIEVE_AREA / area);
        i_half = std::floor(i_half * scale);
        j_size = std::floor(j_size * scale);
    }

    // Clamp to int32 range to prevent overflow on extreme skewness
    constexpr double MAX_I_HALF = static_cast<double>(INT32_MAX - 1);
    constexpr double MAX_J_SIZE = static_cast<double>(INT32_MAX - 1);
    i_half = std::min(i_half, MAX_I_HALF);
    j_size = std::max(std::min(j_size, MAX_J_SIZE), 1.0);

    region.i_min = -static_cast<int32_t>(i_half);
    region.i_max = static_cast<int32_t>(i_half) - 1;
    region.j_min = 1;
    region.j_max = static_cast<int32_t>(j_size);

    return region;
}

} // namespace sieve
} // namespace gnfs
