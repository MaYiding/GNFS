// Unit tests for include/gnfs/cofactor/ecm_prime_cache.hpp (W13 T3).
//
// Verifies:
//   * ENV parsing: unset, "0", "4", "33" (clamp to 32), "garbage",
//     leading whitespace " 4", empty
//   * compute_b1_prime_powers correctness for B1 ∈ {0, 1, 2, 3, 10, 20, 100}
//   * Literal expected sequence for B1=20: [16, 9, 5, 7, 11, 13, 17, 19]
//   * Cache hit: second lookup shares the same immutable vector
//   * Cache miss + not full: inserts and returns an owning handle
//   * Cache miss + full or zero capacity: independent retained results
//   * cache.size() / cache.capacity() / cache.clear() behavior
//   * Retained handles survive other-thread misses, clear, and destruction
//   * Concurrent unique keys exceed capacity and match literal content oracles
//   * Shared singleton thread-safety (concurrent first call)
//   * reset_env_cache_for_testing lets next env query re-resolve
//   * Perf info: cache hit vs miss wall-time for B1=10000

#include "gnfs/cofactor/ecm_prime_cache.hpp"
#include "support/test_check.hpp"

#include <array>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
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
    GNFS_TEST_CHECK(ecm_b1_cache_size() == 0);
    GNFS_TEST_CHECK(ecm_b1_cache_enabled() == false);
    std::cout << "  ENV unset: PASS (size=0, disabled)" << std::endl;
}

static void test_env_zero_explicit_off() {
    std::cout << "Testing GNFS_ECM_B1_CACHE_SIZE=0 => 0..." << std::endl;
    setenv("GNFS_ECM_B1_CACHE_SIZE", "0", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(ecm_b1_cache_size() == 0);
    GNFS_TEST_CHECK(ecm_b1_cache_enabled() == false);
    unsetenv("GNFS_ECM_B1_CACHE_SIZE");
    ecm_b1_cache_reset_env_cache_for_testing();
    std::cout << "  ENV=0: PASS (size=0, disabled)" << std::endl;
}

static void test_env_4_enabled() {
    std::cout << "Testing GNFS_ECM_B1_CACHE_SIZE=4 => 4..." << std::endl;
    setenv("GNFS_ECM_B1_CACHE_SIZE", "4", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(ecm_b1_cache_size() == 4);
    GNFS_TEST_CHECK(ecm_b1_cache_enabled() == true);
    unsetenv("GNFS_ECM_B1_CACHE_SIZE");
    ecm_b1_cache_reset_env_cache_for_testing();
    std::cout << "  ENV=4: PASS (size=4, enabled)" << std::endl;
}

static void test_env_33_clamp_to_32() {
    std::cout << "Testing GNFS_ECM_B1_CACHE_SIZE=33 => 32 (clamp)..." << std::endl;
    setenv("GNFS_ECM_B1_CACHE_SIZE", "33", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(ecm_b1_cache_size() == 32 && "33 must clamp to 32");
    GNFS_TEST_CHECK(ecm_b1_cache_enabled() == true);

    // Extreme out-of-range still clamps to 32.
    setenv("GNFS_ECM_B1_CACHE_SIZE", "999999", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(ecm_b1_cache_size() == 32);

    unsetenv("GNFS_ECM_B1_CACHE_SIZE");
    ecm_b1_cache_reset_env_cache_for_testing();
    std::cout << "  ENV=33: PASS (clamped to 32)" << std::endl;
}

static void test_env_huge_value_clamps_to_32() {
    std::cout << "Testing GNFS_ECM_B1_CACHE_SIZE huge value clamps..." << std::endl;
    // Values above INT_MAX are still valid positive inputs under the documented
    // clamp rule; they must not be mistaken for an invalid/disabled setting.
    for (const char* value : {"2147483648", "999999999999999999999999999"}) {
        setenv("GNFS_ECM_B1_CACHE_SIZE", value, 1);
        ecm_b1_cache_reset_env_cache_for_testing();
        GNFS_TEST_CHECK(ecm_b1_cache_size() == 32);
    }
    GNFS_TEST_CHECK(ecm_b1_cache_enabled());
    std::cout << "  huge value clamps to 32: PASS" << std::endl;
}

static void test_env_garbage_default_off() {
    std::cout << "Testing GNFS_ECM_B1_CACHE_SIZE=garbage => 0..." << std::endl;
    setenv("GNFS_ECM_B1_CACHE_SIZE", "garbage", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(ecm_b1_cache_size() == 0);

    setenv("GNFS_ECM_B1_CACHE_SIZE", "-5", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(ecm_b1_cache_size() == 0);

    // Empty string → unset-like (returns 0).
    setenv("GNFS_ECM_B1_CACHE_SIZE", "", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(ecm_b1_cache_size() == 0);

    unsetenv("GNFS_ECM_B1_CACHE_SIZE");
    ecm_b1_cache_reset_env_cache_for_testing();
    std::cout << "  ENV garbage / negative / empty: PASS (all disabled)" << std::endl;
}

static void test_env_leading_whitespace_off() {
    std::cout << "Testing GNFS_ECM_B1_CACHE_SIZE='  4' (leading whitespace) => 0..." << std::endl;
    // Leading whitespace explicitly disabled by strict parser.
    setenv("GNFS_ECM_B1_CACHE_SIZE", "  4", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(ecm_b1_cache_size() == 0 &&
                    "Leading whitespace must be rejected by strict parser");

    setenv("GNFS_ECM_B1_CACHE_SIZE", "\t8", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(ecm_b1_cache_size() == 0 && "Leading tab must be rejected");

    setenv("GNFS_ECM_B1_CACHE_SIZE", "\n2", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(ecm_b1_cache_size() == 0 && "Leading newline must be rejected");

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
    GNFS_TEST_CHECK(r0.empty() && "B1=0 must return empty");

    // B1 = 1 → no primes
    auto r1 = compute_b1_prime_powers(1);
    GNFS_TEST_CHECK(r1.empty() && "B1=1 must return empty");

    // B1 = 2 → primes {2}, prime powers {2} (2^1 = 2 ≤ 2, 2^2 = 4 > 2)
    auto r2 = compute_b1_prime_powers(2);
    GNFS_TEST_CHECK(r2.size() == 1);
    GNFS_TEST_CHECK(r2[0] == 2);

    // B1 = 3 → primes {2, 3}, prime powers {2, 3} (2^2 = 4 > 3, 3^2 = 9 > 3)
    auto r3 = compute_b1_prime_powers(3);
    GNFS_TEST_CHECK(r3.size() == 2);
    GNFS_TEST_CHECK(r3[0] == 2);
    GNFS_TEST_CHECK(r3[1] == 3);

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
    GNFS_TEST_CHECK(r.size() == 4);
    GNFS_TEST_CHECK(r[0] == 8);
    GNFS_TEST_CHECK(r[1] == 9);
    GNFS_TEST_CHECK(r[2] == 5);
    GNFS_TEST_CHECK(r[3] == 7);
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
    GNFS_TEST_CHECK(r == expected && "B1=20 must match expected literal sequence");
    std::cout << "  B1=20: PASS [16, 9, 5, 7, 11, 13, 17, 19]" << std::endl;
}

static void test_compute_b1_100_count() {
    std::cout << "Testing compute_b1_prime_powers(B1=100) prime count..." << std::endl;
    // pi(100) = 25 primes ≤ 100. Verify count + first few + sortedness by prime base.
    auto r = compute_b1_prime_powers(100);
    GNFS_TEST_CHECK(r.size() == 25 && "pi(100) = 25");

    // First 4 entries:
    //   p=2: 2^6 = 64 ≤ 100, 2^7 = 128 > 100  → 64
    //   p=3: 3^4 = 81 ≤ 100, 3^5 = 243 > 100  → 81
    //   p=5: 5^2 = 25 ≤ 100, 5^3 = 125 > 100  → 25
    //   p=7: 7^2 = 49 ≤ 100, 7^3 = 343 > 100  → 49
    GNFS_TEST_CHECK(r[0] == 64);
    GNFS_TEST_CHECK(r[1] == 81);
    GNFS_TEST_CHECK(r[2] == 25);
    GNFS_TEST_CHECK(r[3] == 49);

    // primes 11, 13, 17, ..., 97 each contribute themselves (p^2 > 100).
    // Specifically r[4] is 11, r[24] is 97.
    GNFS_TEST_CHECK(r[4] == 11);
    GNFS_TEST_CHECK(r[24] == 97);

    std::cout << "  B1=100: PASS (25 primes, first 4 powers: 64, 81, 25, 49)" << std::endl;
}

static void test_compute_deterministic() {
    std::cout << "Testing compute_b1_prime_powers deterministic..." << std::endl;
    // Same input must yield same output across repeated calls.
    auto r1 = compute_b1_prime_powers(50);
    auto r2 = compute_b1_prime_powers(50);
    auto r3 = compute_b1_prime_powers(50);
    GNFS_TEST_CHECK(r1 == r2);
    GNFS_TEST_CHECK(r2 == r3);
    std::cout << "  Determinism: PASS (3 calls same output)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Cache behavior tests
// ────────────────────────────────────────────────────────────────────

using PrimePowersHandle = std::shared_ptr<const std::vector<uint64_t>>;

static void test_cache_hit_returns_same_content() {
    std::cout << "Testing cache hit shares the same immutable vector..." << std::endl;
    EcmB1PrimeCache cache(4);
    GNFS_TEST_CHECK(cache.size() == 0);
    GNFS_TEST_CHECK(cache.capacity() == 4);

    const auto first = cache.get_or_compute(20);
    GNFS_TEST_CHECK(first);
    GNFS_TEST_CHECK(cache.size() == 1);

    const auto second = cache.get_or_compute(20);
    GNFS_TEST_CHECK(second);
    GNFS_TEST_CHECK(cache.size() == 1 && "Cache must not grow on hit");
    GNFS_TEST_CHECK(first.get() == second.get() && "Hits must share the cached vector");

    const std::vector<uint64_t> expected{16, 9, 5, 7, 11, 13, 17, 19};
    GNFS_TEST_CHECK(*first == expected);
    GNFS_TEST_CHECK(*second == expected);
    std::cout << "  Cache hit: PASS (shared identity + content match)" << std::endl;
}

static void test_cache_miss_not_full_inserts() {
    std::cout << "Testing cache miss + not full inserts..." << std::endl;
    EcmB1PrimeCache cache(8);

    for (uint64_t bound : {10ULL, 20ULL, 50ULL, 100ULL}) {
        const auto result = cache.get_or_compute(bound);
        const auto expected = compute_b1_prime_powers(bound);
        GNFS_TEST_CHECK(result);
        GNFS_TEST_CHECK(*result == expected && "Cached vector must match fresh compute");
    }
    GNFS_TEST_CHECK(cache.size() == 4);

    for (uint64_t bound : {10ULL, 20ULL, 50ULL, 100ULL}) {
        const auto result = cache.get_or_compute(bound);
        const auto expected = compute_b1_prime_powers(bound);
        GNFS_TEST_CHECK(result);
        GNFS_TEST_CHECK(*result == expected);
    }
    GNFS_TEST_CHECK(cache.size() == 4);
    std::cout << "  Cache miss + not full: PASS (4 entries inserted)" << std::endl;
}

static void test_cache_miss_full_preserves_retained_results() {
    std::cout << "Testing full-cache misses preserve all retained results..." << std::endl;
    EcmB1PrimeCache cache(2);
    const auto cached_10 = cache.get_or_compute(10);
    const auto cached_20 = cache.get_or_compute(20);
    const auto missed_30 = cache.get_or_compute(30);
    const auto missed_40 = cache.get_or_compute(40);
    const auto missed_30_again = cache.get_or_compute(30);
    const auto cached_10_again = cache.get_or_compute(10);

    const std::vector<uint64_t> expected_10{8, 9, 5, 7};
    const std::vector<uint64_t> expected_20{16, 9, 5, 7, 11, 13, 17, 19};
    const std::vector<uint64_t> expected_30{16, 27, 25, 7, 11, 13, 17, 19, 23, 29};
    const std::vector<uint64_t> expected_40{32, 27, 25, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    GNFS_TEST_CHECK(cached_10 && cached_20 && missed_30 && missed_40 && missed_30_again &&
                    cached_10_again);
    GNFS_TEST_CHECK(*cached_10 == expected_10);
    GNFS_TEST_CHECK(*cached_20 == expected_20);
    GNFS_TEST_CHECK(*missed_30 == expected_30);
    GNFS_TEST_CHECK(*missed_40 == expected_40);
    GNFS_TEST_CHECK(*missed_30_again == expected_30);
    GNFS_TEST_CHECK(cached_10.get() == cached_10_again.get());
    GNFS_TEST_CHECK(cache.size() == 2 && "Misses must not exceed the hard capacity");
    std::cout << "  Full cache: PASS (all handles retain their original content)" << std::endl;
}

static void test_cache_clear_preserves_retained_results() {
    std::cout << "Testing clear preserves cached and uncached handles..." << std::endl;
    EcmB1PrimeCache cache(1);
    auto cached = cache.get_or_compute(10);
    auto uncached = cache.get_or_compute(20);
    const std::weak_ptr<const std::vector<uint64_t>> cached_observer = cached;
    const std::weak_ptr<const std::vector<uint64_t>> uncached_observer = uncached;
    GNFS_TEST_CHECK(cache.size() == 1);

    cache.clear();
    GNFS_TEST_CHECK(cache.size() == 0);
    GNFS_TEST_CHECK(cache.capacity() == 1 && "Clear must not change capacity");
    const auto refilled = cache.get_or_compute(30);
    GNFS_TEST_CHECK(cache.size() == 1);

    const std::vector<uint64_t> expected_10{8, 9, 5, 7};
    const std::vector<uint64_t> expected_20{16, 9, 5, 7, 11, 13, 17, 19};
    const std::vector<uint64_t> expected_30{16, 27, 25, 7, 11, 13, 17, 19, 23, 29};
    GNFS_TEST_CHECK(cached && uncached && refilled);
    GNFS_TEST_CHECK(*cached == expected_10);
    GNFS_TEST_CHECK(*uncached == expected_20);
    GNFS_TEST_CHECK(*refilled == expected_30);
    GNFS_TEST_CHECK(!cached_observer.expired() && !uncached_observer.expired());
    cached.reset();
    uncached.reset();
    GNFS_TEST_CHECK(cached_observer.expired());
    GNFS_TEST_CHECK(uncached_observer.expired());
    std::cout << "  Clear: PASS (empty cache, retained content, refill)" << std::endl;
}

static void test_cache_zero_capacity_preserves_retained_results() {
    std::cout << "Testing zero-capacity results have independent lifetimes..." << std::endl;
    EcmB1PrimeCache cache(0);
    const auto empty_zero = cache.get_or_compute(0);
    const auto empty_one = cache.get_or_compute(1);
    const auto first = cache.get_or_compute(20);
    const auto second = cache.get_or_compute(30);
    const auto third = cache.get_or_compute(2);

    const std::vector<uint64_t> expected_20{16, 9, 5, 7, 11, 13, 17, 19};
    const std::vector<uint64_t> expected_30{16, 27, 25, 7, 11, 13, 17, 19, 23, 29};
    const std::vector<uint64_t> expected_2{2};
    GNFS_TEST_CHECK(empty_zero && empty_one && first && second && third);
    GNFS_TEST_CHECK(empty_zero->empty());
    GNFS_TEST_CHECK(empty_one->empty());
    GNFS_TEST_CHECK(*first == expected_20);
    GNFS_TEST_CHECK(*second == expected_30);
    GNFS_TEST_CHECK(*third == expected_2);
    GNFS_TEST_CHECK(cache.size() == 0 && "Zero-capacity cache must never insert");
    GNFS_TEST_CHECK(cache.capacity() == 0);
    std::cout << "  Zero capacity: PASS (all prior handles remain valid)" << std::endl;
}

static void test_invalid_bound_preserves_results_and_cache() {
    std::cout << "Testing a rejected bound leaves handles and cache usable..." << std::endl;
    const std::vector<uint64_t> expected_20{16, 9, 5, 7, 11, 13, 17, 19};
    const std::vector<uint64_t> expected_30{16, 27, 25, 7, 11, 13, 17, 19, 23, 29};
    for (std::size_t capacity : {std::size_t{0}, std::size_t{1}}) {
        EcmB1PrimeCache cache(capacity);
        const auto retained = cache.get_or_compute(20);
        bool rejected = false;
        try {
            (void)cache.get_or_compute(100'000'001);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        GNFS_TEST_CHECK(rejected);
        GNFS_TEST_CHECK(cache.size() == capacity);
        GNFS_TEST_CHECK(retained);
        GNFS_TEST_CHECK(*retained == expected_20);
        // A successful follow-up also proves the throwing lookup released its lock.
        const auto next = cache.get_or_compute(30);
        GNFS_TEST_CHECK(next);
        GNFS_TEST_CHECK(*next == expected_30);
        GNFS_TEST_CHECK(*retained == expected_20);
        GNFS_TEST_CHECK(cache.size() == capacity);
    }
    std::cout << "  Rejected bound: PASS (prior results, capacity, and subsequent lookup)"
              << std::endl;
}

static void test_results_outlive_cache() {
    std::cout << "Testing cached and uncached handles outlive the cache..." << std::endl;
    PrimePowersHandle cached;
    PrimePowersHandle uncached;
    {
        EcmB1PrimeCache cache(1);
        cached = cache.get_or_compute(10);
        uncached = cache.get_or_compute(20);
    }
    const std::vector<uint64_t> expected_10{8, 9, 5, 7};
    const std::vector<uint64_t> expected_20{16, 9, 5, 7, 11, 13, 17, 19};
    GNFS_TEST_CHECK(cached && uncached);
    GNFS_TEST_CHECK(*cached == expected_10);
    GNFS_TEST_CHECK(*uncached == expected_20);
    std::cout << "  Cache destruction: PASS (both retained results remain valid)" << std::endl;
}

// The barriers impose the faulty interleaving without a scheduling race:
// retain B1=20, finish another thread's misses, then read the retained value.
static void test_retained_result_survives_other_thread_misses() {
    std::cout << "Testing retained handles across another thread's misses..." << std::endl;
    const std::vector<uint64_t> expected_20{16, 9, 5, 7, 11, 13, 17, 19};
    const std::vector<uint64_t> expected_30{16, 27, 25, 7, 11, 13, 17, 19, 23, 29};
    const std::vector<uint64_t> expected_40{32, 27, 25, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    for (std::size_t capacity : {std::size_t{0}, std::size_t{1}}) {
        EcmB1PrimeCache cache(capacity);
        const auto initial = cache.get_or_compute(10);
        const auto retained = cache.get_or_compute(20);
        PrimePowersHandle missed_30;
        PrimePowersHandle missed_40;
        std::barrier checkpoint(2);
        std::thread other([&] {
            checkpoint.arrive_and_wait();
            missed_30 = cache.get_or_compute(30);
            missed_40 = cache.get_or_compute(40);
            checkpoint.arrive_and_wait();
        });
        checkpoint.arrive_and_wait();
        checkpoint.arrive_and_wait();
        other.join();

        GNFS_TEST_CHECK(initial && retained && missed_30 && missed_40);
        GNFS_TEST_CHECK(*retained == expected_20);
        GNFS_TEST_CHECK(*missed_30 == expected_30);
        GNFS_TEST_CHECK(*missed_40 == expected_40);
        GNFS_TEST_CHECK(cache.size() == capacity);
    }
    std::cout << "  Other-thread misses: PASS (deterministic retained-content checks)" << std::endl;
}

// Each vector is a literal mathematical oracle in ascending prime-base order.
// In particular, bounds at 8, 9, 16, 25, 27, 32, and 49 cross power boundaries.
static void test_cache_thread_safe_concurrent_lookups() {
    std::cout << "Testing 4 threads with 16 unique bounds and capacity 2..." << std::endl;
    struct PrimePowerCase {
        uint64_t bound;
        std::vector<uint64_t> expected;
    };
    constexpr std::size_t thread_count = 4;
    constexpr std::size_t rounds = 4;
    constexpr std::size_t case_count = thread_count * rounds;
    const std::array<PrimePowerCase, case_count> cases{{
        {2, {2}},
        {3, {2, 3}},
        {4, {4, 3}},
        {5, {4, 3, 5}},
        {7, {4, 3, 5, 7}},
        {8, {8, 3, 5, 7}},
        {9, {8, 9, 5, 7}},
        {10, {8, 9, 5, 7}},
        {16, {16, 9, 5, 7, 11, 13}},
        {20, {16, 9, 5, 7, 11, 13, 17, 19}},
        {25, {16, 9, 25, 7, 11, 13, 17, 19, 23}},
        {27, {16, 27, 25, 7, 11, 13, 17, 19, 23}},
        {30, {16, 27, 25, 7, 11, 13, 17, 19, 23, 29}},
        {32, {32, 27, 25, 7, 11, 13, 17, 19, 23, 29, 31}},
        {40, {32, 27, 25, 7, 11, 13, 17, 19, 23, 29, 31, 37}},
        {49, {32, 27, 25, 49, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47}},
    }};
    EcmB1PrimeCache cache(2);
    std::array<PrimePowersHandle, case_count> retained;
    std::array<std::size_t, case_count> observed_sizes{};
    std::barrier checkpoint(static_cast<std::ptrdiff_t>(thread_count));
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            for (std::size_t round = 0; round < rounds; ++round) {
                const std::size_t index = round * thread_count + t;
                checkpoint.arrive_and_wait();
                retained[index] = cache.get_or_compute(cases[index].bound);
                observed_sizes[index] = cache.size();
                checkpoint.arrive_and_wait();
            }
        });
    }
    for (auto& thread : threads)
        thread.join();

    for (std::size_t index = 0; index < cases.size(); ++index) {
        GNFS_TEST_CHECK(retained[index]);
        GNFS_TEST_CHECK(*retained[index] == cases[index].expected);
        GNFS_TEST_CHECK(observed_sizes[index] <= cache.capacity());
    }
    GNFS_TEST_CHECK(cache.size() == 2 && "Unique concurrent misses must respect capacity");
    std::cout << "  Concurrent lookups: PASS (all retained literal oracles, hard capacity)"
              << std::endl;
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
        threads.emplace_back([&, t]() { ptrs[t] = &shared_ecm_b1_cache(); });
    }
    for (auto& th : threads)
        th.join();

    // All threads must see the same singleton instance.
    for (std::size_t t = 1; t < kThreads; ++t) {
        GNFS_TEST_CHECK(ptrs[t] == ptrs[0] && "Singleton ref must be identical across threads");
    }

    // Singleton must be usable.
    auto& sg = shared_ecm_b1_cache();
    const auto v = sg.get_or_compute(20);
    auto expected = compute_b1_prime_powers(20);
    GNFS_TEST_CHECK(v);
    GNFS_TEST_CHECK(*v == expected);

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
    GNFS_TEST_CHECK(ecm_b1_cache_size() == 0);

    // Set new value + reset cache.
    setenv("GNFS_ECM_B1_CACHE_SIZE", "16", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(ecm_b1_cache_size() == 16);

    // Change again.
    setenv("GNFS_ECM_B1_CACHE_SIZE", "32", 1);
    ecm_b1_cache_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(ecm_b1_cache_size() == 32);

    unsetenv("GNFS_ECM_B1_CACHE_SIZE");
    ecm_b1_cache_reset_env_cache_for_testing();
    GNFS_TEST_CHECK(ecm_b1_cache_size() == 0);

    std::cout << "  Reset cache: PASS (re-reads modified env)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Perf info (informational only)
// ────────────────────────────────────────────────────────────────────

static void test_perf_hit_vs_miss_B1_10000() {
    std::cout << "Perf info: cache hit vs miss wall-time at B1=10000..." << std::endl;
    EcmB1PrimeCache cache(4);

    constexpr int kRepeats = 100;

    // Warm: insert B1=10000.
    auto t_first_start = std::chrono::steady_clock::now();
    const auto first = cache.get_or_compute(10000);
    auto t_first_end = std::chrono::steady_clock::now();
    const auto first_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t_first_end - t_first_start).count();

    GNFS_TEST_CHECK(first);

    // Repeated hits: same B1, cache hit path.
    PrimePowersHandle last_hit;
    auto t_hit_start = std::chrono::steady_clock::now();
    for (int i = 0; i < kRepeats; ++i) {
        last_hit = cache.get_or_compute(10000);
    }
    auto t_hit_end = std::chrono::steady_clock::now();
    const auto hit_total_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t_hit_end - t_hit_start).count();
    const auto hit_avg_ns = hit_total_ns / static_cast<long long>(kRepeats);
    GNFS_TEST_CHECK(last_hit && last_hit.get() == first.get());

    // Miss (uncached): direct compute on new B1 value each iteration.
    // Use distinct B1 values to bypass any caching. We use values just below
    // 10000 to keep sieve cost comparable.
    auto t_miss_start = std::chrono::steady_clock::now();
    for (int i = 0; i < kRepeats; ++i) {
        auto r = compute_b1_prime_powers(10000ULL - static_cast<uint64_t>(i));
        (void)r;
    }
    auto t_miss_end = std::chrono::steady_clock::now();
    const auto miss_total_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t_miss_end - t_miss_start).count();
    const auto miss_avg_ns = miss_total_ns / static_cast<long long>(kRepeats);

    std::cout << "  B1=10000 first insert: " << first_ns << " ns" << std::endl;
    std::cout << "  Hit avg over " << kRepeats << " runs: " << hit_avg_ns << " ns" << std::endl;
    std::cout << "  Miss avg over " << kRepeats << " runs: " << miss_avg_ns << " ns" << std::endl;
    if (hit_avg_ns > 0) {
        const double speedup = static_cast<double>(miss_avg_ns) / static_cast<double>(hit_avg_ns);
        std::cout << "  Speedup (miss/hit): " << speedup << "x" << std::endl;
    }
    std::cout << "  Perf info: PASS (informational only)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Additional behavior check: owned cache result vs fresh compute equal
// ────────────────────────────────────────────────────────────────────

static void test_cache_vs_fresh_compute_parity() {
    std::cout << "Testing cached vector == fresh compute across multiple B1..." << std::endl;
    EcmB1PrimeCache cache(8);

    for (uint64_t B1 : {2ULL, 7ULL, 31ULL, 100ULL, 313ULL, 1000ULL, 9973ULL, 50000ULL}) {
        const auto cached = cache.get_or_compute(B1);
        auto fresh = compute_b1_prime_powers(B1);
        GNFS_TEST_CHECK(cached);
        GNFS_TEST_CHECK(*cached == fresh && "Cache content must match fresh compute");
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
    test_env_huge_value_clamps_to_32();
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
    test_cache_miss_full_preserves_retained_results();
    test_cache_clear_preserves_retained_results();
    test_cache_zero_capacity_preserves_retained_results();
    test_results_outlive_cache();
    test_invalid_bound_preserves_results_and_cache();
    test_cache_vs_fresh_compute_parity();

    // Thread safety
    test_retained_result_survives_other_thread_misses();
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
