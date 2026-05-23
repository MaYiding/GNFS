// test_saturated_sub_simd.cpp - Correctness tests for the SIMD-accelerated
// batch uint8_t saturated subtract helper.
//
// Strategy
// --------
// For each test that exercises the dispatcher path, the test runs the
// scalar reference (`saturated_sub_u8_batch_scalar`) on a fresh copy
// of the input and the dispatched helper (`saturated_sub_u8_batch`)
// on the original buffer, then asserts byte-by-byte equality on the
// mutated span. Since saturating subtract is a pure per-byte function,
// any divergence indicates a real kernel bug — we do not tolerate any
// difference across SIMD lanes.
//
// Additional coverage:
// * ENV parsing matrix (Auto / ForceOff / ForceOn / garbage variants).
// * Aligned chunk boundaries (NEON 16-lane / AVX2 32-lane multiples) plus
//   unaligned tails that exercise the scalar residual fall-through.
// * Bias edge cases (0, 128, 255) on hand-crafted bytes and the
//   bias=0 zero-write contract.
// * Random byte + bias sweep to cover (v, bias) cross product.
// * ForceOff vs Auto parity.
// * 1M-byte perf-info probe (informational, also enforces parity).

#include <gnfs/sieve/saturated_sub_simd.hpp>

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
        ::unsetenv("GNFS_SIEVE_SATURATED_SUB_SIMD");
    } else {
        ::setenv("GNFS_SIEVE_SATURATED_SUB_SIMD", value, 1);
    }
    sieve::saturated_sub_simd_reset_env_cache_for_testing();
}

// Run scalar reference and dispatcher (SIMD when supported + Auto) on
// independent copies of the same input, then assert the mutated bytes
// match exactly. Returns true on parity, false on mismatch (caller
// already recorded the failure).
static bool assert_parity(const std::vector<std::uint8_t>& base_input,
                          std::uint8_t bias,
                          const char* label) {
    std::vector<std::uint8_t> scalar_buf = base_input;
    std::vector<std::uint8_t> simd_buf = base_input;
    sieve::saturated_sub_u8_batch_scalar(scalar_buf, bias);
    sieve::saturated_sub_u8_batch(simd_buf, bias);
    for (std::size_t i = 0; i < scalar_buf.size(); ++i) {
        if (scalar_buf[i] != simd_buf[i]) {
            std::fprintf(stderr,
                "  parity mismatch [%s] bias=%u i=%zu scalar=%u simd=%u\n",
                label, static_cast<unsigned>(bias), i,
                static_cast<unsigned>(scalar_buf[i]),
                static_cast<unsigned>(simd_buf[i]));
            tests_failed++;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Test 1 — ENV unset → Auto mode → enabled when SIMD supported.
static void test_env_unset_auto() {
    std::printf("[1] env unset -> Auto + supported -> enabled\n");
    set_env_and_reload(nullptr);
    sieve::SaturatedSubSimdMode mode = sieve::saturated_sub_simd_mode();
    TEST_ASSERT(mode == sieve::SaturatedSubSimdMode::Auto,
                "unset env should resolve to Auto");
    const bool enabled = sieve::saturated_sub_simd_enabled();
    const bool supported = sieve::saturated_sub_simd_supported();
    std::printf("    supported=%d enabled=%d\n",
                supported ? 1 : 0, enabled ? 1 : 0);
    TEST_ASSERT(enabled == supported,
                "Auto mode must enable iff compile-time SIMD supported");
    TEST_PASS("env unset -> Auto + supported -> enabled");
}

// Test 2 — ENV "0" / "off" → ForceOff → always disabled even on SIMD host.
static void test_env_force_off() {
    std::printf("[2] env=0/off -> ForceOff\n");
    set_env_and_reload("0");
    sieve::SaturatedSubSimdMode mode = sieve::saturated_sub_simd_mode();
    TEST_ASSERT(mode == sieve::SaturatedSubSimdMode::ForceOff,
                "env '0' should resolve to ForceOff");
    TEST_ASSERT(!sieve::saturated_sub_simd_enabled(),
                "ForceOff must disable the SIMD path");
    set_env_and_reload("off");
    TEST_ASSERT(sieve::saturated_sub_simd_mode() ==
                    sieve::SaturatedSubSimdMode::ForceOff,
                "env 'off' should resolve to ForceOff");
    TEST_ASSERT(!sieve::saturated_sub_simd_enabled(),
                "ForceOff (alias 'off') must disable the SIMD path");
    set_env_and_reload(nullptr);
    TEST_PASS("env=0/off -> ForceOff");
}

// Test 3 — ENV "1" / "on" → ForceOn.
static void test_env_force_on() {
    std::printf("[3] env=1/on -> ForceOn\n");
    set_env_and_reload("1");
    sieve::SaturatedSubSimdMode mode = sieve::saturated_sub_simd_mode();
    TEST_ASSERT(mode == sieve::SaturatedSubSimdMode::ForceOn,
                "env '1' should resolve to ForceOn");
    const bool enabled = sieve::saturated_sub_simd_enabled();
    const bool supported = sieve::saturated_sub_simd_supported();
    TEST_ASSERT(enabled == supported,
                "ForceOn + supported must enable; ForceOn + unsupported must disable");
    set_env_and_reload("on");
    TEST_ASSERT(sieve::saturated_sub_simd_mode() ==
                    sieve::SaturatedSubSimdMode::ForceOn,
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
        sieve::SaturatedSubSimdMode mode = sieve::saturated_sub_simd_mode();
        if (mode != sieve::SaturatedSubSimdMode::Auto) {
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

// Test 5 — empty input is a no-op for every bias.
static void test_empty_input() {
    std::printf("[5] empty input -> no-op\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint8_t> empty;
    for (int bias = 0; bias <= 255; bias += 16) {
        sieve::saturated_sub_u8_batch(empty, static_cast<std::uint8_t>(bias));
        if (!empty.empty()) {
            std::fprintf(stderr,
                "  empty input mutated by bias=%d (size now %zu)\n",
                bias, empty.size());
            tests_failed++;
            return;
        }
    }
    TEST_PASS("empty input -> no-op");
}

// Test 6 — single byte across 9 (value, bias) combinations.
static void test_single_byte_combinations() {
    std::printf("[6] single byte: 3 values x 3 biases\n");
    set_env_and_reload(nullptr);
    struct Case {
        std::uint8_t v;
        std::uint8_t bias;
        std::uint8_t expected;
    };
    const Case cases[] = {
        // v=0
        {0, 0,   0},   // identity (bias=0 short-circuited)
        {0, 128, 0},   // saturate to 0
        {0, 255, 0},   // saturate to 0
        // v=127
        {127, 0,   127},  // identity
        {127, 128, 0},    // 127 < 128 → 0
        {127, 255, 0},    // saturate to 0
        // v=255
        {255, 0,   255},  // identity
        {255, 128, 127},  // 255 - 128 = 127
        {255, 255, 0},    // 255 - 255 = 0
    };
    for (const Case& c : cases) {
        std::vector<std::uint8_t> v = {c.v};
        sieve::saturated_sub_u8_batch(v, c.bias);
        if (v[0] != c.expected) {
            std::fprintf(stderr,
                "  fail: v=%u bias=%u expected=%u got=%u\n",
                static_cast<unsigned>(c.v),
                static_cast<unsigned>(c.bias),
                static_cast<unsigned>(c.expected),
                static_cast<unsigned>(v[0]));
            tests_failed++;
            return;
        }
    }
    TEST_PASS("single byte: 3 values x 3 biases");
}

// Test 7 — bias=0 contract: zero writes to memory + input preserved.
static void test_bias_zero_noop() {
    std::printf("[7] bias=0 -> no-op (zero writes)\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint8_t> v(100);
    std::mt19937 rng(0xABCDU);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : v) b = static_cast<std::uint8_t>(byte_dist(rng));
    std::vector<std::uint8_t> snapshot = v;

    sieve::saturated_sub_u8_batch(v, 0);

    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] != snapshot[i]) {
            std::fprintf(stderr,
                "  bias=0 mutated index %zu: before=%u after=%u\n",
                i, static_cast<unsigned>(snapshot[i]),
                static_cast<unsigned>(v[i]));
            tests_failed++;
            return;
        }
    }
    TEST_PASS("bias=0 -> no-op");
}

// Test 8 — bias=255 collapses every byte to 0.
static void test_bias_255_collapse() {
    std::printf("[8] bias=255 -> all bytes collapse to 0\n");
    set_env_and_reload(nullptr);

    // Mix of small / mid / max values including 0xFF.
    std::vector<std::uint8_t> v = {
        0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF,
        0x00, 0xFF, 0x55, 0xAA, 0x10, 0xF0,
        0x00, 0x01, 0xFF, 0xFE, 0x55, 0xAA,
        0x00, 0xFF, 0x10, 0xF0, 0x80, 0x7F,
        0x33, 0xCC, 0x99, 0x66, 0x44, 0xBB,
        0x22, 0xDD, 0x11, 0xEE, 0x88, 0x77,
    };
    const std::size_t orig_size = v.size();
    sieve::saturated_sub_u8_batch(v, 255);
    TEST_ASSERT(v.size() == orig_size,
                "bias=255 must not resize the span");
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] != 0) {
            std::fprintf(stderr,
                "  bias=255 left non-zero byte at index %zu: %u\n",
                i, static_cast<unsigned>(v[i]));
            tests_failed++;
            return;
        }
    }
    TEST_PASS("bias=255 -> all bytes collapse to 0");
}

// Test 9 — aligned 32-byte input parity (NEON: 2 full chunks; AVX2: 1).
static void test_aligned_32() {
    std::printf("[9] aligned 32 bytes parity\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint8_t> v(32);
    std::mt19937 rng(0xC0FFEEU);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : v) b = static_cast<std::uint8_t>(byte_dist(rng));
    std::vector<std::uint8_t> biases = {1, 64, 128, 192, 254};
    for (std::uint8_t bias : biases) {
        if (!assert_parity(v, bias, "aligned 32 bytes")) return;
    }
    TEST_PASS("aligned 32 bytes parity");
}

// Test 10 — unaligned 33-byte input (NEON: 16+16+1 tail; AVX2: 32+1 tail).
static void test_unaligned_33() {
    std::printf("[10] unaligned 33 bytes parity (tail handling)\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint8_t> v(33);
    std::mt19937 rng(0xFEEDFACEU);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : v) b = static_cast<std::uint8_t>(byte_dist(rng));
    std::vector<std::uint8_t> biases = {1, 50, 128, 200, 254};
    for (std::uint8_t bias : biases) {
        if (!assert_parity(v, bias, "unaligned 33 bytes")) return;
    }
    TEST_PASS("unaligned 33 bytes parity (tail handling)");
}

// Test 11 — unaligned 65-byte input (NEON: 4 chunks + 1 tail; AVX2: 2 + 1 tail).
static void test_unaligned_65() {
    std::printf("[11] unaligned 65 bytes parity (tail handling)\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint8_t> v(65);
    std::mt19937 rng(0x12345678U);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : v) b = static_cast<std::uint8_t>(byte_dist(rng));
    std::vector<std::uint8_t> biases = {1, 75, 128, 220, 254};
    for (std::uint8_t bias : biases) {
        if (!assert_parity(v, bias, "unaligned 65 bytes")) return;
    }
    TEST_PASS("unaligned 65 bytes parity (tail handling)");
}

// Test 12 — random 1000 bytes x bias 1..255 sweep parity.
static void test_random_1000_bias_sweep() {
    std::printf("[12] random 1000 bytes x bias sweep parity\n");
    set_env_and_reload(nullptr);
    std::vector<std::uint8_t> v(1000);
    std::mt19937 rng(0xABCDEFU);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : v) b = static_cast<std::uint8_t>(byte_dist(rng));
    // Skip bias=0 (separate test); sweep biases 1, 17, 33, ..., 255.
    for (int bias = 1; bias <= 255; bias += 16) {
        if (!assert_parity(v, static_cast<std::uint8_t>(bias),
                           "random 1000 bytes")) return;
    }
    // Boundary biases.
    for (std::uint8_t bias : {std::uint8_t{1}, std::uint8_t{127},
                              std::uint8_t{128}, std::uint8_t{254},
                              std::uint8_t{255}}) {
        if (!assert_parity(v, bias, "random 1000 bytes (boundary)")) return;
    }
    TEST_PASS("random 1000 bytes x bias sweep parity");
}

// Test 13 — ForceOff vs Auto parity on a 500-byte random batch across biases.
static void test_force_off_vs_auto_parity() {
    std::printf("[13] ForceOff vs Auto parity (500 random bytes, bias sweep)\n");
    std::vector<std::uint8_t> v(500);
    std::mt19937 rng(0xDEAD0123U);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : v) b = static_cast<std::uint8_t>(byte_dist(rng));

    std::vector<std::uint8_t> biases = {1, 32, 64, 96, 128, 160, 192, 224, 255};

    // Auto path (likely SIMD on host).
    set_env_and_reload(nullptr);
    std::vector<std::vector<std::uint8_t>> auto_results;
    for (std::uint8_t bias : biases) {
        std::vector<std::uint8_t> copy = v;
        sieve::saturated_sub_u8_batch(copy, bias);
        auto_results.push_back(std::move(copy));
    }

    // ForceOff (scalar) path.
    set_env_and_reload("0");
    std::vector<std::vector<std::uint8_t>> off_results;
    for (std::uint8_t bias : biases) {
        std::vector<std::uint8_t> copy = v;
        sieve::saturated_sub_u8_batch(copy, bias);
        off_results.push_back(std::move(copy));
    }

    for (std::size_t k = 0; k < biases.size(); ++k) {
        if (auto_results[k] != off_results[k]) {
            std::fprintf(stderr,
                "  parity mismatch bias=%u: Auto vs ForceOff diverge\n",
                static_cast<unsigned>(biases[k]));
            for (std::size_t i = 0; i < auto_results[k].size(); ++i) {
                if (auto_results[k][i] != off_results[k][i]) {
                    std::fprintf(stderr,
                        "    first divergence at index %zu: auto=%u off=%u\n",
                        i, static_cast<unsigned>(auto_results[k][i]),
                        static_cast<unsigned>(off_results[k][i]));
                    break;
                }
            }
            tests_failed++;
            return;
        }
    }

    set_env_and_reload(nullptr);
    TEST_PASS("ForceOff vs Auto parity (500 bytes, bias sweep)");
}

// Test 14 — perf-info probe (1M bytes). Reports scalar / SIMD ns/byte;
// also enforces parity on the full result.
static void test_perf_info_1m() {
    std::printf("[14] perf info (1M bytes)\n");
    constexpr std::size_t kN = 1'000'000;
    std::vector<std::uint8_t> base(kN);
    std::mt19937 rng(0xACE0U);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : base) b = static_cast<std::uint8_t>(byte_dist(rng));
    const std::uint8_t bias = 100;

    // Scalar (ForceOff) timing.
    set_env_and_reload("0");
    std::vector<std::uint8_t> scalar_buf = base;
    auto t0 = std::chrono::steady_clock::now();
    sieve::saturated_sub_u8_batch(scalar_buf, bias);
    auto t1 = std::chrono::steady_clock::now();
    double scalar_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

    // SIMD (Auto) timing.
    set_env_and_reload(nullptr);
    std::vector<std::uint8_t> simd_buf = base;
    auto t2 = std::chrono::steady_clock::now();
    sieve::saturated_sub_u8_batch(simd_buf, bias);
    auto t3 = std::chrono::steady_clock::now();
    double simd_us = std::chrono::duration<double, std::micro>(t3 - t2).count();

    const double scalar_ns_per_byte =
        scalar_us * 1000.0 / static_cast<double>(kN);
    const double simd_ns_per_byte =
        simd_us * 1000.0 / static_cast<double>(kN);
    const double speedup = (simd_us > 0.0) ? (scalar_us / simd_us) : 0.0;
    std::printf("    scalar=%.1f us (%.3f ns/byte) simd=%.1f us (%.3f ns/byte) speedup=%.2fx\n",
                scalar_us, scalar_ns_per_byte,
                simd_us, simd_ns_per_byte, speedup);

    TEST_ASSERT(scalar_buf.size() == simd_buf.size(),
                "scalar and SIMD must agree on size");
    for (std::size_t i = 0; i < scalar_buf.size(); ++i) {
        if (scalar_buf[i] != simd_buf[i]) {
            std::fprintf(stderr,
                "  parity fail at index %zu: scalar=%u simd=%u\n",
                i, static_cast<unsigned>(scalar_buf[i]),
                static_cast<unsigned>(simd_buf[i]));
            tests_failed++;
            return;
        }
    }
    set_env_and_reload(nullptr);
    TEST_PASS("perf info (1M bytes)");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    std::printf("=== test_saturated_sub_simd ===\n");
    std::printf("compile-time SIMD supported: %s\n",
                sieve::saturated_sub_simd_supported() ? "yes" : "no");
    const char* env = std::getenv("GNFS_SIEVE_SATURATED_SUB_SIMD");
    std::printf("GNFS_SIEVE_SATURATED_SUB_SIMD = %s\n",
                env ? env : "(unset)");

    test_env_unset_auto();
    test_env_force_off();
    test_env_force_on();
    test_env_garbage_fallback();
    test_empty_input();
    test_single_byte_combinations();
    test_bias_zero_noop();
    test_bias_255_collapse();
    test_aligned_32();
    test_unaligned_33();
    test_unaligned_65();
    test_random_1000_bias_sweep();
    test_force_off_vs_auto_parity();
    test_perf_info_1m();

    std::printf("\n=== Summary ===\n");
    std::printf("  passed: %d\n", tests_passed);
    std::printf("  failed: %d\n", tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
