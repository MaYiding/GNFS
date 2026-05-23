# 多项式 (polynomial) 模块 ENV 调优开关

> 本文档收录 `polynomial` 模块所有 `GNFS_*` 运行时调优开关 / helper 的详细设计(算法、ENV 解析、bit-for-bit 保证、ROI、集成点、测试)。
>
> 返回: [ENV 开关总览](README.md) · [CLAUDE.md ENV 索引表](../../CLAUDE.md#env-调优开关总览)

---

## Murphy E alpha 并行 (GNFS_MURPHY_ALPHA_THREADS)

**ENV `GNFS_MURPHY_ALPHA_THREADS=N`** (2026-05-18 实施, lightweight optimization):
MurphyEvaluator::compute_alpha 用 ThreadPool 并行扫 ~78k primes. 每 thread
accumulates partial double, 序列 reduce.

```bash
GNFS_MURPHY_ALPHA_THREADS=0 ./gnfs <N>   # 序列 (debug / 单线程对照)
GNFS_MURPHY_ALPHA_THREADS=8 ./gnfs <N>   # 显式 8-thread
# 默认: hardware_concurrency
```

**ROI**: M5 10-core → 5-7x compute_alpha speedup (CZ求根 perfect embarrassingly
parallel by prime). Kleinjung selector + 多 polynomial 评估时 sieve 主流程
wall-time 显著缩短.

**集成点** (commit `0dd1799`, 2026-05-18):
- `include/gnfs/polynomial/murphy_evaluator.hpp` — `compute_alpha(f, prime_bound)` parallel sweep + `alpha_contribution(f, df, p)` thread-safe helper
- Lazy `std::once_flag` + `unique_ptr<ThreadPool>` per evaluator instance
- 回归测试 `tests/test_murphy.cpp` parallel == sequential invariant (commit `2ef928a`)

**Rotation-incremental 算法重构**: multi-day pure-math 工作仍 deferred.
当前 parallelization 是 orthogonal lightweight 加速, 不替代真正 incremental.

---

## Polynomial Half-GCD (GNFS_POLY_HGCD)

**ENV `GNFS_POLY_HGCD=1`** (2026-05-21 实施, default OFF):
启用 Knuth-Schönhage Half-GCD (HGCD) 算法替代 Euclidean GCD 在 polynomial
GCD over F_p[x]. 默认 OFF, Euclidean path 完整保留.

```bash
GNFS_POLY_HGCD=1 ./gnfs <N>   # 启用 HGCD path
unset GNFS_POLY_HGCD          # default OFF (Euclidean)
```

**算法**: Recursive divide-and-conquer on polynomial pair (a, b) — 把
deg(a)=n 切半, 递归求 transformation matrix M 使 M * (a, b) = (a', b')
满足 deg(b') < n/2. 主 GCD 通过反复调用 HGCD + Euclidean tail 完成.

**Threshold `kHGCDThreshold = 16`**: deg(a) 小于此值直接走 Euclidean (递归 +
matrix-vector mult overhead 在小度数 dominate).

**Bit-for-bit guarantee**: `gcd_via_hgcd(a, b, p)` 输出与 `ModularPoly::gcd(a, b, p)`
monic-normalized 结果完全一致. 单元测试 `test_half_gcd` 16 个测试强制验证
(包括 deg [10, 200] 随机 polynomial / large prime ~2^64 / edge cases).

**ROI 定位**:
- HGCD 真正加速依赖 sub-quadratic polynomial multiplication M(n)
  (e.g., FFT 给 O(n log n)). 当前 `ModularPoly::mul_raw` 走 schoolbook
  O(n²), 所以 HGCD wall-time 在 deg ≤ 500 略慢于 Euclidean
  (实测 deg=100 0.37x, deg=500 0.46x).
- GNFS 主路径 polynomial GCD 调用都在小 degree (CZ 求根 ≤ 6),
  ROI 不适用. HGCD 主要为未来 FFT 乘法集成预留接口.
- 不影响正确性, 实验 path 完整测试.

**集成点** (2026-05-21):
- `include/gnfs/polynomial/half_gcd.hpp` — `gcd_via_hgcd()` + `poly_hgcd_enabled()`
  + `kHGCDThreshold` + `HGCDMatrix` 2x2 transformation matrix
- `tests/test_half_gcd.cpp` — 16 个测试 (8 correctness across deg / 4 edge cases
  / 2 ENV / 2 perf info)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout

**Default OFF**: pipeline.cpp 与 `ModularPoly::gcd` 入口不动, opt-in 实验通道.

---

## Polynomial Karatsuba multiplication threshold (GNFS_POLY_KARATSUBA_THRESHOLD)

**ENV `GNFS_POLY_KARATSUBA_THRESHOLD=N`** (2026-05-22 实施, range [4, 4096], default 32):
Polynomial multiplication helper `karatsuba_mul_mod` 在 F_p[x] 上实现 3-split
Karatsuba O(n^1.585), 基础情形 `max(deg a, deg b) < N` 回退 schoolbook. 与
`schoolbook_mul_mod` 参考实现 bit-for-bit 一致. 默认 threshold N=32 是
schoolbook-vs-Karatsuba 经验 sweet spot.

```bash
unset GNFS_POLY_KARATSUBA_THRESHOLD            # 默认 32
GNFS_POLY_KARATSUBA_THRESHOLD=4    ./gnfs <N>  # 极小 threshold (recursion 走到最深)
GNFS_POLY_KARATSUBA_THRESHOLD=64   ./gnfs <N>  # 较大 threshold (schoolbook 占主导)
GNFS_POLY_KARATSUBA_THRESHOLD=4096 ./gnfs <N>  # 上限 (实测几乎 schoolbook every call)
```

**ENV 解析规则** (严格):
- unset / "" / "0" / 负数 / 非数字 (`garbage` / `1.5` / `12abc` / bare `+` `-`)
  / 含 leading 空白 (` 32`) → default 32
- "10000" → clamp 到上限 4096
- "2" → clamp 到下限 4 (低于 4 时 recursion 无意义, 退化为 split 2+1)

**算法** (3-split Karatsuba):
- 拆分: a = a_low + x^m · a_high, b = b_low + x^m · b_high, m = ceil(nmax / 2)
- z0 = a_low * b_low                                   (递归)
- z2 = a_high * b_high                                 (递归, 一侧空则跳过)
- z1 = (a_low + a_high) * (b_low + b_high) - z0 - z2   (递归 + 减法)
- 结果: out = z0 + x^m · z1 + x^{2m} · z2
- 中间和 (a_low + a_high) mod p 防溢出; subtraction 用 `(a + p - b) mod p`
  避免下溢

**Threshold default 32 选择理由**:
- Karatsuba 每层有显著 per-call overhead (3 个 sum vector + 3 个 sub-product
  vector + 3 个递归 stack frame)
- Schoolbook 内循环紧凑, tiny n 下 mul 数虽然 O(n²) 但常数极小
- 经验 sweet spot 在 16-64 之间, 选 32 作 conservative middle
- 低于 4 时 recursion 退化 (3 系数 polynomial 切 2+1, "高" side 只剩
  degree 1, 无法 amortise)

**Bit-for-bit guarantee**: 同 `(a, b, p)` 输入下 (p 素数, p < 2^32,
coefficients < p), `karatsuba_mul_mod` 与 `schoolbook_mul_mod` 输出
`out` vector 完全一致 (size + 每位 content). Threshold 值仅影响递归深度,
不影响数学结果. Empty 输入双方都给 empty 输出. 单元测试
`tests/test_poly_karatsuba.cpp` 通过 13 random shapes 与 threshold
extremes (4 vs 999999) 严格强制覆盖.

**修复历史** (commit `25169c4`):
初版在 a/b 跨 split 边界时 z1 = sum_a * sum_b - z0 - z2 留有 trailing zero
(Karatsuba 算法故意取消 leading coefficient), compose 时 grow `out` 超出
`na + nb - 1` 上限. 修复: z0 / z1 / z2 / sum_a / sum_b 每次计算后
`trim_trailing_zeros`, 且 `add_shifted_in_place` 不再 grow out (out-of-range
src 必须为 0, 否则 assert).

**Modulus precondition**: p < 2^32 (保证 uint64 * uint64 不溢出).
caller 需要 p >= 2^32 时仍走 `ModularPoly::mul_raw` (内部 `__uint128_t`).

**ROI 与定位**:
- 主要 ROI: Karatsuba 是 sub-quadratic primitive M(n), 是 W7 HGCD
  (`GNFS_POLY_HGCD`) 等待的 sub-quadratic 乘法. HGCD 真正 wall-time
  加速依赖 M(n) 复杂度低于 schoolbook O(n²). 当前 `ModularPoly::mul_raw`
  仍走 schoolbook, 所以 HGCD 在 deg ≤ 500 略慢 (W7 实测 deg=100 0.37x,
  deg=500 0.46x).
- 当前主路径 `ModularPoly::mul_raw` **未** wire-in Karatsuba — 是
  future-infrastructure helper. 当未来 caller (例如 `ModularPoly::mul_raw`
  内部, 或 HGCD recursion 内部) 决定切到 sub-quadratic primitive 时直接
  调用 `karatsuba_mul_mod` 即可.
- perf-info probe (size=500, p=2^31-1): schoolbook 3.73 ms/call vs
  karatsuba 1.72 ms/call → 2.17x 加速. 真正 ROI 在 deg >> 100 时显著.

**集成点** (2026-05-22, W9 T2):
- `include/gnfs/polynomial/karatsuba_mul.hpp` — `karatsuba_mul_mod()` +
  `schoolbook_mul_mod()` + `poly_karatsuba_threshold()` (cached env, strict
  parsing) + `poly_karatsuba_threshold_reset_env_cache_for_testing()` test hook
- `tests/test_poly_karatsuba.cpp` — 10 个测试 (5 env parsing / 2 edge cases /
  2 correctness parity / 1 perf info)
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  polynomial 模块

**Default 32 主路径无影响**: `ModularPoly::mul_raw` 入口未改, helper
仅在显式 caller wire-in 时启用. 现有 schoolbook path 与 W7 HGCD path
均保持原行为. ENV 仅对显式调用 `karatsuba_mul_mod` 的 caller 生效.

---

## Polynomial NTT multiplication (GNFS_POLY_NTT)

**ENV `GNFS_POLY_NTT=auto|0|1`** (2026-05-22 实施, W12 T2, default auto):
Polynomial multiplication helper `ntt_mul_mod` 在 F_p[x] 上 (p prime,
p < 2^32) 实现 3-prime CRT NTT 路径, asymptotic O(n log n). 与
`schoolbook_mul_mod` 参考实现 bit-for-bit 一致. 默认 auto 在
`max(deg_a, deg_b) >= kNttAutoThreshold = 256` 时启用 NTT, 低于阈值
回退 schoolbook.

```bash
unset GNFS_POLY_NTT             # 默认 Auto (>=256 走 NTT, 否则 schoolbook)
GNFS_POLY_NTT=auto ./gnfs <N>   # 同 unset
GNFS_POLY_NTT=0    ./gnfs <N>   # 显式 ForceOff (强制 schoolbook)
GNFS_POLY_NTT=off  ./gnfs <N>   # 同 "0"
GNFS_POLY_NTT=1    ./gnfs <N>   # ForceOn (NTT 适用任意 size >= 2)
GNFS_POLY_NTT=on   ./gnfs <N>   # 同 "1"
```

**ENV 解析规则** (三态严格):
- unset / "" / "auto" → Auto (default)
- "0" / "off" → ForceOff (强制 schoolbook)
- "1" / "on" → ForceOn (NTT 适用任意 size >= 2, size <= 1 仍 short-circuit)
- 任何其他值 (`garbage`, `2`, `true`, `-1`, `yes`, `ON/OFF/Auto` 大写,
  含 leading 空白 `  1`) → Auto

**算法** (3-prime CRT NTT):
- 选 3 个 "Schönhage NTT-friendly" 素数 q_i = c_i · 2^{k_i} + 1, 每个
  < 2^30 (这点关键, 让 inner butterfly uint64 * uint64 不溢):
    * q1 = 998244353  = 119 · 2^23 + 1, primitive root 3
    * q2 = 985661441  = 235 · 2^22 + 1, primitive root 3
    * q3 = 754974721  = 45  · 2^24 + 1, primitive root 11
- 每个 q 上做 forward NTT (in-place iterative Cooley-Tukey + bit-reverse
  permute) + 点积 + inverse NTT (omega^{-1} + 最后 n^{-1} mod q scale)
- 输入 zero-pad 到 next_pow2(deg_a + deg_b + 1)
- Garner 风格 CRT 重构每个 output 系数:
    * u1 = r1
    * u2 = ((r2 - r1) · inv(q1) mod q2) mod q2
    * u3 = ((r3 - r1 - q1·u2) · inv(q1·q2) mod q3) mod q3
    * x   = u1 + q1·u2 + q1·q2·u3
- mod p 化简时不展开整个 x (90-bit 不入 uint64): 用预计算 `q1 mod p` 与
  `q1·q2 mod p` (都 < p < 2^32), 配合 u2/u3 (< 2^30) 做 uint64 算术

**为什么 3 个 prime (不是 1 个 64-bit prime)**:
- 卷积 output 系数上界 = (p-1)^2 · n, 对 p < 2^32 与 n < 2^24 是 ~2^88,
  超过任何单一 64-bit prime 容量
- 单一 prime 必然走 `__uint128_t` inner-loop (×× 慢), 3 prime 让每次
  butterfly mul 都 uint64 * uint64 → uint64 (积 < 2^60)
- 3 prime 总 CRT 容量 = q1·q2·q3 ≈ 2^90, 远 > (p-1)^2 · n 上界

**Threshold default 256 选择理由**:
- NTT 有显著 per-call 常数: 3 个 forward + 3 个 inverse transform +
  3 个 pointwise mul + Garner 重构 per coefficient + zero-pad 到下一个
  2 的幂
- 小输入下 schoolbook 内循环紧凑 (4 instruction multiply-add 链), tiny n
  下 schoolbook 完胜
- 经验 crossover 在 128 - 512 区间, 选 256 作 conservative midpoint
- ForceOn 让 caller 在 size >= 2 时也走 NTT path (用于 test parity 覆盖)

**Modulus precondition**: p prime, p < 2^32 (保证 CRT reduction 内
uint64 * uint64 fits uint64 — 即 q1_mod_p · u2 < 2^62 in worst case).
Caller 需保证 `a` / `b` 系数都已 reduced mod p; helper 不校验 p 素性,
是 caller 责任.

**Bit-for-bit guarantee**: 同 `(a, b, p)` 输入下 (p prime, p < 2^32,
coefficients < p), `ntt_mul_mod` 与 `schoolbook_mul_mod` 输出 `out`
vector 完全一致 (size + 每位 content, 都是 trim 过 trailing zeros 的
canonical form). Gate 值仅影响 dispatch kernel, 不影响数学结果. 单元
测试 `tests/test_poly_ntt.cpp` 通过 12 个 case 严格覆盖 (4 ENV / 2
edge / 4 parity 多 prime 多 size / 1 ForceOff vs ForceOn / 1 threshold
routing / 1 perf info).

**ROI 与定位**:
- 主要 ROI: NTT 在 deg >> threshold 时是 O(n log n) vs schoolbook 的
  O(n^2). 实测 deg=2000, p=2^31-1: schoolbook 39.31 ms/call vs ntt
  3.89 ms/call → 10.1x 加速. 真正 ROI 在 deg >= 500 后体现, deg ~ 5000
  上数十倍加速
- helper 当前 standalone (主路径 `ModularPoly::mul_raw` 未 wire-in),
  是 future-infrastructure. caller 可主动调 `ntt_mul_mod` 替代
  `mul_raw`, 适用于 Half-GCD (W7) 等下游需要大度数 polynomial mul 的
  实验路径
- 与 W9 `GNFS_POLY_KARATSUBA_THRESHOLD` 互补: Karatsuba 是 O(n^1.585)
  中间级 primitive, NTT 是 O(n log n) 顶级 primitive. 三者覆盖小/中/大
  size 不同范围

**集成点** (2026-05-22, W12 T2):
- `include/gnfs/polynomial/ntt_mul.hpp` — `ntt_mul_mod()` +
  `schoolbook_mul_mod()` + `poly_ntt_mode()` 三态 + `poly_ntt_enabled_for_size()`
  dispatcher + `poly_ntt_reset_env_cache_for_testing()` + `kNttAutoThreshold = 256` +
  3 个 NTT prime constexpr + Garner CRT helper + iterative Cooley-Tukey NTT
- `tests/test_poly_ntt.cpp` — 12 instant tier tests, TIMEOUT 60
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  polynomial 模块

**Default Auto 主路径无影响**: `ModularPoly::mul_raw` 入口未改, helper
仅在显式 caller wire-in 时启用. 现有 schoolbook path / W7 HGCD path /
W9 Karatsuba helper / W11 divrem helper 路径均保持原行为. Auto 在
size < threshold 时等价 schoolbook; size >= threshold 时走 NTT path
(但同一 caller 必须直接调 `ntt_mul_mod`, 不是 `mul_raw`).

---

## Polynomial modular squaring (GNFS_POLY_SQUARE_OPT)

**ENV `GNFS_POLY_SQUARE_OPT=auto|0|1`** + **`GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD=N`** (2026-05-22 实施, W13 T2, default auto + 32):
Polynomial modular squaring helper. 利用 `(sum a_i x^i)^2` 的 (i, j) ↔
(j, i) 对称性, schoolbook 路径相对 W9 `karatsuba_mul_mod(a, a, p, out)`
full-mul 大致省一半工 (对角项 a[k]^2 + off-diagonal 2 * a[i] * a[j],
i < j). Karatsuba squaring 沿用 W9 split-recurse 结构, 但三个 sub-mul
换成三个 sub-square (`a_low^2`, `a_high^2`, `(a_low + a_high)^2`) 加
一个 cross 减法 (`z1 = (a_low + a_high)^2 - z0 - z2`). 与 W9
`karatsuba_mul_mod(a, a, p, out)` 输出 bit-for-bit identical (trim 过
trailing zeros canonical form).

```bash
unset GNFS_POLY_SQUARE_OPT                      # 默认 Auto (squaring 启用)
GNFS_POLY_SQUARE_OPT=auto ./gnfs <N>            # 同 unset
GNFS_POLY_SQUARE_OPT=0    ./gnfs <N>            # ForceOff (退到 W9 full-mul)
GNFS_POLY_SQUARE_OPT=off  ./gnfs <N>            # 同 "0"
GNFS_POLY_SQUARE_OPT=1    ./gnfs <N>            # ForceOn (squaring 启用, 与 Auto 等价)
GNFS_POLY_SQUARE_OPT=on   ./gnfs <N>            # 同 "1"

unset GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD               # 默认 32
GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD=4    ./gnfs <N>     # 极小 threshold (recursion 走到最深)
GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD=64   ./gnfs <N>     # 较大 threshold
GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD=4096 ./gnfs <N>     # 上限
```

**ENV 解析规则** (与 W12 NTT 三态 + W9 Karatsuba threshold 解析一致):
- `GNFS_POLY_SQUARE_OPT`: unset / "" / "auto" / 任何未识别 token (含
  "2" / "true" / "ON" / "Auto" 大写, 含 leading 空白) → Auto. "0" / "off"
  → ForceOff. "1" / "on" → ForceOn.
- `GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD`: unset / "" / "0" / 负数 / 非数字
  (`garbage` / `1.5` / `12abc` / bare `+` `-`) / 含 leading 空白 → default 32.
  "1".."3" → clamp 到 4. "5000" → clamp 到 4096.

**算法**:
- Schoolbook squaring (`schoolbook_square_mod`): 对每个 `(i, j)` 索引对,
  i == j 走 `out[2i] += a[i]^2 mod p` (n 次), i < j 走 `out[i+j] +=
  (2 * a[i] * a[j]) mod p` (n(n-1)/2 次). doubled product 之前先 `mod p`
  再 doubled, 然后 `mod p`. uint64 * uint64 不溢出依赖 p < 2^32 precondition.
- Karatsuba squaring (`karatsuba_square_mod`): split a = a_low + x^m * a_high,
  m = ceil(n/2). 递归三个 sub-square (z0 = a_low^2, z2 = a_high^2,
  z1' = (a_low + a_high)^2). z1 = z1' - z0 - z2. out = z0 + x^m * z1 +
  x^{2m} * z2. 与 W9 mul kernel 一致, z1 trailing-zero trim 后 compose
  保证 `add_shifted_in_place` 不越界.
- `square_mod` 主入口: empty → empty; size-1 → `out = { (a[0] * a[0]) mod p }`
  (a[0] == 0 → empty); gate == ForceOff → 走 W9 `karatsuba_mul_mod(a, a, p, out)`;
  gate == ForceOn / Auto + size < threshold → `schoolbook_square_mod`;
  size >= threshold → `karatsuba_square_mod`.

**Bit-for-bit guarantee**: 同 `(a, p)` 输入下 (p 素数, p < 2^32,
coefficients < p), `square_mod`, `schoolbook_square_mod`, `karatsuba_square_mod`
三个内核输出 vector 与 W9 `karatsuba_mul_mod(a, a, p, out)` (trim 后)
完全 per-index 一致. ENV gate 值仅影响 dispatch kernel, 不影响数学结果.
单元测试 `tests/test_poly_square.cpp` 16 个测试 (含 4 ENV 解析 + 2
threshold 解析 + 3 edge cases + 4 parity sweep deg 10 / 50 / 200 / 500
across 3 primes + 1 random 10-shape sweep 含 threshold 边界 31/32/33 +
1 Mersenne p=2^31-1 边界 + 1 perf-info probe) 严格 enforce.

**Threshold default 32 选择理由**:
- Karatsuba squaring 每层 per-call overhead 与 W9 Karatsuba mul 同级
  (3 个递归 sub-buffer 分配 + 3 个递归 stack frame). 小输入 schoolbook
  内循环紧凑, 即使 N=32 阈值下 schoolbook 的对角对称性已经把工作量
  砍半, ROI 非常稳健.
- 默认 32 与 W9 `GNFS_POLY_KARATSUBA_THRESHOLD` 一致, 用户调一个 threshold
  即可在 mul / square 两个 helper 上同步生效, 避免双调优面.

**Modulus precondition**: p < 2^32 (保证 uint64 * uint64 不溢出 — schoolbook
内层 `a[i] * a[j]` 与 Karatsuba sub-square 的 `mul_mod` 都依赖). caller
需要 p >= 2^32 时仍走 `ModularPoly::mul_raw(a, a)` (内部 `__uint128_t`).

**ROI 与定位**:
- 主要 ROI: 与 W9 `karatsuba_mul_mod(a, a, ...)` 相比, squaring 节省的工作
  来自对角对称性. 实测 deg=200 / p=2^31-1: square_mod (Auto/Karatsuba)
  0.11 ms/call vs karatsuba_mul_mod(a, a) 0.20 ms/call → 1.75x 加速.
  schoolbook_square_mod (ForceOn + threshold=4096) 0.16 ms/call → 1.23x
  vs W9 mul. 真正 ROI 在 deg >> 100 时显著.
- helper 当前 standalone (主路径 `ModularPoly::sqr` 与 `ModularPoly::mul_raw(a, a)`
  未 wire-in), 是 future-infrastructure. caller 可主动调 `square_mod` 替代
  `mul_raw(a, a)`, 适用于 CRT root-finding power chains, Karatsuba mul
  内部 cross sub-square 优化, NTT self-convolution 短路等下游需要平方
  hot path 的实验路径.
- 与 W7 HGCD / W9 Karatsuba / W11 divrem / W12 NTT 互补: W9 mul / W12
  NTT 是 (a, b) 通用 multiplication primitive, 本 helper 是
  (a, a) 专用 squaring primitive. 二者覆盖不同 caller path, 同 caller
  可以根据 self vs cross 自动选择.

**集成点** (2026-05-22, W13 T2):
- `include/gnfs/polynomial/poly_square.hpp` — `square_mod()` +
  `schoolbook_square_mod()` + `karatsuba_square_mod()` +
  `poly_square_mode()` (三态 cached env) + `poly_square_enabled()`
  predicate + `poly_square_karatsuba_threshold()` (cached env, strict
  parsing, clamp [4, 4096]) + `poly_square_reset_env_cache_for_testing()`
  (重置两个 cache)
- `tests/test_poly_square.cpp` — 16 instant tier tests, TIMEOUT 60
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  polynomial 模块

**Default Auto 主路径无影响**: `ModularPoly::sqr` / `ModularPoly::mul_raw(a, a)`
入口未改, helper 仅在显式 caller wire-in 时启用. 现有 schoolbook path /
W7 HGCD path / W9 Karatsuba helper / W11 divrem helper / W12 NTT helper
路径均保持原行为. Auto 与 ForceOn 行为一致 (squaring 启用); 仅 ForceOff
退回 W9 full-mul.

---

## Polynomial mod-p add/sub batch SIMD (GNFS_POLY_ADD_MOD_SIMD)

**ENV `GNFS_POLY_ADD_MOD_SIMD=auto|0|1`** (2026-05-23 实施, W14 T2, default auto):
F_p[x] 系数批量加减 modulo p (p < 2^32) 的 SIMD 助手, 与 W11
`GNFS_GF2_ROW_XOR_SIMD` / W13 `GNFS_GF2_AND_WORDS_SIMD` 并列, 是 polynomial
模块第一个 mod-p 算术 SIMD primitive. 应用场景: Cantor-Zassenhaus polynomial
root finding inner loop, polynomial chain compute, 系数批量加减场景的
hot path. Pure header, 不依赖外部库. 提供两个入口:

```text
add_mod_p_batch(a, b, p, out): out[i] = (a[i] + b[i]) mod p
sub_mod_p_batch(a, b, p, out): out[i] = (a[i] - b[i] + p) mod p
```

NEON 4-lane (ARM64, `vaddq_u32`/`vsubq_u32` + `vcgeq_u32`/`vcltq_u32`
条件 reduce) / AVX2 8-lane (x86_64, `_mm256_add_epi32`/`_mm256_sub_epi32` +
bias-then-`_mm256_cmpgt_epi32` 条件 reduce, 因为 AVX2 缺 unsigned compare)
替代逐元素 scalar `add + cmp + branch`.

```bash
GNFS_POLY_ADD_MOD_SIMD=auto ./gnfs <N>   # 默认: NEON/AVX2 可用且 p <= 2^31 则启用
GNFS_POLY_ADD_MOD_SIMD=0    ./gnfs <N>   # 强制 scalar (回归 bisect 用)
GNFS_POLY_ADD_MOD_SIMD=off  ./gnfs <N>   # 同 0
GNFS_POLY_ADD_MOD_SIMD=1    ./gnfs <N>   # 强制 SIMD (无 SIMD 平台 fallback)
GNFS_POLY_ADD_MOD_SIMD=on   ./gnfs <N>   # 同 1
unset GNFS_POLY_ADD_MOD_SIMD             # 同 auto
```

**Helper API** (`include/gnfs/polynomial/add_mod_simd.hpp`):
- `add_mod_p_batch(a, b, p, out)` — 主入口 (add), `out[i] = (a[i] + b[i]) mod p`.
  SIMD path 当 `poly_add_mod_simd_enabled() == true` 且 `p <= 2^31` 时启用.
  Defensive clamp 到 `min(a.size(), b.size(), out.size())`.
- `sub_mod_p_batch(a, b, p, out)` — 主入口 (sub), `out[i] = (a[i] - b[i] + p) mod p`.
- `add_mod_p_batch_scalar(...)` / `sub_mod_p_batch_scalar(...)` — scalar reference
  (test golden + 无 SIMD fallback). 用 `uint64_t` widening 处理 `a + b` 的
  partial sum, 用 `int64_t` widening 处理 `a - b` 的 negative branch, 整个
  `p < 2^32` 范围都正确.
- `poly_add_mod_simd_mode()` — 返回 `PolyAddModSimdMode { Auto, ForceOff, ForceOn }`.
- `poly_add_mod_simd_enabled()` — 三态 dispatcher decision (ForceOff → false,
  ForceOn/Auto + supported → true, 否则 false). 注意 `p` 是否在 SIMD 窗口
  内的检查在每次 `add_mod_p_batch` / `sub_mod_p_batch` 调用内部完成 (不在
  这个 predicate 内).
- `add_mod_simd_supported()` — compile-time `__ARM_NEON / __AVX2__` 探测.
- `poly_add_mod_simd_reset_env_cache_for_testing()` — 测试专用 re-resolve ENV.

**算法**:
- NEON add: `vld1q_u32(4 lanes)` × 2 inputs → `vaddq_u32` → `vcgeq_u32(sum, p)`
  mask → `vandq_u32(mask, p)` → `vsubq_u32(sum, mask_p)`. 条件 reduce 在
  register 内 branch-free 完成.
- NEON sub: `vld1q_u32(4 lanes)` × 2 inputs → `vsubq_u32(a, b)` (underflow
  wraps to `a - b + 2^32`) → `vcltq_u32(a, b)` mask (检测 `a < b` 即下溢) →
  `vandq_u32(mask, p)` → `vaddq_u32(d, mask_p)`. underflow lane 加回 `p`.
- AVX2 add: `_mm256_loadu_si256(8 lanes)` × 2 inputs → `_mm256_add_epi32` →
  bias 两侧 by `0x80000000` (XOR) → `_mm256_cmpgt_epi32(biased_p, biased_sum)`
  → bitwise NOT (XOR with all-ones) → `_mm256_and_si256(mask, p)` → sub. AVX2
  缺 native unsigned compare, 故走 bias-then-signed-compare trick.
- AVX2 sub: `_mm256_sub_epi32(a, b)` → 类似 bias 后 `_mm256_cmpgt_epi32(biased_b,
  biased_a)` 检测 `a < b` (unsigned 语义) → `_mm256_and_si256(mask, p)` → add.

**Precondition (caller responsibility)**:
- `p` 是素数 (helper 不校验 primality, 仅用 p 作 canonical reduction modulus)
- 每个输入系数已 reduced: `a[i] < p` and `b[i] < p` for all `i`. helper 不会
  re-reduce 输入 (那会掩盖 caller reduction pipeline 的 bug)

**SIMD 加速窗口**: `p <= 2^31`. SIMD path 的 conditional reduce 要求
`a + b < 2 * 2^31 = 2^32` 不溢 uint32 (才能在 register 内 branch-free
完成 `vcgeq_u32` / `_mm256_cmpgt_epi32` 比较). `p > 2^31` 时 dispatcher 自动
fallback 到 scalar reference (这是文档化行为, 不是 silent bug — scalar 路径
本身用 uint64 widening 仍正确处理整个 `p < 2^32` 范围). 单元测试覆盖
`p > 2^31` 的 fallback case (p = 2147483659, 紧邻 2^31 之上).

**Bit-for-bit guarantee**: 对任意 (a, b, p) 满足 precondition (p prime, p < 2^32,
a[i] < p, b[i] < p), SIMD path 与 scalar path 输出严格 per-index 一致. ENV
gate 值仅影响 dispatch 内核, 不影响数学结果. 输出系数永远满足 `out[i] < p`.
单元测试 `tests/test_poly_add_mod_simd.cpp` 19 个测试强制覆盖 (4 ENV 解析 +
empty / single + aligned 32 / unaligned 33 + 11-size sweep + random 1000 跨
3 primes (101, 65537, 2^31-1) + p > 2^31 fallback + sub mixed-sign explicit
fixture + algebraic identities (a+0=a, a-a=0, (a+b)-b=a) + boundary p-1
(同时验 p=2^16+1 与 p=2^31-1) + ForceOff vs Auto parity + clamp_to_min_size
+ 1M perf info probe (add + sub 都打印 wall) + reset env hook).

**Defensive clamping**: helper clamp 迭代次数到 `min(a.size(), b.size(),
out.size())`. spans 长度不同时只处理公共前缀, `out` tail 不动 (匹配 W11
`xor_words_simd` / W13 `and_words_simd` 兄弟 helper 的契约). 测试强制覆盖.

**ROI 与定位**:
- 主要 ROI: 多项式系数链上的 add/sub mod p 是 CZ root finding inner loop
  的高频 op. perf-info 实测 1M 系数 NEON ARM64 M5: add scalar 7.70ms vs SIMD
  4.23ms → 1.82x speedup; sub scalar 8.06ms vs SIMD 3.38ms → 2.38x speedup.
  Sub 加速更显著, 因为 sub 的 scalar 路径含 int64_t widening + 负数分支,
  SIMD 用单条 `vsubq_u32` + mask add 完全 branch-free.
- helper 当前 standalone (主路径 CZ root finding / `ModularPoly::add` /
  `ModularPoly::sub` 未 wire-in), 是 future-infrastructure. wire-in 时 caller
  把 inner per-coeff `c = (a + b) % p` 切到 batched `add_mod_p_batch`, sub
  类似. 适用 CZ 求根 + Karatsuba 内部 sub 累加 + polynomial chain 的 hot
  loop.
- 与 W11 `GNFS_GF2_ROW_XOR_SIMD` / W13 `GNFS_GF2_AND_WORDS_SIMD` / W11 T1
  W10 T1 等 GF(2) SIMD helper 完全 orthogonal: GF(2) word-level 操作 vs
  mod-p word-level 操作, 不同模数空间 (GF(2) 是 `mod 2`, 本 helper 是
  `mod p` for general p < 2^32). 二者可同时启用.
- 默认 auto 在 macOS arm64 / Linux x86_64 都启用 SIMD path; ENV=0 在 PMU
  sweep / sanitizer 调试时回到 scalar baseline.

**集成点** (2026-05-23, W14 T2):
- `include/gnfs/polynomial/add_mod_simd.hpp` — helper API + 三态 ENV gate +
  NEON / AVX2 inner kernels + scalar reference + `modulus_in_simd_window(p)`
  predicate.
- `tests/test_poly_add_mod_simd.cpp` — 19 instant tier tests, TIMEOUT 60.
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  polynomial 模块.

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点, ENV
对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

---

## Polynomial modular Horner batch evaluation SIMD (GNFS_POLY_HORNER_MOD_SIMD)

**ENV `GNFS_POLY_HORNER_MOD_SIMD=auto|0|1`** (2026-05-23 实施, W15 T2, default auto):
F_p[x] 多点 Horner 求值 mod p (p prime, p < 2^32) 的 SIMD 助手, polynomial
模块第二个 mod-p 算术 SIMD primitive (兄弟 helper: W10 T2 int64 Horner
`horner_batch_simd.hpp` + W14 T2 add/sub `add_mod_simd.hpp`). 应用场景:
Cantor-Zassenhaus polynomial root finding 候选 root 验证 (检验
`f(x_i) == 0 mod p` over a batch of probe points), polynomial chain compute
评估批量点, factor-base 构建 systematic small-prime probes. Pure header,
不依赖外部库. 提供 batched 入口:

```text
batch_eval_poly_mod(coeffs, xs, p, ys):
    ys[i] = (c[0] + c[1]*xs[i] + ... + c[d]*xs[i]^d) mod p
```

NEON 2-lane (ARM64) / AVX2 4-lane (x86_64) 一次性 load K 个 evaluation
points 到 SIMD register, inner Horner mul-add-reduce
`acc = (uint64(acc) * x + c[k]) % p` 在 scalar uint64 GPR per lane 跑
(NEON 缺 `vmulq_u64`, AVX2 缺整数除法). SIMD 价值在 consolidated load /
store 减小 address-gen pressure.

```bash
GNFS_POLY_HORNER_MOD_SIMD=auto ./gnfs <N>   # 默认: NEON/AVX2 可用且 p <= 2^31 则启用
GNFS_POLY_HORNER_MOD_SIMD=0    ./gnfs <N>   # 强制 scalar (回归 bisect 用)
GNFS_POLY_HORNER_MOD_SIMD=off  ./gnfs <N>   # 同 0
GNFS_POLY_HORNER_MOD_SIMD=1    ./gnfs <N>   # 强制 SIMD (无 SIMD 平台 fallback)
GNFS_POLY_HORNER_MOD_SIMD=on   ./gnfs <N>   # 同 1
unset GNFS_POLY_HORNER_MOD_SIMD             # 同 auto
```

**Helper API** (`include/gnfs/polynomial/horner_mod_simd.hpp`):
- `batch_eval_poly_mod(coeffs, xs, p, ys)` — 主入口, per-point
  `ys[i] = (c[0] + c[1]*xs[i] + ... + c[d]*xs[i]^d) mod p`. SIMD path 当
  `poly_horner_mod_simd_enabled() == true` 且 `p <= 2^31` 时启用.
  Defensive clamp 到 `min(xs.size(), ys.size())`.
- `batch_eval_poly_mod_scalar(coeffs, xs, p, ys)` — scalar reference
  (test golden + 无 SIMD fallback). 用 `uint64` widening 处理
  `acc * x + c[k]` 的 partial sum, 整个 `p < 2^32` 范围都正确.
- `horner_eval_one_mod_scalar(coeffs, x, p)` — per-point Horner, return
  `uint32_t`. SIMD path 的 tail residual 直接调用.
- `poly_horner_mod_simd_mode()` — 返回 `PolyHornerModSimdMode { Auto,
  ForceOff, ForceOn }`.
- `poly_horner_mod_simd_enabled()` — 三态 dispatcher decision (ForceOff →
  false, ForceOn/Auto + supported → true, 否则 false). 注意 `p` 是否在
  SIMD 窗口内的检查在每次 `batch_eval_poly_mod` 调用内部完成 (不在这个
  predicate 内).
- `horner_mod_simd_supported()` — compile-time `__ARM_NEON / __AVX2__` 探测.
- `poly_horner_mod_simd_reset_env_cache_for_testing()` — 测试专用
  re-resolve ENV.

**算法 (Horner schema mod p)**:
- 每个 evaluation point:
  `acc = c[d]; for k in [d-1..0]: acc = (uint64(acc) * x + c[k]) % p`.
- NEON 2-lane: `vld1_u32(2 lanes)` 读 xs 8 字节, 提取到 GPR scalar, 每 lane
  独立跑 `uint64` mul-add-mod, `vst1_u32` consolidated store 写 ys. Tail
  走 scalar.
- AVX2 4-lane: `_mm_loadu_si128(4 lanes)` 读 xs 16 字节 (SSE2 boundary load,
  避免拉 AVX2 256-bit 仅为 4×uint32), 提取 4 lane scalar, 4 个 lane mul-add-mod
  在 GPR 并发 (compiler 排度独立), `_mm_storeu_si128` store. Tail scalar.

**Precondition (caller responsibility)**:
- `p` 是素数 (helper 不校验 primality, 仅用 p 作 canonical reduction modulus).
- 每个输入已 reduced: `coeffs[k] < p` and `xs[i] < p` for all 有效 index.
  helper 不会 re-reduce 输入 (那会掩盖 caller reduction pipeline 的 bug).

**SIMD 加速窗口**: `p <= 2^31`. SIMD path 与 scalar path 都用 uint64 widening,
但 dispatcher 在 `p > 2^31` 时强制 fallback 到 scalar reference 以保持与
W14 T2 兄弟 helper 的 boundary 文档一致. scalar 路径本身覆盖整个
`p < 2^32` 范围 (产品 `acc * x` 上界 `(p-1)^2 < 2^64` 仍 fits uint64).
单元测试覆盖 `p > 2^31` 的 fallback case (p = 2147483659, 紧邻 2^31 之上,
第一个 prime > 2^31).

**Bit-for-bit guarantee**: 对任意 (coeffs, xs, p) 满足 precondition (p prime,
p < 2^32, coeffs[k] < p, xs[i] < p), SIMD path 与 scalar path 输出严格
per-index 一致. ENV gate 值仅影响 dispatch 内核, 不影响数学结果. 输出
系数永远满足 `ys[i] < p`. 单元测试 `tests/test_poly_horner_mod_simd.cpp`
19 个测试强制覆盖 (4 ENV 解析 + empty xs / empty coeffs / single xs +
deg=0 (constant) / deg=1 (linear) 手算 + random sweep deg=5 across
3 primes (101, 65537, 2^31-1) + random 1000 deg=8 + ForceOff vs Auto
parity 256 evals + unaligned size sweep 1..33 across NEON 2-lane 与
AVX2 4-lane boundaries + SIMD window boundary (p = 2^31 in-window 与
p = 2147483659 fallback) + Mersenne p = 2^31-1 + reset env cache hook +
defensive clamping (xs > ys / ys > xs / coeffs >> xs) + xs all zeros →
ys all equal coeffs[0] + 1M-eval perf info probe).

**Defensive clamping**: helper clamp 迭代次数到 `min(xs.size(), ys.size())`.
spans 长度不同时只处理公共前缀, `ys` tail 不动 (匹配 W10 T2
`horner_batch_simd` / W14 T2 `add_mod_simd` 兄弟 helper 的契约). 测试强制
覆盖. 空 xs short-circuit, 空 coeffs 写 0 到每个 ys[i] (degree-(-1)
polynomial = zero polynomial).

**与兄弟 helper 的区别 (polynomial mod-p SIMD family 第 2 名成员)**:
- **W10 T2 `horner_batch_simd` (int64 Horner, `batch_eval_poly_int64`)**:
  无模数, 累加器走 native int64 mul / add, 容许 signed wrap-around. 适用
  Murphy E rotation sweep / Kleinjung skewness grid 等无 modulus 场景.
- **W14 T2 `add_mod_simd` (mod-p add/sub, `add_mod_p_batch` / `sub_mod_p_batch`)**:
  modulus 算术但只做 add/sub, 用 NEON `vaddq_u32` / `_mm256_add_epi32` +
  conditional subtract reduce (branch-free).
- **W15 T2 `horner_mod_simd` (mod-p Horner eval, `batch_eval_poly_mod`)**:
  modulus 算术 + multiplication chain + per-step reduction. 三者覆盖不同
  caller path: int64 vs mod-p, add/sub vs full polynomial eval. 可同时启用.

**ROI 与定位**:
- 主要 ROI: 多点 polynomial modular evaluation 是 CZ root finding inner
  loop 的高频 op. perf-info 实测 1M 系数 deg=8 NEON ARM64 M5 Debug:
  scalar 22.05ms (22.05ns/eval) vs SIMD 16.24ms (16.24ns/eval) → 1.36x
  speedup; Release: scalar 9.07ms vs SIMD 7.43ms → 1.22x speedup. 加速
  比 W14 T2 add/sub 稍弱, 原因: Horner inner mul-add-mod 是 scalar uint64
  serial chain (NEON 缺 vmulq_u64, AVX2 缺整数除法), SIMD 加速主要来自
  load/store 摊销; W14 T2 add/sub 整个 conditional reduce 都能 branch-free
  vector. ROI 在 deg 更大 / 更紧凑 caller loop 时更显著.
- helper 当前 standalone (主路径 CZ root finding / `ModularPoly::evaluate`
  未 wire-in), 是 future-infrastructure. wire-in 时 caller 把 inner
  per-point Horner 切到 batched `batch_eval_poly_mod`, 适用 CZ 求根 +
  polynomial chain 的 hot loop.
- 与 W11 `GNFS_GF2_ROW_XOR_SIMD` / W13 `GNFS_GF2_AND_WORDS_SIMD` / W14 T2
  `GNFS_POLY_ADD_MOD_SIMD` / W10 T2 `GNFS_POLY_HORNER_BATCH_SIMD` 完全
  orthogonal: GF(2) vs mod-p vs int64 不同 ALU 路径, add/sub vs mul-eval
  不同算法 hot site. 二者可同时启用.
- 默认 auto 在 macOS arm64 / Linux x86_64 都启用 SIMD path; ENV=0 在
  PMU sweep / sanitizer 调试时回到 scalar baseline.

**集成点** (2026-05-23, W15 T2):
- `include/gnfs/polynomial/horner_mod_simd.hpp` — helper API + 三态 ENV gate +
  NEON 2-lane / AVX2 4-lane inner kernels + scalar reference +
  `modulus_in_simd_window(p)` predicate.
- `tests/test_poly_horner_mod_simd.cpp` — 19 instant tier tests, TIMEOUT 60.
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  polynomial 模块.

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点, ENV
对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.

---

## Polynomial subquadratic divrem (GNFS_POLY_DIVREM_SUBQUADRATIC)

**ENV `GNFS_POLY_DIVREM_SUBQUADRATIC=auto|0|1`** (2026-05-22 实施, W11 T2, default auto):
Polynomial Euclidean division helper `divrem_modp` 在 F_p[x] 上实现
Newton-reciprocal subquadratic divrem, 通过 reversed-denominator 的
power-series inverse 把 divrem 归约成两次 polynomial multiplication.
基础情形 (`num.size() < kDivremSubquadraticThreshold = 32` 或 gate 非
ForceOn) 回退 schoolbook. 与 `divrem_modp_schoolbook` 参考实现
(matching `ModularPoly::divmod` 语义) bit-for-bit 一致.

```bash
unset GNFS_POLY_DIVREM_SUBQUADRATIC            # 默认 Auto (= schoolbook, 零行为变化)
GNFS_POLY_DIVREM_SUBQUADRATIC=auto ./gnfs <N>  # 同 unset
GNFS_POLY_DIVREM_SUBQUADRATIC=0    ./gnfs <N>  # 显式 ForceOff (schoolbook)
GNFS_POLY_DIVREM_SUBQUADRATIC=off  ./gnfs <N>  # 同 "0"
GNFS_POLY_DIVREM_SUBQUADRATIC=1    ./gnfs <N>  # ForceOn (Newton-reciprocal above threshold)
GNFS_POLY_DIVREM_SUBQUADRATIC=on   ./gnfs <N>  # 同 "1"
```

**ENV 解析规则** (三态严格):
- unset / "" / "auto" → Auto (default, 当前等价于 ForceOff, 保守路由)
- "0" / "off" → ForceOff (强制 schoolbook)
- "1" / "on" → ForceOn (启用 Newton-reciprocal above threshold)
- 任何其他值 (`garbage`, `2`, `true`, `-1`, `yes`, 大小写 `ON/OFF/Auto`,
  含 leading 空白 ` 1`) → Auto

**算法** (Newton-reciprocal divrem):
- 给定 `num, den ∈ F_p[x]`, 计算 `(quot, rem)` 满足
  `num = quot · den + rem`, `deg(rem) < deg(den)`
- 系数反转: `num_rev = reverse(num)`, `den_rev = reverse(den)`
- Newton iteration 求 `den_rev^{-1} mod x^{q+1}` (q = deg(num) - deg(den)):
  从 `r_0 = den_rev[0]^{-1} mod p` (precision 1) 出发, 每轮 `r_{k+1} =
  r_k · (2 - den_rev · r_k) mod x^{2k}` 倍增 precision, O(log q) 轮收敛
- 一次乘法恢复 quotient: `quot_rev = num_rev · den_rev^{-1} mod x^{q+1}`
- `quot = reverse(quot_rev)`
- 一次乘法 + 减法恢复 remainder: `rem = num - quot · den`
- 内部 multiplication 都用 self-contained schoolbook (不依赖 W9 Karatsuba),
  保持 helper 独立; 未来 caller wire-in 可以分别 dispatch 到 Karatsuba 或
  其他 sub-quadratic primitive

**Threshold default 32 选择理由**:
- Newton-reciprocal 每轮有 per-call overhead (truncated 中间 series 分配,
  反转 / 截断系数拷贝), 加上常数性 O(log q) iteration 数
- Schoolbook 内循环紧凑, 小 deg(num) 时 quadratic walk 常数比 Newton 小
- 经验 crossover 在 32-64 之间, 选 32 与 W9 Karatsuba threshold default
  保持一致 (用户语义统一)
- ForceOn 但 `num.size() < 32` 时仍 route schoolbook (`divrem_modp` 内
  dispatch 检查), 单元测试 `test_threshold_below_routes_to_schoolbook`
  强制覆盖

**Modulus precondition**: p prime, p < 2^32 (保证 uint64 * uint64 fits
into uint64 in the schoolbook inner products and Newton iteration).
Caller 需保证 `num`, `den` 系数已 reduced mod p; `den` 非零多项式
(zero denominator 抛 `std::runtime_error`).

**Bit-for-bit guarantee**: 同 `(num, den, p)` 输入下 (p 素数, p < 2^32,
coefficients < p, den != 0), `divrem_modp` 与 `divrem_modp_schoolbook`
输出 `(quot, rem)` vector 完全一致 (size + 每位 content, 都是 trim 过
trailing zeros 的 canonical form). Gate 值仅影响 dispatch kernel,
不影响数学结果. 单元测试 `tests/test_divrem_subquadratic.cpp` 通过
17 个 case 严格覆盖 (4 ENV / 5 schoolbook unit / 7 subquadratic parity
deg 50/200/500 + 10-shape random sweep + exact-multiple + den constant +
num zero / 1 perf info).

**ROI 与定位**:
- 主要 ROI: divrem 是 W7 HGCD recursion 内部 sub-routine. 当前 HGCD
  recursion 调 `ModularPoly::divmod` (schoolbook), 整体 wall-time 在
  deg ≤ 500 略慢 (W7 实测 0.37x - 0.46x). Newton-reciprocal divrem 提供
  sub-quadratic primitive, 让 HGCD 真正 exhibit O(M(n) log n) 行为, 前提
  是 M(n) 也是 sub-quadratic (即 W9 Karatsuba 已 wire-in)
- helper 当前 standalone (主路径 `ModularPoly::divmod` 与 HGCD 未 wire-in),
  是 future-infrastructure
- perf-info probe (deg=500, p=2^31-1, 内部 schoolbook M(n)): schoolbook
  0.44 ms/call vs subquadratic 3.34 ms/call → 0.13x (subquadratic 比
  schoolbook 慢, 因为 internal mul 仍走 schoolbook, Newton 多了 O(log q)
  rounds 的常数开销). 真正 ROI 需要 wire-in Karatsuba 后 deg >> 500 才显著

**集成点** (2026-05-22, W11 T2):
- `include/gnfs/polynomial/divrem_subquadratic.hpp` — `divrem_modp()` +
  `divrem_modp_schoolbook()` + `divrem_subquadratic_mode()` (cached env
  三态 parsing) + `divrem_subquadratic_enabled()` 等价 predicate +
  `divrem_subquadratic_reset_env_cache_for_testing()` 测试 hook +
  `kDivremSubquadraticThreshold = 32`
- `tests/test_divrem_subquadratic.cpp` — 17 instant tier tests, TIMEOUT 60
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  polynomial 模块

**Default Auto 主路径无影响**: `ModularPoly::divmod` 入口未改, helper
仅在显式 caller wire-in 时启用. 现有 schoolbook path / W7 HGCD path /
W9 Karatsuba helper 路径均保持原行为. Auto 与 ForceOff 当前等价 (保守
路由 schoolbook), 仅 ForceOn 才启用 Newton-reciprocal. ENV 仅对显式
调用 `divrem_modp` 的 caller 生效.

---

## Polynomial Horner batch evaluation SIMD (GNFS_POLY_HORNER_BATCH_SIMD)

**ENV `GNFS_POLY_HORNER_BATCH_SIMD=auto|0|1`** (2026-05-22 实施, W10 T2, default auto):
多点 Horner 求值 batched helper, 把 dense polynomial `p(x) = c[0] + c[1]*x +
... + c[d]*x^d` 在批量 `xs[0..n-1]` 上的 Horner 求值切到 NEON 2-lane
(ARM64) / AVX2 4-lane (x86_64) wide load + scalar GPR inner mul-add 路径.
应用场景: Murphy E rotation sweeps, polynomial verification during
Cantor-Zassenhaus root finding, Kleinjung skewness search — 任何对小 dense
polynomial 多点求值的 hot path. Pure header, 不依赖外部库.

```bash
GNFS_POLY_HORNER_BATCH_SIMD=auto ./gnfs <N>   # 默认: NEON/AVX2 可用则启用
GNFS_POLY_HORNER_BATCH_SIMD=0    ./gnfs <N>   # 强制 scalar (回归 bisect 用)
GNFS_POLY_HORNER_BATCH_SIMD=1    ./gnfs <N>   # 强制 SIMD (无 SIMD 平台 fallback)
unset GNFS_POLY_HORNER_BATCH_SIMD             # 同 auto
```

**Helper API** (`include/gnfs/polynomial/horner_batch_simd.hpp`):
- `batch_eval_poly_int64(coeffs, xs, ys)` — 主入口, `ys[i] = c[0] + c[1]*xs[i]
  + ... + c[d]*xs[i]^d`. SIMD path 当 `horner_batch_simd_enabled()` 为 true
  时启用. `ys.size() >= xs.size()` 必须成立 (defensive clamp).
- `batch_eval_poly_int64_scalar(coeffs, xs, ys)` — scalar reference (test
  golden + 无 SIMD fallback).
- `horner_eval_one_scalar(coeffs, x)` — per-point Horner, return `int64_t`.
  SIMD path 的 tail residual 直接调用.
- `horner_batch_simd_mode()` — 返回 `HornerBatchSimdMode { Auto, ForceOff, ForceOn }`.
- `horner_batch_simd_enabled()` — 三态 dispatcher decision (ForceOff → false,
  ForceOn/Auto + supported → true, 否则 false).
- `horner_batch_simd_supported()` — compile-time `__ARM_NEON / __AVX2__` 探测.
- `horner_batch_simd_reset_env_cache_for_testing()` — 测试专用 re-resolve ENV.

**算法 (Horner schema)**:
- 每个 evaluation point: `acc = c[d]; for k in [d-1..0]: acc = acc * x + c[k]`
- NEON / AVX2 path: SIMD load 把 2 (NEON) / 4 (AVX2) 个 `xs[i]` 一次性载入,
  inner Horner 在 scalar GPR 上跑 (Apple Silicon NEON 缺 `vmulq_s64`, AVX2
  缺 `_mm256_mullo_epi64` 除非 AVX-512 DQ), SIMD store 把结果写回. SIMD 价值
  在 consolidated address-gen, 不在 vector mul.
- Tail scalar fallback: 处理 `xs.size()` 非 SIMD 宽度倍数的尾部.

**Bit-for-bit guarantee**: 同 `(coeffs, xs)` 输入下 (无 int64 溢出),
SIMD path 与 scalar path 产出 `ys` 严格 per-index 一致. 单元测试
`tests/test_horner_batch_simd.cpp` 16 个测试强制覆盖 (4 ENV 解析 + empty
xs / empty coeffs + deg=0 / 1 / 5 random 100 / 10 random 1000 + ForceOff
vs Auto parity + single-x tail + unaligned len sweep 1..33 + negative
coeff / negative x + horner_eval_one_scalar sanity + 1M-eval perf info).

**Modular overflow note**: helper 不做 overflow check. caller 负责保证
`|acc|` 在 Horner 累乘期间不溢 int64. 典型 Murphy E sample grid 满足
`|x[i]| <= skew` + `|c[k]| << 2^63 / skew^deg`, 无溢出风险. 任意精度需求
应改用 `Integer`-based polynomial API.

**ROI 与定位**:
- 主要 ROI: 1M-eval perf-info 实测 M5 ARM64 deg=8: scalar 12.82ms,
  dispatch (Auto) 10.73ms → 1.20x speedup. SIMD path 节省 per-iter
  address-gen pressure, 内核 mul-add 仍走 GPR (Apple Silicon 整数管线
  4-way superscalar, 两条 lane 并发 mul-add 自然 pipeline).
- helper 当前 standalone (主 pipeline `MurphyEvaluator` / `KleinjungSelector`
  / CZ root verify 未 wire-in), 是 future-infrastructure. wire-in 时
  caller 切到 `batch_eval_poly_int64` + 提供连续 `xs` / `ys` span.
- 初版 NEON path keep accumulator 在 `int64x2_t`, `vsetq_lane_s64`
  per-iter round-trip 导致 0.13× 慢于 scalar; 修复为 inner loop 全 GPR,
  仅 boundary load/store SIMD, 恢复 1.20× 加速.

**集成点** (W10 T2, 2026-05-22):
- `include/gnfs/polynomial/horner_batch_simd.hpp` — helper API + ENV gate +
  NEON / AVX2 inner kernels + scalar reference.
- `tests/test_horner_batch_simd.cpp` — 16 instant tier tests, TIMEOUT 60.
- `CMakeLists.txt` / `scripts/test.sh` — 注册 instant tier, 60s timeout,
  polynomial 模块.

**Default ON (auto)**: helper standalone, 当前主 pipeline 无调用点,
ENV 对运行行为无影响. 仅 helper 被 wire-in 后 ENV 才生效.
