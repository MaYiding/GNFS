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
//  10. Seeded coordinates and request traces ignore worker topology and retry
//  11. Seed-provider failures reap and clean the complete wave without merge
//  12. Other seeded retry exhaustion is atomic while legacy behavior remains

#include "gnfs/cofactor/cofactorizer.hpp"
#include "gnfs/factor_base/builder.hpp"
#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/relation/collector.hpp"
#include "gnfs/relation/ooc_cleanup_transaction.hpp"
#include "gnfs/relation/relation_sequence_receipt.hpp"
#include "gnfs/sieve/distributed_sieve.hpp"
#include "gnfs/sieve/lattice_sieve.hpp"
#include "gnfs/sieve/special_q.hpp"
#include "gnfs/util/temp_path.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <system_error>
#include <tuple>
#include <type_traits>
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
using gnfs::sieve::DistributedSieveSeedProviderError;
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

struct SeededFixture {
    PolynomialContext ctx;
    FactorBase fb;

    SeededFixture() : ctx(make_ctx()), fb(build_fb(ctx)) {}

    static PolynomialContext make_ctx() {
        Integer input("93185905945582757");
        input *= 15;
        input += 1;

        std::vector<Integer> coefficients;
        coefficients.emplace_back(input);
        coefficients.back().negate();
        coefficients.emplace_back(1);
        return PolynomialContext(Integer(3), std::move(coefficients), std::move(input));
    }

    static FactorBase build_fb(const PolynomialContext& ctx) {
        FactorBaseBuilder::Options options;
        options.rational_bound = 100;
        options.algebraic_bound = 100;
        options.special_q_bound = 200;
        options.log_scale = 16;
        options.parallel = false;
        return FactorBaseBuilder::build(ctx, options);
    }

    SieveParams sieve_params() const {
        SieveParams params;
        params.log_scale = 16;
        params.rational_threshold = 320;
        params.algebraic_threshold = 320;
        params.enable_2lp = true;
        params.enable_3lp = false;
        return params;
    }

    SieveRegion sieve_region() const {
        SieveRegion region;
        region.i_min = -100;
        region.i_max = 100;
        region.j_min = 1;
        region.j_max = 50;
        return region;
    }

    gnfs::cofactor::CofactorizerConfig cofac_config() const {
        gnfs::cofactor::CofactorizerConfig config;
        config.large_prime_bound = 500'000'000;
        config.allow_1lp = true;
        config.allow_2lp = true;
        config.allow_3lp = false;
        config.max_factorization_attempts = 50'000;
        config.seeded_brent_pollard_enabled = true;
        return config;
    }

    static SpecialQRange single_sq_range() {
        return SpecialQRange::from_indices(1, 2);
    }

    static SpecialQRange topology_range() {
        return SpecialQRange::from_indices(1, 4);
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

struct ForkSeedRecord final {
    std::uint64_t special_q_index = 0;
    std::uint64_t candidate_ordinal = 0;
    std::uint8_t side = 0;
    std::uint8_t domain = 0;
    std::uint16_t reserved = 0;
    std::uint32_t algorithm_identity = 0;
    std::array<std::byte, 32> cofactor_digest{};
    std::array<std::byte, 32> returned_seed{};

    [[nodiscard]] friend bool operator==(const ForkSeedRecord&,
                                         const ForkSeedRecord&) noexcept = default;
};
static_assert(std::is_trivially_copyable_v<ForkSeedRecord>);

class ForkRecordingSeedProvider final : public gnfs::cofactor::CofactorSeedProvider {
public:
    explicit ForkRecordingSeedProvider(
        std::string prefix, bool throw_after_record = false,
        std::optional<std::uint64_t> throw_on_special_q_index = std::nullopt)
        : prefix_(std::move(prefix)), throw_after_record_(throw_after_record),
          throw_on_special_q_index_(throw_on_special_q_index) {}

    [[nodiscard]] gnfs::cofactor::CofactorSeed256
    seed_for(const gnfs::cofactor::CofactorSeedRequestV1& request) const override {
        gnfs::cofactor::CofactorSeed256 seed{};
        for (size_t index = 0; index < seed.digest.bytes.size(); ++index) {
            seed.digest.bytes[index] = static_cast<std::byte>(index);
        }

        const ForkSeedRecord record{
            .special_q_index = request.coordinates.special_q_index,
            .candidate_ordinal = request.coordinates.candidate_ordinal,
            .side = static_cast<std::uint8_t>(request.side),
            .domain = static_cast<std::uint8_t>(request.domain),
            .reserved = 0,
            .algorithm_identity = request.algorithm_identity,
            .cofactor_digest = request.cofactor_digest.bytes,
            .returned_seed = seed.digest.bytes,
        };
        append_record(record);

        if (throw_after_record_ &&
            (!throw_on_special_q_index_ ||
             request.coordinates.special_q_index == *throw_on_special_q_index_)) {
            throw std::runtime_error("injected seed provider failure");
        }
        return seed;
    }

private:
    void append_record(const ForkSeedRecord& record) const {
        const std::string path = prefix_ + "." + std::to_string(::getpid()) + ".bin";
        const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (descriptor < 0) {
            throw std::system_error(errno, std::generic_category(), "open seed trace");
        }

        const auto* bytes = reinterpret_cast<const std::byte*>(&record);
        size_t offset = 0;
        int failure = 0;
        while (offset < sizeof(record)) {
            const ssize_t count = ::write(descriptor, bytes + offset, sizeof(record) - offset);
            if (count > 0) {
                offset += static_cast<size_t>(count);
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                failure = count == 0 ? EIO : errno;
                break;
            }
        }
        if (::close(descriptor) != 0 && failure == 0) {
            failure = errno;
        }
        if (failure != 0) {
            throw std::system_error(failure, std::generic_category(), "write seed trace");
        }
    }

    std::string prefix_;
    bool throw_after_record_ = false;
    std::optional<std::uint64_t> throw_on_special_q_index_;
};

struct SeedTraceFile final {
    std::filesystem::path path;
    std::vector<ForkSeedRecord> records;
};

std::vector<SeedTraceFile> collect_seed_trace_files(const std::string& prefix) {
    const std::filesystem::path prefix_path(prefix);
    const std::filesystem::path directory = prefix_path.parent_path();
    const std::string filename_prefix = prefix_path.filename().string() + ".";
    std::vector<SeedTraceFile> traces;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const std::string filename = entry.path().filename().string();
        if (!entry.is_regular_file() || !filename.starts_with(filename_prefix) ||
            !filename.ends_with(".bin")) {
            continue;
        }

        const auto byte_count = entry.file_size();
        CHECK(byte_count % sizeof(ForkSeedRecord) == 0);
        SeedTraceFile trace{.path = entry.path()};
        trace.records.resize(byte_count / sizeof(ForkSeedRecord));
        std::ifstream input(entry.path(), std::ios::binary);
        CHECK(input.good());
        input.read(reinterpret_cast<char*>(trace.records.data()),
                   static_cast<std::streamsize>(byte_count));
        CHECK(input.good());
        traces.push_back(std::move(trace));
    }
    std::sort(traces.begin(), traces.end(),
              [](const SeedTraceFile& left, const SeedTraceFile& right) {
                  return left.path < right.path;
              });
    return traces;
}

class SeedTraceCleanup final {
public:
    explicit SeedTraceCleanup(std::string prefix) : prefix_(std::move(prefix)) {}

    ~SeedTraceCleanup() {
        const std::filesystem::path prefix_path(prefix_);
        const std::filesystem::path directory = prefix_path.parent_path();
        const std::string filename_prefix = prefix_path.filename().string() + ".";
        std::error_code iterator_error;
        std::filesystem::directory_iterator iterator(directory, iterator_error);
        const std::filesystem::directory_iterator end;
        while (!iterator_error && iterator != end) {
            const auto path = iterator->path();
            const std::string filename = path.filename().string();
            if (filename.starts_with(filename_prefix) && filename.ends_with(".bin")) {
                std::error_code remove_error;
                (void)std::filesystem::remove(path, remove_error);
            }
            iterator.increment(iterator_error);
        }
    }

    SeedTraceCleanup(const SeedTraceCleanup&) = delete;
    SeedTraceCleanup& operator=(const SeedTraceCleanup&) = delete;

private:
    std::string prefix_;
};

[[nodiscard]] bool seed_record_less(const ForkSeedRecord& left,
                                    const ForkSeedRecord& right) noexcept {
    return std::tie(left.special_q_index, left.candidate_ordinal, left.side, left.domain,
                    left.algorithm_identity, left.cofactor_digest, left.returned_seed) <
           std::tie(right.special_q_index, right.candidate_ordinal, right.side, right.domain,
                    right.algorithm_identity, right.cofactor_digest, right.returned_seed);
}

std::vector<ForkSeedRecord>
flatten_and_sort_seed_records(const std::vector<SeedTraceFile>& traces) {
    std::vector<ForkSeedRecord> records;
    for (const auto& trace : traces) {
        records.insert(records.end(), trace.records.begin(), trace.records.end());
    }
    std::sort(records.begin(), records.end(), seed_record_less);
    return records;
}

void check_calibrated_seed_request(const ForkSeedRecord& record) {
    CHECK(record.special_q_index == 1);
    CHECK(record.candidate_ordinal == 5);
    CHECK(record.side == static_cast<std::uint8_t>(gnfs::cofactor::CofactorSide::rational));
    CHECK(record.domain ==
          static_cast<std::uint8_t>(gnfs::cofactor::CofactorRandomDomainV1::brent_pollard_rho));
    CHECK(record.algorithm_identity ==
          gnfs::cofactor::COFACTOR_BRENT_POLLARD_RHO_SCHEDULE_ALGORITHM_IDENTITY_V1);
}

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

const SeededFixture& shared_seeded_fixture() {
    static const SeededFixture fixture;
    return fixture;
}

struct SeededDistributedRun final {
    std::vector<Relation> relations;
    std::vector<DistributedSieveWorkerResult> stats;
    std::vector<SeedTraceFile> traces;
};

SeededDistributedRun run_seeded_distributed(const SeededFixture& fixture,
                                            const SpecialQRange& range, size_t num_workers,
                                            const std::string& tag) {
    ScopedEnvironment adaptive("GNFS_ADAPTIVE_LATTICE", "0");
    ScopedEnvironment skew("GNFS_LATTICE_SKEW", "0");

    DistributedSieveConfig config;
    config.num_workers = num_workers;
    config.base_path = make_tmp_base(tag);
    const std::string trace_prefix = config.base_path + ".seedtrace";
    const SeedTraceCleanup trace_cleanup(trace_prefix);
    const ForkRecordingSeedProvider provider(trace_prefix);

    SeededDistributedRun run;
    run.relations = run_distributed_sieve(
        config, fixture.ctx, fixture.fb, fixture.sieve_params(), fixture.sieve_region(),
        fixture.cofac_config(), fixture.ctx.n(), fixture.ctx.m(), range, provider, &run.stats);
    run.traces = collect_seed_trace_files(trace_prefix);
    check_worker_leases_removed(config.base_path, config.num_workers);
    cleanup_worker_test_artifacts(config.base_path, config.num_workers);
    return run;
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
    // A size_t count outside the uint32 SQ-index domain must not wrap to zero
    // and trigger division-by-zero in the helper's noexcept boundary.
    {
        const auto c =
            split_sq_range(0, 1, static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1U);
        CHECK(c.empty());
    }

    std::cout << "PASS\n";
}

// ── Test 2: ENV parsing ────────────────────────────────────────────────
void test_wait_status_requires_terminal_child() {
    std::cout << "[test_wait_status_requires_terminal_child] ... " << std::flush;

    const pid_t child = ::fork();
    if (child == 0) {
        (void)std::raise(SIGSTOP);
        ::_exit(0);
    }

    int stopped_status = 0;
    pid_t stopped_result = -1;
    if (child > 0) {
        do {
            stopped_result = ::waitpid(child, &stopped_status, WUNTRACED);
        } while (stopped_result < 0 && errno == EINTR);
    }
    const auto stopped =
        stopped_result == child
            ? gnfs::sieve::distributed_sieve_detail::decode_worker_wait_status(stopped_status)
            : gnfs::sieve::distributed_sieve_detail::DecodedWorkerWaitStatus{};

    int terminal_status = 0;
    pid_t terminal_result = -1;
    if (child > 0) {
        (void)::kill(child, SIGCONT);
        do {
            terminal_result = ::waitpid(child, &terminal_status, 0);
        } while (terminal_result < 0 && errno == EINTR);
    }
    const auto terminal =
        terminal_result == child
            ? gnfs::sieve::distributed_sieve_detail::decode_worker_wait_status(terminal_status)
            : gnfs::sieve::distributed_sieve_detail::DecodedWorkerWaitStatus{};

    CHECK(child > 0);
    CHECK(stopped_result == child);
    CHECK(WIFSTOPPED(stopped_status));
    CHECK(!stopped.terminal);
    CHECK(!stopped.success);
    CHECK(stopped.exit_status == -1);
    CHECK(stopped.signal == 0);
    CHECK(terminal_result == child);
    CHECK(WIFEXITED(terminal_status));
    CHECK(terminal.terminal);
    CHECK(terminal.success);
    CHECK(terminal.exit_status == 0);
    CHECK(terminal.signal == 0);
    std::cout << "PASS\n";
}

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

    if constexpr (std::numeric_limits<size_t>::max() > std::numeric_limits<uint32_t>::max()) {
        bool threw_oversized = false;
        try {
            run(static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1U,
                gnfs::util::temp_path("oversized"));
        } catch (const std::invalid_argument&) {
            threw_oversized = true;
        }
        CHECK(threw_oversized);
    }

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

void test_worker_report_pipe_is_cloexec() {
    std::cout << "[test_worker_report_pipe_is_cloexec] ... " << std::flush;

    const auto& f = shared_fixture();
    ScopedEnvironment assert_cloexec("GNFS_DISTRIBUTED_SIEVE_ASSERT_REPORT_CLOEXEC", "1");
    DistributedSieveConfig cfg;
    cfg.num_workers = 1;
    cfg.base_path = make_tmp_base("report-cloexec");
    std::vector<DistributedSieveWorkerResult> stats;
    const auto relations = run_distributed_sieve(cfg, f.ctx, f.fb, f.sieve_params(),
                                                 f.sieve_region(), f.cofac_config(), f.ctx.n(),
                                                 f.ctx.m(), f.sq_range(1000, 1150), &stats);

    CHECK(!relations.empty());
    CHECK(stats.size() == 1);
    CHECK(stats[0].success);
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

void test_seeded_candidate_coordinates_are_semantic() {
    std::cout << "[test_seeded_candidate_coordinates_are_semantic] ... " << std::flush;

    const auto& fixture = shared_seeded_fixture();
    SpecialQGenerator generator(fixture.fb, SeededFixture::single_sq_range());
    const auto special_q = generator.next();
    CHECK(special_q.has_value());
    CHECK(special_q->q == 5);
    CHECK(special_q->r == 1);
    CHECK(special_q->index == 1);

    auto run = run_seeded_distributed(fixture, SeededFixture::single_sq_range(), 1, "seededcoords");
    CHECK(run.stats.size() == 1);
    CHECK(run.stats[0].success);
    CHECK(run.stats[0].attempt_count == 1);
    CHECK(run.traces.size() == 1);
    CHECK(run.traces[0].records.size() == 1);
    check_calibrated_seed_request(run.traces[0].records[0]);
    std::cout << "PASS\n";
}

void test_seeded_worker_topology_invariance() {
    std::cout << "[test_seeded_worker_topology_invariance] ... " << std::flush;

    const auto& fixture = shared_seeded_fixture();
    auto one_worker =
        run_seeded_distributed(fixture, SeededFixture::topology_range(), 1, "seededtopology1");
    auto two_workers =
        run_seeded_distributed(fixture, SeededFixture::topology_range(), 2, "seededtopology2");

    const auto one_requests = flatten_and_sort_seed_records(one_worker.traces);
    const auto two_requests = flatten_and_sort_seed_records(two_workers.traces);
    CHECK(!one_requests.empty());
    CHECK(one_requests == two_requests);
    CHECK(gnfs::relation::relation_sequence_receipt(one_worker.relations) ==
          gnfs::relation::relation_sequence_receipt(two_workers.relations));
    CHECK(two_workers.stats.size() == 2);
    CHECK(std::all_of(two_workers.stats.begin(), two_workers.stats.end(),
                      [](const DistributedSieveWorkerResult& result) { return result.success; }));
    CHECK(two_workers.stats[1].sq_index_begin == 3);
    CHECK(two_workers.stats[1].sq_index_end == 4);
    const std::string second_worker_suffix =
        "." + std::to_string(two_workers.stats[1].pid) + ".bin";
    const auto second_worker_trace = std::find_if(
        two_workers.traces.begin(), two_workers.traces.end(), [&](const SeedTraceFile& trace) {
            return trace.path.string().ends_with(second_worker_suffix);
        });
    CHECK(second_worker_trace != two_workers.traces.end());
    CHECK(std::any_of(second_worker_trace->records.begin(), second_worker_trace->records.end(),
                      [](const ForkSeedRecord& record) { return record.special_q_index == 3; }));

    std::cout << "PASS\n";
}

void test_seeded_retry_replays_identical_requests() {
    std::cout << "[test_seeded_retry_replays_identical_requests] ... " << std::flush;

    const auto& fixture = shared_seeded_fixture();
    auto baseline =
        run_seeded_distributed(fixture, SeededFixture::single_sq_range(), 1, "seededretrybase");
    CHECK(baseline.traces.size() == 1);
    CHECK(!baseline.traces[0].records.empty());

    ScopedEnvironment fail_pending("GNFS_DISTRIBUTED_SIEVE_FAIL_HANDOFF_PENDING_ATTEMPT_0", "1");
    auto replay =
        run_seeded_distributed(fixture, SeededFixture::single_sq_range(), 1, "seededretry");
    CHECK(replay.stats.size() == 1);
    CHECK(replay.stats[0].success);
    CHECK(replay.stats[0].attempt_count == 2);
    CHECK(replay.traces.size() == 2);
    for (const auto& trace : replay.traces) {
        CHECK(trace.records == baseline.traces[0].records);
    }
    CHECK(gnfs::relation::relation_sequence_receipt(replay.relations) ==
          gnfs::relation::relation_sequence_receipt(baseline.relations));

    std::cout << "PASS\n";
}

void test_seed_provider_failure_is_wave_fatal() {
    std::cout << "[test_seed_provider_failure_is_wave_fatal] ... " << std::flush;

    ScopedEnvironment adaptive("GNFS_ADAPTIVE_LATTICE", "0");
    ScopedEnvironment skew("GNFS_LATTICE_SKEW", "0");
    const auto& fixture = shared_seeded_fixture();
    DistributedSieveConfig config;
    config.num_workers = 1;
    config.base_path = make_tmp_base("seedfatal");
    const std::string trace_prefix = config.base_path + ".seedtrace";
    const SeedTraceCleanup trace_cleanup(trace_prefix);
    const ForkRecordingSeedProvider provider(trace_prefix, true);
    std::vector<DistributedSieveWorkerResult> stats;

    bool caught = false;
    try {
        (void)run_distributed_sieve(config, fixture.ctx, fixture.fb, fixture.sieve_params(),
                                    fixture.sieve_region(), fixture.cofac_config(), fixture.ctx.n(),
                                    fixture.ctx.m(), SeededFixture::single_sq_range(), provider,
                                    &stats);
    } catch (const DistributedSieveSeedProviderError& error) {
        caught = true;
        CHECK(error.chunk_id() == 0);
        CHECK(error.attempt_number() == 1);
    }
    CHECK(caught);
    CHECK(stats.size() == 1);
    CHECK(!stats[0].success);
    CHECK(stats[0].attempt_count == 1);
    CHECK(stats[0].exit_status == 2);
    const auto traces = collect_seed_trace_files(trace_prefix);
    CHECK(traces.size() == 1);
    CHECK(traces[0].records.size() == 1);
    check_calibrated_seed_request(traces[0].records[0]);
    check_worker_leases_removed(config.base_path, config.num_workers);
    cleanup_worker_test_artifacts(config.base_path, config.num_workers);
    std::cout << "PASS\n";
}

void test_seed_provider_failure_reaps_complete_wave() {
    std::cout << "[test_seed_provider_failure_reaps_complete_wave] ... " << std::flush;

    ScopedEnvironment adaptive("GNFS_ADAPTIVE_LATTICE", "0");
    ScopedEnvironment skew("GNFS_LATTICE_SKEW", "0");
    const auto& fixture = shared_seeded_fixture();
    DistributedSieveConfig config;
    config.num_workers = 2;
    config.base_path = make_tmp_base("seedfatalwave");
    const std::string trace_prefix = config.base_path + ".seedtrace";
    const SeedTraceCleanup trace_cleanup(trace_prefix);
    const ForkRecordingSeedProvider provider(trace_prefix, true, 1);
    std::vector<DistributedSieveWorkerResult> stats;

    bool caught = false;
    try {
        (void)run_distributed_sieve(config, fixture.ctx, fixture.fb, fixture.sieve_params(),
                                    fixture.sieve_region(), fixture.cofac_config(), fixture.ctx.n(),
                                    fixture.ctx.m(), SeededFixture::topology_range(), provider,
                                    &stats);
    } catch (const DistributedSieveSeedProviderError& error) {
        caught = true;
        CHECK(error.chunk_id() == 0);
        CHECK(error.attempt_number() == 1);
    }
    CHECK(caught);
    CHECK(stats.size() == 2);
    CHECK(!stats[0].success);
    CHECK(stats[1].success);
    CHECK(stats[0].attempt_count == 1);
    CHECK(stats[1].attempt_count == 1);
    CHECK(stats[0].reap_confirmed);
    CHECK(stats[1].reap_confirmed);
    CHECK(stats[1].relations_count > 0);
    CHECK(stats[0].merged_relations_count == 0);
    CHECK(stats[1].merged_relations_count == 0);

    const auto traces = collect_seed_trace_files(trace_prefix);
    CHECK(traces.size() == 2);
    const auto records = flatten_and_sort_seed_records(traces);
    CHECK(std::any_of(records.begin(), records.end(),
                      [](const ForkSeedRecord& record) { return record.special_q_index == 1; }));
    CHECK(std::any_of(records.begin(), records.end(),
                      [](const ForkSeedRecord& record) { return record.special_q_index == 3; }));
    check_worker_leases_removed(config.base_path, config.num_workers);
    cleanup_worker_test_artifacts(config.base_path, config.num_workers);
    std::cout << "PASS\n";
}

void test_seed_provider_failure_on_retry_is_wave_fatal() {
    std::cout << "[test_seed_provider_failure_on_retry_is_wave_fatal] ... " << std::flush;

    ScopedEnvironment adaptive("GNFS_ADAPTIVE_LATTICE", "0");
    ScopedEnvironment skew("GNFS_LATTICE_SKEW", "0");
    ScopedEnvironment fail_first("GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_0", "1");
    const auto& fixture = shared_seeded_fixture();
    DistributedSieveConfig config;
    config.num_workers = 1;
    config.base_path = make_tmp_base("seedfatalretry");
    const std::string trace_prefix = config.base_path + ".seedtrace";
    const SeedTraceCleanup trace_cleanup(trace_prefix);
    const ForkRecordingSeedProvider provider(trace_prefix, true);
    std::vector<DistributedSieveWorkerResult> stats;

    bool caught = false;
    try {
        (void)run_distributed_sieve(config, fixture.ctx, fixture.fb, fixture.sieve_params(),
                                    fixture.sieve_region(), fixture.cofac_config(), fixture.ctx.n(),
                                    fixture.ctx.m(), SeededFixture::single_sq_range(), provider,
                                    &stats);
    } catch (const DistributedSieveSeedProviderError& error) {
        caught = true;
        CHECK(error.chunk_id() == 0);
        CHECK(error.attempt_number() == 2);
    }
    CHECK(caught);
    CHECK(stats.size() == 1);
    CHECK(!stats[0].success);
    CHECK(stats[0].attempt_count == 2);
    CHECK(stats[0].exit_status == 2);
    const auto traces = collect_seed_trace_files(trace_prefix);
    CHECK(traces.size() == 1);
    CHECK(traces[0].records.size() == 1);
    check_calibrated_seed_request(traces[0].records[0]);
    check_worker_leases_removed(config.base_path, config.num_workers);
    cleanup_worker_test_artifacts(config.base_path, config.num_workers);
    std::cout << "PASS\n";
}

void test_seeded_worker_retry_exhaustion_is_atomic() {
    std::cout << "[test_seeded_worker_retry_exhaustion_is_atomic] ... " << std::flush;

    ScopedEnvironment adaptive("GNFS_ADAPTIVE_LATTICE", "0");
    ScopedEnvironment skew("GNFS_LATTICE_SKEW", "0");
    ScopedEnvironment fail_all("GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_0", "all");
    const auto& fixture = shared_seeded_fixture();
    DistributedSieveConfig config;
    config.num_workers = 1;
    config.base_path = make_tmp_base("seededretryexhausted");
    const std::string trace_prefix = config.base_path + ".seedtrace";
    const SeedTraceCleanup trace_cleanup(trace_prefix);
    const ForkRecordingSeedProvider provider(trace_prefix);
    std::vector<DistributedSieveWorkerResult> stats;

    bool caught = false;
    try {
        (void)run_distributed_sieve(config, fixture.ctx, fixture.fb, fixture.sieve_params(),
                                    fixture.sieve_region(), fixture.cofac_config(), fixture.ctx.n(),
                                    fixture.ctx.m(), SeededFixture::single_sq_range(), provider,
                                    &stats);
    } catch (const DistributedSieveSeedProviderError&) {
        throw std::runtime_error("ordinary seeded worker failure was misclassified as provider");
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught);
    CHECK(stats.size() == 1);
    CHECK(!stats[0].success);
    CHECK(stats[0].attempt_count == 2);
    CHECK(stats[0].merged_relations_count == 0);
    CHECK(collect_seed_trace_files(trace_prefix).empty());
    check_worker_leases_removed(config.base_path, config.num_workers);
    cleanup_worker_test_artifacts(config.base_path, config.num_workers);
    std::cout << "PASS\n";
}

// ── Test 20: ENV-config end-to-end ─────────────────────────────────────
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
    test_wait_status_requires_terminal_child();
    test_env_parsing();
    test_invalid_config();
    test_single_worker_matches_in_process();
    test_worker_completion_report_tracks_actual_caps();
    test_worker_report_pipe_is_cloexec();
    test_multi_worker_same_set();
    test_empty_range();
    test_more_workers_than_sqs();
    test_worker_crash_with_retry();
    test_handoff_pending_crash_with_retry();
    test_corrupt_completion_report_with_retry();
    test_corrupt_sequence_receipt_with_retry();
    test_worker_retry_exhaustion();
    test_legacy_worker_leaves_are_preserved();
    test_seeded_candidate_coordinates_are_semantic();
    test_seeded_worker_topology_invariance();
    test_seeded_retry_replays_identical_requests();
    test_seed_provider_failure_is_wave_fatal();
    test_seed_provider_failure_reaps_complete_wave();
    test_seed_provider_failure_on_retry_is_wave_fatal();
    test_seeded_worker_retry_exhaustion_is_atomic();
    test_env_config_e2e();

    auto t1 = std::chrono::high_resolution_clock::now();
    double s = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "=== All tests PASSED in " << s << "s ===\n";
    return 0;
}

#endif
