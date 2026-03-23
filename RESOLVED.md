# RESOLVED — 已完成与误报记录

> 从 `BACKLOG.md` 拆分而来。记录所有已修复、已验证通过的条目和经核查确认的误报。
> 作为项目审计和知识沉淀，避免重复上报、记录修复方案供后续参考。

---

## 已完成 ✅

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
