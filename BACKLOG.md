# BACKLOG — 待办备忘录

> 只记录**未完成**的问题。已完成和误报条目见 `RESOLVED.md`。
> 从文件开头往下读即为优先级：P1 > P1-OPT > P2 > P3 > TEST。

| 级别 | 条数 | 涵盖 |
|------|------|------|
| **P1** | 0 | (已全部修复 — Session 61) |
| **P1-OPT** | 0 | (已清空) |
| **P2** | 0 | (已全部修复 — Session 70) |
| **P3** | 0 | (已全部完成 — Session 69) |
| **TEST** | 0 | (已全部修复 — Session 64) |

---

## P1 — 高优先级（影响正确性、可靠性）

> **全部已修复** — Session 61, 2026-03-14。详见 RESOLVED.md。

---

## P2 — 中优先级

> **Session 62 修复 25 条, Session 63 修复 7 条, Session 64 修复 9 条, Session 70 修复 1 条。详见 RESOLVED.md。**

---

## P3 — 低优先级

> **Session 65 修复 9 条, Session 67 修复 4 条, Session 69 修复 3 条（Block Wiedemann + OOC Relations/Matrix + Bucket Sieve Deep）。全部完成。详见 RESOLVED.md。**

---

## FEAT — 80/100-digit Scalability ✅
- **描述**: 已用 CADO-NFS 校准参数 (C80: B=1M/2M, C100: B=8M/16M)。Session 67-69 实现 bucket region sieve (含多线程 scatter)、CSR SpMV、Block Wiedemann、Out-of-core Relations/Matrix。Session 71 实现 Kleinjung 参数自适应缩放 (from_gnfs_params + SelectorDispatch 集成)。所有代码路径已就绪，实际 80/100-digit 分解需要对应规模的计算资源和时间
