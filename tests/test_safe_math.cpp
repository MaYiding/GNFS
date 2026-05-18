// Unit tests for gnfs::util::safe_math.
//
// safe_abs() is a critical UB-avoidance helper used wherever the codebase
// needs |x| for an int64_t. The standard library's std::abs(INT64_MIN) is
// UB because the result (2^63) doesn't fit in int64_t. safe_abs returns
// uint64_t to safely cover the full range.

#include "gnfs/util/safe_math.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>

using gnfs::util::safe_abs;

void test_zero() {
    std::cout << "Testing safe_abs(0)..." << std::endl;
    assert(safe_abs(0) == 0u);
    std::cout << "  PASS" << std::endl;
}

void test_positive_values() {
    std::cout << "Testing safe_abs positive values..." << std::endl;

    assert(safe_abs(1) == 1u);
    assert(safe_abs(42) == 42u);
    assert(safe_abs(1000000) == 1000000u);
    assert(safe_abs(int64_t(1) << 32) == (uint64_t(1) << 32));

    // INT64_MAX itself
    assert(safe_abs(INT64_MAX) == static_cast<uint64_t>(INT64_MAX));

    std::cout << "  PASS" << std::endl;
}

void test_negative_values() {
    std::cout << "Testing safe_abs negative values..." << std::endl;

    assert(safe_abs(-1) == 1u);
    assert(safe_abs(-42) == 42u);
    assert(safe_abs(-1000000) == 1000000u);
    assert(safe_abs(-(int64_t(1) << 32)) == (uint64_t(1) << 32));

    // Symmetric across zero
    assert(safe_abs(int64_t(12345)) == safe_abs(int64_t(-12345)));

    std::cout << "  PASS" << std::endl;
}

void test_int64_min_no_ub() {
    std::cout << "Testing safe_abs(INT64_MIN) — the critical UB case..." << std::endl;

    // |INT64_MIN| = 2^63, which does NOT fit in int64_t.
    // std::abs(INT64_MIN) is UB. safe_abs returns uint64_t and gets it right.
    const uint64_t expected = uint64_t(1) << 63;  // 9223372036854775808

    assert(safe_abs(INT64_MIN) == expected);
    assert(safe_abs(std::numeric_limits<int64_t>::min()) == expected);

    // Conceptually: |INT64_MIN| = INT64_MAX + 1 in unsigned space
    assert(safe_abs(INT64_MIN) == static_cast<uint64_t>(INT64_MAX) + 1u);

    std::cout << "  PASS" << std::endl;
}

void test_near_int64_min() {
    std::cout << "Testing safe_abs near INT64_MIN..." << std::endl;

    // INT64_MIN + 1 is the largest representable negative result of -INT64_MIN's
    // would-be value (i.e., one less than 2^63). Check |INT64_MIN+1| = 2^63-1.
    assert(safe_abs(INT64_MIN + 1) == static_cast<uint64_t>(INT64_MAX));
    assert(safe_abs(INT64_MIN + 2) == static_cast<uint64_t>(INT64_MAX) - 1u);

    // Monotonicity: larger absolute value → larger return.
    assert(safe_abs(INT64_MIN) > safe_abs(INT64_MIN + 1));
    assert(safe_abs(INT64_MIN + 1) > safe_abs(INT64_MIN + 2));

    std::cout << "  PASS" << std::endl;
}

void test_constexpr() {
    std::cout << "Testing safe_abs is constexpr..." << std::endl;

    // Compile-time evaluation — these are required to work at constexpr
    // since safe_abs is declared constexpr.
    static_assert(safe_abs(0) == 0u);
    static_assert(safe_abs(42) == 42u);
    static_assert(safe_abs(-42) == 42u);
    static_assert(safe_abs(INT64_MAX) == static_cast<uint64_t>(INT64_MAX));
    static_assert(safe_abs(INT64_MIN) == uint64_t(1) << 63);

    std::cout << "  PASS (compile-time)" << std::endl;
}

void test_noexcept_contract() {
    std::cout << "Testing safe_abs noexcept..." << std::endl;
    static_assert(noexcept(safe_abs(int64_t(0))));
    static_assert(noexcept(safe_abs(INT64_MIN)));
    static_assert(noexcept(safe_abs(INT64_MAX)));
    std::cout << "  PASS" << std::endl;
}

void test_return_type() {
    std::cout << "Testing safe_abs returns uint64_t..." << std::endl;

    // Return type must be uint64_t — anything narrower (e.g., int64_t)
    // wouldn't be able to represent |INT64_MIN|.
    static_assert(std::is_same_v<decltype(safe_abs(int64_t(0))), uint64_t>);

    std::cout << "  PASS" << std::endl;
}

void test_sign_symmetry_in_range() {
    std::cout << "Testing safe_abs(x) == safe_abs(-x) in safe range..." << std::endl;

    // For x ∈ [INT64_MIN+1, INT64_MAX], safe_abs(x) == safe_abs(-x).
    // (INT64_MIN excluded because -INT64_MIN doesn't exist as int64_t.)
    for (int64_t x : {int64_t(1), int64_t(100), int64_t(10000),
                       int64_t(1) << 20, INT64_MAX}) {
        assert(safe_abs(x) == safe_abs(-x));
    }

    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== util/safe_math.hpp tests ===" << std::endl;

    test_zero();
    test_positive_values();
    test_negative_values();
    test_int64_min_no_ub();
    test_near_int64_min();
    test_constexpr();
    test_noexcept_contract();
    test_return_type();
    test_sign_symmetry_in_range();

    std::cout << "\n=== All util/safe_math.hpp tests PASSED ===" << std::endl;
    return 0;
}
