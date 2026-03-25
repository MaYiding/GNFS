// Unit tests for ClassGroup — discriminant, Minkowski bound, generators, characters
#include "gnfs/sqrt/class_group.hpp"
#include <cassert>
#include <iostream>

using namespace gnfs::sqrt;
using namespace gnfs::core;

// ─── helpers ───────────────────────────────────────────────

static Integer I(long long v) { return Integer(static_cast<int64_t>(v)); }

// Build PolynomialContext for f(x) = x^3 + x + 1; f(5)=131, m=5, n=131
// Discriminant (depressed cubic): Δ = -4(1)^3 - 27(1)^2 = -31; MB ≈ 1.57 < 2 → trivial
static PolynomialContext make_cubic_131() {
    std::vector<Integer> c = {I(1), I(1), I(0), I(1)};
    return PolynomialContext(I(131), std::move(c), I(5));
}

// Build PolynomialContext for f(x) = x^3 + 5x + 1; f(2)=19, m=2, n=19
// Discriminant: Δ = -4(5)^3 - 27(1)^2 = -527; MB ≈ 6.49 → non-trivial computation runs
static PolynomialContext make_cubic_19() {
    std::vector<Integer> c = {I(1), I(5), I(0), I(1)};
    return PolynomialContext(I(19), std::move(c), I(2));
}

// Build PolynomialContext for f(x) = x^3 + 0*x + 0 → degenerate; using x^3 + x - 2, m=1, n=0.
// We need f(m) ≡ 0 mod n: use x^3 + 2x + 1; f(1)=4; m=1, n=4.
static PolynomialContext make_cubic_4() {
    // f(x) = x^3 + 2x + 1; coeff: [1, 2, 0, 1]
    std::vector<Integer> c = {I(1), I(2), I(0), I(1)};
    return PolynomialContext(I(4), std::move(c), I(1));
}

// ─── PrimeIdeal and IdealClass struct tests ───────────────────

void test_prime_ideal_equality() {
    std::cout << "Testing PrimeIdeal equality..." << std::endl;
    PrimeIdeal p1{5, 3, 1};
    PrimeIdeal p2{5, 3, 1};
    PrimeIdeal p3{5, 4, 1};
    assert(p1 == p2);
    assert(!(p1 == p3));
    std::cout << "  PASS" << std::endl;
}

void test_prime_ideal_ordering() {
    std::cout << "Testing PrimeIdeal operator<..." << std::endl;
    PrimeIdeal p_small{2, 0, 1};
    PrimeIdeal p_med  {5, 0, 1};
    PrimeIdeal p_med2 {5, 3, 1};
    assert(p_small < p_med);
    assert(!(p_med < p_small));
    assert(p_med < p_med2);   // same p, r differs
    std::cout << "  PASS" << std::endl;
}

void test_ideal_class_is_principal_empty() {
    std::cout << "Testing IdealClass.is_principal() when empty..." << std::endl;
    IdealClass cls;
    assert(cls.is_principal());
    std::cout << "  PASS" << std::endl;
}

void test_ideal_class_not_principal_with_entry() {
    std::cout << "Testing IdealClass.is_principal() with non-zero exponent..." << std::endl;
    IdealClass cls;
    PrimeIdeal pi{3, 1, 1};
    cls.add_prime(pi, 1);
    assert(!cls.is_principal());
    std::cout << "  PASS" << std::endl;
}

void test_ideal_class_reduce_mod() {
    std::cout << "Testing IdealClass.reduce_mod()..." << std::endl;
    IdealClass cls;
    PrimeIdeal pi{3, 1, 1};
    cls.add_prime(pi, 5);
    cls.reduce_mod(3);
    auto it = cls.prime_powers.find(pi);
    assert(it != cls.prime_powers.end());
    assert(it->second == 2);  // 5 mod 3 = 2
    std::cout << "  PASS" << std::endl;
}

void test_ideal_class_reduce_mod_zeros_removed() {
    std::cout << "Testing IdealClass.reduce_mod() removes zero entries..." << std::endl;
    IdealClass cls;
    PrimeIdeal pi{7, 2, 1};
    cls.add_prime(pi, 3);
    cls.reduce_mod(3);   // 3 mod 3 = 0 → entry removed
    assert(cls.is_principal());
    assert(cls.prime_powers.empty());
    std::cout << "  PASS" << std::endl;
}

// ─── helper for iterating multiple contexts ─────────────────────

template <typename F>
static void for_each_context(F fn) {
    auto ctx1 = make_cubic_131();
    fn(ctx1);
    auto ctx2 = make_cubic_19();
    fn(ctx2);
    auto ctx3 = make_cubic_4();
    fn(ctx3);
}

// ─── ClassGroup construction and invariants ─────────────────────

void test_class_number_at_least_one() {
    std::cout << "Testing class_number() >= 1..." << std::endl;
    auto ctx = make_cubic_131();
    ClassGroup cg(ctx);
    assert(cg.class_number() >= 1);
    std::cout << "  PASS" << std::endl;
}

void test_trivial_class_group_small_discriminant() {
    std::cout << "Testing trivial class group (MB < 2)..." << std::endl;
    // f(x) = x^3 + x + 1; Δ = -31; MB ≈ 1.57 < 2 → class_number = 1
    auto ctx = make_cubic_131();
    ClassGroup cg(ctx);
    assert(cg.class_number() == 1);
    // Minkowski bound should be positive but < 2
    assert(cg.minkowski_bound() > 0.0);
    assert(cg.minkowski_bound() < 2.0);
    // No generators for trivial class group
    assert(cg.num_generators() == 0);
    assert(cg.generators().empty());
    std::cout << "  PASS" << std::endl;
}

void test_discriminant_depressed_cubic() {
    std::cout << "Testing discriminant formula for depressed cubic f=x^3+x+1..." << std::endl;
    // Δ = -4a^3 - 27b^2 where f = x^3 + ax + b
    // a = coeff(1) = 1, b = coeff(0) = 1
    // Δ = -4 - 27 = -31
    auto ctx = make_cubic_131();
    ClassGroup cg(ctx);
    Integer expected(-31LL);
    assert(cg.discriminant() == expected);
    std::cout << "  PASS" << std::endl;
}

void test_discriminant_another_cubic() {
    std::cout << "Testing discriminant for f=x^3+2x+1 (Δ=-4*8-27=-59)..." << std::endl;
    auto ctx = make_cubic_4();
    ClassGroup cg(ctx);
    // a = coeff(1) = 2, b = coeff(0) = 1
    // Δ = -4(2)^3 - 27(1)^2 = -32 - 27 = -59
    Integer expected(-59LL);
    assert(cg.discriminant() == expected);
    std::cout << "  PASS" << std::endl;
}

void test_minkowski_bound_positive() {
    std::cout << "Testing minkowski_bound() > 0..." << std::endl;
    for_each_context([](PolynomialContext& ctx) {
        ClassGroup cg(ctx);
        assert(cg.minkowski_bound() >= 0.0);
    });
    std::cout << "  PASS" << std::endl;
}

void test_nontrivial_class_group_runs() {
    std::cout << "Testing non-trivial class group (MB >= 2) runs without crash..." << std::endl;
    // f(x) = x^3 + 5x + 1; Δ = -527; MB ≈ 6.49 > 2
    auto ctx = make_cubic_19();
    ClassGroup cg(ctx);
    // class_number must be >= 1
    assert(cg.class_number() >= 1);
    // Minkowski bound should be > 2
    assert(cg.minkowski_bound() > 2.0);
    std::cout << "  PASS" << std::endl;
}

void test_compute_character_returns_correct_size() {
    std::cout << "Testing compute_character() returns num_generators bits..." << std::endl;
    auto ctx = make_cubic_131();
    ClassGroup cg(ctx);
    // For trivial class group, character is empty
    auto char1 = cg.compute_character(1, 1);
    assert(char1.size() == cg.num_generators());

    auto ctx2 = make_cubic_19();
    ClassGroup cg2(ctx2);
    auto char2 = cg2.compute_character(3, 1);
    assert(char2.size() == cg2.num_generators());
    std::cout << "  PASS" << std::endl;
}

void test_compute_character_trivial_all_false() {
    std::cout << "Testing compute_character() trivial → all false..." << std::endl;
    auto ctx = make_cubic_131();
    ClassGroup cg(ctx);
    assert(cg.class_number() == 1);
    // Any (a,b) pair should return empty vector
    for (int a : {-5, 0, 1, 3, 7}) {
        for (uint64_t b : {1ULL, 2ULL}) {
            auto ch = cg.compute_character(a, b);
            assert(ch.empty());
        }
    }
    std::cout << "  PASS" << std::endl;
}

void test_generators_are_valid_prime_ideals() {
    std::cout << "Testing generators() are degree-1 prime ideals..." << std::endl;
    auto ctx = make_cubic_19();
    ClassGroup cg(ctx);
    for (const auto& gen : cg.generators()) {
        assert(gen.p >= 2);
        assert(gen.degree == 1);
        assert(gen.r < gen.p);
    }
    std::cout << "  PASS" << std::endl;
}

void test_config_verbose_does_not_crash() {
    std::cout << "Testing ClassGroup with verbose config does not crash..." << std::endl;
    ClassGroup::Config cfg;
    cfg.verbose = true;
    auto ctx = make_cubic_131();
    ClassGroup cg(ctx, cfg);
    assert(cg.class_number() >= 1);
    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== ClassGroup Unit Tests ===" << std::endl;

    test_prime_ideal_equality();
    test_prime_ideal_ordering();
    test_ideal_class_is_principal_empty();
    test_ideal_class_not_principal_with_entry();
    test_ideal_class_reduce_mod();
    test_ideal_class_reduce_mod_zeros_removed();

    test_class_number_at_least_one();
    test_trivial_class_group_small_discriminant();
    test_discriminant_depressed_cubic();
    test_discriminant_another_cubic();
    test_minkowski_bound_positive();
    test_nontrivial_class_group_runs();
    test_compute_character_returns_correct_size();
    test_compute_character_trivial_all_false();
    test_generators_are_valid_prime_ideals();
    test_config_verbose_does_not_crash();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
