// test_lattice_basis_parallel.cpp — lattice basis reduction parallel
// dispatcher tests.
//
// Validates the GNFS_LATTICE_BASIS_PARALLEL_THREADS env-gated dispatcher
// introduced in include/gnfs/sieve/lattice_basis_parallel.hpp:
//
//   * ENV parsing handles unset / "0" / "4" / "10000" correctly with the
//     clamping mirror of W7/W8/W9/W10 T4 (default 1, cap at
//     hardware_concurrency * 2).
//   * Sequential (N=1, default) and parallel (N>=2) paths produce identical
//     per-basis outcomes for the same input span. The dispatcher is a pure
//     wrapper around a caller-supplied `reduce_fn` callable, so a
//     deterministic mock reducer (no real lattice reduction algorithm
//     required) drives the parity assertions.
//   * Empty basis list returns empty vector cleanly without creating a pool
//     or invoking the reduce functor.
//   * Single basis under N>=2 short-circuits to sequential (exactly-once
//     invocation, no stall).
//   * Non-trivial Result type (`std::vector<uint64_t>`) is moved / copied
//     correctly without race or lost elements under either path.
//   * Reduce functor throwing propagates the first exception to the caller
//     under both N=1 and N=4 dispatch.

#include <gnfs/sieve/lattice_basis_parallel.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using gnfs::sieve::lattice_basis_parallel_threads;
using gnfs::sieve::lattice_basis_parallel_threads_reset_env_cache_for_testing;
using gnfs::sieve::parallel_lattice_basis_reduce;

namespace {

// Mock per-Special-Q basis input. Stand-in for the real
// `LatticeBasis::reduce_fn(q, root, ...)` signature without pulling in the
// full integer / NumberField stack.
struct MockBasis {
    uint64_t q;
    uint64_t root;
};

// Mock per-basis reduction result. Real wire-in would carry an
// {Integer b1, Integer b2} pair; here we use a tiny POD whose value is a
// deterministic function of (q, root) so per-index parity is byte-exact.
struct MockReduced {
    uint64_t value;

    bool operator==(const MockReduced& other) const noexcept {
        return value == other.value;
    }
};

// Deterministic per-index "reduction" — emulates the per-Special-Q reduce_fn
// the helper will dispatch in a future wire-in. Pure function of (q, root),
// no shared state, no GMP. Choice of multiplier 31 and per-bit XOR mix is
// arbitrary but stable across runs so N=1 vs N>=2 paths compare bit-for-bit.
MockReduced mock_reduce(const MockBasis& b) {
    uint64_t v = b.q * 31ULL + b.root;
    v ^= (v >> 13);
    v *= 0x9E3779B97F4A7C15ULL;
    v ^= (v >> 11);
    return MockReduced{v};
}

// Helper: set or unset the env var and refresh the cached parse result so the
// next call to lattice_basis_parallel_threads() reflects the new value.
void apply_env(const char* value) {
    if (value == nullptr) {
        unsetenv("GNFS_LATTICE_BASIS_PARALLEL_THREADS");
    } else {
        setenv("GNFS_LATTICE_BASIS_PARALLEL_THREADS", value, /*overwrite=*/1);
    }
    lattice_basis_parallel_threads_reset_env_cache_for_testing();
}

[[noreturn]] void fail_check(const char* message) {
    std::cerr << "\n  ERROR: " << message << std::endl;
    std::abort();
}

void check(bool condition, const char* message) {
    if (!condition) {
        fail_check(message);
    }
}

std::vector<MockBasis> make_cache_isolation_inputs() {
    std::vector<MockBasis> bases;
    bases.reserve(16);
    for (uint64_t i = 0; i < 16; ++i) {
        bases.push_back({i * 1009ULL + 17, i * 11ULL + 3});
    }
    return bases;
}

void check_mock_results(const std::vector<MockBasis>& bases,
                        const std::vector<MockReduced>& results, const char* message) {
    check(results.size() == bases.size(), message);
    for (std::size_t i = 0; i < bases.size(); ++i) {
        if (!(results[i] == mock_reduce(bases[i]))) {
            fail_check(message);
        }
    }
}

// ───────────────────────────────────────────────────────────────────────────
// Test 1: ENV unset -> 1 (default sequential)
// ───────────────────────────────────────────────────────────────────────────
void test_env_unset_defaults_to_one() {
    std::cout << "Test 1: ENV unset -> 1..." << std::flush;
    apply_env(nullptr);
    std::size_t v = lattice_basis_parallel_threads();
    if (v != 1) {
        std::cerr << "\n  ERROR: unset env parsed to " << v << ", expected 1" << std::endl;
        std::abort();
    }
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 2: ENV "0" -> 1 (invalid, non-positive -> sequential)
// ───────────────────────────────────────────────────────────────────────────
void test_env_zero_to_one() {
    std::cout << "Test 2: ENV '0' -> 1..." << std::flush;
    apply_env("0");
    std::size_t v = lattice_basis_parallel_threads();
    if (v != 1) {
        std::cerr << "\n  ERROR: '0' parsed to " << v << ", expected 1" << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 3: ENV "4" -> 4 (or clamped to cap on tiny CI runners)
// ───────────────────────────────────────────────────────────────────────────
void test_env_four() {
    std::cout << "Test 3: ENV '4' -> 4..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    std::size_t cap = static_cast<std::size_t>(hw) * 2;
    std::size_t expect = (4 < cap) ? 4 : cap;

    apply_env("4");
    std::size_t v = lattice_basis_parallel_threads();
    if (v != expect) {
        std::cerr << "\n  ERROR: '4' parsed to " << v << ", expected " << expect
                  << " (hw*2 cap = " << cap << ")" << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (parsed " << v << ", cap " << cap << ")\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 4: ENV "10000" -> clamped at hardware_concurrency() * 2
// ───────────────────────────────────────────────────────────────────────────
void test_env_clamp_at_hw_times_two() {
    std::cout << "Test 4: ENV '10000' clamped at hw*2..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    std::size_t cap = static_cast<std::size_t>(hw) * 2;

    apply_env("10000");
    std::size_t v = lattice_basis_parallel_threads();
    if (v != cap) {
        std::cerr << "\n  ERROR: '10000' parsed to " << v << ", expected cap=" << cap << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (cap=" << cap << ")\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 5: empty input span — both N=1 and N=4 return empty vector cleanly
//          without creating a pool or invoking reduce_fn.
// ───────────────────────────────────────────────────────────────────────────
void test_empty_input() {
    std::cout << "Test 5: empty input (no-op both paths)..." << std::flush;

    std::vector<MockBasis> empty_in;

    // Sequential (N=1).
    apply_env("1");
    auto seq_results = parallel_lattice_basis_reduce<MockReduced, MockBasis>(
        std::span<const MockBasis>(empty_in), [](const MockBasis&) -> MockReduced {
            std::cerr << "\n  ERROR: reduce_fn invoked on empty span" << std::endl;
            std::abort();
            return MockReduced{};
        });
    assert(seq_results.empty());

    // Parallel (N=4).
    apply_env("4");
    auto par_results = parallel_lattice_basis_reduce<MockReduced, MockBasis>(
        std::span<const MockBasis>(empty_in), [](const MockBasis&) -> MockReduced {
            std::cerr << "\n  ERROR: reduce_fn invoked on empty span" << std::endl;
            std::abort();
            return MockReduced{};
        });
    assert(par_results.empty());

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 6: single basis, N=1 — result correct, sequential path exercised.
// ───────────────────────────────────────────────────────────────────────────
void test_single_basis_n1() {
    std::cout << "Test 6: single basis N=1 (result correct)..." << std::flush;

    apply_env("1");
    std::vector<MockBasis> bases = {{12345ULL, 67ULL}};
    auto results = parallel_lattice_basis_reduce<MockReduced, MockBasis>(
        std::span<const MockBasis>(bases), mock_reduce);

    assert(results.size() == 1);
    MockReduced expect = mock_reduce(bases[0]);
    if (!(results[0] == expect)) {
        std::cerr << "\n  ERROR: got value=" << results[0].value << ", expected " << expect.value
                  << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (value=" << results[0].value << ")\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 7: single basis, N=4 — short-circuit to sequential, exactly-once
//          invocation, no stall on pool spin-up.
// ───────────────────────────────────────────────────────────────────────────
void test_single_basis_n4_no_stall() {
    std::cout << "Test 7: single basis N=4 (exactly-once, no stall)..." << std::flush;

    apply_env("4");
    std::vector<MockBasis> bases = {{42ULL, 7ULL}};
    std::atomic<int> calls{0};

    auto t0 = std::chrono::steady_clock::now();
    auto results = parallel_lattice_basis_reduce<MockReduced, MockBasis>(
        std::span<const MockBasis>(bases), [&calls](const MockBasis& b) -> MockReduced {
            calls.fetch_add(1, std::memory_order_relaxed);
            return mock_reduce(b);
        });
    auto t1 = std::chrono::steady_clock::now();
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    assert(results.size() == 1);
    MockReduced expect = mock_reduce(bases[0]);
    if (!(results[0] == expect)) {
        std::cerr << "\n  ERROR: got value=" << results[0].value << ", expected " << expect.value
                  << std::endl;
        std::abort();
    }
    int total_calls = calls.load(std::memory_order_relaxed);
    if (total_calls != 1) {
        std::cerr << "\n  ERROR: expected exactly 1 call, got " << total_calls << std::endl;
        std::abort();
    }

    // Sanity-bound the wall-time: if the helper accidentally spawned a
    // 4-thread pool the spin-up alone would push past this.
    if (ms > 1000) {
        std::cerr << "\n  WARN: single-basis dispatch took " << ms << " ms (expected << 1000 ms)"
                  << std::endl;
        // No abort — sanitizers can be slow.
    }

    apply_env(nullptr);
    std::cout << " PASS (" << ms << " ms)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 8: 100 bases, N=1 baseline — deterministic mock_reduce, dispatcher
//          returns vector aligned with input, exactly-once invocation per
//          basis.
// ───────────────────────────────────────────────────────────────────────────
void test_100_bases_n1_baseline() {
    std::cout << "Test 8: 100 bases N=1 baseline (identity reduce)..." << std::flush;
    apply_env("1");

    std::vector<MockBasis> bases;
    bases.reserve(100);
    for (uint64_t i = 0; i < 100; ++i) {
        bases.push_back({i * 1009ULL + 1, i * 3ULL + 11});
    }

    auto results = parallel_lattice_basis_reduce<MockReduced, MockBasis>(
        std::span<const MockBasis>(bases), mock_reduce);

    if (results.size() != bases.size()) {
        std::cerr << "\n  ERROR: expected " << bases.size() << " results, got " << results.size()
                  << std::endl;
        std::abort();
    }
    for (std::size_t i = 0; i < bases.size(); ++i) {
        MockReduced expect = mock_reduce(bases[i]);
        if (!(results[i] == expect)) {
            std::cerr << "\n  ERROR: idx " << i << " got " << results[i].value << " expected "
                      << expect.value << std::endl;
            std::abort();
        }
    }

    apply_env(nullptr);
    std::cout << " PASS (" << bases.size() << " bases)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 9: N=1 vs N=4 parity — strict per-index bit-identical assertion
//          across the two paths.
// ───────────────────────────────────────────────────────────────────────────
void test_n1_vs_n4_parity() {
    std::cout << "Test 9: N=1 vs N=4 parity (per-index bit-identical)..." << std::flush;

    std::vector<MockBasis> bases;
    bases.reserve(100);
    for (uint64_t i = 0; i < 100; ++i) {
        bases.push_back({i * 1009ULL + 1, i * 3ULL + 11});
    }

    apply_env("1");
    auto seq = parallel_lattice_basis_reduce<MockReduced, MockBasis>(
        std::span<const MockBasis>(bases), mock_reduce);

    apply_env("4");
    auto par = parallel_lattice_basis_reduce<MockReduced, MockBasis>(
        std::span<const MockBasis>(bases), mock_reduce);

    apply_env(nullptr);

    if (seq.size() != par.size()) {
        std::cerr << "\n  ERROR: seq.size()=" << seq.size() << " par.size()=" << par.size()
                  << std::endl;
        std::abort();
    }
    for (std::size_t i = 0; i < seq.size(); ++i) {
        if (!(seq[i] == par[i])) {
            std::cerr << "\n  ERROR: idx " << i << " seq=" << seq[i].value
                      << " par=" << par[i].value << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (" << bases.size() << " per-index identical)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 10: N=1 vs N=hw_concurrency parity — extra coverage at the runtime
//           upper bound to catch races / aliasing that smaller N might miss.
//           Also reports an informational mock speedup.
// ───────────────────────────────────────────────────────────────────────────
void test_n1_vs_n_hw_parity() {
    std::cout << "Test 10: N=1 vs N=hw parity + perf info..." << std::flush;

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    std::string hw_str = std::to_string(hw);

    // Heavier mock reducer so the pool has visible work to chew on. Pure
    // function of (q, root); no shared state.
    auto heavy_reduce = [](const MockBasis& b) -> MockReduced {
        uint64_t acc = (b.q | 1ULL) ^ b.root;
        for (int k = 0; k < 256; ++k) {
            acc = acc * 6364136223846793005ULL + 1442695040888963407ULL + b.root;
            acc ^= (acc >> 11);
        }
        return MockReduced{acc};
    };

    std::vector<MockBasis> bases;
    bases.reserve(100);
    for (uint64_t i = 0; i < 100; ++i) {
        bases.push_back({i * 7919ULL + 1, i * 23ULL + 5});
    }

    apply_env("1");
    auto t0 = std::chrono::steady_clock::now();
    auto seq = parallel_lattice_basis_reduce<MockReduced, MockBasis>(
        std::span<const MockBasis>(bases), heavy_reduce);
    auto t1 = std::chrono::steady_clock::now();
    long long ms_seq = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    apply_env(hw_str.c_str());
    auto t2 = std::chrono::steady_clock::now();
    auto par = parallel_lattice_basis_reduce<MockReduced, MockBasis>(
        std::span<const MockBasis>(bases), heavy_reduce);
    auto t3 = std::chrono::steady_clock::now();
    long long ms_par = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

    apply_env(nullptr);

    assert(seq.size() == par.size());
    for (std::size_t i = 0; i < seq.size(); ++i) {
        if (!(seq[i] == par[i])) {
            std::cerr << "\n  ERROR: idx " << i << " seq=" << seq[i].value
                      << " par=" << par[i].value << std::endl;
            std::abort();
        }
    }

    double speedup =
        (ms_par > 0) ? (static_cast<double>(ms_seq) / static_cast<double>(ms_par)) : 0.0;
    std::cout << " PASS (N=hw=" << hw << ", " << bases.size()
              << " per-index identical, seq=" << ms_seq << "us, par=" << ms_par
              << "us, speedup=" << speedup << "x)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 11: Non-trivial Result type (`std::vector<uint64_t>`) — exercises
//           move semantics so a per-basis vector return does not drop or
//           shred elements when assigned into the output slot under either
//           dispatch path.
// ───────────────────────────────────────────────────────────────────────────
void test_non_trivial_result_type() {
    std::cout << "Test 11: non-trivial Result (vector<uint64_t>) parity..." << std::flush;

    std::vector<MockBasis> bases;
    bases.reserve(64);
    for (uint64_t i = 0; i < 64; ++i) {
        bases.push_back({i * 31ULL + 17, i * 5ULL + 1});
    }

    // reduce_fn emits a (i+1)-element vector populated with basis-derived
    // values; ensures non-trivial move semantics travel through the
    // dispatcher's results[i] = ... write.
    auto vec_reduce = [](const MockBasis& b) -> std::vector<uint64_t> {
        std::size_t count = static_cast<std::size_t>(b.root % 7) + 1;
        std::vector<uint64_t> out;
        out.reserve(count + 1);
        out.push_back(b.q);
        for (std::size_t k = 0; k < count; ++k) {
            out.push_back(b.q * 31ULL + b.root * (k + 1));
        }
        return out;
    };

    apply_env("1");
    auto seq = parallel_lattice_basis_reduce<std::vector<uint64_t>, MockBasis>(
        std::span<const MockBasis>(bases), vec_reduce);

    apply_env("4");
    auto par = parallel_lattice_basis_reduce<std::vector<uint64_t>, MockBasis>(
        std::span<const MockBasis>(bases), vec_reduce);

    apply_env(nullptr);

    assert(seq.size() == par.size());
    for (std::size_t i = 0; i < seq.size(); ++i) {
        if (seq[i] != par[i]) {
            std::cerr << "\n  ERROR: idx " << i << " seq.size()=" << seq[i].size()
                      << " par.size()=" << par[i].size() << std::endl;
            std::abort();
        }
        // And cross-check seq[i] against the freshly computed reference.
        auto expect = vec_reduce(bases[i]);
        if (seq[i] != expect) {
            std::cerr << "\n  ERROR: idx " << i << " seq does not match vec_reduce reference"
                      << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (" << bases.size() << " vectors compared)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 12: reduce_fn throws — exception propagates to caller (does not
//           swallow). Verified under both N=1 and N=4 paths.
// ───────────────────────────────────────────────────────────────────────────
void test_reduce_fn_exception_propagates() {
    std::cout << "Test 12: reduce_fn exception propagates..." << std::flush;

    std::vector<MockBasis> bases;
    for (uint64_t i = 0; i < 8; ++i) {
        bases.push_back({i + 1, i * 7ULL});
    }

    auto throw_fn = [](const MockBasis& b) -> MockReduced {
        if (b.q == 4) {
            throw std::runtime_error("basis 4 sentinel");
        }
        return MockReduced{b.q * 10ULL};
    };

    // Sequential path: exception must rethrow.
    apply_env("1");
    bool seq_caught = false;
    try {
        auto seq = parallel_lattice_basis_reduce<MockReduced, MockBasis>(
            std::span<const MockBasis>(bases), throw_fn);
        (void)seq;
    } catch (const std::runtime_error& e) {
        seq_caught = true;
        std::string what = e.what();
        if (what.find("basis 4 sentinel") == std::string::npos) {
            std::cerr << "\n  ERROR: seq path unexpected what(): " << what << std::endl;
            std::abort();
        }
    }
    if (!seq_caught) {
        std::cerr << "\n  ERROR: seq path swallowed exception" << std::endl;
        std::abort();
    }

    // Parallel path: at least one task throws; first observed exception
    // rethrows (any exception of expected type satisfies the contract).
    apply_env("4");
    bool par_caught = false;
    try {
        auto par = parallel_lattice_basis_reduce<MockReduced, MockBasis>(
            std::span<const MockBasis>(bases), throw_fn);
        (void)par;
    } catch (const std::runtime_error& e) {
        par_caught = true;
        std::string what = e.what();
        if (what.find("basis 4 sentinel") == std::string::npos) {
            std::cerr << "\n  ERROR: par path unexpected what(): " << what << std::endl;
            std::abort();
        }
    }
    if (!par_caught) {
        std::cerr << "\n  ERROR: par path swallowed exception" << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS (seq + par both rethrow)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 13: reset_env_cache hook — toggling the env between assertions
//           without resetting the cache should reuse the stale parsed value;
//           after reset, the new value should take effect.
// ───────────────────────────────────────────────────────────────────────────
void test_reset_env_cache_hook() {
    std::cout << "Test 13: reset_env_cache hook..." << std::flush;

    // Establish baseline cached value of 1 from unset env.
    apply_env(nullptr);
    std::size_t v1 = lattice_basis_parallel_threads();
    assert(v1 == 1);

    // Set env to 4, but DO NOT call reset — cache still holds 1.
    setenv("GNFS_LATTICE_BASIS_PARALLEL_THREADS", "4", /*overwrite=*/1);
    std::size_t v2 = lattice_basis_parallel_threads();
    if (v2 != 1) {
        std::cerr << "\n  ERROR: stale cache should still report 1, got " << v2 << std::endl;
        std::abort();
    }

    // Now reset; the new "4" parse should take effect.
    lattice_basis_parallel_threads_reset_env_cache_for_testing();
    std::size_t v3 = lattice_basis_parallel_threads();
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    std::size_t cap = static_cast<std::size_t>(hw) * 2;
    std::size_t expect = (4 < cap) ? 4 : cap;
    if (v3 != expect) {
        std::cerr << "\n  ERROR: post-reset got " << v3 << ", expected " << expect << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 14: an explicit four-thread request must ignore a stale cached legacy
//          value of one. A two-worker rendezvous proves the parallel path was
//          entered; output parity alone could not distinguish the paths.
// ───────────────────────────────────────────────────────────────────────────
void test_explicit_four_ignores_stale_cached_one() {
    std::cout << "Test 14: explicit 4 ignores stale cached 1..." << std::flush;

    apply_env("1");
    check(lattice_basis_parallel_threads() == 1, "failed to seed stale cache with one");
    setenv("GNFS_LATTICE_BASIS_PARALLEL_THREADS", "4", /*overwrite=*/1);

    auto bases = make_cache_isolation_inputs();
    std::mutex mutex;
    std::condition_variable ready;
    std::size_t active = 0;
    std::size_t max_active = 0;
    bool release = false;

    auto results = parallel_lattice_basis_reduce<MockReduced, MockBasis>(
        std::span<const MockBasis>(bases),
        [&](const MockBasis& basis) -> MockReduced {
            std::unique_lock lock(mutex);
            ++active;
            max_active = std::max(max_active, active);
            if (active >= 2) {
                release = true;
                ready.notify_all();
            }
            const bool rendezvous_reached =
                ready.wait_for(lock, std::chrono::seconds(2), [&release]() { return release; });
            if (!rendezvous_reached) {
                throw std::runtime_error(
                    "explicit four-thread dispatch used stale sequential cache");
            }
            --active;
            lock.unlock();
            return mock_reduce(basis);
        },
        4);

    check(max_active >= 2, "explicit four-thread dispatch did not run reducers concurrently");
    check_mock_results(bases, results, "explicit four-thread results differ from reference");
    check(lattice_basis_parallel_threads() == 1,
          "explicit four-thread dispatch mutated stale cached one");

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 15: an explicit one-thread request must ignore a stale cached legacy
//          parallel value. Every reducer must execute on the caller thread.
// ───────────────────────────────────────────────────────────────────────────
void test_explicit_one_ignores_stale_cached_four() {
    std::cout << "Test 15: explicit 1 ignores stale cached 4..." << std::flush;

    apply_env("4");
    const std::size_t stale_parallel_threads = lattice_basis_parallel_threads();
    check(stale_parallel_threads >= 2, "failed to seed stale cache with a parallel value");
    setenv("GNFS_LATTICE_BASIS_PARALLEL_THREADS", "1", /*overwrite=*/1);

    auto bases = make_cache_isolation_inputs();
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> stayed_on_caller{true};
    auto results = parallel_lattice_basis_reduce<MockReduced, MockBasis>(
        std::span<const MockBasis>(bases),
        [&](const MockBasis& basis) -> MockReduced {
            if (std::this_thread::get_id() != caller) {
                stayed_on_caller.store(false, std::memory_order_relaxed);
            }
            return mock_reduce(basis);
        },
        1);

    check(stayed_on_caller.load(std::memory_order_relaxed),
          "explicit one-thread dispatch used a worker thread");
    check_mock_results(bases, results, "explicit one-thread results differ from reference");
    check(lattice_basis_parallel_threads() == stale_parallel_threads,
          "explicit one-thread dispatch mutated stale parallel cache");

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 16: the explicit overload neither seeds an uninitialized legacy cache
//          nor mutates an initialized one. threads=0 also proves the explicit
//          zero request takes the sequential caller-thread path.
// ───────────────────────────────────────────────────────────────────────────
void test_explicit_dispatch_does_not_seed_or_mutate_cache() {
    std::cout << "Test 16: explicit dispatch does not seed/mutate cache..." << std::flush;

    setenv("GNFS_LATTICE_BASIS_PARALLEL_THREADS", "1", /*overwrite=*/1);
    lattice_basis_parallel_threads_reset_env_cache_for_testing();

    auto bases = make_cache_isolation_inputs();
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> stayed_on_caller{true};
    auto zero_thread_results = parallel_lattice_basis_reduce<MockReduced, MockBasis>(
        std::span<const MockBasis>(bases),
        [&](const MockBasis& basis) -> MockReduced {
            if (std::this_thread::get_id() != caller) {
                stayed_on_caller.store(false, std::memory_order_relaxed);
            }
            return mock_reduce(basis);
        },
        0);
    check(stayed_on_caller.load(std::memory_order_relaxed),
          "explicit zero-thread dispatch used a worker thread");
    check_mock_results(bases, zero_thread_results,
                       "explicit zero-thread results differ from reference");

    // If the explicit call had seeded the cache from the previous "1", this
    // raw env mutation would remain invisible to the first legacy lookup.
    setenv("GNFS_LATTICE_BASIS_PARALLEL_THREADS", "4", /*overwrite=*/1);
    const std::size_t cached_parallel_threads = lattice_basis_parallel_threads();
    check(cached_parallel_threads >= 2, "explicit dispatch seeded the legacy cache");

    auto explicit_results = parallel_lattice_basis_reduce<MockReduced, MockBasis>(
        std::span<const MockBasis>(bases), mock_reduce, 1);
    check_mock_results(bases, explicit_results, "explicit dispatch results differ from reference");
    check(lattice_basis_parallel_threads() == cached_parallel_threads,
          "explicit dispatch mutated initialized legacy cache");

    apply_env(nullptr);
    std::cout << " PASS\n";
}

} // namespace

int main() {
    std::cout << "=== Lattice Basis Parallel Dispatch Tests ===" << std::endl;

    test_env_unset_defaults_to_one();
    test_env_zero_to_one();
    test_env_four();
    test_env_clamp_at_hw_times_two();
    test_empty_input();
    test_single_basis_n1();
    test_single_basis_n4_no_stall();
    test_100_bases_n1_baseline();
    test_n1_vs_n4_parity();
    test_n1_vs_n_hw_parity();
    test_non_trivial_result_type();
    test_reduce_fn_exception_propagates();
    test_reset_env_cache_hook();
    test_explicit_four_ignores_stale_cached_one();
    test_explicit_one_ignores_stale_cached_four();
    test_explicit_dispatch_does_not_seed_or_mutate_cache();

    std::cout << std::endl << "=== All Lattice Basis Parallel Tests PASSED ===" << std::endl;
    return 0;
}
