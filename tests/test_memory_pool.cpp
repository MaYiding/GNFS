// Unit tests for include/gnfs/util/memory_pool.hpp (W6 T4).
//
// Verifies:
//   * RelationPoolResource basic construction and upstream() access
//   * reset() invalidates prior allocations + accepts new ones
//   * Move construction transfers ownership
//   * std::pmr::vector<int> backed by the pool serves push_back without crash
//   * Chunk overflow (push beyond initial chunk) still succeeds via upstream
//   * Many small allocations do not corrupt the pool
//   * relation_pool_size_bytes() ENV parsing for unset / "0" / positive /
//     negative / overflowing values
//   * relation_pool_enabled() agrees with relation_pool_size_bytes() > 0

// Keep NDEBUG undefined in this translation unit: library headers included
// below may keep their own assert checks live here. The checks of this file
// use TEST_ASSERT and do not depend on NDEBUG.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "gnfs/util/memory_pool.hpp"
#include "support/test_assert.hpp"

#include <cstdlib>
#include <iostream>
#include <memory_resource>
#include <utility>
#include <vector>

using gnfs::test::run_case;
using namespace gnfs::util;

static void test_basic_construction() {
    std::cout << "Testing RelationPoolResource basic construction..." << std::endl;

    // Default chunk size
    RelationPoolResource pool;
    TEST_ASSERT(pool.initial_chunk_bytes() == RelationPoolResource::DEFAULT_INITIAL_CHUNK_BYTES);
    TEST_ASSERT(pool.upstream() != nullptr);

    // Custom chunk size
    RelationPoolResource pool2(64 * 1024);
    TEST_ASSERT(pool2.initial_chunk_bytes() == 64 * 1024);
    TEST_ASSERT(pool2.upstream() != nullptr);

    // Distinct upstream pointers — each pool owns its own resource
    RelationPoolResource pool3;
    TEST_ASSERT(pool.upstream() != pool3.upstream());

    std::cout << "  Basic construction: PASS" << std::endl;
}

static void test_reset_releases_and_reallocates() {
    std::cout << "Testing RelationPoolResource reset() releases and reallocates..." << std::endl;

    RelationPoolResource pool(1024);

    // Allocate enough to commit at least one chunk
    {
        std::pmr::vector<int> v(pool.upstream());
        for (int i = 0; i < 100; ++i)
            v.push_back(i);
        TEST_ASSERT(v.size() == 100);
    }

    // upstream pointer remains stable across reset
    auto* before = pool.upstream();
    pool.reset();
    auto* after = pool.upstream();
    // After reset, the underlying monotonic_buffer_resource is a new instance,
    // so the upstream pointer typically changes; assert that the new pointer
    // is valid and usable rather than equal to the old one.
    (void)before;
    TEST_ASSERT(after != nullptr);

    // Post-reset, fresh allocations succeed
    {
        std::pmr::vector<int> v(pool.upstream());
        for (int i = 0; i < 50; ++i)
            v.push_back(i * 2);
        TEST_ASSERT(v.size() == 50);
        for (int i = 0; i < 50; ++i)
            TEST_ASSERT(v[static_cast<size_t>(i)] == i * 2);
    }

    std::cout << "  Reset releases and reallocates: PASS" << std::endl;
}

static void test_move_construction() {
    std::cout << "Testing RelationPoolResource move semantics..." << std::endl;

    RelationPoolResource pool1(2048);
    auto* before = pool1.upstream();
    TEST_ASSERT(before != nullptr);

    // Allocate a bit
    {
        std::pmr::vector<int> v(pool1.upstream());
        v.push_back(42);
        v.push_back(99);
    }

    // Move-construct pool2 from pool1
    RelationPoolResource pool2(std::move(pool1));
    TEST_ASSERT(pool2.upstream() == before);
    TEST_ASSERT(pool2.initial_chunk_bytes() == 2048);
    // pool1 is now hollow but valid for destruction; upstream is null
    TEST_ASSERT(pool1.upstream() == nullptr);

    // pool2 still serves allocations
    {
        std::pmr::vector<int> v(pool2.upstream());
        for (int i = 0; i < 32; ++i)
            v.push_back(i);
        TEST_ASSERT(v.size() == 32);
    }

    // Move assignment
    RelationPoolResource pool3;
    auto* pool3_orig = pool3.upstream();
    pool3 = std::move(pool2);
    TEST_ASSERT(pool3.upstream() == before); // adopted pool2's resource
    TEST_ASSERT(pool3.upstream() != pool3_orig);

    std::cout << "  Move semantics: PASS" << std::endl;
}

static void test_pmr_vector_int_basic() {
    std::cout << "Testing std::pmr::vector<int> backed by pool..." << std::endl;

    RelationPoolResource pool(8192);
    std::pmr::vector<int> v(pool.upstream());

    // Sequential push_back
    for (int i = 0; i < 1000; ++i)
        v.push_back(i);
    TEST_ASSERT(v.size() == 1000);
    for (int i = 0; i < 1000; ++i)
        TEST_ASSERT(v[static_cast<size_t>(i)] == i);

    // Clear and refill
    v.clear();
    TEST_ASSERT(v.empty());
    for (int i = 0; i < 500; ++i)
        v.push_back(i * 3);
    TEST_ASSERT(v.size() == 500);
    for (int i = 0; i < 500; ++i)
        TEST_ASSERT(v[static_cast<size_t>(i)] == i * 3);

    std::cout << "  pmr vector<int> backed by pool: PASS" << std::endl;
}

static void test_chunk_overflow() {
    std::cout << "Testing chunk overflow (allocation beyond initial chunk)..." << std::endl;

    // Tiny initial chunk to force overflow quickly
    RelationPoolResource pool(64);
    std::pmr::vector<int> v(pool.upstream());

    // Push enough ints to clearly exceed 64-byte initial chunk
    // Each int is 4 bytes; std::pmr::vector also grows geometrically, so we
    // push 10000 ints to ensure multiple overflow allocations.
    for (int i = 0; i < 10000; ++i)
        v.push_back(i);
    TEST_ASSERT(v.size() == 10000);
    // Spot-check the data is intact across chunk boundaries
    TEST_ASSERT(v[0] == 0);
    TEST_ASSERT(v[5000] == 5000);
    TEST_ASSERT(v[9999] == 9999);

    std::cout << "  Chunk overflow: PASS (10000 ints, initial chunk 64B)" << std::endl;
}

static void test_many_small_objects() {
    std::cout << "Testing many small object allocations..." << std::endl;

    RelationPoolResource pool(4096);
    std::pmr::vector<int> v(pool.upstream());

    // Repeated push -- pool should handle without corruption
    constexpr int N = 50000;
    for (int i = 0; i < N; ++i)
        v.push_back(i ^ 0x5A5A5A5A);

    TEST_ASSERT(v.size() == N);
    // Verify a few entries
    for (int i = 0; i < N; i += 1000) {
        TEST_ASSERT(v[static_cast<size_t>(i)] == (i ^ 0x5A5A5A5A));
    }

    std::cout << "  Many small objects: PASS (" << N << " ints)" << std::endl;
}

static void test_env_unset_returns_zero() {
    std::cout << "Testing GNFS_RELATION_POOL_SIZE unset returns 0..." << std::endl;

    // Note: relation_pool_size_bytes() caches via std::call_once on first
    // invocation. To make this test deterministic regardless of test order,
    // we manipulate the env then re-parse via the test-only reset helper.
    unsetenv("GNFS_RELATION_POOL_SIZE");
    relation_pool_reset_env_cache_for_testing();
    TEST_ASSERT(relation_pool_size_bytes() == 0);
    TEST_ASSERT(!relation_pool_enabled());

    std::cout << "  ENV unset: PASS (returns 0)" << std::endl;
}

static void test_env_zero_returns_zero() {
    std::cout << "Testing GNFS_RELATION_POOL_SIZE=0 returns 0..." << std::endl;

    setenv("GNFS_RELATION_POOL_SIZE", "0", 1);
    relation_pool_reset_env_cache_for_testing();
    TEST_ASSERT(relation_pool_size_bytes() == 0);
    TEST_ASSERT(!relation_pool_enabled());

    unsetenv("GNFS_RELATION_POOL_SIZE");
    std::cout << "  ENV=0: PASS (returns 0)" << std::endl;
}

static void test_env_positive_returns_value() {
    std::cout << "Testing GNFS_RELATION_POOL_SIZE=4194304 returns value..." << std::endl;

    setenv("GNFS_RELATION_POOL_SIZE", "4194304", 1);
    relation_pool_reset_env_cache_for_testing();
    TEST_ASSERT(relation_pool_size_bytes() == 4194304);
    TEST_ASSERT(relation_pool_enabled());

    setenv("GNFS_RELATION_POOL_SIZE", "1024", 1);
    relation_pool_reset_env_cache_for_testing();
    TEST_ASSERT(relation_pool_size_bytes() == 1024);
    TEST_ASSERT(relation_pool_enabled());

    unsetenv("GNFS_RELATION_POOL_SIZE");
    std::cout << "  ENV positive: PASS" << std::endl;
}

static void test_env_non_numeric_returns_zero() {
    std::cout << "Testing GNFS_RELATION_POOL_SIZE non-numeric returns 0..." << std::endl;

    setenv("GNFS_RELATION_POOL_SIZE", "abc", 1);
    relation_pool_reset_env_cache_for_testing();
    TEST_ASSERT(relation_pool_size_bytes() == 0);
    TEST_ASSERT(!relation_pool_enabled());

    setenv("GNFS_RELATION_POOL_SIZE", "", 1);
    relation_pool_reset_env_cache_for_testing();
    TEST_ASSERT(relation_pool_size_bytes() == 0);

    unsetenv("GNFS_RELATION_POOL_SIZE");
    std::cout << "  ENV non-numeric / empty: PASS" << std::endl;
}

static void test_env_negative_returns_zero() {
    std::cout << "Testing GNFS_RELATION_POOL_SIZE negative returns 0..." << std::endl;

    setenv("GNFS_RELATION_POOL_SIZE", "-1", 1);
    relation_pool_reset_env_cache_for_testing();
    TEST_ASSERT(relation_pool_size_bytes() == 0);
    TEST_ASSERT(!relation_pool_enabled());

    // strtoull accepts whitespace before a sign, so cover that spelling too.
    setenv("GNFS_RELATION_POOL_SIZE", "  -42", 1);
    relation_pool_reset_env_cache_for_testing();
    TEST_ASSERT(relation_pool_size_bytes() == 0);

    unsetenv("GNFS_RELATION_POOL_SIZE");
    std::cout << "  ENV negative: PASS" << std::endl;
}

static void test_env_overflow_returns_zero() {
    std::cout << "Testing GNFS_RELATION_POOL_SIZE overflow returns 0..." << std::endl;

    setenv("GNFS_RELATION_POOL_SIZE", "18446744073709551616", 1);
    relation_pool_reset_env_cache_for_testing();
    TEST_ASSERT(relation_pool_size_bytes() == 0);
    TEST_ASSERT(!relation_pool_enabled());

    unsetenv("GNFS_RELATION_POOL_SIZE");
    std::cout << "  ENV overflow: PASS" << std::endl;
}

int main() {
    int failed = 0;

    std::cout << "=== Memory Pool Tests (W6 T4) ===" << std::endl;

    // RelationPoolResource unit tests (6)
    failed += !run_case(test_basic_construction);
    failed += !run_case(test_reset_releases_and_reallocates);
    failed += !run_case(test_move_construction);
    failed += !run_case(test_pmr_vector_int_basic);
    failed += !run_case(test_chunk_overflow);
    failed += !run_case(test_many_small_objects);

    // ENV parsing tests (6: unset / =0 / positive / non-numeric / negative /
    // overflow)
    std::cout << "\n=== ENV parsing tests ===" << std::endl;
    failed += !run_case(test_env_unset_returns_zero);
    failed += !run_case(test_env_zero_returns_zero);
    failed += !run_case(test_env_positive_returns_value);
    failed += !run_case(test_env_non_numeric_returns_zero);
    failed += !run_case(test_env_negative_returns_zero);
    failed += !run_case(test_env_overflow_returns_zero);

    if (failed != 0) {
        std::cout << failed << " test case(s) failed" << std::endl;
        return 1;
    }

    std::cout << "\nAll memory pool tests passed!" << std::endl;
    return 0;
}
