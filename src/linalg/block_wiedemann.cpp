#include "gnfs/linalg/block_wiedemann.hpp"
#include "gnfs/util/thread_pool.hpp"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <random>

namespace gnfs::linalg {

// ============================================================================
// SpMV utilities (same as block_lanczos.cpp — shared via identical implementation)
// ============================================================================

namespace {

/// Forward SpMV: y = M * x
void bw_spmv_forward(const CSRMatrix& M, const BlockVector& x, BlockVector& y,
                     gnfs::util::ThreadPool& pool) {
    pool.parallel_for_index(0, M.num_rows(), [&](size_t i) {
        uint64_t acc = 0;
        for (const uint32_t* p = M.row_begin(i); p != M.row_end(i); ++p)
            if (*p < x.length) acc ^= x.data[*p];
        y.data[i] = acc;
    });
}

/// Transpose SpMV: y = M^T * x
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

/// Inner product: C = X^T * V (64×64)
DenseGF2_64x64 bw_inner_product(const BlockVector& X, const BlockVector& V,
                                gnfs::util::ThreadPool& pool) {
    const size_t m = X.length;
    const size_t T = pool.num_threads();
    const size_t chunk = (m + T - 1) / T;

    std::vector<DenseGF2_64x64> locals(T);
    for (auto& l : locals) l.clear();
    std::vector<std::future<void>> futures;

    for (size_t t = 0; t < T; ++t) {
        size_t start = t * chunk;
        size_t end = std::min(start + chunk, m);
        if (start >= m) break;
        futures.push_back(pool.submit([&X, &V, &locals, t, start, end]() {
            DenseGF2_64x64& C = locals[t];
            for (size_t i = start; i < end; ++i) {
                uint64_t ai = X.data[i], bi = V.data[i];
                while (ai) {
                    C.rows[__builtin_ctzll(ai)] ^= bi;
                    ai &= ai - 1;
                }
            }
        }));
    }
    for (auto& f : futures) f.get();

    DenseGF2_64x64 result;
    result.clear();
    for (auto& l : locals) result.xor_with(l);
    return result;
}

/// Symmetric SpMV: y = M·M^T·x
void bw_spmv_symmetric(const CSRMatrix& M, const BlockVector& x, BlockVector& y,
                       BlockVector& tmp, gnfs::util::ThreadPool& pool) {
    bw_spmv_transpose(M, x, tmp, pool);
    bw_spmv_forward(M, tmp, y, pool);
}

// ============================================================================
// Scalar Berlekamp-Massey over GF(2)
//
// Finds the shortest LFSR (= minimal polynomial) generating the binary
// sequence s_0, s_1, ..., s_{2D-1}.
//
// Returns polynomial C of degree L where:
//   sum_{j=0}^{L} C[j] * s[n-j] = 0   for all n >= L
//
// Over GF(2), the update rule simplifies: no division needed (b=1 always).
// ============================================================================

struct BM_Result {
    std::vector<uint8_t> poly;  // Coefficients: poly[0] + poly[1]*t + ...
    int degree;                 // Degree of the polynomial
    bool constant_zero;         // Whether poly[0] == 0 (needed for extraction)
};

BM_Result scalar_berlekamp_massey(const std::vector<uint8_t>& s) {
    const size_t len = s.size();

    // C = current polynomial (C[0] = 1 initially)
    std::vector<uint8_t> C = {1};
    // B = previous polynomial
    std::vector<uint8_t> B = {1};
    int L = 0;   // Current span (LFSR length)
    int m = 1;   // Shift counter

    for (size_t n = 0; n < len; ++n) {
        // Compute discrepancy: d = sum_{j=0}^{L} C[j] * s[n-j]
        uint8_t d = 0;
        for (size_t j = 0; j < C.size() && j <= n; ++j) {
            d ^= (C[j] & s[n - j]);
        }

        if (d == 0) {
            m++;
        } else {
            // T = C (save current)
            std::vector<uint8_t> T = C;

            // C = C XOR (x^m * B)
            size_t new_len = std::max(C.size(), B.size() + static_cast<size_t>(m));
            C.resize(new_len, 0);
            for (size_t j = 0; j < B.size(); ++j) {
                C[j + static_cast<size_t>(m)] ^= B[j];
            }

            if (2 * L <= static_cast<int>(n)) {
                L = static_cast<int>(n) + 1 - L;
                B = T;
                m = 1;
            } else {
                m++;
            }
        }
    }

    BM_Result result;
    result.poly = C;
    result.degree = L;
    result.constant_zero = (C.size() > 0 && C[0] == 0);
    return result;
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

std::vector<std::vector<bool>> BlockWiedemann::find_dependencies(
    const SparseMatrix& matrix, size_t max_deps) {

    const size_t m = matrix.num_rows();
    const size_t n = matrix.num_cols();
    if (m == 0 || n == 0) return {};

    // For small matrices, delegate to Gaussian (same as BL)
    if (m < 5000 && n < 5000) {
        BlockLanczos bl;
        return bl.find_dependencies(matrix, max_deps);
    }

    return block_wiedemann_solve(matrix, max_deps);
}

// ============================================================================
// Block Wiedemann: Three-Phase Algorithm
//
// Phase 1 (Krylov): V_i = B^i·Y, A_i = X^T·V_i (64×64 matrices)
// Phase 2 (BM):     64 independent scalar BM on diagonal entries of {A_i}
// Phase 3 (Extract): For each BM with f_0=0, accumulate null vector from V_k
// ============================================================================

std::vector<std::vector<bool>> BlockWiedemann::block_wiedemann_solve(
    const SparseMatrix& matrix, size_t max_deps) {

    const size_t m = matrix.num_rows();
    const size_t n = matrix.num_cols();
    const size_t N = m;  // B = M·M^T is m×m

    std::cout << "  [BW] Block Wiedemann: " << m << "×" << n
              << " matrix, N=" << N << std::endl;

    CSRMatrix csr(matrix);

    // Sequence length: need 2·ceil(N/64) + safety for BM to converge
    // BM needs >= 2L terms where L = rank(B) <= min(m,n)
    const size_t L = 2 * ((N + 63) / 64) + 64;

    std::cout << "  [BW] Sequence length L=" << L << std::endl;

    // Random initialization
    std::mt19937_64 rng(42);
    BlockVector X(N), Y(N);
    for (size_t i = 0; i < N; ++i) { X.data[i] = rng(); Y.data[i] = rng(); }

    // === Phase 1: Krylov sequence + store all V_k ===
    std::cout << "  [BW] Phase 1: Krylov sequence..." << std::flush;

    gnfs::util::ThreadPool pool(0);
    std::vector<DenseGF2_64x64> A_seq(L);
    // Store all Krylov vectors V_k for Phase 3 (OK for 25-digit: L~128, N~2K → ~2MB)
    std::vector<BlockVector> V_all(L, BlockVector(N));

    BlockVector V(N), Vnext(N), tmp(n);
    for (size_t i = 0; i < N; ++i) V.data[i] = Y.data[i];

    for (size_t k = 0; k < L; ++k) {
        // Store V_k
        for (size_t i = 0; i < N; ++i) V_all[k].data[i] = V.data[i];
        // A_k = X^T · V_k
        A_seq[k] = bw_inner_product(X, V, pool);
        // V_{k+1} = B · V_k = M · M^T · V_k
        if (k + 1 < L) {
            bw_spmv_symmetric(csr, V, Vnext, tmp, pool);
            std::swap(V.data, Vnext.data);
        }
    }
    std::cout << " done (" << L << " terms)" << std::endl;

    // === Phase 2: 64 independent scalar BM ===
    std::cout << "  [BW] Phase 2: Scalar Berlekamp-Massey ×64..." << std::flush;

    struct ColumnBM {
        BM_Result bm;
        int col_idx;  // which diagonal entry (0..63)
    };
    std::vector<ColumnBM> valid_bms;

    for (int c = 0; c < 64; ++c) {
        // Extract scalar sequence: s_k = A_k[c][c] (diagonal entry c)
        std::vector<uint8_t> scalar_seq(L);
        for (size_t k = 0; k < L; ++k) {
            scalar_seq[k] = (A_seq[k].rows[c] >> c) & 1;
        }

        auto bm = scalar_berlekamp_massey(scalar_seq);

        if (bm.constant_zero && bm.degree > 0) {
            valid_bms.push_back({std::move(bm), c});
        }
    }

    std::cout << " " << valid_bms.size() << " valid (f_0=0)" << std::endl;

    // Free A_seq
    A_seq.clear();
    A_seq.shrink_to_fit();

    // === Phase 3: Solution extraction ===
    //
    // For each valid BM result on column c:
    //   f(t) = f_0 + f_1*t + ... + f_D*t^D  with f_0 = 0
    //   g(t) = f_1 + f_2*t + ... + f_D*t^{D-1}   (f shifted by t)
    //   w_c = g(B)·y_c = sum_{k=0}^{D-1} g_k · B^k · y_c
    //
    //   y_c = column c of Y (block vector), so y_c[i] = (Y.data[i] >> c) & 1
    //   B^k · y_c = column c of V_k
    //   w_c[i] = XOR_{k: g_k=1} (V_k.data[i] >> c) & 1
    //
    // We pack all candidates into a single BlockVector W:
    //   W.data[i] bit c = w_c[i]

    std::cout << "  [BW] Phase 3: Extracting solutions..." << std::flush;

    BlockVector W(N);
    W.clear();

    for (const auto& cbm : valid_bms) {
        int c = cbm.col_idx;
        const auto& poly = cbm.bm.poly;
        uint64_t col_mask = 1ULL << c;

        // g[k] = poly[k+1] for k = 0..degree-1
        for (size_t k = 0; k + 1 < poly.size() && k < V_all.size(); ++k) {
            if (poly[k + 1] == 0) continue;
            // W bit c ^= V_all[k] bit c
            for (size_t i = 0; i < N; ++i) {
                W.data[i] ^= V_all[k].data[i] & col_mask;
            }
        }
    }

    // Free stored Krylov vectors
    V_all.clear();
    V_all.shrink_to_fit();

    // Verify each candidate: M^T · w = 0
    std::vector<std::vector<bool>> deps;

    for (const auto& cbm : valid_bms) {
        if (deps.size() >= max_deps) break;
        int c = cbm.col_idx;

        auto candidate = W.extract_column(c);

        // Check non-zero
        bool nonzero = false;
        for (size_t i = 0; i < candidate.size(); ++i)
            if (candidate[i]) { nonzero = true; break; }
        if (!nonzero) continue;

        // Verify: v^T · M = 0 (left null space)
        // Equivalently: for each column j of M, XOR of M[r][j] for r in support(v) = 0
        std::vector<uint8_t> col_sum(n, 0);
        for (size_t r = 0; r < m; ++r) {
            if (!candidate[r]) continue;
            for (const uint32_t* p = csr.row_begin(r); p != csr.row_end(r); ++p)
                col_sum[*p] ^= 1;
        }

        bool valid = true;
        for (size_t j = 0; j < n; ++j) {
            if (col_sum[j]) { valid = false; break; }
        }

        if (valid) {
            deps.push_back(std::move(candidate));
        }
    }

    std::cout << " " << deps.size() << " verified" << std::endl;
    return deps;
}

// ============================================================================
// Phase 1 helper (used by the standalone version; the three-phase solver
// above integrates Phase 1 directly for memory efficiency)
// ============================================================================

std::vector<DenseGF2_64x64> BlockWiedemann::compute_krylov_sequence(
    const CSRMatrix& csr, size_t N, size_t L,
    const BlockVector& X, BlockVector& V) {

    gnfs::util::ThreadPool pool(0);
    std::vector<DenseGF2_64x64> sequence(L);
    BlockVector tmp(csr.num_cols());
    BlockVector Vnext(N);

    for (size_t i = 0; i < L; ++i) {
        sequence[i] = bw_inner_product(X, V, pool);
        if (i + 1 < L) {
            bw_spmv_symmetric(csr, V, Vnext, tmp, pool);
            std::swap(V.data, Vnext.data);
        }
    }
    return sequence;
}

// Stubs for header-declared methods (lingen and extract used inline above)
BlockWiedemann::LingenResult BlockWiedemann::matrix_berlekamp_massey(
    const std::vector<DenseGF2_64x64>& /*seq*/, size_t /*N*/) {
    // Not used in the scalar BM path — stub for API compatibility
    LingenResult r;
    r.valid_mask = 0;
    r.degrees.fill(0);
    return r;
}

std::vector<std::vector<bool>> BlockWiedemann::extract_solutions(
    const CSRMatrix& /*csr*/, size_t /*N*/,
    const LingenResult& /*lingen*/,
    const BlockVector& /*Y_initial*/,
    size_t /*max_deps*/) {
    // Not used in the scalar BM path — stub for API compatibility
    return {};
}

} // namespace gnfs::linalg
