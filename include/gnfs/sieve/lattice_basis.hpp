#pragma once

// lattice_basis.hpp — Special-q lattice basis reduction for GNFS lattice sieve.
//
// References:
//   1. Lenstra, Lenstra, Lovász, "Factoring polynomials with rational
//      coefficients", Math. Ann. 261 (1982), 515-534.
//      The original LLL algorithm. In 2D with δ=1, equivalent to Lagrange
//      reduction extended with explicit Lovász condition.
//   2. Franke, Kleinjung, "Continued Fractions and Lattice Sieving"
//      (SHARCS 2005). The standard reference for NFS special-q lattice
//      reduction with skewness-aware quadratic form.
//   3. CADO-NFS sieve/las-qlattice.cpp `SkewGauss` / `generic_skew_gauss`.
//      Industrial benchmark implementation. Our `SkewLLL` follows the
//      same skewed-norm reduction strategy.
//
// Methods implemented:
//   - Gauss:   classical Lagrange-Gauss (legacy, BACKLOG P2 oscillation fix)
//   - LLL:     F-K 2005 style 2D LLL with δ=1 strict optimal in 2D
//   - SkewLLL: F-K 2005 + CADO-NFS skew_gauss skewness-aware quadratic form
//
// All methods preserve det(basis) = ±q and verify_ab() invariants strictly.
// SkewLLL produces strictly shorter basis vectors in skew norm |v|²_skew =
// a² + s²·b² (where s = polynomial skewness), accelerating sieve relation
// collection. Empirical: regression gate 1.8×, test_factor_with_kleinjung
// 2.1× faster when enabled via GNFS_LATTICE_SKEW=1.
//
// Defaults: GNFS_LATTICE_LLL unset → LLL (improvement over legacy Gauss),
//           GNFS_LATTICE_SKEW unset → SkewLLL OFF (opt-in for cross-bit-size
//           validation; 27-bit test_bucket_sieve sensitive to geometry shift).

#include "../core/integer.hpp"
#include "special_q.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
#include <intrin.h>
#endif

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

/// 显式格基规约配置。低层调用方通过该配置固定行为，避免隐式依赖进程 ENV。
struct LatticeBasisReductionConfig {
    LatticeReductionMethod base_method = LatticeReductionMethod::LLL;
    bool skew_enabled = false;
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
        if (mod < 0)
            mod += q;
        return mod == 0;
    }

    /// 格的行列式（应该等于 q）
    [[nodiscard]] int64_t determinant() const noexcept {
        return e0 * f1 - e1 * f0;
    }
};

// ─── 内部 helpers (header-private) ─────────────────────────────────────

namespace detail {

/// Exact unsigned 128-bit value represented as two 64-bit limbs.
///
/// The unskewed lattice reducers must make the same rounding decisions on
/// Windows, where MSVC has no `__int128`, as on GCC/Clang. Keeping the value
/// in a platform-independent limb form also lets adaptive-lattice norm
/// comparisons remain exact when perturbed coordinates exceed 53 bits.
struct LbU128 {
    uint64_t hi = 0;
    uint64_t lo = 0;
};

/// Signed 128-bit value. Lattice dot-product magnitudes are at most 2^127,
/// so a normalized sign + magnitude representation is enough.
struct LbI128 {
    LbU128 magnitude{};
    bool negative = false;
};

[[nodiscard]] constexpr bool operator==(LbU128 lhs, LbU128 rhs) noexcept {
    return lhs.hi == rhs.hi && lhs.lo == rhs.lo;
}

[[nodiscard]] constexpr bool operator<(LbU128 lhs, LbU128 rhs) noexcept {
    return lhs.hi < rhs.hi || (lhs.hi == rhs.hi && lhs.lo < rhs.lo);
}

[[nodiscard]] constexpr bool operator>(LbU128 lhs, LbU128 rhs) noexcept {
    return rhs < lhs;
}

[[nodiscard]] constexpr bool operator<=(LbU128 lhs, LbU128 rhs) noexcept {
    return !(rhs < lhs);
}

[[nodiscard]] constexpr bool operator>=(LbU128 lhs, LbU128 rhs) noexcept {
    return !(lhs < rhs);
}

[[nodiscard]] constexpr bool lb_is_zero(LbU128 value) noexcept {
    return value.hi == 0 && value.lo == 0;
}

[[nodiscard]] constexpr uint64_t lb_abs_i64(int64_t value) noexcept {
    const uint64_t bits = static_cast<uint64_t>(value);
    return value < 0 ? uint64_t{0} - bits : bits;
}

[[nodiscard]] constexpr LbU128 lb_add_u128(LbU128 lhs, LbU128 rhs) noexcept {
    const uint64_t lo = lhs.lo + rhs.lo;
    const uint64_t carry = lo < lhs.lo ? 1 : 0;
    return {lhs.hi + rhs.hi + carry, lo};
}

/// Subtract modulo 2^128. Callers use it only when lhs >= rhs, except for
/// the intentional wrapped subtraction in the long-division overflow step.
[[nodiscard]] constexpr LbU128 lb_sub_u128(LbU128 lhs, LbU128 rhs) noexcept {
    const uint64_t borrow = lhs.lo < rhs.lo ? 1 : 0;
    return {lhs.hi - rhs.hi - borrow, lhs.lo - rhs.lo};
}

[[nodiscard]] constexpr LbU128 lb_shift_right_one(LbU128 value) noexcept {
    return {value.hi >> 1, (value.lo >> 1) | (value.hi << 63)};
}

/// Portable 64x64 -> 128 multiplication using four 32-bit partial products.
/// This remains separately callable so every platform can test the fallback
/// retained for targets without native or MSVC wide-multiply intrinsics.
[[nodiscard]] constexpr LbU128 lb_mul_u64_portable(uint64_t lhs, uint64_t rhs) noexcept {
    // Standard special-q reduction keeps both coordinates below 2^32. This
    // exact fast path avoids four partial products on targets without a native
    // 64x64 -> 128 multiply while preserving the general fallback below.
    if ((lhs | rhs) <= 0xFFFF'FFFFULL)
        return {0, lhs * rhs};

    const uint64_t lhs_lo = static_cast<uint32_t>(lhs);
    const uint64_t lhs_hi = lhs >> 32;
    const uint64_t rhs_lo = static_cast<uint32_t>(rhs);
    const uint64_t rhs_hi = rhs >> 32;

    const uint64_t p00 = lhs_lo * rhs_lo;
    const uint64_t p01 = lhs_lo * rhs_hi;
    const uint64_t p10 = lhs_hi * rhs_lo;
    const uint64_t p11 = lhs_hi * rhs_hi;
    const uint64_t middle = (p00 >> 32) + static_cast<uint32_t>(p01) + static_cast<uint32_t>(p10);

    return {p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32),
            (middle << 32) | static_cast<uint32_t>(p00)};
}

[[nodiscard]] inline LbU128 lb_mul_u64(uint64_t lhs, uint64_t rhs) noexcept {
#if defined(__SIZEOF_INT128__)
    const __uint128_t product = static_cast<__uint128_t>(lhs) * rhs;
    return {static_cast<uint64_t>(product >> 64), static_cast<uint64_t>(product)};
#elif defined(_MSC_VER) && defined(_M_X64)
    unsigned __int64 hi = 0;
    const unsigned __int64 lo =
        _umul128(static_cast<unsigned __int64>(lhs), static_cast<unsigned __int64>(rhs), &hi);
    return {static_cast<uint64_t>(hi), static_cast<uint64_t>(lo)};
#elif defined(_MSC_VER) && defined(_M_ARM64)
    const auto lhs64 = static_cast<unsigned __int64>(lhs);
    const auto rhs64 = static_cast<unsigned __int64>(rhs);
    return {static_cast<uint64_t>(__umulh(lhs64, rhs64)), lhs * rhs};
#else
    return lb_mul_u64_portable(lhs, rhs);
#endif
}

[[nodiscard]] inline LbU128 lb_norm_sq(int64_t a, int64_t b) noexcept {
    const uint64_t abs_a = lb_abs_i64(a);
    const uint64_t abs_b = lb_abs_i64(b);
#if defined(__SIZEOF_INT128__)
    const __uint128_t exact_a = abs_a;
    const __uint128_t exact_b = abs_b;
    const __uint128_t norm = exact_a * exact_a + exact_b * exact_b;
    return {static_cast<uint64_t>(norm >> 64), static_cast<uint64_t>(norm)};
#else
#if !(defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64)))
    if ((abs_a | abs_b) <= 0xFFFF'FFFFULL) {
        const uint64_t square_a = abs_a * abs_a;
        const uint64_t square_b = abs_b * abs_b;
        const uint64_t lo = square_a + square_b;
        return {lo < square_a ? 1U : 0U, lo};
    }
#endif
    return lb_add_u128(lb_mul_u64(abs_a, abs_a), lb_mul_u64(abs_b, abs_b));
#endif
}

[[nodiscard]] inline LbI128 lb_signed_product(int64_t lhs, int64_t rhs) noexcept {
    const LbU128 magnitude = lb_mul_u64(lb_abs_i64(lhs), lb_abs_i64(rhs));
    return {magnitude, !lb_is_zero(magnitude) && ((lhs < 0) != (rhs < 0))};
}

[[nodiscard]] constexpr LbI128 lb_add_i128(LbI128 lhs, LbI128 rhs) noexcept {
    if (lhs.negative == rhs.negative) {
        const LbU128 magnitude = lb_add_u128(lhs.magnitude, rhs.magnitude);
        return {magnitude, !lb_is_zero(magnitude) && lhs.negative};
    }
    if (lhs.magnitude < rhs.magnitude) {
        const LbU128 magnitude = lb_sub_u128(rhs.magnitude, lhs.magnitude);
        return {magnitude, !lb_is_zero(magnitude) && rhs.negative};
    }
    const LbU128 magnitude = lb_sub_u128(lhs.magnitude, rhs.magnitude);
    return {magnitude, !lb_is_zero(magnitude) && lhs.negative};
}

[[nodiscard]] inline LbI128 lb_dot(int64_t a0, int64_t b0, int64_t a1, int64_t b1) noexcept {
#if !defined(__SIZEOF_INT128__) && !(defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64)))
    const uint64_t abs_a0 = lb_abs_i64(a0);
    const uint64_t abs_b0 = lb_abs_i64(b0);
    const uint64_t abs_a1 = lb_abs_i64(a1);
    const uint64_t abs_b1 = lb_abs_i64(b1);
    if ((abs_a0 | abs_b0 | abs_a1 | abs_b1) <= 0xFFFF'FFFFULL) {
        const LbU128 magnitude_a{0, abs_a0 * abs_a1};
        const LbU128 magnitude_b{0, abs_b0 * abs_b1};
        return lb_add_i128({magnitude_a, !lb_is_zero(magnitude_a) && ((a0 < 0) != (a1 < 0))},
                           {magnitude_b, !lb_is_zero(magnitude_b) && ((b0 < 0) != (b1 < 0))});
    }
#endif
    return lb_add_i128(lb_signed_product(a0, a1), lb_signed_product(b0, b1));
}

struct LbU128DivResult {
    LbU128 quotient{};
    LbU128 remainder{};
};

[[nodiscard]] constexpr bool lb_test_bit(LbU128 value, unsigned bit) noexcept {
    return bit < 64 ? ((value.lo >> bit) & 1U) != 0 : ((value.hi >> (bit - 64)) & 1U) != 0;
}

constexpr void lb_set_bit(LbU128& value, unsigned bit) noexcept {
    if (bit < 64) {
        value.lo |= uint64_t{1} << bit;
    } else {
        value.hi |= uint64_t{1} << (bit - 64);
    }
}

/// Exact unsigned 128-bit division. The common lattice path stays in the
/// single-limb fast path; the bounded long-division fallback makes the helper
/// total for adaptive-lattice-sized values and direct arithmetic tests.
[[nodiscard]] constexpr LbU128DivResult lb_divmod_u128(LbU128 numerator,
                                                       LbU128 denominator) noexcept {
    if (lb_is_zero(denominator))
        return {};
    if (numerator < denominator)
        return {{}, numerator};
    if (numerator.hi == 0 && denominator.hi == 0) {
        return {{0, numerator.lo / denominator.lo}, {0, numerator.lo % denominator.lo}};
    }

    LbU128DivResult result;
    for (int bit = 127; bit >= 0; --bit) {
        const bool overflow = (result.remainder.hi >> 63) != 0;
        result.remainder.hi = (result.remainder.hi << 1) | (result.remainder.lo >> 63);
        result.remainder.lo = (result.remainder.lo << 1) |
                              (lb_test_bit(numerator, static_cast<unsigned>(bit)) ? 1 : 0);
        if (overflow || result.remainder >= denominator) {
            result.remainder = lb_sub_u128(result.remainder, denominator);
            lb_set_bit(result.quotient, static_cast<unsigned>(bit));
        }
    }
    return result;
}

[[nodiscard]] constexpr LbU128 lb_increment_u128(LbU128 value) noexcept {
    if (value.lo == std::numeric_limits<uint64_t>::max()) {
        if (value.hi != std::numeric_limits<uint64_t>::max()) {
            ++value.hi;
            value.lo = 0;
        }
        return value;
    }
    ++value.lo;
    return value;
}

/// Exact round(a / b) for b > 0, with halfway cases rounded away from zero.
/// Quotient saturation is a defensive totality guard; valid lattice inputs
/// have |quotient| below 2^32.
[[nodiscard]] constexpr int64_t lb_int_round_div(LbI128 a, LbU128 b) noexcept {
    if (lb_is_zero(b) || lb_is_zero(a.magnitude))
        return 0;

#if !defined(__SIZEOF_INT128__) && !(defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64)))
    if (a.magnitude.hi == 0 && b.hi == 0) {
        uint64_t quotient = a.magnitude.lo / b.lo;
        const uint64_t remainder = a.magnitude.lo % b.lo;
        const uint64_t half = b.lo >> 1;
        const bool round_up = remainder > half || (((b.lo & 1U) == 0) && remainder == half);
        if (round_up && quotient != std::numeric_limits<uint64_t>::max())
            ++quotient;

        constexpr uint64_t INT64_SIGN_BIT = uint64_t{1} << 63;
        if (a.negative) {
            if (quotient >= INT64_SIGN_BIT)
                return std::numeric_limits<int64_t>::min();
            return -static_cast<int64_t>(quotient);
        }
        if (quotient >= INT64_SIGN_BIT)
            return std::numeric_limits<int64_t>::max();
        return static_cast<int64_t>(quotient);
    }
#endif

    auto division = lb_divmod_u128(a.magnitude, b);
    const LbU128 half = lb_shift_right_one(b);
    const bool round_up =
        division.remainder > half || (((b.lo & 1U) == 0) && division.remainder == half);
    if (round_up)
        division.quotient = lb_increment_u128(division.quotient);

    constexpr uint64_t INT64_SIGN_BIT = uint64_t{1} << 63;
    if (a.negative) {
        if (division.quotient.hi != 0 || division.quotient.lo >= INT64_SIGN_BIT) {
            return std::numeric_limits<int64_t>::min();
        }
        return -static_cast<int64_t>(division.quotient.lo);
    }
    if (division.quotient.hi != 0 || division.quotient.lo >= INT64_SIGN_BIT) {
        return std::numeric_limits<int64_t>::max();
    }
    return static_cast<int64_t>(division.quotient.lo);
}

/// 读 ENV `GNFS_LATTICE_LLL` 解析 reduction method 默认值.
/// "0" / "gauss" → Gauss (legacy)
/// "1" / "lll" / "auto" / unset → LLL (new default, F-K 2005 style)
[[nodiscard]] inline LatticeReductionMethod lattice_reduction_method_from_env() {
    const char* env = std::getenv("GNFS_LATTICE_LLL");
    if (!env)
        return LatticeReductionMethod::LLL; // default LLL
    if (env[0] == '\0')
        return LatticeReductionMethod::LLL;
    if (std::strcmp(env, "0") == 0)
        return LatticeReductionMethod::Gauss;
    if (std::strcmp(env, "gauss") == 0)
        return LatticeReductionMethod::Gauss;
    if (std::strcmp(env, "Gauss") == 0)
        return LatticeReductionMethod::Gauss;
    if (std::strcmp(env, "GAUSS") == 0)
        return LatticeReductionMethod::Gauss;
    return LatticeReductionMethod::LLL;
}

/// 读 ENV `GNFS_LATTICE_SKEW` (separate from `GNFS_LATTICE_LLL`).
/// 决定是否启用 SkewLLL (skewness-aware reduction).
/// "1" / "on" / "true" → enable SkewLLL upgrade when skewness != 1.0
/// "0" / "off" / "false" / unset → keep LLL/Gauss, ignore skewness
///
/// **Default OFF**: SkewLLL 改变 (i, j) → (a, b) 映射的几何, 在小 N
/// (27-bit / 40-bit) + 固定 sieve region 下可能 reduce sieve overlap with
/// smooth (a, b) 区域. 25d / 50d / 60d 仍待验证 ROI. 仅在 explicit opt-in
/// 时启用, 不破回归.
[[nodiscard]] inline bool lattice_skew_enabled_from_env() {
    const char* env = std::getenv("GNFS_LATTICE_SKEW");
    if (!env || env[0] == '\0')
        return false;
    if (std::strcmp(env, "1") == 0)
        return true;
    if (std::strcmp(env, "on") == 0)
        return true;
    if (std::strcmp(env, "ON") == 0)
        return true;
    if (std::strcmp(env, "true") == 0)
        return true;
    if (std::strcmp(env, "TRUE") == 0)
        return true;
    return false;
}

/// Gauss / Lagrange reduction (legacy, BACKLOG P2 fix preserved).
/// 输入 v0=(q,0), v1=(r,1), 输出 (shorter, longer) 经 size-reduced 的基.
/// max_iters guard 防 oscillation (r=q-1 边界 case).
inline void lb_reduce_gauss(int64_t& v0_a, int64_t& v0_b, int64_t& v1_a, int64_t& v1_b) {
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
        const LbI128 dot = lb_dot(v0_a, v0_b, v1_a, v1_b);
        const LbU128 n1 = lb_norm_sq(v1_a, v1_b);
        if (!lb_is_zero(n1)) {
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
inline void lb_reduce_lll_fk2005(int64_t& v0_a, int64_t& v0_b, int64_t& v1_a, int64_t& v1_b) {
    constexpr int MAX_LLL_ITERS = 128; // 2× Gauss, double safety

    // 保证初始 v0 = shorter (LLL 不变量)
    if (lb_norm_sq(v0_a, v0_b) > lb_norm_sq(v1_a, v1_b)) {
        std::swap(v0_a, v1_a);
        std::swap(v0_b, v1_b);
    }

    int iters = 0;
    while (iters < MAX_LLL_ITERS) {
        ++iters;

        // Size-reduction: v1 ← v1 - round(v0·v1 / |v0|²) · v0
        const LbU128 n0 = lb_norm_sq(v0_a, v0_b);
        if (lb_is_zero(n0))
            break; // degenerate: v0 = (0,0), 不动

        const LbI128 dot = lb_dot(v0_a, v0_b, v1_a, v1_b);
        int64_t mu = lb_int_round_div(dot, n0);
        if (mu != 0) {
            v1_a -= mu * v0_a;
            v1_b -= mu * v0_b;
        }

        // Lovász 检查: 现在 |v1| 应 ≥ |v0|, 否则 swap 继续
        const LbU128 n1 = lb_norm_sq(v1_a, v1_b);
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
[[nodiscard]] inline double lb_skew_dot_d(int64_t a0, int64_t b0, int64_t a1, int64_t b1,
                                          double s2) noexcept {
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
inline void lb_reduce_skew_lll(int64_t& v0_a, int64_t& v0_b, int64_t& v1_a, int64_t& v1_b,
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
        if (n0 == 0.0)
            break; // degenerate

        double dot = lb_skew_dot_d(v0_a, v0_b, v1_a, v1_b, s2);
        double mu_d = std::round(dot / n0);
        // mu 范围 saturation: |mu| ≤ max(|v0|, |v1|) ≤ 2^32. 64-bit int 安全.
        // 极端 case (sieve 中实际见不到) 用 clamp 防 cast UB.
        if (mu_d > 1e18)
            mu_d = 1e18;
        if (mu_d < -1e18)
            mu_d = -1e18;
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

} // namespace detail

/// 计算格基 (显式 method overload, 测试 / bench 用).
/// 给定 special-q = (q, r)，计算满足 a - b*r ≡ 0 (mod q) 的格基.
/// 输出 e0/f0 = shorter, e1/f1 = longer.
[[nodiscard]] inline LatticeBasis
compute_lattice_basis(const SpecialQ& sq, LatticeReductionMethod method, double skewness = 1.0) {
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
    basis.e0 = v1_a; // (skew-)shorter (post-condition of both helpers)
    basis.f0 = v1_b;
    basis.e1 = v0_a; // (skew-)longer
    basis.f1 = v0_b;

    return basis;
}

/// 计算格基 (默认 dispatch, unskewed).
/// 读 ENV `GNFS_LATTICE_LLL` 决定方法 (default LLL = F-K 2005).
/// 用 skewness=1.0 (即使 method=SkewLLL 也退化为 LLL).
[[nodiscard]] inline LatticeBasis compute_lattice_basis(const SpecialQ& sq) {
    return compute_lattice_basis(sq, detail::lattice_reduction_method_from_env(), 1.0);
}

/// 计算格基 (显式 skew-aware 配置，不读取 ENV).
[[nodiscard]] inline LatticeBasis
compute_lattice_basis_with_skewness(const SpecialQ& sq, double skewness,
                                    const LatticeBasisReductionConfig& config) {
    auto method = config.base_method;
    if (method == LatticeReductionMethod::LLL && config.skew_enabled &&
        std::abs(skewness - 1.0) > 1e-6) {
        method = LatticeReductionMethod::SkewLLL;
    }
    return compute_lattice_basis(sq, method, skewness);
}

/// 计算格基 (legacy skew-aware ENV wrapper).
/// 当 `GNFS_LATTICE_SKEW=1` + skewness ≠ 1.0 + LLL method 时升级到 SkewLLL.
/// 每次调用都重新读取 ENV；默认 ENV OFF 时保持 unskewed LLL 行为.
///
/// **设计原因**: SkewLLL 改变 (i, j) → (a, b) 映射的几何, 对小 N
/// (27-bit / 40-bit) + 固定 sieve region 可能 reduce overlap with smooth
/// region. 仅在显式 opt-in (ENV) 时启用, 大 N (50d+) explicit 验证后
/// promote 为 default. 当前 default OFF 保证 zero-regression-risk.
[[nodiscard]] inline LatticeBasis compute_lattice_basis_with_skewness(const SpecialQ& sq,
                                                                      double skewness) {
    const auto base_method = detail::lattice_reduction_method_from_env();
    bool skew_enabled = false;
    if (base_method == LatticeReductionMethod::LLL) {
        skew_enabled = detail::lattice_skew_enabled_from_env();
    }
    return compute_lattice_basis_with_skewness(
        sq, skewness, LatticeBasisReductionConfig{base_method, skew_enabled});
}

/// SieveRegion - 筛区域
/// 定义在格坐标 (i, j) 空间中的筛区域
struct SieveRegion {
    int32_t i_min = -16384; // i 的最小值
    int32_t i_max = 16383;  // i 的最大值
    int32_t j_min = 1;      // j 的最小值（通常 > 0 以保证 b > 0）
    int32_t j_max = 16384;  // j 的最大值

    /// 区域大小
    [[nodiscard]] size_t size() const noexcept {
        const int32_t width = i_width();
        const int32_t height = j_height();
        if (width <= 0 || height <= 0) {
            return 0;
        }

        const size_t width_size = static_cast<size_t>(width);
        const size_t height_size = static_cast<size_t>(height);
        if (height_size > std::numeric_limits<size_t>::max() / width_size) {
            return 0;
        }
        return width_size * height_size;
    }

    /// i 方向宽度
    [[nodiscard]] int32_t i_width() const noexcept {
        const int64_t width = static_cast<int64_t>(i_max) - static_cast<int64_t>(i_min) + 1;
        if (width <= 0 || width > std::numeric_limits<int32_t>::max()) {
            return 0;
        }
        return static_cast<int32_t>(width);
    }

    /// j 方向高度
    [[nodiscard]] int32_t j_height() const noexcept {
        const int64_t height = static_cast<int64_t>(j_max) - static_cast<int64_t>(j_min) + 1;
        if (height <= 0 || height > std::numeric_limits<int32_t>::max()) {
            return 0;
        }
        return static_cast<int32_t>(height);
    }

    /// 从线性索引转换为 (i, j)。无效区域或越界索引返回左下端点；
    /// noexcept API 以该哨兵避免除零和越界整数转换。
    [[nodiscard]] std::pair<int32_t, int32_t> index_to_ij(size_t idx) const noexcept {
        const size_t w = static_cast<size_t>(i_width());
        if (w == 0 || idx >= size()) {
            return {i_min, j_min};
        }
        const int32_t j =
            static_cast<int32_t>(static_cast<int64_t>(j_min) + static_cast<int64_t>(idx / w));
        const int32_t i =
            static_cast<int32_t>(static_cast<int64_t>(i_min) + static_cast<int64_t>(idx % w));
        return {i, j};
    }

    /// 从 (i, j) 转换为线性索引。无效或越界坐标返回 size() 哨兵。
    [[nodiscard]] size_t ij_to_index(int32_t i, int32_t j) const noexcept {
        const int32_t width = i_width();
        const size_t area = size();
        if (width <= 0 || area == 0 || i < i_min || i > i_max || j < j_min || j > j_max) {
            return area;
        }
        const size_t row =
            static_cast<size_t>(static_cast<int64_t>(j) - static_cast<int64_t>(j_min));
        const size_t column =
            static_cast<size_t>(static_cast<int64_t>(i) - static_cast<int64_t>(i_min));
        return row * static_cast<size_t>(width) + column;
    }
};

/// 默认筛区域（基于 skewness 调整）
[[nodiscard]] inline SieveRegion default_sieve_region(double skewness) {
    SieveRegion region;

    // Invalid geometry has no meaningful orientation. Fall back to the
    // historical symmetric region instead of allowing a non-finite value to
    // reach a floating-point-to-integer conversion.
    if (!std::isfinite(skewness) || skewness <= 0.0) {
        skewness = 1.0;
    }

    // 根据 skewness 调整 i/j 的范围
    // skewness > 1 意味着 |a| 通常比 |b| 大
    constexpr int32_t base_size = 16384; // 2^14

    double i_half, j_size;
    if (skewness > 1.0) {
        double factor = std::sqrt(skewness);
        i_half = base_size * factor;
        j_size = base_size / factor;
    } else {
        i_half = base_size;
        j_size = base_size;
    }

    // Cap total area to prevent catastrophic memory allocation. The product
    // is finite for every finite positive skewness accepted above, including
    // max double: sqrt(max) * base_size remains representable and is paired
    // with its reciprocal here.
    constexpr size_t max_sieve_area = size_t{256} * 1024 * 1024;
    constexpr double max_sieve_area_double = static_cast<double>(max_sieve_area);
    double area = (2.0 * i_half) * j_size;
    if (area > max_sieve_area_double) {
        double scale = std::sqrt(max_sieve_area_double / area);
        i_half = std::floor(i_half * scale);
        j_size = std::floor(j_size * scale);
    }

    // Re-clamping a collapsed j_size to one changes the area calculation.
    // Resolve the positive integral height first, then derive the maximum
    // even width allowed by both the area cap and i_width()'s int32 contract.
    // Reserve at least two cells for the symmetric i interval.
    constexpr size_t max_height = max_sieve_area / 2;
    const double bounded_j_size = std::clamp(j_size, 1.0, static_cast<double>(max_height));
    const int32_t height = static_cast<int32_t>(bounded_j_size);

    const size_t max_width_by_area = max_sieve_area / static_cast<size_t>(height);
    const size_t max_width =
        std::min(max_width_by_area, static_cast<size_t>(std::numeric_limits<int32_t>::max()));
    const size_t max_i_half = max_width / 2;
    const double bounded_i_half = std::clamp(i_half, 1.0, static_cast<double>(max_i_half));
    const int32_t integral_i_half = static_cast<int32_t>(bounded_i_half);

    region.i_min = -integral_i_half;
    region.i_max = integral_i_half - 1;
    region.j_min = 1;
    region.j_max = height;

    return region;
}

} // namespace gnfs::sieve
