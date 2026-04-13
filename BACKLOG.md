# BACKLOG — 待办备忘录

> 只记录**未完成**的问题。已完成和误报条目见 `RESOLVED.md`。
> 从文件开头往下读即为优先级：P1 > P1-OPT > P2 > P3 > TEST。

| 级别 | 条数 | 涵盖 |
|------|------|------|
| **P1** | 0 | (已全部修复 — Session 61) |
| **P1-OPT** | 0 | (已清空) |
| **P2** | 1 | class group ×1 |
| **P3** | 3 | 远期架构 ×2, FEAT ×1 |
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

> **Session 65 修复 9 条, Session 67 修复 4 条（CSR + 行筛 + Work-Stealing + 命名空间）。详见 RESOLVED.md。**

### 远期架构

#### [FEAT] Out-of-core Relations
- **文件**: `relation/collector.hpp`
- **描述**: 50+ 十进制位需 10-100M 关系（数 GB），当前全在内存

#### [FEAT] Block Lanczos Out-of-core 矩阵
- **描述**: 矩阵必须完全在 RAM 中

#### [OPT] Bucket Sieve 进一步优化 (80+ digit 性能瓶颈)
- **发现日期**: 2026-03-12 (更新: 2026-03-15)
- **描述**: Session 67 实现了基本 bucket region sieve。仍需：(1) 多线程 scatter/apply; (2) 更紧凑 bucket entry; (3) Kleinjung poly selection。
- **建议**: 在当前 bucket region 基础上添加多线程支持和内存优化。

---

## FEAT — 80/100-digit Scalability
- **描述**: 已用 CADO-NFS 校准参数 (C80: B=1M/2M, C100: B=8M/16M)。Session 67 实现 bucket region sieve 和 CSR SpMV。仍需 Kleinjung + 更深度 bucket 优化才能在合理时间完成
