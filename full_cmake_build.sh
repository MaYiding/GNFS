#!/bin/bash
# full_cmake_build.sh - 完整的 CMake 构建

echo "=========================================="
echo "GNFS CMake 完整构建"
echo "=========================================="
echo ""

# 颜色
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# 步骤 1: 检查文件结构
echo -e "${BLUE}步骤 1: 检查项目结构${NC}"

MISSING_FILES=0

# 检查关键头文件
REQUIRED_HEADERS=(
    "include/gnfs/core/integer.hpp"
    "include/gnfs/core/polynomial.hpp"
    "include/gnfs/core/relation.hpp"
)

for header in "${REQUIRED_HEADERS[@]}"; do
    if [ ! -f "$header" ]; then
        echo -e "${RED}✗ 缺失: $header${NC}"
        ((MISSING_FILES++))
    else
        echo -e "${GREEN}✓${NC} $header"
    fi
done

if [ $MISSING_FILES -gt 0 ]; then
    echo ""
    echo -e "${YELLOW}警告: 缺失 $MISSING_FILES 个必需文件${NC}"
    echo "请先运行: bash organize_files.sh"
    echo ""
    read -p "是否继续？ (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

echo ""

# 步骤 2: 清理旧的构建
echo -e "${BLUE}步骤 2: 清理构建目录${NC}"
if [ -d "build" ]; then
    echo "删除旧的 build 目录..."
    rm -rf build
fi
mkdir -p build
echo "✓ 构建目录已准备"
echo ""

# 步骤 3: CMake 配置
echo -e "${BLUE}步骤 3: CMake 配置${NC}"
cd build

# 设置 GMP 路径（macOS Homebrew）
if [ -d "/opt/homebrew" ]; then
    export CMAKE_PREFIX_PATH="/opt/homebrew:$CMAKE_PREFIX_PATH"
    export CPATH="/opt/homebrew/include:$CPATH"
    export LIBRARY_PATH="/opt/homebrew/lib:$LIBRARY_PATH"
fi

echo "运行 cmake ..."
if cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > cmake_config.log 2>&1; then
    echo -e "${GREEN}✓ CMake 配置成功${NC}"
    echo ""
    echo "配置摘要:"
    grep -A 10 "GNFS Configuration Summary" cmake_config.log || tail -15 cmake_config.log
    echo ""
else
    echo -e "${RED}✗ CMake 配置失败${NC}"
    echo ""
    echo "错误日志:"
    cat cmake_config.log
    exit 1
fi

# 步骤 4: 编译
echo -e "${BLUE}步骤 4: 编译项目${NC}"
echo "这可能需要几分钟..."
echo ""

# 获取 CPU 核心数
if command -v nproc &> /dev/null; then
    JOBS=$(nproc)
elif command -v sysctl &> /dev/null; then
    JOBS=$(sysctl -n hw.ncpu)
else
    JOBS=4
fi

echo "使用 $JOBS 个并行任务"
echo ""

if cmake --build . -j$JOBS > build.log 2>&1; then
    echo -e "${GREEN}✓ 编译成功${NC}"
    echo ""
    
    # 显示生成的可执行文件
    echo "生成的测试程序:"
    ls -lh test_* 2>/dev/null | awk '{print "  " $9 " (" $5 ")"}'
    echo ""
else
    echo -e "${RED}✗ 编译失败${NC}"
    echo ""
    echo "错误日志（最后 50 行）:"
    tail -50 build.log
    echo ""
    echo "完整日志: build/build.log"
    exit 1
fi

# 步骤 5: 运行测试
echo -e "${BLUE}步骤 5: 运行测试${NC}"
echo ""

echo "运行 test_integer..."
if ./test_integer > /dev/null 2>&1; then
    echo -e "${GREEN}✓ test_integer PASS${NC}"
else
    echo -e "${RED}✗ test_integer FAIL${NC}"
fi

echo ""
echo "运行所有测试（ctest）..."
echo ""

if ctest --output-on-failure 2>&1 | tee ctest.log; then
    echo ""
    echo -e "${GREEN}✓ 所有测试完成${NC}"
else
    echo ""
    echo -e "${YELLOW}部分测试失败${NC}"
fi

# 测试摘要
echo ""
echo "=========================================="
echo "测试摘要"
echo "=========================================="

grep -E "(Test|Passed|Failed|tests passed|tests failed)" ctest.log | tail -20

echo ""
echo "=========================================="
echo "构建完成！"
echo "=========================================="
echo ""
echo "可执行文件位置: build/"
echo "测试日志: build/ctest.log"
echo "编译日志: build/build.log"
echo ""
echo "运行单个测试:"
echo "  cd build"
echo "  ./test_integer"
echo "  ./test_factor_base"
echo ""
echo "运行所有测试:"
echo "  cd build"
echo "  ctest --verbose"
