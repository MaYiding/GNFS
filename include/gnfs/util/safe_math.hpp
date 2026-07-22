#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace gnfs::util {

/// Compute absolute value of int64_t without undefined behavior.
/// std::abs(INT64_MIN) is UB because |INT64_MIN| > INT64_MAX.
/// Returns uint64_t to accommodate the full range.
[[nodiscard]] constexpr uint64_t safe_abs(int64_t value) noexcept {
    if (value >= 0) {
        return static_cast<uint64_t>(value);
    }
    // For negative values: avoid -value (UB when value == INT64_MIN).
    // Instead: (value + 1) moves toward zero (safe even for INT64_MIN),
    // negate that (safe: result fits int64_t), then add 1 in unsigned space.
    return static_cast<uint64_t>(-(value + 1)) + 1u;
}

/// Saturating size_t addition for resource and target calculations.
[[nodiscard]] constexpr size_t saturating_size_add(size_t lhs, size_t rhs) noexcept {
    return rhs > std::numeric_limits<size_t>::max() - lhs
               ? std::numeric_limits<size_t>::max()
               : lhs + rhs;
}

/// Saturating size_t multiplication for bounded growth policies.
[[nodiscard]] constexpr size_t saturating_size_product(size_t value,
                                                       size_t multiplier) noexcept {
    return value != 0 && multiplier > std::numeric_limits<size_t>::max() / value
               ? std::numeric_limits<size_t>::max()
               : value * multiplier;
}

/// Floor a nonnegative floating-point estimate into size_t without UB.
/// Negative values and NaN map to zero; positive overflow and infinity
/// saturate at SIZE_MAX.
[[nodiscard]] constexpr size_t size_from_nonnegative_double_floor(double value) noexcept {
    if (!(value > 0.0)) {
        return 0;
    }
    const double max_size = static_cast<double>(std::numeric_limits<size_t>::max());
    return value < max_size ? static_cast<size_t>(value)
                            : std::numeric_limits<size_t>::max();
}

} // namespace gnfs::util
