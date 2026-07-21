# 筛法 (sieve) 模块 ENV 调优开关

> 本文档收录 `sieve` 模块所有 `GNFS_*` 运行时调优开关 / helper 的详细设计(算法、ENV 解析、bit-for-bit 保证、ROI、集成点、测试)。
>
> 返回: [ENV 开关总览](README.md) · [CLAUDE.md ENV 索引表](../../CLAUDE.md#env-调优开关总览)

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

**SieveCheckpoint V2 + OOC V3 配对恢复流程**:

1. 新 OOC store 在 `.relidx` 与 `.reldata` 的 V3 header 中持久化同一个不可变
   `store_id`。`RelationCollector::checkpoint_ooc()` flush 两个 stream，写入 prefix
   sentinel，并返回 `format_version/store_id/generation/count/data_end`；offset 与
   `data_end` 都是包含 24-byte data header 的物理文件偏移。
2. `SieveCheckpoint` 把该 descriptor、`sq_count/current_index/round` 和本次
   run identity 写入同目录临时文件；写入 checksum、完整 flush 后以原子替换发布。
3. checkpoint 发布成功后，collector 才以同一个 descriptor 重新打开 append。
4. 重启先严格加载 V2 checkpoint，再比对 N、多项式、因子基和 sieve 参数的
   128-bit run fingerprint；不一致时在打开 OOC store 前 fail closed。
5. identity 匹配后，再从同一次只读打开校验 OOC V3 index/data header、配对
   `store_id` 与 committed prefix；所有检查通过后才允许截断 checkpoint 之后的
   未提交 index/data tail。同尺寸异源 `.reldata` 也会 fail closed。
6. OOC prefix 恢复成功后，才应用 Special-Q 游标。V1/V2 OOC descriptor、checksum
   错误、路径或 store identity 不匹配都 fail closed，不会回退到 fresh 并截断证据。
   Finalized V1/V2 只保留普通 reader 的只读兼容，不允许 append recovery 或 corpus
   ownership promotion。
7. 若进程在 OOC finalize 与 checkpoint 删除之间退出，重启会验证 finalized
   corpus 的 checkpoint prefix 连续性，以只读方式继续，禁止重新 append。

**Crash-safety 边界**:

- 普通 `OOCRelationReader` 始终拒绝 incomplete store；只有带配对 V3 descriptor
  的 recovery path 能读取并回滚到已提交前缀。
- checkpoint 发布使用同目录临时文件和替换操作。发布前失败时重新打开已持久化
  OOC prefix 并重试；若替换后目录同步报错，Pipeline 会严格加载正式文件并与
  本次目标逐字段比较，只有目标版本已可见时才按“已发布、耐久性告警”继续。
- OOC prefix checkpoint 和 finalize 会同步 data/index 文件，并在 POSIX 上同步
  父目录。进程崩溃矩阵覆盖 prefix、checkpoint 临时态/发布态、append tail、
  finalize metadata 与 final magic；文件系统和硬件仍决定断电耐久性的最终边界。
- 同进程 checkpoint/resume 只重验 paired header、精确 extent、首 offset 与
  sentinel，保持 O(1) checkpoint 边界；final precommit 与进程重启恢复才完整扫描
  offset/record，避免固定 checkpoint 周期对增长中 relation index 造成二次复杂度。
- 测试用 self-exec 子进程在 typed save stage 调用 `std::_Exit()`，避免析构自动
  finalize 造成“伪崩溃”。这些测试证明进程退出一致性；不把它表述为完整断电证明。

**集成点**:

- `include/gnfs/sieve/sieve_checkpoint.hpp` — V2 wire format、checksum、原子发布
- `include/gnfs/sieve/sieve_run_identity.hpp` — portable run identity
- `include/gnfs/relation/ooc_relation_format.hpp` — 轻量 V3 format contract
- `include/gnfs/relation/ooc_relation_store.hpp` — paired V3 identity、prefix rollback
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
