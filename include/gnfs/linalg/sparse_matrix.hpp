#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "../util/bit_intrin.hpp"

namespace gnfs::linalg {

/// 稀疏行 - 存储非零列索引（用于 GF(2) 矩阵）
/// 在 GF(2) 中，非零元素都是 1，所以只需存储列索引
class SparseRow {
public:
    using IndexList = std::vector<uint32_t>;

    SparseRow() = default;

    /// 从列索引列表构造
    explicit SparseRow(IndexList indices) : indices_(std::move(indices)) {
        // 确保排序
        std::sort(indices_.begin(), indices_.end());
        sorted_ = true;
    }

    /// 设置位（添加列索引）- 幂等：多次 set(col) 等效于一次
    void set(uint32_t col) {
        if (sorted_) {
            auto it = std::lower_bound(indices_.begin(), indices_.end(), col);
            if (it != indices_.end() && *it == col) {
                return;  // Already set
            }
            sorted_ = false;
        } else {
            // Unsorted: linear scan to prevent duplicate append (preserves set idempotency)
            for (auto idx : indices_) {
                if (idx == col) return;  // Already present
            }
        }
        indices_.push_back(col);
    }

    /// 快速 append:**调用方必须保证 col 在此行中尚未出现**。
    /// 用于 build_row 中按因子顺序累加 — 已知不重复时跳过 O(n) 检查。
    /// 调用结束后必须显式 `ensure_sorted()` 来去重并恢复排序不变量。
    void append_unchecked(uint32_t col) {
        indices_.push_back(col);
        sorted_ = false;
    }

    /// Reserve capacity for upcoming append_unchecked/set calls.
    /// Called by matrix_builder build_row with FB+LP+QC+SM count upper bound.
    void reserve(size_t n) { indices_.reserve(n); }

    /// 清除位
    void clear(uint32_t col) {
        ensure_sorted();
        auto it = std::lower_bound(indices_.begin(), indices_.end(), col);
        if (it != indices_.end() && *it == col) {
            indices_.erase(it);
        }
    }

    /// 翻转位（GF(2) 加法）
    void flip(uint32_t col) {
        ensure_sorted();
        auto it = std::lower_bound(indices_.begin(), indices_.end(), col);
        if (it != indices_.end() && *it == col) {
            indices_.erase(it);
        } else {
            indices_.insert(it, col);
        }
    }

    /// 测试位（const-safe: 排序时用二分查找，未排序时用线性扫描）
    [[nodiscard]] bool test(uint32_t col) const {
        if (sorted_) {
            auto it = std::lower_bound(indices_.begin(), indices_.end(), col);
            return it != indices_.end() && *it == col;
        }
        // 未排序：线性扫描，计算出现次数 mod 2（GF(2) 语义）
        size_t count = 0;
        for (auto idx : indices_) {
            if (idx == col) ++count;
        }
        return count % 2 == 1;
    }

    /// Ensure indices are sorted and deduplicated (call before operations that need sorted data)
    /// This is logically const — sorting doesn't change GF(2) value, only internal representation.
    void ensure_sorted() const {
        if (!sorted_) {
            std::sort(indices_.begin(), indices_.end());
            // Remove duplicates (GF(2): even count = 0)
            IndexList unique;
            unique.reserve(indices_.size());
            for (size_t i = 0; i < indices_.size(); ) {
                uint32_t val = indices_[i];
                size_t count = 1;
                while (i + count < indices_.size() && indices_[i + count] == val) {
                    count++;
                }
                if (count % 2 == 1) {  // Odd count = 1 in GF(2)
                    unique.push_back(val);
                }
                i += count;
            }
            indices_ = std::move(unique);
            sorted_ = true;
        }
    }

    /// 行异或（GF(2) 加法）
    /// this = this XOR other
    void xor_with(const SparseRow& other) {
        ensure_sorted();
        other.ensure_sorted();

        IndexList result;
        result.reserve(indices_.size() + other.indices_.size());

        auto it1 = indices_.begin();
        auto it2 = other.indices_.begin();

        while (it1 != indices_.end() && it2 != other.indices_.end()) {
            if (*it1 < *it2) {
                result.push_back(*it1);
                ++it1;
            } else if (*it2 < *it1) {
                result.push_back(*it2);
                ++it2;
            } else {
                // 相等 - XOR 得 0，跳过
                ++it1;
                ++it2;
            }
        }

        // 复制剩余元素
        while (it1 != indices_.end()) {
            result.push_back(*it1);
            ++it1;
        }
        while (it2 != other.indices_.end()) {
            result.push_back(*it2);
            ++it2;
        }

        indices_ = std::move(result);
    }

    /// 是否为空行（全零）
    [[nodiscard]] bool empty() const noexcept {
        return indices_.empty();
    }

    /// 非零元素数量（行的汉明重量）
    [[nodiscard]] size_t weight() const {
        ensure_sorted();
        return indices_.size();
    }

    /// 获取最低非零列索引
    [[nodiscard]] uint32_t first_nonzero() const noexcept {
        return indices_.empty() ? UINT32_MAX : indices_.front();
    }

    /// 获取最高非零列索引
    [[nodiscard]] uint32_t last_nonzero() const noexcept {
        return indices_.empty() ? UINT32_MAX : indices_.back();
    }

    /// 获取所有非零列索引 (ensures sorted)
    [[nodiscard]] const IndexList& indices() const {
        ensure_sorted();
        return indices_;
    }

    /// 清空行
    void clear_all() {
        indices_.clear();
        sorted_ = true;
    }

    /// 交换
    void swap(SparseRow& other) noexcept {
        indices_.swap(other.indices_);
        std::swap(sorted_, other.sorted_);
    }

private:
    mutable IndexList indices_;  // 非零列索引 (mutable for lazy normalization)
    mutable bool sorted_ = true; // 是否已排序
};

/// 稀疏二进制矩阵（GF(2) 上）
/// 按行存储，每行是一个 SparseRow
class SparseMatrix {
public:
    SparseMatrix() = default;

    /// 构造指定大小的矩阵
    SparseMatrix(size_t num_rows, size_t num_cols)
        : rows_(num_rows), num_cols_(num_cols) {}

    /// 获取行数
    [[nodiscard]] size_t num_rows() const noexcept {
        return rows_.size();
    }

    /// 获取列数
    [[nodiscard]] size_t num_cols() const noexcept {
        return num_cols_;
    }

    /// 访问行
    [[nodiscard]] SparseRow& row(size_t i) {
        return rows_[i];
    }

    [[nodiscard]] const SparseRow& row(size_t i) const {
        return rows_[i];
    }

    /// 设置元素
    void set(size_t row_idx, size_t col_idx) {
        rows_[row_idx].set(static_cast<uint32_t>(col_idx));
    }

    /// 清除元素
    void clear(size_t row_idx, size_t col_idx) {
        rows_[row_idx].clear(static_cast<uint32_t>(col_idx));
    }

    /// 测试元素
    [[nodiscard]] bool test(size_t row_idx, size_t col_idx) const {
        return rows_[row_idx].test(static_cast<uint32_t>(col_idx));
    }

    /// 交换两行
    void swap_rows(size_t i, size_t j) {
        rows_[i].swap(rows_[j]);
    }

    /// 行 i = 行 i XOR 行 j
    void xor_rows(size_t i, size_t j) {
        rows_[i].xor_with(rows_[j]);
    }

    /// 添加新行
    void add_row(SparseRow row) {
        rows_.push_back(std::move(row));
    }

    /// 调整行数
    void resize_rows(size_t new_num_rows) {
        rows_.resize(new_num_rows);
    }

    /// 设置列数
    void set_num_cols(size_t cols) {
        num_cols_ = cols;
    }

    /// 计算非零元素总数
    [[nodiscard]] size_t total_weight() const noexcept {
        size_t total = 0;
        for (const auto& row : rows_) {
            total += row.weight();
        }
        return total;
    }

    /// 计算平均行重量
    [[nodiscard]] double average_row_weight() const noexcept {
        if (rows_.empty()) return 0.0;
        return static_cast<double>(total_weight()) / static_cast<double>(rows_.size());
    }

    /// Ensure all rows are sorted and deduplicated.
    /// Must be called before concurrent read access (e.g., parallel SpMV).
    void ensure_all_sorted() {
        for (auto& row : rows_) {
            row.ensure_sorted();
        }
    }

    /// 获取所有行（用于迭代）
    [[nodiscard]] const std::vector<SparseRow>& rows() const noexcept {
        return rows_;
    }

    [[nodiscard]] std::vector<SparseRow>& rows() noexcept {
        return rows_;
    }

    /// 创建转置矩阵
    [[nodiscard]] SparseMatrix transpose() const {
        SparseMatrix result(num_cols_, rows_.size());

        for (size_t i = 0; i < rows_.size(); ++i) {
            for (uint32_t col : rows_[i].indices()) {
                result.set(col, i);
            }
        }

        return result;
    }

    /// 矩阵-向量乘法 (GF(2))
    /// result = A * x
    /// 其中 x 是一个比特向量（用 vector<bool> 表示）
    [[nodiscard]] std::vector<bool> multiply(const std::vector<bool>& x) const {
        std::vector<bool> result(rows_.size(), false);

        for (size_t i = 0; i < rows_.size(); ++i) {
            bool sum = false;
            for (uint32_t col : rows_[i].indices()) {
                if (col < x.size() && x[col]) {
                    sum = !sum;  // XOR
                }
            }
            result[i] = sum;
        }

        return result;
    }

private:
    std::vector<SparseRow> rows_;
    size_t num_cols_ = 0;
};

/// 密集比特向量（用于表示解向量）
class BitVector {
public:
    BitVector() = default;

    explicit BitVector(size_t size) : bits_((size + 63) / 64, 0), size_(size) {}

    /// 设置位
    void set(size_t idx) {
        bits_[idx / 64] |= (1ULL << (idx % 64));
    }

    /// 清除位
    void clear(size_t idx) {
        bits_[idx / 64] &= ~(1ULL << (idx % 64));
    }

    /// 翻转位
    void flip(size_t idx) {
        bits_[idx / 64] ^= (1ULL << (idx % 64));
    }

    /// 测试位
    [[nodiscard]] bool test(size_t idx) const noexcept {
        return (bits_[idx / 64] >> (idx % 64)) & 1;
    }

    /// XOR 操作 (取两者长度的较小值，防止越界)
    void xor_with(const BitVector& other) {
        size_t len = std::min(bits_.size(), other.bits_.size());
        for (size_t i = 0; i < len; ++i) {
            bits_[i] ^= other.bits_[i];
        }
    }

    /// 是否全零
    [[nodiscard]] bool is_zero() const noexcept {
        for (uint64_t block : bits_) {
            if (block != 0) return false;
        }
        return true;
    }

    /// 汉明重量
    [[nodiscard]] size_t popcount() const noexcept {
        size_t count = 0;
        for (uint64_t block : bits_) {
            count += static_cast<size_t>(gnfs::util::popcount64(block));
        }
        return count;
    }

    /// 大小
    [[nodiscard]] size_t size() const noexcept {
        return size_;
    }

    /// 获取底层数据
    [[nodiscard]] const std::vector<uint64_t>& data() const noexcept {
        return bits_;
    }

    [[nodiscard]] std::vector<uint64_t>& data() noexcept {
        return bits_;
    }

    /// 获取所有设置位的索引
    [[nodiscard]] std::vector<size_t> set_bits() const {
        std::vector<size_t> result;
        // popcount preflight gives exact result count → no reallocation.
        result.reserve(popcount());
        for (size_t i = 0; i < size_; ++i) {
            if (test(i)) {
                result.push_back(i);
            }
        }
        return result;
    }

    /// 清空
    void clear_all() {
        std::fill(bits_.begin(), bits_.end(), 0);
    }

private:
    std::vector<uint64_t> bits_;
    size_t size_ = 0;
};

/// CSR (Compressed Sparse Row) read-only view of a SparseMatrix.
/// Packs all row indices into a single contiguous array for optimal SpMV performance.
/// Construct from a SparseMatrix once, then use for repeated SpMV.
class CSRMatrix {
public:
    CSRMatrix() = default;

    /// Build CSR from SparseMatrix (ensures all rows are sorted first).
    /// Validates col < num_cols at construction so SpMV hot loops can elide
    /// per-element bounds checks.
    explicit CSRMatrix(const SparseMatrix& mat) {
        num_rows_ = mat.num_rows();
        num_cols_ = mat.num_cols();

        // Compute row offsets
        row_offsets_.resize(num_rows_ + 1);
        row_offsets_[0] = 0;
        size_t total_nnz = 0;
        for (size_t i = 0; i < num_rows_; ++i) {
            total_nnz += mat.row(i).indices().size();
            row_offsets_[i + 1] = total_nnz;
        }

        // Pack all column indices into one contiguous array, validating bounds.
        col_indices_.resize(total_nnz);
        size_t pos = 0;
        for (size_t i = 0; i < num_rows_; ++i) {
            const auto& idx = mat.row(i).indices();
            for (uint32_t c : idx) {
                if (c >= num_cols_) {
                    throw std::out_of_range(
                        "CSRMatrix: column index out of range");
                }
            }
            std::copy(idx.begin(), idx.end(), col_indices_.begin() + static_cast<ptrdiff_t>(pos));
            pos += idx.size();
        }
    }

    [[nodiscard]] size_t num_rows() const noexcept { return num_rows_; }
    [[nodiscard]] size_t num_cols() const noexcept { return num_cols_; }
    [[nodiscard]] size_t nnz() const noexcept { return col_indices_.size(); }

    /// Get column indices for row i as a contiguous span
    [[nodiscard]] const uint32_t* row_begin(size_t i) const noexcept {
        return col_indices_.data() + row_offsets_[i];
    }
    [[nodiscard]] const uint32_t* row_end(size_t i) const noexcept {
        return col_indices_.data() + row_offsets_[i + 1];
    }
    [[nodiscard]] size_t row_nnz(size_t i) const noexcept {
        return row_offsets_[i + 1] - row_offsets_[i];
    }

    /// Access underlying data for direct iteration
    [[nodiscard]] const std::vector<uint32_t>& col_indices() const noexcept { return col_indices_; }
    [[nodiscard]] const std::vector<size_t>& row_offsets() const noexcept { return row_offsets_; }

    /// uint32 view of row_offsets, lazily materialised on first request.
    /// Exists so the Metal SpMV layer can hand the CSR straight to the
    /// GPU without an extra runtime conversion: Metal kernels work in
    /// uint32 and `row_offsets_` is stored as size_t for legacy CPU
    /// kernels. Throws on overflow; in practice GNFS-scale matrices keep
    /// total nnz under 2^32 (60d ≈ 500M nnz at most). Returned pointer
    /// stays valid for the lifetime of the CSRMatrix.
    [[nodiscard]] const uint32_t* row_offsets_u32() const {
        if (row_offsets_u32_.empty() && !row_offsets_.empty()) {
            row_offsets_u32_.resize(row_offsets_.size());
            for (size_t i = 0; i < row_offsets_.size(); ++i) {
                if (row_offsets_[i] > UINT32_MAX) {
                    row_offsets_u32_.clear();
                    throw std::overflow_error(
                        "CSRMatrix::row_offsets_u32: nnz exceeds 2^32");
                }
                row_offsets_u32_[i] = static_cast<uint32_t>(row_offsets_[i]);
            }
        }
        return row_offsets_u32_.data();
    }

private:
    std::vector<uint32_t> col_indices_;   // All column indices, packed contiguously
    std::vector<size_t> row_offsets_;   // row_offsets_[i] = start of row i in col_indices_
    mutable std::vector<uint32_t> row_offsets_u32_;  // Lazy uint32 view for Metal
    size_t num_rows_ = 0;
    size_t num_cols_ = 0;
};

} // namespace gnfs::linalg
