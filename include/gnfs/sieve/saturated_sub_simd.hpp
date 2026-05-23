#pragma once

// SIMD-accelerated batch uint8_t saturated subtract helper for the
// lattice sieve `sieve_array_` (uint8_t log residuals).
//
// Scope
// -----
// After the sieve `apply` phase writes log-residual subtractions into
// the per-cell `sieve_array_` buffer, the caller sometimes wants to
// apply a uniform bias subtraction with saturation at zero. The
// canonical operation is:
//
//   values[i] = max(0, int(values[i]) - bias)
//
// equivalently, `values[i] = values[i] >= bias ? (values[i] - bias) : 0`
// in uint8_t arithmetic. This is exactly the semantics of NEON
// `vqsubq_u8` (saturating subtract) and AVX2 `_mm256_subs_epu8`
// (saturating subtract): both intrinsics map to a single CPU
// instruction per lane, with no compare-then-branch tail. On the
// scalar path the same operation requires `cmp + branch + sub` per
// byte, defeating branch prediction on inputs where the distribution
// of `values[i] >= bias` is roughly even.
//
// This header exposes a stand-alone helper that wraps that primitive
// as `saturated_sub_u8_batch(values, bias)`. NEON processes 16 lanes
// per iteration via `vqsubq_u8`; AVX2 processes 32 lanes per
// iteration via `_mm256_subs_epu8`. Both intrinsics implement
// unsigned saturating subtract directly, so the implementation needs
// no sign-bias trick — unlike the sibling `threshold_scan_simd`
// helper which works around `_mm256_cmpgt_epi8`'s signed semantics.
//
// Pure header, no dependencies beyond the standard library and the
// platform intrinsics. The helper is intentionally stand-alone: it
// does not touch `sieve_bucket_region`, the candidate emission path,
// the scratch buffer used by the norm precompute pass, or any of the
// W6 / W12 / W13 / W14 T4 sieve helpers. It is opt-in future
// infrastructure that a future caller (for example, a sieve bias
// pre-screen or a candidate filter pre-pass) can adopt independently.
//
// Bit-for-bit guarantee
// ---------------------
// `max(0, int(values[i]) - bias)` is a deterministic function of the
// input bytes and the bias. Both the NEON wide path and the AVX2 wide
// path reduce to the same per-byte unsigned saturating subtract, so
// for every input span and every bias the SIMD result equals the
// scalar reference exactly. Empty input is a no-op; the span is left
// untouched. `bias == 0` is short-circuited and the span is left
// untouched without any writes. `bias == 255` collapses every byte to
// 0 (saturated for v < 255; exact for v == 255 since 255 - 255 = 0).
//
// Defensive contract
// ------------------
// The helper takes `std::span<uint8_t>` so the size is captured
// alongside the pointer. There is no separate length argument that
// the helper could disagree with. An empty span (`values.size() ==
// 0`) is a well-defined no-op that leaves the span untouched, even
// before the SIMD dispatch check. `bias == 0` is short-circuited
// before any SIMD or scalar work: subtracting 0 is the identity
// transformation on the span, so the helper deliberately performs
// zero writes to memory in that case (matters for callers that pin
// the span to memory-mapped pages or COW-shared buffers).
//
// ENV gate
// --------
//
//   GNFS_SIEVE_SATURATED_SUB_SIMD=auto    SIMD enabled when supported (default)
//   GNFS_SIEVE_SATURATED_SUB_SIMD=0       force scalar (regression bisect / sanitizer)
//   GNFS_SIEVE_SATURATED_SUB_SIMD=off     same as "0"
//   GNFS_SIEVE_SATURATED_SUB_SIMD=1       force SIMD when supported (else scalar fallback)
//   GNFS_SIEVE_SATURATED_SUB_SIMD=on      same as "1"
//   (unset / empty / any other)           treated as Auto
//
// The runtime gate is cached after the first call so the dispatcher
// branch on the hot path costs only an atomic-relaxed load. Tests
// that toggle the env mid-process call
// `saturated_sub_simd_reset_env_cache_for_testing()` to flush the
// cache.
//
// Build-time guards
// -----------------
// Only the host-platform implementation is compiled. When neither
// NEON nor AVX2 is available `saturated_sub_simd_supported()`
// returns false and the dispatcher silently falls back to the scalar
// reference.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  #if __has_include(<arm_neon.h>)
    #include <arm_neon.h>
    #define GNFS_SATURATED_SUB_SIMD_NEON 1
  #endif
#endif

#if defined(__AVX2__)
  #if __has_include(<immintrin.h>)
    #include <immintrin.h>
    #define GNFS_SATURATED_SUB_SIMD_AVX2 1
  #endif
#endif

namespace gnfs::sieve {

/// Three-state ENV gate matching the `GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD`
/// / `GNFS_LATTICE_COORDS_SIMD` / `GNFS_GF2_ROW_XOR_SIMD` /
/// `GNFS_GF2_POPCNT_SIMD` family. `Auto` defers to compile-time SIMD
/// availability, `ForceOff` forces the scalar path (regression bisect
/// / sanitizer noise reduction), `ForceOn` opts in even when
/// `saturated_sub_simd_supported()` is false — in which case the
/// dispatcher silently falls back to scalar to keep correctness.
enum class SaturatedSubSimdMode : std::uint8_t {
    Auto = 0,
    ForceOff = 1,
    ForceOn = 2,
};

namespace saturated_sub_detail {

inline std::atomic<std::uint8_t>& cached_state() noexcept {
    static std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(SaturatedSubSimdMode::Auto)};
    return state;
}

inline std::once_flag& cached_flag() noexcept {
    static std::once_flag flag;
    return flag;
}

inline SaturatedSubSimdMode resolve_mode_from_env() noexcept {
    const char* v = std::getenv("GNFS_SIEVE_SATURATED_SUB_SIMD");
    if (v == nullptr) return SaturatedSubSimdMode::Auto;
    if (std::strcmp(v, "0") == 0) return SaturatedSubSimdMode::ForceOff;
    if (std::strcmp(v, "off") == 0) return SaturatedSubSimdMode::ForceOff;
    if (std::strcmp(v, "1") == 0) return SaturatedSubSimdMode::ForceOn;
    if (std::strcmp(v, "on") == 0) return SaturatedSubSimdMode::ForceOn;
    // "auto" / "" / anything else → Auto
    return SaturatedSubSimdMode::Auto;
}

inline SaturatedSubSimdMode load_mode() noexcept {
    std::call_once(cached_flag(), []() noexcept {
        cached_state().store(static_cast<std::uint8_t>(resolve_mode_from_env()),
                             std::memory_order_relaxed);
    });
    return static_cast<SaturatedSubSimdMode>(
        cached_state().load(std::memory_order_relaxed));
}

}  // namespace saturated_sub_detail

/// Compile-time SIMD availability. Constant-folds on any given build.
/// Used by the dispatcher to decide whether `Auto` / `ForceOn`
/// actually has a SIMD code path to run.
[[nodiscard]] constexpr bool saturated_sub_simd_supported() noexcept {
#if defined(GNFS_SATURATED_SUB_SIMD_NEON) || defined(GNFS_SATURATED_SUB_SIMD_AVX2)
    return true;
#else
    return false;
#endif
}

/// Returns the parsed ENV state. Cached after the first call.
[[nodiscard]] inline SaturatedSubSimdMode saturated_sub_simd_mode() noexcept {
    return saturated_sub_detail::load_mode();
}

/// Dispatcher decision: should the helper take the SIMD path? Returns
/// `false` when there is no compile-time SIMD support, when ENV
/// forces it off, or when ENV is `ForceOn` but no SIMD is available.
[[nodiscard]] inline bool saturated_sub_simd_enabled() noexcept {
    const SaturatedSubSimdMode mode = saturated_sub_simd_mode();
    if (mode == SaturatedSubSimdMode::ForceOff) return false;
    // ForceOn and Auto both require compile-time availability.
    return saturated_sub_simd_supported();
}

/// Re-read GNFS_SIEVE_SATURATED_SUB_SIMD from the environment and
/// refresh the cached gate. Intended for unit tests that flip the env
/// between scenarios. Not called from production hot paths.
inline void saturated_sub_simd_reset_env_cache_for_testing() noexcept {
    saturated_sub_detail::cached_state().store(
        static_cast<std::uint8_t>(
            saturated_sub_detail::resolve_mode_from_env()),
        std::memory_order_relaxed);
    // Ensure call_once records completion even if it has never run,
    // so a later production call does not overwrite our state.
    std::call_once(saturated_sub_detail::cached_flag(), []() noexcept {});
}

// ---------------------------------------------------------------------------
// Reference scalar path (always available, used as golden for tests
// and as the no-SIMD fallback).
// ---------------------------------------------------------------------------

/// Scalar reference:
///   values[i] = max(0, int(values[i]) - bias)
/// for every `i` in `[0, values.size())`. Empty input is a no-op.
/// `bias == 0` is the identity transformation; the scalar reference
/// still walks the span to keep its semantics simple, but the
/// dispatcher short-circuits before reaching the helper in that case
/// to keep the contract "bias == 0 ⇒ zero writes".
inline void saturated_sub_u8_batch_scalar(std::span<std::uint8_t> values,
                                          std::uint8_t bias) noexcept {
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::uint8_t v = values[i];
        values[i] = (v >= bias) ? static_cast<std::uint8_t>(v - bias)
                                : static_cast<std::uint8_t>(0);
    }
}

namespace saturated_sub_detail {

#if defined(GNFS_SATURATED_SUB_SIMD_NEON)
/// NEON 16-lane chunk: load 16 bytes, run unsigned saturating
/// subtract (`vqsubq_u8`), store back. Each lane becomes
/// `max(0, v - bias)` in a single CPU instruction with no compare-
/// then-branch tail.
inline void saturated_sub_chunk_neon(std::uint8_t* ptr,
                                     std::uint8_t bias) noexcept {
    const uint8x16_t v = vld1q_u8(ptr);
    const uint8x16_t b = vdupq_n_u8(bias);
    const uint8x16_t r = vqsubq_u8(v, b);
    vst1q_u8(ptr, r);
}
#endif  // GNFS_SATURATED_SUB_SIMD_NEON

#if defined(GNFS_SATURATED_SUB_SIMD_AVX2)
/// AVX2 32-lane chunk: load 32 bytes, run unsigned saturating
/// subtract (`_mm256_subs_epu8`), store back. The intrinsic implements
/// unsigned saturating subtract directly, so no sign-bias trick is
/// needed (unlike `_mm256_cmpgt_epi8` which is signed-only).
inline void saturated_sub_chunk_avx2(std::uint8_t* ptr,
                                     std::uint8_t bias) noexcept {
    const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ptr));
    const __m256i b = _mm256_set1_epi8(static_cast<char>(bias));
    const __m256i r = _mm256_subs_epu8(v, b);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(ptr), r);
}
#endif  // GNFS_SATURATED_SUB_SIMD_AVX2

}  // namespace saturated_sub_detail

// ---------------------------------------------------------------------------
// Primary entry point (dispatcher).
// ---------------------------------------------------------------------------

/// Batched uint8_t saturated subtract over a span:
///   values[i] = max(0, int(values[i]) - bias)
/// for every `i` in `[0, values.size())`. The mutation is in-place.
/// Empty input is a no-op and leaves the span untouched. `bias == 0`
/// is short-circuited (identity transformation, zero writes). The
/// SIMD path is taken when `saturated_sub_simd_enabled()` is true;
/// otherwise falls back to the scalar reference. Bit-for-bit
/// identical output across both paths for every input span and every
/// bias.
inline void saturated_sub_u8_batch(std::span<std::uint8_t> values,
                                   std::uint8_t bias) noexcept {
    if (values.empty()) return;
    if (bias == 0) return;  // identity, no writes

    if (!saturated_sub_simd_enabled()) {
        saturated_sub_u8_batch_scalar(values, bias);
        return;
    }

#if defined(GNFS_SATURATED_SUB_SIMD_NEON)
    std::uint8_t* ptr = values.data();
    const std::size_t n = values.size();
    std::size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        saturated_sub_detail::saturated_sub_chunk_neon(ptr + i, bias);
    }
    for (; i < n; ++i) {
        const std::uint8_t v = ptr[i];
        ptr[i] = (v >= bias) ? static_cast<std::uint8_t>(v - bias)
                             : static_cast<std::uint8_t>(0);
    }
#elif defined(GNFS_SATURATED_SUB_SIMD_AVX2)
    std::uint8_t* ptr = values.data();
    const std::size_t n = values.size();
    std::size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        saturated_sub_detail::saturated_sub_chunk_avx2(ptr + i, bias);
    }
    for (; i < n; ++i) {
        const std::uint8_t v = ptr[i];
        ptr[i] = (v >= bias) ? static_cast<std::uint8_t>(v - bias)
                             : static_cast<std::uint8_t>(0);
    }
#else
    saturated_sub_u8_batch_scalar(values, bias);
#endif
}

}  // namespace gnfs::sieve
