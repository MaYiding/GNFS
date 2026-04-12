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

    /// 对矩阵的转置进行高斯消元（用于找行依赖）
    /// 等效于找 A^T 的零空间，即 A 的左零空间
    [[nodiscard]] GaussianResult eliminate_transpose(const SparseMatrix& matrix) const {
        // 创建转置矩阵
        SparseMatrix transposed = matrix.transpose();
        return eliminate(transposed);
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
    void build_null_space(GaussianResult& result,
                          const SparseMatrix& matrix) const {

        size_t num_cols = matrix.num_cols();

        // 对于每个自由变量列，构建一个零空间向量
        size_t num_null = std::min(result.free_cols.size(), config_.max_null_vectors);

        for (size_t i = 0; i < num_null; ++i) {
            size_t free_col = result.free_cols[i];

            // 构建零空间向量：设置自由变量为 1
            BitVector null_vec(num_cols);
            null_vec.set(free_col);

            // 从主元行反推其他变量的值
            // 从底部向上遍历主元
            for (size_t j = result.pivot_cols.size(); j > 0; --j) {
                size_t pivot_col = result.pivot_cols[j - 1];
                size_t pivot_row = j - 1;

                // 检查这个主元行在 free_col 列是否有非零
                // 如果有，需要设置 pivot_col 为 1 来消除
                bool need_set = false;

                // 检查 pivot_row 行中，所有已设置的自由变量列
                for (size_t k = i; k < result.free_cols.size(); ++k) {
                    size_t fc = result.free_cols[k];
                    if (k == i) {
                        // 当前自由变量
                        if (matrix.test(pivot_row, fc)) {
                            need_set = !need_set;
                        }
                    } else if (null_vec.test(fc)) {
                        // 已设置的自由变量
                        if (matrix.test(pivot_row, fc)) {
                            need_set = !need_set;
                        }
                    }
                }

                // 检查已设置的主元变量
                for (size_t k = j; k < result.pivot_cols.size(); ++k) {
                    size_t pc = result.pivot_cols[k];
                    if (null_vec.test(pc) && matrix.test(pivot_row, pc)) {
                        need_set = !need_set;
                    }
                }

                if (need_set) {
                    null_vec.set(pivot_col);
                }
            }

            result.null_space.push_back(std::move(null_vec));
        }
    }
};

/// 将零空间向量转换为关系索引集合
/// @param null_vec 零空间向量
/// @param row_to_relation 行到关系的映射
/// @return 参与依赖的关系索引
[[nodiscard]] inline std::vector<size_t> null_vector_to_relations(
        const BitVector& null_vec,
        const std::vector<size_t>& row_to_relation) {

    std::vector<size_t> relations;
    for (size_t i = 0; i < null_vec.size() && i < row_to_relation.size(); ++i) {
        if (null_vec.test(i)) {
            relations.push_back(row_to_relation[i]);
        }
    }
    return relations;
}

/// 验证零空间向量
/// @param matrix 原始矩阵
/// @param null_vec 零空间向量
/// @return true 如果 matrix * null_vec = 0
[[nodiscard]] inline bool verify_null_vector(
        const SparseMatrix& matrix,
        const BitVector& null_vec) {

    for (size_t i = 0; i < matrix.num_rows(); ++i) {
        bool sum = false;
        for (uint32_t col : matrix.row(i).indices()) {
            if (col < null_vec.size() && null_vec.test(col)) {
                sum = !sum;
            }
        }
        if (sum) {
            return false;  // 不是零
        }
    }
    return true;
}

/// 高斯消元统计
struct GaussianStats {
    size_t num_rows = 0;
    size_t num_cols = 0;
    size_t rank = 0;
    size_t nullity = 0;  // = num_cols - rank
    size_t num_null_vectors = 0;
};

[[nodiscard]] inline GaussianStats compute_gaussian_stats(
        const SparseMatrix& matrix,
        const GaussianResult& result) {

    GaussianStats stats;
    stats.num_rows = matrix.num_rows();
    stats.num_cols = matrix.num_cols();
    stats.rank = result.rank;
    stats.nullity = stats.num_cols - stats.rank;
    stats.num_null_vectors = result.null_space.size();
    return stats;
}

} // namespace gnfs::linalg
