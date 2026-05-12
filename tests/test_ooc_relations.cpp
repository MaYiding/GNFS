// test_ooc_relations.cpp — Out-of-core relation storage correctness tests
//
// Tests write→read round-trip, random access, and compatibility with
// in-memory relation pipeline.

#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/core/relation.hpp>
#include <iostream>
#include <random>
#include <cstdio>

using gnfs::core::Relation;
using gnfs::relation::OOCRelationWriter;
using gnfs::relation::OOCRelationReader;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; \
        tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_PASS(name) do { \
    std::cout << "  PASS: " << name << "\n"; \
    tests_passed++; \
} while(0)

/// Create a test relation with known values
static Relation make_relation(int64_t a, uint64_t b, size_t n_rf, size_t n_af,
                               size_t n_rlp = 0, size_t n_alp = 0) {
    Relation rel;
    rel.a = a;
    rel.b = b;
    for (size_t i = 0; i < n_rf; ++i)
        rel.rational_factors.push_back(static_cast<uint32_t>(100 + i));
    for (size_t i = 0; i < n_af; ++i)
        rel.algebraic_factors.push_back(static_cast<uint32_t>(200 + i));
    for (size_t i = 0; i < n_rlp; ++i)
        rel.rational_large_prime.push_back({1000 + i, 0, 1});
    for (size_t i = 0; i < n_alp; ++i)
        rel.algebraic_large_prime.push_back({2000 + i, 500 + i, 1});
    return rel;
}

/// Compare two relations for equality
static bool relations_equal(const Relation& a, const Relation& b) {
    if (a.a != b.a || a.b != b.b) return false;
    if (a.rational_factors != b.rational_factors) return false;
    if (a.algebraic_factors != b.algebraic_factors) return false;
    if (a.rational_large_prime.size() != b.rational_large_prime.size()) return false;
    for (size_t i = 0; i < a.rational_large_prime.size(); ++i) {
        if (a.rational_large_prime[i].p != b.rational_large_prime[i].p) return false;
        if (a.rational_large_prime[i].r != b.rational_large_prime[i].r) return false;
        if (a.rational_large_prime[i].e != b.rational_large_prime[i].e) return false;
    }
    if (a.algebraic_large_prime.size() != b.algebraic_large_prime.size()) return false;
    for (size_t i = 0; i < a.algebraic_large_prime.size(); ++i) {
        if (a.algebraic_large_prime[i].p != b.algebraic_large_prime[i].p) return false;
        if (a.algebraic_large_prime[i].r != b.algebraic_large_prime[i].r) return false;
        if (a.algebraic_large_prime[i].e != b.algebraic_large_prime[i].e) return false;
    }
    if (a.extra_ab_pairs != b.extra_ab_pairs) return false;
    return true;
}

/// RAII temp file cleanup
struct TempFiles {
    std::string base;
    TempFiles(const std::string& b) : base(b) {}
    ~TempFiles() {
        std::remove((base + ".reldata").c_str());
        std::remove((base + ".relidx").c_str());
    }
};

// ============================================================================
// Tests
// ============================================================================

void test_write_read_single() {
    TempFiles tmp("/tmp/gnfs_test_ooc_single");

    Relation orig = make_relation(42, 7, 5, 3, 1, 2);
    orig.extra_ab_pairs.push_back({10, 20});

    {
        OOCRelationWriter writer(tmp.base);
        writer.write(orig);
        writer.close();
    }

    OOCRelationReader reader(tmp.base);
    TEST_ASSERT(reader.count() == 1, "should have 1 relation");

    auto loaded = reader.read(0);
    TEST_ASSERT(relations_equal(orig, loaded), "loaded relation should match original");

    TEST_PASS("write/read single relation");
}

void test_write_read_batch() {
    TempFiles tmp("/tmp/gnfs_test_ooc_batch");

    std::vector<Relation> originals;
    {
        OOCRelationWriter writer(tmp.base);
        for (int i = 0; i < 1000; ++i) {
            auto rel = make_relation(
                static_cast<int64_t>(i * 3 - 500),
                static_cast<uint64_t>(i + 1),
                static_cast<size_t>(3 + i % 5),
                static_cast<size_t>(2 + i % 4),
                static_cast<size_t>(i % 3),
                static_cast<size_t>(i % 2)
            );
            originals.push_back(rel);
            writer.write(rel);
        }
        writer.close();
    }

    OOCRelationReader reader(tmp.base);
    TEST_ASSERT(reader.count() == 1000, "should have 1000 relations");

    // Verify all relations round-trip correctly
    for (size_t i = 0; i < 1000; ++i) {
        auto loaded = reader.read(i);
        TEST_ASSERT(relations_equal(originals[i], loaded),
                    "relation " + std::to_string(i) + " should match");
    }

    TEST_PASS("write/read 1000 relations batch");
}

void test_random_access() {
    TempFiles tmp("/tmp/gnfs_test_ooc_random");

    std::vector<Relation> originals;
    {
        OOCRelationWriter writer(tmp.base);
        for (int i = 0; i < 500; ++i) {
            auto rel = make_relation(i, static_cast<uint64_t>(i * 7), 4, 3, 1, 1);
            originals.push_back(rel);
            writer.write(rel);
        }
        writer.close();
    }

    OOCRelationReader reader(tmp.base);

    // Access in random order
    std::mt19937 rng(12345);
    for (int trial = 0; trial < 200; ++trial) {
        size_t idx = rng() % 500;
        auto loaded = reader.read(idx);
        TEST_ASSERT(relations_equal(originals[idx], loaded),
                    "random access idx=" + std::to_string(idx));
    }

    TEST_PASS("random access 200 reads from 500 relations");
}

void test_read_all() {
    TempFiles tmp("/tmp/gnfs_test_ooc_readall");

    std::vector<Relation> originals;
    {
        OOCRelationWriter writer(tmp.base);
        for (int i = 0; i < 100; ++i) {
            auto rel = make_relation(i, static_cast<uint64_t>(i), 3, 2);
            originals.push_back(rel);
            writer.write(rel);
        }
        writer.close();
    }

    OOCRelationReader reader(tmp.base);
    auto all = reader.read_all();
    TEST_ASSERT(all.size() == 100, "read_all should return 100");

    for (size_t i = 0; i < 100; ++i) {
        TEST_ASSERT(relations_equal(originals[i], all[i]),
                    "read_all[" + std::to_string(i) + "] mismatch");
    }

    TEST_PASS("read_all() round-trip 100 relations");
}

void test_read_range() {
    TempFiles tmp("/tmp/gnfs_test_ooc_range");

    {
        OOCRelationWriter writer(tmp.base);
        for (int i = 0; i < 50; ++i) {
            writer.write(make_relation(i, static_cast<uint64_t>(i), 2, 2));
        }
        writer.close();
    }

    OOCRelationReader reader(tmp.base);
    auto range = reader.read_range(10, 20);
    TEST_ASSERT(range.size() == 10, "range [10,20) should have 10");
    TEST_ASSERT(range[0].a == 10, "first in range should be a=10");
    TEST_ASSERT(range[9].a == 19, "last in range should be a=19");

    TEST_PASS("read_range [10,20) from 50 relations");
}

void test_empty_relations() {
    TempFiles tmp("/tmp/gnfs_test_ooc_empty");

    // Relation with no factors, no LPs, no extras
    Relation empty_rel;
    empty_rel.a = 1;
    empty_rel.b = 2;

    {
        OOCRelationWriter writer(tmp.base);
        writer.write(empty_rel);
        writer.close();
    }

    OOCRelationReader reader(tmp.base);
    auto loaded = reader.read(0);
    TEST_ASSERT(loaded.a == 1 && loaded.b == 2, "core fields match");
    TEST_ASSERT(loaded.rational_factors.empty(), "no rational factors");
    TEST_ASSERT(loaded.algebraic_factors.empty(), "no algebraic factors");
    TEST_ASSERT(loaded.rational_large_prime.empty(), "no rational LPs");
    TEST_ASSERT(loaded.algebraic_large_prime.empty(), "no algebraic LPs");
    TEST_ASSERT(loaded.extra_ab_pairs.empty(), "no extra pairs");

    TEST_PASS("empty relation (no factors/LPs/extras)");
}

void test_merged_relation_with_extras() {
    TempFiles tmp("/tmp/gnfs_test_ooc_merged");

    Relation merged = make_relation(100, 200, 10, 8, 2, 3);
    merged.extra_ab_pairs = {{1,2}, {3,4}, {5,6}, {7,8}};

    {
        OOCRelationWriter writer(tmp.base);
        writer.write(merged);
        writer.close();
    }

    OOCRelationReader reader(tmp.base);
    auto loaded = reader.read(0);
    TEST_ASSERT(loaded.extra_ab_pairs.size() == 4, "should have 4 extra pairs");
    TEST_ASSERT(loaded.extra_ab_pairs[2].first == 5, "extra pair [2] a=5");
    TEST_ASSERT(loaded.extra_ab_pairs[2].second == 6, "extra pair [2] b=6");
    TEST_ASSERT(relations_equal(merged, loaded), "merged relation round-trip");

    TEST_PASS("merged relation with 4 extra (a,b) pairs");
}

// Writer 析构时若有 in-flight exception,只写入 MAGIC_INCOMPLETE,
// reader 必须拒绝读(避免 idx/data 不一致)。
void test_writer_exception_path() {
    TempFiles tmp("/tmp/gnfs_test_ooc_exc");

    bool reader_threw = false;
    try {
        try {
            OOCRelationWriter writer(tmp.base);
            Relation r = make_relation(1, 2, 3, 2);
            writer.write(r);
            writer.write(r);
            // 模拟 write 中途异常:抛出后,析构期间 std::uncaught_exceptions()
            // 比 ctor 时多 1,close() 走异常分支不写 MAGIC。
            throw std::runtime_error("simulated disk-full mid-write");
        } catch (const std::runtime_error&) {
            // swallow — Writer 析构已发生
        }

        try {
            OOCRelationReader reader(tmp.base);
            (void)reader;
        } catch (const std::runtime_error& e) {
            std::string msg = e.what();
            if (msg.find("invalid magic") != std::string::npos) {
                reader_threw = true;
            }
        }
    } catch (...) {}
    TEST_ASSERT(reader_threw, "reader should reject MAGIC_INCOMPLETE file");
    TEST_PASS("writer exception path → reader rejects incomplete file");
}

int main() {
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  Out-of-core Relations Unit Tests\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    test_write_read_single();
    test_write_read_batch();
    test_random_access();
    test_read_all();
    test_read_range();
    test_empty_relations();
    test_merged_relation_with_extras();
    test_writer_exception_path();

    std::cout << "\n═══════════════════════════════════════════\n";
    std::cout << "  Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
    std::cout << "═══════════════════════════════════════════\n";

    return tests_failed > 0 ? 1 : 0;
}
