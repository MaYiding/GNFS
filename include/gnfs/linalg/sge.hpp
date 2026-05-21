#pragma once

#include "sge_batch_pivots.hpp"
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
    /// weight-2 merges 因 composition cap 跳过的次数 (BACKLOG #6 safety).
    /// >0 表示触发 cap, 防止 row_composition O(n) 膨胀 BUT 也限制 merge throughput.
    size_t weight2_skipped_cap = 0;

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
    /// row_composition[r1] size 上限 (BACKLOG #6 safety). 超过时 skip 这个
    /// weight-2 merge, 防止 chain merge 累积 O(n) composition. 16K = ~128 KB
    /// per row composition (heuristic; n_rows ~50K-300K matrix, 16K 即 5-30% n).
    /// 0 = no cap.
    size_t row_composition_cap = 16384;

    /// Batch size for disjoint-row pivot selection per pass. 1 (default) runs
    /// the original sequential per-pivot path with zero overhead and bit-for-
    /// bit identical output. Values >= 2 collect up to N pivots whose row
    /// supports do not overlap within a batch, then apply them sequentially
    /// (the row supports being disjoint makes the apply order immaterial).
    ///
    /// The default value is 0, which is interpreted as "consult the
    /// `GNFS_SGE_BATCH_PIVOTS` environment variable" inside `preprocess`.
    /// Tests may override this field directly to bypass the ENV lookup
    /// (e.g., parity comparisons that need both N=1 and N=8 paths in the
    /// same process without juggling environment state).
    ///
    /// Setting batch_pivots > 64 is clamped to 64 to bound the per-batch
    /// row-usage tracking memory.
    int batch_pivots = 0;
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

        // Resolve the effective batch-pivot size exactly once for this
        // preprocess call. `batch_pivots == 0` means "fall back to the ENV";
        // any explicit positive value is honoured (and clamped to the same
        // [1, kSGEBatchPivotsMax] bracket the ENV uses) so tests can drive
        // both paths from the same process without environment mutation.
        const int effective_batch = [&]() noexcept {
            int raw = config.batch_pivots > 0
                          ? config.batch_pivots
                          : sge_batch_pivots_size();
            if (raw < 1) raw = 1;
            if (raw > kSGEBatchPivotsMax) raw = kSGEBatchPivotsMax;
            return raw;
        }();

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

        // col_to_rows 在 pass 循环外预分配,且只在 pass 0 之前构建一次。
        // 之后每 pass 都通过 Phase 1 worklist + Phase 2 incremental update 维护
        // (而非冷启动重建)。
        //
        // 旧实现 (commit a203aee, v10): 每 pass 顶端 for-clear + 重建,虽
        // 保留 capacity 但仍要扫遍全部 alive rows × nnz_per_row = O(nnz)
        // 操作。100 passes × 1e7 nnz = 1e9 push_back。
        //
        // 新实现 (v15): pass 0 之前构建一次,后续 Phase 1/2 增量。
        std::vector<std::vector<size_t>> col_to_rows(n_cols);
        for (size_t r = 0; r < n_rows; ++r) {
            if (!row_alive[r]) continue;
            for (auto c : working_rows[r].indices()) {
                if (c < n_cols && col_alive[c]) {
                    col_to_rows[c].push_back(r);
                }
            }
        }

        // ── Per-pivot apply helpers ──
        //
        // These lambdas encapsulate the original per-pivot transformations so
        // both the sequential and batched drivers below can dispatch through a
        // shared implementation. Each lambda mutates the working state
        // (`working_rows`, `composition`, `row_alive`, `col_alive`,
        // `alive_rows`, `alive_cols`, `col_to_rows`, `result`) exactly the
        // same way the original inline code did. Conflict detection (the
        // "batched" property) is applied externally by the caller via the
        // pivot-selection loop; each apply itself runs as if it were the only
        // pivot in flight, which is safe because batches are chosen so the
        // affected row sets are disjoint.
        //
        // apply_w1_pivot returns true if the pivot was applied; false if the
        // column / row was no longer eligible (e.g., killed by an earlier
        // pivot in the same batch via the worklist seed). The sequential
        // worklist already tolerates skips, so the batched path keeps the
        // same contract.
        std::vector<uint32_t> w1_work;
        w1_work.reserve(n_cols / 8);

        auto apply_w1_pivot = [&](uint32_t c) -> bool {
            if (c >= n_cols || !col_alive[c]) return false;
            if (col_to_rows[c].size() != 1) return false;

            const size_t r = col_to_rows[c][0];
            if (!row_alive[r]) return false;

            row_alive[r] = false;
            col_alive[c] = false;
            --alive_rows;
            --alive_cols;

            for (auto c2 : working_rows[r].indices()) {
                if (c2 >= n_cols || !col_alive[c2]) continue;
                auto& rows = col_to_rows[c2];
                rows.erase(std::remove(rows.begin(), rows.end(), r),
                           rows.end());
                if (rows.size() == 1 && col_alive[c2])
                    w1_work.push_back(c2);
                else if (rows.empty() && col_alive[c2]) {
                    col_alive[c2] = false;
                    --alive_cols;
                }
            }
            ++result.weight1_eliminated;
            return true;
        };

        // apply_w2_pivot returns true if the merge was applied. False on
        // either (a) eligibility lapsed (row/column killed by an earlier
        // pivot) or (b) the row-composition cap rejected the merge.
        auto apply_w2_pivot = [&](uint32_t c) -> bool {
            if (!col_alive[c]) return false;
            if (col_to_rows[c].size() != 2) return false;

            size_t r1 = col_to_rows[c][0];
            size_t r2 = col_to_rows[c][1];
            if (!row_alive[r1] || !row_alive[r2]) return false;

            if (working_rows[r1].weight() < working_rows[r2].weight())
                std::swap(r1, r2);

            if (config.row_composition_cap > 0) {
                size_t prospective =
                    composition[r1].size() + composition[r2].size();
                if (prospective > config.row_composition_cap) {
                    ++result.weight2_skipped_cap;
                    return false;
                }
            }

            auto old_r1_indices = working_rows[r1].indices();
            auto old_r2_indices = working_rows[r2].indices();

            working_rows[r1].xor_with(working_rows[r2]);

            auto& comp1 = composition[r1];
            auto& comp2 = composition[r2];
            comp1.reserve(comp1.size() + comp2.size());
            comp1.insert(comp1.end(), comp2.begin(), comp2.end());
            std::sort(comp1.begin(), comp1.end());
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

            auto it1 = old_r1_indices.begin();
            for (auto c2_raw : old_r2_indices) {
                if (c2_raw >= n_cols) continue;
                size_t c2 = static_cast<size_t>(c2_raw);
                if (!col_alive[c2]) continue;

                while (it1 != old_r1_indices.end() && *it1 < c2_raw) ++it1;
                bool was_in_r1 =
                    (it1 != old_r1_indices.end() && *it1 == c2_raw);

                auto& rows = col_to_rows[c2];

                rows.erase(std::remove(rows.begin(), rows.end(), r2),
                           rows.end());

                if (was_in_r1) {
                    rows.erase(std::remove(rows.begin(), rows.end(), r1),
                               rows.end());
                } else {
                    rows.push_back(r1);
                }

                if (rows.empty() && col_alive[c2] && c2 != c) {
                    col_alive[c2] = false;
                    --alive_cols;
                }
            }

            row_alive[r2] = false;
            col_alive[c] = false;
            --alive_rows;
            --alive_cols;
            ++result.weight2_merged;
            return true;
        };

        // ── Iterative elimination ──
        for (size_t pass = 0; pass < config.max_passes; ++pass) {
            size_t eliminated_this_pass = 0;

            // Phase 1: Eliminate ALL weight-1 columns (cascading)
            // When a row is removed, other columns lose a contributor.
            // Use a worklist to handle cascading w1 columns.
            if (config.eliminate_weight1) {
                w1_work.clear();
                for (uint32_t c = 0; c < n_cols; ++c) {
                    if (col_alive[c] && col_to_rows[c].size() == 1)
                        w1_work.push_back(c);
                }

                if (effective_batch == 1) {
                    // Original sequential worklist path. Identical bytes to
                    // the historical implementation (see git history before
                    // this commit).
                    while (!w1_work.empty()) {
                        uint32_t c = w1_work.back();
                        w1_work.pop_back();
                        if (apply_w1_pivot(c)) {
                            ++eliminated_this_pass;
                        }
                    }
                } else {
                    // Batched path: drain the worklist by taking up to
                    // `effective_batch` candidates whose single-row supports
                    // are disjoint from rows already chosen in the current
                    // batch. The "row support" of a w1 column is its sole
                    // owning row; two w1 columns conflict iff they reference
                    // the same row (which means killing one would invalidate
                    // the other before the batch finishes).
                    //
                    // Per-batch state lives in `batch_cols` and `used_rows`.
                    // Both reset between batches, so the bookkeeping cost is
                    // O(batch) per batch — negligible against the apply cost.
                    std::vector<uint32_t> batch_cols;
                    batch_cols.reserve(static_cast<size_t>(effective_batch));
                    std::vector<size_t> used_rows;
                    used_rows.reserve(static_cast<size_t>(effective_batch));
                    std::vector<uint32_t> deferred;
                    deferred.reserve(static_cast<size_t>(effective_batch));

                    while (!w1_work.empty()) {
                        batch_cols.clear();
                        used_rows.clear();
                        deferred.clear();

                        // Greedily pull from the back of the worklist into
                        // the batch as long as the candidate's row is fresh.
                        // A candidate whose row collides with a row already
                        // chosen this batch goes onto `deferred` so it is
                        // reconsidered after the batch applies; this keeps
                        // the algorithm progressing even when many w1
                        // columns share rows.
                        while (!w1_work.empty() &&
                               batch_cols.size() <
                                   static_cast<size_t>(effective_batch)) {
                            uint32_t c = w1_work.back();
                            w1_work.pop_back();
                            if (c >= n_cols || !col_alive[c]) continue;
                            if (col_to_rows[c].size() != 1) continue;
                            size_t r = col_to_rows[c][0];
                            if (!row_alive[r]) continue;
                            bool conflict = false;
                            for (size_t ur : used_rows) {
                                if (ur == r) { conflict = true; break; }
                            }
                            if (conflict) {
                                deferred.push_back(c);
                            } else {
                                used_rows.push_back(r);
                                batch_cols.push_back(c);
                            }
                        }

                        // Restore deferred entries for the next batch. They
                        // remain valid w1 candidates because no pivot in
                        // this batch touched their rows.
                        for (auto c : deferred) {
                            w1_work.push_back(c);
                        }

                        if (batch_cols.empty()) {
                            // Worklist exhausted without picking any pivot
                            // (every candidate became ineligible mid-loop).
                            break;
                        }

                        // Apply the batch sequentially; the disjoint-row
                        // invariant makes the application order immaterial.
                        // Cascading new w1 columns surface via
                        // apply_w1_pivot pushing them back onto w1_work.
                        for (auto c : batch_cols) {
                            if (apply_w1_pivot(c)) {
                                ++eliminated_this_pass;
                            }
                        }
                    }
                }
            }

            // Phase 2: Merge ALL weight-2 columns in one pass
            // For each w2 column c with rows {r1, r2}: merge r2 into r1, kill r2 and c.
            // Process greedily — if r2 was already killed by a prior merge, skip.
            if (config.eliminate_weight2) {
                if (effective_batch == 1) {
                    // Original sequential per-column scan, byte-identical to
                    // the historical implementation.
                    for (uint32_t c = 0; c < n_cols; ++c) {
                        if (apply_w2_pivot(c)) {
                            ++eliminated_this_pass;
                        }
                    }
                } else {
                    // Batched path: scan columns once per pass and gather
                    // pivots whose two-row support is disjoint from the rows
                    // already chosen in the current batch. When the batch is
                    // full, apply it (apply order is immaterial since the
                    // four rows of any two batched pivots are distinct). The
                    // next batch resumes scanning from where the previous one
                    // left off and a final fallback pass at the end of the
                    // pass picks up any deferred columns that became
                    // eligible after batched merges retired conflicting rows.
                    std::vector<uint32_t> batch_cols;
                    batch_cols.reserve(static_cast<size_t>(effective_batch));
                    std::vector<size_t> used_rows;
                    // up to 2 rows per pivot, so 2*N capacity is exact.
                    used_rows.reserve(
                        static_cast<size_t>(effective_batch) * 2);
                    std::vector<uint32_t> deferred;
                    deferred.reserve(n_cols / 16);

                    uint32_t c = 0;
                    while (c < n_cols) {
                        batch_cols.clear();
                        used_rows.clear();

                        // Gather up to `effective_batch` pivots with
                        // disjoint row supports. Columns that conflict with
                        // the current batch (one or both rows already used)
                        // are pushed onto `deferred` to retry after the
                        // batch retires; columns we cannot use even after
                        // the batch (still ineligible) are dropped by
                        // apply_w2_pivot returning false.
                        for (; c < n_cols && batch_cols.size() <
                                                  static_cast<size_t>(
                                                      effective_batch);
                             ++c) {
                            if (!col_alive[c]) continue;
                            if (col_to_rows[c].size() != 2) continue;
                            size_t r1 = col_to_rows[c][0];
                            size_t r2 = col_to_rows[c][1];
                            if (!row_alive[r1] || !row_alive[r2]) continue;

                            bool conflict = false;
                            for (size_t ur : used_rows) {
                                if (ur == r1 || ur == r2) {
                                    conflict = true;
                                    break;
                                }
                            }
                            if (conflict) {
                                deferred.push_back(c);
                                continue;
                            }
                            used_rows.push_back(r1);
                            used_rows.push_back(r2);
                            batch_cols.push_back(c);
                        }

                        if (batch_cols.empty()) {
                            // No pivots found in the rest of the scan. The
                            // outer while exits because c == n_cols now.
                            continue;
                        }

                        for (auto cb : batch_cols) {
                            if (apply_w2_pivot(cb)) {
                                ++eliminated_this_pass;
                            }
                        }
                    }

                    // Re-try deferred columns after every batch has been
                    // applied. Some deferrals freed up rows by killing one
                    // side of the conflict (the merge target r1 survives,
                    // but r2 is gone, so a column whose pair was (r2, r3) is
                    // no longer eligible — apply_w2_pivot detects that and
                    // returns false). Others may now be w1 columns thanks to
                    // a merge having vacated one of their rows — Phase 1 of
                    // the next pass will catch those.
                    for (auto cd : deferred) {
                        if (apply_w2_pivot(cd)) {
                            ++eliminated_this_pass;
                        }
                    }
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

        // BACKLOG #6: 若 composition cap 触发, stderr 警告 (一次, 非每 pass).
        // 不论 verbose, 这是 algorithm correctness signal (cap 限制了 merge throughput).
        if (result.weight2_skipped_cap > 0) {
            std::cerr << "[sge] row_composition_cap "
                      << config.row_composition_cap
                      << " triggered, skipped "
                      << result.weight2_skipped_cap
                      << " weight-2 merges (potential RAM safety vs reduced merge "
                      << "throughput; tune SGEConfig.row_composition_cap if needed)\n";
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
