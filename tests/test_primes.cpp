// Unit tests for gnfs::util::primes — Miller-Rabin, isqrt, next_prime, mul_mod, pow_mod.
//
// Critical helpers used by Couveignes algorithm + class_group sqrt path.
// Until now, only indirect coverage via tests/test_couveignes/test_class_group.
// This file isolates the math primitives so bugs surface immediately.

#include "gnfs/util/primes.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <set>
#include <vector>

using namespace gnfs::util;

namespace {

constexpr uint64_t U64_MAX = static_cast<uint64_t>(-1);

// Reference list of small primes via trial division for cross-validation.
std::vector<uint64_t> small_primes_up_to(uint64_t N) {
    std::vector<uint64_t> p;
    std::vector<bool> sieve(N + 1, true);
    sieve[0] = sieve[1] = false;
    for (uint64_t i = 2; i <= N; ++i) {
        if (sieve[i]) {
            p.push_back(i);
            for (uint64_t j = i * i; j <= N; j += i) sieve[j] = false;
        }
    }
    return p;
}

} // namespace

void test_mul_mod_u64() {
    std::cout << "Testing mul_mod_u64..." << std::endl;

    // Basic sanity
    assert(mul_mod_u64(0, 0, 7) == 0);
    assert(mul_mod_u64(3, 4, 7) == 5);
    assert(mul_mod_u64(7, 7, 7) == 0);
    assert(mul_mod_u64(1, 1, 7) == 1);

    // Large operands that would overflow uint64_t * uint64_t in naive impl.
    // (2^63 - 1) * (2^63 - 1) needs __uint128_t product space.
    const uint64_t a = (uint64_t{1} << 63) - 1;
    const uint64_t b = (uint64_t{1} << 63) - 1;
    const uint64_t m = (uint64_t{1} << 62) + 1;
    __uint128_t expected = static_cast<__uint128_t>(a) * b % m;
    assert(mul_mod_u64(a, b, m) == static_cast<uint64_t>(expected));

    // Modulus very close to UINT64_MAX
    const uint64_t big_mod = U64_MAX - 58;  // a prime near 2^64
    assert(mul_mod_u64(big_mod - 1, big_mod - 1, big_mod) == 1);  // (-1)^2 ≡ 1

    std::cout << "  mul_mod_u64: PASS" << std::endl;
}

void test_pow_mod_u64() {
    std::cout << "Testing pow_mod_u64..." << std::endl;

    // Fermat's little theorem: a^(p-1) ≡ 1 (mod p) for gcd(a, p)=1
    for (uint64_t p : {2u, 3u, 5u, 7u, 11u, 13u, 17u, 19u, 23u, 31u}) {
        for (uint64_t a = 1; a < p; ++a) {
            assert(pow_mod_u64(a, p - 1, p) == 1);
        }
    }

    // Trivial cases
    assert(pow_mod_u64(0, 0, 7) == 1);  // 0^0 = 1 convention
    assert(pow_mod_u64(0, 5, 7) == 0);  // 0^k = 0 for k > 0
    assert(pow_mod_u64(7, 5, 7) == 0);  // base ≡ 0 mod p
    assert(pow_mod_u64(1, U64_MAX, 7) == 1);  // 1^anything = 1

    // Known: 2^10 = 1024, mod 1000 = 24
    assert(pow_mod_u64(2, 10, 1000) == 24);

    // Edge: exp == 0 with non-trivial base
    assert(pow_mod_u64(5, 0, 13) == 1);

    // Carmichael: 561 = 3 * 11 * 17; 2^560 ≡ 1 (mod 561) but 561 composite.
    // We can still compute and verify.
    assert(pow_mod_u64(2, 560, 561) == 1);

    std::cout << "  pow_mod_u64: PASS" << std::endl;
}

void test_is_prime_u64_basics() {
    std::cout << "Testing is_prime_u64 basics..." << std::endl;

    // Bottom edge
    assert(!is_prime_u64(0));
    assert(!is_prime_u64(1));
    assert(is_prime_u64(2));
    assert(is_prime_u64(3));
    assert(!is_prime_u64(4));
    assert(is_prime_u64(5));

    // Cross-validate up to 10000 against sieve
    auto reference = small_primes_up_to(10000);
    std::set<uint64_t> ref_set(reference.begin(), reference.end());
    for (uint64_t n = 0; n <= 10000; ++n) {
        bool expected = ref_set.contains(n);
        bool got = is_prime_u64(n);
        if (expected != got) {
            std::cerr << "  is_prime_u64(" << n << ") got=" << got
                      << " expected=" << expected << std::endl;
        }
        assert(expected == got);
    }

    std::cout << "  is_prime_u64 basics: PASS" << std::endl;
}

void test_is_prime_u64_carmichael() {
    std::cout << "Testing is_prime_u64 against Carmichael numbers..." << std::endl;

    // Carmichael numbers: composite but pass Fermat's test for all bases coprime.
    // Miller-Rabin handles these — must return false.
    const uint64_t carmichael[] = {
        561,    // 3 · 11 · 17
        1105,   // 5 · 13 · 17
        1729,   // 7 · 13 · 19 (Ramanujan's taxicab)
        2465,   // 5 · 17 · 29
        2821,   // 7 · 13 · 31
        6601,   // 7 · 23 · 41
        8911,   // 7 · 19 · 67
        10585,  // 5 · 29 · 73
        15841,  // 7 · 31 · 73
        29341,  // 13 · 37 · 61
        41041,  // 7 · 11 · 13 · 41
        46657,  // 13 · 37 · 97
        62745,  // 3 · 5 · 47 · 89
        63973,  // 7 · 13 · 19 · 37
        75361,  // 11 · 13 · 17 · 31
    };
    for (uint64_t c : carmichael) {
        bool got = is_prime_u64(c);
        if (got) std::cerr << "  Carmichael " << c << " falsely passes!" << std::endl;
        assert(!got);
    }

    std::cout << "  is_prime_u64 Carmichael rejection: PASS" << std::endl;
}

void test_is_prime_u64_large() {
    std::cout << "Testing is_prime_u64 with large values..." << std::endl;

    // Known large primes
    assert(is_prime_u64(2147483647ull));        // 2^31 - 1 Mersenne prime
    assert(is_prime_u64(2305843009213693951ull));  // 2^61 - 1 Mersenne prime
    assert(is_prime_u64(18446744073709551557ull));  // largest 64-bit prime

    // Known large composites
    assert(!is_prime_u64(2147483648ull));       // 2^31 even
    assert(!is_prime_u64(2147483649ull));       // 2^31+1 = 3 · 715827883
    assert(!is_prime_u64(18446744073709551615ull));  // 2^64-1 = 3 · 5 · 17 · 257 · ...

    // Product of two primes (semiprime)
    // 1000003 · 1000033 = 1000036000099
    assert(!is_prime_u64(1000036000099ull));

    std::cout << "  is_prime_u64 large values: PASS" << std::endl;
}

void test_isqrt_u32() {
    std::cout << "Testing isqrt_u32..." << std::endl;

    // Bottom edge
    assert(isqrt_u32(0) == 0);
    assert(isqrt_u32(1) == 1);

    // Perfect squares
    for (uint32_t r = 0; r < 1000; ++r) {
        uint64_t r2 = static_cast<uint64_t>(r) * r;
        if (r2 > UINT32_MAX) break;
        assert(isqrt_u32(static_cast<uint32_t>(r2)) == r);
    }

    // Non-perfect squares: isqrt(n) should be floor(sqrt(n))
    assert(isqrt_u32(2) == 1);
    assert(isqrt_u32(3) == 1);
    assert(isqrt_u32(4) == 2);
    assert(isqrt_u32(8) == 2);
    assert(isqrt_u32(9) == 3);
    assert(isqrt_u32(15) == 3);
    assert(isqrt_u32(16) == 4);

    // Boundary: UINT32_MAX
    // floor(sqrt(2^32 - 1)) = 65535 because 65535^2 = 4294836225, 65536^2 overflows.
    assert(isqrt_u32(UINT32_MAX) == 65535u);
    assert(isqrt_u32(65535u * 65535u) == 65535u);
    assert(isqrt_u32(65535u * 65535u + 1) == 65535u);

    // Near boundary
    assert(isqrt_u32(65534u * 65534u) == 65534u);
    assert(isqrt_u32(65534u * 65534u + 65534) == 65534u);
    assert(isqrt_u32(65534u * 65534u + 65535) == 65534u);  // (65535)^2 = (65534+1)^2 = 65534^2 + 2·65534 + 1

    std::cout << "  isqrt_u32: PASS" << std::endl;
}

void test_is_prime_u32() {
    std::cout << "Testing is_prime_u32..." << std::endl;

    auto reference = small_primes_up_to(5000);
    std::set<uint64_t> ref_set(reference.begin(), reference.end());
    for (uint32_t n = 0; n <= 5000; ++n) {
        bool expected = ref_set.contains(n);
        bool got = is_prime_u32(n);
        assert(expected == got);
    }

    // Large 32-bit primes
    assert(is_prime_u32(2147483647u));  // 2^31 - 1
    assert(is_prime_u32(4294967291u));  // 2^32 - 5, largest 32-bit prime
    assert(!is_prime_u32(4294967295u)); // 2^32 - 1 = 3 · 5 · 17 · 257 · 65537

    // Near sqrt boundary (double-rounding pitfall in std::sqrt)
    // p = 4093 is prime, p^2 = 16752649
    assert(is_prime_u32(16752649u) == false);  // it's 4093^2 composite
    assert(is_prime_u32(4093u));               // 4093 itself prime

    std::cout << "  is_prime_u32: PASS" << std::endl;
}

void test_next_prime_u64() {
    std::cout << "Testing next_prime_u64..." << std::endl;

    // First few primes
    assert(next_prime_u64(0) == 2);
    assert(next_prime_u64(1) == 2);
    assert(next_prime_u64(2) == 3);
    assert(next_prime_u64(3) == 5);
    assert(next_prime_u64(4) == 5);
    assert(next_prime_u64(5) == 7);
    assert(next_prime_u64(6) == 7);
    assert(next_prime_u64(7) == 11);

    // Mid-range
    assert(next_prime_u64(100) == 101);
    assert(next_prime_u64(1000) == 1009);
    assert(next_prime_u64(1000000) == 1000003);

    // Large
    assert(next_prime_u64(2147483647ull) == 2147483659ull);  // 2^31-1 is prime; next prime after is 2^31+11

    // Overflow guard: very near UINT64_MAX
    // 18446744073709551557 is the largest 64-bit prime.
    // Searching beyond it must return 0 (sentinel) per the overflow guard.
    uint64_t result = next_prime_u64(18446744073709551557ull);
    // Either no prime exists in [last_prime+1, UINT64_MAX] (returns 0), or
    // if it does, it'd be returned. We just check: never falsely return something > UINT64_MAX or <= input.
    assert(result == 0 || result > 18446744073709551557ull);

    // Hard sentinel: input is exactly UINT64_MAX - some small slop
    assert(next_prime_u64(U64_MAX - 1) == 0);
    assert(next_prime_u64(U64_MAX - 2) == 0);

    std::cout << "  next_prime_u64: PASS" << std::endl;
}

void test_pow_mod_overflow_paranoia() {
    std::cout << "Testing pow_mod_u64 with near-2^64 modulus (overflow paranoia)..." << std::endl;

    // Largest 64-bit prime
    const uint64_t p = 18446744073709551557ull;

    // a^(p-1) ≡ 1 (mod p) by Fermat
    for (uint64_t a : {2ull, 3ull, 5ull, 7ull, 11ull, 13ull, 17ull, 19ull}) {
        uint64_t res = pow_mod_u64(a, p - 1, p);
        if (res != 1) {
            std::cerr << "  pow_mod_u64(" << a << ", p-1, p) = " << res << " (expected 1)" << std::endl;
        }
        assert(res == 1);
    }

    // (p-1)^2 ≡ 1 (mod p) — squaring stress
    assert(pow_mod_u64(p - 1, 2, p) == 1);

    std::cout << "  pow_mod_u64 overflow paranoia: PASS" << std::endl;
}

int main() {
    std::cout << "=== util/primes.hpp tests ===" << std::endl;

    test_mul_mod_u64();
    test_pow_mod_u64();
    test_is_prime_u64_basics();
    test_is_prime_u64_carmichael();
    test_is_prime_u64_large();
    test_pow_mod_overflow_paranoia();
    test_isqrt_u32();
    test_is_prime_u32();
    test_next_prime_u64();

    std::cout << "\n=== All util/primes.hpp tests PASSED ===" << std::endl;
    return 0;
}
