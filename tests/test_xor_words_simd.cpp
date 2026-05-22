// test_xor_words_simd.cpp - Correctness tests for the SIMD-accelerated GF(2)
// batch row-word XOR helper.
//
// Strategy
// --------
// Every test that exercises the SIMD path runs both the scalar reference
// (a plain `dst[i] ^= src[i]` for loop) and the dispatched helper on the
// same input, then asserts per-index equality. XOR is a pure function of
// the two input words, so any divergence indicates a real kernel bug — we
// do not tolerate any difference across SIMD lanes.
//
// Additional coverage:
// * ENV parsing (GNFS_GF2_ROW_XOR_SIMD = 0 / 1 / auto / unset / garbage).
// * Aligned vs unaligned batch sizes — the SIMD path must fall through
//   cleanly to the scalar residual tail.
// * Identity check: XOR with self must produce all-zero output.
// * Defensive contract: dst shorter than src clamps without UB write past
//   `dst`; src shorter than dst preserves the dst tail unchanged.
//
// The build wires this test into the linalg test set
// (ctest RowXorSIMD).

#include <gnfs/linalg/detail/xor_words_simd.hpp>

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
        ::unsetenv("GNFS_GF2_ROW_XOR_SIMD");
    } else {
        ::setenv("GNFS_GF2_ROW_XOR_SIMD", value, 1);
    }
    detail::xor_words_simd_reset_env_cache_for_testing();
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
// (dst, src) input. Uses fresh copies of dst per path.
static void compare_xor(const std::vector<std::uint64_t>& dst_init,
                        const std::vector<std::uint64_t>& src,
                        const char* label) {
    std::vector<std::uint64_t> scalar_dst = dst_init;
    std::vector<std::uint64_t> dispatch_dst = dst_init;
    detail::batch_xor_words_scalar(scalar_dst, src);
    detail::batch_xor_words(dispatch_dst, src);
    if (!words_equal(scalar_dst, dispatch_dst)) {
        std::fprintf(stderr, "  xor mismatch [%s] n=%zu\n",
                     label, dst_init.size());
        for (std::size_t i = 0; i < dst_init.size(); ++i) {
            if (scalar_dst[i] != dispatch_dst[i]) {
                std::fprintf(stderr,
                    "    index %zu: dst_init=%016llx src=%016llx scalar=%016llx dispatch=%016llx\n",
                    i,
                    static_cast<unsigned long long>(dst_init[i]),
                    static_cast<unsigned long long>(i < src.size() ? src[i] : 0ULL),
                    static_cast<unsigned long long>(scalar_dst[i]),
                    static_cast<unsigned long long>(dispatch_dst[i]));
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
    detail::XorWordsSimdMode mode = detail::xor_words_simd_mode();
    TEST_ASSERT(mode == detail::XorWordsSimdMode::Auto,
                "unset env should resolve to Auto");
    const bool enabled = detail::xor_words_simd_enabled();
    const bool supported = detail::xor_words_simd_supported();
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
    detail::XorWordsSimdMode mode = detail::xor_words_simd_mode();
    TEST_ASSERT(mode == detail::XorWordsSimdMode::ForceOff,
                "env '0' should resolve to ForceOff");
    TEST_ASSERT(!detail::xor_words_simd_enabled(),
                "ForceOff must disable the SIMD path");
    TEST_PASS("env=0 -> ForceOff");
}

// Test 3 — ENV "1" → ForceOn. When supported, enables SIMD; when not
// supported, falls back to scalar but the gate reports `enabled() == supported`.
static void test_env_force_on() {
    std::printf("[3] env=1 -> ForceOn\n");
    set_env_and_reload("1");
    detail::XorWordsSimdMode mode = detail::xor_words_simd_mode();
    TEST_ASSERT(mode == detail::XorWordsSimdMode::ForceOn,
                "env '1' should resolve to ForceOn");
    const bool enabled = detail::xor_words_simd_enabled();
    const bool supported = detail::xor_words_simd_supported();
    TEST_ASSERT(enabled == supported,
                "ForceOn + supported must enable; ForceOn + unsupported must disable");
    TEST_PASS("env=1 -> ForceOn");
}

// Test 4 — ENV "auto" / "" / "garbage" / "2" / "true" / "00" / "01" → Auto.
static void test_env_garbage_fallback() {
    std::printf("[4] env=auto / '' / garbage -> Auto\n");
    for (const char* value : {"auto", "", "garbage", "2", "true", "00", "01"}) {
        set_env_and_reload(value);
        detail::XorWordsSimdMode mode = detail::xor_words_simd_mode();
        if (mode != detail::XorWordsSimdMode::Auto) {
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

// Test 5 — empty input: helper must return without touching the spans.
static void test_empty_input() {
    std::printf("[5] empty input\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> dst;
    std::vector<std::uint64_t> src;
    // Both empty -> no-op, no crash.
    detail::batch_xor_words(dst, src);
    TEST_ASSERT(dst.empty(),
                "batch_xor_words on empty dst+src must leave dst empty");
    // src empty, dst non-empty -> dst preserved.
    std::vector<std::uint64_t> dst_only = {0xABCDULL, 0x1234ULL};
    detail::batch_xor_words(dst_only, src);
    TEST_ASSERT(dst_only.size() == 2 && dst_only[0] == 0xABCDULL && dst_only[1] == 0x1234ULL,
                "batch_xor_words with empty src must preserve dst unchanged");
    // Also exercise scalar reference on empty input.
    detail::batch_xor_words_scalar(dst, src);
    TEST_ASSERT(dst.empty(),
                "scalar reference on empty dst+src must leave dst empty");
    TEST_PASS("empty input");
}

// Test 6 — single word XOR across hand-verified patterns.
static void test_single_word_patterns() {
    std::printf("[6] single word patterns\n");
    set_env_and_reload(nullptr);
    struct Case {
        std::uint64_t dst;
        std::uint64_t src;
        std::uint64_t expected;  // dst ^ src
    };
    Case cases[] = {
        // 0 XOR 0 = 0
        {0ULL, 0ULL, 0ULL},
        // a XOR a = 0 (identity)
        {0xFFULL, 0xFFULL, 0ULL},
        // 0xAA XOR 0x55 = 0xFF (disjoint bits union)
        {0xAAAA'AAAA'AAAA'AAAAULL, 0x5555'5555'5555'5555ULL, 0xFFFF'FFFF'FFFF'FFFFULL},
        // 0xFF XOR 0xAA = 0x55 (selective toggle)
        {0xFFFF'FFFF'FFFF'FFFFULL, 0xAAAA'AAAA'AAAA'AAAAULL, 0x5555'5555'5555'5555ULL},
        // 0 XOR x = x (zero is identity)
        {0ULL, 0xDEAD'BEEF'CAFE'BABEULL, 0xDEAD'BEEF'CAFE'BABEULL},
        // 0xFF...F XOR 0xFF...F = 0 (full identity)
        {0xFFFF'FFFF'FFFF'FFFFULL, 0xFFFF'FFFF'FFFF'FFFFULL, 0ULL},
        // High-bit + low-bit toggle
        {0x8000'0000'0000'0001ULL, 0xC000'0000'0000'0003ULL, 0x4000'0000'0000'0002ULL},
        // Cross-byte pattern
        {0x0F0F'0F0F'0F0F'0F0FULL, 0xF0F0'F0F0'F0F0'F0F0ULL, 0xFFFF'FFFF'FFFF'FFFFULL},
    };
    for (const auto& c : cases) {
        std::vector<std::uint64_t> dst{c.dst};
        std::vector<std::uint64_t> src{c.src};
        detail::batch_xor_words(dst, src);
        if (dst[0] != c.expected) {
            std::fprintf(stderr,
                "  dst=%016llx src=%016llx expected=%016llx got=%016llx\n",
                static_cast<unsigned long long>(c.dst),
                static_cast<unsigned long long>(c.src),
                static_cast<unsigned long long>(c.expected),
                static_cast<unsigned long long>(dst[0]));
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
    std::vector<std::uint64_t> dst(32);
    std::vector<std::uint64_t> src(32);
    std::mt19937_64 rng(0x1234ULL);
    for (auto& w : dst) w = rng();
    for (auto& w : src) w = rng();
    compare_xor(dst, src, "aligned batch 32 (random)");
}

// Test 8 — unaligned batch (size = 33 word): triggers the scalar tail
// after the SIMD-aligned prefix is consumed.
static void test_unaligned_batch_33() {
    std::printf("[8] unaligned batch (size=33)\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> dst(33);
    std::vector<std::uint64_t> src(33);
    std::mt19937_64 rng(0xabcdULL);
    for (auto& w : dst) w = rng();
    for (auto& w : src) w = rng();
    compare_xor(dst, src, "unaligned batch 33 (random)");
}

// Test 9 — large random batch (1000 word). Stresses the unrolled SIMD path
// across many iterations.
static void test_large_random_1000() {
    std::printf("[9] large random batch (size=1000)\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> dst(1000);
    std::vector<std::uint64_t> src(1000);
    std::mt19937_64 rng(0xcafeULL);
    for (auto& w : dst) w = rng();
    for (auto& w : src) w = rng();
    compare_xor(dst, src, "large random batch 1000");
}

// Test 10 — ForceOff vs Auto parity on a 1000-word random batch.
static void test_force_off_vs_auto_parity() {
    std::printf("[10] ForceOff vs Auto parity (1000 random)\n");
    std::vector<std::uint64_t> dst_init(1000);
    std::vector<std::uint64_t> src(1000);
    std::mt19937_64 rng(0xbeefULL);
    for (auto& w : dst_init) w = rng();
    for (auto& w : src) w = rng();

    // Auto path.
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> auto_dst = dst_init;
    detail::batch_xor_words(auto_dst, src);

    // Force-off path.
    set_env_and_reload("0");
    std::vector<std::uint64_t> off_dst = dst_init;
    detail::batch_xor_words(off_dst, src);

    TEST_ASSERT(words_equal(auto_dst, off_dst),
                "ForceOff and Auto batch outputs must match bit-for-bit");

    set_env_and_reload(nullptr);
    TEST_PASS("ForceOff vs Auto parity");
}

// Test 11 — identity: XOR with self produces all-zero output. Exercises
// every SIMD path lane and any horizontal-reduction quirks.
static void test_xor_with_self_yields_zero() {
    std::printf("[11] XOR with self -> all zeros\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> dst(128);
    std::mt19937_64 rng(0x3141ULL);
    for (auto& w : dst) w = rng();
    std::vector<std::uint64_t> src = dst;  // same content -> XOR -> 0
    detail::batch_xor_words(dst, src);
    for (std::size_t i = 0; i < dst.size(); ++i) {
        if (dst[i] != 0ULL) {
            std::fprintf(stderr,
                "  index %zu: expected 0, got %016llx (src=%016llx)\n",
                i,
                static_cast<unsigned long long>(dst[i]),
                static_cast<unsigned long long>(src[i]));
            tests_failed++;
            return;
        }
    }
    TEST_PASS("XOR with self -> all zeros");
}

// Test 12 — defensive contract: dst shorter than src. Only the first
// `dst.size()` words should be written; src tail must NOT cause any
// out-of-bounds access on dst.
static void test_dst_shorter_than_src() {
    std::printf("[12] dst shorter than src clamping\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> dst = {0xFFULL, 0xAAULL};        // size 2
    std::vector<std::uint64_t> src = {0x0FULL, 0x05ULL, 0x33ULL, 0x44ULL};  // size 4
    detail::batch_xor_words(dst, src);
    // dst[0] = 0xFF ^ 0x0F = 0xF0
    // dst[1] = 0xAA ^ 0x05 = 0xAF
    TEST_ASSERT(dst.size() == 2,
                "dst size must not change");
    TEST_ASSERT(dst[0] == 0xF0ULL && dst[1] == 0xAFULL,
                "dst shorter than src must clamp to first dst.size() words");
    TEST_PASS("dst shorter than src clamping");
}

// Test 13 — defensive contract: src shorter than dst. Only the first
// `src.size()` words of `dst` should be XORed; the dst tail must be
// preserved unchanged.
static void test_src_shorter_than_dst() {
    std::printf("[13] src shorter than dst tail preserved\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> dst = {0xFFULL, 0xAAULL, 0x55ULL, 0x11ULL};  // size 4
    std::vector<std::uint64_t> src = {0x0FULL, 0x05ULL};                    // size 2
    detail::batch_xor_words(dst, src);
    // dst[0] = 0xFF ^ 0x0F = 0xF0
    // dst[1] = 0xAA ^ 0x05 = 0xAF
    // dst[2] preserved = 0x55
    // dst[3] preserved = 0x11
    TEST_ASSERT(dst.size() == 4,
                "dst size must not change");
    TEST_ASSERT(dst[0] == 0xF0ULL && dst[1] == 0xAFULL,
                "first src.size() words of dst must be XORed");
    TEST_ASSERT(dst[2] == 0x55ULL && dst[3] == 0x11ULL,
                "dst tail beyond src.size() must be preserved unchanged");
    TEST_PASS("src shorter than dst tail preserved");
}

// Test 14 — perf-info probe (1M XOR cycles, SIMD vs scalar). Not a hard
// assertion on the timing; correctness equality is asserted.
static void test_perf_info_1m() {
    std::printf("[14] perf info (1M words, SIMD vs scalar)\n");
    constexpr std::size_t kN = 1'000'000;
    std::vector<std::uint64_t> dst_init(kN);
    std::vector<std::uint64_t> src(kN);
    std::mt19937_64 rng(0xdeadULL);
    for (auto& w : dst_init) w = rng();
    for (auto& w : src) w = rng();

    // Time the scalar reference path.
    set_env_and_reload("0");
    std::vector<std::uint64_t> scalar_dst = dst_init;
    auto start_scalar = std::chrono::steady_clock::now();
    detail::batch_xor_words(scalar_dst, src);
    auto end_scalar = std::chrono::steady_clock::now();
    double scalar_us = std::chrono::duration<double, std::micro>(
        end_scalar - start_scalar).count();

    // Time the SIMD (or scalar fallback if no SIMD) path.
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> simd_dst = dst_init;
    auto start_simd = std::chrono::steady_clock::now();
    detail::batch_xor_words(simd_dst, src);
    auto end_simd = std::chrono::steady_clock::now();
    double simd_us = std::chrono::duration<double, std::micro>(
        end_simd - start_simd).count();

    std::printf("    scalar=%.1f us simd=%.1f us\n", scalar_us, simd_us);
    TEST_ASSERT(words_equal(scalar_dst, simd_dst),
                "SIMD and scalar outputs must match on 1M words");
    set_env_and_reload(nullptr);
    TEST_PASS("perf info (1M words)");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    std::printf("=== test_xor_words_simd ===\n");
    std::printf("compile-time SIMD supported: %s\n",
                detail::xor_words_simd_supported() ? "yes" : "no");
    const char* env = std::getenv("GNFS_GF2_ROW_XOR_SIMD");
    std::printf("GNFS_GF2_ROW_XOR_SIMD = %s\n", env ? env : "(unset)");

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
    test_xor_with_self_yields_zero();
    test_dst_shorter_than_src();
    test_src_shorter_than_dst();
    test_perf_info_1m();

    std::printf("\n=== Summary ===\n");
    std::printf("  passed: %d\n", tests_passed);
    std::printf("  failed: %d\n", tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
