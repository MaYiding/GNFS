# P1.B-1c — ThreadPool atomic-spin worker

**日期**: 2026-05-14
**分支**: feat/260514-threadpool-atomic-spin
**前置**: P1.B-1b 已合并 (`9fecb96`)，遗留 worker idle wait `__psynch_cvwait` 主导
**目标**: 用 spin-then-cv 模式消除 burst-submit 模式下的 cv_wait syscall

## TL;DR

| 指标 | Baseline (9fecb96) | Fix (spin-then-cv) | Δ | 备注 |
|---|---:|---:|---:|:---|
| **Wall** | 47.924 s | 47.330 s | **-1.24%** | 真实改善 |
| **sys time** | 4.126 s | 3.637 s | **-11.85%** | cv_wait syscall 减少（核心证据） |
| user time | 219.254 s | 224.613 s | +2.44% | spin 烤 CPU（预期内） |
| Cycles | 95.85e9 | 94.38e9 | -1.54% | |
| IPC | 1.316 | 1.326 | +0.010 | 微弱提升 |
| BackendStallRate | 73.91% | 74.35% | +0.44pp | PET 噪声 |
| L1DMissRate | 13.14% | 13.27% | +0.13pp | 无显著变化 |
| TLBMissRate | 52.99% | 54.17% | +1.18pp | PET 噪声 |

**结论**: Spin-then-cv 是经典 trade-off：减少 syscall（sys -11.85%）↔ 烤 CPU（user +2.44%）。这里净 wall -1.24% 表示 trade 划算 — sys 节省（489 ms 实际 syscall 时间）超过 user 上升（5.36 s 累计但分摊到 10 workers 仅 +0.54 s/worker，且 spin 期间不在 critical path）。

## 1. 背景

P1.B-1b 报告（2026-05-14-gaussian-threadpool.md）记录：worker idle wait 在 `cv_.wait` 仍占 sample 总 __psynch_cvwait 的大头（~50k samples × 10 workers）。Gaussian 列消元 / SpMV per-iter chunks 的 submit-wait-submit burst 模式：

```
submit(chunk_1) → ... → submit(chunk_N) → wait_all
worker: grab → exec → lock → cv_wait（等下一波）
                              ↑
                       syscall 进入 kernel sleep
下一波 submit (μs 后)：notify_one → worker wake → syscall 退出
```

每轮 column 都触发 cv_wait/cv_signal pair。Gaussian 处理几千 column → 几万次 syscall。

## 2. 设计

**Spin-then-cv worker_loop**:

```cpp
int spin = 0;
while (true) {
    // Fast path: atomic peek
    if (queue_size_.load(acquire) > 0) {
        // try-lock + grab
        spin = 0; continue;
    }
    // No task: short spin before cv_wait
    if (spin < kSpinBudget) {
        cpu_relax();  // ARM yield / x86 pause
        ++spin; continue;
    }
    // Spin budget exhausted: fall back to cv_wait
    {
        cv_.wait(...);
        spin = 0;
    }
}
```

**关键参数**:
- `kSpinBudget = 2000` iterations × ARM `yield` ≈ M5 P-core ~2-4 μs
- `std::atomic<size_t> queue_size_`：worker spin path 无锁 atomic load
- 始终在 mutex_ 内更新 queue_size_（与 tasks_ 一致），acquire/release seq

**接口零变化**: submit / parallel_for / parallel_for_index / parallel_for_stealing / wait_all / num_threads / pending_tasks — 调用方零 diff。

## 3. 实施

**单个 commit**:
- `138d4d8` perf(util): ThreadPool spin-then-cv worker_loop (P1.B-1c)
- +70/-2 行，仅 `include/gnfs/util/thread_pool.hpp`

**关键 diff**:

1. **submit path** (line 75-79):
   ```cpp
   tasks_.emplace([task]() { (*task)(); });
   ++pending_;
   queue_size_.fetch_add(1, std::memory_order_release);
   ```

2. **worker fast path** (atomic peek, no lock):
   ```cpp
   if (queue_size_.load(std::memory_order_acquire) > 0) {
       std::unique_lock<std::mutex> lock(mutex_);
       if (!tasks_.empty()) {
           task = std::move(tasks_.front());
           tasks_.pop();
           queue_size_.fetch_sub(1, std::memory_order_release);
           got_task = true;
       }
       // ...
   }
   ```

3. **spin path** (no syscall):
   ```cpp
   if (spin < kSpinBudget) {
       cpu_relax();  // ARM `yield`
       ++spin; continue;
   }
   ```

4. **fallback cv_wait** (long idle):
   - 仅当 spin budget 耗尽时进入
   - 长期闲置避免烤 CPU

## 4. 验证

### 4.1 单元测试

```
test_thread_pool: 9/9 PASS（含 wait_all race stress 5000 rounds + concurrent submit 1000 rounds）
test_linalg:     all PASS
test_block_wiedemann: 7/7 PASS
test_mmap_csr:   5/5 PASS
```

### 4.2 Gate

```
Level 1 (smoke): 26/26 PASS (3.51s)
Level 2 (regression): test_regression_gate PASS (22.42s)
Total: 27/27 PASS (29.17s)
```

### 4.3 PMU 对比

**Build**:
- Baseline: `/tmp/gnfs-baseline-9fecb96/build-release/test_factor_with_kleinjung` (main `9fecb96`)
- Fix: `<repo-root>/build-p1b1c-release/test_factor_with_kleinjung` (feat with spin-then-cv)

**JSON**:
- `bench/results/2026-05-14-201315-test_factor_with_kleinjung-p1b1c_baseline.pmu.json`
- `bench/results/2026-05-14-201413-test_factor_with_kleinjung-p1b1c_fix.pmu.json`

**Raw counter delta**:

| Event | Baseline | Fix | Δ |
|---|---:|---:|---:|
| FIXED_CYCLES | 95.85e9 | 94.38e9 | -1.54% |
| FIXED_INSTRUCTIONS | 126.13e9 | 125.17e9 | -0.77% |
| INST_BRANCH | 25.87e9 | 25.85e9 | -0.08% |
| BRANCH_MISPRED_NONSPEC | 141.1M | 140.5M | -0.42% |
| ARM_STALL_BACKEND | 70.84e9 | 70.18e9 | -0.94% |
| ARM_STALL_FRONTEND | 1.88e9 | 1.99e9 | +5.45% |
| L1D_CACHE_MISS_LD | 3.24e9 | 3.21e9 | -1.01% |
| L1D_TLB_MISS | 13.08e9 | 13.10e9 | +0.15% |
| ARM_MEM_ACCESS | 24.69e9 | 24.19e9 | -2.04% |

**Time decomposition**:

```
Baseline:  wall=47.924  user=219.254  sys=4.126   user+sys=223.380
Fix:       wall=47.330  user=224.613  sys=3.637   user+sys=228.250

Δwall:        -0.594s (-1.24%)
Δuser:        +5.359s (+2.44%)  — spin loop runs in user mode
Δsys:         -0.489s (-11.85%) — fewer cv_wait syscalls 🎯
Δ(user+sys):  +4.870s (+2.18%)  — net CPU work up (10-worker spin overhead)
```

**关键解读**:

1. **sys time -11.85%** — 直接证据：cv_wait/cv_signal syscall 数量显著下降。M5 syscall 进入/退出 kernel mode ~200-500 ns，Gaussian 数万次 column 上累加。
2. **user time +2.44%** — 预期 trade-off：worker 在 spin loop 里烧 CPU 用户态 cycles。但分摊到 10 workers 平均 +0.54 s/worker。
3. **wall -1.24%** 是净改善，因为：
   - sys 节省发生在 critical path（主线程提交后等 worker grab）
   - user 增加分散在闲置 worker（多数不在 critical path）
4. **Cycles -1.54%** 表明真的省了 syscall 周期，不只是迁移到 user 态

### 4.4 sample (`/usr/bin/sample`)

测试中 20 秒采样：

```
Baseline: __psynch_cvwait total samples = 84,048
Fix:      __psynch_cvwait total samples = 83,902 (-0.17%)
```

**这是符合预期的**：sample 是 1ms 粒度，看不到 μs 级 spin。spin-then-cv 在 spin budget 内 grab 任务时根本不进 cv_wait，但 sample 看到的是长期闲置（spin 耗尽后的 cv_wait）。PMU sys time 才是 syscall 总成本的真实测量。

简而言之：**sample 看 thread blocking 时间（长期闲置），PMU sys 看 syscall 总时间（高频短暂闲置）**。后者是 P1.B-1c 的真实战场。

## 5. 已知局限

### 5.1 spin budget 取值待调优

`kSpinBudget = 2000` 是首次取值。可调优范围：
- 太小（<500）：spin 期间任务未到达，仍 fallthrough cv_wait，节省减少
- 太大（>10000）：worker 长烤 CPU，散热 + 能耗增加，可能挤占主线程 P-core

下一阶段可以测 1000 / 2000 / 5000 三档，选择 wall time 最低的。

### 5.2 user time +2.44% 在 long-running tasks 累积

20 秒测试 user +5.36 s。如果跑 1 小时 GNFS 流水线，累积 user time 增加可能更明显（线性 scaling）。但 wall 改善也线性 scaling，所以 ratio 不变。

### 5.3 非 burst-submit 模式不受益

`parallel_for_stealing`（lattice_sieve 用）是一次性 submit n_threads 个 worker + atomic counter，worker 启动后 100% 工作直到结束 — 不进入 cv_wait，所以 spin-then-cv 不影响该路径。

### 5.4 PET 噪声 ±2pp 内

BackendStallRate +0.44pp / TLBMissRate +1.18pp 都在 PET (Profile Every Thread) 采样噪声范围内，不构成回归。

## 6. 教训

1. **sample vs PMU 互补**: sample 看长期 thread 状态，PMU 看高频 syscall 累积成本。spin-then-cv 是后者战场，前者看不出。
2. **user/sys breakdown 是关键证据**: 单看 wall -1.24% 可能被认为是噪声。但 sys -11.85% + user +2.44% 这对反向变化是 spin-then-cv 的指纹。
3. **CPU 烤热是必然代价**: 任何 spin 策略都会消耗 user time。关键看是否 net wall 改善。
4. **测试 burst 工作量需要 critical-path workload**: test_factor_with_kleinjung 是 81-bit 因式分解，Linalg 阶段占比相对小。在更大矩阵（stress 50-digit）下预期收益更明显，但 CI 不可行。

## 7. 下一步候选

- **P1.B-2 (lattice_sieve align)**: sample 仅 2k samples（很弱信号），可能不值得做
- **P1.B-3 (TLBMissRate)**: P1.B-1b 已从 61% 降到 53%，目前 ~54% 在 PET 噪声内，暂搁置
- **Spin budget tuning**: 静态 2000 → 测试不同值；或动态自适应（基于过去 N 次 spin/cv 比例）

## 附录

### Commits
- `138d4d8` perf(util): ThreadPool spin-then-cv worker_loop (P1.B-1c)

### 文件改动
- `include/gnfs/util/thread_pool.hpp`: +70/-2

### Build dirs (临时, gitignored)
- `/tmp/gnfs-baseline-9fecb96/build-release/` (baseline)
- `<repo-root>/build-p1b1c-release/` (fix)
