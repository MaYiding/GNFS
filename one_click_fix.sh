#!/bin/bash
# one_click_fix_and_compile.sh - 一键修复并编译

echo "=========================================="
echo "GNFS 一键修复和编译"
echo "=========================================="
echo ""

# 步骤 1: 应用 polynomial.cpp 修复
echo "步骤 1: 修复 polynomial.cpp..."
if [ -f "src/core/polynomial.cpp" ]; then
    # 备份
    cp src/core/polynomial.cpp src/core/polynomial.cpp.backup 2>/dev/null
    
    # 应用修复
    sed -i.tmp 's/return Integer(0);/return Integer(static_cast<int64_t>(0));/g' src/core/polynomial.cpp
    sed -i.tmp 's/coeffs_.push_back(Integer(0));/coeffs_.push_back(Integer(static_cast<int64_t>(0)));/g' src/core/polynomial.cpp
    sed -i.tmp 's/static Integer zero;/static Integer zero(static_cast<int64_t>(0));/g' src/core/polynomial.cpp
    rm -f src/core/polynomial.cpp.tmp
    
    echo "✓ polynomial.cpp 已修复"
else
    echo "⚠ polynomial.cpp 不存在，跳过修复"
fi
echo ""

# 步骤 2: 运行编译
echo "步骤 2: 开始编译..."
echo ""

bash fix_gmp_and_compile.sh
