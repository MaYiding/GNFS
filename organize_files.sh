#!/bin/bash
# organize_files.sh - 组织 GNFS 项目文件

set -e

echo "=========================================="
echo "GNFS 项目文件组织脚本"
echo "=========================================="
echo ""

# 颜色定义
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 创建目录结构
echo -e "${BLUE}步骤 1: 创建目录结构${NC}"
mkdir -p include/gnfs/{core,polynomial,factor_base,sieve,cofactor,relation,linalg,sqrt,util}
mkdir -p src/{core,polynomial,factor_base,sieve,cofactor,relation,linalg,sqrt,util}
mkdir -p tests
echo -e "${GREEN}✓ 目录创建完成${NC}"
echo ""

# 文件映射函数
copy_if_exists() {
    local src=$1
    local dst=$2
    if [ -f "$src" ]; then
        cp "$src" "$dst"
        echo -e "${GREEN}✓${NC} $src → $dst"
        return 0
    else
        echo -e "${YELLOW}⚠${NC} $src 不存在"
        return 1
    fi
}

# 移动核心文件
echo -e "${BLUE}步骤 2: 组织核心文件${NC}"
copy_if_exists "gnfs_core_integer.hpp" "include/gnfs/core/integer.hpp"
copy_if_exists "gnfs_core_integer.cpp" "src/core/integer.cpp"
copy_if_exists "polynomial.hpp" "include/gnfs/core/polynomial.hpp"
copy_if_exists "gnfs_core_polynomial.cpp" "src/core/polynomial.cpp"
copy_if_exists "gnfs_core_relation.hpp" "include/gnfs/core/relation.hpp"
copy_if_exists "gnfs_core_relation.cpp" "src/core/relation.cpp"
echo ""

# 移动多项式文件
echo -e "${BLUE}步骤 3: 组织多项式文件${NC}"
copy_if_exists "base_m.hpp" "include/gnfs/polynomial/base_m.hpp"
copy_if_exists "base_m.cpp" "src/polynomial/base_m.cpp"
echo ""

# 移动因子基文件
echo -e "${BLUE}步骤 4: 组织因子基文件${NC}"
copy_if_exists "factor_base_builder.hpp" "include/gnfs/factor_base/builder.hpp"
copy_if_exists "factor_base_builder.cpp" "src/factor_base/builder.cpp"
echo ""

# 移动筛法文件
echo -e "${BLUE}步骤 5: 组织筛法文件${NC}"
copy_if_exists "lattice_sieve.cpp" "src/sieve/lattice_sieve.cpp"
echo ""

# 移动线性代数文件
echo -e "${BLUE}步骤 6: 组织线性代数文件${NC}"
copy_if_exists "block_lanczos.hpp" "include/gnfs/linalg/block_lanczos.hpp"
copy_if_exists "block_lanczos.cpp" "src/linalg/block_lanczos.cpp"
copy_if_exists "matrix_builder.cpp" "src/linalg/matrix_builder.cpp"
echo ""

# 移动平方根文件
echo -e "${BLUE}步骤 7: 组织平方根文件${NC}"
copy_if_exists "sqrt_rational.cpp" "src/sqrt/rational_sqrt.cpp"
copy_if_exists "sqrt_algebraic.cpp" "src/sqrt/algebraic_sqrt.cpp"
echo ""

# 检查文件完整性
echo -e "${BLUE}步骤 8: 检查文件完整性${NC}"
echo ""

check_files() {
    local missing=0
    echo "检查必需的头文件..."
    local headers=(
        "include/gnfs/core/integer.hpp"
        "include/gnfs/core/polynomial.hpp"
        "include/gnfs/core/relation.hpp"
    )
    for file in "${headers[@]}"; do
        if [ -f "$file" ]; then
            echo -e "  ${GREEN}✓${NC} $file"
        else
            echo -e "  ${YELLOW}✗${NC} $file (缺失)"
            ((missing++))
        fi
    done
    
    echo ""
    echo "检查必需的源文件..."
    local sources=(
        "src/core/integer.cpp"
        "src/core/polynomial.cpp"
        "src/core/relation.cpp"
    )
    for file in "${sources[@]}"; do
        if [ -f "$file" ]; then
            echo -e "  ${GREEN}✓${NC} $file"
        else
            echo -e "  ${YELLOW}✗${NC} $file (缺失)"
            ((missing++))
        fi
    done
    
    return $missing
}

if check_files; then
    echo ""
    echo -e "${GREEN}✓ 所有必需文件都已就位！${NC}"
else
    echo ""
    echo -e "${YELLOW}⚠ 有 $? 个文件缺失${NC}"
fi

echo ""
echo "=========================================="
echo "文件组织完成！"
echo "=========================================="
echo ""
echo "下一步："
echo "  1. cd build (或 mkdir build && cd build)"
echo "  2. cmake .."
echo "  3. make -j8"
echo "  4. ./test_integer"
echo ""
echo "或运行快速测试："
echo "  bash quick_compile_test.sh"
