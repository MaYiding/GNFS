#pragma once

// Metal-backed sparse-matrix-vector multiplication for the GF(2) 64-bit
// block kernels used by Block Wiedemann. The actual implementation lives
// in `src/linalg/metal_spmv.mm` (Objective-C++, macOS only). On non-Apple
// platforms `src/linalg/metal_spmv_stub.cpp` provides stubs whose
// `is_available()` always returns false, so callers transparently fall
// back to the existing CPU path.
//
// Design notes
// ------------
// * Defaults to OFF. Callers must set `GNFS_METAL_SPMV=1` (and the runtime
//   must report `is_available() == true`) to take the GPU path. The CPU
//   path is preserved bit-for-bit when the GPU path is not used.
// * Bit-for-bit equivalence with the CPU kernels in
//   `gnfs/linalg/detail/spmv_kernels.hpp` is the correctness requirement.
//   GF(2) XOR over `uint64_t` is exact (no float, no reordering hazard);
//   any deviation indicates a real bug in the shader or buffer layout.
// * The interface takes raw CSR pointers + the dense vector pointers so
//   the Metal layer can stay decoupled from the C++ `MatrixView` concept
//   (it does not need to know about `CSRMatrix` vs `MmapCSRMatrix`).
// * Buffer reuse is keyed on `(nnz, num_rows, num_cols, last-seen pointer)`.
//   For repeated SpMV calls against the same matrix (the BW Krylov phase
//   pattern), the matrix buffers are kept hot on the GPU.

#include <cstddef>
#include <cstdint>

namespace gnfs::linalg::metal {

/// True when (a) compiled with Metal support, (b) the runtime device is
/// present, and (c) the GF(2) shaders compiled successfully. False
/// triggers the CPU fallback inside `bw_spmv` / `bw_spmv_transpose`.
[[nodiscard]] bool is_available() noexcept;

/// True when the env var `GNFS_METAL_SPMV` is set to a value that decodes
/// as "on" (`1`, `true`, `yes`, `on`, case-insensitive). Independent of
/// `is_available()` so the caller can decide whether to even probe the
/// device.
[[nodiscard]] bool env_opt_in() noexcept;

/// y[i] = XOR over j in row(i) of x[col_idx[j]], for i in [0, num_rows).
/// `row_offsets` has `num_rows + 1` entries, `col_indices` has
/// `row_offsets[num_rows]` entries. Returns true on success; false means
/// the caller must run the CPU kernel (the Metal path either disabled,
/// shader allocation failed, or the matrix size is below threshold).
bool spmv_forward(std::size_t num_rows,
                  std::size_t num_cols,
                  const std::uint32_t* row_offsets,
                  const std::uint32_t* col_indices,
                  std::size_t nnz,
                  const std::uint64_t* x,
                  std::uint64_t* y) noexcept;

/// For each row i in [0, num_rows): if x[i] != 0, XOR x[i] into y[col]
/// for every col in row(i). Output buffer y has `num_cols` entries and
/// MUST be zero-initialised by the caller (the kernel does not clear).
/// Returns true on success; false → CPU fallback.
bool spmv_transpose(std::size_t num_rows,
                    std::size_t num_cols,
                    const std::uint32_t* row_offsets,
                    const std::uint32_t* col_indices,
                    std::size_t nnz,
                    const std::uint64_t* x,
                    std::uint64_t* y) noexcept;

/// Matrices below this row count fall back to CPU because the kernel
/// launch overhead (~50 us on M-series) dominates the compute time.
/// Tuned empirically against the 10K row threshold called out in the
/// task brief; expose as a constant so tests can override via a separate
/// probe entry point.
constexpr std::size_t kGpuSizeThreshold = 10'000;

/// True when matrix dimensions warrant taking the GPU path under the
/// current threshold policy. Centralised so the dispatcher and the test
/// suite agree on the boundary.
[[nodiscard]] constexpr bool size_above_threshold(std::size_t num_rows,
                                                  std::size_t num_cols) noexcept {
    // Either dimension above threshold is enough — `spmv_transpose`
    // does work proportional to `num_rows` reads and `num_cols` writes.
    return num_rows >= kGpuSizeThreshold || num_cols >= kGpuSizeThreshold;
}

/// Convenience predicate: true iff both env opt-in and runtime
/// availability hold AND the matrix is large enough. Used by the
/// dispatcher in `spmv_kernels.hpp`.
[[nodiscard]] inline bool should_use(std::size_t num_rows,
                                     std::size_t num_cols) noexcept {
    return env_opt_in() && is_available()
        && size_above_threshold(num_rows, num_cols);
}

} // namespace gnfs::linalg::metal
