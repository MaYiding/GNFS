# BACKLOG — 待办备忘录

> 只记录**未完成**的问题。已完成和误报条目见 `RESOLVED.md`。
> 从文件开头往下读即为优先级：P1-OPT > P2 > P3。

| 级别 | 条数 | 涵盖 |
|------|------|------|
| **P1-OPT** | 0 | (已清空) |
| **P1** | 0 | (已清空) |
| **P2** | 0 | (已清空) |
| **P3** | 6 | 远期架构 ×6 |

---

---

## P3 — 低优先级

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

---

## TEST — 测试覆盖率缺口

（当前无未解决条目。历史记录见 RESOLVED.md）
