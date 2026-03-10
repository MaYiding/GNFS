#pragma once

#include "gnfs/core/integer.hpp"
#include "gnfs/core/polynomial.hpp"

namespace gnfs::polynomial {

using gnfs::core::Integer;
using gnfs::core::IntPolynomial;
using gnfs::core::PolynomialContext;

/// Base-m polynomial selection method
class BaseMSelector {
public:
    explicit BaseMSelector(const Integer& n);

    /// Select polynomial with given degree
    PolynomialContext select(uint32_t degree);

private:
    Integer n_;
    
    Integer find_m(uint32_t degree);
    IntPolynomial construct_algebraic_poly(const Integer& m, uint32_t degree);
};

/// Convenience function
PolynomialContext select_base_m_polynomial(const Integer& n, uint32_t degree);

} // namespace gnfs::polynomial
