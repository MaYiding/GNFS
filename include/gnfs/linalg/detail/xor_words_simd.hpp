#pragma once

// SIMD-accelerated GF(2) batch row-word XOR helper
// (ARM NEON + x86 AVX2).
//
// Scope
// -----
// This header provides batched in-place XOR over two uint64_t word
// arrays: `dst[i] ^= src[i]`. Block Lanczos / Block Wiedemann inner
// kernels frequently update an accumulating row block via `dst ^= src`
// over packed GF(2) word arrays — Krylov vector recurrences, parity
// accumulation, row-block updates after multiplying by a 64-bit block
// vector all reduce to this primitive. The scalar fallback is a
// straight `for` loop with `^=` (single XOR per word on every modern
// CPU); the SIMD path batches 16 bytes at a time so the compiler can
// schedule independent NEON `veorq_u64` lanes (or AVX2 256-bit
// `_mm256_xor_si256`).
//
// The helper is intentionally stand-alone (no thread-pool, no row
// dispatch, no SpMV coupling): it consumes two flat `std::span`
// inputs (one `<uint64_t>` mutable destination, one `<const uint64_t>`
// source) and produces in-place XOR. Callers compose it into wider
// pipelines (e.g. Block Lanczos / Block Wiedemann row updates,
// dependency accumulation, parity sweeps) without paying for thread
// pool setup.
//
// Bit-for-bit guarantee
// ---------------------
// XOR is a pure function of the two input words: the SIMD path
// returns exactly the same per-word result as the scalar `dst[i] ^=
// src[i]`, no rounding, no floating point. Empty inputs (either span
// size 0) leave `dst` untouched, matching the scalar reference.
//
// Defensive clamping
// ------------------
// If `dst.size() < src.size()` only the first `dst.size()` words are
// XORed (no write past `dst`). If `src.size() < dst.size()` only the
// first `src.size()` words are XORed (the tail of `dst` is preserved
// unchanged). This contract matches the AND-popcount / popcount SIMD
// helpers in this directory.
//
// Build-time guards: only the host-platform implementation is
// compiled. When neither NEON nor AVX2 is available
// `xor_words_simd_supported()` returns false and the dispatcher
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
    #define GNFS_XOR_WORDS_SIMD_NEON 1
  #endif
#endif

#if defined(__AVX2__)
  #if __has_include(<immintrin.h>)
    #include <immintrin.h>
    #define GNFS_XOR_WORDS_SIMD_AVX2 1
  #endif
#endif

namespace gnfs::linalg::detail {

/// Three-state ENV gate matching the `GNFS_GF2_AND_POPCNT_SIMD` /
/// `GNFS_GF2_POPCNT_SIMD` / `GNFS_SPMV_SIMD` / `GNFS_BUCKET_PREFETCH`
/// convention. `Auto` defers to compile-time SIMD availability,
/// `ForceOff` forces the scalar path (useful for regression-bisect or
/// sanitizer runs that want to isolate kernel changes from
/// SIMD-specific noise), and `ForceOn` opts in even when
/// `xor_words_simd_supported()` is false — in which case the
/// dispatcher still falls back to scalar to keep correctness.
enum class XorWordsSimdMode : std::uint8_t {
    Auto = 0,
    ForceOff = 1,
    ForceOn = 2,
};

namespace xor_words_detail {

inline std::atomic<std::uint8_t>& cached_state() noexcept {
    static std::atomic<std::uint8_t> state{static_cast<std::uint8_t>(XorWordsSimdMode::Auto)};
    return state;
}

inline std::once_flag& cached_flag() noexcept {
    static std::once_flag flag;
    return flag;
}

inline XorWordsSimdMode resolve_mode_from_env() noexcept {
    const char* v = std::getenv("GNFS_GF2_ROW_XOR_SIMD");
    if (v == nullptr) return XorWordsSimdMode::Auto;
    if (std::strcmp(v, "0") == 0) return XorWordsSimdMode::ForceOff;
    if (std::strcmp(v, "off") == 0) return XorWordsSimdMode::ForceOff;
    if (std::strcmp(v, "1") == 0) return XorWordsSimdMode::ForceOn;
    if (std::strcmp(v, "on") == 0) return XorWordsSimdMode::ForceOn;
    // "auto" or anything else
    return XorWordsSimdMode::Auto;
}

inline XorWordsSimdMode load_mode() noexcept {
    std::call_once(cached_flag(), []() noexcept {
        cached_state().store(static_cast<std::uint8_t>(resolve_mode_from_env()),
                             std::memory_order_relaxed);
    });
    return static_cast<XorWordsSimdMode>(cached_state().load(std::memory_order_relaxed));
}

}  // namespace xor_words_detail

/// Compile-time SIMD availability. Constant-folds on any given build.
/// Used by the dispatcher to decide whether `Auto` / `ForceOn`
/// actually has a SIMD code path to run.
[[nodiscard]] constexpr bool xor_words_simd_supported() noexcept {
#if defined(GNFS_XOR_WORDS_SIMD_NEON) || defined(GNFS_XOR_WORDS_SIMD_AVX2)
    return true;
#else
    return false;
#endif
}

/// Returns the parsed ENV state. Cached after first call.
[[nodiscard]] inline XorWordsSimdMode xor_words_simd_mode() noexcept {
    return xor_words_detail::load_mode();
}

/// Dispatcher decision: should the batched helper take the SIMD path?
/// Returns `false` when there is no compile-time SIMD support, when
/// ENV forces it off, or when ENV is `Auto` and SIMD is unavailable.
[[nodiscard]] inline bool xor_words_simd_enabled() noexcept {
    const XorWordsSimdMode mode = xor_words_simd_mode();
    if (mode == XorWordsSimdMode::ForceOff) return false;
    // ForceOn and Auto both require compile-time availability.
    return xor_words_simd_supported();
}

/// Re-read GNFS_GF2_ROW_XOR_SIMD from the environment and refresh the
/// cached gate. Intended for unit tests that flip the env between
/// scenarios. Not called from production hot paths.
inline void xor_words_simd_reset_env_cache_for_testing() noexcept {
    xor_words_detail::cached_state().store(
        static_cast<std::uint8_t>(xor_words_detail::resolve_mode_from_env()),
        std::memory_order_relaxed);
    // Ensure call_once records completion even if it has never run,
    // so a later production call does not overwrite our state.
    std::call_once(xor_words_detail::cached_flag(), []() noexcept {});
}

// ---------------------------------------------------------------------------
// Reference scalar path (always available, used as golden for tests
// and as the no-SIMD fallback).
// ---------------------------------------------------------------------------

/// Scalar reference:
///   dst[i] ^= src[i]
/// for `i in [0, min(dst.size(), src.size()))`. O(n) ops, one XOR per
/// word on every modern CPU. Defensive contract: if `dst.size() <
/// src.size()` only the first `dst.size()` words are XORed (avoids UB
/// writes past `dst`); if `src.size() < dst.size()` only the first
/// `src.size()` words are XORed (the tail of `dst` is preserved
/// unchanged).
inline void batch_xor_words_scalar(std::span<std::uint64_t> dst,
                                   std::span<const std::uint64_t> src) noexcept {
    const std::size_t bound = (dst.size() < src.size()) ? dst.size() : src.size();
    for (std::size_t i = 0; i < bound; ++i) {
        dst[i] ^= src[i];
    }
}

namespace xor_words_detail {

#if defined(GNFS_XOR_WORDS_SIMD_NEON)
/// NEON inner kernel: load two 64-bit words from dst + src, XOR in
/// the 128-bit Q register, store back to dst. Two output words per
/// call.
inline void xor_pair_neon(std::uint64_t* dst_inout,
                          const std::uint64_t* src_in) noexcept {
    uint64x2_t vd = vld1q_u64(dst_inout);
    uint64x2_t vs = vld1q_u64(src_in);
    uint64x2_t vx = veorq_u64(vd, vs);
    vst1q_u64(dst_inout, vx);
}
#endif  // GNFS_XOR_WORDS_SIMD_NEON

#if defined(GNFS_XOR_WORDS_SIMD_AVX2)
/// AVX2 inner kernel: load four 64-bit words from dst + src, XOR in
/// the 256-bit Y register, store back to dst. Four output words per
/// call.
inline void xor_quad_avx2(std::uint64_t* dst_inout,
                          const std::uint64_t* src_in) noexcept {
    __m256i vd = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst_inout));
    __m256i vs = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src_in));
    __m256i vx = _mm256_xor_si256(vd, vs);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst_inout), vx);
}
#endif  // GNFS_XOR_WORDS_SIMD_AVX2

}  // namespace xor_words_detail

// ---------------------------------------------------------------------------
// Primary entry point (dispatcher).
// ---------------------------------------------------------------------------

/// Batch in-place XOR: `dst[i] ^= src[i]` for `i in [0,
/// min(dst.size(), src.size()))`. Empty input (either span size 0)
/// leaves `dst` untouched. SIMD path is taken when
/// `xor_words_simd_enabled()` is true; otherwise falls back to the
/// scalar reference. Bit-for-bit identical output across both paths.
///
/// Defensive contract: when `dst.size() < src.size()` the helper
/// clamps to `dst.size()` writes (avoids UB writes past `dst`). When
/// `src.size() < dst.size()` only the first `src.size()` words of
/// `dst` are XORed (tail preserved unchanged).
inline void batch_xor_words(std::span<std::uint64_t> dst,
                            std::span<const std::uint64_t> src) noexcept {
    const std::size_t bound = (dst.size() < src.size()) ? dst.size() : src.size();
    if (bound == 0) return;

    if (!xor_words_simd_enabled()) {
        batch_xor_words_scalar(dst.first(bound), src.first(bound));
        return;
    }

#if defined(GNFS_XOR_WORDS_SIMD_NEON)
    std::uint64_t* pd = dst.data();
    const std::uint64_t* ps = src.data();
    std::size_t i = 0;
    for (; i + 2 <= bound; i += 2) {
        xor_words_detail::xor_pair_neon(pd + i, ps + i);
    }
    for (; i < bound; ++i) {
        pd[i] ^= ps[i];
    }
#elif defined(GNFS_XOR_WORDS_SIMD_AVX2)
    std::uint64_t* pd = dst.data();
    const std::uint64_t* ps = src.data();
    std::size_t i = 0;
    for (; i + 4 <= bound; i += 4) {
        xor_words_detail::xor_quad_avx2(pd + i, ps + i);
    }
    for (; i < bound; ++i) {
        pd[i] ^= ps[i];
    }
#else
    batch_xor_words_scalar(dst.first(bound), src.first(bound));
#endif
}

}  // namespace gnfs::linalg::detail
