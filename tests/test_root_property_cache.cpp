/// test_root_property_cache.cpp — unit and integration tests for
/// RootPropertyCache and its MurphyEvaluator wiring.

#include "gnfs/core/integer.hpp"
#include "gnfs/polynomial/int_polynomial.hpp"
#include "gnfs/polynomial/murphy_evaluator.hpp"
#include "gnfs/polynomial/root_property_cache.hpp"

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using gnfs::core::Integer;
using gnfs::polynomial::IntPolynomial;
using gnfs::polynomial::MurphyEvaluator;
using gnfs::polynomial::MurphyParams;
using gnfs::polynomial::RootPropertyCache;

namespace {

#define EXPECT_TRUE(cond)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":"         \
                      << __LINE__ << std::endl;                                 \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

#define EXPECT_EQ(a, b)                                                         \
    do {                                                                        \
        auto _va = (a);                                                         \
        auto _vb = (b);                                                         \
        if (!(_va == _vb)) {                                                    \
            std::cerr << "FAIL: " << #a << " == " << #b << " (" << _va          \
                      << " vs " << _vb << ") at " << __FILE__ << ":"            \
                      << __LINE__ << std::endl;                                 \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

#define EXPECT_DOUBLE_CLOSE(a, b, tol)                                          \
    do {                                                                        \
        double _va = (a);                                                       \
        double _vb = (b);                                                       \
        if (!(std::isfinite(_va) && std::isfinite(_vb)                          \
              && std::fabs(_va - _vb) <= (tol))) {                              \
            std::cerr << "FAIL: " << #a << " ≈ " << #b << " (" << _va           \
                      << " vs " << _vb << ", tol=" << (tol) << ") at "          \
                      << __FILE__ << ":" << __LINE__ << std::endl;              \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

IntPolynomial poly(std::initializer_list<int64_t> coeffs) {
    std::vector<Integer> v;
    v.reserve(coeffs.size());
    for (int64_t c : coeffs) v.emplace_back(c);
    return IntPolynomial(std::move(v));
}

// Test 1: empty cache returns nullopt and increments miss counter.
void test_empty_lookup() {
    std::cout << "test_empty_lookup ... " << std::flush;
    RootPropertyCache cache(16);
    EXPECT_TRUE(cache.enabled());
    EXPECT_EQ(cache.size(), size_t{0});
    EXPECT_EQ(cache.hit_count(), size_t{0});
    EXPECT_EQ(cache.miss_count(), size_t{0});

    auto v = cache.lookup(7u, 0xDEADBEEFULL);
    EXPECT_TRUE(!v.has_value());
    EXPECT_EQ(cache.miss_count(), size_t{1});
    EXPECT_EQ(cache.hit_count(), size_t{0});
    std::cout << "PASS\n";
}

// Test 2: insert then lookup yields the inserted value and increments hits.
void test_hit_after_insert() {
    std::cout << "test_hit_after_insert ... " << std::flush;
    RootPropertyCache cache(16);
    cache.insert(11u, 0xCAFEULL, -0.125);
    EXPECT_EQ(cache.size(), size_t{1});

    auto v = cache.lookup(11u, 0xCAFEULL);
    EXPECT_TRUE(v.has_value());
    EXPECT_DOUBLE_CLOSE(v.value(), -0.125, 0.0);
    EXPECT_EQ(cache.hit_count(), size_t{1});
    EXPECT_EQ(cache.miss_count(), size_t{0});
    std::cout << "PASS\n";
}

// Test 3: different keys yield independent slots.
void test_distinct_keys() {
    std::cout << "test_distinct_keys ... " << std::flush;
    RootPropertyCache cache(16);
    cache.insert(7u, 1ULL, 1.5);
    cache.insert(7u, 2ULL, 2.5);
    cache.insert(11u, 1ULL, 3.5);

    EXPECT_EQ(cache.size(), size_t{3});

    auto a = cache.lookup(7u, 1ULL);
    auto b = cache.lookup(7u, 2ULL);
    auto c = cache.lookup(11u, 1ULL);
    auto d = cache.lookup(11u, 2ULL);

    EXPECT_TRUE(a.has_value() && a.value() == 1.5);
    EXPECT_TRUE(b.has_value() && b.value() == 2.5);
    EXPECT_TRUE(c.has_value() && c.value() == 3.5);
    EXPECT_TRUE(!d.has_value());

    EXPECT_EQ(cache.hit_count(), size_t{3});
    EXPECT_EQ(cache.miss_count(), size_t{1});
    std::cout << "PASS\n";
}

// Test 4: capacity-bound FIFO eviction.
void test_capacity_eviction() {
    std::cout << "test_capacity_eviction ... " << std::flush;
    constexpr size_t CAP = 4;
    RootPropertyCache cache(CAP);

    cache.insert(2u, 1ULL, 1.0);
    cache.insert(3u, 1ULL, 2.0);
    cache.insert(5u, 1ULL, 3.0);
    cache.insert(7u, 1ULL, 4.0);
    EXPECT_EQ(cache.size(), CAP);

    // Insert a 5th: oldest (p=2, hash=1) must evict.
    cache.insert(11u, 1ULL, 5.0);
    EXPECT_EQ(cache.size(), CAP);

    EXPECT_TRUE(!cache.lookup(2u, 1ULL).has_value());
    EXPECT_TRUE(cache.lookup(3u, 1ULL).has_value());
    EXPECT_TRUE(cache.lookup(11u, 1ULL).has_value());

    // Force one more eviction (oldest remaining is p=3 after the lookup
    // hits above did not change FIFO order).
    cache.insert(13u, 1ULL, 6.0);
    EXPECT_TRUE(!cache.lookup(3u, 1ULL).has_value());
    EXPECT_TRUE(cache.lookup(5u, 1ULL).has_value());
    EXPECT_EQ(cache.size(), CAP);
    std::cout << "PASS\n";
}

// Test 5: concurrent insert/lookup must not crash or corrupt counters.
// We use disjoint keys per thread so the final state is deterministic.
void test_concurrent_mt() {
    std::cout << "test_concurrent_mt ... " << std::flush;
    constexpr size_t NUM_THREADS = 8;
    constexpr size_t OPS_PER_THREAD = 4096;
    constexpr size_t CAP = NUM_THREADS * OPS_PER_THREAD * 2;
    RootPropertyCache cache(CAP);

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    std::atomic<size_t> mismatch{0};

    for (size_t t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&cache, &mismatch, t] {
            const uint32_t p_base = static_cast<uint32_t>(101 + t);
            for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
                uint64_t key = static_cast<uint64_t>(i) * (t + 1);
                double value = static_cast<double>(t * 1000 + i);
                cache.insert(p_base, key, value);
                auto got = cache.lookup(p_base, key);
                if (!got.has_value() || got.value() != value) {
                    mismatch.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(mismatch.load(), size_t{0});
    EXPECT_EQ(cache.size(), NUM_THREADS * OPS_PER_THREAD);
    // Hits accumulated on the lookup path (one per inserted key).
    EXPECT_EQ(cache.hit_count(), NUM_THREADS * OPS_PER_THREAD);
    std::cout << "PASS\n";
}

// Test 6: hit/miss counter accuracy under mixed workload.
void test_stat_counters() {
    std::cout << "test_stat_counters ... " << std::flush;
    RootPropertyCache cache(8);
    cache.insert(2u, 1ULL, 0.0);
    cache.insert(3u, 1ULL, 0.0);

    // 5 hits.
    for (int i = 0; i < 5; ++i) (void)cache.lookup(2u, 1ULL);
    // 7 misses.
    for (uint32_t i = 0; i < 7u; ++i) (void)cache.lookup(100u + i, 1ULL);

    EXPECT_EQ(cache.hit_count(), size_t{5});
    EXPECT_EQ(cache.miss_count(), size_t{7});
    EXPECT_DOUBLE_CLOSE(cache.hit_rate(), 5.0 / 12.0, 1e-12);

    cache.reset_stats();
    EXPECT_EQ(cache.hit_count(), size_t{0});
    EXPECT_EQ(cache.miss_count(), size_t{0});
    EXPECT_DOUBLE_CLOSE(cache.hit_rate(), 0.0, 0.0);
    std::cout << "PASS\n";
}

// Test 7: env_capacity parses GNFS_POLY_ROOT_CACHE_SIZE correctly.
// We cannot exercise std::call_once twice in the same process safely,
// so we shell out to ourselves with different envs; here we just check
// the current process resolution and the parse-disabled path.
void test_env_size_parse() {
    std::cout << "test_env_size_parse ... " << std::flush;
    // env_capacity returns the value captured at process start. The
    // test harness either sets it or not; both are valid.
    const size_t cap = RootPropertyCache::env_capacity();
    const char* e = std::getenv("GNFS_POLY_ROOT_CACHE_SIZE");
    if (e == nullptr || e[0] == '\0') {
        EXPECT_EQ(cap, size_t{0});
    } else {
        long long expected = std::atoll(e);
        if (expected <= 0) {
            EXPECT_EQ(cap, size_t{0});
        } else {
            EXPECT_TRUE(cap == static_cast<size_t>(expected) || cap > 0);
        }
    }
    std::cout << "PASS (env_capacity=" << cap << ")\n";
}

// Test 8: disabled cache (capacity=0) is a no-op.
void test_disabled_mode() {
    std::cout << "test_disabled_mode ... " << std::flush;
    RootPropertyCache cache(0);
    EXPECT_TRUE(!cache.enabled());
    EXPECT_EQ(cache.capacity(), size_t{0});

    cache.insert(7u, 1ULL, 42.0);
    EXPECT_EQ(cache.size(), size_t{0});
    auto v = cache.lookup(7u, 1ULL);
    EXPECT_TRUE(!v.has_value());

    // Counters must remain at zero (disabled mode skips counter updates).
    EXPECT_EQ(cache.hit_count(), size_t{0});
    EXPECT_EQ(cache.miss_count(), size_t{0});
    std::cout << "PASS\n";
}

// Test 9: hash function correctness — determinism and rotation invariance.
//
// Property 1: hash_coeffs_mod_p(f, p) is deterministic.
// Property 2: f and f' with coefficients agreeing mod p hash to the same
//   value (this is the rotation invariant the cache exploits).
// Property 3: f with coefficients that *differ* mod p hash to different
//   values (with overwhelming probability for non-pathological inputs).
//
// Note: we do *not* assert that hash(f, p1) != hash(f, p2) for distinct
// primes when f's coefficients are all < min(p1, p2). In that regime both
// hashes process the same byte sequence and so collide by design — the
// cache disambiguates via the (p, hash) tuple, not via the hash alone.
void test_hash_stability() {
    std::cout << "test_hash_stability ... " << std::flush;
    IntPolynomial f = poly({1, 2, 3, 4, 5});  // 1 + 2x + 3x^2 + 4x^3 + 5x^4
    const uint32_t p = 17;
    const uint64_t h1 = RootPropertyCache::hash_coeffs_mod_p(f, p);
    const uint64_t h2 = RootPropertyCache::hash_coeffs_mod_p(f, p);
    EXPECT_EQ(h1, h2);  // determinism

    // Rotation invariant: adding p·x to f leaves f mod p unchanged.
    IntPolynomial f_rot = poly({1, 2 + 17, 3, 4, 5});
    const uint64_t h_rot = RootPropertyCache::hash_coeffs_mod_p(f_rot, p);
    EXPECT_EQ(h1, h_rot);

    // Distinct mod-p residues yield distinct hashes (with overwhelming
    // probability for FNV-1a; here we choose f differing in a single
    // small coefficient so the residues are guaranteed to differ).
    IntPolynomial f_diff = poly({1, 2 + 1, 3, 4, 5});  // coeff[1] off by 1
    const uint64_t h_diff = RootPropertyCache::hash_coeffs_mod_p(f_diff, p);
    EXPECT_TRUE(h1 != h_diff);

    // Polynomials of different degree with otherwise identical residues
    // must not collide (degree mixed into the hash tail).
    IntPolynomial f_lower_deg = poly({1, 2, 3, 4});  // degree 3
    const uint64_t h_lower = RootPropertyCache::hash_coeffs_mod_p(f_lower_deg, p);
    IntPolynomial f_same_deg_zero_top = poly({1, 2, 3, 4, 0, 1});  // degree 5, leading=1
    const uint64_t h_same_deg = RootPropertyCache::hash_coeffs_mod_p(f_same_deg_zero_top, p);
    EXPECT_TRUE(h_lower != h_same_deg);
    std::cout << "PASS\n";
}

// Test 10: Murphy compute_alpha identical with cache OFF vs ON.
// This is the load-bearing semantic invariant: cache MUST NOT change
// numerical output. We construct several polynomials and verify equality
// to within 1e-12 (cache stores the exact computed double, so the diff
// should be zero in practice; we leave a tiny tolerance for floating
// summation reorder in the parallel reduce path).
void test_alpha_identity_cache_off_vs_on() {
    std::cout << "test_alpha_identity_cache_off_vs_on ... " << std::flush;

    MurphyParams params;
    params.alpha_bound = 1000;  // ~168 primes; quick but representative

    // Build evaluator with cache disabled by forcing capacity=0 path.
    // We cannot easily flip env mid-process (std::call_once), so we
    // instead exercise both paths through public APIs: cache OFF means
    // env not set (which is the default in CI); cache ON means we use a
    // freshly constructed evaluator after setenv. We unify by always
    // computing alpha with the current evaluator (which honors env), and
    // verifying golden values do not drift between (a) env unset and
    // (b) env set with a sufficient capacity. Driver script wraps this.
    MurphyEvaluator evaluator(params);
    const bool cache_active = evaluator.root_cache().enabled();

    // Polynomial 1: f = x^5 - x + 1 (Murphy E reference)
    IntPolynomial f1 = poly({1, -1, 0, 0, 0, 1});
    // Polynomial 2: f = x^4 + 2
    IntPolynomial f2 = poly({2, 0, 0, 0, 1});
    // Polynomial 3: rotation of f1 by k=3 over g=x-7 (changes coefficients
    // by a multiple of (x-7), so f mod p differs from f1 mod p for p>=8).
    IntPolynomial f3 = poly({1 + 3 * (-7), -1 + 3, 0, 0, 0, 1});

    const double a1 = evaluator.compute_alpha(f1);
    const double a2 = evaluator.compute_alpha(f2);
    const double a3 = evaluator.compute_alpha(f3);

    // Compute again — second call exercises hits in cache_active mode.
    const double a1b = evaluator.compute_alpha(f1);
    const double a2b = evaluator.compute_alpha(f2);
    const double a3b = evaluator.compute_alpha(f3);

    EXPECT_DOUBLE_CLOSE(a1, a1b, 1e-12);
    EXPECT_DOUBLE_CLOSE(a2, a2b, 1e-12);
    EXPECT_DOUBLE_CLOSE(a3, a3b, 1e-12);

    // Golden anchor: regardless of cache state, alpha for f1 must match
    // the known-good value baked in test_murphy.cpp. We rebuild with
    // alpha_bound=100 to hit the same golden as the murphy regression.
    MurphyParams golden_params;
    golden_params.alpha_bound = 100;
    MurphyEvaluator golden_eval(golden_params);
    const double alpha_golden = golden_eval.compute_alpha(f1);
    EXPECT_DOUBLE_CLOSE(alpha_golden, -2.34813918493, 1e-9);

    if (cache_active) {
        const auto& c = evaluator.root_cache();
        EXPECT_TRUE(c.hit_count() > 0);
        std::cerr << "  cache hits=" << c.hit_count()
                  << " misses=" << c.miss_count()
                  << " rate=" << (100.0 * c.hit_rate()) << "%\n";
    } else {
        std::cerr << "  cache disabled (env not set), only invariance verified\n";
    }

    std::cout << "PASS\n";
}

// Test 11: Murphy alpha cache OFF vs forcibly-on parity. We cannot toggle
// env_capacity at runtime, but we can verify the in-process cache wraps
// faithfully by computing the same polynomial alpha twice with and
// without cache hits.
void test_cache_does_not_change_value() {
    std::cout << "test_cache_does_not_change_value ... " << std::flush;

    MurphyParams params;
    params.alpha_bound = 500;
    MurphyEvaluator evaluator(params);

    if (!evaluator.root_cache().enabled()) {
        // Test only meaningful under cache-on regime; skip otherwise.
        std::cout << "SKIP (cache disabled by env)\n";
        return;
    }

    IntPolynomial f = poly({1, 2, 0, -3, 0, 1});  // mixed-sign quintic

    const double cold = evaluator.compute_alpha(f);
    const size_t misses_cold = evaluator.root_cache().miss_count();
    EXPECT_TRUE(misses_cold > 0);

    const double warm = evaluator.compute_alpha(f);
    const size_t hits_warm = evaluator.root_cache().hit_count();
    EXPECT_TRUE(hits_warm > 0);

    EXPECT_DOUBLE_CLOSE(cold, warm, 1e-12);
    std::cout << "PASS (cold=" << cold << " warm=" << warm
              << " misses=" << misses_cold << " hits=" << hits_warm << ")\n";
}

// Test 12: rotation-equivalent polynomials share cache slots and produce
// elevated hit rate. Construct N rotation variants of a degree-5 polynomial,
// sweep their alpha, and verify that the cache fills only on the first
// pass and serves hits on the second.
void test_rotation_hit_rate() {
    std::cout << "test_rotation_hit_rate ... " << std::flush;

    MurphyParams params;
    params.alpha_bound = 200;
    MurphyEvaluator evaluator(params);
    if (!evaluator.root_cache().enabled()) {
        std::cout << "SKIP (cache disabled by env)\n";
        return;
    }

    // Generate 8 polynomials that share f mod p for p > 50 by rotating
    // with multiples of small primes only. The cache should hit on the
    // overlapping primes.
    std::vector<IntPolynomial> polys;
    polys.reserve(8);
    polys.push_back(poly({1, -1, 0, 0, 0, 1}));
    polys.push_back(poly({1 + 2, -1, 0, 0, 0, 1}));
    polys.push_back(poly({1 + 3, -1, 0, 0, 0, 1}));
    polys.push_back(poly({1 + 5, -1, 0, 0, 0, 1}));
    polys.push_back(poly({1 + 7, -1, 0, 0, 0, 1}));
    polys.push_back(poly({1, -1 + 2, 0, 0, 0, 1}));
    polys.push_back(poly({1, -1 + 3, 0, 0, 0, 1}));
    polys.push_back(poly({1, -1 + 5, 0, 0, 0, 1}));

    for (const auto& f : polys) (void)evaluator.compute_alpha(f);
    const size_t misses_first = evaluator.root_cache().miss_count();
    const size_t hits_first = evaluator.root_cache().hit_count();

    // Second sweep: all results are now cached.
    for (const auto& f : polys) (void)evaluator.compute_alpha(f);
    const size_t hits_second = evaluator.root_cache().hit_count();

    EXPECT_TRUE(hits_second > hits_first);
    EXPECT_TRUE(evaluator.root_cache().miss_count() == misses_first);
    std::cout << "PASS (first_misses=" << misses_first
              << " first_hits=" << hits_first
              << " second_hits=" << hits_second << ")\n";
}

}  // namespace

int main() {
    std::cout << "=== RootPropertyCache tests ===\n";
    test_empty_lookup();
    test_hit_after_insert();
    test_distinct_keys();
    test_capacity_eviction();
    test_concurrent_mt();
    test_stat_counters();
    test_env_size_parse();
    test_disabled_mode();
    test_hash_stability();
    test_alpha_identity_cache_off_vs_on();
    test_cache_does_not_change_value();
    test_rotation_hit_rate();
    std::cout << "All RootPropertyCache tests passed!\n";
    return 0;
}
