# SIQS 运行时开关

## `GNFS_SIQS_SHADOW_PROOF`

`GNFS_SIQS_SHADOW_PROOF` 控制生产 SIQS 在不改变旧求解路径的前提下，是否运行一次有界的 shadow proof 并输出结构化观测记录。该开关只用于采集迁移证据，不会把 shadow proof 的因子结果用于生产返回，也不会启用 two-large-prime（2LP）收集。

### 取值与解析

| 取值 | 模式 | 行为 |
|------|------|------|
| 未设置 | `off` | 完全沿用 legacy 路径 |
| `0` | `off` | 完全沿用 legacy 路径 |
| `observe` | `observe` | 在 legacy merge 前运行一次只读 shadow proof |

解析是严格且大小写敏感的。空字符串、`1`、`Observe`、前后带空格的值以及其他任何值都会抛出 `std::invalid_argument`，不会静默回退。该异常是公开的 fail-closed 边界，调用方必须处理；没有统一异常处理的命令行入口可能因此终止。

`factor()` 在入口处、开始计时和任何 SIQS 工作之前调用一次 `getenv` 并冻结解析结果。该值不做进程级缓存，因此下一次 `factor()` 调用可以使用新的环境值；一次调用进行期间的环境变化不会改变已冻结的模式。并发修改进程环境不属于支持的使用方式。

### Default 与 bit-for-bit 契约

Default 是未设置，即 `off`。未设置和 `0` 都不进入 shadow 分支、不分配 shadow 状态，也不新增 stderr 输出；它们保留原有 legacy merge、linear algebra 和 extraction 控制流及 factor/relation 结果语义。现有的 `time_seconds` 是墙钟测量，本身不承诺不同进程或重复运行之间逐位一致。

`observe` 不是零开销模式。shadow proof 位于最终 `SIQSResult::time_seconds` 的计时范围内，因此会增加墙钟时间，且可能间接改变并发调度下的运行统计。它只保证不修改 raw relations，并继续执行同一个 legacy merge、solve 和 extract 路径；测试只要求最终因子结果和关系语义一致，不要求 wall time 或所有统计字段逐位相同。

### 集成点与输出

生产接线位于所有 sieve workers `join()` 之后、`merge_partials()` 修改 raw relations 之前：

1. 读取进程内存快照。
2. 从 factor base 构造包含 sign sentinel 的 `{fb.p}` 向量。
3. 以 `const` raw-relation span、`kN`、原始 `N`、当前 1LP bound、`split_cofactor_64` 和默认 `SIQSShadowProofOptions` 调用 `run_siqs_shadow_proof()`。
4. 再读取一份进程内存快照，生成并尝试写出一行 `GNFS_SIQS_SHADOW_PROOF_OBSERVE_V1` 记录。
5. 忽略 shadow 结果，继续 legacy merge、solve 和 extract。

显式 `observe` 即使 `verbose=false` 也尝试写出记录。输出属于独立的机器可读观测通道，不受普通 SIQS 诊断日志开关控制。

所有 shadow terminal outcomes 都是观测结果，而不是生产分解结果。factor-base 向量准备期间的 `std::bad_alloc` 会记录为 `resource_exhausted`，其他异常会记录为 `exception_failure`。runner 已将其内部失败映射为 typed result。记录构造或 emitter 失败同样不得影响 legacy 路径；emitter 失败时该次调用可能没有完整记录，但仍继续分解。唯一有意向调用方传播的新增异常是入口处的非法环境值。

### 固定默认上限

生产 observe 当前使用 `SIQSShadowProofOptions{}`，不从其他环境变量调参：

| 边界 | 默认值 |
|------|-------:|
| raw relations | 32768 |
| portable raw payload | 64 MiB |
| graph edges | 16384 |
| graph cycles | 4096 |
| graph cycle incidences | 262144 |
| row candidates | 4096 |
| pretrim rows | 4096 |
| minimum row excess | 1 |
| assembly trim excess | 100 |
| materialization workers | 1 |
| returned dependencies | 64 |
| elimination workers | 1 |
| parallel column threshold | 20000 |
| packed dense matrix payload | 256 MiB |
| dense variables | 100000 |

所有容量边界都是 inclusive maxima：等于上限时仍可进入下一阶段，只有下一个对象会产生 typed bounded fallback。workers 默认固定为 1；在取得生产 live-row 基准之前，不根据硬件线程数自动调高 shadow workers。

### 2LP 与 RSS 限制

生产 collector 仍设置 `lp_bound_sq = 0`，因此 2LP 保持关闭。observe 只分析当前 full + 1LP raw corpus；它不会改变 sieve admission，也不能作为生产 2LP yield 的证据。

observe 的 before/after 内存字段是进程级端点快照，不是 shadow 阶段的精确瞬时峰值。为保证 legacy fallback，raw relations 在 shadow rows 和 packed matrix 存活时仍被保留；分配器也可能在 shadow 对象析构后保留页面。因此：

- lifetime peak 只有在 fresh process 中才可解释；
- after-minus-before 不能替代 shadow 瞬时峰值；
- 不得套用 256-A V4 proof executable 在提前释放 raw storage 后测得的较低峰值；
- 不支持的平台会明确记录 RSS 不可用，而不是伪造 `0`。

### 测试

- `tests/test_siqs_shadow_proof_observe.cpp` 覆盖严格 parser、typed record、RSS 可用性字段和 emitter 合同。
- `tests/test_siqs_shadow_proof_runner.cpp` 覆盖各阶段 terminal status、inclusive caps、异常和输入不可变性。
- `tests/test_siqs.cpp` 使用跨平台 RAII 环境变量夹具，验证 `143 = 11 * 13` 在 `0` 与 `observe` 下返回同一规范因子对，并验证非法值在 SIQS 工作开始前抛出。

常用验证命令：

```bash
./scripts/test.sh run test_siqs_shadow_proof_observe
./scripts/test.sh run test_siqs_shadow_proof_runner
./scripts/test.sh run test_siqs
./scripts/test.sh changed --deep
```
