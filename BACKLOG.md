# BACKLOG — 待办备忘录

> 只记录**未完成**的问题。已完成和误报条目见 `RESOLVED.md`。
> 从文件开头往下读即为优先级：P1-OPT > P2 > P3。

| 级别 | 条数 | 涵盖 |
|------|------|------|
| **P1-OPT** | 3 | Bucket Sieve, Sieve 内存, Kleinjung 集成 |
| **P1** | 0 | (已清空) |
| **P2** | 5 | SGE, base-m 搜索窗口, ECM 余因子, Clique, NEON |
| **P3** | 21 | 小优化 ×4, 远期架构 ×6, 代码质量 ×10, 调查 ×1 |

---

## P1-OPT — 高优先级性能优化

### [OPT] Bucket Sieve 架构（80+ 位必须）
- **发现日期**: 2026-02-20 (Session 2), 更新 2026-03-11 (Session 45)
- **文件**: `sieve/lattice_sieve.hpp:221-408`
- **描述**: 当前所有因子基素数统一步长 p 逐点减对数。p > L1 cache 跨度时严重 cache miss。应分桶：筛区间分成 ~L1 大小的桶，大素数按桶预计算命中点
- **阈值**: `p_bucket ≈ sieve_i_width / 64`（约 2K-10K）
- **预期收益**: 因子基 10⁵+ 时 3-10× 加速

### [OPT] 筛选区域 + sieve_parallel 内存模型
- **发现日期**: 2026-03-11 (Session 44+45)
- **文件**: `core/params.hpp:127-134`, `sieve/lattice_sieve.hpp:141-143,179`
- **描述**: 两个子问题:
  1. **区域**: P0 修正后已缩小 (15K×3750 vs 旧 16K×16K)，但仍有优化空间
  2. **sieve_parallel 内存**: 每线程构造完整 `LatticeSieve` 对象（含独立 sieve_array + resize）；每 SQ 全量 `std::fill` 清零
- **优化方向**:
  - 预分配 `num_threads` 个 LatticeSieve 对象复用（避免反复构造/析构）
  - 增量清零（只清上个 SQ 的脏位置）

### [OPT] Kleinjung 多项式选择 + 管线集成
- **发现日期**: 2026-03-11 (Session 45), 原始 2026-02-20 (Session 2)
- **文件**: `polynomial/kleinjung_selector.hpp`, `tests/test_gnfs_e2e.cpp:178`, `tests/test_gnfs_progressive.cpp:176`
- **描述**: 双重问题:
  1. **管线未集成**: E2E/progressive 对所有 N 无条件用 `BaseMSelector::select()`，50+ 位需 Murphy E，80+ 位需 Kleinjung
  2. **实现质量**: search_radius=100、暴力循环（非格筛）、无 α 值评估、系数边界过松
- **建议**: 添加按位数分发的 `select_polynomial()`；100+ 十进制位用格筛搜索（参考 CADO-NFS `polyselect`）
- **预期收益**: 更好多项式可减少筛选时间 2-5×

---

---

## P2 — 中优先级

### [FEAT] SGE 预处理（100+ 位必须）
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `linalg/` — 完全不存在
- **描述**: BL 前做部分消元：weight-1 列→消去行列；weight-2 列→合并两行。3-5 轮收敛，矩阵降维 30-60%
- **预期收益**: 100K+ 矩阵 BL 时间减少 30-60%

### [OPT] base-m 搜索窗口 ±5（无 Murphy E 排名）
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `src/polynomial/base_m.cpp:69`
- **描述**: 搜索仅 11 个候选 (δ∈{0,±1,...,±5})，无 Murphy E 排名
- **建议**: 50+ 位时 δ≈1000，所有不可约候选评估 Murphy E 选最优
- **注**: skewness 硬编码已修复 (Session 53)

### [OPT] 2LP Cofactorization: 大 cofactor 应用 ECM
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `cofactor/smooth_check.hpp:232-255`
- **描述**: `classify_cofactor()` 对 `fits_uint64()` 的 cofactor 只用 `pollard_rho(c, 100000)` + 19 个 c 值。30+ bit cofactor（2LP 常见）ECM 更高效
- **建议**: cofactor > 2^35 时切换 ECM 路径

### [OPT] Clique removal 仅处理大素数侧 singleton
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `relation/filter.hpp:71-86,148-199`
- **描述**: `filter_pass()` 只对大素数做 singleton removal，factor base 侧未处理。短期扩展为全列 singleton；中期由 SGE 处理

### [OPT] NEON SIMD 加速
- **发现日期**: 2026-02-20 (Session 2), 补充 Session 45
- **描述**: ARM NEON 加速三个方向:
  - Sieve: `vqsubq_u16` 减对数、`vminvq_u16` 候选扫描
  - LinAlg: `veorq_u64` GF(2) XOR
  - BL `xor_with_mul_par` ctz 循环 (`block_lanczos.cpp:208-217`) → 4-bit lookup table (2-4×)

---

## P3 — 低优先级

### 小优化

#### [OPT] Pollard rho 无批量 GCD
- **文件**: `cofactor/smooth_check.hpp:143-180`
- **描述**: 每迭代一次 `gcd()`，Brent 改进每 128 步批量 GCD 可 5-10× 加速

#### [OPT] Alpha 缺少判别式双根贡献
- **文件**: `polynomial/murphy_evaluator.hpp:126-151`
- **描述**: p | disc(f) 时双根贡献应为 `log(p)/(p(p-1))` 而非 `log(p)/p`，二阶效应

#### [OPT] BL Gaussian fallback 用 vector\<bool\> 低效
- **文件**: `src/linalg/block_lanczos.cpp:291-365`
- **描述**: ≤1000 才触发，影响有限，应改用 packed bitset 或标记 deprecated

#### [OPT] 关系数 vs 列数余剩检查未应用
- **文件**: `relation/filter.hpp:349-367`
- **描述**: `required_relations()` 函数存在但几乎不被调用，管线只做最低限度 `has_excess()` 检查

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

### 代码质量

#### [DEBT] large_prime_bits 参数字段
- **文件**: `core/params.hpp:15-56`
- **描述**: LP bound 存 raw uint64，应增加 `large_prime_bits` 字段（与 CADO-NFS 一致）

#### [DEBT] log_scale 分散在三处
- **文件**: `core/params.hpp`, `factor_base/builder.hpp`, `sieve/lattice_sieve.hpp`
- **描述**: 三者独立维护默认值，Session 44 用 `SIEVE_LOG_SCALE=16` 常量临时修复

#### [DEBT] test_25digit.cpp 预期因子注释错误
- **文件**: `tests/test_25digit.cpp:44`

#### [DEBT] uint64_t b → int64_t 截断（13 处）
- **发现**: Session 5 | b 始终远小于 INT64_MAX，理论风险

#### [DEBT] Schirokauer 文档注释与代码不一致
- **文件**: `linalg/schirokauer.hpp:138`

#### [DEBT] SmallVector 缺少边界检查
- **文件**: `util/small_vector.hpp:96-103`

#### [DEBT] FactorBase 缺少序列化
- **文件**: `factor_base/factor_base.hpp:137-141`

#### [DEBT] Relation 序列化格式（无版本/校验和）
- **文件**: `core/relation.hpp:73-144`

#### [DEBT] -Wconversion 清理（~60 处）
- **发现**: Session 18 | 大多 cosmetic

#### [DEBT] 根目录遗留文件清理

### 调查

#### [RISK] sieve_parallel + 高 threshold 下 cofactorizer 通过率异常
- **发现日期**: 2026-03-11 (Session 44)
- **描述**: threshold=84 + sieve_parallel 256 SQs 时 ~100% 通过率（单线程同配置仅 0.06%），需调查

---

## TEST — 测试覆盖率缺口

（当前无未解决条目。历史记录见 RESOLVED.md）
