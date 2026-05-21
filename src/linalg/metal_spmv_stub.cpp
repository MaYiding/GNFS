// Stub implementation of `gnfs/linalg/metal_spmv.hpp` for non-Apple
// builds. All Metal calls return false / unavailable so the dispatcher
// in `spmv_kernels.hpp` transparently falls back to the CPU path. This
// keeps Linux CI green without conditional compilation in headers.

#include "gnfs/linalg/metal_spmv.hpp"

namespace gnfs::linalg::metal {

bool is_available() noexcept { return false; }

bool env_opt_in() noexcept { return false; }

bool spmv_forward(std::size_t /*num_rows*/,
                  std::size_t /*num_cols*/,
                  const std::uint32_t* /*row_offsets*/,
                  const std::uint32_t* /*col_indices*/,
                  std::size_t /*nnz*/,
                  const std::uint64_t* /*x*/,
                  std::uint64_t* /*y*/) noexcept {
    return false;
}

bool spmv_transpose(std::size_t /*num_rows*/,
                    std::size_t /*num_cols*/,
                    const std::uint32_t* /*row_offsets*/,
                    const std::uint32_t* /*col_indices*/,
                    std::size_t /*nnz*/,
                    const std::uint64_t* /*x*/,
                    std::uint64_t* /*y*/) noexcept {
    return false;
}

} // namespace gnfs::linalg::metal
