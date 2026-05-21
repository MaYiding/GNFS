#pragma once

/// @file bit_intrin.hpp
/// Cross-compiler bit intrinsic wrappers.
///
/// GCC/Clang expose `__builtin_clz`, `__builtin_ctz`, `__builtin_popcount`
/// and their `ll` variants. MSVC exposes `_BitScanForward64`,
/// `_BitScanReverse64`, `__popcnt64` from <intrin.h>. C++20 provides
/// standard `<bit>` intrinsics that map to the appropriate compiler
/// builtin under the hood. This header offers a thin namespace wrapper
/// using C++20 `<bit>` as the implementation. The project requires C++20
/// (see CMakeLists.txt), so `<bit>` is always available.
///
/// Migration plan:
///   `__builtin_ctzll(x)`  -> `gnfs::util::ctz64(x)`
///   `__builtin_clzll(x)`  -> `gnfs::util::clz64(x)`
///   `__builtin_popcountll(x)` -> `gnfs::util::popcount64(x)`
///   `__builtin_clz(x)`    -> `gnfs::util::clz32(x)`
///   `__builtin_ctz(x)`    -> `gnfs::util::ctz32(x)`
///   `__builtin_popcount(x)` -> `gnfs::util::popcount32(x)`
///
/// Behaviour difference vs `__builtin_clz` family:
///   `__builtin_clz(0)` is undefined behaviour on GCC/Clang. The C++20
///   `std::countl_zero(0)` returns the bit width (e.g. 32). Call sites
///   relying on UB-for-zero must be audited, but in practice all uses in
///   this project gate the call with `if (x != 0)`.

#include <bit>
#include <cstdint>

namespace gnfs::util {

/// Count trailing zeros (32-bit). UB if x == 0 on GCC/Clang `__builtin_ctz`;
/// C++20 `std::countr_zero(0) == 32`. Caller should ensure x != 0 to match
/// historic `__builtin_*` semantics.
[[nodiscard]] inline int ctz32(uint32_t x) noexcept {
    return std::countr_zero(x);
}

/// Count trailing zeros (64-bit).
[[nodiscard]] inline int ctz64(uint64_t x) noexcept {
    return std::countr_zero(x);
}

/// Count leading zeros (32-bit). UB if x == 0 on GCC/Clang `__builtin_clz`;
/// C++20 `std::countl_zero(0) == 32`.
[[nodiscard]] inline int clz32(uint32_t x) noexcept {
    return std::countl_zero(x);
}

/// Count leading zeros (64-bit).
[[nodiscard]] inline int clz64(uint64_t x) noexcept {
    return std::countl_zero(x);
}

/// Population count (32-bit).
[[nodiscard]] inline int popcount32(uint32_t x) noexcept {
    return std::popcount(x);
}

/// Population count (64-bit).
[[nodiscard]] inline int popcount64(uint64_t x) noexcept {
    return std::popcount(x);
}

}  // namespace gnfs::util
