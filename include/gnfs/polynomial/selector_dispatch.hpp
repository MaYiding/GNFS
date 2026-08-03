#pragma once

#include "../core/params.hpp"
#include "../core/polynomial_context.hpp"
#include "bai_brent_selector.hpp"
#include "base_m.hpp"
#include "kleinjung_selector.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace gnfs::polynomial {

using core::GNFSParams;
using core::Integer;
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
    [[nodiscard]] static PolynomialContext select(const Integer& n, const GNFSParams& params,
                                                  bool verbose = false) {

        uint32_t degree = params.degree;
        size_t bits = n.bit_length();

        // 警告 degree/bits 不匹配的情况:
        //   - bits>=120 但 degree=4 → 失去 Kleinjung 的优势 (Murphy E 偏低)
        //   - bits<60 但 degree=5  → Kleinjung 浪费 (BaseMSelector 已足够)
        // 不强制改 degree (用户可能有理由),但 verbose 模式提示。
        if (verbose) {
            if (bits >= 120 && degree < 5) {
                std::cerr << "  [warn] N has " << bits << " bits but degree=" << degree
                          << "; consider degree=5+ for Kleinjung" << std::endl;
            } else if (bits < 60 && degree >= 5) {
                std::cerr << "  [warn] N has " << bits << " bits but degree=" << degree
                          << "; BaseM is typically sufficient for small N" << std::endl;
            }
        }

        // Kleinjung 适用条件: degree >= 5
        // Kleinjung 通过 smooth 领导系数 + rotation 优化产生更优多项式。
        // 对 degree 3-4，BaseMSelector 的 Murphy E 已足够好。
        //
        // BaiBrent 扩展: ENV `GNFS_POLY_BAI_BRENT=1` 启用非首一选择 (Bai 2011)。
        // 默认 OFF — Kleinjung path 完整保留, ENV opt-in 时优先 BaiBrent, 失败
        // 回退到 Kleinjung, 仍失败则回退 BaseMSelector。
        if (degree >= 5) {
            const bool use_bai_brent = []() {
                const char* env = std::getenv("GNFS_POLY_BAI_BRENT");
                return env && env[0] == '1';
            }();

            if (use_bai_brent) {
                if (verbose) {
                    std::cerr << "  Selector: BaiBrent (degree=" << degree << ", bits=" << bits
                              << ", GNFS_POLY_BAI_BRENT=1)\n";
                }

                auto ctx = try_bai_brent_from_params(n, params, verbose);
                if (ctx.has_value()) {
                    return std::move(*ctx);
                }

                if (verbose) {
                    std::cerr << "  BaiBrent failed, falling back to Kleinjung\n";
                }
            } else if (verbose) {
                std::cerr << "  Selector: Kleinjung (degree=" << degree << ", bits=" << bits
                          << ")\n";
            }

            auto ctx = try_kleinjung_from_params(n, params, verbose);
            if (ctx.has_value()) {
                return std::move(*ctx);
            }

            if (verbose) {
                std::cerr << "  Kleinjung failed, falling back to BaseMSelector\n";
            }
        } else {
            if (verbose) {
                std::cerr << "  Selector: BaseMSelector (degree=" << degree << ", bits=" << bits
                          << ")\n";
            }
        }

        // BaseMSelector 路径
        auto poly_result = BaseMSelector::select(n, degree);
        if (!poly_result.success) {
            throw std::runtime_error("Polynomial selection failed for " + std::to_string(bits) +
                                     "-bit N");
        }

        return BaseMSelector::create_context(n, poly_result);
    }

    /// 自动选择多项式（传统接口，内部构造 GNFSParams）
    /// @param n 待分解的合数
    /// @param degree 多项式度数
    /// @param verbose 是否输出选择过程信息
    /// @return PolynomialContext
    [[nodiscard]] static PolynomialContext select(const Integer& n, uint32_t degree,
                                                  bool verbose = false) {

        // 构造 GNFSParams 以获取自适应参数
        auto params = GNFSParams::compute(n.bit_length());
        params.degree = degree; // 尊重调用者指定的 degree
        return select(n, params, verbose);
    }

private:
    /// 尝试 Kleinjung 选择（从 GNFSParams 自动推导参数）
    [[nodiscard]] static std::optional<PolynomialContext>
    try_kleinjung_from_params(const Integer& n, const GNFSParams& params, bool verbose) {

        auto kparams = KleinjungParams::from_gnfs_params(params);

        KleinjungSelector selector(kparams);
        auto result = selector.select(n);

        if (!result.success) {
            return std::nullopt;
        }

        if (verbose) {
            std::cerr << "  Kleinjung: Murphy E = " << result.score.log_e_score
                      << ", skewness = " << result.skewness
                      << ", candidates tested = " << result.candidates_tested << " ("
                      << result.elapsed_seconds << "s)\n";
        }

        return create_context_from_kleinjung(n, result);
    }

    /// 尝试 Bai-Brent 非首一选择 (从 GNFSParams 自动推导参数)
    [[nodiscard]] static std::optional<PolynomialContext>
    try_bai_brent_from_params(const Integer& n, const GNFSParams& params, bool verbose) {

        auto bp = BaiBrentParams::from_gnfs_params(params);

        BaiBrentSelector selector(bp);
        auto result = selector.select(n);

        if (!result.success) {
            return std::nullopt;
        }

        if (verbose) {
            std::cerr << "  BaiBrent: Murphy E = " << result.score.log_e_score
                      << ", skewness = " << result.skewness
                      << ", candidates tested = " << result.candidates_tested
                      << ", a_d = " << result.f.leading_coeff().to_string() << " ("
                      << result.elapsed_seconds << "s)\n";
        }

        return create_context_from_bai_brent(n, result);
    }
};

} // namespace gnfs::polynomial
