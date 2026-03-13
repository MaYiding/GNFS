#!/bin/bash
# test_more_modules.sh - 测试更多模块

echo "=========================================="
echo "GNFS 扩展测试 - 测试更多模块"
echo "=========================================="
echo ""

# 颜色
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# GMP 设置
GMP_INCLUDE="-I/opt/homebrew/include"
GMP_LIB="-L/opt/homebrew/lib"
if [ ! -f "/opt/homebrew/include/gmp.h" ]; then
    GMP_INCLUDE="-I/usr/local/include"
    GMP_LIB="-L/usr/local/lib"
fi

CXX=clang++
CXXFLAGS="-std=c++20 -I include $GMP_INCLUDE"
LDFLAGS="$GMP_LIB -lgmp -pthread"

# 测试计数
TOTAL=0
PASSED=0
FAILED=0

# 测试函数
test_module() {
    local name=$1
    local sources=$2
    local test_file=$3
    
    echo -e "${BLUE}测试: $name${NC}"
    ((TOTAL++))
    
    # 编译
    if $CXX $CXXFLAGS $sources tests/$test_file $LDFLAGS -o /tmp/$name 2>/dev/null; then
        echo -n "  编译: ✓  "
        
        # 运行
        if /tmp/$name > /tmp/${name}_output.txt 2>&1; then
            echo -e "${GREEN}运行: ✓ PASS${NC}"
            ((PASSED++))
            return 0
        else
            echo -e "${RED}运行: ✗ FAIL${NC}"
            echo "    输出:"
            head -20 /tmp/${name}_output.txt | sed 's/^/    /'
            ((FAILED++))
            return 1
        fi
    else
        echo -e "${RED}  编译: ✗ FAIL${NC}"
        echo "    错误:"
        $CXX $CXXFLAGS $sources tests/$test_file $LDFLAGS -o /tmp/$name 2>&1 | head -10 | sed 's/^/    /'
        ((FAILED++))
        return 1
    fi
}

# 编译基础对象文件
echo -e "${BLUE}步骤 1: 编译基础模块${NC}"
echo ""

echo "编译 integer.o..."
$CXX $CXXFLAGS -c src/core/integer.cpp -o /tmp/integer.o
echo "✓"

echo "编译 polynomial.o..."
$CXX $CXXFLAGS -c src/core/polynomial.cpp -o /tmp/polynomial.o
echo "✓"

echo "编译 relation.o..."
$CXX $CXXFLAGS -c src/core/relation.cpp -o /tmp/relation.o
echo "✓"

BASE_OBJS="/tmp/integer.o /tmp/polynomial.o /tmp/relation.o"

echo ""
echo -e "${BLUE}步骤 2: 运行测试${NC}"
echo ""

# 测试 1: test_integer (已经通过，但再确认一次)
test_module "test_integer" "$BASE_OBJS" "test_integer.cpp"
echo ""

# 测试 2: test_small_vector (不需要 GMP)
echo -e "${BLUE}测试: test_small_vector${NC}"
((TOTAL++))
if $CXX -std=c++20 -I include tests/test_small_vector.cpp -o /tmp/test_small_vector 2>/dev/null; then
    echo -n "  编译: ✓  "
    if /tmp/test_small_vector > /tmp/test_small_vector_output.txt 2>&1; then
        echo -e "${GREEN}运行: ✓ PASS${NC}"
        ((PASSED++))
    else
        echo -e "${RED}运行: ✗ FAIL${NC}"
        ((FAILED++))
    fi
else
    echo -e "${RED}  编译: ✗ FAIL${NC}"
    ((FAILED++))
fi
echo ""

# 测试 3: test_thread_pool
echo -e "${BLUE}测试: test_thread_pool${NC}"
((TOTAL++))
if [ -f "src/util/thread_pool.cpp" ]; then
    $CXX $CXXFLAGS -c src/util/thread_pool.cpp -o /tmp/thread_pool.o 2>/dev/null
    if $CXX -std=c++20 -I include tests/test_thread_pool.cpp /tmp/thread_pool.o -pthread -o /tmp/test_thread_pool 2>/dev/null; then
        echo -n "  编译: ✓  "
        if /tmp/test_thread_pool > /tmp/test_thread_pool_output.txt 2>&1; then
            echo -e "${GREEN}运行: ✓ PASS${NC}"
            ((PASSED++))
        else
            echo -e "${RED}运行: ✗ FAIL${NC}"
            ((FAILED++))
        fi
    else
        echo -e "${RED}  编译: ✗ FAIL${NC}"
        ((FAILED++))
    fi
else
    echo -e "${YELLOW}  跳过: 源文件不存在${NC}"
fi
echo ""

# 总结
echo "=========================================="
echo "测试总结"
echo "=========================================="
echo -e "总测试数: $TOTAL"
echo -e "${GREEN}通过: $PASSED${NC}"
if [ $FAILED -gt 0 ]; then
    echo -e "${RED}失败: $FAILED${NC}"
else
    echo -e "失败: $FAILED"
fi
echo ""

# 计算通过率
if [ $TOTAL -gt 0 ]; then
    PASS_RATE=$((PASSED * 100 / TOTAL))
    echo "通过率: ${PASS_RATE}%"
    echo ""
fi

if [ $PASSED -eq $TOTAL ]; then
    echo -e "${GREEN}🎉 所有测试通过！🎉${NC}"
    echo ""
    echo "下一步："
    echo "  1. 使用 CMake 完整编译: bash full_cmake_build.sh"
    echo "  2. 测试更复杂的模块: bash test_advanced_modules.sh"
else
    echo -e "${YELLOW}部分测试通过${NC}"
    echo ""
    echo "建议："
    echo "  1. 查看失败的测试详细输出"
    echo "  2. 修复失败的模块"
    echo "  3. 重新运行测试"
fi
