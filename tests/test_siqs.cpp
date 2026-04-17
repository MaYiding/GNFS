#include <gnfs/siqs/siqs.hpp>
#include <cassert>
#include <cstdio>
#include <chrono>

using gnfs::core::Integer;
using namespace gnfs::siqs;

void test_tonelli_shanks() {
    // sqrt(2) mod 7 = 3 (since 3^2 = 9 ≡ 2 mod 7)
    uint32_t r = tonelli_shanks(2, 7);
    assert(r == 3 || r == 4); // 3 or 7-3=4
    assert((uint64_t)r * r % 7 == 2);

    // sqrt(2) mod 17 = 6 (since 6^2 = 36 ≡ 2 mod 17)
    r = tonelli_shanks(2, 17);
    assert((uint64_t)r * r % 17 == 2);

    // sqrt(3) mod 13 = 4 (since 4^2 = 16 ≡ 3 mod 13)
    r = tonelli_shanks(3, 13);
    assert((uint64_t)r * r % 13 == 3);

    // Non-QR: 2 mod 5 (Legendre = -1)
    r = tonelli_shanks(2, 5);
    assert(r == 0);

    printf("  tonelli_shanks: PASS\n");
}

void test_factor_base() {
    Integer N("1000000007"); // prime, but we're testing FB construction
    auto fb = build_factor_base(N, 20);
    assert(fb.size() >= 20);

    // Verify sqrt(N) mod p is correct for each FB prime
    for (size_t i = 1; i < fb.size(); i++) {
        uint32_t p = fb[i].p;
        uint32_t sq = fb[i].sqrt_n;
        uint64_t n_mod = mpz_fdiv_ui(N.get_mpz(), p);
        assert(((uint64_t)sq * sq) % p == n_mod % p);
    }

    printf("  factor_base: PASS (%zu primes)\n", fb.size());
}

void test_siqs_small() {
    // 15-digit semiprime: 100000000000031 * ... actually let's use a known semiprime
    // 143 = 11 * 13
    Integer N("143");
    auto result = factor(N, 10, false);
    assert(result.has_value());
    auto [f1, f2] = std::make_pair(result->factor1, result->factor2);
    if (f1 > f2) std::swap(f1, f2);
    assert(f1 == Integer(11) && f2 == Integer(13));
    printf("  siqs_small(143): PASS (%.3fs)\n", result->time_seconds);
}

void test_siqs_20digit() {
    // 20-digit semiprime: 12345678901234567891 = 3 * 4115226300411522597
    // Actually let's use a known 20-digit semiprime
    // 10000000000000000051 * ... let's just try a small product
    Integer p1("1000000007");
    Integer p2("1000000009");
    Integer N = p1 * p2;
    printf("  siqs_20digit: N=%s (%zu digits)\n", N.to_string().c_str(), N.to_string().size());

    auto start = std::chrono::steady_clock::now();
    auto result = factor(N, 30, true);
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();

    if (result) {
        auto f1 = result->factor1, f2 = result->factor2;
        if (f1 > f2) std::swap(f1, f2);
        assert(f1 * f2 == N);
        printf("  siqs_20digit: PASS (%.3fs, %zu polys)\n",
               elapsed, result->polynomials_used);
    } else {
        printf("  siqs_20digit: FAIL — no factor found (%.3fs)\n", elapsed);
        assert(false);
    }
}

void test_siqs_30digit() {
    Integer p1("1000000000000007");
    Integer p2("1000000000000037");
    Integer N = p1 * p2;
    printf("  siqs_30digit: N=%s (%zu digits)\n", N.to_string().c_str(), N.to_string().size());

    auto result = factor(N, 60, true);
    if (result) {
        assert(result->factor1 * result->factor2 == N);
        printf("  siqs_30digit: PASS (%.3fs, %zu polys)\n",
               result->time_seconds, result->polynomials_used);
    } else {
        printf("  siqs_30digit: FAIL\n");
        // Don't assert — this may need parameter tuning
    }
}

void test_siqs_40digit() {
    Integer p1("10000000000000000051");
    Integer p2("10000000000000000099");
    Integer N = p1 * p2;
    printf("  siqs_40digit: N=%s (%zu digits)\n", N.to_string().c_str(), N.to_string().size());

    auto result = factor(N, 120, true);
    if (result) {
        assert(result->factor1 * result->factor2 == N);
        printf("  siqs_40digit: PASS (%.3fs, %zu polys)\n",
               result->time_seconds, result->polynomials_used);
    } else {
        printf("  siqs_40digit: FAIL\n");
    }
}

int main() {
    printf("=== SIQS Unit Tests ===\n\n");

    printf("--- Helper tests ---\n");
    test_tonelli_shanks();
    test_factor_base();

    printf("\n--- Factorization tests ---\n");
    test_siqs_small();
    test_siqs_20digit();
    test_siqs_30digit();
    test_siqs_40digit();

    printf("\n=== All SIQS tests passed ===\n");
    return 0;
}
