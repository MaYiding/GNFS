#include "gnfs/core/polynomial_context.hpp"
#include "gnfs/factor_base/fb_checkpoint.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace gnfs::factor_base;
using gnfs::core::AlgebraicPrime;
using gnfs::core::Integer;
using gnfs::core::PolynomialContext;
using gnfs::core::RationalPrime;

static std::string tmp_ckpt_path(const char* label) {
    static int seq = 0;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "gnfs_test_fb_ckpt_%d_%d_%s", gnfs::util::process_id(), ++seq,
                  label);
    return gnfs::util::temp_path(buf);
}

struct CkptCleanup {
    std::string path;
    ~CkptCleanup() {
        if (!path.empty())
            std::remove(path.c_str());
    }
};

static PolynomialContext make_ctx() {
    std::vector<Integer> coeffs;
    coeffs.emplace_back(Integer(static_cast<int64_t>(-5)));
    coeffs.emplace_back(Integer(static_cast<int64_t>(3)));
    coeffs.emplace_back(Integer(static_cast<int64_t>(1)));
    return PolynomialContext(Integer("123456789012345"), std::move(coeffs), Integer("9999"), 1.0);
}

void test_roundtrip_small_fb() {
    std::cout << "Testing FB roundtrip (small synthetic)..." << std::endl;
    auto path = tmp_ckpt_path("rt_small");
    CkptCleanup c{path};

    FbCheckpoint orig;
    orig.rational_bound = 1000;
    orig.algebraic_bound = 1500;
    orig.special_q_bound = 5000;
    orig.large_prime_bound = 100000;
    orig.log_scale = 16;
    orig.ctx_degree = 4;
    orig.ctx_n = Integer("314159265358979323846");

    orig.rational = {
        RationalPrime(2, 16), RationalPrime(3, 25),  RationalPrime(5, 37),
        RationalPrime(7, 45), RationalPrime(11, 55),
    };
    orig.algebraic = {
        AlgebraicPrime(2, 1, 16, 1),
        AlgebraicPrime(3, 2, 25, 1),
        AlgebraicPrime(5, AlgebraicPrime::PROJECTIVE_ROOT, 37, 2),
        AlgebraicPrime(7, 4, 45, 1),
    };
    orig.sieve_algebraic_count = 3; // First 3 are sieve range

    orig.save(path);
    auto loaded = FbCheckpoint::load(path);

    assert(loaded.rational_bound == 1000);
    assert(loaded.algebraic_bound == 1500);
    assert(loaded.special_q_bound == 5000);
    assert(loaded.large_prime_bound == 100000);
    assert(loaded.log_scale == 16);
    assert(loaded.ctx_degree == 4);
    assert(loaded.ctx_n == Integer("314159265358979323846"));

    assert(loaded.rational.size() == 5);
    assert(loaded.rational[0].p == 2 && loaded.rational[0].log_p == 16);
    assert(loaded.rational[4].p == 11 && loaded.rational[4].log_p == 55);

    assert(loaded.algebraic.size() == 4);
    assert(loaded.algebraic[0].p == 2 && loaded.algebraic[0].r == 1);
    assert(loaded.algebraic[2].is_projective());
    assert(loaded.algebraic[2].degree == 2);
    assert(loaded.algebraic[3].p == 7 && loaded.algebraic[3].r == 4);
    assert(loaded.sieve_algebraic_count == 3);

    std::cout << "  Small FB roundtrip: PASS" << std::endl;
}

void test_to_from_factor_base() {
    std::cout << "Testing to/from FactorBase..." << std::endl;
    auto path = tmp_ckpt_path("fb_rt");
    CkptCleanup c{path};

    FbCheckpoint ck;
    ck.rational_bound = 100;
    ck.algebraic_bound = 200;
    ck.special_q_bound = 1000;
    ck.large_prime_bound = 5000;
    ck.log_scale = 16;
    ck.ctx_degree = 2;
    ck.ctx_n = Integer("999983");
    ck.rational = {
        RationalPrime(13, 50),
        RationalPrime(17, 60),
    };
    ck.algebraic = {
        AlgebraicPrime(13, 5, 50, 1),
    };
    ck.sieve_algebraic_count = 1;

    FactorBase fb = ck.to_factor_base();
    assert(fb.rational_count() == 2);
    assert(fb.algebraic_count() == 1);
    assert(fb.sieve_algebraic_count() == 1);
    auto idx = fb.find_rational(17);
    assert(idx.has_value() && *idx == 1);
    auto aidx = fb.find_algebraic(13, 5);
    assert(aidx.has_value() && *aidx == 0);

    // Roundtrip back via from_factor_base + a context
    PolynomialContext ctx = make_ctx();
    auto ck2 = FbCheckpoint::from_factor_base(fb, ctx, /*special_q=*/2000);
    assert(ck2.rational_bound == ck.rational_bound);
    assert(ck2.algebraic_bound == ck.algebraic_bound);
    assert(ck2.special_q_bound == 2000);
    assert(ck2.rational.size() == 2);
    assert(ck2.algebraic.size() == 1);
    assert(ck2.ctx_n == ctx.n());
    assert(ck2.ctx_degree == ctx.degree());

    ck2.save(path);
    auto loaded = FbCheckpoint::load(path);
    FactorBase fb2 = loaded.to_factor_base();
    assert(fb2.rational_count() == 2);
    assert(fb2.algebraic_count() == 1);

    std::cout << "  FactorBase roundtrip: PASS" << std::endl;
}

void test_explicit_zero_sieve_count() {
    std::cout << "Testing explicit zero sieve count roundtrip..." << std::endl;
    const auto require = [](bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    };
    auto path = tmp_ckpt_path("explicit_zero");
    CkptCleanup c{path};

    FactorBase fb;
    fb.add_algebraic(3, 0, 25, 1);
    fb.set_sieve_algebraic_count_explicit(0);
    assert(fb.sieve_algebraic_count() == 0);

    PolynomialContext ctx = make_ctx();
    auto ck = FbCheckpoint::from_factor_base(fb, ctx, /*special_q=*/100);
    require(ck.sieve_algebraic_count == 0, "checkpoint changed the explicit zero count");
    require(ck.sieve_algebraic_count_explicit,
            "checkpoint lost the explicit zero state before serialization");
    ck.save(path);

    auto loaded = FbCheckpoint::load(path);
    FactorBase restored = loaded.to_factor_base();
    require(restored.algebraic_count() == 1, "checkpoint roundtrip changed algebraic entry count");
    require(restored.sieve_algebraic_count() == 0,
            "checkpoint roundtrip lost explicit empty sieve prefix");

    std::cout << "  Explicit zero sieve count: PASS" << std::endl;
}

void test_matches_ok() {
    std::cout << "Testing matches() Ok..." << std::endl;
    PolynomialContext ctx = make_ctx();
    FbCheckpoint ck;
    ck.rational_bound = 100;
    ck.algebraic_bound = 100;
    ck.special_q_bound = 200;
    ck.large_prime_bound = 10000;
    ck.log_scale = 16;
    ck.ctx_n = ctx.n();
    ck.ctx_degree = ctx.degree();
    auto status = ck.matches(ctx, 100, 100, 200, 10000, 16);
    assert(status == FbCheckpoint::MatchStatus::Ok);
    std::cout << "  matches() Ok: PASS" << std::endl;
}

void test_matches_mismatch() {
    std::cout << "Testing matches() mismatches..." << std::endl;
    PolynomialContext ctx = make_ctx();
    FbCheckpoint ck;
    ck.rational_bound = 100;
    ck.algebraic_bound = 100;
    ck.special_q_bound = 200;
    ck.large_prime_bound = 10000;
    ck.log_scale = 16;
    ck.ctx_n = ctx.n();
    ck.ctx_degree = ctx.degree();

    // N mismatch
    std::vector<Integer> ccs;
    ccs.emplace_back(Integer(static_cast<int64_t>(0)));
    ccs.emplace_back(Integer(static_cast<int64_t>(1)));
    PolynomialContext other_n(Integer("99991"), std::move(ccs), Integer(static_cast<int64_t>(0)),
                              1.0);
    assert(ck.matches(other_n, 100, 100, 200, 10000, 16) == FbCheckpoint::MatchStatus::NMismatch);

    // Degree mismatch (synthesize new ctx with same N but degree 1)
    std::vector<Integer> ccs2;
    ccs2.emplace_back(Integer(static_cast<int64_t>(0)));
    ccs2.emplace_back(Integer(static_cast<int64_t>(1)));
    PolynomialContext other_deg(Integer(ctx.n()), std::move(ccs2), Integer(ctx.m()),
                                ctx.skewness());
    assert(other_deg.degree() == 1);
    assert(ck.matches(other_deg, 100, 100, 200, 10000, 16) ==
           FbCheckpoint::MatchStatus::DegreeMismatch);

    // Params mismatch: change rational_bound
    assert(ck.matches(ctx, 101, 100, 200, 10000, 16) == FbCheckpoint::MatchStatus::ParamsMismatch);
    // Params mismatch: change log_scale
    assert(ck.matches(ctx, 100, 100, 200, 10000, 8) == FbCheckpoint::MatchStatus::ParamsMismatch);
    // Params mismatch: change large_prime_bound
    assert(ck.matches(ctx, 100, 100, 200, 99999, 16) == FbCheckpoint::MatchStatus::ParamsMismatch);
    std::cout << "  matches() mismatches: PASS" << std::endl;
}

void test_empty_fb() {
    std::cout << "Testing empty FB..." << std::endl;
    auto path = tmp_ckpt_path("empty");
    CkptCleanup c{path};

    FbCheckpoint ck;
    ck.rational_bound = 0;
    ck.ctx_n = Integer(static_cast<int64_t>(2));
    ck.ctx_degree = 1;
    ck.save(path);
    auto loaded = FbCheckpoint::load(path);
    assert(loaded.rational.empty());
    assert(loaded.algebraic.empty());
    assert(loaded.sieve_algebraic_count == 0);
    std::cout << "  Empty FB: PASS" << std::endl;
}

void test_sieve_count_invariants() {
    std::cout << "Testing checkpoint sieve-count invariants..." << std::endl;

    FbCheckpoint ck;
    ck.ctx_n = Integer(static_cast<int64_t>(7));
    ck.ctx_degree = 1;
    ck.algebraic.emplace_back(3, 1, 16, 1);
    ck.sieve_algebraic_count = 2;

    bool save_threw = false;
    try {
        ck.save(tmp_ckpt_path("invalid_sieve_save"));
    } catch (const std::runtime_error&) {
        save_threw = true;
    }
    assert(save_threw);

    bool rebuild_threw = false;
    try {
        (void)ck.to_factor_base();
    } catch (const std::runtime_error&) {
        rebuild_threw = true;
    }
    assert(rebuild_threw);

    std::cout << "  Checkpoint sieve-count invariants: PASS" << std::endl;
}

void test_large_fb() {
    std::cout << "Testing large FB (10K primes)..." << std::endl;
    auto path = tmp_ckpt_path("large");
    CkptCleanup c{path};

    FbCheckpoint orig;
    orig.rational_bound = 100000;
    orig.algebraic_bound = 100000;
    orig.large_prime_bound = 1000000;
    orig.log_scale = 16;
    orig.ctx_degree = 5;
    orig.ctx_n = Integer("12345678901234567890");
    orig.rational.reserve(10000);
    orig.algebraic.reserve(10000);
    for (uint32_t i = 0; i < 10000; ++i) {
        orig.rational.emplace_back(2 + i, 16 + (i & 31));
        orig.algebraic.emplace_back(2 + i, (i * 3 + 7) % (2 + i), 16 + (i & 31), 1);
    }
    orig.sieve_algebraic_count = 8000;

    orig.save(path);
    auto loaded = FbCheckpoint::load(path);

    assert(loaded.rational.size() == 10000);
    assert(loaded.algebraic.size() == 10000);
    assert(loaded.sieve_algebraic_count == 8000);
    assert(loaded.rational[9999].p == 10001);
    assert(loaded.algebraic[5000].p == 5002);

    std::cout << "  Large FB: PASS" << std::endl;
}

void test_incomplete_magic_rejected() {
    std::cout << "Testing INCOMPLETE magic rejected..." << std::endl;
    auto path = tmp_ckpt_path("incomplete");
    CkptCleanup c{path};
    {
        std::ofstream out(path, std::ios::binary);
        uint64_t magic = FbCheckpoint::MAGIC_INCOMPLETE;
        uint64_t version = FbCheckpoint::VERSION;
        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.write(reinterpret_cast<const char*>(&version), 8);
    }
    assert(!FbCheckpoint::exists_and_valid(path));
    bool threw = false;
    try {
        (void)FbCheckpoint::load(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    std::cout << "  INCOMPLETE rejected: PASS" << std::endl;
}

void test_version_mismatch_rejected() {
    std::cout << "Testing version mismatch rejected..." << std::endl;
    auto path = tmp_ckpt_path("ver_bad");
    CkptCleanup c{path};
    {
        std::ofstream out(path, std::ios::binary);
        uint64_t magic = FbCheckpoint::MAGIC;
        uint64_t version = 999;
        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.write(reinterpret_cast<const char*>(&version), 8);
    }
    bool threw = false;
    try {
        (void)FbCheckpoint::load(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    std::cout << "  Version mismatch: PASS" << std::endl;
}

void test_remove_and_nonexistent() {
    std::cout << "Testing remove + nonexistent..." << std::endl;
    auto path = tmp_ckpt_path("rm");
    FbCheckpoint ck;
    ck.ctx_n = Integer(static_cast<int64_t>(5));
    ck.save(path);
    assert(FbCheckpoint::exists_and_valid(path));
    FbCheckpoint::remove(path);
    assert(!FbCheckpoint::exists_and_valid(path));

    bool threw = false;
    try {
        (void)FbCheckpoint::load(gnfs::util::temp_path("nonexistent_fbck_xx_99999"));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    std::cout << "  Remove + nonexistent: PASS" << std::endl;
}

int main() {
    std::cout << "===== FbCheckpoint Tests =====" << std::endl;
    test_roundtrip_small_fb();
    test_to_from_factor_base();
    test_explicit_zero_sieve_count();
    test_matches_ok();
    test_matches_mismatch();
    test_empty_fb();
    test_sieve_count_invariants();
    test_large_fb();
    test_incomplete_magic_rejected();
    test_version_mismatch_rejected();
    test_remove_and_nonexistent();
    std::cout << "\n===== All FbCheckpoint tests PASSED =====" << std::endl;
    return 0;
}
