#include "gnfs/polynomial/base_m.hpp"
#include <cmath>

namespace gnfs::polynomial {

BaseMSelector::BaseMSelector(const Integer& n) : n_(n.clone()) {}

PolynomialContext BaseMSelector::select(uint32_t degree) {
    Integer m = find_m(degree);
    IntPolynomial f = construct_algebraic_poly(m, degree);
    
    // Rational polynomial: g(x) = x - m
    IntPolynomial g(1);
    g[0] = -m.clone();
    g[1] = Integer(1);
    
    return PolynomialContext(n_.clone(), std::move(f), std::move(g), m.clone());
}

Integer BaseMSelector::find_m(uint32_t degree) {
    // m ≈ n^(1/degree)
    double n_log = n_.bit_length() * std::log(2.0);
    double m_log = n_log / degree;
    double m_approx = std::exp(m_log);
    
    // Use GMP's nth root function
    Integer m_lower;
    mpz_root(m_lower.get_mpz(), n_.get_mpz(), degree);
    
    // Search around this value for best m
    Integer best_m = m_lower.clone();
    
    // For simplicity, just use the nth root
    return best_m;
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
    return selector.select(degree);
}

} // namespace gnfs::polynomial
