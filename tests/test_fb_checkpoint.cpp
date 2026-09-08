#include "gnfs/core/polynomial_context.hpp"
#include "gnfs/factor_base/fb_checkpoint.hpp"
#include "gnfs/util/primes.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"
#include "support/test_check.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gnfs::factor_base;
using gnfs::core::AlgebraicPrime;
using gnfs::core::FactorBaseParams;
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

static void write_u64_le(std::ofstream& out, uint64_t value) {
    const unsigned char bytes[8] = {
        static_cast<unsigned char>(value & 0xffULL),
        static_cast<unsigned char>((value >> 8U) & 0xffULL),
        static_cast<unsigned char>((value >> 16U) & 0xffULL),
        static_cast<unsigned char>((value >> 24U) & 0xffULL),
        static_cast<unsigned char>((value >> 32U) & 0xffULL),
        static_cast<unsigned char>((value >> 40U) & 0xffULL),
        static_cast<unsigned char>((value >> 48U) & 0xffULL),
        static_cast<unsigned char>((value >> 56U) & 0xffULL),
    };
    out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

static std::vector<unsigned char> read_bytes(const std::string& path, size_t count) {
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> bytes(count);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    GNFS_TEST_CHECK(in.gcount() == static_cast<std::streamsize>(bytes.size()));
    return bytes;
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

static FbCheckpoint checkpoint_for_context(const PolynomialContext& ctx) {
    FactorBaseParams params(100, 100, 10000, 16);
    FactorBase fb(params);
    return FbCheckpoint::from_factor_base(fb, ctx, /*special_q=*/200);
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
    assert(loaded.ctx_fingerprint_present);
    assert(loaded.ctx_fingerprint_lo == ck2.ctx_fingerprint_lo);
    assert(loaded.ctx_fingerprint_hi == ck2.ctx_fingerprint_hi);

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
    FbCheckpoint ck = checkpoint_for_context(ctx);
    auto status = ck.matches(ctx, 100, 100, 200, 10000, 16);
    assert(status == FbCheckpoint::MatchStatus::Ok);
    std::cout << "  matches() Ok: PASS" << std::endl;
}

void test_matches_mismatch() {
    std::cout << "Testing matches() mismatches..." << std::endl;
    PolynomialContext ctx = make_ctx();
    FbCheckpoint ck = checkpoint_for_context(ctx);

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

    // Same N + degree, but a different m.
    std::vector<Integer> same_coeffs;
    same_coeffs.emplace_back(Integer(static_cast<int64_t>(-5)));
    same_coeffs.emplace_back(Integer(static_cast<int64_t>(3)));
    same_coeffs.emplace_back(Integer(static_cast<int64_t>(1)));
    PolynomialContext other_m(Integer(ctx.n()), std::move(same_coeffs), Integer("10000"),
                              ctx.skewness());
    assert(other_m.degree() == ctx.degree());
    assert(ck.matches(other_m, 100, 100, 200, 10000, 16) ==
           FbCheckpoint::MatchStatus::ContextMismatch);

    // Same N + degree + m, but a different coefficient.
    std::vector<Integer> other_coeffs;
    other_coeffs.emplace_back(Integer(static_cast<int64_t>(-4)));
    other_coeffs.emplace_back(Integer(static_cast<int64_t>(3)));
    other_coeffs.emplace_back(Integer(static_cast<int64_t>(1)));
    PolynomialContext other_poly(Integer(ctx.n()), std::move(other_coeffs), Integer(ctx.m()),
                                 ctx.skewness());
    assert(other_poly.degree() == ctx.degree());
    assert(ck.matches(other_poly, 100, 100, 200, 10000, 16) ==
           FbCheckpoint::MatchStatus::ContextMismatch);

    // Same N + degree + m + coefficients, but a different IEEE-754 skewness bit pattern.
    std::vector<Integer> other_skew_coeffs;
    other_skew_coeffs.emplace_back(Integer(static_cast<int64_t>(-5)));
    other_skew_coeffs.emplace_back(Integer(static_cast<int64_t>(3)));
    other_skew_coeffs.emplace_back(Integer(static_cast<int64_t>(1)));
    PolynomialContext other_skew(Integer(ctx.n()), std::move(other_skew_coeffs), Integer(ctx.m()),
                                 2.0);
    assert(ck.matches(other_skew, 100, 100, 200, 10000, 16) ==
           FbCheckpoint::MatchStatus::ContextMismatch);

    // Params mismatch: change rational_bound
    assert(ck.matches(ctx, 101, 100, 200, 10000, 16) == FbCheckpoint::MatchStatus::ParamsMismatch);
    // Params mismatch: change log_scale
    assert(ck.matches(ctx, 100, 100, 200, 10000, 8) == FbCheckpoint::MatchStatus::ParamsMismatch);
    // Params mismatch: change large_prime_bound
    assert(ck.matches(ctx, 100, 100, 200, 99999, 16) == FbCheckpoint::MatchStatus::ParamsMismatch);
    std::cout << "  matches() mismatches: PASS" << std::endl;
}

void test_legacy_v1_is_readable_but_stale() {
    std::cout << "Testing legacy v1 checkpoint compatibility..." << std::endl;
    auto path = tmp_ckpt_path("legacy_v1");
    CkptCleanup c{path};

    PolynomialContext ctx = make_ctx();
    FbCheckpoint legacy;
    legacy.ctx_n = ctx.n();
    legacy.ctx_degree = ctx.degree();
    legacy.save(path);

    const auto loaded = FbCheckpoint::load(path);
    assert(!loaded.ctx_fingerprint_present);
    assert(loaded.matches(ctx, 0, 0, 0, 0, gnfs::core::SIEVE_LOG_SCALE) ==
           FbCheckpoint::MatchStatus::ContextMismatch);
    std::cout << "  Legacy v1 readable but stale: PASS" << std::endl;
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
    uint32_t p = 2;
    for (uint32_t i = 0; i < 10000; ++i) {
        while (!gnfs::util::is_prime_u32(p))
            ++p;
        orig.rational.emplace_back(p, 16 + (i & 31));
        orig.algebraic.emplace_back(p, (i * 3 + 7) % p, 16 + (i & 31), 1);
        ++p;
    }
    orig.sieve_algebraic_count = 8000;

    orig.save(path);
    auto loaded = FbCheckpoint::load(path);

    assert(loaded.rational.size() == 10000);
    assert(loaded.algebraic.size() == 10000);
    assert(loaded.sieve_algebraic_count == 8000);
    assert(loaded.rational[9999].p == orig.rational[9999].p);
    assert(loaded.algebraic[5000].p == orig.algebraic[5000].p);

    std::cout << "  Large FB: PASS" << std::endl;
}

void test_wire_scalars_are_little_endian() {
    std::cout << "Testing little-endian scalar wire encoding..." << std::endl;
    auto path = tmp_ckpt_path("wire_endian");
    CkptCleanup c{path};

    FbCheckpoint ck;
    ck.rational_bound = 0x01020304U;
    ck.algebraic_bound = 0xa0b0c0d0U;
    ck.special_q_bound = 0x0a0b0c0dU;
    ck.large_prime_bound = 0x0102030405060708ULL;
    ck.log_scale = 0x7e;
    ck.ctx_degree = 0x0e0f1011U;
    ck.ctx_n = Integer(static_cast<int64_t>(-1));
    ck.algebraic.emplace_back(0xa0b0c0d1U, 0xa0b0c0d0U, 0x0a0b0c0dU, 0x7f);
    ck.sieve_algebraic_count = 0x0000000000000001ULL;
    ck.save(path);

    const auto bytes = read_bytes(path, 85);
    const unsigned char expected_magic[8] = {
        0x50, 0x4b, 0x43, 0x46, 0x53, 0x46, 0x4e, 0x47,
    };
    for (size_t i = 0; i < 8; ++i)
        GNFS_TEST_CHECK(bytes[i] == expected_magic[i]);
    GNFS_TEST_CHECK(bytes[8] == 1);
    for (size_t i = 9; i < 16; ++i)
        GNFS_TEST_CHECK(bytes[i] == 0);

    const unsigned char expected_rational_bound[4] = {0x04, 0x03, 0x02, 0x01};
    for (size_t i = 0; i < 4; ++i)
        GNFS_TEST_CHECK(bytes[16 + i] == expected_rational_bound[i]);
    const unsigned char expected_algebraic_bound[4] = {0xd0, 0xc0, 0xb0, 0xa0};
    for (size_t i = 0; i < 4; ++i)
        GNFS_TEST_CHECK(bytes[20 + i] == expected_algebraic_bound[i]);
    const unsigned char expected_special_q_bound[4] = {0x0d, 0x0c, 0x0b, 0x0a};
    for (size_t i = 0; i < 4; ++i)
        GNFS_TEST_CHECK(bytes[24 + i] == expected_special_q_bound[i]);
    const unsigned char expected_large_prime_bound[8] = {
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    };
    for (size_t i = 0; i < 8; ++i)
        GNFS_TEST_CHECK(bytes[28 + i] == expected_large_prime_bound[i]);
    GNFS_TEST_CHECK(bytes[36] == 0x7e);
    for (size_t i = 37; i < 40; ++i)
        GNFS_TEST_CHECK(bytes[i] == 0);

    const unsigned char expected_degree[4] = {0x11, 0x10, 0x0f, 0x0e};
    for (size_t i = 0; i < 4; ++i)
        GNFS_TEST_CHECK(bytes[40 + i] == expected_degree[i]);
    const unsigned char expected_negative_sign[4] = {0xff, 0xff, 0xff, 0xff};
    for (size_t i = 0; i < 4; ++i)
        GNFS_TEST_CHECK(bytes[44 + i] == expected_negative_sign[i]);
    const unsigned char expected_limb_count[4] = {1, 0, 0, 0};
    for (size_t i = 0; i < 4; ++i)
        GNFS_TEST_CHECK(bytes[48 + i] == expected_limb_count[i]);
    GNFS_TEST_CHECK(bytes[52] == 1);
    // The rational count is zero.
    for (size_t i = 53; i < 57; ++i)
        GNFS_TEST_CHECK(bytes[i] == 0);
    GNFS_TEST_CHECK(bytes[57] == 1);
    for (size_t i = 58; i < 61; ++i)
        GNFS_TEST_CHECK(bytes[i] == 0);
    const unsigned char expected_prime[4] = {0xd1, 0xc0, 0xb0, 0xa0};
    for (size_t i = 0; i < 4; ++i)
        GNFS_TEST_CHECK(bytes[61 + i] == expected_prime[i]);
    const unsigned char expected_root[4] = {0xd0, 0xc0, 0xb0, 0xa0};
    for (size_t i = 0; i < 4; ++i)
        GNFS_TEST_CHECK(bytes[65 + i] == expected_root[i]);
    const unsigned char expected_log[4] = {0x0d, 0x0c, 0x0b, 0x0a};
    for (size_t i = 0; i < 4; ++i)
        GNFS_TEST_CHECK(bytes[69 + i] == expected_log[i]);
    GNFS_TEST_CHECK(bytes[73] == 0x7f);
    for (size_t i = 74; i < 77; ++i)
        GNFS_TEST_CHECK(bytes[i] == 0);
    GNFS_TEST_CHECK(bytes[77] == 1);
    for (size_t i = 78; i < 85; ++i)
        GNFS_TEST_CHECK(bytes[i] == 0);
    GNFS_TEST_CHECK(FbCheckpoint::exists_and_valid(path));
    const auto loaded = FbCheckpoint::load(path);
    GNFS_TEST_CHECK(loaded.rational_bound == ck.rational_bound);
    GNFS_TEST_CHECK(loaded.algebraic_bound == ck.algebraic_bound);
    GNFS_TEST_CHECK(loaded.special_q_bound == ck.special_q_bound);
    GNFS_TEST_CHECK(loaded.large_prime_bound == ck.large_prime_bound);
    GNFS_TEST_CHECK(loaded.ctx_degree == ck.ctx_degree);
    GNFS_TEST_CHECK(loaded.ctx_n == ck.ctx_n);
    GNFS_TEST_CHECK(loaded.algebraic.size() == 1);
    GNFS_TEST_CHECK(loaded.algebraic[0].p == ck.algebraic[0].p);
    GNFS_TEST_CHECK(loaded.sieve_algebraic_count == ck.sieve_algebraic_count);
    std::cout << "  Little-endian scalar encoding: PASS" << std::endl;
}

void test_incomplete_magic_rejected() {
    std::cout << "Testing INCOMPLETE magic rejected..." << std::endl;
    auto path = tmp_ckpt_path("incomplete");
    CkptCleanup c{path};
    {
        std::ofstream out(path, std::ios::binary);
        uint64_t magic = FbCheckpoint::MAGIC_INCOMPLETE;
        uint64_t version = FbCheckpoint::VERSION;
        write_u64_le(out, magic);
        write_u64_le(out, version);
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
        write_u64_le(out, magic);
        write_u64_le(out, version);
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

void test_malformed_entries_rejected() {
    std::cout << "Testing malformed checkpoint entries rejected..." << std::endl;
    const auto expect_rejected = [](FbCheckpoint& ck, const char* label) {
        const auto path = tmp_ckpt_path(label);
        CkptCleanup cleanup{path};
        ck.ctx_degree = 1;
        ck.ctx_n = Integer(static_cast<int64_t>(5));
        ck.save(path);

        bool threw = false;
        try {
            (void)FbCheckpoint::load(path);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw)
            throw std::runtime_error(std::string("malformed checkpoint accepted: ") + label);
    };

    FbCheckpoint zero_rational;
    zero_rational.rational.emplace_back(0, 1);
    expect_rejected(zero_rational, "bad_zero_rational");

    FbCheckpoint composite_rational;
    composite_rational.rational.emplace_back(9, 1);
    expect_rejected(composite_rational, "bad_composite_rational");

    FbCheckpoint root_out_of_range;
    root_out_of_range.algebraic.emplace_back(3, 3, 1, 1);
    expect_rejected(root_out_of_range, "bad_algebraic_root");

    FbCheckpoint zero_degree;
    zero_degree.algebraic.emplace_back(3, 1, 1, 0);
    expect_rejected(zero_degree, "bad_algebraic_degree");

    std::cout << "  Malformed checkpoint entries: PASS" << std::endl;
}

void test_truncated_payload_rejected_before_allocation() {
    std::cout << "Testing truncated payload preflight..." << std::endl;
    const auto patch_u32 = [](const std::string& path, std::streamoff offset, uint32_t value) {
        std::fstream io(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!io)
            throw std::runtime_error("cannot open checkpoint for patching");
        io.seekp(offset);
        const unsigned char bytes[4] = {
            static_cast<unsigned char>(value & 0xffU),
            static_cast<unsigned char>((value >> 8U) & 0xffU),
            static_cast<unsigned char>((value >> 16U) & 0xffU),
            static_cast<unsigned char>((value >> 24U) & 0xffU),
        };
        io.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
        if (!io)
            throw std::runtime_error("cannot patch checkpoint");
    };
    const auto patch_u64 = [](const std::string& path, std::streamoff offset, uint64_t value) {
        std::fstream io(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!io)
            throw std::runtime_error("cannot open checkpoint for patching");
        io.seekp(offset);
        const unsigned char bytes[8] = {
            static_cast<unsigned char>(value & 0xffULL),
            static_cast<unsigned char>((value >> 8U) & 0xffULL),
            static_cast<unsigned char>((value >> 16U) & 0xffULL),
            static_cast<unsigned char>((value >> 24U) & 0xffULL),
            static_cast<unsigned char>((value >> 32U) & 0xffULL),
            static_cast<unsigned char>((value >> 40U) & 0xffULL),
            static_cast<unsigned char>((value >> 48U) & 0xffULL),
            static_cast<unsigned char>((value >> 56U) & 0xffULL),
        };
        io.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
        if (!io)
            throw std::runtime_error("cannot patch checkpoint");
    };
    const auto expect_rejected = [](const std::string& path) {
        bool threw = false;
        try {
            (void)FbCheckpoint::load(path);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw)
            throw std::runtime_error("truncated checkpoint was accepted");
    };
    const auto resize_file = [](const std::string& path, std::uintmax_t size) {
        std::error_code error;
        std::filesystem::resize_file(path, size, error);
        if (error)
            throw std::runtime_error("cannot resize checkpoint: " + error.message());
    };

    // With N=5, the rational count starts at byte 53 in the v1 wire format.
    {
        const auto path = tmp_ckpt_path("huge_count");
        CkptCleanup cleanup{path};
        FbCheckpoint ck;
        ck.ctx_degree = 1;
        ck.ctx_n = Integer(static_cast<int64_t>(5));
        ck.save(path);
        patch_u32(path, 53, 100'000'000U);
        expect_rejected(path);
    }

    // A large integer body must be checked against the file before allocation.
    {
        const auto path = tmp_ckpt_path("huge_integer");
        CkptCleanup cleanup{path};
        FbCheckpoint ck;
        ck.ctx_degree = 1;
        ck.ctx_n = Integer(static_cast<int64_t>(5));
        ck.save(path);
        patch_u32(path, 48, 64U * 1024U * 1024U);
        expect_rejected(path);
    }

    // A large integer body ending exactly at EOF must still leave the fixed
    // rational/algebraic/sieve trailer available before allocation.
    {
        const auto path = tmp_ckpt_path("integer_body_at_eof");
        CkptCleanup cleanup{path};
        FbCheckpoint ck;
        ck.ctx_degree = 1;
        ck.ctx_n = Integer(static_cast<int64_t>(5));
        ck.save(path);
        constexpr uint32_t body_bytes = 64U * 1024U * 1024U;
        constexpr std::uintmax_t integer_body_offset = 52;
        resize_file(path, integer_body_offset + body_bytes);
        patch_u32(path, 48, body_bytes);
        expect_rejected(path);
    }

    // A zero sign must not carry a non-zero body length.
    {
        const auto path = tmp_ckpt_path("noncanonical_zero");
        CkptCleanup cleanup{path};
        FbCheckpoint ck;
        ck.ctx_degree = 1;
        ck.ctx_n = Integer(static_cast<int64_t>(5));
        ck.save(path);
        patch_u32(path, 44, 0);
        expect_rejected(path);
    }

    // The algebraic degree is stored in a padded u32 and must not truncate.
    {
        const auto path = tmp_ckpt_path("degree_high_bits");
        CkptCleanup cleanup{path};
        FbCheckpoint ck;
        ck.ctx_degree = 1;
        ck.ctx_n = Integer(static_cast<int64_t>(5));
        ck.algebraic.emplace_back(3, 1, 1, 1);
        ck.save(path);
        patch_u32(path, 73, 257);
        expect_rejected(path);
    }

    // The sieve prefix is a bounded view into the decoded algebraic entries.
    {
        const auto path = tmp_ckpt_path("sieve_count_overrun");
        CkptCleanup cleanup{path};
        FbCheckpoint ck;
        ck.ctx_degree = 1;
        ck.ctx_n = Integer(static_cast<int64_t>(5));
        ck.save(path);
        patch_u64(path, 61, 1);
        expect_rejected(path);
    }

    std::cout << "  Truncated payload preflight: PASS" << std::endl;
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
    test_legacy_v1_is_readable_but_stale();
    test_empty_fb();
    test_sieve_count_invariants();
    test_large_fb();
    test_wire_scalars_are_little_endian();
    test_incomplete_magic_rejected();
    test_version_mismatch_rejected();
    test_malformed_entries_rejected();
    test_truncated_payload_rejected_before_allocation();
    test_remove_and_nonexistent();
    std::cout << "\n===== All FbCheckpoint tests PASSED =====" << std::endl;
    return 0;
}
