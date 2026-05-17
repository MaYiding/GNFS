# GNFS Project — Claude Code Instructions

## Project Overview

Industrial-grade **General Number Field Sieve (GNFS)** implementation in C++20.
Factorizes large composite integers using the most powerful known classical algorithm.

## Build System

```bash
# 配置 (首次或 CMakeLists.txt 改动后)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# 编译
make -C build -j$(sysctl -n hw.ncpu)

# 运行全部测试
cd build && ctest --output-on-failure

# 运行单个测试
./build/test_gnfs_e2e    # 端到端测试 (最重要)
./build/test_linalg      # 线性代数测试
./build/test_sqrt        # 平方根测试
```

**编译依赖:** GMP (必需), NTL (可选), Metal (macOS 可选), pthreads

## 自动化测试工作流 (`scripts/test.sh`)

统一入口脚本，**自带超时机制**（zsh 原生实现，不依赖 GNU coreutils），自动编译 + 运行 + 报告。

### 快速参考

```bash
# ── 日常开发 (最常用) ──
./scripts/test.sh                      # 冒烟测试: 23 个 instant 测试, ~5s (Debug) / ~5s (Release)
./scripts/test.sh smoke                # 同上
./scripts/test.sh changed              # 根据 git diff 自动选择受影响模块
./scripts/test.sh changed --deep       # 同上 + 级联依赖模块

# ── 模块级 ──
./scripts/test.sh module linalg        # 只跑线性代数模块
./scripts/test.sh module sieve sqrt    # 多模块
./scripts/test.sh module all           # 全部模块 (仅 instant+fast 测试)
./scripts/test.sh module all --slow    # 全部模块 (含 slow+heavy 测试)

# ── 单个测试 ──
./scripts/test.sh run test_linalg      # 指定测试二进制
./scripts/test.sh run sqrt             # 自动补 test_ 前缀

# ── 合并门禁 ──
./scripts/test.sh gate                 # 二级门禁: smoke + 回归 (17/27/40/81-bit) ~18s Debug / ~9s Release
./scripts/test.sh gate --quick         # 快速门禁: 仅 smoke ~5s

# ── E2E & 渐进 ──
./scripts/test.sh e2e                  # 完整 GNFS 流水线 (slow, ~5min)
./scripts/test.sh L1                   # 渐进式 Level 1 only
./scripts/test.sh progressive 1 3      # L1 到 L3

# ── 全量 ──
./scripts/test.sh full                 # ctest + E2E + Progressive L1-L2
./scripts/test.sh thorough             # 全模块 + 集成 + L1-L3
./scripts/test.sh nightly              # 全部 + L4 + L5 + stress L1 (过夜跑)

# ── 压力测试 ──
./scripts/test.sh stress               # 50/60-digit 全部
./scripts/test.sh stress 1 1           # 仅 50-digit (164 bit)
./scripts/test.sh stress 1 2           # 50 + 60-digit
./scripts/test.sh run test_stress 1 1  # 等价写法

# ── 工具 ──
./scripts/test.sh list                 # 查看所有测试、模块、超时、分级
./scripts/test.sh build                # 仅编译
./scripts/test.sh --no-build smoke     # 跳过编译直接跑
./scripts/test.sh -t Release full      # Release 模式
./scripts/test.sh --fail-fast full     # 首个失败即停
./scripts/test.sh --timeout 30 run test_kleinjung  # 自定义超时
./scripts/test.sh bench --save         # 性能基准 + 保存结果
./scripts/test.sh watch                # 监视文件变更自动重测 (需 fswatch)
```

### 测试分级 (基于实测数据)

| 分级 | 超时 | 测试 | 包含在 |
|------|------|------|--------|
| **instant** | 10s | test_integer, test_small_vector, test_thread_pool, test_factor_base, test_special_q, test_relation_collector, test_cofactor, test_linalg, test_sqrt, test_sqrt_debug, test_murphy | smoke, module, changed |
| **fast** | 60s | test_sieve_basic | module, changed |
| **slow** | 120-300s | test_regression_gate, test_kleinjung, test_lattice_sieve, test_factor_with_kleinjung, test_gnfs_e2e | gate, module --slow, e2e, full |
| **heavy** | 600-3600s | test_kleinjung_large, test_gnfs_progressive, test_25digit | progressive, nightly, bench |
| **stress** | 43200s | test_stress (L1=50-digit, L2=60-digit) | stress, nightly (L1 only) |

### 使用场景对照

| 场景 | 推荐命令 | 预计时间 |
|------|----------|----------|
| 改了一个函数，快速验证 | `./scripts/test.sh` | ~5s |
| 改了 linalg 模块 | `./scripts/test.sh module linalg` | ~1s |
| 改了核心流程，要 E2E | `./scripts/test.sh e2e` | ~5min |
| 不确定改了什么 | `./scripts/test.sh changed` | 自动判断 |
| 特性分支合并前验证 | `./scripts/test.sh gate` | ~9s (Release) |
| 大改动，全面回归 | `./scripts/test.sh full` | ~10min |
| PR 前最终验证 | `./scripts/test.sh thorough` | ~30min |
| 跑完整性能基准 | `./scripts/test.sh bench --save` | ~1hr |
| 验证大数分解能力 | `./scripts/test.sh stress 1 1` | 50-digit: ~2.6h |
| 极限压力测试 | `./scripts/test.sh stress` | 50+60-digit, 小时级 |

### 超时机制

- 每个测试有**分级默认超时** (instant=10s, fast=60s, slow=180-300s, heavy=600-3600s)
- `--timeout N` 可全局覆盖所有测试的超时秒数
- 超时后自动杀进程，显示 "TIMEOUT" + 最后 10 行输出
- 慢测试运行时每 10 秒打一次心跳 `[10s][20s]...` 表明进程还活着

### 特性分支工作流 (`scripts/feature-branch.sh`)

```bash
# 创建特性分支
./scripts/feature-branch.sh create feat bucket-sieve   # → feat/260315-bucket-sieve

# 开发完成后运行门禁
./scripts/feature-branch.sh gate                        # 委托给 test.sh gate

# 门禁通过后合并到 main
./scripts/feature-branch.sh merge                       # 自动运行门禁 + --no-ff merge

# 查看状态
./scripts/feature-branch.sh status                      # 分支概览
./scripts/feature-branch.sh list                        # 列出所有特性分支
```

## Architecture

```
include/gnfs/           # 53 头文件 (.hpp)
├── api/           (6)  # Config, Pipeline, Factorizer, i18n — 公开 API 层
├── core/          (6)  # Integer, Polynomial, Relation, Params, Types — 基础类型
├── polynomial/    (6)  # Kleinjung 选择, Murphy E, base-m, IntPolynomial, Optimizer, Dispatch
├── factor_base/   (2)  # 因子基构建 (Cantor-Zassenhaus 求根)
├── sieve/         (4)  # Lattice sieve, Line sieve, Special-Q, Lattice basis
├── cofactor/      (5)  # 余因子分解: 试除法, ECM, SQUFOF, 光滑性检查
├── relation/      (3)  # 关系收集, 过滤/合并 (1LP+2LP), OOC 存储
├── linalg/        (8)  # GF(2) 矩阵, Block Lanczos, Block Wiedemann, Gaussian, SGE, Schirokauer, Mmap CSR
├── sqrt/          (7)  # 代数平方根 (Nguyen Hybrid + Couveignes), 有理平方根, 类群, ModularPoly
└── util/          (6)  # SmallVector, ThreadPool, Logger, Timer, MmapFile, SafeMath

src/                    # 14 源文件 (.cpp)，按模块组织
├── api/           (2)  # pipeline.cpp, factorizer.cpp
├── cli/           (1)  # main.cpp — CLI 入口
├── core/          (3)  # integer, polynomial, relation
├── factor_base/   (1)  # builder (Cantor-Zassenhaus)
├── linalg/        (3)  # block_lanczos, block_wiedemann, matrix_builder
├── polynomial/    (1)  # base_m
├── sieve/         (1)  # lattice_sieve
└── sqrt/          (2)  # algebraic_sqrt, rational_sqrt

tests/                  # 41 测试文件 (.cpp)
```

## GNFS Pipeline

1. **Polynomial Selection** → Kleinjung 算法选择 f(x), g(x)（SelectorDispatch 自动选 base-m / Kleinjung）
2. **Factor Base** → 构建有理/代数因子基（并行 Cantor-Zassenhaus 求根）
3. **Sieving** → Lattice sieve with Special-Q（bucket sieve 用于大因子基，支持多线程 scatter）
4. **Cofactorization** → 试除 + SQUFOF + ECM (Stage 1+2 BSGS) + 光滑性验证
5. **Relation Collection** → 收集光滑关系，自适应多轮筛-过滤-合并
6. **Linear Algebra** → SGE 预处理 + GF(2) 矩阵 + Block Lanczos / Block Wiedemann 求零空间
7. **Square Root** → Nguyen Hybrid (Hensel lift + CRT) 优先，Couveignes 兜底
8. **GCD** → gcd(a ± b, N) 得到非平凡因子

## Critical Conventions

### 元素表示
**Elements are `a - b*α` (NOT `a + b*α`).** 这是整个代码库的基本约定。

### Schirokauer Maps
- 对于 GF(2) 矩阵，**只能使用 `schirokauer_primes = {2}`**
- ℓ > 2 的 Schirokauer maps 要求非 GF(2) 矩阵 (矩阵条目需 mod ℓ)

### 关系过滤
- 必须拒绝 `gcd(a - b*m, N) > 1` 的关系（这些关系会产生退化依赖，product ≡ 0 mod N）

### 整数类型
- 使用 `gnfs::core::Integer` 封装 GMP `mpz_class`
- 所有大整数运算通过 `Integer` 类完成

### Filter Merge — V0 + V3 Clique Cascade

**V0** (默认, `PartialRelationMerger::merge_all`): 仅 weight=2 LP keys. 适合 lp_bits ≤ 22.

**V3** (备用, `CliqueRelationMerger::merge_cliques`): BFS spanning tree over LP-sharing graph + LP cancel check. 处理 weight≥3 LP keys (50d/60d 的 lp_bits 23/26 真实 corner case).

**启用 V3 cascade** (三态 ENV, commit 56e5b14):
```bash
GNFS_CASCADE_V3=1     ./test_stress 1 1   # ON: V3 every round
GNFS_CASCADE_V3=auto  ./test_stress 1 1   # AUTO: V3 Round 2+ only (sieve), Phase 4 always
GNFS_CASCADE_V3=1     ./gnfs <N>          # GNFS pipeline V0+V3
unset GNFS_CASCADE_V3                     # OFF (default, V0 only)
```

V3 cascade 默认 OFF (V0 path 零开销). 启用时:
- V3 run on partial **copy** (V0 path 完整保留)
- Dedup via (a,b) XOR hash 避免 V0 ∩ V3 重复
- Auto 模式 Round 1 跳过 (LP overlaps 稀少, fast-path rejects 主导, ROI 低)
- stderr 输出 `[v3_cascade.sieve] in=... full=... residual=... added=...`

**集成点** (commits 7f9de82, 975ac8b, 56e5b14):
- `src/api/pipeline.cpp:39-58` — V3Mode enum + cascade_v3_enabled_for_round
- `src/api/pipeline.cpp:627` — sieve_and_collect adaptive loop (Auto-aware)
- `src/api/pipeline.cpp:747` — Phase 4 filter (always enable when not Off)
- `tests/test_stress.cpp:393` — stress sieve loop

**详细**: `docs/perf/v3-cascade-design.md`

**禁用条件**: V0 已 PASS 时不需要 V3 (额外开销但 0 收益). V3 仅在 V0+fix 50d/60d NO_EXCESS 时启用.

### lp_bits 实验 (GNFS_OVERRIDE_LP_BITS)

**ENV `GNFS_OVERRIDE_LP_BITS=N`** (commit `dce0a5e`, `e271c5a`): runtime override `params.hpp` digit-based lp_bits default. 范围 1-30. 不在范围则忽略 (default).

```bash
GNFS_OVERRIDE_LP_BITS=25 ./test_stress 2 2   # 60d with lp_bits=25 (vs default 26)
GNFS_OVERRIDE_LP_BITS=27 ./gnfs <N>          # any size with lp_bits=27
```

**用途**:
- BACKLOG [OPT] 60d lp_bits 25 vs 26 trade-off 比较
- LP space 影响 sieve duration: smaller lp_bits = smaller LP space = fewer LP cols = less raw needed for PASS (但 fewer LP cofactor candidates)
- 实验前后必须 reg-test 25d / 50d (lp_bits 不该影响 < 50d behavior, 默认 path unchanged)

### Thin matrix BW solve (GNFS_THIN_MATRIX_TRY)

**ENV `GNFS_THIN_MATRIX_TRY=1`** (BACKLOG #80 step 7, 2026-05-17):
当 matrix rows ≤ cols (NO_EXCESS), 跳过 pipeline 默认放弃, 让 Block Wiedemann
尝试 thin-matrix solve. BW 数学上能处理 m ≤ n case (找 left kernel of M via
B=M*M^T (m×m), 当 rank(M)<m 时存在). BL 在 thin 时已知 fail, 自动 skip.

```bash
GNFS_THIN_MATRIX_TRY=1 ./test_stress 1 1   # 50d 试 thin solve
GNFS_THIN_MATRIX_TRY=1 ./gnfs <N>          # any GNFS run
```

**默认 OFF**: 保持 prior "abort on no excess" behavior. 仅当用户 explicitly enable
时尝试 thin solve. **实验性** — BW historically 假设 m > n, thin path 未广泛验证.

**触发场景**: 50d β plateau (m=282K < n=365K). Pipeline 之前在 `has_excess()`
返回 false 时直接 return MatrixResult without calling solvers. 启用后 BW 会尝试.

### Drop-residual + weight-cutoff (BACKLOG #80 algorithmic breakthrough)

**ENV `GNFS_DROP_RESIDUAL=1`** (commits `da51e0b` + `b001606`, 2026-05-17):
V0 + V3 cascade 都 drop "merged-with-residual" partials (含残留 LP 的合并关系).
对 50d β plateau ~121% 的根因假设: residual partials 贡献 ~70% lp_cols.

```bash
GNFS_DROP_RESIDUAL=1 ./test_stress 1 1   # 50d 用 drop=1
GNFS_DROP_RESIDUAL=1 ./gnfs <N>          # any GNFS run
```

**ENV `GNFS_WEIGHT_CUTOFF=N`** (commit `0c8b745`, 2026-05-17):
Phase 2 dead 集合扩展, drop 任何 LP key weight > N 的关系. CADO-NFS purge.c 思路.
N=2 时 = "weight≤2 keys 才保留" (V0 mergeable subset).

```bash
GNFS_WEIGHT_CUTOFF=2 ./test_stress 1 1   # weight-3+ key 的关系全 drop
```

**用途与实测**:
- 单独 drop=1: gate (81-bit) 4/4 PASS 42s
- 单独 cutoff=2: gate 4/4 PASS 60s (adaptive loop 多 round 补偿删除)
- 50d 实测 background (PID 67047, log `/tmp/p10_drop_residual_50d.log`)
- 二者组合预期: β < 100% 必要条件 (待实测).
- 注意: drop 模式 V3 cascade 的 v3_added 可能 = 0 (V0 已覆盖 full, V3 残留全 drop), test_clique_merger_50d_synthetic 已 conditional skip assertion (line 253).

### Trim limit 必须含 LP cols (P1 BUG 模式, 防 50d/60d NO_EXCESS)

**所有 Phase 4 relation trim 必须使用 `effective_cols = matrix_cols + count_unique_lp_keys(relations)`,**
而非裸 `matrix_cols`。lp_bits ≥ 20 时 LP cols 占总 cols 60-70% (50d 实测 65%, 60d ~70%),
仅按 FB cols trim 会砍光 30-40% rows → matrix NO_EXCESS。

正确公式 (5 个 test entry points 一致):
```cpp
size_t lp_cols_for_trim = lp_enabled ? count_unique_lp_keys(relations) : 0;
size_t effective_cols = matrix_cols + lp_cols_for_trim;
size_t max_rels = effective_cols * 1.25;  // 25% safety; 1.3 for ≤25-digit gate paths
```

**触发**:
- test_stress.cpp (commit 71193bb 主犯, 50d V0+fix FAIL 根本原因)
- test_gnfs_progressive.cpp (commit ed8a7b5)
- test_regression_gate.cpp (commit 33d9a8f, latent)
- test_25digit.cpp (commit d7037ff, latent)
- pipeline.cpp Phase 5 (使用 actual matrix stats, 不受影响 — `compute_matrix_stats` 已含 LP cols)

**预防**: 任何新 test entry point 引入 Phase 4 trim 必须用 `effective_cols`。
`matrix_cols * N` 这个 pattern 在 lp_bits≥20 size 下永远是 BUG。

## 跨 bit-size 验证 (小 case PASS ≠ 大 case PASS)

**81-bit 测试 PASS ≠ 164-bit (50-digit) PASS ≠ 197-bit (60-digit) PASS。** GNFS 算法行为随 LP_bound (lp_bits) 显著变化:

- **lp_bits 20** (25-digit, ≤30-digit): weight=2 LP keys 主导 (BG birthday probability ~0.5+). weight≥3 keys 稀少 (~< 5%). 经验估计 `lp_cols / usable ≈ 5%` 有效.
- **lp_bits 23** (50-digit): LP 空间 8M, weight-3+ LP keys 大量 (~30%). chain-merge 累 LP residue → singleton 飙升. 经验估计 `lp_cols / usable ≈ 64%`.
- **lp_bits 26** (60-digit): LP 空间 67M, 大部分 LP key weight=1 (orphan). 经验估计 `lp_cols / usable ≈ 70%+`.

**典型陷阱**:
1. **算法修改在 25-digit PASS, 在 50-digit 失败** (本会话 V2 commit 21dcbcd 案例): V2 让 weight≥2 都 merge, 25d Merged +27% (好看), 50d Merged -69% + sngl ×49 (灾难). lp_bits=23 chain LP residue 累积 是真实 corner case.
2. **经验比例失效**: `lp_col_estimate = relations.size() / 20` 来自 ≤30-digit, 50d 实际 64% (×12.6). sieve loop 提前 break, matrix build NO EXCESS.

**铁律**:
- 改 filter/merge/sieve 等 size-sensitive 代码后, 必须 reg-test 至少**3 个 size band**: 81-bit (25d), 164-bit (50d 至少 1 Round), 100-150 bit (e.g. test_kleinjung_large).
- 经验估计 (`/ 20`, `× 1.5`, etc.) 都标 "size 边界" 注释, 触发 size 改变时重 audit.
- 准确计算 > 经验估计. `count_unique_lp_keys` 在 filter.hpp 提供准确 LP cols 数, **deprecate 5% guess**.

## 跨平台编译注意事项 (macOS / Linux CI)

**本地 macOS 编译通过 ≠ Linux CI 通过。** 常见差异:

1. **STL 头隐式包含**:Apple libc++ 隐式拉 `<optional>/<stdexcept>/<array>/<memory>/<string>/<atomic>/<chrono>/<cstring>/<iostream>/<iosfwd>`,Linux libstdc++ 严格 — 用就显式 `#include`。
2. **64-bit 整数 typedef 差异**:Linux LP64 下 `int64_t == long`、`uint64_t == unsigned long`;macOS 下 `int64_t == long long`、`uint64_t == unsigned long long`。任何 `static_cast<(unsigned) long long>(...)` 传给重载函数(如 `Integer(int64_t)/Integer(uint64_t)`)在 Linux 会歧义。修法:加约束模板 `requires (std::is_same_v<T, long long> && !std::is_same_v<long long, int64_t>)`,只在 Linux 启用,委托给 `int64_t` 版本(`Integer` 已有此模式)。
3. **Release 优化掉 UB**:`double → uint64_t` 当 double 超出 `[0, 2^64)` 是 UB。Release CI 不触发,但 Sanitizers (Debug+UBSan) 会抓。任何 cast 前先 `std::min(double_val, 1e18)` clamp。
4. **测试期望被 NDEBUG 静默优化**:Release `assert()` 失效,所以期望本身错的测试看似通过,Debug + sanitizers 才暴露。最常见的模式是测试假设小 N 走完整 GNFS 流水线,但 `select_method` 把 ≤20-bit N 路由到 TrialDivision 快速路径(不调 progress callback,不累加 relations_found)。写测试断言时验证"路径选择"(`method_used == TrialDivision`) 比验证"流水线产生了副作用"(`relations_found > 0`) 更稳定。本地用 `cmake -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"` 复现 CI 失败。

## Code Style

- C++20 标准，使用 `std::optional`, `std::span`, concepts
- Header-heavy 设计：大部分实现在 `.hpp` 中（模板和内联）
- 命名: `snake_case` 用于函数和变量, `PascalCase` 用于类型
- Namespace: `gnfs::core`, `gnfs::linalg`, `gnfs::sieve`, `gnfs::api` 等

## Shell 脚本规范（`scripts/test.sh`）

### CJK 显示宽度

终端中 CJK（中日韩）字符占 **2 列**宽，而 `${#str}` 只计字符数（1）。凡涉及对齐、框线、表格的输出，**必须**用 `display_width()` 计算显示宽度，而非 `${#str}`。

```bash
# ✅ 正确 — 用 display_width 计算终端列宽
local -i w=$(display_width "$msg")
local -i pad=$(( 48 - w ))

# ❌ 错误 — ${#msg} 对 CJK 少算一半
local pad=$(( 48 - ${#msg} ))
```

`display_width()` 已在 `test.sh` 中定义，基于 zsh `$((#ch))` 取 Unicode codepoint，`>U+2FFF` 按 2 列计。

### Box-drawing 对齐清单

修改 `log_header` / `show_summary` 等框线输出时，确认：
- [ ] 用 `display_width` 而非 `${#str}` 计算填充
- [ ] 每行内容 + 填充 = box 内宽（当前 50 列）
- [ ] 所有内容行都有右边框 `║`
- [ ] 测试: `./scripts/test.sh smoke` 肉眼检查框线对齐

## Performance-Critical Code

- `PackedGF2Matrix`: 64-bit word-packed，O(1) 位访问
- `FastPoly`: uint64_t 快速多项式算术（Schirokauer maps 专用）
- Couveignes: 预计算期望乘积，65536 模式搜索（Nguyen Hybrid 优先路径）
- Block Lanczos: 64-bit block 并行 SpMV (ThreadPool)
- Block Wiedemann: Krylov+Gaussian 三阶段零空间求解 (Coppersmith 1994)
- SGE: 结构化 Gaussian 预处理，weight-1/2 消元，矩阵缩减 30-60%
- SQUFOF: 2-word 余因子分解（替代 Pollard rho，10-100× 更快）
- Bucket sieve: 大因子基多线程 scatter，按 region 分桶减少 cache miss
- OOC 基础设施: MmapCSRMatrix（矩阵）+ OOCRelationStore（关系），支持超内存规模
- 并行 FB 构建: Cantor-Zassenhaus 多线程求根 + `mpz_divisible_ui_p`

## Testing

**优先使用 `scripts/test.sh`**（见上方「自动化测试工作流」），它封装了编译、超时、报告的全部逻辑。

- **日常开发**: `./scripts/test.sh` (冒烟, ~5s) 或 `./scripts/test.sh changed` (自动检测)
- **模块改动**: `./scripts/test.sh module <模块名>` (如 linalg, sqrt, sieve)
- **核心改动**: `./scripts/test.sh e2e` (完整 GNFS 流水线)
- **PR 前**: `./scripts/test.sh full` 或 `./scripts/test.sh thorough`
- **注意超时**: slow 测试 (kleinjung, lattice_sieve, gnfs_e2e) 可能需要数分钟，脚本自带超时保护
- 测试框架：自定义 assert 宏（非 GoogleTest/Catch2）
- 查看全部测试列表: `./scripts/test.sh list`

### CI 上跑的测试子集
- GitHub runners (2-4 vCPU) 跑不动 slow/heavy/stress 测试,会超时
- `.github/workflows/ci.yml` 用 `ctest --label-exclude "slow|heavy|stress" --parallel N`
- 新增测试在 CMakeLists.txt 必须打 LABELS:`set_tests_properties(<Name> PROPERTIES LABELS "<tier>" TIMEOUT <s>)`,tier 与 `scripts/test.sh` 的 TEST_TIER 保持一致
- Sanitizers 还排除 `gate` label 和 `^(Integration|API)$`(后者是 Debug 下 pre-existing assert,见 BACKLOG.md)

## CI 调试常用命令

- `gh run list --branch <branch> --limit 5` — 查近期 CI 运行
- `gh run view <id> --log-failed` — 只打印失败 step 的日志(全 log 太大)
- `gh pr checks <num> --json name,bucket,state` — JSON 监控 PR 所有 check
- 用 `Monitor` 工具配 `gh pr checks` 轮询,事件驱动等 CI 完成,不要 sleep 循环

## Git 规范

### 初始化

```bash
git init
git add -A
git commit -m "chore: initial commit — GNFS project"
```

### 自动提交策略（已授权）

**Claude Code 被授权在每一小步改动后自动提交**，无需额外确认。
Commit 的核心目的是**状态记录和进度管理**，而非仅仅是"完成一个功能"。

- **粒度极细**：每做一小步就 commit — 改一个函数、修一个编译错误、加一个测试、调整一个参数，都应该立即 commit
- **一 Bug 一 Commit（强制）**：每个独立 Bug 修复必须是单独的 commit，即使多个 Bug 在同一个文件中。不允许将多个不相关的 Bug 修复打包到一个 commit 里。唯一例外：两个 Bug 的修复代码**在物理上交织**（改同一段代码、互为前置条件）时可以合并。同一分支内的多个 Bug commit 是正常的——分支可以共享，但 commit 不能混合
- **不需要等编译全部通过**：修了一个编译错误就可以 commit（即使还有其他编译错误）。目的是记录进度，不是证明完成度
- **改动大小不限**：即使只改一行也要 commit，这样出错时能精确 `git diff` 定位问题
- **禁止提交敏感文件**：不提交 `.env`、credentials、私钥等

### Commit Message 格式

采用 **Conventional Commits** 规范：

```
<type>(<scope>): <简短描述>

[可选正文：解释 why，而非 what]

Co-Authored-By: Claude <noreply@anthropic.com>
```

**type 取值：**

| type | 用途 | 示例 |
|------|------|------|
| `feat` | 新功能 | `feat(sieve): add bucket sieve for large factor bases` |
| `fix` | Bug 修复 | `fix(sqrt): prevent Hensel p^d overflow with Integer` |
| `perf` | 性能优化 | `perf(lanczos): parallelize SpMV with 12 threads` |
| `refactor` | 重构（不改行为） | `refactor(core): extract polynomial context` |
| `test` | 测试增删改 | `test(linalg): add GF(2) matrix edge case tests` |
| `chore` | 构建/工具/配置 | `chore: update CMakeLists for NTL optional` |
| `docs` | 文档 | `docs: update CLAUDE.md with git conventions` |

**scope 取值**（对应目录）：`core`, `polynomial`, `factor_base`, `sieve`, `cofactor`, `relation`, `linalg`, `sqrt`, `util`, `api`, `cli`

### 分支规范

#### 核心原则

**`main` 必须始终可编译、可测试通过。** 如果当前 `main` 能跑，任何新工作都必须在新分支上进行，直到编译通过、测试通过后再合并回 `main`。合并后的分支**保留不删除**，作为历史记录。

#### 命名格式

分支名包含**日期前缀**，格式：`<type>/<YYMMDD>-<简短描述>`

| 类型 | 格式 | 示例 |
|------|------|------|
| `main` | 稳定主线，始终可编译通过 | — |
| `feat/` | `feat/YYMMDD-描述` | `feat/260308-bucket-sieve` |
| `fix/` | `fix/YYMMDD-描述` | `fix/260308-hensel-overflow` |
| `perf/` | `perf/YYMMDD-描述` | `perf/260308-lanczos-simd` |
| `refactor/` | `refactor/YYMMDD-描述` | `refactor/260308-extract-poly-ctx` |
| `exp/` | `exp/YYMMDD-描述` | `exp/260308-neon-sieve` |

#### 分支生命周期

```
main (稳定，能跑)
  ↓ git checkout -b feat/260308-bucket-sieve
feat/260308-bucket-sieve
  ↓ 开发中：频繁 commit，允许编译不通过
  ↓ 完成：编译通过 + 测试通过
  ↓ git checkout main && git merge --no-ff feat/260308-bucket-sieve
main (合并后仍然稳定)
  ↓ 分支保留，不删除
```

#### 何时必须新建分支

- `main` 当前编译+测试通过，且即将进行的改动**可能破坏编译**
- 开发新功能（涉及 3+ 文件改动）
- 性能优化实验（结果不确定时）
- 任何可能引入回归的重构

#### 何时可以直接在 `main` 上工作

- `main` 当前已经编译不通过（在修复过程中）
- 纯文档修改（`CLAUDE.md`、`README.md`）
- `.gitignore`、CI 配置等不影响编译的文件
- 单行 hotfix（如修一个明显的 typo）

#### 合并规则

- 合并前：**必须**确认分支上编译通过 + 相关测试通过
- 合并方式：`git merge --no-ff`（保留分支合并记录）
- 合并后：分支**保留不删除**（`git branch -d` 禁止）
- 合并消息：`Merge branch 'feat/260308-bucket-sieve'`（默认即可）

### .gitignore

已配置，排除：`build/`, `xcode-build/`, `.cache/`, `.DS_Store`, IDE 文件, 计划文件（`task_plan.md` 等会话级文件）

### BACKLOG 自动修复策略（已授权）

**BACKLOG.md 中已记录的问题，Claude Code 被授权在二次确认错误确实存在后直接修复**，无需逐条等待用户手动审核。

- **前提条件**：BACKLOG 中已有条目 + 二次确认（读源码确认 bug 真实存在）+ 已制定修复计划
- **直接执行**：确认后按计划修复、提交、更新 BACKLOG/RESOLVED，全程自主，**无任何暂停条件**
- **方案不确定时**：自行选择最优方案并执行，不暂停询问
- **大阶段结束时**：更新 progress.md 后**直接继续下一阶段**，不等待用户确认

### 自动 Push 策略（已授权）

**这是非公开私有仓库，Claude Code 被授权在每个 to-do 大阶段完成后自动 push**。

- **何时 push**：每个大阶段（如 1.x 全部完成）检查点时 push，确保远程有最新备份
- **也可提前 push**：如果当前小步骤涉及重要突破或关键修复，可立即 push 保护成果
- **push 命令**：`git push origin <当前分支>` （带 `-u` 如果是新分支首次 push）
- **禁止 force push 到 `main`**

### Git 操作安全

- **禁止** `git reset --hard` 除非用户明确要求
- **禁止** `git push --force` 到 `main`（其他分支允许 force push）
- **禁止** `--no-verify` 跳过 hooks
- 优先创建**新 commit** 而非 `--amend`（amend 会覆盖历史）
- 每次 commit 前用 `git diff --staged` 确认内容

### 代码质量提升实践

为写出更好的代码，在开发过程中应主动采用以下手段：

- **查阅文档**：对不确定的 API 或算法，使用 `context7` MCP 或 `WebSearch` 查最新文档
- **参考实现**：实现复杂数论算法前，先搜索论文或成熟开源实现确认正确性
- **渐进式验证**：先用小规模数据验证算法正确性，再推广到大规模
- **性能意识**：hot path 中避免不必要的内存分配和拷贝，优先使用 `std::span`、`const&`
- **可读性优先**：清晰的变量命名和函数拆分胜过行内注释

## Error Handling 约定

### C++ 错误处理
- **内部逻辑错误**：使用 `assert()` 或自定义 `GNFS_ASSERT` 宏
- **运行时可恢复错误**：返回 `std::optional` 或错误码
- **致命错误**：`throw std::runtime_error("描述")` 或 `std::logic_error`
- **不要吞掉异常**：catch 后至少 log，不要空 catch

### 数值安全
- `uint64_t` 乘法可能溢出时使用 `__uint128_t`
- 大整数一律使用 `gnfs::core::Integer`
- 除法前检查除数非零
- 模运算前检查模数 > 0

## 项目文件结构

根目录只包含配置和文档文件，所有代码在子目录中：

```
GNFS/
├── include/gnfs/       # 53 头文件 (.hpp)，10 个子模块
├── src/                # 14 源文件 (.cpp)，8 个子目录
├── tests/              # 41 测试文件 (.cpp)
├── scripts/            # test.sh, feature-branch.sh
├── docs/plans/         # 设计文档
├── .claude/            # Claude Code 配置 (agents, skills, hooks)
├── .github/workflows/  # CI/CD
├── CMakeLists.txt      # 构建配置
├── CLAUDE.md           # 项目指令
├── BACKLOG.md          # 待办追踪
├── RESOLVED.md         # 已完成记录
├── README.md           # 项目概述
└── LICENSE             # 许可证
```

**注意**: `.gitignore` 排除了根目录 `*.cpp`/`*.hpp`/`*.sh` 和遗留文档模式，防止误添加。
所有代码必须放在 `include/`、`src/`、`tests/` 中。

## 后台任务管理规范（强制执行）

运行压力测试或长时间 GNFS 筛法可能需要数小时甚至数十小时。**核心原则：后台任务不阻塞主线程工作。**

### 架构原则

```
后台任务 (nohup)                 主线程 (Claude Code)
┌─────────────────┐             ┌─────────────────────┐
│ test_stress 50d │  日志文件    │ 继续其他优化工作      │
│ PID: 77339      │───────────→│ 用户交互时按需查日志   │
│ /tmp/xxx.log    │             │ 不等待、不轮询、不阻塞 │
└─────────────────┘             └─────────────────────┘
```

### 启动规则

1. **用 `nohup` + 文件日志**，不用 `run_in_background`
   ```bash
   cd build && nohup ./test_xxx args > /tmp/xxx.log 2>&1 &
   echo "PID=$! LOG=/tmp/xxx.log" >> /tmp/bg_tasks.txt
   ```
2. **记录到 `/tmp/bg_tasks.txt`**：每次启动后追加 PID、日志路径、启动时间、预期用途
3. **代码中必须加 `std::flush`**：在进度报告输出点加 flush，否则 nohup 全缓冲导致日志不更新
4. **进度报告间隔不超过 100 个 Special-Q**：确保日志定期更新，便于监控

### 监控规则（严格）

- **禁止堆叠多个 `sleep N && tail` 后台任务** — 造成进程泄漏和任务混乱
- **禁止 `run_in_background` 启动 sleep 循环** — 无法可靠取消，上下文膨胀
- **禁止 sleep 超过 300 秒** — 容易忘记清理
- **一次最多一个后台 sleep** — 如果确实需要延迟检查
- **监控是按需的**：用户问进度或自己需要结果时才查，不主动轮询

### 正确监控方法

```bash
# 按需查进度（不自动化）
tail -5 /tmp/xxx.log
ps -p <PID> -o pid,%cpu,etime

# 检查是否完成
ps -p <PID> > /dev/null 2>&1 && echo "running" || echo "finished"

# 查看所有后台任务
cat /tmp/bg_tasks.txt
ps -p $(awk '{print $1}' /tmp/bg_tasks.txt | tr -d 'PID=') -o pid,%cpu,etime 2>/dev/null

# 测试结束后查看完整结果
tail -50 /tmp/xxx.log
```

### 工作流集成

| 场景 | 做法 |
|------|------|
| 启动后台测试后 | **立即继续其他优化工作**，不等待 |
| 用户问 "跑完了吗" | `tail -5 /tmp/xxx.log` + `ps -p PID` |
| 需要测试结果才能继续 | 先做不依赖该结果的工作，最后再查 |
| 测试完成 | 查结果 → 记录到 progress.md → 清理 bg_tasks.txt |
| 新会话开始 | 先读 `/tmp/bg_tasks.txt` 检查是否有遗留后台任务 |
| 会话即将结束 | 在 progress.md 记录所有运行中的后台任务 PID 和日志路径 |

### stdout 缓冲说明

`nohup` 重定向到文件时 C++ `std::cout` 是全缓冲（~4KB-8KB）。代码中已在关键输出点加了 `std::flush`。报告间隔为每 100 SQ，因此两次输出之间可能有几分钟间隔——这是**正常的**，不要因此启动更多监控进程。

## Known Limitations

- 大类群 (>20 generators) 的 Couveignes 实现可能失败（Nguyen Hybrid 优先路径可回避）
- Block Wiedemann 内存使用: Krylov 序列 O(n · L · 8 bytes)，4 GB guard 限制
- OOC 基础设施已实现但未集成到主 pipeline（需手动调用）
- NEON SIMD 尚未实现（P2-A，msieve/CADO-NFS 经验表明 sieve 内核收益有限）

## 工作流规范（强制执行）

### 1. 任务前必须列计划

**所有任务开始前，必须先制定计划。** 默认调用 `planning-with-files` skill（创建 `task_plan.md`, `findings.md`, `progress.md` 于项目根目录）。

- **复杂任务**（涉及多文件修改、新功能、架构变更）：必须走完整 plan 流程，写入 `task_plan.md`
- **简单任务**（单文件小改动、typo 修复）：可以心中有计划后直接执行，但仍需在回复中说明意图
- **计划文件保存在项目根目录**，便于追溯

### 2. 测试设计原则

- **持久化测试**：所有新增功能必须编写可重复运行的单元测试/集成测试，提交到 `tests/` 目录
- **使用 `scripts/test.sh` 运行测试**，不要直接调用 `ctest` 或裸跑二进制（脚本自带超时保护）
- **增量测试**：每次改动只需运行相关模块的测试，**不要每次都重跑整个项目**
  - 改了 `linalg/` → `./scripts/test.sh module linalg`
  - 改了 `sqrt/` → `./scripts/test.sh module sqrt`
  - 不确定影响范围 → `./scripts/test.sh changed`
  - 改了核心流程 → `./scripts/test.sh e2e`
- **注意测试分级**：新测试必须在 `scripts/test.sh` 中注册超时和分级
  - 新增 instant 测试 (<1s): 加入 `SMOKE_TESTS` 和 `MODULE_TESTS`
  - 新增 slow/heavy 测试: 加入 `MODULE_SLOW_TESTS`，设置合理的 `TEST_TIMEOUT`
- **边界/极端情况**：测试必须覆盖边界值、零值、最大值、溢出等极端场景
- 测试框架：沿用项目自定义 assert 宏

### 3. 调试规范

- **禁止盲猜**：遇到 bug 时，不要在未定位问题的情况下直接修改代码碰运气
- **优先使用 lldb 调试**：段错误、逻辑错误等运行时问题，必须用 lldb 设断点、查看变量、追踪调用栈
- **调试流程**：
  1. 复现问题（最小化复现用例）
  2. 用 lldb attach/设断点定位根因
  3. 确认根因后再修改代码
  4. 修改后验证修复

### 4. 代码变更后检查（强制）

每次代码有大改动后，必须执行以下检查：

1. **编译检查**：`make -C build -j$(sysctl -n hw.ncpu)` 无 warning 无 error
2. **相关模块测试**：运行受影响模块的单元测试
3. **边界/极端情况审查**：
   - 整数溢出（特别是 `uint64_t` → `__uint128_t` / `Integer` 的边界）
   - 除零保护
   - 空容器 / 空指针
   - 大输入下的性能退化
4. **E2E 回归**：如改动涉及核心流水线，运行 `test_gnfs_e2e`
5. **代码审查**：使用 code-reviewer agent 检查高风险改动

### 5. 大型工程持久化管理

当任务较大时（预计超过 20+ 个 commit 或涉及多模块），**必须创建持久化工程文件**：

- **`task_plan.md`**：总体计划，拆分为大阶段（1, 2, 3...）和小步骤（1.1, 1.2, 2.1...）
- **`progress.md`**：实时进度记录，每完成一个小步骤更新
- **`findings.md`**：发现的问题、注意事项、技术决策
- **`BACKLOG.md`**：**待办备忘录**（仅未完成条目），本地文件，不纳入 git
- **`RESOLVED.md`**：**已完成与误报记录**（修复历史和审计记录），本地文件，不纳入 git

**任务必须自主拆分**为多个阶段。每个小步骤完成后即 commit，不等待整个阶段完成。

### 5.1 BACKLOG.md + RESOLVED.md — 待办与完成记录（强制维护）

项目维护两个互补的追踪文件（本地使用，不纳入 git）：

| 文件 | 内容 | 侧重点 |
|------|------|--------|
| **`BACKLOG.md`** | 仅未完成的待办条目 | 问题描述、文件位置、修复建议 |
| **`RESOLVED.md`** | 已完成的修复 + 误报记录 | 修复方案、验证方式、Commit hash |

#### BACKLOG.md — 待办备忘录

只记录**未完成**的问题，按严重程度从高到低排序：

```
P1 — 高优先（影响正确性、可靠性、线程安全）
P1-OPT — 高优先性能优化
P2 — 中优先（边界情况、代码质量、非核心路径）
P3 — 低优先（理论风险、技术债务、死代码）
TEST — 测试覆盖缺口
```

**查找最重要问题时从文件开头往下读即可**。

**新增条目格式**（插入到对应严重等级区域末尾）：
```markdown
### [BUG] 简短标题
- **发现日期**: 2026-03-09
- **文件**: `sqrt/hensel_sqrt.hpp:123`
- **描述**: 具体问题描述
- **建议**: 修复思路（如果有的话）
```

**分类标签**：`[BUG]` 缺陷 | `[OPT]` 优化 | `[FEAT]` 新功能 | `[DEBT]` 技术债务 | `[RISK]` 潜在风险

**注意事项**：修复一个 bug 前，先扫描 BACKLOG 中同模块的其他条目，看是否可以一并解决。

#### RESOLVED.md — 已完成与误报记录

记录所有已修复的条目和经核查确认的误报，作为项目审计和知识沉淀。

**关闭条目流程**（从 BACKLOG 移到 RESOLVED）：

条目必须满足以下**全部条件**才能移入：

1. **代码已合入**：修复代码已 commit（最好已合入 main）
2. **测试已通过**：相关模块测试通过，或有专门的回归测试覆盖
3. **无副作用**：修复未引入新的编译警告、测试失败或性能退化

**已完成条目格式**（写入 RESOLVED.md 对应级别区域）：
```markdown
#### [BUG] ~~Hensel p^d-2 overflow~~ ✅
- **发现**: 2026-03-08
- **解决**: 2026-03-09
- **修复**: `sqrt/hensel_sqrt.hpp:123` 改用 Integer 类型
- **验证**: `./scripts/test.sh module sqrt` 全部通过
- **Commit**: `abc1234`
```

同时从 BACKLOG.md 中**删除**该条目（不保留）。

**标记误报**：经源码验证确认不是真实问题的条目，写入 RESOLVED.md 的「误报」区域，说明误报原因。同时从 BACKLOG.md 中删除。

#### 何时写入 BACKLOG

- 工作中发现了 bug 或隐患，但不属于当前任务范围
- 发现了可优化的点，但当前阶段不做
- 审查或找茬时发现的非阻塞问题，记录后继续
- 用户提出了新需求，但当前 Part 不处理
- 任何"先记下来，以后再说"的事项

#### 维护规则

- **发现即记录**：不要等到阶段结束才补记，发现时立即写入 BACKLOG 并 commit
- **开始新任务时必读**：每次启动新任务前，先读 `BACKLOG.md` 检查是否有相关待办
- **大阶段检查点时回顾**：每个大阶段完成后，扫一遍 BACKLOG 看有没有顺手能解决的
- **修复后双文件更新**：修复某条目后，从 BACKLOG 删除 + 写入 RESOLVED，两步缺一不可
- **修复失败/搁置时**：保留在 BACKLOG 对应区域，追加说明失败原因或搁置理由

#### 禁止行为

- **禁止不验证就关闭**：改了代码不跑测试，直接移入 RESOLVED
- **禁止遗忘双文件更新**：修复后只更新了一个文件而忘了另一个
- **禁止批量静默关闭**：不能一次性移动多个条目而不逐条说明验证方式
- **禁止删除 BACKLOG 中未解决条目**：只能通过修复→移入 RESOLVED 来清除
- **禁止打乱排序**：新增条目必须插入对应等级末尾

### 6. 两级 Commit + 检查点机制

#### 小步骤 Commit（自动，高频）

每做一小步就 commit，不需要审批：
- 改了一个函数 → commit
- 修了一个编译错误 → commit
- 加了一个测试 → commit
- 调了一个参数 → commit

小步骤 commit 前只需确认：
- [ ] `git diff --staged` 内容符合预期
- [ ] commit message 符合 Conventional Commits 格式
- [ ] 未包含敏感文件或构建产物

#### 大阶段检查点（自动继续，不等待）

当一个大阶段（如完成所有 1.x 步骤）全部完成后，执行以下流程后**立即继续**：

1. **编译 + 测试验证**：确保当前状态编译通过、相关测试通过
2. **更新持久化文件**：详细记录到 `progress.md`
3. **按需压缩上下文**：`/compact` 压缩后直接开始下一大阶段
4. **不等待、不暂停、不询问** — 直接开始下一大阶段

```
大阶段 1 开始
  ↓
步骤 1.1：实施 → commit（自动）
步骤 1.2：实施 → commit（自动）
步骤 1.3：实施 → commit（自动）
  ↓
大阶段 1 完成：编译验证 + 更新 progress.md
  ↓
/compact 压缩（按需）→ 直接开始大阶段 2
  ↓
步骤 2.1：实施 → commit（自动）
...
```

### 7. 计划执行流程总结

```
任务开始
  ↓
制定计划 → 拆分大阶段 + 小步骤 → 写入 task_plan.md
  ↓
执行小步骤 → 每步 commit（自动，不等编译全通过）
  ↓
大阶段完成 → 编译+测试 → 更新 progress.md → 直接继续下一阶段
  ↓
（按需 /compact 压缩，不暂停）
  ↓
全部完成 → 最终验证 + E2E 回归 → 总结报告
```

### 8. 上下文窗口管理

**不要自己压缩上下文。** 系统会自动处理上下文压缩，不需要手动执行 `/compact`。

#### 核心原则

**绝不因上下文问题停止解决问题。** 上下文管理不是你的职责，专注于完成任务本身。

#### 良好实践（不强制）

- 关键信息写入持久化文件（`progress.md`, `findings.md`, `BACKLOG.md`）是好习惯，但不应为此中断工作流
- 子 Agent 优先处理大量探索性工作，减少主线程负担
- 编译和测试输出只关注错误/失败部分

#### 禁止行为

- **禁止**因为担心上下文膨胀而停止工作
- **禁止**在工作流中间插入 `/compact` 导致中断
- **禁止**将上下文管理作为暂停或停止的理由

## Agent Team 协作编排

### 设计原则

采用**专职分工、职责明确**的 Agent 团队模式。每个 Agent 有且仅有一个核心职责，禁止越权操作或混淆任务边界。任何 Agent 不得怠工（跳过检查、简化流程、默认通过）——所有输出必须有实质内容。

### 角色分配

| 角色 | Agent 类型 | 职责 | 触发时机 |
|------|-----------|------|---------|
| **架构师** | `feature-dev:code-architect` | 分析需求，设计实现方案，确定文件结构和数据流 | 任务启动阶段，在编码之前 |
| **探索员** | `Explore` | 检索代码库，定位相关文件，理解现有模式和依赖 | 需要理解现有代码时（架构师或开发者需要参考时） |
| **开发者** | `feature-dev:code-explorer` + 主线程编码 | 理解现有实现细节，执行编码任务 | 实施阶段 |
| **审查员** | `feature-dev:code-reviewer` | 检查代码质量、逻辑正确性、是否符合项目约定 | 每个大阶段完成后，commit 前 |
| **找茬员** | `pr-review-toolkit:silent-failure-hunter` | 专门查找静默失败、错误吞没、边界遗漏、极端情况 | 审查员通过后的二次检查 |
| **进度员** | 主线程自身 | 维护 `progress.md`，跟踪计划执行状态，确保不偏离目标 | 持续进行 |

### 协作流程

```
架构师 → 分析需求，输出实现蓝图
  ↓
探索员 → 检索相关代码，提供上下文
  ↓
开发者 → 按蓝图逐步编码 → 每步 commit
  ↓
审查员 → 代码审查（质量 + 正确性 + 约定符合度）
  ↓
找茬员 → 静默失败猎杀（边界 + 异常 + 极端情况）
  ↓
进度员 → 更新 progress.md → 报告给用户
```

### 纪律约束（防怠工条例）

以下行为视为**怠工**，严格禁止：

1. **审查放水**：审查员或找茬员未发现任何问题就直接通过（至少要说明审查了哪些方面、为什么无问题）
2. **跳过环节**：跳过架构分析直接编码，或跳过审查直接 commit
3. **任务漂移**：Agent 执行了不属于自己职责的工作（如审查员去改代码，开发者去做架构决策）
4. **信息丢失**：未将关键发现写入持久化文件，导致上下文压缩后信息丢失
5. **盲目执行**：未读懂现有代码就开始修改，未理解需求就开始实现
6. **沉默失败**：Agent 遇到问题不上报，自行绕过或忽略

### 何时启用完整团队

- **简单任务**（<3 文件改动）：主线程直接执行，只在 commit 前调审查员
- **中等任务**（3-10 文件改动）：架构师 + 开发者 + 审查员
- **大型任务**（>10 文件 或 跨模块）：完整六角色团队
- **调试任务**：替换架构师为 `debugging-toolkit:debugger`，其他不变

### 并行调度

独立任务**必须并行派发**以提升效率：

- 多个模块互不依赖时 → 并行派发多个开发者 Agent（使用 `isolation: "worktree"`）
- 审查和找茬可并行 → 同时派发审查员和找茬员
- 探索多个方向时 → 并行派发多个探索员

禁止在可并行的场景下串行执行。
