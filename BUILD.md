# GNFS 项目构建指南

## 概述

本文档提供 GNFS 项目的详细构建说明和故障排除指南。

## 系统要求

### macOS

```bash
# 安装 Xcode Command Line Tools
xcode-select --install

# 使用 Homebrew 安装依赖
brew install cmake gmp

# （可选）安装 NTL
brew install ntl
```

### Linux (Ubuntu/Debian)

```bash
# 安装编译工具
sudo apt update
sudo apt install build-essential cmake

# 安装 GMP
sudo apt install libgmp-dev

# （可选）安装 NTL
sudo apt install libntl-dev
```

### Linux (Fedora/RHEL)

```bash
# 安装编译工具
sudo dnf install gcc-c++ cmake

# 安装 GMP
sudo dnf install gmp-devel

# （可选）安装 NTL
sudo dnf install ntl-devel
```

## 构建步骤

### 1. 克隆或下载项目

```bash
cd /path/to/GNFS
```

### 2. 创建构建目录

```bash
mkdir -p build
cd build
```

### 3. 配置项目

#### Release 构建（推荐）

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release
```

#### Debug 构建（用于开发）

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
```

#### 禁用测试

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DGNFS_BUILD_TESTS=OFF
```

### 4. 编译

```bash
# 使用所有可用 CPU 核心编译
cmake --build . -j$(nproc)

# 或在 macOS 上
cmake --build . -j$(sysctl -n hw.ncpu)

# 或手动指定核心数
cmake --build . -j8
```

### 5. 运行测试

```bash
# 运行所有测试
ctest

# 详细输出
ctest --verbose

# 运行特定测试
ctest -R test_integer

# 并行运行测试
ctest -j8
```

## 常见问题

### 问题 1: 找不到 GMP

```
CMake Error: Could not find GMP library
```

**解决方案**:

```bash
# macOS
brew install gmp
export CPATH=/opt/homebrew/include
export LIBRARY_PATH=/opt/homebrew/lib

# Linux
sudo apt install libgmp-dev  # Debian/Ubuntu
sudo dnf install gmp-devel   # Fedora/RHEL
```

### 问题 2: C++20 支持

```
CMake Error: The compiler does not support C++20
```

**解决方案**: 更新编译器

```bash
# Ubuntu
sudo apt install gcc-10 g++-10
export CC=gcc-10
export CXX=g++-10

# 或使用 Clang
sudo apt install clang-12
export CC=clang-12
export CXX=clang++-12
```

### 问题 3: 找不到头文件

```
fatal error: 'gnfs/core/integer.hpp' file not found
```

**解决方案**: 确保所有头文件都已创建，并且 include 路径正确。检查项目结构：

```
GNFS/
├── include/
│   └── gnfs/
│       ├── core/
│       ├── polynomial/
│       ├── factor_base/
│       ├── sieve/
│       ├── cofactor/
│       ├── relation/
│       ├── linalg/
│       ├── sqrt/
│       └── util/
├── src/
│   ├── core/
│   ├── polynomial/
│   ├── factor_base/
│   ├── sieve/
│   ├── cofactor/
│   ├── relation/
│   ├── linalg/
│   ├── sqrt/
│   └── util/
└── tests/
```

### 问题 4: 链接错误

```
undefined reference to `mpz_init'
```

**解决方案**: 确保正确链接 GMP 库

```bash
# 手动指定 GMP 路径
cmake .. -DGMP_LIBRARY=/usr/local/lib/libgmp.a \
         -DGMP_INCLUDE_DIR=/usr/local/include
```

## 性能优化

### 编译优化选项

```bash
# 最大优化（Release）
cmake .. -DCMAKE_BUILD_TYPE=Release

# 针对本机 CPU 优化（自动启用 -march=native）
cmake .. -DCMAKE_BUILD_TYPE=Release

# 使用 LTO（链接时优化）
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
```

### 运行时配置

```bash
# 设置线程数
export OMP_NUM_THREADS=16

# 限制内存使用（某些系统）
ulimit -m 16777216  # 16GB
```

## IDE 集成

### Visual Studio Code

1. 安装 C/C++ 扩展
2. 安装 CMake Tools 扩展
3. 打开项目文件夹
4. 选择编译器套件
5. 点击 "Build" 按钮

### CLion

1. 打开项目文件夹
2. CLion 会自动检测 CMakeLists.txt
3. 等待 CMake 配置完成
4. 点击构建按钮

### Xcode (macOS)

```bash
# 生成 Xcode 项目
cmake .. -G Xcode

# 打开项目
open GNFS.xcodeproj
```

## 安装

```bash
# 安装到系统路径（需要 sudo）
sudo cmake --install .

# 安装到自定义路径
cmake --install . --prefix=/path/to/install
```

## 清理

```bash
# 清理构建文件
cd build
cmake --build . --target clean

# 完全重新构建
cd ..
rm -rf build
mkdir build
cd build
cmake ..
cmake --build .
```

## 调试

### 使用 GDB

```bash
# Debug 构建
cmake .. -DCMAKE_BUILD_TYPE=Debug

# 运行特定测试
gdb ./test_integer

# 在 GDB 中
(gdb) run
(gdb) backtrace
```

### 使用 Valgrind（内存检查）

```bash
# 安装 Valgrind
sudo apt install valgrind  # Linux

# 运行内存检查
valgrind --leak-check=full ./test_integer
```

### 使用地址消毒器（AddressSanitizer）

```bash
# 使用 ASAN 构建
cmake .. -DCMAKE_BUILD_TYPE=Debug \
         -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"

# 运行测试
./test_integer
```

## 生成文档

```bash
# 安装 Doxygen
brew install doxygen  # macOS
sudo apt install doxygen  # Linux

# 生成文档（如果有 Doxyfile）
doxygen Doxyfile
```

## 持续集成

项目使用 CMake，可以轻松集成到 CI/CD 流程：

### GitHub Actions 示例

```yaml
name: Build and Test

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@v2
    - name: Install dependencies
      run: |
        sudo apt update
        sudo apt install -y libgmp-dev
    - name: Configure
      run: cmake -B build -DCMAKE_BUILD_TYPE=Release
    - name: Build
      run: cmake --build build -j$(nproc)
    - name: Test
      run: cd build && ctest --verbose
```

## 获取帮助

如果遇到问题：

1. 查看 CMake 配置输出的摘要信息
2. 检查 CMakeCache.txt 中的变量设置
3. 查看 CMakeFiles/CMakeError.log
4. 提交 Issue 并附上：
   - 操作系统和版本
   - 编译器和版本
   - CMake 版本
   - 完整的错误信息
