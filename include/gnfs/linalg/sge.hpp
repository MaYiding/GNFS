#pragma once

#include "sparse_matrix.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

namespace gnfs {
namespace linalg {

/// SGE (Structured Gaussian Elimination) 预处理结果
///
/// 在 Block Lanczos 之前对 GF(2) 矩阵进行预处理：
///   - 消除 weight-1 列（对应行可直接移除）
///   - 合并 weight-2 列（XOR 两行，消除一行）
///   - 反复迭代直到无变化
///
/// 典型降维 30-60%，显著减少 BL 迭代次数。
struct SGEResult {
    SparseMatrix reduced_matrix;   ///< 降维后的矩阵

    /// 行组成：reduced_row_i 由原始矩阵的哪些行 XOR 组成
    /// 用于将 BL 依赖展开回原始行索引
    std::vector<std::vector<size_t>> row_composition;

    /// 列映射：reduced_col_j → 原始列索引
    std::vector<uint32_t> col_map;

    // 统计
    size_t original_rows = 0;
    size_t original_cols = 0;
    size_t passes = 0;
    size_t weight1_eliminated = 0; ///< weight-1 消除的列/行数
    size_t weight2_merged = 0;     ///< weight-2 合并次数

    /// 将 BL 在降维矩阵上找到的依赖展开回原始行索引
    ///
    /// BL 返回 reduced_dep[i] = true/false 对每个降维行。
    /// 展开后得到 original_dep[j] = true/false 对每个原始行。
    /// 展开逻辑：对 reduced_dep 中每个 set bit i，将 row_composition[i]
    /// 中的原始行索引 XOR 入结果（GF(2) 语义）。
    [[nodiscard]] std::vector<bool> expand_dependency(
            const std::vector<bool>& reduced_dep) const {
        std::vector<bool> original(original_rows, false);

        for (size_t i = 0; i < reduced_dep.size() && i < row_composition.size(); ++i) {
            if (!reduced_dep[i]) continue;

            for (size_t orig_row : row_composition[i]) {
                original[orig_row] = !original[orig_row]; // XOR
            }
        }

        return original;
    }
};

/// SGE 配置
struct SGEConfig {
    size_t max_passes = 20;        ///< 最大迭代轮数
    bool eliminate_weight1 = true; ///< 消除 weight-1 列
    bool eliminate_weight2 = true; ///< 合并 weight-2 列
    bool verbose = false;          ///< 详细输出
};

/// Structured Gaussian Elimination 预处理
///
/// 标准 GNFS 预处理步骤，在 Block Lanczos 之前执行。
/// 通过消除低权重列来降低矩阵维度，减少 BL 运算量。
///
/// 参考：Cavallar (2000) "Strategies for Filtering in the Number Field Sieve"
class SGE {
public:
    /// 执行 SGE 预处理
    /// @param matrix 原始 GF(2) 稀疏矩阵
    /// @param config SGE 配置
    /// @return SGEResult 包含降维矩阵和映射信息
    [[nodiscard]] static SGEResult preprocess(
            const SparseMatrix& matrix,
            const SGEConfig& config = SGEConfig{}) {

        const size_t n_rows = matrix.num_rows();
        const size_t n_cols = matrix.num_cols();

        SGEResult result;
        result.original_rows = n_rows;
        result.original_cols = n_cols;

        if (n_rows == 0 || n_cols == 0) {
            result.reduced_matrix = SparseMatrix(0, 0);
            return result;
        }

        // ── 工作副本 ──
        // 复制行数据到工作区（避免修改原始矩阵）
        std::vector<SparseRow> working_rows(n_rows);
        for (size_t r = 0; r < n_rows; ++r) {
            // 深拷贝：获取排序后的索引，重新构造
            working_rows[r] = SparseRow(
                SparseRow::IndexList(matrix.row(r).indices()));
        }

        // 行组成追踪：working_rows[r] = XOR of original rows in composition[r]
        std::vector<std::vector<size_t>> composition(n_rows);
        for (size_t r = 0; r < n_rows; ++r) {
            composition[r] = {r};
        }

        // 活跃标记
        std::vector<bool> row_alive(n_rows, true);
        std::vector<bool> col_alive(n_cols, true);
        size_t alive_rows = n_rows;
        size_t alive_cols = n_cols;

        // ── 迭代消除 ──
        for (size_t pass = 0; pass < config.max_passes; ++pass) {
            size_t eliminated_this_pass = 0;

            // 构建列→行映射（只对活跃行/列）
            // col_to_rows[c] = 包含列 c 的活跃行列表
            std::vector<std::vector<size_t>> col_to_rows(n_cols);
            for (size_t r = 0; r < n_rows; ++r) {
                if (!row_alive[r]) continue;
                for (auto c : working_rows[r].indices()) {
                    if (c < n_cols && col_alive[c]) {
                        col_to_rows[c].push_back(r);
                    }
                }
            }

            // Phase 1: Weight-1 列消除
            if (config.eliminate_weight1) {
                for (uint32_t c = 0; c < n_cols; ++c) {
                    if (!col_alive[c]) continue;
                    if (col_to_rows[c].size() != 1) continue;

                    size_t r = col_to_rows[c][0];
                    if (!row_alive[r]) continue; // 可能已被其他 w1 消除

                    // 消除该行：它是列 c 的唯一贡献者
                    row_alive[r] = false;
                    --alive_rows;

                    // 同时消除列 c
                    col_alive[c] = false;
                    --alive_cols;

                    // 更新 col_to_rows: 移除 r 对其他列的贡献
                    for (auto c2 : working_rows[r].indices()) {
                        if (c2 < n_cols && col_alive[c2]) {
                            auto& rows = col_to_rows[c2];
                            rows.erase(
                                std::remove(rows.begin(), rows.end(), r),
                                rows.end());
                        }
                    }

                    ++eliminated_this_pass;
                    ++result.weight1_eliminated;
                }
            }

            // Phase 2: Weight-2 列合并（每次合并后 break 重建 col_to_rows）
            if (config.eliminate_weight2) {
                bool did_merge = false;
                for (uint32_t c = 0; c < n_cols; ++c) {
                    if (!col_alive[c]) continue;
                    if (col_to_rows[c].size() != 2) continue;

                    size_t r1 = col_to_rows[c][0];
                    size_t r2 = col_to_rows[c][1];
                    if (!row_alive[r1] || !row_alive[r2]) continue;

                    // 合并 r2 into r1: row[r1] = row[r1] XOR row[r2]
                    working_rows[r1].xor_with(working_rows[r2]);

                    // 更新行组成
                    auto& comp1 = composition[r1];
                    auto& comp2 = composition[r2];
                    // GF(2) XOR: 合并两个组成列表，去重（偶数次出现的抵消）
                    comp1.insert(comp1.end(), comp2.begin(), comp2.end());
                    std::sort(comp1.begin(), comp1.end());
                    std::vector<size_t> deduped;
                    deduped.reserve(comp1.size());
                    for (size_t i = 0; i < comp1.size(); ) {
                        size_t val = comp1[i];
                        size_t count = 1;
                        while (i + count < comp1.size() && comp1[i + count] == val) {
                            ++count;
                        }
                        if (count % 2 == 1) {
                            deduped.push_back(val);
                        }
                        i += count;
                    }
                    comp1 = std::move(deduped);

                    // 标记 r2 为死亡，消除列 c
                    row_alive[r2] = false;
                    --alive_rows;
                    col_alive[c] = false;
                    --alive_cols;

                    ++eliminated_this_pass;
                    ++result.weight2_merged;

                    // XOR changes r1's column set — col_to_rows is now stale.
                    // Break to rebuild from scratch on next pass iteration.
                    did_merge = true;
                    break;
                }

                // If a w2 merge happened, restart pass to rebuild col_to_rows
                if (did_merge) {
                    --pass;  // Counteract the ++pass at end of loop
                    continue;
                }
            }

            ++result.passes;

            if (config.verbose) {
                std::cout << "  SGE pass " << (pass + 1)
                          << ": rows=" << alive_rows
                          << " cols=" << alive_cols
                          << " (eliminated " << eliminated_this_pass << ")\n";
            }

            if (eliminated_this_pass == 0) break;
        }

        // ── 构建降维矩阵 ──

        // 列映射：active col index → original col index
        result.col_map.reserve(alive_cols);
        std::vector<uint32_t> old_to_new_col(n_cols, UINT32_MAX);
        for (uint32_t c = 0; c < n_cols; ++c) {
            if (col_alive[c]) {
                old_to_new_col[c] = static_cast<uint32_t>(result.col_map.size());
                result.col_map.push_back(c);
            }
        }

        // 构建降维矩阵
        result.reduced_matrix = SparseMatrix(alive_rows, alive_cols);
        result.row_composition.reserve(alive_rows);

        size_t new_row = 0;
        for (size_t r = 0; r < n_rows; ++r) {
            if (!row_alive[r]) continue;

            // 将工作行的活跃列映射到新列索引
            for (auto old_col : working_rows[r].indices()) {
                if (old_col < n_cols && old_to_new_col[old_col] != UINT32_MAX) {
                    result.reduced_matrix.row(new_row).set(old_to_new_col[old_col]);
                }
            }

            result.row_composition.push_back(std::move(composition[r]));
            ++new_row;
        }

        if (config.verbose) {
            std::cout << "  SGE done: " << n_rows << "×" << n_cols
                      << " → " << alive_rows << "×" << alive_cols
                      << " (" << result.passes << " passes"
                      << ", w1=" << result.weight1_eliminated
                      << ", w2=" << result.weight2_merged << ")\n";
        }

        return result;
    }
};

} // namespace linalg
} // namespace gnfs
