#pragma once

#include "safe_math.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>

namespace gnfs::util {

/// Scale a count, keeping ordinary binary64 estimates and flooring wide products exactly.
[[nodiscard]] inline size_t scaled_size_floor(size_t count, double factor) noexcept {
    constexpr size_t max_size = std::numeric_limits<size_t>::max();
    constexpr int precision = std::numeric_limits<double>::digits;
    constexpr uint64_t consecutive_limit = uint64_t{1} << precision;
    if (factor == 1.0)
        return count;
    if (count == 0 || !(factor > 0.0))
        return 0;

    // Keep historical rounding for ordinary estimates, including 100 * 1.2.
    // On 32-bit targets the strict-excess increment can reach SIZE_MAX before
    // double loses integer precision. Scale exactly at that boundary too.
    if (static_cast<uint64_t>(count) <= consecutive_limit && factor <= consecutive_limit) {
        const double estimate = static_cast<double>(count) * factor;
        if (estimate < consecutive_limit && estimate < static_cast<double>(max_size - 1))
            return static_cast<size_t>(estimate);
    }

    double integer_part;
    const double fraction = std::modf(factor, &integer_part);
    const size_t whole =
        saturating_size_product(count, size_from_nonnegative_double_floor(integer_part));
    if (whole == max_size || fraction == 0.0)
        return whole;

    int exponent;
    const double mantissa = std::frexp(fraction, &exponent);
    if (-exponent >= std::numeric_limits<size_t>::digits)
        return whole;
    auto bits = static_cast<uint64_t>(std::ldexp(mantissa, precision));
    size_t part = 0;
    // Read low bits first. Each step floors (part + bit * count) / 2.
    // part <= count, so midpoint rounds down and avoids overflowing the sum.
    while (bits != 0) {
        part = (bits & 1) ? std::midpoint(part, count) : part / 2;
        bits >>= 1;
    }
    return saturating_size_add(whole, part >> -exponent);
}

} // namespace gnfs::util
