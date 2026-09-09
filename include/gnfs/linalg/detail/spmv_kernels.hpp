#pragma once

// Internal SpMV templates shared by block_wiedemann.cpp and any other
// solver that wants to plug an alternative MatrixView into the
// devirtualised hot loop.
//
// Implementation notes (preserved from the original anonymous-namespace
// kernels in block_wiedemann.cpp):
//
// * SPMV_PREFETCH_AHEAD comes from the PMU baseline 2026-05-13 study
//   (BackendStallRate=74.79%, L1DMissRate=12.80%). Splitting the row
//   scan into two phases lets us prefetch the next x.data[*p] without
//   adding a branch to the inner loop. N=8 is chosen for M5
//   L1D line size 64 B and load-to-use ~4 cy. Locality hint = 0
//   (streaming, no L2 retention).
//
// * `bw_spmv_transpose` uses a persistent per-thread scratch buffer so
//   alloc count drops from O(L) to O(1). Templating only the matrix
//   accessor leaves that optimisation intact — the scratch struct is a
//   single function-local static, shared by all matrix types.
//
// Concept: any type satisfying `MatrixView` works. CSRMatrix and
// MmapCSRMatrix already satisfy the concept (see matrix_view.hpp).

#include "gnfs/linalg/block_lanczos.hpp" // BlockVector
#include "gnfs/linalg/detail/spmv_simd.hpp"
#include "gnfs/linalg/matrix_view.hpp"
#include "gnfs/linalg/metal_spmv.hpp"
#include "gnfs/util/cpu_intrin.hpp"
#include "gnfs/util/thread_pool.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace gnfs::linalg::detail {

constexpr std::ptrdiff_t SPMV_PREFETCH_AHEAD = 8;

/// Metal dispatch mode.  `environment` preserves the legacy process-global
/// opt-in behavior; the other values are explicit and never call
/// `metal::env_opt_in()` (important for bounded seeded solves).
enum class SpmvMetalPolicy : std::uint8_t {
    environment,
    enabled,
    disabled,
};

[[nodiscard]] inline bool should_use_metal(std::size_t num_rows, std::size_t num_cols,
                                           SpmvMetalPolicy policy) noexcept {
    switch (policy) {
    case SpmvMetalPolicy::environment:
        return metal::should_use(num_rows, num_cols);
    case SpmvMetalPolicy::enabled:
        return metal::is_available() && metal::size_above_threshold(num_rows, num_cols);
    case SpmvMetalPolicy::disabled:
        return false;
    }
    return false;
}

inline void validate_block_vector_shape(const BlockVector& vector, std::size_t expected,
                                        const char* operation) {
    if (vector.length != expected || vector.data.size() < expected) {
        throw std::invalid_argument(std::string(operation) + ": incompatible block vector length");
    }
}

template <MatrixView M>
inline void spmv_forward(const M& matrix, const BlockVector& x, BlockVector& y,
                         gnfs::util::ThreadPool& pool,
                         SpmvMetalPolicy metal_policy = SpmvMetalPolicy::environment) {
    validate_block_vector_shape(x, matrix.num_cols(), "spmv_forward input");
    validate_block_vector_shape(y, matrix.num_rows(), "spmv_forward output");
    // x.length == matrix.num_cols() by contract — CSRMatrix ctor and the
    // MmapCSRMatrix v2 file layout both validate col < num_cols at build
    // time, so the inner loop can skip per-element bounds checks.
    assert(x.length == matrix.num_cols());
    assert(y.length == matrix.num_rows());

    // Metal SpMV opt-in branch (default off). Only enabled for in-memory
    // CSRMatrix because (a) it is the common Phase 1 path and (b) the
    // uint32 row_offsets view is exposed only on that type. Falls
    // through to the CPU kernel on any failure so correctness never
    // depends on the GPU path succeeding.
    if constexpr (std::is_same_v<M, CSRMatrix>) {
        if (should_use_metal(matrix.num_rows(), matrix.num_cols(), metal_policy) &&
            matrix.nnz() <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            bool ok = metal::spmv_forward(matrix.num_rows(), matrix.num_cols(),
                                          matrix.row_offsets_u32(), matrix.col_indices().data(),
                                          matrix.nnz(), x.data.data(), y.data.data());
            if (ok)
                return;
        }
    }

    // Cache the SIMD decision once per call (the helper itself caches the
    // ENV read across calls, so this is a memory-load + branch only).
    const bool simd_on = simd::use_simd_runtime();

    pool.parallel_for_index(0, matrix.num_rows(), [&, simd_on](std::size_t i) {
        // Fetch both bounds before consulting row_nnz so MatrixView accessor
        // failures are still surfaced and drained even for an empty row. No
        // pointer arithmetic is performed until the non-empty case below.
        const std::uint32_t* p_begin = matrix.row_begin(i);
        const std::uint32_t* p_end = matrix.row_end(i);
        const std::size_t row_nnz = matrix.row_nnz(i);
        if (row_nnz == 0) {
            y.data[i] = 0;
            return;
        }
        std::uint64_t acc = 0;
        const std::uint32_t* p_pref =
            (row_nnz > SPMV_PREFETCH_AHEAD) ? p_end - SPMV_PREFETCH_AHEAD : p_begin;
        const std::uint32_t* p = p_begin;
        // Prefetch phase stays scalar — the prefetch hint references one
        // element ahead and the gather is naturally serialised by the
        // hardware load queue. Mixing SIMD here would either drop the
        // prefetch (correctness preserved, latency loss) or duplicate it.
        for (; p < p_pref; ++p) {
            gnfs::util::prefetch_read<0>(&x.data[*(p + SPMV_PREFETCH_AHEAD)]);
            acc ^= x.data[*p];
        }
        // Tail phase batches into the wide XOR helper when the SIMD path
        // is enabled. The helper falls back to scalar on hosts without
        // NEON or AVX2 so the dispatch decision degrades gracefully.
        if (simd_on) {
            acc ^= simd::gather_xor_row(p, p_end, x.data.data());
        } else {
            for (; p < p_end; ++p)
                acc ^= x.data[*p];
        }
        y.data[i] = acc;
    });
}

// Persistent thread-local scratch buffer holder. The transpose kernel
// XOR-accumulates into per-thread vectors and reduces in a second pass. The
// holder deliberately drops slots and backing allocations when a later call
// has a different shape; otherwise a previous solve would keep charging
// memory against a future call that was admitted under its own estimate.
// Each thread writes to its own slot, so no internal synchronisation is needed
// beyond ThreadPool's per-call barrier.
struct SpmvLocals {
    std::vector<std::vector<std::uint64_t>> locals;
    void ensure(std::size_t T, std::size_t n) {
        if (locals.size() > T) {
            // Release the outer vector first.  Merely resizing would leave
            // its capacity (and the unused worker slots) retained in TLS.
            std::vector<std::vector<std::uint64_t>> released;
            locals.swap(released);
        }
        if (locals.size() < T)
            locals.resize(T);

        // Release every stale slot before allocating any replacement. Doing
        // this as a separate pass avoids a transient old-plus-new peak when a
        // later solve grows the column dimension on the same owner thread.
        bool shape_changed = false;
        for (std::size_t t = 0; t < T; ++t) {
            if (locals[t].size() != n || locals[t].capacity() != n) {
                shape_changed = true;
                break;
            }
        }
        if (shape_changed) {
            for (std::size_t t = 0; t < T; ++t) {
                std::vector<std::uint64_t> released;
                locals[t].swap(released);
            }
        }

        for (std::size_t t = 0; t < T; ++t) {
            if (locals[t].size() != n)
                locals[t].resize(n);
            std::fill(locals[t].begin(), locals[t].end(), 0);
        }
    }
};

// Keep one scratch owner per calling thread across all MatrixView
// specializations. A function-template static would create separate TLS
// buffers for CSRMatrix and MmapCSRMatrix, allowing their peak allocations to
// accumulate outside the sparse solver's per-call workspace estimate.
inline SpmvLocals& transpose_scratch_locals() noexcept {
    thread_local SpmvLocals scratch_tls;
    return scratch_tls;
}

template <MatrixView M>
inline void spmv_transpose(const M& matrix, const BlockVector& x, BlockVector& y,
                           gnfs::util::ThreadPool& pool,
                           SpmvMetalPolicy metal_policy = SpmvMetalPolicy::environment) {
    const std::size_t m = matrix.num_rows();
    const std::size_t n = y.length;
    validate_block_vector_shape(x, m, "spmv_transpose input");
    validate_block_vector_shape(y, matrix.num_cols(), "spmv_transpose output");
    assert(n == matrix.num_cols());
    assert(x.length == m);

    // Metal SpMV opt-in branch (default off). Same guard rationale as
    // spmv_forward: only CSRMatrix, only above threshold, only when
    // GNFS_METAL_SPMV is set, transparent CPU fallback on failure.
    if constexpr (std::is_same_v<M, CSRMatrix>) {
        if (should_use_metal(matrix.num_rows(), matrix.num_cols(), metal_policy) &&
            matrix.nnz() <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            bool ok = metal::spmv_transpose(matrix.num_rows(), matrix.num_cols(),
                                            matrix.row_offsets_u32(), matrix.col_indices().data(),
                                            matrix.nnz(), x.data.data(), y.data.data());
            if (ok)
                return;
        }
    }

    const std::size_t T = pool.num_threads();
    if (T == 0) {
        throw std::runtime_error("spmv_transpose: thread pool has no workers");
    }
    // Use quotient/remainder partitioning instead of (m + T - 1) / T. The
    // latter wraps for a representable row count near SIZE_MAX, even though
    // each resulting range could otherwise be formed safely.
    const std::size_t base_rows = m / T;
    const std::size_t remainder_rows = m % T;

    // Per-caller-thread scratch (thread_local). Multiple concurrent owner
    // threads (e.g. GNFS_BW_KRYLOV_STREAMS=K workers each with their own
    // ThreadPool) must not share scratch — they would race on the per-pool
    // worker slots. C++ disallows capturing thread_local by reference in
    // lambdas (static storage duration), so we bind a local pointer for
    // capture; pool worker threads access through the captured pointer,
    // not their own TLS.
    SpmvLocals& scratch_tls = transpose_scratch_locals();
    scratch_tls.ensure(T, n);
    SpmvLocals* scratch = &scratch_tls;

    std::vector<std::future<void>> futures;
    futures.reserve(T);
    std::size_t T_used = 0;

    // Read the SIMD decision once per call rather than per row — the helper
    // already caches the env lookup, but the function-static load still
    // costs a few cycles relative to a plain bool capture.
    const bool simd_on = simd::use_simd_runtime();

    for (std::size_t t = 0; t < T; ++t) {
        const std::size_t start = t * base_rows + std::min(t, remainder_rows);
        const std::size_t end_row = start + base_rows + (t < remainder_rows ? 1 : 0);
        if (start >= m)
            break;
        T_used = t + 1;
        futures.push_back(pool.submit([&matrix, &x, scratch, t, start, end_row, simd_on]() {
            auto& local = scratch->locals[t];
            for (std::size_t i = start; i < end_row; ++i) {
                const std::uint64_t xi = x.data[i];
                if (xi == 0)
                    continue;
                const std::uint32_t* p_begin = matrix.row_begin(i);
                const std::uint32_t* p_end = matrix.row_end(i);
                const std::size_t row_nnz = matrix.row_nnz(i);
                if (row_nnz == 0)
                    continue;
                const std::uint32_t* p_pref =
                    (row_nnz > SPMV_PREFETCH_AHEAD) ? p_end - SPMV_PREFETCH_AHEAD : p_begin;
                const std::uint32_t* p = p_begin;
                // Prefetch phase stays scalar so the L1 prefetcher
                // continues to see one access at a time and the prefetch
                // hint to `local[*(p+AHEAD)]` keeps its meaning.
                for (; p < p_pref; ++p) {
                    gnfs::util::prefetch_read<0>(&local[*(p + SPMV_PREFETCH_AHEAD)]);
                    local[*p] ^= xi;
                }
                // Batch the tail through the SIMD scatter helper when the
                // SIMD path is enabled. The helper takes a raw pointer to
                // the scratch buffer and unrolls the XOR-store sequence.
                if (simd_on) {
                    simd::scatter_xor_row(p, p_end, xi, local.data());
                } else {
                    for (; p < p_end; ++p)
                        local[*p] ^= xi;
                }
            }
        }));
    }
    // Drain every submitted task before propagating a worker failure. A
    // MatrixView accessor may throw from a worker; returning immediately on
    // the first future would leave later tasks using the caller's references
    // after this function unwinds.
    std::exception_ptr first_exception;
    for (auto& f : futures) {
        try {
            f.get();
        } catch (...) {
            if (!first_exception)
                first_exception = std::current_exception();
        }
    }
    if (first_exception)
        std::rethrow_exception(first_exception);

    pool.parallel_for_index(0, n, [&y, scratch, T_used](std::size_t j) {
        std::uint64_t val = 0;
        for (std::size_t t = 0; t < T_used; ++t)
            val ^= scratch->locals[t][j];
        y.data[j] = val;
    });
}

// B = M·M^T (operates on R^m, used by standard wide-matrix BW path).
// tmp must have length matrix.num_cols(); y must have length matrix.num_rows().
template <MatrixView M>
inline void spmv_B(const M& matrix, const BlockVector& x, BlockVector& y, BlockVector& tmp,
                   gnfs::util::ThreadPool& pool,
                   SpmvMetalPolicy metal_policy = SpmvMetalPolicy::environment) {
    spmv_transpose(matrix, x, tmp, pool, metal_policy);
    spmv_forward(matrix, tmp, y, pool, metal_policy);
}

// B' = M^T·M (operates on R^n, used by thin-matrix BW path).
// tmp must have length matrix.num_rows(); y must have length matrix.num_cols().
template <MatrixView M>
inline void spmv_B_prime(const M& matrix, const BlockVector& x, BlockVector& y, BlockVector& tmp,
                         gnfs::util::ThreadPool& pool,
                         SpmvMetalPolicy metal_policy = SpmvMetalPolicy::environment) {
    spmv_forward(matrix, x, tmp, pool, metal_policy);
    spmv_transpose(matrix, tmp, y, pool, metal_policy);
}

} // namespace gnfs::linalg::detail
