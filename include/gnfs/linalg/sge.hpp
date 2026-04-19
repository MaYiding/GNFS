#pragma once

#include "sparse_matrix.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

namespace gnfs::linalg {

/// SGE (Structured Gaussian Elimination) 预处理结果
struct SGEResult {
    SparseMatrix reduced_matrix;

    /// 行组成：reduced_row_i 由原始矩阵的哪些行 XOR 组成
    std::vector<std::vector<size_t>> row_composition;

    /// 列映射：reduced_col_j → 原始列索引
    std::vector<uint32_t> col_map;

    size_t original_rows = 0;
    size_t original_cols = 0;
    size_t passes = 0;
    size_t weight1_eliminated = 0;
    size_t weight2_merged = 0;

    /// 将 BL 在降维矩阵上找到的依赖展开回原始行索引
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
    size_t max_passes = 100;       ///< 最大迭代轮数 (raised from 20)
    bool eliminate_weight1 = true;
    bool eliminate_weight2 = true;
    bool verbose = false;
};

/// Structured Gaussian Elimination 预处理
///
/// 标准 GNFS 预处理步骤。消除低权重列来降低矩阵维度。
/// 参考：Cavallar (2000) "Strategies for Filtering in the Number Field Sieve"
class SGE {
public:
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

        // ── Working copy ──
        std::vector<SparseRow> working_rows(n_rows);
        for (size_t r = 0; r < n_rows; ++r) {
            working_rows[r] = SparseRow(
                SparseRow::IndexList(matrix.row(r).indices()));
        }

        // Row composition tracking
        std::vector<std::vector<size_t>> composition(n_rows);
        for (size_t r = 0; r < n_rows; ++r) {
            composition[r] = {r};
        }

        // Active flags
        std::vector<bool> row_alive(n_rows, true);
        std::vector<bool> col_alive(n_cols, true);
        size_t alive_rows = n_rows;
        size_t alive_cols = n_cols;

        // ── Iterative elimination ──
        for (size_t pass = 0; pass < config.max_passes; ++pass) {
            size_t eliminated_this_pass = 0;

            // Build col→rows map for all active rows/cols
            std::vector<std::vector<size_t>> col_to_rows(n_cols);
            for (size_t r = 0; r < n_rows; ++r) {
                if (!row_alive[r]) continue;
                for (auto c : working_rows[r].indices()) {
                    if (c < n_cols && col_alive[c]) {
                        col_to_rows[c].push_back(r);
                    }
                }
            }

            // Phase 1: Eliminate ALL weight-1 columns (cascading)
            // When a row is removed, other columns lose a contributor.
            // Use a worklist to handle cascading w1 columns.
            if (config.eliminate_weight1) {
                // Seed worklist with all w1 columns
                std::vector<uint32_t> w1_work;
                for (uint32_t c = 0; c < n_cols; ++c) {
                    if (col_alive[c] && col_to_rows[c].size() == 1)
                        w1_work.push_back(c);
                }

                while (!w1_work.empty()) {
                    uint32_t c = w1_work.back();
                    w1_work.pop_back();
                    if (!col_alive[c]) continue;
                    if (col_to_rows[c].size() != 1) continue;

                    size_t r = col_to_rows[c][0];
                    if (!row_alive[r]) continue;

                    // Kill row and column
                    row_alive[r] = false;
                    col_alive[c] = false;
                    --alive_rows;
                    --alive_cols;

                    // Remove r from all its columns. If any become w1, add to worklist.
                    for (auto c2 : working_rows[r].indices()) {
                        if (c2 >= n_cols || !col_alive[c2]) continue;
                        auto& rows = col_to_rows[c2];
                        rows.erase(
                            std::remove(rows.begin(), rows.end(), r),
                            rows.end());
                        if (rows.size() == 1 && col_alive[c2])
                            w1_work.push_back(c2);
                        else if (rows.empty() && col_alive[c2]) {
                            // Column has no remaining rows — dead column
                            col_alive[c2] = false;
                            --alive_cols;
                        }
                    }

                    ++eliminated_this_pass;
                    ++result.weight1_eliminated;
                }
            }

            // Phase 2: Merge ALL weight-2 columns in one pass
            // For each w2 column c with rows {r1, r2}: merge r2 into r1, kill r2 and c.
            // Process greedily — if r2 was already killed by a prior merge, skip.
            if (config.eliminate_weight2) {
                for (uint32_t c = 0; c < n_cols; ++c) {
                    if (!col_alive[c]) continue;
                    if (col_to_rows[c].size() != 2) continue;

                    size_t r1 = col_to_rows[c][0];
                    size_t r2 = col_to_rows[c][1];
                    if (!row_alive[r1] || !row_alive[r2]) continue;

                    // Prefer merging into the heavier row (preserves more structure)
                    if (working_rows[r1].weight() < working_rows[r2].weight())
                        std::swap(r1, r2);

                    // Merge: row[r1] ^= row[r2]
                    working_rows[r1].xor_with(working_rows[r2]);

                    // Update composition: comp[r1] ^= comp[r2] (GF(2))
                    auto& comp1 = composition[r1];
                    auto& comp2 = composition[r2];
                    comp1.insert(comp1.end(), comp2.begin(), comp2.end());
                    std::sort(comp1.begin(), comp1.end());
                    // Deduplicate with GF(2) semantics: even count → cancel
                    std::vector<size_t> deduped;
                    deduped.reserve(comp1.size());
                    for (size_t i = 0; i < comp1.size(); ) {
                        size_t val = comp1[i];
                        size_t count = 1;
                        while (i + count < comp1.size() && comp1[i + count] == val)
                            ++count;
                        if (count % 2 == 1)
                            deduped.push_back(val);
                        i += count;
                    }
                    comp1 = std::move(deduped);

                    // Kill r2 and column c
                    row_alive[r2] = false;
                    col_alive[c] = false;
                    --alive_rows;
                    --alive_cols;

                    ++eliminated_this_pass;
                    ++result.weight2_merged;
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

        // ── Build reduced matrix ──
        result.col_map.reserve(alive_cols);
        std::vector<uint32_t> old_to_new_col(n_cols, UINT32_MAX);
        for (uint32_t c = 0; c < n_cols; ++c) {
            if (col_alive[c]) {
                old_to_new_col[c] = static_cast<uint32_t>(result.col_map.size());
                result.col_map.push_back(c);
            }
        }

        result.reduced_matrix = SparseMatrix(alive_rows, alive_cols);
        result.row_composition.reserve(alive_rows);

        size_t new_row = 0;
        for (size_t r = 0; r < n_rows; ++r) {
            if (!row_alive[r]) continue;

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

} // namespace gnfs::linalg
