# BACKLOG — 待办备忘录

> 只记录**未完成**的问题。已完成和误报条目见 `RESOLVED.md`。
> 从文件开头往下读即为优先级：P1 > P1-OPT > P2 > P3 > TEST。

| 级别 | 条数 | 涵盖 |
|------|------|------|
| **P1** | 0 | (已全部修复 — Session 61) |
| **P1-OPT** | 0 | (已清空) |
| **P2** | 1 | class group ×1 |
| **P3** | 15 | 远期架构 ×7, style ×1, risk ×1, FEAT ×1, 已搁置 ×5 |
| **TEST** | 0 | (已全部修复 — Session 64) |

---

## P1 — 高优先级（影响正确性、可靠性）

> **全部已修复** — Session 61, 2026-03-14。详见 RESOLVED.md。

---

## P2 — 中优先级

> **Session 62 修复 25 条, Session 63 修复 7 条, Session 64 修复 9 条。详见 RESOLVED.md。**

### 基础设施

#### [BUG] Class Group Characters 实现仅支持 Cubic Fields
- **发现日期**: 2026-03-13
- **文件**: `sqrt/class_group.hpp`
- **描述**: `class_group.hpp` 的 Minkowski bound、signature、character computation 均假定 degree=3。对 degree≥4 产生错误列值，导致所有 BL 依赖在 sqrt 阶段失败。当前已全局禁用 (`include_class_group=false`)，QC+Schirokauer 足够替代。如需恢复 class group 功能需全面重写。
- **建议**: 若要支持，需正确处理任意 degree 的签名 (r1,r2)、SNF、character computation。参考 PARI/GP 或 SageMath 的实现。

---

## P3 — 低优先级

> **Session 65 修复 9 条（含 3 个误报关闭）。详见 RESOLVED.md。**

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

#### [OPT] Bucket Sieve for B > 500K (80+ digit 必需)
- **发现日期**: 2026-03-12
- **描述**: 80-digit (264 bit, degree 5) 实测 15 rels/s，需 11 天完成筛法。主因：lattice sieve O(sieve_entries × sieve_area) 对 149K 条目 × 33M 位置太慢。
  CADO-NFS 用 bucket sieve：按 log_p 桶分配、顺序写入，O(hits) 而非 O(entries×area)。
  还需 Kleinjung poly selection（降低有效范数 10-100×）。
- **建议**: 实现 2 级 bucket sieve (small primes 直接筛, large primes 用 bucket)。参考 CADO-NFS `las/` 或 msieve `sieve/`。

### 风格 & 一致性

#### [DEBT] 命名空间风格不一致（搁置 — 34 文件机械变更）
- **发现日期**: 2026-03-14
- **文件**: 多个（34 个头文件用 C++98 style，8 个用 C++17 style）
- **描述**: 部分文件用 `namespace gnfs { namespace core {` (C++98)，部分用 `namespace gnfs::core {` (C++17)。项目标准为 C++20。
- **搁置原因**: 34 文件纯机械变更，风险大于收益。无功能影响。
- **建议**: 统一为 C++17 嵌套命名空间语法。

### 潜在风险（需进一步调查）

#### [RISK] Block Lanczos 三步递推与 Montgomery 1995 不一致
- **发现日期**: 2026-03-14
- **文件**: `src/linalg/block_lanczos.cpp:479-499`
- **描述**: 代码使用包含 `V_pprev`（前两步向量）的三步递推，而 Montgomery 1995 论文只使用两步递推（V_cur 和 V_prev）。额外的 `F_cur = V_pprev^T · B · V_cur` 项和 `D_pprev * F_cur` 应用没有已知数学基础。然而所有测试（L1-L5, 25-digit, stress）均通过，可能是有效的变体或冗余项。
- **建议**: 对比 Montgomery 1995 §3 公式逐项核实。若额外项冗余（恒等于零），则为死代码可移除。若实际影响结果，需确认数学正当性。

---

## FEAT — 80/100-digit Scalability
- **描述**: 已用 CADO-NFS 校准参数 (C80: B=1M/2M, C100: B=8M/16M)。矩阵大小合理但筛法太慢。需 bucket sieve + Kleinjung 才能在合理时间完成
