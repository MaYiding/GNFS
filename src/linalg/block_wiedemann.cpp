#include "gnfs/linalg/block_wiedemann.hpp"
#include "gnfs/util/thread_pool.hpp"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <random>

namespace gnfs::linalg {

// ============================================================================
// SpMV utilities
// ============================================================================

namespace {

void bw_spmv_forward(const CSRMatrix& M, const BlockVector& x, BlockVector& y,
                     gnfs::util::ThreadPool& pool) {
    pool.parallel_for_index(0, M.num_rows(), [&](size_t i) {
        uint64_t acc = 0;
        for (const uint32_t* p = M.row_begin(i); p != M.row_end(i); ++p)
            if (*p < x.length) acc ^= x.data[*p];
        y.data[i] = acc;
    });
}

void bw_spmv_transpose(const CSRMatrix& M, const BlockVector& x, BlockVector& y,
                       gnfs::util::ThreadPool& pool) {
    const size_t m = M.num_rows();
    const size_t n = y.length;
    const size_t T = pool.num_threads();
    const size_t chunk = (m + T - 1) / T;

    std::vector<std::vector<uint64_t>> locals(T, std::vector<uint64_t>(n, 0));
    std::vector<std::future<void>> futures;
    size_t T_used = 0;

    for (size_t t = 0; t < T; ++t) {
        size_t start = t * chunk;
        size_t end_row = std::min(start + chunk, m);
        if (start >= m) break;
        T_used = t + 1;
        futures.push_back(pool.submit([&M, &x, &locals, t, start, end_row, n]() {
            auto& local = locals[t];
            for (size_t i = start; i < end_row; ++i) {
                uint64_t xi = x.data[i];
                if (xi == 0) continue;
                for (const uint32_t* p = M.row_begin(i); p != M.row_end(i); ++p)
                    if (*p < n) local[*p] ^= xi;
            }
        }));
    }
    for (auto& f : futures) f.get();

    pool.parallel_for_index(0, n, [&y, &locals, T_used](size_t j) {
        uint64_t val = 0;
        for (size_t t = 0; t < T_used; ++t) val ^= locals[t][j];
        y.data[j] = val;
    });
}

void bw_spmv_symmetric(const CSRMatrix& M, const BlockVector& x, BlockVector& y,
                       BlockVector& tmp, gnfs::util::ThreadPool& pool) {
    bw_spmv_transpose(M, x, tmp, pool);
    bw_spmv_forward(M, tmp, y, pool);
}

// ============================================================================
// Krylov Null Space Finder
//
// Instead of Berlekamp-Massey, we use a direct approach:
// 1. Compute Krylov vectors V_k = B^k · Y for B = M·M^T
// 2. Compute U_k = M^T · V_k for each k (n-dimensional block vectors)
// 3. Find null space of [U_0 | U_1 | ... | U_{L-1}] via Gaussian elimination
// 4. The null vectors give coefficients c_k such that sum_k V_k · c_k ∈ null(M^T)
//
// This is mathematically equivalent to finding null(M^T) ∩ Krylov(B, Y),
// which the BW Krylov phase naturally generates.
//
// Complexity: O(L × n × nnz) for SpMV + O(n × (64L)²) for Gaussian.
// For small n (columns << rows), Gaussian is cheap.
// ============================================================================

/// GF(2) packed row for Gaussian elimination
/// Each row stores (64L + 64L) bits: the equation part and the identity part
/// (for tracking which linear combination gives each null vector)
class PackedBitRow {
public:
    PackedBitRow() = default;
    explicit PackedBitRow(size_t total_bits)
        : words_((total_bits + 63) / 64, 0) {}

    void set(size_t bit) { words_[bit / 64] |= 1ULL << (bit % 64); }
    [[nodiscard]] bool test(size_t bit) const {
        return (words_[bit / 64] >> (bit % 64)) & 1;
    }
    void xor_with(const PackedBitRow& other) {
        assert(other.words_.size() >= words_.size());
        for (size_t i = 0; i < words_.size(); ++i)
            words_[i] ^= other.words_[i];
    }
    [[nodiscard]] bool is_zero(size_t from_bit, size_t to_bit) const {
        size_t w0 = from_bit / 64, w1 = (to_bit + 63) / 64;
        for (size_t w = w0; w < w1 && w < words_.size(); ++w) {
            uint64_t mask = ~0ULL;
            if (w == w0 && (from_bit % 64) != 0)
                mask &= ~((1ULL << (from_bit % 64)) - 1);
            if (w + 1 == w1 && (to_bit % 64) != 0)
                mask &= (1ULL << (to_bit % 64)) - 1;
            if (words_[w] & mask) return false;
        }
        return true;
    }
    std::vector<uint64_t> words_;
};

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

std::vector<std::vector<bool>> BlockWiedemann::find_dependencies(
    const SparseMatrix& matrix, size_t max_deps) {

    const size_t m = matrix.num_rows();
    const size_t n = matrix.num_cols();
    if (m == 0 || n == 0) return {};

    // For small matrices, delegate to Gaussian (same threshold as BL)
    if (m < 5000 && n < 5000) {
        BlockLanczos bl;
        return bl.find_dependencies(matrix, max_deps);
    }

    return block_wiedemann_solve(matrix, max_deps);
}

// ============================================================================
// Block Wiedemann: Krylov + Null Space via Gaussian
//
// Phase 1 (Krylov): Compute V_k = B^k·Y and U_k = M^T·V_k
// Phase 2 (Gaussian): Find null space of [U_0 | U_1 | ... | U_{L-1}]
// Phase 3 (Extract): Accumulate w = sum_k V_k · c_k and verify
// ============================================================================

std::vector<std::vector<bool>> BlockWiedemann::block_wiedemann_solve(
    const SparseMatrix& matrix, size_t max_deps) {

    const size_t m = matrix.num_rows();
    const size_t n = matrix.num_cols();
    const size_t N = m;

    std::cout << "  [BW] Block Wiedemann: " << m << "×" << n
              << " matrix, N=" << N << std::endl;

    CSRMatrix csr(matrix);

    // Number of Krylov steps: need 64·L > n for the Gaussian to have a null space
    // L = ceil(n / 64) + safety
    const size_t L = (n + 63) / 64 + 10;
    const size_t total_krylov_cols = 64 * L;

    std::cout << "  [BW] Krylov steps L=" << L
              << " (" << total_krylov_cols << " columns)" << std::endl;

    // Random initialization
    std::mt19937_64 rng(42);
    BlockVector Y(N);
    for (size_t i = 0; i < N; ++i) Y.data[i] = rng();

    gnfs::util::ThreadPool pool(0);

    // === Phase 1: Krylov + Project ===
    // Compute U_k = M^T · V_k where V_k = B^k · Y
    // U_k is an n-dimensional block vector (n × 64)
    // Store all U_k for Gaussian, and all V_k for extraction

    std::cout << "  [BW] Phase 1: Krylov + projection..." << std::flush;

    std::vector<BlockVector> U_all;  // each is n-length, 64 bits
    std::vector<BlockVector> V_all;  // each is N-length, 64 bits
    U_all.reserve(L);
    V_all.reserve(L);

    BlockVector V(N), Vnext(N), tmp_t(n);
    for (size_t i = 0; i < N; ++i) V.data[i] = Y.data[i];

    for (size_t k = 0; k < L; ++k) {
        // Store V_k
        V_all.emplace_back(N);
        for (size_t i = 0; i < N; ++i) V_all.back().data[i] = V.data[i];

        // U_k = M^T · V_k
        BlockVector U(n);
        bw_spmv_transpose(csr, V, U, pool);
        U_all.push_back(std::move(U));

        // V_{k+1} = B · V_k = M · M^T · V_k
        if (k + 1 < L) {
            bw_spmv_symmetric(csr, V, Vnext, tmp_t, pool);
            std::swap(V.data, Vnext.data);
        }
    }
    std::cout << " done" << std::endl;

    // === Phase 2: Gaussian elimination on [U_0 | U_1 | ... | U_{L-1}] ===
    // Matrix has n rows and 64·L columns (over GF(2))
    // Each U_k contributes 64 columns: column (64k + j) is bit j of U_k
    //
    // We augment with an identity block of size (64L × 64L) to track
    // which linear combination produces each null vector.

    std::cout << "  [BW] Phase 2: Gaussian on " << n << "×" << total_krylov_cols
              << " matrix..." << std::flush;

    // Build the transposed system: 64L rows × n columns, augmented with 64L identity
    // Row (64k + j) = [bit j of U_k[0], bit j of U_k[1], ..., bit j of U_k[n-1] | identity row]
    size_t num_rows = total_krylov_cols;
    size_t eq_cols = n;
    size_t total_bits = eq_cols + num_rows;  // equation part + identity tracker

    std::vector<PackedBitRow> gauss_rows(num_rows, PackedBitRow(total_bits));

    // Fill equation columns
    for (size_t k = 0; k < L; ++k) {
        for (int j = 0; j < 64; ++j) {
            size_t row_idx = k * 64 + static_cast<size_t>(j);
            // This row's equation part: bit j of U_all[k] for each of the n components
            for (size_t c = 0; c < n; ++c) {
                if ((U_all[k].data[c] >> j) & 1) {
                    gauss_rows[row_idx].set(c);
                }
            }
            // Identity tracker part
            gauss_rows[row_idx].set(eq_cols + row_idx);
        }
    }

    // Gaussian elimination (reduce equation part to row echelon)
    // O(1) pivot-used lookup via boolean vector
    std::vector<bool> row_is_pivot(num_rows, false);
    for (size_t col = 0; col < eq_cols; ++col) {
        // Find pivot: first non-pivot row with bit col set
        int piv = -1;
        for (size_t r = 0; r < num_rows; ++r) {
            if (!row_is_pivot[r] && gauss_rows[r].test(col)) {
                piv = static_cast<int>(r);
                break;
            }
        }
        if (piv < 0) continue;
        row_is_pivot[static_cast<size_t>(piv)] = true;

        // Eliminate column from all other rows
        for (size_t r = 0; r < num_rows; ++r) {
            if (static_cast<int>(r) != piv && gauss_rows[r].test(col)) {
                gauss_rows[r].xor_with(gauss_rows[static_cast<size_t>(piv)]);
            }
        }
    }

    // Find null vectors: rows where the equation part is all zero
    // The identity tracker part gives the linear combination coefficients
    std::vector<std::vector<bool>> deps;

    for (size_t r = 0; r < num_rows && deps.size() < max_deps; ++r) {
        if (!gauss_rows[r].is_zero(0, eq_cols)) continue;

        // This row is in the null space. Extract coefficients from identity part.
        // Coefficient for Krylov step k, bit j = gauss_rows[r].test(eq_cols + 64k + j)
        // Accumulate: w = sum_{k,j} c_{k,j} · column_j(V_k)

        BlockVector W(N);
        W.clear();

        for (size_t k = 0; k < L; ++k) {
            uint64_t coeff = 0;
            for (int j = 0; j < 64; ++j) {
                if (gauss_rows[r].test(eq_cols + k * 64 + static_cast<size_t>(j))) {
                    coeff |= 1ULL << j;
                }
            }
            if (coeff == 0) continue;

            // W += V_all[k] masked by coeff
            // For each row i: W[i] ^= (V_all[k][i] & coeff) reduced to single bits
            // Actually, we need: for each bit j in coeff, W[i] ^= (V_all[k][i] >> j) & 1
            // But W is a vector of SINGLE bits (std::vector<bool>), not a block vector.
            // Let's use bit 0 of a block vector to accumulate:
            for (size_t i = 0; i < N; ++i) {
                // XOR all selected bits of V_all[k][i] into bit 0 of W[i]
                uint64_t selected = V_all[k].data[i] & coeff;
                uint64_t parity = static_cast<uint64_t>(__builtin_parityll(selected));
                W.data[i] ^= parity;
            }
        }

        // Extract bit 0 as the candidate
        auto candidate = W.extract_column(0);

        // Check non-zero
        bool nonzero = false;
        for (size_t i = 0; i < candidate.size(); ++i)
            if (candidate[i]) { nonzero = true; break; }
        if (!nonzero) continue;

        // Verify: M^T · candidate = 0
        std::vector<uint8_t> col_sum(n, 0);
        for (size_t row = 0; row < m; ++row) {
            if (!candidate[row]) continue;
            for (const uint32_t* p = csr.row_begin(row); p != csr.row_end(row); ++p)
                col_sum[*p] ^= 1;
        }

        bool valid = true;
        for (size_t c = 0; c < n; ++c) {
            if (col_sum[c]) { valid = false; break; }
        }

        if (valid) {
            deps.push_back(std::move(candidate));
        }
    }

    std::cout << " " << deps.size() << " deps found" << std::endl;
    return deps;
}

// ============================================================================
// Reserved for future matrix BM implementation.
// Current algorithm uses Krylov+Gaussian (block_wiedemann_solve) instead.
// ============================================================================

std::vector<DenseGF2_64x64> BlockWiedemann::compute_krylov_sequence(
    const CSRMatrix&, size_t, size_t, const BlockVector&, BlockVector&) {
    assert(false && "compute_krylov_sequence: not used in current BW path");
    return {};
}

BlockWiedemann::LingenResult BlockWiedemann::matrix_berlekamp_massey(
    const std::vector<DenseGF2_64x64>&, size_t) {
    assert(false && "matrix_berlekamp_massey: not used in current BW path");
    LingenResult r;
    r.valid_mask = 0;
    r.degrees.fill(0);
    return r;
}

std::vector<std::vector<bool>> BlockWiedemann::extract_solutions(
    const CSRMatrix&, size_t, const LingenResult&, const BlockVector&, size_t) {
    assert(false && "extract_solutions: not used in current BW path");
    return {};
}

} // namespace gnfs::linalg
