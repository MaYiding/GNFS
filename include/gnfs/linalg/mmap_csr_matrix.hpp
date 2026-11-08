#pragma once

#include "gnfs/linalg/sparse_matrix.hpp"
#include "gnfs/util/mmap_file.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

namespace gnfs::linalg {

namespace detail {

/// Widen narrow row offsets into the V2 representation with bounded scratch.
template <typename Offset, typename WriteBytes>
void write_widened_csr_offsets(std::span<const Offset> offsets, WriteBytes&& write_bytes) {
    std::array<uint64_t, 512> buffer;
    while (!offsets.empty()) {
        const size_t count = std::min(offsets.size(), buffer.size());
        std::copy_n(offsets.begin(), count, buffer.begin());
        write_bytes(buffer.data(), count * sizeof(uint64_t));
        offsets = offsets.subspan(count);
    }
}

} // namespace detail

/// Out-of-core CSR matrix backed by memory-mapped files.
///
/// File format v2 (.csrmat):
///   [uint64_t magic]       -- "GNFSCSR2" identifies the v2 64-bit-offset format
///   [uint64_t num_rows]
///   [uint64_t num_cols]
///   [uint64_t nnz]
///   [uint64_t row_offsets[num_rows + 1]]   -- entry offsets within col_indices
///   [uint32_t col_indices[nnz]]            -- column indices, packed contiguously
///
/// The entire file is mmap'd read-only. row_begin/row_end return pointers
/// directly into the mapped region, matching CSRMatrix's interface.
///
/// Row offsets count entries and can exceed UINT32_MAX on a matrix with more
/// than UINT32_MAX nonzeros. V2 stores these offsets in 64 bits and retains
/// 32-bit column indices. The complete image must fit in the host address space.

class MmapCSRMatrix {
    static constexpr uint64_t header_bytes = 4 * sizeof(uint64_t);

    struct FileLayout {
        uint64_t row_count;
        uint64_t column_start;
        uint64_t total_bytes;
    };

    static FileLayout checked_layout(uint64_t rows, uint64_t cols, uint64_t nnz) {
        constexpr uint64_t max_bytes = std::numeric_limits<size_t>::max();
        if (rows > max_bytes || cols > max_bytes || nnz > max_bytes) {
            throw std::overflow_error("MmapCSRMatrix: header field exceeds size_t");
        }
        if (cols > std::numeric_limits<uint32_t>::max()) {
            throw std::invalid_argument("MmapCSRMatrix: column count exceeds uint32_t storage");
        }
        // Include the final row offset and keep the entire image addressable on this host.
        if (rows >= (max_bytes - header_bytes) / sizeof(uint64_t)) {
            throw std::overflow_error("MmapCSRMatrix: row table size exceeds addressable storage");
        }
        const uint64_t row_count = rows + 1;
        const uint64_t column_start = header_bytes + row_count * sizeof(uint64_t);
        if (nnz > (max_bytes - column_start) / sizeof(uint32_t)) {
            throw std::overflow_error("MmapCSRMatrix: column data exceeds addressable storage");
        }
        return {row_count, column_start, column_start + nnz * sizeof(uint32_t)};
    }

public:
    static constexpr uint64_t MAGIC_V2 = 0x324E465343535200ULL; // "GNFSCSR2"

    MmapCSRMatrix() = default;

    /// Save a CSRMatrix to disk in the v2 mmap-friendly format.
    static void save(const CSRMatrix& csr, const std::string& path) {
        const uint64_t num_rows = csr.num_rows();
        const uint64_t num_cols = csr.num_cols();
        const uint64_t nnz = csr.nnz();
        const auto layout = checked_layout(num_rows, num_cols, nnz);

        const size_t zero = 0;
        std::span<const size_t> src_offsets = csr.row_offsets();
        if (num_rows == 0 && nnz == 0 && src_offsets.empty()) {
            src_offsets = {&zero, 1};
        }
        if (src_offsets.size() != layout.row_count) {
            throw std::runtime_error("MmapCSRMatrix::save: row_offsets size inconsistent");
        }

        std::ofstream out(path, std::ios::binary);
        if (!out)
            throw std::runtime_error("MmapCSRMatrix::save: cannot open " + path);

        const auto write_bytes = [&out](const void* source, uint64_t bytes) {
            const char* cursor = static_cast<const char*>(source);
            const uint64_t max_chunk =
                static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max());
            while (bytes != 0) {
                const uint64_t chunk = std::min(bytes, max_chunk);
                out.write(cursor, static_cast<std::streamsize>(chunk));
                if (!out) {
                    throw std::runtime_error("MmapCSRMatrix::save: write failed");
                }
                cursor += static_cast<std::ptrdiff_t>(chunk);
                bytes -= chunk;
            }
        };

        write_bytes(&MAGIC_V2, sizeof(MAGIC_V2));
        write_bytes(&num_rows, sizeof(num_rows));
        write_bytes(&num_cols, sizeof(num_cols));
        write_bytes(&nnz, sizeof(nnz));

        const uint64_t row_bytes = layout.column_start - header_bytes;
        if constexpr (sizeof(size_t) == sizeof(uint64_t)) {
            write_bytes(src_offsets.data(), row_bytes);
        } else {
            detail::write_widened_csr_offsets(src_offsets, write_bytes);
        }

        // col_indices: nnz uint32_t values
        write_bytes(csr.col_indices().data(), nnz * sizeof(uint32_t));

        out.flush();
        if (!out)
            throw std::runtime_error("MmapCSRMatrix::save: flush failed");
    }

    /// Load from a memory-mapped file.
    explicit MmapCSRMatrix(const std::string& path) : file_(path) {
        if (file_.size() < 32) {
            throw std::runtime_error("MmapCSRMatrix: file too small: " + path);
        }

        uint64_t magic = file_.read_at<uint64_t>(0);
        if (magic != MAGIC_V2) {
            throw std::runtime_error("MmapCSRMatrix: invalid magic (expected v2 format) in " +
                                     path);
        }

        const uint64_t raw_rows = file_.read_at<uint64_t>(8);
        const uint64_t raw_cols = file_.read_at<uint64_t>(16);
        const uint64_t raw_nnz = file_.read_at<uint64_t>(24);

        const auto layout = checked_layout(raw_rows, raw_cols, raw_nnz);
        if (layout.total_bytes > file_.size()) {
            throw std::runtime_error("MmapCSRMatrix: file truncated");
        }

        num_rows_ = static_cast<size_t>(raw_rows);
        num_cols_ = static_cast<size_t>(raw_cols);
        nnz_ = static_cast<size_t>(raw_nnz);
        row_offsets_ = file_.ptr_at<uint64_t>(header_bytes);
        col_indices_ = file_.ptr_at<uint32_t>(static_cast<size_t>(layout.column_start));

        // SpMV intentionally omits per-entry bounds checks, so validate the
        // complete CSR contract once while loading untrusted mmap data.
        uint64_t previous = 0;
        for (uint64_t i = 0; i < layout.row_count; ++i) {
            const uint64_t offset = row_offsets_[static_cast<size_t>(i)];
            if (offset > raw_nnz || (i != 0 && offset < previous)) {
                throw std::runtime_error("MmapCSRMatrix: invalid row offsets");
            }
            if (i == 0 && offset != 0) {
                throw std::runtime_error("MmapCSRMatrix: first row offset must be zero");
            }
            previous = offset;
        }
        if (previous != raw_nnz) {
            throw std::runtime_error("MmapCSRMatrix: final row offset does not match nnz");
        }
        for (size_t i = 0; i < nnz_; ++i) {
            if (col_indices_[i] >= num_cols_) {
                throw std::out_of_range("MmapCSRMatrix: column index out of range");
            }
        }

        // For SpMV: sequential access pattern (scan rows in order)
        // madvise is set to SEQUENTIAL by MmapFile constructor
    }

    // Same interface as CSRMatrix for drop-in replacement in SpMV

    [[nodiscard]] size_t num_rows() const noexcept {
        return num_rows_;
    }
    [[nodiscard]] size_t num_cols() const noexcept {
        return num_cols_;
    }
    [[nodiscard]] size_t nnz() const noexcept {
        return nnz_;
    }

    [[nodiscard]] const uint32_t* row_begin(size_t i) const noexcept {
        return column_data() + row_offsets_[i];
    }
    [[nodiscard]] const uint32_t* row_end(size_t i) const noexcept {
        return column_data() + row_offsets_[i + 1];
    }
    [[nodiscard]] size_t row_nnz(size_t i) const noexcept {
        return row_offsets_[i + 1] - row_offsets_[i];
    }

private:
    [[nodiscard]] static const uint32_t* empty_column_data() noexcept {
        static const uint32_t sentinel = 0;
        return &sentinel;
    }

    [[nodiscard]] const uint32_t* column_data() const noexcept {
        // A zero-NNZ mmap has no column payload. Keep the MatrixView pointer
        // range valid for callers that compare or subtract empty row bounds.
        return nnz_ == 0 ? empty_column_data() : col_indices_;
    }

    gnfs::util::MmapFile file_;
    size_t num_rows_ = 0;
    size_t num_cols_ = 0;
    size_t nnz_ = 0;
    const uint64_t* row_offsets_ = nullptr;
    const uint32_t* col_indices_ = nullptr;
};

/// Helper: persist a SparseMatrix to disk as MmapCSRMatrix in one shot.
///
/// Builds the in-memory CSRMatrix (which validates `col < num_cols`),
/// writes it through `MmapCSRMatrix::save`, then opens the file back as
/// an `MmapCSRMatrix`. Used by Phase 5 to flip the SGE-reduced matrix to
/// out-of-core storage before feeding BW.
///
/// The in-memory CSRMatrix is destroyed before opening the mmap, so peak
/// extra RAM during the round-trip is `nnz * 4 bytes` (the CSRMatrix
/// staging copy) and only briefly. After return the only resident bytes
/// are the kernel mmap pagecache, which the OS can evict.
inline MmapCSRMatrix save_sparse_as_mmap(const SparseMatrix& mat, const std::string& path) {
    {
        CSRMatrix csr(mat);
        MmapCSRMatrix::save(csr, path);
    }
    return MmapCSRMatrix(path);
}

} // namespace gnfs::linalg
