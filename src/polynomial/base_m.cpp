#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/polynomial/murphy_evaluator.hpp"
#include "gnfs/polynomial/polynomial_optimizer.hpp"
#include "gnfs/core/params.hpp"
#include "gnfs/sqrt/modular_poly.hpp"

#include <stdexcept>


namespace gnfs::polynomial {

namespace {

/// Construct base-m polynomial of n with given degree.
/// Guarantees f(m) = n for any m > 1.
/// Returns polynomial with degree <= `degree` (may be less if m is too large).
IntPolynomial construct_base_m_poly(const Integer& n, const Integer& m, uint32_t degree) {
    IntPolynomial f(0);
    Integer temp = n.clone();

    // Extract d lower-order base-m digits
    for (uint32_t i = 0; i < degree; ++i) {
        Integer coeff;
        Integer::divmod(temp, coeff, temp, m);
        f[i] = std::move(coeff);
    }
    // Leading coefficient gets everything remaining → guarantees f(m) = n
    f[degree] = std::move(temp);
    f.normalize();
    return f;
}

/// Check if integer polynomial f is likely irreducible over Q[x]
/// by testing irreducibility mod several small primes (Rabin test).
/// Eisenstein criterion 快速判定:
/// 若存在素数 p 使得:
///   - p ∤ a_d (leading)
///   - p | a_i for i = 0, ..., d-1
///   - p² ∤ a_0
/// 则 f 在 Q 上不可约。
bool eisenstein_check(const IntPolynomial& f) {
    uint32_t d = f.degree();
    if (d <= 1) return false;

    // 取 a_0 的小素因子作候选(满足 p | a_0 是必要条件)
    Integer a0 = f[0];
    if (a0.is_zero()) return false;
    if (a0.is_negative()) a0.negate();

    // 试小素数 p 是否满足完整 Eisenstein (mpz_divisible_ui_p zero-alloc)
    constexpr uint64_t small_primes[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31};
    for (uint64_t p : small_primes) {
        // p ∤ a_d
        if (mpz_divisible_ui_p(f[d].get_mpz(), p)) continue;

        // p | a_i for 0 ≤ i < d
        bool all_div = true;
        for (uint32_t i = 0; i < d; ++i) {
            if (!mpz_divisible_ui_p(f[i].get_mpz(), p)) { all_div = false; break; }
        }
        if (!all_div) continue;

        // p² ∤ a_0 (p ≤ 31, p² ≤ 961 fits ulong easily)
        if (mpz_divisible_ui_p(f[0].get_mpz(), p * p)) continue;

        return true;  // Eisenstein with this p → irreducible
    }
    return false;
}

/// If f mod p is irreducible over GF(p) for any prime p (not dividing
/// the leading coefficient), then f is definitely irreducible over Q.
bool check_irreducible_over_Q(const IntPolynomial& f) {
    uint32_t d = f.degree();
    if (d <= 1) return true;

    // Eisenstein 准则:满足时直接判定不可约,极快(d 次除法)。
    // base-m 多项式形如 f(m) = N,系数 (a_0, a_1, ..., a_d) 通常没明显
    // 公共素因子,Eisenstein 命中率不高,但作为 hot-path 早出。
    if (eisenstein_check(f)) return true;

    // 15 primes: for degree 6, false-negative rate ≈ (5/6)^15 ≈ 6.5%.
    // Combined with 11 m-perturbations, overall miss rate is negligible.
    constexpr uint64_t test_primes[] = {
        2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47
    };

    for (uint64_t p : test_primes) {
        std::vector<uint64_t> f_mod_p(d + 1);
        // hoist p_int + c_buf out of d+1 loop
        Integer p_int(p);
        Integer c_buf;
        for (uint32_t i = 0; i <= d; ++i) {
            c_buf = f[i];  // mpz_set into reused buffer
            c_buf %= p_int;
            if (c_buf.is_negative()) c_buf += p_int;
            f_mod_p[i] = c_buf.to_uint64();
        }
        // Skip if leading coefficient vanishes mod p (degree drops)
        if (f_mod_p[d] == 0) continue;

        if (gnfs::sqrt::ModularPoly::is_irreducible(f_mod_p, p)) {
            return true;
        }
    }
    return false;
}

} // anonymous namespace

BaseMSelector::BaseMSelector(const Integer& n) : n_(n.clone()) {}

PolynomialSelectionResult BaseMSelector::select(const Integer& n, uint32_t degree) {
    // Compute m_base ≈ n^(1/degree)
    Integer m_base;
    mpz_root(m_base.get_mpz(), n.get_mpz(), degree);

    // Search window scales with N's size:
    //   ≤45 bit: ±5 (11 candidates, old behavior — covers L1-L5 tests)
    //   46-60 bit: ±50
    //   61-90 bit: ±200
    //   91+ bit: ±1000
    size_t n_bits = mpz_sizeinbase(n.get_mpz(), 2);
    int max_delta;
    if (n_bits <= 45) {
        max_delta = 5;
    } else if (n_bits <= 150) {
        max_delta = 50;   // 101 candidates — fast with degree 3
    } else if (n_bits <= 250) {
        max_delta = 200;  // more exploration for medium N (degree 4-5)
    } else {
        max_delta = 500;  // large N — Kleinjung handles most cases
    }

    // Collect all irreducible candidates in the search window
    struct Candidate {
        Integer m;
        IntPolynomial f;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<size_t>(2 * max_delta + 1));

    for (int delta = -max_delta; delta <= max_delta; ++delta) {
        Integer m = m_base;
        m += static_cast<int64_t>(delta);
        if (mpz_cmp_si(m.get_mpz(), 1) <= 0) continue;

        auto f = construct_base_m_poly(n, m, degree);
        if (f.degree() != degree) continue;

        if (check_irreducible_over_Q(f)) {
            candidates.push_back({std::move(m), std::move(f)});
        }
    }

    if (candidates.empty()) {
        // Fallback: use m_base (overwhelmingly likely irreducible)
        PolynomialSelectionResult result;
        result.degree = degree;
        result.m = m_base.clone();
        result.f = construct_base_m_poly(n, m_base, degree);
        result.success = true;
        return result;
    }

    // Small window or single candidate: return first (backward compat)
    if (max_delta <= 5 || candidates.size() == 1) {
        PolynomialSelectionResult result;
        result.degree = degree;
        result.m = std::move(candidates[0].m);
        result.f = std::move(candidates[0].f);
        result.success = true;
        return result;
    }

    // Rank by Murphy E-score for larger search windows.
    // For ≤85 bit: ultra-light Murphy (relative ranking only, not absolute).
    // For larger N: standard parameters.
    MurphyParams mparams;
    if (n_bits <= 85) {
        mparams.alpha_bound = 500;     // 95 primes (was 303)
        mparams.sample_points = 100;   // was 500
    } else {
        mparams.alpha_bound = 2000;    // 303 primes
        mparams.sample_points = 500;
    }
    auto gnfs_params = core::GNFSParams::compute(n_bits);
    mparams.smoothness_bound = gnfs_params.algebraic_bound;

    MurphyEvaluator evaluator(mparams);

    size_t best_idx = 0;
    double best_log_e = -1e100;

    for (size_t i = 0; i < candidates.size(); ++i) {
        // g(x) = x - m
        IntPolynomial g(0);
        Integer neg_m = candidates[i].m;  // copy ctor (mpz_init_set)
        neg_m.negate();
        g[0] = std::move(neg_m);
        g[1] = int64_t(1);  // mpz_set_si into existing slot (skip Integer tmp)

        double skewness = PolynomialOptimizer::estimate_skewness(candidates[i].f);
        auto score = evaluator.compute(candidates[i].f, g, n, skewness);

        if (score.log_e_score > best_log_e) {
            best_log_e = score.log_e_score;
            best_idx = i;
        }
    }

    PolynomialSelectionResult result;
    result.degree = degree;
    result.m = std::move(candidates[best_idx].m);
    result.f = std::move(candidates[best_idx].f);
    result.success = true;
    return result;
}

PolynomialContext BaseMSelector::create_context(const Integer& n, const PolynomialSelectionResult& result) {
    if (!result.success) {
        throw std::runtime_error("Cannot create context from failed selection");
    }

    // Extract coefficients from IntPolynomial
    std::vector<Integer> f_coeffs;
    f_coeffs.reserve(result.f.degree() + 1);
    for (size_t i = 0; i <= result.f.degree(); ++i) {
        f_coeffs.push_back(result.f[i].clone());
    }

    // Compute skewness from polynomial coefficients: s ≈ (c_0 / c_d)^{1/d}
    double skewness = PolynomialOptimizer::estimate_skewness(result.f);

    return PolynomialContext(n.clone(), std::move(f_coeffs), result.m.clone(), skewness);
}

PolynomialContext BaseMSelector::select_poly(uint32_t degree) {
    auto result = select(n_, degree);
    return create_context(n_, result);
}

Integer BaseMSelector::find_m(uint32_t degree) {
    // m ≈ n^(1/degree)
    Integer m_lower;
    mpz_root(m_lower.get_mpz(), n_.get_mpz(), degree);
    return m_lower;
}

IntPolynomial BaseMSelector::construct_algebraic_poly(const Integer& m, uint32_t degree) {
    return construct_base_m_poly(n_, m, degree);
}

PolynomialContext select_base_m_polynomial(const Integer& n, uint32_t degree) {
    BaseMSelector selector(n);
    return selector.select_poly(degree);
}

} // namespace gnfs::polynomial
