# GNFS 性能优化宪章 (Performance Doctrine)

> **作者**: Claude (本宪章) + 马一丁 (审阅)
> **创建日期**: 2026-05-12
> **范围**: 本项目所有性能相关改动的指导原则、分析方法论与 Apple M5 调优手册
> **状态**: 活文档 — 每次大优化结束后必更新「§6 路线图」与「附录 C 参考」

---

## §0  序言：为什么需要"宗旨"

经 v6-v21 系统清理后，GNFS C++20 实现已达工业级稳定（8/8 progressive、gate 27/27、stress L1 通过）。**算法层 90% 的"低悬果实"已摘**：

- 已做: SGE 预处理、PackedGF2 word-pack、Block Lanczos 并行 SpMV、Bucket sieve、SQUFOF k=1 D=4N、Couveignes Gray code、Murphy E thread_local、Schirokauer atomic、OOC mmap-CSR
- 未做（真正未开荒地）:
  1. **PGO**（项目零配置）
  2. **NEON SIMD 仅 1 处** (`lattice_sieve::collect_candidates`)
  3. **`[[likely]]/[[unlikely]]` 仅 1 处** (`trial_division.hpp`)
  4. **无显式 `__builtin_prefetch`**
  5. **未使用 SME** (M4+ 矩阵协处理器)
  6. **无 Instruments + PMU 闭环**
  7. ~~BlockWiedemann 真 block BM~~ ✅ **已完成 2026-05-14** (Coppersmith matrix BM, 48× speedup on 62K×10K, §6 P2 详见)

再做"试错式"调参或猜测式优化已无收益；必须建立**数据驱动 + 硬件感知 + 理论严谨**的工程纪律。本文档就是这个纪律的成文化。

---

## §1  核心宗旨：六条铁律

### 铁律 1：测量先于优化

> **没有 profile 数据的改动，一律拒绝合并。**

- 任何 `perf(...)` commit 必须附 before/after 实测数据（含 CPU 时间、cycles、cache miss 率三项至少其一）
- 「直觉热点」一律先 profile 验证，不允许"我觉得这里慢"
- 禁止仅凭 release 计时下结论；必须配合 PMU 计数器（cycles / instructions / branch_misses / dcache_load_miss）
- **本项目实战**: `./scripts/test.sh bench --save` 接入 PGO 前后对比；自定义 `.tracetemplate` 跑 Instruments

### 铁律 2：理论先于试错

> **能查论文获得正解的，不允许试参数。能写更优算法的，不允许微调常数。**

- 改算法核心前**必须**有论文/权威实现支撑（CADO-NFS、msieve、Pomerance、Crandall-Pomerance）
- 不允许在"调一调 prime cap 试试" / "改 buffer 大小看看" 上花超过 10 分钟
- 「奇怪 bug 改了就好」要追根，不接受"先放着"
- **本项目实战**: Couveignes f'(α)² 修正引 Thomé 2008（commit `eba0c7c`）、α-projective-root 锁定 Guillevic-Singh 2021（commit `075ef73`）

### 铁律 3：利用硬件，不糟蹋硬件

> **M5 给的 NEON / SME / 分支预测器 / 缓存层级必须吃透；不能让 4.61 GHz P-core 跑串行 cache-miss 循环。**

- 热点循环必须能解释"为什么这条指令在这一个周期能完成"
- 数据布局必须有意识：cache line 64 B、L1D 128 KB、L2 共享池
- `-mcpu=native` 已开（CMakeLists L21-25）— 但要知道它**没**自动开 PGO、没自动 SME、没自动 prefetch
- E-core 不是 P-core 的弱化版，它是**完全不同的处理器**；后台任务该分流 E-core

### 铁律 4：数据布局先于算法实现

> **Cache-friendly 是底线，不是奖励。L2 miss 一次损失 30+ ns，是 4.61 GHz 下 130+ cycle。**

- 默认 SoA 而非 AoS；除非 access pattern 明显是 row-wise
- Hot-path 数据结构必须 64 B 对齐（`alignas(64)`）
- 大表（>1 MB）必须考虑 mmap + `MADV_SEQUENTIAL/RANDOM`
- Packed bitmaps 优先于 `std::vector<bool>`，且必须 word-aligned
- **本项目实战**: `PackedGF2Matrix`（64-bit word）、`MmapCSRMatrix`（uint64 row_offsets）— 这是榜样

### 铁律 5：并行是放大器，不是补丁

> **串行慢 100 ns 的循环并行后只会更慢；并行前先把串行做到最优。**

- 任何并行化前先 profile 单核 IPC；IPC < 2.0 时并行收益打折
- ThreadPool 不是免费的：task overhead ~1-10 μs；粒度必须 >> 100 μs
- 锁是性能杀手：`std::atomic` 也不便宜；优先无锁数据结构 / per-thread 累加
- **本项目实战**: Bucket sieve 多线程 scatter（commit `41aead9` 系列）、ThreadPool work-stealing

### 铁律 6：闭环验证，不轻信编译器

> **`-O3 -mcpu=native -flto=thin` 不是魔法。编译器优化掉的代码可能正是你以为的"热点"，留下来的可能是 cache 灾难。**

- 关键改动后必须看 disassembly（`objdump -d` / Compiler Explorer）
- IPC 突然下降是红灯；branch_misses 突然升高是红灯
- Release build 跑得通 ≠ Debug+Sanitizers 跑得通；性能改动**必须**两边都跑（CLAUDE.md「跨平台编译注意事项」明确要求）

---

## §2  分析方法论：M5 上的 Top-Down 范式

### 2.1  Intel TMA 类比 → ARM / Apple 等价

Intel 提出的 **Top-Down Microarchitecture Analysis (TMA)** 已成行业标准，将 CPU 时间分解为 4 大类：

```
                   Total Pipeline Slots
                   /                  \
                Issued                Not Issued
               /      \                   |
        Retiring     Bad Speculation     /  \
                                Frontend  Backend
                                Bound     Bound
                                          /    \
                                     Core      Memory
                                     Bound     Bound
```

| TMA 类别 | 含义 | M5 PMU event (近似) | 主要修法 |
|---------|------|--------------------|---------|
| **Retiring** | 真正干活 | `INST_ALL` / `CYCLES` | 已经在干活，看 IPC 是否够高（M5 P-core 理论 ≥8） |
| **Bad Speculation** | 分支误判浪费 | `BRANCH_MISPRED_NONSPEC` | 减少 unpredictable branch / 加 `[[likely]]` / branchless 算法 |
| **Frontend Bound** | 指令取不到 / 解码慢 | `INST_DECODE_STALL` (M3+) | 减小 code footprint / 避免巨型函数 / inline 谨慎 |
| **Backend Core Bound** | 执行单元打满 | `PMC_CORE_ACTIVE_STALL` | SIMD 化 / ILP 提升 / 减少 dependency chain |
| **Backend Memory Bound** | 等内存 | `L1D_CACHE_MISS_LD`, `L2_TLB_MISS` | prefetch / 数据布局 / blocking |

**判定流程**:
1. 跑 Instruments → CPU Counters 选 "Microarchitecture Analysis" preset (WWDC25 新增)
2. 看哪类占比 > 30%
3. 对照表选修法
4. 改完重测，**期望该类占比下降 + IPC 上升**

### 2.2  Apple Silicon PMU 全景

M5 (和 M3/M4 一脉相承) 提供：

- **2 fixed counters**: cycles, instructions
- **8 configurable counters**: 任选 PMU event
- **M3+ ESR 寄存器**: 64-bit, 每个 event 16-bit（M1/M2 是 8-bit/event）

可用 event（不完全列表，via reverse-engineered kperf framework）:

| Event | 用途 |
|-------|------|
| `INST_ALL` | 总指令数（fixed） |
| `CORE_ACTIVE_CYCLE` | 活跃周期（fixed） |
| `INST_BRANCH` | 分支指令 |
| `BRANCH_MISPRED_NONSPEC` | 误判分支 |
| `L1D_CACHE_MISS_LD` | L1D load miss |
| `L1D_CACHE_MISS_ST` | L1D store miss |
| `L1I_CACHE_MISS` | L1I miss |
| `L2_TLB_MISS_INST` | iTLB miss |
| `L2_TLB_MISS_DATA` | dTLB miss |
| `INST_INT_LD`, `INST_INT_ST` | 整数 load/store |
| `INST_NEON` | NEON 指令数 |
| `INST_SIMD_LD`, `INST_SIMD_ST` | SIMD load/store |
| `MAP_DISPATCH_BUBBLE` | 前端 bubble |

**关键比率**:
- IPC = `INST_ALL / CORE_ACTIVE_CYCLE` （目标 P-core ≥ 4，理想 ≥ 6）
- L1D miss rate = `L1D_CACHE_MISS_LD / INST_INT_LD` （目标 < 2%）
- Branch mispred rate = `BRANCH_MISPRED_NONSPEC / INST_BRANCH` （目标 < 1%）

### 2.3  Instruments 三件套

Xcode 16+ 安装后开箱可用（无需付费开发者账号）：

| 工具 | 用途 | 何时用 |
|------|------|--------|
| **Time Profiler** | 火焰图 / 函数级 wall time | 首次 profile / 找宏观热点 |
| **CPU Counters** | PMU event 计数 + Top-down | 第二轮 / 微架构瓶颈定位 |
| **Processor Trace** | 每条指令级 trace (M3+ 独占) | 极小热点 / 异常分支研究 |

**WWDC25 #308 关键更新**: CPU Counters 新增 **preset modes** ("CPU Bottlenecks", "Instructions Retired"等)，避免手动配 8 个 event。

**典型工作流**:
```bash
# 1. 编译 Release+debug info (推荐 RelWithDebInfo)
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -C build -j$(sysctl -n hw.ncpu) test_factor_with_kleinjung

# 2. 启动 Instruments（GUI）
open -a Instruments build/test_factor_with_kleinjung

# 3. 选 "CPU Counters" template，选 "CPU Bottlenecks" preset
# 4. Record → 跑 ~30s → Stop → 查 Top-down breakdown

# 5. 或 CLI 化（更可重复）
xctrace record --template 'CPU Counters' \
    --launch build/test_factor_with_kleinjung \
    --output /tmp/gnfs-cpu.trace
xctrace export --input /tmp/gnfs-cpu.trace --xpath '//*' --output /tmp/gnfs-cpu.xml
```

### 2.4  CLI 化 + 自动化

GUI Instruments 不利于回归追踪。本项目需要建立 **`.tracetemplate` + xctrace + 报告解析** 流水线（§5 详述）。

参考工具:
- `xctrace` — Apple 官方 CLI（Xcode 11+；instruments CLI 2024 已弃用）
- `samply` — 第三方采样 profiler，输出 Firefox Profiler 兼容（推荐快速看火焰图）
- `mperf` — 第三方 PMU CLI（macOS ARM64 hardware counters）
- `asitop` — top-style Apple Silicon 监控（看 SoC 全局，不看进程级）

### 2.5  火焰图工作流

```bash
# samply: cargo install samply
samply record build/test_factor_with_kleinjung
# 自动打开 Firefox Profiler，含火焰图 + 调用栈 + 时间轴

# 替代：Instruments Time Profiler 导出 → speedscope
xctrace record --template 'Time Profiler' --launch ./bin --output /tmp/t.trace
# 用 speedscope.app 加载 /tmp/t.trace
```

### 2.6  GNFS 专用基准接入

现有基准（来自 CMakeLists）:
- `test_perf_targets` — 10-70 digit 性能目标
- `test_gnfs_bench` — 20-35 digit 流水线基准
- `test_stress` — 50/60-digit 极限测试

**框架接入要求** (§5 实施):
- 每个基准包装 PGO instrument/use 两阶段
- 接入 `xctrace` 抓 Top-down 报告 → CSV
- `./scripts/test.sh bench --save` 后端保存到 `bench/results/YYYY-MM-DD-HHMMSS/`
- 比较脚本输出 markdown diff

---

## §3  优化技术目录（按抽象层从高到低）

> **优化次序公理**: 算法 → 数据布局 → 指令 → 内存 → 并行 → 编译器 → I/O
>
> 跳过上层做下层是浪费工时（如未排除 O(n²) 算法就上 NEON）。

### 3.1  算法层

| 技术 | 何时用 | 注意 |
|------|--------|------|
| **复杂度降阶** | O(n²) → O(n log n) | 论文支撑必备 |
| **预计算 / 表查** | 重复计算 small domain | 内存 vs CPU trade-off |
| **增量更新** | 输入小幅变化 | 注意状态正确性（如 Couveignes Gray code commit `48c107e`） |
| **Early reject / short-circuit** | 候选过滤 | 顺序按"廉价检查在前" |
| **分而治之 / Block** | 数据 > cache | block size = L1 / L2 / L3 三档 |
| **算法替换** | 旧算法已饱和 | 如 line sieve → lattice sieve (5× 加速) |

### 3.2  数据布局

```cpp
// Cache line 对齐（M5: 64 B）
struct alignas(64) HotState {
    std::atomic<uint64_t> counter;  // 独占一行
    char pad[64 - sizeof(std::atomic<uint64_t>)];
};

// SoA 优先（cache-friendly + SIMD-friendly）
struct Bad_AoS { struct { double x, y, z; } points[N]; };
struct Good_SoA { double x[N], y[N], z[N]; };

// Packed bitmaps（已用：PackedGF2Matrix）
using Word = uint64_t;  // 64-bit packed，避免 std::vector<bool> 单 bit overhead
```

| 技术 | 用途 | 项目内例 |
|------|------|---------|
| `alignas(64)` | False sharing / 缓存行独占 | 尚未广泛使用，是 hot-path 改进点 |
| Packed bit storage | GF(2) 矩阵 | `PackedGF2Matrix` |
| SoA layout | SIMD-friendly | Relations 当前是 AoS，可重审 |
| mmap | 超内存数据 | `MmapCSRMatrix`, `OOCRelationStore` |
| `madvise(MADV_*)` | 提示 kernel 访问模式 | 项目未显式调用，是补丁项 |

### 3.3  指令级（NEON / SME / 内建）

**NEON (128-bit, 已在 M1-M5 通用)**:
```cpp
#include <arm_neon.h>

// 例：16-byte 阈值比较（lattice_sieve.collect_candidates 已用）
uint16x8_t vals = vld1q_u16(&buf[i]);
uint16x8_t thresh = vdupq_n_u16(eff_thresh);
uint16x8_t cmp = vcgeq_u16(vals, thresh);
// Quick reject: 整个 128-bit 比较结果为 0 → 跳过 8 个 lane
uint64x2_t cmp64 = vreinterpretq_u64_u16(cmp);
if ((vgetq_lane_u64(cmp64, 0) | vgetq_lane_u64(cmp64, 1)) == 0) continue;
```

**常用 NEON intrinsics** (附录 B 完整列表):

| 类别 | intrinsic | 用途 |
|------|-----------|------|
| Load/Store | `vld1q_u64`, `vst1q_u64` | 16 B 对齐内存 |
| 整数算术 | `vaddq_u64`, `vsubq_u64`, `vmulq_u32` | 2× u64 / 4× u32 并行 |
| 位运算 | `veorq_u64`, `vandq_u64`, `vorrq_u64` | GF(2) 矩阵核心 |
| 比较 | `vcgeq_u16`, `vceqq_u32` | 阈值 / 等值过滤 |
| 移位 | `vshlq_n_u64`, `vshrq_n_u64` | 位 pack/unpack |
| 水平归约 | `vaddvq_u64`, `vmaxvq_u32` | 收尾求和/最大 |
| Pairwise | `vpaddq_u64`, `vpminq_u32` | tree reduction |

**编译器内建**:
```cpp
__builtin_expect(cond, 1)         // 等价于 C++20 [[likely]]，旧 lambda/未来 if-init 可用
__builtin_prefetch(ptr, 0, 3)     // (read, T0) — 拉到 L1
__builtin_prefetch(ptr, 1, 3)     // (write, T0)
__builtin_unreachable()           // 帮编译器消除死分支
__builtin_clz(x)                  // count leading zeros
__builtin_popcountll(x)           // popcount（M5 有专用指令）
__builtin_ctzll(x)                // trailing zeros
```

**C++ 属性**:
```cpp
if (x > 0) [[likely]] { ... }
if (err) [[unlikely]] { return; }

[[gnu::hot]] void hot_function();    // 编译器优先优化 + 同段定位
[[gnu::cold]] void error_path();     // 移到独立 section
[[gnu::flatten]] void f();           // 强制 inline 所有 callee
[[gnu::noinline]] void boundary();   // 反向：稳定函数边界利于 profile
```

### 3.4  内存子系统

| 技术 | 目的 | 注意 |
|------|------|------|
| `__builtin_prefetch` | 隐藏 cache miss | 预取距离需调（8-32 cache line） |
| `madvise(MADV_SEQUENTIAL)` | 大文件顺序读 | 立即生效 |
| `posix_madvise(MADV_WILLNEED)` | 预读 | 与 `mmap` 配合 |
| `mlock` | 防换出 | 谨慎使用 |
| `_Alignas` / `alignas(64)` | 缓存行对齐 | 内核接口数据结构 |
| `__restrict__` | 别名优化 | C++ 标准不含，Apple Clang 支持 |

**Apple Silicon 注意**:
- macOS 默认 16 KB page（vs Linux 4 KB），page table 压力小
- 没有 transparent huge pages 概念（与 x86 不同）
- 内存控制器统一寻址（UMA），无 NUMA distance

### 3.5  并行

| 技术 | 何时用 | 注意 |
|------|--------|------|
| **ThreadPool** | 同质 task 批处理 | 已有 `gnfs::util::ThreadPool` + work-stealing |
| **`std::async`** | 一次性异步 | 注意默认 launch policy |
| **`std::execution::par`** (C++17) | STL 算法并行 | Apple libc++ 部分实现 |
| **Apple GCD (libdispatch)** | macOS 原生 | M5 上调度更优，但 cross-platform 受限 |
| **`pthread_set_qos_class_self_np`** | E-core / P-core 调度提示 | macOS 专属 |

**QoS Class 速查**:

```cpp
#include <pthread/qos.h>

pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);  // 最高，强制 P-core
pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);    // 高，倾向 P-core
pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);            // 中，可能 E-core
pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);         // 最低，强制 E-core
```

**本项目实战策略**:
- Sieving / Linear algebra 主线程 → `QOS_CLASS_USER_INITIATED`
- I/O flush / log 后台线程 → `QOS_CLASS_UTILITY`
- 监控线程 → `QOS_CLASS_BACKGROUND`

### 3.6  编译器优化

当前 CMakeLists 已有:
- `-O3 -mcpu=native` (Apple Silicon)
- `-flto=thin` (Apple Clang)
- `-Wall -Wextra -Wpedantic`

**缺失项 (P0/P1 待补)**:

| 选项 | 收益 | 工作量 |
|------|------|--------|
| **PGO** (`-fprofile-instr-generate` → `-fprofile-instr-use`) | 5-20% （热分支预测 + inline 决策） | CMakeLists + scripts 改造（§5） |
| `-fprofile-use=...` 自动化 | 通常含在 PGO 中 | 同上 |
| `-fvirtual-function-elimination` | 仅 LTO 下生效 | 已隐含 |
| `-fno-semantic-interposition` | 防 libc++ symbol interpose | 默认开（Apple Clang） |
| `-fno-omit-frame-pointer` (Release+samply) | profile 准确性 | trade-off：少 1 register |

**PGO 接入提案**（§5 完整代码）:
```cmake
option(GNFS_ENABLE_PGO_GEN "Enable PGO profile generation" OFF)
option(GNFS_ENABLE_PGO_USE "Enable PGO profile use" OFF)
set(GNFS_PGO_PROFILE_DIR "${CMAKE_BINARY_DIR}/pgo-profiles" CACHE PATH "...")

if(GNFS_ENABLE_PGO_GEN)
    add_compile_options(-fprofile-instr-generate)
    add_link_options(-fprofile-instr-generate)
endif()

if(GNFS_ENABLE_PGO_USE)
    add_compile_options(-fprofile-instr-use=${GNFS_PGO_PROFILE_DIR}/merged.profdata)
    add_link_options(-fprofile-instr-use=${GNFS_PGO_PROFILE_DIR}/merged.profdata)
endif()
```

### 3.7  I/O

| 技术 | 目的 | 用法 |
|------|------|------|
| `mmap` | 零拷贝 + 按需读 | 已用：`MmapFile`, `MmapCSRMatrix` |
| `posix_fadvise` | 文件访问模式提示 | Apple 上有限支持 |
| `O_DIRECT` 等价 | 绕过 page cache | macOS 用 `F_NOCACHE` (`fcntl`) |
| `setvbuf`/`pubsetbuf` | I/O 缓冲 | 已用：`OOCRelationWriter` |
| 异步 I/O (`dispatch_io`) | 重叠 I/O 与计算 | 项目未用，重 I/O 场景可选 |

---

## §4  Apple M5 调优手册

### 4.1  M5 微架构关键参数 (速查)

| 参数 | P-core | E-core |
|------|--------|--------|
| 数量 | 4 | 6 |
| 时钟 (max) | 4.61 GHz | ~2.4 GHz |
| 微架构代号 | 4th-gen Sawtooth | 4th-gen Sawtooth |
| ISA | ARMv9.2-A | ARMv9.2-A |
| L1 I | 192 KB | 128 KB |
| L1 D | 128 KB | 64 KB |
| L2 (cluster) | 24-32 MB (共享) | ~4 MB (共享) |
| SLC (chip-wide) | 28 MB+ | 同上 |
| Cache line | 128 B（注意：macOS 报告 128 B，部分 prefetcher 按 64 B 工作） |
| SIMD | NEON 128-bit (4× FP32 / 2× FP64 / 16× I8) | NEON 128-bit |
| SVE | 仅供 SME 协处理器 (M4+) | 同 |
| SME | ✅ (M4 起, M5 称 Neural Accelerators) | ✅ |
| 矩阵存储 ZA tile | 512 B per tile (32 tiles for SVL=64 B) | 同 |

**重要校准**: Apple 报告的"P-core L1D = 128 KB" 是单核数据；L2 在 cluster 内共享。L1I = 192 KB 远大于 x86 同档（M5 偏向超大 L1I，对长函数体宽容）。

### 4.2  P-core / E-core 分工实战

```cpp
#include <pthread/qos.h>
#include <pthread.h>

// 主线程：sieve / linalg → P-core
int main() {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    // ...
}

// 后台 I/O / 监控 → E-core
std::thread bg([](){
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    // flush relations, log, ...
});

// ThreadPool 工作线程 → P-core
// 在 ThreadPool::start() 中：
worker_thread = std::thread([this]() {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    // run tasks
});
```

**禁用 E-core 仅 P-core 跑**（基准测试用）:
```bash
# taskpolicy 是 macOS 等价 numactl
taskpolicy -c utility ./test_perf_targets   # 强制 E-core 跑（测试 E-core 性能）
taskpolicy -c maintenance ./test_perf_targets  # 同
# 反向：无法直接强制只 P-core；用 QOS_CLASS_USER_INTERACTIVE 隐式
```

### 4.3  NEON 128-bit 实战模板

**模板 A: 阈值过滤 + Quick reject** (已用于 `lattice_sieve`)
```cpp
#ifdef __ARM_NEON
#include <arm_neon.h>
const uint16x8_t thresh_vec = vdupq_n_u16(threshold);
const size_t vec_end = n & ~size_t(7);
for (size_t i = 0; i < vec_end; i += 8) {
    uint16x8_t vals = vld1q_u16(&buf[i]);
    uint16x8_t cmp = vcgeq_u16(vals, thresh_vec);
    uint64x2_t cmp64 = vreinterpretq_u64_u16(cmp);
    if ((vgetq_lane_u64(cmp64, 0) | vgetq_lane_u64(cmp64, 1)) == 0)
        continue;  // 8-lane reject
    for (size_t k = 0; k < 8; ++k)
        if (buf[i+k] >= threshold) process(i+k);
}
#endif
```

**模板 B: GF(2) packed XOR (BL/BW SpMV 核心)**
```cpp
// 等价于 y[i] ^= x[i]; 但每周期处理 128 bits = 2 words
void xor_2words(uint64_t* __restrict y, const uint64_t* __restrict x, size_t n) {
    const size_t vec_end = n & ~size_t(1);
    for (size_t i = 0; i < vec_end; i += 2) {
        uint64x2_t vy = vld1q_u64(&y[i]);
        uint64x2_t vx = vld1q_u64(&x[i]);
        vst1q_u64(&y[i], veorq_u64(vy, vx));
    }
    if (n & 1) y[vec_end] ^= x[vec_end];
}
```

**模板 C: popcount (M5 有 vcnt 专用)**
```cpp
// uint8 popcount → uint64 总和
uint64_t popcount_array(const uint64_t* p, size_t n) {
    uint64x2_t acc = vdupq_n_u64(0);
    const size_t vec_end = n & ~size_t(1);
    for (size_t i = 0; i < vec_end; i += 2) {
        uint8x16_t bytes = vreinterpretq_u8_u64(vld1q_u64(&p[i]));
        uint8x16_t pop = vcntq_u8(bytes);  // 16× per-byte popcount
        acc = vaddq_u64(acc, vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(pop))));
    }
    uint64_t total = vgetq_lane_u64(acc, 0) + vgetq_lane_u64(acc, 1);
    for (size_t i = vec_end; i < n; ++i) total += __builtin_popcountll(p[i]);
    return total;
}
```

### 4.4  SME (Scalable Matrix Extension) — M4+ 矩阵协处理器

> **状态**: 前沿；macOS 文档极少；CADO-NFS 未利用；潜在收益巨大（BL/BW 64×N SpMV）。

**SME 关键事实** (来自 tzakharko/m4-sme-exploration):
- SVL (Streaming Vector Length) on M4: **64 字节 = 512 bit**
- ZA 矩阵存储: 32 个 tile，每个 SVL × SVL = 64 × 64 B = 4 KB
- 仅在 "streaming mode" 下访问；SVE 寄存器是协处理器视图
- 单独的 throughput / latency 特性（不与主 NEON pipeline 共享）

**适用判定**（不是所有矩阵运算都该上 SME）:
- ✅ 大矩阵（≥ 256×256）密集乘法
- ✅ 重复执行（一次 init 摊销）
- ❌ 稀疏矩阵（SpMV 在 SME 上 IPC 不一定优于 NEON）
- ❌ 短向量（streaming mode 切换 ~100 cycle overhead）

**对 GNFS 的潜在用法**:
- ❓ BL/BW SpMV: 矩阵稀疏，但 packed 64×64 block 内是稠密 → 可探索 ZA tile 内积
- ❓ Murphy E `compute_alpha` (78k 素数循环): 不适合（标量主导）
- ❓ Couveignes 多素数 Gray code: 不适合（branchy）

**最小可行实验** (FYI):
```c
// 需要 clang 18+ + ARMv9.2-A target，启用 -march=armv9-a+sme
#include <arm_sme.h>
// SVE intrinsic
__arm_streaming void sme_kernel(svfloat32_t* a, svfloat32_t* b, svfloat32_t* c) {
    // FMOPA: outer product accumulate into ZA tile
    svfloat32_t va = svld1_f32(svptrue_b32(), (float*)a);
    svfloat32_t vb = svld1_f32(svptrue_b32(), (float*)b);
    svmopa_za32_f32_m(0, svptrue_b32(), svptrue_b32(), va, vb);
}
```

**建议**: §6 路线图 P2，先把 NEON 全部吃干后再考虑。需要专项实验 + reverse-engineered SME PMU 才能验证收益。

### 4.5  分支预测 & ARMv9 新特性

M5 是 4th-gen Sawtooth，相比 M4 增强的分支预测器（Apple 官方说"new branch prediction"）。

**实战意义**:
- 间接调用（virtual function, function pointer）开销持续下降，但仍 > 直接调用
- 紧凑分支（if-else 链）预测器表现优于稀疏分支
- `__builtin_expect` / `[[likely]]` 影响**指令布局**（hot path linear, cold path jump out）

**ARMv9.2-A 新增** (M5 已支持):
- BFloat16（AI 训练）— GNFS 不需要
- I8MM (Int8 matrix multiply) — GNFS 不需要
- MTE (Memory Tagging Extension) — 调试用
- Speculation Barrier (`SB` instruction) — Spectre 缓解；性能场景**避免**

---

## §5  首战路径：Instruments 闭环 + PGO

### 5.1  阶段划分

```
S1. CMakeLists PGO 改造          (2-4h)
S2. PGO 训练脚本                 (2h)
S3. Instruments tracetemplate 抓取 (3-4h)
S4. 报告解析 + diff 工具         (2-3h)
S5. ./scripts/test.sh bench 集成 (2h)
S6. 首次基线 + PGO 收益评估       (2h)
─────────────────────────────────
预计总工时：15-20h（1-2 个会话）
```

### 5.2  S1: CMakeLists PGO 改造

```cmake
# ===== PGO Support =====
option(GNFS_ENABLE_PGO_GEN "Enable PGO instrumentation (training run)" OFF)
option(GNFS_ENABLE_PGO_USE "Enable PGO optimized build (consume profile)" OFF)
set(GNFS_PGO_PROFILE_DIR "${CMAKE_BINARY_DIR}/pgo-profiles"
    CACHE PATH "Directory for PGO .profraw / merged.profdata")

if(GNFS_ENABLE_PGO_GEN AND GNFS_ENABLE_PGO_USE)
    message(FATAL_ERROR "Cannot enable both PGO_GEN and PGO_USE")
endif()

if(GNFS_ENABLE_PGO_GEN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "PGO requires Clang")
    endif()
    file(MAKE_DIRECTORY ${GNFS_PGO_PROFILE_DIR})
    add_compile_options(-fprofile-instr-generate)
    add_link_options(-fprofile-instr-generate)
    # LLVM_PROFILE_FILE 在运行时通过 env var 设置：
    #   LLVM_PROFILE_FILE=${GNFS_PGO_PROFILE_DIR}/%m-%p.profraw
endif()

if(GNFS_ENABLE_PGO_USE)
    set(_pgo_data "${GNFS_PGO_PROFILE_DIR}/merged.profdata")
    if(NOT EXISTS ${_pgo_data})
        message(FATAL_ERROR "PGO_USE: profile not found at ${_pgo_data}. Run training first.")
    endif()
    add_compile_options(-fprofile-instr-use=${_pgo_data})
    add_link_options(-fprofile-instr-use=${_pgo_data})
    # 必备：-Wno-profile-instr-out-of-date 防止小代码改动后强失败
    add_compile_options(-Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled)
endif()
```

### 5.3  S2: PGO 训练脚本

`scripts/pgo-train.sh`:
```bash
#!/usr/bin/env zsh
set -euo pipefail

ROOT="$(cd "$(dirname "${0}")/.." && pwd)"
BUILD_GEN="${ROOT}/build-pgo-gen"
BUILD_USE="${ROOT}/build-pgo-use"
PROFILE_DIR="${BUILD_GEN}/pgo-profiles"

echo "== Phase 1: Instrumented build =="
cmake -B "${BUILD_GEN}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DGNFS_ENABLE_PGO_GEN=ON \
    -DGNFS_PGO_PROFILE_DIR="${PROFILE_DIR}"
make -C "${BUILD_GEN}" -j$(sysctl -n hw.ncpu) test_factor_with_kleinjung test_lattice_sieve test_linalg

echo "== Phase 2: Training runs =="
mkdir -p "${PROFILE_DIR}"
export LLVM_PROFILE_FILE="${PROFILE_DIR}/%m-%p.profraw"

# 训练样本：选择能覆盖主要 code path 的工作负载
# - test_factor_with_kleinjung: 完整 GNFS pipeline (27/40-bit)
# - test_lattice_sieve: sieve 热点
# - test_linalg: BL/BW 热点
"${BUILD_GEN}/test_factor_with_kleinjung"
"${BUILD_GEN}/test_lattice_sieve"
"${BUILD_GEN}/test_linalg"

echo "== Phase 3: Merge profiles =="
xcrun llvm-profdata merge -output="${PROFILE_DIR}/merged.profdata" "${PROFILE_DIR}"/*.profraw

echo "== Phase 4: PGO-optimized build =="
cmake -B "${BUILD_USE}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DGNFS_ENABLE_PGO_USE=ON \
    -DGNFS_PGO_PROFILE_DIR="${PROFILE_DIR}"
make -C "${BUILD_USE}" -j$(sysctl -n hw.ncpu)

echo "== Done. PGO-optimized binaries in ${BUILD_USE}/ =="
echo "Compare:"
echo "  time ${ROOT}/build/test_factor_with_kleinjung   # baseline"
echo "  time ${BUILD_USE}/test_factor_with_kleinjung    # PGO"
```

### 5.4  S3: Instruments tracetemplate 自动化

创建 `scripts/perf/cpu-counters.tracetemplate`（GUI 中导出）后:

```bash
# scripts/profile-cpu.sh
#!/usr/bin/env zsh
set -euo pipefail
BIN="${1:?usage: $0 <binary> [args...]}"
shift
OUT="${ROOT}/bench/results/$(date +%Y-%m-%d-%H%M%S)-$(basename ${BIN}).trace"
mkdir -p "$(dirname ${OUT})"

xctrace record \
    --template '/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/Library/Instruments/Templates/CPU Counters.tracetemplate' \
    --launch "${BIN}" "$@" \
    --output "${OUT}"

# 导出 XML 供后续解析
xctrace export --input "${OUT}" --xpath '/trace-toc/run/instrument' --output "${OUT}.xml"
echo "Trace: ${OUT}"
echo "XML:   ${OUT}.xml"
```

### 5.5  S4: 报告解析 + diff

`scripts/perf/parse-trace.py` (Python 3, 解析 xctrace XML):
```python
#!/usr/bin/env python3
"""Parse xctrace export and emit markdown summary."""
import sys, xml.etree.ElementTree as ET
from pathlib import Path

def parse_counters(xml_path: Path) -> dict:
    tree = ET.parse(xml_path)
    counters = {}
    for row in tree.iter('row'):
        ev = row.findtext('event')
        val = row.findtext('value')
        if ev and val:
            counters[ev] = int(val.replace(',', ''))
    return counters

def derived_metrics(c: dict) -> dict:
    return {
        'IPC':              c.get('INST_ALL', 0) / max(c.get('CORE_ACTIVE_CYCLE', 1), 1),
        'L1D_miss_rate':    c.get('L1D_CACHE_MISS_LD', 0) / max(c.get('INST_INT_LD', 1), 1),
        'BR_mispred_rate':  c.get('BRANCH_MISPRED_NONSPEC', 0) / max(c.get('INST_BRANCH', 1), 1),
        'iTLB_miss_per_inst': c.get('L2_TLB_MISS_INST', 0) / max(c.get('INST_ALL', 1), 1),
    }

if __name__ == '__main__':
    if len(sys.argv) == 2:
        c = parse_counters(Path(sys.argv[1]))
        m = derived_metrics(c)
        for k, v in m.items():
            print(f"{k:30s}: {v:.4f}")
    elif len(sys.argv) == 3:  # diff mode
        a = derived_metrics(parse_counters(Path(sys.argv[1])))
        b = derived_metrics(parse_counters(Path(sys.argv[2])))
        print(f"| Metric | Before | After | Δ% |")
        print(f"|---|---|---|---|")
        for k in a:
            delta = (b[k] - a[k]) / a[k] * 100
            print(f"| {k} | {a[k]:.4f} | {b[k]:.4f} | {delta:+.2f}% |")
```

### 5.6  S5: 集成到 test.sh

`scripts/test.sh` 增加新子命令:
```bash
# 在 case 中：
case "$cmd" in
    # ... existing ...
    pgo-train)
        exec "${SCRIPT_DIR}/pgo-train.sh" "$@"
        ;;
    profile)
        # ./scripts/test.sh profile <test_name>
        local test_bin="${BUILD_DIR}/test_${1:-factor_with_kleinjung}"
        exec "${SCRIPT_DIR}/perf/profile-cpu.sh" "${test_bin}" "${@:2}"
        ;;
esac
```

### 5.7  S6: 首次基线

执行顺序:
```bash
# 1. baseline (无 PGO)
cd /Users/mayiding/Desktop/GitMy/GNFS
./scripts/test.sh build  # 现有 Release build
time ./build/test_factor_with_kleinjung   # 记录 wall time
./scripts/test.sh profile factor_with_kleinjung  # 抓 trace
python3 scripts/perf/parse-trace.py bench/results/<timestamp>.trace.xml > bench/results/<timestamp>-baseline.md

# 2. PGO build
./scripts/test.sh pgo-train

# 3. PGO profile
./scripts/test.sh profile factor_with_kleinjung
# trace 路径同上，新 timestamp
python3 scripts/perf/parse-trace.py bench/results/<new>.trace.xml > bench/results/<new>-pgo.md

# 4. diff
python3 scripts/perf/parse-trace.py \
    bench/results/<timestamp>-baseline.trace.xml \
    bench/results/<new>-pgo.trace.xml \
    > bench/results/<date>-pgo-impact.md
```

**期望 PGO 收益** (基于行业 baseline):
- 整体 wall time: -5% 至 -20%
- IPC: +5% 至 +15%
- Branch mispred rate: -10% 至 -40%
- Code size: ±3%（hot inline 增 / cold 缩）

**判定标准** (是否合入):
- 必要: wall time 不退化（小幅波动 ±2% 接受）
- 充分: 至少一个 derived metric 提升 ≥ 5%
- 风险: PGO 二进制对训练样本 overfit — 必须用**未参与训练**的样本（如 test_gnfs_bench）验证

---

## §6  本项目优化路线图

### P0 — 已完成 (2026-05-12)

- ✅ **本文档** (performance-doctrine.md)
- ✅ **§5 完整实施**: Instruments 闭环 + PGO 接入
  - CMakeLists `GNFS_ENABLE_PGO_GEN/USE` (commit `5ac809d`)
  - `scripts/pgo-train.sh` 4-phase 自动化 (commit `31a4b95`)
  - `scripts/perf/profile-cpu.sh` xctrace recorder (commit `3a42087`)
  - `scripts/perf/parse-trace.py` TMA 4-col diff parser (commit `0d6da7c`)
  - `scripts/test.sh pgo-train` / `profile` 子命令 (commit `099018c`)
- ✅ 首次 PGO 影响评估: `bench/results/2026-05-12-pgo-impact.md`
  - Verdict: PGO 接受为可工作 baseline；wall time -2.09% (median)，PMU total counter -7.11%
  - 训练样本过度集中（factor_with_kleinjung 既是训练又是评估）— 建议扩展样本集
  - 决定: PGO **保留为 opt-in**，不默认开启；用于 release / 基准发布

### P1.A — 已完成 (2026-05-13)

xctrace "CPU Counters" 4 列聚合（P0 用）无法区分 MemBound vs CoreBound。P1.A 切换到 mperf + as5.plist (M5 PMU 数据库)，采集 10 个真实事件：

- `scripts/perf/install-mperf.sh` — mperf 外部工具安装 (commit `c47f6a5`)
- `scripts/perf/pmu-stat.sh` — 10-event wrapper (commit `83f9b76`, `25dcd0c`, `64b449f`)
- `scripts/perf/pmu-derive.py` — JSON 解析 + 派生指标 + P1 分类器 (commit `83f9b76`, `c1bd380`)
- `scripts/test.sh pmu` 子命令 (commit `1f18bbb`)
- 首次实测报告: `bench/results/2026-05-13-pmu-deepening.md` (commit `509bbf4`)

**实测决策依据（test_factor_with_kleinjung baseline）**:

| Metric | 值 | 阈值 | 触发 |
|---|---:|---:|---|
| BackendStallRate   | **74.79%** | >30% | ✅ |
| L1DMissRate        | **12.80%** | >5%  | ✅ |
| BranchMispredRate  | 0.55%      | >5%  | ❌ |
| FrontendStallRate  | 2.26%      | >20% | ❌ |
| SIMDDensity        | 5.85%      | -    | n/a |

→ **MemBound 触发**，BadSpec / FrontendBound / CoreBound 均不触发。

### P1.B — MemBound 治理 (基于 P1.A 实测 + 2026-05-14 attribution 校准)

P1.A 锁定 MemBound 是宏观结论，但 doctrine 铁律 5（target validation）要求验证：**真正在烧 cycles 的代码段是什么**。2026-05-14 用 Apple `sample` 抓 20s hot-symbol 攻破"哪个函数 cache miss"的 attribution 黑盒。结论与原计划的目标顺序大幅不同。

#### P1.B-1: SpMV prefetch（已实施，pending 大矩阵验证）

- **改动**: `src/linalg/block_wiedemann.cpp` `bw_spmv_forward` / `bw_spmv_transpose` 加 split-loop `__builtin_prefetch`，N_AHEAD=8 (commits `eab6245`, `5dbce80`)
- **正确性**: smoke 26/26 + module linalg 全 PASS + test_block_wiedemann 7/7（含 5400×200 BW 路径）
- **测试结果**: `test_factor_with_kleinjung` PMU 无变化（详见 `bench/results/2026-05-14-spmv-prefetch.md`）
- **根因**: `BlockLanczos::find_dependencies` dispatcher 在 `m × (m+n) ≤ 4 GiB` 时路由到 Gaussian on PackedGF2Matrix (`find_dependencies_sparse`)。 `test_factor_with_kleinjung` 跑 ≤50-bit 案例，**所有矩阵**都走 Gaussian，BW SpMV 0 samples。
- **决策**: 保留改动（零副作用，对 ≥4GiB 矩阵理论生效），但 P1.B-1 视为 implementation-only。 验证路径：`test_25digit`（heavy tier，~1h）或专用 micro-bench。

#### P1.B-1b: Gaussian xor_rows / ThreadPool 竞争（已实施 path B+D，2026-05-14）

**Instrumentation** (commit `1d522cf`，GNFS_DEBUG_GAUSSIAN=1 env-gated) 实测 `test_factor_with_kleinjung` 三个矩阵：

| Run | m | n | aug | pivots | parallel_calls (旧 500 阈值) | serial_subcalls | avg_elim |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | 105095 | 5582 | **1.39 GB** | 5581 | 1099 | 4482 | 7274 |
| 2 | 100813 | 7895 | **1.31 GB** | 7891 | 1100 | 6791 | 5062 |
| 3 | 35928 | 6517 | **186 MB** | 6515 | 1578 | 4937 | 4005 |

**关键发现**：
- `aug_bytes` **全部远超 L2 (8 MB) / L3 (~32 MB)** → row 跳变 cache miss prefetch 有效
- `m` 全部 > 阈值 2000 → ThreadPool 总会启动
- serial_subcalls : parallel_calls ≈ 4-6:1，但 ThreadPool 仍持有 10 个 worker 在 `cv_wait`
- 每 parallel_call 启动 10 submit + 10 future.get，全部走 mutex → 11-16k 同步事件量级与 sample 一致

**修复方案 (commits `14050a3` + `1f5e3bf`)**：

- **Path B**: 并行 elim 阈值 500 → 5000
  - work:overhead 比从 4:1 升到 42:1
  - 实测 parallel_calls 下降 25-28%（Run 1/2），9%（Run 3，elim 分布偏小）
- **Path D**: `xor_rows` 内 `__builtin_prefetch(&aug.data_[elim_rows[i+1] * wpr], rw=1, locality=1)`
  - 在 parallel chunk 内 + serial subcall 内均加
  - 覆盖 186 MB - 1.4 GB aug 的 row-跳变 cache miss

**测试结果（详 `bench/results/2026-05-14-gaussian-threadpool.md`）**：

| 项 | Baseline | Fix | Δ |
|---|---:|---:|---:|
| Wall | 48.29 s | 47.78 s | **-1.06%** |
| **TLBMissRate** | **61.62%** | **52.69%** | **-8.93pp (大改善)** |
| L1DMissRate | 12.86% | 13.06% | +0.20pp (prefetch 自身) |
| BackendStallRate | 73.79% | 74.30% | +0.50pp (噪声内) |
| IPC | 1.323 | 1.326 | +0.003 |
| 正确性 | 100% | 100% | OK |

**核心收益意外集中在 TLB**：aug 186 MB-1.4 GB 跨多 hugepage，`__builtin_prefetch` 不只拉 cache line **还触发 ARM TLB 预取** → page-table walker 提前算好 PTE。L1DMissRate 反而微升（prefetch 自身 cold fetch），但被 TLB -8.93pp 覆盖。

**为什么 wall -1% 而非更多**：
- submit/get overhead 仅占总 wall ~0.2%（11k × 10us / 47s）
- TLB 收益巨大但被 prefetch instruction 自身成本（+2.5% instructions）抵消一半
- **worker idle wait 是 ThreadPool 设计固有成本**，~50k samples，fix 无法消除

**决策**：保留改动（正向、零回归），但 promote **ThreadPool atomic-spin worker** 到独立任务（BACKLOG.md `[OPT] ThreadPool atomic-spin worker`，~200 LOC，risk medium）。

#### P1.B-1c: ThreadPool atomic-spin worker ✅ 已完成 (2026-05-14, commit `138d4d8`)

**修复**: `include/gnfs/util/thread_pool.hpp` — spin-then-cv worker_loop:
- `std::atomic<size_t> queue_size_`：worker spin path 无锁 atomic load
- `kSpinBudget = 2000` iterations × ARM `yield` ≈ M5 P-core ~2-4 μs
- spin 期间任务到达直接 grab；budget 耗尽 fallthrough cv_wait（长闲置节能）
- 接口 100% 兼容（submit / parallel_for / wait_all 全保留）

**PMU 验证** (test_factor_with_kleinjung Release):

| 指标 | Baseline (9fecb96) | Fix | Δ |
|---|---:|---:|---:|
| Wall | 47.924 s | 47.330 s | **-1.24%** ✅ |
| **sys time** | 4.126 s | 3.637 s | **-11.85%** 🎯 |
| user time | 219.254 s | 224.613 s | +2.44% (spin 烤 CPU) |
| Cycles | 95.85e9 | 94.38e9 | -1.54% |
| IPC | 1.316 | 1.326 | +0.010 |
| BackendStallRate | 73.91% | 74.35% | +0.44pp (PET 噪声) |

**关键证据**: sys time -11.85%（489 ms syscall 节省）是 cv_wait 减少的直接测量。user time +2.44% 是 spin loop user-mode CPU 代价。Net wall -1.24% 表示 sys 节省（critical path 上）超过 user 上升（分散在 10 idle workers）。

**教训**: sample 看不到 μs 级 spin（1ms 粒度），__psynch_cvwait 总数几乎不变（-0.17%）。但 PMU sys time 才是 syscall 累积成本的真实测量。**sample / PMU 是互补工具**。

**报告**: [`bench/results/2026-05-14-threadpool-atomic-spin.md`](../../bench/results/2026-05-14-threadpool-atomic-spin.md)

**未做**: spin budget 调优（静态 2000 → 测试 1000/5000）。暂搁置，等 wall time 长 workload 出现回归再启动。

#### P1.B-2: `lattice_sieve` 对齐（**null result，关闭** — 2026-05-14）

- 文件: `include/gnfs/sieve/lattice_sieve.hpp:885-960` (`sieve_row_chunk` 热内循环)
- doctrine 假设: row stride `i_width × 2B` 非 128B(M5 cache line) 整数倍 → 撕裂
- **实测结论**: alignment 不影响热路径
  - sample attribution: `sieve_row_chunk` 占 2103/总 25s × 10 worker × 复用 ≈ **2-4% wall**
  - inner loop 反汇编 6 指令 (offset +1116~+1136)：`ldrh w16, [x15, x11, lsl#1]` (1228 samples) → `add` → `strh` (508) → `add idx` → `cmp` → `b.lo`
  - micro-bench (`/tmp/p1b2_microbench/bench.cpp`)：width 6000(misaligned, mod128=96) vs 6016(aligned, mod128=0) → per-iter **5.79 vs 5.78 ms**，<0.5% 差异，在噪声内
  - 根因: `strh`/`ldrh` 16-bit 永不跨 cache line + M5 HW prefetcher 对小 stride 模式不依赖 row base 对齐
- **未做（也不该做）**: `alignas(64)`、SoA 重构、padding 到 64 元素倍数 — 均无效
- **报告**: [`bench/results/2026-05-14-lattice-sieve-align-null.md`](../../bench/results/2026-05-14-lattice-sieve-align-null.md)
- **教训扩展**: doctrine 铁律 5 在此场景的延伸 — **测量精度 < 改善幅度时，应放弃 micro-optimization**。`test_factor_with_kleinjung` wall 噪声 33% (50-67s)，远超 P1.B-2 即使 lucky 给的 1-2% 改善幅度，**不可分辨即不可优化**

#### P1.B-3: TLBMissRate 60% 调查 ✅ Calibration PASS, follow-up not actionable (2026-05-15)

**Calibration micro-bench** (`bench/microbench/tlb_calibration.cpp`, commit `666c01b`) sweep `bytes` × `access pattern` confirmed `L1D_TLB_MISS` PMU event on M5 is **well-calibrated**:

| Case | TLB_MISS/inst | 解释 |
|---|---:|---|
| small_seq (1 MB / 64 pages, in dTLB) | 0.47% | baseline 噪声 |
| small_rand (1 MB / 64 pages, in dTLB) | 0.28% | random in dTLB 仍 0 |
| large_seq (256 MB / 16384 pages, out) | **12.45%** | PTE prefetch 部分覆盖 |
| large_rand (256 MB / 16384 pages, out) | **32.55%** | 几乎每访问必 miss |

50-100× rate 跳变, random > seq 符合 ARMv9 TLB 行为。`TLB/mem = 246%` (large_rand) 反映 speculative path 多次 PTE fetch。完整报告: [`bench/results/2026-05-15-tlb-calibration.md`](../../bench/results/2026-05-15-tlb-calibration.md).

**结论**:
- ✅ P1.B-1b -8.93pp TLB rate 收益是真的, 不是 PMU 误标
- ❌ **macOS arm64 user-space super-pages 无 API** (VM_FLAGS_SUPERPAGE_SIZE_ANY x86_64-only). 原计划 `madvise(MADV_HUGEPAGE)` 路线作废 (Linux-only)
- 🔄 Follow-up directions (报告 §5):
  - 方向 1: BW SpMV prefetch 在 P2 block path 后已激活 (大矩阵 ≥4GiB) — 不需新代码, doctrine 状态升级
  - 方向 2: linalg pivot order TLB locality — 结构性改动, 潜在收益 < 2%, 风险高, **deferred 至下一轮 sample attribution**
  - 方向 3: 已废

**P1.B 不做的事**:
- NEON 全面化 (SIMDDensity 5.85% 偏低，但 CoreBound **未触发** — 内存才是瓶颈，不是执行宽度)。NEON 推迟到 P1.C，仅当 MemBound 修复后重测仍有 backend stall > 30%
- `[[likely]]/[[unlikely]]` 标注 (BranchMispredRate 0.55%，无信号)

**doctrine 铁律 5（新增，源自本次校准）**: 在采用任何 PMU 决策前，先用 `sample` / xctrace Time Profiler 验证 attribution。否则容易把 hardware-prefetcher-friendly 的代码当作 prefetch 受益者。SpMV prefetch 是教科书 prefetch 题目，但放错代码段就是 null result。

### P2 — 大工程 (前提：P1 收益打满)

- ✅ **BlockWiedemann 真 block BM** (Coppersmith matrix BM) — **关闭 2026-05-14**
  - 实施: column-extended quadratic basecase, 严格按 CADO-NFS `lingen_qcode_binary.cpp::lingen_qcode_do_tmpl` (clone 至 /tmp 离线参考)
  - 6 commits: foundation (DenseGF2_64x128 列主序 + mksol_accumulate Phase 3 原子) → matrix BM (single uint64 PoC → multi-word W = ⌈(L+10)/64⌉) → wire 3-phase pipeline (env GNFS_BW_ALGORITHM=scalar 强制 fallback) → cross-validate + benchmark
  - **关键调试**: Phase 3 mksol 必须用反向 F 系数 (`F_{D-k}` at Krylov step k), 类比 scalar BM 的 `c_{L-1-k}` extraction. 没反向 → 64/64 全部 M^T·w ≠ 0.
  - 实测加速 (`bench/microbench/bw_block_vs_scalar.cpp`, Release single-run):
    - 14K×2K: 3.45s → 0.15s = **22.5×**
    - 35K×5K: 14.98s → 0.69s = **21.8×**
    - 62K×10K: **53.52s → 1.12s = 48.0×** (符合 doctrine 30-60× 预期)
  - 路径选择: env `GNFS_BW_ALGORITHM=scalar` 强制旧路径用于调试/校验; 默认 block, 失败自动 fallback scalar
  - 限制: matrix BM 仍是 quadratic O(L²·b²·W), n > 500K 后需 Thomé subquadratic lingen (P2 后续)
  - 报告: `bench/results/2026-05-14-blockwiedemann-block-bm.md`
- ✅ **SME 探索性应用** (BL/BW 64×N SpMV) — **关闭 2026-05-15** (isolated micro-bench)
  - **Stage A baseline**: per-phase chrono timing 加入 `block_wiedemann_block_solve` (commit `989d9b4`). 278K×10K Release: Phase 1 Krylov 2620ms (63%), Phase 2 BM 308ms (7%), Phase 3 mksol 1186ms (28%). sample attribution: bw_spmv_B 占 >85% 总 wall — hot path 确认.
  - **Stage B NEON 128 isolated**: `bench/microbench/spmv_neon_gate.cpp` (commit `1c92212`). BlockVector128 = uint64_t[2] interleaved, veorq_u64 128-bit XOR + prefetch + cross-validate. **per-bit speedup**: forward 1.73×, transpose 1.30×, **bw_spmv_B 1.47×**.
  - **Stage C SME baseline (4×NEON unroll SVL=512)**: commit `1624c13`. 真 SME streaming mode 在 macOS 26.5 user-space SIGILL (xnu lazy-trap + SME entitlement gate 未开). 用 4× NEON 128-bit 等效 SVE2 streaming load 8 uint64 (SVL=512). **per-bit speedup**: forward 4.33×, transpose 2.83×, **bw_spmv_B 3.37×**.
  - **决策**: 不集成入 BW pipeline. 完整 BV128 BW pipeline 工程量 ~1150 行 / 15 commits 预期 1.30× 总 BW 加速; 真 SME (若 macOS gate 打开) 预期 2.86× 总加速. doctrine 措辞"专项实验 + 小规模验证"对应 isolated micro-bench, 完整集成移入 BACKLOG.
  - 报告: [`bench/results/2026-05-15-bw-neon-sme.md`](../../bench/results/2026-05-15-bw-neon-sme.md)
  - **教训**: doctrine 铁律 5 (measurement-first) 反向应用 — 完整 BV128 改造前, isolated SpMV gate 给出准确 per-bit speedup 决策路径短了 ~15 commits. macOS 与 Linux SME 兼容裂痕: 跨平台代码必须有 fallback (4×NEON unroll = portable SME 等效).

### P3 — 长期/低优先

- ~~E-core 后台分流（QoS Class 注入 ThreadPool）~~ ✅ **已关闭 2026-05-15**
  - **Stage B ThreadPool QoS 注入** (commit `849e453`): `QoSClass enum` + `set_current_thread_qos()` helper + `ThreadPool` ctor `qos` 参数 (默认 `UserInitiated`). `worker_loop` 入口 set QoS. macOS only, Linux no-op.
  - **Stage C main + bench QoS** (commit `0f12535`): `src/cli/main.cpp` 入口 + 3 个 bench microbench 入口 set `UserInitiated`. doctrine §7.2 第 3 条 落地证据.
  - **Stage D Microbench** (commit `34ea549`): `ecore_qos_gate.cpp` 4-trial 对照. 278K × 10K matrix, scalar 64-bit SpMV. **Key data**: `10-BG / 10-USER = 5.18×` (P-core hint 极限收益, worker 漂移 E-core 时基准失真 5×); `10-DEF ≈ 10-USER` (macOS 已隐式给前台 CLI USER QoS).
  - **决策**: 实测 wall delta ≈ 0 (macOS default 已 OK), 但显式 set 主要价值是 (1) 跨平台显式化, (2) 防 nohup/daemon 退化 (后台 mode QoS 降到 UTILITY 会触发 5× 退化), (3) `QoSClass::Background` opt-in 给真后台任务用.
  - 报告: [`bench/results/2026-05-15-ecore-qos.md`](../../bench/results/2026-05-15-ecore-qos.md)
  - **教训**: doctrine 铁律 5 (measurement-first) 又一次救场 — 看到 `grep qos 0` 就判断"必改"过激, 实测 10-DEF ≈ 10-USER 表明改动是预防性. 极限对照 (10-BG) 比正常情况对照 (10-DEF) 更有信号 — 5× 极限 boundary 才暴露真实风险.
- ~~内存使用减半（peak RAM 优化）~~ ✅ **已评估 2026-05-16 — deferred-by-data** (P3 第 2 条)
  - **Baseline 测量** (`/usr/bin/time -l ./test_stress 1 1`, 50-digit 164-bit, M5 16 GB):
    - max RSS: **2.03 GiB** (12.7% of 16 GiB)
    - peak memory footprint: 2.63 GiB (16.4%)
    - 0 swaps, 93 page faults (well within RAM)
    - real 7142s, user 30463s (4.27× parallelism)
  - **Polling vs `time -l` 教训**: 手动 ps RSS polling (60s 间隔) 观察 peak 634 MB,
    远低于 `time -l` 报告的 2.03 GiB. peak 在 phase transition (Phase 4 trim 或 Phase 5
    matrix construct) sub-poll window 闪现. `time -l` 是权威, polling 仅看长稳态.
  - **决策**: 50-digit 不是 RAM 瓶颈. 12.7% memory utilization 实施 "减半" 对 wall time
    0 影响 (无 swap). P3-2 实施触发条件 = 60+digit baseline RSS > 8 GiB (50% memory).
    当前 60-digit (`test_stress 1 2`) hours+ 未实测, 触发条件未达 → deferred.
  - 候选 hot spots (deferred follow-up, 触发后启动):
    1. CSR in-place transpose (BL/BW path) — 节省 ~30% linalg RAM
    2. RelationCollector OOC 集成 (已有 OOCRelationStore 基础设施) — 节省 ~40% phase 4
    3. BW Krylov sequence mmap (已有 MmapCSRMatrix 基础设施) — 节省 ~10% phase 5
  - 报告: [`bench/results/2026-05-16-50digit-ram-baseline.md`](../../bench/results/2026-05-16-50digit-ram-baseline.md)
  - **教训**: doctrine "减半" 的意义随 size 改变. 50-digit 减半 wall 0 影响; 60+digit
    才是 trigger size. measurement-first 又一次救场 — P3 优先级标"低"但听起来应实施,
    实测显示直接 defer 才对.
- ~~跨平台 Linux 同等优化（CI runners）~~ ✅ **已关闭 2026-05-15** (P3 第 3 条)
  - **Audit**: 最近 15 次 main CI run 全 PASS (ubuntu-latest + macos-latest matrix). 3 workflow: CI (build + ctest, ~4min), Sanitizers ASAN+UBSAN+TSAN (~5min ubuntu), CodeQL (~7min ubuntu). 已有 `--label-exclude "slow|heavy|stress"` 排除长测试.
  - **发现 Parity Gap**: CMakeLists.txt 46 个 `add_test` 仅 9 个有 `LABELS`, 其他 37 个无 label → `ctest -L instant`/`ctest -L fast` tier-based selection 工具链不可用, 与 `scripts/test.sh` TEST_TIER 系统脱钩. CLAUDE.md doctrine 明确要求 "新增测试必须打 LABELS, tier 与 scripts/test.sh TEST_TIER 保持一致" — 这是 doctrine compliance gap 而非 CI runtime 问题 (无 label 默认会跑, 所以 CI 实际行为正确).
  - **修复** (commit `7c7e5b4`): 给 37 个未带 LABEL 的 `add_test` 补齐 `set_tests_properties(... LABELS "<tier>" TIMEOUT <s>)`, tier 取自 `scripts/test.sh` 的 `TEST_TIER` 表. 最终分布: 30 instant + 8 fast + 4 slow + 2 heavy + 1 gate + 1 stress = 46.
  - **Sanitizer Hotfix** (commit `7bd6e99`): 首次 push 后 Linux Sanitizer CI 失败 — FactorBase/BaseM (instant 10s) + SIQS (fast 60s) 在 ASAN+UBSAN 下被 set_tests_properties TIMEOUT 卡 (TIMEOUT 是 hard limit, 优先于 ctest --timeout). 加 sanitizer flag 自动检测: `CMAKE_CXX_FLAGS MATCHES "fsanitize=(address|thread|memory|undefined|leak)"` 时对所有测试 TIMEOUT × 10.
  - **验证**: `ctest -LE "slow|heavy|stress"` (CI 等价) = 39 tests, Release 13.44s PASS; `ctest -LE "slow|heavy|stress|gate"` (Sanitizer 等价) = 38 tests; `ctest -L instant` = 30 tests PASS. Push 后 Linux CI 3/3 workflow PASS (CI + Sanitizers + CodeQL).
  - **决策**: 不改 CI 实际跑的测试集合 (改动是 metadata), 但修复 tier-based selection 工具链. Linux/macOS CI parity 已经存在 (matrix `[ubuntu-latest, macos-latest]`), 仅是 tier metadata 缺失. 真正的 Linux-specific 优化 (sched_setattr / cgroups / NUMA tuning) 当前无触发条件 — 现有 GitHub runners (ubuntu-latest 2 vCPU, 无 P/E 异构) 不需要.
  - **教训**: (1) "Linux CI parity" 听起来像缺 Linux 测试覆盖, 实际 audit 发现是 metadata gap (tier 标签不全). 启动审计前 grep `set_tests_properties` 5 秒就能定位真实问题, 比预设假设方向更准. (2) ctest `set_tests_properties TIMEOUT` 是 hard limit (优先于 `ctest --timeout` 全局), sanitizer build 下必须考虑 5-10× 慢. 改 TIMEOUT 前先想 sanitizer 是否会被卡 — 这次本地 macOS Release/Debug 都不会出现, 必须 push CI 才暴露.

### P4 — 破壁: lp_col_estimate 修复 (2026-05-16, 待 V0+fix 50d/60d PASS 确认)

修复 test_stress + pipeline 长期潜在 bug: `lp_col_estimate = relations.size() / 20` (5% 经验值) 对 ≥50-digit 严重低估. 实测 50-digit usable/lp_cols 比例为 **64%** (24677/38464), 而非 5%. 后果: sieve loop 提前 break, matrix build 时实际 cols 超出 break 阈值 9000 行 → NO EXCESS.

**Commits** (本会话):
- `9e84a73` Revert V2 (commit 21dcbcd) — 误诊为 filter merge bug, 实际是 estimate bug
- `c4cbe3a` fix(test_stress): accurate LP col estimate via count_unique_lp_keys
- `7013dd8` fix(test_stress): raise sieve target cap 5× → 20× initial
- `3a29e14` refactor: extract count_unique_lp_keys 到 filter.hpp library
- `117133e` fix(api/pipeline): apply same fix to sieve loop
- `dd8b5eb` test(filter): 6 unit tests for count_unique_lp_keys
- `a37cfe5` feat(test_stress): log β (lp_cols/usable ratio) per round telemetry
- `82d342e` fix(api/pipeline): post-sieve check also use effective_cols

**V2 失败 lesson** (revert 教训):
- V2 (commit 21dcbcd, 已 revert) 假设 bug 在 filter merge "weight-3+ LP keys 全弃". 改为 weight≥2 都 merge.
- 25-digit (81-bit) V2 假阳 +27% Merged. 但 lp_bits=20, weight-3 keys 稀少, V2 marginal benefit.
- 50-digit (164-bit) V2 真负 Merged 6786→**2088** (-69%), sngl 436→**21539** (×49). lp_bits=23 weight-3 keys 大量 → V2 "1 merge per key" 仍累 chain LP residue → sngl 飙升.
- **doctrine 铁律 5 教训**: 81-bit reg-test 不能预测 164-bit 行为. 跨 bit-range size 验证必须. 没有 50d 实测 confirm 前不能 push merge logic 修改.

**真 root cause** (after V2 revert):
- 50d 65% LP cols ratio vs 5% 估计 (12.6× under-estimate)
- 是 estimate bug not merge bug
- fix: 实际 count unique LP keys (同 matrix_builder convention) + cap 20× 让 adaptive loop ramp

**PASS formula** (V0 + accurate estimate):
- `raw_needed > matrix_cols / (α × (1 - β))` where α=merge_rate (50d ~1.1%), β=lp_cols/usable (50d ~0.65)
- 50d: 22156 / (0.011 × 0.35) = **5.75M raw** needed
- 60d: ~24000 / (0.005 × 0.30) ≈ 16M raw (估)

**实测确认** (50d V0+fix Round 1):
- Merged=6786 与 V0 baseline 一致 ✓
- lp_estimate=5412 (vs 旧 339, ×16 准确度)
- effective_cols=27568 (vs 旧 23099)
- Round 2 sieve target 2.76M (vs 旧 2.31M cap 卡死)

**Round 2 完成数据** (2026-05-16, 2.54h elapsed):
- Full=0 1LP=31969 2LP=349977 **Merged=46706** (6.9× Round 1 growth)
- merge_rate **1.691%** (vs Round 1 1.098%, +54% improvement — birthday paradox kicks in at scale)
- effective_cols=53113 (β=0.66 stable)
- 仍 NO_EXCESS: usable 46706 < 53113 → Round 3 target 3.45M
- 更新 PASS formula: raw_needed = 22156 / (0.01691 × 0.34) = **3.85M**
- Round 4 (~3.94M) 预 PASS, total ETA ~5.7h sieve (Round 2 完成时已 2.54h, 还需 1.3h)
- **教训**: α 随 N 增长显著 improvement (birthday效应), 不应假 constant α
- Round 5 估 PASS at ~5.6M raw, total wall ~5-6h

**60d follow-up** (BACKLOG 已加):
- [OPT] sieve 仅用 4 P-cores (M5 4P+6E), E-core 闲. 60d 27h 估可优化.
- [OPT] lp_bits 25 vs 26 trade-off (smaller LP space → fewer LP cols, less raw needed).

**测试覆盖**:
- test_filter: +6 unit tests for count_unique_lp_keys (covers empty/odd-exp/duplicate/multi-root)
- test_25digit 5-run: Merged=7536 deterministic (zero variance) confirms V0 stable
- test_gnfs_progressive 1-5: 8/8 PASS
- test_gnfs_e2e: 5/5 PASS
- test_factor_with_kleinjung: PASS

**V3 Clique Merge backup** (post-P4 fallback infrastructure):
若 V0+fix 50d/60d 在 PASS formula `raw > 5.6M` 仍 NO_EXCESS, 启用 V3 cascade.

- **算法** (`include/gnfs/relation/clique_merger.hpp`, 220+ LOC):
  - BFS spanning tree over LP-sharing bipartite graph
  - LP cancel check: `merged.lp_count() < before.lp_count()` 才 accept
  - 避免 V1/V2 chain-residue trap (V2 在 50d -69% Merged 教训)
  - **Overlap fast-path** (commit `d2ef403`, 4.6× speedup): 99.99% rejections
    are no-LP-overlap cases — pre-check via cached `acc_lp_set` skips
    heavy merge_two() entirely. Test_api 19.93s → 4.31s.

- **集成** (commits `babc322` + `975ac8b` + `7f9de82` + `56e5b14`):
  - ENV-gated `GNFS_CASCADE_V3` 三态 (默认 OFF, V0 path 零开销):
    - unset / "0" → OFF (V0 only)
    - "1" / "on" → ON (V3 every round)
    - "auto" → AUTO (V3 only Round 2+ in sieve loop; Phase 4 always enable)
  - 3 入口: pipeline.cpp:627 (sieve_and_collect, Auto-aware), pipeline.cpp:747 (Phase 4),
    test_stress.cpp:393 (stress sieve loop)
  - Dedup via (a, b) XOR hash 避免 V0 ∩ V3 重复

- **测试** (commits `babc322` + `5cf9e41` + `4d143d2`):
  - 6 基本 unit tests (test_clique_merger): empty, 1LP×N clique, 2LP triangle, no_overlap, 3LP+ filter
  - 3 synthetic 50d-like tests (test_clique_merger_50d_synthetic):
    - V0 alone: 885 merged
    - V0 + V3 cascade + dedup: 1115 merged
    - **V3 实测 +26% added beyond V0** (synthetic empirical evidence)
  - **V0 vs V0+V3 head-to-head bench** (commit `8a15eaa`):
    - 30K rel input (50d scale)
    - V0 alone: 13380 merged in 9ms
    - V0+V3 cascade: 13380 + 3240 = 16620 merged in 27ms
    - **V3 adds 24.2% in only 18ms overhead** — clear positive ROI
  - 2 Pipeline e2e tests (test_api.cpp):
    - test_v3_cascade_pipeline_integration: forces 12d Pipeline GNFS path
      with GNFS_CASCADE_V3=1, captures log callback
      - **40-bit real run: V3 added 2742 rels** (in=21096 full=3802 residual=1507)
      - lp_rejects=28M (LP cancel check actively prevents chain residue)
    - test_v3_cascade_disabled_by_default: ENV unset → V0-only unchanged

- **可见性**: stderr fprintf alongside emit_log → `[v3_cascade.sieve] in=N full=N residual=N added=N`

- **文档**: `docs/perf/v3-cascade-design.md` (设计 + ENV 用法 guide)

- **触发条件**:
  - V0+fix 50d Round 3+ 仍 NO_EXCESS
  - V0+fix 60d > 24h 未 PASS
  - 不在 V0 已 PASS 的 size 启用 (额外开销 0 收益)

- **fallback launch**: `bash /tmp/start_50d_v3cascade.sh`
  (复刻: `GNFS_CASCADE_V3=1 ./build-v3/test_stress 1 1`)
  (或 auto mode: `GNFS_CASCADE_V3=auto ./build-v3/test_stress 1 1` — Round 2+ 才 enable, 节省 Round 1 V3 overhead)

---

## §7  纪律与禁忌

### 7.1  禁止

1. **盲改 compiler flag** — 必须知道每个 flag 的影响 + 用对照测试证明
2. **未 profile 加 `[[likely]]/[[unlikely]]`** — 错误的预测比无预测更糟
3. **抄网上 micro-benchmark** — 缺乏上下文的 benchmark 几乎都有问题（搜索 Hyrum's Law of Benchmarks）
4. **release+NDEBUG 跑性能基准而不验证 Debug+sanitizers** — CLAUDE.md 已有明确警告
5. **改完不跑测试就 push** — 性能 commit 也是 commit，必须通过 `./scripts/test.sh module <m>` 或更高
6. **在 E-core 跑基准** — 默认 macOS 调度可能把进程放到 E-core，必须显式 QoS

### 7.2  必做

1. **每个 perf commit 附实测数据** — 至少 wall time before/after，PMU 数据加分
2. **基准跑前预热** — 至少 2 轮丢弃（cache + branch predictor + thermal）
3. **基准用 P-core 强制** — `pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0)` 或 wrapper
4. **PGO 训练样本 ≠ 评估样本** — 防 overfit
5. **改完看 disassembly** — `objdump -d` 或 `Compiler Explorer` 重点确认热点
6. **保留 trace 文件** — `bench/results/` 长期保存，便于历史回归

### 7.3  红线 (违反则回滚)

- 任何提升 < 2% 但增加复杂度的改动 — 不值得
- 任何让 sanitizers 报新警告的改动 — 不安全
- 任何破坏跨平台 (Linux CI) 的改动 — 必须有 `#ifdef` 保护

---

## 附录 A  M5 PMU Event 速查表

> 来源: tzakharko / blog.bugsiki.dev / blog.clf3.org（reverse-engineered，非官方）
>
> **M5 本机实测**（2026-05-13）：事件名取自 `/usr/share/kpep/as5.plist`（macOS 25.4 / Darwin 25.4.0 自带，M5 P/E 核分别为 `as5-1.plist` / `as5-2.plist`）。完整 135 事件清单导出:
>   ```bash
>   plutil -convert xml1 -o - /usr/share/kpep/as5.plist | grep -oE '<key>[A-Z][A-Z_0-9]+</key>' | sort -u
>   ```
> **mperf -l** 列出当前可编程的子集（~80 事件 + 别名）。
>
> **counters_mask 约束**: 部分事件不能任意分配 slot。如 `INST_BRANCH` / `BRANCH_MISPRED_NONSPEC` / `INST_ALL` / `INST_BRANCH_TAKEN` mask = `0b11111100`（仅 slots 0/1 可用）。mperf 按用户顺序贪心分配，约束事件必须放最前面，否则 `Failed to add event (conflict: 0xfc)`。检查方法:
>   ```bash
>   plutil -convert xml1 -o - /usr/share/kpep/as5.plist | \
>     awk '/<key>EVENT_NAME<\/key>/,/<\/dict>/'  # 找 counters_mask 字段
>   ```

| Event 名 | 描述 | 类型 |
|---------|------|------|
| `CORE_ACTIVE_CYCLE` | 活跃核周期 | Fixed |
| `INST_ALL` | 总退役指令 | Fixed |
| `INST_BRANCH` | 分支指令数 | Configurable |
| `INST_BRANCH_TAKEN` | 取分支 | Configurable |
| `BRANCH_COND_MISPRED_NONSPEC` | 条件分支误判 | Configurable |
| `BRANCH_INDIR_MISPRED_NONSPEC` | 间接分支误判 | Configurable |
| `BRANCH_RET_MISPRED_NONSPEC` | RET 误判 | Configurable |
| `L1I_TLB_MISS` | iTLB miss | Configurable |
| `L1D_TLB_MISS` | dTLB miss | Configurable |
| `L1D_CACHE_MISS_LD` | L1D load miss | Configurable |
| `L1D_CACHE_MISS_ST` | L1D store miss | Configurable |
| `L1I_CACHE_MISS_DEMAND` | L1I miss | Configurable |
| `L2_CACHE_MISS_LD` | L2 load miss | Configurable |
| `L2_TLB_MISS_INST` | L2 iTLB miss | Configurable |
| `L2_TLB_MISS_DATA` | L2 dTLB miss | Configurable |
| `INST_INT_LD` | 整数 load | Configurable |
| `INST_INT_ST` | 整数 store | Configurable |
| `INST_INT_ALU` | 整数 ALU | Configurable |
| `INST_SIMD_LD` | SIMD load | Configurable |
| `INST_SIMD_ST` | SIMD store | Configurable |
| `INST_NEON` | NEON 指令 | Configurable |
| `INST_FP` | 浮点指令 | Configurable |
| `MAP_DISPATCH_BUBBLE` | 前端 dispatch bubble | Configurable |
| `ATOMIC_OR_EXCLUSIVE_SUCC` | 原子操作成功 | Configurable |
| `ATOMIC_OR_EXCLUSIVE_FAIL` | 原子操作失败（争用） | Configurable |

**关键计算**:
- IPC = `INST_ALL / CORE_ACTIVE_CYCLE`
- L1D miss rate = `L1D_CACHE_MISS_LD / INST_INT_LD`
- Branch mispred rate (cond) = `BRANCH_COND_MISPRED_NONSPEC / INST_BRANCH`
- Front-end bound (proxy) = `MAP_DISPATCH_BUBBLE / CORE_ACTIVE_CYCLE`

## 附录 B  NEON Intrinsics 速查表（GNFS 相关子集）

| 用途 | Intrinsic | Lane 形态 |
|------|-----------|-----------|
| **Load/Store** |
| 16B load | `vld1q_u64`, `vld1q_u32`, `vld1q_u16`, `vld1q_u8` | 2×u64, 4×u32, 8×u16, 16×u8 |
| 16B store | `vst1q_*` (同上) | 同 |
| Aligned load | `vld1q_lane_u64` (interleaved) | per-lane |
| **整数算术** |
| Add | `vaddq_u64`, `vaddq_u32`, `vaddq_u16`, `vaddq_u8` | 同 |
| Sub | `vsubq_*` | 同 |
| Mul (low) | `vmulq_u32` | 4×u32 |
| Saturating add | `vqaddq_u8`, `vqaddq_u16` | 饱和 |
| **位运算** |
| AND | `vandq_u64` | 128-bit |
| OR | `vorrq_u64` | 同 |
| XOR | `veorq_u64` | 同 (GF(2) 主力) |
| NOT | `vmvnq_u32` | 同 |
| AND-NOT | `vbicq_u64` (A AND NOT B) | 同 |
| **比较** |
| Equal | `vceqq_u64`, `vceqq_u32`, `vceqq_u16`, `vceqq_u8` | mask |
| Greater/equal | `vcgeq_u32` etc. | mask |
| **移位** |
| Left shift | `vshlq_n_u64` (constant), `vshlq_u64` (variable) | 同 |
| Right shift | `vshrq_n_u64`, `vshrq_u64` | 同 |
| **水平归约** |
| Sum all lanes | `vaddvq_u64` (2 lanes), `vaddvq_u32` (4 lanes) | scalar 输出 |
| Max all lanes | `vmaxvq_u32`, `vmaxvq_u16` | scalar |
| Pairwise add | `vpaddq_u64`, `vpaddq_u32` | 同宽度 |
| Pairwise widen | `vpaddlq_u8` (8→16), `vpaddlq_u16` (16→32) | 升宽 |
| **位计数** |
| popcount per byte | `vcntq_u8` | 16×u8 |
| 转 u64 总和 | `vaddvq_u64(vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(x))))` | scalar |
| **类型转换** |
| reinterpret | `vreinterpretq_u64_u8` 等 | bit-identical |
| 拓宽 | `vmovl_u8` (8→16), `vmovl_u16` (16→32) | half→full |
| 收窄 | `vmovn_u32` (32→16), `vmovn_u16` (16→8) | full→half |

## 附录 C  参考资料

### 微架构与硬件
- [Apple M5 Wikipedia](https://en.wikipedia.org/wiki/Apple_M5)
- [Apple unleashes M5 (newsroom, 2025-10)](https://www.apple.com/newsroom/2025/10/apple-unleashes-m5-the-next-big-leap-in-ai-performance-for-apple-silicon/)
- [M5 Apple Silicon: It's All About the Cache And Tensors (Creative Strategies)](https://creativestrategies.com/research/m5-apple-silicon-its-all-about-the-cache-and-tensors/)
- [Benchmarking M-series Apple CPUs (ETH Zurich, 2025)](https://acl.inf.ethz.ch/teaching/fastcode/2025/benchmarking_m_series_apple_cpus.pdf)

### Apple PMU & 性能分析
- [PMU Counters on Apple Silicon (Bugsik)](https://blog.bugsiki.dev/posts/apple-pmu/)
- [Utilizing PMU Event Counters on Apple M3 and M4 (clf3)](https://blog.clf3.org/post/pmu-event-counters/)
- [Quick Hardware Performance Counters on macOS ARM64 (Perpetually Curious)](https://lambdafoo.com/posts/2026-03-25-mperf-hardware-counters-macos.html)
- [Counting cycles and instructions on the Apple M1 (Daniel Lemire)](https://lemire.me/blog/2021/03/24/counting-cycles-and-instructions-on-the-apple-m1-processor/)
- [macos-perf (siedentop, GitHub)](https://github.com/siedentop/macos-perf)

### SME / 矩阵协处理器
- [M4 SME Exploration (tzakharko)](https://github.com/tzakharko/m4-sme-exploration)
- [SME Overview (tzakharko)](https://github.com/tzakharko/m4-sme-exploration/blob/main/reports/01-sme-overview.md)
- [Hello SME! (arXiv:2409.18779)](https://ar5iv.labs.arxiv.org/html/2409.18779)
- [Hello SME Documentation (Jena)](https://scalable.uni-jena.de/opt/sme/index.html)

### Instruments / xctrace
- [Using Xcode Instruments for C++ CPU profiling (jviotti)](https://www.jviotti.com/2024/01/29/using-xcode-instruments-for-cpp-cpu-profiling.html)
- [Optimize CPU performance with Instruments — WWDC25 #308](https://developer.apple.com/videos/play/wwdc2025/308/)
- [asitop (tlkh)](https://github.com/tlkh/asitop)

### NFS 算法
- [CADO-NFS Official](https://cado-nfs.gitlabpages.inria.fr/)
- [CADO-NFS Repository](https://github.com/cado-nfs/cado-nfs)
- [Lattice Sieve Field Selection of Cado-NFS (Bai/Filbois/Thomé)](https://www.researchgate.net/publication/339683240_The_Lattice_Sieve_Field_Selection_of_Cado-NFS)
- [CADO-NFS Talk (Zimmermann)](https://members.loria.fr/PZimmermann/talks/cado.pdf)

### C++ 优化
- [Agner Fog - Software Optimization Resources](https://www.agner.org/optimize/)
- [Optimizing software in C++ (Agner Fog)](https://www.agner.org/optimize/optimizing_cpp.pdf)
- [Performance Analysis and Tuning on Modern CPUs (NIU)](https://faculty.cs.niu.edu/~winans/notes/patmc.pdf)
- [Intel TMA Cookbook 2025-4](https://www.intel.com/content/www/us/en/docs/vtune-profiler/cookbook/2025-4/top-down-microarchitecture-analysis-method.html)

### 数论库
- [The GNU MP Bignum Library](https://gmplib.org/)
- [GMP 6.3 News](https://gmplib.org/gmp6.3)

---

**END of Performance Doctrine v1.0**

*下一步: 实施 §5 的 S1-S6，建立 PGO + Instruments 闭环；改动产生的所有 commit 必须援引本文档某一条铁律。*
