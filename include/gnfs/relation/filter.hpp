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
/// degree≥3 多项式下同一素数 p 可能有多个代数根 r₁,r₂,...，
/// 对应不同素理想 (p, α-rᵢ)。键必须包含 root 以区分。
struct LargePrimeKey {
    uint64_t prime;
    uint64_t root;         // 代数侧的根 r（有理侧为 0）
    bool is_algebraic;     // true = 代数侧, false = 有理侧

    bool operator==(const LargePrimeKey& other) const noexcept {
        return prime == other.prime && root == other.root &&
               is_algebraic == other.is_algebraic;
    }
};

/// LargePrimeKey 哈希
struct LargePrimeKeyHash {
    size_t operator()(const LargePrimeKey& k) const noexcept {
        size_t h = std::hash<uint64_t>{}(k.prime);
        h ^= std::hash<uint64_t>{}(k.root) * 2654435761ULL;
        h ^= std::hash<bool>{}(k.is_algebraic) << 1;
        return h;
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
            // 有理侧大素数 (root=0)
            for (size_t i = 0; i < rel.rational_large_prime.size(); ++i) {
                LargePrimeKey key{rel.rational_large_prime[i].p, 0, false};
                ++counts[key];
            }

            // 代数侧大素数 (root=r，区分同一 p 的不同素理想)
            for (size_t i = 0; i < rel.algebraic_large_prime.size(); ++i) {
                const auto& lp = rel.algebraic_large_prime[i];
                LargePrimeKey key{lp.p, lp.r, true};
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
                seen.insert(LargePrimeKey{rel.rational_large_prime[i].p, 0, false});
            }
            for (size_t i = 0; i < rel.algebraic_large_prime.size(); ++i) {
                const auto& lp = rel.algebraic_large_prime[i];
                seen.insert(LargePrimeKey{lp.p, lp.r, true});
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
                LargePrimeKey key{rel.rational_large_prime[i].p, 0, false};
                if (singletons.count(key) > 0) {
                    has_singleton = true;
                }
            }

            // 检查代数侧
            for (size_t i = 0; i < rel.algebraic_large_prime.size() && !has_singleton; ++i) {
                const auto& lp = rel.algebraic_large_prime[i];
                LargePrimeKey key{lp.p, lp.r, true};
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
    /// 合并结果统计
    struct MergeStats {
        size_t full_produced = 0;       // 产出的完全关系数
        size_t rounds = 0;              // 迭代轮数
        size_t weight2_merges = 0;      // weight-2 合并次数
        size_t singletons_removed = 0;  // 移除的 singleton 关系数
        size_t input_1lp = 0;           // 输入 1LP 关系数
        size_t input_2lp = 0;           // 输入 2LP 关系数
        size_t input_3lp_plus = 0;      // 输入 3LP+ 关系数（已丢弃）
    };

    /// 提取关系的"有效" LP key（奇数次出现的 = 未取消的）
    /// 合并关系中，共享 LP 出现偶数次（已取消），只返回奇数次的
    [[nodiscard]] static std::vector<LargePrimeKey> remaining_lp_keys(const Relation& rel) {
        std::unordered_map<LargePrimeKey, size_t, LargePrimeKeyHash> counts;
        for (const auto& lp : rel.rational_large_prime) {
            ++counts[{lp.p, 0, false}];
        }
        for (const auto& lp : rel.algebraic_large_prime) {
            ++counts[{lp.p, lp.r, true}];
        }
        std::vector<LargePrimeKey> keys;
        for (const auto& [key, count] : counts) {
            if (count % 2 != 0) keys.push_back(key);
        }
        return keys;
    }

    /// 检查关系是否"有效完全"（所有 LP 都已取消）
    [[nodiscard]] static bool is_effectively_full(const Relation& rel) {
        return remaining_lp_keys(rel).empty();
    }

    /// 合并两个关系
    /// LP 列表完整保留（不取消），因为 rational_sqrt 需要完整指数信息。
    /// 取消判断由 remaining_lp_keys() 在 merge_all 中负责。
    [[nodiscard]] static Relation merge_two(const Relation& r1, const Relation& r2) {
        Relation m;
        m.a = r1.a;
        m.b = r1.b;

        // 收集所有 (a,b) 对（含已有的 extra pairs）
        m.extra_ab_pairs = r1.extra_ab_pairs;
        m.extra_ab_pairs.emplace_back(r2.a, r2.b);
        m.extra_ab_pairs.insert(m.extra_ab_pairs.end(),
            r2.extra_ab_pairs.begin(), r2.extra_ab_pairs.end());

        // 合并 factor base indices
        m.rational_factors = r1.rational_factors;
        m.rational_factors.insert(m.rational_factors.end(),
            r2.rational_factors.begin(), r2.rational_factors.end());

        m.algebraic_factors = r1.algebraic_factors;
        m.algebraic_factors.insert(m.algebraic_factors.end(),
            r2.algebraic_factors.begin(), r2.algebraic_factors.end());

        // LP 列表完整连接（不取消共享 LP）
        // rational_sqrt 和 matrix_builder 都需要完整列表来正确计算指数
        m.rational_large_prime = r1.rational_large_prime;
        m.rational_large_prime.insert(m.rational_large_prime.end(),
            r2.rational_large_prime.begin(), r2.rational_large_prime.end());

        m.algebraic_large_prime = r1.algebraic_large_prime;
        m.algebraic_large_prime.insert(m.algebraic_large_prime.end(),
            r2.algebraic_large_prime.begin(), r2.algebraic_large_prime.end());

        return m;
    }

    /// 全类型合并：处理 1LP 和 2LP 关系
    /// 迭代 weight-2 处理 + singleton 清理
    /// @param partials 所有部分关系（1LP + 2LP，3LP+ 会被丢弃）
    /// @param max_rounds 最大迭代轮数
    /// @return 产出的完全关系列表
    [[nodiscard]] static std::vector<Relation> merge_all(
            std::vector<Relation> partials,
            size_t max_rounds = 10,
            MergeStats* stats_out = nullptr) {

        MergeStats stats;
        std::vector<Relation> full_results;

        // 统计并丢弃 3LP+ 关系（基于原始 LP 数量）
        std::vector<Relation> pool;
        pool.reserve(partials.size());
        for (auto& rel : partials) {
            size_t nlp = rel.num_large_primes();
            if (nlp == 1) { ++stats.input_1lp; pool.push_back(std::move(rel)); }
            else if (nlp == 2) { ++stats.input_2lp; pool.push_back(std::move(rel)); }
            else { ++stats.input_3lp_plus; }  // 丢弃
        }

        // ═══ Phase 1: 1LP 贪婪匹配 (weight≥2, 与旧 merge() 行为一致) ═══
        // 1LP+1LP 合并总是产生 full relation，对任何 weight 都值得做
        {
            // 识别 1LP 关系（remaining keys 恰好 1 个）
            std::unordered_set<size_t> is_1lp;
            std::unordered_map<LargePrimeKey, std::vector<size_t>, LargePrimeKeyHash> lp1_index;
            for (size_t i = 0; i < pool.size(); ++i) {
                auto keys = remaining_lp_keys(pool[i]);
                if (keys.size() == 1) {
                    is_1lp.insert(i);
                    lp1_index[keys[0]].push_back(i);
                }
            }

            std::unordered_set<size_t> used_1lp;
            for (const auto& [key, indices] : lp1_index) {
                if (indices.size() < 2) continue;
                size_t first_unused = SIZE_MAX;
                for (size_t idx : indices) {
                    if (used_1lp.count(idx)) continue;
                    if (first_unused == SIZE_MAX) {
                        first_unused = idx; continue;
                    }
                    auto m = merge_two(pool[first_unused], pool[idx]);
                    used_1lp.insert(first_unused);
                    used_1lp.insert(idx);
                    ++stats.weight2_merges;
                    full_results.push_back(std::move(m));
                    first_unused = SIZE_MAX;
                }
            }

            // 从 pool 中移除已用的 1LP 关系
            if (!used_1lp.empty()) {
                std::vector<Relation> new_pool;
                new_pool.reserve(pool.size() - used_1lp.size());
                for (size_t i = 0; i < pool.size(); ++i) {
                    if (!used_1lp.count(i)) {
                        new_pool.push_back(std::move(pool[i]));
                    }
                }
                pool = std::move(new_pool);
            }
        }

        // ═══ Phase 2: 2LP 迭代 weight-2 处理 ═══
        for (size_t round = 0; round < max_rounds; ++round) {
            ++stats.rounds;

            // 构建 LP 索引
            std::unordered_map<LargePrimeKey, std::vector<size_t>, LargePrimeKeyHash> lp_index;
            lp_index.reserve(pool.size() * 2);
            for (size_t i = 0; i < pool.size(); ++i) {
                auto keys = remaining_lp_keys(pool[i]);
                for (const auto& key : keys) {
                    lp_index[key].push_back(i);
                }
            }

            // Singleton detection
            std::unordered_set<LargePrimeKey, LargePrimeKeyHash> singleton_keys;
            for (const auto& [key, indices] : lp_index) {
                if (indices.size() == 1) singleton_keys.insert(key);
            }

            // Weight-2 merge（标准 2LP 处理）
            std::unordered_set<size_t> used;
            std::vector<Relation> new_merged;

            // NOTE: Only weight-2 LP keys are merged. Weight-3+ keys could
            // be chain-merged in principle, but this conservative strategy avoids
            // creating overly dense matrix rows. Future: implement weight-3 merge
            // by pairing the two cheapest relations per LP key.
            for (const auto& [key, indices] : lp_index) {
                if (indices.size() != 2) continue;
                size_t i = indices[0], j = indices[1];
                if (used.count(i) || used.count(j)) continue;

                auto m = merge_two(pool[i], pool[j]);
                used.insert(i);
                used.insert(j);
                ++stats.weight2_merges;

                if (is_effectively_full(m)) {
                    full_results.push_back(std::move(m));
                } else {
                    new_merged.push_back(std::move(m));
                }
            }

            // Singleton removal: 只移除所有剩余 LP 都是 singleton 的关系
            std::unordered_set<size_t> dead;
            for (size_t i = 0; i < pool.size(); ++i) {
                if (used.count(i)) continue;
                auto keys = remaining_lp_keys(pool[i]);
                bool all_singleton = true;
                for (const auto& k : keys) {
                    if (!singleton_keys.count(k)) { all_singleton = false; break; }
                }
                if (all_singleton && !keys.empty()) dead.insert(i);
            }

            if (used.empty() && dead.empty()) break;
            stats.singletons_removed += dead.size();

            // 重建 pool
            std::vector<Relation> new_pool;
            new_pool.reserve(pool.size() - used.size() - dead.size()
                             + new_merged.size());
            for (size_t i = 0; i < pool.size(); ++i) {
                if (!used.count(i) && !dead.count(i))
                    new_pool.push_back(std::move(pool[i]));
            }
            new_pool.insert(new_pool.end(),
                std::make_move_iterator(new_merged.begin()),
                std::make_move_iterator(new_merged.end()));

            pool = std::move(new_pool);
            if (pool.empty()) break;
        }

        // 收集 pool 中已合并但仍有残留 LP 的关系
        // NOTE: Unmerged original 2LP relations are intentionally discarded.
        // Including them would add LP columns that may outnumber their
        // contribution to the matrix, causing column explosion.
        for (auto& rel : pool) {
            if (rel.is_merged()) full_results.push_back(std::move(rel));
        }

        stats.full_produced = full_results.size();
        if (stats_out) *stats_out = stats;
        return full_results;
    }

    /// 旧版 1LP-only 合并（保留向后兼容）
    [[nodiscard]] static std::vector<Relation> merge(
            const std::vector<Relation>& partials) {

        std::vector<Relation> merged;

        // Pre-filter: only index 1LP relations (those with exactly 1 large prime).
        std::vector<size_t> lp1_indices;
        lp1_indices.reserve(partials.size() / 4);
        for (size_t i = 0; i < partials.size(); ++i) {
            if (partials[i].num_large_primes() == 1) {
                lp1_indices.push_back(i);
            }
        }

        // 按大素数建立索引 (only 1LP relations)
        std::unordered_map<LargePrimeKey, std::vector<size_t>, LargePrimeKeyHash>
            prime_to_relations;
        prime_to_relations.reserve(lp1_indices.size());

        for (size_t idx : lp1_indices) {
            const auto& rel = partials[idx];

            for (size_t j = 0; j < rel.rational_large_prime.size(); ++j) {
                LargePrimeKey key{rel.rational_large_prime[j].p, 0, false};
                prime_to_relations[key].push_back(idx);
            }

            for (size_t j = 0; j < rel.algebraic_large_prime.size(); ++j) {
                const auto& lp = rel.algebraic_large_prime[j];
                LargePrimeKey key{lp.p, lp.r, true};
                prime_to_relations[key].push_back(idx);
            }
        }

        // Greedy matching: for each LP key with ≥2 relations, pair them off
        std::unordered_set<size_t> used;
        used.reserve(lp1_indices.size());

        for (const auto& [key, indices] : prime_to_relations) {
            if (indices.size() < 2) continue;

            // Linear scan: match consecutive unused pairs
            size_t first_unused = SIZE_MAX;
            for (size_t idx : indices) {
                if (used.count(idx) > 0) continue;
                if (first_unused == SIZE_MAX) {
                    first_unused = idx;
                    continue;
                }

                // Merge first_unused with idx
                auto m = merge_two(partials[first_unused], partials[idx]);
                used.insert(first_unused);
                used.insert(idx);
                merged.push_back(std::move(m));

                // Reset for next pair in same key
                first_unused = SIZE_MAX;
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
