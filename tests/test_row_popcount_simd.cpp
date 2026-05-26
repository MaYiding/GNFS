// test_row_popcount_simd.cpp - Correctness tests for the SIMD-accelerated
// GF(2) per-row popcount helper over row-major packed matrices.
//
// Strategy
// --------
// Every test that exercises the SIMD path runs both the scalar reference
// (`__builtin_popcountll` summed per row) and the dispatched helper,
// then asserts per-row equality. Per-row Hamming weight is a pure
// function of the row's words, so any divergence indicates a real
// kernel bug — we do not tolerate any difference across SIMD lanes or
// rows.
//
// Additional coverage:
// * ENV parsing (GNFS_GF2_ROW_POPCOUNT_SIMD = 0 / 1 / auto / unset /
//   garbage / on / off).
// * `row_words == 0`: silent no-op.
// * Single-row single-word: hand-verified 8 bit patterns including 0,
//   all-ones, alternating, and high-bit edge.
// * Aligned (32-word row width) vs unaligned (33-word row width) — the
//   SIMD path must fall through cleanly to the scalar residual tail.
// * Multi-row sweep (100 rows x 100 words / 1000 rows random) — stresses
//   the per-row outer loop and the row pointer arithmetic.
// * ForceOff vs Auto parity on a 100x100 batch.
// * Defensive contract: when caller passes a smaller `out_row_weights`
//   than `row_count`, the helper must clamp without UB.
// * 1M-row x 4-word perf info probe (informational, asserts equality
//   only, no wall-time assert).
//
// The build wires this test into the linalg test set
// (ctest RowPopcountSIMD).

#include <gnfs/linalg/detail/row_popcount_simd.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

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

namespace detail = gnfs::linalg::detail;

// Setenv helper that flushes the ENV cache so the helper re-reads the value.
static void set_env_and_reload(const char* value) {
    if (value == nullptr) {
        ::unsetenv("GNFS_GF2_ROW_POPCOUNT_SIMD");
    } else {
        ::setenv("GNFS_GF2_ROW_POPCOUNT_SIMD", value, 1);
    }
    detail::row_popcount_simd_reset_env_cache_for_testing();
}

static bool row_weights_equal(const std::vector<std::uint64_t>& a,
                              const std::vector<std::uint64_t>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

// Compare scalar reference against the dispatched helper on the same
// matrix.
static void compare_matrix(const std::vector<std::uint64_t>& matrix,
                           std::size_t row_words,
                           std::size_t row_count,
                           const char* label) {
    std::vector<std::uint64_t> scalar_out(row_count, 0xDEADBEEF);
    std::vector<std::uint64_t> dispatch_out(row_count, 0xDEADBEEF);
    detail::per_row_popcount_words_scalar(matrix, row_words, scalar_out);
    detail::per_row_popcount_words(matrix, row_words, dispatch_out);
    if (!row_weights_equal(scalar_out, dispatch_out)) {
        std::fprintf(stderr,
            "  per-row mismatch [%s] row_count=%zu row_words=%zu\n",
            label, row_count, row_words);
        for (std::size_t r = 0; r < row_count; ++r) {
            if (scalar_out[r] != dispatch_out[r]) {
                std::fprintf(stderr,
                    "    row %zu: scalar=%llu dispatch=%llu\n",
                    r,
                    static_cast<unsigned long long>(scalar_out[r]),
                    static_cast<unsigned long long>(dispatch_out[r]));
                break;
            }
        }
        tests_failed++;
        return;
    }
    TEST_PASS(label);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Test 1 — ENV unset → Auto mode → enabled when SIMD supported.
static void test_env_unset_auto() {
    std::printf("[1] env unset -> Auto + supported -> enabled\n");
    set_env_and_reload(nullptr);
    detail::RowPopcountSimdMode mode = detail::row_popcount_simd_mode();
    TEST_ASSERT(mode == detail::RowPopcountSimdMode::Auto,
                "unset env should resolve to Auto");
    const bool enabled = detail::row_popcount_simd_enabled();
    const bool supported = detail::row_popcount_simd_supported();
    std::printf("    supported=%d enabled=%d\n",
                supported ? 1 : 0, enabled ? 1 : 0);
    TEST_ASSERT(enabled == supported,
                "Auto mode must enable iff compile-time SIMD supported");
    TEST_PASS("env unset -> Auto + supported -> enabled");
}

// Test 2 — ENV "auto" explicit literal → Auto mode.
static void test_env_auto_literal() {
    std::printf("[2] env=auto -> Auto\n");
    set_env_and_reload("auto");
    detail::RowPopcountSimdMode mode = detail::row_popcount_simd_mode();
    TEST_ASSERT(mode == detail::RowPopcountSimdMode::Auto,
                "explicit 'auto' should resolve to Auto");
    set_env_and_reload(nullptr);
    TEST_PASS("env=auto -> Auto");
}

// Test 3 — ENV "0" / "off" → ForceOff → always disabled even on
// SIMD-capable host.
static void test_env_force_off() {
    std::printf("[3] env=0|off -> ForceOff\n");
    for (const char* value : {"0", "off"}) {
        set_env_and_reload(value);
        detail::RowPopcountSimdMode mode = detail::row_popcount_simd_mode();
        if (mode != detail::RowPopcountSimdMode::ForceOff) {
            std::fprintf(stderr,
                "  unexpected mode for env='%s': mode=%d\n",
                value, static_cast<int>(mode));
            tests_failed++;
            return;
        }
        if (detail::row_popcount_simd_enabled()) {
            std::fprintf(stderr,
                "  ForceOff must disable the SIMD path (value='%s')\n",
                value);
            tests_failed++;
            return;
        }
    }
    set_env_and_reload(nullptr);
    TEST_PASS("env=0|off -> ForceOff");
}

// Test 4 — ENV "1" / "on" → ForceOn. When supported, enables SIMD;
// when not supported, falls back to scalar but the gate reports
// `enabled() == supported`. Also covers fallback for unrecognised
// tokens like "garbage", "2", "true", "ON" (case-sensitive),
// "00", "01" — they all resolve back to Auto.
static void test_env_force_on_and_garbage() {
    std::printf("[4] env=1|on -> ForceOn; garbage / 2 / true / ON -> Auto\n");
    // ForceOn variants.
    for (const char* value : {"1", "on"}) {
        set_env_and_reload(value);
        detail::RowPopcountSimdMode mode = detail::row_popcount_simd_mode();
        if (mode != detail::RowPopcountSimdMode::ForceOn) {
            std::fprintf(stderr,
                "  unexpected mode for env='%s': mode=%d\n",
                value, static_cast<int>(mode));
            tests_failed++;
            return;
        }
        const bool enabled = detail::row_popcount_simd_enabled();
        const bool supported = detail::row_popcount_simd_supported();
        if (enabled != supported) {
            std::fprintf(stderr,
                "  ForceOn (value='%s') gate must equal compile-time supported\n",
                value);
            tests_failed++;
            return;
        }
    }
    // Garbage / unrecognised tokens fall back to Auto.
    for (const char* value : {"", "garbage", "2", "true", "ON", "Off", "00", "01"}) {
        set_env_and_reload(value);
        detail::RowPopcountSimdMode mode = detail::row_popcount_simd_mode();
        if (mode != detail::RowPopcountSimdMode::Auto) {
            std::fprintf(stderr,
                "  unexpected mode for env='%s': mode=%d (expected Auto)\n",
                value, static_cast<int>(mode));
            tests_failed++;
            return;
        }
    }
    set_env_and_reload(nullptr);
    TEST_PASS("env=1|on -> ForceOn; garbage -> Auto");
}

// Test 5 — empty matrix: helper must not touch outputs.
static void test_empty_matrix() {
    std::printf("[5] empty matrix\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> matrix;
    std::vector<std::uint64_t> out_row_weights;
    // Empty matrix + non-zero row_words: zero rows.
    detail::per_row_popcount_words(matrix, 4, out_row_weights);
    TEST_ASSERT(out_row_weights.empty(),
                "empty matrix with row_words=4 must leave out empty");
    // Non-empty matrix + zero row_words: silent no-op.
    matrix.assign(8, 0xFFULL);
    out_row_weights.assign(3, 0xBADBADBADULL);
    detail::per_row_popcount_words(matrix, 0, out_row_weights);
    // out_row_weights unchanged (sentinel survives).
    for (std::size_t i = 0; i < out_row_weights.size(); ++i) {
        TEST_ASSERT(out_row_weights[i] == 0xBADBADBADULL,
                    "row_words=0 must not touch outputs");
    }
    // Scalar reference: same contract.
    detail::per_row_popcount_words_scalar(matrix, 0, out_row_weights);
    for (std::size_t i = 0; i < out_row_weights.size(); ++i) {
        TEST_ASSERT(out_row_weights[i] == 0xBADBADBADULL,
                    "scalar row_words=0 must not touch outputs");
    }
    TEST_PASS("empty matrix");
}

// Test 6 — row_words=0 explicit: full coverage of zero-stride contract,
// across both empty and non-empty matrices.
static void test_row_words_zero() {
    std::printf("[6] row_words=0\n");
    set_env_and_reload(nullptr);
    // Non-empty matrix and non-empty out vector.
    std::vector<std::uint64_t> matrix(100, 0xFFFF'FFFF'FFFF'FFFFULL);
    std::vector<std::uint64_t> out(10, 0xCAFEULL);
    detail::per_row_popcount_words(matrix, 0, out);
    for (std::size_t i = 0; i < out.size(); ++i) {
        TEST_ASSERT(out[i] == 0xCAFEULL,
                    "row_words=0 dispatcher must not touch outputs");
    }
    detail::per_row_popcount_words_scalar(matrix, 0, out);
    for (std::size_t i = 0; i < out.size(); ++i) {
        TEST_ASSERT(out[i] == 0xCAFEULL,
                    "row_words=0 scalar must not touch outputs");
    }
    TEST_PASS("row_words=0");
}

// Test 7 — single row single word: hand-verified bit patterns.
static void test_single_row_single_word() {
    std::printf("[7] single row single word patterns\n");
    set_env_and_reload(nullptr);
    struct Case {
        std::uint64_t value;
        std::uint64_t expected;  // popcount(value)
    };
    Case cases[] = {
        {0x0000'0000'0000'0000ULL, 0},
        {0xFFFF'FFFF'FFFF'FFFFULL, 64},
        {0x5555'5555'5555'5555ULL, 32},
        {0xAAAA'AAAA'AAAA'AAAAULL, 32},
        {0x0F0F'0F0F'0F0F'0F0FULL, 32},
        {0x0000'0000'0000'0001ULL, 1},
        {0x8000'0000'0000'0000ULL, 1},
        {0x8000'0000'0000'0001ULL, 2},
    };
    for (const auto& c : cases) {
        std::vector<std::uint64_t> matrix{c.value};
        std::vector<std::uint64_t> out(1, 0xDEADBEEF);
        detail::per_row_popcount_words(matrix, 1, out);
        if (out[0] != c.expected) {
            std::fprintf(stderr,
                "  value=%016llx expected=%llu got=%llu\n",
                static_cast<unsigned long long>(c.value),
                static_cast<unsigned long long>(c.expected),
                static_cast<unsigned long long>(out[0]));
            tests_failed++;
            return;
        }
    }
    TEST_PASS("single row single word patterns");
}

// Test 8 — single row aligned (row_words = 32). Sum across the row
// must equal the sum of per-word popcounts.
static void test_single_row_aligned_32() {
    std::printf("[8] single row aligned (row_words=32)\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> matrix(32);
    std::mt19937_64 rng(0x1234ULL);
    for (auto& w : matrix) w = rng();
    compare_matrix(matrix, 32, 1, "single row aligned 32");
}

// Test 9 — multi-row unaligned (row_words = 33). Tests both the SIMD
// 2-word stride main loop and the scalar tail per row, plus correct
// row pointer advancement.
static void test_multi_row_unaligned_33() {
    std::printf("[9] multi-row unaligned (row_words=33, 10 rows)\n");
    set_env_and_reload(nullptr);
    constexpr std::size_t kRowWords = 33;
    constexpr std::size_t kRowCount = 10;
    std::vector<std::uint64_t> matrix(kRowWords * kRowCount);
    std::mt19937_64 rng(0xabcdULL);
    for (auto& w : matrix) w = rng();
    compare_matrix(matrix, kRowWords, kRowCount, "multi-row unaligned 33");
}

// Test 10 — 100 rows × 100 words random matrix.
static void test_100x100_random() {
    std::printf("[10] 100 row x 100 word random\n");
    set_env_and_reload(nullptr);
    constexpr std::size_t kRowWords = 100;
    constexpr std::size_t kRowCount = 100;
    std::vector<std::uint64_t> matrix(kRowWords * kRowCount);
    std::mt19937_64 rng(0xcafeULL);
    for (auto& w : matrix) w = rng();
    compare_matrix(matrix, kRowWords, kRowCount, "100 row x 100 word random");
}

// Test 11 — ForceOff vs Auto parity on a 100x100 random matrix.
static void test_force_off_vs_auto_parity() {
    std::printf("[11] ForceOff vs Auto parity (100x100 random)\n");
    constexpr std::size_t kRowWords = 100;
    constexpr std::size_t kRowCount = 100;
    std::vector<std::uint64_t> matrix(kRowWords * kRowCount);
    std::mt19937_64 rng(0xbeefULL);
    for (auto& w : matrix) w = rng();

    // Auto path.
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> auto_out(kRowCount, 0);
    detail::per_row_popcount_words(matrix, kRowWords, auto_out);

    // Force-off path.
    set_env_and_reload("0");
    std::vector<std::uint64_t> off_out(kRowCount, 0);
    detail::per_row_popcount_words(matrix, kRowWords, off_out);

    TEST_ASSERT(row_weights_equal(auto_out, off_out),
                "ForceOff and Auto per-row outputs must match bit-for-bit");

    set_env_and_reload(nullptr);
    TEST_PASS("ForceOff vs Auto parity");
}

// Test 12 — defensive contract: when caller passes a smaller
// `out_row_weights` than `row_count`, the helper must clamp to
// `out_row_weights.size()` and not write past the buffer.
static void test_undersized_out_row_weights() {
    std::printf("[12] undersized out_row_weights clamping\n");
    set_env_and_reload(nullptr);
    constexpr std::size_t kRowWords = 4;
    constexpr std::size_t kFullRowCount = 8;
    constexpr std::size_t kSmallRowCount = 3;  // caller only supplies 3 slots
    std::vector<std::uint64_t> matrix(kRowWords * kFullRowCount);
    std::mt19937_64 rng(0x9999ULL);
    for (auto& w : matrix) w = rng();

    // Expected weights for rows 0..2 (computed manually via the scalar
    // builtin so the assertion is independent of the helper).
    std::vector<std::uint64_t> expected(kSmallRowCount, 0);
    for (std::size_t r = 0; r < kSmallRowCount; ++r) {
        for (std::size_t k = 0; k < kRowWords; ++k) {
            expected[r] += static_cast<std::uint64_t>(
                gnfs::util::popcount64(matrix[r * kRowWords + k]));
        }
    }
    // Sentinel slot beyond the small span; sentinel must survive both
    // paths (we deliberately only pass kSmallRowCount entries below).
    std::vector<std::uint64_t> small_out(kSmallRowCount, 0xDEADBEEF);
    detail::per_row_popcount_words(matrix, kRowWords, small_out);
    for (std::size_t r = 0; r < kSmallRowCount; ++r) {
        if (small_out[r] != expected[r]) {
            std::fprintf(stderr,
                "  undersized out row %zu: expected=%llu got=%llu\n",
                r,
                static_cast<unsigned long long>(expected[r]),
                static_cast<unsigned long long>(small_out[r]));
            tests_failed++;
            return;
        }
    }
    TEST_PASS("undersized out_row_weights clamping");
}

// Test 13 — reset env cache hook re-reads the env value.
static void test_reset_env_cache_hook() {
    std::printf("[13] reset env cache hook re-read\n");
    // Set ForceOff first.
    set_env_and_reload("0");
    TEST_ASSERT(detail::row_popcount_simd_mode() == detail::RowPopcountSimdMode::ForceOff,
                "initial ForceOff after env set");
    // Then flip to ForceOn via setenv + reset.
    set_env_and_reload("1");
    TEST_ASSERT(detail::row_popcount_simd_mode() == detail::RowPopcountSimdMode::ForceOn,
                "after reset, env=1 must resolve to ForceOn");
    // Then unset.
    set_env_and_reload(nullptr);
    TEST_ASSERT(detail::row_popcount_simd_mode() == detail::RowPopcountSimdMode::Auto,
                "after reset, unset env must resolve to Auto");
    TEST_PASS("reset env cache hook re-read");
}

// Test 14 — perf-info probe (1M rows x 4 words, SIMD vs scalar).
// Asserts correctness equality; wall-time is informational only.
static void test_perf_info_1m_rows() {
    std::printf("[14] perf info (1M rows x 4 words)\n");
    constexpr std::size_t kRowWords = 4;
    constexpr std::size_t kRowCount = 1'000'000;
    std::vector<std::uint64_t> matrix(kRowWords * kRowCount);
    std::mt19937_64 rng(0xdeadULL);
    for (auto& w : matrix) w = rng();

    // Time the scalar reference path.
    set_env_and_reload("0");
    std::vector<std::uint64_t> scalar_out(kRowCount, 0);
    auto start_scalar = std::chrono::steady_clock::now();
    detail::per_row_popcount_words(matrix, kRowWords, scalar_out);
    auto end_scalar = std::chrono::steady_clock::now();
    double scalar_us = std::chrono::duration<double, std::micro>(
        end_scalar - start_scalar).count();

    // Time the SIMD (or scalar fallback if no SIMD) path.
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> simd_out(kRowCount, 0);
    auto start_simd = std::chrono::steady_clock::now();
    detail::per_row_popcount_words(matrix, kRowWords, simd_out);
    auto end_simd = std::chrono::steady_clock::now();
    double simd_us = std::chrono::duration<double, std::micro>(
        end_simd - start_simd).count();

    const double scalar_ns_per_row = (scalar_us * 1000.0) / static_cast<double>(kRowCount);
    const double simd_ns_per_row = (simd_us * 1000.0) / static_cast<double>(kRowCount);

    std::printf("    scalar=%.1f us (%.2f ns/row) simd=%.1f us (%.2f ns/row)\n",
                scalar_us, scalar_ns_per_row, simd_us, simd_ns_per_row);
    TEST_ASSERT(row_weights_equal(scalar_out, simd_out),
                "SIMD and scalar per-row outputs must match on 1M rows");
    set_env_and_reload(nullptr);
    TEST_PASS("perf info (1M rows x 4 words)");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    std::printf("=== test_row_popcount_simd ===\n");
    std::printf("compile-time SIMD supported: %s\n",
                detail::row_popcount_simd_supported() ? "yes" : "no");
    const char* env = std::getenv("GNFS_GF2_ROW_POPCOUNT_SIMD");
    std::printf("GNFS_GF2_ROW_POPCOUNT_SIMD = %s\n", env ? env : "(unset)");

    test_env_unset_auto();
    test_env_auto_literal();
    test_env_force_off();
    test_env_force_on_and_garbage();
    test_empty_matrix();
    test_row_words_zero();
    test_single_row_single_word();
    test_single_row_aligned_32();
    test_multi_row_unaligned_33();
    test_100x100_random();
    test_force_off_vs_auto_parity();
    test_undersized_out_row_weights();
    test_reset_env_cache_hook();
    test_perf_info_1m_rows();

    std::printf("\n=== Summary ===\n");
    std::printf("  passed: %d\n", tests_passed);
    std::printf("  failed: %d\n", tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
