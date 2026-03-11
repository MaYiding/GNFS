#pragma once

#include "gnfs/core/integer.hpp"
#include "gnfs/core/polynomial.hpp"
#include "gnfs/polynomial/murphy_evaluator.hpp"

namespace gnfs::polynomial {

using gnfs::core::Integer;
using gnfs::core::IntPolynomial;
using gnfs::core::PolynomialContext;

/// Kleinjung polynomial selection algorithm
class KleinjungSelector {
public:
    explicit KleinjungSelector(const Integer& n);
    
    /// Select polynomial with given degree
    PolynomialContext select(uint32_t degree, size_t num_candidates = 100);
    
private:
    Integer n_;
    MurphyEvaluator murphy_;
    
    std::vector<PolynomialContext> generate_candidates(uint32_t degree, size_t count);
    PolynomialContext select_best(const std::vector<PolynomialContext>& candidates);
};

/// Convenience function
PolynomialContext select_kleinjung_polynomial(const Integer& n, uint32_t degree, size_t num_candidates = 100);

} // namespace gnfs::polynomial
