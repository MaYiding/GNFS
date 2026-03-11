#pragma once

#include "gnfs/core/integer.hpp"
#include "gnfs/core/polynomial.hpp"

namespace gnfs::polynomial {

using gnfs::core::Integer;
using gnfs::core::IntPolynomial;
using gnfs::core::PolynomialContext;

/// Murphy E score evaluator for polynomial quality
class MurphyEvaluator {
public:
    MurphyEvaluator() = default;
    
    /// Compute Murphy E score (log scale)
    double evaluate(const PolynomialContext& ctx);
    
private:
    double alpha_f(const IntPolynomial& f);
    double alpha_g(const IntPolynomial& g);
};

} // namespace gnfs::polynomial
