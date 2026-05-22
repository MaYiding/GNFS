// test_fb_roots_parallel.cpp — Factor base CZ root-finding parallel dispatch tests
//
// Validates the GNFS_FB_ROOTS_THREADS env-gated dispatcher introduced in
// include/gnfs/factor_base/fb_roots_parallel.hpp:
//
//   * ENV parsing handles unset / "0" / "1" / "4" / "9999" / "garbage" /
//     "-5" / "12abc" correctly; clamps high values to hardware_concurrency * 2.
//   * Sequential (env <= 1) and parallel (env >= 2) dispatcher paths produce
//     identical per-prime output vectors for the same deterministic worker
//     function. Bit-for-bit invariant covers the typical 1-100k prime sweep
//     the factor base builder performs.
//   * Empty prime list returns empty vector cleanly (no ThreadPool spawn).
//   * Single prime dispatches in sequential path even under env >= 2 (helper
//     short-circuit to avoid pool overhead for one task).
//   * Worker function is called exactly once per prime (no double-call,
//     no skip).

#include <gnfs/factor_base/fb_roots_parallel.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

using gnfs::factor_base::fb_roots_threads;
using gnfs::factor_base::fb_roots_threads_reset_env_cache_for_testing;
using gnfs::factor_base::parallel_fb_roots;
using gnfs::factor_base::resolve_fb_roots_threads;

namespace {

// Helper: set or unset GNFS_FB_ROOTS_THREADS and refresh the cache so the
// next call to fb_roots_threads() reflects the new value.
void apply_env(const char* value) {
    if (value == nullptr) {
        unsetenv("GNFS_FB_ROOTS_THREADS");
    } else {
        setenv("GNFS_FB_ROOTS_THREADS", value, /*overwrite=*/1);
    }
    fb_roots_threads_reset_env_cache_for_testing();
}

// Helper: build a vector of `count` small odd primes-ish uint32_t values.
// We do not need true primes here — the tests verify the dispatcher itself,
// not CZ root-finding. The worker functions below are pure mathematical
// transforms of `p` so the actual primality is irrelevant.
std::vector<uint32_t> make_primes(std::size_t count, uint32_t start = 3) {
    std::vector<uint32_t> primes;
    primes.reserve(count);
    uint32_t v = start;
    for (std::size_t i = 0; i < count; ++i) {
        primes.push_back(v);
        v += 2;  // odd-stride; sufficient diversity for dispatcher tests
    }
    return primes;
}

// Helper: deterministic worker — returns a small vector of "roots" derived
// from `p` (just a hash-like transform). Pure function of `p`, so
// sequential and parallel paths must produce identical output[i] for the
// same primes[i].
std::vector<uint32_t> deterministic_roots(uint32_t p) {
    // Produce a vector of length (p % 4 + 1) where each entry is
    // f(p, i) = (p * (i + 1)) ^ (p >> 1). Pure determinism, no shared
    // state, no global side effects.
    std::size_t len = static_cast<std::size_t>(p % 4u) + 1u;
    std::vector<uint32_t> out;
    out.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back((p * static_cast<uint32_t>(i + 1)) ^ (p >> 1));
    }
    return out;
}

// Convenience: run the dispatcher with a given env value and return the
// per-prime root vectors.
std::vector<std::vector<uint32_t>>
run_dispatch_with_env(const std::vector<uint32_t>& primes,
                      const char* env_value) {
    apply_env(env_value);
    auto results = parallel_fb_roots<std::vector<uint32_t>>(
        primes,
        [](uint32_t p) { return deterministic_roots(p); });
    apply_env(nullptr);
    return results;
}

// ───────────────────────────────────────────────────────────────────────────
// Test 1: ENV unset -> 0 (default, fall back to hardware_concurrency())
// ───────────────────────────────────────────────────────────────────────────
void test_env_unset_default_zero() {
    std::cout << "Test 1: ENV unset -> 0 (default)..." << std::flush;

    apply_env(nullptr);
    int v = fb_roots_threads();
    if (v != 0) {
        std::cerr << "\n  ERROR: expected 0 (unset default), got " << v
                  << std::endl;
        std::abort();
    }

    // resolve() with env == 0 should pick up hardware_concurrency, bounded by n.
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::size_t resolved100 = resolve_fb_roots_threads(100);
    std::size_t expect100 = std::min<std::size_t>(hw, 100);
    if (resolved100 != expect100) {
        std::cerr << "\n  ERROR: resolve(100) = " << resolved100
                  << ", expected " << expect100 << std::endl;
        std::abort();
    }

    std::cout << " PASS (hw=" << hw << ")\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 2: ENV "0" -> 0 explicit (same as unset)
// ───────────────────────────────────────────────────────────────────────────
void test_env_zero_explicit() {
    std::cout << "Test 2: ENV \"0\" -> 0 explicit..." << std::flush;

    apply_env("0");
    int v = fb_roots_threads();
    if (v != 0) {
        std::cerr << "\n  ERROR: expected 0 for \"0\", got " << v << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 3: ENV "1" -> 1 (force sequential)
// ───────────────────────────────────────────────────────────────────────────
void test_env_one_sequential() {
    std::cout << "Test 3: ENV \"1\" -> 1 sequential..." << std::flush;

    apply_env("1");
    int v = fb_roots_threads();
    if (v != 1) {
        std::cerr << "\n  ERROR: expected 1 for \"1\", got " << v << std::endl;
        std::abort();
    }

    // resolve(n) should return 1 for any n >= 1.
    if (resolve_fb_roots_threads(100) != 1) {
        std::cerr << "\n  ERROR: resolve(100) under env=1 expected 1, got "
                  << resolve_fb_roots_threads(100) << std::endl;
        std::abort();
    }
    if (resolve_fb_roots_threads(1) != 1) {
        std::cerr << "\n  ERROR: resolve(1) under env=1 expected 1, got "
                  << resolve_fb_roots_threads(1) << std::endl;
        std::abort();
    }
    if (resolve_fb_roots_threads(0) != 0) {
        std::cerr << "\n  ERROR: resolve(0) expected 0, got "
                  << resolve_fb_roots_threads(0) << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 4: ENV "4" -> 4 (explicit thread count)
// ───────────────────────────────────────────────────────────────────────────
void test_env_four_parallel() {
    std::cout << "Test 4: ENV \"4\" -> 4 parallel..." << std::flush;

    apply_env("4");
    int v = fb_roots_threads();
    unsigned int hw = std::thread::hardware_concurrency();
    int hw_max = static_cast<int>(hw) * 2;
    if (hw_max <= 0) hw_max = 16;
    int expect = (4 <= hw_max) ? 4 : hw_max;
    if (v != expect) {
        std::cerr << "\n  ERROR: expected " << expect << " for \"4\" (hw*2="
                  << hw_max << "), got " << v << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (resolved=" << v << ")\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 5: ENV "9999" -> clamp to hardware_concurrency * 2
// ───────────────────────────────────────────────────────────────────────────
void test_env_above_max_clamps() {
    std::cout << "Test 5: ENV \"9999\" -> clamp..." << std::flush;

    apply_env("9999");
    int v = fb_roots_threads();
    unsigned int hw = std::thread::hardware_concurrency();
    int hw_max = static_cast<int>(hw) * 2;
    if (hw_max <= 0) hw_max = 16;
    if (v != hw_max) {
        std::cerr << "\n  ERROR: expected clamp to " << hw_max
                  << ", got " << v << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (clamped to " << v << ")\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 6: ENV invalid inputs -> 0 (default fallback)
// ───────────────────────────────────────────────────────────────────────────
void test_env_invalid_non_numeric() {
    std::cout << "Test 6: ENV invalid -> 0 fallback..." << std::flush;

    const char* bads[] = {
        "",        // empty -> 0
        "garbage", // pure non-numeric -> 0
        "abc",     // pure non-numeric -> 0
        "-5",      // negative -> 0
        "-1",      // negative -> 0
    };
    for (const char* s : bads) {
        apply_env(s);
        int v = fb_roots_threads();
        if (v != 0) {
            std::cerr << "\n  ERROR: \"" << s << "\" expected 0, got " << v
                      << std::endl;
            std::abort();
        }
    }

    // Leading garbage that std::stoi cannot parse at position 0.
    apply_env("xyz5");
    int v_xyz = fb_roots_threads();
    if (v_xyz != 0) {
        std::cerr << "\n  ERROR: \"xyz5\" expected 0, got " << v_xyz
                  << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 7: Dispatcher N=1 sequential — calls worker exactly once per prime
// ───────────────────────────────────────────────────────────────────────────
void test_parallel_dispatcher_n1_sequential() {
    std::cout << "Test 7: dispatcher N=1 sequential, exactly-once invocation..."
              << std::flush;

    apply_env("1");
    auto primes = make_primes(50, /*start=*/11);
    std::atomic<int> call_count{0};

    auto results = parallel_fb_roots<std::vector<uint32_t>>(
        primes,
        [&call_count](uint32_t p) {
            call_count.fetch_add(1, std::memory_order_relaxed);
            return deterministic_roots(p);
        });

    int calls = call_count.load(std::memory_order_relaxed);
    if (calls != static_cast<int>(primes.size())) {
        std::cerr << "\n  ERROR: expected " << primes.size() << " calls, got "
                  << calls << std::endl;
        std::abort();
    }
    if (results.size() != primes.size()) {
        std::cerr << "\n  ERROR: results.size()=" << results.size()
                  << " != primes.size()=" << primes.size() << std::endl;
        std::abort();
    }
    for (std::size_t i = 0; i < primes.size(); ++i) {
        if (results[i] != deterministic_roots(primes[i])) {
            std::cerr << "\n  ERROR: results[" << i << "] mismatch for prime="
                      << primes[i] << std::endl;
            std::abort();
        }
    }

    apply_env(nullptr);
    std::cout << " PASS (" << calls << " calls)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 8: Dispatcher N=4 parity vs N=1 — bit-for-bit identical output
// ───────────────────────────────────────────────────────────────────────────
void test_parallel_dispatcher_n4_parity() {
    std::cout << "Test 8: dispatcher N=4 vs N=1 parity..." << std::flush;

    // Use a non-trivial prime count so chunking across 4 workers produces
    // varying chunk sizes. We pick 197 (prime) to ensure 4 workers split
    // unevenly (49, 49, 49, 50).
    auto primes = make_primes(197, /*start=*/7);

    auto seq = run_dispatch_with_env(primes, "1");
    auto par = run_dispatch_with_env(primes, "4");

    if (seq.size() != par.size()) {
        std::cerr << "\n  ERROR: seq.size()=" << seq.size()
                  << " != par.size()=" << par.size() << std::endl;
        std::abort();
    }
    for (std::size_t i = 0; i < seq.size(); ++i) {
        if (seq[i] != par[i]) {
            std::cerr << "\n  ERROR: index " << i << " mismatch for prime="
                      << primes[i] << " seq.size=" << seq[i].size()
                      << " par.size=" << par[i].size() << std::endl;
            std::abort();
        }
    }

    // Spot-check the larger 1000-prime sweep to exercise heavier load.
    auto primes_big = make_primes(1000, /*start=*/13);
    auto seq_big = run_dispatch_with_env(primes_big, "1");
    auto par_big = run_dispatch_with_env(primes_big, "4");
    assert(seq_big.size() == par_big.size());
    for (std::size_t i = 0; i < seq_big.size(); ++i) {
        if (seq_big[i] != par_big[i]) {
            std::cerr << "\n  ERROR: 1000-sweep index " << i
                      << " mismatch for prime=" << primes_big[i] << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (" << primes.size() << " + " << primes_big.size()
              << " primes, bit-identical)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 9: Dispatcher N=hw parity vs N=1 — bit-for-bit identical output
// ───────────────────────────────────────────────────────────────────────────
void test_parallel_dispatcher_nhw_parity() {
    std::cout << "Test 9: dispatcher N=hw parity..." << std::flush;

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::string hw_str = std::to_string(hw);

    auto primes = make_primes(500, /*start=*/19);
    auto seq = run_dispatch_with_env(primes, "1");
    auto par = run_dispatch_with_env(primes, hw_str.c_str());

    assert(seq.size() == par.size());
    for (std::size_t i = 0; i < seq.size(); ++i) {
        if (seq[i] != par[i]) {
            std::cerr << "\n  ERROR: hw-parity index " << i
                      << " mismatch for prime=" << primes[i] << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (N=hw=" << hw << ", " << primes.size()
              << " primes)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 10: Empty primes -> empty output, no worker calls, no stall
// ───────────────────────────────────────────────────────────────────────────
void test_empty_primes_no_stall() {
    std::cout << "Test 10: empty primes (no-op all paths)..." << std::flush;

    std::vector<uint32_t> empty;
    std::atomic<int> calls{0};

    // Sequential (env=1) — empty input yields empty output, worker never
    // called.
    apply_env("1");
    auto seq = parallel_fb_roots<std::vector<uint32_t>>(
        empty,
        [&calls](uint32_t) -> std::vector<uint32_t> {
            calls.fetch_add(1);
            std::cerr << "\n  ERROR: worker invoked on empty primes vector"
                      << std::endl;
            std::abort();
        });
    assert(seq.empty());
    assert(calls.load() == 0);

    // Parallel (env=4) — same invariant. Helper must short-circuit before
    // ThreadPool spawn.
    apply_env("4");
    auto par = parallel_fb_roots<std::vector<uint32_t>>(
        empty,
        [&calls](uint32_t) -> std::vector<uint32_t> {
            calls.fetch_add(1);
            std::cerr << "\n  ERROR: worker invoked on empty primes vector"
                      << std::endl;
            std::abort();
        });
    assert(par.empty());
    assert(calls.load() == 0);

    // Default (env unset) — also no-op.
    apply_env(nullptr);
    auto def = parallel_fb_roots<std::vector<uint32_t>>(
        empty,
        [&calls](uint32_t) -> std::vector<uint32_t> {
            calls.fetch_add(1);
            std::cerr << "\n  ERROR: worker invoked on empty primes vector"
                      << std::endl;
            std::abort();
        });
    assert(def.empty());
    assert(calls.load() == 0);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 11: Single prime under N=4 — sequential short-circuit (no stall,
// no double-call)
// ───────────────────────────────────────────────────────────────────────────
void test_single_prime_no_stall() {
    std::cout << "Test 11: single prime N=4 (no stall, called exactly once)..."
              << std::flush;

    apply_env("4");
    std::vector<uint32_t> single = {17};
    std::atomic<int> calls{0};

    auto t0 = std::chrono::steady_clock::now();
    auto results = parallel_fb_roots<std::vector<uint32_t>>(
        single,
        [&calls](uint32_t p) {
            calls.fetch_add(1, std::memory_order_relaxed);
            return deterministic_roots(p);
        });
    auto t1 = std::chrono::steady_clock::now();
    long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    assert(results.size() == 1);
    assert(results[0] == deterministic_roots(17));
    int total_calls = calls.load(std::memory_order_relaxed);
    if (total_calls != 1) {
        std::cerr << "\n  ERROR: expected exactly 1 call, got " << total_calls
                  << std::endl;
        std::abort();
    }

    // Single prime must short-circuit to the sequential path (no pool
    // creation). Guard against accidental pool spin-up — a generous bound
    // so sanitizers do not false-fail.
    if (ms > 1000) {
        std::cerr << "\n  WARN: single-prime dispatch took " << ms
                  << " ms (expected << 1000 ms)" << std::endl;
        // Soft signal; do not abort.
    }

    apply_env(nullptr);
    std::cout << " PASS (" << ms << " ms)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 12: "12abc" partial parse — std::stoi accepts the "12" prefix.
// Document this behaviour explicitly to avoid silent surprise.
// ───────────────────────────────────────────────────────────────────────────
void test_env_partial_parse_behaviour() {
    std::cout << "Test 12: \"12abc\" partial parse..." << std::flush;

    apply_env("12abc");
    int v = fb_roots_threads();
    // std::stoi parses the leading "12" then stops at 'a'. We do not reject
    // partial parses (consumed > 0) so this yields 12 (clamped to hw*2).
    unsigned int hw = std::thread::hardware_concurrency();
    int hw_max = static_cast<int>(hw) * 2;
    if (hw_max <= 0) hw_max = 16;
    int expect = (12 <= hw_max) ? 12 : hw_max;
    if (v != expect) {
        std::cerr << "\n  ERROR: \"12abc\" expected " << expect
                  << ", got " << v << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (parsed to " << v << ")\n";
}

}  // namespace

int main() {
    std::cout << "=== Factor Base Roots Parallel Dispatch Tests ==="
              << std::endl;

    test_env_unset_default_zero();
    test_env_zero_explicit();
    test_env_one_sequential();
    test_env_four_parallel();
    test_env_above_max_clamps();
    test_env_invalid_non_numeric();
    test_parallel_dispatcher_n1_sequential();
    test_parallel_dispatcher_n4_parity();
    test_parallel_dispatcher_nhw_parity();
    test_empty_primes_no_stall();
    test_single_prime_no_stall();
    test_env_partial_parse_behaviour();

    std::cout << std::endl
              << "=== All FB Roots Parallel Tests PASSED ===" << std::endl;
    return 0;
}
