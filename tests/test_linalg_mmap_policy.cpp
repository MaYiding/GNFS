// test_linalg_mmap_policy.cpp — Unit tests for the three-state
// GNFS_LINALG_MMAP policy parser used by Pipeline::solve_matrix to
// decide between in-memory CSR and out-of-core MmapCSRMatrix.

#include <gnfs/linalg/linalg_mmap_policy.hpp>
#include <cstdio>
#include <cstdlib>

using gnfs::linalg::MmapPolicy;
using gnfs::linalg::parse_mmap_policy;
using gnfs::linalg::should_use_mmap;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, msg); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define TEST_PASS(name) do { \
    std::printf("  PASS: %s\n", name); \
    tests_passed++; \
} while (0)

void test_parse_null_is_off() {
    TEST_ASSERT(parse_mmap_policy(nullptr) == MmapPolicy::Off, "nullptr → Off");
    TEST_PASS("nullptr / unset env → Off");
}

void test_parse_empty_is_off() {
    TEST_ASSERT(parse_mmap_policy("") == MmapPolicy::Off, "empty → Off");
    TEST_PASS("empty env → Off");
}

void test_parse_off_variants() {
    TEST_ASSERT(parse_mmap_policy("0") == MmapPolicy::Off, "0 → Off");
    TEST_ASSERT(parse_mmap_policy("off") == MmapPolicy::Off, "off → Off");
    TEST_ASSERT(parse_mmap_policy("false") == MmapPolicy::Off, "false → Off");
    TEST_PASS("Off variants parsed (0/off/false)");
}

void test_parse_on_variants() {
    TEST_ASSERT(parse_mmap_policy("1") == MmapPolicy::On, "1 → On");
    TEST_ASSERT(parse_mmap_policy("on") == MmapPolicy::On, "on → On");
    TEST_ASSERT(parse_mmap_policy("true") == MmapPolicy::On, "true → On");
    TEST_PASS("On variants parsed (1/on/true)");
}

void test_parse_auto_variants() {
    TEST_ASSERT(parse_mmap_policy("auto") == MmapPolicy::Auto, "auto");
    TEST_ASSERT(parse_mmap_policy("AUTO") == MmapPolicy::Auto, "AUTO");
    TEST_ASSERT(parse_mmap_policy("Auto") == MmapPolicy::Auto, "Auto");
    TEST_PASS("Auto variants parsed (auto/AUTO/Auto)");
}

void test_parse_unknown_is_off() {
    TEST_ASSERT(parse_mmap_policy("yes") == MmapPolicy::Off, "unknown → Off");
    TEST_ASSERT(parse_mmap_policy("xyz") == MmapPolicy::Off, "garbage → Off");
    TEST_PASS("unknown values fall through to Off (safe default)");
}

void test_should_use_mmap_off() {
    TEST_ASSERT(!should_use_mmap(MmapPolicy::Off, 0), "Off + 0 nnz");
    TEST_ASSERT(!should_use_mmap(MmapPolicy::Off, 1ULL << 40), "Off + 1T nnz");
    TEST_PASS("Off never picks mmap regardless of nnz");
}

void test_should_use_mmap_on() {
    TEST_ASSERT(should_use_mmap(MmapPolicy::On, 0), "On + 0 nnz");
    TEST_ASSERT(should_use_mmap(MmapPolicy::On, 1), "On + 1 nnz");
    TEST_ASSERT(should_use_mmap(MmapPolicy::On, 1ULL << 40), "On + huge");
    TEST_PASS("On always picks mmap regardless of nnz");
}

void test_should_use_mmap_auto() {
    // Default threshold = 2 GiB = 2147483648 bytes; nnz * 4 bytes.
    // Threshold in nnz = 2 GiB / 4 = 536870912.
    constexpr std::uint64_t threshold_nnz = (2ULL * 1024 * 1024 * 1024) / 4;
    TEST_ASSERT(!should_use_mmap(MmapPolicy::Auto, 0), "Auto + 0 → in-mem");
    TEST_ASSERT(!should_use_mmap(MmapPolicy::Auto, threshold_nnz - 1),
                "Auto + just below threshold → in-mem");
    TEST_ASSERT(should_use_mmap(MmapPolicy::Auto, threshold_nnz),
                "Auto + at threshold → mmap");
    TEST_ASSERT(should_use_mmap(MmapPolicy::Auto, threshold_nnz * 2),
                "Auto + above threshold → mmap");
    TEST_PASS("Auto uses 2 GiB nnz·uint32 threshold");
}

void test_threshold_env_override() {
    // Override threshold to 1 KiB (very low) and verify a small matrix triggers
    // mmap; then unset and verify default returns.
    setenv("GNFS_LINALG_MMAP_THRESHOLD_BYTES", "1024", 1);
    TEST_ASSERT(should_use_mmap(MmapPolicy::Auto, 300),  // 300*4=1200 > 1024
                "low threshold triggers mmap on tiny matrix");
    unsetenv("GNFS_LINALG_MMAP_THRESHOLD_BYTES");
    TEST_ASSERT(!should_use_mmap(MmapPolicy::Auto, 300),
                "default threshold blocks mmap on tiny matrix after unset");
    TEST_PASS("GNFS_LINALG_MMAP_THRESHOLD_BYTES override works");
}

int main() {
    std::printf("== GNFS_LINALG_MMAP policy tests ==\n");
    test_parse_null_is_off();
    test_parse_empty_is_off();
    test_parse_off_variants();
    test_parse_on_variants();
    test_parse_auto_variants();
    test_parse_unknown_is_off();
    test_should_use_mmap_off();
    test_should_use_mmap_on();
    test_should_use_mmap_auto();
    test_threshold_env_override();
    std::printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
