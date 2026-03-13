#!/bin/bash
# quick_compile_test.sh - 快速编译测试 test_integer

echo "========================================"
echo "GNFS 快速编译测试 - test_integer"
echo "========================================"
echo ""

# 检查 GMP
if ! pkg-config --exists gmp 2>/dev/null && ! [ -f /opt/homebrew/lib/libgmp.dylib ]; then
    echo "错误: GMP 未找到"
    echo "安装命令:"
    echo "  macOS:  brew install gmp"
    echo "  Ubuntu: sudo apt install libgmp-dev"
    exit 1
fi

# 设置变量
CXX=${CXX:-g++}
if command -v clang++ >/dev/null 2>&1; then
    CXX=clang++
fi

echo "使用编译器: $CXX"
echo ""

# 编译 integer.cpp
echo "编译 integer.cpp..."
$CXX -std=c++20 -I include -c src/core/integer.cpp -o /tmp/integer.o 2>&1 | head -20
if [ ${PIPESTATUS[0]} -ne 0 ]; then
    echo "✗ integer.cpp 编译失败"
    exit 1
fi
echo "✓ integer.cpp 编译成功"
echo ""

# 编译 polynomial.cpp
echo "编译 polynomial.cpp..."
$CXX -std=c++20 -I include -c src/core/polynomial.cpp -o /tmp/polynomial.o 2>&1 | head -20
if [ ${PIPESTATUS[0]} -ne 0 ]; then
    echo "✗ polynomial.cpp 编译失败"
    exit 1
fi
echo "✓ polynomial.cpp 编译成功"
echo ""

# 编译 relation.cpp
echo "编译 relation.cpp..."
$CXX -std=c++20 -I include -c src/core/relation.cpp -o /tmp/relation.o 2>&1 | head -20
if [ ${PIPESTATUS[0]} -ne 0 ]; then
    echo "✗ relation.cpp 编译失败"
    exit 1
fi
echo "✓ relation.cpp 编译成功"
echo ""

# 编译 test_integer
echo "编译 test_integer..."
$CXX -std=c++20 -I include tests/test_integer.cpp /tmp/integer.o /tmp/polynomial.o /tmp/relation.o -lgmp -o /tmp/test_integer 2>&1 | head -20
if [ ${PIPESTATUS[0]} -ne 0 ]; then
    echo "✗ test_integer 链接失败"
    exit 1
fi
echo "✓ test_integer 编译成功"
echo ""

# 运行测试
echo "========================================"
echo "运行 test_integer..."
echo "========================================"
echo ""
/tmp/test_integer

echo ""
echo "========================================"
echo "✓ 测试完成！"
echo "========================================"
