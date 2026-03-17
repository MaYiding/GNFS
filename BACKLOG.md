# BACKLOG — 待办备忘录

> 记录发现但当前不处理的问题。每个条目必须有：分类、来源、描述、发现日期。
> 已解决的条目移到末尾「已完成」区域，不要删除。

## 待处理

### [BUG] Split Schirokauer: f mod 2 可约时映射计算错误
- **发现日期**: 2026-02-20 (Session 2)
- **来源**: Schirokauer maps 测试中发现
- **描述**: 当多项式 f mod 2 可约时，需要先做 valuation stripping 再计算 Schirokauer map。当前部分实现存在但有 bug（非单位元素处理不正确）
- **文件**: `include/gnfs/linalg/schirokauer.hpp`
- **影响**: 部分 N 值（f mod 2 可约的）无法正确因式分解
- **当前规避**: 选择 f mod 2 不可约的测试 N
- **优先级**: P1

### [OPT] ECM Stage 2 BSGS 优化
- **发现日期**: 2026-02-20 (Session 2)
- **来源**: 性能分析
- **描述**: 当前 ECM Stage 2 是朴素实现，复杂度 O(π(B2))。应改为 Baby-step Giant-step，复杂度降至 O(√(B2/B1))
- **文件**: `include/gnfs/cofactor/ecm.hpp`
- **影响**: 大余因子分解速度受限
- **优先级**: P1

### [OPT] Block Lanczos 是 25-digit 的主要瓶颈
- **发现日期**: 2026-02-22 (Session 3)
- **来源**: 25-digit 性能分析（193s = 81.6%）
- **描述**: Block Lanczos 占 25-digit 因式分解总时间的 81.6%。需要优化 SpMV 或考虑并行化
- **影响**: 30-digit+ 因式分解将更加受限于此
- **优先级**: P1

### [FEAT] NEON SIMD 加速
- **发现日期**: 2026-02-20 (Session 2)
- **来源**: 性能优化规划
- **描述**: 利用 ARM NEON 指令加速关键路径：`vqsubq_u16` 用于 sieve，`vminvq_u16` 用于候选扫描，`veorq_u64` 用于 GF(2) XOR
- **影响**: 预计 sieve 和 linalg 各有 2-4× 提升
- **优先级**: P2

### [FEAT] Bucket Sieve 架构
- **发现日期**: 2026-02-20 (Session 2)
- **来源**: 大因子基性能需求
- **描述**: 当前 sieve 对大因子基不够 cache-friendly。需要重新设计为 bucket sieve 架构
- **影响**: 大 N（50-digit+）的 sieve 阶段性能瓶颈
- **优先级**: P2

### [OPT] Kleinjung 多项式选择质量提升
- **发现日期**: 2026-02-20 (Session 2)
- **来源**: 多项式选择分析
- **描述**: 当前 Kleinjung 实现缺少 lattice-based search 和 coefficient rotation 优化
- **影响**: 较差的多项式会导致 sieve 阶段产出率低
- **优先级**: P2

### [DEBT] Out-of-core relations 支持
- **发现日期**: 2026-02-20 (Session 2)
- **来源**: 大 N 可扩展性分析
- **描述**: 当前所有关系存储在内存中。对于大 N，关系数可达数百万，需要磁盘存储和流式处理
- **优先级**: P3

### [DEBT] Integer(uint64_t) 构造函数缺失
- **发现日期**: 2026-02-20 (Session 2)
- **来源**: 代码审查
- **描述**: `core::Integer` 缺少从 `uint64_t` 直接构造的方法，经常需要绕道 string 或 mpz_class
- **优先级**: P3

### [DEBT] 根目录遗留文件清理
- **发现日期**: 2026-03-08 (Session 4)
- **来源**: git 初始化时发现
- **描述**: 根目录有 ~70 个遗留文件（.sh 脚本、过时 .md、扁平命名文件）。已纳入 git 但应考虑整理或移入 `legacy/` 目录
- **优先级**: P3

## 已完成

### [OPT] ~~Hensel Sqrt 预计算优化~~ ✅
- **发现日期**: 2026-02-20 (Session 2)
- **解决日期**: 2026-02-22 (Session 3)
- **结果**: Hensel 15.5× 加速，25-digit 总耗时 603s → 236.5s
