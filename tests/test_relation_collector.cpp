// Force assert() to remain live even in Release builds. Several cases here
// embed side-effecting calls inside assert (e.g. assert(collector.add(...)));
// NDEBUG would otherwise strip both the check and the call, leaving phase-1
// setup unexecuted and phase-3 reads scanning uninitialized index entries —
// which surfaced on CI as "OOCRelationReader: corrupt record (truncated)".
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "gnfs/relation/collector.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/safe_math.hpp"
#include "gnfs/util/temp_path.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace gnfs;
using namespace gnfs::relation;
using namespace gnfs::core;

[[noreturn]] static void check_failed(const char* expression, int line) {
    throw std::runtime_error(std::string("CHECK failed at line ") + std::to_string(line) + ": " +
                             expression);
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition))                                                                          \
            check_failed(#condition, __LINE__);                                                    \
    } while (false)

static void check_stats_equal(const CollectorStats& actual, const CollectorStats& expected) {
    CHECK(actual.total_relations == expected.total_relations);
    CHECK(actual.full_relations == expected.full_relations);
    CHECK(actual.partial_1lp == expected.partial_1lp);
    CHECK(actual.partial_2lp == expected.partial_2lp);
    CHECK(actual.duplicates_rejected == expected.duplicates_rejected);
    CHECK(actual.invalid_rejected == expected.invalid_rejected);
    CHECK(actual.n_divisible_rejected == expected.n_divisible_rejected);
}

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
    Relation rel1(100, 201); // gcd(100, 201) = 1

    bool added1 = collector.add(std::move(rel1));
    assert(added1);

    // 尝试添加重复
    Relation rel2(100, 201);

    bool added2 = collector.add(std::move(rel2));
    assert(!added2); // 应该被拒绝

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
    Relation rel2(100, 50); // gcd(100, 50) = 50 != 1

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

void test_effective_large_prime_stats() {
    std::cout << "Testing effective large prime statistics..." << std::endl;

    RelationCollector collector;

    Relation even_exponent(7, 8);
    even_exponent.rational_large_prime.push_back(PrimePower{101, 0, 2});
    CHECK(collector.add(std::move(even_exponent)));

    Relation repeated_key(9, 10);
    repeated_key.algebraic_large_prime.push_back(PrimePower{103, 7, 1});
    repeated_key.algebraic_large_prime.push_back(PrimePower{103, 7, 1});
    CHECK(collector.add(std::move(repeated_key)));

    Relation three_lp(11, 12);
    three_lp.rational_large_prime.push_back(PrimePower{107, 0, 1});
    three_lp.rational_large_prime.push_back(PrimePower{109, 0, 1});
    three_lp.algebraic_large_prime.push_back(PrimePower{113, 5, 1});
    CHECK(collector.add(std::move(three_lp)));

    const auto stats = collector.stats();
    CHECK(stats.total_relations == 3);
    CHECK(stats.full_relations == 2);
    CHECK(stats.partial_1lp == 0);
    CHECK(stats.partial_2lp == 1);

    std::cout << "  Effective large prime statistics: PASS" << std::endl;
}

void test_batch_add() {
    std::cout << "Testing batch add..." << std::endl;

    RelationCollector collector;

    std::vector<Relation> batch;
    for (int i = 1; i <= 10; ++i) {
        Relation rel(i, static_cast<uint64_t>(i + 1));
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
            Relation rel(i * 10, static_cast<uint64_t>(i * 10 + 1));
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

    // 所有 400 个关系键唯一，应全部保留
    assert(collector.size() == 400);

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
        Relation rel(i, static_cast<uint64_t>(i + 1));
        collector1.add(std::move(rel));
    }

    // 添加到第二个收集器（有重叠）
    for (int i = 3; i <= 8; ++i) {
        Relation rel(i, static_cast<uint64_t>(i + 1));
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
        Relation rel(i % 5, static_cast<uint64_t>((i % 5) + 1)); // 会有重复
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
                Relation rel(a, static_cast<uint64_t>(b));
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
        assert(prev.ab().b < curr.ab().b ||
               (prev.ab().b == curr.ab().b && prev.ab().a <= curr.ab().a));
    }

    std::cout << "  Sort relations: PASS (" << relations.size() << " relations)" << std::endl;
}

void test_callback() {
    std::cout << "Testing callback..." << std::endl;

    RelationCollector collector;

    int callback_count = 0;
    collector.set_callback([&callback_count](const Relation&) { ++callback_count; });

    for (int i = 1; i <= 5; ++i) {
        Relation rel(i, static_cast<uint64_t>(i + 1));
        collector.add(std::move(rel));
    }

    assert(callback_count == 5);

    std::cout << "  Callback: PASS" << std::endl;
}

void test_callback_no_deadlock() {
    std::cout << "Testing callback does not deadlock when calling collector methods..."
              << std::endl;

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
        Relation rel(i, static_cast<uint64_t>(i + 1));
        collector.add(std::move(rel));
    }

    assert(callback_count == 5);
    assert(last_size == 5);

    std::cout << "  Callback no-deadlock: PASS" << std::endl;
}

// CLAUDE.md 强制约定:必须拒绝 gcd(a-bm, N)>1 的关系。
// 该约定在没 set_polynomial_context 时退化为旧行为(只查 b/gcd(a,b)),
// 但一旦调用 set_polynomial_context(n, m) 后,add/load/merge 三条路径
// 都必须拒绝退化关系。这是测试该回归保护。
void test_n_divisibility_rejection() {
    std::cout << "Testing N-divisibility rejection (CLAUDE.md mandate)..." << std::endl;

    // N = 143 = 11 × 13, m = 12. (a - b*m) mod N == 0 当 a ≡ 12·b (mod 143)。
    // 取 (a, b) = (12, 1):a - bm = 12 - 12 = 0 → gcd(0, 143) = 143 > 1。
    Integer n("143");
    Integer m("12");

    CollectorConfig config;
    config.check_duplicates = false;

    // (1) 未设置 polynomial context:退回旧行为,任何 b>0 且 gcd(a,b)=1 的关系通过
    {
        RelationCollector collector(config);
        Relation rel(12, 1);
        bool added = collector.add(std::move(rel));
        assert(added);
        auto st = collector.stats();
        assert(st.total_relations == 1);
        assert(st.n_divisible_rejected == 0);
    }

    // (2) 设置后:gcd(a-bm, N) > 1 的关系被拒
    {
        RelationCollector collector(config);
        collector.set_polynomial_context(n, m);

        // 退化关系 (12, 1):a - b*m = 0 → gcd(0, 143) = 143
        Relation bad_rel(12, 1);
        bool added = collector.add(std::move(bad_rel));
        assert(!added);
        auto st = collector.stats();
        assert(st.total_relations == 0);
        assert(st.n_divisible_rejected == 1);

        // 另一退化关系 (155, 1):155 - 12 = 143 → gcd(143, 143) = 143
        Relation bad_rel2(155, 1);
        added = collector.add(std::move(bad_rel2));
        assert(!added);
        st = collector.stats();
        assert(st.n_divisible_rejected == 2);

        // 正常关系 (5, 1):5 - 12 = -7, gcd(7, 143) = 1 → 接受
        Relation good_rel(5, 1);
        added = collector.add(std::move(good_rel));
        assert(added);
        st = collector.stats();
        assert(st.total_relations == 1);
        assert(st.n_divisible_rejected == 2);
    }

    std::cout << "  N-divisibility rejection: PASS" << std::endl;
}

// ──────────────────────────────────────────────────────────────────────────
// OOC mode tests (BACKLOG #11c — 50d Round 2 OOM defense)
// 验证 ooc_enabled 配置下 add/dedup/get/clear 行为, 以及 CLAUDE.md gcd(a-bm,N)
// 强制约束在 OOC 模式仍生效。
// ──────────────────────────────────────────────────────────────────────────

/// 生成测试唯一 OOC base path (pid + counter, 避免并发 / 上次未清理)
static std::string make_tmp_ooc_path(const std::string& label) {
    static int seq = 0;
    return gnfs::util::temp_path("gnfs_test_collector_ooc_" +
                                 std::to_string(gnfs::util::process_id()) + "_" +
                                 std::to_string(++seq) + "_" + label);
}

/// RAII OOC artifact cleanup
struct OOCArtifacts {
    std::string base;
    explicit OOCArtifacts(std::string b) : base(std::move(b)) {}
    ~OOCArtifacts() {
        std::remove((base + ".reldata").c_str());
        std::remove((base + ".relidx").c_str());
    }
};

void test_ooc_basic_add() {
    std::cout << "Testing OOC basic add..." << std::endl;
    auto path = make_tmp_ooc_path("basic_add");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;

    RelationCollector collector(config);
    for (int i = 1; i <= 5; ++i) {
        Relation rel(i * 10, static_cast<uint64_t>(i * 10 + 1));
        rel.rational_factors.push_back(static_cast<uint32_t>(i));
        bool added = collector.add(std::move(rel));
        assert(added);
    }

    assert(collector.size() == 5);
    auto stats = collector.stats();
    assert(stats.total_relations == 5);
    assert(stats.full_relations == 5);

    // get_relations 从盘 read_all → vector
    auto rels = collector.get_relations();
    assert(rels.size() == 5);
    for (size_t i = 0; i < rels.size(); ++i) {
        assert(rels[i].a == static_cast<int64_t>((i + 1) * 10));
        assert(rels[i].b == (i + 1) * 10 + 1);
        assert(rels[i].rational_factors.size() == 1);
        assert(rels[i].rational_factors[0] == i + 1);
    }

    std::cout << "  OOC basic add: PASS" << std::endl;
}

void test_ooc_duplicate_rejection() {
    std::cout << "Testing OOC duplicate rejection..." << std::endl;
    auto path = make_tmp_ooc_path("dup");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.check_duplicates = true;
    config.ooc_enabled = true;
    config.ooc_base_path = path;

    RelationCollector collector(config);

    Relation rel1(100, 201);
    assert(collector.add(std::move(rel1)));

    Relation rel2(100, 201); // 重复 (a,b)
    assert(!collector.add(std::move(rel2)));

    assert(collector.size() == 1); // OOC writer count = 1, dedup 拒绝第二个
    auto stats = collector.stats();
    assert(stats.duplicates_rejected == 1);

    auto rels = collector.get_relations();
    assert(rels.size() == 1);

    std::cout << "  OOC duplicate rejection: PASS" << std::endl;
}

void test_ooc_n_divisibility() {
    std::cout << "Testing OOC N-divisibility rejection (CLAUDE.md mandate)..." << std::endl;
    auto path = make_tmp_ooc_path("ndiv");
    OOCArtifacts cleanup(path);

    Integer n("143");
    Integer m("12");

    CollectorConfig config;
    config.check_duplicates = false;
    config.ooc_enabled = true;
    config.ooc_base_path = path;

    RelationCollector collector(config);
    collector.set_polynomial_context(n, m);

    // (12, 1): a - b*m = 0, gcd(0, 143) = 143 → reject
    Relation bad(12, 1);
    assert(!collector.add(std::move(bad)));
    auto st = collector.stats();
    assert(st.n_divisible_rejected == 1);

    // (5, 1): 5 - 12 = -7, gcd(7, 143) = 1 → accept
    Relation good(5, 1);
    assert(collector.add(std::move(good)));
    st = collector.stats();
    assert(st.total_relations == 1);
    assert(st.n_divisible_rejected == 1);

    auto rels = collector.get_relations();
    assert(rels.size() == 1);
    assert(rels[0].a == 5);
    assert(rels[0].b == 1);

    std::cout << "  OOC N-divisibility rejection: PASS" << std::endl;
}

void test_ooc_partial_relations() {
    std::cout << "Testing OOC partial relations (serialization round-trip)..." << std::endl;
    auto path = make_tmp_ooc_path("partial");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;

    RelationCollector collector(config);

    Relation full_rel(1, 2);
    collector.add(std::move(full_rel));

    Relation rel_1lp(3, 4);
    rel_1lp.rational_large_prime.push_back(PrimePower{1000003, 0, 1});
    collector.add(std::move(rel_1lp));

    Relation rel_2lp(5, 6);
    rel_2lp.rational_large_prime.push_back(PrimePower{1000003, 0, 1});
    rel_2lp.algebraic_large_prime.push_back(PrimePower{1000033, 17, 1});
    collector.add(std::move(rel_2lp));

    assert(collector.size() == 3);
    auto stats = collector.stats();
    assert(stats.full_relations == 1);
    assert(stats.partial_1lp == 1);
    assert(stats.partial_2lp == 1);

    auto rels = collector.get_relations();
    assert(rels.size() == 3);
    // 验证 LP 完整序列化往返
    assert(rels[1].rational_large_prime.size() == 1);
    assert(rels[1].rational_large_prime[0].p == 1000003);
    assert(rels[2].rational_large_prime[0].p == 1000003);
    assert(rels[2].algebraic_large_prime[0].p == 1000033);
    assert(rels[2].algebraic_large_prime[0].r == 17);

    std::cout << "  OOC partial relations: PASS" << std::endl;
}

void test_ooc_concurrent_add() {
    std::cout << "Testing OOC concurrent add (mutex-protected writer)..." << std::endl;
    auto path = make_tmp_ooc_path("concurrent");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.check_duplicates = true;
    config.ooc_enabled = true;
    config.ooc_base_path = path;

    RelationCollector collector(config);

    const int num_threads = 4;
    const int per_thread = 100;
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&collector, t]() {
            for (int i = 0; i < per_thread; ++i) {
                int64_t a = t * per_thread + i;
                uint64_t b = static_cast<uint64_t>(t * per_thread + i + 1);
                while (std::gcd(util::safe_abs(a), b) != 1) {
                    ++b;
                }
                Relation rel(a, b);
                collector.add(std::move(rel));
            }
        });
    }
    for (auto& th : threads)
        th.join();

    // 全部 400 个 (a,b) 唯一, 都应被接受
    assert(collector.size() == 400);

    auto rels = collector.get_relations();
    assert(rels.size() == 400);

    std::cout << "  OOC concurrent add: PASS (" << collector.size() << " relations on disk)"
              << std::endl;
}

void test_ooc_clear_recycle() {
    std::cout << "Testing OOC clear() recycles writer + deletes files..." << std::endl;
    auto path = make_tmp_ooc_path("clear");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;

    RelationCollector collector(config);
    for (int i = 1; i <= 3; ++i) {
        Relation rel(i, static_cast<uint64_t>(i + 1));
        collector.add(std::move(rel));
    }
    assert(collector.size() == 3);

    collector.clear();
    assert(collector.size() == 0); // OOC writer count reset to 0 after recycle
    auto stats = collector.stats();
    assert(stats.total_relations == 0);

    // 重新可用: 加新 relations
    for (int i = 100; i <= 102; ++i) {
        Relation rel(i, static_cast<uint64_t>(i + 1));
        collector.add(std::move(rel));
    }
    assert(collector.size() == 3);

    auto rels = collector.get_relations();
    assert(rels.size() == 3);
    assert(rels[0].a == 100);

    std::cout << "  OOC clear() recycle: PASS" << std::endl;
}

void test_ooc_empty_base_path_rejected() {
    std::cout << "Testing OOC rejects empty base_path..." << std::endl;

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = ""; // empty → ctor 必须抛

    bool threw = false;
    try {
        RelationCollector collector(config);
        (void)collector;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "  OOC empty base_path rejected: PASS" << std::endl;
}

void test_ooc_legacy_save_load_disabled() {
    std::cout << "Testing OOC save/load legacy methods disabled..." << std::endl;
    auto path = make_tmp_ooc_path("legacy");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;

    RelationCollector collector(config);
    Relation rel(1, 2);
    collector.add(std::move(rel));

    // legacy save / load 在 OOC 模式必须 return false
    assert(!collector.save(gnfs::util::temp_path("unused.bin")));
    assert(!collector.load(gnfs::util::temp_path("unused.bin")));

    std::cout << "  OOC legacy save/load disabled: PASS" << std::endl;
}

static Relation make_snapshot_relation(int index) {
    const int64_t a = static_cast<int64_t>(2 * index + 1);
    const uint64_t b = static_cast<uint64_t>(2 * index + 2);
    Relation relation(a, b);
    relation.rational_factors.push_back(static_cast<uint32_t>(100 + index));
    return relation;
}

void test_ooc_snapshot_append_snapshot_finalize() {
    std::cout << "Testing OOC snapshot -> append -> snapshot -> finalize..." << std::endl;
    auto path = make_tmp_ooc_path("snapshot_append");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;
    RelationCollector collector(config);

    for (int i = 0; i < 3; ++i) {
        CHECK(collector.add(make_snapshot_relation(i)));
    }
    const auto first = collector.snapshot_relations();
    CHECK(first.size() == 3);
    CHECK(first[0].a == 1);
    CHECK(first[2].a == 5);

    for (int i = 3; i < 5; ++i) {
        CHECK(collector.add(make_snapshot_relation(i)));
    }
    const auto second = collector.snapshot_relations();
    CHECK(second.size() == 5);
    CHECK(second[3].a == 7);
    CHECK(second[4].a == 9);
    CHECK(first.size() == 3); // prior materialized prefix remains stable

    const auto finalized = collector.finalize_relations();
    CHECK(finalized.size() == 5);
    for (size_t i = 0; i < finalized.size(); ++i) {
        CHECK(finalized[i].a == static_cast<int64_t>(2 * i + 1));
        CHECK(finalized[i].rational_factors.size() == 1);
    }

    const auto stats_before_rejection = collector.stats();
    Relation pending_add = make_snapshot_relation(10);
    bool write_after_finalize_threw = false;
    try {
        (void)collector.add(std::move(pending_add));
    } catch (const std::logic_error&) {
        write_after_finalize_threw = true;
    }
    CHECK(write_after_finalize_threw);

    RelationCollector merge_source;
    CHECK(merge_source.add(make_snapshot_relation(11)));
    bool merge_after_finalize_threw = false;
    try {
        (void)collector.merge(merge_source);
    } catch (const std::logic_error&) {
        merge_after_finalize_threw = true;
    }
    CHECK(merge_after_finalize_threw);
    CHECK(collector.size() == 5);
    check_stats_equal(collector.stats(), stats_before_rejection);

    std::cout << "  OOC appendable snapshots: PASS" << std::endl;
}

void test_ooc_checkpoint_requires_explicit_resume() {
    std::cout << "Testing OOC checkpoint mutation rejection and resume..." << std::endl;
    auto path = make_tmp_ooc_path("checkpoint_resume");
    OOCArtifacts cleanup(path);
    auto foreign_path = make_tmp_ooc_path("checkpoint_foreign");
    OOCArtifacts foreign_cleanup(foreign_path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;
    RelationCollector collector(config);
    CHECK(collector.add(make_snapshot_relation(0)));

    CollectorConfig foreign_config;
    foreign_config.ooc_enabled = true;
    foreign_config.ooc_base_path = foreign_path;
    RelationCollector foreign(foreign_config);
    CHECK(foreign.add(make_snapshot_relation(20)));

    RelationCollector merge_source;
    CHECK(merge_source.add(make_snapshot_relation(2)));
    Relation pending_add = make_snapshot_relation(1);

    const auto stats_before_rejection = collector.stats();
    const auto descriptor = collector.checkpoint_ooc();
    const auto foreign_descriptor = foreign.checkpoint_ooc();

    auto stale_descriptor = descriptor;
    ++stale_descriptor.generation;
    bool stale_rejected = false;
    try {
        collector.resume_ooc(stale_descriptor);
    } catch (const std::invalid_argument&) {
        stale_rejected = true;
    }
    CHECK(stale_rejected);

    bool foreign_rejected = false;
    try {
        collector.resume_ooc(foreign_descriptor);
    } catch (const std::invalid_argument&) {
        foreign_rejected = true;
    }
    CHECK(foreign_rejected);

    bool add_while_suspended_threw = false;
    try {
        (void)collector.add(std::move(pending_add));
    } catch (const std::logic_error&) {
        add_while_suspended_threw = true;
    }
    CHECK(add_while_suspended_threw);

    bool merge_while_suspended_threw = false;
    try {
        (void)collector.merge(merge_source);
    } catch (const std::logic_error&) {
        merge_while_suspended_threw = true;
    }
    CHECK(merge_while_suspended_threw);
    CHECK(collector.size() == 1);
    check_stats_equal(collector.stats(), stats_before_rejection);

    // The rejected operations must not poison seen_: both exact keys remain
    // acceptable after the matching descriptor resumes the writer.
    collector.resume_ooc(descriptor);
    CHECK(collector.add(std::move(pending_add)));
    CHECK(collector.merge(merge_source) == 1);
    CHECK(collector.snapshot_relations().size() == 3);
    collector.finalize_ooc();

    // A suspended writer can be finalized directly and repeatedly.
    foreign.finalize_ooc();
    foreign.finalize_ooc();
    OOCRelationReader foreign_reader(foreign_path);
    CHECK(foreign_reader.count() == 1);

    std::cout << "  OOC explicit checkpoint lifecycle: PASS" << std::endl;
}

void test_ooc_failed_state_rejects_mutation() {
    std::cout << "Testing OOC failed-state mutation rejection..." << std::endl;
    auto path = make_tmp_ooc_path("failed_state");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;
    RelationCollector collector(config);
    CHECK(collector.add(make_snapshot_relation(0)));

    RelationCollector merge_source;
    CHECK(merge_source.add(make_snapshot_relation(2)));
    const auto stats_before_failure = collector.stats();
    const auto descriptor = collector.checkpoint_ooc();

    // checkpoint_ooc closed both handles, so removing this temp artifact is a
    // deterministic and isolated way to make resume fail and enter Failed.
    CHECK(std::filesystem::remove(path + ".reldata"));
    bool resume_failed = false;
    try {
        collector.resume_ooc(descriptor);
    } catch (const std::runtime_error&) {
        resume_failed = true;
    }
    CHECK(resume_failed);

    Relation pending_add = make_snapshot_relation(1);
    bool add_failed = false;
    try {
        (void)collector.add(std::move(pending_add));
    } catch (const std::logic_error&) {
        add_failed = true;
    }
    CHECK(add_failed);

    bool merge_failed = false;
    try {
        (void)collector.merge(merge_source);
    } catch (const std::logic_error&) {
        merge_failed = true;
    }
    CHECK(merge_failed);
    CHECK(collector.size() == 1);
    check_stats_equal(collector.stats(), stats_before_failure);

    std::cout << "  OOC failed-state mutation rejection: PASS" << std::endl;
}

void test_ooc_snapshot_integrity_failure_fails_closed() {
    std::cout << "Testing OOC snapshot integrity failure fails closed..." << std::endl;
    auto path = make_tmp_ooc_path("snapshot_integrity_failure");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;
    RelationCollector collector(config);
    CHECK(collector.add(make_snapshot_relation(0)));

    RelationCollector merge_source;
    CHECK(merge_source.add(make_snapshot_relation(2)));
    const auto stats_before_failure = collector.stats();
    const auto descriptor = collector.checkpoint_ooc();

    // Compact records start with a/b, followed by rational_factors count.
    // Corrupt that count while checkpoint_ooc has both handles closed, then
    // resume so snapshot_relations itself detects the untrusted prefix.
    {
        std::fstream data(path + ".reldata", std::ios::in | std::ios::out | std::ios::binary);
        CHECK(static_cast<bool>(data));
        const uint32_t corrupt_count = std::numeric_limits<uint32_t>::max();
        data.seekp(16);
        data.write(reinterpret_cast<const char*>(&corrupt_count), sizeof(corrupt_count));
        data.flush();
        CHECK(static_cast<bool>(data));
    }
    collector.resume_ooc(descriptor);

    bool snapshot_failed = false;
    try {
        (void)collector.snapshot_relations();
    } catch (const std::runtime_error&) {
        snapshot_failed = true;
    }
    CHECK(snapshot_failed);

    Relation pending_add = make_snapshot_relation(1);
    bool add_failed = false;
    try {
        (void)collector.add(std::move(pending_add));
    } catch (const std::logic_error&) {
        add_failed = true;
    }
    CHECK(add_failed);

    bool merge_failed = false;
    try {
        (void)collector.merge(merge_source);
    } catch (const std::logic_error&) {
        merge_failed = true;
    }
    CHECK(merge_failed);
    CHECK(collector.size() == 1);
    check_stats_equal(collector.stats(), stats_before_failure);

    std::cout << "  OOC snapshot integrity fail-closed: PASS" << std::endl;
}

void test_ooc_empty_and_repeated_snapshot() {
    std::cout << "Testing OOC empty and repeated snapshots..." << std::endl;
    auto path = make_tmp_ooc_path("snapshot_repeat");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;
    RelationCollector collector(config);

    CHECK(collector.snapshot_relations().empty());
    CHECK(collector.snapshot_relations().empty());

    CHECK(collector.add(make_snapshot_relation(0)));
    const auto first = collector.snapshot_relations();
    const auto repeated = collector.snapshot_relations();
    CHECK(first.size() == 1);
    CHECK(repeated.size() == 1);
    CHECK(first[0].a == repeated[0].a);
    CHECK(first[0].b == repeated[0].b);

    CHECK(collector.add(make_snapshot_relation(1)));
    const auto finalized = collector.get_relations();
    CHECK(finalized.size() == 2);

    std::cout << "  OOC empty/repeated snapshots: PASS" << std::endl;
}

void test_ooc_writer_finalize_state() {
    std::cout << "Testing OOC writer explicit finalize state..." << std::endl;
    auto path = make_tmp_ooc_path("writer_finalize_state");
    OOCArtifacts cleanup(path);

    OOCRelationWriter writer(path);
    CHECK(writer.state() == OOCWriterState::Open);
    CHECK(writer.write(make_snapshot_relation(0)) == 0);

    const auto first = writer.finalize();
    const auto repeated = writer.finalize();
    CHECK(first == repeated);
    CHECK(first.count == 1);
    CHECK(writer.state() == OOCWriterState::Finalized);

    bool threw = false;
    try {
        (void)writer.write(make_snapshot_relation(1));
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(writer.count() == 1);

    OOCRelationReader reader(path);
    CHECK(reader.count() == 1);
    CHECK(reader.read(0).a == 1);

    std::cout << "  OOC explicit finalize state: PASS" << std::endl;
}

void test_ooc_reader_rejects_corrupt_variable_lengths() {
    std::cout << "Testing OOC reader corrupt variable lengths..." << std::endl;
    auto path = make_tmp_ooc_path("corrupt_variable_lengths");
    OOCArtifacts cleanup(path);

    OOCRelationWriter writer(path);
    CHECK(writer.write(Relation(1, 2)) == 0);
    CHECK(writer.finalize().count == 1);

    // A relation with empty vectors is laid out as a/b followed by the five
    // uint32_t count fields. Exercise the shared checked-count path for every
    // variable-length field, restoring each field before corrupting the next.
    constexpr std::array<std::streamoff, 5> count_offsets = {16, 20, 24, 28, 32};
    const auto overwrite_count = [&](std::streamoff offset, uint32_t value) {
        std::fstream data(path + ".reldata", std::ios::in | std::ios::out | std::ios::binary);
        CHECK(static_cast<bool>(data));
        data.seekp(offset);
        data.write(reinterpret_cast<const char*>(&value), sizeof(value));
        data.flush();
        CHECK(static_cast<bool>(data));
    };

    for (const std::streamoff offset : count_offsets) {
        overwrite_count(offset, std::numeric_limits<uint32_t>::max());

        bool rejected = false;
        try {
            OOCRelationReader reader(path);
            (void)reader.read(0);
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("count exceeds limit") != std::string::npos;
        }
        CHECK(rejected);

        overwrite_count(offset, 0);
    }

    OOCRelationReader reader(path);
    CHECK(reader.read(0).a == 1);

    std::cout << "  OOC corrupt variable lengths: PASS" << std::endl;
}

void test_ooc_reader_rejects_trailing_bytes() {
    std::cout << "Testing OOC reader trailing bytes..." << std::endl;
    auto path = make_tmp_ooc_path("trailing_bytes");
    OOCArtifacts cleanup(path);

    OOCRelationWriter writer(path);
    CHECK(writer.write(Relation(1, 2)) == 0);
    const auto descriptor = writer.finalize();

    {
        std::ofstream data(path + ".reldata", std::ios::app | std::ios::binary);
        CHECK(static_cast<bool>(data));
        const uint8_t trailing = 0xA5;
        data.write(reinterpret_cast<const char*>(&trailing), sizeof(trailing));
        data.flush();
        CHECK(static_cast<bool>(data));
    }
    {
        std::fstream index(path + ".relidx", std::ios::in | std::ios::out | std::ios::binary);
        CHECK(static_cast<bool>(index));
        const uint64_t corrupt_end = descriptor.data_end + 1;
        index.seekp(24); // header + offset_0; overwrite the final sentinel
        index.write(reinterpret_cast<const char*>(&corrupt_end), sizeof(corrupt_end));
        index.flush();
        CHECK(static_cast<bool>(index));
    }

    bool rejected = false;
    try {
        OOCRelationReader reader(path);
        (void)reader.read(0);
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("trailing bytes") != std::string::npos;
    }
    CHECK(rejected);

    std::cout << "  OOC trailing bytes: PASS" << std::endl;
}

void test_ooc_prefix_reader_rejects_bad_descriptor_and_offsets() {
    std::cout << "Testing OOC prefix reader validation..." << std::endl;
    auto path = make_tmp_ooc_path("prefix_validation");
    OOCArtifacts cleanup(path);

    OOCRelationWriter writer(path);
    CHECK(writer.write(make_snapshot_relation(0)) == 0);
    CHECK(writer.write(make_snapshot_relation(1)) == 1);
    const auto descriptor = writer.checkpoint_prefix();
    CHECK(writer.state() == OOCWriterState::Suspended);

    bool ordinary_reader_threw = false;
    try {
        OOCRelationReader reader(path);
        (void)reader;
    } catch (const std::runtime_error&) {
        ordinary_reader_threw = true;
    }
    CHECK(ordinary_reader_threw);

    auto bad_end = descriptor;
    ++bad_end.data_end;
    bool bad_end_threw = false;
    try {
        OOCRelationPrefixReader reader(path, bad_end, writer);
        (void)reader;
    } catch (const std::invalid_argument&) {
        bad_end_threw = true;
    }
    CHECK(bad_end_threw);

    auto bad_count = descriptor;
    ++bad_count.count;
    bool bad_count_threw = false;
    try {
        OOCRelationPrefixReader reader(path, bad_count, writer);
        (void)reader;
    } catch (const std::invalid_argument&) {
        bad_count_threw = true;
    }
    CHECK(bad_count_threw);

    auto stale = descriptor;
    ++stale.generation;
    bool stale_threw = false;
    try {
        writer.resume_append(stale);
    } catch (const std::invalid_argument&) {
        stale_threw = true;
    }
    CHECK(stale_threw);
    CHECK(writer.state() == OOCWriterState::Suspended);

    bool stale_reader_threw = false;
    try {
        OOCRelationPrefixReader reader(path, stale, writer);
        (void)reader;
    } catch (const std::invalid_argument&) {
        stale_reader_threw = true;
    }
    CHECK(stale_reader_threw);

    auto foreign_path = make_tmp_ooc_path("foreign_descriptor");
    OOCArtifacts foreign_cleanup(foreign_path);
    OOCRelationWriter foreign_writer(foreign_path);
    CHECK(foreign_writer.write(make_snapshot_relation(0)) == 0);
    CHECK(foreign_writer.write(make_snapshot_relation(1)) == 1);
    const auto foreign_descriptor = foreign_writer.checkpoint_prefix();
    CHECK(foreign_descriptor.count == descriptor.count);
    CHECK(foreign_descriptor.store_id != descriptor.store_id);

    bool foreign_threw = false;
    try {
        OOCRelationPrefixReader reader(path, foreign_descriptor, writer);
        (void)reader;
    } catch (const std::invalid_argument&) {
        foreign_threw = true;
    }
    CHECK(foreign_threw);

    bool foreign_resume_threw = false;
    try {
        writer.resume_append(foreign_descriptor);
    } catch (const std::invalid_argument&) {
        foreign_resume_threw = true;
    }
    CHECK(foreign_resume_threw);
    CHECK(writer.state() == OOCWriterState::Suspended);

    foreign_writer.resume_append(foreign_descriptor);
    (void)foreign_writer.finalize();

    uint64_t original_second_offset = 0;
    {
        std::fstream index(path + ".relidx", std::ios::in | std::ios::out | std::ios::binary);
        CHECK(static_cast<bool>(index));
        index.seekg(24);
        index.read(reinterpret_cast<char*>(&original_second_offset), 8);
        CHECK(static_cast<bool>(index));
        const uint64_t corrupt_offset = 0;
        index.seekp(24);
        index.write(reinterpret_cast<const char*>(&corrupt_offset), 8);
        index.flush();
        CHECK(static_cast<bool>(index));
    }

    bool corrupt_offset_threw = false;
    try {
        OOCRelationPrefixReader reader(path, descriptor, writer);
        (void)reader;
    } catch (const std::runtime_error&) {
        corrupt_offset_threw = true;
    }
    CHECK(corrupt_offset_threw);

    {
        std::fstream index(path + ".relidx", std::ios::in | std::ios::out | std::ios::binary);
        CHECK(static_cast<bool>(index));
        index.seekp(24);
        index.write(reinterpret_cast<const char*>(&original_second_offset), 8);
        index.flush();
        CHECK(static_cast<bool>(index));
    }

    {
        OOCRelationPrefixReader reader(path, descriptor, writer);
        CHECK(reader.count() == 2);
        CHECK(reader.read(0).a == 1);
        CHECK(reader.read(1).a == 3);
    }
    writer.resume_append(descriptor);
    CHECK(writer.state() == OOCWriterState::Open);
    CHECK(writer.write(make_snapshot_relation(2)) == 2);
    CHECK(writer.finalize().count == 3);

    OOCRelationReader final_reader(path);
    CHECK(final_reader.count() == 3);

    std::cout << "  OOC prefix validation: PASS" << std::endl;
}

// ──────────────────────────────────────────────────────────────────────────
// OOC Resume mode tests (BACKLOG #11e — sieve mid-flight checkpoint)
// 验证 OOCRelationWriter(path, resume=true) 加载现有文件 + 末尾追加 + reader
// 看到 N+M 个 relation. 仅当 prior session magic = INCOMPLETE 时允许 resume.
// ──────────────────────────────────────────────────────────────────────────

void test_ooc_writer_resume_append() {
    std::cout << "Testing OOC writer resume append..." << std::endl;
    auto path = make_tmp_ooc_path("resume_append");
    OOCArtifacts cleanup(path);

    // Phase 1: 写 3 个 rel, close (finalize MAGIC)
    {
        OOCRelationWriter writer(path);
        for (int i = 1; i <= 3; ++i) {
            Relation r(i * 10, static_cast<uint64_t>(i * 10 + 1));
            r.rational_factors.push_back(static_cast<uint32_t>(i));
            writer.write(r);
        }
        assert(writer.count() == 3);
    } // destructor → close() → flip MAGIC

    // 手动 flip MAGIC → INCOMPLETE 模拟 prior session crash
    {
        std::fstream idx(path + ".relidx", std::ios::in | std::ios::out | std::ios::binary);
        uint64_t incomplete = OOCRelationWriter::MAGIC_INCOMPLETE;
        idx.write(reinterpret_cast<const char*>(&incomplete), 8);
    }

    // Phase 2: resume, 追加 2 个 rel, close
    {
        OOCRelationWriter writer(path, /*resume=*/true);
        assert(writer.count() == 3); // prior count 加载
        for (int i = 4; i <= 5; ++i) {
            Relation r(i * 10, static_cast<uint64_t>(i * 10 + 1));
            r.rational_factors.push_back(static_cast<uint32_t>(i));
            writer.write(r);
        }
        assert(writer.count() == 5);
    }

    // Reader 看到 5 个 rel, 顺序正确
    OOCRelationReader reader(path);
    assert(reader.count() == 5);
    for (size_t i = 0; i < 5; ++i) {
        auto rel = reader.read(i);
        assert(rel.a == static_cast<int64_t>((i + 1) * 10));
        assert(rel.b == (i + 1) * 10 + 1);
        assert(rel.rational_factors.size() == 1);
        assert(rel.rational_factors[0] == static_cast<uint32_t>(i + 1));
    }

    std::cout << "  OOC writer resume append: PASS (5 = 3 prior + 2 new)" << std::endl;
}

void test_ooc_writer_resume_finalized_rejected() {
    std::cout << "Testing OOC writer resume rejects finalized files..." << std::endl;
    auto path = make_tmp_ooc_path("resume_finalized");
    OOCArtifacts cleanup(path);

    // 写 1 个 rel, close → MAGIC finalized
    {
        OOCRelationWriter writer(path);
        Relation r(1, 2);
        writer.write(r);
    }

    // resume=true 对 MAGIC 文件必抛
    bool threw = false;
    try {
        OOCRelationWriter resumed(path, /*resume=*/true);
        (void)resumed;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "  OOC writer resume rejects finalized: PASS" << std::endl;
}

void test_ooc_writer_resume_nonexistent_rejected() {
    std::cout << "Testing OOC writer resume rejects nonexistent..." << std::endl;
    auto path = gnfs::util::temp_path(
        "gnfs_test_nonexistent_" + std::to_string(gnfs::util::process_id()) + "_xyz_resume_check");

    bool threw = false;
    try {
        OOCRelationWriter resumed(path, /*resume=*/true);
        (void)resumed;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "  OOC writer resume rejects nonexistent: PASS" << std::endl;
}

void test_ooc_collector_resume_loads_seen() {
    std::cout << "Testing OOC collector resume loads (a,b) seen set..." << std::endl;
    auto path = make_tmp_ooc_path("collector_resume");
    OOCArtifacts cleanup(path);

    // Phase 1: collector add 3 rels, scope exit closes writer (flip MAGIC)
    {
        CollectorConfig cfg;
        cfg.check_duplicates = true;
        cfg.ooc_enabled = true;
        cfg.ooc_base_path = path;
        RelationCollector collector(cfg);
        for (int i = 1; i <= 3; ++i) {
            Relation r(i * 10, static_cast<uint64_t>(i * 10 + 1));
            r.rational_factors.push_back(static_cast<uint32_t>(i));
            if (i >= 2) {
                r.rational_large_prime.push_back(PrimePower{1009 + static_cast<uint64_t>(i), 0, 1});
            }
            if (i == 3) {
                r.algebraic_large_prime.push_back(PrimePower{2003, 17, 1});
            }
            assert(collector.add(std::move(r)));
        }
        assert(collector.size() == 3);
    }

    // 手动 flip MAGIC → INCOMPLETE 模拟 prior session crash 前未 finalize
    {
        std::fstream idx(path + ".relidx", std::ios::in | std::ios::out | std::ios::binary);
        uint64_t incomplete = OOCRelationWriter::MAGIC_INCOMPLETE;
        idx.write(reinterpret_cast<const char*>(&incomplete), 8);
    }

    // Phase 2: collector + ooc_resume=true
    {
        CollectorConfig cfg;
        cfg.check_duplicates = true;
        cfg.ooc_enabled = true;
        cfg.ooc_resume = true;
        cfg.ooc_base_path = path;
        RelationCollector collector(cfg);

        // size() reflects prior writer count
        assert(collector.size() == 3);
        auto stats0 = collector.stats();
        assert(stats0.total_relations == 3);
        assert(stats0.full_relations == 1);
        assert(stats0.partial_1lp == 1);
        assert(stats0.partial_2lp == 1);

        // 尝试重 add prior (a,b) — seen_ 拒绝 (dedup)
        Relation dup1(10, 11);
        dup1.rational_factors.push_back(1);
        assert(!collector.add(std::move(dup1))); // 重复
        Relation dup2(20, 21);
        dup2.rational_factors.push_back(2);
        assert(!collector.add(std::move(dup2))); // 重复
        assert(collector.size() == 3);           // 不变

        // Add 2 new (a,b) 通过
        for (int i = 4; i <= 5; ++i) {
            Relation r(i * 10, static_cast<uint64_t>(i * 10 + 1));
            r.rational_factors.push_back(static_cast<uint32_t>(i));
            assert(collector.add(std::move(r)));
        }
        assert(collector.size() == 5);
        const auto stats1 = collector.stats();
        assert(stats1.total_relations == 5);
        assert(stats1.full_relations == 3);
        assert(stats1.partial_1lp == 1);
        assert(stats1.partial_2lp == 1);
    } // 析构 close + finalize MAGIC

    // Reader 验证 final state
    OOCRelationReader reader(path);
    assert(reader.count() == 5);
    for (size_t i = 0; i < 5; ++i) {
        auto rel = reader.read(i);
        assert(rel.a == static_cast<int64_t>((i + 1) * 10));
        assert(rel.b == (i + 1) * 10 + 1);
    }

    std::cout << "  OOC collector resume + seen restore: PASS" << std::endl;
}

void test_ooc_collector_resume_empty_files_graceful() {
    std::cout << "Testing OOC collector resume with empty prior count..." << std::endl;
    auto path = make_tmp_ooc_path("collector_resume_empty");
    OOCArtifacts cleanup(path);

    // Phase 1: collector open + immediate close (0 relations added)
    {
        CollectorConfig cfg;
        cfg.ooc_enabled = true;
        cfg.ooc_base_path = path;
        RelationCollector collector(cfg);
        assert(collector.size() == 0);
    }

    // Flip MAGIC → INCOMPLETE
    {
        std::fstream idx(path + ".relidx", std::ios::in | std::ios::out | std::ios::binary);
        uint64_t incomplete = OOCRelationWriter::MAGIC_INCOMPLETE;
        idx.write(reinterpret_cast<const char*>(&incomplete), 8);
    }

    // Phase 2: resume from 0-count session, add new rels
    {
        CollectorConfig cfg;
        cfg.ooc_enabled = true;
        cfg.ooc_resume = true;
        cfg.ooc_base_path = path;
        RelationCollector collector(cfg);
        assert(collector.size() == 0);

        // Coprime (a,b): (1,2), (3,4) — gcd 始终 1, 通过 collector validate
        for (int i = 1; i <= 2; ++i) {
            Relation r(2 * i - 1, static_cast<uint64_t>(2 * i));
            assert(collector.add(std::move(r)));
        }
        assert(collector.size() == 2);
    }

    OOCRelationReader reader(path);
    assert(reader.count() == 2);

    std::cout << "  OOC collector resume from empty: PASS" << std::endl;
}

void test_ooc_writer_resume_large_payload() {
    std::cout << "Testing OOC writer resume with variable-size payloads..." << std::endl;
    auto path = make_tmp_ooc_path("resume_large");
    OOCArtifacts cleanup(path);

    // Phase 1: 写 100 个 rel, 每个 varying weight (1-5 rational factors)
    {
        OOCRelationWriter writer(path);
        for (int i = 1; i <= 100; ++i) {
            Relation r(i, static_cast<uint64_t>(i + 1000));
            size_t weight = static_cast<size_t>((i % 5) + 1);
            for (size_t j = 0; j < weight; ++j) {
                r.rational_factors.push_back(static_cast<uint32_t>(static_cast<size_t>(i) + j));
            }
            writer.write(r);
        }
        assert(writer.count() == 100);
    }

    // Flip to INCOMPLETE
    {
        std::fstream idx(path + ".relidx", std::ios::in | std::ios::out | std::ios::binary);
        uint64_t incomplete = OOCRelationWriter::MAGIC_INCOMPLETE;
        idx.write(reinterpret_cast<const char*>(&incomplete), 8);
    }

    // Phase 2: resume, 追加 50 个 rel (different weights)
    {
        OOCRelationWriter writer(path, /*resume=*/true);
        assert(writer.count() == 100);
        for (int i = 101; i <= 150; ++i) {
            Relation r(i, static_cast<uint64_t>(i + 1000));
            size_t weight = static_cast<size_t>(((i - 100) % 3) + 2);
            for (size_t j = 0; j < weight; ++j) {
                r.rational_factors.push_back(static_cast<uint32_t>(static_cast<size_t>(i) + j + 7));
            }
            writer.write(r);
        }
        assert(writer.count() == 150);
    }

    // Reader 验证 all 150 + payload integrity
    OOCRelationReader reader(path);
    assert(reader.count() == 150);
    for (size_t i = 0; i < 100; ++i) {
        auto rel = reader.read(i);
        int idx = static_cast<int>(i) + 1;
        assert(rel.a == idx);
        assert(rel.b == static_cast<uint64_t>(idx + 1000));
        size_t expected_weight = static_cast<size_t>((idx % 5) + 1);
        assert(rel.rational_factors.size() == expected_weight);
        for (size_t j = 0; j < expected_weight; ++j) {
            assert(rel.rational_factors[j] == static_cast<uint32_t>(static_cast<size_t>(idx) + j));
        }
    }
    for (size_t i = 100; i < 150; ++i) {
        auto rel = reader.read(i);
        int idx = static_cast<int>(i) + 1;
        assert(rel.a == idx);
        assert(rel.b == static_cast<uint64_t>(idx + 1000));
        size_t expected_weight = static_cast<size_t>(((idx - 100) % 3) + 2);
        assert(rel.rational_factors.size() == expected_weight);
        for (size_t j = 0; j < expected_weight; ++j) {
            assert(rel.rational_factors[j] ==
                   static_cast<uint32_t>(static_cast<size_t>(idx) + j + 7));
        }
    }

    std::cout << "  OOC writer resume large payload: PASS (150 rels, mixed weight)" << std::endl;
}

int main() {
    std::cout << "=== Relation Collector Tests ===" << std::endl;

    test_basic_add();
    test_duplicate_rejection();
    test_invalid_rejection();
    test_partial_relations();
    test_effective_large_prime_stats();
    test_batch_add();
    test_save_load();
    test_concurrent_add();
    test_merge();
    test_filter_duplicates();
    test_sort_relations();
    test_callback();
    test_callback_no_deadlock();
    test_n_divisibility_rejection();

    std::cout << "\n=== OOC mode tests (BACKLOG #11c) ===" << std::endl;
    test_ooc_basic_add();
    test_ooc_duplicate_rejection();
    test_ooc_n_divisibility();
    test_ooc_partial_relations();
    test_ooc_concurrent_add();
    test_ooc_clear_recycle();
    test_ooc_empty_base_path_rejected();
    test_ooc_legacy_save_load_disabled();
    test_ooc_snapshot_append_snapshot_finalize();
    test_ooc_checkpoint_requires_explicit_resume();
    test_ooc_failed_state_rejects_mutation();
    test_ooc_snapshot_integrity_failure_fails_closed();
    test_ooc_empty_and_repeated_snapshot();
    test_ooc_writer_finalize_state();
    test_ooc_reader_rejects_corrupt_variable_lengths();
    test_ooc_reader_rejects_trailing_bytes();
    test_ooc_prefix_reader_rejects_bad_descriptor_and_offsets();

    std::cout << "\n=== OOC resume mode tests (BACKLOG #11e) ===" << std::endl;
    test_ooc_writer_resume_append();
    test_ooc_writer_resume_finalized_rejected();
    test_ooc_writer_resume_nonexistent_rejected();
    test_ooc_writer_resume_large_payload();
    test_ooc_collector_resume_loads_seen();
    test_ooc_collector_resume_empty_files_graceful();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
