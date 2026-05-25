// Force assert() to remain live even in Release builds. Several cases here
// embed side-effecting calls inside assert (e.g. assert(collector.add(...)));
// NDEBUG would otherwise strip both the check and the call, leaving phase-1
// setup unexecuted and phase-3 reads scanning uninitialized index entries —
// which surfaced on CI as "OOCRelationReader: corrupt record (truncated)".
#ifdef NDEBUG
#  undef NDEBUG
#endif

#include "gnfs/relation/collector.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/safe_math.hpp"
#include "gnfs/util/temp_path.hpp"

#include <cassert>
#include <cstdio>
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
        Relation rel(i % 5, static_cast<uint64_t>((i % 5) + 1));  // 会有重复
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
        Relation rel(i, static_cast<uint64_t>(i + 1));
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
    return gnfs::util::temp_path(
        "gnfs_test_collector_ooc_" + std::to_string(gnfs::util::process_id()) +
        "_" + std::to_string(++seq) + "_" + label);
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

    Relation rel2(100, 201);  // 重复 (a,b)
    assert(!collector.add(std::move(rel2)));

    assert(collector.size() == 1);  // OOC writer count = 1, dedup 拒绝第二个
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
    for (auto& th : threads) th.join();

    // 全部 400 个 (a,b) 唯一, 都应被接受
    assert(collector.size() == 400);

    auto rels = collector.get_relations();
    assert(rels.size() == 400);

    std::cout << "  OOC concurrent add: PASS (" << collector.size()
              << " relations on disk)" << std::endl;
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
    assert(collector.size() == 0);  // OOC writer count reset to 0 after recycle
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
    config.ooc_base_path = "";  // empty → ctor 必须抛

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
    }  // destructor → close() → flip MAGIC

    // 手动 flip MAGIC → INCOMPLETE 模拟 prior session crash
    {
        std::fstream idx(path + ".relidx",
                         std::ios::in | std::ios::out | std::ios::binary);
        uint64_t incomplete = OOCRelationWriter::MAGIC_INCOMPLETE;
        idx.write(reinterpret_cast<const char*>(&incomplete), 8);
    }

    // Phase 2: resume, 追加 2 个 rel, close
    {
        OOCRelationWriter writer(path, /*resume=*/true);
        assert(writer.count() == 3);  // prior count 加载
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
        "gnfs_test_nonexistent_" + std::to_string(gnfs::util::process_id()) +
        "_xyz_resume_check");

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
            assert(collector.add(std::move(r)));
        }
        assert(collector.size() == 3);
    }

    // 手动 flip MAGIC → INCOMPLETE 模拟 prior session crash 前未 finalize
    {
        std::fstream idx(path + ".relidx",
                         std::ios::in | std::ios::out | std::ios::binary);
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

        // 尝试重 add prior (a,b) — seen_ 拒绝 (dedup)
        Relation dup1(10, 11);
        dup1.rational_factors.push_back(1);
        assert(!collector.add(std::move(dup1)));  // 重复
        Relation dup2(20, 21);
        dup2.rational_factors.push_back(2);
        assert(!collector.add(std::move(dup2)));  // 重复
        assert(collector.size() == 3);  // 不变

        // Add 2 new (a,b) 通过
        for (int i = 4; i <= 5; ++i) {
            Relation r(i * 10, static_cast<uint64_t>(i * 10 + 1));
            r.rational_factors.push_back(static_cast<uint32_t>(i));
            assert(collector.add(std::move(r)));
        }
        assert(collector.size() == 5);
    }  // 析构 close + finalize MAGIC

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
        std::fstream idx(path + ".relidx",
                         std::ios::in | std::ios::out | std::ios::binary);
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
            Relation r(2*i - 1, static_cast<uint64_t>(2*i));
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
        std::fstream idx(path + ".relidx",
                         std::ios::in | std::ios::out | std::ios::binary);
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
