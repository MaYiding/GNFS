// Unit tests for gnfs::polynomial::gcd_via_hgcd — Half-GCD over F_p[x].
//
// Goals:
//   1. Correctness: HGCD output is bit-for-bit identical to ModularPoly::gcd
//      across a wide range of input degrees and common-factor structures.
//   2. Edge cases: zero polynomial, constant polynomial, coprime inputs,
//      perfect divisibility.
//   3. ENV parsing: poly_hgcd_enabled() respects GNFS_POLY_HGCD.
//   4. Informational perf comparison (not asserted).

#include "gnfs/polynomial/half_gcd.hpp"
#include "gnfs/sqrt/modular_poly.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using gnfs::polynomial::gcd_via_hgcd;
using gnfs::polynomial::kHGCDThreshold;
using gnfs::polynomial::poly_hgcd_enabled;
using gnfs::polynomial::should_use_hgcd;
using gnfs::sqrt::ModularPoly;

namespace {

constexpr uint64_t kSmallPrime = 1000003;
constexpr uint64_t kLargePrime = 18446744073709551557ULL;  // largest 64-bit prime

/// Build a random polynomial of exact degree `deg` mod p, leading coeff = 1.
ModularPoly random_poly(std::mt19937_64& rng, size_t deg, uint64_t p) {
    std::vector<uint64_t> coeffs(deg + 1);
    for (size_t i = 0; i < deg; ++i) {
        coeffs[i] = rng() % p;
    }
    coeffs[deg] = 1;  // monic (force nonzero leading coeff)
    return ModularPoly(std::move(coeffs));
}

/// Compare two polynomials for bit-for-bit equality.
bool polys_equal(const ModularPoly& a, const ModularPoly& b) {
    if (a.degree() != b.degree()) return false;
    if (a.is_zero() != b.is_zero()) return false;
    if (a.is_zero()) return true;
    for (size_t i = 0; i <= static_cast<size_t>(a.degree()); ++i) {
        if (a.coeff(i) != b.coeff(i)) return false;
    }
    return true;
}

std::string poly_to_string(const ModularPoly& p) {
    if (p.is_zero()) return "0";
    std::string s = "[";
    for (size_t i = 0; i <= static_cast<size_t>(p.degree()); ++i) {
        if (i > 0) s += ", ";
        s += std::to_string(p.coeff(i));
    }
    s += "]";
    return s;
}

void expect_equal(const ModularPoly& euclidean, const ModularPoly& hgcd,
                  const std::string& tag) {
    if (!polys_equal(euclidean, hgcd)) {
        std::cerr << "  FAIL [" << tag << "]\n";
        std::cerr << "    Euclidean: deg=" << euclidean.degree()
                  << " " << poly_to_string(euclidean) << "\n";
        std::cerr << "    HGCD     : deg=" << hgcd.degree()
                  << " " << poly_to_string(hgcd) << "\n";
        assert(false && "HGCD output must match Euclidean bit-for-bit");
    }
}

// =================== Correctness tests (random polynomials) ===================

void test_correctness_small() {
    std::cout << "test_correctness_small (deg [10, 30])..." << std::endl;
    std::mt19937_64 rng(0xDEADBEEF12345678ULL);  // deterministic seed

    for (size_t deg : std::initializer_list<size_t>{10, 12, 16, 20, 25, 30}) {
        for (int trial = 0; trial < 3; ++trial) {
            ModularPoly a = random_poly(rng, deg, kSmallPrime);
            ModularPoly b = random_poly(rng, deg / 2 + 1, kSmallPrime);

            ModularPoly eu = ModularPoly::gcd(a, b, kSmallPrime);
            ModularPoly hg = gcd_via_hgcd(a, b, kSmallPrime);

            expect_equal(eu, hg, "small_deg=" + std::to_string(deg));
        }
    }
    std::cout << "  PASS" << std::endl;
}

void test_correctness_medium() {
    std::cout << "test_correctness_medium (deg [40, 100])..." << std::endl;
    std::mt19937_64 rng(0xCAFEBABE9ABCDEF0ULL);

    for (size_t deg : std::initializer_list<size_t>{40, 50, 70, 100}) {
        for (int trial = 0; trial < 3; ++trial) {
            ModularPoly a = random_poly(rng, deg, kSmallPrime);
            ModularPoly b = random_poly(rng, deg - 5, kSmallPrime);

            ModularPoly eu = ModularPoly::gcd(a, b, kSmallPrime);
            ModularPoly hg = gcd_via_hgcd(a, b, kSmallPrime);

            expect_equal(eu, hg, "medium_deg=" + std::to_string(deg));
        }
    }
    std::cout << "  PASS" << std::endl;
}

void test_correctness_large() {
    std::cout << "test_correctness_large (deg [150, 200])..." << std::endl;
    std::mt19937_64 rng(0x1234567890ABCDEFULL);

    for (size_t deg : std::initializer_list<size_t>{150, 200}) {
        ModularPoly a = random_poly(rng, deg, kSmallPrime);
        ModularPoly b = random_poly(rng, deg - 10, kSmallPrime);

        ModularPoly eu = ModularPoly::gcd(a, b, kSmallPrime);
        ModularPoly hg = gcd_via_hgcd(a, b, kSmallPrime);

        expect_equal(eu, hg, "large_deg=" + std::to_string(deg));
    }
    std::cout << "  PASS" << std::endl;
}

void test_correctness_with_common_factor() {
    std::cout << "test_correctness_with_common_factor..." << std::endl;
    std::mt19937_64 rng(0xABCD1234EFAB5678ULL);

    // Construct a = g * h_a, b = g * h_b where g has known degree.
    for (size_t common_deg : std::initializer_list<size_t>{3, 5, 10, 20, 50}) {
        for (size_t extra_deg : std::initializer_list<size_t>{5, 10, 30}) {
            ModularPoly g = random_poly(rng, common_deg, kSmallPrime);
            ModularPoly ha = random_poly(rng, extra_deg, kSmallPrime);
            ModularPoly hb = random_poly(rng, extra_deg + 5, kSmallPrime);

            ModularPoly a = ModularPoly::mul_raw(g, ha, kSmallPrime);
            ModularPoly b = ModularPoly::mul_raw(g, hb, kSmallPrime);

            ModularPoly eu = ModularPoly::gcd(a, b, kSmallPrime);
            ModularPoly hg = gcd_via_hgcd(a, b, kSmallPrime);

            expect_equal(eu, hg,
                "common=" + std::to_string(common_deg)
                + "_extra=" + std::to_string(extra_deg));
            // GCD degree must be at least common_deg.
            assert(static_cast<size_t>(eu.degree()) >= common_deg);
        }
    }
    std::cout << "  PASS" << std::endl;
}

void test_correctness_large_prime() {
    std::cout << "test_correctness_large_prime (p ~2^64)..." << std::endl;
    std::mt19937_64 rng(0xFEEDFACEDEADBEEFULL);

    for (size_t deg : std::initializer_list<size_t>{15, 30, 60}) {
        ModularPoly a = random_poly(rng, deg, kLargePrime);
        ModularPoly b = random_poly(rng, deg - 5, kLargePrime);

        ModularPoly eu = ModularPoly::gcd(a, b, kLargePrime);
        ModularPoly hg = gcd_via_hgcd(a, b, kLargePrime);

        expect_equal(eu, hg, "large_prime_deg=" + std::to_string(deg));
    }
    std::cout << "  PASS" << std::endl;
}

void test_correctness_swapped_args() {
    std::cout << "test_correctness_swapped_args..." << std::endl;
    std::mt19937_64 rng(0x0011223344556677ULL);

    // gcd(a, b) == gcd(b, a). HGCD must handle deg(a) < deg(b) correctly.
    for (size_t deg : std::initializer_list<size_t>{20, 50}) {
        ModularPoly small = random_poly(rng, deg / 2, kSmallPrime);
        ModularPoly large = random_poly(rng, deg, kSmallPrime);

        ModularPoly eu1 = ModularPoly::gcd(large, small, kSmallPrime);
        ModularPoly hg1 = gcd_via_hgcd(large, small, kSmallPrime);
        ModularPoly hg2 = gcd_via_hgcd(small, large, kSmallPrime);

        expect_equal(eu1, hg1, "swapped_normal_deg=" + std::to_string(deg));
        expect_equal(eu1, hg2, "swapped_reversed_deg=" + std::to_string(deg));
    }
    std::cout << "  PASS" << std::endl;
}

void test_correctness_at_threshold() {
    std::cout << "test_correctness_at_threshold (deg around kHGCDThreshold)..." << std::endl;
    std::mt19937_64 rng(0xAAAA5555AAAA5555ULL);

    // Probe degrees on either side of kHGCDThreshold to verify both base case
    // and recursive case agree with Euclidean.
    for (size_t deg = kHGCDThreshold - 4; deg <= kHGCDThreshold + 8; ++deg) {
        for (int trial = 0; trial < 2; ++trial) {
            ModularPoly a = random_poly(rng, deg, kSmallPrime);
            ModularPoly b = random_poly(rng, deg / 2 + 1, kSmallPrime);

            ModularPoly eu = ModularPoly::gcd(a, b, kSmallPrime);
            ModularPoly hg = gcd_via_hgcd(a, b, kSmallPrime);

            expect_equal(eu, hg, "threshold_deg=" + std::to_string(deg));
        }
    }
    std::cout << "  PASS" << std::endl;
}

void test_correctness_random_extensive() {
    std::cout << "test_correctness_random_extensive..." << std::endl;
    std::mt19937_64 rng(0x123456789ABCDEF0ULL);

    // Wide sweep covering many random configurations.
    for (int trial = 0; trial < 20; ++trial) {
        size_t da = 30 + (rng() % 80);
        size_t db = 5 + (rng() % da);
        ModularPoly a = random_poly(rng, da, kSmallPrime);
        ModularPoly b = random_poly(rng, db, kSmallPrime);

        ModularPoly eu = ModularPoly::gcd(a, b, kSmallPrime);
        ModularPoly hg = gcd_via_hgcd(a, b, kSmallPrime);

        expect_equal(eu, hg,
            "extensive trial=" + std::to_string(trial)
            + " da=" + std::to_string(da)
            + " db=" + std::to_string(db));
    }
    std::cout << "  PASS" << std::endl;
}

// =================== Edge case tests ===================

void test_edge_zero_polynomial() {
    std::cout << "test_edge_zero_polynomial..." << std::endl;
    std::mt19937_64 rng(42);

    ModularPoly zero;
    ModularPoly a = random_poly(rng, 20, kSmallPrime);

    // gcd(a, 0) = a (monic-normalized) — but a is already monic here.
    auto eu1 = ModularPoly::gcd(a, zero, kSmallPrime);
    auto hg1 = gcd_via_hgcd(a, zero, kSmallPrime);
    expect_equal(eu1, hg1, "gcd(a, 0)");

    // gcd(0, a) = a
    auto eu2 = ModularPoly::gcd(zero, a, kSmallPrime);
    auto hg2 = gcd_via_hgcd(zero, a, kSmallPrime);
    expect_equal(eu2, hg2, "gcd(0, a)");

    // gcd(0, 0) = 0
    auto eu3 = ModularPoly::gcd(zero, zero, kSmallPrime);
    auto hg3 = gcd_via_hgcd(zero, zero, kSmallPrime);
    expect_equal(eu3, hg3, "gcd(0, 0)");
    assert(hg3.is_zero());

    std::cout << "  PASS" << std::endl;
}

void test_edge_constant_polynomial() {
    std::cout << "test_edge_constant_polynomial..." << std::endl;
    std::mt19937_64 rng(123);

    ModularPoly one(uint64_t{1});
    ModularPoly seven(uint64_t{7});
    ModularPoly a = random_poly(rng, 50, kSmallPrime);

    // gcd(a, 1) = 1
    auto eu1 = ModularPoly::gcd(a, one, kSmallPrime);
    auto hg1 = gcd_via_hgcd(a, one, kSmallPrime);
    expect_equal(eu1, hg1, "gcd(a, 1)");
    assert(hg1.degree() == 0);
    assert(hg1.coeff(0) == 1);

    // gcd(a, 7) — both make it monic = 1
    auto eu2 = ModularPoly::gcd(a, seven, kSmallPrime);
    auto hg2 = gcd_via_hgcd(a, seven, kSmallPrime);
    expect_equal(eu2, hg2, "gcd(a, 7)");

    // gcd(7, a)
    auto eu3 = ModularPoly::gcd(seven, a, kSmallPrime);
    auto hg3 = gcd_via_hgcd(seven, a, kSmallPrime);
    expect_equal(eu3, hg3, "gcd(7, a)");

    std::cout << "  PASS" << std::endl;
}

void test_edge_coprime_polynomials() {
    std::cout << "test_edge_coprime_polynomials..." << std::endl;
    std::mt19937_64 rng(0xDEAD12345678BEEFULL);

    // Generate random pairs — high probability coprime; verify HGCD = Euclidean.
    int coprime_count = 0;
    for (int trial = 0; trial < 10; ++trial) {
        ModularPoly a = random_poly(rng, static_cast<size_t>(25 + trial), kSmallPrime);
        ModularPoly b = random_poly(rng, static_cast<size_t>(15 + trial), kSmallPrime);

        ModularPoly eu = ModularPoly::gcd(a, b, kSmallPrime);
        ModularPoly hg = gcd_via_hgcd(a, b, kSmallPrime);

        expect_equal(eu, hg, "coprime_trial=" + std::to_string(trial));
        if (eu.degree() == 0) ++coprime_count;
    }
    // At least some trials should produce coprime results.
    assert(coprime_count >= 5);
    std::cout << "  PASS (" << coprime_count << "/10 coprime)" << std::endl;
}

void test_edge_perfect_division() {
    std::cout << "test_edge_perfect_division (b divides a)..." << std::endl;
    std::mt19937_64 rng(0x55AA55AA55AA55AAULL);

    // a = b * q. gcd should equal b (monic-normalized).
    for (size_t b_deg : std::initializer_list<size_t>{5, 15, 30, 50}) {
        for (size_t q_deg : std::initializer_list<size_t>{3, 10, 25}) {
            ModularPoly b = random_poly(rng, b_deg, kSmallPrime);
            ModularPoly q = random_poly(rng, q_deg, kSmallPrime);
            ModularPoly a = ModularPoly::mul_raw(b, q, kSmallPrime);

            ModularPoly eu = ModularPoly::gcd(a, b, kSmallPrime);
            ModularPoly hg = gcd_via_hgcd(a, b, kSmallPrime);

            expect_equal(eu, hg, "perfect_div_b=" + std::to_string(b_deg)
                + "_q=" + std::to_string(q_deg));
            // GCD == b (both monic).
            assert(eu.degree() == static_cast<int>(b_deg));
        }
    }
    std::cout << "  PASS" << std::endl;
}

// =================== ENV parsing tests ===================
//
// Note: poly_hgcd_enabled() caches result via std::once_flag, so a single test
// binary observes a single ENV value. We verify the caching behavior by
// calling twice and confirming identical result (no flakiness).

void test_env_default_off() {
    std::cout << "test_env_default_off..." << std::endl;
    // In CI / default test invocation, GNFS_POLY_HGCD is unset → expect false.
    // If a user sets GNFS_POLY_HGCD=1 before invoking this test, this asserts
    // the alternative. We accept both outcomes and verify the *caching* stays
    // consistent.
    bool first = poly_hgcd_enabled();
    bool second = poly_hgcd_enabled();
    bool third = poly_hgcd_enabled();
    assert(first == second);
    assert(second == third);

    // Independent check: read ENV directly to confirm cache matches reality.
    const char* env = std::getenv("GNFS_POLY_HGCD");
    bool expected = (env != nullptr && std::string(env) == "1");
    assert(first == expected);

    std::cout << "  PASS (cached value = " << (first ? "true" : "false")
              << ", ENV=" << (env ? env : "<unset>") << ")" << std::endl;
}

void test_should_use_hgcd_dispatch() {
    std::cout << "test_should_use_hgcd_dispatch..." << std::endl;
    bool enabled = poly_hgcd_enabled();

    // Below threshold: should never dispatch.
    assert(!should_use_hgcd(0));
    assert(!should_use_hgcd(1));
    assert(!should_use_hgcd(kHGCDThreshold - 1));

    // At/above threshold: dispatch == enabled.
    assert(should_use_hgcd(kHGCDThreshold) == enabled);
    assert(should_use_hgcd(kHGCDThreshold + 10) == enabled);
    assert(should_use_hgcd(1000) == enabled);

    std::cout << "  PASS" << std::endl;
}

// =================== Performance info (not asserted) ===================

void perf_info_medium() {
    std::cout << "perf_info_medium (deg=100, informational)..." << std::endl;
    std::mt19937_64 rng(0xC0FFEE);
    using clk = std::chrono::high_resolution_clock;

    ModularPoly a = random_poly(rng, 100, kSmallPrime);
    ModularPoly b = random_poly(rng, 90, kSmallPrime);

    constexpr int iters = 5;

    auto t0 = clk::now();
    for (int i = 0; i < iters; ++i) {
        ModularPoly r = ModularPoly::gcd(a, b, kSmallPrime);
        (void)r;
    }
    auto t1 = clk::now();
    for (int i = 0; i < iters; ++i) {
        ModularPoly r = gcd_via_hgcd(a, b, kSmallPrime);
        (void)r;
    }
    auto t2 = clk::now();

    double eu_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
    double hg_ms = std::chrono::duration<double, std::milli>(t2 - t1).count() / iters;
    double speedup = (hg_ms > 0.0) ? (eu_ms / hg_ms) : 0.0;

    std::cout << "  Euclidean: " << eu_ms << " ms/call"
              << "  HGCD: " << hg_ms << " ms/call"
              << "  ratio (eu/hg): " << speedup << "x" << std::endl;
    std::cout << "  PASS (informational only)" << std::endl;
}

void perf_info_large() {
    std::cout << "perf_info_large (deg=500, informational)..." << std::endl;
    std::mt19937_64 rng(0xBEEF1234);
    using clk = std::chrono::high_resolution_clock;

    ModularPoly a = random_poly(rng, 500, kSmallPrime);
    ModularPoly b = random_poly(rng, 450, kSmallPrime);

    constexpr int iters = 2;

    auto t0 = clk::now();
    for (int i = 0; i < iters; ++i) {
        ModularPoly r = ModularPoly::gcd(a, b, kSmallPrime);
        (void)r;
    }
    auto t1 = clk::now();
    for (int i = 0; i < iters; ++i) {
        ModularPoly r = gcd_via_hgcd(a, b, kSmallPrime);
        (void)r;
    }
    auto t2 = clk::now();

    double eu_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
    double hg_ms = std::chrono::duration<double, std::milli>(t2 - t1).count() / iters;
    double speedup = (hg_ms > 0.0) ? (eu_ms / hg_ms) : 0.0;

    std::cout << "  Euclidean: " << eu_ms << " ms/call"
              << "  HGCD: " << hg_ms << " ms/call"
              << "  ratio (eu/hg): " << speedup << "x" << std::endl;
    std::cout << "  Note: HGCD ROI requires sub-quadratic M(n); schoolbook"
              << " multiplication keeps Euclidean ahead at these sizes." << std::endl;
    std::cout << "  PASS (informational only)" << std::endl;
}

}  // namespace

int main() {
    std::cout << "=== test_half_gcd ===" << std::endl;
    std::cout << "kHGCDThreshold = " << kHGCDThreshold << std::endl;
    std::cout << "GNFS_POLY_HGCD = "
              << (std::getenv("GNFS_POLY_HGCD") ? std::getenv("GNFS_POLY_HGCD") : "<unset>")
              << std::endl;
    std::cout << std::endl;

    // 8 correctness tests over deg range [10, 200].
    test_correctness_small();
    test_correctness_medium();
    test_correctness_large();
    test_correctness_with_common_factor();
    test_correctness_large_prime();
    test_correctness_swapped_args();
    test_correctness_at_threshold();
    test_correctness_random_extensive();

    // 4 edge cases.
    test_edge_zero_polynomial();
    test_edge_constant_polynomial();
    test_edge_coprime_polynomials();
    test_edge_perfect_division();

    // 2 ENV parsing tests.
    test_env_default_off();
    test_should_use_hgcd_dispatch();

    // 2 informational perf tests (not asserted).
    perf_info_medium();
    perf_info_large();

    std::cout << std::endl;
    std::cout << "All tests passed." << std::endl;
    return 0;
}
