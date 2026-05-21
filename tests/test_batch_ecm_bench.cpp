// test_batch_ecm_bench.cpp — Micro-benchmark for ECM::quick_factor hot path.
//
// Measures wall-clock time for repeated quick_factor calls on a synthetic
// cofactor stream. Compares against the "naive" implementation that calls
// sieve_primes(B1) inside each invocation (replicating pre-BatchContext code).
//
// Usage:
//   ./test_batch_ecm_bench [count] [seed]
//   Default: count=2000, seed=12345

#include <gnfs/cofactor/ecm.hpp>
#include <gnfs/core/integer.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using gnfs::core::Integer;
using gnfs::cofactor::ECM;

namespace {

// Generate `count` synthetic cofactor candidates that look like Phase 4 ECM
// inputs: composites in the 30–55 bit range that survived SQUFOF + rho.
std::vector<Integer> generate_cofactor_stream(size_t count, uint64_t seed) {
    // Pool of primes in [10K, 100K] — pairs land in 28-34 bit range.
    static const std::vector<uint32_t> small_primes = []() {
        std::vector<uint32_t> ps;
        std::vector<bool> sieve(100001, true);
        sieve[0] = sieve[1] = false;
        for (uint64_t i = 2; i * i <= 100000; ++i) {
            if (sieve[i]) {
                for (uint64_t j = i * i; j <= 100000; j += i) sieve[j] = false;
            }
        }
        for (uint32_t i = 10000; i <= 100000; ++i) {
            if (sieve[i]) ps.push_back(i);
        }
        return ps;
    }();

    std::vector<Integer> stream;
    stream.reserve(count);
    std::mt19937_64 rng(seed);
    for (size_t i = 0; i < count; ++i) {
        uint32_t p = small_primes[rng() % small_primes.size()];
        uint32_t q = small_primes[rng() % small_primes.size()];
        if (p == q) ++q;  // avoid perfect squares
        uint64_t pq = static_cast<uint64_t>(p) * q;
        stream.emplace_back(static_cast<unsigned long long>(pq));
    }
    return stream;
}

struct BenchResult {
    double total_ms = 0.0;
    size_t found = 0;
    double ms_per_call() const noexcept { return total_ms / std::max(1.0, total_ms > 0 ? total_ms / total_ms : 1.0); }
};

// Bench: current quick_factor (uses thread_local BatchContext cache)
BenchResult bench_quick_factor(const std::vector<Integer>& stream) {
    BenchResult br;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (const auto& n : stream) {
        auto r = ECM::quick_factor(n);
        if (r) ++br.found;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    br.total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return br;
}

// Bench: explicit factor() path (also routes through BatchContext now, but
// always rebuilds primes_cache + prime_powers per call)
BenchResult bench_factor_no_cache(const std::vector<Integer>& stream) {
    BenchResult br;
    ECM::Config cfg;
    cfg.num_curves = 10;
    cfg.B1 = 2000;
    cfg.B2 = 50000;
    cfg.auto_params = false;

    auto t0 = std::chrono::high_resolution_clock::now();
    for (const auto& n : stream) {
        auto r = ECM::factor(n, cfg);
        if (r) ++br.found;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    br.total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return br;
}

// Bench: batch API — single prepare_batch + factor_batch
BenchResult bench_batch_api(const std::vector<Integer>& stream) {
    BenchResult br;
    ECM::Config cfg;
    cfg.num_curves = 10;
    cfg.B1 = 2000;
    cfg.B2 = 50000;
    cfg.auto_params = false;

    auto ctx = ECM::prepare_batch(cfg, /*sigma_seed=*/0xCAFEBABEULL);

    auto t0 = std::chrono::high_resolution_clock::now();
    auto rs = ECM::factor_batch(std::span<const Integer>(stream.data(), stream.size()), ctx);
    auto t1 = std::chrono::high_resolution_clock::now();
    br.total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    for (const auto& r : rs) {
        if (r) ++br.found;
    }
    return br;
}

}  // namespace

int main(int argc, char** argv) {
    size_t count = (argc > 1) ? static_cast<size_t>(std::atoll(argv[1])) : 2000;
    uint64_t seed = (argc > 2) ? static_cast<uint64_t>(std::atoll(argv[2])) : 12345ULL;

    std::cout << "═════════════════════════════════════════════" << std::endl;
    std::cout << "  Batch ECM Micro-benchmark" << std::endl;
    std::cout << "═════════════════════════════════════════════" << std::endl;
    std::cout << "  cofactor count: " << count << std::endl;
    std::cout << "  seed:           " << seed << std::endl << std::endl;

    auto stream = generate_cofactor_stream(count, seed);

    // Warmup: prime thread_local cache + warm the CPU
    (void)bench_quick_factor(std::vector<Integer>(stream.begin(),
                                                   stream.begin() + std::min<size_t>(50, count)));

    std::cout << "Running bench..." << std::endl;
    auto qf = bench_quick_factor(stream);
    auto bf = bench_batch_api(stream);
    auto ff = bench_factor_no_cache(stream);

    std::cout << std::endl;
    std::cout << "Results:" << std::endl;
    std::cout << "  quick_factor      total=" << qf.total_ms << " ms"
              << " per_call=" << (qf.total_ms / count) << " ms"
              << " found=" << qf.found << std::endl;
    std::cout << "  batch_api         total=" << bf.total_ms << " ms"
              << " per_call=" << (bf.total_ms / count) << " ms"
              << " found=" << bf.found << std::endl;
    std::cout << "  factor (per-call) total=" << ff.total_ms << " ms"
              << " per_call=" << (ff.total_ms / count) << " ms"
              << " found=" << ff.found << std::endl;
    std::cout << std::endl;

    double qf_vs_ff = ff.total_ms > 0 ? (qf.total_ms / ff.total_ms) : 1.0;
    double bf_vs_ff = ff.total_ms > 0 ? (bf.total_ms / ff.total_ms) : 1.0;
    std::cout << "Speedup vs factor() (lower is better):" << std::endl;
    std::cout << "  quick_factor/factor = " << qf_vs_ff << " (saving "
              << ((1.0 - qf_vs_ff) * 100.0) << "%)" << std::endl;
    std::cout << "  batch_api/factor    = " << bf_vs_ff << " (saving "
              << ((1.0 - bf_vs_ff) * 100.0) << "%)" << std::endl;

    return 0;
}
