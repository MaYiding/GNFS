# GNFS Project — Claude Code Instructions

## Project Overview

Industrial-grade **General Number Field Sieve (GNFS)** implementation in C++20.
Factorizes large composite integers using the most powerful known classical algorithm.

## Build System

```bash
# 配置 (首次或 CMakeLists.txt 改动后)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# 编译
make -C build -j$(sysctl -n hw.ncpu)

# 运行全部测试
cd build && ctest --output-on-failure

# 运行单个测试
./build/test_gnfs_e2e    # 端到端测试 (最重要)
./build/test_linalg      # 线性代数测试
./build/test_sqrt        # 平方根测试
```

**编译依赖:** GMP (必需), NTL (可选), Metal (macOS 可选), pthreads

## Architecture

```
include/gnfs/
├── core/           # Integer, Polynomial, Relation — 基础类型
├── polynomial/     # Kleinjung 多项式选择, Murphy E 评估, base-m
├── factor_base/    # 因子基构建
├── sieve/          # Lattice sieve, Special-Q
├── cofactor/       # 余因子分解, 试除法, 光滑性检查
├── relation/       # 关系收集与过滤
├── linalg/         # GF(2) 矩阵, Block Lanczos, Schirokauer maps
├── sqrt/           # 代数平方根 (Couveignes), 有理平方根, 类群
└── util/           # SmallVector, ThreadPool, Logger, Timer

src/                # 对应 .cpp 实现
tests/              # 17 个测试文件
```

## GNFS Pipeline

1. **Polynomial Selection** → Kleinjung 算法选择 f(x), g(x)
2. **Factor Base** → 构建有理/代数因子基
3. **Sieving** → Lattice sieve with Special-Q
4. **Cofactorization** → 余因子试除 + 光滑性验证
5. **Relation Collection** → 收集足够多的光滑关系
6. **Linear Algebra** → GF(2) 矩阵 + Block Lanczos 求零空间
7. **Square Root** → Couveignes 算法计算代数平方根
8. **GCD** → gcd(a ± b, N) 得到非平凡因子

## Critical Conventions

### 元素表示
**Elements are `a - b*α` (NOT `a + b*α`).** 这是整个代码库的基本约定。

### Schirokauer Maps
- 对于 GF(2) 矩阵，**只能使用 `schirokauer_primes = {2}`**
- ℓ > 2 的 Schirokauer maps 要求非 GF(2) 矩阵 (矩阵条目需 mod ℓ)

### 关系过滤
- 必须拒绝 `gcd(a - b*m, N) > 1` 的关系（这些关系会产生退化依赖，product ≡ 0 mod N）

### 整数类型
- 使用 `gnfs::core::Integer` 封装 GMP `mpz_class`
- 所有大整数运算通过 `Integer` 类完成

## Code Style

- C++20 标准，使用 `std::optional`, `std::span`, concepts
- Header-heavy 设计：大部分实现在 `.hpp` 中（模板和内联）
- 命名: `snake_case` 用于函数和变量, `PascalCase` 用于类型
- Namespace: `gnfs::core`, `gnfs::linalg`, `gnfs::sieve` 等

## Performance-Critical Code

- `PackedGF2Matrix`: 64-bit word-packed，O(1) 位访问
- `FastPoly`: uint64_t 快速多项式算术（Schirokauer maps 专用）
- Couveignes: 预计算期望乘积，65536 模式搜索
- Block Lanczos: 64-bit block 并行运算

## Testing

- **E2E 测试 (`test_gnfs_e2e`)** 是最重要的验证：对 N=143 执行完整 GNFS 流水线
- 修改任何核心逻辑后，必须运行 E2E 测试
- 测试框架：自定义 assert 宏（非 GoogleTest/Catch2）

## Git 规范

### 初始化

```bash
git init
git add -A
git commit -m "chore: initial commit — GNFS project"
```

### 自动提交策略（已授权）

**Claude Code 被授权在每一小步改动后自动提交**，无需额外确认。
Commit 的核心目的是**状态记录和进度管理**，而非仅仅是"完成一个功能"。

- **粒度极细**：每做一小步就 commit — 改一个函数、修一个编译错误、加一个测试、调整一个参数，都应该立即 commit
- **不需要等编译全部通过**：修了一个编译错误就可以 commit（即使还有其他编译错误）。目的是记录进度，不是证明完成度
- **改动大小不限**：即使只改一行也要 commit，这样出错时能精确 `git diff` 定位问题
- **禁止提交敏感文件**：不提交 `.env`、credentials、私钥等

### Commit Message 格式

采用 **Conventional Commits** 规范：

```
<type>(<scope>): <简短描述>

[可选正文：解释 why，而非 what]

Co-Authored-By: Claude <noreply@anthropic.com>
```

**type 取值：**

| type | 用途 | 示例 |
|------|------|------|
| `feat` | 新功能 | `feat(sieve): add bucket sieve for large factor bases` |
| `fix` | Bug 修复 | `fix(sqrt): prevent Hensel p^d overflow with Integer` |
| `perf` | 性能优化 | `perf(lanczos): parallelize SpMV with 12 threads` |
| `refactor` | 重构（不改行为） | `refactor(core): extract polynomial context` |
| `test` | 测试增删改 | `test(linalg): add GF(2) matrix edge case tests` |
| `chore` | 构建/工具/配置 | `chore: update CMakeLists for NTL optional` |
| `docs` | 文档 | `docs: update CLAUDE.md with git conventions` |

**scope 取值**（对应目录）：`core`, `polynomial`, `factor_base`, `sieve`, `cofactor`, `relation`, `linalg`, `sqrt`, `util`

### 分支规范

| 分支 | 用途 |
|------|------|
| `main` | 稳定版本，所有测试必须通过 |
| `dev` | 日常开发主线 |
| `feat/<name>` | 新功能开发 |
| `fix/<name>` | Bug 修复 |
| `perf/<name>` | 性能优化 |
| `exp/<name>` | 实验性改动（可能丢弃） |

- 功能完成后合并到 `dev`，`dev` 稳定后合并到 `main`
- **禁止 force push 到 `main`**
- 用 `git worktree` 进行并行实验

### .gitignore

已配置，排除：`build/`, `xcode-build/`, `.cache/`, `.DS_Store`, IDE 文件, 计划文件（`task_plan.md` 等会话级文件）

### 自动 Push 策略（已授权）

**这是非公开私有仓库，Claude Code 被授权在每个 to-do 大阶段完成后自动 push**。

- **何时 push**：每个大阶段（如 1.x 全部完成）检查点时 push，确保远程有最新备份
- **也可提前 push**：如果当前小步骤涉及重要突破或关键修复，可立即 push 保护成果
- **push 命令**：`git push origin <当前分支>` （带 `-u` 如果是新分支首次 push）
- **禁止 force push 到 `main`**

### Git 操作安全

- **禁止** `git reset --hard` 除非用户明确要求
- **禁止** `git push --force` 到 `main`（其他分支允许 force push）
- **禁止** `--no-verify` 跳过 hooks
- 优先创建**新 commit** 而非 `--amend`（amend 会覆盖历史）
- 每次 commit 前用 `git diff --staged` 确认内容

### 代码质量提升实践

为写出更好的代码，在开发过程中应主动采用以下手段：

- **查阅文档**：对不确定的 API 或算法，使用 `context7` MCP 或 `WebSearch` 查最新文档
- **参考实现**：实现复杂数论算法前，先搜索论文或成熟开源实现确认正确性
- **渐进式验证**：先用小规模数据验证算法正确性，再推广到大规模
- **性能意识**：hot path 中避免不必要的内存分配和拷贝，优先使用 `std::span`、`const&`
- **可读性优先**：清晰的变量命名和函数拆分胜过行内注释

## Error Handling 约定

### C++ 错误处理
- **内部逻辑错误**：使用 `assert()` 或自定义 `GNFS_ASSERT` 宏
- **运行时可恢复错误**：返回 `std::optional` 或错误码
- **致命错误**：`throw std::runtime_error("描述")` 或 `std::logic_error`
- **不要吞掉异常**：catch 后至少 log，不要空 catch

### 数值安全
- `uint64_t` 乘法可能溢出时使用 `__uint128_t`
- 大整数一律使用 `gnfs::core::Integer`
- 除法前检查除数非零
- 模运算前检查模数 > 0

## 项目根目录清理指南

根目录存在大量遗留文件（早期开发产物），分类如下：

| 类型 | 文件 | 处理方式 |
|------|------|----------|
| 遗留脚本 | `*.sh`（compile_test, fix_*, quick_fix 等） | **不纳入 git**，后续可删除 |
| 遗留文档 | `BUILD.md`, `*_STATUS.md`, `*_REPORT.md` 等 | **不纳入 git**，信息已过时 |
| 遗留源码 | `algebraic_sqrt.cpp`, `base_m.cpp` 等根目录 `.cpp/.hpp` | **不纳入 git**，正式代码在 `src/` 和 `include/` |
| 扁平命名文件 | `includegnfs*`, `src*`（无斜杠） | **不纳入 git**，是早期错误路径产物 |
| **正式代码** | `include/`, `src/`, `tests/`, `CMakeLists.txt` | **纳入 git** |
| **项目配置** | `CLAUDE.md`, `.gitignore`, `.claude/` | **纳入 git** |
| **文档** | `docs/`, `README.md` | **纳入 git** |

首次提交时，建议只 add 正式文件，不要 `git add -A`。推荐：

```bash
git add include/ src/ tests/ CMakeLists.txt CLAUDE.md .gitignore README.md docs/
git commit -m "chore: initial commit — GNFS core codebase"
```

## Known Limitations

- 大类群 (>20 generators) 的 Couveignes 实现可能失败
- 项目根目录有遗留脚本（`.sh`, 部分 `.md`），勿将其视为当前架构（见上方清理指南）

## 工作流规范（强制执行）

### 1. 任务前必须列计划

**所有任务开始前，必须先制定计划。** 默认调用 `planning-with-files` skill（创建 `task_plan.md`, `findings.md`, `progress.md` 于项目根目录）。

- **复杂任务**（涉及多文件修改、新功能、架构变更）：必须走完整 plan 流程，写入 `task_plan.md`
- **简单任务**（单文件小改动、typo 修复）：可以心中有计划后直接执行，但仍需在回复中说明意图
- **计划文件保存在项目根目录**，便于追溯

### 2. 测试设计原则

- **持久化测试**：所有新增功能必须编写可重复运行的单元测试/集成测试，提交到 `tests/` 目录
- **增量测试**：每次改动只需运行相关模块的测试，**不要每次都重跑整个项目**
  - 改了 `linalg/` → 运行 `./build/test_linalg`
  - 改了 `sqrt/` → 运行 `./build/test_sqrt`
  - 改了核心流程 → 运行 `./build/test_gnfs_e2e` 或 `./build/test_gnfs_progressive`
- **边界/极端情况**：测试必须覆盖边界值、零值、最大值、溢出等极端场景
- 测试框架：沿用项目自定义 assert 宏

### 3. 调试规范

- **禁止盲猜**：遇到 bug 时，不要在未定位问题的情况下直接修改代码碰运气
- **优先使用 lldb 调试**：段错误、逻辑错误等运行时问题，必须用 lldb 设断点、查看变量、追踪调用栈
- **调试流程**：
  1. 复现问题（最小化复现用例）
  2. 用 lldb attach/设断点定位根因
  3. 确认根因后再修改代码
  4. 修改后验证修复

### 4. 代码变更后检查（强制）

每次代码有大改动后，必须执行以下检查：

1. **编译检查**：`make -C build -j$(sysctl -n hw.ncpu)` 无 warning 无 error
2. **相关模块测试**：运行受影响模块的单元测试
3. **边界/极端情况审查**：
   - 整数溢出（特别是 `uint64_t` → `__uint128_t` / `Integer` 的边界）
   - 除零保护
   - 空容器 / 空指针
   - 大输入下的性能退化
4. **E2E 回归**：如改动涉及核心流水线，运行 `test_gnfs_e2e`
5. **代码审查**：使用 code-reviewer agent 检查高风险改动

### 5. 大型工程持久化管理

当任务较大时（预计超过 20+ 个 commit 或涉及多模块），**必须创建持久化工程文件**：

- **`task_plan.md`**：总体计划，拆分为大阶段（1, 2, 3...）和小步骤（1.1, 1.2, 2.1...）
- **`progress.md`**：实时进度记录，每完成一个小步骤更新
- **`findings.md`**：发现的问题、注意事项、技术决策

**任务必须自主拆分**为多个阶段。每个小步骤完成后即 commit，不等待整个阶段完成。

### 6. 两级 Commit + 检查点机制

#### 小步骤 Commit（自动，高频）

每做一小步就 commit，不需要审批：
- 改了一个函数 → commit
- 修了一个编译错误 → commit
- 加了一个测试 → commit
- 调了一个参数 → commit

小步骤 commit 前只需确认：
- [ ] `git diff --staged` 内容符合预期
- [ ] commit message 符合 Conventional Commits 格式
- [ ] 未包含敏感文件或构建产物

#### 大阶段检查点（需用户确认后继续）

当一个大阶段（如完成所有 1.x 步骤）全部完成后，**必须执行以下流程**：

1. **编译 + 测试验证**：确保当前状态编译通过、相关测试通过
2. **更新持久化文件**：详细记录到 `progress.md`，内容必须包含：
   - 当前状态总结（做了什么，结果如何）
   - 具体改了哪些文件，为什么
   - 下一阶段的计划和前置条件
   - 需要特别注意的风险点或技术细节
3. **展示给用户**：将状态摘要输出给用户查看
4. **等待用户回复"继续"**
5. **压缩上下文**：收到"继续"后，用 `/compact` 压缩之前的对话上下文，并引用持久化文件
6. **开始下一大阶段**

```
大阶段 1 开始
  ↓
步骤 1.1：实施 → commit（自动）
步骤 1.2：实施 → commit（自动）
步骤 1.3：实施 → commit（自动）
  ↓
大阶段 1 完成：编译验证 + 更新 progress.md + 展示状态
  ↓
用户回复 "继续"
  ↓
/compact 压缩上下文 + 链接持久化文件
  ↓
大阶段 2 开始
  ↓
步骤 2.1：实施 → commit（自动）
...
```

### 7. 计划执行流程总结

```
任务开始
  ↓
制定计划 → 拆分大阶段 + 小步骤 → 写入 task_plan.md
  ↓
执行小步骤 → 每步 commit（自动，不等编译全通过）
  ↓
大阶段完成 → 编译+测试 → 更新 progress.md → 展示给用户
  ↓
用户"继续" → /compact 压缩 → 下一大阶段
  ↓
全部完成 → 最终验证 + E2E 回归 → 总结报告
```

## Agent Team 协作编排

### 设计原则

采用**专职分工、职责明确**的 Agent 团队模式。每个 Agent 有且仅有一个核心职责，禁止越权操作或混淆任务边界。任何 Agent 不得怠工（跳过检查、简化流程、默认通过）——所有输出必须有实质内容。

### 角色分配

| 角色 | Agent 类型 | 职责 | 触发时机 |
|------|-----------|------|---------|
| **架构师** | `feature-dev:code-architect` | 分析需求，设计实现方案，确定文件结构和数据流 | 任务启动阶段，在编码之前 |
| **探索员** | `Explore` | 检索代码库，定位相关文件，理解现有模式和依赖 | 需要理解现有代码时（架构师或开发者需要参考时） |
| **开发者** | `feature-dev:code-explorer` + 主线程编码 | 理解现有实现细节，执行编码任务 | 实施阶段 |
| **审查员** | `feature-dev:code-reviewer` | 检查代码质量、逻辑正确性、是否符合项目约定 | 每个大阶段完成后，commit 前 |
| **找茬员** | `pr-review-toolkit:silent-failure-hunter` | 专门查找静默失败、错误吞没、边界遗漏、极端情况 | 审查员通过后的二次检查 |
| **进度员** | 主线程自身 | 维护 `progress.md`，跟踪计划执行状态，确保不偏离目标 | 持续进行 |

### 协作流程

```
架构师 → 分析需求，输出实现蓝图
  ↓
探索员 → 检索相关代码，提供上下文
  ↓
开发者 → 按蓝图逐步编码 → 每步 commit
  ↓
审查员 → 代码审查（质量 + 正确性 + 约定符合度）
  ↓
找茬员 → 静默失败猎杀（边界 + 异常 + 极端情况）
  ↓
进度员 → 更新 progress.md → 报告给用户
```

### 纪律约束（防怠工条例）

以下行为视为**怠工**，严格禁止：

1. **审查放水**：审查员或找茬员未发现任何问题就直接通过（至少要说明审查了哪些方面、为什么无问题）
2. **跳过环节**：跳过架构分析直接编码，或跳过审查直接 commit
3. **任务漂移**：Agent 执行了不属于自己职责的工作（如审查员去改代码，开发者去做架构决策）
4. **信息丢失**：未将关键发现写入持久化文件，导致上下文压缩后信息丢失
5. **盲目执行**：未读懂现有代码就开始修改，未理解需求就开始实现
6. **沉默失败**：Agent 遇到问题不上报，自行绕过或忽略

### 何时启用完整团队

- **简单任务**（<3 文件改动）：主线程直接执行，只在 commit 前调审查员
- **中等任务**（3-10 文件改动）：架构师 + 开发者 + 审查员
- **大型任务**（>10 文件 或 跨模块）：完整六角色团队
- **调试任务**：替换架构师为 `debugging-toolkit:debugger`，其他不变

### 并行调度

独立任务**必须并行派发**以提升效率：

- 多个模块互不依赖时 → 并行派发多个开发者 Agent（使用 `isolation: "worktree"`）
- 审查和找茬可并行 → 同时派发审查员和找茬员
- 探索多个方向时 → 并行派发多个探索员

禁止在可并行的场景下串行执行。
