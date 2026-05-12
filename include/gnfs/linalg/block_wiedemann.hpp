#pragma once

#include "gnfs/linalg/block_lanczos.hpp"  // BlockVector, DenseGF2_64x64, CSRMatrix, etc.
#include "gnfs/linalg/sparse_matrix.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace gnfs::linalg {

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

    /// Matrix polynomial representation: F(t) = F[0] + F[1]·t + ... + F[D]·t^D
    /// Each F[k] is a 64×64 GF(2) matrix.
    using MatrixPoly = std::vector<DenseGF2_64x64>;

    /// Column-wise generating polynomial for the Krylov sequence.
    /// Each column j of the result polynomial is the minimal generator
    /// for column j of the sequence.
    struct LingenResult {
        /// Polynomial coefficients (each is 64×64 matrix)
        MatrixPoly poly;
        /// Degree of each of the 64 columns
        std::array<int, 64> degrees;
        /// Which columns are valid generators (non-zero)
        uint64_t valid_mask;
    };

    /// Coppersmith's Block Berlekamp-Massey algorithm.
    /// Input: sequence of L matrices A_0, A_1, ..., A_{L-1} (each 64×64 over GF(2))
    /// Output: minimal generating matrix polynomial F
    ///
    /// The algorithm maintains a 64×128 "extended" system and tracks per-column
    /// degrees, performing Gaussian pivoting at each step to minimize degree growth.
    ///
    /// Complexity: O(64^2 · L^2) — quadratic in sequence length, suitable for
    /// matrices up to ~10M. For larger matrices, Thomé's subquadratic variant
    /// would be needed.
    static LingenResult matrix_berlekamp_massey(
        const std::vector<DenseGF2_64x64>& sequence, size_t N);

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
