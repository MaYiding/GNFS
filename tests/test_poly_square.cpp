// Unit tests for gnfs::polynomial modular squaring helper.
//
// Covers:
//   1. ENV parsing for GNFS_POLY_SQUARE_OPT (Auto / ForceOff / ForceOn /
//      unrecognized → Auto).
//   2. ENV parsing for GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD (default 32,
//      lower clamp to 4, upper clamp to 4096, garbage → default).
//   3. Edge cases: empty input, single coefficient.
//   4. Schoolbook squaring parity vs W9 karatsuba_mul_mod(a, a, p, out)
//      at deg 2 / 10 / 50.
//   5. Karatsuba squaring parity vs W9 karatsuba_mul_mod(a, a, p, out)
//      at deg 200 / 500.
//   6. Random shape sweep across degrees 5..400.
//   7. p = 2^31 - 1 (Mersenne) coefficient boundary.
//   8. Informational perf probe at deg=200 (no assert; squaring vs
//      W9 karatsuba_mul_mod full-mul wall comparison).
//
// Notes:
//   poly_square_mode() and poly_square_karatsuba_threshold() cache the
//   env via std::once_flag. ENV-focused tests call
//   poly_square_reset_env_cache_for_testing() between mutations.
//   Both caches are reset by a single helper call (header design).

#include "gnfs/polynomial/karatsuba_mul.hpp"
#include "gnfs/polynomial/poly_square.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <vector>

#if defined(_WIN32)
  #include <stdlib.h>
  #define gnfs_setenv(name, value) _putenv_s((name), (value))
  #define gnfs_unsetenv(name)      _putenv_s((name), "")
#else
  #include <stdlib.h>
  #define gnfs_setenv(name, value) ::setenv((name), (value), 1)
  #define gnfs_unsetenv(name)      ::unsetenv((name))
#endif

using gnfs::polynomial::karatsuba_mul_mod;
using gnfs::polynomial::karatsuba_square_mod;
using gnfs::polynomial::PolySquareMode;
using gnfs::polynomial::poly_square_enabled;
using gnfs::polynomial::poly_square_karatsuba_threshold;
using gnfs::polynomial::poly_square_mode;
using gnfs::polynomial::poly_square_reset_env_cache_for_testing;
using gnfs::polynomial::schoolbook_mul_mod;
using gnfs::polynomial::schoolbook_square_mod;
using gnfs::polynomial::square_mod;

namespace {

// (2^31 - 1) — largest Mersenne prime below 2^32. uint64 * uint64 of two
// reduced coefficients fits comfortably (max ~ 2^62), so both the
// schoolbook square inner product and the Karatsuba inner sums stay in
// range.
constexpr uint64_t kPrime31 = (1ULL << 31) - 1;

// Another prime near the high end of [0, 2^32): the next prime above
// 2^30. Picked to exercise modular reduction with a modulus distinct
// from the standard Mersenne fixture.
constexpr uint64_t kPrime30 = (1ULL << 30) + 3ULL;

// Small prime for unit-style hand-verifiable cases.
constexpr uint64_t kSmallPrime = 1009ULL;

// ---------- helpers ----------

std::vector<uint64_t> random_poly(std::mt19937_64& rng, size_t size,
                                  uint64_t p) {
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

bool vectors_equal(const std::vector<uint64_t>& a,
                   const std::vector<uint64_t>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

std::string vec_to_string(const std::vector<uint64_t>& v,
                          size_t max_show = 8) {
    std::string s = "[";
    const size_t n = std::min(v.size(), max_show);
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) s += ", ";
        s += std::to_string(v[i]);
    }
    if (n < v.size()) s += ", ...";
    s += "]";
    return s;
}

void expect_equal(const std::vector<uint64_t>& expected,
                  const std::vector<uint64_t>& actual,
                  const std::string& tag) {
    if (!vectors_equal(expected, actual)) {
        std::cerr << "  FAIL [" << tag << "]\n";
        std::cerr << "    expected size=" << expected.size()
                  << " " << vec_to_string(expected) << "\n";
        std::cerr << "    actual   size=" << actual.size()
                  << " " << vec_to_string(actual) << "\n";
        assert(false && "square_mod output must match karatsuba_mul_mod(a, a) bit-for-bit");
    }
}

// Build the W9 golden reference by calling karatsuba_mul_mod(a, a, p, out).
// karatsuba_mul_mod is the bit-for-bit-equivalent kernel that this
// helper's square_mod must replicate.
void karatsuba_self_mul_reference(const std::vector<uint64_t>& a,
                                  uint64_t p,
                                  std::vector<uint64_t>& out) {
    std::span<const uint64_t> sa(a.data(), a.size());
    karatsuba_mul_mod(sa, sa, p, out);
    // karatsuba_mul_mod does not trim; the helper's square_mod does
    // (matching schoolbook_mul_mod's canonical form in the W12 NTT
    // helper). Trim here so the parity comparison runs against a
    // canonical golden.
    while (!out.empty() && out.back() == 0) {
        out.pop_back();
    }
}

// ====================== ENV parsing tests ======================

void test_env_unset_default_auto() {
    std::cout << "test_env_unset_default_auto..." << std::endl;
    gnfs_unsetenv("GNFS_POLY_SQUARE_OPT");
    poly_square_reset_env_cache_for_testing();
    auto mode = poly_square_mode();
    assert(mode == PolySquareMode::Auto);
    assert(poly_square_enabled());
    std::cout << "  PASS (mode=Auto, enabled)" << std::endl;
}

void test_env_explicit_off() {
    std::cout << "test_env_explicit_off..." << std::endl;
    for (const char* v : {"0", "off"}) {
        gnfs_setenv("GNFS_POLY_SQUARE_OPT", v);
        poly_square_reset_env_cache_for_testing();
        auto mode = poly_square_mode();
        if (mode != PolySquareMode::ForceOff) {
            std::cerr << "  FAIL: ENV=\"" << v << "\" gave mode="
                      << static_cast<int>(mode)
                      << ", expected ForceOff\n";
            assert(false);
        }
        assert(!poly_square_enabled());
    }
    gnfs_unsetenv("GNFS_POLY_SQUARE_OPT");
    poly_square_reset_env_cache_for_testing();
    std::cout << "  PASS (both \"0\" and \"off\" → ForceOff, disabled)" << std::endl;
}

void test_env_explicit_on() {
    std::cout << "test_env_explicit_on..." << std::endl;
    for (const char* v : {"1", "on"}) {
        gnfs_setenv("GNFS_POLY_SQUARE_OPT", v);
        poly_square_reset_env_cache_for_testing();
        auto mode = poly_square_mode();
        if (mode != PolySquareMode::ForceOn) {
            std::cerr << "  FAIL: ENV=\"" << v << "\" gave mode="
                      << static_cast<int>(mode)
                      << ", expected ForceOn\n";
            assert(false);
        }
        assert(poly_square_enabled());
    }
    gnfs_unsetenv("GNFS_POLY_SQUARE_OPT");
    poly_square_reset_env_cache_for_testing();
    std::cout << "  PASS (both \"1\" and \"on\" → ForceOn, enabled)" << std::endl;
}

void test_env_unrecognized_to_auto() {
    std::cout << "test_env_unrecognized_to_auto..." << std::endl;
    const char* unrecognized[] = {
        "garbage",
        "2",
        "true",
        "-1",
        "yes",
        "ON",      // case-sensitive: only lowercase "on" recognized
        "OFF",
        "Auto",
        "  1",     // leading whitespace not stripped
    };
    for (const char* v : unrecognized) {
        gnfs_setenv("GNFS_POLY_SQUARE_OPT", v);
        poly_square_reset_env_cache_for_testing();
        auto mode = poly_square_mode();
        if (mode != PolySquareMode::Auto) {
            std::cerr << "  FAIL: ENV=\"" << v << "\" gave mode="
                      << static_cast<int>(mode)
                      << ", expected Auto\n";
            assert(false);
        }
    }
    gnfs_unsetenv("GNFS_POLY_SQUARE_OPT");
    poly_square_reset_env_cache_for_testing();
    std::cout << "  PASS ("
              << (sizeof(unrecognized) / sizeof(unrecognized[0]))
              << " unrecognized values mapped to Auto)" << std::endl;
}

void test_threshold_env_default_32() {
    std::cout << "test_threshold_env_default_32..." << std::endl;
    gnfs_unsetenv("GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD");
    poly_square_reset_env_cache_for_testing();
    auto t = poly_square_karatsuba_threshold();
    assert(t == 32);
    gnfs_setenv("GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD", "32");
    poly_square_reset_env_cache_for_testing();
    assert(poly_square_karatsuba_threshold() == 32);
    gnfs_unsetenv("GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD");
    poly_square_reset_env_cache_for_testing();
    std::cout << "  PASS (default 32 reported)" << std::endl;
}

void test_threshold_env_lower_clamp() {
    std::cout << "test_threshold_env_lower_clamp..." << std::endl;
    // "1", "2", "3" all below the min (4) → clamp to 4.
    for (const char* v : {"1", "2", "3"}) {
        gnfs_setenv("GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD", v);
        poly_square_reset_env_cache_for_testing();
        auto t = poly_square_karatsuba_threshold();
        if (t != 4) {
            std::cerr << "  FAIL: ENV=\"" << v << "\" gave threshold="
                      << t << ", expected 4 (lower clamp)\n";
            assert(false);
        }
    }
    // Explicit "4" stays 4.
    gnfs_setenv("GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD", "4");
    poly_square_reset_env_cache_for_testing();
    assert(poly_square_karatsuba_threshold() == 4);
    // "5000" upper clamp to 4096.
    gnfs_setenv("GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD", "5000");
    poly_square_reset_env_cache_for_testing();
    assert(poly_square_karatsuba_threshold() == 4096);
    // garbage → default 32.
    gnfs_setenv("GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD", "garbage");
    poly_square_reset_env_cache_for_testing();
    assert(poly_square_karatsuba_threshold() == 32);
    gnfs_unsetenv("GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD");
    poly_square_reset_env_cache_for_testing();
    std::cout << "  PASS (lower clamp 1/2/3→4, explicit 4, upper clamp 5000→4096, garbage→32)"
              << std::endl;
}

// ====================== Edge case tests ======================

void test_empty_input() {
    std::cout << "test_empty_input..." << std::endl;
    gnfs_unsetenv("GNFS_POLY_SQUARE_OPT");
    gnfs_unsetenv("GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD");
    poly_square_reset_env_cache_for_testing();

    std::vector<uint64_t> out{77};
    square_mod({}, kPrime31, out);
    assert(out.empty());

    // schoolbook_square_mod on empty → empty
    out = {88};
    schoolbook_square_mod({}, kPrime31, out);
    assert(out.empty());

    // karatsuba_square_mod on empty → empty
    out = {99};
    karatsuba_square_mod({}, kPrime31, out);
    assert(out.empty());

    std::cout << "  PASS (empty input produces empty output across all kernels)"
              << std::endl;
}

void test_single_coefficient() {
    std::cout << "test_single_coefficient..." << std::endl;
    gnfs_unsetenv("GNFS_POLY_SQUARE_OPT");
    gnfs_unsetenv("GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD");
    poly_square_reset_env_cache_for_testing();

    std::vector<uint64_t> a{7};

    // square_mod
    std::vector<uint64_t> out;
    std::span<const uint64_t> sa(a.data(), a.size());
    square_mod(sa, kSmallPrime, out);
    assert(out.size() == 1);
    assert(out[0] == (7 * 7) % kSmallPrime);

    // schoolbook_square_mod
    std::vector<uint64_t> out2;
    schoolbook_square_mod(sa, kSmallPrime, out2);
    assert(out2.size() == 1);
    assert(out2[0] == 49 % kSmallPrime);

    // karatsuba_square_mod
    std::vector<uint64_t> out3;
    karatsuba_square_mod(sa, kSmallPrime, out3);
    assert(out3.size() == 1);
    assert(out3[0] == 49 % kSmallPrime);

    // Single coefficient = 0 → out should canonicalize to empty.
    std::vector<uint64_t> zero{0};
    std::vector<uint64_t> outz;
    std::span<const uint64_t> sz(zero.data(), zero.size());
    square_mod(sz, kPrime31, outz);
    assert(outz.empty());

    std::cout << "  PASS (single coefficient squared correctly; 0 → empty)"
              << std::endl;
}

void test_deg_2_cross_product() {
    std::cout << "test_deg_2_cross_product..." << std::endl;
    gnfs_unsetenv("GNFS_POLY_SQUARE_OPT");
    gnfs_unsetenv("GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD");
    poly_square_reset_env_cache_for_testing();

    // (a0 + a1 x)^2 = a0^2 + 2 a0 a1 x + a1^2 x^2
    std::vector<uint64_t> a{3, 5};
    std::span<const uint64_t> sa(a.data(), a.size());

    std::vector<uint64_t> out;
    square_mod(sa, kSmallPrime, out);
    assert(out.size() == 3);
    assert(out[0] == 9);            // 3^2
    assert(out[1] == 30);           // 2 * 3 * 5
    assert(out[2] == 25);           // 5^2

    // Also verify against W9 karatsuba_mul_mod(a, a).
    std::vector<uint64_t> ref;
    karatsuba_self_mul_reference(a, kSmallPrime, ref);
    expect_equal(ref, out, "deg2_cross_product");

    std::cout << "  PASS (deg=2 cross product matches expected algebra)"
              << std::endl;
}

// ====================== Schoolbook parity tests ======================

// Compare `square_mod` (helper main entry, all gates) with W9
// `karatsuba_mul_mod(a, a, p, out)` golden for bit-for-bit identity.
// Runs the comparison across:
//   - Auto gate (default routing)
//   - ForceOn gate (squaring optimisation enabled, identical to Auto)
//   - ForceOff gate (squaring optimisation disabled, routes through W9
//     full-mul; should still produce identical output).
// All four kernels (Auto schoolbook, Auto Karatsuba, ForceOn schoolbook,
// ForceOn Karatsuba) get exercised when used across different sizes
// because the threshold dispatch fires at `a.size() >= threshold`.
void check_square_parity_all_gates(const std::vector<uint64_t>& a,
                                   uint64_t p,
                                   const std::string& tag) {
    std::vector<uint64_t> ref;
    karatsuba_self_mul_reference(a, p, ref);

    std::span<const uint64_t> sa(a.data(), a.size());

    // Auto
    gnfs_unsetenv("GNFS_POLY_SQUARE_OPT");
    poly_square_reset_env_cache_for_testing();
    std::vector<uint64_t> out_auto;
    square_mod(sa, p, out_auto);
    expect_equal(ref, out_auto, tag + "_auto");

    // ForceOn
    gnfs_setenv("GNFS_POLY_SQUARE_OPT", "1");
    poly_square_reset_env_cache_for_testing();
    std::vector<uint64_t> out_on;
    square_mod(sa, p, out_on);
    expect_equal(ref, out_on, tag + "_force_on");

    // ForceOff
    gnfs_setenv("GNFS_POLY_SQUARE_OPT", "0");
    poly_square_reset_env_cache_for_testing();
    std::vector<uint64_t> out_off;
    square_mod(sa, p, out_off);
    expect_equal(ref, out_off, tag + "_force_off");

    // Direct kernels (independent of gate, for symmetry coverage)
    std::vector<uint64_t> out_school;
    schoolbook_square_mod(sa, p, out_school);
    expect_equal(ref, out_school, tag + "_schoolbook_direct");

    std::vector<uint64_t> out_kara;
    karatsuba_square_mod(sa, p, out_kara);
    expect_equal(ref, out_kara, tag + "_karatsuba_direct");

    gnfs_unsetenv("GNFS_POLY_SQUARE_OPT");
    poly_square_reset_env_cache_for_testing();
}

void test_schoolbook_parity_deg_10() {
    std::cout << "test_schoolbook_parity_deg_10..." << std::endl;
    std::mt19937_64 rng(0xAA110011BB22ABCDULL);
    for (uint64_t p : {kPrime31, kPrime30, kSmallPrime}) {
        for (int trial = 0; trial < 3; ++trial) {
            auto a = random_poly(rng, 10, p);
            check_square_parity_all_gates(a, p,
                "deg10_p" + std::to_string(p) + "_t" +
                std::to_string(trial));
        }
    }
    std::cout << "  PASS (3 primes × 3 trials at deg=10)" << std::endl;
}

void test_schoolbook_parity_deg_50() {
    std::cout << "test_schoolbook_parity_deg_50..." << std::endl;
    std::mt19937_64 rng(0xCC330033DD44CDEFULL);
    for (uint64_t p : {kPrime31, kPrime30, kSmallPrime}) {
        for (int trial = 0; trial < 2; ++trial) {
            auto a = random_poly(rng, 50, p);
            check_square_parity_all_gates(a, p,
                "deg50_p" + std::to_string(p) + "_t" +
                std::to_string(trial));
        }
    }
    std::cout << "  PASS (3 primes × 2 trials at deg=50)" << std::endl;
}

// ====================== Karatsuba parity tests ======================

void test_karatsuba_parity_deg_200() {
    std::cout << "test_karatsuba_parity_deg_200..." << std::endl;
    std::mt19937_64 rng(0xEE550055FF66BEEFULL);
    for (uint64_t p : {kPrime31, kPrime30}) {
        auto a = random_poly(rng, 200, p);
        check_square_parity_all_gates(a, p,
            "deg200_p" + std::to_string(p));
    }
    std::cout << "  PASS (2 primes at deg=200)" << std::endl;
}

void test_karatsuba_parity_deg_500() {
    std::cout << "test_karatsuba_parity_deg_500..." << std::endl;
    std::mt19937_64 rng(0x7777888899990000ULL);
    // Single large fixture — karatsuba_mul_mod at 500x500 is sub-second.
    auto a = random_poly(rng, 500, kPrime31);
    check_square_parity_all_gates(a, kPrime31, "deg500_p2^31-1");
    std::cout << "  PASS (deg=500 under p=2^31-1)" << std::endl;
}

// ====================== Random shape sweep ======================

void test_random_shape_sweep() {
    std::cout << "test_random_shape_sweep..." << std::endl;
    std::mt19937_64 rng(0xDEADBEEFCAFEBABEULL);
    // 10 random shapes across degrees [5, 400] to stress different
    // recursion depths and threshold boundary crossings.
    const std::vector<size_t> sizes{5, 7, 16, 31, 32, 33, 64, 100, 250, 400};
    int idx = 0;
    for (size_t n : sizes) {
        const uint64_t p = (idx & 1) ? kPrime31 : kPrime30;
        auto a = random_poly(rng, n, p);
        check_square_parity_all_gates(a, p,
            "shape_n" + std::to_string(n) + "_p" + std::to_string(p));
        ++idx;
    }
    std::cout << "  PASS (10 shapes swept, including threshold-boundary 31/32/33)"
              << std::endl;
}

// ====================== Mersenne boundary ======================

void test_mersenne_p_boundary() {
    std::cout << "test_mersenne_p_boundary..." << std::endl;
    // p = 2^31 - 1 is the largest Mersenne prime below 2^32. Coefficients
    // up to p-1 ≈ 2^31; products up to (p-1)^2 ≈ 2^62 fit uint64_t with
    // room. Build an input where every coefficient is exactly p-1 to
    // stress the upper bound, then verify against W9.
    const uint64_t p = kPrime31;
    std::vector<uint64_t> a(50, p - 1);  // all coefficients = p - 1
    check_square_parity_all_gates(a, p, "mersenne_all_pminus1_n50");

    // Also a single big-coeff input at a moderate size.
    a.assign(150, 0);
    a[0] = p - 1;
    a[149] = p - 1;
    a[74] = p - 1;
    check_square_parity_all_gates(a, p, "mersenne_sparse_n150");

    std::cout << "  PASS (Mersenne p boundary, both dense and sparse)"
              << std::endl;
}

// ====================== Informational perf probe ======================

void perf_info_deg_200() {
    std::cout << "perf_info_deg_200 (informational)..." << std::endl;
    using clk = std::chrono::high_resolution_clock;
    std::mt19937_64 rng(0xC0FFEE0BADCAFEULL);
    const auto a = random_poly(rng, 200, kPrime31);
    std::span<const uint64_t> sa(a.data(), a.size());

    constexpr int iters = 100;
    std::vector<uint64_t> out;

    // square_mod (Auto, which routes to Karatsuba squaring above
    // threshold 32 since size=200 >> 32).
    gnfs_unsetenv("GNFS_POLY_SQUARE_OPT");
    poly_square_reset_env_cache_for_testing();
    auto t0 = clk::now();
    for (int i = 0; i < iters; ++i) {
        square_mod(sa, kPrime31, out);
    }
    auto t1 = clk::now();

    // W9 karatsuba_mul_mod(a, a, p, out) full-mul reference.
    auto t2 = clk::now();
    for (int i = 0; i < iters; ++i) {
        karatsuba_mul_mod(sa, sa, kPrime31, out);
    }
    auto t3 = clk::now();

    // Schoolbook square (ForceOn + threshold above size 200 forces
    // schoolbook path).
    gnfs_setenv("GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD", "4096");
    gnfs_setenv("GNFS_POLY_SQUARE_OPT", "1");
    poly_square_reset_env_cache_for_testing();
    auto t4 = clk::now();
    for (int i = 0; i < iters; ++i) {
        square_mod(sa, kPrime31, out);
    }
    auto t5 = clk::now();

    double sq_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
    double mul_ms =
        std::chrono::duration<double, std::milli>(t3 - t2).count() / iters;
    double sb_sq_ms =
        std::chrono::duration<double, std::milli>(t5 - t4).count() / iters;
    double sq_speedup = (sq_ms > 0.0) ? (mul_ms / sq_ms) : 0.0;
    double sb_speedup = (sb_sq_ms > 0.0) ? (mul_ms / sb_sq_ms) : 0.0;

    std::cout << "  square_mod (Auto/Karatsuba): " << sq_ms << " ms/call"
              << std::endl;
    std::cout << "  karatsuba_mul_mod(a, a):     " << mul_ms << " ms/call"
              << std::endl;
    std::cout << "  schoolbook_square_mod:       " << sb_sq_ms << " ms/call"
              << std::endl;
    std::cout << "  square/mul speedup: " << sq_speedup << "x"
              << "  schoolbook_square/mul speedup: " << sb_speedup << "x"
              << std::endl;
    std::cout << "  PASS (informational only — squaring at deg=200 should be "
              << "competitive with W9 mul)" << std::endl;

    gnfs_unsetenv("GNFS_POLY_SQUARE_OPT");
    gnfs_unsetenv("GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD");
    poly_square_reset_env_cache_for_testing();
}

}  // namespace

int main() {
    std::cout << "=== test_poly_square ===" << std::endl;
    std::cout << "default Karatsuba squaring threshold = 32 (matches W9 mul)"
              << std::endl;
    std::cout << "golden reference: W9 karatsuba_mul_mod(a, a, p, out) "
              << "(trimmed canonical)" << std::endl;
    std::cout << std::endl;

    // 4 ENV_OPT parsing tests.
    test_env_unset_default_auto();
    test_env_explicit_off();
    test_env_explicit_on();
    test_env_unrecognized_to_auto();

    // 2 ENV_THRESHOLD tests.
    test_threshold_env_default_32();
    test_threshold_env_lower_clamp();

    // 3 edge cases.
    test_empty_input();
    test_single_coefficient();
    test_deg_2_cross_product();

    // 2 schoolbook parity.
    test_schoolbook_parity_deg_10();
    test_schoolbook_parity_deg_50();

    // 2 Karatsuba parity.
    test_karatsuba_parity_deg_200();
    test_karatsuba_parity_deg_500();

    // 1 random shape sweep.
    test_random_shape_sweep();

    // 1 Mersenne boundary.
    test_mersenne_p_boundary();

    // 1 informational perf probe.
    perf_info_deg_200();

    std::cout << std::endl;
    std::cout << "All tests passed." << std::endl;
    return 0;
}
