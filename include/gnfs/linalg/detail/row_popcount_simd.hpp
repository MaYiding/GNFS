#pragma once

// SIMD-accelerated GF(2) row-wise popcount helper for row-major packed
// matrices (ARM NEON + x86 AVX2).
//
// Scope
// -----
// Given a row-major packed GF(2) matrix `matrix` laid out as
// `row_count * row_words` contiguous `uint64_t` words (i.e. row `r`
// occupies `matrix[r * row_words .. r * row_words + row_words)`),
// `per_row_popcount_words` writes the Hamming weight (total set-bit
// count) of each row to `out_row_weights[r]`. Used as a primitive by
// callers that need per-row weight tallies for column statistics,
// dependency vector parity, sparsity profiles, etc.
//
// The scalar fallback is `__builtin_popcountll` summed across each row's
// word range; the SIMD path batches 16 bytes per NEON Q-register pair
// (or 32 bytes per AVX2 256-bit lane) within each row and folds the
// per-byte popcount counters into a running 64-bit total. Row boundaries
// are respected — each row produces an independent total. Cross-row
// parallelism is the caller's responsibility (W7 / W8 / W10 / W11 family
// of `parallel_*` dispatchers already cover that axis); this helper
// stays a per-row primitive so it composes cleanly with both sequential
// and parallel outer loops.
//
// Difference from sibling SIMD primitives (helper family member #6)
// ----------------------------------------------------------------
// * `popcount_simd.hpp` (W9): flat 1-D batch popcount over a single
//   `uint64_t` span. No row dimension.
// * `and_popcnt_simd.hpp` (W10): two flat inputs, fused AND-then-popcount.
// * `xor_words_simd.hpp` (W11): two flat inputs, in-place `dst[i] ^= src[i]`.
// * `and_words_simd.hpp` (W13 T1): two flat inputs, three-arg
//   `out[i] = a[i] & b[i]`. Keeps the AND'd vector as output.
// * `xor_popcnt_simd.hpp` (W14 T1): two flat inputs, fused
//   XOR-then-popcount (Hamming distance).
// * **`row_popcount_simd.hpp` (W15 T1, this helper): one matrix input
//   with explicit row width, produces one weight per row.** First member
//   of the family that respects a 2-D layout rather than a flat span.
//
// W9 `popcount_simd.hpp::total_popcount_words` can compute the same
// answer for a single row, but cannot mass-process N rows without N
// independent calls (and N independent SIMD-gate / dispatcher reads).
// This helper reads the SIMD gate once per matrix and folds across all
// rows in one outer loop.
//
// Bit-for-bit guarantee
// ---------------------
// Per-row Hamming weight is a pure function of the row's words: SIMD
// and scalar paths return identical per-row totals, byte-for-byte. The
// SIMD path uses the same `vcntq_u8` / `_mm256_popcnt_epi64` / scalar
// `__builtin_popcountll` semantics within each row, only differing in
// reduction width. Each row is computed independently, so partial sums
// never cross row boundaries. Output ordering is preserved exactly
// (`out_row_weights[r]` always corresponds to matrix row `r`).
//
// Defensive contract
// ------------------
// * `row_words == 0`: helper returns without touching outputs (zero
//   rows worth of work).
// * `out_row_weights.size() < row_count`: clamps to the smaller span to
//   avoid UB writes past the buffer. Rows beyond the clamped range are
//   silently skipped (matching W14 T1 / W13 T1 / W11 / W10 / W9 sibling
//   defensive contracts).
// * `matrix.size() % row_words != 0`: only the integer-row prefix is
//   processed; the trailing partial row is discarded (it is not a valid
//   row in the row-major layout convention).
//
// Build-time guards: only the host-platform implementation is compiled.
// When neither NEON nor AVX2 is available
// `row_popcount_simd_supported()` returns false and the dispatcher
// silently falls back to the scalar path.

#include <atomic>
#include <cassert>
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
    #define GNFS_ROW_POPCOUNT_SIMD_NEON 1
  #endif
#endif

#if defined(__AVX2__)
  #if __has_include(<immintrin.h>)
    #include <immintrin.h>
    #define GNFS_ROW_POPCOUNT_SIMD_AVX2 1
  #endif
#endif

namespace gnfs::linalg::detail {

/// Three-state ENV gate matching the `GNFS_GF2_XOR_POPCNT_SIMD` /
/// `GNFS_GF2_AND_POPCNT_SIMD` / `GNFS_GF2_POPCNT_SIMD` /
/// `GNFS_GF2_ROW_XOR_SIMD` / `GNFS_GF2_AND_WORDS_SIMD` convention.
/// `Auto` defers to compile-time SIMD availability, `ForceOff` forces
/// the scalar path (useful for regression-bisect or sanitizer runs that
/// want to isolate kernel changes from SIMD-specific noise), and
/// `ForceOn` opts in even when `row_popcount_simd_supported()` is false
/// — in which case the dispatcher still falls back to scalar to keep
/// correctness.
enum class RowPopcountSimdMode : std::uint8_t {
    Auto = 0,
    ForceOff = 1,
    ForceOn = 2,
};

namespace row_popcount_detail {

inline std::atomic<std::uint8_t>& cached_state() noexcept {
    static std::atomic<std::uint8_t> state{static_cast<std::uint8_t>(RowPopcountSimdMode::Auto)};
    return state;
}

inline std::once_flag& cached_flag() noexcept {
    static std::once_flag flag;
    return flag;
}

inline RowPopcountSimdMode resolve_mode_from_env() noexcept {
    const char* v = std::getenv("GNFS_GF2_ROW_POPCOUNT_SIMD");
    if (v == nullptr) return RowPopcountSimdMode::Auto;
    if (std::strcmp(v, "0") == 0) return RowPopcountSimdMode::ForceOff;
    if (std::strcmp(v, "off") == 0) return RowPopcountSimdMode::ForceOff;
    if (std::strcmp(v, "1") == 0) return RowPopcountSimdMode::ForceOn;
    if (std::strcmp(v, "on") == 0) return RowPopcountSimdMode::ForceOn;
    // "auto" or anything else
    return RowPopcountSimdMode::Auto;
}

inline RowPopcountSimdMode load_mode() noexcept {
    std::call_once(cached_flag(), []() noexcept {
        cached_state().store(static_cast<std::uint8_t>(resolve_mode_from_env()),
                             std::memory_order_relaxed);
    });
    return static_cast<RowPopcountSimdMode>(cached_state().load(std::memory_order_relaxed));
}

}  // namespace row_popcount_detail

/// Compile-time SIMD availability. Constant-folds on any given build.
/// Used by the dispatcher to decide whether `Auto` / `ForceOn` actually
/// has a SIMD code path to run.
[[nodiscard]] constexpr bool row_popcount_simd_supported() noexcept {
#if defined(GNFS_ROW_POPCOUNT_SIMD_NEON) || defined(GNFS_ROW_POPCOUNT_SIMD_AVX2)
    return true;
#else
    return false;
#endif
}

/// Returns the parsed ENV state. Cached after first call.
[[nodiscard]] inline RowPopcountSimdMode row_popcount_simd_mode() noexcept {
    return row_popcount_detail::load_mode();
}

/// Dispatcher decision: should the per-row helper take the SIMD path?
/// Returns `false` when there is no compile-time SIMD support, when ENV
/// forces it off, or when ENV is `Auto` and SIMD is unavailable.
[[nodiscard]] inline bool row_popcount_simd_enabled() noexcept {
    const RowPopcountSimdMode mode = row_popcount_simd_mode();
    if (mode == RowPopcountSimdMode::ForceOff) return false;
    // ForceOn and Auto both require compile-time availability.
    return row_popcount_simd_supported();
}

/// Re-read GNFS_GF2_ROW_POPCOUNT_SIMD from the environment and refresh
/// the cached gate. Intended for unit tests that flip the env between
/// scenarios. Not called from production hot paths.
inline void row_popcount_simd_reset_env_cache_for_testing() noexcept {
    row_popcount_detail::cached_state().store(
        static_cast<std::uint8_t>(row_popcount_detail::resolve_mode_from_env()),
        std::memory_order_relaxed);
    // Ensure call_once records completion even if it has never run, so
    // a later production call does not overwrite our state.
    std::call_once(row_popcount_detail::cached_flag(), []() noexcept {});
}

// ---------------------------------------------------------------------------
// Reference scalar path (always available, used as golden for tests
// and as the no-SIMD fallback).
// ---------------------------------------------------------------------------

/// Scalar reference:
///   out_row_weights[r] = sum over k in [0, row_words) of
///                          popcount(matrix[r * row_words + k])
/// using the compiler builtin. O(row_count * row_words) ops.
///
/// Pre-conditions: `row_words` may be 0 (no work); `out_row_weights`
/// may be shorter than `matrix.size() / row_words` (clamps to the
/// smaller span); `matrix.size()` need not be a multiple of `row_words`
/// (only the integer-row prefix is processed).
inline void per_row_popcount_words_scalar(std::span<const std::uint64_t> matrix,
                                          std::size_t row_words,
                                          std::span<std::uint64_t> out_row_weights) noexcept {
    if (row_words == 0) return;
    const std::size_t row_count_from_matrix = matrix.size() / row_words;
    const std::size_t row_count =
        (out_row_weights.size() < row_count_from_matrix)
            ? out_row_weights.size()
            : row_count_from_matrix;
    for (std::size_t r = 0; r < row_count; ++r) {
        const std::uint64_t* row_ptr = matrix.data() + r * row_words;
        std::uint64_t total = 0;
        for (std::size_t k = 0; k < row_words; ++k) {
            total += static_cast<std::uint64_t>(gnfs::util::popcount64(row_ptr[k]));
        }
        out_row_weights[r] = total;
    }
}

namespace row_popcount_detail {

#if defined(GNFS_ROW_POPCOUNT_SIMD_NEON)
/// NEON inner kernel: popcount one row of `row_words` 64-bit values
/// using `vcntq_u8` to fold every byte of a 128-bit Q-register, then
/// reduce with `vaddvq_u8` per 2-word stride. Returns total Hamming
/// weight of the row.
[[nodiscard]] inline std::uint64_t per_row_popcount_neon(const std::uint64_t* row_ptr,
                                                          std::size_t row_words) noexcept {
    std::uint64_t total = 0;
    std::size_t k = 0;
    // 2-word stride: 1 NEON Q-register = 16 bytes = 2 uint64_t words.
    for (; k + 2 <= row_words; k += 2) {
        uint64x2_t v = vld1q_u64(row_ptr + k);
        uint8x16_t bytes = vreinterpretq_u8_u64(v);
        uint8x16_t counts = vcntq_u8(bytes);
        // Each lane in `counts` is in [0, 8] so the sum across 16 lanes
        // fits in [0, 128], comfortably inside the 8-bit horizontal-sum
        // intermediate of `vaddvq_u8`.
        total += static_cast<std::uint64_t>(vaddvq_u8(counts));
    }
    // Scalar tail for the odd word.
    for (; k < row_words; ++k) {
        total += static_cast<std::uint64_t>(gnfs::util::popcount64(row_ptr[k]));
    }
    return total;
}
#endif  // GNFS_ROW_POPCOUNT_SIMD_NEON

#if defined(GNFS_ROW_POPCOUNT_SIMD_AVX2)
/// AVX2 inner kernel: popcount one row of `row_words` 64-bit values
/// using `_mm256_popcnt_epi64` (AVX-512 VPOPCNTDQ + VL) when available,
/// otherwise a 4-wide `_mm_popcnt_u64` unroll. Returns total Hamming
/// weight of the row.
[[nodiscard]] inline std::uint64_t per_row_popcount_avx2(const std::uint64_t* row_ptr,
                                                          std::size_t row_words) noexcept {
    std::uint64_t total = 0;
    std::size_t k = 0;
    // 4-wide unrolled inner loop (256-bit lane = 4 uint64_t words).
    for (; k + 4 <= row_words; k += 4) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row_ptr + k));
#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512VL__)
        __m256i pc = _mm256_popcnt_epi64(v);
        total += static_cast<std::uint64_t>(_mm256_extract_epi64(pc, 0));
        total += static_cast<std::uint64_t>(_mm256_extract_epi64(pc, 1));
        total += static_cast<std::uint64_t>(_mm256_extract_epi64(pc, 2));
        total += static_cast<std::uint64_t>(_mm256_extract_epi64(pc, 3));
#else
        alignas(32) std::uint64_t lanes[4];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(lanes), v);
        total += static_cast<std::uint64_t>(_mm_popcnt_u64(lanes[0]));
        total += static_cast<std::uint64_t>(_mm_popcnt_u64(lanes[1]));
        total += static_cast<std::uint64_t>(_mm_popcnt_u64(lanes[2]));
        total += static_cast<std::uint64_t>(_mm_popcnt_u64(lanes[3]));
#endif
    }
    for (; k < row_words; ++k) {
        total += static_cast<std::uint64_t>(gnfs::util::popcount64(row_ptr[k]));
    }
    return total;
}
#endif  // GNFS_ROW_POPCOUNT_SIMD_AVX2

}  // namespace row_popcount_detail

// ---------------------------------------------------------------------------
// Primary entry point (dispatcher).
// ---------------------------------------------------------------------------

/// Per-row popcount over a row-major packed GF(2) matrix.
/// `out_row_weights[r] = popcount(matrix[r * row_words ..
/// r * row_words + row_words))`. Pre-conditions: see file header for the
/// full defensive contract. Empty input (`row_words == 0` or the
/// resulting `row_count == 0`) is a silent no-op. SIMD path is taken
/// when `row_popcount_simd_enabled()` is true; otherwise falls back to
/// the scalar reference. Bit-for-bit identical per-row totals across
/// both paths.
inline void per_row_popcount_words(std::span<const std::uint64_t> matrix,
                                   std::size_t row_words,
                                   std::span<std::uint64_t> out_row_weights) noexcept {
    if (row_words == 0) return;
    const std::size_t row_count_from_matrix = matrix.size() / row_words;
    // Defensive: the API contract requires `out_row_weights.size() >=
    // row_count_from_matrix`, but a length mismatch would otherwise
    // UB-write past `out_row_weights`. Take the smaller span to keep
    // correctness; callers should still size them equal at the call
    // site.
    const std::size_t row_count =
        (out_row_weights.size() < row_count_from_matrix)
            ? out_row_weights.size()
            : row_count_from_matrix;
    if (row_count == 0) return;

    if (!row_popcount_simd_enabled()) {
        per_row_popcount_words_scalar(matrix, row_words, out_row_weights);
        return;
    }

#if defined(GNFS_ROW_POPCOUNT_SIMD_NEON)
    const std::uint64_t* base = matrix.data();
    for (std::size_t r = 0; r < row_count; ++r) {
        out_row_weights[r] = row_popcount_detail::per_row_popcount_neon(
            base + r * row_words, row_words);
    }
#elif defined(GNFS_ROW_POPCOUNT_SIMD_AVX2)
    const std::uint64_t* base = matrix.data();
    for (std::size_t r = 0; r < row_count; ++r) {
        out_row_weights[r] = row_popcount_detail::per_row_popcount_avx2(
            base + r * row_words, row_words);
    }
#else
    per_row_popcount_words_scalar(matrix, row_words, out_row_weights);
#endif
}

}  // namespace gnfs::linalg::detail
