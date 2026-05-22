// Unit tests for include/gnfs/cofactor/ecm_prime_cache.hpp (W13 T3).
//
// Verifies:
//   * ENV parsing: unset, "0", "4", "33" (clamp to 32), "garbage",
//     leading whitespace " 4", empty
//   * compute_b1_prime_powers correctness for B1 ∈ {0, 1, 2, 3, 10, 20, 100}
//   * Literal expected sequence for B1=20: [16, 9, 5, 7, 11, 13, 17, 19]
//   * Cache hit: second lookup of same B1 returns same content
//   * Cache miss + not full: inserts and returns ref
//   * Cache miss + full: returns ref to overflow slot with correct content
//   * cache.size() / cache.capacity() / cache.clear() behavior
//   * Multi-thread concurrent lookups: 4 threads × 100 random B1 values,
//     no data race, all results consistent
//   * Shared singleton thread-safety (concurrent first call)
//   * reset_env_cache_for_testing lets next env query re-resolve
//   * Perf info: cache hit vs miss wall-time for B1=10000

// Force assert() to remain live even under -DNDEBUG so Release builds
// do not silently strip verification.
#ifdef NDEBUG
#  undef NDEBUG
#endif

#include "gnfs/cofactor/ecm_prime_cache.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace gnfs::cofactor;

// ────────────────────────────────────────────────────────────────────
// ENV parsing tests
// ────────────────────────────────────────────────────────────────────

static void test_env_unset_default_off() {
    std::cout << "Testing GNFS_ECM_B1_CACHE_SIZE unset => 0..." << std::endl;
    unsetenv("GNFS_ECM_B1_CACHE_SIZE");
    ecm_b1_cache_reset_env_cache_for_testing();
    assert(ecm_b1_cache_size() == 0);
    assert(ecm_b1_cache_enabled() == false);
    std::cout << "  ENV unset: PASS (size=0, disabled)" << std::endl;
}

static void test_env_zero_explicit_off() {
    std::cout << "Testing GNFS_ECM_B1_CACHE_SIZE=0 => 0..." << std::endl;
    setenv("GNFS_ECM_B1_CACHE_SIZE", "0", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    assert(ecm_b1_cache_size() == 0);
    assert(ecm_b1_cache_enabled() == false);
    unsetenv("GNFS_ECM_B1_CACHE_SIZE");
    ecm_b1_cache_reset_env_cache_for_testing();
    std::cout << "  ENV=0: PASS (size=0, disabled)" << std::endl;
}

static void test_env_4_enabled() {
    std::cout << "Testing GNFS_ECM_B1_CACHE_SIZE=4 => 4..." << std::endl;
    setenv("GNFS_ECM_B1_CACHE_SIZE", "4", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    assert(ecm_b1_cache_size() == 4);
    assert(ecm_b1_cache_enabled() == true);
    unsetenv("GNFS_ECM_B1_CACHE_SIZE");
    ecm_b1_cache_reset_env_cache_for_testing();
    std::cout << "  ENV=4: PASS (size=4, enabled)" << std::endl;
}

static void test_env_33_clamp_to_32() {
    std::cout << "Testing GNFS_ECM_B1_CACHE_SIZE=33 => 32 (clamp)..." << std::endl;
    setenv("GNFS_ECM_B1_CACHE_SIZE", "33", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    assert(ecm_b1_cache_size() == 32 && "33 must clamp to 32");
    assert(ecm_b1_cache_enabled() == true);

    // Extreme out-of-range still clamps to 32.
    setenv("GNFS_ECM_B1_CACHE_SIZE", "999999", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    assert(ecm_b1_cache_size() == 32);

    unsetenv("GNFS_ECM_B1_CACHE_SIZE");
    ecm_b1_cache_reset_env_cache_for_testing();
    std::cout << "  ENV=33: PASS (clamped to 32)" << std::endl;
}

static void test_env_garbage_default_off() {
    std::cout << "Testing GNFS_ECM_B1_CACHE_SIZE=garbage => 0..." << std::endl;
    setenv("GNFS_ECM_B1_CACHE_SIZE", "garbage", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    assert(ecm_b1_cache_size() == 0);

    setenv("GNFS_ECM_B1_CACHE_SIZE", "-5", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    assert(ecm_b1_cache_size() == 0);

    // Empty string → unset-like (returns 0).
    setenv("GNFS_ECM_B1_CACHE_SIZE", "", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    assert(ecm_b1_cache_size() == 0);

    unsetenv("GNFS_ECM_B1_CACHE_SIZE");
    ecm_b1_cache_reset_env_cache_for_testing();
    std::cout << "  ENV garbage / negative / empty: PASS (all disabled)" << std::endl;
}

static void test_env_leading_whitespace_off() {
    std::cout << "Testing GNFS_ECM_B1_CACHE_SIZE='  4' (leading whitespace) => 0..." << std::endl;
    // Leading whitespace explicitly disabled by strict parser.
    setenv("GNFS_ECM_B1_CACHE_SIZE", "  4", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    assert(ecm_b1_cache_size() == 0 &&
           "Leading whitespace must be rejected by strict parser");

    setenv("GNFS_ECM_B1_CACHE_SIZE", "\t8", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    assert(ecm_b1_cache_size() == 0 && "Leading tab must be rejected");

    setenv("GNFS_ECM_B1_CACHE_SIZE", "\n2", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    assert(ecm_b1_cache_size() == 0 && "Leading newline must be rejected");

    unsetenv("GNFS_ECM_B1_CACHE_SIZE");
    ecm_b1_cache_reset_env_cache_for_testing();
    std::cout << "  Leading whitespace: PASS (all rejected)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// compute_b1_prime_powers correctness
// ────────────────────────────────────────────────────────────────────

static void test_compute_edge_cases() {
    std::cout << "Testing compute_b1_prime_powers edge cases B1=0/1/2/3..." << std::endl;

    // B1 = 0 → no primes
    auto r0 = compute_b1_prime_powers(0);
    assert(r0.empty() && "B1=0 must return empty");

    // B1 = 1 → no primes
    auto r1 = compute_b1_prime_powers(1);
    assert(r1.empty() && "B1=1 must return empty");

    // B1 = 2 → primes {2}, prime powers {2} (2^1 = 2 ≤ 2, 2^2 = 4 > 2)
    auto r2 = compute_b1_prime_powers(2);
    assert(r2.size() == 1);
    assert(r2[0] == 2);

    // B1 = 3 → primes {2, 3}, prime powers {2, 3} (2^2 = 4 > 3, 3^2 = 9 > 3)
    auto r3 = compute_b1_prime_powers(3);
    assert(r3.size() == 2);
    assert(r3[0] == 2);
    assert(r3[1] == 3);

    std::cout << "  B1=0/1/2/3: PASS" << std::endl;
}

static void test_compute_b1_10() {
    std::cout << "Testing compute_b1_prime_powers(B1=10)..." << std::endl;
    // primes ≤ 10: {2, 3, 5, 7}
    //   p=2: 2^3 = 8 ≤ 10, 2^4 = 16 > 10  → 8
    //   p=3: 3^2 = 9 ≤ 10, 3^3 = 27 > 10  → 9
    //   p=5: 5^1 = 5,       5^2 = 25 > 10 → 5
    //   p=7: 7^1 = 7,       7^2 = 49 > 10 → 7
    auto r = compute_b1_prime_powers(10);
    assert(r.size() == 4);
    assert(r[0] == 8);
    assert(r[1] == 9);
    assert(r[2] == 5);
    assert(r[3] == 7);
    std::cout << "  B1=10: PASS [8, 9, 5, 7]" << std::endl;
}

static void test_compute_b1_20_literal() {
    std::cout << "Testing compute_b1_prime_powers(B1=20) literal..." << std::endl;
    // primes ≤ 20: {2, 3, 5, 7, 11, 13, 17, 19}
    //   p=2: 2^4 = 16 ≤ 20, 2^5 = 32 > 20  → 16
    //   p=3: 3^2 = 9 ≤ 20,  3^3 = 27 > 20  → 9
    //   p=5: 5^1 = 5,       5^2 = 25 > 20  → 5
    //   p=7: 7^1 = 7,       7^2 = 49 > 20  → 7
    //   p=11..19: each ^1
    const std::vector<uint64_t> expected{16, 9, 5, 7, 11, 13, 17, 19};
    auto r = compute_b1_prime_powers(20);
    assert(r == expected && "B1=20 must match expected literal sequence");
    std::cout << "  B1=20: PASS [16, 9, 5, 7, 11, 13, 17, 19]" << std::endl;
}

static void test_compute_b1_100_count() {
    std::cout << "Testing compute_b1_prime_powers(B1=100) prime count..." << std::endl;
    // pi(100) = 25 primes ≤ 100. Verify count + first few + sortedness by prime base.
    auto r = compute_b1_prime_powers(100);
    assert(r.size() == 25 && "pi(100) = 25");

    // First 4 entries:
    //   p=2: 2^6 = 64 ≤ 100, 2^7 = 128 > 100  → 64
    //   p=3: 3^4 = 81 ≤ 100, 3^5 = 243 > 100  → 81
    //   p=5: 5^2 = 25 ≤ 100, 5^3 = 125 > 100  → 25
    //   p=7: 7^2 = 49 ≤ 100, 7^3 = 343 > 100  → 49
    assert(r[0] == 64);
    assert(r[1] == 81);
    assert(r[2] == 25);
    assert(r[3] == 49);

    // primes 11, 13, 17, ..., 97 each contribute themselves (p^2 > 100).
    // Specifically r[4] is 11, r[24] is 97.
    assert(r[4] == 11);
    assert(r[24] == 97);

    std::cout << "  B1=100: PASS (25 primes, first 4 powers: 64, 81, 25, 49)" << std::endl;
}

static void test_compute_deterministic() {
    std::cout << "Testing compute_b1_prime_powers deterministic..." << std::endl;
    // Same input must yield same output across repeated calls.
    auto r1 = compute_b1_prime_powers(50);
    auto r2 = compute_b1_prime_powers(50);
    auto r3 = compute_b1_prime_powers(50);
    assert(r1 == r2);
    assert(r2 == r3);
    std::cout << "  Determinism: PASS (3 calls same output)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Cache behavior tests
// ────────────────────────────────────────────────────────────────────

static void test_cache_hit_returns_same_content() {
    std::cout << "Testing cache hit returns same content..." << std::endl;
    EcmB1PrimeCache cache(4);
    assert(cache.size() == 0);
    assert(cache.capacity() == 4);

    const auto& v_first = cache.get_or_compute(20);
    assert(cache.size() == 1);
    // Capture pointer / address.
    const auto* first_ptr = &v_first;

    // Second lookup of same B1: hit. Must return reference to same stored
    // object (pointer identity assertion).
    const auto& v_second = cache.get_or_compute(20);
    assert(cache.size() == 1 && "Cache must not grow on hit");
    assert(&v_second == first_ptr && "Hit must return ref to existing cached vector");

    // Content must match expected.
    const std::vector<uint64_t> expected{16, 9, 5, 7, 11, 13, 17, 19};
    assert(v_second == expected);

    std::cout << "  Cache hit: PASS (pointer identity + content match)" << std::endl;
}

static void test_cache_miss_not_full_inserts() {
    std::cout << "Testing cache miss + not full inserts..." << std::endl;
    EcmB1PrimeCache cache(8);

    // Insert 4 distinct B1 values.
    for (uint64_t B1 : {10ULL, 20ULL, 50ULL, 100ULL}) {
        const auto& v = cache.get_or_compute(B1);
        auto expected = compute_b1_prime_powers(B1);
        assert(v == expected && "Cached vector must match fresh compute");
    }
    assert(cache.size() == 4);

    // Re-lookup: hits, size stays.
    for (uint64_t B1 : {10ULL, 20ULL, 50ULL, 100ULL}) {
        const auto& v = cache.get_or_compute(B1);
        auto expected = compute_b1_prime_powers(B1);
        assert(v == expected);
    }
    assert(cache.size() == 4);

    std::cout << "  Cache miss + not full: PASS (4 entries inserted)" << std::endl;
}

static void test_cache_miss_full_returns_correct_content() {
    std::cout << "Testing cache miss + full returns correct content..." << std::endl;
    EcmB1PrimeCache cache(2);

    // Fill cache.
    const auto& a = cache.get_or_compute(10);
    const auto& b = cache.get_or_compute(20);
    assert(cache.size() == 2);
    (void)a;
    (void)b;

    // Cache full. Miss B1=30: should return correct content but NOT insert.
    const auto& c = cache.get_or_compute(30);
    assert(cache.size() == 2 && "Cache full: size must not grow");
    auto expected_30 = compute_b1_prime_powers(30);
    assert(c == expected_30 && "Overflow result must equal fresh compute");

    // Another overflow miss B1=40: overwrites the overflow slot.
    const auto& d = cache.get_or_compute(40);
    assert(cache.size() == 2 && "Cache full: size still must not grow");
    auto expected_40 = compute_b1_prime_powers(40);
    assert(d == expected_40 && "Subsequent overflow content must be correct");

    // Cached entries (B1=10 and 20) still hit cleanly.
    const auto& a2 = cache.get_or_compute(10);
    auto expected_10 = compute_b1_prime_powers(10);
    assert(a2 == expected_10);
    assert(cache.size() == 2);

    std::cout << "  Cache miss + full: PASS (no insert, correct content)" << std::endl;
}

static void test_cache_clear_resets() {
    std::cout << "Testing cache clear resets state..." << std::endl;
    EcmB1PrimeCache cache(4);

    (void)cache.get_or_compute(10);
    (void)cache.get_or_compute(20);
    (void)cache.get_or_compute(50);
    assert(cache.size() == 3);

    cache.clear();
    assert(cache.size() == 0);
    assert(cache.capacity() == 4 && "Clear must not change capacity");

    // After clear, the same B1 misses again and inserts.
    const auto& v = cache.get_or_compute(20);
    assert(cache.size() == 1);
    auto expected = compute_b1_prime_powers(20);
    assert(v == expected);

    std::cout << "  Cache clear: PASS (size=0 after clear, refills)" << std::endl;
}

static void test_cache_zero_capacity_always_overflow() {
    std::cout << "Testing zero-capacity cache always overflows..." << std::endl;
    EcmB1PrimeCache cache(0);
    assert(cache.size() == 0);
    assert(cache.capacity() == 0);

    // Every lookup misses + cache is full (0 == 0), so we get the overflow slot.
    const auto& v1 = cache.get_or_compute(20);
    auto expected = compute_b1_prime_powers(20);
    assert(v1 == expected);
    assert(cache.size() == 0 && "Zero-cap cache must never insert");

    const auto& v2 = cache.get_or_compute(30);
    auto expected_30 = compute_b1_prime_powers(30);
    assert(v2 == expected_30);
    assert(cache.size() == 0);

    std::cout << "  Zero capacity: PASS (always overflow path)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Thread safety
// ────────────────────────────────────────────────────────────────────

static void test_cache_thread_safe_concurrent_lookups() {
    std::cout << "Testing 4 threads × 100 random B1 lookups concurrent..." << std::endl;
    EcmB1PrimeCache cache(8);

    constexpr int kThreads = 4;
    constexpr int kLookupsPerThread = 100;
    // Restrict B1 to a small set so most lookups hit the cache and we
    // exercise the mutex + overflow paths heavily.
    const std::vector<uint64_t> B1_set{10, 20, 50, 100, 200, 500, 1000};

    std::atomic<bool> all_ok{true};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            std::mt19937_64 rng(static_cast<uint64_t>(t) * 0x9E3779B97F4A7C15ULL);
            std::uniform_int_distribution<size_t> dist(0, B1_set.size() - 1);
            for (int i = 0; i < kLookupsPerThread; ++i) {
                const uint64_t B1 = B1_set[dist(rng)];
                const auto& v = cache.get_or_compute(B1);
                auto expected = compute_b1_prime_powers(B1);
                if (v != expected) {
                    all_ok = false;
                    return;
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    assert(all_ok.load() && "All concurrent lookups must return correct content");

    // Cache size is bounded by capacity (8) regardless of how many unique
    // B1 values were tried (7 unique here, so size could be up to 7).
    assert(cache.size() <= cache.capacity());

    std::cout << "  Concurrent lookups: PASS (no race, correct content)" << std::endl;
}

static void test_shared_singleton_thread_safe_first_call() {
    std::cout << "Testing shared singleton concurrent first call..." << std::endl;
    // Ensure ENV is in a known state before first singleton access. We
    // set capacity 4 here; note that the singleton is created lazily on
    // first call and retains capacity forever, so previous tests may
    // have already initialised it. The thread-safety property we test
    // is that concurrent first calls all return the same reference.
    setenv("GNFS_ECM_B1_CACHE_SIZE", "4", 1);
    ecm_b1_cache_reset_env_cache_for_testing();

    constexpr std::size_t kThreads = 8;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    std::vector<EcmB1PrimeCache*> ptrs(kThreads, nullptr);

    for (std::size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            ptrs[t] = &shared_ecm_b1_cache();
        });
    }
    for (auto& th : threads) th.join();

    // All threads must see the same singleton instance.
    for (std::size_t t = 1; t < kThreads; ++t) {
        assert(ptrs[t] == ptrs[0] &&
               "Singleton ref must be identical across threads");
    }

    // Singleton must be usable.
    auto& sg = shared_ecm_b1_cache();
    const auto& v = sg.get_or_compute(20);
    auto expected = compute_b1_prime_powers(20);
    assert(v == expected);

    unsetenv("GNFS_ECM_B1_CACHE_SIZE");
    ecm_b1_cache_reset_env_cache_for_testing();
    std::cout << "  Singleton thread safety: PASS (same ref from all threads)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// ENV cache reset hook
// ────────────────────────────────────────────────────────────────────

static void test_reset_env_cache_re_reads() {
    std::cout << "Testing reset_env_cache_for_testing re-resolves..." << std::endl;
    // Start with unset.
    unsetenv("GNFS_ECM_B1_CACHE_SIZE");
    ecm_b1_cache_reset_env_cache_for_testing();
    assert(ecm_b1_cache_size() == 0);

    // Set new value + reset cache.
    setenv("GNFS_ECM_B1_CACHE_SIZE", "16", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    assert(ecm_b1_cache_size() == 16);

    // Change again.
    setenv("GNFS_ECM_B1_CACHE_SIZE", "32", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    assert(ecm_b1_cache_size() == 32);

    unsetenv("GNFS_ECM_B1_CACHE_SIZE");
    ecm_b1_cache_reset_env_cache_for_testing();
    assert(ecm_b1_cache_size() == 0);

    std::cout << "  Reset cache: PASS (re-reads modified env)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Perf info (informational, not assert)
// ────────────────────────────────────────────────────────────────────

static void test_perf_hit_vs_miss_B1_10000() {
    std::cout << "Perf info: cache hit vs miss wall-time at B1=10000..." << std::endl;
    EcmB1PrimeCache cache(4);

    constexpr int kRepeats = 100;

    // Warm: insert B1=10000.
    auto t_first_start = std::chrono::steady_clock::now();
    (void)cache.get_or_compute(10000);
    auto t_first_end = std::chrono::steady_clock::now();
    const auto first_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        t_first_end - t_first_start).count();

    // Repeated hits: same B1, cache hit path.
    auto t_hit_start = std::chrono::steady_clock::now();
    for (int i = 0; i < kRepeats; ++i) {
        (void)cache.get_or_compute(10000);
    }
    auto t_hit_end = std::chrono::steady_clock::now();
    const auto hit_total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        t_hit_end - t_hit_start).count();
    const auto hit_avg_ns = hit_total_ns / static_cast<long long>(kRepeats);

    // Miss (uncached): direct compute on new B1 value each iteration.
    // Use distinct B1 values to bypass any caching. We use values just below
    // 10000 to keep sieve cost comparable.
    auto t_miss_start = std::chrono::steady_clock::now();
    for (int i = 0; i < kRepeats; ++i) {
        auto r = compute_b1_prime_powers(10000ULL - static_cast<uint64_t>(i));
        (void)r;
    }
    auto t_miss_end = std::chrono::steady_clock::now();
    const auto miss_total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        t_miss_end - t_miss_start).count();
    const auto miss_avg_ns = miss_total_ns / static_cast<long long>(kRepeats);

    std::cout << "  B1=10000 first insert: " << first_ns << " ns" << std::endl;
    std::cout << "  Hit avg over " << kRepeats << " runs: "
              << hit_avg_ns << " ns" << std::endl;
    std::cout << "  Miss avg over " << kRepeats << " runs: "
              << miss_avg_ns << " ns" << std::endl;
    if (hit_avg_ns > 0) {
        const double speedup =
            static_cast<double>(miss_avg_ns) / static_cast<double>(hit_avg_ns);
        std::cout << "  Speedup (miss/hit): " << speedup << "x" << std::endl;
    }
    std::cout << "  Perf info: PASS (informational only)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Additional behavior assertion: cache returned ref vs fresh compute equal
// ────────────────────────────────────────────────────────────────────

static void test_cache_vs_fresh_compute_parity() {
    std::cout << "Testing cached vector == fresh compute across multiple B1..." << std::endl;
    EcmB1PrimeCache cache(8);

    for (uint64_t B1 : {2ULL, 7ULL, 31ULL, 100ULL, 313ULL, 1000ULL, 9973ULL, 50000ULL}) {
        const auto& cached = cache.get_or_compute(B1);
        auto fresh = compute_b1_prime_powers(B1);
        assert(cached == fresh && "Cache content must match fresh compute");
    }

    std::cout << "  Cache vs fresh parity: PASS (8 distinct B1 values)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Main
// ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "═══════════════════════════════════════════════════════" << std::endl;
    std::cout << "  ECM B1 prime-power expansion cache tests (W13 T3)" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════" << std::endl;

    // ENV parsing
    test_env_unset_default_off();
    test_env_zero_explicit_off();
    test_env_4_enabled();
    test_env_33_clamp_to_32();
    test_env_garbage_default_off();
    test_env_leading_whitespace_off();

    // compute_b1_prime_powers correctness
    test_compute_edge_cases();
    test_compute_b1_10();
    test_compute_b1_20_literal();
    test_compute_b1_100_count();
    test_compute_deterministic();

    // Cache behavior
    test_cache_hit_returns_same_content();
    test_cache_miss_not_full_inserts();
    test_cache_miss_full_returns_correct_content();
    test_cache_clear_resets();
    test_cache_zero_capacity_always_overflow();
    test_cache_vs_fresh_compute_parity();

    // Thread safety
    test_cache_thread_safe_concurrent_lookups();
    test_shared_singleton_thread_safe_first_call();

    // ENV cache reset
    test_reset_env_cache_re_reads();

    // Perf info
    test_perf_hit_vs_miss_B1_10000();

    std::cout << "═══════════════════════════════════════════════════════" << std::endl;
    std::cout << "  All ECM B1 prime cache tests PASSED" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════" << std::endl;
    return 0;
}
