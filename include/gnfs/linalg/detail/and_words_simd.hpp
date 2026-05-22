#pragma once

// SIMD-accelerated GF(2) batch word-AND helper (ARM NEON + x86 AVX2).
//
// Scope
// -----
// This header provides batched bitwise AND of two uint64_t word arrays
// into a separate output array: `out[i] = a[i] & b[i]`. It is the
// three-argument AND counterpart of two sibling helpers in this
// directory:
//
//   * `xor_words_simd.hpp` — in-place batch XOR (`dst[i] ^= src[i]`),
//     two-argument: writes back into the destination span. Useful for
//     accumulator updates.
//   * `and_popcnt_simd.hpp` — fused batch AND-then-popcount, returning
//     per-word Hamming weights (uint32_t) or a single total reduction.
//     Useful for GF(2) inner-product / orthogonality checks where only
//     the parity / weight of the conjunction is needed.
//
// This helper (and_words_simd.hpp) keeps the conjunction as a fresh
// uint64_t output span without any reduction step. Callers compose it
// into wider pipelines that need to materialise the bitwise AND for
// downstream consumption — e.g. Block Lanczos / Block Wiedemann mask
// applications, GF(2) row intersection caches, structured Gaussian
// elimination active-column projections — without paying for thread
// pool setup or a fused reduction kernel.
//
// The scalar fallback is a straight `for` loop with `out[i] = a[i] &
// b[i]` (single AND per word on every modern CPU); the SIMD path
// batches 16 bytes at a time so the compiler can schedule independent
// NEON `vandq_u64` lanes (or AVX2 256-bit `_mm256_and_si256`).
//
// Bit-for-bit guarantee
// ---------------------
// AND is a pure function of the two input words: the SIMD path returns
// exactly the same per-word result as the scalar `out[i] = a[i] &
// b[i]`, no rounding, no floating point. Empty inputs (either input
// span size 0) leave `out` untouched, matching the scalar reference.
//
// Defensive clamping
// ------------------
// The helper writes `min(a.size(), b.size(), out.size())` output
// words. If `a.size() != b.size()` only the first `min(a.size(),
// b.size())` words are computed (avoids reading past the shorter
// input). If `out.size()` is smaller than `min(a.size(), b.size())`
// only the first `out.size()` words are written (avoids UB writes
// past `out`); the remaining tail of either input span is ignored.
// This contract matches the AND-popcount / XOR / popcount SIMD helpers
// in this directory.
//
// Build-time guards: only the host-platform implementation is
// compiled. When neither NEON nor AVX2 is available
// `and_words_simd_supported()` returns false and the dispatcher
// silently falls back to the scalar path.

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  #if __has_include(<arm_neon.h>)
    #include <arm_neon.h>
    #define GNFS_AND_WORDS_SIMD_NEON 1
  #endif
#endif

#if defined(__AVX2__)
  #if __has_include(<immintrin.h>)
    #include <immintrin.h>
    #define GNFS_AND_WORDS_SIMD_AVX2 1
  #endif
#endif

namespace gnfs::linalg::detail {

/// Three-state ENV gate matching the `GNFS_GF2_ROW_XOR_SIMD` /
/// `GNFS_GF2_AND_POPCNT_SIMD` / `GNFS_GF2_POPCNT_SIMD` / `GNFS_SPMV_SIMD`
/// / `GNFS_BUCKET_PREFETCH` convention. `Auto` defers to compile-time
/// SIMD availability, `ForceOff` forces the scalar path (useful for
/// regression-bisect or sanitizer runs that want to isolate kernel
/// changes from SIMD-specific noise), and `ForceOn` opts in even when
/// `and_words_simd_supported()` is false — in which case the
/// dispatcher still falls back to scalar to keep correctness.
enum class AndWordsSimdMode : std::uint8_t {
    Auto = 0,
    ForceOff = 1,
    ForceOn = 2,
};

namespace and_words_detail {

inline std::atomic<std::uint8_t>& cached_state() noexcept {
    static std::atomic<std::uint8_t> state{static_cast<std::uint8_t>(AndWordsSimdMode::Auto)};
    return state;
}

inline std::once_flag& cached_flag() noexcept {
    static std::once_flag flag;
    return flag;
}

inline AndWordsSimdMode resolve_mode_from_env() noexcept {
    const char* v = std::getenv("GNFS_GF2_AND_WORDS_SIMD");
    if (v == nullptr) return AndWordsSimdMode::Auto;
    if (std::strcmp(v, "0") == 0) return AndWordsSimdMode::ForceOff;
    if (std::strcmp(v, "off") == 0) return AndWordsSimdMode::ForceOff;
    if (std::strcmp(v, "1") == 0) return AndWordsSimdMode::ForceOn;
    if (std::strcmp(v, "on") == 0) return AndWordsSimdMode::ForceOn;
    // "auto" or anything else
    return AndWordsSimdMode::Auto;
}

inline AndWordsSimdMode load_mode() noexcept {
    std::call_once(cached_flag(), []() noexcept {
        cached_state().store(static_cast<std::uint8_t>(resolve_mode_from_env()),
                             std::memory_order_relaxed);
    });
    return static_cast<AndWordsSimdMode>(cached_state().load(std::memory_order_relaxed));
}

}  // namespace and_words_detail

/// Compile-time SIMD availability. Constant-folds on any given build.
/// Used by the dispatcher to decide whether `Auto` / `ForceOn`
/// actually has a SIMD code path to run.
[[nodiscard]] constexpr bool and_words_simd_supported() noexcept {
#if defined(GNFS_AND_WORDS_SIMD_NEON) || defined(GNFS_AND_WORDS_SIMD_AVX2)
    return true;
#else
    return false;
#endif
}

/// Returns the parsed ENV state. Cached after first call.
[[nodiscard]] inline AndWordsSimdMode and_words_simd_mode() noexcept {
    return and_words_detail::load_mode();
}

/// Dispatcher decision: should the batched helper take the SIMD path?
/// Returns `false` when there is no compile-time SIMD support, when
/// ENV forces it off, or when ENV is `Auto` and SIMD is unavailable.
[[nodiscard]] inline bool and_words_simd_enabled() noexcept {
    const AndWordsSimdMode mode = and_words_simd_mode();
    if (mode == AndWordsSimdMode::ForceOff) return false;
    // ForceOn and Auto both require compile-time availability.
    return and_words_simd_supported();
}

/// Re-read GNFS_GF2_AND_WORDS_SIMD from the environment and refresh the
/// cached gate. Intended for unit tests that flip the env between
/// scenarios. Not called from production hot paths.
inline void and_words_simd_reset_env_cache_for_testing() noexcept {
    and_words_detail::cached_state().store(
        static_cast<std::uint8_t>(and_words_detail::resolve_mode_from_env()),
        std::memory_order_relaxed);
    // Ensure call_once records completion even if it has never run,
    // so a later production call does not overwrite our state.
    std::call_once(and_words_detail::cached_flag(), []() noexcept {});
}

// ---------------------------------------------------------------------------
// Reference scalar path (always available, used as golden for tests
// and as the no-SIMD fallback).
// ---------------------------------------------------------------------------

/// Scalar reference:
///   out[i] = a[i] & b[i]
/// for `i in [0, min(a.size(), b.size(), out.size()))`. O(n) ops, one
/// AND per word on every modern CPU. Defensive contract: when input
/// span sizes diverge or `out` is shorter than the inputs, the helper
/// clamps to the smallest of the three to avoid UB reads past either
/// input or UB writes past `out`. The tail beyond the clamp window in
/// `out` is preserved unchanged.
inline void batch_and_words_scalar(std::span<const std::uint64_t> a,
                                   std::span<const std::uint64_t> b,
                                   std::span<std::uint64_t> out) noexcept {
    const std::size_t n_ab = (a.size() < b.size()) ? a.size() : b.size();
    const std::size_t bound = (out.size() < n_ab) ? out.size() : n_ab;
    for (std::size_t i = 0; i < bound; ++i) {
        out[i] = a[i] & b[i];
    }
}

namespace and_words_detail {

#if defined(GNFS_AND_WORDS_SIMD_NEON)
/// NEON inner kernel: load two 64-bit words from each of a + b, AND in
/// the 128-bit Q register, store the result to out. Two output words
/// per call.
inline void and_pair_neon(const std::uint64_t* a_in,
                          const std::uint64_t* b_in,
                          std::uint64_t* out) noexcept {
    uint64x2_t va = vld1q_u64(a_in);
    uint64x2_t vb = vld1q_u64(b_in);
    uint64x2_t vand = vandq_u64(va, vb);
    vst1q_u64(out, vand);
}
#endif  // GNFS_AND_WORDS_SIMD_NEON

#if defined(GNFS_AND_WORDS_SIMD_AVX2)
/// AVX2 inner kernel: load four 64-bit words from each of a + b, AND
/// in the 256-bit Y register, store the result to out. Four output
/// words per call.
inline void and_quad_avx2(const std::uint64_t* a_in,
                          const std::uint64_t* b_in,
                          std::uint64_t* out) noexcept {
    __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a_in));
    __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b_in));
    __m256i vand = _mm256_and_si256(va, vb);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out), vand);
}
#endif  // GNFS_AND_WORDS_SIMD_AVX2

}  // namespace and_words_detail

// ---------------------------------------------------------------------------
// Primary entry point (dispatcher).
// ---------------------------------------------------------------------------

/// Batch bitwise AND: `out[i] = a[i] & b[i]` for `i in [0,
/// min(a.size(), b.size(), out.size()))`. Empty input (either input
/// span size 0) leaves `out` untouched. SIMD path is taken when
/// `and_words_simd_enabled()` is true; otherwise falls back to the
/// scalar reference. Bit-for-bit identical output across both paths.
///
/// Defensive contract: when the input span sizes diverge or `out` is
/// shorter than `min(a.size(), b.size())`, the helper clamps to the
/// smallest of the three (avoids UB reads past the shorter input and
/// UB writes past `out`). The tail of `out` beyond the clamp window
/// is preserved unchanged.
inline void batch_and_words(std::span<const std::uint64_t> a,
                            std::span<const std::uint64_t> b,
                            std::span<std::uint64_t> out) noexcept {
    const std::size_t n_ab = (a.size() < b.size()) ? a.size() : b.size();
    if (n_ab == 0) return;
    const std::size_t bound = (out.size() < n_ab) ? out.size() : n_ab;
    if (bound == 0) return;

    if (!and_words_simd_enabled()) {
        batch_and_words_scalar(a.first(bound),
                               b.first(bound),
                               out.first(bound));
        return;
    }

#if defined(GNFS_AND_WORDS_SIMD_NEON)
    const std::uint64_t* pa = a.data();
    const std::uint64_t* pb = b.data();
    std::uint64_t* po = out.data();
    std::size_t i = 0;
    for (; i + 2 <= bound; i += 2) {
        and_words_detail::and_pair_neon(pa + i, pb + i, po + i);
    }
    for (; i < bound; ++i) {
        po[i] = pa[i] & pb[i];
    }
#elif defined(GNFS_AND_WORDS_SIMD_AVX2)
    const std::uint64_t* pa = a.data();
    const std::uint64_t* pb = b.data();
    std::uint64_t* po = out.data();
    std::size_t i = 0;
    for (; i + 4 <= bound; i += 4) {
        and_words_detail::and_quad_avx2(pa + i, pb + i, po + i);
    }
    for (; i < bound; ++i) {
        po[i] = pa[i] & pb[i];
    }
#else
    batch_and_words_scalar(a.first(bound),
                           b.first(bound),
                           out.first(bound));
#endif
}

}  // namespace gnfs::linalg::detail
