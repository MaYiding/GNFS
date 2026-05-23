// test_xor_popcnt_simd.cpp - Correctness tests for the SIMD-accelerated GF(2)
// batch XOR-then-popcount (Hamming distance) helpers.
//
// Strategy
// --------
// Every test that exercises the SIMD path runs both the scalar reference
// (`__builtin_popcountll(a ^ b)` per word) and the dispatched helper, then
// asserts per-index equality. `popcount(a ^ b)` is a pure function of the
// two input words, so any divergence indicates a real kernel bug — we do
// not tolerate any difference across SIMD lanes.
//
// Additional coverage:
// * ENV parsing (GNFS_GF2_XOR_POPCNT_SIMD = 0 / 1 / auto / unset / garbage).
// * Aligned vs unaligned batch sizes — the SIMD path must fall through
//   cleanly to the scalar residual tail.
// * total_xor_popcount_words: sum reduction equivalence.
// * Defensive contract: undersized `out` clamps without UB; mismatched
//   `a` / `b` sizes trip the debug assert.
// * a == b identity: `popcount(a ^ a) = 0` for every word (sanity check
//   distinguishing XOR-popcount from OR/AND/plain popcount).
//
// The build wires this test into the linalg test set
// (ctest XorPopcntSIMD).

#include <gnfs/linalg/detail/xor_popcnt_simd.hpp>

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
        ::unsetenv("GNFS_GF2_XOR_POPCNT_SIMD");
    } else {
        ::setenv("GNFS_GF2_XOR_POPCNT_SIMD", value, 1);
    }
    detail::xor_popcnt_simd_reset_env_cache_for_testing();
}

static bool batch_results_equal(const std::vector<std::uint32_t>& a,
                                const std::vector<std::uint32_t>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

// Compare scalar reference against the dispatched helper on the same
// (a, b) input. Tests both the per-word batch entry and the total
// reduction entry.
static void compare_batch(const std::vector<std::uint64_t>& a,
                          const std::vector<std::uint64_t>& b,
                          const char* label) {
    if (a.size() != b.size()) {
        std::fprintf(stderr,
            "  compare_batch internal error: a.size=%zu b.size=%zu\n",
            a.size(), b.size());
        tests_failed++;
        return;
    }
    std::vector<std::uint32_t> scalar_out(a.size(), 0);
    std::vector<std::uint32_t> dispatch_out(a.size(), 0);
    detail::batch_xor_popcount_words_scalar(a, b, scalar_out);
    detail::batch_xor_popcount_words(a, b, dispatch_out);
    if (!batch_results_equal(scalar_out, dispatch_out)) {
        std::fprintf(stderr, "  batch mismatch [%s] n=%zu\n",
                     label, a.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (scalar_out[i] != dispatch_out[i]) {
                std::fprintf(stderr,
                    "    index %zu: a=%016llx b=%016llx scalar=%u dispatch=%u\n",
                    i,
                    static_cast<unsigned long long>(a[i]),
                    static_cast<unsigned long long>(b[i]),
                    scalar_out[i], dispatch_out[i]);
                break;
            }
        }
        tests_failed++;
        return;
    }
    // Total reduction parity.
    std::uint64_t scalar_total = detail::total_xor_popcount_words_scalar(a, b);
    std::uint64_t dispatch_total = detail::total_xor_popcount_words(a, b);
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
static void test_env_unset_auto() {
    std::printf("[1] env unset -> Auto + supported -> enabled\n");
    set_env_and_reload(nullptr);
    detail::XorPopcntSimdMode mode = detail::xor_popcnt_simd_mode();
    TEST_ASSERT(mode == detail::XorPopcntSimdMode::Auto,
                "unset env should resolve to Auto");
    const bool enabled = detail::xor_popcnt_simd_enabled();
    const bool supported = detail::xor_popcnt_simd_supported();
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
    detail::XorPopcntSimdMode mode = detail::xor_popcnt_simd_mode();
    TEST_ASSERT(mode == detail::XorPopcntSimdMode::ForceOff,
                "env '0' should resolve to ForceOff");
    TEST_ASSERT(!detail::xor_popcnt_simd_enabled(),
                "ForceOff must disable the SIMD path");
    TEST_PASS("env=0 -> ForceOff");
}

// Test 3 — ENV "1" → ForceOn. When supported, enables SIMD; when not
// supported, falls back to scalar but the gate reports `enabled() == supported`.
static void test_env_force_on() {
    std::printf("[3] env=1 -> ForceOn\n");
    set_env_and_reload("1");
    detail::XorPopcntSimdMode mode = detail::xor_popcnt_simd_mode();
    TEST_ASSERT(mode == detail::XorPopcntSimdMode::ForceOn,
                "env '1' should resolve to ForceOn");
    const bool enabled = detail::xor_popcnt_simd_enabled();
    const bool supported = detail::xor_popcnt_simd_supported();
    TEST_ASSERT(enabled == supported,
                "ForceOn + supported must enable; ForceOn + unsupported must disable");
    TEST_PASS("env=1 -> ForceOn");
}

// Test 4 — ENV "auto" / "" / "garbage" / "2" / "true" / "00" / "01" → Auto.
static void test_env_garbage_fallback() {
    std::printf("[4] env=auto / '' / garbage -> Auto\n");
    for (const char* value : {"auto", "", "garbage", "2", "true", "00", "01"}) {
        set_env_and_reload(value);
        detail::XorPopcntSimdMode mode = detail::xor_popcnt_simd_mode();
        if (mode != detail::XorPopcntSimdMode::Auto) {
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

// Test 5 — empty input: both APIs must return without touching the spans.
static void test_empty_input() {
    std::printf("[5] empty input\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> a;
    std::vector<std::uint64_t> b;
    std::vector<std::uint32_t> out;
    detail::batch_xor_popcount_words(a, b, out);
    TEST_ASSERT(out.empty(),
                "batch_xor_popcount_words on empty input must leave out empty");
    std::uint64_t total = detail::total_xor_popcount_words(a, b);
    TEST_ASSERT(total == 0,
                "total_xor_popcount_words on empty input must be zero");
    // Also exercise scalar reference on empty input.
    detail::batch_xor_popcount_words_scalar(a, b, out);
    TEST_ASSERT(out.empty(),
                "scalar reference on empty input must leave out empty");
    TEST_ASSERT(detail::total_xor_popcount_words_scalar(a, b) == 0,
                "scalar total on empty input must be zero");
    TEST_PASS("empty input");
}

// Test 6 — single word XOR-popcount across hand-verified bit patterns.
// Critical for catching kernel bugs that confuse XOR with AND/OR.
static void test_single_word_patterns() {
    std::printf("[6] single word patterns\n");
    set_env_and_reload(nullptr);
    struct Case {
        std::uint64_t a;
        std::uint64_t b;
        std::uint32_t expected;  // popcount(a ^ b)
    };
    Case cases[] = {
        // Both zero -> 0 (no differing bits)
        {0ULL, 0ULL, 0},
        // a=0xFF, b=0xFF -> 0 (identical, no differing bits)
        {0xFFULL, 0xFFULL, 0},
        // Disjoint bit patterns: 0xAA ^ 0x55 = 0xFF -> all bits differ
        // 0xAAAA..AAAA ^ 0x5555..5555 = 0xFFFF..FFFF -> 64 differing bits
        {0xAAAA'AAAA'AAAA'AAAAULL, 0x5555'5555'5555'5555ULL, 64},
        // Identical 32-bit mask: 0xAA ^ 0xAA = 0 -> 0 differing bits
        {0xAAAA'AAAA'AAAA'AAAAULL, 0xAAAA'AAAA'AAAA'AAAAULL, 0},
        // All-ones XOR all-ones = 0 -> 0 (identity check)
        {0xFFFF'FFFF'FFFF'FFFFULL, 0xFFFF'FFFF'FFFF'FFFFULL, 0},
        // a=all-ones, b=0xAA pattern -> 0xFF..FF ^ 0xAA..AA = 0x55..55 -> 32 differing bits
        {0xFFFF'FFFF'FFFF'FFFFULL, 0xAAAA'AAAA'AAAA'AAAAULL, 32},
        // High-bit + low-bit patterns: 0x8000...01 ^ 0xC000...03
        // = 0x4000...02 -> popcount 2 (bit 62 differs, bit 1 differs)
        {0x8000'0000'0000'0001ULL, 0xC000'0000'0000'0003ULL, 2},
        // Sparse complement: 0x0F0F ^ 0xF0F0 = 0xFFFF -> 64 differing bits
        {0x0F0F'0F0F'0F0F'0F0FULL, 0xF0F0'F0F0'F0F0'F0F0ULL, 64},
        // Single-bit difference: 0x0 ^ 0x1 = 0x1 -> 1 differing bit
        {0x0ULL, 0x1ULL, 1},
        // Hamming distance 1 across high bit: 0x0 vs 0x8000...0 -> 1
        {0x0ULL, 0x8000'0000'0000'0000ULL, 1},
    };
    for (const auto& c : cases) {
        std::vector<std::uint64_t> a{c.a};
        std::vector<std::uint64_t> b{c.b};
        std::vector<std::uint32_t> out(1, 0);
        detail::batch_xor_popcount_words(a, b, out);
        if (out[0] != c.expected) {
            std::fprintf(stderr,
                "  a=%016llx b=%016llx expected=%u got=%u\n",
                static_cast<unsigned long long>(c.a),
                static_cast<unsigned long long>(c.b),
                c.expected, out[0]);
            tests_failed++;
            return;
        }
        // Builtin cross-check.
        std::uint32_t builtin = static_cast<std::uint32_t>(__builtin_popcountll(c.a ^ c.b));
        if (out[0] != builtin) {
            std::fprintf(stderr,
                "  a=%016llx b=%016llx expected=%u dispatch=%u builtin=%u\n",
                static_cast<unsigned long long>(c.a),
                static_cast<unsigned long long>(c.b),
                c.expected, out[0], builtin);
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
    compare_batch(a, b, "aligned batch 32 (random)");
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
    compare_batch(a, b, "unaligned batch 33 (random)");
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
    compare_batch(a, b, "large random batch 1000");
}

// Test 10 — total XOR-popcount (Hamming distance) equivalence across a
// battery of sizes.
static void test_total_xor_popcount_equivalence() {
    std::printf("[10] total XOR-popcount equivalence\n");
    set_env_and_reload(nullptr);
    std::mt19937_64 rng(0xfeedULL);
    for (std::size_t n : {std::size_t{1}, std::size_t{2}, std::size_t{3},
                          std::size_t{4}, std::size_t{8}, std::size_t{16},
                          std::size_t{31}, std::size_t{32}, std::size_t{63},
                          std::size_t{64}, std::size_t{100}, std::size_t{1000}}) {
        std::vector<std::uint64_t> a(n);
        std::vector<std::uint64_t> b(n);
        for (auto& w : a) w = rng();
        for (auto& w : b) w = rng();
        // Per-word sum vs total entry point.
        std::vector<std::uint32_t> per_word(n, 0);
        detail::batch_xor_popcount_words(a, b, per_word);
        std::uint64_t per_word_sum = 0;
        for (auto v : per_word) per_word_sum += v;
        std::uint64_t total = detail::total_xor_popcount_words(a, b);
        if (per_word_sum != total) {
            std::fprintf(stderr,
                "  total mismatch n=%zu per_word_sum=%llu total=%llu\n",
                n, static_cast<unsigned long long>(per_word_sum),
                static_cast<unsigned long long>(total));
            tests_failed++;
            return;
        }
        // Scalar reference total must equal both.
        std::uint64_t scalar_total = detail::total_xor_popcount_words_scalar(a, b);
        if (scalar_total != total) {
            std::fprintf(stderr,
                "  scalar/dispatch total mismatch n=%zu scalar=%llu dispatch=%llu\n",
                n, static_cast<unsigned long long>(scalar_total),
                static_cast<unsigned long long>(total));
            tests_failed++;
            return;
        }
    }
    TEST_PASS("total XOR-popcount equivalence");
}

// Test 11 — ForceOff vs Auto parity on a 1000-word random batch.
static void test_force_off_vs_auto_parity() {
    std::printf("[11] ForceOff vs Auto parity (1000 random)\n");
    std::vector<std::uint64_t> a(1000);
    std::vector<std::uint64_t> b(1000);
    std::mt19937_64 rng(0xbeefULL);
    for (auto& w : a) w = rng();
    for (auto& w : b) w = rng();

    // Auto path.
    set_env_and_reload(nullptr);
    std::vector<std::uint32_t> auto_out(1000, 0);
    detail::batch_xor_popcount_words(a, b, auto_out);
    std::uint64_t auto_total = detail::total_xor_popcount_words(a, b);

    // Force-off path.
    set_env_and_reload("0");
    std::vector<std::uint32_t> off_out(1000, 0);
    detail::batch_xor_popcount_words(a, b, off_out);
    std::uint64_t off_total = detail::total_xor_popcount_words(a, b);

    TEST_ASSERT(batch_results_equal(auto_out, off_out),
                "ForceOff and Auto batch outputs must match bit-for-bit");
    TEST_ASSERT(auto_total == off_total,
                "ForceOff and Auto totals must match bit-for-bit");

    set_env_and_reload(nullptr);
    TEST_PASS("ForceOff vs Auto parity");
}

// Test 12 — perf-info probe (1M words, SIMD vs scalar). Not a hard
// assertion; correctness equality is asserted.
static void test_perf_info_1m() {
    std::printf("[12] perf info (1M words, SIMD vs scalar)\n");
    constexpr std::size_t kN = 1'000'000;
    std::vector<std::uint64_t> a(kN);
    std::vector<std::uint64_t> b(kN);
    std::mt19937_64 rng(0xdeadULL);
    for (auto& w : a) w = rng();
    for (auto& w : b) w = rng();

    // Time the scalar reference path.
    set_env_and_reload("0");
    auto start_scalar = std::chrono::steady_clock::now();
    std::uint64_t scalar_total = detail::total_xor_popcount_words(a, b);
    auto end_scalar = std::chrono::steady_clock::now();
    double scalar_us = std::chrono::duration<double, std::micro>(
        end_scalar - start_scalar).count();

    // Time the SIMD (or scalar fallback if no SIMD) path.
    set_env_and_reload(nullptr);
    auto start_simd = std::chrono::steady_clock::now();
    std::uint64_t simd_total = detail::total_xor_popcount_words(a, b);
    auto end_simd = std::chrono::steady_clock::now();
    double simd_us = std::chrono::duration<double, std::micro>(
        end_simd - start_simd).count();

    std::printf("    scalar=%.1f us simd=%.1f us total=%llu\n",
                scalar_us, simd_us,
                static_cast<unsigned long long>(simd_total));
    TEST_ASSERT(scalar_total == simd_total,
                "SIMD and scalar totals must match on 1M words");
    set_env_and_reload(nullptr);
    TEST_PASS("perf info (1M words)");
}

// Test 13 — defensive contract: when caller passes a smaller `out` than
// `a`, the helper must clamp to `out.size()` and not write past the
// buffer.
static void test_undersized_out_span() {
    std::printf("[13] undersized out span clamping\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> a = {0xFFULL, 0xAAULL, 0x55ULL, 0xFFFF'FFFF'FFFF'FFFFULL};
    std::vector<std::uint64_t> b = {0x0FULL, 0x55ULL, 0xAAULL, 0x0000'0000'0000'0000ULL};
    // popcount(0xFF ^ 0x0F) = popcount(0xF0) = 4
    // popcount(0xAA ^ 0x55) = popcount(0xFF) = 8
    std::vector<std::uint32_t> small_out(2, 0);
    detail::batch_xor_popcount_words(a, b, small_out);
    TEST_ASSERT(small_out[0] == 4 && small_out[1] == 8,
                "undersized out must contain the first 2 XOR-popcounts");
    set_env_and_reload(nullptr);
    TEST_PASS("undersized out span clamping");
}

// Test 14 — same-input identity: a == b means popcount(a ^ a) = 0 for
// every word. Critical sanity check distinguishing XOR from AND
// (AND-popcount of a, a would give popcount(a); XOR gives 0). Catches
// any accidental kernel swap of XOR for AND/OR/plain popcount.
static void test_self_xor_zero_identity() {
    std::printf("[14] a==b -> all-zero XOR-popcount identity\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint64_t> a(128);
    std::mt19937_64 rng(0x3141ULL);
    for (auto& w : a) w = rng();
    std::vector<std::uint64_t> b = a;  // identical
    std::vector<std::uint32_t> dispatch_out(128, 0xDEADBEEF);  // sentinel
    detail::batch_xor_popcount_words(a, b, dispatch_out);
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (dispatch_out[i] != 0) {
            std::fprintf(stderr,
                "  a==b[%zu]=%016llx expected popcount(a^a)=0 got=%u\n",
                i, static_cast<unsigned long long>(a[i]),
                dispatch_out[i]);
            tests_failed++;
            return;
        }
    }
    std::uint64_t total = detail::total_xor_popcount_words(a, b);
    TEST_ASSERT(total == 0,
                "total_xor_popcount(a, a) must equal 0 (self-XOR identity)");
    TEST_PASS("a==b -> all-zero XOR-popcount identity");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    std::printf("=== test_xor_popcnt_simd ===\n");
    std::printf("compile-time SIMD supported: %s\n",
                detail::xor_popcnt_simd_supported() ? "yes" : "no");
    const char* env = std::getenv("GNFS_GF2_XOR_POPCNT_SIMD");
    std::printf("GNFS_GF2_XOR_POPCNT_SIMD = %s\n", env ? env : "(unset)");

    test_env_unset_auto();
    test_env_force_off();
    test_env_force_on();
    test_env_garbage_fallback();
    test_empty_input();
    test_single_word_patterns();
    test_aligned_batch_32();
    test_unaligned_batch_33();
    test_large_random_1000();
    test_total_xor_popcount_equivalence();
    test_force_off_vs_auto_parity();
    test_perf_info_1m();
    test_undersized_out_span();
    test_self_xor_zero_identity();

    std::printf("\n=== Summary ===\n");
    std::printf("  passed: %d\n", tests_passed);
    std::printf("  failed: %d\n", tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
