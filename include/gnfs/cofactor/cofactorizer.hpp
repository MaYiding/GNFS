#pragma once

#include "smooth_check.hpp"
#include "trial_division.hpp"
#include "../core/polynomial_context.hpp"
#include "../core/relation.hpp"
#include "../factor_base/factor_base.hpp"
#include "../sieve/lattice_sieve.hpp"
#include "../util/safe_math.hpp"

#include <atomic>
#include <optional>

namespace gnfs {
namespace cofactor {

using core::Integer;
using core::PolynomialContext;
using core::PrimePower;
using core::Relation;
using factor_base::FactorBase;
using sieve::SieveCandidate;

/// 验证结果
enum class VerifyResult : uint8_t {
    Success = 0,           // 成功构建完整关系
    PartialSuccess = 1,    // 部分关系（有大素数）
    RationalFail = 2,      // 有理侧不光滑
    AlgebraicFail = 3,     // 代数侧不光滑
    BothFail = 4,          // 两侧都不光滑
    InvalidPair = 5        // 无效的 (a, b) 对
};

/// Cofactorizer 配置
struct CofactorizerConfig {
    uint64_t large_prime_bound = 0;      // 大素数上界 (0 = 使用因子基设置)
    bool allow_1lp = true;               // 允许 1 个大素数
    bool allow_2lp = true;               // 允许 2 个大素数
    bool allow_3lp = false;              // 允许 3 个大素数（通常禁用）
    size_t max_factorization_attempts = 10000;  // Pollard rho 最大尝试次数
};

/// Cofactorizer 统计（原子操作，线程安全）
struct CofactorizerStats {
    std::atomic<size_t> total_candidates{0};
    std::atomic<size_t> full_relations{0};
    std::atomic<size_t> partial_1lp{0};
    std::atomic<size_t> partial_2lp{0};
    std::atomic<size_t> rational_rejects{0};
    std::atomic<size_t> algebraic_rejects{0};
    std::atomic<size_t> both_rejects{0};

    CofactorizerStats() = default;

    // Non-atomic snapshot for reading
    struct Snapshot {
        size_t total_candidates;
        size_t full_relations;
        size_t partial_1lp;
        size_t partial_2lp;
        size_t rational_rejects;
        size_t algebraic_rejects;
        size_t both_rejects;
    };

    [[nodiscard]] Snapshot snapshot() const noexcept {
        return {total_candidates.load(std::memory_order_relaxed),
                full_relations.load(std::memory_order_relaxed),
                partial_1lp.load(std::memory_order_relaxed),
                partial_2lp.load(std::memory_order_relaxed),
                rational_rejects.load(std::memory_order_relaxed),
                algebraic_rejects.load(std::memory_order_relaxed),
                both_rejects.load(std::memory_order_relaxed)};
    }

    void reset() noexcept {
        total_candidates.store(0, std::memory_order_relaxed);
        full_relations.store(0, std::memory_order_relaxed);
        partial_1lp.store(0, std::memory_order_relaxed);
        partial_2lp.store(0, std::memory_order_relaxed);
        rational_rejects.store(0, std::memory_order_relaxed);
        algebraic_rejects.store(0, std::memory_order_relaxed);
        both_rejects.store(0, std::memory_order_relaxed);
    }
};

/// Cofactorizer - 主要的分解验证类
/// 验证筛法候选并构建完整关系
class Cofactorizer {
public:
    /// 构造函数
    /// @param ctx 多项式上下文
    /// @param fb 因子基
    /// @param config 配置
    Cofactorizer(const PolynomialContext& ctx,
                 const FactorBase& fb,
                 const CofactorizerConfig& config = CofactorizerConfig{})
        : ctx_(ctx)
        , fb_(fb)
        , config_(config)
        , divider_(fb) {

        // 设置大素数上界
        if (config_.large_prime_bound == 0) {
            large_prime_bound_ = fb_.params().large_prime_bound;
        } else {
            large_prime_bound_ = config_.large_prime_bound;
        }

        // 构建系数向量
        for (uint32_t i = 0; i <= ctx_.degree(); ++i) {
            coeffs_.push_back(ctx_.coeff(i).clone());
        }
    }

    /// 验证单个候选
    /// @param cand 筛法候选
    /// @return 如果成功，返回完整关系；否则返回空
    [[nodiscard]] std::optional<Relation> verify(const SieveCandidate& cand) {
        return verify(cand.a, cand.b);
    }

    /// 验证 (a, b) 对
    /// @param a, b 候选对
    /// @return 如果成功，返回完整关系；否则返回空
    [[nodiscard]] std::optional<Relation> verify(int64_t a, uint64_t b) {
        stats_.total_candidates.fetch_add(1, std::memory_order_relaxed);

        // 基本验证
        if (b == 0 || std::gcd(util::safe_abs(a), b) != 1) {
            return std::nullopt;
        }

        // ── 有理侧: 计算值 + N-divisible 检查 + 试除 + 分类 ──
        // Rational-first 短路: 如果有理侧不可接受，立即返回，跳过代数试除
        Integer rat_value = ctx_.rational_value(a, b);
        if (rat_value.is_negative()) {
            rat_value.negate();
        }

        // CRITICAL: Reject relations where gcd(a - b*m, N) > 1
        // uint64 快路径: 小值用原生 gcd，大值用 GMP
        {
            if (rat_value.fits_uint64() && ctx_.n().fits_uint64()) {
                uint64_t rv = rat_value.to_uint64();
                uint64_t nv = ctx_.n().to_uint64();
                if (std::gcd(rv, nv) != 1) return std::nullopt;
            } else {
                Integer gcd_with_n = core::gcd(rat_value.clone(), ctx_.n());
                if (!gcd_with_n.is_one()) return std::nullopt;
            }
        }

        auto rat_result = divider_.divide_rational(std::move(rat_value));
        CofactorClassification rat_class = classify_cofactor(
            rat_result.cofactor, large_prime_bound_);

        // Rational-first 短路: 有理侧不可接受 → 跳过代数试除
        if (!is_acceptable_cofactor(rat_class)) {
            stats_.rational_rejects.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        // ── 代数侧: 计算范数 + 试除 + 分类 ──
        // __int128 快路径: 避免 degree+1 次 GMP 堆分配（小系数 + 小 a,b 时）
        Integer alg_norm;
        {
            auto [norm_i128, ok] = ctx_.algebraic_norm_i128(a, b);
            if (ok) {
                if (norm_i128 < 0) norm_i128 = -norm_i128;
                if (norm_i128 <= static_cast<__int128>(UINT64_MAX)) {
                    alg_norm = Integer(static_cast<uint64_t>(norm_i128));
                } else {
                    // Fits __int128 but not uint64 — construct via string or GMP
                    alg_norm = ctx_.algebraic_norm(a, b);
                    if (alg_norm.is_negative()) alg_norm.negate();
                }
            } else {
                alg_norm = ctx_.algebraic_norm(a, b);
                if (alg_norm.is_negative()) alg_norm.negate();
            }
        }

        auto alg_result = divider_.divide_algebraic(std::move(alg_norm), a, b);
        CofactorClassification alg_class = classify_cofactor(
            alg_result.cofactor, large_prime_bound_);

        if (!is_acceptable_cofactor(alg_class)) {
            stats_.algebraic_rejects.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        // 构建关系
        Relation rel(a, b);

        // 添加有理侧因子
        for (size_t i = 0; i < rat_result.factor_indices.size(); ++i) {
            // 每个因子添加 exponent 次
            for (uint8_t e = 0; e < rat_result.exponents[i]; ++e) {
                rel.rational_factors.push_back(rat_result.factor_indices[i]);
            }
        }

        // 添加有理侧大素数
        add_large_primes(rel.rational_large_prime, rat_class);

        // 添加代数侧因子
        // IMPORTANT: Factor base indices >= sieve_algebraic_count() are in the
        // special-Q range. These primes are included in the FB for sieving but
        // do NOT have matrix columns. Route them to algebraic_large_prime instead,
        // so the matrix builder tracks them via large prime columns.
        {
            size_t sieve_alg_count = fb_.sieve_algebraic_count();
            const auto& alg_primes = fb_.algebraic();
            for (size_t i = 0; i < alg_result.factor_indices.size(); ++i) {
                uint32_t idx = alg_result.factor_indices[i];
                uint8_t exp = alg_result.exponents[i];
                if (idx < sieve_alg_count) {
                    // Standard factor base prime — add to algebraic_factors
                    for (uint8_t e = 0; e < exp; ++e) {
                        rel.algebraic_factors.push_back(idx);
                    }
                } else {
                    // Special-Q range prime — route to large primes
                    // Use the stored root from the factor base
                    uint32_t p = alg_primes[idx].p;
                    uint32_t r = alg_primes[idx].r;
                    rel.algebraic_large_prime.push_back(
                        PrimePower{p, r, exp});
                }
            }
        }

        // 添加代数侧大素数（含正确的根 r = a·b⁻¹ mod p）
        add_algebraic_large_primes(rel.algebraic_large_prime, alg_class, a, b);

        // 更新统计
        update_stats(rel);

        return rel;
    }

    /// 批量验证
    /// @param candidates 候选列表
    /// @return 成功验证的关系列表
    [[nodiscard]] std::vector<Relation> verify_batch(
            const std::vector<SieveCandidate>& candidates) {

        std::vector<Relation> relations;
        relations.reserve(candidates.size() / 10);  // 估计约 10% 成功率

        for (const auto& cand : candidates) {
            auto rel = verify(cand);
            if (rel) {
                relations.push_back(std::move(*rel));
            }
        }

        return relations;
    }

    /// 获取统计快照
    [[nodiscard]] CofactorizerStats::Snapshot stats() const noexcept {
        return stats_.snapshot();
    }

    /// 重置统计
    void reset_stats() noexcept {
        stats_.reset();
    }

private:
    const PolynomialContext& ctx_;
    const FactorBase& fb_;
    CofactorizerConfig config_;
    TrialDivider divider_;
    uint64_t large_prime_bound_;
    std::vector<Integer> coeffs_;
    CofactorizerStats stats_;

    /// 检查 cofactor 分类是否可接受
    [[nodiscard]] bool is_acceptable_cofactor(const CofactorClassification& cls) const noexcept {
        switch (cls.type) {
            case CofactorClass::Smooth:
                return true;

            case CofactorClass::Prime:
            case CofactorClass::PrimePower:
                return config_.allow_1lp;

            case CofactorClass::Semiprime:
                return config_.allow_2lp;

            case CofactorClass::Composite:
                return config_.allow_3lp;  // 可能是 3LP

            case CofactorClass::TooLarge:
            case CofactorClass::Unknown:
                return false;

            default:
                return false;
        }
    }

    /// 添加大素数到关系（有理侧，无需根）
    void add_large_primes(Relation::LargePrimeList& list,
                          const CofactorClassification& cls) const {

        switch (cls.type) {
            case CofactorClass::Prime:
                list.push_back(PrimePower{cls.factor1, static_cast<uint8_t>(1)});
                break;

            case CofactorClass::PrimePower:
                list.push_back(PrimePower{cls.factor1, cls.power});
                break;

            case CofactorClass::Semiprime:
                list.push_back(PrimePower{cls.factor1, static_cast<uint8_t>(1)});
                list.push_back(PrimePower{cls.factor2, static_cast<uint8_t>(1)});
                break;

            default:
                break;
        }
    }

    /// 计算代数侧大素数对应的根 r = a·b⁻¹ mod p
    /// 这是 f(x) mod p 的一个根，标识 p 上方的具体素理想 (p, α-r)
    [[nodiscard]] static uint64_t compute_alg_lp_root(int64_t a, uint64_t b, uint64_t p) {
        uint64_t a_mod = static_cast<uint64_t>(
            ((a % static_cast<int64_t>(p)) + static_cast<int64_t>(p)) % static_cast<int64_t>(p));
        // b^{-1} mod p via extended GCD
        uint64_t b_mod = b % p;
        // p ∤ b is guaranteed (see proof: if p|b and gcd(a,b)=1 then p ∤ Norm)
        int64_t t = 0, nt = 1;
        int64_t r = static_cast<int64_t>(p), nr = static_cast<int64_t>(b_mod);
        while (nr != 0) {
            int64_t q = r / nr;
            t -= q * nt; std::swap(t, nt);
            r -= q * nr; std::swap(r, nr);
        }
        uint64_t b_inv = static_cast<uint64_t>((t % static_cast<int64_t>(p) +
                                                  static_cast<int64_t>(p)) % static_cast<int64_t>(p));
        return static_cast<uint64_t>(
            (static_cast<__uint128_t>(a_mod) * b_inv) % p);
    }

    /// 添加代数侧大素数（带正确的素理想根 r）
    void add_algebraic_large_primes(Relation::LargePrimeList& list,
                                     const CofactorClassification& cls,
                                     int64_t a, uint64_t b) const {
        switch (cls.type) {
            case CofactorClass::Prime: {
                uint64_t r = compute_alg_lp_root(a, b, cls.factor1);
                list.push_back(PrimePower{cls.factor1, r, static_cast<uint8_t>(1)});
                break;
            }
            case CofactorClass::PrimePower: {
                uint64_t r = compute_alg_lp_root(a, b, cls.factor1);
                list.push_back(PrimePower{cls.factor1, r, cls.power});
                break;
            }
            case CofactorClass::Semiprime: {
                uint64_t r1 = compute_alg_lp_root(a, b, cls.factor1);
                uint64_t r2 = compute_alg_lp_root(a, b, cls.factor2);
                list.push_back(PrimePower{cls.factor1, r1, static_cast<uint8_t>(1)});
                list.push_back(PrimePower{cls.factor2, r2, static_cast<uint8_t>(1)});
                break;
            }
            default:
                break;
        }
    }

    /// 更新统计
    void update_stats(const Relation& rel) {
        size_t lp_count = rel.rational_large_prime.size() + rel.algebraic_large_prime.size();

        if (lp_count == 0) {
            stats_.full_relations.fetch_add(1, std::memory_order_relaxed);
        } else if (lp_count == 1) {
            stats_.partial_1lp.fetch_add(1, std::memory_order_relaxed);
        } else {
            stats_.partial_2lp.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

/// 便捷函数：验证单个候选
[[nodiscard]] inline std::optional<Relation> verify_candidate(
        const PolynomialContext& ctx,
        const FactorBase& fb,
        int64_t a, uint64_t b,
        uint64_t large_prime_bound = 0) {

    CofactorizerConfig config;
    config.large_prime_bound = large_prime_bound;

    Cofactorizer cofactorizer(ctx, fb, config);
    return cofactorizer.verify(a, b);
}

} // namespace cofactor
} // namespace gnfs
