#!/bin/bash
# minimal_build.sh - 只编译已测试通过的基础模块

echo "=========================================="
echo "GNFS 最小化构建（只构建已通过测试的模块）"
echo "=========================================="
echo ""

cd build 2>/dev/null || { echo "错误: build 目录不存在"; exit 1; }

echo "只编译基础测试..."
echo ""

# 清理之前的失败构建
make clean 2>/dev/null

# 只编译已经通过的测试
echo "编译 test_small_vector..."
make test_small_vector -j12
echo ""

echo "编译 test_thread_pool..."
make test_thread_pool -j12
echo ""

echo "编译 Integer 相关..."
make test_integer -j12 2>&1 | head -50
echo ""

echo "=========================================="
echo "运行测试"
echo "=========================================="
echo ""

if [ -f "test_small_vector" ]; then
    echo "运行 test_small_vector..."
    ./test_small_vector && echo "✓ PASS" || echo "✗ FAIL"
    echo ""
fi

if [ -f "test_thread_pool" ]; then
    echo "运行 test_thread_pool..."
    ./test_thread_pool && echo "✓ PASS" || echo "✗ FAIL"
    echo ""
fi

if [ -f "test_integer" ]; then
    echo "运行 test_integer..."
    ./test_integer && echo "✓ PASS" || echo "✗ FAIL"
    echo ""
fi

echo "=========================================="
echo "完成！"
echo "=========================================="
