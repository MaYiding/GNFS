// test_sieve_tiny_simd.cpp — Parity & micro-benchmark for tiny prime stride helpers.
//
// Validates `detail::apply_log_p_stride` produces byte-for-byte the same
// sieve_array result as the reference scalar implementation across a wide
// matrix of strides (tiny primes 2..251), start offsets, region sizes,
// log_p values, and accumulated state — plus a micro-bench that compares
// the SIMD/unrolled path against the scalar reference.

#include <gnfs/sieve/lattice_sieve.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                                \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::cerr << "FAIL: " << (msg) << " (line " << __LINE__ << ")\n"; \
            ++tests_failed;                                                   \
            return;                                                           \
        }                                                                     \
    } while (0)

#define TEST_PASS(name)                              \
    do {                                             \
        std::cout << "  PASS: " << (name) << "\n";   \
        ++tests_passed;                              \
    } while (0)

using gnfs::sieve::detail::apply_log_p_stride;
using gnfs::sieve::detail::apply_log_p_stride_scalar;
using gnfs::sieve::detail::apply_log_p_range;
using gnfs::sieve::detail::tiny_simd_enabled;

// The first 54 odd primes up to 256 (matches the TINY_THRESHOLD = 256 in
// lattice_sieve.hpp build_prime_entries split).
constexpr uint32_t kTinyPrimes[] = {
      2,   3,   5,   7,  11,  13,  17,  19,  23,  29,
     31,  37,  41,  43,  47,  53,  59,  61,  67,  71,
     73,  79,  83,  89,  97, 101, 103, 107, 109, 113,
    127, 131, 137, 139, 149, 151, 157, 163, 167, 173,
    179, 181, 191, 193, 197, 199, 211, 223, 227, 229,
    233, 239, 241, 251};

// Reference scalar implementation — identical to the original lattice_sieve
// stride loop before the helper was introduced.  We do not rely on
// `apply_log_p_stride_scalar` here so we can cross-check the helper itself.
void reference_stride(std::vector<uint16_t>& arr,
                      size_t start,
                      size_t end,
                      size_t stride,
                      uint16_t lp) {
    if (stride == 0 || start >= end) return;
    for (size_t idx = start; idx < end; idx += stride) {
        arr[idx] = static_cast<uint16_t>(arr[idx] + lp);
    }
}

// Build an initial sieve_array_ filled with a noise pattern so empty rewrites
// would be obvious in a diff.
std::vector<uint16_t> make_seeded(size_t n, uint32_t seed) {
    std::vector<uint16_t> arr(n, 0);
    std::mt19937 rng(seed);
    for (size_t i = 0; i < n; ++i) {
        arr[i] = static_cast<uint16_t>(rng() & 0xFFFFu);
    }
    return arr;
}

// ── Tier A: pure parity tests ─────────────────────────────────────────────

void test_parity_basic_strides() {
    constexpr size_t REGION = 1u << 12;  // 4 KB
    constexpr uint16_t LP = 7;

    for (uint32_t p : kTinyPrimes) {
        for (size_t start_off = 0; start_off < 8; ++start_off) {
            const uint32_t seed = 0xC0FFEEu ^ p ^ static_cast<uint32_t>(start_off);
            auto a = make_seeded(REGION, seed);
            auto b = a;  // identical seed

            size_t start = std::min(start_off, REGION - 1);
            reference_stride(a, start, REGION, p, LP);
            apply_log_p_stride(b.data(), start, REGION, p, LP);

            TEST_ASSERT(a == b,
                        "stride parity p=" + std::to_string(p) +
                            " start=" + std::to_string(start_off));
        }
    }
    TEST_PASS("apply_log_p_stride parity across all tiny primes & start offsets");
}

void test_parity_random_lp() {
    // Random log_p values + region sizes, ensure accumulating writes survive
    // overflow (uint16_t wraps cleanly with `+=`).
    constexpr size_t TRIALS = 64;
    std::mt19937 rng(0xABCD1234u);

    for (size_t t = 0; t < TRIALS; ++t) {
        const uint32_t p = kTinyPrimes[rng() % std::size(kTinyPrimes)];
        const size_t region = 1024u + (rng() % 8192u);
        const uint16_t lp = static_cast<uint16_t>(rng() & 0xFFFFu);
        const size_t start = rng() % p;

        auto a = make_seeded(region, static_cast<uint32_t>(t * 7919u));
        auto b = a;

        reference_stride(a, start, region, p, lp);
        apply_log_p_stride(b.data(), start, region, p, lp);

        TEST_ASSERT(a == b, "random lp parity trial " + std::to_string(t));
    }
    TEST_PASS("apply_log_p_stride parity with random log_p & region sizes");
}

void test_parity_with_accumulated_array() {
    // Caller usually invokes the helper after other primes already wrote into
    // the array.  Verify the saturating add behavior is identical even when
    // accumulator values approach 0xFFFF.
    constexpr size_t REGION = 8192;
    auto seed = make_seeded(REGION, 0x55AA55AAu);
    // Pump in some high values to risk overflow.
    for (size_t i = 0; i < REGION; ++i) {
        seed[i] = static_cast<uint16_t>(seed[i] | 0xFE00u);
    }

    for (uint32_t p : kTinyPrimes) {
        auto a = seed;
        auto b = seed;

        reference_stride(a, 0, REGION, p, 0x0123);
        apply_log_p_stride(b.data(), 0, REGION, p, 0x0123);

        TEST_ASSERT(a == b,
                    "overflow-prone parity p=" + std::to_string(p));
    }
    TEST_PASS("apply_log_p_stride parity under wraparound (uint16_t overflow)");
}

void test_edge_cases() {
    constexpr size_t REGION = 256;
    std::vector<uint16_t> arr(REGION, 42);
    auto orig = arr;

    // stride=0: should no-op (caller checks; helper guards too).
    apply_log_p_stride(arr.data(), 0, REGION, 0, 99);
    TEST_ASSERT(arr == orig, "stride=0 must be a no-op");

    // start>=end: should no-op.
    apply_log_p_stride(arr.data(), REGION, REGION, 3, 99);
    TEST_ASSERT(arr == orig, "start==end no-op");
    apply_log_p_stride(arr.data(), 99, 50, 3, 99);
    TEST_ASSERT(arr == orig, "start>end no-op");

    // Single write (stride > end-start): touches only `start`.
    apply_log_p_stride(arr.data(), 0, REGION, 1000, 1);
    TEST_ASSERT(arr[0] == 43 && arr[1] == 42 && arr[REGION - 1] == 42,
                "stride>span: only start written");
    TEST_PASS("edge cases (stride=0, empty range, oversized stride)");
}

void test_env_gate_disables_simd() {
    // Force the scalar reference path via env var.  Note that
    // tiny_simd_enabled() caches its result, so we cannot toggle the env at
    // runtime within the same process.  Instead we directly compare the
    // explicit scalar reference helper against the public helper to confirm
    // both code paths return identical results.
    constexpr size_t REGION = 4096;
    constexpr uint16_t LP = 13;

    for (uint32_t p : kTinyPrimes) {
        auto a = make_seeded(REGION, 0xDEADBEEFu ^ p);
        auto b = a;

        apply_log_p_stride_scalar(a.data(), 0, REGION, p, LP);
        apply_log_p_stride(b.data(), 0, REGION, p, LP);

        TEST_ASSERT(a == b,
                    "scalar vs helper parity p=" + std::to_string(p));
    }
    TEST_PASS("apply_log_p_stride_scalar reference == apply_log_p_stride helper");
}

void test_apply_log_p_range_parity() {
    // Cross-check the existing NEON helper too — we now route v-prime
    // broadcasts through it from `sieve_row_chunk`, so confirm the helper
    // matches a naive scalar broadcast for assorted lengths.
    std::vector<size_t> lengths = {0, 1, 2, 7, 8, 9, 15, 16, 17, 31, 64, 1023, 4096};
    for (size_t len : lengths) {
        auto a = make_seeded(len + 16, 0x42424242u + static_cast<uint32_t>(len));
        auto b = a;

        const uint16_t lp = 0x1234;
        for (size_t i = 0; i < len; ++i) a[i] = static_cast<uint16_t>(a[i] + lp);
        apply_log_p_range(b.data(), len, lp);

        TEST_ASSERT(a == b,
                    "apply_log_p_range parity len=" + std::to_string(len));
    }
    TEST_PASS("apply_log_p_range parity across lengths 0..4096");
}

// ── Tier B: stress-mix simulating real sieve row apply ────────────────────

void test_full_row_mix() {
    // Mimic a single sieve_row_chunk pass: apply *all* tiny primes via the
    // helper, then via the reference, on the same buffer; results must agree.
    constexpr size_t REGION = 1u << 16;  // 64 KB row, typical bucket region.
    std::mt19937 rng(0xFEED5EEDu);

    std::vector<std::pair<size_t, uint16_t>> work;  // (start_offset, log_p)
    work.reserve(std::size(kTinyPrimes));
    for (uint32_t p : kTinyPrimes) {
        work.push_back({rng() % p, static_cast<uint16_t>((rng() & 0xFFu) + 1)});
    }

    auto a = make_seeded(REGION, 0xCAFEF00Du);
    auto b = a;

    size_t i = 0;
    for (uint32_t p : kTinyPrimes) {
        const auto& [start, lp] = work[i++];
        reference_stride(a, start, REGION, p, lp);
        apply_log_p_stride(b.data(), start, REGION, p, lp);
    }

    TEST_ASSERT(a == b, "full-row tiny-prime mix parity");
    TEST_PASS("full-row tiny-prime mix matches scalar reference byte-for-byte");
}

// ── Tier C: bench ─────────────────────────────────────────────────────────
//
// Wall-time micro-bench comparing SIMD/unrolled vs scalar reference for a
// realistic row-apply workload (one 64 KB row, all 54 tiny primes, 200
// iterations).  Output is informational; the test does not gate on a perf
// ratio — the gate-quality bench lives in test_lattice_sieve and the
// upstream test_25digit.

void bench_full_row_apply() {
    constexpr size_t REGION = 1u << 16;
    constexpr size_t ITERS = 200;

    std::vector<std::pair<size_t, uint16_t>> work;
    std::mt19937 rng(0xBE7C44E1u);
    for (uint32_t p : kTinyPrimes) {
        work.push_back({rng() % p, static_cast<uint16_t>((rng() & 0xFFu) + 1)});
    }

    std::vector<uint16_t> arr_helper(REGION, 0);
    std::vector<uint16_t> arr_scalar(REGION, 0);

    auto run = [&](auto helper, std::vector<uint16_t>& buf, const char* label) {
        std::fill(buf.begin(), buf.end(), 0u);
        auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t it = 0; it < ITERS; ++it) {
            size_t i = 0;
            for (uint32_t p : kTinyPrimes) {
                const auto& [start, lp] = work[i++];
                helper(buf.data(), start, REGION, p, lp);
            }
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        std::cout << "  [bench] " << label << ": " << us << " us total ("
                  << (us / ITERS) << " us/iter)\n";
        return us;
    };

    double t_simd = run(static_cast<void (*)(uint16_t*, size_t, size_t, size_t, uint16_t)>(
                            &apply_log_p_stride),
                        arr_helper, "apply_log_p_stride (SIMD/unrolled)");
    double t_scal = run(&apply_log_p_stride_scalar,
                        arr_scalar, "apply_log_p_stride_scalar (reference)");

    TEST_ASSERT(arr_helper == arr_scalar,
                "bench parity guard — buffers must match exactly");

    double ratio = t_scal / t_simd;
    std::cout << "  [bench] speedup (scalar / helper) = " << ratio << "x\n";
    // Sanity: helper must not be > 3x slower than scalar (defensive guard).
    TEST_ASSERT(ratio > 0.33, "helper should not be wildly slower than scalar");
    TEST_PASS("micro-bench: SIMD/unrolled path matches scalar reference");
}

}  // namespace

int main() {
    std::cout << "============================================\n"
              << "  Sieve Tiny Prime SIMD / Loop Fusion Tests\n"
              << "============================================\n";

    std::cout << "[env] tiny_simd_enabled() = "
              << (tiny_simd_enabled() ? "true (SIMD path)" : "false (scalar fallback)")
              << "  (set GNFS_SIEVE_NO_TINY_SIMD=1 to disable)\n\n";

    test_parity_basic_strides();
    test_parity_random_lp();
    test_parity_with_accumulated_array();
    test_edge_cases();
    test_env_gate_disables_simd();
    test_apply_log_p_range_parity();
    test_full_row_mix();
    bench_full_row_apply();

    std::cout << "\n============================================\n"
              << "  Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n"
              << "============================================\n";
    return (tests_failed == 0) ? 0 : 1;
}
