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

/// 确定性 uint64 Miller-Rabin (零 GMP 分配)
///
/// 使用 7 个 witness bases {2, 3, 5, 7, 11, 13, 17}，对所有 n < 3.317×10^24
/// 给出 100% 确定性结果（覆盖全部 uint64 值域）。
/// 参考: Jim Sinclair, https://miller-rabin.appspot.com/
[[nodiscard]] inline bool is_probable_prime_u64(uint64_t n, [[maybe_unused]] int rounds = 25) {
    if (n < 2) return false;
    if (n == 2 || n == 3 || n == 5 || n == 7 || n == 11 || n == 13 || n == 17) return true;
    if (n % 2 == 0 || n % 3 == 0 || n % 5 == 0) return false;
    if (n < 19 * 19) return true;  // All remaining primes < 361

    // Decompose n-1 = d · 2^r
    uint64_t d = n - 1;
    int r = 0;
    while ((d & 1) == 0) { d >>= 1; ++r; }

    // Modular exponentiation: base^exp mod mod, using __uint128_t
    auto mod_pow = [](uint64_t base, uint64_t exp, uint64_t mod) -> uint64_t {
        uint64_t result = 1;
        base %= mod;
        while (exp > 0) {
            if (exp & 1)
                result = static_cast<uint64_t>(
                    (static_cast<__uint128_t>(result) * base) % mod);
            exp >>= 1;
            base = static_cast<uint64_t>(
                (static_cast<__uint128_t>(base) * base) % mod);
        }
        return result;
    };

    // Single-witness MR test
    auto witness_test = [&](uint64_t a) -> bool {
        if (a % n == 0) return true;  // trivial witness
        uint64_t x = mod_pow(a, d, n);
        if (x == 1 || x == n - 1) return true;
        for (int i = 1; i < r; ++i) {
            x = static_cast<uint64_t>(
                (static_cast<__uint128_t>(x) * x) % n);
            if (x == n - 1) return true;
        }
        return false;
    };

    // Deterministic Miller-Rabin for full uint64 range (Jim Sinclair):
    // n < 2,047:                          {2}
    // n < 1,373,653:                      {2, 3}
    // n < 25,326,001:                     {2, 3, 5}
    // n < 3,215,031,751:                  {2, 3, 5, 7}
    // n < 2,152,302,898,747:              {2, 3, 5, 7, 11}
    // n < 3,474,749,660,383:              {2, 3, 5, 7, 11, 13}
    // n < 341,550,071,728,321:            {2, 3, 5, 7, 11, 13, 17}
    // n < 3,825,123,056,546,413,051:      +19, 23
    // n < 2^64:                           +29, 31, 37 (Sinclair full set)
    constexpr uint64_t witnesses[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    int num_witnesses;
    if      (n < 2047ULL)                     num_witnesses = 1;
    else if (n < 1373653ULL)                  num_witnesses = 2;
    else if (n < 25326001ULL)                 num_witnesses = 3;
    else if (n < 3215031751ULL)               num_witnesses = 4;
    else if (n < 2152302898747ULL)            num_witnesses = 5;
    else if (n < 3474749660383ULL)            num_witnesses = 6;
    else if (n < 341550071728321ULL)          num_witnesses = 7;
    else if (n < 3825123056546413051ULL)      num_witnesses = 9;
    else                                      num_witnesses = 12;
    for (int i = 0; i < num_witnesses; ++i) {
        if (!witness_test(witnesses[i])) return false;
    }
    return true;
}

/// 检查一个数是否可能是素数（Miller-Rabin）
/// uint64 范围使用确定性 MR，大数使用 GMP probabilistic MR
[[nodiscard]] inline bool is_probable_prime(const Integer& n, int rounds = 25) {
    if (n.fits_uint64()) {
        return is_probable_prime_u64(n.to_uint64());
    }
    return n.is_probable_prime(rounds) > 0;
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
/// Brent variant with batch GCD (128-step accumulation)
///
/// 相比 Floyd + 逐步 GCD:
///   - Brent 减少 ~36% 函数求值（利用 2^k 步长周期探测）
///   - 批量 GCD 减少 ~128× gcd 调用（累乘后一次性检测）
///   - 当累积积恰好 ≡ 0 (mod n) 时自动回退逐步检测
///
/// @param n 要分解的数
/// @param max_iterations 最大函数求值次数（跨所有 c 值的总限额）
/// @return 找到的因子，如果失败返回 1
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

    constexpr size_t BATCH_SIZE = 128;

    for (uint64_t c = 1; c <= 19; c += 2) {
        auto f = [n, c](uint64_t x) -> uint64_t {
            __uint128_t xx = static_cast<__uint128_t>(x) * x + c;
            return static_cast<uint64_t>(xx % n);
        };

        uint64_t x = 2, y = 2;
        uint64_t d = 1;
        uint64_t r = 1;      // Brent step size (doubles each phase)
        uint64_t q = 1;      // accumulated product for batch GCD
        uint64_t ys = 0;     // save point for backtracking
        size_t total_evals = 0;

        do {
            x = y;
            // Advance y by r steps (Brent phase advance)
            for (uint64_t i = 0; i < r && total_evals < max_iterations; ++i, ++total_evals) {
                y = f(y);
            }

            // Inner loop: accumulate |x - y| products, check gcd every BATCH_SIZE
            uint64_t k = 0;
            do {
                ys = y;
                uint64_t batch = std::min(static_cast<uint64_t>(BATCH_SIZE), r - k);
                for (uint64_t i = 0; i < batch && total_evals < max_iterations; ++i, ++total_evals) {
                    y = f(y);
                    uint64_t diff = (x > y) ? x - y : y - x;
                    if (diff == 0) { d = n; break; } // cycle without factor
                    __uint128_t qq = static_cast<__uint128_t>(q) * diff;
                    q = static_cast<uint64_t>(qq % n);
                }
                if (q == 0) { d = n; break; } // product ≡ 0 (mod n)
                d = gcd(q, n);
                k += BATCH_SIZE;
            } while (k < r && d == 1);

            r *= 2;
        } while (d == 1 && total_evals < max_iterations);

        // Backtrack: batch product was divisible by n, check individual steps
        if (d == n) {
            d = 1;
            while (d == 1) {
                ys = f(ys);
                uint64_t diff = (x > ys) ? x - ys : ys - x;
                if (diff == 0) break;
                d = gcd(diff, n);
            }
        }

        if (d != 1 && d != n) return d;
    }

    return 1;
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
            } else {
                // base > LP bound or composite base — no point trying rho/ECM
                result.type = CofactorClass::TooLarge;
                return result;
            }
        }

        // 检查是否是半素数 (p * q)
        // 分层策略: 小素数试除 → 小合数试除 → Pollard rho → ECM
        {
            // Phase 1: 小素数预筛 (2,3,5,...,97)
            // 很多 semiprime 有小因子，25 次除法比 Pollard rho 快 100-1000×
            constexpr uint64_t small_primes[] = {
                2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,
                53,59,61,67,71,73,79,83,89,97
            };
            uint64_t factor = 1;
            for (uint64_t sp : small_primes) {
                if (c % sp == 0) { factor = sp; break; }
            }

            // Phase 2: 小合数直接试除到 sqrt(c)
            // 对 c < 2^32 (sqrt < 2^16)，用 101-65535 的奇数试除
            if (factor == 1 && c < UINT64_C(0x100000000)) {
                uint64_t limit = static_cast<uint64_t>(std::sqrt(static_cast<double>(c))) + 1;
                for (uint64_t p = 101; p <= limit; p += 2) {
                    if (c % p == 0) { factor = p; break; }
                }
            }

            // Phase 3: Pollard rho (Brent variant)
            if (factor == 1) {
                size_t max_iter = (c < (UINT64_C(1) << 40)) ? 10000 : 100000;
                factor = pollard_rho(c, max_iter);
            }

            if (factor != 1 && factor != c) {
                uint64_t other = c / factor;
                if (is_probable_prime_u64(factor) && is_probable_prime_u64(other)) {
                    if (factor <= large_prime_bound && other <= large_prime_bound) {
                        result.type = CofactorClass::Semiprime;
                        result.factor1 = std::min(factor, other);
                        result.factor2 = std::max(factor, other);
                        return result;
                    }
                }
                result.type = CofactorClass::Composite;
                return result;
            }
        }

        // Phase 4: ECM 回退 (仅 Pollard rho 失败时)
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

        // 所有分解方法均失败
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
