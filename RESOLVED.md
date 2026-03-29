# RESOLVED — 已完成与误报记录

> 从 `BACKLOG.md` 拆分而来。记录所有已修复、已验证通过的条目和经核查确认的误报。
> 作为项目审计和知识沉淀，避免重复上报、记录修复方案供后续参考。

---

## 已完成 ✅

### P1 级修复 (Session 40 — Special-Q 素数矩阵丢失)

#### [BUG] ~~Special-Q 范围素数指数从矩阵静默丢失~~ ✅
- **发现**: 2026-03-11 (Session 40) — L5 progressive 测试失败
- **解决**: 2026-03-11 (Session 40)
- **修复**: `cofactorizer.hpp:202-222` — trial division 产生的 SQ 范围因子基索引（>= sieve_algebraic_count()）原先直接加入 algebraic_factors，但矩阵构建器只为 sieve 范围索引分配列，导致 SQ 素数指数被静默丢弃。修复：将 SQ 范围索引路由到 algebraic_large_prime（含 p, r, exp），通过大素数列追踪
- **验证**: L1-L5 全通过，L5: 31.91s (61-bit)，smoke 20/20 通过
- **Commit**: `dba9262`

#### [BUG] ~~Schirokauer split 路径非单位元素未剥离 ℓ-part~~ ✅
- **发现**: 2026-03-11 (Session 40)
- **解决**: 2026-03-11 (Session 40)
- **修复**: `schirokauer.hpp:636-693` — 当 P_i | γ 时 γ 不是单位元素，λ_ℓ 公式未定义。修复：gamma == 0 → 返回 0；gamma % ℓ == 0 → 循环除以 ℓ 剥离 ℓ-part 得到单位元素。同时 exponent_k 从 3 提升到 8（ℓ^k=256，处理 v≤6）
- **验证**: 同上
- **Commit**: `dba9262`

#### [BUG] ~~QC 素数选择对 degree≥4 多项式不正确~~ ✅
- **发现**: 2026-03-11 (Session 40)
- **解决**: 2026-03-11 (Session 40)
- **修复**: `matrix_builder.hpp:426-444` — 旧代码用 "无根" 启发式选 QC 素数，但 degree≥4 时 f 可约 mod p 也可能无根。改用 Rabin 不可约性测试 (`ModularPoly::is_irreducible`)
- **验证**: 同上（对 degree≤3 无影响，为 degree≥4 预防性修复）
- **Commit**: `dba9262`

### P3 级安全修复 (Session 39)

#### [BUG] ~~Integer +=/-=/÷= INT64_MIN 取反 UB~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 39)
- **修复**: `integer.cpp` 三个运算符中 `-value` 改为 `-(value+1)+1` 安全取绝对值，避免 INT64_MIN 取反的有符号溢出 UB
- **验证**: smoke 20/20 通过
- **Commit**: `7dbf26e`

#### [BUG] ~~4 处 static Integer zero 别名 + 线程风险~~ ✅
- **发现**: 2026-03-09 (Session 7)
- **解决**: 2026-03-10 (Session 39)
- **修复**: `number_field.hpp:65,195` / `polynomial_context.hpp:77` / `int_polynomial.hpp:71` 中 `static Integer zero` → `static const Integer zero`，消除可变性风险
- **验证**: smoke 20/20 通过
- **Commit**: `f74fdfe`

#### [BUG] ~~rational_sqrt 负号检测到但未应用~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 39)
- **修复**: `rational_sqrt.hpp:138` has_negative 为 true 时应用 `sqrt_value = n - sqrt_value`
- **验证**: smoke 20/20 通过
- **Commit**: `793435b`

#### [BUG] ~~无 N 素性检测~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 39)
- **修复**: `test_gnfs_e2e.cpp` 和 `test_gnfs_progressive.cpp` 管线入口添加 `mpz_probab_prime_p(n, 25)` 检查
- **验证**: smoke 20/20 通过
- **Commit**: `e0c6937`

#### [BUG] ~~smooth_check is_perfect_power 浮点精度损失~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 39)
- **修复**: `smooth_check.hpp` 对 `std::pow(double,1.0/k)` 的 round 结果检查 b-1, b, b+1 三个候选，精确验证 b^k == n
- **验证**: smoke 20/20 通过
- **Commit**: `c0cea89`

#### [DEBT] ~~polynomial_context coeff() 返回可变静态引用~~ ✅
- **发现**: Session 5
- **解决**: 2026-03-10 (Session 39)
- **修复**: 同「4 处 static Integer zero」条目，`polynomial_context.hpp:77` 加 `const`
- **验证**: 同上
- **Commit**: `f74fdfe`

### P1-OPT + P3 级修复 (Session 38)

#### [OPT] ~~ECM Stage 2 BSGS 优化~~ ✅
- **发现**: 2026-02-20 (Session 2)
- **解决**: 2026-03-10 (Session 38)
- **修复**: `ecm.hpp:stage2()` 从朴素逐素数 mont_mul 改为 BSGS 算法：D=2310 (2·3·5·7·11)，预计算 φ(D)=480 baby step 点 d*Q，差分加法链推进 giant step j·D*Q，cross product 检测因子。gcd==n 时回退到朴素实现。预期加速 13-17×
- **验证**: smoke 20/20 通过, test_cofactor 通过
- **Commit**: `6a9926a`

#### [BUG] ~~BitVector::xor_with() 无大小检查~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 38)
- **修复**: `sparse_matrix.hpp:xor_with()` 用 `std::min(bits_.size(), other.bits_.size())` 防止越界
- **验证**: test_linalg 通过
- **Commit**: `69e1219`

#### [BUG] ~~types.hpp ABPair 注释错误~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 38)
- **修复**: `types.hpp:10` "a + b*m" → "a - b*m"（与项目 a-bα 约定一致）
- **Commit**: `2d2a3f1`

#### [BUG] ~~trial_division uint8_t 指数溢出~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 38)
- **修复**: `trial_division.hpp` 有理侧+代数侧 while 循环添加 `exp < 255` 上限
- **验证**: smoke 20/20 通过
- **Commit**: `c64c341`

#### [BUG] ~~multiply_blocks() 死代码~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 38)
- **修复**: 整个函数移除（索引计算错误且从未被调用，BL 使用 BlockVector）
- **Commit**: `a4e39a7`

#### [BUG] ~~sieve_batch() 死代码~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 38)
- **修复**: 整个函数移除（stub，管线使用 sieve_parallel()）
- **Commit**: `a4e39a7`

#### [BUG] ~~Kleinjung construct_polynomial 死代码~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 38)
- **修复**: 60 行手动系数提取移除，直接调用 base_m_expansion()
- **Commit**: `f95d03d`

### P2 级修复 (Session 37c)

#### [BUG] ~~class_group 判别式计算仅对 depressed cubic 正确~~ ✅
- **发现**: 2026-03-08 (Session 5+6)
- **解决**: 2026-03-10 (Session 37c)
- **修复**: `class_group.hpp:compute_discriminant()` 改用 Sylvester 矩阵 Bareiss 行列式计算 Res(f,f')，Δ = (-1)^(d(d-1)/2) · Res(f,f') / a_d，支持任意度数
- **验证**: smoke 20/20 通过, test_class_group 通过
- **Commit**: `36c6341`

#### [BUG] ~~class_group 判别式公式对非 depressed 三次多项式错误~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 37c)
- **修复**: 同上（合并修复）
- **Commit**: `36c6341`

#### [BUG] ~~class_group SNF 不是真正的 Smith Normal Form~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 37c)
- **修复**: `class_group.hpp:compute_smith_normal_form()` 完整 SNF 算法（行/列操作+整除条件），class_number = ∏|d_i| 替代 2^(#generators)
- **验证**: smoke 20/20 通过, test_class_group 通过
- **Commit**: `4c98931`

#### [BUG] ~~IntPolynomial CZ 是 O(p) 暴力搜索~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 37c)
- **修复**: `int_polynomial.hpp:roots_cantor_zassenhaus()` 使用 ModularPoly 实现真正的 CZ 算法：gcd(f, x^p-x) + 随机分裂，O(d² log p)
- **验证**: smoke 20/20 通过, test_int_polynomial 通过
- **Commit**: `da87398`

#### [BUG] ~~Integer 除零行为不一致~~ ✅
- **发现**: 2026-03-09 (Session 6)
- **解决**: 2026-03-10 (Session 37c)
- **修复**: `integer.cpp` 所有 8 个 div/mod 运算符统一在 GMP 调用前检查零，抛出 `std::domain_error`
- **验证**: smoke 20/20 通过, test_integer 通过
- **Commit**: `38072fe`

### P1-OPT + P2 + P3 级修复 (Session 37b)

#### [OPT] ~~lattice_basis 浮点高斯约化不够精确~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 37b)
- **修复**: `lattice_basis.hpp:compute_lattice_basis()` norm²/dot/round 从 double 改为 `__int128_t` 精确整数算术
- **验证**: smoke 20/20 通过, test_special_q 通过, test_integration 通过
- **Commit**: `50436c4`

#### [BUG] ~~Kleinjung is_valid_polynomial() 浮点验证无意义~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 37b)
- **修复**: `kleinjung_selector.hpp:is_valid_polynomial()` 用 Integer 精确验证 f(m) mod n == 0
- **验证**: smoke 20/20 通过
- **Commit**: `9a4f8b5`

#### [BUG] ~~Kleinjung base_m_expansion 系数不平衡~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 37b)
- **修复**: `kleinjung_selector.hpp:base_m_expansion()` 从截断除法 [0,m) 改为平衡展开 [-m/2,m/2]
- **验证**: smoke 20/20 通过
- **Commit**: `9a4f8b5`

#### [BUG] ~~代数因子基射影根在筛选中产生算术垃圾~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 37b)
- **修复**: `lattice_sieve.hpp:sieve_algebraic_side()` 添加 `is_projective()` 跳过
- **验证**: smoke 20/20 通过, test_special_q 通过
- **Commit**: `4925590`

#### [BUG] ~~SmallVector move constructor 不销毁源 inline 元素~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 37b)
- **修复**: `small_vector.hpp` move ctor/assignment 中添加源 inline 元素析构调用
- **验证**: smoke 20/20 通过, test_small_vector 通过
- **Commit**: `b70381c`

#### [BUG] ~~gauss.hpp history 矩阵 O(n²) 空间浪费~~ ✅
- **发现**: 2026-03-09 (Session 6)
- **解决**: 2026-03-10 (Session 37b)
- **修复**: `gauss.hpp:eliminate()` 移除未使用的 history 矩阵（build_null_space 用回代法不需要）
- **验证**: smoke 20/20 通过, test_linalg 通过
- **Commit**: `da4cd54`

### P2 级修复 (Session 37)

#### [BUG] ~~RelationCollector::merge() 未锁定 other 的 mutex~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 37)
- **修复**: `collector.hpp:246` `std::lock_guard` → `std::scoped_lock(mutex_, other.mutex_)` 同时锁两个 mutex
- **验证**: smoke 20/20 通过, test_relation_collector 通过
- **Commit**: `5d81b1f`

#### [BUG] ~~Pollard rho 只使用单一多项式 x²+1~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 37)
- **修复**: `smooth_check.hpp:pollard_rho()` 从固定 c=1 改为依次尝试 c=1,3,5,...,19 共 10 个值
- **验证**: smoke 20/20 通过, test_cofactor 通过
- **Commit**: `3649e16`

#### [BUG] ~~FactorBaseBuilder 实例 build() 返回空 FactorBase~~ ✅
- **发现**: 2026-03-09 (Session 7)
- **解决**: 2026-03-10 (Session 37)
- **修复**: `builder.cpp:build(uint32_t,uint32_t)` 改为 throw std::logic_error（不再静默返回空）
- **验证**: smoke 20/20 通过, test_factor_base 通过
- **Commit**: `0b3af1e`

#### [BUG] ~~lattice_sieve 模运算中间值 int64 溢出~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 37)
- **修复**: `lattice_sieve.hpp` 有理侧+代数侧筛选先对 p 取模再乘法，保证中间值 < 2^64
- **验证**: smoke 20/20 通过, test_special_q 通过
- **Commit**: `234588d`

#### [BUG] ~~SieveRegion default_sieve_region 大 skewness 时 int32 溢出~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 37)
- **修复**: `lattice_basis.hpp:default_sieve_region()` 添加 clamp 到 INT32_MAX-1
- **验证**: smoke 20/20 通过
- **Commit**: `88c2274`

#### [BUG] ~~matrix_builder FB 索引无越界检查~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 37)
- **修复**: `matrix_builder.hpp:build_row()` 有理/代数 FB 索引添加 `idx < num_*_fb` 越界检查
- **验证**: smoke 20/20 通过, test_linalg 通过, test_integration 通过
- **Commit**: `dfebb8d`

### P1 + P2 级修复 (Session 36)

#### [BUG] ~~modular_poly sub()/mod_inverse() 对 p > INT64_MAX 溢出~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 36)
- **修复**: `modular_poly.hpp` sub() 改为纯 uint64_t 模减法; mod_inverse() 改用 `__int128_t`
- **验证**: smoke 20/20 通过, integration 24/24 通过
- **Commit**: `13d9ace`

#### [BUG] ~~ModularPoly::is_irreducible 首项系数 ≡ 0 (mod p) 时断言崩溃~~ ✅
- **发现**: 2026-03-10 (Session 30)
- **解决**: 2026-03-10 (Session 36)
- **修复**: `modular_poly.hpp` is_irreducible() 首项 ≡ 0 (mod p) 返回 false; reduce() assert 改为 throw
- **验证**: smoke 20/20 通过, integration 24/24 通过
- **Commit**: `39a6a0c`

#### [BUG] ~~Cofactorizer::stats_ 无 mutex 保护~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 36)
- **修复**: `cofactorizer.hpp` CofactorizerStats 全部字段改为 std::atomic<size_t>, stats() 返回 Snapshot 值类型
- **验证**: smoke 20/20 通过, integration 24/24 通过
- **Commit**: `34bf212`

#### [BUG] ~~ECM 固定随机种子导致重复曲线~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 36)
- **修复**: `ecm.hpp` rng seed 改为 random_device ^ n 低 64 位
- **验证**: test_cofactor 通过
- **Commit**: `759835d`

#### [BUG] ~~ECM Stage 2 链式乘法因子丢失~~ ✅
- **发现**: 2026-03-09 (Session 6)
- **解决**: 2026-03-10 (Session 36)
- **修复**: `ecm.hpp` stage2() gcd==N 时保存 checkpoint + batch, 回退逐素数检查恢复因子
- **验证**: test_cofactor 通过, smoke 20/20 通过
- **Commit**: `759835d`

### TEST 级关闭 (Session 35)

#### [TEST] ~~边界/极端情况覆盖率 ~98% → 完成~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 35)
- **最终状态**: test_edge_cases 43 个测试函数（12 组 ~89 子测试），覆盖全部可独立测试的边界场景
- **覆盖范围**: Integer 边界、SparseMatrix/BitVector/Gaussian/BL 退化、ECM、cofactor/trial_division、relation 序列化、collector/filter、sieve params/special_q、HenselSqrt(8)、Schirokauer(6)、NumberField(13)、MatrixBuilder(4)、RationalSqrt(2)、AlgebraicSqrt(4)、CouveignesSqrt(4)、PolynomialContext(12)、FactorBase(10)、ModularPoly(14)、LatticeBasis(8)、ClassGroup(8)
- **关闭理由**: 仅剩 Hensel 精度充分性验证（需完整管线数据），已由 test_gnfs_progressive L1-L5 覆盖，不适合独立单元测试
- **验证**: smoke 20/20 通过(6.4s)、test_edge_cases 43/43 通过(1.6s)
- **Commit**: `6c0d807` (代码) / `666b424` (文档)

#### [TEST] ~~模块间集成测试 → 完成~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 35)
- **最终状态**: test_integration 24 个跨模块场景，覆盖全部关键管线组合
- **覆盖范围**: Cofactorizer N-div rejection、stats tracking、Cof→Collector、Collector→Filter、MatrixBuilder real FB、MatrixBuilder→BL dependency、MurphyEvaluator real ctx、FactorBase sieve invariants、Schirokauer+MatrixBuilder、MatrixBuilder all columns、Sieve→Cofactorizer joint、RationalSqrt+AlgebraicSqrt full gcd、large-scale relations→matrix(1200+)、sieve_parallel consistency、BaseMSelector→ctx verify(6N)、Filter→MatrixBuilder shrink、FB bounds→relations、alg_norm+rat_value consistency、LatticeBasis→Sieve geometry、FB→MatrixBuilder cols、ClassGroup character size、MurphyE score finiteness
- **关闭理由**: 无核心缺口，所有关键模块间接口已覆盖
- **验证**: test_integration 24/24 通过(7.2s)
- **Commit**: `6c0d807` (代码) / `666b424` (文档)

### TEST 级修复 (Session 33)

#### [TEST] ~~边界/极端情况覆盖率 ~90% → ~95%~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 33)
- **修复**: test_edge_cases 从 36→40 个测试用例（含 9 组 59 个子测试），新增 4 组：
  - **AlgebraicSqrt 退化输入** (4 sub): 空依赖→error、config构造(use_couveignes=false)、长dep越界、禁用Couveignes回退
  - **CouveignesSqrt 退化输入** (4 sub): 空pairs→one()、config构造、max_prime_checks=3→失败、num_primes=0→nullopt
  - **PolynomialContext 构造边界** (12 sub): 空系数→exception、degree0、尾零剥离、越界coeff→zero、evaluate(0)=f_0、evaluate_mod(p=1)=0、evaluate_mod(x=0)=f_0%p、负系数evaluate、verify()真实多项式、norm(0,0)=0、rational_value(m,1)=0、skewness默认
  - **FactorBase 查找构造** (10 sub): 空FB counts=0、空FB lookup→nullopt(rat/alg)、add+find rational、add+find algebraic(含wrong root)、sieve_algebraic_count默认、显式set_sieve_count、build_index重建、仅rational/仅algebraic
- **验证**: `./build/test_edge_cases` 全 40 通过(1.5s)；smoke 20/20 通过(6.2s)
- **Commit**: `08d5147`

#### [TEST] ~~模块间集成测试 16/30 → 20/30~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 33)
- **修复**: test_integration 从 16→20 个测试用例，新增 4 个：
  - **Test 17**: BaseMSelector→PolynomialContext verify 一致性（6个N值，degree 2-3，全部 f(m)≡0 mod N）
  - **Test 18**: Filter singleton removal→MatrixBuilder 维度缩减验证（unfiltered=347→filtered=302, singletons=45）
  - **Test 19**: FactorBase bounds 敏感性→关系产出（small FB rat=15 vs large FB rat=93）
  - **Test 20**: algebraic_norm + rational_value 一致性（87个光滑关系 both non-zero）
- **验证**: `./build/test_integration` 全 20 通过(8.3s)
- **Commit**: `08d5147`

### TEST 级修复 (Session 32)

#### [TEST] ~~边界/极端情况覆盖率 ~85% → ~90%~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 32)
- **修复**: test_edge_cases 从 33→36 个测试用例（含 5 组 33 个子测试），新增 3 组：
  - **NumberField 算术边界** (13 sub): zero/one 元素、multiply zero*alpha=zero、from_ab 退化(0,0/5,0/0,1/-100,3)、norm_linear b=0/a=0 退化、evaluate_at_m zero/alpha、大 a crash safety
  - **MatrixBuilder 退化输入** (4 sub): 空关系→0行矩阵、单关系→1行、禁用所有列配置、build_with_qc 空输入
  - **RationalSqrt 退化输入** (2 sub): 空依赖(全零 BitVector)、单关系偶指数
- **验证**: `./build/test_edge_cases` 全 36 通过；smoke 20/20 通过
- **Commit**: `db04dd6`

#### [TEST] ~~模块间集成测试 15/30 → 16/30 + sieve_parallel 关闭~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 32)
- **修复**: test_integration 从 15→16 个测试用例，新增：
  - **Test 16**: sieve_parallel vs sequential 一致性验证（N=10403, 5 SQ, 2 线程并行，候选数完全匹配 seq=par=8）
- **验证**: `./build/test_integration` 全 16 通过
- **Commit**: `db04dd6`

### TEST 级修复 (Session 31)

#### [TEST] ~~边界/极端情况覆盖率 ~75% → ~85%~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 31)
- **修复**: test_edge_cases 从 31→33 个测试用例（含 14 个子测试），新增 2 组：
  - **HenselSqrt** (8 sub): 空输入→Integer(1)、Config 变体(precision=0/2000, prime_start=100000)、大 a 值、负 a 值、多种 pair 组合 crash safety
  - **Schirokauer 大域 ℓ** (6 sub): ℓ=3/5/7 值域验证([0,ℓ))、多素数[2,3]→2×degree 列、空素数→0 列、ℓ=3 确定性
- **验证**: `./build/test_edge_cases` 全 33 通过；smoke 20/20 通过
- **Commit**: `f1935b3`

#### [TEST] ~~缺少模块间集成测试 12/30 → 15/30（基本完成）~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 31)
- **修复**: test_integration 从 12→15 个测试用例，新增 3 个：
  - **Test 13**: Sieve→Cofactorizer 联合（LatticeSieve + SpecialQ + Cofactorizer，FB 扩展 special_q_bound=500）
  - **Test 14**: RationalSqrt + AlgebraicSqrt 联合（完整 gcd 分解 N=143 → 13×11，含 sign 列矩阵构建）
  - **Test 15**: 大规模关系→矩阵流水线（N=10403，1200+ 关系，sign+Schirokauer 列，BL 依赖验证）
- **验证**: `./build/test_integration` 全 15 通过；smoke 20/20 通过
- **Commit**: `f1935b3`

### TEST 级修复 (Session 30)

#### [TEST] ~~边界/极端情况覆盖率 ~60% → ~75%~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 30)
- **修复**: test_edge_cases 从 20→31 个测试用例，新增 11 个：
  - ECM edge cases (n=1, prime, small composite)
  - cofactor classify (1, 2, 4, lpb=1, perfect power/square)
  - trial division (0, 1, negative, large prime)
  - relation serialization round-trip
  - relation collector (empty, dup, max, merge)
  - relation filter (empty, singletons, full, disabled)
  - sieve params uint8 overflow
  - special_q validity (0, 1, 2)
  - quick cofactor check (cofactor=0, 1, lpb=0)
- **验证**: `./build/test_edge_cases` 全 31 通过；smoke 20/20 通过
- **Commit**: `d472e71`

#### [TEST] ~~缺少模块间集成测试 8/30 → 12/30~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 30)
- **修复**: test_integration 从 8→12 个测试用例，新增 4 个：
  - Schirokauer + MatrixBuilder 列数验证 (build_with_qc)
  - MatrixBuilder 列映射一致性 (sign + Schirokauer)
  - Full mini-pipeline (Cofactorizer→Collector→Filter→MatrixBuilder→BL)
  - Schirokauer map consistency (值域 [0,ℓ) + 确定性)
- **注意**: 使用 N=10403 (非 N=143) 因 ModularPoly::is_irreducible 对偶数首项系数断言失败 (新 BACKLOG 条目)
- **验证**: `./build/test_integration` 全 12 通过；smoke 20/20 通过
- **Commit**: `d472e71`

### TEST 级修复 (Session 29)

#### [TEST] ~~缺少模块间集成测试（大部分）~~ ✅ (部分，8/30 主要接口覆盖)
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 29)
- **修复**: 新增 `test_integration`（8 个跨模块测试用例）：
  1. **Cofactorizer + PolyCtx + FB**: N-divisible 对被拒绝（Session 1 bug regression 集成验证）
  2. **Cofactorizer stats**: 141 候选中 accepted/rejected 统计正确
  3. **Cofactorizer → RelationCollector**: 100 个关系正确流入 collector
  4. **RelationCollector → RelationFilter**: 7→5，单例被删，配对被保留
  5. **MatrixBuilder + real FB**: 4 行 × 23 列（rat=13, alg=10）维度正确
  6. **MatrixBuilder → BlockLanczos**: r0⊕r1⊕r2=0 依赖被 BL 正确发现
  7. **MurphyEvaluator + PolyCtx**: N=10403 评分有限（log_e=-0.375）
  8. **FB sieve count 不变式**: 三种 N 均满足 sieve_alg_count≤alg_count
- **验证**: `./build/test_integration` 全通过；elapsed <2s
- **Commit**: `9e0ecfc`

### TEST 级修复 (Session 28)

#### [TEST] ~~边界/极端情况覆盖率约 15%~~ ✅ (提升至 ~60%)
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 28)
- **修复**: 新增 `test_edge_cases`（20 测试用例），专攻三类缺口：
  - **Integer 边界** (6 项)：负 mod 截断语义、gcd 零输入、sqrt(0/完全平方/非完全平方)、pow/powmod 边界(exp=0/base=0/1)、INT64_{MIN,MAX} 加减不截断
  - **SparseMatrix/SparseRow 空矩阵** (6 项)：0行/0列矩阵、1×1 零/一矩阵 Gaussian、全零 rank=0；SparseRow 空行 first/last_nonzero=UINT32_MAX；set() 双次 GF(2) toggle 语义验证
  - **BitVector 字边界** (4 项)：size=0、size=1、63/64/65/128 word 边界 bit 正确访问；XOR 跨 word 边界
  - **Gaussian/BlockLanczos 退化** (4 项)：identity 矩阵 rank=3、单行 rank=1、0行/0列 empty→no deps
- **额外发现**: SparseRow::empty() 不调用 ensure_sorted()，与 weight()/test() 语义不一致（BACKLOG 已记录）
- **验证**: `./build/test_edge_cases` 全通过；smoke 20/20 全通过 <5s
- **Commit**: `8da3f84`

### TEST 级修复 (Session 27)

#### [TEST] ~~4 个模块无专属单元测试~~ ✅ (全覆盖)
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 28)
- **修复**: 新增 4 个模块专属测试：
  - `test_polynomial_context`（13 项）: evaluate/evaluate_mod/algebraic_norm/rational_value/verify/clone/边界
  - `test_base_m`（12 项）: f(m)=N 精确验证/多 degree/create_context/select_poly API
  - `test_polynomial_optimizer`（22 项）: derivative/translate/rotate/skewness/golden section/smooth numbers/newton root
  - `test_class_group`（16 项）: PrimeIdeal/IdealClass/discriminant公式/MB/trivial判定/generators
- **验证**: smoke 19/19 通过，各测试独立全通过
- **Commit**: `e34c800`

#### [TEST] ~~7 个模块无专属单元测试~~ ✅ (部分，Session 27)
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 27)
- **修复**: 新增 3 个模块专属测试：`test_params`（14 项）、`test_int_polynomial`（15 项）、`test_filter`（13 项）。覆盖 params、int_polynomial、filter 模块。
- **验证**: smoke 15/15 通过
- **Commit**: `bbe711d`

#### [TEST] ~~0 个已修复 bug 的回归测试~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 27)
- **修复**: 新增 `test_regressions`（14 项回归测试）覆盖：Integer uint64_t 构造、Schirokauer 指数公式、evaluate_mod 溢出、非 monic 多项式、base-m 不可约、代数范数 a-bα 约定、rational_value、SQ 范围、筛区上限、ℓ=2 约定、导数边界、mod_inverse、params 边界
- **验证**: smoke 15/15 通过
- **Commit**: `bbe711d`

### P2 级修复 (Session 26)

#### [BUG] ~~params.hpp special_q_min = rational_bound/5 落入因子基范围~~ ✅
- **发现**: 2026-03-09 (Session 6)
- **解决**: 2026-03-10 (Session 26)
- **修复**: 三层修复：(1) `factor_base.hpp` 新增 `sieve_algebraic_count_` 追踪筛选/SQ 边界；(2) `builder.cpp` 新增 `special_q_bound` 选项 + `find_algebraic_primes_range()` 在因子基界以上构建 SQ 专用代数素数；(3) `params.hpp` 修改 `special_q_min = algebraic_bound + 1`, `special_q_max = 3×algebraic_bound`；(4) `lattice_sieve.hpp` 筛选只使用 ≤ algebraic_bound 的素数
- **验证**: 回归测试（SQ 全在 FB 界以上）+ smoke 11/11 + E2E + progressive L1-L2 通过
- **Commit**: `c6c6f9f`

### P2 级修复 (Session 25)

#### [BUG] ~~MurphyEvaluator rng_ 多线程数据竞争~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 25)
- **修复**: `murphy_evaluator.hpp` — 移除类成员 `std::mt19937_64 rng_`，改为 `sample_e_score_log()` 内创建函数局部 RNG。所有评估方法标记 `const`。`kleinjung_selector.hpp` 参数改为 `const MurphyEvaluator&`
- **验证**: 8 线程并发回归测试（结果一致） + smoke 11/11 通过
- **Commit**: `669fed0`

### P2 级修复 (Session 24)

#### [BUG] ~~estimate_initial_log NaN/Inf → uint16_t UB~~ ✅
- **发现**: 2026-03-09 (Session 7)
- **解决**: 2026-03-10 (Session 24)
- **修复**: `include/gnfs/sieve/lattice_sieve.hpp:209-231` — (1) `typical_i` 从半宽度改为 `range/4`（E[|i|]），与 `typical_j` 一致；(2) `rat_val` 和 `typical_a` 添加 `std::max(1.0, ...)` 下限防止 `log2(0)=-Inf`；(3) 最终 `isfinite + 非负` guard 防止所有 NaN/Inf 到 uint16_t 的 UB
- **验证**: 回归测试（退化 1×1 区域、零 j 区域、正常区域）+ smoke 11/11 通过
- **Commit**: `5a2c4ea`

#### [BUG] ~~estimate_initial_log typical_i/typical_j 不一致~~ ✅
- **发现**: 2026-03-09 (Session 6)
- **解决**: 2026-03-10 (Session 24)
- **修复**: 同上（typical_i 改为 range/4，消除 2x 偏高）
- **Commit**: `5a2c4ea`

### P1 级修复 (Session 23)

#### [BUG] ~~L5/25-digit 测试失败 — 代数大素数缺失素理想根 r~~ ✅
- **发现**: 2026-03-10 (Session 22)
- **解决**: 2026-03-10 (Session 23)
- **根因**: `cofactorizer.hpp:add_large_primes()` 对代数侧大素数使用 2-arg `PrimePower{p, e}`（r=0），但 `matrix_builder.hpp` 使用 per-(p,r) 列映射。所有代数 LP 映射到 `(p,0)` 列，无法区分同一素数 p 上方的不同素理想。零空间给出的依赖中各理想指数非偶数，代数乘积非完全平方，sqrt 必然失败
- **修复**: (1) `cofactorizer.hpp` 新增 `compute_alg_lp_root(a,b,p)` 计算 `r = a·b⁻¹ mod p`；(2) 新增 `add_algebraic_large_primes()` 使用 3-arg `PrimePower{p, r, e}`；(3) `params.hpp` 阈值改为 `3.5*log_scale` + target_excess 覆盖额外列；(4) `schirokauer.hpp` 完美幂/无 mult-1 因子回退到 unsplit 而非零填充；(5) `hensel_sqrt.hpp` f'(α)² 技巧处理 O_K/Z[α] 指标
- **验证**: smoke 11/11 通过 + L1-L5 全部通过（L5: 38.53s, dep#2 成功）
- **Commit**: `9950450` (cofactorizer), `f509907` (params), `5c89f2c` (schirokauer), `0d5f6ea` (hensel+algebraic_sqrt)

### P1-OPT 级修复 (Session 22)

#### [OPT] ~~Block Lanczos 是 25-digit 的主要瓶颈（需并行化）~~ ✅
- **发现**: 2026-02-22 (Session 3)
- **解决**: 2026-03-10 (Session 22)
- **修复**: `src/linalg/block_lanczos.cpp` 完全重写 `block_lanczos_solve()`：(1) SpMV forward/transpose 使用 ThreadPool parallel_for_index 并行化；(2) inner_product 和 xor_with_mul 使用 thread-local 归约并行化；(3) 旋转指针池替代 per-iteration BlockVector 分配；(4) ParallelContext 预分配所有线程局部缓冲区；(5) `mask_cur == 0` 提前终止
- **验证**: smoke 11/11 通过 + linalg 模块全部通过（含新增的 parallel BL correctness 测试）
- **Commit**: `0de6280`, `f70bf21`

#### [BUG] ~~SparseRow const_cast ensure_sorted() 多线程并发排序是 UB~~ ✅
- **发现**: 2026-03-09 (Session 7)
- **解决**: 2026-03-10 (Session 22)
- **修复**: `include/gnfs/linalg/sparse_matrix.hpp` 新增 `SparseMatrix::ensure_all_sorted()` 方法，在 Block Lanczos 并行 SpMV 前一次性排序所有行，避免并发 const_cast 修改
- **验证**: smoke 11/11 通过 + 新增 `test_ensure_all_sorted()` 测试
- **Commit**: `8bbb6e8`

#### [BUG] ~~Block Lanczos 终止条件仅检查 V_cur 为零~~ ✅
- **发现**: 2026-03-09 (Session 6)
- **解决**: 2026-03-10 (Session 22)
- **修复**: `src/linalg/block_lanczos.cpp` 在 `V_cur.is_zero()` 检查后添加 `if (mask_cur == 0) break;` 提前终止
- **验证**: smoke 11/11 通过
- **Commit**: `0de6280`

### P2 级修复 (Session 19)

#### [BUG] ~~编译器 warning: test_relation_collector.cpp 未使用的 lambda 捕获~~ ✅
- **发现**: 2026-03-10 (Session 19)
- **解决**: 2026-03-10 (Session 19)
- **修复**: `tests/test_relation_collector.cpp:185` 从 lambda 捕获列表移除 `per_thread`（const int 隐式 constexpr 不需要捕获）
- **验证**: 编译零 warning + `test_relation_collector` 全部通过
- **Commit**: `ac964aa`

#### [BUG] ~~VSCode CMake 配置 CMAKE_BUILD_TYPE 丢失~~ ✅
- **发现**: 2026-03-10 (Session 19)
- **解决**: 2026-03-10 (Session 19)
- **修复**: 创建 `CMakePresets.json`（debug/release/relwithdebinfo 三个 preset），持久化 BUILD_TYPE 配置
- **验证**: `cmake --preset debug` 输出 `Build Type: Debug` ✓
- **Commit**: `9136853`

### P3 级修复 (Session 19)

#### [DEBT] ~~ninja 构建系统未安装~~ ✅
- **发现**: 2026-03-10 (Session 19)
- **解决**: 2026-03-10 (Session 19)
- **修复**: `brew install ninja` + CMakePresets.json 添加 `"generator": "Ninja"` + `scripts/test.sh` 改用 `cmake --build`（generator 无关）
- **验证**: ninja 1.13.2 安装成功 + smoke 11/11 通过
- **Commit**: `2bafff6`

### P0 级修复

#### [BUG] ~~compute_log_prime() 系统性低估所有素数对数值~~ ✅
- **发现**: 2026-03-09 (Session 6)
- **解决**: 2026-03-09 (Session 11)
- **修复**: `src/factor_base/builder.cpp` 3 处调用改为 `compute_log_prime_precise()`
- **验证**: smoke 11/11 通过 + L1-L2 progressive 5/5 通过
- **Commit**: `b3bbe3a`

#### [BUG] ~~Couveignes rat_sqrt 对合数 N 计算根本性错误~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-09 (Session 13)
- **修复**: 移除错误的 `powmod(rat_product, (N+1)/4, N)` 计算，改用 `Y² ≡ ∏(a_i - b_i·m) mod N` 直接验证。无需计算模合数平方根
- **验证**: smoke 11/11 通过 + L1-L2 progressive 全部通过
- **Commit**: `8abb0d3`

#### [BUG] ~~rational_sqrt 验证函数声称验证但实际什么也不做~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-09 (Session 12)
- **修复**: `rational_sqrt.hpp:143-170` 实现验证逻辑：计算 ∏(a_i - b_i·m) mod N 与 X² mod N 对比
- **验证**: smoke 11/11 通过 + L1-L2 progressive 全部通过
- **Commit**: `5786188`

### P1 级修复

#### [BUG] ~~base_m.cpp select() 不验证 f 不可约~~ ✅
- **发现**: 2026-03-09 (Session 6)
- **解决**: 2026-03-10 (Session 21)
- **修复**: `base_m.cpp:select()` 新增 `check_irreducible_over_Q()` 对 15 个小素数做 mod-p Rabin 测试；若 m_base 的多项式可约，自动尝试 m±1..±5 共 11 个候选。附带修复 `construct_base_m_poly()` 使 f(m)=N 对任意 m 成立（旧代码 f[d]=temp 替换导致扰动时 f(m)≠N）
- **验证**: smoke 11/11 通过 + 回归测试 N=1320 (m=10 可约→自动选 m=9 不可约) 通过
- **Commit**: `d38ab90`

#### [BUG] ~~Schirokauer factorize_and_setup 重复根处理~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 20)
- **修复**: `schirokauer.hpp:factorize_and_setup()` 三情况处理：(1) f 是完美幂 → 零填充所有列；(2) f 无平方因子 → 标准多因子 Hensel 提升（不变）；(3) 重复根 → 计算每个因子在 f 中的重数，仅 Hensel 提升重数=1 的因子（与余因子互素），其余列零填充。旧代码回退到 unsplit 模式用 γ^(ℓ^d-1)，但 f 不可约时才正确——导致错误 Schirokauer 值使依赖全部无效
- **验证**: smoke 11/11 通过 + 3 个新回归测试（重复根、完美幂、无平方可约）全部通过
- **Commit**: `56cb783`

#### [BUG] ~~Split Schirokauer: f mod 2 可约时映射精度不足~~ ✅
- **发现**: 2026-02-20 (Session 2)
- **解决**: 2026-03-10
- **修复**: `schirokauer.hpp` 新增 `GFPolyOps` 结构体实现 GF(ℓ) 上完整多项式因式分解（DDF + EDF/Cantor-Zassenhaus）和递归多因子 Hensel 提升。替换旧的暴力搜根逻辑，使无线性因子的可约情况（如 x⁵+x⁴+1 ≡ (x²+x+1)(x³+x+1) mod 2）也能正确分解并提升到 mod ℓ^k
- **验证**: smoke 11/11 通过 + L1-L2 progressive 通过 + linalg/sqrt 模块全部通过
- **Commit**: `d1293f6`

#### [BUG] ~~Couveignes 符号归一化不一致导致 CRT 重建错误~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 17)
- **修复**: `couveignes.hpp` compute_from_element() 移除错误的 eval_at_1 > p/2 per-prime 归一化（不同素数阈值不同 → 符号不一致），改用 Gray code 枚举 + Y² 验证，与 compute() 统一
- **验证**: smoke 11/11 通过 + sqrt 模块测试全部通过
- **Commit**: `29ce7fc`

#### [BUG] ~~Couveignes Gray Code 系数漂移——翻转间无 mod M 约化~~ ✅
- **发现**: 2026-03-09 (Session 6)
- **解决**: 2026-03-10 (Session 17)
- **修复**: `couveignes.hpp` compute() 的 verify_current/extract_result 在 % N 前先 % M → center → % N，防止漂移的 k*M 项（gcd(M,N)=1 → k*M mod N ≠ 0）破坏结果
- **验证**: smoke 11/11 通过 + sqrt 模块测试全部通过
- **Commit**: `29ce7fc`

#### [BUG] ~~matrix_builder QC 系数 int64 截断~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 18)
- **修复**: `matrix_builder.hpp` select_qc_primes() 用 `Integer % p` 替换 `fits_int64() ? to_int64() : 0` 截断逻辑；同时修复 N > 2^64 时整除性检查被跳过
- **验证**: smoke 11/11 通过
- **Commit**: `becd25a`

#### [BUG] ~~Hensel poly_inverse_mod_direct p^d uint64 溢出~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-10 (Session 19)
- **修复**: `hensel_sqrt.hpp:544-546` 费马小定理计算 a^{p^d-2} 的指数从 uint64_t 改为 Integer。d=6, p=2000 时 p^d=6.4×10^19 超过 UINT64_MAX。同文件 line 126 和 modular_poly.hpp:322 已有正确写法
- **验证**: sqrt 模块测试全部通过 + E2E 通过
- **Commit**: `576d933`

#### [BUG] ~~matrix_builder 多项式度 > 8 时数组越界~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-10 (Session 18)
- **修复**: `schirokauer.hpp` SchirokaurMap 构造函数新增 `degree > FastPoly::MAX_DEGREE` 守卫；`matrix_builder.hpp` 前置 `can_use_schirokauer` 检查，degree 过大时用额外 QC 列补偿
- **验证**: smoke 11/11 通过
- **Commit**: `becd25a`

#### [BUG] ~~ECM sieve_primes(B2) 内存爆炸~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-09 (Session 14)
- **修复**: 新增 `for_each_prime_in_range()` 分段筛法，内存从 O(B2) 降到 O(√B2 + 1M)。Stage 2 不再整筛 [2, B2]，改为分段处理 (B1, B2]
- **验证**: smoke 11/11 通过 + cofactor 测试通过
- **Commit**: `8974849`

#### [BUG] ~~Sieve 区域对大 N 导致灾难性内存分配（>100GB）~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-09 (Session 15)
- **修复**: `params.hpp` 和 `lattice_basis.hpp` 新增 `MAX_SIEVE_AREA = 256M positions` 面积上限。计算 width×height 后若超限则等比缩小。100-digit N: 32GB/线程 → 512MB/线程（减少 62.5×）
- **验证**: smoke 11/11 通过 + L1-L2 progressive 5/5 通过 + 200/332-bit 参数验证
- **Commit**: `86a746c`

#### [BUG] ~~RelationCollector callback 在非递归 mutex 下调用——回调内访问 collector 死锁~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-09 (Session 16)
- **修复**: callback 调用移至 mutex 作用域外；add() 在锁内 clone 关系数据，锁外调用 callback；set_callback() 新增 mutex 保护。新增回归测试 test_callback_no_deadlock
- **验证**: smoke 11/11 通过 + L1-L2 progressive 通过
- **Commit**: `c27804c`

#### [BUG] ~~Couveignes 回退公式 (N+1)/2 对所有 N 都数学错误~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-09 (Session 13)
- **修复**: 整个 `powmod((N+1)/4)` + `(N+1)/2` 回退块被移除，改用 Y² 直接验证
- **验证**: smoke 11/11 通过 + L1-L2 progressive 全部通过
- **Commit**: `8abb0d3`（随 Couveignes rat_sqrt 一并修复）

#### [BUG] ~~algebraic_sqrt compute_heuristic() 数学不正确~~ ✅
- **发现**: 2026-03-08 (Session 5)
- **解决**: 2026-03-09 (Session 17)
- **修复**: `compute_heuristic()` 改为始终返回失败（`product^((N+1)/2)` 仅对素数有效，对合数 N 无数学依据）。测试中两处 `use_couveignes=false` 的 heuristic fallback 改为使用正常 Hensel→Couveignes 流程
- **验证**: smoke 11/11 通过 + L1-L2 progressive 通过
- **Commit**: `088a48d`

#### [BUG] ~~Schirokauer Hensel 提升 quadratic factor 访问未构建的 prime_info_~~ ✅
- **发现**: 2026-03-08 (Session 6)
- **解决**: 2026-03-09 (Session 10)
- **修复**: `schirokauer.hpp` hensel_lift_factor 改用 info.factors + synthetic division 计算提升后余因子
- **验证**: `./build/test_gnfs_progressive 1 1` N=9991 成功分解 97×103
- **Commit**: `ab278c6`

### P2 级修复

#### [BUG] ~~RelationCollector::set_callback() 无 mutex 保护~~ ✅
- **发现**: 2026-03-09 (Session 7)
- **解决**: 2026-03-09 (Session 16)
- **修复**: set_callback() 新增 `std::lock_guard<std::mutex>` 保护（随 P1 callback 死锁一并修复）
- **验证**: smoke 11/11 通过
- **Commit**: `c27804c`

### 早期修复（Session 1-3）

| 条目 | Commit |
|------|--------|
| Integer(uint64_t) 构造函数 | `b0e79f9` |
| Relation::ab() b=INT64_MIN UB | `4b4ec08` |
| std::abs(INT64_MIN) UB（7 处） | `4b4ec08`, `3789872` |
| modular_poly p=2 Tonelli-Shanks | `145201c` |
| NumberField monic 假设 | `ec2aa32` |
| polynomial_optimizer Newton divmod | `82bbec1` |
| newton_root() 验证永远成功 | `82bbec1` |
| Hensel/Couveignes 不可约性检查（Rabin 测试） | `7516710` |
| trial_division divide_exact() int64 溢出 | `b0e79f9` |
| ThreadPool pending_ 竞态 | `e6dd3f7` |
| MatrixBuilderConfig schirokauer_primes {2,3}→{2} | `5791463` |
| smooth_check large_prime_bound² uint64 溢出 | `783294a` |
| cofactorizer 大素数 uint32→uint64 | `3b93104` |
| Couveignes 无上限搜索循环 | `41213e1` |
| polynomial_optimizer divmod 参数命名 | `82bbec1` |
| matrix_builder 存储实际 primes | `5791463` |
| FactorBaseParams large_prime_bound uint64 | `3b93104` |
| smooth_check quick_cofactor_check lpb² | `783294a` |
| Hensel Sqrt 预计算优化 | Session 3 |
| 代数侧大素数映射按 (p,r) 素理想键索引 | `273dcdd` |
| compute_log_prime() 系统性低估 | `b3bbe3a` |
| Schirokauer split 路径 hensel_lift_factor SIGSEGV | `ab278c6` |
| rational_sqrt 验证函数 no-op → 实际验证 | `5786188` |
| Couveignes rat_sqrt 对合数 N 错误 → Y² 直接验证 | `8abb0d3` |
| Couveignes (N+1)/2 回退公式错误 → 随 rat_sqrt 移除 | `8abb0d3` |

---

## 核查为误报

> 经源码验证确认不存在的问题。保留作为审计记录，避免后续重复上报。

| 原始分类 | 条目 | 文件 | 误报原因 |
|----------|------|------|----------|
| P1 | modular_poly q_minus_2 uint64 溢出 | `modular_poly.hpp:529-531` | 代码中不存在此模式，所有大指数运算使用 Integer/uint128 |
| P1 | Eratosthenes 筛法 p*2 溢出 | `builder.cpp:80,102,131` | p 最大 = rational_bound（cap 在 1e9），p*2 < UINT32_MAX |
| P1 | class_group factor_ideal val=0 无限循环 | `class_group.hpp:383-393` | `val != 0` 守卫阻止了循环（exp=0 丢失贡献是独立 P3 条目） |
| P1 | Schirokauer precompute_for_prime "无根=不可约" d≥4 | `schirokauer.hpp:369-385` | 已修复——代码已调用 `ModularPoly::is_irreducible()`（Rabin 测试） |
| P1 | ThreadPool func 引用捕获 | `thread_pool.hpp:104,140` | parallel_for 通过 future.get() 等待完成，func 存活期间安全 |
| P0 | Hensel S[i].to_uint64() 截断 | `hensel_sqrt.hpp:115` | 初始步骤中 S 是 mod p（小素数），值安全 |
| P0 | Couveignes Gray Code __builtin_ctzll(0) | `couveignes.hpp` | Gray 码恰差 1 位，输入永远非零 |
| P0 | Gaussian 消元 pivot_cols.back() 空容器 | `gauss.hpp:100-101` | 三元运算符守卫 |
| P1 | Integer::powmod() 不验证负指数 | `integer.hpp` | GMP mpz_powm 正确处理负指数（计算逆元） |
| P1 | matrix_builder f mod 2 检查 uint64 截断 | `matrix_builder.hpp:196-201` | 代码已使用 `Integer % 2` 后再 `to_uint64()`，结果为 0/1，无截断风险 |
| P1 | FastPoly reduce_inplace 系数潜在溢出 | `schirokauer.hpp:87-91` | 公式 `m - (t - a.coeffs[idx])` 数学正确：t 和 a.coeffs[idx] 均在 [0,m)，差值在 (0,m)，结果在 (0,m)。mul_raw 确保输入 ∈ [0,m)，reduce_inplace 维持不变量。Schirokauer 中 m=ℓ^k=8，远离溢出边界 |
| P2 | Hensel 提升无精度充分性验证 | `hensel_sqrt.hpp` | retry 机制（4次递增精度）+ Y²≡P(mod N) 终端验证已构成完整验证系统。200-bit 余量使首次成功率极高，retry 捕获极罕见的精度不足。centering 后无需单独验证 |
| P3 | norm_linear 符号公式 (-b)^d 应替代 b^d | `number_field.hpp:304` | 数学验证：N(a-bα) = b^d · f(a/b) 是正确公式。(-b)^d · f(a/b) = Res(a-bx, f) 是结式而非范数。用 f(x)=x³-2, a=b=1 验证：b^d·f(a/b)=-1=Π(a-bα_j)✓，(-b)^d=1≠范数✗ |
