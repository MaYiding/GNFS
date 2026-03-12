#!/bin/bash
# 编译测试脚本

echo "======================================"
echo "GNFS 项目编译测试"
echo "======================================"
echo ""

# 设置颜色
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 检查必需的工具
echo "检查必需工具..."

if ! command -v cmake &> /dev/null; then
    echo -e "${RED}✗ CMake 未找到${NC}"
    exit 1
fi
echo -e "${GREEN}✓ CMake 已安装${NC}"

if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
    echo -e "${RED}✗ C++ 编译器未找到${NC}"
    exit 1
fi
echo -e "${GREEN}✓ C++ 编译器已安装${NC}"

# 检查 GMP
echo ""
echo "检查依赖库..."
if ldconfig -p 2>/dev/null | grep -q libgmp || [ -f /opt/homebrew/lib/libgmp.dylib ] || [ -f /usr/local/lib/libgmp.dylib ]; then
    echo -e "${GREEN}✓ GMP 已安装${NC}"
else
    echo -e "${RED}✗ GMP 未找到${NC}"
    echo "  安装命令:"
    echo "    macOS:  brew install gmp"
    echo "    Ubuntu: sudo apt install libgmp-dev"
    exit 1
fi

# 创建构建目录
echo ""
echo "准备构建目录..."
if [ -d "build" ]; then
    echo -e "${YELLOW}! 删除旧的 build 目录${NC}"
    rm -rf build
fi
mkdir build
cd build

# 配置项目
echo ""
echo "配置项目..."
if cmake .. -DCMAKE_BUILD_TYPE=Release > cmake_config.log 2>&1; then
    echo -e "${GREEN}✓ CMake 配置成功${NC}"
    cat cmake_config.log | grep "GNFS Configuration" -A 10
else
    echo -e "${RED}✗ CMake 配置失败${NC}"
    echo "错误日志:"
    cat cmake_config.log
    exit 1
fi

# 编译项目
echo ""
echo "编译项目..."
echo "这可能需要几分钟..."

if cmake --build . -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) > build.log 2>&1; then
    echo -e "${GREEN}✓ 编译成功${NC}"
else
    echo -e "${RED}✗ 编译失败${NC}"
    echo ""
    echo "错误日志（最后 50 行）:"
    tail -50 build.log
    exit 1
fi

# 列出生成的测试程序
echo ""
echo "生成的测试程序:"
ls -lh test_* 2>/dev/null | awk '{print "  " $9 " (" $5 ")"}'

echo ""
echo "======================================"
echo -e "${GREEN}编译完成！${NC}"
echo "======================================"
echo ""
echo "运行所有测试: ctest"
echo "运行单个测试: ./test_integer"
