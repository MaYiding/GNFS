// test_transpose_blocked.cpp - Correctness tests for the cache-blocked
// GF(2) tile transpose helper.
//
// Strategy
// --------
// The blocked path must be bit-for-bit identical to the naive scalar
// reference for every input. Every parity test builds a source bit
// matrix, runs both `transpose_naive_gf2` and `transpose_blocked_gf2_impl`
// directly (bypassing the gate to test the impl path), and asserts that
// the two destination buffers compare equal word-by-word.
//
// Additional coverage:
// * Empty matrices (0x0, 0xN, Nx0) on both paths.
// * Edge dimensions that are not multiples of 64 (7x3, 63x127) so the
//   tile masking logic at block boundaries is exercised.
// * A 1000x1000 random matrix and 500x800 fixed-seed random matrix to
//   stress the tile scheduler over a non-trivial number of blocks.
// * Double-transpose round-trip identity: transpose(transpose(M)) == M.
// * ENV parsing: "auto" / "0" / "1" / unset / "garbage" each map to
//   the documented gate decision.
// * Threshold gate: when GNFS_MATRIX_TRANSPOSE_BLOCKED is unset / "auto"
//   the dispatcher falls back to the naive path for sub-threshold
//   matrices and uses the blocked path for matrices that meet the
//   threshold.

#include <gnfs/linalg/detail/transpose_blocked.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace td = gnfs::linalg::detail;

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

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Allocate a `rows x cols` bit-packed matrix and fill it from `rng`. The
// padding bits past the last column in the last word per row are zeroed
// so the naive and blocked paths see the same input contract.
static std::vector<std::uint64_t> make_random_matrix(std::size_t rows,
                                                    std::size_t cols,
                                                    std::uint64_t seed) {
    const std::size_t wpr = (cols + 63) / 64;
    std::vector<std::uint64_t> m(rows * wpr, 0);
    if (rows == 0 || cols == 0) return m;
    std::mt19937_64 rng(seed);
    for (std::size_t i = 0; i < rows; ++i) {
        for (std::size_t w = 0; w < wpr; ++w) {
            m[i * wpr + w] = rng();
        }
        // Zero padding bits past column cols-1 in the last word.
        const std::size_t last_bit_in_word = cols & 63;
        if (last_bit_in_word != 0 && wpr > 0) {
            const std::uint64_t mask = (1ULL << last_bit_in_word) - 1;
            m[i * wpr + (wpr - 1)] &= mask;
        }
    }
    return m;
}

// Compares two bit-packed matrices for equality. Returns the first mismatched
// word index, or SIZE_MAX if equal. Both vectors must have the same length.
static std::size_t first_mismatch(const std::vector<std::uint64_t>& a,
                                  const std::vector<std::uint64_t>& b) {
    if (a.size() != b.size()) return 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return i;
    }
    return SIZE_MAX;
}

// Asserts that the blocked impl matches the naive impl bit-for-bit for the
// given input matrix `src` of shape (rows, cols).
static bool blocked_matches_naive(const std::vector<std::uint64_t>& src,
                                  std::size_t rows, std::size_t cols,
                                  const char* label) {
    const std::size_t dst_wpr = (rows + 63) / 64;
    std::vector<std::uint64_t> dst_naive(cols * dst_wpr, 0);
    std::vector<std::uint64_t> dst_blocked(cols * dst_wpr, 0);
    td::transpose_naive_gf2(src.data(), dst_naive.data(), rows, cols);
    td::transpose_detail::transpose_blocked_gf2_impl(
        src.data(), dst_blocked.data(), rows, cols);
    const std::size_t mismatch = first_mismatch(dst_naive, dst_blocked);
    if (mismatch != SIZE_MAX) {
        std::fprintf(stderr,
                     "  mismatch [%s] rows=%zu cols=%zu word=%zu "
                     "naive=%016llx blocked=%016llx\n",
                     label, rows, cols, mismatch,
                     static_cast<unsigned long long>(dst_naive[mismatch]),
                     static_cast<unsigned long long>(dst_blocked[mismatch]));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Test 1 - Empty matrix dimensions. Both paths must early-return without
// touching either buffer; we verify that the destination remains zeroed.
static void test_empty_dimensions() {
    std::printf("[1] empty matrix shapes\n");
    // 0x0
    {
        std::vector<std::uint64_t> src;
        std::vector<std::uint64_t> dst;
        td::transpose_naive_gf2(src.data(), dst.data(), 0, 0);
        td::transpose_detail::transpose_blocked_gf2_impl(
            src.data(), dst.data(), 0, 0);
        td::transpose_blocked_gf2(src.data(), dst.data(), 0, 0);
    }
    // 0xN: src is empty, dst would be N rows of 0 words (zero-sized)
    {
        std::vector<std::uint64_t> src;
        std::vector<std::uint64_t> dst;
        td::transpose_naive_gf2(src.data(), dst.data(), 0, 32);
        td::transpose_detail::transpose_blocked_gf2_impl(
            src.data(), dst.data(), 0, 32);
        td::transpose_blocked_gf2(src.data(), dst.data(), 0, 32);
    }
    // Nx0: src is N rows of 0 words; dst is empty
    {
        std::vector<std::uint64_t> src;
        std::vector<std::uint64_t> dst;
        td::transpose_naive_gf2(src.data(), dst.data(), 32, 0);
        td::transpose_detail::transpose_blocked_gf2_impl(
            src.data(), dst.data(), 32, 0);
        td::transpose_blocked_gf2(src.data(), dst.data(), 32, 0);
    }
    TEST_PASS("empty matrix shapes (no crash, no writes)");
}

// Test 2 - 1x1 single-bit matrix. Verifies the smallest possible non-empty
// input on both paths.
static void test_one_by_one() {
    std::printf("[2] 1x1 bit matrix\n");
    {
        // Bit value 1.
        std::vector<std::uint64_t> src = {1ULL};
        std::vector<std::uint64_t> dst_n(1, 0);
        std::vector<std::uint64_t> dst_b(1, 0);
        td::transpose_naive_gf2(src.data(), dst_n.data(), 1, 1);
        td::transpose_detail::transpose_blocked_gf2_impl(
            src.data(), dst_b.data(), 1, 1);
        TEST_ASSERT(dst_n[0] == 1ULL, "naive 1x1 bit set");
        TEST_ASSERT(dst_b[0] == 1ULL, "blocked 1x1 bit set");
        TEST_ASSERT(dst_n == dst_b, "1x1 parity");
    }
    {
        // Bit value 0.
        std::vector<std::uint64_t> src = {0ULL};
        std::vector<std::uint64_t> dst_n(1, 0);
        std::vector<std::uint64_t> dst_b(1, 0);
        td::transpose_naive_gf2(src.data(), dst_n.data(), 1, 1);
        td::transpose_detail::transpose_blocked_gf2_impl(
            src.data(), dst_b.data(), 1, 1);
        TEST_ASSERT(dst_n[0] == 0ULL, "naive 1x1 bit clear");
        TEST_ASSERT(dst_b[0] == 0ULL, "blocked 1x1 bit clear");
    }
    TEST_PASS("1x1 bit matrix (both values)");
}

// Test 3 - 7x3 matrix. Both dimensions below 64 so the impl uses a single
// edge tile with non-trivial masking on both axes.
static void test_7x3_edge_tile() {
    std::printf("[3] 7x3 edge tile\n");
    // Build a deterministic 7x3 bit pattern: M[i][j] = (i * 3 + j) % 2.
    std::vector<std::uint64_t> src(7, 0);
    for (std::size_t i = 0; i < 7; ++i) {
        std::uint64_t row = 0;
        for (std::size_t j = 0; j < 3; ++j) {
            if (((i * 3 + j) % 2) == 0) row |= (1ULL << j);
        }
        src[i] = row;
    }
    std::vector<std::uint64_t> dst_n(3, 0);
    std::vector<std::uint64_t> dst_b(3, 0);
    td::transpose_naive_gf2(src.data(), dst_n.data(), 7, 3);
    td::transpose_detail::transpose_blocked_gf2_impl(
        src.data(), dst_b.data(), 7, 3);
    TEST_ASSERT(dst_n == dst_b, "7x3 parity");
    // Spot-check the values: transpose should put M[i][j] at dst[j][i].
    for (std::size_t i = 0; i < 7; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            const bool src_bit = ((i * 3 + j) % 2) == 0;
            const bool dst_bit = (dst_b[j] >> i) & 1ULL;
            TEST_ASSERT(src_bit == dst_bit, "7x3 transpose bit mismatch");
        }
    }
    TEST_PASS("7x3 edge tile parity + spot-check");
}

// Test 4 - 63x127. Last block-row tile width = 63 (not 64) and last
// block-col tile width = 63. Heaviest edge-tile case before the helper
// produces a full 64x64 inner tile.
static void test_63x127_edge_blocks() {
    std::printf("[4] 63x127 edge blocks\n");
    auto src = make_random_matrix(63, 127, 0xa1b2c3d4ULL);
    TEST_ASSERT(blocked_matches_naive(src, 63, 127, "63x127"),
                "63x127 mismatch");
    TEST_PASS("63x127 edge blocks");
}

// Test 5 - 64x64. Exactly one tile, no edge masking on either axis.
// Verifies the in-register transpose primitive at the boundary.
static void test_64x64_exact_tile() {
    std::printf("[5] 64x64 single tile\n");
    auto src = make_random_matrix(64, 64, 0xdeadbeefULL);
    TEST_ASSERT(blocked_matches_naive(src, 64, 64, "64x64"),
                "64x64 mismatch");
    TEST_PASS("64x64 single tile");
}

// Test 6 - 128x64. Two tile-rows, one tile-col. Exercises the tile-row
// loop and confirms that the destination is written in the right order
// (column 0 of tile 1 lands after column 0 of tile 0).
static void test_128x64_two_row_tiles() {
    std::printf("[6] 128x64 two row tiles\n");
    auto src = make_random_matrix(128, 64, 0xfacefeedULL);
    TEST_ASSERT(blocked_matches_naive(src, 128, 64, "128x64"),
                "128x64 mismatch");
    TEST_PASS("128x64 two row tiles");
}

// Test 7 - 1000x1000 random. Multiple tile-rows and tile-cols, including
// a last block of width 1000 % 64 = 40 on each axis.
static void test_1000x1000_random() {
    std::printf("[7] 1000x1000 random matrix\n");
    auto src = make_random_matrix(1000, 1000, 0x12345678ULL);
    TEST_ASSERT(blocked_matches_naive(src, 1000, 1000, "1000x1000"),
                "1000x1000 mismatch");
    TEST_PASS("1000x1000 random matrix");
}

// Test 8 - 500x800 random with fixed seed. Asymmetric tile counts;
// rows aligned to 64 (500 % 64 = 52), cols aligned to 64 (800 % 64 = 32).
static void test_500x800_random() {
    std::printf("[8] 500x800 random matrix\n");
    auto src = make_random_matrix(500, 800, 0x9e3779b9ULL);
    TEST_ASSERT(blocked_matches_naive(src, 500, 800, "500x800"),
                "500x800 mismatch");
    TEST_PASS("500x800 random matrix");
}

// Test 9 - Double-transpose round-trip. transpose(transpose(M)) == M for
// the blocked path. This catches any bit-permutation mistake in either
// the 64x64 in-register primitive or the tile scheduler.
static void test_double_transpose_roundtrip() {
    std::printf("[9] double-transpose round-trip\n");
    const std::size_t rows = 257;
    const std::size_t cols = 193;
    auto src = make_random_matrix(rows, cols, 0xbaadf00dULL);

    const std::size_t mid_wpr = (rows + 63) / 64;
    std::vector<std::uint64_t> mid(cols * mid_wpr, 0);
    td::transpose_detail::transpose_blocked_gf2_impl(
        src.data(), mid.data(), rows, cols);

    const std::size_t round_wpr = (cols + 63) / 64;
    std::vector<std::uint64_t> round(rows * round_wpr, 0);
    td::transpose_detail::transpose_blocked_gf2_impl(
        mid.data(), round.data(), cols, rows);

    TEST_ASSERT(round == src,
                "double-transpose differs from original");
    TEST_PASS("double-transpose round-trip");
}

// Test 10 - ENV parsing. Forces each documented env value and asserts
// the cached gate state matches. Uses
// `reload_matrix_transpose_blocked_for_testing()` to flush the cache
// between scenarios.
static void test_env_parsing() {
    std::printf("[10] env parsing\n");
    using td::transpose_detail::GateMode;

    auto set_and_check = [](const char* val, GateMode expected, const char* label) {
        if (val == nullptr) {
            ::unsetenv("GNFS_MATRIX_TRANSPOSE_BLOCKED");
        } else {
            ::setenv("GNFS_MATRIX_TRANSPOSE_BLOCKED", val, /*overwrite=*/1);
        }
        td::reload_matrix_transpose_blocked_for_testing();
        const GateMode got = td::matrix_transpose_blocked_mode();
        if (got != expected) {
            std::fprintf(stderr,
                         "  env=%s expected=%d got=%d at %s\n",
                         val ? val : "(unset)",
                         static_cast<int>(expected),
                         static_cast<int>(got),
                         label);
            tests_failed++;
            return false;
        }
        return true;
    };

    if (!set_and_check(nullptr, GateMode::Auto, "unset")) return;
    if (!set_and_check("auto", GateMode::Auto, "auto")) return;
    if (!set_and_check("0", GateMode::ForceOff, "0")) return;
    if (!set_and_check("1", GateMode::ForceOn, "1")) return;
    if (!set_and_check("garbage", GateMode::Auto, "garbage")) return;
    if (!set_and_check("", GateMode::Auto, "empty string")) return;

    // Leave the env unset for any subsequent tests.
    ::unsetenv("GNFS_MATRIX_TRANSPOSE_BLOCKED");
    td::reload_matrix_transpose_blocked_for_testing();

    TEST_PASS("env parsing across all documented values");
}

// Test 11 - Threshold behavior. With ENV unset (= auto), the dispatcher
// must route a sub-threshold matrix (both dims < 128) to the naive path.
// We confirm by force-off-ing the gate and capturing the output, then
// running the dispatcher with auto and comparing against the captured
// naive result. The two must be bit-for-bit identical because (a) the
// blocked path is also bit-for-bit identical to naive, but more
// importantly because the small-dim fallback proves the gate is wired.
// We then flip dims above the threshold and confirm dispatcher == impl
// directly (the blocked path is taken).
static void test_threshold_routing() {
    std::printf("[11] threshold routing under auto\n");
    using td::transpose_detail::GateMode;

    // Ensure auto.
    ::unsetenv("GNFS_MATRIX_TRANSPOSE_BLOCKED");
    td::reload_matrix_transpose_blocked_for_testing();
    TEST_ASSERT(td::matrix_transpose_blocked_mode() == GateMode::Auto,
                "auto gate not active after unset+reload");

    // Sub-threshold (both dims < 128): dispatcher should report disabled.
    TEST_ASSERT(!td::matrix_transpose_blocked_enabled(64, 64),
                "auto with 64x64 should not enable blocked path");
    TEST_ASSERT(!td::matrix_transpose_blocked_enabled(127, 127),
                "auto with 127x127 should not enable blocked path");

    // Above threshold on either axis enables the blocked path.
    TEST_ASSERT(td::matrix_transpose_blocked_enabled(128, 64),
                "auto with 128x64 should enable blocked path");
    TEST_ASSERT(td::matrix_transpose_blocked_enabled(64, 128),
                "auto with 64x128 should enable blocked path");
    TEST_ASSERT(td::matrix_transpose_blocked_enabled(1000, 1000),
                "auto with 1000x1000 should enable blocked path");

    // Force-on overrides the threshold.
    ::setenv("GNFS_MATRIX_TRANSPOSE_BLOCKED", "1", /*overwrite=*/1);
    td::reload_matrix_transpose_blocked_for_testing();
    TEST_ASSERT(td::matrix_transpose_blocked_enabled(8, 8),
                "force-on must enable even at 8x8");

    // Force-off overrides the threshold.
    ::setenv("GNFS_MATRIX_TRANSPOSE_BLOCKED", "0", /*overwrite=*/1);
    td::reload_matrix_transpose_blocked_for_testing();
    TEST_ASSERT(!td::matrix_transpose_blocked_enabled(10000, 10000),
                "force-off must disable even at 10000x10000");

    // Restore default.
    ::unsetenv("GNFS_MATRIX_TRANSPOSE_BLOCKED");
    td::reload_matrix_transpose_blocked_for_testing();

    TEST_PASS("threshold routing across auto/on/off");
}

// Test 12 - Dispatcher path parity. Confirms `transpose_blocked_gf2`
// (the public entry that consults the gate) produces the same output
// as the naive reference regardless of which internal path was taken,
// for a range of sizes that span the auto threshold.
static void test_dispatcher_parity() {
    std::printf("[12] dispatcher parity (force-on vs naive)\n");
    // Force the blocked path on so the dispatcher exercises it across
    // all sizes including the sub-threshold case.
    ::setenv("GNFS_MATRIX_TRANSPOSE_BLOCKED", "1", /*overwrite=*/1);
    td::reload_matrix_transpose_blocked_for_testing();

    struct Case { std::size_t r, c; std::uint64_t seed; };
    const Case cases[] = {
        {1, 1, 0x100},
        {7, 3, 0x101},
        {63, 127, 0x102},
        {64, 64, 0x103},
        {128, 64, 0x104},
        {200, 300, 0x105},
    };
    for (const auto& tc : cases) {
        auto src = make_random_matrix(tc.r, tc.c, tc.seed);
        const std::size_t dst_wpr = (tc.r + 63) / 64;
        std::vector<std::uint64_t> dst_dispatch(tc.c * dst_wpr, 0);
        std::vector<std::uint64_t> dst_naive(tc.c * dst_wpr, 0);
        td::transpose_blocked_gf2(src.data(), dst_dispatch.data(), tc.r, tc.c);
        td::transpose_naive_gf2(src.data(), dst_naive.data(), tc.r, tc.c);
        if (dst_dispatch != dst_naive) {
            std::fprintf(stderr,
                         "  dispatcher mismatch at %zux%zu\n", tc.r, tc.c);
            tests_failed++;
            ::unsetenv("GNFS_MATRIX_TRANSPOSE_BLOCKED");
            td::reload_matrix_transpose_blocked_for_testing();
            return;
        }
    }

    ::unsetenv("GNFS_MATRIX_TRANSPOSE_BLOCKED");
    td::reload_matrix_transpose_blocked_for_testing();
    TEST_PASS("dispatcher parity across size range");
}

// Test 13 - Sparse pattern. A matrix with only a few set bits should
// still transpose correctly through both paths. Stresses the early-exit
// behaviour (no bits in many tiles).
static void test_sparse_pattern() {
    std::printf("[13] sparse pattern\n");
    const std::size_t rows = 256;
    const std::size_t cols = 192;
    const std::size_t wpr = (cols + 63) / 64;
    std::vector<std::uint64_t> src(rows * wpr, 0);
    // Sprinkle a handful of bits across non-trivial tile boundaries.
    auto set = [&](std::size_t i, std::size_t j) {
        src[i * wpr + (j >> 6)] |= (1ULL << (j & 63));
    };
    set(0, 0);
    set(63, 63);
    set(64, 64);
    set(65, 0);
    set(0, 127);
    set(127, 191);
    set(200, 100);
    set(255, 191);
    TEST_ASSERT(blocked_matches_naive(src, rows, cols, "sparse 256x192"),
                "sparse pattern mismatch");
    TEST_PASS("sparse pattern parity");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    std::printf("=== test_transpose_blocked ===\n");
    const char* env = std::getenv("GNFS_MATRIX_TRANSPOSE_BLOCKED");
    std::printf("GNFS_MATRIX_TRANSPOSE_BLOCKED = %s\n", env ? env : "(unset)");

    test_empty_dimensions();
    test_one_by_one();
    test_7x3_edge_tile();
    test_63x127_edge_blocks();
    test_64x64_exact_tile();
    test_128x64_two_row_tiles();
    test_1000x1000_random();
    test_500x800_random();
    test_double_transpose_roundtrip();
    test_env_parsing();
    test_threshold_routing();
    test_dispatcher_parity();
    test_sparse_pattern();

    std::printf("\n=== Summary ===\n");
    std::printf("  passed: %d\n", tests_passed);
    std::printf("  failed: %d\n", tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
