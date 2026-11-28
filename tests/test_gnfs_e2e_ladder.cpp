// test_gnfs_e2e_ladder.cpp — Digit-banded evidence ladder for complete GNFS
// factorizations
//
// One process factors one semiprime with the General Number Field Sieve (GNFS)
// forced through Pipeline::run(), from polynomial selection to the square root,
// and prints exactly one machine-readable line on stdout:
//
//   GNFS_E2E_LADDER {...JSON...}
//
// Usage:
//   test_gnfs_e2e_ladder --digits <30|35|40|50> --route <default|structured>
//                        --expect <p> <q>
//
// The automatic route selects SIQS from 25 through 100 digits, and a forced
// GNFS run at or below 100 digits still probes SIQS for 30 seconds unless
// GNFS_DISABLE_SIQS=1. The ladder sets that variable, so a passing run can only
// report factors that the GNFS pipeline produced.
//
// Fixtures. Each N is a semiprime. The 30-, 40-, and 50-digit inputs pair
// primes of equal length; the 35-digit input pairs an 18-digit prime with a
// 17-digit prime. The expected primes arrive on the command line, and the test
// rejects a pair that does not multiply back to the fixture.
//
// Process variables. The default route leaves GNFS_STRUCTURED_FILTER and
// GNFS_OOC_RELATIONS unset. The structured route sets both to 1, because the
// structured filter requires an explicitly enabled out-of-core collector. All
// remaining variables copy the isolation set of
// tests/test_structured_ooc_50d_probe.cpp, which keeps resume, distributed
// workers, the V3 cascade, V0 BFS, 3LP, the adaptive lattice, and the lattice
// skew off. Keep the two lists in step so both binaries measure the same
// production path.
//
// Stage wall times come from the progress callback. Each interval is charged
// to the stage that the latest progress event announced, so alternating sieve
// and filter rounds stay separated. Phase::LinearAlgebra covers both the
// matrix build and the dependency solver; the "SGE preprocessing" event marks
// the boundary between them.

#include "support/scoped_environment_stderr.hpp"
#include "support/test_check.hpp"

#include <gnfs/api/json.hpp>
#include <gnfs/api/pipeline.hpp>
#include <gnfs/core/integer.hpp>
#include <gnfs/util/process_memory.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using gnfs::api::Config;
using gnfs::api::FactorizationMethod;
using gnfs::api::FactorResult;
using gnfs::api::FactorStats;
using gnfs::api::LogEntry;
using gnfs::api::Phase;
using gnfs::api::Pipeline;
using gnfs::api::ProgressInfo;
using gnfs::core::Integer;
using gnfs::tests::support::ScopedEnvironmentVariable;

namespace {

constexpr std::string_view kRecordPrefix = "GNFS_E2E_LADDER ";
constexpr std::string_view kStructuredRecordPrefix = "structured_filter ";
constexpr std::string_view kStructuredMatrixRecordPrefix = "structured_filter_matrix ";
// Phase::LinearAlgebra starts with the matrix build. This progress event is
// the first one that the dependency solver announces.
constexpr std::string_view kSolverStartMessage = "SGE preprocessing";

struct LadderFixture final {
    uint32_t digits;
    const char* n;
};

constexpr LadderFixture kFixtures[] = {
    {30, "191890662872264280268190961847"},
    {35, "39014589280961698834918279677183371"},
    {40, "6366991587432775009956293539027674167473"},
    {50, "47490818589724128449908212011909439728066961128833"},
};

enum class LadderRoute : uint8_t {
    Default,
    Structured,
};

[[nodiscard]] constexpr std::string_view route_name(LadderRoute route) noexcept {
    switch (route) {
    case LadderRoute::Default:
        return "default";
    case LadderRoute::Structured:
        return "structured";
    }
    return "unknown";
}

// Pipeline stages in execution order. `Other` collects the non-GNFS prechecks
// and every interval outside a named stage.
enum class Stage : uint8_t {
    PolynomialSelection,
    FactorBase,
    SieveCollect,
    FilterReduction,
    Matrix,
    LinearAlgebra,
    SquareRoot,
    Other,
};
constexpr size_t kStageCount = static_cast<size_t>(Stage::Other) + 1;

[[nodiscard]] constexpr std::string_view stage_name(Stage stage) noexcept {
    switch (stage) {
    case Stage::PolynomialSelection:
        return "polynomial_selection";
    case Stage::FactorBase:
        return "factor_base";
    case Stage::SieveCollect:
        return "sieve_collect";
    case Stage::FilterReduction:
        return "filter_reduction";
    case Stage::Matrix:
        return "matrix";
    case Stage::LinearAlgebra:
        return "linear_algebra";
    case Stage::SquareRoot:
        return "square_root";
    case Stage::Other:
        return "other";
    }
    return "unknown";
}

struct StageTiming final {
    // Seconds between the start of Pipeline::run() and the first progress
    // event of this stage. Disengaged when the stage never started.
    std::optional<double> start_seconds;
    double wall_seconds = 0.0;
};

using Clock = std::chrono::steady_clock;

[[nodiscard]] double elapsed_seconds(Clock::time_point start, Clock::time_point finish) noexcept {
    return std::chrono::duration<double>(finish - start).count();
}

// Attributes wall time to pipeline stages from the progress callback.
class StageRecorder final {
public:
    void start() {
        const std::lock_guard<std::mutex> lock(mutex_);
        run_start_ = Clock::now();
        stage_since_ = run_start_;
        current_ = Stage::Other;
        stages_.fill(StageTiming{});
        stages_[stage_index(Stage::Other)].start_seconds = 0.0;
        running_ = true;
    }

    void observe(const ProgressInfo& info) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        const Clock::time_point now = Clock::now();
        stages_[stage_index(current_)].wall_seconds += elapsed_seconds(stage_since_, now);
        stage_since_ = now;
        const Stage next = classify(info);
        if (!stages_[stage_index(next)].start_seconds.has_value()) {
            stages_[stage_index(next)].start_seconds = elapsed_seconds(run_start_, now);
        }
        current_ = next;
    }

    void finish() {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        const Clock::time_point now = Clock::now();
        stages_[stage_index(current_)].wall_seconds += elapsed_seconds(stage_since_, now);
        wall_total_seconds_ = elapsed_seconds(run_start_, now);
        running_ = false;
    }

    [[nodiscard]] double wall_total_seconds() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return wall_total_seconds_;
    }

    [[nodiscard]] std::array<StageTiming, kStageCount> stages() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return stages_;
    }

private:
    [[nodiscard]] static constexpr size_t stage_index(Stage stage) noexcept {
        return static_cast<size_t>(stage);
    }

    // The caller holds the mutex and marks the returned stage as started.
    [[nodiscard]] Stage classify(const ProgressInfo& info) const {
        switch (info.phase) {
        case Phase::PolynomialSelection:
            return Stage::PolynomialSelection;
        case Phase::FactorBase:
            return Stage::FactorBase;
        case Phase::Sieving:
            return Stage::SieveCollect;
        case Phase::Filtering:
            return Stage::FilterReduction;
        case Phase::LinearAlgebra:
            if (stages_[stage_index(Stage::LinearAlgebra)].start_seconds.has_value() ||
                info.message.starts_with(kSolverStartMessage)) {
                return Stage::LinearAlgebra;
            }
            return Stage::Matrix;
        case Phase::SquareRoot:
            return Stage::SquareRoot;
        case Phase::FactorExtraction:
            // Before the square root this phase reports the primality, trial
            // division, and rho prechecks. After it, the XOR-pair fallback
            // still spends square-root work.
            return stages_[stage_index(Stage::SquareRoot)].start_seconds.has_value()
                       ? Stage::SquareRoot
                       : Stage::Other;
        case Phase::Done:
            return Stage::Other;
        }
        return Stage::Other;
    }

    mutable std::mutex mutex_;
    std::array<StageTiming, kStageCount> stages_{};
    Clock::time_point run_start_{};
    Clock::time_point stage_since_{};
    Stage current_ = Stage::Other;
    double wall_total_seconds_ = 0.0;
    bool running_ = false;
};

// Stops the recorder on every exit path, including a pipeline exception.
class RecorderScope final {
public:
    explicit RecorderScope(StageRecorder& recorder) : recorder_(recorder) {
        recorder_.start();
    }
    ~RecorderScope() {
        recorder_.finish();
    }
    RecorderScope(const RecorderScope&) = delete;
    RecorderScope& operator=(const RecorderScope&) = delete;

private:
    StageRecorder& recorder_;
};

// Counts the structured-route records that the log callback observes.
class RouteEvidence final {
public:
    void observe(const LogEntry& entry) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (entry.message.starts_with(kStructuredMatrixRecordPrefix)) {
            ++structured_matrix_records_;
        } else if (entry.message.starts_with(kStructuredRecordPrefix)) {
            ++structured_filter_records_;
        }
    }

    [[nodiscard]] size_t structured_filter_records() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return structured_filter_records_;
    }

    [[nodiscard]] size_t structured_matrix_records() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return structured_matrix_records_;
    }

private:
    mutable std::mutex mutex_;
    size_t structured_filter_records_ = 0;
    size_t structured_matrix_records_ = 0;
};

// The published statistics that the record carries. A disengaged field means
// that Pipeline::run() threw before it published statistics.
struct LadderStats final {
    std::optional<std::string> executed_algorithm;
    std::optional<std::vector<std::string>> algorithms_executed;
    std::optional<size_t> relations;
    std::optional<size_t> relations_after_filter;
    std::optional<size_t> sieve_rounds_completed;
    std::optional<std::string> sieve_stop_reason;
    std::optional<size_t> matrix_rows;
    std::optional<size_t> matrix_cols;
    std::optional<size_t> matrix_nonzeros;
    std::optional<int64_t> matrix_excess;
    std::optional<size_t> dependencies_found;
    std::optional<size_t> dependencies_tried;
};

struct LadderRecord final {
    std::string status = "fail";
    std::string error;
    size_t digits = 0;
    size_t n_bits = 0;
    LadderRoute route = LadderRoute::Default;
    std::string n;
    std::vector<std::string> factors;
    LadderStats stats;
    size_t structured_filter_records = 0;
    size_t structured_matrix_records = 0;
    double wall_total_seconds = 0.0;
    std::array<StageTiming, kStageCount> stages{};
    std::optional<uint64_t> peak_rss_bytes;
};

struct EnvironmentBinding final {
    const char* name;
    const char* value; // nullptr removes the variable for the run
};

using EnvironmentGuards = std::vector<std::unique_ptr<ScopedEnvironmentVariable>>;

[[nodiscard]] std::vector<EnvironmentBinding> ladder_environment(LadderRoute route) {
    const bool structured = route == LadderRoute::Structured;
    return {
        {"GNFS_DISABLE_SIQS", "1"},
        {"GNFS_STRUCTURED_FILTER", structured ? "1" : nullptr},
        {"GNFS_OOC_RELATIONS", structured ? "1" : nullptr},
        {"GNFS_OOC_BASE_PATH", nullptr},
        {"GNFS_RESUME", nullptr},
        {"GNFS_SIEVE_RESUME", nullptr},
        {"GNFS_DISTRIBUTED_SIEVE_WORKERS", "0"},
        {"GNFS_DISTRIBUTED_SIEVE_FORCE_SMALL", "0"},
        {"GNFS_ADAPTIVE_LATTICE", "0"},
        {"GNFS_ADAPTIVE_LATTICE_THRESHOLD", nullptr},
        {"GNFS_ADAPTIVE_LATTICE_MAX_RETRIES", nullptr},
        {"GNFS_ADAPTIVE_LATTICE_SEED", nullptr},
        {"GNFS_3LP", "0"},
        {"GNFS_CASCADE_V3", "0"},
        {"GNFS_V0_BFS", "0"},
        {"GNFS_OVERRIDE_LP_BITS", nullptr},
        {"GNFS_SIEVE_TARGET_MULT", nullptr},
        {"GNFS_LATTICE_LLL", nullptr},
        {"GNFS_LATTICE_SKEW", "0"},
        {"GNFS_SGE_STREAMING", "off"},
        {"GNFS_COFACTOR_BRENT", "0"},
        {"GNFS_ECM_BRENT_SUYAMA", "0"},
        {"GNFS_ECM_BS_DEGREE", nullptr},
        {"GNFS_ECM_STAGE1_PARALLEL_THREADS", "1"},
        {"GNFS_ECM_STAGE2_PARALLEL", "1"},
        {"GNFS_BRENT_POLLARD_RHO_THREADS", "1"},
        {"GNFS_ECM_CURVE_POOL", "0"},
        {"GNFS_COFACTOR_BATCH_SIZE", "1"},
        {"GNFS_ECM_SIGMA_POOL_SIZE", "0"},
        {"GNFS_ECM_B1_CACHE_SIZE", "0"},
        {"GNFS_COFACTOR_RESULT_CACHE_SIZE", "0"},
        {"GNFS_ECM_BATCH_INV", "0"},
        {"GNFS_SURVIVAL_FILTER", "0"},
        {"GNFS_SURVIVAL_THRESHOLD", nullptr},
        {"GNFS_COFACTOR_TIMING_ENABLE", "0"},
        {"GNFS_TRIAL_DIV_SIMD", "auto"},
    };
}

// Applies the ladder process variables and returns the guards that restore the
// previous values.
[[nodiscard]] EnvironmentGuards freeze_ladder_environment(LadderRoute route) {
    EnvironmentGuards guards;
    for (const EnvironmentBinding& binding : ladder_environment(route)) {
        guards.push_back(std::make_unique<ScopedEnvironmentVariable>(binding.name, binding.value));
    }
    return guards;
}

struct CliOptions final {
    const LadderFixture* fixture = nullptr;
    LadderRoute route = LadderRoute::Default;
    Integer expected_p;
    Integer expected_q;
    bool help = false;
};

[[nodiscard]] const LadderFixture* find_fixture(uint32_t digits) noexcept {
    for (const LadderFixture& fixture : kFixtures) {
        if (fixture.digits == digits) {
            return &fixture;
        }
    }
    return nullptr;
}

[[nodiscard]] bool is_decimal(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    return std::all_of(text.begin(), text.end(),
                       [](char character) { return character >= '0' && character <= '9'; });
}

// A factor is nontrivial when it differs from both 1 and N.
[[nodiscard]] bool is_nontrivial(const Integer& factor, const Integer& n) {
    return factor.compare(Integer(1)) > 0 && factor.compare(n) < 0;
}

[[nodiscard]] uint32_t parse_digits(std::string_view text) {
    uint32_t parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [position, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || position != end || parsed == 0) {
        throw std::invalid_argument("--digits must be a positive decimal integer");
    }
    return parsed;
}

[[nodiscard]] LadderRoute parse_route(std::string_view text) {
    if (text == "default") {
        return LadderRoute::Default;
    }
    if (text == "structured") {
        return LadderRoute::Structured;
    }
    throw std::invalid_argument("--route must be default or structured");
}

[[nodiscard]] CliOptions parse_cli(int argc, char** argv) {
    CliOptions options;
    bool digits_seen = false;
    bool route_seen = false;
    bool expect_seen = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            options.help = true;
            continue;
        }
        if (argument == "--digits") {
            if (digits_seen) {
                throw std::invalid_argument("--digits may be specified only once");
            }
            if (index + 1 >= argc) {
                throw std::invalid_argument("--digits requires a value");
            }
            options.fixture = find_fixture(parse_digits(argv[index + 1]));
            if (options.fixture == nullptr) {
                throw std::invalid_argument("--digits must be 30, 35, 40, or 50");
            }
            digits_seen = true;
            ++index;
            continue;
        }
        if (argument == "--route") {
            if (route_seen) {
                throw std::invalid_argument("--route may be specified only once");
            }
            if (index + 1 >= argc) {
                throw std::invalid_argument("--route requires a value");
            }
            options.route = parse_route(argv[index + 1]);
            route_seen = true;
            ++index;
            continue;
        }
        if (argument == "--expect") {
            if (expect_seen) {
                throw std::invalid_argument("--expect may be specified only once");
            }
            if (index + 2 >= argc) {
                throw std::invalid_argument("--expect requires two values: <p> <q>");
            }
            const std::string_view p_text(argv[index + 1]);
            const std::string_view q_text(argv[index + 2]);
            if (!is_decimal(p_text) || !is_decimal(q_text)) {
                throw std::invalid_argument("--expect requires two positive decimal integers");
            }
            options.expected_p = Integer(std::string(p_text));
            options.expected_q = Integer(std::string(q_text));
            expect_seen = true;
            index += 2;
            continue;
        }
        throw std::invalid_argument("unknown command-line argument");
    }
    if (options.help) {
        return options;
    }
    if (!digits_seen) {
        throw std::invalid_argument("--digits is required");
    }
    if (!route_seen) {
        throw std::invalid_argument("--route is required");
    }
    if (!expect_seen) {
        throw std::invalid_argument("--expect is required");
    }
    return options;
}

void print_usage(std::ostream& os) {
    os << "Usage: test_gnfs_e2e_ladder --digits <30|35|40|50> "
          "--route <default|structured> --expect <p> <q>\n"
       << "Factors one ladder semiprime with GNFS forced and prints one "
          "GNFS_E2E_LADDER JSON record on stdout.\n";
}

// Guards the fixture and the command line before the long run.
void verify_fixture(const CliOptions& options) {
    const Integer n(options.fixture->n);
    GNFS_TEST_CHECK(n.num_digits() == options.fixture->digits);
    GNFS_TEST_CHECK(is_nontrivial(options.expected_p, n));
    GNFS_TEST_CHECK(is_nontrivial(options.expected_q, n));
    Integer product = options.expected_p;
    product *= options.expected_q;
    GNFS_TEST_CHECK(product == n);
}

// Runs the forced-GNFS factorization and fills the measurement part of the
// record. A pipeline exception still leaves the stage timings in the record.
FactorResult run_ladder(const CliOptions& options, LadderRecord& record) {
    const Integer n(options.fixture->n);
    const EnvironmentGuards environment = freeze_ladder_environment(options.route);

    Config config;
    config.set_method(FactorizationMethod::GNFS).set_verbose(false);

    // Declared before the Pipeline so that the callbacks never outlive their
    // state.
    StageRecorder recorder;
    RouteEvidence evidence;
    Pipeline pipeline(n, config);
    pipeline.set_progress_callback(
        [&recorder](const ProgressInfo& info) { recorder.observe(info); });
    pipeline.set_log_callback([&evidence](const LogEntry& entry) { evidence.observe(entry); });

    std::optional<FactorResult> result;
    {
        const RecorderScope scope(recorder);
        result.emplace(pipeline.run());
    }

    record.wall_total_seconds = recorder.wall_total_seconds();
    record.stages = recorder.stages();
    record.structured_filter_records = evidence.structured_filter_records();
    record.structured_matrix_records = evidence.structured_matrix_records();
    record.peak_rss_bytes = gnfs::util::process_memory_snapshot().lifetime_peak_rss_bytes;

    const FactorStats& published = result->stats;
    LadderStats& stats = record.stats;
    stats.executed_algorithm = published.executed_algorithm;
    stats.algorithms_executed = published.algorithms_executed;
    stats.relations = published.relations_found;
    stats.relations_after_filter = published.relations_after_filter;
    stats.sieve_rounds_completed = published.sieve_rounds_completed;
    stats.sieve_stop_reason =
        std::string(gnfs::api::sieve_stop_reason_name(published.sieve_stop_reason));
    stats.matrix_rows = published.matrix_rows;
    stats.matrix_cols = published.matrix_cols;
    stats.matrix_nonzeros = published.matrix_weight;
    stats.matrix_excess = published.matrix_excess;
    stats.dependencies_found = published.dependencies_found;
    stats.dependencies_tried = published.dependencies_tried;

    for (const Integer& factor : result->factors) {
        record.factors.push_back(factor.to_string());
    }
    return std::move(*result);
}

void verify_result(const CliOptions& options, const FactorResult& result,
                   const LadderRecord& record) {
    const Integer n(options.fixture->n);
    const FactorStats& stats = result.stats;

    // (a) The GNFS pipeline produced the factors. Trial division and Pollard
    // rho may run as probes, but SIQS must not start at all.
    GNFS_TEST_CHECK(result.n == n);
    GNFS_TEST_CHECK(stats.gnfs_parameters_applied);
    GNFS_TEST_CHECK(stats.method_used == FactorizationMethod::GNFS);
    GNFS_TEST_CHECK(stats.executed_algorithm == "gnfs");
    GNFS_TEST_CHECK(std::find(stats.algorithms_executed.begin(), stats.algorithms_executed.end(),
                              "siqs") == stats.algorithms_executed.end());
    GNFS_TEST_CHECK(stats.matrix_rows > 0);
    GNFS_TEST_CHECK(stats.matrix_cols > 0);
    GNFS_TEST_CHECK(stats.dependencies_found > 0);
    GNFS_TEST_CHECK(stats.dependencies_tried > 0);

    // (b) Two nontrivial factors whose product is N.
    GNFS_TEST_CHECK(result.success);
    GNFS_TEST_CHECK(result.factors.size() == 2);
    const Integer& factor1 = result.factors[0];
    const Integer& factor2 = result.factors[1];
    GNFS_TEST_CHECK(is_nontrivial(factor1, n));
    GNFS_TEST_CHECK(is_nontrivial(factor2, n));
    Integer product = factor1;
    product *= factor2;
    GNFS_TEST_CHECK(product == n);

    // (c) The factors are the two expected primes, in either order.
    const bool ordered = factor1 == options.expected_p && factor2 == options.expected_q;
    const bool reversed = factor1 == options.expected_q && factor2 == options.expected_p;
    GNFS_TEST_CHECK(ordered || reversed);

    // (d) The reduction route that the record names is the one that ran.
    if (options.route == LadderRoute::Structured) {
        GNFS_TEST_CHECK(record.structured_filter_records > 0);
        GNFS_TEST_CHECK(record.structured_matrix_records > 0);
    } else {
        GNFS_TEST_CHECK(record.structured_filter_records == 0);
        GNFS_TEST_CHECK(record.structured_matrix_records == 0);
    }
}

[[nodiscard]] std::string seconds_text(double value) {
    if (!std::isfinite(value)) {
        return "null";
    }
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::fixed << std::setprecision(3) << value;
    return os.str();
}

void append_stage(std::ostream& os, std::string_view key, const StageTiming& timing) {
    os << gnfs::api::json::quote(key) << ":{\"start_seconds\":";
    if (timing.start_seconds.has_value()) {
        os << seconds_text(*timing.start_seconds);
    } else {
        os << "null";
    }
    os << ",\"wall_seconds\":" << seconds_text(timing.wall_seconds) << '}';
}

// Appends a JSON value, or null when the pipeline published no statistics.
void append_count(std::ostream& os, std::optional<size_t> value) {
    os << (value.has_value() ? std::to_string(*value) : std::string("null"));
}

void append_signed(std::ostream& os, std::optional<int64_t> value) {
    os << (value.has_value() ? std::to_string(*value) : std::string("null"));
}

void append_text(std::ostream& os, const std::optional<std::string>& value) {
    if (value.has_value()) {
        os << gnfs::api::json::quote(*value);
    } else {
        os << "null";
    }
}

void append_text_array(std::ostream& os, const std::vector<std::string>& values) {
    os << '[';
    for (size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            os << ',';
        }
        os << gnfs::api::json::quote(values[index]);
    }
    os << ']';
}

// Writes the record as one line: the prefix and one JSON object. Statistics
// that the pipeline never published appear as null.
void emit_record(std::ostream& os, const LadderRecord& record) {
    using gnfs::api::json::quote;
    const LadderStats& stats = record.stats;

    os << kRecordPrefix << "{\"schema\":1,\"status\":" << quote(record.status)
       << ",\"error\":" << (record.error.empty() ? std::string("null") : quote(record.error))
       << ",\"digits\":" << record.digits << ",\"n_bits\":" << record.n_bits
       << ",\"route\":" << quote(route_name(record.route)) << ",\"n\":" << quote(record.n)
       << ",\"factors\":";
    append_text_array(os, record.factors);
    os << ",\"executed_algorithm\":";
    append_text(os, stats.executed_algorithm);
    os << ",\"algorithms_executed\":";
    if (stats.algorithms_executed.has_value()) {
        append_text_array(os, *stats.algorithms_executed);
    } else {
        os << "null";
    }
    os << ",\"wall_total_seconds\":" << seconds_text(record.wall_total_seconds);
    os << ",\"stages\":{";
    for (size_t index = 0; index < record.stages.size(); ++index) {
        if (index > 0) {
            os << ',';
        }
        append_stage(os, stage_name(static_cast<Stage>(index)), record.stages[index]);
    }
    os << "},\"structured_filter_records\":" << record.structured_filter_records
       << ",\"structured_matrix_records\":" << record.structured_matrix_records;
    os << ",\"relations\":";
    append_count(os, stats.relations);
    os << ",\"relations_after_filter\":";
    append_count(os, stats.relations_after_filter);
    os << ",\"sieve_rounds_completed\":";
    append_count(os, stats.sieve_rounds_completed);
    os << ",\"sieve_stop_reason\":";
    append_text(os, stats.sieve_stop_reason);
    os << ",\"matrix_rows\":";
    append_count(os, stats.matrix_rows);
    os << ",\"matrix_cols\":";
    append_count(os, stats.matrix_cols);
    os << ",\"matrix_nonzeros\":";
    append_count(os, stats.matrix_nonzeros);
    os << ",\"matrix_excess\":";
    append_signed(os, stats.matrix_excess);
    os << ",\"dependencies_found\":";
    append_count(os, stats.dependencies_found);
    os << ",\"dependencies_tried\":";
    append_count(os, stats.dependencies_tried);
    os << ",\"peak_rss_bytes\":"
       << (record.peak_rss_bytes.has_value() ? std::to_string(*record.peak_rss_bytes)
                                             : std::string("null"));
    os << '}';
}

} // namespace

int main(int argc, char** argv) {
    LadderRecord record;
    int exit_code = 1;
    try {
        const CliOptions options = parse_cli(argc, argv);
        if (options.help) {
            print_usage(std::cout);
            return 0;
        }
        record.digits = options.fixture->digits;
        record.route = options.route;
        record.n = options.fixture->n;
        record.n_bits = Integer(record.n).bit_length();
        try {
            verify_fixture(options);
            const FactorResult result = run_ladder(options, record);
            verify_result(options, result, record);
            record.status = "pass";
            exit_code = 0;
        } catch (const std::exception& error) {
            record.error = error.what();
            std::cerr << "test_gnfs_e2e_ladder: " << error.what() << '\n';
        } catch (...) {
            record.error = "unknown exception";
            std::cerr << "test_gnfs_e2e_ladder: unknown exception\n";
        }
    } catch (const std::exception& usage_error) {
        std::cerr << "test_gnfs_e2e_ladder: " << usage_error.what() << '\n';
        print_usage(std::cerr);
        return 2;
    }
    emit_record(std::cout, record);
    std::cout << '\n';
    std::cout.flush();
    return exit_code;
}
