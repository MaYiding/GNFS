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

#include "gnfs/linalg/matrix_view.hpp"
#include "gnfs/linalg/block_lanczos.hpp"   // BlockVector
#include "gnfs/linalg/metal_spmv.hpp"
#include "gnfs/util/thread_pool.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <future>
#include <type_traits>
#include <vector>

namespace gnfs::linalg::detail {

constexpr std::ptrdiff_t SPMV_PREFETCH_AHEAD = 8;

template <MatrixView M>
inline void spmv_forward(const M& matrix,
                         const BlockVector& x,
                         BlockVector& y,
                         gnfs::util::ThreadPool& pool) {
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
        if (metal::should_use(matrix.num_rows(), matrix.num_cols())) {
            bool ok = metal::spmv_forward(
                matrix.num_rows(), matrix.num_cols(),
                matrix.row_offsets_u32(), matrix.col_indices().data(),
                matrix.nnz(),
                x.data.data(), y.data.data());
            if (ok) return;
        }
    }

    pool.parallel_for_index(0, matrix.num_rows(), [&](std::size_t i) {
        std::uint64_t acc = 0;
        const std::uint32_t* p_end  = matrix.row_end(i);
        const std::uint32_t* p_pref =
            (p_end - matrix.row_begin(i) > SPMV_PREFETCH_AHEAD)
                ? p_end - SPMV_PREFETCH_AHEAD
                : matrix.row_begin(i);
        const std::uint32_t* p = matrix.row_begin(i);
        for (; p < p_pref; ++p) {
            __builtin_prefetch(&x.data[*(p + SPMV_PREFETCH_AHEAD)], 0, 0);
            acc ^= x.data[*p];
        }
        for (; p < p_end; ++p)
            acc ^= x.data[*p];
        y.data[i] = acc;
    });
}

// Persistent thread-local scratch buffer holder. Cleared (zeroed up to n)
// on every SpMV call because the transpose kernel XOR-accumulates into
// per-thread vectors and reduces in a second pass. Each thread writes to
// its own slot, so no internal synchronisation needed beyond ThreadPool's
// per-call barrier.
struct SpmvLocals {
    std::vector<std::vector<std::uint64_t>> locals;
    void ensure(std::size_t T, std::size_t n) {
        if (locals.size() < T) locals.resize(T);
        for (std::size_t t = 0; t < T; ++t) {
            if (locals[t].size() < n) locals[t].resize(n);
            std::fill(locals[t].begin(), locals[t].begin() + static_cast<std::ptrdiff_t>(n), 0);
        }
    }
};

template <MatrixView M>
inline void spmv_transpose(const M& matrix,
                           const BlockVector& x,
                           BlockVector& y,
                           gnfs::util::ThreadPool& pool) {
    const std::size_t m = matrix.num_rows();
    const std::size_t n = y.length;
    assert(n == matrix.num_cols());
    assert(x.length == m);

    // Metal SpMV opt-in branch (default off). Same guard rationale as
    // spmv_forward: only CSRMatrix, only above threshold, only when
    // GNFS_METAL_SPMV is set, transparent CPU fallback on failure.
    if constexpr (std::is_same_v<M, CSRMatrix>) {
        if (metal::should_use(matrix.num_rows(), matrix.num_cols())) {
            bool ok = metal::spmv_transpose(
                matrix.num_rows(), matrix.num_cols(),
                matrix.row_offsets_u32(), matrix.col_indices().data(),
                matrix.nnz(),
                x.data.data(), y.data.data());
            if (ok) return;
        }
    }

    const std::size_t T = pool.num_threads();
    const std::size_t chunk = (m + T - 1) / T;

    // Per-caller-thread scratch (thread_local). Multiple concurrent owner
    // threads (e.g. GNFS_BW_KRYLOV_STREAMS=K workers each with their own
    // ThreadPool) must not share scratch — they would race on the per-pool
    // worker slots. C++ disallows capturing thread_local by reference in
    // lambdas (static storage duration), so we bind a local pointer for
    // capture; pool worker threads access through the captured pointer,
    // not their own TLS.
    thread_local SpmvLocals scratch_tls;
    scratch_tls.ensure(T, n);
    SpmvLocals* scratch = &scratch_tls;

    std::vector<std::future<void>> futures;
    futures.reserve(T);
    std::size_t T_used = 0;

    for (std::size_t t = 0; t < T; ++t) {
        const std::size_t start = t * chunk;
        const std::size_t end_row = std::min(start + chunk, m);
        if (start >= m) break;
        T_used = t + 1;
        futures.push_back(pool.submit([&matrix, &x, scratch, t, start, end_row]() {
            auto& local = scratch->locals[t];
            for (std::size_t i = start; i < end_row; ++i) {
                const std::uint64_t xi = x.data[i];
                if (xi == 0) continue;
                const std::uint32_t* p_end  = matrix.row_end(i);
                const std::uint32_t* p_pref =
                    (p_end - matrix.row_begin(i) > SPMV_PREFETCH_AHEAD)
                        ? p_end - SPMV_PREFETCH_AHEAD
                        : matrix.row_begin(i);
                const std::uint32_t* p = matrix.row_begin(i);
                for (; p < p_pref; ++p) {
                    __builtin_prefetch(&local[*(p + SPMV_PREFETCH_AHEAD)], 0, 0);
                    local[*p] ^= xi;
                }
                for (; p < p_end; ++p)
                    local[*p] ^= xi;
            }
        }));
    }
    for (auto& f : futures) f.get();

    pool.parallel_for_index(0, n, [&y, scratch, T_used](std::size_t j) {
        std::uint64_t val = 0;
        for (std::size_t t = 0; t < T_used; ++t) val ^= scratch->locals[t][j];
        y.data[j] = val;
    });
}

// B = M·M^T (operates on R^m, used by standard wide-matrix BW path).
// tmp must have length matrix.num_cols(); y must have length matrix.num_rows().
template <MatrixView M>
inline void spmv_B(const M& matrix,
                   const BlockVector& x,
                   BlockVector& y,
                   BlockVector& tmp,
                   gnfs::util::ThreadPool& pool) {
    spmv_transpose(matrix, x, tmp, pool);
    spmv_forward(matrix, tmp, y, pool);
}

// B' = M^T·M (operates on R^n, used by thin-matrix BW path).
// tmp must have length matrix.num_rows(); y must have length matrix.num_cols().
template <MatrixView M>
inline void spmv_B_prime(const M& matrix,
                         const BlockVector& x,
                         BlockVector& y,
                         BlockVector& tmp,
                         gnfs::util::ThreadPool& pool) {
    spmv_forward(matrix, x, tmp, pool);
    spmv_transpose(matrix, tmp, y, pool);
}

} // namespace gnfs::linalg::detail
