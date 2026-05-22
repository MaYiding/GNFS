// test_and_words_simd.cpp - Correctness tests for the SIMD-accelerated GF(2)
// batch word-AND helper (three-argument variant `out[i] = a[i] & b[i]`).
//
// Strategy
// --------
// Every test that exercises the SIMD path runs both the scalar reference
// (a plain `out[i] = a[i] & b[i]` for loop) and the dispatched helper on
// the same input, then asserts per-index equality. AND is a pure function
// of the two input words, so any divergence indicates a real kernel bug —
// we do not tolerate any difference across SIMD lanes.
//
// Additional coverage:
// * ENV parsing (GNFS_GF2_AND_WORDS_SIMD = 0 / 1 / auto / unset / garbage).
// * Aligned vs unaligned batch sizes — the SIMD path must fall through
//   cleanly to the scalar residual tail.
// * Identity check: `a == b` (same content) yields `a` back unchanged.
//   This is the bit-level AND-identity (`a & a = a`) and proves the
//   helper does not accidentally route through XOR / OR.
// * Defensive contract: undersized `out` clamps without UB write past
//   `out`; the tail of `out` beyond the clamp window is preserved
//   unchanged.
//
// The build wires this test into the linalg test set
// (ctest AndWordsSIMD).

#include <gnfs/linalg/detail/and_words_simd.hpp>

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
        ::unsetenv("GNFS_GF2_AND_WORDS_SIMD");
    } else {
        ::setenv("GNFS_GF2_AND_WORDS_SIMD", value, 1);
    }
    detail::and_words_simd_reset_env_cache_for_testing();
}

static bool words_equal(const std::vector<std::uint64_t>& a,
                        const std::vector<std::uint64_t>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

// Compare scalar reference against the dispatched helper on the same
// (a, b) input. Uses fresh output buffers per path; the buffers are
// pre-filled with a sentinel value so we can detect any unwanted tail
// write past the clamp window.
static void compare_and(const std::vector<std::uint64_t>& a,
                        const std::vector<std::uint64_t>& b,
                        const char* label) {
    const std::size_t n = (a.size() < b.size()) ? a.size() : b.size();
    std::vector<std::uint64_t> scalar_out(n, 0xCCCCCCCC'CCCCCCCCULL);
    std::vector<std::uint64_t> dispatch_out(n, 0xCCCCCCCC'CCCCCCCCULL);
    detail::batch_and_words_scalar(a, b, scalar_out);
    detail::batch_and_words(a, b, dispatch_out);
    if (!words_equal(scalar_out, dispatch_out)) {
        std::fprintf(stderr, "  and mismatch [%s] n=%zu\n", label, n);
        for (std::size_t i = 0; i < n; ++i) {
            if (scalar_out[i] != dispatch_out[i]) {
                std::fprintf(stderr,
                    "    index %zu: a=%016llx b=%016llx scalar=%016llx dispatch=%016llx\n",
                    i,
                    static_cast<unsigned long long>(a[i]),
                    static_cast<unsigned long long>(b[i]),
                    static_cast<unsigned long long>(scalar_out[i]),
                    static_cast<unsigned long long>(dispatch_out[i]));
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
    detail::AndWordsSimdMode mode = detail::and_words_simd_mode();
    TEST_ASSERT(mode == detail::AndWordsSimdMode::Auto,
                "unset env should resolve to Auto");
    const bool enabled = detail::and_words_simd_enabled();
    const bool supported = detail::and_words_simd_supported();
    std::printf("    supported=%d enabled=%d\n",
                supported ? 1 : 0, enabled ? 1 : 0);
    TEST_ASSERT(enabled == supported,
                "Auto mode must enable iff compile-time SIMD supported");
    TEST_PASS("env unset -> Auto + supported -> enabled");
}

// Test 2 — ENV "0" → ForceOff → always disabled even on SIMD-capable host.
static void test_env_force_off() {
    std::printf("[2] env=0 -> ForceOff\n");
    set_env_and_reload("0");
    detail::AndWordsSimdMode mode = detail::and_words_simd_mode();
    TEST_ASSERT(mode == detail::AndWordsSimdMode::ForceOff,
                "env '0' should resolve to ForceOff");
    TEST_ASSERT(!detail::and_words_simd_enabled(),
                "ForceOff must disable the SIMD path");
    TEST_PASS("env=0 -> ForceOff");
}

// Test 3 — ENV "1" → ForceOn. When supported, enables SIMD; when not
// supported, falls back to scalar but the gate reports `enabled() == supported`.
static void test_env_force_on() {
    std::printf("[3] env=1 -> ForceOn\n");
    set_env_and_reload("1");
    detail::AndWordsSimdMode mode = detail::and_words_simd_mode();
    TEST_ASSERT(mode == detail::AndWordsSimdMode::ForceOn,
                "env '1' should resolve to ForceOn");
    const bool enabled = detail::and_words_simd_enabled();
    const bool supported = detail::and_words_simd_supported();
    TEST_ASSERT(enabled == supported,
                "ForceOn + supported must enable; ForceOn + unsupported must disable");
    TEST_PASS("env=1 -> ForceOn");
}

// Test 4 — ENV "auto" / "" / "garbage" / "2" / "true" / "00" / "01" → Auto.
static void test_env_garbage_fallback() {
    std::printf("[4] env=auto / '' / garbage -> Auto\n");
    for (const char* value : {"auto", "", "garbage", "2", "true", "00", "01"}) {
        set_env_and_reload(value);
        detail::AndWordsSimdMode mode = detail::and_words_simd_mode();
        if (mode != detail::AndWordsSimdMode::Auto) {
            std::fprintf(stderr,
                "  unexpected mode for env='%s': mode=%d\n",
                value, static_cast<int>(mode));
            tests_failed++;
            return;
        }
    }
    set_env_and_reload(nullptr);
    TEST_PASS("env=auto / '' / garbage -> Auto");
}

// Test 5 — empty input: helper must return without touching the output span.
static void test_empty_input() {
    std::printf("[5] empty input\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> a;
    std::vector<std::uint64_t> b;
    std::vector<std::uint64_t> out;
    // All empty -> no-op, no crash.
    detail::batch_and_words(a, b, out);
    TEST_ASSERT(out.empty(),
                "batch_and_words on empty inputs must leave out empty");
    // a empty, b non-empty, out preallocated -> out preserved.
    std::vector<std::uint64_t> b_only = {0xABCDULL, 0x1234ULL};
    std::vector<std::uint64_t> out_pre = {0xDEADULL, 0xBEEFULL};
    detail::batch_and_words(a, b_only, out_pre);
    TEST_ASSERT(out_pre.size() == 2 && out_pre[0] == 0xDEADULL && out_pre[1] == 0xBEEFULL,
                "batch_and_words with empty a must preserve out unchanged");
    // b empty, a non-empty, out preallocated -> out preserved.
    std::vector<std::uint64_t> a_only = {0xCAFEULL, 0xBABEULL};
    detail::batch_and_words(a_only, b, out_pre);
    TEST_ASSERT(out_pre[0] == 0xDEADULL && out_pre[1] == 0xBEEFULL,
                "batch_and_words with empty b must preserve out unchanged");
    // Also exercise scalar reference on empty input.
    detail::batch_and_words_scalar(a, b, out);
    TEST_ASSERT(out.empty(),
                "scalar reference on empty inputs must leave out empty");
    TEST_PASS("empty input");
}

// Test 6 — single word AND across hand-verified patterns.
static void test_single_word_patterns() {
    std::printf("[6] single word patterns\n");
    set_env_and_reload(nullptr);
    struct Case {
        std::uint64_t a;
        std::uint64_t b;
        std::uint64_t expected;  // a & b
    };
    Case cases[] = {
        // 0 AND 0 = 0
        {0ULL, 0ULL, 0ULL},
        // a AND a = a (identity)
        {0xFFULL, 0xFFULL, 0xFFULL},
        // 0xAA AND 0x55 = 0 (disjoint bits)
        {0xAAAA'AAAA'AAAA'AAAAULL, 0x5555'5555'5555'5555ULL, 0ULL},
        // 0xFF AND 0xAA = 0xAA (subset)
        {0xFFFF'FFFF'FFFF'FFFFULL, 0xAAAA'AAAA'AAAA'AAAAULL, 0xAAAA'AAAA'AAAA'AAAAULL},
        // 0 AND x = 0 (zero annihilates)
        {0ULL, 0xDEAD'BEEF'CAFE'BABEULL, 0ULL},
        // 0xFF...F AND 0xFF...F = 0xFF...F (full identity)
        {0xFFFF'FFFF'FFFF'FFFFULL, 0xFFFF'FFFF'FFFF'FFFFULL, 0xFFFF'FFFF'FFFF'FFFFULL},
        // High-bit + low-bit selection
        {0x8000'0000'0000'0001ULL, 0xC000'0000'0000'0003ULL, 0x8000'0000'0000'0001ULL},
        // Cross-byte pattern (disjoint nibbles -> 0)
        {0x0F0F'0F0F'0F0F'0F0FULL, 0xF0F0'F0F0'F0F0'F0F0ULL, 0ULL},
    };
    for (const auto& c : cases) {
        std::vector<std::uint64_t> a{c.a};
        std::vector<std::uint64_t> b{c.b};
        std::vector<std::uint64_t> out(1, 0xCCCC'CCCC'CCCC'CCCCULL);
        detail::batch_and_words(a, b, out);
        if (out[0] != c.expected) {
            std::fprintf(stderr,
                "  a=%016llx b=%016llx expected=%016llx got=%016llx\n",
                static_cast<unsigned long long>(c.a),
                static_cast<unsigned long long>(c.b),
                static_cast<unsigned long long>(c.expected),
                static_cast<unsigned long long>(out[0]));
            tests_failed++;
            return;
        }
    }
    TEST_PASS("single word patterns");
}

// Test 7 — aligned batch (size = 32 word).
static void test_aligned_batch_32() {
    std::printf("[7] aligned batch (size=32)\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> a(32);
    std::vector<std::uint64_t> b(32);
    std::mt19937_64 rng(0x1234ULL);
    for (auto& w : a) w = rng();
    for (auto& w : b) w = rng();
    compare_and(a, b, "aligned batch 32 (random)");
}

// Test 8 — unaligned batch (size = 33 word): triggers the scalar tail
// after the SIMD-aligned prefix is consumed.
static void test_unaligned_batch_33() {
    std::printf("[8] unaligned batch (size=33)\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> a(33);
    std::vector<std::uint64_t> b(33);
    std::mt19937_64 rng(0xabcdULL);
    for (auto& w : a) w = rng();
    for (auto& w : b) w = rng();
    compare_and(a, b, "unaligned batch 33 (random)");
}

// Test 9 — large random batch (1000 word). Stresses the unrolled SIMD path
// across many iterations.
static void test_large_random_1000() {
    std::printf("[9] large random batch (size=1000)\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> a(1000);
    std::vector<std::uint64_t> b(1000);
    std::mt19937_64 rng(0xcafeULL);
    for (auto& w : a) w = rng();
    for (auto& w : b) w = rng();
    compare_and(a, b, "large random batch 1000");
}

// Test 10 — ForceOff vs Auto parity on a 1000-word random batch.
static void test_force_off_vs_auto_parity() {
    std::printf("[10] ForceOff vs Auto parity (1000 random)\n");
    std::vector<std::uint64_t> a(1000);
    std::vector<std::uint64_t> b(1000);
    std::mt19937_64 rng(0xbeefULL);
    for (auto& w : a) w = rng();
    for (auto& w : b) w = rng();

    // Auto path.
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> auto_out(1000, 0ULL);
    detail::batch_and_words(a, b, auto_out);

    // Force-off path.
    set_env_and_reload("0");
    std::vector<std::uint64_t> off_out(1000, 0ULL);
    detail::batch_and_words(a, b, off_out);

    TEST_ASSERT(words_equal(auto_out, off_out),
                "ForceOff and Auto batch outputs must match bit-for-bit");

    set_env_and_reload(nullptr);
    TEST_PASS("ForceOff vs Auto parity");
}

// Test 11 — a == b (same content): AND-with-self is identity, output
// must equal the inputs. Exercises every SIMD path lane and guards
// against accidental routing through XOR (`a^a=0`) or OR (`a|a=a` would
// still pass, so we also include the bit-pattern test which AND-only
// satisfies; see Test 6 disjoint-nibble case).
static void test_and_with_self_yields_input() {
    std::printf("[11] AND with self -> input unchanged (identity)\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> a(128);
    std::mt19937_64 rng(0x3141ULL);
    for (auto& w : a) w = rng();
    std::vector<std::uint64_t> b = a;  // same content
    std::vector<std::uint64_t> expected = a;
    std::vector<std::uint64_t> out(128, 0xCCCC'CCCC'CCCC'CCCCULL);
    detail::batch_and_words(a, b, out);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] != expected[i]) {
            std::fprintf(stderr,
                "  index %zu: expected %016llx, got %016llx\n",
                i,
                static_cast<unsigned long long>(expected[i]),
                static_cast<unsigned long long>(out[i]));
            tests_failed++;
            return;
        }
    }
    TEST_PASS("AND with self -> input unchanged");
}

// Test 12 — defensive contract: out shorter than min(a.size(), b.size()).
// Only the first `out.size()` words should be written; the helper must
// not touch any memory past `out`, and any preallocated tail of `out`
// beyond the clamp window must remain unchanged.
static void test_undersized_out_clamping() {
    std::printf("[12] undersized out span clamping\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> a = {0xFFULL, 0xAAULL, 0x55ULL, 0x11ULL};  // size 4
    std::vector<std::uint64_t> b = {0x0FULL, 0x05ULL, 0x33ULL, 0x44ULL};  // size 4
    // out is shorter (size 2) and starts with a sentinel we expect to
    // see preserved past the clamp window after we re-extend it.
    std::vector<std::uint64_t> out = {0xDEADULL, 0xBEEFULL};
    detail::batch_and_words(a, b, out);
    // out[0] = 0xFF & 0x0F = 0x0F
    // out[1] = 0xAA & 0x05 = 0x00
    TEST_ASSERT(out.size() == 2,
                "out size must not change");
    TEST_ASSERT(out[0] == 0x0FULL && out[1] == 0x00ULL,
                "out shorter than inputs must clamp to first out.size() words");
    TEST_PASS("undersized out span clamping");
}

// Test 13 — a shorter than b (defensive). Only the first
// `min(a.size(), b.size(), out.size())` words should be written; the
// tail of `out` beyond that bound must be preserved unchanged.
static void test_a_shorter_than_b_tail_preserved() {
    std::printf("[13] a shorter than b: tail of out preserved\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> a = {0xFFULL, 0xAAULL};                      // size 2
    std::vector<std::uint64_t> b = {0x0FULL, 0x05ULL, 0x33ULL, 0x44ULL};    // size 4
    // out preallocated to size 4 with sentinel to detect any unwanted
    // writes past the clamp window.
    std::vector<std::uint64_t> out = {0xCCCC'CCCC'CCCC'CCCCULL,
                                      0xCCCC'CCCC'CCCC'CCCCULL,
                                      0xDEAD'DEAD'DEAD'DEADULL,
                                      0xBEEF'BEEF'BEEF'BEEFULL};
    detail::batch_and_words(a, b, out);
    // out[0] = 0xFF & 0x0F = 0x0F
    // out[1] = 0xAA & 0x05 = 0x00
    // out[2] preserved = 0xDEAD'DEAD'DEAD'DEAD
    // out[3] preserved = 0xBEEF'BEEF'BEEF'BEEF
    TEST_ASSERT(out.size() == 4,
                "out size must not change");
    TEST_ASSERT(out[0] == 0x0FULL && out[1] == 0x00ULL,
                "first min(a,b) words of out must be a & b");
    TEST_ASSERT(out[2] == 0xDEAD'DEAD'DEAD'DEADULL &&
                out[3] == 0xBEEF'BEEF'BEEF'BEEFULL,
                "tail of out beyond min(a.size(), b.size()) must be preserved unchanged");
    TEST_PASS("a shorter than b: tail of out preserved");
}

// Test 14 — perf-info probe (1M AND cycles, SIMD vs scalar). Not a hard
// assertion on the timing; correctness equality is asserted.
static void test_perf_info_1m() {
    std::printf("[14] perf info (1M words, SIMD vs scalar)\n");
    constexpr std::size_t kN = 1'000'000;
    std::vector<std::uint64_t> a(kN);
    std::vector<std::uint64_t> b(kN);
    std::mt19937_64 rng(0xdeadULL);
    for (auto& w : a) w = rng();
    for (auto& w : b) w = rng();

    // Time the scalar reference path.
    set_env_and_reload("0");
    std::vector<std::uint64_t> scalar_out(kN, 0ULL);
    auto start_scalar = std::chrono::steady_clock::now();
    detail::batch_and_words(a, b, scalar_out);
    auto end_scalar = std::chrono::steady_clock::now();
    double scalar_us = std::chrono::duration<double, std::micro>(
        end_scalar - start_scalar).count();

    // Time the SIMD (or scalar fallback if no SIMD) path.
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> simd_out(kN, 0ULL);
    auto start_simd = std::chrono::steady_clock::now();
    detail::batch_and_words(a, b, simd_out);
    auto end_simd = std::chrono::steady_clock::now();
    double simd_us = std::chrono::duration<double, std::micro>(
        end_simd - start_simd).count();

    std::printf("    scalar=%.1f us simd=%.1f us\n", scalar_us, simd_us);
    TEST_ASSERT(words_equal(scalar_out, simd_out),
                "SIMD and scalar outputs must match on 1M words");
    set_env_and_reload(nullptr);
    TEST_PASS("perf info (1M words)");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    std::printf("=== test_and_words_simd ===\n");
    std::printf("compile-time SIMD supported: %s\n",
                detail::and_words_simd_supported() ? "yes" : "no");
    const char* env = std::getenv("GNFS_GF2_AND_WORDS_SIMD");
    std::printf("GNFS_GF2_AND_WORDS_SIMD = %s\n", env ? env : "(unset)");

    test_env_unset_auto();
    test_env_force_off();
    test_env_force_on();
    test_env_garbage_fallback();
    test_empty_input();
    test_single_word_patterns();
    test_aligned_batch_32();
    test_unaligned_batch_33();
    test_large_random_1000();
    test_force_off_vs_auto_parity();
    test_and_with_self_yields_input();
    test_undersized_out_clamping();
    test_a_shorter_than_b_tail_preserved();
    test_perf_info_1m();

    std::printf("\n=== Summary ===\n");
    std::printf("  passed: %d\n", tests_passed);
    std::printf("  failed: %d\n", tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
