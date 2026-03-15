---
name: gnfs-status
description: 显示 GNFS 项目当前状态：构建状态、测试结果、最近修改
disable-model-invocation: true
---

# GNFS Project Status

快速检查项目健康状态。

## Steps

1. **构建状态** — 尝试编译，报告是否成功
```bash
make -C /Users/admin/Desktop/Documents/GNFS/build -j$(sysctl -n hw.ncpu) 2>&1 | tail -5
```

2. **测试状态** — 运行全部测试并汇总
```bash
cd /Users/admin/Desktop/Documents/GNFS/build && ctest --output-on-failure 2>&1 | tail -20
```

3. **最近修改** — 列出最近修改的源文件
```bash
find /Users/admin/Desktop/Documents/GNFS/include /Users/admin/Desktop/Documents/GNFS/src /Users/admin/Desktop/Documents/GNFS/tests -name "*.hpp" -o -name "*.cpp" | xargs ls -lt | head -10
```

4. **代码统计** — 报告文件数和行数
```bash
find /Users/admin/Desktop/Documents/GNFS/include /Users/admin/Desktop/Documents/GNFS/src -name "*.hpp" -o -name "*.cpp" | xargs wc -l | tail -1
```

## Output Format

汇总为简洁的状态报告：
- 🔨 Build: ✅ Success / ❌ Failed (error summary)
- 🧪 Tests: X/Y passed
- 📝 Recent changes: top 5 modified files
- 📊 Codebase: N files, M lines
