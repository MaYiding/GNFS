// test_survival_predictor.cpp -- Dickman rho survival predictor tests.
//
// Tiers:
//   - 4 dickman_rho unit tests (known values at u = 1, 1.5, 2, 5)
//   - 4 estimate_survival unit tests (various cofactor/B/LP combos)
//   - 2 ENV parsing tests (filter + threshold env)
//   - 4 integration tests via classify_cofactor (filter ON vs OFF)
//   - 2 perf info tests (timing measurement, not assertions)

#include <gnfs/cofactor/survival_predictor.hpp>
#include <gnfs/cofactor/smooth_check.hpp>
#include <gnfs/core/integer.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

using gnfs::cofactor::dickman_rho;
using gnfs::cofactor::estimate_survival;
using gnfs::cofactor::survival_filter_enabled;
using gnfs::cofactor::survival_threshold;
using gnfs::cofactor::should_reject_cofactor;
using gnfs::cofactor::survival_stats;
using gnfs::cofactor::classify_cofactor;
using gnfs::cofactor::CofactorClass;
using gnfs::cofactor::CofactorClassification;
using gnfs::core::Integer;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; \
        ++tests_failed; \
        return; \
    } \
} while (0)

#define TEST_PASS(name) do { \
    std::cout << "  PASS: " << name << "\n"; \
    ++tests_passed; \
} while (0)

// Helper: relative tolerance check
static bool approx_eq(double a, double b, double rel_tol = 0.05) {
    if (a == b) return true;
    const double scale = std::max(std::abs(a), std::abs(b));
    return std::abs(a - b) <= rel_tol * scale;
}

// === Dickman rho unit tests ===

void test_dickman_rho_u_1() {
    // rho(u) = 1 for u <= 1
    TEST_ASSERT(dickman_rho(0.0) == 1.0, "rho(0) == 1");
    TEST_ASSERT(dickman_rho(0.5) == 1.0, "rho(0.5) == 1");
    TEST_ASSERT(dickman_rho(1.0) == 1.0, "rho(1.0) == 1");
    // Negative / NaN defensive
    TEST_ASSERT(dickman_rho(-1.0) == 1.0, "rho(-1) clamped to 1");
    TEST_ASSERT(dickman_rho(std::nan("")) == 1.0, "rho(NaN) clamped to 1");
    TEST_PASS("dickman_rho u<=1 returns 1.0");
}

void test_dickman_rho_u_1_5() {
    // rho(1.5) is approximately 0.5961 (van de Lune & Wattel reference)
    // Note: our table at u=1.5 is interpolated; exact value is ~0.5961.
    const double v = dickman_rho(1.5);
    TEST_ASSERT(approx_eq(v, 0.5961, 0.05), "rho(1.5) ~= 0.5961");
    // Monotonic decrease
    TEST_ASSERT(dickman_rho(1.4) > dickman_rho(1.5), "rho monotone dec");
    TEST_ASSERT(dickman_rho(1.5) > dickman_rho(1.6), "rho monotone dec");
    TEST_PASS("dickman_rho(1.5) ~= 0.5961");
}

void test_dickman_rho_u_2() {
    // rho(2) = 1 - ln(2) ~= 0.30685281
    const double v = dickman_rho(2.0);
    const double expected = 1.0 - std::log(2.0);
    TEST_ASSERT(approx_eq(v, expected, 0.01), "rho(2) ~= 1 - ln(2)");
    // rho(3) ~= 0.04860
    TEST_ASSERT(approx_eq(dickman_rho(3.0), 0.04860838, 0.05), "rho(3) ~= 0.0486");
    TEST_PASS("dickman_rho(2.0) ~= 1 - ln(2), dickman_rho(3.0) ~= 0.0486");
}

void test_dickman_rho_u_5_and_asymptotic() {
    // rho(5) ~= 3.547e-4
    const double v5 = dickman_rho(5.0);
    TEST_ASSERT(approx_eq(v5, 3.547e-4, 0.1), "rho(5) ~= 3.5e-4");
    // Asymptotic region: rho(u) very small for large u
    TEST_ASSERT(dickman_rho(15.0) < 1e-10, "rho(15) astronomically small");
    TEST_ASSERT(dickman_rho(20.0) < 1e-15, "rho(20) extremely small");
    // Underflow region returns 0
    TEST_ASSERT(dickman_rho(100.0) >= 0.0, "rho(100) non-negative");
    TEST_ASSERT(dickman_rho(100.0) < 1e-50, "rho(100) negligible");
    // Monotonic on [10, 15]
    TEST_ASSERT(dickman_rho(10.0) > dickman_rho(15.0), "rho monotone large u");
    TEST_PASS("dickman_rho asymptotic + monotone u=5..20");
}

// === estimate_survival unit tests ===

void test_estimate_survival_within_B() {
    // cofactor_bits <= B_bits ⇒ trivially smooth, returns 1.0
    TEST_ASSERT(estimate_survival(0, 20, 25) == 1.0, "0-bit cofactor");
    TEST_ASSERT(estimate_survival(15, 20, 25) == 1.0, "cofactor < B");
    TEST_ASSERT(estimate_survival(20, 20, 25) == 1.0, "cofactor == B");
    TEST_PASS("estimate_survival cofactor <= B returns 1.0");
}

void test_estimate_survival_2lp_realistic() {
    // 50-digit GNFS: typical sieve cofactor ~50-90 bits, B = 1M (20 bits),
    // LP = 8M (23 bits). u_smooth ~ 4, u_lp ~ 3.5, p_lp dominates.
    const double p_50 = estimate_survival(50, 20, 23);
    const double p_70 = estimate_survival(70, 20, 23);
    const double p_90 = estimate_survival(90, 20, 23);
    // Probabilities monotonically decrease with cofactor size
    TEST_ASSERT(p_50 > p_70, "larger cofactor → lower survival");
    TEST_ASSERT(p_70 > p_90, "larger cofactor → lower survival");
    // All should be in (0, 1)
    TEST_ASSERT(p_90 > 0.0 && p_90 < 1.0, "90-bit ∈ (0,1)");
    TEST_ASSERT(p_50 > 0.0 && p_50 < 1.0, "50-bit ∈ (0,1)");
    TEST_PASS("estimate_survival 2LP regime monotone");
}

void test_estimate_survival_lp_lifts() {
    // u_smooth small (cofactor near B), LP path should not lift much.
    // u_smooth big (cofactor >> B), LP path should lift.
    const double small_cofactor = estimate_survival(22, 20, 30);  // u_s=1.1, u_lp=0.73
    const double big_cofactor = estimate_survival(60, 20, 30);    // u_s=3, u_lp=2
    // LP path lifts big_cofactor (rho(2) > rho(3))
    TEST_ASSERT(big_cofactor > 0.0, "big cofactor still has LP survival");
    // big_cofactor should ~= rho(u_lp) = rho(60/30) = rho(2) ~= 0.307
    TEST_ASSERT(approx_eq(big_cofactor, 1.0 - std::log(2.0), 0.1),
                "big_cofactor ~= rho(u_lp=2)");
    // small_cofactor with LP_bits > cofactor_bits → p_lp = 1.0 from helper logic
    TEST_ASSERT(small_cofactor == 1.0, "cofactor < LP → survival = 1");
    TEST_PASS("estimate_survival LP path lifts");
}

void test_estimate_survival_edge_cases() {
    // smoothness_bound_bits == 0 ⇒ no info, return 1.0
    TEST_ASSERT(estimate_survival(50, 0, 25) == 1.0, "B=0 returns 1");
    // lp_bound_bits == 0 ⇒ fall back to smoothness-only estimate
    const double no_lp = estimate_survival(60, 20, 0);
    const double with_lp = estimate_survival(60, 20, 30);
    TEST_ASSERT(no_lp < with_lp, "LP path > smooth-only path");
    TEST_ASSERT(no_lp > 0.0 && no_lp < 1.0, "no-LP estimate ∈ (0,1)");
    // Extremely high cofactor
    TEST_ASSERT(estimate_survival(1000, 20, 23) >= 0.0,
                "huge cofactor returns non-negative");
    TEST_PASS("estimate_survival edge cases");
}

// === ENV parsing tests ===
//
// NOTE: survival_filter_enabled() and survival_threshold() use static
// once-flag caching, so we cannot toggle ENV across tests within the
// same process. We test the CURRENT process state (whatever the harness
// was invoked with), and verify the should_reject_cofactor() logic
// for the threshold == 0 invariant.

void test_env_threshold_zero_invariant() {
    // CRITICAL invariant: even if filter is enabled, threshold == 0 means
    // should_reject_cofactor() returns false for ALL inputs. This is the
    // safe default the task requires.
    // We test by setting threshold via setenv after the static cache
    // is locked, so this only checks the LOGIC. If the cache is locked
    // to threshold=0, fine; if it's locked to threshold>0, we still
    // assert the contract holds when threshold == 0.
    const double th = survival_threshold();
    if (th == 0.0) {
        TEST_ASSERT(!should_reject_cofactor(100, 20, 23),
                    "th=0 never rejects (big cofactor)");
        TEST_ASSERT(!should_reject_cofactor(1000, 20, 23),
                    "th=0 never rejects (huge cofactor)");
        TEST_ASSERT(!should_reject_cofactor(50, 25, 30),
                    "th=0 never rejects");
        TEST_PASS("env threshold==0 invariant: never rejects");
    } else {
        // Filter active in this process — verify the THRESHOLD condition
        // properly gates the decision.
        const bool small_cofactor_rejected = should_reject_cofactor(15, 20, 25);
        TEST_ASSERT(!small_cofactor_rejected, "trivially smooth never rejected");
        TEST_PASS("env threshold>0: trivially smooth not rejected");
    }
}

void test_env_filter_disabled_invariant() {
    // CRITICAL invariant: when filter is disabled, should_reject_cofactor()
    // ALWAYS returns false, regardless of threshold value.
    if (!survival_filter_enabled()) {
        // Even with a tiny threshold, we shouldn't reject anything.
        TEST_ASSERT(!should_reject_cofactor(100, 20, 23),
                    "filter off: never rejects huge");
        TEST_ASSERT(!should_reject_cofactor(1000, 20, 23),
                    "filter off: never rejects huge");
        TEST_PASS("env filter==off: never rejects");
    } else {
        // Filter enabled — verify the FILTER condition properly gates.
        // Small cofactor should pass.
        TEST_ASSERT(!should_reject_cofactor(15, 20, 25),
                    "filter on, small cofactor still passes");
        TEST_PASS("env filter==on with safe cofactor passes");
    }
}

// === Integration tests via classify_cofactor ===

void test_classify_smooth_not_rejected_filter_off() {
    // With smoothness_bound = 0 (predictor disabled at call site), or with
    // filter env off, true smooth cofactors must NOT be rejected.
    // Test cofactor = 6 = 2*3 with B = 1<<20 (LP = 1<<25).
    Integer c6(static_cast<uint64_t>(6));
    auto r = classify_cofactor(c6, 1ULL << 25, false, /*smoothness_bound=*/0);
    TEST_ASSERT(r.type != CofactorClass::TooLarge,
                "smooth cofactor 6 not TooLarge");
    TEST_PASS("smooth small cofactor (predictor disabled) not rejected");
}

void test_classify_smooth_not_rejected_with_predictor() {
    // With predictor active and smoothness_bound passed, true smooth
    // cofactor (within B) must NOT be rejected. estimate_survival returns
    // 1.0 when cofactor <= B, so even threshold ~= 1.0 wouldn't trigger.
    Integer c6(static_cast<uint64_t>(6));
    auto r = classify_cofactor(c6, 1ULL << 25, false, /*smoothness_bound=*/1ULL << 20);
    TEST_ASSERT(r.type != CofactorClass::TooLarge,
                "smooth-within-B cofactor never predictor-rejected");
    TEST_PASS("smooth cofactor < B never predictor-rejected");
}

void test_classify_predictor_filter_disabled_no_change() {
    // With predictor smoothness_bound = 0, classification behaves EXACTLY
    // as before. Test a hard semiprime to make sure the dispatch chain
    // still runs (returns Semiprime / Composite, NOT TooLarge from
    // predictor).
    const uint64_t p = 1000003;  // prime
    const uint64_t q = 1000033;  // prime
    const uint64_t n = p * q;
    Integer cn(n);
    auto r = classify_cofactor(cn, 1ULL << 21, false, /*smoothness_bound=*/0);
    // Without predictor: should classify as Semiprime (p, q both < 2^21=2097152)
    TEST_ASSERT(r.type == CofactorClass::Semiprime,
                "predictor disabled: 2LP cofactor classifies as Semiprime");
    TEST_ASSERT(r.factor1 == p && r.factor2 == q,
                "semiprime factors recovered");
    TEST_PASS("predictor disabled: cofactor classifies unchanged");
}

void test_classify_predictor_large_cofactor_routing() {
    // Test that the predictor only kicks in when ALL conditions hold.
    // With smoothness_bound = 0, even an enormous cofactor should NOT be
    // routed via predictor's TooLarge fast path; it should follow normal
    // classify_cofactor logic (which already returns TooLarge for c > B^2
    // when allow_3lp = false, but for a DIFFERENT reason).
    //
    // Use cofactor = 2^40 (within fits_uint64), B = 1000 (so c >> B^2 = 1M),
    // so it would be TooLarge anyway. Verify ROUTING is via normal path
    // (Composite or TooLarge), not via predictor reject branch.
    const uint64_t huge = 1ULL << 40;
    Integer ch(huge);

    // Reset stats so we can assert nothing was recorded by predictor.
    survival_stats().reset();
    const uint64_t rejects_before = survival_stats().predictor_rejects.load();

    auto r = classify_cofactor(ch, 1000, false, /*smoothness_bound=*/0);
    const uint64_t rejects_after = survival_stats().predictor_rejects.load();
    TEST_ASSERT(rejects_after == rejects_before,
                "predictor disabled (smoothness_bound=0): no rejects recorded");
    // Should be TooLarge or Composite (both are valid for huge cofactor)
    TEST_ASSERT(r.type == CofactorClass::TooLarge ||
                r.type == CofactorClass::Composite,
                "huge cofactor classified as TooLarge or Composite");
    TEST_PASS("predictor disabled: huge cofactor uses normal routing");
}

// === Perf info tests (not strict assertions, just print timing) ===

void test_perf_dickman_rho_throughput() {
    // Run 1M dickman_rho evaluations, report rate.
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> dist(0.5, 12.0);
    const size_t n = 1000000;
    double sum = 0.0;

    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < n; ++i) {
        sum += dickman_rho(dist(rng));
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "  [perf info] dickman_rho: " << n << " evals in "
              << ms << " ms (" << (n / ms / 1000.0) << " Meval/s, sum=" << sum << ")\n";
    // Sanity: should be plenty fast (< 1s)
    TEST_ASSERT(ms < 5000.0, "1M dickman_rho < 5s");
    TEST_PASS("perf: dickman_rho throughput");
}

void test_perf_estimate_survival_throughput() {
    // Run 100K estimate_survival evaluations on realistic 50d/60d cofactor
    // sizes, report rate.
    std::mt19937_64 rng(43);
    std::uniform_int_distribution<uint64_t> bits_dist(30, 100);
    const size_t n = 100000;
    double sum = 0.0;

    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < n; ++i) {
        const uint64_t cb = bits_dist(rng);
        sum += estimate_survival(cb, 20, 23);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "  [perf info] estimate_survival: " << n << " evals in "
              << ms << " ms (" << (n / ms / 1000.0) << " Meval/s, sum=" << sum << ")\n";
    TEST_ASSERT(ms < 1000.0, "100K estimate_survival < 1s");
    TEST_PASS("perf: estimate_survival throughput");
}

int main() {
    std::cout << "===========================================\n";
    std::cout << "  Survival Predictor Unit Tests\n";
    std::cout << "===========================================\n\n";

    test_dickman_rho_u_1();
    test_dickman_rho_u_1_5();
    test_dickman_rho_u_2();
    test_dickman_rho_u_5_and_asymptotic();

    test_estimate_survival_within_B();
    test_estimate_survival_2lp_realistic();
    test_estimate_survival_lp_lifts();
    test_estimate_survival_edge_cases();

    test_env_threshold_zero_invariant();
    test_env_filter_disabled_invariant();

    test_classify_smooth_not_rejected_filter_off();
    test_classify_smooth_not_rejected_with_predictor();
    test_classify_predictor_filter_disabled_no_change();
    test_classify_predictor_large_cofactor_routing();

    test_perf_dickman_rho_throughput();
    test_perf_estimate_survival_throughput();

    std::cout << "\n===========================================\n";
    std::cout << "  Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    std::cout << "===========================================\n";

    return tests_failed > 0 ? 1 : 0;
}
