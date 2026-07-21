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
#include <cstdlib>
#include <queue>
#include <string>
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
    size_t lp_cancel_rejections = 0;    // 总 LP cancel rejections (fast-path + heavy)
    size_t fast_path_rejects = 0;       // overlap fast-path 拒绝 (无 LP 重叠)
    size_t heavy_path_rejects = 0;      // merge_two 后 cancel check 拒绝 (rare)
    size_t residual_emitted = 0;        // 残留 LP 的 merged rels 仍 emit
    size_t residual_dropped = 0;        // 残留 LP 的 merged rels 丢弃 (GNFS_DROP_RESIDUAL=1)
    size_t singletons_removed = 0;      // singleton 清理删除

    [[nodiscard]] bool operator==(const CliqueStats&) const noexcept = default;

    /// 一行 summary, 便于 log 输出
    [[nodiscard]] std::string to_string() const {
        return "in=" + std::to_string(input_relations) +
               " (1lp=" + std::to_string(input_1lp) +
               " 2lp=" + std::to_string(input_2lp) +
               " 3lp+=" + std::to_string(input_3lp_plus) + ")" +
               " components=" + std::to_string(components_with_excess) +
               "/" + std::to_string(components_found) +
               " full=" + std::to_string(full_produced) +
               " residual=" + std::to_string(residual_emitted) +
               " dropped=" + std::to_string(residual_dropped) +
               " rejects=" + std::to_string(lp_cancel_rejections) +
               " (fast=" + std::to_string(fast_path_rejects) +
               " heavy=" + std::to_string(heavy_path_rejects) + ")";
    }
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
        // V3 cascade typical yield: 5-15% full + residual produced.
        // partials.size()/8 is conservative; over-reserve worst case OK.
        std::vector<Relation> results;
        results.reserve(partials.size() / 8);

        // ── 预过滤: 1LP + 2LP 总进 pool, 3LP+ 是否进取决于 ENV GNFS_3LP=1 ──
        //
        // V3 cascade BFS spanning tree 算法本身用 pool_lp_keys[i] 是 generic
        // 任意 LP count, 只需放开 pool prefilter 即可让 3LP relations 参与
        // chain merge. CADO-NFS purge.c 同样思路 (cliques over arbitrary degree).
        //
        // 默认 OFF (GNFS_3LP unset) 时 3LP+ 仍 drop, 零回归. 启用 OPT-IN 时:
        //   - 3LP+ 进 pool, Union-Find 建 component, BFS 沿 LP-share 边 merge
        //   - LP cancel check 仍保证 reject merge that doesn't strictly reduce LP count
        //   - merge accumulator 路径上 LP count 单调 decrease (residual ≤ 0)
        //
        // ENV 每次调用重读 (非 static cache): 测试可在同一进程内切换模式,
        // pipeline 长期运行也能 hot-reload (虽然实际不会做).
        const bool accept_3lp_pool = []() {
            const char* env = std::getenv("GNFS_3LP");
            return env && std::atoi(env) == 1;
        }();
        std::vector<Relation> pool;
        pool.reserve(partials.size());
        std::vector<std::vector<LargePrimeKey>> pool_lp_keys;
        pool_lp_keys.reserve(partials.size());
        for (auto& rel : partials) {
            auto keys = odd_large_prime_keys(rel);
            const size_t nlp = keys.size();
            if (nlp == 0) {
                // Relation::num_large_primes() is a raw storage count. Even LP
                // exponents are matrix-full and must survive this fallback path.
                ++stats.full_produced;
                results.push_back(std::move(rel));
            }
            else if (nlp == 1) {
                ++stats.input_1lp;
                pool.push_back(std::move(rel));
                pool_lp_keys.push_back(std::move(keys));
            }
            else if (nlp == 2) {
                ++stats.input_2lp;
                pool.push_back(std::move(rel));
                pool_lp_keys.push_back(std::move(keys));
            }
            else if (accept_3lp_pool) {
                ++stats.input_3lp_plus;
                pool.push_back(std::move(rel));
                pool_lp_keys.push_back(std::move(keys));
            }
            else { ++stats.input_3lp_plus; }
        }

        if (pool.size() < 2) {
            if (stats_out) *stats_out = stats;
            return results;
        }

        // ── 构建 LP 索引 + pool LP keys cache (避免 BFS 重复 remaining_lp_keys 调用) ──
        std::unordered_map<LargePrimeKey, std::vector<size_t>, LargePrimeKeyHash> lp_index;
        lp_index.reserve(pool.size() * 2);
        for (size_t i = 0; i < pool.size(); ++i) {
            for (const auto& key : pool_lp_keys[i]) {
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
        // Reserve assuming worst case (each rel its own component) — avoids
        // mid-insertion rehashing. Real components_found is typically much
        // smaller, but reserve is bounded by pool.size() so memory cost is OK.
        std::unordered_map<size_t, std::vector<size_t>> components;
        components.reserve(pool.size());
        for (size_t i = 0; i < pool.size(); ++i) {
            components[uf.find(i)].push_back(i);
        }
        stats.components_found = components.size();

        // ── Per-component BFS spanning tree merge ──
        std::vector<bool> used(pool.size(), false);
        for (auto& [root, members] : components) {
            if (members.size() < 2) continue;
            ++stats.components_with_excess;
            merge_component(pool, pool_lp_keys, members, lp_index, used, results, stats);
        }

        // ── Singleton cleanup: 剩余未用的 partial 检查 LP keys 是否全 singleton ──
        std::unordered_set<LargePrimeKey, LargePrimeKeyHash> singleton_keys;
        singleton_keys.reserve(lp_index.size());  // upper bound
        for (const auto& [key, rels] : lp_index) {
            // After merging, recompute weights from unused rels
            size_t alive = 0;
            for (size_t idx : rels) if (!used[idx]) ++alive;
            if (alive == 1) singleton_keys.insert(key);
        }
        for (size_t i = 0; i < pool.size(); ++i) {
            if (used[i]) continue;
            const auto& keys = pool_lp_keys[i];  // use pre-computed cache
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
            const std::vector<std::vector<LargePrimeKey>>& pool_lp_keys,
            const std::vector<size_t>& members,
            const std::unordered_map<LargePrimeKey, std::vector<size_t>, LargePrimeKeyHash>& lp_index,
            std::vector<bool>& used,
            std::vector<Relation>& results,
            CliqueStats& stats) {

        // 本 component 的 index 子集
        std::unordered_set<size_t> in_component;
        in_component.reserve(members.size());
        in_component.insert(members.begin(), members.end());
        std::unordered_set<size_t> visited;
        visited.reserve(members.size());

        // 从第 1 个未用成员开始
        for (size_t start : members) {
            if (used[start] || visited.count(start)) continue;

            // BFS, accumulator 沿路 merge
            std::queue<size_t> bfs;
            bfs.push(start);
            visited.insert(start);
            Relation acc = pool[start];  // copy 作 accumulator
            used[start] = true;
            size_t accepted_source_count = 1;
            bool acc_full = PartialRelationMerger::is_effectively_full(acc);
            // 缓存 acc 的 LP key SET (用于 overlap fast-path 跳过 merge_two)
            // Reserve 16: typical merge累计 LP count ≤ 8-16 throughout BFS.
            auto acc_keys = PartialRelationMerger::remaining_lp_keys(acc);
            std::unordered_set<LargePrimeKey, LargePrimeKeyHash> acc_lp_set;
            acc_lp_set.reserve(16);
            acc_lp_set.insert(acc_keys.begin(), acc_keys.end());

            while (!bfs.empty()) {
                size_t cur = bfs.front(); bfs.pop();
                if (acc_full) break;  // accumulator 已满, 等下个 batch

                // 找邻居: 与 cur 共享 LP key 的所有 rels (都在 lp_index)
                // 用 cache pool_lp_keys[cur] (cur is original pool index)
                const auto& cur_keys = pool_lp_keys[cur];
                for (const auto& key : cur_keys) {
                    auto it = lp_index.find(key);
                    if (it == lp_index.end()) continue;
                    for (size_t nbr : it->second) {
                        if (!in_component.count(nbr) || visited.count(nbr) || used[nbr]) continue;

                        // Fast-path: 检查 nbr 与 acc 是否有 LP overlap.
                        // 无 overlap → merge 不 cancel 任何 key → after == before → reject.
                        // 等价于 LP cancel check, 但避免 heavy merge_two + count_keys.
                        const auto& nbr_keys = pool_lp_keys[nbr];  // use cache
                        bool has_overlap = false;
                        for (const auto& k : nbr_keys) {
                            if (acc_lp_set.count(k)) { has_overlap = true; break; }
                        }
                        if (!has_overlap) {
                            ++stats.lp_cancel_rejections;
                            ++stats.fast_path_rejects;
                            continue;  // 必 reject, skip merge_two
                        }

                        // Have overlap: do merge_two + verify strict reduction
                        size_t before = acc_lp_set.size() + nbr_keys.size();
                        Relation candidate = PartialRelationMerger::merge_two(acc, pool[nbr]);
                        auto cand_keys = PartialRelationMerger::remaining_lp_keys(candidate);

                        if (cand_keys.size() >= before) {
                            ++stats.lp_cancel_rejections;
                            ++stats.heavy_path_rejects;
                            continue;  // 罕见: 有 overlap 但 cancellation 没显著减
                        }

                        // Accept: update accumulator + LP set cache
                        acc = std::move(candidate);
                        const bool now_full = cand_keys.empty();  // reuse cand_keys
                        acc_lp_set.clear();
                        if (!now_full) acc_lp_set.insert(cand_keys.begin(), cand_keys.end());
                        visited.insert(nbr);
                        used[nbr] = true;
                        ++accepted_source_count;
                        bfs.push(nbr);

                        if (now_full) {
                            // is_effectively_full(acc) == cand_keys.empty() — saved a remaining_lp_keys call
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
            } else if (accepted_source_count > 1) {
                // Merged 但仍残留 LP → emit 或 drop
                //
                // BACKLOG #80 算法突破 [drop-residual]: 含残留 LP 的 merged rels 是
                // weight≥3 LP chain 留下的"碎片",在 matrix 里会增 LP cols 但不能
                // cancel,实测 50d V3 cascade β=121.4-121.6% 主要由这些 residual
                // 贡献 (推算 ~70%). 启用 GNFS_DROP_RESIDUAL=1 时 drop 这些 (CADO-NFS
                // purge.c 思路: weight-cutoff 而非 merge).
                //
                // 注意: drop residual 会减 usable count (~30-50%),但同步减 lp_cols
                // (~50-70%),净效应是 β 改善,需配合更大 sieve target 补偿 raw drop.
                static const bool drop_residual = []() {
                    const char* env = std::getenv("GNFS_DROP_RESIDUAL");
                    return env && std::atoi(env) == 1;
                }();
                if (drop_residual) {
                    ++stats.residual_dropped;
                } else {
                    ++stats.residual_emitted;
                    results.push_back(std::move(acc));
                }
            } else {
                // This source was tentatively marked used when its local BFS
                // started, but no neighbour was accepted into its accumulator.
                // Keep it unavailable to this component traversal via
                // `visited`, while returning it to the unused population so
                // singleton cleanup can account for it.
                used[start] = false;
            }
        }
    }
};

}  // namespace gnfs::relation
