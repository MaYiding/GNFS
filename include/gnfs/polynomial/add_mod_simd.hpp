#pragma once

// SIMD-accelerated batched F_p[x] coefficient add / sub modulo a small
// prime p (p < 2^32), with NEON 4-lane and AVX2 8-lane backends.
//
// Scope
// -----
// This header provides two batched primitives:
//
//   add_mod_p_batch(a, b, p, out): out[i] = (a[i] + b[i]) mod p
//   sub_mod_p_batch(a, b, p, out): out[i] = (a[i] - b[i] + p) mod p
//
// Both operate on uint32_t coefficient arrays. The intended caller is
// Cantor-Zassenhaus polynomial root finding and other F_p[x] inner-loop
// hot paths that accumulate / cancel polynomial coefficients in long
// chains. Each per-element op is a single add (or sub) plus a single
// conditional reduce against p, fused in 128-bit (NEON) or 256-bit
// (AVX2) lanes so the compiler can schedule independent lanes without
// per-lane branches.
//
// The helper is intentionally stand-alone (no thread-pool, no SpMV
// coupling, no allocation): callers pass three `std::span<const
// uint32_t>` / `std::span<uint32_t>` and the modulus `p`.
//
// Precondition (caller responsibility)
// ------------------------------------
// * `p` must be a prime (the helper does not validate primality; it
//   only uses `p` as the canonical reduction modulus).
// * Every input coefficient must already be reduced: `a[i] < p` and
//   `b[i] < p` for all `i`. The helper does NOT re-reduce inputs that
//   are >= p. Calling with unreduced inputs is undefined.
//
// SIMD-acceleration window: `p <= 2^31`. When `p > 2^31` the SIMD
// path's `vcgeq_u32` / `_mm256_cmpgt_epi32` reductions can race against
// signed-versus-unsigned comparator semantics on the wrap-around
// `a + b` boundary (the partial sum `a + b` is up to `2 * (p - 1)`
// which may exceed 2^32 only when `p > 2^31`). To keep the helper
// straightforward and bit-for-bit identical to the scalar reference
// across every supported lane width, `p > 2^31` is documented as the
// SIMD fallback boundary: the dispatcher reverts to the scalar loop
// for that modulus range (this is NOT a silent bug — the scalar path
// itself supports `p < 2^32` correctly via `uint64_t` widening for the
// sum / difference). All callers we anticipate (CZ root finding over
// the polynomial ring F_p[x] with p < 2^31 — most NFS-derived
// factor-base primes — and most modular arithmetic in polynomial
// chains) hit the SIMD window naturally.
//
// Bit-for-bit guarantee
// ---------------------
// For any (a, b, p) satisfying the precondition (p prime, p < 2^32,
// a[i] < p, b[i] < p), the SIMD and scalar paths produce strictly
// per-index identical outputs. The dispatcher's gate value (Auto /
// ForceOff / ForceOn) selects the inner kernel but does not change
// the mathematical answer. Output coefficients always satisfy
// `out[i] < p`.
//
// Defensive clamping
// ------------------
// The helper clamps the iteration count to `min(a.size(), b.size(),
// out.size())`. If the spans differ in length, only the prefix common
// to all three is processed; the tail of `out` past that prefix is
// left untouched (matches the W11 `GNFS_GF2_ROW_XOR_SIMD` /
// W13 `GNFS_GF2_AND_WORDS_SIMD` contract).
//
// Build-time guards: only the host-platform implementation is
// compiled. When neither NEON nor AVX2 is available
// `add_mod_simd_supported()` returns false and the dispatcher silently
// falls back to the scalar path.

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  #if __has_include(<arm_neon.h>)
    #include <arm_neon.h>
    #define GNFS_ADD_MOD_SIMD_NEON 1
  #endif
#endif

#if defined(__AVX2__)
  #if __has_include(<immintrin.h>)
    #include <immintrin.h>
    #define GNFS_ADD_MOD_SIMD_AVX2 1
  #endif
#endif

namespace gnfs::polynomial {

/// Three-state ENV gate matching the W11 / W12 / W13 SIMD helper family
/// (`GNFS_POLY_NTT`, `GNFS_GF2_ROW_XOR_SIMD`, `GNFS_GF2_AND_WORDS_SIMD`,
/// etc.). `Auto` defers to compile-time SIMD availability + the
/// `p <= 2^31` window; `ForceOff` forces the scalar path (useful for
/// regression-bisect or sanitizer runs); `ForceOn` opts in even when
/// `add_mod_simd_supported()` is false — in which case the dispatcher
/// still falls back to scalar to keep correctness.
enum class PolyAddModSimdMode : std::uint8_t {
    Auto = 0,
    ForceOff = 1,
    ForceOn = 2,
};

namespace add_mod_detail {

inline std::atomic<std::uint8_t>& cached_state() noexcept {
    static std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(PolyAddModSimdMode::Auto)};
    return state;
}

inline std::once_flag& cached_flag() noexcept {
    static std::once_flag flag;
    return flag;
}

inline PolyAddModSimdMode resolve_mode_from_env() noexcept {
    const char* v = std::getenv("GNFS_POLY_ADD_MOD_SIMD");
    if (v == nullptr) return PolyAddModSimdMode::Auto;
    if (std::strcmp(v, "0") == 0) return PolyAddModSimdMode::ForceOff;
    if (std::strcmp(v, "off") == 0) return PolyAddModSimdMode::ForceOff;
    if (std::strcmp(v, "1") == 0) return PolyAddModSimdMode::ForceOn;
    if (std::strcmp(v, "on") == 0) return PolyAddModSimdMode::ForceOn;
    // "auto" or anything else (incl. "ON" / "Auto" / "2" / "true" /
    // " 1" with leading whitespace) → Auto. Matches the W12 NTT helper
    // strict-token convention.
    return PolyAddModSimdMode::Auto;
}

inline PolyAddModSimdMode load_mode() noexcept {
    std::call_once(cached_flag(), []() noexcept {
        cached_state().store(
            static_cast<std::uint8_t>(resolve_mode_from_env()),
            std::memory_order_relaxed);
    });
    return static_cast<PolyAddModSimdMode>(
        cached_state().load(std::memory_order_relaxed));
}

}  // namespace add_mod_detail

/// Compile-time SIMD availability. Constant-folds on any given build.
/// Used by the dispatcher to decide whether `Auto` / `ForceOn`
/// actually has a SIMD code path to run.
[[nodiscard]] constexpr bool add_mod_simd_supported() noexcept {
#if defined(GNFS_ADD_MOD_SIMD_NEON) || defined(GNFS_ADD_MOD_SIMD_AVX2)
    return true;
#else
    return false;
#endif
}

/// Returns the parsed ENV state. Cached after first call.
[[nodiscard]] inline PolyAddModSimdMode poly_add_mod_simd_mode() noexcept {
    return add_mod_detail::load_mode();
}

/// Dispatcher decision: should the batched helper take the SIMD path?
/// Returns `false` when there is no compile-time SIMD support, when
/// ENV forces it off, or when ENV is `Auto` and SIMD is unavailable.
/// The per-call `p <= 2^31` SIMD window is checked separately inside
/// the helpers (so the same gate result is used by both `add` and
/// `sub` entries).
[[nodiscard]] inline bool poly_add_mod_simd_enabled() noexcept {
    const PolyAddModSimdMode mode = poly_add_mod_simd_mode();
    if (mode == PolyAddModSimdMode::ForceOff) return false;
    // ForceOn and Auto both require compile-time availability.
    return add_mod_simd_supported();
}

/// Re-read GNFS_POLY_ADD_MOD_SIMD from the environment and refresh the
/// cached gate. Intended for unit tests that flip the env between
/// scenarios. Not called from production hot paths.
inline void poly_add_mod_simd_reset_env_cache_for_testing() noexcept {
    add_mod_detail::cached_state().store(
        static_cast<std::uint8_t>(add_mod_detail::resolve_mode_from_env()),
        std::memory_order_relaxed);
    // Ensure call_once records completion even if it has never run,
    // so a later production call does not overwrite our state.
    std::call_once(add_mod_detail::cached_flag(), []() noexcept {});
}

// ---------------------------------------------------------------------------
// Reference scalar paths (always available, used as golden for tests
// and as the no-SIMD fallback).
// ---------------------------------------------------------------------------

/// Scalar reference: out[i] = (a[i] + b[i]) mod p
/// for `i in [0, min(a.size(), b.size(), out.size()))`. Uses 64-bit
/// widening for the sum so any `p < 2^32` works without overflow. The
/// caller still must satisfy the documented precondition that each
/// input coefficient already satisfies `< p`; the reference does not
/// re-reduce inputs >= p (that would mask bugs in the caller's
/// reduction pipeline).
inline void add_mod_p_batch_scalar(std::span<const std::uint32_t> a,
                                   std::span<const std::uint32_t> b,
                                   std::uint32_t p,
                                   std::span<std::uint32_t> out) noexcept {
    const std::size_t bound = std::min({a.size(), b.size(), out.size()});
    for (std::size_t i = 0; i < bound; ++i) {
        const std::uint64_t s =
            static_cast<std::uint64_t>(a[i]) + static_cast<std::uint64_t>(b[i]);
        const std::uint64_t pp = static_cast<std::uint64_t>(p);
        out[i] = static_cast<std::uint32_t>(s >= pp ? s - pp : s);
    }
}

/// Scalar reference: out[i] = (a[i] - b[i] + p) mod p
/// for `i in [0, min(a.size(), b.size(), out.size()))`. Uses int64_t
/// widening for the difference so the negative-branch correctly adds
/// `p` back. Same precondition as the add reference.
inline void sub_mod_p_batch_scalar(std::span<const std::uint32_t> a,
                                   std::span<const std::uint32_t> b,
                                   std::uint32_t p,
                                   std::span<std::uint32_t> out) noexcept {
    const std::size_t bound = std::min({a.size(), b.size(), out.size()});
    for (std::size_t i = 0; i < bound; ++i) {
        const std::int64_t d =
            static_cast<std::int64_t>(a[i]) - static_cast<std::int64_t>(b[i]);
        const std::int64_t pp = static_cast<std::int64_t>(p);
        out[i] = static_cast<std::uint32_t>(d < 0 ? d + pp : d);
    }
}

namespace add_mod_detail {

/// Predicate: is the modulus in the SIMD-accelerated window? The
/// NEON `vsubq_u32` / AVX2 `_mm256_sub_epi32` reduction uses
/// `cmp(sum, p)` semantics that only stay bit-faithful when
/// `a + b < 2 * 2^31 = 2^32` (i.e. no uint32 overflow before
/// reduction). With `p < 2^31` we always have `a + b <= 2 * (p - 1) <
/// 2^32` so the wide unsigned add fits in 32 bits and the conditional
/// subtract is well-defined. `p == 2^31` is on the boundary
/// (`a + b` may equal `2^32 - 2`, still within uint32), so we accept
/// `p <= 2^31`. For larger `p`, the dispatcher falls back to the
/// scalar reference.
[[nodiscard]] constexpr bool modulus_in_simd_window(std::uint32_t p) noexcept {
    // 2^31 = 0x80000000. Accept p in [1, 2^31].
    return p != 0u && p <= 0x80000000u;
}

#if defined(GNFS_ADD_MOD_SIMD_NEON)
/// NEON 4-lane add-mod kernel. Input lanes must satisfy a < p and b < p
/// with p in the SIMD window. Output lanes satisfy out < p.
inline uint32x4_t add_mod_neon(uint32x4_t va, uint32x4_t vb,
                               uint32x4_t vp) noexcept {
    const uint32x4_t vs = vaddq_u32(va, vb);
    // mask = (vs >= vp) ? all-ones : zero (per lane).
    const uint32x4_t vmask = vcgeq_u32(vs, vp);
    // Subtract p where mask is set; mask AND p gives 0 or p per lane.
    const uint32x4_t vsub = vandq_u32(vmask, vp);
    return vsubq_u32(vs, vsub);
}

/// NEON 4-lane sub-mod kernel. Input lanes must satisfy a < p and b < p.
/// Output lanes satisfy out < p. The trick: `a - b` underflows to
/// `a - b + 2^32` when a < b in unsigned arithmetic. Add `p` back
/// unconditionally then mask to either `a - b + p` (when a < b) or
/// `(a - b) + 0` (when a >= b), using the underflow detection
/// `(a - b + p) >= p` ⇔ a >= b. We use the equivalent
/// `mask = (a < b)` formulation via `vcltq_u32` for clarity.
inline uint32x4_t sub_mod_neon(uint32x4_t va, uint32x4_t vb,
                               uint32x4_t vp) noexcept {
    const uint32x4_t vd = vsubq_u32(va, vb);
    // mask = (va < vb) ? all-ones : zero (per lane). When underflow,
    // we add p; otherwise leave d alone.
    const uint32x4_t vmask = vcltq_u32(va, vb);
    const uint32x4_t vadd = vandq_u32(vmask, vp);
    return vaddq_u32(vd, vadd);
}
#endif  // GNFS_ADD_MOD_SIMD_NEON

#if defined(GNFS_ADD_MOD_SIMD_AVX2)
/// AVX2 8-lane add-mod kernel. Same semantics as the NEON sibling.
/// AVX2 lacks a native unsigned compare; we use the signed-compare
/// after biasing by `2^31` to translate the `vs >= vp` decision into
/// AVX2's `_mm256_cmpgt_epi32` lane-wise output.
inline __m256i add_mod_avx2(__m256i va, __m256i vb, __m256i vp) noexcept {
    const __m256i vs = _mm256_add_epi32(va, vb);
    // Bias both vs and vp by 0x80000000 so the signed compare gives
    // the same answer as an unsigned compare. Then "vs >= vp" is
    // equivalent to "NOT (biased_vp > biased_vs)".
    const __m256i bias = _mm256_set1_epi32(static_cast<int>(0x80000000u));
    const __m256i bvs = _mm256_xor_si256(vs, bias);
    const __m256i bvp = _mm256_xor_si256(vp, bias);
    // mask_lt = (biased_vs < biased_vp) ? all-ones : zero. We want the
    // opposite (geq), so we compute "not (biased_vs < biased_vp)" via
    // "biased_vs >= biased_vp" ⇔ "(biased_vp - 1) < biased_vs" — but
    // a simpler path: use cmpgt then XOR-NOT.
    const __m256i mask_gt = _mm256_cmpgt_epi32(bvp, bvs);  // biased_vp > biased_vs
    // mask_geq is the bitwise NOT of mask_gt (lane-wise).
    const __m256i all_ones = _mm256_set1_epi32(-1);
    const __m256i mask_geq = _mm256_xor_si256(mask_gt, all_ones);
    // Subtract p where mask_geq is set.
    const __m256i vsub = _mm256_and_si256(mask_geq, vp);
    return _mm256_sub_epi32(vs, vsub);
}

/// AVX2 8-lane sub-mod kernel. mask = (a < b); when underflow, add p.
inline __m256i sub_mod_avx2(__m256i va, __m256i vb, __m256i vp) noexcept {
    const __m256i vd = _mm256_sub_epi32(va, vb);
    // mask_lt = (va < vb) ? all-ones : zero. Bias for unsigned compare.
    const __m256i bias = _mm256_set1_epi32(static_cast<int>(0x80000000u));
    const __m256i bva = _mm256_xor_si256(va, bias);
    const __m256i bvb = _mm256_xor_si256(vb, bias);
    const __m256i mask_lt = _mm256_cmpgt_epi32(bvb, bva);  // biased_vb > biased_va ⇔ vb > va (unsigned)
    const __m256i vadd = _mm256_and_si256(mask_lt, vp);
    return _mm256_add_epi32(vd, vadd);
}
#endif  // GNFS_ADD_MOD_SIMD_AVX2

}  // namespace add_mod_detail

// ---------------------------------------------------------------------------
// Primary entry points (dispatchers).
// ---------------------------------------------------------------------------

/// Batch add modulo p:  out[i] = (a[i] + b[i]) mod p
/// for `i in [0, min(a.size(), b.size(), out.size()))`. Empty input
/// leaves `out` untouched. SIMD path is taken when
/// `poly_add_mod_simd_enabled()` is true AND `p` is in the SIMD
/// window (`p <= 2^31`); otherwise falls back to the scalar
/// reference. Bit-for-bit identical output across both paths.
///
/// Precondition (caller responsibility): `a[i] < p`, `b[i] < p` for
/// all i, and `p` prime with `p < 2^32`.
inline void add_mod_p_batch(std::span<const std::uint32_t> a,
                            std::span<const std::uint32_t> b,
                            std::uint32_t p,
                            std::span<std::uint32_t> out) noexcept {
    const std::size_t bound = std::min({a.size(), b.size(), out.size()});
    if (bound == 0) return;

    // Gate + SIMD window check.
    const bool use_simd = poly_add_mod_simd_enabled() &&
                          add_mod_detail::modulus_in_simd_window(p);
    if (!use_simd) {
        add_mod_p_batch_scalar(a.first(bound), b.first(bound), p,
                               out.first(bound));
        return;
    }

#if defined(GNFS_ADD_MOD_SIMD_NEON)
    const std::uint32_t* pa = a.data();
    const std::uint32_t* pb = b.data();
    std::uint32_t* po = out.data();
    const uint32x4_t vp = vdupq_n_u32(p);
    std::size_t i = 0;
    for (; i + 4 <= bound; i += 4) {
        const uint32x4_t va = vld1q_u32(pa + i);
        const uint32x4_t vb = vld1q_u32(pb + i);
        vst1q_u32(po + i, add_mod_detail::add_mod_neon(va, vb, vp));
    }
    for (; i < bound; ++i) {
        const std::uint64_t s =
            static_cast<std::uint64_t>(pa[i]) +
            static_cast<std::uint64_t>(pb[i]);
        const std::uint64_t pp = static_cast<std::uint64_t>(p);
        po[i] = static_cast<std::uint32_t>(s >= pp ? s - pp : s);
    }
#elif defined(GNFS_ADD_MOD_SIMD_AVX2)
    const std::uint32_t* pa = a.data();
    const std::uint32_t* pb = b.data();
    std::uint32_t* po = out.data();
    const __m256i vp = _mm256_set1_epi32(static_cast<int>(p));
    std::size_t i = 0;
    for (; i + 8 <= bound; i += 8) {
        const __m256i va =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pa + i));
        const __m256i vb =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pb + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(po + i),
                            add_mod_detail::add_mod_avx2(va, vb, vp));
    }
    for (; i < bound; ++i) {
        const std::uint64_t s =
            static_cast<std::uint64_t>(pa[i]) +
            static_cast<std::uint64_t>(pb[i]);
        const std::uint64_t pp = static_cast<std::uint64_t>(p);
        po[i] = static_cast<std::uint32_t>(s >= pp ? s - pp : s);
    }
#else
    add_mod_p_batch_scalar(a.first(bound), b.first(bound), p,
                           out.first(bound));
#endif
}

/// Batch sub modulo p:  out[i] = (a[i] - b[i] + p) mod p
/// for `i in [0, min(a.size(), b.size(), out.size()))`. Empty input
/// leaves `out` untouched. SIMD path is taken when
/// `poly_add_mod_simd_enabled()` is true AND `p` is in the SIMD
/// window (`p <= 2^31`); otherwise falls back to the scalar
/// reference. Bit-for-bit identical output across both paths.
///
/// Precondition (caller responsibility): `a[i] < p`, `b[i] < p` for
/// all i, and `p` prime with `p < 2^32`.
inline void sub_mod_p_batch(std::span<const std::uint32_t> a,
                            std::span<const std::uint32_t> b,
                            std::uint32_t p,
                            std::span<std::uint32_t> out) noexcept {
    const std::size_t bound = std::min({a.size(), b.size(), out.size()});
    if (bound == 0) return;

    const bool use_simd = poly_add_mod_simd_enabled() &&
                          add_mod_detail::modulus_in_simd_window(p);
    if (!use_simd) {
        sub_mod_p_batch_scalar(a.first(bound), b.first(bound), p,
                               out.first(bound));
        return;
    }

#if defined(GNFS_ADD_MOD_SIMD_NEON)
    const std::uint32_t* pa = a.data();
    const std::uint32_t* pb = b.data();
    std::uint32_t* po = out.data();
    const uint32x4_t vp = vdupq_n_u32(p);
    std::size_t i = 0;
    for (; i + 4 <= bound; i += 4) {
        const uint32x4_t va = vld1q_u32(pa + i);
        const uint32x4_t vb = vld1q_u32(pb + i);
        vst1q_u32(po + i, add_mod_detail::sub_mod_neon(va, vb, vp));
    }
    for (; i < bound; ++i) {
        const std::int64_t d =
            static_cast<std::int64_t>(pa[i]) -
            static_cast<std::int64_t>(pb[i]);
        const std::int64_t pp = static_cast<std::int64_t>(p);
        po[i] = static_cast<std::uint32_t>(d < 0 ? d + pp : d);
    }
#elif defined(GNFS_ADD_MOD_SIMD_AVX2)
    const std::uint32_t* pa = a.data();
    const std::uint32_t* pb = b.data();
    std::uint32_t* po = out.data();
    const __m256i vp = _mm256_set1_epi32(static_cast<int>(p));
    std::size_t i = 0;
    for (; i + 8 <= bound; i += 8) {
        const __m256i va =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pa + i));
        const __m256i vb =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pb + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(po + i),
                            add_mod_detail::sub_mod_avx2(va, vb, vp));
    }
    for (; i < bound; ++i) {
        const std::int64_t d =
            static_cast<std::int64_t>(pa[i]) -
            static_cast<std::int64_t>(pb[i]);
        const std::int64_t pp = static_cast<std::int64_t>(p);
        po[i] = static_cast<std::uint32_t>(d < 0 ? d + pp : d);
    }
#else
    sub_mod_p_batch_scalar(a.first(bound), b.first(bound), p,
                           out.first(bound));
#endif
}

}  // namespace gnfs::polynomial
