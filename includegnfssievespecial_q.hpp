#pragma once

#include "gnfs/core/polynomial.hpp"
#include "gnfs/factor_base/builder.hpp"
#include <vector>
#include <cstdint>

namespace gnfs::sieve {

using gnfs::core::Integer;
using gnfs::core::PolynomialContext;
using gnfs::factor_base::FactorBase;

/// Special-Q generator for GNFS sieving
class SpecialQGenerator {
public:
    SpecialQGenerator(uint32_t min_q, uint32_t max_q);
    
    /// Get next special-q value
    bool next(uint32_t& q);
    
    /// Reset generator
    void reset();
    
private:
    uint32_t min_q_;
    uint32_t max_q_;
    uint32_t current_q_;
};

} // namespace gnfs::sieve
