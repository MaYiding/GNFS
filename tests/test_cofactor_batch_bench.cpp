// test_cofactor_batch_bench.cpp — Micro-benchmark for batch trial division
// and ECM curve warm-pool (T5 Stage 4, optional, slow tier).
//
// Not run by smoke / module suites. Invoke manually via
//   ./build/test_cofactor_batch_bench [count] [seed]
// or via `./scripts/test.sh module cofactor --slow` once registered.
//
// Reports K=1 vs K=8 and K=32 timing for batch_trial_divide on 1000+ cofactors,
// and pool=0 vs pool=N timing for EcmCurvePool curve generation. Numbers are
// informational — no assertion failures so CI/test grids don't flake on noisy
// timing.

#include <gnfs/cofactor/batch_trial.hpp>
#include <gnfs/cofactor/ecm_curve_pool.hpp>
#include <gnfs/core/integer.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <vector>

using gnfs::core::Integer;
using gnfs::cofactor::BatchTrialResult;
using gnfs::cofactor::batch_trial_divide;
using gnfs::cofactor::EcmCurvePool;

namespace {

// Generate a vector of cofactors emulating a sieve workload — random
// composites in the 30-50 bit range.
std::vector<Integer> generate_cofactors(size_t count, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<Integer> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        uint64_t v = (rng() % (1ULL << 40)) + 1;
        out.push_back(Integer{v});
    }
    return out;
}

double time_batch_trial(std::span<const Integer> cofactors, size_t prime_bound,
                        size_t batch_K) {
    // Process `cofactors` in chunks of `batch_K`. Wall-clock reported in ms.
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();

    for (size_t off = 0; off < cofactors.size(); off += batch_K) {
        size_t end = std::min(off + batch_K, cofactors.size());
        std::span<const Integer> chunk(cofactors.data() + off, end - off);
        BatchTrialResult r = batch_trial_divide(chunk, prime_bound);
        // touch to defeat dead-code elimination
        if (r.size() == 0) return 0.0;
    }

    auto t1 = clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

void bench_batch_trial(size_t count, uint64_t seed) {
    std::cout << "\n=== batch_trial_divide bench ===\n";
    std::cout << "count=" << count << " seed=" << seed << " prime_bound=1000\n";

    auto cofactors = generate_cofactors(count, seed);
    constexpr size_t bound = 1000;

    double t1  = time_batch_trial(cofactors, bound, 1);
    double t8  = time_batch_trial(cofactors, bound, 8);
    double t32 = time_batch_trial(cofactors, bound, 32);
    double t128 = time_batch_trial(cofactors, bound, 128);

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "K=1   : " << t1   << " ms (baseline)\n";
    std::cout << "K=8   : " << t8   << " ms  (speedup " << (t1 / std::max(t8,  1e-6)) << "x)\n";
    std::cout << "K=32  : " << t32  << " ms  (speedup " << (t1 / std::max(t32, 1e-6)) << "x)\n";
    std::cout << "K=128 : " << t128 << " ms  (speedup " << (t1 / std::max(t128,1e-6)) << "x)\n";
}

void bench_curve_pool(uint64_t seed) {
    std::cout << "\n=== EcmCurvePool bench ===\n";

    // Use a 31-bit semiprime that's common in GNFS Phase 4 (~ASCII demo
    // workload size — still relatively cheap).
    Integer n{uint64_t{2261419229ULL}};
    std::mt19937_64 rng(seed);

    auto make_sigmas = [&](size_t k) {
        std::vector<uint64_t> sgs;
        sgs.reserve(k);
        for (size_t i = 0; i < k; ++i) sgs.push_back((rng() % 100000) + 6);
        return sgs;
    };

    auto time_pool = [&](size_t pool_size, size_t pops) {
        auto sgs = make_sigmas(pool_size);
        using clock = std::chrono::steady_clock;

        auto t0 = clock::now();
        EcmCurvePool pool(pool_size, n, sgs);
        auto t1 = clock::now();
        for (size_t i = 0; i < pops; ++i) (void)pool.pop_curve();
        auto t2 = clock::now();
        double build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double pop_ms   = std::chrono::duration<double, std::milli>(t2 - t1).count();
        return std::make_pair(build_ms, pop_ms);
    };

    std::cout << std::fixed << std::setprecision(3);
    for (size_t k : {size_t{0}, size_t{8}, size_t{32}, size_t{128}}) {
        auto [b, p] = time_pool(k, std::max<size_t>(k, 8));
        std::cout << "pool=" << k << " build=" << b << " ms  pop="
                  << p << " ms (pops=" << std::max<size_t>(k, 8) << ")\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    size_t count = 1000;
    uint64_t seed = 0xCAFEBABEULL;
    if (argc >= 2) count = static_cast<size_t>(std::stoul(argv[1]));
    if (argc >= 3) seed  = static_cast<uint64_t>(std::stoull(argv[2]));

    bench_batch_trial(count, seed);
    bench_curve_pool(seed);

    std::cout << "\n=== bench complete ===\n";
    return 0;
}
