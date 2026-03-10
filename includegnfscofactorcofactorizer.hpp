#pragma once

#include "gnfs/core/integer.hpp"

namespace gnfs::cofactor {

using gnfs::core::Integer;

/// Cofactorizer for trial division and ECM
class Cofactorizer {
public:
    Cofactorizer(uint32_t trial_bound, uint32_t large_prime_bound);
    
    /// Try to factor n into primes below large_prime_bound
    /// Returns true if successful
    bool try_factor(const Integer& n, std::vector<uint32_t>& factors, Integer& large_prime);
    
private:
    uint32_t trial_bound_;
    uint32_t large_prime_bound_;
};

} // namespace gnfs::cofactor
