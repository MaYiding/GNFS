# Harness 工程应用实例汇报

> **以个人练习项目为例：如何让 AI Agent 成为有纪律的软件工程师**

---

## 引言：核心挑战

当 AI Agent 从对话助手升级为软件工程师时，我们面临一个根本性问题：**AI 拥有极强的编码能力，但缺乏工程纪律。**

在实际开发过程中，我观察到了六个反复出现的失败模式：

**失败模式一：半途而废。** AI 在上下文窗口耗尽或对话自然结束时丢失全部进度，新会话从零开始。

**失败模式二：虚假完成。** AI 在没有运行测试的情况下声称"应该没问题"、"代码已修复"。数论代码中的符号约定错误在代码审查中极难发现，只有端到端测试才能暴露。

**失败模式三：知识蒸发。** AI 在调试中发现的这关键知识会随着会话结束而消失。下次遇到同样的问题时，AI 会重新花数小时走完同样的调查路径。

**失败模式四：串行低效。** 项目有几十个个相互正交的调优开关需要实现。AI 逐个串行实现时，任何一个开关的编译失败都阻塞后续所有工作。

**失败模式五：测试混乱。** AI 不知道该跑哪些测试，表现为跑太少遗漏回归，跑太多浪费时间。

**失败模式六：危险操作。** AI 执行 `git push --force`、修改构建产物、跳过 pre-commit hooks 等不可逆操作。

**本报告以 GNFS（通用数域筛法）项目为案例，展示 Harness 工程如何通过八大机制系统性地解决上述问题。**

| 失败模式 | 主解决机制 | 辅助机制 |
|----------|-----------|---------|
| 半途而废 | 持久化文件网络（§3） | Hook 拦截（§4） |
| 虚假完成 | Superpowers 技能（§5） | Hook 拦截（§4） |
| 知识蒸发 | 持久化文件网络（§3） | CLAUDE.md 指令（§2） |
| 串行低效 | 多 Agent 编排（§6） | Git 工作流（§9） |
| 测试混乱 | 测试自动化（§7） | 分层文档（§8） |
| 危险操作 | Hook 拦截（§4） | CLAUDE.md 指令（§2） |

---

## 一、项目概况

GNFS 是一个工业级的 C++20 数论分解算法实现。选择这个项目作为案例，是因为它集中体现了 AI 辅助开发的多个难点：

**算法深度。** 完整的 GNFS 流水线包含 8 个阶段（多项式选择、因子基构建、筛法、余因子分解、关系收集、线性代数、平方根、GCD），每个阶段涉及深层数论知识。

**代码规模。** 61 个头文件、14 个源文件、63 个测试文件，分布在 10 个功能模块中。

**调优空间。** 62 个 `GNFS_*` 运行时环境变量开关，覆盖 SIMD 内核、并行 dispatcher、缓存/内存池等维度。每个开关保证 bit-for-bit 一致性——开关状态不影响正确性，只影响性能。

**测试复杂度。** 80+ 个测试二进制，执行时间跨越 5 个数量级（0.1 秒到 12 小时）。

**开发规模数据：**

| 维度 | 数据 |
|------|------|
| 总 commit 数 | 2051 |
| 分支数（保留不删除） | 304 |
| Agent Worktree（自动管理） | 51 |
| 手动 Worktree | 20 |
| CI 流水线 | 3 路（4 平台 + Sanitizers + CodeQL） |
| ENV 调优开关 | 62 个 |
| 测试二进制 | 80+ |

---

## 二、Harness 体系架构总览

Harness 工程体系由八大机制组成，按解决的问题分为三个层次：

**核心层 —— 解决"能不能干活"的问题。** 包含 CLAUDE.md 指令系统（§2）、持久化文件网络（§3）和 Hook 拦截系统（§4）。这三个机制确保 AI 能够跨会话连续工作、不丢失进度、不做危险操作。**如果只有三个机制可以用，应该选这三个。**

**质量层 —— 解决"活干得好不好"的问题。** 包含 Superpowers 技能系统（§5）、多 Agent 协作编排（§6）和测试自动化体系（§7）。这三个机制确保 AI 遵循工程纪律、代码经过审查和验证、测试覆盖充分。

**效率层 —— 解决"活干得快不快"的问题。** 包含分层文档架构（§8）和 Git 工作流工程（§9）。这两个机制降低信息检索成本、提高版本管理的规范性和可追溯性。

八个机制不是独立的——它们通过共享的持久化文件和 Git 历史形成了一个紧密耦合的体系。例如：Stop Hook 检查 TODO.md（持久化文件 × Hook），CI 流水线读取 CMakeLists.txt 中的测试标签（测试自动化 × Git），CLAUDE.md 中的铁律引用具体的 commit hash（指令 × Git）。

---

## 核心层：让 AI 能干活

> 以下三个机制是 Harness 体系的地基。没有它们，AI 无法跨会话工作、无法保持进度、无法避免危险操作。

---

## 三、CLAUDE.md 指令系统

**解决的问题：** AI 每次会话从零开始，不知道项目的构建方式、测试方法、代码约定和历史教训。

**方案：** CLAUDE.md 文件作为 AI 的"操作手册"，在每次会话开始时由 Claude Code 自动加载。

### 3.1 设计哲学

CLAUDE.md 不是项目介绍文档，而是一份操作性极强的工程规范。它的设计遵循一个核心判据：

**每次开发都要遵守的流程/约定/铁律 → 写 CLAUDE.md；某次开发才查的具体细节 → 写 docs/。**

### 3.2 内容层次

**第一层：构建与运行命令（~30 行）。** 提供 cmake/make/ctest 的完整命令和 `scripts/test.sh` 的全量用法。AI 拿到这一层就可以立即开始编译和测试，无需猜测。

**第二层：架构总览（~30 行）。** 10 个模块的文件分布和职责：

**第三层：关键约定与铁律（~120 行）。** 从真实 Bug 中提炼的反直觉规则。

**第四层：ENV 调优开关速查表（~80 行）。** 62 个开关的一句话速查，每个条目带锚点链接到 `docs/env-flags/` 中的详细设计。

**第五层：工程规范（~400 行）。** Git 规范、后台任务管理、工作流规范、Agent 团队协作编排等。

**第六层：项目文件结构、Error Handling 约定、Known Limitations（~200 行）。**

### 3.3 核心示例：铁律的表达方式

CLAUDE.md 中的铁律不是"请注意 xxx"式的建议，而是"xxx 永远是 BUG"式的判定。每条铁律后面都跟着具体的 commit hash 和实测数据。

**示例一：跨规模验证铁律。** 数论算法的一个反直觉特性是小规模测试通过不代表大规模能通过：

```markdown
## 跨 bit-size 验证 (小 case PASS ≠ 大 case PASS)

**典型陷阱**:
1. 算法修改在 25-digit PASS, 在 50-digit 失败 (commit 21dcbcd):
   V2 让 weight≥2 都 merge, 25d Merged +27% (好看),
   50d Merged -69% + sngl ×49 (灾难).

**铁律**:
- 改 filter/merge/sieve 等 size-sensitive 代码后,
  必须 reg-test 至少 3 个 size band:
  81-bit (25d), 164-bit (50d 至少 1 Round), 100-150 bit.
```

**示例二：跨平台编译铁律。** 来自 macOS 本地编译通过但 Linux CI 反复失败的教训：

```markdown
## 跨平台编译注意事项 (macOS / Linux CI)

1. STL 头隐式包含: Apple libc++ 隐式拉 <optional>/<stdexcept>/...,
   Linux libstdc++ 严格 — 用就显式 #include。
2. 64-bit 整数 typedef 差异: Linux LP64 下 int64_t == long,
   macOS 下 int64_t == long long。static_cast<long long>(...)
   传给重载函数在 Linux 会歧义。
3. Release 优化掉 UB: double → uint64_t 当 double 超出 [0, 2^64) 是 UB。
   Release CI 不触发, 但 Sanitizers 会抓。
```

### 3.4 核心示例：授权与约束并存

CLAUDE.md 同时包含明确的授权和严格的禁止规则。

**授权方面：消除 AI 的犹豫：**

```markdown
### 自动提交策略（已授权）
Claude Code 被授权在每一小步改动后自动提交，无需额外确认。

### BACKLOG 自动修复策略（已授权）
BACKLOG.md 中已记录的问题，Claude Code 被授权在二次确认
错误确实存在后直接修复，无需逐条等待用户手动审核。

### 自动 Push 策略（已授权）
这是非公开私有仓库，Claude Code 被授权在每个大阶段完成后自动 push。
```

**约束方面：项目中共有 18 条以"禁止"开头的硬性规则：**

```markdown
- 禁止 git reset --hard 除非用户明确要求
- 禁止 git push --force 到 main
- 禁止 --no-verify 跳过 hooks
- 禁止堆叠多个 sleep N && tail 后台任务
- 禁止 sleep 超过 300 秒
- 禁止盲猜：遇到 bug 时不要在未定位问题的情况下直接修改代码碰运气
- 禁止不验证就关闭 BACKLOG 条目
- 禁止因为担心上下文膨胀而停止工作
```

这种"宽进严出"的设计让 AI 能高效工作（不需要频繁询问用户确认），同时防止不可逆的危险操作。

### 3.5 成效

AI 无需猜测如何编译、测试、提交代码。从 Git 历史可以看到，最近的 commit 中没有一个触发已记录的铁律——这些铁律已经内化为 AI 的"常识"。

---

## 四、持久化文件网络

**解决的问题：** AI Agent 没有长期记忆。新会话对前一次会话没有任何记忆。一个复杂的调试会话可能产生数百行分析结果，会话结束后全部丢失。

**方案：** 六个持久化文件，构成跨会话记忆网络。

### 4.1 文件职责矩阵

| 文件 | 行数 | 职责 | 谁写入 | 谁读取 |
|------|------|------|--------|--------|
| `TODO.md` | 22 | 用户手写的待办清单 | 用户 | AI（Stop Hook 监督） |
| `task_plan.md` | 30 | AI 的执行计划 | AI | AI（每会话开始） |
| `progress.md` | 1204 | 实时进度与实测数据 | AI | AI（恢复进度） |
| `findings.md` | 119 | 调查发现与技术决策 | AI | AI（避免重复调查） |
| `BACKLOG.md` | 381 | 未修复的问题清单 | AI | AI（新任务前必读） |
| `RESOLVED.md` | 1414 | 已修复的审计记录 | AI | AI（避免重复修复） |

### 4.2 文件间的协作闭环

六个文件不是孤立的，而是形成了一个精密的闭环：

**task_plan.md** 定义要做什么，**progress.md** 记录做到了哪里，**findings.md** 记录发现了什么，**BACKLOG.md** 记录 AI 在工作中的临时待办，**RESOLVED.md** 记录已解决的临时待办。**TODO.md** 独立于这个网络，由用户直接控制。

### 4.3 核心示例：progress.md 的粒度

progress.md 记录的不只是完成了什么功能，更深入到了每个子阶段的详细执行数据。新会话读取后可以在数分钟内恢复进度：

```markdown
## 2026-05-17 lp_bits=22 fallback FINAL FAIL (5h45m)

### Round 5 final
- V0 Merged=102248 + V3 added=178824 = **281072 usable**
- LP cols 341348 / usable 281072 = **β=121.4%**
- Matrix: **281072 × 364008 (excess=0)** — NO EXCESS
- FAIL (factorization unsuccessful)

### 关键洞察: lp_bits 微调对 50d 无收益
|Path               |Round 5 usable|β      |Matrix           |
|-------------------|--------------|-------|-----------------|
|V3 cascade lp_bits=23|282027      |121.6% |282027×365516    |
|lp_bits=22 fallback  |281072      |121.4% |281072×364008    |

差异 <1%! Both paths β converge to 121-122%,
表明 fundamental plateau 在 filter+sieve at 50d, 与 LP bound 微调无关.
```

### 4.4 核心示例：BACKLOG 到 RESOLVED 的闭环

关闭一个 BACKLOG 条目需要满足三个条件：代码已合入、测试已通过、无副作用。RESOLVED.md 中记录完整的修复 timeline：

```markdown
### [ALGO] ~~50d β plateau ~121%~~ ✅
- **发现**: 2026-05-17
- **解决**: 2026-05-19
- **完整 step timeline**:
  - step 7 (commit 0fe36b7): BW thin matrix B'=M^T·M variant
  - step 9 (commit 0fac325): Diagnostic logging + OOC default
  - step 11 (PID 69073, 3934s): 50d empirical Round 1
    LP weight histogram: w2=89416 (50.9%), w3=35544 (20.2%),
    w4+=50789 (28.9%). 49% LP keys 是 weight≥3 — V0 完全错过
  - step 12 (merge 39c533d): V0_BFS size-aware default-ON
  - step 13 (merge c0759a8): 集成 gap 修复
```

### 4.6 成效

六个文件总计 3170 行。新会话通过读取 progress.md 可以在数分钟内恢复到上一次的进度。RESOLVED.md 的 1414 行审计记录覆盖了从 P1 级算法 Bug 到 TEST 级测试缺口的完整修复历史，防止"重新修复已经修复过的问题"这种浪费。

---

## 五、Hook 拦截系统

**解决的问题：** AI 可能修改不该修改的文件（构建产物、遗留脚本）、在修改代码后忘记验证编译、在任务未完成时提前结束对话。

**方案：** 权限白名单 + 三类 Hook，在 AI 的操作边界处设置多层自动检查点。

### 5.1 权限白名单（settings.json）

最基础的安全层。AI 只能执行白名单中的命令：

```json
{
  "permissions": {
    "allow": [
      "Bash(make:*)",
      "Bash(ctest:*)",
      "Bash(cmake:*)",
      "Bash(git:*)",
      "Bash(./build/*)",
      "Bash(ls:*)",
      "WebSearch"
    ]
  }
}
```

白名单之外的命令（如 `rm -rf`、`curl | sh`、`sudo`）会被 Claude Code 自动拦截，需要用户手动确认。

### 5.2 PreToolUse Hook — 编辑前拦截

当 AI 调用 Edit 工具时，检查目标文件路径：

```bash
filepath="$CLAUDE_FILE_PATH"
case "$filepath" in
  *.sh)           echo 'BLOCK: 不要修改遗留脚本文件' ;;
  */build/*)      echo 'BLOCK: 不要修改 build 目录中的生成文件' ;;
  */CMakeFiles/*) echo 'BLOCK: 不要修改 CMake 生成的文件' ;;
esac
```

输出 "BLOCK" 关键词会阻止编辑操作。这防止了 AI 修改构建产物或遗留文件后引发连锁问题。

### 5.3 PostToolUse Hook — 编辑后提醒

编辑完成后自动提醒后续动作：

```bash
# 编辑 C++ 文件后
filepath="$CLAUDE_FILE_PATH"
case "$filepath" in
  *.hpp|*.cpp)
    echo 'Reminder: 修改了 C++ 文件，记得运行 make 验证编译' ;;
  */CMakeLists.txt)
    echo 'Reminder: CMakeLists.txt 已修改，需重新运行 cmake' ;;
esac

# Write 新建文件后
case "$filepath" in
  *.hpp|*.cpp)
    echo 'Reminder: 新建了 C++ 文件，可能需要在 CMakeLists.txt 中添加' ;;
esac
```

### 5.4 Stop Hook — 任务完成拦截

这是最精巧的 Hook。`stop-todo-check.sh` 在 AI 尝试结束对话时执行：

```bash
#!/bin/bash
# 1. 防循环守卫
STOP_HOOK_ACTIVE=$(echo "$INPUT" | python3 -c "
import json, sys
data = json.load(sys.stdin)
print('true' if data.get('stop_hook_active', False) else 'false')
")
if [ "$STOP_HOOK_ACTIVE" = "true" ]; then
  exit 0  # 已被阻止过一次，允许停止，防止无限循环
fi

# 2. 解析 TODO.md
INCOMPLETE=$(grep -cE '^\s*[-*+]\s\[[ ]\]' "$TODO_FILE" 2>/dev/null || true)

# 3. 决策
if [ "$INCOMPLETE" -eq 0 ]; then
  exit 0  # 全部完成，允许停止
fi

# 4. 阻止停止，列出未完成项
INCOMPLETE_ITEMS=$(grep -E '^\s*[-*+0-9.]+\s\[[ ]\]' "$TODO_FILE" \
    | head -10 | sed 's/^[[:space:]]*//' | cut -c1-100)

echo '{"decision": "block", "reason": "TODO.md 中仍有 '${INCOMPLETE}' 项未完成..."}'
```

**三个设计要点：**

**区分三类待办。** Stop Hook 仅检查 TODO.md（用户手写），不检查 BACKLOG.md（AI 发现）或 task_plan.md（AI 计划）。用户拥有对 AI 行为的最终控制权——只要用户在 TODO.md 中写下任务，AI 就无法在完成任务之前结束对话。

**防循环守卫。** `stop_hook_active` 标志确保 Hook 不会导致无限循环——如果 AI 已被阻止过一次，第二次尝试停止时直接放行。

**JSON 决策协议。** Hook 输出 JSON 格式的决策，支持 `block`（阻止）和 `allow`（放行）两种结果，以及可选的 `reason` 字段。这是 Claude Code 的 Hook 协议规范。

### 5.5 四层防护的关系

四层防护形成了纵深防御：

| 层级 | 机制 | 作用点 | 作用 |
|------|------|--------|------|
| 第一层 | 权限白名单 | 命令执行前 | 限制可执行的命令范围 |
| 第二层 | PreToolUse Hook | 文件编辑前 | 阻止修改特定文件 |
| 第三层 | PostToolUse Hook | 文件编辑后 | 提醒后续验证步骤 |
| 第四层 | Stop Hook | 对话结束前 | 确保用户任务已完成 |

### 5.6 成效

从编辑前（PreToolUse）到编辑后（PostToolUse）到对话结束（Stop），三个关键节点全部有自动检查。权限白名单则从底层限制了 AI 可以执行的命令范围。四层防护协同工作，既防止了危险操作，又确保了任务完成度。

---

## 质量层：让 AI 把活干好

> 以下三个机制在核心层之上，确保 AI 不仅"能干活"，还能"干好活"——遵循工程纪律、代码经过审查和验证、测试覆盖充分。

---

## 六、Superpowers 技能系统

**解决的问题：** AI 倾向于"拿到需求就直接写代码"，跳过需求澄清、设计评审、测试验证等工程活动。

**方案：** Superpowers 插件（v5.1.0）提供 14 个结构化工作流技能，每个技能以 Markdown 文件定义，包含决策流程图和强制执行规则。

### 6.1 技能全景

14 个技能覆盖软件开发全生命周期，按阶段分为四组：

**需求与设计（2 个）：** brainstorming 在任何创造性工作之前强制执行需求澄清流程，硬门禁——"在呈现设计并获得用户批准之前，不得调用任何实现技能"。writing-plans 将设计规格转化为详细的实现计划，每个步骤是 2-5 分钟的单一操作。

**实现（4 个）：** test-driven-development 的铁律——"没有失败的测试就不能写产品代码"。subagent-driven-development 为每个任务分派全新子 Agent（隔离上下文），完成后两阶段审查。using-git-worktrees 确保工作在隔离空间中进行。executing-plans 是备选方案。

**质量保障（5 个）：** systematic-debugging 的铁律——"没有根因调查就不能修复"。verification-before-completion 的铁律——"没有新鲜的验证证据就不能声称工作完成"。requesting-code-review、receiving-code-review 处理代码审查流程。

**交付（3 个）：** finishing-a-development-branch 处理分支完成时的合并和清理。dispatching-parallel-agents 指导并行 Agent 分派。using-superpowers 是元技能，定义如何发现和使用其他技能。

### 6.2 核心示例：verification-before-completion

这个技能直接解决了"虚假完成"问题（失败模式二）。它定义了一个严格的 Gate 函数：

```
BEFORE claiming any status or expressing satisfaction:

1. IDENTIFY: What command proves this claim?
2. RUN: Execute the FULL command (fresh, complete)
3. READ: Full output, check exit code, count failures
4. VERIFY: Does output confirm the claim?
   - If NO: State actual status with evidence
   - If YES: State claim WITH evidence
5. ONLY THEN: Make the claim

Skip any step = lying, not verifying
```

并给出了明确的对照表：

| 声称 | 需要什么 | 什么不够 |
|------|---------|---------|
| 测试通过 | 测试命令输出：0 failures | 上次运行结果、"应该通过" |
| 构建成功 | 构建命令：exit 0 | Linter 通过 |
| Bug 已修复 | 原始症状测试：通过 | 代码已改、"假设已修复" |
| Agent 完成 | VCS diff 显示变更 | Agent 报告"成功" |

### 6.3 核心示例：using-superpowers 的红旗清单

元技能包含一个"红旗思维"清单——如果 AI 产生了以下想法，说明它正在合理化逃避使用技能：

| 想法 | 现实 |
|------|------|
| "这只是一个简单问题" | 问题是任务，检查技能 |
| "我先了解一下代码库" | 技能告诉你怎么了解 |
| "这个技能太大了" | 简单的东西会变复杂 |
| "我先做这一件事" | 做任何事之前先检查 |
| "我知道那个意思" | 知道概念不等于使用技能 |

这个清单的价值在于：它预判了 AI 逃避纪律的心理模式，并在 AI 产生这些想法时发出警告。

### 6.4 技能与 CLAUDE.md 的优先级

当 Superpowers 技能与 CLAUDE.md 冲突时，优先级为：

1. **用户的显式指令**（CLAUDE.md、直接请求）——最高优先级
2. **Superpowers 技能**——覆盖默认行为
3. **默认系统提示**——最低优先级

### 6.5 成效

14 个技能将"拿到需求就写代码"的冲动转化为结构化的工程流程。项目中每个 ENV 开关的实现都遵循统一模板（Helper API → ENV 解析 → bit-for-bit guarantee → 测试 → 文档），这正是技能系统纪律性的体现。

---

## 七、多 Agent 协作编排

**解决的问题：** 单一 AI Agent 容易"任务漂移"（在一个问题上跑偏），或"审查放水"（自己写的代码自己审查，发现不了问题）。面对大规模任务时串行效率低下。

**方案：** 六角色专职分工 + 领域专用 Agent + Worktree 级并行隔离。

### 7.1 六角色分工

CLAUDE.md 定义了六个专职角色，每个角色有明确的职责边界和触发时机：

| 角色 | Agent 类型 | 核心职责 | 触发时机 |
|------|-----------|---------|---------|
| 架构师 | code-architect | 设计实现方案、确定文件结构 | 编码之前 |
| 探索员 | Explore | 检索代码、定位文件 | 需理解现有代码时 |
| 开发者 | code-explorer + 主线程 | 执行编码 | 实施阶段 |
| 审查员 | code-reviewer | 检查质量和正确性 | 大阶段完成后 |
| 找茬员 | silent-failure-hunter | 查找静默失败和边界遗漏 | 审查员通过后 |
| 进度员 | 主线程自身 | 维护 progress.md | 持续进行 |

### 7.2 防怠工条例

CLAUDE.md 明确定义了六种"怠工"行为：

1. **审查放水** — 审查员未发现任何问题就直接通过（至少要说明审查了哪些方面）
2. **跳过环节** — 跳过架构分析直接编码，或跳过审查直接 commit
3. **任务漂移** — Agent 执行了不属于自己职责的工作
4. **信息丢失** — 未将关键发现写入持久化文件
5. **盲目执行** — 未读懂现有代码就开始修改
6. **沉默失败** — 遇到问题不上报，自行绕过

### 7.3 领域专用 Agent

项目在 `.claude/agents/` 中定义了两个预置领域知识的 Agent。

**gnfs-debugger** 预置了 GNFS 代码库中常见的故障模式知识：

```markdown
### Square Root Phase Failures
1. "Product ≡ 0 mod N" → Relations with gcd(a-b*m, N) > 1 leaked
2. "No valid sign pattern" → Class group too large for Couveignes
3. All factors trivial → Schirokauer prime mismatch (ℓ≠2 with GF(2))

### Debugging Workflow
1. Reproduce → 2. Isolate (which GNFS phase?) → 3. Trace values
→ 4. Verify math by hand for small cases → 5. Compare with N=143
```

**gnfs-reviewer** 预置了数论特有的审查清单：

```markdown
### Mathematical Correctness
- [ ] Sign conventions match `a - b*α` throughout
- [ ] Norm computations: `f(a/b) * b^d` with correct signs
- [ ] Schirokauer maps: only ℓ=2 for GF(2) matrices

### Memory & Performance
- [ ] No unnecessary Integer copies (prefer const&)
- [ ] GF(2) matrix uses word-packed PackedGF2Matrix
- [ ] Inner loops avoid memory allocation (use SmallVector)
```

### 7.4 Worktree 级并行隔离

项目采用双层 Worktree 结构实现文件系统级的开发隔离：

**第一层：`.claude/worktrees/`（Agent 自动管理）。** 51 个 Agent Worktree，每个对应一个 ENV 开关的独立实现空间。从 `git worktree list` 可以看到完整的并行开发现场：

```
.claude/worktrees/agent-a01958d...  feat/260522-filter-lp-bloom       locked
.claude/worktrees/agent-a07330f...  feat/260522-mpz-powm-parallel     locked
.claude/worktrees/agent-a0b2f0a...  feat/260523-mpz-mul-parallel      locked
.claude/worktrees/agent-a368e50...  feat/260522-trial-div-simd        locked
.claude/worktrees/agent-a394912...  feat/260523-row-popcount-simd     locked
.claude/worktrees/agent-a6ca312...  feat/260523-brent-rho-parallel    locked
... (共 51 个，全部 locked)
```

**第二层：`.worktrees/`（手动管理）。** 20 个更大型的特性 Worktree：

```
.worktrees/metal-spmv/          Metal GPU 加速 SpMV
.worktrees/distributed-sieve/   分布式筛法
.worktrees/couveignes/          改进 Couveignes 算法
.worktrees/mmap-phase5/         MmapCSRMatrix Phase 5 集成
.worktrees/sieve-simd/          筛法 SIMD 内核
... (共 20 个)
```

**Superpowers 的 Worktree 标准化。** `using-git-worktrees` 技能定义了标准流程：先检测是否已在隔离空间（避免重复创建），优先使用平台原生工具（EnterWorktree），否则回退 `git worktree add`。`finishing-a-development-branch` 技能处理完成时的合并和清理。

### 7.5 成效

51 个 Agent Worktree 在两天内完成了 40+ 个 ENV 开关的并行实现，每个开关包含 Helper 实现、ENV 解析、单元测试、文档更新。串行模式下同样的工作量预计需要数周。304 个保留的分支形成了完整的开发树。

---

## 八、测试自动化体系

**解决的问题：** 80+ 个测试二进制，耗时跨越 5 个数量级。AI 不知道该跑哪些测试——跑太少遗漏回归，跑太多浪费时间。

**方案：** `scripts/test.sh`（2228 行 zsh 脚本）封装了五级测试分级、16 种运行模式、智能测试选择和自动超时保护。

### 8.1 五级测试分级

每个测试按执行时间分级，不同场景运行不同级别：

| 分级 | 超时 | 数量 | 场景 | 耗时 |
|------|------|------|------|------|
| instant | 10s | 39 个 | 日常开发（每次改动后） | ~5s |
| fast | 60s | 少量 | 模块级验证 | ~30s |
| slow | 120-300s | 5 个 | 合并门禁 | ~9s (Release) |
| heavy | 600-3600s | 少量 | 渐进测试、性能基准 | 数分钟 |
| stress | 43200s | 1 个 | 50/60-digit 极限压力 | 数小时 |

脚本中为每个测试定义了精确的超时秒数和分级标签：

```bash
# 超时秒数 (基于实测)
typeset -A TEST_TIMEOUT
TEST_TIMEOUT=(
    test_integer             10
    test_linalg              10
    test_sieve_basic         60
    test_kleinjung           180
    test_gnfs_e2e            300
    test_gnfs_progressive    3600
    test_stress              43200
)

# 分级标签 (CI 用)
typeset -A TEST_TIER
TEST_TIER=(
    test_integer             "instant"
    test_sieve_basic         "fast"
    test_kleinjung           "slow"
    test_gnfs_progressive    "heavy"
    test_stress              "heavy"
)
```

### 8.2 16 种运行模式

覆盖所有开发场景的模式选择：

```bash
# ── 日常开发（最常用）──
./scripts/test.sh                      # 冒烟测试：39 个 instant 测试，~5s
./scripts/test.sh changed              # 根据 git diff 自动选择受影响模块
./scripts/test.sh changed --deep       # + 级联依赖模块

# ── 模块级 ──
./scripts/test.sh module linalg        # 只跑线性代数模块
./scripts/test.sh module sieve sqrt    # 多模块

# ── 合并门禁 ──
./scripts/test.sh gate                 # smoke + 回归 (17/27/40/81-bit) ~9s
./scripts/test.sh gate --quick         # 仅 smoke ~5s

# ── 渐进式 ──
./scripts/test.sh L1                   # Level 1 only
./scripts/test.sh progressive 1 3      # L1 到 L3

# ── 全量 ──
./scripts/test.sh full                 # ctest + E2E + Progressive L1-L2
./scripts/test.sh nightly              # 全部 + L4 + L5 + stress（过夜）

# ── 性能 ──
./scripts/test.sh bench --save         # 性能基准 + 保存结果

# ── 工具 ──
./scripts/test.sh list                 # 查看所有测试、模块、超时、分级
./scripts/test.sh watch                # 监视文件变更自动重测
```

### 8.3 智能测试映射

脚本维护了一个从模块到测试二进制的映射表，使得 `./scripts/test.sh module <模块名>` 能精确运行相关测试：

```bash
typeset -A MODULE_TESTS
MODULE_TESTS=(
    core       "test_integer test_params test_regressions test_edge_cases ..."
    util       "test_small_vector test_thread_pool test_logger test_primes ..."
    linalg     "test_linalg test_sge_batch_pivots test_block_wiedemann ..."
    sieve      "test_special_q test_sieve_basic test_bucket_sieve ..."
    cofactor   "test_cofactor test_squfof test_brent_pollard_rho ..."
    relation   "test_relation_collector test_filter test_filter_radix_sort ..."
    sqrt       "test_sqrt test_sqrt_debug test_hensel_parallel ..."
)
```

### 8.4 CI 流水线

三路 CI 流水线覆盖不同维度：

**ci.yml（主 CI）。** 四个平台（Linux x86_64/macOS arm64/Linux arm64/Windows x86_64），排除 slow/heavy/stress 标签，超时 240 秒。

**sanitizers.yml。** AddressSanitizer + UBSanitizer 一路，ThreadSanitizer 一路，仅运行 instant/fast 测试（Sanitizer 使执行慢 5-10 倍）。

**codeql.yml。** 代码安全扫描。

CI 标签与 test.sh 分级保持一致——CMakeLists.txt 中每个测试都打了 LABELS：

```cmake
set_tests_properties(test_stress PROPERTIES
    LABELS "heavy;stress" TIMEOUT 43200)
```

### 8.5 性能调优脚本群

`scripts/` 目录下还有一组性能调优脚本，形成了从参数扫描到 ROI 估算的完整工具链：

**sweep_lp_bits.sh** — 在指定 digit 下对多个 lp_bits 值并行启动 test_stress（每个值一个 nohup 后台进程），用于比较不同 LP bound 的实际分解效率。

**sweep_combo.sh** — 2x2 ENV 组合扫描，比较两个 ENV 开关的交叉效应（additive/synergy/cancel）。

**v3_roi_estimator.sh** — 给定 V0 baseline 日志，估算 V3 cascade 是否值得启用：

```bash
# 输出示例
## Round-by-Round merge_rate (α):
  α=1.098% target=618000
  α=2.341% target=1200000

## V0 vs V0+V3 needed raw comparison
  V0 only:     12.3M raw needed
  V0+V3:        8.7M raw needed (-29%)
  Wall time savings: ~4h at current rate
  Recommendation: ENABLE V3 cascade
```

**perf/ 子目录** — pmu-stat.sh（PMU 硬件计数器采集）、profile-cpu.sh（CPU profiling）、parse-trace.py（解析 Instruments trace）等底层性能分析工具。

### 8.6 成效

AI 可以根据改动范围精确选择测试级别：改一个函数跑 smoke（5 秒），改核心流程跑 e2e（5 分钟），PR 前跑 gate（9 秒）。统一的入口脚本消除了"该跑哪个测试"的决策负担。性能调优脚本群使 AI 能够独立完成从参数扫描到 ROI 分析的全流程。

---

## 效率层：让 AI 把活干快

> 以下两个机制在核心层和质量层之上，进一步提高开发效率——降低信息检索成本、提高版本管理的规范性和可追溯性。

---

## 九、分层文档架构

**解决的问题：** 62 个 ENV 开关各有详细设计文档。如果全部塞入 CLAUDE.md，文件膨胀到 5000+ 行，每次会话浪费大量上下文窗口。如果不写，AI 实现新开关时缺乏参考。

**方案：** 三层文档结构，按信息密度和查阅频率分层。

### 9.1 三层结构

**第一层：CLAUDE.md（995 行）。** 每次对话自动加载。只包含铁律、速查索引和工程规范。

**第二层：`docs/env-flags/`（4620 行，8 个模块文件）。** 实现某个开关时按需查阅。每个开关遵循统一模板。

**第三层：`docs/plans/` + `docs/algorithms/` + `docs/perf/`（3604 行）。** 深入研究时查阅。

### 9.2 瘦身重构的实证

| 维度 | 重构前 | 重构后 | 变化 |
|------|--------|--------|------|
| CLAUDE.md 行数 | 5402 | 995 | -82% |
| ENV 详细设计位置 | CLAUDE.md 内 | docs/env-flags/ | 外移 |
| 上下文窗口占用 | 高（每次全量加载） | 低（按需读取） | 大幅降低 |

### 9.3 文档化规范（强制执行）

新增 `GNFS_*` 开关时必须完成四步：

1. 详细文档写到 `docs/env-flags/<module>.md`
2. 在 CLAUDE.md 速查表加一行（带锚点链接）
3. 遵循统一模板（标题 + 用法 + 算法 + ENV 解析 + bit-for-bit guarantee + ROI + 集成点 + 测试 + Default）
4. 若属 Parallel Dispatcher / SIMD family，同步更新 README 的 family 表

### 9.4 写作规范

`docs/writing-style-guide.md`（138 行）定义了文档写作标准：GitHub Markdown 渲染规范、中英文混排规范（中英文之间加半角空格）、正规英语写作规范（Oxford comma 必须保留、避免缩约形式）。

### 9.5 成效

AI 的上下文窗口不再被低频查阅的详细设计占用。实现某个 ENV 开关时，从速查表找到链接，按需读取对应模块的详细文档。文档与代码同步演进——不是"写完代码再补文档"，而是"文档是功能实现的一部分"。

---

## 十、Git 工作流工程

**解决的问题：** AI 的大规模改动如果不及时提交，上下文窗口压缩时信息丢失，大量工作成果蒸发。Git 历史如果混乱，无法精确追溯每个变更的目的。

**方案：** 极细粒度自动提交 + 规范化分支命名 + 自动推送 + 特性分支工作流。

### 10.1 分支命名规范

格式 `<type>/<YYMMDD>-<description>`，类型包括 feat、fix、perf、refactor、exp：

```
feat/260523-brent-rho-parallel
feat/260523-sieve-saturated-sub-simd
perf/260523-poly-horner-mod-simd
fix/260522-hensel-overflow
refactor/260521-extract-poly-ctx
exp/260521-neon-sieve
```

日期前缀使分支的时间线一目了然。核心原则：**main 必须始终可编译、可测试通过。** 合并后分支保留不删除——304 个分支形成了完整的开发树。

### 10.2 自动提交策略

CLAUDE.md 授权 AI "在每一小步改动后自动提交"，并制定严格规则：

```markdown
- 粒度极细：改一个函数、修一个编译错误、加一个测试，都应立即 commit
- 一 Bug 一 Commit（强制）：每个独立 Bug 修复必须是单独的 commit
- 不需要等编译全部通过：修了一个编译错误就可以 commit
- 改动大小不限：即使只改一行也要 commit
```

从 Git 历史可以看到实际效果——每个 commit 都是原子性改动：

```
feat(sieve): add uint8 saturated subtract SIMD helper (W15 T4)
test(cofactor): cover Brent-Pollard rho parallel dispatcher (W15 T3)
feat(util): add GMP mpz_mul batch parallel dispatcher (W15 T5)
test(linalg): cover GF(2) per-row popcount SIMD helper (W15 T1)
feat(polynomial): add F_p[x] modular Horner evaluation SIMD helper (W15 T2)
```

### 10.3 特性分支工作流

`scripts/feature-branch.sh`（295 行）封装了特性分支的完整生命周期：

```bash
./scripts/feature-branch.sh create feat bucket-sieve
# → 创建 feat/260315-bucket-sieve

./scripts/feature-branch.sh gate
# → 委托 test.sh gate 验证

./scripts/feature-branch.sh merge
# → 自动门禁 + --no-ff merge 到 main
```

### 10.4 自动推送

```markdown
这是非公开私有仓库，Claude Code 被授权在每个大阶段完成后自动 push。
禁止 force push 到 main。
```

### 10.5 成效

2051 个 commit 形成了精确到函数级别的变更记录。任何一行代码的变更都可以通过 `git diff` 精确定位目的。自动推送确保远程仓库始终有最新备份。

---

## 十一、可复用的 Harness 模式

从项目中提炼出七个可复用的模式：

### 模式一：指令瘦身

**判据：** 每次开发都要遵守的写 CLAUDE.md，某次开发才查的写 docs/。

**实施：** CLAUDE.md 只留铁律和速查索引，详细设计下沉到 docs/。用一句话速查表 + 锚点链接指向详细文档。

**适用条件：** 任何 CLAUDE.md 超过 1000 行的项目。

### 模式二：Stop Hook 监督

**机制：** 用户在 TODO.md 中写任务，Stop Hook 在 AI 尝试结束时检查完成状态。有未完成项则阻止停止。

**关键设计：** 区分"用户写的待办"和"AI 自己发现的待办"，Stop Hook 仅检查前者。防循环守卫防止无限循环。

**适用条件：** 任何需要 AI 跨多个对话会话完成的任务。

### 模式三：Worktree 级并行

**机制：** 正交的功能各自在独立 Worktree 中实现，多个 Agent 同时工作互不干扰。

**前提条件：** 功能之间的正交性 + Worktree 的文件系统隔离 + 统一测试脚本（每个 Worktree 可独立验证）。

**适用条件：** 有 5+ 个正交功能需要实现的项目。

### 模式四：测试分级 + 统一入口

**机制：** 按执行时间分级，统一入口脚本封装编译 + 超时 + 报告。模块到测试二进制的映射表支持精确选择。

**关键设计：** 超时秒数基于实测数据，分级标签与 CI 标签保持一致。

**适用条件：** 有 20+ 个测试、执行时间差异超过 10 倍的项目。

### 模式五：失败驱动铁律

**机制：** 每条铁律都有 commit hash 和实测数据作为证据，从真实 Bug 中提炼。表达方式是"xxx 永远是 BUG"而非"请注意 xxx"。

**关键设计：** 铁律必须包含触发条件、受影响的入口点和正确做法。

**适用条件：** 任何经历过重复犯同样错误的项目。

### 模式六：授权与约束并存

**机制：** 明确授权 AI "自动提交"、"自动 push"、"直接修复 BACKLOG"，同时用禁止规则约束危险操作。

**关键设计：** 授权消除犹豫（"我应该先问用户吗？"），约束防止危险（"我可以 force push 吗？"）。

**适用条件：** 任何需要 AI 自主工作超过 30 分钟的项目。

### 模式七：文档化规范强制执行

**机制：** 新增功能时必须同步更新文档。统一模板确保每个功能的文档质量一致。文档是功能实现的一部分，不是"以后再补"。

**关键设计：** 统一模板包含 9 个标准段落（标题 + 用法 + 算法 + ENV 解析 + bit-for-bit guarantee + ROI + 集成点 + 测试 + Default）。

**适用条件：** 任何有 10+ 个类似功能需要实现的项目。

---

## 十二、如何开始构建你自己的 Harness

如果你希望在自己的项目中应用 Harness 工程，建议按以下顺序逐步引入：

**第一步：写 CLAUDE.md（10 分钟）。** 写下项目的构建命令、测试命令、代码约定。即使只有 20 行，也能让 AI 立即开始有效工作。

**第二步：设置权限白名单（5 分钟）。** 在 `settings.json` 中列出 AI 可以执行的命令。这防止了最常见的危险操作。

**第三步：创建 TODO.md + Stop Hook（30 分钟）。** 写下你希望 AI 完成的任务，配置 Stop Hook 脚本。这确保 AI 不会在任务未完成时结束对话。

**第四步：建立持久化文件（1 小时）。** 创建 progress.md 和 BACKLOG.md，将 `.gitignore` 排除它们。这解决了跨会话知识丢失的问题。

**第五步：安装 Superpowers 插件（5 分钟）。** 这为你的 AI 装上了工程纪律——TDD、验证前完成、系统化调试等。

以上五步可以在一个小时内完成，解决"能不能干活"的根本问题。之后根据需要逐步引入测试分级、Worktree 并行、分层文档等进阶机制。

---

## 结语

Harness 工程的核心理念可以用一句话概括：**约束即赋能。**

表面上，Harness 体系给 AI 加了大量"限制"——不能跳过测试、不能不验证就声称完成、不能修改特定文件、不能在有未完成 TODO 时结束。但这些约束实际上是在赋能：它们消除了 AI 的决策模糊性，让 AI 知道什么是正确的做法，从而能够自信地、高效地执行复杂任务。

从 GNFS 项目的实践中可以看到，一个成熟的 Harness 体系不是"锦上添花"的附加物，而是 AI 辅助开发从"能用"到"好用"的关键跨越。没有 Harness，AI 是一个能力强大但缺乏纪律的助手；有了 Harness，AI 成为一个有纪律的工程师。

八大机制中，如果只能选择三个，应该选 CLAUDE.md 指令系统、持久化文件网络和 Hook 拦截系统——它们解决了"能不能干活"的根本问题。如果可以选择六个，再加上 Superpowers 技能系统、多 Agent 编排和测试自动化——它们解决了"活干得好不好"的质量问题。全部八个机制共同构成了一个完整的 AI 工程化开发环境。
