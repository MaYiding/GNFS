#pragma once

#include "../core/polynomial_context.hpp"
#include "../core/types.hpp"
#include "../util/bit_intrin.hpp"

#include <cmath>
#include <cstddef>
#include <iosfwd>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace gnfs::factor_base {

using core::AlgebraicPrime;
using core::FactorBaseParams;
using core::PolynomialContext;
using core::RationalPrime;

/// FactorBase - 因子基
/// 存储有理侧和代数侧的因子基，供筛法使用
class FactorBase {
public:
    /// 默认构造
    FactorBase() = default;

    /// 从参数构造（实际构建由 FactorBaseBuilder 完成）
    explicit FactorBase(const FactorBaseParams& params) : params_(params) {}

    // 移动语义
    FactorBase(FactorBase&&) = default;
    FactorBase& operator=(FactorBase&&) = default;

    // 禁止拷贝
    FactorBase(const FactorBase&) = delete;
    FactorBase& operator=(const FactorBase&) = delete;

    // ==================== 访问器 ====================

    /// 有理因子基
    [[nodiscard]] std::span<const RationalPrime> rational() const noexcept {
        return rational_;
    }

    /// 代数因子基
    [[nodiscard]] std::span<const AlgebraicPrime> algebraic() const noexcept {
        return algebraic_;
    }

    /// 参数
    [[nodiscard]] const FactorBaseParams& params() const noexcept {
        return params_;
    }

    /// 有理因子基大小
    [[nodiscard]] size_t rational_count() const noexcept {
        return rational_.size();
    }

    /// 代数因子基大小（包括 special-Q 范围的素数）
    [[nodiscard]] size_t algebraic_count() const noexcept {
        return algebraic_.size();
    }

    /// 用于筛选的代数素数数量（≤ algebraic_bound 的部分）
    /// special-Q 范围的素数（> algebraic_bound）不参与筛选
    [[nodiscard]] size_t sieve_algebraic_count() const noexcept {
        return sieve_algebraic_count_ > 0 ? sieve_algebraic_count_ : algebraic_.size();
    }

    /// 设置筛选代数素数计数（由 builder 调用）
    void set_sieve_algebraic_count(size_t count) noexcept {
        sieve_algebraic_count_ = count;
    }

    // ==================== 查找 ====================

    /// 查找有理侧素数的索引
    [[nodiscard]] std::optional<uint32_t> find_rational(uint32_t p) const {
        auto it = rat_index_.find(p);
        if (it != rat_index_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /// 查找代数侧素理想的索引
    [[nodiscard]] std::optional<uint32_t> find_algebraic(uint32_t p, uint32_t r) const {
        uint64_t key = (static_cast<uint64_t>(p) << 32) | r;
        auto it = alg_index_.find(key);
        if (it != alg_index_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /// 查找 special-q 的索引（代数侧）
    [[nodiscard]] std::optional<uint32_t> find_special_q(uint32_t q, uint32_t r) const {
        return find_algebraic(q, r);
    }

    // ==================== 修改（由 Builder 调用） ====================

    /// 添加有理侧素数
    void add_rational(uint32_t p, uint32_t log_p) {
        if (rational_.size() >= max_serialized_count()) {
            throw std::overflow_error(
                "FactorBase::add_rational: rational-prime count exceeds uint32_t");
        }
        uint32_t idx = static_cast<uint32_t>(rational_.size());
        rational_.push_back(RationalPrime{p, log_p});
        rat_index_[p] = idx;
    }

    /// 添加代数侧素理想
    void add_algebraic(uint32_t p, uint32_t r, uint32_t log_p, uint8_t degree = 1) {
        if (algebraic_.size() >= max_serialized_count()) {
            throw std::overflow_error(
                "FactorBase::add_algebraic: algebraic-prime count exceeds uint32_t");
        }
        uint32_t idx = static_cast<uint32_t>(algebraic_.size());
        algebraic_.push_back(AlgebraicPrime{p, r, log_p, degree});
        uint64_t key = (static_cast<uint64_t>(p) << 32) | r;
        alg_index_[key] = idx;
    }

    /// 设置参数
    void set_params(const FactorBaseParams& params) {
        params_ = params;
    }

    /// 预分配空间
    void reserve(size_t rational_size, size_t algebraic_size) {
        if (rational_size > max_serialized_count()) {
            throw std::overflow_error("FactorBase::reserve: rational-prime count exceeds uint32_t");
        }
        if (algebraic_size > max_serialized_count()) {
            throw std::overflow_error(
                "FactorBase::reserve: algebraic-prime count exceeds uint32_t");
        }
        rational_.reserve(rational_size);
        algebraic_.reserve(algebraic_size);
    }

    /// 构建索引表（在所有元素添加后调用）
    void build_index() {
        if (rational_.size() > max_serialized_count()) {
            throw std::overflow_error(
                "FactorBase::build_index: rational-prime count exceeds uint32_t");
        }
        if (algebraic_.size() > max_serialized_count()) {
            throw std::overflow_error(
                "FactorBase::build_index: algebraic-prime count exceeds uint32_t");
        }
        rat_index_.clear();
        alg_index_.clear();
        // Reserve exact sizes to avoid rehashing during build.
        rat_index_.reserve(rational_.size());
        alg_index_.reserve(algebraic_.size());

        for (size_t i = 0; i < rational_.size(); ++i) {
            rat_index_[rational_[i].p] = static_cast<uint32_t>(i);
        }

        for (size_t i = 0; i < algebraic_.size(); ++i) {
            const auto& ap = algebraic_[i];
            uint64_t key = (static_cast<uint64_t>(ap.p) << 32) | ap.r;
            alg_index_[key] = static_cast<uint32_t>(i);
        }
    }

    // ==================== 序列化 ====================

    /// 保存到流
    void save(std::ostream& os) const;

    /// 从流加载
    static FactorBase load(std::istream& is);

    // ==================== 统计 ====================

    /// 获取统计信息
    struct Stats {
        size_t rational_count;
        size_t algebraic_count;
        uint32_t rational_bound;
        uint32_t algebraic_bound;
        uint64_t large_prime_bound;
    };

    [[nodiscard]] Stats stats() const {
        return Stats{rational_.size(), algebraic_.size(), params_.rational_bound,
                     params_.algebraic_bound, params_.large_prime_bound};
    }

private:
    [[nodiscard]] static constexpr size_t max_serialized_count() noexcept {
        return static_cast<size_t>((std::numeric_limits<uint32_t>::max)());
    }

    std::vector<RationalPrime> rational_;
    std::vector<AlgebraicPrime> algebraic_;
    FactorBaseParams params_;
    // 筛选用的代数素数数量（0 = 全部）
    // IMPORTANT: 实现 save()/load() 时必须序列化此字段
    size_t sieve_algebraic_count_ = 0;

    // 快速查找表
    std::unordered_map<uint32_t, uint32_t> rat_index_; // p -> index
    std::unordered_map<uint64_t, uint32_t> alg_index_; // (p << 32 | r) -> index
};

/// 计算对数值（用于筛法的定点数）
[[nodiscard]] inline uint32_t compute_log_prime(uint32_t p, uint8_t scale) {
    // log_p = floor(log2(p) * scale)
    // 使用 clz (count leading zeros) 来快速计算 log2
    if (p <= 1)
        return 0;
    uint32_t log2_p = static_cast<uint32_t>(31 - gnfs::util::clz32(p));
    return log2_p * static_cast<uint32_t>(scale);
}

/// 计算对数值（更精确版本）
[[nodiscard]] inline uint32_t compute_log_prime_precise(uint32_t p, uint8_t scale) {
    if (p <= 1)
        return 0;
    return static_cast<uint32_t>(std::log2(static_cast<double>(p)) * scale);
}

} // namespace gnfs::factor_base
