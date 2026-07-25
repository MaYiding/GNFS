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

`prefer` 运行模式尚未接线，因此也属于非法值。当前 V1 记录只表示
`mode=observe`、`route=legacy_continue` 和 `promotion=false`，不能将它解释为
shadow 结果路由协议。

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

### Fresh-process production probe

生产重叠 RSS 合同使用固定
`siqs50_production_shadow_observe_v1` profile 和当前 1LP collector。单次
probe 与四进程 comparison 命令如下：

```bash
./scripts/test.sh probe-siqs-shadow-observe off
./scripts/test.sh probe-siqs-shadow-observe observe
./scripts/test.sh compare-siqs-shadow-observe
```

每个 probe 都以 Release/NDEBUG fresh process 启动，只调用一次 `factor()`。
成功进程输出一条闭集
`GNFS_SIQS_SHADOW_PROOF_OBSERVE_PROBE_V1` 记录。关键字段包括 `mode`、
`sample_ordinal`、`factor_identity`、`factor_wall_ns`、before/after RSS、
`peak_growth_supported`、`peak_growth_bytes`、`route=legacy_result` 和
`promotion=false`。`observe` 进程还必须从生产 seam 输出恰好一条合法的
`GNFS_SIQS_SHADOW_PROOF_OBSERVE_V1`；`off` 进程不得输出该记录。

`compare-siqs-shadow-observe` 先构建一次，再启动 1 个 `off` 和 3 个
`observe` fresh processes。全部样本通过独立 schema、因子和 RSS 守恒检查后，
才输出
`GNFS_SIQS_SHADOW_PROOF_OBSERVE_COMPARISON_V1`。comparison 检查因子一致性、
proof、matrix shape 和 RSS 可用性，但不比较 raw corpus 或 fingerprint identity。
生产多线程 collector 的完成边界受调度影响，因此记录固定为
`identity_compared=false`。

两种 schema 的 before/after 范围不同。`PROBE_V1` 在调用 `factor()` 前取 before
快照，并在 legacy 结果返回后取 after 快照；其 peak growth 覆盖完整 factor
调用。observe V1 telemetry 的 before peak 是进程启动至 post-join、pre-shadow
seam 的 lifetime HWM；after peak 是进程启动至 shadow 完成的 lifetime HWM。
此时 raw relations 在 shadow rows 和 packed matrix 存活期间保持有效。
`peak_growth_bytes` 只表示同一 record 内 after HWM 超过 before HWM 的部分，
不表示分配总量。off/observe 的跨进程差值也只用于描述样本分布，不能冒充同
corpus 的因果差值。

当前 comparison 只报告 min/max wall、peak 和 growth。它固定输出
`timing_threshold_applied=false`、`rss_threshold_applied=false`、
`prefer_scope=explicit_experiment_only`、`shadow_outcome_routed=false` 和
`promotion=false`。只有 off 与三次 observe 的必要 RSS 观测全部可用且 backend
一致时，`experiment_eligibility=candidate`；否则记录
`rss_evidence=unavailable` 和 `experiment_eligibility=insufficient_evidence`。
手工运行结果不能冻结 RSS 预算，也不能勾选 production promotion gate。

上述样本固定归类为 `calibration_excluded`。它们只验证采集协议、RSS 可用性和
量级，不得重新用作 promotion gate。正式语料已在
`src/siqs/shadow_proof_rss_holdout_fixture_internal.hpp` 中 outcome-blind 封存。
corpus ID 为 `siqs50_shadow_observe_rss_holdout_v1`，包含 8 个全新、balanced 的
50 位 semiprimes。header 使用公开 decimal base/stride 常量和 GMP
`mpz_nextprime` 规则确定因子，selection protocol ID 为
`gmp_nextprime_decimal_stride_v1`。header 固定规范因子顺序，并以 stable、
non-cryptographic 128-bit identity digest 绑定有序 corpus identity；digest lanes 为
`low=303806906129662515` 和 `high=18179245792498443738`。

`tests/test_siqs_shadow_observe_rss_holdouts.cpp` 只验证 corpus ID、数量、十进制位数、
seed 生成、规范因子顺序、probable-prime identity、乘积、唯一性和 digest。该测试
不会调用 production `factor()`、observe probe 或任何 RSS measurement path。上述
8 个输入从未产生 production factorization result、shadow proof record、timing
sample 或 memory measurement，因此 holdout 尚未打开。现有
`GNFS_SIQS_SHADOW_PROOF_OBSERVE_PROBE_V1` 和
`GNFS_SIQS_SHADOW_PROOF_OBSERVE_COMPARISON_V1` 仍只属于 calibration evidence。

Darwin、Linux 和 Windows 必须分平台评估；每个 backend、每个 fixture 分别运行
3 次 `off` 和 7 次 `observe` fresh processes。预注册的判定量只能是 `observe`
的绝对 process peak RSS，并与部署方批准的内存预算减去预留 headroom 后的上限
比较。`off` 与 `observe` 的差值以及 record 内 `peak_growth_bytes` 只保留为诊断
信息，不能单独通过或否决 gate。

### Pure RSS Gate Contract

纯 typed gate 位于 `include/gnfs/siqs/shadow_proof_rss_gate.hpp`。
`evaluate_siqs_shadow_proof_rss_gate` 接收
`SIQSShadowProofRssGatePolicy` pointer 和 `SIQSShadowProofRssGateSample` span。它不读取
环境变量，不调用 production `factor()`，也不运行 probe 或 measurement。null policy
返回 `status=blocked reason=policy_missing`。有效 policy 必须绑定 `approval_id`、
sealed corpus ID 和 digest、operating system、architecture、RSS backend、resolved
production sieve workers、candidate revision、deployment budget 和 reserved headroom；
以及 deployment-owned trusted-base ID、journal-store ID 和 canonical lowercase ASCII
relative locator，以及 probe executable SHA-256 和 canonical execution-contract
SHA-256；gate 不负责发现或批准这些值。absolute path 和 runtime inode/file ID
不进入 policy binding。任何 filesystem I/O 之前，production-owned registry 必须
根据 trusted-base ID 解析已配置的 base，且不得接收 caller path、resolver 或 base
handle；locator 对应的 store ID 也必须与 provisioned mapping 一致。

coverage 必须严格等于 80 个 fresh-process records。8 个 fixture 中的每一个都必须
恰好包含 3 个 `off` 和 7 个 `observe` records；缺失、重复或额外 coverage 均不能
通过。唯一判定量是每个 `observe` record 的 absolute process peak RSS，并要求
`peak <= budget - headroom`，因此等号通过。`off` records、跨进程 delta、current
RSS、`peak_growth_bytes` 和 wall time 都只用于诊断，不能改变 gate 结果。

`SIQSShadowProofRssGateStatus` 是闭集：`blocked`、`invalid`、`limit_exceeded` 和
`manual_review_candidate`。只有
`status=manual_review_candidate reason=all_observe_peaks_within_limit` 表示通过，且
仅进入人工审查。terminal `SIQSShadowProofRssGateOutcome` 还保存完整 probe execution
identity，以及覆盖完整 policy binding 的 stable、non-cryptographic checksum。
`emit_siqs_shadow_proof_rss_gate_outcome` 会重新评估 policy 和完整 sample span，并
要求结果与 supplied outcome 完全一致，然后输出以
`GNFS_SIQS_SHADOW_PROOF_RSS_GATE_V3` 开头的 closed record。emitter 只接受
`limit_exceeded` 和 `manual_review_candidate`；`blocked` / `invalid` 保持 typed
outcome，但不形成 audit line。terminal record 同时输出
`probe_executable_sha256`、`probe_execution_contract_sha256`、
`policy_binding_digest_low` 和 `policy_binding_digest_high`。Outcome 中的
`shadow_outcome_routed=false` 和 `promotion=false` 对所有 status 固定，不会启用
`prefer` 或返回 shadow factor；terminal record 也固定输出这两个值。

campaign journal 的 schema 和 wire V3 会将 `synthetic_test` 或
`production_holdout` 分类及两段 32-byte SHA-256 直接绑定到 runtime facts、header、
plan/record digest、commit payload 和 joined artifact。header/record 固定宽度分别为
160B 和 320B；V1/V2 wire 均 fail closed。完整 synthetic campaign 只能终止为
`synthetic_complete` 且 `action=none`，不能重建 gate samples。

private deployment row 同时持有完整 approved policy 和 expected runtime contract。
调用方提供的 policy/runtime 只是声明；store 选出唯一 row 后逐字段核对，并仅用
row-owned 值构造 session。store 会重算 canonical execution contract。schema V2
除平台、build mode、完整排序 environment、timeout、owner、argument template、capture
上限和 output schema 外，还绑定 launch profile、固定逻辑 `argv[0]` 和 profile-specific
transport ID。authenticated profile 会按平台拒绝 `LD_*`、`DYLD_*` 和
`GLIBC_TUNABLES`。相对 executable path、revision mismatch、非法或未排序 environment、
configured-owner mismatch、非法 timeout、零 identity 或 identity mismatch 都会在打开
journal 目录前失败。production row 必须配置 probe binding，且不能使用
`PublicationOps` 测试 seam。

Linux 的 `linux_sealed_memfd_execveat_v1` 已闭合 same-object launch。start record
持久化且 pending slot 再验证后，runner 才为该 slot 创建一次性 move-only capability。
它以 `O_NOFOLLOW` 打开批准路径，验证 owner、link、mode 和 bounded size，将稳定 ELF
字节复制到 `MFD_EXEC` memfd，核对 source SHA-256，施加包含 `F_SEAL_EXEC` 的完整
seals，再重新 hash sealed object。child 使用固定逻辑 `argv[0]` 和
`execveat(..., AT_EMPTY_PATH)`；之后替换原路径不会改变执行对象。capability 每个 slot
只消费一次。child 在任何其他 setup 前设置 `PR_SET_PDEATHSIG(SIGKILL)`，随后确认
`getppid()` 仍等于 `_Fork` 前捕获的 launcher PID；即使 supervisor 被 `SIGKILL`、无法
执行常规 process-group cleanup，direct probe 也会终止。对应 transport ID 为
`gnfs.util.authenticated_bounded_child_process.linux_memfd_execveat_pdeathsig.v2`，
contract version 为 2，旧 execution identity 不能复用。authentication 或 launch
失败会 durable taint，不能发布 artifacts 或 sample commit；production commit 还必须
携带私有 same-object evidence。

该 Linux profile 要求 executable memfd、`F_SEAL_EXEC`、`execveat`、`close_range`、
`_Fork`、`prctl` 和 `getppid`，缺失或被 host policy 阻止即 fail closed，且不回退到
path spawn。`PR_SET_PDEATHSIG` 绑定创建 child 的具体线程，且 child 后续 `fork()` 不会
继承该设置。因此 approved probe 固定为 direct、no-fork/no-descendant process，runner
必须在同一个持续存活的同步调用线程内完成 launch；这不是任意 process tree 的
containment 保证。digest 只认证主 ELF image，不认证 dynamic loader、shared libraries、
kernel、Linux Security Module policy 或外部配置，这些仍属于 approved deployment 与
host boundary。approved timeout 是 authentication 成功后才开始的 child launch/capture
deadline。authentication 本身只受 256 MiB size cap 约束，使用同步 regular-file I/O；
慢或阻塞 filesystem 仍可能造成 availability stall，真正的时钟上限需要后续加入独立监督
的 authenticator。

普通 POSIX path transport 保持 libc 可移植，并由 Alpine Linux musl 定向 CI
验证构建与 transport test。authenticated descriptor launch 的范围更窄：只有现代
glibc 且所需 syscall 与 macro 完整时才可用。musl 等 unsupported libc 会在访问
campaign journal namespace、executable path 或权限前返回 `platform_unavailable`。
在受支持的 glibc 构建上，被归类为 capability unavailable 的 runtime 拒绝同样返回
`platform_unavailable`，并通过 `native_error` 保留原生错误。项目不支持 musl
authenticated launch；该可移植性边界不会修改现有 transport ID。

`darwin_hardened_suspended_v1` 只保留 schema 合同，尚未实现 production launch。
当前 hardened probe 链接的 Homebrew GMP 与主 binary 使用不同 signing identity，
无法形成统一 loaded-code signer chain。因此 macOS production row 会在 journal
filesystem I/O 前返回 unavailable；private test 才能使用 synthetic path profile。
Windows 继续显式 fail closed。authority-held terminal gate transaction 已按默认关闭
方式实现；serial 80-slot production entry point 和首个 production deployment row
仍待单独批准。

编译进 `gnfs_core`、但不安装 internal header 的 source-private 层已经有 fresh-only
serial controller。它只接收空 journal 的
`ready/create_header/count=0/next=1` session，并在同一 lease 链上按 canonical
顺序启动全部 80 个 synthetic child。partial committed prefix、complete journal
和 dangling start 都会在零 child、零新 journal 写入下被拒绝；controller 不拥有
reopen、resume、pending-taint recovery、retry、callback、cancellation 或 gate
evaluation 权限。begin/commit 不确定以及 taint closure 失败只返回
`reconcile_required`，不会暴露可能过时的 terminal view。完整 synthetic run 的唯一
终态是 `synthetic_complete/none`，不会获得 gate authority。

独立的 source-private reconciliation boundary 已闭合 nonfresh journal 的恢复侧，
但不会给 controller 增加权限。调用方只能提供 policy/runtime claims；deployment
仍由私有 registry 唯一选择。reconciliation 每次重新打开 namespace 并取得 fresh
native lease，然后只按磁盘证据分类：

- 真正 pristine 且没有 persistent lock 时返回 `no_nonfresh_state`，不创建 lock、
  header 或 record；
- header-only 或 committed prefix 会依次确认 header，以及每个已提交 slot 的
  start、三个 artifact 和 commit，再返回 `stable_prefix_confirmed`；
- dangling start 会先确认完整 predecessor chain 和 start，只追加一次精确派生的
  taint，严格 reread 后返回 `dangling_start_durably_tainted`；
- explicit taint 会确认 prefix、start、精确 taint 和 replay；完整 80-slot journal
  会确认 header 加 400 个 slot 对象；两者都只返回 `terminal_confirmed`。

nonfresh state 缺少 lock、lease 正被持有、layout/identity/generation 漂移、任一
durability confirmation 失败，或 taint publication 已开始但结果不确定，都会返回
`reconcile_required` 且不携带 observation。同一次调用不会在 publication 后重试；
后续调用必须重新打开磁盘状态。registry/preflight 拒绝单独返回
`admission_rejected`。reconciliation 不会 launch child，因此不要求当前平台具备
authenticated-launch capability；但 deployment row 仍必须完整有效。

`gnfs_core` 不提供接收 deployment table、path、handle 或 publication seam 的
reconciliation 函数。closed selector 会把自己持有的 deployment row 转换成
move-only opaque binding，platform loader 只消费该 binding。session 与
reconciliation 使用彼此独立的 interface 和 concrete wrapper；reconciliation
wrapper 不是 `SessionCore`，只暴露 `reconcile()`。fixture deployment 注入桥仅编译
进 native-store 测试可执行文件，不进入 `gnfs_core`。最终 projector 还要求精确匹配
deployment 派生的非零 plan digest；outcome、observation 或 diagnostic 任一组合不合法
都会 fail closed。

其 copyable result 只含 outcome、diagnostic，以及可选的 confirmed
status/reason/committed-count/plan-digest。类型不暴露 session、active slot、action、
next slot、retry、reopen、callback、controller、executable 或 gate。internal header
不安装，默认 production registry 继续为空。

同一 closed registry 还供 source-private terminal gate transaction 唯一选择
deployment。调用方只能提交 policy/runtime claims；测试部署桥只进入 native-store
测试二进制。pristine namespace 或未完成 prefix 返回 `gate_not_ready`，不创建 lock
或 terminal leaf。只有完整 `production_holdout` journal 才会在 fresh lease 下确认
header 和 400 个 slot 对象、严格 reread 全部 journal/artifact、重建 80 个样本并调用
pure gate。

成功结果固定写入 192-byte `terminal-gate.rtgr`。该 immutable record 绑定 plan、
最终 journal record、policy、probe executable、execution contract、样本计数、RSS
limit、max observe peak 和 gate outcome；只允许 `limit_exceeded` 或
`manual_review_candidate`，并保持 route/promotion 为 false。首次提交只有在 terminal
leaf 自身确认 durable 且 strict reread 精确匹配后才返回 observation。精确重开会先
确认 401 个 predecessor，再把 terminal leaf 作为第 402 个对象确认。

malformed bytes 返回 `layout_invalid`；可正常解码但不等于当前 approved binding 与
leased journal 唯一推导值的 record 返回 `publication_conflict`。同次调用观察到
`already_exists`、发布后确认失败或 durability action 后发生 drift 时，只返回
`outcome_uncertain` 且不携带 observation；可证明发生在创建前的失败返回
`reconcile_required`。session、reconciliation、terminal 三条 open 路径都会识别该
leaf 并对异常状态 fail closed。最终 copyable result 不含 samples、policy、
deployment、path、session、retry、callback、routing 或 promotion authority。

同一私有层还包含默认关闭的 production composition。它先复用 journal 的精确
absent-state preflight，随后在 registry lookup 前拒绝 synthetic claim；production
claim 只能调用一次公开 store-open boundary，成功得到的 session 必须立刻被 controller
消费。返回值只含 outcome、diagnostic 和可选 controller 数据，不含 session、retry、
reopen、recovery 或 gate 能力。默认 registry 为空，因此表面合法的 production claim
固定返回 `binding_not_registered/deployment_registry`，不会创建目录、写 journal、
启动 child 或读取 sealed holdout。该 composition 没有 installed public header；
它不是受支持的 installed API。加入首个 production registry row 属于入口激活提交，
必须携带仍待完成的审批证据，不能作为普通 packaging 变更。批准 policy、部署
registry row 和公开生产入口仍是独立的后续审批里程碑。

durable receipt、launch permit、authenticated executable image、session、
active slot 及其 authority-bearing result wrapper 只允许 move construction，
不允许 move assignment，避免覆盖目标时静默丢弃现有 lease 或 one-shot capability。
这是 journal-store 已安装类型的有意破坏性源码/链接兼容收紧；调用方必须改用
move construction，或只对空 `optional` 使用 `emplace()`。
`SIQSShadowProofRssPreparedSlotStart` 是例外：它是 durable publication 前可由 replay
重建的 proposal，verified refresh 可以在任何 execution authority 产生前替换它。
source-private artifact-batch receipt 更严格：它只在持有 lease 的 core 内原位构造，
既不可复制也不可移动。

当前没有批准的 per-platform policy，也没有实际 budget、headroom 或阈值。
production deployment registry 仍为空，sealed holdout 尚未运行，80-process
campaign 仍是 `blocked` / `pending`。policy 获批前不得构造或启动 campaign，也不得
写入任何 holdout 结果。terminal transaction 的存在不激活 production entry，也不
产生真实 gate outcome。

### Pure V2 `prefer` Decision Contract

V2 只定义纯决策和审计合同，不能放宽或重解释 observe V1。当前 parser 仍只接受
未设置、`0` 和 `observe`；`prefer` 仍会 fail closed。`factor()` 和生产 observe
seam 都没有接入 V2，也不会返回 shadow 结果。

纯实现位于 `include/gnfs/siqs/shadow_proof_prefer.hpp`。
`evaluate_siqs_shadow_proof_prefer` 先生成 owning draft，
`finalize_siqs_shadow_proof_prefer(SIQSShadowProofPreferDraft&&, uint64_t)`
只接收 owning rvalue draft，再附加调用方提供的 wall-time 样本，
`emit_siqs_shadow_proof_prefer_decision` 输出以
`GNFS_SIQS_SHADOW_PROOF_PREFER_DECISION_V2` 开头的单行闭集记录。记录固定
`schema_version=2`、`status=valid`、`mode=prefer`、
`emit_phase=before_route` 和 `promotion=false`。`decision` 只能是 `shadow_candidate` 或
`legacy_fallback`；`reason` 只能是 `shadow_factor_valid`、
`shadow_not_factor`、`shadow_contract_invalid`、`factor_identity_invalid`、
`result_metadata_invalid` 或 `decision_internal_failure`。

V2 字段顺序和语义固定如下：

| 字段 | 语义 |
|------|------|
| `schema_version status mode` | 固定为 `2 valid prefer`；`valid` 表示审计记录自身通过一致性检查，不表示 shadow 一定成功 |
| `decision reason next_route` | 闭集决策、闭集原因和尚未执行的路由建议 |
| `shadow_terminal shadow_stage shadow_fallback` | 被审计 typed shadow result 的 terminal、最后阶段和 bounded fallback 原因 |
| `factorization_present input_n factor cofactor factor_identity` | source 是否报告 factorization、原始十进制输入和重新验证结果；fallback 不输出因子值，`factor` / `cofactor` 为 `0` |
| `result_present relations_found relations_source` | 只有 candidate 携带 future result；关系数来自 `shadow_selected_rows` |
| `polynomials_used polynomials_source` | 只有 candidate 携带 post-join `production_sieve_counter` |
| `decision_wall_ns_supported decision_wall_ns time_scope` | candidate 携带正的 caller sample 和 `siqs_timer_to_pre_emit_decision`；fallback 为 `false`、`0`、`unavailable` |
| `emit_phase promotion` | 固定为 `before_route false` |

`factorization_present` 只陈述 source 是否报告了 factorization，因此它在某些
fallback 中仍可为 `true`；只有 `result_present=true` 才表示 candidate result
存在。`factor_identity` 的闭集值是 `pass`、`fail` 和 `not_checked`。对
candidate 传入零 `decision_wall_ns` 会使
`finalize_siqs_shadow_proof_prefer` 转为
`reason=result_metadata_invalid` 的 legacy fallback。

V2 对每次候选 early return 重新检查 typed result：terminal 必须为
`factor_found`，factorization 必须存在，两个因子必须非平凡且采用规范顺序，
并且乘积必须精确等于原始输入 `N`。任何未知 enum、terminal / fallback / factor
presence 矛盾、proof evidence 不守恒、结果指标非法或内部异常，都只产生
`legacy_fallback` 决策。

V2 record 中的 `next_route` 是在 `emit_phase=before_route` 时生成的下一步建议，
不是已经执行的返回路径。纯合同不会自行路由。未来接线只有在完整 record 成功
写出、flush 成功且 stream 无错误，emitter 返回 `true` 后，才能把该成功视为
shadow route 的 commit point；写入失败、部分写入、flush 失败或 stream 错误都必须
继续未修改的 legacy 路径。即使失败通道留下完整
可见行，该行仍只陈述 pre-emit 决策，不声称 route 已执行。

`shadow_candidate` 记录使用 `next_route=shadow_return`、
`relations_source=shadow_selected_rows`、
`polynomials_source=production_sieve_counter`、
`decision_wall_ns_supported=true` 和
`time_scope=siqs_timer_to_pre_emit_decision`。`legacy_fallback` 则使用
`next_route=legacy_continue`，不携带 candidate result，相关 source 为 `none`。

未来 shadow `SIQSResult` 的指标语义固定如下：

- `relations_found` 等于实际送入 shadow matrix 的 selected row 数，并且必须与
  matrix row count 一致；它不是 raw、pretrim 或 graph edge 数。
- `polynomials_used` 复用所有 production sieve workers join 后的 polynomial
  counter，不使用 shadow 内部计数。
- `time_seconds` 由同一份 pre-emit decision wall-time 样本派生。它从现有 SIQS
  timer 起点计到 pure evaluation 完成，包含 shadow proof、factor / evidence
  验证和 accepted-factor copy；样本在
  `finalize_siqs_shadow_proof_prefer` 和 emitter I/O 之前取得。未来的
  `SIQSResult` 必须直接复用该值，不得重新采样。

实验期采用 emit-before-route：先完整构造 shadow `SIQSResult` 和 V2 decision
record；未来只有 emitter 成功后才能执行其 `next_route=shadow_return` 建议。记录
构造或 emitter 失败时继续未修改的 legacy 路径。所有 `no_factor`、bounded
fallback、invalid input、stage failure、resource exhaustion、exception 和
invariant failure 同样继续 legacy。默认模式仍是 `off`，且 future `prefer`
只能先作为显式实验，不能由当前 candidate comparison 或 sealed holdout 自动
启用。

### 测试

- `tests/test_siqs_shadow_proof_prefer.cpp` 覆盖纯 V2 决策、防御性 typed
  result / factor / metadata 验证和 pre-route emitter 合同。
- `tests/test_siqs_shadow_proof_observe.cpp` 覆盖严格 parser（包括拒绝
  `prefer`）、typed record、RSS 可用性字段和 emitter 合同。
- `tests/test_siqs_shadow_observe_rss_holdouts.cpp` 只覆盖 sealed corpus 的数学和
  identity 合同；它不调用 production `factor()` 或 probe，也不采集 outcome。
- `tests/test_siqs_shadow_proof_rss_gate.cpp` 使用 synthetic policy 和 records 覆盖
  policy binding、严格 80-sample coverage、budget 等号边界、diagnostic independence
  和 terminal-only closed emitter；它不运行 sealed holdout。
- `tests/test_siqs_shadow_proof_rss_probe_execution_identity.cpp` 覆盖 executable
  SHA-256、schema V2 launch profile、canonical contract、严格 environment 和单字段
  mutation；它不启动 child。
- `tests/test_bounded_child_process.cpp` 只启动 synthetic fake child。Linux case 会覆盖
  sealed-image authentication、认证后 path replacement、one-shot consumption、timeout、
  overflow、descriptor closure 和并发 launch；它不读取 sealed fixture。
- `tests/test_siqs_shadow_proof_rss_campaign_journal_store.cpp` 使用临时本地目录和
  subprocess 覆盖 deployment registry、严格 native layout、跨进程 lease、崩溃释放、
  held-root publication、Linux same-object authentication、认证失败后的 durable taint、
  私有 same-child commit、fresh-only controller 实际串行启动 80 个 synthetic child
  后的完整终态、prefix/dangling 拒绝、begin/taint/commit 不确定性闭包、source-private
  reconciliation 的 pristine 零写入、缺锁/活跃 lease 拒绝、逐 durable object
  confirmation fault、dangling 精确 taint、uncertain publication 后 reopen、explicit
  taint 和完整 80-slot/401-confirmation 终态，以及 recovery 与 launch capability
  解耦；还覆盖第 41 槽 commit 确认失败后不启动第 42 槽并由 reopen 观察真实前缀、
  terminal 第 402 次确认、durable publish/confirm 后真实进程崩溃重开、durable
  half-frame 残留的三入口零修复拒绝、restart relabel rejection，以及 execution
  identity 与完整 approval/runtime 字段错配的 journal 零写入拒绝。
  测试只启动 synthetic child，不运行 production probe 或打开 sealed holdout。
- `tests/test_siqs_shadow_proof_rss_terminal_gate_record.cpp` 只覆盖 synthetic
  fixed-width record 的 round trip，以及 length、magic、version、reserved/tag、
  terminal outcome、identity 和 digest 的 fail-closed 解码。
- `tests/test_siqs_shadow_proof_rss_campaign_entry.cpp` 独立覆盖 source-private
  composition 的 policy-first preflight、synthetic classification 拒绝、默认空
  registry 的两次一致拒绝和递归目录快照不变；它不注入 deployment、session、
  controller callback 或成功 production 分支。
- `tests/test_siqs.cpp` 锁定公开 `factor()` 路径对 `prefer` 的 fail-closed
  拒绝，并确认拒绝前不发出 V1 或 V2 记录。
- `tests/test_siqs_shadow_proof_observe_probe.cpp` 提供 Release-only production 1LP fresh-process measurement target；它不进入 CTest 或常规测试 tier。
- `tests/test_siqs_shadow_proof_runner.cpp` 覆盖各阶段 terminal status、inclusive caps、异常和输入不可变性。
- `tests/test_siqs.cpp` 使用跨平台 RAII 环境变量夹具，验证 `143 = 11 * 13` 在 `0` 与 `observe` 下返回同一规范因子对，并验证非法值在 SIQS 工作开始前抛出。

常用验证命令：

```bash
./scripts/test.sh run test_siqs_shadow_proof_prefer
./scripts/test.sh run test_siqs_shadow_proof_observe
./scripts/test.sh run test_siqs_shadow_observe_rss_holdouts
./scripts/test.sh run test_siqs_shadow_proof_rss_gate
./scripts/test.sh run test_siqs_shadow_proof_rss_campaign_journal_store
./scripts/test.sh run test_siqs_shadow_proof_rss_campaign_entry
./scripts/test.sh run test_siqs_shadow_proof_runner
./scripts/test.sh run test_siqs
./scripts/test.sh probe-siqs-shadow-observe observe
./scripts/test.sh compare-siqs-shadow-observe
./scripts/test.sh changed --deep
```
