// test_poly_add_mod_simd.cpp — Correctness tests for the SIMD-accelerated
// F_p[x] batch add / sub modulo p helper.
//
// Strategy
// --------
// Every test that exercises the SIMD path runs both the scalar reference
// (`add_mod_p_batch_scalar` / `sub_mod_p_batch_scalar`) and the dispatched
// helper, then asserts per-index equality. Add and sub modulo a fixed prime
// are deterministic uint32 operations; for inputs satisfying the
// `a[i] < p && b[i] < p` precondition the SIMD and scalar paths must
// produce bit-identical results.
//
// Coverage:
// * ENV parsing (GNFS_POLY_ADD_MOD_SIMD = auto / 0 / 1 / unset / garbage).
// * Empty / single coefficient edge cases.
// * Aligned (32) and unaligned (33) batch sizes (tests the SIMD residual tail).
// * Random 1000 with small prime (p=101) / medium (p=2^16+1) / Mersenne
//   (p=2^31-1, the SIMD-window boundary).
// * p > 2^31 fallback path (p=2147483659): the helper must still produce
//   correct output via the scalar reference even though SIMD is gated off.
// * sub_mod_p_batch sweep with mixed signs (a < b and a >= b cases).
// * Algebraic identities: a + 0 = a, a - a = 0, (a + (p-1)) - (p-1) = a.
// * Boundary: a = p-1, b = p-1 → add yields (2p-2) mod p = p-2; sub yields 0.
// * ForceOff vs Auto parity (same outputs across both dispatch states).
// * Perf info probe (1M coeffs, no assertion — wall time printed).
// * Reset env cache hook (covers test infrastructure).
//
// Build: registered in CMakeLists.txt instant tier as PolyAddModSimd.

#include <gnfs/polynomial/add_mod_simd.hpp>

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
        ::unsetenv("GNFS_POLY_ADD_MOD_SIMD");
    } else {
        ::setenv("GNFS_POLY_ADD_MOD_SIMD", value, 1);
    }
    poly::poly_add_mod_simd_reset_env_cache_for_testing();
}

static bool vectors_equal(const std::vector<std::uint32_t>& a,
                          const std::vector<std::uint32_t>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static void compare_add(const std::vector<std::uint32_t>& a,
                        const std::vector<std::uint32_t>& b,
                        std::uint32_t p,
                        const char* label) {
    std::vector<std::uint32_t> scalar_out(a.size(), 0xDEADBEEFu);
    std::vector<std::uint32_t> dispatch_out(a.size(), 0xBEEFDEADu);
    poly::add_mod_p_batch_scalar(a, b, p, scalar_out);
    poly::add_mod_p_batch(a, b, p, dispatch_out);
    if (!vectors_equal(scalar_out, dispatch_out)) {
        std::fprintf(stderr, "  add mismatch [%s] n=%zu p=%u\n",
                     label, a.size(), p);
        std::size_t shown = 0;
        for (std::size_t i = 0; i < a.size() && shown < 4; ++i) {
            if (scalar_out[i] != dispatch_out[i]) {
                std::fprintf(stderr,
                    "    index %zu: a=%u b=%u scalar=%u dispatch=%u\n",
                    i, a[i], b[i], scalar_out[i], dispatch_out[i]);
                ++shown;
            }
        }
        tests_failed++;
        return;
    }
}

static void compare_sub(const std::vector<std::uint32_t>& a,
                        const std::vector<std::uint32_t>& b,
                        std::uint32_t p,
                        const char* label) {
    std::vector<std::uint32_t> scalar_out(a.size(), 0xDEADBEEFu);
    std::vector<std::uint32_t> dispatch_out(a.size(), 0xBEEFDEADu);
    poly::sub_mod_p_batch_scalar(a, b, p, scalar_out);
    poly::sub_mod_p_batch(a, b, p, dispatch_out);
    if (!vectors_equal(scalar_out, dispatch_out)) {
        std::fprintf(stderr, "  sub mismatch [%s] n=%zu p=%u\n",
                     label, a.size(), p);
        std::size_t shown = 0;
        for (std::size_t i = 0; i < a.size() && shown < 4; ++i) {
            if (scalar_out[i] != dispatch_out[i]) {
                std::fprintf(stderr,
                    "    index %zu: a=%u b=%u scalar=%u dispatch=%u\n",
                    i, a[i], b[i], scalar_out[i], dispatch_out[i]);
                ++shown;
            }
        }
        tests_failed++;
        return;
    }
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

// ---------------------------------------------------------------------------
// ENV parsing tests
// ---------------------------------------------------------------------------

static void test_env_unset_yields_auto() {
    set_env_and_reload(nullptr);
    TEST_ASSERT(poly::poly_add_mod_simd_mode() ==
                    poly::PolyAddModSimdMode::Auto,
                "unset env should yield Auto mode");
    TEST_PASS("env_unset_yields_auto");
}

static void test_env_zero_yields_force_off() {
    set_env_and_reload("0");
    TEST_ASSERT(poly::poly_add_mod_simd_mode() ==
                    poly::PolyAddModSimdMode::ForceOff,
                "env=0 should yield ForceOff");
    TEST_ASSERT(!poly::poly_add_mod_simd_enabled(),
                "env=0 should disable SIMD");
    set_env_and_reload("off");
    TEST_ASSERT(poly::poly_add_mod_simd_mode() ==
                    poly::PolyAddModSimdMode::ForceOff,
                "env=off should yield ForceOff");
    set_env_and_reload(nullptr);
    TEST_PASS("env_zero_yields_force_off");
}

static void test_env_one_yields_force_on() {
    set_env_and_reload("1");
    TEST_ASSERT(poly::poly_add_mod_simd_mode() ==
                    poly::PolyAddModSimdMode::ForceOn,
                "env=1 should yield ForceOn");
    set_env_and_reload("on");
    TEST_ASSERT(poly::poly_add_mod_simd_mode() ==
                    poly::PolyAddModSimdMode::ForceOn,
                "env=on should yield ForceOn");
    set_env_and_reload(nullptr);
    TEST_PASS("env_one_yields_force_on");
}

static void test_env_garbage_yields_auto() {
    set_env_and_reload("garbage");
    TEST_ASSERT(poly::poly_add_mod_simd_mode() ==
                    poly::PolyAddModSimdMode::Auto,
                "garbage env should yield Auto");
    set_env_and_reload("auto");
    TEST_ASSERT(poly::poly_add_mod_simd_mode() ==
                    poly::PolyAddModSimdMode::Auto,
                "'auto' string should yield Auto");
    set_env_and_reload("");
    TEST_ASSERT(poly::poly_add_mod_simd_mode() ==
                    poly::PolyAddModSimdMode::Auto,
                "empty string should yield Auto");
    set_env_and_reload("2");
    TEST_ASSERT(poly::poly_add_mod_simd_mode() ==
                    poly::PolyAddModSimdMode::Auto,
                "'2' should yield Auto (not Force*)");
    set_env_and_reload("true");
    TEST_ASSERT(poly::poly_add_mod_simd_mode() ==
                    poly::PolyAddModSimdMode::Auto,
                "'true' should yield Auto (only '1'/'on' are ForceOn)");
    set_env_and_reload(" 1");
    TEST_ASSERT(poly::poly_add_mod_simd_mode() ==
                    poly::PolyAddModSimdMode::Auto,
                "' 1' with leading space should yield Auto (strict token match)");
    set_env_and_reload(nullptr);
    TEST_PASS("env_garbage_yields_auto");
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

static void test_empty_input() {
    set_env_and_reload(nullptr);
    std::vector<std::uint32_t> a;
    std::vector<std::uint32_t> b;
    std::vector<std::uint32_t> out;
    poly::add_mod_p_batch(a, b, 101u, out);
    TEST_ASSERT(out.empty(), "empty input → empty output (add)");
    poly::sub_mod_p_batch(a, b, 101u, out);
    TEST_ASSERT(out.empty(), "empty input → empty output (sub)");
    TEST_PASS("empty_input");
}

static void test_single_coefficient() {
    set_env_and_reload(nullptr);
    const std::uint32_t p = 101u;
    std::vector<std::uint32_t> a = {50u};
    std::vector<std::uint32_t> b = {51u};
    std::vector<std::uint32_t> out(1, 0xCAFEu);
    // 50 + 51 = 101 ≡ 0 (mod 101)
    poly::add_mod_p_batch(a, b, p, out);
    TEST_ASSERT(out[0] == 0u,
                "single coeff add (50+51 mod 101) = 0");
    // 50 - 51 = -1 ≡ 100 (mod 101)
    poly::sub_mod_p_batch(a, b, p, out);
    TEST_ASSERT(out[0] == 100u,
                "single coeff sub (50-51 mod 101) = 100");
    compare_add(a, b, p, "single_coeff_add");
    compare_sub(a, b, p, "single_coeff_sub");
    TEST_PASS("single_coefficient");
}

static void test_aligned_32() {
    set_env_and_reload(nullptr);
    const std::uint32_t p = 65537u;  // 2^16 + 1 (small Fermat prime)
    std::vector<std::uint32_t> a = make_random_vec(32, p, 0xAA01ULL);
    std::vector<std::uint32_t> b = make_random_vec(32, p, 0xBB02ULL);
    compare_add(a, b, p, "aligned_32_add");
    compare_sub(a, b, p, "aligned_32_sub");
    TEST_PASS("aligned_32");
}

static void test_unaligned_33() {
    set_env_and_reload(nullptr);
    // 33 forces NEON 4-lane to do 8 full pair iters + 1 scalar tail;
    // AVX2 8-lane to do 4 full iters + 1 scalar tail.
    const std::uint32_t p = 65537u;
    std::vector<std::uint32_t> a = make_random_vec(33, p, 0xAA03ULL);
    std::vector<std::uint32_t> b = make_random_vec(33, p, 0xBB04ULL);
    compare_add(a, b, p, "unaligned_33_add");
    compare_sub(a, b, p, "unaligned_33_sub");
    // Sweep small sizes near SIMD lane boundaries.
    for (std::size_t n : {std::size_t{1}, std::size_t{2}, std::size_t{3},
                           std::size_t{4}, std::size_t{5}, std::size_t{7},
                           std::size_t{8}, std::size_t{9}, std::size_t{15},
                           std::size_t{16}, std::size_t{17}}) {
        auto av = make_random_vec(n, p, 0xCC00ULL ^ n);
        auto bv = make_random_vec(n, p, 0xDD00ULL ^ n);
        compare_add(av, bv, p, "unaligned_sweep_add");
        compare_sub(av, bv, p, "unaligned_sweep_sub");
    }
    TEST_PASS("unaligned_33");
}

static void test_random_1000_small_prime() {
    set_env_and_reload(nullptr);
    const std::uint32_t p = 101u;  // small prime
    auto a = make_random_vec(1000, p, 0xAA05ULL);
    auto b = make_random_vec(1000, p, 0xBB06ULL);
    compare_add(a, b, p, "random_1000_p101_add");
    compare_sub(a, b, p, "random_1000_p101_sub");
    TEST_PASS("random_1000_small_prime");
}

static void test_random_1000_medium_prime() {
    set_env_and_reload(nullptr);
    const std::uint32_t p = 65537u;  // medium prime (2^16+1)
    auto a = make_random_vec(1000, p, 0xAA07ULL);
    auto b = make_random_vec(1000, p, 0xBB08ULL);
    compare_add(a, b, p, "random_1000_p65537_add");
    compare_sub(a, b, p, "random_1000_p65537_sub");
    TEST_PASS("random_1000_medium_prime");
}

static void test_random_1000_mersenne_31() {
    set_env_and_reload(nullptr);
    // Mersenne prime 2^31 - 1 = 2147483647 — the upper edge of the
    // SIMD acceleration window (p <= 2^31).
    const std::uint32_t p = 2147483647u;
    auto a = make_random_vec(1000, p, 0xAA09ULL);
    auto b = make_random_vec(1000, p, 0xBB0AULL);
    compare_add(a, b, p, "random_1000_mersenne31_add");
    compare_sub(a, b, p, "random_1000_mersenne31_sub");
    TEST_PASS("random_1000_mersenne_31");
}

static void test_p_above_2_to_31_fallback_path() {
    set_env_and_reload(nullptr);
    // Smallest prime strictly above 2^31 = 2147483648. The prime
    // 2147483659 is the first prime > 2^31 (verified: well-known
    // "9th prime above 2^31"). For our purposes any p > 2^31 should
    // trigger the SIMD-window gate fallback to scalar. We don't need
    // to verify primality here — the helper does not validate it.
    const std::uint32_t p = 2147483659u;
    auto a = make_random_vec(50, p, 0xAA0BULL);
    auto b = make_random_vec(50, p, 0xBB0CULL);
    // dispatch path falls back to scalar (SIMD window excludes this p);
    // scalar reference still works via uint64 widening. Outputs must
    // be valid (< p) and bit-for-bit equal to the scalar reference.
    compare_add(a, b, p, "p_above_2_to_31_add");
    compare_sub(a, b, p, "p_above_2_to_31_sub");
    // Sanity: every output is strictly less than p.
    std::vector<std::uint32_t> out(50, 0);
    poly::add_mod_p_batch(a, b, p, out);
    for (std::size_t i = 0; i < out.size(); ++i) {
        TEST_ASSERT(out[i] < p,
                    "add output must satisfy out < p in fallback path");
    }
    poly::sub_mod_p_batch(a, b, p, out);
    for (std::size_t i = 0; i < out.size(); ++i) {
        TEST_ASSERT(out[i] < p,
                    "sub output must satisfy out < p in fallback path");
    }
    TEST_PASS("p_above_2_to_31_fallback_path");
}

static void test_sub_sweep_mixed_signs() {
    set_env_and_reload(nullptr);
    // Sub must handle both a >= b (no underflow) and a < b (needs +p).
    // Explicit fixture that hits both branches per-lane.
    const std::uint32_t p = 101u;
    std::vector<std::uint32_t> a = {50, 0, 100, 1, 75, 25, 99, 0, 50};
    std::vector<std::uint32_t> b = {0, 50, 1, 100, 25, 75, 0, 99, 50};
    std::vector<std::uint32_t> out(a.size(), 0);
    poly::sub_mod_p_batch(a, b, p, out);
    // Hand-computed expected:
    // 50-0=50, 0-50=-50≡51, 100-1=99, 1-100=-99≡2, 75-25=50,
    // 25-75=-50≡51, 99-0=99, 0-99=-99≡2, 50-50=0.
    std::vector<std::uint32_t> expected = {50, 51, 99, 2, 50, 51, 99, 2, 0};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        TEST_ASSERT(out[i] == expected[i],
                    "sub_sweep_mixed_signs explicit expected mismatch");
    }
    // And parity vs scalar across both paths.
    compare_sub(a, b, p, "sub_sweep_mixed_signs_parity");
    TEST_PASS("sub_sweep_mixed_signs");
}

static void test_algebraic_identities() {
    set_env_and_reload(nullptr);
    const std::uint32_t p = 65537u;
    auto a = make_random_vec(64, p, 0xAA0DULL);
    std::vector<std::uint32_t> zero(64, 0);

    // Identity: a + 0 = a (mod p)
    std::vector<std::uint32_t> out_add(64, 0xDEAD);
    poly::add_mod_p_batch(a, zero, p, out_add);
    for (std::size_t i = 0; i < a.size(); ++i) {
        TEST_ASSERT(out_add[i] == a[i],
                    "a + 0 != a (identity violated)");
    }

    // Identity: a - a = 0 (mod p)
    std::vector<std::uint32_t> out_sub(64, 0xBEEF);
    poly::sub_mod_p_batch(a, a, p, out_sub);
    for (std::size_t i = 0; i < a.size(); ++i) {
        TEST_ASSERT(out_sub[i] == 0u,
                    "a - a != 0 (identity violated)");
    }

    // Identity: (a + b) - b = a (mod p)
    auto b = make_random_vec(64, p, 0xBB0EULL);
    std::vector<std::uint32_t> tmp(64, 0);
    std::vector<std::uint32_t> roundtrip(64, 0);
    poly::add_mod_p_batch(a, b, p, tmp);
    poly::sub_mod_p_batch(tmp, b, p, roundtrip);
    for (std::size_t i = 0; i < a.size(); ++i) {
        TEST_ASSERT(roundtrip[i] == a[i],
                    "(a + b) - b != a (identity violated)");
    }

    TEST_PASS("algebraic_identities");
}

static void test_boundary_p_minus_1() {
    set_env_and_reload(nullptr);
    const std::uint32_t p = 65537u;
    const std::uint32_t pm1 = p - 1u;
    std::vector<std::uint32_t> a(16, pm1);  // all p-1
    std::vector<std::uint32_t> b(16, pm1);  // all p-1
    std::vector<std::uint32_t> out(16, 0);

    // add: (p-1) + (p-1) = 2p - 2 ≡ p - 2 (mod p)
    poly::add_mod_p_batch(a, b, p, out);
    for (std::size_t i = 0; i < 16; ++i) {
        TEST_ASSERT(out[i] == p - 2u,
                    "(p-1) + (p-1) mod p != p-2 (boundary)");
    }

    // sub: (p-1) - (p-1) = 0
    poly::sub_mod_p_batch(a, b, p, out);
    for (std::size_t i = 0; i < 16; ++i) {
        TEST_ASSERT(out[i] == 0u,
                    "(p-1) - (p-1) mod p != 0 (boundary)");
    }

    // Same test at the SIMD-window upper boundary p = 2^31 - 1.
    const std::uint32_t pm = 2147483647u;
    std::vector<std::uint32_t> a2(16, pm - 1u);
    std::vector<std::uint32_t> b2(16, pm - 1u);
    poly::add_mod_p_batch(a2, b2, pm, out);
    for (std::size_t i = 0; i < 16; ++i) {
        TEST_ASSERT(out[i] == pm - 2u,
                    "(p-1) + (p-1) mod (2^31-1) != p-2 (boundary, large)");
    }

    TEST_PASS("boundary_p_minus_1");
}

static void test_force_off_vs_auto_parity() {
    const std::uint32_t p = 65537u;
    auto a = make_random_vec(256, p, 0xAA0FULL);
    auto b = make_random_vec(256, p, 0xBB10ULL);

    // ForceOff (scalar) path.
    set_env_and_reload("0");
    std::vector<std::uint32_t> off_add(256, 0);
    std::vector<std::uint32_t> off_sub(256, 0);
    poly::add_mod_p_batch(a, b, p, off_add);
    poly::sub_mod_p_batch(a, b, p, off_sub);
    TEST_ASSERT(!poly::poly_add_mod_simd_enabled(),
                "ForceOff should disable SIMD");

    // Auto path.
    set_env_and_reload(nullptr);
    std::vector<std::uint32_t> auto_add(256, 0);
    std::vector<std::uint32_t> auto_sub(256, 0);
    poly::add_mod_p_batch(a, b, p, auto_add);
    poly::sub_mod_p_batch(a, b, p, auto_sub);

    TEST_ASSERT(vectors_equal(off_add, auto_add),
                "ForceOff and Auto add must agree bit-for-bit");
    TEST_ASSERT(vectors_equal(off_sub, auto_sub),
                "ForceOff and Auto sub must agree bit-for-bit");

    set_env_and_reload(nullptr);
    TEST_PASS("force_off_vs_auto_parity");
}

static void test_clamp_to_min_size() {
    // Defensive contract: if spans differ in length only the prefix
    // common to all three is processed; the tail of out past that
    // prefix is left untouched.
    set_env_and_reload(nullptr);
    const std::uint32_t p = 101u;
    std::vector<std::uint32_t> a = {10, 20, 30, 40, 50};
    std::vector<std::uint32_t> b = {1, 2, 3};  // shorter
    std::vector<std::uint32_t> out(5, 0xCAFEu);
    poly::add_mod_p_batch(a, b, p, out);
    // First 3 entries computed; last 2 preserved.
    TEST_ASSERT(out[0] == 11u, "clamp add[0]");
    TEST_ASSERT(out[1] == 22u, "clamp add[1]");
    TEST_ASSERT(out[2] == 33u, "clamp add[2]");
    TEST_ASSERT(out[3] == 0xCAFEu, "clamp tail untouched [3]");
    TEST_ASSERT(out[4] == 0xCAFEu, "clamp tail untouched [4]");

    // Similar for sub.
    std::fill(out.begin(), out.end(), 0xBEEFu);
    poly::sub_mod_p_batch(a, b, p, out);
    TEST_ASSERT(out[0] == 9u, "clamp sub[0]");
    TEST_ASSERT(out[1] == 18u, "clamp sub[1]");
    TEST_ASSERT(out[2] == 27u, "clamp sub[2]");
    TEST_ASSERT(out[3] == 0xBEEFu, "clamp sub tail untouched [3]");
    TEST_ASSERT(out[4] == 0xBEEFu, "clamp sub tail untouched [4]");

    // Output shorter than inputs (should compute only out.size() entries).
    std::vector<std::uint32_t> a2 = {10, 20, 30, 40, 50};
    std::vector<std::uint32_t> b2 = {1, 2, 3, 4, 5};
    std::vector<std::uint32_t> out2(2, 0xFEEDu);
    poly::add_mod_p_batch(a2, b2, p, out2);
    TEST_ASSERT(out2[0] == 11u, "clamp output-shorter add[0]");
    TEST_ASSERT(out2[1] == 22u, "clamp output-shorter add[1]");

    TEST_PASS("clamp_to_min_size");
}

// ---------------------------------------------------------------------------
// Perf info (not assertive)
// ---------------------------------------------------------------------------

static void test_perf_info_1M() {
    const std::uint32_t p = 2147483647u;  // Mersenne, in SIMD window
    const std::size_t N = 1'000'000;
    auto a = make_random_vec(N, p, 0xAA11ULL);
    auto b = make_random_vec(N, p, 0xBB12ULL);
    std::vector<std::uint32_t> ys_scalar(N, 0);
    std::vector<std::uint32_t> ys_dispatch(N, 0);

    // Scalar (ForceOff) timing for add.
    set_env_and_reload("0");
    auto t0 = std::chrono::steady_clock::now();
    poly::add_mod_p_batch(a, b, p, ys_scalar);
    auto t1 = std::chrono::steady_clock::now();

    // Dispatch (Auto) timing for add.
    set_env_and_reload(nullptr);
    auto t2 = std::chrono::steady_clock::now();
    poly::add_mod_p_batch(a, b, p, ys_dispatch);
    auto t3 = std::chrono::steady_clock::now();

    const double scalar_add_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) /
        1000.0;
    const double dispatch_add_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count()) /
        1000.0;
    std::printf("  perf_info_1M_add: scalar=%.2fms dispatch=%.2fms speedup=%.2fx\n",
                scalar_add_ms, dispatch_add_ms,
                dispatch_add_ms > 0 ? scalar_add_ms / dispatch_add_ms : 0.0);
    TEST_ASSERT(vectors_equal(ys_scalar, ys_dispatch),
                "perf_info_add: scalar and dispatch must agree");

    // Sub timing.
    std::fill(ys_scalar.begin(), ys_scalar.end(), 0);
    std::fill(ys_dispatch.begin(), ys_dispatch.end(), 0);
    set_env_and_reload("0");
    auto u0 = std::chrono::steady_clock::now();
    poly::sub_mod_p_batch(a, b, p, ys_scalar);
    auto u1 = std::chrono::steady_clock::now();
    set_env_and_reload(nullptr);
    auto u2 = std::chrono::steady_clock::now();
    poly::sub_mod_p_batch(a, b, p, ys_dispatch);
    auto u3 = std::chrono::steady_clock::now();

    const double scalar_sub_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(u1 - u0).count()) /
        1000.0;
    const double dispatch_sub_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(u3 - u2).count()) /
        1000.0;
    std::printf("  perf_info_1M_sub: scalar=%.2fms dispatch=%.2fms speedup=%.2fx\n",
                scalar_sub_ms, dispatch_sub_ms,
                dispatch_sub_ms > 0 ? scalar_sub_ms / dispatch_sub_ms : 0.0);
    TEST_ASSERT(vectors_equal(ys_scalar, ys_dispatch),
                "perf_info_sub: scalar and dispatch must agree");

    TEST_PASS("perf_info_1M");
}

static void test_reset_env_cache_hook() {
    // Ensure the reset hook re-resolves the ENV state on next access.
    ::unsetenv("GNFS_POLY_ADD_MOD_SIMD");
    poly::poly_add_mod_simd_reset_env_cache_for_testing();
    TEST_ASSERT(poly::poly_add_mod_simd_mode() ==
                    poly::PolyAddModSimdMode::Auto,
                "after reset+unset, mode should be Auto");
    ::setenv("GNFS_POLY_ADD_MOD_SIMD", "0", 1);
    poly::poly_add_mod_simd_reset_env_cache_for_testing();
    TEST_ASSERT(poly::poly_add_mod_simd_mode() ==
                    poly::PolyAddModSimdMode::ForceOff,
                "after reset+set=0, mode should be ForceOff");
    ::setenv("GNFS_POLY_ADD_MOD_SIMD", "1", 1);
    poly::poly_add_mod_simd_reset_env_cache_for_testing();
    TEST_ASSERT(poly::poly_add_mod_simd_mode() ==
                    poly::PolyAddModSimdMode::ForceOn,
                "after reset+set=1, mode should be ForceOn");
    ::unsetenv("GNFS_POLY_ADD_MOD_SIMD");
    poly::poly_add_mod_simd_reset_env_cache_for_testing();
    TEST_PASS("reset_env_cache_hook");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    std::printf("Running test_poly_add_mod_simd...\n");

    // ENV
    test_env_unset_yields_auto();
    test_env_zero_yields_force_off();
    test_env_one_yields_force_on();
    test_env_garbage_yields_auto();

    // Edges
    test_empty_input();
    test_single_coefficient();

    // Sizes
    test_aligned_32();
    test_unaligned_33();

    // Random sweeps across prime scales
    test_random_1000_small_prime();
    test_random_1000_medium_prime();
    test_random_1000_mersenne_31();
    test_p_above_2_to_31_fallback_path();

    // Sub-specific coverage
    test_sub_sweep_mixed_signs();

    // Identities + boundaries
    test_algebraic_identities();
    test_boundary_p_minus_1();

    // Dispatch parity + clamp contract
    test_force_off_vs_auto_parity();
    test_clamp_to_min_size();

    // Perf info (not assertive)
    test_perf_info_1M();

    // Test infrastructure
    test_reset_env_cache_hook();

    std::printf("\nResults: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
