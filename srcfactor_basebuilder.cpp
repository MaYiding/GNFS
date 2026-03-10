#include "gnfs/factor_base/builder.hpp"

namespace gnfs::factor_base {

FactorBaseBuilder::FactorBaseBuilder(const PolynomialContext& ctx) : ctx_(ctx) {}

FactorBase FactorBaseBuilder::build(uint32_t rational_bound, uint32_t algebraic_bound) {
    FactorBase fb;
    
    // Build rational factor base
    // Simple sieve of Eratosthenes
    std::vector<bool> is_prime(rational_bound + 1, true);
    is_prime[0] = is_prime[1] = false;
    
    for (uint32_t p = 2; p <= rational_bound; ++p) {
        if (!is_prime[p]) continue;
        
        // Mark multiples
        for (uint32_t k = p * 2; k <= rational_bound; k += p) {
            is_prime[k] = false;
        }
        
        // Add to factor base (root is 0 for rational side in simple implementation)
        fb.rational_primes.emplace_back(p, 0);
    }
    
    // Build algebraic factor base (simplified)
    is_prime.assign(algebraic_bound + 1, true);
    is_prime[0] = is_prime[1] = false;
    
    for (uint32_t p = 2; p <= algebraic_bound; ++p) {
        if (!is_prime[p]) continue;
        
        for (uint32_t k = p * 2; k <= algebraic_bound; k += p) {
            is_prime[k] = false;
        }
        
        // Find roots of algebraic polynomial mod p
        // Simplified: just use 0 as placeholder
        fb.algebraic_primes.emplace_back(p, 0);
    }
    
    return fb;
}

} // namespace gnfs::factor_base
