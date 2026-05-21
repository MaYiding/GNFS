// test_brent_pollard_rho.cpp — Brent-Pollard rho correctness + concurrency.

#include <gnfs/cofactor/brent_pollard_rho.hpp>
#include <gnfs/core/integer.hpp>

#include <atomic>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using gnfs::cofactor::BrentPollardRho;
using gnfs::core::Integer;

static int tests_passed = 0;
static int tests_failed = 0;
static std::mutex io_mutex;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; \
        ++tests_failed; \
        return; \
    } \
} while (0)

#define TEST_PASS(name) do { \
    std::cout << "  PASS: " << name << "\n"; \
    ++tests_passed; \
} while (0)

// Helper — verify split satisfies all invariants.
static bool valid_split(const Integer& n, const Integer& p, const Integer& q) {
    Integer one(static_cast<uint64_t>(1));
    if (!(p.compare(one) > 0 && p.compare(n) < 0)) return false;
    if (!(q.compare(one) > 0 && q.compare(n) < 0)) return false;
    Integer prod = p * q;
    return prod.compare(n) == 0;
}

static Integer make_int(uint64_t v) { return Integer(v); }

void test_trivial_semiprime() {
    // 35 = 5*7 — smallest non-trivial composite where rho should work.
    Integer n = make_int(35);
    auto result = BrentPollardRho::split(n, 1ULL << 16, 1);
    TEST_ASSERT(result.has_value(), "35 = 5*7 should split");
    TEST_ASSERT(valid_split(n, result->first, result->second), "valid split");
    TEST_PASS("trivial semiprime 35 = 5*7");
}

void test_balanced_50bit_semiprime() {
    // ~25-bit primes: 33554467 * 33554497 = ~50 bits.
    // 33554467 and 33554497 are both prime.
    uint64_t p = 33554467ULL;
    uint64_t q = 33554497ULL;
    Integer n(p * q);
    auto result = BrentPollardRho::split(n, 1ULL << 20, 1);
    TEST_ASSERT(result.has_value(), "balanced 50-bit semiprime should split");
    TEST_ASSERT(valid_split(n, result->first, result->second), "valid split");
    TEST_PASS("balanced ~50-bit semiprime");
}

void test_unbalanced_factor() {
    // 2^20 * 1000000007 (prime ~30 bits). One factor much smaller.
    uint64_t small = 1ULL << 20;
    uint64_t large = 1000000007ULL;
    Integer n(small * large);
    auto result = BrentPollardRho::split(n, 1ULL << 20, 1);
    TEST_ASSERT(result.has_value(), "unbalanced semiprime should split");
    TEST_ASSERT(valid_split(n, result->first, result->second), "valid split");
    TEST_PASS("unbalanced 2^20 * ~30-bit prime");
}

void test_carmichael_561() {
    // 561 = 3*11*17 — smallest Carmichael. Rho should find any pair.
    Integer n = make_int(561);
    auto result = BrentPollardRho::split(n, 1ULL << 16, 1);
    TEST_ASSERT(result.has_value(), "561 = 3*11*17 should split");
    TEST_ASSERT(valid_split(n, result->first, result->second), "valid split");
    TEST_PASS("Carmichael 561 = 3*11*17");
}

void test_near_square() {
    // p * (p+2) — twin primes 1000003 and 1000033 (both prime).
    // Actually use a real twin pair: 1000003 (prime) * 1000033 (prime).
    // Wait, those aren't twins (diff=30). Use 41 * 43.
    uint64_t p1 = 1000003ULL;
    uint64_t p2 = 1000033ULL;  // not twin, but two close primes.
    Integer n(p1 * p2);
    auto result = BrentPollardRho::split(n, 1ULL << 20, 1);
    TEST_ASSERT(result.has_value(), "near-square semiprime should split");
    TEST_ASSERT(valid_split(n, result->first, result->second), "valid split");
    TEST_PASS("near-square semiprime");
}

void test_twin_prime_product() {
    // Twin primes 1000037 and 1000039 — actually not twin (diff 2 between
    // 1000037 and 1000039, yes). Both happen to be prime? Let's use 11*13.
    // For a real twin pair > 1000: 1000037 is not prime actually.
    // Use known twin primes: 4799 and 4801 (both prime).
    uint64_t p1 = 4799ULL;
    uint64_t p2 = 4801ULL;
    Integer n(p1 * p2);
    auto result = BrentPollardRho::split(n, 1ULL << 18, 1);
    TEST_ASSERT(result.has_value(), "twin-prime product should split");
    TEST_ASSERT(valid_split(n, result->first, result->second), "valid split");
    TEST_PASS("twin-prime product 4799 * 4801");
}

void test_prime_input() {
    // 1000003 is prime — must return nullopt.
    Integer n = make_int(1000003ULL);
    auto result = BrentPollardRho::split(n, 1ULL << 16, 1);
    TEST_ASSERT(!result.has_value(),
                "prime input must NOT yield a split");
    TEST_PASS("prime input returns nullopt");
}

void test_perfect_power_p_squared() {
    // n = p^2 — Pollard rho cycles. Must return nullopt.
    uint64_t p = 1000003ULL;
    Integer n(p * p);
    auto result = BrentPollardRho::split(n, 1ULL << 18, 1);
    TEST_ASSERT(!result.has_value(),
                "perfect square (p^2) input must NOT split");
    TEST_PASS("perfect-power p^2 returns nullopt");
}

void test_perfect_power_p_cubed() {
    // n = p^3 — same as above.
    uint64_t p = 101ULL;
    Integer n(p * p * p);
    auto result = BrentPollardRho::split(n, 1ULL << 18, 1);
    TEST_ASSERT(!result.has_value(),
                "perfect-power (p^3) input must NOT split");
    TEST_PASS("perfect-power p^3 returns nullopt");
}

void test_max_iter_budget_respected() {
    // Use a hard ~60-bit semiprime; max_iter=1 should not split.
    uint64_t p = 1000000007ULL;
    uint64_t q = 1000000009ULL;
    Integer n(p * q);
    auto result = BrentPollardRho::split(n, 1, 1);
    TEST_ASSERT(!result.has_value(),
                "max_iter=1 must respect budget and return nullopt");
    TEST_PASS("max_iter=1 budget respected");
}

void test_determinism_same_seed() {
    // Same seed → same split.
    uint64_t p = 33554467ULL, q = 33554497ULL;
    Integer n(p * q);
    auto r1 = BrentPollardRho::split(n, 1ULL << 20, 42);
    auto r2 = BrentPollardRho::split(n, 1ULL << 20, 42);
    TEST_ASSERT(r1.has_value() && r2.has_value(),
                "both calls must succeed");
    TEST_ASSERT(r1->first.compare(r2->first) == 0
                && r1->second.compare(r2->second) == 0,
                "same seed must yield same factor pair");
    TEST_PASS("determinism: same seed yields same factor");
}

void test_different_seed_retry() {
    // Show that different seeds may yield same factor or both succeed.
    // We don't require the *first* seed to fail (rho usually succeeds on
    // c=1 for balanced semiprimes), but seed=2 must also succeed.
    uint64_t p = 33554467ULL, q = 33554497ULL;
    Integer n(p * q);
    auto r1 = BrentPollardRho::split(n, 1ULL << 20, 1);
    auto r2 = BrentPollardRho::split(n, 1ULL << 20, 2);
    TEST_ASSERT(r1.has_value() && r2.has_value(),
                "both seeds must yield valid splits");
    TEST_ASSERT(valid_split(n, r1->first, r1->second), "seed 1 valid");
    TEST_ASSERT(valid_split(n, r2->first, r2->second), "seed 2 valid");
    TEST_PASS("seed 1 and seed 2 both yield valid splits");
}

void test_integer_slow_path_large() {
    // > 64 bits — force Integer path. 80-bit semiprime: p * q
    // where p, q are ~40-bit. We need p, q prime.
    // p = 1099511628211 (prime, 40 bits), q = 1099511628401 (prime, 40 bits)
    // Then n ~ 80 bits.
    Integer p("1099511628211", 10);
    Integer q("1099511628401", 10);
    Integer n = p * q;
    // Verify n actually exceeds uint64 (>= 2^64).
    Integer u64_max("18446744073709551616", 10);  // 2^64
    TEST_ASSERT(n.compare(u64_max) > 0, "n must exceed uint64 range");
    auto result = BrentPollardRho::split(n, 1ULL << 22, 1);
    TEST_ASSERT(result.has_value(),
                "80-bit semiprime should split via Integer path");
    TEST_ASSERT(valid_split(n, result->first, result->second),
                "valid 80-bit split");
    TEST_PASS("Integer slow path 80-bit semiprime");
}

void test_too_small_n() {
    // n < 4 must return nullopt (no non-trivial split).
    Integer n2 = make_int(2);
    Integer n3 = make_int(3);
    auto r2 = BrentPollardRho::split(n2);
    auto r3 = BrentPollardRho::split(n3);
    TEST_ASSERT(!r2.has_value(), "n=2 returns nullopt");
    TEST_ASSERT(!r3.has_value(), "n=3 returns nullopt");
    TEST_PASS("n < 4 returns nullopt");
}

void test_even_n() {
    // n=12 = 2*6 — even fast path returns (2, 6).
    Integer n = make_int(12);
    auto result = BrentPollardRho::split(n);
    TEST_ASSERT(result.has_value(), "even n must split");
    TEST_ASSERT(valid_split(n, result->first, result->second), "valid split");
    // First factor should be 2.
    Integer two = make_int(2);
    TEST_ASSERT(result->first.compare(two) == 0,
                "even fast path returns 2 as smaller factor");
    TEST_PASS("even n fast path");
}

void test_concurrent_splits() {
    // 4 threads × 100 splits — verify no race, all yield valid splits.
    constexpr int NUM_THREADS = 4;
    constexpr int PER_THREAD = 100;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    std::atomic<int> fail_count{0};

    uint64_t p = 33554467ULL, q = 33554497ULL;
    Integer n(p * q);

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                int seed = t * 1000 + i + 1;
                auto result = BrentPollardRho::split(n, 1ULL << 18, seed);
                if (result && valid_split(n, result->first, result->second)) {
                    success_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    fail_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    // Allow a few failures on extreme seeds, but expect >= 95% success.
    int total = NUM_THREADS * PER_THREAD;
    int min_required = total * 95 / 100;
    {
        std::lock_guard<std::mutex> lk(io_mutex);
        std::cout << "  [concurrent] " << success_count.load() << "/"
                  << total << " valid splits\n";
    }
    TEST_ASSERT(success_count.load() >= min_required,
                "concurrent splits should mostly succeed");
    TEST_PASS("concurrent 4t x 100 splits no race");
}

void test_stats_accumulate() {
    // Stats should accumulate across calls.
    auto& stats = BrentPollardRho::global_stats();
    stats.reset();
    Integer n = make_int(35);
    (void)BrentPollardRho::split(n);
    (void)BrentPollardRho::split(n, 1ULL << 16, 2);
    uint64_t tried = stats.tried.load();
    uint64_t succ = stats.succ.load();
    TEST_ASSERT(tried >= 2, "tried count should be >= 2");
    TEST_ASSERT(succ >= 2, "succ count should be >= 2");
    TEST_PASS("stats accumulate across calls");
}

void test_record_flag_off() {
    auto& stats = BrentPollardRho::global_stats();
    stats.reset();
    Integer n = make_int(35);
    (void)BrentPollardRho::split(n, 1ULL << 16, 1, /*record=*/false);
    TEST_ASSERT(stats.tried.load() == 0,
                "record=false must not touch stats");
    TEST_PASS("record=false skips stats");
}

int main() {
    std::cout << "===========================================\n";
    std::cout << "  BrentPollardRho Unit Tests\n";
    std::cout << "===========================================\n\n";

    test_trivial_semiprime();
    test_balanced_50bit_semiprime();
    test_unbalanced_factor();
    test_carmichael_561();
    test_near_square();
    test_twin_prime_product();
    test_prime_input();
    test_perfect_power_p_squared();
    test_perfect_power_p_cubed();
    test_max_iter_budget_respected();
    test_determinism_same_seed();
    test_different_seed_retry();
    test_integer_slow_path_large();
    test_too_small_n();
    test_even_n();
    test_concurrent_splits();
    test_stats_accumulate();
    test_record_flag_off();

    std::cout << "\n===========================================\n";
    std::cout << "  Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    std::cout << "===========================================\n";

    return tests_failed > 0 ? 1 : 0;
}
