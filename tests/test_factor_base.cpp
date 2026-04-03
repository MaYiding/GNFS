#include "gnfs/factor_base/builder.hpp"
#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/sqrt/modular_poly.hpp"

#include <cassert>
#include <iostream>
#include <sstream>

using namespace gnfs;
using namespace gnfs::factor_base;
using namespace gnfs::polynomial;
using namespace gnfs::core;

// 测试用的半素数: 1000003 * 1000033 = 1000036000099
const char* test_n = "1000036000099";

void test_prime_sieve() {
    std::cout << "Testing prime sieve..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);

    auto ctx = BaseMSelector::create_context(n, result);

    // 小范围测试
    FactorBaseBuilder::Options opts;
    opts.rational_bound = 100;
    opts.algebraic_bound = 100;
    opts.parallel = false;

    auto fb = FactorBaseBuilder::build(ctx, opts);

    // 验证有理因子基
    assert(fb.rational_count() > 0);
    // 100 以内有 25 个素数
    assert(fb.rational_count() == 25);

    // 验证第一个素数是 2
    auto rationals = fb.rational();
    assert(rationals[0].p == 2);

    // 验证最后一个素数是 97
    assert(rationals[24].p == 97);

    std::cout << "  Prime sieve: PASS" << std::endl;
}

void test_algebraic_roots() {
    std::cout << "Testing algebraic roots..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);

    auto ctx = BaseMSelector::create_context(n, result);

    FactorBaseBuilder::Options opts;
    opts.rational_bound = 50;
    opts.algebraic_bound = 50;
    opts.parallel = false;

    auto fb = FactorBaseBuilder::build(ctx, opts);

    // 代数侧应该有一些素理想
    assert(fb.algebraic_count() > 0);

    // 每个代数素理想 (p, r) 应该满足 f(r) ≡ 0 (mod p)
    auto algebraics = fb.algebraic();
    for (const auto& ap : algebraics) {
        uint64_t fr = ctx.evaluate_mod(ap.r, ap.p);
        assert(fr == 0);  // f(r) mod p should be 0
    }

    std::cout << "  Algebraic roots: PASS (" << fb.algebraic_count() << " primes)" << std::endl;
}

void test_index_lookup() {
    std::cout << "Testing index lookup..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);

    auto ctx = BaseMSelector::create_context(n, result);

    FactorBaseBuilder::Options opts;
    opts.rational_bound = 100;
    opts.algebraic_bound = 100;
    opts.parallel = false;

    auto fb = FactorBaseBuilder::build(ctx, opts);

    // 测试有理侧查找
    auto idx = fb.find_rational(7);
    assert(idx.has_value());
    assert(fb.rational()[*idx].p == 7);

    // 查找不存在的素数
    auto not_found = fb.find_rational(6);
    assert(!not_found.has_value());

    // 测试代数侧查找
    auto algebraics = fb.algebraic();
    if (!algebraics.empty()) {
        const auto& first = algebraics[0];
        auto alg_idx = fb.find_algebraic(first.p, first.r);
        assert(alg_idx.has_value());
        assert(*alg_idx == 0);
    }

    std::cout << "  Index lookup: PASS" << std::endl;
}

void test_log_values() {
    std::cout << "Testing log values..." << std::endl;

    uint8_t scale = 16;

    // log2(2) * 16 = 16
    assert(compute_log_prime(2, scale) == 16);

    // log2(4) * 16 = 32
    assert(compute_log_prime(4, scale) == 32);

    // log2(1024) * 16 = 160
    assert(compute_log_prime(1024, scale) == 160);

    std::cout << "  Log values: PASS" << std::endl;
}

void test_parallel_build() {
    std::cout << "Testing parallel build..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);

    auto ctx = BaseMSelector::create_context(n, result);

    // 构建两个因子基：顺序和并行
    FactorBaseBuilder::Options opts_seq;
    opts_seq.rational_bound = 10000;
    opts_seq.algebraic_bound = 10000;
    opts_seq.parallel = false;

    FactorBaseBuilder::Options opts_par;
    opts_par.rational_bound = 10000;
    opts_par.algebraic_bound = 10000;
    opts_par.parallel = true;

    auto fb_seq = FactorBaseBuilder::build(ctx, opts_seq);
    auto fb_par = FactorBaseBuilder::build(ctx, opts_par);

    // 两者应该有相同数量的元素
    assert(fb_seq.rational_count() == fb_par.rational_count());
    assert(fb_seq.algebraic_count() == fb_par.algebraic_count());

    // 验证内容一致性
    auto rat_seq = fb_seq.rational();
    auto rat_par = fb_par.rational();
    for (size_t i = 0; i < rat_seq.size(); ++i) {
        assert(rat_seq[i].p == rat_par[i].p);
        assert(rat_seq[i].log_p == rat_par[i].log_p);
    }

    std::cout << "  Parallel build: PASS (seq=" << fb_seq.rational_count()
              << ", par=" << fb_par.rational_count() << " rationals)" << std::endl;
}

void test_larger_bound() {
    std::cout << "Testing larger bound (100k)..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);

    auto ctx = BaseMSelector::create_context(n, result);

    FactorBaseBuilder::Options opts;
    opts.rational_bound = 100000;
    opts.algebraic_bound = 100000;
    opts.parallel = true;

    auto fb = FactorBaseBuilder::build(ctx, opts);

    // 100000 以内约有 9592 个素数
    assert(fb.rational_count() > 9000);
    assert(fb.rational_count() < 10000);

    std::cout << "  Larger bound: PASS (" << fb.rational_count() << " rational, "
              << fb.algebraic_count() << " algebraic)" << std::endl;
}

/// Regression test: base-m irreducibility check.
/// N = 1320, d = 3: m_base = 10 gives f(x) = x³ + 3x² + 2x = x(x+1)(x+2) — reducible!
/// The fix should select a different m (e.g., m=9 → f = x³ + 7x² + 2x + 6, irreducible).
void test_base_m_irreducibility() {
    std::cout << "Testing base-m irreducibility check..." << std::endl;

    // --- Case 1: N where m_base gives a reducible polynomial ---
    // N=1320, d=3, m_base=10 → f = x³+3x²+2x = x(x+1)(x+2) — reducible!
    {
        Integer n("1320");
        auto result = BaseMSelector::select(n, 3);
        assert(result.success);
        assert(result.f.degree() == 3);

        // Verify f(m) = N
        Integer fm = result.f.evaluate(result.m);
        assert(fm == n);

        // m should NOT be 10 (the base value with reducible polynomial)
        assert(result.m != Integer(10));

        // Verify the selected polynomial is irreducible over Q:
        // For degree 3, no rational roots ⟹ irreducible.
        // Rational roots must divide the constant term.
        Integer c0 = result.f[0].clone();
        if (c0.is_zero()) {
            // If constant term is 0, x=0 is a root → reducible → should not happen
            assert(false && "selected polynomial has root 0");
        }
        // Check small integer roots: ±1, ±2, ±3, ±6 (divisors up to |c0|)
        for (int r : {1, -1, 2, -2, 3, -3, 6, -6}) {
            Integer val = result.f.evaluate(Integer(r));
            assert(!val.is_zero());  // no rational roots allowed
        }

        // Verify irreducibility via mod-p test for at least one prime
        bool found_irred = false;
        uint32_t d = result.f.degree();
        for (uint64_t p : {uint64_t(2), uint64_t(3), uint64_t(5), uint64_t(7),
                           uint64_t(11), uint64_t(13)}) {
            std::vector<uint64_t> f_mod(d + 1);
            for (uint32_t i = 0; i <= d; ++i) {
                Integer c = result.f[i] % Integer(p);
                if (c.is_negative()) c += Integer(p);
                f_mod[i] = c.to_uint64();
            }
            if (f_mod[d] == 0) continue;
            if (gnfs::sqrt::ModularPoly::is_irreducible(f_mod, p)) {
                found_irred = true;
                break;
            }
        }
        assert(found_irred);

        std::cout << "  N=1320 reducible case: PASS (m=" << result.m.to_string() << ")" << std::endl;
    }

    // --- Case 2: standard semiprimes still work ---
    {
        Integer n("10403");  // 101 × 103
        auto result = BaseMSelector::select(n, 3);
        assert(result.success);
        assert(result.f.degree() == 3);
        Integer fm = result.f.evaluate(result.m);
        assert(fm == n);
        std::cout << "  N=10403 normal case: PASS" << std::endl;
    }

    // --- Case 3: larger N ---
    {
        Integer n(test_n);  // 1000036000099 = 1000003 × 1000033
        auto result = BaseMSelector::select(n, 3);
        assert(result.success);
        assert(result.f.degree() == 3);
        Integer fm = result.f.evaluate(result.m);
        assert(fm == n);
        std::cout << "  N=1000036000099 normal case: PASS" << std::endl;
    }

    std::cout << "  ALL base-m irreducibility tests PASSED" << std::endl;
}

void test_serialization_roundtrip() {
    std::cout << "Testing serialization roundtrip..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);
    auto ctx = BaseMSelector::create_context(n, result);

    FactorBaseBuilder::Options opts;
    opts.rational_bound = 1000;
    opts.algebraic_bound = 2000;
    opts.special_q_bound = 3000;
    opts.parallel = false;

    auto fb = FactorBaseBuilder::build(ctx, opts);

    // Save to stream
    std::stringstream ss;
    fb.save(ss);

    // Load back
    ss.seekg(0);
    auto fb2 = FactorBase::load(ss);

    // Verify params
    assert(fb2.params().rational_bound == fb.params().rational_bound);
    assert(fb2.params().algebraic_bound == fb.params().algebraic_bound);
    assert(fb2.params().large_prime_bound == fb.params().large_prime_bound);
    assert(fb2.params().log_scale == fb.params().log_scale);

    // Verify counts
    assert(fb2.rational_count() == fb.rational_count());
    assert(fb2.algebraic_count() == fb.algebraic_count());
    assert(fb2.sieve_algebraic_count() == fb.sieve_algebraic_count());

    // Verify rational primes content
    auto rat1 = fb.rational();
    auto rat2 = fb2.rational();
    for (size_t i = 0; i < rat1.size(); ++i) {
        assert(rat1[i].p == rat2[i].p);
        assert(rat1[i].log_p == rat2[i].log_p);
    }

    // Verify algebraic primes content
    auto alg1 = fb.algebraic();
    auto alg2 = fb2.algebraic();
    for (size_t i = 0; i < alg1.size(); ++i) {
        assert(alg1[i].p == alg2[i].p);
        assert(alg1[i].r == alg2[i].r);
        assert(alg1[i].log_p == alg2[i].log_p);
        assert(alg1[i].degree == alg2[i].degree);
    }

    // Verify index lookup works after load
    for (const auto& rp : rat1) {
        auto idx = fb2.find_rational(rp.p);
        assert(idx.has_value());
        assert(fb2.rational()[*idx].p == rp.p);
    }

    std::cout << "  Serialization roundtrip: PASS ("
              << fb.rational_count() << " rat, "
              << fb.algebraic_count() << " alg, "
              << "sieve_alg=" << fb.sieve_algebraic_count() << ")" << std::endl;
}

void test_serialization_invalid() {
    std::cout << "Testing serialization error handling..." << std::endl;

    // Bad magic
    {
        std::stringstream ss;
        uint32_t bad_magic = 0xDEADBEEF;
        ss.write(reinterpret_cast<const char*>(&bad_magic), sizeof(bad_magic));
        bool caught = false;
        try { FactorBase::load(ss); }
        catch (const std::runtime_error&) { caught = true; }
        assert(caught);
    }

    // Bad version
    {
        std::stringstream ss;
        uint32_t magic = 0x47464246;
        uint32_t bad_version = 99;
        ss.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        ss.write(reinterpret_cast<const char*>(&bad_version), sizeof(bad_version));
        bool caught = false;
        try { FactorBase::load(ss); }
        catch (const std::runtime_error&) { caught = true; }
        assert(caught);
    }

    std::cout << "  Serialization error handling: PASS" << std::endl;
}

int main() {
    std::cout << "=== Factor Base Tests ===" << std::endl;

    test_prime_sieve();
    test_algebraic_roots();
    test_index_lookup();
    test_log_values();
    test_parallel_build();
    test_larger_bound();
    test_base_m_irreducibility();
    test_serialization_roundtrip();
    test_serialization_invalid();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
