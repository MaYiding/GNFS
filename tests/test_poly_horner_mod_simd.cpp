// test_poly_horner_mod_simd.cpp — Correctness tests for the SIMD-
// accelerated F_p[x] batched modular Horner evaluation helper (W15 T2).
//
// Strategy
// --------
// Every test that exercises the SIMD path runs both the scalar reference
// (`batch_eval_poly_mod_scalar`) and the dispatched helper, then asserts
// per-index equality. Modular Horner evaluation over F_p with reduced
// inputs is a deterministic uint32 operation; for inputs satisfying the
// `coeffs[k] < p && xs[i] < p` precondition the SIMD and scalar paths
// must produce bit-identical results.
//
// Coverage (19 tests, mirrors W14 T2 structure):
//   1-4. ENV parsing (GNFS_POLY_HORNER_MOD_SIMD = unset / 0|off /
//        1|on / garbage|auto|empty|"2"|"true"|" 1").
//   5.   Empty xs → no-op.
//   6.   Empty coeffs → zero-fill ys.
//   7.   deg=0 (constant polynomial) → ys[i] = c[0] for all i.
//   8.   deg=1 → ys[i] = (c[0] + c[1]*xs[i]) mod p.
//   9.   Random sweep deg=5 100 evals across p ∈ {101, 65537, 2^31-1}.
//   10.  Random 1000 evals deg=8 medium prime.
//   11.  ForceOff vs Auto parity (256 evals).
//   12.  Single xs (1 eval, edge boundary).
//   13.  Unaligned size sweep 1..33 across both NEON and AVX2 lane
//        boundaries (4-lane / 2-lane), exercises tail residual.
//   14.  SIMD acceleration boundary: p = 2^31 (in-window) and
//        p = 2147483659 (first prime > 2^31, fallback path).
//   15.  p = 2^31 - 1 (Mersenne prime, in-window).
//   16.  Reset env cache hook (test infrastructure).
//   17.  Defensive clamping: coeffs longer than xs, xs longer than ys,
//        ys longer than xs.
//   18.  xs all zeros → ys all equal coeffs[0] (P(0) = c[0]).
//   19.  Perf info probe (1M evals, prints scalar vs dispatch ns/eval,
//        no assertion).
//
// Build: registered in CMakeLists.txt instant tier as PolyHornerModSimd.

#include <gnfs/polynomial/horner_mod_simd.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
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
        ::unsetenv("GNFS_POLY_HORNER_MOD_SIMD");
    } else {
        ::setenv("GNFS_POLY_HORNER_MOD_SIMD", value, 1);
    }
    poly::poly_horner_mod_simd_reset_env_cache_for_testing();
}

static bool vectors_equal(const std::vector<std::uint32_t>& a,
                          const std::vector<std::uint32_t>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

// Generate a random vector reduced mod p.
static std::vector<std::uint32_t> make_random_vec(std::size_t n,
                                                  std::uint32_t p,
                                                  std::uint64_t seed) {
    std::vector<std::uint32_t> v(n);
    std::mt19937_64 rng(seed);
    if (p == 0) {
        for (auto& x : v) x = 0;
        return v;
    }
    for (auto& x : v) {
        x = static_cast<std::uint32_t>(rng() % p);
    }
    return v;
}

static void compare_eval(const std::vector<std::uint32_t>& coeffs,
                         const std::vector<std::uint32_t>& xs,
                         std::uint32_t p,
                         const char* label) {
    std::vector<std::uint32_t> ys_scalar(xs.size(), 0xDEADBEEFu);
    std::vector<std::uint32_t> ys_dispatch(xs.size(), 0xBEEFDEADu);
    poly::batch_eval_poly_mod_scalar(coeffs, xs, p, ys_scalar);
    poly::batch_eval_poly_mod(coeffs, xs, p, ys_dispatch);
    if (!vectors_equal(ys_scalar, ys_dispatch)) {
        std::fprintf(stderr, "  eval mismatch [%s] n=%zu p=%u deg=%zu\n",
                     label, xs.size(), p,
                     coeffs.empty() ? std::size_t{0} : coeffs.size() - 1);
        std::size_t shown = 0;
        for (std::size_t i = 0; i < xs.size() && shown < 4; ++i) {
            if (ys_scalar[i] != ys_dispatch[i]) {
                std::fprintf(stderr,
                    "    index %zu: x=%u scalar=%u dispatch=%u\n",
                    i, xs[i], ys_scalar[i], ys_dispatch[i]);
                ++shown;
            }
        }
        tests_failed++;
        return;
    }
    // Sanity: every output must be strictly less than p.
    for (std::size_t i = 0; i < ys_scalar.size(); ++i) {
        if (ys_scalar[i] >= p) {
            std::fprintf(stderr,
                "  output out-of-range [%s] ys[%zu]=%u >= p=%u\n",
                label, i, ys_scalar[i], p);
            tests_failed++;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// ENV parsing tests (4)
// ---------------------------------------------------------------------------

static void test_env_unset_yields_auto() {
    set_env_and_reload(nullptr);
    TEST_ASSERT(poly::poly_horner_mod_simd_mode() ==
                    poly::PolyHornerModSimdMode::Auto,
                "unset env should yield Auto mode");
    TEST_PASS("env_unset_yields_auto");
}

static void test_env_zero_yields_force_off() {
    set_env_and_reload("0");
    TEST_ASSERT(poly::poly_horner_mod_simd_mode() ==
                    poly::PolyHornerModSimdMode::ForceOff,
                "env=0 should yield ForceOff");
    TEST_ASSERT(!poly::poly_horner_mod_simd_enabled(),
                "env=0 should disable SIMD");
    set_env_and_reload("off");
    TEST_ASSERT(poly::poly_horner_mod_simd_mode() ==
                    poly::PolyHornerModSimdMode::ForceOff,
                "env=off should yield ForceOff");
    set_env_and_reload(nullptr);
    TEST_PASS("env_zero_yields_force_off");
}

static void test_env_one_yields_force_on() {
    set_env_and_reload("1");
    TEST_ASSERT(poly::poly_horner_mod_simd_mode() ==
                    poly::PolyHornerModSimdMode::ForceOn,
                "env=1 should yield ForceOn");
    set_env_and_reload("on");
    TEST_ASSERT(poly::poly_horner_mod_simd_mode() ==
                    poly::PolyHornerModSimdMode::ForceOn,
                "env=on should yield ForceOn");
    set_env_and_reload(nullptr);
    TEST_PASS("env_one_yields_force_on");
}

static void test_env_garbage_yields_auto() {
    set_env_and_reload("garbage");
    TEST_ASSERT(poly::poly_horner_mod_simd_mode() ==
                    poly::PolyHornerModSimdMode::Auto,
                "garbage env should yield Auto");
    set_env_and_reload("auto");
    TEST_ASSERT(poly::poly_horner_mod_simd_mode() ==
                    poly::PolyHornerModSimdMode::Auto,
                "'auto' string should yield Auto");
    set_env_and_reload("");
    TEST_ASSERT(poly::poly_horner_mod_simd_mode() ==
                    poly::PolyHornerModSimdMode::Auto,
                "empty string should yield Auto");
    set_env_and_reload("2");
    TEST_ASSERT(poly::poly_horner_mod_simd_mode() ==
                    poly::PolyHornerModSimdMode::Auto,
                "'2' should yield Auto (not Force*)");
    set_env_and_reload("true");
    TEST_ASSERT(poly::poly_horner_mod_simd_mode() ==
                    poly::PolyHornerModSimdMode::Auto,
                "'true' should yield Auto (only '1'/'on' are ForceOn)");
    set_env_and_reload(" 1");
    TEST_ASSERT(poly::poly_horner_mod_simd_mode() ==
                    poly::PolyHornerModSimdMode::Auto,
                "' 1' with leading space should yield Auto (strict)");
    set_env_and_reload(nullptr);
    TEST_PASS("env_garbage_yields_auto");
}

// ---------------------------------------------------------------------------
// Edge cases (3): empty xs / empty coeffs / single xs
// ---------------------------------------------------------------------------

static void test_empty_xs() {
    set_env_and_reload(nullptr);
    const std::uint32_t p = 101u;
    std::vector<std::uint32_t> coeffs = {1u, 2u, 3u};
    std::vector<std::uint32_t> xs;
    std::vector<std::uint32_t> ys;
    poly::batch_eval_poly_mod(coeffs, xs, p, ys);
    TEST_ASSERT(ys.empty(), "empty xs → empty ys");
    // Also ensure no UB if ys is non-empty but xs is empty.
    std::vector<std::uint32_t> ys_preserved(5, 0xCAFEu);
    poly::batch_eval_poly_mod(coeffs, xs, p, ys_preserved);
    for (std::size_t i = 0; i < ys_preserved.size(); ++i) {
        TEST_ASSERT(ys_preserved[i] == 0xCAFEu,
                    "empty xs must not touch any ys element");
    }
    TEST_PASS("empty_xs");
}

static void test_empty_coeffs() {
    set_env_and_reload(nullptr);
    const std::uint32_t p = 101u;
    std::vector<std::uint32_t> coeffs;
    std::vector<std::uint32_t> xs = {1u, 2u, 3u, 4u, 5u};
    std::vector<std::uint32_t> ys(5, 0xDEADBEEFu);
    poly::batch_eval_poly_mod(coeffs, xs, p, ys);
    // Empty coeffs = degree-(-1) polynomial = the zero polynomial.
    for (std::size_t i = 0; i < ys.size(); ++i) {
        TEST_ASSERT(ys[i] == 0u, "empty coeffs → ys[i] = 0");
    }
    compare_eval(coeffs, xs, p, "empty_coeffs_parity");
    TEST_PASS("empty_coeffs");
}

static void test_single_xs() {
    set_env_and_reload(nullptr);
    const std::uint32_t p = 65537u;
    // p(x) = 3 + 5x + 7x^2; evaluate at x = 10.
    // = 3 + 50 + 700 = 753.
    std::vector<std::uint32_t> coeffs = {3u, 5u, 7u};
    std::vector<std::uint32_t> xs = {10u};
    std::vector<std::uint32_t> ys(1, 0xCAFEu);
    poly::batch_eval_poly_mod(coeffs, xs, p, ys);
    TEST_ASSERT(ys[0] == 753u, "single xs deg=2 explicit");
    compare_eval(coeffs, xs, p, "single_xs_parity");
    TEST_PASS("single_xs");
}

// ---------------------------------------------------------------------------
// Degree sweeps (2): deg=0, deg=1
// ---------------------------------------------------------------------------

static void test_deg0_constant() {
    set_env_and_reload(nullptr);
    const std::uint32_t p = 65537u;
    // p(x) = 42 (constant); evaluate at any xs[i] → 42.
    std::vector<std::uint32_t> coeffs = {42u};
    std::vector<std::uint32_t> xs = {0u, 1u, 100u, 65530u, 65536u};
    std::vector<std::uint32_t> ys(5, 0xDEADBEEFu);
    poly::batch_eval_poly_mod(coeffs, xs, p, ys);
    for (std::size_t i = 0; i < ys.size(); ++i) {
        TEST_ASSERT(ys[i] == 42u, "deg=0 polynomial → constant 42");
    }
    // Random xs sweep with constant polynomial.
    auto xs_random = make_random_vec(100, p, 0xAA01ULL);
    std::vector<std::uint32_t> ys_random(100, 0);
    poly::batch_eval_poly_mod(coeffs, xs_random, p, ys_random);
    for (std::size_t i = 0; i < ys_random.size(); ++i) {
        TEST_ASSERT(ys_random[i] == 42u, "deg=0 random xs sweep constant");
    }
    TEST_PASS("deg0_constant");
}

static void test_deg1_linear() {
    set_env_and_reload(nullptr);
    const std::uint32_t p = 65537u;
    // p(x) = c0 + c1*x; evaluate at xs[i] → (c0 + c1*xs[i]) mod p.
    std::vector<std::uint32_t> coeffs = {7u, 11u};
    std::vector<std::uint32_t> xs = {0u, 1u, 2u, 100u, 65535u};
    std::vector<std::uint32_t> ys(5, 0xDEADBEEFu);
    poly::batch_eval_poly_mod(coeffs, xs, p, ys);
    // Hand check: p(0) = 7, p(1) = 18, p(2) = 29, p(100) = 1107,
    // p(65535) = (7 + 11*65535) mod 65537 = (7 + 720885) mod 65537
    // = 720892 mod 65537. 720892 / 65537 = 11 (11*65537=720907), so
    // 720892 - 11*65537 = 720892 - 720907 = -15 ≡ 65522 (mod 65537).
    TEST_ASSERT(ys[0] == 7u, "deg=1 p(0) = 7");
    TEST_ASSERT(ys[1] == 18u, "deg=1 p(1) = 18");
    TEST_ASSERT(ys[2] == 29u, "deg=1 p(2) = 29");
    TEST_ASSERT(ys[3] == 1107u, "deg=1 p(100) = 1107");
    TEST_ASSERT(ys[4] == 65522u, "deg=1 p(65535) mod 65537 = 65522");
    compare_eval(coeffs, xs, p, "deg1_parity");
    TEST_PASS("deg1_linear");
}

// ---------------------------------------------------------------------------
// Random sweeps (2)
// ---------------------------------------------------------------------------

static void test_random_deg5_three_primes() {
    set_env_and_reload(nullptr);
    for (std::uint32_t p : {101u, 65537u, 2147483647u}) {
        auto coeffs = make_random_vec(6, p, 0xBB10ULL ^ p);
        auto xs = make_random_vec(100, p, 0xAA10ULL ^ p);
        compare_eval(coeffs, xs, p, "random_deg5");
    }
    TEST_PASS("random_deg5_three_primes");
}

static void test_random_1000_deg8() {
    set_env_and_reload(nullptr);
    const std::uint32_t p = 65537u;
    auto coeffs = make_random_vec(9, p, 0xBB12ULL);
    auto xs = make_random_vec(1000, p, 0xAA12ULL);
    compare_eval(coeffs, xs, p, "random_1000_deg8");
    TEST_PASS("random_1000_deg8");
}

// ---------------------------------------------------------------------------
// Parity (1) + edge / size sweep (1)
// ---------------------------------------------------------------------------

static void test_force_off_vs_auto_parity() {
    const std::uint32_t p = 65537u;
    auto coeffs = make_random_vec(5, p, 0xBB13ULL);
    auto xs = make_random_vec(256, p, 0xAA13ULL);

    // ForceOff (scalar) path.
    set_env_and_reload("0");
    std::vector<std::uint32_t> off_ys(256, 0);
    poly::batch_eval_poly_mod(coeffs, xs, p, off_ys);
    TEST_ASSERT(!poly::poly_horner_mod_simd_enabled(),
                "ForceOff should disable SIMD");

    // Auto path.
    set_env_and_reload(nullptr);
    std::vector<std::uint32_t> auto_ys(256, 0);
    poly::batch_eval_poly_mod(coeffs, xs, p, auto_ys);

    TEST_ASSERT(vectors_equal(off_ys, auto_ys),
                "ForceOff and Auto must agree bit-for-bit");
    set_env_and_reload(nullptr);
    TEST_PASS("force_off_vs_auto_parity");
}

static void test_unaligned_size_sweep() {
    set_env_and_reload(nullptr);
    const std::uint32_t p = 65537u;
    auto coeffs = make_random_vec(7, p, 0xBB14ULL);
    // Exercise sizes that hit both NEON (2-lane) and AVX2 (4-lane)
    // boundaries plus tail residual.
    for (std::size_t n = 1; n <= 33; ++n) {
        auto xs = make_random_vec(n, p, 0xAA14ULL ^ n);
        compare_eval(coeffs, xs, p, "unaligned_size_sweep");
    }
    TEST_PASS("unaligned_size_sweep");
}

// ---------------------------------------------------------------------------
// SIMD acceleration boundary (1) + Mersenne (1)
// ---------------------------------------------------------------------------

static void test_simd_window_boundary() {
    set_env_and_reload(nullptr);
    // p = 2^31 = 2147483648: edge of the SIMD window (accept ≤ 2^31).
    // This is not prime but the helper does not validate primality; we
    // only need a value to check the dispatcher gate. Use random values
    // strictly below p, which satisfy the documented precondition.
    {
        const std::uint32_t p = 0x80000000u;  // 2^31
        auto coeffs = make_random_vec(4, p, 0xBB20ULL);
        auto xs = make_random_vec(64, p, 0xAA20ULL);
        compare_eval(coeffs, xs, p, "p_eq_2_to_31_in_window");
    }
    // p = 2147483659: smallest prime strictly above 2^31. Falls outside
    // the SIMD window so the dispatcher routes to scalar reference.
    // Scalar path still works via uint64 widening.
    {
        const std::uint32_t p = 2147483659u;
        auto coeffs = make_random_vec(4, p, 0xBB21ULL);
        auto xs = make_random_vec(50, p, 0xAA21ULL);
        compare_eval(coeffs, xs, p, "p_above_2_to_31_fallback");
        // Sanity: output strictly less than p.
        std::vector<std::uint32_t> ys(50, 0);
        poly::batch_eval_poly_mod(coeffs, xs, p, ys);
        for (std::size_t i = 0; i < ys.size(); ++i) {
            TEST_ASSERT(ys[i] < p,
                        "output must be < p in fallback path");
        }
    }
    TEST_PASS("simd_window_boundary");
}

static void test_mersenne_prime_31() {
    set_env_and_reload(nullptr);
    // Mersenne prime 2^31 - 1 = 2147483647 (in-window).
    const std::uint32_t p = 2147483647u;
    auto coeffs = make_random_vec(6, p, 0xBB22ULL);
    auto xs = make_random_vec(500, p, 0xAA22ULL);
    compare_eval(coeffs, xs, p, "mersenne_31");
    TEST_PASS("mersenne_prime_31");
}

// ---------------------------------------------------------------------------
// Reset hook (1)
// ---------------------------------------------------------------------------

static void test_reset_env_cache_hook() {
    ::unsetenv("GNFS_POLY_HORNER_MOD_SIMD");
    poly::poly_horner_mod_simd_reset_env_cache_for_testing();
    TEST_ASSERT(poly::poly_horner_mod_simd_mode() ==
                    poly::PolyHornerModSimdMode::Auto,
                "after reset+unset, mode should be Auto");
    ::setenv("GNFS_POLY_HORNER_MOD_SIMD", "0", 1);
    poly::poly_horner_mod_simd_reset_env_cache_for_testing();
    TEST_ASSERT(poly::poly_horner_mod_simd_mode() ==
                    poly::PolyHornerModSimdMode::ForceOff,
                "after reset+set=0, mode should be ForceOff");
    ::setenv("GNFS_POLY_HORNER_MOD_SIMD", "1", 1);
    poly::poly_horner_mod_simd_reset_env_cache_for_testing();
    TEST_ASSERT(poly::poly_horner_mod_simd_mode() ==
                    poly::PolyHornerModSimdMode::ForceOn,
                "after reset+set=1, mode should be ForceOn");
    ::unsetenv("GNFS_POLY_HORNER_MOD_SIMD");
    poly::poly_horner_mod_simd_reset_env_cache_for_testing();
    TEST_PASS("reset_env_cache_hook");
}

// ---------------------------------------------------------------------------
// Defensive clamping (1)
// ---------------------------------------------------------------------------

static void test_defensive_clamping() {
    set_env_and_reload(nullptr);
    const std::uint32_t p = 101u;
    // Case A: xs much longer than ys → only ys.size() entries written.
    {
        std::vector<std::uint32_t> coeffs = {3u, 5u, 7u};
        std::vector<std::uint32_t> xs = {1u, 2u, 3u, 4u, 5u};
        std::vector<std::uint32_t> ys(3, 0xCAFEu);
        poly::batch_eval_poly_mod(coeffs, xs, p, ys);
        // p(x) = 3 + 5x + 7x^2 mod 101
        // p(1) = 15, p(2) = 41, p(3) = 81
        TEST_ASSERT(ys[0] == 15u, "clamp xs>ys: ys[0]");
        TEST_ASSERT(ys[1] == 41u, "clamp xs>ys: ys[1]");
        TEST_ASSERT(ys[2] == 81u, "clamp xs>ys: ys[2]");
    }
    // Case B: ys longer than xs → only xs.size() entries written;
    // ys tail untouched.
    {
        std::vector<std::uint32_t> coeffs = {3u, 5u, 7u};
        std::vector<std::uint32_t> xs = {1u, 2u};
        std::vector<std::uint32_t> ys(5, 0xBEEFu);
        poly::batch_eval_poly_mod(coeffs, xs, p, ys);
        TEST_ASSERT(ys[0] == 15u, "clamp ys>xs: ys[0]");
        TEST_ASSERT(ys[1] == 41u, "clamp ys>xs: ys[1]");
        TEST_ASSERT(ys[2] == 0xBEEFu, "clamp ys>xs: ys[2] tail preserved");
        TEST_ASSERT(ys[3] == 0xBEEFu, "clamp ys>xs: ys[3] tail preserved");
        TEST_ASSERT(ys[4] == 0xBEEFu, "clamp ys>xs: ys[4] tail preserved");
    }
    // Case C: coeffs much longer than xs (no impact on clamp; deg sets
    // inner loop iters, xs.size() sets outer loop iters).
    {
        std::vector<std::uint32_t> coeffs(50, 1u);  // all ones, deg=49
        std::vector<std::uint32_t> xs = {1u};
        std::vector<std::uint32_t> ys(1, 0);
        poly::batch_eval_poly_mod(coeffs, xs, p, ys);
        // p(1) = sum of 50 ones = 50.
        TEST_ASSERT(ys[0] == 50u, "long coeffs at x=1 → sum of all coeffs");
    }
    TEST_PASS("defensive_clamping");
}

// ---------------------------------------------------------------------------
// Degenerate xs (1)
// ---------------------------------------------------------------------------

static void test_xs_all_zeros() {
    set_env_and_reload(nullptr);
    const std::uint32_t p = 65537u;
    auto coeffs = make_random_vec(8, p, 0xBB30ULL);
    std::vector<std::uint32_t> xs(50, 0u);
    std::vector<std::uint32_t> ys(50, 0xDEADu);
    poly::batch_eval_poly_mod(coeffs, xs, p, ys);
    // p(0) = c[0] for every evaluation.
    for (std::size_t i = 0; i < ys.size(); ++i) {
        TEST_ASSERT(ys[i] == coeffs[0],
                    "xs all zeros → ys[i] = coeffs[0]");
    }
    compare_eval(coeffs, xs, p, "xs_all_zeros_parity");
    TEST_PASS("xs_all_zeros");
}

// ---------------------------------------------------------------------------
// Perf info (1, not assertive)
// ---------------------------------------------------------------------------

static void test_perf_info_1M() {
    const std::uint32_t p = 2147483647u;  // Mersenne, in SIMD window
    const std::size_t N = 1'000'000;
    const std::size_t D = 8;  // deg=8 polynomial
    auto coeffs = make_random_vec(D + 1, p, 0xBB40ULL);
    auto xs = make_random_vec(N, p, 0xAA40ULL);
    std::vector<std::uint32_t> ys_scalar(N, 0);
    std::vector<std::uint32_t> ys_dispatch(N, 0);

    // Scalar (ForceOff) timing.
    set_env_and_reload("0");
    auto t0 = std::chrono::steady_clock::now();
    poly::batch_eval_poly_mod(coeffs, xs, p, ys_scalar);
    auto t1 = std::chrono::steady_clock::now();

    // Dispatch (Auto) timing.
    set_env_and_reload(nullptr);
    auto t2 = std::chrono::steady_clock::now();
    poly::batch_eval_poly_mod(coeffs, xs, p, ys_dispatch);
    auto t3 = std::chrono::steady_clock::now();

    const double scalar_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) /
        1000.0;
    const double dispatch_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count()) /
        1000.0;
    const double scalar_ns_per_eval =
        scalar_ms * 1000.0 * 1000.0 / static_cast<double>(N);
    const double dispatch_ns_per_eval =
        dispatch_ms * 1000.0 * 1000.0 / static_cast<double>(N);
    std::printf("  perf_info_1M_deg8: scalar=%.2fms (%.2fns/eval) "
                "dispatch=%.2fms (%.2fns/eval) speedup=%.2fx\n",
                scalar_ms, scalar_ns_per_eval,
                dispatch_ms, dispatch_ns_per_eval,
                dispatch_ms > 0 ? scalar_ms / dispatch_ms : 0.0);
    TEST_ASSERT(vectors_equal(ys_scalar, ys_dispatch),
                "perf_info: scalar and dispatch must agree");
    set_env_and_reload(nullptr);
    TEST_PASS("perf_info_1M");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    std::printf("Running test_poly_horner_mod_simd...\n");

    // ENV (1-4)
    test_env_unset_yields_auto();
    test_env_zero_yields_force_off();
    test_env_one_yields_force_on();
    test_env_garbage_yields_auto();

    // Edges (5-7)
    test_empty_xs();
    test_empty_coeffs();
    test_single_xs();

    // Degree sweeps (8-9: deg0 and deg1 hand-checked; included in 7/8)
    test_deg0_constant();
    test_deg1_linear();

    // Random sweeps (10-11)
    test_random_deg5_three_primes();
    test_random_1000_deg8();

    // Parity + size sweep (12-13)
    test_force_off_vs_auto_parity();
    test_unaligned_size_sweep();

    // SIMD boundary + Mersenne (14-15)
    test_simd_window_boundary();
    test_mersenne_prime_31();

    // Reset hook (16)
    test_reset_env_cache_hook();

    // Defensive clamping + degenerate xs (17-18)
    test_defensive_clamping();
    test_xs_all_zeros();

    // Perf info (19)
    test_perf_info_1M();

    std::printf("\nResults: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
