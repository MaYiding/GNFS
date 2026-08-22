# 筛法 (sieve) 模块 ENV 调优开关

> 本文档收录 `sieve` 模块所有 `GNFS_*` 运行时调优开关 / helper 的详细设计(算法、ENV 解析、bit-for-bit 保证、ROI、集成点、测试)。
>
> 返回: [ENV 开关总览](README.md) · [CLAUDE.md ENV 索引表](../../CLAUDE.md#env-调优开关总览)

---

## LatticeSieve Region Storage Contract

`LatticeSieve` 的默认 region 约为 268.4M 个 cell，即约 512MiB `uint16_t`
storage。构造器现在只冻结默认 region，不立即分配数组；首次处理有效 special-Q
时才按当前 region 分配。`set_region()` 使用新 vector 加 `swap`，所以从大 region
切到小 region 会确定释放旧 capacity，不依赖非强制的 `shrink_to_fit()`。

production Pipeline 在 worker 启动前已经知道 `GNFSParams` region。50 位 region 为
4096 x 2048，只需 16MiB/worker。Pipeline 不再构造未使用的常驻 sieve 实例；
`allocated_sieve_bytes()` 提供只读 capacity 诊断。该改动只修正分配生命周期，不改变
region、special-Q 顺序、筛数组值、候选关系或停止条件。

固定 4-SQ 的 Release 50 位探针在改动前后得到相同 raw/output digest 和矩阵形状。
process lifetime peak RSS 从 1,530,101,760 bytes 降到 218,169,344 bytes，sieve wall
time 从 1.76s 降到 0.82s。该数字用于回归证据，不是跨机器性能阈值。

**集成点**：

- `include/gnfs/sieve/lattice_sieve.hpp`：lazy allocation、region capacity release 和
  `allocated_sieve_bytes()`；
- `src/api/pipeline.cpp`：删除未使用的常驻 `LatticeSieve`；
- `tests/test_lattice_sieve.cpp`：Release 下执行的 lazy/shrink/r=0 storage contract。

---

## Lattice Sieve Numeric Contract

无 skew 的 Gauss/LLL 规约使用精确的双 64-bit limb 算术；GCC/Clang、MSVC x64、
MSVC ARM64 和无宽乘法 intrinsic 的 fallback 必须作出相同的规约与舍入决定。
SkewLLL 的加权度量仍使用 `double`，不属于该 bit-for-bit 整数算术保证。

`SieveRegion` 的 inclusive width/height 必须为正且各自可表示为 `int32_t`，面积必须
可表示为 `size_t` 并可由 `vector<uint16_t>` 分配。默认 region 生成器另外把面积限制为
256 Mi cells。宽度 32768 是 compact row-state 的上界；更宽的合法 region 把全部素数
路由到 region-bucket 路径，而不是直接拒绝。每次初始或 adaptive sieve pass 之前都会
精确检查矩形的四个投影角点；任何 `(a,b)` 超出 `int64_t` 时 fail closed。SkewLLL
还会在浮点转整数前拒绝不能保持有限 norm/dot/quotient 的 skew；分布式 identity
仅接受平方在任意 rounding/FTZ 环境中都为 normal finite 的 binary64 指数域。进入
fixed-width sieve state 的 factor-base 素数范围是 `[2, INT32_MAX]`，且
`log_p <= UINT16_MAX`。
分布式 worker 会在接管 writer authority 前，按 special-Q cap 预检初始格基和零命中
时可到达的完整 adaptive retry 轨迹；实际命中只能提前结束该确定性轨迹。

完整边界、舍入规则、backend 和验证点见
[Lattice Sieve Numeric Contract](../algorithms/lattice-sieve-numeric-contract.md)。

---

## Special-Q Local Compute Budget (Config)

`max_special_q_batch_workers` 和 `max_local_sieve_threads` 是本地 production
Pipeline 的类型化配置，不是 `GNFS_*` ENV。前者限制每批外层 worker 数，默认值为
4，合法范围为 `[1, 4]`。后者限制本地 Pipeline 的计算通道；未配置时使用
`hardware_concurrency`，读取失败时回退到 4。显式值的合法范围为
`[1, UINT32_MAX]`，Pipeline 构造时再钳制到硬件并发数。CLI 的 `--threads N` 设置
同一计算通道预算。

```ini
max_special_q_batch_workers = 2
max_local_sieve_threads = 8
```

```cpp
gnfs::api::Config config;
config.set_max_special_q_batch_workers(2);
config.set_max_local_sieve_threads(8);
```

Pipeline 构造时冻结有效预算 $B$。非空 special-Q 批次的大小为 $Q$，外层配置上限为
$C$ 时，实际 worker 数为：

$$
W = \min(B, C, Q)
$$

预算按商和余数分配。第 $i$ 个 worker 的 `LatticeSieve` 内层通道数为：

$$
t_i = \left\lfloor\frac{B}{W}\right\rfloor + [i < B \bmod W]
$$

因此非空批次满足 $\sum_i t_i = B$，且任意两个 worker 的分配相差不超过 1。
Pipeline 按两个不重叠的阶段执行本地批次：

1. sieve 阶段中，每个外层 worker 持有一个 `LatticeSieve`；
2. 所有 sieve workers 结束并释放 region storage 后，candidate 阶段把每个
   special-Q 的候选按 256 个一块切分，再由 worker-local `Cofactorizer` 动态领取。

`LatticeSieve::set_max_threads()` 统一约束 bucket scatter、bucket apply 和 row-major
阶段的内层并行。candidate 阶段最多启动 $\min(B, K)$ 个 workers，其中 $K$ 是当前
批次的非空 candidate chunks 数。两个阶段顺序复用预算 $B$，不会叠加并行度。

当前 production candidate worker 在 `Cofactorizer` 内串行执行。ECM Stage 1、
ECM Stage 2、Brent-Pollard rho 和 ECM curve pool 的并行 helper 均未接入该路径。
未来接入任一 helper 时，Pipeline 必须显式分配每个 candidate worker 的内层预算。
所有同时活跃 worker 的内层预算总和不得超过 $B$。worker 内不得直接把 ENV 值当作
独立线程上限，否则会形成 candidate worker 数与内层线程数的乘积。

该契约限制本地批次各顺序阶段的计算通道，不限制进程 OS 线程数或 RSS。多通道
`LatticeSieve` 执行时，外层 worker 线程会阻塞等待内层线程；运行库线程以及显式启用
的余因子分解嵌套并行也不计入此预算。candidate worker 数受该预算约束，但预算仍不
等于进程的 OS 线程上限。它不改变
`DistributedSieveConfig::num_workers`，也不约束独立调用的
`LatticeSieve::sieve_parallel()`。distributed route 不填充本地批次遥测。

小于等于 50 位的固定批次宽度仍为 4，大于 50 位仍为 2。实际外层 worker 数为
`min(local_sieve_thread_budget, batch_size, max_special_q_batch_workers)`。调度参数不改变
special-Q 顺序、批次成员、归约输入或 checkpoint identity。`max_special_q` 仍是处理
数量的硬上限；最后一批只取剩余配额，并为该批重新分配完整计算通道预算。

`FactorStats` 提供以下本地调度遥测：

- `local_sieve_thread_budget`：Pipeline 冻结后的有效计算通道预算；
- `special_q_batch_worker_limit`：计算通道预算与外层配置上限的较小值；
- `special_q_batch_peak_workers`：实际同时启动的最多外层 workers；
- `special_q_batch_count`：已执行的本地批次数；
- `special_q_batch_peak_size`：实际最大批次大小；
- `special_q_batch_peak_assigned_threads`：单批分配的最大计算通道总数；
- `special_q_worker_peak_sieve_threads`：单个 worker 获得的最大内层通道数；
- `candidate_batch_peak_workers`：candidate 阶段实际启动的最大 worker 数；
- `candidate_batch_total_chunks`：所有本地批次处理的 candidate chunks 总数；
- `candidate_batch_peak_chunks`：单个批次的最大 candidate chunks 数；
- `candidate_batch_peak_candidates`：单个批次保留的最大候选数；
- `candidate_batch_rss_sample_candidates`：配对 RSS 样本对应的候选数；
- `candidate_batch_after_generation_current_rss_bytes`：Stage A 结束后的 current RSS；
- `candidate_batch_after_cofactor_current_rss_bytes`：Stage B 结束后的 current RSS；
- `candidate_batch_after_release_current_rss_bytes`：批次 storage 析构后的 current RSS；
- `timings.candidate_generation_s`：生成候选的累计 wall time；
- `timings.candidate_cofactor_s`：candidate cofactor 阶段的累计 wall time。

RSS 采样策略为 `first_max_candidates`。Pipeline 只在当前批次候选数严格大于既有样本时
替换配对样本。因此，并列最大值保留最早批次，且三个 RSS 值始终来自同一批次。
三个 current RSS 字段必须全有或全无；不支持的平台不以 0 bytes 伪装成功样本。

`after_generation` 在 Stage A workers 和 worker-local `LatticeSieve` 析构后采样，
但仍保留全部 `SieveResult`。`after_cofactor` 在 candidate workers、chunk scratch
和 worker-local `Cofactorizer` 析构后采样，此时输入与归并前输出仍在内存。
`after_release` 在输出移入 collector，并析构批次输入与输出 storage 后采样。
它表示进程 current RSS，不表示这些对象独占的 bytes，也不保证小于前两个样本。

当 candidate worker 数为 1 时，顺序路径在调用线程执行。ECM 的 thread-local state
可保留到进程结束。allocator 也可能保留已释放页面。因此，三个样本之间不得建立
单调断言，也不得作为跨平台 CI 阈值。

`special_q_batch_peak_assigned_threads` 和 `special_q_worker_peak_sieve_threads` 在
worker join 后从各 `LatticeSieve` 的实际配置值汇总。它们不只复述 planner 的预期
向量；若预算没有写入 worker，Pipeline 会 fail closed。

真实 50 位 Release 探针在固定 10 通道预算下对比 workers 1、2 和 4。runner 声明的
relation、matrix 与生命周期身份集合完全一致；4-SQ 和 64-SQ 两个尺寸都通过。资源值
只作为单机证据，详见 [structured OOC measurement](../perf/structured-ooc-measurement.md)。

**集成点**：

- `include/gnfs/core/params.hpp`：默认值和冻结后的 Pipeline 参数；
- `include/gnfs/api/config.hpp`：配置文件解析、builder、merge 和范围校验；
- `include/gnfs/sieve/local_thread_budget.hpp`：均衡线程分配纯函数；
- `include/gnfs/cofactor/candidate_chunk_plan.hpp`：规范 candidate chunk 规划；
- `include/gnfs/cofactor/candidate_batch.hpp`：确定性 candidate 执行器；
- `src/api/pipeline.cpp`：预算冻结、两阶段批次执行、worker 分配和遥测；
- `tests/test_api.cpp`：类型化配置与公开结果格式；
- `tests/test_local_sieve_thread_budget.cpp`：分配示例、无效输入和性质网格；
- `tests/test_candidate_chunk_plan.cpp`：chunk 覆盖、顺序和溢出契约；
- `tests/test_candidate_batch.cpp`：真实 Special-Q fixture 和跨线程顺序不变性；
- `tests/test_sieve_checkpoint.cpp`：调度参数不进入数学 run identity；
- `tests/test_structured_ooc_50d_probe.cpp`：真实 50 位调度与 identity 证据。

---

## Sieve mid-flight checkpoint (GNFS_RESUME / GNFS_SIEVE_RESUME)

**ENV `GNFS_RESUME=<base_path>`** 启用全流水线恢复；历史名称
`GNFS_SIEVE_RESUME=<base_path>` 仍作为别名。进入 sieve 阶段后，该路径同时
作为 OOC relation store 与 sieve checkpoint 的 base path，不需要再设置
`GNFS_OOC_RELATIONS`。

```bash
# 首次启动 / 续跑同一 path
GNFS_RESUME=/var/tmp/gnfs-session ./gnfs <N>
# 进程崩溃后使用同一路径重启
GNFS_RESUME=/var/tmp/gnfs-session ./gnfs <N>
```

**SieveCheckpoint V3 + OOC V3 配对恢复流程**:

1. 新 OOC store 在 `.relidx` 与 `.reldata` 的 V3 header 中持久化同一个不可变
   `store_id`。`RelationCollector::checkpoint_ooc()` flush 两个 stream，写入 prefix
   sentinel，并返回 `format_version/store_id/generation/count/data_end`；collector
   同时给出从每次成功 `add()` 独立滚动得到的 relation-sequence receipt。offset 与
   `data_end` 都是包含 24-byte data header 的物理文件偏移。
2. `SieveCheckpoint` 把 descriptor、sequence receipt、`sq_count/current_index/round`
   和本次 run identity 写入同目录临时文件；写入 checksum、完整 flush 后以原子
   替换发布。
3. 周期性 checkpoint 发布成功后，collector 才以同一个 descriptor 重新打开
   append。终态 checkpoint 额外写入 `collection_complete=true`，保持该 exact prefix
   suspended，并直接从同一 generation 发布 final magic。
4. 重启先严格加载 V3 checkpoint，再比对 N、多项式、因子基和 sieve 参数的
   128-bit run fingerprint；不一致时在打开 OOC store 前 fail closed。当前 run
   identity schema 3 还绑定 affine-only Special-Q 枚举规则，以及冻结后的 cascade
   V3、3LP、V0 weight/cutoff/residual 和 structured/legacy reduction 选择，因此旧
   schema 或不同语义策略不会跨 checkpoint 恢复。
5. identity 匹配后，再从同一次只读打开校验 OOC V3 index/data header、配对
   `store_id` 与 committed prefix，并重算 checkpoint prefix 的 sequence receipt；
   所有检查通过后才允许截断 checkpoint 之后的未提交 index/data tail。同尺寸改写
   factor payload、异源 `.reldata` 或 AB 保持不变的载荷漂移都会 fail closed。
6. OOC prefix 恢复成功后，才应用 Special-Q 游标。旧 SieveCheckpoint V1/V2、
   V1/V2 OOC descriptor、checksum 错误、路径、store identity 或 receipt 不匹配
   都 fail closed，不会回退到 fresh 并截断证据。
   Finalized V1/V2 只保留普通 reader 的只读兼容，不允许 append recovery 或 corpus
   ownership promotion。
7. 若进程在终态 checkpoint 发布后、OOC final magic 前退出，重启会识别
   `collection_complete`，只重做确定性 reduction/finalize，不再重复 adaptive round
   或追加 relation。若退出发生在 final magic 与 checkpoint 删除之间，重启要求
   finalized corpus 与终态 descriptor/receipt 的 count 和 extent 精确相等，以只读
   方式继续；任何 checkpoint 后 finalized extension 都 fail closed。

**Crash-safety 边界**:

- 普通 `OOCRelationReader` 始终拒绝 incomplete store；只有带配对 V3 descriptor
  的 recovery path 能读取并回滚到已提交前缀。
- checkpoint 发布使用同目录临时文件和替换操作。发布前失败时重新打开已持久化
  OOC prefix 并重试；若替换后目录同步报错，Pipeline 会严格加载正式文件并与
  本次目标逐字段比较，只有目标版本已可见时才按“已发布、耐久性告警”继续。
- OOC prefix checkpoint 和 finalize 会同步 data/index 文件，并在 POSIX 上同步
  父目录。进程崩溃矩阵覆盖 prefix、checkpoint 临时态/发布态、append tail、
  terminal publication、finalize metadata 与 final magic；文件系统和硬件仍决定
  断电耐久性的最终边界。
- 同进程 checkpoint/resume 只重验 paired header、精确 extent、首 offset 与
  sentinel，并读取 collector 已滚动维护的 receipt，保持 O(1) checkpoint 边界；
  final precommit 与进程重启恢复才完整扫描 offset/record，避免固定 checkpoint
  周期对增长中 relation index 造成二次复杂度。
- 测试用 self-exec 子进程在 typed save stage 调用 `std::_Exit()`，避免析构自动
  finalize 造成“伪崩溃”。这些测试证明进程退出一致性；不把它表述为完整断电证明。

**集成点**:

- `include/gnfs/sieve/sieve_checkpoint.hpp` — V3 wire format、receipt、checksum、原子发布
- `include/gnfs/sieve/sieve_run_identity.hpp` — portable run identity
- `include/gnfs/relation/ooc_relation_format.hpp` — 轻量 V3 format contract
- `include/gnfs/relation/ooc_relation_store.hpp` — paired V3 identity、prefix rollback
- `include/gnfs/relation/relation_sequence_receipt.hpp` — constant-memory accepted-sequence receipt
- `include/gnfs/relation/collector.hpp` — paired resume descriptor 与 recovery outcome
- `src/api/pipeline.cpp` — fail-closed load 与 prefix/checkpoint/reopen 顺序
- `tests/test_sieve_checkpoint.cpp` — 格式、原子发布与真实子进程 crash 边界
- `tests/test_ooc_store_integrity.cpp` — prefix、tail、identity、finalized-corpus 校验

**触发条件**: 50d+ sieve 持续 hours+ 且存在 OOM、进程退出或重启风险。对短任务
通常不值得启用。该模式与 `GNFS_OOC_RELATIONS` 不叠加，resume path 优先。
同一 base path 目前只支持单个活跃进程；并发 writer 的进程级 lease 尚未实现。

---

## Sieve bucket prefetch (GNFS_BUCKET_PREFETCH)

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

---

## Sieve region tile bits (GNFS_SIEVE_REGION_TILE_BITS)

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

---

## Sieve norm tile bits (GNFS_SIEVE_NORM_TILE_BITS)

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

---

## Sieve apply-tile 并行 (GNFS_SIEVE_APPLY_TILE_THREADS)

**ENV `GNFS_SIEVE_APPLY_TILE_THREADS=N`** (2026-05-22 实施, W12 T4, default 1, range [1, hardware_concurrency * 2]):
Lattice sieve `sieve_bucket_region` apply phase 把 `sieve_array_` 扫描产生
candidate (a, b) pair. W6 `GNFS_SIEVE_REGION_TILE_BITS` 把 row range 切成
`2^N` 行 tile 提高 L1/L2 命中, 但 tile 之间是 **sequential cache-blocking**
(顺序扫). 本 helper 是与 W6 完全 orthogonal 的轴: **parallel work
distribution** —— caller 决定好 tile 总数后 (典型 `ceil(rows /
region_tile_size_rows())`), `parallel_apply_tiles` 把不同 tile 派到不同
worker 并行执行. N=1 (默认) 走 sequential per-tile 循环, 不创建 ThreadPool,
零开销保留原行为. N>=2 时把 K 个 tile dispatch 到大小为 min(N, K) 的
ThreadPool, tile 之间靠 future 同步收口.

```bash
GNFS_SIEVE_APPLY_TILE_THREADS=1 ./gnfs <N>   # default sequential, zero overhead
GNFS_SIEVE_APPLY_TILE_THREADS=4 ./gnfs <N>   # 4 outer workers for apply tiles
GNFS_SIEVE_APPLY_TILE_THREADS=8 ./gnfs <N>   # 8 outer workers
unset GNFS_SIEVE_APPLY_TILE_THREADS          # same as N=1

# 与 W6 region_tile 同开 (二者完全独立: region_tile 控制 tile 大小,
# apply_tile_parallel 控制 tile 之间的并发度)
GNFS_SIEVE_REGION_TILE_BITS=6 GNFS_SIEVE_APPLY_TILE_THREADS=4 ./gnfs <N>
```

**并行模型**:
- Outer = `parallel_apply_tiles<Result, TileFn>(tile_count, tile_fn)` over
  K 个 tile (caller 决定 K, 典型来自 `region_tile_size_rows()` 切分)
- 内部 per-tile work 算法 bit-identical (helper 仅改变外层 dispatch,
  不触碰 `sieve_bucket_region` 内核, 也不修改
  `src/sieve/lattice_sieve.cpp` 主 sieve loop)
- 每个 tile task 拥有独立 Result buffer (caller 选择 Result 类型,
  典型 candidate emit buffer / `std::vector<Candidate>` / 小 (idx,
  count) record), 共享只读 state 通过 lambda capture 引用
- 空 batch (n==0) / 单 tile (n==1) 都走 sequential 短路, 不创建 pool
- Exception path: dispatcher drain 全部 future, 第一个 thrown exception
  通过 `std::rethrow_exception` 传给 caller (不 swallow); pool 析构干净 join

**Bit-for-bit guarantee**: 每 tile work 是 pure function of `tile_index`,
不依赖 dispatch 顺序. Sequential (N=1) 与 parallel (N>=2) 路径产生的
per-index `Result` 完全一致, downstream candidate list 严格相同. 由
`tests/test_sieve_apply_tile_parallel.cpp` 强制覆盖 (100-tile mock_scan
+ HeavyTileResult heavy_scan N=1 vs N=4 vs N=hw_concurrency 严格 per-index
bit-identical assert + move-only `std::unique_ptr` Result 流转测试).

**与家族成员关系**: parallel-dispatcher 家族第六位:
- W7 `GNFS_SQRT_HENSEL_THREADS` — Hensel lift K-prime slot
- W8 T1 `GNFS_ECM_STAGE2_PARALLEL` — ECM Stage 2 BSGS 多曲线
- W9 T1 `GNFS_ECM_STAGE1_PARALLEL_THREADS` — ECM Stage 1 Lucas-chain 多曲线
- W10 T4 `GNFS_FILTER_MERGE_THREADS` — LP-key bucket merge
- W11 T3 `GNFS_MPZ_POWM_BATCH_THREADS` — batched `mpz_powm`
- W11 T4 `GNFS_LATTICE_BASIS_PARALLEL_THREADS` — basis reduction 多基
- **W12 T4 `GNFS_SIEVE_APPLY_TILE_THREADS` — apply-tile 并行 (本 helper)**

七者全部 default 1 (sequential), opt-in, 互不冲突. 可同时启用.

**ROI 与定位**:
- 主要 ROI: 50d+/60d sieve 主循环每 region 可能切几十到几百 row tile,
  per-tile apply scan 内部 cache miss + candidate threshold check 比较密集.
  K tile 并发后 outer wall ~ T_max_tile + tasking overhead, 替代 sum(K)
  sequential 累计. 真实 wall ROI 在 50d+/60d 大 region 上体现.
- W6 region_tile (sequential cache-blocking) 与本 helper (parallel work
  distribution) 完全 orthogonal — region_tile 把 sieve_array_ 工作集控
  在 L1/L2 内, apply_tile_parallel 把不同 tile 跨核并行. caller 可同时
  启用; 二者结合典型: `region_tile_size_rows()` 算 tile size →
  `(rows + size - 1) / size` 算 tile count → `parallel_apply_tiles`
  dispatch.
- helper 是 opt-in 工具, **不修改** `src/sieve/lattice_sieve.cpp` 主
  sieve loop. 调用方需要在 apply phase 入口聚合 tiles 后传 tile_count +
  tile_fn lambda 才生效, 是 future-infrastructure landing.
- Default OFF (N=1) 保证 zero behavior change for legacy callers, 仅当
  调用方 wire-in helper 且用户明确 `GNFS_SIEVE_APPLY_TILE_THREADS=N>=2`
  时启用.

**集成点** (2026-05-22, W12 T4):
- `include/gnfs/sieve/apply_tile_parallel.hpp` — `sieve_apply_tile_threads()`
  env reader with `std::once_flag` cache + `resolve_sieve_apply_tile_threads(tile_count)`
  helper + `parallel_apply_tiles<Result, TileFn>` template dispatcher +
  `sieve_apply_tile_threads_reset_env_cache_for_testing()` test hook
- `tests/test_sieve_apply_tile_parallel.cpp` — 15 个测试 (5 env parsing /
  empty tile_count / single tile N=1 / single tile N=4 no-stall / 100 tile
  N=1 baseline / N=1 vs N=4 simple parity / N=1 vs N=hw HeavyTileResult
  parity / move-only `unique_ptr<int>` Result / tile_fn exception propagation /
  reset env cache hook / resolve helper edge cases)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  sieve 模块

---

## Lattice coordinate SIMD batch (GNFS_LATTICE_COORDS_SIMD)

**ENV `GNFS_LATTICE_COORDS_SIMD=auto|0|1`** (2026-05-22 实施, W13 T4, default auto):
Lattice sieve `sieve_bucket_region` 把每个 sieve cell `(i, j)` 投影到 lattice
坐标 `(a, b)` 时, 每 cell 计算 `a = b1x*i + b2x*j` 与 `b = b1y*i + b2y*j`
(`b1`, `b2` 是 reduced basis vectors, int64). 当前 caller (norm 预计算 + candidate
emission) per-cell 走 scalar 算这两个数, basis `(b1, b2)` 在整个 row tile 内
被 broadcast 时 per-iteration address-gen pressure 高. helper 提供 batched
kernel: NEON 2-lane (ARM64) / AVX2 4-lane (x86_64) 一次性 load K 个 cell 的
`(i, j)`, 在 scalar GPR 上跑 `int64` mul-add (NEON 无 `vmulq_s64`, AVX2 无
`_mm256_mullo_epi64` 除非 AVX-512 DQ), 再 SIMD store `(a, b)`. SIMD value 在
consolidated load / store + 减小 address-gen pressure, 不在 vector mul.

```bash
GNFS_LATTICE_COORDS_SIMD=auto ./gnfs <N>   # 默认: NEON/AVX2 可用则启用
GNFS_LATTICE_COORDS_SIMD=0    ./gnfs <N>   # 强制 scalar (回归 bisect 用)
GNFS_LATTICE_COORDS_SIMD=off  ./gnfs <N>   # 同 0
GNFS_LATTICE_COORDS_SIMD=1    ./gnfs <N>   # 强制 SIMD (无 SIMD 平台 fallback)
GNFS_LATTICE_COORDS_SIMD=on   ./gnfs <N>   # 同 1
unset GNFS_LATTICE_COORDS_SIMD             # 同 auto
```

**Helper API** (`include/gnfs/sieve/lattice_coords_simd.hpp`):
- `struct LatticeBasis { int64_t b1x, b1y, b2x, b2y; }` — compact basis
  descriptor 传值, 每 lane 在 inner loop 内复用 4 个 scalar.
- `batch_lattice_coords(basis, i_coords, j_coords, a_out, b_out)` — 主入口,
  per-cell `a_out[k] = b1x*i_coords[k] + b2x*j_coords[k]`, `b_out[k] =
  b1y*i_coords[k] + b2y*j_coords[k]`. SIMD path 当
  `lattice_coords_simd_enabled()` 为 true 时启用. defensive clamp 到
  `min(i_coords.size(), j_coords.size(), a_out.size(), b_out.size())`.
- `batch_lattice_coords_scalar(basis, i, j, a, b)` — scalar reference (test
  golden + 无 SIMD fallback).
- `lattice_coords_simd_mode()` — 返回 `LatticeCoordsSimdMode { Auto, ForceOff, ForceOn }`.
- `lattice_coords_simd_enabled()` — 三态 dispatcher decision.
- `lattice_coords_simd_supported()` — compile-time `__ARM_NEON / __AVX2__` 探测.
- `lattice_coords_simd_reset_env_cache_for_testing()` — 测试专用 re-resolve ENV.

**算法** (NEON 2-lane / AVX2 4-lane):
- NEON: `vld1q_s64(2 cells)` 读 i + j 各 16 字节, 提取到 GPR scalar, 每 lane
  跑 `int64 * int64 + int64 * int64` mul-add 在 register file, `vst1q_s64`
  consolidated store 写 a + b. Tail scalar fallback 处理非 2 倍数 size.
- AVX2: `_mm256_loadu_si256(4 cells)` 读 i + j 各 32 字节, 提取 4 lane scalar,
  4 个 lane 的 mul-add 在 GPR 并发 (compiler 排度独立), `_mm256_storeu_si256`
  store a + b. Tail scalar.

**Bit-for-bit guarantee**: 每 cell `(a, b)` 是 `(i, j) + basis` 的 fixed
linear combination, SIMD path 与 scalar 内核做相同的 `int64` mul / add, 仅
batched 在 SIMD load/store. 无 int64 overflow 时 (caller responsibility,
典型 sieve region 远低于 int64 上限), 输出严格 per-index 一致. signed wrap-
around 在两条 path 一致 (`-fwrapv` 默认). Empty input 留 outputs 不变. 单元
测试 `tests/test_lattice_coords_simd.cpp` 15 个测试强制覆盖 (4 ENV 解析 +
empty / single cell / identity basis / realistic 5-cell hand check / random
100 / random 1000 / ForceOff vs Auto parity / unaligned 33 tail / negative
coords + basis / undersized a_out clamp / 1M cells perf info).

**ROI 与定位**:
- 主要 ROI: 当前主路径每 cell 计算两次 `b1*i + b2*j` 时 per-iteration
  address-gen 紧, basis 4 个 scalar 在 inner loop 被反复 load. helper 把 K
  cell 一次 SIMD load, basis 仅 4 个 GPR scalar 在 lane 间复用. M5 ARM64
  实测 1M cell: scalar 3.9 ms, SIMD 7.0 ms (Apple Silicon 整数管线 4-way
  superscalar 已对 scalar mul-add 高度优化, SIMD load/store 多走的几条 NEON
  instruction 反而增加 latency). 真正 ROI 在 x86_64 AVX2 平台或当 caller 把
  helper wire-in 到 norm 预计算 hot loop (那里 polynomial coefficient
  pressure 已挤满 GPR, basis 复用让 SIMD 加载摊销显著).
- helper 当前 standalone (主 pipeline `sieve_bucket_region` / candidate
  emission 未 wire-in), 是 future-infrastructure. wire-in 时 caller 把
  inner per-cell 计算切到 `batch_lattice_coords` + 在外层聚合 `(i, j)`
  span.
- 与 W7 `GNFS_BUCKET_PREFETCH` / W6 `GNFS_SIEVE_REGION_TILE_BITS` /
  W6 `GNFS_SIEVE_NORM_TILE_BITS` / W11 `GNFS_LATTICE_BASIS_PARALLEL_THREADS` /
  W12 T4 `GNFS_SIEVE_APPLY_TILE_THREADS` 完全 orthogonal: 各自解决不同
  sieve hot site (prefetch / cache tile / norm tile / basis reduce dispatch /
  apply-tile dispatch / coord projection). 可同时启用而不冲突.

**集成点** (2026-05-22, W13 T4):
- `include/gnfs/sieve/lattice_coords_simd.hpp` — helper API + 三态 ENV gate +
  `LatticeBasis` struct + NEON / AVX2 inner kernels + scalar reference.
- `tests/test_lattice_coords_simd.cpp` — 15 instant tier tests, TIMEOUT 60.
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  sieve 模块.

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点, ENV
对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

---

## Sieve threshold count SIMD (GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD)

**ENV `GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD=auto|0|1`** (2026-05-23 实施, W14 T4, default auto):
Lattice sieve `sieve_array_` (uint8_t log residuals) 批量 threshold 比较
helper. 计算 `count_above_threshold_u8(values, threshold)` 即
`count(i: values[i] >= threshold)`. 应用场景: lattice sieve apply phase
估算 candidate 数 (workload telemetry), threshold pre-screen (在交付候选
列表之前先 cheap-count 一次防止下游 cofactor cascade 被假阳性灌爆).
helper-only future-infra, 主 sieve loop 未 wire-in.

```bash
GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD=auto ./gnfs <N>   # 默认: NEON/AVX2 可用则启用
GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD=0    ./gnfs <N>   # 强制 scalar (回归 bisect 用)
GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD=off  ./gnfs <N>   # 同 0
GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD=1    ./gnfs <N>   # 强制 SIMD (无 SIMD 平台 fallback)
GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD=on   ./gnfs <N>   # 同 1
unset GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD             # 同 auto
```

**Helper API** (`include/gnfs/sieve/threshold_scan_simd.hpp`):
- `count_above_threshold_u8(values, threshold)` — 主入口, 返回
  `count(i: values[i] >= threshold)`. 空 span 直接 return 0.
- `count_above_threshold_u8_scalar(values, threshold)` — scalar reference
  (test golden + 无 SIMD fallback).
- `threshold_scan_simd_mode()` — 返回 `ThresholdScanSimdMode { Auto,
  ForceOff, ForceOn }`.
- `threshold_scan_simd_enabled()` — 三态 dispatcher decision (ForceOff →
  false, ForceOn/Auto + supported → true, 否则 false).
- `threshold_scan_simd_supported()` — compile-time `__ARM_NEON / __AVX2__`
  探测.
- `threshold_scan_simd_reset_env_cache_for_testing()` — 测试专用
  re-resolve ENV.

**算法**:
- NEON 16-lane: `vld1q_u8(16 bytes)` → `vcgeq_u8(v, broadcast(t))` →
  `vandq_u8(pass_mask, 0x01)` → `vpaddlq_u8 → uint16x8_t` (pairwise
  widening add, 单 chunk 总和最大 16, 安全 < 2^16) → `vaddvq_u16`
  (horizontal sum) per chunk. Tail scalar `>=`.
- AVX2 32-lane: `_mm256_cmpgt_epi8` 是 signed 比较, 必须经 sign-bias
  XOR 0x80 把 unsigned 转 signed. 因为 NEON path 用 `vcgeq_u8` (>=)
  语义, AVX2 这边用 `cmpgt(v, t-1)` 等价于 `v >= t`. 当 `threshold == 0`
  时显式 short-circuit 32 避免 `t-1 = 0xFF` 的下溢. `_mm256_movemask_epi8`
  → 32-bit bitmask → `__builtin_popcount` per chunk. Tail scalar `>=`.

**Bit-for-bit guarantee**: `popcount({values[i] >= threshold})` 是
deterministic 函数 of input bytes. SIMD path 与 scalar 路径返回值
严格相等 (`size_t == size_t`, 不容忍单 byte 差异). 空 input 返回 0,
threshold == 0 返回 size, threshold == 255 返回 0xFF byte 数. 单元
测试 `tests/test_threshold_scan_simd.cpp` 14 个测试强制覆盖 (4 ENV /
empty / single byte below/at/above / aligned 32/64 / unaligned 33/65 /
random 1024 + threshold sweep / edge cases — all-equal/all-below/
all-above/t=0/t=255 / ForceOff vs Auto parity / 1M-byte perf info).

**ROI 与定位**:
- 主要 ROI: 大 sieve region (50d+/60d 几万 byte sieve_array_) 上
  apply-scan 的候选预估或 threshold pre-screen. M5 ARM64 实测 1M byte
  scan: scalar 3.68 ms vs SIMD 2.14 ms → ~1.7x speedup (NEON 16-lane
  + `CNT` 流水线效率较高, 与 W9 `popcount_simd` 在 uint64 输入路径上
  Apple Silicon autovectorise 的"持平"行为不同; 这里 uint8 输入 +
  比较语义让 compiler autovectorise 不发生, 故 SIMD 实际胜出更明显).
  x86_64 AVX2 上 32-lane + movemask + popcount 单 chunk overhead 极低,
  ROI 更显著.
- helper 当前 standalone (主路径 `sieve_bucket_region` 的 apply scan 未
  wire-in), 是 future-infrastructure. wire-in 时调用方在 apply scan 入口
  或 telemetry probe 处直接调 `count_above_threshold_u8(span, threshold)`.
- 与 W6 region_tile / W6 norm_tile / W7 bucket_prefetch / W11 lattice
  basis parallel / W12 T4 apply_tile_parallel / W13 T4 lattice_coords_simd
  完全 orthogonal: 各自解决不同 sieve hot site (cache tile / prefetch /
  basis reduce / apply-tile dispatch / coord projection / 本 helper 是
  threshold count primitive). 可同时启用而不冲突.

**集成点** (2026-05-23, W14 T4):
- `include/gnfs/sieve/threshold_scan_simd.hpp` — helper API + 三态 ENV
  gate + NEON 16-lane / AVX2 32-lane inner kernels + scalar reference.
- `tests/test_threshold_scan_simd.cpp` — 14 instant tier tests, TIMEOUT 60.
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  sieve 模块.

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点, ENV
对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

---

## Sieve uint8 saturated subtract SIMD (GNFS_SIEVE_SATURATED_SUB_SIMD)

**ENV `GNFS_SIEVE_SATURATED_SUB_SIMD=auto|0|1`** (2026-05-23 实施, W15 T4, default auto):
Lattice sieve `sieve_array_` (uint8_t log residuals) 批量 in-place 饱和减法
helper. 计算 `values[i] = max(0, int(values[i]) - bias)` for all `i`. 应用
场景: sieve apply phase 后对 `sieve_array_` 做 uniform bias 减法 + 零饱和,
candidate filter pre-pass, threshold pre-screen 之前的 baseline 调整. NEON
`vqsubq_u8` (ARM64) 与 AVX2 `_mm256_subs_epu8` (x86_64) 都是 single CPU
instruction per lane, 无 compare-then-branch 尾部 — scalar 路径 per byte
需要 `cmp + branch + sub`. helper-only future-infra, 主 sieve loop 未
wire-in.

```bash
GNFS_SIEVE_SATURATED_SUB_SIMD=auto ./gnfs <N>   # 默认: NEON/AVX2 可用则启用
GNFS_SIEVE_SATURATED_SUB_SIMD=0    ./gnfs <N>   # 强制 scalar (回归 bisect 用)
GNFS_SIEVE_SATURATED_SUB_SIMD=off  ./gnfs <N>   # 同 0
GNFS_SIEVE_SATURATED_SUB_SIMD=1    ./gnfs <N>   # 强制 SIMD (无 SIMD 平台 fallback)
GNFS_SIEVE_SATURATED_SUB_SIMD=on   ./gnfs <N>   # 同 1
unset GNFS_SIEVE_SATURATED_SUB_SIMD             # 同 auto
```

**Helper API** (`include/gnfs/sieve/saturated_sub_simd.hpp`):
- `saturated_sub_u8_batch(values, bias)` — 主入口, `values[i] = max(0,
  int(values[i]) - bias)` for all `i`. Empty span 直接 return (no-op),
  `bias == 0` short-circuit (identity, 零写入).
- `saturated_sub_u8_batch_scalar(values, bias)` — scalar reference (test
  golden + 无 SIMD fallback).
- `saturated_sub_simd_mode()` — 返回 `SaturatedSubSimdMode { Auto,
  ForceOff, ForceOn }`.
- `saturated_sub_simd_enabled()` — 三态 dispatcher decision (ForceOff →
  false, ForceOn/Auto + supported → true, 否则 false).
- `saturated_sub_simd_supported()` — compile-time `__ARM_NEON / __AVX2__`
  探测.
- `saturated_sub_simd_reset_env_cache_for_testing()` — 测试专用
  re-resolve ENV.

**算法**:
- NEON 16-lane: `vld1q_u8(16 bytes)` → `vqsubq_u8(v, broadcast(bias))`
  → `vst1q_u8(16 bytes)`. Tail scalar `v >= bias ? v - bias : 0`.
- AVX2 32-lane: `_mm256_loadu_si256(32 bytes)` →
  `_mm256_subs_epu8(v, broadcast(bias))` → `_mm256_storeu_si256(32
  bytes)`. Tail scalar. `_mm256_subs_epu8` 是 native unsigned
  saturating subtract, 无需 W14 T4 threshold scan 那种 sign-bias trick
  (因为 NEON 与 AVX2 都直接提供 unsigned saturating subtract intrinsic).

**Bit-for-bit guarantee**: `max(0, int(values[i]) - bias)` 是
deterministic 函数 of input byte 与 bias. SIMD path 与 scalar 路径
mutate 后的 span 字节严格相等 (`uint8_t == uint8_t`, 不容忍单 byte 差异).
空 span 不写, bias=0 不写 (零 memory traffic), bias=255 把每个 byte
collapse 到 0 (包括 `v == 255` 因为 `255 - 255 = 0`). 单元测试
`tests/test_saturated_sub_simd.cpp` 14 个测试强制覆盖 (4 ENV / empty /
single byte 3 values × 3 biases = 9 sub-cases / bias=0 zero-write
contract / bias=255 collapse / aligned 32 / unaligned 33 / unaligned 65
/ random 1000 + bias sweep / ForceOff vs Auto parity / 1M-byte perf
info).

**ROI 与定位**:
- 主要 ROI: 大 sieve region (50d+/60d 几万 byte sieve_array_) 上 uniform
  bias adjustment, scalar 路径 per byte 走 `cmp + branch + sub` 受
  branch prediction 影响 (当 `v >= bias` 分布约 50% 时最严重). M5 ARM64
  实测 1M byte: scalar 6.14 ms (6.14 ns/byte) vs SIMD 0.60 ms (0.60
  ns/byte) → **10.21x speedup**. 这是 W14/W15 sieve SIMD helper family
  中最显著的 — saturating subtract 是 SIMD 一条指令最经典场景 (intrinsic
  直接提供 unsigned saturating semantics, 无 sign-bias trick 开销).
  x86_64 AVX2 上 32-lane subs_epu8 应该有类似或更显著的加速.
- helper 当前 standalone (主路径 `sieve_bucket_region` 与候选预筛选未
  wire-in), 是 future-infrastructure. wire-in 时调用方在 sieve_array_
  uniform bias 调整入口或 candidate pre-screen 处直接调
  `saturated_sub_u8_batch(span, bias)`.
- 与 W6 region_tile / W6 norm_tile / W7 bucket_prefetch / W11 lattice
  basis parallel / W12 T4 apply_tile_parallel / W13 T4 lattice_coords_simd
  / W14 T4 threshold_scan_simd 完全 orthogonal: 各自解决不同 sieve hot
  site (cache tile / prefetch / basis reduce / apply-tile dispatch /
  coord projection / threshold count / 本 helper 是 saturated subtract
  primitive). 可同时启用而不冲突.

**集成点** (2026-05-23, W15 T4):
- `include/gnfs/sieve/saturated_sub_simd.hpp` — helper API + 三态 ENV
  gate + NEON 16-lane `vqsubq_u8` / AVX2 32-lane `_mm256_subs_epu8` inner
  kernels + scalar reference.
- `tests/test_saturated_sub_simd.cpp` — 14 instant tier tests, TIMEOUT 60.
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  sieve 模块.

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点, ENV
对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

---

## Lattice basis reduction 多基并行 (GNFS_LATTICE_BASIS_PARALLEL_THREADS)

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
- 每个 basis task 拥有独立 Result slot。helper 本身不调用 GMP；若 caller 的
  `reduce_fn` 使用 `Integer` / `mpz_*`，其操作数和 scratch 必须 per-task 独立
- 空 basis span (n==0) / 单 basis (n==1) 都走 sequential 短路, 不创建 pool
- Exception path: dispatcher drain 全部 future, 第一个 thrown exception
  通过 `std::rethrow_exception` 传给 caller (不 swallow); pool 析构干净 join

**Bit-for-bit guarantee**: 每 basis reduction 是 pure function of basis content,
不依赖 dispatch 顺序. Sequential (N=1) 与 parallel (N>=2) 路径产生的
per-index `Result` 完全一致, downstream sieve region 设置严格相同. 由
`tests/test_lattice_basis_parallel.cpp` 强制覆盖 (100-basis mock_reduce
N=1 vs N=4 vs N=hw_concurrency 严格 per-index bit-identical assert).

**ROI 与定位**:
- 主要 ROI: 50d+/60d sieve 主循环每 batch 可能有数百到上千 Special-Q。
  当前 unskewed reducer 使用精确 fixed-width limb 算术，SkewLLL 使用 `double`
  加权度量；generic caller 也可在 `reduce_fn` 中使用独立 GMP 数据。K basis 并发后
  outer wall 约为最慢 basis 的时间加 tasking overhead，而不是所有 basis 时间之和。
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
