#pragma once

#include "../core/integer.hpp"
#include "ecm.hpp"

#include <cmath>
#include <cstdint>

namespace gnfs {
namespace cofactor {

using core::Integer;

/// Cofactor 分类
enum class CofactorClass : uint8_t {
    Smooth = 0,        // cofactor = 1, 完全光滑
    Prime = 1,         // cofactor 是单个素数 (1LP)
    PrimePower = 2,    // cofactor 是素数幂 p^k
    Semiprime = 3,     // cofactor = p * q (2LP)
    Composite = 4,     // 其他合数（可能是 3LP 或更多）
    TooLarge = 5,      // cofactor 超出大素数界限
    Unknown = 6        // 无法确定
};

/// 分类结果
struct CofactorClassification {
    CofactorClass type = CofactorClass::Unknown;
    uint64_t factor1 = 0;      // 第一个因子（如果适用）
    uint64_t factor2 = 0;      // 第二个因子（如果适用）
    uint8_t power = 1;         // 幂次（如果是素数幂）
};

/// 检查一个数是否可能是素数（Miller-Rabin）
/// @param n 要检查的数
/// @param rounds Miller-Rabin 轮数
/// @return true 如果可能是素数
[[nodiscard]] inline bool is_probable_prime(const Integer& n, int rounds = 25) {
    if (n.fits_uint64()) {
        uint64_t val = n.to_uint64();
        if (val < 2) return false;
        if (val == 2) return true;
        if (val % 2 == 0) return false;
    }
    return n.is_probable_prime(rounds) > 0;
}

/// 检查一个 uint64_t 是否是素数
[[nodiscard]] inline bool is_probable_prime_u64(uint64_t n, int rounds = 25) {
    if (n < 2) return false;
    if (n == 2) return true;
    if (n % 2 == 0) return false;
    if (n < 9) return true;  // 3, 5, 7
    if (n % 3 == 0) return false;

    Integer temp(static_cast<unsigned long long>(n));
    return temp.is_probable_prime(rounds) > 0;
}

/// 检查是否是完全平方数
/// @param n 要检查的数
/// @param root 输出：如果是完全平方，存储平方根
/// @return true 如果是完全平方
[[nodiscard]] inline bool is_perfect_square(uint64_t n, uint64_t& root) {
    if (n == 0) {
        root = 0;
        return true;
    }

    uint64_t x = static_cast<uint64_t>(std::sqrt(static_cast<double>(n)));

    // 检查附近的值（浮点精度问题）
    for (uint64_t candidate = (x > 0 ? x - 1 : 0); candidate <= x + 1; ++candidate) {
        if (candidate * candidate == n) {
            root = candidate;
            return true;
        }
    }

    return false;
}

/// 检查是否是完全幂 n = b^k
/// @param n 要检查的数
/// @param base 输出：底数
/// @param exp 输出：指数
/// @return true 如果是完全幂
[[nodiscard]] inline bool is_perfect_power(uint64_t n, uint64_t& base, uint8_t& exp) {
    if (n <= 1) {
        base = n;
        exp = 1;
        return true;
    }

    // 检查是否是 2 的幂
    if ((n & (n - 1)) == 0) {
        base = 2;
        exp = 0;
        while (n > 1) {
            n >>= 1;
            ++exp;
        }
        return true;
    }

    // 检查小指数 (2 到 63)
    // std::pow(double, 1.0/k) 对 n > 2^53 有精度损失，
    // 所以对 round 结果的 b-1, b, b+1 三个候选都验证
    for (uint8_t k = 2; k <= 63; ++k) {
        double root = std::pow(static_cast<double>(n), 1.0 / k);
        uint64_t b_mid = static_cast<uint64_t>(std::round(root));

        if (b_mid < 2) break;

        for (uint64_t b_try = (b_mid > 2 ? b_mid - 1 : 2); b_try <= b_mid + 1; ++b_try) {
            uint64_t power = 1;
            bool overflow = false;

            for (uint8_t i = 0; i < k && !overflow; ++i) {
                if (power > UINT64_MAX / b_try) {
                    overflow = true;
                } else {
                    power *= b_try;
                }
            }

            if (!overflow && power == n) {
                base = b_try;
                exp = k;
                return true;
            }
        }
    }

    base = n;
    exp = 1;
    return false;
}

/// 使用 Pollard's rho 算法尝试分解
/// @param n 要分解的数
/// @param max_iterations 每次尝试的最大迭代次数
/// @return 找到的因子，如果失败返回 1 或 n
[[nodiscard]] inline uint64_t pollard_rho(uint64_t n, size_t max_iterations = 100000) {
    if (n % 2 == 0) return 2;
    if (n % 3 == 0) return 3;

    auto gcd = [](uint64_t a, uint64_t b) -> uint64_t {
        while (b != 0) {
            uint64_t t = b;
            b = a % b;
            a = t;
        }
        return a;
    };

    // 尝试多个 c 值，避免单一多项式对某些 n 永远循环
    for (uint64_t c = 1; c <= 19; c += 2) {
        uint64_t x = 2, y = 2, d = 1;
        size_t iterations = 0;

        auto f = [n, c](uint64_t x) -> uint64_t {
            __uint128_t xx = static_cast<__uint128_t>(x) * x + c;
            return static_cast<uint64_t>(xx % n);
        };

        while (d == 1 && iterations < max_iterations) {
            x = f(x);
            y = f(f(y));

            uint64_t diff = (x > y) ? x - y : y - x;
            d = gcd(diff, n);

            ++iterations;
        }

        if (d != 1 && d != n) return d;
    }

    return 1;  // 所有 c 值均失败
}

/// 分类 cofactor
/// @param cofactor 剩余的未分解部分
/// @param large_prime_bound 大素数上界
/// @return 分类结果
[[nodiscard]] inline CofactorClassification classify_cofactor(
        const Integer& cofactor,
        uint64_t large_prime_bound) {

    CofactorClassification result;

    // 检查是否为 1
    if (cofactor.fits_uint64()) {
        uint64_t c = cofactor.to_uint64();

        if (c == 1) {
            result.type = CofactorClass::Smooth;
            return result;
        }

        // 检查是否超出界限 (use __uint128_t to avoid overflow when lpb > 2^32)
        if (c > static_cast<__uint128_t>(large_prime_bound) * large_prime_bound) {
            // 可能是 3LP 或更多
            result.type = CofactorClass::TooLarge;
            return result;
        }

        // 检查是否是素数
        if (is_probable_prime_u64(c)) {
            if (c <= large_prime_bound) {
                result.type = CofactorClass::Prime;
                result.factor1 = c;
            } else {
                result.type = CofactorClass::TooLarge;
            }
            return result;
        }

        // 检查是否是素数幂
        uint64_t base;
        uint8_t exp;
        if (is_perfect_power(c, base, exp) && exp > 1) {
            if (is_probable_prime_u64(base) && base <= large_prime_bound) {
                result.type = CofactorClass::PrimePower;
                result.factor1 = base;
                result.power = exp;
                return result;
            }
        }

        // 检查是否是半素数 (p * q)
        // 使用 Pollard's rho 尝试分解
        uint64_t factor = pollard_rho(c);

        if (factor != 1 && factor != c) {
            uint64_t other = c / factor;

            // 验证两个因子都是素数
            if (is_probable_prime_u64(factor) && is_probable_prime_u64(other)) {
                // 检查是否在界限内
                if (factor <= large_prime_bound && other <= large_prime_bound) {
                    result.type = CofactorClass::Semiprime;
                    result.factor1 = std::min(factor, other);
                    result.factor2 = std::max(factor, other);
                    return result;
                }
            }

            // 分解成功但因子不符合要求
            result.type = CofactorClass::Composite;
            return result;
        }

        // Pollard's rho 失败 — 尝试 ECM 作为回退
        // 某些特殊结构的合数（如 p-1 光滑）Pollard rho 可能循环
        {
            Integer c_int(static_cast<unsigned long long>(c));
            auto ecm_result = ECM::quick_factor(c_int);
            if (ecm_result && ecm_result->fits_uint64()) {
                uint64_t f1 = ecm_result->to_uint64();
                if (f1 != 1 && f1 != c) {
                    uint64_t f2 = c / f1;
                    if (is_probable_prime_u64(f1) && is_probable_prime_u64(f2)) {
                        if (f1 <= large_prime_bound && f2 <= large_prime_bound) {
                            result.type = CofactorClass::Semiprime;
                            result.factor1 = std::min(f1, f2);
                            result.factor2 = std::max(f1, f2);
                            return result;
                        }
                    }
                    result.type = CofactorClass::Composite;
                    return result;
                }
            }
        }

        // Pollard rho + ECM 均失败
        result.type = CofactorClass::Composite;
        return result;
    }

    // 大数情况 - 超出 uint64_t
    // 先检查是否是素数
    if (is_probable_prime(cofactor)) {
        // 大素数，检查是否在界限内 (用 Integer 比较)
        Integer lp_int(static_cast<unsigned long long>(large_prime_bound));
        if (cofactor.compare(lp_int) <= 0) {
            result.type = CofactorClass::Prime;
            // factor1/factor2 无法存储大数，但 type 正确
            return result;
        }
        result.type = CofactorClass::TooLarge;
        return result;
    }

    // 检查是否在 B^2 范围内
    Integer lp_sq(static_cast<unsigned long long>(large_prime_bound));
    lp_sq *= Integer(large_prime_bound);
    if (cofactor.compare(lp_sq) > 0) {
        result.type = CofactorClass::TooLarge;
        return result;
    }

    // 尝试 ECM 分解
    auto ecm_result = ECM::quick_factor(cofactor);
    if (ecm_result) {
        Integer other = cofactor.clone();
        other /= *ecm_result;

        // 检查两个因子是否都是素数且在界限内
        Integer lp_int(static_cast<unsigned long long>(large_prime_bound));
        if (is_probable_prime(*ecm_result) && is_probable_prime(other) &&
            ecm_result->compare(lp_int) <= 0 && other.compare(lp_int) <= 0) {
            result.type = CofactorClass::Semiprime;
            // 尝试将因子存储为 uint64_t (如果可能)
            if (ecm_result->fits_uint64() && other.fits_uint64()) {
                result.factor1 = std::min(ecm_result->to_uint64(), other.to_uint64());
                result.factor2 = std::max(ecm_result->to_uint64(), other.to_uint64());
            }
            return result;
        }
    }

    // ECM 也失败了
    result.type = CofactorClass::Composite;
    return result;
}

/// 快速检查 cofactor 是否可能有用（粗略筛选）
/// @param cofactor 剩余的未分解部分
/// @param large_prime_bound 大素数上界
/// @param allow_2lp 是否允许 2LP
/// @return true 如果值得进一步检查
[[nodiscard]] inline bool quick_cofactor_check(
        const Integer& cofactor,
        uint64_t large_prime_bound,
        bool allow_2lp = true) {

    if (cofactor.fits_uint64()) {
        uint64_t c = cofactor.to_uint64();

        // 完全光滑
        if (c == 1) return true;

        // 单个大素数
        if (c <= large_prime_bound) return true;

        // 2LP: cofactor <= B^2 (use __uint128_t to avoid overflow when lpb > 2^32)
        if (allow_2lp && c <= static_cast<__uint128_t>(large_prime_bound) * large_prime_bound) {
            return true;
        }

        return false;
    }

    // 超出 uint64_t 范围 — 用 Integer 比较
    Integer lp_int(static_cast<unsigned long long>(large_prime_bound));
    if (cofactor.compare(lp_int) <= 0) return true;  // 1LP

    if (allow_2lp) {
        Integer lp_sq = lp_int.clone();
        lp_sq *= lp_int;
        if (cofactor.compare(lp_sq) <= 0) return true;  // 2LP
    }

    return false;
}

/// 获取分类的字符串表示
[[nodiscard]] inline const char* cofactor_class_str(CofactorClass cls) {
    switch (cls) {
        case CofactorClass::Smooth: return "Smooth";
        case CofactorClass::Prime: return "Prime(1LP)";
        case CofactorClass::PrimePower: return "PrimePower";
        case CofactorClass::Semiprime: return "Semiprime(2LP)";
        case CofactorClass::Composite: return "Composite";
        case CofactorClass::TooLarge: return "TooLarge";
        case CofactorClass::Unknown: return "Unknown";
        default: return "???";
    }
}

} // namespace cofactor
} // namespace gnfs
