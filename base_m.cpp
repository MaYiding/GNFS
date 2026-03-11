#include "gnfs/polynomial/base_m.hpp"
#include <cmath>

namespace gnfs::polynomial {

BaseMSelector::BaseMSelector(const Integer& n) : n_(n.clone()) {}

PolynomialSelectionResult BaseMSelector::select(const Integer& n, uint32_t degree) {
    PolynomialSelectionResult result;
    result.degree = degree;
    
    // Compute m ≈ n^(1/degree)
    Integer m;
    mpz_root(m.get_mpz(), n.get_mpz(), degree);
    result.m = m.clone();
    
    // Construct algebraic polynomial
    IntPolynomial f(degree);
    Integer temp = n.clone();
    
    for (uint32_t i = 0; i <= degree; ++i) {
        Integer coeff;
        Integer::divmod(temp, coeff, temp, m);
        f[i] = std::move(coeff);
    }
    
    // Add high-degree coefficient if needed
    if (!temp.is_zero()) {
        f[degree] = std::move(temp);
    }
    
    f.normalize();
    result.f = std::move(f);
    result.success = true;
    
    return result;
}

PolynomialContext BaseMSelector::create_context(const Integer& n, const PolynomialSelectionResult& result) {
    if (!result.success) {
        throw std::runtime_error("Cannot create context from failed selection");
    }
    
    // Rational polynomial: g(x) = x - m
    IntPolynomial g(1);
    g[0] = -result.m.clone();
    g[1] = Integer(1);
    
    return PolynomialContext(n.clone(), result.f, std::move(g), result.m.clone());
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
    // Construct f(x) such that f(m) ≡ 0 (mod n)
    // Using base-m representation of n
    
    IntPolynomial f(degree);
    Integer temp = n_.clone();
    
    for (uint32_t i = 0; i <= degree; ++i) {
        Integer coeff;
        Integer::divmod(temp, coeff, temp, m);
        f[i] = std::move(coeff);
    }
    
    // Add high-degree coefficient if needed
    if (!temp.is_zero()) {
        f[degree] = std::move(temp);
    }
    
    return f;
}

PolynomialContext select_base_m_polynomial(const Integer& n, uint32_t degree) {
    BaseMSelector selector(n);
    return selector.select_poly(degree);
}

} // namespace gnfs::polynomial
