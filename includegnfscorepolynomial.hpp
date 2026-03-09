#pragma once

#include "gnfs/core/integer.hpp"
#include <vector>
#include <cstddef>

namespace gnfs::core {

/// Polynomial with Integer coefficients
class IntPolynomial {
public:
    IntPolynomial() = default;
    explicit IntPolynomial(size_t degree);
    IntPolynomial(std::vector<Integer> coeffs);

    // Access
    size_t degree() const;
    const Integer& operator[](size_t i) const;
    Integer& operator[](size_t i);

    // Evaluation
    Integer evaluate(const Integer& x) const;

    // Resize
    void resize(size_t new_degree);
    
    // Normalize (remove leading zeros)
    void normalize();

    const std::vector<Integer>& coefficients() const { return coeffs_; }
    std::vector<Integer>& coefficients() { return coeffs_; }

private:
    std::vector<Integer> coeffs_;
};

/// Polynomial context for GNFS
struct PolynomialContext {
    Integer n;              // Number to factor
    IntPolynomial f;        // Algebraic polynomial
    IntPolynomial g;        // Rational polynomial (usually linear: g(x) = x - m)
    Integer m;              // Common root modulo n
    
    PolynomialContext() = default;
    PolynomialContext(Integer n_, IntPolynomial f_, IntPolynomial g_, Integer m_)
        : n(std::move(n_)), f(std::move(f_)), g(std::move(g_)), m(std::move(m_)) {}
};

} // namespace gnfs::core
