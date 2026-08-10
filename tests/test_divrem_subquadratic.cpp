// Unit tests for gnfs::polynomial::divrem_modp and the ENV-gated three-state
// helper divrem_subquadratic_mode / divrem_subquadratic_enabled.
//
// Goals:
//   1. ENV parsing: unset / "auto" → Auto; "0" / "off" → ForceOff;
//      "1" / "on" → ForceOn; anything else → Auto.
//   2. Correctness: Newton-reciprocal output bit-for-bit identical to
//      schoolbook divrem across small / medium / large degree pairs,
//      including extreme shapes (num is multiple of den, num degree
//      smaller than den, num all zero, den is constant).
//   3. Threshold: when num.size() < kDivremSubquadraticThreshold even
//      gate=ForceOn must route to schoolbook (no observable difference
//      from a parity standpoint, but exercises the dispatch branch).
//   4. Perf info: schoolbook vs subquadratic wall time at deg=500.
//
// Notes:
//   divrem_subquadratic_mode() caches the env via std::once_flag. The
//   parsing-focused tests use divrem_subquadratic_reset_env_cache_for_testing()
//   to permit multi-value sweeps within a single test binary.

#include "gnfs/polynomial/divrem_subquadratic.hpp"
#include "gnfs/util/primes.hpp"
#include "support/test_check.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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

using gnfs::polynomial::divrem_modp;
using gnfs::polynomial::divrem_modp_schoolbook;
using gnfs::polynomial::divrem_subquadratic_enabled;
using gnfs::polynomial::divrem_subquadratic_mode;
using gnfs::polynomial::divrem_subquadratic_reset_env_cache_for_testing;
using gnfs::polynomial::DivremSubquadraticMode;
using gnfs::polynomial::kDivremSubquadraticThreshold;

namespace {

// (2^31 - 1) — largest Mersenne prime below 2^32. uint64 * uint64 of
// reduced coefficients fits comfortably (max ~ 2^62).
constexpr uint64_t kPrime = (1ULL << 31) - 1;

// Smaller prime for unit-style boundary tests where exact arithmetic
// is easier to verify by hand.
constexpr uint64_t kSmallPrime = 7;

// Trim trailing zeros in a freshly produced reference polynomial so
// comparisons against the helper output (which trims internally) line
// up.
void trim(std::vector<uint64_t>& v) {
    while (!v.empty() && v.back() == 0)
        v.pop_back();
}

std::vector<uint64_t> random_poly(std::mt19937_64& rng, size_t size, uint64_t p) {
    std::vector<uint64_t> v(size);
    for (size_t i = 0; i < size; ++i) {
        v[i] = rng() % p;
    }
    // Ensure leading coefficient is non-zero so deg(v) == size - 1.
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

void expect_equal_pair(const std::vector<uint64_t>& q_expected,
                       const std::vector<uint64_t>& r_expected,
                       const std::vector<uint64_t>& q_actual, const std::vector<uint64_t>& r_actual,
                       const std::string& tag) {
    if (!vectors_equal(q_expected, q_actual) || !vectors_equal(r_expected, r_actual)) {
        std::cerr << "  FAIL [" << tag << "]\n";
        std::cerr << "    quot expected size=" << q_expected.size() << " "
                  << vec_to_string(q_expected) << "\n";
        std::cerr << "    quot actual   size=" << q_actual.size() << " " << vec_to_string(q_actual)
                  << "\n";
        std::cerr << "    rem  expected size=" << r_expected.size() << " "
                  << vec_to_string(r_expected) << "\n";
        std::cerr << "    rem  actual   size=" << r_actual.size() << " " << vec_to_string(r_actual)
                  << "\n";
        GNFS_TEST_CHECK(false && "divrem output must match schoolbook bit-for-bit");
    }
}

// ====================== ENV parsing tests ======================

void test_env_unset_default_auto() {
    std::cout << "test_env_unset_default_auto..." << std::endl;
    gnfs_unsetenv("GNFS_POLY_DIVREM_SUBQUADRATIC");
    divrem_subquadratic_reset_env_cache_for_testing();
    auto mode = divrem_subquadratic_mode();
    GNFS_TEST_CHECK(mode == DivremSubquadraticMode::Auto);
    GNFS_TEST_CHECK(divrem_subquadratic_enabled() == false); // Auto is conservative
    std::cout << "  PASS (mode=Auto, enabled=false)" << std::endl;
}

void test_env_explicit_off() {
    std::cout << "test_env_explicit_off..." << std::endl;
    for (const char* v : {"0", "off"}) {
        gnfs_setenv("GNFS_POLY_DIVREM_SUBQUADRATIC", v);
        divrem_subquadratic_reset_env_cache_for_testing();
        auto mode = divrem_subquadratic_mode();
        if (mode != DivremSubquadraticMode::ForceOff) {
            std::cerr << "  FAIL: ENV=\"" << v << "\" gave mode=" << static_cast<int>(mode)
                      << ", expected ForceOff\n";
            GNFS_TEST_CHECK(false);
        }
        GNFS_TEST_CHECK(divrem_subquadratic_enabled() == false);
    }
    gnfs_unsetenv("GNFS_POLY_DIVREM_SUBQUADRATIC");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::cout << "  PASS (both \"0\" and \"off\" → ForceOff, enabled=false)" << std::endl;
}

void test_env_explicit_on() {
    std::cout << "test_env_explicit_on..." << std::endl;
    for (const char* v : {"1", "on"}) {
        gnfs_setenv("GNFS_POLY_DIVREM_SUBQUADRATIC", v);
        divrem_subquadratic_reset_env_cache_for_testing();
        auto mode = divrem_subquadratic_mode();
        if (mode != DivremSubquadraticMode::ForceOn) {
            std::cerr << "  FAIL: ENV=\"" << v << "\" gave mode=" << static_cast<int>(mode)
                      << ", expected ForceOn\n";
            GNFS_TEST_CHECK(false);
        }
        GNFS_TEST_CHECK(divrem_subquadratic_enabled() == true);
    }
    gnfs_unsetenv("GNFS_POLY_DIVREM_SUBQUADRATIC");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::cout << "  PASS (both \"1\" and \"on\" → ForceOn, enabled=true)" << std::endl;
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
        gnfs_setenv("GNFS_POLY_DIVREM_SUBQUADRATIC", v);
        divrem_subquadratic_reset_env_cache_for_testing();
        auto mode = divrem_subquadratic_mode();
        if (mode != DivremSubquadraticMode::Auto) {
            std::cerr << "  FAIL: ENV=\"" << v << "\" gave mode=" << static_cast<int>(mode)
                      << ", expected Auto\n";
            GNFS_TEST_CHECK(false);
        }
    }
    gnfs_unsetenv("GNFS_POLY_DIVREM_SUBQUADRATIC");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::cout << "  PASS (" << (sizeof(unrecognized) / sizeof(unrecognized[0]))
              << " unrecognized values mapped to Auto)" << std::endl;
}

// ====================== Schoolbook unit tests ======================

void test_schoolbook_basic_division() {
    std::cout << "test_schoolbook_basic_division..." << std::endl;
    // (x^3 + 2x^2 + x + 1) / (x + 1) mod 7
    // Expected: quotient x^2 + x + 0  → coeffs [0, 1, 1]; remainder 1 → [1]
    // Verification: (x + 1) * (x^2 + x) = x^3 + x^2 + x^2 + x = x^3 + 2x^2 + x
    //   + remainder 1 = x^3 + 2x^2 + x + 1. Correct.
    std::vector<uint64_t> num{1, 1, 2, 1}; // coeffs[0..3]
    std::vector<uint64_t> den{1, 1};
    std::vector<uint64_t> q, r;
    divrem_modp_schoolbook(num, den, kSmallPrime, q, r);
    std::vector<uint64_t> q_exp{0, 1, 1};
    trim(q_exp);
    std::vector<uint64_t> r_exp{1};
    expect_equal_pair(q_exp, r_exp, q, r, "basic_division");
    std::cout << "  PASS (quot=" << vec_to_string(q) << " rem=" << vec_to_string(r) << ")"
              << std::endl;
}

void test_schoolbook_num_smaller_than_den() {
    std::cout << "test_schoolbook_num_smaller_than_den..." << std::endl;
    std::vector<uint64_t> num{3, 5};    // deg 1
    std::vector<uint64_t> den{1, 0, 1}; // x^2 + 1, deg 2
    std::vector<uint64_t> q, r;
    divrem_modp_schoolbook(num, den, kSmallPrime, q, r);
    GNFS_TEST_CHECK(q.empty());
    std::vector<uint64_t> r_exp{3, 5};
    expect_equal_pair(std::vector<uint64_t>{}, r_exp, q, r, "num<den");
    std::cout << "  PASS (quot empty, rem=" << vec_to_string(r) << ")" << std::endl;
}

void test_schoolbook_num_multiple_of_den() {
    std::cout << "test_schoolbook_num_multiple_of_den..." << std::endl;
    // den = x + 1, q_truth = x^2 + 2x + 3
    // num = (x + 1) * (x^2 + 2x + 3) = x^3 + 3x^2 + 5x + 3 mod 7
    std::vector<uint64_t> den{1, 1};
    std::vector<uint64_t> q_truth{3, 2, 1};
    std::vector<uint64_t> num{3, 5, 3, 1};
    std::vector<uint64_t> q, r;
    divrem_modp_schoolbook(num, den, kSmallPrime, q, r);
    GNFS_TEST_CHECK(r.empty()); // exact division
    expect_equal_pair(q_truth, std::vector<uint64_t>{}, q, r, "num = q*den");
    std::cout << "  PASS (rem all zeros)" << std::endl;
}

void test_schoolbook_den_constant() {
    std::cout << "test_schoolbook_den_constant..." << std::endl;
    // num = 3x^2 + 5x + 4, den = 1 (monic constant). quot = num, rem empty.
    std::vector<uint64_t> num{4, 5, 3};
    std::vector<uint64_t> den{1};
    std::vector<uint64_t> q, r;
    divrem_modp_schoolbook(num, den, kSmallPrime, q, r);
    expect_equal_pair(num, std::vector<uint64_t>{}, q, r, "den=1");
    std::cout << "  PASS (quot=num, rem empty)" << std::endl;
}

void test_schoolbook_num_zero() {
    std::cout << "test_schoolbook_num_zero..." << std::endl;
    std::vector<uint64_t> num;
    std::vector<uint64_t> den{1, 2, 3};
    std::vector<uint64_t> q, r;
    divrem_modp_schoolbook(num, den, kSmallPrime, q, r);
    GNFS_TEST_CHECK(q.empty() && r.empty());
    std::cout << "  PASS (both quot and rem empty)" << std::endl;
}

// ====================== Subquadratic parity tests ======================

// Runs `divrem_modp` and `divrem_modp_schoolbook` on the same (num, den)
// and asserts bit-for-bit identical output. Caller is responsible for
// having set the gate to ForceOn (and reset the env cache) so the
// helper's Newton path is actually exercised above the threshold.
void check_parity(const std::vector<uint64_t>& num, const std::vector<uint64_t>& den, uint64_t p,
                  const std::string& tag) {
    std::vector<uint64_t> q_subq, r_subq, q_school, r_school;
    divrem_modp(num, den, p, q_subq, r_subq);
    divrem_modp_schoolbook(num, den, p, q_school, r_school);
    expect_equal_pair(q_school, r_school, q_subq, r_subq, tag);
}

void test_parity_deg_50() {
    std::cout << "test_parity_deg_50..." << std::endl;
    gnfs_setenv("GNFS_POLY_DIVREM_SUBQUADRATIC", "1");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::mt19937_64 rng(0x1111222233334444ULL);
    for (int trial = 0; trial < 4; ++trial) {
        auto num = random_poly(rng, 50, kPrime);
        auto den = random_poly(rng, 10, kPrime);
        check_parity(num, den, kPrime, "deg_50_trial_" + std::to_string(trial));
    }
    gnfs_unsetenv("GNFS_POLY_DIVREM_SUBQUADRATIC");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::cout << "  PASS (4 trials at num.size()=50)" << std::endl;
}

void test_parity_deg_200() {
    std::cout << "test_parity_deg_200..." << std::endl;
    gnfs_setenv("GNFS_POLY_DIVREM_SUBQUADRATIC", "1");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::mt19937_64 rng(0x5555666677778888ULL);
    for (int trial = 0; trial < 3; ++trial) {
        auto num = random_poly(rng, 200, kPrime);
        auto den = random_poly(rng, 40, kPrime);
        check_parity(num, den, kPrime, "deg_200_trial_" + std::to_string(trial));
    }
    gnfs_unsetenv("GNFS_POLY_DIVREM_SUBQUADRATIC");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::cout << "  PASS (3 trials at num.size()=200)" << std::endl;
}

void test_parity_deg_500() {
    std::cout << "test_parity_deg_500..." << std::endl;
    gnfs_setenv("GNFS_POLY_DIVREM_SUBQUADRATIC", "1");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::mt19937_64 rng(0x9999AAAABBBBCCCCULL);
    for (int trial = 0; trial < 2; ++trial) {
        auto num = random_poly(rng, 500, kPrime);
        auto den = random_poly(rng, 100, kPrime);
        check_parity(num, den, kPrime, "deg_500_trial_" + std::to_string(trial));
    }
    gnfs_unsetenv("GNFS_POLY_DIVREM_SUBQUADRATIC");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::cout << "  PASS (2 trials at num.size()=500)" << std::endl;
}

void test_threshold_below_routes_to_schoolbook() {
    std::cout << "test_threshold_below_routes_to_schoolbook..." << std::endl;
    // Even with ForceOn, num.size() < kDivremSubquadraticThreshold (= 32)
    // must route to schoolbook. We cannot directly observe the route
    // (no telemetry), but we can verify the result still matches
    // schoolbook exactly — and that the helper does not throw / abort
    // when the Newton path is bypassed (which would happen if dispatch
    // accidentally entered the Newton kernel at degree < threshold and
    // the inversion misbehaved at very small precision).
    gnfs_setenv("GNFS_POLY_DIVREM_SUBQUADRATIC", "1");
    divrem_subquadratic_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(divrem_subquadratic_enabled());

    std::mt19937_64 rng(0xDEADBEEFCAFEBABEULL);
    // Pick num.size() values strictly below the threshold of 32.
    for (size_t sz : {1u, 5u, 10u, 20u, 31u}) {
        auto num = random_poly(rng, sz, kPrime);
        auto den = random_poly(rng, std::max<size_t>(1, sz / 3), kPrime);
        check_parity(num, den, kPrime, "below_threshold_" + std::to_string(sz));
    }
    // Single-coefficient den (constant) at small num.
    {
        auto num = random_poly(rng, 5, kPrime);
        std::vector<uint64_t> den{3};
        check_parity(num, den, kPrime, "small_const_den");
    }
    gnfs_unsetenv("GNFS_POLY_DIVREM_SUBQUADRATIC");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::cout << "  PASS (all below-threshold dispatches matched schoolbook)" << std::endl;
}

void test_boundary_den_constant() {
    std::cout << "test_boundary_den_constant..." << std::endl;
    // den is non-monic constant; quot = num * inv(den_const), rem empty.
    gnfs_setenv("GNFS_POLY_DIVREM_SUBQUADRATIC", "1");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::mt19937_64 rng(0xBEEFCAFE12345678ULL);
    auto num = random_poly(rng, 60, kPrime);
    std::vector<uint64_t> den{42};
    check_parity(num, den, kPrime, "den_constant");
    gnfs_unsetenv("GNFS_POLY_DIVREM_SUBQUADRATIC");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::cout << "  PASS (constant den, both paths agree)" << std::endl;
}

void test_boundary_num_zero() {
    std::cout << "test_boundary_num_zero..." << std::endl;
    gnfs_setenv("GNFS_POLY_DIVREM_SUBQUADRATIC", "1");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::vector<uint64_t> num; // zero polynomial
    std::vector<uint64_t> den{1, 2, 3, 4, 5};
    std::vector<uint64_t> q, r;
    divrem_modp(num, den, kPrime, q, r);
    GNFS_TEST_CHECK(q.empty() && r.empty());
    gnfs_unsetenv("GNFS_POLY_DIVREM_SUBQUADRATIC");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::cout << "  PASS (zero num → quot=rem=empty)" << std::endl;
}

void test_random_sweep() {
    std::cout << "test_random_sweep..." << std::endl;
    gnfs_setenv("GNFS_POLY_DIVREM_SUBQUADRATIC", "1");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::mt19937_64 rng(0xFEDCBA9876543210ULL);
    // 10 shape pairs covering: just-above threshold, very skewed (long
    // num / short den), near-balanced (num ~ 2*den), all medium-large.
    const std::pair<size_t, size_t> shapes[] = {
        {40, 5},    {50, 25},  {64, 8},    {100, 30}, {150, 50},
        {200, 100}, {256, 64}, {300, 150}, {400, 80}, {500, 250},
    };
    for (const auto& [na, nb] : shapes) {
        auto num = random_poly(rng, na, kPrime);
        auto den = random_poly(rng, nb, kPrime);
        check_parity(num, den, kPrime, "sweep_" + std::to_string(na) + "x" + std::to_string(nb));
    }
    gnfs_unsetenv("GNFS_POLY_DIVREM_SUBQUADRATIC");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::cout << "  PASS (" << (sizeof(shapes) / sizeof(shapes[0])) << " shapes)" << std::endl;
}

void test_num_exact_multiple_subq() {
    std::cout << "test_num_exact_multiple_subq..." << std::endl;
    // Confirm Newton path gives zero remainder when input is an exact
    // multiple of den.
    gnfs_setenv("GNFS_POLY_DIVREM_SUBQUADRATIC", "1");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::mt19937_64 rng(0x0123456789ABCDEFULL);
    // Pick random den (deg 30) and quotient (deg 60), multiply to get num.
    auto den = random_poly(rng, 31, kPrime);
    auto quot_truth = random_poly(rng, 61, kPrime);
    // num = den * quot_truth (schoolbook mul for the construction step).
    std::vector<uint64_t> num(den.size() + quot_truth.size() - 1, 0);
    for (size_t i = 0; i < den.size(); ++i) {
        for (size_t j = 0; j < quot_truth.size(); ++j) {
            uint64_t pp = gnfs::util::mul_mod_u64(den[i], quot_truth[j], kPrime);
            num[i + j] = gnfs::util::add_mod_u64(num[i + j], pp, kPrime);
        }
    }
    std::vector<uint64_t> q, r;
    divrem_modp(num, den, kPrime, q, r);
    GNFS_TEST_CHECK(r.empty() && "exact division must yield empty remainder");
    trim(quot_truth);
    if (!vectors_equal(quot_truth, q)) {
        std::cerr << "  FAIL: recovered quotient differs from construction\n";
        std::cerr << "    truth size=" << quot_truth.size() << " " << vec_to_string(quot_truth)
                  << "\n";
        std::cerr << "    actual size=" << q.size() << " " << vec_to_string(q) << "\n";
        GNFS_TEST_CHECK(false);
    }
    gnfs_unsetenv("GNFS_POLY_DIVREM_SUBQUADRATIC");
    divrem_subquadratic_reset_env_cache_for_testing();
    std::cout << "  PASS (exact multiple: rem empty, quotient recovered)" << std::endl;
}

// ====================== Perf info (not asserted) ======================

void perf_info_deg_500() {
    std::cout << "perf_info_deg_500 (informational)..." << std::endl;
    using clk = std::chrono::high_resolution_clock;
    std::mt19937_64 rng(0xC0FFEE0BADCAFEULL);
    const auto num = random_poly(rng, 500, kPrime);
    const auto den = random_poly(rng, 100, kPrime);

    constexpr int iters = 3;
    std::vector<uint64_t> q, r;

    // Schoolbook baseline (ForceOff guarantees the dispatch routes via
    // schoolbook even though the gate is read after parsing here).
    gnfs_setenv("GNFS_POLY_DIVREM_SUBQUADRATIC", "0");
    divrem_subquadratic_reset_env_cache_for_testing();
    auto t0 = clk::now();
    for (int i = 0; i < iters; ++i) {
        divrem_modp(num, den, kPrime, q, r);
    }
    auto t1 = clk::now();

    // Newton-reciprocal.
    gnfs_setenv("GNFS_POLY_DIVREM_SUBQUADRATIC", "1");
    divrem_subquadratic_reset_env_cache_for_testing();
    auto t2 = clk::now();
    for (int i = 0; i < iters; ++i) {
        divrem_modp(num, den, kPrime, q, r);
    }
    auto t3 = clk::now();

    double sb_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
    double subq_ms = std::chrono::duration<double, std::milli>(t3 - t2).count() / iters;
    double ratio = (subq_ms > 0.0) ? (sb_ms / subq_ms) : 0.0;

    std::cout << "  schoolbook: " << sb_ms << " ms/call" << "  subquadratic: " << subq_ms
              << " ms/call" << "  ratio (sb/subq): " << ratio << "x" << std::endl;
    std::cout << "  PASS (informational only — Newton wins asymptotically;"
              << " at deg=500 with schoolbook M(n) the ratio may be < 1)" << std::endl;

    gnfs_unsetenv("GNFS_POLY_DIVREM_SUBQUADRATIC");
    divrem_subquadratic_reset_env_cache_for_testing();
}

} // namespace

int main() {
    std::cout << "=== test_divrem_subquadratic ===" << std::endl;
    std::cout << "threshold = " << kDivremSubquadraticThreshold << " coefficients" << std::endl;
    std::cout << std::endl;

    // 4 ENV parsing tests.
    test_env_unset_default_auto();
    test_env_explicit_off();
    test_env_explicit_on();
    test_env_unrecognized_to_auto();

    // 5 schoolbook unit / boundary tests.
    test_schoolbook_basic_division();
    test_schoolbook_num_smaller_than_den();
    test_schoolbook_num_multiple_of_den();
    test_schoolbook_den_constant();
    test_schoolbook_num_zero();

    // 7 subquadratic parity / boundary tests.
    test_parity_deg_50();
    test_parity_deg_200();
    test_parity_deg_500();
    test_threshold_below_routes_to_schoolbook();
    test_boundary_den_constant();
    test_boundary_num_zero();
    test_random_sweep();
    test_num_exact_multiple_subq();

    // 1 informational perf probe.
    perf_info_deg_500();

    std::cout << std::endl;
    std::cout << "All tests passed." << std::endl;
    return 0;
}
