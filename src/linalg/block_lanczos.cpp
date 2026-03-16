#include "gnfs/linalg/block_lanczos.hpp"
#include <algorithm>
#include <random>
#include <cstring>

namespace gnfs::linalg {

// ============================================================================
// 64-bit Word-Packed GF(2) Matrix for Fast Gaussian Elimination
// ============================================================================
// Key insight: For matrices up to ~10K x 10K, a dense word-packed representation
// is faster than sparse due to:
// 1. O(1) bit access vs O(log n) binary search
// 2. 64 bits XORed per instruction
// 3. Cache-friendly sequential access
// 4. No dynamic memory allocation during elimination

class PackedGF2Matrix {
public:
    size_t rows_;
    size_t cols_;
    size_t words_per_row_;  // ceil(cols / 64)
    std::vector<uint64_t> data_;  // Row-major, each row is words_per_row_ words

    PackedGF2Matrix(size_t rows, size_t cols)
        : rows_(rows), cols_(cols),
          words_per_row_((cols + 63) / 64),
          data_(rows * words_per_row_, 0) {}

    // Set bit at (row, col)
    void set(size_t row, size_t col) {
        size_t word_idx = row * words_per_row_ + col / 64;
        size_t bit_idx = col % 64;
        data_[word_idx] |= (1ULL << bit_idx);
    }

    // Test bit at (row, col)
    bool test(size_t row, size_t col) const {
        size_t word_idx = row * words_per_row_ + col / 64;
        size_t bit_idx = col % 64;
        return (data_[word_idx] >> bit_idx) & 1;
    }

    // XOR row src into row dst: dst ^= src
    void xor_rows(size_t dst, size_t src) {
        uint64_t* dst_ptr = &data_[dst * words_per_row_];
        const uint64_t* src_ptr = &data_[src * words_per_row_];
        for (size_t w = 0; w < words_per_row_; ++w) {
            dst_ptr[w] ^= src_ptr[w];
        }
    }

    // Swap two rows
    void swap_rows(size_t r1, size_t r2) {
        uint64_t* ptr1 = &data_[r1 * words_per_row_];
        uint64_t* ptr2 = &data_[r2 * words_per_row_];
        for (size_t w = 0; w < words_per_row_; ++w) {
            std::swap(ptr1[w], ptr2[w]);
        }
    }

    // Check if range [start_col, end_col) in row is all zeros
    bool is_zero_range(size_t row, size_t start_col, size_t end_col) const {
        size_t start_word = start_col / 64;
        size_t end_word = (end_col + 63) / 64;
        const uint64_t* row_ptr = &data_[row * words_per_row_];

        for (size_t w = start_word; w < end_word && w < words_per_row_; ++w) {
            uint64_t mask = ~0ULL;

            // Mask out bits before start_col in first word
            if (w == start_word && start_col % 64 != 0) {
                mask &= (~0ULL << (start_col % 64));
            }

            // Mask out bits after end_col in last word
            if (w == end_word - 1 && end_col % 64 != 0) {
                mask &= ((1ULL << (end_col % 64)) - 1);
            }

            if ((row_ptr[w] & mask) != 0) {
                return false;
            }
        }
        return true;
    }

    // Extract bits [0, num_bits) from row as vector<bool>
    std::vector<bool> extract_bits(size_t row, size_t num_bits) const {
        std::vector<bool> result(num_bits, false);
        const uint64_t* row_ptr = &data_[row * words_per_row_];

        for (size_t i = 0; i < num_bits; ++i) {
            if ((row_ptr[i / 64] >> (i % 64)) & 1) {
                result[i] = true;
            }
        }
        return result;
    }
};

// ============================================================================
// Optimized Gaussian Elimination using Word-Packed Matrix
// ============================================================================
std::vector<std::vector<bool>> BlockLanczos::find_dependencies_sparse(
    const SparseMatrix& matrix, size_t max_deps) {

    std::vector<std::vector<bool>> dependencies;

    size_t m = matrix.num_rows();    // number of relations
    size_t n = matrix.num_cols();    // number of primes

    if (m == 0 || n == 0) return dependencies;

    // Debug output removed for cleaner logs

    // Build word-packed augmented matrix [I_m | M]
    // Total columns = m + n
    PackedGF2Matrix aug(m, m + n);

    // Fill in the augmented matrix
    for (size_t row = 0; row < m; ++row) {
        // Identity part: single 1 at position 'row'
        aug.set(row, row);

        // M part: shifted by m
        for (uint32_t col : matrix.row(row).indices()) {
            if (col < n) {
                aug.set(row, m + col);
            }
        }
    }

    // Gaussian elimination on M columns (columns m to m+n-1)

    // Gaussian elimination on M columns (columns m to m+n-1)
    // We process in-place, swapping rows to bring pivots to the top
    size_t pivot_row = 0;

    for (size_t col = m; col < m + n && pivot_row < m; ++col) {
        // Find a pivot in rows [pivot_row, m) with 1 in this column
        size_t best_pivot = m;
        for (size_t row = pivot_row; row < m; ++row) {
            if (aug.test(row, col)) {
                best_pivot = row;
                break;
            }
        }

        if (best_pivot == m) {
            // No pivot found for this column (free variable)
            continue;
        }

        // Swap pivot row to current position
        if (best_pivot != pivot_row) {
            aug.swap_rows(pivot_row, best_pivot);
        }

        // Eliminate: XOR pivot row into all other rows with 1 in this column
        for (size_t row = 0; row < m; ++row) {
            if (row != pivot_row && aug.test(row, col)) {
                aug.xor_rows(row, pivot_row);
            }
        }

        ++pivot_row;
    }

    // Find rows where M part is all zeros (columns m to m+n are zero)
    // These rows represent dependencies in the identity part
    for (size_t row = 0; row < m && dependencies.size() < max_deps; ++row) {
        if (aug.is_zero_range(row, m, m + n)) {
            // Extract identity part (columns 0 to m-1) as dependency
            auto dep = aug.extract_bits(row, m);

            // Check non-trivial
            bool has_nonzero = false;
            for (bool b : dep) {
                if (b) { has_nonzero = true; break; }
            }

            if (has_nonzero) {
                dependencies.push_back(std::move(dep));
            }
        }
    }

    return dependencies;
}

// ============================================================================
// Original dense Gaussian (kept for small matrices or fallback)
// ============================================================================
std::vector<std::vector<bool>> BlockLanczos::find_dependencies_gaussian(
    const SparseMatrix& matrix, size_t max_deps) {

    std::vector<std::vector<bool>> dependencies;

    size_t m = matrix.num_rows();
    size_t n = matrix.num_cols();

    // For larger matrices, use sparse version
    if (m > 1000 || n > 1000) {
        return find_dependencies_sparse(matrix, max_deps);
    }

    // Dense version for small matrices
    std::vector<std::vector<bool>> aug(m, std::vector<bool>(m + n, false));

    for (size_t row = 0; row < m; ++row) {
        aug[row][row] = true;
        for (uint32_t col : matrix.row(row).indices()) {
            if (col < n) {
                aug[row][m + col] = true;
            }
        }
    }

    size_t pivot_row = 0;
    for (size_t col = m; col < m + n && pivot_row < m; ++col) {
        size_t best_pivot = m;
        for (size_t row = pivot_row; row < m; ++row) {
            if (aug[row][col]) {
                best_pivot = row;
                break;
            }
        }

        if (best_pivot == m) continue;

        if (best_pivot != pivot_row) {
            std::swap(aug[pivot_row], aug[best_pivot]);
        }

        for (size_t row = 0; row < m; ++row) {
            if (row != pivot_row && aug[row][col]) {
                for (size_t j = 0; j < aug[row].size(); ++j) {
                    aug[row][j] = aug[row][j] != aug[pivot_row][j];
                }
            }
        }

        ++pivot_row;
    }

    for (size_t row = 0; row < m && dependencies.size() < max_deps; ++row) {
        bool right_all_zero = true;
        for (size_t col = m; col < m + n; ++col) {
            if (aug[row][col]) {
                right_all_zero = false;
                break;
            }
        }

        if (right_all_zero) {
            std::vector<bool> dep(m, false);
            bool has_nonzero = false;
            for (size_t i = 0; i < m; ++i) {
                dep[i] = aug[row][i];
                if (dep[i]) has_nonzero = true;
            }

            if (has_nonzero) {
                dependencies.push_back(dep);
            }
        }
    }

    return dependencies;
}

// ============================================================================
// Main entry point — dispatches to Gaussian or Block Lanczos
// ============================================================================
std::vector<std::vector<bool>> BlockLanczos::find_dependencies(
    const SparseMatrix& matrix, size_t max_deps) {

    if (matrix.num_rows() == 0 || matrix.num_cols() == 0) {
        return {};
    }

    // For small matrices, Gaussian elimination is faster and more reliable
    if (matrix.num_rows() < 10000 && matrix.num_cols() < 10000) {
        return find_dependencies_sparse(matrix, max_deps);
    }

    // For large matrices, use true Block Lanczos
    return block_lanczos_solve(matrix, max_deps);
}

// ============================================================================
// True Block Lanczos over GF(2) — Montgomery 1995
// ============================================================================
// Finds left null-space of M (m×n): vectors v with v^T M = 0
// Works with B = M M^T (m×m, symmetric) computed implicitly via SpMV
// Complexity: O(m * w / 32) where w = total matrix weight
// ============================================================================
std::vector<std::vector<bool>> BlockLanczos::block_lanczos_solve(
    const SparseMatrix& matrix, size_t max_deps) {

    const size_t m = matrix.num_rows();
    const size_t n = matrix.num_cols();
    const size_t max_iter = m / 64 + 100;

    // Random starting block vector Y
    BlockVector Y(m);
    {
        std::mt19937_64 rng(42);
        for (size_t i = 0; i < m; ++i)
            Y.data[i] = rng();
    }

    // Accumulator for solution
    BlockVector S(m);

    // Lanczos vectors: current, previous, previous-previous
    BlockVector V_cur(m), V_prev(m), V_pprev(m);

    // Intermediate n-length block vector for SpMV
    BlockVector temp_n(n);

    // B * Y = M * (M^T * Y)
    spmv_transpose(matrix, Y, temp_n);
    spmv_forward(matrix, temp_n, V_cur);

    // 64x64 matrices for recurrence
    DenseGF2_64x64 D_prev, D_pprev;
    uint64_t mask_prev = 0, mask_pprev = 0;

    for (size_t iter = 0; iter < max_iter; ++iter) {
        // Step 1: Inner product A_i = V_cur^T * V_cur
        auto A_cur = inner_product_64x64(V_cur, V_cur);

        // Step 2: Termination check
        if (V_cur.is_zero()) break;

        // Step 3: Partial inverse of A_i
        auto [D_cur, mask_cur] = A_cur.partial_inverse();

        // Step 4: Compute B * V_cur = M * (M^T * V_cur)
        BlockVector BV_cur(m);
        spmv_transpose(matrix, V_cur, temp_n);
        spmv_forward(matrix, temp_n, BV_cur);

        // Step 5: Recurrence coefficients
        auto E_cur = inner_product_64x64(V_prev, BV_cur);
        auto F_cur = inner_product_64x64(V_pprev, BV_cur);

        // Step 6: Compute V_next = BV_cur + V_cur*(D*A+I) + V_prev*(D_prev*E) + V_pprev*(D_pprev*F)
        BlockVector V_next(m);
        // Start with BV_cur
        for (size_t i = 0; i < m; ++i)
            V_next.data[i] = BV_cur.data[i];

        // + V_cur * (D_cur * A_cur + I)
        {
            auto DA = D_cur.multiply(A_cur);
            DA.add_identity();
            V_next.xor_with_mul(V_cur, DA.rows);
        }

        // + V_prev * (D_prev * E_cur)
        if (iter >= 1) {
            auto DE = D_prev.multiply(E_cur);
            V_next.xor_with_mul(V_prev, DE.rows);
        }

        // + V_pprev * (D_pprev * F_cur)
        if (iter >= 2) {
            auto DF = D_pprev.multiply(F_cur);
            V_next.xor_with_mul(V_pprev, DF.rows);
        }

        // Step 7: Accumulate solution S += V_cur * D_cur * (V_cur^T * Y)
        {
            auto VtY = inner_product_64x64(V_cur, Y);
            auto DVtY = D_cur.multiply(VtY);
            S.xor_with_mul(V_cur, DVtY.rows);
        }

        // Step 8: Shift
        V_pprev = std::move(V_prev);
        V_prev = std::move(V_cur);
        V_cur = std::move(V_next);
        D_pprev = D_prev;
        D_prev = D_cur;
        mask_pprev = mask_prev;
        mask_prev = mask_cur;
    }

    // Step 9: Extract and verify dependencies
    std::vector<std::vector<bool>> dependencies;

    for (int j = 0; j < 64 && dependencies.size() < max_deps; ++j) {
        auto candidate = S.extract_column(j);

        // Check non-zero
        bool nonzero = false;
        for (bool b : candidate) { if (b) { nonzero = true; break; } }
        if (!nonzero) continue;

        // Verify: M^T * candidate = 0
        // Compute M^T * candidate using sparse matrix
        std::vector<bool> check(n, false);
        for (size_t i = 0; i < m; ++i) {
            if (!candidate[i]) continue;
            for (uint32_t col : matrix.row(i).indices()) {
                if (col < n) check[col] = !check[col];
            }
        }
        bool valid = true;
        for (bool b : check) { if (b) { valid = false; break; } }

        if (valid) {
            dependencies.push_back(std::move(candidate));
        }
    }

    // If Block Lanczos found fewer than needed, try Gaussian on remaining
    if (dependencies.empty()) {
        return find_dependencies_sparse(matrix, max_deps);
    }

    return dependencies;
}

} // namespace gnfs::linalg
