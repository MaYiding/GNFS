#pragma once

#include "gnfs/linalg/sparse_matrix.hpp"
#include "gnfs/util/mmap_file.hpp"
#include <cassert>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace gnfs::linalg {

/// Out-of-core CSR matrix backed by memory-mapped files.
///
/// File format (.csrmat):
///   [uint64_t magic]
///   [uint64_t num_rows]
///   [uint64_t num_cols]
///   [uint64_t nnz]
///   [uint32_t row_offsets[num_rows + 1]]   -- byte offsets within col_indices
///   [uint32_t col_indices[nnz]]            -- column indices, packed contiguously
///
/// The entire file is mmap'd read-only. row_begin/row_end return pointers
/// directly into the mapped region, matching CSRMatrix's interface.
///
/// For 50+ digit GNFS: matrix can be 100M+ rows, 500M+ nnz (~2GB col_indices).
/// mmap lets the OS manage paging without explicit I/O.

class MmapCSRMatrix {
public:
    static constexpr uint64_t MAGIC = 0x474E465343535200ULL;  // "GNFSCSR\0"

    MmapCSRMatrix() = default;

    /// Save a CSRMatrix to disk in the mmap-friendly format.
    static void save(const CSRMatrix& csr, const std::string& path) {
        std::ofstream out(path, std::ios::binary);
        if (!out) throw std::runtime_error("MmapCSRMatrix::save: cannot open " + path);

        uint64_t magic = MAGIC;
        uint64_t num_rows = csr.num_rows();
        uint64_t num_cols = csr.num_cols();
        uint64_t nnz = csr.nnz();

        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.write(reinterpret_cast<const char*>(&num_rows), 8);
        out.write(reinterpret_cast<const char*>(&num_cols), 8);
        out.write(reinterpret_cast<const char*>(&nnz), 8);

        // row_offsets: num_rows + 1 uint32_t values.
        // CSRMatrix stores row_offsets as std::vector<size_t>; the on-disk
        // format is uint32_t (matches the nnz < 2^32 invariant used elsewhere
        // in the linalg pipeline). Narrow explicitly — reinterpreting a
        // size_t buffer as uint32_t* would truncate every other element.
        const auto& src_offsets = csr.row_offsets();
        if (src_offsets.size() != num_rows + 1) {
            throw std::runtime_error("MmapCSRMatrix::save: row_offsets size inconsistent");
        }
        std::vector<uint32_t> offsets32;
        offsets32.reserve(num_rows + 1);
        for (size_t v : src_offsets) {
            if (v > std::numeric_limits<uint32_t>::max()) {
                throw std::runtime_error(
                    "MmapCSRMatrix::save: row offset exceeds uint32_t range");
            }
            offsets32.push_back(static_cast<uint32_t>(v));
        }
        out.write(reinterpret_cast<const char*>(offsets32.data()),
                  static_cast<std::streamsize>((num_rows + 1) * sizeof(uint32_t)));

        // col_indices: nnz uint32_t values
        out.write(reinterpret_cast<const char*>(csr.col_indices().data()),
                  static_cast<std::streamsize>(nnz * sizeof(uint32_t)));

        out.flush();
    }

    /// Load from a memory-mapped file.
    explicit MmapCSRMatrix(const std::string& path) : file_(path) {
        if (file_.size() < 32) {
            throw std::runtime_error("MmapCSRMatrix: file too small: " + path);
        }

        uint64_t magic = file_.read_at<uint64_t>(0);
        if (magic != MAGIC) {
            throw std::runtime_error("MmapCSRMatrix: invalid magic in " + path);
        }

        num_rows_ = file_.read_at<uint64_t>(8);
        num_cols_ = file_.read_at<uint64_t>(16);
        uint64_t nnz = file_.read_at<uint64_t>(24);

        // Validate file size
        size_t expected = 32 + (num_rows_ + 1) * sizeof(uint32_t) + nnz * sizeof(uint32_t);
        if (file_.size() < expected) {
            throw std::runtime_error("MmapCSRMatrix: file truncated");
        }

        row_offsets_ = file_.ptr_at<uint32_t>(32);
        col_indices_ = file_.ptr_at<uint32_t>(32 + (num_rows_ + 1) * sizeof(uint32_t));
        nnz_ = static_cast<size_t>(nnz);

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
    const uint32_t* row_offsets_ = nullptr;
    const uint32_t* col_indices_ = nullptr;
};

} // namespace gnfs::linalg
