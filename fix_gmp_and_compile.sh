#!/bin/bash
# fix_gmp_and_compile.sh - 诊断并修复 GMP 问题，然后编译

echo "=========================================="
echo "GNFS GMP 诊断和修复"
echo "=========================================="
echo ""

# 颜色定义
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# 1. 检测操作系统
echo -e "${BLUE}步骤 1: 检测操作系统${NC}"
OS="unknown"
if [[ "$OSTYPE" == "darwin"* ]]; then
    OS="macos"
    echo "检测到: macOS"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    OS="linux"
    echo "检测到: Linux"
else
    echo "检测到: $OSTYPE"
fi
echo ""

# 2. 查找 GMP
echo -e "${BLUE}步骤 2: 查找 GMP 库${NC}"

GMP_FOUND=0
GMP_INCLUDE=""
GMP_LIB=""

# macOS 可能的位置
if [ "$OS" == "macos" ]; then
    echo "在 macOS 常见位置搜索..."
    
    # Homebrew (Apple Silicon)
    if [ -d "/opt/homebrew/include" ] && [ -f "/opt/homebrew/include/gmp.h" ]; then
        GMP_INCLUDE="/opt/homebrew/include"
        GMP_LIB="/opt/homebrew/lib"
        GMP_FOUND=1
        echo -e "${GREEN}✓ 找到 GMP (Homebrew Apple Silicon)${NC}"
        echo "  Include: $GMP_INCLUDE"
        echo "  Lib: $GMP_LIB"
    # Homebrew (Intel)
    elif [ -d "/usr/local/include" ] && [ -f "/usr/local/include/gmp.h" ]; then
        GMP_INCLUDE="/usr/local/include"
        GMP_LIB="/usr/local/lib"
        GMP_FOUND=1
        echo -e "${GREEN}✓ 找到 GMP (Homebrew Intel)${NC}"
        echo "  Include: $GMP_INCLUDE"
        echo "  Lib: $GMP_LIB"
    fi
fi

# Linux 可能的位置
if [ "$OS" == "linux" ]; then
    echo "在 Linux 常见位置搜索..."
    
    if [ -f "/usr/include/gmp.h" ]; then
        GMP_INCLUDE="/usr/include"
        GMP_LIB="/usr/lib"
        GMP_FOUND=1
        echo -e "${GREEN}✓ 找到 GMP (系统路径)${NC}"
        echo "  Include: $GMP_INCLUDE"
        echo "  Lib: $GMP_LIB"
    elif [ -f "/usr/local/include/gmp.h" ]; then
        GMP_INCLUDE="/usr/local/include"
        GMP_LIB="/usr/local/lib"
        GMP_FOUND=1
        echo -e "${GREEN}✓ 找到 GMP (local)${NC}"
        echo "  Include: $GMP_INCLUDE"
        echo "  Lib: $GMP_LIB"
    fi
fi

# 使用 pkg-config
if [ $GMP_FOUND -eq 0 ] && command -v pkg-config &> /dev/null; then
    echo "尝试使用 pkg-config..."
    if pkg-config --exists gmp 2>/dev/null; then
        GMP_INCLUDE=$(pkg-config --cflags-only-I gmp | sed 's/-I//')
        GMP_LIB=$(pkg-config --libs-only-L gmp | sed 's/-L//')
        GMP_FOUND=1
        echo -e "${GREEN}✓ 找到 GMP (pkg-config)${NC}"
        echo "  Include: $GMP_INCLUDE"
        echo "  Lib: $GMP_LIB"
    fi
fi

echo ""

# 3. 如果没找到，提供安装指南
if [ $GMP_FOUND -eq 0 ]; then
    echo -e "${RED}✗ 未找到 GMP 库${NC}"
    echo ""
    echo "请安装 GMP:"
    echo ""
    if [ "$OS" == "macos" ]; then
        echo "  brew install gmp"
    elif [ "$OS" == "linux" ]; then
        echo "  # Ubuntu/Debian:"
        echo "  sudo apt update && sudo apt install libgmp-dev"
        echo ""
        echo "  # Fedora/RHEL:"
        echo "  sudo dnf install gmp-devel"
    fi
    echo ""
    exit 1
fi

# 4. 设置编译器
echo -e "${BLUE}步骤 3: 选择编译器${NC}"
CXX=""
if command -v clang++ &> /dev/null; then
    CXX="clang++"
    echo "使用: clang++"
elif command -v g++ &> /dev/null; then
    CXX="g++"
    echo "使用: g++"
else
    echo -e "${RED}✗ 未找到 C++ 编译器${NC}"
    exit 1
fi

# 检查 C++20 支持
echo "检查 C++20 支持..."
$CXX --version | head -1
echo ""

# 5. 编译测试
echo -e "${BLUE}步骤 4: 开始编译${NC}"
echo ""

CXXFLAGS="-std=c++20 -I include -I $GMP_INCLUDE"
LDFLAGS="-L $GMP_LIB -lgmp"

# 编译 integer.cpp
echo "编译 integer.cpp..."
if $CXX $CXXFLAGS -c src/core/integer.cpp -o /tmp/integer.o 2>&1; then
    echo -e "${GREEN}✓ integer.cpp 编译成功${NC}"
else
    echo -e "${RED}✗ integer.cpp 编译失败${NC}"
    echo ""
    echo "详细错误:"
    $CXX $CXXFLAGS -c src/core/integer.cpp -o /tmp/integer.o 2>&1 | tail -20
    exit 1
fi
echo ""

# 编译 polynomial.cpp
echo "编译 polynomial.cpp..."
if $CXX $CXXFLAGS -c src/core/polynomial.cpp -o /tmp/polynomial.o 2>&1; then
    echo -e "${GREEN}✓ polynomial.cpp 编译成功${NC}"
else
    echo -e "${RED}✗ polynomial.cpp 编译失败${NC}"
    echo ""
    echo "详细错误:"
    $CXX $CXXFLAGS -c src/core/polynomial.cpp -o /tmp/polynomial.o 2>&1 | tail -20
    exit 1
fi
echo ""

# 编译 relation.cpp
echo "编译 relation.cpp..."
if $CXX $CXXFLAGS -c src/core/relation.cpp -o /tmp/relation.o 2>&1; then
    echo -e "${GREEN}✓ relation.cpp 编译成功${NC}"
else
    echo -e "${RED}✗ relation.cpp 编译失败${NC}"
    echo ""
    echo "详细错误:"
    $CXX $CXXFLAGS -c src/core/relation.cpp -o /tmp/relation.o 2>&1 | tail -20
    exit 1
fi
echo ""

# 编译 test_integer
echo "编译 test_integer..."
if $CXX $CXXFLAGS tests/test_integer.cpp /tmp/integer.o /tmp/polynomial.o /tmp/relation.o $LDFLAGS -o /tmp/test_integer 2>&1; then
    echo -e "${GREEN}✓ test_integer 编译成功${NC}"
else
    echo -e "${RED}✗ test_integer 编译失败${NC}"
    echo ""
    echo "详细错误:"
    $CXX $CXXFLAGS tests/test_integer.cpp /tmp/integer.o /tmp/polynomial.o /tmp/relation.o $LDFLAGS -o /tmp/test_integer 2>&1 | tail -20
    exit 1
fi
echo ""

# 6. 运行测试
echo "=========================================="
echo -e "${BLUE}步骤 5: 运行 test_integer${NC}"
echo "=========================================="
echo ""

if /tmp/test_integer; then
    echo ""
    echo "=========================================="
    echo -e "${GREEN}✓✓✓ 所有测试通过！ ✓✓✓${NC}"
    echo "=========================================="
else
    echo ""
    echo "=========================================="
    echo -e "${RED}✗ 测试失败${NC}"
    echo "=========================================="
    exit 1
fi

echo ""
echo "下一步:"
echo "  1. 测试其他模块"
echo "  2. 使用 CMake 完整编译"
echo "  3. 运行所有测试"
