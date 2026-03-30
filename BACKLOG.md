# BACKLOG — 待办备忘录

> 只记录**未完成**的问题。已完成和误报条目见 `RESOLVED.md`。
> 严重程度排序：P0 > P1 > P1-OPT > P2 > P3 > TEST。从文件开头往下读即为优先级。

---

## P1 — 高优先级（影响正确性或大数支持）

（当前无未解决条目。历史记录见 RESOLVED.md）

---

## P1-OPT — 高优先级性能优化

### [OPT] Block Lanczos 占 25-digit 分解 42% 时间（380s）
- **发现日期**: 2026-03-11 (Session 44)
- **文件**: `src/linalg/block_lanczos.cpp`
- **描述**: 37K×39K GF(2) 矩阵求 200 个依赖用 380s。Session 22 已做 ThreadPool 并行化 SpMV，但仍是主要瓶颈之一。可能的优化方向：
  - NEON SIMD 加速 GF(2) 运算（veorq_u64 等）
  - 矩阵压缩/稀疏表示优化减少内存带宽
  - 更大的 block width（当前 64-bit）
  - 减少依赖数量需求（当前请求 200 个，实际只需几个有效的）
- **实测数据**: 25-digit (81-bit) Release 模式，12 线程

### [OPT] Hensel Sqrt 占 25-digit 分解 48% 时间（437s）——依赖失败率高
- **发现日期**: 2026-03-11 (Session 44)
- **文件**: `include/gnfs/sqrt/hensel_sqrt.hpp`, `include/gnfs/sqrt/algebraic_sqrt.hpp`
- **描述**: 25-digit 分解中 dep #1-4 全部 Hensel 验证失败（`φ(S)² ≠ P·f'(m)² mod N`），每个 dep 做 4 次尝试（额外 lift 15→16→17→18），直到 dep #5 才成功。共 17 次 Hensel 提升，每次涉及 10K 因子 × 327K-2.6M bit modulus 的大整数乘法
- **瓶颈分析**:
  - 大积预计算（parallel product of ~10K factors）每次 dep 都要重新计算
  - 每次 lift 的模乘运算涉及百万位级 GMP 整数
  - 4/5 的 dep 产出平凡因子是正常的（GNFS 预期 ~50% 成功率），但每个失败 dep 的 4 次重试代价太高
- **优化方向**:
  - **早期剪枝**: 在低 lift 阶段（如 lift 5-6）就检查 dep 是否可能有效，避免浪费高精度 lift
  - **缓存大积**: 不同 dep 共享大部分关系，积的差异可增量计算
  - **减少重试次数**: 验证失败后不应做 4 次 extra lift（精度已经远超需要）
  - **选择更好的起始素数 p**: 当前固定 p=1013，可尝试多个小素数并行

### [OPT] 筛选区域过大导致内存开销（256M positions = 512MB/sieve）
- **发现日期**: 2026-03-11 (Session 44)
- **文件**: `include/gnfs/core/params.hpp:127-134`, `include/gnfs/sieve/lattice_sieve.hpp`
- **描述**: 25-digit 使用 32000×8000 = 256M 位置的筛区域（uint16_t × 256M = 512 MB）。sieve_parallel 12 线程各自分配独立 sieve_array，峰值内存 ~6 GB。单线程模式下 92s 只需 4 个 SQ 就收集到 63K 关系——说明区域过大、每 SQ 产出远超需要
- **优化方向**:
  - 缩小筛区域（如 16000×4000 = 64M），减少内存 4× 同时增加 SQ 数量
  - 动态调整：根据前几个 SQ 的 yield 自适应调节区域大小
  - 分块筛选（line sieve）：按 j 行分块处理，控制内存在 L2/L3 cache 内

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

### [DEBT] log_scale 分散在三个配置结构体中
- **发现日期**: 2026-03-11 (Session 44)
- **文件**: `include/gnfs/core/params.hpp`, `include/gnfs/factor_base/builder.hpp`, `include/gnfs/sieve/lattice_sieve.hpp`
- **描述**: `GNFSParams.log_scale`、`FactorBaseBuilder::Options.log_scale`、`SieveParams.log_scale` 三者独立维护各自默认值（10/12 vs 16 vs 16）。Session 44 修复了 threshold 使用常量 `SIEVE_LOG_SCALE=16` 的临时方案，但根本解法是统一为单一来源：
  - 方案 A：GNFSParams 作为唯一来源，测试/管线负责传递给 FB/Sieve
  - 方案 B：删除 GNFSParams.log_scale，FB 和 Sieve 各自用默认 16
  - 方案 C：GNFSParams.compute() 自动输出 fb_opts 和 sieve_params（最彻底）

### [RISK] sieve_parallel + 高 threshold 下 cofactorizer 通过率异常
- **发现日期**: 2026-03-11 (Session 44)
- **描述**: 使用 threshold=84（combined）+ sieve_parallel 256 SQs 时，collector 报告 1,941,652 个验证通过的关系（~7600/SQ），但诊断显示每 SQ 仅 ~7500 候选。这意味着 ~100% cofactorizer 通过率，而单线程模式同 threshold 下仅 ~0.06% 通过率。串行/并行 sieve 结果完全一致（诊断已验证），怀疑 cofactorizer 在处理 parallel sieve 返回的大量候选时存在某种交互效应。需要进一步调查
- **复现**: threshold=42+42=84, BATCH_SIZE=256, sieve_parallel + cofac.verify 循环

### [DEBT] test_25digit.cpp 预期因子注释错误
- **发现日期**: 2026-03-11 (Session 44)
- **文件**: `tests/test_25digit.cpp:44`
- **描述**: 注释写 `// Expected: 40883763227 × 40853175319`，但实际因子为 `1292282677523 × 1292282676071`。注释中 40883763227 × 40853175319 ≈ 1.67×10²¹（22位），而 N = 1.67×10²⁴（25位），量级不符

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
