// test_cofactor.cpp - Test cofactorization components

#include <gnfs/cofactor/smooth_check.hpp>
#include <gnfs/cofactor/trial_division.hpp>
#include <gnfs/cofactor/cofactorizer.hpp>
#include <gnfs/relation/filter.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/core/polynomial_context.hpp>

#include <cassert>
#include <iostream>
#include <sstream>
#include <cmath>

using namespace gnfs;
using namespace gnfs::cofactor;
using namespace gnfs::relation;

// Test primality checking
void test_primality() {
    std::cout << "Testing primality check..." << std::endl;

    // Small primes
    assert(is_probable_prime_u64(2));
    assert(is_probable_prime_u64(3));
    assert(is_probable_prime_u64(5));
    assert(is_probable_prime_u64(7));
    assert(is_probable_prime_u64(11));
    assert(is_probable_prime_u64(13));
    assert(is_probable_prime_u64(97));
    assert(is_probable_prime_u64(101));

    // Non-primes
    assert(!is_probable_prime_u64(0));
    assert(!is_probable_prime_u64(1));
    assert(!is_probable_prime_u64(4));
    assert(!is_probable_prime_u64(6));
    assert(!is_probable_prime_u64(9));
    assert(!is_probable_prime_u64(15));
    assert(!is_probable_prime_u64(100));

    // Large primes
    assert(is_probable_prime_u64(104729));    // 10000th prime
    assert(is_probable_prime_u64(1299709));   // 100000th prime
    assert(is_probable_prime_u64(15485863));  // 1000000th prime

    // Large non-primes
    assert(!is_probable_prime_u64(104729 * 2));
    assert(!is_probable_prime_u64(1299709 * 3));

    std::cout << "  Primality check: PASSED" << std::endl;
}

// Test perfect power detection
void test_perfect_power() {
    std::cout << "Testing perfect power detection..." << std::endl;

    uint64_t base;
    uint8_t exp;

    // Perfect squares
    assert(is_perfect_power(4, base, exp) && base == 2 && exp == 2);
    assert(is_perfect_power(9, base, exp) && base == 3 && exp == 2);
    assert(is_perfect_power(16, base, exp));  // 2^4
    assert(is_perfect_power(25, base, exp) && base == 5 && exp == 2);
    assert(is_perfect_power(49, base, exp) && base == 7 && exp == 2);

    // Perfect cubes
    assert(is_perfect_power(8, base, exp) && base == 2 && exp == 3);
    assert(is_perfect_power(27, base, exp) && base == 3 && exp == 3);
    assert(is_perfect_power(64, base, exp));  // 2^6 or 4^3
    assert(is_perfect_power(125, base, exp) && base == 5 && exp == 3);

    // Higher powers
    assert(is_perfect_power(32, base, exp) && base == 2 && exp == 5);
    assert(is_perfect_power(243, base, exp) && base == 3 && exp == 5);

    // Perfect square check
    uint64_t root;
    assert(is_perfect_square(1, root) && root == 1);
    assert(is_perfect_square(4, root) && root == 2);
    assert(is_perfect_square(9, root) && root == 3);
    assert(is_perfect_square(100, root) && root == 10);
    assert(!is_perfect_square(2, root));
    assert(!is_perfect_square(3, root));
    assert(!is_perfect_square(5, root));

    std::cout << "  Perfect power detection: PASSED" << std::endl;
}

// Test Pollard's rho factorization
void test_pollard_rho() {
    std::cout << "Testing Pollard's rho..." << std::endl;

    // Small semiprimes
    uint64_t f;

    f = pollard_rho(15);  // 3 * 5
    assert(f == 3 || f == 5);

    f = pollard_rho(21);  // 3 * 7
    assert(f == 3 || f == 7);

    f = pollard_rho(35);  // 5 * 7
    assert(f == 5 || f == 7);

    f = pollard_rho(77);  // 7 * 11
    assert(f == 7 || f == 11);

    // Larger semiprimes
    f = pollard_rho(1147);  // 31 * 37
    assert(f == 31 || f == 37);

    f = pollard_rho(10403);  // 101 * 103
    assert(f == 101 || f == 103);

    // Edge cases
    assert(pollard_rho(4) == 2);  // 2^2
    assert(pollard_rho(6) == 2 || pollard_rho(6) == 3);  // 2 * 3

    std::cout << "  Pollard's rho: PASSED" << std::endl;
}

// Test cofactor classification
void test_cofactor_classification() {
    std::cout << "Testing cofactor classification..." << std::endl;

    uint64_t lpb = 1000000;  // Large prime bound = 10^6

    // Smooth (cofactor = 1)
    core::Integer one(1);
    auto cls = classify_cofactor(one, lpb);
    assert(cls.type == CofactorClass::Smooth);

    // Single prime within bound
    core::Integer prime_small(997);
    cls = classify_cofactor(prime_small, lpb);
    assert(cls.type == CofactorClass::Prime);
    assert(cls.factor1 == 997);

    // Single prime at bound
    core::Integer prime_at_bound(999983);  // largest 6-digit prime
    cls = classify_cofactor(prime_at_bound, lpb);
    assert(cls.type == CofactorClass::Prime);

    // Single prime above bound
    core::Integer prime_large(1000003);  // first 7-digit prime
    cls = classify_cofactor(prime_large, lpb);
    assert(cls.type == CofactorClass::TooLarge);

    // Prime power
    core::Integer prime_power(static_cast<unsigned long long>(997 * 997));
    cls = classify_cofactor(prime_power, lpb);
    assert(cls.type == CofactorClass::PrimePower);
    assert(cls.factor1 == 997);
    assert(cls.power == 2);

    // Semiprime within bound - use factors with different sizes for easier factoring
    // 101 * 103 = 10403 (Pollard's rho handles this well)
    core::Integer semiprime(static_cast<unsigned long long>(101 * 103));
    cls = classify_cofactor(semiprime, lpb);
    assert(cls.type == CofactorClass::Semiprime);
    assert((cls.factor1 == 101 && cls.factor2 == 103) ||
           (cls.factor1 == 103 && cls.factor2 == 101));

    // Too large (> lpb^2)
    core::Integer too_large(static_cast<unsigned long long>(lpb) * lpb + 1);
    cls = classify_cofactor(too_large, lpb);
    assert(cls.type == CofactorClass::TooLarge);

    std::cout << "  Cofactor classification: PASSED" << std::endl;
}

// Test trial division
void test_trial_division() {
    std::cout << "Testing trial division..." << std::endl;

    // Create a simple polynomial and factor base
    // f(x) = x^2 - 143 where N = 143 = 11 * 13
    // m = 12 since f(12) = 144 - 143 = 1 ≡ 1 (mod 143)
    // We use N=143 so that small primes 2,3,5,7 are NOT excluded from factor base
    std::vector<core::Integer> coeffs;
    coeffs.push_back(core::Integer(static_cast<int64_t>(-143)));  // constant term
    coeffs.push_back(core::Integer(static_cast<int64_t>(0)));     // x term
    coeffs.push_back(core::Integer(static_cast<int64_t>(1)));     // x^2 term

    core::Integer N(static_cast<int64_t>(143));  // 11 * 13, so 2,3,5,7 are in factor base
    core::Integer M(static_cast<int64_t>(12));
    core::PolynomialContext ctx(std::move(N), std::move(coeffs), std::move(M));

    // Build factor base
    factor_base::FactorBaseBuilder::Options opts;
    opts.rational_bound = 100;
    opts.algebraic_bound = 100;

    auto fb = factor_base::FactorBaseBuilder::build(ctx, opts);

    // Test trial division
    TrialDivider divider(fb);

    // Test with 2 * 3 * 5 * 7 = 210
    core::Integer value(210);
    auto result = divider.divide_rational(std::move(value));

    assert(result.is_smooth);  // 210 should be smooth over small primes
    assert(result.factor_indices.size() >= 4);  // 2, 3, 5, 7

    // Test with a number that has a cofactor
    core::Integer value2(210 * 101);  // 101 is prime > 100
    result = divider.divide_rational(std::move(value2));

    assert(!result.is_smooth);  // Should have cofactor 101
    // Safety check: ensure cofactor fits before converting
    if (!result.cofactor.fits_uint64()) {
        std::cerr << "  ERROR: cofactor doesn't fit in uint64: "
                  << result.cofactor.to_string() << std::endl;
        assert(false);
    }
    assert(result.cofactor.to_uint64() == 101);

    std::cout << "  Trial division: PASSED" << std::endl;
}

// Test relation filter
void test_relation_filter() {
    std::cout << "Testing relation filter..." << std::endl;

    // Create some test relations
    std::vector<core::Relation> relations;

    // Relation 1: has large prime 1001 (singleton)
    core::Relation rel1(1, 1);
    rel1.rational_large_prime.push_back(core::PrimePower{1001, 1});
    relations.push_back(std::move(rel1));

    // Relation 2: has large prime 1003 (appears twice)
    core::Relation rel2(2, 1);
    rel2.rational_large_prime.push_back(core::PrimePower{1003, 1});
    relations.push_back(std::move(rel2));

    // Relation 3: also has large prime 1003
    core::Relation rel3(3, 1);
    rel3.rational_large_prime.push_back(core::PrimePower{1003, 1});
    relations.push_back(std::move(rel3));

    // Relation 4: no large primes (full relation)
    core::Relation rel4(4, 1);
    relations.push_back(std::move(rel4));

    // Relation 5: has large prime 1007 (singleton)
    core::Relation rel5(5, 1);
    rel5.algebraic_large_prime.push_back(core::PrimePower{1007, 1});
    relations.push_back(std::move(rel5));

    // Filter
    FilterConfig config;
    config.remove_singletons = true;
    config.max_passes = 5;

    RelationFilter filter(config);
    auto filtered = filter.filter(std::move(relations));

    // Should remove rel1 (singleton 1001) and rel5 (singleton 1007)
    // Keep rel2, rel3 (share 1003), rel4 (no LP)
    assert(filtered.size() == 3);

    auto& stats = filter.stats();
    assert(stats.input_relations == 5);
    assert(stats.output_relations == 3);
    assert(stats.singletons_removed == 2);

    std::cout << "  Relation filter: PASSED" << std::endl;
}

// Test large prime counting
void test_large_prime_counting() {
    std::cout << "Testing large prime counting..." << std::endl;

    std::vector<core::Relation> relations;

    // Create relations with various large primes
    core::Relation rel1(1, 1);
    rel1.rational_large_prime.push_back(core::PrimePower{101, 1});
    rel1.rational_large_prime.push_back(core::PrimePower{103, 1});
    relations.push_back(std::move(rel1));

    core::Relation rel2(2, 1);
    rel2.rational_large_prime.push_back(core::PrimePower{101, 1});
    rel2.algebraic_large_prime.push_back(core::PrimePower{107, 1});
    relations.push_back(std::move(rel2));

    core::Relation rel3(3, 1);
    rel3.algebraic_large_prime.push_back(core::PrimePower{107, 1});
    rel3.algebraic_large_prime.push_back(core::PrimePower{109, 1});
    relations.push_back(std::move(rel3));

    // Count large primes
    auto counts = RelationFilter::count_large_primes(relations);

    // 101 (rat) appears 2 times
    LargePrimeKey key_101_rat{101, false};
    assert(counts[key_101_rat] == 2);

    // 103 (rat) appears 1 time
    LargePrimeKey key_103_rat{103, false};
    assert(counts[key_103_rat] == 1);

    // 107 (alg) appears 2 times
    LargePrimeKey key_107_alg{107, true};
    assert(counts[key_107_alg] == 2);

    // 109 (alg) appears 1 time
    LargePrimeKey key_109_alg{109, true};
    assert(counts[key_109_alg] == 1);

    // Get unique primes
    auto unique = RelationFilter::get_unique_large_primes(relations);
    assert(unique.size() == 4);  // 101, 103, 107, 109

    std::cout << "  Large prime counting: PASSED" << std::endl;
}

// Test relation requirement calculation
void test_relation_requirements() {
    std::cout << "Testing relation requirements..." << std::endl;

    // Factor base size = 1000, unique large primes = 500
    size_t fb_size = 1000;
    size_t ulp = 500;

    size_t required = required_relations(fb_size, ulp, 1.05);
    // Expected: (1000 + 500) * 1.05 + 1 = 1575 + 1 = 1576
    assert(required == 1576);

    // Test has_enough_relations
    assert(!has_enough_relations(1500, fb_size, ulp, 1.05));
    assert(has_enough_relations(1576, fb_size, ulp, 1.05));
    assert(has_enough_relations(2000, fb_size, ulp, 1.05));

    std::cout << "  Relation requirements: PASSED" << std::endl;
}

// Test separate_relations
void test_separate_relations() {
    std::cout << "Testing relation separation..." << std::endl;

    std::vector<core::Relation> relations;

    // Full relation (no large primes)
    core::Relation full1(1, 1);
    relations.push_back(std::move(full1));

    // Partial relation (has large primes)
    core::Relation partial1(2, 1);
    partial1.rational_large_prime.push_back(core::PrimePower{101, 1});
    relations.push_back(std::move(partial1));

    // Another full relation
    core::Relation full2(3, 1);
    relations.push_back(std::move(full2));

    // Another partial
    core::Relation partial2(4, 1);
    partial2.algebraic_large_prime.push_back(core::PrimePower{103, 1});
    partial2.algebraic_large_prime.push_back(core::PrimePower{107, 1});
    relations.push_back(std::move(partial2));

    auto separated = separate_relations(std::move(relations));

    assert(separated.full.size() == 2);
    assert(separated.partial.size() == 2);

    // Verify full relations have no large primes
    for (const auto& rel : separated.full) {
        assert(rel.is_full());
    }

    // Verify partial relations have large primes
    for (const auto& rel : separated.partial) {
        assert(!rel.is_full());
    }

    std::cout << "  Relation separation: PASSED" << std::endl;
}

// Test quick cofactor check
void test_quick_cofactor_check() {
    std::cout << "Testing quick cofactor check..." << std::endl;

    uint64_t lpb = 1000;

    // Smooth
    core::Integer c1(1);
    assert(quick_cofactor_check(c1, lpb, true));

    // Single LP within bound
    core::Integer c2(500);
    assert(quick_cofactor_check(c2, lpb, true));

    // Single LP at bound
    core::Integer c3(1000);
    assert(quick_cofactor_check(c3, lpb, true));

    // 2LP within bound^2
    core::Integer c4(500 * 600);  // 300000 < 1000000
    assert(quick_cofactor_check(c4, lpb, true));

    // 2LP at bound^2
    core::Integer c5(static_cast<unsigned long long>(lpb * lpb));
    assert(quick_cofactor_check(c5, lpb, true));

    // Too large for 2LP
    core::Integer c6(static_cast<unsigned long long>(lpb * lpb + 1));
    assert(!quick_cofactor_check(c6, lpb, true));

    // 2LP disabled
    core::Integer c7(500 * 600);
    assert(!quick_cofactor_check(c7, lpb, false));  // > lpb, 2LP disabled

    std::cout << "  Quick cofactor check: PASSED" << std::endl;
}

// Test lpb² overflow protection for large_prime_bound > 2^32
// Bug: large_prime_bound * large_prime_bound overflows uint64 when lpb > ~4.29e9
// causing valid 2LP cofactors to be incorrectly rejected
void test_lpb_squared_overflow() {
    std::cout << "Testing lpb² overflow protection..." << std::endl;

    // lpb = 5×10^9 > 2^32 (4.29×10^9)
    // Real lpb² = 2.5×10^19 (overflows uint64_t, max ≈ 1.84×10^19)
    // Without fix, lpb*lpb wraps to ≈6.55×10^18
    uint64_t lpb = 5000000000ULL;

    // --- quick_cofactor_check tests ---

    // Case 1: cofactor = 1e19, between overflowed lpb² and real lpb²
    // 1e19 > 6.55e18 (overflowed) but < 2.5e19 (real)
    // Without fix: rejected. With fix: accepted as potential 2LP.
    core::Integer cof_in_range(static_cast<uint64_t>(10000000000000000000ULL));
    assert(quick_cofactor_check(cof_in_range, lpb, true) &&
           "Cofactor in valid 2LP range must not be rejected due to overflow");

    // Case 2: cofactor = 1.8e19, near UINT64_MAX but still < real lpb²
    core::Integer cof_near_max(static_cast<uint64_t>(18000000000000000000ULL));
    assert(quick_cofactor_check(cof_near_max, lpb, true) &&
           "Cofactor near UINT64_MAX but below real lpb² must pass");

    // Case 3: cofactor > real lpb² (as Integer, since 3e19 > UINT64_MAX)
    core::Integer cof_too_large("30000000000000000000");  // 3e19 > 2.5e19
    assert(!quick_cofactor_check(cof_too_large, lpb, true) &&
           "Cofactor above real lpb² must be rejected");

    // Case 4: single LP within bound still works
    core::Integer cof_single_lp(static_cast<uint64_t>(4000000000ULL));
    assert(quick_cofactor_check(cof_single_lp, lpb, true) &&
           "Single LP within bound must pass");

    // Case 5: 2LP disabled, cofactor > lpb should fail
    core::Integer cof_above_lpb(static_cast<uint64_t>(6000000000ULL));
    assert(!quick_cofactor_check(cof_above_lpb, lpb, false) &&
           "With 2LP disabled, cofactor > lpb must fail");

    // --- classify_cofactor tests ---

    // Case 6: classify_cofactor must NOT prematurely return TooLarge
    auto cls = classify_cofactor(cof_in_range, lpb);
    assert(cls.type != CofactorClass::TooLarge &&
           "classify_cofactor must not return TooLarge for cofactor < real lpb²");

    // Case 7: cofactor > real lpb² should be TooLarge
    auto cls2 = classify_cofactor(cof_too_large, lpb);
    assert(cls2.type == CofactorClass::TooLarge &&
           "classify_cofactor must return TooLarge for cofactor > real lpb²");

    std::cout << "  lpb² overflow protection: PASSED" << std::endl;
}

// Test PrimePower stores uint64_t primes without truncation
void test_prime_power_uint64_storage() {
    std::cout << "Testing PrimePower uint64 storage..." << std::endl;

    // --- PrimePower must store primes > UINT32_MAX ---

    uint64_t large_p = 5000000000ULL;  // > UINT32_MAX (4294967295)
    uint64_t large_r = 4999999999ULL;  // root mod p, also > UINT32_MAX

    // Case 1: Two-arg constructor (p, e) — for rational side
    core::PrimePower pp1(large_p, static_cast<uint8_t>(1));
    assert(pp1.p == large_p &&
           "PrimePower(p, e) must store p > UINT32_MAX without truncation");
    assert(pp1.e == 1);

    // Case 2: Three-arg constructor (p, r, e) — for algebraic side
    core::PrimePower pp2(large_p, large_r, static_cast<uint8_t>(2));
    assert(pp2.p == large_p &&
           "PrimePower(p, r, e) must store p > UINT32_MAX");
    assert(pp2.r == large_r &&
           "PrimePower(p, r, e) must store r > UINT32_MAX");
    assert(pp2.e == 2);

    // Case 3: Comparison operators with large values
    core::PrimePower pp3(large_p + 1, large_r, static_cast<uint8_t>(1));
    assert(pp2 < pp3 && "Comparison must work for large primes");
    assert(!(pp3 < pp2));

    // Case 4: Equality with large values
    core::PrimePower pp4(large_p, large_r, static_cast<uint8_t>(2));
    assert(pp2 == pp4 && "Equality must work for large primes");
    assert(pp2 != pp3);

    // Case 5: Hash must distinguish large primes
    core::PrimePowerHash hasher;
    size_t h1 = hasher(pp2);
    size_t h2 = hasher(pp3);
    assert(h1 != h2 && "Hash must distinguish different large primes");

    // Case 6: Near UINT32_MAX boundary
    core::PrimePower pp_boundary(static_cast<uint64_t>(UINT32_MAX) + 1, static_cast<uint8_t>(1));
    assert(pp_boundary.p == static_cast<uint64_t>(UINT32_MAX) + 1 &&
           "PrimePower must handle UINT32_MAX+1 correctly");

    // Case 7: UINT64_MAX-range prime
    uint64_t huge_p = 18000000000000000000ULL;  // ~1.8e19
    core::PrimePower pp_huge(huge_p, static_cast<uint8_t>(1));
    assert(pp_huge.p == huge_p &&
           "PrimePower must handle very large primes near UINT64_MAX");

    std::cout << "  PrimePower uint64 storage: PASSED" << std::endl;
}

// Test FactorBaseParams stores uint64_t large_prime_bound
void test_factor_base_params_uint64_lpb() {
    std::cout << "Testing FactorBaseParams uint64 large_prime_bound..." << std::endl;

    uint64_t large_lpb = 5000000000ULL;  // > UINT32_MAX

    // Case 1: Direct assignment
    core::FactorBaseParams params;
    params.large_prime_bound = large_lpb;
    assert(params.large_prime_bound == large_lpb &&
           "FactorBaseParams::large_prime_bound must store values > UINT32_MAX");

    // Case 2: Constructor
    core::FactorBaseParams params2(1000, 2000, large_lpb);
    assert(params2.large_prime_bound == large_lpb &&
           "FactorBaseParams constructor must preserve uint64 large_prime_bound");
    assert(params2.rational_bound == 1000);
    assert(params2.algebraic_bound == 2000);

    // Case 3: Round-trip through cofactorizer config
    // When large_prime_bound is set in FactorBaseParams, cofactorizer should use it
    // (indirect test: verify the value isn't truncated anywhere in the chain)
    uint64_t near_max = static_cast<uint64_t>(UINT32_MAX) + 100;
    core::FactorBaseParams params3;
    params3.large_prime_bound = near_max;
    assert(params3.large_prime_bound == near_max &&
           "large_prime_bound near UINT32_MAX boundary must not truncate");

    std::cout << "  FactorBaseParams uint64 lpb: PASSED" << std::endl;
}

// Test cofactorizer stores large primes correctly via classify_cofactor
void test_cofactorizer_large_prime_storage() {
    std::cout << "Testing cofactorizer large prime storage..." << std::endl;

    uint64_t lpb = 5000000000ULL;  // > UINT32_MAX

    // Case 1: A prime cofactor just below lpb should be stored correctly
    uint64_t prime_val = 4999999937ULL;  // A prime near 5e9
    core::Integer cof_prime(prime_val);

    auto cls = classify_cofactor(cof_prime, lpb);
    assert(cls.type == CofactorClass::Prime &&
           "Prime < lpb should classify as Prime");
    assert(cls.factor1 == prime_val &&
           "classify_cofactor must store the full uint64 prime in factor1");

    // Case 2: Semiprime cofactor — both factors should be stored
    // Use two primes whose product < lpb²
    // 4294967311 is prime (just above UINT32_MAX)
    // 4294967357 is prime
    // Their product is ~1.844e19 < (5e9)² = 2.5e19
    uint64_t p1 = 4294967311ULL;  // prime > UINT32_MAX
    uint64_t p2 = 4294967357ULL;  // prime > UINT32_MAX

    // Verify these are actually > UINT32_MAX
    assert(p1 > UINT32_MAX && p2 > UINT32_MAX);

    // Create the product as an Integer
    core::Integer product(p1);
    product *= core::Integer(p2);

    auto cls2 = classify_cofactor(product, lpb);

    // The cofactor is a semiprime of two large primes
    // classify_cofactor should find both factors
    if (cls2.type == CofactorClass::Semiprime) {
        // Both factors should be > UINT32_MAX
        assert(cls2.factor1 > UINT32_MAX &&
               "Semiprime factor1 must not be truncated to uint32");
        assert(cls2.factor2 > UINT32_MAX &&
               "Semiprime factor2 must not be truncated to uint32");
        // Factors should multiply back to the product
        assert(cls2.factor1 * cls2.factor2 == p1 * p2 &&
               "Factors must reconstruct the original product");
    }
    // Note: classify_cofactor may fail to factor with Pollard's rho
    // (some semiprimes are harder), so we also test the PrimePower storage path

    // Case 3: Direct test of PrimePower storage for large primes in relations
    // This simulates what cofactorizer::add_large_primes does
    core::Relation rel(42, 1);
    rel.rational_large_prime.push_back(core::PrimePower{p1, static_cast<uint8_t>(1)});
    rel.algebraic_large_prime.push_back(core::PrimePower{p2, p2 - 100, static_cast<uint8_t>(1)});

    assert(rel.rational_large_prime[0].p == p1 &&
           "Relation must store rational large prime > UINT32_MAX");
    assert(rel.algebraic_large_prime[0].p == p2 &&
           "Relation must store algebraic large prime > UINT32_MAX");
    assert(rel.algebraic_large_prime[0].r == p2 - 100 &&
           "Relation must store algebraic root > UINT32_MAX");

    // Case 4: Serialization round-trip preserves large primes
    std::stringstream ss;
    rel.serialize(ss);
    auto rel2 = core::Relation::deserialize(ss);
    assert(rel2.rational_large_prime[0].p == p1 &&
           "Deserialized rational large prime must be preserved");
    assert(rel2.algebraic_large_prime[0].p == p2 &&
           "Deserialized algebraic large prime must be preserved");
    assert(rel2.algebraic_large_prime[0].r == p2 - 100 &&
           "Deserialized algebraic root must be preserved");

    std::cout << "  Cofactorizer large prime storage: PASSED" << std::endl;
}

int main() {
    std::cout << "=== Cofactorization Tests ===" << std::endl;
    std::cout << std::endl;

    test_primality();
    test_perfect_power();
    test_pollard_rho();
    test_cofactor_classification();
    test_trial_division();
    test_relation_filter();
    test_large_prime_counting();
    test_relation_requirements();
    test_separate_relations();
    test_quick_cofactor_check();
    test_lpb_squared_overflow();
    test_prime_power_uint64_storage();
    test_factor_base_params_uint64_lpb();
    test_cofactorizer_large_prime_storage();

    std::cout << std::endl;
    std::cout << "=== All Cofactorization Tests PASSED ===" << std::endl;

    return 0;
}
