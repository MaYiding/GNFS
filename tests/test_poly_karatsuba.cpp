// Unit tests for gnfs::polynomial::karatsuba_mul_mod and the ENV-gated
// threshold helper poly_karatsuba_threshold.
//
// Goals:
//   1. ENV parsing: unset / "garbage" / "" / "0" / "-5" → default 32;
//      explicit "64" → 64; "10000" → clamp 4096.
//   2. Correctness: Karatsuba output bit-for-bit identical to
//      schoolbook reference across small / medium / large sizes,
//      including extreme threshold = 4 (recursion all the way down)
//      and threshold = 999999 (always-schoolbook path).
//   3. Edge cases: empty input, size-1 inputs.
//
// Notes:
//   poly_karatsuba_threshold() caches the env via std::once_flag. The
//   parsing-focused tests use poly_karatsuba_threshold_reset_env_cache_for_testing()
//   to permit multi-value sweeps within a single test binary.

#include "gnfs/polynomial/karatsuba_mul.hpp"
#include "support/test_check.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <stdlib.h>
#define gnfs_setenv(name, value) _putenv_s((name), (value))
#define gnfs_unsetenv(name) _putenv_s((name), "")
#else
#include <stdlib.h>
#define gnfs_setenv(name, value) ::setenv((name), (value), 1)
#define gnfs_unsetenv(name) ::unsetenv((name))
#endif

using gnfs::polynomial::karatsuba_mul_mod;
using gnfs::polynomial::poly_karatsuba_threshold;
using gnfs::polynomial::poly_karatsuba_threshold_reset_env_cache_for_testing;
using gnfs::polynomial::schoolbook_mul_mod;

namespace {

// (2^31 - 1) is the largest Mersenne prime fitting comfortably below 2^32,
// so uint64 * uint64 of two coefficients < p fits safely (max ~ 2^62).
constexpr uint64_t kPrime = (1ULL << 31) - 1;

// Smaller prime for fine-grained size-1 cases / boundary checks.
constexpr uint64_t kSmallPrime = 101;

// ----- helpers -----

std::vector<uint64_t> random_poly(std::mt19937_64& rng, size_t size, uint64_t p) {
    std::vector<uint64_t> v(size);
    for (size_t i = 0; i < size; ++i) {
        v[i] = rng() % p;
    }
    return v;
}

bool vectors_equal(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

std::string vec_to_string(const std::vector<uint64_t>& v, size_t max_show = 8) {
    std::string s = "[";
    const size_t n = std::min(v.size(), max_show);
    for (size_t i = 0; i < n; ++i) {
        if (i > 0)
            s += ", ";
        s += std::to_string(v[i]);
    }
    if (n < v.size())
        s += ", ...";
    s += "]";
    return s;
}

void expect_equal(const std::vector<uint64_t>& expected, const std::vector<uint64_t>& actual,
                  const std::string& tag) {
    if (!vectors_equal(expected, actual)) {
        std::cerr << "  FAIL [" << tag << "]\n";
        std::cerr << "    expected size=" << expected.size() << " " << vec_to_string(expected)
                  << "\n";
        std::cerr << "    actual   size=" << actual.size() << " " << vec_to_string(actual) << "\n";
        GNFS_TEST_CHECK(false && "Karatsuba output must match schoolbook bit-for-bit");
    }
}

// ====================== ENV parsing tests ======================

void test_env_unset_default() {
    std::cout << "test_env_unset_default..." << std::endl;
    gnfs_unsetenv("GNFS_POLY_KARATSUBA_THRESHOLD");
    poly_karatsuba_threshold_reset_env_cache_for_testing();
    int t = poly_karatsuba_threshold();
    GNFS_TEST_CHECK(t == 32);
    std::cout << "  PASS (threshold=" << t << ")" << std::endl;
}

void test_env_explicit_64() {
    std::cout << "test_env_explicit_64..." << std::endl;
    gnfs_setenv("GNFS_POLY_KARATSUBA_THRESHOLD", "64");
    poly_karatsuba_threshold_reset_env_cache_for_testing();
    int t = poly_karatsuba_threshold();
    GNFS_TEST_CHECK(t == 64);
    std::cout << "  PASS (threshold=" << t << ")" << std::endl;
}

void test_env_invalid_returns_default() {
    std::cout << "test_env_invalid_returns_default..." << std::endl;
    // Every invalid value should yield the default 32.
    const char* invalids[] = {
        "garbage", "",  "0", "-5", "12abc",
        " 32", // leading whitespace is rejected (strict parser)
        "1.5",     "+", "-",
    };
    for (const char* v : invalids) {
        gnfs_setenv("GNFS_POLY_KARATSUBA_THRESHOLD", v);
        poly_karatsuba_threshold_reset_env_cache_for_testing();
        int t = poly_karatsuba_threshold();
        if (t != 32) {
            std::cerr << "  FAIL: ENV=\"" << v << "\" gave threshold=" << t << ", expected 32"
                      << std::endl;
            GNFS_TEST_CHECK(false);
        }
    }
    gnfs_unsetenv("GNFS_POLY_KARATSUBA_THRESHOLD");
    std::cout << "  PASS (" << (sizeof(invalids) / sizeof(invalids[0]))
              << " invalid values mapped to default 32)" << std::endl;
}

void test_env_clamp_upper() {
    std::cout << "test_env_clamp_upper..." << std::endl;
    gnfs_setenv("GNFS_POLY_KARATSUBA_THRESHOLD", "10000");
    poly_karatsuba_threshold_reset_env_cache_for_testing();
    int t = poly_karatsuba_threshold();
    GNFS_TEST_CHECK(t == 4096);
    gnfs_unsetenv("GNFS_POLY_KARATSUBA_THRESHOLD");
    std::cout << "  PASS (10000 → " << t << ")" << std::endl;
}

void test_env_clamp_lower() {
    std::cout << "test_env_clamp_lower..." << std::endl;
    // Positive but below the minimum (4) should clamp upward.
    gnfs_setenv("GNFS_POLY_KARATSUBA_THRESHOLD", "2");
    poly_karatsuba_threshold_reset_env_cache_for_testing();
    int t = poly_karatsuba_threshold();
    GNFS_TEST_CHECK(t == 4);
    gnfs_unsetenv("GNFS_POLY_KARATSUBA_THRESHOLD");
    std::cout << "  PASS (2 → " << t << ")" << std::endl;
}

// ====================== Edge case tests ======================

void test_empty_inputs() {
    std::cout << "test_empty_inputs..." << std::endl;
    std::vector<uint64_t> out;

    // both empty
    out = {99};
    karatsuba_mul_mod(std::span<const uint64_t>(), std::span<const uint64_t>(), kPrime, out);
    GNFS_TEST_CHECK(out.empty());

    // a empty
    std::vector<uint64_t> b{1, 2, 3};
    out = {77};
    karatsuba_mul_mod(std::span<const uint64_t>(), std::span<const uint64_t>(b), kPrime, out);
    GNFS_TEST_CHECK(out.empty());

    // b empty
    std::vector<uint64_t> a{1, 2, 3};
    out = {55};
    karatsuba_mul_mod(std::span<const uint64_t>(a), std::span<const uint64_t>(), kPrime, out);
    GNFS_TEST_CHECK(out.empty());

    std::cout << "  PASS" << std::endl;
}

void test_size_1_times_size_1() {
    std::cout << "test_size_1_times_size_1..." << std::endl;
    // a = 7, b = 13, p = 101 → out = [7 * 13 mod 101] = [91]
    std::vector<uint64_t> a{7};
    std::vector<uint64_t> b{13};
    std::vector<uint64_t> out_k, out_s;
    karatsuba_mul_mod(a, b, kSmallPrime, out_k);
    schoolbook_mul_mod(a, b, kSmallPrime, out_s);
    GNFS_TEST_CHECK(out_k.size() == 1);
    GNFS_TEST_CHECK(out_k[0] == 91);
    expect_equal(out_s, out_k, "size_1_times_size_1");
    std::cout << "  PASS (out=[" << out_k[0] << "])" << std::endl;
}

// ====================== Correctness (schoolbook parity) ======================

void test_parity_random_sizes() {
    std::cout << "test_parity_random_sizes..." << std::endl;
    // Reset threshold to default to make this independent of prior tests.
    gnfs_unsetenv("GNFS_POLY_KARATSUBA_THRESHOLD");
    poly_karatsuba_threshold_reset_env_cache_for_testing();

    std::mt19937_64 rng(0x4242DEADBEEF1234ULL);

    // Sweep covering: under threshold (10), at threshold (32), above
    // threshold (50, 100, 200, 500), and very asymmetric (200 x 50).
    const std::pair<size_t, size_t> shapes[] = {
        {1, 1},     {1, 10},    {10, 1},   {10, 10},  {32, 32},   {50, 50},  {100, 100},
        {200, 200}, {500, 500}, {200, 50}, {50, 200}, {333, 111}, {77, 333},
    };
    for (const auto& [na, nb] : shapes) {
        auto a = random_poly(rng, na, kPrime);
        auto b = random_poly(rng, nb, kPrime);
        std::vector<uint64_t> out_k, out_s;
        karatsuba_mul_mod(a, b, kPrime, out_k);
        schoolbook_mul_mod(a, b, kPrime, out_s);
        expect_equal(out_s, out_k, "random_" + std::to_string(na) + "x" + std::to_string(nb));
    }
    std::cout << "  PASS (" << (sizeof(shapes) / sizeof(shapes[0])) << " shapes)" << std::endl;
}

void test_threshold_boundary_4_vs_999999() {
    std::cout << "test_threshold_boundary_4_vs_999999..." << std::endl;
    // Generate a single random fixture and verify both extreme thresholds
    // produce the same output as schoolbook.
    std::mt19937_64 rng(0xABCDEF0123456789ULL);
    const auto a = random_poly(rng, 150, kPrime);
    const auto b = random_poly(rng, 120, kPrime);

    std::vector<uint64_t> out_school;
    schoolbook_mul_mod(a, b, kPrime, out_school);

    // threshold=4 → recursion all the way down (deep stack).
    gnfs_setenv("GNFS_POLY_KARATSUBA_THRESHOLD", "4");
    poly_karatsuba_threshold_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(poly_karatsuba_threshold() == 4);
    std::vector<uint64_t> out_k_small;
    karatsuba_mul_mod(a, b, kPrime, out_k_small);
    expect_equal(out_school, out_k_small, "threshold=4");

    // threshold=999999 → clamped to 4096, but since both inputs are below
    // 4096 this means schoolbook every call (Karatsuba never triggers).
    gnfs_setenv("GNFS_POLY_KARATSUBA_THRESHOLD", "999999");
    poly_karatsuba_threshold_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(poly_karatsuba_threshold() == 4096);
    std::vector<uint64_t> out_k_large;
    karatsuba_mul_mod(a, b, kPrime, out_k_large);
    expect_equal(out_school, out_k_large, "threshold=999999 (clamped to 4096)");

    // The two karatsuba outputs must also agree with each other.
    expect_equal(out_k_small, out_k_large, "threshold extremes mutual");

    gnfs_unsetenv("GNFS_POLY_KARATSUBA_THRESHOLD");
    poly_karatsuba_threshold_reset_env_cache_for_testing();

    std::cout << "  PASS (both extremes produce schoolbook-identical output)" << std::endl;
}

// ====================== Informational perf probe (not asserted) ======================

void perf_info_large() {
    std::cout << "perf_info_large (size=500, informational)..." << std::endl;
    using clk = std::chrono::high_resolution_clock;

    gnfs_unsetenv("GNFS_POLY_KARATSUBA_THRESHOLD");
    poly_karatsuba_threshold_reset_env_cache_for_testing();

    std::mt19937_64 rng(0xC0FFEE0BADCAFEULL);
    const auto a = random_poly(rng, 500, kPrime);
    const auto b = random_poly(rng, 500, kPrime);

    constexpr int iters = 3;
    std::vector<uint64_t> out;

    auto t0 = clk::now();
    for (int i = 0; i < iters; ++i) {
        schoolbook_mul_mod(a, b, kPrime, out);
    }
    auto t1 = clk::now();
    for (int i = 0; i < iters; ++i) {
        karatsuba_mul_mod(a, b, kPrime, out);
    }
    auto t2 = clk::now();

    double sb_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
    double kt_ms = std::chrono::duration<double, std::milli>(t2 - t1).count() / iters;
    double ratio = (kt_ms > 0.0) ? (sb_ms / kt_ms) : 0.0;

    std::cout << "  schoolbook: " << sb_ms << " ms/call"
              << "  karatsuba: " << kt_ms << " ms/call"
              << "  ratio (sb/kt): " << ratio << "x" << std::endl;
    std::cout << "  PASS (informational only — Karatsuba wins for larger n)" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== test_poly_karatsuba ===" << std::endl;
    std::cout << "default threshold = 32, clamp range = [4, 4096]" << std::endl;
    std::cout << std::endl;

    // 5 ENV parsing tests.
    test_env_unset_default();
    test_env_explicit_64();
    test_env_invalid_returns_default();
    test_env_clamp_upper();
    test_env_clamp_lower();

    // 2 edge cases.
    test_empty_inputs();
    test_size_1_times_size_1();

    // 2 correctness tests (schoolbook parity).
    test_parity_random_sizes();
    test_threshold_boundary_4_vs_999999();

    // 1 informational perf probe.
    perf_info_large();

    std::cout << std::endl;
    std::cout << "All tests passed." << std::endl;
    return 0;
}
