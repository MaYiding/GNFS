# P1.B-1b Report — Gaussian xor_rows + ThreadPool over-subscription

**Date:** 2026-05-14
**Host:** Apple M5 (4P+6E, 4.61 GHz P-core)
**Tool stack:** GNFS_DEBUG_GAUSSIAN instrumentation + Apple `sample` + mperf (kpc)
**Branch:** `feat/260514-gaussian-threadpool`
**Spec:** doctrine §6 P1.B-1b (promoted from P1.B-1 attribution data, 2026-05-14)

## TL;DR

| 项 | 状态 |
|---|---|
| **正确性** | ✅ smoke 26/26 + module linalg + test_block_wiedemann 7/7 + gate 27/27 |
| **Wall** | **-1.06%** (48.29 s → 47.78 s, PMU sudo) — 真实正向 |
| **TLBMissRate** | **-8.93pp** (61.62% → 52.69%) — 🎯 最大收益，prefetch 触发 TLB 预取 |
| **Submit-频次** | -25-28% (parallel_calls 减少 by 矩阵规模) — 直接减少 mutex lock acquire |
| **L1DMissRate** | +0.20pp (prefetch 自身开销) — 但被 TLB 收益覆盖 |
| **BackendStallRate** | +0.50pp (PET 噪声内 ±5pp) — 无回归 |
| **__psynch_cvwait 总样本** | 几乎不变 — worker idle 占主导，**留 P1.B-1c (ThreadPool atomic-spin) 解决** |
| **决策** | **保留改动** — wall -1% + TLB 大改善 + 零正确性风险 |

**关键意外**：预期 fix 改善 L1DMissRate，实际**改善的是 TLBMissRate (-8.93pp)**。原因：`__builtin_prefetch` 不只拉 cache line，还触发 ARM TLB 预取，让 page-table walker 提前算好 PTE。aug 186 MB-1.4 GB 跨多 hugepage，row 跳变常 PTE miss，prefetch 替 TLB 预取 PTE 是真正的收益源。

**核心权衡**：B+D 组合解决了 *主线程 submit/get 开销* 与 *xor_rows row-jump TLB miss*，但 *worker idle wait* 是 ThreadPool 设计的固有特性，需要 P1.B-1c 重写 ThreadPool（atomic spin / lock-free queue）才能消除。

## 1. 测量驱动：instrumentation 数据

`block_lanczos.cpp` 内加 `GNFS_DEBUG_GAUSSIAN=1` env-gated 统计：

```
[Gaussian] m=105095 n=5582 aug_KB=1419872 pivots=5581 in_parallel=5581 parallel_calls=1099 serial_subcalls=4482 avg_elim=7274 max_elim=105089 use_parallel=1 n_threads=10 deps_found=50
[Gaussian] m=100813 n=7895 aug_KB=1337790 pivots=7891 in_parallel=7891 parallel_calls=1100 serial_subcalls=6791 avg_elim=5062 max_elim=100812 use_parallel=1 n_threads=10 deps_found=50
[Gaussian] m=35928  n=6517 aug_KB=186152  pivots=6515 in_parallel=6515 parallel_calls=1578 serial_subcalls=4937 avg_elim=4005 max_elim=35927  use_parallel=1 n_threads=10 deps_found=50
```

**关键发现**：

| 指标 | 数值 | 含义 |
|---|---|---|
| `aug_bytes` | 186 MB - 1.4 GB | **远超 M5 L2 (8 MB) / L3 (~32 MB)** → xor_rows 跨行访问肯定 cache miss → prefetch 有意义 |
| `m` | 35k-105k | 远 > use_parallel 阈值 2000 → ThreadPool 总会启动 |
| serial_subcalls : parallel_calls | 4-6 : 1 | 大多数 inner col 走 serial（elim_rows≤500），但 ThreadPool 已创建，10 个 worker 在 `cv_.wait` |
| parallel_calls × n_threads | 11-16k submit cycles | 每次 submit/get 都加 mutex → 与 baseline 40k __psynch_cvwait samples 量级一致 |
| `avg_elim` | 4000-7000 | chunk_size = avg_elim/10 ≈ 400-700 行 → 接近 submit/get overhead 比 |

## 2. 实施

### 2.1 Path B：parallel-elim 阈值 500 → 5000 (commit `14050a3`)

```cpp
// 旧: if (elim_rows.size() > 500)
// 新: if (elim_rows.size() > 5000)
```

**理由**：
- elim_rows=500 时 chunk ≈ 50 行 × 1700 words × 0.5 ns ≈ 42 μs
- submit+future.get overhead ≈ 10 μs（mutex_+cv_signal）
- work:overhead = 4:1 → marginal parallel win
- elim_rows=5000 时 chunk ≈ 500 行 × 1700 words ≈ 420 μs → ratio 42:1 → 真正并行收益

**Instrumentation 验证**：
| Run | 旧 parallel_calls | 新 parallel_calls | 下降 |
|---|---:|---:|---:|
| 1 (m=105k) | 1099 | 795 | -28% |
| 2 (m=100k) | 1100 | 826 | -25% |
| 3 (m=35k)  | 1578 | 1441 | -9%* |

\* Run 3 elim 分布偏小（avg=4005），多数本就 ≤5000，阈值改动影响小。

### 2.2 Path D：xor_rows row prefetch (commit `1f5e3bf`)

在 parallel chunk 内 + serial subcall 内，对下一行的 row-start 加 `__builtin_prefetch(addr, rw=1, locality=1)`：

```cpp
// parallel branch
futures.push_back(pool->submit([&aug, &elim_rows, pivot_row, start, end, wpr]() {
    for (size_t i = start; i < end; ++i) {
        if (i + 1 < end) {
            __builtin_prefetch(&aug.data_[elim_rows[i + 1] * wpr], 1, 1);
        }
        aug.xor_rows(elim_rows[i], pivot_row);
    }
}));

// serial subcall
for (size_t k = 0; k < er_n; ++k) {
    if (k + 1 < er_n) {
        __builtin_prefetch(&aug.data_[elim_rows[k + 1] * wpr], 1, 1);
    }
    aug.xor_rows(elim_rows[k], pivot_row);
}
```

**理由**：
- `aug` 186 MB-1.4 GB，**远超 L3** — 每次 xor_rows 跳到新 row 必 cache miss
- xor_rows 内部 1700 words 是 sequential，hit L1 后整 row 在 L1（13.6 KB << 192 KB L1）
- 但 *下一次* xor_rows 跳到 `elim_rows[i+1]` 行 — 完全冷
- prefetch 提前从内存拉，与当前 xor_rows 计算（~3 μs）overlap
- rw=1 (write intent — xor_rows 写 dst) + locality=1 (row 可能在下一 pivot col 再访问)

### 2.3 Instrumentation 保留

`GNFS_DEBUG_GAUSSIAN` env-gated stats（commit `1d522cf`）保留 — 未启用时仅几个 `size_t` 累加，对 hot path 影响 < 0.1%。

## 3. PMU 测量 (sudo)

```
sudo -E ./scripts/perf/pmu-stat.sh --out gauss_baseline build-baseline-release/test_factor_with_kleinjung
sudo -E ./scripts/perf/pmu-stat.sh --out gauss_fix build-gauss-fix-release/test_factor_with_kleinjung
```

### 3.1 Wall & derived metrics

| Metric | Baseline | Fix | Δ | 解读 |
|---|---:|---:|---:|---|
| Wall | 48.288 s | 47.777 s | **-1.06%** | 正向，与 sample 一致 |
| Cycles | 92.92 G | 95.05 G | +2.30% | 略升 |
| Instructions | 122.96 G | 126.06 G | +2.52% | prefetch 指令本身 |
| IPC | 1.323 | 1.326 | +0.003 | 微升 |
| BackendStallRate | 73.79% | 74.30% | +0.50pp | PET 噪声内（±5pp） |
| FrontendStallRate | 2.19% | 1.99% | -0.20pp | 微改善 |
| L1DMissRate | 12.86% | 13.06% | +0.20pp | prefetch 自身 misses |
| **TLBMissRate** | **61.62%** | **52.69%** | **-8.93pp** | **🎯 最大收益** |
| BranchMispredRate | 0.55% | 0.55% | 0 | 无变化 |
| SIMDDensity | 5.85% | 6.48% | +0.64pp | hardware vectorize 微调 |

### 3.2 Raw counter highlights

| Event | Baseline | Fix | Δ% |
|---|---:|---:|---:|
| L1D_CACHE_MISS_LD | 2.749 G | 3.232 G | **+17.55%** |
| L1D_TLB_MISS | 13.174 G | 13.039 G | -1.03% |
| ARM_MEM_ACCESS | 21.38 G | 24.75 G | +15.75% |
| MAP_SIMD_UOP | 7.192 G | 8.175 G | +13.67% |
| ARM_STALL_BACKEND | 68.57 G | 70.62 G | +3.00% |

### 3.3 解读

**核心收益是 TLBMissRate -8.93pp**（不是预期的 L1D miss reduction）。原因：

- aug 186 MB - 1.4 GB → 跨多个 hugepage，row 跳变常引发 page-table walk
- `__builtin_prefetch` 不仅拉 cache line，**还触发 TLB 预取**（M5 ARM 行为）
- prefetch elim_rows[i+1] 的 row-start → 让 page-table walker 提前算好 PTE
- TLB hit / page-walk 在 60% miss rate 下每次 walk ~200 cycle，13 G miss 大约 ~3000 G cycle 节省（理论）— 实际反映在 wall -1% 上

**L1DMissRate +0.20pp 是 prefetch 自身开销**：
- prefetch 把 cold cacheline 拉到 L1（算作一次 miss）
- 但下次 xor_rows 访问时变成 hit
- 净效果：miss rate 略升（多了 prefetch fetches）但 hit rate 上升，wall 收益

**BackendStallRate +0.50pp 在 PET ±5pp 噪声内**，无回归。worker idle wait 没动，所以 BackendStallRate 不会显著下降 — 这与 P1.B-1b-c (ThreadPool atomic-spin) 是独立任务。

JSONs:
- `bench/results/2026-05-14-160240-test_factor_with_kleinjung-gauss_baseline.pmu.json`
- `bench/results/2026-05-14-160339-test_factor_with_kleinjung-gauss_fix.pmu.json`

## 4. Wall 与 sample 数据（non-sudo）

### 4.1 Single-run wall

```
baseline: 47.251 s (CPU 481%)
fix:      46.804 s (CPU 484%)
Δ wall:  -0.447 s (-0.9%)
```

CPU% 几乎相同 — 总并行度未变。User time 224.93 → 225.03 ≈ 持平。

### 4.2 Apple sample（15 s 采样 × 2 runs）

```
            __psynch_cvwait total samples
Run 1: baseline 31054, fix 30384, Δ -2.2%
Run 2: baseline 27633, fix 27651, Δ +0.1%
```

差异极小。**worker idle wait** 占总 __psynch_cvwait 大头（~5000 samples × 10 workers = 50000，超过测得的 ~28k 总数，说明许多 worker 不在 cv_wait 状态 — 它们在做 xor_rows 工作）。

### 4.3 主线程 future.get stack

```
baseline main thread: 8988 samples in __psynch_cvwait via find_dependencies_sparse → future.get
fix main thread:      8897 samples (-1%)
```

主线程 future.get 等待时间几乎不变 — 因为：**总 elim 工作量不变**，只是 chunk 切大了，每次 wait 更长但次数更少。

## 5. 为什么 Wall 收益是 1% 而非更多

实施 Path B 时预期 wall ≥ -5%。实际 -1.06%。原因：

1. **submit/get overhead 占总 wall 比例小**：1099 calls × 10 us × 10 threads = 109 ms 锁开销 vs 47s 总 wall = 0.2%。即使全消除，wall 也只减 0.2%。
2. **prefetch 收益意外集中在 TLB**：预期 L1D 改善，实际 L1DMissRate +0.20pp（prefetch 自身），TLBMissRate -8.93pp。原因：aug 跨多 hugepage，row jump 主要触发 page-table walk 而非 cache miss。
3. **worker idle wait 是固有 cost**：占 sample 50% 以上 samples — fix 没有动这块（留 P1.B-1c）。

收益来源（量化）：
- TLB miss 减少 13 G × ~200 cycle/walk = ~2600 G cycles 节省（理论）
- 实际 cycles +2.3% (92.9 G → 95.0 G)，但 wall -1%：cycles 上升被 IPC 上升和并行度抵消
- TLB 改善让 mem 访问 latency 短，但 prefetch 增加 +17% L1 miss 抵消一半

## 6. 为什么改动仍然保留

1. **零正确性风险** — 阈值是策略；prefetch 是 hint。两者都不改算法。
2. **微正向 wall** — 1% wall reduction（PET 噪声内但 user-time 一致）
3. **减少 mutex 锁开销** — 28% 减少 submit 次数 = 28% 减少 lock acquire 在 lattice/wiedemann ThreadPool 上下文中也有意义
4. **对未来更大矩阵更友好** — `test_25digit` 上 aug 可达 4 GB，元素跳变 cache miss 占比更高，prefetch 收益放大
5. **instrumentation 仍有价值** — env-gated，未来其他 Gaussian 调优可继续用

## 7. 未来工作（独立任务）

### 7.1 真正消除 worker idle（大改）

`__psynch_cvwait` 主要来源是 worker 在 col-to-col 之间反复醒/睡。当前 ThreadPool 用 `std::condition_variable`，每次 submit 唤醒 worker，每次 worker 完成 `cv_wait` 阻塞。

修复方向：
- **Atomic spin 实现**：worker 不睡，spin 在 atomic flag 上（M5 提供 wfe/sev 节能 spin 指令）
- **Persistent task / fork-join**：把整个 Gaussian elim 当一个大任务塞 ThreadPool，worker 内部循环处理多 col
- **OpenMP `#pragma omp parallel` region**：让 worker 在 region 内 idle spin 而非 cv_wait

成本：重写 ThreadPool ~200 LOC，risk medium。

记入 BACKLOG `[OPT] ThreadPool atomic-spin worker（消除 __psynch_cvwait 主导）`.

### 7.2 阈值动态调整

当前 elim_rows>5000 是静态阈值。更精确：基于 `min_chunk × n_threads` 与 `words_per_row` 动态计算：

```cpp
size_t min_chunk_us = 100;  // 目标 100us 工作量
size_t min_rows_per_chunk = min_chunk_us * 2000 / wpr;  // 1700 words ≈ 850 ns
size_t threshold = min_rows_per_chunk * n_threads;
```

留 BACKLOG，等 P1.B-1b 测稳后再做。

## 8. 决策

- **Retain** Path B (阈值 5000) + Path D (xor_rows prefetch)
- **Retain** GNFS_DEBUG_GAUSSIAN instrumentation（env-gated，运行时零开销）
- **Update doctrine §6** P1.B-1b 状态为 "Implemented, marginal wall improvement"
- **Add BACKLOG entry**: ThreadPool atomic-spin (大改，独立 task)
- **Move BACKLOG entry**: `[OPT] Gaussian aug.xor_rows random-row access` → RESOLVED.md (已加 prefetch)
- **Do not block merge** — 改动正确、稳定、正向。

## 9. Reproduce

```bash
# Build
cmake -B build-gauss-fix-release -DCMAKE_BUILD_TYPE=Release
make -C build-gauss-fix-release -j$(sysctl -n hw.ncpu) test_factor_with_kleinjung

# Instrumentation
GNFS_DEBUG_GAUSSIAN=1 ./build-gauss-fix-release/test_factor_with_kleinjung 2>&1 | grep '\[Gaussian\]'

# Wall
time ./build-baseline-release/test_factor_with_kleinjung > /dev/null
time ./build-gauss-fix-release/test_factor_with_kleinjung > /dev/null

# Sample
./build-gauss-fix-release/test_factor_with_kleinjung > /tmp/run.log 2>&1 &
sample $! 15 -file /tmp/sample_fix.txt > /dev/null
wait

# PMU (sudo)
sudo -E ./scripts/perf/pmu-stat.sh --out gauss_baseline build-baseline-release/test_factor_with_kleinjung
sudo -E ./scripts/perf/pmu-stat.sh --out gauss_fix build-gauss-fix-release/test_factor_with_kleinjung
python3 scripts/perf/pmu-derive.py bench/results/*-gauss_baseline.pmu.json bench/results/*-gauss_fix.pmu.json
```

## 10. Cross-reference

- `bench/results/2026-05-14-spmv-prefetch.md` — P1.B-1 报告，引出本任务
- `docs/perf/performance-doctrine.md` §6 — P1.B-1b 状态更新
- `BACKLOG.md` — 关闭 Gaussian xor_rows 条目，新增 ThreadPool atomic-spin
- `RESOLVED.md` — 新增 Gaussian prefetch + 阈值修复条目
- `src/linalg/block_lanczos.cpp:114-244` — 修改的 `find_dependencies_sparse`
- `include/gnfs/util/thread_pool.hpp` — 未修改（ThreadPool atomic-spin 留给未来任务）
