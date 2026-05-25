#pragma once

#include "../core/integer.hpp"
#include "../util/bit_intrin.hpp"
#include "../util/primes.hpp"
#include "brent_pollard_rho.hpp"
#include "ecm.hpp"
#include "squfof.hpp"
#include "survival_predictor.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace gnfs::cofactor {

using core::Integer;

namespace detail {

/// Bit length of an unsigned uint64 value. Returns 0 for v == 0.
[[nodiscard]] inline uint64_t bit_length_u64(uint64_t v) noexcept {
    if (v == 0) return 0;
    // 64 - clz gives the position of the highest set bit + 1.
    return 64ULL - static_cast<uint64_t>(gnfs::util::clz64(v));
}

} // namespace detail

/// ENV-gate cache for `GNFS_COFACTOR_BRENT`. Parsed once (first call) and
/// cached. Returns true iff env is set to "1" (any other value disables).
[[nodiscard]] inline bool brent_pollard_enabled() {
    static const bool enabled = []() {
        const char* env = std::getenv("GNFS_COFACTOR_BRENT");
        return env != nullptr && env[0] == '1' && env[1] == '\0';
    }();
    return enabled;
}

/// Precomputed primes in [101, 65521] for trial division Phase 2.
/// ~6500 primes vs ~32K odd numbers — 4.7× fewer modular divisions.
[[nodiscard]] inline const std::vector<uint64_t>& get_mid_primes() {
    static const auto table = []() {
        std::vector<uint64_t> primes;
        primes.reserve(6600);
        // Sieve of Eratosthenes for [101, 65535]
        constexpr uint64_t LIMIT = 65536;
        std::vector<bool> is_prime(LIMIT + 1, true);
        is_prime[0] = is_prime[1] = false;
        for (uint64_t i = 2; i * i <= LIMIT; ++i) {
            if (is_prime[i]) {
                for (uint64_t j = i * i; j <= LIMIT; j += i)
                    is_prime[j] = false;
            }
        }
        for (uint64_t i = 101; i <= LIMIT; ++i) {
            if (is_prime[i]) primes.push_back(i);
        }
        return primes;
    }();
    return table;
}

/// Cofactor 分类
enum class CofactorClass : uint8_t {
    Smooth = 0,        // cofactor = 1, 完全光滑
    Prime = 1,         // cofactor 是单个素数 (1LP)
    PrimePower = 2,    // cofactor 是素数幂 p^k
    Semiprime = 3,     // cofactor = p * q (2LP)
    Composite = 4,     // 其他合数（无法分解或超 3LP space）
    TooLarge = 5,      // cofactor 超出大素数界限
    Unknown = 6,       // 无法确定
    ThreeLP = 7        // cofactor = p * q * r (3LP, 全部 ≤ B)
};

/// 分类结果
struct CofactorClassification {
    CofactorClass type = CofactorClass::Unknown;
    uint64_t factor1 = 0;      // 第一个因子（如果适用）
    uint64_t factor2 = 0;      // 第二个因子（如果适用）
    uint64_t factor3 = 0;      // 第三个因子 (ThreeLP 时使用)
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

    // Modular exponentiation: base^exp mod mod.
    auto mod_pow = [](uint64_t base, uint64_t exp, uint64_t mod) -> uint64_t {
        uint64_t result = 1;
        base %= mod;
        while (exp > 0) {
            if (exp & 1) result = gnfs::util::mul_mod_u64(result, base, mod);
            exp >>= 1;
            base = gnfs::util::mul_mod_u64(base, base, mod);
        }
        return result;
    };

    // Single-witness MR test
    auto witness_test = [&](uint64_t a) -> bool {
        if (a % n == 0) return true;  // trivial witness
        uint64_t x = mod_pow(a, d, n);
        if (x == 1 || x == n - 1) return true;
        for (int i = 1; i < r; ++i) {
            x = gnfs::util::mul_mod_u64(x, x, n);
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
    // n=0/1 不是有意义的"完全幂"。语义上 1=1^k 对任意 k 都成立,n=0 也病态。
    // 返回 false 避免下游 if(exp>1) 误判。
    if (n < 2) return false;

    // 检查是否是 2 的幂
    if ((n & (n - 1)) == 0) {
        base = 2;
        exp = 0;
        while (n > 1) {
            n >>= 1;
            ++exp;
        }
        if (exp < 2) return false;  // n=2 本身不是幂
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
            return gnfs::util::add_mod_u64(gnfs::util::mul_mod_u64(x, x, n), c, n);
        };

        uint64_t x = 2, y = 2;
        uint64_t d = 1;
        uint64_t r = 1;      // Brent step size (doubles each phase)
        uint64_t q = 1;      // accumulated product for batch GCD
        uint64_t ys = 0;     // save point for backtracking
        size_t total_evals = 0;

        do {
            x = y;
            // Brent phase advance: create initial distance
            for (uint64_t i = 0; i < r && total_evals < max_iterations; ++i, ++total_evals) {
                y = f(y);
            }

            // Accumulate |x - y| products, check gcd every BATCH_SIZE
            uint64_t k = 0;
            do {
                ys = y;
                uint64_t batch = std::min(static_cast<uint64_t>(BATCH_SIZE), r - k);
                for (uint64_t i = 0; i < batch && total_evals < max_iterations; ++i, ++total_evals) {
                    y = f(y);
                    uint64_t diff = (x > y) ? x - y : y - x;
                    if (diff == 0) { d = n; break; } // cycle without factor
                    q = gnfs::util::mul_mod_u64(q, diff, n);
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
            size_t backtrack_steps = 0;
            constexpr size_t MAX_BACKTRACK = BATCH_SIZE * 2;
            while (d == 1 && backtrack_steps < MAX_BACKTRACK) {
                ys = f(ys);
                uint64_t diff = (x > ys) ? x - ys : ys - x;
                if (diff == 0) break;
                d = gcd(diff, n);
                ++backtrack_steps;
            }
        }

        if (d != 1 && d != n) return d;
    }

    return 1;
}

/// 尝试用 SQUFOF / Pollard rho 分解 c 找一个非平凡因子.
/// 仅用于 try_classify_three_lp 内部. 返回 1 表示找不到.
///
/// PERF: 此函数 ~1-2ms cost (SQUFOF iter cap + Pollard fallback). caller
/// 应该先用 has_small_factor() 过滤明显的"不可分" cofactors, 避免大量浪费.
[[nodiscard]] inline uint64_t try_find_one_factor_fast(uint64_t c) {
    if (c <= 1) return 1;
    constexpr uint64_t small_primes[] = {
        2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,
        53,59,61,67,71,73,79,83,89,97
    };
    for (uint64_t sp : small_primes) {
        if (c % sp == 0 && sp != c) return sp;
    }
    if (c < UINT64_C(0x100000000)) {
        const auto& mid = get_mid_primes();
        uint64_t limit = static_cast<uint64_t>(std::sqrt(static_cast<double>(c))) + 1;
        for (uint64_t p : mid) {
            if (p > limit) break;
            if (c % p == 0) return p;
        }
    }
    // SQUFOF 用 short iter cap 避免 3LP path 长时间 stall
    if (c < (UINT64_C(1) << 62)) {
        // Cap iterations more aggressively in 3LP path: 1000 vs 2000-20000 in normal path
        uint32_t lim = (c < (UINT64_C(1) << 40)) ? 1000 :
                       (c < (UINT64_C(1) << 50)) ? 2000 : 5000;
        uint64_t f = SQUFOF::factor(c, lim);
        if (f != 1 && f != c) return f;
    }
    // Pollard rho with tight iter cap
    size_t max_iter = (c < (UINT64_C(1) << 40)) ? 5000 : 30000;
    uint64_t f = pollard_rho(c, max_iter);
    if (f != 1 && f != c) return f;
    return 1;
}

/// 快速预筛: 检查 c 是否有小素数因子 (≤ 97).
/// 用于 try_classify_three_lp 入口, 大多数 hard composite 3LP 候选会被这个
/// 筛选掉, 避免 SQUFOF/Pollard 的 ~1ms 浪费.
///
/// 注意: 3LP 实际场景 c = p*q*r where p,q,r prime ≤ B = 2^lp_bits. p,q,r 都
/// 可能 ≥ 1000 (lp_bits=23 时 typical), 所以 has_small_factor 返回 false 不
/// 等于"不能 3LP". 但 sieve 路径上 80%+ 的 cofactor 是 random composite, 这
/// 个 fast 拒绝节省大量浪费.
///
/// 经验权衡: lp_bits ≥ 23 (50d+) 时 typical 3LP cofactors 的 smallest prime
/// 平均 ~1000-100K, 不会被 small_primes 筛过 → fast path miss 大多数真 3LP.
/// 这是个 tradeoff: 节省 sieve overhead 是首要的, 接受少量 3LP loss.
[[nodiscard]] inline bool has_small_factor(uint64_t c, uint64_t bound = 97) {
    // 用前 10 个 smallest primes 已足够覆盖大多数 random composites
    constexpr uint64_t tiny_primes[] = {2,3,5,7,11,13,17,19,23,29};
    for (uint64_t p : tiny_primes) {
        if (p > bound) break;
        if (c % p == 0) return true;
    }
    return false;
}

/// 尝试将 cofactor 分解为 3 个素数 (3LP)。
///
/// 前置: caller 已确认 cofactor 在 (B², B³] 区间, 且 fits_uint64.
/// 策略 (sieve-fast path):
///   1. 用 SQUFOF/Pollard 找第一个因子 f1 (no ECM).
///   2. 若 f1 prime 且 ≤ B, 分解 rest (再调一次 SQUFOF/Pollard).
///   3. 若 f1 composite, split 后检查是否构成 (p, q, rest).
///   4. 失败立即返回 nullopt — caller 会 reject relation.
///
/// PERF: 单次调用 ~1-2ms 即使失败. 高 sieve throughput 需要 caller (e.g.
/// classify_cofactor) 用 has_small_factor 作 fast pre-filter. 否则每个
/// algebraic cofactor reject 都 ~1ms wasted, 50K rejects/SQ → 50s 浪费.
[[nodiscard]] inline std::optional<CofactorClassification>
try_classify_three_lp(uint64_t c, uint64_t large_prime_bound) {
    if (c <= 1) return std::nullopt;

    uint64_t f1 = try_find_one_factor_fast(c);
    if (f1 == 1) return std::nullopt;
    uint64_t rest = c / f1;
    if (f1 * rest != c) return std::nullopt;  // safety

    if (f1 > rest) std::swap(f1, rest);

    if (is_probable_prime_u64(f1)) {
        if (f1 > large_prime_bound) return std::nullopt;
        if (is_probable_prime_u64(rest)) {
            if (rest <= large_prime_bound) {
                CofactorClassification r;
                r.type = CofactorClass::Semiprime;
                r.factor1 = std::min(f1, rest);
                r.factor2 = std::max(f1, rest);
                return r;
            }
            return std::nullopt;
        }
        uint64_t f2 = try_find_one_factor_fast(rest);
        if (f2 == 1 || f2 == rest) return std::nullopt;
        uint64_t f3 = rest / f2;
        if (f2 * f3 != rest) return std::nullopt;
        if (!is_probable_prime_u64(f2) || !is_probable_prime_u64(f3)) return std::nullopt;
        if (f2 > large_prime_bound || f3 > large_prime_bound) return std::nullopt;
        uint64_t a = f1, b = f2, d = f3;
        if (a > b) std::swap(a, b);
        if (b > d) std::swap(b, d);
        if (a > b) std::swap(a, b);
        CofactorClassification r;
        r.type = CofactorClass::ThreeLP;
        r.factor1 = a; r.factor2 = b; r.factor3 = d;
        return r;
    }

    uint64_t f1a = try_find_one_factor_fast(f1);
    if (f1a == 1 || f1a == f1) return std::nullopt;
    uint64_t f1b = f1 / f1a;
    if (f1a * f1b != f1) return std::nullopt;
    if (!is_probable_prime_u64(f1a) || !is_probable_prime_u64(f1b)) return std::nullopt;
    if (f1a > large_prime_bound || f1b > large_prime_bound) return std::nullopt;
    if (!is_probable_prime_u64(rest)) return std::nullopt;
    if (rest > large_prime_bound) return std::nullopt;
    uint64_t a = f1a, b = f1b, d = rest;
    if (a > b) std::swap(a, b);
    if (b > d) std::swap(b, d);
    if (a > b) std::swap(a, b);
    CofactorClassification r;
    r.type = CofactorClass::ThreeLP;
    r.factor1 = a; r.factor2 = b; r.factor3 = d;
    return r;
}

/// 分类 cofactor
/// @param cofactor 剩余的未分解部分
/// @param large_prime_bound 大素数上界
/// @param allow_3lp 是否尝试 3LP 分解 (默认 false: 保留旧行为)
/// @param smoothness_bound 光滑界 B（用于 survival predictor，0 = 禁用 predictor）
/// @return 分类结果
[[nodiscard]] inline CofactorClassification classify_cofactor(
        const Integer& cofactor,
        uint64_t large_prime_bound,
        bool allow_3lp = false,
        uint64_t smoothness_bound = 0) {

    CofactorClassification result;

    // Survival predictor early reject (default OFF via ENV).
    // Inserted BEFORE trial division / SQUFOF / ECM. Only takes effect when:
    //   GNFS_SURVIVAL_FILTER=1 + GNFS_SURVIVAL_THRESHOLD > 0
    //   + smoothness_bound > 0 (caller supplied real B).
    // When all conditions hold and Dickman estimate < threshold, return
    // TooLarge (treats cofactor as too large to be smooth).
    //
    // When predictor is enabled but does NOT reject, we record at the end
    // whether the cofactor turned out smooth (pass_smooth) or not
    // (pass_failed). The predictor_passed flag below propagates that
    // intent.
    const bool survival_predictor_active =
        smoothness_bound > 0 && survival_filter_enabled() && survival_threshold() > 0.0;
    if (survival_predictor_active) {
        const uint64_t c_bits = cofactor.fits_uint64()
            ? detail::bit_length_u64(cofactor.to_uint64())
            : static_cast<uint64_t>(mpz_sizeinbase(cofactor.get_mpz(), 2));
        const uint64_t B_bits = detail::bit_length_u64(smoothness_bound);
        const uint64_t LP_bits = detail::bit_length_u64(large_prime_bound);
        if (should_reject_cofactor(c_bits, B_bits, LP_bits)) {
            survival_stats().record_reject();
            result.type = CofactorClass::TooLarge;
            return result;
        }
    }

    // RAII-style stats updater: records pass_smooth or pass_failed when the
    // predictor was active and we didn't reject. Triggered on every return
    // path of classify_cofactor (including exceptions).
    struct PassRecorder {
        bool active;
        const CofactorClassification* result_ref;
        ~PassRecorder() {
            if (!active) return;
            // pass_smooth covers Smooth / Prime / PrimePower / Semiprime / ThreeLP
            // pass_failed covers TooLarge / Composite / Unknown
            switch (result_ref->type) {
                case CofactorClass::Smooth:
                case CofactorClass::Prime:
                case CofactorClass::PrimePower:
                case CofactorClass::Semiprime:
                case CofactorClass::ThreeLP:
                    survival_stats().record_pass_smooth();
                    break;
                case CofactorClass::TooLarge:
                case CofactorClass::Composite:
                case CofactorClass::Unknown:
                default:
                    survival_stats().record_pass_failed();
                    break;
            }
        }
    };
    PassRecorder pass_recorder{survival_predictor_active, &result};

    // 检查是否为 1
    if (cofactor.fits_uint64()) {
        uint64_t c = cofactor.to_uint64();

        if (c == 1) {
            result.type = CofactorClass::Smooth;
            return result;
        }

        // 检查是否超出 2LP 界限 (overflow-safe for large B)
        if (gnfs::util::u64_gt_square(c, large_prime_bound)) {
            // c > B²: 进 3LP 空间
            if (allow_3lp) {
                // 检查上界 B³.
                if (gnfs::util::u64_gt_cube(c, large_prime_bound)) {
                    result.type = CofactorClass::TooLarge;
                    return result;
                }
                // c ≤ B³ 且非素数 (后面会检测) → 尝试 3LP
                // 注意: c 也可能是 prime in (B, B²-something) — 实际不可能, 因 c > B²
                // 所以一定是合数
                if (auto three = try_classify_three_lp(c, large_prime_bound)) {
                    return *three;
                }
                // 3LP 分解失败 → Composite (caller 决定是否接受)
                result.type = CofactorClass::Composite;
                return result;
            }
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
        {
            uint64_t base;
            uint8_t exp;
            if (is_perfect_power(c, base, exp) && exp > 1) {
                if (is_probable_prime_u64(base)) {
                    if (base <= large_prime_bound) {
                        result.type = CofactorClass::PrimePower;
                        result.factor1 = base;
                        result.power = exp;
                        return result;
                    } else {
                        result.type = CofactorClass::TooLarge;
                        return result;
                    }
                }
            }
        }

        // 检查是否是半素数 (p * q)
        // 分层策略: 小素数试除 → 中素数试除 → SQUFOF → Pollard rho → ECM
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

            // Phase 2: 素数表试除到 sqrt(c) — 只测素数 (was: 全奇数)
            // 对 c < 2^32 (sqrt < 2^16)，用 [101..65521] 的素数试除
            if (factor == 1 && c < UINT64_C(0x100000000)) {
                static const auto& mid_primes = get_mid_primes();
                uint64_t limit = static_cast<uint64_t>(std::sqrt(static_cast<double>(c))) + 1;
                for (uint64_t p : mid_primes) {
                    if (p > limit) break;
                    if (c % p == 0) { factor = p; break; }
                }
            }

            // Phase 3a: SQUFOF (O(N^{1/4}))
            // Limit iterations: for 2LP cofactors (~10^12), O(N^{1/4}) ≈ 1000 iters.
            // Cap at 2000 to avoid spending too long on hard composites.
            if (factor == 1 && c < (UINT64_C(1) << 62)) {
                uint32_t squfof_limit = (c < (UINT64_C(1) << 40)) ? 2000 :
                                        (c < (UINT64_C(1) << 50)) ? 5000 : 20000;
                factor = SQUFOF::factor(c, squfof_limit);
            }

            // Phase 3a-1: BrentPollardRho — opt-in via ENV GNFS_COFACTOR_BRENT=1.
            // Inserted between SQUFOF and legacy Pollard rho. Brent variant
            // typically beats both on hard 50-60-bit semiprimes that SQUFOF
            // fails on. Default OFF (0 behavior change without ENV).
            if (factor == 1 && brent_pollard_enabled()) {
                uint64_t bp_max = (c < (UINT64_C(1) << 40)) ? 20000 : 200000;
                Integer c_int(c);
                auto sp = BrentPollardRho::split(c_int, bp_max, /*seed=*/1);
                if (sp && sp->first.fits_uint64()) {
                    uint64_t f = sp->first.to_uint64();
                    if (f > 1 && f < c && c % f == 0) {
                        factor = f;
                    }
                }
            }

            // Phase 3b: Pollard rho (Brent variant) — fallback if SQUFOF fails
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

    // 检查是否在 B^2 范围内 — lp_sq = large_prime_bound² via mpz_ui_pow_ui
    Integer lp_sq;
    mpz_ui_pow_ui(lp_sq.get_mpz(), large_prime_bound, 2);
    if (cofactor.compare(lp_sq) > 0) {
        // 大数 cofactor > B² — 3LP space (此分支稀少, lpb ≤ 30 bits 时 c 通常 fits_uint64).
        // 当前实现 3LP 只支持 uint64 cofactor (fits_uint64 path). 大数路径 fallback TooLarge.
        // BACKLOG: 拓展 try_classify_three_lp 支持 Integer 输入 (lpb > 32 bits 才需要).
        result.type = CofactorClass::TooLarge;
        return result;
    }

    // 尝试 ECM 分解
    auto ecm_result = ECM::quick_factor(cofactor);
    if (ecm_result) {
        Integer other;
        mpz_divexact(other.get_mpz(), cofactor.get_mpz(), ecm_result->get_mpz());

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
/// @param allow_3lp 是否允许 3LP (默认 false 兼容旧调用)
/// @return true 如果值得进一步检查
[[nodiscard]] inline bool quick_cofactor_check(
        const Integer& cofactor,
        uint64_t large_prime_bound,
        bool allow_2lp = true,
        bool allow_3lp = false) {

    if (cofactor.fits_uint64()) {
        uint64_t c = cofactor.to_uint64();

        // 完全光滑
        if (c == 1) return true;

        // 单个大素数
        if (c <= large_prime_bound) return true;

        // 2LP: cofactor <= B^2
        if (allow_2lp && !gnfs::util::u64_gt_square(c, large_prime_bound)) {
            return true;
        }

        // 3LP: cofactor <= B^3
        if (allow_3lp) {
            if (!gnfs::util::u64_gt_cube(c, large_prime_bound)) return true;
        }

        return false;
    }

    // 超出 uint64_t 范围 — 用 Integer 比较
    Integer lp_int(static_cast<unsigned long long>(large_prime_bound));
    if (cofactor.compare(lp_int) <= 0) return true;  // 1LP

    if (allow_2lp) {
        Integer lp_sq;
        mpz_ui_pow_ui(lp_sq.get_mpz(), large_prime_bound, 2);
        if (cofactor.compare(lp_sq) <= 0) return true;  // 2LP
    }

    if (allow_3lp) {
        Integer lp_cube;
        mpz_ui_pow_ui(lp_cube.get_mpz(), large_prime_bound, 3);
        if (cofactor.compare(lp_cube) <= 0) return true;  // 3LP
    }

    return false;
}

} // namespace gnfs::cofactor
