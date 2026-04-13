# BACKLOG — 待办备忘录

> 只记录**未完成**的问题。已完成和误报条目见 `RESOLVED.md`。
> 从文件开头往下读即为优先级：P1 > P1-OPT > P2 > P3 > TEST。

| 级别 | 条数 | 涵盖 |
|------|------|------|
| **P1** | 0 | (已全部修复 — Session 61) |
| **P1-OPT** | 0 | (已清空) |
| **P2** | 1 | class group ×1 |
| **P3** | 0 | (已全部完成 — Session 69) |
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

> **Session 65 修复 9 条, Session 67 修复 4 条, Session 69 修复 3 条（Block Wiedemann + OOC Relations/Matrix + Bucket Sieve Deep）。全部完成。详见 RESOLVED.md。**

---

## FEAT — 80/100-digit Scalability
- **描述**: 已用 CADO-NFS 校准参数 (C80: B=1M/2M, C100: B=8M/16M)。Session 67-69 实现 bucket region sieve (含多线程 scatter)、CSR SpMV、Block Wiedemann、Out-of-core Relations/Matrix。仍需 Kleinjung poly selection 改进才能在合理时间完成 100-digit
