// test_sieve_region_tile.cpp - Correctness tests for the row-tile gate
// helper exposed in `<gnfs/sieve/region_tile.hpp>`.
//
// Strategy
// --------
// The helper only exposes an ENV-cached integer (`region_tile_bits`) and
// two derived predicates (`region_tile_enabled`, `region_tile_size_rows`).
// The unit tests verify the documented contract:
//
//   * Unset / empty / non-numeric env -> bits = 0, enabled = false,
//     size_rows = 0.
//   * Explicit "0" env -> same as unset.
//   * Valid N in [1, 8] -> bits = N, size_rows = 1 << N, enabled = true.
//   * N >= 9 -> clamped to 8.
//   * Negative numbers / trailing garbage -> 0.
//   * The reset hook re-reads the env so successive scenarios in the same
//     process see the updated state.
//
// The tests intentionally avoid invoking the real lattice sieve — this
// helper is a standalone gate, the apply-loop wiring is a follow-up
// change. We just need to lock the contract down before any callsite
// starts depending on it.

#include <gnfs/sieve/region_tile.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace rt = gnfs::sieve;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do {                                           \
    if (!(cond)) {                                                            \
        std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, msg);            \
        tests_failed++;                                                       \
        return;                                                               \
    }                                                                         \
} while (0)

#define TEST_PASS(name) do {                                                  \
    std::printf("  PASS: %s\n", name);                                        \
    tests_passed++;                                                           \
} while (0)

// Helpers -------------------------------------------------------------------

// Sets the env var (or unsets it when `value == nullptr`) and flushes the
// cached state so a subsequent `region_tile_bits()` call re-resolves.
static void set_env_and_reload(const char* value) {
    if (value == nullptr) {
        ::unsetenv("GNFS_SIEVE_REGION_TILE_BITS");
    } else {
        ::setenv("GNFS_SIEVE_REGION_TILE_BITS", value, /*overwrite=*/1);
    }
    rt::region_tile_reset_env_cache_for_testing();
}

// Tests ---------------------------------------------------------------------

// Test 1 - Unset env defaults to N = 0 (disabled). The cached state must
// report bits == 0, enabled == false, and size_rows == 0 so callers can
// branch on a single predicate at apply-phase entry without an additional
// check.
static void test_env_unset_default_zero() {
    std::printf("[1] env unset defaults to N=0\n");
    set_env_and_reload(nullptr);

    TEST_ASSERT(rt::region_tile_bits() == 0, "unset bits should be 0");
    TEST_ASSERT(!rt::region_tile_enabled(),
                "unset must not enable tiling");
    TEST_ASSERT(rt::region_tile_size_rows() == 0,
                "unset must yield 0 rows");
    TEST_PASS("env unset defaults to N=0");
}

// Test 2 - Explicit "0" env. Same contract as unset: disabled, size 0.
// Important for regression bisects that want to force the path off via a
// concrete value rather than relying on shell defaults.
static void test_env_zero_explicit() {
    std::printf("[2] env=\"0\" explicit disable\n");
    set_env_and_reload("0");

    TEST_ASSERT(rt::region_tile_bits() == 0,
                "explicit 0 should resolve to 0 bits");
    TEST_ASSERT(!rt::region_tile_enabled(),
                "explicit 0 must not enable tiling");
    TEST_ASSERT(rt::region_tile_size_rows() == 0,
                "explicit 0 must yield 0 rows");
    TEST_PASS("env=\"0\" explicit disable");
}

// Test 3 - Valid N in [1, 8]. For each documented value the cached bits
// must round-trip and the size in rows must equal exactly 1 << N. This
// covers the full supported range, so we sweep all eight values rather
// than picking a representative subset.
static void test_env_valid_range() {
    std::printf("[3] env in [1, 8] sweep\n");
    for (int n = 1; n <= rt::kRegionTileMaxBits; ++n) {
        const std::string s = std::to_string(n);
        set_env_and_reload(s.c_str());

        const int got = rt::region_tile_bits();
        if (got != n) {
            std::fprintf(stderr,
                         "  N=%d expected bits=%d got=%d\n", n, n, got);
            tests_failed++;
            return;
        }
        if (!rt::region_tile_enabled()) {
            std::fprintf(stderr,
                         "  N=%d enabled=false (expected true)\n", n);
            tests_failed++;
            return;
        }
        const std::size_t rows = rt::region_tile_size_rows();
        const std::size_t expected_rows =
            static_cast<std::size_t>(1) << static_cast<unsigned>(n);
        if (rows != expected_rows) {
            std::fprintf(stderr,
                         "  N=%d expected size_rows=%zu got=%zu\n",
                         n, expected_rows, rows);
            tests_failed++;
            return;
        }
    }
    TEST_PASS("env in [1, 8] sweep");
}

// Test 4 - Values strictly above kRegionTileMaxBits clamp to the max so
// users cannot accidentally request a 1024-row tile that would defeat
// the locality goal. We also verify a much larger value still clamps,
// guaranteeing the clamp is not a special case of N=9.
static void test_env_above_max_clamps_to_8() {
    std::printf("[4] env above max clamps to 8\n");
    set_env_and_reload("9");
    TEST_ASSERT(rt::region_tile_bits() == rt::kRegionTileMaxBits,
                "N=9 should clamp to kRegionTileMaxBits");
    TEST_ASSERT(rt::region_tile_size_rows() == 256,
                "N=9 clamped tile size must be 256 rows");

    set_env_and_reload("100");
    TEST_ASSERT(rt::region_tile_bits() == rt::kRegionTileMaxBits,
                "N=100 should clamp to kRegionTileMaxBits");
    TEST_ASSERT(rt::region_tile_size_rows() == 256,
                "N=100 clamped tile size must be 256 rows");

    set_env_and_reload("2147483647");  // INT_MAX-like ceiling
    TEST_ASSERT(rt::region_tile_bits() == rt::kRegionTileMaxBits,
                "very large N must still clamp");
    TEST_PASS("env above max clamps to 8");
}

// Test 5 - Non-numeric values (garbage, empty, whitespace) must resolve
// to 0. `std::strtol` would otherwise return 0 silently for "garbage"
// while leaving `end == v`, but we reject that case explicitly to
// distinguish "valid zero" from "parse failed". The empty-string case
// is also exercised here because the env API permits empty values.
static void test_env_invalid_non_numeric() {
    std::printf("[5] non-numeric env values default to 0\n");
    const char* cases[] = {
        "garbage",
        "abc",
        "1.5",       // decimal point trailing junk
        "1x",        // numeric prefix + suffix
        "+",         // sign without digits
        "-",
        "",          // empty string
        " ",
        "  3  ",     // whitespace around number (strtol allows leading ws,
                     // but our parser rejects trailing junk)
        "-1",        // negative
        "-5",
    };
    for (const char* v : cases) {
        set_env_and_reload(v);
        const int got = rt::region_tile_bits();
        if (got != 0) {
            std::fprintf(stderr,
                         "  invalid env=\"%s\" should yield 0, got %d\n",
                         v, got);
            tests_failed++;
            return;
        }
        if (rt::region_tile_enabled()) {
            std::fprintf(stderr,
                         "  invalid env=\"%s\" enabled=true (expected false)\n",
                         v);
            tests_failed++;
            return;
        }
    }
    TEST_PASS("non-numeric env values default to 0");
}

// Test 6 - size_rows == 2^N for the full supported range, computed from
// the documented constant. Redundant with test 3 in spirit but isolates
// the formula in case the helper layout changes in future and a regression
// flips the relationship.
static void test_region_tile_size_rows() {
    std::printf("[6] region_tile_size_rows == 2^N\n");
    const std::size_t expected[] = {
        0,    // N=0 (disabled): no tile
        2,    // N=1
        4,    // N=2
        8,    // N=3
        16,   // N=4
        32,   // N=5
        64,   // N=6
        128,  // N=7
        256,  // N=8 = kRegionTileMaxBits
    };
    for (int n = 0; n <= rt::kRegionTileMaxBits; ++n) {
        if (n == 0) {
            set_env_and_reload("0");
        } else {
            const std::string s = std::to_string(n);
            set_env_and_reload(s.c_str());
        }
        const std::size_t got = rt::region_tile_size_rows();
        if (got != expected[n]) {
            std::fprintf(stderr,
                         "  N=%d expected size=%zu got=%zu\n",
                         n, expected[n], got);
            tests_failed++;
            return;
        }
    }
    TEST_PASS("region_tile_size_rows == 2^N");
}

// Test 7 - The reset hook actually re-reads the env. Without the hook the
// `std::call_once` flag would freeze the cached value at process start,
// so unit tests rely on the hook to flip scenarios. We confirm both the
// "off -> on" and "on -> off" transitions plus an idempotent re-read.
static void test_reset_env_cache() {
    std::printf("[7] reset_env_cache re-reads env\n");
    // off -> on -> off chain
    set_env_and_reload(nullptr);
    TEST_ASSERT(rt::region_tile_bits() == 0,
                "baseline unset should yield 0");

    set_env_and_reload("4");
    TEST_ASSERT(rt::region_tile_bits() == 4,
                "reset to N=4 should yield 4");
    TEST_ASSERT(rt::region_tile_size_rows() == 16,
                "reset to N=4 should yield 16 rows");

    set_env_and_reload("0");
    TEST_ASSERT(rt::region_tile_bits() == 0,
                "reset back to 0 should yield 0");
    TEST_ASSERT(!rt::region_tile_enabled(),
                "reset back to 0 should disable");

    set_env_and_reload("6");
    TEST_ASSERT(rt::region_tile_bits() == 6,
                "reset to N=6 should yield 6");

    // Idempotent re-read - calling the hook twice with the same env must
    // not change the cached value.
    set_env_and_reload("6");
    TEST_ASSERT(rt::region_tile_bits() == 6,
                "idempotent reload at N=6 should still be 6");

    set_env_and_reload(nullptr);
    TEST_PASS("reset_env_cache re-reads env");
}

// Test 8 - The enabled() predicate is exactly the "bits > 0" condition.
// We verify both directions explicitly so a future refactor that changes
// the predicate to a non-trivial formula cannot silently regress this
// invariant (callers branch on `region_tile_enabled()` at apply-phase
// entry, so the predicate is a load-bearing contract).
static void test_enabled_predicate() {
    std::printf("[8] enabled() iff bits > 0\n");
    // N = 0 must report disabled.
    set_env_and_reload("0");
    TEST_ASSERT(!rt::region_tile_enabled(),
                "N=0 should report disabled");

    // All N in [1, 8] must report enabled.
    for (int n = 1; n <= rt::kRegionTileMaxBits; ++n) {
        const std::string s = std::to_string(n);
        set_env_and_reload(s.c_str());
        if (!rt::region_tile_enabled()) {
            std::fprintf(stderr,
                         "  N=%d enabled=false expected true\n", n);
            tests_failed++;
            return;
        }
    }

    // Cleanup: leave the env unset for any subsequent tests.
    set_env_and_reload(nullptr);
    TEST_PASS("enabled() iff bits > 0");
}

// Main ---------------------------------------------------------------------

int main() {
    std::printf("=== test_sieve_region_tile ===\n");
    const char* env = std::getenv("GNFS_SIEVE_REGION_TILE_BITS");
    std::printf("GNFS_SIEVE_REGION_TILE_BITS = %s\n", env ? env : "(unset)");

    test_env_unset_default_zero();
    test_env_zero_explicit();
    test_env_valid_range();
    test_env_above_max_clamps_to_8();
    test_env_invalid_non_numeric();
    test_region_tile_size_rows();
    test_reset_env_cache();
    test_enabled_predicate();

    // Restore default env for any downstream tests that run in the same
    // process (we run each test binary as a separate process, but the
    // habit of cleaning up the env keeps test interactions predictable).
    ::unsetenv("GNFS_SIEVE_REGION_TILE_BITS");
    rt::region_tile_reset_env_cache_for_testing();

    std::printf("\n=== Summary ===\n");
    std::printf("  passed: %d\n", tests_passed);
    std::printf("  failed: %d\n", tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
