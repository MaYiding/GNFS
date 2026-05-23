#pragma once

// SIMD-accelerated GF(2) batch XOR-then-popcount (Hamming distance) helpers
// (ARM NEON + x86 AVX2).
//
// Scope
// -----
// This header provides batched popcount over the bitwise XOR of two
// uint64_t word arrays, where the per-word output is the Hamming
// distance between `a[i]` and `b[i]` (`popcount(a[i] ^ b[i])`). The
// total reduction returns the cumulative Hamming distance over the
// full arrays. The helper is the XOR-fused counterpart of
// `and_popcnt_simd.hpp`'s AND-fused popcount and `popcount_simd.hpp`'s
// plain single-input popcount.
//
// The scalar fallback uses `__builtin_popcountll(a[i] ^ b[i])` (single
// XOR + single CNT on M-series ARM64 and on x86_64 with `-mpopcnt`);
// the SIMD path batches 16 bytes at a time so the compiler can
// schedule independent NEON `veorq_u64 + vcntq_u8` lanes (or AVX2
// 256-bit `_mm256_xor_si256` followed by `_mm256_popcnt_epi64` when
// the AVX-512 popcount extension is available, otherwise a 4-wide
// `_mm_popcnt_u64` unroll).
//
// The helpers are intentionally stand-alone (no thread-pool, no row
// dispatch, no SpMV coupling): they consume two flat `std::span<const
// uint64_t>` inputs and produce either an element-wise `uint32_t`
// output span or a single `uint64_t` total sum. Callers compose them
// into wider pipelines (e.g. Block Lanczos / Block Wiedemann dependency
// drift metric, GF(2) Hamming distance between two basis vectors,
// parity-difference checks) without paying for thread pool setup.
//
// Difference from sibling SIMD primitives (helper family member #5)
// ----------------------------------------------------------------
// * `popcount_simd.hpp` (W9): single input, returns per-word weight.
// * `and_popcnt_simd.hpp` (W10): two inputs, fused AND-then-popcount.
//   Computes `popcount(a & b)` (intersection cardinality).
// * `xor_words_simd.hpp` (W11): two inputs, in-place `dst[i] ^= src[i]`,
//   keeps the XOR'd vector as output (no reduction).
// * `and_words_simd.hpp` (W13 T1): two inputs, three-arg `out[i] = a[i]
//   & b[i]`, keeps the AND'd vector as output (no reduction).
// * **`xor_popcnt_simd.hpp` (W14 T1, this helper): two inputs, fused
//   XOR-then-popcount. Computes `popcount(a ^ b)` (Hamming distance /
//   symmetric difference cardinality).**
//
// AND-popcount counts shared bits (intersection size), XOR-popcount
// counts differing bits (symmetric difference size, i.e. Hamming
// distance). Both are reduction primitives over a fused two-input
// kernel, but they answer complementary questions about a pair of
// GF(2) word arrays.
//
// Bit-for-bit guarantee
// ---------------------
// `popcount(a ^ b)` is a pure function of the two input words: the
// SIMD path returns exactly the same per-word weight as
// `__builtin_popcountll(a ^ b)`, no rounding, no floating point. The
// reduction in `total_xor_popcount_words` is an integer sum of pure
// `uint32_t` weights, so the total is also bit-for-bit equal
// regardless of the SIMD lane count. Empty inputs return empty
// outputs / zero totals without touching pointers, matching the
// scalar reference.
//
// Build-time guards: only the host-platform implementation is
// compiled. When neither NEON nor AVX2 is available
// `xor_popcnt_simd_supported()` returns false and the dispatcher
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
    #define GNFS_XOR_POPCNT_SIMD_NEON 1
  #endif
#endif

#if defined(__AVX2__)
  #if __has_include(<immintrin.h>)
    #include <immintrin.h>
    #define GNFS_XOR_POPCNT_SIMD_AVX2 1
  #endif
#endif

namespace gnfs::linalg::detail {

/// Three-state ENV gate matching the `GNFS_GF2_AND_POPCNT_SIMD` /
/// `GNFS_GF2_POPCNT_SIMD` / `GNFS_GF2_ROW_XOR_SIMD` /
/// `GNFS_GF2_AND_WORDS_SIMD` / `GNFS_SPMV_SIMD` convention. `Auto`
/// defers to compile-time SIMD availability, `ForceOff` forces the
/// scalar path (useful for regression-bisect or sanitizer runs that
/// want to isolate kernel changes from SIMD-specific noise), and
/// `ForceOn` opts in even when `xor_popcnt_simd_supported()` is false
/// — in which case the dispatcher still falls back to scalar to keep
/// correctness.
enum class XorPopcntSimdMode : std::uint8_t {
    Auto = 0,
    ForceOff = 1,
    ForceOn = 2,
};

namespace xor_popcnt_detail {

inline std::atomic<std::uint8_t>& cached_state() noexcept {
    static std::atomic<std::uint8_t> state{static_cast<std::uint8_t>(XorPopcntSimdMode::Auto)};
    return state;
}

inline std::once_flag& cached_flag() noexcept {
    static std::once_flag flag;
    return flag;
}

inline XorPopcntSimdMode resolve_mode_from_env() noexcept {
    const char* v = std::getenv("GNFS_GF2_XOR_POPCNT_SIMD");
    if (v == nullptr) return XorPopcntSimdMode::Auto;
    if (std::strcmp(v, "0") == 0) return XorPopcntSimdMode::ForceOff;
    if (std::strcmp(v, "off") == 0) return XorPopcntSimdMode::ForceOff;
    if (std::strcmp(v, "1") == 0) return XorPopcntSimdMode::ForceOn;
    if (std::strcmp(v, "on") == 0) return XorPopcntSimdMode::ForceOn;
    // "auto" or anything else
    return XorPopcntSimdMode::Auto;
}

inline XorPopcntSimdMode load_mode() noexcept {
    std::call_once(cached_flag(), []() noexcept {
        cached_state().store(static_cast<std::uint8_t>(resolve_mode_from_env()),
                             std::memory_order_relaxed);
    });
    return static_cast<XorPopcntSimdMode>(cached_state().load(std::memory_order_relaxed));
}

}  // namespace xor_popcnt_detail

/// Compile-time SIMD availability. Constant-folds on any given build.
/// Used by the dispatcher to decide whether `Auto` / `ForceOn`
/// actually has a SIMD code path to run.
[[nodiscard]] constexpr bool xor_popcnt_simd_supported() noexcept {
#if defined(GNFS_XOR_POPCNT_SIMD_NEON) || defined(GNFS_XOR_POPCNT_SIMD_AVX2)
    return true;
#else
    return false;
#endif
}

/// Returns the parsed ENV state. Cached after first call.
[[nodiscard]] inline XorPopcntSimdMode xor_popcnt_simd_mode() noexcept {
    return xor_popcnt_detail::load_mode();
}

/// Dispatcher decision: should the batched helper take the SIMD path?
/// Returns `false` when there is no compile-time SIMD support, when
/// ENV forces it off, or when ENV is `Auto` and SIMD is unavailable.
[[nodiscard]] inline bool xor_popcnt_simd_enabled() noexcept {
    const XorPopcntSimdMode mode = xor_popcnt_simd_mode();
    if (mode == XorPopcntSimdMode::ForceOff) return false;
    // ForceOn and Auto both require compile-time availability.
    return xor_popcnt_simd_supported();
}

/// Re-read GNFS_GF2_XOR_POPCNT_SIMD from the environment and refresh
/// the cached gate. Intended for unit tests that flip the env between
/// scenarios. Not called from production hot paths.
inline void xor_popcnt_simd_reset_env_cache_for_testing() noexcept {
    xor_popcnt_detail::cached_state().store(
        static_cast<std::uint8_t>(xor_popcnt_detail::resolve_mode_from_env()),
        std::memory_order_relaxed);
    // Ensure call_once records completion even if it has never run,
    // so a later production call does not overwrite our state.
    std::call_once(xor_popcnt_detail::cached_flag(), []() noexcept {});
}

// ---------------------------------------------------------------------------
// Reference scalar paths (always available, used as golden for tests
// and as the no-SIMD fallback).
// ---------------------------------------------------------------------------

/// Scalar reference:
///   out[i] = popcount(a[i] ^ b[i])
/// using the compiler builtin. O(n) ops; one XOR + one CNT per word
/// on M-series ARM64 and on x86_64 with the POPCNT extension.
/// Pre-conditions: `a.size() == b.size()` and `out.size() >=
/// a.size()`. The implementation defensively clamps to
/// `min(a.size(), b.size(), out.size())` to avoid UB writes past the
/// output buffer.
inline void batch_xor_popcount_words_scalar(std::span<const std::uint64_t> a,
                                            std::span<const std::uint64_t> b,
                                            std::span<std::uint32_t> out) noexcept {
    assert(a.size() == b.size() && "a.size() must equal b.size()");
    const std::size_t n_ab = (a.size() < b.size()) ? a.size() : b.size();
    const std::size_t bound = (out.size() < n_ab) ? out.size() : n_ab;
    for (std::size_t i = 0; i < bound; ++i) {
        out[i] = static_cast<std::uint32_t>(__builtin_popcountll(a[i] ^ b[i]));
    }
}

/// Scalar reference: total = sum over i of popcount(a[i] ^ b[i]).
/// Accumulated in uint64_t to avoid overflow for any conceivable n
/// (each per-word weight is in [0, 64], so even 2^58 words fit
/// without overflow). Pre-conditions: `a.size() == b.size()`. The
/// implementation clamps to the shorter span if sizes diverge.
[[nodiscard]] inline std::uint64_t
total_xor_popcount_words_scalar(std::span<const std::uint64_t> a,
                                std::span<const std::uint64_t> b) noexcept {
    assert(a.size() == b.size() && "a.size() must equal b.size()");
    const std::size_t n = (a.size() < b.size()) ? a.size() : b.size();
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < n; ++i) {
        total += static_cast<std::uint64_t>(__builtin_popcountll(a[i] ^ b[i]));
    }
    return total;
}

namespace xor_popcnt_detail {

#if defined(GNFS_XOR_POPCNT_SIMD_NEON)
/// NEON inner kernel: XOR two pairs of 64-bit words, popcount each
/// resulting byte via `vcntq_u8`, then horizontal-sum each word's
/// 8-byte half with `vaddv_u8`. Two output words per call.
inline void xor_popcount_pair_neon(const std::uint64_t* a_in,
                                   const std::uint64_t* b_in,
                                   std::uint32_t* out) noexcept {
    // Load two contiguous 64-bit words per input as uint64x2_t.
    uint64x2_t va = vld1q_u64(a_in);
    uint64x2_t vb = vld1q_u64(b_in);
    // Bitwise XOR in the 128-bit register.
    uint64x2_t vxor = veorq_u64(va, vb);
    // Reinterpret as 16 bytes; vcntq_u8 emits CNT per byte (0..8 bits).
    uint8x16_t bytes = vreinterpretq_u8_u64(vxor);
    uint8x16_t counts = vcntq_u8(bytes);
    // Sum the lower 8 bytes (word 0) and the upper 8 bytes (word 1).
    uint8x8_t lo = vget_low_u8(counts);
    uint8x8_t hi = vget_high_u8(counts);
    out[0] = static_cast<std::uint32_t>(vaddv_u8(lo));
    out[1] = static_cast<std::uint32_t>(vaddv_u8(hi));
}

/// NEON path for the total sum: accumulate 2-word XOR-popcounts in a
/// uint64_t total. Single horizontal sum across 16 bytes per
/// Q-register load.
[[nodiscard]] inline std::uint64_t total_xor_popcount_neon(std::span<const std::uint64_t> a,
                                                           std::span<const std::uint64_t> b) noexcept {
    const std::size_t n = (a.size() < b.size()) ? a.size() : b.size();
    const std::uint64_t* pa = a.data();
    const std::uint64_t* pb = b.data();
    std::uint64_t total = 0;
    std::size_t i = 0;
    for (; i + 2 <= n; i += 2) {
        uint64x2_t va = vld1q_u64(pa + i);
        uint64x2_t vb = vld1q_u64(pb + i);
        uint64x2_t vxor = veorq_u64(va, vb);
        uint8x16_t bytes = vreinterpretq_u8_u64(vxor);
        uint8x16_t counts = vcntq_u8(bytes);
        // Single horizontal sum across 16 bytes (both words combined).
        total += static_cast<std::uint64_t>(vaddvq_u8(counts));
    }
    for (; i < n; ++i) {
        total += static_cast<std::uint64_t>(__builtin_popcountll(pa[i] ^ pb[i]));
    }
    return total;
}
#endif  // GNFS_XOR_POPCNT_SIMD_NEON

#if defined(GNFS_XOR_POPCNT_SIMD_AVX2)
/// AVX2 inner kernel: XOR four 64-bit words, popcount each via either
/// `_mm256_popcnt_epi64` (AVX-512 VPOPCNTDQ + VL) or a 4-wide
/// `_mm_popcnt_u64` fallback. Four output words per call.
inline void xor_popcount_quad_avx2(const std::uint64_t* a_in,
                                   const std::uint64_t* b_in,
                                   std::uint32_t* out) noexcept {
    __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a_in));
    __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b_in));
    __m256i vxor = _mm256_xor_si256(va, vb);
#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512VL__)
    // AVX-512 VPOPCNTDQ + VL: native 256-bit popcount in one instruction.
    __m256i pc = _mm256_popcnt_epi64(vxor);
    out[0] = static_cast<std::uint32_t>(_mm256_extract_epi64(pc, 0));
    out[1] = static_cast<std::uint32_t>(_mm256_extract_epi64(pc, 1));
    out[2] = static_cast<std::uint32_t>(_mm256_extract_epi64(pc, 2));
    out[3] = static_cast<std::uint32_t>(_mm256_extract_epi64(pc, 3));
#else
    // Fallback: scalar POPCNT in a 4-unrolled loop after extracting
    // four 64-bit lanes. `_mm_popcnt_u64` is one instruction on every
    // x86_64 CPU with POPCNT (Nehalem+); the unrolling exposes the
    // four independent 64-bit ops to the OoO pipeline for better
    // throughput than a serial loop.
    alignas(32) std::uint64_t lanes[4];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(lanes), vxor);
    out[0] = static_cast<std::uint32_t>(_mm_popcnt_u64(lanes[0]));
    out[1] = static_cast<std::uint32_t>(_mm_popcnt_u64(lanes[1]));
    out[2] = static_cast<std::uint32_t>(_mm_popcnt_u64(lanes[2]));
    out[3] = static_cast<std::uint32_t>(_mm_popcnt_u64(lanes[3]));
#endif
}

[[nodiscard]] inline std::uint64_t total_xor_popcount_avx2(std::span<const std::uint64_t> a,
                                                           std::span<const std::uint64_t> b) noexcept {
    const std::size_t n = (a.size() < b.size()) ? a.size() : b.size();
    const std::uint64_t* pa = a.data();
    const std::uint64_t* pb = b.data();
    std::uint64_t total = 0;
    std::size_t i = 0;
    // 4-wide unrolled inner loop.
    for (; i + 4 <= n; i += 4) {
        __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pa + i));
        __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pb + i));
        __m256i vxor = _mm256_xor_si256(va, vb);
#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512VL__)
        __m256i pc = _mm256_popcnt_epi64(vxor);
        total += static_cast<std::uint64_t>(_mm256_extract_epi64(pc, 0));
        total += static_cast<std::uint64_t>(_mm256_extract_epi64(pc, 1));
        total += static_cast<std::uint64_t>(_mm256_extract_epi64(pc, 2));
        total += static_cast<std::uint64_t>(_mm256_extract_epi64(pc, 3));
#else
        alignas(32) std::uint64_t lanes[4];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(lanes), vxor);
        total += static_cast<std::uint64_t>(_mm_popcnt_u64(lanes[0]));
        total += static_cast<std::uint64_t>(_mm_popcnt_u64(lanes[1]));
        total += static_cast<std::uint64_t>(_mm_popcnt_u64(lanes[2]));
        total += static_cast<std::uint64_t>(_mm_popcnt_u64(lanes[3]));
#endif
    }
    for (; i < n; ++i) {
        total += static_cast<std::uint64_t>(__builtin_popcountll(pa[i] ^ pb[i]));
    }
    return total;
}
#endif  // GNFS_XOR_POPCNT_SIMD_AVX2

}  // namespace xor_popcnt_detail

// ---------------------------------------------------------------------------
// Primary entry points (dispatcher).
// ---------------------------------------------------------------------------

/// Batch XOR-popcount: `out[i] = popcount(a[i] ^ b[i])` for `i in
/// [0, a.size())`. Pre-conditions: `a.size() == b.size()` and
/// `out.size() >= a.size()`. Empty input returns without touching
/// any span. SIMD path is taken when `xor_popcnt_simd_enabled()` is
/// true; otherwise falls back to the scalar reference. Bit-for-bit
/// identical output across both paths.
///
/// Defensive contract: when `out.size() < a.size()` the helper
/// clamps to `out.size()` writes (avoids UB writes past `out`). The
/// `a.size() == b.size()` invariant is asserted in debug builds.
inline void batch_xor_popcount_words(std::span<const std::uint64_t> a,
                                     std::span<const std::uint64_t> b,
                                     std::span<std::uint32_t> out) noexcept {
    assert(a.size() == b.size() && "a.size() must equal b.size()");
    const std::size_t n_ab = (a.size() < b.size()) ? a.size() : b.size();
    if (n_ab == 0) return;
    // Defensive: the API contract requires `out.size() >= n_ab`,
    // but a length mismatch would otherwise UB-write past `out`.
    // Take the smaller span to keep correctness; callers should
    // still size them equal at the call site.
    const std::size_t bound = (out.size() < n_ab) ? out.size() : n_ab;
    if (bound == 0) return;

    if (!xor_popcnt_simd_enabled()) {
        batch_xor_popcount_words_scalar(a.first(bound),
                                        b.first(bound),
                                        out.first(bound));
        return;
    }

#if defined(GNFS_XOR_POPCNT_SIMD_NEON)
    const std::uint64_t* pa = a.data();
    const std::uint64_t* pb = b.data();
    std::uint32_t* o = out.data();
    std::size_t i = 0;
    for (; i + 2 <= bound; i += 2) {
        xor_popcnt_detail::xor_popcount_pair_neon(pa + i, pb + i, o + i);
    }
    for (; i < bound; ++i) {
        o[i] = static_cast<std::uint32_t>(__builtin_popcountll(pa[i] ^ pb[i]));
    }
#elif defined(GNFS_XOR_POPCNT_SIMD_AVX2)
    const std::uint64_t* pa = a.data();
    const std::uint64_t* pb = b.data();
    std::uint32_t* o = out.data();
    std::size_t i = 0;
    for (; i + 4 <= bound; i += 4) {
        xor_popcnt_detail::xor_popcount_quad_avx2(pa + i, pb + i, o + i);
    }
    for (; i < bound; ++i) {
        o[i] = static_cast<std::uint32_t>(__builtin_popcountll(pa[i] ^ pb[i]));
    }
#else
    batch_xor_popcount_words_scalar(a.first(bound),
                                    b.first(bound),
                                    out.first(bound));
#endif
}

/// Total XOR-popcount (Hamming distance): returns the sum of
/// `popcount(a[i] ^ b[i])` over all `i`. Empty input returns zero.
/// SIMD path is taken when `xor_popcnt_simd_enabled()` is true;
/// otherwise falls back to the scalar reference. Bit-for-bit
/// identical total across both paths.
///
/// Pre-condition: `a.size() == b.size()` (asserted in debug builds).
/// If sizes diverge the implementation clamps to the shorter span.
[[nodiscard]] inline std::uint64_t
total_xor_popcount_words(std::span<const std::uint64_t> a,
                         std::span<const std::uint64_t> b) noexcept {
    assert(a.size() == b.size() && "a.size() must equal b.size()");
    if (a.empty() || b.empty()) return 0;
    if (!xor_popcnt_simd_enabled()) {
        return total_xor_popcount_words_scalar(a, b);
    }
#if defined(GNFS_XOR_POPCNT_SIMD_NEON)
    return xor_popcnt_detail::total_xor_popcount_neon(a, b);
#elif defined(GNFS_XOR_POPCNT_SIMD_AVX2)
    return xor_popcnt_detail::total_xor_popcount_avx2(a, b);
#else
    return total_xor_popcount_words_scalar(a, b);
#endif
}

}  // namespace gnfs::linalg::detail
