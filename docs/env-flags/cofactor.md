# 余因子分解 (cofactor) 模块 ENV 调优开关

> 本文档收录 `cofactor` 模块所有 `GNFS_*` 运行时调优开关 / helper 的详细设计(算法、ENV 解析、bit-for-bit 保证、ROI、集成点、测试)。
>
> 返回: [ENV 开关总览](README.md) · [CLAUDE.md ENV 索引表](../../CLAUDE.md#env-调优开关总览)

---

## Trial division SIMD 8-prime batch (GNFS_TRIAL_DIV_SIMD)

**ENV `GNFS_TRIAL_DIV_SIMD=auto|0|1`** (2026-05-22 实施, W9 T3, default auto):
Cofactor pipeline 入口 (`include/gnfs/cofactor/`) 在 SQUFOF / ECM 之前
先扫小素数池做 trial division. helper `batch_check_divisibility`
把每 4 个 prime 批量 load 进一个 SIMD 寄存器 (NEON `uint32x4_t` 在
ARM64; AVX2 / SSE2 4-lane 在 x86), per-lane 再走 scalar `cofactor % p`
(NEON / AVX2 / SSE2 均不加速 uint32 除法), 输出 bit-for-bit 与 scalar
reference 一致.

```bash
GNFS_TRIAL_DIV_SIMD=auto ./gnfs <N>   # 默认: NEON/AVX2/SSE2 可用则启用
GNFS_TRIAL_DIV_SIMD=0    ./gnfs <N>   # 强制 scalar (回归 bisect 用)
GNFS_TRIAL_DIV_SIMD=1    ./gnfs <N>   # 强制 SIMD (无 SIMD 平台 fallback)
unset GNFS_TRIAL_DIV_SIMD             # 同 auto
```

**Helper API** (`include/gnfs/cofactor/trial_div_simd.hpp`):
- `batch_check_divisibility(cofactor, primes, out_divisible_indices)` —
  主入口, 内部三态 gate 路由到 SIMD 或 scalar 路径, 输出 indices 按
  input 顺序 append (不清空 out).
- `batch_check_divisibility_scalar(...)` — scalar reference, 测试 golden
  也供希望显式禁 SIMD 的 caller 使用.
- `trial_div_simd_mode()` / `trial_div_simd_enabled()` — cached
  `std::once_flag` + `std::atomic<int>` ENV reader, 严格 "0"/"1" parsing,
  其它值 (unset / "" / "auto" / "garbage" / "2" / "true") 均视为 Auto.
- `trial_div_simd_supported()` — compile-time `__ARM_NEON` / `__AVX2__` /
  `__SSE2__` 探测.
- `trial_div_simd_reset_env_cache_for_testing()` — 测试专用 re-resolve ENV.

**Bit-for-bit guarantee**: SIMD 仅 batch load + register allocation,
inner `cofactor % p` 与 scalar 路径同函数. 单元测试
`tests/test_trial_div_simd.cpp` 13 个测试强制覆盖 (5 ENV / empty /
single / 8-prime mixed / 100 cofactor x 30 prime 大 sweep / 1000-prime
batch / 0..8 boundary sweep / ForceOff 路径 / append 语义).

**ROI 与定位**:
- helper-only future-infrastructure. 当前主 pipeline cofactor 入口
  (`trial_division.hpp` / `batch_trial.hpp`) 未 wire-in, 行为完全不变.
- 当 caller 显式 wire-in 时 ROI 主要在 retired uop 数 (4 个 lane 的
  prime load + index extract 由 SIMD register 批量完成, 避免 4 次独立
  memory load 的 address-gen 串联). 实际 wall-time 提升依赖具体调用
  pattern (50d+/60d cofactor 短池 trial < 1µs/cofactor, 几 % 改进).
- 默认 auto 在 macOS arm64 / Linux x86_64 二者都 enable;
  ENV=0 用于 PMU sweep / sanitizer 回归 bisect.

**集成点** (2026-05-22):
- `include/gnfs/cofactor/trial_div_simd.hpp` — helper API + 三态 ENV gate
  + NEON / AVX2 / SSE2 inner kernel + scalar reference.
- `tests/test_trial_div_simd.cpp` — 13 instant tier tests, TIMEOUT 60.
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  cofactor 模块.

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点,
ENV 对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

---

## Cofactor survival rate predictor (GNFS_SURVIVAL_FILTER + GNFS_SURVIVAL_THRESHOLD)

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

---

## Cofactor per-stage timing telemetry (GNFS_COFACTOR_TIMING_ENABLE)

**ENV `GNFS_COFACTOR_TIMING_ENABLE={0,1}`** (2026-05-22 实施, W12 T5, default 0):
余因子 pipeline 6 个 stage (TrialDivision, SQUFOF, BrentPollardRho, PollardRho,
EcmStage1, EcmStage2) 的 wall-time 与 call-count 累积器. 每个 stage 一个
`std::atomic<uint64_t>` 纳秒计数 + 一个 `std::atomic<uint64_t>` 调用计数,
RAII `StageTimer` 在 scope 入口采 steady_clock, scope 退出累加 elapsed 到
对应 stage. 关闭时 (默认) 完全零开销 — ctor 与 dtor 都不调 `steady_clock::now()`,
不访问 atomic 计数. 仅当用户 `GNFS_COFACTOR_TIMING_ENABLE=1` 显式启用时才采集.

```bash
GNFS_COFACTOR_TIMING_ENABLE=1 ./gnfs <N>   # 启用 telemetry, scope 入退采样
unset GNFS_COFACTOR_TIMING_ENABLE          # default OFF, 零开销
GNFS_COFACTOR_TIMING_ENABLE=0 ./gnfs <N>   # 显式 disable (= default)
```

**Helper API** (`include/gnfs/cofactor/stage_timing.hpp`):
- `enum class CofactorStage`: `TrialDivision` (0), `Squfof` (1),
  `BrentPollardRho` (2), `PollardRho` (3), `EcmStage1` (4), `EcmStage2` (5),
  `kNumStages` (6, sentinel).
- `stage_name(stage)` — human-readable 名称 (`"trial"`, `"squfof"`,
  `"brent_rho"`, `"pollard_rho"`, `"ecm_s1"`, `"ecm_s2"`).
- `struct StageTimingStats` — 进程单例, 6 个 atomic ns 累加器 + 6 个 atomic
  调用计数. `total_ns_for(stage)` / `call_count_for(stage)` / `reset()`.
- `cofactor_timing_enabled()` — cached `std::call_once` + `std::atomic<bool>`
  ENV reader. 严格仅 "1" 启用.
- `cofactor_timing_stats()` — process-singleton 访问器 (function-local
  static, 与 `survival_stats()` 同 idiom).
- `cofactor_timing_reset_env_cache_for_testing()` — 测试专用 re-resolve ENV.
- `class StageTimer` — RAII 测时. Ctor 在 enabled 时采 steady_clock, dtor
  累加 elapsed_ns 与 +1 call_count. Non-copyable, movable. `moved_from_`
  标志防 move 后双重计数.
- `format_cofactor_timing_summary()` — 单行格式化:
  `[cofactor_timing] trial=<ns>ns/<calls>calls squfof=... ...`. 关闭时返回
  `"[cofactor_timing] disabled"`.
- `print_cofactor_timing_summary()` — 写 stderr + `std::flush`.

**Process-singleton storage 策略**:
- `cofactor_timing_stats()` 用 function-local static `StageTimingStats stats`
  (与 `survival_predictor.hpp::survival_stats()` 同 pattern). 优势: 保证
  thread-safe C++11 一次性初始化, ODR-safe 跨 TU. 选 fn-local static 而非
  `inline namespace var` 仅因为 cofactor 模块其他 telemetry 单例已经
  约定如此, 保持一致.

**Memory ordering**:
- 所有原子操作用 `std::memory_order_relaxed`. Telemetry 不驱动 control
  flow, 仅用于 format summary; 不同线程的累加最终一致即可, 不需要 release/acquire
  fence 制造 happens-before 关系.

**与 W5 T5 GNFS_SURVIVAL_FILTER 互补**:
- W5 T5 survival predictor 估算 cofactor 是否值得跑全 pipeline (前置筛选).
- W12 T5 telemetry 测量 cofactor pipeline 各 stage 真实耗时.
- 二者组合让用户调 `GNFS_SURVIVAL_THRESHOLD` 时观察"提高 threshold 是否
  真的把 ECM Stage 2 的累计 wall-time 砍掉 70%". 默认 OFF 时二者都零开销.

**Bit-for-bit guarantee**: telemetry 不改变 cofactor pipeline 任何行为,
仅累加测量数据. enabled / disabled 状态对 `classify_cofactor` 等 cofactor
入口的输出 (CofactorClassification) 严格一致. helper 不修改任何 cofactor
算法文件.

**Nested timer 语义**: 嵌套 `StageTimer` (e.g. TrialDivision 内嵌 EcmStage1)
各自独立累加. 外层 timer 包含内层时间 (调用方按需放置 timer 决定归属).

**Concurrent timer**: 多 thread 各自构造 `StageTimer`, 相同 stage 的 atomic
计数无锁累加. 4 thread × 100 timers 强制测试通过.

**集成点** (W12 T5, 2026-05-22):
- `include/gnfs/cofactor/stage_timing.hpp` — 240+ 行 header-only, 6-stage
  enum + atomic stats + RAII timer + ENV gate + summary formatter
- `tests/test_cofactor_stage_timing.cpp` — 16 tests (5 ENV / disabled 不动
  计数 / enabled 累加正确 / 嵌套独立 / 4 thread × 100 并发 / format 关 vs
  开 / reset zeros / move-construct 不重复 / move-assign 完成前次 / stage_name
  lookup / perf info scope overhead)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  cofactor 模块

**Default OFF**: ENV unset → `cofactor_timing_enabled() == false` →
`StageTimer` ctor 仅 1 atomic load 后立即返回 (不采 clock), dtor 同样
no-op. 主路径 wall-time 与 legacy 等价, 零行为变化. 当前主 pipeline 无
wire-in 调用, 是 future-infrastructure. 调用方在自身 cofactor stage scope
入口 `StageTimer t(CofactorStage::EcmStage1);` 即可启用归属.

---

## ECM B1 prime-power expansion cache (GNFS_ECM_B1_CACHE_SIZE)

**ENV `GNFS_ECM_B1_CACHE_SIZE=N`** (2026-05-22 实施, W13 T3, range [0, 32], default 0):
ECM Stage 1 内部需要计算 `k = lcm(1, 2, ..., B1) = ∏ p^⌊log_p B1⌋ for primes p ≤ B1`
来跑 `k * Q` scalar multiplication. 真实 caller (`ECM::stage1`, `try_curve_with_pk`)
通常 iterate prime power `p^e` 一条一条 (per-prime Lucas chain) 而非
materialize 整个 `k`. 当 multiple curves 共享 same B1 (典型 `EcmCurvePool::
prepare_batch` 批量 ECM), 每条 curve 重新跑 sieve + per-prime max-exponent
loop 是浪费. helper 提供 opt-in thread-safe insert-only cache, key = B1,
value = `std::vector<uint64_t>` 按升序素数排列的 prime-power 序列.

```bash
unset GNFS_ECM_B1_CACHE_SIZE              # default 0 (disabled, 零开销)
GNFS_ECM_B1_CACHE_SIZE=0    ./gnfs <N>    # 同 default
GNFS_ECM_B1_CACHE_SIZE=4    ./gnfs <N>    # 容量 4 (典型 ECM B1 set {1e4, 1e5, 1e6, 1e7})
GNFS_ECM_B1_CACHE_SIZE=32   ./gnfs <N>    # 上限
GNFS_ECM_B1_CACHE_SIZE=33   ./gnfs <N>    # clamp 到 32
```

**Helper API** (`include/gnfs/cofactor/ecm_prime_cache.hpp`):
- `compute_b1_prime_powers(B1)` — pure deterministic function, 返回升序素数
  prime power 序列. B1=0/1 返回 empty; B1=20 返回 [16, 9, 5, 7, 11, 13, 17, 19]
  (primes 2/3/5/7/11/13/17/19, 各自 max exp s.t. p^e ≤ 20). 不依赖 cache,
  适用于一次性计算或 cache disabled 场景.
- `EcmB1PrimeCache(capacity)` — mutex-protected insert-only map<B1, vector>.
  `get_or_compute(B1)`: 命中返回 cached vector ref (lifetime 与 cache 一致);
  未命中且未满 insert 后返回 ref; 未命中且满则计算后写入单 slot overflow buffer,
  返回 overflow ref (下次满 miss 时被覆盖, reference invalidation 文档化).
- `EcmB1PrimeCache::size() / capacity() / clear()` — 测试 / debug helper.
  `clear()` 释放所有 cached vectors 并 invalidate 之前返回的所有 references.
- `ecm_b1_cache_size()` — cached `std::once_flag` + `std::atomic<int>` ENV
  reader, 解析 `GNFS_ECM_B1_CACHE_SIZE`. 0 = disabled.
- `ecm_b1_cache_enabled()` — `ecm_b1_cache_size() > 0` 等价 predicate.
- `shared_ecm_b1_cache()` — process-singleton 访问器 (function-local static,
  与 W5 T5 `survival_stats()` / W12 T5 `cofactor_timing_stats()` 同 idiom).
  容量在首次调用时由 `ecm_b1_cache_size()` 决定, 进程 lifetime 固定.
- `ecm_b1_cache_reset_env_cache_for_testing()` — 测试专用 re-resolve ENV.

**算法 (Eratosthenes 素数筛 + 最大 exponent loop)**:
- `sieve_primes_up_to(B1)`: 标准 Eratosthenes 筛, O(B1 log log B1).
  Hard cap B1 ≤ 100_000_000 防止 runaway allocation (典型 ECM B1 上限 ~1e7).
- `prime_power_at_most(p, B1)`: `p^e = max k s.t. p^k ≤ B1`. 用安全 `acc > B1 / p`
  检查避免 uint64_t 溢出.

**ENV parsing 规则** (严格, std::stoi 风格):
- unset / "" / "0" / 负数 / leading 非数字 ("garbage" / "abc123") → 0
- leading 空白 (" 4" / "\t8") → 0 (主动 reject 与 W12 T1 linalg_progress 一致)
- "1".."32" → as-is
- "33"+ / "999999" → 32 (clamp)
- "12abc" → 12 (std::stoi 接受前缀, 文档化但 caller 应传 clean 值)

**Bit-for-bit guarantee**: `compute_b1_prime_powers(B1)` 是 deterministic
pure function of B1. 同一 B1 多次 call 输出 byte-identical. Cache hit 路径
返回 ref 与 cache miss 路径返回 vector 在 element-wise 完全一致. 单元
测试 `tests/test_ecm_prime_cache.cpp` 强制覆盖 (17 个 test, B1=0/1/2/3/10/
20/100 prime count 与 literal sequence + cache hit/miss 行为 + thread safety
4 thread × 100 lookup).

**ROI 与定位**:
- 主要 ROI: 多条 ECM curve 共享 B1 时 amortise prime sieve + exponent loop.
  perf-info 实测 B1=10000 hit ~10ns vs miss ~30-50µs (~3000-5000x), Eratosthenes
  + per-prime exponent loop 是 measurable wall-time. 典型 ECM batch (10-100
  curves, B1=1e6) 共享 cache 整体节省 ~10ms-1s.
- helper 当前 standalone (主路径 `ECM::stage1` / `try_curve_with_pk` 未
  wire-in), 是 future-infrastructure. wire-in 时调用方:
  ```cpp
  if (ecm_b1_cache_enabled()) {
      const auto& powers = shared_ecm_b1_cache().get_or_compute(B1);
      for (uint64_t pe : powers) {
          point_multiply_in_place(Q, pe, n);
      }
  } else {
      // 原 per-curve sieve + exp loop
  }
  ```
- 与 W10 T3 `GNFS_ECM_SIGMA_POOL_SIZE` / W8 T1 `GNFS_ECM_STAGE2_PARALLEL` /
  W9 T1 `GNFS_ECM_STAGE1_PARALLEL_THREADS` 完全 orthogonal — sigma pool 缓存
  PRNG 输出, prime cache 缓存 prime power 列表, stage parallel 跑多 curve.
  三者可同时启用.

**Thread safety**:
- `EcmB1PrimeCache::get_or_compute` 用内部 `std::mutex`, 多 thread 并发 lookup
  serialise on mutex 但 hit 路径快 (hash lookup + ref return).
- value 用 `std::unique_ptr<std::vector<uint64_t>>` 间接存储, 即使 underlying
  `std::unordered_map` rehash 也保持 cached vector 地址稳定 (返回 ref 不失效).
- `shared_ecm_b1_cache()` function-local static 保证 C++11 thread-safe 一次性
  初始化, ODR-safe 跨 TU.
- 4 thread × 100 lookup 强制测试通过.

**Overflow 语义** (cache 满 + 新 B1 miss):
- 设计选择: 不 insert (保持 capacity 严格约束), 但仍需返回 ref. 解决方案:
  cache 持有单 slot `std::vector<uint64_t> overflow_`, 满时 miss 把计算结果
  move 到 overflow 返回 ref. 下次满 miss 时 overflow 被覆盖, 之前返回的
  overflow ref 失效 (文档化 hazard).
- Cached entries (in-map) 的 ref 永远稳定到 `clear()` 调用为止.
- 典型生产 caller 不会 hit overflow path (B1 working set ~4-8 个, capacity
  4-32 充足); overflow 是 corner case 安全网而非主流路径.

**集成点** (W13 T3, 2026-05-22):
- `include/gnfs/cofactor/ecm_prime_cache.hpp` — 295 行 header-only helper,
  pure compute + thread-safe cache + ENV gate + process singleton
- `tests/test_ecm_prime_cache.cpp` — 17 个测试 (6 ENV 解析 / 5 compute
  correctness 含 literal sequence / 6 cache 行为 + thread safety + perf info)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  cofactor 模块

**Default OFF (N=0)**: ENV unset → `ecm_b1_cache_size() == 0` →
`ecm_b1_cache_enabled() == false`. 调用方主路径完全不变, helper-only
future-infra. 仅 caller 显式 wire-in + 用户 explicit
`GNFS_ECM_B1_CACHE_SIZE=N>=1` 时启用.

---

## Cofactor classification result cache (GNFS_COFACTOR_RESULT_CACHE_SIZE)

**ENV `GNFS_COFACTOR_RESULT_CACHE_SIZE=N`** (2026-05-23 实施, W14 T3, range [0, 1048576], default 0):
`classify_cofactor(cofactor, smoothness_bound, large_prime_bound)` 是
deterministic pure function (固定 process 全局 state 时). 真实 GNFS sieve loop
中相同 `(cofactor, B, lp_bound)` tuple 会被反复查询 —— adaptive sieve 多 round
重复访问同一小余因子, bucket merge 再 flush 同一 residual cofactor.
每次查询走完整 cofactor pipeline (trial → SQUFOF → Brent rho → Pollard rho
→ ECM Stage 1+2) 是 µs 到 ms 量级. opt-in LRU cache 让重复查询 O(1) hash
lookup 直接返回, 跳过整个 pipeline.

```bash
unset GNFS_COFACTOR_RESULT_CACHE_SIZE              # default 0 (disabled, 零开销)
GNFS_COFACTOR_RESULT_CACHE_SIZE=0       ./gnfs <N> # 同 default
GNFS_COFACTOR_RESULT_CACHE_SIZE=1024    ./gnfs <N> # 1K 容量 (典型小批量)
GNFS_COFACTOR_RESULT_CACHE_SIZE=65536   ./gnfs <N> # 64K 容量 (50d/60d 大批量)
GNFS_COFACTOR_RESULT_CACHE_SIZE=1048576 ./gnfs <N> # 1M 上限
GNFS_COFACTOR_RESULT_CACHE_SIZE=9999999 ./gnfs <N> # clamp 到 1M
```

**Helper API** (`include/gnfs/cofactor/result_cache.hpp`):
- `class CofactorResultCache(capacity)` — capacity == 0 表 disabled (允许
  singleton 在未启用时仍可构造). capacity > 0 时是 textbook LRU.
- `std::optional<CofactorClassification> get(cofactor, B, lp_bound)` —
  命中: `splice` 到 list front (MRU promote), 返回 value-copy. 未命中:
  `std::nullopt`. Disabled cache 总是返回 nullopt.
- `void put(cofactor, B, lp_bound, result)` — 存在 key 时更新 value + 提升
  MRU. 新 key + 未满: `push_front` + insert. 新 key + 已满: evict
  `list.back()` (LRU), 然后 `push_front`. Disabled cache 是 no-op.
- `size() / capacity() / clear()` — 测试 / debug helper.
- `cofactor_result_cache_size()` — cached `std::once_flag` + `std::size_t`
  ENV reader.
- `cofactor_result_cache_enabled()` — `size() > 0` predicate.
- `cofactor_result_cache_reset_env_cache_for_testing()` — 测试 re-resolve hook.
- `shared_cofactor_result_cache()` — process-singleton 访问器 (function-local
  static, 与 W5 T5 `survival_stats()` / W12 T5 `cofactor_timing_stats()` /
  W13 T3 `shared_ecm_b1_cache()` 同 idiom). Capacity 在首次调用时由
  `cofactor_result_cache_size()` 决定, 进程 lifetime 固定.

**Key 设计** (3-tuple `{uint64 cofactor, uint32 B, uint32 lp}`):
- `cofactor` (uint64): 真实超 uint64 的 `Integer` cofactor 已被主 pipeline
  路由到 Composite / TooLarge 分支, 不进入 expensive subfactor 探测, 因此
  cache 仅服务 uint64-fits cofactor 路径.
- `B` (uint32): smoothness_bound. 两个相同 cofactor 在不同 B 下分类可能不
  同 (survival predictor TooLarge 边界依赖 B).
- `lp` (uint32): large_prime_bound. 同一 cofactor 在不同 lp 下分类可能不
  同 (Prime vs TooLarge 边界依赖 lp).
- Hash: 三字段折成单 uint64 (`cofactor ^ (B<<32) ^ (lp<<16)`) 后走 W11 T5
  splitmix64 (Stafford Mix 13) — 与 lp_key_hash 同算法, 避免
  `std::hash<uint64_t>` near-identity.

**ENV parsing 规则** (与 W12 T5 / W13 T3 一致, 严格 std::stoi):
- unset / "" / "0" / 负数 / 非数字 (`garbage`) / leading 空白 (`"  100"`) → 0 (disabled)
- "1".."1048576" → as-is
- "1048577"+ / "9999999" → 1048576 (clamp)
- 数字前缀 ("12abc") → 12 (std::stoi 接受, 文档化但 caller 应传 clean 值)

**Bit-for-bit guarantee**: `put(K, V)` 后 `get(K)` 返回 V 的 byte-identical
copy (`type`, `factor1`, `factor2`, `factor3`, `power` 字段). 单元测试
`test_cofactor_result_cache` 19 个测试强制覆盖.

**ROI 与定位**:
- 主要 ROI: cofactor pipeline 重复查询时跳过整个 trial/SQUFOF/Brent/Pollard/
  ECM 链. 50d+/60d 大批量 sieve loop 中 (重复访问同 cofactor 是 norm)
  显著 amortise.
- helper 当前 standalone (主路径 `classify_cofactor` 未 wire-in), 是
  future-infrastructure. wire-in 时调用方:
  ```cpp
  if (cofactor_result_cache_enabled()) {
      if (auto cached = shared_cofactor_result_cache().get(c, B, lpb)) {
          return *cached;
      }
  }
  auto result = classify_cofactor(c_int, lpb, /*allow_3lp=*/false, B);
  if (cofactor_result_cache_enabled()) {
      shared_cofactor_result_cache().put(c, B, lpb, result);
  }
  return result;
  ```
- 与 W5 T5 survival predictor / W12 T5 timing telemetry / W13 T3 ECM B1
  cache 完全 orthogonal —— 各自缓存 / 测量不同 cofactor 子阶段. 可同时启用.

**Thread safety**:
- `CofactorResultCache::get` / `put` / `size` / `clear` 用内部 `std::mutex`,
  并发 lookup 序列化 in mutex (LRU 链表 / hash map 同步).
- `get` 返回 `std::optional<CofactorClassification>` 值副本, lock 释放后仍
  安全使用 (与同 key 后续 put / evict 无 aliasing 风险).
- `shared_cofactor_result_cache()` function-local static 保证 C++11
  thread-safe 一次性初始化, ODR-safe 跨 TU.
- 4 thread × 100 mixed get/put 强制测试通过.

**集成点** (W14 T3, 2026-05-23):
- `include/gnfs/cofactor/result_cache.hpp` — 350+ 行 header-only, textbook
  LRU + 三字段 key + splitmix64 hash + ENV gate + process singleton
- `tests/test_cofactor_result_cache.cpp` — 19 个测试 (9 ENV 解析 + capacity=0
  disabled / put-get 5 种 CofactorClass / LRU eviction / LRU promotion /
  clear+reuse / 同 cofactor 不同 (B, lp) 独立 / 重复 put 更新+提升 /
  accessor / shared singleton / 4 thread x 100 mixed / 16 key hash sweep)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  cofactor 模块

**Default OFF (N=0)**: ENV unset → `cofactor_result_cache_size() == 0` →
`cofactor_result_cache_enabled() == false`. 调用方主路径完全不变, 主
pipeline `classify_cofactor` 入口未改, helper-only future-infra. 仅
caller 显式 wire-in + 用户 explicit `GNFS_COFACTOR_RESULT_CACHE_SIZE=N>=1`
时启用.

---

## ECM sigma seed warm pool (GNFS_ECM_SIGMA_POOL_SIZE)

**ENV `GNFS_ECM_SIGMA_POOL_SIZE=N`** (2026-05-22 实施, W10 T3, range [0, 1024], default 0):
ECM Suyama curve setup 入口选 sigma (>=6) 时, 生产 caller 走 PRNG (典型
`std::mt19937_64` seeded from `std::random_device ^ n_low`). 50d+/60d
cofactor 紧凑 retry loop 中, PRNG state advance (mt19937_64 624-word ring)
变成 inner loop 的 serial dependency, 限制跨 sigma attempt 的 ILP. helper
提供 opt-in per-thread sigma seed warm pool, 让 caller 把 N 个 PRNG draw
bulk refill 后 LIFO `pop_back` 一次性 amortise.

```bash
unset GNFS_ECM_SIGMA_POOL_SIZE              # default 0 (disabled, 零开销)
GNFS_ECM_SIGMA_POOL_SIZE=0    ./gnfs <N>    # 同 default
GNFS_ECM_SIGMA_POOL_SIZE=100  ./gnfs <N>    # per-thread 容量 100
GNFS_ECM_SIGMA_POOL_SIZE=1025 ./gnfs <N>    # clamp 到 1024 上限
```

**Helper API** (`include/gnfs/cofactor/sigma_seed_pool.hpp`):
- `sigma_seed_pool_size()` — cached ENV pool 容量, 0 表 disabled
- `sigma_seed_pool_enabled()` — `pool_size() > 0` 等价 predicate
- `refill_sigma_seed_pool(generator)` — 启用时 thread_local pool 用
  `generator()` 填到 capacity (空, 已满, 或禁用时 no-op)
- `get_next_sigma_seed(fresh)` — 启用且 pool 非空: `pop_back` LIFO; 禁用
  或空: 返回 `fresh` 参数. 调用方负责生成 `fresh` 作为 fallback (无 PRNG
  绑定耦合)
- `sigma_seed_pool_remaining()` / `sigma_seed_pool_clear()` — 测试 / debug
- `sigma_seed_pool_reset_env_cache_for_testing()` — 测试 re-resolve hook

**ENV parsing** (`std::stoi`-based, cached `std::call_once`):
- unset / "" / "0" / 负数 / leading 非数字 (`garbage`) → 0 (disabled)
- 1..1024 → as-is
- 1025+ → 1024 (clamp)
- 数字前缀 ("12abc"): 取首数字段 → 12 (std::stoi 接受). 文档化, 但 caller
  应传 clean 整数值, 不依赖 partial-parse 行为

**实现细节**:
- `inline thread_local std::vector<uint64_t> tls_sigma_pool` — per-thread
  存储, C++17 `inline` 保证多 TU 单实例. 线程退出时 vector dtor 跑, uint64_t
  无资源 ownership 故 teardown trivial, 不泄漏.
- `refill`: pool 已满 → no-op (不二次调用 generator). 调用 generator
  `(capacity - current_size)` 次 `push_back`. generator 抛异常 → propagate,
  pool 保持 partial-fill consistent 状态.
- `get_next`: 启用 + pool 非空 → `pop_back` LIFO. 禁用 / pool 空 → 返回 fresh.
- Pool 是 per-thread, **无锁**. 不同 thread 的 pool 完全隔离.

**Bit-for-bit guarantee (within deterministic generator)**:
- helper 不保证与 OFF 路径 sigma 序列完全相同. PRNG generator 在 refill
  时被 bulk-invoke, 与 OFF 路径 per-attempt invoke 调度不同.
- 给定 deterministic generator (e.g. fixed-seed mt19937_64), refill 后
  连续 `get_next` 返回的序列 bit-for-bit 等于
  reversed([gen(), gen(), ..., gen()]) (LIFO 顺序). 由
  `tests/test_sigma_seed_pool.cpp::test_mt19937_generator_consistent`
  强制覆盖.
- Caller 若需严格 deterministic sigma 序列, 应禁用 pool 或 refill from
  deterministic generator 并把 pool 当作 source of truth.

**ROI 与定位**:
- 主要 ROI: 紧凑 ECM retry loop 中 PRNG state advance amortise. mt19937_64
  per-call cost 几十 cycle, 在 small absolute 但 inner loop branch
  prediction defeat + serial dependency.
- helper 当前 standalone (主 pipeline `ECM::factor` / `EcmCurvePool::
  prepare_batch` 未 wire-in), 是 future-infrastructure. caller wire-in
  时把 inner loop `rng()` 替换为 `get_next_sigma_seed(rng())`, 并在外层
  attempt round 入口 `refill_sigma_seed_pool([&rng]() { return rng(); })`.
- Helper 与 W8 T1/W9 T1 `GNFS_ECM_STAGE{1,2}_PARALLEL` 完全 orthogonal —
  并行 dispatcher 跑多条 curve, helper 是 per-thread sigma 池, 二者可
  同时启用.

**集成点** (W10 T3, 2026-05-22):
- `include/gnfs/cofactor/sigma_seed_pool.hpp` — 260 行 header-only,
  thread_local pool + ENV gate + LIFO `pop_back` 语义
- `tests/test_sigma_seed_pool.cpp` — 15 个测试 (5 ENV + 6 行为 (OFF/empty/
  LIFO/exhaust/clear/full no-op) + 1 multi-thread isolation + 1 generator
  exception + 1 env cache reset + 1 mt19937 round-trip)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  cofactor 模块

**Default OFF (N=0)**: ENV unset → `sigma_seed_pool_size() == 0` →
`get_next_sigma_seed(fresh)` 总是返回 fresh, `refill_sigma_seed_pool`
no-op 不调 generator. caller 主路径行为完全等同 legacy, 零开销, 零行为
变化. 仅 helper 被 wire-in + 用户 explicit `GNFS_ECM_SIGMA_POOL_SIZE=N>=1`
时启用.

---

## ECM Stage 2 多曲线并行 (GNFS_ECM_STAGE2_PARALLEL)

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

---

## ECM Stage 1 多曲线并行 (GNFS_ECM_STAGE1_PARALLEL_THREADS)

**ENV `GNFS_ECM_STAGE1_PARALLEL_THREADS=N`** (2026-05-22 实施, W9 T1, default 1, range [1, hardware_concurrency * 2]):
ECM Stage 1 (Lucas-chain Montgomery ladder, 即 `try_curve_with_pk` 内的
scalar-multiplication `k * Q`) 在多条曲线之间相互独立 (embarrassingly
parallel). N=1 (默认) 走 sequential per-curve 循环, 不创建 ThreadPool,
零开销保留原行为. N>=2 时把 K 条曲线 dispatch 到大小为 min(N, K) 的
ThreadPool, 曲线之间靠 future 同步收口.

```bash
GNFS_ECM_STAGE1_PARALLEL_THREADS=1 ./gnfs <N>    # default sequential, zero overhead
GNFS_ECM_STAGE1_PARALLEL_THREADS=4 ./gnfs <N>    # 4 outer workers for Stage 1 Montgomery ladder
GNFS_ECM_STAGE1_PARALLEL_THREADS=8 ./gnfs <N>    # 8 outer workers
unset GNFS_ECM_STAGE1_PARALLEL_THREADS           # same as N=1
```

**并行模型**:
- Outer = `parallel_stage1_curves<Result, Curve, Func>(curves, run_stage1)`
  over K 条独立曲线 (每条由 caller 提供 (sigma, n, B1) setup tuple)
- 内部 Stage 1 Lucas-chain / Montgomery ladder 算法 bit-identical (helper
  仅改变外层 dispatch, 不触碰 `ECM::stage1` / `try_curve_with_pk` 内核)
- 每条曲线 task 拥有独立 Integer buffer 与 Point 状态, GMP `mpz_*` 调用
  操作数互不重叠, 满足 GMP per-call disjoint-operands thread-safety

**Bit-for-bit guarantee**: 每条曲线 (sigma, n, B1) 的 Stage 1 结果是该
sigma 的 pure function, 不依赖 dispatch 顺序. Sequential (N=1) 与
parallel (N>=2) 路径产生的 per-index `Result` 完全一致 (caller 选 Result
类型: 常见 `std::optional<Integer>` 表 "factor found / not found", 或
post-Stage-1 Point + a24 state 供下游 Stage 2 dispatch 复用). 由
`tests/test_ecm_stage1_parallel.cpp` 强制覆盖 (mock worker N=1 vs N=4 /
N=hw bit-identical per-index assert + 真实 ECM Stage 1 via
`factor_with_batch` N=1 vs N=4 per-curve `std::optional<Integer>` 严格一致).

**ROI 与定位**:
- 主要 ROI: 50d+/60d 余因子分解每条曲线的 Stage 1 Lucas chain (B1=10^6 ~
  10^9 时 chain 长 ~10^5 ~ 10^8 ladder step) wall-time 可观, K 条曲线
  并发后 outer wall ~ T_single + tasking overhead, 替代 K * T_single
  sequential 累计.
- 与 W8 T1 `GNFS_ECM_STAGE2_PARALLEL` 正交: Stage 1 + Stage 2 二者各自有
  独立 ENV 控制, caller 可同时启用 (Stage 1 并发跑 K 条曲线, post-Stage-1
  Point 数据收口后再 dispatch 到 Stage 2 helper, 或在同一 task 内串接).
- 与 `EcmCurvePool` 不冲突: pool 是 Stage 1 warm-pool (预生成 sigma 池),
  helper 是 outer dispatch (跑多条曲线). pool 解决 sigma 生成成本, helper
  解决跨曲线 Stage 1 并发. 二者可同时启用.
- Helper 是 opt-in 工具, 不修改 `ECM::factor` / `ECM::quick_factor` /
  `ECM::factor_with_batch` public path. 调用方在自身循环里 wire-in 即可.
- Default OFF (N=1) 保证 zero behavior change for legacy callers, 仅当用户
  明确 opt-in 时启用.

**集成点** (2026-05-22, W9 T1):
- `include/gnfs/cofactor/ecm_stage1_parallel.hpp` — `ecm_stage1_parallel_threads()`
  env reader with `std::once_flag` cache + `parallel_stage1_curves<>` template
  dispatcher + `ecm_stage1_parallel_reset_env_cache_for_testing()` test hook
- `tests/test_ecm_stage1_parallel.cpp` — 9 个测试 (3 ENV 解析 / N=1 baseline
  mock worker / N=1 vs N=4 mock parity / empty span / single-curve N=4
  no-stall / N=hw_concurrency mock parity / 真实 ECM Stage 1
  via `factor_with_batch` N=1 vs N=4 per-curve `optional<Integer>` bit-identical)

---

## ECM Montgomery batch inversion (GNFS_ECM_BATCH_INV)

**ENV `GNFS_ECM_BATCH_INV={0,1}`** (2026-05-22 实施, W8 T3, default 0):
ECM point arithmetic 在 Stage 1 / Stage 2 hot loop 频繁对一批 mod-N 整数
逐个求逆 (`mpz_invert`). 当 N 较大 (50d+/60d cofactor 100-300 bits) 时
extended Euclidean inverse 显著贵于 `mpz_mul + mpz_mod`. Montgomery batch
inversion trick 把 k 个逆操作 amortise 成 1 个 inverse + 3k 个 modular mul:

```text
forward: p_0 = v_0;   p_i = p_{i-1} * v_i mod n      (k-1 mults)
central: q   = p_{k-1}^{-1} mod n                    (1 invert)
reverse: inv_i = q * p_{i-1} mod n, q = q * v_i mod n (2*(k-1) mults)
```

```bash
GNFS_ECM_BATCH_INV=1 ./gnfs <N>          # 启用 helper gate
GNFS_ECM_BATCH_INV=0 ./gnfs <N>          # 显式 disable (= default)
unset GNFS_ECM_BATCH_INV                 # 默认 disable
```

**Helper API** (`include/gnfs/cofactor/batch_inversion.hpp`):
- `batch_mod_inverse(values, n)` — Montgomery 路径, 1 inverse + 3k mul,
  返回 `BatchInvResult { inverses, found_factor }`. k=0 立即 return 空;
  k=1 走 single `mpz_invert` 短路 (零 prefix overhead).
- `naive_mod_inverse(values, n)` — k 个 per-element `mpz_invert` 参考实现.
  单元测试 golden, 也供希望显式禁 batched trick 的 caller 使用.
- `ecm_batch_inv_enabled()` — cached `std::once_flag` + `std::atomic<bool>`,
  strict "1" parsing (= W6 `GNFS_FILTER_RADIX_SORT` / W6 `GNFS_V0_BFS`
  pattern). 任何非 "1" 值 (unset / "" / "0" / "garbage" / "2" / "true" /
  "10" / 含空格的 "1") 都返回 `false`.
- `ecm_batch_inv_reset_env_cache_for_testing()` — 测试专用, 重置 cached
  gate 让下次 `enabled()` 再读 env.

**Failure semantics** (与现有 ECM lucky-factor idiom 对齐):
- `mpz_invert(_, p_{k-1}, n) == 0` 时知道 gcd(p_{k-1}, n) > 1, 说明至少一个
  v_i 与 n 有非平凡公因子. helper 顺序扫 input span 找到第一个非平凡
  gcd(v_i, n), 放到 `BatchInvResult::found_factor` (与 ECM 现有 per-curve
  "inverse failure exposes factor" 语义一致).
- 若 v_i 全是 1 或 n 的倍数 (gcd 仅为 1 或 n), `found_factor` 仍是
  `std::nullopt`. caller 必须按 "无法分解" 处理, 不能假设始终能 extract factor.
- `naive_mod_inverse` 用同一 `find_first_nontrivial_gcd` 扫法保证 batch 与
  naive 对同一 input 报告同一 culprit (per-index identical failure mode).

**Bit-for-bit guarantee**: 当 gcd(v_i, n) == 1 for all i, Montgomery trick 与
逐 `mpz_invert` mathematically equivalent (不是 approximation). 单元测试
`tests/test_batch_inversion.cpp` 强制覆盖 k = 0, 1, 5 (n=101), 20 (n ~ 2^64
prime), 100 (n ~ 200-bit prime) 各 size 严格 per-index bit-for-bit assert,
另外测 unreduced v_i (>= n) 与 boundary v_i (= 1, = n-1).

**ROI 与定位**:
- 主要 ROI: 50d+/60d cofactor (200-330 bit N) ECM Stage 1+2 hot loop 当前
  per-point 调 `mpz_invert`. 对 200-bit N, mpz_invert ≈ 10-20 倍 mpz_mul
  cost; batch path k=8 时 amortised inverse cost ≈ 4 mul cost (4-5× 提速).
  k 越大 ROI 越显著, 但需要 caller 能 batch up k >= 2 个独立 inversion site.
- 当前主 pipeline 无 wire-in: ECM Stage 1 / Stage 2 / Brent-Suyama 都仍走
  per-point `mpz_invert`. helper 作为 future-infrastructure 落地, 等具体
  inversion hot site (e.g. Stage 2 BSGS giant-step accumulation, Brent-Suyama
  polynomial 系数 batched eval) 显式 wire-in 时启用.
- helper 与 W8 T1 `GNFS_ECM_STAGE2_PARALLEL` 完全 orthogonal — Stage 2 并行
  跑 K 条独立曲线, batch_inversion 是 per-curve inner loop 的 inversion
  amortisation. 二者可同时启用.

**集成点** (2026-05-22, W8 T3):
- `include/gnfs/cofactor/batch_inversion.hpp` — helper API + ENV gate +
  `BatchInvResult` + `find_first_nontrivial_gcd` 内部 helper.
- `tests/test_batch_inversion.cpp` — 12 tests (ENV unset / "1" / 8 non-"1"
  rejects / reset cache / empty k=0 / single k=1 / parity k=5,20,100 /
  found_factor / boundary v_i / unreduced v_i).
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout
  (实测 wall ~125ms).

**Default OFF**: ENV unset → `ecm_batch_inv_enabled() == false` → 任何 caller
看到 gate 关闭则跑 per-element `mpz_invert` path 不变, 零行为变化. 仅当 caller
主动 wire-in 且用户 explicit `GNFS_ECM_BATCH_INV=1` 时启用.

---

## Brent-Pollard rho 多配置并行 (GNFS_BRENT_POLLARD_RHO_THREADS)

**ENV `GNFS_BRENT_POLLARD_RHO_THREADS=N`** (2026-05-23 实施, W15 T3, default 1, range [1, hardware_concurrency * 2]):
W7 / W8 T1 / W9 T1 / W10 T4 / W11 T3 / W11 T4 / W12 T3 / W12 T4 / W13 T5 / W14 T5
之后的 parallel-dispatcher 家族第 11 名成员. Brent's variant of Pollard rho
(`include/gnfs/cofactor/brent_pollard_rho.hpp`) 是 cofactor pipeline 的一站,
通过 `GNFS_COFACTOR_BRENT=1` 启用. 每次 rho run 由 `(c, x0)` 配置参数化
(c 是 `f(x) = x^2 + c mod n` 的常数, x0 是起点), 不同 `(c, x0)` 的 rho run
互相独立 — 共享 modulus `n`, 不共享 mutable state, 产 deterministic per-config
output (`std::optional<...>` 携带 non-trivial factor 或 nullopt). 这让
"try K different rho configurations" 成为 embarrassingly parallel batch.
N=1 (默认) 走 sequential per-config 循环, 不创建 ThreadPool, 零开销保留
原行为. N>=2 时把 K 个 config dispatch 到大小为 min(N, K) 的 ThreadPool,
config 之间靠 future 同步收口.

```bash
GNFS_BRENT_POLLARD_RHO_THREADS=1 ./gnfs <N>    # default sequential, zero overhead
GNFS_BRENT_POLLARD_RHO_THREADS=4 ./gnfs <N>    # 4 workers for parallel rho fan-out
GNFS_BRENT_POLLARD_RHO_THREADS=8 ./gnfs <N>    # 8 workers
unset GNFS_BRENT_POLLARD_RHO_THREADS           # same as N=1

# 与 GNFS_COFACTOR_BRENT 正交 (二者完全独立)
GNFS_COFACTOR_BRENT=1 GNFS_BRENT_POLLARD_RHO_THREADS=4 ./gnfs <N>
```

**与 GNFS_COFACTOR_BRENT 的关系 (正交两个 ENV)**:
- `GNFS_COFACTOR_BRENT=1` 切换主 pipeline cofactor dispatch chain 在
  SQUFOF 之后是否调用 Brent rho (vs 直接 fall through 到 legacy Pollard rho /
  ECM). 单一布尔 gate, 与并发度无关.
- `GNFS_BRENT_POLLARD_RHO_THREADS=N` 控制 batched 调用 `BrentPollardRho::split`
  时的并发度. helper 是 opt-in 工具, 调用方需要自己构造 `(c, x0)` batch +
  调 `parallel_brent_pollard_rho`. 默认 N=1 即 sequential, 与 legacy 行为
  完全等价.
- 二者可同时启用 (典型: `GNFS_COFACTOR_BRENT=1 GNFS_BRENT_POLLARD_RHO_THREADS=4`),
  也可单独启用 / 单独关闭. 互不冲突.

**Helper API** (`include/gnfs/cofactor/brent_pollard_rho_parallel.hpp`):
- `brent_pollard_rho_threads()` — cached `std::once_flag` + `std::atomic<int>`
  ENV reader, default 1, clamp `[1, hw*2]`
- `resolve_brent_pollard_rho_threads(batch_size)` — 返回
  `min(threads, batch_size)`, empty batch (size==0) 返回 0
- `parallel_brent_pollard_rho<Result>(cs, x0s, worker_fn)` — 主入口, 返回
  `std::vector<Result>` 按 input index 对齐
- `brent_pollard_rho_threads_reset_env_cache_for_testing()` — 测试 re-resolve hook

**并行模型**:
- Outer = `parallel_brent_pollard_rho<Result, WorkerFn>(cs, x0s, worker_fn)`
  over n configs
- Inner = `gnfs::util::ThreadPool` 大小为 min(N, n), 每 task 调
  `worker_fn(cs[i], x0s[i])` 写到 disjoint `results[i]` slot
- 内部 Brent rho 算法 bit-identical (helper 仅改变外层 dispatch, 不触碰
  `BrentPollardRho::split` 内核或 `cofactorizer.hpp` 主 dispatch chain)
- 共享 read-only state (modulus `n`, per-cofactor metadata) 由 worker
  通过 lambda capture 引用, 每个 task 拥有独立 Integer / GMP `mpz_*` buffer,
  满足 GMP per-call disjoint-operands thread-safety
- 空 batch (n==0) / 单 config (n==1) 都走 sequential 短路, 不创建 pool
- Exception path: dispatcher drain 全部 future, 第一个 thrown exception
  通过 `std::rethrow_exception` 传给 caller (不 swallow); pool 析构干净 join

**Bit-for-bit guarantee**: 每 config rho run 是 `(c, x0)` + immutable
shared state 的 pure function (caller responsibility). 不依赖 dispatch 顺序.
Sequential (N=1) 与 parallel (N>=2) 路径产生的 per-index `Result` 完全一致.
由 `tests/test_brent_pollard_rho_parallel.cpp` 强制覆盖 (100 / 1000 mock
worker config N=1 vs N=4 / N=hw 严格 per-index assert, plus 50 个 deterministic
semi-prime cofactor 通过真实 `BrentPollardRho::split` N=1 vs N=4 per-config
`std::optional<Integer>` 完全一致).

**输入语义 — Two-span shape (与 W14 T5 一致)**:
- helper 消费两个 parallel input span `cs` + `x0s`, 长度必须相等
- precondition: `cs.size() == x0s.size()`, 不等抛 `std::invalid_argument`
- 与 W7 / W8 T1 / W9 T1 / W10 T4 / W11 T4 / W12 T4 的 single-span "curves"
  形状不同; 与 W11 T3 / W12 T3 / W13 T5 / W14 T5 的 GMP-primitive 输入
  shape 同属 multi-parameter family

**Failure semantics — 无 failure mode**:
- `worker_fn` 的返回类型由 caller 选 (典型 `std::optional<std::pair<Integer, Integer>>`
  匹配 `BrentPollardRho::split` 的 shape, 或 `std::optional<Integer>` 表
  "found / not found")
- helper 本身不引入 failure mode, 不返回 `std::vector<bool> success`
  (与 W12 T3 mpz_invert 不同 — invert 在 `gcd(base, modulus) != 1` 时 fail,
  此处 worker 自己的 optional return 已覆盖 not-found 情况, helper
  无需额外暴露 success vector)

**ROI 与定位**:
- 主要 ROI: 50d+/60d cofactor 阶段 Brent rho `BrentPollardRho::split` per-call
  wall-time 显著 (典型 max_iter = 2^18 ~ 2^20). K config 并发后 outer wall
  ~ T_max_config + tasking overhead, 替代 sum(K) sequential 累计. perf-info
  实测 50-config M5 ARM64: N=1 10ms vs N=4 2ms → 5x speedup (10-core P/E
  混合, 4 P-core 满负载, mock worker per-call ~0.2ms 体现了 tasking
  overhead amortise 良好).
- 与 W7/W8/W9/W10 T4/W11 T3/W11 T4/W12 T3/W12 T4/W13 T5/W14 T5 parallel
  dispatcher family 互补, 本 helper 是第 11 名成员:
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
    * W15 T3 `GNFS_BRENT_POLLARD_RHO_THREADS` — batched Brent rho 配置 (本 helper)
  十一者全部 default 1 (sequential), opt-in, 互不冲突. 可同时启用.
- Helper 是 opt-in 工具, **不修改** `BrentPollardRho::split` 内核 /
  `cofactorizer.hpp` 主 dispatch chain. 调用方需要自己 batch up `(c_i, x0_i)`
  对 + 传入 worker_fn lambda. 当前主 pipeline 无 wire-in 调用, 是 future-infra.
- Default OFF (N=1) 保证 zero behavior change for legacy callers, 仅当用户
  明确 opt-in 时启用.

**集成点** (2026-05-23, W15 T3):
- `include/gnfs/cofactor/brent_pollard_rho_parallel.hpp` —
  `brent_pollard_rho_threads()` env reader with `std::once_flag` cache +
  `parallel_brent_pollard_rho<Result, WorkerFn>(cs, x0s, worker_fn)`
  template dispatcher + `resolve_brent_pollard_rho_threads(batch_size)`
  helper + `brent_pollard_rho_threads_reset_env_cache_for_testing()` test hook
- `tests/test_brent_pollard_rho_parallel.cpp` — 17 个测试 (6 ENV 解析 含
  "12abc" partial parse + leading whitespace + "10000" clamp / empty span /
  single config N=1 / single config N=4 no-stall / N=1 baseline mock /
  N=1 vs N=4 mock parity 100 configs / N=1 vs N=hw mock parity 1000 configs /
  real Brent-Pollard rho 50 cofactor parity N=1 vs N=4 per-config
  optional<Integer> identical / mismatched span throws invalid_argument /
  reset env cache hook / resolve helper edges / perf info probe)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  cofactor 模块
