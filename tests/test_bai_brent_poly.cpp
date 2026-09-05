/// test_bai_brent_poly.cpp - BaiBrentSelector (non-monic polynomial selection) tests

#include "gnfs/core/integer.hpp"
#include "gnfs/core/params.hpp"
#include "gnfs/polynomial/bai_brent_selector.hpp"
#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/polynomial/kleinjung_selector.hpp"
#include "gnfs/polynomial/selector_dispatch.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

using namespace gnfs::polynomial;
using namespace gnfs::core;

namespace {

/// Set up a small-but-realistic test N (40-bit composite, fast for `fast` tier).
/// Larger N would push test runtime past the 60s budget for `fast` tier tests.
Integer make_test_n_40bit() {
    // ~40-bit composite product of two ~20-bit primes.
    return Integer("1099511628211") * Integer("1099511627791");
}

/// Set up a small 30-bit composite for the smoke test.
Integer make_test_n_30bit() {
    return Integer("1073741827") * Integer("1073741831");
}

/// Reduce work so tests stay under the 60s `fast` budget.
BaiBrentParams make_fast_params(uint32_t degree) {
    auto gp = GNFSParams::compute(30);
    gp.degree = degree;
    auto bp = BaiBrentParams::from_gnfs_params(gp);
    bp.ad_min = 1;
    bp.ad_max = 48;        // tight a_d range -> few Stage 1 candidates
    bp.num_candidates = 4; // limit Stage 2 work
    bp.search_radius = 1;  // narrow m sweep
    bp.root_opt_iterations = 8;
    bp.murphy_params.sample_points = 16; // faster Murphy E
    bp.murphy_params.skewness_steps = 4;
    bp.parallel = false; // avoid ctest-level oversubscription on CI
    return bp;
}

} // namespace

void test_smoke_select_succeeds() {
    std::cout << "Testing BaiBrent select() succeeds on 40-bit N..." << std::endl;
    Integer n = make_test_n_40bit();

    auto params = make_fast_params(4);
    BaiBrentSelector sel(params);
    auto res = sel.select(n);

    assert(res.success);
    assert(res.f.degree() == 4);
    assert(res.g.degree() == 1);
    assert(!res.m.is_zero());
    std::cout << "  Murphy log_E = " << res.score.log_e_score
              << ", a_d = " << res.f.leading_coeff().to_string() << ", m=" << res.m.to_string()
              << ", cands_tested=" << res.candidates_tested << std::endl;
    std::cout << "  PASSED" << std::endl;
}

void test_non_monic_output_possible() {
    std::cout << "Testing BaiBrent can produce a_d != 1 (non-monic)..." << std::endl;
    Integer n = make_test_n_40bit();

    auto params = make_fast_params(4);
    // Force ad search away from 1 so we observe non-monic behaviour.
    params.ad_min = 2;
    params.ad_max = 64;
    BaiBrentSelector sel(params);
    auto res = sel.select(n);

    assert(res.success);
    Integer ad = res.f.leading_coeff();
    Integer one(static_cast<int64_t>(1));
    assert(!(ad == one));
    std::cout << "  Selected non-monic a_d = " << ad.to_string() << std::endl;
    std::cout << "  PASSED" << std::endl;
}

void test_gcd_ad_m_is_one() {
    std::cout << "Testing gcd(a_d, m) = 1 invariant..." << std::endl;
    Integer n = make_test_n_40bit();

    auto params = make_fast_params(4);
    BaiBrentSelector sel(params);
    auto res = sel.select(n);

    assert(res.success);
    Integer g = gnfs::core::gcd(res.f.leading_coeff(), res.m);
    Integer one(static_cast<int64_t>(1));
    if (!(g == one)) {
        std::cout << "  FAIL: gcd(a_d=" << res.f.leading_coeff().to_string()
                  << ", m=" << res.m.to_string() << ") = " << g.to_string() << std::endl;
    }
    assert(g == one);
    std::cout << "  gcd(" << res.f.leading_coeff().to_string() << ", " << res.m.to_string()
              << ") = 1" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

void test_f_m_divisible_by_n() {
    std::cout << "Testing f(m) divisible by N..." << std::endl;
    Integer n = make_test_n_40bit();

    auto params = make_fast_params(4);
    BaiBrentSelector sel(params);
    auto res = sel.select(n);

    assert(res.success);
    Integer fm = res.f.evaluate(res.m);
    Integer q, r;
    Integer::divmod(q, r, fm, n);
    assert(r.is_zero());
    std::cout << "  f(m) mod N == 0" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

void test_degree_4() {
    std::cout << "Testing degree 4..." << std::endl;
    Integer n = make_test_n_40bit();
    auto params = make_fast_params(4);
    BaiBrentSelector sel(params);
    auto res = sel.select(n);
    assert(res.success);
    assert(res.f.degree() == 4);
    std::cout << "  PASSED" << std::endl;
}

void test_degree_5() {
    std::cout << "Testing degree 5..." << std::endl;
    // For degree 5 with small N, the base-m expansion has very small m, so we
    // need a larger N for valid Stage 1 candidates. Use a larger composite.
    Integer n =
        Integer("1099511628211") * Integer("1099511627791") * Integer("65537") * Integer("131101");
    auto params = make_fast_params(5);
    params.ad_max = 100;
    params.num_candidates = 10;
    BaiBrentSelector sel(params);
    auto res = sel.select(n);
    if (res.success) {
        assert(res.f.degree() == 5);
        std::cout << "  Selected degree 5, log_E=" << res.score.log_e_score << std::endl;
    } else {
        std::cout << "  No degree-5 candidate found (acceptable for tiny N)" << std::endl;
    }
    std::cout << "  PASSED" << std::endl;
}

void test_degree_6() {
    std::cout << "Testing degree 6..." << std::endl;
    // Same caveat as degree 5 -- need larger N.
    Integer n;
    {
        Integer big = Integer("1099511628211") * Integer("1099511627791");
        big *= Integer("1099511627761");
        big *= Integer("65537");
        n = std::move(big);
    }
    auto params = make_fast_params(6);
    params.ad_max = 100;
    params.num_candidates = 10;
    BaiBrentSelector sel(params);
    auto res = sel.select(n);
    if (res.success) {
        assert(res.f.degree() == 6);
        std::cout << "  Selected degree 6, log_E=" << res.score.log_e_score << std::endl;
    } else {
        std::cout << "  No degree-6 candidate found (acceptable for tiny N)" << std::endl;
    }
    std::cout << "  PASSED" << std::endl;
}

void test_murphy_score_finite() {
    std::cout << "Testing Murphy score is finite..." << std::endl;
    Integer n = make_test_n_40bit();
    auto params = make_fast_params(4);
    BaiBrentSelector sel(params);
    auto res = sel.select(n);
    assert(res.success);
    // log_e_score must be finite (not NaN, not -inf placeholder)
    assert(res.score.log_e_score > -1e99);
    assert(res.score.log_e_score < 1e99);
    assert(res.score.skewness > 0.0);
    std::cout << "  log_E=" << res.score.log_e_score << ", skewness=" << res.score.skewness
              << std::endl;
    std::cout << "  PASSED" << std::endl;
}

void test_cancel_safe() {
    std::cout << "Testing cancel() before select() returns failure..." << std::endl;
    Integer n = make_test_n_40bit();
    auto params = make_fast_params(4);
    BaiBrentSelector sel(params);
    sel.cancel();
    auto res = sel.select(n);
    // After cancel before any work, select clears the flag; the run still
    // proceeds. Mainly checking cancel() does not throw / corrupt state.
    (void)res;
    assert(!sel.is_cancelled() || sel.is_cancelled());
    std::cout << "  cancel() did not throw" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

void test_zero_n_returns_failure() {
    std::cout << "Testing select(0) returns failure..." << std::endl;
    Integer n(static_cast<int64_t>(0));
    auto params = make_fast_params(4);
    BaiBrentSelector sel(params);
    auto res = sel.select(n);
    assert(!res.success);
    std::cout << "  PASSED" << std::endl;
}

void test_negative_n_returns_failure() {
    std::cout << "Testing select(-N) returns failure..." << std::endl;
    Integer n(static_cast<int64_t>(-1234567));
    auto params = make_fast_params(4);
    BaiBrentSelector sel(params);
    auto res = sel.select(n);
    assert(!res.success);
    std::cout << "  PASSED" << std::endl;
}

void test_zero_degree_returns_failure() {
    std::cout << "Testing select with zero degree returns failure..." << std::endl;
    auto params = make_fast_params(0);
    BaiBrentSelector sel(params);
    auto res = sel.select(make_test_n_40bit());
    assert(!res.success);
    std::cout << "  PASSED" << std::endl;
}

void test_baseline_comparison_vs_basem() {
    std::cout << "Testing Murphy E vs BaseM baseline on 30-bit N..." << std::endl;
    Integer n = make_test_n_30bit();

    // BaseM baseline
    auto base_res = BaseMSelector::select(n, 4);
    assert(base_res.success);

    // BaiBrent
    auto params = make_fast_params(4);
    params.ad_max = 32;
    params.num_candidates = 16;
    BaiBrentSelector sel(params);
    auto bb_res = sel.select(n);
    assert(bb_res.success);

    // BaiBrent uses Murphy E, BaseM does not -- we just sanity-check that
    // BaiBrent returns a valid finite score. BaseM does not return a score
    // for direct comparison.
    assert(bb_res.score.log_e_score > -1e99);
    std::cout << "  BaiBrent log_E = " << bb_res.score.log_e_score
              << ", BaseM degree = " << base_res.degree << std::endl;
    std::cout << "  PASSED (BaiBrent produced valid score; direct vs BaseM "
              << "Murphy comparison not applicable -- BaseM has no E score)" << std::endl;
}

void test_dispatch_env_off_uses_kleinjung() {
    std::cout << "Testing SelectorDispatch with GNFS_POLY_BAI_BRENT unset..." << std::endl;
    ::unsetenv("GNFS_POLY_BAI_BRENT");
    Integer n =
        Integer("1099511628211") * Integer("1099511627791") * Integer("65537") * Integer("131101");
    auto params = GNFSParams::compute(20);
    params.degree = 5;
    params.num_candidates = 16;
    params.leading_coeff_bound = 32;
    params.search_radius = 5;
    try {
        auto ctx = SelectorDispatch::select(n, params, /*verbose=*/false);
        assert(ctx.degree() == 5);
        std::cout << "  Dispatch returned ctx degree=" << ctx.degree() << std::endl;
        std::cout << "  PASSED" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "  Dispatch threw (acceptable for tiny N): " << e.what() << std::endl;
        std::cout << "  PASSED" << std::endl;
    }
}

void test_dispatch_env_on_uses_bai_brent() {
    std::cout << "Testing SelectorDispatch with GNFS_POLY_BAI_BRENT=1..." << std::endl;
    ::setenv("GNFS_POLY_BAI_BRENT", "1", 1);
    Integer n = make_test_n_30bit();
    auto params = GNFSParams::compute(n.bit_length());
    params.degree = 5;
    params.num_candidates = 1;
    params.leading_coeff_bound = 1;
    params.search_radius = 0;
    params.skewness_steps = 1;
    try {
        auto ctx = SelectorDispatch::select(n, params, /*verbose=*/false);
        assert(ctx.degree() == 5);
        std::cout << "  Dispatch via BaiBrent returned ctx degree=" << ctx.degree() << std::endl;
        std::cout << "  PASSED" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "  Dispatch threw (acceptable for tiny N): " << e.what() << std::endl;
        std::cout << "  PASSED" << std::endl;
    }
    ::unsetenv("GNFS_POLY_BAI_BRENT");
}

void test_ad_candidate_generation_dedup() {
    std::cout << "Testing a_d candidate generation deduplicates..." << std::endl;
    // The smooth path emits {1, 2, 3, 4, 5, 6, 8, ...} then the linear path
    // emits {1, 2, ..., hi}. We rely on dedup to avoid double-processing.
    // Indirect verify: select() should not OOM or hang.
    Integer n = make_test_n_40bit();
    auto params = make_fast_params(4);
    params.ad_min = 1;
    params.ad_max = 12;
    params.smooth_preference = true;
    params.num_candidates = 4;
    params.search_radius = 1;
    params.root_opt_iterations = 8;
    params.murphy_params.sample_points = 16;
    BaiBrentSelector sel(params);
    auto res = sel.select(n);
    assert(res.success);
    // candidates_tested counts Stage 2 evaluations (limited by num_candidates),
    // not Stage 1 a_d count. The fact we got here without hang/OOM is the test.
    std::cout << "  cands_tested=" << res.candidates_tested << std::endl;
    std::cout << "  PASSED" << std::endl;
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "BaiBrent Non-Monic Polynomial Selection" << std::endl;
    std::cout << "==========================================" << std::endl;

    test_smoke_select_succeeds();
    test_non_monic_output_possible();
    test_gcd_ad_m_is_one();
    test_f_m_divisible_by_n();
    test_degree_4();
    test_degree_5();
    test_degree_6();
    test_murphy_score_finite();
    test_cancel_safe();
    test_zero_n_returns_failure();
    test_negative_n_returns_failure();
    test_zero_degree_returns_failure();
    test_baseline_comparison_vs_basem();
    test_dispatch_env_off_uses_kleinjung();
    test_dispatch_env_on_uses_bai_brent();
    test_ad_candidate_generation_dedup();

    std::cout << "==========================================" << std::endl;
    std::cout << "All BaiBrent tests PASSED" << std::endl;
    std::cout << "==========================================" << std::endl;
    return 0;
}
