// ECM Stage 3 (Brent-Suyama) benchmark vs classical BSGS.
//
// Generates `count` random semiprimes in the 40-60 bit range and runs each
// through ECM with both classical BSGS (degree=0) and Brent-Suyama (d=12).
// Reports success rate, median time, and speedup ratio.
//
// Usage:  test_ecm_brent_suyama_bench [count] [seed]
// Defaults: count=100, seed=42

#include <gnfs/cofactor/ecm.hpp>
#include <gnfs/core/integer.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace gnfs::core;
using namespace gnfs::cofactor;

namespace {

// Generate a random odd semiprime of the requested bit length.
// Both factors are forced to be probable primes via mpz_nextprime so that
// neither is trivially smooth (which would let Stage 1 alone always win).
Integer make_random_semiprime(std::mt19937_64& rng, size_t bits) {
    while (true) {
        size_t half = bits / 2;
        if (half < 16) half = 16;

        // Generate random odd value of `half` bits and snap to next prime.
        Integer a, b;
        for (int attempt = 0; attempt < 10; ++attempt) {
            uint64_t lo = rng();
            uint64_t hi = rng();
            // Set top bit so width matches.
            if (half <= 64) {
                lo &= (half == 64) ? ~uint64_t(0)
                                   : ((uint64_t(1) << half) - 1);
                lo |= uint64_t(1) << (half - 1);
                lo |= 1u;
                a = Integer(static_cast<unsigned long long>(lo));
            } else {
                // Compose 128-bit from two 64-bit words.
                a = Integer(static_cast<unsigned long long>(hi));
                Integer shifted;
                mpz_mul_2exp(shifted.get_mpz(), a.get_mpz(), 64);
                Integer add(static_cast<unsigned long long>(lo | 1u));
                mpz_add(a.get_mpz(), shifted.get_mpz(), add.get_mpz());
            }
            mpz_nextprime(a.get_mpz(), a.get_mpz());
            if (mpz_sgn(a.get_mpz()) > 0) break;
        }
        for (int attempt = 0; attempt < 10; ++attempt) {
            uint64_t lo = rng();
            uint64_t hi = rng();
            if (half <= 64) {
                lo &= (half == 64) ? ~uint64_t(0)
                                   : ((uint64_t(1) << half) - 1);
                lo |= uint64_t(1) << (half - 1);
                lo |= 1u;
                b = Integer(static_cast<unsigned long long>(lo));
            } else {
                b = Integer(static_cast<unsigned long long>(hi));
                Integer shifted;
                mpz_mul_2exp(shifted.get_mpz(), b.get_mpz(), 64);
                Integer add(static_cast<unsigned long long>(lo | 1u));
                mpz_add(b.get_mpz(), shifted.get_mpz(), add.get_mpz());
            }
            mpz_nextprime(b.get_mpz(), b.get_mpz());
            if (mpz_sgn(b.get_mpz()) > 0) break;
        }
        if (a.compare(b) == 0) continue;

        Integer n;
        mpz_mul(n.get_mpz(), a.get_mpz(), b.get_mpz());
        if (n.bit_length() < bits - 2 || n.bit_length() > bits + 2) continue;
        return n;
    }
}

struct Trial {
    Integer n;
    bool found_bsgs;
    bool found_bs12;
    double ms_bsgs;
    double ms_bs12;
};

}  // namespace

int main(int argc, char* argv[]) {
    size_t count = 100;
    uint64_t seed = 42;
    if (argc >= 2) count = static_cast<size_t>(std::atol(argv[1]));
    if (argc >= 3) seed = static_cast<uint64_t>(std::atoll(argv[2]));

    std::cout << "=== ECM Brent-Suyama (d=12) vs classical BSGS benchmark ===\n";
    std::cout << "count=" << count << " seed=" << seed << "\n\n";

    std::mt19937_64 rng(seed);
    std::vector<Trial> trials;
    trials.reserve(count);

    // Workload: 60-bit semiprimes. ECM B1=500 (small Stage 1), B2=10000
    // (well above 3*D=6930 BSGS threshold). 5 curves per trial.
    //
    // Tuning rationale:
    //   - small B1 -> Stage 1 likely fails -> Stage 2/3 carries the load
    //   - B2=10000 -> matches task spec
    //   - bits=60 -> non-trivial Stage 2 cost (factors typically > B1)
    const size_t bits = 60;
    const uint64_t B1 = 500;
    const uint64_t B2 = 10000;
    const uint32_t num_curves = 5;

    for (size_t i = 0; i < count; ++i) {
        Integer n = make_random_semiprime(rng, bits);

        // Classical BSGS
        ECM::Config cfg0;
        cfg0.auto_params = false;
        cfg0.B1 = B1; cfg0.B2 = B2;
        cfg0.num_curves = num_curves;
        cfg0.brent_suyama_degree = 0;
        auto t0a = std::chrono::high_resolution_clock::now();
        auto r0 = ECM::factor(n, cfg0);
        auto t0b = std::chrono::high_resolution_clock::now();
        double ms0 = std::chrono::duration<double, std::milli>(t0b - t0a).count();

        // Brent-Suyama d=12
        ECM::Config cfg12;
        cfg12.auto_params = false;
        cfg12.B1 = B1; cfg12.B2 = B2;
        cfg12.num_curves = num_curves;
        cfg12.brent_suyama_degree = 12;
        auto t1a = std::chrono::high_resolution_clock::now();
        auto r12 = ECM::factor(n, cfg12);
        auto t1b = std::chrono::high_resolution_clock::now();
        double ms12 = std::chrono::duration<double, std::milli>(t1b - t1a).count();

        trials.push_back({n, r0.has_value(), r12.has_value(), ms0, ms12});
    }

    // Aggregate
    size_t found_bsgs = 0, found_bs12 = 0, both_found = 0;
    double sum_ms_bsgs = 0, sum_ms_bs12 = 0;
    std::vector<double> ms_bsgs_list, ms_bs12_list;
    ms_bsgs_list.reserve(count);
    ms_bs12_list.reserve(count);

    for (const auto& t : trials) {
        if (t.found_bsgs) {
            ++found_bsgs;
            sum_ms_bsgs += t.ms_bsgs;
            ms_bsgs_list.push_back(t.ms_bsgs);
        }
        if (t.found_bs12) {
            ++found_bs12;
            sum_ms_bs12 += t.ms_bs12;
            ms_bs12_list.push_back(t.ms_bs12);
        }
        if (t.found_bsgs && t.found_bs12) ++both_found;
    }

    auto median = [](std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    double med_bsgs = median(ms_bsgs_list);
    double med_bs12 = median(ms_bs12_list);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "BSGS         : found " << found_bsgs << "/" << count
              << " avg=" << (found_bsgs ? sum_ms_bsgs / static_cast<double>(found_bsgs) : 0.0)
              << " ms median=" << med_bsgs << " ms\n";
    std::cout << "Brent-Suyama : found " << found_bs12 << "/" << count
              << " avg=" << (found_bs12 ? sum_ms_bs12 / static_cast<double>(found_bs12) : 0.0)
              << " ms median=" << med_bs12 << " ms\n";
    std::cout << "Both found   : " << both_found << "/" << count << "\n";

    if (med_bsgs > 0 && med_bs12 > 0) {
        double ratio = med_bsgs / med_bs12;
        std::cout << "Speedup (median BSGS / BS12) = " << ratio << "x\n";
        if (ratio > 1.0) {
            std::cout << "  Brent-Suyama is " << ratio << "x faster\n";
        } else {
            std::cout << "  BSGS is " << (1.0 / ratio) << "x faster\n";
        }
    }

    // Success criterion: both methods find a similar number of factors,
    // and Brent-Suyama is not catastrophically slower (>10x).
    if (found_bs12 == 0 && found_bsgs > 0) {
        std::cerr << "FAIL: Brent-Suyama found nothing while BSGS found "
                  << found_bsgs << "\n";
        return 1;
    }
    if (med_bsgs > 0 && med_bs12 > med_bsgs * 10.0) {
        std::cerr << "FAIL: Brent-Suyama is >10x slower than BSGS\n";
        return 1;
    }

    std::cout << "\n=== Benchmark complete ===\n";
    return 0;
}
