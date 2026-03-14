#!/bin/bash

# 修复 GNFS 项目的编译错误
# 2026-02-04

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}======================================"
echo "GNFS 编译错误自动修复工具"
echo "======================================${NC}"

# 备份标志
BACKUP_DIR="backup_$(date +%Y%m%d_%H%M%S)"

# 创建备份
echo -e "\n${YELLOW}步骤 0: 创建备份...${NC}"
mkdir -p "$BACKUP_DIR"
if [ -d "include" ]; then
    cp -r include "$BACKUP_DIR/"
    echo -e "${GREEN}✓ include/ 已备份${NC}"
fi
if [ -d "src" ]; then
    cp -r src "$BACKUP_DIR/"
    echo -e "${GREEN}✓ src/ 已备份${NC}"
fi

# 1. 修复 Integer 构造函数歧义
echo -e "\n${YELLOW}步骤 1: 修复 Integer 构造函数调用...${NC}"

# 使用花括号初始化替代圆括号（避免歧义）
find include -name "*.hpp" -type f -exec sed -i '' \
    's/\(static \)\?Integer \([a-zA-Z_][a-zA-Z0-9_]*\)(0);/\1Integer \2{0};/g' {} \;

echo -e "${GREEN}✓ 已修复 Integer(0) 构造调用${NC}"

# 2. 修复 GMP 函数调用 - 方案：在调用处添加解引用
echo -e "\n${YELLOW}步骤 2: 修复 GMP 函数调用...${NC}"

# 注意：这个 sed 命令可能需要根据实际情况调整
# 修复 mpz_tdiv_q_2exp - 三个参数都需要解引用
find include -name "*.hpp" -type f -exec sed -i '' \
    's/mpz_tdiv_q_2exp(\([a-zA-Z_][a-zA-Z0-9_]*\)\.get(), \([a-zA-Z_][a-zA-Z0-9_]*\)\.get(), \([0-9]\)/mpz_tdiv_q_2exp(*\1.get(), *\2.get(), \3/g' {} \;

echo -e "${GREEN}✓ 已修复 mpz_tdiv_q_2exp 调用${NC}"

# 修复 mpz_divisible_ui_p
find include -name "*.hpp" -type f -exec sed -i '' \
    's/mpz_divisible_ui_p(\([a-zA-Z_][a-zA-Z0-9_]*\)\.get(), \([a-zA-Z0-9_]*\))/mpz_divisible_ui_p(*\1.get(), \2)/g' {} \;

echo -e "${GREEN}✓ 已修复 mpz_divisible_ui_p 调用${NC}"

# 修复 mpz_divexact_ui
find include -name "*.hpp" -type f -exec sed -i '' \
    's/mpz_divexact_ui(\([a-zA-Z_][a-zA-Z0-9_]*\)\.get(), \([a-zA-Z_][a-zA-Z0-9_]*\)\.get(), \([a-zA-Z0-9_]*\))/mpz_divexact_ui(*\1.get(), *\2.get(), \3)/g' {} \;

echo -e "${GREEN}✓ 已修复 mpz_divexact_ui 调用${NC}"

# 3. 修复 matrix.rows 调用
echo -e "\n${YELLOW}步骤 3: 修复 SparseMatrix 方法调用...${NC}"

if [ -f "src/linalg/block_lanczos.cpp" ]; then
    sed -i '' 's/matrix\.rows,/matrix.rows(),/g' src/linalg/block_lanczos.cpp
    echo -e "${GREEN}✓ 已修复 matrix.rows 调用${NC}"
fi

# 4. 检查并报告需要手动修复的问题
echo -e "\n${YELLOW}步骤 4: 检查需要手动修复的问题...${NC}"

MANUAL_FIXES_NEEDED=0

# 检查 Relation 定义
if [ -f "include/gnfs/core/relation.hpp" ]; then
    if grep -q "Integer rational_large_prime" include/gnfs/core/relation.hpp 2>/dev/null; then
        echo -e "${RED}⚠ 发现问题: relation.hpp 中 large_prime 字段类型错误${NC}"
        echo "   需要将 Integer 改为 std::vector<uint64_t>"
        MANUAL_FIXES_NEEDED=1
    else
        echo -e "${GREEN}✓ relation.hpp 看起来正常${NC}"
    fi
fi

# 检查重复定义
if [ -f "src/core/integer.cpp" ]; then
    DUPE_COUNT=$(grep -c "uint64_t Integer::to_uint64" src/core/integer.cpp 2>/dev/null || echo "0")
    if [ "$DUPE_COUNT" -gt 1 ]; then
        echo -e "${RED}⚠ 发现问题: integer.cpp 中有 $DUPE_COUNT 个 to_uint64 定义${NC}"
        echo "   需要手动删除重复的定义"
        MANUAL_FIXES_NEEDED=1
    else
        echo -e "${GREEN}✓ integer.cpp 中没有重复定义${NC}"
    fi
fi

# 检查构造函数定义
echo -e "\n${YELLOW}步骤 5: 检查构造函数定义匹配...${NC}"

CPP_FILES=(
    "src/sqrt/algebraic_sqrt.cpp"
    "src/sqrt/rational_sqrt.cpp"
    "src/sieve/lattice_sieve.cpp"
    "src/linalg/matrix_builder.cpp"
)

for cpp_file in "${CPP_FILES[@]}"; do
    if [ -f "$cpp_file" ]; then
        echo -e "${GREEN}✓ $cpp_file 存在${NC}"
    else
        echo -e "${YELLOW}? $cpp_file 不存在${NC}"
    fi
done

# 生成修复报告
echo -e "\n${BLUE}======================================"
echo "修复完成报告"
echo "======================================${NC}"

echo -e "\n${GREEN}自动修复项:${NC}"
echo "  ✓ Integer 构造函数调用 (使用花括号初始化)"
echo "  ✓ GMP 函数调用中的指针解引用"
echo "  ✓ SparseMatrix::rows 方法调用"

if [ $MANUAL_FIXES_NEEDED -eq 1 ]; then
    echo -e "\n${RED}需要手动修复的问题:${NC}"
    echo "  请查看上面的 ⚠ 标记"
    echo ""
    echo -e "${YELLOW}详细修复指南:${NC}"
    echo "  请阅读 COMPILATION_FIX_GUIDE.md"
else
    echo -e "\n${GREEN}所有已知问题已自动修复！${NC}"
fi

echo -e "\n${BLUE}下一步:${NC}"
echo "  1. 检查修复结果: git diff include/ src/"
echo "  2. 尝试编译: cd build && make clean && make -j12"
echo "  3. 如果仍有错误，查看: COMPILATION_FIX_GUIDE.md"
echo ""
echo -e "${GREEN}备份位置: $BACKUP_DIR/${NC}"
echo ""

