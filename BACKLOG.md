# BACKLOG — 待办备忘录

> 只记录**未完成**的问题。已完成和误报条目见 `RESOLVED.md`。
> 从文件开头往下读即为优先级：P1-OPT > P2 > P3。

| 级别 | 条数 | 涵盖 |
|------|------|------|
| **P1-OPT** | 3 | Bucket Sieve, Sieve 内存, Kleinjung 质量 |
| **P1** | 0 | (已清空) |
| **P2** | 2 | Clique, NEON (SGE 已完成) |
| **P3** | 18 | 小优化 ×2, 远期架构 ×6, 代码质量 ×9, 调查 ×1 |

---

## P1-OPT — 高优先级性能优化

### [OPT] Bucket Sieve 架构（80+ 位必须）
- **发现日期**: 2026-02-20 (Session 2), 更新 2026-03-11 (Session 45)
- **文件**: `sieve/lattice_sieve.hpp:221-408`
- **描述**: 当前所有因子基素数统一步长 p 逐点减对数。p > L1 cache 跨度时严重 cache miss。应分桶：筛区间分成 ~L1 大小的桶，大素数按桶预计算命中点
- **阈值**: `p_bucket ≈ sieve_i_width / 64`（约 2K-10K）
- **预期收益**: 因子基 10⁵+ 时 3-10× 加速

### [OPT] 筛选区域 + sieve_parallel 内存模型
- **发现日期**: 2026-03-11 (Session 44+45)
- **文件**: `core/params.hpp:127-134`, `sieve/lattice_sieve.hpp:141-143,179`
- **描述**: 两个子问题:
  1. **区域**: P0 修正后已缩小 (15K×3750 vs 旧 16K×16K)，但仍有优化空间
  2. **sieve_parallel 内存**: 每线程构造完整 `LatticeSieve` 对象（含独立 sieve_array + resize）；每 SQ 全量 `std::fill` 清零
- **优化方向**:
  - 预分配 `num_threads` 个 LatticeSieve 对象复用（避免反复构造/析构）
  - 增量清零（只清上个 SQ 的脏位置）

### [OPT] Kleinjung 多项式选择 — 实现质量提升（管线集成已完成）
- **发现日期**: 2026-03-11 (Session 45), 原始 2026-02-20 (Session 2)
- **文件**: `polynomial/kleinjung_selector.hpp`
- **已完成**: ✅ 管线集成 — `SelectorDispatch` 自动按 degree 分发（Session 55, `f6ad14d`）
- **剩余**: 实现质量: search_radius=100、暴力循环（非格筛）、无 α 值评估、系数边界过松
- **建议**: 100+ 十进制位用格筛搜索（参考 CADO-NFS `polyselect`）
- **预期收益**: 更好多项式可减少筛选时间 2-5×

---

---

## P2 — 中优先级

### [OPT] Clique removal 仅处理大素数侧 singleton
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `relation/filter.hpp:71-86,148-199`
- **描述**: `filter_pass()` 只对大素数做 singleton removal，factor base 侧未处理。短期扩展为全列 singleton；中期由 SGE 处理
- **备注**: SGE 已集成(Session 56)，可在矩阵层面处理全列 singleton

### [OPT] NEON SIMD 加速
- **发现日期**: 2026-02-20 (Session 2), 补充 Session 45
- **描述**: ARM NEON 加速两个方向:
  - Sieve: `vqsubq_u16` 减对数、`vminvq_u16` 候选扫描
  - LinAlg: `veorq_u64` GF(2) XOR
- **已完成**: ✅ BL `xor_with_mul_par` ctz → 4-bit nibble LUT（Session 56, `3856308`）

---

## P3 — 低优先级

### 小优化

#### [OPT] Alpha 缺少判别式双根贡献
- **文件**: `polynomial/murphy_evaluator.hpp:126-151`
- **描述**: p | disc(f) 时双根贡献应为 `log(p)/(p(p-1))` 而非 `log(p)/p`，二阶效应

#### [OPT] BL Gaussian fallback 用 vector\<bool\> 低效
- **文件**: `src/linalg/block_lanczos.cpp:291-365`
- **描述**: ≤1000 才触发，影响有限，应改用 packed bitset 或标记 deprecated

### 远期架构

#### [FEAT] CSR 矩阵格式（100K+ 行）
- **文件**: `linalg/sparse_matrix.hpp:179-315`
- **描述**: 当前 `vector<SparseRow>` 每行独立 heap 分配。改 CSR（连续 `col_indices` + `row_offsets`）利于 prefetch 和 SIMD

#### [FEAT] Block Wiedemann（130+ 位）
- **描述**: 矩阵 >5M 时 BL 顺序迭代成瓶颈，BW 的 SpMV 可分布式并行

#### [FEAT] 行筛实现（小 N 更高效）
- **描述**: 所有 N 都走格筛。<50 十进制位行筛更简单高效，非紧急

#### [FEAT] Out-of-core Relations
- **文件**: `relation/collector.hpp`
- **描述**: 50+ 十进制位需 10-100M 关系（数 GB），当前全在内存

#### [FEAT] Block Lanczos Out-of-core 矩阵
- **描述**: 矩阵必须完全在 RAM 中

#### [FEAT] ThreadPool Work-Stealing
- **描述**: 筛选 special-Q 开销不均匀，work-stealing 可提升负载均衡

### 代码质量

#### [DEBT] large_prime_bits 参数字段
- **文件**: `core/params.hpp:15-56`
- **描述**: LP bound 存 raw uint64，应增加 `large_prime_bits` 字段（与 CADO-NFS 一致）

#### [DEBT] log_scale 分散在三处
- **文件**: `core/params.hpp`, `factor_base/builder.hpp`, `sieve/lattice_sieve.hpp`
- **描述**: 三者独立维护默认值，Session 44 用 `SIEVE_LOG_SCALE=16` 常量临时修复

#### [DEBT] uint64_t b → int64_t 截断（13 处）
- **发现**: Session 5 | b 始终远小于 INT64_MAX，理论风险

#### [DEBT] Schirokauer 文档注释与代码不一致
- **文件**: `linalg/schirokauer.hpp:138`

#### [DEBT] SmallVector 缺少边界检查
- **文件**: `util/small_vector.hpp:96-103`

#### [DEBT] FactorBase 缺少序列化
- **文件**: `factor_base/factor_base.hpp:137-141`

#### [DEBT] Relation 序列化格式（无版本/校验和）
- **文件**: `core/relation.hpp:73-144`

#### [DEBT] -Wconversion 清理（~60 处）
- **发现**: Session 18 | 大多 cosmetic

#### [DEBT] 根目录遗留文件清理

### 调查

#### [RISK] sieve_parallel + 高 threshold 下 cofactorizer 通过率异常
- **发现日期**: 2026-03-11 (Session 44)
- **调查**: 2026-03-12 (Session 55) — **sieve_parallel 线程安全已确认**
  - 每线程独立 LatticeSieve 对象，无共享可变状态
  - 原子计数器 (relaxed) 分配 SQ，各线程写唯一 `all_results[idx]`
  - 异常可能来自下游 cofactorizer 配置或报告统计口径，非筛法本身
- **状态**: 降级为 P3 观测项，sieve 安全，关注 cofactorizer 侧

---

## TEST — 测试覆盖率缺口

（当前无未解决条目。历史记录见 RESOLVED.md）
