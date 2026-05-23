#pragma once

// SIMD-accelerated batched F_p[x] Horner polynomial evaluation modulo a
// small prime p (p < 2^32), with NEON 2-lane (ARM64) and AVX2 4-lane
// (x86_64) backends. Sibling of W10 T2 (`horner_batch_simd.hpp`, int64
// evaluation) and W14 T2 (`add_mod_simd.hpp`, F_p[x] add/sub).
//
// Scope
// -----
// Multi-point modular polynomial evaluation appears in several GNFS hot
// paths:
//  * Cantor-Zassenhaus polynomial root finding (verify candidate roots
//    `f(x_i) == 0 mod p` over a batch of probe points).
//  * Polynomial chain compute (evaluate a long polynomial at many sample
//    points reduced mod p).
//  * Systematic small-prime probes for factor-base construction.
//
// The helper exposes a stand-alone batched Horner-mod-p kernel that
// consumes plain `std::span` inputs (coeffs, xs) and produces a flat
// `std::span<uint32_t>` output. Each per-point evaluation is a Horner
// recurrence `acc = (acc * x + c[k]) mod p`, which collapses
// degree-`d` polynomial evaluation to `d` fused multiply-add-reduce
// steps with one shared accumulator. Batching `n` evaluation points
// multiplies the work by `n`; the per-batch loop is embarrassingly
// parallel over independent points, which lets us SIMD-load multiple
// `x[i]` lanes and run the Horner recurrence per lane while the inner
// `(acc * x + c[k]) mod p` work runs in GPR `uint64_t` widening
// (NEON / AVX2 lack native unsigned-64 div).
//
// Algorithm
// ---------
// For a polynomial `p(x) = c[0] + c[1]*x + c[2]*x^2 + ... + c[d]*x^d`
// over F_p, Horner's method evaluates `p(x) mod p` via the recurrence
//
//     acc = c[d]
//     for k in [d - 1, ..., 0]:
//         acc = (uint64(acc) * x + c[k]) % p
//     return acc
//
// which uses `d` multiplies, `d` adds, and `d` modular reductions —
// optimal for general dense polynomials. The 64-bit widening keeps the
// product `acc * x` strictly below 2^63 whenever `p < 2^32` (so
// `acc, x < p < 2^32` gives `acc * x < 2^64`), and `+ c[k]` with
// `c[k] < p < 2^32` adds another sub-2^32 term, so the intermediate
// `uint64` value never overflows before the `% p` reduction.
//
// SIMD layout: NEON loads 2 consecutive `uint32_t` evaluation points
// per iteration via `vld1_u32`; AVX2 loads 4 via `_mm_loadu_si128`.
// The inner mul-add-reduce runs entirely in scalar GPRs (NEON lacks
// `vmulq_u64`; AVX2 has `_mm256_mul_epu32` only for low-32-bit-input
// halves and no integer modulo). The SIMD value is therefore in the
// consolidated load and store, which reduces address-gen pressure on
// the compiler relative to four scalar loads/stores.
//
// Precondition (caller responsibility)
// ------------------------------------
// * `p` must be a prime (the helper does not validate primality; it
//   only uses `p` as the canonical reduction modulus).
// * Every input coefficient must satisfy `coeffs[k] < p` for all `k`,
//   and every evaluation point must satisfy `xs[i] < p` for all `i`.
//   The helper does NOT re-reduce inputs that are >= p. Calling with
//   unreduced inputs is undefined.
//
// SIMD-acceleration window: `p <= 2^31`. The accumulator goes through
// `acc = (uint64(acc) * x + c[k]) % p` after every Horner step, so
// `acc < p` always. The product `acc * x` is at most `(p-1) * (p-1) <
// p^2 < 2^62` when `p <= 2^31`. Adding `c[k] < p <= 2^31` keeps the
// intermediate below `2^62 + 2^31 < 2^63`, safely within `uint64`.
// For `p > 2^31` the same arithmetic still works in `uint64` (product
// bounded by `(p-1)^2 < 2^64`, plus the addend stays under `2^64` by
// a wide margin), but to stay strictly parallel to W14 T2's documented
// boundary the dispatcher falls back to scalar reference. The scalar
// path itself supports the full `p < 2^32` range correctly.
//
// Bit-for-bit guarantee
// ---------------------
// For any `(coeffs, xs, p)` satisfying the precondition (`p` prime,
// `p < 2^32`, `coeffs[k] < p` and `xs[i] < p` for all valid indices),
// the SIMD and scalar paths produce strictly per-index identical
// outputs. The dispatcher's gate value (Auto / ForceOff / ForceOn)
// selects the inner kernel but does not change the mathematical
// answer. Output coefficients always satisfy `ys[i] < p`.
//
// Defensive clamping
// ------------------
// The helper clamps the iteration count to `min(xs.size(), ys.size())`.
// If the spans differ in length only the prefix common to both is
// processed; the tail of `ys` past that prefix is left untouched.
// Empty `xs` returns immediately. Empty `coeffs` writes zero to every
// `ys[i]` in `[0, bound)` (degree-(-1) polynomial evaluates to 0).
//
// Build-time guards: only the host-platform implementation is
// compiled. When neither NEON nor AVX2 is available
// `horner_mod_simd_supported()` returns false and the dispatcher
// silently falls back to the scalar path.

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
    #define GNFS_HORNER_MOD_SIMD_NEON 1
  #endif
#endif

#if defined(__AVX2__)
  #if __has_include(<immintrin.h>)
    #include <immintrin.h>
    #define GNFS_HORNER_MOD_SIMD_AVX2 1
  #endif
#endif

namespace gnfs::polynomial {

/// Three-state ENV gate matching the W10 T2 `GNFS_POLY_HORNER_BATCH_SIMD`
/// / W14 T2 `GNFS_POLY_ADD_MOD_SIMD` convention. `Auto` defers to
/// compile-time SIMD availability plus the `p <= 2^31` SIMD window
/// check; `ForceOff` forces the scalar path (regression bisect /
/// sanitizer noise reduction); `ForceOn` opts in even when
/// `horner_mod_simd_supported()` is false — in which case the
/// dispatcher still falls back to scalar to keep correctness.
enum class PolyHornerModSimdMode : std::uint8_t {
    Auto = 0,
    ForceOff = 1,
    ForceOn = 2,
};

namespace horner_mod_detail {

inline std::atomic<std::uint8_t>& cached_state() noexcept {
    static std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(PolyHornerModSimdMode::Auto)};
    return state;
}

inline std::once_flag& cached_flag() noexcept {
    static std::once_flag flag;
    return flag;
}

/// Parse `GNFS_POLY_HORNER_MOD_SIMD` into a `PolyHornerModSimdMode`.
/// Strict three-state token matching, mirroring W14 T2:
///   "0" / "off"   → ForceOff
///   "1" / "on"    → ForceOn
///   "auto" / unset / ""    → Auto
///   any other token         → Auto
inline PolyHornerModSimdMode resolve_mode_from_env() noexcept {
    const char* v = std::getenv("GNFS_POLY_HORNER_MOD_SIMD");
    if (v == nullptr) return PolyHornerModSimdMode::Auto;
    if (std::strcmp(v, "0") == 0) return PolyHornerModSimdMode::ForceOff;
    if (std::strcmp(v, "off") == 0) return PolyHornerModSimdMode::ForceOff;
    if (std::strcmp(v, "1") == 0) return PolyHornerModSimdMode::ForceOn;
    if (std::strcmp(v, "on") == 0) return PolyHornerModSimdMode::ForceOn;
    // "auto" / "" / "garbage" / "2" / "true" / " 1" → Auto (strict).
    return PolyHornerModSimdMode::Auto;
}

inline PolyHornerModSimdMode load_mode() noexcept {
    std::call_once(cached_flag(), []() noexcept {
        cached_state().store(
            static_cast<std::uint8_t>(resolve_mode_from_env()),
            std::memory_order_relaxed);
    });
    return static_cast<PolyHornerModSimdMode>(
        cached_state().load(std::memory_order_relaxed));
}

/// Predicate: is the modulus in the SIMD-accelerated window?
/// `p <= 2^31` keeps `acc * x` below `2^62` for `acc, x < p`, which
/// leaves headroom for the `+ c[k]` addend and the final `% p`
/// reduction in `uint64`. We accept `p in [1, 2^31]`.
[[nodiscard]] constexpr bool
modulus_in_simd_window(std::uint32_t p) noexcept {
    return p != 0u && p <= 0x80000000u;
}

}  // namespace horner_mod_detail

/// Compile-time SIMD availability. Constant-folds on any given build.
/// Used by the dispatcher to decide whether `Auto` / `ForceOn` actually
/// has a SIMD code path to run.
[[nodiscard]] constexpr bool horner_mod_simd_supported() noexcept {
#if defined(GNFS_HORNER_MOD_SIMD_NEON) || defined(GNFS_HORNER_MOD_SIMD_AVX2)
    return true;
#else
    return false;
#endif
}

/// Returns the parsed ENV state. Cached after first call.
[[nodiscard]] inline PolyHornerModSimdMode poly_horner_mod_simd_mode() noexcept {
    return horner_mod_detail::load_mode();
}

/// Dispatcher decision: should the batched helper take the SIMD path?
/// Returns `false` when there is no compile-time SIMD support, when
/// ENV forces it off, or when ENV is `Auto` and SIMD is unavailable.
/// The per-call `p <= 2^31` SIMD window is checked separately inside
/// `batch_eval_poly_mod` (so this predicate models gate state alone).
[[nodiscard]] inline bool poly_horner_mod_simd_enabled() noexcept {
    const PolyHornerModSimdMode mode = poly_horner_mod_simd_mode();
    if (mode == PolyHornerModSimdMode::ForceOff) return false;
    // ForceOn and Auto both require compile-time availability.
    return horner_mod_simd_supported();
}

/// Re-read `GNFS_POLY_HORNER_MOD_SIMD` from the environment and refresh
/// the cached gate. Intended for unit tests that flip the env between
/// scenarios. Not called from production hot paths.
inline void poly_horner_mod_simd_reset_env_cache_for_testing() noexcept {
    horner_mod_detail::cached_state().store(
        static_cast<std::uint8_t>(horner_mod_detail::resolve_mode_from_env()),
        std::memory_order_relaxed);
    // Ensure call_once records completion even if it has never run,
    // so a later production call does not overwrite our state.
    std::call_once(horner_mod_detail::cached_flag(), []() noexcept {});
}

// ---------------------------------------------------------------------------
// Reference scalar paths (always available, used as golden for tests and
// as the no-SIMD fallback).
// ---------------------------------------------------------------------------

/// Per-point scalar Horner evaluation modulo p:
/// returns `(coeffs[0] + coeffs[1]*x + ... + coeffs[d]*x^d) mod p`
/// computed via the standard right-to-left recurrence. Empty coeffs
/// yields 0. Uses 64-bit widening for the intermediate product so any
/// `p < 2^32` works without overflow. Preconditions: `x < p` and
/// `coeffs[k] < p` for all k.
[[nodiscard]] inline std::uint32_t
horner_eval_one_mod_scalar(std::span<const std::uint32_t> coeffs,
                           std::uint32_t x,
                           std::uint32_t p) noexcept {
    if (coeffs.empty()) return 0u;
    const std::size_t d = coeffs.size() - 1;
    const std::uint64_t pp = static_cast<std::uint64_t>(p);
    std::uint64_t acc = static_cast<std::uint64_t>(coeffs[d]);
    const std::uint64_t xx = static_cast<std::uint64_t>(x);
    for (std::size_t k = d; k-- > 0;) {
        acc = (acc * xx + static_cast<std::uint64_t>(coeffs[k])) % pp;
    }
    return static_cast<std::uint32_t>(acc);
}

/// Scalar reference: `ys[i] = horner_eval_one_mod_scalar(coeffs, xs[i], p)`
/// for each `i in [0, min(xs.size(), ys.size()))`. Empty `xs` returns
/// without touching `ys`. Empty `coeffs` writes zero to every `ys[i]`
/// in `[0, bound)`. This is the golden reference for the SIMD dispatcher.
inline void
batch_eval_poly_mod_scalar(std::span<const std::uint32_t> coeffs,
                           std::span<const std::uint32_t> xs,
                           std::uint32_t p,
                           std::span<std::uint32_t> ys) noexcept {
    const std::size_t n = xs.size();
    if (n == 0) return;
    const std::size_t bound = (ys.size() < n) ? ys.size() : n;
    if (bound == 0) return;
    if (coeffs.empty()) {
        for (std::size_t i = 0; i < bound; ++i) {
            ys[i] = 0u;
        }
        return;
    }
    const std::size_t d = coeffs.size() - 1;
    const std::uint64_t pp = static_cast<std::uint64_t>(p);
    for (std::size_t i = 0; i < bound; ++i) {
        std::uint64_t acc = static_cast<std::uint64_t>(coeffs[d]);
        const std::uint64_t xx = static_cast<std::uint64_t>(xs[i]);
        for (std::size_t k = d; k-- > 0;) {
            acc = (acc * xx + static_cast<std::uint64_t>(coeffs[k])) % pp;
        }
        ys[i] = static_cast<std::uint32_t>(acc);
    }
}

namespace horner_mod_detail {

#if defined(GNFS_HORNER_MOD_SIMD_NEON)
/// NEON 2-lane batched Horner-mod-p: process two evaluation points per
/// iteration. NEON has no native unsigned-64-bit divide, so the
/// `acc = (uint64(acc) * x + c[k]) % p` step runs in scalar `uint64`
/// GPRs per lane. SIMD value comes from the consolidated 8-byte load
/// (`vld1_u32` extracts both `x[i]`s in one instruction) and the
/// consolidated store (one `vst1_u32`). Keeping `acc0`/`acc1` in GPRs
/// throughout the inner loop avoids round-trips to NEON registers that
/// `vget_lane_u32` / `vset_lane_u32` would require each iteration on
/// Apple Silicon, where lane swaps are slower than independent integer
/// pipes.
inline void horner_eval_pair_mod_neon(std::span<const std::uint32_t> coeffs,
                                      const std::uint32_t* xs_ptr,
                                      std::uint32_t* ys_ptr,
                                      std::uint32_t p) noexcept {
    const std::size_t d = coeffs.size() - 1;
    // Consolidated 8-byte load via NEON; extract to GPR scalars once.
    alignas(8) std::uint32_t x_buf[2];
    vst1_u32(x_buf, vld1_u32(xs_ptr));
    const std::uint64_t x0 = static_cast<std::uint64_t>(x_buf[0]);
    const std::uint64_t x1 = static_cast<std::uint64_t>(x_buf[1]);
    const std::uint64_t pp = static_cast<std::uint64_t>(p);
    std::uint64_t a0 = static_cast<std::uint64_t>(coeffs[d]);
    std::uint64_t a1 = a0;
    for (std::size_t k = d; k-- > 0;) {
        const std::uint64_t c_k = static_cast<std::uint64_t>(coeffs[k]);
        a0 = (a0 * x0 + c_k) % pp;
        a1 = (a1 * x1 + c_k) % pp;
    }
    // Consolidated 8-byte store via NEON.
    alignas(8) std::uint32_t y_buf[2] = {
        static_cast<std::uint32_t>(a0),
        static_cast<std::uint32_t>(a1),
    };
    vst1_u32(ys_ptr, vld1_u32(y_buf));
}
#endif  // GNFS_HORNER_MOD_SIMD_NEON

#if defined(GNFS_HORNER_MOD_SIMD_AVX2)
/// AVX2 4-lane batched Horner-mod-p: process four evaluation points per
/// iteration. AVX2 lacks an integer modulo instruction, so the inner
/// `acc = (uint64(acc) * x + c[k]) % p` step runs in scalar `uint64`
/// GPRs per lane. Same SIMD value as the NEON path: consolidated load
/// plus lane-parallel scalar scheduling. Uses `__m128i` 4xuint32 load
/// to avoid pulling in AVX2 256-bit ops just for the boundary load.
inline void horner_eval_quad_mod_avx2(std::span<const std::uint32_t> coeffs,
                                      const std::uint32_t* xs_ptr,
                                      std::uint32_t* ys_ptr,
                                      std::uint32_t p) noexcept {
    const std::size_t d = coeffs.size() - 1;
    // Load four consecutive evaluation points via 128-bit SSE load
    // (every AVX2 host also has SSE2).
    alignas(16) std::uint32_t x_buf[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(x_buf),
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(xs_ptr)));
    const std::uint64_t pp = static_cast<std::uint64_t>(p);
    const std::uint64_t x0 = static_cast<std::uint64_t>(x_buf[0]);
    const std::uint64_t x1 = static_cast<std::uint64_t>(x_buf[1]);
    const std::uint64_t x2 = static_cast<std::uint64_t>(x_buf[2]);
    const std::uint64_t x3 = static_cast<std::uint64_t>(x_buf[3]);
    const std::uint64_t cd = static_cast<std::uint64_t>(coeffs[d]);
    std::uint64_t a0 = cd;
    std::uint64_t a1 = cd;
    std::uint64_t a2 = cd;
    std::uint64_t a3 = cd;
    for (std::size_t k = d; k-- > 0;) {
        const std::uint64_t c_k = static_cast<std::uint64_t>(coeffs[k]);
        a0 = (a0 * x0 + c_k) % pp;
        a1 = (a1 * x1 + c_k) % pp;
        a2 = (a2 * x2 + c_k) % pp;
        a3 = (a3 * x3 + c_k) % pp;
    }
    alignas(16) std::uint32_t y_buf[4] = {
        static_cast<std::uint32_t>(a0),
        static_cast<std::uint32_t>(a1),
        static_cast<std::uint32_t>(a2),
        static_cast<std::uint32_t>(a3),
    };
    _mm_storeu_si128(reinterpret_cast<__m128i*>(ys_ptr),
                     _mm_load_si128(reinterpret_cast<const __m128i*>(y_buf)));
}
#endif  // GNFS_HORNER_MOD_SIMD_AVX2

}  // namespace horner_mod_detail

// ---------------------------------------------------------------------------
// Primary entry point (dispatcher).
// ---------------------------------------------------------------------------

/// Batched modular Horner evaluation:
/// `ys[i] = (coeffs[0] + coeffs[1]*xs[i] + ... + coeffs[d]*xs[i]^d) mod p`
/// for each `i in [0, min(xs.size(), ys.size()))`.
///
/// Preconditions (caller responsibility):
///   * `p` prime with `p < 2^32`.
///   * `coeffs[k] < p` for all `k`.
///   * `xs[i] < p` for all `i`.
///
/// Empty `xs` returns immediately without touching `ys`. Empty `coeffs`
/// writes zero to every `ys[i]` in `[0, bound)`. SIMD path is taken when
/// `poly_horner_mod_simd_enabled()` is true AND `p` is in the SIMD
/// window (`p <= 2^31`); otherwise falls back to the scalar reference.
/// Bit-for-bit identical output across both paths.
inline void batch_eval_poly_mod(std::span<const std::uint32_t> coeffs,
                                std::span<const std::uint32_t> xs,
                                std::uint32_t p,
                                std::span<std::uint32_t> ys) noexcept {
    const std::size_t n = xs.size();
    if (n == 0) return;
    const std::size_t bound = (ys.size() < n) ? ys.size() : n;
    if (bound == 0) return;
    if (coeffs.empty()) {
        for (std::size_t i = 0; i < bound; ++i) {
            ys[i] = 0u;
        }
        return;
    }

    // Gate + SIMD window check. p > 2^31 falls back to scalar reference
    // (scalar path supports the full p < 2^32 range via uint64 widening).
    const bool use_simd = poly_horner_mod_simd_enabled() &&
                          horner_mod_detail::modulus_in_simd_window(p);
    if (!use_simd) {
        batch_eval_poly_mod_scalar(coeffs, xs.first(bound), p,
                                   ys.first(bound));
        return;
    }

#if defined(GNFS_HORNER_MOD_SIMD_NEON)
    const std::uint32_t* xp = xs.data();
    std::uint32_t* yp = ys.data();
    std::size_t i = 0;
    for (; i + 2 <= bound; i += 2) {
        horner_mod_detail::horner_eval_pair_mod_neon(coeffs, xp + i,
                                                     yp + i, p);
    }
    // Tail.
    for (; i < bound; ++i) {
        yp[i] = horner_eval_one_mod_scalar(coeffs, xp[i], p);
    }
#elif defined(GNFS_HORNER_MOD_SIMD_AVX2)
    const std::uint32_t* xp = xs.data();
    std::uint32_t* yp = ys.data();
    std::size_t i = 0;
    for (; i + 4 <= bound; i += 4) {
        horner_mod_detail::horner_eval_quad_mod_avx2(coeffs, xp + i,
                                                     yp + i, p);
    }
    // Tail.
    for (; i < bound; ++i) {
        yp[i] = horner_eval_one_mod_scalar(coeffs, xp[i], p);
    }
#else
    batch_eval_poly_mod_scalar(coeffs, xs.first(bound), p,
                               ys.first(bound));
#endif
}

}  // namespace gnfs::polynomial
