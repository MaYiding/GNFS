#pragma once

#include "base_m.hpp"
#include "kleinjung_selector.hpp"
#include "../core/polynomial_context.hpp"

#include <iostream>
#include <stdexcept>

namespace gnfs::polynomial {

using core::Integer;
using core::PolynomialContext;

/// 多项式选择自动分发
///
/// 根据 N 的位数和多项式度数自动选择最优算法：
///   - degree < 5:  BaseMSelector（含 Murphy E 排名）
///   - degree >= 5: KleinjungSelector（全量 Murphy E + skewness 优化）
///                  失败时回退到 BaseMSelector
///
/// BaseMSelector 对 46+ 位 N 已内置 Murphy E 评估（自适应搜索窗口），
/// 因此 degree 3-4 范围内（≤~200 位 N）无需 Kleinjung。
/// Kleinjung 的优势在于 degree ≥ 5（~250+ 位 N），通过 smooth 领导系数
/// 和 Newton root 优化产生显著更优的多项式。
class SelectorDispatch {
public:
    /// 自动选择多项式
    /// @param n 待分解的合数
    /// @param degree 多项式度数（通常由 GNFSParams::compute 确定）
    /// @param verbose 是否输出选择过程信息
    /// @return PolynomialContext
    /// @throws std::runtime_error 如果所有选择器均失败
    [[nodiscard]] static PolynomialContext select(
            const Integer& n,
            uint32_t degree,
            bool verbose = false) {

        size_t bits = n.bit_length();

        // Kleinjung 适用条件：degree >= 5 AND bits >= 300 (~100 digit)
        // 80-digit (264 bits) with degree 5: base-m is sufficient.
        // Kleinjung's poly quality advantage only pays off at 100+ digits
        // where sieve time dominates and a 10-20% quality gain matters.
        if (degree >= 5 && bits >= 300) {
            if (verbose) {
                std::cout << "  Selector: Kleinjung (degree=" << degree
                          << ", bits=" << bits << ")\n";
            }

            auto ctx = try_kleinjung(n, degree, bits, verbose);
            if (ctx.has_value()) {
                return std::move(*ctx);
            }

            if (verbose) {
                std::cout << "  Kleinjung failed, falling back to BaseMSelector\n";
            }
        } else {
            if (verbose) {
                std::cout << "  Selector: BaseMSelector (degree=" << degree
                          << ", bits=" << bits << ")\n";
            }
        }

        // BaseMSelector 路径（degree < 5 或 Kleinjung 回退）
        auto poly_result = BaseMSelector::select(n, degree);
        if (!poly_result.success) {
            throw std::runtime_error(
                "Polynomial selection failed for " + std::to_string(bits) + "-bit N");
        }

        return BaseMSelector::create_context(n, poly_result);
    }

private:
    /// 尝试 Kleinjung 选择
    [[nodiscard]] static std::optional<PolynomialContext> try_kleinjung(
            const Integer& n,
            uint32_t degree,
            size_t bits,
            bool verbose) {

        KleinjungParams kparams;
        kparams.degree = degree;

        // 按 N 位数调整参数
        // 注意：Kleinjung 搜索时间 ∝ leading_coeff_bound × candidates
        // 80-digit (264 bit) 应在 ≤2 min 内完成 poly 选择
        if (bits <= 150) {
            // 中等大小：少量候选，快速评估
            kparams.num_candidates = 300;
            kparams.root_opt_iterations = 128;
            kparams.leading_coeff_bound = 5000;
            kparams.search_radius = 50;
        } else if (bits <= 300) {
            // 标准规模 (80-90 digit)
            kparams.num_candidates = 1000;
            kparams.root_opt_iterations = 256;
            kparams.leading_coeff_bound = 10000;
            kparams.search_radius = 100;
        } else {
            // 大规模 (100+ digit): 更多候选，更精细优化
            kparams.num_candidates = 3000;
            kparams.root_opt_iterations = 512;
            kparams.leading_coeff_bound = 50000;
            kparams.search_radius = 200;
        }

        KleinjungSelector selector(kparams);
        auto result = selector.select(n);

        if (!result.success) {
            return std::nullopt;
        }

        if (verbose) {
            std::cout << "  Kleinjung: Murphy E = " << result.score.log_e_score
                      << ", skewness = " << result.skewness
                      << ", candidates tested = " << result.candidates_tested
                      << " (" << result.elapsed_seconds << "s)\n";
        }

        return create_context_from_kleinjung(n, result);
    }
};

} // namespace gnfs::polynomial
