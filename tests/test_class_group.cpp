// Unit tests for ClassGroup — discriminant, Minkowski bound, generators, characters
// Tests cover degree 3, 4, 5, 6 polynomials with known reference values.
#include "gnfs/sqrt/class_group.hpp"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace gnfs::sqrt;
using namespace gnfs::core;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; \
        tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_PASS(name) do { \
    std::cout << "  PASS: " << name << "\n"; \
    tests_passed++; \
} while(0)

// ─── helpers ───────────────────────────────────────────────

static Integer I(long long v) { return Integer(static_cast<int64_t>(v)); }

// ─── Degree 3 contexts ─────────────────────────────────────

// f(x) = x^3 + x + 1; f(5)=131, m=5, n=131
// disc = -31, signature (1,1), MB ≈ 1.57
static PolynomialContext make_cubic_131() {
    std::vector<Integer> c = {I(1), I(1), I(0), I(1)};
    return PolynomialContext(I(131), std::move(c), I(5));
}

// f(x) = x^3 + 5x + 1; f(2)=19, m=2, n=19
// disc = -527, signature (1,1), MB ≈ 6.49
static PolynomialContext make_cubic_19() {
    std::vector<Integer> c = {I(1), I(5), I(0), I(1)};
    return PolynomialContext(I(19), std::move(c), I(2));
}

// f(x) = x^3 + 2x + 1; f(1)=4, m=1, n=4
// disc = -59, signature (1,1)
static PolynomialContext make_cubic_4() {
    std::vector<Integer> c = {I(1), I(2), I(0), I(1)};
    return PolynomialContext(I(4), std::move(c), I(1));
}

// ─── Degree 4 contexts ─────────────────────────────────────

// f(x) = x^4 + 1; f(1)=2, m=1, n=2
// disc = 256, signature (0,2) — no real roots (x^4=-1 impossible)
// LMFDB 4.0.256.1 (8th cyclotomic field)
static PolynomialContext make_quartic_cyclotomic() {
    std::vector<Integer> c = {I(1), I(0), I(0), I(0), I(1)};
    return PolynomialContext(I(2), std::move(c), I(1));
}

// f(x) = x^4 - 5x^2 + 6 = (x^2-2)(x^2-3); f(1)=2, m=1, n=2
// disc = 96, signature (4,0) — four real roots: ±√2, ±√3
static PolynomialContext make_quartic_4real() {
    std::vector<Integer> c = {I(6), I(0), I(-5), I(0), I(1)};
    return PolynomialContext(I(2), std::move(c), I(1));
}

// f(x) = x^4 + x + 1; f(1)=3, m=1, n=3
// disc = 229, signature (0,2) — no real roots
// LMFDB 4.0.229.1
static PolynomialContext make_quartic_229() {
    std::vector<Integer> c = {I(1), I(1), I(0), I(0), I(1)};
    return PolynomialContext(I(3), std::move(c), I(1));
}

// f(x) = x^4 - x - 1; f(1)=-1, f(2)=13, m=2, n=13
// disc = -283, signature (2,1) — two real roots
// LMFDB 4.2.283.1
static PolynomialContext make_quartic_283() {
    std::vector<Integer> c = {I(-1), I(-1), I(0), I(0), I(1)};
    return PolynomialContext(I(13), std::move(c), I(2));
}

// ─── Degree 5 context ──────────────────────────────────────

// f(x) = x^5 + x + 1 = (x^2+x+1)(x^3-x^2+1); f(1)=3, m=1, n=3
// disc = 3381, signature (1,2) — one real root (from x^3-x^2+1)
static PolynomialContext make_quintic_3381() {
    std::vector<Integer> c = {I(1), I(1), I(0), I(0), I(0), I(1)};
    return PolynomialContext(I(3), std::move(c), I(1));
}

// ─── Degree 6 context ──────────────────────────────────────

// f(x) = x^6 + x + 1; f(1)=3, m=1, n=3
// disc = -43531, signature (0,3) — no real roots
// LMFDB 6.0.43531.1
static PolynomialContext make_sextic_43531() {
    std::vector<Integer> c = {I(1), I(1), I(0), I(0), I(0), I(0), I(1)};
    return PolynomialContext(I(3), std::move(c), I(1));
}

// ═══════════════════════════════════════════════════════════
// Tests: PrimeIdeal / IdealClass structs
// ═══════════════════════════════════════════════════════════

void test_prime_ideal_equality() {
    PrimeIdeal p1{5, 3, 1}, p2{5, 3, 1}, p3{5, 4, 1};
    TEST_ASSERT(p1 == p2, "equal ideals");
    TEST_ASSERT(!(p1 == p3), "unequal ideals");
    TEST_PASS("PrimeIdeal equality");
}

void test_prime_ideal_ordering() {
    PrimeIdeal a{2, 0, 1}, b{5, 0, 1}, c{5, 3, 1};
    TEST_ASSERT(a < b, "p=2 < p=5");
    TEST_ASSERT(!(b < a), "not reverse");
    TEST_ASSERT(b < c, "same p, r=0 < r=3");
    TEST_PASS("PrimeIdeal ordering");
}

void test_ideal_class_basics() {
    IdealClass cls;
    TEST_ASSERT(cls.is_principal(), "empty is principal");
    PrimeIdeal pi{3, 1, 1};
    cls.add_prime(pi, 1);
    TEST_ASSERT(!cls.is_principal(), "non-zero exp not principal");
    cls.add_prime(pi, -1);
    TEST_ASSERT(cls.is_principal(), "cancels to principal");
    TEST_PASS("IdealClass basics");
}

void test_ideal_class_reduce_mod() {
    IdealClass cls;
    PrimeIdeal pi{3, 1, 1};
    cls.add_prime(pi, 5);
    cls.reduce_mod(3);
    TEST_ASSERT(cls.prime_powers[pi] == 2, "5 mod 3 = 2");

    IdealClass cls2;
    cls2.add_prime(pi, 3);
    cls2.reduce_mod(3);
    TEST_ASSERT(cls2.is_principal(), "3 mod 3 = 0 → principal");
    TEST_PASS("IdealClass reduce_mod");
}

// ═══════════════════════════════════════════════════════════
// Tests: count_real_roots (Sturm's theorem)
// ═══════════════════════════════════════════════════════════

void test_sturm_linear() {
    // x + 1: one real root
    std::vector<Integer> c = {I(1), I(1)};
    TEST_ASSERT(ClassGroup::count_real_roots(c, 1) == 1, "linear has 1 root");
    TEST_PASS("Sturm: linear");
}

void test_sturm_quadratic() {
    // x^2 + 1: no real roots (disc = -4)
    std::vector<Integer> c1 = {I(1), I(0), I(1)};
    TEST_ASSERT(ClassGroup::count_real_roots(c1, 2) == 0, "x^2+1 has 0 roots");

    // x^2 - 1: two real roots (disc = 4)
    std::vector<Integer> c2 = {I(-1), I(0), I(1)};
    TEST_ASSERT(ClassGroup::count_real_roots(c2, 2) == 2, "x^2-1 has 2 roots");

    // x^2 + x + 1: no real roots (disc = -3)
    std::vector<Integer> c3 = {I(1), I(1), I(1)};
    TEST_ASSERT(ClassGroup::count_real_roots(c3, 2) == 0, "x^2+x+1 has 0 roots");
    TEST_PASS("Sturm: quadratic");
}

void test_sturm_cubic() {
    // x^3 + x + 1: disc = -31 → r1=1
    std::vector<Integer> c1 = {I(1), I(1), I(0), I(1)};
    TEST_ASSERT(ClassGroup::count_real_roots(c1, 3) == 1, "x^3+x+1 has 1 root");

    // x^3 - 3x + 1: disc = 81 > 0 → r1=3
    // Roots: 2cos(π/9), 2cos(5π/9), 2cos(7π/9) — all real
    std::vector<Integer> c2 = {I(1), I(-3), I(0), I(1)};
    TEST_ASSERT(ClassGroup::count_real_roots(c2, 3) == 3, "x^3-3x+1 has 3 roots");
    TEST_PASS("Sturm: cubic");
}

void test_sturm_quartic() {
    // x^4 + 1: r1=0 (LMFDB)
    std::vector<Integer> c1 = {I(1), I(0), I(0), I(0), I(1)};
    TEST_ASSERT(ClassGroup::count_real_roots(c1, 4) == 0, "x^4+1 r1=0");

    // x^4 - 5x^2 + 6: r1=4 (roots: ±√2, ±√3)
    std::vector<Integer> c2 = {I(6), I(0), I(-5), I(0), I(1)};
    TEST_ASSERT(ClassGroup::count_real_roots(c2, 4) == 4, "x^4-5x^2+6 r1=4");

    // x^4 + x + 1: r1=0 (LMFDB 4.0.229.1)
    std::vector<Integer> c3 = {I(1), I(1), I(0), I(0), I(1)};
    TEST_ASSERT(ClassGroup::count_real_roots(c3, 4) == 0, "x^4+x+1 r1=0");

    // x^4 - x - 1: r1=2 (LMFDB 4.2.283.1)
    std::vector<Integer> c4 = {I(-1), I(-1), I(0), I(0), I(1)};
    TEST_ASSERT(ClassGroup::count_real_roots(c4, 4) == 2, "x^4-x-1 r1=2");
    TEST_PASS("Sturm: quartic");
}

void test_sturm_quintic() {
    // x^5 + x + 1: r1=1 (one real root from x^3-x^2+1 factor)
    std::vector<Integer> c = {I(1), I(1), I(0), I(0), I(0), I(1)};
    TEST_ASSERT(ClassGroup::count_real_roots(c, 5) == 1, "x^5+x+1 r1=1");
    TEST_PASS("Sturm: quintic");
}

void test_sturm_sextic() {
    // x^6 + x + 1: r1=0 (LMFDB 6.0.43531.1)
    std::vector<Integer> c = {I(1), I(1), I(0), I(0), I(0), I(0), I(1)};
    TEST_ASSERT(ClassGroup::count_real_roots(c, 6) == 0, "x^6+x+1 r1=0");
    TEST_PASS("Sturm: sextic");
}

void test_sturm_repeated_roots() {
    // (x-1)^2 = x^2 - 2x + 1: one distinct real root (multiplicity 2)
    // Sturm counts distinct roots, not multiplicities
    std::vector<Integer> c1 = {I(1), I(-2), I(1)};
    TEST_ASSERT(ClassGroup::count_real_roots(c1, 2) == 1, "(x-1)^2 r1=1");

    // (x-1)^2 * (x+1) = x^3 - x^2 - x + 1: two distinct real roots
    std::vector<Integer> c2 = {I(1), I(-1), I(-1), I(1)};
    TEST_ASSERT(ClassGroup::count_real_roots(c2, 3) == 2, "(x-1)^2(x+1) r1=2");

    // x^2: one root at 0 (multiplicity 2)
    std::vector<Integer> c3 = {I(0), I(0), I(1)};
    TEST_ASSERT(ClassGroup::count_real_roots(c3, 2) == 1, "x^2 r1=1");
    TEST_PASS("Sturm: repeated roots");
}

void test_sturm_all_real_quintic() {
    // x^5 - 5x^3 + 4x = x(x^2-1)(x^2-4) = x(x-1)(x+1)(x-2)(x+2)
    // 5 real roots: -2, -1, 0, 1, 2
    std::vector<Integer> c = {I(0), I(4), I(0), I(-5), I(0), I(1)};
    TEST_ASSERT(ClassGroup::count_real_roots(c, 5) == 5, "x^5-5x^3+4x r1=5");
    TEST_PASS("Sturm: all-real quintic");
}

// ═══════════════════════════════════════════════════════════
// Tests: Discriminant computation (Sylvester + Bareiss)
// ═══════════════════════════════════════════════════════════

void test_discriminant_degree3() {
    // x^3 + x + 1: disc = -31
    auto ctx1 = make_cubic_131();
    ClassGroup cg1(ctx1);
    TEST_ASSERT(cg1.discriminant() == I(-31), "disc(x^3+x+1) = -31");

    // x^3 + 2x + 1: disc = -59
    auto ctx2 = make_cubic_4();
    ClassGroup cg2(ctx2);
    TEST_ASSERT(cg2.discriminant() == I(-59), "disc(x^3+2x+1) = -59");

    // x^3 + 5x + 1: disc = -527
    auto ctx3 = make_cubic_19();
    ClassGroup cg3(ctx3);
    TEST_ASSERT(cg3.discriminant() == I(-527), "disc(x^3+5x+1) = -527");
    TEST_PASS("discriminant degree 3");
}

void test_discriminant_degree4() {
    // x^4 + 1: disc = 256
    auto ctx1 = make_quartic_cyclotomic();
    ClassGroup cg1(ctx1);
    TEST_ASSERT(cg1.discriminant() == I(256), "disc(x^4+1) = 256");

    // x^4 - 5x^2 + 6: disc = 96
    auto ctx2 = make_quartic_4real();
    ClassGroup cg2(ctx2);
    TEST_ASSERT(cg2.discriminant() == I(96), "disc(x^4-5x^2+6) = 96");

    // x^4 + x + 1: disc = 229
    auto ctx3 = make_quartic_229();
    ClassGroup cg3(ctx3);
    TEST_ASSERT(cg3.discriminant() == I(229), "disc(x^4+x+1) = 229");

    // x^4 - x - 1: disc = -283
    auto ctx4 = make_quartic_283();
    ClassGroup cg4(ctx4);
    TEST_ASSERT(cg4.discriminant() == I(-283), "disc(x^4-x-1) = -283");
    TEST_PASS("discriminant degree 4");
}

void test_discriminant_degree5() {
    // x^5 + x + 1: disc = 3381
    auto ctx = make_quintic_3381();
    ClassGroup cg(ctx);
    TEST_ASSERT(cg.discriminant() == I(3381), "disc(x^5+x+1) = 3381");
    TEST_PASS("discriminant degree 5");
}

void test_discriminant_degree6() {
    // x^6 + x + 1: disc = -43531
    auto ctx = make_sextic_43531();
    ClassGroup cg(ctx);
    TEST_ASSERT(cg.discriminant() == I(-43531), "disc(x^6+x+1) = -43531");
    TEST_PASS("discriminant degree 6");
}

// ═══════════════════════════════════════════════════════════
// Tests: Minkowski bound uses correct signature
// ═══════════════════════════════════════════════════════════

void test_minkowski_bound_degree3() {
    // x^3 + x + 1: disc=-31, (r1,r2)=(1,1)
    // MB = (3!/3^3) * (4/π)^1 * sqrt(31) = (6/27) * 1.2732 * 5.568 = 0.2222 * 7.093 = 1.576
    auto ctx = make_cubic_131();
    ClassGroup cg(ctx);
    double mb = cg.minkowski_bound();
    TEST_ASSERT(mb > 1.5 && mb < 1.7, "MB(x^3+x+1) ≈ 1.58");
    TEST_PASS("Minkowski bound degree 3");
}

void test_minkowski_bound_degree4_0_2() {
    // x^4 + 1: disc=256, (r1,r2)=(0,2)
    // MB = (4!/4^4) * (4/π)^2 * sqrt(256)
    //    = (24/256) * (1.2732)^2 * 16
    //    = 0.09375 * 1.621 * 16 = 2.431
    auto ctx = make_quartic_cyclotomic();
    ClassGroup cg(ctx);
    double mb = cg.minkowski_bound();
    // With r2=2 (correct): MB ≈ 2.43
    // With r2=0 (old bug): MB = (24/256) * 1 * 16 = 1.50
    TEST_ASSERT(mb > 2.0 && mb < 3.0, "MB(x^4+1) ≈ 2.43 (needs r2=2)");
    TEST_PASS("Minkowski bound degree 4 (0,2)");
}

void test_minkowski_bound_degree4_4_0() {
    // x^4 - 5x^2 + 6: disc=96, (r1,r2)=(4,0)
    // MB = (4!/4^4) * (4/π)^0 * sqrt(96)
    //    = 0.09375 * 1 * 9.798 = 0.919
    auto ctx = make_quartic_4real();
    ClassGroup cg(ctx);
    double mb = cg.minkowski_bound();
    TEST_ASSERT(mb > 0.8 && mb < 1.1, "MB(x^4-5x^2+6) ≈ 0.92 (r2=0)");
    TEST_PASS("Minkowski bound degree 4 (4,0)");
}

void test_minkowski_bound_degree4_2_1() {
    // x^4 - x - 1: disc=-283, (r1,r2)=(2,1)
    // MB = (24/256) * (4/π)^1 * sqrt(283)
    //    = 0.09375 * 1.2732 * 16.823 = 2.008
    auto ctx = make_quartic_283();
    ClassGroup cg(ctx);
    double mb = cg.minkowski_bound();
    TEST_ASSERT(mb > 1.8 && mb < 2.3, "MB(x^4-x-1) ≈ 2.01 (r2=1)");
    TEST_PASS("Minkowski bound degree 4 (2,1)");
}

void test_minkowski_bound_degree5() {
    // x^5 + x + 1: disc=3381, (r1,r2)=(1,2)
    // MB = (5!/5^5) * (4/π)^2 * sqrt(3381)
    //    = (120/3125) * 1.621 * 58.15 = 0.0384 * 94.27 = 3.62
    auto ctx = make_quintic_3381();
    ClassGroup cg(ctx);
    double mb = cg.minkowski_bound();
    TEST_ASSERT(mb > 3.0 && mb < 4.5, "MB(x^5+x+1) ≈ 3.62");
    TEST_PASS("Minkowski bound degree 5");
}

void test_minkowski_bound_degree6() {
    // x^6 + x + 1: disc=-43531, (r1,r2)=(0,3)
    // MB = (6!/6^6) * (4/π)^3 * sqrt(43531)
    //    = (720/46656) * 2.064 * 208.6 = 0.01543 * 430.5 = 6.64
    auto ctx = make_sextic_43531();
    ClassGroup cg(ctx);
    double mb = cg.minkowski_bound();
    TEST_ASSERT(mb > 5.5 && mb < 8.0, "MB(x^6+x+1) ≈ 6.64");
    TEST_PASS("Minkowski bound degree 6");
}

// ═══════════════════════════════════════════════════════════
// Tests: ClassGroup construction for higher degrees
// ═══════════════════════════════════════════════════════════

void test_class_group_degree4_no_crash() {
    auto ctx1 = make_quartic_cyclotomic();
    ClassGroup cg1(ctx1);
    TEST_ASSERT(cg1.class_number() >= 1, "quartic cyclotomic class number >= 1");

    auto ctx2 = make_quartic_4real();
    ClassGroup cg2(ctx2);
    TEST_ASSERT(cg2.class_number() >= 1, "quartic 4-real class number >= 1");

    auto ctx3 = make_quartic_283();
    ClassGroup cg3(ctx3);
    TEST_ASSERT(cg3.class_number() >= 1, "quartic 283 class number >= 1");
    TEST_PASS("ClassGroup degree 4 no crash");
}

void test_class_group_degree5_6_no_crash() {
    auto ctx1 = make_quintic_3381();
    ClassGroup cg1(ctx1);
    TEST_ASSERT(cg1.class_number() >= 1, "quintic class number >= 1");

    auto ctx2 = make_sextic_43531();
    ClassGroup cg2(ctx2);
    TEST_ASSERT(cg2.class_number() >= 1, "sextic class number >= 1");
    TEST_PASS("ClassGroup degree 5/6 no crash");
}

void test_character_size_higher_degree() {
    auto ctx = make_quartic_cyclotomic();
    ClassGroup cg(ctx);
    auto ch = cg.compute_character(1, 1);
    TEST_ASSERT(ch.size() == cg.num_generators(), "character size = num_generators");
    TEST_PASS("character size for degree 4");
}

// ═══════════════════════════════════════════════════════════
// Tests: disc sign ↔ r2 parity consistency
// ═══════════════════════════════════════════════════════════

void test_disc_sign_r2_parity() {
    // Verify: disc > 0 ⟺ r2 even, disc < 0 ⟺ r2 odd
    struct Case {
        const char* name;
        std::vector<Integer> coeffs;
        uint32_t degree;
        int64_t expected_disc;
    };

    Case cases[] = {
        {"x^3+x+1", {I(1), I(1), I(0), I(1)}, 3, -31},
        {"x^4+1", {I(1), I(0), I(0), I(0), I(1)}, 4, 256},
        {"x^4-5x^2+6", {I(6), I(0), I(-5), I(0), I(1)}, 4, 96},
        {"x^4-x-1", {I(-1), I(-1), I(0), I(0), I(1)}, 4, -283},
        {"x^5+x+1", {I(1), I(1), I(0), I(0), I(0), I(1)}, 5, 3381},
        {"x^6+x+1", {I(1), I(1), I(0), I(0), I(0), I(0), I(1)}, 6, -43531},
    };

    for (const auto& tc : cases) {
        uint32_t r1 = ClassGroup::count_real_roots(tc.coeffs, tc.degree);
        uint32_t r2 = (tc.degree - r1) / 2;
        bool disc_positive = tc.expected_disc > 0;
        bool r2_even = (r2 % 2 == 0);
        TEST_ASSERT(disc_positive == r2_even,
            std::string(tc.name) + ": disc sign ↔ r2 parity mismatch");
    }
    TEST_PASS("disc sign ↔ r2 parity consistency");
}

// ═══════════════════════════════════════════════════════════
// Legacy regression tests (original cubic-only tests)
// ═══════════════════════════════════════════════════════════

void test_trivial_class_group_small_discriminant() {
    auto ctx = make_cubic_131();
    ClassGroup cg(ctx);
    TEST_ASSERT(cg.class_number() == 1, "class number = 1");
    TEST_ASSERT(cg.minkowski_bound() > 0.0, "MB > 0");
    TEST_ASSERT(cg.minkowski_bound() < 2.0, "MB < 2");
    TEST_ASSERT(cg.num_generators() == 0, "no generators");
    TEST_PASS("trivial class group (MB < 2)");
}

void test_nontrivial_class_group_runs() {
    auto ctx = make_cubic_19();
    ClassGroup cg(ctx);
    TEST_ASSERT(cg.class_number() >= 1, "class number >= 1");
    TEST_ASSERT(cg.minkowski_bound() > 2.0, "MB > 2");
    TEST_PASS("non-trivial class group runs");
}

void test_compute_character_trivial_all_false() {
    auto ctx = make_cubic_131();
    ClassGroup cg(ctx);
    TEST_ASSERT(cg.class_number() == 1, "trivial");
    for (int a : {-5, 0, 1, 3, 7}) {
        for (uint64_t b : {1ULL, 2ULL}) {
            auto ch = cg.compute_character(a, b);
            TEST_ASSERT(ch.empty(), "trivial → empty character");
        }
    }
    TEST_PASS("character trivial → all false");
}

void test_generators_are_valid_prime_ideals() {
    auto ctx = make_cubic_19();
    ClassGroup cg(ctx);
    for (const auto& gen : cg.generators()) {
        TEST_ASSERT(gen.p >= 2, "generator p >= 2");
        TEST_ASSERT(gen.degree == 1, "generator degree-1");
        TEST_ASSERT(gen.r < gen.p, "generator r < p");
    }
    TEST_PASS("generators are valid prime ideals");
}

// ═══════════════════════════════════════════════════════════

int main() {
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  ClassGroup Unit Tests (Any Degree)\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    // Struct tests
    test_prime_ideal_equality();
    test_prime_ideal_ordering();
    test_ideal_class_basics();
    test_ideal_class_reduce_mod();

    // Sturm's theorem — real root counting
    test_sturm_linear();
    test_sturm_quadratic();
    test_sturm_cubic();
    test_sturm_quartic();
    test_sturm_quintic();
    test_sturm_sextic();
    test_sturm_repeated_roots();
    test_sturm_all_real_quintic();

    // Discriminant computation
    test_discriminant_degree3();
    test_discriminant_degree4();
    test_discriminant_degree5();
    test_discriminant_degree6();

    // Minkowski bound (correct signature)
    test_minkowski_bound_degree3();
    test_minkowski_bound_degree4_0_2();
    test_minkowski_bound_degree4_4_0();
    test_minkowski_bound_degree4_2_1();
    test_minkowski_bound_degree5();
    test_minkowski_bound_degree6();

    // ClassGroup higher degree
    test_class_group_degree4_no_crash();
    test_class_group_degree5_6_no_crash();
    test_character_size_higher_degree();

    // Cross-validation: disc sign ↔ r2 parity
    test_disc_sign_r2_parity();

    // Legacy regressions
    test_trivial_class_group_small_discriminant();
    test_nontrivial_class_group_runs();
    test_compute_character_trivial_all_false();
    test_generators_are_valid_prime_ideals();

    std::cout << "\n═══════════════════════════════════════════\n";
    std::cout << "  Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
    std::cout << "═══════════════════════════════════════════\n";

    return tests_failed > 0 ? 1 : 0;
}
