// Force assert() to remain live even in Release builds. Several cases here
// embed side-effecting calls inside assert (e.g. assert(collector.add(...)));
// NDEBUG would otherwise strip both the check and the call, leaving phase-1
// setup unexecuted and phase-3 reads scanning uninitialized index entries —
// which surfaced on CI as "OOCRelationReader: corrupt record (truncated)".
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "gnfs/relation/collector.hpp"
#include "gnfs/relation/relation_source.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/safe_math.hpp"
#include "gnfs/util/temp_path.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

using namespace gnfs;
using namespace gnfs::relation;
using namespace gnfs::core;

static_assert(RelationSource<CollectorUniqueOOCPrefixSource>);
static_assert(CollectorUniqueOOCPrefixSource::provides_unique_relations);
static_assert(
    !std::is_constructible_v<CollectorUniqueOOCPrefixSource, const CollectorOOCPrefixSource&>);

template <typename Source>
concept PublicSourceUntrustChannel = requires(const Source& source, std::exception_ptr failure) {
    source.mark_untrusted(std::move(failure));
};

static_assert(!PublicSourceUntrustChannel<CollectorUniqueOOCPrefixSource>);

template <typename Source>
concept PublicProvenABMembership = requires(const Source& source, const ABPair& ab_pair) {
    source.contains_proven_ab_pair(ab_pair);
};

static_assert(!PublicProvenABMembership<CollectorUniqueOOCPrefixSource>);

struct FreshPrefixNoop final {
    void operator()(const CollectorOOCPrefixSource&) const {}
};

template <typename Source>
concept PublicFreshPrefixView =
    requires(const Source& source) { source.with_fresh_prefix_view(FreshPrefixNoop{}); };

static_assert(!PublicFreshPrefixView<CollectorUniqueOOCPrefixSource>);

[[noreturn]] static void check_failed(const char* expression, int line) {
    throw std::runtime_error(std::string("CHECK failed at line ") + std::to_string(line) + ": " +
                             expression);
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition))                                                                          \
            check_failed(#condition, __LINE__);                                                    \
    } while (false)

template <typename Operation> static void check_logic_error(Operation&& operation) {
    bool threw = false;
    try {
        operation();
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
}

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

static Relation make_load_test_relation(int64_t a, uint64_t b, bool partial) {
    Relation relation(a, b);
    relation.rational_factors.push_back(static_cast<uint32_t>(a > 0 ? a : -a));
    if (partial)
        relation.rational_large_prime.push_back(PrimePower{1000003, 0, 1});
    return relation;
}

static void check_relation_ab_equal(const Relation& actual, const Relation& expected) {
    CHECK(actual.a == expected.a);
    CHECK(actual.b == expected.b);
}

static void test_load_replaces_state_transactionally(bool use_pool) {
    std::cout << "Testing transactional load replacement (" << (use_pool ? "pool" : "vector")
              << ")..." << std::endl;

    const auto test_file =
        gnfs::util::temp_path(use_pool ? "gnfs_test_collector_load_replace_pool.bin"
                                       : "gnfs_test_collector_load_replace_vector.bin");
    std::filesystem::remove(test_file);

    CollectorConfig source_config;
    source_config.use_pool = false;
    RelationCollector source(source_config);
    CHECK(source.add(make_load_test_relation(11, 12, false)));
    CHECK(source.add(make_load_test_relation(13, 14, true)));
    CHECK(source.save(test_file));

    CollectorConfig target_config;
    target_config.use_pool = use_pool;
    target_config.pool_initial_bytes = 256;
    target_config.check_duplicates = true;
    RelationCollector target(target_config);
    Relation old_relation = make_load_test_relation(100, 101, false);
    CHECK(target.add(Relation(old_relation)));
    CHECK(!target.add(Relation(old_relation)));
    CHECK(!target.add(Relation(7, 0)));
    CHECK(target.stats().duplicates_rejected == 1);
    CHECK(target.stats().invalid_rejected == 1);

    CHECK(target.load(test_file));
    CHECK(target.size() == 2);
    const auto loaded_stats = target.stats();
    CHECK(loaded_stats.total_relations == 2);
    CHECK(loaded_stats.full_relations == 1);
    CHECK(loaded_stats.partial_1lp == 1);
    CHECK(loaded_stats.partial_2lp == 0);
    CHECK(loaded_stats.duplicates_rejected == 0);
    CHECK(loaded_stats.invalid_rejected == 0);
    CHECK(loaded_stats.n_divisible_rejected == 0);

    const auto loaded = target.finalize_relations();
    CHECK(loaded.size() == 2);
    CHECK(loaded[0].a == 11);
    CHECK(loaded[1].a == 13);

    // The old key must not remain in seen_, while a loaded key still rejects a
    // duplicate after the replacement commits.
    CHECK(target.add(std::move(old_relation)));
    CHECK(!target.add(make_load_test_relation(11, 12, false)));
    CHECK(target.stats().duplicates_rejected == 1);

    std::filesystem::remove(test_file);
    std::cout << "  Transactional load replacement: PASS" << std::endl;
}

static void test_load_rejects_duplicate_ab_pairs(bool use_pool) {
    std::cout << "Testing load duplicate rejection (" << (use_pool ? "pool" : "vector") << ")..."
              << std::endl;

    const auto test_file =
        gnfs::util::temp_path(use_pool ? "gnfs_test_collector_load_duplicate_pool.bin"
                                       : "gnfs_test_collector_load_duplicate_vector.bin");
    std::filesystem::remove(test_file);

    CollectorConfig source_config;
    source_config.use_pool = false;
    source_config.check_duplicates = false;
    RelationCollector source(source_config);
    const Relation duplicate = make_load_test_relation(17, 18, false);
    CHECK(source.add(Relation(duplicate)));
    CHECK(source.add(Relation(duplicate)));
    CHECK(source.size() == 2);
    CHECK(source.save(test_file));

    CollectorConfig target_config;
    target_config.use_pool = use_pool;
    target_config.pool_initial_bytes = 256;
    target_config.check_duplicates = true;
    RelationCollector target(target_config);
    CHECK(target.load(test_file));
    CHECK(target.size() == 1);
    const auto stats = target.stats();
    CHECK(stats.total_relations == 1);
    CHECK(stats.duplicates_rejected == 1);
    CHECK(target.finalize_relations().front().ab() == duplicate.ab());

    std::filesystem::remove(test_file);
    std::cout << "  Load duplicate rejection: PASS" << std::endl;
}

static void test_load_failure_preserves_state_transactionally(bool use_pool) {
    std::cout << "Testing transactional load failure rollback (" << (use_pool ? "pool" : "vector")
              << ")..." << std::endl;

    const auto payload_file =
        gnfs::util::temp_path(use_pool ? "gnfs_test_collector_load_truncated_pool.bin"
                                       : "gnfs_test_collector_load_truncated_vector.bin");
    const auto header_file =
        gnfs::util::temp_path(use_pool ? "gnfs_test_collector_load_header_pool.bin"
                                       : "gnfs_test_collector_load_header_vector.bin");
    const auto forged_count_file =
        gnfs::util::temp_path(use_pool ? "gnfs_test_collector_load_forged_count_pool.bin"
                                       : "gnfs_test_collector_load_forged_count_vector.bin");
    std::filesystem::remove(payload_file);
    std::filesystem::remove(header_file);
    std::filesystem::remove(forged_count_file);

    CollectorConfig source_config;
    source_config.use_pool = false;
    RelationCollector source(source_config);
    CHECK(source.add(make_load_test_relation(21, 22, false)));
    CHECK(source.save(payload_file));
    const auto payload_size = std::filesystem::file_size(payload_file);
    CHECK(payload_size > 1);
    std::filesystem::resize_file(payload_file, payload_size - 1);

    {
        std::ofstream header(header_file, std::ios::binary | std::ios::trunc);
        const uint32_t partial_header = 1;
        header.write(reinterpret_cast<const char*>(&partial_header), sizeof(partial_header));
        CHECK(header.good());
    }
    {
        std::ofstream forged_count(forged_count_file, std::ios::binary | std::ios::trunc);
        const uint64_t forged_count_value = std::numeric_limits<uint64_t>::max();
        forged_count.write(reinterpret_cast<const char*>(&forged_count_value),
                           sizeof(forged_count_value));
        CHECK(forged_count.good());
    }

    CollectorConfig target_config;
    target_config.use_pool = use_pool;
    target_config.pool_initial_bytes = 256;
    target_config.check_duplicates = true;
    RelationCollector target(target_config);
    Relation old_relation = make_load_test_relation(200, 201, false);
    CHECK(target.add(Relation(old_relation)));
    CHECK(!target.add(Relation(old_relation)));
    CHECK(!target.add(Relation(9, 0)));
    const auto stats_before = target.stats();
    const auto relations_before = target.finalize_relations();

    for (const auto& filename : {payload_file, header_file, forged_count_file}) {
        CHECK(!target.load(filename));
        CHECK(target.size() == relations_before.size());
        check_stats_equal(target.stats(), stats_before);
        const auto relations_after = target.finalize_relations();
        CHECK(relations_after.size() == relations_before.size());
        check_relation_ab_equal(relations_after.front(), relations_before.front());
    }

    // Failure must also leave the duplicate index intact.
    CHECK(!target.add(std::move(old_relation)));
    CHECK(target.stats().duplicates_rejected == stats_before.duplicates_rejected + 1);

    std::filesystem::remove(payload_file);
    std::filesystem::remove(header_file);
    std::filesystem::remove(forged_count_file);
    std::cout << "  Transactional load failure rollback: PASS" << std::endl;
}

static void test_load_respects_max_relations() {
    std::cout << "Testing transactional load max_relations guard..." << std::endl;

    const auto test_file = gnfs::util::temp_path("gnfs_test_collector_load_max_relations.bin");
    std::filesystem::remove(test_file);

    CollectorConfig source_config;
    source_config.use_pool = false;
    RelationCollector source(source_config);
    CHECK(source.add(make_load_test_relation(31, 32, false)));
    CHECK(source.add(make_load_test_relation(33, 34, false)));
    CHECK(source.save(test_file));

    CollectorConfig target_config;
    target_config.use_pool = false;
    target_config.max_relations = 1;
    RelationCollector target(target_config);
    CHECK(target.add(make_load_test_relation(101, 102, false)));
    const auto stats_before = target.stats();
    const auto relations_before = target.finalize_relations();

    CHECK(!target.load(test_file));
    CHECK(target.size() == relations_before.size());
    check_stats_equal(target.stats(), stats_before);
    const auto relations_after = target.finalize_relations();
    CHECK(relations_after.size() == relations_before.size());
    check_relation_ab_equal(relations_after.front(), relations_before.front());

    std::filesystem::remove(test_file);
    std::cout << "  Transactional load max_relations guard: PASS" << std::endl;
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

    // Full-width uint64_t b must survive the a - b*m validation path on
    // Windows LLP64, where unsigned long is only 32 bits.  Here b % N is
    // 179,869,065, so a - b*m = -17*N and the relation must be rejected.
    {
        const Integer wide_n("1000000007");
        const Integer wide_m("1");
        RelationCollector collector(config);
        collector.set_polynomial_context(wide_n, wide_m);

        const uint64_t b = uint64_t{1} << 34;
        Relation wide_b_rel(179869065, b);
        const bool added = collector.add(std::move(wide_b_rel));
        assert(!added);
        const auto st = collector.stats();
        assert(st.total_relations == 0);
        assert(st.n_divisible_rejected == 1);
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

void test_output_file_open_failure() {
    std::cout << "Testing output file open failure..." << std::endl;

    const auto root = std::filesystem::path(gnfs::util::temp_path(
        "gnfs_test_collector_output_failure_" + std::to_string(gnfs::util::process_id())));
    const auto output_path = root / "missing" / "relations.bin";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    CollectorConfig config;
    config.output_file = output_path.string();

    bool threw = false;
    try {
        RelationCollector collector(config);
    } catch (const std::runtime_error& error) {
        threw = true;
        CHECK(std::string(error.what()).find(output_path.string()) != std::string::npos);
    }

    CHECK(threw);
    CHECK(!std::filesystem::exists(root));
    std::cout << "  Output file open failure: PASS" << std::endl;
}

/// RAII OOC artifact cleanup
struct OOCArtifacts {
    std::string base;
    explicit OOCArtifacts(std::string b) : base(std::move(b)) {}
    ~OOCArtifacts() {
        std::remove((base + ".reldata").c_str());
        std::remove((base + ".relidx").c_str());
        std::remove((base + ".gnfs-ooc-cleanup-v1.lock").c_str());
    }
};

/// Best-effort cleanup for the private directory reserved by RelationSink.
struct OOCSinkLeaseArtifacts {
    std::filesystem::path lease_root;
    explicit OOCSinkLeaseArtifacts(const std::string& base)
        : lease_root(RelationSink::lease_root_for(base)) {}
    ~OOCSinkLeaseArtifacts() {
        std::error_code ec;
        (void)std::filesystem::remove_all(lease_root, ec);
        ec.clear();
        (void)std::filesystem::remove(lease_root.string() + ".gnfs-ooc-cleanup-v1.lock", ec);
    }
};

static std::vector<char> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    CHECK(static_cast<bool>(input));
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

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

void test_ooc_fresh_store_refuses_existing_artifacts() {
    std::cout << "Testing fresh OOC collector refuses existing artifacts..." << std::endl;

    for (const char* occupied_suffix : {".relidx", ".reldata"}) {
        const auto path = make_tmp_ooc_path(std::string("fresh_collision") + occupied_suffix);
        OOCArtifacts cleanup(path);
        const std::filesystem::path occupied(path + occupied_suffix);
        const std::vector<char> sentinel{'G', 'N', 'F', 'S', '-', 's', 'e',
                                         'n', 't', 'i', 'n', 'e', 'l'};
        {
            std::ofstream output(occupied, std::ios::binary);
            CHECK(static_cast<bool>(output));
            output.write(sentinel.data(), static_cast<std::streamsize>(sentinel.size()));
            CHECK(static_cast<bool>(output));
        }

        CollectorConfig config;
        config.ooc_enabled = true;
        config.ooc_base_path = path;
        bool rejected = false;
        try {
            RelationCollector collector(config);
            (void)collector;
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(read_file_bytes(occupied) == sentinel);
        const std::string other_suffix =
            std::string(occupied_suffix) == ".relidx" ? ".reldata" : ".relidx";
        CHECK(!std::filesystem::exists(path + other_suffix));
    }

    // A dangling symlink is still an occupied filesystem entry. Some Windows
    // environments disallow unprivileged symlink creation, so only that setup
    // limitation skips this platform-specific sentinel case.
    {
        const auto path = make_tmp_ooc_path("fresh_dangling_symlink");
        OOCArtifacts cleanup(path);
        const std::filesystem::path link(path + ".relidx");
        std::error_code ec;
        std::filesystem::create_symlink("missing-gnfs-ooc-target", link, ec);
        if (!ec) {
            CollectorConfig config;
            config.ooc_enabled = true;
            config.ooc_base_path = path;
            bool rejected = false;
            try {
                RelationCollector collector(config);
                (void)collector;
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            CHECK(rejected);
            CHECK(std::filesystem::symlink_status(link).type() ==
                  std::filesystem::file_type::symlink);
            CHECK(!std::filesystem::exists(path + ".reldata"));
        }
    }

    std::cout << "  Fresh OOC collision guard: PASS" << std::endl;
}

void test_ooc_uncommitted_fresh_exception_cleanup() {
    std::cout << "Testing uncommitted fresh OOC exception cleanup..." << std::endl;

    {
        const auto path = make_tmp_ooc_path("fresh_exception_cleanup");
        OOCArtifacts cleanup(path);
        CollectorConfig config;
        config.ooc_enabled = true;
        config.ooc_base_path = path;

        RelationCollector collector(config);
        Relation relation(1, 2);
        CHECK(collector.add(std::move(relation)));
        CHECK(collector.discard_uncommitted_fresh_ooc_noexcept());
        CHECK(!std::filesystem::exists(path + ".relidx"));
        CHECK(!std::filesystem::exists(path + ".reldata"));
        CHECK(collector.discard_uncommitted_fresh_ooc_noexcept());
    }

    {
        const auto path = make_tmp_ooc_path("fresh_exception_foreign_header");
        OOCArtifacts cleanup(path);
        CollectorConfig config;
        config.ooc_enabled = true;
        config.ooc_base_path = path;

        RelationCollector collector(config);
        {
            std::fstream index(path + ".relidx", std::ios::binary | std::ios::in | std::ios::out);
            CHECK(static_cast<bool>(index));
            index.seekp(static_cast<std::streamoff>(OOCRelationWriter::INDEX_STORE_ID_OFFSET));
            const uint64_t foreign_store_id = 1;
            index.write(reinterpret_cast<const char*>(&foreign_store_id),
                        static_cast<std::streamsize>(sizeof(foreign_store_id)));
            index.flush();
            CHECK(static_cast<bool>(index));
        }
        CHECK(!collector.discard_uncommitted_fresh_ooc_noexcept());
        CHECK(std::filesystem::exists(path + ".relidx"));
        CHECK(std::filesystem::exists(path + ".reldata"));
    }

    {
        const auto path = make_tmp_ooc_path("fresh_exception_finalized");
        OOCArtifacts cleanup(path);
        CollectorConfig config;
        config.ooc_enabled = true;
        config.ooc_base_path = path;

        RelationCollector collector(config);
        Relation relation(3, 4);
        CHECK(collector.add(std::move(relation)));
        CHECK(collector.finalize_ooc().has_value());
        CHECK(collector.discard_uncommitted_fresh_ooc_noexcept());
        CHECK(!std::filesystem::exists(path + ".relidx"));
        CHECK(!std::filesystem::exists(path + ".reldata"));
    }

    std::cout << "  Uncommitted fresh OOC exception cleanup: PASS" << std::endl;
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

void test_finalize_ooc_vector_mode_remains_appendable() {
    std::cout << "Testing finalize_ooc vector-mode no-op..." << std::endl;

    RelationCollector collector;
    CHECK(collector.add(make_snapshot_relation(0)));
    CHECK(!collector.finalize_ooc().has_value());
    CHECK(collector.add(make_snapshot_relation(1)));
    CHECK(!collector.finalize_ooc().has_value());

    const auto relations = collector.finalize_relations();
    CHECK(relations.size() == 2);
    CHECK(relations[0].a == 1);
    CHECK(relations[1].a == 3);

    // Vector finalize_relations() is also non-consuming for compatibility.
    CHECK(collector.add(make_snapshot_relation(2)));
    CHECK(collector.size() == 3);

    std::cout << "  finalize_ooc vector-mode no-op: PASS" << std::endl;
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

    const auto descriptor = collector.finalize_ooc();
    const auto repeated_descriptor = collector.finalize_ooc();
    CHECK(descriptor.has_value());
    CHECK(repeated_descriptor == descriptor);
    CHECK(descriptor->format_version == OOCRelationWriter::FORMAT_VERSION);
    CHECK(descriptor->store_id != 0);
    CHECK(descriptor->generation != 0);
    CHECK(descriptor->count == 5);
    CHECK(std::filesystem::file_size(path + ".reldata") == descriptor->data_end);
    CHECK(std::filesystem::file_size(path + ".relidx") ==
          OOCRelationWriter::index_size_for_count(descriptor->count));

    OOCRelationReader expected_reader(path, *descriptor);
    CHECK(expected_reader.count() == 5);

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

void test_ooc_borrowed_prefix_append_and_finalize() {
    std::cout << "Testing borrowed OOC prefix -> append -> prefix -> finalize..." << std::endl;
    const auto path = make_tmp_ooc_path("borrowed_prefix_append");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;
    RelationCollector collector(config);
    for (int i = 0; i < 3; ++i) {
        CHECK(collector.add(make_snapshot_relation(i)));
    }

    OOCSnapshotDescriptor first_descriptor;
    auto first_values = collector.with_ooc_prefix([&](const CollectorOOCPrefixSource& source) {
        CHECK(source.count() == 3);
        first_descriptor = source.descriptor();
        auto values = std::make_unique<std::array<int64_t, 3>>();
        std::array<std::thread, 3> workers;
        for (size_t i = 0; i < workers.size(); ++i) {
            workers[i] = std::thread([&, i]() { (*values)[i] = source.read(i).a; });
        }
        // Every source user is joined before the callback-scoped lease
        // returns and destroys the reader mappings.
        for (auto& worker : workers) {
            worker.join();
        }
        return values; // move-only callback result
    });
    CHECK(first_values != nullptr);
    CHECK((*first_values)[0] == 1);
    CHECK((*first_values)[1] == 3);
    CHECK((*first_values)[2] == 5);
    CHECK(first_descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
    CHECK(first_descriptor.store_id != 0);
    CHECK(first_descriptor.count == 3);

    CHECK(collector.add(make_snapshot_relation(3)));
    CHECK(collector.add(make_snapshot_relation(4)));

    OOCSnapshotDescriptor second_descriptor;
    const int64_t second_sum =
        collector.with_ooc_prefix([&](const CollectorOOCPrefixSource& source) {
            CHECK(source.count() == 5);
            second_descriptor = source.descriptor();
            int64_t sum = 0;
            for (size_t i = 0; i < source.count(); ++i) {
                sum += source.read(i).a;
            }
            return sum;
        });
    CHECK(second_sum == 25);
    CHECK(second_descriptor.store_id == first_descriptor.store_id);
    CHECK(second_descriptor.generation > first_descriptor.generation);
    CHECK(second_descriptor.count == 5);
    CHECK(second_descriptor.data_end > first_descriptor.data_end);

    const auto final_descriptor = collector.finalize_ooc();
    CHECK(final_descriptor.has_value());
    CHECK(final_descriptor->store_id == second_descriptor.store_id);
    CHECK(final_descriptor->count == second_descriptor.count);
    CHECK(final_descriptor->data_end == second_descriptor.data_end);
    OOCRelationReader reader(path, *final_descriptor);
    CHECK(reader.count() == 5);
    CHECK(reader.read(4).a == 9);

    std::cout << "  Borrowed OOC prefix append/finalize: PASS" << std::endl;
}

namespace {
struct BorrowedPrefixCallbackFailure final {};
struct BorrowedPrefixOutputFailure final {};

struct ThrowingMoveOnlyResult final {
    ThrowingMoveOnlyResult() = default;
    ThrowingMoveOnlyResult(const ThrowingMoveOnlyResult&) = delete;
    ThrowingMoveOnlyResult& operator=(const ThrowingMoveOnlyResult&) = delete;
    ThrowingMoveOnlyResult(ThrowingMoveOnlyResult&&) {
        throw BorrowedPrefixOutputFailure{};
    }
    ThrowingMoveOnlyResult& operator=(ThrowingMoveOnlyResult&&) = delete;
};

struct ReentrantObserverResult final {
    RelationCollector* collector = nullptr;
    bool* destroyed = nullptr;

    ReentrantObserverResult(RelationCollector& owner, bool& destruction_observed) noexcept
        : collector(&owner), destroyed(&destruction_observed) {}
    ReentrantObserverResult(const ReentrantObserverResult&) = delete;
    ReentrantObserverResult& operator=(const ReentrantObserverResult&) = delete;
    ReentrantObserverResult(ReentrantObserverResult&& other) noexcept
        : collector(other.collector), destroyed(other.destroyed) {
        other.collector = nullptr;
        other.destroyed = nullptr;
    }
    ReentrantObserverResult& operator=(ReentrantObserverResult&&) = delete;
    ~ReentrantObserverResult() {
        if (collector != nullptr) {
            (void)collector->size();
            *destroyed = true;
        }
    }
};
} // namespace

void test_ooc_unique_borrowed_prefix_capability_and_lifecycle() {
    std::cout << "Testing unique borrowed OOC capability/lifecycle..." << std::endl;
    const auto path = make_tmp_ooc_path("unique_borrowed_prefix");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.check_duplicates = true;
    config.ooc_enabled = true;
    config.ooc_base_path = path;
    RelationCollector collector(config);
    for (int i = 0; i < 3; ++i) {
        CHECK(collector.add(make_snapshot_relation(i)));
    }
    CHECK(!collector.add(make_snapshot_relation(0)));

    bool mutation_rejected = false;
    OOCSnapshotDescriptor descriptor;
    auto values =
        collector.with_unique_ooc_prefix([&](const CollectorUniqueOOCPrefixSource& source) {
            CHECK(source.count() == 3);
            CHECK(source.ab_pairs_unique());
            descriptor = source.descriptor();
            CHECK(collector.size() == 3);
            CHECK(collector.stats().total_relations == 3);

            auto result = std::make_unique<std::array<int64_t, 3>>();
            std::array<std::thread, 3> workers;
            for (size_t ordinal = 0; ordinal < workers.size(); ++ordinal) {
                workers[ordinal] =
                    std::thread([&, ordinal]() { (*result)[ordinal] = source.read(ordinal).a; });
            }
            for (auto& worker : workers) {
                worker.join();
            }

            std::thread mutation([&]() {
                try {
                    (void)collector.add(make_snapshot_relation(3));
                } catch (const std::logic_error&) {
                    mutation_rejected = true;
                }
            });
            mutation.join();
            return result;
        });

    CHECK(values != nullptr);
    CHECK((*values)[0] == 1);
    CHECK((*values)[1] == 3);
    CHECK((*values)[2] == 5);
    CHECK(mutation_rejected);
    CHECK(descriptor.count == 3);
    CHECK(descriptor.store_id != 0);
    CHECK(collector.add(make_snapshot_relation(3)));

    bool callback_failure_seen = false;
    try {
        collector.with_unique_ooc_prefix([](const CollectorUniqueOOCPrefixSource& source) {
            CHECK(source.read(0).a == 1);
            throw BorrowedPrefixCallbackFailure{};
        });
    } catch (const BorrowedPrefixCallbackFailure&) {
        callback_failure_seen = true;
    }
    CHECK(callback_failure_seen);
    CHECK(collector.add(make_snapshot_relation(4)));

    const auto final_descriptor = collector.finalize_ooc();
    CHECK(final_descriptor.has_value());
    CHECK(final_descriptor->count == 5);
    OOCRelationReader reader(path, *final_descriptor);
    CHECK(reader.count() == 5);
    CHECK(reader.read(4).a == 9);

    std::cout << "  Unique borrowed OOC capability/lifecycle: PASS" << std::endl;
}

void test_ooc_unique_borrowed_prefix_rejects_unproven_sources() {
    std::cout << "Testing unique borrowed OOC proof preconditions..." << std::endl;

    const auto unchecked_path = make_tmp_ooc_path("unique_borrowed_unchecked");
    OOCArtifacts unchecked_cleanup(unchecked_path);
    {
        CollectorConfig config;
        config.check_duplicates = false;
        config.ooc_enabled = true;
        config.ooc_base_path = unchecked_path;
        RelationCollector collector(config);
        CHECK(collector.add(make_snapshot_relation(0)));
        CHECK(collector.add(make_snapshot_relation(0)));

        bool callback_called = false;
        check_logic_error([&]() {
            collector.with_unique_ooc_prefix(
                [&](const CollectorUniqueOOCPrefixSource&) { callback_called = true; });
        });
        CHECK(!callback_called);

        // The compatibility borrowed-source API remains available and does not
        // misrepresent this prefix as unique.
        collector.with_ooc_prefix([&](const CollectorOOCPrefixSource& source) {
            CHECK(source.count() == 2);
            CHECK(source.read(0).ab() == source.read(1).ab());
        });
        CHECK(collector.add(make_snapshot_relation(1)));
        CHECK(collector.finalize_ooc()->count == 3);
    }

    // A recovered store may predate the collector's duplicate-rejection policy.
    // Its reconstructed seen set exposes that the two persisted rows are not a
    // whole-prefix uniqueness proof, so the strong capability must stay closed.
    const auto recovered_path = make_tmp_ooc_path("unique_borrowed_recovered_duplicates");
    OOCArtifacts recovered_cleanup(recovered_path);
    OOCSnapshotDescriptor recovered_descriptor;
    RelationSequenceReceiptAccumulator recovered_sequence;
    {
        OOCRelationWriter writer(recovered_path);
        const Relation duplicate = make_snapshot_relation(0);
        CHECK(writer.write(duplicate) == 0);
        recovered_sequence.append(duplicate);
        CHECK(writer.write(duplicate) == 1);
        recovered_sequence.append(duplicate);
        recovered_descriptor = writer.checkpoint_prefix();
        writer.fail_suspended_snapshot();
    }

    CollectorConfig recovered_config;
    recovered_config.check_duplicates = true;
    recovered_config.ooc_enabled = true;
    recovered_config.ooc_base_path = recovered_path;
    recovered_config.ooc_resume_snapshot = recovered_descriptor;
    recovered_config.ooc_resume_sequence_receipt = recovered_sequence.finish();
    RelationCollector recovered(recovered_config);
    CHECK(recovered.size() == 2);

    bool recovered_callback_called = false;
    check_logic_error([&]() {
        recovered.with_unique_ooc_prefix(
            [&](const CollectorUniqueOOCPrefixSource&) { recovered_callback_called = true; });
    });
    CHECK(!recovered_callback_called);
    recovered.with_ooc_prefix(
        [](const CollectorOOCPrefixSource& source) { CHECK(source.count() == 2); });
    CHECK(recovered.add(make_snapshot_relation(1)));
    CHECK(recovered.finalize_ooc()->count == 3);

    std::cout << "  Unique borrowed OOC proof preconditions: PASS" << std::endl;
}

void test_ooc_unique_borrowed_prefix_source_corruption_fails_closed() {
    std::cout << "Testing unique borrowed OOC corruption fail-closed..." << std::endl;
    const auto path = make_tmp_ooc_path("unique_borrowed_corruption");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;
    RelationCollector collector(config);
    CHECK(collector.add(make_snapshot_relation(0)));
    const auto stats_before_failure = collector.stats();

    const auto descriptor = collector.checkpoint_ooc();
    {
        std::fstream data(path + ".reldata", std::ios::in | std::ios::out | std::ios::binary);
        CHECK(static_cast<bool>(data));
        const uint32_t corrupt_count = std::numeric_limits<uint32_t>::max();
        data.seekp(static_cast<std::streamoff>(OOCRelationWriter::DATA_HEADER_BYTES + 16));
        data.write(reinterpret_cast<const char*>(&corrupt_count), sizeof(corrupt_count));
        data.flush();
        CHECK(static_cast<bool>(data));
    }
    collector.resume_ooc(descriptor);

    bool callback_caught_source_failure = false;
    bool method_failed_closed = false;
    try {
        collector.with_unique_ooc_prefix([&](const CollectorUniqueOOCPrefixSource& source) {
            try {
                (void)source.read(0);
            } catch (const std::runtime_error&) {
                callback_caught_source_failure = true;
            }
        });
    } catch (const std::runtime_error&) {
        method_failed_closed = true;
    }
    CHECK(callback_caught_source_failure);
    CHECK(method_failed_closed);
    check_logic_error([&]() { (void)collector.add(make_snapshot_relation(1)); });
    check_stats_equal(collector.stats(), stats_before_failure);

    std::cout << "  Unique borrowed OOC corruption fail-closed: PASS" << std::endl;
}

void test_ooc_unique_borrowed_prefix_resume_failure_takes_precedence() {
    std::cout << "Testing unique borrowed OOC resume precedence..." << std::endl;
    const auto path = make_tmp_ooc_path("unique_borrowed_resume_failure");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;
    RelationCollector collector(config);
    CHECK(collector.add(make_snapshot_relation(0)));

    bool resume_failure_seen = false;
    bool callback_failure_leaked = false;
    try {
        collector.with_unique_ooc_prefix([&](const CollectorUniqueOOCPrefixSource& source) {
            CHECK(source.read(0).a == 1);
            std::fstream data(path + ".reldata", std::ios::in | std::ios::out | std::ios::binary);
            CHECK(static_cast<bool>(data));
            const uint64_t corrupt_magic = 0;
            data.seekp(0);
            data.write(reinterpret_cast<const char*>(&corrupt_magic), sizeof(corrupt_magic));
            data.flush();
            CHECK(static_cast<bool>(data));
            throw BorrowedPrefixCallbackFailure{};
        });
    } catch (const BorrowedPrefixCallbackFailure&) {
        callback_failure_leaked = true;
    } catch (const std::runtime_error&) {
        resume_failure_seen = true;
    }
    CHECK(resume_failure_seen);
    CHECK(!callback_failure_leaked);
    check_logic_error([&]() { (void)collector.add(make_snapshot_relation(1)); });

    std::cout << "  Unique borrowed OOC resume precedence: PASS" << std::endl;
}

void test_ooc_borrowed_prefix_callback_failures_resume() {
    std::cout << "Testing borrowed OOC callback/output failure recovery..." << std::endl;
    const auto path = make_tmp_ooc_path("borrowed_prefix_callback_failure");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;
    RelationCollector collector(config);
    CHECK(collector.add(make_snapshot_relation(0)));

    bool callback_failure_seen = false;
    try {
        collector.with_ooc_prefix([](const CollectorOOCPrefixSource& source) {
            CHECK(source.read(0).a == 1);
            throw BorrowedPrefixCallbackFailure{};
        });
    } catch (const BorrowedPrefixCallbackFailure&) {
        callback_failure_seen = true;
    }
    CHECK(callback_failure_seen);
    CHECK(collector.add(make_snapshot_relation(1)));

    bool invalid_ordinal_seen = false;
    try {
        collector.with_ooc_prefix(
            [](const CollectorOOCPrefixSource& source) { (void)source.read(source.count()); });
    } catch (const std::out_of_range&) {
        invalid_ordinal_seen = true;
    }
    CHECK(invalid_ordinal_seen);
    CHECK(collector.add(make_snapshot_relation(2)));

    bool allocation_failure_seen = false;
    try {
        collector.with_ooc_prefix([](const CollectorOOCPrefixSource&) { throw std::bad_alloc{}; });
    } catch (const std::bad_alloc&) {
        allocation_failure_seen = true;
    }
    CHECK(allocation_failure_seen);
    CHECK(collector.add(make_snapshot_relation(3)));

    bool output_failure_seen = false;
    try {
        (void)collector.with_ooc_prefix([](const CollectorOOCPrefixSource& source) {
            CHECK(source.read(0).a == 1);
            return ThrowingMoveOnlyResult{};
        });
    } catch (const BorrowedPrefixOutputFailure&) {
        output_failure_seen = true;
    }
    CHECK(output_failure_seen);
    CHECK(collector.add(make_snapshot_relation(4)));

    const auto final_descriptor = collector.finalize_ooc();
    CHECK(final_descriptor.has_value());
    CHECK(final_descriptor->count == 5);

    std::cout << "  Borrowed OOC callback/output recovery: PASS" << std::endl;
}

void test_ooc_borrowed_prefix_source_corruption_fails_closed() {
    std::cout << "Testing borrowed OOC source corruption fails closed..." << std::endl;
    const auto path = make_tmp_ooc_path("borrowed_prefix_corruption");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;
    RelationCollector collector(config);
    CHECK(collector.add(make_snapshot_relation(0)));
    const auto stats_before_failure = collector.stats();

    // Flush first, then corrupt a compact-record field while handles are
    // closed. Resume validates physical boundaries but record decoding remains
    // the borrowed source's responsibility.
    const auto descriptor = collector.checkpoint_ooc();
    {
        std::fstream data(path + ".reldata", std::ios::in | std::ios::out | std::ios::binary);
        CHECK(static_cast<bool>(data));
        const uint32_t corrupt_count = std::numeric_limits<uint32_t>::max();
        data.seekp(static_cast<std::streamoff>(OOCRelationWriter::DATA_HEADER_BYTES + 16));
        data.write(reinterpret_cast<const char*>(&corrupt_count), sizeof(corrupt_count));
        data.flush();
        CHECK(static_cast<bool>(data));
    }
    collector.resume_ooc(descriptor);

    bool callback_caught_source_failure = false;
    bool method_failed_closed = false;
    bool callback_result_destroyed = false;
    try {
        collector.with_ooc_prefix([&](const CollectorOOCPrefixSource& source) {
            try {
                (void)source.read(0);
            } catch (const std::runtime_error&) {
                callback_caught_source_failure = true;
            }
            return ReentrantObserverResult(collector, callback_result_destroyed);
        });
    } catch (const std::runtime_error&) {
        method_failed_closed = true;
    }
    CHECK(callback_caught_source_failure);
    CHECK(method_failed_closed);
    CHECK(callback_result_destroyed);

    check_logic_error([&]() { (void)collector.add(make_snapshot_relation(1)); });
    check_stats_equal(collector.stats(), stats_before_failure);
    CHECK(collector.size() == 1);

    std::cout << "  Borrowed OOC source corruption fail-closed: PASS" << std::endl;
}

void test_ooc_borrowed_prefix_resume_failure_takes_precedence() {
    std::cout << "Testing borrowed OOC resume failure precedence..." << std::endl;
    const auto path = make_tmp_ooc_path("borrowed_prefix_resume_failure");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;
    RelationCollector collector(config);
    CHECK(collector.add(make_snapshot_relation(0)));

    bool resume_failure_seen = false;
    bool callback_failure_leaked = false;
    try {
        collector.with_ooc_prefix([&](const CollectorOOCPrefixSource& source) {
            CHECK(source.read(0).a == 1);
            std::fstream data(path + ".reldata", std::ios::in | std::ios::out | std::ios::binary);
            CHECK(static_cast<bool>(data));
            const uint64_t corrupt_magic = 0;
            data.seekp(0);
            data.write(reinterpret_cast<const char*>(&corrupt_magic), sizeof(corrupt_magic));
            data.flush();
            CHECK(static_cast<bool>(data));
            throw BorrowedPrefixCallbackFailure{};
        });
    } catch (const BorrowedPrefixCallbackFailure&) {
        callback_failure_leaked = true;
    } catch (const std::runtime_error&) {
        resume_failure_seen = true;
    }
    CHECK(resume_failure_seen);
    CHECK(!callback_failure_leaked);
    check_logic_error([&]() { (void)collector.add(make_snapshot_relation(1)); });

    std::cout << "  Borrowed OOC resume failure precedence: PASS" << std::endl;
}

void test_ooc_borrowed_prefix_serialization_and_state_rules() {
    std::cout << "Testing borrowed OOC serialization and state rules..." << std::endl;
    const auto path = make_tmp_ooc_path("borrowed_prefix_serialization");
    OOCArtifacts cleanup(path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;
    RelationCollector collector(config);
    CHECK(collector.add(make_snapshot_relation(0)));

    bool mutation_rejected = false;
    std::exception_ptr mutation_failure;
    collector.with_ooc_prefix([&](const CollectorOOCPrefixSource& source) {
        // Observer re-entry remains usable while owner/mutation calls reject
        // without waiting on the suspended writer.
        CHECK(collector.size() == 1);
        CHECK(collector.stats().total_relations == 1);
        std::thread mutation_thread([&]() {
            try {
                (void)collector.add(make_snapshot_relation(1));
            } catch (const std::logic_error&) {
                mutation_rejected = true;
            } catch (...) {
                mutation_failure = std::current_exception();
            }
        });
        mutation_thread.join();
        CHECK(source.read(0).a == 1);
    });
    CHECK(!mutation_failure);
    CHECK(mutation_rejected);
    CHECK(collector.add(make_snapshot_relation(1)));
    CHECK(collector.size() == 2);

    // Explicitly suspended, finalized, handed-off, vector, and pool collectors
    // all reject the OOC-only appendable-prefix lease.
    const auto suspended = collector.checkpoint_ooc();
    check_logic_error([&]() { collector.with_ooc_prefix([](const CollectorOOCPrefixSource&) {}); });
    collector.resume_ooc(suspended);
    CHECK(collector.finalize_ooc().has_value());
    check_logic_error([&]() { collector.with_ooc_prefix([](const CollectorOOCPrefixSource&) {}); });

    RelationCollector vector_collector;
    check_logic_error(
        [&]() { vector_collector.with_ooc_prefix([](const CollectorOOCPrefixSource&) {}); });
    CollectorConfig pool_config;
    pool_config.use_pool = true;
    pool_config.pool_initial_bytes = 4096;
    RelationCollector pool_collector(pool_config);
    check_logic_error(
        [&]() { pool_collector.with_ooc_prefix([](const CollectorOOCPrefixSource&) {}); });

    const auto handoff_path = make_tmp_ooc_path("borrowed_prefix_handoff_state");
    OOCArtifacts handoff_cleanup(handoff_path);
    CollectorConfig handoff_config;
    handoff_config.ooc_enabled = true;
    handoff_config.ooc_base_path = handoff_path;
    RelationCollector handoff_collector(handoff_config);
    CHECK(handoff_collector.add(make_snapshot_relation(0)));
    RelationCorpus handed_off = handoff_collector.handoff_ooc_corpus(501);
    check_logic_error(
        [&]() { handoff_collector.with_ooc_prefix([](const CollectorOOCPrefixSource&) {}); });
    CHECK(handed_off.count() == 1);

    std::cout << "  Borrowed OOC serialization/state rules: PASS" << std::endl;
}

void test_ooc_corpus_snapshot_append_snapshot_handoff() {
    std::cout << "Testing streaming OOC corpus snapshots and terminal handoff..." << std::endl;
    const auto raw_path = make_tmp_ooc_path("corpus_bridge_raw");
    const auto first_path = make_tmp_ooc_path("corpus_bridge_first");
    const auto second_path = make_tmp_ooc_path("corpus_bridge_second");
    OOCArtifacts raw_cleanup(raw_path);
    OOCSinkLeaseArtifacts first_cleanup(first_path);
    OOCSinkLeaseArtifacts second_cleanup(second_path);

    {
        RelationCorpus handed_off = [&]() {
            CollectorConfig config;
            config.ooc_enabled = true;
            config.ooc_base_path = raw_path;
            RelationCollector collector(config);

            for (int i = 0; i < 3; ++i) {
                CHECK(collector.add(make_snapshot_relation(i)));
            }

            CollectorOOCCorpusSnapshot first_snapshot =
                collector.snapshot_ooc_corpus(101, first_path);
            RelationCorpus& first = first_snapshot.corpus;
            CHECK(first.storage_kind() == RelationStorageKind::FinalizedOOC);
            CHECK(first.logical_generation() == 101);
            CHECK(first.count() == 3);
            CHECK(first.read(0).a == 1);
            CHECK(first.read(2).a == 5);
            CHECK(first_snapshot.source_descriptor.count == 3);
            CHECK(first_snapshot.source_descriptor.store_id != 0);

            for (int i = 3; i < 5; ++i) {
                CHECK(collector.add(make_snapshot_relation(i)));
            }

            CollectorOOCCorpusSnapshot second_snapshot =
                collector.snapshot_ooc_corpus(102, second_path);
            RelationCorpus& second = second_snapshot.corpus;
            CHECK(second.storage_kind() == RelationStorageKind::FinalizedOOC);
            CHECK(second.logical_generation() == 102);
            CHECK(second.count() == 5);
            CHECK(second.read(4).a == 9);
            CHECK(first.count() == 3);
            CHECK(first.read(2).a == 5);
            CHECK(second_snapshot.source_descriptor.store_id ==
                  first_snapshot.source_descriptor.store_id);
            CHECK(second_snapshot.source_descriptor.count == 5);
            CHECK(second_snapshot.source_descriptor.data_end >
                  first_snapshot.source_descriptor.data_end);

            const auto first_scope = first.ooc_artifact_scope();
            const auto second_scope = second.ooc_artifact_scope();
            CHECK(first_scope.has_value());
            CHECK(second_scope.has_value());
            CHECK(first_scope->descriptor.store_id != second_scope->descriptor.store_id);

            RelationCorpus original = collector.handoff_ooc_corpus(103);
            CHECK(original.storage_kind() == RelationStorageKind::FinalizedOOC);
            CHECK(original.logical_generation() == 103);
            CHECK(original.count() == 5);
            CHECK(original.read(4).a == 9);
            const auto original_scope = original.ooc_artifact_scope();
            CHECK(original_scope.has_value());
            CHECK(original_scope->base_path == relation_corpus_detail::freeze_ooc_path(raw_path));
            CHECK(original_scope->descriptor.store_id != first_scope->descriptor.store_id);
            CHECK(original_scope->descriptor.store_id != second_scope->descriptor.store_id);
            CHECK(original_scope->descriptor.store_id ==
                  second_snapshot.source_descriptor.store_id);
            CHECK(original_scope->descriptor.count == second_snapshot.source_descriptor.count);
            CHECK(original_scope->descriptor.data_end ==
                  second_snapshot.source_descriptor.data_end);

            const auto terminal_stats = collector.stats();
            CHECK(collector.size() == 5);
            CHECK(!collector.empty());
            check_logic_error([&]() { (void)collector.add(make_snapshot_relation(20)); });
            check_logic_error([&]() { collector.clear(); });
            check_logic_error([&]() { (void)collector.snapshot_relations(); });
            check_logic_error([&]() { (void)collector.finalize_relations(); });
            check_logic_error([&]() { (void)collector.finalize_ooc(); });
            check_logic_error([&]() { (void)collector.checkpoint_ooc(); });
            check_logic_error([&]() { collector.resume_ooc(second_snapshot.source_descriptor); });
            RelationCollector merge_source;
            CHECK(merge_source.add(make_snapshot_relation(21)));
            check_logic_error([&]() { (void)collector.merge(merge_source); });
            RelationCollector merge_destination;
            check_logic_error([&]() { (void)merge_destination.merge(collector); });
            check_logic_error([&]() {
                (void)collector.snapshot_ooc_corpus(104,
                                                    make_tmp_ooc_path("after_handoff_snapshot"));
            });
            check_logic_error([&]() { (void)collector.handoff_ooc_corpus(104); });
            check_stats_equal(collector.stats(), terminal_stats);
            return original;
        }();

        // The moved corpus owns the exact original V3 pair after the collector
        // is gone and remains readable for its full independent lifetime.
        CHECK(std::filesystem::exists(raw_path + ".relidx"));
        CHECK(std::filesystem::exists(raw_path + ".reldata"));
        CHECK(handed_off.count() == 5);
        CHECK(handed_off.read(0).a == 1);
    }
    CHECK(!std::filesystem::exists(raw_path + ".relidx"));
    CHECK(!std::filesystem::exists(raw_path + ".reldata"));

    std::cout << "  Streaming OOC corpus snapshots/handoff: PASS" << std::endl;
}

void test_ooc_corpus_snapshot_collision_is_retryable() {
    std::cout << "Testing OOC corpus destination collision retry..." << std::endl;
    const auto raw_path = make_tmp_ooc_path("corpus_collision_raw");
    const auto destination_path = make_tmp_ooc_path("corpus_collision_destination");
    OOCArtifacts raw_cleanup(raw_path);
    OOCSinkLeaseArtifacts destination_cleanup(destination_path);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = raw_path;
    RelationCollector collector(config);
    CHECK(collector.add(make_snapshot_relation(0)));

    const auto lease_root = RelationSink::lease_root_for(destination_path);
    CHECK(std::filesystem::create_directory(lease_root));
    bool collision_rejected = false;
    try {
        (void)collector.snapshot_ooc_corpus(201, destination_path);
    } catch (const std::runtime_error&) {
        collision_rejected = true;
    }
    CHECK(collision_rejected);

    // Reservation failed before checkpointing, so the source remains Open.
    CHECK(collector.add(make_snapshot_relation(1)));
    CHECK(std::filesystem::remove(lease_root));

    CollectorOOCCorpusSnapshot retry = collector.snapshot_ooc_corpus(202, destination_path);
    CHECK(retry.source_descriptor.count == 2);
    CHECK(retry.corpus.count() == 2);
    CHECK(retry.corpus.read(0).a == 1);
    CHECK(retry.corpus.read(1).a == 3);

    // Successful copy also resumes the exact prefix before returning.
    CHECK(collector.add(make_snapshot_relation(2)));
    RelationCorpus original = collector.handoff_ooc_corpus(203);
    CHECK(original.count() == 3);

    std::cout << "  OOC corpus collision retry: PASS" << std::endl;
}

void test_ooc_corpus_handoff_adoption_retry_and_identity() {
    std::cout << "Testing OOC corpus handoff adoption retry and identity..." << std::endl;
    const auto raw_path = make_tmp_ooc_path("corpus_handoff_retry");
    const auto configured_raw_path = (std::filesystem::path(raw_path).parent_path() / "." /
                                      std::filesystem::path(raw_path).filename())
                                         .string();
    OOCArtifacts raw_cleanup(raw_path);

    {
        CollectorConfig config;
        config.ooc_enabled = true;
        config.ooc_base_path = configured_raw_path;
        RelationCollector collector(config);
        CHECK(collector.add(make_snapshot_relation(0)));
        CHECK(collector.add(make_snapshot_relation(1)));

        const auto finalized = collector.finalize_ooc();
        CHECK(finalized.has_value());

        bool adoption_rejected = false;
        try {
            (void)collector.handoff_ooc_corpus(0);
        } catch (const std::invalid_argument&) {
            adoption_rejected = true;
        }
        CHECK(adoption_rejected);

        // Failed corpus construction retains the idempotent Finalized writer
        // and descriptor, including physical store identity, for retry.
        const auto retained = collector.finalize_ooc();
        CHECK(retained == finalized);
        CHECK(collector.snapshot_relations().size() == 2);

        RelationCorpus corpus = collector.handoff_ooc_corpus(301);
        const auto scope = corpus.ooc_artifact_scope();
        CHECK(scope.has_value());
        CHECK(scope->base_path == relation_corpus_detail::freeze_ooc_path(raw_path));
        CHECK(scope->descriptor == *finalized);
        CHECK(corpus.count() == 2);
        CHECK(corpus.read(1).a == 3);
        check_logic_error([&]() { (void)collector.handoff_ooc_corpus(302); });
        check_logic_error([&]() { (void)collector.finalize_ooc(); });
    }
    CHECK(!std::filesystem::exists(raw_path + ".relidx"));
    CHECK(!std::filesystem::exists(raw_path + ".reldata"));

    std::cout << "  OOC corpus handoff retry/identity: PASS" << std::endl;
}

void test_ooc_corpus_bridge_rejects_vector_mode() {
    std::cout << "Testing OOC corpus bridge rejects vector mode..." << std::endl;
    const auto destination_path = make_tmp_ooc_path("vector_bridge_rejection");
    OOCSinkLeaseArtifacts destination_cleanup(destination_path);

    RelationCollector collector;
    CHECK(collector.add(make_snapshot_relation(0)));
    check_logic_error([&]() { (void)collector.snapshot_ooc_corpus(401, destination_path); });
    check_logic_error([&]() { (void)collector.handoff_ooc_corpus(401); });
    CHECK(!std::filesystem::exists(RelationSink::lease_root_for(destination_path)));

    // Existing in-memory compatibility remains intentionally appendable.
    CHECK(collector.add(make_snapshot_relation(1)));
    CHECK(collector.snapshot_relations().size() == 2);

    std::cout << "  Vector-mode OOC bridge rejection: PASS" << std::endl;
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
    check_logic_error([&]() { collector.clear(); });
    CHECK(std::filesystem::exists(path + ".relidx"));
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
        data.seekp(static_cast<std::streamoff>(OOCRelationWriter::DATA_HEADER_BYTES + 16));
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
    constexpr std::array<std::streamoff, 5> count_offsets = {
        static_cast<std::streamoff>(OOCRelationWriter::DATA_HEADER_BYTES + 16),
        static_cast<std::streamoff>(OOCRelationWriter::DATA_HEADER_BYTES + 20),
        static_cast<std::streamoff>(OOCRelationWriter::DATA_HEADER_BYTES + 24),
        static_cast<std::streamoff>(OOCRelationWriter::DATA_HEADER_BYTES + 28),
        static_cast<std::streamoff>(OOCRelationWriter::DATA_HEADER_BYTES + 32),
    };
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
        index.seekp(
            static_cast<std::streamoff>(OOCRelationWriter::INDEX_HEADER_BYTES + sizeof(uint64_t)));
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
        const auto second_offset_position =
            static_cast<std::streamoff>(OOCRelationWriter::INDEX_HEADER_BYTES + sizeof(uint64_t));
        index.seekg(second_offset_position);
        index.read(reinterpret_cast<char*>(&original_second_offset), 8);
        CHECK(static_cast<bool>(index));
        const uint64_t corrupt_offset = 0;
        index.seekp(second_offset_position);
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
        index.seekp(
            static_cast<std::streamoff>(OOCRelationWriter::INDEX_HEADER_BYTES + sizeof(uint64_t)));
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
// Paired OOC V3 resume tests (persisted by SieveCheckpoint V3).
// ──────────────────────────────────────────────────────────────────────────

void test_ooc_collector_rejects_legacy_resume_flag() {
    auto path = make_tmp_ooc_path("legacy_resume_flag");
    OOCArtifacts cleanup(path);
    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;
    config.ooc_resume = true;

    bool rejected = false;
    try {
        RelationCollector collector(config);
        (void)collector;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(!std::filesystem::exists(path + ".relidx"));
    CHECK(!std::filesystem::exists(path + ".reldata"));
}

void test_ooc_writer_resume_append() {
    std::cout << "Testing paired OOC writer recovery and append..." << std::endl;
    auto path = make_tmp_ooc_path("resume_append");
    OOCArtifacts cleanup(path);

    OOCSnapshotDescriptor descriptor;
    RelationSequenceReceiptAccumulator committed_sequence;
    {
        OOCRelationWriter writer(path);
        for (int i = 1; i <= 3; ++i) {
            Relation r(i * 10, static_cast<uint64_t>(i * 10 + 1));
            r.rational_factors.push_back(static_cast<uint32_t>(i));
            CHECK(writer.write(r) == static_cast<size_t>(i - 1));
            committed_sequence.append(r);
        }
        descriptor = writer.checkpoint_prefix();
        writer.fail_suspended_snapshot();
    }

    {
        OOCRelationWriter writer(path, descriptor, committed_sequence.finish());
        CHECK(writer.recovery_outcome() == OOCRecoveryOutcome::AppendablePrefix);
        CHECK(writer.count() == 3);
        for (int i = 4; i <= 5; ++i) {
            Relation r(i * 10, static_cast<uint64_t>(i * 10 + 1));
            r.rational_factors.push_back(static_cast<uint32_t>(i));
            CHECK(writer.write(r) == static_cast<size_t>(i - 1));
        }
        CHECK(writer.finalize().count == 5);
    }

    // Reader 看到 5 个 rel, 顺序正确
    OOCRelationReader reader(path);
    CHECK(reader.count() == 5);
    for (size_t i = 0; i < 5; ++i) {
        auto rel = reader.read(i);
        CHECK(rel.a == static_cast<int64_t>((i + 1) * 10));
        CHECK(rel.b == (i + 1) * 10 + 1);
        CHECK(rel.rational_factors.size() == 1);
        CHECK(rel.rational_factors[0] == static_cast<uint32_t>(i + 1));
    }

    std::cout << "  OOC writer resume append: PASS (5 = 3 prior + 2 new)" << std::endl;
}

void test_ooc_writer_finalized_recovery() {
    std::cout << "Testing OOC finalized-corpus recovery..." << std::endl;
    auto path = make_tmp_ooc_path("resume_finalized");
    OOCArtifacts cleanup(path);

    OOCSnapshotDescriptor descriptor;
    RelationSequenceReceiptAccumulator committed_sequence;
    {
        OOCRelationWriter writer(path);
        Relation r(1, 2);
        CHECK(writer.write(r) == 0);
        committed_sequence.append(r);
        descriptor = writer.checkpoint_prefix();
        writer.resume_append(descriptor);
        CHECK(writer.finalize().count == 1);
    }

    OOCRelationWriter recovered(path, descriptor, committed_sequence.finish());
    CHECK(recovered.recovery_outcome() == OOCRecoveryOutcome::FinalizedCorpus);
    CHECK(recovered.state() == OOCWriterState::Finalized);
    CHECK(recovered.count() == 1);

    std::cout << "  OOC finalized-corpus recovery: PASS" << std::endl;
}

void test_ooc_writer_resume_nonexistent_rejected() {
    std::cout << "Testing OOC writer resume rejects nonexistent..." << std::endl;
    auto path = gnfs::util::temp_path(
        "gnfs_test_nonexistent_" + std::to_string(gnfs::util::process_id()) + "_xyz_resume_check");

    OOCSnapshotDescriptor descriptor;
    descriptor.format_version = OOCRelationWriter::FORMAT_VERSION;
    descriptor.store_id = 123;
    descriptor.generation = 1;
    descriptor.data_end = OOCRelationWriter::DATA_HEADER_BYTES;
    bool threw = false;
    try {
        OOCRelationWriter resumed(path, descriptor, RelationSequenceReceiptAccumulator{}.finish());
        (void)resumed;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    std::cout << "  OOC writer resume rejects nonexistent: PASS" << std::endl;
}

void test_ooc_collector_resume_loads_seen() {
    std::cout << "Testing paired OOC collector recovery restores seen set..." << std::endl;
    auto path = make_tmp_ooc_path("collector_resume");
    OOCArtifacts cleanup(path);

    OOCSnapshotDescriptor descriptor;
    RelationSequenceReceiptAccumulator accepted_sequence;
    {
        OOCRelationWriter writer(path);
        for (int i = 1; i <= 3; ++i) {
            Relation r(i * 10, static_cast<uint64_t>(i * 10 + 1));
            r.rational_factors.push_back(static_cast<uint32_t>(i));
            if (i >= 2) {
                r.rational_large_prime.push_back(PrimePower{1009 + static_cast<uint64_t>(i), 0, 1});
            }
            if (i == 3) {
                r.algebraic_large_prime.push_back(PrimePower{2003, 17, 1});
            }
            CHECK(writer.write(r) == static_cast<size_t>(i - 1));
            accepted_sequence.append(r);
        }
        descriptor = writer.checkpoint_prefix();
        writer.fail_suspended_snapshot();
    }

    {
        CollectorConfig cfg;
        cfg.check_duplicates = true;
        cfg.ooc_enabled = true;
        cfg.ooc_resume_snapshot = descriptor;
        cfg.ooc_resume_sequence_receipt = accepted_sequence.finish();
        cfg.ooc_base_path = path;
        RelationCollector collector(cfg);
        CHECK(collector.ooc_recovery_outcome() == OOCRecoveryOutcome::AppendablePrefix);

        CHECK(collector.size() == 3);
        auto stats0 = collector.stats();
        CHECK(stats0.total_relations == 3);
        CHECK(stats0.full_relations == 1);
        CHECK(stats0.partial_1lp == 1);
        CHECK(stats0.partial_2lp == 1);

        // 尝试重 add prior (a,b) — seen_ 拒绝 (dedup)
        Relation dup1(10, 11);
        dup1.rational_factors.push_back(1);
        CHECK(!collector.add(std::move(dup1)));
        Relation dup2(20, 21);
        dup2.rational_factors.push_back(2);
        CHECK(!collector.add(std::move(dup2)));
        CHECK(collector.size() == 3);

        // Add 2 new (a,b) 通过
        for (int i = 4; i <= 5; ++i) {
            Relation r(i * 10, static_cast<uint64_t>(i * 10 + 1));
            r.rational_factors.push_back(static_cast<uint32_t>(i));
            CHECK(collector.add(std::move(r)));
        }
        CHECK(collector.size() == 5);
        const auto stats1 = collector.stats();
        CHECK(stats1.total_relations == 5);
        CHECK(stats1.full_relations == 3);
        CHECK(stats1.partial_1lp == 1);
        CHECK(stats1.partial_2lp == 1);
        const auto final_descriptor = collector.finalize_ooc();
        CHECK(final_descriptor.has_value());
        CHECK(final_descriptor->count == 5);

        check_logic_error([&]() { collector.clear(); });
        CHECK(std::filesystem::exists(path + ".relidx"));
        CHECK(std::filesystem::exists(path + ".reldata"));
        check_logic_error([&]() {
            (void)collector.handoff_ooc_corpus(8'501, OOCCleanupPolicy::RemoveArtifacts);
        });
        {
            auto preserved = collector.handoff_ooc_corpus(8'501, OOCCleanupPolicy::Preserve);
            CHECK(preserved.count() == 5);
            CHECK(!preserved.arm_ooc_cleanup());
        }
        CHECK(std::filesystem::exists(path + ".relidx"));
        CHECK(std::filesystem::exists(path + ".reldata"));
    }

    // Reader 验证 final state
    OOCRelationReader reader(path);
    CHECK(reader.count() == 5);
    for (size_t i = 0; i < 5; ++i) {
        auto rel = reader.read(i);
        CHECK(rel.a == static_cast<int64_t>((i + 1) * 10));
        CHECK(rel.b == (i + 1) * 10 + 1);
    }

    std::cout << "  OOC collector resume + seen restore: PASS" << std::endl;
}

void test_ooc_collector_resume_rejects_sequence_receipt_drift() {
    std::cout << "Testing paired OOC collector receipt drift rejection..." << std::endl;
    const auto path = make_tmp_ooc_path("collector_resume_receipt_drift");
    OOCArtifacts cleanup(path);

    const Relation original = make_snapshot_relation(0);
    RelationSequenceReceiptAccumulator accepted_sequence;
    accepted_sequence.append(original);
    OOCSnapshotDescriptor descriptor;
    {
        OOCRelationWriter writer(path);
        CHECK(writer.write(original) == 0);
        descriptor = writer.checkpoint_prefix();
        writer.resume_append(descriptor);
        CHECK(writer.write(make_snapshot_relation(1)) == 1);
        (void)writer.checkpoint_prefix();
        writer.fail_suspended_snapshot();
    }

    {
        CollectorConfig missing_receipt;
        missing_receipt.ooc_enabled = true;
        missing_receipt.ooc_base_path = path;
        missing_receipt.ooc_resume_snapshot = descriptor;
        bool rejected = false;
        try {
            RelationCollector collector(missing_receipt);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
    }

    {
        std::fstream data(path + ".reldata", std::ios::in | std::ios::out | std::ios::binary);
        CHECK(static_cast<bool>(data));
        constexpr uint32_t replacement_factor = 777;
        constexpr std::streamoff first_rational_factor_offset =
            static_cast<std::streamoff>(OOCRelationWriter::DATA_HEADER_BYTES) +
            static_cast<std::streamoff>(sizeof(int64_t) + sizeof(uint64_t) + sizeof(uint32_t));
        data.seekp(first_rational_factor_offset);
        data.write(reinterpret_cast<const char*>(&replacement_factor), sizeof(replacement_factor));
        data.flush();
        CHECK(static_cast<bool>(data));
    }

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;
    config.ooc_resume_snapshot = descriptor;
    config.ooc_resume_sequence_receipt = accepted_sequence.finish();
    const auto index_size_before = std::filesystem::file_size(path + ".relidx");
    const auto data_size_before = std::filesystem::file_size(path + ".reldata");
    bool drift_rejected = false;
    try {
        RelationCollector collector(config);
    } catch (const std::runtime_error&) {
        drift_rejected = true;
    }
    CHECK(drift_rejected);
    CHECK(std::filesystem::file_size(path + ".relidx") == index_size_before);
    CHECK(std::filesystem::file_size(path + ".reldata") == data_size_before);

    std::cout << "  Paired OOC collector receipt drift rejection: PASS" << std::endl;
}

void test_ooc_collector_resume_empty_files_graceful() {
    std::cout << "Testing paired OOC collector recovery from empty prefix..." << std::endl;
    auto path = make_tmp_ooc_path("collector_resume_empty");
    OOCArtifacts cleanup(path);

    OOCSnapshotDescriptor descriptor;
    RelationSequenceReceiptAccumulator empty_sequence;
    {
        OOCRelationWriter writer(path);
        descriptor = writer.checkpoint_prefix();
        CHECK(descriptor.count == 0);
        writer.fail_suspended_snapshot();
    }

    {
        CollectorConfig cfg;
        cfg.ooc_enabled = true;
        cfg.ooc_resume_snapshot = descriptor;
        cfg.ooc_resume_sequence_receipt = empty_sequence.finish();
        cfg.ooc_base_path = path;
        RelationCollector collector(cfg);
        CHECK(collector.ooc_recovery_outcome() == OOCRecoveryOutcome::AppendablePrefix);
        CHECK(collector.size() == 0);

        // Coprime (a,b): (1,2), (3,4) — gcd 始终 1, 通过 collector validate
        for (int i = 1; i <= 2; ++i) {
            Relation r(2 * i - 1, static_cast<uint64_t>(2 * i));
            CHECK(collector.add(std::move(r)));
        }
        CHECK(collector.size() == 2);
        collector.finalize_ooc();
    }

    OOCRelationReader reader(path);
    CHECK(reader.count() == 2);

    std::cout << "  OOC collector resume from empty: PASS" << std::endl;
}

void test_ooc_collector_recovers_finalized_corpus() {
    std::cout << "Testing collector detects finalized crash window..." << std::endl;
    auto path = make_tmp_ooc_path("collector_finalized_recovery");
    OOCArtifacts cleanup(path);

    OOCSnapshotDescriptor stale_descriptor;
    OOCSnapshotDescriptor descriptor;
    RelationSequenceReceipt stale_sequence_receipt;
    RelationSequenceReceiptAccumulator checkpoint_sequence;
    {
        OOCRelationWriter writer(path);
        const auto checkpoint_relation = make_snapshot_relation(0);
        CHECK(writer.write(checkpoint_relation) == 0);
        checkpoint_sequence.append(checkpoint_relation);
        stale_descriptor = writer.checkpoint_prefix();
        stale_sequence_receipt = checkpoint_sequence.finish();
        writer.resume_append(stale_descriptor);
        const auto terminal_relation = make_snapshot_relation(1);
        CHECK(writer.write(terminal_relation) == 1);
        checkpoint_sequence.append(terminal_relation);
        descriptor = writer.checkpoint_prefix();
        writer.resume_append(descriptor);
        CHECK(writer.finalize().count == 2);
    }

    const auto index_size_before = std::filesystem::file_size(path + ".relidx");
    const auto data_size_before = std::filesystem::file_size(path + ".reldata");
    CollectorConfig stale_config;
    stale_config.ooc_enabled = true;
    stale_config.ooc_base_path = path;
    stale_config.ooc_resume_snapshot = stale_descriptor;
    stale_config.ooc_resume_sequence_receipt = stale_sequence_receipt;
    bool stale_extension_rejected = false;
    try {
        RelationCollector collector(stale_config);
    } catch (const std::runtime_error&) {
        stale_extension_rejected = true;
    }
    CHECK(stale_extension_rejected);
    CHECK(std::filesystem::file_size(path + ".relidx") == index_size_before);
    CHECK(std::filesystem::file_size(path + ".reldata") == data_size_before);

    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = path;
    config.ooc_resume_snapshot = descriptor;
    config.ooc_resume_sequence_receipt = checkpoint_sequence.finish();
    RelationCollector collector(config);
    CHECK(collector.ooc_recovery_outcome() == OOCRecoveryOutcome::FinalizedCorpus);
    CHECK(collector.size() == 2);
    CHECK(collector.stats().total_relations == 2);

    bool append_rejected = false;
    try {
        (void)collector.add(make_snapshot_relation(2));
    } catch (const std::logic_error&) {
        append_rejected = true;
    }
    CHECK(append_rejected);

    const auto recovered_descriptor = collector.finalize_ooc();
    const auto repeated_descriptor = collector.finalize_ooc();
    CHECK(recovered_descriptor.has_value());
    CHECK(repeated_descriptor == recovered_descriptor);
    CHECK(recovered_descriptor->format_version == OOCRelationWriter::FORMAT_VERSION);
    CHECK(recovered_descriptor->store_id == descriptor.store_id);
    CHECK(recovered_descriptor->generation == descriptor.generation);
    CHECK(recovered_descriptor->count == 2);
    CHECK(recovered_descriptor->data_end == descriptor.data_end);

    const auto snapshot = collector.snapshot_relations();
    CHECK(snapshot.size() == 2);
    CHECK(snapshot[0].a == 1);
    CHECK(snapshot[1].a == 3);

    const auto finalized = collector.finalize_relations();
    CHECK(finalized.size() == 2);
    CHECK(finalized[0].a == 1);
    CHECK(finalized[1].a == 3);

    {
        OOCRelationReader expected_reader(path, *recovered_descriptor);
        CHECK(expected_reader.count() == 2);
    }

    bool clear_rejected = false;
    try {
        collector.clear();
    } catch (const std::logic_error&) {
        clear_rejected = true;
    }
    CHECK(clear_rejected);
    CHECK(std::filesystem::exists(path + ".relidx"));
    CHECK(std::filesystem::exists(path + ".reldata"));

    bool remove_handoff_rejected = false;
    try {
        (void)collector.handoff_ooc_corpus(9'001, OOCCleanupPolicy::RemoveArtifacts);
    } catch (const std::logic_error&) {
        remove_handoff_rejected = true;
    }
    CHECK(remove_handoff_rejected);
    CHECK(collector.finalize_ooc() == recovered_descriptor);

    {
        auto preserved = collector.handoff_ooc_corpus(9'001, OOCCleanupPolicy::Preserve);
        CHECK(preserved.count() == 2);
        CHECK(!preserved.arm_ooc_cleanup());
    }
    CHECK(std::filesystem::exists(path + ".relidx"));
    CHECK(std::filesystem::exists(path + ".reldata"));

    std::cout << "  Collector finalized crash-window recovery: PASS" << std::endl;
}

void test_ooc_writer_resume_large_payload() {
    std::cout << "Testing OOC writer resume with variable-size payloads..." << std::endl;
    auto path = make_tmp_ooc_path("resume_large");
    OOCArtifacts cleanup(path);

    OOCSnapshotDescriptor descriptor;
    RelationSequenceReceiptAccumulator committed_sequence;
    {
        OOCRelationWriter writer(path);
        for (int i = 1; i <= 100; ++i) {
            Relation r(i, static_cast<uint64_t>(i + 1000));
            size_t weight = static_cast<size_t>((i % 5) + 1);
            for (size_t j = 0; j < weight; ++j) {
                r.rational_factors.push_back(static_cast<uint32_t>(static_cast<size_t>(i) + j));
            }
            CHECK(writer.write(r) == static_cast<size_t>(i - 1));
            committed_sequence.append(r);
        }
        descriptor = writer.checkpoint_prefix();
        writer.fail_suspended_snapshot();
    }

    {
        OOCRelationWriter writer(path, descriptor, committed_sequence.finish());
        CHECK(writer.count() == 100);
        for (int i = 101; i <= 150; ++i) {
            Relation r(i, static_cast<uint64_t>(i + 1000));
            size_t weight = static_cast<size_t>(((i - 100) % 3) + 2);
            for (size_t j = 0; j < weight; ++j) {
                r.rational_factors.push_back(static_cast<uint32_t>(static_cast<size_t>(i) + j + 7));
            }
            CHECK(writer.write(r) == static_cast<size_t>(i - 1));
        }
        CHECK(writer.finalize().count == 150);
    }

    // Reader 验证 all 150 + payload integrity
    OOCRelationReader reader(path);
    CHECK(reader.count() == 150);
    for (size_t i = 0; i < 100; ++i) {
        auto rel = reader.read(i);
        int idx = static_cast<int>(i) + 1;
        CHECK(rel.a == idx);
        CHECK(rel.b == static_cast<uint64_t>(idx + 1000));
        size_t expected_weight = static_cast<size_t>((idx % 5) + 1);
        CHECK(rel.rational_factors.size() == expected_weight);
        for (size_t j = 0; j < expected_weight; ++j) {
            CHECK(rel.rational_factors[j] == static_cast<uint32_t>(static_cast<size_t>(idx) + j));
        }
    }
    for (size_t i = 100; i < 150; ++i) {
        auto rel = reader.read(i);
        int idx = static_cast<int>(i) + 1;
        CHECK(rel.a == idx);
        CHECK(rel.b == static_cast<uint64_t>(idx + 1000));
        size_t expected_weight = static_cast<size_t>(((idx - 100) % 3) + 2);
        CHECK(rel.rational_factors.size() == expected_weight);
        for (size_t j = 0; j < expected_weight; ++j) {
            CHECK(rel.rational_factors[j] ==
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
    test_output_file_open_failure();
    test_save_load();
    test_load_replaces_state_transactionally(false);
    test_load_replaces_state_transactionally(true);
    test_load_rejects_duplicate_ab_pairs(false);
    test_load_rejects_duplicate_ab_pairs(true);
    test_load_failure_preserves_state_transactionally(false);
    test_load_failure_preserves_state_transactionally(true);
    test_load_respects_max_relations();
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
    test_ooc_fresh_store_refuses_existing_artifacts();
    test_ooc_uncommitted_fresh_exception_cleanup();
    test_ooc_legacy_save_load_disabled();
    test_finalize_ooc_vector_mode_remains_appendable();
    test_ooc_snapshot_append_snapshot_finalize();
    test_ooc_borrowed_prefix_append_and_finalize();
    test_ooc_unique_borrowed_prefix_capability_and_lifecycle();
    test_ooc_unique_borrowed_prefix_rejects_unproven_sources();
    test_ooc_unique_borrowed_prefix_source_corruption_fails_closed();
    test_ooc_unique_borrowed_prefix_resume_failure_takes_precedence();
    test_ooc_borrowed_prefix_callback_failures_resume();
    test_ooc_borrowed_prefix_source_corruption_fails_closed();
    test_ooc_borrowed_prefix_resume_failure_takes_precedence();
    test_ooc_borrowed_prefix_serialization_and_state_rules();
    test_ooc_corpus_snapshot_append_snapshot_handoff();
    test_ooc_corpus_snapshot_collision_is_retryable();
    test_ooc_corpus_handoff_adoption_retry_and_identity();
    test_ooc_corpus_bridge_rejects_vector_mode();
    test_ooc_checkpoint_requires_explicit_resume();
    test_ooc_failed_state_rejects_mutation();
    test_ooc_snapshot_integrity_failure_fails_closed();
    test_ooc_empty_and_repeated_snapshot();
    test_ooc_writer_finalize_state();
    test_ooc_reader_rejects_corrupt_variable_lengths();
    test_ooc_reader_rejects_trailing_bytes();
    test_ooc_prefix_reader_rejects_bad_descriptor_and_offsets();

    std::cout << "\n=== OOC resume mode tests (BACKLOG #11e) ===" << std::endl;
    test_ooc_collector_rejects_legacy_resume_flag();
    test_ooc_writer_resume_append();
    test_ooc_writer_finalized_recovery();
    test_ooc_writer_resume_nonexistent_rejected();
    test_ooc_writer_resume_large_payload();
    test_ooc_collector_resume_loads_seen();
    test_ooc_collector_resume_rejects_sequence_receipt_drift();
    test_ooc_collector_resume_empty_files_graceful();
    test_ooc_collector_recovers_finalized_corpus();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
