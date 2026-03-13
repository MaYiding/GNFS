#!/bin/bash
# compile_with_gmp.sh - 带 GMP 路径检测的编译脚本

echo "========================================"
echo "GNFS 编译测试（自动检测 GMP）"
echo "========================================"
echo ""

# 查找 GMP
echo "查找 GMP 库..."
GMP_INCLUDE=""
GMP_LIB=""
GMP_FOUND=0

# macOS (Homebrew Apple Silicon)
if [ -f "/opt/homebrew/include/gmp.h" ]; then
    GMP_INCLUDE="-I/opt/homebrew/include"
    GMP_LIB="-L/opt/homebrew/lib"
    GMP_FOUND=1
    echo "✓ 找到 GMP: /opt/homebrew"
# macOS (Homebrew Intel)
elif [ -f "/usr/local/include/gmp.h" ]; then
    GMP_INCLUDE="-I/usr/local/include"
    GMP_LIB="-L/usr/local/lib"
    GMP_FOUND=1
    echo "✓ 找到 GMP: /usr/local"
# Linux 系统路径
elif [ -f "/usr/include/gmp.h" ]; then
    GMP_INCLUDE=""
    GMP_LIB=""
    GMP_FOUND=1
    echo "✓ 找到 GMP: 系统路径"
# 使用 pkg-config
elif command -v pkg-config >/dev/null 2>&1 && pkg-config --exists gmp 2>/dev/null; then
    GMP_INCLUDE=$(pkg-config --cflags gmp)
    GMP_LIB=$(pkg-config --libs gmp | sed 's/-lgmp//')
    GMP_FOUND=1
    echo "✓ 找到 GMP: pkg-config"
fi

if [ $GMP_FOUND -eq 0 ]; then
    echo "✗ GMP 未找到"
    echo ""
    echo "请安装 GMP:"
    echo "  macOS:  brew install gmp"
    echo "  Ubuntu: sudo apt install libgmp-dev"
    echo "  Fedora: sudo dnf install gmp-devel"
    exit 1
fi

# 设置编译器
CXX=${CXX:-g++}
if command -v clang++ >/dev/null 2>&1; then
    CXX=clang++
fi

echo "使用编译器: $CXX"
$CXX --version | head -1
echo ""

# 编译标志
CXXFLAGS="-std=c++20 -I include $GMP_INCLUDE"
LDFLAGS="$GMP_LIB -lgmp"

# 编译 integer.cpp
echo "编译 integer.cpp..."
$CXX $CXXFLAGS -c src/core/integer.cpp -o /tmp/integer.o
if [ $? -ne 0 ]; then
    echo "✗ integer.cpp 编译失败"
    exit 1
fi
echo "✓ integer.cpp"

# 编译 polynomial.cpp
echo "编译 polynomial.cpp..."
$CXX $CXXFLAGS -c src/core/polynomial.cpp -o /tmp/polynomial.o
if [ $? -ne 0 ]; then
    echo "✗ polynomial.cpp 编译失败"
    exit 1
fi
echo "✓ polynomial.cpp"

# 编译 relation.cpp
echo "编译 relation.cpp..."
$CXX $CXXFLAGS -c src/core/relation.cpp -o /tmp/relation.o
if [ $? -ne 0 ]; then
    echo "✗ relation.cpp 编译失败"
    exit 1
fi
echo "✓ relation.cpp"

# 链接 test_integer
echo ""
echo "链接 test_integer..."
$CXX $CXXFLAGS tests/test_integer.cpp /tmp/integer.o /tmp/polynomial.o /tmp/relation.o $LDFLAGS -o /tmp/test_integer
if [ $? -ne 0 ]; then
    echo "✗ test_integer 链接失败"
    exit 1
fi
echo "✓ test_integer"

# 运行测试
echo ""
echo "========================================"
echo "运行测试..."
echo "========================================"
echo ""
/tmp/test_integer

echo ""
echo "========================================"
if [ $? -eq 0 ]; then
    echo "✓✓✓ 所有测试通过！ ✓✓✓"
else
    echo "✗ 测试失败"
fi
echo "========================================"
