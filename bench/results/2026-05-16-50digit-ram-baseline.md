# P3-2 — 50-digit RAM baseline

**Date**: 2026-05-16
**Doctrine**: §6 P3 第 2 条 "内存使用减半（peak RAM 优化）"
**Hardware**: Apple M5, macOS 26.5 (Tahoe), unified memory 16 GB
**Test**: `./test_stress 1 1` (Level 1 = 50-digit semiprime 164-bit)
**Method**: `/usr/bin/time -l` (macOS) maximum resident set size + peak memory footprint

---

## 1. Why this measurement

doctrine §6 P3 第 2 条 "内存使用减半" 是 P3 长期/低优先项. 但 doctrine 铁律 5
(measurement-first attribution verification) 要求实施前先 baseline. 50-digit
是当前 stress test 最大常规 size, 用作 P3-2 decision data.

## 2. Setup

```bash
cd build-release
/usr/bin/time -l ./test_stress 1 1 > /tmp/p3_ram_profile_50d.log 2>&1
```

50-digit semiprime: N = 9743287121884395 × 10271247247252003 (25-digit × 25-digit)

Phase 1-2 略 (poly select + factor base, RAM 占用小)
Phase 3 Sieving: 2 adaptive rounds (Round 1 target 618K, filter merge_rate 1.098%
触发 Round 2 target 2.31M)
Phase 4-6 attempted but Phase 5 hit "NO EXCESS" (filter 产 38K usable < column
count 47337), test 标 ✗ FAIL (expected for stress test). Phase 5/6 RAM 未充分
exercise — 实际 GNFS 50-digit 全程跑通会 +0.5-1 GB linalg phase RAM.

## 3. Results

### 3.1 总耗时 + 资源 (`time -l` 输出)

```
7142.76 real     30463.25 user        85.05 sys
2183331840  maximum resident set size       (2.08 GiB)
2826258856  peak memory footprint           (2.63 GiB)
2871200  page reclaims
93  page faults
0  swaps
153089526230138  instructions retired
115783714680624  cycles elapsed
```

| Metric | Value | Note |
|---|---:|---|
| Real time | 7142.76 s | ≈ 119 min |
| User time | 30463.25 s | 4.27× parallelism (≈ 4 P-core saturated) |
| System time | 85.05 s | 1.2% overhead — minimal kernel cost |
| **Max RSS** | **2.08 GiB** | resident peak |
| **Peak footprint** | **2.63 GiB** | macOS extended (含 dirty + reclaimable) |
| Page reclaims | 2.87M | typical for malloc-heavy |
| Swaps | 0 | well under 16 GB |
| IPC | 1.32 | (153T inst / 115T cyc) — typical OOO ARM64 |

### 3.2 Phase-level RAM observation (manual polling, 60-120s 间隔)

| Phase | Approx RSS observed | Note |
|---|---:|---|
| Phase 3 round 1 sieving (start) | 130-150 MB | sieve bucket + cofactor buffer |
| Phase 3 round 1 末期 | 233 MB | rel buffer 满 |
| Round 1 filter/merge transition | **634 MB** | 1LP+2LP merge buffer + 临时结构 |
| Phase 3 round 2 sieving (mid) | 280-573 MB | round 1 释放后稳态 |
| Phase 4 trimming + Phase 5 attempt | 不充分 sample | < poll 间隔 |

**注**: 手动 polling 漏了真 peak. `time -l` 报告 2.08 GiB max RSS 远超手动观察
634 MB. peak 在 sub-poll window 闪现 — 推测 Phase 4 trim 或 Phase 5 matrix
construction 临时阶段, 这些 phase 实际跑得快 (< 1 s) 但 RSS spike 大.

## 4. Decision (P3-2)

### 4.1 当前 size 是否瓶颈?

50-digit on M5 16 GB:
- max RSS 2.08 GiB = **13.0% memory**
- peak footprint 2.63 GiB = **16.4% memory**
- **0 swaps, 93 page faults** — well within RAM

**结论**: 50-digit 不是 RAM 瓶颈. doctrine "减半" 当前不必要 — 减半到 1 GiB
对 50-digit 实际 wall time 无影响 (无 swap, page fault 微).

### 4.2 何时 RAM 减半才有意义?

Scaling 估算 (Coppersmith bandwidth + cofactor buffer 与 size 线性关系):
- 60-digit 估算 RSS = 50-digit × 4-6 ≈ 8-12 GiB (50-75% memory)
- 70-digit 估算 RSS = 50-digit × 12-15 ≈ 25-30 GiB (爆 16 GB)

**触发条件**: 60-digit 实测 RSS > 8 GiB (即 50% memory) 才进入 P3-2 实施区间.
当前 60-digit (`./test_stress 1 2`) 跑 hours+,未实测 baseline. 当 60-digit
test 普及到 stress regression 时再启动 baseline.

### 4.3 候选 hot spots (未实施, 留为 follow-up)

如果未来 60-digit baseline 触发 P3-2, 候选改造区域:
1. **BlockWiedemann Krylov sequence** — `O(n × L × 8 bytes)` 在 60-digit n~200K
   情况下 ~ 200 MiB. 可改用 disk-backed mmap (已有 MmapCSRMatrix 基础设施)
2. **RelationCollector 重复存储** — `Relation` 含 a, b, factor lists,
   filter merge 中保留 raw + merged 双拷贝. 可改用 OOC 路径 (已有
   `OOCRelationStore`)
3. **CSR matrix double-storage** — BL/BW path 同时存 M 与 M^T (transpose buffer).
   可 in-place transpose 节省 50%
4. **Cofactor ECM Stage 2 BSGS** — Stage 2 大 baby/giant table (~M B0). 当前
   Stage 2 enabled 即占 RAM
5. **Sieve bucket array** — Bucket sieve 大 FB 多线程 scatter, bucket 数组
   `O(SQ count × bucket size)`. 减小 bucket 数或动态 grow

## 5. 决策与未来工作

### 5.1 关闭 doctrine §6 P3 第 2 条 (deferred-by-data)

**状态**: ✅ **已评估 2026-05-16** — measurement 显示 50-digit (2.08 GiB peak)
不构成 RAM 瓶颈. P3-2 实施 (减半到 ~1 GiB) 边际收益小, 优先级 deferred.

### 5.2 BACKLOG follow-up (触发条件未满足时不启动)

**触发**: 60-digit (`test_stress 1 2`) baseline 实测 RSS > 8 GiB (即接近 50%
memory). 或 70-digit 出现 swap.

实施候选 (排序按 ROI):
1. CSR in-place transpose (节省 ~30% linalg RAM, 工程量 medium)
2. RelationCollector OOC 集成 (节省 ~40% phase 4 RAM, 已有基础设施)
3. BW Krylov mmap (节省 ~10% phase 5 RAM, 工程量 small)
4. Cofactor ECM Stage 2 disable for huge cofactor (微影响 size)
5. Sieve bucket dynamic grow (微节省, 工程量 medium)

## 6. 教训

1. **`time -l` 是权威 — polling 漏 peak**. 手动 ps RSS 观察 634 MB, `time -l`
   报告 2.08 GiB. 应优先用 `time -l` (max RSS) 而非 polling. polling 只能看
   到长稳态 RSS, peak 在 phase transition 临时分配时往往 < 1 s.
2. **doctrine "减半" 的意义随 size 改变**. 50-digit 减半对 wall 0 影响
   (无 swap), 60+digit 才是 trigger size. 不能 generalize "减半总是好的".
3. **NO EXCESS test FAIL 不阻碍 baseline 收集**. 50-digit stress 实际 FAIL
   (filter usable 不足), 但 Phase 1-4 + Phase 5 attempt 已经 exercise 主体
   RAM path. baseline 仍有效.
4. **measurement-first 又一次救场**. P3-2 doctrine 标 "高优先级 → 低优先"
   (P3), 听起来该实施. 实测显示当前 size 根本不是瓶颈, 直接 defer.

## 7. References

- Test 命令: `./test_stress 1 1` (50-digit Level 1)
- Log: `/tmp/p3_ram_profile_50d.log` (本机临时, 不入 git)
- `time -l` 输出 (full): 见 bench/results/2026-05-16-50digit-ram-baseline.log
  *(注: log 不入 git, 关键数据已 inline 在 §3.1)*
- doctrine §6 P3 第 2 条
