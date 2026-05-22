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
./scripts/test.sh                      # 冒烟测试: 39 个 instant 测试, ~5s (Debug) / ~5s (Release)
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
| **instant** | 10s | 39 个纯单元测试 (test_integer / test_linalg / test_sqrt / test_murphy / test_filter / test_v0_bfs_policy / test_clique_merger / test_ooc_policy / test_sieve_ecore_qos 等) — 完整清单见 `scripts/test.sh` SMOKE_TESTS 数组 | smoke, module, changed |
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
include/gnfs/           # 61 头文件 (.hpp)
├── api/           (6)  # Config, Pipeline, Factorizer, i18n — 公开 API 层
├── core/          (6)  # Integer, Polynomial, Relation, Params, Types — 基础类型
├── polynomial/    (7)  # Kleinjung 选择, Murphy E, base-m, IntPolynomial, Optimizer, Dispatch, Resultant
├── factor_base/   (2)  # 因子基构建 (Cantor-Zassenhaus 求根)
├── sieve/         (5)  # Lattice sieve, Special-Q, Lattice basis, sieve_checkpoint, ecore_qos
├── cofactor/      (5)  # 余因子分解: 试除法, ECM, SQUFOF, 光滑性检查
├── relation/      (6)  # collector, filter, clique_merger, v0_bfs_policy, ooc_policy, ooc_relation_store
├── linalg/        (9)  # GF(2) 矩阵, Block Lanczos, Block Wiedemann, Gaussian, SGE, Schirokauer, Mmap CSR, Krylov mmap
├── sqrt/          (7)  # 代数平方根 (Nguyen Hybrid + Couveignes), 有理平方根, 类群, ModularPoly
├── siqs/          (1)  # 备选 SIQS 路径 (小 N 兜底)
└── util/          (7)  # SmallVector, ThreadPool, Logger, Timer, MmapFile, SafeMath, Primes

src/                    # 14 源文件 (.cpp)，按模块组织
├── api/           (2)  # pipeline.cpp, factorizer.cpp
├── cli/           (1)  # main.cpp — CLI 入口
├── core/          (3)  # integer, polynomial, relation
├── factor_base/   (1)  # builder (Cantor-Zassenhaus)
├── linalg/        (3)  # block_lanczos, block_wiedemann, matrix_builder
├── polynomial/    (1)  # base_m
├── sieve/         (1)  # lattice_sieve
└── sqrt/          (2)  # algebraic_sqrt, rational_sqrt

tests/                  # 63 测试文件 (.cpp)
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
- `src/api/pipeline.cpp` — `V3Mode` enum + `cascade_v3_mode()` + `cascade_v3_enabled_for_round(round)` (~line 47)
- `src/api/pipeline.cpp` — `sieve_and_collect` adaptive loop (Auto-aware, 见 `cascade_v3_enabled_for_round` 调用点)
- `src/api/pipeline.cpp` — `Pipeline::filter` Phase 4 (always enable when not Off)
- `tests/test_stress.cpp` — stress sieve loop (V3 cascade prep + run, 见 `[v3_cascade]` 输出)

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

### Thin matrix BW solve (B'=M^T·M variant, default ON)

**实施 2026-05-17**: 当 matrix rows ≤ cols (NO_EXCESS),
`find_dependencies` 自动调用 `block_wiedemann_thin_solve`, 使用 `B'=M^T·M`
operator 而非标准 `B=M·M^T`. 工作在 R^n, recovery via `u=M·w`.

**算法数学正确性**:
- BW phase 3 给 w ∈ R^n 满足 `(M^T·M)·w = 0` 严格 over GF(2)
- 由 associativity, `M^T·u = (M^T·M)·w = 0` 严格成立, u 是 left null space vector
- 与标准 path 的 `v^T·M·M^T·v = parity(M^T·v)` GF(2) quadratic-form quirk 不同 —
  这是 strict linear relation, 不依赖 quadratic form

**ENV `GNFS_NO_THIN_SOLVE=1`** (opt-out): 恢复 prior "abort on no excess" 行为.
正常使用无需 ENV — pipeline 自动 detect m ≤ n 并 route.

**Robustness** (实测 across rank profiles):
- rank ≈ m: `test_thin_matrix_bw_solve` (5200×6000, rank≈5100, deps≈100) — 10/10 valid
- rank ≪ m: `test_thin_matrix_bw_extreme_rank_deficiency` (rank=100, 5100 deps) — 10/10 valid
- multi-seed retry (3 seeds) 处理 Phase 1 Krylov projection edge cases
- Phase 4 verification (M^T·u=0) 严格 enforces — never returns invalid deps

`find_dependencies` routes m<n → thin_solve, m≥n → block_solve (existing path).

### V0 weight-3 merge (GNFS_V0_WEIGHT3)

**ENV `GNFS_V0_WEIGHT3=1`** (commit `81d3331`, 2026-05-17):
V0 Phase 2 也 merge weight=3 LP keys 的 first 2 partials (3rd 下轮变 singleton).
V0 partial weight≥3 handling — lightweight V3 cascade alternative
不走 BFS spanning tree.

```bash
GNFS_V0_WEIGHT3=1 ./test_stress 1 1   # 50d 启用 V0 weight=3 merge
GNFS_V0_WEIGHT3=1 ./gnfs <N>          # any GNFS run
```

**默认 OFF**: V0 仍只 merge weight=2 (保守 行为). 仅在用户 explicit opt-in 时启用.
**V3 cascade 与之 orthogonal**: V0_WEIGHT3 加快 V0 convergence (单 pair per key),
V3 cascade 走 full chain BFS. 二者可同时启用.

### Drop-residual + weight-cutoff (50d β plateau 实验通道)

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
- 注意: drop 模式 V3 cascade 的 v3_added 可能 = 0 (V0 已覆盖 full, V3 残留全 drop), `tests/test_clique_merger_50d_synthetic.cpp` 的 `v3_added > 0` 已 conditional skip (检测 `GNFS_DROP_RESIDUAL` ENV).

### V0 BFS chain merge (GNFS_V0_BFS)

**ENV `GNFS_V0_BFS=1`** (2026-05-18 实施, test entry wiring 2026-05-19):
Pipeline filter() 中 V0 主路径用 CliqueRelationMerger BFS spanning tree 算法替代
PartialRelationMerger::merge_all (standard Phase 1+2 weight=2 simple match).
启用时 V3 cascade redundant (skip).

**Size-aware gate** (实测 finding 2026-05-18):
- lp_bits ≥ 22 (50d+): V0_BFS 启用. BFS 处理 weight≥3 LP chain
- lp_bits ≤ 20 (25d/81-bit): ENV ignored, fallback to V0 standard
  + stderr warning. **原因**: BFS chain merge 在 small LP space 产生 ~87%
  residual partials, matrix LP cols 严重 dominate, BL 找不到 dependencies.
  实测 test_regression_gate Level 4 (81-bit) V0_BFS=1 FAIL "no dependencies found"

```bash
GNFS_V0_BFS=1 ./gnfs <50d-or-larger>   # 50d+ V0 主路径 BFS
GNFS_V0_BFS=1 ./gnfs <81-bit>          # 自动 fallback, stderr 警告
```

**用途**: "weight≥3 LP keys 也可合并" 的 lightweight 实现.
不重写 PartialRelationMerger, 仅 dispatch to CliqueRelationMerger (已实现 BFS).

**集成点** (commits `086afb2` + `d50fd61`, 2026-05-18; test wiring `0f4e9c3` + `8d222f8`, 2026-05-19):
- `include/gnfs/relation/v0_bfs_policy.hpp` — env-gate + size-aware dispatch decision
- `src/api/pipeline.cpp` — `Pipeline::filter()` V0_BFS dispatch (检索 `GNFS_V0_BFS` getenv 点)
- `tests/test_stress.cpp` / `tests/test_gnfs_progressive.cpp` — 同 policy wire-in (避免 test entry point bypass)

### OOC Relation Store (GNFS_OOC_RELATIONS)

**ENV `GNFS_OOC_RELATIONS=1`** (2026-05-18 实施):
启用 RelationCollector OOC 流式持久化, sieve 期间 relations 流式写盘
`/tmp/gnfs_relations_<pid>.{reldata,relidx}` 而非 in-memory vector. 内存只保留
(a,b) seen set + stats. Phase 4 filter 入口 OOCRelationReader 一次性 read_all
→ vector. 默认 OFF (vector mode).

```bash
GNFS_OOC_RELATIONS=1 ./gnfs <N>                  # 启用 OOC streaming
GNFS_OOC_RELATIONS=1 GNFS_OOC_BASE_PATH=/path ./gnfs <N>  # override 路径
GNFS_OOC_RELATIONS=1 ./test_gnfs_e2e             # e2e stress test OOC path
```

**用途**:
- 触发场景: 50d Round 2 909K relations 时 macOS OOM-killed
  (2026-05-17 实测, RSS ~3.5GB, sieve buckets + collector.relations_ 联合 OOM).
- OOC mode 减小 sieve 期间 RAM peak (relations_ vector 不再 grow, seen_ 占
  ~16 B/relation, 1M relations 仅 16 MB; vs vector 1M × 500B = 500 MB).
- fault tolerance: OOCWriter MAGIC_INCOMPLETE → MAGIC flip 设计保证 mid-write
  crash 时 reader 严格拒绝, 不会 partial-load.

**集成点** (commits `3b843fc` → `d39b637`, 2026-05-18):
- `include/gnfs/relation/collector.hpp` — CollectorConfig + add/get/clear/merge OOC dual mode
- `include/gnfs/relation/ooc_relation_store.hpp` — OOCRelationWriter/Reader, MAGIC/INCOMPLETE flip
- `include/gnfs/relation/ooc_policy.hpp` — 三态 ENV 解析 (off / auto-by-size / on)
- `src/api/pipeline.cpp` — `sieve_and_collect` ENV-gate + base_path (检索 `GNFS_OOC_RELATIONS` getenv 点)
- `tests/test_relation_collector.cpp` — 8 OOC unit tests (basic/dedup/N-divisibility/partial/concurrent/clear/empty-path/legacy)
- `tests/test_ooc_relations.cpp` / `tests/test_ooc_policy.cpp` — OOC store + policy
- `tests/test_gnfs_e2e.cpp` — OOC stress test in real GNFS pipeline (5/5 PASS)

**API 兼容**:
- `add()`: OOC 模式跳过 relations_.push_back, 走 OOCWriter::write
- `get_relations()`: OOC 模式 close writer + open reader + read_all → vector (spike at Phase 4 entry)
- `size()/empty()`: 基于 writer->count() (准确反映写盘 relation 数)
- `clear()`: OOC 模式 close + delete files + recreate writer (允许 reuse)
- `save/load`: legacy 序列化协议 OOC 模式 disabled (return false); 直接用 OOCRelationReader
- `merge`: OOC source 不支持 (read overhead 不实用); OOC sink 工作

### Sieve mid-flight checkpoint (GNFS_SIEVE_RESUME)

**ENV `GNFS_SIEVE_RESUME=<base_path>`** (2026-05-18 实施):
启用 OOC streaming + sieve loop checkpoint, 长时间 50d+/60d sieve 中断后能 resume.
ENV 隐含启用 OOC (base_path 作 OOC base 和 checkpoint base, 不需 单独 set
GNFS_OOC_RELATIONS).

```bash
# 首次启动 / 续跑同 path
GNFS_SIEVE_RESUME=/tmp/gnfs_50d_session ./gnfs <50d-N>
# 进程崩溃后, 同 path 再跑 → resume from last checkpoint
GNFS_SIEVE_RESUME=/tmp/gnfs_50d_session ./gnfs <50d-N>
```

**Resume 流程**:
1. Pipeline::sieve_and_collect 检测 `<base_path>.sieve_ckpt` 存在 + magic 有效
2. 加载 ckpt: sq_count, current_index (SpecialQGenerator 位置), round (adaptive
   loop 进度), batch_target, candidates_total
3. CollectorConfig.ooc_resume=true → OOCWriter 用 resume mode 续写
   .reldata/.relidx (要求 magic=INCOMPLETE, finalized files 不允许 resume)
4. RelationCollector ctor 从 .reldata 读 (a,b) 16 bytes/rel 重建 seen_ set
   (防 resume 后 dedup 错过)
5. SpecialQGenerator::reset_to(current_index) skip 已 done SQs
6. Sieve loop 从 round_start 继续, 每 CHECKPOINT_BATCH_INTERVAL=25 batches
   保存 ckpt (每 batch 2-4 SQ, ~50-100 SQs/checkpoint)
7. Sieve 正常完成 → 删 ckpt + OOC finalize MAGIC (后续 read 通过 reader)
8. 异常退出 (crash/kill) → ckpt + INCOMPLETE OOC 保留, 下次 resume

**Crash safety** (MAGIC/INCOMPLETE flip 双重保护):
- SieveCheckpoint: save() 先写 MAGIC_INCOMPLETE, flush, seek 头 flip MAGIC
- OOCRelationWriter: ctor 写 INCOMPLETE, close() flip MAGIC; uncaught_exceptions
  跟踪让析构异常路径 skip flip → 文件保留 INCOMPLETE → reader 拒读
- 任一 stage crash 时下次 resume 仍 detect partial state (允许丢 ≤25 batches)

**集成点** (commits `b4c6364` → `60a1282`, 2026-05-18):
- `include/gnfs/sieve/sieve_checkpoint.hpp` — SieveCheckpoint binary format (MAGIC/INCOMPLETE flip)
- `include/gnfs/relation/ooc_relation_store.hpp` — OOCWriter resume ctor
- `include/gnfs/relation/collector.hpp` — CollectorConfig.ooc_resume + `restore_seen_from_ooc` helper
- `src/api/pipeline.cpp` — `sieve_and_collect` ENV-gate + ckpt save/load (检索 `GNFS_SIEVE_RESUME` getenv 点 + `SieveCheckpoint::save` 调用点)
- `tests/test_sieve_checkpoint.cpp` — 9 unit tests (roundtrip/corrupt/version/INCOMPLETE)
- `tests/test_relation_collector.cpp` — 6 new tests (writer append + collector resume)
- `tests/test_api.cpp` — 2 e2e tests (fresh + synthetic_ckpt resume)

**触发条件**: 50d+ sieve 持续 hours+ 而 crash 风险 (OOM/电源/Ctrl-C) 存在.
对 25d/40-bit 短任务 overhead 不实用 (sieve <1 min). 不与 GNFS_OOC_RELATIONS
共存 (SIEVE_RESUME 优先).

### BW Krylov sequence mmap (GNFS_BW_KRYLOV_MMAP)

**ENV `GNFS_BW_KRYLOV_MMAP=1`** (2026-05-18 实施):
BW Phase 1 Krylov sequence 写到 `/tmp/gnfs_bw_krylov_*.kry` mmap-backed file
而非 in-memory vector. Phase 2 BM 入口 copy mmap → vector 再 close.

```bash
GNFS_BW_KRYLOV_MMAP=1 ./gnfs <N>   # 50d+/60d 大矩阵 Phase 5 启用
```

**ROI**:
- matrix BM `A_seq[L]` of DenseGF2_64x64 (512B): 16 MB @ n=1M
- scalar BM `sequences[64][seq_len]` of uint8_t: 128 MB @ n=1M
- 总 ~144 MB physical RAM 释放 给 V/Vnext block vectors + matrix + OS cache

**集成点** (commits `21ac368` → `66ce50f`, 2026-05-18):
- `include/gnfs/linalg/krylov_sequence_mmap.hpp` — 233 行 mmap RAII container
- `src/linalg/block_wiedemann.cpp` — matrix BM `block_solve` + scalar BM `streaming_solve`
- `tests/test_krylov_sequence_mmap.cpp` — 8 unit tests
- `tests/test_bw_krylov_mmap_integration.cpp` — 3 integration tests (5550×5000)

**Default OFF**: vector path 完整保留, 零回归风险. 仅 50d+ Phase 5 RAM pressure 时启用.

### BW Krylov multi-stream parallel (GNFS_BW_KRYLOV_STREAMS)

**ENV `GNFS_BW_KRYLOV_STREAMS=K`** (2026-05-21 实施, range [1, 16], default 1):
BW Phase 1+2+3 跑 K 个独立 worker (各自独立 seed + 独立小 ThreadPool, 每个
`hardware_concurrency/K` 线程). 默认 K=1 保持原 sequential 3-seed retry 行为
bit-for-bit. K>1 时 base seed 每个 retry round 派 K 个 stream 并发, 结果
按 content 去重合并到 max_deps.

```bash
GNFS_BW_KRYLOV_STREAMS=2 ./gnfs <N>   # 2 streams concurrent
GNFS_BW_KRYLOV_STREAMS=4 ./gnfs <N>   # 4 streams concurrent
unset GNFS_BW_KRYLOV_STREAMS          # 默认 K=1, 原行为
```

**ROI 与定位**:
- 主要 ROI: retry latency 减少. K=1 单 seed 失败 → 串行 retry 第 2/3 seed
  (1×T → 2-3×T). K=4 并发 → 4 个 seed 同时 → 1×T 即给 deps. 50d/60d 大矩阵 +
  rank-deficient corner case 时显著.
- 次要: wall-time. Phase 2 (BM) inherently 单线程, 占 BW 总时间 ~70%. 多 stream
  不加速 single solve. K=2 ≈ K=1; K=4 ~10% 慢 (pool overhead). 实测 5550×5000:
  K=1=613ms, K=2=629ms, K=4=688ms (single-seed succeeds case).
- Mmap 路径文件名含 stream_tag (`gnfs_bw_krylov_<pid>_s<N>_<seed>.kry`) 避免
  并发 stream 之间路径冲突.

**集成点** (commits `0857b7d` → `c5416ce`, 2026-05-21):
- `src/linalg/block_wiedemann.cpp` — `bw_num_streams()` ENV parser +
  `find_dependencies_view_impl` multi-stream dispatcher
- `src/linalg/block_wiedemann.cpp` — `block_solve_view_impl` / `thin_solve_view_impl`
  添加 `pool_threads, stream_tag` 参数 (default 0, 0 = 兼容原行为)
- `include/gnfs/linalg/detail/spmv_kernels.hpp` — `spmv_transpose` scratch
  改 `thread_local` (race fix, commit `2d9d2f0`). 多 stream concurrent SpMV
  原本 race static scratch → 产生 garbage Krylov → 0 valid deps.
- `tests/test_bw_krylov_parallel.cpp` — 6 unit tests (K=1/2/4 + clamping +
  speedup measurement informational)

**Default OFF (K=1)**: 单 stream path 完整保留, 零回归. SparseMatrix 的
scalar fallback path 仍在 K>1 时保持 (block 路径多 stream empty 后 fall back
to scalar; thin path 无 fallback).

### SIMD GF(2) SpMV inner kernels (GNFS_SPMV_SIMD)

**ENV `GNFS_SPMV_SIMD=auto|0|1`** (2026-05-21 实施, default auto):
Block Lanczos / Block Wiedemann SpMV 的 inner XOR-gather (forward) +
XOR-scatter (transpose) tail 切到 NEON 2-lane (ARM64) 或 AVX2 4-lane
(x86_64) wide XOR. GF(2) 加法 associative + commutative, SIMD 输出与
scalar 路径 bit-for-bit 一致. 不依赖任何外部库, 纯 header.

```bash
GNFS_SPMV_SIMD=auto ./gnfs <N>    # 默认: NEON/AVX2 可用则启用, 否则 scalar
GNFS_SPMV_SIMD=0    ./gnfs <N>    # 强制 scalar (回归 bisect 用)
GNFS_SPMV_SIMD=1    ./gnfs <N>    # 强制 SIMD (无 SIMD 平台仍 fallback scalar)
unset GNFS_SPMV_SIMD              # 同 auto
```

**ROI 与定位**:
- 主要 ROI: 内核 retired uop 数 ÷2 (NEON) / ÷4 (AVX2) on tail iteration.
  Prefetch phase (前 8 个 column) 保持 scalar — `__builtin_prefetch`
  hint per-element 是 latency 隐藏的主要手段, SIMD 无关. Tail 部分越长
  ROI 越显著, 50d/60d Phase 5 SpMV (row width >> 8) 受益最大.
- 默认 auto 在 macOS arm64 / Linux x86_64 二者上都 enable; CI runner
  也都覆盖 (Apple Silicon + GitHub-hosted x86_64 ubuntu-latest).
- ENV=0 在 PMU sweep / sanitizer 调试时回到旧 baseline 用.

**集成点** (commits `2fe4aee` → `b321e62`, 2026-05-21):
- `include/gnfs/linalg/detail/spmv_simd.hpp` — `gather_xor_row` /
  `scatter_xor_row` inner helpers + `is_simd_available` / `use_simd_runtime`
  cached env reader + 独立测试 wrappers `spmv_forward_simd` /
  `spmv_transpose_simd`
- `include/gnfs/linalg/detail/spmv_kernels.hpp` — `spmv_forward` /
  `spmv_transpose` tail 走 SIMD helper (prefetch phase 保持 scalar)
- `tests/test_spmv_simd.cpp` — 13 个测试 (empty / 1x1 / random 100x100 /
  random 10000x10000 / max density / single row 0..33 / single column /
  transpose round-trip / env parsing / dispatcher integration /
  repeated calls / batch boundaries / zero input). 三种 ENV 都验证

**Default ON (auto)**: 对所有 SpMV 调用方透明启用. zero behavior change
对 user (除内核 uop 数), bit-for-bit 输出一致.

### Trial division SIMD 8-prime batch (GNFS_TRIAL_DIV_SIMD)

**ENV `GNFS_TRIAL_DIV_SIMD=auto|0|1`** (2026-05-22 实施, W9 T3, default auto):
Cofactor pipeline 入口 (`include/gnfs/cofactor/`) 在 SQUFOF / ECM 之前
先扫小素数池做 trial division. helper `batch_check_divisibility`
把每 4 个 prime 批量 load 进一个 SIMD 寄存器 (NEON `uint32x4_t` 在
ARM64; AVX2 / SSE2 4-lane 在 x86), per-lane 再走 scalar `cofactor % p`
(NEON / AVX2 / SSE2 均不加速 uint32 除法), 输出 bit-for-bit 与 scalar
reference 一致.

```bash
GNFS_TRIAL_DIV_SIMD=auto ./gnfs <N>   # 默认: NEON/AVX2/SSE2 可用则启用
GNFS_TRIAL_DIV_SIMD=0    ./gnfs <N>   # 强制 scalar (回归 bisect 用)
GNFS_TRIAL_DIV_SIMD=1    ./gnfs <N>   # 强制 SIMD (无 SIMD 平台 fallback)
unset GNFS_TRIAL_DIV_SIMD             # 同 auto
```

**Helper API** (`include/gnfs/cofactor/trial_div_simd.hpp`):
- `batch_check_divisibility(cofactor, primes, out_divisible_indices)` —
  主入口, 内部三态 gate 路由到 SIMD 或 scalar 路径, 输出 indices 按
  input 顺序 append (不清空 out).
- `batch_check_divisibility_scalar(...)` — scalar reference, 测试 golden
  也供希望显式禁 SIMD 的 caller 使用.
- `trial_div_simd_mode()` / `trial_div_simd_enabled()` — cached
  `std::once_flag` + `std::atomic<int>` ENV reader, 严格 "0"/"1" parsing,
  其它值 (unset / "" / "auto" / "garbage" / "2" / "true") 均视为 Auto.
- `trial_div_simd_supported()` — compile-time `__ARM_NEON` / `__AVX2__` /
  `__SSE2__` 探测.
- `trial_div_simd_reset_env_cache_for_testing()` — 测试专用 re-resolve ENV.

**Bit-for-bit guarantee**: SIMD 仅 batch load + register allocation,
inner `cofactor % p` 与 scalar 路径同函数. 单元测试
`tests/test_trial_div_simd.cpp` 13 个测试强制覆盖 (5 ENV / empty /
single / 8-prime mixed / 100 cofactor x 30 prime 大 sweep / 1000-prime
batch / 0..8 boundary sweep / ForceOff 路径 / append 语义).

**ROI 与定位**:
- helper-only future-infrastructure. 当前主 pipeline cofactor 入口
  (`trial_division.hpp` / `batch_trial.hpp`) 未 wire-in, 行为完全不变.
- 当 caller 显式 wire-in 时 ROI 主要在 retired uop 数 (4 个 lane 的
  prime load + index extract 由 SIMD register 批量完成, 避免 4 次独立
  memory load 的 address-gen 串联). 实际 wall-time 提升依赖具体调用
  pattern (50d+/60d cofactor 短池 trial < 1µs/cofactor, 几 % 改进).
- 默认 auto 在 macOS arm64 / Linux x86_64 二者都 enable;
  ENV=0 用于 PMU sweep / sanitizer 回归 bisect.

**集成点** (2026-05-22):
- `include/gnfs/cofactor/trial_div_simd.hpp` — helper API + 三态 ENV gate
  + NEON / AVX2 / SSE2 inner kernel + scalar reference.
- `tests/test_trial_div_simd.cpp` — 13 instant tier tests, TIMEOUT 60.
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  cofactor 模块.

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点,
ENV 对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

### Cofactor survival rate predictor (GNFS_SURVIVAL_FILTER + GNFS_SURVIVAL_THRESHOLD)

**ENV `GNFS_SURVIVAL_FILTER={0,1}`** + **`GNFS_SURVIVAL_THRESHOLD=<double>`** (2026-05-21 实施, default OFF):
余因子分类入口 (`classify_cofactor`) 用 Dickman ρ 函数估算 cofactor 通过整条
cofactor pipeline (trial division → SQUFOF → Brent-Pollard rho → legacy Pollard rho → ECM)
的 survival 概率. 概率低于阈值时 zero-cost 早 reject (CofactorClass::TooLarge),
跳过昂贵的分解尝试.

```bash
# 默认行为: filter OFF, 零开销, 永不 reject (W5 T5 default)
unset GNFS_SURVIVAL_FILTER GNFS_SURVIVAL_THRESHOLD

# 启用 filter 但 threshold=0 ⇒ 仍永不 reject (安全测试模式)
GNFS_SURVIVAL_FILTER=1 ./gnfs <N>

# 启用 filter + 保守 threshold (catastrophically unlikely 才 reject)
GNFS_SURVIVAL_FILTER=1 GNFS_SURVIVAL_THRESHOLD=1e-12 ./gnfs <N>

# 中度 threshold: reject if survival < 0.0001% (可能损失 1-2% smooth relations)
GNFS_SURVIVAL_FILTER=1 GNFS_SURVIVAL_THRESHOLD=1e-6 ./gnfs <N>

# 激进 threshold: reject if survival < 0.1% (可能损失 5-10% smooth relations)
GNFS_SURVIVAL_FILTER=1 GNFS_SURVIVAL_THRESHOLD=1e-3 ./gnfs <N>
```

**算法 (Dickman ρ)**:
- ρ(u) 函数估算密度: u = log(N) / log(y) 时, ρ(u) ≈ fraction of integers ≤ N 是 y-smooth
- u_smooth = cofactor_bits / smoothness_bound_bits (全 B-smooth path)
- u_lp = cofactor_bits / lp_bound_bits (允许 ≤ 1 prime in (B, LP] path)
- 综合估算: max(ρ(u_smooth), ρ(u_lp)) — 取 max 保守 lower-bound (LP 路径更宽容)
- 实现: u ∈ [1, 2] 用闭式 ρ(u) = 1 - ln(u); u ∈ (2, 10] 用整数 anchor + log-linear 插值;
  u > 10 用 u^{-u} 渐进式
- ρ(2) ≈ 0.30685, ρ(3) ≈ 0.04860, ρ(5) ≈ 3.5e-4 (van de Lune & Wattel 1969)

**触发条件 (三态 AND)**: `GNFS_SURVIVAL_FILTER=1` AND `GNFS_SURVIVAL_THRESHOLD > 0`
AND caller 传入 `smoothness_bound > 0` (sieve params 必须传递 B 给
`classify_cofactor`). 任一条件失败则跳过 predictor (零开销).

**与 W5 T4 Brent-Pollard rho 的相对位置**:
```
survival_predictor (W5 T5, BEFORE) -- 最前面的早 reject
  ↓ (predictor passes)
trial division (small primes)
  ↓
SQUFOF
  ↓
BrentPollardRho (W5 T4, GNFS_COFACTOR_BRENT=1)
  ↓
Pollard rho (legacy)
  ↓
ECM Stage 1+2
```

**Threshold 调优建议**:
- 0.0 (default): 仅启用 telemetry 收集, 不实际 reject. 用于测量 predictor 假设
- 1e-12: 极保守, 仅 reject 100% 确定无法 smooth 的 case (u > 8 等)
- 1e-6 — 1e-9: 实用上限, 50d/60d 大 cofactor 大幅 prune. 实测前 reg-test 25d/50d
- 1e-3 — 1e-2: 高侵略, 必然丢失部分真 smooth relations. 仅在用户接受 sieve loop 多 round 时合理

**正确性保证**:
- threshold == 0 path 等价于 filter OFF (严格 invariant). 测试 `test_env_threshold_zero_invariant`
- Dickman ρ 是估算 (非精确), 启用后可能 false-negative (误 reject 真 smooth).
  这是用户 ROI 选择, 默认 0.0 保守
- predictor pass 仍走完整 cofactor pipeline, 不会因 predictor pass 跳过任何分解步骤

**Telemetry (`SurvivalPredictorStats` atomic)**:
- `predictor_rejects`: predictor 早 reject 的 cofactor 数
- `predictor_passes_then_smooth`: predictor 通过 + cofactor 真 smooth (好 pass)
- `predictor_passes_then_failed`: predictor 通过 + cofactor 不 smooth (浪费 cofactor cost,
  但是必要的 — predictor 不会因此误 reject)
- pipeline 结束可输出 `[survival_pred] rejected=X, smooth=Y, failed=Z` 行

**集成点** (commits `2fc977a` → `b6850d7`, 2026-05-21):
- `include/gnfs/cofactor/survival_predictor.hpp` — Dickman ρ + estimate_survival
  + should_reject_cofactor + SurvivalPredictorStats
- `include/gnfs/cofactor/smooth_check.hpp` — `classify_cofactor` 新 `smoothness_bound`
  参数 (default 0 = disabled) + survival predictor 早 reject 分支 + RAII PassRecorder
- `tests/test_survival_predictor.cpp` — 16 tests (4 dickman + 4 estimate + 2 env
  + 4 integration + 2 perf info)

**Default OFF**: 任何 caller 不传 `smoothness_bound` (或传 0) 时 predictor 完全跳过,
零开销, 零行为变化. classify_cofactor 现有调用者无需更新即保持原 behavior. 仅在
Pipeline / sieve loop wire-in `smoothness_bound = params.smoothness_bound_B` 时启用.

### Cofactor per-stage timing telemetry (GNFS_COFACTOR_TIMING_ENABLE)

**ENV `GNFS_COFACTOR_TIMING_ENABLE={0,1}`** (2026-05-22 实施, W12 T5, default 0):
余因子 pipeline 6 个 stage (TrialDivision, SQUFOF, BrentPollardRho, PollardRho,
EcmStage1, EcmStage2) 的 wall-time 与 call-count 累积器. 每个 stage 一个
`std::atomic<uint64_t>` 纳秒计数 + 一个 `std::atomic<uint64_t>` 调用计数,
RAII `StageTimer` 在 scope 入口采 steady_clock, scope 退出累加 elapsed 到
对应 stage. 关闭时 (默认) 完全零开销 — ctor 与 dtor 都不调 `steady_clock::now()`,
不访问 atomic 计数. 仅当用户 `GNFS_COFACTOR_TIMING_ENABLE=1` 显式启用时才采集.

```bash
GNFS_COFACTOR_TIMING_ENABLE=1 ./gnfs <N>   # 启用 telemetry, scope 入退采样
unset GNFS_COFACTOR_TIMING_ENABLE          # default OFF, 零开销
GNFS_COFACTOR_TIMING_ENABLE=0 ./gnfs <N>   # 显式 disable (= default)
```

**Helper API** (`include/gnfs/cofactor/stage_timing.hpp`):
- `enum class CofactorStage`: `TrialDivision` (0), `Squfof` (1),
  `BrentPollardRho` (2), `PollardRho` (3), `EcmStage1` (4), `EcmStage2` (5),
  `kNumStages` (6, sentinel).
- `stage_name(stage)` — human-readable 名称 (`"trial"`, `"squfof"`,
  `"brent_rho"`, `"pollard_rho"`, `"ecm_s1"`, `"ecm_s2"`).
- `struct StageTimingStats` — 进程单例, 6 个 atomic ns 累加器 + 6 个 atomic
  调用计数. `total_ns_for(stage)` / `call_count_for(stage)` / `reset()`.
- `cofactor_timing_enabled()` — cached `std::call_once` + `std::atomic<bool>`
  ENV reader. 严格仅 "1" 启用.
- `cofactor_timing_stats()` — process-singleton 访问器 (function-local
  static, 与 `survival_stats()` 同 idiom).
- `cofactor_timing_reset_env_cache_for_testing()` — 测试专用 re-resolve ENV.
- `class StageTimer` — RAII 测时. Ctor 在 enabled 时采 steady_clock, dtor
  累加 elapsed_ns 与 +1 call_count. Non-copyable, movable. `moved_from_`
  标志防 move 后双重计数.
- `format_cofactor_timing_summary()` — 单行格式化:
  `[cofactor_timing] trial=<ns>ns/<calls>calls squfof=... ...`. 关闭时返回
  `"[cofactor_timing] disabled"`.
- `print_cofactor_timing_summary()` — 写 stderr + `std::flush`.

**Process-singleton storage 策略**:
- `cofactor_timing_stats()` 用 function-local static `StageTimingStats stats`
  (与 `survival_predictor.hpp::survival_stats()` 同 pattern). 优势: 保证
  thread-safe C++11 一次性初始化, ODR-safe 跨 TU. 选 fn-local static 而非
  `inline namespace var` 仅因为 cofactor 模块其他 telemetry 单例已经
  约定如此, 保持一致.

**Memory ordering**:
- 所有原子操作用 `std::memory_order_relaxed`. Telemetry 不驱动 control
  flow, 仅用于 format summary; 不同线程的累加最终一致即可, 不需要 release/acquire
  fence 制造 happens-before 关系.

**与 W5 T5 GNFS_SURVIVAL_FILTER 互补**:
- W5 T5 survival predictor 估算 cofactor 是否值得跑全 pipeline (前置筛选).
- W12 T5 telemetry 测量 cofactor pipeline 各 stage 真实耗时.
- 二者组合让用户调 `GNFS_SURVIVAL_THRESHOLD` 时观察"提高 threshold 是否
  真的把 ECM Stage 2 的累计 wall-time 砍掉 70%". 默认 OFF 时二者都零开销.

**Bit-for-bit guarantee**: telemetry 不改变 cofactor pipeline 任何行为,
仅累加测量数据. enabled / disabled 状态对 `classify_cofactor` 等 cofactor
入口的输出 (CofactorClassification) 严格一致. helper 不修改任何 cofactor
算法文件.

**Nested timer 语义**: 嵌套 `StageTimer` (e.g. TrialDivision 内嵌 EcmStage1)
各自独立累加. 外层 timer 包含内层时间 (调用方按需放置 timer 决定归属).

**Concurrent timer**: 多 thread 各自构造 `StageTimer`, 相同 stage 的 atomic
计数无锁累加. 4 thread × 100 timers 强制测试通过.

**集成点** (W12 T5, 2026-05-22):
- `include/gnfs/cofactor/stage_timing.hpp` — 240+ 行 header-only, 6-stage
  enum + atomic stats + RAII timer + ENV gate + summary formatter
- `tests/test_cofactor_stage_timing.cpp` — 16 tests (5 ENV / disabled 不动
  计数 / enabled 累加正确 / 嵌套独立 / 4 thread × 100 并发 / format 关 vs
  开 / reset zeros / move-construct 不重复 / move-assign 完成前次 / stage_name
  lookup / perf info scope overhead)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  cofactor 模块

**Default OFF**: ENV unset → `cofactor_timing_enabled() == false` →
`StageTimer` ctor 仅 1 atomic load 后立即返回 (不采 clock), dtor 同样
no-op. 主路径 wall-time 与 legacy 等价, 零行为变化. 当前主 pipeline 无
wire-in 调用, 是 future-infrastructure. 调用方在自身 cofactor stage scope
入口 `StageTimer t(CofactorStage::EcmStage1);` 即可启用归属.

### Sieve bucket prefetch (GNFS_BUCKET_PREFETCH)

**ENV `GNFS_BUCKET_PREFETCH=auto|0|1`** (2026-05-21 实施, default auto):
Lattice sieve bucket scatter / gather 热路径插入 `__builtin_prefetch` hint,
look-ahead 距离 `kBucketPrefetchDistance=8` iterations. Scatter phase
write-intent prefetch 目标 region vector metadata; gather phase
read-intent prefetch 目标 `sieve_array_` 累加位置. Prefetch 是 hint —
sieve_array_ 内容和 candidate list 与 ENV 状态无关, bit-for-bit 一致.

```bash
GNFS_BUCKET_PREFETCH=auto ./gnfs <N>   # 默认: __builtin_prefetch 可用则启用
GNFS_BUCKET_PREFETCH=0    ./gnfs <N>   # 强制 scalar (回归 bisect 用)
GNFS_BUCKET_PREFETCH=1    ./gnfs <N>   # 强制 prefetch (无 builtin 平台 fallback)
unset GNFS_BUCKET_PREFETCH             # 同 auto
```

**Helper API** (`include/gnfs/sieve/bucket_prefetch.hpp`):
- `prefetch_bucket_write(ptr)` — `__builtin_prefetch(ptr, 1, 1)`, write hint
  T1 locality, 用于 scatter phase 的 region vector metadata.
- `prefetch_bucket_read(ptr)` — `__builtin_prefetch(ptr, 0, 1)`, read hint
  T1 locality, 用于 gather phase 的 `sieve_array_` 累加位置.
- `bucket_prefetch_enabled()` — cached `std::once_flag` + `std::atomic<bool>`,
  内循环 branch on stack-local 副本避免每次重读 atomic.
- `bucket_prefetch_supported()` — compile-time `__GNUC__/__clang__` 探测.
- `reload_bucket_prefetch_gate()` — 测试专用 re-resolve ENV.
- `kBucketPrefetchDistance = 8` — look-ahead 距离 (实测 M-series cores 优).

**ROI 与定位**:
- 主要 ROI: scatter / gather 的目标地址跨 bucket region 边界跳跃, hardware
  prefetcher 无法预测 — software prefetch 把 L2/L3 miss 提前 8 个 iteration
  发起, 让 fill 与当前 iteration 的 arithmetic 重叠.
- 50d/60d Phase 3 sieve 受益最大: large factor base → bucket region 路径
  主导, scatter pattern 跨多个 16K-region 跳跃 cache miss 严重.
- 40-bit fixture 实测 single SQ wall-time: prefetch_off=1429ms, prefetch_on=
  1415ms (~1% 改进, large-N 应更显著). 不是 SIMD-style 显著加速, 但 cache
  miss reduction 对长时间 sieve 累积效应大.

**集成点** (commits `a7f944c` → `c827cce`, 2026-05-21):
- `include/gnfs/sieve/bucket_prefetch.hpp` — helper API + ENV gate.
- `include/gnfs/sieve/lattice_sieve.hpp` — 三个 prefetch site:
  `sieve_bucket_region` scatter (region vector write hint),
  `sieve_bucket_region` apply (sieve_array_ read hint), 和
  `sieve_row_chunk` 大素数 bucket apply (sieve_array_ read hint). 每个
  site 把 `bucket_prefetch_enabled()` 提到 chunk/scatter 入口外部, 内循环
  branch on stack-local boolean.
- `tests/test_bucket_prefetch.cpp` — 7 个测试: 40-bit bucket region parity /
  40-bit row-major parity / multi-SQ parity / 27-bit small N parity /
  ENV `0` disable / ENV `1`/`auto`/unset enable / perf info (no assert).
  parity 比较通过 sorted `(a, b, i, j, residual)` tuple 严格匹配.

**Default ON (auto)**: 对所有 sieve 调用方透明启用 prefetch hint, 编译器
不支持 `__builtin_prefetch` 时自动退到 no-op. sieve output 不变.

### Murphy E alpha 并行 (GNFS_MURPHY_ALPHA_THREADS)

**ENV `GNFS_MURPHY_ALPHA_THREADS=N`** (2026-05-18 实施, lightweight optimization):
MurphyEvaluator::compute_alpha 用 ThreadPool 并行扫 ~78k primes. 每 thread
accumulates partial double, 序列 reduce.

```bash
GNFS_MURPHY_ALPHA_THREADS=0 ./gnfs <N>   # 序列 (debug / 单线程对照)
GNFS_MURPHY_ALPHA_THREADS=8 ./gnfs <N>   # 显式 8-thread
# 默认: hardware_concurrency
```

**ROI**: M5 10-core → 5-7x compute_alpha speedup (CZ求根 perfect embarrassingly
parallel by prime). Kleinjung selector + 多 polynomial 评估时 sieve 主流程
wall-time 显著缩短.

**集成点** (commit `0dd1799`, 2026-05-18):
- `include/gnfs/polynomial/murphy_evaluator.hpp` — `compute_alpha(f, prime_bound)` parallel sweep + `alpha_contribution(f, df, p)` thread-safe helper
- Lazy `std::once_flag` + `unique_ptr<ThreadPool>` per evaluator instance
- 回归测试 `tests/test_murphy.cpp` parallel == sequential invariant (commit `2ef928a`)

**Rotation-incremental 算法重构**: multi-day pure-math 工作仍 deferred.
当前 parallelization 是 orthogonal lightweight 加速, 不替代真正 incremental.

### Relation collector memory pool (GNFS_RELATION_POOL_SIZE)

**ENV `GNFS_RELATION_POOL_SIZE=N`** (2026-05-21 实施, W6 T4):
N 为初始 chunk 字节数, 启用 RelationCollector 内 `std::pmr::vector<Relation>`
backed by `std::pmr::monotonic_buffer_resource`. 默认 0 (unset / "0" / 非数字)
走原 `std::vector<Relation>` std::allocator 路径, 零开销.

```bash
GNFS_RELATION_POOL_SIZE=4194304 ./gnfs <N>        # 4 MiB initial chunk
GNFS_RELATION_POOL_SIZE=16777216 ./test_stress 1 1 # 16 MiB initial chunk
unset GNFS_RELATION_POOL_SIZE                      # 默认 OFF
```

**用途**: 50d/60d Round 2 sieve 期间 RelationCollector 频繁 `push_back` Relation
到 in-memory `std::vector` 触发反复 `malloc` + heap fragmentation, M5 多核
高并发 sieve worker 时尤其明显. Pool 一次性大 chunk 分配 (4-16 MiB),
后续 push 直接 bump pointer; chunk 耗尽时 fallback `std::pmr::new_delete_resource`
做几何 chunk growth. 减少 outer-vector reallocation 次数 + 减小 fragmentation
pressure.

**实现细节**:
- `RelationPoolResource` (RAII wrapper) 持有 `std::pmr::monotonic_buffer_resource`.
  Default initial chunk = 4 MiB. Upstream = `std::pmr::new_delete_resource`.
  Non-copyable, movable. `reset()` 释放全部 chunks 并重 allocate.
- `CollectorConfig::use_pool` / `pool_initial_bytes` 暴露 opt-in; defaults
  pick up ENV at default-construction (`util::relation_pool_enabled()` /
  `util::relation_pool_size_bytes()`). ENV 解析 cached via `std::call_once` +
  `std::atomic<size_t>`, 每进程 1 次 getenv 命中.
- Collector 内部双路径: `std::vector<Relation> relations_` (default) 与
  `std::unique_ptr<std::pmr::vector<Relation>> relations_pmr_` (pool mode).
  公开 API (`add()` / `get_relations()` / `size()` / `empty()` / `clear()` /
  `save()` / `load()` / `merge`) 全部 transparent — `get_relations()` 在
  pool mode copy pmr → `std::vector<Relation>` 返回, 调用方无感.
- **Bit-for-bit guarantee**: 同 `(a,b)` 输入序列, pool ON vs OFF 产生完全相同
  的 Relation 集合 (按 `(a,b)` sort 后逐字段相同). 单元测试强制 (`test_relation_pool_integration`).

**与 OOC 兼容**:
- OOC 模式 (`GNFS_OOC_RELATIONS=1`) 不维护 in-memory `relations_` (写盘),
  pool 在 OOC 模式下不激活 (省 RAM). Ctor 内 `use_pool && !ooc_enabled` 检查保证.
- Pool 与 OOC 互斥设计: 二者解决不同问题 — pool 减小 in-memory fragmentation,
  OOC 减小 in-memory peak. 同时启用 pool 仅浪费 chunk 内存.

**集成点** (W6 T4, 2026-05-21):
- `include/gnfs/util/memory_pool.hpp` — `RelationPoolResource` RAII +
  `relation_pool_size_bytes()` / `relation_pool_enabled()` ENV reader (cached) +
  test-only `relation_pool_reset_env_cache_for_testing()`
- `include/gnfs/relation/collector.hpp` — CollectorConfig fields + ctor 路由 +
  `relations_pmr_` field + 全部公开 API dispatch
- `tests/test_memory_pool.cpp` — 10 unit tests (RAII / reset / move /
  pmr::vector<int> usage / chunk overflow / many small allocations /
  ENV parsing 4 variants)
- `tests/test_relation_pool_integration.cpp` — 4 correctness tests
  (small / medium / stats parity / clear recycle) + 2 perf-info probes
  (push_back 100k OFF vs ON, 10k 多 chunk size 对比)

**Default OFF**: ENV unset → `use_pool = false` → `std::allocator` path 完整保留,
零回归风险. 仅 50d+ sieve 期间高并发 push 时启用.

### Integer thread-local scratch pool (GNFS_INTEGER_SCRATCH_POOL)

**ENV `GNFS_INTEGER_SCRATCH_POOL={0,1}`** (2026-05-22 实施, default 0):
GNFS hot path (cofactor pipeline / Hensel lift / Schirokauer compute /
ECM arithmetic) 大量临时 `gnfs::core::Integer` 对象, 每次 ctor 走 `mpz_init`,
dtor 走 `mpz_clear`. GMP 的 limb buffer 在 `mpz_clear` 时释放, tight loop
反复 init/clear 触发 GMP malloc + heap fragmentation, M5 多 core 高并发
ECM/cofactor 路径尤其明显.

```bash
GNFS_INTEGER_SCRATCH_POOL=1 ./gnfs <N>   # 启用 per-thread Integer pool
unset GNFS_INTEGER_SCRATCH_POOL          # 默认 OFF (零开销)
```

**Helper API** (`include/gnfs/util/integer_scratch_pool.hpp`):
- `integer_scratch_pool_enabled()` — cached ENV reader (`std::call_once` +
  `std::atomic<bool>`), 严格仅 "1" 启用, 其它值 (unset / "0" / "true" /
  非数字 / 空串) 均视为 OFF.
- `IntegerScratchHandle` — RAII borrow handle, ctor 从 thread_local pool 取
  (或 fresh default-construct), dtor 还回 pool (启用时). 提供 `get()` /
  `operator*` / `operator->` 直接访问内部 `Integer`.
- `integer_scratch_pool_size()` — 当前线程 pool 中 Integer 数 (测试 / debug 用).
- `integer_scratch_pool_clear()` — 释放当前线程 pool 全部 Integer.
- `integer_scratch_pool_reset_env_cache_for_testing()` — 测试专用 re-resolve ENV.

**实现细节**:
- `inline thread_local std::vector<gnfs::core::Integer> tls_scratch_pool` —
  per-thread 存储, C++17 `inline` 保证多 TU 单实例. 线程退出时 vector
  dtor 跑, 每个 pooled Integer dtor → `mpz_clear` → limb buffer 释放, 不泄漏.
- Borrow 时: pool 非空 → pop_back 并 `mpz_set_si(value, 0)` 重置 (保留 limb
  buffer, GMP 不 realloc). Pool 空 → 默认构造 fresh Integer (走原 `mpz_init`).
- Return 时 (dtor): 启用时 push_back 到 pool, 不 clear. 下次 borrow 重置.
- Move semantics: moved-from handle 标 `returned_ = true`, dtor 跳过 push,
  避免 double-return. Move-assign 释放当前 Integer 后再 adopt 新.
- Pool 是 per-thread, **无锁**. 不同 thread 的 pool 完全隔离.

**Bit-for-bit guarantee**: 同一 `(a, b, ...)` 输入序列, pool ON vs OFF 产生
完全相同的 Integer 值与最终计算结果. 单元测试 `test_integer_scratch_pool`
强制覆盖 (1000 random int64_t 值, OFF vs ON 完全相同 `to_string()` 输出).

**ROI 与定位**:
- 主要 ROI: 避免 GMP limb buffer 反复 malloc/free. Integer struct header
  本身只是 stack-allocated 几个字 (`mp_size_t _mp_alloc, _mp_size; mp_limb_t* _mp_d`),
  真正的 heap 分配在 `_mp_d` (limb buffer). Pool 借出时 limb buffer 已存在,
  GMP `mpz_set_*` 在新值 ≤ 旧分配时不重新 malloc, 直接复用.
- 主要受益场景: cofactor pipeline (trial / ECM / SQUFOF) 跑 hundreds-of-thousands
  迭代的 Integer 临时变量, hot loop 频繁 5-10 字 limb buffer churn.
- Hensel lift / Schirokauer maps 内部已用 `Integer` RAII, pool 加在 outer
  loop 减小 outer alloc pressure.
- 默认 OFF: 任何 caller 不设 ENV 时, `IntegerScratchHandle` 行为与 fresh
  Integer 完全等价 (`get()` 返回 zero-initialized Integer, dtor 走 RAII).
  零行为变化, helper-only, 不影响主路径.
- 当前主 pipeline 不调用 helper, 是 future-infra 阶段. 调用方需在自己的
  hot path 显式 wire-in `IntegerScratchHandle` 替代裸 `Integer tmp`.

**线程退出安全**:
- `thread_local std::vector<Integer>` 在 thread exit 时析构, 释放所有 limb buffer.
- `IntegerScratchHandle::~IntegerScratchHandle()` push_back 抛异常 (e.g. OOM)
  时吞掉 ([以保证 noexcept dtor 性质]), 让 Integer dtor 走 RAII 清理.

**集成点** (W8 T5, 2026-05-22):
- `include/gnfs/util/integer_scratch_pool.hpp` — 260 行 header-only, RAII +
  ENV gate + thread_local pool + move semantics
- `tests/test_integer_scratch_pool.cpp` — 13 个测试 (4 ENV + 5 borrow / return /
  growth bound + 1 bit-for-bit parity 1000 values + 3 edge cases 含 move /
  clear / reset cache + 1 perf-info probe 100k cycles OFF vs ON)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout

**Default OFF**: ENV unset → `integer_scratch_pool_enabled()` 返回 false →
borrow handle 退到 fresh Integer + skip pool push, `std::allocator` path
完整保留, 零回归风险. 仅显式 `GNFS_INTEGER_SCRATCH_POOL=1` 时启用.

### ECM sigma seed warm pool (GNFS_ECM_SIGMA_POOL_SIZE)

**ENV `GNFS_ECM_SIGMA_POOL_SIZE=N`** (2026-05-22 实施, W10 T3, range [0, 1024], default 0):
ECM Suyama curve setup 入口选 sigma (>=6) 时, 生产 caller 走 PRNG (典型
`std::mt19937_64` seeded from `std::random_device ^ n_low`). 50d+/60d
cofactor 紧凑 retry loop 中, PRNG state advance (mt19937_64 624-word ring)
变成 inner loop 的 serial dependency, 限制跨 sigma attempt 的 ILP. helper
提供 opt-in per-thread sigma seed warm pool, 让 caller 把 N 个 PRNG draw
bulk refill 后 LIFO `pop_back` 一次性 amortise.

```bash
unset GNFS_ECM_SIGMA_POOL_SIZE              # default 0 (disabled, 零开销)
GNFS_ECM_SIGMA_POOL_SIZE=0    ./gnfs <N>    # 同 default
GNFS_ECM_SIGMA_POOL_SIZE=100  ./gnfs <N>    # per-thread 容量 100
GNFS_ECM_SIGMA_POOL_SIZE=1025 ./gnfs <N>    # clamp 到 1024 上限
```

**Helper API** (`include/gnfs/cofactor/sigma_seed_pool.hpp`):
- `sigma_seed_pool_size()` — cached ENV pool 容量, 0 表 disabled
- `sigma_seed_pool_enabled()` — `pool_size() > 0` 等价 predicate
- `refill_sigma_seed_pool(generator)` — 启用时 thread_local pool 用
  `generator()` 填到 capacity (空, 已满, 或禁用时 no-op)
- `get_next_sigma_seed(fresh)` — 启用且 pool 非空: `pop_back` LIFO; 禁用
  或空: 返回 `fresh` 参数. 调用方负责生成 `fresh` 作为 fallback (无 PRNG
  绑定耦合)
- `sigma_seed_pool_remaining()` / `sigma_seed_pool_clear()` — 测试 / debug
- `sigma_seed_pool_reset_env_cache_for_testing()` — 测试 re-resolve hook

**ENV parsing** (`std::stoi`-based, cached `std::call_once`):
- unset / "" / "0" / 负数 / leading 非数字 (`garbage`) → 0 (disabled)
- 1..1024 → as-is
- 1025+ → 1024 (clamp)
- 数字前缀 ("12abc"): 取首数字段 → 12 (std::stoi 接受). 文档化, 但 caller
  应传 clean 整数值, 不依赖 partial-parse 行为

**实现细节**:
- `inline thread_local std::vector<uint64_t> tls_sigma_pool` — per-thread
  存储, C++17 `inline` 保证多 TU 单实例. 线程退出时 vector dtor 跑, uint64_t
  无资源 ownership 故 teardown trivial, 不泄漏.
- `refill`: pool 已满 → no-op (不二次调用 generator). 调用 generator
  `(capacity - current_size)` 次 `push_back`. generator 抛异常 → propagate,
  pool 保持 partial-fill consistent 状态.
- `get_next`: 启用 + pool 非空 → `pop_back` LIFO. 禁用 / pool 空 → 返回 fresh.
- Pool 是 per-thread, **无锁**. 不同 thread 的 pool 完全隔离.

**Bit-for-bit guarantee (within deterministic generator)**:
- helper 不保证与 OFF 路径 sigma 序列完全相同. PRNG generator 在 refill
  时被 bulk-invoke, 与 OFF 路径 per-attempt invoke 调度不同.
- 给定 deterministic generator (e.g. fixed-seed mt19937_64), refill 后
  连续 `get_next` 返回的序列 bit-for-bit 等于
  reversed([gen(), gen(), ..., gen()]) (LIFO 顺序). 由
  `tests/test_sigma_seed_pool.cpp::test_mt19937_generator_consistent`
  强制覆盖.
- Caller 若需严格 deterministic sigma 序列, 应禁用 pool 或 refill from
  deterministic generator 并把 pool 当作 source of truth.

**ROI 与定位**:
- 主要 ROI: 紧凑 ECM retry loop 中 PRNG state advance amortise. mt19937_64
  per-call cost 几十 cycle, 在 small absolute 但 inner loop branch
  prediction defeat + serial dependency.
- helper 当前 standalone (主 pipeline `ECM::factor` / `EcmCurvePool::
  prepare_batch` 未 wire-in), 是 future-infrastructure. caller wire-in
  时把 inner loop `rng()` 替换为 `get_next_sigma_seed(rng())`, 并在外层
  attempt round 入口 `refill_sigma_seed_pool([&rng]() { return rng(); })`.
- Helper 与 W8 T1/W9 T1 `GNFS_ECM_STAGE{1,2}_PARALLEL` 完全 orthogonal —
  并行 dispatcher 跑多条 curve, helper 是 per-thread sigma 池, 二者可
  同时启用.

**集成点** (W10 T3, 2026-05-22):
- `include/gnfs/cofactor/sigma_seed_pool.hpp` — 260 行 header-only,
  thread_local pool + ENV gate + LIFO `pop_back` 语义
- `tests/test_sigma_seed_pool.cpp` — 15 个测试 (5 ENV + 6 行为 (OFF/empty/
  LIFO/exhaust/clear/full no-op) + 1 multi-thread isolation + 1 generator
  exception + 1 env cache reset + 1 mt19937 round-trip)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  cofactor 模块

**Default OFF (N=0)**: ENV unset → `sigma_seed_pool_size() == 0` →
`get_next_sigma_seed(fresh)` 总是返回 fresh, `refill_sigma_seed_pool`
no-op 不调 generator. caller 主路径行为完全等同 legacy, 零开销, 零行为
变化. 仅 helper 被 wire-in + 用户 explicit `GNFS_ECM_SIGMA_POOL_SIZE=N>=1`
时启用.

### Hensel lift K-prime slot 并行 (GNFS_SQRT_HENSEL_THREADS)

**ENV `GNFS_SQRT_HENSEL_THREADS=N`** (2026-05-21 实施, default 1, range [1, hardware_concurrency * 2]):
Nguyen Hybrid algebraic sqrt 的 K=3 inert-prime slot 各自做 Hensel lift,
slot 之间相互独立 (embarrassingly parallel). N=1 (默认) 走 sequential per-slot
循环, 不创建 ThreadPool, 零开销保留原行为. N>=2 时把 K 个 slot dispatch 到
大小为 min(N, K) 的 ThreadPool, slot 之间靠 future 同步收口.

```bash
GNFS_SQRT_HENSEL_THREADS=1 ./gnfs <N>    # default sequential, zero overhead
GNFS_SQRT_HENSEL_THREADS=2 ./gnfs <N>    # 2 outer workers, inner_threads = hw / 2
GNFS_SQRT_HENSEL_THREADS=4 ./gnfs <N>    # 4 outer workers, inner_threads = hw / 3 (cap at K)
unset GNFS_SQRT_HENSEL_THREADS           # same as N=1
```

**并行模型**:
- Outer = `parallel_hensel_lift(slots, lift_one)` over K=3 inert-prime slots
- Inner = `hensel_lift_single_prime` 自身的 ThreadPool (poly_mul_mod /
  compute_product_mod_parallel), 受 `inner_threads = hw / min(outer, K)` 限制
  保持 `outer * inner <= hw` 不超订
- Slot state pure-function: 每个 slot 独占 LiftResult buffer; lift_one 仅读
  shared ab_pairs / NumberField. CRT 在 outer 之后单线程 reduce.

**Bit-for-bit guarantee**: K 个 LiftResult 仅依赖 per-slot index + read-only
inputs, sequential 与 parallel 路径产物完全相同, downstream CRT/sign-search
输出 sqrt(N) 严格一致. 由 `tests/test_hensel_parallel.cpp` 强制覆盖 (N=1
vs N=2 / N=4 bit-for-bit assert).

**ROI 与定位**:
- 主要 ROI: 大 K 大 ab_pairs 时 Phase 7 wall-time 由 3 倍 single-prime
  lift 时间 → 1 倍 + tasking overhead. M5 P-core 三 lift 并发 ≈ 单 lift 时间
  (实测 small case 1ms 级别,大 case 待 stress 验证).
- ROI 主要在 50d+ stress 路径, 25d gate 多数情况下走 single-prime fallback
  (ab_pairs < 100 阈值), 不进入 Nguyen hybrid.
- Default OFF (N=1) 保证 zero behavior change for legacy callers, 仅当用户
  明确 opt-in 时启用.

**集成点** (commits `8feb2de` → `1cc8704`, 2026-05-21):
- `include/gnfs/sqrt/hensel_parallel.hpp` — `sqrt_hensel_threads()` env reader
  with `std::once_flag` cache + `parallel_hensel_lift<Slot, Func>` dispatcher
- `include/gnfs/sqrt/hensel_sqrt.hpp` — `compute_nguyen_hybrid` 初始 lift
  dispatch 通过 helper (替代旧 raw `std::thread`), inner thread budget
  recompute 基于 runtime outer count
- `tests/test_hensel_parallel.cpp` — 4 correctness (N=1 vs N=2/N=4 +
  small-ab_pairs fallback) + 2 env parsing + 1 perf info + 1 edge case
  (single-slot, empty-span)

### Schirokauer map 每关系并行 (GNFS_SCHIROKAUER_THREADS)

**ENV `GNFS_SCHIROKAUER_THREADS=N`** (2026-05-22 实施, default 1, range [1, hardware_concurrency * 2]):
Schirokauer map computation 每关系独立 (`compute_flat(a, b)` 是 const pure 函数,
仅读 immutable `prime_info_` + scalar 输入, 无 shared mutable state). N=1 (默认)
走 sequential 路径, 不创建 ThreadPool, 零开销保留原行为. N>=2 时 batch helper
`compute_schirokauer_flat_batch(map, ab_pairs)` 把 relations dispatch 到
`min(N, ab_pairs.size())` 个 ThreadPool worker, 输出按 input index 完全一致.

```bash
GNFS_SCHIROKAUER_THREADS=1 ./gnfs <N>    # default sequential, zero overhead
GNFS_SCHIROKAUER_THREADS=4 ./gnfs <N>    # 4 workers, dispatch via parallel_for_index
GNFS_SCHIROKAUER_THREADS=8 ./gnfs <N>    # 8 workers
unset GNFS_SCHIROKAUER_THREADS           # same as N=1
```

**并行模型**:
- Batch entry = `compute_schirokauer_flat_batch(map, ab_pairs)` over n relations
- Inner = `ThreadPool::parallel_for_index(0, n, lambda)` 直接调度 (chunk-based)
- 每个 task 调 `map.compute_flat(a_i, b_i)` 写到 `out[i]` (disjoint per index)
- 空 batch (n==0) 与 单 relation (n==1) 都走 sequential 短路, 不创建 pool

**Bit-for-bit guarantee**: `compute_flat` 是 deterministic pure function over
`(a, b) + prime_info_`, prime_info_ ctor 后 immutable. 同一组 ab_pairs 在 N=1
与 N>=2 路径输出 `vector<vector<uint32_t>>` 按 index/column bit-for-bit 一致.
由 `tests/test_schirokauer_parallel.cpp` 强制覆盖 (500 pair N=1 vs N=4 /
N=hw_concurrency 严格 assert).

**ROI 与定位**:
- 主要 ROI: matrix_builder 主循环 1M+ relations × Schirokauer compute (FastPoly
  power 4-byte ^ 256-byte exponent), 是 Phase 5 矩阵构建的热点之一. M5 10-core
  实测 2000 pair batch 约 2.3x speedup (perf-info, 非 assert).
- 默认 OFF (N=1): 保证 zero behavior change for legacy callers. matrix_builder
  主循环本身已有 `ThreadPool::parallel_for_index` 在 row 级别 (见
  `include/gnfs/linalg/matrix_builder.hpp:315`), 因此 ENV 默认不启用避免与
  外层 pool 双重 oversubscription. 仅在显式 opt-in 实验场景启用.
- Pool 与 OOC 等其他 ENV 无冲突 — 仅 schirokauer 自身的 helper API.

**集成点** (2026-05-22):
- `include/gnfs/linalg/schirokauer_parallel.hpp` — `schirokauer_threads()` env
  reader with `std::once_flag` cache + `compute_schirokauer_flat_batch<ABPair>`
  template dispatcher + `schirokauer_threads_reset_env_cache_for_testing()`
- `include/gnfs/linalg/schirokauer.hpp` — 未改动 (核心算法保持 bit-identical)
- `tests/test_schirokauer_parallel.cpp` — 7 个测试 (baseline + 2 parity +
  env parsing + empty + single + perf info)

### ECM Stage 2 多曲线并行 (GNFS_ECM_STAGE2_PARALLEL)

**ENV `GNFS_ECM_STAGE2_PARALLEL=N`** (2026-05-22 实施, default 1, range [1, hardware_concurrency * 2]):
ECM Stage 2 (Baby-Step Giant-Step) 在多条曲线之间相互独立 (embarrassingly
parallel). N=1 (默认) 走 sequential per-curve 循环, 不创建 ThreadPool,
零开销保留原行为. N>=2 时把 K 条曲线 dispatch 到大小为 min(N, K) 的
ThreadPool, 曲线之间靠 future 同步收口.

```bash
GNFS_ECM_STAGE2_PARALLEL=1 ./gnfs <N>    # default sequential, zero overhead
GNFS_ECM_STAGE2_PARALLEL=4 ./gnfs <N>    # 4 outer workers for Stage 2 BSGS
GNFS_ECM_STAGE2_PARALLEL=8 ./gnfs <N>    # 8 outer workers
unset GNFS_ECM_STAGE2_PARALLEL           # same as N=1
```

**并行模型**:
- Outer = `parallel_stage2_curves<Result, Curve, Func>(curves, run_stage2)`
  over K 条独立曲线 (每条已完成 Stage 1, post-Stage-1 Point + a24 准备好)
- 内部 Stage 2 BSGS / Brent-Suyama 算法 bit-identical (helper 仅改变外层
  dispatch, 不触碰 `ECM::stage2` / `ECM::stage2_brent_suyama` 内核)
- 每条曲线 task 拥有独立 Integer buffer 与 Point 状态, GMP `mpz_*` 调用
  操作数互不重叠, 满足 GMP per-call disjoint-operands thread-safety

**Bit-for-bit guarantee**: 每条曲线 (sigma, n, B1, B2) 的 Stage 2 结果是
该 sigma 的 pure function, 不依赖 dispatch 顺序. Sequential (N=1) 与
parallel (N>=2) 路径产生的 per-index `std::optional<Integer>` 完全一致,
factor 集合严格相同. 由 `tests/test_ecm_stage2_parallel.cpp` 强制覆盖
(N=1 vs N=4 vs N=hw bit-identical per-index assert).

**ROI 与定位**:
- 主要 ROI: 50d+/60d 余因子 B2 较大时 (B2=1e8 ~ B2=5e9), Stage 2 wall-time
  显著超过 Stage 1. Stage 1 已有 `EcmCurvePool` 多曲线 warm-pool, Stage 2
  此前 sequential 是真实 gap.
- Stage 1 行为完全不变 (`EcmCurvePool` / `try_curve_with_pk` 语义保持).
- Helper 是 opt-in 工具, 不修改 `ECM::factor` / `ECM::quick_factor` /
  `ECM::factor_with_batch` public path. 调用方在自身循环里 wire-in 即可.
- Default OFF (N=1) 保证 zero behavior change for legacy callers, 仅当用户
  明确 opt-in 时启用.

**集成点** (commits `c663ed7` → `6caba7f`, 2026-05-22):
- `include/gnfs/cofactor/ecm_stage2_parallel.hpp` — `ecm_stage2_parallel_threads()`
  env reader with `std::once_flag` cache + `parallel_stage2_curves<>` template
  dispatcher + `ecm_stage2_parallel_reset_env_cache_for_testing()` test hook
- `tests/test_ecm_stage2_parallel.cpp` — 6 个测试 (N=1 baseline factor /
  N=1 vs N=4 parity / N=1 vs N=hw parity / ENV parsing / empty span /
  single-curve N=4 no-stall)

### ECM Stage 1 多曲线并行 (GNFS_ECM_STAGE1_PARALLEL_THREADS)

**ENV `GNFS_ECM_STAGE1_PARALLEL_THREADS=N`** (2026-05-22 实施, W9 T1, default 1, range [1, hardware_concurrency * 2]):
ECM Stage 1 (Lucas-chain Montgomery ladder, 即 `try_curve_with_pk` 内的
scalar-multiplication `k * Q`) 在多条曲线之间相互独立 (embarrassingly
parallel). N=1 (默认) 走 sequential per-curve 循环, 不创建 ThreadPool,
零开销保留原行为. N>=2 时把 K 条曲线 dispatch 到大小为 min(N, K) 的
ThreadPool, 曲线之间靠 future 同步收口.

```bash
GNFS_ECM_STAGE1_PARALLEL_THREADS=1 ./gnfs <N>    # default sequential, zero overhead
GNFS_ECM_STAGE1_PARALLEL_THREADS=4 ./gnfs <N>    # 4 outer workers for Stage 1 Montgomery ladder
GNFS_ECM_STAGE1_PARALLEL_THREADS=8 ./gnfs <N>    # 8 outer workers
unset GNFS_ECM_STAGE1_PARALLEL_THREADS           # same as N=1
```

**并行模型**:
- Outer = `parallel_stage1_curves<Result, Curve, Func>(curves, run_stage1)`
  over K 条独立曲线 (每条由 caller 提供 (sigma, n, B1) setup tuple)
- 内部 Stage 1 Lucas-chain / Montgomery ladder 算法 bit-identical (helper
  仅改变外层 dispatch, 不触碰 `ECM::stage1` / `try_curve_with_pk` 内核)
- 每条曲线 task 拥有独立 Integer buffer 与 Point 状态, GMP `mpz_*` 调用
  操作数互不重叠, 满足 GMP per-call disjoint-operands thread-safety

**Bit-for-bit guarantee**: 每条曲线 (sigma, n, B1) 的 Stage 1 结果是该
sigma 的 pure function, 不依赖 dispatch 顺序. Sequential (N=1) 与
parallel (N>=2) 路径产生的 per-index `Result` 完全一致 (caller 选 Result
类型: 常见 `std::optional<Integer>` 表 "factor found / not found", 或
post-Stage-1 Point + a24 state 供下游 Stage 2 dispatch 复用). 由
`tests/test_ecm_stage1_parallel.cpp` 强制覆盖 (mock worker N=1 vs N=4 /
N=hw bit-identical per-index assert + 真实 ECM Stage 1 via
`factor_with_batch` N=1 vs N=4 per-curve `std::optional<Integer>` 严格一致).

**ROI 与定位**:
- 主要 ROI: 50d+/60d 余因子分解每条曲线的 Stage 1 Lucas chain (B1=10^6 ~
  10^9 时 chain 长 ~10^5 ~ 10^8 ladder step) wall-time 可观, K 条曲线
  并发后 outer wall ~ T_single + tasking overhead, 替代 K * T_single
  sequential 累计.
- 与 W8 T1 `GNFS_ECM_STAGE2_PARALLEL` 正交: Stage 1 + Stage 2 二者各自有
  独立 ENV 控制, caller 可同时启用 (Stage 1 并发跑 K 条曲线, post-Stage-1
  Point 数据收口后再 dispatch 到 Stage 2 helper, 或在同一 task 内串接).
- 与 `EcmCurvePool` 不冲突: pool 是 Stage 1 warm-pool (预生成 sigma 池),
  helper 是 outer dispatch (跑多条曲线). pool 解决 sigma 生成成本, helper
  解决跨曲线 Stage 1 并发. 二者可同时启用.
- Helper 是 opt-in 工具, 不修改 `ECM::factor` / `ECM::quick_factor` /
  `ECM::factor_with_batch` public path. 调用方在自身循环里 wire-in 即可.
- Default OFF (N=1) 保证 zero behavior change for legacy callers, 仅当用户
  明确 opt-in 时启用.

**集成点** (2026-05-22, W9 T1):
- `include/gnfs/cofactor/ecm_stage1_parallel.hpp` — `ecm_stage1_parallel_threads()`
  env reader with `std::once_flag` cache + `parallel_stage1_curves<>` template
  dispatcher + `ecm_stage1_parallel_reset_env_cache_for_testing()` test hook
- `tests/test_ecm_stage1_parallel.cpp` — 9 个测试 (3 ENV 解析 / N=1 baseline
  mock worker / N=1 vs N=4 mock parity / empty span / single-curve N=4
  no-stall / N=hw_concurrency mock parity / 真实 ECM Stage 1
  via `factor_with_batch` N=1 vs N=4 per-curve `optional<Integer>` bit-identical)

### ECM Montgomery batch inversion (GNFS_ECM_BATCH_INV)

**ENV `GNFS_ECM_BATCH_INV={0,1}`** (2026-05-22 实施, W8 T3, default 0):
ECM point arithmetic 在 Stage 1 / Stage 2 hot loop 频繁对一批 mod-N 整数
逐个求逆 (`mpz_invert`). 当 N 较大 (50d+/60d cofactor 100-300 bits) 时
extended Euclidean inverse 显著贵于 `mpz_mul + mpz_mod`. Montgomery batch
inversion trick 把 k 个逆操作 amortise 成 1 个 inverse + 3k 个 modular mul:

```text
forward: p_0 = v_0;   p_i = p_{i-1} * v_i mod n      (k-1 mults)
central: q   = p_{k-1}^{-1} mod n                    (1 invert)
reverse: inv_i = q * p_{i-1} mod n, q = q * v_i mod n (2*(k-1) mults)
```

```bash
GNFS_ECM_BATCH_INV=1 ./gnfs <N>          # 启用 helper gate
GNFS_ECM_BATCH_INV=0 ./gnfs <N>          # 显式 disable (= default)
unset GNFS_ECM_BATCH_INV                 # 默认 disable
```

**Helper API** (`include/gnfs/cofactor/batch_inversion.hpp`):
- `batch_mod_inverse(values, n)` — Montgomery 路径, 1 inverse + 3k mul,
  返回 `BatchInvResult { inverses, found_factor }`. k=0 立即 return 空;
  k=1 走 single `mpz_invert` 短路 (零 prefix overhead).
- `naive_mod_inverse(values, n)` — k 个 per-element `mpz_invert` 参考实现.
  单元测试 golden, 也供希望显式禁 batched trick 的 caller 使用.
- `ecm_batch_inv_enabled()` — cached `std::once_flag` + `std::atomic<bool>`,
  strict "1" parsing (= W6 `GNFS_FILTER_RADIX_SORT` / W6 `GNFS_V0_BFS`
  pattern). 任何非 "1" 值 (unset / "" / "0" / "garbage" / "2" / "true" /
  "10" / 含空格的 "1") 都返回 `false`.
- `ecm_batch_inv_reset_env_cache_for_testing()` — 测试专用, 重置 cached
  gate 让下次 `enabled()` 再读 env.

**Failure semantics** (与现有 ECM lucky-factor idiom 对齐):
- `mpz_invert(_, p_{k-1}, n) == 0` 时知道 gcd(p_{k-1}, n) > 1, 说明至少一个
  v_i 与 n 有非平凡公因子. helper 顺序扫 input span 找到第一个非平凡
  gcd(v_i, n), 放到 `BatchInvResult::found_factor` (与 ECM 现有 per-curve
  "inverse failure exposes factor" 语义一致).
- 若 v_i 全是 1 或 n 的倍数 (gcd 仅为 1 或 n), `found_factor` 仍是
  `std::nullopt`. caller 必须按 "无法分解" 处理, 不能假设始终能 extract factor.
- `naive_mod_inverse` 用同一 `find_first_nontrivial_gcd` 扫法保证 batch 与
  naive 对同一 input 报告同一 culprit (per-index identical failure mode).

**Bit-for-bit guarantee**: 当 gcd(v_i, n) == 1 for all i, Montgomery trick 与
逐 `mpz_invert` mathematically equivalent (不是 approximation). 单元测试
`tests/test_batch_inversion.cpp` 强制覆盖 k = 0, 1, 5 (n=101), 20 (n ~ 2^64
prime), 100 (n ~ 200-bit prime) 各 size 严格 per-index bit-for-bit assert,
另外测 unreduced v_i (>= n) 与 boundary v_i (= 1, = n-1).

**ROI 与定位**:
- 主要 ROI: 50d+/60d cofactor (200-330 bit N) ECM Stage 1+2 hot loop 当前
  per-point 调 `mpz_invert`. 对 200-bit N, mpz_invert ≈ 10-20 倍 mpz_mul
  cost; batch path k=8 时 amortised inverse cost ≈ 4 mul cost (4-5× 提速).
  k 越大 ROI 越显著, 但需要 caller 能 batch up k >= 2 个独立 inversion site.
- 当前主 pipeline 无 wire-in: ECM Stage 1 / Stage 2 / Brent-Suyama 都仍走
  per-point `mpz_invert`. helper 作为 future-infrastructure 落地, 等具体
  inversion hot site (e.g. Stage 2 BSGS giant-step accumulation, Brent-Suyama
  polynomial 系数 batched eval) 显式 wire-in 时启用.
- helper 与 W8 T1 `GNFS_ECM_STAGE2_PARALLEL` 完全 orthogonal — Stage 2 并行
  跑 K 条独立曲线, batch_inversion 是 per-curve inner loop 的 inversion
  amortisation. 二者可同时启用.

**集成点** (2026-05-22, W8 T3):
- `include/gnfs/cofactor/batch_inversion.hpp` — helper API + ENV gate +
  `BatchInvResult` + `find_first_nontrivial_gcd` 内部 helper.
- `tests/test_batch_inversion.cpp` — 12 tests (ENV unset / "1" / 8 non-"1"
  rejects / reset cache / empty k=0 / single k=1 / parity k=5,20,100 /
  found_factor / boundary v_i / unreduced v_i).
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout
  (实测 wall ~125ms).

**Default OFF**: ENV unset → `ecm_batch_inv_enabled() == false` → 任何 caller
看到 gate 关闭则跑 per-element `mpz_invert` path 不变, 零行为变化. 仅当 caller
主动 wire-in 且用户 explicit `GNFS_ECM_BATCH_INV=1` 时启用.

### Polynomial Half-GCD (GNFS_POLY_HGCD)

**ENV `GNFS_POLY_HGCD=1`** (2026-05-21 实施, default OFF):
启用 Knuth-Schönhage Half-GCD (HGCD) 算法替代 Euclidean GCD 在 polynomial
GCD over F_p[x]. 默认 OFF, Euclidean path 完整保留.

```bash
GNFS_POLY_HGCD=1 ./gnfs <N>   # 启用 HGCD path
unset GNFS_POLY_HGCD          # default OFF (Euclidean)
```

**算法**: Recursive divide-and-conquer on polynomial pair (a, b) — 把
deg(a)=n 切半, 递归求 transformation matrix M 使 M * (a, b) = (a', b')
满足 deg(b') < n/2. 主 GCD 通过反复调用 HGCD + Euclidean tail 完成.

**Threshold `kHGCDThreshold = 16`**: deg(a) 小于此值直接走 Euclidean (递归 +
matrix-vector mult overhead 在小度数 dominate).

**Bit-for-bit guarantee**: `gcd_via_hgcd(a, b, p)` 输出与 `ModularPoly::gcd(a, b, p)`
monic-normalized 结果完全一致. 单元测试 `test_half_gcd` 16 个测试强制验证
(包括 deg [10, 200] 随机 polynomial / large prime ~2^64 / edge cases).

**ROI 定位**:
- HGCD 真正加速依赖 sub-quadratic polynomial multiplication M(n)
  (e.g., FFT 给 O(n log n)). 当前 `ModularPoly::mul_raw` 走 schoolbook
  O(n²), 所以 HGCD wall-time 在 deg ≤ 500 略慢于 Euclidean
  (实测 deg=100 0.37x, deg=500 0.46x).
- GNFS 主路径 polynomial GCD 调用都在小 degree (CZ 求根 ≤ 6),
  ROI 不适用. HGCD 主要为未来 FFT 乘法集成预留接口.
- 不影响正确性, 实验 path 完整测试.

**集成点** (2026-05-21):
- `include/gnfs/polynomial/half_gcd.hpp` — `gcd_via_hgcd()` + `poly_hgcd_enabled()`
  + `kHGCDThreshold` + `HGCDMatrix` 2x2 transformation matrix
- `tests/test_half_gcd.cpp` — 16 个测试 (8 correctness across deg / 4 edge cases
  / 2 ENV / 2 perf info)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout

**Default OFF**: pipeline.cpp 与 `ModularPoly::gcd` 入口不动, opt-in 实验通道.

### Polynomial Karatsuba multiplication threshold (GNFS_POLY_KARATSUBA_THRESHOLD)

**ENV `GNFS_POLY_KARATSUBA_THRESHOLD=N`** (2026-05-22 实施, range [4, 4096], default 32):
Polynomial multiplication helper `karatsuba_mul_mod` 在 F_p[x] 上实现 3-split
Karatsuba O(n^1.585), 基础情形 `max(deg a, deg b) < N` 回退 schoolbook. 与
`schoolbook_mul_mod` 参考实现 bit-for-bit 一致. 默认 threshold N=32 是
schoolbook-vs-Karatsuba 经验 sweet spot.

```bash
unset GNFS_POLY_KARATSUBA_THRESHOLD            # 默认 32
GNFS_POLY_KARATSUBA_THRESHOLD=4    ./gnfs <N>  # 极小 threshold (recursion 走到最深)
GNFS_POLY_KARATSUBA_THRESHOLD=64   ./gnfs <N>  # 较大 threshold (schoolbook 占主导)
GNFS_POLY_KARATSUBA_THRESHOLD=4096 ./gnfs <N>  # 上限 (实测几乎 schoolbook every call)
```

**ENV 解析规则** (严格):
- unset / "" / "0" / 负数 / 非数字 (`garbage` / `1.5` / `12abc` / bare `+` `-`)
  / 含 leading 空白 (` 32`) → default 32
- "10000" → clamp 到上限 4096
- "2" → clamp 到下限 4 (低于 4 时 recursion 无意义, 退化为 split 2+1)

**算法** (3-split Karatsuba):
- 拆分: a = a_low + x^m · a_high, b = b_low + x^m · b_high, m = ceil(nmax / 2)
- z0 = a_low * b_low                                   (递归)
- z2 = a_high * b_high                                 (递归, 一侧空则跳过)
- z1 = (a_low + a_high) * (b_low + b_high) - z0 - z2   (递归 + 减法)
- 结果: out = z0 + x^m · z1 + x^{2m} · z2
- 中间和 (a_low + a_high) mod p 防溢出; subtraction 用 `(a + p - b) mod p`
  避免下溢

**Threshold default 32 选择理由**:
- Karatsuba 每层有显著 per-call overhead (3 个 sum vector + 3 个 sub-product
  vector + 3 个递归 stack frame)
- Schoolbook 内循环紧凑, tiny n 下 mul 数虽然 O(n²) 但常数极小
- 经验 sweet spot 在 16-64 之间, 选 32 作 conservative middle
- 低于 4 时 recursion 退化 (3 系数 polynomial 切 2+1, "高" side 只剩
  degree 1, 无法 amortise)

**Bit-for-bit guarantee**: 同 `(a, b, p)` 输入下 (p 素数, p < 2^32,
coefficients < p), `karatsuba_mul_mod` 与 `schoolbook_mul_mod` 输出
`out` vector 完全一致 (size + 每位 content). Threshold 值仅影响递归深度,
不影响数学结果. Empty 输入双方都给 empty 输出. 单元测试
`tests/test_poly_karatsuba.cpp` 通过 13 random shapes 与 threshold
extremes (4 vs 999999) 严格强制覆盖.

**修复历史** (commit `25169c4`):
初版在 a/b 跨 split 边界时 z1 = sum_a * sum_b - z0 - z2 留有 trailing zero
(Karatsuba 算法故意取消 leading coefficient), compose 时 grow `out` 超出
`na + nb - 1` 上限. 修复: z0 / z1 / z2 / sum_a / sum_b 每次计算后
`trim_trailing_zeros`, 且 `add_shifted_in_place` 不再 grow out (out-of-range
src 必须为 0, 否则 assert).

**Modulus precondition**: p < 2^32 (保证 uint64 * uint64 不溢出).
caller 需要 p >= 2^32 时仍走 `ModularPoly::mul_raw` (内部 `__uint128_t`).

**ROI 与定位**:
- 主要 ROI: Karatsuba 是 sub-quadratic primitive M(n), 是 W7 HGCD
  (`GNFS_POLY_HGCD`) 等待的 sub-quadratic 乘法. HGCD 真正 wall-time
  加速依赖 M(n) 复杂度低于 schoolbook O(n²). 当前 `ModularPoly::mul_raw`
  仍走 schoolbook, 所以 HGCD 在 deg ≤ 500 略慢 (W7 实测 deg=100 0.37x,
  deg=500 0.46x).
- 当前主路径 `ModularPoly::mul_raw` **未** wire-in Karatsuba — 是
  future-infrastructure helper. 当未来 caller (例如 `ModularPoly::mul_raw`
  内部, 或 HGCD recursion 内部) 决定切到 sub-quadratic primitive 时直接
  调用 `karatsuba_mul_mod` 即可.
- perf-info probe (size=500, p=2^31-1): schoolbook 3.73 ms/call vs
  karatsuba 1.72 ms/call → 2.17x 加速. 真正 ROI 在 deg >> 100 时显著.

**集成点** (2026-05-22, W9 T2):
- `include/gnfs/polynomial/karatsuba_mul.hpp` — `karatsuba_mul_mod()` +
  `schoolbook_mul_mod()` + `poly_karatsuba_threshold()` (cached env, strict
  parsing) + `poly_karatsuba_threshold_reset_env_cache_for_testing()` test hook
- `tests/test_poly_karatsuba.cpp` — 10 个测试 (5 env parsing / 2 edge cases /
  2 correctness parity / 1 perf info)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  polynomial 模块

**Default 32 主路径无影响**: `ModularPoly::mul_raw` 入口未改, helper
仅在显式 caller wire-in 时启用. 现有 schoolbook path 与 W7 HGCD path
均保持原行为. ENV 仅对显式调用 `karatsuba_mul_mod` 的 caller 生效.

### Polynomial subquadratic divrem (GNFS_POLY_DIVREM_SUBQUADRATIC)

**ENV `GNFS_POLY_DIVREM_SUBQUADRATIC=auto|0|1`** (2026-05-22 实施, W11 T2, default auto):
Polynomial Euclidean division helper `divrem_modp` 在 F_p[x] 上实现
Newton-reciprocal subquadratic divrem, 通过 reversed-denominator 的
power-series inverse 把 divrem 归约成两次 polynomial multiplication.
基础情形 (`num.size() < kDivremSubquadraticThreshold = 32` 或 gate 非
ForceOn) 回退 schoolbook. 与 `divrem_modp_schoolbook` 参考实现
(matching `ModularPoly::divmod` 语义) bit-for-bit 一致.

```bash
unset GNFS_POLY_DIVREM_SUBQUADRATIC            # 默认 Auto (= schoolbook, 零行为变化)
GNFS_POLY_DIVREM_SUBQUADRATIC=auto ./gnfs <N>  # 同 unset
GNFS_POLY_DIVREM_SUBQUADRATIC=0    ./gnfs <N>  # 显式 ForceOff (schoolbook)
GNFS_POLY_DIVREM_SUBQUADRATIC=off  ./gnfs <N>  # 同 "0"
GNFS_POLY_DIVREM_SUBQUADRATIC=1    ./gnfs <N>  # ForceOn (Newton-reciprocal above threshold)
GNFS_POLY_DIVREM_SUBQUADRATIC=on   ./gnfs <N>  # 同 "1"
```

**ENV 解析规则** (三态严格):
- unset / "" / "auto" → Auto (default, 当前等价于 ForceOff, 保守路由)
- "0" / "off" → ForceOff (强制 schoolbook)
- "1" / "on" → ForceOn (启用 Newton-reciprocal above threshold)
- 任何其他值 (`garbage`, `2`, `true`, `-1`, `yes`, 大小写 `ON/OFF/Auto`,
  含 leading 空白 ` 1`) → Auto

**算法** (Newton-reciprocal divrem):
- 给定 `num, den ∈ F_p[x]`, 计算 `(quot, rem)` 满足
  `num = quot · den + rem`, `deg(rem) < deg(den)`
- 系数反转: `num_rev = reverse(num)`, `den_rev = reverse(den)`
- Newton iteration 求 `den_rev^{-1} mod x^{q+1}` (q = deg(num) - deg(den)):
  从 `r_0 = den_rev[0]^{-1} mod p` (precision 1) 出发, 每轮 `r_{k+1} =
  r_k · (2 - den_rev · r_k) mod x^{2k}` 倍增 precision, O(log q) 轮收敛
- 一次乘法恢复 quotient: `quot_rev = num_rev · den_rev^{-1} mod x^{q+1}`
- `quot = reverse(quot_rev)`
- 一次乘法 + 减法恢复 remainder: `rem = num - quot · den`
- 内部 multiplication 都用 self-contained schoolbook (不依赖 W9 Karatsuba),
  保持 helper 独立; 未来 caller wire-in 可以分别 dispatch 到 Karatsuba 或
  其他 sub-quadratic primitive

**Threshold default 32 选择理由**:
- Newton-reciprocal 每轮有 per-call overhead (truncated 中间 series 分配,
  反转 / 截断系数拷贝), 加上常数性 O(log q) iteration 数
- Schoolbook 内循环紧凑, 小 deg(num) 时 quadratic walk 常数比 Newton 小
- 经验 crossover 在 32-64 之间, 选 32 与 W9 Karatsuba threshold default
  保持一致 (用户语义统一)
- ForceOn 但 `num.size() < 32` 时仍 route schoolbook (`divrem_modp` 内
  dispatch 检查), 单元测试 `test_threshold_below_routes_to_schoolbook`
  强制覆盖

**Modulus precondition**: p prime, p < 2^32 (保证 uint64 * uint64 fits
into uint64 in the schoolbook inner products and Newton iteration).
Caller 需保证 `num`, `den` 系数已 reduced mod p; `den` 非零多项式
(zero denominator 抛 `std::runtime_error`).

**Bit-for-bit guarantee**: 同 `(num, den, p)` 输入下 (p 素数, p < 2^32,
coefficients < p, den != 0), `divrem_modp` 与 `divrem_modp_schoolbook`
输出 `(quot, rem)` vector 完全一致 (size + 每位 content, 都是 trim 过
trailing zeros 的 canonical form). Gate 值仅影响 dispatch kernel,
不影响数学结果. 单元测试 `tests/test_divrem_subquadratic.cpp` 通过
17 个 case 严格覆盖 (4 ENV / 5 schoolbook unit / 7 subquadratic parity
deg 50/200/500 + 10-shape random sweep + exact-multiple + den constant +
num zero / 1 perf info).

**ROI 与定位**:
- 主要 ROI: divrem 是 W7 HGCD recursion 内部 sub-routine. 当前 HGCD
  recursion 调 `ModularPoly::divmod` (schoolbook), 整体 wall-time 在
  deg ≤ 500 略慢 (W7 实测 0.37x - 0.46x). Newton-reciprocal divrem 提供
  sub-quadratic primitive, 让 HGCD 真正 exhibit O(M(n) log n) 行为, 前提
  是 M(n) 也是 sub-quadratic (即 W9 Karatsuba 已 wire-in)
- helper 当前 standalone (主路径 `ModularPoly::divmod` 与 HGCD 未 wire-in),
  是 future-infrastructure
- perf-info probe (deg=500, p=2^31-1, 内部 schoolbook M(n)): schoolbook
  0.44 ms/call vs subquadratic 3.34 ms/call → 0.13x (subquadratic 比
  schoolbook 慢, 因为 internal mul 仍走 schoolbook, Newton 多了 O(log q)
  rounds 的常数开销). 真正 ROI 需要 wire-in Karatsuba 后 deg >> 500 才显著

**集成点** (2026-05-22, W11 T2):
- `include/gnfs/polynomial/divrem_subquadratic.hpp` — `divrem_modp()` +
  `divrem_modp_schoolbook()` + `divrem_subquadratic_mode()` (cached env
  三态 parsing) + `divrem_subquadratic_enabled()` 等价 predicate +
  `divrem_subquadratic_reset_env_cache_for_testing()` 测试 hook +
  `kDivremSubquadraticThreshold = 32`
- `tests/test_divrem_subquadratic.cpp` — 17 instant tier tests, TIMEOUT 60
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  polynomial 模块

**Default Auto 主路径无影响**: `ModularPoly::divmod` 入口未改, helper
仅在显式 caller wire-in 时启用. 现有 schoolbook path / W7 HGCD path /
W9 Karatsuba helper 路径均保持原行为. Auto 与 ForceOff 当前等价 (保守
路由 schoolbook), 仅 ForceOn 才启用 Newton-reciprocal. ENV 仅对显式
调用 `divrem_modp` 的 caller 生效.

### Polynomial Horner batch evaluation SIMD (GNFS_POLY_HORNER_BATCH_SIMD)

**ENV `GNFS_POLY_HORNER_BATCH_SIMD=auto|0|1`** (2026-05-22 实施, W10 T2, default auto):
多点 Horner 求值 batched helper, 把 dense polynomial `p(x) = c[0] + c[1]*x +
... + c[d]*x^d` 在批量 `xs[0..n-1]` 上的 Horner 求值切到 NEON 2-lane
(ARM64) / AVX2 4-lane (x86_64) wide load + scalar GPR inner mul-add 路径.
应用场景: Murphy E rotation sweeps, polynomial verification during
Cantor-Zassenhaus root finding, Kleinjung skewness search — 任何对小 dense
polynomial 多点求值的 hot path. Pure header, 不依赖外部库.

```bash
GNFS_POLY_HORNER_BATCH_SIMD=auto ./gnfs <N>   # 默认: NEON/AVX2 可用则启用
GNFS_POLY_HORNER_BATCH_SIMD=0    ./gnfs <N>   # 强制 scalar (回归 bisect 用)
GNFS_POLY_HORNER_BATCH_SIMD=1    ./gnfs <N>   # 强制 SIMD (无 SIMD 平台 fallback)
unset GNFS_POLY_HORNER_BATCH_SIMD             # 同 auto
```

**Helper API** (`include/gnfs/polynomial/horner_batch_simd.hpp`):
- `batch_eval_poly_int64(coeffs, xs, ys)` — 主入口, `ys[i] = c[0] + c[1]*xs[i]
  + ... + c[d]*xs[i]^d`. SIMD path 当 `horner_batch_simd_enabled()` 为 true
  时启用. `ys.size() >= xs.size()` 必须成立 (defensive clamp).
- `batch_eval_poly_int64_scalar(coeffs, xs, ys)` — scalar reference (test
  golden + 无 SIMD fallback).
- `horner_eval_one_scalar(coeffs, x)` — per-point Horner, return `int64_t`.
  SIMD path 的 tail residual 直接调用.
- `horner_batch_simd_mode()` — 返回 `HornerBatchSimdMode { Auto, ForceOff, ForceOn }`.
- `horner_batch_simd_enabled()` — 三态 dispatcher decision (ForceOff → false,
  ForceOn/Auto + supported → true, 否则 false).
- `horner_batch_simd_supported()` — compile-time `__ARM_NEON / __AVX2__` 探测.
- `horner_batch_simd_reset_env_cache_for_testing()` — 测试专用 re-resolve ENV.

**算法 (Horner schema)**:
- 每个 evaluation point: `acc = c[d]; for k in [d-1..0]: acc = acc * x + c[k]`
- NEON / AVX2 path: SIMD load 把 2 (NEON) / 4 (AVX2) 个 `xs[i]` 一次性载入,
  inner Horner 在 scalar GPR 上跑 (Apple Silicon NEON 缺 `vmulq_s64`, AVX2
  缺 `_mm256_mullo_epi64` 除非 AVX-512 DQ), SIMD store 把结果写回. SIMD 价值
  在 consolidated address-gen, 不在 vector mul.
- Tail scalar fallback: 处理 `xs.size()` 非 SIMD 宽度倍数的尾部.

**Bit-for-bit guarantee**: 同 `(coeffs, xs)` 输入下 (无 int64 溢出),
SIMD path 与 scalar path 产出 `ys` 严格 per-index 一致. 单元测试
`tests/test_horner_batch_simd.cpp` 16 个测试强制覆盖 (4 ENV 解析 + empty
xs / empty coeffs + deg=0 / 1 / 5 random 100 / 10 random 1000 + ForceOff
vs Auto parity + single-x tail + unaligned len sweep 1..33 + negative
coeff / negative x + horner_eval_one_scalar sanity + 1M-eval perf info).

**Modular overflow note**: helper 不做 overflow check. caller 负责保证
`|acc|` 在 Horner 累乘期间不溢 int64. 典型 Murphy E sample grid 满足
`|x[i]| <= skew` + `|c[k]| << 2^63 / skew^deg`, 无溢出风险. 任意精度需求
应改用 `Integer`-based polynomial API.

**ROI 与定位**:
- 主要 ROI: 1M-eval perf-info 实测 M5 ARM64 deg=8: scalar 12.82ms,
  dispatch (Auto) 10.73ms → 1.20x speedup. SIMD path 节省 per-iter
  address-gen pressure, 内核 mul-add 仍走 GPR (Apple Silicon 整数管线
  4-way superscalar, 两条 lane 并发 mul-add 自然 pipeline).
- helper 当前 standalone (主 pipeline `MurphyEvaluator` / `KleinjungSelector`
  / CZ root verify 未 wire-in), 是 future-infrastructure. wire-in 时
  caller 切到 `batch_eval_poly_int64` + 提供连续 `xs` / `ys` span.
- 初版 NEON path keep accumulator 在 `int64x2_t`, `vsetq_lane_s64`
  per-iter round-trip 导致 0.13× 慢于 scalar; 修复为 inner loop 全 GPR,
  仅 boundary load/store SIMD, 恢复 1.20× 加速.

**集成点** (W10 T2, 2026-05-22):
- `include/gnfs/polynomial/horner_batch_simd.hpp` — helper API + ENV gate +
  NEON / AVX2 inner kernels + scalar reference.
- `tests/test_horner_batch_simd.cpp` — 16 instant tier tests, TIMEOUT 60.
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  polynomial 模块.

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点,
ENV 对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

### Phase 0 radix-sort dedup-sort (GNFS_FILTER_RADIX_SORT)

**ENV `GNFS_FILTER_RADIX_SORT={0,1}`** (2026-05-22 实施, default 0):
`sort_relations` (Phase 0 dedup-sort 之前的 `(b, a)` lex 排序) 切到
LSD 字节-radix 路径. 默认 0 走 `std::sort` 比较器路径, bit-for-bit
identical 输出. 严格仅 "1" 启用, 其它值 (unset / "garbage" / "2" /
"true" / "0" / 空串) 均视为 0.

```bash
GNFS_FILTER_RADIX_SORT=1 ./gnfs <N>          # 启用 LSD radix path
GNFS_FILTER_RADIX_SORT=0 ./gnfs <N>          # 显式 disable (= default)
unset GNFS_FILTER_RADIX_SORT                  # 默认 disable (std::sort)
```

**算法**: 稳定 LSD 字节-radix, 16 次 byte-pass (a 低位到高位 8 次,
b 低位到高位 8 次). LSD 稳定性使 b 成为主键, a 次键, 与 `std::sort`
比较器同序. signed `a` (int64) 经 `kSignBias = 0x8000_0000_0000_0000`
XOR 后按 unsigned 字节排序仍得正确数值序 (negative 排在 non-negative
之前). 单线程, O(n) wall, O(n) scratch (两份 uint64 keys + 两份
uint32 indices + 最终一次 Relation move 重排).

**Bit-for-bit guarantee**: 同 relation 列表, radix path 与 std::sort
path sort 后 `filter_duplicates` 结果完全相同 (顺序相同, 字段相同).
稳定排序使重复 `(a, b)` 对保留首次插入顺序, 故 dedup 选同一代表关系.
单元测试 `tests/test_filter_radix_sort.cpp` 8 个测试强制覆盖 (empty /
single / sorted / reverse / random 100/10000 / 5×duplicate stability /
ENV parsing matrix).

**ROI 与定位**:
- 主要 ROI: 1M+ relations 时 `std::sort` 比较器每次 probe 触及 ≥ 500B
  的 `Relation` struct, cache miss heavy. radix 仅扫一次 (建 keys) 再
  扫一次 (final permute), 中间 16 pass 全部跑在 8B keys + 4B indices,
  L1/L2 友好. 理论 wall O(n) vs `n log n`.
- 50d+/60d Round 2 Phase 0 sort 可见时间被压缩 (具体 wall 视 n / Relation
  size, smaller n 走 std::sort 更快).
- ROI 主要在 50d+ stress 路径, 25d gate sort 时间 < 1ms 无差别. 因此
  默认 OFF 保证零回归风险.

**集成点** (2026-05-22):
- `include/gnfs/relation/radix_sort.hpp` — 新增 helper header. 提供
  `radix_sort_relations()` + `filter_radix_sort_enabled()` (cached) +
  `filter_radix_sort_reset_env_cache_for_testing()` test hook +
  `detail::kSignBias` / `detail::radix_byte_pass` 实现细节
- `include/gnfs/relation/collector.hpp` — `sort_relations()` 入口处
  ENV-gate 分发. 检索 `filter_radix_sort_enabled()` 调用点
- `tests/test_filter_radix_sort.cpp` — 8 instant tier tests, TIMEOUT 60
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout

**Default OFF**: `sort_relations()` 调用方零行为变化, `std::sort` path
完整保留. 仅当用户 explicit `GNFS_FILTER_RADIX_SORT=1` 时启用.

### Filter Phase 0 LP key Bloom pre-screen (GNFS_FILTER_LP_BLOOM_BITS)

**ENV `GNFS_FILTER_LP_BLOOM_BITS=N`** (2026-05-22 实施, W9 T5, range [10, 28], default 0):
LP key (large prime key) 去重计数的可选 Bloom filter pre-screen helper.
`filter.hpp::count_unique_lp_keys(relations)` 是 50d+/60d Round 2 adaptive
sieve loop 的 hot path, 每次 Phase 4 entry 都扫全部 relations 把 LP key
插入 `std::unordered_set<uint64_t>` 然后取 `.size()` 作为 effective_cols
trim limit. 1M+ relations 时 hash-set bucket probe cache miss 显著.

```bash
GNFS_FILTER_LP_BLOOM_BITS=0  ./gnfs <N>   # default, pure hash-set baseline (零开销)
GNFS_FILTER_LP_BLOOM_BITS=14 ./gnfs <N>   # 16 KiB filter (L1-friendly)
GNFS_FILTER_LP_BLOOM_BITS=18 ./gnfs <N>   # 256 KiB filter (L2-friendly)
GNFS_FILTER_LP_BLOOM_BITS=22 ./gnfs <N>   # 4 MiB filter (L3-friendly, 50d+)
GNFS_FILTER_LP_BLOOM_BITS=24 ./gnfs <N>   # 16 MiB filter (60d 大数量 LP)
GNFS_FILTER_LP_BLOOM_BITS=28 ./gnfs <N>   # 256 MiB filter (上限)
unset GNFS_FILTER_LP_BLOOM_BITS           # 同 default 0
```

**算法** (k=4, m=2^bits):
- 4 个 hash function 派自 splitmix64 variant (4 个不同 salt seed)
- insert: 4 个 hash 位置全部 set
- maybe_contains: 4 个 hash 位置全部 set 则返回 true
- false negative rate = 0 (correctness invariant)
- false positive rate ≈ (1 − exp(−4n / 2^bits))^4
- 实测 bits=20 + 10k inserts: FP=0/10000 (theoretical 1.96e-6)

**Helper API** (`include/gnfs/relation/lp_bloom.hpp`):
- `BloomLPKeyFilter(bits)` — ctor, bits ∈ [10, 30], 否则 throw `invalid_argument`
- `insert(key)` / `maybe_contains(key)` / `size_bytes()` / `bits()` /
  `estimated_fp_rate(n)`
- `filter_lp_bloom_bits()` — cached `std::once_flag` + `std::atomic<int>`
- `filter_lp_bloom_enabled()` — `bits() > 0` predicate
- `filter_lp_bloom_reset_env_cache_for_testing()` — 测试 re-resolve hook
- `count_unique_with_bloom<KeyIt>(first, last, bits)` — 计数模板, `bits == 0`
  走 pure hash-set baseline, `bits > 0` 走 Bloom pre-screen + hash-set 确认

**Bit-for-bit guarantee**: `count_unique_with_bloom` 输出与 `bloom_bits == 0`
baseline 完全相同, 不管 Bloom 是否启用. Bloom false positive 仍走 hash-set
exact match 确认; Bloom "definitely not seen" 直接 hash-set insert (跳过
probe 但产物一致). 单元测试 `test_lp_bloom` 强制 100k random keys 跨
bits ∈ {0, 10, 14, 18, 22} 严格相等.

**ENV parsing**:
- unset / "0" / 负数 / 非数字 / 空字符串 → 0 (disabled)
- "1".."9" → 0 (clamp 至 disabled, 低于 1 KiB floor)
- "10".."28" → as-is
- "29"+ → 28 (clamp)
- 数字前缀 ("16abc"): 取首数字段; 非数字前缀 ("abc16"): 视为 0

**ROI 与定位**:
- 主要 ROI: 50d+/60d 大 relation 数 (1M+) 时 `count_unique_lp_keys` 内
  hash-set bucket cache miss 显著. Bloom (m=2^22 = 4 MiB) 完全在 L3,
  大部分 query 直接 4-hash mask + bit-test 在 L1/L2 完成, 跳过 hash-set
  probe.
- 25d/40-bit small N (< 50k relations) 无 ROI, Bloom 构造与 4-hash overhead
  反而增加常数项. 默认 OFF 保证零回归.
- helper 当前 standalone (`count_unique_lp_keys` 主路径未 wire-in), 是
  future-infrastructure. wire-in 时调用方需切到 `count_unique_with_bloom`
  并传入 `filter_lp_bloom_bits()`.

**集成点** (W9 T5, 2026-05-22):
- `include/gnfs/relation/lp_bloom.hpp` — helper API + ENV gate + k=4 Bloom
  primitive + `count_unique_with_bloom<KeyIt>` template
- `tests/test_lp_bloom.cpp` — 11 个测试 (4 ENV / 3 Bloom 行为 / 1 parity
  100k keys / 3 edge cases). 全部 instant tier
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout
- 主路径 `include/gnfs/relation/filter.hpp::count_unique_lp_keys` **未改动**
  (helper-only landing, future wire-in)

**Default OFF (bits=0)**: ENV unset → `filter_lp_bloom_bits() == 0` →
`count_unique_with_bloom` 退化为 pure `std::unordered_set<uint64_t>` baseline,
零行为变化. 仅当 caller wire-in helper 且用户 explicit
`GNFS_FILTER_LP_BLOOM_BITS=N>=10` 时启用.

### LP key splitmix64 hash mixing (GNFS_FILTER_LP_HASH_MIX)

**ENV `GNFS_FILTER_LP_HASH_MIX=auto|0|1`** (2026-05-22 实施, W11 T5, default auto):
LP key (large prime key) `std::unordered_set<uint64_t>` / `std::unordered_map`
的 hash 混合 helper. libstdc++ / libc++ 默认 `std::hash<uint64_t>` 几乎是
identity, 而 LP key 典型 packing `(prime_id << 1) | side` 在低位严重 cluster
(小素数 ID 在低位密集, side bit 固定). 这导致 unordered_set 的 bucket 集中,
chain 长, probe 数升高. helper 提供 splitmix64 (Stafford Mix 13) bit mixer
打散 input bit pattern. `count_unique_lp_keys` (W9 T5 Bloom 兄弟 helper) 与
`filter.hpp` / `clique_merger.hpp` 主路径 `std::unordered_set` 调用方 **未改动**,
是 opt-in future wire-in.

```bash
GNFS_FILTER_LP_HASH_MIX=auto ./gnfs <N>   # 默认: mixing 启用
GNFS_FILTER_LP_HASH_MIX=0    ./gnfs <N>   # 显式 disable mixing (回归 bisect 用)
GNFS_FILTER_LP_HASH_MIX=off  ./gnfs <N>   # 同 0
GNFS_FILTER_LP_HASH_MIX=1    ./gnfs <N>   # 显式 enable (与 auto 行为一致)
GNFS_FILTER_LP_HASH_MIX=on   ./gnfs <N>   # 同 1
unset GNFS_FILTER_LP_HASH_MIX             # 同 auto
```

**Helper API** (`include/gnfs/relation/lp_key_hash.hpp`):
- `enum class LpHashMixMode { Auto, ForceOff, ForceOn }` — gate 三态.
- `lp_hash_mix_mode()` — cached `std::call_once` + `std::atomic<int>` ENV reader.
- `lp_hash_mix_enabled()` — `mode != ForceOff` 等价 predicate.
- `lp_hash_mix_reset_env_cache_for_testing()` — 测试 re-resolve hook.
- `constexpr uint64_t mix_lp_key(uint64_t)` — splitmix64 round, 永远启用
  (不查 gate). 可在 compile time 求值.
- `inline uint64_t maybe_mix_lp_key(uint64_t)` — gate-aware wrapper:
  `enabled() ? mix_lp_key(k) : k`.
- `struct LpKeyHash { size_t operator()(uint64_t) const noexcept; }` —
  `std::hash`-兼容 functor, 内部调 `maybe_mix_lp_key`. 可作为
  `std::unordered_set<uint64_t, LpKeyHash>` 的第二模板参数.

**算法 (splitmix64 / Stafford Mix 13)**:
```cpp
uint64_t z = key + 0x9E3779B97F4A7C15ULL;  // golden ratio fract
z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
z = z ^ (z >> 31);
return z;
```
- 三个 magic 常量来自 Stafford 的 `Mix13` (基于 SplittableRandom 设计),
  优化 avalanche (每个 output bit 依赖大约一半 input bit).
- golden-ratio (`0x9E37...`) 加法防止 `key == 0` 落到 xorshift 的 zero
  fixed-point.
- 两个 64-bit mul + 3 个 xorshift, 一条 dependency chain, 无 branch,
  noexcept, allocation-free.

**Bit-for-bit guarantee**: `mix_lp_key` 是 deterministic pure function
of input. 同一输入永远产生同一输出. constexpr-evaluable. 单元测试
`tests/test_lp_key_hash.cpp` 强制 known vectors (4 个):
- `mix_lp_key(0x0000000000000000) = 0xE220A8397B1DCDAF`
- `mix_lp_key(0x0000000000000001) = 0x910A2DEC89025CC1`
- `mix_lp_key(0x00000000DEADBEEF) = 0x4ADFB90F68C9EB9B`
- `mix_lp_key(0xFFFFFFFFFFFFFFFF) = 0xE4D971771B652C20`

**ENV parsing 严格 token 匹配**:
- unset / "" / "auto" / 任何未识别 token (含 "2" / "true" / "ON" 大写) → Auto (mixing 启用)
- "0" / "off" → ForceOff (mixing 禁用)
- "1" / "on" → ForceOn (mixing 启用, 与 Auto 行为一致, 仅语义区分用户意图)

**ROI 与定位**:
- 主要 ROI: LP key 集合用 `std::unordered_set<uint64_t, LpKeyHash>` 后,
  clustered LP key 散到全 bucket 范围, 减小 chain 长. 50d+/60d 大 LP key
  集合 (1M+ unique) 上 hash-set lookup wall-time 实测可见. 默认 ON (Auto)
  对未来 wire-in 调用方零额外配置.
- 与 W9 T5 `GNFS_FILTER_LP_BLOOM_BITS` 互补: Bloom 是 pre-screen 减少
  hash-set probe 数, 本 helper 是改善 hash-set 内部 bucket 分布. 可同时启用.
- helper 当前 standalone (`filter.hpp::count_unique_lp_keys` 与
  `clique_merger.hpp` 主路径 `std::unordered_set` 未 wire-in), 是
  future-infrastructure. wire-in 时 caller 把
  `std::unordered_set<uint64_t>` 改为 `std::unordered_set<uint64_t, LpKeyHash>`
  即可生效, 不需修改 insert / find / count 等调用.
- perf-info 实测 10k LP-shaped keys (`(pid << 1) | side` 序列):
  identity hash max_bucket_load=1 (uniform-stride pattern 已被 identity 完美散开),
  LpKeyHash max_bucket_load=6 (mixer 引入 Poisson-style 自然 collision).
  此 informational probe 不 assert — 真实 ROI 在 clustered (非 uniform)
  pattern 上体现.

**集成点** (W11 T5, 2026-05-22):
- `include/gnfs/relation/lp_key_hash.hpp` — helper API + 三态 ENV gate +
  splitmix64 `mix_lp_key` (`constexpr noexcept`) + `LpKeyHash` 函子 +
  `maybe_mix_lp_key` wrapper.
- `tests/test_lp_key_hash.cpp` — 15 个测试 (4 ENV 解析 + splitmix64 known
  vectors + 1 determinism + 1 avalanche + 3 gate semantics +
  4 `LpKeyHash` functor + 1 reset cache hook).
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  relation 模块.
- 主路径 `include/gnfs/relation/filter.hpp` / `include/gnfs/relation/clique_merger.hpp`
  **未改动** (helper-only landing, future wire-in).

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点,
ENV 对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

### SGE batch-pivot 选择 (GNFS_SGE_BATCH_PIVOTS)

**ENV `GNFS_SGE_BATCH_PIVOTS=N`** (2026-05-22 实施, range [1, 64], default 1):
Structured Gaussian Elimination 每 pass 收集 N 个 row-support 互不相交的 pivot
集中应用, 而非 sequential 单 pivot 扫描. N=1 (默认) 保留原 Phase 1 worklist /
Phase 2 列扫描语义, 零开销.

```bash
GNFS_SGE_BATCH_PIVOTS=1  ./gnfs <N>   # default 序列 path (与历史一致)
GNFS_SGE_BATCH_PIVOTS=8  ./gnfs <N>   # 每 pass 收 8 个 disjoint-row pivot
GNFS_SGE_BATCH_PIVOTS=32 ./gnfs <N>   # 50d+/60d 大矩阵激进 batch
GNFS_SGE_BATCH_PIVOTS=64 ./gnfs <N>   # 上限 (kSGEBatchPivotsMax)
unset GNFS_SGE_BATCH_PIVOTS           # 同 default 1
```

**ROI 与定位**:
- 主要 ROI: 50d+ 大矩阵 (~1M cols) 时 SGE Phase 1 worklist 与 Phase 2 列扫描
  wall-time 占 SGE 总时间 60-70%. N>=8 batch 让每 pass 单次扫描多收几个 pivot,
  减少 pass 数 + 提升 cache locality.
- 25d / 81-bit gate 矩阵 (≤几千 col) 上 SGE 全程 < 100 ms, batch ROI 不显著
  但行为完全一致 (canonical-form equivalent).
- 真正 parallelism 是 *logical* (per-pass 选 N 个 disjoint pivot 后顺序 apply,
  apply 顺序无关因为四 row 集合 disjoint). 未来若 worker pool 加入 SGE,
  此结构已就绪.

**等价不变量 (实测强制)**:
- 相同 surviving row / column 数 (matrix shape 严格等)
- 相同 `col_map` 内容 (col_alive 扫描顺序两 path 一致)
- 相同 `row_composition` multiset (XOR 在 GF(2) 上 commute)
- 相同 reduced matrix rows multiset (每 row indices sort 后整体 sort 比较)

**不**严格 bit-identical 的部分: surviving row 在 reduced matrix 内的*位置顺序*
可能不同 (batch path 选 pivot 顺序与 sequential 不同 → "heavier row" tiebreak
偶发不同时刻触发). 测试通过 canonical form (各 row 内 sort + 整体 sort) 抹平此差异.

**集成点** (2026-05-22):
- `include/gnfs/linalg/sge_batch_pivots.hpp` — ENV reader + `std::once_flag` cache
  + `kSGEBatchPivotsMax=64` + `sge_batch_pivots_size()` + `*_reset_env_cache_for_testing()`
- `include/gnfs/linalg/sge.hpp` — `SGEConfig.batch_pivots` 字段 + `SGE::preprocess`
  `effective_batch` 解析 + `apply_w1_pivot` / `apply_w2_pivot` lambda 抽取 +
  Phase 1 / Phase 2 dispatch (N=1 走原路径, N>=2 走 disjoint-row 选 pivot 批量 apply)
- `tests/test_sge_batch_pivots.cpp` — 8 个测试 (N=1 baseline / N=8 / N=32 parity /
  ENV 6 case / empty / single-pivot chain / 5×4 random sweep / config override)
- `CMakeLists.txt` / `scripts/test.sh` — instant tier, 60s timeout, linalg 模块

**Default OFF (N=1)**: 任何 caller 不设 ENV 也不传 `config.batch_pivots > 0` 时
完全跑历史序列 path, 零行为变化. 仅 50d+ 用户 explicit opt-in 时启用.

### Cache-blocked GF(2) matrix transpose (GNFS_MATRIX_TRANSPOSE_BLOCKED)

**ENV `GNFS_MATRIX_TRANSPOSE_BLOCKED=auto|0|1`** (2026-05-22 实施, default auto):
密集 word-packed GF(2) 矩阵转置 helper, 64×64 tile 块化 + "Hacker's Delight"
6 阶段 swap-reduction 在 register 内完成 tile 转置, 朴素 bit-by-bit 路径与
块化路径 bit-for-bit 一致. 默认 auto 在任一维度 >= 128 时启用块化.

```bash
GNFS_MATRIX_TRANSPOSE_BLOCKED=auto ./gnfs <N>   # 默认: dim >= 128 启用 block
GNFS_MATRIX_TRANSPOSE_BLOCKED=0    ./gnfs <N>   # 强制 naive (回归 bisect 用)
GNFS_MATRIX_TRANSPOSE_BLOCKED=1    ./gnfs <N>   # 强制 block (不考虑 dim 阈值)
unset GNFS_MATRIX_TRANSPOSE_BLOCKED             # 同 auto
```

**Helper API** (`include/gnfs/linalg/detail/transpose_blocked.hpp`):
- `transpose_blocked_gf2(src, dst, rows, cols)` — 主入口, 内部根据 gate
  路由到 `transpose_naive_gf2` 或 `transpose_blocked_gf2_impl`.
- `transpose_naive_gf2(src, dst, rows, cols)` — 朴素 bit-by-bit reference
  (O(rows · cols) ops), 作为单元测试 golden + 小矩阵 fallback.
- `matrix_transpose_blocked_enabled(rows, cols)` — 三态 dispatcher.
- `matrix_transpose_blocked_mode()` — 返回 cached `GateMode` (Auto / ForceOff /
  ForceOn), 测试用.
- `reload_matrix_transpose_blocked_for_testing()` — 重置 cached gate, 单元
  测试切换 ENV 用.
- `kTransposeTileBits = 64` — tile 边长 (与 64-bit word 对齐).
- `kTransposeAutoThreshold = 128` — auto 阈值, 任一维度 >= 此值启用 block.

**算法 (Hacker's Delight 6-stage swap reduction)**:
- Tile 加载: 一次读 64 行的 1 个 word (64 列 = 1 word), zero-pad 边缘 tile.
- 在 register 内 6 阶段 swap: 第 s 阶段把宽度 `2^s` 的 row band 与对应
  band 沿对角线 swap, 用 mask `(0101..., 0011..., 00001111..., 8-bit,
  16-bit, 32-bit)` 隔离每阶段 bit.
- 总开销: 64 word load + 192 XOR (6 阶段 × 32 swap) + 64 word store.
- 缓存友好性: tile 读写连续 64 word, 每个 cache line 64 B (8 word), 自然
  对齐. Naive 路径每 bit 1 个 random row stride access, cache miss 严重.

**Bit-for-bit guarantee**: GF(2) 加法 associative + commutative, swap
reduction 是纯置换, blocked 与 naive 路径输出严格相同. 单元测试
`test_transpose_blocked` 13 个测试强制覆盖 (空矩阵 / 1x1 / 7x3 / 63x127 /
64x64 / 128x64 / 1000x1000 / 500x800 random / 双 transpose 回路 /
ENV 解析 / threshold 路由 / dispatcher parity / 稀疏 pattern).

**ROI 与定位**:
- 主要 ROI: 大矩阵 (>= 128 维) 转置 wall-time 由 O(rows · cols) bit ops
  降到 O((rows · cols) / 64²) tile ops + register-level swap. 50d+/60d
  Phase 5 / 未来 dense Gaussian-Jordan 在 PackedGF2Matrix 实施
  时可受益.
- 当前主路径无 materialized dense 转置 hot site: SpMV-transpose
  (`detail::spmv_transpose`) on-the-fly 工作在 CSR sparse layout, 不需要
  bit-packed 转置; SparseMatrix::transpose() 工作在稀疏 row-of-indices
  layout, 不是 word-packed; PackedGF2Matrix (block_lanczos.cpp 内部
  file-local) 也没有 transpose method.
- helper 作为 future-infrastructure 落地: 当 PackedGF2Matrix 暴露 +
  Gauss-Jordan 路径需要 column-major access, 或 dense SpMV 实验 (e.g.
  Block Lanczos rebirth) 引入 materialized 转置时, 直接调用即可.

**集成点** (2026-05-22):
- `include/gnfs/linalg/detail/transpose_blocked.hpp` — helper API + ENV
  gate + 64×64 in-register transpose primitive + 朴素 reference.
- `tests/test_transpose_blocked.cpp` — 13 correctness + ENV + threshold
  tests, 全部 instant tier (binary 运行 < 50 ms).
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout.

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点,
所以 ENV 对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

### GF(2) word popcount SIMD batch (GNFS_GF2_POPCNT_SIMD)

**ENV `GNFS_GF2_POPCNT_SIMD=auto|0|1`** (2026-05-22 实施, default auto):
GF(2) word array 批量 popcount helper, 提供 NEON 2-lane (ARM64) /
AVX2 4-lane (x86_64) wide popcount 替代逐 word `__builtin_popcountll`.
应用场景: matrix column-weight tally, dependency Hamming distance,
parity check 等需要 batch popcount uint64_t 数组的内核. Pure header,
不依赖外部库.

```bash
GNFS_GF2_POPCNT_SIMD=auto ./gnfs <N>   # 默认: NEON/AVX2 可用则启用
GNFS_GF2_POPCNT_SIMD=0    ./gnfs <N>   # 强制 scalar (回归 bisect 用)
GNFS_GF2_POPCNT_SIMD=1    ./gnfs <N>   # 强制 SIMD (无 SIMD 平台 fallback)
unset GNFS_GF2_POPCNT_SIMD             # 同 auto
```

**Helper API** (`include/gnfs/linalg/detail/popcount_simd.hpp`):
- `batch_popcount_words(words, out)` — 主入口, `out[i] = popcount(words[i])`.
  SIMD path 当 `popcount_simd_enabled()` 为 true 时启用. `out.size() ==
  words.size()` 必须成立 (defensive clamp 防止 UB write past out).
- `total_popcount_words(words)` — 累加 sum 入口, 返回 `uint64_t` 总和.
- `batch_popcount_words_scalar(words, out)` — 朴素 `__builtin_popcountll`
  参考 (test golden + 无 SIMD fallback).
- `total_popcount_words_scalar(words)` — 朴素累加参考.
- `popcount_simd_mode()` — 返回 `PopcountSimdMode { Auto, ForceOff, ForceOn }`.
- `popcount_simd_enabled()` — 三态 dispatcher decision (ForceOff → false,
  ForceOn/Auto + supported → true, 否则 false).
- `popcount_simd_supported()` — compile-time `__ARM_NEON / __AVX2__` 探测.
- `popcount_simd_reset_env_cache_for_testing()` — 测试专用 re-resolve ENV.

**算法**:
- NEON: `vld1q_u64(2 word)` → `vcntq_u8` (16-byte popcount) →
  `vget_low_u8 + vaddv_u8` (per-word horizontal sum) per word.
  Tail 走 scalar `__builtin_popcountll`.
- AVX2: `_mm256_popcnt_epi64` 若 AVX-512 VPOPCNTDQ 可用, 否则 fallback
  `_mm_popcnt_u64` 4-wide unroll (POPCNT 指令在 Nehalem+ x86_64 单条).
- Reduction: `total_popcount_words` 用单条 `vaddvq_u8` (NEON) 或累加
  4 个 popcount (AVX2) 减少 horizontal sum 次数.

**Bit-for-bit guarantee**: popcount 是 pure function of input word, SIMD
path 与 scalar `__builtin_popcountll` 输出严格 per-index 一致, reduction
sum 同样 byte-identical. 空输入返回空 output / 零 total 而不 touch pointers.
单元测试 `test_popcount_simd` 13 个测试强制覆盖 (4 ENV / empty / 单 word
8 pattern / aligned 32 / unaligned 33 / random 1000 / total parity /
ForceOff vs Auto parity / 1M perf info / undersized out clamping).

**ROI 与定位**:
- 主要 ROI: 大 batch (>1k word) popcount wall-time. M-series ARM64 上 `CNT`
  指令 4-way pipelined, scalar `__builtin_popcountll` 在 Apple Silicon 已经
  非常快, perf-info 1M word 实测 scalar 略快 (autovectorise 优秀). x86_64
  上若有 AVX-512 VPOPCNTDQ 可见显著加速 (1 instruction per 4 word).
- helper 当前 standalone (主 pipeline 无 wire-in), 是 future-infra:
  Block Lanczos / Block Wiedemann 内部 distance metric, column weight
  tally, parity check 等 batch popcount 调用点 explicit wire-in 后启用.
- 默认 auto 在 macOS arm64 / Linux x86_64 都启用 SIMD path; ENV=0 在
  PMU sweep / sanitizer 调试时回到 scalar baseline.

**集成点** (2026-05-22):
- `include/gnfs/linalg/detail/popcount_simd.hpp` — helper API + ENV gate +
  NEON / AVX2 inner kernels + 朴素 reference.
- `tests/test_popcount_simd.cpp` — 13 个测试 (4 ENV 解析 + 5 batch
  correctness + 2 total reduction + 1 ForceOff vs Auto parity + 1 perf
  info + 1 undersized out span clamping).
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout.

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点,
所以 ENV 对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

### GF(2) AND-popcount SIMD batch (GNFS_GF2_AND_POPCNT_SIMD)

**ENV `GNFS_GF2_AND_POPCNT_SIMD=auto|0|1`** (2026-05-22 实施, default auto):
GF(2) batch `popcount(a[i] & b[i])` helper, AND-fused 兄弟 helper of
`GNFS_GF2_POPCNT_SIMD`. 提供 NEON 2-lane (ARM64) / AVX2 4-lane (x86_64)
wide AND-then-popcount 替代逐 word `__builtin_popcountll(a[i] & b[i])`.
应用场景: Block Lanczos / Block Wiedemann 正交性检查 (`v^T·w` GF(2)
inner-product 是 `total_and_popcount_words(v, w)` 的 parity), parity
dot-product reduction, GF(2) inner product 等需要 batch AND + popcount
两个 uint64_t array 的内核. Pure header, 不依赖外部库.

```bash
GNFS_GF2_AND_POPCNT_SIMD=auto ./gnfs <N>   # 默认: NEON/AVX2 可用则启用
GNFS_GF2_AND_POPCNT_SIMD=0    ./gnfs <N>   # 强制 scalar (回归 bisect 用)
GNFS_GF2_AND_POPCNT_SIMD=1    ./gnfs <N>   # 强制 SIMD (无 SIMD 平台 fallback)
unset GNFS_GF2_AND_POPCNT_SIMD             # 同 auto
```

**Helper API** (`include/gnfs/linalg/detail/and_popcnt_simd.hpp`):
- `batch_and_popcount_words(a, b, out)` — 主入口, `out[i] = popcount(a[i] & b[i])`.
  SIMD path 当 `and_popcnt_simd_enabled()` 为 true 时启用. `a.size() ==
  b.size()` 必须成立 (debug build assert); `out.size() < a.size()` 时
  defensive clamp 到 `out.size()` 防止 UB write past out.
- `total_and_popcount_words(a, b)` — 累加 sum 入口, 返回 `uint64_t` 总和.
- `batch_and_popcount_words_scalar(a, b, out)` — 朴素 `__builtin_popcountll(a & b)`
  参考 (test golden + 无 SIMD fallback).
- `total_and_popcount_words_scalar(a, b)` — 朴素累加参考.
- `and_popcnt_simd_mode()` — 返回 `AndPopcntSimdMode { Auto, ForceOff, ForceOn }`.
- `and_popcnt_simd_enabled()` — 三态 dispatcher decision (ForceOff → false,
  ForceOn/Auto + supported → true, 否则 false).
- `and_popcnt_simd_supported()` — compile-time `__ARM_NEON / __AVX2__` 探测.
- `and_popcnt_simd_reset_env_cache_for_testing()` — 测试专用 re-resolve ENV.

**算法**:
- NEON: `vld1q_u64(2 word)` × 2 inputs → `vandq_u64` → `vcntq_u8`
  (16-byte popcount) → `vget_low_u8 + vaddv_u8` (per-word horizontal sum)
  per word. Tail 走 scalar `__builtin_popcountll(a & b)`.
- AVX2: `_mm256_loadu_si256(4 word)` × 2 inputs → `_mm256_and_si256` →
  `_mm256_popcnt_epi64` 若 AVX-512 VPOPCNTDQ 可用, 否则 fallback
  `_mm_popcnt_u64` 4-wide unroll after 4-lane store (POPCNT 指令在
  Nehalem+ x86_64 单条).
- Reduction: `total_and_popcount_words` 用单条 `vaddvq_u8` (NEON) 或累加
  4 个 popcount (AVX2) 减少 horizontal sum 次数.

**Bit-for-bit guarantee**: `popcount(a & b)` 是 pure function of (a, b),
SIMD path 与 scalar `__builtin_popcountll(a & b)` 输出严格 per-index 一致,
reduction sum 同样 byte-identical. 空输入返回空 output / 零 total 而不
touch pointers. 单元测试 `test_and_popcnt_simd` 14 个测试强制覆盖 (4 ENV /
empty / 单 word 8 pattern / aligned 32 / unaligned 33 / random 1000 /
total parity / ForceOff vs Auto parity / 1M perf info / undersized out
clamping / a==b 等同 plain popcount 防止 AND/OR/XOR 误用).

**ROI 与定位**:
- 主要 ROI: GF(2) 向量 inner product 与 Block Lanczos / Block Wiedemann
  正交性检查的 hot path. 单一 fused AND+popcount kernel 比独立 AND +
  独立 popcount 少一次 memory traffic + 一次 register round-trip.
- helper 当前 standalone (主 pipeline 无 wire-in), 是 future-infra:
  Block Lanczos / Block Wiedemann 内部 dot-product / orthogonality
  check 等 explicit wire-in 后启用.
- 默认 auto 在 macOS arm64 / Linux x86_64 都启用 SIMD path; ENV=0 在
  PMU sweep / sanitizer 调试时回到 scalar baseline. perf-info 实测
  1M word M-series ARM64: scalar ~2.0 ms, SIMD ~5.7 ms (CNT autovectorise
  在 Apple Silicon 上对单 array 已经非常优秀, 而 AND-fused 路径多了 2 倍
  load + 1 个 AND op; ROI 在 x86_64 AVX-512 VPOPCNTDQ 平台或 wire-in
  到 GF(2) inner product hot loop 后体现).

**集成点** (2026-05-22, W10 T1):
- `include/gnfs/linalg/detail/and_popcnt_simd.hpp` — helper API + ENV gate +
  NEON / AVX2 inner kernels + 朴素 reference.
- `tests/test_and_popcnt_simd.cpp` — 14 个测试 (4 ENV 解析 + 6 batch
  correctness + 1 total reduction + 1 ForceOff vs Auto parity + 1 perf
  info + 1 undersized out span clamping + 1 a==b 等同 plain popcount).
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout.

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点,
所以 ENV 对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

### GF(2) row word XOR SIMD batch (GNFS_GF2_ROW_XOR_SIMD)

**ENV `GNFS_GF2_ROW_XOR_SIMD=auto|0|1`** (2026-05-22 实施, W11 T1, default auto):
GF(2) batch in-place `dst[i] ^= src[i]` helper, 与 W9 `GNFS_GF2_POPCNT_SIMD`
/ W10 `GNFS_GF2_AND_POPCNT_SIMD` 并列的第三个 SIMD primitive. 提供 NEON
2-lane (ARM64, `veorq_u64`) / AVX2 4-lane (x86_64, `_mm256_xor_si256`)
wide XOR 替代逐 word `^=`. 应用场景: Block Lanczos / Block Wiedemann
row-block 更新 (`accumulator ^= rhs` over packed GF(2) word array),
Krylov vector recurrence, parity accumulation, dependency XOR 等需要
batch in-place XOR uint64_t 数组的内核. Pure header, 不依赖外部库.

```bash
GNFS_GF2_ROW_XOR_SIMD=auto ./gnfs <N>   # 默认: NEON/AVX2 可用则启用
GNFS_GF2_ROW_XOR_SIMD=0    ./gnfs <N>   # 强制 scalar (回归 bisect 用)
GNFS_GF2_ROW_XOR_SIMD=1    ./gnfs <N>   # 强制 SIMD (无 SIMD 平台 fallback)
unset GNFS_GF2_ROW_XOR_SIMD             # 同 auto
```

**Helper API** (`include/gnfs/linalg/detail/xor_words_simd.hpp`):
- `batch_xor_words(dst, src)` — 主入口, `dst[i] ^= src[i]` for
  `i in [0, min(dst.size(), src.size()))`. SIMD path 当
  `xor_words_simd_enabled()` 为 true 时启用. Empty 输入 (任一 span size 0)
  no-op, 不 touch dst.
- `batch_xor_words_scalar(dst, src)` — 朴素 `dst[i] ^= src[i]` loop
  参考 (test golden + 无 SIMD fallback).
- `xor_words_simd_mode()` — 返回 `XorWordsSimdMode { Auto, ForceOff, ForceOn }`.
- `xor_words_simd_enabled()` — 三态 dispatcher decision (ForceOff → false,
  ForceOn/Auto + supported → true, 否则 false).
- `xor_words_simd_supported()` — compile-time `__ARM_NEON / __AVX2__` 探测.
- `xor_words_simd_reset_env_cache_for_testing()` — 测试专用 re-resolve ENV.

**算法**:
- NEON: `vld1q_u64(2 word)` × dst+src → `veorq_u64` (128-bit XOR) →
  `vst1q_u64(2 word)` per 2-word stride. Tail 走 scalar `^=`.
- AVX2: `_mm256_loadu_si256(4 word)` × dst+src → `_mm256_xor_si256`
  (256-bit XOR) → `_mm256_storeu_si256(4 word)` per 4-word stride.
  Tail 走 scalar `^=`.
- Defensive clamp: `min(dst.size(), src.size())` 决定 XOR 字数, 避免
  UB write past dst 或 read past src. Length mismatch 不抛异常, 静默
  clamp (符合 W9 / W10 兄弟 helper 的契约).

**Bit-for-bit guarantee**: XOR 是 pure function of (dst[i], src[i]),
SIMD path 与 scalar `^=` 输出严格 per-index 一致. 空输入返回不 touch
dst (size 不变, content 不变). XOR with self (src == dst content)
精确产出 all-zero. 单元测试 `test_xor_words_simd` 14 个测试强制覆盖
(4 ENV / empty / 单 word 8 pattern / aligned 32 / unaligned 33 /
random 1000 / ForceOff vs Auto parity / self-XOR identity / dst <
src 长度 clamp / src < dst 长度 tail 保留 / 1M perf info).

**ROI 与定位**:
- 主要 ROI: Block Lanczos / Block Wiedemann inner kernel 频繁
  `dst ^= src` row block updates, Krylov recurrence (`V_next ^=
  M * V_prev` 累积), dependency XOR 等 batch in-place hot path. 单
  fused load+XOR+store kernel 节省 per-iter address-gen pressure.
- helper 当前 standalone (主 pipeline 无 wire-in), 是 future-infra:
  Block Lanczos / Block Wiedemann row-block accumulation, Krylov
  vector recurrence, parity sweep 等 explicit wire-in 后启用.
- 默认 auto 在 macOS arm64 / Linux x86_64 都启用 SIMD path; ENV=0 在
  PMU sweep / sanitizer 调试时回到 scalar baseline. perf-info 实测
  1M word M-series ARM64: scalar ~2.4 ms, SIMD ~3.7 ms (scalar `^=`
  loop 在 Apple Silicon 上 autovectorise 已经非常优秀; SIMD path 主
  ROI 在 x86_64 AVX2 平台或 wire-in 到 row-block hot loop 后体现).

**集成点** (2026-05-22, W11 T1):
- `include/gnfs/linalg/detail/xor_words_simd.hpp` — helper API + ENV gate +
  NEON / AVX2 inner kernels + 朴素 reference.
- `tests/test_xor_words_simd.cpp` — 14 个测试 (4 ENV 解析 + empty + 单
  word 8 pattern + aligned 32 + unaligned 33 + random 1000 + ForceOff
  vs Auto parity + self-XOR identity + dst < src clamp + src < dst
  tail 保留 + 1M perf info).
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  linalg 模块.

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点,
所以 ENV 对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

### Sieve region tile bits (GNFS_SIEVE_REGION_TILE_BITS)

**ENV `GNFS_SIEVE_REGION_TILE_BITS=N`** (2026-05-22 实施, range [0, 8], default 0):
Lattice sieve `sieve_bucket_region` 的 apply scan 阶段可按 row 将
`sieve_array_` 划分成 `2^N` 行 tile, 每个 tile 完整扫描后再进下一 tile,
提升 L1/L2 cache locality. 默认 0 (unset / "0" / 空 / 非数字) 走原 untiled
scan path, 零开销保留原行为. N>=1 时 tile size = `2^N` rows, N>=9 clamp
到 8 (256-row tile 上限, 超出该值 tile 不再适合 L2 容量, 反而使 apply
scan 付出 cache eviction 代价).

```bash
GNFS_SIEVE_REGION_TILE_BITS=0  ./gnfs <N>   # default, untiled scan (零开销)
GNFS_SIEVE_REGION_TILE_BITS=4  ./gnfs <N>   # 16-row tile
GNFS_SIEVE_REGION_TILE_BITS=6  ./gnfs <N>   # 64-row tile
GNFS_SIEVE_REGION_TILE_BITS=8  ./gnfs <N>   # 256-row tile (上限)
GNFS_SIEVE_REGION_TILE_BITS=10 ./gnfs <N>   # 同 8 (clamp)
unset GNFS_SIEVE_REGION_TILE_BITS           # 同 default 0
```

**Helper API** (`include/gnfs/sieve/region_tile.hpp`):
- `region_tile_bits()` — 返回 cached N, clamp 到 `[0, kRegionTileMaxBits]`.
- `region_tile_enabled()` — `bits > 0` 的等价 predicate.
- `region_tile_size_rows()` — 返回 `1 << N` (启用) 或 0 (禁用).
- `region_tile_reset_env_cache_for_testing()` — 测试专用 re-resolve.
- `kRegionTileMaxBits = 8` — 256-row tile cap.

**算法**: 启用时 caller 把 row range 划分成 `floor(rows / 2^N)` 个 tile,
每个 tile `2^N` rows 完整 scan 后再进下一个 (rows 非 `2^N` 倍数时尾部
tile 是残余 rows). gather pass 不变, 仅 apply scan 的 iteration order
从 "整行 region 顺序" 改为 "tile-by-tile 顺序". Candidate threshold check
是 `(residual, threshold)` 的 pure function, 与 scan order 无关.

**Bit-for-bit guarantee**: `sieve_array_` 内容 + candidate list 与 N=0
路径完全一致. tile 仅改变扫描顺序, 不改变 candidate threshold check 结果.
每个 wire-in 的 callsite 必须在自身 fixture 验证 candidate 输出与
untiled baseline match (后续 wire-in 时由 callsite regression 测试保证).

**ROI 与定位**:
- 主要 ROI: 50d+/60d sieve region (`(i_max - i_min) * j_count` 几万 byte)
  时, apply scan 与 gather pass 争抢 L1 working set, 第二 pass 起在 L1
  cold. tile 把每个 scan window 控制在 L1/L2 内 (16-row tile = 1 KiB),
  让 scan 完成前 cache miss 不发生.
- N 选择: 16-row (N=4) ~ 64-row (N=6) 是常见 sweet spot. 256-row (N=8)
  在极宽 j 时仍有 ROI, 但接近 L2 边界. 25d/40-bit small region 上 N>=1
  的 ROI 可忽略, 但行为正确性不变.
- helper 当前 standalone (apply-loop wiring 留给后续 task), 所以 ENV 不
  影响主 pipeline 运行行为. 仅 helper 被 wire-in 后 ENV 才生效.

**集成点** (2026-05-22):
- `include/gnfs/sieve/region_tile.hpp` — helper API + ENV gate + clamp.
- `tests/test_sieve_region_tile.cpp` — 8 个测试 (unset default / "0"
  explicit / [1..8] sweep / >=9 clamp / non-numeric / size_rows = 2^N /
  reset hook re-read / enabled predicate). 全部 instant tier.
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout.

**Default OFF (N=0)**: 任何 caller 不设 ENV 时 helper 报告 disabled,
应用 untiled scan path 不变. 仅当用户 explicit `GNFS_SIEVE_REGION_TILE_BITS=N>0`
时 helper 报告 tile size; 实际启用还需 callsite 显式 dispatch helper 结果.

### Sieve norm tile bits (GNFS_SIEVE_NORM_TILE_BITS)

**ENV `GNFS_SIEVE_NORM_TILE_BITS=N`** (2026-05-22 实施, range [0, 8], default 0):
Lattice sieve 的 norm 预计算 (`Polynomial::evaluate(a, b)` 对每个 lattice
cell 求 |F(a,b)| / |G(a,b)| seed `sieve_array_` 起始 log-residual) 可按
row 将 region 划分成 `2^N` 行 tile, 每个 tile 完整预计算后再进下一 tile,
让 polynomial coefficients 在每个 tile 期间留在 L1 hot. 默认 0
(unset / "0" / 空 / 非数字) 走原 untiled row-major 预计算 path, 零开销
保留原行为. N>=1 时 tile size = `2^N` rows, N>=9 clamp 到 8 (256-row tile
上限, 超出后 L2 边界, ROI 反转).

**与 W6 region_tile_bits 互补但 distinct**: 后者 (`GNFS_SIEVE_REGION_TILE_BITS`,
`<gnfs/sieve/region_tile.hpp>`) tile 的是 *apply scan* 阶段 (第二 pass 扫
`sieve_array_` 发候选), 这个 (`<gnfs/sieve/norm_tile.hpp>`) tile 的是 *norm
预计算* 阶段 (seed `sieve_array_` 的第一 pass). 二者有完全独立的 cache /
ENV / 调优, caller 视具体 fixture 哪个 phase dominate 决定单开或同开.
设成同一值合理但非必须.

```bash
GNFS_SIEVE_NORM_TILE_BITS=0  ./gnfs <N>   # default, untiled precompute (零开销)
GNFS_SIEVE_NORM_TILE_BITS=4  ./gnfs <N>   # 16-row tile
GNFS_SIEVE_NORM_TILE_BITS=6  ./gnfs <N>   # 64-row tile
GNFS_SIEVE_NORM_TILE_BITS=8  ./gnfs <N>   # 256-row tile (上限)
GNFS_SIEVE_NORM_TILE_BITS=10 ./gnfs <N>   # 同 8 (clamp)
unset GNFS_SIEVE_NORM_TILE_BITS           # 同 default 0

# 与 region_tile 同开 (二者完全独立)
GNFS_SIEVE_NORM_TILE_BITS=4 GNFS_SIEVE_REGION_TILE_BITS=6 ./gnfs <N>
```

**Helper API** (`include/gnfs/sieve/norm_tile.hpp`):
- `norm_tile_bits()` — 返回 cached N, clamp 到 `[0, kNormTileMaxBits]`.
- `norm_tile_enabled()` — `bits > 0` 的等价 predicate.
- `norm_tile_size_rows()` — 返回 `1 << N` (启用) 或 0 (禁用).
- `norm_tile_reset_env_cache_for_testing()` — 测试专用 re-resolve.
- `kNormTileMaxBits = 8` — 256-row tile cap.

**算法**: 启用时 caller 把 row range 划分成 `floor(rows / 2^N)` 个 tile,
每个 tile `2^N` rows 完整预计算 (`Polynomial::evaluate(a, b)` over 2^N row
× j_count cells) 后再进下一个 (rows 非 `2^N` 倍数时尾部 tile 是残余 rows).
gather + apply pass 不变, 仅 norm 预计算的 iteration order 从 "整行 region
顺序" 改为 "tile-by-tile 顺序". `Polynomial::evaluate(a, b)` 是 (a, b) 与
polynomial coefficients 的 pure function, 每 cell 写入 disjoint scratch
位置.

**Bit-for-bit guarantee**: norm scratch buffer 内容 (与 seed 到 `sieve_array_`
的 log-residual) 与 N=0 路径完全一致. tile 仅改变 row 扫描顺序, 不改变
polynomial evaluation 结果. 每个 wire-in 的 callsite 必须在自身 fixture
验证 seed residuals (与下游 candidate list) 与 untiled baseline match
(后续 wire-in 时由 callsite regression 测试保证).

**ROI 与定位**:
- 主要 ROI: 50d+/60d sieve region (`(i_max - i_min) * j_count` 几万 cells)
  时, norm 预计算与 gather pass 争抢 L1 (polynomial coefficients + scratch
  buffer + bucket region vectors). tile 把每个 norm-precompute window 控制
  在 L1/L2 内 (16-row tile, coefficient pressure 控制在 ~几 KiB), 让
  polynomial coefficients 在 tile 期间不被 evict.
- N 选择: 16-row (N=4) ~ 64-row (N=6) 是常见 sweet spot. 256-row (N=8) 在
  极宽 j 时仍有 ROI, 但接近 L2 边界. 25d/40-bit small region 上 N>=1 的 ROI
  可忽略, 但行为正确性不变.
- helper 当前 standalone (norm-precompute wiring 留给后续 task), 所以 ENV
  不影响主 pipeline 运行行为. 仅 helper 被 wire-in 后 ENV 才生效.

**集成点** (2026-05-22):
- `include/gnfs/sieve/norm_tile.hpp` — helper API + ENV gate + clamp.
- `tests/test_sieve_norm_tile.cpp` — 8 个测试 (unset default / "0" explicit /
  [1..8] sweep / >=9 clamp / non-numeric / size_rows = 2^N / reset hook re-read
  / enabled predicate). 全部 instant tier.
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout.

**Default OFF (N=0)**: 任何 caller 不设 ENV 时 helper 报告 disabled, 应用
untiled precompute path 不变. 仅当用户 explicit `GNFS_SIEVE_NORM_TILE_BITS=N>0`
时 helper 报告 tile size; 实际启用还需 callsite 显式 dispatch helper 结果.

### Factor Base CZ roots 并行 (GNFS_FB_ROOTS_THREADS)

**ENV `GNFS_FB_ROOTS_THREADS=N`** (2026-05-22 实施, default 0, range [0, hardware_concurrency * 2]):
Factor Base 构建的 Cantor-Zassenhaus 求根 per-prime parallel dispatcher.
`find_roots_mod_p(ctx, p)` 是 pure function of `(p, monic_polynomial_mod_p)`,
跨素数相互独立 (embarrassingly parallel). 当前 `src/factor_base/builder.cpp`
主路径直接用 `std::vector<std::thread>` + `std::thread::hardware_concurrency()`,
没有 runtime 旋钮可调.

```bash
unset GNFS_FB_ROOTS_THREADS              # default: hardware_concurrency() (legacy)
GNFS_FB_ROOTS_THREADS=0  ./gnfs <N>      # 同 default, 显式
GNFS_FB_ROOTS_THREADS=1  ./gnfs <N>      # 强制 sequential (lldb / sanitizer)
GNFS_FB_ROOTS_THREADS=4  ./gnfs <N>      # 显式 4 thread (CI runner 用)
GNFS_FB_ROOTS_THREADS=16 ./gnfs <N>      # 高并发实验
```

**Semantics**:
- N == 0 (默认, unset / "0" / negative / 非数字 / 空字符串): 走 helper "fall
  back to hardware_concurrency()" path, 对调用方等价于现有 `std::thread::
  hardware_concurrency()` 行为, bit-for-bit identical
- N == 1: 强制 sequential, 不创建 ThreadPool. 用于 `lldb` 单步, sanitizer 调试
  (concurrent GMP 在某些 sanitizer 下噪声大), 回归 bisect
- N >= 2: 显式 N-worker `util::ThreadPool` dispatch
- 超出 `hardware_concurrency() * 2` 自动 clamp (fallback cap 16 if hw==0)
- 非数字 / 空 / 负数 / partial-parse-empty 都解析为 0 (== default)
- `"12abc"` 解析为 12 (std::stoi 接受前缀, consumed > 0)

**并行模型**:
- Entry = `parallel_fb_roots<Result>(primes, worker_fn)` over n primes
- Inner = dedicated `gnfs::util::ThreadPool(min(threads, n))` + `parallel_for_index(0, n, lambda)`
- 每个 task 调 `worker_fn(primes[i])` 写到 `results[i]` (disjoint per index)
- 空 batch (n==0) 与 单 prime (n==1) 都走 sequential 短路, 不创建 pool
- 默认 fallback 用 `hardware_concurrency()`, 与 legacy `src/factor_base/builder.cpp`
  内的 `std::thread` 数量保持一致

**Bit-for-bit guarantee**: `worker_fn(p)` 是 pure function of `p` 加 read-only
lambda capture, output[i] 仅由 owns-index-i 的 task 写入, output 容器预 size.
单元测试 `tests/test_fb_roots_parallel.cpp` 强制 197 + 1000 + 500 prime sweep
across N=1 / N=4 / N=hardware_concurrency 严格 per-index 比较.

**ROI 与定位**:
- 主要 ROI: opt-in 控制能力. 当前 `src/factor_base/builder.cpp` 已经 parallel
  (按 `hardware_concurrency()`), 主路径并不缺乏并行度. helper 价值在于:
    * Debug: ENV=1 强制 sequential, lldb 单步无需修改源码
    * Sandbox CI runner (2-4 vCPU): ENV=N 显式 cap, 避免与其他 step 竞争 CPU
    * 实验: thread-count sweeps 测 ROI 边际效应
    * 复用: future 任何 per-prime parallel 工作都可经此 dispatcher
- 主路径 wall-time: ENV=0 (默认) 与 legacy 直接 spawn `hardware_concurrency()` thread
  完全等价, 零行为变化
- Default OFF (N=0): 任何调用方未设 ENV 均走 fall-back path, 零回归风险

**集成点** (2026-05-22):
- `include/gnfs/factor_base/fb_roots_parallel.hpp` — `fb_roots_threads()` env
  reader with `std::once_flag` cache + `resolve_fb_roots_threads(n)` helper +
  `parallel_fb_roots<Result>(primes, worker_fn)` template dispatcher +
  `fb_roots_threads_reset_env_cache_for_testing()` test hook
- `src/factor_base/builder.cpp` — **未改动** (主路径继续走原 `std::thread` +
  `hardware_concurrency()`). helper 是 future-infra, 等真有 wire-in 需求时用
- `tests/test_fb_roots_parallel.cpp` — 12 个测试 (6 ENV 解析 + 5 dispatcher
  parity + 1 partial-parse 行为文档化)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60 s timeout,
  factor_base 模块

### Couveignes pattern search 并行 (GNFS_COUVEIGNES_PARALLEL_THREADS)

**ENV `GNFS_COUVEIGNES_PARALLEL_THREADS=N`** (2026-05-22 实施, default 1, range [1, hardware_concurrency * 2]):
Couveignes algebraic sqrt 在 sign-pattern search 阶段穷举 2^16 = 65536 sign
patterns. 每个 pattern 的 verify (compute_candidate_signed_root +
quad-character filter check) 在 read-only shared state 下独立, 是
embarrassingly parallel. N=1 (默认) 走 sequential Gray-code 路径 (bit-for-bit
等同 Couveignes legacy 行为), 不创建 ThreadPool, 零开销. N>=2 时把
pattern range [start, end) 切 N 个 chunk 派发到 ThreadPool, 任一 worker
找到 first match 通过 `std::atomic<bool>` short-circuit signal 让其余
worker 提前退出, `std::atomic<uint64_t>` first_match 做 atomic-min
reduction 收最小匹配 index.

```bash
GNFS_COUVEIGNES_PARALLEL_THREADS=1 ./gnfs <N>   # default sequential, zero overhead
GNFS_COUVEIGNES_PARALLEL_THREADS=4 ./gnfs <N>   # 4 workers, partition 65536 范围
GNFS_COUVEIGNES_PARALLEL_THREADS=8 ./gnfs <N>   # 8 workers
unset GNFS_COUVEIGNES_PARALLEL_THREADS          # same as N=1
```

**"First valid pattern" 语义**:
- Sequential (N=1): 返回 scan 顺序的 first valid pattern (legacy Couveignes 行为)
- Parallel (N>=2): 返回 atomic-min observed match. 当 search space 仅含
  唯一 valid pattern 时, 与 sequential first-match 等价 (二者输出相同).
  多 valid pattern 场景下选择不严格 deterministic, 但保证返回的 pattern
  通过 verify_fn (语义正确, 仅位置选择有运行间差异).
- 调用方若需要严格确定的选择, 应自行调整 verify_fn 让仅 1 个 pattern 匹配.

**并行模型**:
- Outer = `parallel_pattern_search<VerifyFn>(start, end, verify_fn)` over
  K = end - start patterns (典型 K = 65536)
- Inner = `gnfs::util::ThreadPool` 大小为 min(N, K), 每 worker 各自扫
  ~K/N 连续 chunk, 每 iter 用 acquire load 检查 found_flag 提前退出
- Empty range / range == 1 / N == 1 都走 sequential 短路 (零 ThreadPool 开销)
- `verify_fn` 必须 thread-safe: 仅读 shared immutable state (weights,
  base CRT, expected residues), 不写 shared mutable. Per-thread scratch
  应该在 verify_fn 内 (thread_local 或 per-call 构造)

**Bit-for-bit guarantee** (N=1 path): N=1 sequential 路径 byte-identical
等同于原始 Gray-code loop 输出. N>=2 路径在 single-valid 场景下与 N=1
返回相同 pattern index; multi-valid 场景下返回任一 valid index (atomic-min
偏向最小观察值, 但跨 chunk 调度不严格 deterministic). 单元测试
`tests/test_couveignes_parallel.cpp` 16 个测试强制覆盖.

**ROI 与定位**:
- 主要 ROI: 50d+/60d Couveignes 兜底路径 (Nguyen Hybrid 失败时进入)
  的 sign search 阶段 wall-time 由 ~K 倍 single-verify 时间 → ~K/N + tasking
  overhead. perf-info 实测 65536 patterns + mock heavy verify: N=4 ≈ 2.77x
  N=1 (M5 10-core P-cores).
- ROI 主要在大 N stress 路径, 25d gate 走 Nguyen Hybrid first 不进入
  Couveignes search.
- Helper 仅是 standalone template, **不修改** `include/gnfs/sqrt/couveignes.hpp`
  主路径 (future-infra, 类似 W7 T2/T3 helper-only landings). 当用户决定
  wire-in Couveignes 主 search loop 时直接调用即可.

**集成点** (2026-05-22):
- `include/gnfs/sqrt/couveignes_parallel.hpp` — `couveignes_parallel_threads()`
  env reader with `std::once_flag` cache + `parallel_pattern_search<VerifyFn>`
  template dispatcher + `couveignes_parallel_threads_reset_env_cache_for_testing()`
  test hook + `parse_couveignes_parallel_threads_env()` strict numeric prefix
  validation (any non-digit after optional sign treated as invalid -> 1)
- `tests/test_couveignes_parallel.cpp` — 16 个测试 (5 env parsing /
  2 sequential / 4 parallel + atomic-min / 2 edge cases + reset / 1 perf info /
  1 single-pattern range / 1 dense-match)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout

**Default OFF (N=1)**: 任何 caller 不设 ENV 也不传 helper 调用时完全跑
历史 sequential 路径, 零行为变化. 仅 Couveignes 主路径 wire-in helper +
用户 explicit opt-in 时启用.

### Partial relation merger 并行 (GNFS_FILTER_MERGE_THREADS)

**ENV `GNFS_FILTER_MERGE_THREADS=N`** (2026-05-22 实施, W10 T4, default 1, range [1, hardware_concurrency * 2]):
PartialRelationMerger (`include/gnfs/relation/filter.hpp`) 与 V3
CliqueRelationMerger (`include/gnfs/relation/clique_merger.hpp`) 都按 LP key
分桶后逐桶 merge partial relations 产 full relations. 同一 merge round 内
不同 LP-key bucket 的工作互不依赖, 满足 embarrassingly parallel. N=1 (默认)
走 sequential per-bucket 循环, 不创建 ThreadPool, 零开销保留原行为. N>=2 时
把 K 个 bucket dispatch 到大小为 min(N, K) 的 ThreadPool, bucket 之间靠
future 同步收口.

```bash
GNFS_FILTER_MERGE_THREADS=1 ./gnfs <N>    # default sequential, zero overhead
GNFS_FILTER_MERGE_THREADS=4 ./gnfs <N>    # 4 workers per merge round
GNFS_FILTER_MERGE_THREADS=8 ./gnfs <N>    # 8 workers
unset GNFS_FILTER_MERGE_THREADS           # same as N=1
```

**并行模型**:
- Outer = `parallel_merge_partials<Result, Bucket, MergeFn>(buckets, merge_fn)`
  over K 个 LP-key bucket (caller 自定义 Bucket 类型, e.g. `std::span<const
  Relation>` / `const std::vector<Relation>*` / 小描述符 struct)
- 内部 per-bucket merge 算法 bit-identical (helper 仅改变外层 dispatch,
  不触碰 `PartialRelationMerger::merge_all` / `CliqueRelationMerger::
  merge_cliques` 内核)
- 每个 bucket task 拥有独立 Integer / Relation buffer, GMP `mpz_*` 调用
  操作数互不重叠, 满足 GMP per-call disjoint-operands thread-safety
- 空 bucket span (n==0) / 单 bucket (n==1) 都走 sequential 短路, 不创建 pool
- Exception path: dispatcher drain 全部 future, 第一个 thrown exception
  通过 `std::rethrow_exception` 传给 caller (不 swallow); pool 析构干净 join

**Bit-for-bit guarantee**: 每 bucket merge 是 pure function of bucket content,
不依赖 dispatch 顺序. Sequential (N=1) 与 parallel (N>=2) 路径产生的
per-index `Result` 完全一致, downstream relation pool 严格相同. 由
`tests/test_merger_parallel.cpp` 强制覆盖 (N=1 vs N=4 vs N=hw 64+96
mixed bucket 严格 per-index bit-identical assert).

**ROI 与定位**:
- 主要 ROI: 50d+/60d adaptive sieve loop 每 round Phase 4 filter 进 merge
  阶段时, V0/V3 多个 LP-key bucket sequential 处理 wall-time 可见. K bucket
  并发后 outer wall ~ T_max_bucket + tasking overhead, 替代 sum(K) sequential
  累计.
- helper 与 W6 T4 RelationPoolResource (`GNFS_RELATION_POOL_SIZE`) /
  W6 `GNFS_FILTER_RADIX_SORT` Phase 0 dedup-sort / W9 T5 `GNFS_FILTER_LP_BLOOM_BITS`
  正交: 各自解决不同 hot site (memory pool / dedup sort / LP key dedup /
  merge dispatch). 可同时启用.
- Helper 是 opt-in 工具, **不修改** `PartialRelationMerger::merge_all` /
  `CliqueRelationMerger::merge_cliques` public path. 调用方需要自己 group
  by LP key 后传 buckets + merge_fn lambda.
- Default OFF (N=1) 保证 zero behavior change for legacy callers, 仅当用户
  明确 opt-in 时启用.

**集成点** (2026-05-22, W10 T4):
- `include/gnfs/relation/merger_parallel.hpp` — `filter_merge_threads()` env
  reader with `std::once_flag` cache + `parallel_merge_partials<Result,
  Bucket, MergeFn>` template dispatcher + `filter_merge_threads_reset_env_cache_for_testing()`
  test hook
- `tests/test_merger_parallel.cpp` — 12 个测试 (5 env parsing / sequential
  baseline / N=1 vs N=4 parity / N=1 vs N=hw parity / empty / single
  no-stall / non-trivial Result move / exception propagation)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  relation 模块

### GMP mpz_powm 批量并行 (GNFS_MPZ_POWM_BATCH_THREADS)

**ENV `GNFS_MPZ_POWM_BATCH_THREADS=N`** (2026-05-22 实施, W11 T3, default 1, range [1, hardware_concurrency * 2]):
GMP `mpz_powm`(modular exponentiation `base^exp mod modulus`) 在
多个独立 base 之间相互独立 (embarrassingly parallel). 每次 `mpz_powm`
调用是 `(base, exp, modulus)` 的 deterministic pure function, 满足
GMP per-call disjoint-operands thread-safety 契约 (每个 worker 写自己
disjoint 的 result slot, 共享 `exp` / `modulus` 仅 read). N=1 (默认)
走 sequential per-base 循环, 不创建 ThreadPool, 零开销保留原行为.
N>=2 时把 K 个 base dispatch 到大小为 min(N, K) 的 ThreadPool,
base 之间靠 future 同步收口.

```bash
GNFS_MPZ_POWM_BATCH_THREADS=1 ./gnfs <N>    # default sequential, zero overhead
GNFS_MPZ_POWM_BATCH_THREADS=4 ./gnfs <N>    # 4 workers for Schirokauer-style batch powm
GNFS_MPZ_POWM_BATCH_THREADS=8 ./gnfs <N>    # 8 workers
unset GNFS_MPZ_POWM_BATCH_THREADS           # same as N=1
```

**并行模型**:
- Outer = `parallel_mpz_powm(bases, exp, modulus, results)` over n bases
- Inner = `gnfs::util::ThreadPool` 大小为 min(N, bases.size()), 每 task 调
  `mpz_powm(results[i], bases[i], exp, modulus)` 写到 disjoint `results[i]` slot
- 内部 GMP modular exponentiation 算法 bit-identical (helper 仅改变外层
  dispatch, 不触碰 `mpz_powm` 内核或 `gnfs::core::powmod` wrapper)
- 共享 `exp` / `modulus` 仅由 worker read, 满足 "concurrent read 是安全的,
  仅 concurrent write 通过 alias `mpz_t` 才需要 disjoint operands" 的 GMP
  线程安全 invariant
- 空 batch (n==0) / 单 base (n==1) 都走 sequential 短路, 不创建 pool
- Exception path: dispatcher drain 全部 future, 第一个 thrown exception
  通过 `std::rethrow_exception` 传给 caller (不 swallow); pool 析构干净 join

**Bit-for-bit guarantee**: 每 base `mpz_powm` 是 pure function of `(base, exp,
modulus)`, 不依赖 dispatch 顺序. Sequential (N=1) 与 parallel (N>=2) 路径
产生的 per-index `Integer` 完全一致. 由
`tests/test_mpz_powm_parallel.cpp` 强制覆盖 (100-base random
N=1 vs N=4 vs N=hw 严格 per-index `mpz_cmp == 0` assert, plus 200-bit
prime modulus + 100-bit exponent 多 limb 路径 parity).

**ROI 与定位**:
- 主要 ROI: Schirokauer maps computation (`include/gnfs/linalg/schirokauer.hpp`)
  per-relation 调用 `mpz_powm` (modulus 100-300 bit, exponent 数十 bit) 上
  O(thousands) 关系的 batch wall-time 可观. K base 并发后 outer wall ~
  T_max_base + tasking overhead, 替代 sum(K) sequential 累计. 对 50d+/60d 大
  modulus 收益更显著 (single-call cost 增加, pool overhead 占比下降).
- 与 W7/W8/W9/W10 T4 parallel dispatcher family 互补:
    * W7 `GNFS_SQRT_HENSEL_THREADS` — Hensel lift K-prime slot
    * W8 T1 `GNFS_ECM_STAGE2_PARALLEL` — ECM Stage 2 BSGS 多曲线
    * W9 T1 `GNFS_ECM_STAGE1_PARALLEL_THREADS` — ECM Stage 1 Lucas-chain 多曲线
    * W10 T4 `GNFS_FILTER_MERGE_THREADS` — LP-key bucket merge
    * W11 T3 `GNFS_MPZ_POWM_BATCH_THREADS` — batched `mpz_powm`
  五者全部 default 1 (sequential), opt-in, 互不冲突. 可同时启用.
- Helper 是 opt-in 工具, **不修改** `gnfs::core::powmod` /
  Schirokauer maps / matrix-builder 主路径. 调用方需要自己 batch up 一组
  base (典型 a vector of per-relation `Integer`) + 共享 `(exp, modulus)` 后
  传入 `parallel_mpz_powm`. 当前主 pipeline 无 wire-in 调用, 是 future-infra.
- Default OFF (N=1) 保证 zero behavior change for legacy callers, 仅当用户
  明确 opt-in 时启用.

**集成点** (2026-05-22, W11 T3):
- `include/gnfs/util/mpz_powm_parallel.hpp` — `mpz_powm_batch_threads()` env
  reader with `std::once_flag` cache + `parallel_mpz_powm(bases, exp,
  modulus, results)` dispatcher + `mpz_powm_batch_threads_reset_env_cache_for_testing()`
  test hook
- `tests/test_mpz_powm_parallel.cpp` — 14 个测试 (5 env parsing / empty /
  single base N=1 / single base N=4 no-stall / N=1 vs scalar mpz_powm
  reference / N=1 vs N=4 parity / N=1 vs N=hw parity / 200-bit modulus
  common-exponent semantics / cache reset hook / perf info)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  util 模块

### Lattice basis reduction 多基并行 (GNFS_LATTICE_BASIS_PARALLEL_THREADS)

**ENV `GNFS_LATTICE_BASIS_PARALLEL_THREADS=N`** (2026-05-22 实施, W11 T4, default 1, range [1, hardware_concurrency * 2]):
GNFS lattice sieve `include/gnfs/sieve/lattice_basis.hpp` 在 sieve 主循环
开始前需要把每个 Special-Q 的 2 向量基 `(b1, b2)` 跑一遍 reduction (legacy
Gauss / 2D LLL / skew-LLL). 每个 basis 的 reduction 是 `(q, root, skew,
params)` 的 pure function, 不同 Special-Q 之间无 shared state, 满足
embarrassingly parallel. N=1 (默认) 走 sequential per-basis 循环, 不创建
ThreadPool, 零开销保留原行为. N>=2 时把 K 个 basis dispatch 到大小为
min(N, K) 的 ThreadPool, basis 之间靠 future 同步收口.

```bash
GNFS_LATTICE_BASIS_PARALLEL_THREADS=1 ./gnfs <N>   # default sequential, zero overhead
GNFS_LATTICE_BASIS_PARALLEL_THREADS=4 ./gnfs <N>   # 4 outer workers for basis reduction batch
GNFS_LATTICE_BASIS_PARALLEL_THREADS=8 ./gnfs <N>   # 8 outer workers
unset GNFS_LATTICE_BASIS_PARALLEL_THREADS          # same as N=1
```

**并行模型**:
- Outer = `parallel_lattice_basis_reduce<Result, Basis, ReduceFn>(basis_inputs,
  reduce_fn)` over K 个 Special-Q basis (caller 自定义 Basis 类型, e.g. 含
  `(q, root)` 或 explicit `(b1, b2)` integer pair 的小 struct)
- 内部 per-basis reduction 算法 bit-identical (helper 仅改变外层 dispatch,
  不触碰 `LatticeBasis::Gauss` / `LatticeBasis::LLL` / `LatticeBasis::SkewLLL`
  内核, 也不修改 `src/sieve/lattice_sieve.cpp` 主 sieve loop)
- 每个 basis task 拥有独立 Integer / Result buffer, GMP `mpz_*` 调用操作数
  互不重叠, 满足 GMP per-call disjoint-operands thread-safety
- 空 basis span (n==0) / 单 basis (n==1) 都走 sequential 短路, 不创建 pool
- Exception path: dispatcher drain 全部 future, 第一个 thrown exception
  通过 `std::rethrow_exception` 传给 caller (不 swallow); pool 析构干净 join

**Bit-for-bit guarantee**: 每 basis reduction 是 pure function of basis content,
不依赖 dispatch 顺序. Sequential (N=1) 与 parallel (N>=2) 路径产生的
per-index `Result` 完全一致, downstream sieve region 设置严格相同. 由
`tests/test_lattice_basis_parallel.cpp` 强制覆盖 (100-basis mock_reduce
N=1 vs N=4 vs N=hw_concurrency 严格 per-index bit-identical assert).

**ROI 与定位**:
- 主要 ROI: 50d+/60d sieve 主循环每 batch 可能有数百到上千 Special-Q,
  per-basis reduction 内部 GMP 多精度算术 (skew-LLL 涉及 Lagrange-Gauss
  iteration + 双向 reduce). K basis 并发后 outer wall ~ T_max_basis +
  tasking overhead, 替代 sum(K) sequential 累计. 当 reduction 内部走
  schoolbook GMP 大数时 ROI 显著.
- helper 与 W7 (`hensel_parallel`) / W8 T1 (`ecm_stage2_parallel`) /
  W9 T1 (`ecm_stage1_parallel`) / W10 T4 (`merger_parallel`) / W11 T3
  (mpz_powm 并行) 共享同一 ENV-gate + ThreadPool dispatcher 设计模式,
  是 parallel-dispatcher 家族的第五位成员. 各自独立 ENV 控制, 互相正交,
  可同时启用而不冲突.
- Helper 是 opt-in 工具, **不修改** `src/sieve/lattice_sieve.cpp` 主
  sieve loop. 调用方需要在 Special-Q batch 入口聚合 basis 后传 inputs +
  reduce_fn lambda 才生效, 是 future-infrastructure landing.
- Default OFF (N=1) 保证 zero behavior change for legacy callers, 仅当
  调用方 wire-in helper 且用户明确 `GNFS_LATTICE_BASIS_PARALLEL_THREADS=N>=2`
  时启用.

**集成点** (2026-05-22, W11 T4):
- `include/gnfs/sieve/lattice_basis_parallel.hpp` — `lattice_basis_parallel_threads()`
  env reader with `std::once_flag` cache + `parallel_lattice_basis_reduce<Result,
  Basis, ReduceFn>` template dispatcher + `lattice_basis_parallel_threads_reset_env_cache_for_testing()`
  test hook
- `tests/test_lattice_basis_parallel.cpp` — 13 个测试 (4 env parsing /
  empty input / single basis N=1 / single basis N=4 no-stall / 100 basis
  N=1 baseline / N=1 vs N=4 parity / N=1 vs N=hw parity / non-trivial
  Result move / exception propagation / reset env cache hook)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  sieve 模块

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

## README 与文档写作规范

`README.md` 是首次访问 GitHub 仓库页面的访客的「门面」，撰写或修改 README/docs 时严格遵循以下规范。所有规则同样适用于 `docs/` 下的设计文档、长 commit message body、PR 描述等正式文本。

### GitHub Markdown 渲染规范

GitHub 使用 GFM（GitHub Flavored Markdown），与 CommonMark 有细微差异，撰写时必须考虑实际渲染效果：

- **标题层级**：全文仅 1 个 H1（项目名，文件首行）；H2 作章节，H3 作子节；不跳级（H2 → H4 ❌）
- **标题锚点**：GitHub 自动从标题文本生成锚点（lowercase + dash，删除标点）。跨章节链接用 `[文本](#标题文本)`。含中文的锚点保持中文（URL encoded），但稳定性差，**优先用英文标题或独立 anchor**
- **代码块（必带语言标识）**：三反引号后紧贴语言标识，**不要留空格**
  - `` ```cpp ``、`` ```bash ``、`` ```cmake ``、`` ```python `` 等小写
  - 纯输出或无语言用 `` ```text ``
  - ❌ `` ``` `` 裸开 — GitHub 不会高亮且语义丢失
- **行内代码**：函数名、变量名、文件路径、命令、ENV 变量一律用反引号
  - ✅ `` `Pipeline::filter()` ``、`` `include/gnfs/api/pipeline.hpp` ``、`` `GNFS_OOC_RELATIONS` ``
- **表格**：GFM 表格需对齐符（`|---|:---:|---:|` 表左/居中/右）；单元格内不能换行，长文用 `<br>` 或拆行；列数 ≤ 6 以保证窄屏移动端不挤压
- **任务列表**：`- [ ]` 未完成 / `- [x]` 已完成（GFM 扩展；仅 README/issue/PR 渲染为复选框）
- **水平线**：统一用 `---`（不混用 `***` / `___`）
- **引用块**：`>` 前后必须留空行；嵌套用 `> >`（两层）
- **链接**：
  - 仓库内文件用相对路径：`[Pipeline](src/api/pipeline.cpp)` 优于绝对 URL
  - 裸 URL 用尖括号包裹防止 markdown linkify 失败：`<https://example.com>`
  - 锚点链接必须 **lowercase**：`[Build](#build-system)` ✅，`[Build](#Build-System)` ❌
- **图片**：
  - 优先存仓库内 `docs/images/`（用路径引用，不依赖外部 host）
  - **避免** `user-attachments.githubusercontent.com` 这类 issue 上传 URL — 用户重命名仓库或删除 attachment 会失效
  - 必须有 `alt` 文本：`![CI Status](badge.svg "CI passing")`，不要写 `![](badge.svg)`
  - 深浅模式双图用 `<picture>` 包裹：
    ```html
    <picture>
      <source srcset="dark.png" media="(prefers-color-scheme: dark)">
      <img src="light.png" alt="Architecture">
    </picture>
    ```
- **允许的 HTML 子集**：GitHub sanitize HTML，仅放行 `<details>`、`<summary>`、`<sub>`、`<sup>`、`<br>`、`<kbd>`、`<picture>`、`<source>`、`<img>`、`<a>` 等少数标签。`<script>`、`<style>`、自定义 `class`/`id`/`onclick` 等属性会被剥离
- **HTML 块内不解析 Markdown**：`<details>` 内的 markdown 需在 `<summary>` 之后**空一行**才会解析
- **Badge**：用 [shields.io](https://shields.io) 标准格式；紧邻 H1 下方同行排列；推荐顺序 — CI status / coverage / version / license
- **不要使用 emoji**：项目政策禁止 emoji（除非用户明确请求）。即便用，shortcode（`:rocket:`）的 GitHub 兼容性也优于直接 Unicode emoji
- **trailing whitespace**：行末空格在 Markdown 中是 hard line break，**不要留**；编辑器开 "trim trailing whitespace on save"

### 中英文混排规范

参考 [中文文案排版指北](https://github.com/sparanoid/chinese-copywriting-guidelines)：

- **中英文之间加半角空格**：`使用 GMP 库` ✅，`使用GMP库` ❌
- **中文与阿拉伯数字之间加空格**：`支持 64 位整数` ✅，`支持64位整数` ❌
- **数字与单位之间不加空格**（SI 惯例）：`100MB`、`5%`、`10ms` ✅；但 `10 个测试` 要加空格（数字 + 量词）
- **半角与全角标点**：
  - 中文体：用全角 `，。；：？！「」（）`
  - 英文体：用半角 `,.;:?!"'()`
  - **数学符号 / 代码内 / inline code 内**：半角（如 `x = 3, y = 5`）
- **不混用简繁体**：全文保持简体中文（除非引用繁体原文献）
- **常见错别字**：「的 / 地 / 得」、「再 / 在」、「做 / 作」、「他 / 她 / 它」需区分
- **数学公式**：行内用 `$...$`（GitHub 已支持 MathJax），块级用 `$$...$$`；纯字母变量用斜体（`$f(x)$`），运算符不要斜体

### 正规英语写作规范

撰写 README 英文段落、API 文档、英文 commit body、英文注释时遵循：

- **避免破折号（em-dash）插入语，优先用从句或括号**
  - ❌ `GNFS — the most powerful classical factoring algorithm — is the standard for large numbers.`
  - ✅ `GNFS, which is the most powerful classical factoring algorithm, is the standard for large numbers.`
  - ✅ `GNFS (the most powerful classical factoring algorithm) is the standard for large numbers.`
- **复合形容词的连字符（hyphen）规则**
  - 修饰名词时连字符：`high-performance code`、`64-bit integer`、`memory-mapped file`
  - 作谓语时不连字符：`the code is high performance`、`the integer is 64 bits wide`
- **Oxford comma 必须保留**：`A, B, and C`（清晰区分 list 最后两项与并列结构）
- **缩写首次使用展开**：`General Number Field Sieve (GNFS)` 首次出现展开；后续可直接 `GNFS`
- **拉丁缩写后加逗号**：`e.g.,`、`i.e.,`、`etc.,`；句首避免用，改为 `For example,` / `That is,`
- **被动语态优先改主动**：
  - ❌ `The matrix was solved by Block Lanczos.`
  - ✅ `Block Lanczos solves the matrix.`
- **数字拼写规则**：0-9 拼写（`zero`、`nine`）；10+ 用阿拉伯数字（`10`、`1000`）；**句首数字必须拼写**（`Twelve tests pass` ✅，`12 tests pass` ❌ 作句首）
- **避免缩约形式（contraction）**：正式文档用 `do not` 不用 `don't`，`cannot` 不用 `can't`，`it is` 不用 `it's`
- **避免主观强化词**：`very fast`、`really good`、`quite efficient` → 量化（`5× faster than baseline`、`under 5ms`）
- **句号后单空格**（现代标准；早期打字机时代的双空格规则已废除）
- **标题用 Title Case**：`## Build System`、`## Performance-Critical Code`（主要词首字母大写，介词 / 冠词 / 并列连词除外，但句首词永远大写）
- **避免冗余**：
  - ❌ `in order to` → ✅ `to`
  - ❌ `due to the fact that` → ✅ `because`
  - ❌ `at this point in time` → ✅ `now`

### 正规中文写作规范

中文 README 段落、设计文档、长 commit body 遵循：

- **统一全角标点**：`，。；：？！「」（）——……`
- **省略号**：用 `……`（两个 `…`，共 6 个点），不用 `...` 或 `。。。`
- **破折号**：表示插入语或转折时用 `——`（两个全角 em-dash），不用 `--` 或 `-`
- **引号嵌套**：外层 `「」`，内层 `『』`；或外层 `""`，内层 `''`；全文一致，不混用
- **数字格式**：
  - 阿拉伯数字表示精确量：`64 位`、`300 个测试`、`5 月 18 日`
  - 汉字数字表示约数或习语：`几十种`、`三五次`、`两三天`
- **专有名词不翻译**：GitHub、CMake、GMP、NTL、Linux、macOS、LLVM 保持英文
- **避免直译腔**：
  - ❌ `这是一个高性能的实现` → ✅ `实现高性能` 或 `性能优异`
  - ❌ `让我们考虑这个问题` → ✅ `考虑这个问题`
- **避免冗余动词组**：
  - ❌ `进行编译` → ✅ `编译`
  - ❌ `做出修改` → ✅ `修改`
  - ❌ `给予支持` → ✅ `支持`
- **避免「做」滥用**：用具体动词替代（实现 / 完成 / 执行 / 编写 / 调用 / 部署）
- **句长控制**：单句不超过 50 字；超过则拆分或加分号 / 句号
- **避免「的」字句堆叠**：`一个高性能的多线程的并行的实现` ❌ → `高性能多线程并行实现` ✅

### README 撰写检查清单

提交 README/docs 改动前确认：

- [ ] **本地渲染验证**：用 `grip` 或 VSCode "Markdown Preview Enhanced" 检查 GFM 渲染效果（裸 CommonMark preview 不等同 GitHub）
- [ ] **所有内部链接有效**：文件相对路径 / 锚点链接 / 图片路径都可点开
- [ ] **所有外部链接 HTTP 200**：第三方库主页 / 论文 DOI / 在线 demo 用 `lychee` 批量校验
- [ ] **代码块带正确语言标识**：每个 fence 都标 `cpp` / `bash` / `cmake` / `text` 等
- [ ] **表格在窄屏可读**：列数 ≤ 6；列宽不要让单元格挤到换行
- [ ] **图片有 alt 文本**：`![desc](path)` 而非裸 `![](path)`
- [ ] **HTML 嵌入实测**：`<details>` / `<picture>` 等需在 GitHub 实际渲染验证（本地 preview 可能假阳性）
- [ ] **无 trailing whitespace**：grep 检查 `grep -nE " +$" README.md` 应为空
- [ ] **中英文混排空格一致**：用 `autocorrect` 自动修正
- [ ] **拼写检查**：英文用 `aspell` 或 VSCode "Code Spell Checker"
- [ ] **目录（TOC）同步**：章节增删后头部 TOC 一并更新
- [ ] **commit 前 diff 复审**：`git diff README.md` 重读一遍，避免误删 / 误改

### 推荐工具链

| 工具 | 用途 | 安装 |
|------|------|------|
| `autocorrect` | 中英文混排空格自动修正 | `cargo install autocorrect-cli` |
| `markdownlint-cli` | Markdown 语法静态检查 | `npm i -g markdownlint-cli` |
| `grip` | 本地 GitHub-flavored Markdown 渲染预览 | `pip install grip` |
| `lychee` | 批量链接有效性检查 | `cargo install lychee` |
| `aspell` / `hunspell` | 英文拼写检查 | `brew install aspell` |
| `prettier` | Markdown 格式统一（缩进、列表对齐） | `npm i -g prettier` |

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

已配置，排除：
- 构建产物：`build/`, `build-*/`, `xcode-build/`, `cmake-build-*/`, `*.o`, `*.a`, `*.dylib`, `*.dSYM/`
- IDE：`.vscode/`, `.idea/`, `*.xcodeproj/`, `.clangd/`, `compile_commands.json`
- OS：`.DS_Store`, `Thumbs.db`
- 会话级持久化文件（**不入 git**）：`task_plan.md`, `task_plan_*.md`, `findings.md`, `progress.md`, `BACKLOG.md`, `RESOLVED.md`
- 根目录代码污染防护：`/*.cpp`, `/*.hpp`, `/*.sh`（代码必须放 `include/` `src/` `tests/`，脚本放 `scripts/`）
- 遗留废弃文档：`/BUILD.md`, `/QUICKSTART.md`, `/PROGRESS_UPDATE.md` 等多个 root *.md
- 性能采集：`bench/results/*.trace`, `bench/results/*.pmu.json`, `*.profraw`, `*.profdata`, `pgo-profiles/`
- Claude 运行时：`.claude/scheduled_tasks.lock`

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
  + **缓解 (2026-05-18)**: ENV `GNFS_BW_KRYLOV_MMAP=1` mmap A_seq 到磁盘,
    matrix BM 节省 ~16 MB, scalar BM 节省 ~128 MB (60d n=1M), 总 ~144 MB Phase 5 RAM
- OOC 基础设施已部分集成到主 pipeline:
  + RelationCollector OOC: ENV `GNFS_OOC_RELATIONS=1` / sieve checkpoint
    `GNFS_SIEVE_RESUME=<base_path>` (2026-05-18)
  + BW Krylov mmap: ENV `GNFS_BW_KRYLOV_MMAP=1` (2026-05-18)
  + BW Krylov multi-stream: ENV `GNFS_BW_KRYLOV_STREAMS=K` (2026-05-21,
    K range [1,16], default 1). 主 ROI 是 retry latency 减少, wall-time 因
    Phase 2 BM 单线程 dominant 而 K>1 与 K=1 相当.
  + MmapCSRMatrix Phase 5 集成尚未实施 (需 SpMV API generic 化, multi-day surgery)
- NEON SIMD sieve baseline 已实施 (2026-05-18): `detail::apply_log_p_range`
  helper 在 Phase 0 global + v-prime row 用 NEON 8-lane. bucket scatter + tiny stride
  保持 scalar (doctrine "sieve 内核 NEON 收益有限" — gather/scatter 不适合 SIMD).
- SIMD SpMV inner kernels 已实施 (2026-05-21, commits `2fe4aee` → `b321e62`):
  ENV `GNFS_SPMV_SIMD=auto|0|1` (default auto). NEON 2-lane (ARM64) +
  AVX2 4-lane (x86_64) 在 Block Lanczos / Block Wiedemann SpMV tail 走宽
  XOR. Prefetch phase 保持 scalar (per-element prefetch hint 是 latency
  隐藏主要手段). 输出 bit-for-bit 与 scalar 一致. 不依赖外部库, 纯 header.
- Murphy E `compute_alpha` 已 ThreadPool 并行化 (2026-05-18):
  ENV `GNFS_MURPHY_ALPHA_THREADS=N` opt-out (默认 hardware concurrency).
  Rotation-incremental 算法重构 deferred (multi-day pure math).
- E-core QoS 分离 (2026-05-18, commits `e47ab08` + `a958fc9`): `include/gnfs/sieve/ecore_qos.hpp`
  helper, 4 个 sieve thread spawn site 已 wire-in, M5 P/E-core 调度差异已 mitigate.
- Relation collector memory pool 已实施 (2026-05-21, W6 T4):
  ENV `GNFS_RELATION_POOL_SIZE=N` opt-in (default 0 / OFF). Switches
  RelationCollector in-memory `relations_` to `std::pmr::vector<Relation>`
  backed by `RelationPoolResource` (monotonic_buffer_resource, initial chunk
  N bytes). 减少 sieve 期间反复 malloc + fragmentation. Bit-for-bit identical
  output guaranteed by `tests/test_relation_pool_integration.cpp`.
  Mutually exclusive with OOC (pool disabled when `ooc_enabled=true`).

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
3. **不等待、不暂停、不询问** — 直接开始下一大阶段
4. **不要自己 `/compact`** — 系统会自动压缩上下文（详见 §8）

```
大阶段 1 开始
  ↓
步骤 1.1：实施 → commit（自动）
步骤 1.2：实施 → commit（自动）
步骤 1.3：实施 → commit（自动）
  ↓
大阶段 1 完成：编译验证 + 更新 progress.md
  ↓
直接开始大阶段 2（不等用户确认、不自己 /compact）
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
全部完成 → 最终验证 + E2E 回归 → 总结报告
```

**不要自己 `/compact`**：系统自动管理上下文压缩，主线程专注完成任务（详见 §8）。

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
