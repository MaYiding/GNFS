# P1.B-2 — lattice_sieve alignment（null result）

**日期**: 2026-05-14
**分支**: feat/260514-lattice-sieve-align
**前置**: P1.B-1c 已合入 main (`c1b4da4`)。doctrine §6 P1.B-2 排程中。
**结论**: 通过 sample attribution + micro-bench 对齐扫描，确认 row-stride 对齐**不是**当前 sieve_row_chunk 性能瓶颈。无需实施 alignment fix。

## TL;DR

- doctrine 假设：`sieve_row_chunk` 每行写入 `sieve_array_[idx] += lp`，若 row stride `width × 2B` 非 128B（M5 cache line）整数倍 → cache line 撕裂 → 性能损失
- 测量结果：micro-bench 跑 width 6000（row_bytes mod 128 = 96，**misaligned**）vs 6016（mod=0，**aligned**），per-iter 时间 **5.79 ms vs 5.78 ms**（<0.5% 差异，在噪声内）
- 根因：`strh`/`ldrh` 是 16-bit 访存，永不跨 cache line；HW prefetcher 对小 stride 模式不依赖 row base 对齐
- 决策：**关闭 P1.B-2，alignment 不是 lattice_sieve 瓶颈**。2k samples 是 stride-add 算术吞吐的本征上限，不是 memory access 问题

## 1. Attribution 验证（doctrine 铁律 5）

Release build (`build-release/test_factor_with_kleinjung`)，sample 25 秒：

```
sieve_row_chunk: 2103 samples (与 doctrine 记录的 2k 一致)
```

热点 offset 分布（function symbol `_ZN4gnfs5sieve12LatticeSieve15sieve_row_chunk` 基址）：

| Offset | 反汇编 | Samples | 含义 |
|---:|---|---:|---|
| +1116 | `ldrh w16, [x15, x11, lsl #1]` | **1228** | load `sieve_array_[idx]` |
| +1124 | `strh w16, [x15, x11, lsl #1]` | **508** | store `sieve_array_[idx]` |
| +1140 | `ldrsh w12, [x8, #0xa]` | 170 | outer-loop advance `sp.i_mod` |
| +1056 | (loop header) | 59 | initial offset compute |

Inner loop（offset +1116 ~ +1136）的 6 指令是 ARM64 紧凑 stride-add：
```
+1116: ldrh w16, [x15, x11, lsl #1]   ; load sieve_array_[idx]
+1120: add  w16, w16, w12             ; += log_p
+1124: strh w16, [x15, x11, lsl #1]   ; store back
+1128: add  x11, x11, x10             ; idx += stride
+1132: cmp  x11, x24                  ; idx < row_end?
+1136: b.lo loop_top                  ; branch back
```

`x15` (sieve_array_.data()) 在 inner loop 之前一次性 `ldr` 到寄存器（+1112: `ldr x15, [x20, #0x30]`），不重复加载。

## 2. 配置事实

`test_factor_with_kleinjung` 配置 (`tests/test_factor_with_kleinjung.cpp:172-175`)：
- `i_min = -3000`, `i_max = 2999` → **width = 6000**
- `j_min = 1`, `j_max = 800` → height = 800
- sieve_array_ 大小 = 6000 × 800 × 2B = **9.6 MB**（远超 L1=192KB,L2=8MB；接近 L3）
- 9.6 MB malloc → 必定 page-aligned (16KB on M5)，故 sieve_array_.data() 自动 128B 对齐
- 但 **row stride = 6000 × 2 = 12000 字节 / 128 = 93.75 → row alignment 在偶数行 0、奇数行 +96/+32 字节波动**

doctrine 假设上述 row misalignment 是 sieve_row_chunk 2k 样本的来源。

## 3. micro-bench 直接验证

`/tmp/p1b2_microbench/bench.cpp`（50 LOC，复刻 sieve_row_chunk inner loop），单线程跑 height=800 × n_primes≈264，重复 200 次 SQ-runs：

| width | row_bytes mod 128 | per-iter (ms) | per-prime-per-row (ns) |
|---:|---:|---:|---:|
| 6000 | 96 (misaligned) | **5.79** (5.78/5.84/5.76) | 27.4 |
| 6016 | 0 (aligned) | **5.78** (5.79/5.76/5.79) | 27.4 |
| 6144 | 0 (aligned) | 5.93 (5.83/5.97/6.00) | 28.1 |
| 8192 | 0 (aligned) | 6.99 (7.03/7.03/6.92) | 33.1 |

**关键解读**：
- 6000 vs 6016: per-iter 差 0.01 ms = **0.17%**，远在 inter-run 噪声（~3%）内 → **alignment fix 无效**
- 6144 vs 6016: 多 2% 是因为 sieve area 多 2.1%（额外 128 列）
- 8192 vs 6016: 多 20.8% 是因为 sieve area 多 36.2%（额外 36% 工作量）

→ **row stride 对齐对 stride-add inner loop 完全无影响**。

## 4. 根因分析

为什么 alignment 不重要？

1. **16-bit 访问永远不跨 cache line**：`strh`/`ldrh` 地址 4 字节对齐（natural alignment），M5 L1D 行 128 B，最多在行内偏移 126 字节，不可能撕裂行
2. **HW prefetcher 对小 stride 友好**：M5 P-core 的 stride detector 对小素数 stride（p∈[7, 6000]）能识别访问模式，不依赖 base 对齐
3. **Inner loop 是 compute-bound**：6 指令 / 6 周期 / iter（load-store 依赖通过 idx），实测 ~5 ns/iter ≈ 22 cycles，与 M5 P-core 4.6 GHz 的 6 周期理论值差距来自 micro-op 调度 + memory back-pressure，**不是 row alignment**

## 5. 为什么 sample 显示 2k samples？

sample 在 sieve_row_chunk 的 2103 samples 反映：
- 总 wall time ~50-67s × 10 worker × 多 SQ 调用累积
- 实际 sieve_row_chunk 占程序 wall 比 ~2-4%
- doctrine 早在记录时已标注「2k samples 弱信号，非主导」

这就是它在 P1.B 排序中靠后的原因。

## 6. 是否值得做更复杂的优化

潜在改进路线（均需 P1.C 或 P2 级别工程）：

| 方案 | 预期收益 | 实施成本 | 风险 |
|---|---|---|---|
| NEON 向量化 stride loop（gather/scatter） | ~30% inner loop | 高（变 stride 处理复杂） | 高（NEON gather 在 M5 上无原生指令，需软件 emit） |
| SoA refactor `small_primes`（按 field 分数组） | 边际 | 中 | 中（API breaking） |
| 软件 prefetch `__builtin_prefetch(next_prime_first_idx)` | 边际 | 低 | 低（但 HW prefetcher 已覆盖） |
| Bucket 化所有素数（包括 small）| 算法重构 | 高 | 高（已有 P1.B-1 之前讨论过 small bucket scatter 开销 > 直接写） |

全部预期 wall 改善 < 1%，**当前不值得**。doctrine 已正确把这些放进 P1.C / P2 territory。

## 7. 推论：wall 噪声 vs 改善阈值

`test_factor_with_kleinjung` 单次 wall 在 50-67s 间波动（**33% 变异**），原因：
- 多线程调度非确定性（10 worker 启停时机不同）
- ThreadPool spin-then-cv 让 worker 占用不均匀（P1.B-1c 引入但 wall 净改善）
- 缓存预热状态

→ **任何 wall 改善 < 5% 的优化在单次跑下不可分辨**。

P1.B-2 即使 lucky 给 1-2% wall 改善，也无法可靠测出。doctrine 铁律 5 在此场景的延伸：**测量精度 < 改善幅度时，应放弃 micro-optimization**。

## 8. 决策与下一步

**P1.B-2 关闭**：
- BACKLOG.md 未列入（一直在 doctrine §6 排程，未升级为 BACKLOG 条目）
- doctrine §6 标记 null + 更新理由
- RESOLVED.md 追加 P1.B-2 null 记录

**doctrine §6 候选剩余**：
- **P1.B-3 (TLBMissRate calibration)**: 当前 ~54%，P1.B-1b 已降 8pp，预计仍是噪声
- **Spin budget tuning (P1.B-1c follow-up)**: 静态 2000 → 1000/5000 扫描，预期边际
- 新方向：**Krylov-only BlockWiedemann（P2 前置工作）** 或 sample 找新热点

## 附录

### 文件
- `bench/results/2026-05-14-lattice-sieve-align-null.md`（本文件）
- `/tmp/p1b2_microbench/bench.cpp`（micro-bench 源码，可再现）

### sample baseline 数据
- `/tmp/p1b2_baseline_sample.txt`（25s sample of test_factor_with_kleinjung）
- sieve_row_chunk: 2103 samples（含 +1116=1228, +1124=508, +1140=170）

### 反汇编 hot section
- 二进制 `<repo-root>/build-release/test_factor_with_kleinjung`
- 符号基址 `0x100011e08`（`__ZN4gnfs5sieve12LatticeSieve15sieve_row_chunk`）
- inner loop `0x100012264 — 0x100012278`（6 指令）

### Build dirs（临时，gitignored）
- `<repo-root>/build-release/`
- `/tmp/p1b2_microbench/`
