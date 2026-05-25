#pragma once

#include "gnfs/linalg/matrix_builder.hpp"
#include "gnfs/util/bit_intrin.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

namespace gnfs::linalg {

/// Block Lanczos algorithm for finding GF(2) null space
/// Implements Montgomery's Block Lanczos (1995) for large sparse matrices,
/// with Gaussian elimination fallback for small matrices.
class BlockLanczos {
public:
    BlockLanczos() = default;

    /// Find dependencies (left null space vectors) in the matrix
    /// Returns vectors v such that v^T * M = 0 over GF(2)
    std::vector<std::vector<bool>> find_dependencies(const SparseMatrix& matrix, size_t max_deps = 64);

private:
    // Gaussian elimination with packed GF(2) matrix for small matrices.
    // (Montgomery Block Lanczos was removed — had a 50% per-dep error rate and
    // was never reachable from find_dependencies. See git history.)
    std::vector<std::vector<bool>> find_dependencies_sparse(const SparseMatrix& matrix, size_t max_deps);
};

// ============================================================================
// Block Vector: m entries, each uint64_t (packs 64 GF(2) vectors)
// ============================================================================
struct BlockVector {
    std::vector<uint64_t> data;
    size_t length = 0;

    BlockVector() = default;
    explicit BlockVector(size_t n) : data(n, 0), length(n) {}

    void clear() { std::fill(data.begin(), data.end(), 0); }

    void xor_with(const BlockVector& other) {
        assert(other.length >= length);
        for (size_t i = 0; i < length; ++i)
            data[i] ^= other.data[i];
    }

    [[nodiscard]] bool is_zero() const {
        for (size_t i = 0; i < length; ++i)
            if (data[i] != 0) return false;
        return true;
    }

    [[nodiscard]] std::vector<bool> extract_column(size_t j) const {
        std::vector<bool> col(length, false);
        uint64_t mask = 1ULL << j;
        for (size_t i = 0; i < length; ++i)
            col[i] = (data[i] & mask) != 0;
        return col;
    }
};

// ============================================================================
// 64x64 GF(2) Matrix
// ============================================================================
struct DenseGF2_64x64 {
    uint64_t rows[64] = {};

    void clear() { std::memset(rows, 0, sizeof(rows)); }

    void set_identity() {
        clear();
        for (int i = 0; i < 64; ++i)
            rows[i] = 1ULL << i;
    }

    [[nodiscard]] DenseGF2_64x64 multiply(const DenseGF2_64x64& B) const {
        DenseGF2_64x64 C;
        for (int i = 0; i < 64; ++i) {
            uint64_t a = rows[i];
            uint64_t acc = 0;
            while (a) {
                int j = gnfs::util::ctz64(a);
                acc ^= B.rows[j];
                a &= a - 1;
            }
            C.rows[i] = acc;
        }
        return C;
    }

    void xor_with(const DenseGF2_64x64& other) {
        for (int i = 0; i < 64; ++i)
            rows[i] ^= other.rows[i];
    }

    void add_identity() {
        for (int i = 0; i < 64; ++i)
            rows[i] ^= 1ULL << i;
    }

    /// Partial inverse: find D such that D*A*D = D on the invertible subspace
    /// Returns (D, mask) where mask indicates which columns are invertible
    [[nodiscard]] std::pair<DenseGF2_64x64, uint64_t> partial_inverse() const {
        // Augmented matrix [A | I]
        uint64_t left[64], right[64];
        for (int i = 0; i < 64; ++i) {
            left[i] = rows[i];
            right[i] = 1ULL << i;
        }

        uint64_t mask = 0;
        for (int col = 0; col < 64; ++col) {
            uint64_t col_bit = 1ULL << col;
            // Find pivot
            int pivot = -1;
            for (int row = col; row < 64; ++row) {
                if (left[row] & col_bit) { pivot = row; break; }
            }
            if (pivot < 0) continue;

            // Swap
            if (pivot != col) {
                std::swap(left[col], left[pivot]);
                std::swap(right[col], right[pivot]);
            }
            mask |= col_bit;

            // Eliminate
            for (int row = 0; row < 64; ++row) {
                if (row != col && (left[row] & col_bit)) {
                    left[row] ^= left[col];
                    right[row] ^= right[col];
                }
            }
        }

        // Zero out non-pivot rows to avoid garbage in the non-invertible subspace
        for (int i = 0; i < 64; ++i) {
            if (!(mask & (1ULL << i))) {
                right[i] = 0;
            }
        }

        DenseGF2_64x64 D;
        for (int i = 0; i < 64; ++i)
            D.rows[i] = right[i];
        return {D, mask};
    }
};

/// Compute inner product C = A^T * B where A, B are block vectors
/// C is a 64x64 GF(2) matrix: C[j] = XOR of B[i] for all i where bit j of A[i] is set
inline DenseGF2_64x64 inner_product_64x64(const BlockVector& A, const BlockVector& B) {
    DenseGF2_64x64 C;
    for (size_t i = 0; i < A.length; ++i) {
        uint64_t ai = A.data[i];
        uint64_t bi = B.data[i];
        while (ai) {
            int j = gnfs::util::ctz64(ai);
            C.rows[j] ^= bi;
            ai &= ai - 1;
        }
    }
    return C;
}

} // namespace gnfs::linalg
