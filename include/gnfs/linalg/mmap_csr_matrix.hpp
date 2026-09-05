#pragma once

#include "gnfs/linalg/sparse_matrix.hpp"
#include "gnfs/util/mmap_file.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace gnfs::linalg {

/// Out-of-core CSR matrix backed by memory-mapped files.
///
/// File format v2 (.csrmat):
///   [uint64_t magic]       -- "GNFSCSR2" identifies the v2 64-bit-offset format
///   [uint64_t num_rows]
///   [uint64_t num_cols]
///   [uint64_t nnz]
///   [uint64_t row_offsets[num_rows + 1]]   -- byte offsets within col_indices
///   [uint32_t col_indices[nnz]]            -- column indices, packed contiguously
///
/// The entire file is mmap'd read-only. row_begin/row_end return pointers
/// directly into the mapped region, matching CSRMatrix's interface.
///
/// For 50+ digit GNFS: matrix can be 100M+ rows, 500M+ nnz (~2GB col_indices).
/// row_offsets cannot fit in uint32_t (500M > 2^32 / 4 = 1G entries lower bound,
/// and offsets are cumulative). v1's uint32 row_offsets contradicted the design
/// target — save() threw "row offset exceeds uint32_t range" before any 50d+
/// matrix could be written. v2 uses uint64 throughout for row_offsets while
/// keeping col_indices uint32 (column count is still bounded by uint32::max
/// in the linalg layer; see SparseRow uint32_t index type).

class MmapCSRMatrix {
public:
    static constexpr uint64_t MAGIC_V2 = 0x324E465343535200ULL;  // "GNFSCSR2"

    MmapCSRMatrix() = default;

    /// Save a CSRMatrix to disk in the v2 mmap-friendly format.
    static void save(const CSRMatrix& csr, const std::string& path) {
        std::ofstream out(path, std::ios::binary);
        if (!out) throw std::runtime_error("MmapCSRMatrix::save: cannot open " + path);

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

        uint64_t magic = MAGIC_V2;
        uint64_t num_rows = csr.num_rows();
        uint64_t num_cols = csr.num_cols();
        uint64_t nnz = csr.nnz();

        write_bytes(&magic, sizeof(magic));
        write_bytes(&num_rows, sizeof(num_rows));
        write_bytes(&num_cols, sizeof(num_cols));
        write_bytes(&nnz, sizeof(nnz));

        // row_offsets: num_rows + 1 uint64_t values (was uint32_t in v1)
        const auto& src_offsets = csr.row_offsets();
        if (num_rows == std::numeric_limits<uint64_t>::max()) {
            throw std::overflow_error("MmapCSRMatrix::save: row count overflows row table");
        }
        const uint64_t row_count = num_rows + 1;
        if (row_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
            src_offsets.size() != static_cast<size_t>(row_count)) {
            throw std::runtime_error("MmapCSRMatrix::save: row_offsets size inconsistent");
        }
        if (src_offsets.size() > std::numeric_limits<uint64_t>::max() / sizeof(uint64_t)) {
            throw std::length_error("MmapCSRMatrix::save: row offset table is too large");
        }
        const uint64_t row_bytes = static_cast<uint64_t>(src_offsets.size()) * sizeof(uint64_t);
        if constexpr (sizeof(size_t) == sizeof(uint64_t)) {
            write_bytes(src_offsets.data(), row_bytes);
        } else {
            // The on-disk format is explicitly uint64_t even on 32-bit hosts.
            // Convert in bounded chunks instead of assuming size_t has the same width.
            std::vector<uint64_t> offsets(src_offsets.size());
            for (size_t i = 0; i < src_offsets.size(); ++i) {
                offsets[i] = static_cast<uint64_t>(src_offsets[i]);
            }
            write_bytes(offsets.data(), row_bytes);
        }

        // col_indices: nnz uint32_t values
        if (nnz > std::numeric_limits<uint64_t>::max() / sizeof(uint32_t)) {
            throw std::length_error("MmapCSRMatrix::save: column index data is too large");
        }
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

        const uint64_t max_size_t = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
        if (raw_rows > max_size_t || raw_cols > max_size_t || raw_nnz > max_size_t) {
            throw std::overflow_error("MmapCSRMatrix: header field exceeds size_t");
        }
        if (raw_cols > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
            throw std::invalid_argument("MmapCSRMatrix: column count exceeds uint32_t storage");
        }
        if (raw_rows == std::numeric_limits<uint64_t>::max()) {
            throw std::overflow_error("MmapCSRMatrix: row count overflows row table");
        }

        const uint64_t row_count = raw_rows + 1;
        if (row_count > max_size_t) {
            throw std::overflow_error("MmapCSRMatrix: row table exceeds size_t indexing");
        }
        constexpr uint64_t header_bytes = 4 * sizeof(uint64_t);
        if (row_count > (std::numeric_limits<uint64_t>::max() - header_bytes) / sizeof(uint64_t)) {
            throw std::overflow_error("MmapCSRMatrix: row table size overflows uint64_t");
        }
        const uint64_t row_bytes = row_count * sizeof(uint64_t);
        const uint64_t row_and_header = header_bytes + row_bytes;
        if (raw_nnz > (std::numeric_limits<uint64_t>::max() - row_and_header) / sizeof(uint32_t)) {
            throw std::overflow_error("MmapCSRMatrix: column data size overflows uint64_t");
        }
        const uint64_t expected = row_and_header + raw_nnz * sizeof(uint32_t);

        // Validate file size
        if (expected > static_cast<uint64_t>(file_.size())) {
            throw std::runtime_error("MmapCSRMatrix: file truncated");
        }

        num_rows_ = static_cast<size_t>(raw_rows);
        num_cols_ = static_cast<size_t>(raw_cols);
        nnz_ = static_cast<size_t>(raw_nnz);
        row_offsets_ = file_.ptr_at<uint64_t>(32);
        col_indices_ = file_.ptr_at<uint32_t>(static_cast<size_t>(row_and_header));

        // SpMV intentionally omits per-entry bounds checks, so validate the
        // complete CSR contract once while loading untrusted mmap data.
        uint64_t previous = 0;
        for (uint64_t i = 0; i < row_count; ++i) {
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

    [[nodiscard]] size_t num_rows() const noexcept { return num_rows_; }
    [[nodiscard]] size_t num_cols() const noexcept { return num_cols_; }
    [[nodiscard]] size_t nnz() const noexcept { return nnz_; }

    [[nodiscard]] const uint32_t* row_begin(size_t i) const noexcept {
        return col_indices_ + row_offsets_[i];
    }
    [[nodiscard]] const uint32_t* row_end(size_t i) const noexcept {
        return col_indices_ + row_offsets_[i + 1];
    }
    [[nodiscard]] size_t row_nnz(size_t i) const noexcept {
        return row_offsets_[i + 1] - row_offsets_[i];
    }

private:
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
inline MmapCSRMatrix save_sparse_as_mmap(const SparseMatrix& mat,
                                         const std::string& path) {
    {
        CSRMatrix csr(mat);
        MmapCSRMatrix::save(csr, path);
    }
    return MmapCSRMatrix(path);
}

} // namespace gnfs::linalg
