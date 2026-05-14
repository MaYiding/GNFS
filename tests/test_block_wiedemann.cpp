// test_block_wiedemann.cpp — Block Wiedemann correctness tests
//
// Verifies that Block Wiedemann produces valid GF(2) null space vectors
// by cross-validating against Block Lanczos on the same matrices.

#include <gnfs/linalg/block_wiedemann.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <gnfs/linalg/sparse_matrix.hpp>
#include <iostream>
#include <random>

using gnfs::linalg::SparseMatrix;
using gnfs::linalg::BlockLanczos;
using gnfs::linalg::BlockWiedemann;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; \
        tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_PASS(name) do { \
    std::cout << "  PASS: " << name << "\n"; \
    tests_passed++; \
} while(0)

/// Verify that v^T · M = 0 over GF(2)
static bool verify_dependency(const SparseMatrix& M, const std::vector<bool>& v) {
    if (v.size() != M.num_rows()) return false;

    size_t ncols = M.num_cols();
    std::vector<uint8_t> col_sum(ncols, 0);

    for (size_t r = 0; r < M.num_rows(); ++r) {
        if (!v[r]) continue;
        for (uint32_t c : M.row(r).indices()) {
            if (c < ncols) col_sum[c] ^= 1;
        }
    }

    for (size_t c = 0; c < ncols; ++c) {
        if (col_sum[c]) return false;
    }
    return true;
}

/// Build a matrix with guaranteed null space by making some rows linear combinations
static SparseMatrix build_matrix_with_nullspace(size_t rows, size_t cols,
                                                 size_t extra_rows, uint32_t seed) {
    // Build (rows + extra_rows) × cols matrix where extra rows
    // are XOR of random subsets of original rows
    SparseMatrix M(rows + extra_rows, cols);
    std::mt19937 rng(seed);

    // Base rows: random with ~5 nonzeros each
    for (size_t i = 0; i < rows; ++i) {
        size_t nnz = 3 + rng() % 5;
        for (size_t k = 0; k < nnz; ++k) {
            M.row(i).set(static_cast<uint32_t>(rng() % cols));
        }
    }

    // Extra rows: XOR of 2-3 base rows → creates dependencies
    for (size_t i = 0; i < extra_rows; ++i) {
        size_t r1 = rng() % rows;
        size_t r2 = rng() % rows;
        M.row(rows + i).xor_with(M.row(r1));
        M.row(rows + i).xor_with(M.row(r2));
    }

    return M;
}

// ============================================================================
// Test cases — DenseGF2_64x128 foundation (P2 Stage A.1)
// ============================================================================

void test_dense_64x128_clear_and_identity() {
    using gnfs::linalg::DenseGF2_64x128;
    DenseGF2_64x128 m;
    m.clear();
    for (int j = 0; j < 128; ++j) TEST_ASSERT(m.get_col(j) == 0, "clear: all cols zero");

    m.set_left_identity();
    for (int j = 0; j < 64; ++j)
        TEST_ASSERT(m.get_col(j) == (1ULL << j), "left identity: cols 0..63 = e_j");
    for (int j = 64; j < 128; ++j)
        TEST_ASSERT(m.get_col(j) == 0, "left identity: cols 64..127 zero");
    TEST_PASS("DenseGF2_64x128 clear + set_left_identity");
}

void test_dense_64x128_set_get_col() {
    using gnfs::linalg::DenseGF2_64x128;
    DenseGF2_64x128 m;
    m.clear();
    std::mt19937_64 rng(0xC0FFEE);
    uint64_t expected[128];
    for (int j = 0; j < 128; ++j) {
        expected[j] = rng();
        m.set_col(j, expected[j]);
    }
    for (int j = 0; j < 128; ++j)
        TEST_ASSERT(m.get_col(j) == expected[j], "set/get round-trip");
    TEST_PASS("DenseGF2_64x128 set_col / get_col round-trip");
}

void test_dense_64x128_xor_cols() {
    using gnfs::linalg::DenseGF2_64x128;
    DenseGF2_64x128 m;
    m.clear();
    m.set_col(10, 0xAAAAAAAAAAAAAAAAULL);
    m.set_col(20, 0x5555555555555555ULL);
    m.xor_cols(10, 20);
    TEST_ASSERT(m.get_col(10) == 0xFFFFFFFFFFFFFFFFULL, "xor_cols result");
    TEST_ASSERT(m.get_col(20) == 0x5555555555555555ULL, "xor_cols src unchanged");
    TEST_PASS("DenseGF2_64x128 xor_cols");
}

void test_dense_64x128_swap_cols() {
    using gnfs::linalg::DenseGF2_64x128;
    DenseGF2_64x128 m;
    m.clear();
    m.set_col(5, 0xDEADBEEFCAFEBABEULL);
    m.set_col(100, 0x1234567890ABCDEFULL);
    m.swap_cols(5, 100);
    TEST_ASSERT(m.get_col(5) == 0x1234567890ABCDEFULL, "swap: col5 got col100");
    TEST_ASSERT(m.get_col(100) == 0xDEADBEEFCAFEBABEULL, "swap: col100 got col5");
    // self-swap is no-op
    m.swap_cols(5, 5);
    TEST_ASSERT(m.get_col(5) == 0x1234567890ABCDEFULL, "self-swap no-op");
    TEST_PASS("DenseGF2_64x128 swap_cols");
}

void test_dense_64x128_xor_with() {
    using gnfs::linalg::DenseGF2_64x128;
    DenseGF2_64x128 a, b;
    std::mt19937_64 rng(0xBEEF);
    for (int j = 0; j < 128; ++j) {
        a.set_col(j, rng());
        b.set_col(j, rng());
    }
    DenseGF2_64x128 a_copy = a;
    a.xor_with(b);
    for (int j = 0; j < 128; ++j)
        TEST_ASSERT(a.get_col(j) == (a_copy.get_col(j) ^ b.get_col(j)), "xor_with per col");
    TEST_PASS("DenseGF2_64x128 xor_with (full matrix)");
}

void test_dense_64x128_is_zero() {
    using gnfs::linalg::DenseGF2_64x128;
    DenseGF2_64x128 m;
    m.clear();
    TEST_ASSERT(m.is_zero(), "cleared is zero");
    m.set_col(64, 1ULL << 7);
    TEST_ASSERT(!m.is_zero(), "after set_col not zero");
    m.set_col(64, 0);
    TEST_ASSERT(m.is_zero(), "after clear back to zero");
    TEST_PASS("DenseGF2_64x128 is_zero");
}

void test_mksol_accumulate_identity() {
    // V · I = V
    using gnfs::linalg::BlockVector;
    using gnfs::linalg::DenseGF2_64x64;
    using gnfs::linalg::mksol_accumulate;

    const size_t m = 100;
    BlockVector V(m), acc(m);
    std::mt19937_64 rng(0x1234);
    for (size_t r = 0; r < m; ++r) V.data[r] = rng();

    DenseGF2_64x64 I;
    I.set_identity();
    mksol_accumulate(V, I, acc);

    for (size_t r = 0; r < m; ++r)
        TEST_ASSERT(acc.data[r] == V.data[r], "V·I row matches V");
    TEST_PASS("mksol_accumulate identity (acc += V·I = V)");
}

void test_mksol_accumulate_zero() {
    using gnfs::linalg::BlockVector;
    using gnfs::linalg::DenseGF2_64x64;
    using gnfs::linalg::mksol_accumulate;

    const size_t m = 50;
    BlockVector V(m), acc(m);
    std::mt19937_64 rng(0xABCD);
    for (size_t r = 0; r < m; ++r) {
        V.data[r] = rng();
        acc.data[r] = rng();  // non-zero starting accumulator
    }
    BlockVector acc_before = acc;

    DenseGF2_64x64 Z;
    Z.clear();
    mksol_accumulate(V, Z, acc);

    for (size_t r = 0; r < m; ++r)
        TEST_ASSERT(acc.data[r] == acc_before.data[r], "V·0 = 0, acc unchanged");
    TEST_PASS("mksol_accumulate zero matrix (acc += V·0 = 0)");
}

void test_mksol_accumulate_xor_accumulation() {
    // acc += V·F1, then acc += V·F2 should equal V·(F1 ^ F2) (linearity over GF(2))
    using gnfs::linalg::BlockVector;
    using gnfs::linalg::DenseGF2_64x64;
    using gnfs::linalg::mksol_accumulate;

    const size_t m = 30;
    BlockVector V(m), acc1(m), acc2(m);
    std::mt19937_64 rng(0xDEED);
    for (size_t r = 0; r < m; ++r) V.data[r] = rng();

    DenseGF2_64x64 F1, F2, F_xor;
    for (int i = 0; i < 64; ++i) {
        F1.rows[i] = rng();
        F2.rows[i] = rng();
        F_xor.rows[i] = F1.rows[i] ^ F2.rows[i];
    }

    mksol_accumulate(V, F1, acc1);
    mksol_accumulate(V, F2, acc1);

    mksol_accumulate(V, F_xor, acc2);

    for (size_t r = 0; r < m; ++r)
        TEST_ASSERT(acc1.data[r] == acc2.data[r], "linearity: V·F1 + V·F2 = V·(F1+F2)");
    TEST_PASS("mksol_accumulate linearity over GF(2)");
}

void test_mksol_accumulate_against_naive() {
    // Direct V·F computation: (V·F)[r, j] = XOR_i V[r, i] · F[i, j]
    using gnfs::linalg::BlockVector;
    using gnfs::linalg::DenseGF2_64x64;
    using gnfs::linalg::mksol_accumulate;

    const size_t m = 17;
    BlockVector V(m), acc(m);
    std::mt19937_64 rng(0x42424242);
    for (size_t r = 0; r < m; ++r) V.data[r] = rng();

    DenseGF2_64x64 F;
    for (int i = 0; i < 64; ++i) F.rows[i] = rng();

    mksol_accumulate(V, F, acc);

    // Naive: for each (r, j), XOR over i where V[r,i]=1 of F[i,j]
    for (size_t r = 0; r < m; ++r) {
        uint64_t expected = 0;
        for (int j = 0; j < 64; ++j) {
            uint64_t bit_j = 0;
            for (int i = 0; i < 64; ++i) {
                if (((V.data[r] >> i) & 1) && ((F.rows[i] >> j) & 1))
                    bit_j ^= 1;
            }
            if (bit_j) expected |= (1ULL << j);
        }
        TEST_ASSERT(acc.data[r] == expected, "matches naive XOR computation");
    }
    TEST_PASS("mksol_accumulate matches naive entry-by-entry");
}

void test_dense_64x128_extract_halves() {
    using gnfs::linalg::DenseGF2_64x128;
    using gnfs::linalg::DenseGF2_64x64;
    DenseGF2_64x128 m;
    // Build a known pattern: col j (left) = j, col j+64 (right) = j+1
    // i.e., cols[j] is the bit pattern of integer j (treating j as a 64-bit number)
    for (int j = 0; j < 64; ++j) m.set_col(j, static_cast<uint64_t>(j));
    for (int j = 0; j < 64; ++j) m.set_col(64 + j, static_cast<uint64_t>(j + 1));

    DenseGF2_64x64 left = m.extract_left();
    DenseGF2_64x64 right = m.extract_right();

    // left[i] (row i) bit j = bit i of cols[j] = bit i of j (as integer)
    // bit i of j is 1 iff (j >> i) & 1
    for (int i = 0; i < 64; ++i) {
        uint64_t expected_left = 0;
        for (int j = 0; j < 64; ++j) {
            if ((static_cast<uint64_t>(j) >> i) & 1) expected_left |= (1ULL << j);
        }
        TEST_ASSERT(left.rows[i] == expected_left, "extract_left row");
        uint64_t expected_right = 0;
        for (int j = 0; j < 64; ++j) {
            if ((static_cast<uint64_t>(j + 1) >> i) & 1) expected_right |= (1ULL << j);
        }
        TEST_ASSERT(right.rows[i] == expected_right, "extract_right row");
    }
    TEST_PASS("DenseGF2_64x128 extract_left / extract_right");
}

// ============================================================================
// Test cases — original Block Wiedemann tests
// ============================================================================

void test_scalar_bm_basic() {
    // Test the BM algorithm on a known sequence
    // The all-zero sequence should give trivial result
    // (BW delegates small matrices to Gaussian, so this tests the overall API)

    SparseMatrix M = build_matrix_with_nullspace(100, 80, 30, 12345);

    BlockWiedemann bw;
    auto deps = bw.find_dependencies(M, 10);

    TEST_ASSERT(deps.size() > 0, "should find at least one dependency");

    for (const auto& dep : deps) {
        bool valid = verify_dependency(M, dep);
        TEST_ASSERT(valid, "dependency should satisfy v^T M = 0");
    }

    TEST_PASS("scalar BM basic — small matrix (Gaussian path)");
}

void test_cross_validate_small() {
    // Both BL and BW should find valid dependencies on the same matrix
    SparseMatrix M = build_matrix_with_nullspace(200, 150, 60, 54321);

    BlockLanczos bl;
    auto bl_deps = bl.find_dependencies(M, 10);

    BlockWiedemann bw;
    auto bw_deps = bw.find_dependencies(M, 10);

    TEST_ASSERT(bl_deps.size() > 0, "BL should find deps");
    TEST_ASSERT(bw_deps.size() > 0, "BW should find deps");

    // Both should produce valid dependencies
    for (const auto& dep : bl_deps) {
        TEST_ASSERT(verify_dependency(M, dep), "BL dep should be valid");
    }
    for (const auto& dep : bw_deps) {
        TEST_ASSERT(verify_dependency(M, dep), "BW dep should be valid");
    }

    TEST_PASS("cross-validate small — BL vs BW both produce valid deps");
}

void test_overdetermined_matrix() {
    // More rows than columns (typical GNFS: rows > cols by 10-50%)
    SparseMatrix M = build_matrix_with_nullspace(500, 400, 120, 99999);

    BlockWiedemann bw;
    auto deps = bw.find_dependencies(M, 20);

    TEST_ASSERT(deps.size() > 0, "should find dependencies in overdetermined matrix");

    size_t valid_count = 0;
    for (const auto& dep : deps) {
        if (verify_dependency(M, dep)) valid_count++;
    }
    TEST_ASSERT(valid_count > 0, "at least one dependency should be valid");

    TEST_PASS("overdetermined matrix (500×400 + 120 deps)");
}

void test_sparse_gnfs_like() {
    // Simulate a GNFS-like sparse matrix:
    // - Rows >> Cols (overdetermined)
    // - Each row has ~20-30 nonzeros (typical for FB primes + LP + QC + Schirokauer)
    // - Guaranteed dependencies from row construction
    size_t rows = 800;
    size_t cols = 600;
    size_t extra = 220;

    SparseMatrix M(rows + extra, cols);
    std::mt19937 rng(42424242);

    // Base rows with ~20 nonzeros
    for (size_t i = 0; i < rows; ++i) {
        size_t nnz = 15 + rng() % 15;
        for (size_t k = 0; k < nnz; ++k) {
            M.row(i).set(static_cast<uint32_t>(rng() % cols));
        }
    }

    // Extra rows: XOR of 3-5 random base rows (more complex dependencies)
    for (size_t i = 0; i < extra; ++i) {
        size_t num_sources = 3 + rng() % 3;
        for (size_t s = 0; s < num_sources; ++s) {
            M.row(rows + i).xor_with(M.row(rng() % rows));
        }
    }

    BlockWiedemann bw;
    auto deps = bw.find_dependencies(M, 10);

    TEST_ASSERT(deps.size() > 0, "BW should find deps in GNFS-like matrix");

    for (const auto& dep : deps) {
        TEST_ASSERT(verify_dependency(M, dep), "GNFS-like dep should be valid");
    }

    TEST_PASS("GNFS-like sparse matrix (1020×600, ~20 nnz/row)");
}

void test_identity_no_nullspace() {
    // Square identity matrix: no null space
    size_t n = 64;
    SparseMatrix M(n, n);
    for (size_t i = 0; i < n; ++i) {
        M.row(i).set(static_cast<uint32_t>(i));
    }

    BlockWiedemann bw;
    auto deps = bw.find_dependencies(M, 5);

    // Should find 0 dependencies (identity has trivial null space)
    TEST_ASSERT(deps.empty(), "identity matrix should have no dependencies");

    TEST_PASS("identity matrix — no null space");
}

void test_repeated_rows() {
    // Matrix with some duplicate rows → known dependencies
    size_t n = 100;
    SparseMatrix M(n + 10, 80);
    std::mt19937 rng(777);

    for (size_t i = 0; i < n; ++i) {
        size_t nnz = 3 + rng() % 5;
        for (size_t k = 0; k < nnz; ++k) {
            M.row(i).set(static_cast<uint32_t>(rng() % 80));
        }
    }

    // Rows n..n+9 are copies of rows 0..9
    for (size_t i = 0; i < 10; ++i) {
        M.row(n + i).xor_with(M.row(i));
    }

    BlockWiedemann bw;
    auto deps = bw.find_dependencies(M, 10);

    TEST_ASSERT(deps.size() > 0, "repeated rows should create dependencies");

    for (const auto& dep : deps) {
        TEST_ASSERT(verify_dependency(M, dep), "repeated-row dep should be valid");
    }

    TEST_PASS("repeated rows — known dependencies");
}

void test_large_matrix_bw_path() {
    // Matrix > 5000 rows to exercise the true Block Wiedemann path.
    // Key: keep cols small (=200) so rank(B) ≤ 200 and scalar BM converges
    // quickly. L = 2·200 + 100 = 500 (not 8000).
    size_t base_rows = 200;
    size_t cols = 200;
    size_t extra = 5200;  // 5400 total rows >> 5000 threshold, rank ≤ 200

    std::cout << "  Building large matrix (" << base_rows + extra << "×" << cols << ")..." << std::flush;
    SparseMatrix M(base_rows + extra, cols);
    std::mt19937 rng(314159);

    // Base rows with ~10 nonzeros
    for (size_t i = 0; i < base_rows; ++i) {
        size_t nnz = 5 + rng() % 10;
        for (size_t k = 0; k < nnz; ++k) {
            M.row(i).set(static_cast<uint32_t>(rng() % cols));
        }
    }

    // Extra rows: XOR of 2-4 base rows → guaranteed dependencies
    for (size_t i = 0; i < extra; ++i) {
        size_t n_src = 2 + rng() % 3;
        for (size_t s = 0; s < n_src; ++s) {
            M.row(base_rows + i).xor_with(M.row(rng() % base_rows));
        }
    }
    std::cout << " done" << std::endl;

    BlockWiedemann bw;
    auto deps = bw.find_dependencies(M, 10);

    TEST_ASSERT(deps.size() > 0, "BW should find deps in large matrix (>5000 rows)");

    size_t valid = 0;
    for (const auto& dep : deps) {
        if (verify_dependency(M, dep)) valid++;
    }
    TEST_ASSERT(valid > 0, "large matrix BW deps should be valid");

    std::cout << "  (found " << deps.size() << " deps, " << valid << " verified)" << std::endl;
    TEST_PASS("large matrix — true BW path (5400×200, rank≤200)");
}

int main() {
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  Block Wiedemann Unit Tests\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    // P2 Stage A.1 — DenseGF2_64x128 foundation
    test_dense_64x128_clear_and_identity();
    test_dense_64x128_set_get_col();
    test_dense_64x128_xor_cols();
    test_dense_64x128_swap_cols();
    test_dense_64x128_xor_with();
    test_dense_64x128_is_zero();
    test_dense_64x128_extract_halves();

    // P2 Stage A.2 — mksol_accumulate primitive (Phase 3)
    test_mksol_accumulate_identity();
    test_mksol_accumulate_zero();
    test_mksol_accumulate_xor_accumulation();
    test_mksol_accumulate_against_naive();

    // Original BW tests
    test_scalar_bm_basic();
    test_cross_validate_small();
    test_overdetermined_matrix();
    test_sparse_gnfs_like();
    test_identity_no_nullspace();
    test_repeated_rows();
    test_large_matrix_bw_path();

    std::cout << "\n═══════════════════════════════════════════\n";
    std::cout << "  Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
    std::cout << "═══════════════════════════════════════════\n";

    return tests_failed > 0 ? 1 : 0;
}
