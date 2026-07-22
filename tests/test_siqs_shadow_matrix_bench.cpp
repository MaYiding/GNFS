/// test_siqs_shadow_matrix_bench.cpp - manual SIQS shadow-matrix microbenchmarks.
///
/// CMake registers this executable as a disabled CTest entry for discovery, but
/// it is a manual-only runner. Build it with optimization and NDEBUG, then
/// select one benchmark scope explicitly. Timing is diagnostic only: no
/// wall-clock value is a pass/fail condition.

#include <gnfs/siqs/shadow_matrix.hpp>
#include <gnfs/util/thread_pool.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef GNFS_SIQS_SHADOW_BENCH_BUILD_TYPE
#define GNFS_SIQS_SHADOW_BENCH_BUILD_TYPE "unconfigured"
#endif

namespace {

using gnfs::core::Integer;
using gnfs::siqs::check_siqs_post_merge_row_identity;
using gnfs::siqs::checked_siqs_shadow_dense_matrix_bytes;
using gnfs::siqs::SIQSFactorPower;
using gnfs::siqs::SIQSPostMergeRow;
using gnfs::siqs::SIQSPostMergeRowStatus;
using gnfs::siqs::SIQSShadowMatrixOptions;
using gnfs::siqs::SIQSShadowMatrixResult;
using gnfs::siqs::SIQSShadowMatrixStatus;
using gnfs::siqs::SIQSShadowRow;
using gnfs::siqs::SIQSShadowRowOrigin;
using gnfs::siqs::SIQSSourceId;
using gnfs::siqs::solve_siqs_shadow_matrix;
using Clock = std::chrono::steady_clock;

constexpr uint64_t BENCHMARK_SEED = UINT64_C(0x53a9f19d97e8c641);
constexpr std::string_view CMAKE_BUILD_TYPE = GNFS_SIQS_SHADOW_BENCH_BUILD_TYPE;
constexpr size_t DEFAULT_FACTOR_BASE_SIZE = 1'601;
constexpr size_t DEFAULT_ROW_COUNT = 1'701;
constexpr size_t DEFAULT_ODD_WEIGHT = 20;
constexpr size_t DEFAULT_WARMUPS = 1;
constexpr size_t DEFAULT_REPETITIONS = 3;
constexpr size_t DEFAULT_PARALLEL_THRESHOLD = 0;
constexpr size_t DEFAULT_FBCHECK_INNER_ITERATIONS = 1'000;
constexpr size_t MAX_FACTOR_BASE_SIZE = 1'000'001;
constexpr size_t MAX_ROW_COUNT = 2'000'000;
constexpr size_t MAX_ODD_WEIGHT = 4'096;
constexpr size_t MAX_WARMUPS = 20;
constexpr size_t MAX_REPETITIONS = 50;
constexpr size_t MAX_WORKERS = 256;
constexpr size_t MAX_FBCHECK_INNER_ITERATIONS = 100'000'000;
constexpr uint64_t MAX_FORCED_DENSE_BYTES = UINT64_C(8) * 1024 * 1024 * 1024;

// Keeps repeated fbcheck calls observable under -O3. The benchmark is
// single-threaded in this mode, so volatile is sufficient and avoids timing an
// atomic read-modify-write alongside every factor-base scan.
volatile uint64_t fbcheck_call_barrier = 0;

#if defined(NDEBUG)
constexpr bool NDEBUG_ENABLED = true;
constexpr std::string_view BUILD_CONTRACT = "release_ndebug";
#else
constexpr bool NDEBUG_ENABLED = false;
constexpr std::string_view BUILD_CONTRACT = "debug_assertions_enabled";
#endif

enum class BenchmarkMode : uint8_t {
    solve,
    kernel,
    prepare,
    fbcheck,
};

struct CliOptions final {
    BenchmarkMode mode = BenchmarkMode::solve;
    size_t factor_base_size = DEFAULT_FACTOR_BASE_SIZE;
    size_t row_count = DEFAULT_ROW_COUNT;
    size_t odd_weight = DEFAULT_ODD_WEIGHT;
    size_t warmups = DEFAULT_WARMUPS;
    size_t repetitions = DEFAULT_REPETITIONS;
    size_t parallel_threshold = DEFAULT_PARALLEL_THRESHOLD;
    size_t fbcheck_inner_iterations = DEFAULT_FBCHECK_INNER_ITERATIONS;
    std::vector<uint32_t> workers{1, 2, 4};
    bool row_count_was_set = false;
    bool force_dense = false;
    bool help = false;
};

struct TimingSummary final {
    uint64_t minimum_ns = 0;
    uint64_t median_ns = 0;
    uint64_t maximum_ns = 0;
    uint64_t result_digest = 0;
};

struct PackedByteProjection final {
    std::optional<size_t> single_dense_bytes;
    std::optional<size_t> peak_packed_bytes;
};

struct KernelView final {
    std::span<const uint64_t> matrix;
    std::span<const size_t> pivots;
};

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

void require(bool condition, std::string message) {
    if (!condition) {
        fail(std::move(message));
    }
}

[[nodiscard]] std::string_view mode_name(BenchmarkMode mode) noexcept {
    switch (mode) {
    case BenchmarkMode::solve:
        return "solve";
    case BenchmarkMode::kernel:
        return "kernel";
    case BenchmarkMode::prepare:
        return "prepare";
    case BenchmarkMode::fbcheck:
        return "fbcheck";
    }
    return "unknown";
}

[[nodiscard]] uint64_t splitmix64(uint64_t& state) noexcept {
    state += UINT64_C(0x9e3779b97f4a7c15);
    uint64_t value = state;
    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

void digest_byte(uint64_t& digest, uint8_t value) noexcept {
    digest ^= static_cast<uint64_t>(value);
    digest *= UINT64_C(1099511628211);
}

void digest_u64(uint64_t& digest, uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        digest_byte(digest, static_cast<uint8_t>(value >> shift));
    }
}

[[nodiscard]] std::string hexadecimal(uint64_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

[[nodiscard]] size_t parse_size(std::string_view text, std::string_view option) {
    uint64_t parsed = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto [position, error] = std::from_chars(begin, end, parsed);
    if (text.empty() || error != std::errc{} || position != end ||
        parsed > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        throw std::invalid_argument(std::string(option) + " requires a non-negative integer");
    }
    return static_cast<size_t>(parsed);
}

[[nodiscard]] std::vector<uint32_t> parse_workers(std::string_view text) {
    if (text == "all") {
        return {1, 2, 4};
    }
    std::vector<uint32_t> workers;
    while (!text.empty()) {
        const size_t separator = text.find(',');
        const std::string_view item = text.substr(0, separator);
        if (item.size() > size_t{1} && item.front() == '0') {
            throw std::invalid_argument(
                "--workers entries must use canonical decimal without leading zeros");
        }
        const size_t parsed = parse_size(item, "--workers");
        if (parsed == 0 || parsed > MAX_WORKERS) {
            throw std::invalid_argument("--workers entries must be integers in [1,256]");
        }
        const auto worker = static_cast<uint32_t>(parsed);
        if (std::find(workers.begin(), workers.end(), worker) != workers.end()) {
            throw std::invalid_argument("--workers must not contain duplicates");
        }
        workers.push_back(worker);
        if (separator == std::string_view::npos) {
            break;
        }
        text.remove_prefix(separator + 1);
        if (text.empty()) {
            throw std::invalid_argument("--workers contains an empty entry");
        }
    }
    if (workers.empty()) {
        throw std::invalid_argument("--workers requires at least one worker count");
    }
    return workers;
}

[[nodiscard]] std::string_view next_value(int argc, char** argv, int& index,
                                          std::string_view option) {
    if (++index >= argc) {
        throw std::invalid_argument(std::string(option) + " requires a value");
    }
    return argv[index];
}

[[nodiscard]] BenchmarkMode parse_mode(std::string_view text) {
    if (text == "solve") {
        return BenchmarkMode::solve;
    }
    if (text == "kernel") {
        return BenchmarkMode::kernel;
    }
    if (text == "prepare") {
        return BenchmarkMode::prepare;
    }
    if (text == "fbcheck") {
        return BenchmarkMode::fbcheck;
    }
    throw std::invalid_argument("mode must be solve, kernel, prepare, or fbcheck");
}

[[nodiscard]] CliOptions parse_cli(int argc, char** argv) {
    CliOptions options;
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        options.help = true;
        return options;
    }
    if (argc < 2) {
        throw std::invalid_argument("a benchmark mode is required; use --help for usage");
    }
    options.mode = parse_mode(argv[1]);
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            options.help = true;
        } else if (argument == "--force-dense") {
            options.force_dense = true;
        } else if (argument == "--fb") {
            options.factor_base_size =
                parse_size(next_value(argc, argv, index, argument), argument);
        } else if (argument == "--rows") {
            options.row_count = parse_size(next_value(argc, argv, index, argument), argument);
            options.row_count_was_set = true;
        } else if (argument == "--weight") {
            options.odd_weight = parse_size(next_value(argc, argv, index, argument), argument);
        } else if (argument == "--warmups") {
            options.warmups = parse_size(next_value(argc, argv, index, argument), argument);
        } else if (argument == "--reps") {
            options.repetitions = parse_size(next_value(argc, argv, index, argument), argument);
        } else if (argument == "--workers") {
            options.workers = parse_workers(next_value(argc, argv, index, argument));
        } else if (argument == "--parallel-threshold") {
            options.parallel_threshold =
                parse_size(next_value(argc, argv, index, argument), argument);
        } else if (argument == "--inner") {
            options.fbcheck_inner_iterations =
                parse_size(next_value(argc, argv, index, argument), argument);
        } else {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        }
    }
    if (!options.row_count_was_set) {
        if (options.factor_base_size > std::numeric_limits<size_t>::max() - size_t{100}) {
            throw std::invalid_argument("--fb is too large to derive the default row count");
        }
        options.row_count = options.factor_base_size + size_t{100};
    }
    return options;
}

void print_usage() {
    std::cout << "Usage: test_siqs_shadow_matrix_bench <mode> [options]\n"
                 "\n"
                 "Modes:\n"
                 "  solve     Public shadow solver for each requested worker count.\n"
                 "  kernel    Legacy jthreads, queued ThreadPool, and the production persistent "
                 "worker team.\n"
                 "  prepare   Public row-identity wrapper and the prevalidated helper.\n"
                 "  fbcheck   Repeated factor-base validation.\n"
                 "\n"
                 "Shape and sampling options:\n"
                 "  --fb N                  Factor-base entries including sign sentinel (default "
              << DEFAULT_FACTOR_BASE_SIZE
              << ").\n"
                 "  --rows N                Shadow rows (default --fb + 100; initial default "
              << DEFAULT_ROW_COUNT
              << ").\n"
                 "  --weight N              Odd non-sign columns per row (default "
              << DEFAULT_ODD_WEIGHT
              << ").\n"
                 "  --warmups N             Untimed passes in [0,20] (default "
              << DEFAULT_WARMUPS
              << ").\n"
                 "  --reps N                Timed passes in [1,50] (default "
              << DEFAULT_REPETITIONS
              << ").\n"
                 "  --workers all|N[,N...]  Worker counts for solve/kernel (default 1,2,4).\n"
                 "  --parallel-threshold N  Current-kernel parallel gate (default 0).\n"
                 "  --inner N                Validations per fbcheck timing sample (default "
              << DEFAULT_FBCHECK_INNER_ITERATIONS
              << ").\n"
                 "  --force-dense            Permit solve/kernel peak packed storage above 256 "
                 "MiB or 100000 rows.\n"
                 "  --help                   Show this text.\n"
                 "\n"
                 "The executable refuses benchmark runs without NDEBUG. Timings are reported but "
                 "never asserted.\n";
}

void validate_cli(const CliOptions& options) {
    require(NDEBUG_ENABLED,
            "this benchmark requires an optimized Release/NDEBUG build; rebuild with -DNDEBUG");
    require(options.factor_base_size >= 3 && options.factor_base_size <= MAX_FACTOR_BASE_SIZE,
            "--fb must be in [3,1000001]");
    require(options.row_count >= 1 && options.row_count <= MAX_ROW_COUNT,
            "--rows must be in [1,2000000]");
    require(options.odd_weight >= 1 && options.odd_weight <= MAX_ODD_WEIGHT &&
                options.odd_weight <= options.factor_base_size - size_t{2},
            "--weight must be in [1,min(4096,--fb-2)]");
    require(options.warmups <= MAX_WARMUPS, "--warmups must be in [0,20]");
    require(options.repetitions >= 1 && options.repetitions <= MAX_REPETITIONS,
            "--reps must be in [1,50]");
    require(options.fbcheck_inner_iterations >= 1 &&
                options.fbcheck_inner_iterations <= MAX_FBCHECK_INNER_ITERATIONS,
            "--inner must be in [1,100000000]");
    require(!options.workers.empty(), "at least one worker count is required");
}

[[nodiscard]] PackedByteProjection project_packed_bytes(const CliOptions& options) {
    if (options.mode != BenchmarkMode::solve && options.mode != BenchmarkMode::kernel) {
        return {};
    }

    const auto single_dense_bytes =
        checked_siqs_shadow_dense_matrix_bytes(options.row_count, options.factor_base_size);
    require(single_dense_bytes.has_value(),
            "the requested packed dense matrix size overflows size_t");
    const size_t packed_copy_count = options.mode == BenchmarkMode::kernel ? size_t{2} : size_t{1};
    require(*single_dense_bytes <= std::numeric_limits<size_t>::max() / packed_copy_count,
            "the requested peak packed storage size overflows size_t");
    return {*single_dense_bytes, *single_dense_bytes * packed_copy_count};
}

void validate_dense_request(const CliOptions& options, const PackedByteProjection& projection) {
    if (options.mode != BenchmarkMode::solve && options.mode != BenchmarkMode::kernel) {
        return;
    }
    require(projection.single_dense_bytes.has_value() && projection.peak_packed_bytes.has_value(),
            "dense mode is missing its packed-byte projection");

    const size_t peak_packed_bytes = *projection.peak_packed_bytes;
    const uint64_t peak_limit =
        options.force_dense
            ? MAX_FORCED_DENSE_BYTES
            : static_cast<uint64_t>(gnfs::siqs::SIQS_SHADOW_DEFAULT_MAX_DENSE_MATRIX_BYTES);
    if (static_cast<uint64_t>(peak_packed_bytes) > peak_limit) {
        fail(std::string(mode_name(options.mode)) + " projects peak packed storage of " +
             std::to_string(peak_packed_bytes) + " bytes, above the " +
             (options.force_dense ? "8 GiB forced" : "256 MiB default") + " limit" +
             (options.force_dense ? std::string{}
                                  : "; pass --force-dense explicitly after reviewing the "
                                    "allocation"));
    }
    if (!options.force_dense &&
        options.row_count > gnfs::siqs::SIQS_SHADOW_DEFAULT_MAX_DENSE_VARIABLE_COUNT) {
        fail("dense solve/kernel request exceeds the 100000-row safe default; pass --force-dense "
             "explicitly after reviewing the allocation");
    }
}

[[nodiscard]] std::vector<uint32_t> make_factor_base(size_t count) {
    require(count >= 3, "factor-base generation requires at least three entries");
    const size_t prime_count = count - size_t{1};
    double estimate = 32.0;
    if (prime_count > 6) {
        const double n = static_cast<double>(prime_count);
        estimate = n * (std::log(n) + std::log(std::log(n))) + 32.0;
    }
    require(estimate < static_cast<double>(std::numeric_limits<uint32_t>::max()),
            "factor-base prime bound exceeds uint32_t");
    size_t limit = static_cast<size_t>(estimate);
    for (;;) {
        std::vector<bool> composite(limit + size_t{1}, false);
        for (size_t prime = 2; prime <= limit / prime; ++prime) {
            if (!composite[prime]) {
                for (size_t multiple = prime * prime; multiple <= limit; multiple += prime) {
                    composite[multiple] = true;
                }
            }
        }

        std::vector<uint32_t> factor_base;
        factor_base.reserve(count);
        factor_base.push_back(0);
        for (size_t candidate = 2; candidate <= limit && factor_base.size() < count; ++candidate) {
            if (!composite[candidate]) {
                factor_base.push_back(static_cast<uint32_t>(candidate));
            }
        }
        if (factor_base.size() == count) {
            return factor_base;
        }
        require(limit <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()) / size_t{2},
                "factor-base sieve bound overflow");
        limit *= size_t{2};
    }
}

[[nodiscard]] std::vector<SIQSShadowRow> make_rows(size_t factor_base_size, size_t row_count,
                                                   size_t odd_weight) {
    require(factor_base_size >= 3 && odd_weight <= factor_base_size - size_t{2},
            "invalid synthetic row shape");
    std::vector<SIQSShadowRow> rows;
    rows.reserve(row_count);
    for (size_t row_index = 0; row_index < row_count; ++row_index) {
        uint64_t state =
            BENCHMARK_SEED ^ (static_cast<uint64_t>(row_index) * UINT64_C(0xd6e8feb86659fd93));
        std::vector<uint32_t> columns;
        columns.reserve(odd_weight);
        while (columns.size() < odd_weight) {
            const uint32_t column = static_cast<uint32_t>(
                size_t{2} + splitmix64(state) % (factor_base_size - size_t{2}));
            if (std::find(columns.begin(), columns.end(), column) == columns.end()) {
                columns.push_back(column);
            }
        }
        std::sort(columns.begin(), columns.end());

        std::vector<SIQSFactorPower> powers;
        powers.reserve(columns.size());
        for (const uint32_t column : columns) {
            powers.push_back(SIQSFactorPower{column, 1});
        }
        rows.push_back(
            SIQSShadowRow{row_index % size_t{2} == 0 ? SIQSShadowRowOrigin::raw_full
                                                     : SIQSShadowRowOrigin::large_prime_cycle,
                          SIQSPostMergeRow{Integer(1),
                                           (splitmix64(state) & UINT64_C(1)) != 0,
                                           std::move(powers),
                                           {},
                                           {SIQSSourceId{static_cast<uint64_t>(row_index)}}}});
    }
    return rows;
}

[[nodiscard]] uint64_t dependency_digest(const SIQSShadowMatrixResult& result) {
    require(result.is_valid() && result.solution().has_value(),
            "public shadow solver returned status " +
                std::to_string(static_cast<unsigned>(result.status())));
    const auto& solution = *result.solution();
    uint64_t digest = UINT64_C(14695981039346656037);
    digest_u64(digest, static_cast<uint64_t>(solution.row_count));
    digest_u64(digest, static_cast<uint64_t>(solution.column_count));
    digest_u64(digest, static_cast<uint64_t>(solution.dependencies.size()));
    for (const auto& dependency : solution.dependencies) {
        digest_u64(digest, static_cast<uint64_t>(dependency.size()));
        for (const size_t row_index : dependency) {
            digest_u64(digest, static_cast<uint64_t>(row_index));
        }
    }
    return digest;
}

[[nodiscard]] uint64_t matrix_digest(KernelView view) noexcept {
    uint64_t digest = UINT64_C(14695981039346656037);
    digest_u64(digest, static_cast<uint64_t>(view.matrix.size()));
    for (const uint64_t word : view.matrix) {
        digest_u64(digest, word);
    }
    digest_u64(digest, static_cast<uint64_t>(view.pivots.size()));
    for (const size_t pivot : view.pivots) {
        digest_u64(digest, static_cast<uint64_t>(pivot));
    }
    return digest;
}

[[nodiscard]] uint64_t median_sample(std::vector<uint64_t> samples) {
    require(!samples.empty(), "cannot summarize an empty timing sample set");
    std::sort(samples.begin(), samples.end());
    const size_t upper = samples.size() / size_t{2};
    if (samples.size() % size_t{2} != 0) {
        return samples[upper];
    }
    return samples[upper - size_t{1}] + (samples[upper] - samples[upper - size_t{1}]) / uint64_t{2};
}

template <typename Operation, typename Observer>
[[nodiscard]] TimingSummary measure(size_t warmups, size_t repetitions, Operation&& operation,
                                    Observer&& observer) {
    std::optional<uint64_t> reference_digest;
    auto observe_once = [&](auto&& outcome) {
        const uint64_t digest = observer(std::forward<decltype(outcome)>(outcome));
        if (reference_digest.has_value()) {
            require(digest == *reference_digest, "result identity changed across repetitions");
        } else {
            reference_digest = digest;
        }
    };

    for (size_t warmup = 0; warmup < warmups; ++warmup) {
        auto outcome = operation();
        observe_once(outcome);
    }

    std::vector<uint64_t> samples;
    samples.reserve(repetitions);
    for (size_t repetition = 0; repetition < repetitions; ++repetition) {
        const auto started = Clock::now();
        auto outcome = operation();
        const auto finished = Clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();
        require(elapsed >= 0, "steady clock moved backwards");
        observe_once(outcome);
        samples.push_back(static_cast<uint64_t>(elapsed));
    }

    const auto [minimum, maximum] = std::minmax_element(samples.begin(), samples.end());
    return TimingSummary{*minimum, median_sample(samples), *maximum, *reference_digest};
}

[[nodiscard]] std::vector<uint64_t> pack_rows(std::span<const SIQSShadowRow> rows,
                                              size_t equation_count) {
    const size_t words_per_equation =
        rows.size() / size_t{64} + (rows.size() % size_t{64} != 0 ? size_t{1} : size_t{0});
    std::vector<uint64_t> matrix(equation_count * words_per_equation, uint64_t{0});
    for (size_t variable = 0; variable < rows.size(); ++variable) {
        const size_t word = variable / size_t{64};
        const uint64_t mask = uint64_t{1} << (variable % size_t{64});
        gnfs::siqs::visit_siqs_post_merge_odd_columns(rows[variable].row, [&](size_t equation) {
            matrix[equation * words_per_equation + word] |= mask;
        });
    }
    return matrix;
}

enum class KernelImplementation : uint8_t {
    legacy_per_pivot_jthread,
    benchmark_only_queued_thread_pool,
    production_persistent_worker_team,
};

[[nodiscard]] std::string_view kernel_implementation_name(KernelImplementation implementation) {
    switch (implementation) {
    case KernelImplementation::legacy_per_pivot_jthread:
        return "legacy_per_pivot_jthread";
    case KernelImplementation::benchmark_only_queued_thread_pool:
        return "benchmark_only_queued_thread_pool";
    case KernelImplementation::production_persistent_worker_team:
        return "production_persistent_worker_team";
    }
    return "unknown";
}

/// Benchmark-only reduction driver. Pivot search and elimination reuse the
/// existing helpers. The queued ThreadPool branch is only a comparison
/// prototype; the persistent-worker branch directly owns the production team.
class BenchmarkKernel final {
public:
    BenchmarkKernel(std::span<const SIQSShadowRow> rows, size_t equation_count, uint32_t workers,
                    size_t parallel_threshold, KernelImplementation implementation)
        : variable_count_(rows.size()), equation_count_(equation_count),
          words_per_equation_(rows.size() / size_t{64} +
                              (rows.size() % size_t{64} != 0 ? size_t{1} : size_t{0})),
          worker_count_(std::min(equation_count, static_cast<size_t>(workers))),
          use_parallel_(workers > 1 && equation_count >= parallel_threshold),
          implementation_(implementation), initial_(pack_rows(rows, equation_count)),
          matrix_(initial_.size()), pivots_(equation_count) {
        options_.max_dependencies = 64;
        options_.elimination_workers = workers;
        options_.parallel_column_threshold = parallel_threshold;
        if (!use_parallel_) {
            return;
        }
        if (implementation_ == KernelImplementation::benchmark_only_queued_thread_pool) {
            queued_pool_ =
                std::make_unique<gnfs::util::ThreadPool>(static_cast<uint32_t>(worker_count_));
            queued_pool_->parallel_for_index(size_t{0}, worker_count_, [](size_t) {});
        } else if (implementation_ == KernelImplementation::production_persistent_worker_team) {
            auto status =
                gnfs::siqs::shadow_matrix_detail::create_persistent_pivot_elimination_team(
                    matrix_, equation_count_, words_per_equation_, worker_count_, production_team_);
            require(status == SIQSShadowMatrixStatus::valid && production_team_ != nullptr,
                    "production persistent elimination team construction failed");
            // matrix_ is still all-zero. One no-op dispatch completes worker
            // startup and the first generation rendezvous outside timed scope.
            status = production_team_->eliminate(0, 0);
            require(status == SIQSShadowMatrixStatus::valid,
                    "production persistent elimination team startup failed");
        }
    }

    [[nodiscard]] KernelView run() {
        using namespace gnfs::siqs::shadow_matrix_detail;
        const size_t no_pivot = std::numeric_limits<size_t>::max();
        std::copy(initial_.begin(), initial_.end(), matrix_.begin());
        std::fill(pivots_.begin(), pivots_.end(), no_pivot);

        for (size_t equation = 0; equation < equation_count_; ++equation) {
            const size_t offset = equation * words_per_equation_;
            const size_t pivot = leftmost_set_bit(
                std::span<const uint64_t>(matrix_.data() + offset, words_per_equation_),
                variable_count_);
            if (pivot == no_pivot) {
                continue;
            }
            pivots_[equation] = pivot;

            if (!use_parallel_) {
                eliminate_pivot_range(matrix_, words_per_equation_, equation, pivot, 0,
                                      equation_count_);
                continue;
            }

            SIQSShadowMatrixStatus status = SIQSShadowMatrixStatus::valid;
            switch (implementation_) {
            case KernelImplementation::legacy_per_pivot_jthread:
                status = eliminate_pivot(matrix_, equation_count_, words_per_equation_, equation,
                                         pivot, options_);
                break;
            case KernelImplementation::benchmark_only_queued_thread_pool:
                require(queued_pool_ != nullptr, "queued ThreadPool is missing");
                queued_pool_->parallel_for_index(size_t{0}, worker_count_, [&](size_t worker) {
                    const size_t base_range = equation_count_ / worker_count_;
                    const size_t remainder = equation_count_ % worker_count_;
                    const size_t begin = worker * base_range + std::min(worker, remainder);
                    const size_t end =
                        begin + base_range + (worker < remainder ? size_t{1} : size_t{0});
                    eliminate_pivot_range(matrix_, words_per_equation_, equation, pivot, begin,
                                          end);
                });
                break;
            case KernelImplementation::production_persistent_worker_team:
                require(production_team_ != nullptr, "production persistent team is missing");
                status = production_team_->eliminate(equation, pivot);
                break;
            }
            require(status == SIQSShadowMatrixStatus::valid,
                    "kernel elimination returned status " +
                        std::to_string(static_cast<unsigned>(status)));
        }
        return {matrix_, pivots_};
    }

private:
    size_t variable_count_;
    size_t equation_count_;
    size_t words_per_equation_;
    size_t worker_count_;
    bool use_parallel_;
    KernelImplementation implementation_;
    SIQSShadowMatrixOptions options_;
    std::vector<uint64_t> initial_;
    std::vector<uint64_t> matrix_;
    std::vector<size_t> pivots_;
    std::unique_ptr<gnfs::util::ThreadPool> queued_pool_;
    std::unique_ptr<gnfs::siqs::shadow_matrix_detail::PersistentPivotEliminationTeam>
        production_team_;
};

void emit_optional_size(std::ostream& output, const std::optional<size_t>& value) {
    if (value.has_value()) {
        output << *value;
    } else {
        output << "na";
    }
}

void emit_configuration(const CliOptions& options, const PackedByteProjection& projection) {
    std::cout << "GNFS_SIQS_SHADOW_MATRIX_BENCH_CONFIG_V1"
              << " mode=" << mode_name(options.mode) << " build_contract=" << BUILD_CONTRACT
              << " cmake_build_type=" << CMAKE_BUILD_TYPE << " timing_asserted=false"
              << " seed=" << hexadecimal(BENCHMARK_SEED)
              << " factor_base_entries=" << options.factor_base_size
              << " rows=" << options.row_count << " odd_weight=" << options.odd_weight
              << " warmups=" << options.warmups << " reps=" << options.repetitions
              << " parallel_threshold=" << options.parallel_threshold
              << " projected_single_dense_bytes=";
    emit_optional_size(std::cout, projection.single_dense_bytes);
    std::cout << " projected_peak_packed_bytes=";
    emit_optional_size(std::cout, projection.peak_packed_bytes);
    std::cout << " force_dense=" << (options.force_dense ? "true" : "false") << '\n';
}

void emit_result(const CliOptions& options, std::string_view implementation, uint32_t workers,
                 const TimingSummary& summary, bool is_dependency_digest,
                 std::string_view prototype = "false", size_t inner_iterations = 1) {
    std::cout << "GNFS_SIQS_SHADOW_MATRIX_BENCH_RESULT_V1"
              << " status=ok"
              << " mode=" << mode_name(options.mode) << " implementation=" << implementation
              << " prototype=" << prototype << " build_contract=" << BUILD_CONTRACT
              << " cmake_build_type=" << CMAKE_BUILD_TYPE << " timing_asserted=false"
              << " seed=" << hexadecimal(BENCHMARK_SEED)
              << " factor_base_entries=" << options.factor_base_size
              << " rows=" << options.row_count << " odd_weight=" << options.odd_weight
              << " workers=" << workers << " parallel_threshold=" << options.parallel_threshold
              << " warmups=" << options.warmups << " reps=" << options.repetitions
              << " inner_iterations=" << inner_iterations << " wall_min_ns=" << summary.minimum_ns
              << " wall_median_ns=" << summary.median_ns << " wall_max_ns=" << summary.maximum_ns
              << " dependency_digest="
              << (is_dependency_digest ? hexadecimal(summary.result_digest) : "na")
              << " result_digest=" << hexadecimal(summary.result_digest) << '\n';
}

void validate_generated_rows(std::span<const SIQSShadowRow> rows,
                             std::span<const uint32_t> factor_base, const Integer& modulus) {
    require(gnfs::siqs::post_merge_row_detail::has_valid_modulus(modulus),
            "synthetic modulus is invalid");
    require(gnfs::siqs::post_merge_row_detail::has_valid_factor_base(factor_base),
            "synthetic factor base is invalid");
    for (const auto& row : rows) {
        require(gnfs::siqs::post_merge_row_detail::check_siqs_post_merge_row_identity_prevalidated(
                    row.row, factor_base, modulus) == SIQSPostMergeRowStatus::valid,
                "synthetic row identity is invalid");
    }
}

void run_solve(const CliOptions& options, std::span<const SIQSShadowRow> rows,
               std::span<const uint32_t> factor_base, const Integer& modulus) {
    std::optional<uint64_t> cross_worker_digest;
    for (const uint32_t workers : options.workers) {
        SIQSShadowMatrixOptions solver_options;
        solver_options.max_dependencies = 64;
        solver_options.elimination_workers = workers;
        solver_options.parallel_column_threshold = options.parallel_threshold;
        if (options.force_dense) {
            solver_options.max_dense_matrix_bytes = std::numeric_limits<size_t>::max();
            solver_options.max_dense_variable_count = std::numeric_limits<size_t>::max();
        }
        const auto summary = measure(
            options.warmups, options.repetitions,
            [&] { return solve_siqs_shadow_matrix(rows, factor_base, modulus, solver_options); },
            [](const SIQSShadowMatrixResult& result) { return dependency_digest(result); });
        if (cross_worker_digest.has_value()) {
            require(summary.result_digest == *cross_worker_digest,
                    "public solver dependency identity differs across worker counts");
        } else {
            cross_worker_digest = summary.result_digest;
        }
        emit_result(options, "public_solver", workers, summary, true);
    }
}

void run_kernel(const CliOptions& options, std::span<const SIQSShadowRow> rows) {
    constexpr std::array implementations{
        KernelImplementation::legacy_per_pivot_jthread,
        KernelImplementation::benchmark_only_queued_thread_pool,
        KernelImplementation::production_persistent_worker_team,
    };
    std::optional<uint64_t> cross_implementation_digest;
    for (const uint32_t workers : options.workers) {
        for (const KernelImplementation implementation : implementations) {
            BenchmarkKernel kernel(rows, options.factor_base_size, workers,
                                   options.parallel_threshold, implementation);
            const auto summary = measure(
                options.warmups, options.repetitions, [&] { return kernel.run(); },
                [](KernelView view) { return matrix_digest(view); });
            if (cross_implementation_digest.has_value()) {
                require(summary.result_digest == *cross_implementation_digest,
                        "kernel identity differs across implementations or worker counts");
            } else {
                cross_implementation_digest = summary.result_digest;
            }
            emit_result(options, kernel_implementation_name(implementation), workers, summary,
                        false,
                        implementation == KernelImplementation::benchmark_only_queued_thread_pool
                            ? "benchmark_only"
                            : "false");
        }
    }
}

[[nodiscard]] uint64_t public_prepare_pass(std::span<const SIQSShadowRow> rows,
                                           std::span<const uint32_t> factor_base,
                                           const Integer& modulus) {
    uint64_t digest = UINT64_C(14695981039346656037);
    for (const auto& row : rows) {
        const auto status = check_siqs_post_merge_row_identity(row.row, factor_base, modulus);
        digest_byte(digest, static_cast<uint8_t>(status));
    }
    return digest;
}

[[nodiscard]] uint64_t prevalidated_prepare_pass(std::span<const SIQSShadowRow> rows,
                                                 std::span<const uint32_t> factor_base,
                                                 const Integer& modulus) {
    require(gnfs::siqs::post_merge_row_detail::has_valid_modulus(modulus),
            "prevalidated prepare modulus check failed");
    require(gnfs::siqs::post_merge_row_detail::has_valid_factor_base(factor_base),
            "prevalidated prepare factor-base check failed");
    uint64_t digest = UINT64_C(14695981039346656037);
    for (const auto& row : rows) {
        const auto status =
            gnfs::siqs::post_merge_row_detail::check_siqs_post_merge_row_identity_prevalidated(
                row.row, factor_base, modulus);
        digest_byte(digest, static_cast<uint8_t>(status));
    }
    return digest;
}

void run_prepare(const CliOptions& options, std::span<const SIQSShadowRow> rows,
                 std::span<const uint32_t> factor_base, const Integer& modulus) {
    const auto public_summary = measure(
        options.warmups, options.repetitions,
        [&] { return public_prepare_pass(rows, factor_base, modulus); },
        [](uint64_t digest) { return digest; });
    const auto prevalidated_summary = measure(
        options.warmups, options.repetitions,
        [&] { return prevalidated_prepare_pass(rows, factor_base, modulus); },
        [](uint64_t digest) { return digest; });
    require(public_summary.result_digest == prevalidated_summary.result_digest,
            "public and prevalidated row checks produced different identities");
    emit_result(options, "public_identity_wrapper", 1, public_summary, false);
    emit_result(options, "prevalidated_identity_helper", 1, prevalidated_summary, false);
}

#if defined(_MSC_VER)
#define GNFS_BENCH_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define GNFS_BENCH_NOINLINE __attribute__((noinline))
#else
#define GNFS_BENCH_NOINLINE
#endif

[[nodiscard]] GNFS_BENCH_NOINLINE bool
validate_factor_base_once(std::span<const uint32_t> factor_base) {
    const uint64_t barrier = fbcheck_call_barrier;
    fbcheck_call_barrier = barrier + UINT64_C(1);
    return gnfs::siqs::post_merge_row_detail::has_valid_factor_base(factor_base);
}

#undef GNFS_BENCH_NOINLINE

void run_fbcheck(const CliOptions& options, std::span<const uint32_t> factor_base) {
    const auto summary = measure(
        options.warmups, options.repetitions,
        [&] {
            size_t valid_count = 0;
            for (size_t iteration = 0; iteration < options.fbcheck_inner_iterations; ++iteration) {
                valid_count += validate_factor_base_once(factor_base) ? size_t{1} : size_t{0};
            }
            return valid_count;
        },
        [&](size_t valid_count) {
            require(valid_count == options.fbcheck_inner_iterations,
                    "factor-base validation unexpectedly failed");
            uint64_t digest = UINT64_C(14695981039346656037);
            digest_u64(digest, static_cast<uint64_t>(valid_count));
            digest_u64(digest, static_cast<uint64_t>(factor_base.size()));
            return digest;
        });
    emit_result(options, "factor_base_validation", 1, summary, false, "false",
                options.fbcheck_inner_iterations);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const CliOptions options = parse_cli(argc, argv);
        if (options.help) {
            print_usage();
            return 0;
        }
        validate_cli(options);
        const PackedByteProjection projection = project_packed_bytes(options);
        validate_dense_request(options, projection);
        emit_configuration(options, projection);

        gnfs::util::set_current_thread_qos(gnfs::util::QoSClass::UserInitiated);
        const auto factor_base = make_factor_base(options.factor_base_size);
        const auto factor_base_span =
            std::span<const uint32_t>(factor_base.data(), factor_base.size());
        if (options.mode == BenchmarkMode::fbcheck) {
            run_fbcheck(options, factor_base_span);
            return 0;
        }

        const auto rows =
            make_rows(options.factor_base_size, options.row_count, options.odd_weight);
        const auto row_span = std::span<const SIQSShadowRow>(rows.data(), rows.size());
        const Integer modulus(2);
        validate_generated_rows(row_span, factor_base_span, modulus);
        switch (options.mode) {
        case BenchmarkMode::solve:
            run_solve(options, row_span, factor_base_span, modulus);
            break;
        case BenchmarkMode::kernel:
            run_kernel(options, row_span);
            break;
        case BenchmarkMode::prepare:
            run_prepare(options, row_span, factor_base_span, modulus);
            break;
        case BenchmarkMode::fbcheck:
            break;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    } catch (...) {
        std::cerr << "error: unknown exception\n";
        return 2;
    }
}
