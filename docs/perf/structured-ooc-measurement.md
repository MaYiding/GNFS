# Structured OOC Measurement

本文定义 structured ordinary-OOC route 的可复现资源测量、真实 50 位有界探针和
candidate 批次调度扫测。这些入口提供证据，不设置性能通过阈值，也不改变
`GNFS_STRUCTURED_FILTER` 的默认策略。

## Measurement Semantics

`gnfs::util::process_memory_snapshot()` 统一返回当前进程的 current RSS 和 lifetime
peak RSS，单位为 bytes，scope 为当前进程及其全部线程，不含子进程：

- macOS 使用 Mach current resident size 和 `getrusage(RUSAGE_SELF)` peak；
- Linux 使用 `/proc/self/statm` current resident pages 和 `getrusage` peak；
- Windows 使用 Psapi `WorkingSetSize` 和 `PeakWorkingSetSize`；
- 不支持或单项读取失败时返回 `std::nullopt`，不伪造为成功的 0 bytes。

lifetime peak 从进程启动开始累计，不能重置。因此，每个可比较场景必须由新进程
执行。`peak_growth = final_peak - baseline_peak` 只表示测量窗口让进程高水位增长了
多少；它不是当前内存差值，也不是净分配量。

## Synthetic Direct-Route RSS

以下命令默认使用 Release 构建。显式传入 `-t` 时保留调用方选择。测量入口拒绝
`--no-build`，避免把旧构建类型或过期二进制标记为本次资源证据：

```bash
./scripts/test.sh structured-ooc-rss 5000 1
./scripts/test.sh structured-ooc-rss 50000 4
./scripts/test.sh structured-ooc-rss 200000 4
```

每次调用只启动一个 scenario 和一个 structured reduction。允许的 rows 为
`5000|50000|200000`，workers 为 `1|2|4`。测量窗口在 source 建成后开始，覆盖
`reduce_direct_borrowed_structured()`、finalized-OOC result 生命周期和 collector raw
resume。该模式不创建 in-memory oracle，也不回读 output payload。正常无参数的
`test_structured_ooc_scale` 仍是多尺寸 correctness/determinism gate。

成功时输出唯一一行 `GNFS_RESOURCE_V1`，包含 backend、support 状态、rows、workers、
source/output rows、baseline/final current 与 peak、peak growth、wall time 和明确的
source/output backend。缺失的 RSS 值写为 `na`。

## Dense Structured Stage Replay

以下手动入口固定一个 50 位规模的 structured cardinality anchor，并在 fresh Release
进程中执行完整 direct observed route：

```bash
./scripts/test.sh structured-ooc-dense-stage 1
./scripts/test.sh structured-ooc-dense-stage 4
```

fixture 固定为 618,449 rows、576,189 unique LP keys 和 1,236,898 incidence entries。
每行恰好包含 2 个 LP keys。它使用 493,601 个 weight-1 keys；其余 82,588 个 keys
属于 weight-4+ 桶。具体拓扑包含：

- 493,601 个 singleton spokes，连接 54,839 个 degree-9 consumed hubs 和 5 个
  degree-10 consumed hubs；
- 27,744 个 degree-9 core keys，使用 offsets 1 至 4 加一组 perfect matching 的
  circulant，形成 124,848 个 core rows。

singleton peeling 移除全部 spokes。剩余 core 的 active degree 为 9，高于当前
weight-8 planner cap，因此 reducer 以 `no_candidates` 停止。该构造避免测量入口因
1,024 次 bounded commits 重复生成 corpus-scale plans，同时仍覆盖输入扫描、AB set
释放、incidence receipt、reducer 构造、OOC output、reducer 释放和 fresh-view 验证。

`synthetic_cardinality_anchor` 只固定 rows、keys、incidences 和声明的合成拓扑。它不复现
完整 50 位语料的 LP-weight distribution、连通分量、merge yield 或 matrix 质量，不能与
真实 50 位 route comparison 互换。

成功运行恰好输出一条 closed
`GNFS_STRUCTURED_OOC_DENSE_STAGE_V1 schema=1` 记录。记录包含固定 fixture identity、
输入与输出 digests、冻结的关键 reduction/incidence 统计、OOC 生命周期、四类 source-read
计数，以及十个 checkpoint 的 elapsed wall time 与 process RSS 样本。关键合同要求
`incidence_build` source reads 为 0；initial scan 和 fresh validation 必须分别读取全部
618,449 rows。`process_rss_scope=self_lifetime` 明确 peak 包含 fixture 构建期和先前进程
阶段；`output_lease_removed` 与 `source_pair_removed` 不声称永久 cleanup lock 已删除。
wall time 与 RSS 仍只作观测，不设置性能阈值。

### M6d-A Sealed Receipt Adoption Evidence

M6d-A 比较 main baseline commit `610742b` 与 production candidate commit `1559e33`。
记 A 为 baseline，B 为 candidate。每个 worker lane 都在 fresh Release 进程中执行
`ABBA BAAB ABBA BAAB AB`，得到每个 revision、每个 worker 9 个样本。worker 1 与
worker 4 的运行顺序交错，共执行 36 个进程。每组配对的 closed record 中，所有
非观测字段都逐字段相等。

主内存指标定义为
`Pg = cp_reducer_constructed_peak_rss - cp_incidence_receipt_built_peak_rss`。每个
worker lane 的
通过门槛要求 `Pg` 至少减少 24MiB，且相对 baseline 至少减少 15%。结果如下：

| Workers | Baseline Pg | Candidate Pg | Saved bytes | Saved MiB | Reduction |
|---:|---:|---:|---:|---:|---:|
| 1 | 140,984,320 | 113,311,744 | 27,672,576 | 26.391 | 19.6281% |
| 4 | 141,410,304 | 113,311,744 | 28,098,560 | 26.797 | 19.8702% |

时间回归门槛要求中位数同时恶化超过 5% 和 2 个 median absolute deviations (MAD)。
完整 wall time 与 reducer constructor time 的中位数均改善，没有触发该门槛：

| Workers | Baseline wall | Candidate wall | Baseline constructor | Candidate constructor |
|---:|---:|---:|---:|---:|
| 1 | 832,424,500ns | 822,350,791ns | 45,452,458ns | 42,965,208ns |
| 4 | 833,643,750ns | 821,835,792ns | 45,912,750ns | 43,079,791ns |

容器布局分析估算，receipt 到 reducer 构造边界原先有约 40.531MiB 的 LP64/LLP64
瞬时外壳重叠。该估算解释被移除的 row 与 bucket 外壳，不承诺 allocator 会稳定归还
页面，也不保证 `current RSS` 出现相同差值。`current RSS` 节省只作辅助观测，不是
硬门槛。

验证覆盖 relation module 35/35、TSan relation 16/16、Release gate 188/188、120-bit
relation path 1/1、bounded 50-digit route comparison 2/2、dense replay workers 1 和 4，
以及上述 sealed ABBA 对照。dense fixture 仍只固定 cardinality 与声明的合成拓扑；
这些资源结果不能替代真实 50 位 LP 分布、merge yield、矩阵质量或完整首轮证据。

### M6d-B Borrowed Rank Basis Evidence

M6d-B 比较合并后的 baseline commit
`bcf2cf49f772f2cf4ec91375a03854993272486f` 与 production candidate commit
`1985a18e794d744a06800b630ccbc8bc928c461f`。两者的 source tree 分别与测量时 commit
`87706844678f9935f50428734de16615ce7a5882` 和
`29ad00d3b35cc68489a80ed30a8935a119a0441f` 逐位相同。对应 Release 二进制的
SHA-256 分别为
`41c67ca93186dac96e24333649bf25c165f9cb3fea5e9e27b7a35a81880a5f71` 和
`128d71e90a275c49f398bc4843a2f42282fa3981df16fc644685805ceb20e198`。记 A 为
baseline，B 为 candidate。每个 worker lane 都在 fresh process 中执行
`ABBA BAAB ABBA BAAB AB`，得到每个 revision、每个 worker 9 个样本。worker 1 与
worker 4 的运行顺序交错，共执行 36 个 Release 进程。

每个 lane 的 80-field 非观测 identity 都逐字段相等。worker 1 的 identity SHA-256 为
`569b41a5b53203a7f02fa8c644db46547649f3ce18c783683b6e98f28990dc1b`，worker 4 为
`e8ae0eb68c3d5d18030fe6ee63e669b3ba8dc08250a6227483f857fb854518af`。再排除
`workers` 与 `peak_incidence_workers` 后，跨 worker 的 78-field identity SHA-256 为
`2ce17e7f15b5ecf3dd6bc2c7ebb761f616df6c3ccaa364b34c4d4510a9dd1d4b`。这些 identity
绑定 fixture、输入与输出 digests、reduction 结果、生命周期、telemetry 状态和
source-read counters；wall time 与 RSS 样本不参与 identity。

主内存指标继续使用
`Pg = cp_reducer_constructed_peak_rss - cp_incidence_receipt_built_peak_rss`。本轮硬门槛
要求每个 worker lane 至少减少 20MiB，且相对 baseline 至少减少 15%。两个 lane 的
中位数相同，并通过门槛：

| Workers | Baseline Pg | Candidate Pg | Saved bytes | Saved MiB | Reduction |
|---:|---:|---:|---:|---:|---:|
| 1 | 113,311,744 | 88,539,136 | 24,772,608 | 23.625 | 21.862348% |
| 4 | 113,311,744 | 88,539,136 | 24,772,608 | 23.625 | 21.862348% |

时间回归仍要求中位数同时恶化超过 5% 和 2 个 baseline median absolute deviations
(MAD)。表中的 regression threshold 是两项边界的较大值。constructor 与完整 route
的 candidate 中位数均改善，因此没有触发门槛：

| Workers | Baseline constructor | Candidate constructor | Baseline MAD | Regression threshold |
|---:|---:|---:|---:|---:|
| 1 | 50,946,375ns | 42,068,000ns | 1,430,750ns | 2,861,500ns |
| 4 | 51,080,458ns | 41,055,541ns | 945,750ns | 2,554,023ns |

| Workers | Baseline route | Candidate route | Baseline MAD | Regression threshold |
|---:|---:|---:|---:|---:|
| 1 | 866,939,375ns | 847,101,375ns | 7,585,667ns | 43,346,969ns |
| 4 | 866,557,750ns | 843,487,833ns | 7,502,125ns | 43,327,888ns |

dense fixture 的 survivor source transforms 没有 pivot collision，因此本节的资源收益
只测量 collision-free basis rows 从 ownership copy 改为同步借用后的效果。发生
elimination 时，candidate 仍把生成的 basis row 存入地址稳定的 owned deque。专门的
tree-basis 单元测试覆盖多次 pivot collision、连续 owned append、后续 basis reuse 和
reducer row growth 后再次验证。该测试证明 owned 路径的正确性，但不提供
collision-heavy corpus 的 RSS 收益估计。

验证覆盖 `test_structured_tree_basis`、`test_structured_tree_basis_property` 和
`test_structured_batch_commit`，以及 relation module 35/35、TSan relation 16/16、
Release gate 188/188、120-bit relation path 1/1、bounded 50-digit route comparison
2/2、dense replay workers 1 和 4，以及上述 36-process sealed ABBA 对照。本轮没有
运行完整真实 50 位首轮，因此不能据此更新 NNZ、完整首轮 wall time、完整首轮 RSS 或
matrix-quality 结论。runner 继续记录 `promotion=false`。

### M6d-C Validation Epoch Cache Evidence

M6d-C 的正式 candidate commit 为
`4e71260fb56d1be9db6810894b7ecb2aca5f703d`，source tree 为
`0e541f113c0838865b16063b00042651a3ed6bf9`。该 tree 与测量时 candidate commit
`5554936d4fd6e5eb4b8cb154a80f95d81da16ac2` 逐位相同。baseline 测量 worktree 的
head commit 为 `57287252cff572b84acb8e8def052553fa422fbd`；它相对 production
commit `29ad00d3b35cc68489a80ed30a8935a119a0441f` 只修改本文，不影响可执行文件。
baseline 与 candidate Release 二进制的 SHA-256 分别为
`128d71e90a275c49f398bc4843a2f42282fa3981df16fc644685805ceb20e198` 和
`864015dbc22b9bbf6181644a1d80c21d45cea5eda2beff6d595d0966d753a1fb`。后者在证据
冻结后重新执行 Release 构建，仍精确复现相同 SHA-256。

记 A 为 baseline，B 为 candidate。每个 worker lane 都在 fresh process 中执行
`ABBA BAAB ABBA BAAB AB`。每个 revision、每个 worker 有 9 个样本，worker 1 与
worker 4 的顺序交错，共执行 36 个 Release 进程。每条 closed record 有 110 个字段，
其中 30 个是 wall time 或 RSS 观测字段。其余字段按既有规则形成 80-field lane
identity。worker 1 的 identity SHA-256 为
`569b41a5b53203a7f02fa8c644db46547649f3ce18c783683b6e98f28990dc1b`，worker 4 为
`e8ae0eb68c3d5d18030fe6ee63e669b3ba8dc08250a6227483f857fb854518af`。排除
`workers` 和 `peak_incidence_workers` 后，78-field 跨 worker identity SHA-256 为
`2ce17e7f15b5ecf3dd6bc2c7ebb761f616df6c3ccaa364b34c4d4510a9dd1d4b`。这些值与
M6d-B 相同，证明 fixture、digests、reduction 结果、生命周期和 source-read
counters 均未改变。证据 bundle `gnfs_m6dc_abba.VqN68s` 保留 `summary.json`、
`metrics.tsv` 和全部 36 条 raw records，供本机复核。

主指标定位在 reducer 构造完成至 reduction 完成之间：

$$
T_r = \mathtt{cp\_reduction\_complete\_wall\_ns}
    - \mathtt{cp\_reducer\_constructed\_wall\_ns}.
$$

硬门槛要求每个 worker lane 的 $T_r$ 中位数至少减少 8ms，且至少减少 25%。结果中的
`±` 值是 median absolute deviation (MAD)：

| Workers | Baseline $T_r$ | Candidate $T_r$ | Saved | Reduction | Separation gap |
|---:|---:|---:|---:|---:|---:|
| 1 | 34,884,208 ± 560,833ns | 21,742,334 ± 242,001ns | 13,141,874ns | 37.672846% | 10,932,333ns |
| 4 | 34,467,666 ± 592,708ns | 21,936,416 ± 240,125ns | 12,531,250ns | 36.356538% | 11,121,542ns |

`separation gap` 是最小 baseline 样本减去最大 candidate 样本。两个 lane 的全部样本
均分离，且同时通过绝对值和相对值门槛。constructor 与完整 route 使用非回归边界：
只有 candidate 中位数增量同时超过 baseline 的 5% 和两倍的较大 MAD 时才失败。
本轮四项增量均为负值：

| Workers | Baseline constructor | Candidate constructor | Delta |
|---:|---:|---:|---:|
| 1 | 45,144,125ns | 41,773,250ns | -3,370,875ns |
| 4 | 45,262,792ns | 42,211,292ns | -3,051,500ns |

| Workers | Baseline route | Candidate route | Delta |
|---:|---:|---:|---:|
| 1 | 885,739,750ns | 843,090,000ns | -42,649,750ns |
| 4 | 883,382,375ns | 836,132,542ns | -47,249,833ns |

`Pg` 的四组中位数均为 88,539,136 bytes，因此本轮不声明 reducer 构造边界的额外
内存收益。candidate 缓存最近一次完整验证成功的 incidence epoch。每次逻辑状态
修改都在首个写入前使缓存失效。$T_r$ 覆盖 singleton peeling、planning、commit 和
其间的验证，因此它不是独立 validation microbenchmark。不过，被删除的工作只位于
该窗口，identity 又保持不变，所以该指标可以定位 dense fixture 中重复全量验证扫描
的成本。它不能量化真实 50 位语料的命中率，也不能外推完整首轮收益。cache 使用的
atomic 只避免只读验证之间的数据竞争，不扩展 reducer mutation 的并发合同。

验证覆盖 `test_structured_tree_basis`、`test_structured_tree_basis_property` 和
`test_structured_batch_commit`，以及 Debug relation module 35/35 和 TSan relation
16/16。首次 relation module 运行只有无关的
`test_ooc_cleanup_transaction` 超时；该测试隔离复跑通过，随后完整 clean rerun 为
35/35。Debug 和 Release gate 均为 188/188。Release 120-bit relation path 为 1/1，
耗时 3.25s。bounded 50-digit route comparison 为 2/2，耗时 2.19s。后者的 51 个 raw
identity 字段逐字段相等，固定为 188 条 raw relations，digest 为
`2999840282289098554 / 11378523343223252016`。两条 route 均输出 0 行，并形成
`0 x 22660` matrix，符合冻结身份。dense replay workers 1 和 4 及上述 36-process
sealed ABBA 对照全部通过。

bounded 50-digit comparison 仍只是 4-SQ prefix，不是完整真实 50 位首轮。本节不更新
完整首轮的 NNZ、wall time、RSS、matrix quality 或 dependency-space 结论，也不提供
auto-promotion authority。runner 继续记录 `promotion=false`。

## Bounded Real 50-Digit Probe

真实探针固定输入：

```text
16000000000000004000000216000000000000027000000729
```

该数为 50 位、164 bit。探针调用公开 Pipeline phase API：
`select_polynomial()`、`build_factor_base()`、`sieve_and_collect()` 和
`build_matrix()`。二进制显式选择 legacy ordinary-OOC 或 structured direct-OOC
生产路由；旧短命令固定 structured，双路命令分别运行两者。两条路都关闭 resume、
distributed sieve、V0 BFS、V3 cascade、3LP 和其它会改变实验族的开关。探针还使用
`cofactor_inner_parallel_policy=forced_sequential`。ECM Stage 1、ECM Stage 2 和
Brent rho helper 固定为单线程，ECM curve pool 保持关闭。

```bash
./scripts/test.sh probe-50d-structured-ooc
./scripts/test.sh probe-50d-structured-ooc 8 2 auto
./scripts/test.sh probe-50d-structured-ooc 8 2 4
./scripts/test.sh compare-50d-bounded-routes
./scripts/test.sh compare-50d-bounded-routes 8 2 4
./scripts/test.sh compare-50d-first-round
./scripts/test.sh compare-50d-first-round 8192 4 auto
./scripts/test.sh campaign-50d-first-round
./scripts/test.sh campaign-50d-first-round 5 4 auto
./scripts/test.sh probe-50d-special-q-workers 4 auto
./scripts/test.sh probe-50d-special-q-workers 4 4
./scripts/test.sh check-50d-contracts
```

`probe-50d-structured-ooc` 保留短 structured-only 命令。三个可选参数依次是
`max_special_q`、`max_special_q_batch_workers` 和 `max_local_sieve_threads`。
`max_special_q` 接受 `[1, UINT32_MAX]`，默认值为 4。batch worker 接受 `[1, 4]`，
默认值为 4。local thread 接受 `auto` 或 `[1, UINT32_MAX]`，默认值为 `auto`。
显式线程值在 Pipeline 构造时钳制到硬件并发数。`max_special_q` 是严格硬上限；
最后一批不会向上取整到固定批次宽度。默认 4 个 special-Q 对应一个 production
batch，但仍只是 bounded prefix，不代表完整首轮。
CTest 中禁用的 `StructuredOOC50dProbe` 也固定使用 structured route 和 4-SQ 上限。

`compare-50d-bounded-routes` 使用相同参数，默认在两个顺序 fresh processes 中运行
4-SQ legacy 和 structured route。每个进程有独占临时 OOC base。runner 要求两条
`GNFS_EXPERIMENT_V2` 记录都报告 `first_round_complete=false`、
`sieve_rounds_completed=1`，以及
`special_q_budget_reached` 或 `special_q_range_exhausted`。默认每个进程的硬超时为
900 秒；显式 `--timeout` 可以替换该上限。

`compare-50d-first-round` 使用同一双进程协议，但默认
`max_special_q=8192`，每个进程的默认硬超时为 7200 秒。两条记录都必须报告
`first_round_complete=true`、`sieve_rounds_completed=1`，以及
`adaptive_round_limit_reached` 或 `effective_column_excess`。首轮限制来自 typed
Pipeline option，不依赖日志文本，也不会因为较大的 special-Q cap 进入第二轮。

两个双路由 runner 都只接受 Release 构建，并拒绝 `--no-build` 和 `--retry`。每个
子进程结束后，runner 检查原始 `.reldata`/`.relidx` pair 已删除，然后要求独占临时
目录可由 `rmdir` 删除。legacy 和 structured route 因此使用相同的终端清理契约。
任一路径失败或留下工件都会使对照失败，并保留非空目录供诊断。

双路由对照强制比较输入数字、固定配置、special-Q 数、raw rows/digest/duplicates、
完整 LP weight histogram、factor-base 列和 candidate 调度拓扑。output rows/digest、
LP columns 和 full-matrix shape/nonzeros 属于策略结果，runner 分别记录而不要求相同。
最终 `GNFS_EXPERIMENT_COMPARISON_V2` 明示 `promotion=false`；wall time 和 RSS 仅来自
两个 fresh processes，不是门禁阈值。

`campaign-50d-first-round` 在一个 Release 构建后运行可重复的完整首轮 campaign。
三个可选参数依次是每条 route 的样本数、special-Q batch worker 上限和 local sieve
线程数。样本数接受 2 至 9，默认值为 2；special-Q 上限固定为 8192。runner 拒绝
`--no-build` 和 `--retry`。默认每个 slot 的硬超时为 7200 秒，显式 `--timeout` 可以
替换该值。真实 campaign 当前只支持 Linux 和 macOS：精确 lock cleanup 依赖 POSIX
directory-fd 语义，递归进程回收依赖独占 POSIX process group。schema/parser synthetic
self-test 仍可在其它平台运行，但不会启动真实 slot。

唯一受支持的真实入口是 `./scripts/test.sh campaign-50d-first-round`。Python `run`
子命令仅供该 shell runner 内部调用；它不持有 shell flock，也不生成 closed one-test
`test_report.json`，不得作为独立或并发发布入口。

同一 `BUILD_DIR`/scope 的 shell 生命周期持有
`build/50d-campaigns/.complete_first_round_abba_v1.lock` 的 nonblocking
`zsh/system` flock，直到 canonical 与原子 `test_report.json` 都完成终态复核。并发调用在
触碰旧 report 或 canonical 前 fail-fast。lock leaf 会持久存在；它只是稳定 inode，不表示
当前有人持锁，也不得在 campaign 之间删除。真实所有权只属于 top-level zsh 的
close-on-exec fd，正常退出、失败和 signal unwind 都释放它。
在 terminal report 尚未 commit 时，top-level EXIT trap 默认撤销 canonical 与任何未提交
report、清理 stdout capture，并在锁内尽力发布唯一 `shell_unexpected_exit` 失败报告；只有
closed report 成功落盘后才 disarm，因此任一未守护的 shell `set -e` 退出也不能留下 pass。
failure diagnostic 的 slots 固定为 fail-fast prefix：failed slot 至多一个且只能位于末尾，
failure ordinal 必须绑定该 slot；若 prefix 全通过，则 ordinal 只能为空或绑定尚未执行的下一
slot。显式 local-thread 与 per-slot timeout 在 summary、CLI 和 artifact 中统一限制为 uint32。

runner 将 legacy 记为 A，将 structured 记为 B。它重复 `ABBA BAAB` 并取前
`2 * samples_per_route` 项。默认顺序因此是 legacy、structured、structured、legacy。
4 个样本使用 `ABBA BAAB`，5 个样本使用 `ABBA BAAB AB`。每个 slot 都启动新的
probe 进程，使用独占目录，并保留原始 stdout 和 stderr bytes。成功 slot 通过既有精确
cleanup 合同删除 OOC 工件；通过 source/artifact preflight 后的失败保留该次 campaign
独占诊断目录。preflight 自身失败时还没有 run directory，但 canonical 仍保持缺失且会写入
本次失败 test report。slot 运行中收到 Ctrl-C、SIGTERM 或内部轮询异常时，runner 会对
独占 process group 先发 TERM、限时等待，再按需发 KILL 并限时回收 leader 与 descendants，
随后才记录 `process/interrupted` 失败；不会遗留后台 50 位 probe 或孙进程。
outer shell 不使用 command substitution 承载长跑 runner：它把 stdout 写入独占临时文件，
保存 Python PID，并在只向 top-level zsh PID 发送 HUP/INT/TERM 时显式转发 TERM、等待 Python
完成 probe process-group 回收，再清理 capture、失效 canonical、发布失败 report 并释放锁。

campaign 在运行前、每个 slot 前和最终汇总前复核 clean tracked source、source commit、
source tree 和 probe binary SHA-256。direct runner 还从 probe 同目录的单链接
`CMakeCache.txt` 精确要求 `CMAKE_BUILD_TYPE:STRING=Release`，不能只在 artifact 中声称
Release。所有 slot 必须逐字段匹配现有 51-field raw identity。
同一 route 的 stop reason、output rows、LP columns、structured commit 结果、output digest、
matrix shape、nonzeros、signed delta 和 row-mapping identity 也必须稳定。legacy 与
structured 的策略输出仍允许不同。每条 source record 还独立复核 special-Q/candidate
topology、raw duplicate、LP histogram、raw target、reduction/matrix shape 和 route-specific
OOC lifecycle；两个 route 一致地报告错误值也不能通过。秒值使用 `Decimal` 解析并要求
canonical、有限、非负且位于 binary64 范围。显式 local-thread request 可被 C++ 按硬件
并发数向下钳制；effective budget 必须为正且不超过 request，不要求错误的相等关系。
production candidate chunk 固定为 256；structured route 还必须证明 reducer 已启动，不能
报告 `structured_stop=not_started`。`structured_emitted_rows` 是中间发射计数，可合法大于
后续 reduction output rows，因此不会被错误地当作 output 上界。

方向预算使用精确整数和有理数计算。每个 structured 样本必须满足
`matrix_signed_delta > 0`；structured 的 wall 中位数不得超过 legacy 的 1.20 倍，peak
RSS 中位数不得超过 1.60 倍，matrix nonzeros 不得超过 30 倍。RSS backend 缺失或跨样本
不一致会得到 `unavailable` 并 fail closed。这些宽边界只拦截方向性退化；campaign 仍固定
`promotion=false`，不能据此声明完整分解或自动推广。

campaign 使用独立 closed `GNFS_50D_ROUTE_CAMPAIGN_V1`，不会增加或重解释 58-field
`GNFS_EXPERIMENT_COMPARISON_V2`。合法新运行先失效
`build/50d-campaigns/complete_first_round_abba_v1.json`。只有全通过的 pass evidence 才能
成为 canonical artifact：runner 先从每个原始 route record 重建 identity、稳定性、预算和
summary，再精确清理成功 staging；随后在 canonical 同目录写入临时文件、flush、fsync、
重读并按 closed schema 复核，最后才通过 `os.replace()` 发布。所有失败都让 canonical
保持缺失；通过 source/artifact preflight 并创建 run directory 后的失败只在该目录原子
写入 `diagnostic.json`，其 summary 固定
`artifact_published=false`；旧 pass 不会存活，也不会用 fail JSON 替代。outer shell 在
记 pass 前还会重新读取 canonical regular file、运行同一 closed validator，并要求其中的
summary 与 stdout summary 逐字一致。整个 campaign（含 build、binary/CLI preflight）在
`test_report.json` 中只计为一个测试；任一 preflight failure 都覆盖旧报告，slot 不单独
增加 pass 或 fail 计数。canonical leaf 使用不解析最终 symlink 的 lexical absolute path；
失效操作删除链接本身，validator 也拒绝链接。`build/50d-campaigns` 的父目录仍属于受信任的
本地 build namespace，不接受攻击者可替换的祖先 symlink。
唯一 `test_report.json` 也在锁内通过同目录 temp、flush/fsync、序列化重读、`os.replace()`、
parent fsync 和最终 closed re-read 发布；任何 post-replace 失败会先精确删除未通过验证的
report，再撤销 canonical pass。artifact/diagnostic 的整数和布尔字段按 JSON 实际类型比较，
不接受 Python 的 `true == 1`/`false == 0` 等价；schedule 时间固定为可解析且非逆序的
`YYYY-MM-DDTHH:MM:SSZ`。
独立 summary validator 对 pass 记录也直接要求 wall/RSS/matrix-nonzeros ratio 分别不超过
1.20/1.60/30.0 倍，而不是只检查三个字段非 `na`。

所有 50 位 runner 通过同一封闭式 `GNFS_EXPERIMENT_V2` schema 解析器。它固定完整
字段集合和顺序，拒绝缺失、重复、未知或非规范字段，并校验布尔/数值/枚举、RSS
support-value 联动、factor-base 列总数和 matrix signed delta。解析器同时运行
missing/duplicate/unknown/reordered/noncanonical/partial-RSS 合成负向自检。
`check-50d-contracts` 通过 probe emitter 的无流水线固定
`GNFS_EXPERIMENT_FIXTURE_V2` fixture 执行同一解析器，并先断言 fixture 没有输出
production `GNFS_EXPERIMENT_V2` 前缀，再覆盖 CLI 负例、help 边界和 campaign V1 的
closed schema、顺序、identity、稳定性、预算、timeout 及原子发布边界；
`StructuredOOC50dContract` 将它作为 fast CTest。
shell synthetic 直接安装生产 EXIT/signal handler，预放 stale pass evidence 后分别强制
unexpected exit 和只向 top-level zsh PID 发送 TERM，并复核 runner/child 回收、canonical/
capture 清理、唯一失败 report 与 lock 重获；process self-test 另覆盖忽略 TERM 后的 KILL
fallback，以及 emitted rows 大于 output rows 的合法正例。

`probe-50d-special-q-workers` 使用同一个 Release 构建，分别在 3 个新进程中运行
workers 1、2 和 4。它的第二个可选参数设置 3 个进程共同使用的
`max_local_sieve_threads`；默认值为 `auto`。runner 比较 relation digest、structured
reduction、矩阵形状和工件生命周期字段；每个 worker 的通道分配、wall time 与 RSS
只记录，不参与 identity 判定。1/2/4 对照要求冻结后的预算至少为 4，并机械检查每次
实际达到声明的 outer-worker topology；低核机器或更小显式预算会 fail closed。

探针使用显式 matrix-only 边界，在 MatrixBuilder 完成后返回。它不依赖 thin-matrix
bypass，也不要求 `matrix_signed_delta` 为负；该值和矩阵形状都是需要分别记录的策略
结果。记录必须证明没有进入 SGE、Block Lanczos、Block Wiedemann、平方根或因子提取。

成功时输出一行 `GNFS_EXPERIMENT_V2`。报告必须使用以下 claim boundary：

```text
scope=bounded_50d_prefix_probe
resume_scope=none
route=legacy|structured
route_evidence=production_legacy_ooc|production_direct_ooc
sge_attempted=false
solver_attempted=false
sqrt_attempted=false
factorization_attempted=false
```

legacy route 使用 `production_legacy_ooc`；structured route 使用
`production_direct_ooc`。`first_round_complete` 只在 raw relation count 达到初始 raw
target、恰好完成一轮 reduction，且 typed stop reason 为
`adaptive_round_limit_reached` 或 `effective_column_excess` 时为 true。报告还包含
typed `sieve_stop_reason`、完成轮数、hard cap、请求与
冻结后的计算通道预算、批次 worker 配置、单批总通道和单 worker 峰值、两阶段
candidate 模式、固定 chunk size、candidate worker/chunk/corpus 统计、raw/output
digest 与 rows、完整输入 LP weight histogram、矩阵 rows/columns、row mapping
identity、有符号 row-column delta、nonzeros、wall time 和 process RSS。
`candidate_generation_s` 与 `candidate_cofactor_s` 分别记录两个阶段的累计 wall time。
runner 使用独立临时目录；成功后只在目录为空时执行 `rmdir`，失败或生命周期异常时保留
目录供诊断。

完整首轮双路由记录是 M5 的真实输入证据入口，不是 M5 自动通过标志。它也不是 SIQS
sealed RSS 协议，两者的语料、进程边界和 promotion authority 都不同。生成一条通过的
comparison record 不会启用 `GNFS_STRUCTURED_FILTER=auto`，不会授权 M6，也不会改变
默认 reduction policy。

candidate 批次的 current RSS 使用 `first_max_candidates` 配对策略。Pipeline 只在
当前批次候选数严格大于既有样本时替换样本；并列最大值保留最早批次。记录包含：

- `candidate_batch_rss_sample_candidates`；
- `candidate_batch_after_generation_current_rss_bytes`；
- `candidate_batch_after_cofactor_current_rss_bytes`；
- `candidate_batch_after_release_current_rss_bytes`。

后三个字段必须全有或全无。缺少 current RSS 支持时统一写为 `na`，不得混合部分样本。
`candidate_batch_rss_sample_candidates` 标识配对样本的 workload，不是 RSS 数值。
RSS 字段只作资源观测，不参与 worker identity 比较。

`after_generation` 在 Stage A workers 和 `LatticeSieve` 析构后采样，但仍保留
`SieveResult`。`after_cofactor` 在 candidate workers、chunk scratch 和 worker-local
`Cofactorizer` 析构后采样，此时仍保留批次输入与归并前输出。`after_release` 在输出
移入 collector，并析构批次输入与输出 storage 后采样。

`after_release` 是进程 current RSS，不是已析构 storage 的 owned bytes。allocator
可能保留页面，因此它不保证低于前两个样本。candidate worker 数为 1 时，顺序路径在
调用线程执行，ECM thread-local state 可保留到进程结束。任何阶段差值都不能形成
单调断言、跨平台阈值或 CI 通过条件。

## Candidate Worker and Chunk Sweep

```bash
./scripts/test.sh sweep-50d-candidate-batch
./scripts/test.sh sweep-50d-candidate-batch 1
```

`sweep-50d-candidate-batch` 固定同一个 50 位输入，并通过公开 Pipeline 阶段生成
polynomial 与 factor base。随后按 production 参数生成首个 4-SQ 批次的完整
`SieveResult`，整个 sweep 复用该不可变候选语料。该模式不执行 relation filtering、
matrix 或平方根。计时边界覆盖一次完整的 `verify_candidate_batch()` 调用，包括 chunk
规划、worker-local `Cofactorizer` 构造、线程创建与回收、结果 storage 和有序归并；它
不是纯 scheduler 或纯 cofactor 内核计时。

测试先用单个 `Cofactorizer` 按 special-Q 和 candidate ordinal 建立串行 oracle。之后
执行以下笛卡尔积，默认重复 3 次：

- worker cap：`1,2,4,6,8,10`；
- chunk size：`64,128,256,512,1024`。

每个 case 必须逐字段匹配串行 relation corpus，并匹配 order-sensitive relation digest。
测试还在每次调用后重算候选语料 digest，拒绝输入 mutation。重复轮次以固定 stride
轮换起始 case，减少固定顺序偏差；这不能消除温度、调频、allocator 或 thread-local
state 造成的噪声。

成功运行输出 30 条 `GNFS_CANDIDATE_SWEEP_CASE_V1` 和 1 条
`GNFS_CANDIDATE_SWEEP_SUMMARY_V1`。case 记录包含 worker cap、实际 worker 数、chunk
数、候选数、关系数、最小值、中位数、最大 wall time，以及 candidate/relation digest。
summary 记录包含 Release 构建类型、计时边界、固定输入身份、Stage A 通道计划和完整
sweep 网格。runner fail closed：它拒绝非 Release 构建，并验证记录数量、网格唯一性、
状态、计时边界和 repetitions。每条 case 的 run fingerprint、candidate/relation digest
及候选与关系计数还必须与 summary 一致。

该测试是 disabled `bench;stress` CTest target，只能提供本机固定语料的调度证据。
wall time 不参与测试通过条件，也不能单独证明其它输入尺寸、完整首轮或其它机器的最优
production 默认值。修改默认 worker 或 chunk policy 前，应保留原始 30 条 case 记录，
并结合 64-SQ 有界探针复核端到端 candidate cofactor 时间与结果身份。

### 2026-07-22 Fixed-Batch Result

macOS arm64、10 个逻辑 CPU、Release 构建和 3 次重复得到 2,284 个候选与 188 条
关系。每格为 `wall_median_ns`，30 组均匹配同一个串行 oracle、candidate digest 和
relation digest：

| Worker cap | Chunk 64 | Chunk 128 | Chunk 256 | Chunk 512 | Chunk 1024 |
|---:|---:|---:|---:|---:|---:|
| 1 | 1,215,956,708 | 1,201,334,542 | 1,201,845,958 | 1,205,810,667 | 1,203,964,750 |
| 2 | 634,944,625 | 664,563,750 | 640,195,542 | 653,112,000 | 655,523,417 |
| 4 | 367,986,792 | 377,014,750 | 365,794,083 | 388,611,333 | 555,833,458 |
| 6 | 290,232,792 | 288,052,584 | 272,003,667 | 391,309,084 | 557,975,542 |
| 8 | 245,546,708 | 251,104,917 | 287,579,833 | 393,264,625 | 556,367,500 |
| 10 | 217,915,042 | 218,573,792 | 272,537,208 | 391,530,500 | 556,931,875 |

固定身份为：

```text
run_fingerprint=4092857329928806441:5852943191461741189
candidate_digest=5728613215710933158:14682671205889688810
relation_digest=2999840282289098554:11378523343223252016
relations_per_special_q=39,13,116,20
```

10-worker 下，chunk 64 与 128 的中位数相差约 0.3%；二者分别比当前固定 chunk 256
缩短约 20%。4-worker 和 6-worker 的最优值仍在 chunk 256。该交叉说明单个静态值不
适合直接按这一个批次推广。后续自适应原型结果见下一节。

### Adaptive-Grain Prototype: Not Promoted

`adaptive_canonical_v1` 原型根据每批 candidate 分布和 worker cap，在 64、128 与
256 中选择最小 grain，使 canonical chunk 数不超过每个 worker 3 个。64-SQ 探针固定
4 个 outer workers、10 个本地计算通道和同一 50 位输入。每个 case 使用 3 个独立
Release 进程：

| Policy | Candidate cofactor runs | Median | Chunks |
|---|---|---:|---:|
| Fixed 256 | 10.6729s, 11.0043s, 10.7301s | 10.7301s | 499 |
| Adaptive | 11.0434s, 10.6903s, 10.6495s | 10.6903s | 571 |

两组均处理 118,311 个 candidates，产生 6,047 条 raw relations 和 16 条 output
relations。relation digest、matrix identity 和生命周期字段一致。自适应策略在 16 个
批次中分别选择 1 次 64、5 次 128 和 10 次 256，并有 7 个批次无法满足目标 chunk
数。

自适应策略的均值和中位数只缩短 0.07% 与 0.37%，小于两组约 3% 至 4% 的轮间波动。
它同时把 chunk 数从 499 增加到 571，增幅为 14.4%。该证据不能支持新增生产策略、
统计 schema 和维护面，因此生产默认值继续固定为 256，原型不合入。

重新评估前，应在 4-SQ 和 64-SQ 上用新进程交错比较固定 128 与固定 256，并扩展到
更大的真实尺寸。候选策略至少应稳定缩短 3% 的 cofactor 中位时间，理想目标为 5%，
且尾部时间与 RSS 不得退化。

### SQUFOF Profile and Residue-Filter No-Go

固定 50 位语料的 15 秒 `/usr/bin/sample` profile 显示，active top-of-stack 样本主要
位于 `SQUFOF::squfof_core()`。它有 21,225 个样本；`__udivmodti4`、代数试除、有理
试除和 allocator 分别只有 614、166、99 和 11 个。该 profile 将下一轮优化边界从
调度和分配收敛到 SQUFOF 内核。

第一个原型在平方根前增加精确的模 64 二次剩余过滤。64 个 residue 中只有 12 个可能
是平方，因此该过滤可以跳过 52 个不可能分支。编译期穷举和单元测试证明它不会拒绝
平方；50 位 sweep 的 candidate、relation 和 run identity 也保持不变。

性能对照保留两份独立 Release 二进制，并按 baseline、filter、filter、baseline 顺序
各运行 1 次完整 30-case sweep：

| Metric | Baseline | Filter | Delta |
|---|---:|---:|---:|
| Mean process wall | 19.375s | 19.405s | +0.15% |
| Mean process user CPU | 44.51s | 44.90s | +0.88% |
| Mean summed case wall | 17.306s | 17.534s | +1.32% |
| Worker 1, chunk 256 | 1.254s | 1.276s | +1.72% |
| Worker 10, chunk 256 | 295.806ms | 306.108ms | +3.48% |

额外的 residue 检查没有抵消 Apple M5 上硬件平方根与既有循环的成本，且生产相关
case 出现退化。该结果未达到 3% 推广门槛，因此原型不合入。后续 SQUFOF 优化必须
减少实际循环、除法或失败 multiplier 工作量，不能只在平方检测前增加过滤分支。

### SQUFOF Quotient Fast Path and Half-Step Unroll No-Go

[Gower–Wagstaff 第 3.3 节](https://homes.cerias.purdue.edu/~ssw/squfof.pdf#page=11)
规定递推后只检查 `Q_2, Q_4, ...`。msieve 的
[官方实现](https://github.com/radii/msieve/blob/c8727d91305bdbe0972d160ef0ce61dd02ce9193/common/smallfact/squfof.c#L72-L172)
还为商等于 1 增加无除法快路，并把奇偶半步展开。两项原型均保持 multiplier、迭代
预算、无符号运算顺序和 inverse-walk 边界不变。

无除法快路使用独立 Release 二进制做单轮成对比较。基线二进制 SHA-256 为
`4c523a3cd84d44ea9fece0594e73285de9edf58b7ef69b8d45da2bc401993c90`，快路二进制为
`9be6af65f751d57f261ca6127f184abc0f2a758a3ffe183cf8b9eb4cb2338ee3`。在 Apple M5 上，
30-case wall sum 从 17.397s 增至 21.485s，增幅为 23.5%；process user time 从
44.68s 增至 54.86s。该分支优化针对的旧架构除法代价不适用于当前目标机，原型撤回。

双步展开不含上述快路。展开版二进制 SHA-256 为
`41091ddebd33bf38f6d5f8ab64c90b1e282d480428471ed871ad069ffc71e6f7`。按
baseline、unroll、unroll、baseline 顺序各运行两次完整 sweep：

| Metric | Baseline mean | Unroll mean | Delta |
|---|---:|---:|---:|
| Process wall | 19.435s | 19.470s | +0.18% |
| Process user CPU | 44.525s | 44.460s | -0.15% |
| Summed case wall | 17.390s | 17.403s | +0.08% |
| Worker 1, chunk 256 | 1.284s | 1.276s | -0.67% |
| Worker 10, chunk 256 | 299.895ms | 295.614ms | -1.43% |

两项原型的 2,284 个 candidates、188 条 relations 和 candidate/relation digest 均与
基线一致。展开版的差异小于轮间波动，也远低于 3% 推广门槛，因此生产循环保持原状。
下一轮若改变 multiplier 集合、顺序、竞速方式、queue 或总预算，必须作为成功率与失败
集合可能变化的独立策略实验，不能归类为等价微优化。

### SQUFOF Multiplier Fallback Promotion

固定 50 位 serial oracle 实际调用 SQUFOF 159 次。一次不计时的临时诊断显示，`k=1`
接受 80 次；剩余调用按旧顺序由 `k=3,5,7,11,15,...` 分别再接受
42、15、6、5、9、0、0、2 次，总计执行 744,448 个 forward iterations。
[Gower–Wagstaff Table 5](https://homes.cerias.purdue.edu/~ssw/squfof.pdf#page=34)
给出当前 multiplier 集合中 `k=15` 的最低理论工作系数。候选策略因此保留 `k=1`
首位，只把 `k=15` 提到第二位，其余 fallback 顺序不变。

基线 Release 二进制 SHA-256 为
`4c523a3cd84d44ea9fece0594e73285de9edf58b7ef69b8d45da2bc401993c90`，候选二进制为
`02862a90b9c93d453ffd5b7f605e20702d0b44c19c950ffa01f4c485944f54ac`。按
baseline、candidate、candidate、baseline 顺序完成 ABBA sweep：

| Metric | Baseline mean | `k=15` second mean | Delta |
|---|---:|---:|---:|
| Process wall | 19.270s | 15.400s | -20.1% |
| Process user CPU | 44.860s | 37.375s | -16.7% |
| Summed case wall | 17.464s | 13.817s | -20.9% |
| Worker 1, chunk 256 | 1.271s | 1.066s | -16.1% |
| Worker 10, chunk 256 | 294.162ms | 208.093ms | -29.3% |
| Maximum RSS | 230.8MiB | 222.6MiB | -3.6% |

每轮均保持 2,284 个 candidates、188 条 relations，以及完全相同的 run fingerprint、
candidate digest 和 relation digest。完全倒序会让 summed case wall 增长约 81%，因为
本实现会把每个大 multiplier 跑满预算；按论文系数重排全部 fallback 的单轮结果为
16.166s，也明显慢于只提升 `k=15` 的 13.817s。因此不采用 msieve 的降序策略或完整
理论排序。

同集合重排不会改变单次 `SQUFOF::factor()` 的成功集合，但可能改变首先返回的因子；
opt-in 3LP 的递归拆分因此需要单独保护。`test_3lp_cofactor` 新增 6 组约 60-bit、三个
因子都大于一百万的 hard triples。旧顺序和候选顺序均通过全部 39 项检查，并返回相同
的排序后三因子。该证据与 50 位生产语料共同支持推广最小重排，不支持新增 multiplier、
共享总预算或 multiplier racing。

64-SQ bounded probe 另保留两份独立二进制。旧顺序 SHA-256 为
`75e1f4fee12b64fb02bfc440df9f4d0896abeeb812ba2df232ac60d6c57aeac3`，候选顺序为
`f5624182a44504f8c646376c2242cab1a1f05bc6b2aaa433706d1ed27efb32ca`。ABBA 两轮中，
candidate cofactor 均值从 11.889s 降至 11.716s，改善 1.46%；process wall 改善
0.52%，user CPU 增加 0.29%，peak RSS 增加 2.72%。这些差异不足以单独证明 64-SQ
提速，但没有出现超过 3% 的链路退化。四轮均保持 118,311 个 candidates、6,047 条
raw relations、16 条 output relations 和相同的 raw/output digest。

### SQUFOF Strategy Benchmark Baseline

专用基准将策略筛选从完整 CandidateBatch 扫测中分离。固定
[`fixed_50d_squfof_strategy_v1`](../../tests/fixtures/squfof_strategy_corpus_v1.hpp)
包含 192 个 `(n, max_iterations)`，运行时不生成随机数：

- 前 159 项保持固定 50 位 serial oracle 的实际 SQUFOF 调用顺序；
- 12 项来自 hard-3LP 生产 helper 流；
- 21 项来自现有 seed-42、60 位 ECM 测试语料，并使用生产可达预算。

前一来源受 50 位 `B²` 上界限制，不能覆盖高位 cofactor。后两个来源明确作为补充，
而不宣称来自同一无偏生产分布。语料覆盖如下：

| Dimension | Value | Cases |
|---|---:|---:|
| Bit band | `<2^40` | 65 |
| Bit band | `2^40`–`2^50` | 100 |
| Bit band | `2^50`–`2^62` | 27 |
| Iteration cap | 1000 | 6 |
| Iteration cap | 2000 | 59 |
| Iteration cap | 5000 | 117 |
| Iteration cap | 20000 | 10 |

`./scripts/test.sh bench-squfof 3` 只计普通 `SQUFOF::factor()` 调用和预分配结果写入。
热身、哈希、合法性检查和 diagnostics pass 均在计时外。2026-07-22 的首个 Release
基线通过 3 条 CASE、11 条 MULTIPLIER 和 1 条 SUMMARY 的 fail-closed 校验：

| Metric | Baseline |
|---|---:|
| Cases per repetition | 192 |
| Successes / bounded failures / invalid | 166 / 26 / 0 |
| Wall min / median / max | 1.405s / 1.407s / 1.417s |
| Untimed forward iterations | 2,584,580 |
| Accepted by `k=1 / 15 / 3 / 5` | 84 / 47 / 18 / 9 |
| Accepted by `k=7 / 11 / 21 / 35` | 3 / 1 / 2 / 2 |

当前 identity 供后续独立二进制 A/B 使用：

| Identity | Low | High |
|---|---:|---:|
| Corpus | 11585003526353080300 | 16066302168872607439 |
| Schedule | 12597619067809015512 | 9409246306371504405 |
| Factor result | 18063867608300455638 | 15891697325343906257 |
| Success set | 5104749477666353807 | 5452693313291758656 |
| Failure set | 13060603425789684408 | 2814528182299744335 |

失败位图为
`0000000000000000000000000000000000000008aaaffff7`。策略实验不得新增失败；
等价微优化还必须保持 factor digest。墙钟时间仅用于交错 A/B/B/A 比较，不进入通过阈值。

### SQUFOF Budget Prospective Corpus V1

旧 V1 在 `max_iterations=20000` 的 10 行中只有 1 个成功 case，不能据此推广按 slot
缩短预算。统一绝对 cap `10056` 是在旧公开语料上形成的预注册候选：它保持旧语料
192/192 的原始 factor identity，并把 forward iterations 从 2,584,580 降至
1,888,500；训练集拟合的 per-slot cap 则在旧 validation/holdout 共丢失 4 个 factor，
已经判为 no-go。以上数字只用于确定下一轮候选和样本需求，不构成生产推广证据。

[`prospective_squfof_budget_corpus_v1`](../../tests/fixtures/squfof_budget_corpus_v1.hpp)
在运行任何新 SQUFOF probe 前冻结。固定 SplitMix64 seed
`0x4255444745545631` 为 low、mid、high 三个 production budget band 各生成 32 个
不同素数乘积；生成器只因素性、重复输入和结构边界拒绝候选，禁止读取 SQUFOF
成功、factor、multiplier 或迭代数。每个 `n` 相邻保存 3LP 与普通 2LP 两行，预算为：

| Band | Product range | 3LP budget | normal 2LP budget | Unique `n` |
|---|---:|---:|---:|---:|
| Low | `<2^40` | 1000 | 2000 | 32 |
| Mid | `[2^40, 2^50)` | 2000 | 5000 | 32 |
| High | `[2^50, 2^62)` | 5000 | 20000 | 32 |

相同 `n` 的两行是一个不可拆分组。每个 band 内只按固定 `n` hash 排序，再以
train、train、validation、holdout 循环分配；三个 split 分别包含 48/24/24 个唯一
输入。冻结 identity 为：

| Identity | Low | High |
|---|---:|---:|
| Corpus | 16007979797267497993 | 6430637409354473680 |
| Grouped split | 17722147925989565997 | 4435973663510799258 |

`test_squfof_budget_corpus` 只验证 generator provenance、素因子、乘积、band、caller
budget、digest 和 split；它刻意不包含 `squfof.hpp`。因此该提交形成输入密封点。
后续首次 probe 可用于检验已预注册的 `10056` 候选。高位 semiprime 的普通 2LP 行
是该候选的直接前瞻证据；配对的 3LP 行仅冻结预算接口，不代表自然三素因子输入分布。
SIQS 的 50000 预算与不同 fallback 链不在此语料范围内。

### SQUFOF Budget Matrix V3 and Coverage No-Go

`test_squfof_budget_oracle` 在上述输入密封提交之后才首次调用生产 probe。它分别为旧
公开 V1 与 prospective V1 构建 192×11×5 的 case×slot×cap 矩阵；cap ladder 为
`1000,2000,5000,10056,baseline`，另保留每个 slot 的独立 production-baseline
观测。矩阵校验覆盖 clamped effective cap、重复 cap 一致性、overflow、前缀单调、
terminal result、原始 factor 和固定顺序 baseline replay。两个 V3 identity 为：

| Matrix | Low | High |
|---|---:|---:|
| Legacy public V1 | 17887356378357031550 | 10510892644681870966 |
| Prospective locked V1 | 10761457262857602067 | 5850380446892829808 |

矩阵采集按输入行并行，但结果写回固定 row index，最多使用 8 个 worker；摘要和所有
digest 不包含机器相关线程数。旧矩阵的 baseline projection 还必须逐字段重现 V2
matrix 与 split identity。

预注册统一 `10056` cap 的确定性结果为：

| Corpus | Baseline successes / failures | Baseline iterations | Candidate iterations | Raw-factor mismatch |
|---|---:|---:|---:|---:|
| Legacy public V1 | 166 / 26 | 2,584,580 | 1,888,500 | 0 |
| Prospective locked V1 | 117 / 75 | 6,938,458 | 4,512,122 | 0 |

prospective 高位普通 2LP 的 32 行是本轮真正目标切片。它们在 production cap 20000
下全部 bounded-fail，候选将 work 从 4,880,000 降至 2,453,664，未新增 factor；但
baseline success 覆盖为 0。门禁预先要求至少 8 个目标成功 case，因此结果必须是
`offline_decision=insufficient_evidence`，不能因失败路径更便宜而标记 eligible。

固定 production order 的精确 train-only cap 搜索进一步说明不能跳过独立切片。旧
训练集最优 policy 在 published validation/holdout 分别产生 2/1 个 factor mismatch；
prospective 训练最优 policy 虽在 validation 保持一致，却在 confirmation 丢失 3 个
factor。两者都只作为过拟合回归证据，生产 schedule、per-slot cap 和 caller 均未改动。
下一轮需要在读取结果前冻结按 factor-balance 结构生成的高位普通 2LP success
challenge corpus；只有补足成功覆盖后才允许重新判断 `10056` 候选。

### SQUFOF Success-Coverage Challenge V1

[`prospective_squfof_success_challenge_v1`](../../tests/fixtures/squfof_success_challenge_v1.hpp)
是上述 no-go 后的独立输入密封点。生成器 `splitmix64-factor-balance-prime-pairs-v1`
使用 seed `0x5355434345535331` 和 profile mixer `0xd1b54a32d192ed03`，只按素性、
结构区间、重复输入或重复因子拒绝样本；它不得读取 SQUFOF 成功、factor、multiplier、
迭代数、split 或 hash。三个 profile 的因子位数和样本数为：

| Profile | Factor widths | Additional structure | Unique `n` |
|---|---:|---|---:|
| Close balanced | 29 × 29 bit | `q-p` in `[2^18, 2^22)` | 64 |
| Mildly skewed | 28 × 30 bit | distinct factors | 64 |
| Moderately skewed | 27 × 31 bit | distinct factors | 64 |

三组都固定为 57 位或 58 位的普通 2LP 输入和 production budget `20000`，从而把因子
平衡度与总位宽分离。每个 profile 内仅按固定 `n` hash 排序，再循环分配
train、train、validation、confirmation；总 split 为 96/48/48，并且本轮 train 不得
重新拟合已经冻结的统一 cap `10056`。密封 identity 为：

| Identity | Low | High |
|---|---:|---:|
| Corpus | 10783171939506602749 | 9236118909252415409 |
| Grouped split | 5936611983363779581 | 6396469101558652297 |

`test_squfof_success_challenge_corpus` 不包含 `squfof.hpp`，只验证生成合同、素因子、
乘积、profile、唯一性、split 和 digest。本提交不产生任何 factor 或迭代结果；只有在
该输入合同提交后，后续独立提交才可比较 production cap `20000` 与候选 `10056`。
即使挑战集补足成功覆盖，它也只验证高位 normal-2LP caller，不代表自然 LP 出现频率，
仍不能替代真实 `classify_cofactor`、CandidateBatch 和 fallback 链 A/B。

密封提交后的首次观测只比较上述两个固定 cap，不运行 optimizer，也不按结果筛样。
`test_squfof_success_challenge_oracle` 对每行独立调用两套 `factor()`，再完整采集并重放
11 个 production multiplier slot。最多 8 个 worker 只写固定 row index；线程数和墙钟
不进入 digest。结果为：

| Scope | Baseline success / failure | Candidate success / failure | New failures | Baseline work | Candidate work |
|---|---:|---:|---:|---:|---:|
| All | 5 / 187 | 3 / 189 | 2 | 41,526,608 | 20,976,188 |
| Close balanced | 0 / 64 | 0 / 64 | 0 | 13,980,000 | 7,029,144 |
| Mildly skewed | 4 / 60 | 2 / 62 | 2 | 13,637,284 | 6,948,800 |
| Moderately skewed | 1 / 63 | 1 / 63 | 0 | 13,909,324 | 6,998,244 |
| Train | 1 / 95 | 1 / 95 | 0 | 20,821,282 | 10,469,578 |
| Validation | 1 / 47 | 0 / 48 | 1 | 10,377,302 | 5,289,456 |
| Confirmation | 3 / 45 | 2 / 46 | 1 | 10,328,024 | 5,217,154 |

两个新失败都来自 mildly-skewed profile：validation row 67 的原始 factor
`617881661` 变为失败，confirmation row 97 的原始 factor `152489791` 变为失败。
虽然总 work 降低 49.49%，但 raw-factor identity 和 new-failure 门禁均失败；同时
baseline 仅有 5 个成功 case，低于预注册的 8 个成功覆盖下限。正确性失败优先，因此
最终结论是 `offline_decision=no_go`，不得进入 production 或 fallback 链 A/B。

首次观测冻结的 identity 为：

| Identity | Low | High |
|---|---:|---:|
| Matrix | 66321629368464418 | 14097916444529299682 |
| Result | 8167599806903207849 | 12264992225924573372 |

### SQUFOF Strategy Matrix V2 and Order No-Go

`test_squfof_strategy_oracle` 对上述 192 项语料和生产 11-slot schedule 生成完整的
192×11 反事实矩阵。每个 cell 独立运行一个生产 multiplier stage，记录 overflow、
forward iterations、core hit 和原始 accepted factor。当前顺序重放必须逐项等于
`SQUFOF::factor()`；不能把互补因子规范化为较小因子。

```bash
./scripts/test.sh -t Release run test_squfof_strategy_oracle
```

矩阵优化只允许重排全部 11 个 slot，不允许删减 multiplier。精确 subset DP 对尚未解决
的 case 累加下一 slot 的 forward iterations；26 个全失败 case 因此始终支付全部 slot
成本。若某次转移会让不同于生产 reference factor 的因子先返回，该转移不可行。

数据划分不读取 probe outcome。每个位宽段内先按 `n` 分组，再按固定哈希排序，以
train、train、validation、holdout 循环分配；相同 `n` 的不同预算不会跨集合。V2 身份为：

| Identity | Low | High |
|---|---:|---:|
| Matrix | 13897133093001924776 | 2379028879204942237 |
| Stratified split | 11247766345367590209 | 16940138875728727164 |

train/validation/holdout 分别包含 99/47/46 行；四个位宽段的 unique-`n` 分组分别按
33/16/16、26/13/13、24/12/12 和 9/4/4 分配。该 holdout 在 V2 发布后是固定的回归
切片，不应在后续实验中冒充新的独立统计样本。

train-only DP 得到顺序
`1,15,3,33,21,5,7,11,35,55,77`。它保持全部 192 项的 factor identity，但离线门禁
明确拒绝推广：

| Slice | Current iterations | Candidate iterations | Delta | Gate |
|---|---:|---:|---:|---|
| Train | 1,634,438 | 1,588,918 | -2.79% | fail: requires at least -5% |
| Validation | 550,664 | 562,016 | +2.06% | fail: requires at least -3% |
| Holdout | 399,478 | 392,062 | -1.86% | fail: requires at least -3% |
| All 192 | 2,584,580 | 2,542,996 | -1.61% | informational |

逐位宽门禁也失败：41–43 bit 与 44–46 bit 分别回退 3.21% 和 5.10%，超过 1% 上限。
因此不构建候选生产二进制，也不运行墙钟 A/B；当前生产顺序保持不变。这个 no-go 与
训练、验证、holdout、位宽门禁均由测试 summary 机器可读地报告。

## Deterministic 120-Bit Relation-Path Gate

The M5 cross-size route now has a real sieve-to-reduction gate:

```bash
./scripts/test.sh -t Release run test_structured_filter_pipeline_120bit
```

The input is the 120-bit semiprime
`664613997892503507403755373348813853`. The test freezes one polynomial exactly,
builds its factor base, fixes `max_special_q=32`, disables OOC/resume/distributed
and other strategy-changing experiments, and compares two production routes:

- StandardV0 with one local sieve lane;
- forced structured reduction with the hardware-bounded profile, up to four
  local sieve and structured-incidence lanes.

Two independent Release processes produced the same canonical record. The
frozen identity is:

| Field | Legacy | Structured |
|---|---:|---:|
| Raw rows | 9,170 | 9,170 |
| Raw digest | `16200879394137992316 / 17871977238653261677` | same |
| Raw LP columns | 13,479 | 13,479 |
| Output rows | 248 | 477 |
| Output LP columns | 18 | 23 |
| Output digest | `10700067927127482413 / 7933828173714541669` | `16984277476308836056 / 7231745490714097264` |
| Structured commits / emitted rows | n/a | 45 / 79 |
| Matrix shape | 248 x 14,648 | 477 x 14,653 |
| Matrix nonzeros | 15,585 | 25,678 |
| Matrix digest | `14525310064104378093 / 16319658707909074699` | `14532202369606426594 / 8411150515085501241` |

The raw LP histogram is also frozen at weight-1/2/3/4+ counts
`13048/370/29/32`. Both routes stop after full thin-matrix construction with
dependency extraction disabled. This closes the planned 100-150-bit
size-transition requirement and proves raw relation identity across the one-lane
and multi-lane schedules. It does not establish a wall-time win or justify
`GNFS_STRUCTURED_FILTER=auto`; the complete bounded 50-digit first-round
comparison remains the promotion boundary.

## Production Telemetry

生产 Pipeline 的 `structured_filter schema=1` 记录覆盖每个 logical generation。它
区分 `owned_snapshot` 与 `direct_ooc_prefix` route，并记录 raw/output identity、LP
histogram、归约 counters/caps、reduction-engine wall time 和 process RSS。

`structured_filter_matrix` 保留非负 `excess` 兼容字段，同时记录有符号
`row_column_delta=rows-cols`、MatrixBuilder wall time 和 nonzeros。只有 raw digest、
参数和 raw rows 完全一致的两个 case 才能用于 baseline 对比。bounded prefix、
synthetic RSS 和完整首轮属于不同 scope，不能互相替代。

direct-OOC 的 after snapshot 在 reducer 返回后、borrowed raw prefix 仍映射时获取。
因此该窗口不包含 reader unmap 和 collector writer resume；它不能表示完整
checkpoint-to-resume route 的 current RSS。

## Promotion Boundary

这些测量不能单独支持 structured auto promotion。自动选路至少还需要同一真实 raw
corpus 上的 legacy/structured 对照、完整首轮关系目标、依赖空间正确性、full matrix
质量、下游耗时和多尺寸重复测量。在这些证据齐全前，structured route 保持显式 opt-in。

## 2026-07-22 Release Evidence

以下数据来自 macOS arm64 的独立 Release 进程。它们用于冻结本次实现证据，不构成
跨机器性能阈值。

测量环境为 macOS 26.5.2、Apple M5（10 个逻辑 CPU、24GiB RAM）和 Apple Clang
21.0.0。源码基线为 `05993af` 加本里程碑改动；`build/CMakeCache.txt` 明确记录
`CMAKE_BUILD_TYPE=Release`。当时的原始记录通过本文列出的 runner 命令生成，并使用
`GNFS_RESOURCE_V1` 或 `GNFS_EXPERIMENT_V1`。当前 50 位双路由 runner 拒绝
`--no-build`，并强制每个成功进程恰好输出一条 `GNFS_EXPERIMENT_V2`。

| Scenario | Source rows | Output rows | Workers | Reduction wall | Peak growth |
|---|---:|---:|---:|---:|---:|
| Synthetic 5K | 4,981 | 4,977 | 1 | 18.83ms | 4,096,000 bytes |
| Synthetic 50K | 49,806 | 49,780 | 4 | 237.32ms | 24,166,400 bytes |
| Synthetic 200K | 199,222 | 199,124 | 4 | 1.56s | 93,159,424 bytes |

真实 50 位 probe 使用 `max_special_q=4`，处理了 4 个 special-Q，得到 188 条 raw
relations。首轮 raw target 为 617,939，所以 `first_round_complete=false`。structured
route 以 `no_candidates` 结束，并在 singleton peeling 后输出 0 行。最终 full matrix
为 0 x 22,660，`row_column_delta=-22660`；探针没有进入 SGE 或 solver。

最终 fail-closed runner 复测的 lifetime peak RSS 为 1,530,101,760 bytes。筛法结束时
current RSS 为 206,848,000 bytes；poly、factor-base 和 sieve wall time 分别为
546ms、462ms 和 1.76s。相同 4-SQ 场景的较早独立进程曾达到 2,680,422,400 bytes
peak 和 1,897,005,056 bytes sieve-end current，说明单次高水位对分配器与进程布局
敏感。peak 还包含 polynomial selection、factor-base、4 路 special-Q sieve、
cofactorization 和 structured reduction，不能把它归因于 reducer，也不能用 bounded
prefix 推断完整首轮内存或 structured 相对收益。

同一构建的 `max_special_q=64` 扩展 probe 处理了 6,047 条 raw relations，structured
route 以 12 次 commit 输出 16 行；full matrix 为 16 x 22,660，nonzeros 为 1,296，
`row_column_delta=-22644`。sieve wall time 为 47.04s，process peak RSS 为
2,318,090,240 bytes，sieve-end current RSS 为 2,313,863,168 bytes。4-SQ 重复值和
单次 64-SQ 值不能组成单调曲线；但 64-SQ 进程在批次结束后仍保留约 2.3GB current
RSS。下一阶段因此需要独立控制 special-Q batch concurrency，并区分每批工作区和
跨批保留状态。

## 2026-07-22 Sieve Region Capacity Fix

源码审计随后确认，`LatticeSieve` 构造器先为约 268.4M-cell 默认 region 分配约
512MiB，再用 `resize()` 缩到 50 位实际 4096 x 2048 region。缩小只改变 size，不会
释放 vector capacity。4 个 worker 加一个未使用的 Pipeline 实例因此可保留约
2.5GiB 筛数组 capacity。

修复将默认 storage 改为 lazy allocation，`set_region()` 用新 vector 加 `swap`
确定释放旧 capacity，并删除未使用的 Pipeline sieve 实例。同一 Release 4-SQ
探针保持以下 identity 完全不变：

```text
raw_rows=188
raw_digest_low=2999840282289098554
raw_digest_high=11378523343223252016
output_rows=0
output_digest_low=12384855047597894612
output_digest_high=7406486012983705512
matrix_rows=0
matrix_cols=22660
matrix_signed_delta=-22660
```

process lifetime peak RSS 从 1,530,101,760 bytes 降到 218,169,344 bytes；sieve-end
current RSS 为 217,825,280 bytes，sieve wall time 从 1.76s 降到 0.82s。完整 sieve
module、Release gate 和 ASan+UBSan 的 lattice-sieve storage 回归均通过。该结果
消除了 1/2/4 worker 对照中的固定 512MiB/实例测量偏差，并为
后续 typed worker cap 与跨批 allocator-retention 实验建立了可比较基线。

## 2026-07-22 Special-Q Batch Worker Evidence

`max_special_q` 的旧批次填充逻辑只在外层检查已完成计数。`max_special_q=1` 仍会一次
取满 4 个 special-Q。修复后，真实 50 位 Release 探针严格处理 1 个 special-Q：
`special_q_batch_count=1`、`special_q_batch_peak_size=1`、
`special_q_batch_peak_workers=1`，raw rows 为 39。

以下数据来自统一预算落地前的 `4b1b242` revision，保留为历史 outer-only 基线。3 个
独立进程都处理同一 4-SQ prefix，得到 188 条 raw relations。raw digest 固定为
`2999840282289098554/11378523343223252016`，output digest 固定为
`12384855047597894612/7406486012983705512`。full matrix 均为 `0 x 22660`，
`matrix_signed_delta=-22660`，row mapping 为 identity。runner 声明的全部非资源
identity 字段一致。

| Outer workers | Peak RSS | Sieve-end current RSS | Sieve wall |
|---:|---:|---:|---:|
| 1 | 85,737,472 bytes | 85,360,640 bytes | 1.41s |
| 2 | 137,314,304 bytes | 136,953,856 bytes | 0.96s |
| 4 | 221,675,520 bytes | 221,331,456 bytes | 0.91s |

这些数据来自 macOS arm64、10 个逻辑 CPU 的 fresh Release 进程。它们说明外层并发
上限能明确控制 4-SQ prefix 的内存与延迟权衡，但不能推断完整首轮或其它机器的最优值。
这些测量早于统一预算实现；当时每个 `LatticeSieve` 都独立使用硬件并发数，因此存在
嵌套过量并行。

同一 runner 的 64-SQ 扩展覆盖 16 个批次。3 个进程都得到 6,047 条 raw relations、
16 条 output relations 和 `16 x 22660` full matrix；runner 声明的 identity 集合仍
完全一致。
raw digest 为 `3689494670318064948/10851036734780297310`，output digest 为
`8861842470919209299/15961672454890669926`。

| Outer workers | Peak RSS | Sieve-end current RSS | Sieve wall |
|---:|---:|---:|---:|
| 1 | 100,810,752 bytes | 86,130,688 bytes | 71.54s |
| 2 | 143,261,696 bytes | 128,548,864 bytes | 50.58s |
| 4 | 269,762,560 bytes | 230,883,328 bytes | 46.74s |

4-worker 的 64-SQ sieve-end current RSS 只比 4-SQ 高约 9.6MB，不再出现修复前约
2.3GB 的跨批残留。该单点不能证明任意长运行都没有 allocator retention，但已覆盖
当前 16 批生产路径。2 workers 到 4 workers 只缩短约 7.6% sieve wall time，同时
增加约 126.5MB lifetime peak RSS；该结果推动了下一节的统一线程预算。

## 2026-07-22 Unified Local Sieve Compute Budget

Pipeline 现在把外层 special-Q worker 与每个 `LatticeSieve` 的内层 fan-out 统一到
`max_local_sieve_threads`。以下数据使用自动冻结的 10 通道预算。该值表示可运行的本地
筛法计算通道，不是 OS 线程数或 RSS 上限。外层线程在内层 fan-out 期间会阻塞，运行库
和显式启用的其它嵌套并行仍可能创建额外线程。表值来自实际 worker 配置回读和 outer
topology fail-closed 校验落地后的最终 Release fresh-process 复测。

4-SQ worker 对照的 raw rows 均为 188，full matrix 均为 `0 x 22660`。runner 声明的
全部 relation、matrix 和生命周期身份字段一致：

| Outer workers | Lane plan | Assigned lanes | Peak RSS | Sieve-end current | Sieve wall |
|---:|---:|---:|---:|---:|---:|
| 1 | 10 | 10 | 86,245,376 bytes | 85,917,696 bytes | 1.386s |
| 2 | 5 + 5 | 10 | 149,209,088 bytes | 148,897,792 bytes | 0.860s |
| 4 | 3 + 3 + 2 + 2 | 10 | 227,377,152 bytes | 227,049,472 bytes | 0.859s |

固定 4 个 outer workers 后再扫描总预算，所有场景仍得到同一 relation digest 和矩阵
身份。预算小于 outer cap 时，worker 数自动下降，避免每个 worker 获得零通道：

| Budget | Effective workers | Peak worker lanes | Peak RSS | Sieve wall |
|---:|---:|---:|---:|---:|
| 1 | 1 | 1 | 83,836,928 bytes | 1.554s |
| 2 | 2 | 1 | 137,904,128 bytes | 0.935s |
| 4 | 4 | 1 | 247,775,232 bytes | 0.837s |
| 10 | 4 | 3 | 228,704,256 bytes | 0.790s |

64-SQ 对照覆盖 16 个 production batches。三组都得到 6,047 条 raw relations、16 条
output relations 和 `16 x 22660` full matrix，摘要与生命周期身份不变：

| Outer workers | Lane plan | Assigned lanes | Peak RSS | Sieve-end current | Sieve wall |
|---:|---:|---:|---:|---:|---:|
| 1 | 10 | 10 | 95,322,112 bytes | 90,472,448 bytes | 69.194s |
| 2 | 5 + 5 | 10 | 204,226,560 bytes | 202,407,936 bytes | 48.501s |
| 4 | 3 + 3 + 2 + 2 | 10 | 342,376,448 bytes | 340,557,824 bytes | 45.181s |

4-SQ 的 budget 4 单次 peak 高于 budget 10，64-SQ 的 RSS 也随 outer workers 增加。
这些现象说明 lifetime peak 对分配器和进程布局敏感，不能据此建立单调预算模型或 CI
阈值。可复现契约是计算通道上限、均衡分配和 bit-for-bit 结果身份；资源值仅作本机
调优证据。

## 2026-07-22 Two-Stage Candidate Work Stealing

Pipeline 随后把本地批次拆成两个不重叠阶段。第一阶段只生成每个 special-Q 的
`SieveResult`；第二阶段把整个批次的候选按 256 个一块切分，由 worker-local
`Cofactorizer` 动态领取。第二阶段复用完整的 10 通道预算，不受 outer worker cap
限制。每个 chunk 写入独立槽位，主线程再按 special-Q 和 candidate 原始顺序归并。

4-SQ 对照均得到 2,284 个候选和 10 个 chunks。candidate 阶段实际使用 10 个
workers。188 条 raw relations、两个 corpus digests、`0 x 22660` full matrix 和工件
生命周期身份与上一节完全一致：

| Outer workers | Peak RSS | Sieve-end current | Candidate generation | Candidate cofactor | Sieve wall |
|---:|---:|---:|---:|---:|---:|
| 1 | 82,870,272 bytes | 82,526,208 bytes | 0.073s | 0.312s | 0.394s |
| 2 | 143,998,976 bytes | 143,671,296 bytes | 0.068s | 0.296s | 0.370s |
| 4 | 239,599,616 bytes | 239,288,320 bytes | 0.072s | 0.333s | 0.413s |

相对上一节的统一预算基线，4-SQ Phase 3 wall 分别缩短约 3.5、2.3 和 2.1 倍；表中的
`Sieve wall` 是历史兼容字段，包含 candidate generation、cofactor 和少量批次开销，
不是单独的筛核计时。在表中这一次 fresh-process 记录里，4-worker peak RSS 增加约
5.4%，另外两个 topology 略降；复跑可能改变方向和幅度，因此不把该差值视为稳定收益
或回归。两阶段不会同时保留 sieve region 与活跃 cofactor workers，但分配器仍可能
保留已释放的线程局部 arena。上述配对 current RSS 字段用于区分生命周期边界，但仍不
表示 storage 的独占内存，不能把顺序阶段理解为 RSS 相加或取最大值的简单模型。

64-SQ 对照覆盖 16 个 production batches。三组都处理 118,311 个候选和 499 个
chunks，单批最多 29,675 个候选；candidate 阶段峰值均为 10 个 workers。输出仍为
6,047 条 raw relations、16 条 output relations 和 `16 x 22660` full matrix，raw 与
output digests 沿用上一节的固定值：

| Outer workers | Peak RSS | Sieve-end current | Candidate generation | Candidate cofactor | Sieve wall |
|---:|---:|---:|---:|---:|---:|
| 1 | 110,034,944 bytes | 108,232,704 bytes | 1.171s | 11.950s | 13.168s |
| 2 | 202,227,712 bytes | 175,652,864 bytes | 1.116s | 12.091s | 13.247s |
| 4 | 316,817,408 bytes | 303,546,368 bytes | 1.087s | 11.906s | 13.038s |

相对统一预算基线，64-SQ Phase 3 wall 分别缩短约 5.3、3.7 和 3.5 倍。在该次记录中，
outer worker 数主要影响候选生成阶段的内存布局；占主导的 cofactor 阶段始终使用同一
预算，因此三组总时间接近。该证据仍是 macOS arm64、10 个逻辑 CPU 上的 bounded
prefix，不能推断完整首轮或其它机器的最优 chunk size 或 candidate worker cap。

## 2026-08-20 and 21 Complete 50-Digit First-Round Evidence

完整首轮对照按 `legacy`、`structured` 的顺序，在两个 fresh Release 进程中执行。
runner 对比并通过了 51 个原始输入与调度身份字段。两条路由共享以下输入证据：

```bash
./scripts/test.sh compare-50d-first-round 8192 4 auto
```

本次二进制由 source commit `2b21694bd8d8decf722567c180f71697e6732156`
构建。后续合入的 PR #68 只更新 GitHub Actions 固定 SHA，不改变该二进制的源文件。

本节历史数字来自当时的 stdout records；该次运行早于 JSON 持久化校验器。自本 PR
及后续运行起，这两个 legacy/structured route scope 使用按 `scope` 分派的
`GNFS_EXPERIMENT_COMPARISON_V2` 协议中的固定 58-field variant。校验器会将成功记录
原子发布到 `build/50d-comparisons/<scope>.json`；该 build artifact 不纳入版本控制。
一次参数合法的新运行会在构建前失效同 scope 的旧快照，只有两条 route、清理合同、
schema 和来源绑定全部通过时才发布替代文件，因此失败运行不会留下陈旧的
`status=pass`。同 scope 的并发 comparison 不在该 latest-snapshot 合同内。JSON 将
`n` 和所有 digest 固定编码为 canonical decimal string；其它超过 IEEE-754
safe-integer 范围的整数也使用该编码。顶层 `legacy_stop` 和 `structured_stop` 均指
sieve route 的终止原因；每条 route 的 `fields.structured_stop` 才是 structured
reduction 的终止原因。

```text
raw_rows=618449
input_lp_columns=576189
raw_digest_low=13981542011392217821
raw_digest_high=10762676124248923769
raw_identity_fields=51
```

`structured` reduction 执行了 1,024 次 commit，并发布了 1,024 行，然后以
`budget_limit` 停止。structured sieve route 以 `effective_column_excess` 完成首轮。
两条 route 都停在 matrix-only 边界，不将该结果表述为 solver 或完整分解证据。

| Metric | Legacy | Structured | Structured Change |
|---|---:|---:|---:|
| Reduction output rows | 6,559 (1.06% of raw) | 267,456 (43.25% of raw) | +260,897 (+3,977.69%; 40.78x) |
| Output LP columns | 4,647 | 169,824 | +165,177 (+3,554.49%; 36.54x) |
| Matrix rows | 6,559 | 211,732 | +205,173 (+3,128.11%; 32.28x) |
| Matrix columns | 27,307 | 188,413 | +161,106 (+589.98%; 6.90x) |
| Matrix signed delta | -20,748 | +23,319 | +44,067; crosses zero |
| Matrix nonzeros | 525,868 | 14,326,278 | +13,800,410 (+2,624.31%; 27.24x) |
| Mean nonzeros per row | 80.18 | 67.66 | -15.61% |
| MatrixBuilder wall time | 41ms | 2,399ms | +2,358ms (+5,751.22%; 58.51x) |
| Route wall time | 1,269,976ms | 1,441,922ms | +171,946ms (+13.54%) |
| Lifetime peak RSS | 613,924,864 bytes | 894,304,256 bytes | +280,379,392 bytes (+45.67%) |

### Structural Feasibility Gain

Legacy reduction 保留了 1.06% 的 raw rows，并从 raw LP 集合中移除了 99.19% 的
columns，但最终矩阵仍缺少 20,748 行。Structured reduction 保留了 43.25% 的
raw rows 和 29.47% 的 raw LP columns。矩阵阶段使用了 211,732 行，比 reduction
output 少 55,724 行，即 20.83%，仍将 nominal signed delta 从 -20,748 提高到
+23,319。该 44,067 行的跨零改善是显著的结构可行性证据。它证明 bounded
structured basis 可以避免 legacy route 的 nominal thin-matrix 结果，但尚未证明
solver 可行性或保证实际 dependency yield。

### NNZ and Resource Cost

结构改善并非矩阵压缩。Structured matrix 的总 nonzeros 是 legacy 的 27.24 倍，
增加 2,624.31%。虽然每行平均 nonzeros 降低 15.61%，但总行数的增加使
MatrixBuilder 时间扩大到 58.51 倍。单次 route wall time 增加 13.54%，lifetime peak
RSS 增加 45.67%，即约 267.39MiB。因此，该结果同时记录了巨大的结构
收益和实质性 NNZ、构建时间与 RSS 代价；不能用其中一类指标替代另一类。

### Bias and Claim Boundary

完整对照耗时 45m13.7s。两条 route 内部记录的 wall time 合计为 45m11.898s。
fresh-process 边界避免了跨 route 的 lifetime RSS 累积，但该次运行只包含一个
`legacy -> structured` 顺序，不能消除温度、调频、allocator 状态或系统调度的顺序
偏差。host 同期运行 Windows VM，其背景负载约为 1.7 个 CPU 核。因此，
`timing_asserted=false` 和 `rss_asserted=false` 是正确的 claim boundary。本次时间与 RSS
只是观测值，不是回归门禁或跨机器性能阈值。

### 2026-08-21 N=2 Interleaved Campaign

正式 campaign 在同一固定 50 位输入上完成了最小交错重复。每条 route 有 2 个
fresh-process 样本，顺序为 `ABBA`。证据绑定 source commit
`103607844c65a5221e3fdafd5291a8364f0d8f51`、source tree
`1274fbfb0297a6384489a1ede1f24f35fbfaec95` 和 Release probe binary SHA-256
`326140d50316f569a662df579e8063b24e2c4464b2dd846626cd69f425e47402`。

4 个 slot 同时记录 route 内部 wall time 和 runner 外层 elapsed time：

| Slot | Route | Route `wall_ms` | Runner `elapsed_ms` |
|---:|---|---:|---:|
| 1 | `legacy` #1 | 1,263,686 | 1,263,808 |
| 2 | `structured` #1 | 1,359,153 | 1,359,205 |
| 3 | `structured` #2 | 1,394,025 | 1,394,321 |
| 4 | `legacy` #2 | 1,235,971 | 1,236,117 |

4 条 route record 的 51 个 raw identity 字段逐字段相等，identity SHA-256 为
`491a3ef8f29cff7f7af59911267665a7fb0d65bda45532f7301f94222223489a`。同一路由的策略
结果也保持稳定。legacy route-stability SHA-256 为
`4b6b1b298acb02049338533a1338ff55882ebb901354788e71e12521bc38241e`，structured 为
`aa62a3c32f58dbc56103685ed240341145f69024e833415953a376b7870b0b3f`。

方向预算先使用精确有理数，再向上取整为整数 ppm。`1,101,423ppm` wall ratio 只按
route record `wall_ms` 的 structured/legacy 精确中位数计算。runner `elapsed_ms` 包含
外层进程生命周期开销，只作生命周期观测，不参与该预算。peak RSS 使用
structured/legacy 中位数比；matrix nonzeros 在同一路由内稳定，因此使用两条路由的
固定结果比：

| Budget | Observed | Limit | Result |
|---|---:|---:|:---:|
| Wall ratio | 1,101,423ppm | 1,200,000ppm | pass |
| Peak RSS ratio | 1,334,535ppm | 1,600,000ppm | pass |
| Matrix nonzeros ratio | 27,243,107ppm | 30,000,000ppm | pass |
| Structured positive signed delta | 2/2 | 2/2 | pass |

独立审计重新执行 closed validator，并复核 source binding、schema、identity、route
stability、方向预算和生命周期。4 个成功 slot 的 staging、stdout capture 与临时 OOC
目录均按合同清理。终态 one-test report 固定为 Release、1/1 pass、0 fail 和 0 skipped。

本轮 claim boundary 仅覆盖 relation reduction 结果与 matrix shape。nonzeros、wall time
和 RSS 只记录该边界的资源代价。所有 probe 都在 MatrixBuilder 后停止，没有进入 SGE、
solver、dependency search、平方根或因子提取。因此，该结果不证明 dependency yield、
solver 可行性、平方根正确性或完整分解。方向预算也只是宽退化边界，不是性能推广门槛；
canonical summary 继续记录
`promotion=false`，不得据此启用 `GNFS_STRUCTURED_FILTER=auto`。

### Decision

早期单次对照完成了 M5 的完整首轮证据采集。正式 N=2 campaign 又完成了最小
fresh-process 交错重复，并在同一输入和 host 上复现 structured 正 signed delta，且通过
预注册的 NNZ、wall time 和 RSS 方向预算。该结论仍只涉及 relation reduction 与 matrix
shape，不能扩展为下游正确性或自动选路结论。

M6 已完成 direct incidence reread elimination、stage telemetry、sealed receipt
adoption、borrowed rank basis 和 validation epoch cache。dense fixture 资源结果与本次
真实 50 位 campaign 属于不同 scope，不能相互替代。runner 继续明示记录
`promotion=false`；禁止将 unset 默认值推广为 `GNFS_STRUCTURED_FILTER=auto`。

下一阶段应扩展到多尺寸、多输入和多主机的交错重复，并单独记录 validation epoch cache
在真实 mutation/validation 序列中的命中范围。下游证据还必须覆盖 solver、dependency
验证、平方根和因子提取。只有这些结果保持可复现，并且资源仍位于预注册预算内，才能
重新审议 auto promotion。
