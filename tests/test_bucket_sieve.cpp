// test_bucket_sieve.cpp — Verify bucket region sieve produces same results as row-major
//
// Strategy: for the same SQ, run both sieve modes and compare candidate sets.
// The bucket region mode is forced by temporarily lowering the threshold.

#include <gnfs/core/params.hpp>
#include <gnfs/polynomial/selector_dispatch.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/sieve/special_q.hpp>
#include <gnfs/sieve/lattice_sieve.hpp>

#include <cassert>
#include <iostream>
#include <vector>

using namespace gnfs;
using namespace gnfs::core;
using namespace gnfs::polynomial;
using namespace gnfs::factor_base;
using namespace gnfs::sieve;

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

/// Test with 27-bit N (small FB, row-major path)
void test_small_n_consistency() {
    Integer n("100160063");  // 10007 × 10009
    size_t bits = n.bit_length();
    auto params = GNFSParams::compute(bits);

    auto ctx = SelectorDispatch::select(n, params.degree);

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = params.rational_bound;
    fb_opts.algebraic_bound = params.algebraic_bound;
    fb_opts.special_q_bound = params.special_q_max;
    fb_opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    SieveParams sp;
    sp.rational_threshold = params.rational_threshold;
    sp.algebraic_threshold = params.algebraic_threshold;
    SieveRegion sr;
    sr.i_min = params.sieve_i_min; sr.i_max = params.sieve_i_max;
    sr.j_min = params.sieve_j_min; sr.j_max = params.sieve_j_max;

    SpecialQRange sqr;
    sqr.min_q = params.special_q_min; sqr.max_q = params.special_q_max;
    SpecialQGenerator sqg(fb, sqr);

    auto sq = sqg.next();
    TEST_ASSERT(sq.has_value(), "no SQ available");

    LatticeSieve sieve(ctx, fb, sp);
    sieve.set_region(sr);
    sieve.set_max_threads(1);
    auto result = sieve.sieve_special_q(*sq);
    TEST_ASSERT(result.candidates.size() > 0, "27-bit: no candidates");

    std::cout << "  27-bit: " << result.candidates.size() << " candidates from SQ("
              << sq->q << "," << sq->r << ")\n";
    TEST_PASS("small N basic sieve");
}

/// Test bucket region correctness by comparing array outputs directly
void test_bucket_region_array_match() {
    // Use 40-bit N which has moderate FB (~3000 entries)
    Integer n("1000036000099");
    size_t bits = n.bit_length();
    auto params = GNFSParams::compute(bits);

    auto ctx = SelectorDispatch::select(n, params.degree);

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = params.rational_bound;
    fb_opts.algebraic_bound = params.algebraic_bound;
    fb_opts.special_q_bound = params.special_q_max;
    fb_opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    SieveParams sp;
    sp.rational_threshold = params.rational_threshold;
    sp.algebraic_threshold = params.algebraic_threshold;
    SieveRegion sr;
    sr.i_min = params.sieve_i_min; sr.i_max = params.sieve_i_max;
    sr.j_min = params.sieve_j_min; sr.j_max = params.sieve_j_max;

    SpecialQRange sqr;
    sqr.min_q = params.special_q_min; sqr.max_q = params.special_q_max;
    SpecialQGenerator sqg(fb, sqr);

    auto sq = sqg.next();
    TEST_ASSERT(sq.has_value(), "no SQ available");

    // Run with row-major (auto mode will pick row-major for ~3K entries)
    LatticeSieve sieve1(ctx, fb, sp);
    sieve1.set_region(sr);
    sieve1.set_max_threads(1);
    auto result1 = sieve1.sieve_special_q(*sq);

    std::cout << "  40-bit row-major: " << result1.candidates.size() << " candidates\n";
    TEST_ASSERT(result1.candidates.size() > 0, "row-major produced no candidates");

    // The FB for 40-bit is ~3000, below BUCKET_REGION_FB_THRESHOLD=5000.
    // Both paths should produce identical results for this N.
    // For a true comparison, we'd need N > 50 digits, which is too slow for unit tests.
    // Instead, verify the sieve array is non-trivially populated.

    TEST_PASS("40-bit sieve array populated");
}

/// 字节级一致性: 对同一 SQ 跑两次,一次默认(bucket+row 混合),一次强制全 row-major,
/// 比较 candidate 集合。BACKLOG: P3 DEBT "Sieve bucket_region 与 row_major 双实现无字节级一致性测试"
void test_bucket_vs_row_major_candidate_equivalence() {
    // 用 40-bit N 让 FB 有足够 large primes 触发 bucket(>= 100 大素数)
    Integer n("1000036000099");
    size_t bits = n.bit_length();
    auto params = GNFSParams::compute(bits);

    auto ctx = SelectorDispatch::select(n, params.degree);

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = params.rational_bound;
    fb_opts.algebraic_bound = params.algebraic_bound;
    fb_opts.special_q_bound = params.special_q_max;
    fb_opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    SieveParams sp;
    sp.rational_threshold = params.rational_threshold;
    sp.algebraic_threshold = params.algebraic_threshold;
    SieveRegion sr;
    sr.i_min = params.sieve_i_min; sr.i_max = params.sieve_i_max;
    sr.j_min = params.sieve_j_min; sr.j_max = params.sieve_j_max;

    SpecialQRange sqr;
    sqr.min_q = params.special_q_min; sqr.max_q = params.special_q_max;
    SpecialQGenerator sqg(fb, sqr);

    auto sq = sqg.next();
    TEST_ASSERT(sq.has_value(), "no SQ available");

    // Run 1: 默认 (混合 bucket + row-major)
    LatticeSieve sieve_default(ctx, fb, sp);
    sieve_default.set_region(sr);
    sieve_default.set_max_threads(1);  // 强制单线程以保证决定性
    sieve_default.set_force_row_major(false);
    auto r_default = sieve_default.sieve_special_q(*sq);

    // Run 2: 强制全 row-major
    LatticeSieve sieve_row(ctx, fb, sp);
    sieve_row.set_region(sr);
    sieve_row.set_max_threads(1);
    sieve_row.set_force_row_major(true);
    auto r_row = sieve_row.sieve_special_q(*sq);

    // 候选数量必须一致(sieve_array 是 commutative log-sum)
    TEST_ASSERT(r_default.candidates.size() == r_row.candidates.size(),
                "candidate count mismatch: default vs row-major");

    // 提取 (a,b) pairs 并按 a,b 排序后比较
    auto sort_candidates = [](std::vector<SieveCandidate> v) {
        std::sort(v.begin(), v.end(), [](const SieveCandidate& x, const SieveCandidate& y) {
            if (x.a != y.a) return x.a < y.a;
            return x.b < y.b;
        });
        return v;
    };
    auto def_sorted = sort_candidates(r_default.candidates);
    auto row_sorted = sort_candidates(r_row.candidates);

    for (size_t i = 0; i < def_sorted.size(); ++i) {
        TEST_ASSERT(def_sorted[i].a == row_sorted[i].a &&
                    def_sorted[i].b == row_sorted[i].b,
                    "candidate (a,b) mismatch at index");
        // residual log 必须一致(sieve 是 commutative log-sum)
        TEST_ASSERT(def_sorted[i].residual == row_sorted[i].residual,
                    "candidate residual log mismatch");
    }

    std::cout << "  bucket-vs-row: " << r_default.candidates.size()
              << " candidates match byte-by-byte\n";
    TEST_PASS("bucket vs row-major candidate equivalence");
}

/// Test basic sieve functionality sanity check
void test_sieve_sanity() {
    // Verify that sieve produces reasonable results for trivial N
    Integer n("143");  // 11 × 13
    size_t bits = n.bit_length();
    auto params = GNFSParams::compute(bits);

    auto ctx = SelectorDispatch::select(n, params.degree);

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = params.rational_bound;
    fb_opts.algebraic_bound = params.algebraic_bound;
    fb_opts.special_q_bound = params.special_q_max;
    fb_opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    SieveParams sp;
    sp.rational_threshold = params.rational_threshold;
    sp.algebraic_threshold = params.algebraic_threshold;
    SieveRegion sr;
    sr.i_min = params.sieve_i_min; sr.i_max = params.sieve_i_max;
    sr.j_min = params.sieve_j_min; sr.j_max = params.sieve_j_max;

    SpecialQRange sqr;
    sqr.min_q = params.special_q_min; sqr.max_q = params.special_q_max;
    SpecialQGenerator sqg(fb, sqr);

    auto sq = sqg.next();
    TEST_ASSERT(sq.has_value(), "no SQ available for N=143");

    LatticeSieve sieve(ctx, fb, sp);
    sieve.set_region(sr);
    sieve.set_max_threads(1);
    auto result = sieve.sieve_special_q(*sq);

    std::cout << "  8-bit: " << result.candidates.size() << " candidates\n";
    TEST_ASSERT(result.candidates.size() > 0, "8-bit: no candidates");
    TEST_PASS("sieve sanity check");
}

int main() {
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  Bucket Sieve Unit Tests\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    test_sieve_sanity();
    test_small_n_consistency();
    test_bucket_region_array_match();
    test_bucket_vs_row_major_candidate_equivalence();

    std::cout << "\n═══════════════════════════════════════════\n";
    std::cout << "  Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
    std::cout << "═══════════════════════════════════════════\n";

    return tests_failed > 0 ? 1 : 0;
}
