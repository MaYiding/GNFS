#!/bin/bash

# 快速诊断 GNFS 编译问题
# 2026-02-04

echo "======================================"
echo "GNFS 编译问题诊断工具"
echo "======================================"
echo ""

# 1. 检查关键文件是否存在
echo "1. 检查关键文件..."
FILES_TO_CHECK=(
    "include/gnfs/core/integer.hpp"
    "include/gnfs/core/relation.hpp"
    "include/gnfs/core/polynomial_context.hpp"
    "src/core/integer.cpp"
    "src/sqrt/rational_sqrt.cpp"
    "src/sqrt/algebraic_sqrt.cpp"
    "src/sieve/lattice_sieve.cpp"
    "src/linalg/matrix_builder.cpp"
)

for file in "${FILES_TO_CHECK[@]}"; do
    if [ -f "$file" ]; then
        echo "  ✓ $file"
    else
        echo "  ✗ $file (缺失)"
    fi
done

# 2. 检查 Integer 构造函数问题
echo ""
echo "2. 检查 Integer(0) 调用（可能导致歧义）..."
AMBIG_COUNT=$(grep -r "Integer [a-zA-Z_][a-zA-Z0-9_]*(0)" include/ --include="*.hpp" 2>/dev/null | wc -l | tr -d ' ')
if [ "$AMBIG_COUNT" -gt 0 ]; then
    echo "  ⚠ 发现 $AMBIG_COUNT 处潜在的歧义构造"
    echo "    示例:"
    grep -r "Integer [a-zA-Z_][a-zA-Z0-9_]*(0)" include/ --include="*.hpp" 2>/dev/null | head -3
else
    echo "  ✓ 未发现 Integer(0) 形式的调用"
fi

# 3. 检查 GMP 函数调用
echo ""
echo "3. 检查 GMP 函数调用（指针类型问题）..."

# mpz_tdiv_q_2exp
TDIV_COUNT=$(grep -r "mpz_tdiv_q_2exp([^*]" include/ --include="*.hpp" 2>/dev/null | grep -v "mpz_tdiv_q_2exp(\*" | wc -l | tr -d ' ')
if [ "$TDIV_COUNT" -gt 0 ]; then
    echo "  ⚠ 发现 $TDIV_COUNT 处 mpz_tdiv_q_2exp 调用可能有问题"
else
    echo "  ✓ mpz_tdiv_q_2exp 调用看起来正常"
fi

# mpz_divisible_ui_p
DIVIS_COUNT=$(grep -r "mpz_divisible_ui_p([^*]" include/ --include="*.hpp" 2>/dev/null | grep -v "mpz_divisible_ui_p(\*" | wc -l | tr -d ' ')
if [ "$DIVIS_COUNT" -gt 0 ]; then
    echo "  ⚠ 发现 $DIVIS_COUNT 处 mpz_divisible_ui_p 调用可能有问题"
else
    echo "  ✓ mpz_divisible_ui_p 调用看起来正常"
fi

# 4. 检查 Relation 结构
echo ""
echo "4. 检查 Relation 结构定义..."
if [ -f "include/gnfs/core/relation.hpp" ]; then
    if grep -q "struct Relation" include/gnfs/core/relation.hpp; then
        echo "  ✓ 找到 Relation 结构定义"
        
        # 检查 large_prime 字段
        if grep -A 20 "struct Relation" include/gnfs/core/relation.hpp | grep -q "rational_large_prime"; then
            FIELD_TYPE=$(grep -A 20 "struct Relation" include/gnfs/core/relation.hpp | grep "rational_large_prime" | head -1)
            echo "  字段定义: $FIELD_TYPE"
            
            if echo "$FIELD_TYPE" | grep -q "std::vector"; then
                echo "  ✓ large_prime 字段是 vector 类型"
            elif echo "$FIELD_TYPE" | grep -q "Integer"; then
                echo "  ⚠ large_prime 字段是 Integer 类型（应该是 vector）"
            fi
        fi
    fi
else
    echo "  ✗ relation.hpp 不存在"
fi

# 5. 检查重复定义
echo ""
echo "5. 检查重复定义..."
if [ -f "src/core/integer.cpp" ]; then
    DUPE_COUNT=$(grep -c "Integer::to_uint64" src/core/integer.cpp 2>/dev/null || echo "0")
    if [ "$DUPE_COUNT" -gt 1 ]; then
        echo "  ⚠ integer.cpp 中有 $DUPE_COUNT 个 to_uint64 定义"
        grep -n "Integer::to_uint64" src/core/integer.cpp
    else
        echo "  ✓ 未发现重复的 to_uint64 定义"
    fi
fi

# 6. 总结
echo ""
echo "======================================"
echo "诊断总结"
echo "======================================"

if [ "$AMBIG_COUNT" -eq 0 ] && [ "$TDIV_COUNT" -eq 0 ] && [ "$DIVIS_COUNT" -eq 0 ]; then
    echo "✓ 未发现明显问题，可以尝试编译"
else
    echo "⚠ 发现以下问题需要修复:"
    [ "$AMBIG_COUNT" -gt 0 ] && echo "  - Integer 构造函数歧义 ($AMBIG_COUNT 处)"
    [ "$TDIV_COUNT" -gt 0 ] && echo "  - mpz_tdiv_q_2exp 调用 ($TDIV_COUNT 处)"
    [ "$DIVIS_COUNT" -gt 0 ] && echo "  - mpz_divisible_ui_p 调用 ($DIVIS_COUNT 处)"
    echo ""
    echo "运行修复脚本: bash fix_compilation_errors.sh"
fi

echo ""
