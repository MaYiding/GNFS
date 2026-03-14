#!/bin/bash
# quick_full_build.sh - 快速完整构建

echo "=========================================="
echo "GNFS 快速完整构建"
echo "=========================================="
echo ""

# 步骤 1: 组织文件
echo "步骤 1: 组织现有文件..."
if [ -f "organize_files.sh" ]; then
    bash organize_files.sh
else
    echo "⚠ organize_files.sh 不存在，跳过"
fi
echo ""

# 步骤 2: 创建缺失文件
echo "步骤 2: 创建缺失文件..."
bash create_missing_files.sh
echo ""

# 步骤 3: CMake 构建
echo "步骤 3: CMake 构建..."
bash full_cmake_build.sh
