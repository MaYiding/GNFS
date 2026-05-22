// Unit tests for include/gnfs/util/integer_scratch_pool.hpp (W8 T5).
//
// Verifies:
//   * ENV parsing for unset / "1" / "0" / "true" / "" / "garbage"
//   * Default-off behavior: borrow returns a fresh Integer, pool stays at 0
//   * Opt-in behavior: borrow returns a 0-initialized Integer; return pushes
//     to pool; subsequent borrow reuses
//   * Pool size growth bounded under repeated borrow-release cycles
//   * Bit-for-bit parity: 1000 random Integer values produce identical
//     multisets through OFF and ON paths
//   * Disabled mode never grows the pool, regardless of borrow count
//   * pool_clear() releases all pooled Integers
//   * Move construction transfers ownership without double-push
//   * Move assignment releases prior + adopts new

// Force assert() to remain live even under -DNDEBUG so Release builds
// do not silently strip verification.
#ifdef NDEBUG
#  undef NDEBUG
#endif

#include "gnfs/util/integer_scratch_pool.hpp"
#include "gnfs/core/integer.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace gnfs::util;
using gnfs::core::Integer;

static void clear_thread_pool() {
    // Reset per-thread pool to a known state before each test.
    integer_scratch_pool_clear();
}

// ────────────────────────────────────────────────────────────────────
// ENV parsing tests
// ────────────────────────────────────────────────────────────────────

static void test_env_unset_default_off() {
    std::cout << "Testing GNFS_INTEGER_SCRATCH_POOL unset => off..." << std::endl;
    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();
    assert(integer_scratch_pool_enabled() == false);
    std::cout << "  ENV unset: PASS (disabled)" << std::endl;
}

static void test_env_one_enabled() {
    std::cout << "Testing GNFS_INTEGER_SCRATCH_POOL=1 => on..." << std::endl;
    setenv("GNFS_INTEGER_SCRATCH_POOL", "1", 1);
    integer_scratch_pool_reset_env_cache_for_testing();
    assert(integer_scratch_pool_enabled() == true);
    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();
    std::cout << "  ENV=1: PASS (enabled)" << std::endl;
}

static void test_env_zero_explicit_off() {
    std::cout << "Testing GNFS_INTEGER_SCRATCH_POOL=0 => off..." << std::endl;
    setenv("GNFS_INTEGER_SCRATCH_POOL", "0", 1);
    integer_scratch_pool_reset_env_cache_for_testing();
    assert(integer_scratch_pool_enabled() == false);
    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();
    std::cout << "  ENV=0: PASS (disabled)" << std::endl;
}

static void test_env_other_values_off() {
    std::cout << "Testing GNFS_INTEGER_SCRATCH_POOL other values => off..." << std::endl;
    const char* others[] = {"true", "yes", "TRUE", "01", "2", "10", "", "on", " 1", "1 "};
    for (const char* v : others) {
        setenv("GNFS_INTEGER_SCRATCH_POOL", v, 1);
        integer_scratch_pool_reset_env_cache_for_testing();
        bool en = integer_scratch_pool_enabled();
        assert(en == false && "Only the exact string \"1\" should enable the pool");
    }
    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();
    std::cout << "  ENV other values: PASS (all disabled)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Behavior tests
// ────────────────────────────────────────────────────────────────────

static void test_borrow_default_off_returns_fresh() {
    std::cout << "Testing borrow (pool off) returns fresh zero Integer..." << std::endl;
    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();
    clear_thread_pool();

    {
        IntegerScratchHandle h;
        assert(h.get().is_zero());
        // Use it like a fresh Integer
        h.get() = Integer(static_cast<int64_t>(42));
        assert(h.get() == Integer(static_cast<int64_t>(42)));
    }
    // Pool stays at 0 size since pool is off
    assert(integer_scratch_pool_size() == 0);

    std::cout << "  borrow OFF: PASS (fresh zero Integer, no pool growth)" << std::endl;
}

static void test_borrow_enabled_returns_zero_initially() {
    std::cout << "Testing borrow (pool on) returns zero-initialized Integer..." << std::endl;
    setenv("GNFS_INTEGER_SCRATCH_POOL", "1", 1);
    integer_scratch_pool_reset_env_cache_for_testing();
    clear_thread_pool();

    {
        IntegerScratchHandle h;
        assert(h.get().is_zero());
    }
    // Pool size should be exactly 1 (one Integer returned)
    assert(integer_scratch_pool_size() == 1);

    // Next borrow should reuse the pooled Integer and re-zero it
    {
        IntegerScratchHandle h2;
        assert(h2.get().is_zero());
        // Use it
        h2.get() = Integer(static_cast<int64_t>(99999));
        assert(h2.get() == Integer(static_cast<int64_t>(99999)));
    }
    // Returned to pool again
    assert(integer_scratch_pool_size() == 1);

    clear_thread_pool();
    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();
    std::cout << "  borrow ON: PASS (zero on borrow, returned to pool)" << std::endl;
}

static void test_return_to_pool() {
    std::cout << "Testing IntegerScratchHandle dtor returns to pool..." << std::endl;
    setenv("GNFS_INTEGER_SCRATCH_POOL", "1", 1);
    integer_scratch_pool_reset_env_cache_for_testing();
    clear_thread_pool();
    assert(integer_scratch_pool_size() == 0);

    // Three scoped borrows. Pool grows to 3 since each handle returns its
    // Integer at scope exit (no reuse occurs because borrow happens after
    // the previous handle is already destroyed only for sequential scope;
    // here we keep all three alive).
    {
        IntegerScratchHandle a;
        a.get() = Integer(static_cast<int64_t>(1));
        IntegerScratchHandle b;
        b.get() = Integer(static_cast<int64_t>(2));
        IntegerScratchHandle c;
        c.get() = Integer(static_cast<int64_t>(3));
        // All three alive: pool still empty (each took or default-constructed)
        assert(integer_scratch_pool_size() == 0);
    }
    // After scope exit: all three pushed back
    assert(integer_scratch_pool_size() == 3);

    clear_thread_pool();
    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();
    std::cout << "  return to pool: PASS (3 borrows -> pool size 3)" << std::endl;
}

static void test_reuse_limb_buffer() {
    std::cout << "Testing many borrow-release cycles do not unbounded grow pool..." << std::endl;
    setenv("GNFS_INTEGER_SCRATCH_POOL", "1", 1);
    integer_scratch_pool_reset_env_cache_for_testing();
    clear_thread_pool();

    // 1000 sequential scoped borrows. Each scope exits before the next
    // borrow, so the pool should never exceed size 1 (one Integer
    // recycled between borrows).
    for (int i = 0; i < 1000; ++i) {
        IntegerScratchHandle h;
        h.get() = Integer(static_cast<int64_t>(i));
        assert(h.get() == Integer(static_cast<int64_t>(i)));
    }
    // After the loop: exactly one Integer in the pool (the recycled slot).
    assert(integer_scratch_pool_size() == 1);

    clear_thread_pool();
    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();
    std::cout << "  reuse limb buffer: PASS (1000 sequential borrows -> pool size 1)" << std::endl;
}

static void test_disabled_no_pool_growth() {
    std::cout << "Testing pool disabled never grows under repeated borrows..." << std::endl;
    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();
    clear_thread_pool();
    assert(integer_scratch_pool_size() == 0);

    for (int i = 0; i < 1000; ++i) {
        IntegerScratchHandle h;
        h.get() = Integer(static_cast<int64_t>(i));
    }
    assert(integer_scratch_pool_size() == 0);

    std::cout << "  disabled no growth: PASS (1000 borrows OFF -> pool size 0)" << std::endl;
}

static void test_bit_for_bit_parity() {
    std::cout << "Testing bit-for-bit parity OFF vs ON over 1000 random values..." << std::endl;

    // Deterministic random value sequence
    std::mt19937_64 rng(0xC0FFEEULL);
    std::vector<int64_t> values;
    values.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        // Mix of small, medium, and large values to exercise different
        // limb sizes (1-2 limbs at most for int64_t, but worth covering).
        int64_t v = static_cast<int64_t>(rng());
        values.push_back(v);
    }

    // OFF path: collect Integers via fresh allocation
    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();
    clear_thread_pool();
    std::vector<std::string> off_outputs;
    off_outputs.reserve(values.size());
    for (int64_t v : values) {
        IntegerScratchHandle h;
        h.get() = Integer(v);
        // Round-trip a non-trivial computation to make sure value
        // behaves identically in pooled state.
        Integer doubled = h.get() + h.get();
        Integer plus_one = doubled + Integer(static_cast<int64_t>(1));
        off_outputs.push_back(plus_one.to_string());
    }

    // ON path: same sequence, pooled
    setenv("GNFS_INTEGER_SCRATCH_POOL", "1", 1);
    integer_scratch_pool_reset_env_cache_for_testing();
    clear_thread_pool();
    std::vector<std::string> on_outputs;
    on_outputs.reserve(values.size());
    for (int64_t v : values) {
        IntegerScratchHandle h;
        h.get() = Integer(v);
        Integer doubled = h.get() + h.get();
        Integer plus_one = doubled + Integer(static_cast<int64_t>(1));
        on_outputs.push_back(plus_one.to_string());
    }

    // Strict equality element-by-element (same input sequence => same output)
    assert(off_outputs.size() == on_outputs.size());
    for (std::size_t i = 0; i < off_outputs.size(); ++i) {
        assert(off_outputs[i] == on_outputs[i]
               && "OFF vs ON path produced different result for some index");
    }

    // Also check as multisets (order-insensitive sanity).
    auto off_sorted = off_outputs;
    auto on_sorted = on_outputs;
    std::sort(off_sorted.begin(), off_sorted.end());
    std::sort(on_sorted.begin(), on_sorted.end());
    assert(off_sorted == on_sorted);

    clear_thread_pool();
    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();
    std::cout << "  bit-for-bit parity: PASS (" << values.size() << " values match)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Edge cases
// ────────────────────────────────────────────────────────────────────

static void test_clear_pool() {
    std::cout << "Testing integer_scratch_pool_clear() releases pooled Integers..." << std::endl;
    setenv("GNFS_INTEGER_SCRATCH_POOL", "1", 1);
    integer_scratch_pool_reset_env_cache_for_testing();
    clear_thread_pool();

    // Push 5 Integers into the pool
    {
        std::vector<IntegerScratchHandle> handles;
        for (int i = 0; i < 5; ++i) {
            handles.emplace_back();
            handles.back().get() = Integer(static_cast<int64_t>(i));
        }
    }
    assert(integer_scratch_pool_size() == 5);

    integer_scratch_pool_clear();
    assert(integer_scratch_pool_size() == 0);

    // Verify the pool still functions after clear
    {
        IntegerScratchHandle h;
        assert(h.get().is_zero());
        h.get() = Integer(static_cast<int64_t>(123));
    }
    assert(integer_scratch_pool_size() == 1);

    clear_thread_pool();
    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();
    std::cout << "  clear pool: PASS" << std::endl;
}

static void test_reset_env_cache() {
    std::cout << "Testing reset_env_cache_for_testing toggles cached value..." << std::endl;

    // Establish initial cached value as OFF
    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();
    assert(integer_scratch_pool_enabled() == false);

    // Change env and reset cache; should now read ON
    setenv("GNFS_INTEGER_SCRATCH_POOL", "1", 1);
    integer_scratch_pool_reset_env_cache_for_testing();
    assert(integer_scratch_pool_enabled() == true);

    // Toggle again
    setenv("GNFS_INTEGER_SCRATCH_POOL", "0", 1);
    integer_scratch_pool_reset_env_cache_for_testing();
    assert(integer_scratch_pool_enabled() == false);

    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();
    std::cout << "  reset env cache: PASS" << std::endl;
}

static void test_move_semantics() {
    std::cout << "Testing IntegerScratchHandle move semantics..." << std::endl;
    setenv("GNFS_INTEGER_SCRATCH_POOL", "1", 1);
    integer_scratch_pool_reset_env_cache_for_testing();
    clear_thread_pool();

    // Move construction: moved-from must not double-push
    {
        IntegerScratchHandle a;
        a.get() = Integer(static_cast<int64_t>(7));
        IntegerScratchHandle b(std::move(a));
        // b owns the Integer now
        assert(b.get() == Integer(static_cast<int64_t>(7)));
        // After scope exit, only b's destructor pushes to pool (1 entry).
    }
    assert(integer_scratch_pool_size() == 1);
    clear_thread_pool();

    // Move assignment: lhs releases old, adopts new
    {
        IntegerScratchHandle a;
        a.get() = Integer(static_cast<int64_t>(10));
        IntegerScratchHandle b;
        b.get() = Integer(static_cast<int64_t>(20));
        // Both alive: pool size 0
        assert(integer_scratch_pool_size() == 0);

        // Move a -> b: b's old Integer is pushed to pool, b adopts a's
        b = std::move(a);
        assert(b.get() == Integer(static_cast<int64_t>(10)));
        // b's old (value 20) is now in the pool
        assert(integer_scratch_pool_size() == 1);
    }
    // After scope: b's destructor pushes (one entry); a's dtor sees
    // returned_=true and skips push. Pool grows from 1 to 2.
    assert(integer_scratch_pool_size() == 2);

    clear_thread_pool();
    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();
    std::cout << "  move semantics: PASS" << std::endl;
}

// Additional perf-info probe (not asserting anything, just informational)
static void perf_info_borrow_loop() {
    std::cout << "Perf-info: 100,000 borrow-release cycles OFF vs ON..." << std::endl;

    constexpr int N = 100000;
    // OFF
    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();
    clear_thread_pool();

    auto t0 = std::chrono::steady_clock::now();
    int64_t accum_off = 0;
    for (int i = 0; i < N; ++i) {
        IntegerScratchHandle h;
        h.get() = Integer(static_cast<int64_t>(i));
        accum_off += h.get().to_int64() & 0xFF;
    }
    auto t1 = std::chrono::steady_clock::now();
    long long ms_off = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // ON
    setenv("GNFS_INTEGER_SCRATCH_POOL", "1", 1);
    integer_scratch_pool_reset_env_cache_for_testing();
    clear_thread_pool();

    auto t2 = std::chrono::steady_clock::now();
    int64_t accum_on = 0;
    for (int i = 0; i < N; ++i) {
        IntegerScratchHandle h;
        h.get() = Integer(static_cast<int64_t>(i));
        accum_on += h.get().to_int64() & 0xFF;
    }
    auto t3 = std::chrono::steady_clock::now();
    long long ms_on = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();

    clear_thread_pool();
    unsetenv("GNFS_INTEGER_SCRATCH_POOL");
    integer_scratch_pool_reset_env_cache_for_testing();

    // Sanity check
    assert(accum_off == accum_on);
    std::cout << "  OFF " << ms_off << " ms, ON " << ms_on << " ms (info only)" << std::endl;
}

int main() {
    std::cout << "=== Integer Scratch Pool Tests (W8 T5) ===" << std::endl;

    // ENV parsing (4)
    std::cout << "\n--- ENV parsing ---" << std::endl;
    test_env_unset_default_off();
    test_env_one_enabled();
    test_env_zero_explicit_off();
    test_env_other_values_off();

    // Borrow / return behavior (5)
    std::cout << "\n--- Borrow / return behavior ---" << std::endl;
    test_borrow_default_off_returns_fresh();
    test_borrow_enabled_returns_zero_initially();
    test_return_to_pool();
    test_reuse_limb_buffer();
    test_disabled_no_pool_growth();

    // Correctness (1)
    std::cout << "\n--- Correctness ---" << std::endl;
    test_bit_for_bit_parity();

    // Edge cases (3)
    std::cout << "\n--- Edge cases ---" << std::endl;
    test_clear_pool();
    test_reset_env_cache();
    test_move_semantics();

    // Perf info (1, no assertion on timing)
    std::cout << "\n--- Perf info ---" << std::endl;
    perf_info_borrow_loop();

    std::cout << "\nAll integer scratch pool tests passed!" << std::endl;
    return 0;
}
