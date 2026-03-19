#pragma once

#include <cstdint>

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

} // namespace gnfs::util
