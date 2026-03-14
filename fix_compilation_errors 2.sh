#!/bin/bash

# 修复 GNFS 项目的编译错误
# 2026-02-04

set -e

echo "======================================"
echo "修复编译错误"
echo "======================================"

# 1. 修复 Integer 构造函数歧义
echo "步骤 1: 修复 Integer 构造函数..."

# 查找并修复所有 Integer(0) 调用
find include -name "*.hpp" -type f -exec sed -i '' 's/Integer zero(0);/Integer zero = Integer(int64_t(0));/g' {} \;
find include -name "*.hpp" -type f -exec sed -i '' 's/Integer result(0);/Integer result = Integer(int64_t(0));/g' {} \;
find include -name "*.hpp" -type f -exec sed -i '' 's/Integer coeff_i(0);/Integer coeff_i = Integer(int64_t(0));/g' {} \;

# 2. 修复 GMP 函数调用
echo "步骤 2: 修复 GMP 函数调用中的指针问题..."

# 修复 mpz_tdiv_q_2exp 调用
find include -name "*.hpp" -type f -exec sed -i '' 's/mpz_tdiv_q_2exp(\([^,]*\)\.get(), \([^,]*\)\.get(),/mpz_tdiv_q_2exp(*\1.get(), *\2.get(),/g' {} \;

# 修复 mpz_divisible_ui_p 调用
find include -name "*.hpp" -type f -exec sed -i '' 's/mpz_divisible_ui_p(\([^,]*\)\.get(),/mpz_divisible_ui_p(*\1.get(),/g' {} \;

# 修复 mpz_divexact_ui 调用
find include -name "*.hpp" -type f -exec sed -i '' 's/mpz_divexact_ui(\([^,]*\)\.get(), \([^,]*\)\.get(),/mpz_divexact_ui(*\1.get(), *\2.get(),/g' {} \;

echo "步骤 3: 查看需要手动修复的文件..."

# 列出需要检查 Relation 结构的文件
echo ""
echo "以下文件需要手动检查 Relation 结构体定义:"
echo "  - include/gnfs/core/relation.hpp"
echo ""

# 3. 创建临时修复文件
cat > /tmp/relation_fixes.txt << 'EOF'
需要修复的 Relation 字段:

在 relation.hpp 中，确保:
1. rational_large_prime 和 algebraic_large_prime 应该是 vector 类型
2. 使用 std::vector<PrimePowerPair> 或类似结构

示例:
struct PrimePowerPair {
    Integer p;  // 素数
    int e;      // 指数
};

struct Relation {
    // ... 其他字段 ...
    std::vector<PrimePowerPair> rational_large_prime;
    std::vector<PrimePowerPair> algebraic_large_prime;
};
EOF

echo "步骤 4: 修复重复定义..."

# 检查 integer.cpp 中的重复定义
if grep -q "uint64_t Integer::to_uint64" src/core/integer.cpp; then
    echo "检测到 integer.cpp 中可能有重复的 to_uint64 定义"
    echo "需要手动检查和修复 src/core/integer.cpp"
fi

echo ""
echo "======================================"
echo "自动修复完成！"
echo "======================================"
echo ""
echo "下一步:"
echo "1. 检查 include/gnfs/core/relation.hpp 中的字段定义"
echo "2. 检查 src/core/integer.cpp 中的重复定义"
echo "3. 运行: cd build && make clean && make -j12"
echo ""
echo "详细修复说明已保存到: /tmp/relation_fixes.txt"
cat /tmp/relation_fixes.txt

