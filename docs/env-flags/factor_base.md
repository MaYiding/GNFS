# 因子基 (factor_base) 模块 ENV 调优开关

> 本文档收录 `factor_base` 模块所有 `GNFS_*` 运行时调优开关 / helper 的详细设计(算法、ENV 解析、bit-for-bit 保证、ROI、集成点、测试)。
>
> 返回: [ENV 开关总览](README.md) · [CLAUDE.md ENV 索引表](../../CLAUDE.md#env-调优开关总览)

---

## Factor Base CZ roots 并行 (GNFS_FB_ROOTS_THREADS)

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

### `FactorBaseBuilder::Options::parallel`

`FactorBaseBuilder::Options::parallel` 是 C++ 调用方的显式并行开关，默认值为
`true`。设置为 `false` 时，构建过程会同时关闭大范围分段筛和代数素数的跨素数根查找线程；
根查找仍按与并行路径相同的素数顺序合并，输出保持 bit-for-bit 一致。该选项适用于调试、
sanitizer 运行，以及调用方已经管理外层并发的场景。`GNFS_FB_ROOTS_THREADS` 只控制独立的
helper，不会覆盖 `Options::parallel = false` 的单线程契约。

**集成点** (2026-05-22):
- `include/gnfs/factor_base/fb_roots_parallel.hpp` — `fb_roots_threads()` env
  reader with `std::once_flag` cache + `resolve_fb_roots_threads(n)` helper +
  `parallel_fb_roots<Result>(primes, worker_fn)` template dispatcher +
  `fb_roots_threads_reset_env_cache_for_testing()` test hook
- `src/factor_base/builder.cpp` — 主路径消费 `Options::parallel`：默认值保持原有
  `std::thread` + `hardware_concurrency()` 行为，显式 `false` 时不创建并行工作线程。
  `GNFS_FB_ROOTS_THREADS` helper 仍是独立的 future-infra，不改变 `Options` 的契约
- `tests/test_fb_roots_parallel.cpp` — 12 个测试 (6 ENV 解析 + 5 dispatcher
  parity + 1 partial-parse 行为文档化)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60 s timeout,
  factor_base 模块
