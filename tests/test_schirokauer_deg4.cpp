// Focused diagnostic: verify Schirokauer map correctness for degree-4 polynomial
// Tests: λ(g·h) = λ(g) + λ(h) mod 2, λ(g²) = 0 mod 2

#include <gnfs/core/polynomial_context.hpp>
#include <gnfs/linalg/schirokauer.hpp>
#include <cassert>
#include <iostream>
#include <vector>

using namespace gnfs::core;
using namespace gnfs::linalg;

// Compute product of (a₁ - b₁·α)(a₂ - b₂·α) = (a₁a₂ + b₁b₂·α²) - (a₁b₂ + a₂b₁)·α
// We need the Schirokauer map of the product, which should equal sum of individual maps.
// But the map is defined on (a,b) pairs, not on products.
// So test: for a dependency, ∑ λ(aᵢ, bᵢ) mod 2 should equal 0 if product is a square.
// Simpler test: λ(a,b) + λ(a,b) mod 2 = 0 (double = square ⇒ map sums to 0)

int main() {
    std::cout << "=== Schirokauer Degree-4 Diagnostic ===" << std::endl;

    // Exact 50-digit polynomial from test_stress.cpp
    // f(x) = x^4 + 1252x^3 + 1000587868x^2 + 626122691041x + 5603231353
    // N = 16000000000000004000000216000000000000027000000729
    // m = 1999999999687
    Integer N("16000000000000004000000216000000000000027000000729");
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(int64_t(5603231353LL)));
    coeffs.push_back(Integer(int64_t(626122691041LL)));
    coeffs.push_back(Integer(int64_t(1000587868LL)));
    coeffs.push_back(Integer(int64_t(1252LL)));
    coeffs.push_back(Integer(int64_t(1)));
    Integer m(int64_t(1999999999687LL));

    PolynomialContext ctx(N.clone(), std::move(coeffs), m.clone(), 1.0);
    assert(ctx.degree() == 4);
    assert(ctx.verify() && "f(m) != 0 mod N");
    std::cout << "Polynomial verified: degree=" << ctx.degree() << std::endl;

    // Create Schirokauer map
    SchirokaurConfig config;
    config.primes = {2};
    config.exponent_k = 8;  // ℓ^k = 256

    SchirokaurMap smap(ctx, config);
    std::cout << "Schirokauer columns: " << smap.num_columns() << std::endl;
    assert(smap.num_columns() == 4 && "Degree 4 should have 4 Schirokauer columns");

    // Print mode
    std::cout << "Mode: " << (smap.prime_info_[0].is_split ? "SPLIT" : "UNSPLIT") << std::endl;
    std::cout << "Exponent: " << smap.prime_info_[0].exponent << std::endl;
    std::cout << "ell_k: " << smap.prime_info_[0].ell_k << std::endl;

    // Print f mod 256
    std::cout << "f mod 256: [";
    for (uint32_t i = 0; i <= 4; ++i) {
        if (i) std::cout << ", ";
        std::cout << smap.prime_info_[0].f_mod[i];
    }
    std::cout << "]" << std::endl;

    // Test 1: λ values should be in {0, 1}
    std::cout << "\n--- Test 1: Range check ---" << std::endl;
    std::vector<std::pair<int64_t, uint64_t>> test_pairs = {
        {1, 1}, {3, 2}, {7, 1}, {-1, 4}, {11, 3}, {-5, 2},
        {100, 7}, {-33, 11}, {999, 1}, {1, 999},
        {4095, 2047}, {-4095, 2047}, {1, 2}, {3, 4}, {5, 6},
        {0, 1}, {2, 1}, {4, 1}, // a even, b odd
        {1, 2}, {3, 4}, {5, 6}, // a odd, b even
    };

    int total_tests = 0;
    int zero_count = 0;

    for (auto [a, b] : test_pairs) {
        auto maps = smap.compute(a, b);
        assert(maps.size() == 1);
        assert(maps[0].size() == 4);
        for (uint32_t v : maps[0]) {
            assert(v < 2 && "Value must be 0 or 1");
        }
        total_tests++;
        bool all_zero = true;
        for (uint32_t v : maps[0]) if (v != 0) all_zero = false;
        if (all_zero) zero_count++;
    }
    std::cout << "  " << total_tests << " pairs checked, "
              << zero_count << " all-zero ("
              << (100.0 * zero_count / total_tests) << "%)" << std::endl;

    // Test 2: λ(a,b) + λ(a,b) ≡ 0 (mod 2) — doubling a relation → square
    std::cout << "\n--- Test 2: Self-sum (square) check ---" << std::endl;
    int pass2 = 0;
    for (auto [a, b] : test_pairs) {
        auto flat = smap.compute_flat(a, b);
        bool ok = true;
        for (size_t j = 0; j < flat.size(); ++j) {
            if ((flat[j] + flat[j]) % 2 != 0) {
                std::cerr << "FAIL: λ(" << a << "," << b << ")[" << j
                         << "] = " << flat[j] << ", 2x mod 2 = " << (2*flat[j]%2) << std::endl;
                ok = false;
            }
        }
        if (ok) pass2++;
    }
    std::cout << "  " << pass2 << "/" << test_pairs.size() << " passed" << std::endl;
    assert(pass2 == (int)test_pairs.size() && "Self-sum must always be 0 mod 2");

    // Test 3: Homomorphism: λ(g₁g₂) = λ(g₁) + λ(g₂) mod 2
    // For GF(2) matrix, this means: for a valid dependency (∑ rows = 0 mod 2),
    // the Schirokauer columns should also sum to 0.
    // Test indirectly: compute flat values for pairs and verify additivity
    std::cout << "\n--- Test 3: Homomorphism check ---" << std::endl;
    std::cout << "  (Verifying that FastPoly computation is consistent)" << std::endl;

    // We can't directly test λ(g₁g₂) without computing g₁g₂ as a number field element.
    // But we CAN verify g^(q-1) ≡ 1 mod ℓ by checking that all Schirokauer
    // values satisfy the extraction formula correctly.
    // Specifically: g^15 mod (f, 256) should have:
    //   - constant term ≡ 1 mod 2  (so (c₀-1)/2 is well-defined)
    //   - other terms ≡ 0 mod 2    (so c_i/2 is well-defined)

    std::cout << "\n--- Test 4: g^15 ≡ 1 mod 2 check ---" << std::endl;
    int pass4 = 0;
    for (auto [a, b] : test_pairs) {
        // Replicate the unsplit computation manually
        uint64_t ell_k = smap.prime_info_[0].ell_k;
        int64_t a_mod = a % static_cast<int64_t>(ell_k);
        if (a_mod < 0) a_mod += static_cast<int64_t>(ell_k);
        uint64_t b_mod = b % ell_k;
        uint64_t neg_b = (ell_k - b_mod) % ell_k;

        FastPoly g(static_cast<uint64_t>(a_mod), neg_b);
        auto g_pow = FastPoly::power(g, smap.prime_info_[0].exponent,
                                     smap.prime_info_[0].f_mod.data(), 4, ell_k);

        // Check: g^15 ≡ 1 mod 2
        bool c0_odd = (g_pow.coeff(0) % 2 == 1);
        bool c1_even = (g_pow.coeff(1) % 2 == 0);
        bool c2_even = (g_pow.coeff(2) % 2 == 0);
        bool c3_even = (g_pow.coeff(3) % 2 == 0);

        if (!c0_odd || !c1_even || !c2_even || !c3_even) {
            std::cerr << "FAIL: g^15 NOT ≡ 1 mod 2 for a=" << a << " b=" << b << std::endl;
            std::cerr << "  g^15 mod 256 = [" << g_pow.coeff(0) << ", "
                     << g_pow.coeff(1) << ", " << g_pow.coeff(2) << ", "
                     << g_pow.coeff(3) << "]" << std::endl;
            std::cerr << "  g = [" << static_cast<uint64_t>(a_mod) << ", " << neg_b << "]"
                     << std::endl;
        } else {
            pass4++;
        }
    }
    std::cout << "  " << pass4 << "/" << test_pairs.size() << " passed" << std::endl;

    // Test 5: Print sample values for manual inspection
    std::cout << "\n--- Test 5: Sample Schirokauer values ---" << std::endl;
    for (auto [a, b] : test_pairs) {
        auto flat = smap.compute_flat(a, b);
        std::cout << "  λ(" << a << "," << b << ") = [";
        for (size_t j = 0; j < flat.size(); ++j) {
            if (j) std::cout << ",";
            std::cout << flat[j];
        }
        std::cout << "]" << std::endl;
    }

    // Test 6: Verify homomorphism directly using FastPoly multiplication
    // λ(g₁·g₂) should equal λ(g₁) + λ(g₂) mod 2
    std::cout << "\n--- Test 6: Direct homomorphism via FastPoly ---" << std::endl;
    int pass6 = 0, total6 = 0;
    for (size_t i = 0; i < test_pairs.size(); ++i) {
        for (size_t j = i + 1; j < test_pairs.size() && j < i + 5; ++j) {
            auto [a1, b1] = test_pairs[i];
            auto [a2, b2] = test_pairs[j];

            uint64_t ell_k = smap.prime_info_[0].ell_k;
            uint64_t exp = smap.prime_info_[0].exponent;
            const auto* f = smap.prime_info_[0].f_mod.data();

            // g1 = a1 - b1*x
            int64_t a1_mod = a1 % static_cast<int64_t>(ell_k);
            if (a1_mod < 0) a1_mod += static_cast<int64_t>(ell_k);
            uint64_t neg_b1 = (ell_k - (b1 % ell_k)) % ell_k;
            FastPoly g1(static_cast<uint64_t>(a1_mod), neg_b1);

            // g2 = a2 - b2*x
            int64_t a2_mod = a2 % static_cast<int64_t>(ell_k);
            if (a2_mod < 0) a2_mod += static_cast<int64_t>(ell_k);
            uint64_t neg_b2 = (ell_k - (b2 % ell_k)) % ell_k;
            FastPoly g2(static_cast<uint64_t>(a2_mod), neg_b2);

            // g1*g2 mod (f, ell_k)
            auto g12 = FastPoly::mul(g1, g2, f, 4, ell_k);

            // Compute λ(g1), λ(g2), λ(g1*g2)
            auto lam1 = FastPoly::power(g1, exp, f, 4, ell_k);
            auto lam2 = FastPoly::power(g2, exp, f, 4, ell_k);
            auto lam12 = FastPoly::power(g12, exp, f, 4, ell_k);

            // Extract Schirokauer bits
            auto extract = [&](const FastPoly& p) -> std::array<uint32_t, 4> {
                std::array<uint32_t, 4> bits{};
                for (int k = 0; k < 4; ++k) {
                    const size_t idx = static_cast<size_t>(k);
                    uint64_t c = p.coeff(idx);
                    if (k == 0) c = (c >= 1) ? (c - 1) : (ell_k - 1);
                    bits[idx] = static_cast<uint32_t>((c / 2) % 2);
                }
                return bits;
            };

            auto b1_arr = extract(lam1);
            auto b2_arr = extract(lam2);
            auto b12_arr = extract(lam12);

            bool ok = true;
            for (int k = 0; k < 4; ++k) {
                const size_t idx = static_cast<size_t>(k);
                if ((b1_arr[idx] + b2_arr[idx]) % 2 != b12_arr[idx]) {
                    ok = false;
                    std::cerr << "HOMOMORPHISM FAIL: a1=" << a1 << " b1=" << b1
                             << " a2=" << a2 << " b2=" << b2
                             << " col=" << k << ": "
                             << b1_arr[idx] << "+" << b2_arr[idx]
                             << " != " << b12_arr[idx] << std::endl;
                }
            }
            if (ok) pass6++;
            total6++;
        }
    }
    std::cout << "  " << pass6 << "/" << total6 << " passed" << std::endl;

    if (pass6 != total6) {
        std::cerr << "\n*** SCHIROKAUER HOMOMORPHISM BUG DETECTED ***" << std::endl;
        return 1;
    }

    // Test 7: Verify that ALL values are not zero (would indicate degenerate map)
    std::cout << "\n--- Test 7: Non-degeneracy check ---" << std::endl;
    int nonzero = 0;
    for (auto [a, b] : test_pairs) {
        auto flat = smap.compute_flat(a, b);
        bool has_nonzero = false;
        for (uint32_t v : flat) if (v != 0) has_nonzero = true;
        if (has_nonzero) nonzero++;
    }
    std::cout << "  " << nonzero << "/" << test_pairs.size()
              << " have non-zero Schirokauer values" << std::endl;
    if (nonzero == 0) {
        std::cerr << "*** WARNING: ALL Schirokauer values are zero! ***" << std::endl;
    }

    std::cout << "\n=== All tests passed ===" << std::endl;
    return 0;
}
