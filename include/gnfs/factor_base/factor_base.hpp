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
        return sieve_algebraic_count_set_ ? sieve_algebraic_count_ : algebraic_.size();
    }

    /// Whether the sieve count was explicitly set (including an explicit zero).
    [[nodiscard]] bool has_explicit_sieve_algebraic_count() const noexcept {
        return sieve_algebraic_count_set_;
    }

    /// 设置筛选代数素数计数（由 builder 调用）。
    /// 保留历史兼容语义：count == 0 表示未设置，筛选全部代数条目。
    void set_sieve_algebraic_count(size_t count) noexcept {
        sieve_algebraic_count_ = count;
        sieve_algebraic_count_set_ = count != 0;
    }

    /// Set an exact sieve prefix, including an explicit zero-length prefix.
    /// This is used when a factor base contains only special-Q entries.
    void set_sieve_algebraic_count_explicit(size_t count) noexcept {
        sieve_algebraic_count_ = count;
        sieve_algebraic_count_set_ = true;
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
        append_indexed_prime(rational_, rat_index_, p, RationalPrime{p, log_p});
    }

    /// 添加代数侧素理想
    void add_algebraic(uint32_t p, uint32_t r, uint32_t log_p, uint8_t degree = 1) {
        if (algebraic_.size() >= max_serialized_count()) {
            throw std::overflow_error(
                "FactorBase::add_algebraic: algebraic-prime count exceeds uint32_t");
        }
        const uint64_t key = (static_cast<uint64_t>(p) << 32) | r;
        append_indexed_prime(algebraic_, alg_index_, key, AlgebraicPrime{p, r, log_p, degree});
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
        auto rational = replacement_index(rational_, rat_index_,
                                          [](const RationalPrime& prime) { return prime.p; });
        auto algebraic = replacement_index(algebraic_, alg_index_, [](const AlgebraicPrime& prime) {
            return (static_cast<uint64_t>(prime.p) << 32) | prime.r;
        });
        // Publish only after both sides have completed every allocation.
        if (rational)
            rat_index_.swap(*rational);
        if (algebraic)
            alg_index_.swap(*algebraic);
    }

    // ==================== 序列化 ====================

    /// 保存到流（版本 1 的固定宽度字段使用 little-endian 编码）
    void save(std::ostream& os) const;

    /// 从流加载；同时兼容旧版本 1 的本机字节序文件。
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
    template <typename Entries, typename Index, typename KeyOf>
    static std::optional<Index> replacement_index(const Entries& entries, const Index& index,
                                                  KeyOf key_of) {
        const auto is_current = [&] {
            for (const auto& [key, column] : index) {
                if (column >= entries.size() || key_of(entries[column]) != key)
                    return false;
            }
            for (size_t i = 0; i < entries.size(); ++i) {
                const auto it = index.find(key_of(entries[i]));
                // For repeated keys the mapped column must be the last one.
                if (it == index.end() || it->second < i)
                    return false;
            }
            return true;
        };
        if (is_current())
            return std::nullopt;

        Index replacement;
        replacement.reserve(entries.size());
        for (size_t i = 0; i < entries.size(); ++i)
            replacement[key_of(entries[i])] = static_cast<uint32_t>(i);
        return replacement;
    }

    template <typename Prime, typename Key>
    static void append_indexed_prime(std::vector<Prime>& primes,
                                     std::unordered_map<Key, uint32_t>& index, Key key,
                                     Prime prime) {
        // Stage a new key before growing the vector so a failed add also keeps
        // existing spans valid. Duplicate keys change columns only on success.
        const auto column = static_cast<uint32_t>(primes.size());
        const auto [entry, inserted] = index.try_emplace(key, column);
        try {
            primes.push_back(prime);
        } catch (...) {
            if (inserted)
                index.erase(entry);
            throw;
        }
        entry->second = column;
    }

    [[nodiscard]] static constexpr size_t max_serialized_count() noexcept {
        return static_cast<size_t>((std::numeric_limits<uint32_t>::max)());
    }

    std::vector<RationalPrime> rational_;
    std::vector<AlgebraicPrime> algebraic_;
    FactorBaseParams params_;
    // 筛选用的代数素数数量。未显式设置时使用 algebraic_.size()；显式 0
    // 表示没有代数素数参与筛选（例如只有 special-Q 条目）。
    // IMPORTANT: 实现 save()/load() 时必须序列化这两个字段
    size_t sieve_algebraic_count_ = 0;
    bool sieve_algebraic_count_set_ = false;

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
