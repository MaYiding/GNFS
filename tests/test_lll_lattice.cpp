// test_lll_lattice.cpp — F-K 2005 LLL lattice basis reduction tests
//
// Verifies the LLL implementation in include/gnfs/sieve/lattice_basis.hpp:
//   1. Mathematical invariants (size-reduced + Lovasz)
//   2. Determinant preservation (|det| == q)
//   3. Caller API stability (e0/f0 = shorter, e1/f1 = longer)
//   4. LLL never worse than Gauss in |b0|^2 + |b1|^2
//   5. Asymmetric r ~ q/2 cases (LLL expected to outperform Gauss)
//   6. Boundary cases: r = 0, r = 1, r = q-1
//   7. Large q close to uint32_t upper bound
//   8. Cross-check verify_ab on both basis vectors

#include "gnfs/sieve/lattice_basis.hpp"
#include "gnfs/sieve/special_q.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

using namespace gnfs::sieve;

namespace {

using i128 = __int128_t;

[[nodiscard]] i128 norm_sq_i128(int64_t a, int64_t b) noexcept {
    i128 a128 = a, b128 = b;
    return a128 * a128 + b128 * b128;
}

[[nodiscard]] i128 dot_i128(int64_t a0, int64_t b0, int64_t a1, int64_t b1) noexcept {
    return static_cast<i128>(a0) * a1 + static_cast<i128>(b0) * b1;
}

[[nodiscard]] i128 abs_i128(i128 x) noexcept { return x < 0 ? -x : x; }

/// Print 128-bit integer to stderr (for assertion failure debugging).
void print_i128(i128 x) {
    if (x < 0) {
        std::cerr << "-";
        x = -x;
    }
    if (x == 0) {
        std::cerr << "0";
        return;
    }
    char buf[64];
    int pos = 0;
    while (x > 0) {
        buf[pos++] = static_cast<char>('0' + (x % 10));
        x /= 10;
    }
    while (pos > 0) std::cerr << buf[--pos];
}

/// Verify the size-reduction invariant: |2 * (v0.v1)| <= |v0|^2.
/// Equivalent to |mu| = |v0.v1/|v0|^2| <= 1/2.
[[nodiscard]] bool is_size_reduced(const LatticeBasis& basis) {
    // basis.e0/f0 is the shorter v0, basis.e1/f1 is the longer v1
    i128 n0 = norm_sq_i128(basis.e0, basis.f0);
    if (n0 == 0) return true;  // degenerate
    i128 d = dot_i128(basis.e0, basis.f0, basis.e1, basis.f1);
    // |2*d| <= n0
    return abs_i128(2 * d) <= n0;
}

/// Verify Lovasz condition with delta = 1 (LLL strict optimal in 2D).
/// |v1|^2 >= |v0|^2 (after size-reduction; v0 = shorter).
[[nodiscard]] bool satisfies_lovasz(const LatticeBasis& basis) {
    i128 n0 = norm_sq_i128(basis.e0, basis.f0);
    i128 n1 = norm_sq_i128(basis.e1, basis.f1);
    return n1 >= n0;
}

/// Caller API: e0/f0 = shorter, e1/f1 = longer.
[[nodiscard]] bool e0_is_shorter(const LatticeBasis& basis) {
    return norm_sq_i128(basis.e0, basis.f0) <= norm_sq_i128(basis.e1, basis.f1);
}

[[nodiscard]] bool det_equals_q(const LatticeBasis& basis) {
    int64_t det = basis.determinant();
    int64_t q = static_cast<int64_t>(basis.q);
    return det == q || det == -q;
}

[[nodiscard]] SpecialQ make_sq(uint32_t q, uint32_t r) {
    SpecialQ sq;
    sq.q = q;
    sq.r = r;
    sq.index = 0;
    return sq;
}

}  // anonymous namespace

// ─── Test 1: basic LLL correctness on small primes ───────────────────

void test_lll_basic() {
    std::cout << "Testing LLL basic correctness..." << std::endl;

    // Small primes covering diverse r values
    std::vector<std::pair<uint32_t, uint32_t>> cases = {
        {2, 1}, {3, 1}, {5, 2}, {7, 3}, {11, 5}, {13, 4}, {17, 8},
        {101, 42}, {1009, 500}, {99991, 12345},
    };

    for (auto [q, r] : cases) {
        SpecialQ sq = make_sq(q, r);
        auto basis = compute_lattice_basis(sq, LatticeReductionMethod::LLL);

        // determinant invariant
        assert(det_equals_q(basis));
        // both basis vectors must satisfy a - b*r ≡ 0 (mod q)
        assert(basis.verify_ab(basis.e0, basis.f0));
        assert(basis.verify_ab(basis.e1, basis.f1));
        // caller API: e0 = shorter
        assert(e0_is_shorter(basis));
        // size-reduced
        assert(is_size_reduced(basis));
        // Lovasz (delta=1, 2D optimal)
        assert(satisfies_lovasz(basis));
    }

    std::cout << "  PASS (10 small primes, all invariants hold)" << std::endl;
}

// ─── Test 2: large q close to uint32 upper bound ────────────────────

void test_lll_large_q() {
    std::cout << "Testing LLL with large q (close to 2^32)..." << std::endl;

    // q chosen to stress the __int128_t intermediate computation
    // (q^2 ~ 2^64 > 2^63, so int64_t alone would overflow).
    std::vector<std::pair<uint32_t, uint32_t>> cases = {
        {2'000'003u, 1'000'001u},   // 2M
        {16'777'259u, 8'388'629u},  // 16M ~ 2^24
        {268'435'459u, 134'217'729u}, // 268M ~ 2^28
        {1'073'741'827u, 536'870'913u}, // 2^30
        // Largest case below 2^32 (q^2 fits in __int128_t but not int64_t)
        {2'147'483'647u, 1'073'741'823u}, // 2^31 - 1 (Mersenne prime)
    };

    for (auto [q, r] : cases) {
        SpecialQ sq = make_sq(q, r);
        auto basis = compute_lattice_basis(sq, LatticeReductionMethod::LLL);

        assert(det_equals_q(basis));
        assert(basis.verify_ab(basis.e0, basis.f0));
        assert(basis.verify_ab(basis.e1, basis.f1));
        assert(e0_is_shorter(basis));
        assert(is_size_reduced(basis));
        assert(satisfies_lovasz(basis));

        // |b0|^2 should be O(q), not O(q^2). For ideal LLL, |b0| ~ sqrt(q).
        i128 n0 = norm_sq_i128(basis.e0, basis.f0);
        i128 q128 = static_cast<i128>(q);
        // |b0|^2 should be <= 2*q (loose upper bound, theory: <= 4/3 * q)
        if (n0 > 2 * q128) {
            std::cerr << "    [WARN] q=" << q << " r=" << r << " |b0|^2=";
            print_i128(n0);
            std::cerr << " > 2q=" << (2 * q) << std::endl;
            // Don't assert — this is a quality metric, not correctness
        }
    }

    std::cout << "  PASS (5 large q values up to 2^31)" << std::endl;
}

// ─── Test 3: asymmetric r ~ q/2 (LLL key benefit) ───────────────────

void test_lll_asymmetric_r() {
    std::cout << "Testing LLL on asymmetric r ~ q/2 (key F-K 2005 benefit)..." << std::endl;

    // Asymmetric special-q where Gauss may produce suboptimal basis
    std::vector<uint32_t> qs = {1009, 10007, 100003, 1'000'003u, 10'000'019u};
    int lll_beats_gauss = 0;
    int gauss_better = 0;
    int equal = 0;

    for (uint32_t q : qs) {
        // Test multiple r values near q/2
        std::vector<uint32_t> rs = {
            q / 2, q / 2 - 1, q / 2 + 1, q / 2 - 7, q / 2 + 13,
        };
        for (uint32_t r : rs) {
            if (r == 0 || r >= q) continue;

            SpecialQ sq = make_sq(q, r);
            auto lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
            auto gauss = compute_lattice_basis(sq, LatticeReductionMethod::Gauss);

            // Both must be valid
            assert(det_equals_q(lll));
            assert(det_equals_q(gauss));

            i128 lll_total = norm_sq_i128(lll.e0, lll.f0) + norm_sq_i128(lll.e1, lll.f1);
            i128 gauss_total = norm_sq_i128(gauss.e0, gauss.f0) + norm_sq_i128(gauss.e1, gauss.f1);

            if (lll_total < gauss_total) ++lll_beats_gauss;
            else if (lll_total > gauss_total) ++gauss_better;
            else ++equal;

            // Critical invariant: LLL never produces strictly worse basis
            // (because Gauss is also size-reduced + 2D Lovasz subset).
            // If gauss_total < lll_total, we have a bug.
            // Allow equality (most common case in 2D).
            if (gauss_total < lll_total) {
                std::cerr << "    [FAIL] q=" << q << " r=" << r
                          << " gauss="; print_i128(gauss_total);
                std::cerr << " lll="; print_i128(lll_total);
                std::cerr << " (LLL is worse)" << std::endl;
                assert(false && "LLL produced worse basis than Gauss");
            }
        }
    }

    std::cout << "  LLL_better=" << lll_beats_gauss
              << " equal=" << equal
              << " gauss_better=" << gauss_better
              << " (LLL must be >= Gauss; allow equal in 2D)" << std::endl;
    std::cout << "  PASS (asymmetric r ~ q/2 invariants hold)" << std::endl;
}

// ─── Test 4: boundary cases — r = 0, r = 1, r = q-1 ─────────────────

void test_lll_boundary_cases() {
    std::cout << "Testing LLL boundary cases (r=0, r=1, r=q-1)..." << std::endl;

    std::vector<uint32_t> qs = {3, 5, 7, 11, 97, 1009, 99991, 1'000'003u};

    for (uint32_t q : qs) {
        // Case r = 0: lattice is { (a, b) : a ≡ 0 (mod q) }
        // Optimal basis: (0, 1), (q, 0). |b0|^2 = 1, |b1|^2 = q^2.
        {
            SpecialQ sq = make_sq(q, 0);
            auto basis = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
            assert(det_equals_q(basis));
            assert(basis.verify_ab(basis.e0, basis.f0));
            assert(basis.verify_ab(basis.e1, basis.f1));
            assert(e0_is_shorter(basis));
            assert(is_size_reduced(basis));
            assert(satisfies_lovasz(basis));
            // For r=0, optimal |b0|^2 = 1 (the (0, 1) vector).
            i128 n0 = norm_sq_i128(basis.e0, basis.f0);
            assert(n0 == 1);
        }

        // Case r = 1: lattice is { (a, b) : a ≡ b (mod q) }
        // Optimal basis: (1, 1), then second short vector.
        {
            SpecialQ sq = make_sq(q, 1);
            auto basis = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
            assert(det_equals_q(basis));
            assert(basis.verify_ab(basis.e0, basis.f0));
            assert(basis.verify_ab(basis.e1, basis.f1));
            assert(e0_is_shorter(basis));
            assert(is_size_reduced(basis));
            assert(satisfies_lovasz(basis));
            // For r=1, |b0|^2 should be very small (= 2 for (1,1) vector).
            i128 n0 = norm_sq_i128(basis.e0, basis.f0);
            assert(n0 <= 4);  // (1,1) → 2, or some near-equivalent
        }

        // Case r = q-1: known to oscillate in legacy Gauss path (BACKLOG P2).
        // LLL must still terminate and produce valid basis.
        {
            SpecialQ sq = make_sq(q, q - 1);
            auto basis = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
            // Determinant must hold (it is the strongest mathematical invariant)
            assert(det_equals_q(basis));
            // Caller API: shorter first
            assert(e0_is_shorter(basis));
            // Lattice membership (a - b*r ≡ 0 mod q)
            assert(basis.verify_ab(basis.e0, basis.f0));
            assert(basis.verify_ab(basis.e1, basis.f1));
            // size-reduction + Lovasz must hold for LLL (this is the
            // critical improvement over Gauss for r=q-1)
            assert(is_size_reduced(basis));
            assert(satisfies_lovasz(basis));
        }
    }

    std::cout << "  PASS (8 q values x 3 boundary cases, all invariants)" << std::endl;
}

// ─── Test 5: LLL <= Gauss in basis quality ──────────────────────────

void test_lll_dominates_gauss() {
    std::cout << "Testing LLL never produces worse basis than Gauss..." << std::endl;

    // Systematic sweep
    int total = 0;
    int lll_strictly_better = 0;
    int equal = 0;

    std::vector<uint32_t> qs = {1009, 10007, 100003, 1'000'003u};
    for (uint32_t q : qs) {
        // Sample diverse r values
        std::vector<uint32_t> sample_rs;
        for (uint32_t frac = 1; frac <= 9; ++frac) {
            uint32_t r = (q * frac) / 10;
            if (r > 0 && r < q) sample_rs.push_back(r);
        }
        // Add small / large extremes
        sample_rs.push_back(1);
        sample_rs.push_back(q - 1);
        sample_rs.push_back(2);
        sample_rs.push_back(q / 2);

        for (uint32_t r : sample_rs) {
            if (r == 0 || r >= q) continue;
            SpecialQ sq = make_sq(q, r);
            auto lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
            auto gauss = compute_lattice_basis(sq, LatticeReductionMethod::Gauss);

            i128 lll_total = norm_sq_i128(lll.e0, lll.f0) + norm_sq_i128(lll.e1, lll.f1);
            i128 gauss_total = norm_sq_i128(gauss.e0, gauss.f0) + norm_sq_i128(gauss.e1, gauss.f1);

            ++total;
            if (lll_total < gauss_total) {
                ++lll_strictly_better;
            } else if (lll_total == gauss_total) {
                ++equal;
            } else {
                std::cerr << "    [FAIL] q=" << q << " r=" << r
                          << " gauss="; print_i128(gauss_total);
                std::cerr << " lll="; print_i128(lll_total);
                std::cerr << " (LLL is worse)" << std::endl;
                assert(false && "LLL must dominate Gauss");
            }
        }
    }

    std::cout << "  total=" << total
              << " lll_strictly_better=" << lll_strictly_better
              << " equal=" << equal
              << " (LLL >= Gauss invariant holds)" << std::endl;
    std::cout << "  PASS" << std::endl;
}

// ─── Test 6: ENV gate (GNFS_LATTICE_LLL) ────────────────────────────

void test_lll_env_gate() {
    std::cout << "Testing GNFS_LATTICE_LLL ENV gate..." << std::endl;

    SpecialQ sq = make_sq(99991u, 12345u);

    // Default (no ENV) = LLL
    unsetenv("GNFS_LATTICE_LLL");
    {
        auto basis_default = compute_lattice_basis(sq);
        auto basis_lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
        // Should be byte-identical (same algorithm)
        assert(basis_default.e0 == basis_lll.e0);
        assert(basis_default.f0 == basis_lll.f0);
        assert(basis_default.e1 == basis_lll.e1);
        assert(basis_default.f1 == basis_lll.f1);
    }

    // GNFS_LATTICE_LLL=0 -> Gauss
    setenv("GNFS_LATTICE_LLL", "0", 1);
    {
        auto basis_env_gauss = compute_lattice_basis(sq);
        auto basis_gauss = compute_lattice_basis(sq, LatticeReductionMethod::Gauss);
        assert(basis_env_gauss.e0 == basis_gauss.e0);
        assert(basis_env_gauss.f0 == basis_gauss.f0);
        assert(basis_env_gauss.e1 == basis_gauss.e1);
        assert(basis_env_gauss.f1 == basis_gauss.f1);
    }

    // GNFS_LATTICE_LLL=1 -> LLL
    setenv("GNFS_LATTICE_LLL", "1", 1);
    {
        auto basis_env_lll = compute_lattice_basis(sq);
        auto basis_lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
        assert(basis_env_lll.e0 == basis_lll.e0);
        assert(basis_env_lll.f0 == basis_lll.f0);
        assert(basis_env_lll.e1 == basis_lll.e1);
        assert(basis_env_lll.f1 == basis_lll.f1);
    }

    // Restore default
    unsetenv("GNFS_LATTICE_LLL");

    std::cout << "  PASS (default=LLL, ENV=0->Gauss, ENV=1->LLL)" << std::endl;
}

// ─── Test 7: random property sweep (fuzz-style) ─────────────────────

void test_lll_random_sweep() {
    std::cout << "Testing LLL random property sweep (200 random q,r pairs)..." << std::endl;

    // Deterministic small-LCG (no <random> for portability)
    uint64_t state = 0xC0FFEE12345ull;
    auto next = [&]() -> uint64_t {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return state >> 32;
    };

    int count = 0;
    for (int i = 0; i < 200; ++i) {
        uint32_t q = static_cast<uint32_t>((next() % 10'000'000u) + 1009u);
        uint32_t r = static_cast<uint32_t>(next() % q);
        if (r == 0) r = 1;

        SpecialQ sq = make_sq(q, r);
        auto basis = compute_lattice_basis(sq, LatticeReductionMethod::LLL);

        assert(det_equals_q(basis));
        assert(basis.verify_ab(basis.e0, basis.f0));
        assert(basis.verify_ab(basis.e1, basis.f1));
        assert(e0_is_shorter(basis));
        assert(is_size_reduced(basis));
        assert(satisfies_lovasz(basis));
        ++count;
    }

    std::cout << "  PASS (" << count << "/200 random pairs, all invariants)" << std::endl;
}

// ─── Test 8: norm reduction quality benchmark ───────────────────────

void test_lll_norm_quality() {
    std::cout << "Testing LLL norm quality (avg |b0|^2 + |b1|^2)..." << std::endl;

    std::vector<uint32_t> qs = {100003u, 1'000'003u, 10'000'019u};

    for (uint32_t q : qs) {
        double total_lll = 0.0;
        double total_gauss = 0.0;
        int n = 0;

        // Sample 20 r values evenly spaced
        for (uint32_t k = 1; k < 20; ++k) {
            uint32_t r = (q / 20) * k;
            if (r == 0 || r >= q) continue;

            SpecialQ sq = make_sq(q, r);
            auto lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
            auto gauss = compute_lattice_basis(sq, LatticeReductionMethod::Gauss);

            i128 lll_total = norm_sq_i128(lll.e0, lll.f0) + norm_sq_i128(lll.e1, lll.f1);
            i128 gauss_total = norm_sq_i128(gauss.e0, gauss.f0) + norm_sq_i128(gauss.e1, gauss.f1);

            total_lll += static_cast<double>(static_cast<int64_t>(lll_total));
            total_gauss += static_cast<double>(static_cast<int64_t>(gauss_total));
            ++n;
        }

        double avg_lll = total_lll / n;
        double avg_gauss = total_gauss / n;
        double ratio = avg_gauss / avg_lll;
        std::cout << "  q=" << q
                  << " avg |b0|^2+|b1|^2 (LLL)=" << avg_lll
                  << " (Gauss)=" << avg_gauss
                  << " ratio Gauss/LLL=" << ratio << std::endl;
        // LLL never worse (ratio >= 1, with floating tolerance)
        assert(ratio >= 0.999);
    }

    std::cout << "  PASS (quality ratio Gauss/LLL >= 1.0 for all q)" << std::endl;
}

// ─── Test 9: SkewLLL invariants ─────────────────────────────────────

void test_skew_lll_invariants() {
    std::cout << "Testing SkewLLL invariants (skewness sweep)..." << std::endl;

    // Skewness range typical for GNFS polynomials: 1.0 to ~5000.
    std::vector<double> skewnesses = {
        1.0,    // unskewed (equivalent to LLL)
        2.0,
        5.0,
        10.0,
        100.0,
        1000.0,
        5000.0,
    };

    std::vector<std::pair<uint32_t, uint32_t>> cases = {
        {1009, 500}, {99991, 12345}, {1'000'003u, 500'000u},
        {10'000'019u, 5'000'000u},
    };

    int total = 0;
    int skew_better = 0;
    int equal_skew = 0;
    int skew_worse = 0;

    for (double s : skewnesses) {
        for (auto [q, r] : cases) {
            SpecialQ sq = make_sq(q, r);
            auto basis = compute_lattice_basis(sq, LatticeReductionMethod::SkewLLL, s);
            auto plain_lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);

            // Core invariants (must hold for any reduction):
            assert(det_equals_q(basis));
            assert(basis.verify_ab(basis.e0, basis.f0));
            assert(basis.verify_ab(basis.e1, basis.f1));

            // For s=1.0, SkewLLL should be bit-identical to LLL (dispatched).
            if (s == 1.0) {
                assert(basis.e0 == plain_lll.e0);
                assert(basis.f0 == plain_lll.f0);
                assert(basis.e1 == plain_lll.e1);
                assert(basis.f1 == plain_lll.f1);
            }

            // Quality metric: skew norm² (a² + s²·b²)
            // SkewLLL should produce smaller skew norms than plain LLL when s != 1.
            auto skew_n = [s](int64_t a, int64_t b) {
                double da = static_cast<double>(a), db = static_cast<double>(b);
                return da * da + s * s * db * db;
            };
            double skew_total_skewlll = skew_n(basis.e0, basis.f0) + skew_n(basis.e1, basis.f1);
            double skew_total_plain = skew_n(plain_lll.e0, plain_lll.f0)
                                    + skew_n(plain_lll.e1, plain_lll.f1);

            ++total;
            if (skew_total_skewlll < skew_total_plain * 0.9999) ++skew_better;
            else if (skew_total_skewlll > skew_total_plain * 1.0001) ++skew_worse;
            else ++equal_skew;

            // Hard invariant: SkewLLL never substantially worse in skew norm
            // (allow small double error: ratio < 1.01)
            assert(skew_total_skewlll <= skew_total_plain * 1.01);
        }
    }

    std::cout << "  total=" << total
              << " skew_better=" << skew_better
              << " equal=" << equal_skew
              << " skew_worse=" << skew_worse
              << " (SkewLLL <= LLL in skew norm)" << std::endl;
    std::cout << "  PASS" << std::endl;
}

// ─── Test 10: SkewLLL boundary cases ────────────────────────────────

void test_skew_lll_boundary() {
    std::cout << "Testing SkewLLL boundary cases (large skewness, r=0/q-1)..." << std::endl;

    // Very large skewness (5000+, e.g., 50d high-skew polynomial)
    std::vector<double> extreme_s = {0.5, 1.0, 1.1, 100.0, 5000.0, 50000.0};
    std::vector<std::pair<uint32_t, uint32_t>> cases = {
        {1009, 0}, {1009, 1}, {1009, 1008},
        {99991, 0}, {99991, 1}, {99991, 99990},
        {1'000'003u, 0}, {1'000'003u, 500'001u}, {1'000'003u, 1'000'002u},
    };

    for (double s : extreme_s) {
        for (auto [q, r] : cases) {
            SpecialQ sq = make_sq(q, r);
            auto basis = compute_lattice_basis(sq, LatticeReductionMethod::SkewLLL, s);

            assert(det_equals_q(basis));
            assert(basis.verify_ab(basis.e0, basis.f0));
            assert(basis.verify_ab(basis.e1, basis.f1));
        }
    }

    std::cout << "  PASS (6 skewnesses x 9 cases, all invariants)" << std::endl;
}

// ─── Test 11: compute_lattice_basis_with_skewness dispatch ──────────

void test_skew_lll_dispatch() {
    std::cout << "Testing compute_lattice_basis_with_skewness ENV interaction..." << std::endl;

    SpecialQ sq = make_sq(1'000'003u, 500'000u);

    // Clean ENV first
    unsetenv("GNFS_LATTICE_LLL");
    unsetenv("GNFS_LATTICE_SKEW");

    // ENV default (SKEW unset) + skewness=1.0 -> LLL (no skew upgrade)
    {
        auto basis = compute_lattice_basis_with_skewness(sq, 1.0);
        auto lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
        assert(basis.e0 == lll.e0 && basis.e1 == lll.e1);
    }

    // ENV default (SKEW unset) + skewness=10.0 -> still LLL (default OFF for skew)
    {
        auto basis = compute_lattice_basis_with_skewness(sq, 10.0);
        auto lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
        assert(basis.e0 == lll.e0 && basis.e1 == lll.e1);
    }

    // SKEW=1 + skewness=10.0 -> SkewLLL
    setenv("GNFS_LATTICE_SKEW", "1", 1);
    {
        auto basis = compute_lattice_basis_with_skewness(sq, 10.0);
        auto skew = compute_lattice_basis(sq, LatticeReductionMethod::SkewLLL, 10.0);
        assert(basis.e0 == skew.e0 && basis.e1 == skew.e1);
    }

    // SKEW=1 + skewness=1.0 -> LLL (skew=1 degenerate)
    {
        auto basis = compute_lattice_basis_with_skewness(sq, 1.0);
        auto lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
        assert(basis.e0 == lll.e0 && basis.e1 == lll.e1);
    }
    unsetenv("GNFS_LATTICE_SKEW");

    // SKEW=1 + LLL=0 (Gauss) + skewness=10.0 -> Gauss (LLL gate dominates, skew ignored)
    setenv("GNFS_LATTICE_SKEW", "1", 1);
    setenv("GNFS_LATTICE_LLL", "0", 1);
    {
        auto basis = compute_lattice_basis_with_skewness(sq, 10.0);
        auto gauss = compute_lattice_basis(sq, LatticeReductionMethod::Gauss);
        assert(basis.e0 == gauss.e0 && basis.e1 == gauss.e1);
    }
    unsetenv("GNFS_LATTICE_LLL");
    unsetenv("GNFS_LATTICE_SKEW");

    std::cout << "  PASS (dispatch: default off, ENV opt-in, LLL gate dominates)" << std::endl;
}

int main() {
    std::cout << "===========================================" << std::endl;
    std::cout << "  F-K 2005 LLL Lattice Reduction Tests" << std::endl;
    std::cout << "===========================================" << std::endl;

    test_lll_basic();
    test_lll_large_q();
    test_lll_asymmetric_r();
    test_lll_boundary_cases();
    test_lll_dominates_gauss();
    test_lll_env_gate();
    test_lll_random_sweep();
    test_lll_norm_quality();
    test_skew_lll_invariants();
    test_skew_lll_boundary();
    test_skew_lll_dispatch();

    std::cout << "===========================================" << std::endl;
    std::cout << "  All LLL lattice tests passed!" << std::endl;
    std::cout << "===========================================" << std::endl;
    return 0;
}
