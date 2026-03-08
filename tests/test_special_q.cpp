#include "gnfs/sieve/special_q.hpp"
#include "gnfs/factor_base/builder.hpp"
#include "gnfs/polynomial/base_m.hpp"

#include <cassert>
#include <iostream>

using namespace gnfs;
using namespace gnfs::sieve;
using namespace gnfs::factor_base;
using namespace gnfs::polynomial;
using namespace gnfs::core;

// 测试用的半素数
const char* test_n = "1000036000099";

void test_special_q_generator() {
    std::cout << "Testing SpecialQGenerator..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);

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
        if (!sq) break;
        collected.push_back(*sq);
    }

    // 验证
    assert(!collected.empty());

    // 所有 q 应该在范围内
    for (const auto& sq : collected) {
        assert(sq.q >= range.min_q);
        assert(sq.q <= range.max_q);
        assert(sq.is_valid());
    }

    // q 应该是递增的（因子基是排序的）
    for (size_t i = 1; i < collected.size(); ++i) {
        assert(collected[i].q >= collected[i-1].q);
    }

    std::cout << "  Generator: PASS (" << collected.size() << " special-q in range)" << std::endl;
}

void test_special_q_batch() {
    std::cout << "Testing SpecialQBatch..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);

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

    assert(batch1.size() <= batch_size);
    assert(!batch1.empty());

    // 验证批次内容
    for (const auto& sq : batch1) {
        assert(sq.is_valid());
        assert(sq.q >= range.min_q);
    }

    // 获取更多批次
    auto batch2 = SpecialQBatch::fetch(gen, batch_size);

    // 第二批的第一个应该大于等于第一批的最后一个
    if (!batch2.empty()) {
        assert(batch2[0].q >= batch1[batch1.size() - 1].q);
    }

    std::cout << "  Batch: PASS (batch1=" << batch1.size()
              << ", batch2=" << batch2.size() << ")" << std::endl;
}

void test_estimate_count() {
    std::cout << "Testing estimate_special_q_count..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);

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
        if (gen.next()) ++actual;
    }

    assert(estimated == actual);

    std::cout << "  Estimate: PASS (count=" << estimated << ")" << std::endl;
}

void test_generator_reset() {
    std::cout << "Testing generator reset..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);

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
    assert(sq1 && sq2 && sq3);

    uint32_t saved_index = gen.current_index();

    // 继续获取
    auto sq4 = gen.next();
    assert(sq4);

    // 重置
    gen.reset_to(saved_index);

    // 现在应该再次获得 sq4
    auto sq4_again = gen.next();
    assert(sq4_again);
    assert(sq4->q == sq4_again->q);
    assert(sq4->r == sq4_again->r);

    std::cout << "  Reset: PASS" << std::endl;
}

void test_range_selector() {
    std::cout << "Testing SpecialQRangeSelector..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);

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
    assert(range.min_q == selector.algebraic_bound);

    // max_q 应该大于 min_q
    assert(range.max_q > range.min_q);

    std::cout << "  RangeSelector: PASS (range=[" << range.min_q
              << ", " << range.max_q << "])" << std::endl;
}

void test_empty_range() {
    std::cout << "Testing empty range..." << std::endl;

    Integer n(test_n);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);

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
    assert(!gen.has_next() || gen.next() == std::nullopt);

    std::cout << "  Empty range: PASS" << std::endl;
}

int main() {
    std::cout << "=== Special-Q Tests ===" << std::endl;

    test_special_q_generator();
    test_special_q_batch();
    test_estimate_count();
    test_generator_reset();
    test_range_selector();
    test_empty_range();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
