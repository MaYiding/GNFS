// test_brent_pollard_rho_parallel.cpp -- batched Brent-Pollard rho parallel
// dispatcher tests
//
// Validates the GNFS_BRENT_POLLARD_RHO_THREADS env-gated dispatcher
// introduced in include/gnfs/cofactor/brent_pollard_rho_parallel.hpp:
//
//   * ENV parsing handles unset / "0" / "1" / "4" / "garbage" / "" /
//     leading whitespace / "12abc" / "9999" correctly; clamping at
//     hardware_concurrency() * 2.
//   * Sequential (N=1, default) and parallel (N>=2) paths produce per-index
//     bit-identical results for the same (cs, x0s) input. The dispatcher
//     is a pure parallel wrapper around a caller-supplied `worker_fn`.
//   * Empty input vectors return cleanly without creating a pool or
//     invoking `worker_fn` at all.
//   * Single config under N>=2 short-circuits to sequential (exactly-once
//     `worker_fn` invocation, no stall).
//   * 100-config and 1000-config mock parity at N=1 vs N=4 / N=hw.
//   * Real Brent-Pollard rho integration: 50 composite cofactors, N=1 vs
//     N=4 per-config `std::optional<Integer>` factor identical.
//   * Mismatched span sizes throw `std::invalid_argument`.
//   * Cache reset hook re-parses ENV between assertions.
//   * Perf-info probe (informational, no assert).

#include <gnfs/cofactor/brent_pollard_rho.hpp>
#include <gnfs/cofactor/brent_pollard_rho_parallel.hpp>
#include <gnfs/core/integer.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using gnfs::cofactor::brent_pollard_rho_threads;
using gnfs::cofactor::brent_pollard_rho_threads_reset_env_cache_for_testing;
using gnfs::cofactor::BrentPollardRho;
using gnfs::cofactor::parallel_brent_pollard_rho;
using gnfs::cofactor::resolve_brent_pollard_rho_threads;
using gnfs::core::Integer;

namespace {

// Helper: set or unset GNFS_BRENT_POLLARD_RHO_THREADS and refresh the
// cache so the next call to brent_pollard_rho_threads() reflects the new
// value.
void apply_env(const char* value) {
    if (value == nullptr) {
        unsetenv("GNFS_BRENT_POLLARD_RHO_THREADS");
    } else {
        setenv("GNFS_BRENT_POLLARD_RHO_THREADS", value, /*overwrite=*/1);
    }
    brent_pollard_rho_threads_reset_env_cache_for_testing();
}

// ---------------------------------------------------------------------------
// Test 1: ENV unset -> 1 (default sequential)
// ---------------------------------------------------------------------------
void test_env_unset_defaults_to_one() {
    std::cout << "Test 1: ENV unset -> 1..." << std::flush;
    apply_env(nullptr);
    int v = brent_pollard_rho_threads();
    if (v != 1) {
        std::cerr << "\n  ERROR: unset env parsed to " << v << ", expected 1" << std::endl;
        std::abort();
    }
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 2: ENV "0" -> 1, ENV "1" -> 1
// ---------------------------------------------------------------------------
void test_env_zero_and_one() {
    std::cout << "Test 2: ENV '0' / '1' -> 1..." << std::flush;
    apply_env("0");
    assert(brent_pollard_rho_threads() == 1);

    apply_env("1");
    assert(brent_pollard_rho_threads() == 1);

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 3: ENV "4" -> 4 (or clamped to cap)
// ---------------------------------------------------------------------------
void test_env_four() {
    std::cout << "Test 3: ENV '4' -> 4..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    int cap = static_cast<int>(hw) * 2;
    int expect = (4 < cap) ? 4 : cap;

    apply_env("4");
    int v = brent_pollard_rho_threads();
    if (v != expect) {
        std::cerr << "\n  ERROR: '4' parsed to " << v << ", expected " << expect
                  << " (hw*2 cap = " << cap << ")" << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (parsed " << v << ", cap " << cap << ")\n";
}

// ---------------------------------------------------------------------------
// Test 4: ENV non-numeric / boundary -> 1
// ---------------------------------------------------------------------------
void test_env_non_numeric() {
    std::cout << "Test 4: ENV non-numeric / boundary -> 1..." << std::flush;

    apply_env("");
    assert(brent_pollard_rho_threads() == 1);

    apply_env("-5");
    assert(brent_pollard_rho_threads() == 1);

    apply_env("garbage");
    assert(brent_pollard_rho_threads() == 1);

    apply_env("abc123");
    assert(brent_pollard_rho_threads() == 1);

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 5: ENV "12abc" partial parse -> 12 (atoi accepts leading numeric
// prefix; document the behaviour but callers should pass clean values).
// ENV "  4" leading whitespace -> 4 (atoi consumes leading whitespace).
// ---------------------------------------------------------------------------
void test_env_partial_parse() {
    std::cout << "Test 5: ENV partial-parse ('12abc' / '  4')..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    int cap = static_cast<int>(hw) * 2;

    apply_env("12abc");
    int v12 = brent_pollard_rho_threads();
    int expect12 = (12 < cap) ? 12 : cap;
    if (v12 != expect12) {
        std::cerr << "\n  ERROR: '12abc' parsed to " << v12 << ", expected " << expect12
                  << std::endl;
        std::abort();
    }

    apply_env("  4");
    int v4 = brent_pollard_rho_threads();
    int expect4 = (4 < cap) ? 4 : cap;
    if (v4 != expect4) {
        std::cerr << "\n  ERROR: '  4' parsed to " << v4 << ", expected " << expect4 << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 6: ENV "10000" -> clamped to hw_concurrency * 2
// ---------------------------------------------------------------------------
void test_env_clamp_high() {
    std::cout << "Test 6: ENV '10000' clamp to hw*2..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    int cap = static_cast<int>(hw) * 2;

    apply_env("10000");
    int v = brent_pollard_rho_threads();
    if (v != cap) {
        std::cerr << "\n  ERROR: '10000' should clamp to " << cap << " got " << v << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS (clamped to " << cap << ")\n";
}

// ---------------------------------------------------------------------------
// Test 7: Positive values above INT_MAX still clamp instead of becoming
// negative through an overflowing atoi conversion.
// ---------------------------------------------------------------------------
void test_env_positive_overflow_clamps() {
    std::cout << "Test 7: oversized positive ENV values clamp..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    int cap = static_cast<int>(hw) * 2;

    for (const char* value : {"2147483648", "999999999999999999999999999999"}) {
        apply_env(value);
        int parsed = brent_pollard_rho_threads();
        if (parsed != cap) {
            std::cerr << "\n  ERROR: '" << value << "' parsed to " << parsed
                      << ", expected clamp=" << cap << std::endl;
            std::abort();
        }
    }

    apply_env(nullptr);
    std::cout << " PASS (clamped to " << cap << ")\n";
}

// ---------------------------------------------------------------------------
// Test 8: Empty input spans return empty results, no pool created, worker
// never invoked.
// ---------------------------------------------------------------------------
void test_empty_span() {
    std::cout << "Test 7: empty input spans (no-op both paths)..." << std::flush;

    std::vector<uint64_t> empty;

    apply_env("1");
    auto seq = parallel_brent_pollard_rho<uint64_t>(
        std::span<const uint64_t>(empty), std::span<const uint64_t>(empty),
        [](uint64_t, uint64_t) -> uint64_t {
            std::cerr << "\n  ERROR: should not invoke worker on empty span" << std::endl;
            std::abort();
            return 0;
        });
    assert(seq.empty());

    apply_env("4");
    auto par = parallel_brent_pollard_rho<uint64_t>(
        std::span<const uint64_t>(empty), std::span<const uint64_t>(empty),
        [](uint64_t, uint64_t) -> uint64_t {
            std::cerr << "\n  ERROR: should not invoke worker on empty span" << std::endl;
            std::abort();
            return 0;
        });
    assert(par.empty());

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 8: Single config N=1 -- exactly-once invocation
// ---------------------------------------------------------------------------
void test_single_config_n1() {
    std::cout << "Test 8: single config N=1 (exactly-once)..." << std::flush;
    apply_env("1");

    std::vector<uint64_t> cs = {7};
    std::vector<uint64_t> x0s = {2};
    std::atomic<int> calls{0};

    auto results = parallel_brent_pollard_rho<uint64_t>(
        std::span<const uint64_t>(cs), std::span<const uint64_t>(x0s),
        [&calls](uint64_t c, uint64_t x0) -> uint64_t {
            calls.fetch_add(1, std::memory_order_relaxed);
            return c * 1000 + x0;
        });

    assert(results.size() == 1);
    assert(results[0] == 7002);
    assert(calls.load() == 1);

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 9: Single config N=4 -- must short-circuit to sequential, exactly-once
// invocation, no stall.
// ---------------------------------------------------------------------------
void test_single_config_n4_no_stall() {
    std::cout << "Test 9: single config N=4 (no stall, exactly-once)..." << std::flush;
    apply_env("4");

    std::vector<uint64_t> cs = {5};
    std::vector<uint64_t> x0s = {3};
    std::atomic<int> calls{0};

    auto t0 = std::chrono::steady_clock::now();
    auto results = parallel_brent_pollard_rho<uint64_t>(
        std::span<const uint64_t>(cs), std::span<const uint64_t>(x0s),
        [&calls](uint64_t c, uint64_t x0) -> uint64_t {
            calls.fetch_add(1, std::memory_order_relaxed);
            return c * 100 + x0;
        });
    auto t1 = std::chrono::steady_clock::now();
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    assert(results.size() == 1);
    assert(results[0] == 503);
    int total = calls.load(std::memory_order_relaxed);
    if (total != 1) {
        std::cerr << "\n  ERROR: expected exactly 1 call, got " << total << std::endl;
        std::abort();
    }
    if (ms > 1000) {
        std::cerr << "\n  WARN: single-config dispatch took " << ms << " ms (expected << 1000 ms)"
                  << std::endl;
        // Soft signal; sanitizers can be slow but should not exceed 1s
        // on a single-call short-circuit path.
    }

    apply_env(nullptr);
    std::cout << " PASS (" << ms << " ms)\n";
}

// ---------------------------------------------------------------------------
// Test 10: N=1 baseline with mock worker, no GMP. Validates dispatcher
// lambda dispatch + result aggregation.
// ---------------------------------------------------------------------------
void test_n1_baseline_mock() {
    std::cout << "Test 10: N=1 baseline mock worker..." << std::flush;
    apply_env("1");

    std::vector<uint64_t> cs;
    std::vector<uint64_t> x0s;
    for (uint64_t i = 0; i < 20; ++i) {
        cs.push_back(i + 1);
        x0s.push_back(i * 2 + 5);
    }

    auto worker = [](uint64_t c, uint64_t x0) -> uint64_t {
        // Pure function: rotate / mix to ensure dispatcher index alignment.
        uint64_t acc = c;
        acc = (acc * 6364136223846793005ULL + x0);
        acc ^= (acc >> 13);
        return acc;
    };

    auto results = parallel_brent_pollard_rho<uint64_t>(std::span<const uint64_t>(cs),
                                                        std::span<const uint64_t>(x0s), worker);

    if (results.size() != cs.size()) {
        std::cerr << "\n  ERROR: expected " << cs.size() << " results, got " << results.size()
                  << std::endl;
        std::abort();
    }
    for (std::size_t i = 0; i < cs.size(); ++i) {
        uint64_t expect = worker(cs[i], x0s[i]);
        if (results[i] != expect) {
            std::cerr << "\n  ERROR: idx " << i << " got " << results[i] << " expected " << expect
                      << std::endl;
            std::abort();
        }
    }

    apply_env(nullptr);
    std::cout << " PASS (" << cs.size() << " results aligned)\n";
}

// ---------------------------------------------------------------------------
// Test 11: N=1 vs N=4 mock-worker parity, 100 configs.
// ---------------------------------------------------------------------------
void test_n1_vs_n4_mock_parity() {
    std::cout << "Test 11: N=1 vs N=4 mock parity (100 configs)..." << std::flush;

    auto worker = [](uint64_t c, uint64_t x0) -> uint64_t {
        // Pure function: a small computation that exercises dispatcher
        // index alignment without thread-local state.
        uint64_t acc = c;
        for (int k = 0; k < 32; ++k) {
            acc = (acc * 1103515245ULL + 12345ULL + x0) ^ (acc >> 7);
        }
        return acc;
    };

    std::vector<uint64_t> cs;
    std::vector<uint64_t> x0s;
    for (uint64_t i = 0; i < 100; ++i) {
        cs.push_back(i + 1);
        x0s.push_back(i * 3 + 5);
    }

    apply_env("1");
    auto seq = parallel_brent_pollard_rho<uint64_t>(std::span<const uint64_t>(cs),
                                                    std::span<const uint64_t>(x0s), worker);

    apply_env("4");
    auto par = parallel_brent_pollard_rho<uint64_t>(std::span<const uint64_t>(cs),
                                                    std::span<const uint64_t>(x0s), worker);

    apply_env(nullptr);

    if (seq.size() != par.size()) {
        std::cerr << "\n  ERROR: seq.size()=" << seq.size() << " par.size()=" << par.size()
                  << std::endl;
        std::abort();
    }
    for (std::size_t i = 0; i < seq.size(); ++i) {
        if (seq[i] != par[i]) {
            std::cerr << "\n  ERROR: idx " << i << " seq=" << seq[i] << " par=" << par[i]
                      << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (" << cs.size() << " per-index identical)\n";
}

// ---------------------------------------------------------------------------
// Test 12: N=1 vs N=hw_concurrency mock-worker parity, 1000 configs.
// ---------------------------------------------------------------------------
void test_n1_vs_n_hw_mock_parity() {
    std::cout << "Test 12: N=1 vs N=hw mock parity (1000 configs)..." << std::flush;

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    std::string hw_str = std::to_string(hw);

    auto worker = [](uint64_t c, uint64_t x0) -> uint64_t {
        // Heavier mock to give the pool work to chew on.
        uint64_t acc = (c | 1ULL) ^ x0;
        for (int k = 0; k < 128; ++k) {
            acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
            acc ^= (acc >> 11);
        }
        return acc;
    };

    std::vector<uint64_t> cs;
    std::vector<uint64_t> x0s;
    for (uint64_t i = 0; i < 1000; ++i) {
        cs.push_back(i + 1);
        x0s.push_back(i * 7 + 13);
    }

    apply_env("1");
    auto seq = parallel_brent_pollard_rho<uint64_t>(std::span<const uint64_t>(cs),
                                                    std::span<const uint64_t>(x0s), worker);

    apply_env(hw_str.c_str());
    auto par = parallel_brent_pollard_rho<uint64_t>(std::span<const uint64_t>(cs),
                                                    std::span<const uint64_t>(x0s), worker);

    apply_env(nullptr);

    assert(seq.size() == par.size());
    for (std::size_t i = 0; i < seq.size(); ++i) {
        if (seq[i] != par[i]) {
            std::cerr << "\n  ERROR: idx " << i << " seq=" << seq[i] << " par=" << par[i]
                      << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (N=hw=" << hw << ", " << cs.size() << " per-index identical)\n";
}

// ---------------------------------------------------------------------------
// Test 13: Real Brent-Pollard rho integration -- 50 composite cofactors,
// N=1 vs N=4 per-config `std::optional<Integer>` factor identical.
//
// We pick semi-primes built from two known small primes per index so the
// rho run converges deterministically within the iteration budget. The
// worker uses BrentPollardRho::split() with a generous budget; both
// sequential and parallel runs see the same fixed seed schedule (we run
// `split(n, max_iter, seed=1)` so the c-selection inside split is
// deterministic and shared across runs).
//
// Note: the dispatcher's `(c, x0)` interpretation is up to the worker;
// here we ignore them and let split() pick its own c values internally
// (the helper is purely about parallel dispatch fan-out across cofactors,
// not about exposing rho internals). The legitimate way to "use" cs/x0s
// would be a wire-in that pre-builds (c, x0) explicitly.
// ---------------------------------------------------------------------------
void test_real_brent_rho_parity() {
    std::cout << "Test 13: real Brent-Pollard rho parity (50 cofactors, N=1 vs N=4)..."
              << std::flush;

    // Build 50 semi-prime cofactors deterministically. We pick small primes
    // so rho is fast (each fits in uint64_t for the __uint128_t fast path).
    std::vector<uint64_t> primes_a = {
        10007, 10009, 10037, 10039, 10061, 10067, 10069, 10079, 10091, 10093, 10099, 10103, 10111,
        10133, 10139, 10141, 10151, 10159, 10163, 10169, 10177, 10181, 10193, 10211, 10223, 10243,
        10247, 10253, 10259, 10267, 10271, 10273, 10289, 10301, 10303, 10313, 10321, 10331, 10333,
        10337, 10343, 10357, 10369, 10391, 10399, 10427, 10429, 10433, 10453, 10457,
    };
    std::vector<uint64_t> primes_b = {
        11003, 11027, 11047, 11057, 11059, 11069, 11071, 11083, 11087, 11093, 11113, 11117, 11119,
        11131, 11149, 11159, 11161, 11171, 11173, 11177, 11197, 11213, 11239, 11243, 11251, 11257,
        11261, 11273, 11279, 11287, 11299, 11311, 11317, 11321, 11329, 11351, 11353, 11369, 11383,
        11393, 11399, 11411, 11423, 11437, 11443, 11447, 11467, 11471, 11483, 11489,
    };
    assert(primes_a.size() == primes_b.size());
    assert(primes_a.size() == 50);

    // We do not actually use cs / x0s in the worker (split() picks its own
    // c values from a deterministic seed). We pass index-based dummy spans
    // simply because the dispatcher requires two-span input; the worker
    // looks up the cofactor by recomputing primes_a[idx] * primes_b[idx]
    // from a captured vector built per-index.
    std::vector<Integer> cofactors;
    cofactors.reserve(primes_a.size());
    for (std::size_t i = 0; i < primes_a.size(); ++i) {
        Integer n(static_cast<uint64_t>(primes_a[i] * primes_b[i]));
        cofactors.push_back(std::move(n));
    }

    // Build cs / x0s spans by index (each task uses `c` as the index it
    // should look up in `cofactors`). The actual rho work depends solely
    // on the captured `cofactors[idx]`, not on `x0`. This keeps the test
    // a deterministic per-index function while still exercising the
    // dispatcher with two-span input.
    std::vector<uint64_t> cs;
    std::vector<uint64_t> x0s;
    for (std::size_t i = 0; i < cofactors.size(); ++i) {
        cs.push_back(static_cast<uint64_t>(i));
        x0s.push_back(1); // unused by this worker
    }

    auto worker = [&cofactors](uint64_t c_idx, uint64_t /*x0*/) -> std::optional<Integer> {
        // c_idx is the cofactor table index (we reinterpret cs[] as
        // indices because the test purpose is dispatcher parity, not
        // exercising c-parameter sensitivity inside rho).
        const Integer& n = cofactors[c_idx];
        auto split = BrentPollardRho::split(n, /*max_iter=*/1ULL << 18,
                                            /*seed=*/1, /*record=*/false);
        if (!split.has_value())
            return std::nullopt;
        // Return the smaller factor (split() returns (lo, hi) with lo <= hi).
        return std::optional<Integer>(split->first.clone());
    };

    apply_env("1");
    auto seq = parallel_brent_pollard_rho<std::optional<Integer>>(
        std::span<const uint64_t>(cs), std::span<const uint64_t>(x0s), worker);

    apply_env("4");
    auto par = parallel_brent_pollard_rho<std::optional<Integer>>(
        std::span<const uint64_t>(cs), std::span<const uint64_t>(x0s), worker);

    apply_env(nullptr);

    assert(seq.size() == par.size());
    std::size_t found = 0;
    for (std::size_t i = 0; i < seq.size(); ++i) {
        bool seq_has = seq[i].has_value();
        bool par_has = par[i].has_value();
        if (seq_has != par_has) {
            std::cerr << "\n  ERROR: idx " << i << " seq_has=" << seq_has << " par_has=" << par_has
                      << std::endl;
            std::abort();
        }
        if (seq_has) {
            ++found;
            if (seq[i]->compare(*par[i]) != 0) {
                std::cerr << "\n  ERROR: idx " << i << " mismatch: seq=" << seq[i]->to_string()
                          << " par=" << par[i]->to_string() << std::endl;
                std::abort();
            }
            // Sanity: factor must divide cofactor and be non-trivial.
            assert(!seq[i]->is_one());
            assert(seq[i]->compare(cofactors[i]) != 0);
            Integer rem;
            mpz_mod(rem.get_mpz(), cofactors[i].get_mpz(), seq[i]->get_mpz());
            assert(rem.is_zero());
        }
    }

    std::cout << " PASS (" << cs.size() << " cofactors, " << found
              << " factored, per-index optional<Integer> identical)\n";
}

// ---------------------------------------------------------------------------
// Test 14: Mismatched span sizes throw std::invalid_argument.
// ---------------------------------------------------------------------------
void test_mismatched_span_throws() {
    std::cout << "Test 14: mismatched span size throws invalid_argument..." << std::flush;
    apply_env("4");

    std::vector<uint64_t> cs = {1, 2, 3};
    std::vector<uint64_t> x0s = {10, 20}; // shorter

    bool threw = false;
    try {
        auto unused = parallel_brent_pollard_rho<uint64_t>(
            std::span<const uint64_t>(cs), std::span<const uint64_t>(x0s),
            [](uint64_t a, uint64_t b) -> uint64_t { return a + b; });
        (void)unused;
    } catch (const std::invalid_argument& e) {
        threw = true;
        std::string msg = e.what();
        if (msg.find("size") == std::string::npos) {
            std::cerr << "\n  WARN: exception message does not mention 'size': " << msg
                      << std::endl;
        }
    } catch (...) {
        std::cerr << "\n  ERROR: wrong exception type thrown" << std::endl;
        std::abort();
    }
    if (!threw) {
        std::cerr << "\n  ERROR: mismatched span did not throw" << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 15: Reset env cache hook re-reads ENV between assertions.
// ---------------------------------------------------------------------------
void test_reset_env_cache_hook() {
    std::cout << "Test 15: reset env cache hook..." << std::flush;

    apply_env("4");
    int v4 = brent_pollard_rho_threads();
    if (v4 < 1) {
        std::cerr << "\n  ERROR: expected >=1 after '4', got " << v4 << std::endl;
        std::abort();
    }

    // Without reset, subsequent setenv has no effect on the cached value.
    setenv("GNFS_BRENT_POLLARD_RHO_THREADS", "1", /*overwrite=*/1);
    int v4_stale = brent_pollard_rho_threads();
    if (v4_stale != v4) {
        std::cerr << "\n  ERROR: cache did not stick: prev=" << v4 << " stale=" << v4_stale
                  << std::endl;
        std::abort();
    }

    // After explicit reset, the next call sees the new env.
    brent_pollard_rho_threads_reset_env_cache_for_testing();
    int v1 = brent_pollard_rho_threads();
    if (v1 != 1) {
        std::cerr << "\n  ERROR: after reset to '1', got " << v1 << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 16: resolve_brent_pollard_rho_threads(batch_size) edge cases.
// ---------------------------------------------------------------------------
void test_resolve_helper_edges() {
    std::cout << "Test 16: resolve_brent_pollard_rho_threads edges..." << std::flush;

    apply_env("4");
    assert(resolve_brent_pollard_rho_threads(0) == 0);  // empty -> 0
    assert(resolve_brent_pollard_rho_threads(1) == 1);  // single -> 1
    assert(resolve_brent_pollard_rho_threads(10) >= 1); // bound by 4 or hw*2 cap
    assert(resolve_brent_pollard_rho_threads(10) <= 4);

    apply_env("1");
    assert(resolve_brent_pollard_rho_threads(100) == 1); // N=1 -> 1

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 17: Perf-info probe (50 configs N=1 vs N=4, informational only).
// ---------------------------------------------------------------------------
void test_perf_info_probe() {
    std::cout << "Test 17: perf-info probe (informational)..." << std::flush;

    // Heavy mock worker so the pool sees real work per task.
    auto worker = [](uint64_t c, uint64_t x0) -> uint64_t {
        uint64_t acc = c ^ x0;
        for (int k = 0; k < 50000; ++k) {
            acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
            acc ^= (acc >> 13);
        }
        return acc;
    };

    std::vector<uint64_t> cs;
    std::vector<uint64_t> x0s;
    for (uint64_t i = 0; i < 50; ++i) {
        cs.push_back(i + 1);
        x0s.push_back(i * 3 + 7);
    }

    apply_env("1");
    auto t0 = std::chrono::steady_clock::now();
    auto seq = parallel_brent_pollard_rho<uint64_t>(std::span<const uint64_t>(cs),
                                                    std::span<const uint64_t>(x0s), worker);
    auto t1 = std::chrono::steady_clock::now();
    long long ms_n1 = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    apply_env("4");
    auto t2 = std::chrono::steady_clock::now();
    auto par = parallel_brent_pollard_rho<uint64_t>(std::span<const uint64_t>(cs),
                                                    std::span<const uint64_t>(x0s), worker);
    auto t3 = std::chrono::steady_clock::now();
    long long ms_n4 = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();

    apply_env(nullptr);

    // Sanity: parity must hold even in perf probe.
    assert(seq.size() == par.size());
    for (std::size_t i = 0; i < seq.size(); ++i) {
        assert(seq[i] == par[i]);
    }

    double speedup = (ms_n4 > 0) ? (static_cast<double>(ms_n1) / static_cast<double>(ms_n4)) : 0.0;
    std::cout << " PASS (N=1 " << ms_n1 << " ms, N=4 " << ms_n4 << " ms, " << speedup
              << "x speedup)\n";
}

} // namespace

int main() {
    std::cout << "=== Brent-Pollard rho Parallel Dispatch Tests ===" << std::endl;

    test_env_unset_defaults_to_one();
    test_env_zero_and_one();
    test_env_four();
    test_env_non_numeric();
    test_env_partial_parse();
    test_env_clamp_high();
    test_env_positive_overflow_clamps();
    test_empty_span();
    test_single_config_n1();
    test_single_config_n4_no_stall();
    test_n1_baseline_mock();
    test_n1_vs_n4_mock_parity();
    test_n1_vs_n_hw_mock_parity();
    test_real_brent_rho_parity();
    test_mismatched_span_throws();
    test_reset_env_cache_hook();
    test_resolve_helper_edges();
    test_perf_info_probe();

    std::cout << std::endl << "=== All Brent-Pollard rho Parallel Tests PASSED ===" << std::endl;
    return 0;
}
