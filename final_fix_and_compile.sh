#!/bin/bash
# final_fix_and_compile.sh - 最终修复并编译

echo "=========================================="
echo "GNFS 最终修复和编译"
echo "=========================================="
echo ""

# 颜色
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

# 步骤 1: 修复所有 Integer(0) 歧义
echo -e "${BLUE}步骤 1: 修复所有 Integer(0) 歧义${NC}"

FILES_TO_FIX=(
    "src/core/polynomial.cpp"
    "tests/test_integer.cpp"
)

for file in "${FILES_TO_FIX[@]}"; do
    if [ -f "$file" ]; then
        # 备份
        cp "$file" "${file}.backup" 2>/dev/null
        
        # 修复
        sed -i.tmp 's/Integer(0)/Integer(static_cast<int64_t>(0))/g' "$file"
        sed -i.tmp 's/Integer(1)/Integer(static_cast<int64_t>(1))/g' "$file"
        rm -f "${file}.tmp"
        
        echo "  ✓ $file"
    fi
done

echo ""

# 步骤 2: 编译和测试
echo -e "${BLUE}步骤 2: 编译和测试${NC}"
echo ""

# 查找 GMP
GMP_INCLUDE=""
GMP_LIB=""

if [ -f "/opt/homebrew/include/gmp.h" ]; then
    GMP_INCLUDE="-I/opt/homebrew/include"
    GMP_LIB="-L/opt/homebrew/lib"
elif [ -f "/usr/local/include/gmp.h" ]; then
    GMP_INCLUDE="-I/usr/local/include"
    GMP_LIB="-L/usr/local/lib"
fi

# 设置编译器
CXX=clang++
if ! command -v clang++ &> /dev/null; then
    CXX=g++
fi

# 编译参数
CXXFLAGS="-std=c++20 -I include $GMP_INCLUDE"
LDFLAGS="$GMP_LIB -lgmp"

# 编译
echo "编译 integer.cpp..."
$CXX $CXXFLAGS -c src/core/integer.cpp -o /tmp/integer.o
[ $? -eq 0 ] && echo "✓" || exit 1

echo "编译 polynomial.cpp..."
$CXX $CXXFLAGS -c src/core/polynomial.cpp -o /tmp/polynomial.o
[ $? -eq 0 ] && echo "✓" || exit 1

echo "编译 relation.cpp..."
$CXX $CXXFLAGS -c src/core/relation.cpp -o /tmp/relation.o
[ $? -eq 0 ] && echo "✓" || exit 1

echo "链接 test_integer..."
$CXX $CXXFLAGS tests/test_integer.cpp /tmp/integer.o /tmp/polynomial.o /tmp/relation.o $LDFLAGS -o /tmp/test_integer
[ $? -eq 0 ] && echo "✓" || exit 1

echo ""
echo "=========================================="
echo "运行 test_integer"
echo "=========================================="
echo ""

/tmp/test_integer

if [ $? -eq 0 ]; then
    echo ""
    echo "=========================================="
    echo -e "${GREEN}✓✓✓ 所有测试通过！ ✓✓✓${NC}"
    echo "=========================================="
    echo ""
    echo "恭喜！test_integer 成功通过！"
    echo ""
    echo "下一步："
    echo "  1. 测试其他模块"
    echo "  2. 使用 CMake 完整编译: bash full_cmake_build.sh"
    echo "  3. 运行所有测试: ctest"
else
    echo ""
    echo "测试失败，请查看上面的错误信息"
    exit 1
fi
