// Unit tests for gnfs::polynomial::bareiss_determinant + resultant.
//
// These primitives sit under IntPolynomial::discriminant and ClassGroup,
// but were only tested indirectly through known-discriminant polynomials.
// Direct coverage isolates the Sylvester-matrix construction and the
// fraction-free Gaussian elimination step.

#include "gnfs/polynomial/resultant.hpp"
#include "support/test_check.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using gnfs::core::Integer;
using gnfs::polynomial::bareiss_determinant;
using gnfs::polynomial::discriminant;
using gnfs::polynomial::resultant;

namespace {

// Make a matrix from int64_t rows for readability.
std::vector<std::vector<Integer>> make_matrix(const std::vector<std::vector<std::int64_t>>& rows) {
    std::vector<std::vector<Integer>> M;
    M.reserve(rows.size());
    for (const auto& r : rows) {
        std::vector<Integer> row;
        row.reserve(r.size());
        for (std::int64_t v : r)
            row.emplace_back(v);
        M.push_back(std::move(row));
    }
    return M;
}

std::vector<Integer> make_poly(const std::vector<std::int64_t>& coeffs) {
    std::vector<Integer> p;
    p.reserve(coeffs.size());
    for (std::int64_t c : coeffs)
        p.emplace_back(c);
    return p;
}

std::string to_string(const Integer& x) {
    std::ostringstream oss;
    oss << x;
    return oss.str();
}

#define CHECK_INT_EQ(actual_expression, expected_expression)                                       \
    do {                                                                                           \
        const Integer actual_value{actual_expression};                                             \
        const Integer expected_value{expected_expression};                                         \
        const bool matches = actual_value == expected_value;                                       \
        if (!matches) {                                                                            \
            std::cerr << "  FAIL: " << #actual_expression << " = " << to_string(actual_value)      \
                      << " expected " << to_string(expected_value) << std::endl;                   \
        }                                                                                          \
        GNFS_TEST_CHECK(matches);                                                                  \
    } while (false)

} // namespace

void test_bareiss_identity() {
    std::cout << "Testing bareiss_determinant on identity matrix..." << std::endl;

    for (std::uint32_t n : {1u, 2u, 3u, 4u, 5u}) {
        std::vector<std::vector<Integer>> I(n, std::vector<Integer>(n));
        for (std::uint32_t i = 0; i < n; ++i)
            I[i][i] = Integer(1);
        Integer det = bareiss_determinant(I, n);
        CHECK_INT_EQ(det, 1);
    }

    std::cout << "  identity: PASS" << std::endl;
}

void test_bareiss_2x2() {
    std::cout << "Testing bareiss_determinant 2x2 ad-bc..." << std::endl;

    // |1 2| = 1*4 - 2*3 = -2
    // |3 4|
    auto M = make_matrix({{1, 2}, {3, 4}});
    Integer det = bareiss_determinant(M, 2);
    CHECK_INT_EQ(det, -2);

    // |5 6| = 5*8 - 6*7 = 40 - 42 = -2
    // |7 8|
    auto M2 = make_matrix({{5, 6}, {7, 8}});
    Integer det2 = bareiss_determinant(M2, 2);
    CHECK_INT_EQ(det2, -2);

    // |9 0| = 9 * 1 - 0 = 9
    // |0 1|
    auto M3 = make_matrix({{9, 0}, {0, 1}});
    Integer det3 = bareiss_determinant(M3, 2);
    CHECK_INT_EQ(det3, 9);

    std::cout << "  2x2: PASS" << std::endl;
}

void test_bareiss_3x3() {
    std::cout << "Testing bareiss_determinant 3x3 Sarrus..." << std::endl;

    // |1 2 3|
    // |4 5 6| — Sarrus: 1(5·9-6·8) - 2(4·9-6·7) + 3(4·8-5·7)
    // |7 8 9|         = 1·(-3) - 2·(-6) + 3·(-3) = -3 + 12 - 9 = 0 (singular)
    auto M = make_matrix({{1, 2, 3}, {4, 5, 6}, {7, 8, 9}});
    Integer det = bareiss_determinant(M, 3);
    CHECK_INT_EQ(det, 0);

    // |2 1 1|
    // |1 2 1| = 2(2·2-1) - 1(1·2-1) + 1(1-2) = 2·3 - 1·1 + 1·(-1) = 6-1-1 = 4
    // |1 1 2|
    auto M2 = make_matrix({{2, 1, 1}, {1, 2, 1}, {1, 1, 2}});
    Integer det2 = bareiss_determinant(M2, 3);
    CHECK_INT_EQ(det2, 4);

    // |3 0 0|
    // |0 4 0| = upper triangular product = 3·4·5 = 60
    // |0 0 5|
    auto M3 = make_matrix({{3, 0, 0}, {0, 4, 0}, {0, 0, 5}});
    Integer det3 = bareiss_determinant(M3, 3);
    CHECK_INT_EQ(det3, 60);

    std::cout << "  3x3: PASS" << std::endl;
}

void test_bareiss_singular() {
    std::cout << "Testing bareiss_determinant singular matrices..." << std::endl;

    // Row of zeros → det = 0
    auto M1 = make_matrix({{1, 2, 3}, {0, 0, 0}, {7, 8, 9}});
    Integer det1 = bareiss_determinant(M1, 3);
    CHECK_INT_EQ(det1, 0);

    // Duplicate rows → det = 0
    auto M2 = make_matrix({{2, 1}, {2, 1}});
    Integer det2 = bareiss_determinant(M2, 2);
    CHECK_INT_EQ(det2, 0);

    // Column of zeros at position 0 → det = 0
    auto M3 = make_matrix({{0, 1, 2}, {0, 4, 5}, {0, 7, 8}});
    Integer det3 = bareiss_determinant(M3, 3);
    CHECK_INT_EQ(det3, 0);

    std::cout << "  singular: PASS" << std::endl;
}

void test_bareiss_row_swap_sign() {
    std::cout << "Testing bareiss_determinant row swap sign flip..." << std::endl;

    // det(I) = 1, but if we present rows swapped, det should be -1.
    // |0 1|
    // |1 0|
    auto M1 = make_matrix({{0, 1}, {1, 0}});
    Integer det1 = bareiss_determinant(M1, 2);
    CHECK_INT_EQ(det1, -1);

    // Three-way pivot swap on 3x3 cyclic permutation:
    // |0 1 0|
    // |0 0 1|  -- two row swaps: (0↔1)(1↔2) = +1 sign
    // |1 0 0|
    auto M2 = make_matrix({{0, 1, 0}, {0, 0, 1}, {1, 0, 0}});
    Integer det2 = bareiss_determinant(M2, 3);
    CHECK_INT_EQ(det2, 1);

    std::cout << "  row swap sign: PASS" << std::endl;
}

void test_bareiss_large_values() {
    std::cout << "Testing bareiss_determinant with large entries..." << std::endl;

    // Use values with large intermediate products while keeping an exact,
    // compact determinant oracle.
    // |1000000000  1000000007 |
    // |1000000009  1000000021 |
    // det = 10^9 · 10^9+21 - (10^9+7) · (10^9+9) = ?
    //     = (10^9)^2 + 21·10^9 - [(10^9)^2 + 16·10^9 + 63]
    //     = 21·10^9 - 16·10^9 - 63
    //     = 5·10^9 - 63
    auto M = make_matrix({{1000000000, 1000000007}, {1000000009, 1000000021}});
    Integer det = bareiss_determinant(M, 2);
    Integer expected = Integer(std::int64_t{5000000000ll}) - Integer(std::int64_t{63});
    CHECK_INT_EQ(det, expected);

    std::cout << "  large values: PASS" << std::endl;
}

void test_resultant_linear_factors() {
    std::cout << "Testing resultant(x-a, x-b) = b-a..." << std::endl;

    // f = x - 3 → [coefficient of x^0, x^1] = [-3, 1], deg=1
    auto f = make_poly({-3, 1});
    // g = x - 7 → [-7, 1], deg=1
    auto g = make_poly({-7, 1});
    // Res(x-3, x-7) = 3 - 7 = -4 (with our convention)
    // Sylvester: |1 -3| → det = 1·-7 - (-3)·1 = -7+3 = -4
    //           |1 -7|
    Integer res = resultant(f, 1, g, 1);
    CHECK_INT_EQ(res, -4);

    // Res(x-a, x-a) should be 0 (common root)
    auto fa = make_poly({-5, 1});
    Integer res_eq = resultant(fa, 1, fa, 1);
    CHECK_INT_EQ(res_eq, 0);

    std::cout << "  linear factors: PASS" << std::endl;
}

void test_resultant_x2_minus_one_x2_plus_one() {
    std::cout << "Testing Res(x^2-1, x^2+1) = 4..." << std::endl;

    // f = x^2 - 1 = [-1, 0, 1]
    auto f = make_poly({-1, 0, 1});
    // g = x^2 + 1 = [1, 0, 1]
    auto g = make_poly({1, 0, 1});

    // Roots of f: ±1; roots of g: ±i. No common roots → Res ≠ 0.
    // Classical: Res(x^2-1, x^2+1) = (1-i)(1+i)(-1-i)(-1+i) = (1+1)(1+1) = 4.
    Integer res = resultant(f, 2, g, 2);
    CHECK_INT_EQ(res, 4);

    std::cout << "  x^2-1, x^2+1: PASS" << std::endl;
}

void test_resultant_with_shared_root() {
    std::cout << "Testing resultant zero when shared root exists..." << std::endl;

    // f = (x-2)(x-3) = x^2 - 5x + 6 = [6, -5, 1]
    auto f = make_poly({6, -5, 1});
    // g = x - 3 = [-3, 1], shares root x=3 with f
    auto g = make_poly({-3, 1});
    Integer res = resultant(f, 2, g, 1);
    CHECK_INT_EQ(res, 0);

    // f, f' for f with double root x=2: f = (x-2)^2 = x^2 - 4x + 4 = [4, -4, 1]
    //   f' = 2x - 4 = [-4, 2]
    // Res(f, f') = 0 because shared root at x=2
    auto f2 = make_poly({4, -4, 1});
    auto fp = make_poly({-4, 2});
    Integer res2 = resultant(f2, 2, fp, 1);
    CHECK_INT_EQ(res2, 0);

    std::cout << "  shared root → 0: PASS" << std::endl;
}

void test_resultant_via_discriminant() {
    std::cout << "Testing resultant correctness via discriminant identity..." << std::endl;

    // Δ(f) = (-1)^(d(d-1)/2) · Res(f, f') / a_d
    // For f = x^3 + x + 1, a_d = 1, d=3, sign=(-1)^3=-1.
    // Known: Δ = -4·1^3 - 27·1^2 = -31.
    // Therefore Res(f, f') = -Δ = 31.
    auto f = make_poly({1, 1, 0, 1}); // x^3 + x + 1
    auto fp = make_poly({1, 0, 3});   // f' = 3x^2 + 1
    Integer res = resultant(f, 3, fp, 2);
    CHECK_INT_EQ(res, 31);

    // Cross-check via the discriminant wrapper itself
    Integer disc = discriminant(f, 3);
    CHECK_INT_EQ(disc, -31);

    std::cout << "  via discriminant: PASS" << std::endl;
}

void test_discriminant_edge_cases() {
    std::cout << "Testing discriminant degree edge cases..." << std::endl;

    // d == 0: convention returns 0 (empty)
    auto f0 = make_poly({5});
    Integer disc0 = discriminant(f0, 0);
    CHECK_INT_EQ(disc0, 0);

    // d == 1: convention returns 1
    auto f1 = make_poly({3, 5});
    Integer disc1 = discriminant(f1, 1);
    CHECK_INT_EQ(disc1, 1);

    // d == 2: classical b^2 - 4ac
    // f = 2x^2 + 3x + 5 → b^2 - 4ac = 9 - 40 = -31
    auto f2 = make_poly({5, 3, 2});
    Integer disc2 = discriminant(f2, 2);
    CHECK_INT_EQ(disc2, -31);

    std::cout << "  discriminant edges: PASS" << std::endl;
}

int main() {
    std::cout << "=== polynomial::resultant tests ===" << std::endl;

    test_bareiss_identity();
    test_bareiss_2x2();
    test_bareiss_3x3();
    test_bareiss_singular();
    test_bareiss_row_swap_sign();
    test_bareiss_large_values();
    test_resultant_linear_factors();
    test_resultant_x2_minus_one_x2_plus_one();
    test_resultant_with_shared_root();
    test_resultant_via_discriminant();
    test_discriminant_edge_cases();

    std::cout << "\n=== All polynomial::resultant tests PASSED ===" << std::endl;
    return 0;
}
