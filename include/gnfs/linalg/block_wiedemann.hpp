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
    ///
    /// **Known limitation (BACKLOG #80, 2026-05-17)**: For thin matrices (m<n,
    /// rows < cols), BW may return 0 dependencies even when left null space
    /// exists. Root cause: over GF(2), null(M·M^T) ⊋ null(M^T) when
    /// rank(M·M^T) < rank(M) (which can happen due to GF(2) bilinear form
    /// quirk: v^T M M^T v = ‖M^T v‖² mod 2 = parity of M^T v, which can be 0
    /// without M^T v = 0). Standard BW verification rejects all candidates.
    ///
    /// Attempted fix: zero-row padding to square (n×n) — does NOT work because
    /// padded matrix has same rank as original; the issue is rank deficiency
    /// over GF(2), not shape.
    ///
    /// Proper fix would require either (a) BW with B=M^T·M (n×n) and recover
    /// dep as u = M·v where v ∈ null(M^T·M), since M^T·u = M^T·M·v = 0 places
    /// u in null(M^T); or (b) iterative refinement on M^T·w residuals.
    /// Algorithmic work — not done in this session. GNFS_THIN_MATRIX_TRY=1
    /// ENV in pipeline allows this code path but solution may be empty.
    std::vector<std::vector<bool>> find_dependencies(
        const SparseMatrix& matrix, size_t max_deps = 64);

    /// Coppersmith's Block Berlekamp-Massey algorithm (public for unit testing).
    /// Input: sequence of L matrices A_0, A_1, ..., A_{L-1} (each 64×64 over GF(2))
    /// Output: minimal generating matrix polynomial F such that
    ///   for sufficient t:  (A·F)_t = 0
    /// where the convolution (A·F)_t = sum_k A_{t-k} · F_k.
    ///
    /// Algorithm: column-extended Coppersmith quadratic basecase (CADO-NFS
    /// lingen_qcode_binary.cpp). Maintains 64×128 extended polynomial matrix
    /// E (input: A | I@z=0) and 128×128 polynomial matrix P (output), with
    /// per-column delta. Each step e processes m=64 rows: for each row find
    /// the smallest-delta column j_p with E[i,j_p][e]=1, XOR pivot into other
    /// columns where E[i,k][e]=1, then shift pivot col up by 1 ("consume"
    /// bit e). After L steps, the n=64 cols of P with smallest delta give F.
    ///
    /// Complexity: O(L · m · b² · W) bit-ops where W = ⌈(L+10)/64⌉ words/poly.
    /// For L = 2n/b (matrix dim n, block size b=64): O(n³/b · W) ≈ O(n³/64²).
    /// Suitable for n ≤ ~300K. For larger matrices, Thomé's subquadratic
    /// lingen would be needed (P2 follow-up).
    static LingenResult matrix_berlekamp_massey(
        const std::vector<DenseGF2_64x64>& sequence, size_t N);

private:
    /// Scalar-BM fallback path (legacy, O(n) SpMV count).
    /// Used when env GNFS_BW_ALGORITHM=scalar, or as automatic fallback if
    /// block_solve returns empty.
    std::vector<std::vector<bool>> block_wiedemann_scalar_solve(
        const SparseMatrix& matrix, size_t max_deps, uint64_t seed = 42);

    /// True block Wiedemann with Coppersmith matrix BM (O(n/64) SpMV count).
    /// Phase 1 collects matrix sequence A_k = X^T · V_k (L = 2⌈n/64⌉+32 iters);
    /// Phase 2 runs matrix_berlekamp_massey; Phase 3 recomputes Krylov and
    /// accumulates W = sum_k V_k · F_k (block-mksol).
    std::vector<std::vector<bool>> block_wiedemann_block_solve(
        const SparseMatrix& matrix, size_t max_deps, uint64_t seed = 42);

    /// Thin matrix BW variant (BACKLOG #80 step 7): operates on
    /// B'=M^T·M (n×n) instead of B=M·M^T (m×m). Works in R^n. Krylov,
    /// BM, mksol same structure but vectors length n. Recovery: u = M·w
    /// (1 SpMV) gives left null space vector u∈R^m since M^T·u =
    /// (M^T·M)·w = 0 strict over GF(2). Used when m ≤ n (thin matrix).
    /// L = 2·⌈m/64⌉ + 32 (rank ≤ m).
    std::vector<std::vector<bool>> block_wiedemann_thin_solve(
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
