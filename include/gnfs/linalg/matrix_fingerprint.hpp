#pragma once

#include "gnfs/linalg/sparse_matrix.hpp"
#include "gnfs/util/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace gnfs::linalg {

/// Bind a matrix to its ordered row/column coordinates. indices() uses the
/// existing sparse-row normalization; already-normalized rows need no copies.
[[nodiscard]] inline util::Sha256Digest sparse_matrix_fingerprint(const SparseMatrix& matrix) {
    util::Sha256Accumulator hash;
    if (!hash.update("GNFS_SPARSE_MATRIX_ID_V1")) {
        throw std::length_error("Sparse matrix fingerprint input is too large");
    }
    const auto append_u64 = [&hash](uint64_t value) {
        std::array<std::byte, sizeof(uint64_t)> bytes{};
        for (size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::byte>(value >> (index * 8));
        }
        if (!hash.update(bytes)) {
            throw std::length_error("Sparse matrix fingerprint input is too large");
        }
    };
    append_u64(static_cast<uint64_t>(matrix.num_rows()));
    append_u64(static_cast<uint64_t>(matrix.num_cols()));
    for (size_t row = 0; row < matrix.num_rows(); ++row) {
        const auto& indices = matrix.row(row).indices();
        append_u64(static_cast<uint64_t>(indices.size()));
        for (uint32_t column : indices) {
            append_u64(column);
        }
    }
    const auto digest = hash.finalize();
    if (!digest) {
        throw std::length_error("Sparse matrix fingerprint input is too large");
    }
    return *digest;
}

} // namespace gnfs::linalg
