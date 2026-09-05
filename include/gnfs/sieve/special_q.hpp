#pragma once

#include "../core/types.hpp"
#include "../factor_base/factor_base.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace gnfs::sieve {

using core::AlgebraicPrime;
using factor_base::FactorBase;

/// Whether an algebraic factor-base entry denotes the projective root.
[[nodiscard]] constexpr bool is_projective_special_q_root(const AlgebraicPrime& prime) noexcept {
    return prime.is_projective();
}

/// Whether an algebraic factor-base entry can define an affine special-q.
///
/// Invalid encodings are rejected explicitly instead of relying on the
/// projective sentinel also comparing greater than p.
[[nodiscard]] constexpr bool is_affine_special_q_root(const AlgebraicPrime& prime) noexcept {
    return prime.p > 1 && !is_projective_special_q_root(prime) && prime.r < prime.p;
}

/// SpecialQ - 特殊Q值
/// 在格筛法中，每个 special-q 定义一个待筛的格
struct SpecialQ {
    uint32_t q;     // 素数 q
    uint32_t r;     // f(r) ≡ 0 (mod q)
    uint32_t index; // 在因子基中的索引

    /// 是否为 projective root
    [[nodiscard]] constexpr bool is_projective() const noexcept {
        return r == AlgebraicPrime::PROJECTIVE_ROOT;
    }

    /// 是否为可用的 affine special-q
    [[nodiscard]] constexpr bool is_affine() const noexcept {
        return q > 1 && !is_projective() && r < q;
    }

    /// 检查是否有效
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return is_affine();
    }
};

/// SpecialQRange - Special-Q 的范围参数
struct SpecialQRange {
    uint32_t min_q = 1'000'000;      // 最小 q 值
    uint32_t max_q = 10'000'000;     // 最大 q 值
    uint32_t start_index = 0;        // 起始索引（用于恢复）
    uint32_t end_index = UINT32_MAX; // 终止索引（UINT32_MAX = 无限制）

    /// 从索引范围创建
    static SpecialQRange from_indices(uint32_t start, uint32_t end) {
        SpecialQRange range;
        range.start_index = start;
        range.end_index = end;
        range.min_q = 0; // 使用索引模式，不限制 q 值
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
        : fb_(fb), range_(range), current_index_(range.start_index) {

        // 找到起始位置
        if (range.min_q > 0) {
            skip_to_min_q();
        }
    }

    /// 是否有更多 special-q
    [[nodiscard]] bool has_next() const noexcept {
        return find_next_affine_index(current_index_).has_value();
    }

    /// 获取下一个 special-q
    [[nodiscard]] std::optional<SpecialQ> next() {
        const auto index = find_next_affine_index(current_index_);
        if (!index) {
            return std::nullopt;
        }

        const auto& ap = fb_.algebraic()[*index];
        current_index_ = *index + 1;
        return SpecialQ{ap.p, ap.r, *index};
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
        size_t remaining = 0;
        auto index = find_next_affine_index(current_index_);
        while (index) {
            ++remaining;
            index = find_next_affine_index(*index + 1);
        }
        return remaining;
    }

private:
    const FactorBase& fb_;
    SpecialQRange range_;
    uint32_t current_index_;

    [[nodiscard]] bool exceeds_max_q(uint32_t q) const noexcept {
        return range_.max_q > 0 && q > range_.max_q;
    }

    /// Find the next usable affine entry without changing generator state.
    [[nodiscard]] std::optional<uint32_t>
    find_next_affine_index(uint32_t start_index) const noexcept {
        const auto& algebraics = fb_.algebraic();
        const size_t upper = std::min(algebraics.size(), static_cast<size_t>(range_.end_index));

        for (size_t index = start_index; index < upper; ++index) {
            const auto& ap = algebraics[index];
            if (exceeds_max_q(ap.p)) {
                break;
            }
            if (ap.p >= range_.min_q && is_affine_special_q_root(ap)) {
                return static_cast<uint32_t>(index);
            }
        }
        return std::nullopt;
    }

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
            if (!sq)
                break;
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
    [[nodiscard]] auto begin() const noexcept {
        return items_.begin();
    }
    [[nodiscard]] auto end() const noexcept {
        return items_.end();
    }

private:
    std::vector<SpecialQ> items_;
};

/// 估计 special-q 范围内的数量
/// @param fb 因子基
/// @param min_q 最小 q
/// @param max_q 最大 q
[[nodiscard]] inline size_t estimate_special_q_count(const FactorBase& fb, uint32_t min_q,
                                                     uint32_t max_q) {

    const auto& algebraics = fb.algebraic();
    size_t count = 0;

    for (const auto& ap : algebraics) {
        if (max_q > 0 && ap.p > max_q) {
            break; // 已排序，可以提前退出
        }
        if (ap.p >= min_q && is_affine_special_q_root(ap)) {
            ++count;
        }
    }

    return count;
}

/// 选择最优 special-q 范围
/// 基于目标关系数量和预期收益
struct SpecialQRangeSelector {
    /// 参数
    uint32_t algebraic_bound; // 代数因子基上界
    size_t target_relations;  // 目标关系数量
    double relations_per_sq;  // 每个 special-q 预期产出

    /// 选择范围
    [[nodiscard]] SpecialQRange select(const FactorBase& /* fb */) const {
        if (algebraic_bound == 0) {
            throw std::invalid_argument("Special-Q algebraic bound must be positive");
        }
        if (!std::isfinite(relations_per_sq) || relations_per_sq <= 0.0) {
            throw std::invalid_argument("Special-Q relations per q must be finite and positive");
        }

        SpecialQRange range;

        // 经验公式：special-q 从因子基上界附近开始
        range.min_q = algebraic_bound;

        // 估计需要多少 special-q
        const double estimated_sq = static_cast<double>(target_relations) / relations_per_sq;
        const double needed_sq = std::max(estimated_sq, 1000.0);

        // 估计 max_q
        // 对于 [min_q, max_q] 区间内的素数数量，使用素数定理估计
        // π(x) ≈ x / ln(x)
        // π(max) - π(min) ≈ needed_sq
        // 简化估计：max_q ≈ min_q + needed_sq * ln(min_q)

        const double ln_min = std::log(static_cast<double>(range.min_q));
        const double delta = needed_sq * ln_min * 1.5;
        const double max_delta = static_cast<double>(UINT32_MAX - range.min_q);

        // Keep the estimate in floating point until it is known to fit. This avoids
        // undefined floating-point-to-integer conversions for very small rates or
        // large relation targets, and the subtraction-based bound avoids uint64_t
        // addition overflow near UINT32_MAX.
        if (!std::isfinite(delta) || delta >= max_delta) {
            range.max_q = UINT32_MAX;
        } else {
            range.max_q = static_cast<uint32_t>(static_cast<uint64_t>(range.min_q) +
                                                static_cast<uint64_t>(delta));
        }

        return range;
    }
};

} // namespace gnfs::sieve
