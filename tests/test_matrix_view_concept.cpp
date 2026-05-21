// test_matrix_view_concept.cpp — Compile-time checks that CSRMatrix and
// MmapCSRMatrix satisfy the MatrixView concept and that the conformance
// can be exercised through a templated function.
//
// Catches accidental signature drift (e.g. someone renames row_begin or
// changes the return type) at compile time rather than at the Phase 5 SpMV
// hot loop where the failure mode is silent inlining loss.

#include <gnfs/linalg/matrix_view.hpp>
#include <gnfs/linalg/sparse_matrix.hpp>
#include <gnfs/linalg/mmap_csr_matrix.hpp>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

using gnfs::linalg::CSRMatrix;
using gnfs::linalg::MmapCSRMatrix;
using gnfs::linalg::MatrixView;
using gnfs::linalg::SparseMatrix;

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

// Templated SpMV-style row scan that compiles only if the matrix
// type satisfies MatrixView. Returns total nnz seen.
template <MatrixView M>
static std::size_t scan_all_rows(const M& matrix) {
    std::size_t total = 0;
    for (std::size_t i = 0; i < matrix.num_rows(); ++i) {
        const std::uint32_t* p = matrix.row_begin(i);
        const std::uint32_t* e = matrix.row_end(i);
        total += static_cast<std::size_t>(e - p);
        // Also exercise row_nnz to confirm parity.
        if (matrix.row_nnz(i) != static_cast<std::size_t>(e - p)) {
            return SIZE_MAX;
        }
    }
    return total;
}

struct TempFile {
    std::string path;
    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile() { std::remove(path.c_str()); }
};

static SparseMatrix make_random_matrix(std::size_t rows, std::size_t cols, std::uint32_t seed) {
    SparseMatrix M(rows, cols);
    std::mt19937 rng(seed);
    for (std::size_t i = 0; i < rows; ++i) {
        for (int k = 0; k < 5; ++k) {
            M.row(i).set(static_cast<std::uint32_t>(rng() % cols));
        }
    }
    return M;
}

void test_concept_compile_time() {
    static_assert(MatrixView<CSRMatrix>, "CSRMatrix breaks MatrixView contract");
    static_assert(MatrixView<MmapCSRMatrix>, "MmapCSRMatrix breaks MatrixView contract");
    TEST_PASS("concept satisfied at compile time (CSRMatrix, MmapCSRMatrix)");
}

void test_templated_call_csr() {
    SparseMatrix sparse = make_random_matrix(64, 32, 1234);
    CSRMatrix csr(sparse);
    std::size_t total = scan_all_rows(csr);
    TEST_ASSERT(total == csr.nnz(), "CSR scan_all_rows total != nnz");
    TEST_PASS("templated MatrixView<CSRMatrix> scan returns correct nnz");
}

void test_templated_call_mmap() {
    TempFile tmp("/tmp/gnfs_test_matrix_view_concept.csrmat");
    SparseMatrix sparse = make_random_matrix(80, 40, 4321);
    CSRMatrix csr(sparse);
    MmapCSRMatrix::save(csr, tmp.path);
    MmapCSRMatrix mmap(tmp.path);
    std::size_t total = scan_all_rows(mmap);
    TEST_ASSERT(total == mmap.nnz(), "Mmap scan_all_rows total != nnz");
    TEST_ASSERT(total == csr.nnz(), "Mmap nnz differs from in-memory CSR");
    TEST_PASS("templated MatrixView<MmapCSRMatrix> scan returns correct nnz");
}

// Negative check: SparseMatrix is intentionally NOT a MatrixView (it's a
// random-access GF(2) bitset, not a CSR row stream). Compile-time guard.
void test_sparse_matrix_not_a_view() {
    static_assert(!MatrixView<SparseMatrix>,
                  "SparseMatrix should NOT satisfy MatrixView (it has no row_begin/row_end)");
    TEST_PASS("SparseMatrix correctly rejected by concept");
}

int main() {
    std::printf("== matrix_view concept tests ==\n");
    test_concept_compile_time();
    test_templated_call_csr();
    test_templated_call_mmap();
    test_sparse_matrix_not_a_view();
    std::printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
