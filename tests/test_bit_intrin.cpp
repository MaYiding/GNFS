// Unit tests for gnfs/util/bit_intrin.hpp and cpu_intrin.hpp cross-compiler
// intrinsic wrappers. Ensures C++20 <bit> wrappers behave identically to
// GCC/Clang `__builtin_*` semantics for non-zero inputs (which is the only
// case used in the codebase).

#include "gnfs/util/bit_intrin.hpp"
#include "gnfs/util/cpu_intrin.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace gnfs::util;

static int failures = 0;

#define EXPECT_EQ(a, b)                                                        \
    do {                                                                        \
        auto _av = (a);                                                         \
        auto _bv = (b);                                                         \
        if (_av != _bv) {                                                       \
            std::fprintf(stderr,                                                \
                         "FAIL %s:%d: %s != %s (lhs=%lld rhs=%lld)\n",          \
                         __FILE__, __LINE__, #a, #b,                            \
                         static_cast<long long>(_av),                           \
                         static_cast<long long>(_bv));                          \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static void test_ctz32() {
    EXPECT_EQ(ctz32(1u), 0);
    EXPECT_EQ(ctz32(2u), 1);
    EXPECT_EQ(ctz32(8u), 3);
    EXPECT_EQ(ctz32(0x80000000u), 31);
    EXPECT_EQ(ctz32(0xffffffffu), 0);
}

static void test_ctz64() {
    EXPECT_EQ(ctz64(uint64_t{1}), 0);
    EXPECT_EQ(ctz64(uint64_t{1} << 7), 7);
    EXPECT_EQ(ctz64(uint64_t{1} << 63), 63);
    EXPECT_EQ(ctz64(uint64_t{0xfffffffffffffffful}), 0);
}

static void test_clz32() {
    EXPECT_EQ(clz32(1u), 31);
    EXPECT_EQ(clz32(2u), 30);
    EXPECT_EQ(clz32(0x80000000u), 0);
    EXPECT_EQ(clz32(0xffffffffu), 0);
}

static void test_clz64() {
    EXPECT_EQ(clz64(uint64_t{1}), 63);
    EXPECT_EQ(clz64(uint64_t{1} << 32), 31);
    EXPECT_EQ(clz64(uint64_t{1} << 63), 0);
}

static void test_popcount32() {
    EXPECT_EQ(popcount32(0u), 0);
    EXPECT_EQ(popcount32(1u), 1);
    EXPECT_EQ(popcount32(0xffu), 8);
    EXPECT_EQ(popcount32(0xffffffffu), 32);
    EXPECT_EQ(popcount32(0x55555555u), 16);
}

static void test_popcount64() {
    EXPECT_EQ(popcount64(uint64_t{0}), 0);
    EXPECT_EQ(popcount64(uint64_t{1}), 1);
    EXPECT_EQ(popcount64(uint64_t{0xff}), 8);
    EXPECT_EQ(popcount64(uint64_t{0xffffffffffffffff}), 64);
    EXPECT_EQ(popcount64(uint64_t{0x5555555555555555}), 32);
}

static void test_cpu_intrin_smoke() {
    // These should not crash on any supported platform.
    int dummy = 0;
    prefetch_read<0>(&dummy);
    prefetch_read<1>(&dummy);
    prefetch_read<2>(&dummy);
    prefetch_read<3>(&dummy);
    prefetch_write<1>(&dummy);
    for (int i = 0; i < 10; ++i) {
        cpu_pause();
    }
}

int main() {
    test_ctz32();
    test_ctz64();
    test_clz32();
    test_clz64();
    test_popcount32();
    test_popcount64();
    test_cpu_intrin_smoke();

    if (failures == 0) {
        std::printf("test_bit_intrin: all PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_bit_intrin: %d FAILURES\n", failures);
    return 1;
}
