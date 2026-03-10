#include "gnfs/cofactor/cofactorizer.hpp"

namespace gnfs::cofactor {

Cofactorizer::Cofactorizer(uint32_t trial_bound, uint32_t large_prime_bound)
    : trial_bound_(trial_bound), large_prime_bound_(large_prime_bound) {}

bool Cofactorizer::try_factor(const Integer& n, std::vector<uint32_t>& factors, Integer& large_prime) {
    factors.clear();
    large_prime = Integer(1);
    
    Integer temp = n.clone();
    
    // Trial division
    for (uint32_t p = 2; p <= trial_bound_; ++p) {
        Integer prime(static_cast<int64_t>(p));
        Integer rem;
        
        while (true) {
            Integer::mod(rem, temp, prime);
            if (!rem.is_zero()) break;
            
            factors.push_back(p);
            temp /= prime;
        }
    }
    
    // Check if remainder is small enough
    if (temp.is_one()) {
        return true;
    }
    
    // Check if remainder is prime and small
    size_t bits = temp.bit_length();
    if (bits <= 32 && temp.is_probable_prime() > 0) {
        large_prime = temp;
        return true;
    }
    
    return false;
}

} // namespace gnfs::cofactor
