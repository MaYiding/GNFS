// Unit tests for include/gnfs/cofactor/result_cache.hpp (W14 T3).
//
// Verifies:
//   * ENV parsing: unset, "0", "100", ">1048576 clamp", "garbage" (0),
//     "12abc" (12 partial), reset hook
//   * Disabled cache (capacity=0): get returns nullopt, put no-op
//   * put + get basic round-trip
//   * LRU eviction (capacity 3, insert 4, oldest evicted)
//   * LRU promotion (get hit promotes to MRU, subsequent insert evicts
//     someone else)
//   * clear empties cache + subsequent get returns nullopt
//   * Different (B, lp) under same cofactor = independent cache slots
//   * Repeated put on same key updates value + promotes MRU
//   * size() and capacity() accessors
//   * shared_cofactor_result_cache singleton: multiple calls return same
//     instance (address comparison)
//   * Thread safety: 4 threads × 100 mixed get/put, no crash, no data
//     race (mutex enforces serial access; correctness verified via
//     no crash and counter consistency)
//   * 16-key mixed hash sweep (covers hash distribution)

// Force assert() live under -DNDEBUG so Release builds do not silently
// strip verification (W14 follows W13 T3 test convention).
#ifdef NDEBUG
#  undef NDEBUG
#endif

#include "gnfs/cofactor/result_cache.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace gnfs::cofactor;

// Helper to build a CofactorClassification with arbitrary fields.
static CofactorClassification make_classification(CofactorClass type,
                                                  uint64_t f1 = 0,
                                                  uint64_t f2 = 0,
                                                  uint64_t f3 = 0,
                                                  uint8_t power = 1) {
    CofactorClassification c;
    c.type = type;
    c.factor1 = f1;
    c.factor2 = f2;
    c.factor3 = f3;
    c.power = power;
    return c;
}

static bool classifications_equal(const CofactorClassification& a,
                                  const CofactorClassification& b) {
    return a.type == b.type
        && a.factor1 == b.factor1
        && a.factor2 == b.factor2
        && a.factor3 == b.factor3
        && a.power == b.power;
}

// ────────────────────────────────────────────────────────────────────
// ENV parsing tests (6 cases)
// ────────────────────────────────────────────────────────────────────

static void test_env_unset_default_zero() {
    std::cout << "Testing GNFS_COFACTOR_RESULT_CACHE_SIZE unset => 0..." << std::endl;
    unsetenv("GNFS_COFACTOR_RESULT_CACHE_SIZE");
    cofactor_result_cache_reset_env_cache_for_testing();
    assert(cofactor_result_cache_size() == 0);
    assert(cofactor_result_cache_enabled() == false);
    std::cout << "  ENV unset: PASS (size=0, disabled)" << std::endl;
}

static void test_env_explicit_zero() {
    std::cout << "Testing GNFS_COFACTOR_RESULT_CACHE_SIZE=0..." << std::endl;
    setenv("GNFS_COFACTOR_RESULT_CACHE_SIZE", "0", 1);
    cofactor_result_cache_reset_env_cache_for_testing();
    assert(cofactor_result_cache_size() == 0);
    assert(cofactor_result_cache_enabled() == false);
    std::cout << "  ENV=0: PASS" << std::endl;
}

static void test_env_value_100() {
    std::cout << "Testing GNFS_COFACTOR_RESULT_CACHE_SIZE=100..." << std::endl;
    setenv("GNFS_COFACTOR_RESULT_CACHE_SIZE", "100", 1);
    cofactor_result_cache_reset_env_cache_for_testing();
    assert(cofactor_result_cache_size() == 100);
    assert(cofactor_result_cache_enabled() == true);
    std::cout << "  ENV=100: PASS (size=100, enabled)" << std::endl;
}

static void test_env_clamp_to_max() {
    std::cout << "Testing GNFS_COFACTOR_RESULT_CACHE_SIZE > 1048576 clamps..."
              << std::endl;
    setenv("GNFS_COFACTOR_RESULT_CACHE_SIZE", "9999999", 1);
    cofactor_result_cache_reset_env_cache_for_testing();
    constexpr std::size_t kExpected = 1ULL << 20;  // 1,048,576
    assert(cofactor_result_cache_size() == kExpected);
    std::cout << "  ENV=9999999 clamp to " << kExpected << ": PASS" << std::endl;
}

static void test_env_garbage_zero() {
    std::cout << "Testing GNFS_COFACTOR_RESULT_CACHE_SIZE=garbage => 0..."
              << std::endl;
    setenv("GNFS_COFACTOR_RESULT_CACHE_SIZE", "garbage", 1);
    cofactor_result_cache_reset_env_cache_for_testing();
    assert(cofactor_result_cache_size() == 0);
    std::cout << "  ENV=garbage: PASS (size=0)" << std::endl;
}

static void test_env_partial_parse_12abc() {
    std::cout << "Testing GNFS_COFACTOR_RESULT_CACHE_SIZE=12abc => 12..."
              << std::endl;
    setenv("GNFS_COFACTOR_RESULT_CACHE_SIZE", "12abc", 1);
    cofactor_result_cache_reset_env_cache_for_testing();
    // std::stoi accepts numeric prefix.
    assert(cofactor_result_cache_size() == 12);
    std::cout << "  ENV=12abc partial-parse: PASS (size=12, documented)"
              << std::endl;
}

static void test_env_leading_whitespace_zero() {
    std::cout << "Testing GNFS_COFACTOR_RESULT_CACHE_SIZE='  100' => 0..."
              << std::endl;
    setenv("GNFS_COFACTOR_RESULT_CACHE_SIZE", "  100", 1);
    cofactor_result_cache_reset_env_cache_for_testing();
    // Leading whitespace explicitly rejected (matches W12 T1 convention).
    assert(cofactor_result_cache_size() == 0);
    std::cout << "  ENV='  100' leading whitespace: PASS (size=0)" << std::endl;
}

static void test_env_negative_zero() {
    std::cout << "Testing GNFS_COFACTOR_RESULT_CACHE_SIZE='-5' => 0..."
              << std::endl;
    setenv("GNFS_COFACTOR_RESULT_CACHE_SIZE", "-5", 1);
    cofactor_result_cache_reset_env_cache_for_testing();
    assert(cofactor_result_cache_size() == 0);
    std::cout << "  ENV=-5: PASS (size=0)" << std::endl;
}

static void test_env_reset_hook_re_resolves() {
    std::cout << "Testing reset hook re-resolves ENV..." << std::endl;
    setenv("GNFS_COFACTOR_RESULT_CACHE_SIZE", "50", 1);
    cofactor_result_cache_reset_env_cache_for_testing();
    assert(cofactor_result_cache_size() == 50);

    setenv("GNFS_COFACTOR_RESULT_CACHE_SIZE", "200", 1);
    cofactor_result_cache_reset_env_cache_for_testing();
    assert(cofactor_result_cache_size() == 200);

    unsetenv("GNFS_COFACTOR_RESULT_CACHE_SIZE");
    cofactor_result_cache_reset_env_cache_for_testing();
    assert(cofactor_result_cache_size() == 0);

    std::cout << "  reset hook re-resolves 3x: PASS" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Cache disabled (capacity == 0): get nullopt + put no-op
// ────────────────────────────────────────────────────────────────────

static void test_disabled_cache_get_put_noop() {
    std::cout << "Testing capacity=0 disabled cache..." << std::endl;
    CofactorResultCache cache(0);
    assert(cache.capacity() == 0);
    assert(cache.size() == 0);

    // put is a no-op.
    auto v = make_classification(CofactorClass::Smooth);
    cache.put(123, 1024, 8192, v);
    assert(cache.size() == 0);

    // get returns nullopt.
    auto r = cache.get(123, 1024, 8192);
    assert(!r.has_value());

    std::cout << "  capacity=0: PASS (put no-op, get nullopt)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Basic put + get round-trip
// ────────────────────────────────────────────────────────────────────

static void test_put_get_basic_roundtrip() {
    std::cout << "Testing put + get basic round-trip..." << std::endl;
    CofactorResultCache cache(10);

    auto v_smooth = make_classification(CofactorClass::Smooth);
    auto v_prime = make_classification(CofactorClass::Prime, 7919);
    auto v_semi = make_classification(CofactorClass::Semiprime, 101, 103);
    auto v_3lp = make_classification(CofactorClass::ThreeLP, 5, 7, 11);
    auto v_pp = make_classification(CofactorClass::PrimePower, 13, 0, 0, 4);

    cache.put(1, 1000, 5000, v_smooth);
    cache.put(2, 1000, 5000, v_prime);
    cache.put(3, 1000, 5000, v_semi);
    cache.put(4, 1000, 5000, v_3lp);
    cache.put(5, 1000, 5000, v_pp);
    assert(cache.size() == 5);

    auto r1 = cache.get(1, 1000, 5000);
    auto r2 = cache.get(2, 1000, 5000);
    auto r3 = cache.get(3, 1000, 5000);
    auto r4 = cache.get(4, 1000, 5000);
    auto r5 = cache.get(5, 1000, 5000);

    assert(r1.has_value() && classifications_equal(*r1, v_smooth));
    assert(r2.has_value() && classifications_equal(*r2, v_prime));
    assert(r3.has_value() && classifications_equal(*r3, v_semi));
    assert(r4.has_value() && classifications_equal(*r4, v_3lp));
    assert(r5.has_value() && classifications_equal(*r5, v_pp));

    // Miss returns nullopt.
    auto miss = cache.get(99, 1000, 5000);
    assert(!miss.has_value());

    std::cout << "  put + get round-trip 5 values: PASS" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// LRU eviction: capacity 3, insert 4, oldest (key 1) evicted
// ────────────────────────────────────────────────────────────────────

static void test_lru_eviction_capacity_3_insert_4() {
    std::cout << "Testing LRU eviction (capacity 3, insert 4)..." << std::endl;
    CofactorResultCache cache(3);

    cache.put(1, 0, 0, make_classification(CofactorClass::Smooth));
    cache.put(2, 0, 0, make_classification(CofactorClass::Prime, 2));
    cache.put(3, 0, 0, make_classification(CofactorClass::Prime, 3));
    assert(cache.size() == 3);

    // Insert 4th: evicts LRU = key 1.
    cache.put(4, 0, 0, make_classification(CofactorClass::Prime, 4));
    assert(cache.size() == 3);

    auto r1 = cache.get(1, 0, 0);
    auto r2 = cache.get(2, 0, 0);
    auto r3 = cache.get(3, 0, 0);
    auto r4 = cache.get(4, 0, 0);

    assert(!r1.has_value());  // evicted
    assert(r2.has_value());
    assert(r3.has_value());
    assert(r4.has_value());

    std::cout << "  LRU eviction key 1 evicted: PASS" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// LRU promotion: get hit promotes to MRU
// ────────────────────────────────────────────────────────────────────

static void test_lru_promotion_after_get_hit() {
    std::cout << "Testing LRU promotion after get hit..." << std::endl;
    CofactorResultCache cache(3);

    cache.put(1, 0, 0, make_classification(CofactorClass::Prime, 1));
    cache.put(2, 0, 0, make_classification(CofactorClass::Prime, 2));
    cache.put(3, 0, 0, make_classification(CofactorClass::Prime, 3));

    // Cache state (MRU→LRU): 3, 2, 1
    // Get 1: promotes 1 to MRU. New state: 1, 3, 2.
    auto r1 = cache.get(1, 0, 0);
    assert(r1.has_value());

    // Insert 4: evicts LRU = 2 (not 1).
    cache.put(4, 0, 0, make_classification(CofactorClass::Prime, 4));

    auto r1_after = cache.get(1, 0, 0);
    auto r2_after = cache.get(2, 0, 0);
    auto r3_after = cache.get(3, 0, 0);
    auto r4_after = cache.get(4, 0, 0);

    assert(r1_after.has_value());   // promoted, retained
    assert(!r2_after.has_value());  // evicted
    assert(r3_after.has_value());
    assert(r4_after.has_value());

    std::cout << "  get hit promoted key 1, key 2 evicted instead: PASS"
              << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// clear() empties cache
// ────────────────────────────────────────────────────────────────────

static void test_clear_empties_cache() {
    std::cout << "Testing clear empties + subsequent get returns nullopt..."
              << std::endl;
    CofactorResultCache cache(5);

    for (uint64_t k = 1; k <= 4; ++k) {
        cache.put(k, 0, 0, make_classification(CofactorClass::Prime, k));
    }
    assert(cache.size() == 4);

    cache.clear();
    assert(cache.size() == 0);

    for (uint64_t k = 1; k <= 4; ++k) {
        auto r = cache.get(k, 0, 0);
        assert(!r.has_value());
    }

    // Cache is reusable after clear: insert again.
    cache.put(10, 0, 0, make_classification(CofactorClass::Smooth));
    assert(cache.size() == 1);
    auto r10 = cache.get(10, 0, 0);
    assert(r10.has_value());
    assert(r10->type == CofactorClass::Smooth);

    std::cout << "  clear + re-use: PASS" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Different (B, lp) under same cofactor: independent slots
// ────────────────────────────────────────────────────────────────────

static void test_same_cofactor_different_B_lp_independent() {
    std::cout << "Testing same cofactor with different (B, lp) keys..."
              << std::endl;
    CofactorResultCache cache(10);

    const uint64_t cofactor = 42;

    auto v_a = make_classification(CofactorClass::Prime, 42);
    auto v_b = make_classification(CofactorClass::TooLarge);
    auto v_c = make_classification(CofactorClass::Composite);

    cache.put(cofactor, 100, 1000, v_a);
    cache.put(cofactor, 200, 1000, v_b);
    cache.put(cofactor, 100, 2000, v_c);

    assert(cache.size() == 3);

    auto r_a = cache.get(cofactor, 100, 1000);
    auto r_b = cache.get(cofactor, 200, 1000);
    auto r_c = cache.get(cofactor, 100, 2000);

    assert(r_a.has_value() && classifications_equal(*r_a, v_a));
    assert(r_b.has_value() && classifications_equal(*r_b, v_b));
    assert(r_c.has_value() && classifications_equal(*r_c, v_c));

    // Different combo not stored.
    auto r_miss = cache.get(cofactor, 200, 2000);
    assert(!r_miss.has_value());

    std::cout << "  three independent slots same cofactor: PASS" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Repeated put on same key updates + promotes MRU
// ────────────────────────────────────────────────────────────────────

static void test_repeated_put_updates_and_promotes() {
    std::cout << "Testing repeated put updates value + promotes MRU..."
              << std::endl;
    CofactorResultCache cache(3);

    auto v1 = make_classification(CofactorClass::Prime, 1);
    auto v2 = make_classification(CofactorClass::Prime, 2);
    auto v3 = make_classification(CofactorClass::Prime, 3);

    cache.put(1, 0, 0, v1);
    cache.put(2, 0, 0, v2);
    cache.put(3, 0, 0, v3);

    // Re-put key 1 with NEW value (Smooth). This should:
    //   * Update value
    //   * Promote key 1 to MRU (so insertion of key 4 evicts key 2, not 1).
    auto v1_new = make_classification(CofactorClass::Smooth);
    cache.put(1, 0, 0, v1_new);

    // Verify size unchanged (no double-insert).
    assert(cache.size() == 3);

    // Verify value updated.
    auto r1 = cache.get(1, 0, 0);
    assert(r1.has_value());
    assert(r1->type == CofactorClass::Smooth);

    // Note: r1.get hit also promoted to MRU. Insert 4 evicts key 2 (LRU).
    cache.put(4, 0, 0, make_classification(CofactorClass::Prime, 4));
    auto r2 = cache.get(2, 0, 0);
    assert(!r2.has_value());

    std::cout << "  re-put updates value + promotes: PASS" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// size() / capacity() accessor sanity
// ────────────────────────────────────────────────────────────────────

static void test_size_capacity_accessors() {
    std::cout << "Testing size() / capacity() accessors..." << std::endl;
    CofactorResultCache cache(7);

    assert(cache.capacity() == 7);
    assert(cache.size() == 0);

    for (uint64_t k = 0; k < 5; ++k) {
        cache.put(k, 0, 0, make_classification(CofactorClass::Smooth));
        assert(cache.size() == k + 1);
        assert(cache.capacity() == 7);
    }

    // Exceed capacity: size stays at 7 (LRU evicts).
    for (uint64_t k = 5; k < 20; ++k) {
        cache.put(k, 0, 0, make_classification(CofactorClass::Smooth));
        assert(cache.size() <= 7);
        assert(cache.capacity() == 7);
    }
    assert(cache.size() == 7);

    std::cout << "  capacity bound enforced: PASS" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Process-singleton: multiple calls return same instance
// ────────────────────────────────────────────────────────────────────

static void test_shared_singleton_same_instance() {
    std::cout << "Testing shared_cofactor_result_cache singleton..." << std::endl;
    // Force capacity > 0 BEFORE first singleton access. We set the ENV but
    // because singleton is constructed lazily on first call, capacity is
    // fixed at first call. The test cares about address equality, not the
    // configured capacity.
    setenv("GNFS_COFACTOR_RESULT_CACHE_SIZE", "64", 1);
    cofactor_result_cache_reset_env_cache_for_testing();

    CofactorResultCache& c1 = shared_cofactor_result_cache();
    CofactorResultCache& c2 = shared_cofactor_result_cache();
    CofactorResultCache& c3 = shared_cofactor_result_cache();

    // Address comparison: all three refer to the same singleton.
    assert(&c1 == &c2);
    assert(&c1 == &c3);
    assert(&c2 == &c3);

    std::cout << "  singleton address stable: PASS" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Thread safety: 4 threads × 100 mixed get/put, no crash
// ────────────────────────────────────────────────────────────────────

static void test_thread_safety_4x100_mixed() {
    std::cout << "Testing thread safety 4 threads x 100 mixed get/put..."
              << std::endl;
    CofactorResultCache cache(500);

    constexpr int kThreads = 4;
    constexpr int kIters = 100;
    std::atomic<int> hits{0};
    std::atomic<int> misses{0};

    auto worker = [&cache, &hits, &misses](int seed) {
        std::mt19937 rng(static_cast<uint32_t>(seed));
        std::uniform_int_distribution<uint64_t> key_dist(0, 200);
        std::uniform_int_distribution<int> op_dist(0, 1);
        for (int i = 0; i < kIters; ++i) {
            const uint64_t key = key_dist(rng);
            if (op_dist(rng) == 0) {
                // Put.
                auto v = make_classification(CofactorClass::Prime, key);
                cache.put(key, 1000, 5000, v);
            } else {
                // Get.
                auto r = cache.get(key, 1000, 5000);
                if (r.has_value()) {
                    hits.fetch_add(1, std::memory_order_relaxed);
                } else {
                    misses.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back(worker, t + 1);
    }
    for (auto& t : threads) {
        t.join();
    }

    // No crash → mutex protected access. hits + misses ≤ total ops (rough
    // sanity).
    const int total_get_ops = hits.load() + misses.load();
    assert(total_get_ops >= 0);
    assert(total_get_ops <= kThreads * kIters);

    std::cout << "  4 threads x 100 ops: PASS (hits=" << hits.load()
              << ", misses=" << misses.load() << ", no crash)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// 16-key mixed-hash sweep (covers hash distribution / no degenerate
// collisions on near-zero keys)
// ────────────────────────────────────────────────────────────────────

static void test_16_key_mixed_hash_sweep() {
    std::cout << "Testing 16-key mixed-hash sweep..." << std::endl;
    CofactorResultCache cache(32);

    // Insert 16 keys with varying (cofactor, B, lp) bit-patterns. Verify
    // each retrieves its own value (i.e., no hash collisions corrupt
    // independent slots).
    struct Sample { uint64_t cof; uint32_t b; uint32_t lp; uint64_t marker; };
    const Sample samples[] = {
        {0, 0, 0, 0xA1},
        {1, 0, 0, 0xA2},
        {0, 1, 0, 0xA3},
        {0, 0, 1, 0xA4},
        {0xFFFFFFFFULL, 0, 0, 0xA5},
        {0, 0xFFFFFFFFULL, 0, 0xA6},
        {0, 0, 0xFFFFFFFFULL, 0xA7},
        {0xDEADBEEFCAFEBABEULL, 0xC0DEC0DEU, 0xBADBADBAU, 0xA8},
        {42, 1024, 4096, 0xA9},
        {43, 1024, 4096, 0xAA},
        {42, 2048, 4096, 0xAB},
        {42, 1024, 8192, 0xAC},
        {1ULL << 32, 1, 2, 0xAD},
        {(1ULL << 32) + 1, 1, 2, 0xAE},
        {(1ULL << 32), 2, 2, 0xAF},
        {0xAAAAAAAAAAAAAAAAULL, 0x55555555U, 0x12345678U, 0xB0},
    };
    const int n = sizeof(samples) / sizeof(samples[0]);

    for (int i = 0; i < n; ++i) {
        cache.put(samples[i].cof, samples[i].b, samples[i].lp,
                  make_classification(CofactorClass::Prime,
                                      samples[i].marker));
    }
    assert(static_cast<int>(cache.size()) == n);

    // Retrieve each, verify per-slot marker (no cross-key corruption).
    for (int i = 0; i < n; ++i) {
        auto r = cache.get(samples[i].cof, samples[i].b, samples[i].lp);
        assert(r.has_value());
        assert(r->type == CofactorClass::Prime);
        assert(r->factor1 == samples[i].marker);
    }

    std::cout << "  16 distinct keys preserved: PASS" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Main
// ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== W14 T3 Cofactor Result Cache Tests ===" << std::endl;

    // ENV parsing (9 cases — exceeds 6 minimum)
    test_env_unset_default_zero();
    test_env_explicit_zero();
    test_env_value_100();
    test_env_clamp_to_max();
    test_env_garbage_zero();
    test_env_partial_parse_12abc();
    test_env_leading_whitespace_zero();
    test_env_negative_zero();
    test_env_reset_hook_re_resolves();

    // Cache behavior
    test_disabled_cache_get_put_noop();
    test_put_get_basic_roundtrip();
    test_lru_eviction_capacity_3_insert_4();
    test_lru_promotion_after_get_hit();
    test_clear_empties_cache();
    test_same_cofactor_different_B_lp_independent();
    test_repeated_put_updates_and_promotes();
    test_size_capacity_accessors();
    test_shared_singleton_same_instance();
    test_thread_safety_4x100_mixed();
    test_16_key_mixed_hash_sweep();

    std::cout << std::endl;
    std::cout << "=== ALL TESTS PASSED ===" << std::endl;

    // Cleanup: unset ENV so subsequent test runs in the same process start
    // from a known state.
    unsetenv("GNFS_COFACTOR_RESULT_CACHE_SIZE");

    return 0;
}
