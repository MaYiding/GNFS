// test_threshold_scan_simd.cpp - Correctness tests for the SIMD-accelerated
// batch uint8_t threshold count helper.
//
// Strategy
// --------
// For each test that exercises the dispatcher path, the test runs the
// scalar reference (`count_above_threshold_u8_scalar`) and the dispatched
// helper (`count_above_threshold_u8`) on the same inputs, then asserts
// exact equality on the returned count. Since the count is a single
// `size_t` reduction, any divergence indicates a real kernel bug — we do
// not tolerate any difference across SIMD lanes.
//
// Additional coverage:
// * ENV parsing matrix (Auto / ForceOff / ForceOn / garbage variants).
// * Aligned chunk boundaries (NEON 16-lane / AVX2 32-lane multiples) plus
//   unaligned tails that exercise the scalar residual fall-through.
// * Threshold edge cases (0, 255, mid-range) on hand-crafted bytes.
// * All-equal / all-below / all-above edge cases.
// * ForceOff vs Auto parity on a sweep of thresholds.
// * 1M-byte perf-info probe (informational, also enforces parity).
//
// The build wires this test into the sieve test set as
// (ctest ThresholdScanSimd).

#include <gnfs/sieve/threshold_scan_simd.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do {                                \
    if (!(cond)) {                                                 \
        std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, msg); \
        tests_failed++;                                            \
        return;                                                    \
    }                                                              \
} while (0)

#define TEST_PASS(name) do {                                       \
    std::printf("  PASS: %s\n", name);                             \
    tests_passed++;                                                \
} while (0)

namespace sieve = gnfs::sieve;

// Setenv helper that flushes the ENV cache so the helper re-reads the value.
static void set_env_and_reload(const char* value) {
    if (value == nullptr) {
        ::unsetenv("GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD");
    } else {
        ::setenv("GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD", value, 1);
    }
    sieve::threshold_scan_simd_reset_env_cache_for_testing();
}

// Generic parity check used by most tests: assert SIMD vs scalar agree
// for the supplied span across one or more thresholds.
static void assert_parity(std::span<const std::uint8_t> values,
                          std::span<const std::uint8_t> thresholds,
                          const char* label) {
    for (std::uint8_t t : thresholds) {
        const std::size_t scalar =
            sieve::count_above_threshold_u8_scalar(values, t);
        const std::size_t simd =
            sieve::count_above_threshold_u8(values, t);
        if (scalar != simd) {
            std::fprintf(stderr,
                "  parity mismatch [%s] threshold=%u n=%zu scalar=%zu simd=%zu\n",
                label, static_cast<unsigned>(t), values.size(),
                scalar, simd);
            tests_failed++;
            return;
        }
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
    sieve::ThresholdScanSimdMode mode = sieve::threshold_scan_simd_mode();
    TEST_ASSERT(mode == sieve::ThresholdScanSimdMode::Auto,
                "unset env should resolve to Auto");
    const bool enabled = sieve::threshold_scan_simd_enabled();
    const bool supported = sieve::threshold_scan_simd_supported();
    std::printf("    supported=%d enabled=%d\n",
                supported ? 1 : 0, enabled ? 1 : 0);
    TEST_ASSERT(enabled == supported,
                "Auto mode must enable iff compile-time SIMD supported");
    TEST_PASS("env unset -> Auto + supported -> enabled");
}

// Test 2 — ENV "0" / "off" → ForceOff → always disabled even on SIMD host.
static void test_env_force_off() {
    std::printf("[2] env=0 -> ForceOff\n");
    set_env_and_reload("0");
    sieve::ThresholdScanSimdMode mode = sieve::threshold_scan_simd_mode();
    TEST_ASSERT(mode == sieve::ThresholdScanSimdMode::ForceOff,
                "env '0' should resolve to ForceOff");
    TEST_ASSERT(!sieve::threshold_scan_simd_enabled(),
                "ForceOff must disable the SIMD path");
    set_env_and_reload("off");
    TEST_ASSERT(sieve::threshold_scan_simd_mode() ==
                    sieve::ThresholdScanSimdMode::ForceOff,
                "env 'off' should resolve to ForceOff");
    TEST_ASSERT(!sieve::threshold_scan_simd_enabled(),
                "ForceOff (alias 'off') must disable the SIMD path");
    set_env_and_reload(nullptr);
    TEST_PASS("env=0/off -> ForceOff");
}

// Test 3 — ENV "1" / "on" → ForceOn.
static void test_env_force_on() {
    std::printf("[3] env=1 -> ForceOn\n");
    set_env_and_reload("1");
    sieve::ThresholdScanSimdMode mode = sieve::threshold_scan_simd_mode();
    TEST_ASSERT(mode == sieve::ThresholdScanSimdMode::ForceOn,
                "env '1' should resolve to ForceOn");
    const bool enabled = sieve::threshold_scan_simd_enabled();
    const bool supported = sieve::threshold_scan_simd_supported();
    TEST_ASSERT(enabled == supported,
                "ForceOn + supported must enable; ForceOn + unsupported must disable");
    set_env_and_reload("on");
    TEST_ASSERT(sieve::threshold_scan_simd_mode() ==
                    sieve::ThresholdScanSimdMode::ForceOn,
                "env 'on' should resolve to ForceOn");
    set_env_and_reload(nullptr);
    TEST_PASS("env=1/on -> ForceOn");
}

// Test 4 — ENV garbage / case-variants / mixed → Auto.
static void test_env_garbage_fallback() {
    std::printf("[4] env=auto / '' / garbage / case-variants -> Auto\n");
    for (const char* value :
         {"auto", "", "garbage", "2", "true", "On", "OFF", "01", "00", "Auto"}) {
        set_env_and_reload(value);
        sieve::ThresholdScanSimdMode mode = sieve::threshold_scan_simd_mode();
        if (mode != sieve::ThresholdScanSimdMode::Auto) {
            std::fprintf(stderr,
                "  unexpected mode for env='%s': mode=%d\n",
                value, static_cast<int>(mode));
            tests_failed++;
            return;
        }
    }
    set_env_and_reload(nullptr);
    TEST_PASS("env garbage values resolve to Auto");
}

// Test 5 — empty input returns 0 regardless of threshold.
static void test_empty_input() {
    std::printf("[5] empty input -> 0\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint8_t> empty;
    for (int t = 0; t <= 255; t += 16) {
        const std::size_t result =
            sieve::count_above_threshold_u8(empty, static_cast<std::uint8_t>(t));
        if (result != 0) {
            std::fprintf(stderr,
                "  unexpected count for empty input threshold=%d: %zu\n",
                t, result);
            tests_failed++;
            return;
        }
    }
    TEST_PASS("empty input -> 0");
}

// Test 6 — single byte below / at / above threshold (3-in-1 case).
static void test_single_byte() {
    std::printf("[6] single byte below/at/above threshold\n");
    set_env_and_reload(nullptr);

    // single byte below threshold (value < threshold)
    {
        std::vector<std::uint8_t> v = {99};
        TEST_ASSERT(sieve::count_above_threshold_u8(v, 100) == 0,
                    "single byte below threshold should yield 0");
    }
    // single byte at threshold (value == threshold, >= holds)
    {
        std::vector<std::uint8_t> v = {100};
        TEST_ASSERT(sieve::count_above_threshold_u8(v, 100) == 1,
                    "single byte at threshold should yield 1");
    }
    // single byte above threshold (value > threshold)
    {
        std::vector<std::uint8_t> v = {200};
        TEST_ASSERT(sieve::count_above_threshold_u8(v, 100) == 1,
                    "single byte above threshold should yield 1");
    }
    TEST_PASS("single byte below/at/above threshold");
}

// Test 7 — aligned 32-byte input (NEON: 2 full chunks; AVX2: 1 full chunk).
static void test_aligned_32() {
    std::printf("[7] aligned 32 bytes parity\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint8_t> v(32);
    std::mt19937 rng(0xC0FFEEU);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : v) b = static_cast<std::uint8_t>(byte_dist(rng));
    std::vector<std::uint8_t> ts = {0, 1, 64, 128, 192, 255};
    assert_parity(v, ts, "aligned 32 bytes parity");
}

// Test 8 — aligned 64-byte input (NEON: 4 full chunks; AVX2: 2 full chunks).
static void test_aligned_64() {
    std::printf("[8] aligned 64 bytes parity\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint8_t> v(64);
    std::mt19937 rng(0xDEADBEEFU);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : v) b = static_cast<std::uint8_t>(byte_dist(rng));
    std::vector<std::uint8_t> ts = {0, 32, 100, 128, 200, 255};
    assert_parity(v, ts, "aligned 64 bytes parity");
}

// Test 9 — unaligned 33-byte input (16+16+1 tail for NEON; 32+1 for AVX2).
static void test_unaligned_33() {
    std::printf("[9] unaligned 33 bytes tail handling\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint8_t> v(33);
    std::mt19937 rng(0xFEEDFACEU);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : v) b = static_cast<std::uint8_t>(byte_dist(rng));
    std::vector<std::uint8_t> ts = {0, 50, 128, 200, 255};
    assert_parity(v, ts, "unaligned 33 bytes tail");
}

// Test 10 — unaligned 65-byte input (NEON: 4 chunks + 1 tail; AVX2: 2 chunks + 1 tail).
static void test_unaligned_65() {
    std::printf("[10] unaligned 65 bytes tail handling\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint8_t> v(65);
    std::mt19937 rng(0x12345678U);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : v) b = static_cast<std::uint8_t>(byte_dist(rng));
    std::vector<std::uint8_t> ts = {0, 75, 150, 220, 255};
    assert_parity(v, ts, "unaligned 65 bytes tail");
}

// Test 11 — random 1024 bytes parity across threshold sweep.
static void test_random_1024_threshold_sweep() {
    std::printf("[11] random 1024 bytes x threshold sweep\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint8_t> v(1024);
    std::mt19937 rng(0xABCDEFU);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : v) b = static_cast<std::uint8_t>(byte_dist(rng));
    std::vector<std::uint8_t> ts = {0, 1, 16, 32, 64, 96, 128, 160, 192, 224, 254, 255};
    assert_parity(v, ts, "random 1024 bytes threshold sweep");
}

// Test 12 — edge cases: all-equal-threshold, all-below, all-above,
// threshold==0, threshold==255.
static void test_edge_cases() {
    std::printf("[12] edge cases: all-equal / all-below / all-above / t=0 / t=255\n");
    set_env_and_reload(nullptr);

    // all-equal-threshold (every byte == threshold, >= passes)
    {
        std::vector<std::uint8_t> v(40, 128);
        TEST_ASSERT(sieve::count_above_threshold_u8(v, 128) == 40,
                    "all-equal-threshold: every byte should pass");
    }
    // all-below (every byte < threshold)
    {
        std::vector<std::uint8_t> v(40, 50);
        TEST_ASSERT(sieve::count_above_threshold_u8(v, 100) == 0,
                    "all-below: nothing should pass");
    }
    // all-above (every byte > threshold)
    {
        std::vector<std::uint8_t> v(40, 200);
        TEST_ASSERT(sieve::count_above_threshold_u8(v, 100) == 40,
                    "all-above: every byte should pass");
    }
    // threshold == 0: every byte passes (count == size).
    {
        std::vector<std::uint8_t> v(50);
        std::mt19937 rng(0x42U);
        std::uniform_int_distribution<int> byte_dist(0, 255);
        for (auto& b : v) b = static_cast<std::uint8_t>(byte_dist(rng));
        TEST_ASSERT(sieve::count_above_threshold_u8(v, 0) == 50,
                    "threshold=0: every byte must pass");
    }
    // threshold == 255: only bytes equal to 0xFF pass.
    {
        std::vector<std::uint8_t> v = {
            0x00, 0xFF, 0x80, 0xFF, 0x7F, 0xFF, 0xFE, 0xFF
        };
        // 4 bytes equal to 0xFF (>= 255).
        TEST_ASSERT(sieve::count_above_threshold_u8(v, 255) == 4,
                    "threshold=255: only 0xFF bytes pass");
    }
    TEST_PASS("edge cases (all-equal / all-below / all-above / t=0 / t=255)");
}

// Test 13 — ForceOff vs Auto parity on a 500-byte random batch across
// thresholds; the Auto path drives the SIMD code on SIMD hosts while
// the ForceOff path is locked to scalar. The two must agree.
static void test_force_off_vs_auto_parity() {
    std::printf("[13] ForceOff vs Auto parity (500 random bytes, threshold sweep)\n");
    std::vector<std::uint8_t> v(500);
    std::mt19937 rng(0xDEAD0123U);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : v) b = static_cast<std::uint8_t>(byte_dist(rng));

    std::vector<std::uint8_t> ts = {0, 32, 64, 96, 128, 160, 192, 224, 255};

    // Auto path counts.
    set_env_and_reload(nullptr);
    std::vector<std::size_t> auto_counts;
    auto_counts.reserve(ts.size());
    for (std::uint8_t t : ts) {
        auto_counts.push_back(sieve::count_above_threshold_u8(v, t));
    }

    // ForceOff (scalar) counts.
    set_env_and_reload("0");
    std::vector<std::size_t> off_counts;
    off_counts.reserve(ts.size());
    for (std::uint8_t t : ts) {
        off_counts.push_back(sieve::count_above_threshold_u8(v, t));
    }

    for (std::size_t k = 0; k < ts.size(); ++k) {
        if (auto_counts[k] != off_counts[k]) {
            std::fprintf(stderr,
                "  parity mismatch threshold=%u auto=%zu off=%zu\n",
                static_cast<unsigned>(ts[k]), auto_counts[k], off_counts[k]);
            tests_failed++;
            return;
        }
    }

    set_env_and_reload(nullptr);
    TEST_PASS("ForceOff vs Auto parity (500 bytes, threshold sweep)");
}

// Test 14 — perf-info probe (1M bytes). Not an assertion on the timing;
// the test asserts SIMD vs scalar parity on the full result.
static void test_perf_info_1m() {
    std::printf("[14] perf info (1M bytes)\n");
    constexpr std::size_t kN = 1'000'000;
    std::vector<std::uint8_t> v(kN);
    std::mt19937 rng(0xACE0U);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : v) b = static_cast<std::uint8_t>(byte_dist(rng));
    const std::uint8_t threshold = 200;

    // Scalar (ForceOff) timing.
    set_env_and_reload("0");
    auto t0 = std::chrono::steady_clock::now();
    const std::size_t scalar_count =
        sieve::count_above_threshold_u8(v, threshold);
    auto t1 = std::chrono::steady_clock::now();
    double scalar_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

    // SIMD (Auto) timing.
    set_env_and_reload(nullptr);
    auto t2 = std::chrono::steady_clock::now();
    const std::size_t simd_count =
        sieve::count_above_threshold_u8(v, threshold);
    auto t3 = std::chrono::steady_clock::now();
    double simd_us = std::chrono::duration<double, std::micro>(t3 - t2).count();

    std::printf("    scalar=%.1f us simd=%.1f us count=%zu\n",
                scalar_us, simd_us, simd_count);
    TEST_ASSERT(scalar_count == simd_count,
                "SIMD and scalar must agree on 1M-byte count");
    set_env_and_reload(nullptr);
    TEST_PASS("perf info (1M bytes)");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    std::printf("=== test_threshold_scan_simd ===\n");
    std::printf("compile-time SIMD supported: %s\n",
                sieve::threshold_scan_simd_supported() ? "yes" : "no");
    const char* env = std::getenv("GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD");
    std::printf("GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD = %s\n",
                env ? env : "(unset)");

    test_env_unset_auto();
    test_env_force_off();
    test_env_force_on();
    test_env_garbage_fallback();
    test_empty_input();
    test_single_byte();
    test_aligned_32();
    test_aligned_64();
    test_unaligned_33();
    test_unaligned_65();
    test_random_1024_threshold_sweep();
    test_edge_cases();
    test_force_off_vs_auto_parity();
    test_perf_info_1m();

    std::printf("\n=== Summary ===\n");
    std::printf("  passed: %d\n", tests_passed);
    std::printf("  failed: %d\n", tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
