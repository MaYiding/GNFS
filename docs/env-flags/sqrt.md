# 平方根 (sqrt) 模块 ENV 调优开关

> 本文档收录 `sqrt` 模块所有 `GNFS_*` 运行时调优开关 / helper 的详细设计(算法、ENV 解析、bit-for-bit 保证、ROI、集成点、测试)。
>
> 返回: [ENV 开关总览](README.md) · [CLAUDE.md ENV 索引表](../../CLAUDE.md#env-调优开关总览)

---

## Hensel lift K-prime slot 并行 (GNFS_SQRT_HENSEL_THREADS)

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

---

## Couveignes pattern search 并行 (GNFS_COUVEIGNES_PARALLEL_THREADS)

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
