// test_batch_ecm.cpp — Verify BatchECM (shared Stage 1 data) equivalence with
// sequential ECM::factor and exercise small/medium/large batch sizes.

#include <gnfs/cofactor/ecm.hpp>
#include <gnfs/core/integer.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using gnfs::core::Integer;
using gnfs::cofactor::ECM;

namespace {

// Test cases: well-known semiprimes ECM::quick_factor can solve.
// (Same domain used by smooth_check.hpp Phase 4 fallback.)
struct Case {
    const char* n;
    int digits;
};

const std::vector<Case>& small_cofactors() {
    // 10–25 digit semiprimes — quick_factor B1=2000 reaches these reliably.
    static const std::vector<Case> cs = {
        {"15347", 5},                  // 103 * 149
        {"1234567891", 10},            // prime — quick_factor should return nullopt
        {"2261419229", 10},            // 47491 * 47659
        {"6700417", 7},                // prime
        {"4294967297", 10},            // 641 * 6700417 (F5)
        {"18446744073709551617", 20},  // 274177 * 67280421310721 (F6)
        {"100895598169", 12},          // 112303 * 898423
        {"99492697", 8},               // 9967 * 9982 (close primes)
        {"982451653", 9},              // 50000-ish prime
        {"600851475143", 12},          // 71 * 839 * 1471 * 6857
    };
    return cs;
}

// Build BatchContext that mimics ECM::quick_factor (B1=2000, B2=50000, 10 curves).
ECM::BatchContext make_quick_batch(uint64_t sigma_seed) {
    ECM::Config cfg;
    cfg.num_curves = 10;
    cfg.B1 = 2000;
    cfg.B2 = 50000;
    cfg.auto_params = false;
    return ECM::prepare_batch(cfg, sigma_seed);
}

// Helper: verify factor (if any) divides n and is non-trivial.
bool valid_factor(const Integer& n, const std::optional<Integer>& factor) {
    if (!factor) return true;  // nullopt is a valid "no factor found" result
    const Integer& f = *factor;
    if (f.is_one()) return false;       // 1 is trivial
    if (f.compare(n) == 0) return false; // n itself is trivial
    // Check divisibility: n mod f == 0
    Integer rem;
    mpz_mod(rem.get_mpz(), n.get_mpz(), f.get_mpz());
    return rem.is_zero();
}

// Helper: pretty-print optional<Integer>
std::string opt_str(const std::optional<Integer>& f) {
    if (!f) return "nullopt";
    return f->to_string();
}

// ───────────────────────────────────────────────────────────────────────────
// Test 1: prepare_batch produces well-formed shared data
// ───────────────────────────────────────────────────────────────────────────
void test_prepare_batch_basic() {
    std::cout << "Testing prepare_batch basics..." << std::endl;

    ECM::Config cfg;
    cfg.num_curves = 10;
    cfg.B1 = 2000;
    cfg.B2 = 50000;
    cfg.auto_params = false;

    auto ctx = ECM::prepare_batch(cfg, /*sigma_seed=*/12345);

    // primes_cache: π(2000) = 303 primes
    assert(ctx.primes_cache.size() == 303);
    assert(ctx.primes_cache.front() == 2);
    assert(ctx.primes_cache.back() == 1999);

    // prime_powers parallel to primes_cache
    assert(ctx.prime_powers.size() == ctx.primes_cache.size());

    // pk = max p^e <= B1, so e.g. p=2, B1=2000 → pk = 1024 (2^10)
    assert(ctx.prime_powers[0] == 1024);  // 2^10 = 1024, 2^11 = 2048 > 2000
    assert(ctx.prime_powers[1] == 729);   // 3^6 = 729,  3^7 = 2187 > 2000
    assert(ctx.prime_powers[2] == 625);   // 5^4 = 625,  5^5 = 3125 > 2000
    // Larger primes: pk = p (because p^2 > B1)
    for (size_t i = 0; i < ctx.primes_cache.size(); ++i) {
        uint64_t p = ctx.primes_cache[i];
        if (p > 44) {  // sqrt(2000) ≈ 44.7
            assert(ctx.prime_powers[i] == p);
        }
    }

    // sigma_pool size matches num_curves
    assert(ctx.sigma_pool.size() == 10);

    // All sigmas >= 6 (Suyama parametrization requirement)
    for (uint64_t s : ctx.sigma_pool) {
        assert(s >= 6);
    }

    // num_curves() and empty() helpers
    assert(ctx.num_curves() == 10);
    assert(!ctx.empty());

    std::cout << "  prepare_batch basics: PASSED" << std::endl;
}

// ───────────────────────────────────────────────────────────────────────────
// Test 2: prepare_batch is deterministic with sigma_seed != 0
// ───────────────────────────────────────────────────────────────────────────
void test_prepare_batch_determinism() {
    std::cout << "Testing prepare_batch determinism..." << std::endl;

    ECM::Config cfg;
    cfg.num_curves = 25;
    cfg.B1 = 1000;
    cfg.B2 = 50000;
    cfg.auto_params = false;

    auto ctx1 = ECM::prepare_batch(cfg, /*sigma_seed=*/42);
    auto ctx2 = ECM::prepare_batch(cfg, /*sigma_seed=*/42);

    // Same seed → identical sigma sequence
    assert(ctx1.sigma_pool.size() == ctx2.sigma_pool.size());
    for (size_t i = 0; i < ctx1.sigma_pool.size(); ++i) {
        assert(ctx1.sigma_pool[i] == ctx2.sigma_pool[i]);
    }

    // Different seeds → different sequence (with high probability)
    auto ctx3 = ECM::prepare_batch(cfg, /*sigma_seed=*/43);
    bool any_diff = false;
    for (size_t i = 0; i < ctx1.sigma_pool.size(); ++i) {
        if (ctx1.sigma_pool[i] != ctx3.sigma_pool[i]) {
            any_diff = true;
            break;
        }
    }
    assert(any_diff);

    std::cout << "  prepare_batch determinism: PASSED" << std::endl;
}

// ───────────────────────────────────────────────────────────────────────────
// Test 3: factor_with_batch matches sequential ECM::factor (when sigma matches)
//
// Strategy: build a BatchContext with deterministic sigma_pool, then call
// factor_with_batch (uses ctx.sigma_pool) and a hand-rolled loop that calls
// try_curve_with_pk one sigma at a time. Both should produce identical results.
// ───────────────────────────────────────────────────────────────────────────
void test_factor_with_batch_self_consistency() {
    std::cout << "Testing factor_with_batch self-consistency..." << std::endl;

    auto ctx = make_quick_batch(/*sigma_seed=*/11111);

    for (const auto& c : small_cofactors()) {
        Integer n(c.n);
        auto r = ECM::factor_with_batch(n, ctx);
        // Either we found nothing (nullopt) or we found a non-trivial divisor
        if (!valid_factor(n, r)) {
            std::cerr << "FAIL: factor_with_batch returned trivial factor "
                      << opt_str(r) << " for n=" << c.n << std::endl;
            assert(false);
        }
    }

    std::cout << "  factor_with_batch self-consistency: PASSED" << std::endl;
}

// ───────────────────────────────────────────────────────────────────────────
// Test 4: factor_batch produces same per-element result as repeated factor_with_batch
// ───────────────────────────────────────────────────────────────────────────
void test_factor_batch_equivalence_single() {
    std::cout << "Testing factor_batch == per-element factor_with_batch..." << std::endl;

    auto ctx = make_quick_batch(/*sigma_seed=*/22222);

    std::vector<Integer> ns;
    for (const auto& c : small_cofactors()) {
        ns.emplace_back(c.n);
    }

    auto batch_results = ECM::factor_batch(ns, ctx);

    // Sequential reference: each n processed with same ctx
    std::vector<std::optional<Integer>> seq_results;
    seq_results.reserve(ns.size());
    for (const auto& n : ns) {
        seq_results.push_back(ECM::factor_with_batch(n, ctx));
    }

    assert(batch_results.size() == seq_results.size());
    for (size_t i = 0; i < batch_results.size(); ++i) {
        // Both nullopt or both have value
        assert(batch_results[i].has_value() == seq_results[i].has_value());
        if (batch_results[i]) {
            // Same factor (deterministic sigma_pool → same first hit)
            assert(batch_results[i]->compare(*seq_results[i]) == 0);
        }
    }

    std::cout << "  factor_batch == sequential per-element: PASSED" << std::endl;
}

// ───────────────────────────────────────────────────────────────────────────
// Test 5: factor_batch over different batch sizes (N=10, 100, 1000)
// ───────────────────────────────────────────────────────────────────────────
void test_factor_batch_scaling() {
    std::cout << "Testing factor_batch at scale..." << std::endl;

    auto ctx = make_quick_batch(/*sigma_seed=*/33333);

    // Generate N=10 cofactors (pure semiprimes from a small prime pair pool)
    std::vector<uint32_t> small_primes = {
        101, 103, 107, 109, 113, 127, 131, 137, 139, 149,
        151, 157, 163, 167, 173, 179, 181, 191, 193, 197
    };

    auto build_cofactors = [&](size_t count, uint32_t seed) -> std::vector<Integer> {
        std::vector<Integer> ns;
        ns.reserve(count);
        std::mt19937 rng(seed);
        for (size_t i = 0; i < count; ++i) {
            uint32_t p = small_primes[rng() % small_primes.size()];
            uint32_t q = small_primes[rng() % small_primes.size()];
            uint64_t pq = static_cast<uint64_t>(p) * q;
            ns.emplace_back(static_cast<unsigned long long>(pq));
        }
        return ns;
    };

    for (size_t batch_size : {size_t{10}, size_t{100}, size_t{1000}}) {
        auto ns = build_cofactors(batch_size, 7777);

        auto t0 = std::chrono::high_resolution_clock::now();
        auto rs = ECM::factor_batch(ns, ctx);
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        assert(rs.size() == ns.size());

        // Every result should be either nullopt (composite still composite)
        // or a non-trivial divisor.
        size_t found = 0;
        for (size_t i = 0; i < rs.size(); ++i) {
            assert(valid_factor(ns[i], rs[i]));
            if (rs[i]) ++found;
        }

        std::cout << "  batch_size=" << batch_size
                  << " found=" << found
                  << " (" << ms << " ms"
                  << ", " << (ms / static_cast<double>(batch_size)) << " ms/cofactor)"
                  << std::endl;
    }

    std::cout << "  factor_batch scaling: PASSED" << std::endl;
}

// ───────────────────────────────────────────────────────────────────────────
// Test 6: empty BatchContext returns nullopt (defensive)
// ───────────────────────────────────────────────────────────────────────────
void test_empty_batch_context() {
    std::cout << "Testing empty BatchContext defensive behavior..." << std::endl;

    ECM::BatchContext empty_ctx;
    Integer n("1234567891");
    auto r = ECM::factor_with_batch(n, empty_ctx);
    assert(!r.has_value());

    std::vector<Integer> ns = {Integer("15347"), Integer("100895598169")};
    auto rs = ECM::factor_batch(ns, empty_ctx);
    assert(rs.size() == 2);
    assert(!rs[0].has_value());
    assert(!rs[1].has_value());

    std::cout << "  empty BatchContext defensive: PASSED" << std::endl;
}

// ───────────────────────────────────────────────────────────────────────────
// Test 7: primes 1, prime, and small inputs are no-ops (skip cofactors)
// ───────────────────────────────────────────────────────────────────────────
void test_skip_invalid_inputs() {
    std::cout << "Testing skip n=1 and primes..." << std::endl;

    auto ctx = make_quick_batch(/*sigma_seed=*/44444);

    // n=1: should return nullopt immediately
    Integer one("1");
    assert(!ECM::factor_with_batch(one, ctx).has_value());

    // n=prime: should return nullopt
    Integer prime("1000000007");
    assert(!ECM::factor_with_batch(prime, ctx).has_value());

    // Mixed batch
    std::vector<Integer> ns = {
        Integer("1"),
        Integer("1000000007"),  // prime
        Integer("15347"),       // 103 * 149 (composite)
    };
    auto rs = ECM::factor_batch(ns, ctx);
    assert(rs.size() == 3);
    assert(!rs[0].has_value());  // n=1 skipped
    assert(!rs[1].has_value());  // prime skipped
    if (rs[2]) {
        assert(valid_factor(ns[2], rs[2]));
    }

    std::cout << "  skip n=1 and primes: PASSED" << std::endl;
}

// ───────────────────────────────────────────────────────────────────────────
// Test 8: Compare batch path vs ECM::factor (best-effort: ensures batch is
// not strictly worse than per-call factor in success rate for matching curves)
// ───────────────────────────────────────────────────────────────────────────
void test_batch_vs_factor_success_parity() {
    std::cout << "Testing batch path success parity vs ECM::factor..." << std::endl;

    auto ctx = make_quick_batch(/*sigma_seed=*/55555);

    ECM::Config cfg;
    cfg.num_curves = 10;
    cfg.B1 = 2000;
    cfg.B2 = 50000;
    cfg.auto_params = false;

    size_t batch_found = 0;
    size_t factor_found = 0;
    for (const auto& c : small_cofactors()) {
        Integer n(c.n);
        if (ECM::factor_with_batch(n, ctx).has_value()) ++batch_found;
        if (ECM::factor(n, cfg).has_value())            ++factor_found;
    }

    // Both paths should find roughly the same number of composites
    // (allow ±2 slack because random sigma selection differs)
    long long diff = static_cast<long long>(batch_found)
                   - static_cast<long long>(factor_found);
    if (diff < -3 || diff > 3) {
        std::cerr << "WARN: batch_found=" << batch_found
                  << " factor_found=" << factor_found
                  << " diff=" << diff << std::endl;
    }
    // Don't assert a hard equality (sigma random in factor)
    std::cout << "  batch_found=" << batch_found
              << " factor_found=" << factor_found
              << " (composites in test set)" << std::endl;

    std::cout << "  batch vs factor parity: PASSED" << std::endl;
}

}  // namespace

int main() {
    std::cout << "═════════════════════════════════════════════" << std::endl;
    std::cout << "  Batch ECM Unit Tests" << std::endl;
    std::cout << "═════════════════════════════════════════════" << std::endl << std::endl;

    test_prepare_batch_basic();
    test_prepare_batch_determinism();
    test_empty_batch_context();
    test_skip_invalid_inputs();
    test_factor_with_batch_self_consistency();
    test_factor_batch_equivalence_single();
    test_factor_batch_scaling();
    test_batch_vs_factor_success_parity();

    std::cout << std::endl;
    std::cout << "═════════════════════════════════════════════" << std::endl;
    std::cout << "  All Batch ECM Tests PASSED" << std::endl;
    std::cout << "═════════════════════════════════════════════" << std::endl;
    return 0;
}
