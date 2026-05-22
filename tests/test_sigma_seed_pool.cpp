// Unit tests for include/gnfs/cofactor/sigma_seed_pool.hpp (W10 T3).
//
// Verifies:
//   * ENV parsing: unset, "0", "100", "1025" (clamp to 1024), "garbage"
//   * Default OFF behavior: get_next returns fresh fallback unchanged
//   * Enable + empty pool: get_next returns fresh fallback
//   * Enable + refill + drain: pool returns generator outputs in LIFO order
//   * Pool exhaust after partial drain: get_next falls back to fresh
//   * sigma_seed_pool_clear releases all pooled seeds
//   * Multi-thread isolation: per-thread pools do not interfere
//   * Generator exception propagates with consistent partial-fill state
//   * reset_env_cache_for_testing re-reads modified ENV

// Force assert() to remain live even under -DNDEBUG so Release builds
// do not silently strip verification.
#ifdef NDEBUG
#  undef NDEBUG
#endif

#include "gnfs/cofactor/sigma_seed_pool.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace gnfs::cofactor;

// ────────────────────────────────────────────────────────────────────
// ENV parsing tests
// ────────────────────────────────────────────────────────────────────

static void test_env_unset_default_off() {
    std::cout << "Testing GNFS_ECM_SIGMA_POOL_SIZE unset => 0..." << std::endl;
    unsetenv("GNFS_ECM_SIGMA_POOL_SIZE");
    sigma_seed_pool_reset_env_cache_for_testing();
    assert(sigma_seed_pool_size() == 0);
    assert(sigma_seed_pool_enabled() == false);
    std::cout << "  ENV unset: PASS (size=0, disabled)" << std::endl;
}

static void test_env_zero_explicit_off() {
    std::cout << "Testing GNFS_ECM_SIGMA_POOL_SIZE=0 => 0..." << std::endl;
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "0", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    assert(sigma_seed_pool_size() == 0);
    assert(sigma_seed_pool_enabled() == false);
    unsetenv("GNFS_ECM_SIGMA_POOL_SIZE");
    sigma_seed_pool_reset_env_cache_for_testing();
    std::cout << "  ENV=0: PASS (size=0, disabled)" << std::endl;
}

static void test_env_100_enabled() {
    std::cout << "Testing GNFS_ECM_SIGMA_POOL_SIZE=100 => 100..." << std::endl;
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "100", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    assert(sigma_seed_pool_size() == 100);
    assert(sigma_seed_pool_enabled() == true);
    unsetenv("GNFS_ECM_SIGMA_POOL_SIZE");
    sigma_seed_pool_reset_env_cache_for_testing();
    std::cout << "  ENV=100: PASS (size=100, enabled)" << std::endl;
}

static void test_env_1025_clamp_to_1024() {
    std::cout << "Testing GNFS_ECM_SIGMA_POOL_SIZE=1025 => 1024 (clamp)..." << std::endl;
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "1025", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    int s = sigma_seed_pool_size();
    assert(s == 1024 && "1025 must clamp to 1024");
    assert(sigma_seed_pool_enabled() == true);

    // Also test extreme out-of-range (positive overflow but valid int range).
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "999999", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    assert(sigma_seed_pool_size() == 1024);

    unsetenv("GNFS_ECM_SIGMA_POOL_SIZE");
    sigma_seed_pool_reset_env_cache_for_testing();
    std::cout << "  ENV=1025: PASS (clamped to 1024)" << std::endl;
}

static void test_env_garbage_default_off() {
    std::cout << "Testing GNFS_ECM_SIGMA_POOL_SIZE=garbage => 0..." << std::endl;
    // std::stoi throws on leading non-digit, so "garbage" → exception → 0.
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "garbage", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    assert(sigma_seed_pool_size() == 0);

    // Negative parses fine but clamped to 0.
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "-5", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    assert(sigma_seed_pool_size() == 0);

    // Empty string → unset-like behavior.
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    assert(sigma_seed_pool_size() == 0);

    unsetenv("GNFS_ECM_SIGMA_POOL_SIZE");
    sigma_seed_pool_reset_env_cache_for_testing();
    std::cout << "  ENV garbage / negative / empty: PASS (all disabled)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Behavior tests
// ────────────────────────────────────────────────────────────────────

static void test_default_off_get_next_returns_fresh() {
    std::cout << "Testing default OFF get_next returns fresh fallback..." << std::endl;
    unsetenv("GNFS_ECM_SIGMA_POOL_SIZE");
    sigma_seed_pool_reset_env_cache_for_testing();
    sigma_seed_pool_clear();

    assert(sigma_seed_pool_enabled() == false);
    assert(sigma_seed_pool_remaining() == 0);

    const uint64_t fresh = 0xDEADBEEFCAFEBABEULL;
    uint64_t got = get_next_sigma_seed(fresh);
    assert(got == fresh && "OFF path must return fresh fallback unchanged");

    // Try multiple calls; pool size never grows when disabled.
    for (int i = 0; i < 100; ++i) {
        uint64_t f = 0x1000ULL + static_cast<uint64_t>(i);
        uint64_t r = get_next_sigma_seed(f);
        assert(r == f);
    }
    assert(sigma_seed_pool_remaining() == 0);

    // refill_sigma_seed_pool must be no-op when disabled (and generator
    // must NOT be invoked).
    int gen_calls = 0;
    refill_sigma_seed_pool([&gen_calls]() -> uint64_t {
        ++gen_calls;
        return 42;
    });
    assert(gen_calls == 0 && "Generator must not run when pool disabled");
    assert(sigma_seed_pool_remaining() == 0);

    std::cout << "  Default OFF: PASS (fresh returned, refill no-op)" << std::endl;
}

static void test_enable_empty_pool_returns_fresh() {
    std::cout << "Testing enable + empty pool: get_next returns fresh..." << std::endl;
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "10", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    sigma_seed_pool_clear();

    assert(sigma_seed_pool_enabled() == true);
    assert(sigma_seed_pool_remaining() == 0);

    const uint64_t fresh = 0xABCDEF0123456789ULL;
    uint64_t got = get_next_sigma_seed(fresh);
    assert(got == fresh && "Empty enabled pool must fall back to fresh");
    assert(sigma_seed_pool_remaining() == 0 && "Empty pool stays empty after get");

    unsetenv("GNFS_ECM_SIGMA_POOL_SIZE");
    sigma_seed_pool_reset_env_cache_for_testing();
    std::cout << "  Empty pool: PASS (fresh fallback)" << std::endl;
}

static void test_refill_and_drain_lifo_order() {
    std::cout << "Testing refill + drain produces LIFO generator outputs..." << std::endl;
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "8", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    sigma_seed_pool_clear();

    // Deterministic generator emitting [100, 101, 102, ..., 107].
    int gen_counter = 100;
    refill_sigma_seed_pool([&gen_counter]() -> uint64_t {
        return static_cast<uint64_t>(gen_counter++);
    });
    assert(sigma_seed_pool_remaining() == 8);
    assert(gen_counter == 108 && "Generator must run exactly 8 times");

    // Drain: pool is LIFO so we get [107, 106, 105, 104, 103, 102, 101, 100].
    std::vector<uint64_t> drained;
    for (int i = 0; i < 8; ++i) {
        uint64_t f = 0xFFFFFFFFFFFFFFFFULL;  // fallback never expected
        uint64_t s = get_next_sigma_seed(f);
        assert(s != f && "Pool non-empty, must NOT return fresh");
        drained.push_back(s);
    }
    assert(sigma_seed_pool_remaining() == 0);
    for (int i = 0; i < 8; ++i) {
        uint64_t expected = static_cast<uint64_t>(107 - i);
        assert(drained[static_cast<size_t>(i)] == expected && "LIFO order");
    }

    unsetenv("GNFS_ECM_SIGMA_POOL_SIZE");
    sigma_seed_pool_reset_env_cache_for_testing();
    sigma_seed_pool_clear();
    std::cout << "  Refill + drain LIFO: PASS (8 seeds in reverse order)" << std::endl;
}

static void test_pool_exhaust_falls_back_to_fresh() {
    std::cout << "Testing pool exhaust then fresh fallback..." << std::endl;
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "3", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    sigma_seed_pool_clear();

    int gen_counter = 200;
    refill_sigma_seed_pool([&gen_counter]() -> uint64_t {
        return static_cast<uint64_t>(gen_counter++);
    });
    assert(sigma_seed_pool_remaining() == 3);

    // Drain all 3.
    (void)get_next_sigma_seed(0);
    (void)get_next_sigma_seed(0);
    (void)get_next_sigma_seed(0);
    assert(sigma_seed_pool_remaining() == 0);

    // Now pool exhausted: must fall back to fresh on subsequent calls.
    for (int i = 0; i < 5; ++i) {
        uint64_t fresh = 0x12340000ULL + static_cast<uint64_t>(i);
        uint64_t got = get_next_sigma_seed(fresh);
        assert(got == fresh && "Exhausted pool must return fresh");
    }
    assert(sigma_seed_pool_remaining() == 0);

    unsetenv("GNFS_ECM_SIGMA_POOL_SIZE");
    sigma_seed_pool_reset_env_cache_for_testing();
    sigma_seed_pool_clear();
    std::cout << "  Exhaust fallback: PASS" << std::endl;
}

static void test_clear_releases_pool() {
    std::cout << "Testing sigma_seed_pool_clear releases seeds..." << std::endl;
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "20", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    sigma_seed_pool_clear();

    int gen_counter = 0;
    refill_sigma_seed_pool([&gen_counter]() -> uint64_t {
        return static_cast<uint64_t>(gen_counter++);
    });
    assert(sigma_seed_pool_remaining() == 20);

    sigma_seed_pool_clear();
    assert(sigma_seed_pool_remaining() == 0 && "Clear must drop all seeds");

    // After clear, refill should run generator from scratch (count 20).
    int gen2_calls = 0;
    refill_sigma_seed_pool([&gen2_calls]() -> uint64_t {
        return static_cast<uint64_t>(gen2_calls++);
    });
    assert(gen2_calls == 20 && "Refill after clear must repopulate");
    assert(sigma_seed_pool_remaining() == 20);

    unsetenv("GNFS_ECM_SIGMA_POOL_SIZE");
    sigma_seed_pool_reset_env_cache_for_testing();
    sigma_seed_pool_clear();
    std::cout << "  Clear: PASS (20 seeds dropped, repopulated)" << std::endl;
}

static void test_multithread_isolation() {
    std::cout << "Testing multi-thread per-thread pool isolation..." << std::endl;
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "16", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    sigma_seed_pool_clear();

    // Two threads refill from different generator sequences; each must
    // see its own seeds when draining.
    std::atomic<bool> thread_a_ok{false};
    std::atomic<bool> thread_b_ok{false};

    std::thread ta([&thread_a_ok]() {
        // Thread A: gen emits [1000, 1001, ..., 1015]
        sigma_seed_pool_clear();  // ensure clean per-thread start
        int counter = 1000;
        refill_sigma_seed_pool([&counter]() -> uint64_t {
            return static_cast<uint64_t>(counter++);
        });
        if (sigma_seed_pool_remaining() != 16) return;
        // Drain: LIFO gives [1015, 1014, ..., 1000].
        bool ok = true;
        for (int i = 0; i < 16; ++i) {
            uint64_t s = get_next_sigma_seed(0);
            uint64_t expected = static_cast<uint64_t>(1015 - i);
            if (s != expected) { ok = false; break; }
        }
        if (sigma_seed_pool_remaining() != 0) ok = false;
        thread_a_ok = ok;
    });

    std::thread tb([&thread_b_ok]() {
        // Thread B: gen emits [2000, 2001, ..., 2015]
        sigma_seed_pool_clear();
        int counter = 2000;
        refill_sigma_seed_pool([&counter]() -> uint64_t {
            return static_cast<uint64_t>(counter++);
        });
        if (sigma_seed_pool_remaining() != 16) return;
        bool ok = true;
        for (int i = 0; i < 16; ++i) {
            uint64_t s = get_next_sigma_seed(0);
            uint64_t expected = static_cast<uint64_t>(2015 - i);
            if (s != expected) { ok = false; break; }
        }
        if (sigma_seed_pool_remaining() != 0) ok = false;
        thread_b_ok = ok;
    });

    ta.join();
    tb.join();

    assert(thread_a_ok.load() && "Thread A must see its own [1000..1015] seeds");
    assert(thread_b_ok.load() && "Thread B must see its own [2000..2015] seeds");

    // Main thread pool is independent — should still be empty.
    assert(sigma_seed_pool_remaining() == 0 && "Main thread pool untouched");

    unsetenv("GNFS_ECM_SIGMA_POOL_SIZE");
    sigma_seed_pool_reset_env_cache_for_testing();
    sigma_seed_pool_clear();
    std::cout << "  Multi-thread isolation: PASS (both threads see disjoint seeds)" << std::endl;
}

static void test_generator_exception_propagates() {
    std::cout << "Testing generator exception propagates with partial fill..." << std::endl;
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "10", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    sigma_seed_pool_clear();

    // Generator throws on the 5th call.
    int counter = 0;
    bool caught = false;
    try {
        refill_sigma_seed_pool([&counter]() -> uint64_t {
            if (counter == 5) {
                throw std::runtime_error("generator failure on 5th call");
            }
            return static_cast<uint64_t>(counter++);
        });
    } catch (const std::runtime_error&) {
        caught = true;
    }
    assert(caught && "Generator exception must propagate to caller");

    // Pool should contain the 5 successfully generated seeds (0..4).
    // The 5th call threw before push_back, so size is 5 not 6.
    assert(sigma_seed_pool_remaining() == 5 && "Partial fill: 5 seeds before throw");

    // The pool is still in a usable state; we can drain partially.
    uint64_t s = get_next_sigma_seed(0xFFFFULL);
    assert(s == 4 && "Top of partial-fill pool should be the 4th generator output");

    unsetenv("GNFS_ECM_SIGMA_POOL_SIZE");
    sigma_seed_pool_reset_env_cache_for_testing();
    sigma_seed_pool_clear();
    std::cout << "  Generator exception: PASS (propagated, partial fill consistent)" << std::endl;
}

static void test_reset_env_cache_re_reads() {
    std::cout << "Testing reset_env_cache re-reads modified ENV..." << std::endl;

    // Start with unset.
    unsetenv("GNFS_ECM_SIGMA_POOL_SIZE");
    sigma_seed_pool_reset_env_cache_for_testing();
    assert(sigma_seed_pool_size() == 0);

    // Set to 50 and re-read.
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "50", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    assert(sigma_seed_pool_size() == 50);

    // Change to 200 and re-read.
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "200", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    assert(sigma_seed_pool_size() == 200);

    // Unset and re-read.
    unsetenv("GNFS_ECM_SIGMA_POOL_SIZE");
    sigma_seed_pool_reset_env_cache_for_testing();
    assert(sigma_seed_pool_size() == 0);

    std::cout << "  Reset env cache: PASS (multi-mutation reads)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Additional realistic-generator parity test
// ────────────────────────────────────────────────────────────────────

static void test_mt19937_generator_consistent() {
    std::cout << "Testing std::mt19937_64 generator produces consistent pool..." << std::endl;
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "32", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    sigma_seed_pool_clear();

    // Deterministic seed for reproducibility.
    std::mt19937_64 rng(0xABCDEF12345ULL);

    // Pre-compute expected sequence (what generator() would return in order).
    std::vector<uint64_t> expected_in_order;
    expected_in_order.reserve(32);
    {
        std::mt19937_64 rng_copy(0xABCDEF12345ULL);
        for (int i = 0; i < 32; ++i) {
            expected_in_order.push_back(rng_copy());
        }
    }

    // Refill via real PRNG (consumes rng state).
    refill_sigma_seed_pool([&rng]() -> uint64_t {
        return rng();
    });
    assert(sigma_seed_pool_remaining() == 32);

    // Drain LIFO and verify against reversed expected sequence.
    for (int i = 0; i < 32; ++i) {
        uint64_t s = get_next_sigma_seed(0);
        uint64_t expected = expected_in_order[static_cast<size_t>(31 - i)];
        assert(s == expected && "Drained seed must match reversed generator order");
    }
    assert(sigma_seed_pool_remaining() == 0);

    unsetenv("GNFS_ECM_SIGMA_POOL_SIZE");
    sigma_seed_pool_reset_env_cache_for_testing();
    sigma_seed_pool_clear();
    std::cout << "  mt19937_64 round-trip: PASS (32 seeds reversed correctly)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Refill already-full no-op test
// ────────────────────────────────────────────────────────────────────

static void test_refill_already_full_noop() {
    std::cout << "Testing refill no-op when pool already full..." << std::endl;
    setenv("GNFS_ECM_SIGMA_POOL_SIZE", "5", 1);
    sigma_seed_pool_reset_env_cache_for_testing();
    sigma_seed_pool_clear();

    int first_counter = 1000;
    refill_sigma_seed_pool([&first_counter]() -> uint64_t {
        return static_cast<uint64_t>(first_counter++);
    });
    assert(sigma_seed_pool_remaining() == 5);
    assert(first_counter == 1005);

    // Second refill: pool is already at capacity. Generator must NOT run.
    int second_counter = 9000;
    refill_sigma_seed_pool([&second_counter]() -> uint64_t {
        return static_cast<uint64_t>(second_counter++);
    });
    assert(second_counter == 9000 && "Generator must not run when pool full");
    assert(sigma_seed_pool_remaining() == 5);

    // Drain — should give first generator's output (NOT second).
    uint64_t s = get_next_sigma_seed(0);
    assert(s == 1004 && "Top must be from FIRST refill (1004), not second");

    unsetenv("GNFS_ECM_SIGMA_POOL_SIZE");
    sigma_seed_pool_reset_env_cache_for_testing();
    sigma_seed_pool_clear();
    std::cout << "  Refill no-op when full: PASS" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Main
// ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "\n========== Sigma Seed Pool Unit Tests (W10 T3) ==========\n" << std::endl;

    // ENV parsing
    test_env_unset_default_off();
    test_env_zero_explicit_off();
    test_env_100_enabled();
    test_env_1025_clamp_to_1024();
    test_env_garbage_default_off();

    // Behavior
    test_default_off_get_next_returns_fresh();
    test_enable_empty_pool_returns_fresh();
    test_refill_and_drain_lifo_order();
    test_pool_exhaust_falls_back_to_fresh();
    test_clear_releases_pool();
    test_refill_already_full_noop();
    test_multithread_isolation();
    test_generator_exception_propagates();
    test_reset_env_cache_re_reads();
    test_mt19937_generator_consistent();

    std::cout << "\n========== ALL SIGMA SEED POOL TESTS PASSED ==========\n" << std::endl;
    return 0;
}
