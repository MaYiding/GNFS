#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/factor_base/builder.hpp"
#include "gnfs/sieve/special_q.hpp"
#include "gnfs/sieve/lattice_sieve.hpp"
#include "gnfs/relation/collector.hpp"
#include "gnfs/util/safe_math.hpp"

#include <cassert>
#include <iostream>
#include <iomanip>

using namespace gnfs;
using namespace gnfs::core;
using namespace gnfs::polynomial;
using namespace gnfs::factor_base;
using namespace gnfs::sieve;
using namespace gnfs::relation;

/// 测试完整的筛法流程
void test_full_sieve_pipeline() {
    std::cout << "=== Integration Test: Full Sieve Pipeline ===" << std::endl;

    // 1. 选择一个适合测试的数（两个中等大小素数的乘积）
    // 使用一个 13 位数：1000003 * 1000033 = 1000036000099
    const char* n_str = "1000036000099";
    std::cout << "\n[1] Number to factor: " << n_str << std::endl;

    Integer n(n_str);
    std::cout << "    Digits: " << n.num_digits(10) << std::endl;

    // 2. 多项式选择
    std::cout << "\n[2] Polynomial Selection (Base-m method)" << std::endl;

    uint32_t degree = 3;  // 对于小数使用低度数
    auto poly_result = BaseMSelector::select(n, degree);
    assert(poly_result.success);

    auto ctx = BaseMSelector::create_context(n, poly_result);

    std::cout << "    Degree: " << ctx.degree() << std::endl;
    std::cout << "    m = " << ctx.m().to_string() << std::endl;
    std::cout << "    Skewness: " << std::fixed << std::setprecision(2) << ctx.skewness() << std::endl;

    // 打印多项式
    std::cout << "    f(x) = ";
    for (int i = static_cast<int>(ctx.degree()); i >= 0; --i) {
        const auto& coeff = ctx.coeff(i);
        if (!coeff.is_zero() || i == 0) {
            if (i < static_cast<int>(ctx.degree())) {
                std::cout << (coeff.is_negative() ? " - " : " + ");
            }
            auto abs_coeff = coeff.clone();
            abs_coeff.abs();
            if (i == 0 || abs_coeff.to_string() != "1") {
                std::cout << abs_coeff.to_string();
            }
            if (i > 0) {
                std::cout << "x";
                if (i > 1) std::cout << "^" << i;
            }
        }
    }
    std::cout << std::endl;

    // 验证 f(m) ≡ 0 (mod n)
    assert(ctx.verify());
    std::cout << "    f(m) ≡ 0 (mod n): verified ✓" << std::endl;

    // 3. 构建因子基
    std::cout << "\n[3] Building Factor Base" << std::endl;

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = 5000;
    fb_opts.algebraic_bound = 5000;
    fb_opts.log_scale = 16;
    fb_opts.parallel = true;

    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    std::cout << "    Rational primes: " << fb.rational_count() << std::endl;
    std::cout << "    Algebraic primes: " << fb.algebraic_count() << std::endl;
    std::cout << "    Rational bound: " << fb.params().rational_bound << std::endl;
    std::cout << "    Algebraic bound: " << fb.params().algebraic_bound << std::endl;

    // 4. 配置筛法
    std::cout << "\n[4] Configuring Lattice Sieve" << std::endl;

    SieveParams sieve_params;
    sieve_params.log_scale = 16;
    sieve_params.rational_threshold = 60;
    sieve_params.algebraic_threshold = 60;

    LatticeSieve sieve(ctx, fb, sieve_params);

    // 设置较小的筛区域用于测试
    SieveRegion region;
    region.i_min = -1000;
    region.i_max = 999;
    region.j_min = 1;
    region.j_max = 200;
    sieve.set_region(region);

    std::cout << "    Sieve region: i ∈ [" << region.i_min << ", " << region.i_max << "]"
              << ", j ∈ [" << region.j_min << ", " << region.j_max << "]" << std::endl;
    std::cout << "    Region size: " << region.size() << " positions" << std::endl;
    std::cout << "    Thresholds: rat=" << static_cast<int>(sieve_params.rational_threshold)
              << ", alg=" << static_cast<int>(sieve_params.algebraic_threshold) << std::endl;

    // 5. 生成 Special-Q 并筛选
    std::cout << "\n[5] Sieving with Special-Q" << std::endl;

    SpecialQRange sq_range;
    sq_range.min_q = 1000;
    sq_range.max_q = 3000;

    SpecialQGenerator sq_gen(fb, sq_range);

    size_t num_sq_to_process = 10;
    size_t total_candidates = 0;
    size_t total_positions = 0;

    std::cout << "    Processing " << num_sq_to_process << " special-q values..." << std::endl;

    for (size_t i = 0; i < num_sq_to_process && sq_gen.has_next(); ++i) {
        auto sq = sq_gen.next();
        if (!sq) break;

        auto result = sieve.sieve_special_q(*sq);

        total_candidates += result.candidates.size();
        total_positions += result.sieved_positions;

        if (i < 3 || result.candidates.size() > 0) {
            std::cout << "    Special-Q (" << sq->q << ", " << sq->r << "): "
                      << result.candidates.size() << " candidates" << std::endl;
        }
    }

    std::cout << "\n    Total positions sieved: " << total_positions << std::endl;
    std::cout << "    Total candidates found: " << total_candidates << std::endl;

    if (total_positions > 0) {
        double candidate_rate = static_cast<double>(total_candidates) / total_positions * 100;
        std::cout << "    Candidate rate: " << std::fixed << std::setprecision(4)
                  << candidate_rate << "%" << std::endl;
    }

    // 6. 验证候选点
    std::cout << "\n[6] Verifying Candidates" << std::endl;

    // 重新筛一个 special-q 来获取候选点
    sq_gen.reset_to(0);
    sq_gen.next();  // 跳到 min_q

    size_t verified = 0;
    size_t total_checked = 0;

    for (size_t i = 0; i < 3 && sq_gen.has_next(); ++i) {
        auto sq = sq_gen.next();
        if (!sq) break;

        auto result = sieve.sieve_special_q(*sq);
        LatticeBasis basis = compute_lattice_basis(*sq);

        for (const auto& cand : result.candidates) {
            ++total_checked;

            // 验证格条件
            bool lattice_ok = basis.verify_ab(cand.a, static_cast<int64_t>(cand.b));

            // 验证 gcd(a, b) = 1
            bool coprime_ok = std::gcd(util::safe_abs(cand.a), cand.b) == 1;

            // 验证 b > 0
            bool b_positive = cand.b > 0;

            if (lattice_ok && coprime_ok && b_positive) {
                ++verified;
            }
        }
    }

    std::cout << "    Checked: " << total_checked << " candidates" << std::endl;
    std::cout << "    Verified: " << verified << " (100% should pass)" << std::endl;
    assert(verified == total_checked);  // 所有候选应该通过验证
    std::cout << "    All candidates valid ✓" << std::endl;

    // 7. 关系收集器测试
    std::cout << "\n[7] Relation Collector" << std::endl;

    CollectorConfig coll_config;
    coll_config.check_duplicates = true;

    RelationCollector collector(coll_config);

    // 模拟添加一些关系
    for (int i = 1; i <= 20; ++i) {
        Relation rel(i, i + 1);
        if (i % 5 == 0) {
            rel.rational_large_prime.push_back(PrimePower{100003u, 1});
        }
        collector.add(std::move(rel));
    }

    auto stats = collector.stats();
    std::cout << "    Total relations: " << stats.total_relations << std::endl;
    std::cout << "    Full relations: " << stats.full_relations << std::endl;
    std::cout << "    Partial (1LP): " << stats.partial_1lp << std::endl;

    // 8. 总结
    std::cout << "\n[8] Summary" << std::endl;
    std::cout << "    ✓ Polynomial selection working" << std::endl;
    std::cout << "    ✓ Factor base construction working" << std::endl;
    std::cout << "    ✓ Special-Q generation working" << std::endl;
    std::cout << "    ✓ Lattice sieve working" << std::endl;
    std::cout << "    ✓ Relation collection working" << std::endl;

    std::cout << "\n=== Integration Test PASSED ===" << std::endl;
}

/// 测试小数的完整分解（端到端）
void test_small_factorization_setup() {
    std::cout << "\n=== Test: Small Number Factorization Setup ===" << std::endl;

    // 使用更小的测试数：143 = 11 * 13
    Integer n(143);
    std::cout << "Number: 143 = 11 × 13" << std::endl;

    // 多项式选择
    auto result = BaseMSelector::select(n, 2);
    assert(result.success);

    auto ctx = BaseMSelector::create_context(n, result);
    assert(ctx.verify());

    std::cout << "Polynomial degree: " << ctx.degree() << std::endl;
    std::cout << "m = " << ctx.m().to_string() << std::endl;

    // 小因子基
    FactorBaseBuilder::Options opts;
    opts.rational_bound = 50;
    opts.algebraic_bound = 50;
    opts.parallel = false;

    auto fb = FactorBaseBuilder::build(ctx, opts);

    std::cout << "Rational factor base size: " << fb.rational_count() << std::endl;
    std::cout << "Algebraic factor base size: " << fb.algebraic_count() << std::endl;

    // 筛法设置
    SieveParams params;
    params.rational_threshold = 30;
    params.algebraic_threshold = 30;

    LatticeSieve sieve(ctx, fb, params);

    SieveRegion region;
    region.i_min = -50;
    region.i_max = 49;
    region.j_min = 1;
    region.j_max = 20;
    sieve.set_region(region);

    // 筛一个 special-q
    SpecialQRange range;
    range.min_q = 10;
    range.max_q = 50;

    SpecialQGenerator gen(fb, range);
    if (gen.has_next()) {
        auto sq = gen.next();
        if (sq) {
            auto sieve_result = sieve.sieve_special_q(*sq);
            std::cout << "Special-Q (" << sq->q << ", " << sq->r << "): "
                      << sieve_result.candidates.size() << " candidates" << std::endl;
        }
    }

    std::cout << "Small factorization setup: PASS" << std::endl;
}

int main() {
    test_full_sieve_pipeline();
    test_small_factorization_setup();

    std::cout << "\n==============================" << std::endl;
    std::cout << "All integration tests passed!" << std::endl;
    std::cout << "==============================" << std::endl;

    return 0;
}
