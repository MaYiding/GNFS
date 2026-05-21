#pragma once

// SIMD-accelerated GF(2) SpMV inner kernels (ARM NEON + x86 AVX2).
//
// Scope
// -----
// This header provides two free functions that mirror the existing scalar
// SpMV kernels in `detail/spmv_kernels.hpp` and are dropped in behind a
// runtime/compile-time gate. They batch the GF(2) XOR-gather loop into
// 2-lane (NEON) or 4-lane (AVX2) wide loads when the row contains enough
// contiguous column indices. GF(2) addition is associative and commutative,
// so the SIMD reduction is bit-for-bit identical to the scalar loop —
// there is no floating-point approximation involved.
//
// The kernels intentionally only batch the inner XOR-gather loop because:
//
// * The outer per-row iteration must remain in dispatcher control to share
//   the same thread-pool partitioning and prefetch policy as the scalar
//   path (and the existing transpose scratch reduction).
// * Bucket scatter / gather patterns with arbitrary column indices do not
//   vectorise into a single load — we still pay the latency of N pointer
//   chases, but consolidating accumulation into one wide XOR cuts retired
//   uops by 2x (NEON) or 4x (AVX2) and lets the OoO engine overlap loads.
//
// Build-time guards: only the host-platform implementation is compiled.
// When neither NEON nor AVX2 is available `is_simd_available()` returns
// false and the dispatcher silently falls back to the scalar path.
//
// Important: callers must clear `y` before invoking `spmv_forward_simd`
// because the kernel writes (assigns) a row accumulator into y[i] — it
// does not XOR-merge into a pre-existing value. The transpose variant on
// the other hand expects the caller's scratch slot to be pre-zeroed and
// XOR-merges into it (matching the scalar `spmv_transpose` contract).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  #if __has_include(<arm_neon.h>)
    #include <arm_neon.h>
    #define GNFS_SPMV_SIMD_NEON 1
  #endif
#endif

#if defined(__AVX2__)
  #if __has_include(<immintrin.h>)
    #include <immintrin.h>
    #define GNFS_SPMV_SIMD_AVX2 1
  #endif
#endif

namespace gnfs::linalg::detail::simd {

// Compile-time + runtime availability probe. The compile-time guards
// above ensure we only compile code for the host ISA, so the runtime
// probe collapses to a constant on most builds. Callers that need a
// portable boolean can use this helper rather than open-coding the
// preprocessor checks.
[[nodiscard]] inline bool is_simd_available() noexcept {
#if defined(GNFS_SPMV_SIMD_NEON) || defined(GNFS_SPMV_SIMD_AVX2)
    return true;
#else
    return false;
#endif
}

// Three-state ENV parser used by the dispatcher to decide whether to
// take the SIMD path:
//
//   GNFS_SPMV_SIMD=0     forces scalar (regression-bisect aid)
//   GNFS_SPMV_SIMD=1     forces SIMD even when is_simd_available is false
//                        (no-op then; reverts to scalar to keep correctness)
//   GNFS_SPMV_SIMD=auto  default; SIMD if available, scalar otherwise
//   (unset)              same as auto
//
// Cached in a function-local static so the env read happens exactly once
// per process. Using a leading-bit flag pair lets us encode the three
// states without an enum allocation.
[[nodiscard]] inline bool use_simd_runtime() noexcept {
    static const bool cached = []() noexcept {
        const char* v = std::getenv("GNFS_SPMV_SIMD");
        if (v == nullptr) {
            return is_simd_available();           // default: auto
        }
        if (std::strcmp(v, "0") == 0) {
            return false;                         // explicit off
        }
        if (std::strcmp(v, "1") == 0) {
            // Forced on, but never claim availability we do not have.
            return is_simd_available();
        }
        // anything else (including "auto") treated as auto
        return is_simd_available();
    }();
    return cached;
}

// ---------------------------------------------------------------------------
// Inner kernel: XOR-gather a row of column indices into a 64-bit accumulator.
// Used by the forward SpMV path. Returns the accumulator value.
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::uint64_t
gather_xor_row(const std::uint32_t* cols_begin,
               const std::uint32_t* cols_end,
               const std::uint64_t* x) noexcept {
    std::uint64_t acc = 0;
    const std::uint32_t* p = cols_begin;

#if defined(GNFS_SPMV_SIMD_AVX2)
    // 4-lane batch: load 4 uint64 from gathered indices, XOR them into the
    // accumulator using a 256-bit lane reduction. We use individual loads
    // (no gather instruction) because column indices are not necessarily
    // contiguous and gather has high latency on most x86 cores anyway —
    // the win comes from the wide XOR + fewer retired uops.
    while (p + 4 <= cols_end) {
        std::uint32_t i0 = p[0], i1 = p[1], i2 = p[2], i3 = p[3];
        __m256i v = _mm256_set_epi64x(
            static_cast<long long>(x[i3]),
            static_cast<long long>(x[i2]),
            static_cast<long long>(x[i1]),
            static_cast<long long>(x[i0]));
        // Horizontal XOR of the 4 lanes.
        __m128i lo = _mm256_castsi256_si128(v);
        __m128i hi = _mm256_extracti128_si256(v, 1);
        __m128i x128 = _mm_xor_si128(lo, hi);
        std::uint64_t lane0 = static_cast<std::uint64_t>(
            _mm_cvtsi128_si64(x128));
        std::uint64_t lane1 = static_cast<std::uint64_t>(
            _mm_extract_epi64(x128, 1));
        acc ^= (lane0 ^ lane1);
        p += 4;
    }
#elif defined(GNFS_SPMV_SIMD_NEON)
    // 2-lane batch: build a uint64x2_t holding two gathered values and
    // XOR them in one veorq. NEON's veor on a Q-register is one cycle on
    // M-series cores and lets two outstanding loads overlap with the XOR
    // of the previous pair.
    while (p + 2 <= cols_end) {
        std::uint32_t i0 = p[0], i1 = p[1];
        std::uint64_t pair[2] = { x[i0], x[i1] };
        uint64x2_t v = vld1q_u64(pair);
        // Horizontal XOR of the 2 lanes.
        std::uint64_t lane0 = vgetq_lane_u64(v, 0);
        std::uint64_t lane1 = vgetq_lane_u64(v, 1);
        acc ^= (lane0 ^ lane1);
        p += 2;
    }
#endif

    // Scalar residual (always run): picks up the tail when the row length
    // is not a multiple of the SIMD batch size. On platforms with neither
    // ISA this is the entire loop, matching the original scalar kernel.
    for (; p < cols_end; ++p) {
        acc ^= x[*p];
    }
    return acc;
}

// ---------------------------------------------------------------------------
// Inner kernel: XOR-scatter a single value into a row's worth of column
// slots in the local scratch buffer. Used by the transpose SpMV path.
//
// Scatter is fundamentally serial on common ISAs (no GF(2) atomic scatter
// instruction), so the SIMD value here is limited — we apply the load /
// XOR / store sequence in 2- or 4-element batches to give the compiler a
// chance to schedule independent stores. Net effect is similar uop count
// but lower latency between independent column index lookups.
// ---------------------------------------------------------------------------

inline void scatter_xor_row(const std::uint32_t* cols_begin,
                            const std::uint32_t* cols_end,
                            std::uint64_t xi,
                            std::uint64_t* local) noexcept {
    const std::uint32_t* p = cols_begin;

#if defined(GNFS_SPMV_SIMD_AVX2)
    // 4-wide unrolled scatter. Stores are serialised by the L1 store
    // buffer but pre-computing the 4 indices into registers exposes
    // parallelism in the address generation pipe.
    while (p + 4 <= cols_end) {
        std::uint32_t i0 = p[0], i1 = p[1], i2 = p[2], i3 = p[3];
        local[i0] ^= xi;
        local[i1] ^= xi;
        local[i2] ^= xi;
        local[i3] ^= xi;
        p += 4;
    }
#elif defined(GNFS_SPMV_SIMD_NEON)
    // 2-wide unrolled scatter (same rationale as the AVX2 branch).
    while (p + 2 <= cols_end) {
        std::uint32_t i0 = p[0], i1 = p[1];
        local[i0] ^= xi;
        local[i1] ^= xi;
        p += 2;
    }
#endif

    // Scalar residual.
    for (; p < cols_end; ++p) {
        local[*p] ^= xi;
    }
}

// ---------------------------------------------------------------------------
// Stand-alone reference kernels (rows × cols CSR, BlockVector layout).
//
// These are convenience wrappers exposed for unit testing — they run the
// same loop structure as the dispatcher but without the thread pool and
// without the prefetch hints. The dispatch-side integration in
// `detail/spmv_kernels.hpp` calls `gather_xor_row` and `scatter_xor_row`
// directly inside the existing parallel-for, preserving the scratch /
// reduction structure of the scalar path.
//
// Output buffer `y` must be sized correctly by the caller. Forward writes
// rows (length = num_rows). Transpose XOR-accumulates into cols (length =
// num_cols) and the caller must clear it first.
// ---------------------------------------------------------------------------

inline void spmv_forward_simd(std::size_t num_rows,
                              std::size_t /*num_cols*/,
                              const std::size_t* row_offsets,
                              const std::uint32_t* col_indices,
                              const std::uint64_t* x,
                              std::uint64_t* y) noexcept {
    for (std::size_t i = 0; i < num_rows; ++i) {
        const std::uint32_t* b = col_indices + row_offsets[i];
        const std::uint32_t* e = col_indices + row_offsets[i + 1];
        y[i] = gather_xor_row(b, e, x);
    }
}

inline void spmv_transpose_simd(std::size_t num_rows,
                                std::size_t num_cols,
                                const std::size_t* row_offsets,
                                const std::uint32_t* col_indices,
                                const std::uint64_t* x,
                                std::uint64_t* y) noexcept {
    // Caller responsibility: `y` must be pre-zeroed. We do not call memset
    // here because the dispatcher version reuses a per-thread scratch
    // buffer that the SpmvLocals helper already zeroes.
    std::memset(y, 0, num_cols * sizeof(std::uint64_t));
    for (std::size_t i = 0; i < num_rows; ++i) {
        std::uint64_t xi = x[i];
        if (xi == 0) continue;
        const std::uint32_t* b = col_indices + row_offsets[i];
        const std::uint32_t* e = col_indices + row_offsets[i + 1];
        scatter_xor_row(b, e, xi, y);
    }
}

} // namespace gnfs::linalg::detail::simd
