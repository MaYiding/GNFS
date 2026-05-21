#pragma once

#include "../core/integer.hpp"
#include "special_q.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace gnfs::sieve {

using core::Integer;

/// LatticeReductionMethod - 格基规约方法
/// Gauss: 经典 Gaussian (Lagrange) reduction (legacy default).
/// LLL: Franke-Kleinjung 2005 风格 2D LLL — 严格 size-reduced + Lovász δ=1
///      enforcement, 单遍 Lagrange-Gauss + 双向 reduce. 在 2D 中等价 LLL optimal.
/// SkewLLL: F-K 2005 + skewness 加权 quadratic form (CADO-NFS skew_gauss style).
///      用 |v|²_skew = a² + s²·b² (s = polynomial skewness) 做 reduce target,
///      产出的 basis 在 (i, j) → (a, b) 映射后, sieve region 中 a/b 分布
///      更均匀, 对 skewness ≠ 1 的 polynomial 显著改善 sieve quality.
///      Skewness=1.0 时退化为 LLL.
enum class LatticeReductionMethod {
    Gauss,
    LLL,
    SkewLLL,
};

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

// ─── 内部 helpers (header-private) ─────────────────────────────────────

namespace detail {

/// __int128_t norm² (避免 q² > 2^63 溢出 — q max 2^32 → q² max 2^64).
[[nodiscard]] inline __int128_t lb_norm_sq(int64_t a, int64_t b) noexcept {
    __int128_t a128 = a, b128 = b;
    return a128 * a128 + b128 * b128;
}

/// 精确整数 round-half-to-even-like: round(a/b) for b > 0.
/// 用 (2a + b)/(2b) for a≥0, (2a - b)/(2b) for a<0 (round-half-away-from-zero).
[[nodiscard]] inline int64_t lb_int_round_div(__int128_t a, __int128_t b) noexcept {
    if (b <= 0) return 0;  // safety
    if (a >= 0) {
        return static_cast<int64_t>((2 * a + b) / (2 * b));
    }
    return static_cast<int64_t>((2 * a - b) / (2 * b));
}

/// 读 ENV `GNFS_LATTICE_LLL` 解析 reduction method 默认值.
/// "0" / "gauss" → Gauss (legacy)
/// "1" / "lll" / "auto" / unset → LLL (new default, F-K 2005 style)
[[nodiscard]] inline LatticeReductionMethod lattice_reduction_method_from_env() {
    const char* env = std::getenv("GNFS_LATTICE_LLL");
    if (!env) return LatticeReductionMethod::LLL;  // default LLL
    if (env[0] == '\0') return LatticeReductionMethod::LLL;
    if (std::strcmp(env, "0") == 0) return LatticeReductionMethod::Gauss;
    if (std::strcmp(env, "gauss") == 0) return LatticeReductionMethod::Gauss;
    if (std::strcmp(env, "Gauss") == 0) return LatticeReductionMethod::Gauss;
    if (std::strcmp(env, "GAUSS") == 0) return LatticeReductionMethod::Gauss;
    return LatticeReductionMethod::LLL;
}

/// Gauss / Lagrange reduction (legacy, BACKLOG P2 fix preserved).
/// 输入 v0=(q,0), v1=(r,1), 输出 (shorter, longer) 经 size-reduced 的基.
/// max_iters guard 防 oscillation (r=q-1 边界 case).
inline void lb_reduce_gauss(int64_t& v0_a, int64_t& v0_b,
                             int64_t& v1_a, int64_t& v1_b) {
    constexpr int MAX_GAUSSIAN_ITERS = 64;
    bool changed = true;
    int iters = 0;
    while (changed && iters < MAX_GAUSSIAN_ITERS) {
        changed = false;
        ++iters;

        // 确保 v0 是较长的
        if (lb_norm_sq(v0_a, v0_b) < lb_norm_sq(v1_a, v1_b)) {
            std::swap(v0_a, v1_a);
            std::swap(v0_b, v1_b);
        }

        // v0 = v0 - round(v0·v1 / v1·v1) * v1
        __int128_t dot = static_cast<__int128_t>(v0_a) * v1_a +
                         static_cast<__int128_t>(v0_b) * v1_b;
        __int128_t n1 = lb_norm_sq(v1_a, v1_b);
        if (n1 > 0) {
            int64_t mu = lb_int_round_div(dot, n1);
            if (mu != 0) {
                v0_a -= mu * v1_a;
                v0_b -= mu * v1_b;
                changed = true;
            }
        }
    }

    // 最终确保 v1 是较短的（用作主要遍历方向）
    if (lb_norm_sq(v0_a, v0_b) < lb_norm_sq(v1_a, v1_b)) {
        std::swap(v0_a, v1_a);
        std::swap(v0_b, v1_b);
    }
}

/// LLL (Franke-Kleinjung 2005 风格) reduction for 2D lattice.
/// 严格 size-reduced + Lovász δ=1 enforcement (2D 中 δ=1 严格 optimal).
///
/// 算法 (单遍 Lagrange-Gauss + 双向 reduce):
///   repeat:
///     1. 保证 |v0|² ≤ |v1|² (swap if needed)
///     2. 计算 μ = round(v0·v1 / |v0|²) (用较短的 v0 reduce v1)
///     3. v1 ← v1 - μ·v0
///     4. 若 |v1|² < |v0|²: swap (Lovász 违反 — v1 现在更短)
///     5. 否则: STOP — size-reduced + |v1| ≥ |v0| (Lovász 满足)
///
/// 与 Gauss 区别:
///   - 双向 reduce: Gauss 只 reduce longer with shorter, LLL 维护 v0 = shorter
///     不变量, 用 v0 reduce v1.
///   - Lovász 显式检查 swap 后 |v1| < |v0|, 不止 iterate.
///   - 2D 中数学上 LLL δ=1 = 真正 optimal (vs Gauss δ=3/4 弱).
///
/// 复杂度 O(log(max(q,r))) iterations, 每 iter O(1) integer ops.
/// max_iters guard 防极端 oscillation (理论上 2D LLL 不会, 但安全网).
inline void lb_reduce_lll_fk2005(int64_t& v0_a, int64_t& v0_b,
                                   int64_t& v1_a, int64_t& v1_b) {
    constexpr int MAX_LLL_ITERS = 128;  // 2× Gauss, double safety

    // 保证初始 v0 = shorter (LLL 不变量)
    if (lb_norm_sq(v0_a, v0_b) > lb_norm_sq(v1_a, v1_b)) {
        std::swap(v0_a, v1_a);
        std::swap(v0_b, v1_b);
    }

    int iters = 0;
    while (iters < MAX_LLL_ITERS) {
        ++iters;

        // Size-reduction: v1 ← v1 - round(v0·v1 / |v0|²) · v0
        __int128_t n0 = lb_norm_sq(v0_a, v0_b);
        if (n0 == 0) break;  // degenerate: v0 = (0,0), 不动

        __int128_t dot = static_cast<__int128_t>(v0_a) * v1_a +
                         static_cast<__int128_t>(v0_b) * v1_b;
        int64_t mu = lb_int_round_div(dot, n0);
        if (mu != 0) {
            v1_a -= mu * v0_a;
            v1_b -= mu * v0_b;
        }

        // Lovász 检查: 现在 |v1| 应 ≥ |v0|, 否则 swap 继续
        __int128_t n1 = lb_norm_sq(v1_a, v1_b);
        if (n1 >= n0) {
            // Lovász 满足 + size-reduced → 终止
            break;
        }
        // |v1| < |v0|, swap 后继续 reduce
        std::swap(v0_a, v1_a);
        std::swap(v0_b, v1_b);
    }

    // 最终 swap: v0 应是 shorter (LLL 不变量保持). 此处 swap 与 Gauss 路径
    // 一致 — Gauss 把 longer 放 v0, LLL 把 shorter 放 v0, callers 期待
    // (e0, f0) = shorter (= LLL v0 = Gauss v1). 故 caller 端约定:
    //   e0 = v1 (Gauss longer-then-swap), e0 = v0 (LLL).
    // 为了 caller API 完全一致, 这里 swap 后让 v1 = shorter (= e0 source).
    if (lb_norm_sq(v0_a, v0_b) < lb_norm_sq(v1_a, v1_b)) {
        std::swap(v0_a, v1_a);
        std::swap(v0_b, v1_b);
    }
    // 后置 invariant: v1 = shorter, v0 = longer (与 Gauss 路径一致).
}

/// Skew-aware quadratic form q(v) = a² + s²·b² where s = polynomial skewness.
///
/// 实现策略 (避免 __int128_t 溢出, 同时保持精度):
/// 用 double 算 skew norm² + skew dot product, 用 std::round() 取整 mu.
/// basis vectors 本身仍是 int64_t, 只有 reduction direction 由 skew quad form 决定.
///
/// 精度分析:
///   a, b 范围 |a|, |b| ≤ q (initial), 经 LLL 后通常 |a|, |b| ≤ √(s·q) (典型).
///   a² ≤ q² ~ 2^64, s²·b² 同 order. double mantissa 53 bits — 对 q ≤ 2^25
///   (~3e7, 60d range) 精度严格. 对 q ~ 2^30 (上界), 累积误差 ≤ 几 ULP,
///   不影响 round-half tie 决策 (LLL 容忍 small mu round error, only 影响
///   convergence 速度, 不影响 final basis validity — 因 size-reduced + Lovász
///   都用 double 比较自一致).
///
/// 后备: 若 s = 1.0, 直接 dispatch 到 unskewed lb_reduce_lll_fk2005 (bit-exact).

/// double-precision skew norm² (overflow-safe for q ≤ 2^30 typical).
[[nodiscard]] inline double lb_skew_norm_sq_d(int64_t a, int64_t b, double s2) noexcept {
    double da = static_cast<double>(a);
    double db = static_cast<double>(b);
    return da * da + s2 * db * db;
}

/// double-precision skew dot product.
[[nodiscard]] inline double lb_skew_dot_d(
        int64_t a0, int64_t b0, int64_t a1, int64_t b1, double s2) noexcept {
    double da0 = static_cast<double>(a0), db0 = static_cast<double>(b0);
    double da1 = static_cast<double>(a1), db1 = static_cast<double>(b1);
    return da0 * da1 + s2 * db0 * db1;
}

/// Skew-aware LLL (F-K 2005 + CADO-NFS skew_gauss style).
/// 算法与 lb_reduce_lll_fk2005 相同, 但 norm² 和 dot 用 skew-quadratic form q(v) = a² + s²·b².
///
/// 关键 reduction: mu = round(<v0,v1>_skew / |v0|²_skew). 选 mu 后
///   v1 ← v1 - mu·v0 (basis vector 仍是整数 update)
///
/// 数学不变量保持: det(basis) = ±q 严格 (因 basis update 是 integer 线性变换),
/// verify_ab() 严格 (因 a-b·r ≡ 0 mod q 在 integer 线性变换下保持).
/// "size-reduced" 和 "Lovász" 在 skew quad form 下成立 (double 误差不影响有效性).
///
/// Skewness=1.0 时输出可能与 unskewed LLL 不 bit-identical (因 double rounding),
/// 但 caller 应直接 dispatch 到 unskewed path 在 s=1.0 case.
inline void lb_reduce_skew_lll(int64_t& v0_a, int64_t& v0_b,
                                 int64_t& v1_a, int64_t& v1_b,
                                 double skewness) {
    constexpr int MAX_LLL_ITERS = 128;
    const double s2 = skewness * skewness;

    // 保证初始 v0 = skew-shorter
    if (lb_skew_norm_sq_d(v0_a, v0_b, s2) > lb_skew_norm_sq_d(v1_a, v1_b, s2)) {
        std::swap(v0_a, v1_a);
        std::swap(v0_b, v1_b);
    }

    int iters = 0;
    while (iters < MAX_LLL_ITERS) {
        ++iters;

        double n0 = lb_skew_norm_sq_d(v0_a, v0_b, s2);
        if (n0 == 0.0) break;  // degenerate

        double dot = lb_skew_dot_d(v0_a, v0_b, v1_a, v1_b, s2);
        double mu_d = std::round(dot / n0);
        // mu 范围 saturation: |mu| ≤ max(|v0|, |v1|) ≤ 2^32. 64-bit int 安全.
        // 极端 case (sieve 中实际见不到) 用 clamp 防 cast UB.
        if (mu_d > 1e18) mu_d = 1e18;
        if (mu_d < -1e18) mu_d = -1e18;
        int64_t mu = static_cast<int64_t>(mu_d);
        if (mu != 0) {
            v1_a -= mu * v0_a;
            v1_b -= mu * v0_b;
        }

        // Lovász (skew): |v1|²_skew ≥ |v0|²_skew?
        double n1 = lb_skew_norm_sq_d(v1_a, v1_b, s2);
        if (n1 >= n0) {
            break;
        }
        std::swap(v0_a, v1_a);
        std::swap(v0_b, v1_b);
    }

    // 后置 swap: v1 = skew-shorter (一致约定)
    if (lb_skew_norm_sq_d(v0_a, v0_b, s2) < lb_skew_norm_sq_d(v1_a, v1_b, s2)) {
        std::swap(v0_a, v1_a);
        std::swap(v0_b, v1_b);
    }
}

}  // namespace detail

/// 计算格基 (显式 method overload, 测试 / bench 用).
/// 给定 special-q = (q, r)，计算满足 a - b*r ≡ 0 (mod q) 的格基.
/// 输出 e0/f0 = shorter, e1/f1 = longer.
[[nodiscard]] inline LatticeBasis compute_lattice_basis(const SpecialQ& sq,
                                                         LatticeReductionMethod method,
                                                         double skewness = 1.0) {
    LatticeBasis basis;
    basis.q = sq.q;
    basis.r = sq.r;

    int64_t q64 = static_cast<int64_t>(sq.q);
    int64_t r64 = static_cast<int64_t>(sq.r);

    // 初始基: v0 = (q, 0), v1 = (r, 1) 都满足 a ≡ b·r (mod q).
    int64_t v0_a = q64, v0_b = 0;
    int64_t v1_a = r64, v1_b = 1;

    switch (method) {
        case LatticeReductionMethod::Gauss:
            detail::lb_reduce_gauss(v0_a, v0_b, v1_a, v1_b);
            break;
        case LatticeReductionMethod::LLL:
            detail::lb_reduce_lll_fk2005(v0_a, v0_b, v1_a, v1_b);
            break;
        case LatticeReductionMethod::SkewLLL: {
            // Skewness=1.0 退化为 unskewed LLL (bit-exact path).
            if (std::abs(skewness - 1.0) < 1e-9) {
                detail::lb_reduce_lll_fk2005(v0_a, v0_b, v1_a, v1_b);
            } else {
                detail::lb_reduce_skew_lll(v0_a, v0_b, v1_a, v1_b, skewness);
            }
            break;
        }
    }

    // Caller 约定: e0/f0 是较短的 (sieve i-axis primary).
    // Note for SkewLLL: shorter 按 skew-norm 比, 不是 Euclidean norm.
    basis.e0 = v1_a;  // (skew-)shorter (post-condition of both helpers)
    basis.f0 = v1_b;
    basis.e1 = v0_a;  // (skew-)longer
    basis.f1 = v0_b;

    return basis;
}

/// 计算格基 (默认 dispatch, unskewed).
/// 读 ENV `GNFS_LATTICE_LLL` 决定方法 (default LLL = F-K 2005).
/// 用 skewness=1.0 (即使 method=SkewLLL 也退化为 LLL).
[[nodiscard]] inline LatticeBasis compute_lattice_basis(const SpecialQ& sq) {
    return compute_lattice_basis(sq, detail::lattice_reduction_method_from_env(), 1.0);
}

/// 计算格基 (skew-aware overload).
/// 当 skewness != 1.0 + ENV 启用 LLL/auto 时, 自动用 SkewLLL.
/// ENV `GNFS_LATTICE_LLL=0` (Gauss) skewness 忽略 (legacy 行为).
[[nodiscard]] inline LatticeBasis compute_lattice_basis_with_skewness(
        const SpecialQ& sq, double skewness) {
    auto method = detail::lattice_reduction_method_from_env();
    // 若 method 是 LLL 且 skewness 显著 ≠ 1.0, 升级为 SkewLLL
    if (method == LatticeReductionMethod::LLL && std::abs(skewness - 1.0) > 1e-6) {
        method = LatticeReductionMethod::SkewLLL;
    }
    return compute_lattice_basis(sq, method, skewness);
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
    i_half = std::max(std::min(i_half, MAX_I_HALF), 1.0);
    j_size = std::max(std::min(j_size, MAX_J_SIZE), 1.0);

    region.i_min = -static_cast<int32_t>(i_half);
    region.i_max = static_cast<int32_t>(i_half) - 1;
    region.j_min = 1;
    region.j_max = static_cast<int32_t>(j_size);

    return region;
}

} // namespace gnfs::sieve
