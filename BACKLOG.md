# BACKLOG — 待办备忘录

> 记录发现但当前不处理的问题。每个条目必须有：分类、来源、描述、发现日期。
> 已解决的条目移到末尾「已完成」区域，不要删除。

---

## P0 — 正确性关键（必须修复）

### [BUG] Integer 缺少 uint64_t 构造函数，导致大值静默溢出
- **发现日期**: 2026-02-20 (Session 2), 2026-03-08 审计确认
- **来源**: Core 模块审计
- **文件**: `include/gnfs/core/integer.hpp:16-20`
- **描述**: `Integer` 只有 `explicit Integer(int64_t)` 构造函数，无 `uint64_t` 版本。值在 [2^63, 2^64) 范围内会静默溢出为负数。代码中多处使用 `Integer(static_cast<unsigned long long>(p))` 这种不安全的转换
- **影响**: 任何 p > 2^63 的素数或参数会产生错误结果
- **优先级**: P0

### [BUG] Relation::ab() 中 b=INT64_MIN 时未定义行为
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Core 模块审计
- **文件**: `include/gnfs/core/relation.hpp:37`
- **描述**: `static_cast<uint64_t>(b > 0 ? b : -b)` 当 b=INT64_MIN 时，`-b` 溢出为 INT64_MIN（UB）
- **影响**: 罕见但对特定 b 值是致命的
- **建议**: 先检查 `b == INT64_MIN` 或用 `uint64_t(~b) + 1`

### [BUG] std::abs(INT64_MIN) 未定义行为（多处）
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sieve + Cofactor + Relation 模块审计
- **文件**: `lattice_sieve.hpp:404`, `cofactorizer.hpp:95`, `collector.hpp:269`
- **描述**: 多处使用 `std::abs(a)` 或 `std::gcd(std::abs(a), b)` 但未检查 a=INT64_MIN 的情况
- **影响**: 当 a=INT64_MIN 时产生 UB（通常返回 INT64_MIN 本身）
- **建议**: 统一封装 `safe_abs()` 函数，检查 INT64_MIN

### [BUG] Hensel Sqrt S[i].to_uint64() 截断：N > 2^64 时 Hensel 失败
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/hensel_sqrt.hpp:113-122`
- **描述**: Hensel 提升中 `uint64_t si = S[i].to_uint64()` 在 S[i] 系数大于 2^64 时静默截断。对 N > 65 位数的因式分解，系数可超过此范围
- **影响**: 65+ 位 N 的 Hensel 提升产生垃圾结果，因式分解失败
- **建议**: 改用 Integer 算术或在 to_uint64() 前检查 fits_uint64()

### [BUG] Couveignes Gray Code `__builtin_ctzll(0)` 未定义行为
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/couveignes.hpp:425-429`
- **描述**: `__builtin_ctzll(changed_bit)` 在 `changed_bit == 0` 时为未定义行为。如果 Gray Code 转换不翻转任何位，会崩溃
- **影响**: 潜在崩溃
- **建议**: 添加 `if (changed_bit == 0) continue;` 守卫

### [BUG] modular_poly p=2 时 Tonelli-Shanks 无限循环
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/modular_poly.hpp:353-362`
- **描述**: 查找非二次剩余的循环 z=2,3,... 在 F_2 中永远找不到非平方，导致无限循环
- **影响**: 任何使用 p=2 的 Tonelli-Shanks 调用会挂起
- **建议**: 对 p=2 做特殊处理（直接返回）

### [BUG] NumberField 假设 f 是 monic 但从不验证
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/number_field.hpp:416-451`
- **描述**: 多项式约化假设 f[d]=1（monic），但无任何检查。如果 f 非 monic，所有 Q[α] 中的算术都是错误的
- **影响**: 非 monic 多项式会产生完全错误的平方根
- **建议**: 添加构造时 assert `f[d] == 1`，或实现非 monic 支持

### [BUG] params.hpp special_q_max 的 uint32 溢出
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Core 模块审计
- **文件**: `include/gnfs/core/params.hpp:166`
- **描述**: `p.special_q_max = p.rational_bound * 2` 当 rational_bound > 2^31 时 uint32 溢出。对 100+ 位 N，rational_bound ≈ 1e9，翻倍后溢出
- **影响**: special_q_max 变为极小值，算法失效
- **建议**: 改用 uint64_t 或 `std::min(uint64_t(rb)*2, UINT32_MAX)`

### [BUG] trial_division.hpp uint8_t 指数溢出
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Cofactor 模块审计
- **文件**: `include/gnfs/cofactor/trial_division.hpp:62-64`
- **描述**: 试除循环 `++exp` 使用 uint8_t，如果某素数 p^256 整除 value，exp 溢出回零
- **影响**: 数据损坏：关系中该因子指数记为 0
- **建议**: 改用 uint16_t 或添加溢出检查

### [BUG] Gaussian 消元 pivot_cols.back() 空容器 UB
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `include/gnfs/linalg/gauss.hpp:100-105`
- **描述**: 检测 free columns 时调用 `pivot_cols.back()` 但未检查 `pivot_cols.empty()`
- **影响**: 空矩阵或全零矩阵时 UB
- **建议**: 添加 `if (pivot_cols.empty())` 守卫

### [BUG] SparseMatrix::test() const_cast 违反 const 契约
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `include/gnfs/linalg/sparse_matrix.hpp:62-63`
- **描述**: `const_cast<SparseRow*>(this)->ensure_sorted()` 在 const 方法中修改内部状态，技术上是 UB
- **建议**: 将 `sorted_` 和 `indices_` 标记为 `mutable`

### [BUG] ThreadPool pending_ 计数竞态条件
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Util 模块审计
- **文件**: `include/gnfs/util/thread_pool.hpp:188-189`
- **描述**: `--pending_` 和后续 `done_cv_.notify_all()` 在锁外执行。另一线程可能在递减和检查之间增加 pending_
- **影响**: `wait_all()` 可能过早返回或永不返回
- **建议**: 在互斥锁内递减和检查

### [BUG] RelationCollector::relations() 返回非安全引用
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Relation 模块审计
- **文件**: `include/gnfs/relation/collector.hpp:151-152`
- **描述**: 返回 `relations_` 的非 const 引用，调用者可在其他线程添加时修改，导致数据竞争
- **影响**: 多线程场景下 UB
- **建议**: 返回 const 引用或 `std::span<const Relation>`

---

## P1 — 高优先级（影响正确性或大数支持）

### [BUG] Split Schirokauer: f mod 2 可约时映射计算错误
- **发现日期**: 2026-02-20 (Session 2)
- **来源**: Schirokauer maps 测试中发现
- **文件**: `include/gnfs/linalg/schirokauer.hpp`
- **描述**: 当多项式 f mod 2 可约时，需要先做 valuation stripping 再计算 Schirokauer map。当前部分实现存在但有 bug（非单位元素处理不正确）。审计还发现 split case root lifting (line 251) 使用了错误的根值
- **影响**: 部分 N 值（f mod 2 可约的）无法正确因式分解
- **当前规避**: 选择 f mod 2 不可约的测试 N
- **优先级**: P1

### [BUG] Couveignes 符号归一化不一致导致 CRT 重建错误
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/couveignes.hpp:510-524`
- **描述**: `needs_negate = (eval_at_1 > p/2)` 对不同素数使用不同阈值 p/2。不同素数的符号模式不一致，CRT 重建产生错误结果。此外，有理平方根指数 (N+1)/4 未检查 N ≡ 3 mod 4 前提
- **影响**: ~50% 的有效符号模式被错误丢弃，大幅降低 Couveignes 成功率

### [BUG] algebraic_sqrt compute_heuristic() 数学不正确
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/algebraic_sqrt.hpp:173-195`
- **描述**: 使用 `elem^((n+1)/2)` 计算平方根，但这在 Q[α]/f(α) 中不成立。此 fallback 方法对任何非平凡情况都会失败
- **影响**: Hensel 失败时的 fallback 路径不可用

### [BUG] matrix_builder f mod 2 检查在大系数时 uint64 截断
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:174-184`
- **描述**: 检查 f mod 2 是否不可约时，先调用 `c.to_uint64()` 再取 mod 2。但大系数在 to_uint64() 时已截断。应先做 `c %= 2` 再转换
- **影响**: 错误判断 f mod 2 的可约性，导致 Schirokauer 或 QC 列数错误

### [BUG] matrix_builder 多项式度 > 8 时数组越界
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:416`
- **描述**: `FastPoly::MAX_DEGREE = 8`，但 `select_qc_primes()` 未验证 `ctx.degree() <= 8`。度 > 8 时数组越界
- **影响**: 高阶 GNFS（度 9+）崩溃
- **建议**: 添加 degree 边界检查

### [BUG] matrix_builder QC 系数 int64 截断
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:387-388`
- **描述**: `ctx.coeff(i).to_int64()` 对大 N 的多项式系数可能超出 int64 范围，静默截断导致 QC 素数选择使用错误多项式
- **影响**: QC 列不正确，矩阵零空间求解失败

### [BUG] Schirokauer FastPoly 系数潜在溢出
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `include/gnfs/linalg/schirokauer.hpp:73, 87-91`
- **描述**: `reduce_inplace()` 中 `a.coeffs[idx]` 可能 > m（违反 GF(m) 语义）。当 t > a.coeffs[idx] 时公式 `m - (t - a.coeffs[idx])` 会给出错误结果
- **影响**: Schirokauer map 值不正确

### [BUG] lattice_sieve 模运算中间值 int64 溢出
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sieve 模块审计
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:268-269`
- **描述**: `u = (basis.e0 - f0*m) % p` 中 `f0*m` 可溢出 int64。对大格基值产生错误结果
- **建议**: 使用 `__int128_t` 或先做模约化再乘

### [BUG] lattice_basis 行列式计算 int64 溢出
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sieve 模块审计
- **文件**: `include/gnfs/sieve/lattice_basis.hpp:52`
- **描述**: `e0 * f1 - e1 * f0` 对大格基值溢出 int64，无边界检查
- **影响**: 大 special-Q 时格行列式错误

### [BUG] cofactorizer 大素数存储 uint32 截断
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Cofactor 模块审计
- **文件**: `include/gnfs/cofactor/cofactorizer.hpp:160, 170`
- **描述**: 余因子 > UINT32_MAX（50+ 位素数）时，PrimePower 中存储为 0。50+ 位 N 需要更大的大素数
- **建议**: PrimePower 改用 uint64_t 或 Integer

### [BUG] ECM 固定随机种子导致重复曲线
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Cofactor 模块审计
- **文件**: `include/gnfs/cofactor/ecm.hpp:49`
- **描述**: `std::mt19937_64 rng(42)` 硬编码种子。每次 ECM 运行产生相同 sigma 序列，多曲线策略失效，显著降低找到因子的概率
- **建议**: 使用非确定性种子或曲线索引 + 哈希

### [BUG] modular_poly q_minus_2 uint64 溢出
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/modular_poly.hpp:529-531`
- **描述**: 计算 `q_minus_2 = mod^d` 时 uint64 溢出（mod>2, d>20）
- **影响**: Tonelli-Shanks 平方根失败

### [BUG] class_group 判别式计算仅对 d=3 正确
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/class_group.hpp:165-209`
- **描述**: 判别式计算仅对 d=3 正确；d>3 时结果完全错误
- **影响**: 类群计算对高阶多项式不正确

### [BUG] filter.hpp 合并关系硬编码 1000 上限
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Relation 模块审计
- **文件**: `include/gnfs/relation/filter.hpp:262`
- **描述**: `merged.size() < 1000` 硬编码上限，大因子基时丢弃有效合并
- **建议**: 改为可配置参数

### [BUG] Logger 递归日志死锁
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Util 模块审计
- **文件**: `include/gnfs/util/logger.hpp:140-141`
- **描述**: 如果日志回调或异常处理器中再次调用 log()，会导致 `lock_guard` 死锁
- **建议**: 改用 `recursive_mutex` 或线程局部嵌套计数器

---

## P1-OPT — 高优先级性能优化

### [OPT] Block Lanczos 是 25-digit 的主要瓶颈（需并行化）
- **发现日期**: 2026-02-22 (Session 3), 2026-03-08 审计补充
- **来源**: 25-digit 性能分析（193s = 81.6%）
- **文件**: `src/linalg/block_lanczos.cpp`
- **描述**: SpMV 操作（lines 181-201）是顺序执行的 O(w)。审计还发现：(1) AND 应为 OR 的阈值判断 (line 284) (2) 硬编码种子 42 (line 310) (3) M^T * V 每次迭代重复计算 (line 343)
- **影响**: 30-digit+ 因式分解的主要瓶颈
- **建议**: OpenMP 并行 SpMV，预计 4-8× 加速

### [OPT] ECM Stage 2 BSGS 优化
- **发现日期**: 2026-02-20 (Session 2)
- **来源**: 性能分析 + 2026-03-08 审计确认
- **文件**: `include/gnfs/cofactor/ecm.hpp:410-430`
- **描述**: 当前 Stage 2 朴素实现 O(π(B2))。审计还发现每次 ECM 调用都重新做素数筛选（Eratosthenes），应缓存素数列表
- **影响**: 60 位余因子的 Stage 2 比 BSGS 慢 5-10×

### [OPT] lattice_basis 浮点高斯约化不够精确
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sieve 模块审计
- **文件**: `include/gnfs/sieve/lattice_basis.hpp:83-109`
- **描述**: 使用 double 精度做高斯约化，当格基系数 |e0|, |f0| ~ 10^18 时绝对误差 ~ 10^3，不可接受。应使用纯整数 GCD 或 LLL 约化
- **影响**: 大 special-Q 时格基质量差，筛选效率降低

### [OPT] Gauss 消元 history 矩阵 O(n^2) 空间
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `include/gnfs/linalg/gauss.hpp`
- **描述**: 每行一个 num_rows 位的 BitVector。500K 行 = 31GB，不可用于大矩阵
- **影响**: 阻止 Gaussian fallback 用于大矩阵
- **建议**: 仅存储参与行的索引，或使用压缩表示

---

## P2 — 中优先级（50+ 位数支持和架构改进）

### [FEAT] Bucket Sieve 架构
- **发现日期**: 2026-02-20 (Session 2)
- **来源**: 大因子基性能需求
- **描述**: 当前 sieve 对大因子基不够 cache-friendly。需要重新设计为 bucket sieve 架构
- **影响**: 大 N（50-digit+）的 sieve 阶段性能瓶颈

### [FEAT] NEON SIMD 加速
- **发现日期**: 2026-02-20 (Session 2)
- **来源**: 性能优化规划
- **描述**: 利用 ARM NEON 指令加速关键路径：`vqsubq_u16` 用于 sieve，`vminvq_u16` 用于候选扫描，`veorq_u64` 用于 GF(2) XOR
- **影响**: 预计 sieve 和 linalg 各有 2-4× 提升

### [OPT] Kleinjung 多项式选择质量提升
- **发现日期**: 2026-02-20 (Session 2), 2026-03-08 审计补充
- **来源**: Polynomial 模块审计
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp`
- **描述**: 审计新发现：(1) search_radius=100 硬编码，大 N 需 >1000 (line 238) (2) 系数边界 O(m) 太松，应为 O(m^{1/d}) (line 275) (3) 不检查 f 在 Q 上不可约性。原有问题：缺少 lattice-based search 和 coefficient rotation

### [FEAT] Factor Base 支持 ramified/projective 素数
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: FactorBase 模块审计
- **文件**: `src/factor_base/builder.cpp`
- **描述**: AlgebraicPrime 有 degree 字段和 PROJECTIVE_ROOT 常量，但 builder 始终设 degree=1，从不使用 projective root。对 p | disc(f) 的分歧素数和无穷远素数缺失
- **影响**: 大约丢失 ~1% 代数侧素数，大 N 时关系收集效率降低

### [FEAT] Out-of-core Relations 支持
- **发现日期**: 2026-02-20 (Session 2), 2026-03-08 审计强化为 P2
- **来源**: Relation 模块审计
- **文件**: `include/gnfs/relation/collector.hpp`
- **描述**: 所有关系存储在内存中。50+ 位 N 需要 10-100M 关系（数 GB）。审计还发现：(1) 无压缩 (2) 去重哈希表无界增长 (3) 无批量 I/O
- **影响**: 50+ 位 N 时 OOM

### [FEAT] Block Lanczos Out-of-core 矩阵支持
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **描述**: 矩阵必须完全在 RAM 中。50+ 位 GNFS 的 500K 关系矩阵可能需要数十 GB
- **建议**: 流式 SpMV + 磁盘矩阵存储

### [DEBT] params.hpp 对 100+ 位 N 参数不足
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Core 模块审计
- **文件**: `include/gnfs/core/params.hpp`
- **描述**: (1) degree 上限硬编码为 6，100+ 位应为 7-8 (2) rational_bound 上限 1e9，150+ 位应为 1e10+ (3) log_scale 最大 16，应随因子基增长 (4) 小 N (n_bits<2) 时 ln_ln_n 产生 NaN
- **建议**: 动态度缩放，移除硬编码上限

### [FEAT] Relation Filter 完成 clique-based 合并
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Relation 模块审计
- **文件**: `include/gnfs/relation/filter.hpp:286-287`
- **描述**: `PartialRelationMerger::merge()` 有框架但从未真正实现因子指数的异或/合并。50+ 位 N 依赖部分关系的图合并
- **建议**: 实现完整的基于图的 clique 解析

### [FEAT] ThreadPool Work-Stealing
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Util 模块审计
- **文件**: `include/gnfs/util/thread_pool.hpp`
- **描述**: 当前固定分块分配。筛选的 special-Q 开销不均匀，导致负载不平衡
- **建议**: 实现工作窃取队列

### [DEBT] Relation 序列化格式缺陷
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Core 模块审计
- **文件**: `include/gnfs/core/relation.hpp:73-144`
- **描述**: (1) 无版本字段，格式不可升级 (2) 无校验和/CRC (3) 依赖字节序 (4) 反序列化不检查流状态
- **影响**: 截断文件产生静默损坏的关系

### [BUG] smooth_check 浮点精度损失
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Cofactor 模块审计
- **文件**: `include/gnfs/cofactor/smooth_check.hpp:107-108`
- **描述**: `std::pow(double(n), 1.0/k)` 对大 n 有 ~10^-15 相对误差，round() 可能取错值。is_perfect_square 中 `candidate * candidate == n` 可溢出
- **建议**: 使用 GMP 的 mpz_root / mpz_perfect_square_p

### [OPT] Murphy E-score 低估 20-40%
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Polynomial 模块审计
- **文件**: `include/gnfs/polynomial/murphy_evaluator.hpp`
- **描述**: (1) 只用 B 以下素数，忽略 B-10B 余因子贡献 (2) Dickman rho 近似在 u>100 时误差 >5% (3) 采样边界对大 skewness 可能为负
- **影响**: 多项式排名不准确

### [DEBT] PrimePowerHash 忽略指数字段
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Core 模块审计
- **文件**: `include/gnfs/core/types.hpp:87-92`
- **描述**: 哈希仅组合 (p, r) 忽略 e，不同指数的 PrimePower 哈希相同
- **影响**: 如果用于 unordered_set 去重会出错

---

## P3 — 低优先级（代码质量和长期改进）

### [DEBT] Integer(uint64_t) 构造函数缺失（重复项，主修在 P0）
- **发现日期**: 2026-02-20 (Session 2)
- **描述**: 见 P0 同名条目。此处保留历史记录

### [DEBT] 根目录遗留文件清理
- **发现日期**: 2026-03-08 (Session 4)
- **描述**: 根目录有 ~70 个遗留文件（.sh 脚本、过时 .md、扁平命名文件）

### [DEBT] SmallVector 缺少边界检查
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Util 模块审计
- **文件**: `include/gnfs/util/small_vector.hpp:96-103`
- **描述**: `operator[]` 和 `back()` 无边界检查，空容器 back() 为 UB。ensure_capacity() 无溢出检查
- **建议**: Debug 模式添加 assert

### [DEBT] FactorBase 缺少序列化
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: FactorBase 模块审计
- **文件**: `include/gnfs/factor_base/factor_base.hpp:137-141`
- **描述**: 标记为 TODO。大 FB (100K+) 重建耗时，应支持保存/加载

### [DEBT] Block Lanczos 阈值判断逻辑错误 (AND vs OR)
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `src/linalg/block_lanczos.cpp:284`
- **描述**: `if (rows < 10000 && cols < 10000)` 应为 OR。10001×100 的矩阵不应该用 Block Lanczos
- **建议**: 改为 `||`

### [DEBT] Schirokauer 文档注释与代码不一致
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `include/gnfs/linalg/schirokauer.hpp:138`
- **描述**: 注释写的指数公式 ℓ^(k-1)·(ℓ-1) 与代码实际使用的 ℓ^d-1 不一致。代码是对的，注释是错的

### [DEBT] polynomial_context coeff() 返回可变静态引用
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Core 模块审计
- **文件**: `include/gnfs/core/polynomial_context.hpp:76-80`
- **描述**: 越界访问返回同一个静态 zero Integer 引用，多次调用返回同一引用可能产生别名 bug

### [DEBT] SpecialQ from_indices 忽略 end_index 参数
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sieve 模块审计
- **文件**: `include/gnfs/sieve/special_q.hpp:36`
- **描述**: `from_indices(uint32_t start, uint32_t /* end_index */)` 中 end_index 被注释掉但仍在签名中

### [DEBT] number_field evaluate 无溢出保护
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/number_field.hpp:328-343`
- **描述**: `m_power *= m` 循环对大 m 和 d>5 溢出，无边界检查

---

## TEST — 测试覆盖率缺口

### [TEST] 7 个模块无专属单元测试
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: 测试覆盖率审计
- **缺失测试**: `base_m`, `params`, `polynomial_context`, `polynomial_optimizer`, `int_polynomial`, `filter`, `class_group`
- **建议**: 至少为 params 和 polynomial_context 添加单元测试

### [TEST] 0 个已修复 bug 的回归测试
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: 测试覆盖率审计
- **描述**: Session 1-3 修复的 10 个关键 bug 均无专属回归测试。包括：N-divisible 关系过滤、Schirokauer ℓ=2 only、指数公式、QC degree>3、evaluate_mod 溢出、extract_factors、Hensel p^d 溢出等
- **影响**: 任何重构都可能无意间回退这些修复
- **建议**: 优先添加 P0 级回归测试

### [TEST] 边界/极端情况覆盖率约 5%
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: 测试覆盖率审计
- **描述**: 缺失的关键边界测试：Integer 溢出、负数 mod、空矩阵、度 0 多项式、FB 边界素数、Single row 矩阵、Hensel p^d 溢出等

### [TEST] 缺少模块间集成测试
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: 测试覆盖率审计
- **描述**: 约 22/30 跨模块场景未测试，包括：FB→Sieve（空FB）、Sieve→Cofactorizer（1LP→2LP 升级）、Filter→MatrixBuilder（singleton 链）、BL→Sqrt（多依赖→多次 sqrt 尝试）、Sqrt→GCD（X=Y 退化情况）

### [TEST] 缺少压力/模糊测试
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: 测试覆盖率审计
- **描述**: 无 1000+ 关系过滤、100K+ FB 构建、1M 筛选位置、50K×50K 矩阵构建等压力测试

---

## 已完成

### [OPT] ~~Hensel Sqrt 预计算优化~~ ✅
- **发现日期**: 2026-02-20 (Session 2)
- **解决日期**: 2026-02-22 (Session 3)
- **结果**: Hensel 15.5× 加速，25-digit 总耗时 603s → 236.5s
