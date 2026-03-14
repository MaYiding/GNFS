#!/bin/bash
# quick_fix.sh - 快速修复所有编译错误

echo "快速修复编译错误..."

# 1. 在 Integer.hpp 添加 to_uint64
if ! grep -q "uint64_t to_uint64" include/gnfs/core/integer.hpp 2>/dev/null; then
    # 找到 to_int64 那一行，在其后添加 to_uint64
    awk '
    /int64_t to_int64\(\) const;/ {
        print
        print "    uint64_t to_uint64() const;"
        next
    }
    { print }
    ' include/gnfs/core/integer.hpp > /tmp/integer.hpp.new
    mv /tmp/integer.hpp.new include/gnfs/core/integer.hpp
    echo "✓ 添加 to_uint64() 到 integer.hpp"
fi

# 2. 在 Integer.cpp 添加实现
if ! grep -q "Integer::to_uint64" src/core/integer.cpp 2>/dev/null; then
    # 在文件末尾添加（在 namespace 结束前）
    awk '
    /^} \/\/ namespace gnfs::core/ {
        print ""
        print "uint64_t Integer::to_uint64() const {"
        print "    if (!mpz_fits_ulong_p(value_)) {"
        print "        throw std::overflow_error(\"Integer does not fit in uint64_t\");"
        print "    }"
        print "    return mpz_get_ui(value_);"
        print "}"
        print ""
    }
    { print }
    ' src/core/integer.cpp > /tmp/integer.cpp.new
    mv /tmp/integer.cpp.new src/core/integer.cpp
    echo "✓ 添加 to_uint64() 实现到 integer.cpp"
fi

# 3. 修复头文件中的字段名
for file in include/gnfs/{sieve,sqrt,linalg}/*.hpp 2>/dev/null; do
    if [ -f "$file" ]; then
        sed -i.bak \
            -e 's/rel\.ab\.a/rel.a/g' \
            -e 's/rel\.ab\.b/rel.b/g' \
            -e 's/\.rat_factors/.rational_factors/g' \
            -e 's/\.alg_factors/.algebraic_factors/g' \
            "$file" 2>/dev/null
    fi
done

echo "✓ 修复完成"
echo ""
echo "重新编译: cd build && make -j12"
