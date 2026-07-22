# 关系处理 (relation) 模块 ENV 调优开关

> 本文档收录 `relation` 模块所有 `GNFS_*` 运行时调优开关 / helper 的详细设计(算法、ENV 解析、bit-for-bit 保证、ROI、集成点、测试)。
>
> 返回: [ENV 开关总览](README.md) · [CLAUDE.md ENV 索引表](../../CLAUDE.md#env-调优开关总览)

---

## Filter Merge — V0 + V3 Clique Cascade

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

---

## Structured relation filter policy (GNFS_STRUCTURED_FILTER)

`GNFS_STRUCTURED_FILTER` 是结构化关系消元的 opt-in 策略开关。M4 提供严格解析、
预消费策略决策和共享 `RelationReductionEngine` 的单次 structured dispatch，并已
接入 adaptive sieve、distributed 前置决策和公开 `Pipeline::filter()` 入口。当前
调用方固定传入 `auto_eligible=false`；在获得独立尺寸实证前，不得复用 V0 BFS 或
OOC 的 `lp_bits` 阈值冒充结构化策略的 auto 证据。

受支持的实验边界包括两类启用 LP 的输入：内存 vector route，以及显式普通 OOC
route。后者要求同时设置 `GNFS_STRUCTURED_FILTER=1` 和按现有 `atoi` 兼容语义解析为
1 的 `GNFS_OOC_RELATIONS`，且不能启用 sieve resume 或 distributed route。尺寸阈值
自动打开的 OOC 不算显式授权；forced structured 会在运行前置判定中拒绝，`auto`
继续使用调用方具名的 legacy 策略。resume corpus 和 distributed worker stores 仍未
接入 structured reducer。

普通 OOC adaptive 生产 route 要求 collector 以 `check_duplicates=true` 运行。
`with_unique_ooc_prefix()` 在 collector 锁内证明
`seen_.size() == writer.count() == stats.total_relations` 后，签发私有构造的
`CollectorUniqueOOCPrefixSource` 强类型 capability。engine 的直达入口只接受该具体
类型；任意 `RelationSource` 自行声明 marker 不能替代 collector 的唯一性证明。

collector 暂停 append，并把已提交 raw prefix 暴露为 callback-scoped、只读的
indexed source。engine 在该 callback 内完成全量 raw relation 校验、固定 V1 corpus
digest、LP histogram、incidence 构建、并行归约和 transactional OOC output
发布；所有 reducer worker 与 future 必须在 callback 返回前收束。该路径不再做
`ABPair` 去重，也不创建 per-generation working corpus。callback 结束后，collector
先销毁 reader、解除 mmap，再用精确 descriptor 恢复 raw append。source
integrity/I/O 失败会使 raw fail closed；配置、归约、output 或 `std::bad_alloc`
失败会先恢复 raw 再传播，resume 失败优先。

强类型 capability 的 count 证明还会绑定到实际 payload：初扫使用临时 exact
`ABPair` set，逐行核对 raw AB 属于 collector 的完整 `seen_` 集合，并为每个 ordinal
保存 128-bit V1 relation fingerprint。reducer 的每次并发重读都校验该 fingerprint；
output finalize 后，engine 在同一 suspended descriptor 上打开第二个 fresh reader/new
mapping，再完整复核一次。这样不会依赖旧 `MAP_PRIVATE` view 是否观察到底层同尺寸
改写。集合、语义或 fingerprint 漂移会删除未发布 output 并把 raw writer 标记为
Failed。fingerprint 是进程内重放一致性校验，不是密码学认证或持久格式。
最终成功 probe 必须与 terminal raw 的 format、store identity、count 和 physical extent
一致，Pipeline 才会在所有用户 callback 成功后转交并删除 raw owner。

这条路径不是 native incremental reduction：第 `r` 轮仍完整读取截至该轮的
raw prefix，因此累计解码与 I/O 仍为 `O(rounds * relations)`。raw writer 在整轮
校验、归约和 output 发布期间都处于 `Suspended`，所以 relation collection 不能与
该轮归约重叠。生产直达 route 的每代 relation-payload 磁盘峰值为 `raw + output`
两份；这只是 payload 拓扑，不是 RSS 结论。
collector `ABPair` set、每行 16-byte fingerprint、LP histogram、incidence、logical
rows 与 history 仍是 corpus-scale metadata。初扫的第二个 exact AB set 和 LP weight
map 会在 incidence 构建前释放，避免与 reducer metadata 叠加；这仍不构成 RSS 上界。
跨平台 process RSS helper 和 5K/50K/200K 独立进程测量入口已经接入，但其
lifetime peak 是进程高水位，不是 route 的净分配量。真实 50 位入口使用硬
special-Q 上限，并明确标为 bounded prefix probe；它不等价于完整首轮或完整分解。
当前仍无足以支持自动选路的真实尺寸收益证据，所以该能力继续保持 forced
experimental route，不构成 auto 或默认启用依据。测量边界见
[Structured OOC Measurement](../perf/structured-ooc-measurement.md)。

完整 `Pipeline::run()` 在任何 progress/log callback、试探算法、checkpoint 读写和
relation generation 分配前捕获不创建 artifact 的 route snapshot；callback 后续修改
ENV 不会改变本次运行的 resume/OOC/distributed 路由。直接 `sieve_and_collect()` 还会
在 fresh raw pair 原子占有成功后才发出首个 sieve callback，所以同路径冲突不截断
sentinel，也不消费 relation generation。完整 `run()` 的显式 raw 路径冲突在进入
sieve 时 fail closed；此前已完成的 polynomial/factor-base callback 不属于该
fresh-pair 边界。

| ENV 值 | 受支持 | `auto_eligible` | 决策 |
|---|---:|---:|---|
| unset / `0` | 任意 | 任意 | 使用调用方具名 legacy 策略 |
| `1` | 是 | 任意 | 强制选择 structured |
| `1` | 否 | 任意 | 消费 raw snapshot 前抛出 `std::invalid_argument` |
| `auto` | 是 | 是 | 选择 structured |
| `auto` | 否 | 任意 | 使用调用方具名 legacy 策略并记录 unsupported 原因 |
| `auto` | 是 | 否 | 使用调用方具名 legacy 策略并记录 ineligible 原因 |
| 其他值 | 任意 | 任意 | 消费 raw snapshot 前抛出 `std::invalid_argument` |

解析严格且区分 unset 与显式空串。仅 `nullptr`、`0`、`1`、`auto` 合法；
空串、大小写变体、`on` / `off`、`true` / `false`、前后空白和其它数字均为
配置错误。策略 helper 接收调用方传入的 ENV 原始值，不调用 `getenv`，也不使用
`std::once_flag` 或其它进程级静态缓存。生产入口在消费 vector raw snapshot 或借读
OOC prefix 前解析一次，再把不可变决策传给 reducer；测试无需 reset cache。

**Fallback contract**：fallback 只允许发生在 structured 启动前。`auto` 对不受
支持或未 eligible 的输入返回 legacy 选择与稳定原因；具体 V0/V3 策略仍由调用方
命名。`GNFS_STRUCTURED_FILTER=1` 不允许静默 fallback。structured 一旦启动，
内部 invariant 错误必须向上传播；`NoCandidates` 是成功的未变 structured 结果，
不是 fallback。

**Bit-for-bit contract**：unset / `0` 仅返回 legacy 选择，不消费或修改 corpus，
因此后续 legacy 输出必须与引入该策略层前逐位一致。structured 与 legacy 之间
只要求依赖空间等价，不承诺关系行逐位相同。

**M4 experimental caps**：forced ON 使用固定、保守的研究 profile，不接受额外
ENV 数字，也不使用伪无限上限。一次 snapshot 的 candidate examinations/pass 与
累计 emitted rows 均不超过 4096，commits 不超过 1024，正 LP fill growth 为 0，
单次 accepted payload 不超过 8192 entries，单输出 source/materialized pairs 不超过
64，单侧 factors 不超过 4096，batch width 不超过 4，incidence shard 不超过
4096 rows，worker 数为 `min(hardware_concurrency, 4)`（零报告回退 1）。小 corpus
的 row-dependent cap 进一步收紧到实际行数。达到 cap 返回显式 `BudgetLimit` 和
完整 active basis，不 fallback legacy。相同 profile 可消费 vector 或 finalized OOC
corpus；上述重复 prefix 扫描、corpus-scale metadata 和未测 RSS 仍阻止 auto promotion。
profile factory 同时拒绝 worker=0 或 worker>4，因此 4-worker ceiling 是 API
invariant，不只是 production caller 的约定。

每次 structured generation 发出一条 `schema=1` relation-graph
`structured_filter` 记录。记录包含 policy reason、generation、route、source/output
backend、输入/输出 rows 与 LP columns、raw/output digest、raw duplicates、输入 LP
weight histogram、commits、emitted rows、LP fill、planning/candidate/rejection
counters、所有主 cap、batch、workers、stop reason、reduction-engine wall time，以及
归约前后的 process current/lifetime-peak RSS。RSS 单位固定为 bytes，scope 为
`self_lifetime`；unsupported 字段使用显式 support bit 和数值 0。peak growth 只表示
归约测量窗口内进程高水位的增量，不能解释成净分配量。

进入 `solve_matrix()` 后另发 `structured_filter_matrix`。该记录保留兼容字段
`excess=max(rows-cols, 0)`，并新增有符号 `row_column_delta=rows-cols`、累计
MatrixBuilder wall time 和 nonzeros。记录在 deterministic trim 完成后、SGE 前恰好
发出一次，并与最终 `MatrixResult.matrix` handoff 对齐。thin matrix 因此不会再把
负缺口误报为 0；归约阶段也不会伪造尚未构建的矩阵指标。

**集成点**：

- `include/gnfs/relation/structured_filter_policy.hpp`：严格 parser 与独立
  `selection` / `reason` 决策，不依赖 `ReductionStrategy`。
- `include/gnfs/relation/reduction_engine.hpp`：vector、finalized-OOC 和 generic borrowed
  route 在 raw 校验与完整 `ABPair` 去重后选择 structured reducer；生产普通 OOC
  直达 route 改为消费 collector-proven unique source，不重复去重。结构化执行不再
  运行 V0/V3，错误也不 fallback。
- `include/gnfs/relation/structured_filter_profile.hpp`：M4 forced-on profile 与
  production worker 选择。
- `include/gnfs/relation/relation_source.hpp` / `relation_sink.hpp`：共享 indexed
  source 契约与显式 finalize/abort output transaction。OOC sink pair 位于
  `<base>.gnfs-sink-lease/corpus.{relidx,reldata}`；`RemoveArtifacts` 把 pair 与
  私有目录的清理责任一并交给最终 corpus owner，`Preserve` 保留两者。
- `RelationReductionConfig::StructuredExecutionConfig::deduplicated_ooc_base_path`：
  generic finalized-OOC 输入以及 generic `prepare_borrowed_structured()` route 的必填 working
  base。它必须与 output base 不同；engine 使用 `RemoveArtifacts` 管理 working
  corpus。working/output lease root 互不允许相等或形成祖先关系，也不得与
  raw corpus 的独占 cleanup root 重叠。sink/corpus 构造时会冻结规范化绝对路径，
  避免后续工作目录变化重定向清理。生产普通 OOC 直达 route 拒绝非空 working
  path；Pipeline 只从一次冻结的 run namespace 为每个 logical generation 派生
  output sibling base。该 API 字段不是 ENV。
- `include/gnfs/relation/collector.hpp`：appendable OOC prefix 的 callback-scoped borrowed
  source、collector-proven unique 强类型 capability、authoritative source descriptor、兼容性
  corpus snapshot 和 terminal one-shot handoff。fresh raw pair 使用 exclusive create，不覆盖
  既有 artifact。
- `src/api/pipeline.cpp`：adaptive/final probe 与公开 filter 的统一 overlay；仅显式普通
  OOC 接入 structured，size-aware OOC、resume 和 distributed 保持 legacy/unsupported。
- `tests/test_structured_filter_policy.cpp`：合法与非法 token、OFF、forced ON、
  unsupported 和显式 auto eligibility 的表驱动边界。
- `tests/test_relation_reduction_engine.cpp`：structured config 预检、NoCandidates、
  singleton 所有权、generic borrowed prepare/finish 边界、强类型 unique prefix 直达归约、
  AB proof/payload drift、output-finalize 后 raw-resume 失败清理、去重顺序、1/2/4
  线程等价和 invariant fail-closed。
- `CMakeLists.txt` / `scripts/test.sh`：注册 relation 模块的 instant 测试。
- `tests/test_api.cpp`：公开 route 的 forced ON、unset/0/auto legacy 等价、非法值
  callback/generation 边界、无 LP fail-closed、resume/distributed sentinel、显式 OOC
  path drift/collision、finalized output 生命周期、terminal callback 失败保留 raw、每代
  exact-once、最终 matrix record 对齐，以及真实 adaptive sieve structured 路径。

---

## lp_bits 实验 (GNFS_OVERRIDE_LP_BITS)

**ENV `GNFS_OVERRIDE_LP_BITS=N`** (commit `dce0a5e`, `e271c5a`): runtime override `params.hpp` digit-based lp_bits default. 范围 1-30. 不在范围则忽略 (default).

```bash
GNFS_OVERRIDE_LP_BITS=25 ./test_stress 2 2   # 60d with lp_bits=25 (vs default 26)
GNFS_OVERRIDE_LP_BITS=27 ./gnfs <N>          # any size with lp_bits=27
```

**用途**:
- BACKLOG [OPT] 60d lp_bits 25 vs 26 trade-off 比较
- LP space 影响 sieve duration: smaller lp_bits = smaller LP space = fewer LP cols = less raw needed for PASS (但 fewer LP cofactor candidates)
- 实验前后必须 reg-test 25d / 50d (lp_bits 不该影响 < 50d behavior, 默认 path unchanged)

---

## V0 weight-3 merge (GNFS_V0_WEIGHT3)

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

---

## Drop-residual + weight-cutoff (50d β plateau 实验通道)

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

---

## V0 BFS chain merge (GNFS_V0_BFS)

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

---

## OOC Relation Store (GNFS_OOC_RELATIONS)

**ENV `GNFS_OOC_RELATIONS=1`** (2026-05-18 实施):
启用 RelationCollector OOC 流式持久化, sieve 期间 relations 流式写盘
`<system-temp>/gnfs_relations_<run-id>.{reldata,relidx}` 而非 in-memory vector。内存只保留
`(a,b)` seen set + stats。legacy filter 在 probe 边界 materialize vector；同时显式
强制 structured 时走上一节描述的 collector-proven unique raw-prefix/output 直达 route。ENV unset
时 `lp_bits >= 22` 使用 size-aware default，显式 `0` 始终关闭，显式 `1` 绕过尺寸门槛。

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
- fault tolerance: fresh writer 在 V3 index/data header 中写入同一个 durable
  store ID；普通 reader 严格拒绝 incomplete store。只有与 `SieveCheckpoint` V2
  wire format 配对的 OOC V3 descriptor 可以验证并恢复 committed prefix，同时
  截断 crash 后未提交 tail。
- fresh writer 用 exclusive create 原子占有 `.relidx` 和 `.reldata`；任一既有 regular
  file、directory 或 dangling symlink 都会 fail closed，不允许 fresh route 截断旧 pair。
- V3 index 使用固定布局
  `[magic][format_version][store_id][count][offsets...]`，data 使用
  `[data_magic][format_version][store_id][records...]`。offset 与 descriptor
  `data_end` 都是物理文件偏移；首 offset 及空库 EOF 均为 byte 24。finalize
  只在最后切换 index magic，不会覆盖任一 store ID。
- 普通 reader 保留 finalized V1/V2 只读兼容；V1/V2 不能参与 append recovery
  或 `RelationCorpus` ownership promotion。
- `checkpoint_prefix()` 与 finalize 会同步 data/index；POSIX 还同步父目录。
  finalize metadata 已落盘但 final magic 未切换时，paired descriptor 仍可恢复。
- final magic 已 flush 且文件句柄已关闭后，writer 状态固定为 `Finalized`。
  后续目录同步或 observer hook 抛错不会把可读 pair 重新标成 `Failed`；若最终
  durability barrier 尚未完成，重复 finalize 会重试同步。

**集成点** (commits `3b843fc` → `d39b637`, 2026-05-18):
- `include/gnfs/relation/collector.hpp` — CollectorConfig + add/get/clear/merge OOC dual mode
- `include/gnfs/relation/ooc_relation_format.hpp` — V1/V2/V3 稳定布局常量
- `include/gnfs/relation/ooc_relation_store.hpp` — OOCRelationWriter/Reader, MAGIC/INCOMPLETE flip
- `include/gnfs/relation/ooc_policy.hpp` — 三态 ENV 解析 (off / auto-by-size / on)
- `src/api/pipeline.cpp` — `sieve_and_collect` ENV-gate、per-run path namespace 和 structured
  ordinary-OOC bridge
- `tests/test_relation_collector.cpp` — OOC lifecycle、snapshot/handoff、unique-prefix capability、
  collision、recovery 与 terminal-state tests
- `tests/test_ooc_relations.cpp` / `tests/test_ooc_policy.cpp` — OOC store + policy
- `tests/test_gnfs_e2e.cpp` — real GNFS pipeline OOC stress path

**API 语义**:
- `add()`: OOC 模式跳过 relations_.push_back, 走 OOCWriter::write
- `snapshot_relations()`: 暂停 writer、读取受信 prefix、解除映射并重新打开 append；
  后续 `add()` 仍有效
- `with_ooc_prefix()`: 在线性化 callback 内借读 committed prefix；observer 可重入，
  owner/mutation API 在借读期确定性拒绝。callback 不得让 source/view/worker 逃逸；
  reader 解除映射并精确 resume 后才返回 move-only 结果
- `with_unique_ooc_prefix()`: 仅在 `check_duplicates=true` 且 seen/writer/stats 计数完全一致
  时签发私有构造的 `CollectorUniqueOOCPrefixSource`；其 callback 生命期、source 失败分类和
  exact-resume 顺序与 `with_ooc_prefix()` 相同
- `snapshot_ooc_corpus()`: 不构建全量 relation vector，逐行复制 committed prefix 到独立
  finalized corpus，同时返回 authoritative raw source descriptor；保留为显式复制 API，
  structured adaptive 生产路径不再使用它
- `handoff_ooc_corpus()`: terminal one-shot ownership transfer；成功后 mutation/materialize/
  repeated finalize 均拒绝，`size()` / `stats()` 仍可读
- `finalize_relations()` / `get_relations()`: finalize 后 read_all；此后禁止 append
- `checkpoint_ooc()` / `resume_ooc()`: 为 sieve transaction 暴露 descriptor 配对边界
- `size()/empty()`: 基于 writer->count() (准确反映写盘 relation 数)
- `clear()`: finalize 并 descriptor-bound 验证 exact pair 后删除，再 exclusive-create 新 writer；
  Failed 或 handed-off collector 拒绝 destructive recycle
- `save/load`: legacy 序列化协议 OOC 模式 disabled (return false); 直接用 OOCRelationReader
- `merge`: OOC source 显式抛错 (read overhead 不实用); OOC sink 工作

---

## Relation collector memory pool (GNFS_RELATION_POOL_SIZE)

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

---

## Phase 0 radix-sort dedup-sort (GNFS_FILTER_RADIX_SORT)

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

---

## Filter Phase 0 LP key Bloom pre-screen (GNFS_FILTER_LP_BLOOM_BITS)

**ENV `GNFS_FILTER_LP_BLOOM_BITS=N`** (2026-05-22 实施, W9 T5, range [10, 28], default 0):
为 `uint64_t` key 流提供去重计数的可选 Bloom filter pre-screen helper。
该 helper 最初面向 50d+/60d Round 2 adaptive sieve loop 中的
`filter.hpp::count_unique_lp_keys(relations)` hot path。

**当前状态**：生产主路径已改用 full-width structural
`LargePrimeKey{prime, root, is_algebraic}`，并以
`std::unordered_set<LargePrimeKey, LargePrimeKeyHash>` 精确计数。当前
`count_unique_lp_keys` 不调用这个仅接收 `uint64_t` 的 standalone helper，
因此 `GNFS_FILTER_LP_BLOOM_BITS` 对生产主路径无效。不得把结构键有损压缩为
单个 `uint64_t` 来接线；未来集成需要让 Bloom 层直接支持完整结构键，同时
保留结构键集合的精确相等性确认。

```bash
GNFS_FILTER_LP_BLOOM_BITS=0  ./gnfs <N>   # helper default；当前主路径无变化
GNFS_FILTER_LP_BLOOM_BITS=14 ./gnfs <N>   # helper 配置 16 KiB；当前主路径无变化
GNFS_FILTER_LP_BLOOM_BITS=18 ./gnfs <N>   # helper 配置 256 KiB；当前主路径无变化
GNFS_FILTER_LP_BLOOM_BITS=22 ./gnfs <N>   # helper 配置 4 MiB；当前主路径无变化
GNFS_FILTER_LP_BLOOM_BITS=24 ./gnfs <N>   # helper 配置 16 MiB；当前主路径无变化
GNFS_FILTER_LP_BLOOM_BITS=28 ./gnfs <N>   # helper 配置 256 MiB；当前主路径无变化
unset GNFS_FILTER_LP_BLOOM_BITS           # helper 解析为 default 0
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
- 历史目标是减少 50d+/60d 大 relation 数 (1M+) 时的 hash-set bucket
  probe。Bloom (m=2^22 = 4 MiB) 可容纳在 L3，大部分 query 先执行
  4-hash mask + bit-test。
- 25d/40-bit small N (< 50k relations) 无 ROI, Bloom 构造与 4-hash overhead
  反而增加常数项. 默认 OFF 保证零回归.
- helper 当前 standalone，生产主路径没有调用点，所以上述 ROI 尚未在主路径
  兑现。未来接线必须扩展 helper 以接收完整 `LargePrimeKey`；不能先把
  `prime`、`root` 与 side 有损压成 `uint64_t`。

**集成点** (W9 T5, 2026-05-22):
- `include/gnfs/relation/lp_bloom.hpp` — helper API + ENV gate + k=4 Bloom
  primitive + `count_unique_with_bloom<KeyIt>` template
- `tests/test_lp_bloom.cpp` — 11 个测试 (4 ENV / 3 Bloom 行为 / 1 parity
  100k keys / 3 edge cases). 全部 instant tier
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout
- `include/gnfs/relation/filter.hpp::count_unique_lp_keys` 当前使用 full-width
  structural `LargePrimeKey`，未调用这个 `uint64_t` helper

**Helper default OFF (bits=0)**: ENV unset 时，若 standalone helper 被直接调用，
`filter_lp_bloom_bits() == 0`，`count_unique_with_bloom` 退化为 pure
`std::unordered_set<uint64_t>` baseline。当前生产主路径不调用 helper，故无论
ENV 取值为何，均不会改变 relation filtering 行为。

---

## LP key splitmix64 hash mixing (GNFS_FILTER_LP_HASH_MIX)

**ENV `GNFS_FILTER_LP_HASH_MIX=auto|0|1`** (2026-05-22 实施, W11 T5, default auto):
为 `std::unordered_set<uint64_t>` / `std::unordered_map<uint64_t, ...>` 提供
splitmix64 (Stafford Mix 13) hash 混合。该 standalone helper 最初面向
`(prime_id << 1) | side` 一类 64 位 LP key packing，以改善 clustered input
的 bucket 分布。

**当前状态**：生产主路径已使用 full-width structural
`LargePrimeKey{prime, root, is_algebraic}` 与 `LargePrimeKeyHash`，不再使用
64 位 packed LP identity。`filter.hpp`、`clique_merger.hpp` 与
`count_unique_lp_keys` 均不调用 `LpKeyHash`，因此
`GNFS_FILTER_LP_HASH_MIX` 对生产主路径无效。不得通过截断 `prime` 或 `root`
来把结构键塞入该 helper；如需采用新的混合策略，应直接修改或替换
`LargePrimeKeyHash`，并继续对完整结构键执行相等性比较。

```bash
GNFS_FILTER_LP_HASH_MIX=auto ./gnfs <N>   # helper 默认启用；当前主路径无变化
GNFS_FILTER_LP_HASH_MIX=0    ./gnfs <N>   # helper 禁用；当前主路径无变化
GNFS_FILTER_LP_HASH_MIX=off  ./gnfs <N>   # helper 语义同 0
GNFS_FILTER_LP_HASH_MIX=1    ./gnfs <N>   # helper 启用；当前主路径无变化
GNFS_FILTER_LP_HASH_MIX=on   ./gnfs <N>   # helper 语义同 1
unset GNFS_FILTER_LP_HASH_MIX             # helper 解析为 auto
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
- 历史目标是让 `std::unordered_set<uint64_t, LpKeyHash>` 中的 clustered
  64 位 key 分散到更多 bucket，减小 chain 长。该结论只描述 standalone
  helper，不是当前生产 LP 集合的性能结论。
- 与 W9 T5 `GNFS_FILTER_LP_BLOOM_BITS` 的历史设计互补：Bloom 减少 probe，
  mixer 改善 bucket 分布。两个 helper 目前都没有生产调用点。
- 当前不能把 `std::unordered_set<LargePrimeKey, LargePrimeKeyHash>` 直接替换为
  `std::unordered_set<uint64_t, LpKeyHash>`。这种替换要求有损压缩完整结构键，
  会破坏 LP identity 正确性。
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
  当前使用 full-width structural `LargePrimeKey` 与 `LargePrimeKeyHash`，未调用
  该 `uint64_t` helper。

**Helper default ON (auto)**: `auto` 只控制 standalone `uint64_t` helper。
当前主 pipeline 无调用点，因此 ENV 对运行行为无影响。未来若扩展 hash
策略，应以完整 `LargePrimeKey` 为输入，不能复用有损 packed identity。

---

## Partial relation merger 并行 (GNFS_FILTER_MERGE_THREADS)

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
