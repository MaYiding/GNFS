// Unit tests for gnfs::core types — ABPair + PrimePower + their hashes +
// AlgebraicPrime/RationalPrime constructors.
//
// These types underlie the entire relation/sieve/filter pipeline. Bugs in
// comparison or hash distribution would cause silent corruption of
// std::set / std::unordered_map state. Previously only PrimePowerHash had
// any direct test (in test_cofactor); now all comparison + hash paths
// have isolated coverage.

#include "gnfs/core/types.hpp"
#include "support/test_assert.hpp"

#include <cstdint>
#include <iostream>
#include <set>
#include <unordered_set>

using gnfs::core::ABPair;
using gnfs::core::ABPairHash;
using gnfs::core::AlgebraicPrime;
using gnfs::core::FactorBaseParams;
using gnfs::core::PrimePower;
using gnfs::core::PrimePowerHash;
using gnfs::core::RationalPrime;
using gnfs::test::run_case;

void test_abpair_construction() {
    std::cout << "Testing ABPair construction..." << std::endl;

    // Default
    ABPair p0;
    TEST_ASSERT(p0.a == 0);
    TEST_ASSERT(p0.b == 0);

    // Two-arg
    ABPair p1(int64_t(-5), uint64_t(7));
    TEST_ASSERT(p1.a == -5);
    TEST_ASSERT(p1.b == 7);

    // INT64_MIN edge
    ABPair p2(INT64_MIN, uint64_t(1));
    TEST_ASSERT(p2.a == INT64_MIN);
    TEST_ASSERT(p2.b == 1);

    // Large b
    ABPair p3(int64_t(0), UINT64_MAX);
    TEST_ASSERT(p3.a == 0);
    TEST_ASSERT(p3.b == UINT64_MAX);

    std::cout << "  construction: PASS" << std::endl;
}

void test_abpair_equality() {
    std::cout << "Testing ABPair equality..." << std::endl;

    ABPair p1(5, 3), p2(5, 3), p3(5, 4), p4(6, 3), p5(-5, 3);

    TEST_ASSERT(p1 == p2);
    TEST_ASSERT(!(p1 != p2));

    TEST_ASSERT(p1 != p3);  // b differs
    TEST_ASSERT(p1 != p4);  // a differs
    TEST_ASSERT(p1 != p5);  // sign differs

    // Self-equality
    TEST_ASSERT(p1 == p1);

    std::cout << "  equality: PASS" << std::endl;
}

void test_abpair_ordering() {
    std::cout << "Testing ABPair ordering (b first, then a)..." << std::endl;

    ABPair p1(10, 1), p2(20, 1), p3(0, 2);

    // b: 1 < 2, so p1 < p3 and p2 < p3 regardless of a
    TEST_ASSERT(p1 < p3);
    TEST_ASSERT(p2 < p3);
    TEST_ASSERT(!(p3 < p1));

    // Within same b=1, compare by a: 10 < 20 → p1 < p2
    TEST_ASSERT(p1 < p2);
    TEST_ASSERT(!(p2 < p1));

    // Strict weak ordering: !(a < a)
    TEST_ASSERT(!(p1 < p1));

    // Transitive: p1 < p2 < p3 → p1 < p3
    TEST_ASSERT(p1 < p2 && p2 < p3);
    TEST_ASSERT(p1 < p3);

    // <= behaves consistently
    TEST_ASSERT(p1 <= p1);
    TEST_ASSERT(p1 <= p2);
    TEST_ASSERT(!(p2 <= p1));

    // > and >= are mirror
    TEST_ASSERT(p2 > p1);
    TEST_ASSERT(p1 >= p1);
    TEST_ASSERT(p2 >= p1);
    TEST_ASSERT(!(p1 >= p2));

    std::cout << "  ordering: PASS" << std::endl;
}

void test_abpair_negative_a() {
    std::cout << "Testing ABPair ordering with negative a..." << std::endl;

    // For same b: a = -5 < a = 5
    ABPair p_neg(-5, 1);
    ABPair p_pos(5, 1);
    TEST_ASSERT(p_neg < p_pos);
    TEST_ASSERT(!(p_pos < p_neg));

    // INT64_MIN extreme
    ABPair p_min(INT64_MIN, 1);
    TEST_ASSERT(p_min < p_neg);

    std::cout << "  negative a ordering: PASS" << std::endl;
}

void test_abpair_hash_distinct() {
    std::cout << "Testing ABPairHash distinguishes nearby pairs..." << std::endl;

    ABPairHash h;
    // Two pairs that differ only in one field should have distinct hashes.
    // FNV-1a with two multiplications guarantees this for any distinct input.
    TEST_ASSERT(h(ABPair(5, 3)) != h(ABPair(5, 4)));
    TEST_ASSERT(h(ABPair(5, 3)) != h(ABPair(6, 3)));
    TEST_ASSERT(h(ABPair(0, 0)) != h(ABPair(1, 0)));
    TEST_ASSERT(h(ABPair(0, 0)) != h(ABPair(0, 1)));

    // Same pair → same hash (determinism)
    TEST_ASSERT(h(ABPair(123, 456)) == h(ABPair(123, 456)));

    // Equal-and-opposite a:
    // a=5 and a=-5 with b=3. Different reinterpreted-as-uint64_t bit patterns
    // → distinct hashes.
    TEST_ASSERT(h(ABPair(5, 3)) != h(ABPair(-5, 3)));

    std::cout << "  hash distinct: PASS" << std::endl;
}

void test_abpair_hash_unordered_set() {
    std::cout << "Testing ABPair as unordered_set key..." << std::endl;

    std::unordered_set<ABPair, ABPairHash> set;
    for (int a = -10; a <= 10; ++a) {
        for (uint64_t b = 1; b <= 5; ++b) {
            set.emplace(int64_t(a), b);
        }
    }
    // 21 a values × 5 b values = 105 unique pairs
    TEST_ASSERT(set.size() == 105);

    // Duplicate insertion → still 105
    set.emplace(int64_t(0), uint64_t(3));
    TEST_ASSERT(set.size() == 105);

    // find existing
    auto it = set.find(ABPair(int64_t(-5), uint64_t(2)));
    TEST_ASSERT(it != set.end());

    // find non-existing
    auto it2 = set.find(ABPair(int64_t(100), uint64_t(2)));
    TEST_ASSERT(it2 == set.end());

    std::cout << "  unordered_set: PASS" << std::endl;
}

void test_abpair_set_ordering() {
    std::cout << "Testing ABPair as std::set key..." << std::endl;

    std::set<ABPair> set;
    set.emplace(int64_t(5), uint64_t(2));
    set.emplace(int64_t(-3), uint64_t(1));
    set.emplace(int64_t(10), uint64_t(2));
    set.emplace(int64_t(-5), uint64_t(1));

    // Iteration order: by b ascending, then a ascending
    // Expected: (-5, 1), (-3, 1), (5, 2), (10, 2)
    auto it = set.begin();
    TEST_ASSERT(it->a == -5 && it->b == 1);
    ++it;
    TEST_ASSERT(it->a == -3 && it->b == 1);
    ++it;
    TEST_ASSERT(it->a == 5 && it->b == 2);
    ++it;
    TEST_ASSERT(it->a == 10 && it->b == 2);
    ++it;
    TEST_ASSERT(it == set.end());

    std::cout << "  set ordering: PASS" << std::endl;
}

void test_primepower_construction() {
    std::cout << "Testing PrimePower construction..." << std::endl;

    PrimePower pp0;
    TEST_ASSERT(pp0.p == 0 && pp0.r == 0 && pp0.e == 0);

    PrimePower pp1(uint64_t(7), uint64_t(3), uint8_t(2));
    TEST_ASSERT(pp1.p == 7 && pp1.r == 3 && pp1.e == 2);

    // Rational-only (no r), 2-arg
    PrimePower pp2(uint64_t(11), uint8_t(1));
    TEST_ASSERT(pp2.p == 11);
    TEST_ASSERT(pp2.r == 0);
    TEST_ASSERT(pp2.e == 1);

    std::cout << "  construction: PASS" << std::endl;
}

void test_primepower_equality_and_ordering() {
    std::cout << "Testing PrimePower equality + ordering..." << std::endl;

    PrimePower a(7, 3, 2), b(7, 3, 2), c(7, 3, 1), d(7, 4, 2), e(11, 3, 2);

    TEST_ASSERT(a == b);
    TEST_ASSERT(!(a == c));  // e differs
    TEST_ASSERT(!(a == d));  // r differs
    TEST_ASSERT(!(a == e));  // p differs
    TEST_ASSERT(a != c);

    // Ordering: by p first, then by r (e ignored in <)
    PrimePower x(7, 3, 5);  // same p, r; e=5 ≠ 2
    PrimePower y(7, 4, 1);  // same p; r=4 > 3
    PrimePower z(11, 3, 2); // p=11 > 7

    // 7,3 < 7,4 < 11,3
    TEST_ASSERT(a < y);
    TEST_ASSERT(y < z);
    TEST_ASSERT(a < z);

    // a and x have same (p, r): neither <, neither >
    TEST_ASSERT(!(a < x));
    TEST_ASSERT(!(x < a));

    std::cout << "  equality + ordering: PASS" << std::endl;
}

void test_primepower_hash_unordered() {
    std::cout << "Testing PrimePowerHash as unordered_set key..." << std::endl;

    PrimePowerHash hasher;
    // Distinct keys should hash distinct (or at least useful — std::hash
    // collisions are possible but FNV-1a + double-mul makes them rare).
    TEST_ASSERT(hasher(PrimePower(7, 3, 2)) != hasher(PrimePower(7, 4, 2)));
    TEST_ASSERT(hasher(PrimePower(7, 3, 2)) != hasher(PrimePower(11, 3, 2)));

    // Determinism
    TEST_ASSERT(hasher(PrimePower(7, 3, 2)) == hasher(PrimePower(7, 3, 2)));

    std::unordered_set<PrimePower, PrimePowerHash> set;
    for (uint64_t p : {2u, 3u, 5u, 7u, 11u, 13u}) {
        for (uint64_t r = 0; r < p; ++r) {
            set.emplace(p, r, uint8_t(1));
        }
    }
    // Number of (p, r) pairs = 2 + 3 + 5 + 7 + 11 + 13 = 41
    TEST_ASSERT(set.size() == 41);

    std::cout << "  hash + unordered_set: PASS" << std::endl;
}

void test_algebraic_prime_projective() {
    std::cout << "Testing AlgebraicPrime is_projective()..." << std::endl;

    AlgebraicPrime ap_default;
    TEST_ASSERT(ap_default.p == 0);
    TEST_ASSERT(ap_default.degree == 1);

    AlgebraicPrime ap_normal(uint32_t(7), uint32_t(3), uint32_t(10));
    TEST_ASSERT(!ap_normal.is_projective());
    TEST_ASSERT(ap_normal.degree == 1);  // default

    // PROJECTIVE_ROOT sentinel
    AlgebraicPrime ap_proj(uint32_t(7), AlgebraicPrime::PROJECTIVE_ROOT, uint32_t(10));
    TEST_ASSERT(ap_proj.is_projective());

    TEST_ASSERT(AlgebraicPrime::PROJECTIVE_ROOT == UINT32_MAX);

    // Explicit degree
    AlgebraicPrime ap_deg2(uint32_t(11), uint32_t(5), uint32_t(20), uint8_t(2));
    TEST_ASSERT(ap_deg2.degree == 2);

    std::cout << "  is_projective: PASS" << std::endl;
}

void test_rational_prime_basic() {
    std::cout << "Testing RationalPrime basic construction..." << std::endl;

    RationalPrime rp_default;
    TEST_ASSERT(rp_default.p == 0);
    TEST_ASSERT(rp_default.log_p == 0);

    RationalPrime rp(uint32_t(13), uint32_t(58));
    TEST_ASSERT(rp.p == 13);
    TEST_ASSERT(rp.log_p == 58);

    std::cout << "  basic: PASS" << std::endl;
}

void test_factor_base_params() {
    std::cout << "Testing FactorBaseParams..." << std::endl;

    FactorBaseParams fbp_default;
    TEST_ASSERT(fbp_default.rational_bound == 0);
    TEST_ASSERT(fbp_default.algebraic_bound == 0);
    TEST_ASSERT(fbp_default.large_prime_bound == 0);
    TEST_ASSERT(fbp_default.log_scale == gnfs::core::SIEVE_LOG_SCALE);

    FactorBaseParams fbp(uint32_t(1000), uint32_t(2000), uint64_t(50000));
    TEST_ASSERT(fbp.rational_bound == 1000);
    TEST_ASSERT(fbp.algebraic_bound == 2000);
    TEST_ASSERT(fbp.large_prime_bound == 50000);
    TEST_ASSERT(fbp.log_scale == gnfs::core::SIEVE_LOG_SCALE);

    // Custom log_scale
    FactorBaseParams fbp_custom(uint32_t(1), uint32_t(1), uint64_t(1), uint8_t(8));
    TEST_ASSERT(fbp_custom.log_scale == 8);

    std::cout << "  FactorBaseParams: PASS" << std::endl;
}

int main() {
    int failed = 0;

    std::cout << "=== core/types.hpp tests ===" << std::endl;

    failed += !run_case(test_abpair_construction);
    failed += !run_case(test_abpair_equality);
    failed += !run_case(test_abpair_ordering);
    failed += !run_case(test_abpair_negative_a);
    failed += !run_case(test_abpair_hash_distinct);
    failed += !run_case(test_abpair_hash_unordered_set);
    failed += !run_case(test_abpair_set_ordering);
    failed += !run_case(test_primepower_construction);
    failed += !run_case(test_primepower_equality_and_ordering);
    failed += !run_case(test_primepower_hash_unordered);
    failed += !run_case(test_algebraic_prime_projective);
    failed += !run_case(test_rational_prime_basic);
    failed += !run_case(test_factor_base_params);

    if (failed != 0) {
        std::cout << failed << " test case(s) failed" << std::endl;
        return 1;
    }

    std::cout << "\n=== All core/types.hpp tests PASSED ===" << std::endl;
    return 0;
}
