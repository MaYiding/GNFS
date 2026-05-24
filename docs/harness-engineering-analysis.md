# GNFS 项目 Harness 工程深度分析

> 本文剖析一个工业级 C++20 数论分解算法项目（GNFS — 通用数域筛法）如何围绕 Claude Code 构建出一套成熟的 AI 协作工程体系（Harness）。该体系涵盖指令系统、技能插件、钩子拦截、多 Agent 编排、Git Worktree 隔离并行、持久化文件追踪、测试自动化脚本群以及分层文档架构，共计 **2051 个 commit、304 个分支、51 个 Agent Worktree、62 个运行时调优开关**，是目前可观察到的最为精密的 AI 辅助开发工程实践之一。

---

## 一、Harness 全景：什么是 AI 协作工程体系

在传统的软件工程中，开发者通过 IDE、CI/CD 流水线、代码审查流程和项目管理工具来组织开发活动。而在 AI 辅助开发的范式下，**Harness（协作工程体系）** 指的是围绕 AI Agent 构建的一整套约束、引导、监控和自动化机制，使得 AI 能够像一个有纪律的工程师一样工作，而非一个随意的对话助手。

GNFS 项目的 Harness 体系可以分解为以下八个子系统：

**子系统一：指令系统** — CLAUDE.md 作为 AI 的"操作系统"，定义全局行为规范、铁律和决策框架。

**子系统二：技能插件** — Superpowers 插件提供 14 个结构化工作流技能，覆盖从头脑风暴到代码交付的完整生命周期。

**子系统三：钩子拦截** — Pre/Post Tool Use Hook 和 Stop Hook 在 AI 的操作边界处设置检查点，防止危险操作并确保任务完成。

**子系统四：多 Agent 编排** — 专职分工的 Agent 团队（架构师、探索员、开发者、审查员、找茬员、进度员）实现工程活动的分治与并行。

**子系统五：Worktree 隔离并行** — 51 个 Agent Worktree + 20 个手动 Worktree，实现真正的文件系统级并行开发隔离。

**子系统六：持久化文件体系** — 六个非 Git 追踪的持久化文件构成跨会话记忆网络，解决 AI 上下文窗口有限的根本问题。

**子系统七：测试自动化** — 2228 行的统一测试脚本 + 特性分支工作流 + 三路 CI 流水线，构成五级测试门禁。

**子系统八：分层文档架构** — CLAUDE.md 只留索引和铁律，4620 行 ENV 详细设计、3604 行算法 / 性能 / 计划文档下沉到 docs/ 目录。

---

## 二、指令系统：CLAUDE.md 的设计哲学

### 2.1 定位与规模

CLAUDE.md 是整个 Harness 的中枢神经，995 行，但它不是一份"项目介绍"，而是一份**操作手册**。它的设计遵循一个核心判据：

**写 CLAUDE.md 还是写 docs？—— 每次开发都要遵守的流程/约定/铁律写 CLAUDE.md；写某段代码/跑某个实验时才查的具体细节写 docs/。**

这个判据在 2026 年 5 月经历了一次大规模"瘦身重构"：原本 5402 行的 CLAUDE.md 被拆分，62 个 ENV 调优开关的详细设计文档（4620 行）外移到 `docs/env-flags/` 目录，CLAUDE.md 仅保留一句话速查表加链接。这一重构本身由 `task_plan.md` 记录并被 Stop Hook 监督完成。

### 2.2 内容结构

CLAUDE.md 的内容按优先级从高到低排列：

**第一层：构建与运行命令。** 提供 cmake/make/ctest 的完整命令模板和 `scripts/test.sh` 的全量用法参考，确保 AI 无需猜测如何编译和测试。

**第二层：架构总览。** 10 个代码模块（api, core, polynomial, factor_base, sieve, cofactor, relation, linalg, sqrt, util）的文件分布和职责说明，加上 GNFS 流水线的 8 个阶段描述。这让 AI 在修改代码前能快速定位影响范围。

**第三层：关键约定（Critical Conventions）。** 这是 CLAUDE.md 最有价值的部分。它记录的不是常识，而是**从真实 Bug 中提炼的反直觉铁律**。例如：

"元素表示约定为 a - b*alpha（而非 a + b*alpha）"——如果 AI 不知道这条约定，就会在符号上犯错，而这类错误在数论代码中极其隐蔽。

"Trim limit 必须含 LP cols"——这是一个 P1 级 Bug 模式，曾在 50-digit 分解中导致矩阵 NO_EXCESS 失败。CLAUDE.md 给出了精确的正确公式和 5 个受影响的测试入口点，并注明"matrix_cols * N 这个 pattern 在 lp_bits 大于等于 20 时永远是 BUG"。

"跨 bit-size 验证"——81-bit 测试通过不等于 164-bit 通过。CLAUDE.md 给出了具体的 lp_bits 行为差异数据（lp_bits=20 时 LP cols 占 5%，lp_bits=23 时占 64%，lp_bits=26 时占 70%+），并要求任何 size-sensitive 代码修改后必须在至少 3 个 size band 上回归测试。

"跨平台编译注意事项"——macOS libc++ 隐式拉头文件但 Linux libstdc++ 严格、Linux LP64 下 int64_t 等于 long 而 macOS 下等于 long long、Release 模式优化掉未定义行为等。这些都是 CI 上反复失败后总结的经验。

**第四层：ENV 调优开关速查表。** 62 个 `GNFS_*` 运行时开关的一句话速查，按 8 个模块分组，每个条目带锚点链接到 `docs/env-flags/` 中的详细设计。

**第五层：工程规范。** Git 规范（分支命名、自动提交策略、合并规则）、后台任务管理规范（nohup + 文件日志、禁止 sleep 轮询）、工作流规范（任务前列计划、两级 commit + 检查点机制）等。

**第六层：Agent 团队协作编排。** 六个角色的定义、协作流程和防怠工条例。

### 2.3 设计亮点

**铁律的表达方式。** CLAUDE.md 中的铁律不是"请注意 xxx"式的建议，而是"xxx 永远是 BUG"式的判定。例如"matrix_cols * N 这个 pattern 在 lp_bits>=20 size 下永远是 BUG"，"81-bit 测试 PASS 不等于 164-bit PASS"。这种表达方式消除了 AI 的模糊判断空间。

**从失败中沉淀。** 几乎每条铁律后面都跟着具体的 commit hash、PID 编号和实测数据。例如"V2 在 50d 大失败（revert commit 9e84a73）：25d V2 +27% Merged 骗过去，50d V2 Round 1 Merged 6786 降到 2088（-69%），sngl 436 飙升到 21539（x49）"。这让铁律具有不可辩驳的实证基础。

**授权与约束并存。** CLAUDE.md 明确授权 AI "在每一小步改动后自动提交，无需额外确认"、"BACKLOG 中已记录的问题可直接修复"、"每个大阶段完成后自动 push"。同时用"禁止行为"列表约束危险操作。这种"宽进严出"的设计让 AI 能高效工作而不会畏首畏尾。

---

## 三、Superpowers 技能插件系统

### 3.1 插件架构

Superpowers 是一个 Claude Code 第三方插件（版本 5.1.0），提供 14 个结构化工作流技能。每个技能以 Markdown 文件的形式定义，包含名称、描述、触发条件和详细的执行流程（通常用 Graphviz DOT 语法绘制的决策流程图）。

技能不是简单的提示词模板，而是**强制执行的工程纪律**。技能系统的核心规则是："如果你认为有哪怕 1% 的可能性某个技能适用于当前任务，你必须调用它。"

### 3.2 技能全景

14 个技能覆盖了软件开发全生命周期，按功能可分为四组：

**需求与设计阶段：**

brainstorming（头脑风暴）——在任何创造性工作之前强制执行。流程为：探索项目上下文、提供视觉伴侣（如涉及视觉问题）、逐个提出澄清问题、提出 2-3 个方案及其权衡、呈现设计并获得用户批准、写入设计文档到 `docs/superpowers/specs/`、规格自审、用户审查规格、过渡到实现。硬门禁："在呈现设计并获得用户批准之前，不得调用任何实现技能、写任何代码。"

writing-plans（编写计划）——将规格转化为详细的实现计划。每个步骤是 2-5 分钟的单一操作（"写失败的测试"、"运行它确认失败"、"写最小实现代码"、"运行测试确认通过"、"提交"）。计划保存到 `docs/superpowers/plans/YYYY-MM-DD-<feature-name>.md`。

**实现阶段：**

test-driven-development（测试驱动开发）——铁律："没有失败的测试就不能写产品代码。"如果先写了代码再写测试，规则是"删除代码，从头来过。不要保留它作为参考，不要adapt它，不要看它。删除就是删除。"

subagent-driven-development（子 Agent 驱动开发）——为每个任务分派一个全新的子 Agent（不继承父会话的上下文），完成后进行两阶段审查（规格合规审查 + 代码质量审查）。持续执行原则："不要在任务之间暂停向用户确认。唯一停止的理由是：无法解决的阻塞、阻止进展的歧义、或所有任务完成。"

executing-plans（执行计划）——当没有子 Agent 支持时的备选方案。加载计划、批判性审查、逐任务执行。

using-git-worktrees（使用 Git Worktree）——确保工作在隔离的工作空间中进行。先检测是否已在隔离工作空间（避免重复创建），优先使用平台原生 worktree 工具（如 Claude Code 的 EnterWorktree），否则回退到手动 `git worktree add`。

**质量保障阶段：**

systematic-debugging（系统化调试）——铁律："没有根因调查就不能修复。"四阶段流程：根因调查、假设形成、假设验证、修复实施。特别强调在时间压力下更要系统化："紧急情况让猜测变得诱人，但系统化比乱试更快。"

requesting-code-review（请求代码审查）——在完成任务、实现主要功能或合并前分派代码审查子 Agent。审查者获得精心构造的上下文（BASE_SHA 到 HEAD_SHA 的 diff），而非整个会话历史。

receiving-code-review（接收代码审查）——处理收到的审查反馈。

verification-before-completion（完成前验证）——铁律："没有新鲜的验证证据就不能声称工作完成。"如果在本消息中没有运行验证命令，就不能声称它通过。禁止使用"应该"、"可能"、"似乎"等模糊措辞。

**交付阶段：**

finishing-a-development-branch（完成开发分支）——验证测试通过、检测工作空间状态、呈现选项（合并/PR/保留/丢弃）、执行选择、清理 worktree。

dispatching-parallel-agents（分派并行 Agent）——当面对 2 个以上独立问题时，为每个问题域分派一个 Agent 并行工作。核心原则："每个问题域一个 Agent，让它们并发执行。"

using-superpowers（使用技能）——元技能，定义如何发现和使用其他技能。包含"红旗思维"清单：如果你产生了"这只是一个简单问题"、"我先了解一下代码库"、"这个技能太大了"等想法，说明你正在合理化逃避使用技能。

writing-skills（编写技能）——定义如何创建新的技能。

### 3.3 技能与 CLAUDE.md 的协作

CLAUDE.md 的工作流规范（"任务前必须列计划"、"代码变更后必须检查"）与 Superpowers 技能（writing-plans、verification-before-completion）形成了双重保障。CLAUDE.md 定义了"做什么"，技能定义了"怎么做"。当两者冲突时，CLAUDE.md 中用户的显式指令优先级最高。

---

## 四、钩子拦截系统

### 4.1 Pre/Post Tool Use Hook

项目在 `settings.local.json` 中配置了三类钩子：

**PreToolUse Hook（编辑前拦截）。** 当 AI 调用 Edit 工具时，检查目标文件路径。如果目标是 `.sh` 脚本文件、`build/` 目录中的生成文件或 `CMakeFiles/` 目录中的 CMake 生成文件，输出 BLOCK 消息阻止编辑。这防止了 AI 修改不应修改的文件。

**PostToolUse Hook（编辑后提醒）。** 当 AI 编辑了 `.hpp` 或 `.cpp` 文件后，提醒"修改了 C++ 文件，记得运行 make 验证编译"。当编辑了 `CMakeLists.txt` 后，提醒需要重新运行 cmake。当 Write 创建了新的 `.hpp` 或 `.cpp` 文件后，提醒可能需要在 CMakeLists.txt 中添加。

这些钩子的 timeout 设为 5 秒，确保不会拖慢 AI 的工作节奏。

### 4.2 Stop Hook（完成拦截）

这是整个 Hook 系统中最精巧的部分。`stop-todo-check.sh` 脚本在 AI 尝试结束对话时执行以下逻辑：

**第一步：** 检查 `stop_hook_active` 标志（防循环守卫）。如果本次停止尝试已被阻止过一次，则允许停止，防止无限循环。

**第二步：** 查找项目根目录的 `TODO.md` 文件。如果不存在或为空，允许停止。

**第三步：** 解析 TODO.md 中的任务列表。识别 `- [ ]`（未完成）和 `- [x]`（已完成）格式，包括编号列表 `1. [ ]`。

**第四步：** 如果有未完成项，输出 JSON 决策 `{"decision": "block", "reason": "..."}` 阻止停止，并在原因中列出未完成的条目，要求 AI 先使用 `/compact` 压缩上下文然后继续工作。

这个设计的精妙之处在于：它将"用户写的待办"（TODO.md）与"AI 自己发现的待办"（BACKLOG.md）和"AI 的执行计划"（task_plan.md）区分开来，Stop Hook 仅检查 TODO.md。这意味着用户拥有对 AI 行为的最终控制权——只要用户在 TODO.md 中写下任务，AI 就无法在完成任务之前结束对话。

---

## 五、多 Agent 编排体系

### 5.1 项目自定义 Agent

项目在 `.claude/agents/` 目录下定义了两个领域专用 Agent：

**gnfs-debugger（GNFS 调试器）。** 预置了 GNFS 代码库中常见的故障模式知识：平方根阶段失败（product 同余 0 mod N、无有效符号模式、全因子为平凡因子）、线性代数问题（空核空间、Block Lanczos 发散、错误依赖关系）、筛法问题（关系太少、范数错误、重复关系）。定义了调试流程：复现、隔离（哪个 GNFS 阶段失败）、追踪值（在阶段边界打印中间 Integer 值）、手工验证数学（对小 case 验证）、对比参考（用 N=143 作为基准）。

**gnfs-reviewer（GNFS 代码审查器）。** 预置了领域知识审查清单：数学正确性（符号约定 a - b*alpha、范数计算、模运算溢出、Schirokauer maps 只用 l=2）、内存与性能（无不必要的 Integer 拷贝、内循环避免内存分配、GF(2) 矩阵使用 word-packed PackedGF2Matrix）、C++20 模式（std::optional、std::span、RAII）、构建系统（新 .cpp 文件加入 CMakeLists.txt）。

### 5.2 CLAUDE.md 定义的 Agent 团队

CLAUDE.md 在更高的层面定义了六个角色：

**架构师**（feature-dev:code-architect）——分析需求，设计实现方案，确定文件结构和数据流。在编码之前触发。

**探索员**（Explore）——检索代码库，定位相关文件，理解现有模式和依赖。搜索广度分三级：quick（单次定向查找）、medium（适度探索）、very thorough（跨多个位置和命名约定搜索）。

**开发者**（feature-dev:code-explorer + 主线程编码）——理解现有实现细节，执行编码任务。

**审查员**（feature-dev:code-reviewer）——检查代码质量、逻辑正确性、是否符合项目约定。每个大阶段完成后触发。

**找茬员**（pr-review-toolkit:silent-failure-hunter）——专门查找静默失败、错误吞没、边界遗漏、极端情况。在审查员通过后进行二次检查。

**进度员**（主线程自身）——维护 progress.md，跟踪计划执行状态。

### 5.3 Superpowers 的子 Agent 模式

Superpowers 插件在技能层面进一步定义了子 Agent 的使用模式：

**subagent-driven-development** 技能要求为每个任务分派一个全新的子 Agent（隔离上下文），完成后进行两阶段审查（规格合规 + 代码质量），形成"实现-审查-修复"的闭环。

**dispatching-parallel-agents** 技能要求当面对 2 个以上独立问题时并行分派 Agent，每个 Agent 获得精心构造的上下文（不继承父会话历史）。

### 5.4 团队规模与并行度的实证

从 Git 数据可以看到 Agent 团队的实际规模：

**51 个 Agent Worktree**（`.claude/worktrees/agent-*`），每个对应一个 Agent 独立工作空间，分支名如 `feat/260522-filter-lp-bloom`、`feat/260523-mpz-mul-parallel`、`feat/260521-simd-spmv-kernels` 等。这些 Worktree 全部标记为 locked，表明 Agent 完成后保留了工作现场供后续审查或合并。

**20 个手动 Worktree**（`.worktrees/`），用于更大型的特性开发，如 `metal-spmv`（Metal GPU 加速 SpMV）、`distributed-sieve`（分布式筛法）、`couveignes`（改进 Couveignes 算法）等。

**304 个分支**，分支名格式 `<type>/<YYMMDD>-<description>`（如 `feat/260523-brent-rho-parallel`），保留了完整的开发历史。

---

## 六、Worktree 隔离并行

### 6.1 双层 Worktree 架构

项目采用双层 Worktree 结构：

**第一层：`.claude/worktrees/`（Agent 自动管理）。** 由 Claude Code 的 Worktree 工具自动创建和管理。每个子 Agent 在隔离的 Worktree 中工作，完成后 Worktree 被锁定（locked）。主线程可以在审查后决定是否合并。这一层的 51 个 Worktree 主要用于实施 62 个 ENV 调优开关——每个开关一个 Worktree，互不干扰。

**第二层：`.worktrees/`（手动管理）。** 由开发者手动创建，用于更大型的特性开发。每个 Worktree 对应一个独立的特性分支，如 `sieve-simd`（筛法 SIMD 内核）、`mmap-phase5`（MmapCSRMatrix Phase 5 集成）、`ecm-stage3`（ECM Stage 3 实现）等。

### 6.2 为什么需要 Worktree 隔离

GNFS 项目的代码改动有两个特点使得 Worktree 隔离变得必要：

**第一，ENV 开关之间正交。** 62 个开关分布在 8 个模块中，每个开关的实现、测试和文档是独立的。如果在同一分支上串行实现，任何一个开关的编译失败都会阻塞其他开关的开发。Worktree 隔离允许 51 个 Agent 同时工作在不同开关上。

**第二，大型特性改动涉及多文件。** 如"改进 Couveignes 算法"可能涉及 sqrt 模块的头文件、源文件和测试文件的联动修改。在 Worktree 中开发可以确保 main 分支始终保持可编译状态（CLAUDE.md 的核心原则之一）。

### 6.3 Superpowers 的 Worktree 技能

`using-git-worktrees` 技能定义了标准化的 Worktree 工作流：先检测是否已在隔离工作空间（避免重复创建），优先使用平台原生工具（Claude Code 的 EnterWorktree），否则回退到 `git worktree add`。`finishing-a-development-branch` 技能在分支完成时处理合并和清理。

---

## 七、持久化文件体系

### 7.1 设计动机

AI Agent 的上下文窗口是有限的（通常数万到数十万 token），一个复杂的 GNFS 调试会话可能涉及数百个文件读取、数十次编译测试和大量的分析推理，很容易逼近上下文极限。更关键的是，跨会话的信息传递完全依赖持久化文件——新的对话会话对前一次会话没有任何记忆。

GNFS 项目通过六个非 Git 追踪的持久化文件（在 `.gitignore` 中排除）构建了一个跨会话记忆网络：

### 7.2 文件清单与职责

**TODO.md（22 行）** — 用户手写的待办清单。Stop Hook 监督的唯一文件。用户在这里写下任务，AI 必须完成所有任务才能结束对话。它与 AI 自动生成的文件互不干扰。

**task_plan.md（30 行）** — AI 自动生成的详细开发计划。当前任务是"CLAUDE.md 瘦身重构"，包含 6 个阶段和明确的拆分映射（CLAUDE.md 的哪些行搬到哪个文件）。

**progress.md（1204 行）** — 实时进度记录。记录了每个子阶段的详细执行结果，包括具体的测试数据（如"Round 5: usable 281072, beta=121.4%, Matrix 281072x364008 NO EXCESS"）、commit 列表（如"本子阶段 ~11 commits @ 11:30-12:00"）和关键洞察（如"lp_bits 微调对 50d 无收益"）。这是最厚的持久化文件，承载了大量的实证数据。

**findings.md（119 行）** — 调查发现与技术决策。例如"50d/60d FAIL 调查"记录了完整的失败现象、根因分析（filter.hpp 只处理 weight=2 LP key，weight 大于等于 3 的关系整批丢弃）、修复方案（V1 扩展 Phase 2 merge 到 weight-K）和风险评估。

**BACKLOG.md（381 行）** — 待办备忘录。按严重程度分级（P1/P1-OPT/P2/P3/TEST），每个条目包含发现日期、文件位置、描述和建议。当前有 11 个条目，其中 8 个已标记 RESOLVED，2 个 trigger-pending（等待实测条件激活），1 个 CI 基础设施限制无法执行。

**RESOLVED.md（1414 行）** — 已完成修复的审计记录。每个关闭的条目必须包含：修复代码已 commit、相关测试通过、无副作用。例如"50d beta plateau 121%"的关闭记录包含完整的 13 步 timeline，每步有 commit hash 和验证方式。

### 7.3 文件间的协作关系

六个文件形成了一个精密的协作网络：

task_plan.md 定义"要做什么"，progress.md 记录"做到了哪里"，findings.md 记录"发现了什么"。当 AI 在 progress.md 中发现了一个计划外的问题时，写入 BACKLOG.md。当 BACKLOG.md 中的问题被修复后，从 BACKLOG 移到 RESOLVED.md。TODO.md 独立于这个网络，由用户直接控制。

**BACKLOG 到 RESOLVED 的关闭流程** 被严格定义：代码已合入、测试已通过、无副作用——三个条件缺一不可。禁止"不验证就关闭"、"批量静默关闭"、"打乱排序"等行为。

### 7.4 为什么这些文件不入 Git

这是一个精妙的设计决策。这些文件是"会话级持久化"——它们对当前开发活动有价值，但不应该成为项目历史的一部分。如果入 Git，每次 AI 更新 progress.md 都会产生一个无意义的 commit，污染 Git 历史。通过 `.gitignore` 排除，这些文件可以自由更新而不影响 Git 的清洁性。

---

## 八、测试自动化体系

### 8.1 核心脚本：test.sh

`scripts/test.sh` 是整个测试体系的中枢，2228 行 zsh 脚本。它封装了编译、超时保护、测试执行、结果报告和 JSON 输出的完整逻辑。

**测试分级体系。** 所有测试按执行时间分为五级：

instant（10 秒超时）：39 个纯单元测试，如 test_integer、test_linalg、test_sqrt。这是冒烟测试（smoke）的全部组成，约 5 秒完成。

fast（60 秒超时）：test_sieve_basic 等。

slow（120-300 秒超时）：test_regression_gate、test_kleinjung、test_lattice_sieve、test_gnfs_e2e。这是合并门禁（gate）的组成部分，约 9-18 秒（Release 模式）。

heavy（600-3600 秒超时）：test_kleinjung_large、test_gnfs_progressive、test_25digit。用于渐进测试和性能基准。

stress（43200 秒超时）：test_stress（50-digit 和 60-digit 分解），用于极限压力测试，耗时数小时。

**16 种运行模式。** 从最常用的冒烟测试（`./scripts/test.sh`，约 5 秒）到最全面的夜间测试（`./scripts/test.sh nightly`，包含 L4+L5 渐进测试），覆盖了日常开发、模块改动、核心改动、PR 前验证、性能基准等所有场景。

**智能测试选择。** `changed` 模式根据 `git diff` 自动检测受影响的模块并运行对应测试；`changed --deep` 还会级联运行依赖模块的测试。

**CJK 显示宽度处理。** 脚本中定义了 `display_width()` 函数来正确处理中文字符的终端列宽（CJK 字符占 2 列，而 `${#str}` 只计字符数），确保框线输出对齐。这个细节体现了脚本的工程成熟度。

### 8.2 特性分支工作流：feature-branch.sh

`scripts/feature-branch.sh`（295 行）封装了特性分支的完整生命周期：

创建分支（`create <type> <desc>`）——自动生成分支名（如 `feat/260315-bucket-sieve`），从 main 切出。

门禁验证（`gate`）——委托给 test.sh gate，确保分支可以安全合并。

合并到 main（`merge`）——自动运行门禁 + `--no-ff` merge，保留分支合并记录。

状态查看（`status`/`list`）——分支概览和列表。

### 8.3 CI 流水线

项目配置了三路 CI 流水线：

**ci.yml** — 主 CI，在四个平台上构建和测试：Linux x86_64（gcc，必须通过）、macOS arm64（clang，必须通过）、Linux arm64（gcc，实验性）、Windows x86_64（MSVC + vcpkg，实验性）。排除 slow/heavy/stress 标签的测试（CI runner 跑不动），超时 240 秒。

**sanitizers.yml** — AddressSanitizer + UBSanitizer 和 ThreadSanitizer 两路检查。仅运行 instant/fast 测试（Sanitizer 使执行慢 5-10 倍）。

**codeql.yml** — 代码安全扫描。

### 8.4 性能调优脚本群

`scripts/` 目录下还有一组性能调优脚本：

**sweep_lp_bits.sh** — 在指定 digit 下对多个 lp_bits 值并行启动 test_stress（每个值一个 nohup 后台进程），用于 BACKLOG #5（60d lp_bits 25 vs 26 实测比较）。

**sweep_combo.sh** — 2x2 ENV 组合扫描，比较两个 ENV 开关的交叉效应（additive/synergy/cancel）。

**sweep_full.sh / sweep_analyse.py** — 全量参数扫描和分析。

**v3_roi_estimator.sh** — 给定 V0 baseline 日志，估算 V3 cascade 是否值得启用（输出 merge_rate、matrix_cols、needed raw、wall time estimate）。

**perf/ 子目录** — 包含 `pmu-stat.sh`（PMU 硬件计数器采集）、`profile-cpu.sh`（CPU profiling）、`parse-trace.py`（解析 Instruments trace）、`pmu-derive.py`（从 PMU 原始数据推导派生指标）等底层性能分析工具。

---

## 九、分层文档架构

### 9.1 三层文档结构

项目的文档按信息密度和查阅频率分为三层：

**第一层：CLAUDE.md（995 行）。** 每次对话自动加载。只包含全局操作规范、关键铁律和速查索引。

**第二层：docs/env-flags/（4620 行，8 个模块文件 + 1 个 README）。** 每个 ENV 开关的详细设计文档，遵循统一模板：标题加一句话定位、用法示例（bash）、Helper API/算法、ENV 解析规则、Bit-for-bit guarantee（开关不影响正确性只影响性能）、ROI 与定位、集成点（文件 + commit）、测试清单、Default 行为。

**第三层：docs/plans/（6 个设计文档）+ docs/algorithms/（1 个算法设计）+ docs/perf/（6 个性能优化方案）。** 包括 GNFS 总体设计文档（1309 行）、SIQS 备选算法设计、性能教义（performance-doctrine.md，1374 行）、V3 cascade 设计、全流水线恢复设计等。

### 9.2 文档化规范

CLAUDE.md 定义了严格的文档化规范（强制执行）：

新增 `GNFS_*` 开关时，必须完成四步：详细文档写到 `docs/env-flags/<module>.md`、在 CLAUDE.md 速查表加一行、遵循统一模板、若属 Parallel Dispatcher/SIMD family 则同步更新 README 的 family 表。

这个规范确保了文档与代码的同步演进——不是"写完代码再补文档"，而是"文档是开关实现的一部分"。

### 9.3 写作规范

`docs/writing-style-guide.md`（138 行）定义了文档的写作标准，覆盖 GitHub Markdown 渲染规范（标题层级、代码块语言标识、表格对齐、链接格式）、中英文混排规范（中英文之间加半角空格、中文与数字之间加空格）、正规英语写作规范（避免破折号插入语、Oxford comma 必须保留、避免缩约形式）等。

---

## 十、Git 工作流工程

### 10.1 分支命名与生命周期

分支名格式 `<type>/<YYMMDD>-<description>`（如 `feat/260523-brent-rho-parallel`），类型包括 feat、fix、perf、refactor、exp。日期前缀使得分支的时间线一目了然。

核心原则：**main 必须始终可编译、可测试通过。** 任何可能破坏编译的工作都必须在新分支上进行。

合并后分支保留不删除——从 Git 数据可以看到 304 个分支完整保留，形成了一棵详细的开发树。

### 10.2 自动提交策略

CLAUDE.md 授权 AI "在每一小步改动后自动提交"。从 Git 历史可以看到这一策略的实际效果：最近的 30 个 commit 中，每个 commit 都是一个独立的、原子性的改动（如"feat(sieve): add uint8 saturated subtract SIMD helper"、"test(cofactor): cover Brent-Pollard rho parallel dispatcher"），commit message 遵循 Conventional Commits 规范。

**一 Bug 一 Commit（强制）。** 每个独立 Bug 修复必须是单独的 commit。唯一例外是两个 Bug 的修复代码在物理上交织。这条规则确保了 `git diff` 能精确定位每个变更的目的。

### 10.3 自动 Push 策略

"这是非公开私有仓库，Claude Code 被授权在每个大阶段完成后自动 push。"这条授权消除了 AI 对 push 操作的犹豫，确保远程仓库始终有最新备份。

---

## 十一、后台任务管理

GNFS 的压力测试（50-digit/60-digit 分解）可能耗时数小时甚至数十小时。CLAUDE.md 定义了一套后台任务管理规范：

**架构原则：** 后台任务用 `nohup` + 文件日志运行，不阻塞主线程。主线程继续做其他优化工作，按需查日志。

**启动规则：** 用 `nohup` 而非 `run_in_background`（后者无法可靠取消）；记录到 `/tmp/bg_tasks.txt`（PID、日志路径、启动时间）；代码中必须加 `std::flush`（nohup 全缓冲）。

**监控规则（严格）：** 禁止堆叠多个 `sleep && tail` 后台任务、禁止 `run_in_background` 启动 sleep 循环、禁止 sleep 超过 300 秒、一次最多一个后台 sleep。

这套规范解决了 AI Agent 在长时间运行任务中的典型问题：进程泄漏、上下文膨胀、任务混乱。

---

## 十二、关键设计理念总结

### 12.1 约束即赋能

整个 Harness 体系表面上是在"限制"AI 的行为——不能跳过测试、不能不验证就声称完成、不能修改特定文件、不能在有未完成 TODO 时结束。但这些约束实际上是在**赋能**：它们消除了 AI 的决策模糊性，让 AI 知道什么是正确的做法，从而能够自信地、高效地执行复杂任务。

### 12.2 失败驱动的知识沉淀

CLAUDE.md 中的铁律、BACKLOG.md 中的问题记录、RESOLVED.md 中的修复审计、findings.md 中的调查发现——这些文件的内容几乎全部来自真实的失败案例。每一个"永远不要"后面都跟着一个具体的 commit hash 和实测数据。这种"从失败中提炼知识，用知识预防失败"的循环是 Harness 体系最核心的价值。

### 12.3 人机分工的精确边界

在这个 Harness 中，人和 AI 的分工非常清晰：

**人负责：** 定义需求（TODO.md）、审核设计（brainstorming 批准）、审查代码（code review）、做最终决策（选择实现方案）。

**AI 负责：** 执行计划（subagent-driven-development）、发现问题（BACKLOG.md）、记录进度（progress.md）、自动化测试（test.sh）、维护文档（docs/env-flags/）。

Stop Hook 是这一分工的制度保障——它确保 AI 不会在人没有审核的情况下自行宣布任务完成。

### 12.4 并行度的极致追求

从 51 个 Agent Worktree + 20 个手动 Worktree 的规模可以看出，这个项目在并行度上做到了极致。62 个 ENV 调优开关几乎每个都有独立的 Worktree 和分支，多个 Agent 同时工作在不同的开关实现上。这种并行能力得益于三个前提：开关之间的正交性（每个开关独立不影响其他）、Worktree 的文件系统隔离（不会互相干扰）、以及统一测试脚本（每个 Worktree 可以独立验证）。

### 12.5 持久化是对抗遗忘的唯一手段

AI Agent 没有长期记忆，每次新会话都是从零开始。六个持久化文件（总计 3170 行）构成了项目的"外部记忆"，使得跨会话的连续工作成为可能。progress.md 的 1204 行详细记录意味着任何一次新会话都可以通过读取这个文件快速恢复到上一次的进度。RESOLVED.md 的 1414 行审计记录则防止了"重新修复已经修复过的问题"这种浪费。

### 12.6 数据规模汇总

整个 Harness 体系的关键数据：

CLAUDE.md 指令文件 995 行，62 个 ENV 调优开关速查表。docs/env-flags/ 详细设计文档 4620 行。docs/plans + algorithms + perf 设计文档 3604 行。writing-style-guide 138 行。scripts/test.sh 测试脚本 2228 行。scripts/feature-branch.sh 295 行。性能调优脚本群 8 个。持久化文件 3170 行。CI 流水线 3 路。Superpowers 技能 14 个。自定义 Agent 2 个（debugger + reviewer）。Hook 3 个（PreToolUse + PostToolUse + Stop）。Agent Worktree 51 个。手动 Worktree 20 个。分支 304 个。commit 2051 个。测试二进制 80+。

---

## 十三、可复用的 Harness 模式

从 GNFS 项目的实践中可以提炼出以下可复用的 Harness 模式：

**模式一：指令瘦身。** CLAUDE.md 只留铁律和索引，详细设计下沉到 docs/。判断标准：每次开发都要遵守的写 CLAUDE.md，某次开发才查的写 docs/。

**模式二：Stop Hook 监督。** 用 TODO.md + Stop Hook 确保 AI 完成任务才能结束。用户通过 TODO.md 控制 AI 的任务清单。

**模式三：持久化文件不入 Git。** task_plan.md、progress.md、BACKLOG.md 等会话级文件通过 .gitignore 排除，允许自由更新而不污染 Git 历史。

**模式四：Worktree 级并行。** 正交的特性（如 ENV 开关）各自在独立 Worktree 中实现，多个 Agent 同时工作互不干扰。

**模式五：测试分级 + 统一脚本。** 按执行时间分级（instant/fast/slow/heavy/stress），统一入口脚本封装编译+超时+报告，让 AI（和人）无需记住复杂的 ctest 参数。

**模式六：失败驱动铁律。** 每条铁律都有具体的 commit hash 和实测数据作为证据，从真实 Bug 中提炼而非理论推导。

**模式七：授权与约束并存。** 明确授权 AI "自动提交"、"自动 push"、"直接修复 BACKLOG"，同时用禁止行为列表约束危险操作。

**模式八：文档化规范强制执行。** 新增功能时必须同步更新文档（不是"以后再补"），文档是功能实现的一部分。

这些模式不依赖于 GNFS 项目的特定领域，可以应用于任何使用 Claude Code 进行开发的软件项目。
