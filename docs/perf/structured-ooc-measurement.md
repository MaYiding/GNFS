# Structured OOC Measurement

本文定义 structured ordinary-OOC route 的可复现资源测量和真实 50 位有界探针。
这些入口提供证据，不设置性能通过阈值，也不改变 `GNFS_STRUCTURED_FILTER` 的默认
策略。

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

## Bounded Real 50-Digit Probe

真实探针固定输入：

```text
16000000000000004000000216000000000000027000000729
```

该数为 50 位、164 bit。探针调用公开 Pipeline phase API：
`select_polynomial()`、`build_factor_base()`、`sieve_and_collect()` 和
`solve_matrix()`。它强制显式 ordinary OOC 与 structured route，并关闭 resume、
distributed sieve、V0 BFS、V3 cascade、3LP 和其它会改变实验族的开关。

```bash
./scripts/test.sh probe-50d-structured-ooc
./scripts/test.sh probe-50d-structured-ooc 8
```

可选参数是 `max_special_q`，允许范围为 1 到 64，默认值为 4。`Config` 的
`max_special_q` 字段把该限制写入 `GNFSParams::max_special_q`，因此限制在进程内
生效，不依赖 shell timeout。默认 4 个 special-Q 对应一个 production batch，但仍
只是 bounded prefix，不代表完整首轮。

探针在调用 `solve_matrix()` 前机械证明 reduced rows 不超过已知 non-LP factor-base
columns，并设置 `GNFS_NO_THIN_SOLVE=1`。实际 full matrix 必须满足
`row_column_delta < 0`，否则 fail closed。Pipeline 只构建 full matrix，然后在 thin
分支返回；探针不进入 SGE、Block Lanczos、Block Wiedemann、平方根或因子提取。

成功时输出一行 `GNFS_EXPERIMENT_V1`。报告必须使用以下 claim boundary：

```text
scope=bounded_50d_prefix_probe
resume_scope=none
route_evidence=production_direct_ooc
sge_attempted=false
solver_attempted=false
sqrt_attempted=false
factorization_attempted=false
```

`first_round_complete` 只在 raw relation count 达到初始 raw target 时为 true。报告还
包含 hard cap、实际 special-Q 数、raw/output digest 与 rows、矩阵 rows/columns、
有符号 row-column delta、nonzeros、wall time 和 process RSS。runner 使用独立临时目录；
成功后只在目录为空时执行 `rmdir`，失败或生命周期异常时保留目录供诊断。

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
`CMAKE_BUILD_TYPE=Release`。原始记录通过本文列出的 runner 命令生成，runner 会
拒绝 `--no-build` 并强制每次成功运行恰好输出一条 `GNFS_RESOURCE_V1` 或
`GNFS_EXPERIMENT_V1`。

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
module 为 17/17，Release gate 为 132/132，ASan+UBSan 的 lattice-sieve storage
回归也通过。该结果消除了 1/2/4 worker 对照中的固定 512MiB/实例测量偏差，但不替代
后续 typed worker cap 和跨批 allocator-retention 实验。
