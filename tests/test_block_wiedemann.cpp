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
// Test cases
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

int main() {
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  Block Wiedemann Unit Tests\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    test_scalar_bm_basic();
    test_cross_validate_small();
    test_overdetermined_matrix();
    test_sparse_gnfs_like();
    test_identity_no_nullspace();
    test_repeated_rows();

    std::cout << "\n═══════════════════════════════════════════\n";
    std::cout << "  Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
    std::cout << "═══════════════════════════════════════════\n";

    return tests_failed > 0 ? 1 : 0;
}
