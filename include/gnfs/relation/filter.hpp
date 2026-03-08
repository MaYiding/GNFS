#pragma once

#include "../core/relation.hpp"
#include "../core/types.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gnfs {
namespace relation {

using core::PrimePower;
using core::Relation;

/// 过滤统计
struct FilterStats {
    size_t input_relations = 0;
    size_t output_relations = 0;
    size_t singletons_removed = 0;
    size_t duplicates_removed = 0;
    size_t passes = 0;
};

/// 大素数键（用于哈希）
struct LargePrimeKey {
    uint64_t prime;
    bool is_algebraic;  // true = 代数侧, false = 有理侧

    bool operator==(const LargePrimeKey& other) const noexcept {
        return prime == other.prime && is_algebraic == other.is_algebraic;
    }
};

/// LargePrimeKey 哈希
struct LargePrimeKeyHash {
    size_t operator()(const LargePrimeKey& k) const noexcept {
        return std::hash<uint64_t>{}(k.prime) ^
               (std::hash<bool>{}(k.is_algebraic) << 1);
    }
};

/// 过滤配置
struct FilterConfig {
    bool remove_singletons = true;     // 移除单例大素数
    size_t max_passes = 10;            // 最大过滤轮数
    size_t min_relations = 0;          // 最小关系数（0 = 无限制）
    bool verbose = false;              // 详细输出
};

/// RelationFilter - 关系过滤器
/// 移除单例和执行 clique 合并
class RelationFilter {
public:
    using Config = FilterConfig;

    /// 构造函数
    explicit RelationFilter(const Config& config = Config{})
        : config_(config) {}

    /// 过滤关系
    /// 移除包含仅出现一次的大素数的关系（单例）
    /// @param relations 输入关系
    /// @return 过滤后的关系
    [[nodiscard]] std::vector<Relation> filter(std::vector<Relation>&& relations) {
        stats_.input_relations = relations.size();

        // 多轮过滤，直到没有更多单例
        bool changed = true;
        while (changed && stats_.passes < config_.max_passes) {
            ++stats_.passes;

            size_t before = relations.size();
            relations = filter_pass(std::move(relations));
            size_t after = relations.size();

            changed = (after < before);

            // 检查是否低于最小关系数
            if (config_.min_relations > 0 && after < config_.min_relations) {
                break;
            }
        }

        stats_.output_relations = relations.size();
        return relations;
    }

    /// 获取统计
    [[nodiscard]] const FilterStats& stats() const noexcept {
        return stats_;
    }

    /// 重置统计
    void reset_stats() {
        stats_ = FilterStats{};
    }

    /// 统计大素数出现频率
    [[nodiscard]] static std::unordered_map<LargePrimeKey, size_t, LargePrimeKeyHash>
    count_large_primes(const std::vector<Relation>& relations) {

        std::unordered_map<LargePrimeKey, size_t, LargePrimeKeyHash> counts;

        for (const auto& rel : relations) {
            // 有理侧大素数
            for (size_t i = 0; i < rel.rational_large_prime.size(); ++i) {
                LargePrimeKey key{rel.rational_large_prime[i].p, false};
                ++counts[key];
            }

            // 代数侧大素数
            for (size_t i = 0; i < rel.algebraic_large_prime.size(); ++i) {
                LargePrimeKey key{rel.algebraic_large_prime[i].p, true};
                ++counts[key];
            }
        }

        return counts;
    }

    /// 获取所有唯一的大素数
    [[nodiscard]] static std::vector<LargePrimeKey>
    get_unique_large_primes(const std::vector<Relation>& relations) {

        std::unordered_set<LargePrimeKey, LargePrimeKeyHash> seen;

        for (const auto& rel : relations) {
            for (size_t i = 0; i < rel.rational_large_prime.size(); ++i) {
                seen.insert(LargePrimeKey{rel.rational_large_prime[i].p, false});
            }
            for (size_t i = 0; i < rel.algebraic_large_prime.size(); ++i) {
                seen.insert(LargePrimeKey{rel.algebraic_large_prime[i].p, true});
            }
        }

        return std::vector<LargePrimeKey>(seen.begin(), seen.end());
    }

private:
    Config config_;
    FilterStats stats_;

    /// 单轮过滤
    [[nodiscard]] std::vector<Relation> filter_pass(std::vector<Relation>&& relations) {
        if (!config_.remove_singletons) {
            return std::move(relations);
        }

        // 统计大素数出现次数
        auto counts = count_large_primes(relations);

        // 找出单例
        std::unordered_set<LargePrimeKey, LargePrimeKeyHash> singletons;
        for (const auto& [key, count] : counts) {
            if (count == 1) {
                singletons.insert(key);
            }
        }

        if (singletons.empty()) {
            return std::move(relations);
        }

        // 过滤包含单例的关系
        std::vector<Relation> filtered;
        filtered.reserve(relations.size());

        for (auto& rel : relations) {
            bool has_singleton = false;

            // 检查有理侧
            for (size_t i = 0; i < rel.rational_large_prime.size() && !has_singleton; ++i) {
                LargePrimeKey key{rel.rational_large_prime[i].p, false};
                if (singletons.count(key) > 0) {
                    has_singleton = true;
                }
            }

            // 检查代数侧
            for (size_t i = 0; i < rel.algebraic_large_prime.size() && !has_singleton; ++i) {
                LargePrimeKey key{rel.algebraic_large_prime[i].p, true};
                if (singletons.count(key) > 0) {
                    has_singleton = true;
                }
            }

            if (has_singleton) {
                ++stats_.singletons_removed;
            } else {
                filtered.push_back(std::move(rel));
            }
        }

        return filtered;
    }
};

/// 分离完全关系和部分关系
struct SeparatedRelations {
    std::vector<Relation> full;      // 无大素数
    std::vector<Relation> partial;   // 有大素数
};

[[nodiscard]] inline SeparatedRelations separate_relations(
        std::vector<Relation>&& relations) {

    SeparatedRelations result;

    for (auto& rel : relations) {
        if (rel.is_full()) {
            result.full.push_back(std::move(rel));
        } else {
            result.partial.push_back(std::move(rel));
        }
    }

    return result;
}

/// 合并部分关系（基于共享大素数）
/// 使用简化的 clique 方法
class PartialRelationMerger {
public:
    /// 尝试合并部分关系
    /// @param partials 部分关系列表
    /// @return 合并后产生的新关系（作为完全关系）
    [[nodiscard]] static std::vector<Relation> merge(
            const std::vector<Relation>& partials) {

        std::vector<Relation> merged;

        // 按大素数建立索引
        std::unordered_map<LargePrimeKey, std::vector<size_t>, LargePrimeKeyHash>
            prime_to_relations;

        for (size_t i = 0; i < partials.size(); ++i) {
            const auto& rel = partials[i];

            for (size_t j = 0; j < rel.rational_large_prime.size(); ++j) {
                LargePrimeKey key{rel.rational_large_prime[j].p, false};
                prime_to_relations[key].push_back(i);
            }

            for (size_t j = 0; j < rel.algebraic_large_prime.size(); ++j) {
                LargePrimeKey key{rel.algebraic_large_prime[j].p, true};
                prime_to_relations[key].push_back(i);
            }
        }

        // 找到共享同一大素数的关系对
        std::unordered_set<size_t> used;

        for (const auto& [key, indices] : prime_to_relations) {
            if (indices.size() >= 2) {
                // 这些关系共享一个大素数
                // 对于 1LP 关系，两个共享同一大素数的关系可以合并

                for (size_t i = 0; i < indices.size() && merged.size() < 1000; ++i) {
                    for (size_t j = i + 1; j < indices.size(); ++j) {
                        size_t idx1 = indices[i];
                        size_t idx2 = indices[j];

                        // 跳过已使用的
                        if (used.count(idx1) > 0 || used.count(idx2) > 0) {
                            continue;
                        }

                        const auto& rel1 = partials[idx1];
                        const auto& rel2 = partials[idx2];

                        // 检查是否都是 1LP
                        if (rel1.num_large_primes() == 1 &&
                            rel2.num_large_primes() == 1) {

                            // 可以合并！
                            // 合并后的关系 = rel1 * rel2 (模 2)
                            // 大素数会消除

                            used.insert(idx1);
                            used.insert(idx2);

                            // 创建合并关系（简化：只标记为已合并）
                            // 实际实现需要计算因子的异或
                        }
                    }
                }
            }
        }

        return merged;
    }

    /// 获取可合并的关系对数量
    [[nodiscard]] static size_t count_mergeable_pairs(
            const std::vector<Relation>& partials) {

        auto counts = RelationFilter::count_large_primes(partials);

        size_t pairs = 0;
        for (const auto& [key, count] : counts) {
            if (count >= 2) {
                // n 个关系共享一个大素数可产生 C(n,2) 对
                pairs += count * (count - 1) / 2;
            }
        }

        return pairs;
    }
};

/// 计算矩阵所需的关系数量
/// 需要 关系数 > 因子基大小 + 大素数数量
[[nodiscard]] inline size_t required_relations(
        size_t factor_base_size,
        size_t unique_large_primes,
        double excess_factor = 1.05) {

    size_t columns = factor_base_size + unique_large_primes;
    return static_cast<size_t>(columns * excess_factor) + 1;
}

/// 检查是否有足够的关系
[[nodiscard]] inline bool has_enough_relations(
        size_t num_relations,
        size_t factor_base_size,
        size_t unique_large_primes,
        double excess_factor = 1.05) {

    return num_relations >= required_relations(
        factor_base_size, unique_large_primes, excess_factor);
}

} // namespace relation
} // namespace gnfs
