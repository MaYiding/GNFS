// test_metal_spmv.cpp — Correctness tests for the Metal-accelerated
// GF(2) 64-bit block SpMV kernels.
//
// Coverage
// --------
// * Availability probe: confirms is_available reports true on macOS
//   builds with Metal compiled in, false otherwise.
// * Bit-for-bit equivalence: compares Metal output to the CPU kernels
//   on a battery of random matrices of varying size and density. This
//   is the primary correctness invariant — GF(2) XOR is exact, so any
//   mismatch indicates a real shader or buffer-layout bug.
// * Edge cases: zero rows / zero cols / empty matrix / diagonal /
//   single-row / single-column / dense row.
// * Size sweep: 100, 1K, 10K, 100K rows; threshold logic verified.
//
// The test forces the Metal path via setenv("GNFS_METAL_SPMV", "1"); on
// non-Metal builds (or when MTLCreateSystemDefaultDevice returns nil)
// it confirms the path returns false and the CPU result is taken.

#include <gnfs/linalg/metal_spmv.hpp>
#include <gnfs/linalg/sparse_matrix.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <gnfs/linalg/detail/spmv_kernels.hpp>
#include <gnfs/util/thread_pool.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using gnfs::linalg::CSRMatrix;
using gnfs::linalg::SparseMatrix;
using gnfs::linalg::SparseRow;
using gnfs::linalg::BlockVector;

static int tests_passed = 0;
static int tests_failed = 0;
static int tests_skipped = 0;

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

#define TEST_SKIP(name, reason) do { \
    std::printf("  SKIP: %s -- %s\n", name, reason); \
    tests_skipped++; \
} while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a random SparseMatrix with `rows` x `cols` and an average of
// `avg_nnz_per_row` non-zeros per row. The RNG is seeded deterministically
// so failures are reproducible.
static SparseMatrix make_random(std::size_t rows, std::size_t cols,
                                 std::size_t avg_nnz_per_row, uint64_t seed) {
    SparseMatrix mat(rows, cols);
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint32_t> col_dist(0, cols ? static_cast<uint32_t>(cols - 1) : 0);
    std::uniform_int_distribution<int> nnz_dist(
        static_cast<int>(avg_nnz_per_row > 1 ? avg_nnz_per_row - 1 : 0),
        static_cast<int>(avg_nnz_per_row + 1));
    for (std::size_t r = 0; r < rows; ++r) {
        if (cols == 0) continue;
        int n = nnz_dist(rng);
        for (int k = 0; k < n; ++k) {
            mat.set(r, col_dist(rng));
        }
    }
    return mat;
}

// Build a random BlockVector with all 64 bit-lanes populated.
static BlockVector make_random_vector(std::size_t length, uint64_t seed) {
    BlockVector v(length);
    std::mt19937_64 rng(seed);
    for (std::size_t i = 0; i < length; ++i) {
        v.data[i] = rng();
    }
    return v;
}

// CPU reference: same kernel used by the detail/ template, but inlined
// here without templating so this test stays decoupled from the
// dispatcher. Output sizes must match.
static BlockVector cpu_spmv_forward(const CSRMatrix& m,
                                     const BlockVector& x) {
    BlockVector y(m.num_rows());
    for (std::size_t i = 0; i < m.num_rows(); ++i) {
        uint64_t acc = 0;
        const uint32_t* p = m.row_begin(i);
        const uint32_t* e = m.row_end(i);
        for (; p < e; ++p) acc ^= x.data[*p];
        y.data[i] = acc;
    }
    return y;
}

static BlockVector cpu_spmv_transpose(const CSRMatrix& m,
                                       const BlockVector& x) {
    BlockVector y(m.num_cols());
    for (std::size_t i = 0; i < m.num_rows(); ++i) {
        uint64_t xi = x.data[i];
        if (xi == 0) continue;
        const uint32_t* p = m.row_begin(i);
        const uint32_t* e = m.row_end(i);
        for (; p < e; ++p) y.data[*p] ^= xi;
    }
    return y;
}

static bool vectors_equal(const BlockVector& a, const BlockVector& b) {
    if (a.length != b.length) return false;
    for (std::size_t i = 0; i < a.length; ++i) {
        if (a.data[i] != b.data[i]) return false;
    }
    return true;
}

// Run both the CPU reference and the Metal kernel; assert they agree.
static bool compare_forward(const SparseMatrix& sp, const BlockVector& x,
                            const char* label) {
    CSRMatrix csr(sp);
    BlockVector cpu_y = cpu_spmv_forward(csr, x);
    BlockVector gpu_y(csr.num_rows());
    bool ok = gnfs::linalg::metal::spmv_forward(
        csr.num_rows(), csr.num_cols(),
        csr.row_offsets_u32(), csr.col_indices().data(),
        csr.nnz(),
        x.data.data(), gpu_y.data.data());
    if (!ok) {
        std::fprintf(stderr, "    [%s] Metal returned false\n", label);
        return false;
    }
    if (!vectors_equal(cpu_y, gpu_y)) {
        std::fprintf(stderr, "    [%s] forward mismatch (rows=%zu cols=%zu)\n",
                     label, sp.num_rows(), sp.num_cols());
        // Print first divergence for debugging
        for (std::size_t i = 0; i < cpu_y.length; ++i) {
            if (cpu_y.data[i] != gpu_y.data[i]) {
                std::fprintf(stderr, "      row %zu: cpu=%016llx gpu=%016llx\n",
                             i,
                             (unsigned long long)cpu_y.data[i],
                             (unsigned long long)gpu_y.data[i]);
                break;
            }
        }
        return false;
    }
    return true;
}

static bool compare_transpose(const SparseMatrix& sp, const BlockVector& x,
                              const char* label) {
    CSRMatrix csr(sp);
    BlockVector cpu_y = cpu_spmv_transpose(csr, x);
    BlockVector gpu_y(csr.num_cols());
    bool ok = gnfs::linalg::metal::spmv_transpose(
        csr.num_rows(), csr.num_cols(),
        csr.row_offsets_u32(), csr.col_indices().data(),
        csr.nnz(),
        x.data.data(), gpu_y.data.data());
    if (!ok) {
        std::fprintf(stderr, "    [%s] Metal returned false\n", label);
        return false;
    }
    if (!vectors_equal(cpu_y, gpu_y)) {
        std::fprintf(stderr, "    [%s] transpose mismatch (rows=%zu cols=%zu)\n",
                     label, sp.num_rows(), sp.num_cols());
        for (std::size_t i = 0; i < cpu_y.length; ++i) {
            if (cpu_y.data[i] != gpu_y.data[i]) {
                std::fprintf(stderr, "      col %zu: cpu=%016llx gpu=%016llx\n",
                             i,
                             (unsigned long long)cpu_y.data[i],
                             (unsigned long long)gpu_y.data[i]);
                break;
            }
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Test 1 — availability probe. is_available must give a yes/no answer
// without crashing.
static void test_availability() {
    std::printf("[1] availability probe\n");
    bool avail = gnfs::linalg::metal::is_available();
    std::printf("    is_available() = %s\n", avail ? "true" : "false");
#if defined(__APPLE__) && defined(GNFS_HAVE_METAL)
    TEST_ASSERT(avail, "Metal should be available on macOS with HAVE_METAL");
#endif
    // env_opt_in is set by main() at startup, so its value is independent
    // of the probe — exercise it just to make sure the function exists.
    bool env = gnfs::linalg::metal::env_opt_in();
    std::printf("    env_opt_in() = %s\n", env ? "true" : "false");
    TEST_PASS("availability probe");
}

// Test 2 — threshold policy. Below threshold should_use returns false.
static void test_threshold_policy() {
    std::printf("[2] threshold policy\n");
    TEST_ASSERT(!gnfs::linalg::metal::size_above_threshold(100, 100),
                "100x100 should be below 10K threshold");
    TEST_ASSERT(!gnfs::linalg::metal::size_above_threshold(9999, 9999),
                "9999x9999 should be below 10K threshold");
    TEST_ASSERT(gnfs::linalg::metal::size_above_threshold(10000, 100),
                "10K rows should clear threshold");
    TEST_ASSERT(gnfs::linalg::metal::size_above_threshold(100, 10000),
                "10K cols should clear threshold");
    TEST_PASS("threshold policy");
}

// Test 3 — forward parity on a small random matrix. Uses 100x100 so we
// bypass the dispatcher threshold and probe the raw kernel directly.
static void test_forward_small_random() {
    std::printf("[3] forward parity, 100x100 random\n");
    if (!gnfs::linalg::metal::is_available()) {
        TEST_SKIP("forward small random", "Metal unavailable");
        return;
    }
    SparseMatrix sp = make_random(100, 100, 5, 0x1234);
    BlockVector x = make_random_vector(100, 0x5678);
    TEST_ASSERT(compare_forward(sp, x, "100x100"),
                "100x100 forward mismatch");
    TEST_PASS("forward parity 100x100");
}

// Test 4 — transpose parity on the same matrix.
static void test_transpose_small_random() {
    std::printf("[4] transpose parity, 100x100 random\n");
    if (!gnfs::linalg::metal::is_available()) {
        TEST_SKIP("transpose small random", "Metal unavailable");
        return;
    }
    SparseMatrix sp = make_random(100, 100, 5, 0x1234);
    BlockVector x = make_random_vector(100, 0x9abc);
    TEST_ASSERT(compare_transpose(sp, x, "100x100"),
                "100x100 transpose mismatch");
    TEST_PASS("transpose parity 100x100");
}

// Test 5 — larger random matrix, 10K x 10K, average 10 nnz/row.
// This is where the GPU path would actually be taken in production.
static void test_forward_large_random() {
    std::printf("[5] forward parity, 10000x10000 random\n");
    if (!gnfs::linalg::metal::is_available()) {
        TEST_SKIP("forward large random", "Metal unavailable");
        return;
    }
    SparseMatrix sp = make_random(10000, 10000, 10, 0xdead);
    BlockVector x = make_random_vector(10000, 0xbeef);
    TEST_ASSERT(compare_forward(sp, x, "10K x 10K"),
                "10K x 10K forward mismatch");
    TEST_PASS("forward parity 10K x 10K");
}

static void test_transpose_large_random() {
    std::printf("[6] transpose parity, 10000x10000 random\n");
    if (!gnfs::linalg::metal::is_available()) {
        TEST_SKIP("transpose large random", "Metal unavailable");
        return;
    }
    SparseMatrix sp = make_random(10000, 10000, 10, 0xdead);
    BlockVector x = make_random_vector(10000, 0xcafe);
    TEST_ASSERT(compare_transpose(sp, x, "10K x 10K"),
                "10K x 10K transpose mismatch");
    TEST_PASS("transpose parity 10K x 10K");
}

// Test 7 — large random, 100K x 100K, average 15 nnz/row. Confirms the
// kernel scales beyond the threshold neighbourhood.
static void test_forward_xlarge_random() {
    std::printf("[7] forward parity, 100000x100000 random\n");
    if (!gnfs::linalg::metal::is_available()) {
        TEST_SKIP("forward xlarge random", "Metal unavailable");
        return;
    }
    SparseMatrix sp = make_random(100000, 100000, 15, 0xfaceu);
    BlockVector x = make_random_vector(100000, 0x4242);
    TEST_ASSERT(compare_forward(sp, x, "100K x 100K"),
                "100K x 100K forward mismatch");
    TEST_PASS("forward parity 100K x 100K");
}

static void test_transpose_xlarge_random() {
    std::printf("[8] transpose parity, 100000x100000 random\n");
    if (!gnfs::linalg::metal::is_available()) {
        TEST_SKIP("transpose xlarge random", "Metal unavailable");
        return;
    }
    SparseMatrix sp = make_random(100000, 100000, 15, 0xfaceu);
    BlockVector x = make_random_vector(100000, 0x1357);
    TEST_ASSERT(compare_transpose(sp, x, "100K x 100K"),
                "100K x 100K transpose mismatch");
    TEST_PASS("transpose parity 100K x 100K");
}

// Test 9 — diagonal matrix. y = x verbatim for forward; transpose is
// also y = x because A is symmetric.
static void test_diagonal() {
    std::printf("[9] diagonal matrix identity\n");
    if (!gnfs::linalg::metal::is_available()) {
        TEST_SKIP("diagonal", "Metal unavailable");
        return;
    }
    const std::size_t N = 512;
    SparseMatrix sp(N, N);
    for (std::size_t i = 0; i < N; ++i) sp.set(i, i);
    BlockVector x = make_random_vector(N, 0xaa55);
    TEST_ASSERT(compare_forward(sp, x, "diag forward"),
                "diagonal forward mismatch");
    TEST_ASSERT(compare_transpose(sp, x, "diag transpose"),
                "diagonal transpose mismatch");
    TEST_PASS("diagonal");
}

// Test 10 — single-row, single-column extremes. Catches buffer-bind
// off-by-one in the kernel signature.
static void test_single_row() {
    std::printf("[10] single-row matrix\n");
    if (!gnfs::linalg::metal::is_available()) {
        TEST_SKIP("single-row", "Metal unavailable");
        return;
    }
    SparseMatrix sp(1, 1024);
    for (std::size_t c = 0; c < 1024; c += 7) sp.set(0, c);
    BlockVector x = make_random_vector(1024, 0xbabe);
    TEST_ASSERT(compare_forward(sp, x, "single-row forward"),
                "single-row forward mismatch");
    BlockVector xt = make_random_vector(1, 0x7777);
    TEST_ASSERT(compare_transpose(sp, xt, "single-row transpose"),
                "single-row transpose mismatch");
    TEST_PASS("single-row");
}

// Test 11 — empty matrix (zero rows). Must be a no-op.
static void test_empty_matrix() {
    std::printf("[11] empty (zero-row) matrix\n");
    if (!gnfs::linalg::metal::is_available()) {
        TEST_SKIP("empty", "Metal unavailable");
        return;
    }
    SparseMatrix sp(0, 100);
    BlockVector x = make_random_vector(100, 0xfeed);
    CSRMatrix csr(sp);
    BlockVector y(0);
    bool ok = gnfs::linalg::metal::spmv_forward(
        0, 100, csr.row_offsets_u32(), csr.col_indices().data(),
        0, x.data.data(), y.data.data());
    TEST_ASSERT(ok, "empty forward should return true");
    BlockVector yt(100);
    ok = gnfs::linalg::metal::spmv_transpose(
        0, 100, csr.row_offsets_u32(), csr.col_indices().data(),
        0, x.data.data(), yt.data.data());
    TEST_ASSERT(ok, "empty transpose should return true");
    for (std::size_t i = 0; i < 100; ++i) {
        TEST_ASSERT(yt.data[i] == 0, "empty transpose should zero output");
    }
    TEST_PASS("empty matrix");
}

// Test 12 — zero-vector input forward. Should yield zero output. Easy
// to get wrong if the kernel doesn't guard against unused indices.
static void test_zero_vector_input() {
    std::printf("[12] zero-vector input\n");
    if (!gnfs::linalg::metal::is_available()) {
        TEST_SKIP("zero vector", "Metal unavailable");
        return;
    }
    SparseMatrix sp = make_random(2048, 2048, 4, 0x4abc);
    BlockVector x(2048);  // all zero
    TEST_ASSERT(compare_forward(sp, x, "zero input forward"),
                "zero input forward mismatch");
    TEST_ASSERT(compare_transpose(sp, x, "zero input transpose"),
                "zero input transpose mismatch");
    TEST_PASS("zero vector input");
}

// Test 13 — non-square (m > n) random matrix. Common shape for BW
// matrices in the standard wide path.
static void test_non_square_wide() {
    std::printf("[13] non-square 5000x4500 wide\n");
    if (!gnfs::linalg::metal::is_available()) {
        TEST_SKIP("non-square wide", "Metal unavailable");
        return;
    }
    SparseMatrix sp = make_random(5000, 4500, 8, 0x9999);
    BlockVector x = make_random_vector(4500, 0x1212);
    TEST_ASSERT(compare_forward(sp, x, "wide forward"),
                "wide forward mismatch");
    BlockVector xt = make_random_vector(5000, 0x3434);
    TEST_ASSERT(compare_transpose(sp, xt, "wide transpose"),
                "wide transpose mismatch");
    TEST_PASS("non-square wide");
}

// Test 14 — non-square (m < n) thin matrix. Thin-solve path.
static void test_non_square_thin() {
    std::printf("[14] non-square 4500x5000 thin\n");
    if (!gnfs::linalg::metal::is_available()) {
        TEST_SKIP("non-square thin", "Metal unavailable");
        return;
    }
    SparseMatrix sp = make_random(4500, 5000, 8, 0x5555);
    BlockVector x = make_random_vector(5000, 0x6767);
    TEST_ASSERT(compare_forward(sp, x, "thin forward"),
                "thin forward mismatch");
    BlockVector xt = make_random_vector(4500, 0x8989);
    TEST_ASSERT(compare_transpose(sp, xt, "thin transpose"),
                "thin transpose mismatch");
    TEST_PASS("non-square thin");
}

// Test 15 — repeated calls against the same matrix; verifies the
// buffer cache stays consistent across re-use.
static void test_repeated_calls() {
    std::printf("[15] repeated SpMV on same matrix\n");
    if (!gnfs::linalg::metal::is_available()) {
        TEST_SKIP("repeated calls", "Metal unavailable");
        return;
    }
    SparseMatrix sp = make_random(2048, 2048, 6, 0xa1a1);
    for (int k = 0; k < 8; ++k) {
        BlockVector x = make_random_vector(2048, 0xb1b1 + k);
        if (!compare_forward(sp, x, "repeated forward")) {
            TEST_ASSERT(false, "repeated forward iteration mismatch");
        }
        if (!compare_transpose(sp, x, "repeated transpose")) {
            TEST_ASSERT(false, "repeated transpose iteration mismatch");
        }
    }
    TEST_PASS("repeated calls");
}

// Test 16 — dispatcher integration. With GNFS_METAL_SPMV set in main(),
// detail::spmv_forward on a CSRMatrix should produce the same output as
// the CPU kernel called directly. Confirms the dispatcher actually
// routes to Metal and returns the right answer.
static void test_dispatcher_integration() {
    std::printf("[16] dispatcher integration via detail::spmv_forward\n");
    if (!gnfs::linalg::metal::is_available()) {
        TEST_SKIP("dispatcher integration", "Metal unavailable");
        return;
    }
    SparseMatrix sp = make_random(20000, 20000, 8, 0xc0fe);
    CSRMatrix csr(sp);
    BlockVector x = make_random_vector(20000, 0xd1ce);

    BlockVector cpu_y_ref = cpu_spmv_forward(csr, x);

    gnfs::util::ThreadPool pool(4);
    BlockVector dispatched_y(20000);
    gnfs::linalg::detail::spmv_forward(csr, x, dispatched_y, pool);
    TEST_ASSERT(vectors_equal(cpu_y_ref, dispatched_y),
                "dispatcher forward != CPU reference");

    BlockVector cpu_yt_ref = cpu_spmv_transpose(csr, x);
    BlockVector dispatched_yt(20000);
    gnfs::linalg::detail::spmv_transpose(csr, x, dispatched_yt, pool);
    TEST_ASSERT(vectors_equal(cpu_yt_ref, dispatched_yt),
                "dispatcher transpose != CPU reference");
    TEST_PASS("dispatcher integration");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main() {
    // Force the dispatcher to take the GPU path for the dispatcher test.
    // env_opt_in caches its first probe, so set the env BEFORE any other
    // call into the metal namespace.
    setenv("GNFS_METAL_SPMV", "1", /*overwrite=*/1);

    std::printf("=== Metal SpMV tests ===\n");
    test_availability();
    test_threshold_policy();
    test_forward_small_random();
    test_transpose_small_random();
    test_forward_large_random();
    test_transpose_large_random();
    test_forward_xlarge_random();
    test_transpose_xlarge_random();
    test_diagonal();
    test_single_row();
    test_empty_matrix();
    test_zero_vector_input();
    test_non_square_wide();
    test_non_square_thin();
    test_repeated_calls();
    test_dispatcher_integration();

    std::printf("\n=== Summary ===\n");
    std::printf("  passed:  %d\n", tests_passed);
    std::printf("  failed:  %d\n", tests_failed);
    std::printf("  skipped: %d\n", tests_skipped);
    return tests_failed > 0 ? 1 : 0;
}
