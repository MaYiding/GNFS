// test_line_sieve.cpp — Verify line sieve produces valid candidates
//
// LineSieve 已标 [[deprecated]] (未集成到 Pipeline);此测试仅做算法对照,
// 抑制 deprecated 警告。Pipeline 用 LatticeSieve / SIQS。

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <gnfs/core/params.hpp>
#include <gnfs/polynomial/selector_dispatch.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/sieve/line_sieve.hpp>
#include <gnfs/cofactor/cofactorizer.hpp>

#include <iostream>

using namespace gnfs;
using namespace gnfs::core;
using namespace gnfs::polynomial;
using namespace gnfs::factor_base;
using namespace gnfs::sieve;
using namespace gnfs::cofactor;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; \
        tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_PASS(name) do { \
    std::cout << "  PASS: " << name << "\n"; \
    tests_passed++; \
} while(0)

/// Test line sieve produces candidates for trivial N
void test_trivial_n() {
    Integer n("143");  // 11 × 13
    auto params = GNFSParams::compute(n.bit_length());
    auto ctx = SelectorDispatch::select(n, params.degree);

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = params.rational_bound;
    fb_opts.algebraic_bound = params.algebraic_bound;
    fb_opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    LineSieve sieve(ctx, fb);
    auto lp = LineSieve::auto_params(n.bit_length());
    // Use smaller range for speed
    lp.a_min = -500; lp.a_max = 500; lp.b_max = 50;
    auto result = sieve.sieve(lp);

    std::cout << "  N=143: " << result.candidates.size() << " candidates"
              << " (area=" << result.sieved_positions << ")\n";
    TEST_ASSERT(result.candidates.size() > 0, "N=143 should produce candidates");
    TEST_PASS("trivial N=143");
}

/// Test line sieve candidates can be verified by cofactorizer
void test_cofactorizer_integration() {
    Integer n("96091");  // 307 × 313
    auto params = GNFSParams::compute(n.bit_length());
    auto ctx = SelectorDispatch::select(n, params.degree);

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = params.rational_bound;
    fb_opts.algebraic_bound = params.algebraic_bound;
    fb_opts.special_q_bound = params.special_q_max;
    fb_opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    LineSieve sieve(ctx, fb);
    LineSieve::Params lp;
    lp.a_min = -5000; lp.a_max = 5000; lp.b_max = 200;
    auto result = sieve.sieve(lp);

    std::cout << "  N=96091: " << result.candidates.size() << " candidates\n";
    TEST_ASSERT(result.candidates.size() > 10, "should produce many candidates");

    // Verify some candidates with cofactorizer
    CofactorizerConfig cc;
    cc.large_prime_bound = fb.params().large_prime_bound;
    cc.allow_1lp = true;
    Cofactorizer cofac(ctx, fb, cc);

    size_t verified = 0;
    size_t checked = std::min(result.candidates.size(), size_t(200));
    for (size_t i = 0; i < checked; ++i) {
        const auto& c = result.candidates[i];
        auto rel = cofac.verify(c, 0, 0);  // No SQ
        if (rel) verified++;
    }

    std::cout << "  Verified " << verified << "/" << checked << " candidates\n";
    TEST_ASSERT(verified > 0, "at least some candidates should verify as relations");
    TEST_PASS("cofactorizer integration");
}

/// Test auto_params gives reasonable ranges
void test_auto_params() {
    auto p8 = LineSieve::auto_params(8);
    auto p40 = LineSieve::auto_params(40);
    auto p80 = LineSieve::auto_params(80);

    TEST_ASSERT(p8.a_max - p8.a_min < p40.a_max - p40.a_min, "larger N should have wider range");
    TEST_ASSERT(p40.a_max - p40.a_min < p80.a_max - p80.a_min, "larger N should have wider range");
    TEST_ASSERT(p8.b_max < p40.b_max, "larger N should have more b values");
    TEST_PASS("auto_params scaling");
}

int main() {
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  Line Sieve Unit Tests\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    test_auto_params();
    test_trivial_n();
    test_cofactorizer_integration();

    std::cout << "\n═══════════════════════════════════════════\n";
    std::cout << "  Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
    std::cout << "═══════════════════════════════════════════\n";

    return tests_failed > 0 ? 1 : 0;
}

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
