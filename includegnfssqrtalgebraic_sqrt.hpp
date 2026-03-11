#pragma once

#include "gnfs/core/integer.hpp"
#include "gnfs/core/relation.hpp"
#include "gnfs/core/polynomial.hpp"
#include <vector>

namespace gnfs::sqrt {

using gnfs::core::Integer;
using gnfs::core::Relation;
using gnfs::core::PolynomialContext;

/// Compute square root on algebraic side
class AlgebraicSqrt {
public:
    AlgebraicSqrt(const PolynomialContext& ctx);
    
    /// Compute square root from dependency
    Integer compute(const std::vector<Relation>& relations, const std::vector<bool>& dependency);
    
private:
    PolynomialContext ctx_;
};

} // namespace gnfs::sqrt
