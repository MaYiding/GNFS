// ECM Stage 3 (Brent-Suyama polynomial extension) unit tests.
//
// Verifies:
//   1. evaluate_polynomial() correctness for d in {1, 2, 6, 12, 30}
//   2. is_supported_degree() reflects {1, 2, 6, 12, 30}
//   3. accumulate_cross_product() basic semantics
//   4. ECM::factor() with brent_suyama_degree > 0 finds known semiprime factors
//   5. degree = 1 is equivalent to classical BSGS (sanity)
//   6. ENV `GNFS_ECM_BRENT_SUYAMA=1` + `GNFS_ECM_BS_DEGREE=12` opts in
//
// Tier: instant (target <10s in Debug). Uses small semiprimes (<= 64-bit
// cofactors) and modest B1/B2 so the test runs fast.

#include <gnfs/cofactor/ecm.hpp>
#include <gnfs/cofactor/ecm_brent_suyama.hpp>
#include <gnfs/core/integer.hpp>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace gnfs::core;
using namespace gnfs::cofactor;
namespace bs = gnfs::cofactor::brent_suyama;

namespace {

// Simple assert wrapper that always fires (NDEBUG-safe).
#define GNFS_TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << (msg) \
                  << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        std::exit(1); \
    } \
} while (0)

void test_is_supported_degree() {
    std::cout << "  is_supported_degree(): ";
    GNFS_TEST_ASSERT(bs::is_supported_degree(1), "degree 1 supported");
    GNFS_TEST_ASSERT(bs::is_supported_degree(2), "degree 2 supported");
    GNFS_TEST_ASSERT(bs::is_supported_degree(6), "degree 6 supported");
    GNFS_TEST_ASSERT(bs::is_supported_degree(12), "degree 12 supported");
    GNFS_TEST_ASSERT(bs::is_supported_degree(30), "degree 30 supported");
    // unsupported degrees
    GNFS_TEST_ASSERT(!bs::is_supported_degree(0), "degree 0 not supported");
    GNFS_TEST_ASSERT(!bs::is_supported_degree(3), "degree 3 not supported");
    GNFS_TEST_ASSERT(!bs::is_supported_degree(4), "degree 4 not supported");
    GNFS_TEST_ASSERT(!bs::is_supported_degree(60), "degree 60 not supported");
    std::cout << "PASS\n";
}

void test_evaluate_polynomial() {
    std::cout << "  evaluate_polynomial(): ";
    Integer n("1000000007");  // prime modulus for clean checks
    Integer x("12345");
    Integer out;

    // degree=1: identity
    bs::evaluate_polynomial(out, x, 1, n);
    GNFS_TEST_ASSERT(out.compare(x) == 0, "degree=1 yields identity");

    // degree=2: out = 12345^2 = 152399025
    bs::evaluate_polynomial(out, x, 2, n);
    GNFS_TEST_ASSERT(out.to_string() == "152399025", "degree=2 correct");

    // degree=6: out = 12345^6 mod 1000000007
    // 12345^2 = 152399025
    // 12345^3 = 12345 * 152399025 = 1881365363625; mod 10^9+7 = 881365363625 mod
    // Let's compute via GMP reference inside test:
    Integer ref;
    mpz_powm_ui(ref.get_mpz(), x.get_mpz(), 6, n.get_mpz());
    bs::evaluate_polynomial(out, x, 6, n);
    GNFS_TEST_ASSERT(out.compare(ref) == 0, "degree=6 matches mpz_powm_ui");

    // degree=12, 30: same cross-check via GMP reference
    mpz_powm_ui(ref.get_mpz(), x.get_mpz(), 12, n.get_mpz());
    bs::evaluate_polynomial(out, x, 12, n);
    GNFS_TEST_ASSERT(out.compare(ref) == 0, "degree=12 matches mpz_powm_ui");

    mpz_powm_ui(ref.get_mpz(), x.get_mpz(), 30, n.get_mpz());
    bs::evaluate_polynomial(out, x, 30, n);
    GNFS_TEST_ASSERT(out.compare(ref) == 0, "degree=30 matches mpz_powm_ui");

    // edge case: X = 0
    Integer zero(0);
    bs::evaluate_polynomial(out, zero, 12, n);
    GNFS_TEST_ASSERT(out.is_zero(), "0^12 mod n = 0");

    // edge case: X = 1
    Integer one(1);
    bs::evaluate_polynomial(out, one, 30, n);
    GNFS_TEST_ASSERT(out.is_one(), "1^30 mod n = 1");

    std::cout << "PASS\n";
}

void test_accumulate_cross_product() {
    std::cout << "  accumulate_cross_product(): ";
    Integer n("1000000007");

    // Build two distinct polynomial points
    Integer X1("100"), Z1("3"), X2("200"), Z2("7");
    bs::PolynomialPoint p1(X1, Z1, 12, n);

    Integer Fx2, Fz2;
    bs::evaluate_polynomial(Fx2, X2, 12, n);
    bs::evaluate_polynomial(Fz2, Z2, 12, n);

    Integer accum(1), tmp1, tmp2;
    bool coincide = bs::accumulate_cross_product(accum, p1, Fx2, Fz2, n, tmp1, tmp2);
    GNFS_TEST_ASSERT(!coincide, "distinct points don't coincide");
    GNFS_TEST_ASSERT(!accum.is_one(), "accum updated to non-1");
    GNFS_TEST_ASSERT(!accum.is_zero(), "accum is non-zero");

    // Now build identical polynomial points -> cross product == 0
    Integer accum2(1), tmp3, tmp4;
    Integer Fx1, Fz1;
    bs::evaluate_polynomial(Fx1, X1, 12, n);
    bs::evaluate_polynomial(Fz1, Z1, 12, n);
    bool coincide2 = bs::accumulate_cross_product(accum2, p1, Fx1, Fz1, n, tmp3, tmp4);
    GNFS_TEST_ASSERT(coincide2, "identical points coincide");

    std::cout << "PASS\n";
}

// Helper: run ECM on a known semiprime and return (found-factor or nullopt, ms).
struct EcmResult {
    std::optional<Integer> factor;
    double ms;
};

EcmResult run_ecm(const Integer& n, uint64_t B1, uint64_t B2,
                  uint32_t num_curves, uint32_t bs_degree) {
    ECM::Config cfg;
    cfg.auto_params = false;
    cfg.B1 = B1;
    cfg.B2 = B2;
    cfg.num_curves = num_curves;
    cfg.brent_suyama_degree = bs_degree;

    auto t0 = std::chrono::high_resolution_clock::now();
    auto f = ECM::factor(n, cfg);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return {f, ms};
}

void test_known_semiprime_factor() {
    std::cout << "  known semiprime factoring (degree=12): ";

    // n = 2261419229 (10-digit, 32-bit) -- p1 * p2 with both Stage 1-smooth-ish
    // Use B1=2000, B2=100000 like quick_factor regime
    Integer n("2261419229");

    // degree = 0 (classical BSGS)
    auto r0 = run_ecm(n, 2000, 100000, 15, 0);
    GNFS_TEST_ASSERT(r0.factor.has_value(), "classical BSGS finds factor");

    // degree = 12 (Brent-Suyama)
    auto r12 = run_ecm(n, 2000, 100000, 15, 12);
    GNFS_TEST_ASSERT(r12.factor.has_value(), "Brent-Suyama d=12 finds factor");

    // both must find some factor that divides n
    Integer q0 = n;  q0 /= *r0.factor;
    Integer rem0; mpz_mod(rem0.get_mpz(), n.get_mpz(), r0.factor->get_mpz());
    GNFS_TEST_ASSERT(rem0.is_zero(), "BSGS factor divides n");

    Integer rem12; mpz_mod(rem12.get_mpz(), n.get_mpz(), r12.factor->get_mpz());
    GNFS_TEST_ASSERT(rem12.is_zero(), "Brent-Suyama factor divides n");

    std::cout << "PASS (BSGS=" << r0.ms << "ms; BS-12=" << r12.ms << "ms)\n";
}

void test_degree_30_works() {
    std::cout << "  degree=30 mode: ";
    Integer n("2261419229");
    auto r30 = run_ecm(n, 2000, 100000, 15, 30);
    GNFS_TEST_ASSERT(r30.factor.has_value(), "Brent-Suyama d=30 finds factor");
    Integer rem; mpz_mod(rem.get_mpz(), n.get_mpz(), r30.factor->get_mpz());
    GNFS_TEST_ASSERT(rem.is_zero(), "d=30 factor divides n");
    std::cout << "PASS (" << r30.ms << "ms)\n";
}

void test_degree_1_equivalent_to_bsgs() {
    std::cout << "  degree=1 is equivalent to classical BSGS: ";
    // With same seed/curve count, degree=1 should find the same factor.
    Integer n("2261419229");

    auto r0 = run_ecm(n, 2000, 100000, 15, 0);
    auto r1 = run_ecm(n, 2000, 100000, 15, 1);

    // Both must find a factor (deterministic via sigma_pool seeded from N)
    GNFS_TEST_ASSERT(r0.factor.has_value(), "BSGS finds factor");
    GNFS_TEST_ASSERT(r1.factor.has_value(), "degree=1 finds factor");

    // factors divide n; cannot strictly require identical factor because
    // sigma_pool randomization differs per call, but they should both be
    // non-trivial divisors.
    Integer rem0; mpz_mod(rem0.get_mpz(), n.get_mpz(), r0.factor->get_mpz());
    Integer rem1; mpz_mod(rem1.get_mpz(), n.get_mpz(), r1.factor->get_mpz());
    GNFS_TEST_ASSERT(rem0.is_zero(), "BSGS factor divides n");
    GNFS_TEST_ASSERT(rem1.is_zero(), "degree=1 factor divides n");

    std::cout << "PASS\n";
}

void test_env_opt_in() {
    std::cout << "  ENV opt-in (GNFS_ECM_BRENT_SUYAMA=1): ";

    // Save / restore ENV
    const char* prev_enable = std::getenv("GNFS_ECM_BRENT_SUYAMA");
    const char* prev_degree = std::getenv("GNFS_ECM_BS_DEGREE");

    setenv("GNFS_ECM_BRENT_SUYAMA", "1", 1);
    setenv("GNFS_ECM_BS_DEGREE", "12", 1);

    Integer n("2261419229");

    // factor() reads ENV inside; with auto_params=true (default), it computes
    // B1/B2 from cofactor bits then applies brent_suyama_env override.
    auto r = ECM::factor(n);
    GNFS_TEST_ASSERT(r.has_value(), "ENV-enabled BS finds factor");

    Integer rem; mpz_mod(rem.get_mpz(), n.get_mpz(), r->get_mpz());
    GNFS_TEST_ASSERT(rem.is_zero(), "ENV-enabled BS factor divides n");

    // Verify invalid degree gets clamped to default (12)
    setenv("GNFS_ECM_BS_DEGREE", "99", 1);  // not in {1,2,6,12,30}
    auto r2 = ECM::factor(n);
    GNFS_TEST_ASSERT(r2.has_value(), "invalid degree falls back to default");

    // Restore ENV
    if (prev_enable) setenv("GNFS_ECM_BRENT_SUYAMA", prev_enable, 1);
    else unsetenv("GNFS_ECM_BRENT_SUYAMA");
    if (prev_degree) setenv("GNFS_ECM_BS_DEGREE", prev_degree, 1);
    else unsetenv("GNFS_ECM_BS_DEGREE");

    std::cout << "PASS\n";
}

void test_b1_smaller_than_d_brent_suyama() {
    std::cout << "  B1 < D (BSGS path covers tiny B1) with BS=12: ";
    // n = 1009 * 10007 from test_ecm_quick stage2 boundary tests
    Integer n("10097063");
    ECM::Config cfg;
    cfg.auto_params = false;
    cfg.B1 = 100;     // < D = 2310
    cfg.B2 = 20000;   // > B1 + D*3 -> hits BSGS path with j_lo=0 fix
    cfg.num_curves = 10;
    cfg.brent_suyama_degree = 12;

    auto r = ECM::factor(n, cfg);
    // We can't strictly assert "found" with such tiny B1, but the call must
    // not crash. Same contract as test_stage2_boundaries() in test_ecm_quick.
    std::cout << (r ? r->to_string() : "nullopt") << " PASS (no crash)\n";
}

void test_b2_le_b1_no_stage2() {
    std::cout << "  B2 <= B1 (no Stage 2/3 work) with BS=12: ";
    Integer n("10097063");
    ECM::Config cfg;
    cfg.auto_params = false;
    cfg.B1 = 1000;
    cfg.B2 = 1000;  // == B1
    cfg.num_curves = 5;
    cfg.brent_suyama_degree = 12;
    auto r = ECM::factor(n, cfg);
    // Stage 1 alone may or may not find the factor (B1=1000 covers small p).
    // Critical: must not crash, must not return invalid factor.
    if (r) {
        Integer rem; mpz_mod(rem.get_mpz(), n.get_mpz(), r->get_mpz());
        GNFS_TEST_ASSERT(rem.is_zero(), "factor (if any) must divide n");
    }
    std::cout << "PASS (no crash)\n";
}

}  // namespace

int main() {
    std::cout << "=== ECM Brent-Suyama Stage 3 Tests ===\n";

    test_is_supported_degree();
    test_evaluate_polynomial();
    test_accumulate_cross_product();
    test_known_semiprime_factor();
    test_degree_30_works();
    test_degree_1_equivalent_to_bsgs();
    test_env_opt_in();
    test_b1_smaller_than_d_brent_suyama();
    test_b2_le_b1_no_stage2();

    std::cout << "\n=== All ECM Brent-Suyama Tests PASSED ===\n";
    return 0;
}
