#pragma once

#include "../core/types.hpp"
#include "../factor_base/factor_base.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace gnfs {
namespace sieve {

using core::AlgebraicPrime;
using factor_base::FactorBase;

/// SpecialQ - 特殊Q值
/// 在格筛法中，每个 special-q 定义一个待筛的格
struct SpecialQ {
    uint32_t q;         // 素数 q
    uint32_t r;         // f(r) ≡ 0 (mod q)
    uint32_t index;     // 在因子基中的索引

    /// 检查是否有效
    [[nodiscard]] bool is_valid() const noexcept {
        return q > 1;
    }
};

/// SpecialQRange - Special-Q 的范围参数
struct SpecialQRange {
    uint32_t min_q = 1'000'000;     // 最小 q 值
    uint32_t max_q = 10'000'000;    // 最大 q 值
    uint32_t start_index = 0;       // 起始索引（用于恢复）
    uint32_t end_index = UINT32_MAX; // 终止索引（UINT32_MAX = 无限制）

    /// 从索引范围创建
    static SpecialQRange from_indices(uint32_t start, uint32_t end) {
        SpecialQRange range;
        range.start_index = start;
        range.end_index = end;
        range.min_q = 0;  // 使用索引模式，不限制 q 值
        range.max_q = UINT32_MAX;
        return range;
    }
};

/// SpecialQGenerator - Special-Q 生成器
/// 按顺序生成用于格筛的 special-q 值
class SpecialQGenerator {
public:
    /// 从因子基构造
    /// @param fb 因子基
    /// @param range Q 值范围
    explicit SpecialQGenerator(const FactorBase& fb, const SpecialQRange& range = SpecialQRange{})
        : fb_(fb)
        , range_(range)
        , current_index_(range.start_index) {

        // 找到起始位置
        if (range.min_q > 0) {
            skip_to_min_q();
        }
    }

    /// 是否有更多 special-q
    [[nodiscard]] bool has_next() const noexcept {
        if (current_index_ >= fb_.algebraic_count()) {
            return false;
        }
        if (current_index_ >= range_.end_index) {
            return false;
        }
        // 检查当前素数是否超出 max_q
        if (range_.max_q > 0 && fb_.algebraic()[current_index_].p > range_.max_q) {
            return false;
        }
        return true;
    }

    /// 获取下一个 special-q
    [[nodiscard]] std::optional<SpecialQ> next() {
        while (current_index_ < fb_.algebraic_count()) {
            if (current_index_ >= range_.end_index) {
                return std::nullopt;  // 超出索引范围
            }

            const auto& ap = fb_.algebraic()[current_index_];

            // 检查是否在范围内
            if (ap.p > range_.max_q) {
                return std::nullopt;  // 超出 q 值范围
            }

            if (ap.p >= range_.min_q) {
                SpecialQ sq;
                sq.q = ap.p;
                sq.r = ap.r;
                sq.index = current_index_;
                ++current_index_;
                return sq;
            }

            ++current_index_;
        }
        return std::nullopt;
    }

    /// 获取当前索引
    [[nodiscard]] uint32_t current_index() const noexcept {
        return current_index_;
    }

    /// 重置到指定索引
    void reset_to(uint32_t index) {
        current_index_ = index;
    }

    /// 估计剩余数量
    [[nodiscard]] size_t estimate_remaining() const {
        uint32_t upper = std::min(static_cast<uint32_t>(fb_.algebraic_count()), range_.end_index);
        if (current_index_ >= upper) {
            return 0;
        }
        return upper - current_index_;
    }

private:
    const FactorBase& fb_;
    SpecialQRange range_;
    uint32_t current_index_;

    /// 跳过到 min_q
    void skip_to_min_q() {
        const auto& algebraics = fb_.algebraic();
        while (current_index_ < algebraics.size()) {
            if (algebraics[current_index_].p >= range_.min_q) {
                break;
            }
            ++current_index_;
        }
    }
};

/// SpecialQBatch - 一批 Special-Q
/// 用于并行处理
class SpecialQBatch {
public:
    /// 构造空批次
    SpecialQBatch() = default;

    /// 从生成器获取一批
    /// @param gen 生成器
    /// @param count 最多获取多少个
    static SpecialQBatch fetch(SpecialQGenerator& gen, size_t count) {
        SpecialQBatch batch;
        batch.items_.reserve(count);

        while (batch.items_.size() < count) {
            auto sq = gen.next();
            if (!sq) break;
            batch.items_.push_back(*sq);
        }

        return batch;
    }

    /// 批次大小
    [[nodiscard]] size_t size() const noexcept {
        return items_.size();
    }

    /// 是否为空
    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    /// 访问器
    [[nodiscard]] const std::vector<SpecialQ>& items() const noexcept {
        return items_;
    }

    /// 索引访问
    [[nodiscard]] const SpecialQ& operator[](size_t i) const {
        return items_[i];
    }

    /// 迭代器
    [[nodiscard]] auto begin() const noexcept { return items_.begin(); }
    [[nodiscard]] auto end() const noexcept { return items_.end(); }

private:
    std::vector<SpecialQ> items_;
};

/// 估计 special-q 范围内的数量
/// @param fb 因子基
/// @param min_q 最小 q
/// @param max_q 最大 q
[[nodiscard]] inline size_t estimate_special_q_count(
        const FactorBase& fb,
        uint32_t min_q,
        uint32_t max_q) {

    const auto& algebraics = fb.algebraic();
    size_t count = 0;

    for (const auto& ap : algebraics) {
        if (ap.p >= min_q && ap.p <= max_q) {
            ++count;
        } else if (ap.p > max_q) {
            break;  // 已排序，可以提前退出
        }
    }

    return count;
}

/// 选择最优 special-q 范围
/// 基于目标关系数量和预期收益
struct SpecialQRangeSelector {
    /// 参数
    uint32_t algebraic_bound;        // 代数因子基上界
    size_t target_relations;         // 目标关系数量
    double relations_per_sq;         // 每个 special-q 预期产出

    /// 选择范围
    [[nodiscard]] SpecialQRange select(const FactorBase& /* fb */) const {
        SpecialQRange range;

        // 经验公式：special-q 从因子基上界附近开始
        range.min_q = algebraic_bound;

        // 估计需要多少 special-q
        size_t needed_sq = static_cast<size_t>(target_relations / relations_per_sq);
        needed_sq = std::max(needed_sq, size_t(1000));

        // 估计 max_q
        // 对于 [min_q, max_q] 区间内的素数数量，使用素数定理估计
        // π(x) ≈ x / ln(x)
        // π(max) - π(min) ≈ needed_sq
        // 简化估计：max_q ≈ min_q + needed_sq * ln(min_q)

        double ln_min = std::log(static_cast<double>(range.min_q));
        uint64_t delta = static_cast<uint64_t>(needed_sq * ln_min * 1.5);

        range.max_q = static_cast<uint32_t>(
            std::min(static_cast<uint64_t>(range.min_q) + delta,
                     static_cast<uint64_t>(UINT32_MAX)));

        return range;
    }
};

} // namespace sieve
} // namespace gnfs
