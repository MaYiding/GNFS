// test_bucket_prefetch.cpp — Verify bucket prefetch produces identical output
//
// Strategy:
//   1. Run lattice sieve with `GNFS_BUCKET_PREFETCH=0` (prefetch disabled).
//   2. Run lattice sieve with `GNFS_BUCKET_PREFETCH=1` (prefetch enabled).
//   3. Compare candidate vectors byte-for-byte after sorting by (a, b).
//   4. Validate the ENV-gate parser independently.
//   5. Optionally print informational timings (no assertion) when
//      GNFS_BUCKET_PREFETCH_PERF=1 so PMU sweeps can read them out of the
//      test log without slowing the default instant test.
//
// The test exercises the bucket region path by using a 40-bit input that
// produces ~3K factor-base entries — enough to trigger `sieve_bucket_region`
// when `large_primes.size() >= 100`. Same shape as `test_bucket_sieve`.

#include <gnfs/core/params.hpp>
#include <gnfs/polynomial/selector_dispatch.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/sieve/special_q.hpp>
#include <gnfs/sieve/lattice_sieve.hpp>
#include <gnfs/sieve/bucket_prefetch.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using namespace gnfs;
using namespace gnfs::core;
using namespace gnfs::polynomial;
using namespace gnfs::factor_base;
using namespace gnfs::sieve;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do {                                            \
    if (!(cond)) {                                                             \
        std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n";        \
        tests_failed++;                                                        \
        return;                                                                \
    }                                                                          \
} while (0)

#define TEST_PASS(name) do {                                                   \
    std::cout << "  PASS: " << name << "\n";                                   \
    tests_passed++;                                                            \
} while (0)

namespace {

void set_env_and_reload(const char* value) {
    if (value == nullptr) {
        ::unsetenv("GNFS_BUCKET_PREFETCH");
    } else {
        ::setenv("GNFS_BUCKET_PREFETCH", value, /*overwrite=*/1);
    }
    reload_bucket_prefetch_gate();
}

struct SieveSetup {
    Integer n;
    PolynomialContext ctx;
    FactorBase fb;
    SieveParams sp;
    SieveRegion sr;
    SpecialQ sq;
};

SieveSetup build_setup_40bit() {
    SieveSetup s{Integer("1000036000099"), PolynomialContext{},
                  FactorBase{}, SieveParams{}, SieveRegion{}, SpecialQ{}};
    size_t bits = s.n.bit_length();
    auto params = GNFSParams::compute(bits);

    s.ctx = SelectorDispatch::select(s.n, params.degree);

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = params.rational_bound;
    fb_opts.algebraic_bound = params.algebraic_bound;
    fb_opts.special_q_bound = params.special_q_max;
    fb_opts.parallel = false;
    s.fb = FactorBaseBuilder::build(s.ctx, fb_opts);

    s.sp.rational_threshold = params.rational_threshold;
    s.sp.algebraic_threshold = params.algebraic_threshold;

    s.sr.i_min = params.sieve_i_min;
    s.sr.i_max = params.sieve_i_max;
    s.sr.j_min = params.sieve_j_min;
    s.sr.j_max = params.sieve_j_max;

    SpecialQRange sqr;
    sqr.min_q = params.special_q_min;
    sqr.max_q = params.special_q_max;
    SpecialQGenerator sqg(s.fb, sqr);

    auto opt_sq = sqg.next();
    assert(opt_sq.has_value() && "no SQ available for fixture");
    s.sq = *opt_sq;
    return s;
}

std::vector<SieveCandidate>
sieve_once(const SieveSetup& s, bool force_row_major = false) {
    LatticeSieve sieve(s.ctx, s.fb, s.sp);
    sieve.set_region(s.sr);
    sieve.set_max_threads(1);  // deterministic ordering
    sieve.set_force_row_major(force_row_major);
    auto result = sieve.sieve_special_q(s.sq);
    return std::move(result.candidates);
}

std::vector<SieveCandidate>
sort_candidates(std::vector<SieveCandidate> v) {
    std::sort(v.begin(), v.end(),
              [](const SieveCandidate& x, const SieveCandidate& y) {
                  if (x.a != y.a) return x.a < y.a;
                  return x.b < y.b;
              });
    return v;
}

bool candidates_equal(const std::vector<SieveCandidate>& a,
                      const std::vector<SieveCandidate>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].a != b[i].a) return false;
        if (a[i].b != b[i].b) return false;
        if (a[i].i != b[i].i) return false;
        if (a[i].j != b[i].j) return false;
        if (a[i].residual != b[i].residual) return false;
    }
    return true;
}

}  // namespace

/// Correctness 1: bucket region path bit-for-bit identical between
/// prefetch ON and OFF for a 40-bit fixture.
void test_bucket_region_parity_40bit() {
    auto setup = build_setup_40bit();

    set_env_and_reload("0");
    TEST_ASSERT(!bucket_prefetch_enabled(),
                "GNFS_BUCKET_PREFETCH=0 should disable gate");
    auto off = sort_candidates(sieve_once(setup, /*force_row_major=*/false));

    set_env_and_reload("1");
    if (bucket_prefetch_supported()) {
        TEST_ASSERT(bucket_prefetch_enabled(),
                    "GNFS_BUCKET_PREFETCH=1 should enable gate when supported");
    }
    auto on = sort_candidates(sieve_once(setup, /*force_row_major=*/false));

    TEST_ASSERT(off.size() == on.size(),
                "candidate count differs between prefetch states");
    TEST_ASSERT(candidates_equal(off, on),
                "candidate (a,b,i,j,residual) differs between prefetch states");

    std::cout << "  bucket region parity: " << off.size()
              << " candidates match bit-for-bit\n";
    TEST_PASS("bucket region prefetch parity");
}

/// Correctness 2: row-major bucket apply path bit-for-bit identical
/// between prefetch ON and OFF (force_row_major=true exercises
/// `sieve_row_chunk` instead of `sieve_bucket_region`).
void test_row_major_parity_40bit() {
    auto setup = build_setup_40bit();

    set_env_and_reload("0");
    auto off = sort_candidates(sieve_once(setup, /*force_row_major=*/true));

    set_env_and_reload("1");
    auto on = sort_candidates(sieve_once(setup, /*force_row_major=*/true));

    TEST_ASSERT(off.size() == on.size(),
                "row-major candidate count differs between prefetch states");
    TEST_ASSERT(candidates_equal(off, on),
                "row-major candidate residual / (a,b) differs between prefetch states");

    std::cout << "  row-major parity: " << off.size()
              << " candidates match bit-for-bit\n";
    TEST_PASS("row-major prefetch parity");
}

/// Correctness 3: prefetch parity holds when sweeping multiple SQs in a
/// row. Catches scenarios where mid-loop state would diverge.
void test_multi_sq_parity() {
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

    auto sieve_n = [&](size_t how_many,
                       const char* env_value) {
        set_env_and_reload(env_value);
        SpecialQGenerator gen(fb, sqr);
        std::vector<size_t> counts;
        counts.reserve(how_many);
        LatticeSieve sieve(ctx, fb, sp);
        sieve.set_region(sr);
        sieve.set_max_threads(1);
        for (size_t i = 0; i < how_many; ++i) {
            auto sq = gen.next();
            if (!sq) break;
            auto result = sieve.sieve_special_q(*sq);
            counts.push_back(result.candidates.size());
        }
        return counts;
    };

    auto off = sieve_n(2, "0");
    auto on = sieve_n(2, "1");

    TEST_ASSERT(off.size() == on.size(),
                "multi-SQ: differing number of SQs processed");
    for (size_t i = 0; i < off.size(); ++i) {
        TEST_ASSERT(off[i] == on[i],
                    "multi-SQ: per-SQ candidate count differs");
    }

    std::cout << "  multi-SQ parity: " << off.size()
              << " SQs processed identically\n";
    TEST_PASS("multi-SQ prefetch parity");
}

/// Correctness 4: 27-bit small N exercises the cold/quick path. Prefetch
/// state should never alter output even when bucket region is not taken.
void test_small_n_parity() {
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
    SpecialQGenerator gen(fb, sqr);
    auto sq = gen.next();
    TEST_ASSERT(sq.has_value(), "small N: no SQ available");

    auto run = [&](const char* env_value) {
        set_env_and_reload(env_value);
        LatticeSieve sieve(ctx, fb, sp);
        sieve.set_region(sr);
        sieve.set_max_threads(1);
        return sieve.sieve_special_q(*sq).candidates;
    };

    auto off = sort_candidates(run("0"));
    auto on = sort_candidates(run("1"));

    TEST_ASSERT(off.size() == on.size(),
                "small N: candidate count differs");
    TEST_ASSERT(candidates_equal(off, on),
                "small N: candidate content differs");

    std::cout << "  27-bit parity: " << off.size()
              << " candidates match bit-for-bit\n";
    TEST_PASS("small N prefetch parity");
}

/// ENV-parse 1: explicit "0" disables the gate.
void test_env_parse_zero() {
    set_env_and_reload("0");
    TEST_ASSERT(!bucket_prefetch_enabled(),
                "GNFS_BUCKET_PREFETCH=0 should disable gate");
    TEST_PASS("env parse: 0 disables");
}

/// ENV-parse 2: "1", "auto", and unset all enable the gate when the
/// compiler supports `__builtin_prefetch`.
void test_env_parse_one_auto_unset() {
    const bool supported = bucket_prefetch_supported();

    set_env_and_reload("1");
    TEST_ASSERT(bucket_prefetch_enabled() == supported,
                "GNFS_BUCKET_PREFETCH=1 should match support state");

    set_env_and_reload("auto");
    TEST_ASSERT(bucket_prefetch_enabled() == supported,
                "GNFS_BUCKET_PREFETCH=auto should match support state");

    set_env_and_reload(nullptr);  // unset
    TEST_ASSERT(bucket_prefetch_enabled() == supported,
                "unset GNFS_BUCKET_PREFETCH should match support state");

    // Unrecognised values default to auto behaviour.
    set_env_and_reload("garbage-value");
    TEST_ASSERT(bucket_prefetch_enabled() == supported,
                "unrecognised value should treat as auto");

    std::cout << "  compiler support: "
              << (supported ? "yes (__builtin_prefetch available)" : "no")
              << "\n";
    TEST_PASS("env parse: 1/auto/unset");
}

/// Perf info (no assert): measure wall-time difference between prefetch
/// ON and OFF on the 40-bit fixture. Printed so PMU sweeps can read it.
void test_perf_info_40bit() {
    const char* perf = std::getenv("GNFS_BUCKET_PREFETCH_PERF");
    if (perf == nullptr || std::strcmp(perf, "1") != 0) {
        std::cout << "  perf info skipped (set GNFS_BUCKET_PREFETCH_PERF=1 to run)\n";
        TEST_PASS("perf info opt-in");
        return;
    }

    auto setup = build_setup_40bit();

    auto timed = [&](const char* env_value) {
        set_env_and_reload(env_value);
        auto t0 = std::chrono::steady_clock::now();
        auto cands = sieve_once(setup, /*force_row_major=*/false);
        auto t1 = std::chrono::steady_clock::now();
        return std::pair<double, size_t>{
            std::chrono::duration<double, std::milli>(t1 - t0).count(),
            cands.size()};
    };

    // Warm both paths first to avoid first-touch / page-fault skew.
    (void)timed("0");
    (void)timed("1");

    auto off = timed("0");
    auto on = timed("1");

    std::cout << "  perf info (40-bit, single SQ): "
              << "prefetch_off=" << off.first << "ms (" << off.second
              << " cands), prefetch_on=" << on.first << "ms (" << on.second
              << " cands)\n";
    // No assertion — prefetch is an optimisation hint, not a guarantee.
    TEST_PASS("perf info (no assert)");
}

int main() {
    std::cout << "=== Bucket prefetch tests ===\n";

    test_bucket_region_parity_40bit();
    test_row_major_parity_40bit();
    test_multi_sq_parity();
    test_small_n_parity();
    test_env_parse_zero();
    test_env_parse_one_auto_unset();
    test_perf_info_40bit();

    // Restore default state so subsequent tests in the same process see
    // the auto behaviour.
    set_env_and_reload(nullptr);

    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    std::cout << "═══════════════════════════════════════════\n";
    return tests_failed == 0 ? 0 : 1;
}
