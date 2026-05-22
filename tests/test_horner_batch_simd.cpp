// test_horner_batch_simd.cpp — Correctness tests for the SIMD-accelerated
// batched Horner polynomial evaluation helper.
//
// Strategy
// --------
// Every test that exercises the SIMD path runs both the scalar reference
// (`batch_eval_poly_int64_scalar`) and the dispatched helper, then asserts
// per-index equality. Horner evaluation is a deterministic int64 operation
// sequence; for inputs that do not overflow int64 the SIMD and scalar
// paths must produce bit-identical results.
//
// Coverage:
// * ENV parsing (GNFS_POLY_HORNER_BATCH_SIMD = auto / 0 / 1 / unset / garbage).
// * Empty xs / empty coeffs edge cases.
// * Degree-0 (constant), degree-1 (linear), degree-5 / 10 random polynomials.
// * Aligned and unaligned batch sizes (tests the SIMD residual tail).
// * Single x (len=1) — must take the scalar tail path.
// * Negative x / negative coeffs (signed correctness).
// * ForceOff vs Auto parity.
// * Perf info probe (1M eval, no assertion — wall time printed).
//
// Build: registered in CMakeLists.txt instant tier as HornerBatchSimd.

#include <gnfs/polynomial/horner_batch_simd.hpp>

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

namespace poly = gnfs::polynomial;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void set_env_and_reload(const char* value) {
    if (value == nullptr) {
        ::unsetenv("GNFS_POLY_HORNER_BATCH_SIMD");
    } else {
        ::setenv("GNFS_POLY_HORNER_BATCH_SIMD", value, 1);
    }
    poly::horner_batch_simd_reset_env_cache_for_testing();
}

static bool batch_results_equal(const std::vector<std::int64_t>& a,
                                const std::vector<std::int64_t>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static void compare_batch(const std::vector<std::int64_t>& coeffs,
                          const std::vector<std::int64_t>& xs,
                          const char* label) {
    std::vector<std::int64_t> scalar_out(xs.size(), 0xDEAD);
    std::vector<std::int64_t> dispatch_out(xs.size(), 0xBEEF);
    poly::batch_eval_poly_int64_scalar(coeffs, xs, scalar_out);
    poly::batch_eval_poly_int64(coeffs, xs, dispatch_out);
    if (!batch_results_equal(scalar_out, dispatch_out)) {
        std::fprintf(stderr, "  batch mismatch [%s] n=%zu\n",
                     label, xs.size());
        for (std::size_t i = 0; i < xs.size(); ++i) {
            if (scalar_out[i] != dispatch_out[i]) {
                std::fprintf(stderr,
                    "    index %zu: x=%lld scalar=%lld dispatch=%lld\n",
                    i,
                    static_cast<long long>(xs[i]),
                    static_cast<long long>(scalar_out[i]),
                    static_cast<long long>(dispatch_out[i]));
                if (i >= 4) break;  // limit noise
            }
        }
        tests_failed++;
        return;
    }
}

// ---------------------------------------------------------------------------
// ENV parsing tests
// ---------------------------------------------------------------------------

static void test_env_unset_yields_auto() {
    set_env_and_reload(nullptr);
    TEST_ASSERT(poly::horner_batch_simd_mode() == poly::HornerBatchSimdMode::Auto,
                "unset env should yield Auto mode");
    TEST_PASS("env_unset_yields_auto");
}

static void test_env_zero_yields_force_off() {
    set_env_and_reload("0");
    TEST_ASSERT(poly::horner_batch_simd_mode() == poly::HornerBatchSimdMode::ForceOff,
                "env=0 should yield ForceOff");
    TEST_ASSERT(!poly::horner_batch_simd_enabled(),
                "env=0 should disable SIMD");
    set_env_and_reload(nullptr);
    TEST_PASS("env_zero_yields_force_off");
}

static void test_env_one_yields_force_on() {
    set_env_and_reload("1");
    TEST_ASSERT(poly::horner_batch_simd_mode() == poly::HornerBatchSimdMode::ForceOn,
                "env=1 should yield ForceOn");
    // enabled() depends on compile-time support, but at minimum the mode
    // resolved correctly.
    set_env_and_reload(nullptr);
    TEST_PASS("env_one_yields_force_on");
}

static void test_env_garbage_yields_auto() {
    set_env_and_reload("garbage");
    TEST_ASSERT(poly::horner_batch_simd_mode() == poly::HornerBatchSimdMode::Auto,
                "garbage env should yield Auto");
    set_env_and_reload("auto");
    TEST_ASSERT(poly::horner_batch_simd_mode() == poly::HornerBatchSimdMode::Auto,
                "'auto' string should yield Auto");
    set_env_and_reload("");
    TEST_ASSERT(poly::horner_batch_simd_mode() == poly::HornerBatchSimdMode::Auto,
                "empty string should yield Auto");
    set_env_and_reload(nullptr);
    TEST_PASS("env_garbage_yields_auto");
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

static void test_empty_xs() {
    set_env_and_reload(nullptr);
    std::vector<std::int64_t> coeffs = {1, 2, 3};
    std::vector<std::int64_t> xs;
    std::vector<std::int64_t> ys;
    // Must not touch ys, must not crash.
    poly::batch_eval_poly_int64(coeffs, xs, ys);
    TEST_ASSERT(ys.empty(), "empty xs should leave ys empty");
    TEST_PASS("empty_xs");
}

static void test_empty_coeffs() {
    set_env_and_reload(nullptr);
    std::vector<std::int64_t> coeffs;  // degree -1
    std::vector<std::int64_t> xs = {5, 10, -3, 0, 7};
    std::vector<std::int64_t> ys(xs.size(), 0xCAFE);
    poly::batch_eval_poly_int64(coeffs, xs, ys);
    for (std::size_t i = 0; i < xs.size(); ++i) {
        TEST_ASSERT(ys[i] == 0,
                    "empty coeffs should evaluate to zero everywhere");
    }
    TEST_PASS("empty_coeffs");
}

static void test_deg0_constant() {
    set_env_and_reload(nullptr);
    std::vector<std::int64_t> coeffs = {42};  // p(x) = 42
    std::vector<std::int64_t> xs = {0, 1, -1, 100, -100, 12345};
    std::vector<std::int64_t> ys(xs.size(), 0);
    poly::batch_eval_poly_int64(coeffs, xs, ys);
    for (std::size_t i = 0; i < xs.size(); ++i) {
        TEST_ASSERT(ys[i] == 42, "constant poly should yield coeffs[0]");
    }
    compare_batch(coeffs, xs, "deg0_constant_parity");
    TEST_PASS("deg0_constant");
}

static void test_deg1_linear() {
    set_env_and_reload(nullptr);
    // p(x) = 3 + 5*x
    std::vector<std::int64_t> coeffs = {3, 5};
    std::vector<std::int64_t> xs = {0, 1, -1, 10, -10, 100, 50, 7};
    std::vector<std::int64_t> ys(xs.size(), 0);
    poly::batch_eval_poly_int64(coeffs, xs, ys);
    for (std::size_t i = 0; i < xs.size(); ++i) {
        const std::int64_t expected = 3 + 5 * xs[i];
        TEST_ASSERT(ys[i] == expected, "linear poly p(x)=3+5x mismatch");
    }
    compare_batch(coeffs, xs, "deg1_linear_parity");
    TEST_PASS("deg1_linear");
}

static void test_deg5_random_100() {
    set_env_and_reload(nullptr);
    std::mt19937_64 rng(0xCAFE'BABE'DEAD'BEEFULL);
    // Coefficients in [-100, 100] keep |acc| small enough at deg=5 with
    // |x| <= 50 — worst-case |acc| ≈ 100 * 50^5 ≈ 3.125e10, fits int64
    // (max ≈ 9.2e18) with huge headroom.
    std::uniform_int_distribution<std::int64_t> coeff_dist(-100, 100);
    std::uniform_int_distribution<std::int64_t> x_dist(-50, 50);
    std::vector<std::int64_t> coeffs(6);
    for (auto& c : coeffs) c = coeff_dist(rng);
    std::vector<std::int64_t> xs(100);
    for (auto& x : xs) x = x_dist(rng);
    compare_batch(coeffs, xs, "deg5_random_100");
    TEST_PASS("deg5_random_100");
}

static void test_deg10_random_1000() {
    set_env_and_reload(nullptr);
    std::mt19937_64 rng(0x1234'5678'ABCD'EF01ULL);
    // deg=10 with |x| <= 4 keeps |acc| <= 100 * 4^10 ≈ 1e8 (safe).
    std::uniform_int_distribution<std::int64_t> coeff_dist(-100, 100);
    std::uniform_int_distribution<std::int64_t> x_dist(-4, 4);
    std::vector<std::int64_t> coeffs(11);
    for (auto& c : coeffs) c = coeff_dist(rng);
    std::vector<std::int64_t> xs(1000);
    for (auto& x : xs) x = x_dist(rng);
    compare_batch(coeffs, xs, "deg10_random_1000");
    TEST_PASS("deg10_random_1000");
}

static void test_force_off_vs_auto_parity() {
    std::mt19937_64 rng(0xABCD'1234'5678'9ABCULL);
    std::uniform_int_distribution<std::int64_t> coeff_dist(-50, 50);
    std::uniform_int_distribution<std::int64_t> x_dist(-20, 20);
    std::vector<std::int64_t> coeffs(8);
    for (auto& c : coeffs) c = coeff_dist(rng);
    std::vector<std::int64_t> xs(50);
    for (auto& x : xs) x = x_dist(rng);

    // ForceOff path.
    set_env_and_reload("0");
    std::vector<std::int64_t> off_out(xs.size(), 0);
    poly::batch_eval_poly_int64(coeffs, xs, off_out);
    TEST_ASSERT(!poly::horner_batch_simd_enabled(),
                "ForceOff should disable SIMD");

    // Auto path.
    set_env_and_reload(nullptr);
    std::vector<std::int64_t> auto_out(xs.size(), 0);
    poly::batch_eval_poly_int64(coeffs, xs, auto_out);

    TEST_ASSERT(batch_results_equal(off_out, auto_out),
                "ForceOff and Auto must agree bit-for-bit");
    TEST_PASS("force_off_vs_auto_parity");
}

static void test_single_x_tail_path() {
    set_env_and_reload(nullptr);
    // Single x exercises the scalar tail of both NEON (lanes=2) and AVX2
    // (lanes=4) batched loops.
    std::vector<std::int64_t> coeffs = {1, -2, 3, -4, 5, -6, 7};
    std::vector<std::int64_t> xs = {13};
    std::vector<std::int64_t> ys(1, 0);
    poly::batch_eval_poly_int64(coeffs, xs, ys);
    // Recompute by hand.
    const std::int64_t expected =
        poly::horner_eval_one_scalar(coeffs, 13);
    TEST_ASSERT(ys[0] == expected, "single-x evaluation should match scalar");
    TEST_PASS("single_x_tail_path");
}

static void test_unaligned_len() {
    set_env_and_reload(nullptr);
    // 7 xs forces NEON to do 3 pair iterations + 1 scalar tail; AVX2 to
    // do 1 quad iteration + 3 scalar tail iterations.
    std::vector<std::int64_t> coeffs = {-1, 2, -3, 4};
    std::vector<std::int64_t> xs = {1, 2, 3, 4, 5, 6, 7};
    compare_batch(coeffs, xs, "unaligned_7");

    // Other unaligned sizes covering both SIMD widths.
    for (std::size_t n : {1u, 2u, 3u, 4u, 5u, 6u, 8u, 9u, 11u, 13u, 33u}) {
        std::vector<std::int64_t> xs_n(n);
        for (std::size_t i = 0; i < n; ++i) {
            xs_n[i] = static_cast<std::int64_t>(i) - 5;
        }
        compare_batch(coeffs, xs_n, "unaligned_sweep");
    }
    TEST_PASS("unaligned_len");
}

static void test_negative_values() {
    set_env_and_reload(nullptr);
    // p(x) = -10 + 3*x - 7*x^2 + 2*x^3
    std::vector<std::int64_t> coeffs = {-10, 3, -7, 2};
    std::vector<std::int64_t> xs = {-5, -3, -1, 0, 1, 3, 5, -100, 100};
    std::vector<std::int64_t> ys(xs.size(), 0);
    poly::batch_eval_poly_int64(coeffs, xs, ys);
    for (std::size_t i = 0; i < xs.size(); ++i) {
        const std::int64_t x = xs[i];
        const std::int64_t expected =
            -10 + 3 * x - 7 * x * x + 2 * x * x * x;
        TEST_ASSERT(ys[i] == expected,
                    "negative-coeff / negative-x evaluation mismatch");
    }
    compare_batch(coeffs, xs, "negative_values_parity");
    TEST_PASS("negative_values");
}

// ---------------------------------------------------------------------------
// Perf info (not assertive)
// ---------------------------------------------------------------------------

static void test_perf_info_1M() {
    set_env_and_reload(nullptr);
    std::mt19937_64 rng(0xFEED'1234'5678'BABEULL);
    // deg=8 with |x| <= 10 keeps |acc| < 100 * 10^8 = 1e10 — safe int64.
    std::uniform_int_distribution<std::int64_t> coeff_dist(-100, 100);
    std::uniform_int_distribution<std::int64_t> x_dist(-10, 10);
    std::vector<std::int64_t> coeffs(9);
    for (auto& c : coeffs) c = coeff_dist(rng);
    const std::size_t N = 1'000'000;
    std::vector<std::int64_t> xs(N);
    for (auto& x : xs) x = x_dist(rng);
    std::vector<std::int64_t> ys_scalar(N, 0);
    std::vector<std::int64_t> ys_dispatch(N, 0);

    // Scalar timing.
    set_env_and_reload("0");
    auto t0 = std::chrono::steady_clock::now();
    poly::batch_eval_poly_int64(coeffs, xs, ys_scalar);
    auto t1 = std::chrono::steady_clock::now();

    // Dispatch (Auto) timing.
    set_env_and_reload(nullptr);
    auto t2 = std::chrono::steady_clock::now();
    poly::batch_eval_poly_int64(coeffs, xs, ys_dispatch);
    auto t3 = std::chrono::steady_clock::now();

    const double scalar_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) / 1000.0;
    const double dispatch_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count()) / 1000.0;
    std::printf("  perf_info_1M: scalar=%.2fms dispatch=%.2fms speedup=%.2fx\n",
                scalar_ms, dispatch_ms,
                dispatch_ms > 0 ? scalar_ms / dispatch_ms : 0.0);
    TEST_ASSERT(batch_results_equal(ys_scalar, ys_dispatch),
                "perf_info: scalar and dispatch must agree");
    TEST_PASS("perf_info_1M");
}

// ---------------------------------------------------------------------------
// One-shot horner_eval_one_scalar sanity (used by the SIMD tail).
// ---------------------------------------------------------------------------

static void test_horner_eval_one_scalar_sanity() {
    set_env_and_reload(nullptr);
    // p(x) = 1 + 2x + 3x^2; p(10) = 1 + 20 + 300 = 321
    std::vector<std::int64_t> coeffs = {1, 2, 3};
    TEST_ASSERT(poly::horner_eval_one_scalar(coeffs, 10) == 321,
                "horner_eval_one_scalar deg=2 sanity");
    // p(0) = 1
    TEST_ASSERT(poly::horner_eval_one_scalar(coeffs, 0) == 1,
                "horner_eval_one_scalar p(0) = coeffs[0]");
    // Empty coeffs -> 0
    std::vector<std::int64_t> empty;
    TEST_ASSERT(poly::horner_eval_one_scalar(empty, 999) == 0,
                "empty coeffs always yields 0");
    TEST_PASS("horner_eval_one_scalar_sanity");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    std::printf("== test_horner_batch_simd ==\n");

    // ENV parsing.
    test_env_unset_yields_auto();
    test_env_zero_yields_force_off();
    test_env_one_yields_force_on();
    test_env_garbage_yields_auto();

    // Edge cases / behaviour.
    test_empty_xs();
    test_empty_coeffs();
    test_deg0_constant();
    test_deg1_linear();
    test_horner_eval_one_scalar_sanity();

    // Random parity tests.
    test_deg5_random_100();
    test_deg10_random_1000();

    // SIMD / scalar dispatcher parity.
    test_force_off_vs_auto_parity();
    test_single_x_tail_path();
    test_unaligned_len();
    test_negative_values();

    // Perf info (no asserts beyond parity).
    test_perf_info_1M();

    // Restore the env to the default state for follow-up tests in the
    // same process (defensive — no other test relies on this, but
    // costs nothing).
    set_env_and_reload(nullptr);

    std::printf("\n== Results: %d passed, %d failed ==\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
