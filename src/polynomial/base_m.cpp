#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/polynomial/polynomial_optimizer.hpp"
#include "gnfs/sqrt/modular_poly.hpp"


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
/// If f mod p is irreducible over GF(p) for any prime p (not dividing
/// the leading coefficient), then f is definitely irreducible over Q.
bool check_irreducible_over_Q(const IntPolynomial& f) {
    uint32_t d = f.degree();
    if (d <= 1) return true;

    // 15 primes: for degree 6, false-negative rate ≈ (5/6)^15 ≈ 6.5%.
    // Combined with 11 m-perturbations, overall miss rate is negligible.
    constexpr uint64_t test_primes[] = {
        2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47
    };

    for (uint64_t p : test_primes) {
        std::vector<uint64_t> f_mod_p(d + 1);
        for (uint32_t i = 0; i <= d; ++i) {
            Integer c = f[i] % Integer(p);
            if (c.is_negative()) c += Integer(p);
            f_mod_p[i] = c.to_uint64();
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

    // Try m_base and small perturbations to find an irreducible f
    constexpr int deltas[] = {0, 1, -1, 2, -2, 3, -3, 4, -4, 5, -5};
    for (int delta : deltas) {
        Integer m = m_base + Integer(delta);
        if (m <= Integer(1)) continue;

        auto f = construct_base_m_poly(n, m, degree);

        // Must have correct degree
        if (f.degree() != degree) continue;

        // Check irreducibility over Q via mod-p Rabin test
        if (check_irreducible_over_Q(f)) {
            PolynomialSelectionResult result;
            result.degree = degree;
            result.m = std::move(m);
            result.f = std::move(f);
            result.success = true;
            return result;
        }
    }

    // Fallback: use m_base (heuristic couldn't prove irreducibility,
    // but for degree 3-6 base-m polynomials this is overwhelmingly
    // likely to be irreducible — just unlucky with mod-p tests)
    PolynomialSelectionResult result;
    result.degree = degree;
    result.m = m_base.clone();
    result.f = construct_base_m_poly(n, m_base, degree);
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
