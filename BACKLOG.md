# BACKLOG — 待办备忘录

> 只记录**未完成**的问题。已完成和误报条目见 `RESOLVED.md`。
> 从文件开头往下读即为优先级：P1-OPT > P2 > P3。

| 级别 | 条数 | 涵盖 |
|------|------|------|
| **P1-OPT** | 0 | (已清空) |
| **P1** | 0 | (已清空) |
| **P2** | 1 | class group ×1 |
| **P3** | 7 | 远期架构 ×6, scaling ×1 |

---

---

## P2 — 中优先级

### [BUG] Class Group Characters 实现仅支持 Cubic Fields
- **发现日期**: 2026-03-13
- **文件**: `sqrt/class_group.hpp`
- **描述**: `class_group.hpp` 的 Minkowski bound、signature、character computation 均假定 degree=3。对 degree≥4 产生错误列值，导致所有 BL 依赖在 sqrt 阶段失败。当前已全局禁用 (`include_class_group=false`)，QC+Schirokauer 足够替代。如需恢复 class group 功能需全面重写。
- **建议**: 若要支持，需正确处理任意 degree 的签名 (r1,r2)、SNF、character computation。参考 PARI/GP 或 SageMath 的实现。

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

#### [OPT] Bucket Sieve for B > 500K (80+ digit 必需)
- **发现日期**: 2026-03-12
- **描述**: 80-digit (264 bit, degree 5) 实测 15 rels/s，需 11 天完成筛法。主因：lattice sieve O(sieve_entries × sieve_area) 对 149K 条目 × 33M 位置太慢。
  CADO-NFS 用 bucket sieve：按 log_p 桶分配、顺序写入，O(hits) 而非 O(entries×area)。
  还需 Kleinjung poly selection（降低有效范数 10-100×）。
- **建议**: 实现 2 级 bucket sieve (small primes 直接筛, large primes 用 bucket)。参考 CADO-NFS `las/` 或 msieve `sieve/`。

#### [FEAT] 80/100-digit Scalability
- **描述**: 已用 CADO-NFS 校准参数 (C80: B=1M/2M, C100: B=8M/16M)。矩阵大小合理但筛法太慢。需 bucket sieve + Kleinjung 才能在合理时间完成

---

## TEST — 测试覆盖率缺口

（当前无未解决条目。历史记录见 RESOLVED.md）
