# GNFS 候选开发方向梳理

> 本文是 2026-11-26 一次多代理梳理的定稿，所有行号基于 `main@d54aba29`，代码演进后行号会过期。
>
> **怎么来的。** 十路并行的只读调研（历史脉络、文档里的声明、性能与规模、正确性风险面、筛选与余因子分解、过滤合并与线性代数、平方根与前端、跨平台与发布、CI 与测试体系、可维护性）汇总成 25 个候选方向；每个候选交一名没有参与提出的核实员独立读代码核实；再由一名没看过过程的审读员通读，一名修订员按意见修订并复读了一部分行号。全程没有编译，也没有运行测试。
>
> **25 个候选与正文 29 个方向怎么对应。** 按各节内容对照：五个候选各拆成两个（依赖与检查点的可信度、平方根失败与 Couveignes 回退、测试通道覆盖缺口、小规模线性代数与性能门禁、CI 提速与策略检查器），两个候选合并成一个（legacy V0 过滤的 50 位产出、归约路线收敛），其余一一对应。
>
> **称谓。** 文中「上一轮」「原稿」指修订前的初稿，「核实」指核实员的结论，「修订时」指定稿人，〔复读〕是定稿人重新打开核对过的行号，「未确认」是没能核实的内容。下一节说来历交代不全的三处，由本节补上。


## 先说明依据与局限

- 依据是一次调研、对 25 个候选的核实，以及原稿作者对部分关键更正的复核。来历交代不全的有三处：调研「10 个角度」的清单不在本稿里（未确认）；「核实」是谁、用什么方法做的没有记录（未确认）；25 个候选和正文 29 个编号方向怎么对应也没有记录（§三.18 是两条合并，§三.17、§三.22 删去过内容，未确认）。行号基于 `main@d54aba29`；修订时 `git status --short --branch` 输出 `## main...origin/main`，`git rev-parse --short=8 HEAD` 输出 `d54aba29`。
- 全程只读。原稿和本次修订都没有运行任何构建、测试、`./scripts/test.sh` 子命令或工作流；下文凡是写「会通过」「会失败」，都是读码结论，不是执行结果。文中数字的来历分四类：
  - (a) 引自仓库文档里的既有测量。例如 §四.27 的热点画像来自 `docs/perf/structured-ooc-measurement.md:582-585` 记录的一次 15 秒采样，不是为本稿跑的。
  - (b) 原稿作者用 `gh` 只读查到的 CI 运行记录（§二.9），本次修订没有重查。
  - (c) 标「核实」的算术复刻与选择器复刻（§二.1、§二.2），是实际运行还是纸面推导没有记录，未确认。
  - (d) 本次修订自己做的只读核对，只用读文件、`git log`、`grep` 与 `ls` 计数，命令和结果写在对应位置。
- 「25 个候选经核实全部成立」已删去，它与正文冲突：正文里多条上一轮说法被核实判为不成立、因果不成立或言过其实，逐条见各方向的「更正」。「全部未做」没有逐条依据，未确认。远端只有 main、`gh pr list --repo MaYiding/GNFS --state open` 无输出，是原稿作者的查询，本次修订没有重跑。
- 行号的可信度。原稿有两句互相矛盾的话（一说未标注的行号都亲自读过，一说只复核了关键更正），无法逐条判定，所以改按本次修订的事实标注。一条引用或一段末尾标〔复读〕，表示它里面不带「引核实」的行号本次修订打开核对过，代码与正文描述一致；〔复读〕只保证行号和读码可见的事实，不保证标「核实」的推论。没有〔复读〕的行号本次没有复读，按「出自调研或核实材料、未经第三方重读」对待。复读中发现并更正了四处，见 §三.18、§四.25、§七。
- 原稿作者在上一稿亲自复核的 12 项及所在位置：`norm_tile.hpp` 的实际内容（§四.26）、`ECM::auto_tune` 表（§四.27）、50 位余因子热点画像（§四.27）、生产流水线与 RegressionGate 的裁剪（§二.6）、`test_factor_with_kleinjung` 的 `main`（§二.6）、120 bit 门禁的固定身份断言（§二.2、§三.18）、Schirokauer 的 split 路径与非首一来源（§二.1）、空输入返回 1 的测试契约（§二.5）、收集器默认放行的测试（§三.11）、资格工作流的触发限制（§三.17）、策略脚本的禁止标识符（§三.21）、CI 各 job 与 step 时长及 macOS 行缓存命中后的编译计数（§二.9）。前 11 项的行号本次修订也复读了，第 12 项没有。

### 阅读约定

**单位。** 十进制位数写「十进制 N 位」，二进制写 bit。原稿混用的地方已按代码逐处改正。两处容易弄反：120 bit 门禁是二进制（`tests/test_structured_filter_pipeline_120bit.cpp:33` 的 `FIXTURE_BITS = 120`，对应十进制 37 位）；§四.28 的筛区域分档是十进制位数（`include/gnfs/core/params.hpp:277-297` 按 `p.digits` 分档）。「盈亏」指矩阵行数减列数，正数叫盈余。

**术语**（正文里的自造说法，各只解释一次）。

- 排干：`submit` 抛出后，先对已经提交的任务逐个 `get` 等它们结束，再向外抛。
- 留痕：失败时写一行日志或计数，事后能查到。
- 收口：以 $f_1 f_2=N$ 且两个因子都非平凡作为分解成立的收尾判定（`src/api/pipeline.cpp:3892-3917`）。
- 钉住、钉死：某个脚本对一段文本或一个函数做逐字或哈希校验，改这段内容必须同步改脚本。
- 硬锁：某个值（如版本号 `v0.1.0`）被直接写在脚本的判断里。
- 回填：把后来的实测结果写回设计文档。
- 收起：不再投入、不接入生产，代码留着。
- 并设：同时设置分布式筛（`GNFS_DISTRIBUTED_SIEVE_WORKERS`）与续跑（`GNFS_RESUME` 或 `GNFS_SIEVE_RESUME`）。
- 台账：对环境变量开关的清单及其文档记载。
- 退化有三种用法，各处单独解释：退化行（§二.1）、退化坐标（§三.11）、退化为首一（§三.12）。
- 身份有五种用法：矩阵身份（矩阵的行数、列数、非零元数与摘要）、sieve run identity（断点续跑时核对的一次筛分运行的身份，§三.18）、V1/V2 工作身份（§三.21）、身份字段（§三.22，未确认）、固定身份门禁（把期望身份写死在测试里的门禁，§二.2）。
- 只出现一次的说法在出现处解释：症状级优化（§二.6）、可注入分配（§三.10）、休眠设施（§三.22）、语义键与外层批宽（§四.28）。

**CI 词汇**（修订时读 `.github/workflows/` 下相关文件）。

- 工作流 `CI`（`ci.yml`）有四个 job。`build-and-test` 是 6 行矩阵，每行称一「行」（如 Windows 行、macOS 行），按 `test_labels` 跑 `instant`、`fast` 标签的测试并排除 `slow|heavy|stress|bench|gate`（`:196-201`）。`linux-release-gate` 的显示名是 Linux x86_64 Release deep gate，即「deep gate」，跑 `gate|slow` 标签（`:320-328`）。`linux-musl-transport` 是 Alpine musl 通道。`required` 的显示名是 `CI required`，等前三个 job 都成功（`:373-395`）。
- 「定向步骤」指绕过标签、按名字直接跑某个测试二进制的步骤（macOS 四个，`:203-253`；Windows 一个，`:275-290`）。「Apple 专属的 slow 条目」指只在 `if(APPLE)` 下注册、标签为 slow 的测试。
- instant、fast、gate、slow、heavy、bench、stress 是 CTest 标签，定义在 `docs/testing-ci-policy.md:36-44`。「Sanitizers 的 ASan+UBSan 道」是 `Sanitizers` 工作流里名为 AddressSanitizer + UBSanitizer 的 job，跑 instant 标签。
- 「必需检查」指 `scripts/release_contract.py:138-177` 的 `REQUIRED_MAIN_CHECKS` 表，共 12 个 job：`CI required`、Sanitizers 两个、CodeQL 的 Analyze C++、Static Analysis 两个、Script Checks、Workflow Security 两个、Workbench CI、Release Readiness 两个。Docs Integrity 和 Benchmarks 不在表里。
- 「资格工作流」是 `Release Qualification`（`release-qualification.yml`），只能被别的工作流调用（`:4`）。调用它的有两条：「Nightly Deep Tests」（`nightly.yml`，每周五 17:00 UTC 或手动触发）和「Release Artifacts」（`release.yml`）在 verify-only 模式下的发布预检。「发布预检」指 Release Artifacts 在 verify-only 模式下的 preflight 与 qualification 两个 job。
- 「两条资格通道」指资格工作流里除全量回归外的两个 job：Structured 120-bit route gate（跑 120 bit 门禁）和 Bounded 50-digit route comparison（跑 `compare-50d-bounded-routes`），见 `release-qualification.yml:50-120`。
- 「必需上下文」是发布预检要求目标提交上已经通过的检查；§三.16 说 Release Readiness 是其中之一（引核实）。发布预检怎样逐个核对那张表，本次修订没有读，未确认。

**求解路径**（修订时读 `src/api/pipeline.cpp:3543-3783` 与 `src/linalg/block_lanczos.cpp:373-426`）。

- SGE 之后行数不超过列数的「瘦矩阵」直接走 Block Wiedemann（`:3714-3726`、`:3730-3783`）。
- 其余矩阵先交给 `BlockLanczos::find_dependencies`。这个类名还在，Montgomery 的 Block Lanczos 算法已经移除（`block_lanczos.cpp:100-103` 的注释）。它内部按增广矩阵 $m\times(m+n)$ 的体积分流：不超过 4 GiB 走高斯消元（`find_dependencies_sparse`，`:410-420`；§二.3、§二.4 说的「高斯路径」就是它），超过则走 Block Wiedemann（`:423-425`）。
- 高斯路径没找到依赖时，流水线再落到流式 Block Wiedemann，可经 mmap。
- §三.10 的「BL 检查点」是高斯消元中途的检查点（`GNFS_BL_CHECKPOINT`，`block_lanczos.cpp:138-160`、`:303-325`）。
- 所以「BlockWiedemann 通过」只覆盖 Block Wiedemann 自己的代码和瘦矩阵情形，覆盖不到高斯路径。高斯路径靠走完整流水线、矩阵有盈余的测试（RegressionGate、GNFS_E2E、FactorWithKleinjung）或专门的 `test_block_lanczos*`；各测试是否真走高斯路径取决于各自矩阵的形状，本次修订没有逐个核对。`tests/test_block_wiedemann.cpp:609` 也调用了 `BlockLanczos`，用途本次修订没有读，未确认。
- §四.24 的「30 秒的 SIQS 探测」和 §三.13 的「SIQS 分支超时下限 60 秒」是同一个 `if` 里的两个分支（`src/api/pipeline.cpp:4322-4356`）。方法本身是 SIQS（含自动选路选到 SIQS）时，超时按十进制位数取 60 秒到 7200 秒；方法被强制为 GNFS 但位数不超过十进制 100 位时，先做一次 SIQS 探测，十进制 50 位以内给 30 秒。

## 2026-11-27 增补

> 本节补记 2026-11-27 第一波合入 main 的改动和随后的核实。正文其余部分仍是 2026-11-26 的定稿，只改动了一处：§六「构建缓存命中后仍重编的原因没有调查」一条的末尾加了一句指向本节。§二.9 与「你可能会问」第 10、11 问仍写着原因没有调查，那是定稿时的状态；本节记了之后查到的证据，原因本身是推断。
>
> **依据。** 本节没有编译，也没有运行测试。PR 编号与标题用 `gh pr view` 逐个核对；耗时取自 `gh api repos/MaYiding/GNFS/actions/runs/<id>/jobs`，编译计数取自 `gh run view <run> --job <job> --log`，文件数取自 `git ls-tree`；`CMakeLists.txt` 的条件读的是 `e2675a09` 上的版本。「已核实的线索」里引用的函数与类型，写本节时都重新打开读过。

### 第一梯队：九个方向已合入

九个方向都经集成 PR #423 合入 main，合并提交是 `86d7005c`。

| 方向 | PR | 改了什么 |
|---|---|---|
| 1 | #415 | Schirokauer 模多项式在归一化后才用于约化，并统计退化行 |
| 2 | #416 | 特征素数选在因子基、special-q 区间与大素数界之上 |
| 3 | #417 | 高斯消元与 SpMV 转置在 `submit` 抛出时排干已提交任务 |
| 4、5 | #418 | 矩阵阶段对未通过校验的依赖留痕并剔除；平方根失败按依赖写日志 |
| 6 | #419 | `test_factor_with_kleinjung` 求解前裁剪，并做 Release 下生效的断言 |
| 7、9 | #420 | macOS 的 CI 行跑三个 WaveMergeCommit 分片；构建目录缓存换成 ccache，Windows 行缓存 vcpkg 二进制包 |
| 8 | #421、#422 | 开关台账补齐并加防回归检查；链接检查器加锚点校验并修掉悬空锚点 |

八个 PR 都只经 #423 进入 main：各自的合并提交都在集成分支里，没有一个单独合并到 main。§七 建议方向 2 单独合并，这一点没有照做。#416 的说明写明它不单独合并：120 bit 门禁的矩阵身份随特征素数改变，新基线在集成分支上采集。

### 已核实的线索

线索由一名核实员读码、一名复核员换人复核，全程没有编译。第一条的裁决范围写本节时按代码收窄，第二个缺口换成了 PR #415 说明的写法，出处写在各条里。

- §六「分歧与指标素数只按范数赋值记一列」对单根不成立。$f\bmod p$ 的每个根是一个因子基条目（`src/factor_base/builder.cpp`）；因子基列与大素数列都按 $(p,r)$ 键建，大素数列用的是 `include/gnfs/linalg/matrix_builder.hpp` 的 `PrimeIdealKey`。
- 它对投影根与重根成立，这两处是还没有解决的缺口。整除首项系数的素数各只有一个投影根条目，匹配条件是 $p\mid b$（`src/factor_base/builder.cpp`）。试除把范数的整个 $p$ 进赋值记给匹配到的那个条目（`include/gnfs/cofactor/trial_division.hpp`）；大素数遇到 $p\mid b$ 时根也记为投影根（`include/gnfs/cofactor/cofactorizer.hpp`）。求根只返回不同的根，重根因此只有一个条目（`src/factor_base/builder.cpp`）。
- 由此带出两个约束缺口。一是首项系数不是平方数时，依赖的对数奇偶没有约束；对数指依赖里 $(a,b)$ 对的个数。二是首项系数的素因子上方，范数的整个 $p$ 进赋值进同一个条目，一部分依赖因此不是平方。另外，首项系数为偶数时，Schirokauer 列成了非同态的噪声约束：`include/gnfs/linalg/schirokauer.hpp` 的归一化要乘首项系数模 $\ell^k$ 的逆，偶数首项系数没有逆，这一支保留旧行为。PR #415 的说明把这三处列为没有覆盖；重根的情形它没有提到。
- 三者都只降低成功率，不会算出错误因子。Hensel 与 Couveignes 的返回值都做了 $Y$ 的平方的校验：`include/gnfs/sqrt/hensel_sqrt.hpp` 的 `verify_and_return`、`include/gnfs/sqrt/couveignes.hpp` 的 `verify_current`。流水线按两个因子都非平凡且乘积等于 $N$ 收口（`src/api/pipeline.cpp` 的 `try_factor`）。

### 更正 §二.9 与 §六

- 在 main 的 run 45967343806（提交 `e2675a09`）里，构建目录缓存没有省过一次编译，问题不在缓存键：这一 run 里 §二.9 待查的「缓存不命中」没有出现。只查了这一次 run。「从来没有省过一次」是 PR #420 说明的结论，本节没有查别的 run，也没有查 PR #420 之后的 run。五个 POSIX 行的缓存键都精确命中，每行仍把自己适用的源文件全部编译了一遍。四个 Linux 行各编译 338 个源文件、345 个对象。macOS 行编译 336 个 `.cpp` 加 `src/linalg/metal_spmv.mm`，共 344 个对象。每行对象比源文件多出的 7 个，来自编进多个目标的三个源文件：`src/relation/structured_reduction.cpp` 编 2 次，`tests/test_siqs_multi_a_cycle_profile.cpp` 编 3 次，`tests/support/siqs_shadow_proof_rss_campaign_slot_synthetic_child.cpp` 编 5 次。
- `e2675a09` 上仓库有 351 个 `.cpp`，四个 Linux 行没有编译的 13 个都不在默认构建里：`bench/microbench` 的 7 个不在构建系统里；4 个 `_windows.cpp` 在 `if(WIN32)` 下；`tests/fuzz/fuzz_integer.cpp` 在默认关闭的 `GNFS_BUILD_FUZZERS` 下；`tests/test_siqs_shadow_proof_rss_holdout_probe.cpp` 标了 `EXCLUDE_FROM_ALL`。macOS 行另有 2 个没有编译：`src/linalg/metal_spmv_stub.cpp` 是非 Metal 分支，`tests/support/bounded_child_process_authenticated_supervisor.cpp` 只在 Linux 下定义。
- 原因是推断，没有实测：没有查看恢复出来的目录里的时间戳，也没有做对照 run。推断的内容是：Make 靠时间戳判断新旧，恢复出来的对象若不比 checkout 的源码和新装的依赖头文件新，就会全部重编。macOS 行这一次的日志里，`brew install gmp ntl` 对 gmp 报已安装，对 ntl 是新装；「每次都重新安装 NTL」同样是推断。缓存已换成 ccache（PR #420）。
- 同一提交上必需检查的耗时（CI 的 run 45967343806、CodeQL 的 run 45967343786）：最长的两个并行，Windows 行 47.6 分钟，`CI required` 等它结束才通过；CodeQL 的 Analyze C++ 45.0 分钟。CodeQL 里带插桩的构建 25 分钟、分析 19 分钟，工作流里没有任何缓存。这 351 个 .cpp 里 272 个是测试。§三.23 所引的 CodeQL 68.2 分钟没有注明 run，与这次的 45.0 分钟不是同一次测量，差别的原因未确认。

## 一、过去的方向脉络

2026 年 3 月至 4 月的提交时间戳是按固定节距回放的，这两个月只有顺序可信。「回放」指这些提交的时间戳不是真实的工作时刻，而是按固定间隔生成的。修订时用 `git log --no-merges --format='%ci'` 按 3 至 5 月分段抽查：2026-04-15 当天依次是 16:18:45、15:30:00、14:41:15、13:52:30、13:03:45，2026-04-22 当天是 22:00:00、21:16:40、20:33:20、19:50:00，间隔固定；2026-05-11 当天是 19:23:15、19:15:49、19:11:25、19:05:09、18:54:14、18:51:12，间隔不规则。因此表内落在 3 至 4 月的行只能按提交顺序看，「4 月做内核性能」这类月内划分的可信度低于 5 月以后按日期的划分。

| 时间 | 做了什么 | 留下了什么 |
|---|---|---|
| 2026-02 | 设计文档：目标分解十进制 150 位以上并用 Metal GPU 加速，估约 1.5 万行（`docs/plans/2026-02-03-gnfs-design.md:15`、`:1262`〔复读〕） | 文档状态仍是「已确认，待实现」（同文件 `:5`）；`include/gnfs` 与 `src` 下没有 metal 模块（修订时 `ls` 确认） |
| 2026-03 至 04 | 按 8 阶段流水线把因子基、余因子、special-q、格筛、ECM、稀疏矩阵、SGE、Schirokauer 列搭齐；用「一次一个 bug」修数学错误；4 月做内核性能（加法筛、桶筛、uint64 快路径），新做出 SIQS 与多方法自动选路（本行「做了什么」的内容没有出处，未确认） | 自动选路在十进制 100 位以内不走 GNFS（`src/api/pipeline.cpp:1209-1212`〔复读〕）；常数范数初值加合并阈值、无 skew 的 LLL 这套筛法内核此后只做过防护类修补（未确认） |
| 2026-05 | 最大的一次扩张（判断，未确认）：补跨平台，写下性能准则并用 PMU/PGO 做闭环，做外存存储与检查点续跑；线代与关系侧加入薄矩阵 Block Wiedemann、mmap、V0_BFS、V3 级联；成批加入默认关闭的并行与 SIMD 辅助组件 | 辅助组件多数没接进生产（未确认）；Montgomery Block Lanczos 被移除，增广矩阵不超过 4 GiB 走高斯消元（`src/linalg/block_lanczos.cpp:100-103`、`:396-425`〔复读〕）；性能准则停在 2026-06-02（所指未确认：正文只在 §二.8、§三.15 提到 2026-06-02 是测试分级与 CI 策略的基线日期） |
| 2026-05 下半月至 06 | 攻十进制 50 位矩阵无盈余：修大素数列估计与裁剪上限，回滚 V2 合并，做 V3 级联与 OOC，扫 lp_bits；CI 矩阵扩到 Windows MSVC 与 Linux ARM64（修订时读 `.github/workflows/ci.yml:61-81`，矩阵里确有这两行），搭出发布工作流 | 十进制 50 位大素数列占比停在约 121%（`docs/perf/performance-doctrine.md:1330` 记第 5 轮 β=121.6%；修订时按表内数字折算，β 是大素数列数 342,856 除以可用关系数 282,027），五轮筛完仍无盈余（同文件 `:1332`）〔复读〕 |
| 2026-07 至 08 月初 | 两个大工程：确定性的结构化关系归约、持久化分布式筛 wave（PR #28 一次并入约 33.8 万行；修订时只读核对：合并提交是 `4c7657d9`，`git diff --shortstat 4c7657d9^1 4c7657d9` 为 379 个文件、新增 338,012 行，`git rev-list --count 4c7657d9^1..4c7657d9` 得 275 个提交，与原稿写的 272 差 3，口径未确认）；8 月 5 日发布 v0.1.0（标签指向 `42dd96f0`，提交日期 2026-08-05） | 结构化归约把十进制 50 位矩阵盈亏从 −20,748 转为 +23,319，但非零元增加 2624%、峰值内存增加 45.67%，判不提升（`docs/perf/structured-ooc-measurement.md:1196-1201`、`docs/plans/2026-07-21-parallel-structured-filter.md:763-764`〔复读〕）；持久化 wave 没接入生产流水线（未确认） |
| 2026-08 至 09 | 主线转到发布与质量工程：发布合同、Windows 运行时闭包、测试分层与 CI 策略；9 月做覆盖全模块的边界审计（131 个 PR 合并；修订时 `git log --merges --since=2026-09-01 --until=2026-10-01 --format=%s` 里「Merge pull request」有 131 个） | 线代此后的提交以加固为主，算法主线没有推进（判断，没有判定标准与检索范围，未确认）；版本停在 v0.1.0（修订时 `git tag -l` 只有这一个标签），其后 1104 个提交没有发布出口（修订时 `git rev-list --count v0.1.0..HEAD` 为 1104） |
| 2026-11 | 正确性与可移植性修复潮、积压 PR 集成、Workbench 重建；平方根侧补了一批边界修复；「SIQS 影子证明及其 RSS 设施成为近期投入的大头」（判断，没有统计口径，未确认）；「11 月 10 日修正 Gray 码筛根方向」（未确认） | 此前的计时、标定和影子证据都被标为需要重测（未确认）；仓库里仍没有一次完整的十进制 50 位 GNFS 分解（否定性结论，检索范围没有给出，未确认；§四.24 重复了这一条） |

表里没有 2026 年 10 月：修订时用 `git rev-list --count` 按月计数，2026-10 只有 1 个提交（`a54faf64`，依赖升级），没有可列的方向。上表原稿几乎没有给出处，能用只读命令核对的已写进表里；标「未确认」的是判断性或否定性说法，没有判定标准、检索范围或出处，保留只为交代脉络，引用前需要重新核实。

贯穿始终没有解决的是规模与接线：默认路径到不了十进制 50 位的求解与平方根，投入最大的几块基础设施默认不生效，开关台账与代码漂移。

## 二、第一梯队：证据成立、风险可控，可以马上开工

标题里的「证据成立」指问题所在的代码位置和触发机制经读码确认（各条「要解决什么」里的行号），不包括实际影响面：§二.1、§二.2 自述影响面与损失没有统计，§二.3 的修复形态和 §二.5 的加统计没有定。

### 1. Schirokauer 列：非首一多项式归一化

- **要解决什么**：Schirokauer 列是矩阵里每个素数 $\ell$ 各 $\deg f$ 列的附加约束，让依赖对应的代数乘积成为 $\ell$ 次幂；对元素 $\gamma=a-b\alpha$ 算 $\lambda_\ell(\gamma)=(\gamma^{\ell^d-1}-1)/\ell\bmod\ell$，$d=\deg f$（`include/gnfs/linalg/schirokauer.hpp:664-674` 的类注释）。`SchirokaurMap` 有两条计算路径：$f\bmod\ell$ 不可约时走 unsplit，直接在 $(f,\ell^k)$ 下算 $\gamma$ 的幂；可约时走 split，把 $f\bmod\ell$ 分解成因子，在每个 $(f_i,\ell^k)$ 下算。$k$ 就是 `exponent_k`，见 §五第 15 项。
  问题有两处。unsplit 把未归一化的 $f\bmod\ell^k$ 交给假定首一的 `reduce_inplace`（`include/gnfs/linalg/schirokauer.hpp:603-624`、`:790-791`、`:1021-1032`）。split 路径里二次及以上的因子经 `:910-911`、`:951-952` 进入 `FastPoly::power`（`FastPoly` 是这个头文件里的小型定长多项式类，`power` 是模 $(f,\ell^k)$ 的快速幂，`:643-661`），核实的算术复刻显示最高次因子的首项系数被提升成了 $a_d$，即等于 $a_d$，不是 $a_d$ 次幂（修订时读 Hensel 提升的配对实现 `:428-489`，修正量只加在后一半因子上，前一半的首项不动，所以取这个读法）。据该复刻，列值只在 $a_d\equiv 3\pmod 4$ 时出错；$a_d\equiv 1\pmod 4$ 时列值相同，但中间系数仍不对。模 4 与列值的取法有关：列值取的是结果多项式每个系数的第 2 低二进制位（`:793-800` 的 `(coeff / ell) % ell`，$\ell=2$），这是修订时读码看到的事实；「所以只在 $a_d\equiv 3\pmod 4$ 时出错」的推导过程，以及复刻覆盖了哪些次数、哪些模数、是穷举还是抽样，本稿都没有记录，未确认，读者应把它当作一次观察而不是证明。
  非首一不只在超过 400 bit 的 Kleinjung 路径。次数小于 5 不走 Kleinjung（`include/gnfs/polynomial/selector_dispatch.hpp:58-65`），而次数不超过 400 bit 时至多是 4（`include/gnfs/core/params.hpp:122-125`），所以这一段走默认 Base-m 路径，首项系数取 base-m 展开的剩余部分（`src/polynomial/base_m.cpp:30-31`）；`tests/test_integration.cpp:489-494` 记录了 N=143 得到首项系数为偶数的多项式，测试是靠换输入绕开的。〔复读〕
- **改哪里**：`include/gnfs/linalg/schirokauer.hpp`，在构造 `PrimeInfo` 处把 $f$ 归一化后再用（核实建议一处同时修两条路径）；`include/gnfs/linalg/matrix_builder.hpp:363-379` 加退化行计数。退化行指 Schirokauer 列因退化分支被整体置 0 的矩阵行：`compute_unsplit` 在 $\gamma\equiv 0\pmod{\ell^k}$ 或剥去 $\ell$ 因子后精度不足时返回全 0（`schirokauer.hpp:772-786`），split 路径同理置 0。〔复读〕
- **怎么验证**：新增 instant 用例必须和独立参照比较（`sqrt::ModularPoly` 的求幂与约化，或复用 `tests/test_linalg.cpp:1343-1365` 的检查，引核实），多项式取 $a_d\equiv 3\pmod 4$。照现有「同一个模多项式下自洽」的写法不会失败（引文出处未确认）。用例与修复放同一个 PR，先红后绿。另加一个用非首一多项式跑到平方根并断言因子的用例：RegressionGate、GNFS_E2E、FactorWithKleinjung 都不覆盖非首一。通道是 CI 的 instant 行、Sanitizers 的 ASan+UBSan 道、deep gate 的 RegressionGate 与 GNFS_E2E。
- **风险**：实际影响面未知，默认路径上非首一出现的频率与退化发生率都没有统计。偶数首项系数那一支（`include/gnfs/sqrt/modular_poly.hpp:406-409` 对首项模 $\ell$ 为 0 直接返回「不可约」为假，`include/gnfs/linalg/schirokauer.hpp:1070-1079` 回退 unsplit）只有代码推演。首项系数被 2 整除时，归一化要除以它，而它在模 $2^k$ 下不可逆，所以「归一化」这条修法覆盖不到这一支（修订者的说明）；除此之外还有哪些处理办法，正文没有列，未确认。〔复读〕

### 2. 二次特征素数避开大素数与 special-q 区间

- **要解决什么**：二次特征素数（代码里叫 QC prime，默认 10 个，`include/gnfs/linalg/matrix_builder.hpp:153`）是另一族附加列所用的素数 $q$：对 $f\bmod q$ 的每个根 $r$ 记一列，值是勒让德符号 $(\frac{a-br}{q})$ 是否为 $-1$，$a$、$b$ 是关系的坐标（`:1028-1045`）。它和 Schirokauer 列不是同一样东西（Schirokauer 列从 `schirokauer_start()` 起，`:374-379`）。问题是 $(a-b\cdot r)\bmod q=0$ 时这条特征列被记成 0（`:1036-1045`），而选素数只保证 $q$ 大于代数因子基界（`:240-243`、`:664`）。special-q 从 $B_{alg}+1$ 枚举到 $10B_{alg}$，大素数上界是 $2^{lp\_bits}$（`lp_bits` 是大素数的 bit 数，`include/gnfs/core/params.hpp:266-267`、`:344-346`），special-q 素理想被记为关系的代数大素数（`include/gnfs/cofactor/cofactorizer.hpp:444-451`），于是特征素数会落进 special-q 的区间。后果按 `:661-663` 的注释是「污染 GF(2) 约束」，即找到的依赖不保证对应平方，不是只浪费一列。核实按同一逻辑复刻选择器：120 bit 门禁用的 32 个 special-q 素理想全部与某条特征列相同，即它们的素数 $q$ 与根 $r$ 恰好等于某条特征列的 $(q,r)$（这个读法是修订者按上下文补的；复刻是否实际运行，未确认）。〔复读〕
- **改哪里**：`include/gnfs/linalg/matrix_builder.hpp` 里 `select_qc_prime_roots` 的下界，两个调用点 `:240-243` 与 `:444-446`；`compute_legendre_bit` 加计数。〔复读〕
- **怎么验证**：在 `test_linalg`（instant）加用例，经 `MatrixBuildResult::mapping.qc_prime_roots` 断言，因为选择函数是私有成员。deep gate 的 RegressionGate、GNFS_E2E 仍得到正确因子。120 bit 门禁硬编码了矩阵非零元数与摘要（`tests/test_structured_filter_pipeline_120bit.cpp:37-74`，断言在 `:300-321`），特征素数一换必然失败，要重新采集基线；该门禁只在两条资格通道里跑。〔复读〕
- **风险**：固定身份门禁要重定基线（重定基线指改掉测试里写死的期望值并重新采集；具体要做哪些事，本稿没有展开，未确认）。特征素数变大后选择器的耗时，在 ASan 加 Debug、10 秒超时下没有测量。实际损失（多少依赖因此失败）没有统计。两条资格通道都不产生依赖数，上一轮写的「比较依赖数」做不到，已删去。

### 3. 手写并行提交循环在 submit 失败时的悬垂引用

- **要解决什么**：并行高斯消元（`src/linalg/block_lanczos.cpp:255-279`）和 SpMV 转置（`include/gnfs/linalg/detail/spmv_kernels.hpp:252-290`）逐个 `submit` 再逐个 `get`，提交抛出时不排干。可达的触发只有 `submit` 内部的分配失败；「已停止的线程池」在这两处不可达，因为 `stop_` 只在构造的 catch 和析构里置位（`include/gnfs/util/thread_pool.hpp:90-118`）。Block Wiedemann 的标量路径与瘦矩阵路径声明顺序相同（`src/linalg/block_wiedemann.cpp:629`、`:632`、`:1230`、`:1233`，引核实；修订时复读这四行，分别是线程池对象与块向量 `X`、`Y` 的声明，该文件里没有 `submit` 或 `futures` 字样）。这两处与上面两处是不是同一个缺陷、要不要一起修，原稿没有说，未确认。〔复读〕
- **改哪里**：上述两处生产路径。同类问题还有 `include/gnfs/sqrt/couveignes_parallel.hpp`（线程池在 `:219` 先于 `:227-228` 的两个原子量声明，`:285-287` 不排干，worker 抛异常也会触发），它是只被测试编译的 helper，去留归第二梯队「裁决 helper 头文件」。〔复读〕
- **怎么验证**：先定注入手段（让测试能在第 N 次 `submit` 时抛异常的办法）。`FutureLaunchGuard` 是私有嵌套类（`include/gnfs/util/thread_pool.hpp:300`；它的析构会对已接收的 future 逐个 `get`，`:304-330`），现有提交失败用例的失败点在迭代器的 `operator+=`（`tests/test_thread_pool.cpp:501-509`），不在 `submit` 内部。仓库先例是在测试二进制里替换 `operator new` 并做序号扫描（`tests/test_factor_base_sieve_launch_failure.cpp:16-45`，引核实）；转置路径可用自定义 `MatrixView` 让先提交的任务停住（`tests/test_spmv_simd.cpp:499-587`，引核实）。通道是 CI 的 instant 行、Sanitizers 的 ASan+UBSan 道、fast 行的 BlockWiedemann、deep gate 的 RegressionGate 与 GNFS_E2E。〔复读，`tests/test_spmv_simd.cpp:499-587` 除外〕
- **风险**：修复形态要先定。两个选项：公开 launch guard（把 `thread_pool.hpp:304` 的 `FutureLaunchGuard` 开放给这两处调用方使用），或新增批量提交接口（线程池增加一次提交一批任务的成员函数，在内部排干）。高斯循环的并行分支要求行数超过 2000（`block_lanczos.cpp:191`）且待消元行数过阈值（`:249-252`），任务体是生产代码，很难确定性复现。〔复读〕

### 4. 依赖校验留痕：高斯路径回代校验与已校验依赖数

- **要解决什么**：高斯路径把右半为零的行直接当依赖返回（`src/linalg/block_lanczos.cpp:334-348`；增广矩阵是 $[I_m\mid A]$，右半即 $A$ 那一半被消成全 0 的行，左半就是一个依赖），流水线到提取因子时才校验，失败不留痕，`dependencies_found` 仍是未校验的数量（`src/api/pipeline.cpp:3792`、`:3960-3964`）。标题里的「回代」指把依赖代回矩阵做校验（`verify_dependency`），不是消元法里的回代求解。对精确行变换来说这一步在数学上冗余，价值在于损坏恢复时可诊断：损坏的依赖在 `:3962` 被静默丢弃，事后无从取证。这里的「损坏」指哪一种（内存、检查点还是磁盘上的数据），依赖损坏是否实际发生过、`:3962` 的静默丢弃是否被触发过，本稿都没有实例，未确认。〔复读〕
- **改哪里**：校验点放在依赖展开之后、统计赋值之前（`src/api/pipeline.cpp:3789-3792`）；`verify_dependency` 失败时写日志。〔复读〕
- **怎么验证**：BlockWiedemann（fast）与 deep gate 的 RegressionGate、GNFS_E2E、FactorWithKleinjung 通过（这些能否覆盖高斯路径，见「阅读约定·求解路径」）；新增一个 instant 用例，注入一条坏依赖，断言统计只计有效依赖并留下日志。
- **风险**：低。这一项与上一轮「依赖与检查点的可信度」拆开，检查点那一半要改磁盘格式，放进了第二梯队。

### 5. 平方根失败可观测性：日志

- **要解决什么**：代数平方根失败时流水线用 1 代替继续求 gcd，错误原因被丢弃（`src/api/pipeline.cpp:3947-3948`）。这不是正确性漏洞，因子仍经 $f_1 f_2=N$ 收口（`:3892-3917`），是可观测性问题。〔复读〕
- **改哪里**：`src/api/pipeline.cpp:3938-3958` 加 `emit_log`。加统计要改 `include/gnfs/api/result.hpp` 及其输出格式，属于公共结构体扩展，做不做由你定（已列入 §五第 20 项）。〔复读〕
- **怎么验证**：流水线侧日志沿用 `test_api` 的 `set_log_callback` 写法（API 是 slow，CI deep gate）。三个端到端测试（GNFS_E2E、FactorWithKleinjung、RegressionGate，按本稿 §二.1、§二.4 的用法）都用自己的 harness 直接调 `compute_algebraic_sqrt`，验证不到流水线侧的改动。
- **风险**：低。两件事不在这项里。一是「`HenselSqrt::compute` 空输入返回 1」：这是测试锁定的契约（`tests/test_sqrt.cpp:1815-1819`），且生产路径在调用前已拦截空输入（`include/gnfs/sqrt/algebraic_sqrt.hpp:142-145`）。二是 `compute_f_lead_inv` 的失败分支：首项系数模模数不可逆时返回 1（`include/gnfs/sqrt/hensel_sqrt.hpp:1547-1552`），核实认为按调用链不可达（引核实）。〔复读〕

### 6. 裁剪 test_factor_with_kleinjung 的超定矩阵

- **要解决什么**：生产流水线早已在求解前裁剪（行数超过列数 1.3 倍时裁到 1.1 倍，`src/api/pipeline.cpp:3562-3573`）并做 SGE，RegressionGate 也早已裁剪并走 SGE（`tests/test_regression_gate.cpp:257-271`、`:285`）。只有 `test_factor_with_kleinjung` 把过滤后的全部关系直接建矩阵求解（`tests/test_factor_with_kleinjung.cpp:207-213`、`:309`），2026-05 插桩记录的形状是 105095×5582、100813×7895、35928×6517（`bench/results/2026-05-14-gaussian-threadpool.md:31-33`）。这三组形状对应哪三个输入，记录里没有写，未确认；它们是 2026-05 的记录，是否还能代表当前形状也没有重测。〔复读〕
- **改哪里**：`tests/test_factor_with_kleinjung.cpp`，照 RegressionGate 的写法裁剪。
- **怎么验证**：FactorWithKleinjung（slow，TIMEOUT 900，CI deep gate）。它的 `main` 恒返回 0、失败只打印（`:547-550`、`:586`），CI 退出码证明不了因子正确，要补 Release 下仍生效的检查，或者人工看日志里的 `Verified` 行（`:540`）。〔复读〕
- **风险**：低。高斯并行阈值、预取、线程池自旋这类症状级优化已在 main 上做过（引核实），不重复。症状级优化指针对「高斯求解慢」这个现象做的调优，不是针对超定矩阵这个成因。裁剪能省多少 CI 时间没有估计；「收益只落在这个测试自己的 CI 耗时上，不是产品性能」只是定性说法，没有数字。

### 7. WaveMergeCommit 三个 macOS slow 分片接入 CI 定向步骤

- **要解决什么**：三个分片只在 Apple 注册且标 slow（`CMakeLists.txt:1334-1355`），macOS 行排除 slow（`.github/workflows/ci.yml:198`），四个定向步骤不含它（`:203-253`）。Apple 专属的 slow 条目里只有这一个二进制没有任何 CI 通道（引核实）。〔复读〕
- **改哪里**：`.github/workflows/ci.yml` 的 macOS 步骤。新步骤只能放在第 229 行之前或第 255 行之后：策略脚本对第 229 至 242 行、第 242 至 255 行做去空白 SHA-256 校验（`scripts/check_distributed_sieve_policy.py:4218-4221`、`:4674-4677`），放错位置会让 DistributedSievePolicyInventory 和 Script Checks 变红（前者是 CTest 里注册的检查，后者是工作流，两者都会跑这个策略脚本，`scripts/check_harness.py:206-210`、`:303-310`）。〔复读〕
- **怎么验证**：CI 的 macOS 作业日志出现该二进制的通过记录；`CI required` 保持绿。
- **风险**：低到中。三个分片在 macos-15 runner 上能否通过只能看 PR 的 CI。缓存 key 含 ci.yml 哈希（`.github/workflows/ci.yml:92`），改完首跑各行缓存失效。

### 8. 开关台账与文档对齐

- **要解决什么**：
  - 14 处链接指向 CLAUDE.md 已不存在的三个锚点（`CLAUDE.md:1-5` 只剩转引），而链接检查器丢弃 fragment（`scripts/ci/check_markdown_links.py:90-93`）。
  - 代码读取而开关文档与两个 README 都没出现的名字。两个 README 指仓库根目录的 `README.md` 与 `README-EN.md`（修订时按仓库文件列表点名，原稿没有点名）。按 `getenv` 字面量口径是 25 个，这个口径漏了经 `enabled()` 读取的 `GNFS_3LP`（`include/gnfs/relation/filter.hpp:46-62`）和 5 个带后缀的故障注入钩子，真实数不少于 26（按字面只能读成 25 加 `GNFS_3LP` 得 26；5 个带后缀的钩子是否已含在这 25 个里，未确认）。其中完全没有任何 Markdown 提及的只有 10 个（引核实）。两个数是两群名字：不少于 26 是「代码读取、但开关文档与两个 README 都没出现」的，10 是其中「任何 Markdown 都没出现」的（修订者按句子结构这样理解）。
  - README 与代码的真实出入：`README.md:490` 写所有实验性策略默认 OFF，但 V0_BFS 策略在 lp_bits 不小于 22 时默认开启（`include/gnfs/relation/v0_bfs_policy.hpp:56-61`）；`README.md:497` 写 lp_bits 不超过 20 时回退，代码在 lp_bits 等于 21 时也回退。
  - README 分级表（`README.md:541-547`）等于 2026-06-02 的基线（`docs/testing-ci-policy.md:7`），相对现行测试清单已过期。原稿把这份清单写作「目录」，指 AGENTS.md 指定的 `./scripts/test.sh list`；现行数量本稿没有抄录。〔复读〕
- **更正上一轮**：「README 对 V0_BFS 的描述与主流程不符」不准确。`README.md:412`、`:497` 的描述对独立入口 `Pipeline::filter()` 成立（`src/api/pipeline.cpp:3162-3180`），问题在于文档把它写成对 CLI 有效，而 `run()` 与 CLI 不调用这个入口。〔复读〕
- **改哪里**：上述文档；`scripts/ci/check_markdown_links.py` 加锚点校验和 self-test；以及一张汇总上述开关的总览表（放哪里见风险与 §五第 18 项）。
- **怎么验证**：Docs Integrity 通过，加锚点校验后仍通过。Script Checks 对这个改动没有覆盖力（检查器不在 py_compile 清单里，也没有 self-test，引核实），要一并纳入。仓库里没有比对代码与开关文档的脚本，「未记载开关降为 0」之后需要新增一条检查才防得住回归。
- **风险**：低。总览表的位置受约束：`scripts/check_harness.py:110-119` 限制 AGENTS.md 不超过 200 行、CLAUDE.md 不超过 20 行，推荐放在 `docs/env-flags/README.md`。测试钩子标「（测试）」已有先例（`README.md:512`，引核实）。〔复读〕

### 9. CI：缓存 vcpkg 并排查构建缓存命中后仍全量重编

- **要解决什么**：main@d54aba29 的 CI run 45637781717（原稿作者用 `gh api repos/MaYiding/GNFS/actions/runs/45637781717/jobs` 查询，本次修订没有重查）：Windows 行 47 分 58 秒，其中 vcpkg 安装 13 分 09 秒、构建 33 分 08 秒、测试 34 秒；`CI required` 在它结束后 2 秒才启动，它是 `CI` 工作流的关键路径。构建缓存：macOS 行日志有 `Cache hit for restore-key`、`Cache Size: ~106 MB`、`Cache restored successfully`，而这次 main 相对上一次只改了 2 个文件、没有头文件（`git diff --stat 9895978a..d54aba29`），构建仍输出 344 条 `Building CXX object`（原稿作者用 `gh run view 45637781717 --job 139287785922 --log` 统计）。对象总数没有给出，所以只能确定「仍在大量编译」，是不是全量未确认。`Cache hit for restore-key` 是前缀命中，不是精确命中：`ci.yml:93-94` 的 `restore-keys` 只按前缀匹配，键的后半段（`CMakeLists.txt`、`include/**`、`src/**` 的哈希，`:92`）可以不同。〔复读〕
- **改哪里**：`.github/workflows/ci.yml`。缓存路径只有 `build`（`:88-94`），vcpkg 检出与安装没有缓存（`:112-151`）。〔复读〕
- **怎么验证**：改完后 Windows 行的 step 时长下降；缓存命中后编译计数下降；`CI required` 保持绿。策略脚本钉住的两段不覆盖缓存与 vcpkg 部分（引核实），不用同步改。
- **风险**：低到中。vcpkg 缓存的体积与恢复耗时要试；缓存不命中的原因没有调查。原稿还有一句「重复的 Release 全量构建不是 CI 时长的主因：配置相同的 deep gate 构建约 14 分钟，且与矩阵行并行」（约 14 分钟出自哪次 run、哪个作业，未确认）。这句只就 `CI` 工作流内部而言：deep gate 的构建与矩阵行并行，不拉长 Windows 行；它排除不了 Windows 行 33 分钟的构建本身是这个工作流关键路径的主要部分。§三.23 讲的是 main 上全部必需检查的口径，见那里。

## 三、第二梯队：有价值，但要你先拍板

### 10. BL 检查点强校验与原子保存

- **现状**：载荷校验是逐字异或折叠（`include/gnfs/linalg/bl_checkpoint.hpp:50`），盲区是字的任意置换；保存用 `ios::trunc` 原地截断（`:88`）。「保存失败丢旧检查点」是已被接受的行为：现有流失败测试把「保存失败后旧文件不可读」也判为通过（`tests/test_bl_checkpoint_stream_failure.cpp:213-222`），所以它不能当新性质的证据，要收紧。〔复读〕
- **不要重做**：矩阵身份绑定（检查点记录源矩阵的 SHA-256 指纹，加载时比对，`src/linalg/block_lanczos.cpp:148-160`）、不可达游标拒绝、分配失败用例、fresh-run 一致性已在 main（提交 872d9605、db55cd78，引核实）。
- **怎么验证**：BlockLanczosCheckpoint（instant，10 秒）、BlockLanczosCheckpointStreamFailure（fast，60 秒；「可注入分配」指 `save` 或 `exists` 路径上有能被测试替换的 `operator new` 捕获的 C++ 分配点，没有的标准库上打印 SKIP 跳过，`tests/test_bl_checkpoint_stream_failure.cpp:241`）、BlockLanczosResumeIntegration（fast，60 秒）；两个新用例（是哪两个，正文没有写，未确认）。改格式要同步改硬编码夹具与 `docs/env-flags/linalg.md:69-75`（引核实）。〔复读〕
- **风险**：这个检查点是手动开关（`GNFS_BL_CHECKPOINT`），回归测试对它零覆盖。修订时 `grep -rn GNFS_BL_CHECKPOINT tests/ .github/ scripts/test.sh`，只有 `tests/test_bl_resume_integration.cpp` 设置它，RegressionGate 不设置；所以高斯主循环里保存与加载检查点的分支（`block_lanczos.cpp:138-177`、`:303-325`）不被 RegressionGate 覆盖，覆盖它的是上面三个专门的检查点测试。
- **要你定**：是否改磁盘格式并升版本；原子保存要改提交协议，会多出一个同级临时文件；校验用对顺序敏感的 FNV-1a 还是 SHA-256，性能都没有实测。

### 11. 收集器的 gcd(a−bm, N)>1 检查改为构造期保证

- **更正**：默认放行是有意设计，并被测试钉住（`tests/test_relation_collector.cpp:973-982` 断言未设上下文的收集器接受退化坐标；退化坐标指使 $\gcd(a-bm,N)>1$ 的 $(a,b)$，用例里是 $m=12$、$(a,b)=(12,1)$，$a-bm=0$）〔复读〕。实际暴露面小：Cofactorizer 在三条路径上都拒绝有理值与 N 不互素的候选，`load()` 与 `merge()` 在非测试代码里没有调用者（引核实）。今天没有可达的未受保护生产路径，这项实质是公共头文件接口加固，价值比上一轮写的低。
- **改动面**：构造函数必填要改 130 处测试声明，分布在 16 个文件，其中只有 8 处调用了 `set_polynomial_context`（引核实的统计）。
- **怎么验证**：RelationCollector（instant）、SieveIntegration（fast）、GNFS_E2E、RegressionGate；后三个测试自己就不设上下文，改动后必须同步修改。
- **要你定**：构造函数必填还是保留 setter；未设置时怎么拒绝（`add` 返回 false 等于静默丢关系）；N 与 m 是否按值保存；从既有 OOC 存储恢复时是否重新校验。

### 12. Kleinjung/Bai-Brent 首项系数搜索在默认参数下退化为首一

- **更正**：这是搜索质量问题，结果仍经有效性校验，不是正确性问题。只影响超过 400 bit 的输入，成因是分发门槛：次数不小于 5 才走 Kleinjung 或 Bai-Brent（`include/gnfs/polynomial/selector_dispatch.hpp:65`），而次数由位数决定（`include/gnfs/core/params.hpp:122-130`）。第二阶段确是重复实现（引核实；哪个阶段、与什么重复，正文没有写，未确认）。〔复读〕
- **验收的困难**：第一阶段是私有成员，没有测试钩子。上一轮写的复现条件不等号方向反了：退化的复现条件是搜索半径不小于候选上限的一半（引核实）。修复方向本稿没有明确写出，只给了下面「限制候选配额」一个备选。`tests/test_kleinjung.cpp` 整个二进制是 slow（`CMakeLists.txt:1992-1993`〔复读〕）；`tests/test_kleinjung_large.cpp:117-125` 手写候选数 500、半径 200，不在退化区，也没有任何断言〔复读〕。现有测试没有一个落在退化区，也没有在超过 400 bit 上用默认参数跑过选择器。
- **风险**：没有超过 400 bit 下非首一搜索的 Murphy E 对比记录（Murphy E 是多项式筛选常用的产出评分，越大越好），价值没有实测支撑。本稿没有给出值得做的正面依据。
- **要你定**：是否限制每个 $a_d$ 的候选配额（每个首项系数最多贡献多少候选）；具体怎么限制，正文没有描述，未确认。决定之前需要一条超过 400 bit、默认参数下的实测，这只能在 CI 上做，而 heavy 层目前没有通道。

### 13. 自动选路的回退语义与 SIQS 重标定

- **更正**：自动模式走 SIQS 的完整条件是十进制位数不超过 100 且大于 24、bit 数大于 80（`src/api/pipeline.cpp:1192-1212`）；`:4389` 的注释「User forced SIQS only」与事实不符，因为自动选路在十进制 25 至 100 位时也返回 SIQS（`:1209-1212`），同样进入这个分支（修订时读码）。证据比上一轮更重：仓库里带硬断言的真实 SIQS 分解只到十进制 55 位（`tests/test_siqs_e2e.cpp:145-180`，55 位那例只跑直连 `siqs::factor`，不走 pipeline）；README 的 59 位与 65 位计时早于筛根修复（引核实）。准确说法是：修复后十进制 60 位及以上的完整 SIQS 分解，在任何路径下都没有证据。〔复读〕
- **验收的困难**：MethodSelection 全部用 `assert`，Release 下失效；代码里没有让 SIQS 失败的注入点，SIQS 分支的超时下限是 60 秒（与 §四.24 的 30 秒探测的关系见「阅读约定·求解路径」）。要先把回退决策抽成可单测的纯函数。SiqsE2E 覆盖不到十进制 60 位档，重标定必须新增用例，耗时只能取自 CI。
- **要你定**：自动模式下 SIQS 失败是回退 GNFS，还是保持现状并改文档；是否把 GNFS 的自动选路下界从十进制 101 位下调。下调依赖「十进制 50 位 GNFS 端到端分解证据」先成立。

### 14. CLI 入口路径按 UTF-8 处理

- **更正**：UTF-8 约定只覆盖 mmap 接口，不是整个持久化层（`include/gnfs/util/utf8_path.hpp:8-15` 的注释自己限定为 public mmap interfaces）。两个选项不等价：一是「只在打开文件处转换」，二是「改宽字符入口」（让 `main` 以宽字符接收 argv）。第一个要成立，前提是 argv 已经是 UTF-8，而仓库里没有任何东西把 CLI 的 argv 规范成 UTF-8（引核实）。目前是约定层面的隐患，不是已触发的缺陷。〔复读〕
- **怎么验证**：做成真实进程用例，加进 `tests/test_cli_event_stream.cpp`（已有启动 CLI 的先例，路径全是 ASCII）；Release Readiness 的 Windows 作业只验证构建通过，不跑 CLI 用例。
- **风险**：Windows 行要让中文路径用例通过，大概率需要宽字符入口（平台推断，未验证）。
- **要你定**：是否改宽字符入口。MinGW 下要加 `-municode`（先例 `CMakeLists.txt:713-717`，引核实），而 Release Readiness 的 Windows 作业用 MinGW 构建 `gnfs`。〔复读〕

### 15. Windows fast 层与 SQUFOF oracle 分级

- **更正**：Windows 行不是只跑 instant，另有定向步骤跑 fast 的 BWKrylovMmapIntegration（`.github/workflows/ci.yml:275-290`）。「Windows 只跑 instant」是 2026-06-02 起写明的策略（`docs/testing-ci-policy.md:39`、`:216-219`），所以这是改策略，要同步改文档。扩到 instant|fast 只新增 fast 层；GNFS_E2E、SiqsE2E、FactorWithKleinjung、RegressionGate 仍只在 Linux deep gate。〔复读〕
- **规模**：Windows 上 fast 注册 29 项、已跑 1 项，扩标签后新跑 28 项（引核实的静态解析）。当前 Windows 行 47 分 58 秒，上限 75 分钟（`ci.yml:80` 的 `timeout_minutes: 75`〔复读〕），余量约 27 分钟；28 项 fast 在 MSVC 上的耗时查不到。
- **更正上一轮**：`scripts/test-catalog/windows-unavailable.txt` 只管本地 runner，不影响 CI 的 `ctest --label-regex`；AGENTS.md 的新测试登记规则不支持上一轮引用它的那句话（上一轮引用 AGENTS.md 的原话本稿没有记录，未确认；能说的只有：AGENTS.md 现行规则要求新测试登记 CMake、`scripts/test.sh` 的三张表与标签，没有提到这个文件）。
- **怎么验证**：`CI required` 保持绿，Windows 作业日志出现 Integration 等 fast 测试的通过记录。
- **要你定**：是否接受 Windows 行时长上升；三个 SQUFOF oracle（标 fast 的 `SQUFOFStrategyOracle`、`SQUFOFSuccessChallengeOracle`、`SQUFOFBudgetOracle`，`CMakeLists.txt:1832-1833`、`:1854-1855`、`:1862-1863`〔复读〕；Windows 行扩到 fast 后它们会第一次在 MSVC 上跑）是降到 slow 只在 deep gate 跑一次，还是保持 fast。

### 16. 发布出口：版本合同升级与发布包真实分解冒烟

- **更正**：「任何 SHA 都过不了预检」要分模式：verify-only 对任何 SHA 都拒绝（`scripts/release_contract.py:587-588`）；publish 只对标签指向的那个提交返回恢复态（引核实）。恢复态指标签已存在时契约脚本认可的「已发布 Release 的恢复」状态。标签 `v0.1.0` 已发布为不可变 Release，核实已用 `gh release view` 复核。〔复读〕
- **改动面比上一轮列的大**：`scripts/release_binary_contract.py:123` 独立硬锁（不经过 `release_contract.py`，单独判断版本号）；`scripts/release_contract.py` 另有三处入口、硬编码的资产名与发布说明正文、对工作流文本的断言；两个发布工作流与 Workbench 工作流共 19 处；Workbench 的版本号；两个 README 各 16 处（均引核实）。漏改会让必需检查变红（修订时 `grep` 到这两个契约脚本在 `scripts.yml:73-83` 与 `release-readiness.yml:52`、`:108`、`:184` 被调用，Script Checks 与 Release Readiness 都在必需检查表里）。〔复读〕
- **不要重做**：Workbench 包已有真实分解探针（`apps/macos/GNFSWorkbench/scripts/build-app.sh:192-221`，引核实），缺的是三个 CLI 发布包。〔复读〕
- **怎么验证**：手动触发 Release Readiness（只有 Linux 与 Windows 两个作业，没有 macOS，冒烟覆盖不到 macOS 包）；Release Artifacts 的 verify-only。后一项要等版本合同升级之后才可用：修订时读 `scripts/release_contract.py:578-592`，拒绝的条件是标签已存在且不允许复用，换成尚不存在的新版本号标签后 verify-only 才走得通。
- **风险**：Release Readiness 是发布预检的必需上下文，冒烟失败会连带阻断发布。
- **要你定**：下一版本号、是否现在发、发布说明内容。

### 17. 发布供应链确定性：PPA 依赖与资格构建配置

- **范围缩小**：Windows 的 UCRT64 构建工具闭包已在 main 上冻结（提交 8dfa59e1，2026-11-04，是 HEAD 的祖先；`scripts/windows-ucrt64-build.json` 钉住 57 个包，修订时 `grep -c '"name"'` 为 57；`docs/releasing.md:296-304`），路线图那一节已过时（指哪份路线图的哪一节，未确认），这部分从方向里删去。UCRT64 是 MSYS2 的 64 位运行时环境，这里指在它上面编译 Windows 发布包所用的 CMake、Ninja、Python 与编译器的依赖包，「构建工具闭包」指这整组工具及其依赖的固定版本清单。仍未冻结的只剩 MSYS 基础环境（`.github/workflows/release-readiness.yml:117-121` 的 `update: true` 与 `install: python`，即 setup-msys2 这一步没有固定版本）。〔复读，提交 8dfa59e1 除外〕
- **PPA 在线安装是有意设计**：`docs/releasing.md:253-256` 说明安装器与 Release Readiness 共用，让每个 PR 与 main push 都走一遍发布包所用的边界；`:264-265` 承认它不是 Debian 包闭包的逐位钉住。「边界」按同一处原文指 PPA、指纹、架构、glibc、GCC/G++ 12 与构建配置这套约束（PPA 即 Ubuntu 的个人软件包档案，这里是在线安装工具链的来源）。做缓存或镜像等于放弃这个性质。〔复读〕
- **漏掉的约束**：这些工作流和安装脚本被 `scripts/release_contract.py` 逐片段钉住，自建镜像或换 PPA 路径必须同步改契约和 self-test（引核实）。
- **更正上一轮**：`scripts/check_harness.py:33-49` 只钉 5 个 shell 函数，持有 `do_build` 的 `scripts/test.sh` 不在表内〔复读〕；「手动触发 Release Qualification」不可直接做，资格工作流只能是 `workflow_call`（`scripts/release_contract.py:3547-3556`〔复读〕），可行的替代是 Nightly Deep Tests 的手动触发；上一轮把「日志里出现关闭原生调优」当作可检查的证据，核实认为按现状日志里看不到这一项（引核实；「原生调优」具体指哪个开关，未确认）。
- **要你定**：资格验证是否固定为可移植配置（全量关闭会改变本地默认构建）；PPA 依赖是自建镜像或缓存，还是接受外部可用性风险。前一句只能按字面理解：把资格验证的构建固定成与发布包相同的可移植配置，会连带改变本地默认构建；「资格构建配置」具体指哪些选项，未确认。

### 18. relation 归约：十进制 50 位产出缺口与路线收敛

上一轮的两个方向合并，因为要定的是同一件事：默认归约路线怎么定。

- **更正**：
  - 「V0 是主流程唯一可达的合并算法」不成立。归约路线是 `ReductionStrategy` 的 6 个取值（`include/gnfs/relation/reduction_engine.hpp:39-46`）：`NoLargePrimes`、`FilterOnly`、`StandardV0`、`StandardV0WithV3`、`CliqueV0`、`Structured`。正文里 legacy 指 `Structured` 之外的路线，structured 指 `Structured`；修订时读 `src/api/pipeline.cpp:3162-3180`，`GNFS_V0_BFS` 的判定为真时选 CliqueV0（即 V0_BFS），V3 级联模式非 Off 时选 StandardV0WithV3（即 V3 cascade）。准确说法是：默认环境变量下 `Pipeline::run()` 的自适应筛循环固定用 StandardV0 或 StandardV0WithV3（`src/api/pipeline.cpp:2824-2828`、`:2919-2921`），分布式路径只在 NoLargePrimes 与 StandardV0 间选（`:2437-2439`，引核实），都不读 `GNFS_V0_BFS`。〔复读〕
  - 这个缺口不是新发现，它就是结构化归约项目的基线；替代实现已合入但是 opt-in，2026-08-21 的决定是不提升（`docs/plans/2026-07-21-parallel-structured-filter.md:763-772`）。
  - 「合并后再清理单例」这个杠杆补不上缺口。legacy 输出的大素数列一共 4,647 个（`docs/perf/structured-ooc-measurement.md:1193`），删去 r 行、c 列后新盈亏至多 $-20{,}748+c\le -16{,}101$。推法：盈亏是行数减列数，删 r 行让它再少 r，删 c 列让它加 c，而 c 至多是 4,647。这是核实的算术推导，不是实测。缺口本身在同文件 `:1194-1195`：legacy 矩阵 6,559 行、27,307 列，行数远少于列数，所以说缺口主体是行数不足。上一轮把它列为可做的改进，已删去。
  - V3 cascade 的十进制 50 位五轮实测已存在且为负面（`docs/perf/performance-doctrine.md:1323-1332`），设计文档 `docs/perf/v3-cascade-design.md:128` 没有回填。
  - 6,559 行是 2026-08-20 的历史记录（legacy 路线的归约输出行数，也就是矩阵行数，同文件 `:1192`、`:1194`；日期未确认），当前 main 上没有重测。〔复读〕
- **路线现状**：FilterOnly 在 `src/` 下只有处理它的分支、没有选出它的调用；CliqueV0 只由 `Pipeline::filter()` 和两处测试入口选出（`src/api/pipeline.cpp:3176-3180`，处理分支在 `:3353-3356`；两处测试入口引核实）。原稿写「CliqueV0 与 FilterOnly 在 `src/` 下没有使用者」，与 `:3178` 不符，修订时更正。「退役 legacy V0」当前不可行，能做的只有删除 CliqueV0 与 V0_BFS，以及可选地删除 StandardV0WithV3。〔复读〕
- **验证与约束**：120 bit 门禁把 legacy 输出与矩阵身份钉死（`tests/test_structured_filter_pipeline_120bit.cpp:49-68`、`:304-311`，并把 V0_BFS、WEIGHT_CUTOFF、DROP_RESIDUAL、V0_WEIGHT3 显式置 0，`:334-337`），改归约必然失败，除非重定基线。合并策略被计入 sieve run identity（`include/gnfs/sieve/sieve_run_identity.hpp:30-37`），改默认值会影响断点续跑的身份校验。V0BfsPolicy、Filter、CliqueMerger 是 `assert` 型测试，Release 通道里断言不执行（引核实）。〔复读〕
- **要你定**：structured 是推广、维持显式研究模式，还是另找路线；是否允许默认过滤行为改变；V0_BFS 接入主流程还是删除；V3 cascade 是否先退役。`GNFS_WEIGHT_CUTOFF` 与 `GNFS_DROP_RESIDUAL` 的十进制 50 位实测在仓库里查不到，要启用得先测。

### 19. 性能回归门禁

- **更正**：在 Benchmarks 作业里加失败步骤不会让 PR 失败。作业级 `continue-on-error`（`.github/workflows/benchmark.yml:36`），且该检查不在 main 的必需检查里（引核实；修订时读 `scripts/release_contract.py:138-177`，表里确无 Benchmarks）。PR 上没有可对比的基线：只检出 PR 头，仓库没有提交参考计时，产物只留 30 天。正负 5% 的阈值没有噪声依据。120 bit 档是 heavy、不进 PR CI，且在矩阵构建后停止、不求解（`tests/test_structured_filter_pipeline_120bit.cpp:342`、`:249-250`）。〔复读〕
- **怎么验证**：门禁本身只能靠「制造一次变慢，确认 PR 变红」来证明生效。
- **风险**：CI runner 上的计时方差没有记录。
- **要你定**：阈值与参考机器；Benchmarks 是否成为必需检查，以及「不等 CI 即合并」的协作规则怎么调整；是否在同一 runner 上对基线与头提交做 A/B。

### 20. SIQS 乘数组合与影子证明的推广决定

- **更正**：`src/siqs` 不等于影子证明本体。影子证明是 `GNFS_SIQS_SHADOW_PROOF` 控制的一条有界、只读的 SIQS 旁路（`include/gnfs/siqs/shadow_proof_runner.hpp:3-4`），三态是 off、observe、prefer（`include/gnfs/siqs/shadow_proof_observe.hpp:34-38`）：observe 只记观测记录，prefer 在它给出可接受的因子对时改走它的结果（`include/gnfs/siqs/siqs.hpp:2459-2503`，引核实）。「证明」具体在证明什么命题，正文没有展开，未确认。影子证明本体是 header-only，已按环境变量接进 `factor()`；`src/siqs` 的 20 个文件（8 个 .cpp 加 12 个私有头）只是封存留出集 RSS 闸门的活动设施（引核实；RSS 闸门指对这条旁路常驻内存的上限检查，「留出集」指什么，未确认）。显式 prefer 的生产路由已实现，缺的只是自动推广。`docs/plans/2026-11-05-siqs-multiplier-candidates.md:9-14` 描述的是基线提交，已过时。〔复读〕
- **乘数组合缺的是收益证据**：乘数组合指 SIQS 的 Knuth–Schroeppel 乘数 $k$（筛 $kN$，`siqs.hpp:313-316`）有多个候选时怎么选用。协调器是失败后换乘数重试，不是择优；自动探测预算只有 30 秒，第一次尝试吃掉预算就不会有第二次；计划文档写明没有测过加速（引核实）。
- **影子证明剩余 6 项里有 3 项要人工给部署内存预算与批准身份**（剩余 6 项是哪 6 项，正文没有列出，未确认），靠 CI 完不成；现有活捕获证据早于筛根修复（活捕获指在真实 SIQS 运行中采到的语料，修订者按 `shadow_proof_runner.hpp:260` 注释里的 live capture 理解）。
- **收起的硬依赖**：`CI required` 依赖的 Alpine musl 通道显式构建并运行 RSS 闸门的测试 `SiqsShadowProofRssCampaignJournalStore`（`.github/workflows/ci.yml:330-371`，测试名在 `:364`、`:368`）；这些头随 `include/gnfs` 整目录安装。〔复读〕
- **验证通道**：能出 RSS 记录的 `./scripts/test.sh probe-siqs-shadow-observe` 与 `compare-siqs-shadow-observe` 目前没有任何 workflow 调用（引核实）；资格工作流不能加手动触发，也不适合放实验性作业。
- **要你定**：乘数组合是否设为默认；影子证明继续投入完成剩余项，还是维持「默认 off 加显式 opt-in」。

### 21. durable wave store：接入生产分布式筛或收起

- **更正**：
  - 「仍在持续修改」偏强：45 次提交里 43 次落在 2026-07-27 至 08-03，之后只有 2 次（引核实）。
  - 「src/sieve 之外没有生产代码引用」只对类名成立：`src/relation` 的 OOC 清理授权（批准外存关系清理的那段代码）直接调用 wave store 里的函数，会被安装的公共头含对 durable 命名空间的声明（引核实）。
  - 规模更大：`src/sieve/distributed_*` 共 59 个文件 57,778 行（修订时 `ls` 与 `wc -l` 复核），fork 路线只占其中一个文件的 1,300 行；测试 53,710 行（引核实）。
  - 拒绝「分布式筛加续跑」是 2026-09 刻意加的，之后的方向是加固隔离，不是接入（`src/api/pipeline.cpp:842-851`〔复读〕）。
- **接入的硬前置**：V1 工作身份是 durable wave store 给每份工作（多项式、因子基、筛参数、筛区域、余因子配置）记的身份结构（`include/gnfs/sieve/distributed_sieve_protocol.hpp:580-733`；V2 在代码里还不存在）。核实认为 V1 的因子基身份要求素理想的 (p,r) 严格递增，而真实因子基会乱序输出，要先做 V2 工作身份（引核实）。策略脚本主动禁止生产文件出现消费确认等标识符（`ConsumptionStartedV1`、`WaveConsumptionAckV1`、`begin_consumption`、`acknowledge` 等，`scripts/check_distributed_sieve_policy.py:4655-4666`），也禁止 `pipeline.cpp` 自行铸造 durable 工作（`:10076-10082`，禁止的是 `DistributedSieveWorkIdentityV1`、`bind_distributed_sieve_work_v1` 等）。有两个测试钉住「必须拒绝」（`tests/test_api.cpp:3882-4010`，引核实）。〔复读〕
- **怎么验证**：「并设不再被拒绝」证明不了 durable 续跑接上了，要补流水线级验收：master 中途崩溃后重跑，采纳已完成的 chunk，关系序列与摘要不变。
- **要你定**：上线还是收起。另外 fork 子进程的 `LatticeSieve` 没设线程上限（`src/sieve/distributed_sieve.cpp:411`；未设时取 `hardware_concurrency`，`include/gnfs/sieve/lattice_sieve.hpp:697-710`），修成多少（每子进程 1 线程，还是按预算分配），会改变现在的性能特征。〔复读〕

### 22. 裁决只被测试编译的 helper 头文件

- **更正**：范围比上一轮写的大。按 `#include` 传递闭包，`include/gnfs` 下 52 个头文件生产不可达（只被测试编译、生产代码到不了），其中 45 个带环境变量门控，全部在 2026-05-21 至 23 加入，合计 15,097 行（引核实）。52 个头文件的清单不在本稿里，未确认，所以现在还做不到「逐个裁决」。上一轮「14 个」的计数与点名对不上。
- **同批里确有已接线的**，要逐个裁决：三个头文件 `sqrt/hensel_parallel.hpp`、`linalg/detail/spmv_simd.hpp`、`factor_base/fb_roots_parallel.hpp`，以及两个用环境变量名指代的部分 `GNFS_MURPHY_ALPHA_THREADS`、`GNFS_BW_KRYLOV_STREAMS`（各自门控哪个头文件，未确认；引核实）。
- **更正**：`bench/microbench` 与本议题无关，7 个 .cpp 不包含任何不可达 helper（引核实；7 个这一数字修订时 `ls bench/microbench/*.cpp` 复核），已从这个方向里移除。
- **删改的同步面**：策略脚本两张表、执行策略源码、测试注册、`scripts/test.sh` 三张表。仓库里已有一套与这些变量绑定的休眠设施，durable wave store 一旦接入，它们会以身份字段形式生效（引核实；这句话修订者无法解释也无法核实：是哪些设施、哪些字段，未确认）。
- **怎么验证**：删除路线靠 `CI required`、Script Checks、Docs Integrity；Docs Integrity 只有链接检查、忽略锚点，也不在必需检查里，「文档同步删改」只能靠人工评审。接线路线只要求 instant|fast 行，低于 AGENTS.md 对进入筛法与余因子生产路径的最低验证，要补 deep gate。
- **要你定**：哪些保留待接线、哪些删除。

### 23. CI 触发策略与快速失败层

- **更正**：上一轮「fail-fast: false 让静态检查失败不终止重作业」的因果不成立。`fail-fast: false` 是有意引入的（`ci.yml:25`），只管矩阵内一行失败不取消其余行；静态检查失败不终止重作业，是因为 Script Checks 与 CI 没有 job 依赖（`CI required` 只依赖三个作业，`.github/workflows/ci.yml:376-379`），而且 Script Checks 在 PR 上有路径过滤（`.github/workflows/scripts.yml:7-22`）。〔复读〕
- **定性更正**：这里说的「CI 时长」是 main 上全部必需检查的口径，与 §二.9 说的 `CI` 工作流内部关键路径不是一个口径。重复构建不是主因；CodeQL 是 main 上最慢的必需检查，核实记为 68.2 分钟且无缓存（引核实）。策略检查器的重复代价小且不在关键路径（没有测量、行号或提交号支撑，未确认），「体量」是可维护性问题，不是 CI 时间问题。
- **硬约束**：`scripts/check_harness.py:206-210`、`:303-310` 同时钉住了策略检查器的两条通道，去重要一起改它（引核实）。`scripts/release_contract.py` 的必需检查表钉住 12 个 job 名，Sanitizers 不能改成定时触发、不能加路径过滤（引核实）。Windows 构建并行度 2 由提交 05195353 引入（修订时 `git show -s`：2026-08-04，「fix(ci): avoid Windows vcpkg applocal races」），提交信息没有说明依据。〔复读〕
- **怎么验证**：只能在合并后于 main 上完成，且 CI 时长波动大（核实记近 6 次 34.0 至 68.1 分钟；是哪条工作流的时长，正文没有写，未确认），单次前后对比证明不了提速。
- **要你定**：Benchmarks 与 Coverage 的触发策略；是否引入前置快速失败层（代价是一次拿不到全部矩阵信号）；是否实验提高 Windows 构建并行度。

## 四、第三梯队：体量大，或依赖前置成果

### 24. 十进制 50 位 GNFS 端到端分解证据

- **现状**：structured 首轮矩阵 211,732×188,413（`docs/perf/structured-ooc-measurement.md:1190-1201`），现有探针断言不得进入求解与平方根（`tests/test_structured_ooc_50d_probe.cpp:818-822`）。〔复读〕
- **补充的硬约束**：
  - 环境变量：十进制 50 位的 lp_bits 为 23，OOC 按尺寸默认打开但不算显式启用（lp_bits 不小于 22 时默认为开，但「显式启用」位为假，`include/gnfs/relation/ooc_policy.hpp:44-65`），structured 前置判定会拒绝，必须显式设 `GNFS_OOC_RELATIONS=1`（现有探针的做法，`tests/test_structured_ooc_50d_probe.cpp:843`），且不能启用续跑或分布式。〔复读〕
  - 如果走 `Pipeline::run()`，即使强制 GNFS 也会先做 30 秒的 SIQS 探测（`src/api/pipeline.cpp:4322-4386`），十进制 50 位输入会被 SIQS 抢先解掉而假通过。必须设 `GNFS_DISABLE_SIQS=1`，并断言实际执行了 GNFS 各阶段。〔复读〕
  - 新用例要走 `Pipeline` 新写；`test_stress` 只走 legacy 归约。
  - 资格工作流不能加手动或定时触发，且被发布验证复用、禁止容错（`scripts/release_contract.py:3547-3556`）。更稳妥的做法是在 `nightly.yml` 加独立作业。nightly 是每周五 17:00 UTC，不是每晚（`nightly.yml:5`）。「heavy 与 stress 层没有 CI 通道」只对 stress 和除 120 bit 外的 heavy 成立，120 bit 门禁由资格工作流按名调用（`release-qualification.yml:71-72`）。〔复读〕
- **风险**：耗时无法从仓库证据判断。文档里 structured 首轮单条路线（只跑 structured 一条路线的首轮筛分）1,359 至 1,442 秒，是 macOS arm64 且不含求解（`docs/perf/structured-ooc-measurement.md:1190-1201` 这张表里 structured 的 Route wall time 是 1,441,922 ms；1,359 秒出自哪一行，本次修订没有找到，未确认）。所以「是否接受 nightly 多占时间」这项决定在文中没有量级可依据。
- **依赖**：先有一个 CI 通道。可以直接从 structured 路线起步。
- **要你定**：走 structured 还是 legacy；是否接受 nightly 多占时间。

### 25. Couveignes 回退容量：格基重建

- **更正**：「真实规模的依赖一旦 Hensel 失败基本无从恢复」是推断，仓库里没有实测证据。回退容量指 Couveignes 方法作为 Hensel 失败后的回退路径时能承载的规模：硬上限是素数个数 16（传入 `Couveignes::compute` 的素数列表，`include/gnfs/sqrt/couveignes.hpp:332-336`），起点可调（所指未确认），只是生产路径固定用默认值。`GNFS_FORCE_COUVEIGNES=1` 不是现有 CI 配置，没有任何 workflow 或脚本设置它。〔复读〕
- **没有可复用件**：sqrt 模块里没有格约化代码，仓库现有 LLL 只有二维、用于筛法格基（引核实）。
- **验证太弱**：GNFS_E2E 的 `main` 只跑 5 个不超过 17 bit 的小例（修订时复读 `tests/test_gnfs_e2e.cpp:1097-1101`，输入是 143、9991、10403、10800、96091，最大 17 bit；原稿写「17 位」未标单位，容易被读成十进制），这个规模下穷举本来就够用。要在 RegressionGate 或 FactorWithKleinjung 的规模上、开着该变量跑才有意义，只能是手动设环境变量。〔复读〕
- **依赖**：十进制 50 位端到端，用真实失败依赖做输入。本稿没有给出值得做的正面依据，只有「现有验证太弱」这一条。

### 26. 筛法内核：逐位置范数初值与分侧阈值

- **证据更正**：`norm_tile.hpp` 不是「按位置算范数的 helper」。它只解析 `GNFS_SIEVE_NORM_TILE_BITS` 并返回 $2^N$ 行的 tile 大小（`include/gnfs/sieve/norm_tile.hpp:121-132`、`:148-169`；$N$ 就是 `GNFS_SIEVE_NORM_TILE_BITS` 的取值，上限对应 256 行，`:164-168`）。它头注释里描述的「对每个格点求范数」的 pass 在 `lattice_sieve.hpp` 里不存在。上一轮把它写成现成积木，已改。〔复读〕
- **仍然成立**：`init_sieve_array` 只调一次 `estimate_initial_log`（`include/gnfs/sieve/lattice_sieve.hpp:745-748`；原稿引 `:745-789`，本次只读到 `:752`）；合并阈值是两侧之和，即有理侧阈值加代数侧阈值（`:215-218`、`:1621-1628`）。「区域内范数差别大，导致漏筛和多送候选」是推断，仓库里没有测量。本稿没有给出值得做的正面依据。〔复读〕
- **改动面**：`docs/algorithms/lattice-sieve-numeric-contract.md:91-108` 把「初值是单一估计值」写成了契约；若干测试用独立 oracle 复刻了标量初值公式；`lattice_sieve.hpp:744` 的 `memset(0)` 是刻意的性能选择；新增门控变量大概率要在策略脚本登记（引核实）。
- **验证**：120 bit 门禁是固定身份门禁，默认路径的候选集合一变必然失败；十进制 50 位有界对照只有 4 个 special-q、没有逐 special-q 计时，规模太薄；仓库另有 `compare-50d-first-round`（`scripts/test.sh:9151`〔复读〕）。两条资格通道都只在 nightly 或发布预检触发。
- **依赖**：性能回归门禁，以及 120 bit 基线重定。

### 27. 余因子级联：先接通分阶段计时，再议 P−1、P+1 与多档 ECM

- **证据更正**：
  - 收益前提与仓库已有画像不符。固定十进制 50 位语料的采样显示热点在 `SQUFOF::squfof_core()`，21,225 个样本；128 bit 除法（`__udivmodti4`，128 位整数除法的运行库函数）、代数试除、有理试除、分配器分别只有 614、166、99、11（`docs/perf/structured-ooc-measurement.md:582-585`）。2,284 个候选实际调用 SQUFOF 159 次（`:639-641`）。把 P−1 与 P+1 加在 SQUFOF 之后，针对的不是已知热点。〔复读〕
  - 「没有按位长选 B1 的策略链」只对默认级联成立。`ECM::auto_tune` 已有 6 档位长对应 B1、B2、曲线数的表（`include/gnfs/cofactor/ecm.hpp:404-436`），只是没接入 `classify_cofactor`。〔复读〕
  - 分阶段计时没有接入生产（引核实）。
  - 仓库有相反方向的历史决定：提交 f63eb3bb（2026-05-21）为筛法速度删掉了 3LP 搜索里的 ECM 档，现行代码仍如此（修订时 `git show -s` 复读了提交说明）。
- **更正上一轮的验证方式**：三个端到端测试都不断言关系数，deep gate 用 `--output-on-failure`，通过的测试输出不留档；CofactorBatchBench 与 BatchECMBench 都不经过 `classify_cofactor`（引核实）。
- **调整后的方向**：先接通分阶段计时并画像，再决定是否补 P±1，或把 `auto_tune` 表接入。收益证据用 `./scripts/test.sh sweep-50d-candidate-batch`（固定十进制 50 位语料，输出墙钟中位数与关系摘要，引核实）。
- **依赖**：cofac 耗时的回归手段（cofac 指余因子分解，即关系筛选里试除、SQUFOF、ECM 这一段；「回归手段」指能发现这一段变慢的测量或门禁）。

### 28. 筛区域与 special-q 按 q 自适应，并先验证 SkewLLL

- **证据更正**：
  - 分档写错。按十进制位数：不超过 25 位是 4096×1024，26 至 40 位是 2048×1024，41 至 65 位是 4096×2048，66 至 80 位是 16384×8192，80 位以上是 32768×16384（`include/gnfs/core/params.hpp:276-301`〔复读〕；代码里 41 至 50 与 51 至 65 是两档、取值相同，这里合成一档）。26 至 40 位的 2048×1024 小于不超过 25 位的 4096×1024，是代码里的事实，不是本稿笔误；代码在这两档旁只注了位置数（4M、2M，`:285`、`:288`），没有说明理由。
  - 「25/50/60 位收益待验证」不全对（原稿未标单位；按提交说明里的 50d/60d 与参数分档，读作十进制位）。小规模已有加速记录且已知会回归。加速记录是提交 34bd2a53 的说明：regression gate 从 22 秒到 12 秒、`test_factor_with_kleinjung` 从 4 分 47 秒到 2 分 15 秒。回归记录是提交 e5fa8110 的说明：27 bit 的 `test_bucket_sieve` 候选数从 772 降到 0；候选降到 0 是坏事，那个用例在 SkewLLL 打开后筛不出候选，所以 SkewLLL 保持默认关闭。十进制 25、50、60 位没有记录（两条提交说明修订时用 `git show -s` 复读）。
  - 「区域形状直接影响并行度」只对内层成立。外层批宽（每批一起处理的 special-q 个数）只看十进制位数，不超过 50 位取 4，否则取 2（`src/api/pipeline.cpp:2620`〔复读〕）；内层行数小于 500 即单线程（引核实）。
  - 已有 AdaptiveBasisManager（`GNFS_ADAPTIVE_LATTICE`，默认关闭）按 q 扰动格基后重筛，只改基不改区域（`include/gnfs/sieve/adaptive_lattice.hpp:13-49`；修订时读到 `:24`，默认关闭与扰动重筛的描述相符）。
- **验证目前不可行**：两条资格通道的测试二进制在内部强制 `GNFS_LATTICE_SKEW=0`（`tests/test_structured_ooc_50d_probe.cpp:859`；`tests/test_structured_filter_pipeline_120bit.cpp:341`），环境里的 1 会被覆盖〔复读〕。验证前要先加一个不固定 skew、输出逐 q 产出与耗时的测量入口。
- **改默认时要同步**：`GNFS_LATTICE_SKEW` 是分布式执行策略的语义键（它的取值被计入分布式执行策略的身份，改动会改变身份）；本地 sieve run identity 绑定了区域边界和 q 范围；这三个格基开关在 `docs/env-flags` 下查不到文档（修订时按上下文与 `grep` 确认是 `GNFS_LATTICE_LLL`、`GNFS_LATTICE_SKEW`、`GNFS_ADAPTIVE_LATTICE`；`docs/env-flags` 下能查到的格基相关名字只有 `GNFS_LATTICE_BASIS_PARALLEL_THREADS` 与 `GNFS_LATTICE_COORDS_SIMD`）；没有任何测试在 SKEW=1 下跑 `test_bucket_sieve`（引核实）。
- **依赖**：测量入口与性能回归门禁。

### 29. 拆分 pipeline.cpp 里的超长函数

- **证据更正**：
  - `matrix_phase` 的右花括号在 3803（修订时 `grep`：函数起于 `src/api/pipeline.cpp:3337`，止于 `:3803`，约 467 行）。〔复读〕
  - 「三条路线分段」不准确（上一轮的说法按更正反推是：这个函数可以按 distributed、checkpoint 续跑、OOC 三条路线切成三段）。只有 distributed 是连续的提前返回块（`src/api/pipeline.cpp:2375-2465`）；checkpoint 续跑与 OOC 是贯穿全函数的交叉状态，续跑隐含 OOC，分布式与续跑在 preflight 互斥（`:842-851`）。〔复读，`:2465` 未核对〕
  - 「无法单独测试」言过其实。三条路线都能经环境变量加公开 API 单独触发，且已有测试；做不到的只是绕过 Pipeline 直接测内部块（引核实）。
  - `run()` 的 385 行里大部分是素性、完全幂、试除、rho、ECM、SIQS 快路径，GNFS 段只有 `:4405-4437`（修订时 `grep`：`run()` 起于 `:4054`，止于 `:4438`）。〔复读〕
  - 顺序约束在入口已由结构保证：`run()` 只捕获一次 route snapshot 传给内部实现（`:4066`、`:4409-4411`），筛法主体里没有 `getenv`；在回调之后读环境变量的是 `matrix_phase`（引核实；修订时 `grep -n getenv`，`:3437`、`:3444`、`:3553` 都落在 `matrix_phase` 的范围内）。〔复读〕
- **拆分约束**：策略脚本以字面路径 `src/api/pipeline.cpp` 为键做检查（`scripts/check_distributed_sieve_policy.py:10076-10082`），把 distributed 块搬到新文件后检查不会自动跟随〔复读〕。函数内有 7 个按引用捕获的具名 lambda 共享局部状态（引核实），纯搬移要引入上下文结构体并改头文件的私有成员，规模可能大于「中」（小、中、大没有定义标尺，实际规模本稿给不出）。
- **验证缺口**：Sanitizers 两道对这次重构几乎没有覆盖；应补 FullResume（slow，CI deep gate）；有界十进制 50 位对照把续跑置空、分布式 worker 置 0，不覆盖这两条路线（引核实）。
- **依赖**：durable wave store 的决定。

## 五、需要你决定的事

| 编号 | 要定的事 | 关联方向 |
|---|---|---|
| 1 | structured 归约是推广、维持研究模式，还是另找路线；默认过滤行为能否改变；V0_BFS 接入还是删除；V3 cascade 是否先退役 | 18 |
| 2 | durable wave store 上线还是收起；fork 子进程线程上限的目标值 | 21 |
| 3 | helper 头文件的保留清单与删除清单 | 22 |
| 4 | 自动模式下 SIQS 失败是否回退 GNFS；GNFS 自动选路下界是否下调 | 13 |
| 5 | 乘数组合是否设为默认；影子证明继续投入还是停在默认 off | 20 |
| 6 | 下一版本号、是否现在发、发布说明 | 16 |
| 7 | 资格验证是否固定为可移植配置；PPA 依赖用镜像、缓存，还是接受风险 | 17 |
| 8 | 性能回归门禁的阈值、参考机器、是否成为必需检查，以及「不等 CI 即合并」规则怎么调整 | 19 |
| 9 | Benchmarks 与 Coverage 的触发策略；是否引入前置快速失败层；是否实验 Windows 构建并行度 | 23 |
| 10 | 是否接受 Windows 行时长上升；SQUFOF oracle 是否降到 slow | 15 |
| 11 | BL 检查点是否改磁盘格式与提交协议；校验算法 | 10 |
| 12 | 收集器的构造期保证采用哪种形态 | 11 |
| 13 | 是否限制每个首项系数的候选配额（先要一条实测） | 12 |
| 14 | CLI 是否改宽字符入口 | 14 |
| 15 | Schirokauer 的 exponent_k 是否从 8 提高；首项系数被 2 整除时怎么处理（两问的选项正文都没有展开，见 §二.1 与表下说明） | 1 |
| 16 | 空输入返回 1 的契约是否翻转；平方根失败是否直接跳过该依赖（后半句在 §二.5、§四.25 正文里没有对应的说明和选项，未确认） | 5、25 |
| 17 | 十进制 50 位端到端走 structured 还是 legacy；是否接受 nightly 多占时间 | 24 |
| 18 | 开关总览表放在哪里 | 8 |
| 19 | 方向 3 的修复形态：公开 launch guard，还是新增批量提交接口 | 3 |
| 20 | 方向 5 是否加平方根失败统计（要改 `result.hpp` 及输出格式） | 5 |

第 15、16、18 项不阻塞第一梯队开工；其余 1 至 14、17 项关联的都是第二、三、四梯队的方向，与第一梯队开工无关；新增的第 19 项要在方向 3 动手前定，第 20 项由你决定做不做。

第 15 项里三个 Schirokauer 测试（`SchirokauerFastPoly`、`SchirokauerStrip`、`SchirokauerParallel`，`CMakeLists.txt:2645-2662`）至少 `SchirokauerStrip` 显式写了 exponent_k 为 8（`tests/test_schirokauer_strip.cpp:225-228`），只改默认值不会改变它们的行为，生产路径才用默认值（`include/gnfs/linalg/matrix_builder.hpp:296-299`）。〔复读〕

关于 `exponent_k`：它是 `SchirokaurConfig` 的字段，默认 8（`include/gnfs/linalg/schirokauer.hpp:541`），即 Schirokauer 映射在模 $\ell^k$ 下计算时的 $k$。代码注释写明：剥去 $v$ 个 $\ell$ 因子后需要 $k-v\ge 2$ 的精度，`strip_v ≥ k-1` 的输入会静默退化为全 0 列（`:541-546`）。把它列进决定表的理由、它与非首一归一化的关系，正文没有给出，未确认。〔复读〕

## 六、这次没能确认的内容

- 没有运行任何构建、测试或工作流，所有通过与否的判断都来自读码。联网查询只有原稿作者做的 CI run 的 job、step 时长与 macOS 行日志，以及打开的 PR 列表，都是只读，本次修订没有重做。
- 行号：本次修订只读复读了正文标〔复读〕的条目，其余没有复读；标「引核实」的推论都没有重新推导。
- 调研线索「分歧与指标素数只按范数赋值记一列」：代码事实属实（`include/gnfs/cofactor/trial_division.hpp:242-268`〔复读，本次只读到 `:250`〕；`src/factor_base/builder.cpp:652-711`），但后果无法确认。名词：「分歧素数」指 $f$ 在模该素数下有重根的素数，「一次素理想」指剩余次数为 1 的素理想，$f_P$ 是素理想 $P$ 的剩余次数，「指标素数」具体指哪一类，未确认。按 $v_p(N(x))=f_P\,v_P(x)$ 推导，一次素理想 $f_P=1$ 时列就等于素理想赋值。这条没有列入任何方向。
- Block Wiedemann 的亚二次 lingen（lingen 指由 Krylov 序列求线性递推的那一步，亚二次指用低于二次复杂度的算法实现它）：仓库里还没有规模走到那一步，没有列入。
- Workbench（macOS 应用）自身的方向没有调研。
- 各项「实际损失」都没有统计：非首一多项式在默认路径上的出现频率、Schirokauer 退化发生率、特征列记 0 让多少依赖失败、十进制 60 位在任何路径下的表现。
- 构建缓存命中后仍重编的原因没有调查。构建目录缓存已换成 ccache，原因的推断见「2026-11-27 增补」。
- CodeQL 时长、部分矩阵行的编译计数来自核实；原稿作者只亲自核对了同一 run 的 job、step 时长和 macOS 行的编译计数。
- `bench/microbench` 的 7 个 .cpp 不受构建系统管理，与 helper 裁决无关，没有归入任何方向。
- macOS CLI 发布包没有就绪作业，发布冒烟覆盖不到。
- CI runner 上的计时方差没有记录，性能回归门禁的阈值缺依据。
- 本次修订标「未确认」的地方都已写在原处，集中的有四类：来历（10 个角度、25 个候选、核实者与方法）；§一 表内的判断性说法；若干数字的口径（PR #28 的提交数、fast 层 1,359 秒的出处、近 6 次 CI 时长属于哪条工作流）；以及正文提到但没列出的清单（52 个 helper 头文件、影子证明剩余 6 项、BL 检查点的两个新用例）。

## 七、哪些方向可以并行

- **第一梯队的 9 项互相独立，可以同时开 9 个 PR。** 有两个接触点要协调：方向 1 与方向 2 都动矩阵列，方向 2 一定要重定 120 bit 基线，建议它单独合并；方向 7 与方向 9 都改 `.github/workflows/ci.yml`，建议同一个 PR 或先后合并。原稿还说方向 3 与方向 4 改两个文件里的相邻位置，与两边的「改哪里」不符：方向 3 不改 `src/api/pipeline.cpp`，方向 4 不改 `src/linalg/block_lanczos.cpp`，两者没有共同改动的文件，这条协调点不成立（修订时更正）。
- **第二梯队各方向的拍板互相独立。** 但方向 18 是方向 13 与方向 20 的背景；方向 21 的结论决定方向 22 与方向 29 的范围；方向 16 与方向 17 都改发布工作流，建议一起定。
- **长线里只有方向 24 不依赖其它梯队的成果**，可以先开，但它自带 §五第 17 项的三件要你定的事（走 structured 还是 legacy、是否接受 nightly 多占时间），并且依赖先有一个 CI 通道（修订时更正：原稿写它「不依赖任何决定」，与 §四.24 矛盾）。方向 25 依赖方向 24。方向 26、27、28 都依赖方向 19 或等价的回归手段，以及各自的测量入口。方向 29 依赖方向 21。
- **建议的起步顺序**：先开第一梯队，同时把第 1、2、3 项决定（归约路线、wave store、helper 头文件）拿出来定。原稿说这三项「决定了一半以上的代码该怎么处理」，没有依据，已删去：正文出现的规模数字只有方向 21 的 57,778 行与 53,710 行测试、方向 22 的 15,097 行，仓库总行数没有出现。

## 你可能会问

以下回答只用报告里已有的内容，报告没有的就写没有。

1. **18 项决定每一项的推荐和理由？** 报告只列问题，没有给推荐。能读出的倾向只有三处：第 13 项决定之前需要一条超过 400 bit 的实测（§三.12）；第 8 项的阈值缺噪声依据（§三.19）；第 1 项里 `GNFS_WEIGHT_CUTOFF` 与 `GNFS_DROP_RESIDUAL` 要先有十进制 50 位实测（§三.18）。
2. **第一梯队的开工顺序和工作量？要不要先跑基线？** 报告没有工作量估计。顺序上只说方向 2 单独合并、方向 7 与方向 9 一起或先后合并（§七）。报告全程只读，所有通过与否都是读码结论；方向 2 做完后要重新采集 120 bit 基线（§二.2）。
3. **每一处的「位」是十进制还是 bit？** 约定见「阅读约定·单位」。120 bit 门禁是二进制，§四.28 的筛区域分档是十进制位数。原稿写得不明的两处已改正：§四.25 的「17 位」是 17 bit，§四.28 的「27 位」是 27 bit。
4. **`exponent_k` 是什么？首项系数被 2 整除时有哪些办法？** `exponent_k` 是模 $\ell^k$ 里的 $k$，默认 8，见 §五表下说明；为什么考虑提高、为什么列入决定表，报告没有给出。首项系数被 2 整除时，报告只指出归一化这条修法覆盖不到它（§二.1「风险」），其余办法没有列。
5. **特征列记 0 的后果？修复后基线差多少？** 后果是依赖不保证对应平方（§二.2，据代码注释）。基线会差多少没有数据；重定基线具体要做哪些事，报告没有展开。
6. **能不能先统计出现频率和实际损失？** 报告把「实际损失都没有统计」列在 §六，§二.1 的建议是新增用例并与修复放同一个 PR；没有给出统计方案，也没有建议先后。
7. **方向 3 的修复形态怎么选？Block Wiedemann 那两处要不要一起修？** 两个选项是公开 launch guard 和新增批量提交接口（§二.3「风险」）。Block Wiedemann 那两处是否同一个缺陷，报告没有结论。
8. **「空输入返回 1」和「平方根失败是否跳过该依赖」各有哪些选项？** 前者：报告只说它是测试锁定的契约、生产路径在调用前已拦截空输入（§二.5），翻转的后果没有分析。后者：正文没有说明。
9. **「一半以上的代码」怎么算的？删掉能省多少？** 这句话已删，没有依据。规模数字只有 wave store 的 59 个文件 57,778 行加 53,710 行测试（§三.21），helper 头文件 15,097 行（§三.22）。能省多少 CI 时间，报告没有估计。
10. **CI 时长的主因到底是什么？提高 Windows 并行度的收益和风险？** 三种说法口径不同：§二.9 的 Windows 行是 `CI` 工作流内部的关键路径；§三.23 的 CodeQL 68.2 分钟是 main 上全部必需检查里最慢的一个；缓存不命中的原因没有调查（§六）。并行度 2 由提交 05195353 引入、依据不明，提高后的收益与风险报告没有评估，验证只能合并后在 main 上看。
11. **`restore-key` 命中后仍重编，下一步怎么排查？** 报告只把它列为方向 9 的待查项，原因没有调查（§六）。已知的只有：`restore-key` 是前缀命中，那次 main 只改了 2 个文件，编译记录有 344 条而对象总数未知（§二.9）。
12. **Windows 新跑的 28 项 fast 要多久？要不要先开 PR 试？** 耗时查不到，余量约 27 分钟（§三.15）。报告没有建议先试；类似情形下它说的是「只能看 PR 的 CI」（§二.7）。
13. **下一版本号？发布说明？macOS CLI 发布包要不要补就绪作业？** 报告没有建议。现状是 `v0.1.0` 被 `scripts/release_binary_contract.py:123` 硬锁（§三.16）；macOS CLI 发布包没有就绪作业，冒烟覆盖不到（§六）。
14. **十进制 50 位端到端走哪条路线？跑多久？nightly 容得下吗？要不要先试跑？** 报告说可以直接从 structured 路线起步（§四.24）。耗时无法从仓库证据判断，文档记 1,359 至 1,442 秒且不含求解。nightly 是每周五 17:00 UTC。报告建议在 `nightly.yml` 加独立作业，没有建议先试跑一次。
15. **影子证明、乘数组合、留出集 RSS 闸门、活捕获分别是什么？剩余 6 项？值不值？** 定义见 §三.20。剩余 6 项没有列出。值不值，报告没有结论，只列了两条顾虑：3 项要人工给内存预算与批准身份，靠 CI 完不成；自动探测预算只有 30 秒且没有测过加速。
16. **25 个候选、29 个方向、10 个角度怎么对应？核实是谁做的？复刻有没有运行？哪些行号是这一稿读过的？** 对应关系、核实者与方法、复刻是否运行，三项都未确认。行号：标〔复读〕的条目是本次修订读过的，其余不是。
17. **被更正的说法里，有没有已采纳的决定或已开工的事项需要撤回？** 报告没有追踪哪些已被采纳。能确定的只是若干上一轮建议已删去或改写：「比较依赖数」（§二.2）、「合并后再清理单例」（§三.18）、`bench/microbench`（§三.22）、Windows 工具闭包那一部分（§三.17）。
18. **能不能附两张对照表（测试与 CI 作业、各规模的选路与求解路径）？** 报告没有这两张表。前者只能从「阅读约定·CI 词汇」里的标签正则与必需检查表去推。后者的条件零散写在「阅读约定·求解路径」、§三.13（自动选路）和 §三.18（归约路线）里，没有按规模汇总。
