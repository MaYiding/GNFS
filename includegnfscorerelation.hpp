#pragma once

#include "gnfs/core/integer.hpp"
#include "gnfs/util/small_vector.hpp"
#include <vector>
#include <cstdint>

namespace gnfs::core {

/// A relation in GNFS represents a smooth (a,b) pair
struct Relation {
    int64_t a;
    int64_t b;
    
    // Rational side factorization: product of primes
    std::vector<uint32_t> rational_factors;
    
    // Algebraic side factorization: product of prime ideals
    std::vector<uint32_t> algebraic_factors;
    
    // Large primes (if any)
    Integer rational_large_prime;
    Integer algebraic_large_prime;
    
    bool is_valid() const {
        return b != 0;
    }
};

} // namespace gnfs::core
