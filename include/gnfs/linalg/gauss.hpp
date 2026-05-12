#pragma once

#include "sparse_matrix.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace gnfs::linalg {

/// 高斯消元结果
struct GaussianResult {
    size_t rank = 0;                      // 矩阵的秩
    std::vector<size_t> pivot_cols;       // 主元列
    std::vector<size_t> free_cols;        // 自由变量列
    std::vector<BitVector> null_space;    // 零空间基向量
};

/// 高斯消元配置
struct GaussianConfig {
    bool compute_null_space = true;   // 是否计算零空间
    size_t max_null_vectors = 64;     // 最大零空间向量数
    bool verbose = false;             // 详细输出
};

/// GaussianEliminator - GF(2) 上的高斯消元
/// 用于找到矩阵的零空间（关系依赖）
class GaussianEliminator {
public:
    using Config = GaussianConfig;

    explicit GaussianEliminator(const Config& config = Config{})
        : config_(config) {}

    /// 对矩阵进行高斯消元
    /// 注意：会修改输入矩阵！
    /// @param matrix 输入矩阵（会被修改为行阶梯形）
    /// @return 消元结果
    [[nodiscard]] GaussianResult eliminate(SparseMatrix& matrix) const {
        GaussianResult result;

        size_t num_rows = matrix.num_rows();
        size_t num_cols = matrix.num_cols();

        if (num_rows == 0 || num_cols == 0) {
            return result;
        }

        // 主消元循环
        // NOTE: history 矩阵已移除——build_null_space() 采用回代法
        // 直接从 reduced echelon form 推导零空间，不需要行变换记录
        size_t pivot_row = 0;
        std::vector<bool> is_pivot_col(num_cols, false);

        for (size_t col = 0; col < num_cols && pivot_row < num_rows; ++col) {
            // 找主元
            size_t pivot = find_pivot(matrix, col, pivot_row);

            if (pivot == SIZE_MAX) {
                // 这列没有主元，是自由变量
                result.free_cols.push_back(col);
                continue;
            }

            // 交换行
            if (pivot != pivot_row) {
                matrix.swap_rows(pivot, pivot_row);
            }

            // 记录主元列
            result.pivot_cols.push_back(col);
            is_pivot_col[col] = true;

            // 消去该列的其他非零元素
            for (size_t i = 0; i < num_rows; ++i) {
                if (i != pivot_row && matrix.test(i, col)) {
                    matrix.xor_rows(i, pivot_row);
                }
            }

            ++pivot_row;
        }

        // 剩余的列都是自由变量
        for (size_t col = result.pivot_cols.empty() ? 0 :
                result.pivot_cols.back() + 1; col < num_cols; ++col) {
            if (!is_pivot_col[col]) {
                result.free_cols.push_back(col);
            }
        }

        result.rank = result.pivot_cols.size();

        // 构建零空间基
        if (config_.compute_null_space) {
            build_null_space(result, matrix);
        }

        return result;
    }

    /// 仅计算秩（不需要零空间时更快）
    [[nodiscard]] size_t compute_rank(SparseMatrix& matrix) const {
        Config rank_config = config_;
        rank_config.compute_null_space = false;

        auto result = GaussianEliminator(rank_config).eliminate(matrix);
        return result.rank;
    }

private:
    Config config_;

    /// 在指定列中找主元（从 start_row 开始）
    [[nodiscard]] size_t find_pivot(const SparseMatrix& matrix,
                                     size_t col,
                                     size_t start_row) const {

        // 策略：选择该列非零且行重量最小的行作为主元
        // 这可以减少后续的填充
        size_t best_row = SIZE_MAX;
        size_t best_weight = SIZE_MAX;

        for (size_t i = start_row; i < matrix.num_rows(); ++i) {
            if (matrix.test(i, col)) {
                size_t weight = matrix.row(i).weight();
                if (weight < best_weight) {
                    best_weight = weight;
                    best_row = i;
                }
            }
        }

        return best_row;
    }

    /// 从消元后的矩阵构建零空间基
    /// 矩阵入口必须是 RREF(eliminate() 输出),pivot 行只在 pivot_col 处为 1
    /// (该列其他行都已被消)。对自由变量 fc_i 构造 null vector:
    ///   null[fc_i] = 1, 其他 fc 为 0
    ///   pivot_row[j] 方程: pivot_col[j] = M[j, fc_i] (其他 fc 贡献为 0)
    /// 所以 null[pivot_col[j]] = M[pivot_row=j, fc_i]。
    /// 复杂度 O(rank·num_null) — 比原版 O(rank·free²) 快一个量级。
    void build_null_space(GaussianResult& result,
                          const SparseMatrix& matrix) const {

        size_t num_cols = matrix.num_cols();
        size_t num_null = std::min(result.free_cols.size(), config_.max_null_vectors);

        for (size_t i = 0; i < num_null; ++i) {
            size_t free_col = result.free_cols[i];

            BitVector null_vec(num_cols);
            null_vec.set(free_col);

            // RREF 下直接读 M[pivot_row=j, free_col]
            for (size_t j = 0; j < result.pivot_cols.size(); ++j) {
                if (matrix.test(j, free_col)) {
                    null_vec.set(result.pivot_cols[j]);
                }
            }

            result.null_space.push_back(std::move(null_vec));
        }
    }
};

} // namespace gnfs::linalg
