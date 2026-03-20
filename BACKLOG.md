# BACKLOG — 待办备忘录

> 记录发现但当前不处理的问题。每个条目经过 Session 8 全面核查（5 个 Agent 并行验证源码）。
> 严重程度排序：P0 > P1 > P2 > P3 > TEST。已完成/误报条目在末尾。

---

## P0 — 正确性关键（必须修复）

### [BUG] 代数侧大素数映射仅按 p 索引，忽略根 r——不同素理想被合并为同一列
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6)
- **核查结果**: ✅ 真实 P0
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:350,363-364,579-591`
- **描述**: `collect_large_primes()` 和 `build_row()` 将代数大素数按 `p` 键入 `alg_lp_to_col`，不包含根 `r`。同一素数 p 上方多个不同素理想 (p,α-r₁), (p,α-r₂) 被合并到同一列，违反代数侧因子分解唯一性
- **影响**: 矩阵中不同素理想被错误合并 → 虚假 GF(2) 依赖 → 平方根非完全平方 → 因式分解失败率增加。对 d>3 更严重
- **建议**: 键改为 `(p, r)` 对

### [BUG] compute_log_prime() 系统性低估所有素数对数值
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6)
- **核查结果**: ✅ 真实，但降级为 P1（有 precise 版本可用）
- **文件**: `include/gnfs/factor_base/factor_base.hpp:175-181`
- **描述**: `compute_log_prime()` 计算 `floor(log2(p)) * scale` 而非 `floor(log2(p) * scale)`。p=5, scale=16 时偏差 15.6%。旁边有 `compute_log_prime_precise()` 但 builder.cpp 三处调用不精确版
- **影响**: 筛法中素数 log 贡献被系统低估，影响候选筛选
- **建议**: builder.cpp 改用 `compute_log_prime_precise()`

### [BUG] Couveignes rat_sqrt 对合数 N 计算根本性错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ⚠️ 部分正确（数学不正确但实际作为启发式工作）
- **文件**: `include/gnfs/sqrt/couveignes.hpp:311-329`
- **描述**: `powmod(rat_product, (N+1)/4, N)` 假设 N ≡ 3 mod 4 且 N 是素数，但 N 是合数
- **影响**: 降低 Couveignes 找到正确符号的成功率。有理平方根应通过 rational_sqrt.hpp 独立计算
- **建议**: 使用因子指数直接累积有理平方根

### [BUG] rational_sqrt 验证函数声称验证但实际什么也不做
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P1
- **文件**: `include/gnfs/sqrt/rational_sqrt.hpp:143-162`
- **描述**: `verify=true`（默认）时只有空 for 循环和 `// TODO: 完整验证`。函数总是返回 `success=true`
- **影响**: 错误的有理平方根不会被检测到

---

## P1 — 高优先级（影响正确性或大数支持）

### [BUG] Split Schirokauer: f mod 2 可约时映射计算错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-02-20 (Session 2)
- **核查结果**: ✅ 真实 P1（已知问题，有 TODO 代码）
- **文件**: `include/gnfs/linalg/schirokauer.hpp`
- **描述**: f mod 2 可约时需要 valuation stripping，当前部分实现有 bug。degree-2 factor 提升代码标注为 "less precise but functional for k=3, ell=2"
- **当前规避**: 选择 f mod 2 不可约的测试 N

### [BUG] ECM sieve_primes(B2) 内存爆炸
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ✅ 真实 P1
- **文件**: `include/gnfs/cofactor/ecm.hpp:405`
- **描述**: `sieve_primes(B2)` 分配 B2 个 bool。B2=5e9 时 ~625MB，B2=1e10 时无法分配
- **建议**: 分段筛法或 Miller-Rabin 测试

### [BUG] Sieve 区域对大 N 导致灾难性内存分配（>100GB）
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P1
- **文件**: `include/gnfs/core/params.hpp:128-137`
- **描述**: sieve_width 上限 1e6, height = width/4 = 250K。总面积 = 250 billion × 2 bytes = 500GB
- **影响**: 30+ digit N 直接 OOM
- **建议**: cap 应对总面积而非仅宽度

### [BUG] RelationCollector callback 在非递归 mutex 下调用——回调内访问 collector 死锁
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P1
- **文件**: `include/gnfs/relation/collector.hpp:291-294`
- **描述**: `add()` 持有 `mutex_` 时调用 callback，callback 若调用 `size()` 等方法会死锁（非 recursive_mutex）
- **建议**: 改用 `std::recursive_mutex` 或 mutex 外调用 callback

### [BUG] Couveignes 回退公式 (N+1)/2 对所有 N 都数学错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P1
- **文件**: `include/gnfs/sqrt/couveignes.hpp:325-328`
- **描述**: `a^((N+1)/2) = a * Legendre(a,N)` 对合数 N 无定义。Couveignes 回退路径 100% 失败

### [BUG] algebraic_sqrt compute_heuristic() 数学不正确
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ✅ 真实 P1
- **文件**: `include/gnfs/sqrt/algebraic_sqrt.hpp:173-195`
- **描述**: `elem^((n+1)/2)` 在 Q[α]/f(α) 中不是有效的平方根公式

### [BUG] Couveignes 符号归一化不一致导致 CRT 重建错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ✅ 真实 P1
- **文件**: `include/gnfs/sqrt/couveignes.hpp:510-524`
- **描述**: 不同素数用不同阈值 p/2 做符号归一化，CRT 重建错误

### [BUG] Couveignes Gray Code 系数漂移——翻转间无 mod M 约化
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6)
- **核查结果**: ✅ 真实 P1
- **文件**: `include/gnfs/sqrt/couveignes.hpp:415-437`
- **描述**: 65536 次 Gray code 迭代中每次加减 two_weights 但从不做 mod M 约化，系数漂移

### [BUG] matrix_builder f mod 2 检查在大系数时 uint64 截断
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ✅ 真实 P1
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:174-184`
- **描述**: 先 `c.to_uint64()` 再 mod 2，大系数截断导致可约性误判

### [BUG] matrix_builder 多项式度 > 8 时数组越界
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ✅ 真实 P1
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:416`
- **描述**: `FastPoly::MAX_DEGREE = 8`，`select_qc_primes()` 未验证 degree ≤ 8

### [BUG] matrix_builder QC 系数 int64 截断
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ✅ 真实 P1
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:387-388`
- **描述**: `ctx.coeff(i).to_int64()` 大系数截断

### [BUG] Schirokauer FastPoly 系数潜在溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ✅ 真实 P1
- **文件**: `include/gnfs/linalg/schirokauer.hpp:73,87-91`
- **描述**: `reduce_inplace()` 中 `t > a.coeffs[idx]` 时公式错误

### [BUG] Schirokauer factorize_and_setup 重复根处理
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ⚠️ 部分正确 P2（ℓ=2 时不太可能出现重复根）
- **文件**: `include/gnfs/linalg/schirokauer.hpp`
- **描述**: ℓ | disc(f) 时重复根未正确处理

### [BUG] Hensel poly_inverse_mod_direct p^d uint64 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P1
- **文件**: `include/gnfs/sqrt/hensel_sqrt.hpp:529-531`
- **描述**: uint64_t 计算 p^d-2，d=6,p=2000 时溢出

### [BUG] base_m.cpp select() 不验证 f 不可约
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6)
- **核查结果**: ✅ 真实 P1（但低次多项式可约概率极低）
- **文件**: `src/polynomial/base_m.cpp:8-37`
- **描述**: 不检查 f 在 Q[x] 上不可约性

### [BUG] modular_poly sub()/mod_inverse() 对 p > INT64_MAX 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ✅ 真实但实际不可达（所有素数 < 2^32）
- **文件**: `include/gnfs/sqrt/modular_poly.hpp:90,420`
- **描述**: uint64 系数 cast 到 int64 相减

---

## P1-OPT — 高优先级性能优化

### [OPT] Block Lanczos 是 25-digit 的主要瓶颈（需并行化）
- **状态**: 🔴 待处理
- **发现日期**: 2026-02-22 (Session 3)
- **文件**: `src/linalg/block_lanczos.cpp`
- **描述**: SpMV 顺序执行，193s = 81.6% 耗时
- **建议**: OpenMP 并行 SpMV

### [OPT] ECM Stage 2 BSGS 优化
- **状态**: 🔴 待处理
- **发现日期**: 2026-02-20 (Session 2)
- **文件**: `include/gnfs/cofactor/ecm.hpp:410-430`
- **描述**: 朴素 O(π(B2))，应优化为 BSGS O(√(B2/B1))

### [OPT] lattice_basis 浮点高斯约化不够精确
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/sieve/lattice_basis.hpp:83-109`
- **描述**: double 精度在 |e0|, |f0| ~ 10^18 时误差 ~ 10^3

---

## P2 — 中优先级（大数支持和架构改进）

### [BUG] SparseRow const_cast ensure_sorted() 多线程并发排序是 UB
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7)
- **核查结果**: ✅ 真实 P2
- **文件**: `include/gnfs/linalg/sparse_matrix.hpp:61-63,93,134,150`
- **描述**: const_cast 修改 indices_ 和 sorted_，Block Lanczos SpMV 并发读取同一行时 UB
- **建议**: 标记为 `mutable`，构造后立即排序

### [BUG] MurphyEvaluator rng_ 数据竞争——Kleinjung 多线程并行使用
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P2
- **文件**: `include/gnfs/polynomial/murphy_evaluator.hpp:337` + `include/gnfs/polynomial/kleinjung_selector.hpp:133,167`
- **描述**: 多线程共享 rng_ 产生数据竞争
- **建议**: 使用 thread_local rng

### [BUG] Cofactorizer::stats_ 无 mutex 保护
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P2
- **文件**: `include/gnfs/cofactor/cofactorizer.hpp:137,140,143,172`
- **描述**: verify() 多处修改 stats_ 无同步

### [BUG] ECM Stage 2 链式乘法增加因子丢失概率
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6)
- **核查结果**: ✅ 真实 P2
- **文件**: `include/gnfs/cofactor/ecm.hpp:410-413`
- **描述**: chaining 使 Z 坐标在两侧都变为 0 → gcd=N → 因子丢失

### [BUG] ECM 固定随机种子导致重复曲线
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ✅ 真实 P2（设计选择但限制了 ECM 效果）
- **文件**: `include/gnfs/cofactor/ecm.hpp:49`
- **描述**: `std::mt19937_64 rng(42)` 硬编码种子

### [BUG] class_group SNF 不是真正的 Smith Normal Form
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ✅ 真实 P2
- **文件**: `include/gnfs/sqrt/class_group.hpp:450-517`
- **描述**: 只做了 Gaussian 消元，类数用 `1u << generators.size()` 近似

### [BUG] class_group 判别式计算仅对 d=3 depressed cubic 正确
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ✅ 真实 P2（class group 本身是启发式近似）
- **文件**: `include/gnfs/sqrt/class_group.hpp:165-209`
- **描述**: d>3 返回启发式值；d=3 公式忽略 x² 系数

### [BUG] class_group 判别式公式对非 depressed 三次多项式错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P2
- **文件**: `include/gnfs/sqrt/class_group.hpp:168-188`
- **描述**: Δ = -4a³ - 27b² 仅对 x³ + ax + b 正确，忽略 coeff(2)

### [BUG] estimate_initial_log NaN/Inf → uint16_t 强制转换是 UB
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7)
- **核查结果**: ✅ 真实 P2
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:221-231`
- **描述**: `log2(0) = -Inf`, `static_cast<uint16_t>(-Inf)` 是 UB
- **建议**: 添加 `if (!std::isfinite(combined) || combined < 0) return 0;`

### [BUG] estimate_initial_log typical_i/typical_j 不一致
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6)
- **核查结果**: ✅ 真实 P2
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:213-214`
- **描述**: typical_i = 半宽度，typical_j = 中点，不一致导致初值偏高

### [BUG] params.hpp special_q_min = rational_bound/5 落入因子基范围
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6)
- **核查结果**: ✅ 真实 P2（性能影响，非正确性）
- **文件**: `include/gnfs/core/params.hpp:165`
- **描述**: 80% special-Q 在 FB 内部，筛选效率低
- **建议**: 改为 `special_q_min = algebraic_bound + 1`

### [BUG] Kleinjung base_m_expansion 系数不平衡
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P2（影响多项式质量，不影响正确性）
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp:466-478`
- **描述**: 截断除法产生 [0,m) 系数，应平衡到 [-m/2,m/2]

### [BUG] SmallVector move constructor 不销毁源对象的 inline 元素
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ⚠️ 部分正确 P2（仅影响非平凡析构类型，当前主要用于 POD）
- **文件**: `include/gnfs/util/small_vector.hpp:43-54,67-79`
- **描述**: move 后源元素析构函数未调用

### [BUG] RelationCollector::merge() 未锁定 other 的 mutex
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P2
- **文件**: `include/gnfs/relation/collector.hpp:227`
- **描述**: 读取 other.relations_ 不加锁

### [BUG] RelationCollector::set_callback() 无 mutex 保护
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7)
- **核查结果**: ✅ 真实 P2
- **文件**: `include/gnfs/relation/collector.hpp:249-251`
- **描述**: set_callback() 与 add() 数据竞争

### [BUG] Pollard rho 只使用单一多项式 x²+1
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P2（降低成功率但不影响正确性）
- **文件**: `include/gnfs/cofactor/smooth_check.hpp:148`
- **描述**: 固定 c=1，某些 n 值永远找不到因子

### [BUG] lattice_sieve 模运算中间值 int64 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ✅ 真实 P2（大 q 时可触发但当前测试规模安全）
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:268-269`
- **描述**: f0*m_mod_p 可溢出 int64

### [BUG] SieveRegion default_sieve_region 大 skewness 时 int32 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P2（skewness > 1e10 时触发）
- **文件**: `include/gnfs/sieve/lattice_basis.hpp:175-178`
- **描述**: base_size * factor 溢出 int32

### [BUG] Kleinjung is_valid_polynomial() 浮点验证无意义
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ✅ 真实 P2
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp:505-512`
- **描述**: double 精度对大 N 无效
- **建议**: 用 Integer 精确验证 `f(m) == N`

### [BUG] 代数因子基射影根在筛选中产生算术垃圾
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P2（射影根较少，影响有限）
- **文件**: `include/gnfs/factor_base/factor_base.hpp:78` + `include/gnfs/sieve/lattice_sieve.hpp:342`
- **描述**: PROJECTIVE_ROOT = UINT32_MAX 被当作普通根处理

### [BUG] IntPolynomial::roots_cantor_zassenhaus 实际是 O(p) 暴力搜索
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P2（名字误导 + 大 p 时慢）
- **文件**: `include/gnfs/polynomial/int_polynomial.hpp:384-405`
- **描述**: 名叫 CZ 但实现是暴力枚举

### [BUG] Integer 除零行为不一致
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6)
- **核查结果**: ✅ 真实 P2
- **文件**: `src/core/integer.cpp`
- **描述**: Integer 除零 → GMP abort（不可捕获），int64_t 除零 → domain_error

### [BUG] Hensel 提升无精度充分性验证
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ⚠️ 部分正确 P2（200 位余量通常足够）
- **文件**: `include/gnfs/sqrt/hensel_sqrt.hpp:226-240`
- **描述**: centering 后无验证，但 extra_precision=200 提供余量

### [BUG] FactorBaseBuilder 实例 build() 返回空 FactorBase
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7)
- **核查结果**: ✅ 真实 P2（已知限制但公开 API 不应静默失败）
- **文件**: `src/factor_base/builder.cpp:62-69`
- **描述**: 实例方法直接返回空结果，不抛异常

### [BUG] matrix_builder FB 索引无越界检查
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ⚠️ 部分正确 P2（防御性编程，上游应产生正确索引）
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:540,553`
- **描述**: FB 索引无上限检查

### [BUG] Schirokauer Hensel 提升 quadratic factor 访问未构建的 prime_info_
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P2
- **文件**: `include/gnfs/linalg/schirokauer.hpp:528`
- **描述**: prime_info_.back() 引用上一个 prime 的信息

### [FEAT] Bucket Sieve 架构
- **状态**: 🔴 待处理
- **发现日期**: 2026-02-20 (Session 2)
- **描述**: 大因子基需要 cache-friendly bucket sieve

### [FEAT] NEON SIMD 加速
- **状态**: 🔴 待处理
- **发现日期**: 2026-02-20 (Session 2)
- **描述**: ARM NEON 加速 sieve 和 linalg

### [OPT] Kleinjung 多项式选择质量提升
- **状态**: 🔴 待处理
- **发现日期**: 2026-02-20 (Session 2)
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp`
- **描述**: search_radius 硬编码 100、系数边界过松、缺少 lattice search

### [FEAT] Factor Base 支持 ramified/projective 素数
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `src/factor_base/builder.cpp`
- **描述**: builder 始终设 degree=1，不使用 projective root

### [FEAT] Out-of-core Relations 支持
- **状态**: 🔴 待处理
- **发现日期**: 2026-02-20 (Session 2)
- **文件**: `include/gnfs/relation/collector.hpp`
- **描述**: 50+ 位 N 需要 10-100M 关系（数 GB）

### [FEAT] Block Lanczos Out-of-core 矩阵支持
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **描述**: 矩阵必须完全在 RAM 中

### [DEBT] params.hpp 对 100+ 位 N 参数不足
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/core/params.hpp`
- **描述**: degree 上限 6、rational_bound 上限 1e9

### [FEAT] Relation Filter 完成 clique-based 合并
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/relation/filter.hpp:286-287`
- **描述**: merge 函数是 stub，从不实际合并

### [FEAT] ThreadPool Work-Stealing
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **描述**: 筛选 special-Q 开销不均匀

---

## P3 — 低优先级（代码质量和长期改进）

### [BUG] 4 处 static Integer zero 返回引用——别名 + 线程理论风险
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7)
- **核查结果**: ✅ 真实但降级为 P3（const& 返回，实际风险极低）
- **文件**: number_field.hpp:65,195 / polynomial_context.hpp:77 / int_polynomial.hpp:69
- **描述**: 越界访问返回 static Integer zero 的 const&，别名陷阱

### [BUG] Block Lanczos partial_inverse() 未将非主元行清零
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6)
- **核查结果**: ⚠️ 降级 P3（Montgomery BL 上下文中实际工作正确）
- **文件**: `include/gnfs/linalg/block_lanczos.hpp:119-157`
- **描述**: 非主元行有垃圾值，但 D*A 乘积中自动消除

### [BUG] Block Lanczos add_identity() 添加完整单位矩阵
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6)
- **核查结果**: ⚠️ 真实但影响仅在 A_i 秩亏时
- **文件**: `src/linalg/block_lanczos.cpp:356-361`
- **描述**: 应为 mask 子空间的单位矩阵

### [BUG] SparseMatrix::test() const_cast 违反 const 契约
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ⚠️ 部分正确 P3（单线程 OK，多线程见 P2 条目）
- **文件**: `include/gnfs/linalg/sparse_matrix.hpp:62-63`
- **描述**: const_cast 在 const 方法中修改内部状态

### [BUG] trial_division.hpp uint8_t 指数溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ⚠️ 降级 P3（GNFS 中素数 p^256 整除实际不可能发生）
- **文件**: `include/gnfs/cofactor/trial_division.hpp:62-64`
- **描述**: uint8_t exp 在 256 次自增后溢出

### [BUG] Relation::b 是 int64_t 但应为 uint64_t
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ⚠️ 降级 P3（风格问题，b 实际上从不超过 int64 范围）
- **文件**: `include/gnfs/core/relation.hpp:17`

### [BUG] RelationCollector::relations() 返回非 const 引用
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ⚠️ 降级 P3（实际有 const 版本，API 设计问题）
- **文件**: `include/gnfs/relation/collector.hpp:151-152`

### [BUG] params.hpp special_q_max 的 uint32 溢出
- **状态**: 🟢 当前安全
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/core/params.hpp:166`
- **描述**: cap 在 1e9 保护，rational_bound×2 < UINT32_MAX

### [BUG] rational_sqrt 负号检测到但未应用
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P3（extract_factors 同时检查 X±Y，浪费一次 GCD 但不影响结果）
- **文件**: `include/gnfs/sqrt/rational_sqrt.hpp:138-141`

### [BUG] number_field norm_linear 符号公式错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ⚠️ 被 abs() 掩盖，实际无影响
- **文件**: `include/gnfs/sqrt/number_field.hpp:372-406`
- **描述**: 计算 b^d * f(a/b) 而非 (-b)^d * f(a/b)，但最终取 abs()

### [BUG] class_group factor_ideal/factor_principal_ideal int64 乘法溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ⚠️ 降级 P3（b*r 各 ~10^9，乘积 ~10^18 接近但不超过 INT64_MAX）
- **文件**: `include/gnfs/sqrt/class_group.hpp:383,418`

### [BUG] base_m_expansion 非零余数处理
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ⚠️ 降级 P3（产生劣质多项式，不影响正确性）
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp:476-478`

### [BUG] build_row() 符号列基于 a<0 而非 (a-bm)<0
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ⚠️ 降级 P3（build_with_qc 修正了符号，实际管线不走 build_row）
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:527-530`

### [BUG] SparseRow::set() 非幂等——重复 set 等价于 clear
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实但 P3（GF(2) 语义正确，是 API 命名问题）
- **文件**: `include/gnfs/linalg/sparse_matrix.hpp`
- **描述**: 这实际上是 GF(2) toggle 语义，命名应改为 `toggle()`

### [BUG] BitVector::xor_with() 无大小检查
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **文件**: `include/gnfs/linalg/sparse_matrix.hpp:354-357`
- **描述**: other 更短时越界

### [BUG] multiply_blocks() 死代码——索引计算错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实但 P3（从未被调用，是死代码）
- **文件**: `include/gnfs/linalg/sparse_matrix.hpp:298-319`

### [BUG] gauss.hpp build_null_space() history 参数从未使用
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6)
- **核查结果**: ✅ 真实 P3（O(n²) 空间浪费）
- **文件**: `include/gnfs/linalg/gauss.hpp:161-219`

### [BUG] Block Lanczos 终止条件仅检查 V_cur 为零
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6)
- **核查结果**: ✅ 真实 P3（可能多几十次无效迭代）
- **文件**: `src/linalg/block_lanczos.cpp:336`
- **建议**: 添加 `if (mask_cur == 0) break;`

### [BUG] next_prime() uint64 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ⚠️ 降级 P3（理论问题，prime_start 默认 1000）
- **文件**: couveignes.hpp:619-628 + hensel_sqrt.hpp:550-562

### [BUG] Logger 递归日志死锁
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ⚠️ 降级 P3（无 callback 机制，无实际重入路径）
- **文件**: `include/gnfs/util/logger.hpp:140-141`

### [BUG] Logger::level() 读取 level_ 无锁
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实但 P3（uint8_t 读写在所有架构上原子）
- **文件**: `include/gnfs/util/logger.hpp:63`

### [BUG] SchirokaurMap 存储 const PolynomialContext&
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7)
- **核查结果**: ⚠️ 降级 P3（设计气味，实际 ctx 始终 outlive 使用者）
- **文件**: `include/gnfs/linalg/schirokauer.hpp:342`

### [BUG] LatticeSieve 存储 const 引用
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7)
- **核查结果**: ⚠️ 降级 P3（同上，实际 ctx/fb 始终 outlive sieve）
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:186-187`

### [BUG] Relation 反序列化无输入验证
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实但 P3（仅内部使用，无外部攻击面）
- **文件**: `include/gnfs/core/relation.hpp:104-148`

### [BUG] Integer operator+=/operator-= 对 INT64_MIN 参数 UB
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实但 P3（INT64_MIN 在管线中不会出现）
- **文件**: `src/core/integer.cpp:356-357,374-375,384`

### [BUG] MurphyEvaluator n.to_double() 对 N > 10^308 返回 infinity
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实但 P3（远超实现能力范围）
- **文件**: `include/gnfs/polynomial/murphy_evaluator.hpp:331`

### [BUG] MurphyEvaluator alpha 跳过大首项系数
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实但 P3（实际首项系数总是 fit uint64）
- **文件**: `include/gnfs/polynomial/murphy_evaluator.hpp:143-147`

### [BUG] IntPolynomial add_mod 大 p 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ⚠️ 降级 P3（所有素数 < 2^32）
- **文件**: `include/gnfs/polynomial/int_polynomial.hpp:362`

### [BUG] IntPolynomial mutable operator[] 无上限检查
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7)
- **核查结果**: ⚠️ 降级 P3（设计选择，实际调用点安全）
- **文件**: `include/gnfs/polynomial/int_polynomial.hpp:75-80`

### [BUG] polynomial_optimizer generate_smooth_numbers 不去重
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P3（重复无害，仅微量浪费）
- **文件**: `include/gnfs/polynomial/polynomial_optimizer.hpp:323-355`

### [BUG] Integer bit_length(0) 返回 1
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6)
- **核查结果**: ✅ 真实 P3（GMP 规范行为）
- **文件**: `src/core/integer.cpp:93-94`

### [BUG] Integer::sqrt() 对负数输入无检查
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6)
- **核查结果**: ✅ 真实 P3（所有调用点传正值）
- **文件**: `include/gnfs/core/integer.hpp`

### [BUG] Integer::powmod() 负指数
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6)
- **核查结果**: ⚠️ GMP 实际处理负指数（计算逆）
- **文件**: `include/gnfs/core/integer.hpp`

### [BUG] types.hpp ABPair 注释错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P3（仅文档）
- **文件**: `include/gnfs/core/types.hpp:11`
- **描述**: "a + b*m" 应为 "a - b*m"

### [BUG] matrix_builder exponent 累积用 uint8_t
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ⚠️ 降级 P3（256≡0 mod 2，溢出不影响 GF(2) 奇偶性）
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:534,548`

### [BUG] Kleinjung construct_polynomial 死代码
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P3（60 行被 base_m_expansion 覆写）
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp:380-439`

### [BUG] sieve_batch() 是死代码
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ⚠️ 部分正确 P3（stub 代码，从未被管线调用）
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:127-131`

### [BUG] SieveParams::combined_threshold() uint8_t 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实但 P3（当前参数最大 160，不触发）
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:37`

### [BUG] FactorBase::add_rational() 无去重
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ⚠️ 降级 P3（builder 使用筛法自然不重复）
- **文件**: `include/gnfs/factor_base/factor_base.hpp:94-98`

### [BUG] sieve_parallel() 使用不必要的 mutex
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7)
- **核查结果**: ⚠️ P3（性能浪费但不影响正确性）
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:153,168-169`

### [BUG] class_group factor_ideal val=0 时 exp=0
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6)
- **核查结果**: ⚠️ 部分正确 P3（val!=0 守卫阻止循环，但 a=b*r 时赋值丢失）
- **文件**: `include/gnfs/sqrt/class_group.hpp:383-393`

### [BUG] 无 N 素性检测
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6)
- **核查结果**: ✅ 真实 P3（可用性问题）
- **文件**: 管线入口

### [BUG] smooth_check 浮点精度损失
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/cofactor/smooth_check.hpp:107-108`
- **描述**: std::pow(double,1.0/k) 精度问题

### [OPT] Murphy E-score 低估 20-40%
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/polynomial/murphy_evaluator.hpp`

### [OPT] Gauss 消元 history 矩阵 O(n²) 空间
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/linalg/gauss.hpp`

### [DEBT] 全局性 uint64_t b → int64_t 截断（13 处）
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5)
- **核查结果**: ⚠️ 降级 P3（GNFS 中 b 值始终远小于 INT64_MAX）
- **文件**: 见原始条目（13 处文件列表）
- **描述**: 理论上 b > INT64_MAX 时截断，实际不会发生

### [DEBT] Schirokauer 文档注释与代码不一致
- **状态**: 🔴 待处理
- **文件**: `include/gnfs/linalg/schirokauer.hpp:138`

### [DEBT] polynomial_context coeff() 返回可变静态引用
- **状态**: 🔴 待处理
- **文件**: `include/gnfs/core/polynomial_context.hpp:76-80`

### [DEBT] SpecialQ from_indices 忽略 end_index 参数
- **状态**: 🔴 待处理
- **文件**: `include/gnfs/sieve/special_q.hpp:36`

### [DEBT] number_field evaluate 无溢出保护
- **状态**: 🔴 待处理
- **文件**: `include/gnfs/sqrt/number_field.hpp:328-343`

### [DEBT] PrimePowerHash 忽略指数字段
- **状态**: 🔴 待处理
- **文件**: `include/gnfs/core/types.hpp:87-92`

### [DEBT] SmallVector 缺少边界检查
- **状态**: 🔴 待处理
- **文件**: `include/gnfs/util/small_vector.hpp:96-103`

### [DEBT] FactorBase 缺少序列化
- **状态**: 🔴 待处理
- **文件**: `include/gnfs/factor_base/factor_base.hpp:137-141`

### [DEBT] Block Lanczos 阈值 AND 应为 OR
- **状态**: 🔴 待处理
- **文件**: `src/linalg/block_lanczos.cpp:284`

### [DEBT] Relation 序列化格式缺陷（无版本/校验和）
- **状态**: 🔴 待处理
- **文件**: `include/gnfs/core/relation.hpp:73-144`

### [DEBT] 根目录遗留文件清理
- **状态**: 🔴 待处理

### [DEBT] ThreadPool func 引用捕获
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7)
- **核查结果**: ❌ 误报（parallel_for 通过 future.get() 确保 func 存活）
- **文件**: `include/gnfs/util/thread_pool.hpp:104,140`

---

## TEST — 测试覆盖率缺口

### [TEST] 7 个模块无专属单元测试
- **缺失**: base_m, params, polynomial_context, polynomial_optimizer, int_polynomial, filter, class_group

### [TEST] 0 个已修复 bug 的回归测试
- **描述**: Session 1-3 修复的 10 个关键 bug 无专属回归测试

### [TEST] 边界/极端情况覆盖率约 5%
- **描述**: 缺失 Integer 溢出、负 mod、空矩阵等边界测试

### [TEST] 缺少模块间集成测试
- **描述**: ~22/30 跨模块场景未测试

### [TEST] 缺少压力/模糊测试
- **描述**: 无大规模数据压力测试

---

## 核查为误报的条目

### ❌ modular_poly q_minus_2 uint64 溢出 (`modular_poly.hpp:529-531`)
- **原始分类**: P1
- **核查结论**: 代码中不存在此模式，所有大指数运算使用 Integer/uint128

### ❌ Eratosthenes 筛法 p*2 溢出 (`builder.cpp:80,102,131`)
- **原始分类**: P1
- **核查结论**: p 最大 = rational_bound（cap 在 1e9），p*2 < UINT32_MAX

### ❌ class_group factor_ideal val=0 无限循环 (`class_group.hpp:383-393`)
- **原始分类**: P1
- **核查结论**: `val != 0` 守卫阻止了循环（但 exp=0 丢失贡献是独立问题）

### ❌ Schirokauer precompute_for_prime "无根=不可约" d≥4 (`schirokauer.hpp:369-385`)
- **原始分类**: P1
- **核查结论**: 已修复——代码已调用 `ModularPoly::is_irreducible()`（Rabin 测试）

### ❌ ThreadPool func 引用捕获 (`thread_pool.hpp:104,140`)
- **原始分类**: P1
- **核查结论**: parallel_for 通过 future.get() 等待完成，func 存活期间安全

### ❌ Hensel S[i].to_uint64() 截断 (`hensel_sqrt.hpp:115`)
- **原始分类**: P0
- **核查结论**: 初始步骤中 S 是 mod p（小素数），值安全

### ❌ Couveignes Gray Code __builtin_ctzll(0) (`couveignes.hpp`)
- **原始分类**: P0
- **核查结论**: Gray 码恰差 1 位，输入永远非零

### ❌ Gaussian 消元 pivot_cols.back() 空容器 (`gauss.hpp:100-101`)
- **原始分类**: P0
- **核查结论**: 三元运算符守卫

### ❌ Integer::powmod() 不验证负指数 (`integer.hpp`)
- **原始分类**: P1
- **核查结论**: GMP mpz_powm 正确处理负指数（计算逆元）

---

## 已完成 ✅（全部经 Session 8 核查确认）

### ✅ Integer(uint64_t) 构造函数 — `b0e79f9`
### ✅ Relation::ab() b=INT64_MIN UB — `4b4ec08`
### ✅ std::abs(INT64_MIN) UB（7 处） — `4b4ec08`, `3789872`
### ✅ modular_poly p=2 Tonelli-Shanks — `145201c`
### ✅ NumberField monic 假设 — `ec2aa32`
### ✅ polynomial_optimizer Newton divmod — `82bbec1`
### ✅ newton_root() 验证永远成功 — `82bbec1`
### ✅ Hensel/Couveignes 不可约性检查（Rabin 测试） — `7516710`
### ✅ trial_division divide_exact() int64 溢出 — `b0e79f9`
### ✅ ThreadPool pending_ 竞态 — `e6dd3f7`
### ✅ MatrixBuilderConfig schirokauer_primes {2,3}→{2} — `5791463`
### ✅ smooth_check large_prime_bound² uint64 溢出 — `783294a`
### ✅ cofactorizer 大素数 uint32→uint64 — `3b93104`
### ✅ Couveignes 无上限搜索循环 — `41213e1`
### ✅ polynomial_optimizer divmod 参数命名 — `82bbec1`
### ✅ matrix_builder 存储实际 primes — `5791463`
### ✅ FactorBaseParams large_prime_bound uint64 — `3b93104`
### ✅ smooth_check quick_cofactor_check lpb² — `783294a`
### ✅ Hensel Sqrt 预计算优化 — Session 3
