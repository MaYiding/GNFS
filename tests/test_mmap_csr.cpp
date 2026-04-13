// test_mmap_csr.cpp — Mmap CSR matrix correctness tests
//
// Verifies that save→load round-trip produces identical SpMV results
// as the in-memory CSRMatrix.

#include <gnfs/linalg/mmap_csr_matrix.hpp>
#include <gnfs/linalg/sparse_matrix.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <iostream>
#include <random>
#include <cstdio>

using gnfs::linalg::SparseMatrix;
using gnfs::linalg::CSRMatrix;
using gnfs::linalg::MmapCSRMatrix;
using gnfs::linalg::BlockVector;

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

struct TempFile {
    std::string path;
    TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

/// Build a random sparse matrix
static SparseMatrix make_random_matrix(size_t rows, size_t cols, uint32_t seed, size_t nnz_per_row = 5) {
    SparseMatrix M(rows, cols);
    std::mt19937 rng(seed);
    for (size_t i = 0; i < rows; ++i) {
        for (size_t k = 0; k < nnz_per_row; ++k) {
            M.row(i).set(static_cast<uint32_t>(rng() % cols));
        }
    }
    return M;
}

/// Forward SpMV: y = M * x (templated to work with both CSRMatrix and MmapCSRMatrix)
template <typename CSR>
static void spmv_forward(const CSR& M, const BlockVector& x, BlockVector& y) {
    for (size_t i = 0; i < M.num_rows(); ++i) {
        uint64_t acc = 0;
        for (const uint32_t* p = M.row_begin(i); p != M.row_end(i); ++p) {
            if (*p < x.length) acc ^= x.data[*p];
        }
        y.data[i] = acc;
    }
}

// ============================================================================
// Tests
// ============================================================================

void test_save_load_roundtrip() {
    TempFile tmp("/tmp/gnfs_test_csr.csrmat");

    SparseMatrix sparse = make_random_matrix(100, 80, 12345);
    CSRMatrix csr(sparse);

    MmapCSRMatrix::save(csr, tmp.path);
    MmapCSRMatrix mmap(tmp.path);

    TEST_ASSERT(mmap.num_rows() == csr.num_rows(), "rows match");
    TEST_ASSERT(mmap.num_cols() == csr.num_cols(), "cols match");
    TEST_ASSERT(mmap.nnz() == csr.nnz(), "nnz match");

    // Compare all row pointers
    for (size_t i = 0; i < csr.num_rows(); ++i) {
        TEST_ASSERT(mmap.row_nnz(i) == csr.row_nnz(i),
                    "row " + std::to_string(i) + " nnz mismatch");
        const uint32_t* csr_begin = csr.row_begin(i);
        const uint32_t* mmap_begin = mmap.row_begin(i);
        for (size_t j = 0; j < csr.row_nnz(i); ++j) {
            TEST_ASSERT(csr_begin[j] == mmap_begin[j],
                        "row " + std::to_string(i) + " col " + std::to_string(j) + " mismatch");
        }
    }

    TEST_PASS("save/load round-trip (100×80)");
}

void test_spmv_identical() {
    TempFile tmp("/tmp/gnfs_test_csr_spmv.csrmat");

    SparseMatrix sparse = make_random_matrix(500, 300, 54321, 10);
    CSRMatrix csr(sparse);

    MmapCSRMatrix::save(csr, tmp.path);
    MmapCSRMatrix mmap(tmp.path);

    // Random input vector
    BlockVector x(300);
    std::mt19937_64 rng(99999);
    for (size_t i = 0; i < 300; ++i) x.data[i] = rng();

    // SpMV with both
    BlockVector y_csr(500), y_mmap(500);
    spmv_forward(csr, x, y_csr);
    spmv_forward(mmap, x, y_mmap);

    // Compare
    for (size_t i = 0; i < 500; ++i) {
        TEST_ASSERT(y_csr.data[i] == y_mmap.data[i],
                    "SpMV row " + std::to_string(i) + " differs");
    }

    TEST_PASS("SpMV CSR vs mmap-CSR identical (500×300)");
}

void test_large_matrix() {
    TempFile tmp("/tmp/gnfs_test_csr_large.csrmat");

    SparseMatrix sparse = make_random_matrix(10000, 8000, 42, 15);
    CSRMatrix csr(sparse);

    MmapCSRMatrix::save(csr, tmp.path);
    MmapCSRMatrix mmap(tmp.path);

    TEST_ASSERT(mmap.num_rows() == 10000, "10K rows");
    TEST_ASSERT(mmap.num_cols() == 8000, "8K cols");

    // Spot-check 100 random rows
    std::mt19937 rng(777);
    for (int trial = 0; trial < 100; ++trial) {
        size_t row = rng() % 10000;
        TEST_ASSERT(mmap.row_nnz(row) == csr.row_nnz(row),
                    "row " + std::to_string(row) + " nnz");
        const uint32_t* a = csr.row_begin(row);
        const uint32_t* b = mmap.row_begin(row);
        for (size_t j = 0; j < csr.row_nnz(row); ++j) {
            TEST_ASSERT(a[j] == b[j], "data mismatch at row " + std::to_string(row));
        }
    }

    // SpMV cross-check
    BlockVector x(8000);
    std::mt19937_64 rng2(555);
    for (size_t i = 0; i < 8000; ++i) x.data[i] = rng2();

    BlockVector y_csr(10000), y_mmap(10000);
    spmv_forward(csr, x, y_csr);
    spmv_forward(mmap, x, y_mmap);

    bool all_match = true;
    for (size_t i = 0; i < 10000; ++i) {
        if (y_csr.data[i] != y_mmap.data[i]) { all_match = false; break; }
    }
    TEST_ASSERT(all_match, "SpMV large matrix results identical");

    TEST_PASS("large matrix save/load/SpMV (10000×8000)");
}

void test_empty_matrix() {
    TempFile tmp("/tmp/gnfs_test_csr_empty.csrmat");

    SparseMatrix sparse(0, 0);
    CSRMatrix csr(sparse);

    MmapCSRMatrix::save(csr, tmp.path);
    MmapCSRMatrix mmap(tmp.path);

    TEST_ASSERT(mmap.num_rows() == 0, "0 rows");
    TEST_ASSERT(mmap.num_cols() == 0, "0 cols");
    TEST_ASSERT(mmap.nnz() == 0, "0 nnz");

    TEST_PASS("empty matrix save/load");
}

void test_single_row() {
    TempFile tmp("/tmp/gnfs_test_csr_1row.csrmat");

    SparseMatrix sparse(1, 10);
    sparse.row(0).set(3);
    sparse.row(0).set(7);
    CSRMatrix csr(sparse);

    MmapCSRMatrix::save(csr, tmp.path);
    MmapCSRMatrix mmap(tmp.path);

    TEST_ASSERT(mmap.num_rows() == 1, "1 row");
    TEST_ASSERT(mmap.row_nnz(0) == 2, "2 nnz");

    const uint32_t* begin = mmap.row_begin(0);
    // Columns should be sorted (3, 7) after CSR construction
    TEST_ASSERT(begin[0] == 3, "col 0 = 3");
    TEST_ASSERT(begin[1] == 7, "col 1 = 7");

    TEST_PASS("single row matrix [3, 7]");
}

int main() {
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  Mmap CSR Matrix Unit Tests\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    test_save_load_roundtrip();
    test_spmv_identical();
    test_large_matrix();
    test_empty_matrix();
    test_single_row();

    std::cout << "\n═══════════════════════════════════════════\n";
    std::cout << "  Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
    std::cout << "═══════════════════════════════════════════\n";

    return tests_failed > 0 ? 1 : 0;
}
