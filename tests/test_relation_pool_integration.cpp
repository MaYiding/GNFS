// Integration tests for RelationCollector pool path (W6 T4).
//
// Verifies that toggling GNFS_RELATION_POOL_SIZE / CollectorConfig.use_pool
// produces bit-for-bit identical output for the same (a,b) input sequence.
// Also includes a perf-info probe that prints push timing for pool ON vs OFF;
// not an assertion — only reports for human inspection during local runs.

// Force assert() to remain live even under -DNDEBUG.
#ifdef NDEBUG
#  undef NDEBUG
#endif

#include "gnfs/relation/collector.hpp"
#include "gnfs/util/memory_pool.hpp"
#include "gnfs/util/safe_math.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <utility>
#include <vector>

using namespace gnfs;
using namespace gnfs::relation;
using namespace gnfs::core;

namespace {

struct ABFactors {
    int64_t a;
    uint64_t b;
    std::vector<uint32_t> rat;
    std::vector<uint32_t> alg;
};

// Deterministic sequence of (a,b) pairs with gcd(a,b)=1 plus a couple of
// per-relation factor indices. Output is identical across runs.
std::vector<ABFactors> make_deterministic_sequence(int count) {
    std::vector<ABFactors> seq;
    seq.reserve(static_cast<size_t>(count));
    int64_t a = 1;
    uint64_t b = 2;
    for (int i = 0; i < count; ++i) {
        // Bump until gcd(a,b)=1
        while (std::gcd(util::safe_abs(a), b) != 1) ++b;
        ABFactors r;
        r.a = a;
        r.b = b;
        r.rat = {static_cast<uint32_t>(i % 17), static_cast<uint32_t>((i * 3) % 23)};
        r.alg = {static_cast<uint32_t>((i * 5) % 29)};
        seq.push_back(std::move(r));
        // Advance: alternate +1 / -1 to mix positive/negative a.
        a = (i & 1) ? -(a + 1) : (a + 2);
        b += 1;
    }
    return seq;
}

Relation to_relation(const ABFactors& f) {
    Relation r(f.a, f.b);
    r.rational_factors = f.rat;
    r.algebraic_factors = f.alg;
    return r;
}

// Sort relations by (a,b) for stable comparison.
void sort_by_ab(std::vector<Relation>& v) {
    std::sort(v.begin(), v.end(), [](const Relation& l, const Relation& r) {
        if (l.b != r.b) return l.b < r.b;
        return l.a < r.a;
    });
}

bool same_relation(const Relation& l, const Relation& r) {
    return l.a == r.a && l.b == r.b &&
           l.rational_factors == r.rational_factors &&
           l.algebraic_factors == r.algebraic_factors;
}

std::vector<Relation> collect_with_config(const std::vector<ABFactors>& seq,
                                          bool use_pool,
                                          size_t initial_bytes) {
    CollectorConfig cfg;
    cfg.use_pool = use_pool;
    cfg.pool_initial_bytes = initial_bytes;
    RelationCollector collector(cfg);
    for (const auto& f : seq) {
        Relation r = to_relation(f);
        bool added = collector.add(std::move(r));
        (void)added;  // Some inputs may legitimately fail validation in
                     // extreme corner cases (e.g. a=b=0); deterministic seq
                     // ensures non-degenerate pairs so this should be true.
        assert(added);
    }
    return collector.get_relations();
}

}  // namespace

void test_small_sequence_bit_for_bit() {
    std::cout << "Testing small sequence: pool ON vs OFF bit-for-bit..." << std::endl;
    auto seq = make_deterministic_sequence(50);

    auto off = collect_with_config(seq, /*use_pool=*/false, /*bytes=*/0);
    auto on  = collect_with_config(seq, /*use_pool=*/true, /*bytes=*/64 * 1024);

    assert(off.size() == on.size());
    sort_by_ab(off);
    sort_by_ab(on);

    for (size_t i = 0; i < off.size(); ++i) {
        assert(same_relation(off[i], on[i]));
    }

    std::cout << "  Small sequence (50 relations) bit-for-bit: PASS" << std::endl;
}

void test_medium_sequence_bit_for_bit() {
    std::cout << "Testing medium sequence: pool ON vs OFF bit-for-bit..." << std::endl;
    auto seq = make_deterministic_sequence(5000);

    auto off = collect_with_config(seq, /*use_pool=*/false, /*bytes=*/0);
    auto on  = collect_with_config(seq, /*use_pool=*/true, /*bytes=*/1 * 1024 * 1024);

    assert(off.size() == on.size());
    sort_by_ab(off);
    sort_by_ab(on);

    for (size_t i = 0; i < off.size(); ++i) {
        assert(same_relation(off[i], on[i]));
    }

    std::cout << "  Medium sequence (5000 relations) bit-for-bit: PASS" << std::endl;
}

void test_pool_stats_match_no_pool() {
    std::cout << "Testing stats() identical between pool ON / OFF..." << std::endl;
    auto seq = make_deterministic_sequence(200);

    CollectorConfig cfg_off;
    cfg_off.use_pool = false;
    RelationCollector c_off(cfg_off);
    for (const auto& f : seq) c_off.add(to_relation(f));

    CollectorConfig cfg_on;
    cfg_on.use_pool = true;
    cfg_on.pool_initial_bytes = 256 * 1024;
    RelationCollector c_on(cfg_on);
    for (const auto& f : seq) c_on.add(to_relation(f));

    auto s_off = c_off.stats();
    auto s_on  = c_on.stats();

    assert(s_off.total_relations == s_on.total_relations);
    assert(s_off.full_relations == s_on.full_relations);
    assert(s_off.partial_1lp == s_on.partial_1lp);
    assert(s_off.partial_2lp == s_on.partial_2lp);
    assert(s_off.duplicates_rejected == s_on.duplicates_rejected);
    assert(s_off.invalid_rejected == s_on.invalid_rejected);

    assert(c_off.size() == c_on.size());
    assert(c_off.empty() == c_on.empty());

    std::cout << "  Pool stats parity: PASS" << std::endl;
}

void test_clear_recycles_pool() {
    std::cout << "Testing clear() in pool mode recycles the pool..." << std::endl;

    CollectorConfig cfg;
    cfg.use_pool = true;
    cfg.pool_initial_bytes = 64 * 1024;
    RelationCollector collector(cfg);

    auto seq = make_deterministic_sequence(100);
    for (const auto& f : seq) collector.add(to_relation(f));
    assert(collector.size() == 100);

    collector.clear();
    assert(collector.size() == 0);
    assert(collector.empty());

    // After clear, collector still accepts new additions through the (new) pool.
    for (const auto& f : seq) collector.add(to_relation(f));
    assert(collector.size() == 100);

    auto out = collector.get_relations();
    assert(out.size() == 100);

    std::cout << "  Pool clear() recycle: PASS" << std::endl;
}

// ── Perf info probes (not asserts) ─────────────────────────────────────────
// Print push_back timing for pool ON vs OFF. Used for local human inspection
// to confirm the pool does not regress performance. Do not assert any ratio
// here — wallclock varies wildly under sanitizers and CI.

void perf_info_push_100k() {
    std::cout << "\n[perf-info] 100k push_back timing (pool OFF vs ON)..."
              << std::endl;
    constexpr int N = 100'000;
    auto seq = make_deterministic_sequence(N);

    auto run = [&](bool use_pool, size_t bytes) {
        CollectorConfig cfg;
        cfg.use_pool = use_pool;
        cfg.pool_initial_bytes = bytes;
        // Skip dedup in this probe so we measure only push cost
        cfg.check_duplicates = false;
        RelationCollector collector(cfg);
        auto t0 = std::chrono::steady_clock::now();
        for (const auto& f : seq) collector.add(to_relation(f));
        auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    };

    auto ms_off = run(false, 0);
    auto ms_on  = run(true, 4 * 1024 * 1024);

    std::cout << "  pool OFF: " << ms_off << " ms (" << N << " relations)" << std::endl;
    std::cout << "  pool ON : " << ms_on << " ms (initial chunk 4 MiB)" << std::endl;
    if (ms_off > 0) {
        double ratio = static_cast<double>(ms_on) / static_cast<double>(ms_off);
        std::cout << "  ratio   : " << ratio << " (ON/OFF, <1.0 = pool faster)" << std::endl;
    }
}

void perf_info_push_10k_warm() {
    std::cout << "\n[perf-info] 10k push_back warm timing (ENV-driven pool)..."
              << std::endl;
    constexpr int N = 10'000;
    auto seq = make_deterministic_sequence(N);

    auto run_label = [&](const char* label, bool use_pool, size_t bytes) {
        CollectorConfig cfg;
        cfg.use_pool = use_pool;
        cfg.pool_initial_bytes = bytes;
        cfg.check_duplicates = false;
        RelationCollector collector(cfg);
        auto t0 = std::chrono::steady_clock::now();
        for (const auto& f : seq) collector.add(to_relation(f));
        auto t1 = std::chrono::steady_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        std::cout << "  " << label << ": " << us << " us" << std::endl;
    };

    run_label("pool OFF      ", false, 0);
    run_label("pool 256 KiB  ", true, 256 * 1024);
    run_label("pool 1 MiB    ", true, 1 * 1024 * 1024);
    run_label("pool 4 MiB    ", true, 4 * 1024 * 1024);
}

int main() {
    std::cout << "=== Relation Pool Integration Tests (W6 T4) ===" << std::endl;

    // Ensure ENV does not contaminate test config (we drive use_pool directly)
    unsetenv("GNFS_RELATION_POOL_SIZE");

    // 4 correctness tests
    test_small_sequence_bit_for_bit();
    test_medium_sequence_bit_for_bit();
    test_pool_stats_match_no_pool();
    test_clear_recycles_pool();

    // 2 perf-info probes
    perf_info_push_100k();
    perf_info_push_10k_warm();

    std::cout << "\nAll relation pool integration tests passed!" << std::endl;
    return 0;
}
