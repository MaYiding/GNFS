#!/bin/bash
# fix_compilation_errors.sh - 修复编译错误

echo "=========================================="
echo "修复编译错误"
echo "=========================================="
echo ""

# 修复 1: 添加 to_uint64() 到 Integer 类
echo "修复 1: 更新 Integer 类..."

# 在 integer.hpp 中添加 to_uint64()
if grep -q "to_uint64" include/gnfs/core/integer.hpp; then
    echo "  to_uint64() 已存在"
else
    # 在 to_int64() 后面添加 to_uint64()
    sed -i.bak '/int64_t to_int64/a\
    uint64_t to_uint64() const;
' include/gnfs/core/integer.hpp
    echo "  ✓ 添加了 to_uint64() 声明"
fi

# 在 integer.cpp 中添加实现
if grep -q "to_uint64" src/core/integer.cpp; then
    echo "  to_uint64() 实现已存在"
else
    # 在 to_int64() 实现后面添加
    cat >> src/core/integer.cpp << 'EOF'

uint64_t Integer::to_uint64() const {
    if (!mpz_fits_ulong_p(value_)) {
        throw std::overflow_error("Integer does not fit in uint64_t");
    }
    return mpz_get_ui(value_);
}
EOF
    echo "  ✓ 添加了 to_uint64() 实现"
fi

echo ""
echo "修复 2: 检查问题头文件..."

# 检查是否有问题的头文件
PROBLEM_FILES=(
    "include/gnfs/sieve/lattice_sieve.hpp"
    "include/gnfs/sqrt/rational_sqrt.hpp"
    "include/gnfs/linalg/matrix_builder.hpp"
    "include/gnfs/linalg/block_lanczos.hpp"
)

for file in "${PROBLEM_FILES[@]}"; do
    if [ -f "$file" ]; then
        echo "  发现: $file"
        # 备份
        cp "$file" "${file}.bak"
        
        # 修复字段名: ab.a -> a, ab.b -> b
        sed -i.tmp 's/rel\.ab\.a/rel.a/g' "$file"
        sed -i.tmp 's/rel\.ab\.b/rel.b/g' "$file"
        
        # 修复字段名: rat_factors -> rational_factors
        sed -i.tmp 's/rat_factors/rational_factors/g' "$file"
        
        # 修复字段名: alg_factors -> algebraic_factors
        sed -i.tmp 's/alg_factors/algebraic_factors/g' "$file"
        
        rm -f "${file}.tmp"
        echo "  ✓ 修复了 $file"
    fi
done

echo ""
echo "=========================================="
echo "修复完成！"
echo "=========================================="
echo ""
echo "现在重新编译: bash full_cmake_build.sh"
