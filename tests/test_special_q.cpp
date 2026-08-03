#include "gnfs/core/params.hpp"
#include "gnfs/factor_base/builder.hpp"
#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/sieve/special_q.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gnfs;
using namespace gnfs::sieve;
using namespace gnfs::factor_base;
using namespace gnfs::polynomial;
using namespace gnfs::core;
using core::GNFSParams;

[[noreturn]] void check_failed(const char* expression, int line) {
    throw std::runtime_error(std::string("CHECK failed at line ") + std::to_string(line) + ": " +
                             expression);
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition))                                                                          \
            check_failed(#condition, __LINE__);                                                    \
    } while (false)

// 测试用的半素数
const char* test_n = "1000036000099";

void test_special_q_generator() {
    std::cout << "Testing SpecialQGenerator..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    CHECK(result.success);

    auto ctx = BaseMSelector::create_context(n, result);

    // 构建因子基
    FactorBaseBuilder::Options opts;
    opts.rational_bound = 10000;
    opts.algebraic_bound = 10000;
    opts.parallel = false;

    auto fb = FactorBaseBuilder::build(ctx, opts);

    // 创建生成器
    SpecialQRange range;
    range.min_q = 1000;
    range.max_q = 5000;

    SpecialQGenerator gen(fb, range);

    // 收集所有 special-q
    std::vector<SpecialQ> collected;
    while (gen.has_next()) {
        auto sq = gen.next();
        if (!sq)
            break;
        collected.push_back(*sq);
    }

    // 验证
    CHECK(!collected.empty());

    // 所有 q 应该在范围内
    for (const auto& sq : collected) {
        CHECK(sq.q >= range.min_q);
        CHECK(sq.q <= range.max_q);
        CHECK(sq.is_valid());
    }

    // q 应该是递增的（因子基是排序的）
    for (size_t i = 1; i < collected.size(); ++i) {
        CHECK(collected[i].q >= collected[i - 1].q);
    }

    std::cout << "  Generator: PASS (" << collected.size() << " special-q in range)" << std::endl;
}

void test_special_q_batch() {
    std::cout << "Testing SpecialQBatch..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    CHECK(result.success);

    auto ctx = BaseMSelector::create_context(n, result);

    FactorBaseBuilder::Options opts;
    opts.rational_bound = 10000;
    opts.algebraic_bound = 10000;
    opts.parallel = false;

    auto fb = FactorBaseBuilder::build(ctx, opts);

    SpecialQRange range;
    range.min_q = 100;
    range.max_q = 10000;

    SpecialQGenerator gen(fb, range);

    // 批量获取
    const size_t batch_size = 50;
    auto batch1 = SpecialQBatch::fetch(gen, batch_size);

    CHECK(batch1.size() <= batch_size);
    CHECK(!batch1.empty());

    // 验证批次内容
    for (const auto& sq : batch1) {
        CHECK(sq.is_valid());
        CHECK(sq.q >= range.min_q);
    }

    // 获取更多批次
    auto batch2 = SpecialQBatch::fetch(gen, batch_size);

    // 第二批的第一个应该大于等于第一批的最后一个
    if (!batch2.empty()) {
        CHECK(batch2[0].q >= batch1[batch1.size() - 1].q);
    }

    std::cout << "  Batch: PASS (batch1=" << batch1.size() << ", batch2=" << batch2.size() << ")"
              << std::endl;
}

void test_estimate_count() {
    std::cout << "Testing estimate_special_q_count..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    CHECK(result.success);

    auto ctx = BaseMSelector::create_context(n, result);

    FactorBaseBuilder::Options opts;
    opts.rational_bound = 10000;
    opts.algebraic_bound = 10000;
    opts.parallel = false;

    auto fb = FactorBaseBuilder::build(ctx, opts);

    // 估计 [1000, 5000] 范围内的数量
    size_t estimated = estimate_special_q_count(fb, 1000, 5000);

    // 实际计数
    SpecialQRange range;
    range.min_q = 1000;
    range.max_q = 5000;

    SpecialQGenerator gen(fb, range);
    size_t actual = 0;
    while (gen.has_next()) {
        if (gen.next())
            ++actual;
    }

    CHECK(estimated == actual);

    std::cout << "  Estimate: PASS (count=" << estimated << ")" << std::endl;
}

void test_generator_reset() {
    std::cout << "Testing generator reset..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    CHECK(result.success);

    auto ctx = BaseMSelector::create_context(n, result);

    FactorBaseBuilder::Options opts;
    opts.rational_bound = 5000;
    opts.algebraic_bound = 5000;
    opts.parallel = false;

    auto fb = FactorBaseBuilder::build(ctx, opts);

    SpecialQRange range;
    range.min_q = 100;
    range.max_q = 5000;

    SpecialQGenerator gen(fb, range);

    // 获取几个
    auto sq1 = gen.next();
    auto sq2 = gen.next();
    auto sq3 = gen.next();
    CHECK(sq1 && sq2 && sq3);

    uint32_t saved_index = gen.current_index();

    // 继续获取
    auto sq4 = gen.next();
    CHECK(sq4);

    // 重置
    gen.reset_to(saved_index);

    // 现在应该再次获得 sq4
    auto sq4_again = gen.next();
    CHECK(sq4_again);
    CHECK(sq4->q == sq4_again->q);
    CHECK(sq4->r == sq4_again->r);

    std::cout << "  Reset: PASS" << std::endl;
}

FactorBase make_special_q_boundary_factor_base() {
    FactorBase fb;
    constexpr uint32_t projective = AlgebraicPrime::PROJECTIVE_ROOT;

    // Sorted by q, matching FactorBaseBuilder output. The fixture deliberately
    // places unusable roots at the beginning, between affine roots, and after
    // the final affine root. The two q=5 affine roots must both survive.
    fb.add_algebraic(3, projective, 1);
    fb.add_algebraic(5, 1, 1);
    fb.add_algebraic(5, 4, 1);
    fb.add_algebraic(5, projective, 1);
    fb.add_algebraic(7, 7, 1);
    fb.add_algebraic(11, 3, 1);
    fb.add_algebraic(13, projective, 1);
    fb.add_algebraic(13, 14, 1);
    fb.add_algebraic(17, 7, 1);
    fb.add_algebraic(19, projective, 1);
    fb.add_algebraic(23, 23, 1);
    fb.build_index();
    return fb;
}

void test_special_q_root_predicates() {
    std::cout << "Testing explicit Special-Q root predicates..." << std::endl;

    const AlgebraicPrime affine{5, 4, 1};
    const AlgebraicPrime projective{5, AlgebraicPrime::PROJECTIVE_ROOT, 1};
    const AlgebraicPrime equal_to_q{5, 5, 1};
    const AlgebraicPrime greater_than_q{5, 6, 1};

    CHECK(is_affine_special_q_root(affine));
    CHECK(!is_projective_special_q_root(affine));
    CHECK(!is_affine_special_q_root(projective));
    CHECK(is_projective_special_q_root(projective));
    CHECK(!is_affine_special_q_root(equal_to_q));
    CHECK(!is_projective_special_q_root(equal_to_q));
    CHECK(!is_affine_special_q_root(greater_than_q));
    CHECK(!is_projective_special_q_root(greater_than_q));

    const SpecialQ affine_sq{5, 4, 0};
    const SpecialQ projective_sq{5, AlgebraicPrime::PROJECTIVE_ROOT, 0};
    const SpecialQ malformed_sq{5, 5, 0};
    CHECK(affine_sq.is_affine());
    CHECK(!affine_sq.is_projective());
    CHECK(affine_sq.is_valid());
    CHECK(!projective_sq.is_affine());
    CHECK(projective_sq.is_projective());
    CHECK(!projective_sq.is_valid());
    CHECK(!malformed_sq.is_affine());
    CHECK(!malformed_sq.is_projective());
    CHECK(!malformed_sq.is_valid());

    std::cout << "  Root predicates: PASS" << std::endl;
}

void test_fail_closed_special_q_iteration() {
    std::cout << "Testing fail-closed affine Special-Q iteration..." << std::endl;

    auto fb = make_special_q_boundary_factor_base();
    SpecialQRange range;
    range.min_q = 3;
    range.max_q = 23;
    SpecialQGenerator gen(fb, range);

    CHECK(gen.has_next());
    auto first = gen.next();
    CHECK(first && first->index == 1 && first->q == 5 && first->r == 1);
    CHECK(gen.estimate_remaining() == 3);

    std::vector<uint32_t> indices{first->index};
    std::vector<uint32_t> roots{first->r};
    while (gen.has_next()) {
        auto sq = gen.next();
        CHECK(sq.has_value());
        CHECK(sq->is_valid());
        CHECK(sq->r < sq->q);
        indices.push_back(sq->index);
        roots.push_back(sq->r);
    }

    CHECK((indices == std::vector<uint32_t>{1, 2, 5, 8}));
    CHECK((roots == std::vector<uint32_t>{1, 4, 3, 7}));
    CHECK(gen.estimate_remaining() == 0);
    CHECK(gen.next() == std::nullopt);

    std::cout << "  Fail-closed iteration: PASS" << std::endl;
}

void test_fail_closed_special_q_reset_and_ranges() {
    std::cout << "Testing fail-closed Special-Q reset/ranges..." << std::endl;

    auto fb = make_special_q_boundary_factor_base();
    SpecialQRange full;
    full.min_q = 3;
    full.max_q = 23;
    SpecialQGenerator gen(fb, full);

    gen.reset_to(0);
    CHECK(gen.estimate_remaining() == 4);
    CHECK(gen.next()->index == 1);

    gen.reset_to(3);
    CHECK(gen.estimate_remaining() == 2);
    CHECK(gen.next()->index == 5);

    gen.reset_to(4);
    CHECK(gen.estimate_remaining() == 2);
    CHECK(gen.next()->index == 5);

    gen.reset_to(6);
    CHECK(gen.estimate_remaining() == 1);
    CHECK(gen.next()->index == 8);

    gen.reset_to(9);
    CHECK(!gen.has_next());
    CHECK(gen.estimate_remaining() == 0);
    CHECK(gen.next() == std::nullopt);

    SpecialQRange bounded;
    bounded.min_q = 5;
    bounded.max_q = 13;
    bounded.start_index = 0;
    bounded.end_index = 8;
    SpecialQGenerator bounded_gen(fb, bounded);
    CHECK(bounded_gen.estimate_remaining() == 3);
    auto bounded_batch = SpecialQBatch::fetch(bounded_gen, 10);
    CHECK(bounded_batch.size() == 3);
    CHECK(bounded_batch[0].index == 1);
    CHECK(bounded_batch[1].index == 2);
    CHECK(bounded_batch[2].index == 5);

    auto index_range = SpecialQRange::from_indices(0, 4);
    SpecialQGenerator indexed_gen(fb, index_range);
    CHECK(indexed_gen.estimate_remaining() == 2);
    auto indexed_batch = SpecialQBatch::fetch(indexed_gen, 10);
    CHECK(indexed_batch.size() == 2);
    CHECK(indexed_batch[0].index == 1);
    CHECK(indexed_batch[1].index == 2);

    std::cout << "  Fail-closed reset/ranges: PASS" << std::endl;
}

void test_fail_closed_special_q_count() {
    std::cout << "Testing fail-closed Special-Q counts..." << std::endl;

    auto fb = make_special_q_boundary_factor_base();
    CHECK(estimate_special_q_count(fb, 3, 23) == 4);
    CHECK(estimate_special_q_count(fb, 5, 5) == 2);
    CHECK(estimate_special_q_count(fb, 6, 13) == 1);
    CHECK(estimate_special_q_count(fb, 13, 16) == 0);
    CHECK(estimate_special_q_count(fb, 17, 23) == 1);

    std::cout << "  Fail-closed counts: PASS" << std::endl;
}

void test_range_selector() {
    std::cout << "Testing SpecialQRangeSelector..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    CHECK(result.success);

    auto ctx = BaseMSelector::create_context(n, result);

    FactorBaseBuilder::Options opts;
    opts.rational_bound = 10000;
    opts.algebraic_bound = 10000;
    opts.parallel = false;

    auto fb = FactorBaseBuilder::build(ctx, opts);

    SpecialQRangeSelector selector;
    selector.algebraic_bound = 10000;
    selector.target_relations = 100000;
    selector.relations_per_sq = 10.0;

    auto range = selector.select(fb);

    // min_q 应该等于 algebraic_bound
    CHECK(range.min_q == selector.algebraic_bound);

    // max_q 应该大于 min_q
    CHECK(range.max_q > range.min_q);

    std::cout << "  RangeSelector: PASS (range=[" << range.min_q << ", " << range.max_q << "])"
              << std::endl;
}

void test_empty_range() {
    std::cout << "Testing empty range..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    CHECK(result.success);

    auto ctx = BaseMSelector::create_context(n, result);

    FactorBaseBuilder::Options opts;
    opts.rational_bound = 1000;
    opts.algebraic_bound = 1000;
    opts.parallel = false;

    auto fb = FactorBaseBuilder::build(ctx, opts);

    // 范围超出因子基
    SpecialQRange range;
    range.min_q = 100000;
    range.max_q = 200000;

    SpecialQGenerator gen(fb, range);

    // 应该没有 special-q
    CHECK(!gen.has_next() || gen.next() == std::nullopt);

    std::cout << "  Empty range: PASS" << std::endl;
}

/// 回归测试：special-Q 范围应在因子基界以上
/// 修复前：special_q_min = algebraic_bound/5，大量 SQ 在 FB 内部浪费筛选效率
/// 修复后：Builder 支持 special_q_bound，构建 FB 以上的代数素数供 SQ 使用
void test_special_q_above_fb_bound() {
    std::cout << "Testing special-Q above FB bound (regression)..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    CHECK(result.success);

    auto ctx = BaseMSelector::create_context(n, result);

    uint32_t alg_bound = 5000;
    uint32_t sq_bound = 15000; // 3× algebraic_bound

    FactorBaseBuilder::Options opts;
    opts.rational_bound = alg_bound;
    opts.algebraic_bound = alg_bound;
    opts.special_q_bound = sq_bound;
    opts.parallel = false;

    auto fb = FactorBaseBuilder::build(ctx, opts);

    // (1) 总代数素数数量应大于筛选用数量
    size_t total = fb.algebraic_count();
    size_t sieve = fb.sieve_algebraic_count();
    std::cout << "  Sieve algebraic: " << sieve << ", Total algebraic: " << total << std::endl;
    CHECK(total > sieve); // SQ 范围提供了额外素数

    // (2) 筛选用素数都应 ≤ algebraic_bound
    const auto& algs = fb.algebraic();
    for (size_t i = 0; i < sieve; ++i) {
        CHECK(algs[i].p <= alg_bound);
    }

    // (3) 额外的 SQ 素数应 > algebraic_bound 且 ≤ special_q_bound
    for (size_t i = sieve; i < total; ++i) {
        CHECK(algs[i].p > alg_bound);
        CHECK(algs[i].p <= sq_bound);
    }

    // (4) SpecialQGenerator 应能从 SQ 范围找到素数
    SpecialQRange range;
    range.min_q = alg_bound + 1;
    range.max_q = sq_bound;

    SpecialQGenerator gen(fb, range);
    size_t sq_count = 0;
    uint32_t first_q = 0, last_q = 0;
    while (gen.has_next()) {
        auto sq = gen.next();
        if (!sq)
            break;
        CHECK(sq->q > alg_bound);
        CHECK(sq->q <= sq_bound);
        if (sq_count == 0)
            first_q = sq->q;
        last_q = sq->q;
        ++sq_count;
    }
    CHECK(sq_count > 0);

    std::cout << "  SQ range: [" << first_q << ", " << last_q << "], count=" << sq_count
              << std::endl;

    // (5) 验证 GNFSParams::compute 也设置了正确的 SQ 范围
    auto params = GNFSParams::compute(n.bit_length());
    CHECK(params.special_q_min > params.algebraic_bound);
    CHECK(params.special_q_max > params.special_q_min);
    std::cout << "  GNFSParams: alg_bound=" << params.algebraic_bound
              << ", sq_min=" << params.special_q_min << ", sq_max=" << params.special_q_max
              << std::endl;

    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== Special-Q Tests ===" << std::endl;

    test_special_q_generator();
    test_special_q_batch();
    test_estimate_count();
    test_generator_reset();
    test_special_q_root_predicates();
    test_fail_closed_special_q_iteration();
    test_fail_closed_special_q_reset_and_ranges();
    test_fail_closed_special_q_count();
    test_range_selector();
    test_empty_range();
    test_special_q_above_fb_bound();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
