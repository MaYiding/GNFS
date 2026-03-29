#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace gnfs {
namespace linalg {

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

    /// 设置位（添加列索引）- O(1) amortized using lazy sorting
    void set(uint32_t col) {
        if (sorted_) {
            // If sorted, use binary search to check for duplicate
            auto it = std::lower_bound(indices_.begin(), indices_.end(), col);
            if (it != indices_.end() && *it == col) {
                return;  // Already set
            }
            // Mark as unsorted and append
            sorted_ = false;
        }
        indices_.push_back(col);
    }

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
    void ensure_sorted() {
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
        const_cast<SparseRow&>(other).ensure_sorted();

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
        const_cast<SparseRow*>(this)->ensure_sorted();
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
        const_cast<SparseRow*>(this)->ensure_sorted();
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
    IndexList indices_;  // 非零列索引
    bool sorted_ = true; // 是否已排序
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
        return static_cast<double>(total_weight()) / rows_.size();
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
            count += __builtin_popcountll(block);
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

/// 64 位块矩阵，用于 Block Lanczos
/// 每个"元素"是一个 64 位块
class BlockMatrix {
public:
    BlockMatrix() = default;

    BlockMatrix(size_t num_rows, size_t num_block_cols)
        : data_(num_rows * num_block_cols, 0)
        , num_rows_(num_rows)
        , num_block_cols_(num_block_cols) {}

    /// 访问块
    [[nodiscard]] uint64_t& block(size_t row, size_t col) {
        return data_[row * num_block_cols_ + col];
    }

    [[nodiscard]] uint64_t block(size_t row, size_t col) const {
        return data_[row * num_block_cols_ + col];
    }

    /// 获取行数
    [[nodiscard]] size_t num_rows() const noexcept {
        return num_rows_;
    }

    /// 获取块列数
    [[nodiscard]] size_t num_block_cols() const noexcept {
        return num_block_cols_;
    }

    /// 清空
    void clear() {
        std::fill(data_.begin(), data_.end(), 0);
    }

    /// 获取底层数据
    [[nodiscard]] const std::vector<uint64_t>& data() const noexcept {
        return data_;
    }

    [[nodiscard]] std::vector<uint64_t>& data() noexcept {
        return data_;
    }

private:
    std::vector<uint64_t> data_;
    size_t num_rows_ = 0;
    size_t num_block_cols_ = 0;
};

} // namespace linalg
} // namespace gnfs
