#pragma once

// SIMD-accelerated batch lattice-coordinate projection helper for the
// lattice sieve (ARM NEON + x86 AVX2).
//
// Scope
// -----
// During lattice sieve `sieve_bucket_region` setup, each candidate cell
// `(i, j)` produced by the bucket sieve must be projected back to lattice
// coordinates `(a, b)` via the reduced basis vectors `b1 = (b1x, b1y)` and
// `b2 = (b2x, b2y)`:
//
//     a = b1x * i + b2x * j
//     b = b1y * i + b2y * j
//
// Today every call site computes those two coordinates one cell at a time
// in scalar code (typically inside the norm precompute pass and again
// during candidate emission), which leaves per-iteration address-gen
// pressure on the table when the same `(b1, b2)` is broadcast across an
// entire row tile. This header exposes a batched kernel that loads
// 2 (NEON) or 4 (AVX2) `(i, j)` pairs per iteration into vector registers,
// performs the scalar `int64_t` multiplies in the GPR file (NEON has no
// `vmulq_s64` lane on Apple Silicon; AVX2 requires AVX-512 DQ for
// `_mm256_mullo_epi64`), then writes the consolidated `(a, b)` pair back
// through `vst1q_s64` / `_mm256_storeu_si256`. The SIMD value is in (a)
// the bulk load / store of the cell index streams, (b) reduced address-gen
// pressure in the inner loop, and (c) the compiler's freedom to schedule
// independent scalar multiplies across the lane accumulators.
//
// Pure header, no dependencies beyond the standard library and the
// platform intrinsics. The helper is intentionally stand-alone: it does
// not touch the bucket sieve, the candidate emission path, or the
// norm-precompute loop, and is opt-in future infrastructure.
//
// Bit-for-bit guarantee
// ---------------------
// Each lattice coordinate is a fixed linear combination of `(i, j)` with
// signed `int64_t` arithmetic. The SIMD path runs the same scalar
// `int64_t` multiplies and adds per lane that the scalar reference would
// run, so for inputs that do not overflow `int64_t` (caller's
// responsibility — typical sieve regions are well under the bound), both
// paths produce per-index identical `(a, b)` output. Signed wrap-around
// behaviour is well defined under `-fwrapv` (GCC and Clang assume this in
// nominal builds; the project uses default optimisation which preserves
// the two's-complement wrap). Empty input (either `i_coords` or
// `j_coords` size 0) leaves the output spans untouched. Mismatched
// `i_coords.size() != j_coords.size()` is undefined behaviour from the
// caller's contract; debug builds may assert, release builds clamp to
// `min(i_coords.size(), j_coords.size())`.
//
// Defensive clamping
// ------------------
// If either `a_out.size()` or `b_out.size()` is smaller than
// `i_coords.size()`, the helper clamps to the smaller output and writes
// no further entries. This matches the W11 `batch_xor_words` / W10
// `batch_eval_poly_int64` contract used by the rest of the SIMD helper
// family.
//
// ENV gate
// --------
//
//   GNFS_LATTICE_COORDS_SIMD=auto    SIMD enabled when supported (default)
//   GNFS_LATTICE_COORDS_SIMD=0       force scalar (regression bisect / sanitizer)
//   GNFS_LATTICE_COORDS_SIMD=off     same as "0"
//   GNFS_LATTICE_COORDS_SIMD=1       force SIMD when supported (else scalar fallback)
//   GNFS_LATTICE_COORDS_SIMD=on      same as "1"
//   (unset / empty / any other)      treated as Auto
//
// The runtime gate is cached after the first call so the dispatcher
// branch on the hot path costs only an atomic-relaxed load. Tests that
// toggle the env mid-process call
// `lattice_coords_simd_reset_env_cache_for_testing()` to flush the cache.
//
// Build-time guards
// -----------------
// Only the host-platform implementation is compiled. When neither NEON
// nor AVX2 is available `lattice_coords_simd_supported()` returns false
// and the dispatcher silently falls back to the scalar reference.

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
    #define GNFS_LATTICE_COORDS_SIMD_NEON 1
  #endif
#endif

#if defined(__AVX2__)
  #if __has_include(<immintrin.h>)
    #include <immintrin.h>
    #define GNFS_LATTICE_COORDS_SIMD_AVX2 1
  #endif
#endif

namespace gnfs::sieve {

/// Compact reduced basis descriptor passed by value to the batched
/// helper. The fields hold the same `int64_t` components a caller would
/// otherwise broadcast manually through the inner loop. Plain struct so
/// the SIMD path can capture each scalar into a register at the top of
/// the kernel and reuse it across all lanes.
struct LatticeBasis {
    std::int64_t b1x;
    std::int64_t b1y;
    std::int64_t b2x;
    std::int64_t b2y;
};

/// Three-state ENV gate matching the `GNFS_GF2_ROW_XOR_SIMD` /
/// `GNFS_POLY_HORNER_BATCH_SIMD` / `GNFS_SPMV_SIMD` family. `Auto`
/// defers to compile-time SIMD availability, `ForceOff` forces the
/// scalar path (regression bisect / sanitizer noise reduction),
/// `ForceOn` opts in even when `lattice_coords_simd_supported()` is
/// false — in which case the dispatcher silently falls back to scalar
/// to keep correctness.
enum class LatticeCoordsSimdMode : std::uint8_t {
    Auto = 0,
    ForceOff = 1,
    ForceOn = 2,
};

namespace lattice_coords_detail {

inline std::atomic<std::uint8_t>& cached_state() noexcept {
    static std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(LatticeCoordsSimdMode::Auto)};
    return state;
}

inline std::once_flag& cached_flag() noexcept {
    static std::once_flag flag;
    return flag;
}

inline LatticeCoordsSimdMode resolve_mode_from_env() noexcept {
    const char* v = std::getenv("GNFS_LATTICE_COORDS_SIMD");
    if (v == nullptr) return LatticeCoordsSimdMode::Auto;
    if (std::strcmp(v, "0") == 0) return LatticeCoordsSimdMode::ForceOff;
    if (std::strcmp(v, "off") == 0) return LatticeCoordsSimdMode::ForceOff;
    if (std::strcmp(v, "1") == 0) return LatticeCoordsSimdMode::ForceOn;
    if (std::strcmp(v, "on") == 0) return LatticeCoordsSimdMode::ForceOn;
    // "auto" / "" / anything else → Auto
    return LatticeCoordsSimdMode::Auto;
}

inline LatticeCoordsSimdMode load_mode() noexcept {
    std::call_once(cached_flag(), []() noexcept {
        cached_state().store(static_cast<std::uint8_t>(resolve_mode_from_env()),
                             std::memory_order_relaxed);
    });
    return static_cast<LatticeCoordsSimdMode>(
        cached_state().load(std::memory_order_relaxed));
}

}  // namespace lattice_coords_detail

/// Compile-time SIMD availability. Constant-folds on any given build.
/// Used by the dispatcher to decide whether `Auto` / `ForceOn` actually
/// has a SIMD code path to run.
[[nodiscard]] constexpr bool lattice_coords_simd_supported() noexcept {
#if defined(GNFS_LATTICE_COORDS_SIMD_NEON) || defined(GNFS_LATTICE_COORDS_SIMD_AVX2)
    return true;
#else
    return false;
#endif
}

/// Returns the parsed ENV state. Cached after the first call.
[[nodiscard]] inline LatticeCoordsSimdMode lattice_coords_simd_mode() noexcept {
    return lattice_coords_detail::load_mode();
}

/// Dispatcher decision: should the batched helper take the SIMD path?
/// Returns `false` when there is no compile-time SIMD support, when ENV
/// forces it off, or when ENV is `ForceOn` but no SIMD is available.
[[nodiscard]] inline bool lattice_coords_simd_enabled() noexcept {
    const LatticeCoordsSimdMode mode = lattice_coords_simd_mode();
    if (mode == LatticeCoordsSimdMode::ForceOff) return false;
    // ForceOn and Auto both require compile-time availability.
    return lattice_coords_simd_supported();
}

/// Re-read GNFS_LATTICE_COORDS_SIMD from the environment and refresh
/// the cached gate. Intended for unit tests that flip the env between
/// scenarios. Not called from production hot paths.
inline void lattice_coords_simd_reset_env_cache_for_testing() noexcept {
    lattice_coords_detail::cached_state().store(
        static_cast<std::uint8_t>(
            lattice_coords_detail::resolve_mode_from_env()),
        std::memory_order_relaxed);
    // Ensure call_once records completion even if it has never run, so
    // a later production call does not overwrite our state.
    std::call_once(lattice_coords_detail::cached_flag(), []() noexcept {});
}

// ---------------------------------------------------------------------------
// Reference scalar path (always available, used as golden for tests and
// as the no-SIMD fallback).
// ---------------------------------------------------------------------------

/// Scalar reference:
///   a_out[k] = basis.b1x * i_coords[k] + basis.b2x * j_coords[k]
///   b_out[k] = basis.b1y * i_coords[k] + basis.b2y * j_coords[k]
/// for `k in [0, min(i_coords.size(), j_coords.size(),
/// a_out.size(), b_out.size()))`. Empty input (either index span size 0)
/// leaves outputs untouched. Defensive contract: outputs are clamped to
/// the smaller of `a_out` / `b_out`. Caller is responsible for sizing
/// the output spans at least as large as the index spans at the call
/// site. The helper performs no overflow check.
inline void batch_lattice_coords_scalar(LatticeBasis basis,
                                        std::span<const std::int64_t> i_coords,
                                        std::span<const std::int64_t> j_coords,
                                        std::span<std::int64_t> a_out,
                                        std::span<std::int64_t> b_out) noexcept {
    const std::size_t n_in = (i_coords.size() < j_coords.size())
                                 ? i_coords.size()
                                 : j_coords.size();
    if (n_in == 0) return;
    const std::size_t n_out = (a_out.size() < b_out.size()) ? a_out.size()
                                                            : b_out.size();
    const std::size_t bound = (n_in < n_out) ? n_in : n_out;
    for (std::size_t k = 0; k < bound; ++k) {
        const std::int64_t ik = i_coords[k];
        const std::int64_t jk = j_coords[k];
        a_out[k] = basis.b1x * ik + basis.b2x * jk;
        b_out[k] = basis.b1y * ik + basis.b2y * jk;
    }
}

namespace lattice_coords_detail {

#if defined(GNFS_LATTICE_COORDS_SIMD_NEON)
/// NEON 2-lane batched projection: process two cells per iteration.
/// Apple Silicon NEON lacks a vector 64-bit signed multiply, so the
/// inner mul-add runs as two independent scalar `int64_t` operations in
/// GPRs. SIMD value comes from the consolidated 16-byte load (`vld1q_s64`
/// extracted once at entry) and consolidated 16-byte store (`vst1q_s64`
/// from a small stack buffer at exit), which reduce per-iteration
/// address-gen pressure compared to four scalar loads / stores.
inline void project_pair_neon(LatticeBasis basis,
                              const std::int64_t* i_ptr,
                              const std::int64_t* j_ptr,
                              std::int64_t* a_ptr,
                              std::int64_t* b_ptr) noexcept {
    // Consolidated 16-byte load via NEON; extract to GPR scalars once.
    alignas(16) std::int64_t i_buf[2];
    alignas(16) std::int64_t j_buf[2];
    vst1q_s64(i_buf, vld1q_s64(i_ptr));
    vst1q_s64(j_buf, vld1q_s64(j_ptr));
    const std::int64_t i0 = i_buf[0];
    const std::int64_t i1 = i_buf[1];
    const std::int64_t j0 = j_buf[0];
    const std::int64_t j1 = j_buf[1];

    // Scalar mul-add per lane in GPRs.
    const std::int64_t a0 = basis.b1x * i0 + basis.b2x * j0;
    const std::int64_t a1 = basis.b1x * i1 + basis.b2x * j1;
    const std::int64_t b0 = basis.b1y * i0 + basis.b2y * j0;
    const std::int64_t b1 = basis.b1y * i1 + basis.b2y * j1;

    // Consolidated 16-byte store via NEON.
    alignas(16) std::int64_t a_buf[2] = {a0, a1};
    alignas(16) std::int64_t b_buf[2] = {b0, b1};
    vst1q_s64(a_ptr, vld1q_s64(a_buf));
    vst1q_s64(b_ptr, vld1q_s64(b_buf));
}
#endif  // GNFS_LATTICE_COORDS_SIMD_NEON

#if defined(GNFS_LATTICE_COORDS_SIMD_AVX2)
/// AVX2 4-lane batched projection: process four cells per iteration.
/// AVX2 lacks a native 64-bit signed multiply in the integer domain
/// (AVX-512 DQ adds `_mm256_mullo_epi64`), so each lane's mul-add runs
/// as scalar `int64_t` in GPRs after extracting from the vector register.
/// Same SIMD value as the NEON path: consolidated 32-byte load + store
/// plus lane-parallel scalar scheduling.
inline void project_quad_avx2(LatticeBasis basis,
                              const std::int64_t* i_ptr,
                              const std::int64_t* j_ptr,
                              std::int64_t* a_ptr,
                              std::int64_t* b_ptr) noexcept {
    alignas(32) std::int64_t i_buf[4];
    alignas(32) std::int64_t j_buf[4];
    _mm256_store_si256(reinterpret_cast<__m256i*>(i_buf),
                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(i_ptr)));
    _mm256_store_si256(reinterpret_cast<__m256i*>(j_buf),
                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(j_ptr)));

    alignas(32) std::int64_t a_buf[4];
    alignas(32) std::int64_t b_buf[4];
    a_buf[0] = basis.b1x * i_buf[0] + basis.b2x * j_buf[0];
    a_buf[1] = basis.b1x * i_buf[1] + basis.b2x * j_buf[1];
    a_buf[2] = basis.b1x * i_buf[2] + basis.b2x * j_buf[2];
    a_buf[3] = basis.b1x * i_buf[3] + basis.b2x * j_buf[3];
    b_buf[0] = basis.b1y * i_buf[0] + basis.b2y * j_buf[0];
    b_buf[1] = basis.b1y * i_buf[1] + basis.b2y * j_buf[1];
    b_buf[2] = basis.b1y * i_buf[2] + basis.b2y * j_buf[2];
    b_buf[3] = basis.b1y * i_buf[3] + basis.b2y * j_buf[3];

    _mm256_storeu_si256(reinterpret_cast<__m256i*>(a_ptr),
                        _mm256_load_si256(reinterpret_cast<const __m256i*>(a_buf)));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(b_ptr),
                        _mm256_load_si256(reinterpret_cast<const __m256i*>(b_buf)));
}
#endif  // GNFS_LATTICE_COORDS_SIMD_AVX2

}  // namespace lattice_coords_detail

// ---------------------------------------------------------------------------
// Primary entry point (dispatcher).
// ---------------------------------------------------------------------------

/// Batched lattice-coordinate projection: for each `k` in
/// `[0, min(i_coords.size(), j_coords.size(), a_out.size(),
/// b_out.size()))`,
///
///     a_out[k] = basis.b1x * i_coords[k] + basis.b2x * j_coords[k]
///     b_out[k] = basis.b1y * i_coords[k] + basis.b2y * j_coords[k]
///
/// Preconditions:
///   * `i_coords.size() == j_coords.size()` is the documented contract.
///     The helper does not assert in release builds; it clamps to the
///     shorter input span instead so out-of-bounds reads cannot occur.
///   * `a_out.size() >= i_coords.size()` and `b_out.size() >=
///     i_coords.size()`. The helper clamps to the shorter output to
///     avoid UB writes past either output.
///   * Caller is responsible for ensuring no `int64_t` overflow in
///     `b1x * i + b2x * j` or `b1y * i + b2y * j`. Typical sieve
///     regions stay well under the bound; the helper performs no
///     overflow check.
///
/// Empty input (either index span size 0) returns immediately without
/// touching the outputs. SIMD path is taken when
/// `lattice_coords_simd_enabled()` is true; otherwise falls back to the
/// scalar reference. Bit-for-bit identical output across both paths for
/// inputs that do not overflow.
inline void batch_lattice_coords(LatticeBasis basis,
                                 std::span<const std::int64_t> i_coords,
                                 std::span<const std::int64_t> j_coords,
                                 std::span<std::int64_t> a_out,
                                 std::span<std::int64_t> b_out) noexcept {
    const std::size_t n_in = (i_coords.size() < j_coords.size())
                                 ? i_coords.size()
                                 : j_coords.size();
    if (n_in == 0) return;
    const std::size_t n_out = (a_out.size() < b_out.size()) ? a_out.size()
                                                            : b_out.size();
    const std::size_t bound = (n_in < n_out) ? n_in : n_out;
    if (bound == 0) return;

    if (!lattice_coords_simd_enabled()) {
        batch_lattice_coords_scalar(basis,
                                    i_coords.first(bound),
                                    j_coords.first(bound),
                                    a_out.first(bound),
                                    b_out.first(bound));
        return;
    }

#if defined(GNFS_LATTICE_COORDS_SIMD_NEON)
    const std::int64_t* ip = i_coords.data();
    const std::int64_t* jp = j_coords.data();
    std::int64_t* ap = a_out.data();
    std::int64_t* bp = b_out.data();
    std::size_t k = 0;
    for (; k + 2 <= bound; k += 2) {
        lattice_coords_detail::project_pair_neon(basis,
                                                 ip + k, jp + k,
                                                 ap + k, bp + k);
    }
    for (; k < bound; ++k) {
        const std::int64_t ik = ip[k];
        const std::int64_t jk = jp[k];
        ap[k] = basis.b1x * ik + basis.b2x * jk;
        bp[k] = basis.b1y * ik + basis.b2y * jk;
    }
#elif defined(GNFS_LATTICE_COORDS_SIMD_AVX2)
    const std::int64_t* ip = i_coords.data();
    const std::int64_t* jp = j_coords.data();
    std::int64_t* ap = a_out.data();
    std::int64_t* bp = b_out.data();
    std::size_t k = 0;
    for (; k + 4 <= bound; k += 4) {
        lattice_coords_detail::project_quad_avx2(basis,
                                                 ip + k, jp + k,
                                                 ap + k, bp + k);
    }
    for (; k < bound; ++k) {
        const std::int64_t ik = ip[k];
        const std::int64_t jk = jp[k];
        ap[k] = basis.b1x * ik + basis.b2x * jk;
        bp[k] = basis.b1y * ik + basis.b2y * jk;
    }
#else
    batch_lattice_coords_scalar(basis,
                                i_coords.first(bound),
                                j_coords.first(bound),
                                a_out.first(bound),
                                b_out.first(bound));
#endif
}

}  // namespace gnfs::sieve
