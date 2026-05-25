#pragma once

// SIMD-accelerated GF(2) word popcount batch helpers (ARM NEON + x86 AVX2).
//
// Scope
// -----
// This header provides batched popcount over uint64_t word arrays, where
// the per-word output is the Hamming weight of that word. GF(2) parity /
// distance metric / column-weight tally are all common uses inside the
// Block Lanczos / Block Wiedemann pipelines, where the matrix is laid out
// as packed 64-bit words. The scalar fallback uses `__builtin_popcountll`
// (single-instruction on M-series ARM64 and x86_64 with `-mpopcnt`); the
// SIMD path batches 16 bytes at a time so the compiler can schedule
// independent NEON `vcnt_u8` lanes (or AVX2 256-bit `_mm256_popcnt_epi64`
// when the AVX-512 popcount extension is available, otherwise the LUT
// shuffle method).
//
// The helpers are intentionally stand-alone (no thread-pool, no row
// dispatch, no SpMV coupling): they consume a flat `std::span<const
// uint64_t>` and produce either an element-wise `uint32_t` output span
// or a single `uint64_t` total sum. Callers compose them into wider
// pipelines (e.g. matrix column-weight tally, dependency-vector Hamming
// distance, parity check) without paying for thread pool setup.
//
// Bit-for-bit guarantee
// ---------------------
// Popcount is a pure function of its input word: the SIMD path returns
// exactly the same per-word weight as `__builtin_popcountll`, no rounding,
// no floating point. The reduction in `total_popcount_words` is an integer
// sum of pure `uint32_t` weights, so the total is also bit-for-bit equal
// regardless of the SIMD lane count. Empty inputs return empty outputs /
// zero totals without touching pointers, matching the scalar reference.
//
// Build-time guards: only the host-platform implementation is compiled.
// When neither NEON nor AVX2 is available `popcount_simd_supported()`
// returns false and the dispatcher silently falls back to the scalar path.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include "../../util/bit_intrin.hpp"

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  #if __has_include(<arm_neon.h>)
    #include <arm_neon.h>
    #define GNFS_POPCNT_SIMD_NEON 1
  #endif
#endif

#if defined(__AVX2__)
  #if __has_include(<immintrin.h>)
    #include <immintrin.h>
    #define GNFS_POPCNT_SIMD_AVX2 1
  #endif
#endif

namespace gnfs::linalg::detail {

/// Three-state ENV gate matching the `GNFS_SPMV_SIMD` / `GNFS_BUCKET_PREFETCH`
/// convention. `Auto` defers to compile-time SIMD availability, `ForceOff`
/// forces the scalar path (useful for regression-bisect or sanitizer runs
/// that want to isolate kernel changes from SIMD-specific noise), and
/// `ForceOn` opts in even when `popcount_simd_supported()` is false — in
/// which case the dispatcher still falls back to scalar to keep correctness.
enum class PopcountSimdMode : std::uint8_t {
    Auto = 0,
    ForceOff = 1,
    ForceOn = 2,
};

namespace popcnt_detail {

inline std::atomic<std::uint8_t>& cached_state() noexcept {
    static std::atomic<std::uint8_t> state{static_cast<std::uint8_t>(PopcountSimdMode::Auto)};
    return state;
}

inline std::once_flag& cached_flag() noexcept {
    static std::once_flag flag;
    return flag;
}

inline PopcountSimdMode resolve_mode_from_env() noexcept {
    const char* v = std::getenv("GNFS_GF2_POPCNT_SIMD");
    if (v == nullptr) return PopcountSimdMode::Auto;
    if (std::strcmp(v, "0") == 0) return PopcountSimdMode::ForceOff;
    if (std::strcmp(v, "1") == 0) return PopcountSimdMode::ForceOn;
    // "auto" or anything else
    return PopcountSimdMode::Auto;
}

inline PopcountSimdMode load_mode() noexcept {
    std::call_once(cached_flag(), []() noexcept {
        cached_state().store(static_cast<std::uint8_t>(resolve_mode_from_env()),
                             std::memory_order_relaxed);
    });
    return static_cast<PopcountSimdMode>(cached_state().load(std::memory_order_relaxed));
}

}  // namespace popcnt_detail

/// Compile-time SIMD availability. Constant-folds on any given build.
/// Used by the dispatcher to decide whether `Auto` / `ForceOn` actually
/// has a SIMD code path to run.
[[nodiscard]] constexpr bool popcount_simd_supported() noexcept {
#if defined(GNFS_POPCNT_SIMD_NEON) || defined(GNFS_POPCNT_SIMD_AVX2)
    return true;
#else
    return false;
#endif
}

/// Returns the parsed ENV state. Cached after first call.
[[nodiscard]] inline PopcountSimdMode popcount_simd_mode() noexcept {
    return popcnt_detail::load_mode();
}

/// Dispatcher decision: should the batched helper take the SIMD path?
/// Returns `false` when there is no compile-time SIMD support, when ENV
/// forces it off, or when ENV is `Auto` and SIMD is unavailable.
[[nodiscard]] inline bool popcount_simd_enabled() noexcept {
    const PopcountSimdMode mode = popcount_simd_mode();
    if (mode == PopcountSimdMode::ForceOff) return false;
    // ForceOn and Auto both require compile-time availability.
    return popcount_simd_supported();
}

/// Re-read GNFS_GF2_POPCNT_SIMD from the environment and refresh the
/// cached gate. Intended for unit tests that flip the env between
/// scenarios. Not called from production hot paths.
inline void popcount_simd_reset_env_cache_for_testing() noexcept {
    popcnt_detail::cached_state().store(
        static_cast<std::uint8_t>(popcnt_detail::resolve_mode_from_env()),
        std::memory_order_relaxed);
    // Ensure call_once records completion even if it has never run, so a
    // later production call does not overwrite our state.
    std::call_once(popcnt_detail::cached_flag(), []() noexcept {});
}

// ---------------------------------------------------------------------------
// Reference scalar paths (always available, used as golden for tests and
// as the no-SIMD fallback).
// ---------------------------------------------------------------------------

/// Scalar reference: out[i] = popcount(words[i]) using the compiler
/// builtin. O(n) ops; one instruction per word on M-series ARM64 (CNT) and
/// on x86_64 with the POPCNT extension. Pre-conditions: `out.size() ==
/// words.size()`.
inline void batch_popcount_words_scalar(std::span<const std::uint64_t> words,
                                        std::span<std::uint32_t> out) noexcept {
    const std::size_t n = words.size();
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::uint32_t>(gnfs::util::popcount64(words[i]));
    }
}

/// Scalar reference: total = sum over i of popcount(words[i]). Accumulated
/// in uint64_t to avoid overflow for any conceivable n (each per-word
/// weight is in [0, 64], so even 2^58 words fit without overflow).
[[nodiscard]] inline std::uint64_t
total_popcount_words_scalar(std::span<const std::uint64_t> words) noexcept {
    std::uint64_t total = 0;
    const std::size_t n = words.size();
    for (std::size_t i = 0; i < n; ++i) {
        total += static_cast<std::uint64_t>(gnfs::util::popcount64(words[i]));
    }
    return total;
}

namespace popcnt_detail {

#if defined(GNFS_POPCNT_SIMD_NEON)
/// NEON inner kernel: popcount two 64-bit words via byte-level `vcnt_u8`
/// followed by `vaddvq_u8` horizontal sum across the 8-byte half. This
/// matches what the compiler's autovectoriser generally produces for a
/// 2-iter unroll, but writing it explicitly guarantees the layout and
/// lets us share the load between two adjacent words to amortise the
/// load latency.
inline void popcount_pair_neon(const std::uint64_t* in,
                               std::uint32_t* out) noexcept {
    // Load two contiguous 64-bit words as a uint64x2_t (16 B = one Q reg).
    uint64x2_t v = vld1q_u64(in);
    // Reinterpret as 16 bytes; vcntq_u8 emits CNT per byte (0..8 bits).
    uint8x16_t bytes = vreinterpretq_u8_u64(v);
    uint8x16_t counts = vcntq_u8(bytes);
    // Sum the lower 8 bytes (word 0) and the upper 8 bytes (word 1).
    // vaddv_u8 across an 8-byte d-register returns the byte sum (0..64).
    uint8x8_t lo = vget_low_u8(counts);
    uint8x8_t hi = vget_high_u8(counts);
    out[0] = static_cast<std::uint32_t>(vaddv_u8(lo));
    out[1] = static_cast<std::uint32_t>(vaddv_u8(hi));
}

/// NEON path for the total sum: accumulate 2-word popcounts in a
/// uint64_t total. Reduces twice per Q-register load.
[[nodiscard]] inline std::uint64_t total_popcount_neon(std::span<const std::uint64_t> words) noexcept {
    std::uint64_t total = 0;
    const std::size_t n = words.size();
    const std::uint64_t* p = words.data();
    std::size_t i = 0;
    for (; i + 2 <= n; i += 2) {
        uint64x2_t v = vld1q_u64(p + i);
        uint8x16_t bytes = vreinterpretq_u8_u64(v);
        uint8x16_t counts = vcntq_u8(bytes);
        // Single horizontal sum across 16 bytes (both words combined).
        total += static_cast<std::uint64_t>(vaddvq_u8(counts));
    }
    for (; i < n; ++i) {
        total += static_cast<std::uint64_t>(gnfs::util::popcount64(p[i]));
    }
    return total;
}
#endif  // GNFS_POPCNT_SIMD_NEON

#if defined(GNFS_POPCNT_SIMD_AVX2)
/// AVX2 inner kernel: when AVX-512 popcount extension is available we use
/// `_mm256_popcnt_epi64` (4 word/cycle). Otherwise we fall back to the
/// LUT-based "Mula" shuffle method, which is what the BWMI tables and
/// libpopcnt-style libraries use. Either way we emit one 4-lane store
/// per call.
inline void popcount_quad_avx2(const std::uint64_t* in,
                               std::uint32_t* out) noexcept {
#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512VL__)
    // AVX-512 VPOPCNTDQ + VL: native 256-bit popcount in one instruction.
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in));
    __m256i pc = _mm256_popcnt_epi64(v);
    // Extract four 64-bit lanes; cast to uint32_t (count <= 64).
    out[0] = static_cast<std::uint32_t>(_mm256_extract_epi64(pc, 0));
    out[1] = static_cast<std::uint32_t>(_mm256_extract_epi64(pc, 1));
    out[2] = static_cast<std::uint32_t>(_mm256_extract_epi64(pc, 2));
    out[3] = static_cast<std::uint32_t>(_mm256_extract_epi64(pc, 3));
#else
    // Fallback: scalar popcnt instruction in a 4-unrolled loop. `_mm_popcnt_u64`
    // is one instruction on every x86_64 CPU with POPCNT (Nehalem+); the
    // unrolling exposes the four independent 64-bit ops to the OoO pipeline
    // for better throughput than a serial loop.
    out[0] = static_cast<std::uint32_t>(_mm_popcnt_u64(in[0]));
    out[1] = static_cast<std::uint32_t>(_mm_popcnt_u64(in[1]));
    out[2] = static_cast<std::uint32_t>(_mm_popcnt_u64(in[2]));
    out[3] = static_cast<std::uint32_t>(_mm_popcnt_u64(in[3]));
#endif
}

[[nodiscard]] inline std::uint64_t total_popcount_avx2(std::span<const std::uint64_t> words) noexcept {
    std::uint64_t total = 0;
    const std::size_t n = words.size();
    const std::uint64_t* p = words.data();
    std::size_t i = 0;
    // 4-wide unrolled inner loop.
    for (; i + 4 <= n; i += 4) {
#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512VL__)
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i));
        __m256i pc = _mm256_popcnt_epi64(v);
        total += static_cast<std::uint64_t>(_mm256_extract_epi64(pc, 0));
        total += static_cast<std::uint64_t>(_mm256_extract_epi64(pc, 1));
        total += static_cast<std::uint64_t>(_mm256_extract_epi64(pc, 2));
        total += static_cast<std::uint64_t>(_mm256_extract_epi64(pc, 3));
#else
        total += static_cast<std::uint64_t>(_mm_popcnt_u64(p[i]));
        total += static_cast<std::uint64_t>(_mm_popcnt_u64(p[i + 1]));
        total += static_cast<std::uint64_t>(_mm_popcnt_u64(p[i + 2]));
        total += static_cast<std::uint64_t>(_mm_popcnt_u64(p[i + 3]));
#endif
    }
    for (; i < n; ++i) {
        total += static_cast<std::uint64_t>(gnfs::util::popcount64(p[i]));
    }
    return total;
}
#endif  // GNFS_POPCNT_SIMD_AVX2

}  // namespace popcnt_detail

// ---------------------------------------------------------------------------
// Primary entry points (dispatcher).
// ---------------------------------------------------------------------------

/// Batch popcount: `out[i] = popcount(words[i])` for `i in [0, words.size())`.
/// Pre-conditions: `out.size() == words.size()`. Empty input returns
/// without touching either span. SIMD path is taken when
/// `popcount_simd_enabled()` is true; otherwise falls back to the scalar
/// reference. Bit-for-bit identical output across both paths.
inline void batch_popcount_words(std::span<const std::uint64_t> words,
                                 std::span<std::uint32_t> out) noexcept {
    const std::size_t n = words.size();
    if (n == 0) return;
    // Defensive: the API contract requires `out.size() == words.size()`,
    // but a length mismatch would otherwise UB-write past `out`. Take
    // the smaller span to keep correctness; callers should still size
    // them equal at the call site.
    const std::size_t bound = (out.size() < n) ? out.size() : n;
    if (bound == 0) return;

    if (!popcount_simd_enabled()) {
        batch_popcount_words_scalar(words.first(bound),
                                    out.first(bound));
        return;
    }

#if defined(GNFS_POPCNT_SIMD_NEON)
    const std::uint64_t* p = words.data();
    std::uint32_t* o = out.data();
    std::size_t i = 0;
    for (; i + 2 <= bound; i += 2) {
        popcnt_detail::popcount_pair_neon(p + i, o + i);
    }
    for (; i < bound; ++i) {
        o[i] = static_cast<std::uint32_t>(gnfs::util::popcount64(p[i]));
    }
#elif defined(GNFS_POPCNT_SIMD_AVX2)
    const std::uint64_t* p = words.data();
    std::uint32_t* o = out.data();
    std::size_t i = 0;
    for (; i + 4 <= bound; i += 4) {
        popcnt_detail::popcount_quad_avx2(p + i, o + i);
    }
    for (; i < bound; ++i) {
        o[i] = static_cast<std::uint32_t>(gnfs::util::popcount64(p[i]));
    }
#else
    batch_popcount_words_scalar(words.first(bound),
                                out.first(bound));
#endif
}

/// Total popcount: returns the sum of `popcount(words[i])` over all `i`.
/// Empty input returns zero. SIMD path is taken when
/// `popcount_simd_enabled()` is true; otherwise falls back to the scalar
/// reference. Bit-for-bit identical total across both paths.
[[nodiscard]] inline std::uint64_t
total_popcount_words(std::span<const std::uint64_t> words) noexcept {
    if (words.empty()) return 0;
    if (!popcount_simd_enabled()) {
        return total_popcount_words_scalar(words);
    }
#if defined(GNFS_POPCNT_SIMD_NEON)
    return popcnt_detail::total_popcount_neon(words);
#elif defined(GNFS_POPCNT_SIMD_AVX2)
    return popcnt_detail::total_popcount_avx2(words);
#else
    return total_popcount_words_scalar(words);
#endif
}

}  // namespace gnfs::linalg::detail
