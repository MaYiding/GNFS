// test_trial_div_simd.cpp - Correctness tests for the SIMD-accelerated
// batched trial-division divisibility helper
// (`include/gnfs/cofactor/trial_div_simd.hpp`).
//
// Strategy
// --------
// Every parity test builds a cofactor + prime-pool pair, runs the scalar
// reference (`batch_check_divisibility_scalar`) and the dispatcher
// (`batch_check_divisibility`), and asserts the two output index vectors
// are bit-for-bit identical. Because the SIMD inner loop only batches
// load / register allocation around the same scalar `cofactor % p`
// check, the dispatcher must agree with the reference for every input
// regardless of the runtime gate state.
//
// We also cover:
// * Three-state ENV parsing (`auto`, `0`, `1`, unset, garbage, ``,
//   `2`) into the `TrialDivSimdMode` enum.
// * Edge cases that historically catch off-by-one bugs in the residual
//   loop: empty pool, single prime, all-divisible pool, none-divisible
//   pool, "exact batch multiple", "batch + 1" length.
// * Reset hook semantics for the cached gate (test-only).

#include <gnfs/cofactor/trial_div_simd.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <span>
#include <vector>

namespace cof = gnfs::cofactor;

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

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Project-relative small prime list. Enough for the multi-divisible
// tests but small enough to be obvious by inspection.
static const std::vector<std::uint32_t> small_primes = {
    2u, 3u, 5u, 7u, 11u, 13u, 17u, 19u, 23u, 29u,
    31u, 37u, 41u, 43u, 47u, 53u, 59u, 61u, 67u, 71u,
    73u, 79u, 83u, 89u, 97u, 101u, 103u, 107u, 109u, 113u,
};

// Naive reference: identical to `batch_check_divisibility_scalar` but
// rewritten here so tests do not transitively depend on the helper they
// are validating.
static std::vector<std::uint32_t>
naive_divisible_indices(std::uint64_t cofactor,
                        std::span<const std::uint32_t> primes) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < primes.size(); ++i) {
        if (cofactor % primes[i] == 0) {
            out.push_back(static_cast<std::uint32_t>(i));
        }
    }
    return out;
}

static bool vectors_equal(const std::vector<std::uint32_t>& a,
                          const std::vector<std::uint32_t>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static void unset_env() noexcept {
#ifdef _WIN32
    _putenv("GNFS_TRIAL_DIV_SIMD=");
#else
    unsetenv("GNFS_TRIAL_DIV_SIMD");
#endif
}

static void set_env(const char* v) noexcept {
#ifdef _WIN32
    std::string s = std::string("GNFS_TRIAL_DIV_SIMD=") + v;
    _putenv(s.c_str());
#else
    setenv("GNFS_TRIAL_DIV_SIMD", v, 1);
#endif
}

// ---------------------------------------------------------------------------
// ENV parsing tests
// ---------------------------------------------------------------------------

// Test 1 — env unset → Auto. Default behaviour. We reset the cached
// gate before each env test so the lazy `std::call_once` re-fires.
static void test_env_unset_is_auto() {
    std::printf("[1] ENV unset -> Auto\n");
    unset_env();
    cof::trial_div_simd_reset_env_cache_for_testing();
    auto m = cof::trial_div_simd_mode();
    TEST_ASSERT(m == cof::TrialDivSimdMode::Auto,
                "ENV unset must yield Auto");
    TEST_PASS("ENV unset -> Auto");
}

// Test 2 — env "0" → ForceOff.
static void test_env_zero_is_force_off() {
    std::printf("[2] ENV \"0\" -> ForceOff\n");
    set_env("0");
    cof::trial_div_simd_reset_env_cache_for_testing();
    auto m = cof::trial_div_simd_mode();
    TEST_ASSERT(m == cof::TrialDivSimdMode::ForceOff,
                "ENV \"0\" must yield ForceOff");
    TEST_ASSERT(cof::trial_div_simd_enabled() == false,
                "ForceOff must disable runtime SIMD path");
    TEST_PASS("ENV \"0\" -> ForceOff");
}

// Test 3 — env "1" → ForceOn. Note: actual SIMD execution still requires
// `trial_div_simd_supported()` so the helper safely degrades on
// unsupported platforms.
static void test_env_one_is_force_on() {
    std::printf("[3] ENV \"1\" -> ForceOn\n");
    set_env("1");
    cof::trial_div_simd_reset_env_cache_for_testing();
    auto m = cof::trial_div_simd_mode();
    TEST_ASSERT(m == cof::TrialDivSimdMode::ForceOn,
                "ENV \"1\" must yield ForceOn");
    bool runtime = cof::trial_div_simd_enabled();
    bool support = cof::trial_div_simd_supported();
    // runtime should be true iff platform supports it.
    TEST_ASSERT(runtime == support,
                "ForceOn must match compile-time support flag");
    TEST_PASS("ENV \"1\" -> ForceOn");
}

// Test 4 — env "auto" → Auto.
static void test_env_auto_is_auto() {
    std::printf("[4] ENV \"auto\" -> Auto\n");
    set_env("auto");
    cof::trial_div_simd_reset_env_cache_for_testing();
    auto m = cof::trial_div_simd_mode();
    TEST_ASSERT(m == cof::TrialDivSimdMode::Auto,
                "ENV \"auto\" must yield Auto");
    TEST_PASS("ENV \"auto\" -> Auto");
}

// Test 5 — garbage / empty / "2" → Auto. Any value other than the
// recognised tokens "0" and "1" maps to Auto. This is the same lenient
// fallback used by `GNFS_SPMV_SIMD` and prevents misconfigured
// deployments from accidentally disabling the feature.
static void test_env_garbage_is_auto() {
    std::printf("[5] ENV garbage/empty/\"2\" -> Auto\n");
    for (const char* v : {"garbage", "", "2", "true", "ON", "yes", " 1", "1 "}) {
        set_env(v);
        cof::trial_div_simd_reset_env_cache_for_testing();
        auto m = cof::trial_div_simd_mode();
        if (m != cof::TrialDivSimdMode::Auto) {
            std::fprintf(stderr,
                "  ENV value %s did not yield Auto (mode=%d)\n", v,
                static_cast<int>(m));
            tests_failed++;
            return;
        }
    }
    TEST_PASS("ENV garbage/empty/\"2\" -> Auto");
}

// ---------------------------------------------------------------------------
// Correctness tests
// ---------------------------------------------------------------------------

// Test 6 — empty primes span. Dispatcher must not touch the output and
// must not deref any pointer.
static void test_empty_primes() {
    std::printf("[6] empty primes -> empty output\n");
    unset_env();
    cof::trial_div_simd_reset_env_cache_for_testing();
    std::vector<std::uint32_t> out;
    out.reserve(8);
    cof::batch_check_divisibility(123456789ull, std::span<const std::uint32_t>(),
                                  out);
    TEST_ASSERT(out.empty(), "empty primes must yield empty output");
    // Also confirm the scalar variant with the same input.
    std::vector<std::uint32_t> out2;
    cof::batch_check_divisibility_scalar(
        123456789ull, std::span<const std::uint32_t>(), out2);
    TEST_ASSERT(out2.empty(), "scalar empty input must yield empty output");
    TEST_PASS("empty primes -> empty output");
}

// Test 7 — single-prime inputs. Smallest non-trivial cases that hit only
// the scalar residual path. Verified across cofactors that are / are
// not divisible by the prime.
static void test_single_prime() {
    std::printf("[7] single-prime inputs\n");
    unset_env();
    cof::trial_div_simd_reset_env_cache_for_testing();
    const std::uint32_t p = 7u;
    std::vector<std::uint32_t> primes = {p};
    std::span<const std::uint32_t> span(primes.data(), primes.size());

    // Cofactor divisible by p.
    std::vector<std::uint32_t> out_div;
    cof::batch_check_divisibility(49ull, span, out_div);
    TEST_ASSERT(out_div.size() == 1 && out_div[0] == 0u,
                "49 must report index 0 divisible by 7");

    // Cofactor coprime to p.
    std::vector<std::uint32_t> out_nodiv;
    cof::batch_check_divisibility(50ull, span, out_nodiv);
    TEST_ASSERT(out_nodiv.empty(),
                "50 must not report any divisible prime");

    TEST_PASS("single-prime inputs");
}

// Test 8 — mixed divisible / non-divisible across 8 primes. cofactor is
// crafted so that exactly indexes {2, 5} are divisible (i.e. primes
// {5, 13}). Verifies both correctness and ordering.
static void test_mixed_8_primes() {
    std::printf("[8] mixed divisible/non-divisible 8 primes\n");
    unset_env();
    cof::trial_div_simd_reset_env_cache_for_testing();
    // primes[0..7] = 2, 3, 5, 7, 11, 13, 17, 19
    // cofactor = 5 * 13 * 23 = 1495 (coprime to all other small primes
    // in the pool), so only indexes 2 (=5) and 5 (=13) divide.
    std::vector<std::uint32_t> primes(small_primes.begin(),
                                       small_primes.begin() + 8);
    const std::uint64_t cofactor = 1495ull;
    std::vector<std::uint32_t> out;
    cof::batch_check_divisibility(
        cofactor,
        std::span<const std::uint32_t>(primes.data(), primes.size()),
        out);
    std::vector<std::uint32_t> expected = {2u, 5u};
    TEST_ASSERT(vectors_equal(out, expected),
                "mixed-divisibility output must match expected");
    TEST_PASS("mixed divisible/non-divisible 8 primes");
}

// Test 9 — parity across 100 cofactors x 30 small primes. For every
// (cofactor, primes) pair, run both the scalar reference and the SIMD
// dispatcher under the current ENV and assert exact equality.
static void test_parity_100_cofactors_30_primes() {
    std::printf("[9] parity sweep 100 cofactors x 30 primes\n");
    // Ensure Auto (default) so the dispatcher chooses SIMD where
    // supported and scalar otherwise.
    unset_env();
    cof::trial_div_simd_reset_env_cache_for_testing();
    std::mt19937_64 rng(0x900D5EEDULL);
    std::span<const std::uint32_t> span(small_primes.data(),
                                        small_primes.size());

    int mismatches = 0;
    for (int trial = 0; trial < 100; ++trial) {
        // Mix small cofactors (many divisors) with large 60-bit cofactors
        // (typical real-world cases) by alternating.
        std::uint64_t c = (trial & 1)
            ? rng() & ((1ull << 60) - 1)
            : static_cast<std::uint64_t>(rng() % 1'000'000ull);
        if (c == 0) c = 1;
        std::vector<std::uint32_t> simd_out;
        cof::batch_check_divisibility(c, span, simd_out);
        std::vector<std::uint32_t> ref = naive_divisible_indices(c, span);
        if (!vectors_equal(simd_out, ref)) {
            std::fprintf(stderr,
                "  parity mismatch trial=%d cofactor=%llu\n", trial,
                static_cast<unsigned long long>(c));
            mismatches++;
            if (mismatches > 3) break;
        }
    }
    TEST_ASSERT(mismatches == 0, "parity sweep must produce no mismatches");
    TEST_PASS("parity sweep 100 cofactors x 30 primes");
}

// Test 10 — large batch parity (1000 primes). Generates a long prime
// list (synthetic odd numbers serving as test divisors) so the SIMD
// 4-lane loop iterates many times and the tail path also runs (1000 is
// not a multiple of 4 -> tail trips).
static void test_large_batch_parity() {
    std::printf("[10] large batch parity (1000 primes)\n");
    // Use 1003 entries (not multiple of 4) to force tail residual.
    unset_env();
    cof::trial_div_simd_reset_env_cache_for_testing();

    std::vector<std::uint32_t> primes;
    primes.reserve(1003);
    // First 30 real primes, then synthetic odd values starting at 117.
    primes.insert(primes.end(), small_primes.begin(), small_primes.end());
    for (std::uint32_t v = 117u; primes.size() < 1003; v += 2u) {
        primes.push_back(v);
    }
    std::span<const std::uint32_t> span(primes.data(), primes.size());

    int mismatches = 0;
    std::mt19937_64 rng(0xC0FFEEULL);
    for (int trial = 0; trial < 20; ++trial) {
        std::uint64_t c = rng();
        if (c == 0) c = 1;
        std::vector<std::uint32_t> simd_out;
        cof::batch_check_divisibility(c, span, simd_out);
        std::vector<std::uint32_t> ref = naive_divisible_indices(c, span);
        if (!vectors_equal(simd_out, ref)) {
            std::fprintf(stderr,
                "  large-batch mismatch trial=%d cofactor=%llu "
                "(simd=%zu, ref=%zu)\n", trial,
                static_cast<unsigned long long>(c),
                simd_out.size(), ref.size());
            mismatches++;
            if (mismatches > 3) break;
        }
    }
    TEST_ASSERT(mismatches == 0,
                "large batch must produce no mismatches");
    TEST_PASS("large batch parity (1000 primes)");
}

// Bonus test 11 — sweep batch boundaries (n in {0..8}). Catches off-by-
// one between the 4-lane loop and the scalar tail.
static void test_batch_boundary_sweep() {
    std::printf("[11] batch boundary sweep n=0..8\n");
    unset_env();
    cof::trial_div_simd_reset_env_cache_for_testing();
    // Cofactor = 2*3*5*7*11 = 2310, divides primes[0..4].
    std::uint64_t cofactor = 2310ull;
    for (std::size_t n = 0; n <= 8; ++n) {
        std::vector<std::uint32_t> primes(small_primes.begin(),
                                          small_primes.begin() + n);
        std::span<const std::uint32_t> span(primes.data(), primes.size());
        std::vector<std::uint32_t> simd_out;
        cof::batch_check_divisibility(cofactor, span, simd_out);
        std::vector<std::uint32_t> ref = naive_divisible_indices(cofactor, span);
        if (!vectors_equal(simd_out, ref)) {
            std::fprintf(stderr,
                "  boundary mismatch n=%zu (simd=%zu ref=%zu)\n", n,
                simd_out.size(), ref.size());
            tests_failed++;
            return;
        }
    }
    TEST_PASS("batch boundary sweep n=0..8");
}

// Bonus test 12 — ForceOff path must equal scalar reference (sanity
// check that the dispatcher routes through `batch_check_divisibility_scalar`
// when SIMD is disabled).
static void test_force_off_matches_scalar() {
    std::printf("[12] ForceOff path matches scalar reference\n");
    set_env("0");
    cof::trial_div_simd_reset_env_cache_for_testing();
    std::uint64_t cofactor = 30030ull;  // 2*3*5*7*11*13
    std::span<const std::uint32_t> span(small_primes.data(),
                                        small_primes.size());
    std::vector<std::uint32_t> simd_out;
    cof::batch_check_divisibility(cofactor, span, simd_out);
    std::vector<std::uint32_t> ref = naive_divisible_indices(cofactor, span);
    TEST_ASSERT(vectors_equal(simd_out, ref),
                "ForceOff dispatcher must match scalar reference");
    // Restore default for downstream tests.
    unset_env();
    cof::trial_div_simd_reset_env_cache_for_testing();
    TEST_PASS("ForceOff path matches scalar reference");
}

// Bonus test 13 — output is append (does not clear existing entries).
// The helper documents that callers must clear `out_divisible_indices`
// themselves; we lock that contract in here.
static void test_append_semantics() {
    std::printf("[13] dispatcher appends (does not clear)\n");
    unset_env();
    cof::trial_div_simd_reset_env_cache_for_testing();
    std::vector<std::uint32_t> out = {999u, 998u};  // pre-existing
    std::vector<std::uint32_t> primes = {2u, 3u, 5u};
    cof::batch_check_divisibility(
        6ull, std::span<const std::uint32_t>(primes.data(), primes.size()),
        out);
    // 6 divides 2 and 3 (indexes 0 and 1) but not 5.
    std::vector<std::uint32_t> expected = {999u, 998u, 0u, 1u};
    TEST_ASSERT(vectors_equal(out, expected),
                "dispatcher must append, not clear");
    TEST_PASS("dispatcher appends (does not clear)");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    std::printf("=== test_trial_div_simd ===\n");
    std::printf("compile-time SIMD supported: %s\n",
                cof::trial_div_simd_supported() ? "yes" : "no");
    const char* env_at_start = std::getenv("GNFS_TRIAL_DIV_SIMD");
    std::printf("GNFS_TRIAL_DIV_SIMD at start: %s\n",
                env_at_start ? env_at_start : "(unset)");

    test_env_unset_is_auto();
    test_env_zero_is_force_off();
    test_env_one_is_force_on();
    test_env_auto_is_auto();
    test_env_garbage_is_auto();
    test_empty_primes();
    test_single_prime();
    test_mixed_8_primes();
    test_parity_100_cofactors_30_primes();
    test_large_batch_parity();
    test_batch_boundary_sweep();
    test_force_off_matches_scalar();
    test_append_semantics();

    std::printf("\n=== Summary ===\n");
    std::printf("  passed: %d\n", tests_passed);
    std::printf("  failed: %d\n", tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
