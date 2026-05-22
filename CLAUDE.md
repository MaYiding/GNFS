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
