// test_save_sparse_as_mmap.cpp — Verifies the SparseMatrix → MmapCSRMatrix
// convenience helper produces a result byte-identical to the explicit
// SparseMatrix → CSRMatrix → save → MmapCSRMatrix round-trip used in
// existing tests. This is the Phase 2 "dual-path build" guarantee: an
// in-memory path and an OOC path land on the same matrix.

#include <gnfs/linalg/mmap_csr_matrix.hpp>
#include <gnfs/linalg/sparse_matrix.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

using gnfs::linalg::BlockVector;
using gnfs::linalg::CSRMatrix;
using gnfs::linalg::MmapCSRMatrix;
using gnfs::linalg::SparseMatrix;
using gnfs::linalg::save_sparse_as_mmap;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, msg); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define TEST_PASS(name) do { \
    std::printf("  PASS: %s\n", name); \
    tests_passed++; \
} while (0)

struct TempFile {
    std::string path;
    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile() { std::remove(path.c_str()); }
};

static SparseMatrix make_random_matrix(std::size_t rows, std::size_t cols,
                                       std::uint32_t seed, std::size_t nnz_per_row = 5) {
    SparseMatrix M(rows, cols);
    std::mt19937 rng(seed);
    for (std::size_t i = 0; i < rows; ++i) {
        for (std::size_t k = 0; k < nnz_per_row; ++k) {
            M.row(i).set(static_cast<std::uint32_t>(rng() % cols));
        }
    }
    return M;
}

void test_helper_bitwise_identical_to_explicit_path() {
    TempFile a("/tmp/gnfs_test_save_sparse_helper_a.csrmat");
    TempFile b("/tmp/gnfs_test_save_sparse_helper_b.csrmat");

    SparseMatrix sparse = make_random_matrix(2000, 1500, 9999, 8);

    // Path A: explicit (today's idiom in test_mmap_csr.cpp).
    {
        CSRMatrix csr(sparse);
        MmapCSRMatrix::save(csr, a.path);
    }
    MmapCSRMatrix a_mmap(a.path);

    // Path B: helper.
    MmapCSRMatrix b_mmap = save_sparse_as_mmap(sparse, b.path);

    TEST_ASSERT(a_mmap.num_rows() == b_mmap.num_rows(), "row count parity");
    TEST_ASSERT(a_mmap.num_cols() == b_mmap.num_cols(), "col count parity");
    TEST_ASSERT(a_mmap.nnz()      == b_mmap.nnz(),      "nnz parity");

    for (std::size_t i = 0; i < a_mmap.num_rows(); ++i) {
        TEST_ASSERT(a_mmap.row_nnz(i) == b_mmap.row_nnz(i),
                    "row_nnz parity per row");
        const std::uint32_t* pa = a_mmap.row_begin(i);
        const std::uint32_t* pb = b_mmap.row_begin(i);
        for (std::size_t k = 0; k < a_mmap.row_nnz(i); ++k) {
            TEST_ASSERT(pa[k] == pb[k], "row col index parity");
        }
    }

    TEST_PASS("save_sparse_as_mmap helper byte-identical to explicit round-trip");
}

template <typename M>
static void spmv_forward_simple(const M& mat, const BlockVector& x, BlockVector& y) {
    for (std::size_t i = 0; i < mat.num_rows(); ++i) {
        std::uint64_t acc = 0;
        for (const std::uint32_t* p = mat.row_begin(i); p != mat.row_end(i); ++p) {
            acc ^= x.data[*p];
        }
        y.data[i] = acc;
    }
}

void test_helper_spmv_matches_in_memory() {
    TempFile tmp("/tmp/gnfs_test_save_sparse_helper_spmv.csrmat");

    SparseMatrix sparse = make_random_matrix(3000, 2000, 11111, 10);
    CSRMatrix csr(sparse);

    // Random input vector.
    BlockVector x(csr.num_cols());
    std::mt19937_64 rng(8675309);
    for (std::size_t i = 0; i < x.length; ++i) x.data[i] = rng();

    BlockVector y_csr(csr.num_rows()), y_mmap(csr.num_rows());
    spmv_forward_simple(csr, x, y_csr);

    MmapCSRMatrix mmap = save_sparse_as_mmap(sparse, tmp.path);
    spmv_forward_simple(mmap, x, y_mmap);

    for (std::size_t i = 0; i < csr.num_rows(); ++i) {
        TEST_ASSERT(y_csr.data[i] == y_mmap.data[i], "SpMV row result parity");
    }

    TEST_PASS("save_sparse_as_mmap helper SpMV matches in-memory CSR");
}

void test_helper_empty_matrix() {
    TempFile tmp("/tmp/gnfs_test_save_sparse_helper_empty.csrmat");
    SparseMatrix empty(0, 0);
    MmapCSRMatrix m = save_sparse_as_mmap(empty, tmp.path);
    TEST_ASSERT(m.num_rows() == 0, "empty rows");
    TEST_ASSERT(m.num_cols() == 0, "empty cols");
    TEST_ASSERT(m.nnz() == 0, "empty nnz");
    TEST_PASS("save_sparse_as_mmap helper handles empty matrix");
}

int main() {
    std::printf("== save_sparse_as_mmap helper tests ==\n");
    test_helper_bitwise_identical_to_explicit_path();
    test_helper_spmv_matches_in_memory();
    test_helper_empty_matrix();
    std::printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
