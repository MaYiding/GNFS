// test_popcount_simd.cpp - Correctness tests for the SIMD-accelerated GF(2)
// word popcount batch helpers.
//
// Strategy
// --------
// Every test that exercises the SIMD path runs both the scalar reference
// (`__builtin_popcountll` per word) and the dispatched helper, then asserts
// per-index equality. Popcount is a pure function of its input word, so any
// divergence indicates a real kernel bug — we do not tolerate any difference
// across SIMD lanes.
//
// Additional coverage:
// * ENV parsing (GNFS_GF2_POPCNT_SIMD = 0 / 1 / auto / unset / garbage).
// * Aligned vs unaligned batch sizes — the SIMD path must fall through
//   cleanly to the scalar residual tail.
// * total_popcount_words: sum reduction equivalence.
//
// The build wires this test into the linalg test set
// (ctest LinalgPopcountSimd).

#include <gnfs/linalg/detail/popcount_simd.hpp>

#include <array>
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
// On POSIX `setenv` is the canonical API; we wrap it for clarity.
static void set_env_and_reload(const char* value) {
    if (value == nullptr) {
        ::unsetenv("GNFS_GF2_POPCNT_SIMD");
    } else {
        ::setenv("GNFS_GF2_POPCNT_SIMD", value, 1);
    }
    detail::popcount_simd_reset_env_cache_for_testing();
}

static bool batch_results_equal(const std::vector<std::uint32_t>& a,
                                const std::vector<std::uint32_t>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static void compare_batch(const std::vector<std::uint64_t>& words,
                          const char* label) {
    std::vector<std::uint32_t> scalar_out(words.size(), 0);
    std::vector<std::uint32_t> dispatch_out(words.size(), 0);
    detail::batch_popcount_words_scalar(words, scalar_out);
    detail::batch_popcount_words(words, dispatch_out);
    if (!batch_results_equal(scalar_out, dispatch_out)) {
        std::fprintf(stderr, "  batch mismatch [%s] n=%zu\n",
                     label, words.size());
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (scalar_out[i] != dispatch_out[i]) {
                std::fprintf(stderr,
                    "    index %zu: word=%016llx scalar=%u dispatch=%u\n",
                    i, static_cast<unsigned long long>(words[i]),
                    scalar_out[i], dispatch_out[i]);
                break;
            }
        }
        tests_failed++;
        return;
    }
    // Total reduction parity.
    std::uint64_t scalar_total = detail::total_popcount_words_scalar(words);
    std::uint64_t dispatch_total = detail::total_popcount_words(words);
    if (scalar_total != dispatch_total) {
        std::fprintf(stderr,
            "  total mismatch [%s] scalar=%llu dispatch=%llu\n",
            label, static_cast<unsigned long long>(scalar_total),
            static_cast<unsigned long long>(dispatch_total));
        tests_failed++;
        return;
    }
    TEST_PASS(label);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Test 1 — ENV unset → Auto mode → enabled when SIMD supported.
// When the host has NEON or AVX2, Auto must enable the SIMD path.
// When the host has neither, Auto returns false (no SIMD code to run).
static void test_env_unset_auto() {
    std::printf("[1] env unset -> Auto + supported -> enabled\n");
    set_env_and_reload(nullptr);
    detail::PopcountSimdMode mode = detail::popcount_simd_mode();
    TEST_ASSERT(mode == detail::PopcountSimdMode::Auto,
                "unset env should resolve to Auto");
    const bool enabled = detail::popcount_simd_enabled();
    const bool supported = detail::popcount_simd_supported();
    std::printf("    supported=%d enabled=%d\n",
                supported ? 1 : 0, enabled ? 1 : 0);
    // Auto + supported → must enable.
    TEST_ASSERT(enabled == supported,
                "Auto mode must enable iff compile-time SIMD supported");
    TEST_PASS("env unset -> Auto + supported -> enabled");
}

// Test 2 — ENV "0" → ForceOff → always disabled even on SIMD-capable host.
static void test_env_force_off() {
    std::printf("[2] env=0 -> ForceOff\n");
    set_env_and_reload("0");
    detail::PopcountSimdMode mode = detail::popcount_simd_mode();
    TEST_ASSERT(mode == detail::PopcountSimdMode::ForceOff,
                "env '0' should resolve to ForceOff");
    TEST_ASSERT(!detail::popcount_simd_enabled(),
                "ForceOff must disable the SIMD path");
    TEST_PASS("env=0 -> ForceOff");
}

// Test 3 — ENV "1" → ForceOn. When supported, enables SIMD; when not
// supported, still falls back to scalar (kept correctness) but the gate
// itself reports `enabled() == supported`.
static void test_env_force_on() {
    std::printf("[3] env=1 -> ForceOn\n");
    set_env_and_reload("1");
    detail::PopcountSimdMode mode = detail::popcount_simd_mode();
    TEST_ASSERT(mode == detail::PopcountSimdMode::ForceOn,
                "env '1' should resolve to ForceOn");
    const bool enabled = detail::popcount_simd_enabled();
    const bool supported = detail::popcount_simd_supported();
    // ForceOn: enabled() may only return true when SIMD is actually
    // available (otherwise the dispatcher would call a non-existent
    // SIMD code path).
    TEST_ASSERT(enabled == supported,
                "ForceOn + supported must enable; ForceOn + unsupported must disable (no SIMD code path)");
    TEST_PASS("env=1 -> ForceOn");
}

// Test 4 — ENV "auto" / "" / "garbage" → Auto. Any non-recognised value
// falls back to Auto per the documented decision table.
static void test_env_garbage_fallback() {
    std::printf("[4] env=auto / '' / garbage -> Auto\n");
    for (const char* value : {"auto", "", "garbage", "2", "true", "00", "01"}) {
        set_env_and_reload(value);
        detail::PopcountSimdMode mode = detail::popcount_simd_mode();
        if (mode != detail::PopcountSimdMode::Auto) {
            std::fprintf(stderr,
                "  unexpected mode for env='%s': mode=%d\n",
                value, static_cast<int>(mode));
            tests_failed++;
            return;
        }
    }
    // Restore to unset for downstream tests.
    set_env_and_reload(nullptr);
    TEST_PASS("env=auto / '' / garbage -> Auto");
}

// Test 5 — empty input: both APIs must return without touching the spans.
static void test_empty_input() {
    std::printf("[5] empty input\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> words;
    std::vector<std::uint32_t> out;
    detail::batch_popcount_words(words, out);
    TEST_ASSERT(out.empty(),
                "batch_popcount_words on empty input must leave out empty");
    std::uint64_t total = detail::total_popcount_words(words);
    TEST_ASSERT(total == 0,
                "total_popcount_words on empty input must be zero");
    // Also exercise scalar reference on empty input.
    detail::batch_popcount_words_scalar(words, out);
    TEST_ASSERT(out.empty(), "scalar reference on empty input must leave out empty");
    TEST_ASSERT(detail::total_popcount_words_scalar(words) == 0,
                "scalar total on empty input must be zero");
    TEST_PASS("empty input");
}

// Test 6 — single-word inputs across a range of bit patterns. Each pattern
// is hand-verified; we also cross-check against `__builtin_popcountll`.
static void test_single_word_patterns() {
    std::printf("[6] single word patterns\n");
    set_env_and_reload(nullptr);
    struct Case {
        std::uint64_t word;
        std::uint32_t expected;
    };
    Case cases[] = {
        {0ULL, 0},
        {1ULL, 1},
        {0xFFULL, 8},
        {0xAAAA'AAAA'AAAA'AAAAULL, 32},
        {0x5555'5555'5555'5555ULL, 32},
        {0xFFFF'FFFF'FFFF'FFFFULL, 64},
        {0x8000'0000'0000'0001ULL, 2},
        {0x0F0F'0F0F'0F0F'0F0FULL, 32},
    };
    for (const auto& c : cases) {
        std::vector<std::uint64_t> words{c.word};
        std::vector<std::uint32_t> out(1, 0);
        detail::batch_popcount_words(words, out);
        if (out[0] != c.expected) {
            std::fprintf(stderr,
                "  word=%016llx expected=%u got=%u\n",
                static_cast<unsigned long long>(c.word),
                c.expected, out[0]);
            tests_failed++;
            return;
        }
        // Builtin cross-check (gives us a second oracle, in case the
        // hand-counted "expected" value is wrong).
        std::uint32_t builtin = static_cast<std::uint32_t>(__builtin_popcountll(c.word));
        if (out[0] != builtin) {
            std::fprintf(stderr,
                "  word=%016llx expected=%u dispatch=%u builtin=%u\n",
                static_cast<unsigned long long>(c.word),
                c.expected, out[0], builtin);
            tests_failed++;
            return;
        }
    }
    TEST_PASS("single word patterns");
}

// Test 7 — aligned batch (size = 32 word). Both NEON 2-lane and AVX2 4-lane
// fully saturate inside the unrolled loop with no scalar tail.
static void test_aligned_batch_32() {
    std::printf("[7] aligned batch (size=32)\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> words(32);
    std::mt19937_64 rng(0x1234ULL);
    for (auto& w : words) w = rng();
    compare_batch(words, "aligned batch 32 (random)");
}

// Test 8 — unaligned batch (size=33 word): triggers the scalar residual tail
// path after the SIMD-aligned prefix is consumed.
static void test_unaligned_batch_33() {
    std::printf("[8] unaligned batch (size=33)\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> words(33);
    std::mt19937_64 rng(0xabcdULL);
    for (auto& w : words) w = rng();
    compare_batch(words, "unaligned batch 33 (random)");
}

// Test 9 — large random batch (1000 word). Stresses the unrolled SIMD path
// across many iterations; any cumulative drift would expose itself here.
static void test_large_random_1000() {
    std::printf("[9] large random batch (size=1000)\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> words(1000);
    std::mt19937_64 rng(0xcafeULL);
    for (auto& w : words) w = rng();
    compare_batch(words, "large random batch 1000");
}

// Test 10 — total popcount equivalence on the same inputs as the per-word
// batch path. Confirms that the SIMD reduction kernel and the per-word
// kernel produce a consistent sum.
static void test_total_popcount_equivalence() {
    std::printf("[10] total popcount equivalence\n");
    set_env_and_reload(nullptr);
    std::mt19937_64 rng(0xfeedULL);
    for (std::size_t n : {std::size_t{1}, std::size_t{2}, std::size_t{3},
                          std::size_t{4}, std::size_t{8}, std::size_t{16},
                          std::size_t{31}, std::size_t{32}, std::size_t{63},
                          std::size_t{64}, std::size_t{100}, std::size_t{1000}}) {
        std::vector<std::uint64_t> words(n);
        for (auto& w : words) w = rng();
        // Per-word sum vs total entry point.
        std::vector<std::uint32_t> per_word(n, 0);
        detail::batch_popcount_words(words, per_word);
        std::uint64_t per_word_sum = 0;
        for (auto v : per_word) per_word_sum += v;
        std::uint64_t total = detail::total_popcount_words(words);
        if (per_word_sum != total) {
            std::fprintf(stderr,
                "  total mismatch n=%zu per_word_sum=%llu total=%llu\n",
                n, static_cast<unsigned long long>(per_word_sum),
                static_cast<unsigned long long>(total));
            tests_failed++;
            return;
        }
        // Scalar reference also must equal both.
        std::uint64_t scalar_total = detail::total_popcount_words_scalar(words);
        if (scalar_total != total) {
            std::fprintf(stderr,
                "  scalar/dispatch total mismatch n=%zu scalar=%llu dispatch=%llu\n",
                n, static_cast<unsigned long long>(scalar_total),
                static_cast<unsigned long long>(total));
            tests_failed++;
            return;
        }
    }
    TEST_PASS("total popcount equivalence");
}

// Test 11 — force-off ENV must produce bit-for-bit identical output to
// auto. The scalar fallback and the SIMD path are guaranteed to give the
// same result for any input.
static void test_force_off_vs_auto_parity() {
    std::printf("[11] ForceOff vs Auto parity (1000 random)\n");
    std::vector<std::uint64_t> words(1000);
    std::mt19937_64 rng(0xbeefULL);
    for (auto& w : words) w = rng();

    // Auto path.
    set_env_and_reload(nullptr);
    std::vector<std::uint32_t> auto_out(1000, 0);
    detail::batch_popcount_words(words, auto_out);
    std::uint64_t auto_total = detail::total_popcount_words(words);

    // Force-off path.
    set_env_and_reload("0");
    std::vector<std::uint32_t> off_out(1000, 0);
    detail::batch_popcount_words(words, off_out);
    std::uint64_t off_total = detail::total_popcount_words(words);

    TEST_ASSERT(batch_results_equal(auto_out, off_out),
                "ForceOff and Auto batch outputs must match bit-for-bit");
    TEST_ASSERT(auto_total == off_total,
                "ForceOff and Auto totals must match bit-for-bit");

    // Restore unset.
    set_env_and_reload(nullptr);
    TEST_PASS("ForceOff vs Auto parity");
}

// Test 12 — perf-info probe (not a hard assertion). Measures wall time of
// SIMD path vs scalar path on a 1M-word array to give a sense of relative
// performance. Output goes to stdout for human inspection only.
static void test_perf_info_1m() {
    std::printf("[12] perf info (1M words, SIMD vs scalar)\n");
    constexpr std::size_t kN = 1'000'000;
    std::vector<std::uint64_t> words(kN);
    std::mt19937_64 rng(0xdeadULL);
    for (auto& w : words) w = rng();

    // Time the scalar reference path.
    set_env_and_reload("0");
    auto start_scalar = std::chrono::steady_clock::now();
    std::uint64_t scalar_total = detail::total_popcount_words(words);
    auto end_scalar = std::chrono::steady_clock::now();
    double scalar_us = std::chrono::duration<double, std::micro>(
        end_scalar - start_scalar).count();

    // Time the SIMD (or scalar fallback if no SIMD) path.
    set_env_and_reload(nullptr);
    auto start_simd = std::chrono::steady_clock::now();
    std::uint64_t simd_total = detail::total_popcount_words(words);
    auto end_simd = std::chrono::steady_clock::now();
    double simd_us = std::chrono::duration<double, std::micro>(
        end_simd - start_simd).count();

    std::printf("    scalar=%.1f us simd=%.1f us total=%llu\n",
                scalar_us, simd_us,
                static_cast<unsigned long long>(simd_total));
    // Correctness check only (perf is informational).
    TEST_ASSERT(scalar_total == simd_total,
                "SIMD and scalar totals must match on 1M words");
    set_env_and_reload(nullptr);
    TEST_PASS("perf info (1M words)");
}

// Test 13 — defensive contract: when caller passes a smaller `out` than
// `words`, the helper must clamp to `out.size()` and not write past the
// buffer. Documents the soft contract; production callers should size
// the spans equal.
static void test_undersized_out_span() {
    std::printf("[13] undersized out span clamping\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> words = {0xFF, 0xAA, 0x55, 0xFFFFFFFFFFFFFFFFULL};
    std::vector<std::uint32_t> small_out(2, 0);
    detail::batch_popcount_words(words, small_out);
    // Only the first 2 entries are touched.
    TEST_ASSERT(small_out[0] == 8 && small_out[1] == 4,
                "undersized out must contain the first 2 popcounts");
    set_env_and_reload(nullptr);
    TEST_PASS("undersized out span clamping");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

#include <chrono>

int main() {
    std::printf("=== test_popcount_simd ===\n");
    std::printf("compile-time SIMD supported: %s\n",
                detail::popcount_simd_supported() ? "yes" : "no");
    const char* env = std::getenv("GNFS_GF2_POPCNT_SIMD");
    std::printf("GNFS_GF2_POPCNT_SIMD = %s\n", env ? env : "(unset)");

    test_env_unset_auto();
    test_env_force_off();
    test_env_force_on();
    test_env_garbage_fallback();
    test_empty_input();
    test_single_word_patterns();
    test_aligned_batch_32();
    test_unaligned_batch_33();
    test_large_random_1000();
    test_total_popcount_equivalence();
    test_force_off_vs_auto_parity();
    test_perf_info_1m();
    test_undersized_out_span();

    std::printf("\n=== Summary ===\n");
    std::printf("  passed: %d\n", tests_passed);
    std::printf("  failed: %d\n", tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
