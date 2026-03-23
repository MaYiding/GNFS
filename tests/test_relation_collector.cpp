#include "gnfs/relation/collector.hpp"
#include "gnfs/util/safe_math.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

using namespace gnfs;
using namespace gnfs::relation;
using namespace gnfs::core;

void test_basic_add() {
    std::cout << "Testing basic add..." << std::endl;

    RelationCollector collector;

    // 创建一个关系 (gcd(123, 457) = 1)
    Relation rel(123, 457);
    rel.rational_factors.push_back(2);
    rel.algebraic_factors.push_back(3);

    bool added = collector.add(std::move(rel));
    assert(added);
    assert(collector.size() == 1);

    auto stats = collector.stats();
    assert(stats.total_relations == 1);
    assert(stats.full_relations == 1);

    std::cout << "  Basic add: PASS" << std::endl;
}

void test_duplicate_rejection() {
    std::cout << "Testing duplicate rejection..." << std::endl;

    CollectorConfig config;
    config.check_duplicates = true;

    RelationCollector collector(config);

    // 添加第一个关系
    Relation rel1(100, 201);  // gcd(100, 201) = 1

    bool added1 = collector.add(std::move(rel1));
    assert(added1);

    // 尝试添加重复
    Relation rel2(100, 201);

    bool added2 = collector.add(std::move(rel2));
    assert(!added2);  // 应该被拒绝

    assert(collector.size() == 1);

    auto stats = collector.stats();
    assert(stats.duplicates_rejected == 1);

    std::cout << "  Duplicate rejection: PASS" << std::endl;
}

void test_invalid_rejection() {
    std::cout << "Testing invalid rejection..." << std::endl;

    RelationCollector collector;

    // b = 0 是无效的
    Relation rel1(100, 0);

    bool added1 = collector.add(std::move(rel1));
    assert(!added1);

    // gcd(a, b) != 1 是无效的
    Relation rel2(100, 50);  // gcd(100, 50) = 50 != 1

    bool added2 = collector.add(std::move(rel2));
    assert(!added2);

    assert(collector.size() == 0);

    auto stats = collector.stats();
    assert(stats.invalid_rejected == 2);

    std::cout << "  Invalid rejection: PASS" << std::endl;
}

void test_partial_relations() {
    std::cout << "Testing partial relations..." << std::endl;

    RelationCollector collector;

    // 完全光滑关系
    Relation rel1(1, 2);
    collector.add(std::move(rel1));

    // 1LP 关系
    Relation rel2(3, 4);
    rel2.rational_large_prime.push_back(PrimePower{1000003, 1});
    collector.add(std::move(rel2));

    // 2LP 关系
    Relation rel3(5, 6);
    rel3.rational_large_prime.push_back(PrimePower{1000003, 1});
    rel3.algebraic_large_prime.push_back(PrimePower{1000033, 1});
    collector.add(std::move(rel3));

    auto stats = collector.stats();
    assert(stats.total_relations == 3);
    assert(stats.full_relations == 1);
    assert(stats.partial_1lp == 1);
    assert(stats.partial_2lp == 1);

    std::cout << "  Partial relations: PASS" << std::endl;
}

void test_batch_add() {
    std::cout << "Testing batch add..." << std::endl;

    RelationCollector collector;

    std::vector<Relation> batch;
    for (int i = 1; i <= 10; ++i) {
        Relation rel(i, i + 1);
        batch.push_back(std::move(rel));
    }

    size_t added = collector.add_batch(std::move(batch));
    assert(added == 10);
    assert(collector.size() == 10);

    std::cout << "  Batch add: PASS" << std::endl;
}

void test_save_load() {
    std::cout << "Testing save/load..." << std::endl;

    const std::string test_file = "test_relations.bin";

    // 创建并保存
    {
        RelationCollector collector;

        for (int i = 1; i <= 5; ++i) {
            Relation rel(i * 10, i * 10 + 1);
            rel.rational_factors.push_back(static_cast<uint32_t>(i));
            collector.add(std::move(rel));
        }

        bool saved = collector.save(test_file);
        assert(saved);
    }

    // 加载并验证
    {
        RelationCollector collector;
        bool loaded = collector.load(test_file);
        assert(loaded);
        assert(collector.size() == 5);

        auto stats = collector.stats();
        assert(stats.total_relations == 5);
    }

    // 清理
    std::filesystem::remove(test_file);

    std::cout << "  Save/load: PASS" << std::endl;
}

void test_concurrent_add() {
    std::cout << "Testing concurrent add..." << std::endl;

    CollectorConfig config;
    config.check_duplicates = true;

    RelationCollector collector(config);

    const int num_threads = 4;
    const int per_thread = 100;

    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&collector, t]() {
            for (int i = 0; i < per_thread; ++i) {
                int64_t a = t * per_thread + i;
                uint64_t b = static_cast<uint64_t>(t * per_thread + i + 1);
                // 确保 gcd(a, b) = 1
                while (std::gcd(util::safe_abs(a), b) != 1) {
                    ++b;
                }
                Relation rel(a, b);
                collector.add(std::move(rel));
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // 应该有接近 num_threads * per_thread 个关系（可能有一些被 gcd 调整）
    assert(collector.size() > 0);

    std::cout << "  Concurrent add: PASS (" << collector.size() << " relations)" << std::endl;
}

void test_merge() {
    std::cout << "Testing merge..." << std::endl;

    CollectorConfig config;
    config.check_duplicates = true;

    RelationCollector collector1(config);
    RelationCollector collector2(config);

    // 添加到第一个收集器
    for (int i = 1; i <= 5; ++i) {
        Relation rel(i, i + 1);
        collector1.add(std::move(rel));
    }

    // 添加到第二个收集器（有重叠）
    for (int i = 3; i <= 8; ++i) {
        Relation rel(i, i + 1);
        collector2.add(std::move(rel));
    }

    // 合并
    size_t merged = collector1.merge(collector2);

    // 应该只添加 6, 7, 8（3, 4, 5 是重复的）
    assert(merged == 3);
    assert(collector1.size() == 8);

    std::cout << "  Merge: PASS" << std::endl;
}

void test_filter_duplicates() {
    std::cout << "Testing filter_duplicates..." << std::endl;

    std::vector<Relation> relations;

    for (int i = 0; i < 10; ++i) {
        Relation rel(i % 5, (i % 5) + 1);  // 会有重复
        relations.push_back(std::move(rel));
    }

    auto filtered = filter_duplicates(std::move(relations));
    assert(filtered.size() == 5);

    std::cout << "  Filter duplicates: PASS" << std::endl;
}

void test_sort_relations() {
    std::cout << "Testing sort_relations..." << std::endl;

    std::vector<Relation> relations;

    // 添加无序关系
    for (int b : {5, 3, 7, 1}) {
        for (int a : {10, 5, 15}) {
            // 确保 gcd(a, b) = 1
            if (std::gcd(a, b) == 1) {
                Relation rel(a, b);
                relations.push_back(std::move(rel));
            }
        }
    }

    sort_relations(relations);

    // 验证排序
    for (size_t i = 1; i < relations.size(); ++i) {
        const auto& prev = relations[i - 1];
        const auto& curr = relations[i];

        // 先按 b 排序，再按 a 排序
        assert(prev.ab().b < curr.ab().b || (prev.ab().b == curr.ab().b && prev.ab().a <= curr.ab().a));
    }

    std::cout << "  Sort relations: PASS (" << relations.size() << " relations)" << std::endl;
}

void test_callback() {
    std::cout << "Testing callback..." << std::endl;

    RelationCollector collector;

    int callback_count = 0;
    collector.set_callback([&callback_count](const Relation&) {
        ++callback_count;
    });

    for (int i = 1; i <= 5; ++i) {
        Relation rel(i, i + 1);
        collector.add(std::move(rel));
    }

    assert(callback_count == 5);

    std::cout << "  Callback: PASS" << std::endl;
}

void test_callback_no_deadlock() {
    std::cout << "Testing callback does not deadlock when calling collector methods..." << std::endl;

    RelationCollector collector;

    // This callback calls size() and stats() on the collector.
    // Before the fix, this would deadlock because add() held the
    // non-recursive mutex while invoking the callback.
    size_t last_size = 0;
    size_t callback_count = 0;
    collector.set_callback([&](const Relation&) {
        // These calls acquire mutex_ — would deadlock if callback
        // were invoked inside the lock.
        last_size = collector.size();
        auto st = collector.stats();
        assert(st.total_relations == last_size);
        ++callback_count;
    });

    for (int i = 1; i <= 5; ++i) {
        Relation rel(i, i + 1);
        collector.add(std::move(rel));
    }

    assert(callback_count == 5);
    assert(last_size == 5);

    std::cout << "  Callback no-deadlock: PASS" << std::endl;
}

int main() {
    std::cout << "=== Relation Collector Tests ===" << std::endl;

    test_basic_add();
    test_duplicate_rejection();
    test_invalid_rejection();
    test_partial_relations();
    test_batch_add();
    test_save_load();
    test_concurrent_add();
    test_merge();
    test_filter_duplicates();
    test_sort_relations();
    test_callback();
    test_callback_no_deadlock();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
