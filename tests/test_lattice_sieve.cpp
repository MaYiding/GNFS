#include "gnfs/factor_base/builder.hpp"
#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/sieve/lattice_sieve.hpp"
#include "gnfs/util/safe_math.hpp"
#include "support/test_check.hpp"

#include <cassert>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

using namespace gnfs;
using namespace gnfs::sieve;
using namespace gnfs::factor_base;
using namespace gnfs::polynomial;
using namespace gnfs::core;

// 测试用的半素数
const char* test_n = "1000036000099";

void test_lattice_basis() {
    std::cout << "Testing lattice basis computation..." << std::endl;

    SpecialQ sq;
    sq.q = 1009; // 素数
    sq.r = 42;   // 某个根

    LatticeBasis basis = compute_lattice_basis(sq);

    // 验证基向量满足格条件 (GNFS convention: a - b*r ≡ 0 mod q)
    // v0 = (e0, f0) 应该满足 e0 - f0*r ≡ 0 (mod q)
    int64_t check0 = basis.e0 - static_cast<int64_t>(basis.f0) * sq.r;
    int64_t mod0 = check0 % static_cast<int64_t>(sq.q);
    if (mod0 < 0)
        mod0 += sq.q;
    assert(mod0 == 0);

    // v1 = (e1, f1) 应该满足 e1 - f1*r ≡ 0 (mod q)
    int64_t check1 = basis.e1 - static_cast<int64_t>(basis.f1) * sq.r;
    int64_t mod1 = check1 % static_cast<int64_t>(sq.q);
    if (mod1 < 0)
        mod1 += sq.q;
    assert(mod1 == 0);

    // 行列式应该等于 q（或 -q）
    int64_t det = basis.determinant();
    assert(std::abs(det) == static_cast<int64_t>(sq.q));

    // 测试 to_ab
    auto [a, b] = basis.to_ab(1, 2);
    int64_t expected_a = 1 * basis.e0 + 2 * basis.e1;
    int64_t expected_b = 1 * basis.f0 + 2 * basis.f1;
    assert(a == expected_a);
    assert(b == expected_b);

    // verify_ab 应该返回 true
    assert(basis.verify_ab(a, b));

    std::cout << "  Lattice basis: PASS (det=" << det << ")" << std::endl;
}

void test_sieve_region() {
    std::cout << "Testing sieve region..." << std::endl;

    SieveRegion region;
    region.i_min = -100;
    region.i_max = 99;
    region.j_min = 1;
    region.j_max = 50;

    // 测试尺寸
    assert(region.i_width() == 200);
    assert(region.j_height() == 50);
    assert(region.size() == 200 * 50);

    // 测试坐标转换
    size_t idx = region.ij_to_index(0, 1);
    auto [i, j] = region.index_to_ij(idx);
    assert(i == 0);
    assert(j == 1);

    // 测试边界
    size_t idx_min = region.ij_to_index(region.i_min, region.j_min);
    assert(idx_min == 0);

    size_t idx_max = region.ij_to_index(region.i_max, region.j_max);
    assert(idx_max == region.size() - 1);

    std::cout << "  Sieve region: PASS" << std::endl;
}

void test_sieve_region_extreme_bounds() {
    std::cout << "Testing sieve region extreme bounds..." << std::endl;

    const auto reversed_i = SieveRegion{10, 9, 1, 1};
    const std::pair<int32_t, int32_t> reversed_sentinel{10, 1};
    GNFS_TEST_CHECK(reversed_i.i_width() == 0);
    GNFS_TEST_CHECK(reversed_i.size() == 0);
    GNFS_TEST_CHECK(reversed_i.index_to_ij(0) == reversed_sentinel);
    GNFS_TEST_CHECK(reversed_i.ij_to_index(10, 1) == 0);

    const auto full_i =
        SieveRegion{std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max(), 1, 1};
    GNFS_TEST_CHECK(full_i.i_width() == 0);
    GNFS_TEST_CHECK(full_i.size() == 0);

    const auto full_j =
        SieveRegion{0, 0, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()};
    GNFS_TEST_CHECK(full_j.j_height() == 0);
    GNFS_TEST_CHECK(full_j.size() == 0);

    const auto ordinary = SieveRegion{-2, 1, -3, -2};
    const std::pair<int32_t, int32_t> ordinary_sentinel{ordinary.i_min, ordinary.j_min};
    GNFS_TEST_CHECK(ordinary.size() == 8);
    GNFS_TEST_CHECK(ordinary.index_to_ij(ordinary.size()) == ordinary_sentinel);
    GNFS_TEST_CHECK(ordinary.ij_to_index(ordinary.i_min - 1, ordinary.j_min) == ordinary.size());
    GNFS_TEST_CHECK(ordinary.ij_to_index(ordinary.i_min, ordinary.j_max + 1) == ordinary.size());

    const auto extreme_skew = default_sieve_region(std::numeric_limits<double>::max());
    GNFS_TEST_CHECK(extreme_skew.i_width() > 0);
    GNFS_TEST_CHECK(extreme_skew.j_height() > 0);
    GNFS_TEST_CHECK(extreme_skew.size() <= size_t{256} * 1024 * 1024);

    const auto invalid_skew = default_sieve_region(std::numeric_limits<double>::quiet_NaN());
    const auto unit_skew = default_sieve_region(1.0);
    GNFS_TEST_CHECK(invalid_skew.i_min == unit_skew.i_min);
    GNFS_TEST_CHECK(invalid_skew.i_max == unit_skew.i_max);
    GNFS_TEST_CHECK(invalid_skew.j_min == unit_skew.j_min);
    GNFS_TEST_CHECK(invalid_skew.j_max == unit_skew.j_max);

    std::cout << "  Sieve region extreme bounds: PASS" << std::endl;
}

void test_lattice_sieve_basic() {
    std::cout << "Testing basic lattice sieve..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);

    auto ctx = BaseMSelector::create_context(n, result);

    // 构建小因子基
    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = 1000;
    fb_opts.algebraic_bound = 1000;
    fb_opts.parallel = false;

    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    // 创建筛法
    SieveParams params;
    params.log_scale = 16;
    params.rational_threshold = 50;
    params.algebraic_threshold = 50;

    LatticeSieve sieve(ctx, fb, params);

    // 设置小区域
    SieveRegion small_region;
    small_region.i_min = -500;
    small_region.i_max = 499;
    small_region.j_min = 1;
    small_region.j_max = 100;
    sieve.set_region(small_region);

    // 选择一个 special-q
    SpecialQRange range;
    range.min_q = 500;
    range.max_q = 1000;
    SpecialQGenerator gen(fb, range);

    auto sq_opt = gen.next();
    assert(sq_opt.has_value());

    // 执行筛法
    auto sieve_result = sieve.sieve_special_q(*sq_opt);

    // 验证结果
    assert(sieve_result.sieved_positions == small_region.size());

    std::cout << "  Basic sieve: PASS (candidates=" << sieve_result.candidates.size()
              << ", region_size=" << sieve_result.sieved_positions << ")" << std::endl;
}

void test_candidate_properties() {
    std::cout << "Testing candidate properties..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);

    auto ctx = BaseMSelector::create_context(n, result);

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = 500;
    fb_opts.algebraic_bound = 500;
    fb_opts.parallel = false;

    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    SieveParams params;
    params.log_scale = 16;
    params.rational_threshold = 60;
    params.algebraic_threshold = 60;

    LatticeSieve sieve(ctx, fb, params);

    SieveRegion region;
    region.i_min = -200;
    region.i_max = 199;
    region.j_min = 1;
    region.j_max = 50;
    sieve.set_region(region);

    SpecialQRange range;
    range.min_q = 100;
    range.max_q = 500;
    SpecialQGenerator gen(fb, range);

    auto sq_opt = gen.next();
    assert(sq_opt.has_value());

    auto sieve_result = sieve.sieve_special_q(*sq_opt);

    // 验证所有候选点的属性
    LatticeBasis basis = compute_lattice_basis(*sq_opt);

    for (const auto& cand : sieve_result.candidates) {
        // b 应该 > 0
        assert(cand.b > 0);

        // gcd(a, b) 应该 = 1
        assert(std::gcd(util::safe_abs(cand.a), cand.b) == 1);

        // (a, b) 应该满足格条件
        assert(basis.verify_ab(cand.a, static_cast<int64_t>(cand.b)));
    }

    std::cout << "  Candidate properties: PASS (" << sieve_result.candidates.size()
              << " candidates verified)" << std::endl;
}

void test_mod_inverse() {
    std::cout << "Testing mod_inverse..." << std::endl;

    // 7 * 8 = 56 ≡ 1 (mod 11)
    // 所以 7^{-1} ≡ 8 (mod 11)
    auto test_inv = [](uint64_t a, uint64_t m, uint64_t expected) {
        // 使用 LatticeSieve 的 mod_inverse 是私有的，
        // 但我们可以通过验证 (a * inv) % m == 1 来测试
        // 这里直接测试扩展欧几里得
        int64_t t = 0, newt = 1;
        int64_t r = static_cast<int64_t>(m), newr = static_cast<int64_t>(a);

        while (newr != 0) {
            int64_t quotient = r / newr;
            t -= quotient * newt;
            std::swap(t, newt);
            r -= quotient * newr;
            std::swap(r, newr);
        }

        if (t < 0)
            t += static_cast<int64_t>(m);
        uint64_t inv = static_cast<uint64_t>(t);

        assert(inv == expected);
        assert((a * inv) % m == 1);
    };

    test_inv(7, 11, 8);
    test_inv(3, 7, 5); // 3 * 5 = 15 ≡ 1 (mod 7)
    test_inv(2, 5, 3); // 2 * 3 = 6 ≡ 1 (mod 5)

    std::cout << "  Mod inverse: PASS" << std::endl;
}

void test_default_region() {
    std::cout << "Testing default region with skewness..." << std::endl;

    // skewness = 1.0
    auto region1 = default_sieve_region(1.0);
    assert(region1.i_width() > 0);
    assert(region1.j_height() > 0);

    // skewness = 4.0 应该增大 i 范围
    auto region2 = default_sieve_region(4.0);
    assert(region2.i_width() > region1.i_width());
    assert(region2.j_height() < region1.j_height());

    std::cout << "  Default region: PASS (skew=1: " << region1.i_width() << "x"
              << region1.j_height() << ", skew=4: " << region2.i_width() << "x"
              << region2.j_height() << ")" << std::endl;
}

void test_lattice_sieve_storage_contract() {
    std::cout << "Testing lattice sieve storage contract..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    if (!result.success) {
        throw std::runtime_error("storage fixture polynomial selection failed");
    }
    auto ctx = BaseMSelector::create_context(n, result);

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = 500;
    fb_opts.algebraic_bound = 500;
    fb_opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    SieveParams params;
    LatticeSieve sieve(ctx, fb, params);
    if (sieve.allocated_sieve_bytes() != 0) {
        throw std::runtime_error("constructor eagerly reserved the default sieve region");
    }

    const auto expect_invalid_region = [&](const SieveRegion& invalid) {
        bool rejected = false;
        try {
            sieve.set_region(invalid);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        GNFS_TEST_CHECK(rejected);
        GNFS_TEST_CHECK(sieve.allocated_sieve_bytes() == 0);
    };
    expect_invalid_region({10, 9, 1, 1});
    expect_invalid_region({0, 0, 1, 0});
    expect_invalid_region(
        {std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max(), 1, 1});
    expect_invalid_region(
        {0, 0, std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max()});

    SieveRegion large_region;
    large_region.i_min = -1000;
    large_region.i_max = 999;
    large_region.j_min = 1;
    large_region.j_max = 200;
    sieve.set_region(large_region);
    const size_t large_required = large_region.size() * sizeof(uint16_t);
    const size_t large_allocated = sieve.allocated_sieve_bytes();
    if (large_allocated < large_required) {
        throw std::runtime_error("large region did not allocate its logical sieve storage");
    }

    SieveRegion small_region;
    small_region.i_min = -50;
    small_region.i_max = 49;
    small_region.j_min = 1;
    small_region.j_max = 20;
    sieve.set_region(small_region);
    const size_t small_required = small_region.size() * sizeof(uint16_t);
    const size_t small_allocated = sieve.allocated_sieve_bytes();
    if (small_allocated < small_required || small_allocated >= large_allocated / 4) {
        throw std::runtime_error("shrinking the region retained the prior sieve capacity");
    }

    LatticeSieve degenerate(ctx, fb, params);
    const SpecialQ r_zero{1009, 0, 0};
    const auto empty = degenerate.sieve_special_q(r_zero);
    if (!empty.candidates.empty() || empty.sieved_positions != 0 ||
        degenerate.allocated_sieve_bytes() != 0) {
        throw std::runtime_error("r=0 path allocated unused sieve storage");
    }

    std::cout << "  Storage contract: PASS (large=" << large_allocated
              << " bytes, small=" << small_allocated << " bytes)" << std::endl;
}

void test_lattice_sieve_special_q_entry_contract() {
    std::cout << "Testing lattice sieve special-q entry contract..." << std::endl;

    Integer n(test_n);
    auto selection = BaseMSelector::select(n, 3);
    if (!selection.success) {
        throw std::runtime_error("special-q entry fixture polynomial selection failed");
    }
    auto ctx = BaseMSelector::create_context(n, selection);

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = 500;
    fb_opts.algebraic_bound = 500;
    fb_opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    SieveParams params;
    LatticeSieve rejecting_sieve(ctx, fb, params);
    if (rejecting_sieve.allocated_sieve_bytes() != 0) {
        throw std::runtime_error("special-q rejection fixture started with sieve storage");
    }

    const auto expect_rejected_without_storage = [&](const SpecialQ& sq, const char* description) {
        bool rejected = false;
        try {
            (void)rejecting_sieve.sieve_special_q(sq);
        } catch (const std::invalid_argument&) {
            rejected = true;
        } catch (...) {
            throw std::runtime_error(std::string(description) + " raised the wrong exception type");
        }

        if (!rejected) {
            throw std::runtime_error(std::string(description) + " was not rejected");
        }
        if (rejecting_sieve.allocated_sieve_bytes() != 0) {
            throw std::runtime_error(std::string(description) +
                                     " allocated sieve storage before rejection");
        }
    };

    expect_rejected_without_storage(SpecialQ{101, AlgebraicPrime::PROJECTIVE_ROOT, 0},
                                    "projective special-q");
    expect_rejected_without_storage(SpecialQ{101, 101, 0}, "special-q with r equal to q");
    expect_rejected_without_storage(SpecialQ{1, 0, 0}, "special-q with invalid modulus");

    SpecialQRange range;
    range.min_q = 100;
    range.max_q = 500;
    SpecialQGenerator generator(fb, range);
    std::optional<SpecialQ> affine_sq;
    while (auto candidate = generator.next()) {
        if (candidate->q > 1 && candidate->r > 0 && candidate->r < candidate->q) {
            affine_sq = *candidate;
            break;
        }
    }
    if (!affine_sq.has_value()) {
        throw std::runtime_error("special-q entry fixture has no nonzero affine root");
    }

    LatticeSieve affine_sieve(ctx, fb, params);
    SieveRegion region;
    region.i_min = -16;
    region.i_max = 15;
    region.j_min = 1;
    region.j_max = 8;
    affine_sieve.set_region(region);
    const auto affine_result = affine_sieve.sieve_special_q(*affine_sq);
    if (affine_result.special_q.q != affine_sq->q || affine_result.special_q.r != affine_sq->r ||
        affine_result.sieved_positions != region.size()) {
        throw std::runtime_error("valid affine special-q did not follow the normal sieve path");
    }

    std::cout << "  Special-q entry contract: PASS" << std::endl;
}

// r=0 退化路径:LatticeSieve 在 sq.r==0 时 early-return 空 candidates。
// 该路径仅当 q | f₀ 时出现,极罕见,但代码必须正确 short-circuit
// (不要 estimate_initial_log 塌缩,不要在退化 basis 上跑全 sieve)。
void test_lattice_sieve_r_zero() {
    std::cout << "Testing r=0 degenerate special-q..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);
    auto ctx = BaseMSelector::create_context(n, result);

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = 1000;
    fb_opts.algebraic_bound = 1000;
    fb_opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    SieveParams params;
    params.log_scale = 16;
    params.rational_threshold = 50;
    params.algebraic_threshold = 50;
    LatticeSieve sieve(ctx, fb, params);

    SieveRegion small_region;
    small_region.i_min = -500;
    small_region.i_max = 499;
    small_region.j_min = 1;
    small_region.j_max = 100;
    sieve.set_region(small_region);

    // 手工构造 r=0 退化的 special-q(q 任选,r=0 触发退化分支)
    SpecialQ degenerate;
    degenerate.q = 1009;
    degenerate.r = 0;
    auto sieve_result = sieve.sieve_special_q(degenerate);

    // 期望:retainsspecial_q 字段、不抛、candidates 空、sieved_positions 应为 0
    // (early return 不跑 sieve)
    assert(sieve_result.special_q.q == 1009);
    assert(sieve_result.special_q.r == 0);
    assert(sieve_result.candidates.empty());
    assert(sieve_result.sieved_positions == 0);

    std::cout << "  r=0 degenerate path: PASS (early-return, no sieve work)" << std::endl;
}

int main() {
    std::cout << "=== Lattice Sieve Tests ===" << std::endl;

    test_lattice_basis();
    test_sieve_region();
    test_sieve_region_extreme_bounds();
    test_mod_inverse();
    test_default_region();
    test_lattice_sieve_storage_contract();
    test_lattice_sieve_special_q_entry_contract();
    test_lattice_sieve_basic();
    test_candidate_properties();
    test_lattice_sieve_r_zero();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
