# BACKLOG — 待办备忘录

> 只记录**未完成**的问题。已完成和误报条目见 `RESOLVED.md`。
> 从文件开头往下读即为优先级：P1 > P1-OPT > P2 > P3 > TEST。

| 级别 | 条数 | 涵盖 |
|------|------|------|
| **P1** | 11 | trial_div b·r 溢出, UV int64 UB, sieve flood, SparseRow toggle, LP ideal key, ECM j_lo, E2E/progressive exit, integer UB, ECM stage2, matrix QC |
| **P1-OPT** | 0 | (已清空) |
| **P2** | 43 | correctness ×26, performance ×9, design ×5, class group ×1, infra ×2 |
| **P3** | 27 | 远期架构 ×7, style ×6, dead code ×4, quality ×8, risk ×2 |
| **TEST** | 8 | weak assertions ×4, logic ×1, missing coverage ×2, flaky ×1 |

---

## P1 — 高优先级（影响正确性、可靠性）

### [BUG] trial_division 代数可整除性检查 b·r int64 溢出
- **发现日期**: 2026-03-14
- **文件**: `cofactor/trial_division.hpp:136`
- **描述**: `int64_t check = a - static_cast<int64_t>(b) * static_cast<int64_t>(r);` 其中 b 是 `uint64_t`，r 是 `uint32_t`。当 b > 2^31 且 r 接近 p 时，乘积 b·r 溢出 `int64_t`，导致可整除性检查出错——非整除素数可能被误判为整除（或反之），腐化关系中的因子列表和指数。
- **建议**: 先对 b 取模 p 再乘：`uint64_t b_mod = b % p; uint64_t br_mod = (b_mod * (uint64_t)r) % p;` 然后比较 `a_mod == br_mod`。

### [BUG] compute_rational/algebraic_uv int64_t 有符号溢出（确认 UB）
- **发现日期**: 2026-03-14
- **文件**: `sieve/lattice_sieve.hpp:282-284, 302-304`
- **描述**: `f0_mod * m64` 是 int64_t 乘法，两操作数均在 [0, p-1]，p 最大可达 ~2^32（代数因子基上界）。乘积最大 (2^32-1)^2 ≈ 2^64，超出 INT64_MAX ≈ 2^63。这是 C++ 有符号溢出未定义行为（第三轮审计以 100% 置信度确认）。产出错误的 UV 参数导致对应素数的筛法命中位置错误。
- **建议**: 用 `((__int128_t)f0_mod * m64) % p64` 做中间乘积。

### [BUG] ECM Stage2 BSGS j_lo 边界遗漏 B1 附近素数
- **发现日期**: 2026-03-14
- **文件**: `cofactor/ecm.hpp:493`
- **描述**: `j_lo = B1 / D + 1` 起始太高。素数 `p = floor(B1/D)·D + d` 其中 `d > 0` 且 `p > B1` 永远不会被检测（j = floor(B1/D) < j_lo）。对 D=2310, B1=11000，所有 (11000, 11239] 内素数被系统性跳过。
- **建议**: `j_lo = B1 / D`（而非 `B1 / D + 1`）。

### [BUG] Sieve eff_thresh=0 导致候选洪泛
- **发现日期**: 2026-03-14
- **文件**: `sieve/lattice_sieve.hpp:585-587`
- **描述**: 当 `last_init_val_ <= threshold`（日志估计小于阈值，如 `estimate_initial_log` 返回 0），`eff_thresh` 被设为 0。此时 `sieve_array_[idx] >= 0` 对所有位置为真——整个筛区（最多 256M 个位置）全部成为候选，触发灾难性的伪正候选洪泛。
- **建议**: `if (last_init_val_ <= threshold) return {};`（直接返回空，而非设 eff_thresh=0）。

### [BUG] SparseRow::set() 在 unsorted 状态下重复调用翻转 bit
- **发现日期**: 2026-03-14
- **文件**: `linalg/sparse_matrix.hpp:26-37`
- **描述**: GF(2) 中 `set()` 应幂等（设 bit 为 1）。但当 `sorted_==false` 时无去重检查，直接 append。之后 `ensure_sorted()` 通过对偶消除将重复项相消，使 bit 从 1→0。这违反 `set()` 的语义契约——两次 `set(col)` 反而清除了该列。
- **建议**: 在 `set()` 开头调用 `ensure_sorted()` 再执行去重检查，或维护一个 unsorted 去重机制。

### [BUG] verify_algebraic_ideal_powers 用 lp.p 做 key 忽略 prime-ideal 区分
- **发现日期**: 2026-03-14
- **文件**: `sqrt/algebraic_sqrt.hpp:26-52`
- **描述**: 代数大素数指数奇偶检查用 `lp.p` 做 map key。但同一有理素数 p 上方可有多个代数理想 `(p, r1)` 和 `(p, r2)`，它们是不同的对象。用 p 做 key 会将不同理想的指数求和：两个不同理想各出现 1 次被误判为"指数 2（偶数）"。matrix_builder 中 `collect_large_primes()` 正确使用 `(p, r)` 做 key，但此检查函数遗漏。
- **建议**: 改用 `(lp.p, lp.r)` 作为 map key。

### [BUG] test_gnfs_e2e main() 无条件返回 0
- **发现日期**: 2026-03-14
- **文件**: `tests/test_gnfs_e2e.cpp:1030`
- **描述**: `main()` 始终 `return 0`，所有测试函数只打印 SUCCESS/FAILURE 到 stdout 但不传播退出码。这是覆盖完整 GNFS 流水线的唯一测试，任何算法回归（筛法、线性代数、平方根）在 CI 中都无法被检测。
- **建议**: 各 test 函数返回 bool，main 收集结果返回 `ok ? 0 : 1`。

### [BUG] test_gnfs_progressive main() 无条件返回 0
- **发现日期**: 2026-03-14
- **文件**: `tests/test_gnfs_progressive.cpp`
- **描述**: 同 test_gnfs_e2e，L1-L5 所有 progressive 级别的分解失败都不会导致测试二进制返回非零。CI 永远绿。
- **建议**: 收集 pass/fail 计数，返回 `(fail > 0) ? 1 : 0`。

### [BUG] Integer::operator%=(INT64_MIN) 有符号溢出 UB
- **发现日期**: 2026-03-14
- **文件**: `src/core/integer.cpp:433`
- **描述**: `unsigned long abs_val = (value >= 0) ? value : -value;` 当 `value == INT64_MIN` 时，`-value` 溢出有符号 64 位，是未定义行为。`operator+=` 和 `operator-=` 已用 `-(value+1)+1UL` 技巧处理，但 `operator%=` 遗漏。
- **建议**: `unsigned long abs_val = (value >= 0) ? static_cast<unsigned long>(value) : static_cast<unsigned long>(-(value + 1)) + 1UL;`

### [BUG] ECM Stage2 BSGS 零交叉积静默跳过
- **发现日期**: 2026-03-14
- **文件**: `cofactor/ecm.hpp:512-519`
- **描述**: `accumulate_step` 中当 `c.is_zero()` (G.x·b.z ≡ b.x·G.z mod n) 时直接跳过，不累积到 accum 中。c=0 意味着因子可能存在于 Z 坐标中，但跳过导致 `gcd(accum, n)` 无法检测到该因子。虽然其他 baby-giant 对通常能捕获，但存在漏检风险。
- **建议**: c=0 时触发即时 `gcd(G.z, n)` 检查，而非简单跳过。

### [BUG] has_multiple_root 只检查 f'≡0，未检查 gcd(f,f')
- **发现日期**: 2026-03-14
- **文件**: `linalg/matrix_builder.hpp:513-522`
- **描述**: 判断多项式 mod p 是否有重根时，只检查 f' 是否全零。这是充分条件但非必要条件——`f(x) = (x-1)²(x-2)` 的 f' 非零但有重根。遗漏重根的 QC 素数会导致 Legendre 符号计算错误（除零或错误值）。
- **建议**: 计算 `gcd(f mod p, f' mod p)` 并检查度是否 ≥ 1。

---

## P2 — 中优先级

### 正确性问题

#### [BUG] algebraic_norm_i128 当 b=0 时除零
- **发现日期**: 2026-03-14
- **文件**: `core/polynomial_context.hpp:249-259`
- **描述**: `b_pow /= b_val` 当 b_val=0 时是未定义行为。虽然 GNFS 筛法输出的 b 总是 > 0，但函数无防护。
- **建议**: 函数入口添加 `if (b == 0) return {0, false};`

#### [BUG] Relation::deserialize 无流错误检查
- **发现日期**: 2026-03-14
- **文件**: `core/relation.hpp:150-228`
- **描述**: `read_and_xor` 不检查流状态。EOF 或 I/O 错误后部分读取产生垃圾数据，checksum 检查因 `if (is)` 为 false 被跳过，腐败关系静默返回。
- **建议**: `read_and_xor` 内部每次 read 后检查 `is.good()`，失败则 throw。

#### [BUG] Relation::deserialize 无界 resize/reserve
- **发现日期**: 2026-03-14
- **文件**: `core/relation.hpp:175,181,189,200,213`
- **描述**: `rat_count`, `alg_count` 等直接从流读取后用于 `resize`/`reserve`，无上界验证。腐败数据可导致 OOM。
- **建议**: 添加 `MAX_FACTORS = 1 << 20` 等上界检查。

#### [BUG] CompactSmallPrime int16_t 对大筛区脆弱
- **发现日期**: 2026-03-14
- **文件**: `sieve/lattice_sieve.hpp:490-497`
- **描述**: `delta`, `i_min_mod`, `i_mod` 为 `int16_t`。当前默认 sieve_width=32768 时恰好安全（素数 < 32768，delta ≤ 32766）。但如果 sieve width 增大到 > 32768，int16_t 会静默截断。没有 assert 保护。
- **建议**: 添加 `static_assert` 或运行时断言 `bucket_threshold <= INT16_MAX`，或改用 `int32_t`。

#### [BUG] RelationCollector::relations() 线程不安全引用
- **发现日期**: 2026-03-14
- **文件**: `relation/collector.hpp:174-176`
- **描述**: 返回 `const vector<Relation>&` 而不持锁。其他线程调用 `add()` 时 vector 可能重新分配，导致引用悬挂。对比 `get_relations()` 正确地在锁下克隆。
- **建议**: 移除 `relations()` 或添加文档说明只能在无并发修改时调用。

#### [BUG] RelationCollector::merge() 自我合并死锁
- **发现日期**: 2026-03-14
- **文件**: `relation/collector.hpp:246-248`
- **描述**: `std::scoped_lock lock(mutex_, other.mutex_)` 当 `this == &other` 时尝试重复获取同一 `std::mutex`（不可重入），是未定义行为。
- **建议**: 入口添加 `if (this == &other) return 0;`

#### [BUG] GFPolyOps::mul uint64 乘法溢出
- **发现日期**: 2026-03-14
- **文件**: `linalg/schirokauer.hpp:76`
- **描述**: `a[i] * b[j]` 是 `uint64_t` 乘法，当 ℓ > 2 时系数可接近 p-1，乘积溢出。当前主路径 ℓ=2 安全（系数 0 或 1），但 `divmod` (line 91) 和 `powmod` 同样有此问题。
- **建议**: 中间乘积用 `__uint128_t`。

#### [BUG] Schirokauer edf 固定 RNG 种子
- **发现日期**: 2026-03-14
- **文件**: `linalg/schirokauer.hpp:199`
- **描述**: `std::mt19937 rng(12345 + d)` 固定种子。若随机多项式恰好对特定输入无法分裂，200 次尝试全部失败，且每次调用结果相同。
- **建议**: 改用 `std::random_device{}()` 或从调用者传入 RNG。

#### [BUG] Schirokauer ell_k 溢出无检查
- **发现日期**: 2026-03-14
- **文件**: `linalg/schirokauer.hpp:797-799`
- **描述**: `info.ell_k *= ell` 循环无溢出保护。当前 k=8, ell=2 安全（ell_k=256），但 config 无上界约束。
- **建议**: 添加 `assert(k <= 40 || ell == 2)` 或用 `__uint128_t`。

#### [BUG] number_field reduce() 假设 monic 多项式
- **发现日期**: 2026-03-14
- **文件**: `sqrt/number_field.hpp:436-466`
- **描述**: `reduce()` 减去 `high_coeff * f_coeffs_[i]` 时不除 `f_coeffs_[degree_]`，对非 monic 多项式结果错误。`multiply()` 和 `power()` 调用此函数。虽然当前 `power()` 似未被生产代码调用（`power_mod_n` 用 `multiply_mod_n`），但这是隐蔽的正确性陷阱。
- **建议**: 添加 `assert(f_coeffs_[degree_].is_one())` 或实现非 monic 版本。

#### [BUG] couveignes.hpp expected_product 计算后从未使用
- **发现日期**: 2026-03-14
- **文件**: `sqrt/couveignes.hpp:213-217`
- **描述**: 完整的 `NumberFieldElement` 乘积通过 O(n·d²) 次多项式乘法计算，但结果从未被验证使用。纯浪费。这是旧验证代码被移除后的残留。
- **建议**: 删除 lines 213-217 的 `expected_product` 计算。

#### [BUG] classify_cofactor PrimePower 大于 LP bound 的 fallthrough
- **发现日期**: 2026-03-14
- **文件**: `cofactor/smooth_check.hpp:312-319`
- **描述**: 当余因子是 `p^k`（p 是素数但 p > large_prime_bound）时，内层 if 失败，函数跌落到 Pollard rho / ECM 路径，浪费计算。应直接返回 `TooLarge`。
- **建议**: `is_perfect_power` 块内添加 `else { result.type = CofactorClass::TooLarge; return result; }`。

#### [BUG] filter.hpp 2LP merge 只处理 weight-2，忽略 weight-3+
- **发现日期**: 2026-03-14
- **文件**: `relation/filter.hpp:398-411`
- **描述**: `if (indices.size() != 2) continue;` 跳过 weight ≥ 3 的 LP 键。标准 GNFS 可以通过链式合并处理 weight-3 键。当前设计保守但可能丢弃有效可合并的部分关系。
- **建议**: 记录此限制。未来可实现 weight-3 链式合并（合并最便宜的两个）。

#### [BUG] SGE w2-merge 后 col_to_rows 过时
- **发现日期**: 2026-03-14
- **文件**: `linalg/sge.hpp:183-228`
- **描述**: `xor_with(r1, r2)` 后，r1 的列集变化，但 `col_to_rows` 未更新——r1 不再拥有的列仍指向 r1，r1 新获得的列未添加 r1。同一 pass 内后续 w2 决策基于过时数据，可能产生错误的行组合。
- **建议**: 每次 w2-merge 后重建 `col_to_rows`（较慢），或每 pass 只处理一个 w2 列然后 break 强制重建。

#### [BUG] matrix_builder collect_large_primes uint8_t 指数累积溢出
- **发现日期**: 2026-03-14
- **文件**: `linalg/matrix_builder.hpp:401-420`
- **描述**: `std::unordered_map<uint64_t, uint8_t> rat_exp` 累加多个 `PrimePower::e`，在合并关系后同一 LP 指数可能超过 255，静默溢出导致奇偶检测 (`exp % 2`) 错误。
- **建议**: 改用 `uint32_t` 或 `uint16_t` 作为指数累积类型。

#### [BUG] LP 素数计数公式分母错误导致关系目标低估 ~22%
- **发现日期**: 2026-03-14
- **文件**: `core/params.hpp:319-320`
- **描述**: `lp_primes = (lp_bound - alg_bound) / ln(lp_bound)`，正确公式应为 `π(lp_bound) - π(alg_bound) ≈ lp_bound/ln(lp_bound) - alg_bound/ln(alg_bound)`。25-digit 实测：正确值 ~19746，代码计算 ~12040（低估 39%），导致 `n_min` 低估 ~22%。关系目标不足可能导致矩阵行过少、依赖不足。
- **建议**: `double lp_primes = lp_bound_d / log(lp_bound_d) - alg_bound_d / log(max(alg_bound_d, 2.0));`

#### [BUG] Pollard rho 非标准 Brent 结构浪费 2× 函数求值
- **发现日期**: 2026-03-14
- **文件**: `cofactor/smooth_check.hpp:228-253`
- **描述**: 每 phase 先空推进 y 步 r 步（无乘积累积），再累积另外 r 步乘积。标准 Brent 仅需 r 步。相当于 2× 函数求值，与注释声称的"减少 36%"相反。算法仍正确（能找到因子），但在固定 `max_iterations` 预算下效率减半。
- **建议**: 移除第一个空推进循环，直接在唯一的内循环中累积乘积。

#### [BUG] merge_all() 末尾丢弃未合并的原始 2LP 关系
- **发现日期**: 2026-03-14
- **文件**: `relation/filter.hpp:446-449`
- **描述**: `if (rel.is_merged()) full_results.push_back(...)` 用 `extra_ab_pairs.empty()` 判断是否合并过。原始 2LP 关系从未参与合并（`extra_ab_pairs` 为空），但可能有存活的 LP 键适合作为矩阵 LP 列。这些关系被静默丢弃。
- **建议**: 明确文档是否故意丢弃（防 LP 列爆炸），或改为也保留有 ≥2 存活 LP 键的原始关系。

#### [BUG] build_row_with_qc 的 build_row() 基调用不设 sign 列
- **发现日期**: 2026-03-14
- **文件**: `linalg/matrix_builder.hpp:582`
- **描述**: `build_row()` 注释说"sign 不在此设置"。但若直接调用 `build()`（非 `build_with_qc()`），sign 列永远为 0，即使 `include_sign_column = true`。sign 列错误的矩阵会导致 BL 找到非法依赖。
- **建议**: 在 `build()` 中添加 `assert(!config_.include_sign_column)` 或将 sign 计算移入 `build_row()`。

#### [BUG] Progressive 测试 XOR 组合验证了错误的依赖
- **发现日期**: 2026-03-14
- **文件**: `tests/test_gnfs_progressive.cpp:593`
- **描述**: XOR 组合回退中 `verify_dep(build_result.matrix, deps[i])` 验证的是原始 `deps[i]`（已在之前单独验证过），而非组合后的 `combined = deps[i] XOR deps[j]`。对比 `test_gnfs_e2e.cpp:661` 正确验证了 combined。腐败的组合依赖会不经验证直接进入 sqrt。
- **建议**: 改为 `verify_dep(build_result.matrix, combined)`。

#### [BUG] Miller-Rabin 7-witness 对 >3.4×10^14 仅概率性
- **发现日期**: 2026-03-14
- **文件**: `cofactor/smooth_check.hpp:83-93`
- **描述**: 注释声称 7-witness 集 {2,3,5,7,11,13,17} 覆盖全部 uint64，但 Jaeschke 1993 仅证明到 341,550,071,728,321 (~3.4×10^14)。对更大余因子（50-digit+ 的 LP^2 可达 ~10^18），测试是概率性的。Jim Sinclair 全 uint64 确定性需要 {2,3,5,7,11,13,17,19,23,29,31,37}（12 witnesses）。
- **建议**: else 分支增加 witnesses 19,23（至少 9 个）或使用完整 Sinclair 集。

#### [BUG] BucketEntry::offset uint16_t 对宽筛区溢出
- **发现日期**: 2026-03-14
- **文件**: `sieve/lattice_sieve.hpp:235-238, 416`
- **描述**: `BucketEntry::offset` 是 `uint16_t`（max 65535）。极端 skewness（如 1,000,000）下 `i_width` 可达数百万，offset 截断导致 bucket 素数写入错误列位置，进而 `sieve_array_[row_base + entry.offset]` 可能越界写入。
- **建议**: 改用 `uint32_t`，或在 `default_sieve_region` 中硬上限 `i_width ≤ 65535`。

#### [BUG] ModularPoly::add() 大素数 uint64 溢出
- **发现日期**: 2026-03-14
- **文件**: `sqrt/modular_poly.hpp:79`
- **描述**: `uint64_t sum = a.coeff(i) + b.coeff(i);` 当 p ≥ 2^63 时，两个 [0,p-1] 范围的系数相加溢出 uint64_t。当前 Hensel 使用小素数（~10-30 bit）不会触发，但类型契约错误。
- **建议**: 用 `__uint128_t` 中间和或无溢出形式：`(ai >= p - bi) ? ai - (p - bi) : ai + bi`。

#### [BUG] norm_linear 与 algebraic_norm 符号不一致
- **发现日期**: 2026-03-14
- **文件**: `sqrt/number_field.hpp:421-425` vs `core/polynomial_context.hpp:156-179`
- **描述**: `NumberField::norm_linear()` 无条件取绝对值返回 |N|，但 `PolynomialContext::algebraic_norm()` 返回带符号 N。奇次多项式负范数时两函数结果不同。调用者混用两函数会得到不一致的符号。
- **建议**: 统一符号约定或在 `norm_linear()` 中保留符号，让调用者决定是否取绝对值。

#### [BUG] RelationCollector::merge() 和 load() 跳过 validate()
- **发现日期**: 2026-03-14
- **文件**: `relation/collector.hpp:229-242, 250-263`
- **描述**: `merge()` 从其他 collector 复制关系时不调 `validate()`——b=0 或 gcd(|a|,|b|)≠1 的无效关系绕过全部验证。`load()` 从磁盘反序列化时同理。`add()` 正确调用了 `validate()`，但这两个路径遗漏。
- **建议**: 在 `merge()` 和 `load()` 的内循环中添加 `validate()` 调用。

#### [BUG] compute_product_at_m uint64_t b 转 int64_t UB
- **发现日期**: 2026-03-14
- **文件**: `sqrt/hensel_sqrt.hpp:182`
- **描述**: `bm *= Integer(static_cast<int64_t>(b));` 当 b > INT64_MAX 时是未定义行为。项目已有 `Integer(uint64_t)` 构造函数（Session 12 添加），应直接使用。
- **建议**: `bm *= Integer(b);`

#### [BUG] Schirokauer compute_unsplit 缺少非单位元素防御
- **发现日期**: 2026-03-14（第三轮修正：P1→P2，因 gcd(|a|,|b|)=1 约束使其不可达）
- **文件**: `linalg/schirokauer.hpp:628`
- **描述**: `compute_unsplit` 不检查 `g = a - bα` 是否为 ℓ-adic 单位。若两系数同时为 ℓ 倍数则 `(0-1)/ℓ mod ℓ` 产生错误值。但 `gcd(|a|,|b|) = 1` 保证对素数 ℓ 不会同时整除。与 `compute_split`（有 ℓ-stripping）不一致。
- **建议**: 添加 `assert` 或与 split 路径统一。

#### [BUG] algebraic_norm_i128 溢出检查跳过 degree < 3
- **发现日期**: 2026-03-14
- **文件**: `core/polynomial_context.hpp:228-240`
- **描述**: 溢出保护只对 `max_val > 1 && degree_ >= 3` 生效。degree=1 或 2 且系数大（60-bit）时，`ci * a_power * b_pow` 可溢出 `__int128` 而不被捕获。
- **建议**: 将溢出检查扩展到所有 degree。

### 性能问题

#### [OPT] ECM sieve_primes 每条曲线重新分配
- **发现日期**: 2026-03-14
- **文件**: `cofactor/ecm.hpp:408-427`
- **描述**: `try_curve` 中 `sieve_primes(B1)` 为每条曲线分配 ~375KB `vector<bool>` 和全部素数列表。B1=3M, 200 曲线 = 200 次筛分配。
- **建议**: 在 `factor()` 中预计算一次并复用。

#### [OPT] Sieve small_primes 每线程每 SQ 重建
- **发现日期**: 2026-03-14
- **文件**: `sieve/lattice_sieve.hpp:513-535`
- **描述**: `sieve_row_chunk` 每次调用重建 `small_primes` 和 `v_primes`，O(FB_size) per thread per SQ。4 线程 × 10K 素数 = 40K 次分离/SQ。
- **建议**: 在 `sieve_row_major()` 中预分离一次，传引用给各线程。

#### [OPT] classify_cofactor 试除遍历合数
- **发现日期**: 2026-03-14
- **文件**: `cofactor/smooth_check.hpp:337-342`
- **描述**: 对 c < 2^32 的试除从 101 开始遍历所有奇数至 sqrt(c)，约 60% 是合数（永远不会整除已通过小素数检测的 c）。
- **建议**: 使用预生成的 [101, 65537] 素数表代替全奇数遍历。

#### [OPT] algebraic_norm 热路径每次 heap 分配 GMP vector
- **发现日期**: 2026-03-14
- **文件**: `core/polynomial_context.hpp:156-159`
- **描述**: `algebraic_norm()` 每次调用分配 `vector<Integer>(degree_+1)`，每个 Integer 调用 `mpz_init`。当 i128 快速路径溢出时，此函数在筛法热路径中被调用。
- **建议**: 使用 `SmallVector` 或 `std::array` 栈缓冲。

#### [OPT] Logger log_args 检查级别前构建字符串
- **发现日期**: 2026-03-14
- **文件**: `util/logger.hpp:146-152`
- **描述**: `log_args()` 先构建 `ostringstream` 字符串再调 `log()` 检查级别。热路径中 Debug 级别被禁用时仍支付字符串构建开销。
- **建议**: 在 `log_args` 入口检查 `is_enabled(level)` 后再构建字符串。

#### [OPT] MurphyEvaluator 默认分配 10M 筛
- **发现日期**: 2026-03-14
- **文件**: `polynomial/murphy_evaluator.hpp:233`
- **描述**: 默认 `alpha_bound = 1e7`，构造时分配 ~1.25MB `vector<bool>` + 62 万素数。`compute_alpha` 遍历全部素数，大多贡献微不足道。
- **建议**: 降低默认 alpha_bound 或为热路径提供较小的 bound 参数。

#### [OPT] Murphy E-score 内循环用 std::pow
- **发现日期**: 2026-03-14
- **文件**: `polynomial/murphy_evaluator.hpp:381-384`
- **描述**: 2000 × degree 次 `std::pow` 调用，可用迭代乘法替代。`skewness_pow[j]` 在角度循环外是常量。
- **建议**: 预计算 skewness 幂次，内循环用迭代累乘。

### 设计问题

#### [BUG] Relation::b 类型 int64_t 与 uint64_t 不一致
- **发现日期**: 2026-03-14
- **文件**: `core/relation.hpp:20`, `core/types.hpp:17`, `cofactor/cofactorizer.hpp:129`
- **描述**: `Relation::b` 是 `int64_t`，但 `ABPair::b` 和 `SieveCandidate::b` 是 `uint64_t`。GNFS 约定 b > 0。类型不一致导致每个传递点都有隐式窄化转换风险。`rel.ab()` 用 `safe_abs(b)` 恢复——暗示 b 可以为负，但语义上不应该。
- **建议**: 统一 `Relation::b` 为 `uint64_t`。

#### [BUG] IntPolynomial::discriminant() 对 degree > 2 静默返回 0
- **发现日期**: 2026-03-14
- **文件**: `polynomial/int_polynomial.hpp:308-335`
- **描述**: degree > 2 时返回 `Integer(0)`，而 0 是有效判别值（表示有重根）。调用者无法区分"未实现"和"判别式确实为零"。
- **建议**: `throw std::logic_error("discriminant not implemented for degree > 2")` 或返回 `std::optional<Integer>`。

#### [DEBT] translate/derivative 在 IntPolynomial 和 PolynomialOptimizer 间重复
- **发现日期**: 2026-03-14
- **文件**: `polynomial/int_polynomial.hpp:230,251` + `polynomial/polynomial_optimizer.hpp:118,139`
- **描述**: `translate()` 和 `derivative()` 两处实现完全相同。未来修复一处不会反映到另一处。
- **建议**: 统一到一处，另一处委托调用。

#### [DEBT] generate_smooth_coefficients 缺少去重
- **发现日期**: 2026-03-14
- **文件**: `polynomial/kleinjung_selector.hpp:501-527`
- **描述**: 光滑系数生成后只排序不去重（对比 `PolynomialOptimizer::generate_smooth_numbers` 有 `std::unique`）。重复候选流经整个 Stage 1+2 管线，浪费计算。
- **建议**: 排序后添加 `result.erase(std::unique(...), result.end())`。

### 基础设施

#### [BUG] Class Group Characters 实现仅支持 Cubic Fields
- **发现日期**: 2026-03-13
- **文件**: `sqrt/class_group.hpp`
- **描述**: `class_group.hpp` 的 Minkowski bound、signature、character computation 均假定 degree=3。对 degree≥4 产生错误列值，导致所有 BL 依赖在 sqrt 阶段失败。当前已全局禁用 (`include_class_group=false`)，QC+Schirokauer 足够替代。如需恢复 class group 功能需全面重写。
- **建议**: 若要支持，需正确处理任意 degree 的签名 (r1,r2)、SNF、character computation。参考 PARI/GP 或 SageMath 的实现。

#### [FEAT] CMake 缺少 Sanitizer 支持
- **发现日期**: 2026-03-14
- **文件**: `CMakeLists.txt:18-40`
- **描述**: 项目有多线程代码和复杂内存管理，但无 ASan/TSan/UBSan 构建选项。这些 sanitizer 能捕获 use-after-move、数据竞争等整类 bug。
- **建议**: 添加 `GNFS_ENABLE_ASAN`, `GNFS_ENABLE_TSAN` CMake 选项。

---

## P3 — 低优先级

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

#### [OPT] Bucket Sieve for B > 500K (80+ digit 必需)
- **发现日期**: 2026-03-12
- **描述**: 80-digit (264 bit, degree 5) 实测 15 rels/s，需 11 天完成筛法。主因：lattice sieve O(sieve_entries × sieve_area) 对 149K 条目 × 33M 位置太慢。
  CADO-NFS 用 bucket sieve：按 log_p 桶分配、顺序写入，O(hits) 而非 O(entries×area)。
  还需 Kleinjung poly selection（降低有效范数 10-100×）。
- **建议**: 实现 2 级 bucket sieve (small primes 直接筛, large primes 用 bucket)。参考 CADO-NFS `las/` 或 msieve `sieve/`。

### 代码质量

#### [DEBT] 零大小筛区对极端 skewness
- **发现日期**: 2026-03-14
- **文件**: `sieve/lattice_basis.hpp:196-214`
- **描述**: 极端 skewness 下 `i_half` 可能舍入为 0，导致 `i_width() = 0`，筛法静默产生零候选。
- **建议**: 添加后置条件检查并回退到最小区域。

#### [DEBT] Hensel verbose 路径 f_lead 不可逆时 assert 崩溃
- **发现日期**: 2026-03-14
- **文件**: `sqrt/hensel_sqrt.hpp:979-980`
- **描述**: verbose 模式下 `poly_mul_mod(S, S, f_int, d, modulus)` 重新计算 `f_lead_inv`，若 `f[d]` 对 lifted modulus 不可逆则 assert 崩溃。仅影响 verbose 输出，不影响正确性。
- **建议**: 将 lift 循环中已计算的 `fli` 提升到函数作用域，或用 try/catch 包裹。

#### [DEBT] ECM stage2_naive batch 边界因子遗漏
- **发现日期**: 2026-03-14
- **文件**: `cofactor/ecm.hpp:594-619`
- **描述**: `g == n` fallback 后 `batch_primes.clear()` 清除了包含当前素数的列表。若因子恰好在批次边界素数处，retry 循环可能漏检。
- **建议**: fallback 后确保当前素数也被单独检查。

#### [DEBT] BL gauss_bytes size_t 溢出对超大矩阵
- **发现日期**: 2026-03-14
- **文件**: `src/linalg/block_lanczos.cpp:556`
- **描述**: `m * ((m + n + 63) / 64) * sizeof(uint64_t)` 对 m=n≈4.6M 溢出 `size_t`。当前 GNFS 矩阵不到这个规模，但无保护。
- **建议**: 中间计算用 `uint64_t` 显式类型转换。

### 风格 & 一致性

#### [DEBT] 命名空间风格不一致
- **发现日期**: 2026-03-14
- **文件**: 多个（polynomial_context.hpp, int_polynomial.hpp, gauss.hpp, factor_base.hpp 等）
- **描述**: 部分文件用 `namespace gnfs { namespace core {` (C++98)，部分用 `namespace gnfs::core {` (C++17)。项目标准为 C++20。
- **建议**: 统一为 C++17 嵌套命名空间语法。

#### [DEBT] relation.hpp 包含完整 `<iostream>` 而非 `<iosfwd>`
- **发现日期**: 2026-03-14
- **文件**: `core/relation.hpp:8`
- **描述**: `<iostream>` 在广泛包含的头文件中引入不必要的编译开销。`serialize`/`deserialize` 只需 `std::ostream&` 和 `std::istream&`。
- **建议**: 替换为 `#include <iosfwd>` 或将方法移至 `.cpp`。

#### [DEBT] params.hpp 魔法数 0.30103
- **发现日期**: 2026-03-14
- **文件**: `core/params.hpp:68`
- **描述**: `n_bits * 0.30103` 是 `log10(2)` 的近似值，应使用命名常量。
- **建议**: `constexpr double LOG10_2 = 0.30103;` 或用 `std::log10(2.0)`。

#### [DEBT] MurphyParams::seed 遗留未使用字段
- **发现日期**: 2026-03-14
- **文件**: `polynomial/murphy_evaluator.hpp:44`
- **描述**: `uint32_t seed = 42;` 注释标为 "legacy, unused" 但仍是公开 API。
- **建议**: 删除或标记 `[[deprecated]]`。

#### [DEBT] SparseRow::xor_with 使用 const_cast 而非 mutable
- **发现日期**: 2026-03-14
- **文件**: `linalg/sparse_matrix.hpp:100`
- **描述**: 对 `const SparseRow& other` 使用 `const_cast` 调用 `ensure_sorted()`。应将 `sorted_` 标记为 `mutable`，`ensure_sorted()` 改为 `const`。
- **建议**: `mutable bool sorted_;` + `void ensure_sorted() const`。

### 潜在风险（需进一步调查）

#### [RISK] Block Lanczos 三步递推与 Montgomery 1995 不一致
- **发现日期**: 2026-03-14
- **文件**: `src/linalg/block_lanczos.cpp:479-499`
- **描述**: 代码使用包含 `V_pprev`（前两步向量）的三步递推，而 Montgomery 1995 论文只使用两步递推（V_cur 和 V_prev）。额外的 `F_cur = V_pprev^T · B · V_cur` 项和 `D_pprev * F_cur` 应用没有已知数学基础。然而所有测试（L1-L5, 25-digit, stress）均通过，可能是有效的变体或冗余项。
- **建议**: 对比 Montgomery 1995 §3 公式逐项核实。若额外项冗余（恒等于零），则为死代码可移除。若实际影响结果，需确认数学正当性。

#### [RISK] Schirokauer compute_unsplit docstring 引用旧公式 (Bug #3)
- **发现日期**: 2026-03-14
- **文件**: `linalg/schirokauer.hpp:529-531`
- **描述**: 注释写 `λ_ℓ(γ) = (γ^(ℓ^(k-1)(ℓ-1)) - 1) / ℓ^(k-1) mod ℓ`——这是 Session 3 修复的旧错误公式 (Bug #3)。代码实际使用正确的 `ℓ^d - 1` 公式，但注释误导维护者和测试编写者。
- **建议**: 更正注释为 `λ_ℓ(γ) = (γ^(ℓ^d - 1) - 1) / ℓ mod ℓ`。

#### [DEBT] Pollard rho backtrack 无迭代上限
- **发现日期**: 2026-03-14
- **文件**: `cofactor/smooth_check.hpp:256-264`
- **描述**: `d == n` 时的 backtrack 循环 `while (d == 1)` 无步数上限。若输入是素数幂（`is_perfect_power` 未捕获的边缘情况），backtrack 可能长时间运行直到 cycle 完成。
- **建议**: 添加 `backtrack_steps < BATCH_SIZE * 2` 上限。

#### [DEBT] LP merge 顺序因 unordered_map 不确定
- **发现日期**: 2026-03-14
- **文件**: `relation/filter.hpp:344-359`
- **描述**: Phase 1 和 Phase 2 遍历 `unordered_map`，迭代顺序依赖实现和 ASLR。相同输入不同运行产生不同 LP 配对，导致合并后关系数不确定，影响可重复性。
- **建议**: 排序 key 后处理，或使用 `std::map`。

#### [DEBT] try_verify 累积无中间取模
- **发现日期**: 2026-03-14
- **文件**: `sqrt/hensel_sqrt.hpp:613-621`
- **描述**: `c *= mpow[j]` 后 `val += c` 不做中间 `val %= n`。d=6 时 val 可达 6·N² 再做最终取模。GMP 无溢出，但中间值不必要地大，影响乘法性能。
- **建议**: 每次 `val += c` 后添加 `val %= n`。

#### [DEBT] BlockVector::xor_with 无长度检查
- **发现日期**: 2026-03-14
- **文件**: `linalg/block_lanczos.hpp:41-44`
- **描述**: `xor_with(other)` 用 `this->length` 做循环上界但不检查 `other.length >= length`。当前所有调用者用等长向量，但无强制保证。
- **建议**: 添加 `assert(other.length >= length)`。

### Dead Code

#### [DEBT] params.hpp print_summary() 全空 no-op
- **发现日期**: 2026-03-14
- **文件**: `core/params.hpp:348-356`
- **描述**: 定义了 `print_size` lambda 后立即 `(void)print_size`，函数体无任何输出。
- **建议**: 实现功能或删除。

#### [DEBT] BL header 串行 SpMV 函数未使用
- **发现日期**: 2026-03-14
- **文件**: `linalg/block_lanczos.hpp:185-205`
- **描述**: `spmv_forward` 和 `spmv_transpose` 串行版本从未被调用，`.cpp` 只使用并行版本。
- **建议**: 删除。

#### [DEBT] factor_base compute_log_prime 非精确版未使用
- **发现日期**: 2026-03-14
- **文件**: `factor_base/factor_base.hpp:189-195`
- **描述**: 基于 `__builtin_clz` 的粗略版本从未被调用，所有实际代码使用 `compute_log_prime_precise`。
- **建议**: 删除或标记 `[[deprecated]]`。

#### [DEBT] PolynomialOptimizer golden_section_skewness 未使用
- **发现日期**: 2026-03-14
- **文件**: `polynomial/polynomial_optimizer.hpp:243`
- **描述**: 黄金分割搜索法定义但未被调用（Murphy 用网格搜索）。
- **建议**: 删除。

---

## TEST — 测试质量缺口

### [TEST] test_schirokauer_deg4 未注册到 test.sh
- **发现日期**: 2026-03-14
- **文件**: `CMakeLists.txt:374`, `scripts/test.sh`
- **描述**: 二进制已构建并注册到 ctest（名字还有 typo: SchirokaurDeg4），但完全缺失于 `test.sh` 的 `ALL_TEST_BINARIES`、`MODULE_TESTS`、`TEST_TIMEOUT`、`TEST_TIER` 中。
- **建议**: 添加到 test.sh 各配置项，修复 ctest 名称 typo。

### [TEST] test_linalg rank 断言过于宽松
- **发现日期**: 2026-03-14
- **文件**: `tests/test_linalg.cpp:261-262`
- **描述**: 已知 rank=3 的 5×4 矩阵，断言却接受 rank ∈ [2,4]。高斯消元回归到 rank=2 或 rank=4 都不会被捕获。
- **建议**: `assert(result.rank == 3);`

### [TEST] Schirokauer 回归测试验证公式而非生产代码
- **发现日期**: 2026-03-14
- **文件**: `tests/test_regressions.cpp:54-83, 276-313`
- **描述**: `test_schirokauer_exponent` 和 `test_schirokauer_ell2_only` 在局部变量中计算数学公式并断言——从未调用 `schirokauer.hpp` 中的任何函数。如果生产代码回退到旧公式，测试仍通过。
- **建议**: 设置 PolynomialContext + 已知 (a,b) 对，调用生产代码计算实际 Schirokauer map 值并断言。

### [TEST] test_concurrent_add 断言空洞
- **发现日期**: 2026-03-14
- **文件**: `tests/test_relation_collector.cpp:204`
- **描述**: 4 线程各插入 100 个关系（键唯一），期望 400 个，但断言只检查 `size() > 0`。即使 399 个丢失也通过。
- **建议**: `assert(collector.size() == 400);`

### [TEST] test_murphy alpha 符号和 score 数值未断言
- **发现日期**: 2026-03-14
- **文件**: `tests/test_murphy.cpp:63-68, 97-110`
- **描述**: alpha 应为负值（x^5-1 小素数整除性高于平均），但只断言 `isfinite`。score consistency 同理——只断言 `isfinite` 不检查数值接近。
- **建议**: `assert(alpha < 0.0)` + 相对误差检查。

### [TEST] test_25digit/test_stress 对 ctest 不可见
- **发现日期**: 2026-03-14
- **文件**: `CMakeLists.txt:384-395`
- **描述**: 二进制编译但无 `add_test()`，`ctest -N` 看不到。`scripts/test.sh` 独立知道路径，形成隐藏依赖。
- **建议**: 用 `DISABLED` 属性注册到 ctest 以提高可发现性。

### [TEST] Progressive 测试 XOR 组合验证逻辑错误
- **发现日期**: 2026-03-14
- **文件**: `tests/test_gnfs_progressive.cpp:593`
- **描述**: 与 P2 中同名条目对应。在 TEST 分类下强调：即使 progressive 测试 return 0 被修复，XOR 组合回退路径也存在验证逻辑错误，会允许无效依赖进入 sqrt 阶段。
- **建议**: 见 P2 条目。

### [TEST] test_sqrt 时序断言易 flaky
- **发现日期**: 2026-03-14
- **文件**: `tests/test_sqrt.cpp:633-634`
- **描述**: 30s 内部超时与 test.sh 的 10s 外部超时不匹配。应减至 5s 以快速捕获无限循环回归。
- **建议**: `assert(elapsed_ms < 5000)` 并确保 test.sh 超时 ≥ 10s。

---

## FEAT — 80/100-digit Scalability
- **描述**: 已用 CADO-NFS 校准参数 (C80: B=1M/2M, C100: B=8M/16M)。矩阵大小合理但筛法太慢。需 bucket sieve + Kleinjung 才能在合理时间完成
