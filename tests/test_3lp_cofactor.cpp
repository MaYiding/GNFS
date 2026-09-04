// test_3lp_cofactor.cpp — Unit tests for 3LP cofactor classification + decomposition
//
// Covers:
//   - classify_cofactor(allow_3lp=true) returns CofactorClass::ThreeLP for cofactors
//     in (B^2, B^3] that decompose as p*q*r with all primes ≤ B.
//   - classify_cofactor(allow_3lp=false) returns CofactorClass::TooLarge for the
//     same cofactor (verifies opt-in semantics + zero regression).
//   - try_classify_three_lp() returns nullopt for cofactors that genuinely cannot
//     be split into three primes (e.g., cofactor > B^3 or contains a prime > B).
//   - quick_cofactor_check(allow_3lp=true) extends the acceptance window to B^3.
//   - CofactorClassification.factor1/2/3 are populated in sorted order.

#include <gnfs/cofactor/smooth_check.hpp>
#include <gnfs/core/integer.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

using gnfs::cofactor::classify_cofactor;
using gnfs::cofactor::CofactorClass;
using gnfs::cofactor::CofactorClassification;
using gnfs::cofactor::is_probable_prime_u64;
using gnfs::cofactor::quick_cofactor_check;
using gnfs::cofactor::try_classify_three_lp;
using gnfs::core::Integer;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_pass;                                                                              \
        } else {                                                                                   \
            ++g_fail;                                                                              \
            std::cerr << "FAIL: " << msg << " at " << __FILE__ << ":" << __LINE__ << std::endl;    \
        }                                                                                          \
    } while (0)

// Test 1: 3LP cofactor accepted under allow_3lp=true
void test_3lp_accepted() {
    std::cout << "test_3lp_accepted... ";
    // Pick three small primes well below 2^20 (B for lp_bits=20).
    // Use p=10007, q=10009, r=10037: all prime, all < 2^20.
    constexpr uint64_t p = 10007, q = 10009, r = 10037;
    assert(is_probable_prime_u64(p));
    assert(is_probable_prime_u64(q));
    assert(is_probable_prime_u64(r));
    constexpr uint64_t c = p * q * r;                 // ≈ 1.01e12, < 2^40
    [[maybe_unused]] constexpr uint64_t B = 1u << 20; // lp_bits=20, B = 2^20 ≈ 1.05e6
    // Sanity: c > B² (~1.1e12 > 1.1e12 — actually almost equal!). Let me pick smaller B.
    // Better: B = 2^14 = 16384 so c > B² = 2.7e8 and c < B³ = 4.4e12.
    constexpr uint64_t B2 = 1u << 14;
    static_assert(static_cast<unsigned long long>(p) * q * r >
                      static_cast<unsigned long long>(B2) * B2,
                  "c must be > B^2");
    static_assert(static_cast<unsigned long long>(p) * q * r <
                      static_cast<unsigned long long>(B2) * B2 * B2,
                  "c must be ≤ B^3");

    Integer c_int(static_cast<unsigned long long>(c));
    auto cls = classify_cofactor(c_int, B2, /*allow_3lp=*/true);
    CHECK(cls.type == CofactorClass::ThreeLP,
          "expected ThreeLP, got " << static_cast<int>(cls.type));
    if (cls.type == CofactorClass::ThreeLP) {
        // Factors should be sorted.
        CHECK(cls.factor1 <= cls.factor2 && cls.factor2 <= cls.factor3,
              "factors not sorted: " << cls.factor1 << " " << cls.factor2 << " " << cls.factor3);
        CHECK(cls.factor1 * cls.factor2 * cls.factor3 == c, "factor product != cofactor");
        CHECK(is_probable_prime_u64(cls.factor1), "factor1 not prime");
        CHECK(is_probable_prime_u64(cls.factor2), "factor2 not prime");
        CHECK(is_probable_prime_u64(cls.factor3), "factor3 not prime");
        CHECK(cls.factor1 <= B2 && cls.factor2 <= B2 && cls.factor3 <= B2, "some factor > B");
    }
    std::cout << "OK" << std::endl;
}

// Test 2: allow_3lp=false rejects same cofactor (zero-regression check)
void test_3lp_rejected_by_default() {
    std::cout << "test_3lp_rejected_by_default... ";
    constexpr uint64_t p = 10007, q = 10009, r = 10037;
    constexpr uint64_t c = p * q * r;
    constexpr uint64_t B2 = 1u << 14;

    Integer c_int(static_cast<unsigned long long>(c));
    auto cls = classify_cofactor(c_int, B2, /*allow_3lp=*/false);
    CHECK(cls.type == CofactorClass::TooLarge,
          "expected TooLarge when allow_3lp=false, got " << static_cast<int>(cls.type));
    std::cout << "OK" << std::endl;
}

// Test 3: cofactor with one factor > B is rejected
void test_3lp_rejected_factor_too_large() {
    std::cout << "test_3lp_rejected_factor_too_large... ";
    // p*q*r where r > B. We expect not-ThreeLP (Composite or TooLarge).
    constexpr uint64_t p = 10007, q = 10009;
    constexpr uint64_t r = 20011; // > 2^14
    constexpr uint64_t c = p * q * r;
    constexpr uint64_t B = 1u << 14; // 16384
    static_assert(r > B, "r should exceed B for this test");

    Integer c_int(static_cast<unsigned long long>(c));
    auto cls = classify_cofactor(c_int, B, /*allow_3lp=*/true);
    CHECK(cls.type != CofactorClass::ThreeLP, "should not accept when a factor exceeds B");
    std::cout << "OK (cls=" << static_cast<int>(cls.type) << ")" << std::endl;
}

// Test 4: cofactor > B³ should be TooLarge even with allow_3lp
void test_cofactor_above_b3() {
    std::cout << "test_cofactor_above_b3... ";
    constexpr uint64_t B = 1u << 14;
    // c > B³ → cannot possibly be 3LP. Pick (B+1)^3-ish.
    uint64_t big_factor = static_cast<uint64_t>(B) + 1;
    uint64_t c = big_factor * big_factor * big_factor;

    Integer c_int(c);
    auto cls = classify_cofactor(c_int, B, /*allow_3lp=*/true);
    CHECK(cls.type == CofactorClass::TooLarge,
          "expected TooLarge for c > B^3, got " << static_cast<int>(cls.type));
    std::cout << "OK" << std::endl;
}

// Test 5: quick_cofactor_check(allow_3lp=true) extends window to B^3
void test_quick_check_3lp_window() {
    std::cout << "test_quick_check_3lp_window... ";
    constexpr uint64_t B = 1u << 14;
    // Pick c in (B², B³]
    constexpr uint64_t p = 10007, q = 10009, r = 10037;
    constexpr uint64_t c = p * q * r;

    Integer c_int(static_cast<unsigned long long>(c));
    CHECK(!quick_cofactor_check(c_int, B, /*allow_2lp=*/true, /*allow_3lp=*/false),
          "should reject c > B^2 with allow_3lp=false");
    CHECK(quick_cofactor_check(c_int, B, /*allow_2lp=*/true, /*allow_3lp=*/true),
          "should accept c ≤ B^3 with allow_3lp=true");

    // c > B³ should still be rejected even with allow_3lp=true
    uint64_t big = (uint64_t(B) + 1);
    uint64_t c_above_b3 = big * big * big;
    Integer big_int(c_above_b3);
    CHECK(!quick_cofactor_check(big_int, B, /*allow_2lp=*/true, /*allow_3lp=*/true),
          "should reject c > B^3 even with allow_3lp=true");
    std::cout << "OK" << std::endl;
}

// Test 6: try_classify_three_lp returns the same result as classify_cofactor route
void test_direct_3lp_helper() {
    std::cout << "test_direct_3lp_helper... ";
    constexpr uint64_t p = 10007, q = 10009, r = 10037;
    constexpr uint64_t c = p * q * r;
    constexpr uint64_t B = 1u << 14;

    auto opt = try_classify_three_lp(c, B);
    CHECK(opt.has_value(), "try_classify_three_lp should succeed for p*q*r");
    if (opt) {
        CHECK(opt->type == CofactorClass::ThreeLP, "type should be ThreeLP");
        CHECK(opt->factor1 * opt->factor2 * opt->factor3 == c, "factor product mismatch");
    }
    std::cout << "OK" << std::endl;
}

// Test 7: existing 2LP cofactor still classifies as Semiprime when allow_3lp=true
void test_2lp_still_classified() {
    std::cout << "test_2lp_still_classified... ";
    constexpr uint64_t p = 10007, q = 10009;
    constexpr uint64_t c = p * q; // < B²
    constexpr uint64_t B = 1u << 14;
    static_assert(static_cast<unsigned long long>(c) < static_cast<unsigned long long>(B) * B,
                  "c must be ≤ B²");

    Integer c_int(static_cast<unsigned long long>(c));
    auto cls = classify_cofactor(c_int, B, /*allow_3lp=*/true);
    CHECK(cls.type == CofactorClass::Semiprime,
          "expected Semiprime, got " << static_cast<int>(cls.type));
    if (cls.type == CofactorClass::Semiprime) {
        CHECK(cls.factor1 * cls.factor2 == c, "factor product mismatch");
    }
    std::cout << "OK" << std::endl;
}

// Test 8: cofactor = 1 still Smooth
void test_smooth_unchanged() {
    std::cout << "test_smooth_unchanged... ";
    Integer one(1ULL);
    auto cls = classify_cofactor(one, 1u << 14, /*allow_3lp=*/true);
    CHECK(cls.type == CofactorClass::Smooth, "1 should be Smooth");
    std::cout << "OK" << std::endl;
}

// Test 9: single prime cofactor still Prime even with allow_3lp=true
void test_prime_unchanged() {
    std::cout << "test_prime_unchanged... ";
    constexpr uint64_t p = 10007;
    Integer p_int(static_cast<unsigned long long>(p));
    auto cls = classify_cofactor(p_int, 1u << 14, /*allow_3lp=*/true);
    CHECK(cls.type == CofactorClass::Prime, "prime should classify as Prime");
    CHECK(cls.factor1 == p, "factor1 should equal p");
    std::cout << "OK" << std::endl;
}

// Test 10: hard 60-bit triples remain stable across SQUFOF multiplier policies
void test_hard_3lp_corpus() {
    std::cout << "test_hard_3lp_corpus... ";
    constexpr uint64_t B = UINT64_C(1) << 21;
    constexpr std::array<std::array<uint64_t, 3>, 6> triples{{
        {{1000003, 1000033, 1000037}},
        {{1000039, 1000081, 1000099}},
        {{1000117, 1000121, 1000133}},
        {{1000151, 1000159, 1000171}},
        {{1000183, 1000187, 1000193}},
        {{1000199, 1000211, 1000213}},
    }};

    for (const auto& factors : triples) {
        const uint64_t p = factors[0];
        const uint64_t q = factors[1];
        const uint64_t r = factors[2];
        CHECK(is_probable_prime_u64(p) && is_probable_prime_u64(q) && is_probable_prime_u64(r),
              "hard 3LP corpus metadata must contain primes");
        const uint64_t c = p * q * r;
        CHECK(c > B * B && c <= B * B * B,
              "hard triple must stay inside the 3LP-only classification window");
        Integer c_int(static_cast<unsigned long long>(c));
        const auto cls = classify_cofactor(c_int, B, /*allow_3lp=*/true);
        CHECK(cls.type == CofactorClass::ThreeLP,
              "hard triple classified as " << static_cast<int>(cls.type));
        if (cls.type == CofactorClass::ThreeLP) {
            CHECK(cls.factor1 == p && cls.factor2 == q && cls.factor3 == r,
                  "hard triple factors changed: " << cls.factor1 << " " << cls.factor2 << " "
                                                  << cls.factor3);
        }
    }

    std::cout << "OK" << std::endl;
}

// Test 11: arbitrary-precision product whose three bounded factors exceed 64 bits.
void test_big_integer_3lp() {
    std::cout << "test_big_integer_3lp... ";
    constexpr uint64_t p = 3000017;
    constexpr uint64_t q = 3000073;
    constexpr uint64_t r = 3000103;
    constexpr uint64_t B = UINT64_C(1) << 22;
    CHECK(is_probable_prime_u64(p) && is_probable_prime_u64(q) && is_probable_prime_u64(r),
          "big 3LP metadata must contain primes");

    const Integer c = Integer(p) * Integer(q) * Integer(r);
    const Integer expected = c.clone();
    CHECK(!c.fits_uint64(), "big 3LP product should exceed uint64_t");
    CHECK(c > Integer(static_cast<unsigned long long>(B)) * Integer(B),
          "big 3LP product must be above B^2");
    CHECK(c <= Integer(static_cast<unsigned long long>(B)) * Integer(B) * Integer(B),
          "big 3LP product must be at or below B^3");

    const auto cls = classify_cofactor(c, B, /*allow_3lp=*/true);
    CHECK(cls.type == CofactorClass::ThreeLP,
          "arbitrary-precision 3LP classified as " << static_cast<int>(cls.type));
    if (cls.type == CofactorClass::ThreeLP) {
        CHECK(cls.factor1 == p && cls.factor2 == q && cls.factor3 == r,
              "arbitrary-precision factors changed: " << cls.factor1 << " " << cls.factor2
                                                       << " " << cls.factor3);
        const Integer reconstructed = Integer(cls.factor1) * Integer(cls.factor2) *
                                      Integer(cls.factor3);
        CHECK(reconstructed == expected, "arbitrary-precision factor product mismatch");
    }

    const auto direct = try_classify_three_lp(c, B);
    CHECK(direct.has_value() && direct->type == CofactorClass::ThreeLP,
          "direct arbitrary-precision helper should classify 3LP");

    const auto disabled = classify_cofactor(c, B, /*allow_3lp=*/false);
    CHECK(disabled.type == CofactorClass::TooLarge,
          "allow_3lp=false must retain TooLarge for big cofactor");
    std::cout << "OK" << std::endl;
}

// Test 12: a product in (B^2, B^3] with one factor above B is rejected.
void test_big_integer_factor_above_bound() {
    std::cout << "test_big_integer_factor_above_bound... ";
    constexpr uint64_t p = 3000017;
    constexpr uint64_t q = 4000037;
    constexpr uint64_t r = 5000011; // > B
    constexpr uint64_t B = UINT64_C(1) << 22;
    CHECK(is_probable_prime_u64(p) && is_probable_prime_u64(q) && is_probable_prime_u64(r),
          "out-of-bound 3LP metadata must contain primes");

    const Integer c = Integer(p) * Integer(q) * Integer(r);
    CHECK(!c.fits_uint64(), "out-of-bound product should exceed uint64_t");
    CHECK(c > Integer(static_cast<unsigned long long>(B)) * Integer(B),
          "out-of-bound product must be above B^2");
    CHECK(c <= Integer(static_cast<unsigned long long>(B)) * Integer(B) * Integer(B),
          "out-of-bound product must be at or below B^3");

    const auto cls = classify_cofactor(c, B, /*allow_3lp=*/true);
    CHECK(cls.type != CofactorClass::ThreeLP,
          "factor above B must not be accepted as ThreeLP");
    CHECK(!try_classify_three_lp(c, B).has_value(),
          "direct helper must reject factor above B");
    std::cout << "OK (cls=" << static_cast<int>(cls.type) << ")" << std::endl;
}

// Test 13: exact B^3 boundary with a repeated prime factor is accepted.
void test_big_integer_b3_boundary() {
    std::cout << "test_big_integer_b3_boundary... ";
    constexpr uint64_t B = 3000017; // prime, so B^3 is a valid repeated-factor 3LP
    CHECK(is_probable_prime_u64(B), "B^3 boundary base must be prime");
    const Integer bound(B);
    const Integer c = bound * bound * bound;
    CHECK(!c.fits_uint64(), "B^3 boundary should exceed uint64_t");

    const auto cls = classify_cofactor(c, B, /*allow_3lp=*/true);
    CHECK(cls.type == CofactorClass::ThreeLP,
          "exact B^3 boundary should classify as ThreeLP");
    if (cls.type == CofactorClass::ThreeLP) {
        CHECK(cls.factor1 == B && cls.factor2 == B && cls.factor3 == B,
              "B^3 boundary factors are not the repeated bound prime");
    }
    CHECK(quick_cofactor_check(c, B, /*allow_2lp=*/true, /*allow_3lp=*/true),
          "quick check must include exact B^3 boundary");
    std::cout << "OK" << std::endl;
}

// Test 14: an exhausted factorization budget fails closed without accepting a 3LP.
void test_big_integer_factorization_budget_failure() {
    std::cout << "test_big_integer_factorization_budget_failure... ";
    constexpr uint64_t p = 3000017;
    constexpr uint64_t q = 3000073;
    constexpr uint64_t r = 3000103;
    constexpr uint64_t B = UINT64_C(1) << 22;
    const Integer c = Integer(p) * Integer(q) * Integer(r);

    // Model an ECM/Brent budget that returns no divisor. The production helper
    // must propagate this failure as nullopt rather than guessing a classification.
    const auto no_budget_factor = [](const Integer&) -> std::optional<Integer> {
        return std::nullopt;
    };
    const auto result =
        gnfs::cofactor::detail::try_classify_three_lp_integer_impl(c, B, no_budget_factor);
    CHECK(!result.has_value(), "factorization budget failure must reject the candidate");
    std::cout << "OK" << std::endl;
}

int main() {
    test_3lp_accepted();
    test_3lp_rejected_by_default();
    test_3lp_rejected_factor_too_large();
    test_cofactor_above_b3();
    test_quick_check_3lp_window();
    test_direct_3lp_helper();
    test_2lp_still_classified();
    test_smooth_unchanged();
    test_prime_unchanged();
    test_hard_3lp_corpus();
    test_big_integer_3lp();
    test_big_integer_factor_above_bound();
    test_big_integer_b3_boundary();
    test_big_integer_factorization_budget_failure();

    std::cout << "\n=============================================" << std::endl;
    std::cout << "  Results: " << g_pass << " passed, " << g_fail << " failed" << std::endl;
    std::cout << "=============================================" << std::endl;
    return (g_fail == 0) ? 0 : 1;
}
