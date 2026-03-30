# BACKLOG — 待办备忘录

> 只记录**未完成**的问题。已完成和误报条目见 `RESOLVED.md`。
> 严重程度排序：P0 > P1 > P1-OPT > P2 > P3 > TEST。从文件开头往下读即为优先级。

---

## P1 — 高优先级（影响正确性或大数支持）

（当前无未解决条目。历史记录见 RESOLVED.md）

---

## P1-OPT — 高优先级性能优化

（当前无未解决条目。历史记录见 RESOLVED.md）

---

## P2 — 中优先级（大数支持和架构改进）

### [FEAT] Bucket Sieve 架构
- **发现日期**: 2026-02-20 (Session 2)
- **描述**: 大因子基需要 cache-friendly bucket sieve

### [FEAT] NEON SIMD 加速
- **发现日期**: 2026-02-20 (Session 2)
- **描述**: ARM NEON 加速 sieve 和 linalg

### [OPT] Kleinjung 多项式选择质量提升
- **发现日期**: 2026-02-20 (Session 2)
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp`
- **描述**: search_radius 硬编码 100、系数边界过松、缺少 lattice search

### [FEAT] Out-of-core Relations 支持
- **发现日期**: 2026-02-20 (Session 2)
- **文件**: `include/gnfs/relation/collector.hpp`
- **描述**: 50+ 位 N 需要 10-100M 关系（数 GB）

### [FEAT] Block Lanczos Out-of-core 矩阵支持
- **发现日期**: 2026-03-08 (Session 5)
- **描述**: 矩阵必须完全在 RAM 中

### [FEAT] ThreadPool Work-Stealing
- **发现日期**: 2026-03-08 (Session 5)
- **描述**: 筛选 special-Q 开销不均匀

---

## P3 — 低优先级（代码质量和长期改进）

### [OPT] Murphy E-score 低估 20-40%
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/polynomial/murphy_evaluator.hpp`

### [DEBT] 全局性 uint64_t b → int64_t 截断（13 处）
- **发现日期**: 2026-03-08 (Session 5) | b 值始终远小于 INT64_MAX
- **描述**: 理论上 b > INT64_MAX 时截断，实际不会发生

### [DEBT] Schirokauer 文档注释与代码不一致
- **文件**: `include/gnfs/linalg/schirokauer.hpp:138`

### [DEBT] SmallVector 缺少边界检查
- **文件**: `include/gnfs/util/small_vector.hpp:96-103`

### [DEBT] FactorBase 缺少序列化
- **文件**: `include/gnfs/factor_base/factor_base.hpp:137-141`

### [DEBT] Relation 序列化格式缺陷（无版本/校验和）
- **文件**: `include/gnfs/core/relation.hpp:73-144`

### [DEBT] -Wconversion 清理（~60 处 sign-conversion）
- **发现日期**: 2026-03-10 (Session 18)
- **描述**: `-Wall -Wextra -Wpedantic` 已清零 warning。`-Wconversion` 下有 ~60 处 sign-conversion（int→size_t 数组下标、int64→uint64 等），大多 cosmetic。最可疑的是 `modular_poly.hpp` 中 ~20 处 int degree 循环变量做 vector 下标、`Relation::b` int64→uint64 传参。无安全隐患但降低代码严谨度

### [DEBT] 根目录遗留文件清理

---

## TEST — 测试覆盖率缺口

（当前无未解决条目。历史记录见 RESOLVED.md）
