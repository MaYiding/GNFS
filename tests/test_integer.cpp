#include "gnfs/core/integer.hpp"
#include "gnfs/core/relation.hpp"
#include "gnfs/util/safe_math.hpp"
#include "support/test_check.hpp"

#include <climits>
#include <iostream>
#include <numeric>
#include <sstream>

using namespace gnfs::core;

void test_construction() {
    std::cout << "Testing construction..." << std::endl;

    // 默认构造
    Integer zero;
    GNFS_TEST_CHECK(zero.is_zero());
    GNFS_TEST_CHECK(zero.to_string() == "0");

    // 从 int64_t 构造
    Integer pos(12345);
    GNFS_TEST_CHECK(pos.to_int64() == 12345);

    Integer neg(-9876);
    GNFS_TEST_CHECK(neg.to_int64() == -9876);

    // 从字符串构造
    Integer big("123456789012345678901234567890");
    GNFS_TEST_CHECK(big.to_string() == "123456789012345678901234567890");

    // 十六进制
    Integer hex("FF", 16);
    GNFS_TEST_CHECK(hex.to_int64() == 255);

    std::cout << "  Construction: PASS" << std::endl;
}

void test_arithmetic() {
    std::cout << "Testing arithmetic..." << std::endl;

    Integer a(100);
    Integer b(30);

    // 加法
    Integer sum = a.clone();
    sum += b;
    GNFS_TEST_CHECK(sum.to_int64() == 130);

    // 减法
    Integer diff = a.clone();
    diff -= b;
    GNFS_TEST_CHECK(diff.to_int64() == 70);

    // 乘法
    Integer prod = a.clone();
    prod *= b;
    GNFS_TEST_CHECK(prod.to_int64() == 3000);

    // 除法
    Integer quot = a.clone();
    quot /= b;
    GNFS_TEST_CHECK(quot.to_int64() == 3);

    // 取模
    Integer rem = a.clone();
    rem %= b;
    GNFS_TEST_CHECK(rem.to_int64() == 10);

    std::cout << "  Arithmetic: PASS" << std::endl;
}

void test_comparison() {
    std::cout << "Testing comparison..." << std::endl;

    Integer a(100);
    Integer b(200);
    Integer c(100);

    GNFS_TEST_CHECK(a < b);
    GNFS_TEST_CHECK(b > a);
    GNFS_TEST_CHECK(a <= c);
    GNFS_TEST_CHECK(a >= c);
    GNFS_TEST_CHECK(a == c);
    GNFS_TEST_CHECK(a != b);

    std::cout << "  Comparison: PASS" << std::endl;
}

void test_bit_operations() {
    std::cout << "Testing bit operations..." << std::endl;

    Integer n(0b10101010);

    GNFS_TEST_CHECK(n.test_bit(1));
    GNFS_TEST_CHECK(!n.test_bit(0));
    GNFS_TEST_CHECK(n.test_bit(3));
    GNFS_TEST_CHECK(!n.test_bit(2));

    n.set_bit(0);
    GNFS_TEST_CHECK(n.test_bit(0));

    n.clear_bit(1);
    GNFS_TEST_CHECK(!n.test_bit(1));

    Integer big("123456789012345678901234567890");
    GNFS_TEST_CHECK(big.bit_length() > 90);

    std::cout << "  Bit operations: PASS" << std::endl;
}

void test_move_semantics() {
    std::cout << "Testing move semantics..." << std::endl;

    Integer a("12345678901234567890");
    std::string original = a.to_string();

    Integer b(std::move(a));
    GNFS_TEST_CHECK(b.to_string() == original);

    Integer c;
    c = std::move(b);
    GNFS_TEST_CHECK(c.to_string() == original);

    std::cout << "  Move semantics: PASS" << std::endl;
}

void test_gcd() {
    std::cout << "Testing GCD..." << std::endl;

    Integer a(48);
    Integer b(18);
    Integer g = gcd(a, b);
    GNFS_TEST_CHECK(g.to_int64() == 6);

    Integer big1("123456789012345678901234567890");
    Integer big2("987654321098765432109876543210");
    Integer big_gcd = gcd(big1, big2);
    // 验证 big_gcd 整除两个数
    Integer r1, r2;
    Integer::mod(r1, big1, big_gcd);
    Integer::mod(r2, big2, big_gcd);
    GNFS_TEST_CHECK(r1.is_zero());
    GNFS_TEST_CHECK(r2.is_zero());

    std::cout << "  GCD: PASS" << std::endl;
}

void test_powmod() {
    std::cout << "Testing powmod..." << std::endl;

    Integer base(2);
    Integer exp(10);
    Integer mod(1000);

    Integer result = powmod(base, exp, mod);
    GNFS_TEST_CHECK(result.to_int64() == 24); // 2^10 = 1024 % 1000 = 24

    // 大数 powmod
    Integer big_base("12345");
    Integer big_exp("67890");
    Integer big_mod("1000000007");
    Integer big_result = powmod(big_base, big_exp, big_mod);
    // 只验证结果在范围内
    GNFS_TEST_CHECK(big_result >= Integer(static_cast<int64_t>(0)) && big_result < big_mod);

    std::cout << "  Powmod: PASS" << std::endl;
}

void test_primality() {
    std::cout << "Testing primality..." << std::endl;

    Integer prime(104729); // 第10000个素数
    GNFS_TEST_CHECK(prime.is_probable_prime() > 0);

    Integer composite(104730);
    GNFS_TEST_CHECK(composite.is_probable_prime() == 0);

    // 大素数
    Integer mersenne("2147483647"); // 2^31 - 1，梅森素数
    GNFS_TEST_CHECK(mersenne.is_probable_prime() > 0);

    std::cout << "  Primality: PASS" << std::endl;
}

void test_stream_output() {
    std::cout << "Testing stream output..." << std::endl;

    Integer n(42);
    std::ostringstream oss;
    oss << n;
    GNFS_TEST_CHECK(oss.str() == "42");

    std::cout << "  Stream output: PASS" << std::endl;
}

void test_uint64_construction() {
    std::cout << "Testing uint64_t construction..." << std::endl;

    // --- Case 1: Value exactly at 2^63 (first value that overflows int64_t) ---
    {
        uint64_t val = uint64_t(1) << 63; // 9223372036854775808
        Integer n(val);
        GNFS_TEST_CHECK(n.is_positive() &&
                        "2^63 should be positive, not negative from int64_t overflow");
        GNFS_TEST_CHECK(n.to_string() == "9223372036854775808");
        GNFS_TEST_CHECK(n.bit_length() == 64);
    }

    // --- Case 2: UINT64_MAX ---
    {
        uint64_t val = UINT64_MAX; // 18446744073709551615
        Integer n(val);
        GNFS_TEST_CHECK(n.is_positive());
        GNFS_TEST_CHECK(n.to_string() == "18446744073709551615");
    }

    // --- Case 3: Value in safe range (< 2^63) still works ---
    {
        uint64_t val = 42;
        Integer n(val);
        GNFS_TEST_CHECK(n.to_int64() == 42);
    }

    // --- Case 4: Zero via uint64_t ---
    {
        uint64_t val = 0;
        Integer n(val);
        GNFS_TEST_CHECK(n.is_zero());
    }

    // --- Case 4b: Value in (2^32, 2^63) range ---
    {
        uint64_t val = uint64_t(1) << 40; // 1099511627776, > UINT32_MAX, < INT64_MAX
        Integer n(val);
        GNFS_TEST_CHECK(n.is_positive());
        GNFS_TEST_CHECK(n.to_string() == "1099511627776");
    }

    // --- Case 5: Assignment from uint64_t ---
    {
        Integer n(int64_t(0));
        uint64_t val = uint64_t(1) << 63;
        n = val;
        GNFS_TEST_CHECK(n.is_positive());
        GNFS_TEST_CHECK(n.to_string() == "9223372036854775808");
    }

    // --- Case 6: Arithmetic with large uint64_t Integer ---
    {
        uint64_t val = uint64_t(1) << 63;
        Integer a(val);
        Integer b(val);
        Integer sum = a + b;
        // 2^63 + 2^63 = 2^64
        GNFS_TEST_CHECK(sum.to_string() == "18446744073709551616");
    }

    // --- Case 7: Regression — int literals still work (no ambiguity) ---
    {
        Integer a(42);  // int literal
        Integer b(-17); // negative int literal
        GNFS_TEST_CHECK(a.to_int64() == 42);
        GNFS_TEST_CHECK(b.to_int64() == -17);
    }

    // --- Case 8: Regression — int64_t explicit still works ---
    {
        Integer a(int64_t(-1));
        GNFS_TEST_CHECK(a.to_int64() == -1);
        GNFS_TEST_CHECK(a.is_negative());
    }

    std::cout << "  uint64_t construction: PASS" << std::endl;
}

void test_safe_abs() {
    std::cout << "Testing safe_abs..." << std::endl;

    using gnfs::util::safe_abs;

    // Normal positive value
    GNFS_TEST_CHECK(safe_abs(int64_t(42)) == 42u);

    // Normal negative value
    GNFS_TEST_CHECK(safe_abs(int64_t(-42)) == 42u);

    // Zero
    GNFS_TEST_CHECK(safe_abs(int64_t(0)) == 0u);

    // INT64_MAX
    GNFS_TEST_CHECK(safe_abs(INT64_MAX) == static_cast<uint64_t>(INT64_MAX));

    // -1
    GNFS_TEST_CHECK(safe_abs(int64_t(-1)) == 1u);

    // INT64_MIN — the critical case!
    // |INT64_MIN| = 2^63 = 9223372036854775808, which is INT64_MAX + 1
    // std::abs(INT64_MIN) is UB, but safe_abs must handle it correctly.
    constexpr uint64_t expected = static_cast<uint64_t>(INT64_MAX) + 1u;
    GNFS_TEST_CHECK(safe_abs(INT64_MIN) == expected);
    GNFS_TEST_CHECK(safe_abs(INT64_MIN) == uint64_t(9223372036854775808ull));

    // INT64_MIN + 1 (= -INT64_MAX)
    GNFS_TEST_CHECK(safe_abs(INT64_MIN + 1) == static_cast<uint64_t>(INT64_MAX));

    std::cout << "  safe_abs: PASS" << std::endl;
}

void test_relation_ab_int64_min() {
    std::cout << "Testing Relation::ab() with extreme b values..." << std::endl;

    using gnfs::core::Relation;

    // Normal case: small b
    {
        Relation r(5, 3u);
        auto ab = r.ab();
        GNFS_TEST_CHECK(ab.a == 5);
        GNFS_TEST_CHECK(ab.b == 3u);
    }

    // Typical sieve value: b well within range
    {
        Relation r(-17, 42u);
        auto ab = r.ab();
        GNFS_TEST_CHECK(ab.a == -17);
        GNFS_TEST_CHECK(ab.b == 42u);
    }

    // Large b (still valid — uint64_t range)
    {
        Relation r(5, uint64_t(9223372036854775808ull));
        auto ab = r.ab();
        GNFS_TEST_CHECK(ab.a == 5);
        GNFS_TEST_CHECK(ab.b == uint64_t(9223372036854775808ull));
    }

    // b = UINT64_MAX (edge case)
    {
        Relation r(5, UINT64_MAX);
        auto ab = r.ab();
        GNFS_TEST_CHECK(ab.a == 5);
        GNFS_TEST_CHECK(ab.b == UINT64_MAX);
    }

    std::cout << "  Relation::ab() extreme b: PASS" << std::endl;
}

// 回归保护:Integer 的 int64_t 运算符在 INT64_MIN 时不能 UB。
// 历史上 `-(INT64_MIN)` 是 signed overflow UB,sanitizers 会抓但 Release 静默 wrap。
// integer.cpp 用 `static_cast<unsigned long>(-(v+1)) + 1UL` 模式避免;此测试锁住。
void test_int64_min_boundaries() {
    std::cout << "Testing Integer int64_t boundary (INT64_MIN/MAX/UINT64_MAX)..." << std::endl;

    // operator+=(int64_t) with INT64_MIN
    {
        Integer x(0);
        x += INT64_MIN;
        // x should now be INT64_MIN = -9223372036854775808
        Integer expected("-9223372036854775808");
        GNFS_TEST_CHECK(x == expected);
    }

    // operator-=(int64_t) with INT64_MIN: 0 - INT64_MIN should be 2^63
    {
        Integer x(0);
        x -= INT64_MIN;
        Integer expected("9223372036854775808"); // 2^63
        GNFS_TEST_CHECK(x == expected);
    }

    // operator/=(int64_t) with INT64_MIN
    {
        Integer x("9223372036854775808"); // 2^63
        x /= INT64_MIN;                   // 2^63 / (-2^63) = -1
        Integer expected(-1);
        GNFS_TEST_CHECK(x == expected);
    }

    // operator%=(int64_t) with INT64_MIN
    {
        Integer x(100);
        x %= INT64_MIN;
        // 100 mod -2^63 = 100 (positive remainder, since |x| < |mod|)
        GNFS_TEST_CHECK(x.to_int64() == 100);
    }

    // operator*=(int64_t) with INT64_MIN
    {
        Integer x(2);
        x *= INT64_MIN;
        // 2 * INT64_MIN = -2^64 (does not fit int64_t)
        Integer expected("-18446744073709551616");
        GNFS_TEST_CHECK(x == expected);
    }

    // operator+=(int64_t) with INT64_MAX
    {
        Integer x(0);
        x += INT64_MAX;
        Integer expected("9223372036854775807");
        GNFS_TEST_CHECK(x == expected);
    }

    // construction from uint64_t with UINT64_MAX
    {
        Integer x(UINT64_MAX);
        Integer expected("18446744073709551615");
        GNFS_TEST_CHECK(x == expected);
        GNFS_TEST_CHECK(x.fits_uint64());
        GNFS_TEST_CHECK(!x.fits_int64());
    }

    // construction from int64_t with INT64_MIN
    {
        Integer x(INT64_MIN);
        Integer expected("-9223372036854775808");
        GNFS_TEST_CHECK(x == expected);
        GNFS_TEST_CHECK(x.fits_int64());
        GNFS_TEST_CHECK(x.to_int64() == INT64_MIN);
    }

    std::cout << "  INT64_MIN/MAX/UINT64_MAX boundaries: PASS" << std::endl;
}

void test_safe_gcd_with_int64_min() {
    std::cout << "Testing std::gcd with safe_abs (no UB)..." << std::endl;

    using gnfs::util::safe_abs;

    // gcd(|INT64_MIN|, 2) = gcd(2^63, 2) = 2
    GNFS_TEST_CHECK(std::gcd(safe_abs(INT64_MIN), uint64_t(2)) == 2u);

    // gcd(|INT64_MIN|, 1) = 1
    GNFS_TEST_CHECK(std::gcd(safe_abs(INT64_MIN), uint64_t(1)) == 1u);

    // gcd(|INT64_MIN|, 3) = gcd(2^63, 3) = 1
    GNFS_TEST_CHECK(std::gcd(safe_abs(INT64_MIN), uint64_t(3)) == 1u);

    // gcd(|INT64_MIN|, 4) = gcd(2^63, 4) = 4
    GNFS_TEST_CHECK(std::gcd(safe_abs(INT64_MIN), uint64_t(4)) == 4u);

    std::cout << "  safe_gcd with INT64_MIN: PASS" << std::endl;
}

int main() {
    std::cout << "=== Integer Tests ===" << std::endl;

    test_construction();
    test_arithmetic();
    test_comparison();
    test_bit_operations();
    test_move_semantics();
    test_gcd();
    test_powmod();
    test_primality();
    test_stream_output();
    test_uint64_construction();
    test_safe_abs();
    test_relation_ab_int64_min();
    test_safe_gcd_with_int64_min();
    test_int64_min_boundaries();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
