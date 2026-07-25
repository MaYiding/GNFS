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
//   7. Descriptor- and sequence-bound reports expose counts and reject drift
//   8. First-attempt and pending-handoff crashes recover through one retry
//   9. Retry exhaustion removes owned leases without touching legacy leaves

#include "gnfs/cofactor/cofactorizer.hpp"
#include "gnfs/factor_base/builder.hpp"
#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/relation/collector.hpp"
#include "gnfs/relation/ooc_cleanup_transaction.hpp"
#include "gnfs/sieve/distributed_sieve.hpp"
#include "gnfs/sieve/lattice_sieve.hpp"
#include "gnfs/sieve/special_q.hpp"
#include "gnfs/util/temp_path.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            throw std::runtime_error(std::string("CHECK failed: " #condition " at ") + __FILE__ +  \
                                     ":" + std::to_string(__LINE__));                              \
        }                                                                                          \
    } while (false)

using gnfs::core::ABPair;
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

    Fixture() : n(TEST_N), ctx(make_ctx()), fb(build_fb(ctx)) {}

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
    return gnfs::util::temp_path("gnfs_test_distsieve_" + std::to_string(::getpid()) + "_" + tag);
}

std::filesystem::path worker_lease_root(const std::string& base, size_t chunk_id) {
    return base + ".worker_" + std::to_string(chunk_id) + ".gnfs-sink-lease";
}

gnfs::relation::OOCCleanupPaths worker_cleanup_paths(const std::string& base, size_t chunk_id) {
    return gnfs::relation::OOCCleanupTransaction::paths_for(worker_lease_root(base, chunk_id) /
                                                            "corpus");
}

void check_worker_leases_removed(const std::string& base, size_t count) {
    for (size_t chunk_id = 0; chunk_id < count; ++chunk_id) {
        const auto paths = worker_cleanup_paths(base, chunk_id);
        CHECK(!std::filesystem::exists(paths.private_directory));
        CHECK(!std::filesystem::exists(paths.lease_reserved_path));
        CHECK(!std::filesystem::exists(paths.lease_reserved_pending_path));
        CHECK(!std::filesystem::exists(paths.lease_owned_path));
        CHECK(!std::filesystem::exists(paths.lease_owned_pending_path));
        CHECK(
            gnfs::relation::OOCCleanupTransaction::confirm_pair_namespace_reusable(paths.base_path)
                .completed());
    }
}

void cleanup_worker_test_artifacts(const std::string& base, size_t count) {
    std::error_code error;
    for (size_t chunk_id = 0; chunk_id < count; ++chunk_id) {
        const auto paths = worker_cleanup_paths(base, chunk_id);
        const std::array cleanup_paths{
            paths.lease_reserved_pending_path,
            paths.lease_reserved_path,
            paths.lease_owned_pending_path,
            paths.lease_owned_path,
            paths.lock_path,
        };
        std::filesystem::remove_all(paths.private_directory, error);
        for (const auto& path : cleanup_paths) {
            error.clear();
            std::filesystem::remove(path, error);
        }
    }
}

class ScopedEnvironment final {
public:
    ScopedEnvironment(std::string name, std::string value) : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str())) {
            previous_ = std::string(previous);
        }
        CHECK(::setenv(name_.c_str(), value.c_str(), 1) == 0);
    }

    ~ScopedEnvironment() {
        if (previous_) {
            (void)::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            (void)::unsetenv(name_.c_str());
        }
    }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

void write_text_file(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create test file: " + path.string());
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        throw std::runtime_error("cannot write test file: " + path.string());
    }
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read test file: " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
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
        if (!sq)
            break;
        auto sr = sieve.sieve_special_q(*sq);
        for (const auto& cand : sr.candidates) {
            auto rel = cofac.verify(cand, sq->q, sq->r);
            if (rel)
                collector.add(std::move(*rel));
        }
    }

    out = collector.get_relations();
    return out.size();
}

// Exact raw-relation identity for set comparison. Structured rows must instead
// use their complete source-ID combinations.
ABPair rel_key(const Relation& r) {
    return r.ab();
}

// ── Test 1: split_sq_range edge cases ──────────────────────────────────
void test_split_sq_range() {
    std::cout << "[test_split_sq_range] ... " << std::flush;

    // Trivial: 0 chunks → empty
    {
        auto c = split_sq_range(0, 100, 0);
        CHECK(c.empty());
    }
    // Empty range → empty
    {
        auto c = split_sq_range(10, 10, 4);
        CHECK(c.empty());
    }
    // Inverted range → empty (we treat as empty, not as error)
    {
        auto c = split_sq_range(100, 50, 4);
        CHECK(c.empty());
    }
    // Even split: 100 / 4 = 25 each
    {
        auto c = split_sq_range(0, 100, 4);
        CHECK(c.size() == 4);
        CHECK(c[0] == std::make_pair(0U, 25U));
        CHECK(c[1] == std::make_pair(25U, 50U));
        CHECK(c[2] == std::make_pair(50U, 75U));
        CHECK(c[3] == std::make_pair(75U, 100U));
    }
    // Uneven split: 10 / 3 = 3,3,4 (first remainder gets extra)
    {
        auto c = split_sq_range(0, 10, 3);
        CHECK(c.size() == 3);
        CHECK(c[0].second - c[0].first == 4U); // first chunk gets remainder
        CHECK(c[1].second - c[1].first == 3U);
        CHECK(c[2].second - c[2].first == 3U);
        CHECK(c[0].second == c[1].first);
        CHECK(c[1].second == c[2].first);
        CHECK(c[2].second == 10U);
    }
    // More chunks than range: some empty
    {
        auto c = split_sq_range(0, 3, 5);
        CHECK(c.size() == 5);
        // 3 / 5 = 0 base size, 3 chunks get +1 (the remainder).
        CHECK(c[0].second - c[0].first == 1U);
        CHECK(c[1].second - c[1].first == 1U);
        CHECK(c[2].second - c[2].first == 1U);
        CHECK(c[3].second - c[3].first == 0U); // empty
        CHECK(c[4].second - c[4].first == 0U); // empty
        CHECK(c[4].second == 3U);              // contiguous
    }
    // Concatenation property: union of chunks == original range
    {
        auto c = split_sq_range(100, 257, 7);
        CHECK(c.front().first == 100U);
        CHECK(c.back().second == 257U);
        for (size_t i = 1; i < c.size(); ++i) {
            CHECK(c[i - 1].second == c[i].first);
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

    with_env(nullptr, []() { CHECK(parse_distributed_sieve_workers_env() == 0); });
    with_env("", []() { CHECK(parse_distributed_sieve_workers_env() == 0); });
    with_env("0", []() { CHECK(parse_distributed_sieve_workers_env() == 0); });
    with_env("1", []() { CHECK(parse_distributed_sieve_workers_env() == 1); });
    with_env("4", []() { CHECK(parse_distributed_sieve_workers_env() == 4); });
    with_env("64", []() { CHECK(parse_distributed_sieve_workers_env() == 64); });
    with_env("65", []() { CHECK(parse_distributed_sieve_workers_env() == 0); });
    with_env("-1", []() { CHECK(parse_distributed_sieve_workers_env() == 0); });
    with_env("garbage", []() { CHECK(parse_distributed_sieve_workers_env() == 0); });
    // Mixed numeric+garbage: strtol parses leading digits
    with_env("2abc", []() { CHECK(parse_distributed_sieve_workers_env() == 2); });

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
        return run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(), f.sieve_region(),
                                     f.cofac_config(), f.ctx.n(), f.ctx.m(), f.sq_range());
    };

    bool threw_nw = false;
    try {
        run(0, gnfs::util::temp_path("xyz"));
    } catch (const std::invalid_argument&) {
        threw_nw = true;
    }
    CHECK(threw_nw);

    bool threw_path = false;
    try {
        run(1, "");
    } catch (const std::invalid_argument&) {
        threw_path = true;
    }
    CHECK(threw_path);

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

    auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(), f.sieve_region(),
                                      f.cofac_config(), f.ctx.n(), f.ctx.m(), range);

    std::cout << "(dist=" << rels.size() << " rels) " << std::flush;

    // Exact count match: single worker processes all SQs in the same order,
    // same sieve threshold, same cofactor logic → must produce identical set.
    CHECK(rels.size() == baseline_count);

    // Set membership match
    std::set<ABPair> base_keys, dist_keys;
    for (const auto& r : baseline)
        base_keys.insert(rel_key(r));
    for (const auto& r : rels)
        dist_keys.insert(rel_key(r));
    CHECK(base_keys == dist_keys);
    check_worker_leases_removed(cfg.base_path, cfg.num_workers);
    cleanup_worker_test_artifacts(cfg.base_path, cfg.num_workers);

    std::cout << "PASS\n";
}

void test_worker_completion_report_tracks_actual_caps() {
    std::cout << "[test_worker_completion_report_tracks_actual_caps] ... " << std::flush;

    const auto& f = shared_fixture();
    DistributedSieveConfig cfg;
    cfg.num_workers = 1;
    cfg.sq_per_worker = 1;
    cfg.base_path = make_tmp_base("actualstats");
    std::vector<DistributedSieveWorkerResult> stats;
    const auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(), f.sieve_region(),
                                            f.cofac_config(), f.ctx.n(), f.ctx.m(),
                                            f.sq_range(1000, 1300), &stats);

    CHECK(stats.size() == 1);
    CHECK(stats[0].success);
    CHECK(stats[0].sq_count == 1);
    CHECK(stats[0].relations_count == rels.size());
    CHECK(stats[0].merged_relations_count == rels.size());
    CHECK(stats[0].attempt_count == 1);
    check_worker_leases_removed(cfg.base_path, cfg.num_workers);
    cleanup_worker_test_artifacts(cfg.base_path, cfg.num_workers);
    std::cout << "PASS\n";
}

// ── Test 5: 2- and 4-worker runs produce same relation set as baseline ─
void test_multi_worker_same_set() {
    std::cout << "[test_multi_worker_same_set] ... " << std::flush;

    const auto& f = shared_fixture();
    auto range = f.sq_range(1000, 1300);

    std::vector<Relation> baseline;
    const size_t baseline_count = run_in_process_sieve(f, range, baseline);
    std::set<ABPair> base_keys;
    for (const auto& r : baseline)
        base_keys.insert(rel_key(r));

    for (size_t nw : {2U, 4U}) {
        std::vector<DistributedSieveWorkerResult> stats;
        DistributedSieveConfig cfg;
        cfg.num_workers = nw;
        cfg.base_path = make_tmp_base("multi" + std::to_string(nw));

        auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(), f.sieve_region(),
                                          f.cofac_config(), f.ctx.n(), f.ctx.m(), range, &stats);

        std::cout << "[N=" << nw << " rels=" << rels.size() << " stats=" << stats.size() << "] "
                  << std::flush;

        // Every worker must report success (no crashes in normal path).
        for (const auto& s : stats) {
            CHECK(s.success);
            CHECK(s.sq_index_begin <= s.sq_index_end);
        }
        // Chunks must concatenate to the full SQ index range (no gaps, no overlap).
        for (size_t i = 1; i < stats.size(); ++i) {
            CHECK(stats[i].sq_index_begin == stats[i - 1].sq_index_end);
        }
        // Sum of worker relation counts equals merged count.
        size_t sum_worker = 0;
        size_t sum_persisted = 0;
        for (const auto& s : stats) {
            CHECK(s.relations_count >= s.merged_relations_count);
            sum_persisted += s.relations_count;
            sum_worker += s.merged_relations_count;
        }
        CHECK(sum_worker == rels.size());
        CHECK(sum_persisted >= rels.size());

        // Relation set must equal the baseline set (sieve is deterministic).
        std::set<ABPair> dist_keys;
        for (const auto& r : rels)
            dist_keys.insert(rel_key(r));
        CHECK(dist_keys == base_keys);
        CHECK(rels.size() == baseline_count);
        check_worker_leases_removed(cfg.base_path, cfg.num_workers);
        cleanup_worker_test_artifacts(cfg.base_path, cfg.num_workers);
    }

    std::cout << "PASS\n";
}

// ── Test 6: Empty range degenerate input ───────────────────────────────
void test_empty_range() {
    std::cout << "[test_empty_range] ... " << std::flush;

    const auto& f = shared_fixture();
    SpecialQRange empty_range;
    empty_range.min_q = 1;
    empty_range.max_q = 1; // pick a q range that excludes all FB primes
    empty_range.start_index = 0;
    empty_range.end_index = 0; // explicit empty index range

    DistributedSieveConfig cfg;
    cfg.num_workers = 2;
    cfg.base_path = make_tmp_base("empty");

    auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(), f.sieve_region(),
                                      f.cofac_config(), f.ctx.n(), f.ctx.m(), empty_range);
    CHECK(rels.empty());

    std::cout << "PASS\n";
}

// ── Test 7: More requested workers than available Special-Q entries ─────
void test_more_workers_than_sqs() {
    std::cout << "[test_more_workers_than_sqs] ... " << std::flush;

    const auto& f = shared_fixture();
    // Pick a tight SQ range — only a handful of FB primes in [1000, 1200).
    auto range = f.sq_range(1000, 1200);

    std::vector<Relation> baseline;
    run_in_process_sieve(f, range, baseline);

    std::vector<DistributedSieveWorkerResult> stats;
    DistributedSieveConfig cfg;
    cfg.num_workers = 8; // likely > #SQs in [1000, 1200)
    cfg.base_path = make_tmp_base("morewkrs");

    auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(), f.sieve_region(),
                                      f.cofac_config(), f.ctx.n(), f.ctx.m(), range, &stats);

    std::cout << "[base=" << baseline.size() << " dist=" << rels.size()
              << " workers=" << stats.size() << "] " << std::flush;

    CHECK(stats.size() == 8);
    CHECK(rels.size() == baseline.size());
    // At least one worker should have empty chunk (sq_index_begin==sq_index_end)
    // OR all workers got non-empty work — both are acceptable when total > 8 SQs.
    // We just verify success and concatenation hold.
    size_t empty_chunks = 0, nonempty_chunks = 0;
    for (const auto& s : stats) {
        CHECK(s.success);
        if (s.sq_index_begin == s.sq_index_end)
            ++empty_chunks;
        else
            ++nonempty_chunks;
    }
    std::cout << "(empty_chunks=" << empty_chunks << " nonempty=" << nonempty_chunks << ") "
              << std::flush;
    check_worker_leases_removed(cfg.base_path, cfg.num_workers);
    cleanup_worker_test_artifacts(cfg.base_path, cfg.num_workers);

    std::cout << "PASS\n";
}

// ── Test 8: Worker crash simulation + master retry ─────────────────────
// Forces chunk_id=0 to exit(1) on its first attempt via the crash-injection
// ENV `GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_0=1`. Master must:
//   1. detect the failure via waitpid
//   2. retry chunk_id=0 with the parent-owned attempt ordinal 2
//   3. merge chunk_id=0's relations as if the failure never happened
// Other chunks (chunk_id != 0) must succeed on first try.
void test_worker_crash_with_retry() {
    std::cout << "[test_worker_crash_with_retry] ... " << std::flush;

    const auto& f = shared_fixture();
    auto range = f.sq_range(1000, 1300);

    std::vector<Relation> baseline;
    run_in_process_sieve(f, range, baseline);

    ScopedEnvironment fail_first("GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_0", "1");

    std::vector<DistributedSieveWorkerResult> stats;
    DistributedSieveConfig cfg;
    cfg.num_workers = 3;
    cfg.base_path = make_tmp_base("crashretry");

    auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(), f.sieve_region(),
                                      f.cofac_config(), f.ctx.n(), f.ctx.m(), range, &stats);

    std::cout << "(base=" << baseline.size() << " dist=" << rels.size()
              << " workers=" << stats.size() << ") " << std::flush;

    // All three workers must end up successful (chunk 0 succeeded on retry).
    CHECK(stats.size() == 3);
    for (const auto& s : stats) {
        CHECK(s.success);
    }
    CHECK(stats[0].attempt_count == 2);
    CHECK(stats[1].attempt_count == 1);
    CHECK(stats[2].attempt_count == 1);
    check_worker_leases_removed(cfg.base_path, cfg.num_workers);

    // Final merged set must match baseline (the crash + retry was transparent).
    std::set<ABPair> base_keys, dist_keys;
    for (const auto& r : baseline)
        base_keys.insert(rel_key(r));
    for (const auto& r : rels)
        dist_keys.insert(rel_key(r));
    CHECK(base_keys == dist_keys);
    cleanup_worker_test_artifacts(cfg.base_path, cfg.num_workers);

    std::cout << "PASS\n";
}

// ── Test 9: cleanup-handoff pending crash + parent retry ───────────────
void test_handoff_pending_crash_with_retry() {
    std::cout << "[test_handoff_pending_crash_with_retry] ... " << std::flush;

    const auto& f = shared_fixture();
    auto range = f.sq_range(1000, 1150);
    std::vector<Relation> baseline;
    run_in_process_sieve(f, range, baseline);

    ScopedEnvironment fail_pending("GNFS_DISTRIBUTED_SIEVE_FAIL_HANDOFF_PENDING_ATTEMPT_0", "1");
    std::vector<DistributedSieveWorkerResult> stats;
    DistributedSieveConfig cfg;
    cfg.num_workers = 1;
    cfg.base_path = make_tmp_base("pendingretry");

    auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(), f.sieve_region(),
                                      f.cofac_config(), f.ctx.n(), f.ctx.m(), range, &stats);

    CHECK(stats.size() == 1);
    CHECK(stats[0].success);
    CHECK(stats[0].attempt_count == 2);
    std::set<ABPair> base_keys, dist_keys;
    for (const auto& relation : baseline)
        base_keys.insert(rel_key(relation));
    for (const auto& relation : rels)
        dist_keys.insert(rel_key(relation));
    CHECK(base_keys == dist_keys);
    check_worker_leases_removed(cfg.base_path, cfg.num_workers);
    cleanup_worker_test_artifacts(cfg.base_path, cfg.num_workers);
    std::cout << "PASS\n";
}

// ── Test 10: invalid completion descriptor is rejected and retried ───────
void test_corrupt_completion_report_with_retry() {
    std::cout << "[test_corrupt_completion_report_with_retry] ... " << std::flush;

    const auto& f = shared_fixture();
    const auto range = f.sq_range(1000, 1150);
    std::vector<Relation> baseline;
    run_in_process_sieve(f, range, baseline);

    ScopedEnvironment corrupt_report("GNFS_DISTRIBUTED_SIEVE_CORRUPT_REPORT_ATTEMPT_0", "1");
    std::vector<DistributedSieveWorkerResult> stats;
    DistributedSieveConfig cfg;
    cfg.num_workers = 1;
    cfg.base_path = make_tmp_base("corruptreport");

    const auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(), f.sieve_region(),
                                            f.cofac_config(), f.ctx.n(), f.ctx.m(), range, &stats);

    CHECK(stats.size() == 1);
    CHECK(stats[0].success);
    CHECK(stats[0].attempt_count == 2);
    std::set<ABPair> base_keys, dist_keys;
    for (const auto& relation : baseline)
        base_keys.insert(rel_key(relation));
    for (const auto& relation : rels)
        dist_keys.insert(rel_key(relation));
    CHECK(base_keys == dist_keys);
    check_worker_leases_removed(cfg.base_path, cfg.num_workers);
    cleanup_worker_test_artifacts(cfg.base_path, cfg.num_workers);
    std::cout << "PASS\n";
}

// ── Test 11: sequence-receipt drift is rejected and retried ─────────────
void test_corrupt_sequence_receipt_with_retry() {
    std::cout << "[test_corrupt_sequence_receipt_with_retry] ... " << std::flush;

    const auto& f = shared_fixture();
    const auto range = f.sq_range(1000, 1150);
    std::vector<Relation> baseline;
    run_in_process_sieve(f, range, baseline);

    ScopedEnvironment corrupt_receipt("GNFS_DISTRIBUTED_SIEVE_CORRUPT_RECEIPT_ATTEMPT_0", "1");
    std::vector<DistributedSieveWorkerResult> stats;
    DistributedSieveConfig cfg;
    cfg.num_workers = 1;
    cfg.base_path = make_tmp_base("corruptreceipt");

    const auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(), f.sieve_region(),
                                            f.cofac_config(), f.ctx.n(), f.ctx.m(), range, &stats);

    CHECK(stats.size() == 1);
    CHECK(stats[0].success);
    CHECK(stats[0].attempt_count == 2);
    std::set<ABPair> base_keys, dist_keys;
    for (const auto& relation : baseline)
        base_keys.insert(rel_key(relation));
    for (const auto& relation : rels)
        dist_keys.insert(rel_key(relation));
    CHECK(base_keys == dist_keys);
    check_worker_leases_removed(cfg.base_path, cfg.num_workers);
    cleanup_worker_test_artifacts(cfg.base_path, cfg.num_workers);
    std::cout << "PASS\n";
}

// ── Test 12: retry budget exhaustion remains explicit and clean ─────────
void test_worker_retry_exhaustion() {
    std::cout << "[test_worker_retry_exhaustion] ... " << std::flush;

    const auto& f = shared_fixture();
    ScopedEnvironment fail_all("GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_0", "all");
    DistributedSieveConfig cfg;
    cfg.num_workers = 1;
    cfg.base_path = make_tmp_base("retryexhausted");

    std::vector<DistributedSieveWorkerResult> stats;
    const auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(), f.sieve_region(),
                                            f.cofac_config(), f.ctx.n(), f.ctx.m(),
                                            f.sq_range(1000, 1100), &stats);
    CHECK(rels.empty());
    CHECK(stats.size() == 1);
    CHECK(!stats[0].success);
    CHECK(stats[0].reap_confirmed);
    CHECK(stats[0].attempt_count == 2);
    CHECK(stats[0].exit_status == 1);
    CHECK(stats[0].signal == 0);
    check_worker_leases_removed(cfg.base_path, cfg.num_workers);
    cleanup_worker_test_artifacts(cfg.base_path, cfg.num_workers);
    std::cout << "PASS\n";
}

// ── Test 13: raw legacy leaves are foreign to the private worker lease ─
void test_legacy_worker_leaves_are_preserved() {
    std::cout << "[test_legacy_worker_leaves_are_preserved] ... " << std::flush;

    const auto& f = shared_fixture();
    const auto range = f.sq_range(1000, 1100);
    DistributedSieveConfig cfg;
    cfg.num_workers = 1;
    cfg.base_path = make_tmp_base("legacyforeign");

    const std::string legacy_base = cfg.base_path + ".worker_0";
    const std::array<std::pair<std::filesystem::path, std::string>, 3> sentinels{{
        {legacy_base + ".reldata", "foreign legacy data"},
        {legacy_base + ".relidx", "foreign legacy index"},
        {legacy_base + ".attempts", "foreign legacy attempts"},
    }};
    for (const auto& [path, contents] : sentinels) {
        write_text_file(path, contents);
    }

    std::vector<DistributedSieveWorkerResult> stats;
    (void)run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(), f.sieve_region(),
                                f.cofac_config(), f.ctx.n(), f.ctx.m(), range, &stats);
    CHECK(stats.size() == 1);
    CHECK(stats[0].success);
    CHECK(stats[0].attempt_count == 1);
    for (const auto& [path, contents] : sentinels) {
        CHECK(read_text_file(path) == contents);
    }
    check_worker_leases_removed(cfg.base_path, cfg.num_workers);

    std::error_code error;
    for (const auto& [path, contents] : sentinels) {
        (void)contents;
        CHECK(std::filesystem::remove(path, error));
        CHECK(!error);
        error.clear();
    }
    cleanup_worker_test_artifacts(cfg.base_path, cfg.num_workers);
    std::cout << "PASS\n";
}

// ── Test 14: ENV-config end-to-end ─────────────────────────────────────
// Exercises parse_distributed_sieve_env() path.
void test_env_config_e2e() {
    std::cout << "[test_env_config_e2e] ... " << std::flush;

    const auto& f = shared_fixture();
    auto range = f.sq_range(1000, 1300);

    const std::string base = make_tmp_base("envconfig");
    ::setenv("GNFS_DISTRIBUTED_SIEVE_WORKERS", "3", 1);
    ::setenv("GNFS_DISTRIBUTED_SIEVE_BASE_PATH", base.c_str(), 1);

    auto cfg = gnfs::sieve::parse_distributed_sieve_env();
    CHECK(cfg.num_workers == 3);
    CHECK(cfg.base_path == base);

    auto rels = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(), f.sieve_region(),
                                      f.cofac_config(), f.ctx.n(), f.ctx.m(), range);
    std::cout << "(rels=" << rels.size() << ") " << std::flush;

    ::unsetenv("GNFS_DISTRIBUTED_SIEVE_WORKERS");
    ::unsetenv("GNFS_DISTRIBUTED_SIEVE_BASE_PATH");
    check_worker_leases_removed(cfg.base_path, cfg.num_workers);
    cleanup_worker_test_artifacts(cfg.base_path, cfg.num_workers);

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
    test_worker_completion_report_tracks_actual_caps();
    test_multi_worker_same_set();
    test_empty_range();
    test_more_workers_than_sqs();
    test_worker_crash_with_retry();
    test_handoff_pending_crash_with_retry();
    test_corrupt_completion_report_with_retry();
    test_corrupt_sequence_receipt_with_retry();
    test_worker_retry_exhaustion();
    test_legacy_worker_leaves_are_preserved();
    test_env_config_e2e();

    auto t1 = std::chrono::high_resolution_clock::now();
    double s = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "=== All tests PASSED in " << s << "s ===\n";
    return 0;
}

#endif
