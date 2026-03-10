#pragma once

#include "gnfs/core/integer.hpp"
#include "gnfs/core/polynomial.hpp"
#include <vector>
#include <cstdint>

namespace gnfs::factor_base {

using gnfs::core::Integer;
using gnfs::core::PolynomialContext;

/// Prime in the factor base
struct FactorBasePrime {
    uint32_t prime;
    uint32_t root;  // Root of polynomial mod prime
    
    FactorBasePrime() : prime(0), root(0) {}
    FactorBasePrime(uint32_t p, uint32_t r) : prime(p), root(r) {}
};

/// Factor base for GNFS
struct FactorBase {
    std::vector<FactorBasePrime> rational_primes;
    std::vector<FactorBasePrime> algebraic_primes;
    
    size_t rational_size() const { return rational_primes.size(); }
    size_t algebraic_size() const { return algebraic_primes.size(); }
};

/// Builder for factor base
class FactorBaseBuilder {
public:
    FactorBaseBuilder(const PolynomialContext& ctx);
    
    /// Build factor base with given bounds
    FactorBase build(uint32_t rational_bound, uint32_t algebraic_bound);
    
private:
    PolynomialContext ctx_;
};

} // namespace gnfs::factor_base
