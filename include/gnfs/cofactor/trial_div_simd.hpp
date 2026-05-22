#pragma once

// SIMD-accelerated batch trial-division helper for the cofactor pipeline.
//
// Scope
// -----
// The cofactor pipeline (`include/gnfs/cofactor/`) opens with trial
// division against a pool of small primes before falling through to
// SQUFOF / Brent-Pollard rho / ECM. For typical pools of 100-500 primes
// the per-prime `cofactor % p` check is a tight scalar loop that can be
// micro-batched. This helper exposes a stand-alone wide-load divisibility
// scan that the cofactor entry point may opt into for a small reduction
// in retired uops per cofactor.
//
// Algorithm
// ---------
// Unsigned 32-bit integer division on common SIMD ISAs (ARM NEON, x86
// AVX2 / SSE2) lacks a native vector div instruction; only floating point
// or 16-bit-by-16-bit fixed-point divides are accelerated. The SIMD value
// here is therefore limited: we use vector loads / stores to consolidate
// the address-generation pipe, and let the compiler schedule independent
// 64/32 scalar mod operations against the four lanes that have already
// landed in a 128-bit register. The output index list is identical to the
// scalar reference and the helper is bit-for-bit reproducible across the
// scalar and SIMD paths.
//
// Build-time guards: only the host-platform implementation is compiled.
// When neither NEON nor AVX2/SSE2 is available `trial_div_simd_supported()`
// returns false and the dispatcher silently falls back to the scalar path.
// The runtime three-state ENV gate is the standard project pattern used
// by `GNFS_SPMV_SIMD` and `GNFS_BUCKET_PREFETCH`:
//
//   GNFS_TRIAL_DIV_SIMD=0     forces scalar (regression-bisect aid)
//   GNFS_TRIAL_DIV_SIMD=1     forces SIMD even when support is detected
//                              false (no-op then; reverts to scalar to
//                              keep correctness)
//   GNFS_TRIAL_DIV_SIMD=auto  default; SIMD if available, scalar otherwise
//   (unset / "" / other)      same as auto

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <vector>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  #if __has_include(<arm_neon.h>)
    #include <arm_neon.h>
    #define GNFS_TRIAL_DIV_SIMD_NEON 1
  #endif
#endif

#if defined(__AVX2__)
  #if __has_include(<immintrin.h>)
    #include <immintrin.h>
    #define GNFS_TRIAL_DIV_SIMD_AVX2 1
  #endif
#elif defined(__SSE2__)
  #if __has_include(<emmintrin.h>)
    #include <emmintrin.h>
    #define GNFS_TRIAL_DIV_SIMD_SSE2 1
  #endif
#endif

namespace gnfs::cofactor {

/// Tri-state runtime gate mode for the trial-division SIMD helper.
enum class TrialDivSimdMode {
    Auto,      ///< Use SIMD when supported by the platform.
    ForceOff,  ///< Force the scalar reference path.
    ForceOn,   ///< Force the SIMD path when supported; otherwise scalar.
};

namespace detail {

/// Cached env-parsed gate. The first call resolves the mode based on
/// `GNFS_TRIAL_DIV_SIMD`; subsequent calls return the cached value via a
/// relaxed atomic load. Strict "0"/"1" parsing — everything else (unset,
/// "auto", "", garbage) yields `Auto`.
struct TrialDivSimdCache {
    std::once_flag once;
    std::atomic<int> mode{static_cast<int>(TrialDivSimdMode::Auto)};
};

inline TrialDivSimdCache& trial_div_simd_cache() noexcept {
    static TrialDivSimdCache cache;
    return cache;
}

/// Parse `GNFS_TRIAL_DIV_SIMD` into a `TrialDivSimdMode` value.
/// Decision table:
///   "0"    → ForceOff
///   "1"    → ForceOn
///   "auto" → Auto
///   unset  → Auto
///   ""     → Auto
///   other  → Auto
inline TrialDivSimdMode parse_trial_div_simd_env() noexcept {
    const char* env = std::getenv("GNFS_TRIAL_DIV_SIMD");
    if (env == nullptr) return TrialDivSimdMode::Auto;
    if (std::strcmp(env, "0") == 0) return TrialDivSimdMode::ForceOff;
    if (std::strcmp(env, "1") == 0) return TrialDivSimdMode::ForceOn;
    // "auto", "", anything else → Auto
    return TrialDivSimdMode::Auto;
}

}  // namespace detail

/// Compile-time probe: does the host ISA ship a SIMD path we compile in?
/// AVX2 (x86) gives a 4-lane uint32 load; SSE2 also qualifies as a
/// minimal x86 baseline (still 4-lane uint32 loads). NEON (ARM64) gives a
/// 4-lane uint32 load via `uint32x4_t`.
[[nodiscard]] constexpr bool trial_div_simd_supported() noexcept {
#if defined(GNFS_TRIAL_DIV_SIMD_NEON) || \
    defined(GNFS_TRIAL_DIV_SIMD_AVX2) || \
    defined(GNFS_TRIAL_DIV_SIMD_SSE2)
    return true;
#else
    return false;
#endif
}

/// Resolve the runtime gate mode from the cached env. Result is stable
/// for the lifetime of the process unless
/// `trial_div_simd_reset_env_cache_for_testing()` is invoked.
[[nodiscard]] inline TrialDivSimdMode trial_div_simd_mode() noexcept {
    auto& cache = detail::trial_div_simd_cache();
    std::call_once(cache.once, [&cache]() noexcept {
        cache.mode.store(static_cast<int>(detail::parse_trial_div_simd_env()),
                         std::memory_order_relaxed);
    });
    return static_cast<TrialDivSimdMode>(
        cache.mode.load(std::memory_order_relaxed));
}

/// Whether the SIMD path should actually be taken at the current call
/// site. Combines the runtime mode with the compile-time support probe so
/// that `ForceOn` on an unsupported platform safely falls back to scalar
/// (correctness > performance).
[[nodiscard]] inline bool trial_div_simd_enabled() noexcept {
    const auto m = trial_div_simd_mode();
    switch (m) {
        case TrialDivSimdMode::ForceOff:
            return false;
        case TrialDivSimdMode::ForceOn:
            return trial_div_simd_supported();
        case TrialDivSimdMode::Auto:
        default:
            return trial_div_simd_supported();
    }
}

/// Reset the cached gate. Intended for unit tests that toggle
/// `GNFS_TRIAL_DIV_SIMD` between scenarios via `setenv` / `unsetenv`.
/// Not thread-safe; only call when no concurrent caller is executing
/// `trial_div_simd_mode()` or `trial_div_simd_enabled()`.
inline void trial_div_simd_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::trial_div_simd_cache();
    cache.~TrialDivSimdCache();
    new (&cache) detail::TrialDivSimdCache();
}

/// Scalar reference: check `cofactor % primes[i] == 0` for each i and
/// append `i` to `out_divisible_indices` whenever the modulus is zero.
/// Output is ascending (input scan order). Behaviour is bit-for-bit
/// identical to a hand-rolled scalar loop and is what the SIMD path is
/// validated against.
///
/// Preconditions:
///   * `primes[i] > 0` for every i (caller's responsibility; behaviour
///     undefined for zero divisors)
///   * `out_divisible_indices` may be non-empty on entry; the helper
///     appends, it does not clear.
inline void batch_check_divisibility_scalar(
    std::uint64_t cofactor,
    std::span<const std::uint32_t> primes,
    std::vector<std::uint32_t>& out_divisible_indices) {
    const std::size_t n = primes.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t p = primes[i];
        if (cofactor % p == 0) {
            out_divisible_indices.push_back(static_cast<std::uint32_t>(i));
        }
    }
}

namespace detail {

/// 4-lane batch divisibility tail. Loads four prime divisors into a
/// SIMD register so the compiler may overlap address-generation with the
/// scalar `cofactor % p` evaluation. We deliberately keep the modulus
/// scalar — none of the host ISAs (NEON, SSE2, AVX2) accelerate
/// uint32_t division — and rely on register-level batching to cut per-
/// iteration uop count. Output indices are pushed in ascending order
/// (same as the scalar reference).
inline void batch_check_divisibility_simd_impl(
    std::uint64_t cofactor,
    std::span<const std::uint32_t> primes,
    std::vector<std::uint32_t>& out_divisible_indices) {
    const std::size_t n = primes.size();
    std::size_t i = 0;

#if defined(GNFS_TRIAL_DIV_SIMD_NEON)
    // 4-lane NEON batch: a single vld1q_u32 brings 4 primes into a Q
    // register, the four scalar mods can then issue in parallel.
    while (i + 4 <= n) {
        uint32x4_t pv = vld1q_u32(&primes[i]);
        std::uint32_t p0 = vgetq_lane_u32(pv, 0);
        std::uint32_t p1 = vgetq_lane_u32(pv, 1);
        std::uint32_t p2 = vgetq_lane_u32(pv, 2);
        std::uint32_t p3 = vgetq_lane_u32(pv, 3);
        if (cofactor % p0 == 0) {
            out_divisible_indices.push_back(static_cast<std::uint32_t>(i + 0));
        }
        if (cofactor % p1 == 0) {
            out_divisible_indices.push_back(static_cast<std::uint32_t>(i + 1));
        }
        if (cofactor % p2 == 0) {
            out_divisible_indices.push_back(static_cast<std::uint32_t>(i + 2));
        }
        if (cofactor % p3 == 0) {
            out_divisible_indices.push_back(static_cast<std::uint32_t>(i + 3));
        }
        i += 4;
    }
#elif defined(GNFS_TRIAL_DIV_SIMD_AVX2)
    // 4-lane AVX2 batch: _mm_loadu_si128 yields four uint32 in a single
    // load; horizontal extraction via _mm_extract_epi32 lets the four
    // scalar mods issue against independent registers.
    while (i + 4 <= n) {
        __m128i pv = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(&primes[i]));
        std::uint32_t p0 = static_cast<std::uint32_t>(_mm_extract_epi32(pv, 0));
        std::uint32_t p1 = static_cast<std::uint32_t>(_mm_extract_epi32(pv, 1));
        std::uint32_t p2 = static_cast<std::uint32_t>(_mm_extract_epi32(pv, 2));
        std::uint32_t p3 = static_cast<std::uint32_t>(_mm_extract_epi32(pv, 3));
        if (cofactor % p0 == 0) {
            out_divisible_indices.push_back(static_cast<std::uint32_t>(i + 0));
        }
        if (cofactor % p1 == 0) {
            out_divisible_indices.push_back(static_cast<std::uint32_t>(i + 1));
        }
        if (cofactor % p2 == 0) {
            out_divisible_indices.push_back(static_cast<std::uint32_t>(i + 2));
        }
        if (cofactor % p3 == 0) {
            out_divisible_indices.push_back(static_cast<std::uint32_t>(i + 3));
        }
        i += 4;
    }
#elif defined(GNFS_TRIAL_DIV_SIMD_SSE2)
    // 4-lane SSE2 fallback. SSE2 lacks `_mm_extract_epi32` (SSE4.1+), so
    // we land the load to a stack buffer and read scalars. The stack-
    // local copy is still one load and one straight-line scalar read
    // versus four separate loads.
    while (i + 4 <= n) {
        alignas(16) std::uint32_t buf[4];
        __m128i pv = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(&primes[i]));
        _mm_store_si128(reinterpret_cast<__m128i*>(buf), pv);
        if (cofactor % buf[0] == 0) {
            out_divisible_indices.push_back(static_cast<std::uint32_t>(i + 0));
        }
        if (cofactor % buf[1] == 0) {
            out_divisible_indices.push_back(static_cast<std::uint32_t>(i + 1));
        }
        if (cofactor % buf[2] == 0) {
            out_divisible_indices.push_back(static_cast<std::uint32_t>(i + 2));
        }
        if (cofactor % buf[3] == 0) {
            out_divisible_indices.push_back(static_cast<std::uint32_t>(i + 3));
        }
        i += 4;
    }
#endif

    // Scalar residual (always run): handles the tail when n is not a
    // multiple of the SIMD batch size. On platforms with neither NEON
    // nor SSE2/AVX2 this is the entire loop — `i` is still zero — and
    // the result matches `batch_check_divisibility_scalar` exactly.
    for (; i < n; ++i) {
        const std::uint32_t p = primes[i];
        if (cofactor % p == 0) {
            out_divisible_indices.push_back(static_cast<std::uint32_t>(i));
        }
    }
}

}  // namespace detail

/// Batch divisibility check: appends to `out_divisible_indices` the index
/// of every `primes[i]` that divides `cofactor`. Output ordering is the
/// natural scan order (ascending input index), identical to
/// `batch_check_divisibility_scalar`.
///
/// Dispatches to the SIMD inner loop when `trial_div_simd_enabled()`
/// returns true and the platform has a compiled SIMD path; otherwise
/// falls back to the scalar reference. Both paths produce a bit-for-bit
/// identical `out_divisible_indices` sequence for the same input.
///
/// Preconditions: as for `batch_check_divisibility_scalar`. The helper
/// appends; the caller should clear or `reserve()` according to whether
/// previous content must be preserved.
inline void batch_check_divisibility(
    std::uint64_t cofactor,
    std::span<const std::uint32_t> primes,
    std::vector<std::uint32_t>& out_divisible_indices) {
    if (primes.empty()) {
        return;
    }
    if (trial_div_simd_enabled()) {
        detail::batch_check_divisibility_simd_impl(cofactor, primes,
                                                   out_divisible_indices);
    } else {
        batch_check_divisibility_scalar(cofactor, primes,
                                        out_divisible_indices);
    }
}

}  // namespace gnfs::cofactor
