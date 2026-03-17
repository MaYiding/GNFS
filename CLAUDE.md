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

**Claude Code 被授权在每次有意义的改动后自动提交**，无需额外确认。目的：每一步都有快照，出错时可快速回退。

- **何时提交**：每完成一个逻辑单元（一个 bug 修复、一个功能点、一组相关测试）即提交
- **改动大小不限**：即使只改一行，只要是有意义的改动就提交
- **禁止在编译不通过时提交**：必须 `make -C build` 成功后才能 commit
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

### Git 操作安全

- **禁止** `git reset --hard` 除非用户明确要求
- **禁止** `git push --force` 到 `main`
- **禁止** `--no-verify` 跳过 hooks
- 优先创建**新 commit** 而非 `--amend`（amend 会覆盖历史）
- 每次 commit 前用 `git diff --staged` 确认内容

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

### 5. 计划执行流程总结

```
任务开始
  ↓
制定计划（planning-with-files skill / 简单任务口头说明）
  ↓
按计划逐步实施
  ↓
每步实施后：编译 + 模块测试 → git commit（自动）
  ↓
大改动后：边界审查 + E2E 回归 → git commit（自动）
  ↓
全部完成：最终验证 + 更新 progress.md → git commit（自动）
```

### 6. Git 自动提交检查清单

每次自动提交前，确认以下条件全部满足：

- [ ] 编译通过（`make -C build` 无 error）
- [ ] 相关模块测试通过
- [ ] `git diff --staged` 内容符合预期（无意外文件）
- [ ] commit message 符合 Conventional Commits 格式
- [ ] 未包含敏感文件或构建产物
