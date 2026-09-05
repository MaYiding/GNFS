#include "gnfs/factor_base/builder.hpp"
#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/sieve/lattice_sieve.hpp"
#include "gnfs/util/primes.hpp"
#include "gnfs/util/safe_math.hpp"
#include "support/test_check.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace gnfs;
using namespace gnfs::sieve;
using namespace gnfs::factor_base;
using namespace gnfs::polynomial;
using namespace gnfs::core;

// 测试用的半素数
const char* test_n = "1000036000099";

[[nodiscard]] bool sieve_results_equal(const SieveResult& lhs, const SieveResult& rhs) {
    if (lhs.special_q.q != rhs.special_q.q || lhs.special_q.r != rhs.special_q.r ||
        lhs.special_q.index != rhs.special_q.index ||
        lhs.sieved_positions != rhs.sieved_positions || lhs.smooth_count != rhs.smooth_count ||
        lhs.candidates.size() != rhs.candidates.size()) {
        return false;
    }
    for (size_t index = 0; index < lhs.candidates.size(); ++index) {
        const auto& left = lhs.candidates[index];
        const auto& right = rhs.candidates[index];
        if (left.i != right.i || left.j != right.j || left.a != right.a || left.b != right.b ||
            left.residual != right.residual) {
            return false;
        }
    }
    return true;
}

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

    const auto expect_invalid_region = [&](const SieveRegion& invalid_region) {
        const size_t allocation_before = sieve.allocated_sieve_bytes();
        bool rejected = false;
        try {
            sieve.set_region(invalid_region);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        GNFS_TEST_CHECK(rejected);
        GNFS_TEST_CHECK(sieve.allocated_sieve_bytes() == allocation_before);
    };
    expect_invalid_region(SieveRegion{1, 0, 1, 1});
    expect_invalid_region(SieveRegion{0, 0, 2, 1});
    expect_invalid_region(SieveRegion{
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max(),
        1,
        1,
    });

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

    // Invalid publication must preserve both the allocated storage and the
    // previously selected logical region.
    const SieveRegion rejected_after_publish{1, 0, 1, 1};
    expect_invalid_region(rejected_after_publish);
    GNFS_TEST_CHECK(sieve.allocated_sieve_bytes() == small_allocated);

    const SpecialQ retained_region_sq{101, 1, 0};
    const auto retained_region_result = sieve.sieve_special_q(retained_region_sq);
    GNFS_TEST_CHECK(retained_region_result.sieved_positions == small_region.size());

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

void test_lattice_sieve_compact_width_contract() {
    std::cout << "Testing compact row-major width contract..." << std::endl;

    Integer n(test_n);
    auto selection = BaseMSelector::select(n, 3);
    if (!selection.success) {
        throw std::runtime_error("compact width fixture polynomial selection failed");
    }
    auto ctx = BaseMSelector::create_context(n, selection);

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = 500;
    fb_opts.algebraic_bound = 500;
    fb_opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    LatticeSieve sieve(ctx, fb);
    SieveRegion accepted;
    accepted.i_min = -16'384;
    accepted.i_max = 16'383;
    accepted.j_min = 1;
    accepted.j_max = 1;
    sieve.set_region(accepted);
    if (accepted.i_width() != 32'768 ||
        sieve.allocated_sieve_bytes() != accepted.size() * sizeof(uint16_t)) {
        throw std::runtime_error("compact width boundary was not accepted exactly");
    }

    // Wider regions remain valid: the production path routes them through the
    // fixed-width region-bucket implementation instead of compact row-major.
    SieveRegion wide = accepted;
    ++wide.i_max;
    sieve.set_region(wide);
    if (wide.i_width() != 32769 ||
        sieve.allocated_sieve_bytes() != wide.size() * sizeof(uint16_t)) {
        throw std::runtime_error("wide region was not accepted by the bucket contract");
    }

    std::cout << "  Compact width contract: PASS (32768 compact, 32769 bucket)" << std::endl;
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

void test_wide_region_uses_exact_bucket_path() {
    std::cout << "Testing wide-region bucket contract..." << std::endl;

    Integer n(test_n);
    auto selection = BaseMSelector::select(n, 3);
    GNFS_TEST_CHECK(selection.success);
    auto ctx = BaseMSelector::create_context(n, selection);

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = 500;
    fb_opts.algebraic_bound = 500;
    fb_opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

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
    GNFS_TEST_CHECK(affine_sq.has_value());

    SieveParams params;
    params.log_scale = 16;
    params.rational_threshold = 500;
    params.algebraic_threshold = 500;

    LatticeSieveExecutionConfig config{};
    config.fallback_thread_count = 1;
    config.enable_tiny_simd = false;
    config.enable_bucket_prefetch = false;

    const LatticeBasis basis =
        compute_lattice_basis_with_skewness(*affine_sq, ctx.skewness(), config.lattice_basis);

    // Cross a complete 64K bucket boundary and leave thousands of cells in
    // the second region. Choose the sign/orientation independently from the
    // sieve so b is positive over the complete one-row oracle interval.
    const std::array<SieveRegion, 4> region_candidates{{
        {-69'998, 1, 1, 1},
        {-1, 69'998, 1, 1},
        {-69'998, 1, -1, -1},
        {-1, 69'998, -1, -1},
    }};
    const auto has_positive_b_endpoints = [&](const SieveRegion& region) {
        const auto [first_a, first_b] = basis.to_ab(region.i_min, region.j_min);
        const auto [last_a, last_b] = basis.to_ab(region.i_max, region.j_max);
        static_cast<void>(first_a);
        static_cast<void>(last_a);
        return first_b > 0 && last_b > 0;
    };
    const auto selected_region =
        std::find_if(region_candidates.begin(), region_candidates.end(), has_positive_b_endpoints);
    GNFS_TEST_CHECK(selected_region != region_candidates.end());
    const SieveRegion wide_region = *selected_region;
    GNFS_TEST_CHECK(wide_region.i_width() == 70'000);

    LatticeSieve sieve(ctx, fb, params, config);
    sieve.set_region(wide_region);
    const auto actual = sieve.sieve_special_q(*affine_sq);
    GNFS_TEST_CHECK(actual.sieved_positions == wide_region.size());

    // Independent scalar oracle: test p | (a - b*root) directly for every
    // factor-base entry and reconstruct the additive score. This avoids both
    // production bucket helpers and their compact residue/offset types.
    const auto is_divisible = [](int64_t a, int64_t b, uint64_t root, uint32_t p) {
        const int64_t modulus = static_cast<int64_t>(p);
        int64_t a_mod = a % modulus;
        int64_t b_mod = b % modulus;
        if (a_mod < 0)
            a_mod += modulus;
        if (b_mod < 0)
            b_mod += modulus;
        const uint64_t product =
            (static_cast<uint64_t>(b_mod) * (root % p)) % static_cast<uint64_t>(p);
        return (static_cast<uint64_t>(a_mod) + p - product) % p == 0;
    };

    const double typical_i =
        std::max(1.0, static_cast<double>(static_cast<int64_t>(wide_region.i_max) -
                                          static_cast<int64_t>(wide_region.i_min)) /
                          4.0);
    const double typical_j =
        std::max(1.0, static_cast<double>(static_cast<int64_t>(wide_region.j_max) +
                                          static_cast<int64_t>(wide_region.j_min)) /
                          2.0);
    const double typical_a = std::abs(typical_i * static_cast<double>(basis.e0) +
                                      typical_j * static_cast<double>(basis.e1));
    const double typical_b = std::abs(typical_i * static_cast<double>(basis.f0) +
                                      typical_j * static_cast<double>(basis.f1));
    const double rational_value =
        std::max(1.0, std::abs(typical_a - typical_b * ctx.m().to_double()));
    const double algebraic_value = std::max(1.0, std::pow(std::max(typical_a, 1.0), ctx.degree()));
    const double combined_log =
        (std::log2(rational_value) + std::log2(algebraic_value)) * params.log_scale;
    GNFS_TEST_CHECK(std::isfinite(combined_log));
    GNFS_TEST_CHECK(combined_log >= 0.0);
    const uint16_t initial_log =
        static_cast<uint16_t>(std::min(combined_log, static_cast<double>(UINT16_MAX)));
    const uint16_t threshold = params.combined_threshold();
    GNFS_TEST_CHECK(initial_log > threshold);
    const uint16_t effective_threshold = static_cast<uint16_t>(initial_log - threshold);

    const auto& algebraic = fb.algebraic();
    std::vector<SieveCandidate> expected;
    constexpr size_t bucket_region_size = size_t{1} << 16;
    std::array<size_t, 2> expected_candidates_by_region{};
    GNFS_TEST_CHECK(wide_region.j_min == wide_region.j_max);
    for (size_t index = 0; index < wide_region.size(); ++index) {
        const int64_t i_wide =
            static_cast<int64_t>(wide_region.i_min) + static_cast<int64_t>(index);
        GNFS_TEST_CHECK(i_wide <= wide_region.i_max);
        const int32_t i = static_cast<int32_t>(i_wide);
        const int32_t j = wide_region.j_min;
        const auto [a, b] = basis.to_ab(i, j);
        uint16_t accumulated = 0;

        for (const auto& prime : fb.rational()) {
            const uint64_t m_mod_p = static_cast<uint64_t>(mpz_fdiv_ui(ctx.m().get_mpz(), prime.p));
            if (is_divisible(a, b, m_mod_p, prime.p)) {
                accumulated =
                    static_cast<uint16_t>(accumulated + static_cast<uint16_t>(prime.log_p));
            }
        }

        for (size_t prime_index = 0; prime_index < fb.sieve_algebraic_count(); ++prime_index) {
            const auto& prime = algebraic[prime_index];
            if (prime.is_projective() || (prime.p == affine_sq->q && prime.r == affine_sq->r)) {
                continue;
            }
            if (is_divisible(a, b, prime.r, prime.p)) {
                accumulated =
                    static_cast<uint16_t>(accumulated + static_cast<uint16_t>(prime.log_p));
            }
        }

        if (accumulated < effective_threshold || b <= 0 || std::gcd(util::safe_abs(a), b) != 1) {
            continue;
        }

        const uint16_t residual = accumulated <= initial_log
                                      ? static_cast<uint16_t>(initial_log - accumulated)
                                      : uint16_t{0};
        ++expected_candidates_by_region[index / bucket_region_size];
        expected.push_back(SieveCandidate{
            i,
            j,
            a,
            static_cast<uint64_t>(b),
            static_cast<uint8_t>(std::min(residual, uint16_t{255})),
        });
    }

    GNFS_TEST_CHECK(!expected.empty());
    GNFS_TEST_CHECK(actual.candidates.size() == expected.size());
    for (const size_t count : expected_candidates_by_region) {
        GNFS_TEST_CHECK(count > 0);
    }
    std::array<size_t, 2> actual_candidates_by_region{};
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto& got = actual.candidates[index];
        const auto& want = expected[index];
        GNFS_TEST_CHECK(got.i == want.i);
        GNFS_TEST_CHECK(got.j == want.j);
        GNFS_TEST_CHECK(got.a == want.a);
        GNFS_TEST_CHECK(got.b == want.b);
        GNFS_TEST_CHECK(got.residual == want.residual);
        GNFS_TEST_CHECK(got.b > 0);
        GNFS_TEST_CHECK(std::gcd(util::safe_abs(got.a), got.b) == 1);

        const uint64_t q = affine_sq->q;
        const int64_t row_offset =
            static_cast<int64_t>(got.i) - static_cast<int64_t>(wide_region.i_min);
        GNFS_TEST_CHECK(row_offset >= 0);
        GNFS_TEST_CHECK(row_offset < wide_region.i_width());
        ++actual_candidates_by_region[static_cast<size_t>(row_offset) / bucket_region_size];

        int64_t a_mod_q = got.a % static_cast<int64_t>(q);
        if (a_mod_q < 0)
            a_mod_q += static_cast<int64_t>(q);
        const uint64_t br_mod_q = ((got.b % q) * affine_sq->r) % q;
        GNFS_TEST_CHECK(static_cast<uint64_t>(a_mod_q) == br_mod_q);
    }
    GNFS_TEST_CHECK(actual_candidates_by_region == expected_candidates_by_region);

    std::cout << "  Wide-region bucket contract: PASS (candidates=" << actual.candidates.size()
              << ")" << std::endl;
}

void test_wide_region_prime_classes_are_exact_once() {
    std::cout << "Testing wide-region exact-once prime classes..." << std::endl;

    // f(x) = x^3 + 812 has f(7) = 1155 = 15 * 77, while f(2) is
    // divisible by 5. Thus (q, r) = (5, 2) is a valid affine special-q
    // for a context whose required m-root congruence also holds.
    std::vector<Integer> coefficients;
    coefficients.emplace_back(int64_t{812});
    coefficients.emplace_back(int64_t{0});
    coefficients.emplace_back(int64_t{0});
    coefficients.emplace_back(int64_t{1});
    PolynomialContext ctx(Integer(int64_t{77}), std::move(coefficients), Integer(int64_t{7}), 1.0);
    const SpecialQ sq{5, 2, 0};
    GNFS_TEST_CHECK(ctx.evaluate(ctx.m()).to_int64() == 1155);
    GNFS_TEST_CHECK(ctx.evaluate_mod(7, 77) == 0);
    GNFS_TEST_CHECK(ctx.evaluate_mod(sq.r, sq.q) == 0);
    GNFS_TEST_CHECK(std::gcd(uint64_t{77}, uint64_t{sq.q}) == 1);

    LatticeSieveExecutionConfig config{};
    config.fallback_thread_count = 1;
    config.enable_tiny_simd = false;
    config.enable_bucket_prefetch = false;
    const LatticeBasis basis =
        compute_lattice_basis_with_skewness(sq, ctx.skewness(), config.lattice_basis);
    GNFS_TEST_CHECK(basis.e0 == 1);
    GNFS_TEST_CHECK(basis.f0 == -2);
    GNFS_TEST_CHECK(basis.e1 == 2);
    GNFS_TEST_CHECK(basis.f1 == 1);

    // j=3 makes the independently classified p=3 v-prime hit the complete
    // row. Keep i_max <= 1 so b stays positive, and leave 4464 cells after
    // the 64K boundary so every prime class is observable in both buckets.
    const SieveRegion wide_region{-69'998, 1, 3, 3};
    GNFS_TEST_CHECK(wide_region.i_width() == 70'000);
    GNFS_TEST_CHECK(wide_region.j_height() == 1);

    SieveParams params;
    params.log_scale = 16;
    params.rational_threshold = 64;
    params.algebraic_threshold = 0;

    // Independent reconstruction of the production threshold. This is kept
    // local and does not call a production estimate helper.
    const double typical_i =
        std::max(1.0, static_cast<double>(static_cast<int64_t>(wide_region.i_max) -
                                          static_cast<int64_t>(wide_region.i_min)) /
                          4.0);
    const double typical_j =
        std::max(1.0, static_cast<double>(static_cast<int64_t>(wide_region.j_max) +
                                          static_cast<int64_t>(wide_region.j_min)) /
                          2.0);
    const double typical_a = std::abs(typical_i * static_cast<double>(basis.e0) +
                                      typical_j * static_cast<double>(basis.e1));
    const double typical_b = std::abs(typical_i * static_cast<double>(basis.f0) +
                                      typical_j * static_cast<double>(basis.f1));
    const double rational_value =
        std::max(1.0, std::abs(typical_a - typical_b * ctx.m().to_double()));
    const double algebraic_value = std::max(1.0, std::pow(std::max(typical_a, 1.0), ctx.degree()));
    const double combined_log =
        (std::log2(rational_value) + std::log2(algebraic_value)) * params.log_scale;
    GNFS_TEST_CHECK(std::isfinite(combined_log));
    GNFS_TEST_CHECK(combined_log > 81.0);
    const uint16_t initial_log =
        static_cast<uint16_t>(std::min(combined_log, static_cast<double>(UINT16_MAX)));
    GNFS_TEST_CHECK(initial_log > params.combined_threshold());

    enum class PrimeClass : size_t {
        global,
        v,
        tiny,
        medium,
        count,
    };
    struct OraclePrime {
        uint32_t p;
        uint32_t root;
        uint16_t log_p;
        PrimeClass expected_class;
    };

    const auto positive_mod = [](int64_t value, uint32_t p) {
        int64_t residue = value % static_cast<int64_t>(p);
        if (residue < 0) {
            residue += static_cast<int64_t>(p);
        }
        return static_cast<uint32_t>(residue);
    };
    const auto uv_for_root = [&](uint32_t root, uint32_t p) {
        const uint64_t f0_root = (static_cast<uint64_t>(positive_mod(basis.f0, p)) * root) % p;
        const uint64_t f1_root = (static_cast<uint64_t>(positive_mod(basis.f1, p)) * root) % p;
        const uint32_t u = static_cast<uint32_t>(
            (static_cast<uint64_t>(positive_mod(basis.e0, p)) + p - f0_root) % p);
        const uint32_t v = static_cast<uint32_t>(
            (static_cast<uint64_t>(positive_mod(basis.e1, p)) + p - f1_root) % p);
        return std::pair<uint32_t, uint32_t>{u, v};
    };
    const auto classify = [&](const OraclePrime& prime) {
        const auto [u, v] = uv_for_root(prime.root, prime.p);
        if (u == 0 && v == 0) {
            return PrimeClass::global;
        }
        if (u == 0) {
            return PrimeClass::v;
        }
        return prime.p < 256 ? PrimeClass::tiny : PrimeClass::medium;
    };

    const uint16_t global_log = static_cast<uint16_t>(initial_log - params.combined_threshold());
    GNFS_TEST_CHECK(static_cast<uint32_t>(global_log) + 3U + 5U + 9U < initial_log);
    const std::array<OraclePrime, 4> oracle_primes{{
        {5, 2, global_log, PrimeClass::global},
        {3, 1, 3, PrimeClass::v},
        {2, 1, 5, PrimeClass::tiny},
        {257, 7, 9, PrimeClass::medium},
    }};

    FactorBaseParams fb_params;
    fb_params.rational_bound = 500;
    fb_params.algebraic_bound = 500;
    fb_params.log_scale = params.log_scale;
    FactorBase fb(fb_params);

    std::array<size_t, static_cast<size_t>(PrimeClass::count)> class_counts{};
    for (const auto& prime : oracle_primes) {
        GNFS_TEST_CHECK(prime.root ==
                        static_cast<uint32_t>(mpz_fdiv_ui(ctx.m().get_mpz(), prime.p)));
        const PrimeClass actual_class = classify(prime);
        GNFS_TEST_CHECK(actual_class == prime.expected_class);
        ++class_counts[static_cast<size_t>(actual_class)];
        fb.add_rational(prime.p, prime.log_p);
    }
    for (const size_t count : class_counts) {
        GNFS_TEST_CHECK(count == 1);
    }

    // Keep the valid special-q in the algebraic base. Production must skip
    // this exact (p, r) entry rather than adding it a fifth time.
    fb.add_algebraic(sq.q, sq.r, 7);
    fb.set_sieve_algebraic_count(1);

    LatticeSieve sieve(ctx, fb, params, config);
    sieve.set_region(wide_region);
    const auto actual = sieve.sieve_special_q(sq);
    GNFS_TEST_CHECK(actual.sieved_positions == wide_region.size());

    const auto is_divisible = [](int64_t a, int64_t b, uint32_t root, uint32_t p) {
        const int64_t modulus = static_cast<int64_t>(p);
        int64_t a_mod = a % modulus;
        int64_t b_mod = b % modulus;
        if (a_mod < 0) {
            a_mod += modulus;
        }
        if (b_mod < 0) {
            b_mod += modulus;
        }
        const uint64_t product = (static_cast<uint64_t>(b_mod) * root) % static_cast<uint64_t>(p);
        return (static_cast<uint64_t>(a_mod) + p - product) % p == 0;
    };

    const uint16_t effective_threshold =
        static_cast<uint16_t>(initial_log - params.combined_threshold());
    constexpr size_t bucket_region_size = size_t{1} << 16;
    using ClassHitCounts = std::array<size_t, static_cast<size_t>(PrimeClass::count)>;
    std::array<ClassHitCounts, 2> observable_hits_by_region{};
    std::array<size_t, 2> observable_candidates_by_region{};
    std::vector<SieveCandidate> expected;
    for (size_t index = 0; index < wide_region.size(); ++index) {
        const int64_t i_wide =
            static_cast<int64_t>(wide_region.i_min) + static_cast<int64_t>(index);
        GNFS_TEST_CHECK(i_wide <= wide_region.i_max);
        const int32_t i = static_cast<int32_t>(i_wide);
        const int32_t j = wide_region.j_min;
        // Golden basis oracle: (a,b) = i*(1,-2) + j*(2,1).
        const int64_t a = static_cast<int64_t>(i) + 2 * static_cast<int64_t>(j);
        const int64_t b = -2 * static_cast<int64_t>(i) + static_cast<int64_t>(j);
        GNFS_TEST_CHECK(b > 0);

        uint16_t accumulated = 0;
        std::array<bool, static_cast<size_t>(PrimeClass::count)> hits{};
        for (const auto& prime : oracle_primes) {
            if (!is_divisible(a, b, prime.root, prime.p)) {
                continue;
            }
            accumulated = static_cast<uint16_t>(accumulated + prime.log_p);
            hits[static_cast<size_t>(prime.expected_class)] = true;
        }

        if (accumulated < effective_threshold || b <= 0 || std::gcd(util::safe_abs(a), b) != 1) {
            continue;
        }
        const size_t bucket_region = index / bucket_region_size;
        ++observable_candidates_by_region[bucket_region];
        for (size_t prime_class = 0; prime_class < hits.size(); ++prime_class) {
            if (hits[prime_class]) {
                ++observable_hits_by_region[bucket_region][prime_class];
            }
        }

        const uint16_t residual = accumulated <= initial_log
                                      ? static_cast<uint16_t>(initial_log - accumulated)
                                      : uint16_t{0};
        GNFS_TEST_CHECK(residual < 255);
        expected.push_back(SieveCandidate{
            i,
            j,
            a,
            static_cast<uint64_t>(b),
            static_cast<uint8_t>(std::min(residual, uint16_t{255})),
        });
    }

    for (size_t bucket_region = 0; bucket_region < observable_hits_by_region.size();
         ++bucket_region) {
        GNFS_TEST_CHECK(observable_candidates_by_region[bucket_region] > 0);
        for (const size_t count : observable_hits_by_region[bucket_region]) {
            GNFS_TEST_CHECK(count > 0);
        }
    }
    GNFS_TEST_CHECK(!expected.empty());
    GNFS_TEST_CHECK(actual.candidates.size() == expected.size());
    std::array<size_t, 2> actual_candidates_by_region{};
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto& got = actual.candidates[index];
        const auto& want = expected[index];
        GNFS_TEST_CHECK(got.i == want.i);
        GNFS_TEST_CHECK(got.j == want.j);
        GNFS_TEST_CHECK(got.a == want.a);
        GNFS_TEST_CHECK(got.b == want.b);
        GNFS_TEST_CHECK(got.residual == want.residual);
        GNFS_TEST_CHECK((got.a - static_cast<int64_t>(got.b) * sq.r) % static_cast<int64_t>(sq.q) ==
                        0);
        const int64_t coordinate_offset =
            static_cast<int64_t>(got.i) - static_cast<int64_t>(wide_region.i_min);
        GNFS_TEST_CHECK(coordinate_offset >= 0);
        GNFS_TEST_CHECK(coordinate_offset < wide_region.i_width());
        ++actual_candidates_by_region[static_cast<size_t>(coordinate_offset) / bucket_region_size];
    }
    GNFS_TEST_CHECK(actual_candidates_by_region == observable_candidates_by_region);

    std::cout << "  Wide-region exact-once classes: PASS (candidates=" << actual.candidates.size()
              << ")" << std::endl;
}

void test_extreme_j_row_offset_sieving() {
    std::cout << "Testing extreme j row-offset sieving..." << std::endl;

    std::vector<Integer> coefficients;
    coefficients.emplace_back(int64_t{812});
    coefficients.emplace_back(int64_t{0});
    coefficients.emplace_back(int64_t{0});
    coefficients.emplace_back(int64_t{1});
    PolynomialContext ctx(Integer(int64_t{77}), std::move(coefficients), Integer(int64_t{7}), 1.0);
    const SpecialQ sq{5, 2, 0};
    GNFS_TEST_CHECK(ctx.evaluate_mod(sq.r, sq.q) == 0);

    FactorBaseParams fb_params;
    fb_params.rational_bound = 500;
    fb_params.algebraic_bound = 500;
    fb_params.log_scale = 16;
    FactorBase fb(fb_params);
    fb.add_rational(2, 5);
    fb.add_rational(3, 7);
    fb.add_rational(5, 11);
    fb.add_rational(257, 13);
    fb.set_sieve_algebraic_count(0);

    SieveParams params;
    params.log_scale = 16;
    params.rational_threshold = 64;
    params.algebraic_threshold = 0;

    LatticeSieveExecutionConfig config{};
    config.fallback_thread_count = 2;
    config.enable_tiny_simd = false;
    config.enable_bucket_prefetch = false;
    LatticeSieve sieve(ctx, fb, params, config);
    sieve.set_max_threads(2);

    // Height 500 takes the parallel row-chunk path. Its half-open offset
    // boundary is representable even though coordinate j_max + 1 is not.
    const SieveRegion high_region{
        -2,
        2,
        std::numeric_limits<int32_t>::max() - 499,
        std::numeric_limits<int32_t>::max(),
    };
    GNFS_TEST_CHECK(high_region.j_height() == 500);
    sieve.set_region(high_region);
    const auto high = sieve.sieve_special_q(sq);
    GNFS_TEST_CHECK(high.sieved_positions == high_region.size());

    // The opposite endpoint exercises a negative midpoint outside int32 and
    // the single-thread row-offset path.
    const SieveRegion low_region{
        -2,
        2,
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::min() + 1,
    };
    GNFS_TEST_CHECK(low_region.j_height() == 2);
    sieve.set_region(low_region);
    const auto low = sieve.sieve_special_q(sq);
    GNFS_TEST_CHECK(low.sieved_positions == low_region.size());

    std::cout << "  Extreme j row-offset sieving: PASS" << std::endl;
}

void test_parallel_sieve_phase_equivalence() {
    std::cout << "Testing joined-worker sieve phase equivalence..." << std::endl;

    std::vector<Integer> coefficients;
    coefficients.emplace_back(int64_t{812});
    coefficients.emplace_back(int64_t{0});
    coefficients.emplace_back(int64_t{0});
    coefficients.emplace_back(int64_t{1});
    PolynomialContext ctx(Integer(int64_t{77}), std::move(coefficients), Integer(int64_t{7}), 1.0);
    const SpecialQ sq{5, 2, 0};
    GNFS_TEST_CHECK(ctx.evaluate_mod(sq.r, sq.q) == 0);

    FactorBaseParams fb_params;
    fb_params.rational_bound = 2'000;
    fb_params.algebraic_bound = 2'000;
    fb_params.log_scale = 16;

    LatticeSieveExecutionConfig config{};
    config.fallback_thread_count = 2;
    config.enable_tiny_simd = false;
    config.enable_bucket_prefetch = false;

    const auto run = [&](const FactorBase& factor_base, const SieveParams& params,
                         const SieveRegion& region, size_t threads) {
        LatticeSieve sieve(ctx, factor_base, params, config);
        sieve.set_max_threads(threads);
        sieve.set_region(region);
        return sieve.sieve_special_q(sq);
    };
    const auto has_candidate = [](const SieveResult& result, int32_t i, int32_t j, int64_t a,
                                  uint64_t b) {
        return std::any_of(result.candidates.begin(), result.candidates.end(),
                           [&](const SieveCandidate& candidate) {
                               return candidate.i == i && candidate.j == j && candidate.a == a &&
                                      candidate.b == b;
                           });
    };

    // Exactly 500 rows crosses the row-chunk threshold. The three-column
    // region stays on the compact row-major path, isolating that worker group.
    FactorBase row_factor_base(fb_params);
    row_factor_base.add_rational(2, 17);
    row_factor_base.add_rational(3, 512);
    row_factor_base.set_sieve_algebraic_count(0);
    SieveParams row_params;
    row_params.log_scale = 16;
    row_params.rational_threshold = 100;
    row_params.algebraic_threshold = 0;
    const SieveRegion row_region{-2, 0, 1, 500};
    GNFS_TEST_CHECK(row_region.i_width() == 3);
    GNFS_TEST_CHECK(row_region.j_height() == 500);
    const auto row_serial = run(row_factor_base, row_params, row_region, 1);
    const auto row_parallel = run(row_factor_base, row_params, row_region, 2);
    GNFS_TEST_CHECK(!row_serial.candidates.empty());
    GNFS_TEST_CHECK(has_candidate(row_serial, -2, 3, 4, 7));
    GNFS_TEST_CHECK(has_candidate(row_serial, -1, 252, 503, 254));
    GNFS_TEST_CHECK(sieve_results_equal(row_serial, row_parallel));

    // One row and one 64K region keep apply serial. Exactly 100 normal primes
    // at or above 256 cross only the scatter worker threshold.
    FactorBase scatter_factor_base(fb_params);
    uint64_t prime = 256;
    for (size_t index = 0; index < 100; ++index) {
        prime = util::next_prime_u64(prime);
        GNFS_TEST_CHECK(prime >= 257);
        GNFS_TEST_CHECK(prime <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max()));
        scatter_factor_base.add_rational(static_cast<uint32_t>(prime), 512);
    }
    scatter_factor_base.set_sieve_algebraic_count(0);
    GNFS_TEST_CHECK(scatter_factor_base.rational().size() == 100);
    GNFS_TEST_CHECK(prime == 887);
    SieveParams scatter_params;
    scatter_params.log_scale = 16;
    scatter_params.rational_threshold = 100;
    scatter_params.algebraic_threshold = 0;
    const SieveRegion scatter_region{-255, 0, 1, 1};
    GNFS_TEST_CHECK(scatter_region.i_width() == 256);
    GNFS_TEST_CHECK(scatter_region.size() < (size_t{1} << 16));
    const auto scatter_serial = run(scatter_factor_base, scatter_params, scatter_region, 1);
    const auto scatter_parallel = run(scatter_factor_base, scatter_params, scatter_region, 2);
    GNFS_TEST_CHECK(!scatter_serial.candidates.empty());
    GNFS_TEST_CHECK(has_candidate(scatter_serial, -171, 1, -169, 343));
    GNFS_TEST_CHECK(sieve_results_equal(scatter_serial, scatter_parallel));

    // A width just beyond the compact-row limit forces region buckets. Six
    // rows produce exactly four regions, while one medium prime keeps scatter
    // serial and therefore isolates the dynamic apply worker group.
    FactorBase apply_factor_base(fb_params);
    apply_factor_base.add_rational(2, 17);
    apply_factor_base.add_rational(3, 512);
    apply_factor_base.add_rational(257, 19);
    apply_factor_base.set_sieve_algebraic_count(0);
    SieveParams apply_params;
    apply_params.log_scale = 16;
    apply_params.rational_threshold = 400;
    apply_params.algebraic_threshold = 0;
    const SieveRegion apply_region{-32'768, 0, 1, 6};
    constexpr size_t bucket_region_size = size_t{1} << 16;
    const size_t apply_region_count =
        apply_region.size() / bucket_region_size +
        (apply_region.size() % bucket_region_size != 0 ? size_t{1} : size_t{0});
    GNFS_TEST_CHECK(apply_region.i_width() == 32'769);
    GNFS_TEST_CHECK(apply_region_count == 4);
    const auto apply_serial = run(apply_factor_base, apply_params, apply_region, 1);
    const auto apply_parallel = run(apply_factor_base, apply_params, apply_region, 2);
    GNFS_TEST_CHECK(!apply_serial.candidates.empty());
    GNFS_TEST_CHECK(has_candidate(apply_serial, -5, 6, 7, 16));
    GNFS_TEST_CHECK(sieve_results_equal(apply_serial, apply_parallel));

    std::cout << "  Joined-worker sieve phases: PASS" << std::endl;
}

void test_projection_overflow_rejected_before_sieving() {
    std::cout << "Testing lattice projection overflow rejection..." << std::endl;

    std::vector<Integer> coefficients;
    coefficients.emplace_back(int64_t{812});
    coefficients.emplace_back(int64_t{0});
    coefficients.emplace_back(int64_t{0});
    coefficients.emplace_back(int64_t{1});
    PolynomialContext ctx(Integer(int64_t{77}), std::move(coefficients), Integer(int64_t{7}), 1e12);

    FactorBaseParams fb_params;
    fb_params.rational_bound = 2;
    fb_params.algebraic_bound = 2;
    FactorBase fb(fb_params);

    LatticeSieveExecutionConfig config{};
    config.lattice_basis.base_method = LatticeReductionMethod::SkewLLL;
    config.fallback_thread_count = 1;
    config.enable_tiny_simd = false;
    config.enable_bucket_prefetch = false;

    const SpecialQ sq{4'294'967'291U, 3'542'712'079U, 0};
    GNFS_TEST_CHECK(ctx.evaluate_mod(sq.r, sq.q) == 0);
    const LatticeBasis basis =
        compute_lattice_basis_with_skewness(sq, ctx.skewness(), config.lattice_basis);
    const SieveRegion region{
        -std::numeric_limits<int32_t>::max(),
        -std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::max(),
    };
    GNFS_TEST_CHECK(!lattice_projection_fits_int64(basis, region));

    LatticeSieve sieve(ctx, fb, SieveParams{}, config);
    sieve.set_region(region);
    bool rejected = false;
    try {
        (void)sieve.sieve_special_q(sq);
    } catch (const std::overflow_error&) {
        rejected = true;
    }
    GNFS_TEST_CHECK(rejected);
    GNFS_TEST_CHECK(sieve.sieve_cell_count() == region.size());

    std::cout << "  Projection overflow rejection: PASS" << std::endl;
}

void test_factor_base_prime_admission_contract() {
    std::cout << "Testing lattice factor-base prime admission..." << std::endl;

    std::vector<Integer> coefficients;
    coefficients.emplace_back(int64_t{812});
    coefficients.emplace_back(int64_t{0});
    coefficients.emplace_back(int64_t{0});
    coefficients.emplace_back(int64_t{1});
    PolynomialContext ctx(Integer(int64_t{77}), std::move(coefficients), Integer(int64_t{7}), 1.0);
    const SpecialQ sq{5, 2, 0};
    const SieveRegion region{-2, 2, 1, 2};

    LatticeSieveExecutionConfig config{};
    config.fallback_thread_count = 1;
    config.enable_tiny_simd = false;
    config.enable_bucket_prefetch = false;

    const auto expect_rejected = [&](FactorBase& factor_base) {
        LatticeSieve sieve(ctx, factor_base, SieveParams{}, config);
        sieve.set_region(region);
        bool rejected = false;
        try {
            (void)sieve.sieve_special_q(sq);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        GNFS_TEST_CHECK(rejected);
    };

    FactorBaseParams fb_params;
    fb_params.rational_bound = std::numeric_limits<uint32_t>::max();
    fb_params.algebraic_bound = std::numeric_limits<uint32_t>::max();

    {
        FactorBase factor_base(fb_params);
        factor_base.add_rational(0, 1);
        expect_rejected(factor_base);
    }
    {
        FactorBase factor_base(fb_params);
        factor_base.add_rational(4'294'967'291U, 1);
        expect_rejected(factor_base);
    }
    {
        FactorBase factor_base(fb_params);
        factor_base.add_algebraic(4'294'967'291U, 3'037'000'506U, 1);
        expect_rejected(factor_base);
    }
    {
        FactorBase factor_base(fb_params);
        factor_base.add_rational(3,
                                 static_cast<uint32_t>(std::numeric_limits<uint16_t>::max()) + 1U);
        expect_rejected(factor_base);
    }
    {
        FactorBase factor_base(fb_params);
        factor_base.add_algebraic(3, 1,
                                  static_cast<uint32_t>(std::numeric_limits<uint16_t>::max()) + 1U);
        expect_rejected(factor_base);
    }
    {
        FactorBase factor_base(fb_params);
        factor_base.add_algebraic(3, 1, 1);
        factor_base.set_sieve_algebraic_count(2);
        expect_rejected(factor_base);
    }

    // With this basis and m=7, the INT32_MAX rational prime solves 3i=j.
    // Advancing from j=-2 to j=-1 adds 1,431,655,765 to an initial residue of
    // 1,431,655,764, crossing INT32_MAX before the modular subtraction. Only
    // the second row hits the selected coordinate. Assert that independent
    // oracle through both the compact fill_buckets path and the full wide
    // region scatter path.
    FactorBase boundary_factor_base(fb_params);
    boundary_factor_base.add_rational(static_cast<uint32_t>(std::numeric_limits<int32_t>::max()),
                                      std::numeric_limits<uint16_t>::max());
    SieveParams boundary_params;
    boundary_params.rational_threshold = 0;
    boundary_params.algebraic_threshold = 0;
    constexpr int32_t carry_hit_i = -1'431'655'765;
    const auto expect_single_carry_hit = [&](const SieveRegion& carry_region) {
        LatticeSieve sieve(ctx, boundary_factor_base, boundary_params, config);
        sieve.set_region(carry_region);
        const auto result = sieve.sieve_special_q(sq);
        GNFS_TEST_CHECK(result.sieved_positions == carry_region.size());
        GNFS_TEST_CHECK(result.candidates.size() == 1);
        GNFS_TEST_CHECK(result.candidates[0].i == carry_hit_i);
        GNFS_TEST_CHECK(result.candidates[0].j == -1);
    };
    expect_single_carry_hit({carry_hit_i, carry_hit_i, -2, -1});
    expect_single_carry_hit({carry_hit_i, carry_hit_i + 32'768, -2, -1});

    std::cout << "  Factor-base prime admission: PASS" << std::endl;
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
    test_mod_inverse();
    test_default_region();
    test_lattice_sieve_storage_contract();
    test_lattice_sieve_compact_width_contract();
    test_lattice_sieve_special_q_entry_contract();
    test_wide_region_uses_exact_bucket_path();
    test_wide_region_prime_classes_are_exact_once();
    test_extreme_j_row_offset_sieving();
    test_parallel_sieve_phase_equivalence();
    test_projection_overflow_rejected_before_sieving();
    test_factor_base_prime_admission_contract();
    test_lattice_sieve_basic();
    test_candidate_properties();
    test_lattice_sieve_r_zero();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
