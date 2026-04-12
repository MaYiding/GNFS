# BACKLOG — 待办备忘录

> 只记录**未完成**的问题。已完成和误报条目见 `RESOLVED.md`。
> 从文件开头往下读即为优先级：P1 > P1-OPT > P2 > P3 > TEST。

| 级别 | 条数 | 涵盖 |
|------|------|------|
| **P1** | 0 | (已全部修复 — Session 61) |
| **P1-OPT** | 0 | (已清空) |
| **P2** | 1 | class group ×1 |
| **P3** | 8 | 远期架构 ×6, style ×1, FEAT ×1 |
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

#### [OPT] Bucket Sieve 进一步优化 (80+ digit 性能瓶颈)
- **发现日期**: 2026-03-12 (更新: 2026-03-15)
- **描述**: Session 67 实现了 CADO-NFS 风格 bucket region sieve（三级素数处理: tiny <256 stride, medium 256..64K scatter, large >64K direct），当 FB > 5000 时自动启用。80-digit 仍需进一步优化：(1) 多线程 scatter/apply 并行化; (2) 更紧凑的 bucket entry; (3) Kleinjung poly selection 降低范数。
- **建议**: 在当前 bucket region 基础上添加多线程支持和内存优化。

### 风格 & 一致性

#### [DEBT] 命名空间风格不一致（搁置 — 34 文件机械变更）
- **发现日期**: 2026-03-14
- **文件**: 多个（34 个头文件用 C++98 style，8 个用 C++17 style）
- **描述**: 部分文件用 `namespace gnfs { namespace core {` (C++98)，部分用 `namespace gnfs::core {` (C++17)。项目标准为 C++20。
- **搁置原因**: 34 文件纯机械变更，风险大于收益。无功能影响。
- **建议**: 统一为 C++17 嵌套命名空间语法。

---

## FEAT — 80/100-digit Scalability
- **描述**: 已用 CADO-NFS 校准参数 (C80: B=1M/2M, C100: B=8M/16M)。矩阵大小合理但筛法太慢。需 bucket sieve + Kleinjung 才能在合理时间完成
