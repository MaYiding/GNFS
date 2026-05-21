// test_trial_wheel_bench.cpp — Micro-benchmark for TrialDivider rational paths.
//
// Measures wall-clock time for repeated divide_rational calls on a synthetic
// cofactor stream. The wheel-2-3-5 prefix is inlined in TrialDivider; to
// measure the baseline we run a "naive" loop that walks the same factor base
// using only `while (v % p == 0)` (no wheel). The naive loop lives in this
// translation unit so the comparison is fair (same compiler/optimizer
// settings, same factor base layout, same Integer construction overhead).
//
// Usage:
//   ./test_trial_wheel_bench [count] [seed]
//   Default: count=200000, seed=12345

#include <gnfs/cofactor/trial_division.hpp>
#include <gnfs/cofactor/wheel235.hpp>
#include <gnfs/core/integer.hpp>
#include <gnfs/core/types.hpp>
#include <gnfs/factor_base/factor_base.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

// Build a real-shape FB by listing all primes < bound.
gnfs::factor_base::FactorBase build_fb(uint32_t bound) {
    gnfs::factor_base::FactorBase fb(gnfs::core::FactorBaseParams{bound, bound, bound});
    std::vector<bool> sieve(bound + 1, true);
    sieve[0] = sieve[1] = false;
    for (uint64_t i = 2; i * i <= bound; ++i) {
        if (sieve[i]) {
            for (uint64_t j = i * i; j <= bound; j += i) sieve[j] = false;
        }
    }
    for (uint64_t i = 2; i <= bound; ++i) {
        if (sieve[i]) {
            fb.add_rational(static_cast<uint32_t>(i),
                            gnfs::factor_base::compute_log_prime(static_cast<uint32_t>(i),
                                                                  gnfs::core::SIEVE_LOG_SCALE));
        }
    }
    return fb;
}

// Naive baseline: walk the FB in order, strip each prime via while loop.
// Mirrors divide_rational_u64_from exactly except the wheel-2-3-5 prefix
// is omitted. We populate a vector-backed result so allocator behavior is
// comparable to the wheel TrialDivisionResult.
struct NaiveResult {
    bool is_smooth = false;
    uint64_t cofactor = 0;
    std::vector<uint32_t> factor_indices;
    std::vector<uint8_t> exponents;
};

NaiveResult naive_divide_u64(uint64_t v, const gnfs::factor_base::FactorBase& fb) {
    NaiveResult r;
    r.factor_indices.reserve(16);
    r.exponents.reserve(16);
    auto rationals = fb.rational();
    if (v == 0) { r.is_smooth = true; r.cofactor = 1; return r; }
    for (uint32_t idx = 0; idx < rationals.size(); ++idx) {
        uint32_t p = rationals[idx].p;
        uint8_t e = 0;
        while (v % p == 0 && e < 255) { v /= p; ++e; }
        if (e > 0) { r.factor_indices.push_back(idx); r.exponents.push_back(e); }
        if (v == 1) { r.is_smooth = true; break; }
        if (idx + 1 < rationals.size() && v < rationals[idx + 1].p) break;
        if (v > fb.params().rational_bound &&
            v < static_cast<uint64_t>(p) * p) break;
    }
    r.cofactor = v;
    if (v == 1) r.is_smooth = true;
    return r;
}

// Generate a stream of synthetic cofactor values. Most have several small
// factors (mimicking real sieve cofactors). 30 % have 2, 3, and 5 mass; 30 %
// have 2 and 3 only; 30 % have 2 only; 10 % are coprime to 30.
uint64_t make_cofactor(std::mt19937_64& rng) {
    uint32_t r = rng() % 100;
    uint64_t base = (rng() | 1ULL) >> 12;  // ~52 bits, odd
    base |= 1;
    if (r < 30) {
        // multiply by small powers of 2/3/5
        uint32_t a = 1 + (rng() % 8);
        uint32_t b = 1 + (rng() % 4);
        uint32_t c = 1 + (rng() % 3);
        for (uint32_t k = 0; k < a; ++k) base <<= 1;
        for (uint32_t k = 0; k < b; ++k) base *= 3;
        for (uint32_t k = 0; k < c; ++k) base *= 5;
    } else if (r < 60) {
        uint32_t a = 1 + (rng() % 8);
        uint32_t b = 1 + (rng() % 4);
        for (uint32_t k = 0; k < a; ++k) base <<= 1;
        for (uint32_t k = 0; k < b; ++k) base *= 3;
    } else if (r < 90) {
        uint32_t a = 1 + (rng() % 8);
        for (uint32_t k = 0; k < a; ++k) base <<= 1;
    }
    // Cap at 2^60 so the value comfortably fits uint64 even after multiplies
    return base & ((1ULL << 60) - 1) ?: 1ULL;
}

void run_bench(uint32_t fb_bound, size_t count, uint64_t seed) {
    auto fb = build_fb(fb_bound);
    gnfs::cofactor::TrialDivider divider(fb);

    std::mt19937_64 rng(seed);
    std::vector<uint64_t> values;
    values.reserve(count);
    for (size_t i = 0; i < count; ++i) values.push_back(make_cofactor(rng));

    std::cout << "FB primes < " << fb_bound << " → " << fb.rational().size() << "\n";
    std::cout << "Count: " << count << ", seed: " << seed << "\n";

    // Warm-up: one pass each to settle caches and dynamic linker.
    {
        for (uint64_t v : values) { volatile auto r = naive_divide_u64(v, fb); (void)r; }
        for (uint64_t v : values) { volatile auto r = divider.divide_rational_u64(v); (void)r; }
    }

    // --- Naive baseline ---
    size_t naive_smooth = 0;
    size_t naive_acc = 0;  // prevent dead-store elimination
    auto t0 = std::chrono::steady_clock::now();
    for (uint64_t v : values) {
        auto r = naive_divide_u64(v, fb);
        if (r.is_smooth) ++naive_smooth;
        naive_acc += r.factor_indices.size();
    }
    auto t1 = std::chrono::steady_clock::now();
    auto naive_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    // --- Wheel TrialDivider ---
    size_t wheel_smooth = 0;
    size_t wheel_acc = 0;
    t0 = std::chrono::steady_clock::now();
    for (uint64_t v : values) {
        auto r = divider.divide_rational_u64(v);
        if (r.is_smooth) ++wheel_smooth;
        wheel_acc += r.factor_indices.size();
    }
    t1 = std::chrono::steady_clock::now();
    auto wheel_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    // Side-effect to keep the optimizer honest.
    if (naive_acc + wheel_acc == 0xDEADBEEFULL) std::fprintf(stderr, "x\n");

    // Sanity: same smooth count.
    if (naive_smooth != wheel_smooth) {
        std::fprintf(stderr,
                     "[WARN] smooth count mismatch: naive=%zu wheel=%zu\n",
                     naive_smooth, wheel_smooth);
    }

    double naive_us_per = static_cast<double>(naive_ns) / 1000.0 / static_cast<double>(count);
    double wheel_us_per = static_cast<double>(wheel_ns) / 1000.0 / static_cast<double>(count);
    double speedup = naive_us_per > 0.0 ? naive_us_per / wheel_us_per : 0.0;

    std::printf("\nResults (per call):\n");
    std::printf("  Naive  : %8.3f µs/call  (total %8.3f ms)\n",
                naive_us_per, naive_ns / 1.0e6);
    std::printf("  Wheel  : %8.3f µs/call  (total %8.3f ms)\n",
                wheel_us_per, wheel_ns / 1.0e6);
    std::printf("  Speedup: %5.2fx\n", speedup);
    std::printf("  Smooth hits: naive=%zu wheel=%zu\n", naive_smooth, wheel_smooth);
}

} // namespace

int main(int argc, char** argv) {
    size_t count = (argc > 1) ? static_cast<size_t>(std::atoll(argv[1])) : 200000;
    uint64_t seed = (argc > 2) ? static_cast<uint64_t>(std::strtoull(argv[2], nullptr, 0)) : 12345ULL;

    std::cout << "=== Trial Wheel Microbench ===\n\n";
    // Real-world rational FB sizes for typical GNFS jobs.
    for (uint32_t b : {1000u, 5000u, 20000u}) {
        std::cout << "----- FB bound = " << b << " -----\n";
        run_bench(b, count, seed);
        std::cout << "\n";
    }
    return 0;
}
