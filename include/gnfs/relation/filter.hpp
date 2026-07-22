#pragma once

#include "../core/relation.hpp"
#include "large_prime_key.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gnfs::relation {

using core::Relation;

/// 过滤统计
struct FilterStats {
    size_t input_relations = 0;
    size_t output_relations = 0;
    size_t singletons_removed = 0;
    size_t duplicates_removed = 0;
    size_t passes = 0;

    [[nodiscard]] bool operator==(const FilterStats&) const noexcept = default;
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
        // Statistics and the pass budget are per invocation. A filter object
        // may be reused across adaptive reduction rounds, so carrying passes
        // forward would make later calls silently skip filtering.
        stats_ = FilterStats{};
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

    /// Count effective LP incidence by relation row.
    ///
    /// A key contributes at most once per relation and only when its combined
    /// exponent is odd. Raw PrimePower entry counts are intentionally not used.
    [[nodiscard]] static std::unordered_map<LargePrimeKey, size_t, LargePrimeKeyHash>
    count_large_primes(const std::vector<Relation>& relations) {

        std::unordered_map<LargePrimeKey, size_t, LargePrimeKeyHash> counts;
        // Reserve: avg ~1.5 LPs per partial → reserve N to avoid rehash mid-insertion.
        counts.reserve(relations.size());

        for (const auto& rel : relations) {
            for_each_odd_large_prime_key(rel, [&](const LargePrimeKey& key) {
                ++counts[key];
            });
        }

        return counts;
    }

    /// 获取所有唯一的大素数
    [[nodiscard]] static std::vector<LargePrimeKey>
    get_unique_large_primes(const std::vector<Relation>& relations) {

        std::unordered_set<LargePrimeKey, LargePrimeKeyHash> seen;
        seen.reserve(relations.size());

        for (const auto& rel : relations) {
            for_each_odd_large_prime_key(rel, [&](const LargePrimeKey& key) {
                seen.insert(key);
            });
        }

        std::vector<LargePrimeKey> unique(seen.begin(), seen.end());
        std::sort(unique.begin(), unique.end());
        return unique;
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
        singletons.reserve(counts.size());
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
            for_each_odd_large_prime_key(rel, [&](const LargePrimeKey& key) {
                if (singletons.count(key) > 0) {
                    has_singleton = true;
                }
            });

            if (has_singleton) {
                ++stats_.singletons_removed;
            } else {
                filtered.push_back(std::move(rel));
            }
        }

        return filtered;
    }
};

/// Count unique LP keys with odd combined exponent (matrix_builder convention).
/// 这是 matrix 实际为 LP 创建的列数 — sieve loop adaptive target 必须用准确值
/// 估算 effective_cols. 否则可能在 NO EXCESS gap 前提前 break sieve loop.
///
/// rat: 按 prime 聚合 (no root). alg: 按 (p, r) pair 聚合.
/// 只 count 奇指数 keys (matrix_builder.hpp collect_large_primes 同 convention).
[[nodiscard]] inline size_t count_unique_lp_keys(
        const std::vector<Relation>& relations) {
    // A structural key is required here: p and r are both uint64_t and cannot
    // be packed losslessly into one uint64_t value.
    std::unordered_set<LargePrimeKey, LargePrimeKeyHash> unique_keys;
    unique_keys.reserve(relations.size());

    for (const auto& rel : relations) {
        for_each_odd_large_prime_key(rel, [&](const LargePrimeKey& key) {
            unique_keys.insert(key);
        });
    }
    return unique_keys.size();
}

/// Saturating estimate of all matrix columns, including large-prime columns.
/// Saturation keeps downstream target and handoff arithmetic fail-closed.
[[nodiscard]] constexpr size_t effective_column_count(
        size_t matrix_columns, size_t large_prime_columns) noexcept {
    return large_prime_columns > std::numeric_limits<size_t>::max() - matrix_columns
               ? std::numeric_limits<size_t>::max()
               : matrix_columns + large_prime_columns;
}

/// Whether relation rows strictly exceed all estimated matrix columns.
/// Keep this comparison centralized so LP-aware adaptive drivers cannot
/// accidentally stop at the bare factor-base estimate.
[[nodiscard]] constexpr bool has_effective_column_excess(
        size_t relation_rows, size_t matrix_columns,
        size_t large_prime_columns) noexcept {
    return relation_rows > effective_column_count(matrix_columns, large_prime_columns);
}

/// LP-key weight histogram across a relation set.
///
/// Counts how many unique LP keys appear in exactly k relations,
/// for k ∈ {1, 2, 3, ≥4}. Diagnostic for BACKLOG #1 plateau analysis:
/// - weight_1 = singletons (kill mergeability — produce LP cols)
/// - weight_2 = V0 sweet spot (PartialRelationMerger handles directly)
/// - weight_3plus = chain-merge territory (V0_BFS / V3 cascade only)
///
/// Weight is relation-row incidence in the actual GF(2) matrix: a key counts
/// once in a row only when its combined exponent in that relation is odd.
struct LpKeyWeightHistogram {
    size_t weight_1 = 0;
    size_t weight_2 = 0;
    size_t weight_3 = 0;
    size_t weight_4plus = 0;
    size_t unique_keys = 0;

    [[nodiscard]] bool operator==(const LpKeyWeightHistogram&) const noexcept = default;
};

/// Incremental LP-key row-incidence histogram for streamed corpora.
class LpKeyWeightAccumulator final {
public:
    explicit LpKeyWeightAccumulator(size_t expected_rows = 0) {
        weights_.reserve(expected_rows);
    }

    void append(const Relation& relation) {
        for_each_odd_large_prime_key(relation, [&](const LargePrimeKey& key) { ++weights_[key]; });
    }

    [[nodiscard]] LpKeyWeightHistogram finish() const noexcept {
        LpKeyWeightHistogram histogram;
        for (const auto& [_, weight] : weights_) {
            ++histogram.unique_keys;
            if (weight == 1) {
                ++histogram.weight_1;
            } else if (weight == 2) {
                ++histogram.weight_2;
            } else if (weight == 3) {
                ++histogram.weight_3;
            } else {
                ++histogram.weight_4plus;
            }
        }
        return histogram;
    }

private:
    std::unordered_map<LargePrimeKey, size_t, LargePrimeKeyHash> weights_;
};

[[nodiscard]] inline LpKeyWeightHistogram
count_lp_key_weights(const std::vector<Relation>& relations) {
    LpKeyWeightAccumulator accumulator(relations.size());
    for (const auto& rel : relations) {
        accumulator.append(rel);
    }
    return accumulator.finish();
}

/// 分离完全关系和部分关系
struct SeparatedRelations {
    std::vector<Relation> full;      // 无大素数
    std::vector<Relation> partial;   // 有大素数
};

[[nodiscard]] inline SeparatedRelations separate_relations(
        std::vector<Relation>&& relations) {

    SeparatedRelations result;
    // Reserve: in 50d/60d typically ~5-10% full + 90-95% partial. Reserve
    // partial slightly under 1× input, full at 10%.
    result.partial.reserve(relations.size() * 9 / 10);
    result.full.reserve(relations.size() / 10);

    for (auto& rel : relations) {
        // Relation::is_full() describes raw storage only. A relation carrying
        // even LP exponents is already full in the GF(2) matrix and must not be
        // sent to a partial merger where it could be discarded.
        if (odd_large_prime_keys_empty(rel)) {
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
        size_t full_produced = 0;       // 返回的 effective-full 关系数
        size_t rounds = 0;              // 迭代轮数
        size_t weight2_merges = 0;      // weight-2 合并次数
        size_t singletons_removed = 0;  // 移除的 singleton 关系数
        size_t input_1lp = 0;           // 输入 1LP 关系数
        size_t input_2lp = 0;           // 输入 2LP 关系数
        size_t input_3lp_plus = 0;      // 输入 3LP+ 关系数（已丢弃）
        size_t residual_emitted = 0;    // 返回的 merged residual 关系数
        size_t residual_dropped = 0;    // 由 residual-drop policy 丢弃的关系数
        size_t output_relations = 0;    // 总返回数 = full + emitted residual

        [[nodiscard]] bool operator==(const MergeStats&) const noexcept = default;
    };

    /// 提取关系的"有效" LP key（奇数次出现的 = 未取消的）
    ///
    /// Compatibility wrapper around the canonical GF(2) LP view. Raw
    /// Relation::is_full()/num_large_primes() intentionally keep their storage
    /// semantics; filtering and merging must use this helper instead.
    [[nodiscard]] static std::vector<LargePrimeKey> remaining_lp_keys(const Relation& rel) {
        return odd_large_prime_keys(rel);
    }

    /// 检查关系是否"有效完全"（所有 LP 都已取消）
    [[nodiscard]] static bool is_effectively_full(const Relation& rel) {
        return odd_large_prime_keys_empty(rel);
    }

    /// 合并两个关系
    /// LP 列表完整保留（不取消），因为 rational_sqrt 需要完整指数信息。
    /// 取消判断由 remaining_lp_keys() 在 merge_all 中负责。
    [[nodiscard]] static Relation merge_two(const Relation& r1, const Relation& r2) {
        Relation m;
        m.a = r1.a;
        m.b = r1.b;

        // Pre-reserve + double-insert (instead of copy-assign + insert):
        // copy-assignment may reset capacity (impl-defined), so reserve+insert
        // is the only way to guarantee a single allocation per vector.
        m.extra_ab_pairs.reserve(r1.extra_ab_pairs.size() + 1 + r2.extra_ab_pairs.size());
        m.extra_ab_pairs.insert(m.extra_ab_pairs.end(),
            r1.extra_ab_pairs.begin(), r1.extra_ab_pairs.end());
        m.extra_ab_pairs.emplace_back(r2.a, r2.b);
        m.extra_ab_pairs.insert(m.extra_ab_pairs.end(),
            r2.extra_ab_pairs.begin(), r2.extra_ab_pairs.end());

        m.rational_factors.reserve(r1.rational_factors.size() + r2.rational_factors.size());
        m.rational_factors.insert(m.rational_factors.end(),
            r1.rational_factors.begin(), r1.rational_factors.end());
        m.rational_factors.insert(m.rational_factors.end(),
            r2.rational_factors.begin(), r2.rational_factors.end());

        m.algebraic_factors.reserve(r1.algebraic_factors.size() + r2.algebraic_factors.size());
        m.algebraic_factors.insert(m.algebraic_factors.end(),
            r1.algebraic_factors.begin(), r1.algebraic_factors.end());
        m.algebraic_factors.insert(m.algebraic_factors.end(),
            r2.algebraic_factors.begin(), r2.algebraic_factors.end());

        // LP 列表完整连接（不取消共享 LP）
        // rational_sqrt 和 matrix_builder 都需要完整列表来正确计算指数
        m.rational_large_prime.reserve(r1.rational_large_prime.size() + r2.rational_large_prime.size());
        m.rational_large_prime.insert(m.rational_large_prime.end(),
            r1.rational_large_prime.begin(), r1.rational_large_prime.end());
        m.rational_large_prime.insert(m.rational_large_prime.end(),
            r2.rational_large_prime.begin(), r2.rational_large_prime.end());

        m.algebraic_large_prime.reserve(r1.algebraic_large_prime.size() + r2.algebraic_large_prime.size());
        m.algebraic_large_prime.insert(m.algebraic_large_prime.end(),
            r1.algebraic_large_prime.begin(), r1.algebraic_large_prime.end());
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

        // 分类并入池 LP 关系。3LP+ 是否进 pool 取决于 ENV GNFS_3LP=1.
        //
        // 旧行为 (default): 3LP+ 弃 (input_3lp_plus 仅统计).
        // 新行为 (GNFS_3LP=1): 3LP+ 也进 pool, Phase 2 weight-2 merge 步骤会处理.
        //   注: V0 standard merge 设计为 weight-2 LP keys 匹配, 3LP relations 提
        //   供更多 weight-2 候选 (每个 3LP 提供 3 个 LP keys), 仍可被 V0 处理.
        //   完整 3LP chain merge 需要 CliqueRelationMerger BFS spanning tree.
        //
        // ENV 每次调用重读 (非 static cache): 测试可在同一进程内切换模式,
        // 也不会因第一次 Pipeline 调用就 "固化" 模式. 性能影响微小 (1 个 getenv
        // per merge_all 调用, 而非 per relation).
        const bool accept_3lp_pool = []() {
            const char* env = std::getenv("GNFS_3LP");
            return env && std::atoi(env) == 1;
        }();
        std::vector<Relation> pool;
        pool.reserve(partials.size());
        for (auto& rel : partials) {
            const size_t nlp = count_odd_large_prime_keys(rel);
            if (nlp == 0) {
                // Robustness: callers normally separate these first, but an
                // even-exponent LP relation is already matrix-full and must
                // never disappear merely because it retains raw LP entries.
                full_results.push_back(std::move(rel));
            }
            else if (nlp == 1) { ++stats.input_1lp; pool.push_back(std::move(rel)); }
            else if (nlp == 2) { ++stats.input_2lp; pool.push_back(std::move(rel)); }
            else if (accept_3lp_pool) {
                // 3LP+ 进 pool 但 input_3lp_plus 仍计数, 便于诊断
                ++stats.input_3lp_plus;
                pool.push_back(std::move(rel));
            }
            else { ++stats.input_3lp_plus; }  // 旧路径: 丢弃
        }

        // ═══ Phase 1: 1LP 贪婪匹配 (weight≥2, 与旧 merge() 行为一致) ═══
        // 1LP+1LP 合并总是产生 full relation，对任何 weight 都值得做
        {
            // 识别 1LP 关系（remaining keys 恰好 1 个）
            // Reserve avoids rehash for typical pool size (~30-50% partials are 1lp).
            std::unordered_set<size_t> is_1lp;
            is_1lp.reserve(pool.size() / 2);
            std::unordered_map<LargePrimeKey, std::vector<size_t>, LargePrimeKeyHash> lp1_index;
            lp1_index.reserve(pool.size() / 2);
            for (size_t i = 0; i < pool.size(); ++i) {
                auto keys = remaining_lp_keys(pool[i]);
                if (keys.size() == 1) {
                    is_1lp.insert(i);
                    lp1_index[keys[0]].push_back(i);
                }
            }

            // Sort keys for deterministic merge order across runs
            std::vector<LargePrimeKey> sorted_1lp_keys;
            sorted_1lp_keys.reserve(lp1_index.size());
            for (const auto& [key, indices] : lp1_index) {
                if (indices.size() >= 2) sorted_1lp_keys.push_back(key);
            }
            std::sort(sorted_1lp_keys.begin(), sorted_1lp_keys.end());

            // used_1lp: 2 idx per merge, mergeable keys size sorted_1lp_keys.
            // Worst case used = 2 × sorted_1lp_keys.size() (paired up).
            std::unordered_set<size_t> used_1lp;
            used_1lp.reserve(sorted_1lp_keys.size() * 2);
            for (const auto& key : sorted_1lp_keys) {
                const auto& indices = lp1_index.at(key);
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

            // 构建 LP 索引 + 缓存 pool LP keys (避免下方 singleton check 重复调用)
            std::unordered_map<LargePrimeKey, std::vector<size_t>, LargePrimeKeyHash> lp_index;
            lp_index.reserve(pool.size() * 2);
            std::vector<std::vector<LargePrimeKey>> pool_lp_keys_cache(pool.size());
            for (size_t i = 0; i < pool.size(); ++i) {
                pool_lp_keys_cache[i] = remaining_lp_keys(pool[i]);
                for (const auto& key : pool_lp_keys_cache[i]) {
                    lp_index[key].push_back(i);
                }
            }

            // Singleton detection
            // Reserve: typical ~30% LP keys are singletons in V0 Phase 2.
            std::unordered_set<LargePrimeKey, LargePrimeKeyHash> singleton_keys;
            singleton_keys.reserve(lp_index.size() / 3);
            for (const auto& [key, indices] : lp_index) {
                if (indices.size() == 1) singleton_keys.insert(key);
            }

            // Weight-2 merge（标准 2LP 处理）
            // used: 2 idx per merge, typical 30% pool reaches merge. dead 上限 pool.size().
            // new_merged: 30-50% merges produce non-full → residual. Conservative pool/4.
            std::unordered_set<size_t> used;
            used.reserve(pool.size() / 2);
            std::vector<Relation> new_merged;
            new_merged.reserve(pool.size() / 4);

            // NOTE: Only weight-2 LP keys merged by default. ENV
            // GNFS_V0_WEIGHT3=1 also explicitly merges first 2 partials of
            // weight-3 keys (BACKLOG #80 partial V0 weight≥3 handling — V3
            // cascade already handles full chain, this is a lighter
            // V0-internal alternative without BFS overhead). Conservative
            // since the 3rd partial becomes singleton next round.
            static const bool merge_weight3 = []() {
                const char* env = std::getenv("GNFS_V0_WEIGHT3");
                return env && std::atoi(env) == 1;
            }();
            const size_t max_merge_weight = merge_weight3 ? 3 : 2;

            // Sort keys for deterministic merge order across runs.
            // Reserve: typical ~20-30% LP keys are weight=2 in 50d/lp_bits=23.
            std::vector<LargePrimeKey> sorted_2lp_keys;
            sorted_2lp_keys.reserve(lp_index.size() / 4);
            for (const auto& [key, indices] : lp_index) {
                if (indices.size() >= 2 && indices.size() <= max_merge_weight) {
                    sorted_2lp_keys.push_back(key);
                }
            }
            std::sort(sorted_2lp_keys.begin(), sorted_2lp_keys.end());

            for (const auto& key : sorted_2lp_keys) {
                const auto& indices = lp_index.at(key);
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
            // 使用 pool_lp_keys_cache 避免 remaining_lp_keys 重复调用
            // Reserve: typical 5-10% pool flagged dead per round.
            //
            // BACKLOG #80 [weight-cutoff]: ENV GNFS_WEIGHT_CUTOFF=N (≥2) 时,
            // 还把 "any LP key has weight > N" 的关系标 dead.
            // CADO-NFS purge.c 思路: weight-3+ LP keys 形成 chain residue 污染 matrix,
            // 直接 drop 比 try-merge 更净空 β. N=2 时只保留 weight≤2 keys (= V0 mergeable).
            static const size_t weight_cutoff = []() {
                const char* env = std::getenv("GNFS_WEIGHT_CUTOFF");
                if (!env) return size_t(0);  // 0 = disabled (no cutoff)
                int v = std::atoi(env);
                return (v >= 2 && v <= 100) ? size_t(v) : size_t(0);
            }();
            std::unordered_set<size_t> dead;
            dead.reserve(pool.size() / 8);
            for (size_t i = 0; i < pool.size(); ++i) {
                if (used.count(i)) continue;
                const auto& keys = pool_lp_keys_cache[i];
                bool all_singleton = true;
                bool over_cutoff = false;
                for (const auto& k : keys) {
                    if (!singleton_keys.count(k)) { all_singleton = false; }
                    if (weight_cutoff > 0) {
                        auto it = lp_index.find(k);
                        if (it != lp_index.end() && it->second.size() > weight_cutoff) {
                            over_cutoff = true;
                            break;
                        }
                    }
                }
                if ((all_singleton && !keys.empty()) || over_cutoff) dead.insert(i);
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
        //
        // BACKLOG #80 [drop-residual]: GNFS_DROP_RESIDUAL=1 同 V3 path 一致, drop
        // 这些 residual partial (含残留 LP 的 merged), 仅 emit 完全 cancel 的 full.
        static const bool drop_residual = []() {
            const char* env = std::getenv("GNFS_DROP_RESIDUAL");
            return env && std::atoi(env) == 1;
        }();
        for (auto& rel : pool) {
            if (rel.is_merged()) {
                if (drop_residual) {
                    // 检查是否有残留 LP — 若有, drop; 完全 cancel 的 emit
                    auto keys = remaining_lp_keys(rel);
                    if (keys.empty()) {
                        full_results.push_back(std::move(rel));
                    } else {
                        ++stats.residual_dropped;
                    }
                } else {
                    full_results.push_back(std::move(rel));
                }
            }
        }

        // Keep output accounting explicit: residual relations are useful V0
        // outputs, but they are not full matrix rows and must not inflate the
        // full-production metric.
        for (const auto& rel : full_results) {
            if (is_effectively_full(rel)) {
                ++stats.full_produced;
            } else {
                ++stats.residual_emitted;
            }
        }
        stats.output_relations = full_results.size();
        if (stats_out) *stats_out = stats;
        return full_results;
    }

    /// 旧版 1LP-only 合并（保留向后兼容）
    [[nodiscard]] static std::vector<Relation> merge(
            const std::vector<Relation>& partials) {

        std::vector<Relation> merged;
        merged.reserve(partials.size() / 8);  // ~12% merge yield typical

        // Pre-filter: only index relations with exactly one effective LP key.
        std::vector<size_t> lp1_indices;
        lp1_indices.reserve(partials.size() / 4);
        std::unordered_map<LargePrimeKey, std::vector<size_t>, LargePrimeKeyHash>
            prime_to_relations;
        prime_to_relations.reserve(partials.size() / 4);
        for (size_t i = 0; i < partials.size(); ++i) {
            auto keys = odd_large_prime_keys(partials[i]);
            if (keys.size() == 1) {
                lp1_indices.push_back(i);
                prime_to_relations[keys.front()].push_back(i);
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
    return static_cast<size_t>(static_cast<double>(columns) * excess_factor) + 1;
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

} // namespace gnfs::relation
