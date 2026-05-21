// test_batch_trial.cpp — Unit tests for batch trial division (T5 Stage 1).
//
// Covers:
//   - ENV parser (GNFS_COFACTOR_BATCH_SIZE)
//   - K=1 single cofactor parity with naive trial division
//   - K=4 mixed smooth + non-smooth batch
//   - boundary: empty input, all-smooth, all-non-smooth, zero, one
//   - K=128 large batch (no hang, correctness preserved)
//   - large primes within bound vs prime > bound (proper residual)
//   - GMP-path big-integer cofactor (>uint64_max) still correctly stripped
//   - thread-safety: parallel batch_trial_divide calls produce identical
//     results to sequential calls

#include <gnfs/cofactor/batch_trial.hpp>
#include <gnfs/core/integer.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <numeric>
#include <random>
#include <span>
#include <thread>
#include <vector>

using gnfs::core::Integer;
using gnfs::cofactor::BatchTrialResult;
using gnfs::cofactor::batch_trial_divide;
using gnfs::cofactor::batch_trial_size_from_env;

namespace {

// Naive baseline: trial-divide one cofactor against the prime list up to
// prime_bound. Used as ground truth for the batch path.
struct NaiveResult {
    bool is_smooth;
    Integer remaining;
};

NaiveResult naive_trial_divide(Integer value, size_t prime_bound) {
    if (value.is_negative()) value.negate();
    if (value.fits_uint64() && value.to_uint64() == 0) {
        return {true, Integer{uint64_t{1}}};
    }

    // Local sieve (independent from the cached one to avoid coupling).
    std::vector<bool> is_prime(prime_bound + 1, true);
    if (prime_bound >= 2) {
        is_prime[0] = is_prime[1] = false;
        for (size_t i = 2; i * i <= prime_bound; ++i) {
            if (is_prime[i]) {
                for (size_t j = i * i; j <= prime_bound; j += i) {
                    is_prime[j] = false;
                }
            }
        }
    }

    for (size_t p = 2; p <= prime_bound; ++p) {
        if (!is_prime[p]) continue;
        if (value.fits_uint64()) {
            uint64_t v = value.to_uint64();
            if (v == 1) break;
            while (v != 0 && (v % p) == 0) v /= p;
            value = v;
        } else {
            while (mpz_divisible_ui_p(value.get_mpz(), p) != 0) {
                mpz_divexact_ui(value.get_mpz(), value.get_mpz(), p);
            }
        }
    }

    bool smooth = value.fits_uint64() && value.to_uint64() == 1;
    return {smooth, std::move(value)};
}

// ───────────────────────────────────────────────────────────────────────────
// Test 1: ENV parser
// ───────────────────────────────────────────────────────────────────────────
void test_env_parser() {
    std::cout << "Test 1: env parser..." << std::flush;

    // Unset
    unsetenv("GNFS_COFACTOR_BATCH_SIZE");
    assert(batch_trial_size_from_env() == 1);

    // Empty string
    setenv("GNFS_COFACTOR_BATCH_SIZE", "", 1);
    assert(batch_trial_size_from_env() == 1);

    // Junk
    setenv("GNFS_COFACTOR_BATCH_SIZE", "abc", 1);
    assert(batch_trial_size_from_env() == 1);

    // Zero / one (treated as disabled)
    setenv("GNFS_COFACTOR_BATCH_SIZE", "0", 1);
    assert(batch_trial_size_from_env() == 1);
    setenv("GNFS_COFACTOR_BATCH_SIZE", "1", 1);
    assert(batch_trial_size_from_env() == 1);

    // Normal values
    setenv("GNFS_COFACTOR_BATCH_SIZE", "2", 1);
    assert(batch_trial_size_from_env() == 2);
    setenv("GNFS_COFACTOR_BATCH_SIZE", "16", 1);
    assert(batch_trial_size_from_env() == 16);

    // Overflow / cap
    setenv("GNFS_COFACTOR_BATCH_SIZE", "999999", 1);
    assert(batch_trial_size_from_env() == 4096);

    unsetenv("GNFS_COFACTOR_BATCH_SIZE");

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 2: K=1 parity vs naive trial division (single cofactor)
// ───────────────────────────────────────────────────────────────────────────
void test_k1_parity_single() {
    std::cout << "Test 2: K=1 parity vs naive..." << std::flush;

    const size_t prime_bound = 100;
    const std::vector<uint64_t> samples = {
        1, 2, 3, 4, 6, 12, 30, 60, 97, 100, 101, 121, 210, 211, 999, 1000,
        1001, 9973, 9999, 100001
    };

    for (uint64_t s : samples) {
        std::vector<Integer> in = { Integer{s} };
        BatchTrialResult br = batch_trial_divide(in, prime_bound);
        NaiveResult nr = naive_trial_divide(Integer{s}, prime_bound);

        assert(br.size() == 1);
        assert(br.is_smooth[0] == nr.is_smooth);
        assert(br.remaining[0].compare(nr.remaining) == 0);
    }

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 3: K=4 mixed batch (some smooth, some non-smooth)
// ───────────────────────────────────────────────────────────────────────────
void test_k4_mixed() {
    std::cout << "Test 3: K=4 mixed batch..." << std::flush;

    const size_t prime_bound = 100;
    // Hand-crafted:
    //  - 30  = 2 * 3 * 5            → smooth wrt B=100
    //  - 210 = 2 * 3 * 5 * 7        → smooth wrt B=100
    //  - 101 (prime > 100? No, 101 > 100) → not smooth, remaining 101
    //  - 12 * 9973 = 119676; 9973 prime > 100 → not smooth, remaining 9973
    std::vector<Integer> in = {
        Integer{uint64_t{30}},
        Integer{uint64_t{210}},
        Integer{uint64_t{101}},
        Integer{uint64_t{119676}},
    };

    BatchTrialResult br = batch_trial_divide(in, prime_bound);

    assert(br.size() == 4);
    assert(br.is_smooth[0] == true);
    assert(br.remaining[0].to_uint64() == 1);

    assert(br.is_smooth[1] == true);
    assert(br.remaining[1].to_uint64() == 1);

    assert(br.is_smooth[2] == false);
    assert(br.remaining[2].to_uint64() == 101);

    assert(br.is_smooth[3] == false);
    assert(br.remaining[3].to_uint64() == 9973);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 4: empty input
// ───────────────────────────────────────────────────────────────────────────
void test_empty_input() {
    std::cout << "Test 4: empty input..." << std::flush;

    std::vector<Integer> in;
    BatchTrialResult br = batch_trial_divide(in, 100);

    assert(br.empty());
    assert(br.size() == 0);
    assert(br.is_smooth.empty());
    assert(br.remaining.empty());

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 5: all smooth (every cofactor a B-smooth number)
// ───────────────────────────────────────────────────────────────────────────
void test_all_smooth() {
    std::cout << "Test 5: all smooth..." << std::flush;

    const size_t prime_bound = 100;
    std::vector<Integer> in = {
        Integer{uint64_t{2}},
        Integer{uint64_t{4}},
        Integer{uint64_t{8}},
        Integer{uint64_t{30}},   // 2*3*5
        Integer{uint64_t{60}},   // 2^2*3*5
        Integer{uint64_t{2310}}, // 2*3*5*7*11
    };

    BatchTrialResult br = batch_trial_divide(in, prime_bound);

    for (size_t i = 0; i < br.size(); ++i) {
        assert(br.is_smooth[i] == true);
        assert(br.remaining[i].to_uint64() == 1);
    }

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 6: all non-smooth (every cofactor a single prime > bound)
// ───────────────────────────────────────────────────────────────────────────
void test_all_nonsmooth() {
    std::cout << "Test 6: all non-smooth..." << std::flush;

    const size_t prime_bound = 100;
    std::vector<Integer> in = {
        Integer{uint64_t{101}},
        Integer{uint64_t{103}},
        Integer{uint64_t{211}},
        Integer{uint64_t{9973}},
    };

    BatchTrialResult br = batch_trial_divide(in, prime_bound);

    for (size_t i = 0; i < br.size(); ++i) {
        assert(br.is_smooth[i] == false);
    }
    assert(br.remaining[0].to_uint64() == 101);
    assert(br.remaining[1].to_uint64() == 103);
    assert(br.remaining[2].to_uint64() == 211);
    assert(br.remaining[3].to_uint64() == 9973);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 7: zero and one boundaries
// ───────────────────────────────────────────────────────────────────────────
void test_zero_and_one() {
    std::cout << "Test 7: zero + one boundaries..." << std::flush;

    std::vector<Integer> in = {
        Integer{uint64_t{0}},
        Integer{uint64_t{1}},
        Integer{uint64_t{2}},
    };
    BatchTrialResult br = batch_trial_divide(in, 100);

    // 0 → treat as smooth, remaining becomes 1
    assert(br.is_smooth[0] == true);
    assert(br.remaining[0].to_uint64() == 1);
    // 1 → smooth, remaining 1
    assert(br.is_smooth[1] == true);
    assert(br.remaining[1].to_uint64() == 1);
    // 2 → smooth, remaining 1
    assert(br.is_smooth[2] == true);
    assert(br.remaining[2].to_uint64() == 1);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 8: large K=128 (no hang, correctness preserved)
// ───────────────────────────────────────────────────────────────────────────
void test_large_k128() {
    std::cout << "Test 8: K=128 large batch..." << std::flush;

    const size_t prime_bound = 1000;
    const size_t K = 128;

    std::mt19937_64 rng(12345);
    std::vector<Integer> in;
    in.reserve(K);
    for (size_t i = 0; i < K; ++i) {
        uint64_t v = (rng() % (1ULL << 40)) + 1;
        in.push_back(Integer{v});
    }

    auto t0 = std::chrono::steady_clock::now();
    BatchTrialResult br = batch_trial_divide(in, prime_bound);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    assert(br.size() == K);

    // Spot-check each against naive.
    for (size_t i = 0; i < K; ++i) {
        NaiveResult nr = naive_trial_divide(in[i], prime_bound);
        assert(br.is_smooth[i] == nr.is_smooth);
        assert(br.remaining[i].compare(nr.remaining) == 0);
    }

    std::cout << " PASS (" << ms << " ms)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 9: GMP-path big-integer cofactor (> uint64_max)
// ───────────────────────────────────────────────────────────────────────────
void test_big_integer_path() {
    std::cout << "Test 9: GMP-path big integer..." << std::flush;

    // Construct 2^70 * 3^10 * 5^5 — smooth wrt B=100
    Integer smooth_big{uint64_t{1}};
    for (int i = 0; i < 70; ++i) smooth_big *= int64_t{2};
    for (int i = 0; i < 10; ++i) smooth_big *= int64_t{3};
    for (int i = 0; i < 5; ++i)  smooth_big *= int64_t{5};

    // Construct (2^70 * 3^10 * 5^5) * 9973 — not smooth, remaining = 9973
    Integer nonsmooth_big = smooth_big;
    nonsmooth_big *= int64_t{9973};

    std::vector<Integer> in = { smooth_big, nonsmooth_big };
    BatchTrialResult br = batch_trial_divide(in, 100);

    assert(br.is_smooth[0] == true);
    assert(br.remaining[0].to_uint64() == 1);

    assert(br.is_smooth[1] == false);
    assert(br.remaining[1].to_uint64() == 9973);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 10: thread-safety — parallel calls produce identical results
// ───────────────────────────────────────────────────────────────────────────
void test_thread_safety() {
    std::cout << "Test 10: thread-safety..." << std::flush;

    const size_t prime_bound = 500;
    const size_t K = 32;
    const size_t num_threads = 8;

    std::mt19937_64 rng(0xCAFEBABEULL);
    std::vector<Integer> in;
    in.reserve(K);
    for (size_t i = 0; i < K; ++i) {
        in.push_back(Integer{(rng() % (1ULL << 30)) + 1});
    }

    // Sequential ground truth.
    BatchTrialResult gold = batch_trial_divide(in, prime_bound);

    // Parallel re-runs.
    std::vector<std::thread> ths;
    std::vector<BatchTrialResult> results(num_threads);
    for (size_t t = 0; t < num_threads; ++t) {
        ths.emplace_back([&, t]() {
            results[t] = batch_trial_divide(in, prime_bound);
        });
    }
    for (auto& th : ths) th.join();

    for (size_t t = 0; t < num_threads; ++t) {
        assert(results[t].size() == gold.size());
        for (size_t i = 0; i < gold.size(); ++i) {
            assert(results[t].is_smooth[i] == gold.is_smooth[i]);
            assert(results[t].remaining[i].compare(gold.remaining[i]) == 0);
        }
    }

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 11: prime_bound = 0 / 1 (degenerate sieve, nothing stripped)
// ───────────────────────────────────────────────────────────────────────────
void test_degenerate_bound() {
    std::cout << "Test 11: degenerate prime_bound..." << std::flush;

    std::vector<Integer> in = {
        Integer{uint64_t{1}},
        Integer{uint64_t{6}},   // 2*3, not stripped → not smooth
    };

    BatchTrialResult br0 = batch_trial_divide(in, 0);
    assert(br0.size() == 2);
    assert(br0.is_smooth[0] == true);             // 1 trivially smooth
    assert(br0.remaining[0].to_uint64() == 1);
    assert(br0.is_smooth[1] == false);            // nothing stripped
    assert(br0.remaining[1].to_uint64() == 6);

    BatchTrialResult br1 = batch_trial_divide(in, 1);
    assert(br1.is_smooth[1] == false);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 12: negative cofactor input (absolute value used)
// ───────────────────────────────────────────────────────────────────────────
void test_negative_input() {
    std::cout << "Test 12: negative input..." << std::flush;

    Integer neg30{int64_t{-30}};
    Integer neg101{int64_t{-101}};

    std::vector<Integer> in = { neg30, neg101 };
    BatchTrialResult br = batch_trial_divide(in, 100);

    assert(br.is_smooth[0] == true);   // |-30| = 30 = 2*3*5
    assert(br.remaining[0].to_uint64() == 1);

    assert(br.is_smooth[1] == false);  // |-101| = 101 > 100
    assert(br.remaining[1].to_uint64() == 101);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 13: random soak vs naive (large mixed batch)
// ───────────────────────────────────────────────────────────────────────────
void test_random_soak() {
    std::cout << "Test 13: random soak (200 iters)..." << std::flush;

    std::mt19937_64 rng(0xDEADBEEFULL);
    const size_t K = 50;
    const size_t prime_bound = 200;

    for (int iter = 0; iter < 200; ++iter) {
        std::vector<Integer> in;
        in.reserve(K);
        for (size_t i = 0; i < K; ++i) {
            uint64_t v = (rng() % (1ULL << 35)) + 1;
            in.push_back(Integer{v});
        }
        BatchTrialResult br = batch_trial_divide(in, prime_bound);
        for (size_t i = 0; i < K; ++i) {
            NaiveResult nr = naive_trial_divide(in[i], prime_bound);
            if (br.is_smooth[i] != nr.is_smooth
                || br.remaining[i].compare(nr.remaining) != 0) {
                std::cerr << "\nMISMATCH iter=" << iter << " i=" << i
                          << " in=" << in[i].to_string()
                          << " br=(" << br.is_smooth[i] << "," << br.remaining[i].to_string() << ")"
                          << " nr=(" << nr.is_smooth << "," << nr.remaining.to_string() << ")\n";
                assert(false);
            }
        }
    }

    std::cout << " PASS\n";
}

} // namespace

int main() {
    std::cout << "=== test_batch_trial ===\n";

    test_env_parser();
    test_k1_parity_single();
    test_k4_mixed();
    test_empty_input();
    test_all_smooth();
    test_all_nonsmooth();
    test_zero_and_one();
    test_large_k128();
    test_big_integer_path();
    test_thread_safety();
    test_degenerate_bound();
    test_negative_input();
    test_random_soak();

    std::cout << "=== All tests passed ===\n";
    return 0;
}
