#!/bin/bash
# fix_all_integer_zero.sh - 修复所有 Integer(0) 的歧义问题

echo "=========================================="
echo "修复所有文件中的 Integer(0) 歧义"
echo "=========================================="
echo ""

# 需要修复的文件列表
FILES=(
    "tests/test_integer.cpp"
    "src/core/polynomial.cpp"
    "tests/test_factor_base.cpp"
    "tests/test_linalg.cpp"
    "tests/test_sqrt.cpp"
)

FIXED_COUNT=0

for file in "${FILES[@]}"; do
    if [ -f "$file" ]; then
        echo "检查: $file"
        
        # 备份
        cp "$file" "${file}.backup" 2>/dev/null
        
        # 应用修复
        if grep -q "Integer(0)" "$file"; then
            sed -i.tmp 's/Integer(0)/Integer(static_cast<int64_t>(0))/g' "$file"
            rm -f "${file}.tmp"
            echo "  ✓ 已修复"
            ((FIXED_COUNT++))
        else
            echo "  - 无需修复"
        fi
    else
        echo "跳过: $file (文件不存在)"
    fi
done

echo ""
echo "=========================================="
echo "修复完成！"
echo "修复了 $FIXED_COUNT 个文件"
echo "=========================================="
echo ""
echo "现在运行: bash fix_gmp_and_compile.sh"
