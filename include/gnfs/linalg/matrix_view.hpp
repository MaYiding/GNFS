#pragma once

#include "gnfs/linalg/sparse_matrix.hpp"
#include "gnfs/linalg/mmap_csr_matrix.hpp"
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace gnfs::linalg {

/// C++20 concept enumerating the contract every read-only CSR-style matrix
/// must satisfy so it can serve as an SpMV operand.
///
/// Why a concept (not virtual / variant):
/// - SpMV is the hottest GF(2) loop in Phase 5 (`bw_spmv_forward`,
///   `bw_spmv_transpose`). It runs 2·L times per BW step where L scales
///   with sqrt(n) for matrix BM. Even a single vtable dispatch per row
///   would land on the inner pointer-chase that the prefetch logic in
///   `block_wiedemann.cpp` already battles. A concept gives full
///   compile-time devirtualisation so the existing prefetch / unroll
///   work continues to inline.
/// - The two concrete types (`CSRMatrix`, `MmapCSRMatrix`) already share
///   the same accessor names. The concept just names that contract.
/// - Open for future shard / distributed matrix types without changing
///   call sites.
///
/// Required accessors (all `const`, return-type listed):
///   `num_rows()  -> size_t`
///   `num_cols()  -> size_t`
///   `nnz()       -> size_t`
///   `row_begin(i)-> const uint32_t*`   (i in [0, num_rows))
///   `row_end(i)  -> const uint32_t*`
///   `row_nnz(i)  -> size_t`
template <typename M>
concept MatrixView = requires (const M& m, std::size_t i) {
    { m.num_rows() }    -> std::convertible_to<std::size_t>;
    { m.num_cols() }    -> std::convertible_to<std::size_t>;
    { m.nnz() }         -> std::convertible_to<std::size_t>;
    { m.row_begin(i) }  -> std::convertible_to<const std::uint32_t*>;
    { m.row_end(i) }    -> std::convertible_to<const std::uint32_t*>;
    { m.row_nnz(i) }    -> std::convertible_to<std::size_t>;
};

// Compile-time check that both existing matrix types satisfy the contract.
// Any divergence between the two will surface at build time, not runtime.
static_assert(MatrixView<CSRMatrix>,
              "CSRMatrix must satisfy MatrixView concept");
static_assert(MatrixView<MmapCSRMatrix>,
              "MmapCSRMatrix must satisfy MatrixView concept");

} // namespace gnfs::linalg
