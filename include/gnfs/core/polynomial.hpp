#pragma once

#include "gnfs/core/integer.hpp"
#include "gnfs/core/polynomial_context.hpp"
#include "gnfs/polynomial/int_polynomial.hpp"
#include <vector>
#include <cstddef>

namespace gnfs::core {

// IntPolynomial is now defined in gnfs/polynomial/int_polynomial.hpp
// This provides backward compatibility alias
using IntPolynomial = gnfs::polynomial::IntPolynomial;

// Note: PolynomialContext is defined in polynomial_context.hpp

} // namespace gnfs::core
