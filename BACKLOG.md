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

### [BUG] Block Lanczos partial_inverse() 未将非主元行清零
- **发现日期**: 2026-03-09 (Session 6) | Montgomery BL 上下文中实际工作正确
- **文件**: `include/gnfs/linalg/block_lanczos.hpp:119-157`
- **描述**: 非主元行有垃圾值，但 D*A 乘积中自动消除

### [BUG] Block Lanczos add_identity() 添加完整单位矩阵
- **发现日期**: 2026-03-09 (Session 6) | 影响仅在 A_i 秩亏时
- **文件**: `src/linalg/block_lanczos.cpp:356-361`
- **描述**: 应为 mask 子空间的单位矩阵

### [BUG] SparseMatrix::test() const_cast 违反 const 契约
- **发现日期**: 2026-03-08 (Session 5) | 单线程 OK，多线程见 P2 条目
- **文件**: `include/gnfs/linalg/sparse_matrix.hpp:62-63`
- **描述**: const_cast 在 const 方法中修改内部状态

### [BUG] Relation::b 是 int64_t 但应为 uint64_t
- **发现日期**: 2026-03-08 (Session 6) | 风格问题，b 实际不超过 int64 范围
- **文件**: `include/gnfs/core/relation.hpp:17`

### [BUG] RelationCollector::relations() 返回非 const 引用
- **发现日期**: 2026-03-08 (Session 5) | 实际有 const 版本，API 设计问题
- **文件**: `include/gnfs/relation/collector.hpp:151-152`

### [BUG] params.hpp special_q_max 的 uint32 溢出
- **发现日期**: 2026-03-08 (Session 5) | 🟢 当前安全（cap 在 1e9）
- **文件**: `include/gnfs/core/params.hpp:166`

### [BUG] class_group factor_ideal/factor_principal_ideal int64 乘法溢出
- **发现日期**: 2026-03-08 (Session 5) | b*r ~10^18 接近但不超过 INT64_MAX
- **文件**: `include/gnfs/sqrt/class_group.hpp:383,418`

### [BUG] base_m_expansion 非零余数处理
- **发现日期**: 2026-03-08 (Session 5) | 产生劣质多项式，不影响正确性
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp:476-478`

### [BUG] build_row() 符号列基于 a<0 而非 (a-bm)<0
- **发现日期**: 2026-03-08 (Session 6) | build_with_qc 修正了符号，管线不走 build_row
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:527-530`

### [BUG] SparseRow::set() 非幂等——重复 set 等价于 clear
- **发现日期**: 2026-03-08 (Session 6) | GF(2) toggle 语义正确，API 命名问题
- **文件**: `include/gnfs/linalg/sparse_matrix.hpp`

### [BUG] next_prime() uint64 溢出
- **发现日期**: 2026-03-08 (Session 6) | 理论问题，prime_start 默认 1000
- **文件**: couveignes.hpp:619-628 + hensel_sqrt.hpp:550-562

### [BUG] Logger 递归日志死锁
- **发现日期**: 2026-03-08 (Session 5) | 无实际重入路径
- **文件**: `include/gnfs/util/logger.hpp:140-141`

### [BUG] Logger::level() 读取 level_ 无锁
- **发现日期**: 2026-03-08 (Session 6) | uint8_t 读写在所有架构上原子
- **文件**: `include/gnfs/util/logger.hpp:63`

### [BUG] SchirokaurMap 存储 const PolynomialContext&
- **发现日期**: 2026-03-09 (Session 7) | 设计气味，实际 ctx 始终 outlive 使用者
- **文件**: `include/gnfs/linalg/schirokauer.hpp:342`

### [BUG] LatticeSieve 存储 const 引用
- **发现日期**: 2026-03-09 (Session 7) | 实际 ctx/fb 始终 outlive sieve
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:186-187`

### [BUG] Relation 反序列化无输入验证
- **发现日期**: 2026-03-08 (Session 6) | 仅内部使用，无外部攻击面
- **文件**: `include/gnfs/core/relation.hpp:104-148`

### [BUG] MurphyEvaluator n.to_double() 对 N > 10^308 返回 infinity
- **发现日期**: 2026-03-08 (Session 6) | 远超实现能力范围
- **文件**: `include/gnfs/polynomial/murphy_evaluator.hpp:331`

### [BUG] MurphyEvaluator alpha 跳过大首项系数
- **发现日期**: 2026-03-08 (Session 6) | 实际首项系数总是 fit uint64
- **文件**: `include/gnfs/polynomial/murphy_evaluator.hpp:143-147`

### [BUG] IntPolynomial add_mod 大 p 溢出
- **发现日期**: 2026-03-08 (Session 6) | 所有素数 < 2^32
- **文件**: `include/gnfs/polynomial/int_polynomial.hpp:362`

### [BUG] IntPolynomial mutable operator[] 无上限检查
- **发现日期**: 2026-03-09 (Session 7) | 设计选择，实际调用点安全
- **文件**: `include/gnfs/polynomial/int_polynomial.hpp:75-80`

### [BUG] polynomial_optimizer generate_smooth_numbers 不去重
- **发现日期**: 2026-03-08 (Session 6) | 重复无害，仅微量浪费
- **文件**: `include/gnfs/polynomial/polynomial_optimizer.hpp:323-355`

### [BUG] Integer bit_length(0) 返回 1
- **发现日期**: 2026-03-09 (Session 6) | GMP 规范行为
- **文件**: `src/core/integer.cpp:93-94`

### [BUG] Integer::sqrt() 对负数输入无检查
- **发现日期**: 2026-03-09 (Session 6) | 所有调用点传正值
- **文件**: `include/gnfs/core/integer.hpp`

### [BUG] Integer::powmod() 负指数
- **发现日期**: 2026-03-09 (Session 6) | GMP 实际处理负指数（计算逆）
- **文件**: `include/gnfs/core/integer.hpp`

### [BUG] matrix_builder exponent 累积用 uint8_t
- **发现日期**: 2026-03-08 (Session 6) | 256≡0 mod 2，溢出不影响 GF(2) 奇偶性
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:534,548`

### [BUG] SieveParams::combined_threshold() uint8_t 溢出
- **发现日期**: 2026-03-08 (Session 6) | 当前参数最大 160，不触发
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:37`

### [BUG] FactorBase::add_rational() 无去重
- **发现日期**: 2026-03-08 (Session 6) | builder 使用筛法自然不重复
- **文件**: `include/gnfs/factor_base/factor_base.hpp:94-98`

### [BUG] sieve_parallel() 使用不必要的 mutex
- **发现日期**: 2026-03-09 (Session 7) | 性能浪费但不影响正确性
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:153,168-169`

### [BUG] class_group factor_ideal val=0 时 exp=0
- **发现日期**: 2026-03-09 (Session 6) | val!=0 守卫阻止循环，但 a=b*r 时赋值丢失
- **文件**: `include/gnfs/sqrt/class_group.hpp:383-393`

### [OPT] Murphy E-score 低估 20-40%
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/polynomial/murphy_evaluator.hpp`

### [DEBT] 全局性 uint64_t b → int64_t 截断（13 处）
- **发现日期**: 2026-03-08 (Session 5) | b 值始终远小于 INT64_MAX
- **描述**: 理论上 b > INT64_MAX 时截断，实际不会发生

### [DEBT] Schirokauer 文档注释与代码不一致
- **文件**: `include/gnfs/linalg/schirokauer.hpp:138`

### [DEBT] SpecialQ from_indices 忽略 end_index 参数
- **文件**: `include/gnfs/sieve/special_q.hpp:36`

### [DEBT] number_field evaluate 无溢出保护
- **文件**: `include/gnfs/sqrt/number_field.hpp:328-343`

### [DEBT] PrimePowerHash 忽略指数字段
- **文件**: `include/gnfs/core/types.hpp:87-92`

### [DEBT] SmallVector 缺少边界检查
- **文件**: `include/gnfs/util/small_vector.hpp:96-103`

### [DEBT] FactorBase 缺少序列化
- **文件**: `include/gnfs/factor_base/factor_base.hpp:137-141`

### [DEBT] Block Lanczos 阈值 AND 应为 OR
- **文件**: `src/linalg/block_lanczos.cpp:284`

### [DEBT] Relation 序列化格式缺陷（无版本/校验和）
- **文件**: `include/gnfs/core/relation.hpp:73-144`

### [DEBT] -Wconversion 清理（~60 处 sign-conversion）
- **发现日期**: 2026-03-10 (Session 18)
- **描述**: `-Wall -Wextra -Wpedantic` 已清零 warning。`-Wconversion` 下有 ~60 处 sign-conversion（int→size_t 数组下标、int64→uint64 等），大多 cosmetic。最可疑的是 `modular_poly.hpp` 中 ~20 处 int degree 循环变量做 vector 下标、`Relation::b` int64→uint64 传参。无安全隐患但降低代码严谨度

### [DEBT] 根目录遗留文件清理

---

## TEST — 测试覆盖率缺口

（当前无未解决条目。历史记录见 RESOLVED.md）

