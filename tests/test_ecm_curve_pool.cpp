// test_ecm_curve_pool.cpp — Unit tests for ECM Montgomery curve warm-pool
// (T5 Stage 2).
//
// Covers:
//   - ENV parser (GNFS_ECM_CURVE_POOL)
//   - pool_size = 0 (disabled): pop_curve must fall back synchronously
//   - pool_size = 8 with 8 sigmas: 8 pops succeed, 9th fallback exhausted
//   - pool_size = 4 with 16 sigmas: first 4 pre-built, next 12 sync fallback
//   - sigma < 6 sentinel: build_suyama_curve returns valid=false
//   - cached (A, x_0, z_0) reproduce the in-place ECM curve setup
//   - parallel pop_curve from many threads: every pop succeeds, no double-count
//   - lucky-factor capture: hand-crafted N where Suyama setup produces a
//     non-trivial den-gcd hit (deterministic for the chosen sigma).

#include <gnfs/cofactor/ecm_curve_pool.hpp>
#include <gnfs/core/integer.hpp>

#include "support/test_check.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using gnfs::cofactor::build_suyama_curve;
using gnfs::cofactor::CachedCurve;
using gnfs::cofactor::ecm_curve_pool_size_from_env;
using gnfs::cofactor::EcmCurvePool;
using gnfs::core::Integer;

namespace {

// Helper: deterministic sigma sequence starting from a seed >= 6.
std::vector<uint64_t> make_sigmas(uint64_t start, size_t count) {
    std::vector<uint64_t> sg;
    sg.reserve(count);
    for (size_t i = 0; i < count; ++i)
        sg.push_back(start + i);
    return sg;
}

// Reference Suyama setup (mirrors ECM::try_curve_with_pk) for parity check.
// Returns (a24, x0, z0) all reduced mod n.
struct RefCurve {
    Integer a24;
    Integer x0;
    Integer z0;
    bool valid;
    std::optional<Integer> lucky;
};

RefCurve ref_suyama(const Integer& n, uint64_t sigma) {
    RefCurve r;
    r.valid = false;
    if (sigma < 6)
        return r;

    Integer u(static_cast<unsigned long long>(sigma * sigma - 5));
    u %= n;
    Integer v(static_cast<unsigned long long>(4 * sigma));
    v %= n;
    Integer x0;
    mpz_powm_ui(x0.get_mpz(), u.get_mpz(), 3, n.get_mpz());
    Integer z0;
    mpz_powm_ui(z0.get_mpz(), v.get_mpz(), 3, n.get_mpz());
    Integer diff;
    mpz_sub(diff.get_mpz(), v.get_mpz(), u.get_mpz());
    if (diff.is_negative())
        diff += n;
    diff %= n;
    Integer diff3;
    mpz_powm_ui(diff3.get_mpz(), diff.get_mpz(), 3, n.get_mpz());
    Integer sum3u_v;
    sum3u_v = v;
    mpz_addmul_ui(sum3u_v.get_mpz(), u.get_mpz(), 3);
    sum3u_v %= n;
    Integer num;
    mpz_mul(num.get_mpz(), diff3.get_mpz(), sum3u_v.get_mpz());
    num %= n;
    Integer den;
    mpz_mul(den.get_mpz(), x0.get_mpz(), v.get_mpz());
    den %= n;
    mpz_mul_2exp(den.get_mpz(), den.get_mpz(), 4);
    den %= n;

    Integer g = gnfs::core::gcd(den, n);
    if (!g.is_one()) {
        if (g.compare(n) == 0) {
            r.valid = false;
            return r;
        }
        r.valid = true;
        r.lucky = std::move(g);
        return r;
    }
    Integer denv = gnfs::core::mod_inverse(den, n);
    if (denv.is_zero()) {
        r.valid = false;
        return r;
    }
    Integer a24;
    mpz_mul(a24.get_mpz(), num.get_mpz(), denv.get_mpz());
    a24 %= n;
    r.a24 = std::move(a24);
    r.x0 = std::move(x0);
    r.z0 = std::move(z0);
    r.valid = true;
    return r;
}

// ───────────────────────────────────────────────────────────────────────────
// Test 1: ENV parser
// ───────────────────────────────────────────────────────────────────────────
void test_env_parser() {
    std::cout << "Test 1: env parser..." << std::flush;

    unsetenv("GNFS_ECM_CURVE_POOL");
    assert(ecm_curve_pool_size_from_env() == 0);

    setenv("GNFS_ECM_CURVE_POOL", "", 1);
    assert(ecm_curve_pool_size_from_env() == 0);

    setenv("GNFS_ECM_CURVE_POOL", "abc", 1);
    assert(ecm_curve_pool_size_from_env() == 0);

    // A leading minus must remain disabled. `strtoul` otherwise treats a
    // negative value as an unsigned wraparound and the cap would enable it.
    setenv("GNFS_ECM_CURVE_POOL", "-1", 1);
    GNFS_TEST_CHECK(ecm_curve_pool_size_from_env() == 0);
    setenv("GNFS_ECM_CURVE_POOL", "  -4", 1);
    GNFS_TEST_CHECK(ecm_curve_pool_size_from_env() == 0);

    // Numeric prefixes followed by junk are invalid, not opt-in values.
    setenv("GNFS_ECM_CURVE_POOL", "4junk", 1);
    GNFS_TEST_CHECK(ecm_curve_pool_size_from_env() == 0);
    setenv("GNFS_ECM_CURVE_POOL", "184467440737095516160", 1);
    GNFS_TEST_CHECK(ecm_curve_pool_size_from_env() == 0);

    setenv("GNFS_ECM_CURVE_POOL", "0", 1);
    assert(ecm_curve_pool_size_from_env() == 0);
    setenv("GNFS_ECM_CURVE_POOL", "1", 1);
    assert(ecm_curve_pool_size_from_env() == 0);
    setenv("GNFS_ECM_CURVE_POOL", "3", 1);
    assert(ecm_curve_pool_size_from_env() == 0);

    setenv("GNFS_ECM_CURVE_POOL", "4", 1);
    assert(ecm_curve_pool_size_from_env() == 4);
    setenv("GNFS_ECM_CURVE_POOL", "16", 1);
    assert(ecm_curve_pool_size_from_env() == 16);

    setenv("GNFS_ECM_CURVE_POOL", "99999", 1);
    assert(ecm_curve_pool_size_from_env() == 1024);

    unsetenv("GNFS_ECM_CURVE_POOL");

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 2: pool_size = 0 (disabled): pop drains immediately, returns invalid
// ───────────────────────────────────────────────────────────────────────────
void test_disabled_pool() {
    std::cout << "Test 2: disabled pool..." << std::flush;

    // N = 15347 = 103 * 149 (small composite for fast setup).
    Integer n{uint64_t{15347}};
    EcmCurvePool pool(0, n, make_sigmas(6, 4));

    assert(pool.size() == 0);
    assert(pool.empty());

    // Pop falls through to synchronous build (which itself uses next sigma).
    // sigmas_.size()=4 but pool_size=0, so all 4 sigmas remain for fallback.
    for (int i = 0; i < 4; ++i) {
        CachedCurve c = pool.pop_curve();
        // Each fallback build for these sigmas should succeed (small N, sigma >= 6).
        assert(c.valid);
        assert(c.sigma >= 6);
    }
    // 5th pop drains the sigma reserve too — returns invalid.
    CachedCurve c5 = pool.pop_curve();
    assert(!c5.valid);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 3: pool_size = 8 with 8 sigmas; 8 pops succeed, 9th exhausted
// ───────────────────────────────────────────────────────────────────────────
void test_pool_8_exact() {
    std::cout << "Test 3: pool=8, sigmas=8..." << std::flush;

    Integer n{uint64_t{15347}};
    EcmCurvePool pool(8, n, make_sigmas(100, 8));
    assert(pool.size() == 8);

    std::set<uint64_t> seen;
    for (int i = 0; i < 8; ++i) {
        CachedCurve c = pool.pop_curve();
        assert(c.valid);
        assert(c.sigma >= 100 && c.sigma < 108);
        // Pool dispenses each sigma at most once.
        assert(seen.insert(c.sigma).second);
    }
    assert(pool.size() == 0);

    // 9th pop: no pre-built + no remaining sigmas → invalid.
    CachedCurve c9 = pool.pop_curve();
    assert(!c9.valid);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 4: pool_size = 4 with 16 sigmas; first 4 pre-built, next 12 fallback
// ───────────────────────────────────────────────────────────────────────────
void test_pool_smaller_than_sigmas() {
    std::cout << "Test 4: pool=4, sigmas=16..." << std::flush;

    Integer n{uint64_t{15347}};
    EcmCurvePool pool(4, n, make_sigmas(200, 16));

    assert(pool.size() == 4);
    assert(pool.total_sigmas() == 16);
    assert(pool.next_sigma_index() == 4);

    std::set<uint64_t> seen;
    // Pop all 4 pre-built.
    for (int i = 0; i < 4; ++i) {
        CachedCurve c = pool.pop_curve();
        assert(c.valid);
        // Pre-built ones use sigmas_[0..3] = 200..203.
        assert(c.sigma >= 200 && c.sigma < 204);
        assert(seen.insert(c.sigma).second);
    }
    assert(pool.size() == 0);

    // Pop next 12 — fallback synchronously through sigmas_[4..15].
    for (int i = 0; i < 12; ++i) {
        CachedCurve c = pool.pop_curve();
        assert(c.valid);
        assert(c.sigma >= 204 && c.sigma < 216);
        assert(seen.insert(c.sigma).second);
    }

    // 17th pop: drained.
    CachedCurve cend = pool.pop_curve();
    assert(!cend.valid);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 5: sigma < 6 returns valid=false from build_suyama_curve
// ───────────────────────────────────────────────────────────────────────────
void test_sigma_below_six() {
    std::cout << "Test 5: sigma < 6 sentinel..." << std::flush;

    Integer n{uint64_t{15347}};
    for (uint64_t sg = 0; sg < 6; ++sg) {
        CachedCurve c = build_suyama_curve(n, sg);
        assert(!c.valid);
    }

    // sigma = 6 must succeed.
    CachedCurve c6 = build_suyama_curve(n, 6);
    assert(c6.valid);

    // Pool also exposes the same behaviour via pop fallback.
    std::vector<uint64_t> sgs = {3, 4, 5};
    EcmCurvePool pool(0, n, sgs);
    for (size_t i = 0; i < 3; ++i) {
        CachedCurve c = pool.pop_curve();
        // Build with sigma<6 returns valid=false.
        assert(!c.valid);
    }

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 6: invalid moduli and wide sigma arithmetic
// ───────────────────────────────────────────────────────────────────────────
void test_numeric_boundaries() {
    std::cout << "Test 6: numeric boundaries..." << std::flush;

    const Integer zero(0);
    const Integer negative(-1);
    const Integer one(1);
    for (const Integer& modulus : {zero, negative, one}) {
        const CachedCurve c = build_suyama_curve(modulus, 6);
        GNFS_TEST_CHECK(!c.valid);
        GNFS_TEST_CHECK(!c.lucky_factor.has_value());
    }

    // This used to wrap sigma*sigma and 4*sigma before reaching GMP. Use a
    // near-maximum sigma whose residue is nonzero for this prime modulus, so
    // the setup exercises wide arithmetic without an intentional g=n hit.
    const Integer prime_modulus("6700417");
    const CachedCurve wide =
        build_suyama_curve(prime_modulus, std::numeric_limits<uint64_t>::max() - 1);
    GNFS_TEST_CHECK(wide.valid);
    GNFS_TEST_CHECK(!wide.lucky_factor.has_value());

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 7: cached (A, x_0, z_0) match reference Suyama setup
// ───────────────────────────────────────────────────────────────────────────
void test_cached_matches_reference() {
    std::cout << "Test 6: cached vs reference..." << std::flush;

    // Pick a mid-size composite where (A, x_0, z_0) span the GMP path.
    // 2261419229 = 47491 * 47659 (~31 bit semiprime).
    Integer n{uint64_t{2261419229ULL}};

    std::vector<uint64_t> sigmas = make_sigmas(10, 10);
    EcmCurvePool pool(10, n, sigmas);

    std::vector<CachedCurve> popped;
    for (int i = 0; i < 10; ++i) {
        popped.push_back(pool.pop_curve());
    }

    // Each popped curve must match the reference for its sigma.
    // (Pool ordering is LIFO from sigmas_[0..9] population — exact order is
    // not part of the contract, so we match by sigma.)
    for (const auto& c : popped) {
        assert(c.valid);
        RefCurve r = ref_suyama(n, c.sigma);
        assert(r.valid);
        if (r.lucky) {
            // Reference hit lucky; cache must also have it.
            assert(c.lucky_factor.has_value());
            assert(c.lucky_factor->compare(*r.lucky) == 0);
        } else {
            assert(!c.lucky_factor.has_value());
            // (A, x_0, z_0) must agree bit-for-bit (all reduced mod n already).
            assert(c.A.compare(r.a24) == 0);
            assert(c.x_0.compare(r.x0) == 0);
            assert(c.z_0.compare(r.z0) == 0);
        }
    }

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 8: parallel pop from many threads — no duplicates, all served
// ───────────────────────────────────────────────────────────────────────────
void test_parallel_pop() {
    std::cout << "Test 7: parallel pop..." << std::flush;

    Integer n{uint64_t{15347}};
    const size_t POOL = 64;
    EcmCurvePool pool(POOL, n, make_sigmas(1000, POOL));

    std::mutex seen_mu;
    std::set<uint64_t> seen;
    std::atomic<size_t> ok_count{0};

    const size_t NTHREADS = 8;
    const size_t PER_THREAD = POOL / NTHREADS;
    std::vector<std::thread> ths;
    for (size_t t = 0; t < NTHREADS; ++t) {
        ths.emplace_back([&]() {
            for (size_t i = 0; i < PER_THREAD; ++i) {
                CachedCurve c = pool.pop_curve();
                if (!c.valid)
                    continue;
                {
                    std::lock_guard<std::mutex> lock(seen_mu);
                    // Sigma must be unique across threads.
                    bool inserted = seen.insert(c.sigma).second;
                    assert(inserted);
                }
                ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : ths)
        th.join();

    // All POOL curves should have been dispensed exactly once.
    assert(ok_count.load() == POOL);
    assert(seen.size() == POOL);
    assert(pool.empty());

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 9: lucky-factor capture (deterministic for a hand-picked sigma).
//
// We construct N divisible by den = 16 * x_0 * v for a specific sigma. Easier
// approach: pick sigma=6 → u=31, v=24, x_0=u^3=29791. Let N share a factor
// with 16 * 29791 * 24. We pick N = 12 * 29791 = 357492 (so gcd(den, N) = 12
// for sigma=6). Result: lucky_factor non-empty, A/x_0/z_0 still meaningful
// but unused.
// ───────────────────────────────────────────────────────────────────────────
void test_lucky_factor_capture() {
    std::cout << "Test 8: lucky factor capture..." << std::flush;

    // Sigma=6: u=31, v=24, x_0=29791. den = 16 * 29791 * 24 = 11439744.
    // gcd(11439744, N) — we want this to be > 1 and < N.
    // Pick N = 12 * very_large_prime so gcd(den, N) = 12.
    // very_large_prime so N is composite and not equal to gcd.
    Integer N{uint64_t{12ULL * 9999991ULL}}; // 119999892, factor 12 known.

    CachedCurve c = build_suyama_curve(N, 6);
    assert(c.valid);
    assert(c.lucky_factor.has_value());
    // gcd(den, N) must divide N and be != 1 and != N.
    Integer rem;
    mpz_mod(rem.get_mpz(), N.get_mpz(), c.lucky_factor->get_mpz());
    assert(rem.is_zero());
    assert(!c.lucky_factor->is_one());
    assert(c.lucky_factor->compare(N) < 0);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 10: pool_size = 0, no sigmas → pop returns invalid immediately
// ───────────────────────────────────────────────────────────────────────────
void test_empty_sigmas() {
    std::cout << "Test 9: empty sigmas..." << std::flush;

    Integer n{uint64_t{15347}};
    EcmCurvePool pool(8, n, {});
    assert(pool.size() == 0);
    assert(pool.total_sigmas() == 0);

    CachedCurve c = pool.pop_curve();
    assert(!c.valid);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 11: large pool stress — 200 curves, all build and pop successfully
// ───────────────────────────────────────────────────────────────────────────
void test_large_pool_stress() {
    std::cout << "Test 10: large pool stress..." << std::flush;

    Integer n{uint64_t{2261419229ULL}};
    const size_t POOL = 200;
    EcmCurvePool pool(POOL, n, make_sigmas(50000, POOL));

    assert(pool.size() == POOL);

    std::set<uint64_t> seen;
    for (size_t i = 0; i < POOL; ++i) {
        CachedCurve c = pool.pop_curve();
        assert(c.valid);
        assert(seen.insert(c.sigma).second);
    }
    assert(pool.empty());

    std::cout << " PASS (200 curves)\n";
}

} // namespace

int main() {
    std::cout << "=== test_ecm_curve_pool ===\n";

    test_env_parser();
    test_disabled_pool();
    test_pool_8_exact();
    test_pool_smaller_than_sigmas();
    test_sigma_below_six();
    test_numeric_boundaries();
    test_cached_matches_reference();
    test_parallel_pop();
    test_lucky_factor_capture();
    test_empty_sigmas();
    test_large_pool_stress();

    std::cout << "=== All tests passed ===\n";
    return 0;
}
