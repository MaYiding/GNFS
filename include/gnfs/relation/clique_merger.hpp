#pragma once

// V3 Clique Merge — backup merge strategy.
//
// Algorithm: BFS spanning tree merge across LP-sharing bipartite graph.
// Critical safety: LP cancel check rejects merges that do not strictly
// reduce LP count (avoids V1/V2 chain-residue trap).
//
// Use case: When PartialRelationMerger::merge_all() (V0) produces
// insufficient full relations for matrix excess. V3 strictly expands
// V0 by handling weight≥3 LP keys via spanning tree merge.

#include "filter.hpp"  // for LargePrimeKey, merge_two, remaining_lp_keys, is_effectively_full
#include "../core/relation.hpp"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gnfs::relation {

using core::Relation;

/// Clique merge statistics
struct CliqueStats {
    size_t input_relations = 0;         // 输入关系数
    size_t input_1lp = 0;               // 1LP 输入
    size_t input_2lp = 0;               // 2LP 输入
    size_t input_3lp_plus = 0;          // 3LP+ 被丢弃
    size_t components_found = 0;        // 总连通分量数
    size_t components_with_excess = 0;  // 大小 >= 2 的分量
    size_t full_produced = 0;           // 产生的 full relations
    size_t lp_cancel_rejections = 0;    // LP cancel check 拒绝次数
    size_t residual_emitted = 0;        // 残留 LP 的 merged rels 仍 emit
    size_t singletons_removed = 0;      // singleton 清理删除
};

/// V3 Clique merge: BFS spanning tree with LP cancel check
class CliqueRelationMerger {
public:
    /// 主入口: 处理 1LP+2LP 关系, 产生 full + 残留 partial 输出
    [[nodiscard]] static std::vector<Relation> merge_cliques(
            std::vector<Relation> partials,
            CliqueStats* stats_out = nullptr) {

        CliqueStats stats;
        stats.input_relations = partials.size();
        std::vector<Relation> results;

        // ── 预过滤: 仅 1LP + 2LP (3LP+ 弃) ──
        std::vector<Relation> pool;
        pool.reserve(partials.size());
        for (auto& rel : partials) {
            size_t nlp = rel.num_large_primes();
            if (nlp == 1) { ++stats.input_1lp; pool.push_back(std::move(rel)); }
            else if (nlp == 2) { ++stats.input_2lp; pool.push_back(std::move(rel)); }
            else { ++stats.input_3lp_plus; }
        }

        if (pool.size() < 2) {
            if (stats_out) *stats_out = stats;
            return results;
        }

        // ── 构建 LP 索引 ──
        std::unordered_map<LargePrimeKey, std::vector<size_t>, LargePrimeKeyHash> lp_index;
        lp_index.reserve(pool.size() * 2);
        for (size_t i = 0; i < pool.size(); ++i) {
            auto keys = PartialRelationMerger::remaining_lp_keys(pool[i]);
            for (const auto& key : keys) {
                lp_index[key].push_back(i);
            }
        }

        // ── Union-Find: 联通共享 LP key 的关系 ──
        UnionFind uf(pool.size());
        for (const auto& [key, rels] : lp_index) {
            if (rels.size() < 2) continue;
            for (size_t i = 1; i < rels.size(); ++i) {
                uf.unite(rels[0], rels[i]);
            }
        }

        // ── 按 component 分组 ──
        std::unordered_map<size_t, std::vector<size_t>> components;
        for (size_t i = 0; i < pool.size(); ++i) {
            components[uf.find(i)].push_back(i);
        }
        stats.components_found = components.size();

        // ── Per-component BFS spanning tree merge ──
        std::vector<bool> used(pool.size(), false);
        for (auto& [root, members] : components) {
            if (members.size() < 2) continue;
            ++stats.components_with_excess;
            merge_component(pool, members, lp_index, used, results, stats);
        }

        // ── Singleton cleanup: 剩余未用的 partial 检查 LP keys 是否全 singleton ──
        std::unordered_set<LargePrimeKey, LargePrimeKeyHash> singleton_keys;
        for (const auto& [key, rels] : lp_index) {
            // After merging, recompute weights from unused rels
            size_t alive = 0;
            for (size_t idx : rels) if (!used[idx]) ++alive;
            if (alive == 1) singleton_keys.insert(key);
        }
        for (size_t i = 0; i < pool.size(); ++i) {
            if (used[i]) continue;
            auto keys = PartialRelationMerger::remaining_lp_keys(pool[i]);
            bool all_singleton = !keys.empty();
            for (const auto& k : keys) {
                if (!singleton_keys.count(k)) { all_singleton = false; break; }
            }
            if (all_singleton) ++stats.singletons_removed;
        }

        if (stats_out) *stats_out = stats;
        return results;
    }

private:
    /// Path-compressed Union-Find with union-by-rank
    struct UnionFind {
        std::vector<size_t> parent;
        std::vector<size_t> rank_;
        explicit UnionFind(size_t n) : parent(n), rank_(n, 0) {
            for (size_t i = 0; i < n; ++i) parent[i] = i;
        }
        size_t find(size_t i) {
            while (parent[i] != i) {
                parent[i] = parent[parent[i]];  // path compression
                i = parent[i];
            }
            return i;
        }
        void unite(size_t a, size_t b) {
            size_t ra = find(a), rb = find(b);
            if (ra == rb) return;
            if (rank_[ra] < rank_[rb]) std::swap(ra, rb);
            parent[rb] = ra;
            if (rank_[ra] == rank_[rb]) ++rank_[ra];
        }
    };

    /// BFS spanning tree merge within a component
    static void merge_component(
            std::vector<Relation>& pool,
            const std::vector<size_t>& members,
            const std::unordered_map<LargePrimeKey, std::vector<size_t>, LargePrimeKeyHash>& lp_index,
            std::vector<bool>& used,
            std::vector<Relation>& results,
            CliqueStats& stats) {

        // 本 component 的 index 子集
        std::unordered_set<size_t> in_component(members.begin(), members.end());
        std::unordered_set<size_t> visited;

        // 从第 1 个未用成员开始
        for (size_t start : members) {
            if (used[start] || visited.count(start)) continue;

            // BFS, accumulator 沿路 merge
            std::queue<size_t> bfs;
            bfs.push(start);
            visited.insert(start);
            Relation acc = pool[start];  // copy 作 accumulator
            used[start] = true;
            bool acc_full = PartialRelationMerger::is_effectively_full(acc);
            // 缓存 acc 的 LP key SET (用于 overlap fast-path 跳过 merge_two)
            auto acc_keys = PartialRelationMerger::remaining_lp_keys(acc);
            std::unordered_set<LargePrimeKey, LargePrimeKeyHash> acc_lp_set(
                acc_keys.begin(), acc_keys.end());

            while (!bfs.empty()) {
                size_t cur = bfs.front(); bfs.pop();
                if (acc_full) break;  // accumulator 已满, 等下个 batch

                // 找邻居: 与 cur 共享 LP key 的所有 rels (都在 lp_index)
                auto cur_keys = PartialRelationMerger::remaining_lp_keys(pool[cur]);
                for (const auto& key : cur_keys) {
                    auto it = lp_index.find(key);
                    if (it == lp_index.end()) continue;
                    for (size_t nbr : it->second) {
                        if (!in_component.count(nbr) || visited.count(nbr) || used[nbr]) continue;

                        // Fast-path: 检查 nbr 与 acc 是否有 LP overlap.
                        // 无 overlap → merge 不 cancel 任何 key → after == before → reject.
                        // 等价于 LP cancel check, 但避免 heavy merge_two + count_keys.
                        auto nbr_keys = PartialRelationMerger::remaining_lp_keys(pool[nbr]);
                        bool has_overlap = false;
                        for (const auto& k : nbr_keys) {
                            if (acc_lp_set.count(k)) { has_overlap = true; break; }
                        }
                        if (!has_overlap) {
                            ++stats.lp_cancel_rejections;
                            continue;  // 必 reject, skip merge_two
                        }

                        // Have overlap: do merge_two + verify strict reduction
                        size_t before = acc_lp_set.size() + nbr_keys.size();
                        Relation candidate = PartialRelationMerger::merge_two(acc, pool[nbr]);
                        auto cand_keys = PartialRelationMerger::remaining_lp_keys(candidate);

                        if (cand_keys.size() >= before) {
                            ++stats.lp_cancel_rejections;
                            continue;  // 罕见: 有 overlap 但 cancellation 没显著减
                        }

                        // Accept: update accumulator + LP set cache
                        acc = std::move(candidate);
                        acc_lp_set.clear();
                        acc_lp_set.insert(cand_keys.begin(), cand_keys.end());
                        visited.insert(nbr);
                        used[nbr] = true;
                        bfs.push(nbr);

                        if (PartialRelationMerger::is_effectively_full(acc)) {
                            acc_full = true;
                            break;
                        }
                    }
                    if (acc_full) break;
                }
            }

            // Emit accumulator
            if (acc_full) {
                ++stats.full_produced;
                results.push_back(std::move(acc));
            } else if (!visited.empty() && visited.size() > 1) {
                // Merged 但仍残留 LP → 仍 emit (V0 line 502 同 convention)
                ++stats.residual_emitted;
                results.push_back(std::move(acc));
            }
            // else: 单点 component (visited == {start}, no merge happened) → 弃
        }
    }
};

}  // namespace gnfs::relation
