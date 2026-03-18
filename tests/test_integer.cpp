#include "gnfs/core/integer.hpp"

#include "gnfs/core/integer.hpp"

#include <cassert>
#include <iostream>
#include <sstream>

using namespace gnfs::core;

void test_construction() {
    std::cout << "Testing construction..." << std::endl;

    // 默认构造
    Integer zero;
    assert(zero.is_zero());
    assert(zero.to_string() == "0");

    // 从 int64_t 构造
    Integer pos(12345);
    assert(pos.to_int64() == 12345);

    Integer neg(-9876);
    assert(neg.to_int64() == -9876);

    // 从字符串构造
    Integer big("123456789012345678901234567890");
    assert(big.to_string() == "123456789012345678901234567890");

    // 十六进制
    Integer hex("FF", 16);
    assert(hex.to_int64() == 255);

    std::cout << "  Construction: PASS" << std::endl;
}

void test_arithmetic() {
    std::cout << "Testing arithmetic..." << std::endl;

    Integer a(100);
    Integer b(30);

    // 加法
    Integer sum = a.clone();
    sum += b;
    assert(sum.to_int64() == 130);

    // 减法
    Integer diff = a.clone();
    diff -= b;
    assert(diff.to_int64() == 70);

    // 乘法
    Integer prod = a.clone();
    prod *= b;
    assert(prod.to_int64() == 3000);

    // 除法
    Integer quot = a.clone();
    quot /= b;
    assert(quot.to_int64() == 3);

    // 取模
    Integer rem = a.clone();
    rem %= b;
    assert(rem.to_int64() == 10);

    std::cout << "  Arithmetic: PASS" << std::endl;
}

void test_comparison() {
    std::cout << "Testing comparison..." << std::endl;

    Integer a(100);
    Integer b(200);
    Integer c(100);

    assert(a < b);
    assert(b > a);
    assert(a <= c);
    assert(a >= c);
    assert(a == c);
    assert(a != b);

    std::cout << "  Comparison: PASS" << std::endl;
}

void test_bit_operations() {
    std::cout << "Testing bit operations..." << std::endl;

    Integer n(0b10101010);

    assert(n.test_bit(1));
    assert(!n.test_bit(0));
    assert(n.test_bit(3));
    assert(!n.test_bit(2));

    n.set_bit(0);
    assert(n.test_bit(0));

    n.clear_bit(1);
    assert(!n.test_bit(1));

    Integer big("123456789012345678901234567890");
    assert(big.bit_length() > 90);

    std::cout << "  Bit operations: PASS" << std::endl;
}

void test_move_semantics() {
    std::cout << "Testing move semantics..." << std::endl;

    Integer a("12345678901234567890");
    std::string original = a.to_string();

    Integer b(std::move(a));
    assert(b.to_string() == original);

    Integer c;
    c = std::move(b);
    assert(c.to_string() == original);

    std::cout << "  Move semantics: PASS" << std::endl;
}

void test_gcd() {
    std::cout << "Testing GCD..." << std::endl;

    Integer a(48);
    Integer b(18);
    Integer g = gcd(a, b);
    assert(g.to_int64() == 6);

    Integer big1("123456789012345678901234567890");
    Integer big2("987654321098765432109876543210");
    Integer big_gcd = gcd(big1, big2);
    // 验证 big_gcd 整除两个数
    Integer r1, r2;
    Integer::mod(r1, big1, big_gcd);
    Integer::mod(r2, big2, big_gcd);
    assert(r1.is_zero());
    assert(r2.is_zero());

    std::cout << "  GCD: PASS" << std::endl;
}

void test_powmod() {
    std::cout << "Testing powmod..." << std::endl;

    Integer base(2);
    Integer exp(10);
    Integer mod(1000);

    Integer result = powmod(base, exp, mod);
    assert(result.to_int64() == 24);  // 2^10 = 1024 % 1000 = 24

    // 大数 powmod
    Integer big_base("12345");
    Integer big_exp("67890");
    Integer big_mod("1000000007");
    Integer big_result = powmod(big_base, big_exp, big_mod);
    // 只验证结果在范围内
    assert(big_result >= Integer(static_cast<int64_t>(0)) && big_result < big_mod);

    std::cout << "  Powmod: PASS" << std::endl;
}

void test_primality() {
    std::cout << "Testing primality..." << std::endl;

    Integer prime(104729);  // 第10000个素数
    assert(prime.is_probable_prime() > 0);

    Integer composite(104730);
    assert(composite.is_probable_prime() == 0);

    // 大素数
    Integer mersenne("2147483647");  // 2^31 - 1，梅森素数
    assert(mersenne.is_probable_prime() > 0);

    std::cout << "  Primality: PASS" << std::endl;
}

void test_stream_output() {
    std::cout << "Testing stream output..." << std::endl;

    Integer n(42);
    std::ostringstream oss;
    oss << n;
    assert(oss.str() == "42");

    std::cout << "  Stream output: PASS" << std::endl;
}

void test_uint64_construction() {
    std::cout << "Testing uint64_t construction..." << std::endl;

    // --- Case 1: Value exactly at 2^63 (first value that overflows int64_t) ---
    {
        uint64_t val = uint64_t(1) << 63;  // 9223372036854775808
        Integer n(val);
        assert(n.is_positive() && "2^63 should be positive, not negative from int64_t overflow");
        assert(n.to_string() == "9223372036854775808");
        assert(n.bit_length() == 64);
    }

    // --- Case 2: UINT64_MAX ---
    {
        uint64_t val = UINT64_MAX;  // 18446744073709551615
        Integer n(val);
        assert(n.is_positive());
        assert(n.to_string() == "18446744073709551615");
    }

    // --- Case 3: Value in safe range (< 2^63) still works ---
    {
        uint64_t val = 42;
        Integer n(val);
        assert(n.to_int64() == 42);
    }

    // --- Case 4: Zero via uint64_t ---
    {
        uint64_t val = 0;
        Integer n(val);
        assert(n.is_zero());
    }

    // --- Case 5: Assignment from uint64_t ---
    {
        Integer n(int64_t(0));
        uint64_t val = uint64_t(1) << 63;
        n = val;
        assert(n.is_positive());
        assert(n.to_string() == "9223372036854775808");
    }

    // --- Case 6: Arithmetic with large uint64_t Integer ---
    {
        uint64_t val = uint64_t(1) << 63;
        Integer a(val);
        Integer b(val);
        Integer sum = a + b;
        // 2^63 + 2^63 = 2^64
        assert(sum.to_string() == "18446744073709551616");
    }

    // --- Case 7: Regression — int literals still work (no ambiguity) ---
    {
        Integer a(42);      // int literal
        Integer b(-17);     // negative int literal
        assert(a.to_int64() == 42);
        assert(b.to_int64() == -17);
    }

    // --- Case 8: Regression — int64_t explicit still works ---
    {
        Integer a(int64_t(-1));
        assert(a.to_int64() == -1);
        assert(a.is_negative());
    }

    std::cout << "  uint64_t construction: PASS" << std::endl;
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

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
