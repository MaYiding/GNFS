#pragma once

#include "base_m.hpp"
#include "kleinjung_selector.hpp"
#include "../core/params.hpp"
#include "../core/polynomial_context.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace gnfs::polynomial {

using core::Integer;
using core::GNFSParams;
using core::PolynomialContext;

/// 多项式选择自动分发
///
/// 根据 N 的位数和多项式度数自动选择最优算法：
///   - degree < 5:  BaseMSelector（含 Murphy E 排名）
///   - degree >= 5: KleinjungSelector（全量 Murphy E + skewness 优化）
///                  失败时回退到 BaseMSelector
///
/// 推荐使用 `select(n, params)` 重载，它从 GNFSParams 自动推导
/// Kleinjung 参数，确保多项式选择与因子基/筛法参数一致。
class SelectorDispatch {
public:
    /// 自动选择多项式（从 GNFSParams 推导参数）
    /// @param n 待分解的合数
    /// @param params GNFS 参数（含 degree 和自适应缩放的 poly 参数）
    /// @param verbose 是否输出选择过程信息
    /// @return PolynomialContext
    /// @throws std::runtime_error 如果所有选择器均失败
    [[nodiscard]] static PolynomialContext select(
            const Integer& n,
            const GNFSParams& params,
            bool verbose = false) {

        uint32_t degree = params.degree;
        size_t bits = n.bit_length();

        // Kleinjung 适用条件: degree >= 5
        // Kleinjung 通过 smooth 领导系数 + rotation 优化产生更优多项式。
        // 对 degree 3-4，BaseMSelector 的 Murphy E 已足够好。
        if (degree >= 5) {
            if (verbose) {
                std::cout << "  Selector: Kleinjung (degree=" << degree
                          << ", bits=" << bits << ")\n";
            }

            auto ctx = try_kleinjung_from_params(n, params, verbose);
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

        // BaseMSelector 路径
        auto poly_result = BaseMSelector::select(n, degree);
        if (!poly_result.success) {
            throw std::runtime_error(
                "Polynomial selection failed for " + std::to_string(bits) + "-bit N");
        }

        return BaseMSelector::create_context(n, poly_result);
    }

    /// 自动选择多项式（传统接口，内部构造 GNFSParams）
    /// @param n 待分解的合数
    /// @param degree 多项式度数
    /// @param verbose 是否输出选择过程信息
    /// @return PolynomialContext
    [[nodiscard]] static PolynomialContext select(
            const Integer& n,
            uint32_t degree,
            bool verbose = false) {

        // 构造 GNFSParams 以获取自适应参数
        auto params = GNFSParams::compute(n.bit_length());
        params.degree = degree;  // 尊重调用者指定的 degree
        return select(n, params, verbose);
    }

private:
    /// 尝试 Kleinjung 选择（从 GNFSParams 自动推导参数）
    [[nodiscard]] static std::optional<PolynomialContext> try_kleinjung_from_params(
            const Integer& n,
            const GNFSParams& params,
            bool verbose) {

        auto kparams = KleinjungParams::from_gnfs_params(params);

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
