#pragma once

#include "number_field.hpp"
#include "couveignes.hpp"
#include "hensel_sqrt.hpp"
#include "../core/integer.hpp"
#include "../core/relation.hpp"
#include "../core/polynomial_context.hpp"
#include "../linalg/sparse_matrix.hpp"

#include <atomic>
#include <map>
#include <unordered_map>
#include <vector>

namespace gnfs::sqrt {

using core::Integer;
using core::Relation;
using core::PolynomialContext;
using linalg::BitVector;

/// Pre-check: verify all algebraic ideal powers have even multiplicity.
/// This is msieve's verify_alg_ideal_powers strategy — catches bad deps
/// in O(n·d) before running expensive Hensel lifting.
/// Returns true if parity check passes (all even), false if any odd exponent found.
[[nodiscard]] inline bool verify_algebraic_ideal_powers(
        const BitVector& dependency,
        const std::vector<Relation>& relations) {

    // Count algebraic FB factor multiplicities
    // Reserve dependency.popcount() * 30: avg ~20-30 FB factors per row, dependency
    // typical 64-256 popcount → 2K-8K reserve (way less than relations.size() millions).
    // Map upper bound is matrix_cols (FB part) which is 24K (50d) / 200K (60d).
    const size_t pop = dependency.popcount();
    std::unordered_map<uint32_t, uint64_t> fb_exponents;
    fb_exponents.reserve(std::min(pop * 30, relations.size()));
    // Use (p, r) pair as key to distinguish prime ideals above the same rational prime.
    // Pack into uint64: p (high 32) | r (low 32) — primes fit in 32 bits for 50d/60d
    // (LP bound 8M / 67M both < 2^32). Hash on uint64 is O(1) vs std::map O(log n).
    auto pack_pr = [](uint64_t p, uint64_t r) -> uint64_t {
        return (p << 32) | (r & 0xFFFFFFFFu);
    };
    std::unordered_map<uint64_t, uint64_t> lp_exponents;
    lp_exponents.reserve(std::min(pop * 4, relations.size()));  // avg 2-4 LP/row

    for (size_t i = 0; i < relations.size(); ++i) {
        if (!dependency.test(i)) continue;
        const auto& rel = relations[i];
        for (uint32_t idx : rel.algebraic_factors) {
            fb_exponents[idx]++;
        }
        for (const auto& lp : rel.algebraic_large_prime) {
            lp_exponents[pack_pr(lp.p, lp.r)] += lp.e;
        }
    }

    for (const auto& [idx, exp] : fb_exponents) {
        if (exp % 2 != 0) return false;
    }
    for (const auto& [key, exp] : lp_exponents) {
        if (exp % 2 != 0) return false;
    }
    return true;
}

/// 代数平方根计算结果
struct AlgebraicSqrtResult {
    Integer value;           // 平方根值（模 N）
    bool success = false;    // 是否成功
    std::string error;       // 错误信息
};

/// 代数平方根配置
struct AlgebraicSqrtConfig {
    bool verify = true;                // 是否验证结果
    // Note: num_primes should be <= 16 since sign determination only searches 2^16 patterns
    // With 16 primes starting at 1000, M ≈ 10^48 which is sufficient for most cases
    size_t num_primes = 16;            // CRT 使用的素数数量
    uint64_t prime_start = 1000;       // 素数搜索起点
    bool use_couveignes = true;        // 使用 Couveignes 算法
};

/// AlgebraicSqrt - 计算代数侧的平方根
/// 这是 GNFS 中最复杂的部分
///
/// 给定一组关系，其在代数侧的乘积是完全平方
/// 我们需要在数域 Q[α]/f(α) 中计算这个平方根
/// 然后将其映射到 Z/NZ
class AlgebraicSqrt {
public:
    using Config = AlgebraicSqrtConfig;

    explicit AlgebraicSqrt(const Config& config = Config{})
        : config_(config) {}

    /// 从依赖和关系计算代数平方根
    /// @param dependency 依赖向量
    /// @param relations 所有关系
    /// @param ctx 多项式上下文
    /// @return 平方根结果（模 N）
    [[nodiscard]] AlgebraicSqrtResult compute(
            const BitVector& dependency,
            const std::vector<Relation>& relations,
            const PolynomialContext& ctx) const {

        AlgebraicSqrtResult result;

        // Quick pre-check: algebraic ideal power parity (msieve strategy)
        // Catches bad deps in O(n·d) before expensive Hensel lifting
        if (!verify_algebraic_ideal_powers(dependency, relations)) {
            result.error = "Algebraic ideal powers parity check failed";
            return result;
        }

        // 创建数域
        NumberField nf(ctx);

        // 收集参与的 (a, b) 对（含合并关系的 extra pairs）
        // Reserve: dependency popcount × avg 1.5 ab pairs per relation
        // (merged relations have extra_ab_pairs).
        std::vector<std::pair<int64_t, uint64_t>> ab_pairs;
        ab_pairs.reserve(dependency.popcount() * 2);
        for (size_t i = 0; i < relations.size(); ++i) {
            if (!dependency.test(i)) continue;
            const auto& rel = relations[i];
            ab_pairs.emplace_back(rel.a, rel.b);
            for (const auto& [ea, eb] : rel.extra_ab_pairs) {
                ab_pairs.emplace_back(ea, eb);
            }
        }

        if (ab_pairs.empty()) {
            result.error = "No relations in dependency";
            return result;
        }

        // Try Hensel lifting first (most reliable for all sizes)
        // compute() returns the algebraic sqrt value mod N directly
        // (handles the O_K vs Z[α] index via the f'(α)² trick internally)
        {
            HenselSqrt::Config hcfg;
            hcfg.verbose = (ab_pairs.size() >= 500);
            // Reuse cached inert prime across deps for the same polynomial
            uint64_t cached = cached_inert_prime_.load(std::memory_order_relaxed);
            if (cached != 0) {
                hcfg.cached_inert_prime = cached;
            }
            HenselSqrt hensel(hcfg);
            auto sqrt_val = hensel.compute(ab_pairs, nf);
            if (sqrt_val) {
                if (cached == 0) {
                    uint64_t new_cached = hcfg.cached_inert_prime;
                    if (new_cached == 0) new_cached = hensel.last_inert_prime();
                    cached_inert_prime_.store(new_cached, std::memory_order_relaxed);
                }
                result.value = std::move(*sqrt_val);
                result.success = true;
                return result;
            }
            // CRT sign exhausted: doesn't necessarily mean dependency is invalid.
            // The CRT modulus M might be too small for the actual sqrt value.
            // Fall through to Couveignes as alternative method.
        }

        // Couveignes is the only correct algorithm in this fallback chain.
        // The old `compute_heuristic` branch (product^((N+1)/2)) was
        // mathematically invalid for composite N and always returned failure;
        // it has been removed.
        return compute_couveignes(ab_pairs, nf);
    }

    /// 简化版：假设乘积已经是数域元素
    [[nodiscard]] AlgebraicSqrtResult compute_from_product(
            const NumberFieldElement& product,
            const NumberField& nf) const {

        AlgebraicSqrtResult result;

        CouveignesSqrt::Config cfg;
        cfg.num_primes = config_.num_primes;
        cfg.prime_start = config_.prime_start;

        CouveignesSqrt couveignes(cfg);
        auto sqrt_opt = couveignes.compute_from_element(product, nf);

        if (!sqrt_opt) {
            result.error = "Couveignes algorithm failed to compute square root";
            return result;
        }

        result.value = nf.evaluate_at_m_mod_n(*sqrt_opt);
        result.success = true;

        return result;
    }

private:
    Config config_;
    // mutable atomic — 当前 pipeline 顺序处理 dependencies,但 const compute()
    // 读写 cached_inert_prime_ 是潜在线程不安全。atomic relaxed 提供未来安全网。
    mutable std::atomic<uint64_t> cached_inert_prime_{0};

    /// 使用 Couveignes 算法计算平方根
    [[nodiscard]] AlgebraicSqrtResult compute_couveignes(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf) const {

        AlgebraicSqrtResult result;

        CouveignesSqrt::Config cfg;
        cfg.num_primes = config_.num_primes;
        cfg.prime_start = config_.prime_start;

        CouveignesSqrt couveignes(cfg);

        // ── v19: 预先判定 gcd(f'(m), N) 决定走哪条路径 ──
        // gcd == 1: 标准 f'(α)² 路径 (Thomé), inv(f'(m)) 还原 sqrt(S)(m) mod N
        // gcd > 1 && < N: lucky factor! 跳过 f'(α)² 修正走老路径返回 sqrt(∏)(m)
        //   (caller 用 gcd(rat - alg, N) 仍能找到 N 的因子;另外, gcd 自身就是
        //   一个因子,我们额外通过 result.value 暴露)。
        // gcd == N: f'(m) ≡ 0 mod N — 极少见,直接失败让 caller fallback。
        const Integer& N = nf.n();
        Integer f_prime_m = compute_f_derivative_at_m(nf);
        Integer gcd_fpm;
        mpz_gcd(gcd_fpm.get_mpz(), f_prime_m.get_mpz(), N.get_mpz());

        if (gcd_fpm.compare(N) == 0) {
            result.error = "Couveignes: f'(m) ≡ 0 mod N";
            return result;
        }

        bool apply_correction = (gcd_fpm.compare(Integer(int64_t(1))) == 0);

        auto sqrt_opt = couveignes.compute(ab_pairs, nf, apply_correction);
        if (!sqrt_opt) {
            result.error = "Couveignes algorithm failed";
            return result;
        }

        Integer T_m = nf.evaluate_at_m_mod_n(*sqrt_opt);

        if (apply_correction) {
            // 标准 Thomé 路径: T(m) ≡ f'(m)·√S(m), 除 inv(f'(m)) 还原。
            Integer f_prime_m_inv;
            // mpz_invert 必成功(刚验证 gcd=1),理论不会失败。
            mpz_invert(f_prime_m_inv.get_mpz(),
                       f_prime_m.get_mpz(),
                       N.get_mpz());
            T_m *= f_prime_m_inv;
            T_m %= N;
            if (T_m.is_negative()) T_m += N;
        }
        // else: T(m) 就是 √(∏)(m) (小 N 老路径),无需进一步处理。

        result.value = std::move(T_m);
        result.success = true;

        // Note: Couveignes internally uses per-prime verification which is more
        // reliable than verifying in Z[α]/N (different reduction orders can cause
        // multiply_mod_n to produce inconsistent products). Skip redundant verification.

        return result;
    }

    /// 计算 f'(m) mod N (Horner). 与 hensel_sqrt.hpp 中同名函数复用语义.
    [[nodiscard]] static Integer compute_f_derivative_at_m(const NumberField& nf) {
        uint32_t d = nf.degree();
        const Integer& m = nf.m();
        const Integer& N = nf.n();

        // v22: result/term 直接 assign
        Integer result;
        result = nf.coeff(d);
        result *= Integer(static_cast<int64_t>(d));
        result %= N;

        Integer term;
        for (int i = static_cast<int>(d) - 1; i >= 1; --i) {
            result *= m;
            term = nf.coeff(i);
            term *= Integer(static_cast<int64_t>(i));
            result += term;
            result %= N;
        }
        if (result.is_negative()) result += N;
        return result;
    }

};

/// 便捷函数：计算代数平方根
[[nodiscard]] inline AlgebraicSqrtResult compute_algebraic_sqrt(
        const BitVector& dependency,
        const std::vector<Relation>& relations,
        const PolynomialContext& ctx) {

    AlgebraicSqrt calculator;
    return calculator.compute(dependency, relations, ctx);
}

/// 最终因子提取
/// 给定有理平方根 X 和代数平方根 Y
/// 计算 gcd(X - Y, N) 和 gcd(X + Y, N)
struct FactorResult {
    Integer factor1;      // gcd(X - Y, N)
    Integer factor2;      // gcd(X + Y, N)
    bool is_nontrivial;   // 是否找到非平凡因子
};

[[nodiscard]] inline FactorResult extract_factors(
        const Integer& rational_sqrt,
        const Integer& algebraic_sqrt,
        const Integer& n) {

    FactorResult result;

    // 计算 X - Y mod N (v22: 直接 assign)
    Integer diff;
    diff = rational_sqrt;
    diff -= algebraic_sqrt;
    diff %= n;
    if (diff.is_negative()) {
        diff += n;
    }

    // 计算 X + Y mod N
    Integer sum;
    sum = rational_sqrt;
    sum += algebraic_sqrt;
    sum %= n;

    // 计算 GCD
    result.factor1 = core::gcd(diff, n);
    result.factor2 = core::gcd(sum, n);

    // 检查是否非平凡
    result.is_nontrivial = false;

    auto check_nontrivial = [&n](const Integer& f) -> bool {
        if (f.is_one()) return false;       // f == 1 → trivial
        if (f.compare(n) == 0) return false; // f == N → trivial
        return true;                         // any other value → non-trivial
    };

    if (check_nontrivial(result.factor1) || check_nontrivial(result.factor2)) {
        result.is_nontrivial = true;
    }

    return result;
}

} // namespace gnfs::sqrt
