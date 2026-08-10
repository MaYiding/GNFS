// Unit tests for gnfs::polynomial::ntt_mul_mod and the ENV-gated three-state
// helper poly_ntt_mode / poly_ntt_enabled_for_size.
//
// Goals:
//   1. ENV parsing: unset / "auto" → Auto; "0" / "off" → ForceOff;
//      "1" / "on" → ForceOn; anything else → Auto.
//   2. Edge cases: empty input, single-coefficient inputs.
//   3. Correctness: NTT output bit-for-bit identical to schoolbook
//      reference across sizes 10, 100, 500, 2000 with various primes
//      including 2^31-1, 2^30+3, small p=1009.
//   4. ForceOff vs ForceOn parity: same (a, b, p) under both gates must
//      produce bit-identical output.
//   5. Threshold routing: at Auto, size below threshold takes schoolbook
//      path; size above takes NTT. Both must produce the same answer.
//   6. Informational perf probe at deg=2000 (no assert).
//
// Notes:
//   poly_ntt_mode() caches the env via std::once_flag. The parsing-focused
//   tests use poly_ntt_reset_env_cache_for_testing() to permit multi-value
//   sweeps within a single test binary.

#include "gnfs/polynomial/ntt_mul.hpp"
#include "support/test_check.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <random>
#include <string>
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

using gnfs::polynomial::kNttAutoThreshold;
using gnfs::polynomial::ntt_mul_mod;
using gnfs::polynomial::poly_ntt_enabled_for_size;
using gnfs::polynomial::poly_ntt_mode;
using gnfs::polynomial::poly_ntt_reset_env_cache_for_testing;
using gnfs::polynomial::PolyNttMode;
using gnfs::polynomial::schoolbook_mul_mod;

namespace {

// (2^31 - 1) — largest Mersenne prime below 2^32. uint64 * uint64 of two
// reduced coefficients fits comfortably (max ~ 2^62), so the schoolbook
// inner product stays in range.
constexpr uint64_t kPrime31 = (1ULL << 31) - 1;
// Another prime near the high end of [0, 2^32): the next prime above
// 2^30. Picked to exercise CRT reduction with a modulus distinct from
// the standard Mersenne fixture.
constexpr uint64_t kPrime30 = (1ULL << 30) + 3ULL;
// Small prime for unit-style hand-verifiable cases.
constexpr uint64_t kSmallPrime = 1009ULL;

// ---------- helpers ----------

std::vector<uint64_t> random_poly(std::mt19937_64& rng, size_t size, uint64_t p) {
    std::vector<uint64_t> v(size);
    for (size_t i = 0; i < size; ++i) {
        v[i] = rng() % p;
    }
    // Ensure the leading coefficient is non-zero so degree matches size.
    if (!v.empty() && v.back() == 0) {
        v.back() = (rng() % (p - 1)) + 1;
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
        GNFS_TEST_CHECK(false && "NTT output must match schoolbook bit-for-bit");
    }
}

// ====================== ENV parsing tests ======================

void test_env_unset_default_auto() {
    std::cout << "test_env_unset_default_auto..." << std::endl;
    gnfs_unsetenv("GNFS_POLY_NTT");
    poly_ntt_reset_env_cache_for_testing();
    auto mode = poly_ntt_mode();
    GNFS_TEST_CHECK(mode == PolyNttMode::Auto);
    std::cout << "  PASS (mode=Auto)" << std::endl;
}

void test_env_explicit_off() {
    std::cout << "test_env_explicit_off..." << std::endl;
    for (const char* v : {"0", "off"}) {
        gnfs_setenv("GNFS_POLY_NTT", v);
        poly_ntt_reset_env_cache_for_testing();
        auto mode = poly_ntt_mode();
        if (mode != PolyNttMode::ForceOff) {
            std::cerr << "  FAIL: ENV=\"" << v << "\" gave mode=" << static_cast<int>(mode)
                      << ", expected ForceOff\n";
            GNFS_TEST_CHECK(false);
        }
    }
    gnfs_unsetenv("GNFS_POLY_NTT");
    poly_ntt_reset_env_cache_for_testing();
    std::cout << "  PASS (both \"0\" and \"off\" → ForceOff)" << std::endl;
}

void test_env_explicit_on() {
    std::cout << "test_env_explicit_on..." << std::endl;
    for (const char* v : {"1", "on"}) {
        gnfs_setenv("GNFS_POLY_NTT", v);
        poly_ntt_reset_env_cache_for_testing();
        auto mode = poly_ntt_mode();
        if (mode != PolyNttMode::ForceOn) {
            std::cerr << "  FAIL: ENV=\"" << v << "\" gave mode=" << static_cast<int>(mode)
                      << ", expected ForceOn\n";
            GNFS_TEST_CHECK(false);
        }
    }
    gnfs_unsetenv("GNFS_POLY_NTT");
    poly_ntt_reset_env_cache_for_testing();
    std::cout << "  PASS (both \"1\" and \"on\" → ForceOn)" << std::endl;
}

void test_env_unrecognized_to_auto() {
    std::cout << "test_env_unrecognized_to_auto..." << std::endl;
    const char* unrecognized[] = {
        "garbage", "2",    "true", "-1", "yes",
        "ON", // case-sensitive: only lowercase "on" recognized
        "OFF",     "Auto",
        "  1", // leading whitespace not stripped
    };
    for (const char* v : unrecognized) {
        gnfs_setenv("GNFS_POLY_NTT", v);
        poly_ntt_reset_env_cache_for_testing();
        auto mode = poly_ntt_mode();
        if (mode != PolyNttMode::Auto) {
            std::cerr << "  FAIL: ENV=\"" << v << "\" gave mode=" << static_cast<int>(mode)
                      << ", expected Auto\n";
            GNFS_TEST_CHECK(false);
        }
    }
    gnfs_unsetenv("GNFS_POLY_NTT");
    poly_ntt_reset_env_cache_for_testing();
    std::cout << "  PASS (" << (sizeof(unrecognized) / sizeof(unrecognized[0]))
              << " unrecognized values mapped to Auto)" << std::endl;
}

// ====================== Edge case tests ======================

void test_empty_inputs() {
    std::cout << "test_empty_inputs..." << std::endl;
    // Reset to Auto default so dispatch logic exercises both gates.
    gnfs_unsetenv("GNFS_POLY_NTT");
    poly_ntt_reset_env_cache_for_testing();

    std::vector<uint64_t> out;

    // both empty
    out = {99};
    ntt_mul_mod(std::vector<uint64_t>{}, std::vector<uint64_t>{}, kPrime31, out);
    GNFS_TEST_CHECK(out.empty());

    // a empty
    std::vector<uint64_t> b{1, 2, 3};
    out = {77};
    ntt_mul_mod(std::vector<uint64_t>{}, b, kPrime31, out);
    GNFS_TEST_CHECK(out.empty());

    // b empty
    std::vector<uint64_t> a{1, 2, 3};
    out = {55};
    ntt_mul_mod(a, std::vector<uint64_t>{}, kPrime31, out);
    GNFS_TEST_CHECK(out.empty());

    std::cout << "  PASS (all three empty-input cases produced empty output)" << std::endl;
}

void test_size_1_inputs() {
    std::cout << "test_size_1_inputs..." << std::endl;
    // Single-coefficient inputs should short-circuit to schoolbook
    // (poly_ntt_enabled_for_size returns false for nmax <= 1) and yield
    // a single-coefficient output equal to a[0] * b[0] mod p.
    std::vector<uint64_t> a{7};
    std::vector<uint64_t> b{13};

    // ForceOn must still produce correct output despite the size-1 short
    // circuit, because the dispatcher bails out before calling NTT.
    gnfs_setenv("GNFS_POLY_NTT", "1");
    poly_ntt_reset_env_cache_for_testing();
    std::vector<uint64_t> out_on;
    ntt_mul_mod(a, b, kSmallPrime, out_on);
    GNFS_TEST_CHECK(out_on.size() == 1);
    GNFS_TEST_CHECK(out_on[0] == (7 * 13) % kSmallPrime);

    // ForceOff path identical.
    gnfs_setenv("GNFS_POLY_NTT", "0");
    poly_ntt_reset_env_cache_for_testing();
    std::vector<uint64_t> out_off;
    ntt_mul_mod(a, b, kSmallPrime, out_off);
    expect_equal(out_on, out_off, "size_1_on_vs_off");

    // Schoolbook reference.
    std::vector<uint64_t> out_school;
    schoolbook_mul_mod(a, b, kSmallPrime, out_school);
    expect_equal(out_school, out_on, "size_1_schoolbook_vs_on");

    gnfs_unsetenv("GNFS_POLY_NTT");
    poly_ntt_reset_env_cache_for_testing();
    std::cout << "  PASS (size-1 multiply correct under On / Off / schoolbook)" << std::endl;
}

// ====================== Correctness (schoolbook parity) ======================

// Runs `ntt_mul_mod` (with ENV=ForceOn so NTT path actually runs even at
// small sizes) and `schoolbook_mul_mod` on the same (a, b, p) and asserts
// bit-for-bit identical output.
void check_parity_force_on(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b,
                           uint64_t p, const std::string& tag) {
    gnfs_setenv("GNFS_POLY_NTT", "1");
    poly_ntt_reset_env_cache_for_testing();
    std::vector<uint64_t> out_ntt, out_school;
    ntt_mul_mod(a, b, p, out_ntt);
    schoolbook_mul_mod(a, b, p, out_school);
    expect_equal(out_school, out_ntt, tag);
    gnfs_unsetenv("GNFS_POLY_NTT");
    poly_ntt_reset_env_cache_for_testing();
}

void test_parity_size_10() {
    std::cout << "test_parity_size_10..." << std::endl;
    std::mt19937_64 rng(0xAA110011BB22ABCDULL);
    // Sweep multiple primes at size 10 to catch CRT or modular reduction
    // bugs that only surface under particular p values.
    for (uint64_t p : {kPrime31, kPrime30, kSmallPrime}) {
        for (int trial = 0; trial < 3; ++trial) {
            auto a = random_poly(rng, 10, p);
            auto b = random_poly(rng, 10, p);
            check_parity_force_on(a, b, p,
                                  "size10_p" + std::to_string(p) + "_t" + std::to_string(trial));
        }
    }
    std::cout << "  PASS (3 primes × 3 trials at size=10)" << std::endl;
}

void test_parity_size_100() {
    std::cout << "test_parity_size_100..." << std::endl;
    std::mt19937_64 rng(0xCC330033DD44CDEFULL);
    for (uint64_t p : {kPrime31, kPrime30, kSmallPrime}) {
        for (int trial = 0; trial < 2; ++trial) {
            auto a = random_poly(rng, 100, p);
            auto b = random_poly(rng, 100, p);
            check_parity_force_on(a, b, p,
                                  "size100_p" + std::to_string(p) + "_t" + std::to_string(trial));
        }
    }
    // Also exercise an asymmetric shape at size-100 scale.
    auto a_asym = random_poly(rng, 100, kPrime31);
    auto b_asym = random_poly(rng, 30, kPrime31);
    check_parity_force_on(a_asym, b_asym, kPrime31, "size100x30_asym");
    std::cout << "  PASS (3 primes × 2 trials + 1 asymmetric)" << std::endl;
}

void test_parity_size_500() {
    std::cout << "test_parity_size_500..." << std::endl;
    std::mt19937_64 rng(0xEE550055FF66BEEFULL);
    for (uint64_t p : {kPrime31, kPrime30}) {
        auto a = random_poly(rng, 500, p);
        auto b = random_poly(rng, 500, p);
        check_parity_force_on(a, b, p, "size500_p" + std::to_string(p));
    }
    std::cout << "  PASS (2 primes at size=500)" << std::endl;
}

void test_parity_size_2000() {
    std::cout << "test_parity_size_2000..." << std::endl;
    std::mt19937_64 rng(0x7777888899990000ULL);
    // Single large fixture — schoolbook at 2000^2 = 4M multiplies is
    // still well under one second per call, fine for instant tier.
    auto a = random_poly(rng, 2000, kPrime31);
    auto b = random_poly(rng, 2000, kPrime31);
    check_parity_force_on(a, b, kPrime31, "size2000_p2^31-1");
    std::cout << "  PASS (size=2000 under p=2^31-1)" << std::endl;
}

// ====================== ForceOff vs ForceOn parity ======================

void test_force_off_vs_force_on() {
    std::cout << "test_force_off_vs_force_on..." << std::endl;
    std::mt19937_64 rng(0xDEADBEEFCAFEBABEULL);
    // Pick a moderate size that crosses the auto threshold (256) so the
    // two gate values genuinely route to different kernels.
    auto a = random_poly(rng, 300, kPrime31);
    auto b = random_poly(rng, 300, kPrime31);

    gnfs_setenv("GNFS_POLY_NTT", "0");
    poly_ntt_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(poly_ntt_mode() == PolyNttMode::ForceOff);
    std::vector<uint64_t> out_off;
    ntt_mul_mod(a, b, kPrime31, out_off);

    gnfs_setenv("GNFS_POLY_NTT", "1");
    poly_ntt_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(poly_ntt_mode() == PolyNttMode::ForceOn);
    std::vector<uint64_t> out_on;
    ntt_mul_mod(a, b, kPrime31, out_on);

    expect_equal(out_off, out_on, "force_off_vs_force_on_size300");

    // Also compare both against the schoolbook reference for an
    // independent third witness.
    std::vector<uint64_t> out_school;
    schoolbook_mul_mod(a, b, kPrime31, out_school);
    expect_equal(out_school, out_off, "force_off_eq_schoolbook");
    expect_equal(out_school, out_on, "force_on_eq_schoolbook");

    gnfs_unsetenv("GNFS_POLY_NTT");
    poly_ntt_reset_env_cache_for_testing();
    std::cout << "  PASS (ForceOff, ForceOn, schoolbook all agree at size=300)" << std::endl;
}

// ====================== Auto threshold routing ======================

void test_auto_threshold_routing() {
    std::cout << "test_auto_threshold_routing..." << std::endl;
    // Under Auto: size below kNttAutoThreshold takes schoolbook; size at
    // or above takes NTT. Both paths must yield bit-identical output.
    gnfs_unsetenv("GNFS_POLY_NTT");
    poly_ntt_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(poly_ntt_mode() == PolyNttMode::Auto);

    // Below threshold: routing returns false → schoolbook.
    GNFS_TEST_CHECK(!poly_ntt_enabled_for_size(kNttAutoThreshold - 1, kNttAutoThreshold - 1));
    // At threshold: routing returns true → NTT.
    GNFS_TEST_CHECK(poly_ntt_enabled_for_size(kNttAutoThreshold, kNttAutoThreshold));

    // Verify mathematical agreement.
    std::mt19937_64 rng(0xA0A1A2A3B0B1B2B3ULL);
    {
        // Below threshold: small enough that schoolbook would also run
        // under Auto. We compare to an explicit schoolbook reference.
        auto a = random_poly(rng, kNttAutoThreshold - 50, kPrime31);
        auto b = random_poly(rng, kNttAutoThreshold - 50, kPrime31);
        std::vector<uint64_t> out_auto, out_school;
        ntt_mul_mod(a, b, kPrime31, out_auto);
        schoolbook_mul_mod(a, b, kPrime31, out_school);
        expect_equal(out_school, out_auto, "auto_below_threshold");
    }
    {
        // At threshold: NTT path runs under Auto. Compare to schoolbook.
        auto a = random_poly(rng, kNttAutoThreshold + 10, kPrime31);
        auto b = random_poly(rng, kNttAutoThreshold + 10, kPrime31);
        std::vector<uint64_t> out_auto, out_school;
        ntt_mul_mod(a, b, kPrime31, out_auto);
        schoolbook_mul_mod(a, b, kPrime31, out_school);
        expect_equal(out_school, out_auto, "auto_at_threshold");
    }
    poly_ntt_reset_env_cache_for_testing();
    std::cout << "  PASS (Auto routing dispatches by size; both paths agree)" << std::endl;
}

// ====================== Informational perf probe (not asserted) ======================

void perf_info_deg_2000() {
    std::cout << "perf_info_deg_2000 (informational)..." << std::endl;
    using clk = std::chrono::high_resolution_clock;
    std::mt19937_64 rng(0xC0FFEE0BADCAFEULL);
    const auto a = random_poly(rng, 2000, kPrime31);
    const auto b = random_poly(rng, 2000, kPrime31);

    constexpr int iters = 3;
    std::vector<uint64_t> out;

    // Schoolbook baseline.
    gnfs_setenv("GNFS_POLY_NTT", "0");
    poly_ntt_reset_env_cache_for_testing();
    auto t0 = clk::now();
    for (int i = 0; i < iters; ++i) {
        ntt_mul_mod(a, b, kPrime31, out);
    }
    auto t1 = clk::now();

    // NTT.
    gnfs_setenv("GNFS_POLY_NTT", "1");
    poly_ntt_reset_env_cache_for_testing();
    auto t2 = clk::now();
    for (int i = 0; i < iters; ++i) {
        ntt_mul_mod(a, b, kPrime31, out);
    }
    auto t3 = clk::now();

    double sb_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
    double ntt_ms = std::chrono::duration<double, std::milli>(t3 - t2).count() / iters;
    double ratio = (ntt_ms > 0.0) ? (sb_ms / ntt_ms) : 0.0;

    std::cout << "  schoolbook: " << sb_ms << " ms/call"
              << "  ntt: " << ntt_ms << " ms/call"
              << "  ratio (sb/ntt): " << ratio << "x" << std::endl;
    std::cout << "  PASS (informational only — NTT wins asymptotically; "
              << "ratio > 1 favors NTT at this size)" << std::endl;

    gnfs_unsetenv("GNFS_POLY_NTT");
    poly_ntt_reset_env_cache_for_testing();
}

} // namespace

int main() {
    std::cout << "=== test_poly_ntt ===" << std::endl;
    std::cout << "kNttAutoThreshold = " << kNttAutoThreshold << " coefficients" << std::endl;
    std::cout << "primes: q1=998244353, q2=985661441, q3=754974721" << std::endl;
    std::cout << std::endl;

    // 4 ENV parsing tests.
    test_env_unset_default_auto();
    test_env_explicit_off();
    test_env_explicit_on();
    test_env_unrecognized_to_auto();

    // 2 edge cases.
    test_empty_inputs();
    test_size_1_inputs();

    // 4 correctness parity tests (multiple primes / sizes).
    test_parity_size_10();
    test_parity_size_100();
    test_parity_size_500();
    test_parity_size_2000();

    // 1 ForceOff vs ForceOn parity.
    test_force_off_vs_force_on();

    // 1 threshold routing.
    test_auto_threshold_routing();

    // 1 informational perf probe.
    perf_info_deg_2000();

    std::cout << std::endl;
    std::cout << "All tests passed." << std::endl;
    return 0;
}
