#pragma once

#include <gnfs/core/integer.hpp>

#include <cstdint>
#include <vector>

namespace gnfs::siqs {

using core::Integer;

struct SIQSRelation {
    Integer value;                    // Ax + B (the "square root" side)
    std::vector<uint32_t> fb_indices; // unique indices of nonzero FB exponents
    std::vector<uint8_t> exponents;   // exponent of each FB prime (for sqrt computation)
    uint64_t large_prime;             // LP1 (0 if fully smooth)
    uint64_t large_prime2;            // LP2 (0 if 1LP or full)
    std::vector<uint64_t> merge_lps;  // LP values from merged partials (for Y computation)
    bool negative;                    // Q(x) < 0?
};

} // namespace gnfs::siqs
