# 🔴 编译问题诊断和解决方案

## 问题：GMP 头文件找不到

### 错误信息
```
fatal error: 'gmp.h' file not found
```

## ✅ 快速解决（3 步）

```bash
# 1. 安装 GMP
brew install gmp              # macOS
# 或
sudo apt install libgmp-dev   # Ubuntu

# 2. 给脚本执行权限
chmod +x compile_with_gmp.sh fix_gmp_and_compile.sh

# 3. 运行编译（二选一）
bash compile_with_gmp.sh        # 简单版
bash fix_gmp_and_compile.sh    # 详细版（推荐）
```

## 📋 脚本说明

我已创建两个自动修复脚本：

### 1. `compile_with_gmp.sh` - 简洁版
- 自动检测 GMP 位置
- 快速编译测试
- 适合快速验证

### 2. `fix_gmp_and_compile.sh` - 详细版
- 完整的系统诊断
- 详细的错误信息
- 自动查找多个可能位置
- 推荐用于故障排除

## 🔍 GMP 常见位置

### macOS
```
Homebrew (Apple Silicon): /opt/homebrew/
Homebrew (Intel):         /usr/local/
```

### Linux
```
系统路径: /usr/
```

## 📝 手动检查

```bash
# 查找 gmp.h
find /usr /opt/homebrew /usr/local -name "gmp.h" 2>/dev/null

# 检查是否安装
pkg-config --modversion gmp
```

## 🎯 预期结果

成功后应该看到：
```
✓ integer.cpp
✓ polynomial.cpp  
✓ relation.cpp
✓ test_integer

=== Integer Tests ===
...
All tests passed!

✓✓✓ 所有测试通过！ ✓✓✓
```

## 📞 下一步

成功后运行：
```bash
bash organize_files.sh  # 组织所有文件
# 然后可以尝试 CMake 完整编译
```
