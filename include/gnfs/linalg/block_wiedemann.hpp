#pragma once

#include "gnfs/linalg/block_lanczos.hpp"  // BlockVector, DenseGF2_64x64, CSRMatrix, etc.
#include "gnfs/linalg/sparse_matrix.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace gnfs::linalg {

// ============================================================================
// 64×128 GF(2) Matrix — Coppersmith block-BM extended state
// ============================================================================
//
// Storage layout: column-major, cols[j] packs 64 bits where bit r is the
// element at (row r, col j). All BM hot operations (xor_cols, swap_cols,
// get/set col) are O(1).
//
// Logical role: F^{(t)}(z) coefficient F_k ∈ GF(2)^{64×128} for Coppersmith's
// column-extended matrix Berlekamp-Massey. Columns 0..63 = "active" (left
// half), columns 64..127 = "auxiliary" (right half).
//
// Tests in tests/test_block_wiedemann.cpp.
struct DenseGF2_64x128 {
    uint64_t cols[128] = {};

    void clear() noexcept { std::memset(cols, 0, sizeof(cols)); }

    // Initialize as [I_64 | 0_64]: left half identity, right half zero.
    void set_left_identity() noexcept {
        clear();
        for (int i = 0; i < 64; ++i) cols[i] = 1ULL << i;
    }

    [[nodiscard]] uint64_t get_col(int j) const noexcept {
        assert(j >= 0 && j < 128);
        return cols[j];
    }
    void set_col(int j, uint64_t v) noexcept {
        assert(j >= 0 && j < 128);
        cols[j] = v;
    }

    // dst column ^= src column (in-place column XOR).
    void xor_cols(int dst, int src) noexcept {
        assert(dst >= 0 && dst < 128 && src >= 0 && src < 128 && dst != src);
        cols[dst] ^= cols[src];
    }

    void swap_cols(int a, int b) noexcept {
        assert(a >= 0 && a < 128 && b >= 0 && b < 128);
        if (a == b) return;
        std::swap(cols[a], cols[b]);
    }

    // Full matrix XOR (used to combine polynomial coefficients).
    void xor_with(const DenseGF2_64x128& other) noexcept {
        for (int j = 0; j < 128; ++j) cols[j] ^= other.cols[j];
    }

    [[nodiscard]] bool is_zero() const noexcept {
        for (int j = 0; j < 128; ++j)
            if (cols[j] != 0) return false;
        return true;
    }

    // Extract left half (cols 0..63) as 64×64 row-packed DenseGF2_64x64.
    // Convention: m.rows[i] bit j = entry (row i, col j) of left half.
    [[nodiscard]] DenseGF2_64x64 extract_left() const noexcept {
        DenseGF2_64x64 m;
        for (int i = 0; i < 64; ++i) {
            uint64_t r = 0;
            for (int j = 0; j < 64; ++j) {
                if ((cols[j] >> i) & 1ULL) r |= (1ULL << j);
            }
            m.rows[i] = r;
        }
        return m;
    }

    // Extract right half (cols 64..127) as 64×64.
    [[nodiscard]] DenseGF2_64x64 extract_right() const noexcept {
        DenseGF2_64x64 m;
        for (int i = 0; i < 64; ++i) {
            uint64_t r = 0;
            for (int j = 0; j < 64; ++j) {
                if ((cols[64 + j] >> i) & 1ULL) r |= (1ULL << j);
            }
            m.rows[i] = r;
        }
        return m;
    }
};

// Matrix polynomial: F(z) = F[0] + F[1]·z + ... + F[D]·z^D.
// Used both for the BM extended state (with 64×128 coefficients via parallel
// std::vector<DenseGF2_64x128>) and for the final LingenResult output (with
// 64×64 coefficients, after extracting the "good" half).
using MatrixPoly = std::vector<DenseGF2_64x64>;

// Output of Coppersmith block BM: 64-column generator polynomial.
// `degrees[j]` is the degree of column j in `poly` (entries of poly[k] columns
// > degrees[j] are 0 by construction). `valid_mask` bit j = 1 iff column j
// produced a non-trivial generator (used by Phase 3 mksol).
struct LingenResult {
    MatrixPoly poly;
    std::array<int, 64> degrees{};
    uint64_t valid_mask = 0;
};

// ============================================================================
// Phase 3 (mksol) primitive: accumulator += V_k · F_k
// ============================================================================
//
// V_k is a BlockVector of length m (packs 64 column-vectors as bits).
// F_k is a 64×64 GF(2) matrix (the k-th coefficient of the generator poly).
// `accumulator` is a BlockVector of length m being summed over k.
//
// (V·F)[r, j] = XOR_i V[r, i] · F[i, j], computed by iterating set bits of
// V.data[r] and XOR-ing F.rows[bit] into accumulator.data[r].
//
// This mirrors DenseGF2_64x64::multiply but with the left operand a BlockVector
// of arbitrary length (m), not a 64×64 matrix.
inline void mksol_accumulate(const BlockVector& V_k,
                              const DenseGF2_64x64& F_k,
                              BlockVector& accumulator) noexcept {
    assert(V_k.length == accumulator.length);
    const size_t m = V_k.length;
    for (size_t r = 0; r < m; ++r) {
        uint64_t v = V_k.data[r];
        uint64_t acc = 0;
        while (v) {
            int i = __builtin_ctzll(v);
            acc ^= F_k.rows[i];
            v &= v - 1;
        }
        accumulator.data[r] ^= acc;
    }
}

/// Block Wiedemann algorithm for finding GF(2) null space vectors
/// (Coppersmith 1994, with Berlekamp-Massey for matrix sequences)
///
/// Given a sparse GF(2) matrix M (m rows × n cols), finds vectors v
/// such that v^T · M = 0 (left null space).
///
/// Three-phase algorithm:
///   BW1 (Krylov):  Compute sequence A_i = X^T · B^i · Y where B = M·M^T
///   BW2 (lingen):  Find minimal matrix polynomial F(t) annihilating {A_i}
///   BW3 (mksol):   Extract null space vectors from F
///
/// Block size = 64, matching machine word for GF(2) word packing.
/// Uses the same CSRMatrix/BlockVector/SpMV infrastructure as Block Lanczos.
class BlockWiedemann {
public:
    BlockWiedemann() = default;

    /// Find dependencies (left null space vectors) in the matrix.
    /// Same API as BlockLanczos::find_dependencies for drop-in replacement.
    /// Returns vectors v such that v^T * M = 0 over GF(2).
    ///
    /// For small matrices (<5000), delegates to Gaussian elimination.
    /// For large matrices, runs the three-phase BW algorithm.
    std::vector<std::vector<bool>> find_dependencies(
        const SparseMatrix& matrix, size_t max_deps = 64);

    /// Coppersmith's Block Berlekamp-Massey algorithm (public for unit testing).
    /// Input: sequence of L matrices A_0, A_1, ..., A_{L-1} (each 64×64 over GF(2))
    /// Output: minimal generating matrix polynomial F such that
    ///   for sufficient t:  A(z)·F(z) has zero coefficient at z^t
    /// where A(z) = sum_k A_k·z^k (the input sequence as formal polynomial).
    ///
    /// Algorithm: column-extended Coppersmith — maintains 128-column polynomial
    /// state Pi(z) starting from [I_64 | 0_64], tracks per-column delay δ_j,
    /// at each step computes discrepancy and runs row-pivot Gaussian on Δ in
    /// δ-ascending column order (smallest-δ columns get pivot priority and are
    /// z-multiplied; larger-δ columns are corrected by XOR).
    ///
    /// Complexity: O(64^2 · L^2) bit-ops. Suitable for L ≤ ~10K (b=64 block size,
    /// L = 2n/64+O(1) for matrix dim n, so n ≤ ~300K). For larger matrices,
    /// Thomé's subquadratic lingen would be needed (P2 follow-up).
    static LingenResult matrix_berlekamp_massey(
        const std::vector<DenseGF2_64x64>& sequence, size_t N);

private:
    /// Three-phase Block Wiedemann for large sparse matrices.
    /// Works on the implicit symmetric matrix B = M · M^T.
    /// seed parameterizes the X / Y random vectors so the caller can retry
    /// with different seeds when one happens to produce trivial sequences.
    std::vector<std::vector<bool>> block_wiedemann_solve(
        const SparseMatrix& matrix, size_t max_deps, uint64_t seed = 42);

    // --- BW Phase 1: Krylov sequence generation ---

    /// Compute the Krylov sequence A_i = X^T · (M·M^T)^i · Y
    /// for i = 0, 1, ..., L-1 where L ≈ N/32.
    ///
    /// Uses forward SpMV only (no transpose needed for the sequence,
    /// but we compute M·M^T·v as M·(M^T·v) using both SpMV and SpMV^T).
    ///
    /// Returns the sequence of 64×64 matrices.
    static std::vector<DenseGF2_64x64> compute_krylov_sequence(
        const CSRMatrix& csr, size_t N, size_t L,
        const BlockVector& X, BlockVector& Y);

    // --- BW Phase 2: Matrix Berlekamp-Massey (lingen) ---
    // (matrix_berlekamp_massey is public, declared above. Types MatrixPoly /
    // LingenResult are at namespace level so tests can construct them directly.)

    // --- BW Phase 3: Solution extraction (mksol) ---

    /// Evaluate the generating polynomial on the Krylov vectors to extract
    /// null space vectors: w_j = sum_k F_k[*,j] · B^k · Y
    ///
    /// For each valid column j of F, computes the candidate null vector.
    /// Then verifies M^T · w_j = 0 and returns valid dependencies.
    static std::vector<std::vector<bool>> extract_solutions(
        const CSRMatrix& csr, size_t N,
        const LingenResult& lingen,
        const BlockVector& Y_initial,
        size_t max_deps);
};

} // namespace gnfs::linalg
