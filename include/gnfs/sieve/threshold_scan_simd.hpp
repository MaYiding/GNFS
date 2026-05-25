#pragma once

// SIMD-accelerated batch threshold-count helper for the lattice sieve
// `sieve_array_` (uint8_t log residuals).
//
// Scope
// -----
// Once the sieve `apply` phase has written log-residual subtractions into
// the per-cell `sieve_array_` buffer, the caller still has to scan the
// buffer to count how many cells now fall at or above the smoothness
// threshold. The candidate count is then used as a workload estimate for
// the downstream cofactor cascade and as a pre-screen against floods of
// false-positive cells. The scan itself is a tight `count(values[i] >=
// threshold)` reduction over an arbitrary `uint8_t` span — exactly the
// shape that NEON / AVX2 wide compares were designed for.
//
// This header exposes a stand-alone helper that wraps that primitive as
// `count_above_threshold_u8(values, threshold)`. NEON processes 16 lanes
// per iteration via `vcgeq_u8 -> vandq_u8(0x01) -> vpaddlq_u8 -> vaddvq_u16`;
// AVX2 processes 32 lanes via a sign-bias XOR plus `_mm256_cmpgt_epi8` plus
// `_mm256_movemask_epi8 -> __builtin_popcount`. AVX2 needs the sign bias
// because `_mm256_cmpgt_epi8` operates on `int8_t` and would otherwise
// flip the ordering for threshold byte values >= 0x80.
//
// Pure header, no dependencies beyond the standard library and the
// platform intrinsics. The helper is intentionally stand-alone: it does
// not touch `sieve_bucket_region`, the candidate emission path, the
// scratch buffer used by the norm precompute pass, or any of the W6 /
// W12 / W13 sieve helpers. It is opt-in future infrastructure that a
// future caller (for example, a candidate-throughput telemetry probe or
// a soon-to-be wired apply-scan pre-screen) can adopt independently.
//
// Bit-for-bit guarantee
// ---------------------
// `popcount({values[i] >= threshold})` is a deterministic function of
// the input bytes. Both the NEON wide path and the AVX2 wide path
// reduce to the same per-byte comparison plus a parallel popcount, so
// for every input span and every threshold the SIMD result equals the
// scalar reference exactly. Empty input returns 0 without touching the
// span. Threshold 0 returns `values.size()` (every byte satisfies the
// predicate). Threshold 255 returns the count of bytes equal to 0xFF.
//
// Defensive contract
// ------------------
// The helper takes `std::span<const uint8_t>` so the size is captured
// alongside the pointer. There is no separate length argument that the
// helper could disagree with. An empty span (`values.size() == 0`) is a
// well-defined no-op that returns 0 immediately, even before the SIMD
// dispatch check.
//
// ENV gate
// --------
//
//   GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD=auto    SIMD enabled when supported (default)
//   GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD=0       force scalar (regression bisect / sanitizer)
//   GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD=off     same as "0"
//   GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD=1       force SIMD when supported (else scalar fallback)
//   GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD=on      same as "1"
//   (unset / empty / any other)                   treated as Auto
//
// The runtime gate is cached after the first call so the dispatcher
// branch on the hot path costs only an atomic-relaxed load. Tests that
// toggle the env mid-process call
// `threshold_scan_simd_reset_env_cache_for_testing()` to flush the cache.
//
// Build-time guards
// -----------------
// Only the host-platform implementation is compiled. When neither NEON
// nor AVX2 is available `threshold_scan_simd_supported()` returns false
// and the dispatcher silently falls back to the scalar reference.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include "../util/bit_intrin.hpp"

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  #if __has_include(<arm_neon.h>)
    #include <arm_neon.h>
    #define GNFS_THRESHOLD_SCAN_SIMD_NEON 1
  #endif
#endif

#if defined(__AVX2__)
  #if __has_include(<immintrin.h>)
    #include <immintrin.h>
    #define GNFS_THRESHOLD_SCAN_SIMD_AVX2 1
  #endif
#endif

namespace gnfs::sieve {

/// Three-state ENV gate matching the `GNFS_LATTICE_COORDS_SIMD` /
/// `GNFS_GF2_ROW_XOR_SIMD` / `GNFS_GF2_POPCNT_SIMD` family. `Auto`
/// defers to compile-time SIMD availability, `ForceOff` forces the
/// scalar path (regression bisect / sanitizer noise reduction),
/// `ForceOn` opts in even when `threshold_scan_simd_supported()` is
/// false — in which case the dispatcher silently falls back to scalar
/// to keep correctness.
enum class ThresholdScanSimdMode : std::uint8_t {
    Auto = 0,
    ForceOff = 1,
    ForceOn = 2,
};

namespace threshold_scan_detail {

inline std::atomic<std::uint8_t>& cached_state() noexcept {
    static std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(ThresholdScanSimdMode::Auto)};
    return state;
}

inline std::once_flag& cached_flag() noexcept {
    static std::once_flag flag;
    return flag;
}

inline ThresholdScanSimdMode resolve_mode_from_env() noexcept {
    const char* v = std::getenv("GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD");
    if (v == nullptr) return ThresholdScanSimdMode::Auto;
    if (std::strcmp(v, "0") == 0) return ThresholdScanSimdMode::ForceOff;
    if (std::strcmp(v, "off") == 0) return ThresholdScanSimdMode::ForceOff;
    if (std::strcmp(v, "1") == 0) return ThresholdScanSimdMode::ForceOn;
    if (std::strcmp(v, "on") == 0) return ThresholdScanSimdMode::ForceOn;
    // "auto" / "" / anything else → Auto
    return ThresholdScanSimdMode::Auto;
}

inline ThresholdScanSimdMode load_mode() noexcept {
    std::call_once(cached_flag(), []() noexcept {
        cached_state().store(static_cast<std::uint8_t>(resolve_mode_from_env()),
                             std::memory_order_relaxed);
    });
    return static_cast<ThresholdScanSimdMode>(
        cached_state().load(std::memory_order_relaxed));
}

}  // namespace threshold_scan_detail

/// Compile-time SIMD availability. Constant-folds on any given build.
/// Used by the dispatcher to decide whether `Auto` / `ForceOn` actually
/// has a SIMD code path to run.
[[nodiscard]] constexpr bool threshold_scan_simd_supported() noexcept {
#if defined(GNFS_THRESHOLD_SCAN_SIMD_NEON) || defined(GNFS_THRESHOLD_SCAN_SIMD_AVX2)
    return true;
#else
    return false;
#endif
}

/// Returns the parsed ENV state. Cached after the first call.
[[nodiscard]] inline ThresholdScanSimdMode threshold_scan_simd_mode() noexcept {
    return threshold_scan_detail::load_mode();
}

/// Dispatcher decision: should the helper take the SIMD path? Returns
/// `false` when there is no compile-time SIMD support, when ENV forces
/// it off, or when ENV is `ForceOn` but no SIMD is available.
[[nodiscard]] inline bool threshold_scan_simd_enabled() noexcept {
    const ThresholdScanSimdMode mode = threshold_scan_simd_mode();
    if (mode == ThresholdScanSimdMode::ForceOff) return false;
    // ForceOn and Auto both require compile-time availability.
    return threshold_scan_simd_supported();
}

/// Re-read GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD from the environment
/// and refresh the cached gate. Intended for unit tests that flip the
/// env between scenarios. Not called from production hot paths.
inline void threshold_scan_simd_reset_env_cache_for_testing() noexcept {
    threshold_scan_detail::cached_state().store(
        static_cast<std::uint8_t>(
            threshold_scan_detail::resolve_mode_from_env()),
        std::memory_order_relaxed);
    // Ensure call_once records completion even if it has never run, so
    // a later production call does not overwrite our state.
    std::call_once(threshold_scan_detail::cached_flag(), []() noexcept {});
}

// ---------------------------------------------------------------------------
// Reference scalar path (always available, used as golden for tests and
// as the no-SIMD fallback).
// ---------------------------------------------------------------------------

/// Scalar reference:
///   count = |{ i in [0, values.size()) : values[i] >= threshold }|.
///
/// Empty input returns 0 without touching the span. Threshold 0 returns
/// `values.size()`. Threshold 255 returns the count of bytes equal to
/// 0xFF.
[[nodiscard]] inline std::size_t count_above_threshold_u8_scalar(
    std::span<const std::uint8_t> values,
    std::uint8_t threshold) noexcept {
    std::size_t count = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] >= threshold) ++count;
    }
    return count;
}

namespace threshold_scan_detail {

#if defined(GNFS_THRESHOLD_SCAN_SIMD_NEON)
/// NEON 16-lane chunk count: process 16 bytes per iteration. Each lane
/// becomes either 0x01 (passed) or 0x00 (failed) via `vcgeq_u8` and
/// `vandq_u8(0x01)`. We reduce 16 bytes into a single uint16 via
/// `vpaddlq_u8 -> uint16x8_t` (pairwise add) followed by `vaddvq_u16`
/// (horizontal sum). The per-chunk sum is at most 16, so the uint16
/// accumulator never overflows inside the lane reduction. The caller
/// then accumulates each per-chunk sum into a wider `size_t`.
[[nodiscard]] inline std::size_t count_chunk_neon(const std::uint8_t* ptr,
                                                  std::uint8_t threshold) noexcept {
    const uint8x16_t v = vld1q_u8(ptr);
    const uint8x16_t t = vdupq_n_u8(threshold);
    // `vcgeq_u8` returns 0xFF in lane on >=, 0x00 otherwise; mask down
    // to 0x01 so the pairwise add does not overflow 16 lanes.
    const uint8x16_t pass_mask = vcgeq_u8(v, t);
    const uint8x16_t bits = vandq_u8(pass_mask, vdupq_n_u8(0x01));
    // Pairwise widening add: 16 uint8 -> 8 uint16. Each uint16 holds
    // the sum of two adjacent uint8 lanes (max value 2). The eight
    // resulting uint16 elements sum to at most 16, safely within
    // uint16 range.
    const uint16x8_t paired = vpaddlq_u8(bits);
    const std::uint16_t chunk_sum = vaddvq_u16(paired);
    return static_cast<std::size_t>(chunk_sum);
}
#endif  // GNFS_THRESHOLD_SCAN_SIMD_NEON

#if defined(GNFS_THRESHOLD_SCAN_SIMD_AVX2)
/// AVX2 32-lane chunk count: process 32 bytes per iteration. AVX2's
/// `_mm256_cmpgt_epi8` is signed, so we apply a sign-bias XOR with
/// `0x80` to both the loaded bytes and the threshold before comparing —
/// after the bias, `int8_t` greater-than equals `uint8_t` greater-than.
/// To match the `>= threshold` semantics of the scalar reference (the
/// NEON path uses `vcgeq_u8` directly), we test `cmpgt(v, t-1)`. When
/// threshold == 0, every byte satisfies `>= 0` so we short-circuit to
/// 32 to avoid the underflow that `t-1 = 0xFF` would mean. The result
/// vector contains 0xFF in lanes that passed; `_mm256_movemask_epi8`
/// packs the high bit of each lane into a 32-bit mask, and
/// `__builtin_popcount` counts the set bits.
[[nodiscard]] inline std::size_t count_chunk_avx2(const std::uint8_t* ptr,
                                                  std::uint8_t threshold) noexcept {
    // Threshold 0 is handled by the dispatcher fast path; if the caller
    // reaches this helper with threshold == 0, every byte passes.
    if (threshold == 0) return 32;
    const std::uint8_t adjusted = static_cast<std::uint8_t>(threshold - 1);
    const __m256i sign_bias = _mm256_set1_epi8(static_cast<char>(0x80));
    const __m256i loaded = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(ptr));
    const __m256i v_signed = _mm256_xor_si256(loaded, sign_bias);
    const __m256i t_signed = _mm256_xor_si256(
        _mm256_set1_epi8(static_cast<char>(adjusted)),
        sign_bias);
    const __m256i pass_mask = _mm256_cmpgt_epi8(v_signed, t_signed);
    const int mask = _mm256_movemask_epi8(pass_mask);
    return static_cast<std::size_t>(gnfs::util::popcount32(
        static_cast<std::uint32_t>(mask)));
}
#endif  // GNFS_THRESHOLD_SCAN_SIMD_AVX2

}  // namespace threshold_scan_detail

// ---------------------------------------------------------------------------
// Primary entry point (dispatcher).
// ---------------------------------------------------------------------------

/// Batched threshold count over a `uint8_t` span:
///   return |{ i in [0, values.size()) : values[i] >= threshold }|.
///
/// Empty input returns 0 without touching the span. The SIMD path is
/// taken when `threshold_scan_simd_enabled()` is true; otherwise falls
/// back to the scalar reference. Bit-for-bit identical output across
/// both paths for every input span and every threshold.
[[nodiscard]] inline std::size_t count_above_threshold_u8(
    std::span<const std::uint8_t> values,
    std::uint8_t threshold) noexcept {
    if (values.empty()) return 0;

    if (!threshold_scan_simd_enabled()) {
        return count_above_threshold_u8_scalar(values, threshold);
    }

#if defined(GNFS_THRESHOLD_SCAN_SIMD_NEON)
    const std::uint8_t* ptr = values.data();
    const std::size_t n = values.size();
    std::size_t count = 0;
    std::size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        count += threshold_scan_detail::count_chunk_neon(ptr + i, threshold);
    }
    for (; i < n; ++i) {
        if (ptr[i] >= threshold) ++count;
    }
    return count;
#elif defined(GNFS_THRESHOLD_SCAN_SIMD_AVX2)
    const std::uint8_t* ptr = values.data();
    const std::size_t n = values.size();
    std::size_t count = 0;
    std::size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        count += threshold_scan_detail::count_chunk_avx2(ptr + i, threshold);
    }
    for (; i < n; ++i) {
        if (ptr[i] >= threshold) ++count;
    }
    return count;
#else
    return count_above_threshold_u8_scalar(values, threshold);
#endif
}

}  // namespace gnfs::sieve
