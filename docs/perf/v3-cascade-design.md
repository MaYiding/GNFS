# V3 Clique Merge — 设计与使用 (破壁 fallback)

> 2026-05-16 添加。Trigger: V0+fix 50d/60d 仍 NO EXCESS 时使用。

## 背景

GNFS filter merge V0 (PartialRelationMerger) 只处理 weight=2 LP keys (恰好 2 个关系共享某个 LP 的简单 pairing)。对 lp_bits=23+ 的大 N (50d, 60d),很多 LP key 是 weight≥3 (3+ 关系共享同一 LP key)。V0 直接弃这些 keys,导致大量 partial relations 无法 merge 进 full 输出。

V1 (commit 11d1d83 早期) 尝试递归: 让 weight=3 keys 先 pairwise merge 两个, 第三个仍带 residue, 进入下一轮。结果 chain residue 累积, sngl 飙升, regression。

V2 (commit 21dcbcd, 已 revert): 让 weight≥2 都 merge 一次, 不递归。25d ✓ (+27% merged), 但 50d ✗ (-69%, sngl ×49)。原因: lp_bits=23 下 weight-3+ keys 大量, chain LP residue → 全 sweep singleton 飙升。详见 [BACKLOG.md V2 案例]。

V3: 借鉴 Cavallar 2000 / msieve clique merge, BFS spanning tree + **LP cancel check** 安全网。

## 算法

```
Input: List of partial relations (1LP + 2LP).
1. 预过滤: 3LP+ 弃 (无法在 V3 一次 spanning tree 内取消所有 LP)
2. 构建 LP→relations 索引 (unordered_map<LargePrimeKey, vector<rel_idx>>)
3. Union-Find 联通分量: 同 component = 共享某 LP key 的关系
4. Per-component BFS 从未用关系起步:
   - Accumulator 初始 = pool[start]
   - 队列展开, 每个 cur 找邻居 nbr (with shared LP key in lp_index)
   - LP cancel check: 仅 accept nbr if merge_two(acc, nbr) 的 LP key count
                     < acc + nbr 的 LP key count
                     ↑ 防 V1/V2 的 chain residue 累积
   - Accept: acc = merged, mark nbr used, continue BFS
   - Reject: skip nbr, 继续找别的 neighbor
   - 直到 acc 是 full relation 或队列空
5. Emit: full ✓; merged-but-residual ✓ (V0 同 convention); 单点弃
```

**关键安全: LP cancel check** (`clique_merger.hpp:184-193`)
```cpp
size_t before = remaining_lp_keys(acc).size() + remaining_lp_keys(pool[nbr]).size();
Relation candidate = merge_two(acc, pool[nbr]);
size_t after = remaining_lp_keys(candidate).size();
if (after >= before) { ++stats.lp_cancel_rejections; continue; }
acc = std::move(candidate); ...
```

不 strictly reduce LP 数的 merge 不被接受 — 避免 V1/V2 trap。

## 文件

| 文件 | 内容 |
|------|------|
| `include/gnfs/relation/clique_merger.hpp` (220 LOC) | `CliqueRelationMerger::merge_cliques()` + `CliqueStats` |
| `tests/test_clique_merger.cpp` (160 LOC) | 6 单元测试 (empty, 1LP×2/×4, 2LP triangle, no_overlap, 3LP_filtered) |

## 集成 (commit 975ac8b + 7f9de82)

ENV-gated `GNFS_CASCADE_V3`。默认 OFF, V0 path 零开销。

**3 个 cascade 入口点** (V0 之后 cascade):
1. `src/api/pipeline.cpp:597-642` — `sieve_and_collect()` 内部 sieve loop merge
2. `src/api/pipeline.cpp:686-745` — `Pipeline::filter()` (Phase 4, 当前 N>100d 才走)
3. `tests/test_stress.cpp:368-418` — stress 50d/60d 测试自己的 sieve loop

**Dedup**: V3 output 可能与 V0 重复 (同一 clique 都被覆盖)。原始筛法行使用完整 `ABPair` 相等性和 `ABPairHash`；哈希碰撞仍由相等比较消解，不再把字段压进一个整数：
```cpp
std::unordered_set<core::ABPair, core::ABPairHash> existing;
if (existing.insert(r.ab()).second) {
    // keep the first exact raw row
}
```

V0/V3 的 merged output 不能沿用 primary `(a,b)`：不同组合可以共享 primary，同一组合也可以由不同 primary/order 物化。兼容路径因此展平 primary 与全部 `extra_ab_pairs`，按 GF(2) 规范化成排序 source combination 后去重。正式结构化约简则使用排序的 immutable source-ID combination；两者都不按 materialized primary 去重。

## 使用

```bash
# 50d V3 cascade
cd build && GNFS_CASCADE_V3=1 ./test_stress 1 1

# 60d V3 cascade
cd build && GNFS_CASCADE_V3=1 ./test_stress 2 2

# 任意 GNFS run (N > 100d 才 hit Pipeline 路径)
GNFS_CASCADE_V3=1 ./gnfs <large_N>
```

stderr 输出:
```
[v3_cascade.sieve] in=12345 full=234 residual=56 lp_rejects=78 added=199
```

`added=` 是 dedup 后实际加进 final relation 列表的数量。

## 预期效果 (理论估算)

50d V0+fix Round 1: Full=0 1LP=6655 2LP=50182 Merged=6786 (V0)

如果 V3 cascade ON:
- pool = 6655 + 50182 = 56837 (1LP+2LP)
- 3LP+ 弃: 假设 0 (test_stress 的 cofactor 不产 3LP+ partials)
- LP graph 联通分量: 50d 估 ~ 6000 components with size≥2
- BFS spanning tree: 估 ~ 6000-10000 additional fulls per round
- 实际值取决于 weight 分布。乐观估计 +30-50% merged。

## 触发条件 (何时启用)

仅以下情况 启用 GNFS_CASCADE_V3=1:

1. V0+fix 50d 在 Round 3 仍 NO EXCESS (matrix rows < cols)
2. V0+fix 60d 跑 > 24h 仍 NO EXCESS
3. 实验性: 比较 V0 vs V0+V3 的 raw→usable 比率

**禁止**: V0 已 PASS 的 size 不启用 V3 (额外开销但无收益)。

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| V3 cascade 引入 invalid relations (LP cancel 弱) | LP cancel check 强约束: after < before 才 accept |
| V0+V3 dedup overhead | 每个 merged row 的 source combination 排序/GF(2) 规范化后做 structural hash lookup；仅索引 merged outputs，并在规模实验中单独计时 |
| V3 stats 不一致 | 集成时记录 `cstats.full_produced + v3_added` (dedup 后) |
| V3 在 small N 略增时间 | 25d e2e 实测 ENV=1 vs ENV=0: 9.9s vs 8.9s (~10% overhead, 可接受) |

## V1/V2 历史 (避免重蹈)

| 版本 | 策略 | 结果 |
|------|------|------|
| V0 | weight=2 only, pairwise merge | 50d/60d NO EXCESS (β=64% 未处理) |
| V1 (早期) | weight=2 + recursive chain | sngl 飙升, 不稳 |
| V2 (commit 21dcbcd, revert 9e84a73) | weight≥2 都 merge 一次 | 25d ✓ +27%, 50d ✗ -69% (chain residue) |
| **V3 (current)** | BFS spanning tree + LP cancel check | 单元测试 6/6 PASS, 50d/60d 真实验证 待 V0+fix 后 |

## 参考

- Cavallar 2000 "Strategies in Filtering in the Number Field Sieve" — clique merge 原始定义
- msieve `filter/clique.c` — production 实现
- CADO-NFS `filter/cliques.cpp` — production 实现 (类似)
- 本仓库 `BACKLOG.md V2 案例` — 详细失败分析
- 本仓库 `progress.md V0+fix 50d 后台验证` — V0+fix vs V3 实测数据 (待补)
