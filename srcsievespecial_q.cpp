#include "gnfs/sieve/special_q.hpp"

namespace gnfs::sieve {

SpecialQGenerator::SpecialQGenerator(uint32_t min_q, uint32_t max_q)
    : min_q_(min_q), max_q_(max_q), current_q_(min_q) {}

bool SpecialQGenerator::next(uint32_t& q) {
    if (current_q_ > max_q_) {
        return false;
    }
    
    // Simple: find next prime
    q = current_q_;
    
    // Find next prime (simplified)
    do {
        ++current_q_;
    } while (current_q_ <= max_q_ && current_q_ % 2 == 0);
    
    return true;
}

void SpecialQGenerator::reset() {
    current_q_ = min_q_;
}

} // namespace gnfs::sieve
