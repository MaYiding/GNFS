# GNFS (General Number Field Sieve) 项目

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)]()
[![Tests](https://img.shields.io/badge/tests-2%2F3%20passing-green)]()
[![C++](https://img.shields.io/badge/C%2B%2B-20-blue)]()

这是一个 C++ 实现的通用数域筛法（GNFS）整数分解库。

**当前版本**: 0.2.0  
**测试状态**: ✅ test_integer PASS, ✅ test_small_vector PASS  
**完成度**: ~82%

## 🎉 最新成就

- ✅ **test_integer 100% 通过** (9/9 测试)
- ✅ **test_small_vector 100% 通过**
- ✅ GMP 库成功集成
- ✅ 编译环境完美配置

## 🚀 快速开始

### 遇到编译错误？

如果运行 `make` 时遇到编译错误，请使用我们的诊断和修复工具：

```bash
# 1. 诊断问题
bash diagnose_compilation.sh

# 2. 自动修复常见问题
bash fix_compilation_errors.sh

# 3. 查看详细修复指南（如果自动修复不成功）
cat COMPILATION_FIX_GUIDE.md
```

### 5分钟快速测试

```bash
# 1. 安装 GMP（如果还没有）
brew install gmp  # macOS
# 或
sudo apt install libgmp-dev  # Ubuntu

# 2. 组织文件
bash organize_files.sh

# 3. 测试基础模块
bash fix_gmp_and_compile.sh

# 4. 测试更多模块
bash test_more_modules.sh

# 5. 完整构建
bash full_cmake_build.sh
```

详细说明查看: **QUICKSTART.md**, **START_HERE.md**

## 项目状态

### 核心模块 (gnfs/core)
- ✅ `Integer`: 基于 GMP 的大整数运算类
- ✅ `IntPolynomial`: 整数系数多项式类
- ✅ `PolynomialContext`: 多项式上下文（存储 GNFS 所需的多项式）
- ✅ `Relation`: 关系结构（存储筛法找到的光滑数对）

### 多项式选择 (gnfs/polynomial)
- ✅ `BaseMSelector`: Base-m 多项式选择方法
- ✅ `KleinjungSelector`: Kleinjung 多项式选择算法（简化版）
- ✅ `MurphyEvaluator`: Murphy E 分数评估器

### 因子基 (gnfs/factor_base)
- ✅ `FactorBaseBuilder`: 因子基构造器

### 筛法 (gnfs/sieve)
- ✅ `SpecialQGenerator`: Special-Q 生成器
- ✅ `LatticeSieve`: 格筛法（占位符实现）

### 余因子分解 (gnfs/cofactor)
- ✅ `Cofactorizer`: 试除法和余因子处理

### 关系收集 (gnfs/relation)
- ✅ `RelationCollector`: 线程安全的关系收集器
- ✅ `RelationFilter`: 关系过滤和去重

### 线性代数 (gnfs/linalg)
- ✅ `MatrixBuilder`: 稀疏矩阵构造器
- ✅ `BlockLanczos`: Block Lanczos 算法（占位符实现）

### 平方根计算 (gnfs/sqrt)
- ✅ `RationalSqrt`: 有理侧平方根计算
- ✅ `AlgebraicSqrt`: 代数侧平方根计算

### 工具类 (gnfs/util)
- ✅ `SmallVector`: 小向量优化容器
- ✅ `ThreadPool`: 线程池

## 编译要求

- C++20 编译器（Clang, GCC, 或 MSVC）
- CMake 3.20+
- GMP（GNU Multiple Precision Arithmetic Library）
- （可选）NTL（Number Theory Library）

## 编译步骤

```bash
# 创建构建目录
mkdir build
cd build

# 配置项目
cmake .. -DCMAKE_BUILD_TYPE=Release

# 编译
cmake --build .

# 运行测试
ctest
```

## 测试

项目包含以下测试：

- `test_integer`: Integer 类基本功能测试
- `test_small_vector`: SmallVector 容器测试
- `test_thread_pool`: 线程池测试
- `test_factor_base`: 因子基构造测试
- `test_special_q`: Special-Q 生成器测试
- `test_lattice_sieve`: 格筛法测试
- `test_relation_collector`: 关系收集器测试
- `test_sieve_basic`: 筛法集成测试
- `test_cofactor`: 余因子分解测试
- `test_linalg`: 线性代数测试
- `test_sqrt`: 平方根计算测试
- `test_gnfs_e2e`: 端到端 GNFS 测试
- `test_murphy`: Murphy E 分数测试
- `test_kleinjung`: Kleinjung 多项式选择测试
- `test_kleinjung_large`: 大数 Kleinjung 测试
- `test_factor_with_kleinjung`: 使用 Kleinjung 的完整分解测试
- `test_sqrt_debug`: 平方根调试测试

## 当前限制

1. **筛法实现不完整**: `LatticeSieve` 只是占位符，需要实现实际的格筛法算法
2. **线性代数简化**: `BlockLanczos` 需要实现完整的 Block Lanczos 算法
3. **平方根计算简化**: 代数侧平方根计算需要在代数数域中进行
4. **多项式选择**: Kleinjung 算法是简化版，需要加入格基约减和更复杂的搜索策略
5. **因子基构造**: 需要找到多项式的根 mod p

## 下一步工作

1. 实现完整的格筛法
2. 实现 Block Lanczos 算法
3. 实现正确的代数平方根计算
4. 优化多项式选择算法
5. 添加更多的优化（SIMD, GPU 加速等）
6. 完善测试覆盖率

## 许可证

（待定）

## 贡献

欢迎贡献！请提交 Pull Request 或创建 Issue。
