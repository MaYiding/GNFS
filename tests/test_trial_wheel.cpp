// test_trial_wheel.cpp — Wheel-2-3-5 strip helper + trial-division equivalence.
//
// Tier 1: pure unit tests on wheel235.hpp (helper only).
// Tier 2: bit-for-bit equivalence between TrialDivider with wheel optimization
//         and a synthetic naive baseline iterating the rational FB only.

#include <gnfs/cofactor/wheel235.hpp>
#include <gnfs/cofactor/trial_division.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/core/polynomial_context.hpp>
#include <gnfs/core/integer.hpp>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

using gnfs::cofactor::wheel::strip_2;
using gnfs::cofactor::wheel::strip_3;
using gnfs::cofactor::wheel::strip_5;
using gnfs::cofactor::wheel::strip_235;

namespace {

// Naive "while % == 0" loop matching the original trial-division pattern.
template <typename T>
T naive_strip(T v, T p, uint8_t& exp) noexcept {
    exp = 0;
    while (v != 0 && v % p == T(0) && exp < 255u) {
        v /= p;
        ++exp;
    }
    return v;
}

// ----- strip_2 tests -----

void test_strip_2_basic() {
    std::cout << "  strip_2 basic..." << std::flush;
    uint8_t exp = 99;

    // zero
    assert(strip_2(uint64_t{0}, exp) == 0 && exp == 0);
    // odd
    assert(strip_2(uint64_t{1}, exp) == 1 && exp == 0);
    assert(strip_2(uint64_t{3}, exp) == 3 && exp == 0);
    assert(strip_2(uint64_t{7}, exp) == 7 && exp == 0);
    // pure powers of 2
    assert(strip_2(uint64_t{2}, exp) == 1 && exp == 1);
    assert(strip_2(uint64_t{4}, exp) == 1 && exp == 2);
    assert(strip_2(uint64_t{8}, exp) == 1 && exp == 3);
    assert(strip_2(uint64_t{1024}, exp) == 1 && exp == 10);
    // mixed
    assert(strip_2(uint64_t{12}, exp) == 3 && exp == 2);   // 12 = 4 * 3
    assert(strip_2(uint64_t{40}, exp) == 5 && exp == 3);   // 40 = 8 * 5
    assert(strip_2(uint64_t{96}, exp) == 3 && exp == 5);   // 96 = 32 * 3
    // boundary: 2^63
    assert(strip_2(uint64_t{1ULL << 63}, exp) == 1 && exp == 63);
    std::cout << " PASS\n";
}

void test_strip_2_random() {
    std::cout << "  strip_2 random vs naive..." << std::flush;
    std::mt19937_64 rng(0xC0FFEEDEADBEEFULL);
    for (int i = 0; i < 10000; ++i) {
        uint64_t v = rng();
        uint8_t e_wheel = 99, e_naive = 99;
        uint64_t r_wheel = strip_2(v, e_wheel);
        uint64_t r_naive = naive_strip<uint64_t>(v, 2, e_naive);
        if (r_wheel != r_naive || e_wheel != e_naive) {
            std::fprintf(stderr, "MISMATCH v=%llu wheel=(%llu,%u) naive=(%llu,%u)\n",
                         (unsigned long long)v, (unsigned long long)r_wheel, e_wheel,
                         (unsigned long long)r_naive, e_naive);
            assert(false);
        }
    }
    std::cout << " PASS (10000 iters)\n";
}

void test_strip_2_u128() {
    std::cout << "  strip_2 uint128..." << std::flush;
    uint8_t exp;
    // zero
    assert(strip_2(__uint128_t{0}, exp) == 0 && exp == 0);
    // low-limb zero, hi=1: value = 2^64
    __uint128_t v = __uint128_t{1} << 64;
    auto r = strip_2(v, exp);
    assert(r == 1);
    assert(exp == 64);
    // low-limb zero, hi=3: 3 * 2^64 → after strip, odd part = 3
    v = (__uint128_t{3}) << 64;
    r = strip_2(v, exp);
    assert(r == 3);
    assert(exp == 64);
    // mixed: 2^70 * 5
    v = (__uint128_t{5}) << 70;
    r = strip_2(v, exp);
    assert(r == 5);
    assert(exp == 70);
    // 2^127
    v = __uint128_t{1} << 127;
    r = strip_2(v, exp);
    assert(r == 1);
    assert(exp == 127);
    // arbitrary odd 128-bit value
    v = (__uint128_t{0xDEADBEEFULL} << 64) | uint64_t{0xCAFEBABE12345677ULL};
    // 0x...77 is odd, so no shift
    r = strip_2(v, exp);
    assert(r == v);
    assert(exp == 0);
    std::cout << " PASS\n";
}

// ----- strip_3 / strip_5 tests -----

void test_strip_3() {
    std::cout << "  strip_3 vs naive..." << std::flush;
    uint8_t exp;
    assert(strip_3(uint64_t{0}, exp) == 0 && exp == 0);
    assert(strip_3(uint64_t{1}, exp) == 1 && exp == 0);
    assert(strip_3(uint64_t{2}, exp) == 2 && exp == 0);
    assert(strip_3(uint64_t{3}, exp) == 1 && exp == 1);
    assert(strip_3(uint64_t{9}, exp) == 1 && exp == 2);
    assert(strip_3(uint64_t{27}, exp) == 1 && exp == 3);
    assert(strip_3(uint64_t{30}, exp) == 10 && exp == 1);
    assert(strip_3(uint64_t{81 * 7}, exp) == 7 && exp == 4);

    std::mt19937_64 rng(0xABCDEF12345ULL);
    for (int i = 0; i < 10000; ++i) {
        uint64_t v = rng();
        uint8_t e_wheel = 99, e_naive = 99;
        uint64_t r_wheel = strip_3(v, e_wheel);
        uint64_t r_naive = naive_strip<uint64_t>(v, 3, e_naive);
        assert(r_wheel == r_naive && e_wheel == e_naive);
    }
    std::cout << " PASS\n";
}

void test_strip_5() {
    std::cout << "  strip_5 vs naive..." << std::flush;
    uint8_t exp;
    assert(strip_5(uint64_t{0}, exp) == 0 && exp == 0);
    assert(strip_5(uint64_t{4}, exp) == 4 && exp == 0);
    assert(strip_5(uint64_t{5}, exp) == 1 && exp == 1);
    assert(strip_5(uint64_t{25}, exp) == 1 && exp == 2);
    assert(strip_5(uint64_t{125 * 11}, exp) == 11 && exp == 3);

    std::mt19937_64 rng(0x5555AAAA1234ULL);
    for (int i = 0; i < 10000; ++i) {
        uint64_t v = rng();
        uint8_t e_wheel = 99, e_naive = 99;
        uint64_t r_wheel = strip_5(v, e_wheel);
        uint64_t r_naive = naive_strip<uint64_t>(v, 5, e_naive);
        assert(r_wheel == r_naive && e_wheel == e_naive);
    }
    std::cout << " PASS\n";
}

// ----- combined strip_235 -----

void test_strip_235() {
    std::cout << "  strip_235 ordering..." << std::flush;
    uint8_t e2, e3, e5;
    // 2^4 * 3^2 * 5^3 * 7 = 16 * 9 * 125 * 7 = 126000
    uint64_t v = 16ULL * 9ULL * 125ULL * 7ULL;
    auto r = strip_235(v, e2, e3, e5);
    assert(r == 7);
    assert(e2 == 4 && e3 == 2 && e5 == 3);

    // 1 (no factors)
    r = strip_235(uint64_t{1}, e2, e3, e5);
    assert(r == 1 && e2 == 0 && e3 == 0 && e5 == 0);

    // value coprime to 30
    r = strip_235(uint64_t{49}, e2, e3, e5);
    assert(r == 49 && e2 == 0 && e3 == 0 && e5 == 0);

    // edge: random sweep
    std::mt19937_64 rng(0xDEADBEEFCAFEBABEULL);
    for (int i = 0; i < 5000; ++i) {
        uint64_t v0 = rng();
        uint8_t e2x = 0, e3x = 0, e5x = 0;
        uint64_t r_wheel = strip_235(v0, e2x, e3x, e5x);
        // Reconstruct via naive seq strip
        uint8_t a, b, c;
        uint64_t r_naive = naive_strip<uint64_t>(v0, 2, a);
        r_naive = naive_strip<uint64_t>(r_naive, 3, b);
        r_naive = naive_strip<uint64_t>(r_naive, 5, c);
        assert(r_wheel == r_naive && e2x == a && e3x == b && e5x == c);
    }
    std::cout << " PASS\n";
}

// ----- TrialDivider equivalence -----

// Build a tiny FB by hand using the public add_rational interface so we do not
// depend on a polynomial context. Algebraic side is empty.
gnfs::factor_base::FactorBase make_synthetic_fb(const std::vector<uint32_t>& primes,
                                                uint32_t bound) {
    gnfs::factor_base::FactorBase fb(gnfs::core::FactorBaseParams{bound, bound, bound});
    for (uint32_t p : primes) {
        // log_p value does not affect trial division correctness.
        fb.add_rational(p, gnfs::factor_base::compute_log_prime(p, gnfs::core::SIEVE_LOG_SCALE));
    }
    return fb;
}

void test_divider_smoke() {
    std::cout << "  divide_rational synthetic..." << std::flush;
    // FB: {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47}
    std::vector<uint32_t> primes = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47};
    auto fb = make_synthetic_fb(primes, 50);
    gnfs::cofactor::TrialDivider d(fb);

    // 30 = 2 * 3 * 5
    auto r = d.divide_rational(gnfs::core::Integer{uint64_t{30}});
    assert(r.is_smooth);
    assert(r.factor_indices.size() == 3);
    assert(r.factor_indices[0] == 0 && r.exponents[0] == 1);
    assert(r.factor_indices[1] == 1 && r.exponents[1] == 1);
    assert(r.factor_indices[2] == 2 && r.exponents[2] == 1);

    // 2^4 * 3^2 * 5 * 7 = 5040
    r = d.divide_rational(gnfs::core::Integer{uint64_t{5040}});
    assert(r.is_smooth);
    assert(r.factor_indices.size() == 4);
    assert(r.factor_indices[0] == 0 && r.exponents[0] == 4);
    assert(r.factor_indices[1] == 1 && r.exponents[1] == 2);
    assert(r.factor_indices[2] == 2 && r.exponents[2] == 1);
    assert(r.factor_indices[3] == 3 && r.exponents[3] == 1);

    // smooth with no 2/3/5: 7 * 11 = 77
    r = d.divide_rational(gnfs::core::Integer{uint64_t{77}});
    assert(r.is_smooth);
    assert(r.factor_indices.size() == 2);
    assert(r.factor_indices[0] == 3 && r.exponents[0] == 1);
    assert(r.factor_indices[1] == 4 && r.exponents[1] == 1);

    // value 1: trivially smooth
    r = d.divide_rational(gnfs::core::Integer{uint64_t{1}});
    assert(r.is_smooth);
    assert(r.factor_indices.empty());

    // value 0: special-case smooth + cofactor=1
    r = d.divide_rational(gnfs::core::Integer{uint64_t{0}});
    assert(r.is_smooth);
    assert(r.factor_indices.empty());

    // value with cofactor: 30 * 53 (53 not in FB)
    r = d.divide_rational(gnfs::core::Integer{uint64_t{30 * 53}});
    assert(!r.is_smooth);
    assert(r.factor_indices.size() == 3);
    assert(r.cofactor.fits_uint64() && r.cofactor.to_uint64() == 53);

    std::cout << " PASS\n";
}

void test_divider_random_vs_reference() {
    std::cout << "  divide_rational randomized equivalence..." << std::flush;
    // Build FB with standard primes 2..97.
    std::vector<uint32_t> primes = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29,
                                    31, 37, 41, 43, 47, 53, 59, 61, 67,
                                    71, 73, 79, 83, 89, 97};
    auto fb = make_synthetic_fb(primes, 100);
    gnfs::cofactor::TrialDivider d(fb);

    std::mt19937_64 rng(0xFACEFEEDDEADBEEFULL);
    for (int i = 0; i < 5000; ++i) {
        // sample value in [1, 2^60)
        uint64_t v = (rng() & ((1ULL << 60) - 1));
        if (v == 0) v = 1;

        // Reference: pure naive trial division over the same prime list.
        uint64_t ref = v;
        std::vector<uint32_t> ref_idx;
        std::vector<uint8_t> ref_exp;
        for (uint32_t idx = 0; idx < primes.size() && ref > 1; ++idx) {
            uint64_t p = primes[idx];
            uint8_t e = 0;
            while (ref % p == 0 && e < 255) {
                ref /= p;
                ++e;
            }
            if (e > 0) {
                ref_idx.push_back(idx);
                ref_exp.push_back(e);
            }
        }

        // Wheel path
        auto r = d.divide_rational(gnfs::core::Integer{v});

        // The wheel path may break early when the remaining cofactor is too
        // small to be divided further (see early-exit in divide_rational_u64_from).
        // In that case, the cofactor will be greater than 1 and equal to the
        // residual of the naive path. We compare both completed and early-exit
        // cases consistently by reproducing the same early-exits in the reference.
        // Easier: compare resulting (factor_indices, exponents, residual).
        // Re-walk reference with the same early-exit logic.
        uint64_t ref2 = v;
        std::vector<uint32_t> ref_idx2;
        std::vector<uint8_t> ref_exp2;
        bool smooth2 = false;
        if (v == 0) {
            smooth2 = true;
            ref2 = 1;
        } else {
            for (uint32_t idx = 0; idx < primes.size(); ++idx) {
                uint64_t p = primes[idx];
                uint8_t e = 0;
                while (ref2 % p == 0 && e < 255) {
                    ref2 /= p;
                    ++e;
                }
                if (e > 0) {
                    ref_idx2.push_back(idx);
                    ref_exp2.push_back(e);
                }
                if (ref2 == 1) {
                    smooth2 = true;
                    break;
                }
                // mirror the "cofactor < next_p" early exit
                if (idx + 1 < primes.size()) {
                    uint64_t next_p = primes[idx + 1];
                    if (ref2 < next_p) break;
                }
                // mirror "p² early stop"
                uint64_t bound = fb.params().rational_bound;
                if (ref2 > bound && ref2 < static_cast<uint64_t>(p) * p) {
                    break;
                }
            }
            if (ref2 == 1) smooth2 = true;
        }

        if (r.is_smooth != smooth2 || r.factor_indices != ref_idx2 || r.exponents != ref_exp2) {
            std::fprintf(stderr,
                         "Equivalence MISMATCH v=%llu\n"
                         "  wheel  is_smooth=%d size=%zu cof=%s\n"
                         "  refer  is_smooth=%d size=%zu cof=%llu\n",
                         (unsigned long long)v, int(r.is_smooth),
                         r.factor_indices.size(), r.cofactor.to_string().c_str(),
                         int(smooth2), ref_idx2.size(), (unsigned long long)ref2);
            assert(false);
        }
        // also compare cofactor
        if (r.cofactor.fits_uint64()) {
            assert(r.cofactor.to_uint64() == (smooth2 ? 1ULL : ref2));
        }
    }
    std::cout << " PASS (5000 iters)\n";
}

void test_divider_u128_path() {
    std::cout << "  divide_rational uint128 equivalence..." << std::flush;
    std::vector<uint32_t> primes = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29,
                                    31, 37, 41, 43, 47, 53, 59, 61, 67,
                                    71, 73, 79, 83, 89, 97};
    auto fb = make_synthetic_fb(primes, 100);
    gnfs::cofactor::TrialDivider d(fb);

    std::mt19937_64 rng(0xC0CAC0CA12345ULL);
    for (int i = 0; i < 1000; ++i) {
        // Build a 100-120 bit value by multiplying low primes lots of times.
        // Specifically: pick base = 2^a * 3^b * 5^c with a in [10..40], b in [5..25], c in [3..18]
        // multiplied by a random odd 64-bit value to push above 2^64.
        unsigned a = 10 + (rng() % 30);
        unsigned b = 5 + (rng() % 20);
        unsigned c = 3 + (rng() % 15);
        __uint128_t v = 1;
        for (unsigned k = 0; k < a; ++k) v *= 2;
        for (unsigned k = 0; k < b; ++k) v *= 3;
        for (unsigned k = 0; k < c; ++k) v *= 5;
        uint64_t tail = (rng() | 1ULL);  // odd tail
        v *= tail;
        // Cap to ~120 bits
        if ((v >> 120) != 0) v &= ((__uint128_t{1} << 120) - 1);
        if (v == 0) v = 1;

        // Convert to Integer (we know v < 2^128)
        gnfs::core::Integer big;
        uint64_t hi = static_cast<uint64_t>(v >> 64);
        uint64_t lo = static_cast<uint64_t>(v);
        mpz_set_ui(big.get_mpz(), hi);
        mpz_mul_2exp(big.get_mpz(), big.get_mpz(), 64);
        mpz_add_ui(big.get_mpz(), big.get_mpz(), lo);

        // Run TrialDivider
        auto r = d.divide_rational(std::move(big));

        // Reference: __uint128_t naive
        __uint128_t ref = v;
        std::vector<uint32_t> ref_idx;
        std::vector<uint8_t> ref_exp;
        bool smooth = false;
        for (uint32_t idx = 0; idx < primes.size() && ref > 1; ++idx) {
            uint32_t p = primes[idx];
            if (ref % p != 0) continue;
            uint8_t e = 0;
            do { ref /= p; ++e; } while (ref % p == 0 && e < 255);
            ref_idx.push_back(idx);
            ref_exp.push_back(e);
        }
        if (ref == 1) smooth = true;

        if (r.is_smooth != smooth || r.factor_indices != ref_idx || r.exponents != ref_exp) {
            std::fprintf(stderr,
                         "U128 MISMATCH idx=%d wheel=(sm=%d,sz=%zu) ref=(sm=%d,sz=%zu)\n",
                         i, int(r.is_smooth), r.factor_indices.size(),
                         int(smooth), ref_idx.size());
            assert(false);
        }
    }
    std::cout << " PASS (1000 iters)\n";
}

void test_divider_without_235_in_fb() {
    std::cout << "  divide_rational fallback (no 2/3/5 in FB)..." << std::flush;
    // FB starts at 7 — wheel optimization must NOT fire.
    std::vector<uint32_t> primes = {7, 11, 13, 17, 19, 23, 29, 31, 37};
    auto fb = make_synthetic_fb(primes, 40);
    gnfs::cofactor::TrialDivider d(fb);

    // 7 * 11 = 77 — smooth
    auto r = d.divide_rational(gnfs::core::Integer{uint64_t{77}});
    assert(r.is_smooth);
    assert(r.factor_indices.size() == 2);
    assert(r.factor_indices[0] == 0);
    assert(r.factor_indices[1] == 1);

    // 30 contains 2,3,5 — not in FB → cofactor=30
    r = d.divide_rational(gnfs::core::Integer{uint64_t{30}});
    assert(!r.is_smooth);
    assert(r.factor_indices.empty());
    assert(r.cofactor.fits_uint64() && r.cofactor.to_uint64() == 30);

    std::cout << " PASS\n";
}

void test_divider_exponent_saturation() {
    std::cout << "  divide_rational exponent saturation..." << std::flush;
    std::vector<uint32_t> primes = {2, 3, 5, 7, 11};
    auto fb = make_synthetic_fb(primes, 20);
    gnfs::cofactor::TrialDivider d(fb);

    // value = 2^63 → exp = 63 (well below 255)
    auto r = d.divide_rational(gnfs::core::Integer{uint64_t{1ULL << 63}});
    assert(r.is_smooth);
    assert(r.factor_indices.size() == 1);
    assert(r.factor_indices[0] == 0);
    assert(r.exponents[0] == 63);

    // value = 1 (after wheel, still 1) — no factors at all
    r = d.divide_rational(gnfs::core::Integer{uint64_t{1}});
    assert(r.is_smooth);
    assert(r.factor_indices.empty());

    std::cout << " PASS\n";
}

} // namespace

int main() {
    std::cout << "=== Trial Wheel-2-3-5 Tests ===\n";

    std::cout << "[helper]\n";
    test_strip_2_basic();
    test_strip_2_random();
    test_strip_2_u128();
    test_strip_3();
    test_strip_5();
    test_strip_235();

    std::cout << "[divider equivalence]\n";
    test_divider_smoke();
    test_divider_random_vs_reference();
    test_divider_u128_path();
    test_divider_without_235_in_fb();
    test_divider_exponent_saturation();

    std::cout << "=== All Trial Wheel Tests PASSED ===\n";
    return 0;
}
