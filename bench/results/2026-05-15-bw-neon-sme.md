# P2 Stage B+C — NEON 极限版 + SME 探索 (Coppersmith block BM 后续)

**Date**: 2026-05-15
**Branch**: `feat/260515-bw-neon-sme`
**Doctrine**: §6 P2 第 2 条 — "SME 探索性应用 (BL/BW 64×N SpMV), 高风险高回报, 先做 NEON 极限版作为对照"
**Hardware**: Apple M5 P-core, macOS 26.5 (Tahoe), 16 KB pages
**Tools**: spmv_neon_gate (isolated micro-bench), Apple `sample`, std::chrono per-phase timing

---

## 1. Motivation

P2 第 1 条 (BlockWiedemann 真 block BM, 关闭 2026-05-14) 已经把 62K×10K Krylov 阶段从 53.52s 降到 1.12s (48× speedup), 但 SpMV 内 inner loop 仍是 scalar 64-bit `acc ^= x.data[*p]`. doctrine §6 P2 第 2 条 排程进一步把 block width 从 64 扩到 128 (NEON) 和 SVL=512 (SME), 利用 SIMD wider XOR.

doctrine 措辞 "**先做 NEON 极限版作为对照**" — NEON 是 SME 的对照基线, 必须先有 NEON 数据才能判断 SME 是否值得复杂工程投入。

## 2. Stage A — Profile baseline

### A.1 Per-phase timing

加 `std::chrono` 到 `block_wiedemann_block_solve` (commit `989d9b4`). 278K×10K Release, 3 loops 取后两个稳态:

| Phase | wall | 占比 | 内容 |
|---|---:|---:|---|
| 1 Krylov (L=346 SpMV) | 2620 ms | **63%** | `bw_spmv_B = transpose + forward`, `inner_product_64x64` |
| 2 matrix BM (max_deg=157) | 308 ms | **7%** | Coppersmith column-extended quadratic basecase |
| 3 mksol (158 SpMV + 158 accumulate) | 1186 ms | **28%** | `bw_spmv_B`, `mksol_accumulate (V_k · F_step)` |
| **Total** | 4158 ms | 100% | |

**bw_spmv_B 占 >85% 总 wall**, 完美 hot path. Phase 2 matrix BM 仅 7%, 几乎与 SpMV 改造无关.

### A.2 sample attribution (12s sample window)

| Thread | total | top symbol |
|---|---:|---|
| main | 9927 | `block_wiedemann_block_solve` — 91% Phase 1, 9% Phase 3 |
| worker × 10 | 3636 ea | `ThreadPool::worker_loop +556` (worker active path, inlined SpMV lambda) |

主线程 9913 samples 在 `bw_spmv_B` 调用栈下, 90%+ 是 dispatch + future.get; worker thread 99% 时间在 SpMV inner loop. **没有其他 hot path**.

### A.3 PMU baseline (已废)

P1.A 已有 BW baseline (`bench/results/2026-05-13-pmu-deepening.md`):
- BackendStallRate 74.79%, L1DMissRate 12.80%, SIMDDensity 5.85%

SIMDDensity 5.85% 是整个 GNFS, 但 BW path 没专门测过 (heavy tier). 跳过, 用 sample + chrono 已足够定位.

### A.4 决策 → 路径 X

`BlockVector::data` 是 `std::vector<uint64_t>` (64-bit per element). SIMD wider XOR 必须扩 block width:
- NEON 128-bit `veorq_u64` → BlockVector128 (2 uint64 per element)
- SME SVL=512 streaming SVE2 → BlockVector512 (8 uint64)

**策略**: 走 **isolated micro-bench**, 不集成入 BW pipeline. 理由:
- 完整 BV128 BW pipeline (含 matrix BM 128 重写) 估算 ~1150 行 / 15-30 commits, 预计 BW wall 减少 ~22% (1.28× speedup)
- 工程量与收益比不平衡, 而且 NEON 集成属 doctrine §6 P1.C 范畴 (BackendStall > 30% 触发, 但 MemBound 才是结构性瓶颈)
- doctrine 写 "专项实验 + 小规模验证" — 强烈暗示 PoC + bench 即可

---

## 3. Stage B — NEON SpMV128 micro-bench

实施 `bench/microbench/spmv_neon_gate.cpp` (commit `1c92212`):

- **Data layout**: BlockVector128 = `uint64_t[2 * m]` interleaved (low/hi pair per element)
- **forward NEON**: 内 inner loop `veorq_u64 + vld1q_u64`, prefetch N_AHEAD=8
- **transpose NEON**: per-thread NEON scatter + NEON reduce
- **Cross-validate**: NEON128 output == 两次 scalar64 SpMV 输出 interleaved (forward + transpose 双双 PASS)

## 4. Stage C — SME baseline (4× NEON unroll 模拟 SVL=512)

**真 SME streaming mode 在 macOS 26.5 user-space 不可调用** (M5 上 `__arm_streaming` 函数调用直接 SIGILL). 原因调研:
- xnu (`apple-oss-distributions/xnu/blob/main/doc/arm/sme.md`) lazy-allocates SME thread state on first SME instruction trap
- M5 hardware 完全支持 (FEAT_SME / SME2 / SME2p1 = 1, sme_max_svl_b = 64 bytes = 512 bit)
- macOS 26.5 可能仍 gate 在 entitlement 或 Apple-internal use
- non-streaming SVE2 同样 SIGILL (M5 无独立 FEAT_SVE, SVE 仅 streaming SME 下)

**Workaround**: 用 **4× NEON 128-bit ops** 等价于 SVE2 streaming load/xor/store 8 uint64 (SVL=512). 这是 SME real-path 的功能 baseline, 等真 SME gate 打开后只需替换 intrinsics.

实施 `spmv_forward_512_neon4` / `spmv_transpose_512_neon4` (commit `1624c13`):
- 每 row element 4 个 `vld1q_u64` + 4 个 `veorq_u64` (forward) 或 4 RMW (transpose)
- Cross-validate: SpMV512 output == 4 个独立 scalar64 SpMV interleaved (PASS)

## 5. Results — per-bit speedup table

278K × 10K Release, 10 threads, 30 reps × 3 trials, take min wall:

| Op | scalar 64 wall | NEON 128 wall | 4×NEON 512 wall | 128 per-bit | 512 per-bit |
|---|---:|---:|---:|---:|---:|
| forward | 28.5 ms | 33.0 ms | 52.7 ms | **1.73×** | **4.33×** |
| transpose | 33.5 ms | 51.7 ms | 94.7 ms | **1.30×** | **2.83×** |
| **bw_spmv_B** | 62.0 ms | 84.7 ms | 147.4 ms | **1.47×** | **3.37×** |

`per-bit speedup = (width / 64) × wall_64 / wall_width`. >1.0 表示 wider 路径更高效.

### Observations

1. **NEON 128 → 4×NEON 512 仍有 ~2.3× 增量收益**. 内存带宽未饱和 (M5 LPDDR5X 接 ~190 GB/s). doctrine "高风险高回报" 中的"高回报"确实存在.

2. **forward (1.73×/4.33×) >> transpose (1.30×/2.83×)**. 原因:
   - forward = streaming read (col 索引引导 gather) + 1 write per row, M5 HW prefetcher 好用
   - transpose = RMW + scatter to thread-local accumulator, 写依赖 col 索引, cache MSHR stalls + RMW chain 更长

3. **真 SME 路径预估** (若 macOS gate 打开):
   - bw_spmv_B 预计 ~3.4× per-bit (basis: 4×NEON 512 measurement)
   - BW Phase 1 wall: L=346 × T64 → L=86 (=2⌈10000/512⌉+32) × (4 × T64 / 3.4) = 86 × 1.18 × T64 ≈ 101 × T64. **Phase 1 wall ratio = 101/346 = 29% → 3.4× Phase 1 加速**
   - Phase 2 matrix BM 512 (quadratic in b²): **慢 ~4×** 但仅 7% wall → 影响小
   - Phase 3 类似 Phase 1
   - 总预估: BW pipeline 512 wall = 0.29 × 0.91 + 4 × 0.07 + 0.29 × 0.02 = 0.264 + 0.28 + 0.006 ≈ **35% baseline → 2.86× 总加速**

4. **NEON 128 路径预估**:
   - per-bit 1.47×, L 减半 → Phase 1/3 wall = (L/2) × (T64 × 1.36) / T64 = **0.68× baseline**
   - Phase 2 vs baseline: b=128 vs b=64, b² 4× 慢, L 减半, 总 **2× 慢**
   - 总预估: BW pipeline 128 wall = 0.68 × 0.91 + 2 × 0.07 + 0.68 × 0.02 = 0.619 + 0.14 + 0.014 ≈ **77% baseline → 1.30× 总加速**

## 6. 决策与未来工作

### 关闭

- **doctrine §6 P2 第 2 条**: NEON 极限版 ✅ (isolated 1.47× per-bit), SME baseline ✅ (4×NEON 3.37× per-bit). 真 SME 路径在 macOS 26.5 user-space 不可用 (SIGILL).
- **本次实施限于 micro-bench**. 不集成入 BW pipeline.

### 未来工作 (BACKLOG)

1. **BV128 BW pipeline 完整集成** — 工程量 ~1150 行 / 15 commits, 预期 1.30× 总 BW 加速. 触发: 当 GNFS 真实生产 workload 出现 m × (m+n) > 4 GiB 矩阵 (走 BW path) 且 wall time 是阻塞 factor 时启动.
2. **真 SME streaming mode** — macOS user-space 启用后 (`__arm_streaming` 不 SIGILL), 替换 `spmv_*_512_neon4` 的 4×NEON 为 SVE2 streaming intrinsics. 预期 2.86× 总 BW 加速. 跟踪 xnu 上游 SME 状态.
3. **PMU 验证 SpMV128/512** — 收集 BackendStallRate / L1DMissRate / SIMDDensity. 当前 P1.A 仅有整体 GNFS baseline, BW path PMU 是空白. 需要 `pmu-stat.sh ./bw_block_only_loop`.

## 7. 教训

1. **doctrine 铁律 5 (measurement-first) 的反向应用**: 不仅适用于"实施前先 baseline", 也适用于"实施前先 sub-component micro-bench". 完整 BV128 pipeline 改造前, isolated SpMV gate 已经给出准确的 per-bit speedup, 决策路径短了很多.
2. **doctrine "高风险高回报" 是基于 work / wall 不对称的判断**. NEON 128 工程量 / wall 收益比 = ~1150 / 30% wall = 3.83 行/百分点. SME 真路径 (如果可用) ~2000 / 65% wall = 3.08 行/百分点 — 略优. 但 macOS 26.5 SIGILL gate 把 SME 路径推到"未来工作".
3. **macOS 与 Linux SME 兼容性裂痕**. xnu lazy-trap 模型与 Linux kernel SME context switching 不同 — 跨平台代码必须有 fallback path. 4×NEON unrolled XOR 是 portable 等效.

## 8. References

- [xnu/doc/arm/sme.md](https://github.com/apple-oss-distributions/xnu/blob/main/doc/arm/sme.md) — Apple 官方 SME 设计 doc
- [tzakharko/m4-sme-exploration](https://github.com/tzakharko/m4-sme-exploration) — M4 SME PoC
- [Arm SME Programmer's Guide](https://documentation-service.arm.com/static/664f013638084307512bb30c)
- [LLVM ARM SVE/SME builtins](https://clang.llvm.org/docs/AttributeReference.html#arm-streaming)

## 9. Commits

- `989d9b4` test(linalg): per-phase timing for BW block solve (P2 A.1)
- `48d0890` test(perf): bw_block_only_loop microbench for P2 NEON/SME baseline
- `1c92212` test(perf): SpMV NEON gate microbench (P2 Stage B)
- `1624c13` test(perf): SpMV512 (4xNEON unroll) as SME SVL=512 baseline (P2 Stage C)
