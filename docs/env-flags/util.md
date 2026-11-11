# 工具 (util) 模块 ENV 调优开关

> 本文档收录 `util` 模块所有 `GNFS_*` 运行时调优开关 / helper 的详细设计(算法、ENV 解析、bit-for-bit 保证、ROI、集成点、测试)。
>
> 返回: [ENV 开关总览](README.md) · [CLAUDE.md ENV 索引表](../../CLAUDE.md#env-调优开关总览)

---

## Integer thread-local scratch pool (GNFS_INTEGER_SCRATCH_POOL)

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

---

## GMP mpz_powm 批量并行 (GNFS_MPZ_POWM_BATCH_THREADS)

`GNFS_MPZ_POWM_BATCH_THREADS` 控制 `parallel_mpz_powm` 的线程数。默认值为 1；
未设置、空值、非数字及非正数均采用默认值。解析沿用 `std::atoi` 的数字前缀语义，
例如 `"12abc"` 取 12，前导空白允许。正数上限为 `hardware_concurrency() * 2`，
硬件线程数未知时按 4 计算。结果保存在原子缓存中；测试可用
`mpz_powm_batch_threads_reset_env_cache_for_testing()` 在无调用运行时重置缓存。

`parallel_mpz_powm(bases, exp, modulus, results)` 处理前
`min(bases.size(), results.size())` 个元素。输入或输出为空时立即返回，
不复制操作数、不创建线程池，也不修改输出；多余的输出槽位保持原值。
调用方须保证 `modulus > 0`；其余算术条件沿用 GMP `mpz_powm`，
此 helper 不增加参数验证策略。

每个非空批次先各复制一次指数和模数，因此这两个参数可以引用输出中的元素，
也可以同时引用同一元素。输入与输出起始地址相同时支持原位运算，无需复制 bases。
起始地址不同但处理区间重叠时，先复制将要处理的 bases 前缀，避免写入覆盖后续输入；
使用 `std::less` 比较指针，避免依赖无关对象指针的内建大小比较。
不重叠的输入无需复制 bases。

线程数为 1 或仅处理一个元素时执行顺序循环。其余情况使用大小为
`min(threads, processed_count)` 的 `ThreadPool`，每个任务只写一个输出槽位，
共享标量读取不可变快照。快照和输入视图在线程池之前构造，确保提交异常时
线程池先完成清理；正常提交后收集全部 future，再传播首个任务异常。

**一致性与成本**：每项输出等于调用前的 `base^exp mod modulus`，线程数不影响结果。
每个非空批次有两次 `Integer` 标量复制；错位重叠时还需复制处理范围内的 bases。
顺序路径不创建线程池，但不是零开销。当前 `src/` 与 `include/` 没有生产调用点，
设置此变量不会自动加速 GNFS 主流水线；接入前需针对实际批次测量收益。

实现位于 `include/gnfs/util/mpz_powm_parallel.hpp`；
`tests/test_mpz_powm_parallel.cpp` 覆盖环境解析、标量参考值、线程间一致性、
共享标量别名、双向错位重叠、原位运算、短输出、空批次和宽整数。
检查使用 Release 下仍生效的 `GNFS_TEST_CHECK`。测试目录、层级和超时以
`./scripts/test.sh list` 为准，可通过 `./scripts/test.sh run test_mpz_powm_parallel` 验证。

---

## GMP mpz_invert 批量并行 (GNFS_MPZ_INVERT_BATCH_THREADS)

**ENV `GNFS_MPZ_INVERT_BATCH_THREADS=N`** (2026-05-22 实施, W12 T3, default 1, range [1, hardware_concurrency * 2]):
W11 T3 `GNFS_MPZ_POWM_BATCH_THREADS` 的姐妹 helper. GMP `mpz_invert(result,
base, modulus)` (modular inverse `base^{-1} mod modulus`) 在多个独立 base
之间相互独立 (embarrassingly parallel). 每次 `mpz_invert` 调用是
`(base, modulus)` 的 deterministic function, 满足 GMP per-call disjoint-
operands thread-safety 契约 (每个 worker 写自己 disjoint 的 result slot,
共享 `modulus` 仅 read). N=1 (默认) 走 sequential per-base 循环, 不创建
ThreadPool, 零开销保留原行为. N>=2 时把 K 个 base dispatch 到大小为
min(N, K) 的 ThreadPool, base 之间靠 future 同步收口.

```bash
GNFS_MPZ_INVERT_BATCH_THREADS=1 ./gnfs <N>    # default sequential, zero overhead
GNFS_MPZ_INVERT_BATCH_THREADS=4 ./gnfs <N>    # 4 workers for CZ root finding / ECM batch inv / Schirokauer normalisation
GNFS_MPZ_INVERT_BATCH_THREADS=8 ./gnfs <N>    # 8 workers
unset GNFS_MPZ_INVERT_BATCH_THREADS           # same as N=1
```

**与 W11 T3 powm dispatcher 的关键差异 — Failure semantics**:
`mpz_invert(out, base, modulus)` 在 `gcd(base, modulus) != 1` 时返回 0
(no inverse exists in (Z/modulusZ)^*). 这不是 "永远不发生" 的前置条件 —
GNFS pipeline 真实路径 (Cantor-Zassenhaus root finding 跨系数, ECM
Montgomery batch inversion 的 prefix product collision, Schirokauer base
normalisation, lattice basis Bezout) 都会真实触发此分支, 是 standard "lucky
factor" 提取信号. helper 通过返回 `std::vector<bool> success` 暴露这个分支:
- `success[i] == true`: `results[i]` 是合法 inverse mod modulus
- `success[i] == false`: `gcd(bases[i], modulus) > 1`, `results[i]` *不被
  写入* (caller 预填充的值保持原样). caller 可通过 `gcd(bases[i], modulus)`
  提取 nontrivial factor

W11 T3 `parallel_mpz_powm` 没有这个 success vector — `mpz_powm` 不会失败
(只要 modulus > 0, exp 任意), 所有 slot 永远 valid. 本 helper 必须额外
return path, 是 "result type" 的真实差异 (W11 T3 void return vs W12 T3
`std::vector<bool>`).

**并行模型**:
- Outer = `parallel_mpz_invert(bases, modulus, results)` over n bases
- Inner = `gnfs::util::ThreadPool` 大小为 min(N, bases.size()), 每 task 调
  `mpz_invert(results[i], bases[i], modulus)` 写到 disjoint `results[i]` slot
- 内部 GMP modular inverse 算法 bit-identical (helper 仅改变外层 dispatch,
  不触碰 `mpz_invert` 内核或 `gnfs::core::modinv` wrapper)
- 共享 `modulus` 仅由 worker read, 满足 "concurrent read 是安全的, 仅
  concurrent write 通过 alias `mpz_t` 才需要 disjoint operands" 的 GMP
  线程安全 invariant
- 空 batch (n==0) / 单 base (n==1) 都走 sequential 短路, 不创建 pool
- per-task success bit 经 `std::vector<char>` 暂存 (一字节一 slot,
  disjoint), 收口后再 copy 进 `std::vector<bool>`. 这绕开了
  `std::vector<bool>` packed bit 的并发写 race
- Exception path: dispatcher drain 全部 future, 第一个 thrown exception
  通过 `std::rethrow_exception` 传给 caller (不 swallow); pool 析构干净 join

**Bit-for-bit guarantee**: 每 base `mpz_invert` 是 pure function of
`(base, modulus)`, 不依赖 dispatch 顺序. Sequential (N=1) 与 parallel (N>=2)
路径产生的 per-index `(Integer, bool)` 对完全一致. 由
`tests/test_mpz_invert_parallel.cpp` 强制覆盖 (100-base random 200-bit prime
modulus N=1 vs N=4 vs N=hw 严格 per-index `mpz_cmp == 0` + success bit
完全一致 assert + composite modulus failure case 4-success/6-fail pattern
sequential vs parallel agreement).

**ROI 与定位**:
- 主要 ROI: 50d+/60d cofactor pipeline 与 Schirokauer maps 的 batch inversion
  hot path 上, modulus 100-300 bit, base wider. K base 并发后 outer wall ~
  T_max_base + tasking overhead, 替代 sum(K) sequential 累计. 对大 modulus
  收益更显著 (single-call cost 增加, pool overhead 占比下降, mpz_invert
  内部 Extended Euclidean 是 GMP per-call 最贵的操作之一).
- 与 W7/W8/W9/W10 T4/W11 T3/W11 T4 parallel dispatcher family 互补,
  本 helper 是第 6 名成员:
    * W7 `GNFS_SQRT_HENSEL_THREADS` — Hensel lift K-prime slot
    * W8 T1 `GNFS_ECM_STAGE2_PARALLEL` — ECM Stage 2 BSGS 多曲线
    * W9 T1 `GNFS_ECM_STAGE1_PARALLEL_THREADS` — ECM Stage 1 Lucas-chain 多曲线
    * W10 T4 `GNFS_FILTER_MERGE_THREADS` — LP-key bucket merge
    * W11 T3 `GNFS_MPZ_POWM_BATCH_THREADS` — batched `mpz_powm`
    * W11 T4 `GNFS_LATTICE_BASIS_PARALLEL_THREADS` — lattice basis reduction
    * W12 T3 `GNFS_MPZ_INVERT_BATCH_THREADS` — batched `mpz_invert`
  全部 default 1 (sequential), opt-in, 互不冲突. 可同时启用.
- Helper 是 opt-in 工具, **不修改** `gnfs::core::modinv` (existing free-function
  wrapper) / ECM Montgomery batch inversion / Cantor-Zassenhaus / Schirokauer
  / lattice basis 主路径. 调用方需要自己 batch up 一组 base (典型 a vector
  of per-relation `Integer`) + 共享 `modulus` 后传入 `parallel_mpz_invert`.
  当前主 pipeline 无 wire-in 调用, 是 future-infra.
- Default OFF (N=1) 保证 zero behavior change for legacy callers, 仅当用户
  明确 opt-in 时启用.

**集成点** (2026-05-22, W12 T3):
- `include/gnfs/util/mpz_invert_parallel.hpp` — `mpz_invert_batch_threads()`
  env reader with `std::once_flag` cache + `parallel_mpz_invert(bases,
  modulus, results) -> std::vector<bool>` dispatcher +
  `resolve_mpz_invert_batch_threads(batch_size)` helper +
  `mpz_invert_batch_threads_reset_env_cache_for_testing()` test hook
- `tests/test_mpz_invert_parallel.cpp` — 14 个测试 (5 env parsing 含 "12abc"
  partial parse / empty / single base N=1 / single base N=4 no-stall / N=1
  vs scalar mpz_invert reference / N=1 vs N=4 parity 200-bit prime modulus /
  N=1 vs N=hw parity / composite modulus failure case 4-success/6-fail seq
  vs par agreement / cache reset hook / perf info)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  util 模块

---

## GMP mpz_mod 批量并行 (GNFS_MPZ_MOD_BATCH_THREADS)

`GNFS_MPZ_MOD_BATCH_THREADS` 控制 `parallel_mpz_mod` 的线程数。默认值为 1；
未设置、空值、非数字及非正数均采用默认值。解析沿用 `std::atoi` 的数字前缀语义，
例如 `"12abc"` 取 12，前导空白允许。正数上限为 `hardware_concurrency() * 2`，
硬件线程数未知时按 4 计算。结果保存在原子缓存中；测试可用
`mpz_mod_batch_threads_reset_env_cache_for_testing()` 在无调用运行时重置缓存。
`resolve_mpz_mod_batch_threads(batch_size)` 返回线程数与批次大小的较小值，空批次返回 0。

`parallel_mpz_mod(dividends, modulus, results)` 计算各项的规范非负余数。
调用方须保证 `modulus > 0`，且 `dividends` 和 `results` 是不同向量。
模数允许引用输出元素：每个非空批次在调整输出大小或写入之前复制一次模数。
因此，输出扩容导致的引用失效和较早结果覆盖模数都不会影响本批次的计算。

输出过小时扩展至输入大小，多余输出槽位保持原值。空输入立即返回，
不复制模数、不调整输出大小，也不创建线程池。线程数为 1 或仅有一个输入时
执行顺序循环；其余情况使用大小为 `min(threads, dividends.size())` 的
`ThreadPool`。每个任务只写自己的输出槽位，共享不可变的模数快照。
快照在线程池之前构造，保证提交异常时仍存活到线程池清理完毕；正常提交后
收集全部 future，再传播首个任务异常。有效算术输入不需要逐项成功标记，
但分配和线程池异常仍会传播给调用方。

**一致性与成本**：每项输出等于以调用前模数计算的 `dividends[i] mod modulus`，
范围为 `[0, modulus)`，线程数不影响结果。每个非空批次有一次 `Integer` 模数复制，
不复制 dividends；顺序路径不创建线程池，但不是零开销。
当前 `src/` 与 `include/` 没有生产调用点，设置此变量不会自动加速 GNFS 主流水线；
接入前需针对实际批次测量收益。

实现位于 `include/gnfs/util/mpz_mod_parallel.hpp`；
`tests/test_mpz_mod_parallel.cpp` 覆盖环境解析、标量参考值、线程间一致性、
负数和宽整数、输出中保存的模数、强制输出扩容以及空输入和输出尾部保持。
检查使用 Release 下仍生效的 `GNFS_TEST_CHECK`。测试目录、层级和超时以
`./scripts/test.sh list` 为准，可通过 `./scripts/test.sh run test_mpz_mod_parallel` 验证。

---

## GMP mpz_gcd 批量并行 (GNFS_MPZ_GCD_BATCH_THREADS)

**ENV `GNFS_MPZ_GCD_BATCH_THREADS=N`** (2026-05-23 实施, W14 T5, default 1, range [1, hardware_concurrency * 2]):
W11 T3 `GNFS_MPZ_POWM_BATCH_THREADS`, W12 T3 `GNFS_MPZ_INVERT_BATCH_THREADS`
与 W13 T5 `GNFS_MPZ_MOD_BATCH_THREADS` 的兄弟 helper, parallel-dispatcher
家族第 10 名成员. GMP `mpz_gcd(result, a, b)` (greatest common divisor)
在多个独立 `(a, b)` 对之间相互独立 (embarrassingly parallel). 每次
`mpz_gcd` 调用是 `(a, b)` 的 deterministic pure function, 满足 GMP per-call
disjoint-operands thread-safety 契约 (每个 worker 写自己 disjoint 的
result slot, 读自己 disjoint 的两个 input slot). N=1 (默认) 走 sequential
per-pair 循环, 不创建 ThreadPool, 零开销保留原行为. N>=2 时把 K 个 pair
dispatch 到大小为 min(N, K) 的 ThreadPool, pair 之间靠 future 同步收口.

```bash
GNFS_MPZ_GCD_BATCH_THREADS=1 ./gnfs <N>    # default sequential, zero overhead
GNFS_MPZ_GCD_BATCH_THREADS=4 ./gnfs <N>    # 4 workers for relation-filter gcd / lucky-factor scan / Bezout
GNFS_MPZ_GCD_BATCH_THREADS=8 ./gnfs <N>    # 8 workers
unset GNFS_MPZ_GCD_BATCH_THREADS           # same as N=1
```

**Helper API** (`include/gnfs/util/mpz_gcd_parallel.hpp`):
- `mpz_gcd_batch_threads()` — cached `std::once_flag` + `std::atomic<int>`
  ENV reader, default 1, clamp `[1, hw*2]`
- `resolve_mpz_gcd_batch_threads(batch_size)` — 返回
  `min(threads, batch_size)`, empty batch (size==0) 返回 0
- `parallel_mpz_gcd(a_values, b_values, results)` — 主入口, void return
  (无 failure mode)
- `mpz_gcd_batch_threads_reset_env_cache_for_testing()` — 测试 re-resolve hook

**并行模型**:
- Outer = `parallel_mpz_gcd(a_values, b_values, results)` over n pairs
- Inner = `gnfs::util::ThreadPool` 大小为 min(N, pair count), 每 task 调
  `mpz_gcd(results[i], a_values[i], b_values[i])` 写到 disjoint
  `results[i]` slot
- 内部 GMP GCD 算法 bit-identical (helper 仅改变外层 dispatch, 不触碰
  `mpz_gcd` 内核或任何 `gnfs::core::Integer` 算术 operator)
- 输入两 array 仅由 worker read 自己 index 的 slot (没有共享 modulus
  这样的 broadcast 输入), 满足 GMP per-call disjoint-operands thread-safety
- 空 batch (n==0) / 单 pair (n==1) 都走 sequential 短路, 不创建 pool
- Exception path: dispatcher drain 全部 future, 第一个 thrown exception
  通过 `std::rethrow_exception` 传给 caller (不 swallow); pool 析构干净 join

**Bit-for-bit guarantee**: 每 pair `mpz_gcd` 是 pure function of `(a, b)`,
不依赖 dispatch 顺序. Sequential (N=1) 与 parallel (N>=2) 路径产生的
per-index `Integer` 完全一致. 由 `tests/test_mpz_gcd_parallel.cpp` 强制
覆盖 (100-pair random N=1 vs scalar reference / N=1 vs N=4 vs N=hw 严格
per-index `mpz_cmp == 0` assert, plus gcd(P*Q, P*R) = P 5 个 100-bit prime
pattern 多 limb 路径 parity).

**Failure semantics — 与 W12 T3 mpz_invert 关键差异 (与 W13 T5 mpz_mod 一致)**:
`mpz_gcd(out, a, b)` 是 *total* 函数 — 对全 domain `(a, b)` 都 well-defined.
标准 GMP 约定:
- `gcd(0, 0) = 0`
- `gcd(a, 0) = |a|`, `gcd(0, b) = |b|`
- 结果总是非负, 不受 `a` / `b` 符号影响 (`mpz_gcd` ignore signs)

与 W12 T3 `mpz_invert` 在 `gcd(base, modulus) != 1` 时 fail 不同, `mpz_gcd`
永远成功. 故本 helper 返回 `void`, 不需要 `std::vector<bool> success` 返回值
(与 W13 T5 mpz_mod 同). 调用方不需要处理 per-slot failure case.

**与 W13 T5 mpz_mod 的关键差异 — 输入形状**:
- W13 T5 `parallel_mpz_mod` 消费 dividend vector + 共享 modulus. 一个输入
  array 加一个 broadcast scalar.
- W14 T5 `parallel_mpz_gcd` 消费两个 array (`a_values` + `b_values`), 两者
  per-index 配对. 没有共享 broadcast 输入. precondition
  `a_values.size() == b_values.size()`, 不等抛 `std::invalid_argument`.
- 这是输入语义 surface 的真实差异 (单 array + scalar vs 两 parallel array).
  Result 类型相同 (Integer), failure mode 相同 (无).

**ROI 与定位**:
- 主要 ROI: 50d+/60d relation filtering 中 `gcd(a - b*m, N) > 1` 拒绝路径
  (百万条 relation 都要求 GCD), Schirokauer / lattice basis Bezout 系数
  计算, ECM Montgomery batch inversion 失败时 lucky-factor 扫 `gcd(v_i, n)`,
  Cantor-Zassenhaus root finding 跨系数 GCD. 这些场景都是大量独立 (a, b)
  对求 GCD 的 hot path. K pair 并发后 outer wall ~ T_max_pair + tasking
  overhead, 替代 sum(K) sequential 累计. 对大 operand (multi-limb GMP) 收益
  更显著 (single-call cost 增加, pool overhead 占比下降, mpz_gcd 内部
  binary GCD / Lehmer's 都是 GMP per-call 非平凡操作).
- 与 W7/W8/W9/W10 T4/W11 T3/W11 T4/W12 T3/W12 T4/W13 T5 parallel
  dispatcher family 互补, 本 helper 是第 10 名成员:
    * W7 `GNFS_SQRT_HENSEL_THREADS` — Hensel lift K-prime slot
    * W8 T1 `GNFS_ECM_STAGE2_PARALLEL` — ECM Stage 2 BSGS 多曲线
    * W9 T1 `GNFS_ECM_STAGE1_PARALLEL_THREADS` — ECM Stage 1 Lucas-chain 多曲线
    * W10 T4 `GNFS_FILTER_MERGE_THREADS` — LP-key bucket merge
    * W11 T3 `GNFS_MPZ_POWM_BATCH_THREADS` — batched `mpz_powm`
    * W11 T4 `GNFS_LATTICE_BASIS_PARALLEL_THREADS` — lattice basis reduction
    * W12 T3 `GNFS_MPZ_INVERT_BATCH_THREADS` — batched `mpz_invert`
    * W12 T4 `GNFS_SIEVE_APPLY_TILE_THREADS` — sieve apply tile
    * W13 T5 `GNFS_MPZ_MOD_BATCH_THREADS` — batched `mpz_mod`
    * W14 T5 `GNFS_MPZ_GCD_BATCH_THREADS` — batched `mpz_gcd` (本 helper)
  十者全部 default 1 (sequential), opt-in, 互不冲突. 可同时启用.
- Helper 是 opt-in 工具, **不修改** 任何 `gnfs::core::Integer` 算术 operator
  / `gnfs::core::gcd` (existing free-function wrapper) / relation filter /
  Bezout / CZ root finding / 任何 GCD call-site 主路径. 调用方需要自己
  batch up `(a_i, b_i)` 对后传入 `parallel_mpz_gcd`. 当前主 pipeline 无
  wire-in 调用, 是 future-infra.
- Default OFF (N=1) 保证 zero behavior change for legacy callers, 仅当用户
  明确 opt-in 时启用.

**集成点** (2026-05-23, W14 T5):
- `include/gnfs/util/mpz_gcd_parallel.hpp` — `mpz_gcd_batch_threads()` env
  reader with `std::once_flag` cache + `parallel_mpz_gcd(a_values, b_values,
  results)` dispatcher + `resolve_mpz_gcd_batch_threads(batch_size)` helper +
  `mpz_gcd_batch_threads_reset_env_cache_for_testing()` test hook
- `tests/test_mpz_gcd_parallel.cpp` — 16 个测试 (5 env parsing 含 leading
  whitespace 与 "12abc" partial parse / empty / single pair N=1 / single
  pair N=4 no-stall / N=1 vs scalar mpz_gcd 5 boundary
  (gcd(0,0)=0 / gcd(a,0)=|a| / gcd(0,b)=|b| / gcd(12,18)=6 / negative
  operand → non-negative result) / N=1 vs N=4 parity / N=1 vs N=hw parity /
  gcd(P*Q, P*R)=P pattern 5 cases 100-bit primes 多 limb / mismatched span
  size throws invalid_argument / results undersized defensive clamp /
  reset env cache hook / perf info)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  util 模块

---

## GMP mpz_mul 批量并行 (GNFS_MPZ_MUL_BATCH_THREADS)

**ENV `GNFS_MPZ_MUL_BATCH_THREADS=N`** (2026-05-23 实施, W15 T5, default 1, range [1, hardware_concurrency * 2]):
W11 T3 `GNFS_MPZ_POWM_BATCH_THREADS`, W12 T3 `GNFS_MPZ_INVERT_BATCH_THREADS`,
W13 T5 `GNFS_MPZ_MOD_BATCH_THREADS` 与 W14 T5 `GNFS_MPZ_GCD_BATCH_THREADS`
的兄弟 helper, parallel-dispatcher 家族第 12 名成员 (与同波 W15 T3
`GNFS_BRENT_POLLARD_RHO_THREADS` 第 11 名并列 W15 引入). GMP `mpz_mul(result,
a, b)` (algebraic product `a * b`) 在多个独立 `(a, b)` 对之间相互独立
(embarrassingly parallel). 每次 `mpz_mul` 调用是 `(a, b)` 的 deterministic
pure function, 满足 GMP per-call disjoint-operands thread-safety 契约
(每个 worker 写自己 disjoint 的 result slot, 读自己 disjoint 的两个 input
slot). N=1 (默认) 走 sequential per-pair 循环, 不创建 ThreadPool, 零开销
保留原行为. N>=2 时把 K 个 pair dispatch 到大小为 min(N, K) 的 ThreadPool,
pair 之间靠 future 同步收口.

```bash
GNFS_MPZ_MUL_BATCH_THREADS=1 ./gnfs <N>    # default sequential, zero overhead
GNFS_MPZ_MUL_BATCH_THREADS=4 ./gnfs <N>    # 4 workers for accumulator chains / prefix products / batched products
GNFS_MPZ_MUL_BATCH_THREADS=8 ./gnfs <N>    # 8 workers
unset GNFS_MPZ_MUL_BATCH_THREADS           # same as N=1
```

**Helper API** (`include/gnfs/util/mpz_mul_parallel.hpp`):
- `mpz_mul_batch_threads()` — cached `std::once_flag` + `std::atomic<int>`
  ENV reader, default 1, clamp `[1, hw*2]`
- `resolve_mpz_mul_batch_threads(batch_size)` — 返回
  `min(threads, batch_size)`, empty batch (size==0) 返回 0
- `parallel_mpz_mul(a_values, b_values, results)` — 主入口, void return
  (无 failure mode)
- `mpz_mul_batch_threads_reset_env_cache_for_testing()` — 测试 re-resolve hook

**并行模型**:
- Outer = `parallel_mpz_mul(a_values, b_values, results)` over n pairs
- Inner = `gnfs::util::ThreadPool` 大小为 min(N, pair count), 每 task 调
  `mpz_mul(results[i], a_values[i], b_values[i])` 写到 disjoint
  `results[i]` slot
- 内部 GMP multiplication 算法 bit-identical (helper 仅改变外层 dispatch,
  不触碰 `mpz_mul` 内核或任何 `gnfs::core::Integer` 算术 operator)
- 输入两 array 仅由 worker read 自己 index 的 slot (没有共享 broadcast
  输入, 与 W14 T5 mpz_gcd / W15 T3 brent rho 同), 满足 GMP per-call
  disjoint-operands thread-safety
- 空 batch (n==0) / 单 pair (n==1) 都走 sequential 短路, 不创建 pool
- Exception path: dispatcher drain 全部 future, 第一个 thrown exception
  通过 `std::rethrow_exception` 传给 caller (不 swallow); pool 析构干净 join

**Bit-for-bit guarantee**: 每 pair `mpz_mul` 是 pure function of `(a, b)`,
不依赖 dispatch 顺序. Sequential (N=1) 与 parallel (N>=2) 路径产生的
per-index `Integer` 完全一致. 由 `tests/test_mpz_mul_parallel.cpp` 强制
覆盖 (100-pair random N=1 vs scalar reference / N=1 vs N=4 vs N=hw 严格
per-index `mpz_cmp == 0` assert, plus P*Q pattern 5 个 100-bit prime
对多 limb 路径 parity).

**Failure semantics — 与 W12 T3 mpz_invert 关键差异 (与 W13 T5 mpz_mod 与 W14 T5 mpz_gcd 一致)**:
`mpz_mul(out, a, b)` 是 *total* 函数 — 对全 domain `(a, b)` 都 well-defined.
标准 GMP 约定:
- `0 * 0 = 0`, `a * 0 = 0`, `0 * b = 0`
- 结果符号由两 operand 符号决定: 同号正, 异号负, 任一为 0 则结果为 0
- 不像 `mpz_gcd` 总返回非负值, `mpz_mul` 保留代数符号

与 W12 T3 `mpz_invert` 在 `gcd(base, modulus) != 1` 时 fail 不同, `mpz_mul`
永远成功. 故本 helper 返回 `void`, 不需要 `std::vector<bool> success` 返回值
(与 W13 T5 mpz_mod 与 W14 T5 mpz_gcd 同). 调用方不需要处理 per-slot
failure case.

**与 W14 T5 mpz_gcd 的关键差异 — 输入形状一致但语义不同**:
- 输入形状: 完全一致 (双 array `a_values` + `b_values`, per-index 配对,
  void 返回, 无 failure mode). precondition `a_values.size() ==
  b_values.size()`, 不等抛 `std::invalid_argument`.
- 底层 GMP primitive: `mpz_gcd` 是 GCD (忽略符号, 永远非负), `mpz_mul`
  是代数乘积 (保留符号, 异号产生负积, 任一为 0 则为 0).
- 典型 caller 工作负载: gcd 用于 "lucky factor" 扫, Bezout, 关系过滤
  rejection; mul 用于累加器链, prefix product, 否则会串行化于单
  `mpz_mul` per iteration 的 batched product.

**ROI 与定位**:
- 主要 ROI: 50d+/60d Schirokauer maps 大整数累加器, Cantor-Zassenhaus
  根查找 cross-coefficient product 链, ECM Montgomery batch inversion
  的 prefix product 计算 (在 invert 之前 helper 一步一步构建
  `p_i = prod(v_0..v_i)`), Bezout 系数乘法, 与 lattice basis update.
  这些场景都是大量独立 `(a, b)` 对求积的 hot path. K pair 并发后 outer
  wall ~ T_max_pair + tasking overhead, 替代 sum(K) sequential 累计. 对
  大 operand (multi-limb GMP) 收益更显著 (single-call cost 增加, pool
  overhead 占比下降, mpz_mul 内部 Toom-Cook / FFT 算法是 GMP per-call
  非平凡操作, 中大 operand 上拐点出现).
- 与 W7/W8/W9/W10 T4/W11 T3/W11 T4/W12 T3/W12 T4/W13 T5/W14 T5/W15 T3
  parallel dispatcher family 互补, 本 helper 是第 12 名成员:
    * W7 `GNFS_SQRT_HENSEL_THREADS` — Hensel lift K-prime slot
    * W8 T1 `GNFS_ECM_STAGE2_PARALLEL` — ECM Stage 2 BSGS 多曲线
    * W9 T1 `GNFS_ECM_STAGE1_PARALLEL_THREADS` — ECM Stage 1 Lucas-chain 多曲线
    * W10 T4 `GNFS_FILTER_MERGE_THREADS` — LP-key bucket merge
    * W11 T3 `GNFS_MPZ_POWM_BATCH_THREADS` — batched `mpz_powm`
    * W11 T4 `GNFS_LATTICE_BASIS_PARALLEL_THREADS` — lattice basis reduction
    * W12 T3 `GNFS_MPZ_INVERT_BATCH_THREADS` — batched `mpz_invert`
    * W12 T4 `GNFS_SIEVE_APPLY_TILE_THREADS` — sieve apply tile
    * W13 T5 `GNFS_MPZ_MOD_BATCH_THREADS` — batched `mpz_mod`
    * W14 T5 `GNFS_MPZ_GCD_BATCH_THREADS` — batched `mpz_gcd`
    * W15 T3 `GNFS_BRENT_POLLARD_RHO_THREADS` — batched Brent rho 配置
    * W15 T5 `GNFS_MPZ_MUL_BATCH_THREADS` — batched `mpz_mul` (本 helper)
  十二者全部 default 1 (sequential), opt-in, 互不冲突. 可同时启用.
- Helper 是 opt-in 工具, **不修改** 任何 `gnfs::core::Integer` 算术 operator
  / Schirokauer maps / matrix-builder / Cantor-Zassenhaus / 任何 mul
  call-site 主路径. 调用方需要自己 batch up `(a_i, b_i)` 对后传入
  `parallel_mpz_mul`. 当前主 pipeline 无 wire-in 调用, 是 future-infra.
- Default OFF (N=1) 保证 zero behavior change for legacy callers, 仅当用户
  明确 opt-in 时启用.

**集成点** (2026-05-23, W15 T5):
- `include/gnfs/util/mpz_mul_parallel.hpp` — `mpz_mul_batch_threads()` env
  reader with `std::once_flag` cache + `parallel_mpz_mul(a_values, b_values,
  results)` dispatcher + `resolve_mpz_mul_batch_threads(batch_size)` helper +
  `mpz_mul_batch_threads_reset_env_cache_for_testing()` test hook
- `tests/test_mpz_mul_parallel.cpp` — 16 个测试 (5 env parsing 含 leading
  whitespace 与 "12abc" partial parse / empty / single pair N=1 / single
  pair N=4 no-stall / N=1 vs scalar mpz_mul 5 boundary
  (0*0=0 / a*0=0 / 0*b=0 / 12*18=216 / negative * positive → negative
  product 符号传播) / N=1 vs N=4 parity / N=1 vs N=hw parity /
  P*Q pattern 5 cases 100-bit primes 多 limb / mismatched span size
  throws invalid_argument / results undersized defensive clamp /
  reset env cache hook / 1000-pair 200-bit perf info)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  util 模块
