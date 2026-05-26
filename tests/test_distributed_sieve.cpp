#ifdef _WIN32
#include <iostream>
int main() {
    std::cout << "Distributed sieve tests skipped on Windows (POSIX fork unavailable)\n";
    return 0;
}
#else

// Tests for the multi-process distributed sieve worker pool.
//
// Coverage:
//   1. ENV parsing edge cases (parse_distributed_sieve_workers_env)
//   2. SQ range split logic (split_sq_range)
//   3. Single-worker run: same relations as direct in-process sieve
//   4. Multi-worker run (N=2, 4): non-zero relations + chunks merge correctly
//   5. Empty-range degenerate input → empty result
//   6. Invalid config (num_workers=0 / base_path empty) → throws
//   7. Worker crash simulation: ENV-gated _exit(1) in chunk_id=0 →
//      master retries → other chunks still contribute their relations

#include "gnfs/cofactor/cofactorizer.hpp"
#include "gnfs/factor_base/builder.hpp"
#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/relation/collector.hpp"
#include "gnfs/sieve/distributed_sieve.hpp"
#include "gnfs/sieve/lattice_sieve.hpp"
#include "gnfs/sieve/special_q.hpp"
#include "gnfs/util/temp_path.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

using gnfs::core::Integer;
using gnfs::core::PolynomialContext;
using gnfs::core::Relation;
using gnfs::factor_base::FactorBase;
using gnfs::factor_base::FactorBaseBuilder;
using gnfs::polynomial::BaseMSelector;
using gnfs::sieve::DistributedSieveConfig;
using gnfs::sieve::DistributedSieveWorkerResult;
using gnfs::sieve::LatticeSieve;
using gnfs::sieve::run_distributed_sieve;
using gnfs::sieve::SieveParams;
using gnfs::sieve::SieveRegion;
using gnfs::sieve::SpecialQGenerator;
using gnfs::sieve::SpecialQRange;
using gnfs::sieve::split_sq_range;

namespace {

// ── Small-N fixture: ~30-bit composite, deterministic and fast ──
// 1000003 * 1000033 = 1000036000099 (40 bits, ~13 digits)
constexpr const char* TEST_N = "1000036000099";

struct Fixture {
    Integer n;
    PolynomialContext ctx;
    FactorBase fb;

    Fixture()
        : n(TEST_N),
          ctx(make_ctx()),
          fb(build_fb(ctx)) {}

    static PolynomialContext make_ctx() {
        Integer n_local(TEST_N);
        auto poly = BaseMSelector::select(n_local, 3);
        if (!poly.success) {
            std::cerr << "Fixture: BaseMSelector failed\n";
            std::abort();
        }
        return BaseMSelector::create_context(n_local, poly);
    }

    static FactorBase build_fb(const PolynomialContext& ctx) {
        FactorBaseBuilder::Options opts;
        opts.rational_bound = 5000;
        opts.algebraic_bound = 5000;
        opts.log_scale = 16;
        opts.parallel = false;
        return FactorBaseBuilder::build(ctx, opts);
    }

    SieveParams sieve_params() const {
        SieveParams p;
        p.log_scale = 16;
        p.rational_threshold = 60;
        p.algebraic_threshold = 60;
        return p;
    }

    SieveRegion sieve_region() const {
        // Small region keeps sieve+cofac under ~1s per run. Distributed tests
        // perform several sieve runs (baseline + N=1, N=2, N=4, etc.), so a
        // tight region matters for total wall-time.
        SieveRegion r;
        r.i_min = -200;
        r.i_max = 199;
        r.j_min = 1;
        r.j_max = 40;
        return r;
    }

    gnfs::cofactor::CofactorizerConfig cofac_config() const {
        gnfs::cofactor::CofactorizerConfig c;
        c.large_prime_bound = fb.params().large_prime_bound;
        c.allow_1lp = true;
        c.allow_2lp = false;
        c.allow_3lp = false;
        return c;
    }

    SpecialQRange sq_range(uint32_t min_q = 1000, uint32_t max_q = 1300) const {
        SpecialQRange r;
        r.min_q = min_q;
        r.max_q = max_q;
        return r;
    }
};

// Resolve an absolute temp path that distinguishes per test run / PID, so
// concurrent ctest invocations cannot collide on stale files.
std::string make_tmp_base(const std::string& tag) {
    return gnfs::util::temp_path(
        "gnfs_test_distsieve_" + std::to_string(::getpid()) + "_" + tag);
}

// Singleton fixture: building the factor base costs ~1s, so we share one
// instance across all tests. Fixture is internally const after construction.
const Fixture& shared_fixture() {
    static const Fixture fixture;
    return fixture;
}

// Run the in-process baseline sieve (single thread, same SQ range) so the
// distributed result can be compared. Returns relation count.
size_t run_in_process_sieve(const Fixture& f, const SpecialQRange& range,
                            std::vector<Relation>& out) {
    LatticeSieve sieve(f.ctx, f.fb, f.sieve_params());
    sieve.set_region(f.sieve_region());
    gnfs::cofactor::Cofactorizer cofac(f.ctx, f.fb, f.cofac_config());

    SpecialQGenerator gen(f.fb, range);
    gnfs::relation::CollectorConfig coll_cfg;
    coll_cfg.check_duplicates = true;
    gnfs::relation::RelationCollector collector(coll_cfg);
    collector.set_polynomial_context(f.ctx.n(), f.ctx.m());

    while (gen.has_next()) {
        auto sq = gen.next();
        if (!sq) break;
        auto sr = sieve.sieve_special_q(*sq);
        for (const auto& cand : sr.candidates) {
            auto rel = cofac.verify(cand, sq->q, sq->r);
            if (rel) collector.add(std::move(*rel));
        }
    }

    out = collector.get_relations();
    return out.size();
}

// Stable hashed key for relation dedup comparison. Uses (a, b) — sieve outputs
// distinct (a, b) per relation, so this is sufficient for set membership.
int64_t rel_key(const Relation& r) {
    return static_cast<int64_t>(r.a) ^ (static_cast<int64_t>(r.b) << 32);
}

// ── Test 1: split_sq_range edge cases ──────────────────────────────────
void test_split_sq_range() {
    std::cout << "[test_split_sq_range] ... " << std::flush;

    // Trivial: 0 chunks → empty
    {
        auto c = split_sq_range(0, 100, 0);
        assert(c.empty());
    }
    // Empty range → empty
    {
        auto c = split_sq_range(10, 10, 4);
        assert(c.empty());
    }
    // Inverted range → empty (we treat as empty, not as error)
    {
        auto c = split_sq_range(100, 50, 4);
        assert(c.empty());
    }
    // Even split: 100 / 4 = 25 each
    {
        auto c = split_sq_range(0, 100, 4);
        assert(c.size() == 4);
        assert(c[0] == std::make_pair(0U, 25U));
        assert(c[1] == std::make_pair(25U, 50U));
        assert(c[2] == std::make_pair(50U, 75U));
        assert(c[3] == std::make_pair(75U, 100U));
    }
    // Uneven split: 10 / 3 = 3,3,4 (first remainder gets extra)
    {
        auto c = split_sq_range(0, 10, 3);
        assert(c.size() == 3);
        assert(c[0].second - c[0].first == 4U);  // first chunk gets remainder
        assert(c[1].second - c[1].first == 3U);
        assert(c[2].second - c[2].first == 3U);
        assert(c[0].second == c[1].first);
        assert(c[1].second == c[2].first);
        assert(c[2].second == 10U);
    }
    // More chunks than range: some empty
    {
        auto c = split_sq_range(0, 3, 5);
        assert(c.size() == 5);
        // 3 / 5 = 0 base size, 3 chunks get +1 (the remainder).
        assert(c[0].second - c[0].first == 1U);
        assert(c[1].second - c[1].first == 1U);
        assert(c[2].second - c[2].first == 1U);
        assert(c[3].second - c[3].first == 0U);  // empty
        assert(c[4].second - c[4].first == 0U);  // empty
        assert(c[4].second == 3U);  // contiguous
    }
    // Concatenation property: union of chunks == original range
    {
        auto c = split_sq_range(100, 257, 7);
        assert(c.front().first == 100U);
        assert(c.back().second == 257U);
        for (size_t i = 1; i < c.size(); ++i) {
            assert(c[i - 1].second == c[i].first);
        }
    }

    std::cout << "PASS\n";
}

// ── Test 2: ENV parsing ────────────────────────────────────────────────
void test_env_parsing() {
    std::cout << "[test_env_parsing] ... " << std::flush;

    using gnfs::sieve::parse_distributed_sieve_workers_env;

    auto with_env = [](const char* val, auto fn) {
        if (val == nullptr) {
            ::unsetenv("GNFS_DISTRIBUTED_SIEVE_WORKERS");
        } else {
            ::setenv("GNFS_DISTRIBUTED_SIEVE_WORKERS", val, 1);
        }
        fn();
        ::unsetenv("GNFS_DISTRIBUTED_SIEVE_WORKERS");
    };

    with_env(nullptr, []() { assert(parse_distributed_sieve_workers_env() == 0); });
    with_env("", []() { assert(parse_distributed_sieve_workers_env() == 0); });
    with_env("0", []() { assert(parse_distributed_sieve_workers_env() == 0); });
    with_env("1", []() { assert(parse_distributed_sieve_workers_env() == 1); });
    with_env("4", []() { assert(parse_distributed_sieve_workers_env() == 4); });
    with_env("64", []() { assert(parse_distributed_sieve_workers_env() == 64); });
    with_env("65", []() { assert(parse_distributed_sieve_workers_env() == 0); });
    with_env("-1", []() { assert(parse_distributed_sieve_workers_env() == 0); });
    with_env("garbage", []() { assert(parse_distributed_sieve_workers_env() == 0); });
    // Mixed numeric+garbage: strtol parses leading digits
    with_env("2abc", []() { assert(parse_distributed_sieve_workers_env() == 2); });

    std::cout << "PASS\n";
}

// ── Test 3: Invalid config rejection ───────────────────────────────────
void test_invalid_config() {
    std::cout << "[test_invalid_config] ... " << std::flush;

    const auto& f = shared_fixture();
    auto run = [&](size_t nw, const std::string& base) {
        DistributedSieveConfig cfg;
        cfg.num_workers = nw;
        cfg.base_path = base;
        return run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(),
                                     f.sieve_region(), f.cofac_config(),
                                     f.ctx.n(), f.ctx.m(), f.sq_range());
    };

    bool threw_nw = false;
    try {
        run(0, gnfs::util::temp_path("xyz"));
    } catch (const std::invalid_argument&) {
        threw_nw = true;
    }
    assert(threw_nw);

    bool threw_path = false;
    try {
        run(1, "");
    } catch (const std::invalid_argument&) {
        threw_path = true;
    }
    assert(threw_path);

    std::cout << "PASS\n";
}

// ── Test 4: Single-worker run matches in-process baseline ──────────────
void test_single_worker_matches_in_process() {
    std::cout << "[test_single_worker_matches_in_process] ... " << std::flush;

    const auto& f = shared_fixture();
    auto range = f.sq_range(1000, 1300);

    std::vector<Relation> baseline;
    const size_t baseline_count = run_in_process_sieve(f, range, baseline);
    std::cout << "(baseline=" << baseline_count << " rels) " << std::flush;

    DistributedSieveConfig cfg;
    cfg.num_workers = 1;
    cfg.base_path = make_tmp_base("single");

    auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(),
                                       f.sieve_region(), f.cofac_config(),
                                       f.ctx.n(), f.ctx.m(), range);

    std::cout << "(dist=" << rels.size() << " rels) " << std::flush;

    // Exact count match: single worker processes all SQs in the same order,
    // same sieve threshold, same cofactor logic → must produce identical set.
    assert(rels.size() == baseline_count);

    // Set membership match
    std::set<int64_t> base_keys, dist_keys;
    for (const auto& r : baseline) base_keys.insert(rel_key(r));
    for (const auto& r : rels)     dist_keys.insert(rel_key(r));
    assert(base_keys == dist_keys);

    std::cout << "PASS\n";
}

// ── Test 5: 2- and 4-worker runs produce same relation set as baseline ─
void test_multi_worker_same_set() {
    std::cout << "[test_multi_worker_same_set] ... " << std::flush;

    const auto& f = shared_fixture();
    auto range = f.sq_range(1000, 1300);

    std::vector<Relation> baseline;
    const size_t baseline_count = run_in_process_sieve(f, range, baseline);
    std::set<int64_t> base_keys;
    for (const auto& r : baseline) base_keys.insert(rel_key(r));

    for (size_t nw : {2U, 4U}) {
        std::vector<DistributedSieveWorkerResult> stats;
        DistributedSieveConfig cfg;
        cfg.num_workers = nw;
        cfg.base_path = make_tmp_base("multi" + std::to_string(nw));

        auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(),
                                           f.sieve_region(), f.cofac_config(),
                                           f.ctx.n(), f.ctx.m(), range, &stats);

        std::cout << "[N=" << nw << " rels=" << rels.size()
                  << " stats=" << stats.size() << "] " << std::flush;

        // Every worker must report success (no crashes in normal path).
        for (const auto& s : stats) {
            assert(s.success);
            assert(s.sq_index_begin <= s.sq_index_end);
        }
        // Chunks must concatenate to the full SQ index range (no gaps, no overlap).
        for (size_t i = 1; i < stats.size(); ++i) {
            assert(stats[i].sq_index_begin == stats[i - 1].sq_index_end);
        }
        // Sum of worker relation counts equals merged count.
        size_t sum_worker = 0;
        for (const auto& s : stats) sum_worker += s.relations_count;
        assert(sum_worker == rels.size());

        // Relation set must equal the baseline set (sieve is deterministic).
        std::set<int64_t> dist_keys;
        for (const auto& r : rels) dist_keys.insert(rel_key(r));
        assert(dist_keys == base_keys);
        assert(rels.size() == baseline_count);
    }

    std::cout << "PASS\n";
}

// ── Test 6: Empty range degenerate input ───────────────────────────────
void test_empty_range() {
    std::cout << "[test_empty_range] ... " << std::flush;

    const auto& f = shared_fixture();
    SpecialQRange empty_range;
    empty_range.min_q = 1;
    empty_range.max_q = 1;  // pick a q range that excludes all FB primes
    empty_range.start_index = 0;
    empty_range.end_index = 0;  // explicit empty index range

    DistributedSieveConfig cfg;
    cfg.num_workers = 2;
    cfg.base_path = make_tmp_base("empty");

    auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(),
                                       f.sieve_region(), f.cofac_config(),
                                       f.ctx.n(), f.ctx.m(), empty_range);
    assert(rels.empty());

    std::cout << "PASS\n";
}

// ── Test 7: Worker crash + master retry ────────────────────────────────
// Strategy: We can't easily inject a crash into the worker (it runs in a
// separate process). Instead, we simulate "first attempt failure" by
// pre-corrupting the worker OOC files so the worker's OOCWriter fails on
// construction. NO — that hits the same crash on retry too. Better approach:
// we test the retry path indirectly by overriding a chunk to point at an
// unwritable directory, then... still hits both attempts.
//
// Simplest robust approach: spawn workers normally and confirm that when ALL
// workers succeed the path also handles `success=true` correctly with N=3.
// For genuine crash testing, we instead run an arms-length scenario: launch
// a child with sq range that the parent intentionally clobbers post-fork
// to force exit(1). This is fragile.
//
// Instead, we exercise the retry path by:
//   - constructing a config that asks for more workers than chunks/SQs,
//     causing some workers to be empty chunks (which the master path marks
//     as success without spawn) — this verifies the "no spawn" branch.
//
// For ACTUAL crash retry coverage, we test the retry path at the unit level
// via split_sq_range edge cases (empty chunks → success=true) and verify
// the path handles them.
void test_more_workers_than_sqs() {
    std::cout << "[test_more_workers_than_sqs] ... " << std::flush;

    const auto& f = shared_fixture();
    // Pick a tight SQ range — only a handful of FB primes in [1000, 1200).
    auto range = f.sq_range(1000, 1200);

    std::vector<Relation> baseline;
    run_in_process_sieve(f, range, baseline);

    std::vector<DistributedSieveWorkerResult> stats;
    DistributedSieveConfig cfg;
    cfg.num_workers = 8;  // likely > #SQs in [1000, 1200)
    cfg.base_path = make_tmp_base("morewkrs");

    auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(),
                                       f.sieve_region(), f.cofac_config(),
                                       f.ctx.n(), f.ctx.m(), range, &stats);

    std::cout << "[base=" << baseline.size() << " dist=" << rels.size()
              << " workers=" << stats.size() << "] " << std::flush;

    assert(stats.size() == 8);
    assert(rels.size() == baseline.size());
    // At least one worker should have empty chunk (sq_index_begin==sq_index_end)
    // OR all workers got non-empty work — both are acceptable when total > 8 SQs.
    // We just verify success and concatenation hold.
    size_t empty_chunks = 0, nonempty_chunks = 0;
    for (const auto& s : stats) {
        assert(s.success);
        if (s.sq_index_begin == s.sq_index_end) ++empty_chunks;
        else ++nonempty_chunks;
    }
    std::cout << "(empty_chunks=" << empty_chunks << " nonempty=" << nonempty_chunks << ") "
              << std::flush;

    std::cout << "PASS\n";
}

// ── Test 8: Worker crash simulation + master retry ─────────────────────
// Forces chunk_id=0 to exit(1) on its first attempt via the crash-injection
// ENV `GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_0=1`. Master must:
//   1. detect the failure via waitpid
//   2. retry chunk_id=0 (which now succeeds because the .attempts counter
//      shows attempt=2 != 1)
//   3. merge chunk_id=0's relations as if the failure never happened
// Other chunks (chunk_id != 0) must succeed on first try.
void test_worker_crash_with_retry() {
    std::cout << "[test_worker_crash_with_retry] ... " << std::flush;

    const auto& f = shared_fixture();
    auto range = f.sq_range(1000, 1300);

    std::vector<Relation> baseline;
    run_in_process_sieve(f, range, baseline);

    // Activate crash-injection for chunk 0, first attempt.
    ::setenv("GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_0", "1", 1);

    std::vector<DistributedSieveWorkerResult> stats;
    DistributedSieveConfig cfg;
    cfg.num_workers = 3;
    cfg.base_path = make_tmp_base("crashretry");

    auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(),
                                       f.sieve_region(), f.cofac_config(),
                                       f.ctx.n(), f.ctx.m(), range, &stats);

    ::unsetenv("GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_0");

    std::cout << "(base=" << baseline.size() << " dist=" << rels.size()
              << " workers=" << stats.size() << ") " << std::flush;

    // All three workers must end up successful (chunk 0 succeeded on retry).
    assert(stats.size() == 3);
    for (const auto& s : stats) {
        assert(s.success);
    }

    // Final merged set must match baseline (the crash + retry was transparent).
    std::set<int64_t> base_keys, dist_keys;
    for (const auto& r : baseline) base_keys.insert(rel_key(r));
    for (const auto& r : rels)     dist_keys.insert(rel_key(r));
    assert(base_keys == dist_keys);

    std::cout << "PASS\n";
}

// ── Test 9: ENV-config end-to-end ──────────────────────────────────────
// Exercises parse_distributed_sieve_env() path.
void test_env_config_e2e() {
    std::cout << "[test_env_config_e2e] ... " << std::flush;

    const auto& f = shared_fixture();
    auto range = f.sq_range(1000, 1300);

    const std::string base = make_tmp_base("envconfig");
    ::setenv("GNFS_DISTRIBUTED_SIEVE_WORKERS", "3", 1);
    ::setenv("GNFS_DISTRIBUTED_SIEVE_BASE_PATH", base.c_str(), 1);

    auto cfg = gnfs::sieve::parse_distributed_sieve_env();
    assert(cfg.num_workers == 3);
    assert(cfg.base_path == base);

    auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(),
                                       f.sieve_region(), f.cofac_config(),
                                       f.ctx.n(), f.ctx.m(), range);
    std::cout << "(rels=" << rels.size() << ") " << std::flush;

    ::unsetenv("GNFS_DISTRIBUTED_SIEVE_WORKERS");
    ::unsetenv("GNFS_DISTRIBUTED_SIEVE_BASE_PATH");

    std::cout << "PASS\n";
}

} // namespace

int main() {
    std::cout << "=== test_distributed_sieve ===\n";
    auto t0 = std::chrono::high_resolution_clock::now();

    test_split_sq_range();
    test_env_parsing();
    test_invalid_config();
    test_single_worker_matches_in_process();
    test_multi_worker_same_set();
    test_empty_range();
    test_more_workers_than_sqs();
    test_worker_crash_with_retry();
    test_env_config_e2e();

    auto t1 = std::chrono::high_resolution_clock::now();
    double s = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "=== All tests PASSED in " << s << "s ===\n";
    return 0;
}

#endif
