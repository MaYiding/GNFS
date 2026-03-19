# BACKLOG — 待办备忘录

> 记录发现但当前不处理的问题。每个条目必须有：分类、来源、描述、发现日期。
> 已解决的条目移到末尾「已完成」区域，不要删除。

---

## P0 — 正确性关键（必须修复）

### [BUG] ~~Integer 缺少 uint64_t 构造函数，导致大值静默溢出~~ ✅
- **状态**: ✅ 已完成
- **发现日期**: 2026-02-20 (Session 2), 2026-03-08 审计确认
- **解决日期**: 2026-03-09
- **解决方式**: 新增 `Integer(uint64_t)` 构造函数（使用 `mpz_init_set_ui`），同时新增 `Integer(int)` 和 `Integer(unsigned int)` 重载消除字面量歧义，以及 `operator=(uint64_t)` 赋值运算符
- **验证**: 8 个新测试用例（含 2^63、UINT64_MAX、赋值、算术、回归）全部通过，冒烟测试 11/11 通过
- **Commit**: `b0e79f9`

### [BUG] Relation::ab() 中 b=INT64_MIN 时未定义行为
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Core 模块审计
- **文件**: `include/gnfs/core/relation.hpp:37`
- **描述**: `static_cast<uint64_t>(b > 0 ? b : -b)` 当 b=INT64_MIN 时，`-b` 溢出为 INT64_MIN（UB）
- **影响**: 罕见但对特定 b 值是致命的
- **建议**: 先检查 `b == INT64_MIN` 或用 `uint64_t(~b) + 1`

### [BUG] std::abs(INT64_MIN) 未定义行为（多处）
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sieve + Cofactor + Relation 模块审计
- **文件**: `lattice_sieve.hpp:404`, `cofactorizer.hpp:95`, `collector.hpp:269`
- **描述**: 多处使用 `std::abs(a)` 或 `std::gcd(std::abs(a), b)` 但未检查 a=INT64_MIN 的情况
- **影响**: 当 a=INT64_MIN 时产生 UB（通常返回 INT64_MIN 本身）
- **建议**: 统一封装 `safe_abs()` 函数，检查 INT64_MIN

### [BUG] Hensel Sqrt S[i].to_uint64() 截断：N > 2^64 时 Hensel 失败
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/hensel_sqrt.hpp:113-122`
- **描述**: Hensel 提升中 `uint64_t si = S[i].to_uint64()` 在 S[i] 系数大于 2^64 时静默截断。对 N > 65 位数的因式分解，系数可超过此范围
- **影响**: 65+ 位 N 的 Hensel 提升产生垃圾结果，因式分解失败
- **建议**: 改用 Integer 算术或在 to_uint64() 前检查 fits_uint64()

### [BUG] Couveignes Gray Code `__builtin_ctzll(0)` 未定义行为
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/couveignes.hpp:425-429`
- **描述**: `__builtin_ctzll(changed_bit)` 在 `changed_bit == 0` 时为未定义行为。如果 Gray Code 转换不翻转任何位，会崩溃
- **影响**: 潜在崩溃
- **建议**: 添加 `if (changed_bit == 0) continue;` 守卫

### [BUG] modular_poly p=2 时 Tonelli-Shanks 无限循环
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/modular_poly.hpp:353-362`
- **描述**: 查找非二次剩余的循环 z=2,3,... 在 F_2 中永远找不到非平方，导致无限循环
- **影响**: 任何使用 p=2 的 Tonelli-Shanks 调用会挂起
- **建议**: 对 p=2 做特殊处理（直接返回）

### [BUG] ~~NumberField 假设 f 是 monic 但从不验证~~ ✅
- **状态**: ✅ 已完成
- **发现日期**: 2026-03-08 (Session 5 审计), Session 6 补充
- **解决日期**: 2026-03-09
- **解决方式**: 三处 reduce 函数均已修复：(1) `modular_poly.hpp:reduce()` 用 `mod_inverse(f[d], p)` 缩放; (2) `hensel_sqrt.hpp:poly_mul_mod()` 用 `mpz_invert(f[d], modulus)` 缩放; (3) `number_field.hpp` 新增 `reduce_mod()` 方法，`multiply_mod_n()` 改用此方法。纯 Z 上的 `reduce()` 保留 monic 假设（仅用于测试和不经过 mod 路径的调用）
- **验证**: 7 个新测试（非 monic ModularPoly 3 + NumberField 4）全部通过，冒烟测试 11/11 无回归
- **Commit**: `ec2aa32`

### [BUG] ~~polynomial_optimizer Newton 方法 divmod 覆写导致永不收敛~~ ✅
- **状态**: ✅ 已完成
- **发现日期**: 2026-03-08 (Session 6 审计)
- **解决日期**: 2026-03-09
- **解决方式**: 删除冗余的 lines 61-68，保留 divmod 结果；添加起始点短路检查
- **验证**: test_newton_root 4 个测试用例全部通过，冒烟测试 11/11 通过
- **Commit**: `82bbec1`

### [BUG] ~~polynomial_optimizer newton_root() 验证永远成功~~ ✅
- **状态**: ✅ 已完成
- **发现日期**: 2026-03-08 (Session 6 审计)
- **解决日期**: 2026-03-09
- **解决方式**: 修正 divmod 参数名（quotient/actual_rem），验证失败时返回 nullopt
- **验证**: test_newton_root Case 2 验证 nullopt 返回正确
- **Commit**: `82bbec1`

### [BUG] params.hpp special_q_max 的 uint32 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Core 模块审计
- **文件**: `include/gnfs/core/params.hpp:166`
- **描述**: `p.special_q_max = p.rational_bound * 2` 当 rational_bound > 2^31 时 uint32 溢出。对 100+ 位 N，rational_bound ≈ 1e9，翻倍后溢出
- **影响**: special_q_max 变为极小值，算法失效
- **建议**: 改用 uint64_t 或 `std::min(uint64_t(rb)*2, UINT32_MAX)`

### [BUG] trial_division.hpp uint8_t 指数溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Cofactor 模块审计
- **文件**: `include/gnfs/cofactor/trial_division.hpp:62-64`
- **描述**: 试除循环 `++exp` 使用 uint8_t，如果某素数 p^256 整除 value，exp 溢出回零
- **影响**: 数据损坏：关系中该因子指数记为 0
- **建议**: 改用 uint16_t 或添加溢出检查

### [BUG] Gaussian 消元 pivot_cols.back() 空容器 UB
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `include/gnfs/linalg/gauss.hpp:100-105`
- **描述**: 检测 free columns 时调用 `pivot_cols.back()` 但未检查 `pivot_cols.empty()`
- **影响**: 空矩阵或全零矩阵时 UB
- **建议**: 添加 `if (pivot_cols.empty())` 守卫

### [BUG] SparseMatrix::test() const_cast 违反 const 契约
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `include/gnfs/linalg/sparse_matrix.hpp:62-63`
- **描述**: `const_cast<SparseRow*>(this)->ensure_sorted()` 在 const 方法中修改内部状态，技术上是 UB
- **建议**: 将 `sorted_` 和 `indices_` 标记为 `mutable`

### [BUG] ~~ThreadPool pending_ 计数竞态条件~~ ✅
- **状态**: ✅ 已完成
- **发现日期**: 2026-03-08 (Session 5 审计)
- **解决日期**: 2026-03-09
- **解决方式**: 将 `--pending_` 移入 `std::lock_guard<std::mutex>` 保护区内，`notify_all()` 在锁外触发。添加注释说明 `pending_` 混合同步模型（atomic 用于无锁读，mutex 用于 done_cv_ 同步）
- **验证**: 新增 2 个压力测试（5000 轮 wait_all 竞态 + 1000 轮并发 submit），修复前稳定死锁，修复后 ~600ms 通过。冒烟测试 11/11 通过
- **Commit**: `e6dd3f7`

### [BUG] RelationCollector::relations() 返回非安全引用
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Relation 模块审计
- **文件**: `include/gnfs/relation/collector.hpp:151-152`
- **描述**: 返回 `relations_` 的非 const 引用，调用者可在其他线程添加时修改，导致数据竞争
- **影响**: 多线程场景下 UB
- **建议**: 返回 const 引用或 `std::span<const Relation>`

### [BUG] ~~Hensel/Couveignes 不可约性检查仅测试线性因子——度 > 3 时静默失败~~ ✅
- **状态**: ✅ 已完成
- **发现日期**: 2026-03-08 (Session 5 深度审计)
- **解决日期**: 2026-03-09
- **解决方式**: 实现完整 Rabin 不可约性测试 `ModularPoly::is_irreducible()`，替换 4 处调用点 (hensel_sqrt, couveignes, matrix_builder, schirokauer)
- **验证**: 11 个测试用例通过，含关键的 "reducible without roots" 用例 (x^4+x^2+1 over F_2)，冒烟测试 11/11 通过
- **Commit**: `7516710`

### [BUG] ~~trial_division divide_exact() int64 反向转换溢出~~ ✅
- **状态**: ✅ 已完成
- **发现日期**: 2026-03-08 (Session 5 深度审计)
- **解决日期**: 2026-03-09
- **解决方式**: 改为 `value = Integer(v)` 直接使用 uint64_t 构造（依赖新增的 Integer(uint64_t) 构造函数）
- **验证**: 冒烟测试 11/11 通过，作为 Integer uint64_t 构造函数修复的一部分
- **Commit**: `b0e79f9`

### [BUG] 全局性 uint64_t b → long long/int64_t 截断溢出（13 处）
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 深度审计)
- **来源**: 全代码库逐行审计
- **文件（按模块）**:
  - `polynomial_context.hpp:160` — `b_powers[i] *= static_cast<long long>(b)`
  - `number_field.hpp:230` — `Integer(-static_cast<long long>(b))` in from_ab()
  - `number_field.hpp:383` — `b_powers[i] *= static_cast<long long>(b)` in norm_linear()
  - `trial_division.hpp:128` — `static_cast<int64_t>(b) * static_cast<int64_t>(r)` in divide_algebraic()
  - `trial_division.hpp:238` — `bm *= static_cast<long long>(b)` in compute_rational_value()
  - `trial_division.hpp:264` — `b_powers[i] *= static_cast<long long>(b)` in compute_algebraic_norm()
  - `cofactorizer.hpp:95` — `static_cast<int64_t>(b)` in verify()
  - `collector.hpp:269` — `static_cast<int64_t>(rel.b)` in validate()
  - `lattice_sieve.hpp:268-269` — intermediate overflow in sieve
  - `class_group.hpp:383` — `static_cast<int64_t>(b) * static_cast<int64_t>(pi.r)` in factor_ideal()
  - `class_group.hpp:418` — same in factor_principal_ideal()
  - `lattice_basis.hpp:44` — `static_cast<int64_t>(b) * r` in verify_ab()
  - `couveignes.hpp:305` — `Integer(static_cast<int64_t>(b))` in rat_product calculation
- **描述**: `b` 是 uint64_t，当 b > INT64_MAX (≈ 9.2e18) 时，cast 到 long long/int64_t 产生 UB。当前测试 N ≤ 25 位时 b 很小不触发，但大 N 筛选时 b 可能很大
- **影响**: 大 N 因式分解时 13 个位置产生静默数据损坏
- **建议**: 统一使用 `Integer(b)` 或添加 `uint64_t` 版本的乘法运算符

### [BUG] modular_poly sub()/mod_inverse() 对 p > INT64_MAX 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 深度审计)
- **来源**: Sqrt 模块逐行审计
- **文件**: `include/gnfs/sqrt/modular_poly.hpp:90,420`
- **描述**: `sub()` 将 uint64 系数 cast 到 int64 再相减，p > INT64_MAX 时溢出。`mod_inverse()` 将 p cast 到 int64，同样溢出。影响 Tonelli-Shanks、GCD、除法等所有依赖 ModularPoly 的操作
- **影响**: 使用大素数（>2^63）的 Couveignes/Hensel 崩溃或产生错误
- **建议**: 使用 unsigned 算术或 `__int128_t`

### [BUG] Couveignes compute_from_element() 无上限搜索循环
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 深度审计)
- **来源**: Sqrt 模块逐行审计
- **文件**: `include/gnfs/sqrt/couveignes.hpp:483`
- **描述**: `while (primes.size() < config_.num_primes)` 没有 attempts 上限。如果几乎所有素数都使 f 可约（或导致零乘积），此循环永不终止
- **影响**: 某些多项式可能导致进程永久挂起
- **建议**: 添加 `&& attempts < 100000` 条件（与 compute() 的 primes_checked < 100000 一致）

### [BUG] Relation::b 是 int64_t 但应为 uint64_t — 全局类型不匹配
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: 全代码库类型追溯
- **文件**: `include/gnfs/core/relation.hpp:17`
- **描述**: `Relation::b` 声明为 `int64_t`，但 GNFS 中 b > 0 始终为正。ABPair::b 是 uint64_t，sieve 输出 uint64_t b，algebraic_norm/Schirokauer/ClassGroup 等全部期望 uint64_t。当 sieve 产生 b > INT64_MAX 时：(1) `Relation(a, b)` 构造时 uint64_t→int64_t 溢出为负数 (2) `ab()` 计算 `-b` 得到错误值 (3) 传递给期望 uint64_t 的函数时再次回转但值已错
- **影响**: 大 N 筛选（b > 2^63）时全部关系数据被静默损坏
- **建议**: 将 Relation::b 改为 uint64_t，构造函数参数对应修改

### [BUG] rational_sqrt 验证函数声称验证但实际什么也不做
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: Sqrt 模块逐行审计
- **文件**: `include/gnfs/sqrt/rational_sqrt.hpp:143-162`
- **描述**: 当 `config_.verify = true`（默认），代码进入验证分支但只有一个空的 for 循环和 `// TODO: 完整验证` 注释。函数总是返回 `success = true`。这是一个欺骗性 API：调用者以为启用了验证，但实际从未验证
- **影响**: 错误的有理平方根不会被检测到，直到最终 GCD 步骤才发现因式分解失败
- **建议**: 实现完整验证：计算 sqrt²  mod N 并与原始乘积比较

### [BUG] MatrixBuilderConfig 默认 schirokauer_primes = {2, 3}，ℓ=3 不兼容 GF(2)
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: LinAlg 模块逐行审计
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:113`
- **描述**: 默认配置 `schirokauer_primes = {2, 3}`。对于 ℓ=3，Schirokauer map 值在 {0, 1, 2} 中，但矩阵是 GF(2)。代码在 line 305 取 `sm_values[j] % 2`，将 mod-3 值截断为 mod-2。这数学上不正确：ℓ=3 的 Schirokauer 约束是 Σλ ≡ 0 mod 3，取 mod 2 后变成了不同的约束
- **影响**: 矩阵零空间可能遗漏正确依赖或包含虚假依赖，导致 sqrt 阶段失败概率增加
- **建议**: GF(2) 矩阵只能使用 ℓ=2（CLAUDE.md 已明确记录此约定）。默认值应改为 {2}

### [BUG] Couveignes rat_sqrt 对合数 N 计算根本性错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 深度审计)
- **来源**: Sqrt 模块逐行审计
- **文件**: `include/gnfs/sqrt/couveignes.hpp:311-329`
- **描述**: `powmod(rat_product, (N+1)/4, N)` 假设 N ≡ 3 mod 4 且 N 是素数。但 N 是合数（正在被因式分解的数），(N+1)/4 次方不是合法的平方根公式。实际上，如果我们已经知道 rat_product 的平方根，就不需要因式分解了。正确做法应该是通过有理侧因式分解直接累积平方根，而不是对乘积取"半次方"
- **影响**: 对合数 N，这个"平方根"几乎总是错误的，但由于后续用 GCD 验证，它只影响 Couveignes 找到正确符号模式的成功率（严重降低）
- **建议**: 有理平方根应该在 rational_sqrt.hpp 中独立计算，通过因子指数的半次方累积

### [BUG] ECM sieve_primes(B2) 内存爆炸
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 深度审计)
- **来源**: Cofactor 模块逐行审计
- **文件**: `include/gnfs/cofactor/ecm.hpp:405`
- **描述**: `sieve_primes(B2)` 分配 B2 个 bool 做素数筛。B2=5e9 时分配 ~5GB 内存，B2=1e10 时 ~10GB
- **影响**: 大 B2 参数导致 OOM 或系统卡顿
- **建议**: 分段筛法（segmented sieve）或直接用 Miller-Rabin 测试范围内的素数

### [BUG] smooth_check large_prime_bound² uint64 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 深度审计)
- **来源**: Cofactor 模块逐行审计
- **文件**: `include/gnfs/cofactor/smooth_check.hpp:195`
- **描述**: `classify_cofactor()` 中 `large_prime_bound * large_prime_bound` 当 lpb > 2^32 时溢出 uint64。对大 N，large_prime_bound 可达 10^10+
- **影响**: 余因子分类阈值错误，可能接受非光滑关系或拒绝有效关系
- **建议**: 使用 `__uint128_t` 或 `Integer` 做平方比较

### [BUG] class_group SNF 实现不是真正的 Smith Normal Form
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 深度审计)
- **来源**: Sqrt 模块逐行审计
- **文件**: `include/gnfs/sqrt/class_group.hpp:450-517`
- **描述**: `compute_smith_normal_form()` 实际只做了整数 Gaussian 消元，不是 SNF。类数用 `1u << generators.size()` 计算是粗略近似（假设每个 generator 的 order 都是 2），实际类数可能差几个数量级
- **影响**: 类群结构完全错误，但目前类群只用于 Couveignes 的辅助检查，不影响主路径
- **建议**: 实现真正的 SNF（需要 elementary divisors），或标记为 "approximate"

### [BUG] class_group factor_ideal/factor_principal_ideal int64 乘法溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 深度审计)
- **来源**: Sqrt 模块逐行审计
- **文件**: `include/gnfs/sqrt/class_group.hpp:383, 418`
- **描述**: `static_cast<int64_t>(b) * static_cast<int64_t>(pi.r)` — 当 b 和 r 都较大时（各 ~10^9），乘积可达 ~10^18，接近 INT64_MAX。更大的值会溢出
- **影响**: 大参数时理想分解错误
- **建议**: 使用 `__int128_t` 或 `Integer`

### [BUG] SparseMatrix multiply_blocks() 索引错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 深度审计)
- **来源**: LinAlg 模块逐行审计
- **文件**: `include/gnfs/linalg/sparse_matrix.hpp`
- **描述**: `multiply_blocks()` 中 `x[block_idx * rows_.size() + i]` 假设 x 按 (block, row) 布局，但调用者可能传入按 (row, block) 布局的数据。接口缺少明确的布局文档
- **影响**: Block Lanczos 使用此函数时，如果布局不匹配会产生错误的 SpMV 结果
- **建议**: 明确文档布局约定，或提供两种布局的重载

### [BUG] Kleinjung is_valid_polynomial() 浮点验证无意义
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 深度审计)
- **来源**: Polynomial 模块逐行审计
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp:505-512`
- **描述**: `remainder.to_double()` 和 `n.to_double()` 对 N > 2^53 都损失精度。`rel_error > 1e-10` 的检查对大 N 完全无效——两个大数的 double 表示可能相等即使实际值差很远
- **影响**: 无效多项式可能通过验证，或有效多项式被错误拒绝
- **建议**: 直接用 Integer 精确验证 `f(m) == N`

### [BUG] base_m_expansion 非零余数处理
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 深度审计)
- **来源**: Polynomial 模块逐行审计
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp:476-478`
- **描述**: `base_m_expansion()` 如果展开后有非零余数，将其加到 `coeffs[d-1]`。这使得该系数可能远大于 m，违反了 GNFS 对小系数的要求。正确做法是 m 值需要调整使得 N 能被精确表示
- **影响**: 产生低质量多项式，降低筛选效率

### [BUG] Schirokauer factorize_and_setup 重复根处理
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 深度审计)
- **来源**: LinAlg 模块逐行审计
- **文件**: `include/gnfs/linalg/schirokauer.hpp`
- **描述**: 当 f mod ℓ 有重复根时（即 ℓ | disc(f)），`factorize_and_setup` 不区分重复根和单根。对重复根的 Schirokauer map 计算需要特殊的 valuation lifting，当前代码简单跳过或错误处理
- **影响**: ℓ | disc(f) 时 Schirokauer 列值错误，矩阵零空间不正确
- **建议**: 检测 disc(f) mod ℓ == 0 并避开这些 ℓ，或实现正确的 ramified case

---

## P1 — 高优先级（影响正确性或大数支持）

### [BUG] Split Schirokauer: f mod 2 可约时映射计算错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-02-20 (Session 2)
- **来源**: Schirokauer maps 测试中发现
- **文件**: `include/gnfs/linalg/schirokauer.hpp`
- **描述**: 当多项式 f mod 2 可约时，需要先做 valuation stripping 再计算 Schirokauer map。当前部分实现存在但有 bug（非单位元素处理不正确）。审计还发现 split case root lifting (line 251) 使用了错误的根值
- **影响**: 部分 N 值（f mod 2 可约的）无法正确因式分解
- **当前规避**: 选择 f mod 2 不可约的测试 N
- **优先级**: P1

### [BUG] Couveignes 符号归一化不一致导致 CRT 重建错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/couveignes.hpp:510-524`
- **描述**: `needs_negate = (eval_at_1 > p/2)` 对不同素数使用不同阈值 p/2。不同素数的符号模式不一致，CRT 重建产生错误结果。此外，有理平方根指数 (N+1)/4 未检查 N ≡ 3 mod 4 前提
- **影响**: ~50% 的有效符号模式被错误丢弃，大幅降低 Couveignes 成功率

### [BUG] algebraic_sqrt compute_heuristic() 数学不正确
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/algebraic_sqrt.hpp:173-195`
- **描述**: 使用 `elem^((n+1)/2)` 计算平方根，但这在 Q[α]/f(α) 中不成立。此 fallback 方法对任何非平凡情况都会失败
- **影响**: Hensel 失败时的 fallback 路径不可用

### [BUG] matrix_builder f mod 2 检查在大系数时 uint64 截断
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:174-184`
- **描述**: 检查 f mod 2 是否不可约时，先调用 `c.to_uint64()` 再取 mod 2。但大系数在 to_uint64() 时已截断。应先做 `c %= 2` 再转换
- **影响**: 错误判断 f mod 2 的可约性，导致 Schirokauer 或 QC 列数错误

### [BUG] matrix_builder 多项式度 > 8 时数组越界
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:416`
- **描述**: `FastPoly::MAX_DEGREE = 8`，但 `select_qc_primes()` 未验证 `ctx.degree() <= 8`。度 > 8 时数组越界
- **影响**: 高阶 GNFS（度 9+）崩溃
- **建议**: 添加 degree 边界检查

### [BUG] matrix_builder QC 系数 int64 截断
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:387-388`
- **描述**: `ctx.coeff(i).to_int64()` 对大 N 的多项式系数可能超出 int64 范围，静默截断导致 QC 素数选择使用错误多项式
- **影响**: QC 列不正确，矩阵零空间求解失败

### [BUG] Schirokauer FastPoly 系数潜在溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `include/gnfs/linalg/schirokauer.hpp:73, 87-91`
- **描述**: `reduce_inplace()` 中 `a.coeffs[idx]` 可能 > m（违反 GF(m) 语义）。当 t > a.coeffs[idx] 时公式 `m - (t - a.coeffs[idx])` 会给出错误结果
- **影响**: Schirokauer map 值不正确

### [BUG] lattice_sieve 模运算中间值 int64 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sieve 模块审计
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:268-269`
- **描述**: `u = (basis.e0 - f0*m) % p` 中 `f0*m` 可溢出 int64。对大格基值产生错误结果
- **建议**: 使用 `__int128_t` 或先做模约化再乘

### [BUG] lattice_basis 行列式计算 int64 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sieve 模块审计
- **文件**: `include/gnfs/sieve/lattice_basis.hpp:52`
- **描述**: `e0 * f1 - e1 * f0` 对大格基值溢出 int64，无边界检查
- **影响**: 大 special-Q 时格行列式错误

### [BUG] cofactorizer 大素数存储 uint32 截断
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Cofactor 模块审计
- **文件**: `include/gnfs/cofactor/cofactorizer.hpp:160, 170`
- **描述**: 余因子 > UINT32_MAX（50+ 位素数）时，PrimePower 中存储为 0。50+ 位 N 需要更大的大素数
- **建议**: PrimePower 改用 uint64_t 或 Integer

### [BUG] ECM 固定随机种子导致重复曲线
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Cofactor 模块审计
- **文件**: `include/gnfs/cofactor/ecm.hpp:49`
- **描述**: `std::mt19937_64 rng(42)` 硬编码种子。每次 ECM 运行产生相同 sigma 序列，多曲线策略失效，显著降低找到因子的概率
- **建议**: 使用非确定性种子或曲线索引 + 哈希

### [BUG] modular_poly q_minus_2 uint64 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/modular_poly.hpp:529-531`
- **描述**: 计算 `q_minus_2 = mod^d` 时 uint64 溢出（mod>2, d>20）
- **影响**: Tonelli-Shanks 平方根失败

### [BUG] class_group 判别式计算仅对 d=3 正确
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/class_group.hpp:165-209`
- **描述**: 判别式计算仅对 d=3 正确；d>3 时结果完全错误
- **影响**: 类群计算对高阶多项式不正确

### [BUG] filter.hpp 合并关系硬编码 1000 上限
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Relation 模块审计
- **文件**: `include/gnfs/relation/filter.hpp:262`
- **描述**: `merged.size() < 1000` 硬编码上限，大因子基时丢弃有效合并
- **建议**: 改为可配置参数

### [BUG] Logger 递归日志死锁
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Util 模块审计
- **文件**: `include/gnfs/util/logger.hpp:140-141`
- **描述**: 如果日志回调或异常处理器中再次调用 log()，会导致 `lock_guard` 死锁
- **建议**: 改用 `recursive_mutex` 或线程局部嵌套计数器

### [BUG] matrix_builder 存储 config 的 schirokauer_primes 而非实际使用的素数
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: LinAlg 模块逐行审计
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:267`
- **描述**: `result.mapping.schirokauer_primes = config_.schirokauer_primes` 存储的是配置中的素数列表，但实际用于计算的是经过过滤的 `sm_primes`（移除了 f 可约的素数）。如果后续代码读取 mapping.schirokauer_primes 进行验证，会得到不一致的信息
- **影响**: 矩阵元信息与实际矩阵结构不匹配
- **建议**: 改为 `result.mapping.schirokauer_primes = sm_primes`

### [BUG] types.hpp ABPair 注释错误：写 "a + b*m" 但应为 "a - b*m"
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: Core 模块逐行审计
- **文件**: `include/gnfs/core/types.hpp:11`
- **描述**: 注释写 `a + b*m 在有理侧，a + b*alpha 在代数侧`，但 GNFS 约定是 `a - b*m` 和 `a - b*α`。这个注释会误导开发者
- **影响**: 文档级问题，可能导致新代码写反符号
- **建议**: 修正注释为 `a - b*m 在有理侧, a - b*α 在代数侧`

### [BUG] FactorBaseParams::large_prime_bound 是 uint32_t 但 GNFSParams 用 uint64_t
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: Core + FactorBase 模块类型对比
- **文件**: `include/gnfs/core/types.hpp:128` vs `include/gnfs/core/params.hpp:30`
- **描述**: `FactorBaseParams::large_prime_bound` 是 `uint32_t`，但 `GNFSParams::large_prime_bound` 是 `uint64_t`。从 GNFSParams 复制到 FactorBaseParams 时静默截断。对 50+ 位 N，lpb > 4×10^9 > UINT32_MAX
- **影响**: 大 N 的大素数界被截断，大量有效 1LP/2LP 关系被拒绝
- **建议**: FactorBaseParams::large_prime_bound 改为 uint64_t

### [BUG] rational_sqrt 负号检测到但未应用
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: Sqrt 模块逐行审计
- **文件**: `include/gnfs/sqrt/rational_sqrt.hpp:138-141`
- **描述**: `has_negative` 跟踪 (a-bm) 负值的奇偶性。当 `has_negative = true` 时（有奇数个负值），代码注释说"不应该发生"但什么也不做。正确做法应该对平方根取反（`sqrt_value = n - sqrt_value`）
- **影响**: 当符号列缺失或依赖未完全消除负号时，有理平方根有 50% 概率符号错误。虽然 extract_factors 会同时检查 X±Y，但浪费一次 GCD 机会
- **建议**: 当 has_negative 时，应用 `sqrt_value = n - sqrt_value`

### [BUG] Schirokauer Hensel 提升 quadratic factor 访问未构建的 prime_info_
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: LinAlg 模块逐行审计
- **文件**: `include/gnfs/linalg/schirokauer.hpp:528`
- **描述**: `hensel_lift_factor()` 中 `for (const auto& other_fi : prime_info_.back().factors)` 在 precompute 阶段被调用，此时当前 PrimeInfo 尚未 push 到 prime_info_。`prime_info_.back()` 引用的是上一个 prime 的信息，而不是当前的。对 ℓ=2 且 f mod 2 有 degree-2 cofactor 的情况，代码虽然 fall-through 到 "skip" 而不崩溃，但 quadratic factor 未被正确 Hensel 提升
- **影响**: Split Schirokauer 的精度降低
- **建议**: 从 `info.factors`（已部分构建）中查找 sibling linear factor

### [BUG] Relation 反序列化无输入验证——可 OOM 或产生损坏数据
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: Core 模块逐行审计
- **文件**: `include/gnfs/core/relation.hpp:104-148`
- **描述**: `deserialize()` 从流中读取 rat_count/alg_count/lp_count 后直接 `resize()` 分配内存，无上限检查。(1) 恶意/损坏文件中 count = 4294967295 会尝试分配 ~16GB 内存 (2) 每次 read() 后不检查 `is.good()`，流耗尽时 vector 中充满未初始化数据 (3) 无 magic number/版本号，任何文件都能"成功"反序列化
- **影响**: 加载损坏关系文件时 OOM 或静默数据损坏
- **建议**: 添加 count 上限检查（如 < 1M）、读后检查 `is.good()`、添加文件头

### [BUG] matrix_builder exponent 累积用 uint8_t — 大 FB 关系可溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: LinAlg 模块逐行审计
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:534, 548`
- **描述**: `std::unordered_map<uint32_t, uint8_t> exponents` 用于累积每个因子基索引的出现次数。如果一个关系中同一素数出现 256+ 次（高次幂因子），uint8_t 溢出为 0，导致该列不被设置
- **影响**: 极端情况（小素数高次幂）下矩阵行数据丢失
- **建议**: 改用 uint32_t 或检测溢出

### [BUG] smooth_check quick_cofactor_check 也有 lpb² 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: Cofactor 模块逐行审计
- **文件**: `include/gnfs/cofactor/smooth_check.hpp:319`
- **描述**: `c <= large_prime_bound * large_prime_bound` 与 line 195 相同的 uint64 溢出问题
- **影响**: 快速筛选函数误判余因子状态
- **建议**: 同 line 195 的修复方案

### [BUG] Pollard rho 只使用单一多项式 x²+1
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: Cofactor 模块逐行审计
- **文件**: `include/gnfs/cofactor/smooth_check.hpp:148`
- **描述**: Pollard's rho 使用固定的 `f(x) = x² + 1 mod n`。对某些 n 值（如 n = p² 或某些特定合数），这个多项式可能进入短循环永远找不到因子。标准做法是失败后尝试 f(x) = x² + c 对不同 c
- **影响**: 部分可分解的余因子被错误分类为 Composite/Unknown
- **建议**: 增加循环在 max_iterations 后用不同 c 重试

### [BUG] Integer operator+=/operator-=/operator/=/operator%= 对 INT64_MIN 参数 UB
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: Core 模块逐行审计
- **文件**: `src/core/integer.cpp:356-357, 374-375, 384`
- **描述**: 当 value == INT64_MIN 时，`-value` 是有符号整数溢出（UB）。operator+=(-INT64_MIN)、operator-=(-INT64_MIN)、operator/=(INT64_MIN)、operator%=(INT64_MIN) 全部触发。虽然二进制补码机器上碰巧产生正确结果，但这是标准不保证的行为
- **影响**: 理论上 UB，实践中在 x86/ARM 上碰巧正确
- **建议**: 使用 `static_cast<unsigned long>(value) * (value < 0 ? -1 : 1)` 的无 UB 等价形式，或特殊处理 INT64_MIN

### [BUG] SieveRegion default_sieve_region 大 skewness 时 int32 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: Sieve 模块逐行审计
- **文件**: `include/gnfs/sieve/lattice_basis.hpp:175-178`
- **描述**: `base_size * factor` 其中 base_size = 16384，factor = sqrt(skewness)。当 skewness > 1.7e10 时，factor > 130K，乘积 > 2.1×10^9 > INT32_MAX。`static_cast<int32_t>()` 产生未定义行为
- **影响**: 大 skewness 的多项式（50+ 位 N）导致筛区域参数垃圾化
- **建议**: 添加 clamp：`std::min(base_size * factor, static_cast<double>(INT32_MAX))`

### [BUG] MurphyEvaluator n.to_double() 对 N > 10^308 返回 infinity
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: Polynomial 模块逐行审计
- **文件**: `include/gnfs/polynomial/murphy_evaluator.hpp:331`
- **描述**: `std::sqrt(n.to_double()) / skewness` — 对 N > ~1024 位，`n.to_double()` 返回 `+inf`。`sqrt(inf)/skewness = inf`。`std::uniform_real_distribution(1.0, inf)` 产生 NaN
- **影响**: 大 N 的 Murphy 评分全部为 NaN，多项式选择失败
- **建议**: 使用对数空间采样或限制 B_range 上界

### [BUG] MurphyEvaluator alpha 计算跳过不 fit uint64 的首项系数
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: Polynomial 模块逐行审计
- **文件**: `include/gnfs/polynomial/murphy_evaluator.hpp:143-147`
- **描述**: `if (f.leading_coeff().fits_uint64())` — 如果首项系数大于 2^64（大 N 的高阶多项式），直接跳过投影根贡献。对这些素数，alpha 值被低估
- **影响**: 大 N 多项式的 alpha 估计不准确
- **建议**: 使用 `Integer::mod(coeff, p_int)` 替代 uint64 取模

### [BUG] Hensel poly_inverse_mod_direct p^d uint64 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/hensel_sqrt.hpp:529-531`
- **描述**: `uint64_t q_minus_2 = 1; for (i..d) q_minus_2 *= mod; q_minus_2 -= 2;` — 计算 p^d - 2 时使用 uint64_t。对 d=6, p=2000：p^6 ≈ 6.4×10^19 > UINT64_MAX。与 line 125 正确使用 Integer 的同类计算形成对比
- **影响**: degree-6 多项式 + 中等大小素数时 Hensel 失败
- **建议**: 改用 Integer 算术（参考 line 125-127 的模式）

### [BUG] Kleinjung construct_polynomial 死代码
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 审计)
- **来源**: Polynomial 模块审计
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp:380-439`
- **描述**: Lines 380-435 费力计算系数（包括区间调整、余数重算），然后 Line 439 `coeffs = base_m_expansion(...)` 完全覆写所有结果。60 行代码全部是死代码
- **影响**: 代码膨胀，维护困难，可能误导读者
- **建议**: 删除 lines 380-435 的死代码

### [BUG] Kleinjung base_m_expansion 系数不平衡
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 审计)
- **来源**: Polynomial 模块审计
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp:466-478`
- **描述**: Base-m 展开使用截断除法，产生 [0, m) 范围的系数。GNFS 最佳实践要求平衡系数 [-m/2, m/2]。不平衡系数导致多项式值更大、筛选产率更低
- **影响**: 多项式质量劣化，可能需要更多筛选时间
- **建议**: 在展开后添加平衡步骤：`if (coeffs[i] > m/2) { coeffs[i] -= m; coeffs[i+1] += 1; }`

### [BUG] IntPolynomial add_mod 对大 p 的 uint64 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 审计)
- **来源**: Polynomial 模块审计
- **文件**: `include/gnfs/polynomial/int_polynomial.hpp:362`
- **描述**: `(a + b) % p` — 当 a, b < p 且 p > UINT64_MAX/2 ≈ 9.2×10^18 时，a + b 溢出 uint64_t。实际使用中 p 是 uint32_t 素数所以安全，但作为通用工具函数存在隐患
- **影响**: 大模数下结果错误
- **建议**: 改为 `a >= p - b ? a - (p - b) : a + b` 或使用 __uint128_t

### [BUG] IntPolynomial::roots_cantor_zassenhaus 实际是 O(p) 暴力搜索
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 审计)
- **来源**: Polynomial 模块审计
- **文件**: `include/gnfs/polynomial/int_polynomial.hpp:384-405`
- **描述**: 函数名叫 `roots_cantor_zassenhaus` 但实现是暴力枚举 O(p)。Line 143 注释说"大 p：使用 Cantor-Zassenhaus 算法"也是假的。真正的 Cantor-Zassenhaus 在 `builder.cpp` 中
- **影响**: 如果 IntPolynomial 的 roots_mod_p 被大 p 调用，性能极差
- **建议**: 要么实现真正的 CZ，要么重命名为 `roots_brute_force`

### [BUG] class_group 判别式公式对非 depressed 三次多项式错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/class_group.hpp:168-188`
- **描述**: 使用 Δ = -4a³ - 27b² 公式，但这只适用于 depressed cubic f(x) = x³ + ax + b（x² 系数为 0）。GNFS 多项式通常有非零 x² 系数 c₂，正确公式应包含 c₂ 项。Line 170-171 取 `a = ctx_.coeff(1), b = ctx_.coeff(0)` 完全忽略了 `ctx_.coeff(2)`
- **影响**: 所有非 depressed 三次多项式的类群计算使用错误判别式
- **建议**: 使用完整三次判别式公式，或先做 Tschirnhaus 变换消除 x² 项

### [BUG] ~~polynomial_optimizer divmod 参数命名与语义颠倒~~ ✅
- **状态**: ✅ 已完成
- **发现日期**: 2026-03-08 (Session 6 审计)
- **解决日期**: 2026-03-09
- **解决方式**: 变量名改为 `quotient` 和 `actual_rem`，语义与 divmod 签名一致
- **Commit**: `82bbec1`

---

## P1-OPT — 高优先级性能优化

### [OPT] Block Lanczos 是 25-digit 的主要瓶颈（需并行化）
- **状态**: 🔴 待处理
- **发现日期**: 2026-02-22 (Session 3), 2026-03-08 审计补充
- **来源**: 25-digit 性能分析（193s = 81.6%）
- **文件**: `src/linalg/block_lanczos.cpp`
- **描述**: SpMV 操作（lines 181-201）是顺序执行的 O(w)。审计还发现：(1) AND 应为 OR 的阈值判断 (line 284) (2) 硬编码种子 42 (line 310) (3) M^T * V 每次迭代重复计算 (line 343)
- **影响**: 30-digit+ 因式分解的主要瓶颈
- **建议**: OpenMP 并行 SpMV，预计 4-8× 加速

### [OPT] ECM Stage 2 BSGS 优化
- **状态**: 🔴 待处理
- **发现日期**: 2026-02-20 (Session 2)
- **来源**: 性能分析 + 2026-03-08 审计确认
- **文件**: `include/gnfs/cofactor/ecm.hpp:410-430`
- **描述**: 当前 Stage 2 朴素实现 O(π(B2))。审计还发现每次 ECM 调用都重新做素数筛选（Eratosthenes），应缓存素数列表
- **影响**: 60 位余因子的 Stage 2 比 BSGS 慢 5-10×

### [OPT] lattice_basis 浮点高斯约化不够精确
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sieve 模块审计
- **文件**: `include/gnfs/sieve/lattice_basis.hpp:83-109`
- **描述**: 使用 double 精度做高斯约化，当格基系数 |e0|, |f0| ~ 10^18 时绝对误差 ~ 10^3，不可接受。应使用纯整数 GCD 或 LLL 约化
- **影响**: 大 special-Q 时格基质量差，筛选效率降低

### [OPT] Gauss 消元 history 矩阵 O(n^2) 空间
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `include/gnfs/linalg/gauss.hpp`
- **描述**: 每行一个 num_rows 位的 BitVector。500K 行 = 31GB，不可用于大矩阵
- **影响**: 阻止 Gaussian fallback 用于大矩阵
- **建议**: 仅存储参与行的索引，或使用压缩表示

---

## P2 — 中优先级（50+ 位数支持和架构改进）

### [FEAT] Bucket Sieve 架构
- **状态**: 🔴 待处理
- **发现日期**: 2026-02-20 (Session 2)
- **来源**: 大因子基性能需求
- **描述**: 当前 sieve 对大因子基不够 cache-friendly。需要重新设计为 bucket sieve 架构
- **影响**: 大 N（50-digit+）的 sieve 阶段性能瓶颈

### [FEAT] NEON SIMD 加速
- **状态**: 🔴 待处理
- **发现日期**: 2026-02-20 (Session 2)
- **来源**: 性能优化规划
- **描述**: 利用 ARM NEON 指令加速关键路径：`vqsubq_u16` 用于 sieve，`vminvq_u16` 用于候选扫描，`veorq_u64` 用于 GF(2) XOR
- **影响**: 预计 sieve 和 linalg 各有 2-4× 提升

### [OPT] Kleinjung 多项式选择质量提升
- **状态**: 🔴 待处理
- **发现日期**: 2026-02-20 (Session 2), 2026-03-08 审计补充
- **来源**: Polynomial 模块审计
- **文件**: `include/gnfs/polynomial/kleinjung_selector.hpp`
- **描述**: 审计新发现：(1) search_radius=100 硬编码，大 N 需 >1000 (line 238) (2) 系数边界 O(m) 太松，应为 O(m^{1/d}) (line 275) (3) 不检查 f 在 Q 上不可约性。原有问题：缺少 lattice-based search 和 coefficient rotation

### [FEAT] Factor Base 支持 ramified/projective 素数
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: FactorBase 模块审计
- **文件**: `src/factor_base/builder.cpp`
- **描述**: AlgebraicPrime 有 degree 字段和 PROJECTIVE_ROOT 常量，但 builder 始终设 degree=1，从不使用 projective root。对 p | disc(f) 的分歧素数和无穷远素数缺失
- **影响**: 大约丢失 ~1% 代数侧素数，大 N 时关系收集效率降低

### [FEAT] Out-of-core Relations 支持
- **状态**: 🔴 待处理
- **发现日期**: 2026-02-20 (Session 2), 2026-03-08 审计强化为 P2
- **来源**: Relation 模块审计
- **文件**: `include/gnfs/relation/collector.hpp`
- **描述**: 所有关系存储在内存中。50+ 位 N 需要 10-100M 关系（数 GB）。审计还发现：(1) 无压缩 (2) 去重哈希表无界增长 (3) 无批量 I/O
- **影响**: 50+ 位 N 时 OOM

### [FEAT] Block Lanczos Out-of-core 矩阵支持
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **描述**: 矩阵必须完全在 RAM 中。50+ 位 GNFS 的 500K 关系矩阵可能需要数十 GB
- **建议**: 流式 SpMV + 磁盘矩阵存储

### [DEBT] params.hpp 对 100+ 位 N 参数不足
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Core 模块审计
- **文件**: `include/gnfs/core/params.hpp`
- **描述**: (1) degree 上限硬编码为 6，100+ 位应为 7-8 (2) rational_bound 上限 1e9，150+ 位应为 1e10+ (3) log_scale 最大 16，应随因子基增长 (4) 小 N (n_bits<2) 时 ln_ln_n 产生 NaN
- **建议**: 动态度缩放，移除硬编码上限

### [FEAT] Relation Filter 完成 clique-based 合并
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Relation 模块审计
- **文件**: `include/gnfs/relation/filter.hpp:286-287`
- **描述**: `PartialRelationMerger::merge()` 有框架但从未真正实现因子指数的异或/合并。50+ 位 N 依赖部分关系的图合并
- **建议**: 实现完整的基于图的 clique 解析

### [FEAT] ThreadPool Work-Stealing
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Util 模块审计
- **文件**: `include/gnfs/util/thread_pool.hpp`
- **描述**: 当前固定分块分配。筛选的 special-Q 开销不均匀，导致负载不平衡
- **建议**: 实现工作窃取队列

### [DEBT] Relation 序列化格式缺陷
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Core 模块审计
- **文件**: `include/gnfs/core/relation.hpp:73-144`
- **描述**: (1) 无版本字段，格式不可升级 (2) 无校验和/CRC (3) 依赖字节序 (4) 反序列化不检查流状态
- **影响**: 截断文件产生静默损坏的关系

### [BUG] smooth_check 浮点精度损失
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Cofactor 模块审计
- **文件**: `include/gnfs/cofactor/smooth_check.hpp:107-108`
- **描述**: `std::pow(double(n), 1.0/k)` 对大 n 有 ~10^-15 相对误差，round() 可能取错值。is_perfect_square 中 `candidate * candidate == n` 可溢出
- **建议**: 使用 GMP 的 mpz_root / mpz_perfect_square_p

### [OPT] Murphy E-score 低估 20-40%
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Polynomial 模块审计
- **文件**: `include/gnfs/polynomial/murphy_evaluator.hpp`
- **描述**: (1) 只用 B 以下素数，忽略 B-10B 余因子贡献 (2) Dickman rho 近似在 u>100 时误差 >5% (3) 采样边界对大 skewness 可能为负
- **影响**: 多项式排名不准确

### [DEBT] PrimePowerHash 忽略指数字段
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Core 模块审计
- **文件**: `include/gnfs/core/types.hpp:87-92`
- **描述**: 哈希仅组合 (p, r) 忽略 e，不同指数的 PrimePower 哈希相同
- **影响**: 如果用于 unordered_set 去重会出错

---

## P3 — 低优先级（代码质量和长期改进）

### [DEBT] Integer(uint64_t) 构造函数缺失（重复项，主修在 P0）
- **状态**: 🔴 待处理
- **发现日期**: 2026-02-20 (Session 2)
- **描述**: 见 P0 同名条目。此处保留历史记录

### [DEBT] 根目录遗留文件清理
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 4)
- **描述**: 根目录有 ~70 个遗留文件（.sh 脚本、过时 .md、扁平命名文件）

### [DEBT] SmallVector 缺少边界检查
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Util 模块审计
- **文件**: `include/gnfs/util/small_vector.hpp:96-103`
- **描述**: `operator[]` 和 `back()` 无边界检查，空容器 back() 为 UB。ensure_capacity() 无溢出检查
- **建议**: Debug 模式添加 assert

### [DEBT] FactorBase 缺少序列化
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: FactorBase 模块审计
- **文件**: `include/gnfs/factor_base/factor_base.hpp:137-141`
- **描述**: 标记为 TODO。大 FB (100K+) 重建耗时，应支持保存/加载

### [DEBT] Block Lanczos 阈值判断逻辑错误 (AND vs OR)
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `src/linalg/block_lanczos.cpp:284`
- **描述**: `if (rows < 10000 && cols < 10000)` 应为 OR。10001×100 的矩阵不应该用 Block Lanczos
- **建议**: 改为 `||`

### [DEBT] Schirokauer 文档注释与代码不一致
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: LinAlg 模块审计
- **文件**: `include/gnfs/linalg/schirokauer.hpp:138`
- **描述**: 注释写的指数公式 ℓ^(k-1)·(ℓ-1) 与代码实际使用的 ℓ^d-1 不一致。代码是对的，注释是错的

### [DEBT] polynomial_context coeff() 返回可变静态引用
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Core 模块审计
- **文件**: `include/gnfs/core/polynomial_context.hpp:76-80`
- **描述**: 越界访问返回同一个静态 zero Integer 引用，多次调用返回同一引用可能产生别名 bug

### [DEBT] SpecialQ from_indices 忽略 end_index 参数
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sieve 模块审计
- **文件**: `include/gnfs/sieve/special_q.hpp:36`
- **描述**: `from_indices(uint32_t start, uint32_t /* end_index */)` 中 end_index 被注释掉但仍在签名中

### [DEBT] number_field evaluate 无溢出保护
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: Sqrt 模块审计
- **文件**: `include/gnfs/sqrt/number_field.hpp:328-343`
- **描述**: `m_power *= m` 循环对大 m 和 d>5 溢出，无边界检查

---

## TEST — 测试覆盖率缺口

### [TEST] 7 个模块无专属单元测试
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: 测试覆盖率审计
- **缺失测试**: `base_m`, `params`, `polynomial_context`, `polynomial_optimizer`, `int_polynomial`, `filter`, `class_group`
- **建议**: 至少为 params 和 polynomial_context 添加单元测试

### [TEST] 0 个已修复 bug 的回归测试
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: 测试覆盖率审计
- **描述**: Session 1-3 修复的 10 个关键 bug 均无专属回归测试。包括：N-divisible 关系过滤、Schirokauer ℓ=2 only、指数公式、QC degree>3、evaluate_mod 溢出、extract_factors、Hensel p^d 溢出等
- **影响**: 任何重构都可能无意间回退这些修复
- **建议**: 优先添加 P0 级回归测试

### [TEST] 边界/极端情况覆盖率约 5%
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: 测试覆盖率审计
- **描述**: 缺失的关键边界测试：Integer 溢出、负数 mod、空矩阵、度 0 多项式、FB 边界素数、Single row 矩阵、Hensel p^d 溢出等

### [TEST] 缺少模块间集成测试
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: 测试覆盖率审计
- **描述**: 约 22/30 跨模块场景未测试，包括：FB→Sieve（空FB）、Sieve→Cofactorizer（1LP→2LP 升级）、Filter→MatrixBuilder（singleton 链）、BL→Sqrt（多依赖→多次 sqrt 尝试）、Sqrt→GCD（X=Y 退化情况）

### [TEST] 缺少压力/模糊测试
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 5 审计)
- **来源**: 测试覆盖率审计
- **描述**: 无 1000+ 关系过滤、100K+ FB 构建、1M 筛选位置、50K×50K 矩阵构建等压力测试

---

## P1 — Session 6 深度审计新发现

### [BUG] SparseRow::set() 非幂等——unsorted 时重复 set 等价于 clear
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 审计)
- **来源**: `include/gnfs/linalg/sparse_matrix.hpp` 行 ~70-80
- **描述**: `SparseRow::set(col)` 在 `sorted_=false` 时不检查重复，直接 push_back。若同一 col 被 set 两次，ensure_sorted() 的 GF(2) 去重会将其抵消为 0，即 `set(5); set(5)` = `clear(5)`。这违反了 "set" 的语义契约（幂等性）。
- **影响**: 任何在 unsorted 状态下多次调用 set() 的代码路径都会产生错误的矩阵。目前 MatrixBuilder 等调用者可能恰好不触发此路径，但这是一个隐蔽的 API 陷阱。
- **建议**: 在 unsorted 分支中加线性扫描去重检查，或改名为 `toggle()`/`xor_bit()` 明确 GF(2) 语义。

### [BUG] BitVector::xor_with() 无大小检查——不等长向量导致越界读取
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 审计)
- **来源**: `include/gnfs/linalg/sparse_matrix.hpp` 行 354-357
- **描述**: `xor_with(const BitVector& other)` 直接用 `this->bits_.size()` 作为循环上限，但不检查 `other.bits_.size()` 是否相同。若 other 更短，读取 `other.bits_[i]` 越界（UB）。
- **影响**: Block Lanczos 的依赖向量 XOR 操作可能在关系数不一致时触发 UB。
- **建议**: 加 `assert(bits_.size() == other.bits_.size())` 或取 `min(size(), other.size())`。

### [BUG] SparseMatrix::multiply_blocks() 索引计算错误（死代码）
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 审计)
- **来源**: `include/gnfs/linalg/sparse_matrix.hpp` 行 298-319
- **描述**: `multiply_blocks(x, result)` 中使用 `x[block_idx * rows_.size() + i]`，但 x 的布局假设是 (block_size × num_rows)，与 BlockVector 的实际布局不一致。函数签名使用 `std::vector<uint64_t>&` 而非 `BlockVector`，表明这是早期实现的遗留代码。
- **影响**: 此函数可能未被任何代码调用（Block Lanczos 使用 `spmv_forward`/`spmv_transpose`），但如果被调用会产生错误结果。
- **建议**: 确认是否为死代码，若是则删除；若需保留则修正索引布局。

### [BUG] rational_sqrt 中 fb.rational()[idx] 无越界检查
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 审计)
- **来源**: `include/gnfs/sqrt/rational_sqrt.hpp` 行 107-110
- **描述**: `uint32_t p = fb.rational()[idx].p`，其中 `idx` 来自 `fb_exponents` 的 key（关系中的 factor base 索引）。没有检查 `idx < fb.rational().size()`，若关系中包含无效的 factor base 索引，会导致越界访问。
- **影响**: 如果上游（cofactorizer）产出了包含非法 FB 索引的关系，rational_sqrt 会段错误而非返回有意义的错误信息。
- **建议**: 添加 `if (idx >= fb.rational().size()) continue;` 或 assert 守护。

### [BUG] MurphyEvaluator rng_ 数据竞争——Kleinjung 多线程并行使用
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: `include/gnfs/polynomial/murphy_evaluator.hpp:337` + `include/gnfs/polynomial/kleinjung_selector.hpp:133,167`
- **描述**: `MurphyEvaluator::compute()` 内部调用 `sample_e_score_log()` 时使用成员变量 `rng_`（std::mt19937_64）生成随机数。在 `kleinjung_selector.hpp` 中，单个 `evaluator` 对象通过引用传递给 `process_candidate`，后者在 `parallel_for_index` 中被多线程并发调用。多线程同时调用 `dist_a(rng_)` 产生数据竞争（UB）
- **影响**: Kleinjung 并行模式下的 Murphy 评分是 UB——可能产生错误排名、崩溃或不可重现结果
- **建议**: 改用 thread_local rng，或为每个线程传递独立的 MurphyEvaluator 副本

### [BUG] number_field norm_linear 符号公式错误——奇数度时差异被 abs 掩盖
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: `include/gnfs/sqrt/number_field.hpp:372-406`
- **描述**: norm_linear 计算 `N(a - b·α) = b^d · f(a/b)`，但数学上正确的公式是 `(-b)^d · f(a/b) = (-1)^d · b^d · f(a/b)`。奇数度 d 时差 `-1` 倍。代码在 line 401-403 取 abs() 掩盖了符号错误，但这意味着所有 norm 值都是正数，丢失了符号信息
- **影响**: 下游依赖 norm 符号的代码（如 sign column 计算）可能出错。当前 abs() 使得 GNFS 正确工作，但原理不严谨
- **建议**: 使用 `(-b)^d · f(a/b)` 或在有符号场景保留原始符号

### [BUG] next_prime() 在 couveignes/hensel 中 uint64 溢出——大素数搜索无限循环
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: `include/gnfs/sqrt/couveignes.hpp:619-628` + `include/gnfs/sqrt/hensel_sqrt.hpp:550-562`
- **描述**: 两处独立的 `next_prime()` 实现中，`n++` / `n += 2` 当 n 接近 UINT64_MAX 时溢出 wrap 到 0 或小值，导致无限循环。在 Hensel 中还会触发 O(sqrt(n)) 的暴力试除循环，n 较大时极慢
- **影响**: 如果 prime_start 配置为接近 UINT64_MAX 的值，进程挂起
- **建议**: 添加溢出检查 `if (n >= UINT64_MAX - 2) return std::nullopt;`

### [BUG] SmallVector move constructor/assignment 不销毁源对象的 inline 元素
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: `include/gnfs/util/small_vector.hpp:43-54, 67-79`
- **描述**: Move constructor (line 43-54) 和 move assignment (line 67-79) 在 `other.is_inline()` 时逐个移动 inline 存储中的元素，然后设 `other.size_ = 0`。但被移动的源元素的析构函数未被调用。对于有非平凡析构函数的类型 T（如 std::string、Integer），这会导致资源泄露
- **影响**: SmallVector<Integer> 或 SmallVector<std::string> 在 move 后泄露内存
- **建议**: move 后调用 `other.inline_ptr()[i].~T()` 析构每个已移动的源元素

### [BUG] polynomial_optimizer generate_smooth_numbers 不去重
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: `include/gnfs/polynomial/polynomial_optimizer.hpp:323-355`
- **描述**: 光滑数生成后排序（line 344）但未去重。同一个光滑数可由不同素数组合生成（如 6=2×3 和 3×2），导致重复候选
- **影响**: Kleinjung Stage 1 浪费时间处理重复的领导系数候选
- **建议**: 排序后添加 `smooth.erase(std::unique(smooth.begin(), smooth.end()), smooth.end())`

### [BUG] RelationCollector::merge() 未锁定 other 的 mutex——并发修改时数据竞争
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: `include/gnfs/relation/collector.hpp:227`
- **描述**: `merge()` 获取 `this->mutex_` 但直接读取 `other.relations_` 而不加锁。若另一个线程正在对 other 执行 add()，vector 可能正在重新分配，导致 UB
- **影响**: 并发场景下合并收集器时崩溃或数据损坏
- **建议**: 同时锁定 `other.mutex_`（注意锁顺序避免死锁）

### [BUG] RelationCollector callback 在非递归 mutex 下调用——回调内访问 collector 死锁
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: `include/gnfs/relation/collector.hpp:291-294`
- **描述**: `update_stats()` 在 `add()` 持有 `mutex_` 时被调用，callback 在 mutex 保护内执行。若 callback 调用 `collector.size()` 或其他也需要 mutex 的方法，会死锁（mutex 不是 recursive_mutex）
- **影响**: 使用 callback 的代码（test_relation_collector.cpp line 291-293）如果在 callback 内查询 collector 会挂起
- **建议**: 改用 `std::recursive_mutex`，或在 mutex 外调用 callback

### [BUG] Logger::level() 读取 level_ 无锁——与 set_level() 的写操作构成数据竞争
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 深度审计)
- **来源**: `include/gnfs/util/logger.hpp:63`
- **描述**: `level()` 直接返回 `level_` 而不持有 mutex，但 `set_level()` 在 mutex 保护下写入 `level_`。C++ 标准认定这是 data race (UB)
- **影响**: 理论上 UB，实际在大多数架构上因 uint8_t 读写原子性而无害
- **建议**: 将 `level_` 改为 `std::atomic<LogLevel>`

---

## P1 — Session 6 跨模块交互审计新发现

### [BUG] build_row() 符号列基于 a<0 而非 (a-bm)<0——不使用 QC 时符号完全错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 跨模块审计)
- **来源**: LinAlg→Sqrt 跨模块交互
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:527-530` vs `606-621`
- **描述**: `build_row()` 中符号列设置为 `if (rel.a < 0) row.set(0)`。但正确符号应基于 `(a - b*m) < 0`。只有 `build_row_with_qc()` 才正确重新计算符号（line 607-621）。如果调用 `build()` 而非 `build_with_qc()`，符号列大量错误——例如 a=5, b=1, m=1000 时 a-bm=-995<0 但 a>0 所以符号列记为正
- **影响**: 不使用 QC 的路径中，依赖向量含奇数个负号关系，有理平方根符号错误。E2E 测试用 `build_with_qc()` 所以不触发，但 `build()` 是公开 API
- **建议**: `build_row()` 中使用 `ctx.rational_value(a,b).is_negative()` 计算符号

### [BUG] matrix_builder FB 索引无越界检查——FB 不匹配时静默写入错误列
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 跨模块审计)
- **来源**: Relation→LinAlg 跨模块交互
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:540,553`
- **描述**: `row.set(mapping.rat_fb_start() + idx)` 和 `row.set(mapping.alg_fb_start() + idx)` 中 `idx` 来自 `Relation::rational_factors[j]`，无上限检查。若关系中的 FB 索引 >= fb.rational_count()（例如 FB 被重建后缩小），set() 写入更高区段的列（QC/Schirokauer/大素数列），静默产生错误矩阵
- **影响**: FB 重建或 pruning 后矩阵数据损坏
- **建议**: 添加 `assert(idx < mapping.num_rational_fb)` 和 `assert(idx < mapping.num_algebraic_fb)`

### [BUG] FactorBase::add_rational() 无去重——同一素数添加两次导致索引不一致
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 跨模块审计)
- **来源**: FactorBase→全模块交互
- **文件**: `include/gnfs/factor_base/factor_base.hpp:94-98`
- **描述**: `add_rational(p, log_p)` 对同一素数 p 调用两次时，第二次覆写 `rat_index_[p]` 指向新位置，但第一个条目仍留在 `rational_` 数组中。TrialDivider 遍历 rational_ 时两个条目都会匹配，但只有第二个索引能被 find_rational() 找到。第一个成为孤儿条目
- **影响**: 如果 builder 有 bug 导致重复添加，矩阵中某些行会在两个不同列设置相同素数的 bit
- **建议**: add_rational() 中检查 `rat_index_.count(p)` 去重

### [BUG] 代数因子基射影根 (r=UINT32_MAX) 在筛选中产生算术垃圾
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 跨模块审计)
- **来源**: FactorBase→Sieve 跨模块交互
- **文件**: `include/gnfs/factor_base/factor_base.hpp:78` + `include/gnfs/sieve/lattice_sieve.hpp:342`
- **描述**: `AlgebraicPrime::PROJECTIVE_ROOT = UINT32_MAX`。筛选遍历所有代数 FB 条目，对射影根条目执行 `a - b * UINT32_MAX` 计算。此乘法在 int64_t 中虽不溢出（b≤16384 时结果≈7×10^13），但结果 mod p 不代表正确的整除性检查——射影根的正确检查应是 `p | b` 而非 `p | (a - b*r)`
- **影响**: 射影素数的筛选位置标记完全错误，引入噪声
- **建议**: 在 sieve 循环中检测 `r == PROJECTIVE_ROOT` 并走 `p | b` 专用路径

### [BUG] sieve_batch() 是死代码——所有候选静默丢弃
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 跨模块审计)
- **来源**: Sieve 内部审计
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:127-131`
- **描述**: `sieve_batch()` 遍历 `result.candidates` 但循环体只有 TODO 注释，从不调用 cofactorizer。callback 也不被调用。调用此函数的任何代码路径会收到空的关系集合
- **影响**: 任何使用 sieve_batch() API 的调用者静默得到零关系
- **建议**: 完成 TODO 实现或标记为 `[[deprecated]]`

### [BUG] Sieve 区域对大 N 导致灾难性内存分配（>100GB）
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 跨模块审计)
- **来源**: GNFSParams→Sieve 跨模块交互
- **文件**: `include/gnfs/core/params.hpp:128-137` + `include/gnfs/sieve/lattice_sieve.hpp` 构造函数
- **描述**: 对 40+ digit N，`params.hpp` 计算 `sieve_width = min(sqrt(rational_bound)*8, 1e6)`。即使 clamp 到 1e6，`sieve_height = sieve_width / 4 = 250000`。sieve_array 大小 = width × height = 2.5×10^11 条目 × 2 bytes = 500GB。`sieve_array_.resize(region_.size(), 0)` 会 `std::bad_alloc` 崩溃。即使默认 SieveRegion（32768×16384），大小也是 500M 条目 = 1GB
- **影响**: 40+ digit N 直接崩溃
- **建议**: 分块筛选（bucket sieve）或动态 clamp sieve_height 使总大小 < 可用内存的 50%

### [BUG] SieveParams::combined_threshold() uint8_t 溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 跨模块审计)
- **来源**: GNFSParams→Sieve 参数传递
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:37`
- **描述**: `combined_threshold()` 返回 `uint8_t(rational_threshold + algebraic_threshold)`。当两者之和 > 255 时静默溢出。对 50+ digit N，两个阈值各约 80-130，之和可能超 256。此外 sieve 数组是 `uint16_t`，可累积到 65535，但阈值只能表示到 255
- **影响**: 阈值 wrap 到小值，几乎所有位置都被认为是候选，产生海量假阳性
- **建议**: 改为 `uint16_t combined_threshold()` 或使用两侧分别比较

### [BUG] Cofactorizer::stats_ 无 mutex 保护——多线程共享时数据竞争
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 跨模块审计)
- **来源**: Sieve→Cofactor 线程安全审计
- **文件**: `include/gnfs/cofactor/cofactorizer.hpp:137,140,143,172`
- **描述**: `verify()` 在多处修改 `stats_` 成员（smooth_count, partial_count, rejected_count 等），无任何同步。如果多个线程共享同一 Cofactorizer 调用 verify()，`stats_` 的递增操作构成数据竞争（UB）
- **影响**: 当前 E2E 测试中 cofactorizer 在串行循环中使用，不触发。但 parallel sieve 架构中若共享 cofactorizer 则 UB
- **建议**: 将 stats_ 成员改为 `std::atomic<uint64_t>` 或添加 mutex

### [BUG] Hensel 提升无精度充分性验证——系数超出 modulus/2 时静默错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 跨模块审计)
- **来源**: Sqrt 模块精度分析
- **文件**: `include/gnfs/sqrt/hensel_sqrt.hpp:226-240`
- **描述**: Hensel 提升后在 `extract_algebraic_sqrt()` 中将系数 centered（选 S[i] 或 S[i]-modulus 中绝对值较小者）。正确性前提是 modulus > 2×|真实系数|。精度估计在 line 73-87 使用启发式 `∑ log2(|a|+b×|m|) + 200`，但未验证结果。若估计不足，centering 选错值，所有后续计算基于错误系数
- **影响**: 大规模因式分解（关系多、b值大）时精度可能不够
- **建议**: 提升后添加验证：计算 sqrt² mod p^k 并与 product 比较

### [BUG] 无 N 素性检测——素数 N 导致完整管线空转后静默失败
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 跨模块审计)
- **来源**: 全管线边缘情况分析
- **文件**: 管线入口（如 `tests/test_gnfs_e2e.cpp:136`）
- **描述**: GNFS 管线无任何早期素性测试。如果 N 是素数，所有阶段正常执行（多项式选择、FB 构建、筛选、线性代数），直到 sqrt 阶段所有 GCD 都产生 1 或 N，才返回失败。对 50+ digit 的素数 N，可能浪费数小时计算
- **影响**: 用户提供素数 N 时无提示地浪费大量计算
- **建议**: 在管线入口添加 `mpz_probab_prime_p(n, 25)` 快速素性测试

### [BUG] Couveignes 回退公式 (N+1)/2 对所有 N 都数学错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-08 (Session 6 跨模块审计)
- **来源**: Sqrt 模块数学正确性分析
- **文件**: `include/gnfs/sqrt/couveignes.hpp:325-328`
- **描述**: 当 `rat_product^((N+1)/4) mod N` 不是正确平方根时，代码回退到 `(N+1)/2` 指数。但 `a^((N+1)/2) = a * a^((N-1)/2)` 是 Euler 准则（仅对素数 N 有意义），对合数 N 完全无定义。即使对素数 N，(N+1)/2 指数给出 `a * Legendre(a,N)` 而非 sqrt(a)。这使得 Couveignes 的 `verify_current()` 永远不匹配，回退路径 100% 失败
- **影响**: Couveignes 作为 Hensel 失败后的唯一回退路径，其内部验证机制完全失效。如果 N ≡ 1 mod 4 且 Hensel 失败，因式分解无法完成
- **建议**: 有理平方根应通过因子指数直接累积（rational_sqrt.hpp 已有此功能），不应在 Couveignes 内重算

---

## P0 — Session 6 深层审计（第二轮）

### [BUG] compute_log_prime() 系统性低估所有素数对数值
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6 深层审计)
- **来源**: FactorBase 模块逐行审计
- **文件**: `include/gnfs/factor_base/factor_base.hpp:175-181`
- **描述**: `compute_log_prime()` 计算 `floor(log2(p)) * scale`，但注释和语义应为 `floor(log2(p) * scale)`。例如 p=5, scale=16: floor(log2(5))×16 = 2×16 = 32，但 floor(log2(5)×16) = floor(37.15) = 37。偏差达 15.6%。对所有非 2 的幂的素数都有类似低估。代码旁边有一个 `compute_log_prime_precise()` 使用 `std::log2(double(p)) * scale` 做精确计算，但 builder.cpp 三处全部调用的是不精确版本
- **影响**: 筛法中所有因子基素数的 log 贡献被系统性低估 → 光滑候选的筛值偏低 → 阈值比较失效 → 漏检光滑关系或产生大量假阳性（取决于阈值设置方向）
- **建议**: builder.cpp 改用 `compute_log_prime_precise()`，或修正 fast 版本使用定点算术

### [BUG] 代数侧大素数映射仅按 p 索引，忽略根 r——不同素理想被合并为同一列
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6 深层审计)
- **来源**: LinAlg 矩阵构建逐行审计
- **文件**: `include/gnfs/linalg/matrix_builder.hpp:350,363-364,579-591`
- **描述**: `collect_large_primes()` (line 363) 将代数大素数插入 `alg_primes` 集合时仅使用 `p`。`build_row()` (line 586) 查找大素数列时也用 `alg_lp_to_col.find(p)`。但在 GNFS 中，同一素数 p 上方可能有多个不同的素理想 (p, α-r₁), (p, α-r₂),...，它们是不同的代数因子。两个关系可能有相同的大素数 p 但不同的根 r，对应不同的素理想。当前代码将它们合并到同一矩阵列，违反了代数侧因子分解的唯一性
- **影响**: 矩阵中不同素理想被错误合并，产生虚假的 GF(2) 依赖 → 平方根阶段使用这些依赖时乘积不是完全平方 → 因式分解失败率增加。对 d>3 的多项式（有更多根）问题更严重
- **建议**: 将 `alg_lp_to_col` 的键改为 `(p, r)` 对（如 `(uint64_t(p) << 32) | r`），与 factor base 的 `alg_index_` 保持一致

### [BUG] Block Lanczos partial_inverse() 未将非主元行清零——D·A·D ≠ D
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6 深层审计)
- **来源**: LinAlg Block Lanczos 逐行审计
- **文件**: `include/gnfs/linalg/block_lanczos.hpp:119-157`
- **描述**: `partial_inverse()` 做增广矩阵 [A|I] 的高斯消元后，将 right[] 全部 64 行复制到 D.rows[]。对于主元行（mask 中对应位为 1），D 包含正确的逆矩阵行。但对于非主元行（A 在该维度上秩亏），D 包含消元过程中的垃圾值。Montgomery Block Lanczos 要求 D 是 A 的"偏逆"，满足 D·A·D = D，即 D 在非可逆子空间上必须为零
- **影响**: 当 V_i^T·V_i 不满秩时（接近收敛或矩阵退化），Lanczos 递推公式错误，可能无法收敛或产生错误的零空间向量。当前 <10K 矩阵用 Gaussian fallback 不触发，≥10K 矩阵用 BL 时可能出问题
- **建议**: 第 154 行改为 `D.rows[i] = (mask & (1ULL << i)) ? right[i] : 0;`

---

## P1 — Session 6 深层审计（第二轮）

### [BUG] Block Lanczos add_identity() 添加完整 64×64 单位矩阵——应为 mask 子空间
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6 深层审计)
- **来源**: LinAlg Block Lanczos 逐行审计
- **文件**: `src/linalg/block_lanczos.cpp:356-361` + `include/gnfs/linalg/block_lanczos.hpp:112-115`
- **描述**: BL 递推公式中 `V_{next} += V_cur * (D*A + S̃)` 的 S̃ 应是非可逆子空间上的投影（即 mask 的补集上的单位矩阵）。但代码 `DA.add_identity()` 添加的是完整 64×64 单位矩阵 I，而非 `I & ~mask`。当 A_i 满秩时 D·A = I，D·A + I = 0（正确：无校正项）。但当 A_i 秩亏时（rank < 64），完整 I 在可逆子空间上添加了多余的 1，破坏递推
- **影响**: A_i 秩亏时 Lanczos 递推不收敛。通常只在接近终止时发生，但可能导致产出的零空间向量质量下降
- **建议**: 改为 `DA.add_identity_masked(~mask_cur)`（需新增方法），或手动 `for (int i = 0; i < 64; i++) if (!(mask_cur & (1ULL<<i))) DA.rows[i] ^= (1ULL<<i);`

### [BUG] Couveignes Gray Code 系数漂移——翻转间无 mod M 约化导致 CRT 不一致
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6 深层审计)
- **来源**: Sqrt 模块数学正确性分析
- **文件**: `include/gnfs/sqrt/couveignes.hpp:415-437`
- **描述**: Gray code 迭代中 `current_coeffs[ci] -= two_weights[bit_pos][ci]` (line 426) 和 `+= two_weights[bit_pos][ci]` (line 428) 对 Integer 系数做加减，但从不执行 mod M 约化。two_weights 值可以很大（是 CRT 权重的两倍），迭代 65536 次后系数可能远超 [-M/2, M/2] 范围。`verify_current()` 和 `extract_result()` 期望系数在 [-M/2, M/2] 范围内进行 centered reduction，但实际值可能超出数倍
- **影响**: 后期 Gray code 模式的 verify 检查基于漂移值，可能错过正确模式或产生错误匹配。对 num_primes > 10 时影响显著
- **建议**: 每次加减后添加 `current_coeffs[ci] %= M` 并 center，或在 verify_current() 中先约化再检查

### [BUG] estimate_initial_log typical_i 是半宽度但 typical_j 是中点——不一致导致初值偏高
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6 深层审计)
- **来源**: Sieve 模块逐行审计
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:213-214`
- **描述**: `typical_i = (i_max - i_min) / 2.0` 是区间半宽度（如 [-16384, 16384] → 16384），而 `typical_j = (j_max + j_min) / 2.0` 是区间中点（如 [1, 8192] → 4096）。typical_i 代表 |i| 的最大值而非平均值（平均值应为 ~W/3），而 typical_j 代表 |j| 的平均值。这使得 typical_a 和 typical_b 被高估约 2 倍
- **影响**: 初始筛值偏高 → 需要更多的素数贡献才能使位置低于阈值 → 漏检部分光滑候选。对小 N 影响不大（因为 init_val 被 uint16 截断），但大 N 时可能显著影响产率
- **建议**: 两者统一使用区间均值绝对值：`typical_i = (i_max - i_min) / 3.0`（RMS 近似），`typical_j = (j_max - j_min) / 3.0`

### [BUG] params.hpp special_q_min = rational_bound/5 落入因子基范围——浪费 Special-Q
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6 深层审计)
- **来源**: Core→Sieve 参数分析
- **文件**: `include/gnfs/core/params.hpp:165`
- **描述**: `special_q_min = max(rational_bound / 5, 100)` 将最小 special-Q 设为 FB 界的 1/5。GNFS 中 special-Q 应位于因子基之上（即 >= algebraic_bound），否则该素数已在因子基中被计入，选它做 special-Q 不提供额外的稀疏化。当前设置使约 80% 的 special-Q 范围 [rb/5, rb*2] 与 FB 重叠
- **影响**: 80% 的 special-Q 选择在 FB 内部，筛选效率低下。不是正确性 bug，但严重影响性能
- **建议**: 改为 `special_q_min = algebraic_bound + 1` 或 `rational_bound + 1`

### [BUG] base_m.cpp select() 不验证 f 在 Q 上不可约——可约多项式导致 GNFS 失败
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6 深层审计)
- **来源**: Polynomial 模块审计
- **文件**: `src/polynomial/base_m.cpp:8-37`
- **描述**: `BaseMSelector::select()` 通过 base-m 展开构造多项式 f 后直接返回，不检查 f 是否在 Q[x] 上不可约。如果 f 可约（例如 f = g·h），则 Q[α] 不是数域（而是直和 Q[α]/g × Q[α]/h），所有基于数域算术的后续步骤（factor base、algebraic norm、Schirokauer maps、algebraic sqrt）都数学上无效
- **影响**: 如果碰巧选到可约的 f，整个 GNFS 管线在数学上无效。对小 N 的低次多项式（d=3,4）不太可能可约，但对大 N 的高次多项式概率增加
- **建议**: 选多项式后添加不可约性验证（如用 Berlekamp 或 LLL 算法），不可约时重选 m

### [BUG] Integer bit_length(0) 返回 1——GMP 约定导致下游边界条件错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6 深层审计)
- **来源**: Core 模块逐行审计
- **文件**: `src/core/integer.cpp:93-94`
- **描述**: `mpz_sizeinbase(0, 2)` 按 GMP 规范返回 1。这使得 `Integer(0).bit_length() == 1`。下游代码如 `ecm.hpp:233-235` 中 `if (n_val.bit_length() == 0)` 作为零值守护永远不触发。类似地，任何用 `bit_length()` 估算数值大小的代码对零值都多估一位
- **影响**: 代码中的零值守护条件成为死代码。ECM 中 bit_length==0 守护不生效
- **建议**: 包装 bit_length()：`return is_zero() ? 0 : mpz_sizeinbase(value_, 2);`

### [BUG] Integer 除零行为不一致——Integer(0) 除法 GMP abort，int64_t(0) 除法抛异常
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6 深层审计)
- **来源**: Core 模块逐行审计
- **文件**: `src/core/integer.cpp`
- **描述**: `operator/=(const Integer& other)` 中如果 other 为零，`mpz_tdiv_q` 直接导致 GMP abort（进程终止，不可捕获）。`operator/=(int64_t value)` 中如果 value 为零，代码 `mpz_tdiv_q_ui(..., -value)` 的 -0 参数传给 unsigned 参数仍为 0，GMP 同样 abort。两条路径的错误处理都是不可恢复的进程终止，而非异常
- **影响**: 除零错误导致整个进程 abort，无法被 try/catch 捕获。调试时不友好
- **建议**: 添加前置检查 `if (other.is_zero()) throw std::domain_error("division by zero")`

### [BUG] Integer::sqrt() 对负数输入无检查——mpz_sqrt 调用 GMP abort
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6 深层审计)
- **来源**: Core 模块逐行审计
- **文件**: `include/gnfs/core/integer.hpp` (sqrt 方法)
- **描述**: `Integer::sqrt()` 直接调用 `mpz_sqrt()`。GMP 规定 mpz_sqrt 的参数必须非负，否则行为未定义（通常 abort）。当前无前置检查
- **影响**: 对负整数调用 sqrt 时进程 abort
- **建议**: 添加 `if (is_negative()) throw std::domain_error("sqrt of negative")`

### [BUG] Integer::powmod() 不验证负指数——负指数时结果未定义
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6 深层审计)
- **来源**: Core 模块逐行审计
- **文件**: `include/gnfs/core/integer.hpp` (powmod 方法)
- **描述**: `powmod(base, exp, mod)` 使用 `mpz_powm`。GMP 的 mpz_powm 对负指数要求 base 和 mod 互素，否则行为未定义。当前无指数符号检查
- **影响**: 传入负指数且 base 不可逆时 GMP abort
- **建议**: 添加 `if (exp.is_negative()) { /* 检查互素或抛异常 */ }`

### [BUG] class_group factor_ideal val=0 且 a=b·r 时无限循环
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6 深层审计)
- **来源**: Sqrt/ClassGroup 模块逐行审计
- **文件**: `include/gnfs/sqrt/class_group.hpp:383-393`
- **描述**: `val = a - int64_t(b) * int64_t(pi.r)`。若 a == b*r（精确整除），val = 0。while 循环条件 `val % p == 0 && val != 0` 中 `val != 0` 阻止进入循环，exp 保持 0。但数学上 a - b·r = 0 意味着素理想 (p, α-r) 的赋值是无穷大（或说分子完全被该理想整除），exp 应为某个正数。代码静默返回 exp=0，丢失了这个因子的全部贡献
- **影响**: 特定 (a,b) 对的理想分解不完整。类群字符计算使用错误的赋值
- **建议**: val==0 时使用 `Integer::valuation(a - b·α, p)` 精确计算赋值

### [BUG] ECM Stage 2 链式乘法增加因子丢失概率
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6 深层审计)
- **来源**: Cofactor 模块算法分析
- **文件**: `include/gnfs/cofactor/ecm.hpp:410-413`
- **描述**: Stage 2 中 `Qcurr = mont_mul(Qcurr, p, a24, n)` 将每个素数链式累乘（∏pᵢ·Q0），而非标准做法的独立计算 p·Q0。数学上 chaining 等价但有微妙缺陷：若 N=p₁·q₁ 且恰好 E(Z/p₁Z) 和 E(Z/q₁Z) 的阶各有一个素数在 (B1, B2] 中，chaining 使 Z 坐标在两侧都变为 0 → accum 被 N 整除 → gcd(accum, N)=N → 因子丢失（line 425 返回 nullopt）。独立计算不会有此问题
- **影响**: 少数情况下（两侧阶各有一个 Stage 2 范围内素数）因子丢失，ECM 需要更多曲线尝试
- **建议**: 改为独立计算每个 p·Q0 并检查 Z 坐标，或至少在 gcd=N 时回溯逐个检查

### [BUG] gauss.hpp build_null_space() 的 `history` 参数从未使用——死参数
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6 深层审计)
- **来源**: LinAlg 模块逐行审计
- **文件**: `include/gnfs/linalg/gauss.hpp:161-219`
- **描述**: `build_null_space(result, matrix, history, is_pivot_col)` 声明了 `const std::vector<BitVector>& history` 参数，但函数体中从未引用 `history`。消元阶段（lines 77-92）正确维护了 history，但 null space 构建阶段完全忽略它。函数直接在消元后的矩阵上做反向代入
- **影响**: history 的计算开销被浪费（O(n²) 空间 + O(n³) XOR 操作）。功能上不影响正确性（因为直接在 RREF 上反向代入是等价的），但如果有人期望 history-based 提取会得到错误结果
- **建议**: 移除 history 参数，或重写使用 history 的高效版本

### [BUG] Block Lanczos 终止条件仅检查 V_cur 是否为零——应额外检查 A_cur 秩
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 6 深层审计)
- **来源**: LinAlg Block Lanczos 算法分析
- **文件**: `src/linalg/block_lanczos.cpp:336`
- **描述**: 终止检查 `if (V_cur.is_zero()) break` 仅在所有 64 个 block 向量同时为零时触发。Montgomery 算法的正确终止条件还应包括：当 A_cur = V_cur^T·V_cur 的秩为 0（或降到很低时），意味着在所有 block 方向上都已收敛。当前代码可能在该终止条件满足后仍继续无效迭代
- **影响**: 可能多执行几十次无效迭代，浪费时间
- **建议**: 添加 `if (mask_cur == 0) break;`（mask=0 表示 A_cur 秩为 0）

---

## P0 — Session 7 数学/指针/API 深层审计

### [BUG] 4 处 `static Integer zero` 返回引用——线程不安全 + 别名陷阱
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7 审计)
- **来源**: 指针/生命周期审计
- **文件**:
  - `include/gnfs/sqrt/number_field.hpp:65` — `NumberFieldElement::coeff()`
  - `include/gnfs/sqrt/number_field.hpp:195` — `NumberField::coeff()`
  - `include/gnfs/core/polynomial_context.hpp:77` — `PolynomialContext::coeff()`
  - `include/gnfs/polynomial/int_polynomial.hpp:69` — `IntPolynomial::operator[]() const`
- **描述**: 这 4 处越界访问都返回 `const Integer&` 到同一个函数级 `static Integer zero`。问题有三层：(1) **别名**：两次越界调用返回同一对象的引用，`&a == &b` 为 true，破坏值语义预期；(2) **线程安全**：`Integer` 包装 `mpz_t`（可变结构体），GMP 的 mpz_t 不是线程安全的——多线程并发调用 `coeff(out_of_bounds)` 可能在内部触发 GMP 读/写竞争；(3) **Hensel 并行路径**：`hensel_sqrt.hpp:356` 的并行乘积计算中，多线程调用 `nf.coeff(i)` 若 i 越界则共享同一 static zero
- **影响**: 多线程因式分解（Hensel 并行乘积、sieve_parallel）中可能崩溃或静默数据损坏
- **建议**: 返回值类型改为 `Integer`（按值），或使用 `thread_local static Integer zero`

### [BUG] estimate_initial_log NaN/Inf → uint16_t 强制转换是 UB
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7 审计)
- **来源**: C++ API 误用审计
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:221-231`
- **描述**: `rat_val = std::abs(typical_a - typical_b * m_val)` 在 `typical_a ≈ typical_b * m_val` 时可等于 0。`std::log2(0.0)` 返回 `-Inf`。`-Inf * scale` = `-Inf`。Line 230 `static_cast<uint16_t>(-Inf)` 是未定义行为（C++ 标准 [conv.fpint]：超出目标范围的浮点转整数是 UB）。类似地 `typical_a = 0` 时 `pow(0, d) = 0`，`log2(0)` = `-Inf`
- **影响**: 特定 lattice basis 参数下进程崩溃或返回垃圾初始值
- **建议**: 添加 `if (!std::isfinite(combined) || combined < 0) return 0;`

---

## P1 — Session 7 数学/指针/API 深层审计

### [BUG] Schirokauer precompute_for_prime 同样假设"无根=不可约"——d≥4 时错误
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7 数学审计)
- **来源**: LinAlg Schirokauer 数学正确性分析
- **文件**: `include/gnfs/linalg/schirokauer.hpp:369-385`
- **描述**: 与已知 Bug "Hensel/Couveignes 不可约性检查仅测试线性因子" 同一模式，但此处是**第四个**受影响位置。Line 370-378 枚举 x=0..ℓ-1 检查根，无根则声明"irreducible"并使用指数 `ℓ^d - 1`。但对 d≥4，f mod ℓ 可无根但可约（如 x⁴+x²+1 = (x²+x+1)² over F₂）。此时 `(Z/ℓZ)[x]/(f)` 不是域，乘法群阶不是 `ℓ^d - 1`，Schirokauer map 值数学上错误
- **影响**: d≥4 多项式的 Schirokauer 列在 f mod ℓ 可约但无根时产生错误值，矩阵依赖质量下降
- **建议**: 与 Hensel/Couveignes 一起修复——使用完整不可约检测 `gcd(x^{ℓ^k}-x, f)` for k=1..d/2
- **注**: 此条目是对已有 P0 条目 "Hensel/Couveignes 不可约性检查" 的补充——该条目列了 3 个文件，现增加第 4 个

### [BUG] SchirokaurMap 存储 `const PolynomialContext&`——潜在悬垂引用
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7 指针审计)
- **来源**: 生命周期分析
- **文件**: `include/gnfs/linalg/schirokauer.hpp:342`
- **描述**: `SchirokaurMap` 的 `ctx_` 成员是 `const PolynomialContext&`，生命周期依赖于外部对象。如果构造 SchirokaurMap 后原始 PolynomialContext 被 move 或销毁，`ctx_` 变为悬垂引用。`PolynomialContext` 有 move constructor（line 44），使得 move 后引用失效成为可能
- **影响**: 若调用方不小心 move 了 ctx，后续 compute() 读取已销毁对象——段错误或静默数据损坏
- **建议**: 改为存储 `std::shared_ptr<const PolynomialContext>` 或 clone 一份

### [BUG] LatticeSieve 存储 `const PolynomialContext&` 和 `const FactorBase&`——悬垂引用 + 线程传播
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7 指针审计)
- **来源**: 生命周期分析
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:186-187`
- **描述**: `ctx_` 和 `fb_` 是 const 引用。`sieve_parallel()` (line 159) 创建线程局部 `LatticeSieve local_sieve(ctx_, fb_, params_)` 复制这些引用。所有线程共享同一对引用。若原始 ctx/fb 在线程运行期间被销毁或 move，所有工作线程同时访问已失效对象
- **影响**: 并行筛选中若调用方过早释放 ctx/fb，全部工作线程 use-after-free
- **建议**: `sieve_parallel` 参数改为 `const PolynomialContext&, const FactorBase&` 直接传递，或内部使用 shared_ptr

### [BUG] SparseRow const_cast ensure_sorted() 多线程并发排序是 UB
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7 指针审计)
- **来源**: 并发安全审计
- **文件**: `include/gnfs/linalg/sparse_matrix.hpp:61-63,93,134,150`
- **描述**: 已有 BACKLOG 条目指出 `const_cast` 违反 const 契约。此处补充**多线程角度**：`SparseRow::test()`, `weight()`, `indices()`, `xor_with(const&)` 全部通过 const_cast 调用 `ensure_sorted()` 修改内部 `indices_` 和 `sorted_`。Block Lanczos 的 `spmv_forward`/`spmv_transpose` 在多线程中可能并发调用同一矩阵的 `row(i).indices()`，触发多线程对同一 `indices_` vector 的并发排序——数据竞争 UB
- **影响**: Block Lanczos 并行化后立即触发 UB
- **建议**: `sorted_` 和 `indices_` 标记为 `mutable`，构造后立即调用 `ensure_sorted()`（或在 build_index() 中统一排序），消除运行时 const_cast

### [BUG] RelationCollector::set_callback() 无 mutex 保护——与 add() 数据竞争
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7 指针审计)
- **来源**: 并发安全审计
- **文件**: `include/gnfs/relation/collector.hpp:249-251`
- **描述**: `set_callback(callback)` 直接 move-assign `callback_` 而不加锁。`add()` 在持有 mutex 的情况下通过 `update_stats()` (line 292) 读取 `callback_`。若一个线程调用 set_callback 而另一个线程正在 add()，对 `callback_` 的读/写构成数据竞争
- **影响**: 并发修改回调时 UB
- **建议**: `set_callback()` 内加 `std::lock_guard<std::mutex> lock(mutex_)`

### [BUG] IntPolynomial mutable operator[] 无上限检查——任意大 resize 导致 OOM
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7 API 审计)
- **来源**: C++ 容器安全审计
- **文件**: `include/gnfs/polynomial/int_polynomial.hpp:75-80`
- **描述**: `operator[](size_t i)` 对越界 `i` 直接执行 `coeffs_.resize(i + 1)`。若 i 来自不可信输入（如反序列化的度数），可能分配 TB 级内存。每个新 `Integer` 条目调用 `mpz_init()`，高度 i 意味着大量 GMP 分配
- **影响**: 恶意/损坏数据导致 OOM 崩溃
- **建议**: 添加 `assert(i <= MAX_DEGREE)` 或 `if (i > 1000) throw`

### [BUG] Eratosthenes 筛法 `p * 2` 在 p > UINT32_MAX/2 时溢出
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7 API 审计)
- **来源**: 整数溢出审计
- **文件**: `src/factor_base/builder.cpp:80,102,131`
- **描述**: `for (uint32_t k = p * 2; k <= bound; k += p)` — 当 `p > UINT32_MAX / 2 ≈ 2.15×10⁹` 时，`p * 2` 溢出为小值。循环从错误位置开始，标记了错误的合数，导致因子基中包含合数或遗漏素数。当前 `rational_bound` 被 clamp 到 1e9 所以安全，但如果未来提高上限则触发
- **影响**: 大因子基（bound > 2^31）时因子基数据损坏
- **建议**: 改为 `for (uint64_t k = uint64_t(p) * 2; k <= bound; k += p)`

### [BUG] FactorBaseBuilder 实例方法 build(uint32,uint32) 返回空 FactorBase
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7 API 审计)
- **来源**: 死代码/非功能 API 审计
- **文件**: `src/factor_base/builder.cpp:62-69`
- **描述**: 实例方法 `build(rational_bound, algebraic_bound)` 因无法复制 `PolynomialContext` 而直接返回空 `FactorBase()`。虽然有注释说明，但公共 API 返回空结果而不抛异常，调用方会得到零大小因子基，后续管线静默失效
- **影响**: 使用实例 API 的代码得到空因子基，筛选产出零关系
- **建议**: 标记 `[[deprecated]]` 并抛异常，或完全删除

### [BUG] sieve_parallel() 使用不必要的 mutex 保护独立数组写入——性能浪费
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7 API 审计)
- **来源**: 并发模式审计
- **文件**: `include/gnfs/sieve/lattice_sieve.hpp:153,168-169`
- **描述**: 每个工作线程通过 `atomic fetch_add` 获得唯一索引 `idx`，写入 `all_results[idx]`。由于不同线程写入不同位置，无需 mutex。但 line 168 仍获取 `results_mutex` 锁，导致所有线程在写入时串行化
- **影响**: 并行筛选的写入阶段被不必要地序列化，多线程加速受限
- **建议**: 移除 `results_mutex` 和 `lock_guard`

### [BUG] ThreadPool parallel_for/parallel_for_index 通过引用捕获 func——若未来添加 early return 则悬垂
- **状态**: 🔴 待处理
- **发现日期**: 2026-03-09 (Session 7 指针审计)
- **来源**: Lambda 捕获安全审计
- **文件**: `include/gnfs/util/thread_pool.hpp:104,140`
- **描述**: `submit([..., &func]() { ... })` 将 `func` 通过引用捕获到提交给线程池的 lambda 中。当前安全：`parallel_for` 在返回前通过 `future.get()` 等待所有任务完成。但若未来添加异常处理（try/catch 跳过 get() 循环）或超时机制，func 的引用会在 parallel_for 返回后悬垂，工作线程访问已销毁的函数对象
- **影响**: 当前无 bug，但代码脆弱。任何对等待逻辑的修改都可能引入 use-after-free
- **建议**: 改为按值捕获 `func`（如 `[func]` 或 `[f = std::move(func)]`）

---

## 已完成

### [OPT] ~~Hensel Sqrt 预计算优化~~ ✅
- **状态**: ✅ 已完成
- **发现日期**: 2026-02-20 (Session 2)
- **解决日期**: 2026-02-22 (Session 3)
- **结果**: Hensel 15.5× 加速，25-digit 总耗时 603s → 236.5s
