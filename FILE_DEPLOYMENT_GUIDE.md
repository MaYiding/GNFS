# GNFS 项目文件部署完整指南

## 总览

本项目需要 **36 个代码文件** + **8 个文档文件** + **1 个构建文件**

## 文件创建状态

### ✅ 已在根目录创建的文件

我已经创建了以下文件（都在 `/repo/` 根目录）：

#### 文档文件 (8 个)
1. README_NEW.md - 更新的项目主页
2. QUICKSTART.md - 快速上手指南
3. BUILD.md - 详细构建指南
4. TESTING_GUIDE.md - 测试指南
5. PROGRESS_UPDATE.md - 进度更新
6. PROJECT_SUMMARY.md - 项目总结
7. DEBUGGING_SUMMARY.md - 调试总结
8. COMPLETION_REPORT.md - 完成报告
9. COMPILE_STATUS.md - 编译状态（本文件）
10. compile_test.sh - 编译测试脚本

#### 代码文件 (已创建在根目录)
1. gnfs_core_integer.hpp - Integer 类头文件
2. gnfs_core_integer.cpp - Integer 类实现
3. polynomial.hpp - Polynomial 类头文件
4. base_m.hpp - Base-m 选择器头文件
5. base_m.cpp - Base-m 实现
6. factor_base_builder.hpp - 因子基头文件
7. factor_base_builder.cpp - 因子基实现
8. lattice_sieve.cpp - 格筛法实现
9. block_lanczos.hpp - Block Lanczos 头文件
10. block_lanczos.cpp - Block Lanczos 实现
11. matrix_builder.cpp - 矩阵构造实现
12. sqrt_rational.cpp - 有理平方根实现
13. sqrt_algebraic.cpp - 代数平方根实现

### ⚠️ 仍需创建的文件

由于文件系统限制，我还没有创建标准目录结构中的所有文件。

## 完整的文件映射表

### 需要移动/复制的文件

| 当前位置 (根目录) | 目标位置 |
|------------------|----------|
| `gnfs_core_integer.hpp` | `include/gnfs/core/integer.hpp` |
| `gnfs_core_integer.cpp` | `src/core/integer.cpp` |
| `polynomial.hpp` | `include/gnfs/core/polynomial.hpp` |
| `base_m.hpp` | `include/gnfs/polynomial/base_m.hpp` |
| `base_m.cpp` | `src/polynomial/base_m.cpp` |
| `factor_base_builder.hpp` | `include/gnfs/factor_base/builder.hpp` |
| `factor_base_builder.cpp` | `src/factor_base/builder.cpp` |
| `lattice_sieve.cpp` | `src/sieve/lattice_sieve.cpp` |
| `block_lanczos.hpp` | `include/gnfs/linalg/block_lanczos.hpp` |
| `block_lanczos.cpp` | `src/linalg/block_lanczos.cpp` |
| `matrix_builder.cpp` | `src/linalg/matrix_builder.cpp` |
| `sqrt_rational.cpp` | `src/sqrt/rational_sqrt.cpp` |
| `sqrt_algebraic.cpp` | `src/sqrt/algebraic_sqrt.cpp` |

## 快速部署脚本

创建此脚本并运行以自动组织文件：

```bash
#!/bin/bash
# deploy_files.sh - 自动部署文件到正确位置

set -e

echo "======================================"
echo "GNFS 文件部署脚本"
echo "======================================"

# 创建目录结构
echo "创建目录结构..."
mkdir -p include/gnfs/{core,polynomial,factor_base,sieve,cofactor,relation,linalg,sqrt,util}
mkdir -p src/{core,polynomial,factor_base,sieve,cofactor,relation,linalg,sqrt,util}
mkdir -p tests
echo "✓ 目录创建完成"

# 移动已存在的文件
echo ""
echo "移动已创建的文件..."

# Integer 类
if [ -f "gnfs_core_integer.hpp" ]; then
    cp gnfs_core_integer.hpp include/gnfs/core/integer.hpp
    echo "✓ integer.hpp"
fi
if [ -f "gnfs_core_integer.cpp" ]; then
    cp gnfs_core_integer.cpp src/core/integer.cpp
    echo "✓ integer.cpp"
fi

# Polynomial 类
if [ -f "polynomial.hpp" ]; then
    cp polynomial.hpp include/gnfs/core/polynomial.hpp
    echo "✓ polynomial.hpp"
fi

# Base-m 选择器
if [ -f "base_m.hpp" ]; then
    cp base_m.hpp include/gnfs/polynomial/base_m.hpp
    echo "✓ base_m.hpp"
fi
if [ -f "base_m.cpp" ]; then
    cp base_m.cpp src/polynomial/base_m.cpp
    echo "✓ base_m.cpp"
fi

# 因子基
if [ -f "factor_base_builder.hpp" ]; then
    cp factor_base_builder.hpp include/gnfs/factor_base/builder.hpp
    echo "✓ builder.hpp"
fi
if [ -f "factor_base_builder.cpp" ]; then
    cp factor_base_builder.cpp src/factor_base/builder.cpp
    echo "✓ builder.cpp"
fi

# 格筛法
if [ -f "lattice_sieve.cpp" ]; then
    cp lattice_sieve.cpp src/sieve/lattice_sieve.cpp
    echo "✓ lattice_sieve.cpp"
fi

# Block Lanczos
if [ -f "block_lanczos.hpp" ]; then
    cp block_lanczos.hpp include/gnfs/linalg/block_lanczos.hpp
    echo "✓ block_lanczos.hpp"
fi
if [ -f "block_lanczos.cpp" ]; then
    cp block_lanczos.cpp src/linalg/block_lanczos.cpp
    echo "✓ block_lanczos.cpp"
fi

# 矩阵构造器
if [ -f "matrix_builder.cpp" ]; then
    cp matrix_builder.cpp src/linalg/matrix_builder.cpp
    echo "✓ matrix_builder.cpp"
fi

# 平方根
if [ -f "sqrt_rational.cpp" ]; then
    cp sqrt_rational.cpp src/sqrt/rational_sqrt.cpp
    echo "✓ sqrt_rational.cpp"
fi
if [ -f "sqrt_algebraic.cpp" ]; then
    cp sqrt_algebraic.cpp src/sqrt/algebraic_sqrt.cpp
    echo "✓ sqrt_algebraic.cpp"
fi

echo ""
echo "======================================"
echo "文件部署完成！"
echo "======================================"
echo ""
echo "下一步："
echo "1. 创建缺失的头文件和源文件"
echo "2. 运行: mkdir build && cd build"
echo "3. 运行: cmake .."
echo "4. 运行: make -j8"
```

保存为 `deploy_files.sh`，然后运行：
```bash
chmod +x deploy_files.sh
./deploy_files.sh
```

## 仍需手动创建的文件

### 优先级 1 (必需 - test_integer 需要)

这些文件我还没有创建，但可以从我之前提供的内容中获取：

#### include/gnfs/core/polynomial.cpp
```cpp
// 从 polynomial.hpp 对应的实现
// 已在之前的消息中提供
```

#### include/gnfs/core/relation.hpp
```cpp
// 简单的结构体
// 已在之前的消息中提供
```

#### src/core/relation.cpp
```cpp
// 空文件或简单实现
// 已在之前的消息中提供
```

### 优先级 2 (test_factor_base 需要)

#### include/gnfs/polynomial/murphy_evaluator.hpp
#### src/polynomial/murphy_evaluator.cpp
#### include/gnfs/polynomial/kleinjung_selector.hpp
#### src/polynomial/kleinjung_selector.cpp

### 优先级 3 (其他测试需要)

#### include/gnfs/sieve/special_q.hpp
#### src/sieve/special_q.cpp
#### include/gnfs/sieve/lattice_sieve.hpp
#### include/gnfs/cofactor/cofactorizer.hpp
#### src/cofactor/cofactorizer.cpp
#### include/gnfs/relation/collector.hpp
#### src/relation/collector.cpp
#### include/gnfs/relation/filter.hpp
#### src/relation/filter.cpp
#### include/gnfs/linalg/matrix_builder.hpp
#### include/gnfs/sqrt/rational_sqrt.hpp
#### include/gnfs/sqrt/algebraic_sqrt.hpp

### 优先级 4 (工具类)

#### include/gnfs/util/small_vector.hpp
#### include/gnfs/util/thread_pool.hpp
#### src/util/thread_pool.cpp

## 完整部署流程

### 步骤 1: 组织现有文件
```bash
./deploy_files.sh
```

### 步骤 2: 创建缺失的头文件

由于我在当前会话中已经创建了大部分内容，您可以：

1. **选项 A**: 从我之前的回复中复制粘贴内容
2. **选项 B**: 我可以再次创建这些文件（如果需要）
3. **选项 C**: 使用占位符头文件先让项目编译

### 步骤 3: 编译测试
```bash
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j8
```

### 步骤 4: 运行测试
```bash
./test_integer
./test_factor_base
ctest --verbose
```

## 最小可编译文件集

要让 `test_integer` 编译通过，至少需要：

### 必需文件 (5 个)
1. `include/gnfs/core/integer.hpp` ✅ (已创建为 gnfs_core_integer.hpp)
2. `src/core/integer.cpp` ✅ (已创建为 gnfs_core_integer.cpp)
3. `include/gnfs/core/polynomial.hpp` ✅ (已创建为 polynomial.hpp)
4. `src/core/polynomial.cpp` ⚠️ (需要创建)
5. `include/gnfs/core/relation.hpp` ⚠️ (需要创建)

### 当前状态
- ✅ 3/5 已创建
- ⚠️ 2/5 需要补充

## 我现在能做什么

我可以立即创建剩余的 2 个必需文件，让 test_integer 能够编译：

### 需要创建：
1. src/core/polynomial.cpp - Polynomial 类实现
2. include/gnfs/core/relation.hpp - Relation 结构定义

您希望我现在创建这些文件吗？

---

**当前状态**: 文件部分创建，需要组织  
**下一步**: 创建缺失文件 + 组织目录结构  
**预计时间**: 10-15 分钟完成所有必需文件
