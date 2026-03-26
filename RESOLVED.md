# RESOLVED — 已完成与误报记录

> 从 `BACKLOG.md` 拆分而来。记录所有已修复、已验证通过的条目和经核查确认的误报。
> 作为项目审计和知识沉淀，避免重复上报、记录修复方案供后续参考。

---

## 已完成 ✅

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
