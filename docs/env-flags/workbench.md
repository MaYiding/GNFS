# macOS Workbench 运行时开关

> 本文档收录 macOS workbench（`apps/macos/GNFSWorkbench`）读取的 `GNFS_*` 环境变量，以及它启动运算引擎时怎样处理继承来的环境。C++ 引擎各模块的开关见 [ENV 开关总览](README.md)。
>
> 返回: [ENV 开关总览](README.md)

---

## Engine Executable Override (GNFS_CLI_PATH)

`GNFS_CLI_PATH=<path>` 让 workbench 把 `<path>` 当作 GNFS 运算引擎的可执行文件，排在
bundle 内置的 `gnfs` 之前。未设置或为空值时没有影响。

- **取值与解析**：`GNFSExecutableResolver.resolve()` 每次解析时读取，非空字符串即生效。
  取值按路径原样解释，相对路径按 workbench 进程的当前目录解析。该路径只有确实是可执行
  文件时才会被采用；否则不报错，继续尝试后面的候选。
- **默认值**：未设置。候选顺序是 bundle 内置的 `gnfs`，然后是当前目录及其向上最多
  6 层目录里的 `build/gnfs`，取第一个可执行的候选。全部落空时抛出
  `GNFSExecutableResolverError.missingExecutable`，并列出检查过的路径。
- **对结果的影响**：只决定 workbench 启动哪个引擎二进制，不改变传给引擎的参数。该变量
  只在 workbench 进程里读取，不会传给引擎，见下一节。
- **接入点**：
  - `apps/macos/GNFSWorkbench/GNFSWorkbench/Services/GNFSExecutableResolver.swift`：
    `GNFSExecutableResolver.resolve()` 读取；`environment` 默认是
    `ProcessInfo.processInfo.environment`。
  - `apps/macos/GNFSWorkbench/GNFSWorkbench/Services/GNFSRunner.swift`：
    `ProcessGNFSRunner` 的默认参数构造默认的 resolver，`start()` 在每次启动引擎前调用
    `resolve()`。
  - `apps/macos/GNFSWorkbench/GNFSWorkbench/AppModel.swift`：`AppModel.init` 在非演示
    模式下创建 `ProcessGNFSRunner()`，所以正式运行的 app 会读取该变量。带 `--ui-testing`
    或 `--design-preview` 且不带 `--ui-testing-real-engine` 时改用 `DemoGNFSRunner`，
    不读取任何环境变量。
- **测试**：`apps/macos/GNFSWorkbench/GNFSWorkbenchTests/ProcessGNFSRunnerTests.swift` 的
  多数用例通过 `GNFSExecutableResolver(environment: ["GNFS_CLI_PATH": ...])` 把 runner
  指向探针脚本或真实 CLI。没有测试单独覆盖候选顺序，也没有测试覆盖取值不可执行时的
  回退。

---

## Engine Environment Isolation

workbench 启动引擎时不继承任何 `GNFS_*` 调优变量。`ProcessGNFSRunner.start()` 先删掉
继承环境里所有以 `GNFS_` 开头的键，再把 `GNFS_RESUME` 设成本次运行的
`Runs/<UUID>/state`，并固定 `LC_ALL`（`GNFSRunner.swift`）。`GNFS_RESUME` 的含义见
[sieve.md](sieve.md)。

因此由 workbench 启动的引擎只看得到 `GNFS_RESUME` 一个 `GNFS_*` 变量，本目录其余各页
的开关在这种运行里全部保持默认值；这些开关只在直接运行 `gnfs` 命令行时生效。

`testInheritedEngineOverridesAreIsolatedFromRunConfiguration` 用一个探针脚本验证这一点：
继承环境里的 `GNFS_FORCE_SIQS`、`GNFS_CLI_PATH` 与另一个作为样例的 `GNFS_*` 变量都没有
到达子进程，`GNFS_RESUME` 被替换成工作区的路径，不带 `GNFS_` 前缀的变量原样保留。
workbench 侧的说明见 [workbench README](../../apps/macos/GNFSWorkbench/README.md)。
