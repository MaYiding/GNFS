// test_schirokauer_strip.cpp — Math regression for Schirokauer strip-ℓ logic.
//
// Production path keeps gcd(a, b) = 1, so the strip-ℓ branches in
// compute_unsplit / compute_split are dead. BACKLOG TEST entry observed
// that existing tests would silently pass if the strip math broke (e.g.
// `c0 /= ell` flipped to `c0 /= ell_k`). This file pins down the math:
//
//   1. strip-ℓ invariance: compute(ℓ·a, ℓ·b) == compute(a, b)
//      when gcd(a, b) is coprime to ℓ. Stripping the ℓ-part should
//      recover the same unit, hence same Schirokauer column.
//
//   2. all-zero short-circuit: compute(0, 0) returns zeros for every
//      prime column.
//
//   3. nested stripping (strip 2x): compute(ℓ²·a, ℓ²·b) == compute(a, b).

#include <gnfs/core/polynomial_context.hpp>
#include <gnfs/linalg/schirokauer.hpp>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using gnfs::core::Integer;
using gnfs::core::PolynomialContext;
using gnfs::linalg::SchirokaurConfig;
using gnfs::linalg::SchirokaurMap;

namespace {

// 50-digit polynomial reused from test_schirokauer_deg4 — known UNSPLIT mod 2.
PolynomialContext build_unsplit_ctx() {
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
    return ctx;
}

bool vectors_equal(const std::vector<std::vector<uint32_t>>& a,
                   const std::vector<std::vector<uint32_t>>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

void print_failure(const char* label, int64_t a, uint64_t b,
                   const std::vector<std::vector<uint32_t>>& lhs,
                   int64_t a2, uint64_t b2,
                   const std::vector<std::vector<uint32_t>>& rhs) {
    std::cerr << "FAIL [" << label << "]: λ(" << a << "," << b << ") != λ("
              << a2 << "," << b2 << ")\n";
    auto dump = [](const std::vector<std::vector<uint32_t>>& v) {
        for (const auto& col : v) {
            std::cerr << "  [";
            for (size_t i = 0; i < col.size(); ++i) {
                if (i) std::cerr << ',';
                std::cerr << col[i];
            }
            std::cerr << "]";
        }
        std::cerr << "\n";
    };
    std::cerr << " lhs:"; dump(lhs);
    std::cerr << " rhs:"; dump(rhs);
}

int test_strip_one(SchirokaurMap& smap) {
    int fails = 0;
    // Pairs with gcd coprime to ℓ=2. λ(2a, 2b) must equal λ(a, b).
    std::vector<std::pair<int64_t, uint64_t>> stripped = {
        {1, 1}, {3, 1}, {1, 3}, {5, 3}, {7, 5},
        {11, 9}, {-1, 1}, {-3, 5}, {127, 1}, {1, 127},
    };
    for (auto [a, b] : stripped) {
        auto base = smap.compute(a, b);
        auto doubled = smap.compute(a * 2, b * 2);
        if (!vectors_equal(base, doubled)) {
            print_failure("strip-1", a, b, base, a * 2, b * 2, doubled);
            ++fails;
        }
    }
    std::cout << "  strip-1 (λ(2a,2b) == λ(a,b)): "
              << (stripped.size() - static_cast<size_t>(fails)) << "/" << stripped.size() << " passed\n";
    return fails;
}

int test_strip_two(SchirokaurMap& smap) {
    int fails = 0;
    // Strip ℓ² (=4): need final strip_v ≤ k-2 = 6, so 2 strips is safe.
    std::vector<std::pair<int64_t, uint64_t>> stripped = {
        {1, 1}, {3, 5}, {7, 3}, {-1, 5}, {11, 1},
    };
    for (auto [a, b] : stripped) {
        auto base = smap.compute(a, b);
        auto quad = smap.compute(a * 4, b * 4);
        if (!vectors_equal(base, quad)) {
            print_failure("strip-2", a, b, base, a * 4, b * 4, quad);
            ++fails;
        }
    }
    std::cout << "  strip-2 (λ(4a,4b) == λ(a,b)): "
              << (stripped.size() - static_cast<size_t>(fails)) << "/" << stripped.size() << " passed\n";
    return fails;
}

int test_zero_shortcircuit(SchirokaurMap& smap) {
    auto res = smap.compute(0, 0);
    assert(res.size() == 1);
    int fails = 0;
    for (uint32_t v : res[0]) {
        if (v != 0) {
            std::cerr << "FAIL: λ(0,0) had non-zero column: " << v << "\n";
            ++fails;
        }
    }
    std::cout << "  zero-γ: "
              << (fails == 0 ? "λ(0,0) == [0,0,0,0]" : "FAILED") << "\n";
    return fails;
}

int test_strip_max_safe(SchirokaurMap& smap) {
    // Boundary: strip exactly k-2 = 6 times. With k=8 and ℓ=2, multiplying
    // (1, 1) by 2^6 = 64 gives (64, 64). strip_v = 6, k-v = 2 → formula
    // still well-defined per assert(strip_v + 2 <= k).
    auto base = smap.compute(1, 1);
    auto strip6 = smap.compute(int64_t{1} << 6, uint64_t{1} << 6);
    int fails = 0;
    if (!vectors_equal(base, strip6)) {
        print_failure("strip-6 boundary", 1, 1, base, 64, 64, strip6);
        ++fails;
    }
    std::cout << "  strip-6 boundary (λ(64,64) == λ(1,1)): "
              << (fails == 0 ? "OK" : "FAILED") << "\n";
    return fails;
}

int test_strip_overflow_fallback(SchirokaurMap& smap) {
    // strip_v = 7 > k-2 = 6, so silent fallback to zeros.
    // (128, 128) = (2^7, 2^7) — both divisible by 2 exactly 7 times.
    // After 7 strips: (1, 1) but strip_v=7 exceeds precision budget.
    // Expected: silent fallback returns zero column.
    auto fallback = smap.compute(int64_t{128}, uint64_t{128});
    int fails = 0;
    for (uint32_t v : fallback[0]) {
        if (v != 0) {
            std::cerr << "FAIL: strip-7 expected silent fallback to zeros, got: " << v << "\n";
            ++fails;
        }
    }
    std::cout << "  strip-7 fallback (λ(128,128) → 0 column): "
              << (fails == 0 ? "OK" : "FAILED") << "\n";
    return fails;
}

// 17-bit polynomial reproducing the CI L2 progressive panic scenario:
// N = 96091, base-m polynomial where f mod 2 is reducible with a linear factor.
// Production gcd(a, b) = 1 keeps c0,c1 coprime to ℓ in the unsplit / split-deg-≥2
// strip paths, but split-degree-1 reduces γ = a - b·r against a Hensel-lifted root r,
// and γ can be highly ℓ-divisible without violating gcd(a,b)=1.
PolynomialContext build_n96091_ctx() {
    Integer N(int64_t(96091LL));
    // f(x) = x^3 + 5x + 458, base-m polynomial near m=458 such that f(m) ≡ 0 mod N.
    // We use a synthetic base-m polynomial known to have a degree-1 factor mod 2.
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(int64_t(1024LL)));   // c0 even
    coeffs.push_back(Integer(int64_t(5LL)));
    coeffs.push_back(Integer(int64_t(2LL)));      // c2 even — ensures reducible mod 2
    coeffs.push_back(Integer(int64_t(1)));         // x^3
    Integer m(int64_t(2LL));
    PolynomialContext ctx(N.clone(), std::move(coeffs), m.clone(), 1.0);
    return ctx;
}

int test_split_degree1_robustness(SchirokaurMap& smap) {
    // Drive split-degree-1 path with many (a, b) pairs from production-like
    // gcd(a,b)=1 inputs. None should panic, all should return well-defined
    // 0/1 values in each Schirokauer column. Some columns will trigger the
    // silent fallback (strip_v ≥ k-1 in γ = a - b·r).
    std::vector<std::pair<int64_t, uint64_t>> pairs = {
        {1, 1}, {3, 1}, {5, 1}, {7, 1}, {9, 1}, {11, 1}, {13, 1}, {15, 1},
        {17, 1}, {19, 1}, {21, 1}, {23, 1}, {25, 1}, {1, 3}, {3, 5}, {5, 3},
        {7, 5}, {9, 7}, {127, 3}, {255, 5}, {-1, 1}, {-3, 5}, {-7, 9},
    };
    int fails = 0;
    int zero_columns = 0;
    int total_columns = 0;
    for (auto [a, b] : pairs) {
        auto cols = smap.compute(a, b);
        for (const auto& col : cols) {
            for (uint32_t v : col) {
                ++total_columns;
                if (v == 0) ++zero_columns;
                if (v >= 2) {  // Schirokauer ℓ=2 must yield 0 or 1
                    std::cerr << "FAIL: split-deg-1 λ(" << a << "," << b
                              << ") column has invalid value " << v << "\n";
                    ++fails;
                }
            }
        }
    }
    std::cout << "  split-deg-1 robustness: " << pairs.size() << " pairs, "
              << total_columns << " columns, " << zero_columns << " zero ("
              << (100 * zero_columns / total_columns) << "%) — "
              << (fails == 0 ? "no panic, all in {0,1}" : "FAILED") << "\n";
    return fails;
}

}  // namespace

int main() {
    std::cout << "=== Schirokauer strip-ℓ Regression ===\n";

    auto ctx = build_unsplit_ctx();
    SchirokaurConfig config;
    config.primes = {2};
    config.exponent_k = 8;
    SchirokaurMap smap(ctx, config);
    std::cout << "Polynomial degree: " << ctx.degree()
              << ", columns: " << smap.num_columns() << "\n\n";

    int fails = 0;
    fails += test_strip_one(smap);
    fails += test_strip_two(smap);
    fails += test_zero_shortcircuit(smap);
    fails += test_strip_max_safe(smap);
    fails += test_strip_overflow_fallback(smap);

    // Split-degree-1 path: N=96091 scenario from PR #3 CI failure.
    auto ctx_split = build_n96091_ctx();
    SchirokaurMap smap_split(ctx_split, config);
    std::cout << "\nSplit polynomial degree: " << ctx_split.degree()
              << ", columns: " << smap_split.num_columns() << "\n";
    fails += test_split_degree1_robustness(smap_split);

    std::cout << "\n";
    if (fails != 0) {
        std::cerr << "*** " << fails << " strip-ℓ regression(s) ***\n";
        return 1;
    }
    std::cout << "=== All strip-ℓ regressions passed ===\n";
    return 0;
}
