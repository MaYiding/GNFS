# BACKLOG — 待办备忘录

> 只记录**未完成**的问题。已完成和误报条目见 `RESOLVED.md`。
> 严重程度排序：P0 > P1 > P1-OPT > P2 > P3 > TEST。从文件开头往下读即为优先级。

---

## P1 — 高优先级（影响正确性或大数支持）

### [BUG] L5/25-digit 测试因关系不足而失败（base-m irreducibility fix 回归）
- **发现日期**: 2026-03-10 (Session 22)
- **文件**: 管线整体（多项式选择→筛选→关系收集）
- **描述**: base-m irreducibility fix (commit `d38ab90`) 改变了多项式选择策略，导致 L5 (N=1253371692427905599) 和 25-digit (N=1669994516749619561652133) 筛选产生的关系数不足矩阵列数。L5: 9403 rels < 11690 cols; 25-digit: 20928 rels < 38136 cols。在 fix 之前（Session 3）两者都能通过
- **建议**: (1) 增加筛选区域或 special-Q 范围；(2) 调整 params.hpp 对该规模 N 的参数；(3) 或调查新多项式质量是否更差

### [BUG] modular_poly sub()/mod_inverse() 对 p > INT64_MAX 溢出
- **发现日期**: 2026-03-08 (Session 5) | 实际不可达（所有素数 < 2^32）
- **文件**: `include/gnfs/sqrt/modular_poly.hpp:90,420`
- **描述**: uint64 系数 cast 到 int64 相减

---

## P1-OPT — 高优先级性能优化

### [OPT] ECM Stage 2 BSGS 优化
- **发现日期**: 2026-02-20 (Session 2)
- **文件**: `include/gnfs/cofactor/ecm.hpp:410-430`
- **描述**: 朴素 O(π(B2))，应优化为 BSGS O(√(B2/B1))

### [OPT] lattice_basis 浮点高斯约化不够精确
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/sieve/lattice_basis.hpp:83-109`
- **描述**: double 精度在 |e0|, |f0| ~ 10^18 时误差 ~ 10^3

---

## P2 — 中优先级（大数支持和架构改进）

### [BUG] MurphyEvaluator rng_ 数据竞争——Kleinjung 多线程并行使用
- **发现日期**: 2026-03-08 (Session 6)
- **文件**: `include/gnfs/polynomial/murphy_evaluator.hpp:337` + `include/gnfs/polynomial/kleinjung_selector.hpp:133,167`
- **描述**: 多线程共享 rng_ 产生数据竞争
- **建议**: 使用 thread_local rng

### [BUG] Cofactorizer::stats_ 无 mutex 保护
- **发现日期**: 2026-03-08 (Session 6)
- **文件**: `include/gnfs/cofactor/cofactorizer.hpp:137,140,143,172`
- **描述**: verify() 多处修改 stats_ 无同步

### [BUG] ECM Stage 2 链式乘法增加因子丢失概率
- **发现日期**: 2026-03-09 (Session 6)
- **文件**: `include/gnfs/cofactor/ecm.hpp:410-413`
- **描述**: chaining 使 Z 坐标在两侧都变为 0 → gcd=N → 因子丢失

### [BUG] ECM 固定随机种子导致重复曲线
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/cofactor/ecm.hpp:49`
- **描述**: `std::mt19937_64 rng(42)` 硬编码种子，限制 ECM 效果

### [BUG] class_group SNF 不是真正的 Smith Normal Form
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/sqrt/class_group.hpp:450-517`
- **描述**: 只做了 Gaussian 消元，类数用 `1u << generators.size()` 近似

### [BUG] class_group 判别式计算仅对 d=3 depressed cubic 正确
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/sqrt/class_group.hpp:165-209`
- **描述**: d>3 返回启发式值；d=3 公式忽略 x² 系数

### [BUG] class_group 判别式公式对非 depressed 三次多项式错误
- **发现日期**: 2026-03-08 (Session 6)
- **文件**: `include/gnfs/sqrt/class_group.hpp:168-188`
- **描述**: Δ = -4a³ - 27b² 仅对 x³ + ax + b 正确，忽略 coeff(2)

### [BUG] estimate_initial_log NaN/Inf → uint16_t 强制转换是 UB
- **发现日期**: 2026-03-09 (Session 7)
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:221-231`
- **描述**: `log2(0) = -Inf`, `static_cast<uint16_t>(-Inf)` 是 UB
- **建议**: 添加 `if (!std::isfinite(combined) || combined < 0) return 0;`

### [BUG] estimate_initial_log typical_i/typical_j 不一致
- **发现日期**: 2026-03-09 (Session 6)
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:213-214`
- **描述**: typical_i = 半宽度，typical_j = 中点，不一致导致初值偏高

### [BUG] params.hpp special_q_min = rational_bound/5 落入因子基范围
- **发现日期**: 2026-03-09 (Session 6)
- **文件**: `include/gnfs/core/params.hpp:165`
- **描述**: 80% special-Q 在 FB 内部，筛选效率低
- **建议**: 改为 `special_q_min = algebraic_bound + 1`

### [BUG] Kleinjung base_m_expansion 系数不平衡
- **发现日期**: 2026-03-08 (Session 6)
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp:466-478`
- **描述**: 截断除法产生 [0,m) 系数，应平衡到 [-m/2,m/2]

### [BUG] SmallVector move constructor 不销毁源对象的 inline 元素
- **发现日期**: 2026-03-08 (Session 6) | 仅影响非平凡析构类型，当前主要用于 POD
- **文件**: `include/gnfs/util/small_vector.hpp:43-54,67-79`
- **描述**: move 后源元素析构函数未调用

### [BUG] RelationCollector::merge() 未锁定 other 的 mutex
- **发现日期**: 2026-03-08 (Session 6)
- **文件**: `include/gnfs/relation/collector.hpp:227`
- **描述**: 读取 other.relations_ 不加锁

### [BUG] Pollard rho 只使用单一多项式 x²+1
- **发现日期**: 2026-03-08 (Session 6)
- **文件**: `include/gnfs/cofactor/smooth_check.hpp:148`
- **描述**: 固定 c=1，某些 n 值永远找不到因子

### [BUG] lattice_sieve 模运算中间值 int64 溢出
- **发现日期**: 2026-03-08 (Session 5) | 大 q 时可触发但当前测试规模安全
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:268-269`
- **描述**: f0*m_mod_p 可溢出 int64

### [BUG] SieveRegion default_sieve_region 大 skewness 时 int32 溢出
- **发现日期**: 2026-03-08 (Session 6) | skewness > 1e10 时触发
- **文件**: `include/gnfs/sieve/lattice_basis.hpp:175-178`
- **描述**: base_size * factor 溢出 int32

### [BUG] Kleinjung is_valid_polynomial() 浮点验证无意义
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp:505-512`
- **描述**: double 精度对大 N 无效
- **建议**: 用 Integer 精确验证 `f(m) == N`

### [BUG] 代数因子基射影根在筛选中产生算术垃圾
- **发现日期**: 2026-03-08 (Session 6) | 射影根较少，影响有限
- **文件**: `include/gnfs/factor_base/factor_base.hpp:78` + `include/gnfs/sieve/lattice_sieve.hpp:342`
- **描述**: PROJECTIVE_ROOT = UINT32_MAX 被当作普通根处理

### [BUG] IntPolynomial::roots_cantor_zassenhaus 实际是 O(p) 暴力搜索
- **发现日期**: 2026-03-08 (Session 6)
- **文件**: `include/gnfs/polynomial/int_polynomial.hpp:384-405`
- **描述**: 名叫 CZ 但实现是暴力枚举

### [BUG] Integer 除零行为不一致
- **发现日期**: 2026-03-09 (Session 6)
- **文件**: `src/core/integer.cpp`
- **描述**: Integer 除零 → GMP abort（不可捕获），int64_t 除零 → domain_error

### [BUG] Hensel 提升无精度充分性验证
- **发现日期**: 2026-03-08 (Session 6) | 200 位余量通常足够
- **文件**: `include/gnfs/sqrt/hensel_sqrt.hpp:226-240`
- **描述**: centering 后无验证，但 extra_precision=200 提供余量

### [BUG] FactorBaseBuilder 实例 build() 返回空 FactorBase
- **发现日期**: 2026-03-09 (Session 7)
- **文件**: `src/factor_base/builder.cpp:62-69`
- **描述**: 实例方法直接返回空结果，不抛异常

### [BUG] matrix_builder FB 索引无越界检查
- **发现日期**: 2026-03-08 (Session 6) | 防御性编程，上游应产生正确索引
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:540,553`
- **描述**: FB 索引无上限检查

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

### [FEAT] Factor Base 支持 ramified/projective 素数
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `src/factor_base/builder.cpp`
- **描述**: builder 始终设 degree=1，不使用 projective root

### [FEAT] Out-of-core Relations 支持
- **发现日期**: 2026-02-20 (Session 2)
- **文件**: `include/gnfs/relation/collector.hpp`
- **描述**: 50+ 位 N 需要 10-100M 关系（数 GB）

### [FEAT] Block Lanczos Out-of-core 矩阵支持
- **发现日期**: 2026-03-08 (Session 5)
- **描述**: 矩阵必须完全在 RAM 中

### [DEBT] params.hpp 对 100+ 位 N 参数不足
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/core/params.hpp`
- **描述**: degree 上限 6、rational_bound 上限 1e9

### [FEAT] Relation Filter 完成 clique-based 合并
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/relation/filter.hpp:286-287`
- **描述**: merge 函数是 stub，从不实际合并

### [FEAT] ThreadPool Work-Stealing
- **发现日期**: 2026-03-08 (Session 5)
- **描述**: 筛选 special-Q 开销不均匀

---

## P3 — 低优先级（代码质量和长期改进）

### [BUG] 4 处 static Integer zero 返回引用——别名 + 线程理论风险
- **发现日期**: 2026-03-09 (Session 7)
- **文件**: number_field.hpp:65,195 / polynomial_context.hpp:77 / int_polynomial.hpp:69
- **描述**: 越界访问返回 static Integer zero 的 const&，别名陷阱

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

### [BUG] trial_division.hpp uint8_t 指数溢出
- **发现日期**: 2026-03-08 (Session 5) | p^256 整除实际不可能
- **文件**: `include/gnfs/cofactor/trial_division.hpp:62-64`
- **描述**: uint8_t exp 在 256 次自增后溢出

### [BUG] Relation::b 是 int64_t 但应为 uint64_t
- **发现日期**: 2026-03-08 (Session 6) | 风格问题，b 实际不超过 int64 范围
- **文件**: `include/gnfs/core/relation.hpp:17`

### [BUG] RelationCollector::relations() 返回非 const 引用
- **发现日期**: 2026-03-08 (Session 5) | 实际有 const 版本，API 设计问题
- **文件**: `include/gnfs/relation/collector.hpp:151-152`

### [BUG] params.hpp special_q_max 的 uint32 溢出
- **发现日期**: 2026-03-08 (Session 5) | 🟢 当前安全（cap 在 1e9）
- **文件**: `include/gnfs/core/params.hpp:166`

### [BUG] rational_sqrt 负号检测到但未应用
- **发现日期**: 2026-03-08 (Session 6) | extract_factors 同时检查 X±Y
- **文件**: `include/gnfs/sqrt/rational_sqrt.hpp:138-141`

### [BUG] number_field norm_linear 符号公式错误
- **发现日期**: 2026-03-08 (Session 6) | 被 abs() 掩盖
- **文件**: `include/gnfs/sqrt/number_field.hpp:372-406`
- **描述**: 计算 b^d * f(a/b) 而非 (-b)^d * f(a/b)，但最终取 abs()

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

### [BUG] BitVector::xor_with() 无大小检查
- **发现日期**: 2026-03-08 (Session 6)
- **文件**: `include/gnfs/linalg/sparse_matrix.hpp:354-357`
- **描述**: other 更短时越界

### [BUG] multiply_blocks() 死代码——索引计算错误
- **发现日期**: 2026-03-08 (Session 6) | 从未被调用
- **文件**: `include/gnfs/linalg/sparse_matrix.hpp:298-319`

### [BUG] gauss.hpp history 矩阵 O(n²) 空间浪费——计算但从未使用
- **发现日期**: 2026-03-09 (Session 6) | O(n²) 空间浪费
- **文件**: `include/gnfs/linalg/gauss.hpp:50-58,77-78,88-93`
- **描述**: `eliminate()` 花费 O(n²) 空间维护 history 矩阵（行交换 + XOR 同步），但 `build_null_space()` 从未使用它——零空间构建采用回代法直接从 reduced matrix 推导。Session 18 已移除 `build_null_space` 中的 history/is_pivot_col 参数（`ff4e9b8`），但 eliminate() 中 O(n²) 的 history 计算逻辑仍在。50K 矩阵 ≈ 300MB 浪费
- **建议**: 删除 eliminate() 中的 history 计算逻辑（条件编译为 #if 0 或直接移除）

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

### [BUG] Integer operator+=/operator-= 对 INT64_MIN 参数 UB
- **发现日期**: 2026-03-08 (Session 6) | INT64_MIN 在管线中不会出现
- **文件**: `src/core/integer.cpp:356-357,374-375,384`

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

### [BUG] types.hpp ABPair 注释错误
- **发现日期**: 2026-03-08 (Session 6) | 仅文档
- **文件**: `include/gnfs/core/types.hpp:11`
- **描述**: "a + b*m" 应为 "a - b*m"

### [BUG] matrix_builder exponent 累积用 uint8_t
- **发现日期**: 2026-03-08 (Session 6) | 256≡0 mod 2，溢出不影响 GF(2) 奇偶性
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:534,548`

### [BUG] Kleinjung construct_polynomial 死代码
- **发现日期**: 2026-03-08 (Session 6) | 60 行被 base_m_expansion 覆写
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp:380-439`

### [BUG] sieve_batch() 是死代码
- **发现日期**: 2026-03-08 (Session 6) | stub 代码，从未被管线调用
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:127-131`

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

### [BUG] 无 N 素性检测
- **发现日期**: 2026-03-08 (Session 6) | 可用性问题
- **文件**: 管线入口

### [BUG] smooth_check 浮点精度损失
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/cofactor/smooth_check.hpp:107-108`
- **描述**: std::pow(double,1.0/k) 精度问题

### [OPT] Murphy E-score 低估 20-40%
- **发现日期**: 2026-03-08 (Session 5)
- **文件**: `include/gnfs/polynomial/murphy_evaluator.hpp`

### [OPT] Gauss 消元 history 矩阵 O(n²) 空间
- **发现日期**: 2026-03-08 (Session 5) | 已与 P3 gauss history 条目合并
- **文件**: `include/gnfs/linalg/gauss.hpp`
- **描述**: 见 P3 中 "gauss.hpp history 矩阵 O(n²) 空间浪费" 条目。Session 18 已移除 unused 参数，history 计算逻辑待清理

### [DEBT] 全局性 uint64_t b → int64_t 截断（13 处）
- **发现日期**: 2026-03-08 (Session 5) | b 值始终远小于 INT64_MAX
- **描述**: 理论上 b > INT64_MAX 时截断，实际不会发生

### [DEBT] Schirokauer 文档注释与代码不一致
- **文件**: `include/gnfs/linalg/schirokauer.hpp:138`

### [DEBT] polynomial_context coeff() 返回可变静态引用
- **文件**: `include/gnfs/core/polynomial_context.hpp:76-80`

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
