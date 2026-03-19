#pragma once

#include "smooth_check.hpp"
#include "trial_division.hpp"
#include "../core/polynomial_context.hpp"
#include "../core/relation.hpp"
#include "../factor_base/factor_base.hpp"
#include "../sieve/lattice_sieve.hpp"
#include "../util/safe_math.hpp"

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

/// Cofactorizer 统计
struct CofactorizerStats {
    size_t total_candidates = 0;
    size_t full_relations = 0;
    size_t partial_1lp = 0;
    size_t partial_2lp = 0;
    size_t rational_rejects = 0;
    size_t algebraic_rejects = 0;
    size_t both_rejects = 0;
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
        ++stats_.total_candidates;

        // 基本验证
        if (b == 0 || std::gcd(util::safe_abs(a), b) != 1) {
            return std::nullopt;
        }

        // 计算有理侧值和代数侧范数
        Integer rat_value = ctx_.rational_value(a, b);
        if (rat_value.is_negative()) {
            rat_value.negate();
        }

        // CRITICAL: Reject relations where gcd(a - b*m, N) > 1
        // Such relations would produce degenerate dependencies (product ≡ 0 mod N)
        Integer gcd_with_n = core::gcd(rat_value.clone(), ctx_.n());
        if (!gcd_with_n.is_one()) {
            return std::nullopt;  // This relation contains a factor of N
        }

        Integer alg_norm = ctx_.algebraic_norm(a, b);
        if (alg_norm.is_negative()) {
            alg_norm.negate();
        }

        // 试除有理侧
        auto rat_result = divider_.divide_rational(std::move(rat_value));

        // 试除代数侧
        auto alg_result = divider_.divide_algebraic(std::move(alg_norm), a, b);

        // 检查有理侧 cofactor
        CofactorClassification rat_class = classify_cofactor(
            rat_result.cofactor, large_prime_bound_);

        // 检查代数侧 cofactor
        CofactorClassification alg_class = classify_cofactor(
            alg_result.cofactor, large_prime_bound_);

        // 判断是否可接受
        bool rat_ok = is_acceptable_cofactor(rat_class);
        bool alg_ok = is_acceptable_cofactor(alg_class);

        if (!rat_ok && !alg_ok) {
            ++stats_.both_rejects;
            return std::nullopt;
        }
        if (!rat_ok) {
            ++stats_.rational_rejects;
            return std::nullopt;
        }
        if (!alg_ok) {
            ++stats_.algebraic_rejects;
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
        for (size_t i = 0; i < alg_result.factor_indices.size(); ++i) {
            for (uint8_t e = 0; e < alg_result.exponents[i]; ++e) {
                rel.algebraic_factors.push_back(alg_result.factor_indices[i]);
            }
        }

        // 添加代数侧大素数
        add_large_primes(rel.algebraic_large_prime, alg_class);

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

    /// 获取统计
    [[nodiscard]] const CofactorizerStats& stats() const noexcept {
        return stats_;
    }

    /// 重置统计
    void reset_stats() {
        stats_ = CofactorizerStats{};
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

    /// 添加大素数到关系
    void add_large_primes(Relation::LargePrimeList& list,
                          const CofactorClassification& cls) const {

        switch (cls.type) {
            case CofactorClass::Prime:
                list.push_back(PrimePower{
                    static_cast<uint32_t>(cls.factor1), 1});
                break;

            case CofactorClass::PrimePower:
                list.push_back(PrimePower{
                    static_cast<uint32_t>(cls.factor1), cls.power});
                break;

            case CofactorClass::Semiprime:
                list.push_back(PrimePower{
                    static_cast<uint32_t>(cls.factor1), 1});
                list.push_back(PrimePower{
                    static_cast<uint32_t>(cls.factor2), 1});
                break;

            default:
                break;
        }
    }

    /// 更新统计
    void update_stats(const Relation& rel) {
        size_t lp_count = rel.rational_large_prime.size() + rel.algebraic_large_prime.size();

        if (lp_count == 0) {
            ++stats_.full_relations;
        } else if (lp_count == 1) {
            ++stats_.partial_1lp;
        } else {
            ++stats_.partial_2lp;
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
