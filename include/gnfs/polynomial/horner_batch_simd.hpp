#pragma once

// SIMD-accelerated batch Horner polynomial evaluation helper.
//
// Scope
// -----
// Polynomial evaluation appears in several GNFS hot paths:
// Murphy E rotation sweeps (evaluate `f(x_i)` at many sample points),
// polynomial verification during Cantor-Zassenhaus root finding (probe
// whether a candidate root satisfies `f(x) = 0`), and Kleinjung skewness
// search (evaluate `f(x)` over a sample grid). All three patterns reduce
// to "given coefficient list `c[0..deg]` and a batch of evaluation points
// `x[0..n-1]`, produce `y[i] = c[0] + c[1]*x[i] + ... + c[deg]*x[i]^deg`".
// Horner schema collapses the per-point evaluation to `deg` fused
// multiply-add steps with one shared accumulator. This helper exposes a
// stand-alone batched Horner kernel that consumes plain `std::span`s and
// produces a flat output span; callers compose it into wider sweeps
// without paying for thread-pool setup or polynomial-object overhead.
//
// Algorithm
// ---------
// For a polynomial `p(x) = c[0] + c[1]*x + c[2]*x^2 + ... + c[d]*x^d`,
// Horner's method evaluates `p(x)` via the recurrence
//
//     acc = c[d]
//     for k in [d - 1, ..., 0]:
//         acc = acc * x + c[k]
//     return acc
//
// which uses `d` multiplies and `d` adds — optimal for general dense
// polynomials. Batching `n` evaluation points multiplies the work by `n`;
// the per-batch loop is embarrassingly parallel over independent points,
// which lets us SIMD-load multiple `x[i]` lanes and run one Horner
// recurrence per lane within a single register file. NEON gives 2 lanes
// of `int64x2_t`; AVX2 gives 4 lanes of `__m256i`. Neither ISA accelerates
// 64-bit × 64-bit signed integer multiplication natively in a vector
// instruction (Apple Silicon NEON lacks `vmulq_s64`; AVX2 requires
// AVX-512 DQ for `_mm256_mullo_epi64`), so the actual mul / add fall back
// to scalar `int64_t` per lane. The SIMD value is therefore in (a) the
// vector load and broadcast of `c[k]`, (b) the consolidated stack layout
// of two or four lanes' accumulators, and (c) the compiler's freedom to
// schedule independent scalar mul-add pairs across the lanes. Tail (when
// `xs.size()` is not a multiple of the SIMD width) walks the remaining
// points via the scalar reference.
//
// Bit-for-bit guarantee
// ---------------------
// Horner's accumulator is a sequence of native `int64_t` multiplies and
// adds. The SIMD path performs the same `int64_t` operations per lane
// (just batched in a vector load); the scalar path performs them in a
// `for (k)` loop. Signed integer wrap-around behaviour for both paths is
// well-defined under `-fwrapv` (GCC and Clang make this assumption in
// nominal builds; the project uses default optimisation which preserves
// the two's-complement wrap). For inputs that do not overflow int64
// (caller's responsibility — see Modular overflow note below), both paths
// yield identical results per index. Empty `xs` returns immediately
// without touching `ys`. Empty `coeffs` (degree-(-1) polynomial) returns
// zero in every output slot.
//
// Modular overflow note
// ---------------------
// The recurrence `acc = acc * x + c[k]` accumulates a polynomial value in
// `int64_t`. For high-degree polynomials, large coefficients, or large
// evaluation points the intermediate `acc` may overflow `int64_t`. The
// helper does NOT check for overflow — callers must size the polynomial
// such that the final and all intermediate values fit in `int64_t`. This
// matches the convention of Murphy E sample-grid sweeps where
// `|x[i]| <= skew` and `|c[k]| << 2^63 / skew^deg`, ensuring no overflow.
// Callers needing arbitrary-precision evaluation should use the
// `Integer`-based polynomial API instead.
//
// Build-time guards: only the host-platform implementation is compiled.
// When neither NEON nor AVX2 is available `horner_batch_simd_supported()`
// returns false and the dispatcher silently falls back to the scalar path.

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
    #define GNFS_HORNER_BATCH_SIMD_NEON 1
  #endif
#endif

#if defined(__AVX2__)
  #if __has_include(<immintrin.h>)
    #include <immintrin.h>
    #define GNFS_HORNER_BATCH_SIMD_AVX2 1
  #endif
#endif

namespace gnfs::polynomial {

/// Three-state ENV gate matching the `GNFS_SPMV_SIMD` /
/// `GNFS_GF2_POPCNT_SIMD` / `GNFS_TRIAL_DIV_SIMD` convention. `Auto`
/// defers to compile-time SIMD availability, `ForceOff` forces the scalar
/// path (regression bisect / sanitizer noise reduction), `ForceOn` opts
/// in even when `horner_batch_simd_supported()` is false — in which case
/// the dispatcher falls back to scalar to keep correctness.
enum class HornerBatchSimdMode : std::uint8_t {
    Auto = 0,
    ForceOff = 1,
    ForceOn = 2,
};

namespace horner_detail {

inline std::atomic<std::uint8_t>& cached_state() noexcept {
    static std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(HornerBatchSimdMode::Auto)};
    return state;
}

inline std::once_flag& cached_flag() noexcept {
    static std::once_flag flag;
    return flag;
}

/// Parse `GNFS_POLY_HORNER_BATCH_SIMD` into a `HornerBatchSimdMode`.
/// Decision table (strict):
///   "0"    → ForceOff
///   "1"    → ForceOn
///   "auto" → Auto
///   unset  → Auto
///   ""     → Auto
///   other  → Auto
inline HornerBatchSimdMode resolve_mode_from_env() noexcept {
    const char* v = std::getenv("GNFS_POLY_HORNER_BATCH_SIMD");
    if (v == nullptr) return HornerBatchSimdMode::Auto;
    if (std::strcmp(v, "0") == 0) return HornerBatchSimdMode::ForceOff;
    if (std::strcmp(v, "1") == 0) return HornerBatchSimdMode::ForceOn;
    // "auto" or anything else
    return HornerBatchSimdMode::Auto;
}

inline HornerBatchSimdMode load_mode() noexcept {
    std::call_once(cached_flag(), []() noexcept {
        cached_state().store(static_cast<std::uint8_t>(resolve_mode_from_env()),
                             std::memory_order_relaxed);
    });
    return static_cast<HornerBatchSimdMode>(
        cached_state().load(std::memory_order_relaxed));
}

}  // namespace horner_detail

/// Compile-time SIMD availability. Constant-folds on any given build.
[[nodiscard]] constexpr bool horner_batch_simd_supported() noexcept {
#if defined(GNFS_HORNER_BATCH_SIMD_NEON) || defined(GNFS_HORNER_BATCH_SIMD_AVX2)
    return true;
#else
    return false;
#endif
}

/// Returns the parsed ENV mode. Cached after first call.
[[nodiscard]] inline HornerBatchSimdMode horner_batch_simd_mode() noexcept {
    return horner_detail::load_mode();
}

/// Dispatcher decision: should the batched helper take the SIMD path?
/// `ForceOff` always returns false. `ForceOn` / `Auto` require compile-
/// time SIMD support to return true (otherwise fall back to scalar to
/// keep correctness).
[[nodiscard]] inline bool horner_batch_simd_enabled() noexcept {
    const HornerBatchSimdMode mode = horner_batch_simd_mode();
    if (mode == HornerBatchSimdMode::ForceOff) return false;
    return horner_batch_simd_supported();
}

/// Re-read `GNFS_POLY_HORNER_BATCH_SIMD` from the environment and refresh
/// the cached gate. Intended for unit tests that flip the env between
/// scenarios. Not called from production hot paths.
inline void horner_batch_simd_reset_env_cache_for_testing() noexcept {
    horner_detail::cached_state().store(
        static_cast<std::uint8_t>(horner_detail::resolve_mode_from_env()),
        std::memory_order_relaxed);
    // Ensure call_once records completion even if it has never run, so a
    // later production call does not overwrite our state.
    std::call_once(horner_detail::cached_flag(), []() noexcept {});
}

// ---------------------------------------------------------------------------
// Scalar reference (always available; used as golden in tests and as the
// no-SIMD fallback).
// ---------------------------------------------------------------------------

/// Per-point scalar Horner evaluation: returns
/// `coeffs[0] + coeffs[1]*x + coeffs[2]*x^2 + ... + coeffs[d]*x^d`
/// computed via the standard right-to-left recurrence. Empty coeffs
/// yields 0. No overflow check.
[[nodiscard]] inline std::int64_t
horner_eval_one_scalar(std::span<const std::int64_t> coeffs,
                       std::int64_t x) noexcept {
    if (coeffs.empty()) return 0;
    const std::size_t d = coeffs.size() - 1;
    std::int64_t acc = coeffs[d];
    for (std::size_t k = d; k-- > 0;) {
        acc = acc * x + coeffs[k];
    }
    return acc;
}

/// Scalar reference: `ys[i] = horner_eval_one_scalar(coeffs, xs[i])` for
/// each `i in [0, xs.size())`. Preconditions: `ys.size() >= xs.size()`.
/// Empty `xs` returns without touching `ys`. Empty `coeffs` writes zero
/// to every `ys[i]` in `[0, xs.size())`. This is the golden reference
/// for the SIMD dispatcher.
inline void
batch_eval_poly_int64_scalar(std::span<const std::int64_t> coeffs,
                             std::span<const std::int64_t> xs,
                             std::span<std::int64_t> ys) noexcept {
    const std::size_t n = xs.size();
    if (n == 0) return;
    const std::size_t bound = (ys.size() < n) ? ys.size() : n;
    if (coeffs.empty()) {
        for (std::size_t i = 0; i < bound; ++i) {
            ys[i] = 0;
        }
        return;
    }
    const std::size_t d = coeffs.size() - 1;
    for (std::size_t i = 0; i < bound; ++i) {
        const std::int64_t x = xs[i];
        std::int64_t acc = coeffs[d];
        for (std::size_t k = d; k-- > 0;) {
            acc = acc * x + coeffs[k];
        }
        ys[i] = acc;
    }
}

namespace horner_detail {

#if defined(GNFS_HORNER_BATCH_SIMD_NEON)
/// NEON 2-lane batched Horner: process two evaluation points per iteration.
/// Apple Silicon NEON lacks a vector 64-bit signed multiply, so we extract
/// each lane, do a scalar `int64_t` mul-add, and recombine. The SIMD value
/// comes from the consolidated vector load (`vld1q_s64`) and broadcast
/// (`vdupq_n_s64`), plus the compiler's freedom to schedule the two
/// scalar mul-adds in parallel against independent integer pipes.
inline void horner_eval_pair_neon(std::span<const std::int64_t> coeffs,
                                  const std::int64_t* xs_ptr,
                                  std::int64_t* ys_ptr) noexcept {
    const std::size_t d = coeffs.size() - 1;
    // Load two consecutive evaluation points into one Q register.
    int64x2_t x_vec = vld1q_s64(xs_ptr);
    // Initialise accumulator with the leading coefficient broadcast.
    int64x2_t acc_vec = vdupq_n_s64(coeffs[d]);
    // Horner step: extract lanes, do scalar mul-add, recombine.
    for (std::size_t k = d; k-- > 0;) {
        const std::int64_t c_k = coeffs[k];
        const std::int64_t x0 = vgetq_lane_s64(x_vec, 0);
        const std::int64_t x1 = vgetq_lane_s64(x_vec, 1);
        const std::int64_t a0 = vgetq_lane_s64(acc_vec, 0);
        const std::int64_t a1 = vgetq_lane_s64(acc_vec, 1);
        const std::int64_t n0 = a0 * x0 + c_k;
        const std::int64_t n1 = a1 * x1 + c_k;
        acc_vec = vsetq_lane_s64(n0, acc_vec, 0);
        acc_vec = vsetq_lane_s64(n1, acc_vec, 1);
    }
    vst1q_s64(ys_ptr, acc_vec);
}
#endif  // GNFS_HORNER_BATCH_SIMD_NEON

#if defined(GNFS_HORNER_BATCH_SIMD_AVX2)
/// AVX2 4-lane batched Horner: process four evaluation points per
/// iteration. AVX2 lacks a native 64-bit × 64-bit signed multiply in the
/// integer domain (the AVX-512 DQ extension adds `_mm256_mullo_epi64`),
/// so we extract each lane to scalar, do the mul-add, and rebuild the
/// vector. Same SIMD value as the NEON path: consolidated load plus
/// lane-parallel scalar scheduling.
inline void horner_eval_quad_avx2(std::span<const std::int64_t> coeffs,
                                  const std::int64_t* xs_ptr,
                                  std::int64_t* ys_ptr) noexcept {
    const std::size_t d = coeffs.size() - 1;
    // Load four consecutive evaluation points.
    __m256i x_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(xs_ptr));
    alignas(32) std::int64_t x_buf[4];
    _mm256_store_si256(reinterpret_cast<__m256i*>(x_buf), x_vec);
    // Accumulator scalars, initialised with the leading coefficient.
    std::int64_t acc[4] = {coeffs[d], coeffs[d], coeffs[d], coeffs[d]};
    for (std::size_t k = d; k-- > 0;) {
        const std::int64_t c_k = coeffs[k];
        acc[0] = acc[0] * x_buf[0] + c_k;
        acc[1] = acc[1] * x_buf[1] + c_k;
        acc[2] = acc[2] * x_buf[2] + c_k;
        acc[3] = acc[3] * x_buf[3] + c_k;
    }
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(ys_ptr),
                        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc)));
}
#endif  // GNFS_HORNER_BATCH_SIMD_AVX2

}  // namespace horner_detail

// ---------------------------------------------------------------------------
// Primary entry point (dispatcher).
// ---------------------------------------------------------------------------

/// Batched Horner evaluation:
/// `ys[i] = coeffs[0] + coeffs[1]*xs[i] + ... + coeffs[d]*xs[i]^d` for
/// each `i in [0, xs.size())`.
///
/// Preconditions:
///   * `ys.size() >= xs.size()` (defensive clamp prevents UB write past
///     `ys`; callers should size them equal at the call site).
///   * Caller is responsible for ensuring no `int64_t` overflow in the
///     accumulator (see the "Modular overflow note" in the header
///     docstring). The helper performs no overflow check.
///
/// Empty `xs` returns immediately without touching `ys`. Empty `coeffs`
/// writes zero to every `ys[i]` in `[0, bound)`. SIMD path is taken when
/// `horner_batch_simd_enabled()` returns true; otherwise falls back to
/// the scalar reference. Bit-for-bit identical output across both paths
/// for inputs that do not overflow.
inline void batch_eval_poly_int64(std::span<const std::int64_t> coeffs,
                                  std::span<const std::int64_t> xs,
                                  std::span<std::int64_t> ys) noexcept {
    const std::size_t n = xs.size();
    if (n == 0) return;
    const std::size_t bound = (ys.size() < n) ? ys.size() : n;
    if (bound == 0) return;
    if (coeffs.empty()) {
        for (std::size_t i = 0; i < bound; ++i) {
            ys[i] = 0;
        }
        return;
    }

    if (!horner_batch_simd_enabled()) {
        batch_eval_poly_int64_scalar(coeffs, xs.first(bound), ys.first(bound));
        return;
    }

#if defined(GNFS_HORNER_BATCH_SIMD_NEON)
    const std::int64_t* xp = xs.data();
    std::int64_t* yp = ys.data();
    std::size_t i = 0;
    for (; i + 2 <= bound; i += 2) {
        horner_detail::horner_eval_pair_neon(coeffs, xp + i, yp + i);
    }
    // Tail.
    for (; i < bound; ++i) {
        yp[i] = horner_eval_one_scalar(coeffs, xp[i]);
    }
#elif defined(GNFS_HORNER_BATCH_SIMD_AVX2)
    const std::int64_t* xp = xs.data();
    std::int64_t* yp = ys.data();
    std::size_t i = 0;
    for (; i + 4 <= bound; i += 4) {
        horner_detail::horner_eval_quad_avx2(coeffs, xp + i, yp + i);
    }
    // Tail.
    for (; i < bound; ++i) {
        yp[i] = horner_eval_one_scalar(coeffs, xp[i]);
    }
#else
    batch_eval_poly_int64_scalar(coeffs, xs.first(bound), ys.first(bound));
#endif
}

}  // namespace gnfs::polynomial
