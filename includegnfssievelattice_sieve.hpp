#pragma once

#include "gnfs/core/relation.hpp"
#include "gnfs/core/polynomial.hpp"
#include "gnfs/factor_base/builder.hpp"
#include <vector>

namespace gnfs::sieve {

using gnfs::core::Relation;
using gnfs::core::PolynomialContext;
using gnfs::factor_base::FactorBase;

/// Lattice sieve for GNFS
class LatticeSieve {
public:
    LatticeSieve(const PolynomialContext& ctx, const FactorBase& fb);
    
    /// Sieve region for relations
    std::vector<Relation> sieve(uint32_t special_q, int64_t a_min, int64_t a_max, int64_t b_min, int64_t b_max);
    
private:
    PolynomialContext ctx_;
    FactorBase fb_;
};

} // namespace gnfs::sieve
