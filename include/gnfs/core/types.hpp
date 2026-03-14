#pragma once

#include <cstdint>
#include <functional>

namespace gnfs {
namespace core {

/// ABPair - 关系的原子单位
/// 表示 (a, b) 对，其中 a + b*m 在有理侧，a + b*alpha 在代数侧
struct ABPair {
    int64_t  a;
    uint64_t b;  // b > 0 always

    constexpr ABPair() noexcept : a(0), b(0) {}
    constexpr ABPair(int64_t a_, uint64_t b_) noexcept : a(a_), b(b_) {}

    constexpr bool operator==(const ABPair& other) const noexcept {
        return a == other.a && b == other.b;
    }

    constexpr bool operator!=(const ABPair& other) const noexcept {
        return !(*this == other);
    }

    constexpr bool operator<(const ABPair& other) const noexcept {
        if (b != other.b) return b < other.b;
        return a < other.a;
    }

    constexpr bool operator<=(const ABPair& other) const noexcept {
        return !(other < *this);
    }

    constexpr bool operator>(const ABPair& other) const noexcept {
        return other < *this;
    }

    constexpr bool operator>=(const ABPair& other) const noexcept {
        return !(*this < other);
    }
};

/// ABPair 的哈希函数
struct ABPairHash {
    size_t operator()(const ABPair& ab) const noexcept {
        // FNV-1a 风格的混合
        size_t h = 14695981039346656037ULL;
        h ^= static_cast<size_t>(ab.a);
        h *= 1099511628211ULL;
        h ^= static_cast<size_t>(ab.b);
        h *= 1099511628211ULL;
        return h;
    }
};

/// PrimePower - 素数幂，因子分解的基本单位
struct PrimePower {
    uint32_t p;   // 素数
    uint32_t r;   // 代数侧的根 mod p（有理侧可忽略，设为0）
    uint8_t  e;   // 指数

    constexpr PrimePower() noexcept : p(0), r(0), e(0) {}
    constexpr PrimePower(uint32_t p_, uint32_t r_, uint8_t e_) noexcept
        : p(p_), r(r_), e(e_) {}

    // 仅用于有理侧
    constexpr PrimePower(uint32_t p_, uint8_t e_) noexcept
        : p(p_), r(0), e(e_) {}

    constexpr bool operator==(const PrimePower& other) const noexcept {
        return p == other.p && r == other.r && e == other.e;
    }

    constexpr bool operator!=(const PrimePower& other) const noexcept {
        return !(*this == other);
    }

    // 按 (p, r) 排序
    constexpr bool operator<(const PrimePower& other) const noexcept {
        if (p != other.p) return p < other.p;
        return r < other.r;
    }
};

/// PrimePower 的哈希函数
struct PrimePowerHash {
    size_t operator()(const PrimePower& pp) const noexcept {
        // 将 (p, r) 组合成一个 64 位值
        uint64_t combined = (static_cast<uint64_t>(pp.p) << 32) | pp.r;
        return std::hash<uint64_t>{}(combined);
    }
};

/// 有理因子基条目
struct RationalPrime {
    uint32_t p;        // 素数
    uint32_t log_p;    // floor(log2(p) * scale)，筛法用定点数

    constexpr RationalPrime() noexcept : p(0), log_p(0) {}
    constexpr RationalPrime(uint32_t p_, uint32_t log_p_) noexcept
        : p(p_), log_p(log_p_) {}
};

/// 代数因子基条目
struct AlgebraicPrime {
    uint32_t p;        // 素数
    uint32_t r;        // f(r) = 0 (mod p)，UINT32_MAX 表示 projective root
    uint32_t log_p;    // 对数值
    uint8_t  degree;   // 素理想的度（通常是1）

    static constexpr uint32_t PROJECTIVE_ROOT = UINT32_MAX;

    constexpr AlgebraicPrime() noexcept : p(0), r(0), log_p(0), degree(1) {}
    constexpr AlgebraicPrime(uint32_t p_, uint32_t r_, uint32_t log_p_, uint8_t deg = 1) noexcept
        : p(p_), r(r_), log_p(log_p_), degree(deg) {}

    /// 是否是 projective root
    [[nodiscard]] constexpr bool is_projective() const noexcept {
        return r == PROJECTIVE_ROOT;
    }
};

/// 因子基参数
struct FactorBaseParams {
    uint32_t rational_bound = 0;      // 有理侧上界 B_r
    uint32_t algebraic_bound = 0;     // 代数侧上界 B_a
    uint32_t large_prime_bound = 0;   // 大素数上界（通常 100*B）
    uint8_t  log_scale = 10;          // 对数缩放因子

    constexpr FactorBaseParams() noexcept = default;
    constexpr FactorBaseParams(uint32_t rat, uint32_t alg, uint32_t lp, uint8_t scale = 10) noexcept
        : rational_bound(rat), algebraic_bound(alg), large_prime_bound(lp), log_scale(scale) {}
};

} // namespace core
} // namespace gnfs
