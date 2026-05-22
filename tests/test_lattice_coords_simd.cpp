// test_lattice_coords_simd.cpp - Correctness tests for the SIMD-accelerated
// batch lattice-coordinate projection helper.
//
// Strategy
// --------
// For each test that exercises the dispatcher path, the test runs both the
// scalar reference (a hand-rolled `a = b1x*i + b2x*j` / `b = b1y*i + b2y*j`
// inner loop) and the dispatched helper on the same inputs, then asserts
// per-index equality on both `a_out` and `b_out`. The projection is a
// fixed linear combination of two `int64_t` inputs per cell, so any
// divergence indicates a real kernel bug — we do not tolerate any
// difference across SIMD lanes.
//
// Additional coverage:
// * ENV parsing (GNFS_LATTICE_COORDS_SIMD = auto / 0 / 1 / garbage / unset).
// * Aligned vs unaligned batch sizes — the SIMD path must fall through
//   cleanly to the scalar residual tail.
// * Identity basis (b1={1,0}, b2={0,1}) → output equals input coordinates.
// * Realistic basis vectors with mixed signs.
// * Negative coordinates / negative basis entries — signed int64 arithmetic
//   must round-trip through the SIMD register file correctly.
// * Defensive contract: undersized `a_out` / `b_out` clamps without UB
//   writes past either output span.
//
// The build wires this test into the sieve test set as
// (ctest LatticeCoordsSimd).

#include <gnfs/sieve/lattice_coords_simd.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do {                              \
    if (!(cond)) {                                               \
        std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, msg); \
        tests_failed++;                                          \
        return;                                                  \
    }                                                            \
} while (0)

#define TEST_PASS(name) do {                                      \
    std::printf("  PASS: %s\n", name);                            \
    tests_passed++;                                               \
} while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace sieve = gnfs::sieve;

// Setenv helper that flushes the ENV cache so the helper re-reads the value.
static void set_env_and_reload(const char* value) {
    if (value == nullptr) {
        ::unsetenv("GNFS_LATTICE_COORDS_SIMD");
    } else {
        ::setenv("GNFS_LATTICE_COORDS_SIMD", value, 1);
    }
    sieve::lattice_coords_simd_reset_env_cache_for_testing();
}

static bool int64_vectors_equal(const std::vector<std::int64_t>& a,
                                const std::vector<std::int64_t>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

// Run scalar reference vs dispatched helper on the same inputs and assert
// bit-for-bit parity on both `a_out` and `b_out`.
static void compare_projection(sieve::LatticeBasis basis,
                               const std::vector<std::int64_t>& i_in,
                               const std::vector<std::int64_t>& j_in,
                               const char* label) {
    const std::size_t n = i_in.size();
    std::vector<std::int64_t> a_scalar(n, 0xDEADBEEFLL);
    std::vector<std::int64_t> b_scalar(n, 0xDEADBEEFLL);
    std::vector<std::int64_t> a_dispatch(n, 0xDEADBEEFLL);
    std::vector<std::int64_t> b_dispatch(n, 0xDEADBEEFLL);

    sieve::batch_lattice_coords_scalar(basis, i_in, j_in,
                                       a_scalar, b_scalar);
    sieve::batch_lattice_coords(basis, i_in, j_in,
                                a_dispatch, b_dispatch);

    if (!int64_vectors_equal(a_scalar, a_dispatch) ||
        !int64_vectors_equal(b_scalar, b_dispatch)) {
        std::fprintf(stderr, "  projection mismatch [%s] n=%zu\n", label, n);
        for (std::size_t k = 0; k < n; ++k) {
            if (a_scalar[k] != a_dispatch[k] ||
                b_scalar[k] != b_dispatch[k]) {
                std::fprintf(stderr,
                    "    k=%zu i=%lld j=%lld scalar a=%lld b=%lld dispatch a=%lld b=%lld\n",
                    k,
                    static_cast<long long>(i_in[k]),
                    static_cast<long long>(j_in[k]),
                    static_cast<long long>(a_scalar[k]),
                    static_cast<long long>(b_scalar[k]),
                    static_cast<long long>(a_dispatch[k]),
                    static_cast<long long>(b_dispatch[k]));
                break;
            }
        }
        tests_failed++;
        return;
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
    sieve::LatticeCoordsSimdMode mode = sieve::lattice_coords_simd_mode();
    TEST_ASSERT(mode == sieve::LatticeCoordsSimdMode::Auto,
                "unset env should resolve to Auto");
    const bool enabled = sieve::lattice_coords_simd_enabled();
    const bool supported = sieve::lattice_coords_simd_supported();
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
    sieve::LatticeCoordsSimdMode mode = sieve::lattice_coords_simd_mode();
    TEST_ASSERT(mode == sieve::LatticeCoordsSimdMode::ForceOff,
                "env '0' should resolve to ForceOff");
    TEST_ASSERT(!sieve::lattice_coords_simd_enabled(),
                "ForceOff must disable the SIMD path");
    set_env_and_reload("off");
    TEST_ASSERT(sieve::lattice_coords_simd_mode() ==
                    sieve::LatticeCoordsSimdMode::ForceOff,
                "env 'off' should resolve to ForceOff");
    TEST_ASSERT(!sieve::lattice_coords_simd_enabled(),
                "ForceOff (alias 'off') must disable the SIMD path");
    set_env_and_reload(nullptr);
    TEST_PASS("env=0/off -> ForceOff");
}

// Test 3 — ENV "1" / "on" → ForceOn. When supported, enables SIMD; when not,
// falls back to scalar but the gate reports enabled() == supported.
static void test_env_force_on() {
    std::printf("[3] env=1 -> ForceOn\n");
    set_env_and_reload("1");
    sieve::LatticeCoordsSimdMode mode = sieve::lattice_coords_simd_mode();
    TEST_ASSERT(mode == sieve::LatticeCoordsSimdMode::ForceOn,
                "env '1' should resolve to ForceOn");
    const bool enabled = sieve::lattice_coords_simd_enabled();
    const bool supported = sieve::lattice_coords_simd_supported();
    TEST_ASSERT(enabled == supported,
                "ForceOn + supported must enable; ForceOn + unsupported must disable");
    set_env_and_reload("on");
    TEST_ASSERT(sieve::lattice_coords_simd_mode() ==
                    sieve::LatticeCoordsSimdMode::ForceOn,
                "env 'on' should resolve to ForceOn");
    set_env_and_reload(nullptr);
    TEST_PASS("env=1/on -> ForceOn");
}

// Test 4 — ENV "auto" / "" / "garbage" / "2" / "true" / "On" / mixed-case → Auto.
static void test_env_garbage_fallback() {
    std::printf("[4] env=auto / '' / garbage / case-variants -> Auto\n");
    for (const char* value :
         {"auto", "", "garbage", "2", "true", "On", "OFF", "01", "00"}) {
        set_env_and_reload(value);
        sieve::LatticeCoordsSimdMode mode = sieve::lattice_coords_simd_mode();
        if (mode != sieve::LatticeCoordsSimdMode::Auto) {
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

// Test 5 — empty input: helper must return without touching output spans.
static void test_empty_input() {
    std::printf("[5] empty input\n");
    set_env_and_reload(nullptr);
    sieve::LatticeBasis basis{1, 2, 3, 4};
    std::vector<std::int64_t> empty;
    std::vector<std::int64_t> a_out;
    std::vector<std::int64_t> b_out;
    sieve::batch_lattice_coords(basis, empty, empty, a_out, b_out);
    TEST_ASSERT(a_out.empty() && b_out.empty(),
                "empty input must leave empty outputs unchanged");

    // i_coords non-empty but j_coords empty -> no-op (clamp to min).
    std::vector<std::int64_t> i_only = {1, 2, 3};
    std::vector<std::int64_t> a_buf(3, 0xCAFE);
    std::vector<std::int64_t> b_buf(3, 0xCAFE);
    sieve::batch_lattice_coords(basis, i_only, empty, a_buf, b_buf);
    for (std::size_t k = 0; k < 3; ++k) {
        TEST_ASSERT(a_buf[k] == 0xCAFE && b_buf[k] == 0xCAFE,
                    "empty j_coords must leave outputs untouched");
    }
    TEST_PASS("empty input");
}

// Test 6 — single cell (n=1). Tail-only path; SIMD prefix never engages.
static void test_single_cell() {
    std::printf("[6] single cell (n=1)\n");
    set_env_and_reload(nullptr);
    sieve::LatticeBasis basis{10, -3, 2, 7};
    std::vector<std::int64_t> i_in = {5};
    std::vector<std::int64_t> j_in = {-4};
    // expected:
    //   a = 10*5  + 2*(-4)  = 50 - 8  = 42
    //   b = -3*5  + 7*(-4)  = -15 - 28 = -43
    std::vector<std::int64_t> a_out(1, 0);
    std::vector<std::int64_t> b_out(1, 0);
    sieve::batch_lattice_coords(basis, i_in, j_in, a_out, b_out);
    TEST_ASSERT(a_out[0] == 42, "single cell a mismatch");
    TEST_ASSERT(b_out[0] == -43, "single cell b mismatch");
    TEST_PASS("single cell (n=1)");
}

// Test 7 — identity basis (b1={1,0}, b2={0,1}) → a=i, b=j across all cells.
static void test_identity_basis() {
    std::printf("[7] identity basis (a=i, b=j)\n");
    set_env_and_reload(nullptr);
    sieve::LatticeBasis basis{1, 0, 0, 1};
    std::vector<std::int64_t> i_in;
    std::vector<std::int64_t> j_in;
    std::mt19937_64 rng(0xC0FFEEULL);
    for (std::size_t k = 0; k < 64; ++k) {
        i_in.push_back(static_cast<std::int64_t>(rng() & 0xFFFFFULL));
        j_in.push_back(static_cast<std::int64_t>(rng() & 0xFFFFFULL));
    }
    std::vector<std::int64_t> a_out(i_in.size(), 0);
    std::vector<std::int64_t> b_out(i_in.size(), 0);
    sieve::batch_lattice_coords(basis, i_in, j_in, a_out, b_out);
    for (std::size_t k = 0; k < i_in.size(); ++k) {
        if (a_out[k] != i_in[k] || b_out[k] != j_in[k]) {
            std::fprintf(stderr,
                "  identity mismatch at k=%zu: a=%lld i=%lld b=%lld j=%lld\n",
                k,
                static_cast<long long>(a_out[k]),
                static_cast<long long>(i_in[k]),
                static_cast<long long>(b_out[k]),
                static_cast<long long>(j_in[k]));
            tests_failed++;
            return;
        }
    }
    TEST_PASS("identity basis a=i b=j");
}

// Test 8 — realistic basis (b1={10, -3}, b2={2, 7}) over 5 hand-checked cells.
static void test_realistic_basis_hand_checked() {
    std::printf("[8] realistic basis 5 hand-checked cells\n");
    set_env_and_reload(nullptr);
    sieve::LatticeBasis basis{10, -3, 2, 7};
    std::vector<std::int64_t> i_in = {0,  1,  -2, 100, -50};
    std::vector<std::int64_t> j_in = {0, -1,   3, -10, -20};
    // expected:
    //   k=0: a=0,         b=0
    //   k=1: a=10*1+2*(-1)=8,        b=-3*1+7*(-1)=-10
    //   k=2: a=10*(-2)+2*3=-14,      b=-3*(-2)+7*3=27
    //   k=3: a=10*100+2*(-10)=980,   b=-3*100+7*(-10)=-370
    //   k=4: a=10*(-50)+2*(-20)=-540, b=-3*(-50)+7*(-20)=10
    std::vector<std::int64_t> exp_a = {0, 8, -14, 980, -540};
    std::vector<std::int64_t> exp_b = {0, -10, 27, -370, 10};
    std::vector<std::int64_t> a_out(5, 0);
    std::vector<std::int64_t> b_out(5, 0);
    sieve::batch_lattice_coords(basis, i_in, j_in, a_out, b_out);
    for (std::size_t k = 0; k < 5; ++k) {
        if (a_out[k] != exp_a[k] || b_out[k] != exp_b[k]) {
            std::fprintf(stderr,
                "  cell %zu: expected a=%lld b=%lld, got a=%lld b=%lld\n",
                k,
                static_cast<long long>(exp_a[k]),
                static_cast<long long>(exp_b[k]),
                static_cast<long long>(a_out[k]),
                static_cast<long long>(b_out[k]));
            tests_failed++;
            return;
        }
    }
    TEST_PASS("realistic basis hand-checked");
}

// Test 9 — random 100 cells parity (SIMD vs scalar).
static void test_random_100_parity() {
    std::printf("[9] random 100 cells parity\n");
    set_env_and_reload(nullptr);
    sieve::LatticeBasis basis{1234, -567, 89, 1011};
    std::vector<std::int64_t> i_in;
    std::vector<std::int64_t> j_in;
    std::mt19937_64 rng(0xABCDEFULL);
    for (std::size_t k = 0; k < 100; ++k) {
        // Keep coords small enough that a/b stay in int64 by orders of magnitude.
        i_in.push_back(static_cast<std::int64_t>(rng() & 0xFFFFULL) -
                       0x8000LL);
        j_in.push_back(static_cast<std::int64_t>(rng() & 0xFFFFULL) -
                       0x8000LL);
    }
    compare_projection(basis, i_in, j_in, "random 100 cells parity");
}

// Test 10 — random 1000 cells parity.
static void test_random_1000_parity() {
    std::printf("[10] random 1000 cells parity\n");
    set_env_and_reload(nullptr);
    sieve::LatticeBasis basis{-31415, 27182, 14142, -22360};
    std::vector<std::int64_t> i_in;
    std::vector<std::int64_t> j_in;
    std::mt19937_64 rng(0x123456ULL);
    for (std::size_t k = 0; k < 1000; ++k) {
        i_in.push_back(static_cast<std::int64_t>(rng() & 0xFFFFULL) -
                       0x8000LL);
        j_in.push_back(static_cast<std::int64_t>(rng() & 0xFFFFULL) -
                       0x8000LL);
    }
    compare_projection(basis, i_in, j_in, "random 1000 cells parity");
}

// Test 11 — ForceOff vs Auto parity on a 500-cell random batch.
static void test_force_off_vs_auto_parity() {
    std::printf("[11] ForceOff vs Auto parity (500 random cells)\n");
    sieve::LatticeBasis basis{7, -11, 13, 17};
    std::vector<std::int64_t> i_in;
    std::vector<std::int64_t> j_in;
    std::mt19937_64 rng(0xDEAD0123ULL);
    for (std::size_t k = 0; k < 500; ++k) {
        i_in.push_back(static_cast<std::int64_t>(rng() & 0xFFFFFULL) -
                       0x80000LL);
        j_in.push_back(static_cast<std::int64_t>(rng() & 0xFFFFFULL) -
                       0x80000LL);
    }

    set_env_and_reload(nullptr);  // Auto
    std::vector<std::int64_t> a_auto(500, 0);
    std::vector<std::int64_t> b_auto(500, 0);
    sieve::batch_lattice_coords(basis, i_in, j_in, a_auto, b_auto);

    set_env_and_reload("0");  // ForceOff (scalar)
    std::vector<std::int64_t> a_off(500, 0);
    std::vector<std::int64_t> b_off(500, 0);
    sieve::batch_lattice_coords(basis, i_in, j_in, a_off, b_off);

    TEST_ASSERT(int64_vectors_equal(a_auto, a_off),
                "ForceOff and Auto a_out must match bit-for-bit");
    TEST_ASSERT(int64_vectors_equal(b_auto, b_off),
                "ForceOff and Auto b_out must match bit-for-bit");

    set_env_and_reload(nullptr);
    TEST_PASS("ForceOff vs Auto parity (500 cells)");
}

// Test 12 — unaligned 33 cells (tail handling): SIMD path consumes a 2/4-wide
// prefix then falls through to the scalar residual tail for the last cell(s).
static void test_unaligned_33() {
    std::printf("[12] unaligned 33 cells (tail handling)\n");
    set_env_and_reload(nullptr);
    sieve::LatticeBasis basis{3, -5, 7, 11};
    std::vector<std::int64_t> i_in;
    std::vector<std::int64_t> j_in;
    std::mt19937_64 rng(0xFEEDFACEULL);
    for (std::size_t k = 0; k < 33; ++k) {
        i_in.push_back(static_cast<std::int64_t>(rng() & 0xFFFFULL) -
                       0x8000LL);
        j_in.push_back(static_cast<std::int64_t>(rng() & 0xFFFFULL) -
                       0x8000LL);
    }
    compare_projection(basis, i_in, j_in, "unaligned 33 cells");
}

// Test 13 — negative coords / negative basis entries verified per-index.
// Signed int64 arithmetic through the SIMD register file must round-trip
// without bit corruption (this is the main risk on x86 AVX2 where the
// extract/insert pattern could in principle drop sign bits).
static void test_negative_coords_and_basis() {
    std::printf("[13] negative coords / basis entries\n");
    set_env_and_reload(nullptr);
    sieve::LatticeBasis basis{-1000, -2000, -3000, -4000};
    std::vector<std::int64_t> i_in;
    std::vector<std::int64_t> j_in;
    std::mt19937_64 rng(0x55555555ULL);
    for (std::size_t k = 0; k < 200; ++k) {
        // All negative coords to maximise sign-bit propagation through
        // the inner mul-add chain.
        i_in.push_back(-static_cast<std::int64_t>(rng() & 0xFFFFULL) - 1);
        j_in.push_back(-static_cast<std::int64_t>(rng() & 0xFFFFULL) - 1);
    }
    compare_projection(basis, i_in, j_in, "negative coords + basis");
}

// Test 14 — undersized a_out clamps without UB write past the output.
// The clamp must reduce the per-call effective length to min of all four
// span sizes (i, j, a, b).
static void test_undersized_a_out_clamps() {
    std::printf("[14] undersized a_out clamps\n");
    set_env_and_reload(nullptr);
    sieve::LatticeBasis basis{1, 2, 3, 4};
    std::vector<std::int64_t> i_in = {10, 20, 30, 40, 50};
    std::vector<std::int64_t> j_in = {1, 2, 3, 4, 5};
    // a_out of length 2 should write only the first 2 entries; trailing
    // entries of b_out should also be unwritten (clamp is min(all)).
    std::vector<std::int64_t> a_out(2, 0xBEEF);
    std::vector<std::int64_t> b_out(5, 0xBEEF);

    sieve::batch_lattice_coords(basis, i_in, j_in, a_out, b_out);

    // Expected (only first 2 entries written): a=1*10+3*1=13, b=2*10+4*1=24
    TEST_ASSERT(a_out.size() == 2,
                "a_out size must not change");
    TEST_ASSERT(a_out[0] == 13 && a_out[1] == 1 * 20 + 3 * 2,
                "first 2 a_out entries must be projected; rest of a_out absent");
    TEST_ASSERT(b_out[0] == 24 && b_out[1] == 2 * 20 + 4 * 2,
                "first 2 b_out entries must be projected");
    // Entries at index 2..4 of b_out must remain at sentinel because the
    // dispatcher clamps to min(i,j,a,b) = 2.
    for (std::size_t k = 2; k < 5; ++k) {
        TEST_ASSERT(b_out[k] == 0xBEEF,
                    "b_out tail beyond clamp must remain untouched");
    }
    TEST_PASS("undersized a_out clamps without UB writes");
}

// Test 15 — perf-info probe (1M cells). Not an assertion on the timing;
// the test asserts SIMD vs scalar bit-for-bit equality on the full output.
static void test_perf_info_1m() {
    std::printf("[15] perf info (1M cells)\n");
    constexpr std::size_t kN = 1'000'000;
    sieve::LatticeBasis basis{12345, -6789, 4321, -9876};
    std::vector<std::int64_t> i_in(kN);
    std::vector<std::int64_t> j_in(kN);
    std::mt19937_64 rng(0xACE0ULL);
    for (std::size_t k = 0; k < kN; ++k) {
        i_in[k] = static_cast<std::int64_t>(rng() & 0xFFFFULL) - 0x8000LL;
        j_in[k] = static_cast<std::int64_t>(rng() & 0xFFFFULL) - 0x8000LL;
    }

    // Time the scalar reference path.
    set_env_and_reload("0");
    std::vector<std::int64_t> a_scalar(kN, 0);
    std::vector<std::int64_t> b_scalar(kN, 0);
    auto t0 = std::chrono::steady_clock::now();
    sieve::batch_lattice_coords(basis, i_in, j_in, a_scalar, b_scalar);
    auto t1 = std::chrono::steady_clock::now();
    double scalar_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

    // Time the SIMD (or scalar fallback) path.
    set_env_and_reload(nullptr);
    std::vector<std::int64_t> a_simd(kN, 0);
    std::vector<std::int64_t> b_simd(kN, 0);
    auto t2 = std::chrono::steady_clock::now();
    sieve::batch_lattice_coords(basis, i_in, j_in, a_simd, b_simd);
    auto t3 = std::chrono::steady_clock::now();
    double simd_us = std::chrono::duration<double, std::micro>(t3 - t2).count();

    std::printf("    scalar=%.1f us simd=%.1f us\n", scalar_us, simd_us);
    TEST_ASSERT(int64_vectors_equal(a_scalar, a_simd),
                "SIMD and scalar a_out must match on 1M cells");
    TEST_ASSERT(int64_vectors_equal(b_scalar, b_simd),
                "SIMD and scalar b_out must match on 1M cells");

    set_env_and_reload(nullptr);
    TEST_PASS("perf info (1M cells)");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    std::printf("=== test_lattice_coords_simd ===\n");
    std::printf("compile-time SIMD supported: %s\n",
                sieve::lattice_coords_simd_supported() ? "yes" : "no");
    const char* env = std::getenv("GNFS_LATTICE_COORDS_SIMD");
    std::printf("GNFS_LATTICE_COORDS_SIMD = %s\n", env ? env : "(unset)");

    test_env_unset_auto();
    test_env_force_off();
    test_env_force_on();
    test_env_garbage_fallback();
    test_empty_input();
    test_single_cell();
    test_identity_basis();
    test_realistic_basis_hand_checked();
    test_random_100_parity();
    test_random_1000_parity();
    test_force_off_vs_auto_parity();
    test_unaligned_33();
    test_negative_coords_and_basis();
    test_undersized_a_out_clamps();
    test_perf_info_1m();

    std::printf("\n=== Summary ===\n");
    std::printf("  passed: %d\n", tests_passed);
    std::printf("  failed: %d\n", tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
