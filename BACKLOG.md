# BACKLOG — 待办备忘录

> 只记录**未完成**的问题。已完成和误报条目见 `RESOLVED.md`。
> 严重程度排序：P0 > P1 > P1-OPT > P2 > P3 > TEST。从文件开头往下读即为优先级。
> **Session 47**: BL 三项调优 + 1LP merge 修复 + progressive merge 集成。
> **Session 46**: P0 参数体系全面校准 (913s→347s), P1 LargePrimeKey + 阈值联动。

---

---

## P1-OPT — 高优先级性能优化

### [OPT] Hensel Sqrt 架构重构：单素数最终精度 → Nguyen 多素数 CRT
- **发现日期**: 2026-03-11 (Session 44+45 综合)
- **文件**: `include/gnfs/sqrt/hensel_sqrt.hpp`, `include/gnfs/sqrt/algebraic_sqrt.hpp`
- **描述**: 当前架构的核心问题——在单个 p 的最终精度 p^{2^k}（327K-2.6M bit）下计算 ~10K 因子的乘积，导致每次模乘涉及百万位 GMP 整数。25-digit: 437s (48%)
- **具体瓶颈** (5 项):
  1. **大积在最终精度计算** (`hensel_sqrt.hpp:305-332`): 应在小模数 pᵢ 上分别算（永不膨胀）
  2. **每 dep 独立重算大积** (`algebraic_sqrt.hpp:85-95`): 5 个 dep 共享大部分关系，差异可增量计算
  3. **每 attempt 重算** (`hensel_sqrt.hpp:149-242`): 4 次重试各自重算完整大积，应提升到 attempt 循环外
  4. **无 Nguyen 多素数 CRT**: 选 k 个素数分别在小模数 Fₚᵢ 中算乘积+lifting+CRT 合并——核心改进
  5. **lifting 内层 5 次 poly_mul_mod** (`hensel_sqrt.hpp:336-395`): 模数指数增长，每步代价翻倍；Nguyen 方法下每个 pᵢ 的模数远小于当前
- **预期收益**: Nguyen 改造后 437s → **几秒**（100× 加速）
- **参考**: 优化指南 §2.7.3, Nguyen (2004)

### [OPT] 管线固定使用 base-m 多项式，忽略质量选择
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `tests/test_gnfs_e2e.cpp:178`, `tests/test_gnfs_progressive.cpp:176`
- **描述**: E2E 和 progressive 管线对所有 N 无条件调用 `BaseMSelector::select()`。50+ 十进制位 N 需要 Murphy E 评估和 m 附近搜索；80+ 位需要 Kleinjung 选择。当前 `KleinjungSelector` 已实现但未集成到管线
- **影响**: 更好的多项式可减少筛选时间 2-5×
- **建议**: 添加按位数分发的 `select_polynomial()` 函数

### [OPT] 筛选区域过大 + sieve_parallel 内存模型
- **发现日期**: 2026-03-11 (Session 44+45 综合)
- **文件**: `include/gnfs/core/params.hpp:127-134`, `include/gnfs/sieve/lattice_sieve.hpp:141-143,179`
- **描述**: P0 修正后区域已大幅缩小（15K×3750 vs 旧 16K×16K），但以下优化仍有价值:
  - `sieve_parallel` 每线程构造完整 `LatticeSieve` 对象（含独立 sieve_array）
  - 每 SQ 调用 `init_sieve_array()` 全量 `std::fill`
- **优化方向**:
  - 线程间预分配重用 sieve 对象（避免反复构造/析构）
  - 增量清零（只清上个 SQ 实际用到的脏位置）

### [OPT] Bucket Sieve 架构（100+ 位必须）
- **发现日期**: 2026-02-20 (Session 2), 更新 2026-03-11 (Session 45)
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:221-408`
- **描述**: 当前所有因子基素数（含大素数）统一使用步长 p 逐点减对数。当 p > L1/L2 cache 对应跨度时，步长 p 的访存导致严重 cache miss。应分桶处理：将筛区间分成 ~L1 cache 大小的桶，预计算大素数命中点，按桶顺序处理
- **阈值**: 设 `p_bucket ≈ sieve_i_width / 64`（约 2K-10K），大于此值走 bucket 路径
- **预期收益**: 因子基 10⁵+ 时 3-10× 加速
- **优先级提升说明**: 从 P2 提升为 P1-OPT，因为 80+ 位分解（目标位数范围）已需要数十万因子基

---

## P2 — 中优先级（大数支持和架构改进）

### [FEAT] 2LP 关系合并（80+ 位必须）
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `include/gnfs/relation/filter.hpp:276-278`
- **描述**: `merge()` 仅处理 1LP×1LP 合并，2LP 关系完全跳过。`cofactorizer.hpp:49-51` 已统计 `partial_2lp`（说明 2LP 已在收集），但 filter 不利用。80+ 位时 2LP 是主要关系源
- **建议**: 基于图论实现——大素数为节点，关系为边，找共享节点的边对合并。参考 CADO-NFS `filter/merge.c`

### [FEAT] SGE 预处理（Structured Gaussian Elimination, 100+ 位必须）
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `include/gnfs/linalg/` 全局——完全不存在
- **描述**: BL 前对矩阵做部分消元：找 weight-1 列（只在 1 行出现）→消去该行该列；找 weight-2 列→合并两行。反复 3-5 轮收敛，矩阵降维 30-60%
- **预期收益**: 对 100K+ 矩阵（80+ 位），BL 时间线性减少 30-60%

### [OPT] Murphy E 公式多处缺陷（导致低估 20-40%）
- **发现日期**: 2026-03-08 (Session 5), 细化 2026-03-11 (Session 45)
- **文件**: `include/gnfs/polynomial/murphy_evaluator.hpp`
- **描述**: 三个独立公式问题:
  1. **Alpha 贡献使用任意 `/10.0`** (L107): 应在 Dickman rho 参数中减去 alpha（`u_f = (log|F| - α_f) / log B`），而非事后除 10 加到 E-score
  2. **采样区域用 sqrt(N)** (L329-338): b 范围应匹配实际筛区间 `sieve_j_max`，而非 `sqrt(N)/skewness`（远大于实际筛区域）
  3. **Dickman rho 渐近公式有额外项** (L254-258): `+0.5*log(2πu) - u` 不在标准 Hildebrand 公式中
- **影响**: 多项式排名可能被反转，影响 50+ 位时的多项式选择质量

### [OPT] base-m 搜索窗口仅 ±5 且 skewness 硬编码 1.0
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `src/polynomial/base_m.cpp:69,114`
- **描述**: 搜索窗口 `deltas[] = {0,1,-1,...,5,-5}` 仅 11 个候选，无 Murphy E 排名（第一个不可约即返回）。`create_context()` 固定 `skewness=1.0`，但 `PolynomialOptimizer::estimate_skewness()` 已实现却未调用
- **建议**: 50+ 位时 δ≈1000，对所有不可约候选评估 Murphy E 选最优；调用 estimate_skewness()

### [OPT] 2LP Cofactorization 对 64 位 cofactor 用 Pollard rho 而非 ECM
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `include/gnfs/cofactor/smooth_check.hpp:232-255`
- **描述**: `classify_cofactor()` 对 `fits_uint64()` 的 cofactor 只用 `pollard_rho(c, 100000)` + 19 个 c 值。对 30+ bit cofactor（2LP 场景常见），ECM 更高效
- **建议**: cofactor > 2^35 时切换到 ECM 路径

### [OPT] Clique removal 仅处理大素数侧 singleton
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `include/gnfs/relation/filter.hpp:71-86,148-199`
- **描述**: `filter_pass()` 只对大素数做 singleton removal，factor base 侧 singleton 未处理。设计上应由 SGE 在矩阵构建后处理，但 SGE 未实现
- **建议**: 短期在 filter 中扩展为全列 singleton removal；中期实现 SGE

### [OPT] sieve_parallel 每线程每次构造完整 LatticeSieve 对象
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:141-143`
- **描述**: 每个 worker lambda 内 `LatticeSieve local_sieve(ctx_, fb_, params_)` 触发 sieve_array resize。应预构造 T 个对象，线程间复用
- **建议**: 在 `sieve_parallel()` 入口预分配 `num_threads` 个 LatticeSieve，通过索引分配

### [FEAT] NEON SIMD 加速
- **发现日期**: 2026-02-20 (Session 2)
- **描述**: ARM NEON 加速 sieve（`vqsubq_u16` 减对数、`vminvq_u16` 候选扫描）和 linalg（`veorq_u64` GF(2) XOR）
- **补充 (Session 45)**: BL `xor_with_mul_par` 的 ctz bit-scan 循环 (`block_lanczos.cpp:208-217`) 可用 4-bit/8-bit lookup table 替代，提速 2-4×

### [OPT] Kleinjung 多项式选择质量提升
- **发现日期**: 2026-02-20 (Session 2), 细化 2026-03-11 (Session 45)
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp`
- **描述**: 扩展原条目:
  1. search_radius 硬编码 100、系数边界过松
  2. Stage 1 是暴力循环而非真正的格筛 (L207-303)
  3. 选择标准仅检查系数大小，无 α 值或根属性评估
  4. 未集成到 E2E/progressive 管线（只有 base-m 被调用）
- **建议**: 100+ 十进制位时需要真正的格筛搜索，参考 CADO-NFS `polyselect`

### [FEAT] Out-of-core Relations 支持
- **发现日期**: 2026-02-20 (Session 2)
- **文件**: `include/gnfs/relation/collector.hpp`
- **描述**: 50+ 十进制位 N 需要 10-100M 关系（数 GB），当前全部在内存

### [FEAT] Block Lanczos Out-of-core 矩阵支持
- **发现日期**: 2026-03-08 (Session 5)
- **描述**: 矩阵必须完全在 RAM 中

### [FEAT] ThreadPool Work-Stealing
- **发现日期**: 2026-03-08 (Session 5)
- **描述**: 筛选 special-Q 开销不均匀

### [OPT] Hensel Sqrt 次级优化（Nguyen 改造前也可做）
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `include/gnfs/sqrt/hensel_sqrt.hpp`
- **描述**: 即使不做 Nguyen 重构，以下优化也有收益:
  1. **链式乘法 → subproduct tree** (L560-590): 二叉树分治合并，前期节省系数膨胀
  2. **partial products 合并串行** (L660-665): 改为树状并行合并（log(T) 轮）
  3. **Couveignes verify 无增量 evaluate** (`couveignes.hpp:296-317`): Gray code 每步只改一个系数，可增量更新 evaluate_at_m_mod_n

---

## P3 — 低优先级（代码质量和长期改进）

### [FEAT] CSR 矩阵格式（100K+ 行时必需）
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `include/gnfs/linalg/sparse_matrix.hpp:179-315`
- **描述**: 当前 `SparseMatrix` = `vector<SparseRow>`，每行独立 heap 分配，cache 不友好。应改为 CSR（连续 `col_indices` + `row_offsets`），利于 prefetch 和 SIMD。100K+ 行（80+ 位 N）时影响显著

### [FEAT] Block Wiedemann 实现（130+ 位）
- **发现日期**: 2026-03-11 (Session 45)
- **描述**: 矩阵 >5M 时 BL 顺序迭代成为瓶颈，BW 的 SpMV 可分布式并行。远期工作

### [DEBT] 无 large_prime_bits 参数字段
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `include/gnfs/core/params.hpp:15-56`
- **描述**: LP bound 存为 raw uint64，应增加 `large_prime_bits` 字段（与 CADO-NFS/msieve 参数表一致，更直观）

### [DEBT] 无行筛实现（小 N 格筛有额外开销）
- **发现日期**: 2026-03-11 (Session 45)
- **描述**: 所有 N 都走格筛，包括 N=143。对 <50 十进制位，行筛更简单高效。非紧急，因为格筛对小 N 也能工作

### [OPT] Pollard rho 每步 GCD 无批量优化
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `include/gnfs/cofactor/smooth_check.hpp:143-180`
- **描述**: `pollard_rho()` 每迭代调用一次 `gcd()`。标准 Brent 改进：每 128 步累积乘积再做一次 GCD，5-10× 加速

### [OPT] find_inert_prime 每 dep 重复搜索
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `include/gnfs/sqrt/hensel_sqrt.hpp:529-544`
- **描述**: 惰性素数只依赖 f(x)，不依赖 ab_pairs，同一多项式所有 dep 搜索结果相同。应缓存

### [OPT] poly_mul_mod 每次调用重算 f_lead_inv
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `include/gnfs/sqrt/hensel_sqrt.hpp:696-711`
- **描述**: lifting 每轮 5 次 `poly_mul_mod`，每次对同参数做 `mpz_invert`。应提升到循环外

### [OPT] Alpha 缺少判别式双根贡献
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `include/gnfs/polynomial/murphy_evaluator.hpp:126-151`
- **描述**: p | disc(f) 时双根贡献应为 `log(p)/(p(p-1))` 而非 `log(p)/p`。二阶效应

### [OPT] BL Gaussian fallback 用 vector\<bool\> 低效
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `src/linalg/block_lanczos.cpp:291-365`
- **描述**: `find_dependencies_gaussian` 用 `vector<vector<bool>>` 存增广矩阵（逐 bit 操作）。阈值 ≤1000 才触发，影响有限，但应标记为 deprecated

### [OPT] 关系数 vs 列数余剩检查未在管线中应用
- **发现日期**: 2026-03-11 (Session 45)
- **文件**: `include/gnfs/relation/filter.hpp:349-367`
- **描述**: `required_relations()` utility 函数存在但几乎不被调用；管线只做最低限度 `has_excess()` 检查

### [DEBT] log_scale 分散在三个配置结构体中
- **发现日期**: 2026-03-11 (Session 44)
- **文件**: `include/gnfs/core/params.hpp`, `include/gnfs/factor_base/builder.hpp`, `include/gnfs/sieve/lattice_sieve.hpp`
- **描述**: `GNFSParams.log_scale`、`FactorBaseBuilder::Options.log_scale`、`SieveParams.log_scale` 三者独立维护各自默认值。Session 44 临时修复为 `SIEVE_LOG_SCALE=16` 常量，根本解法见描述中方案 A/B/C

### [RISK] sieve_parallel + 高 threshold 下 cofactorizer 通过率异常
- **发现日期**: 2026-03-11 (Session 44)
- **描述**: threshold=84 (combined) + sieve_parallel 256 SQs 时 ~100% cofactorizer 通过率（单线程同配置仅 0.06%）。需进一步调查

### [DEBT] test_25digit.cpp 预期因子注释错误
- **发现日期**: 2026-03-11 (Session 44)
- **文件**: `tests/test_25digit.cpp:44`
- **描述**: 注释因子与 N 量级不符

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
- **描述**: `-Wconversion` 下 ~60 处 sign-conversion，大多 cosmetic

### [DEBT] 根目录遗留文件清理

---

## TEST — 测试覆盖率缺口

（当前无未解决条目。历史记录见 RESOLVED.md）
