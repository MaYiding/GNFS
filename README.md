> [English](README-EN.md) | **中文**

# GNFS — 一般数域筛法

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License: GPL-2.0](https://img.shields.io/badge/License-GPL%202.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey.svg)]()
[![Tests](https://img.shields.io/badge/tests-41%20files-brightgreen.svg)]()
[![LoC](https://img.shields.io/badge/lines%20of%20code-~41K-informational.svg)]()

C++20 实现的工业级**一般数域筛法** (General Number Field Sieve)。

GNFS 是已知最强的经典大整数分解算法，RSA 破纪录分解背后的核心方法。本项目实现了**完整的 GNFS 流水线**——从多项式选择到平方根提取，包含约 41,000 行代码，分布在 53 个头文件、14 个源文件和 41 个测试文件中。

## 目录

- [亮点](#亮点)
- [分解结果](#分解结果)
- [快速开始](#快速开始)
- [CLI 用法](#cli-用法)
- [C++ API](#c-api)
- [架构](#架构)
- [流水线 — 8 个阶段](#流水线--8-个阶段)
- [核心算法](#核心算法)
- [性能优化](#性能优化)
- [测试](#测试)
- [构建选项](#构建选项)
- [依赖](#依赖)
- [设计约定](#设计约定)
- [项目结构](#项目结构)
- [贡献](#贡献)
- [参考文献](#参考文献)
- [许可证](#许可证)

## 亮点

- **完整 8 阶段流水线**: 多项式选择 → 因子基 → 筛法 → 余因子 → 关系收集 → 线性代数 → 平方根 → GCD，全部实现并集成
- **验证正确性**: 成功分解 8-bit 到 200-bit (60 位十进制) 的整数，配备 5 级渐进测试套件和回归门禁
- **高性能**: 多线程格筛 + bucket sieve 处理大因子基，并行 Block Lanczos / Block Wiedemann，Nguyen 混合平方根 (比朴素 CRT 快 200 倍)
- **生产质量**: 41 个测试文件涵盖单元/集成/回归/E2E/渐进/压力测试；全程溢出安全算术；线程安全关系收集
- **双求解器**: Block Lanczos 和 Block Wiedemann 两种 GF(2) 零空间求解器，SGE 预处理 + Gaussian 小矩阵回退
- **Out-of-Core 支持**: mmap 支撑的 CSR 矩阵和关系存储，适用于内存受限的大规模分解
- **多策略余因子分解**: 试除 + SQUFOF (2-word 余因子) + ECM (Montgomery 曲线, Stage 1+2 BSGS) + 光滑性验证
- **自适应参数**: CADO-NFS 校准参数表 + L_N 启发式 + 经验微调；Kleinjung 自适应缩放适配大输入
- **统一 CLI**: `./gnfs <数字>` 一键分解，双语 UI (中/英)，进度显示，JSON/CSV/报告输出
- **简洁 C++ API**: 高层 `factorize()` 一行调用 + 底层 `Pipeline` 类逐步控制

## 分解结果

| 级别 | N (示例) | 位数 | 十进制位 | 耗时 | 备注 |
|------|----------|------|----------|------|------|
| L1 | 143, 9991, 10403 | 8–14 | 3–5 | ~1.5s | 极小 |
| L2 | 96091, 100160063 | 17–27 | 5–9 | ~1.5s | 瞬时 |
| L3 | 1000036000099 | 40 | 13 | ~4s | 快速 |
| L4 | 100000980001501 | 47 | 15 | ~11s | 秒级 |
| L5 | 1253371692427905599 | 61 | 19 | ~43s | 半分钟 |
| 25 位 | 1669994516749619561652133 | 81 | 25 | **48ms** | Pollard rho 快速路径 |
| 50 位 | (164-bit 半素数) | 164 | 50 | ~2.6h | 压力测试 |
| 60 位 | (200-bit 半素数) | 200 | 60 | 小时级 | 压力测试 |

**≤27 位数**: Pollard rho Brent 快速路径自动启用，通常 <100ms。**GNFS 仅在 >90 bit 时启动**。

## 快速开始

### 前置要求

- **C++20** 编译器 (Clang 14+ 或 GCC 12+)
- **CMake** 3.20+
- **GMP** (GNU 多精度算术库) — 必需

**可选**: NTL (数论库), Metal (macOS GPU 框架)

```bash
# macOS
brew install gmp cmake

# Ubuntu / Debian
sudo apt install libgmp-dev cmake build-essential

# Fedora / RHEL
sudo dnf install gmp-devel cmake gcc-c++

# Arch Linux
sudo pacman -S gmp cmake
```

### 构建

```bash
git clone <repo-url> && cd GNFS

# 配置 & 构建 (Release 以获得最佳性能)
cmake -B build -DCMAKE_BUILD_TYPE=Release
make -C build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

# 冒烟测试 (~5s, 23 个即时测试)
./scripts/test.sh
```

### 分解一个数

```bash
# 一键分解
./build/gnfs 96091

# JSON 输出
./build/gnfs 1000036000099 --json

# 详细报告保存到文件
./build/gnfs 1000036000099 --report -o result.txt

# 英文 UI
./build/gnfs 96091 --lang en

# 交互式 REPL 模式
./build/gnfs --interactive
```

## CLI 用法

### 终端输出示例

```
   ╔══════════════════════════════════════╗
   ║   ██████╗ ███╗   ██╗███████╗███████╗ ║
   ║  ██╔════╝ ████╗  ██║██╔════╝██╔════╝ ║
   ║  ██║  ███╗██╔██╗ ██║█████╗  ███████╗ ║
   ║  ██║   ██║██║╚██╗██║██╔══╝  ╚════██║ ║
   ║  ╚██████╔╝██║ ╚████║██║     ███████║ ║
   ║   ╚═════╝ ╚═╝  ╚═══╝╚═╝     ╚══════╝ ║
   ╚══════════════════════════════════════╝
   General Number Field Sieve v0.1.0

分解: 96091 (17 bits)

   ✓ 因子基                                   [12ms]
   ✓ 筛法                                    [1.35s]
   ✓ 过滤                                     [<1ms]
   ✓ 线性代数                                  [14ms]
   ✓ 平方根                                    [6ms]

   ╔══════════════════════════════════════════════════╗
   ║  分解成功                                        ║
   ╠══════════════════════════════════════════════════╣
   ║  N = 96091                                       ║
   ║      17 bits, 5 digits                           ║
   ║                                                  ║
   ║  = 307 × 313                                     ║
   ╠══════════════════════════════════════════════════╣
   ║  ├─ 多项式           <1ms    0.0%                ║
   ║  ├─ 因子基          12ms    0.9%                ║
   ║  ├─ 筛法            1.35s   97.0%                ║
   ║  ├─ 过滤            <1ms    0.0%                ║
   ║  ├─ 线性代数         14ms    1.0%                ║
   ║  └─ 平方根            6ms    0.4%                ║
   ║                      ____________                ║
   ║      总计                   1.39s                ║
   ╠══════════════════════════════════════════════════╣
   ║  关系: 512  矩阵: 512×313  依赖: 64               ║
   ╚══════════════════════════════════════════════════╝
```

### 全部 CLI 选项

```
用法: gnfs <数字> [选项]

参数:
  <数字>                           待分解的合数

输出选项:
  --json                           以 JSON 格式输出
  --csv                            以 CSV 格式输出
  --report                         输出带统计信息的详细报告
  -o, --output <文件>              输出到文件 (默认: stdout)
  --verbose                        启用结构化详细日志
  --quiet                          最简输出 (仅结果)
  --no-color                       禁用 ANSI 颜色

语言:
  --lang <zh|en>                   UI 语言 (默认: zh = 中文)

参数覆盖:
  --degree <d>                     多项式度数 (省略则自动选择)
  --fb-rational <n>                有理因子基界
  --fb-algebraic <n>               代数因子基界
  --large-prime <n>                大素数界
  --sieve-width <n>                筛区宽度
  --sieve-height <n>               筛区高度
  --threads <n>                    工作线程数

配置文件:
  -c, --config <文件>              从 key=value 配置文件加载参数

其他:
  --interactive                    交互式 REPL 模式
  -h, --help                       显示帮助
  --version                        显示版本
```

### 配置文件格式

```ini
# gnfs.cfg — 示例配置
degree = 4
rational_bound = 50000
algebraic_bound = 100000
large_prime_bound = 3000000
threads = 8
verbose = true
```

## C++ API

### 高层: 一行分解

```cpp
#include <gnfs/api/factorizer.hpp>

// 最简用法
auto result = gnfs::api::factorize(gnfs::core::Integer("96091"));
if (result.success) {
    std::cout << result.factors[0].to_string() << " * "
              << result.factors[1].to_string() << "\n";
}

// 字符串便捷接口
auto result = gnfs::api::factorize("1000036000099");

// 完整 JSON 输出
std::cout << result.to_json();

// 自定义配置
gnfs::api::Config cfg;
cfg.degree = 4;
cfg.verbose = false;
cfg.threads = 8;
auto result = gnfs::api::factorize(n, cfg);

// 带进度回调
auto result = gnfs::api::factorize(n, cfg, [](const auto& info) {
    std::cout << gnfs::api::phase_name(info.phase)
              << ": " << info.phase_progress * 100 << "%\n";
});
```

### 中层: 逐步流水线

```cpp
#include <gnfs/api/pipeline.hpp>

gnfs::api::Pipeline pipeline(n, config);
pipeline.set_progress_callback(my_progress_cb);
pipeline.set_log_callback(my_log_cb);

// 逐阶段执行
auto ctx     = pipeline.select_polynomial();
auto fb      = pipeline.build_factor_base(ctx);
auto rels    = pipeline.sieve_and_collect(ctx, fb);
auto filtered = pipeline.filter(std::move(rels));
auto mr      = pipeline.solve_matrix(std::move(filtered), fb, ctx);
auto result  = pipeline.extract_factors(mr, fb, ctx);

// 或一次执行整个流水线
auto result = pipeline.run();

// 访问详细统计
const auto& stats = pipeline.stats();
std::cout << "矩阵: " << stats.matrix_rows << "x" << stats.matrix_cols << "\n";
std::cout << "总耗时: " << stats.timings.total_s << "s\n";
```

### 结果结构

```cpp
struct FactorResult {
    bool success;
    Integer n;                    // 原始输入
    std::vector<Integer> factors; // 找到的因子 (升序)
    FactorStats stats;            // 详细统计

    std::string to_text();   // "N = p * q\nTime: 1.0s (25 digits, 81 bits)"
    std::string to_json();   // 完整 JSON (含全部统计)
    std::string to_csv();    // "N,p,q,bits,digits,time_s,success"
};

struct FactorStats {
    // 输入: n_bits, n_digits, degree
    // 因子基: rational/algebraic primes, bounds
    // 筛法: special_q_processed, candidates, relations
    // 过滤: singletons_removed, merged_relations
    // 线性代数: matrix_rows/cols/weight, dependencies_found
    // 平方根: dependencies_tried
    // 计时: 各阶段耗时 + 总计
};
```

## 架构

```
include/gnfs/           # 53 个头文件 (.hpp)
├── api/           (6)  # Config, Pipeline, Factorizer, Result, Progress, i18n
├── core/          (6)  # Integer, Polynomial, Relation, Params, PolynomialContext, Types
├── polynomial/    (6)  # Kleinjung, Murphy E, Base-m, IntPolynomial, Optimizer, SelectorDispatch
├── factor_base/   (2)  # FactorBase 数据结构, Builder (Cantor-Zassenhaus)
├── sieve/         (4)  # LatticeSieve (bucket+格), LineSieve, Special-Q, LatticeBasis
├── cofactor/      (5)  # Cofactorizer, ECM (Montgomery), SQUFOF, TrialDivision, SmoothCheck
├── relation/      (3)  # Collector (线程安全), Filter (1LP+2LP 合并), OOCRelationStore
├── linalg/        (8)  # MatrixBuilder, BlockLanczos, BlockWiedemann, Gaussian, SGE,
│                       # Schirokauer, SparseMatrix, MmapCSRMatrix
├── sqrt/          (7)  # AlgebraicSqrt, HenselSqrt, Couveignes, RationalSqrt,
│                       # ClassGroup, ModularPoly, NumberField
└── util/          (6)  # SmallVector, ThreadPool, Logger, Timer, MmapFile, SafeMath

src/                    # 14 个源文件 (.cpp)
├── api/           (2)  # pipeline.cpp, factorizer.cpp
├── cli/           (1)  # main.cpp — CLI 入口
├── core/          (3)  # integer, polynomial, relation
├── factor_base/   (1)  # builder (Cantor-Zassenhaus 求根)
├── linalg/        (3)  # block_lanczos, block_wiedemann, matrix_builder
├── polynomial/    (1)  # base_m
├── sieve/         (1)  # lattice_sieve
└── sqrt/          (2)  # algebraic_sqrt, rational_sqrt

tests/                  # 41 个测试文件 (.cpp)
```

### 模块概览

| 模块 | 头文件 | 核心组件 | 描述 |
|------|:------:|----------|------|
| **api** | 6 | `Factorizer`, `Pipeline`, `Config`, `Result`, `Progress`, `i18n` | 公开 API 层，双语 UI 支持 |
| **core** | 6 | `Integer`, `Polynomial`, `Relation`, `Params`, `PolynomialContext`, `Types` | 基础类型: GMP 整数封装、多项式、关系、自动参数 |
| **polynomial** | 6 | `KleinjungSelector`, `MurphyEvaluator`, `BaseMSelector`, `SelectorDispatch` | 多项式选择: Kleinjung 格搜索 + base-m, Murphy E 评分, 自适应分发 |
| **factor_base** | 2 | `FactorBase`, `FactorBaseBuilder` | 因子基构建，并行 Cantor-Zassenhaus 求根 |
| **sieve** | 4 | `LatticeSieve`, `LineSieve`, `SpecialQ`, `LatticeBasis` | 格筛 + Special-Q 分解，bucket sieve 处理大因子基 |
| **cofactor** | 5 | `Cofactorizer`, `ECM`, `SQUFOF`, `TrialDivider`, `SmoothCheck` | 多策略余因子: 试除 → SQUFOF → ECM → 光滑性验证 |
| **relation** | 3 | `RelationCollector`, `RelationFilter`, `OOCRelationStore` | 线程安全收集, 1LP+2LP 图合并, mmap out-of-core 存储 |
| **linalg** | 8 | `BlockLanczos`, `BlockWiedemann`, `Gaussian`, `SGE`, `MatrixBuilder`, `SparseMatrix`, `MmapCSR`, `Schirokauer` | GF(2) 零空间求解 + SGE 预处理 + 双求解器 + Schirokauer 映射 |
| **sqrt** | 7 | `AlgebraicSqrt`, `HenselSqrt`, `Couveignes`, `RationalSqrt`, `ClassGroup`, `ModularPoly`, `NumberField` | 平方根: Nguyen Hybrid (Hensel+CRT) 为主, Couveignes 备选; 类群特征 |
| **util** | 6 | `SmallVector`, `ThreadPool`, `Logger`, `Timer`, `MmapFile`, `SafeMath` | 工具集: 栈分配小向量、work-stealing 线程池、mmap 文件 I/O |

## 流水线 — 8 个阶段

一般数域筛法将大整数分解拆解为 8 个相互依赖的阶段:

```
  ① 多项式选择    Kleinjung / base-m → f(x), g(x)
       ↓
  ② 因子基构建    并行 Cantor-Zassenhaus 求根
       ↓
  ③ 格筛法        Special-Q + bucket sieve, 多线程
       ↓
  ④ 余因子分解    试除 → SQUFOF → ECM (Stage 1+2)
       ↓
  ⑤ 关系收集      线程安全收集 + 1LP/2LP 合并
       ↓
  ⑥ 线性代数      SGE 预处理 + Block Lanczos / Wiedemann
       ↓
  ⑦ 平方根提取    Nguyen Hybrid (Hensel+CRT) + Couveignes 备选
       ↓
  ⑧ GCD           gcd(a ± b, N) → 非平凡因子
```

### 各阶段详解

**① 多项式选择** — 算法需要两个多项式 f(x) 和 g(x)，满足模 N 有公共根 m。`SelectorDispatch` 根据输入规模自动选择 base-m (小输入) 或 Kleinjung 格搜索 (大输入)，用 Murphy E 函数评估筛法产出质量。

**② 因子基构建** — 对有理侧和代数侧，找出界以下所有素数及其模 f(x) 的根。求根采用 **Cantor-Zassenhaus** 算法 (O(d² log p))，通过线程池并行化——大因子基较朴素扫描快 460 倍。

**③ 格筛法** — 最耗时的阶段。对每个 Special-Q 素数 q，构造满足代数范数被 q 整除的 (a,b) 二维格。筛法识别范数可能光滑的候选。大因子基素数使用 **bucket sieve** 减少 cache miss；多线程 scatter 将工作分布到各筛区。

**④ 余因子分解** — 筛法后，候选的残余余因子需完全分解。策略分层: 试除处理小因子，SQUFOF (比 Pollard rho 快 10-100 倍) 处理 2-word 余因子，ECM (Montgomery 曲线, Stage 1 + Stage 2 BSGS, D=2310) 处理其余。

**⑤ 关系收集与过滤** — 关系是两侧范数均光滑的 (a,b) 对。收集器使用原子计数器和线程局部缓冲保证线程安全。收集后进行**大素数合并**: 1-LP 贪心配对 (weight ≥ 2)，2-LP 图处理 (weight = 2，LP 键为节点，关系为边)。

**⑥ 线性代数** — GNFS 的核心: 在 GF(2) 上找关系间的依赖。首先 **SGE** (结构化 Gaussian 消元) 通过消除 weight-1 和 weight-2 列预处理矩阵，通常缩减 30-60%。然后: < 5K 行的矩阵直接用 **Gaussian**；更大矩阵用 **Block Lanczos** (64-bit block 并行 SpMV + ThreadPool) 或 **Block Wiedemann** (Krylov + Gaussian 三阶段, Coppersmith 1994)。

**⑦ 平方根提取** — 对每个依赖，计算有理和代数平方根。有理侧直接乘积。代数侧使用 **Nguyen Hybrid**: K 个小素数 (~10-bit) + 模多项式 Tonelli-Shanks + Hensel 提升至 ~10-20K bit + CRT + 2^(K-1) 符号搜索——比大素数 CRT 快 200 倍。**Couveignes** 算法作为备选。类群特征验证任意度数多项式的正确性。

**⑧ GCD** — 最终，gcd(有理平方根 ± 代数平方根像, N) 产出非平凡因子。如需，可尝试多个依赖。

## 核心算法

| 组件 | 算法 | 复杂度 / 备注 |
|------|------|---------------|
| **多项式选择** | Kleinjung 格搜索 + base-m | Murphy E 评分; `SelectorDispatch` 按 N 规模自动选择 |
| **求根** | Cantor-Zassenhaus | O(d² log p), 大因子基比朴素 O(p) 扫描快 460 倍 |
| **筛法** | 格筛 + Special-Q + Bucket sieve | 多线程; bucket scatter 处理大因子基素数 |
| **余因子** | 试除 → SQUFOF → ECM | SQUFOF: 2-word 余因子比 Pollard rho 快 10-100 倍 |
| **ECM** | Montgomery 曲线, Stage 1+2 BSGS | D=2310, 480 baby steps, 差分 giant steps |
| **关系合并** | 1-LP 贪心 + 2-LP 图 | 1LP weight ≥ 2, 2LP weight = 2 (GNFS 标准) |
| **SGE** | 结构化 Gaussian 消元 | Weight-1/2 列消除, 矩阵缩减 30-60% |
| **Block Lanczos** | Montgomery (1995) | 64-bit block 并行 SpMV + ThreadPool; > 5K 行 |
| **Block Wiedemann** | Coppersmith (1994) | Krylov + Gaussian 三阶段; 4 GB 内存保护 |
| **Gaussian** | Packed GF(2) 消元 | 64-bit word-packed; < 5K 行回退 |
| **Schirokauer 映射** | GF(2), ℓ=2 | 分裂检测, Hensel 计算; 指数 = ℓ^d - 1 |
| **平方根** | Nguyen Hybrid (主路径) | K 小素数 + Hensel 提升 + CRT; 比大素数 CRT 快 200 倍 |
| **平方根** | Couveignes (备选) | Gray Code CRT, 65536 模式搜索, 逐素数验证 |
| **类群** | Sturm 定理 | 任意度数实根计数用于特征验证 |

## 性能优化

### 筛法 (阶段 3) — 主导阶段

- **Bucket sieve**: 大因子基素数按桶分散，减少 cache miss; 多线程逐区处理
- **紧凑筛素数**: `CompactSmallPrime` (12 字节) vs `PrimeEntry` (36 字节)，提升缓存利用率
- **SQ 预除**: 代数范数预除 Special-Q 素数，跳过 ~10K 个 SQ 区间 FB 条目
- **自适应筛阈值**: ≤25 位输入用 0.6× 因子——候选更少但质量更高

### 余因子分解 (阶段 4)

- **SQUFOF**: 2-word (~128-bit) 余因子比 Pollard rho 快 10-100 倍
- **p² 安全早退**: 余因子 < p² 且 > FB 界时跳过剩余试除
- **自适应 Miller-Rabin**: Jaeschke (1993) 确定性界——按数的大小选 1-7 个 witness
- **ECM Stage 2 BSGS**: D=2310, 480 baby steps + 差分 giant steps

### 线性代数 (阶段 6)

- **SGE 预处理**: Weight-1 和 weight-2 列消除，矩阵缩减 30-60%
- **PackedGF2Matrix**: 64-bit word-packed，O(1) 位访问
- **Block Lanczos**: 64-bit block，持久 ThreadPool 并行 SpMV (取代逐 pivot 线程创建——360ms → ~0ms 开销)
- **Block Wiedemann**: Krylov + Gaussian 三阶段 (Coppersmith 1994); 标量 Berlekamp-Massey 在非幂零 B=M·M^T 上会失败
- **Gaussian 阈值**: < 5K 行直接用 Gaussian (比 BL/BW 开销更低)

### 平方根 (阶段 7)

- **Nguyen Hybrid**: K=3 小素数 (~10-bit) + Hensel 提升至 ~10-20K bit + CRT + 2^(K-1) 符号搜索。L5 平方根: 比大素数 CRT 快 200 倍
- **Couveignes**: 预计算期望乘积, Gray Code CRT, 65536 模式搜索, 逐素数验证

### 因子基构建 (阶段 2)

- **并行 Cantor-Zassenhaus**: 多线程求根 + `mpz_divisible_ui_p` 快速整除测试。25 位因子基: 0.31s → 0.02s

### Out-of-Core 基础设施

- **MmapCSRMatrix**: 内存映射 CSR (压缩稀疏行) 矩阵，用于超出可用 RAM 的矩阵
- **OOCRelationStore**: mmap 支撑的关系存储，用于大规模分解
- **MmapFile**: 全代码库通用的内存映射文件 I/O

## 测试

项目使用 `scripts/test.sh`——统一测试运行器，自带自动编译、逐测试超时 (zsh 原生, 不依赖 GNU coreutils)、心跳监控和分级测试。

### 快速参考

```bash
# 日常开发 (最常用)
./scripts/test.sh                      # 冒烟: 23 个即时测试, ~5s
./scripts/test.sh changed              # 根据 git diff 自动检测受影响模块
./scripts/test.sh changed --deep       # 同上 + 级联依赖模块

# 模块级
./scripts/test.sh module linalg        # 仅线性代数
./scripts/test.sh module sieve sqrt    # 多模块
./scripts/test.sh module all           # 全模块 (仅 instant+fast)
./scripts/test.sh module all --slow    # 全模块 (含 slow+heavy)

# 单个测试
./scripts/test.sh run test_linalg      # 指定测试二进制
./scripts/test.sh run sqrt             # 自动补 test_ 前缀

# 合并门禁
./scripts/test.sh gate                 # 门禁: 冒烟 + 回归 (17/27/40/81-bit) ~20s
./scripts/test.sh gate --quick         # 快速门禁: 仅冒烟 ~5s

# E2E & 渐进
./scripts/test.sh e2e                  # 完整 GNFS 流水线 (~5min)
./scripts/test.sh L1                   # 渐进式 Level 1
./scripts/test.sh progressive 1 3      # L1 至 L3

# 全量套件
./scripts/test.sh full                 # ctest + E2E + Progressive L1-L2
./scripts/test.sh thorough             # 全模块 + 集成 + L1-L3
./scripts/test.sh nightly              # 全部 + L4 + L5 + stress L1

# 压力测试
./scripts/test.sh stress               # 50/60 位, 全部
./scripts/test.sh stress 1 1           # 仅 50 位 (164-bit, ~2.6h)
./scripts/test.sh stress 1 2           # 50 + 60 位

# 工具
./scripts/test.sh list                 # 查看全部测试、模块、超时、分级
./scripts/test.sh build                # 仅编译
./scripts/test.sh --no-build smoke     # 跳过编译直接跑
./scripts/test.sh -t Release full      # Release 模式
./scripts/test.sh --fail-fast full     # 首个失败即停
./scripts/test.sh --timeout 30 run test_kleinjung  # 自定义超时
./scripts/test.sh bench --save         # 性能基准 + 保存结果
./scripts/test.sh watch                # 监视文件变更自动重测 (需 fswatch)
```

### 测试分级

| 分级 | 超时 | 测试 | 描述 |
|------|------|------|------|
| **instant** | 10s | 11 | 单元测试: integer, small_vector, thread_pool, factor_base, special_q, relation_collector, cofactor, linalg, sqrt, sqrt_debug, murphy |
| **fast** | 60s | 1 | 筛法集成 (test_sieve_basic) |
| **slow** | 120–300s | 5 | 回归门禁, Kleinjung, 格筛, E2E 流水线, factor_with_kleinjung |
| **heavy** | 600–3600s | 3 | 渐进 L3-L5, 25 位基准, Kleinjung large |
| **stress** | 43200s | 1 | 50 位 (L1) + 60 位 (L2) + 100 位 (L3) |

### 测试文件 (共 41 个)

| 分类 | 测试 | 描述 |
|------|------|------|
| **核心** | test_integer, test_small_vector, test_thread_pool, test_work_stealing | 基础类型, 线程 |
| **多项式** | test_base_m, test_int_polynomial, test_polynomial_context, test_polynomial_optimizer, test_murphy, test_kleinjung, test_kleinjung_large | 多项式选择流水线 |
| **因子基** | test_factor_base | Builder + Cantor-Zassenhaus |
| **筛法** | test_special_q, test_sieve_basic, test_lattice_sieve, test_line_sieve, test_bucket_sieve | 各筛法策略 |
| **余因子** | test_cofactor, test_squfof | 试除, ECM, SQUFOF |
| **关系** | test_relation_collector, test_filter, test_ooc_relations | 收集, 合并, OOC |
| **线性代数** | test_linalg, test_block_wiedemann, test_schirokauer_deg4, test_mmap_csr | 求解器, 矩阵基础设施 |
| **平方根** | test_sqrt, test_sqrt_debug, test_class_group | Hensel, Couveignes, 类群 |
| **集成** | test_integration, test_edge_cases, test_regressions, test_regression_gate | 跨模块, 边界条件 |
| **E2E** | test_gnfs_e2e, test_factor_with_kleinjung, test_gnfs_progressive | 完整流水线测试 |
| **基准** | test_25digit, test_stress | 性能基准测试 |
| **API** | test_api, test_i18n, test_params | 公开 API, 国际化, 参数 |

### 场景推荐

| 场景 | 命令 | 耗时 |
|------|------|------|
| 改了一个函数，快速验证 | `./scripts/test.sh` | ~5s |
| 改了 linalg 模块 | `./scripts/test.sh module linalg` | ~1s |
| 改了核心流程 | `./scripts/test.sh e2e` | ~5min |
| 不确定改了什么 | `./scripts/test.sh changed` | 自动 |
| 特性分支合并门禁 | `./scripts/test.sh gate` | ~20s |
| 大改动，全面回归 | `./scripts/test.sh full` | ~10min |
| PR 最终验证 | `./scripts/test.sh thorough` | ~30min |
| 性能基准 | `./scripts/test.sh bench --save` | ~1hr |
| 验证大数分解能力 | `./scripts/test.sh stress 1 1` | ~2.6h |

## 构建选项

### 构建类型

```bash
# Debug 构建 (断言开启, 调试符号, 无优化)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
make -C build -j$(sysctl -n hw.ncpu)

# Release 构建 (O3 + LTO + 原生 CPU 标志)
cmake -B build -DCMAKE_BUILD_TYPE=Release
make -C build -j$(sysctl -n hw.ncpu)

# Release + 调试信息
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -C build -j$(sysctl -n hw.ncpu)
```

### CMake 选项

| 选项 | 默认 | 描述 |
|------|------|------|
| `GNFS_BUILD_TESTS` | `ON` | 构建测试可执行文件 |
| `GNFS_ENABLE_ASAN` | `OFF` | 启用 AddressSanitizer |
| `GNFS_ENABLE_TSAN` | `OFF` | 启用 ThreadSanitizer |
| `GNFS_ENABLE_UBSAN` | `OFF` | 启用 UndefinedBehaviorSanitizer |

```bash
# 使用 AddressSanitizer (检测内存错误)
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DGNFS_ENABLE_ASAN=ON

# 使用 ThreadSanitizer (检测数据竞争)
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DGNFS_ENABLE_TSAN=ON

# 不构建测试
cmake -B build -DCMAKE_BUILD_TYPE=Release -DGNFS_BUILD_TESTS=OFF
```

### 自动检测特性

构建系统自动检测并启用:

- **GMP** (必需) — 任意精度整数算术
- **NTL** (可选) — 附加数论例程; 添加 `GNFS_HAVE_NTL=1` 宏
- **Metal** (macOS, 可选) — GPU 加速框架
- **原生 CPU 标志** — Apple Silicon 上 `-mcpu=native`, x86 上 `-march=native`
- **ThinLTO** (Clang/macOS) 或 **LTO** (GCC), Release 模式

## 依赖

| 库 | 必需 | 版本 | 用途 |
|----|:----:|------|------|
| **GMP** | 是 | 6.0+ | 任意精度整数算术 (`mpz_class`) |
| **pthreads** | 是 | — | 多线程 (`std::thread`, `std::mutex` 等) |
| **NTL** | 否 | 11.0+ | 数论 (多项式算术, 可选功能) |
| **Metal** | 否 | — | macOS GPU 加速框架 (未来使用) |

所有依赖均为成熟、广泛可用的库。GMP 和 pthreads 是唯二的硬性依赖。

## 设计约定

### 元素表示

**元素为 `a - b*α` (不是 `a + b*α`)**。这是整个代码库的基本约定。修改此约定将破坏范数计算、Schirokauer 映射和平方根提取。

### 整数类型

- **`gnfs::core::Integer`** — GMP `mpz_class` 封装，用于所有大整数运算
- **`uint64_t`** — 用于因子基素数、筛索引
- **`__uint128_t`** — 用于 `uint64_t` 可能溢出的中间乘积
- **`Integer`** — 当 `__uint128_t` 也不够时使用 (Hensel 提升等)

### 线程安全

- `RelationCollector` 使用 `std::atomic` 计数器和线程局部关系缓冲
- `LatticeSieve::sieve_parallel()` 在工作线程间分配 Special-Q 素数
- `BlockLanczos` 使用 `ThreadPool` 进行并行 SpMV (稀疏矩阵-向量乘)
- `FactorBaseBuilder` 并行化 Cantor-Zassenhaus 求根

### 命名约定

- 函数和变量: `snake_case`
- 类型和类: `PascalCase`
- 命名空间: `gnfs::core`, `gnfs::linalg`, `gnfs::sieve`, `gnfs::api` 等
- 头文件: `snake_case.hpp`

### 错误处理

- 内部逻辑错误: `assert()` 或自定义 `GNFS_ASSERT` 宏
- 可恢复运行时错误: `std::optional` 或错误码
- 致命错误: `throw std::runtime_error("描述")`
- 无空 catch 块——所有异常至少记录日志

## 项目结构

```
GNFS/
├── include/gnfs/           # 53 个头文件 (.hpp), 10 个子模块
│   ├── api/           (6)  # 公开 API 层
│   ├── core/          (6)  # 基础类型
│   ├── polynomial/    (6)  # 多项式选择
│   ├── factor_base/   (2)  # 因子基构建
│   ├── sieve/         (4)  # 格筛 / Bucket 筛
│   ├── cofactor/      (5)  # 余因子分解策略
│   ├── relation/      (3)  # 关系收集与合并
│   ├── linalg/        (8)  # 线性代数求解器
│   ├── sqrt/          (7)  # 平方根提取
│   └── util/          (6)  # 工具集
├── src/                    # 14 个源文件 (.cpp)
│   ├── api/           (2)  # Pipeline, Factorizer
│   ├── cli/           (1)  # CLI 入口
│   ├── core/          (3)  # Integer, Polynomial, Relation
│   ├── factor_base/   (1)  # Builder
│   ├── linalg/        (3)  # Block Lanczos, Block Wiedemann, Matrix Builder
│   ├── polynomial/    (1)  # Base-m
│   ├── sieve/         (1)  # Lattice Sieve
│   └── sqrt/          (2)  # Algebraic Sqrt, Rational Sqrt
├── tests/                  # 41 个测试文件 (.cpp)
├── scripts/
│   ├── test.sh             # 统一测试运行器 (超时, 分级, 心跳)
│   └── feature-branch.sh   # 特性分支工作流工具
├── docs/                   # 文档和设计方案
├── CMakeLists.txt          # 构建配置
├── CLAUDE.md               # 开发指令
├── BACKLOG.md              # 待办追踪 (P1-P3 优先级)
├── RESOLVED.md             # 已解决记录
├── README.md               # 本文件 (中文)
├── README-EN.md            # English version
└── LICENSE                 # GPL-2.0
```

### 代码统计

| 分类 | 文件 | 行数 | 描述 |
|------|:----:|-----:|------|
| 头文件 | 53 | ~18,700 | 模板密集设计; 大部分实现在 `.hpp` 中 |
| 源文件 | 14 | ~3,600 | 编译实现文件 |
| 测试 | 41 | ~19,000 | 单元 / 集成 / 回归 / E2E / 压力 |
| **合计** | **108** | **~41,200** | |

## 贡献

欢迎贡献! 请遵循以下规范:

1. **Fork** 仓库
2. **创建特性分支** (带日期前缀): `feat/YYMMDD-描述`
3. **确保测试通过**: `./scripts/test.sh gate` (合并门禁, ~20s)
4. **遵循约定**: C++20 风格, `snake_case` 函数, `PascalCase` 类型
5. **提交 Pull Request**，附清晰描述

### 提交约定

本项目使用 [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <简短描述>

[可选正文: 解释 why, 而非 what]
```

| 类型 | 用途 | 示例 |
|------|------|------|
| `feat` | 新功能 | `feat(sieve): add bucket sieve for large factor bases` |
| `fix` | Bug 修复 | `fix(sqrt): prevent Hensel p^d overflow with Integer` |
| `perf` | 性能优化 | `perf(lanczos): parallelize SpMV with ThreadPool` |
| `refactor` | 重构 | `refactor(core): extract polynomial context` |
| `test` | 测试 | `test(linalg): add Block Wiedemann edge case tests` |
| `chore` | 构建/工具 | `chore: update CMakeLists for NTL optional` |
| `docs` | 文档 | `docs: update README with architecture details` |

**scope**: `core`, `polynomial`, `factor_base`, `sieve`, `cofactor`, `relation`, `linalg`, `sqrt`, `util`, `api`, `cli`

### 分支命名

| 类型 | 格式 | 示例 |
|------|------|------|
| 功能 | `feat/YYMMDD-desc` | `feat/260315-block-wiedemann` |
| 修复 | `fix/YYMMDD-desc` | `fix/260308-hensel-overflow` |
| 性能 | `perf/YYMMDD-desc` | `perf/260308-lanczos-simd` |
| 实验 | `exp/YYMMDD-desc` | `exp/260308-neon-sieve` |

## 参考文献

### GNFS 核心理论

- Lenstra, A.K., Lenstra, H.W. (eds.) *The Development of the Number Field Sieve*, Lecture Notes in Mathematics 1554, Springer (1993)
- Buhler, J.P., Lenstra, H.W., Pomerance, C. *Factoring integers with the number field sieve*, Journal of Cryptology 6(2):85-105 (1993)
- Briggs, M.E. *An Introduction to the General Number Field Sieve*, Master's thesis, Virginia Tech (1998)

### 多项式选择

- Kleinjung, T. *On polynomial selection for the general number field sieve*, Mathematics of Computation 75(256):2037-2047 (2006)
- Murphy, B.A. *Polynomial Selection for the Number Field Sieve Integer Factorisation Algorithm*, PhD thesis, Australian National University (1999)

### 线性代数

- Montgomery, P.L. *A block Lanczos algorithm for finding dependencies over GF(2)*, Advances in Cryptology — EUROCRYPT '95, pp. 106-120 (1995)
- Coppersmith, D. *Solving homogeneous linear equations over GF(2) via block Wiedemann algorithm*, Mathematics of Computation 62(205):333-350 (1994)

### 平方根

- Couveignes, J.-M. *Computing a square root for the number field sieve*, in *The Development of the Number Field Sieve*, pp. 95-107 (1993)
- Nguyen, P.Q. *A Montgomery-like Square Root for the Number Field Sieve*, ANTS-III, pp. 151-168 (1998)

### 余因子分解

- Lenstra, H.W. Jr. *Factoring integers with elliptic curves*, Annals of Mathematics 126:649-673 (1987)
- Shanks, D. *SQUFOF*, unpublished; analyzed in Gower, J.E. *Square form factorization*, PhD thesis (2004)

### 参考实现

- [CADO-NFS](https://cado-nfs.gitlabpages.inria.fr/) — 最先进的 GNFS 实现 (INRIA/LORIA)
- [msieve](https://github.com/radii/msieve) — Jason Papadopoulos 的整数分解库

## 许可证

本项目采用 **GNU General Public License v2.0** 许可——详见 [LICENSE](LICENSE) 文件。
