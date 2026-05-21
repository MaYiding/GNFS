#pragma once

// Cache-blocked tile transpose for word-packed GF(2) matrices.
//
// Scope
// -----
// This helper transposes a dense bit-packed GF(2) matrix laid out as a
// flat `uint64_t*` buffer where each row occupies `(cols + 63) / 64`
// contiguous words, low-order bits packed into low-indexed columns. The
// destination is laid out symmetrically: a `cols × rows` matrix whose
// rows are `(rows + 63) / 64` words wide.
//
// Two paths exist:
//
// 1. Naive scalar reference (`transpose_naive_gf2`): one bit at a time,
//    O(rows * cols) bit operations. Used as the golden reference in unit
//    tests and as the fallback when the matrix dimensions are too small
//    to amortise the tile setup cost.
//
// 2. Tile-blocked path (`transpose_blocked_gf2_impl`): partitions the
//    matrix into 64x64 tiles, transposes each tile in-register using the
//    standard 6-stage "Hacker's Delight" word-swap reduction (see
//    Warren, Hacker's Delight 2nd ed. ch. 7-3), then scatters the tile
//    columns into the destination rows. Each tile read costs 64 word
//    loads and each tile write costs 64 word stores — both contiguous
//    in their respective row strides, which yields one full 64 B cache
//    line per tile boundary instead of the row-stride strides that the
//    naive path produces. The tile_size constant is fixed at 64 because
//    the swap reduction is built around the GF(2) word size; raising it
//    would require a different transpose primitive (e.g. 128 with SSE
//    intrinsics, not currently used in this codebase).
//
// Correctness
// -----------
// Both paths produce the same bit matrix. The blocked path is bit-for-bit
// identical to the naive path for every input — the swap-reduction is a
// pure permutation that has been proven correct for 64-bit words for
// decades, and the tile scheduling never reads or writes outside the
// declared (rows, cols) range. The runtime gate exists only so that
// regression-bisect or PMU-sweep investigations can force one path or
// the other; production runs should leave it at the default (auto).
//
// Layout
// ------
// * Source has shape `rows × cols`. Row i starts at `src + i * src_wpr`,
//   where `src_wpr = (cols + 63) / 64`. Bit j of row i is in word
//   `src[i * src_wpr + j / 64]`, bit position `j & 63`. Bits past
//   column `cols - 1` inside the last word are required to be zero by
//   the caller (the helper writes zeros to the padding bits regardless,
//   but reading garbage would corrupt the transpose).
// * Destination has shape `cols × rows`. Row j starts at
//   `dst + j * dst_wpr`, where `dst_wpr = (rows + 63) / 64`. Bit i of
//   row j is in word `dst[j * dst_wpr + i / 64]`, bit position `i & 63`.
// * The destination buffer must be sized for at least `cols * dst_wpr`
//   uint64_t entries and must be pre-zeroed by the caller. The helper
//   writes only the bits it produces; pre-zeroing guarantees no stale
//   contents leak through.
//
// ENV gate
// --------
//
//   GNFS_MATRIX_TRANSPOSE_BLOCKED=0     disabled (force naive scalar)
//   GNFS_MATRIX_TRANSPOSE_BLOCKED=1     enabled regardless of size
//   GNFS_MATRIX_TRANSPOSE_BLOCKED=auto  default: enabled when
//                                       max(rows, cols) >= 128
//   (unset)                              same as auto
//   other                                treated as auto
//
// The runtime decision is cached after the first call so the hot path
// only pays for an atomic-relaxed load. Tests that toggle the env mid
// process call `reload_matrix_transpose_blocked_for_testing()` to flush
// the cache.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace gnfs::linalg::detail {

/// Tile dimension in bits. Fixed at 64 because the in-register transpose
/// primitive (`transpose_64x64_inplace`) is built around the native word
/// size. A 32-bit fallback would halve the tile and double the loop count.
inline constexpr std::size_t kTransposeTileBits = 64;

/// Auto-threshold: enable the blocked path when either dimension is at
/// least this many bits. Below the threshold the naive path is faster
/// because the per-tile setup cost dominates over the linear scan.
inline constexpr std::size_t kTransposeAutoThreshold = 128;

namespace transpose_detail {

/// Three-state ENV parser. Cached via `cached_state` + `cached_flag` so
/// the env read happens once per process. The cache holds the "force-on"
/// flag only; the auto decision still depends on per-call dimensions.
///
/// Returned enum encodes:
///   ForceOff    GNFS_MATRIX_TRANSPOSE_BLOCKED=0
///   ForceOn     GNFS_MATRIX_TRANSPOSE_BLOCKED=1
///   Auto        unset, "auto", or any other value (size-gated)
enum class GateMode : std::uint8_t {
    Auto = 0,
    ForceOff = 1,
    ForceOn = 2,
};

inline std::atomic<std::uint8_t>& cached_state() noexcept {
    static std::atomic<std::uint8_t> state{static_cast<std::uint8_t>(GateMode::Auto)};
    return state;
}

inline std::once_flag& cached_flag() noexcept {
    static std::once_flag flag;
    return flag;
}

inline GateMode resolve_gate_from_env() noexcept {
    const char* v = std::getenv("GNFS_MATRIX_TRANSPOSE_BLOCKED");
    if (v == nullptr) return GateMode::Auto;
    if (std::strcmp(v, "0") == 0) return GateMode::ForceOff;
    if (std::strcmp(v, "1") == 0) return GateMode::ForceOn;
    return GateMode::Auto;  // "auto" or anything else
}

inline GateMode load_gate_mode() noexcept {
    std::call_once(cached_flag(), []() noexcept {
        cached_state().store(static_cast<std::uint8_t>(resolve_gate_from_env()),
                             std::memory_order_relaxed);
    });
    return static_cast<GateMode>(cached_state().load(std::memory_order_relaxed));
}

/// Transpose a 64x64 GF(2) bit matrix in place. Standard 6-stage
/// "Hacker's Delight" reduction: at stage s the matrix is swapped along
/// a diagonal of width 2^(5-s), so after stages 0..5 every bit ends up
/// in its mirrored position. Each stage costs 32 XORs + 32 AND/shift
/// pairs, so the whole transpose is 192 XOR ops on 64-bit registers.
///
/// Input/output convention: `a[i]` is row i; bit j of row i is
/// `(a[i] >> j) & 1`. After the call, what was bit j of row i becomes
/// bit i of row j.
inline void transpose_64x64_inplace(std::uint64_t a[64]) noexcept {
    // Stage masks: each m_s isolates the upper (or lower) half of every
    // 2^s-row band that needs to swap with its counterpart 2^s rows away.
    constexpr std::uint64_t m1 = 0x5555555555555555ULL;  // 0101...
    constexpr std::uint64_t m2 = 0x3333333333333333ULL;  // 0011 0011...
    constexpr std::uint64_t m4 = 0x0f0f0f0f0f0f0f0fULL;  // 00001111...
    constexpr std::uint64_t m8 = 0x00ff00ff00ff00ffULL;
    constexpr std::uint64_t m16 = 0x0000ffff0000ffffULL;
    constexpr std::uint64_t m32 = 0x00000000ffffffffULL;

    // Stage 1: swap bit-pairs across rows {0,1}, {2,3}, {4,5}, ...
    for (std::size_t i = 0; i < 64; i += 2) {
        std::uint64_t t = ((a[i] >> 1) ^ a[i + 1]) & m1;
        a[i] ^= t << 1;
        a[i + 1] ^= t;
    }
    // Stage 2: swap 2-bit groups across row blocks of 4.
    for (std::size_t i = 0; i < 64; i += 4) {
        std::uint64_t t0 = ((a[i] >> 2) ^ a[i + 2]) & m2;
        a[i] ^= t0 << 2;
        a[i + 2] ^= t0;
        std::uint64_t t1 = ((a[i + 1] >> 2) ^ a[i + 3]) & m2;
        a[i + 1] ^= t1 << 2;
        a[i + 3] ^= t1;
    }
    // Stage 3: swap 4-bit groups across row blocks of 8.
    for (std::size_t i = 0; i < 64; i += 8) {
        for (std::size_t j = 0; j < 4; ++j) {
            std::uint64_t t = ((a[i + j] >> 4) ^ a[i + 4 + j]) & m4;
            a[i + j] ^= t << 4;
            a[i + 4 + j] ^= t;
        }
    }
    // Stage 4: swap 8-bit groups across row blocks of 16.
    for (std::size_t i = 0; i < 64; i += 16) {
        for (std::size_t j = 0; j < 8; ++j) {
            std::uint64_t t = ((a[i + j] >> 8) ^ a[i + 8 + j]) & m8;
            a[i + j] ^= t << 8;
            a[i + 8 + j] ^= t;
        }
    }
    // Stage 5: swap 16-bit groups across row blocks of 32.
    for (std::size_t i = 0; i < 64; i += 32) {
        for (std::size_t j = 0; j < 16; ++j) {
            std::uint64_t t = ((a[i + j] >> 16) ^ a[i + 16 + j]) & m16;
            a[i + j] ^= t << 16;
            a[i + 16 + j] ^= t;
        }
    }
    // Stage 6: swap 32-bit halves across the full block.
    for (std::size_t j = 0; j < 32; ++j) {
        std::uint64_t t = ((a[j] >> 32) ^ a[j + 32]) & m32;
        a[j] ^= t << 32;
        a[j + 32] ^= t;
    }
}

}  // namespace transpose_detail

/// Naive scalar reference: copy bit (i, j) of src into bit (j, i) of dst
/// one bit at a time. O(rows * cols) ops. Used as the golden reference in
/// tests and as the fallback when dimensions are below the auto threshold.
///
/// Preconditions:
///   * `src` has length at least `rows * ((cols + 63) / 64)` words.
///   * `dst` has length at least `cols * ((rows + 63) / 64)` words and is
///     pre-zeroed by the caller.
///   * `src` and `dst` do not overlap.
inline void transpose_naive_gf2(const std::uint64_t* src,
                                std::uint64_t* dst,
                                std::size_t rows,
                                std::size_t cols) noexcept {
    if (rows == 0 || cols == 0) return;
    const std::size_t src_wpr = (cols + 63) / 64;
    const std::size_t dst_wpr = (rows + 63) / 64;
    for (std::size_t i = 0; i < rows; ++i) {
        const std::uint64_t* row = src + i * src_wpr;
        for (std::size_t j = 0; j < cols; ++j) {
            std::uint64_t bit = (row[j >> 6] >> (j & 63)) & 1ULL;
            if (bit != 0) {
                dst[j * dst_wpr + (i >> 6)] |= (1ULL << (i & 63));
            }
        }
    }
}

namespace transpose_detail {

/// Tile-blocked path. Walks the source in 64x64 tiles, transposes each
/// tile in registers, and writes the columns of the tile as rows of the
/// destination. Edge tiles (last block-row, last block-col) are handled
/// by zero-padding into a stack-resident tile buffer so the in-register
/// transpose primitive can run unchanged; the writeback step masks the
/// out-of-range bits so we never write past the declared dimensions.
inline void transpose_blocked_gf2_impl(const std::uint64_t* src,
                                       std::uint64_t* dst,
                                       std::size_t rows,
                                       std::size_t cols) noexcept {
    if (rows == 0 || cols == 0) return;
    const std::size_t src_wpr = (cols + 63) / 64;
    const std::size_t dst_wpr = (rows + 63) / 64;

    // Number of full 64-row / 64-col tile bands. Edge bands handled below.
    for (std::size_t bi = 0; bi < rows; bi += kTransposeTileBits) {
        const std::size_t tile_rows = (bi + kTransposeTileBits <= rows)
                                          ? kTransposeTileBits
                                          : (rows - bi);
        for (std::size_t bj = 0; bj < cols; bj += kTransposeTileBits) {
            const std::size_t tile_cols = (bj + kTransposeTileBits <= cols)
                                              ? kTransposeTileBits
                                              : (cols - bj);
            // Load tile into a stack buffer, zero-padding the unused rows
            // and unused high bits. After load, tile[i] holds the bits
            // (bi + i, bj .. bj + tile_cols - 1) of the source.
            std::uint64_t tile[kTransposeTileBits];
            std::memset(tile, 0, sizeof(tile));
            const std::size_t src_word_off = bj / 64;  // tiles are 64-aligned
            for (std::size_t i = 0; i < tile_rows; ++i) {
                std::uint64_t w = src[(bi + i) * src_wpr + src_word_off];
                if (tile_cols < 64) {
                    w &= (tile_cols == 64) ? ~0ULL : ((1ULL << tile_cols) - 1);
                }
                tile[i] = w;
            }
            // Transpose in place: tile[j] now holds bits (bj + j, bi ..)
            // for j in [0, 64). The high bits (>= tile_rows) come from
            // the zero-padded source rows and are masked off below.
            transpose_64x64_inplace(tile);
            // Scatter: write tile[j] (j-th destination row's word that
            // covers source rows bi .. bi + tile_rows - 1) into dst.
            const std::size_t dst_word_off = bi / 64;  // tiles are 64-aligned
            for (std::size_t j = 0; j < tile_cols; ++j) {
                std::uint64_t w = tile[j];
                if (tile_rows < 64) {
                    w &= (tile_rows == 64) ? ~0ULL : ((1ULL << tile_rows) - 1);
                }
                dst[(bj + j) * dst_wpr + dst_word_off] |= w;
            }
        }
    }
}

}  // namespace transpose_detail

/// Returns the configured gate mode (Auto / ForceOff / ForceOn). Cached
/// after first call. Production code should prefer
/// `matrix_transpose_blocked_enabled(rows, cols)` instead — this getter
/// is exposed only for tests that need to read the parsed state.
[[nodiscard]] inline transpose_detail::GateMode matrix_transpose_blocked_mode() noexcept {
    return transpose_detail::load_gate_mode();
}

/// Returns whether the blocked path should run for the given dimensions
/// under the currently-cached gate setting. Three-state decision:
///   ForceOff             → false (always naive)
///   ForceOn              → true  (always blocked)
///   Auto, dims < threshold → false (naive faster on small matrices)
///   Auto, dims >= threshold → true (blocked amortises tile setup)
[[nodiscard]] inline bool matrix_transpose_blocked_enabled(std::size_t rows,
                                                           std::size_t cols) noexcept {
    using transpose_detail::GateMode;
    const GateMode mode = transpose_detail::load_gate_mode();
    if (mode == GateMode::ForceOff) return false;
    if (mode == GateMode::ForceOn) return true;
    // Auto: at least one dimension must exceed the threshold.
    return rows >= kTransposeAutoThreshold || cols >= kTransposeAutoThreshold;
}

/// Re-read GNFS_MATRIX_TRANSPOSE_BLOCKED from the environment and refresh
/// the cached gate. Intended for unit tests that flip the env between
/// scenarios. Not called from production hot paths.
inline void reload_matrix_transpose_blocked_for_testing() noexcept {
    transpose_detail::cached_state().store(
        static_cast<std::uint8_t>(transpose_detail::resolve_gate_from_env()),
        std::memory_order_relaxed);
    // Ensure `call_once` records completion even if it has never run
    // before, so a later production call does not overwrite our state.
    std::call_once(transpose_detail::cached_flag(), []() noexcept {});
}

/// Primary entry point: transpose `src` (rows × cols GF(2) matrix) into
/// `dst` (cols × rows). The routing decision is taken once per call from
/// the cached ENV gate. The dst buffer MUST be pre-zeroed by the caller
/// because both paths OR bits into it (the blocked path does so to amortise
/// the per-tile mask; the naive path does so to skip the zero-bit branch).
///
/// Preconditions:
///   * `src` and `dst` do not overlap.
///   * `dst` has been zeroed before this call.
///   * For non-empty dimensions, `src` has at least `rows * ((cols + 63) / 64)`
///     words and `dst` has at least `cols * ((rows + 63) / 64)` words.
///
/// Empty dimensions: `rows == 0` or `cols == 0` returns immediately without
/// touching either buffer.
inline void transpose_blocked_gf2(const std::uint64_t* src,
                                  std::uint64_t* dst,
                                  std::size_t rows,
                                  std::size_t cols) noexcept {
    if (rows == 0 || cols == 0) return;
    if (matrix_transpose_blocked_enabled(rows, cols)) {
        transpose_detail::transpose_blocked_gf2_impl(src, dst, rows, cols);
    } else {
        transpose_naive_gf2(src, dst, rows, cols);
    }
}

}  // namespace gnfs::linalg::detail
