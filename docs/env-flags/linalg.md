# 线性代数 (linalg) 模块 ENV 调优开关

> 本文档收录 `linalg` 模块所有 `GNFS_*` 运行时调优开关 / helper 的详细设计(算法、ENV 解析、bit-for-bit 保证、ROI、集成点、测试)。
>
> 返回: [ENV 开关模块导航](README.md)

---

## Thin matrix BW solve (B'=M^T·M variant, default ON)

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

## Block Lanczos Checkpoint Resume (GNFS_BL_CHECKPOINT)

**ENV `GNFS_BL_CHECKPOINT=<base>`** enables a durable checkpoint for the
word-packed Gaussian elimination path used by `BlockLanczos`. The solver writes
`<base>.bl_ckpt` after each `GNFS_BL_CHECKPOINT_INTERVAL` completed pivots
(default `50`, clamped to `[1, 1'000'000]`). A successful solve removes the
checkpoint; an interrupted process may leave it for the next invocation.

Resume is accepted only when the checksum, source matrix identity, matrix shape,
packed payload, and cursor semantics all agree. For a matrix with `rows` and
`cols`, the persisted cursor must satisfy:

```text
pivot_row <= rows
rows <= cur_col <= rows + cols
pivot_row <= cur_col - rows
```

The final invariant records that every completed pivot corresponds to a matrix
column already scanned. A checksum-valid file that violates any invariant is
rejected, removed, and replaced by a fresh run (fail-closed resume with
deterministic fallback). This prevents a malformed or stale cursor from
skipping elimination work or treating an uninitialized augmented matrix as a
completed prefix.

V3 checkpoints bind the state to a SHA-256 digest of the source matrix. The
canonical byte stream starts with the ASCII tag `GNFS_SPARSE_MATRIX_ID_V1`
(without a terminator), followed by `rows` and `cols`. Each row then contributes
its nonzero count and sorted column indices. Every number uses an unsigned
64-bit little-endian encoding. Row boundaries, row order, and column coordinates
are significant; insertion order is not. The solver normalizes rows before this
step, so hashing reads existing indices without copying them.

The digest is computed once per solve only when checkpointing is enabled. With
`GNFS_BL_CHECKPOINT` unset or empty, the solver does not hash the matrix. Periodic
saves reuse the same digest.

V3 retains the V2 little-endian field order through `header_checksum`, then
inserts the 32 digest bytes before `aug_word_count`. The header checksum XORs
`version`, `rows`, `cols`, `aug_words_per_row`, `pivot_row`, `cur_col`, `iteration`,
and the four little-endian digest words. The payload checksum and incomplete
magic protocol remain unchanged. A complete V3 file contains
`120 + 8 * aug_word_count` bytes. A state without a fingerprint cannot be saved;
the writer rejects it before truncating an existing file.

The reader checks payload counts against the 64 GiB packed-payload cap and the
vector implementation's maximum size before allocation. If allocating a valid
payload still fails, `load()` returns `nullopt`, allowing the solver to restart.
The allocation-failure regression uses a Linux resource limit and explicitly
skips that probe on other platforms or under AddressSanitizer; oversized-count
rejection remains covered on every platform.

V1 native-endian and V2 little-endian files remain decodable, with an absent
matrix fingerprint. They are not safe to resume because a different matrix may
have the same dimensions. The solver removes these unbound checkpoints and
restarts elimination. This one-time compatibility transition loses saved
elimination work but does not change relation data or the resulting dependencies.
There is no option to resume an unbound legacy state.

**集成点与测试**:
- `src/linalg/block_lanczos.cpp` — matrix identity, shape, and cursor validation
  before copying checkpoint state into the live augmented matrix.
- `include/gnfs/linalg/matrix_fingerprint.hpp` — canonical sparse-matrix identity.
- `include/gnfs/linalg/bl_checkpoint.hpp` — V3 binding, legacy decoding, checksums,
  payload bounds, and two-phase completion marker contract.
- `tests/test_bl_checkpoint.cpp` — independent V3 byte fixture, legacy decoding,
  identity corruption, and unbound-save preservation.
- `tests/test_bl_resume_integration.cpp` — baseline parity, synthetic and real
  partial resume, same-shape matrix mismatch, legacy restart, dimension mismatch
  rejection, and impossible-cursor fallback. Resume diagnostics prove a matching
  checkpoint was consumed instead of merely producing fresh-run parity.

---

## BW Krylov sequence mmap (GNFS_BW_KRYLOV_MMAP)

**ENV `GNFS_BW_KRYLOV_MMAP=1`** (2026-05-18 实施):
BW Phase 1 在系统临时目录下独占创建 `gnfs_bw_krylov_*.kry.scratch` 子目录，
将 Krylov 序列映射到其中的 `sequence.kry`。Phase 2 BM 入口把序列复制到内存，
随后关闭映射并删除本次调用的文件和子目录。重名时会尝试下一个 nonce，保留已有内容。

```bash
GNFS_BW_KRYLOV_MMAP=1 ./gnfs <N>   # 50d+/60d 大矩阵 Phase 5 启用
```

**ROI**:
- matrix BM `A_seq[L]` of DenseGF2_64x64 (512B): 16 MB @ n=1M
- scalar BM `sequences[64][seq_len]` of uint8_t: 128 MB @ n=1M
- 总 ~144 MB physical RAM 释放 给 V/Vnext block vectors + matrix + OS cache

**集成点** (commits `21ac368` → `66ce50f`, 2026-05-18):
- `include/gnfs/linalg/krylov_sequence_mmap.hpp` — POSIX mmap / Win32 file-mapping RAII container
- `src/linalg/block_wiedemann.cpp` — matrix BM `block_solve` + scalar BM `streaming_solve`
- `tests/test_krylov_sequence_mmap.cpp` — cross-platform, Release-active construction,
  persistence, access, handle-lifecycle, and pre-I/O size-boundary contracts
- `tests/test_bw_krylov_mmap_integration.cpp` — one cross-platform,
  Release-active 5550×5000 three-mode contract; the required Windows Release
  lane exercises memory, raw Win32 mapping, and compressed positioned I/O

Windows paths passed to the raw mmap container are treated as UTF-8. The
Win32 implementation converts that byte string to a native wide path before
`CreateFileW`, header validation, or deletion, so non-ASCII temporary and
workspace names do not depend on the machine ANSI code page. The public
`path()` accessor continues to return the original UTF-8 string. The
`KrylovSequenceMmap` test includes a non-ASCII filename round trip on every
platform and uses the same native-path conversion for its file probes.

**Default OFF**: vector path 完整保留, 零回归风险. 仅 50d+ Phase 5 RAM pressure 时启用.

---

## BW Krylov sequence compression (GNFS_BW_KRYLOV_COMPRESS)

**ENV `GNFS_BW_KRYLOV_COMPRESS=1`**: 当且仅当
`GNFS_BW_KRYLOV_MMAP=1` 时，将 matrix-BM Phase 1 序列写成
`mmap+zip` 的 chunked scratch 格式。解析规则只识别首字符 `1`；未设置、
空值和其他值均为关闭。默认 OFF，单独设置本开关不会改变内存路径。

每个 chunk 先对相邻 `DenseGF2_64x64` entry 做 XOR delta，再用内置 byte-RLE
编码。writer 必须按序接收恰好 `L` 个 entry，并在 payload、index 与 header
首次同步成功后才发布 completion marker；reader 在分配前验证完整 header、
index extent 和每个 payload 边界。内部 I/O 使用 POSIX positioned I/O 或
Win32 overlapped positioned I/O，路径保持 `std::filesystem::path`。正常完成或
C++ 异常展开会关闭 handle 并清理本次独占目录中的 `sequence.kryz`；进程终止可能遗留
INCOMPLETE scratch，reader 会拒绝加载。

**bit-for-bit contract**: 压缩只改变 Krylov 序列的存储介质，不改变 solver
seed、矩阵运算或 dependency 顺序。`BWKrylovMmapIntegration` 用同一 fixture
和 seed 强制 memory、mmap、mmap+zip 三路返回逐位相同且数学有效的依赖；
required Windows Release lane 还核验 `mmap+zip` route、压缩统计和清理证据。

```bash
GNFS_BW_KRYLOV_MMAP=1 GNFS_BW_KRYLOV_COMPRESS=1 ./gnfs <N>
```

**集成点与测试**:
- `src/linalg/block_wiedemann.cpp` — matrix-BM storage route、copy-in/copy-out
  与 scratch 生命周期
- `include/gnfs/linalg/krylov_sequence_compressed.hpp` — 跨平台 public contract
- `tests/test_krylov_compress.cpp` — 平台无关 codec 合同
- `tests/test_krylov_compression.cpp` — Release-active Win32/POSIX storage、损坏
  拒绝、LRU、Unicode 与 publication 合同
- `tests/test_bw_krylov_mmap_integration.cpp` — 三模式 end-to-end bit-for-bit
  witness

---

## BW Krylov multi-stream parallel (GNFS_BW_KRYLOV_STREAMS)

**ENV `GNFS_BW_KRYLOV_STREAMS=K`** (2026-05-21 实施, range [1, 16], default 1):
BW Phase 1+2+3 跑 K 个独立 worker (各自独立 seed + 独立小 ThreadPool, 每个
`hardware_concurrency/K` 线程). 默认 K=1 保持原 sequential 3-seed retry 行为
bit-for-bit. K>1 时 base seed 每个 retry round 派 K 个 stream 并发, 结果
按 content 去重合并到 max_deps.

调用方若将 `max_deps` 设为 0, solver 会在读取矩阵和创建 worker 之前立即返回
空结果；这是一条显式 no-op 契约，不会触发 Krylov、BM 或多流重试。

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
- Mmap 临时目录名包含 PID、stream tag、seed 和进程内递增的 nonce；每个并发 stream 单独预留目录。

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

---

## SIMD GF(2) SpMV inner kernels (GNFS_SPMV_SIMD)

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

---

## Schirokauer map 每关系并行 (GNFS_SCHIROKAUER_THREADS)

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
- `include/gnfs/linalg/schirokauer.hpp` — 并行化时未改动；2026-11-27 加入下面的首一化
- `tests/test_schirokauer_parallel.cpp` — 7 个测试 (baseline + 2 parity +
  env parsing + empty + single + perf info)

### 模多项式首一化

`SchirokaurMap::precompute_for_prime` 把 $f(x)$ 的系数取模 $\ell^k$ 存入 `info.f_mod` 后，若首项系数模 $\ell$ 可逆，就乘上它的逆元（模 $\ell^k$），使 `f_mod` 首一。

原因：`FastPoly::reduce_inplace` 只以系数 1 消去最高次项。模多项式不首一时，它等于在「把 $f$ 的首项系数换成 1」的多项式下做 reductions，算出的列不是 $f$ 的 Schirokauer 映射。首项系数 $f_d$ 是 $\ell^k$ 的单位时，$(f)$ 与 $(f / f_d)$ 在 $\mathbb{Z}/\ell^k$ 上是同一个理想，商环不变，所以按首一模多项式算出的列就是 $f$ 的 Schirokauer 映射。split 路径的 `f_pk` 取自同一个 `f_mod`，Hensel 提升出的因子因此全部首一。

- 生效条件：$\ell \nmid f_d$。`MatrixBuilder` 固定用 $\ell = 2$，即 $f_d$ 为奇数。
- 首一多项式不受影响：$f_d = 1$ 时逆元为 1，每一位都不变。
- $\ell \mid f_d$ 时（2026-11-28 起）：逆元不存在，`MatrixBuilder` 不为这样的 $\ell$ 构造 `SchirokaurMap`。`ModularPoly::is_irreducible` 对 $f$ 模 $\ell$ 返回 false，`factorize_and_setup` 分解的是 $f$ 模 $\ell$ 去掉为零首项之后的多项式，Hensel 提升补不回首项，也没有校验，得到的列不是同态。直接构造 `SchirokaurMap` 的调用方必须自己排除整除 $f_d$ 的素数。
- 补偿：$\ell = 2$ 整除 $f_d$ 时 $f$ 模 2 必然可约，`build_with_qc` 沿用已有的二次特征列补偿，列数为 `num_qc_primes + degree * 8`。
- 验证：`LinearAlgebra` 的非首一用例以伴随矩阵为参照，并检查偶数首项时 Schirokauer 列数为 0；`SchirokauerNonMonicE2E` 用首项系数模 4 余 3 的多项式走完整条流水线。

### 非平方首项的对数奇偶列

`PolynomialContext::algebraic_norm` 返回的是 $F(a, b) = b^d f(a/b) = f_d \cdot N(a - b\alpha)$，比域范数多一个因子 $f_d$。有理因子基、代数因子基、大素数与符号列只保证依赖里所有 $F$ 的乘积是平方整数。

设依赖共有 $k$ 个 $(a, b)$ 对，合并关系的每个 `extra_ab_pairs` 条目各算一个对。$f_d$ 不是完全平方数且 $k$ 为奇数时，$f_d^k$ 不是平方，$N(\prod(a - b\alpha)) = |\prod F| / f_d^k$ 也不是平方，而数域中平方元素的范数必为平方，所以这样的依赖不可能是平方。

`MatrixBuilder` 在 $f_d$ 不是有理数平方时追加一列：行值是该关系所含对数的奇偶，即 `1 + extra_ab_pairs.size()` 模 2；位置在 Schirokauer 列之后，偏移量由 `ColumnMapping::pair_parity_start()` 给出，列数记在 `num_pair_parity_columns`。$f_d$ 是完全平方数（含 1）时不加这一列，首一夹具与 120 bit 门禁的矩阵逐位不变。平方判断用 `Integer` 的精确整数开方，不经过浮点。不带 `PolynomialContext` 的 `build()` 不加这一列。

- 验证：`LinearAlgebra` 检查首项 3、7、12 有这一列且行值正确（含合并关系），首项 1、4、9、36 没有，内存版与流式版列数一致。

---

## SGE batch-pivot 选择 (GNFS_SGE_BATCH_PIVOTS)

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

---

## Cache-blocked GF(2) matrix transpose (GNFS_MATRIX_TRANSPOSE_BLOCKED)

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

---

## GF(2) word popcount SIMD batch (GNFS_GF2_POPCNT_SIMD)

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

---

## GF(2) AND-popcount SIMD batch (GNFS_GF2_AND_POPCNT_SIMD)

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

---

## GF(2) XOR-popcount SIMD batch (GNFS_GF2_XOR_POPCNT_SIMD)

**ENV `GNFS_GF2_XOR_POPCNT_SIMD=auto|0|1`** (2026-05-23 实施, W14 T1, default auto):
GF(2) batch `popcount(a[i] ^ b[i])` (Hamming distance per word) helper,
与 W9 `GNFS_GF2_POPCNT_SIMD` / W10 `GNFS_GF2_AND_POPCNT_SIMD` /
W11 `GNFS_GF2_ROW_XOR_SIMD` / W13 T1 `GNFS_GF2_AND_WORDS_SIMD` 并列的
第五个 SIMD primitive. 提供 NEON 2-lane (ARM64, `veorq_u64 + vcntq_u8 +
vaddv_u8`) / AVX2 4-lane (x86_64, `_mm256_xor_si256 + _mm256_popcnt_epi64`
if AVX-512 VPOPCNTDQ available, else fall back `_mm_popcnt_u64` 4-wide)
wide XOR-then-popcount 替代逐 word `__builtin_popcountll(a[i] ^ b[i])`.
应用场景: Block Lanczos / Block Wiedemann 依赖向量漂移度量, GF(2) 基向量
Hamming distance 检查, 对称差累加, parity-difference 等需要 batch
XOR + popcount 两个 uint64_t array 的内核. Pure header, 不依赖外部库.

```bash
GNFS_GF2_XOR_POPCNT_SIMD=auto ./gnfs <N>   # 默认: NEON/AVX2 可用则启用
GNFS_GF2_XOR_POPCNT_SIMD=0    ./gnfs <N>   # 强制 scalar (回归 bisect 用)
GNFS_GF2_XOR_POPCNT_SIMD=1    ./gnfs <N>   # 强制 SIMD (无 SIMD 平台 fallback)
unset GNFS_GF2_XOR_POPCNT_SIMD             # 同 auto
```

**Helper API** (`include/gnfs/linalg/detail/xor_popcnt_simd.hpp`):
- `batch_xor_popcount_words(a, b, out)` — 主入口, `out[i] = popcount(a[i] ^ b[i])`.
  SIMD path 当 `xor_popcnt_simd_enabled()` 为 true 时启用. `a.size() ==
  b.size()` 必须成立 (debug build assert); `out.size() < a.size()` 时
  defensive clamp 到 `out.size()` 防止 UB write past out.
- `total_xor_popcount_words(a, b)` — 累加 sum 入口, 返回 `uint64_t` 总
  Hamming distance.
- `batch_xor_popcount_words_scalar(a, b, out)` — 朴素 `__builtin_popcountll(a ^ b)`
  参考 (test golden + 无 SIMD fallback).
- `total_xor_popcount_words_scalar(a, b)` — 朴素累加参考.
- `xor_popcnt_simd_mode()` — 返回 `XorPopcntSimdMode { Auto, ForceOff, ForceOn }`.
- `xor_popcnt_simd_enabled()` — 三态 dispatcher decision (ForceOff → false,
  ForceOn/Auto + supported → true, 否则 false).
- `xor_popcnt_simd_supported()` — compile-time `__ARM_NEON / __AVX2__` 探测.
- `xor_popcnt_simd_reset_env_cache_for_testing()` — 测试专用 re-resolve ENV.

**与兄弟 helper 的区别 (helper family 第 5 名成员)**:
- W9 `popcount_simd` (`batch_popcount_words(words, out_u32)`) — 单输入,
  缩减为 uint32 per-word Hamming weight + uint64 total. 不输出 word vector.
- W10 `and_popcnt_simd` (`batch_and_popcount_words(a, b, out_u32)`) —
  双输入 fused AND-then-popcount, 缩减为 uint32 per-word 权重 + total.
  数学上等于 `popcount(a & b)` (intersection cardinality).
- W11 `xor_words_simd` (`batch_xor_words(dst, src)`) — 双输入, **in-place**
  `dst[i] ^= src[i]`, 保留 dst 作为 accumulator. 双参数 (无独立 out),
  不缩减为 popcount.
- W13 T1 `and_words_simd` (`batch_and_words(a, b, out)`) — 双输入, 三参,
  保留 AND 后的 uint64 vector 供下游消费 (不 in-place, 不缩减为 popcount).
- **W14 T1 `xor_popcnt_simd` (`batch_xor_popcount_words(a, b, out_u32)`) —
  双输入 fused XOR-then-popcount, 缩减为 uint32 per-word Hamming distance +
  uint64 total. 数学上等于 `popcount(a ^ b)` (symmetric difference
  cardinality)**. 与 W10 AND-popcount 是 dual primitive — AND 计交集 bit
  数 (共有), XOR 计对称差 bit 数 (相异).

**算法**:
- NEON: `vld1q_u64(2 word)` × 2 inputs → `veorq_u64` → `vcntq_u8`
  (16-byte popcount) → `vget_low_u8 + vaddv_u8` (per-word horizontal sum)
  per word. Tail 走 scalar `__builtin_popcountll(a ^ b)`.
- AVX2: `_mm256_loadu_si256(4 word)` × 2 inputs → `_mm256_xor_si256` →
  `_mm256_popcnt_epi64` 若 AVX-512 VPOPCNTDQ 可用, 否则 fallback
  `_mm_popcnt_u64` 4-wide unroll after 4-lane store (POPCNT 指令在
  Nehalem+ x86_64 单条).
- Reduction: `total_xor_popcount_words` 用单条 `vaddvq_u8` (NEON) 或累加
  4 个 popcount (AVX2) 减少 horizontal sum 次数.

**Bit-for-bit guarantee**: `popcount(a ^ b)` 是 pure function of (a, b),
SIMD path 与 scalar `__builtin_popcountll(a ^ b)` 输出严格 per-index 一致,
reduction sum 同样 byte-identical. 空输入返回空 output / 零 total 而不
touch pointers. `a == b` content (Hamming distance 0) 严格产出全零
output (与 W10 AND-popcount 的 `popcount(a & a) = popcount(a)` 截然不同,
单元测试强制覆盖此区别防止误路由). 单元测试 `test_xor_popcnt_simd` 14
个测试强制覆盖 (4 ENV / empty / 10 single-word Hamming distance patterns
含互补 0xAA^0x55 全异 / aligned 32 / unaligned 33 / random 1000 /
total parity / ForceOff vs Auto parity / 1M perf info / undersized out
clamping / a==b self-XOR 零距离 identity).

**ROI 与定位**:
- 主要 ROI: GF(2) 向量间 Hamming distance 度量 (Block Lanczos / Block
  Wiedemann 依赖向量漂移检查, 多基差异) 的 hot path. 单一 fused
  XOR+popcount kernel 比独立 XOR + 独立 popcount 少一次 memory traffic +
  一次 register round-trip. 与 W10 AND-popcount 对偶 (AND 计交集 size,
  XOR 计对称差 size).
- helper 当前 standalone (主 pipeline 无 wire-in), 是 future-infra:
  Block Lanczos / Block Wiedemann 内部 distance metric / drift check /
  parity-diff reduction 等 explicit wire-in 后启用.
- 默认 auto 在 macOS arm64 / Linux x86_64 都启用 SIMD path; ENV=0 在
  PMU sweep / sanitizer 调试时回到 scalar baseline. perf-info 实测
  1M word M-series ARM64: scalar ~2.6 ms, SIMD ~6.7 ms (CNT autovectorise
  在 Apple Silicon 上对 XOR-fused 单数组已经非常优秀, 而 SIMD 路径多了
  2 倍 load + 1 个 XOR op; ROI 在 x86_64 AVX-512 VPOPCNTDQ 平台或 wire-in
  到 Hamming distance hot loop 后体现).

**集成点** (2026-05-23, W14 T1):
- `include/gnfs/linalg/detail/xor_popcnt_simd.hpp` — helper API + ENV gate +
  NEON / AVX2 inner kernels + 朴素 reference.
- `tests/test_xor_popcnt_simd.cpp` — 14 个测试 (4 ENV 解析 + empty + 10
  single-word patterns + aligned 32 + unaligned 33 + random 1000 +
  total parity + ForceOff vs Auto parity + 1M perf info + undersized
  out clamping + a==b self-XOR 零距离 identity).
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  linalg 模块.

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点,
所以 ENV 对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

---

## GF(2) per-row popcount SIMD batch (GNFS_GF2_ROW_POPCOUNT_SIMD)

**ENV `GNFS_GF2_ROW_POPCOUNT_SIMD=auto|0|1`** (2026-05-23 实施, W15 T1, default auto):
GF(2) 行主序 packed 矩阵的 per-row Hamming weight helper. 给定
`row_count * row_words` 个 uint64_t 连续摆放的 row-major matrix (行 r
占 `matrix[r * row_words .. r * row_words + row_words)`), helper 把每
行的 set-bit 总数写到 `out_row_weights[r]`. 应用场景: matrix
column-weight tally, dependency 向量 parity 检查, sparsity profile
统计, 或任何需要 per-row Hamming weight 作 primitive 的 caller.
Pure header, 不依赖外部库.

```bash
GNFS_GF2_ROW_POPCOUNT_SIMD=auto ./gnfs <N>   # 默认: NEON/AVX2 可用则启用
GNFS_GF2_ROW_POPCOUNT_SIMD=0    ./gnfs <N>   # 强制 scalar (回归 bisect 用)
GNFS_GF2_ROW_POPCOUNT_SIMD=off  ./gnfs <N>   # 同 0
GNFS_GF2_ROW_POPCOUNT_SIMD=1    ./gnfs <N>   # 强制 SIMD (无 SIMD 平台 fallback)
GNFS_GF2_ROW_POPCOUNT_SIMD=on   ./gnfs <N>   # 同 1
unset GNFS_GF2_ROW_POPCOUNT_SIMD             # 同 auto
```

**Helper API** (`include/gnfs/linalg/detail/row_popcount_simd.hpp`):
- `per_row_popcount_words(matrix, row_words, out_row_weights)` — 主
  入口, `out_row_weights[r] = popcount(matrix[r * row_words .. (r+1) *
  row_words))`. SIMD path 当 `row_popcount_simd_enabled()` 为 true
  时启用. Defensive clamp 到
  `min(matrix.size() / row_words, out_row_weights.size())`.
- `per_row_popcount_words_scalar(matrix, row_words, out)` — scalar
  reference (test golden + 无 SIMD fallback).
- `row_popcount_simd_mode()` — 返回 `RowPopcountSimdMode { Auto,
  ForceOff, ForceOn }`.
- `row_popcount_simd_enabled()` — 三态 dispatcher decision (ForceOff →
  false, ForceOn/Auto + supported → true, 否则 false).
- `row_popcount_simd_supported()` — compile-time `__ARM_NEON / __AVX2__`
  探测.
- `row_popcount_simd_reset_env_cache_for_testing()` — 测试专用
  re-resolve ENV.

**与兄弟 helper 的区别 (helper family 第 6 名成员)**:
- W9 `popcount_simd` (`batch_popcount_words(words, out_u32)`) — 单输入,
  flat 1-D batch popcount over 单 `uint64_t` span. 无 row 维度.
- W10 `and_popcnt_simd` (`batch_and_popcount_words(a, b, out_u32)`) —
  双输入 fused AND-then-popcount, flat 1-D.
- W11 `xor_words_simd` (`batch_xor_words(dst, src)`) — 双输入,
  in-place flat 1-D `dst[i] ^= src[i]`.
- W13 T1 `and_words_simd` (`batch_and_words(a, b, out)`) — 双输入,
  三参 flat 1-D `out[i] = a[i] & b[i]`.
- W14 T1 `xor_popcnt_simd` (`batch_xor_popcount_words(a, b, out_u32)`)
  — 双输入 fused XOR-then-popcount, flat 1-D.
- **W15 T1 `row_popcount_simd` (`per_row_popcount_words(matrix,
  row_words, out_u64)`) — 单矩阵输入加 explicit row width, 输出每
  行一个 weight**. helper 家族第一个尊重 2-D 矩阵 layout 而非 flat span
  的成员. W9 `total_popcount_words` 能对单行算出同样的 answer 但无法
  mass-process N 行 (每行需要独立的 dispatcher gate read); 本 helper
  读 SIMD gate 一次, 然后在 outer loop 折叠所有行.

**算法**:
- NEON: 每行内部 `vld1q_u64(2 word)` → `vcntq_u8` (16-byte popcount) →
  `vaddvq_u8` (per-128-bit horizontal sum). 2-word stride. Tail scalar
  `__builtin_popcountll`. Cross-row 由 outer for-loop 处理.
- AVX2: 每行内部 `_mm256_loadu_si256(4 word)` → `_mm256_popcnt_epi64`
  若 AVX-512 VPOPCNTDQ 可用, 否则 fallback `_mm_popcnt_u64` 4-wide unroll
  after 4-lane store. 4-word stride. Tail scalar.
- Cross-row 并行**不**由本 helper 提供 — caller 通过 W7 /
  W8 / W10 T4 / W11 / W12 T4 / W13 T5 等 `parallel_*` 家族外层
  dispatcher 自行决定串/并.

**Bit-for-bit guarantee**: per-row Hamming weight 是 pure function of
row words, SIMD path 与 scalar `__builtin_popcountll` 累加输出严格
per-row 一致 (`uint64_t == uint64_t`, 不容忍单字差异). 每行独立计算,
partial sum 不跨越 row 边界. 输出顺序严格保留
(`out_row_weights[r]` 永远对应 matrix row `r`). 单元测试
`test_row_popcount_simd` 14 个测试强制覆盖 (ENV unset auto / explicit
auto / 0/off ForceOff / 1/on ForceOn + 8 unrecognised tokens fall to
Auto / empty matrix / row_words=0 silent no-op / single row single
word 8 hand-verified patterns / single row aligned 32 / multi-row
unaligned 33 x 10 / 100x100 random / ForceOff vs Auto parity /
undersized out_row_weights defensive clamp / reset env cache hook /
1M-row x 4-word perf info).

**Defensive contract**:
- `row_words == 0`: 静默 no-op, 不 touch outputs.
- `out_row_weights.size() < matrix.size() / row_words`: 只写前
  `out_row_weights.size()` 行, 越界行静默跳过.
- `matrix.size() % row_words != 0`: 只处理 integer-row 前缀, 末尾
  partial row 丢弃 (它不是 row-major 约定下合法的行).

**ROI 与定位**:
- 主要 ROI: 当 caller 需要 per-row weight tally (column-weight 累计,
  dependency parity, sparsity stats), 本 helper 把 gate 读取从 N 次降
  到 1 次, 并把 SIMD load/popcount 集中在每行 inner kernel. 真正
  ROI 在 x86_64 AVX-512 VPOPCNTDQ 平台 (4 word per instruction) 或
  长行 (row_words >> 4) 时显著.
- helper 当前 standalone (主 pipeline 无 wire-in), 是 future-infra:
  matrix column-weight tally, Block Lanczos / Block Wiedemann 依赖
  parity, SGE column profile 等 explicit wire-in 后启用.
- 默认 auto 在 macOS arm64 / Linux x86_64 都启用 SIMD path; ENV=0 在
  PMU sweep / sanitizer 调试时回到 scalar baseline. perf-info 实测
  1M row x 4 word M-series ARM64: scalar 4.52 ns/row vs SIMD 21.29
  ns/row — 极短行 (row_words=4) 上 Apple Silicon scalar 路径已经
  autovectorise 到接近上限, SIMD 多 1 load + 1 horizontal-sum 是
  负 ROI. 真正 ROI 在长行 (row_words >= 16) 或 x86_64 AVX2 平台.

**集成点** (2026-05-23, W15 T1):
- `include/gnfs/linalg/detail/row_popcount_simd.hpp` — helper API + 三
  态 ENV gate + NEON 2-word stride / AVX2 4-word stride inner kernels +
  scalar reference + defensive clamp.
- `tests/test_row_popcount_simd.cpp` — 14 instant tier tests, TIMEOUT 60.
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  linalg 模块.

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点,
所以 ENV 对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

---

## GF(2) row word XOR SIMD batch (GNFS_GF2_ROW_XOR_SIMD)

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

---

## GF(2) word AND batch SIMD (GNFS_GF2_AND_WORDS_SIMD)

**ENV `GNFS_GF2_AND_WORDS_SIMD=auto|0|1`** (2026-05-22 实施, W13 T1, default auto):
GF(2) batch `out[i] = a[i] & b[i]` helper, 与 W9 `GNFS_GF2_POPCNT_SIMD` /
W10 `GNFS_GF2_AND_POPCNT_SIMD` / W11 `GNFS_GF2_ROW_XOR_SIMD` 并列的第四
个 SIMD primitive. 提供 NEON 2-lane (ARM64, `vandq_u64`) / AVX2 4-lane
(x86_64, `_mm256_and_si256`) wide bitwise AND 替代逐 word `&`. 应用场景:
Block Lanczos / Block Wiedemann mask 应用 (e.g. dependency mask `dep
&= active_cols`), GF(2) 行交集 cache, 结构化高斯消元的活跃列投影,
任何需要把两个 uint64 array 按位 AND 后保留 uint64 vector 形式 (不
立即缩减为 popcount) 供下游消费的 hot path. Pure header, 不依赖外部库.

```bash
GNFS_GF2_AND_WORDS_SIMD=auto ./gnfs <N>   # 默认: NEON/AVX2 可用则启用
GNFS_GF2_AND_WORDS_SIMD=0    ./gnfs <N>   # 强制 scalar (回归 bisect 用)
GNFS_GF2_AND_WORDS_SIMD=1    ./gnfs <N>   # 强制 SIMD (无 SIMD 平台 fallback)
unset GNFS_GF2_AND_WORDS_SIMD             # 同 auto
```

**Helper API** (`include/gnfs/linalg/detail/and_words_simd.hpp`):
- `batch_and_words(a, b, out)` — 主入口, `out[i] = a[i] & b[i]` for
  `i in [0, min(a.size(), b.size(), out.size()))`. SIMD path 当
  `and_words_simd_enabled()` 为 true 时启用. Empty 输入 (任一输入 span
  size 0) no-op, 不 touch out.
- `batch_and_words_scalar(a, b, out)` — 朴素 `out[i] = a[i] & b[i]` loop
  参考 (test golden + 无 SIMD fallback).
- `and_words_simd_mode()` — 返回 `AndWordsSimdMode { Auto, ForceOff, ForceOn }`.
- `and_words_simd_enabled()` — 三态 dispatcher decision (ForceOff → false,
  ForceOn/Auto + supported → true, 否则 false).
- `and_words_simd_supported()` — compile-time `__ARM_NEON / __AVX2__` 探测.
- `and_words_simd_reset_env_cache_for_testing()` — 测试专用 re-resolve ENV.

**与兄弟 helper 的区别 (helper family 第 4 名成员)**:
- W9 `popcount_simd` (`batch_popcount_words(words, out_u32)`) — 单输入,
  缩减为 uint32 per-word Hamming weight + uint64 total. 不输出 word vector.
- W10 `and_popcnt_simd` (`batch_and_popcount_words(a, b, out_u32)`) —
  双输入 fused AND-then-popcount, 缩减为 uint32 per-word 权重 + total.
  数学上等于 `popcount(a & b)` 的 fused kernel, 节省一次中间 word vector
  materialise.
- W11 `xor_words_simd` (`batch_xor_words(dst, src)`) — 双输入, **in-place**
  `dst[i] ^= src[i]`, 保留 dst 作为 accumulator. 双参数 (无独立 out).
- **W13 `and_words_simd` (`batch_and_words(a, b, out)`) — 双输入, 三参,
  保留 AND 后的 uint64 vector 供下游消费 (不 in-place, 不缩减为 popcount)**.
  适用 mask 缓存场景, caller 需要后续对 AND 结果做多次访问 / 进一步
  bitwise op / 持久化, 而不仅仅是统计 weight.

**算法**:
- NEON: `vld1q_u64(2 word)` × 2 inputs → `vandq_u64` (128-bit AND) →
  `vst1q_u64(2 word)` per 2-word stride. Tail 走 scalar `&`.
- AVX2: `_mm256_loadu_si256(4 word)` × 2 inputs → `_mm256_and_si256`
  (256-bit AND) → `_mm256_storeu_si256(4 word)` per 4-word stride.
  Tail 走 scalar `&`.
- Defensive clamp: `min(a.size(), b.size(), out.size())` 决定 AND 字数,
  避免 UB read past 任一输入或 UB write past out. Length mismatch 不
  抛异常, 静默 clamp (符合 W9 / W10 / W11 兄弟 helper 的契约). out 在
  clamp 窗口之外的 tail 保持原值不变.

**Bit-for-bit guarantee**: AND 是 pure function of (a[i], b[i]), SIMD
path 与 scalar `&` 输出严格 per-index 一致. 空输入返回不 touch out
(size 不变, content 不变). AND with self (a == b content) 精确产出
a 本身 (`a & a = a`, 与 XOR 的 `a ^ a = 0` 截然不同, 单元测试强制覆盖
此区别防止误路由). 单元测试 `test_and_words_simd` 14 个测试强制覆盖
(4 ENV / empty / 单 word 8 pattern 含不相交 nibble / aligned 32 /
unaligned 33 / random 1000 / ForceOff vs Auto parity / self-AND identity
/ undersized out clamp / a shorter than b tail 保留 / 1M perf info).

**ROI 与定位**:
- 主要 ROI: 当 caller 需要把 AND 结果保留为 uint64 vector 供下游使用
  (mask 缓存 / 多次访问 / 进一步 bitwise op), W10 fused AND-popcount
  路径不适用 (它丢弃 word vector, 只留 popcount). 此时 `batch_and_words`
  是唯一既享受 SIMD 加速、又保留中间 AND 结果的 helper. 单 fused
  load+AND+store kernel 节省 per-iter address-gen pressure.
- helper 当前 standalone (主 pipeline 无 wire-in), 是 future-infra:
  Block Lanczos / Block Wiedemann mask 应用, SGE 活跃列投影, GF(2) 行
  交集 cache 等 explicit wire-in 后启用.
- 默认 auto 在 macOS arm64 / Linux x86_64 都启用 SIMD path; ENV=0 在
  PMU sweep / sanitizer 调试时回到 scalar baseline. perf-info 实测
  1M word M-series ARM64: scalar ~2.0 ms, SIMD ~3.0 ms (scalar `&` 循环
  在 Apple Silicon 上 autovectorise 已经非常优秀; SIMD path 主 ROI 在
  x86_64 AVX2 平台或 wire-in 到 mask-application hot loop 后体现).

**集成点** (2026-05-22, W13 T1):
- `include/gnfs/linalg/detail/and_words_simd.hpp` — helper API + ENV gate +
  NEON / AVX2 inner kernels + 朴素 reference.
- `tests/test_and_words_simd.cpp` — 14 个测试 (4 ENV 解析 + empty + 单
  word 8 pattern + aligned 32 + unaligned 33 + random 1000 + ForceOff
  vs Auto parity + AND-with-self identity + undersized out clamp +
  a shorter than b tail 保留 + 1M perf info).
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  linalg 模块.

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点,
所以 ENV 对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

---

## linalg 迭代进度遥测 (GNFS_LINALG_PROGRESS_INTERVAL)

**ENV `GNFS_LINALG_PROGRESS_INTERVAL=N`** (2026-05-22 实施, W12 T1, default 0):
Block Lanczos / Block Wiedemann 在 50d+/60d 大矩阵 Phase 5 SpMV loop 可能跑数
小时, 此前无任何 runtime 可视化 — 用户不知道当前 iteration 序号, 不知道 wall
elapsed, 不知道 throughput, 也无法估算 ETA. helper 提供一个 ENV-gated +
opt-in 的迭代进度 logger, caller 在 SpMV loop 入口构造一个 `IterationProgressLogger`,
内循环每次 `tick(current_iter)`. 默认 0 (unset / "0" / 负数 / 非数字 / 含
leading 空白) 时 `tick()` / `finish()` 都是单分支 short-circuit no-op, 不取
clock sample, 不做 string formatting, 不写 stderr, 与历史无 telemetry 路径
bit-for-bit 一致. N >= 1 时每 N 次迭代打一行进度到 stderr (跟着 `std::flush`),
`finish()` 多打一行 DONE summary.

```bash
unset GNFS_LINALG_PROGRESS_INTERVAL              # default 0, 零开销
GNFS_LINALG_PROGRESS_INTERVAL=0    ./gnfs <N>    # 同 default
GNFS_LINALG_PROGRESS_INTERVAL=100  ./gnfs <N>    # 每 100 次 iteration 打一行
GNFS_LINALG_PROGRESS_INTERVAL=1    ./gnfs <N>    # 每次 iteration 都打 (debug)
GNFS_LINALG_PROGRESS_INTERVAL=1000 ./gnfs <N>    # 长跑稀疏报告
```

**输出格式** (全部走 `std::cerr` + `std::flush`):
```text
[linalg_progress] phase=<label> iter=<I>/<T> elapsed=<E>s rate=<R>/s eta=<ETA>s
[linalg_progress] phase=<label> DONE iter=<T>/<T> elapsed=<E>s avg_rate=<R>/s
```
其中 `elapsed` 取 `steady_clock`, 毫秒精度 (3 位小数); `rate` 是 iter / elapsed,
1 位小数; `eta = (total - iter) / rate`, 1 位小数. `rate == 0` 或 `total == 0`
或 `iter >= total` 时 `rate` / `eta` 渲染为 `?` 而非 NaN/inf, 保证 nohup 日志
里不会出现 IEEE 754 异常 token.

**Helper API** (`include/gnfs/linalg/progress_telemetry.hpp`):
- `linalg_progress_interval()` — cached `std::once_flag` + `std::atomic<int>`
  ENV reader, 返回 clamped non-negative int. 0 = disabled.
- `linalg_progress_enabled()` — `interval > 0` 等价 predicate.
- `linalg_progress_reset_env_cache_for_testing()` — 测试专用 re-resolve hook.
- `IterationProgressLogger(phase_label, total_iters)` — RAII ctor, 立即 sample
  start time (启用时). `total_iters < 0` clamp 到 0. `total_iters == 0` 时
  eta 渲染为 `?`.
- `tick(current_iter)` — 启用且 (first call OR delta >= interval) 时 emit
  一行. `current_iter < 0` clamp 到 0; `current_iter > total` clamp 到 total.
- `finish()` — emit DONE summary. Idempotent (后续 `tick` / `finish` 都 no-op).
- 析构时若未显式 `finish()` 则自动调用; moved-from logger 标记为已完成, 不双 emit.
- Move-constructible / move-assignable; **非** copyable.

**ENV 解析规则** (严格):
- unset / "" / "0" / 负数 / leading 非数字 (`garbage` / `abc123`) → 0 (disabled)
- leading 空白 (`" 5"` / `"\t10"`) → 0 (disabled; 与 `std::strtol` 默认行为
  不同, 主动 reject 以让 ENV parsing matrix 与文档一致)
- 清洁数字 ("1", "100", "999999") → as-is
- 数字前缀 (`"12abc"`) → 12 (`std::strtol` 接受前缀, 与 W11 T3
  `GNFS_MPZ_POWM_BATCH_THREADS` 行为一致, 文档化但不依赖)
- > INT_MAX → clamp 到 INT_MAX

**Bit-for-bit guarantee (disabled path)**: 默认 OFF 时 `tick()` 内仅一次
`interval_ <= 0` 分支检查 (单 int compare + branch), 不读 clock, 不做 string
ops, 不接触 stderr 缓冲. 100k 次 `tick` 实测 0 ms (perf-info probe). 启用
时输出仅影响 stderr; caller 状态 / 迭代变量 / 返回值零干扰.

**ROI 与定位**:
- 主要 ROI: 长跑可观察性, 不是 wall-time 加速. 50d+/60d Phase 5 BL/BW 跑
  数小时时, 用户看不到进度等于盲跑; 启用 interval=100~1000 后 nohup 日志
  里可定期看到 `iter=42000/100000 elapsed=1245.6s rate=33.7/s eta=1721.3s`,
  能 ssh 进去 `tail -f log` 估算何时完成, 决定是否继续等待或提早 ctrl-C.
- helper 当前 standalone (BL/BW SpMV loop 未 wire-in), 是 future-infrastructure.
  caller wire-in 时在 SpMV loop 入口构造 logger, 内循环 `lg.tick(i)` 即可.
  典型 wire-in 例子:
  ```cpp
  IterationProgressLogger lg("BW_Krylov", L);
  for (int i = 0; i < L; ++i) {
      run_one_krylov_step(i);
      lg.tick(i);
  }
  ```
- Default OFF (interval == 0) 保证 zero behavior change for legacy callers,
  仅当用户 explicit `GNFS_LINALG_PROGRESS_INTERVAL=N>=1` + caller wire-in 时
  生效.

**集成点** (W12 T1, 2026-05-22):
- `include/gnfs/linalg/progress_telemetry.hpp` — helper API + ENV gate +
  `IterationProgressLogger` RAII 类 + `steady_clock` 采样 + 自包含的
  `format_double` (避免 `<iomanip>` 全局 state 副作用)
- `tests/test_linalg_progress.cpp` — 19 个测试 (6 ENV 解析 + 2 disabled
  silence + 4 enabled tick gating / DONE / idempotent / multi-logger +
  5 edge cases (total=0 / 负 total / iter > total / 负 iter / rate 下溢) +
  2 move semantics + 1 perf info)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  linalg 模块
- BL / BW 主路径 `src/linalg/block_lanczos.cpp` / `src/linalg/block_wiedemann.cpp`
  **未改动** (helper-only landing, future wire-in)

**Default OFF (N=0)**: helper standalone, 当前主 pipeline 无调用点, ENV
对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

---

## Block Wiedemann Algorithm Selection (GNFS_BW_ALGORITHM)

`GNFS_BW_ALGORITHM=scalar` 让 `BlockWiedemann::find_dependencies()` 走早期的 scalar
Berlekamp-Massey 路径，用于校验和调试。默认走 Coppersmith 的 block
Berlekamp-Massey，SpMV 调用次数约为 scalar 路径的 $1/64$。

- **取值与解析**：`find_dependencies()` 每次调用时读取并做整串比较，只有 `scalar`
  生效；未设置、空值和其余取值（包括 `block`）都走 block 路径。
- **默认值**：block 路径。
- **对结果的影响**：两条路径都返回通过校验的依赖向量，但不保证返回同一组向量，也
  不保证逐位一致。在内存路线上，行数和列数都小于 5000 的矩阵在读取该变量之前就已
  转给 Block Lanczos，因此不受影响。薄矩阵（行数小于列数）没有 scalar 变体，请求
  `scalar` 时向 stderr 打印一行警告并改用 block 的薄矩阵变体。
- **接入点**：
  - `src/linalg/block_wiedemann.cpp`：只有 `BlockWiedemann::find_dependencies()` 读取该
    变量；scalar 路径调用 `block_wiedemann_scalar_solve()`。
  - `GNFS_BW_KRYLOV_STREAMS` 的多流并行只作用于 block 路径，强制 scalar 时无效。
  - 落盘路线不读取该变量。`GNFS_LINALG_MMAP=on`（或 `auto` 判定落盘）时，
    `Pipeline::matrix_phase()` 改调 `find_dependencies_view()`，那条路线始终用 block
    或 thin 求解器，`scalar` 被静默忽略，所以两者同时设置做不了交叉验证。
- **测试**：`tests/test_block_wiedemann.cpp` 的 `test_block_vs_scalar_cross_validate` 在
  同一矩阵上分别跑两条路径并验证两边的依赖都成立；
  `tests/test_bw_krylov_mmap_integration.cpp` 在 `scalar` 取值下经 `find_dependencies()`
  求解一个大矩阵，并检查 `[BW-scalar]` 日志。没有测试覆盖落盘路线忽略该变量的行为。

---

## Block Lanczos Checkpoint Trace (GNFS_DEBUG_BL_CKPT)

`GNFS_DEBUG_BL_CKPT` 让 word-packed Gaussian elimination 在每次成功保存 checkpoint 后
向 stderr 打印一行 `[bl_ckpt] save iter=... pivot_row=... col=...`。

- **取值与解析**：`BlockLanczos::find_dependencies_sparse()` 在每次保存后读取。已设置、
  非空且首字符不是 `0` 即生效，`1`、`on`、`true` 都可以；未设置、空值或以 `0` 开头
  时不输出。
- **默认值**：未设置，不输出。
- **对结果的影响**：只增加 stderr 输出，不改变依赖向量。
- **接入点**：`src/linalg/block_lanczos.cpp` 的 `find_dependencies_sparse()`。该变量只在
  `GNFS_BL_CHECKPOINT` 启用且 checkpoint 保存成功时才被读取。
- **测试**：没有测试设置该变量。

---

## Gaussian Elimination Trace (GNFS_DEBUG_GAUSSIAN)

`GNFS_DEBUG_GAUSSIAN` 让 word-packed Gaussian elimination 在结束时向 stderr 打印一行
`[Gaussian]` 统计，包含矩阵规模、pivot 数、并行调用量、平均与最大消元行数、线程数
以及找到的依赖数。

- **取值与解析**：`BlockLanczos::find_dependencies_sparse()` 在消元结束后读取一次。
  已设置、非空且首字符不是 `0` 即生效。
- **默认值**：未设置，不输出。
- **对结果的影响**：只增加 stderr 输出，不改变依赖向量。统计计数器无论是否设置都会
  累加。
- **接入点**：`src/linalg/block_lanczos.cpp` 的 `find_dependencies_sparse()`。
- **测试**：没有测试设置该变量；`bench/results/2026-05-14-gaussian-threadpool.md`
  记录了用它做的一次线程池测量。

---

## Linear-Algebra Matrix mmap Policy (GNFS_LINALG_MMAP)

`GNFS_LINALG_MMAP` 决定 Phase 5 在 Block Wiedemann 分支上把 SGE 约简后的矩阵放在内存
还是磁盘。`1` 强制走 `MmapCSRMatrix`，`auto` 在矩阵足够大时才落盘。

- **取值与解析**：`linalg_mmap_policy_from_env()` 调用 `parse_mmap_policy()` 做整串
  比较。未设置、空值、`0`、`off`、`false` 和任何未列出的取值都是 Off；`1`、`on`、
  `true` 是 On；`auto`、`Auto`、`AUTO` 是 Auto。`off` 与 `on` 只接受小写。
- **默认值**：Off，使用内存 CSR。
- **对结果的影响**：落盘不只是换一个存放位置。`Pipeline::matrix_phase()` 在落盘时改调
  `BlockWiedemann::find_dependencies_view()`，它始终用 block 或 thin 的 Block Wiedemann
  求解器，与内存路线有两处不同。第一，它不读取 `GNFS_BW_ALGORITHM`，`scalar` 在这条
  路线上被静默忽略。第二，它没有内存路线里“行数与列数都小于 5000 的矩阵转交 Block
  Lanczos”的分支，所以 `on` 之下 Block Lanczos 没有解出依赖的小矩阵由 Block Wiedemann
  求解；`off` 之下这类小矩阵根本到不了 Block Wiedemann，`find_dependencies()` 把它们
  再交回 Block Lanczos。`auto` 在默认阈值下只对很大的矩阵落盘，不会遇到这种小矩阵。
  求解器可能不同，返回的依赖向量因此不保证相同。On 与 Auto 会把 SGE
  约简矩阵写进临时目录下的 `.csrmat` scratch 文件；写入或求解抛出异常时回退到内存路径
  的 `find_dependencies()` 并打印 `[linalg-mmap] fallback` 警告，回退之后
  `GNFS_BW_ALGORITHM` 与小矩阵转交重新生效。
- **接入点**：
  - `include/gnfs/linalg/linalg_mmap_policy.hpp`：`linalg_mmap_policy_from_env()` 与
    `should_use_mmap()`。
  - `src/api/pipeline.cpp`：`Pipeline::matrix_phase()` 在 Block Wiedemann 分支读取，即
    Block Lanczos 没有返回依赖，或 SGE 约简矩阵是薄矩阵时。Auto 模式下投影的列索引
    字节数（非零元数乘 4）不小于阈值才落盘。
- **测试**：`tests/test_linalg_mmap_policy.cpp` 覆盖解析、三种模式的判定与 Auto 阈值；
  `tests/test_api.cpp` 的 `test_mmap_solve_preserves_existing_scratch_entries` 在 `on`
  下验证 scratch 文件不覆盖临时目录里的既有文件、成功后清理，以及路径被占用时回退到
  内存路径；同文件其他用例把变量钉为 `off`。

### GNFS_LINALG_MMAP_THRESHOLD_BYTES

Auto 模式的落盘阈值，单位是字节，默认 $2^{31}$（2 GiB，约合 $5.4 \times 10^8$ 个非零
元）。`linalg_mmap_threshold_bytes()` 只接受非零的十进制数字串：出现任何其他字符、值
为 `0` 或超出 `uint64_t` 范围时都回到默认值。Off 与 On 模式不读取该变量。
`tests/test_linalg_mmap_policy.cpp` 覆盖覆盖值、非法输入与溢出。

---

## Metal SpMV Opt-In (GNFS_METAL_SPMV)

`GNFS_METAL_SPMV=1` 让 Block Wiedemann 的 GF(2) SpMV 在 macOS 上优先使用 Metal GPU
内核。默认关闭。

- **取值与解析**：`metal::env_opt_in()` 把取值转成小写后比较，`1`、`true`、`yes`、
  `on` 启用，其余关闭。首次调用后缓存到进程结束。非 Apple 平台编译的是
  `src/linalg/metal_spmv_stub.cpp`，`env_opt_in()` 恒为 `false`。
- **默认值**：关闭。
- **对结果的影响**：只改变性能。GF(2) 上的 `uint64_t` XOR 是精确运算，Metal 输出与
  CPU 内核逐位一致；GPU 路径不可用或失败时自动回到 CPU 路径。
- **接入点**：
  - `src/linalg/metal_spmv.mm`：`env_opt_in()` 读取；`metal::should_use()` 要求同时满足
    ENV 启用、`is_available()` 以及行数或列数不少于 10000。
  - `include/gnfs/linalg/detail/spmv_kernels.hpp`：`spmv_forward()` 与
    `spmv_transpose()` 的默认 `SpmvMetalPolicy::environment` 分支，只对 `CSRMatrix`
    生效，`MmapCSRMatrix` 始终走 CPU。
  - `src/linalg/block_wiedemann.cpp`：传入显式 `SeededPolicy` 的
    `find_dependencies_view_seeded()` 重载不读取该变量，改由 `policy.allow_metal`
    决定。
- **测试**：`tests/test_metal_spmv.cpp` 在 `main()` 里设置 `GNFS_METAL_SPMV=1`，对比
  Metal 与 CPU 内核的输出，并在 Metal 不可用时跳过相关用例；
  `tests/test_siqs_shadow_sparse_backend.cpp` 验证显式 `SeededPolicy` 忽略该变量。

---

## Streaming Matrix Build (GNFS_SGE_STREAMING)

`GNFS_SGE_STREAMING` 让 legacy 路线的关系在构建矩阵时经 `VectorRelationSource` 流式
读取，省去并行建行期间的中间关系副本。structured 路线始终走流式构建，不读取该变量。

- **取值与解析**：`Pipeline::matrix_phase()` 每次调用时读取并做整串比较。`1`、`on`、
  `true` 启用；`auto` 仅在 `GNFS_OOC_RELATIONS=1` 或设置了 `GNFS_RESUME` /
  `GNFS_SIEVE_RESUME` 时启用；未设置、空值和其余取值关闭。
- **默认值**：关闭，使用 `MatrixBuilder::build_with_qc()`。
- **对结果的影响**：不改变输出。流式构建与向量构建产出逐位一致的 `MatrixBuildResult`；
  启用时多一行 `[matrix-streaming]` 的 stderr 输出。
- **接入点**：`src/api/pipeline.cpp` 的 `Pipeline::matrix_phase()`。
- **测试**：`tests/test_sge_streaming.cpp` 覆盖流式与向量构建的等价性；
  `tests/test_api.cpp`、`tests/test_structured_ooc_50d_probe.cpp`、
  `tests/test_structured_filter_pipeline_120bit.cpp` 与
  `tests/test_candidate_batch_50d_sweep.cpp` 把变量钉为 `off` 以隔离各自的路由断言。
  没有测试通过 ENV 打开该开关。
