#include "gnfs/api/pipeline.hpp"
#include "gnfs/cofactor/candidate_batch.hpp"
#include "gnfs/relation/ooc_relation_store.hpp"
#include "gnfs/util/msvc_compat.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/process_memory.hpp"
#include "gnfs/util/temp_path.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using gnfs::api::Config;
using gnfs::api::FactorizationMethod;
using gnfs::api::LegacyRawOOCCleanupPolicy;
using gnfs::api::LogEntry;
using gnfs::api::Phase;
using gnfs::api::Pipeline;
using gnfs::api::ProgressInfo;
using gnfs::api::SieveCollectionOptions;
using gnfs::api::SieveStopReason;
using gnfs::core::Integer;
using gnfs::relation::CorpusDigest;
using gnfs::relation::OOCCorpusArtifactScope;
using gnfs::relation::ReductionStrategy;
using gnfs::relation::RelationStorageKind;
using gnfs::relation::StructuredReductionStopReason;
using gnfs::util::ProcessMemoryBackend;
using gnfs::util::ProcessMemorySnapshot;

constexpr std::string_view PROBE_N = "16000000000000004000000216000000000000027000000729";
constexpr size_t PROBE_BITS = 164;
constexpr size_t PROBE_DIGITS = 50;
constexpr size_t DEFAULT_MAX_SPECIAL_Q = 4;
constexpr size_t MIN_MAX_SPECIAL_Q = 1;
constexpr size_t MAX_MAX_SPECIAL_Q = std::numeric_limits<uint32_t>::max();
constexpr uint32_t DEFAULT_MAX_SPECIAL_Q_BATCH_WORKERS = 4;
constexpr uint32_t MIN_MAX_SPECIAL_Q_BATCH_WORKERS = 1;
constexpr uint32_t MAX_MAX_SPECIAL_Q_BATCH_WORKERS = 4;

enum class ProbeStrategy : uint8_t {
    Legacy,
    Structured,
};

struct CliOptions final {
    ProbeStrategy strategy = ProbeStrategy::Structured;
    size_t max_special_q = DEFAULT_MAX_SPECIAL_Q;
    uint32_t max_special_q_batch_workers = DEFAULT_MAX_SPECIAL_Q_BATCH_WORKERS;
    std::optional<uint32_t> max_local_sieve_threads;
    std::optional<std::string> ooc_base;
    bool help = false;
};

class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable(std::string name, const std::string& value) : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
        }
        if (setenv(name_.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error("could not set environment variable " + name_);
        }
    }

    ScopedEnvironmentVariable(std::string name, std::nullopt_t) : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
        }
        if (unsetenv(name_.c_str()) != 0) {
            throw std::runtime_error("could not unset environment variable " + name_);
        }
    }

    ~ScopedEnvironmentVariable() {
        if (previous_.has_value()) {
            (void)setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            (void)unsetenv(name_.c_str());
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

struct ArtifactPairState final {
    bool data_exists = false;
    bool index_exists = false;
    bool inspection_failed = false;

    [[nodiscard]] bool complete() const noexcept {
        return !inspection_failed && data_exists && index_exists;
    }

    [[nodiscard]] bool absent() const noexcept {
        return !inspection_failed && !data_exists && !index_exists;
    }
};

struct CallbackEvidence final {
    size_t structured_filter_records = 0;
    size_t direct_ooc_filter_records = 0;
    size_t structured_matrix_records = 0;
    bool raw_pair_observed = false;
    bool raw_pair_incoherent = false;
    bool artifact_inspection_failed = false;
    bool sge_attempted = false;
    bool block_lanczos_attempted = false;
    bool block_wiedemann_attempted = false;
    bool square_root_attempted = false;
    bool factor_extraction_attempted = false;
    bool full_pipeline_done_observed = false;
    std::string structured_filter_record;
    std::string structured_matrix_record;
};

struct MemoryObservation final {
    ProcessMemoryBackend backend = ProcessMemoryBackend::Unsupported;
    std::optional<uint64_t> current_rss_bytes;
    std::optional<uint64_t> lifetime_peak_rss_bytes;
    bool captured = false;
};

struct ExperimentRecord final {
    std::string status = "fail";
    std::string failure_stage = "cli";
    std::string error = "none";
    std::string route = "unverified";
    std::string route_evidence = "unverified";
    std::string strategy = "unverified";
    std::string storage = "unverified";
    std::string structured_stop = "unverified";
    std::string sieve_stop_reason = "unverified";
    size_t max_special_q = DEFAULT_MAX_SPECIAL_Q;
    uint32_t max_special_q_batch_workers = DEFAULT_MAX_SPECIAL_Q_BATCH_WORKERS;
    size_t special_q_processed = 0;
    size_t special_q_batch_worker_limit = 0;
    size_t special_q_batch_peak_workers = 0;
    size_t special_q_batch_count = 0;
    size_t special_q_batch_peak_size = 0;
    uint32_t max_local_sieve_threads_requested = 0;
    size_t local_sieve_thread_budget = 0;
    size_t special_q_batch_peak_assigned_threads = 0;
    size_t special_q_worker_peak_sieve_threads = 0;
    size_t candidates_total = 0;
    size_t candidate_batch_peak_workers = 0;
    size_t candidate_batch_total_chunks = 0;
    size_t candidate_batch_peak_chunks = 0;
    size_t candidate_batch_peak_candidates = 0;
    size_t candidate_batch_rss_sample_candidates = 0;
    std::optional<uint64_t> candidate_batch_after_generation_current_rss_bytes;
    std::optional<uint64_t> candidate_batch_after_cofactor_current_rss_bytes;
    std::optional<uint64_t> candidate_batch_after_release_current_rss_bytes;
    double candidate_generation_s = 0.0;
    double candidate_cofactor_s = 0.0;
    size_t rational_fb_columns = 0;
    size_t algebraic_fb_columns = 0;
    size_t base_factor_columns = 0;
    size_t initial_raw_target = 0;
    bool first_round_complete = false;
    size_t sieve_rounds_completed = 0;
    uint64_t generation = 0;
    size_t raw_rows = 0;
    size_t raw_duplicates = 0;
    size_t input_lp_columns = 0;
    size_t input_lp_w1 = 0;
    size_t input_lp_w2 = 0;
    size_t input_lp_w3 = 0;
    size_t input_lp_w4plus = 0;
    size_t output_rows = 0;
    size_t output_lp_columns = 0;
    size_t structured_commits = 0;
    size_t structured_emitted_rows = 0;
    size_t incidence_shards = 0;
    uint32_t incidence_requested_workers = 0;
    uint32_t incidence_peak_workers = 0;
    CorpusDigest raw_digest{};
    CorpusDigest output_digest{};
    size_t matrix_rows = 0;
    size_t matrix_cols = 0;
    size_t matrix_nonzeros = 0;
    std::optional<int64_t> matrix_signed_delta;
    bool matrix_row_mapping_identity = false;
    size_t structured_filter_records = 0;
    size_t structured_matrix_records = 0;
    bool raw_pair_observed = false;
    bool raw_pair_removed = false;
    bool output_pair_observed = false;
    bool output_pair_retained_by_matrix = false;
    bool output_pair_removed = false;
    bool output_lease_removed = false;
    bool sge_attempted = false;
    bool solver_attempted = false;
    bool sqrt_attempted = false;
    bool factorization_attempted = false;
    MemoryObservation start_memory;
    MemoryObservation after_polynomial_memory;
    MemoryObservation after_factor_base_memory;
    MemoryObservation after_sieve_memory;
    MemoryObservation after_matrix_memory;
    MemoryObservation after_cleanup_memory;
    uint64_t polynomial_ms = 0;
    uint64_t factor_base_ms = 0;
    uint64_t sieve_ms = 0;
    uint64_t matrix_ms = 0;
    uint64_t total_ms = 0;
};

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

[[nodiscard]] ArtifactPairState inspect_artifact_pair(const std::string& base_path) noexcept {
    ArtifactPairState result;
    std::error_code data_error;
    std::error_code index_error;
    result.data_exists = std::filesystem::exists(base_path + ".reldata", data_error);
    result.index_exists = std::filesystem::exists(base_path + ".relidx", index_error);
    result.inspection_failed = static_cast<bool>(data_error) || static_cast<bool>(index_error);
    return result;
}

[[nodiscard]] bool path_exists(const std::string& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        throw std::runtime_error("could not inspect probe artifact path");
    }
    return exists;
}

[[nodiscard]] bool path_is_directory(const std::string& path) {
    std::error_code error;
    const bool is_directory = std::filesystem::is_directory(path, error);
    if (error) {
        throw std::runtime_error("could not inspect probe artifact directory");
    }
    return is_directory;
}

[[nodiscard]] size_t checked_add_size(size_t lhs, size_t rhs) {
    if (lhs > std::numeric_limits<size_t>::max() - rhs) {
        throw std::overflow_error("probe size addition overflow");
    }
    return lhs + rhs;
}

[[nodiscard]] int64_t signed_size_delta(size_t lhs, size_t rhs) {
    constexpr std::uintmax_t limit =
        static_cast<std::uintmax_t>(std::numeric_limits<int64_t>::max());
    if (lhs >= rhs) {
        const std::uintmax_t delta = static_cast<std::uintmax_t>(lhs - rhs);
        if (delta > limit) {
            throw std::overflow_error("positive matrix delta exceeds int64_t");
        }
        return static_cast<int64_t>(delta);
    }
    const std::uintmax_t magnitude = static_cast<std::uintmax_t>(rhs - lhs);
    const std::uintmax_t negative_limit = limit + std::uintmax_t{1};
    if (magnitude > negative_limit) {
        throw std::overflow_error("negative matrix delta exceeds int64_t");
    }
    if (magnitude == negative_limit) {
        return std::numeric_limits<int64_t>::min();
    }
    return -static_cast<int64_t>(magnitude);
}

[[nodiscard]] uint64_t elapsed_milliseconds(std::chrono::steady_clock::time_point start,
                                            std::chrono::steady_clock::time_point finish) noexcept {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(finish - start).count();
    return elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0;
}

[[nodiscard]] MemoryObservation capture_memory() noexcept {
    const ProcessMemorySnapshot snapshot = gnfs::util::process_memory_snapshot();
    return {
        snapshot.backend,
        snapshot.current_rss_bytes,
        snapshot.lifetime_peak_rss_bytes,
        true,
    };
}

[[nodiscard]] std::string_view strategy_name(ReductionStrategy strategy) noexcept {
    switch (strategy) {
    case ReductionStrategy::NoLargePrimes:
        return "no_large_primes";
    case ReductionStrategy::FilterOnly:
        return "filter_only";
    case ReductionStrategy::StandardV0:
        return "standard_v0";
    case ReductionStrategy::StandardV0WithV3:
        return "standard_v0_v3";
    case ReductionStrategy::CliqueV0:
        return "clique_v0";
    case ReductionStrategy::Structured:
        return "structured";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view probe_strategy_name(ProbeStrategy strategy) noexcept {
    switch (strategy) {
    case ProbeStrategy::Legacy:
        return "legacy";
    case ProbeStrategy::Structured:
        return "structured";
    }
    return "unknown";
}

[[nodiscard]] std::string_view storage_name(RelationStorageKind storage) noexcept {
    switch (storage) {
    case RelationStorageKind::InMemory:
        return "in_memory";
    case RelationStorageKind::FinalizedOOC:
        return "finalized_ooc";
    }
    return "unknown";
}

[[nodiscard]] std::string_view stop_reason_name(StructuredReductionStopReason reason) noexcept {
    switch (reason) {
    case StructuredReductionStopReason::NotStarted:
        return "not_started";
    case StructuredReductionStopReason::NoCandidates:
        return "no_candidates";
    case StructuredReductionStopReason::BudgetLimit:
        return "budget_limit";
    case StructuredReductionStopReason::PersistenceLimit:
        return "persistence_limit";
    }
    return "unknown";
}

[[nodiscard]] std::string sanitize_token(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (const char character : input) {
        const auto byte = static_cast<unsigned char>(character);
        const bool safe = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                          (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte == '.';
        output.push_back(safe ? static_cast<char>(byte) : '_');
        if (output.size() == 160) {
            break;
        }
    }
    return output.empty() ? "unknown" : output;
}

[[nodiscard]] constexpr std::string_view bool_token(bool value) noexcept {
    return value ? "true" : "false";
}

template <typename T> [[nodiscard]] std::string optional_token(const std::optional<T>& value) {
    return value.has_value() ? std::to_string(*value) : "na";
}

[[nodiscard]] std::string memory_backend_token(const ExperimentRecord& record) {
    const std::array<const MemoryObservation*, 6> observations = {
        &record.start_memory,
        &record.after_polynomial_memory,
        &record.after_factor_base_memory,
        &record.after_sieve_memory,
        &record.after_matrix_memory,
        &record.after_cleanup_memory,
    };
    for (const MemoryObservation* observation : observations) {
        if (observation->captured) {
            return std::string(gnfs::util::process_memory_backend_name(observation->backend));
        }
    }
    return "unobserved";
}

void validate_memory_backends(const ExperimentRecord& record) {
    const std::array<const MemoryObservation*, 6> observations = {
        &record.start_memory,
        &record.after_polynomial_memory,
        &record.after_factor_base_memory,
        &record.after_sieve_memory,
        &record.after_matrix_memory,
        &record.after_cleanup_memory,
    };
    std::optional<ProcessMemoryBackend> expected;
    for (const MemoryObservation* observation : observations) {
        if (!observation->captured) {
            continue;
        }
        if (!expected.has_value()) {
            expected = observation->backend;
        } else {
            require(observation->backend == *expected,
                    "process memory backend changed during one probe");
        }
    }
}

void emit_record(const ExperimentRecord& record, std::string_view prefix = "GNFS_EXPERIMENT_V2") {
    const auto emit_memory = [](std::ostream& output, std::string_view phase,
                                const MemoryObservation& observation) {
        output << " rss_" << phase
               << "_current_bytes=" << optional_token(observation.current_rss_bytes) << " rss_"
               << phase << "_peak_bytes=" << optional_token(observation.lifetime_peak_rss_bytes);
    };

    std::cout
        << prefix << " scope=bounded_50d_prefix_probe"
        << " claim_boundary=relation_reduction_and_matrix_shape_only"
        << " stop_after=matrix_build"
        << " pipeline_batch_mode=two_stage_candidate_batch"
        << " candidate_chunk_size=" << gnfs::cofactor::DEFAULT_CANDIDATE_CHUNK_SIZE
        << " candidate_rss_sample_policy=first_max_candidates"
        << " cofactor_inner_parallel_policy=forced_sequential"
        << " status=" << record.status << " failure_stage=" << record.failure_stage
        << " n=" << PROBE_N << " n_digits=" << PROBE_DIGITS << " n_bits=" << PROBE_BITS
        << " max_special_q=" << record.max_special_q
        << " max_special_q_batch_workers=" << record.max_special_q_batch_workers
        << " special_q_processed=" << record.special_q_processed
        << " special_q_batch_worker_limit=" << record.special_q_batch_worker_limit
        << " special_q_batch_peak_workers=" << record.special_q_batch_peak_workers
        << " special_q_batch_count=" << record.special_q_batch_count
        << " special_q_batch_peak_size=" << record.special_q_batch_peak_size
        << " max_local_sieve_threads_requested=" << record.max_local_sieve_threads_requested
        << " local_sieve_thread_budget=" << record.local_sieve_thread_budget
        << " special_q_batch_peak_assigned_threads=" << record.special_q_batch_peak_assigned_threads
        << " special_q_worker_peak_sieve_threads=" << record.special_q_worker_peak_sieve_threads
        << " candidates_total=" << record.candidates_total
        << " candidate_batch_peak_workers=" << record.candidate_batch_peak_workers
        << " candidate_batch_total_chunks=" << record.candidate_batch_total_chunks
        << " candidate_batch_peak_chunks=" << record.candidate_batch_peak_chunks
        << " candidate_batch_peak_candidates=" << record.candidate_batch_peak_candidates
        << " candidate_batch_rss_sample_candidates=" << record.candidate_batch_rss_sample_candidates
        << " candidate_batch_after_generation_current_rss_bytes="
        << optional_token(record.candidate_batch_after_generation_current_rss_bytes)
        << " candidate_batch_after_cofactor_current_rss_bytes="
        << optional_token(record.candidate_batch_after_cofactor_current_rss_bytes)
        << " candidate_batch_after_release_current_rss_bytes="
        << optional_token(record.candidate_batch_after_release_current_rss_bytes)
        << " rational_fb_columns=" << record.rational_fb_columns
        << " algebraic_fb_columns=" << record.algebraic_fb_columns
        << " base_factor_columns=" << record.base_factor_columns
        << " initial_raw_target=" << record.initial_raw_target
        << " first_round_complete=" << bool_token(record.first_round_complete)
        << " sieve_rounds_completed=" << record.sieve_rounds_completed
        << " sieve_stop_reason=" << record.sieve_stop_reason << " resume_scope=none"
        << " attempted_resume=false"
        << " attempted_distributed=false"
        << " sge_attempted=" << bool_token(record.sge_attempted)
        << " solver_attempted=" << bool_token(record.solver_attempted)
        << " sqrt_attempted=" << bool_token(record.sqrt_attempted)
        << " factorization_attempted=" << bool_token(record.factorization_attempted)
        << " route=" << record.route << " route_evidence=" << record.route_evidence
        << " strategy=" << record.strategy << " storage=" << record.storage
        << " generation=" << record.generation << " raw_rows=" << record.raw_rows
        << " raw_duplicates=" << record.raw_duplicates
        << " input_lp_columns=" << record.input_lp_columns << " input_lp_w1=" << record.input_lp_w1
        << " input_lp_w2=" << record.input_lp_w2 << " input_lp_w3=" << record.input_lp_w3
        << " input_lp_w4plus=" << record.input_lp_w4plus << " output_rows=" << record.output_rows
        << " output_lp_columns=" << record.output_lp_columns
        << " structured_commits=" << record.structured_commits
        << " structured_emitted_rows=" << record.structured_emitted_rows
        << " structured_stop=" << record.structured_stop
        << " incidence_shards=" << record.incidence_shards
        << " incidence_requested_workers=" << record.incidence_requested_workers
        << " incidence_peak_workers=" << record.incidence_peak_workers
        << " raw_digest_low=" << record.raw_digest.low
        << " raw_digest_high=" << record.raw_digest.high
        << " output_digest_low=" << record.output_digest.low
        << " output_digest_high=" << record.output_digest.high
        << " matrix_rows=" << record.matrix_rows << " matrix_cols=" << record.matrix_cols
        << " matrix_nonzeros=" << record.matrix_nonzeros
        << " matrix_signed_delta=" << optional_token(record.matrix_signed_delta)
        << " matrix_row_mapping_identity=" << bool_token(record.matrix_row_mapping_identity)
        << " structured_filter_records=" << record.structured_filter_records
        << " structured_matrix_records=" << record.structured_matrix_records
        << " raw_pair_observed=" << bool_token(record.raw_pair_observed)
        << " raw_pair_removed=" << bool_token(record.raw_pair_removed)
        << " output_pair_observed=" << bool_token(record.output_pair_observed)
        << " output_pair_retained_by_matrix=" << bool_token(record.output_pair_retained_by_matrix)
        << " output_pair_removed=" << bool_token(record.output_pair_removed)
        << " output_lease_removed=" << bool_token(record.output_lease_removed)
        << " process_rss_scope=self_lifetime"
        << " process_rss_backend=" << memory_backend_token(record)
        << " process_current_rss_supported="
        << bool_token(record.after_cleanup_memory.current_rss_bytes.has_value())
        << " process_peak_rss_supported="
        << bool_token(record.after_cleanup_memory.lifetime_peak_rss_bytes.has_value())
        << " process_current_rss_bytes="
        << optional_token(record.after_cleanup_memory.current_rss_bytes)
        << " process_peak_rss_bytes="
        << optional_token(record.after_cleanup_memory.lifetime_peak_rss_bytes);
    emit_memory(std::cout, "start", record.start_memory);
    emit_memory(std::cout, "after_polynomial", record.after_polynomial_memory);
    emit_memory(std::cout, "after_factor_base", record.after_factor_base_memory);
    emit_memory(std::cout, "after_sieve", record.after_sieve_memory);
    emit_memory(std::cout, "after_matrix", record.after_matrix_memory);
    emit_memory(std::cout, "after_cleanup", record.after_cleanup_memory);
    std::cout << " polynomial_ms=" << record.polynomial_ms
              << " factor_base_ms=" << record.factor_base_ms << " sieve_ms=" << record.sieve_ms
              << " candidate_generation_s=" << record.candidate_generation_s
              << " candidate_cofactor_s=" << record.candidate_cofactor_s
              << " matrix_ms=" << record.matrix_ms << " wall_ms=" << record.total_ms
              << " error=" << record.error << '\n';
}

[[nodiscard]] ExperimentRecord contract_fixture_record() {
    ExperimentRecord record;
    record.status = "pass";
    record.failure_stage = "none";
    record.error = "none";
    record.route = "structured";
    record.route_evidence = "production_direct_ooc";
    record.strategy = "structured";
    record.storage = "finalized_ooc";
    record.structured_stop = "no_candidates";
    record.sieve_stop_reason = "special_q_budget_reached";
    record.sieve_rounds_completed = 1;
    record.generation = 1;
    record.matrix_signed_delta = 0;
    record.matrix_row_mapping_identity = true;
    record.raw_pair_observed = true;
    record.raw_pair_removed = true;
    return record;
}

[[nodiscard]] ProbeStrategy parse_probe_strategy(std::string_view text) {
    if (text == "legacy") {
        return ProbeStrategy::Legacy;
    }
    if (text == "structured") {
        return ProbeStrategy::Structured;
    }
    throw std::invalid_argument("--strategy must be legacy or structured");
}

[[nodiscard]] size_t parse_max_special_q(std::string_view text) {
    uint64_t parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [position, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || position != end || parsed < MIN_MAX_SPECIAL_Q ||
        parsed > MAX_MAX_SPECIAL_Q) {
        throw std::invalid_argument("--max-special-q must be an integer in [1,UINT32_MAX]");
    }
    return static_cast<size_t>(parsed);
}

[[nodiscard]] uint32_t parse_max_special_q_batch_workers(std::string_view text) {
    uint64_t parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [position, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || position != end || parsed < MIN_MAX_SPECIAL_Q_BATCH_WORKERS ||
        parsed > MAX_MAX_SPECIAL_Q_BATCH_WORKERS) {
        throw std::invalid_argument("--max-special-q-batch-workers must be an integer in [1,4]");
    }
    return static_cast<uint32_t>(parsed);
}

[[nodiscard]] uint32_t parse_max_local_sieve_threads(std::string_view text) {
    uint64_t parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [position, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || position != end || parsed == 0 ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument(
            "--max-local-sieve-threads must be an integer in [1,UINT32_MAX]");
    }
    return static_cast<uint32_t>(parsed);
}

[[nodiscard]] CliOptions parse_cli(int argc, char** argv) {
    CliOptions options;
    bool strategy_seen = false;
    bool max_special_q_seen = false;
    bool max_special_q_batch_workers_seen = false;
    bool max_local_sieve_threads_seen = false;
    bool ooc_base_seen = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            options.help = true;
            continue;
        }
        if (argument == "--strategy") {
            if (strategy_seen) {
                throw std::invalid_argument("--strategy may be specified only once");
            }
            if (index + 1 >= argc) {
                throw std::invalid_argument("--strategy requires a value");
            }
            options.strategy = parse_probe_strategy(argv[++index]);
            strategy_seen = true;
            continue;
        }
        constexpr std::string_view strategy_prefix = "--strategy=";
        if (argument.starts_with(strategy_prefix)) {
            if (strategy_seen) {
                throw std::invalid_argument("--strategy may be specified only once");
            }
            options.strategy = parse_probe_strategy(argument.substr(strategy_prefix.size()));
            strategy_seen = true;
            continue;
        }
        if (argument == "--max-special-q") {
            if (max_special_q_seen) {
                throw std::invalid_argument("--max-special-q may be specified only once");
            }
            if (index + 1 >= argc) {
                throw std::invalid_argument("--max-special-q requires a value");
            }
            options.max_special_q = parse_max_special_q(argv[++index]);
            max_special_q_seen = true;
            continue;
        }
        if (argument == "--max-special-q-batch-workers") {
            if (max_special_q_batch_workers_seen) {
                throw std::invalid_argument(
                    "--max-special-q-batch-workers may be specified only once");
            }
            if (index + 1 >= argc) {
                throw std::invalid_argument("--max-special-q-batch-workers requires a value");
            }
            options.max_special_q_batch_workers = parse_max_special_q_batch_workers(argv[++index]);
            max_special_q_batch_workers_seen = true;
            continue;
        }
        constexpr std::string_view workers_prefix = "--max-special-q-batch-workers=";
        if (argument.starts_with(workers_prefix)) {
            if (max_special_q_batch_workers_seen) {
                throw std::invalid_argument(
                    "--max-special-q-batch-workers may be specified only once");
            }
            options.max_special_q_batch_workers =
                parse_max_special_q_batch_workers(argument.substr(workers_prefix.size()));
            max_special_q_batch_workers_seen = true;
            continue;
        }
        if (argument == "--max-local-sieve-threads") {
            if (max_local_sieve_threads_seen) {
                throw std::invalid_argument("--max-local-sieve-threads may be specified only once");
            }
            if (index + 1 >= argc) {
                throw std::invalid_argument("--max-local-sieve-threads requires a value");
            }
            options.max_local_sieve_threads = parse_max_local_sieve_threads(argv[++index]);
            max_local_sieve_threads_seen = true;
            continue;
        }
        constexpr std::string_view sieve_threads_prefix = "--max-local-sieve-threads=";
        if (argument.starts_with(sieve_threads_prefix)) {
            if (max_local_sieve_threads_seen) {
                throw std::invalid_argument("--max-local-sieve-threads may be specified only once");
            }
            options.max_local_sieve_threads =
                parse_max_local_sieve_threads(argument.substr(sieve_threads_prefix.size()));
            max_local_sieve_threads_seen = true;
            continue;
        }
        constexpr std::string_view prefix = "--max-special-q=";
        if (argument.starts_with(prefix)) {
            if (max_special_q_seen) {
                throw std::invalid_argument("--max-special-q may be specified only once");
            }
            options.max_special_q = parse_max_special_q(argument.substr(prefix.size()));
            max_special_q_seen = true;
            continue;
        }
        if (argument == "--ooc-base") {
            if (ooc_base_seen) {
                throw std::invalid_argument("--ooc-base may be specified only once");
            }
            if (index + 1 >= argc || std::string_view(argv[index + 1]).empty()) {
                throw std::invalid_argument("--ooc-base requires a nonempty path");
            }
            options.ooc_base = std::string(argv[++index]);
            ooc_base_seen = true;
            continue;
        }
        throw std::invalid_argument("unknown command-line argument");
    }
    return options;
}

[[nodiscard]] std::string unique_raw_base() {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return gnfs::util::temp_path("gnfs_ooc_50d_probe_" + std::to_string(gnfs::util::process_id()) +
                                 "_" + std::to_string(ticks));
}

void observe_raw_pair(const std::string& raw_base, CallbackEvidence& evidence) noexcept {
    const ArtifactPairState state = inspect_artifact_pair(raw_base);
    evidence.artifact_inspection_failed =
        evidence.artifact_inspection_failed || state.inspection_failed;
    if (state.complete()) {
        evidence.raw_pair_observed = true;
    } else if (!state.absent()) {
        evidence.raw_pair_incoherent = true;
    }
}

void observe_progress(const ProgressInfo& info, const std::string& raw_base,
                      CallbackEvidence& evidence) noexcept {
    if (info.phase == Phase::Sieving) {
        observe_raw_pair(raw_base, evidence);
    }
    if (info.phase == Phase::LinearAlgebra) {
        evidence.sge_attempted =
            evidence.sge_attempted || info.message.find("SGE") != std::string::npos;
        evidence.block_lanczos_attempted = evidence.block_lanczos_attempted ||
                                           info.message.find("Block Lanczos") != std::string::npos;
        evidence.block_wiedemann_attempted =
            evidence.block_wiedemann_attempted ||
            info.message.find("Block Wiedemann") != std::string::npos;
    }
    evidence.square_root_attempted =
        evidence.square_root_attempted || info.phase == Phase::SquareRoot;
    evidence.factor_extraction_attempted =
        evidence.factor_extraction_attempted || info.phase == Phase::FactorExtraction;
    evidence.full_pipeline_done_observed =
        evidence.full_pipeline_done_observed || info.phase == Phase::Done;
}

void observe_log(const LogEntry& entry, CallbackEvidence& evidence) {
    if (entry.message.starts_with("structured_filter ")) {
        ++evidence.structured_filter_records;
        evidence.structured_filter_record = entry.message;
        if (entry.message.find(" route=direct_ooc_prefix ") != std::string::npos) {
            ++evidence.direct_ooc_filter_records;
        }
    }
    if (entry.message.starts_with("structured_filter_matrix ")) {
        ++evidence.structured_matrix_records;
        evidence.structured_matrix_record = entry.message;
    }
    evidence.sge_attempted =
        evidence.sge_attempted || entry.message.find("SGE preprocessing") != std::string::npos;
    evidence.block_lanczos_attempted = evidence.block_lanczos_attempted ||
                                       entry.message.find("Block Lanczos") != std::string::npos;
    evidence.block_wiedemann_attempted = evidence.block_wiedemann_attempted ||
                                         entry.message.find("Block Wiedemann") != std::string::npos;
    evidence.square_root_attempted =
        evidence.square_root_attempted || entry.phase == Phase::SquareRoot;
    evidence.factor_extraction_attempted =
        evidence.factor_extraction_attempted || entry.phase == Phase::FactorExtraction;
    evidence.full_pipeline_done_observed =
        evidence.full_pipeline_done_observed || entry.phase == Phase::Done;
}

void validate_probe_parameters(const Integer& n, const Pipeline& pipeline, size_t max_special_q,
                               uint32_t max_special_q_batch_workers,
                               std::optional<uint32_t> max_local_sieve_threads) {
    const auto& params = pipeline.params();
    require(n.bit_length() == PROBE_BITS, "50-digit fixture no longer has 164 bits");
    require(PROBE_N.size() == PROBE_DIGITS, "50-digit fixture literal length changed");
    require(params.bits == PROBE_BITS, "Pipeline parameter bit count changed");
    require(params.digits == PROBE_DIGITS, "Pipeline parameter digit count changed");
    require(params.degree == 3, "50-digit polynomial degree changed");
    require(params.rational_bound == 80'000, "50-digit rational bound changed");
    require(params.algebraic_bound == 160'000, "50-digit algebraic bound changed");
    require(params.large_prime_bits == 23, "50-digit large-prime bit bound changed");
    require(params.large_prime_bound == (uint64_t{1} << 23), "50-digit large-prime bound changed");
    require(params.sieve_i_min == -2'048 && params.sieve_i_max == 2'047,
            "50-digit sieve width changed");
    require(params.sieve_j_min == 1 && params.sieve_j_max == 2'048,
            "50-digit sieve height changed");
    require(params.max_special_q == max_special_q, "Config max_special_q was not applied");
    require(params.max_special_q_batch_workers == max_special_q_batch_workers,
            "Config max_special_q_batch_workers was not applied");
    size_t hardware_threads = std::thread::hardware_concurrency();
    if (hardware_threads == 0) {
        hardware_threads = 4;
    }
    const size_t expected_thread_budget =
        max_local_sieve_threads.has_value()
            ? std::min<size_t>(*max_local_sieve_threads, hardware_threads)
            : hardware_threads;
    require(params.max_local_sieve_threads == expected_thread_budget,
            "Config max_local_sieve_threads was not frozen to the effective budget");
}

void validate_callback_boundary(const CallbackEvidence& evidence) {
    require(!evidence.artifact_inspection_failed,
            "filesystem inspection failed inside a pipeline callback");
    require(!evidence.raw_pair_incoherent,
            "raw OOC artifact pair was only partially visible during a callback");
    require(evidence.raw_pair_observed, "raw OOC pair was never observed during sieving");
    require(!evidence.sge_attempted, "matrix-build-only probe crossed into SGE preprocessing");
    require(!evidence.block_lanczos_attempted, "Block Lanczos was attempted");
    require(!evidence.block_wiedemann_attempted, "Block Wiedemann was attempted");
    require(!evidence.square_root_attempted, "square-root phase was attempted");
    require(!evidence.factor_extraction_attempted, "factor-extraction phase was attempted");
    require(!evidence.full_pipeline_done_observed, "full Pipeline completion was observed");
}

void run_probe(const CliOptions& options, ExperimentRecord& record) {
    record.failure_stage = "preflight";
    record.route = std::string(probe_strategy_name(options.strategy));
    record.max_special_q = options.max_special_q;
    record.max_special_q_batch_workers = options.max_special_q_batch_workers;
    record.max_local_sieve_threads_requested = options.max_local_sieve_threads.value_or(0);

    const std::string requested_raw_base =
        options.ooc_base.has_value() ? *options.ooc_base : unique_raw_base();
    const std::string raw_base =
        gnfs::relation::relation_corpus_detail::freeze_ooc_path(requested_raw_base);
    require(inspect_artifact_pair(raw_base).absent(), "fresh raw OOC base already exists");
    require(!path_exists(raw_base + ".gnfs-collector-lease"),
            "fresh raw OOC collector lease already exists");

    ScopedEnvironmentVariable structured_filter(
        "GNFS_STRUCTURED_FILTER", options.strategy == ProbeStrategy::Structured ? "1" : "0");
    ScopedEnvironmentVariable ordinary_ooc("GNFS_OOC_RELATIONS", "1");
    ScopedEnvironmentVariable ooc_base("GNFS_OOC_BASE_PATH", raw_base);
    ScopedEnvironmentVariable resume("GNFS_RESUME", std::nullopt);
    ScopedEnvironmentVariable sieve_resume("GNFS_SIEVE_RESUME", std::nullopt);
    ScopedEnvironmentVariable distributed_workers("GNFS_DISTRIBUTED_SIEVE_WORKERS", "0");
    ScopedEnvironmentVariable distributed_force_small("GNFS_DISTRIBUTED_SIEVE_FORCE_SMALL", "0");
    ScopedEnvironmentVariable adaptive_lattice("GNFS_ADAPTIVE_LATTICE", "0");
    ScopedEnvironmentVariable adaptive_threshold("GNFS_ADAPTIVE_LATTICE_THRESHOLD", std::nullopt);
    ScopedEnvironmentVariable adaptive_retries("GNFS_ADAPTIVE_LATTICE_MAX_RETRIES", std::nullopt);
    ScopedEnvironmentVariable adaptive_seed("GNFS_ADAPTIVE_LATTICE_SEED", std::nullopt);
    ScopedEnvironmentVariable three_large_primes("GNFS_3LP", "0");
    ScopedEnvironmentVariable cascade_v3("GNFS_CASCADE_V3", "0");
    ScopedEnvironmentVariable v0_bfs("GNFS_V0_BFS", "0");
    ScopedEnvironmentVariable override_lp_bits("GNFS_OVERRIDE_LP_BITS", std::nullopt);
    ScopedEnvironmentVariable target_multiplier("GNFS_SIEVE_TARGET_MULT", std::nullopt);
    ScopedEnvironmentVariable lattice_lll("GNFS_LATTICE_LLL", std::nullopt);
    ScopedEnvironmentVariable lattice_skew("GNFS_LATTICE_SKEW", "0");
    ScopedEnvironmentVariable legacy_streaming_matrix("GNFS_SGE_STREAMING", "off");
    ScopedEnvironmentVariable cofactor_brent("GNFS_COFACTOR_BRENT", "0");
    ScopedEnvironmentVariable ecm_brent_suyama("GNFS_ECM_BRENT_SUYAMA", "0");
    ScopedEnvironmentVariable ecm_bs_degree("GNFS_ECM_BS_DEGREE", std::nullopt);
    ScopedEnvironmentVariable ecm_stage1_threads("GNFS_ECM_STAGE1_PARALLEL_THREADS", "1");
    ScopedEnvironmentVariable ecm_stage2_threads("GNFS_ECM_STAGE2_PARALLEL", "1");
    ScopedEnvironmentVariable rho_threads("GNFS_BRENT_POLLARD_RHO_THREADS", "1");
    ScopedEnvironmentVariable ecm_curve_pool("GNFS_ECM_CURVE_POOL", "0");
    ScopedEnvironmentVariable cofactor_batch_size("GNFS_COFACTOR_BATCH_SIZE", "1");
    ScopedEnvironmentVariable ecm_sigma_pool("GNFS_ECM_SIGMA_POOL_SIZE", "0");
    ScopedEnvironmentVariable ecm_b1_cache("GNFS_ECM_B1_CACHE_SIZE", "0");
    ScopedEnvironmentVariable cofactor_result_cache("GNFS_COFACTOR_RESULT_CACHE_SIZE", "0");
    ScopedEnvironmentVariable ecm_batch_inverse("GNFS_ECM_BATCH_INV", "0");
    ScopedEnvironmentVariable survival_filter("GNFS_SURVIVAL_FILTER", "0");
    ScopedEnvironmentVariable survival_threshold("GNFS_SURVIVAL_THRESHOLD", std::nullopt);
    ScopedEnvironmentVariable cofactor_timing("GNFS_COFACTOR_TIMING_ENABLE", "0");
    ScopedEnvironmentVariable trial_division_simd("GNFS_TRIAL_DIV_SIMD", "auto");

    record.start_memory = capture_memory();

    const Integer n{std::string(PROBE_N)};
    Config config;
    config.set_method(FactorizationMethod::GNFS)
        .set_max_special_q(options.max_special_q)
        .set_max_special_q_batch_workers(options.max_special_q_batch_workers)
        .set_verbose(false);
    if (options.max_local_sieve_threads.has_value()) {
        config.set_max_local_sieve_threads(*options.max_local_sieve_threads);
    }
    Pipeline pipeline(n, config);
    validate_probe_parameters(n, pipeline, options.max_special_q,
                              options.max_special_q_batch_workers, options.max_local_sieve_threads);

    CallbackEvidence evidence;
    pipeline.set_progress_callback(
        [&](const ProgressInfo& info) { observe_progress(info, raw_base, evidence); });
    pipeline.set_log_callback([&](const LogEntry& entry) { observe_log(entry, evidence); });

    record.failure_stage = "polynomial";
    auto phase_started = std::chrono::steady_clock::now();
    auto context = pipeline.select_polynomial();
    record.polynomial_ms = elapsed_milliseconds(phase_started, std::chrono::steady_clock::now());
    record.after_polynomial_memory = capture_memory();

    record.failure_stage = "factor_base";
    phase_started = std::chrono::steady_clock::now();
    auto factor_base = pipeline.build_factor_base(context);
    record.factor_base_ms = elapsed_milliseconds(phase_started, std::chrono::steady_clock::now());
    record.after_factor_base_memory = capture_memory();
    record.rational_fb_columns = factor_base.rational_count();
    record.algebraic_fb_columns = factor_base.sieve_algebraic_count();
    record.base_factor_columns =
        checked_add_size(record.rational_fb_columns, record.algebraic_fb_columns);
    const size_t target_columns =
        checked_add_size(record.base_factor_columns, pipeline.params().target_excess);
    record.initial_raw_target = pipeline.params().raw_relation_target(target_columns);

    record.failure_stage = "sieve";
    phase_started = std::chrono::steady_clock::now();
    auto reduction = pipeline.sieve_and_collect(
        context, factor_base,
        SieveCollectionOptions{
            .adaptive_round_limit = 1,
            .legacy_raw_ooc_cleanup = LegacyRawOOCCleanupPolicy::RemoveArtifacts,
        });
    record.sieve_ms = elapsed_milliseconds(phase_started, std::chrono::steady_clock::now());
    record.after_sieve_memory = capture_memory();

    record.failure_stage = "route_validation";
    const auto& reduction_stats = reduction.stats;
    const auto& pipeline_stats = pipeline.stats();
    record.special_q_processed = pipeline_stats.special_q_processed;
    record.special_q_batch_worker_limit = pipeline_stats.special_q_batch_worker_limit;
    record.special_q_batch_peak_workers = pipeline_stats.special_q_batch_peak_workers;
    record.special_q_batch_count = pipeline_stats.special_q_batch_count;
    record.special_q_batch_peak_size = pipeline_stats.special_q_batch_peak_size;
    record.local_sieve_thread_budget = pipeline_stats.local_sieve_thread_budget;
    record.special_q_batch_peak_assigned_threads =
        pipeline_stats.special_q_batch_peak_assigned_threads;
    record.special_q_worker_peak_sieve_threads = pipeline_stats.special_q_worker_peak_sieve_threads;
    record.candidates_total = pipeline_stats.candidates_total;
    record.candidate_batch_peak_workers = pipeline_stats.candidate_batch_peak_workers;
    record.candidate_batch_total_chunks = pipeline_stats.candidate_batch_total_chunks;
    record.candidate_batch_peak_chunks = pipeline_stats.candidate_batch_peak_chunks;
    record.candidate_batch_peak_candidates = pipeline_stats.candidate_batch_peak_candidates;
    record.candidate_batch_rss_sample_candidates =
        pipeline_stats.candidate_batch_rss_sample_candidates;
    record.candidate_batch_after_generation_current_rss_bytes =
        pipeline_stats.candidate_batch_after_generation_current_rss_bytes;
    record.candidate_batch_after_cofactor_current_rss_bytes =
        pipeline_stats.candidate_batch_after_cofactor_current_rss_bytes;
    record.candidate_batch_after_release_current_rss_bytes =
        pipeline_stats.candidate_batch_after_release_current_rss_bytes;
    record.candidate_generation_s = pipeline_stats.timings.candidate_generation_s;
    record.candidate_cofactor_s = pipeline_stats.timings.candidate_cofactor_s;
    record.sieve_rounds_completed = pipeline_stats.sieve_rounds_completed;
    record.sieve_stop_reason =
        std::string(gnfs::api::sieve_stop_reason_name(pipeline_stats.sieve_stop_reason));
    const bool raw_target_reached = reduction_stats.input_relations >= record.initial_raw_target;
    const bool complete_stop_reason =
        pipeline_stats.sieve_stop_reason == SieveStopReason::AdaptiveRoundLimitReached ||
        pipeline_stats.sieve_stop_reason == SieveStopReason::EffectiveColumnExcess;
    const bool short_prefix_stop_reason =
        pipeline_stats.sieve_stop_reason == SieveStopReason::SpecialQBudgetReached ||
        pipeline_stats.sieve_stop_reason == SieveStopReason::SpecialQRangeExhausted;
    record.first_round_complete =
        raw_target_reached && pipeline_stats.sieve_rounds_completed == 1 && complete_stop_reason;
    record.generation = reduction.generation;
    record.raw_rows = reduction_stats.input_relations;
    record.raw_duplicates = reduction_stats.raw_duplicates_removed;
    record.input_lp_columns = reduction_stats.deduplicated_input_lp_histogram.unique_keys;
    record.input_lp_w1 = reduction_stats.deduplicated_input_lp_histogram.weight_1;
    record.input_lp_w2 = reduction_stats.deduplicated_input_lp_histogram.weight_2;
    record.input_lp_w3 = reduction_stats.deduplicated_input_lp_histogram.weight_3;
    record.input_lp_w4plus = reduction_stats.deduplicated_input_lp_histogram.weight_4plus;
    const size_t input_lp_weight_buckets =
        checked_add_size(checked_add_size(record.input_lp_w1, record.input_lp_w2),
                         checked_add_size(record.input_lp_w3, record.input_lp_w4plus));
    record.output_rows = reduction_stats.output_relations;
    record.output_lp_columns = reduction_stats.output_lp_columns;
    record.structured_commits = reduction_stats.structured_run.commits;
    record.structured_emitted_rows = reduction_stats.structured_run.emitted_rows;
    record.structured_stop =
        std::string(stop_reason_name(reduction_stats.structured_run.stop_reason));
    record.incidence_shards = reduction_stats.structured_incidence.shard_count;
    record.incidence_requested_workers =
        reduction_stats.structured_incidence.requested_worker_count;
    record.incidence_peak_workers = reduction_stats.structured_incidence.peak_worker_count;
    record.raw_digest = reduction_stats.raw_input_digest;
    record.output_digest = reduction_stats.output_digest;
    record.strategy = std::string(strategy_name(reduction_stats.strategy));
    record.storage = std::string(storage_name(reduction.storage_kind()));
    record.structured_filter_records = evidence.structured_filter_records;
    record.raw_pair_observed = evidence.raw_pair_observed;
    record.sge_attempted = evidence.sge_attempted;
    record.solver_attempted =
        evidence.block_lanczos_attempted || evidence.block_wiedemann_attempted;
    record.sqrt_attempted = evidence.square_root_attempted;
    record.factorization_attempted = evidence.factor_extraction_attempted;

    require(reduction.generation != 0, "reduction generation is zero");
    require(reduction_stats.raw_duplicates_removed == 0,
            "collector unique-prefix route unexpectedly removed raw duplicates");
    require(reduction_stats.input_relations == pipeline_stats.relations_found,
            "raw relation count differs across Pipeline and reduction stats");
    require(reduction_stats.output_relations == reduction.size(),
            "reduction output count differs from corpus size");
    require(input_lp_weight_buckets == record.input_lp_columns,
            "input LP weight histogram differs from its unique-key count");
    require(pipeline_stats.sieve_rounds_completed == 1,
            "typed first-round limit did not produce exactly one reduction round");
    require(pipeline_stats.sieve_stop_reason != SieveStopReason::NotStarted,
            "typed sieve stop reason was not published");
    require(record.first_round_complete || short_prefix_stop_reason,
            "incomplete first round did not stop at the special-Q budget or range boundary");
    if (record.first_round_complete) {
        require(raw_target_reached && pipeline_stats.sieve_rounds_completed == 1 &&
                    complete_stop_reason,
                "first-round completion lacks target, round, or stop-reason evidence");
    }
    if (options.strategy == ProbeStrategy::Structured) {
        require(reduction_stats.strategy == ReductionStrategy::Structured,
                "production reduction did not select the structured strategy");
        require(reduction.storage_kind() == RelationStorageKind::FinalizedOOC,
                "structured result is not finalized OOC");
        require(reduction_stats.structured_run.stop_reason !=
                    StructuredReductionStopReason::NotStarted,
                "structured reduction did not start");
        require(evidence.structured_filter_records == 1,
                "bounded prefix produced an unexpected number of structured records");
        require(evidence.direct_ooc_filter_records == 1,
                "structured record did not prove the direct OOC route");
        require(evidence.structured_filter_record.find(
                    " generation=" + std::to_string(reduction.generation) + " ") !=
                    std::string::npos,
                "structured record generation differs from the returned reduction");
        require(reduction_stats.structured_incidence.requested_worker_count > 0,
                "structured incidence worker request is zero");
        if (reduction_stats.input_relations > 0) {
            require(reduction_stats.structured_incidence.peak_worker_count > 0,
                    "nonempty structured incidence build used no worker slot");
        }
    } else {
        require(reduction_stats.strategy == ReductionStrategy::StandardV0,
                "production reduction did not select the frozen legacy strategy");
        require(reduction.storage_kind() == RelationStorageKind::InMemory,
                "legacy result is not an in-memory vector corpus");
        require(reduction_stats.structured_run.stop_reason ==
                    StructuredReductionStopReason::NotStarted,
                "legacy reduction unexpectedly entered the structured reducer");
        require(evidence.structured_filter_records == 0 && evidence.direct_ooc_filter_records == 0,
                "legacy reduction emitted structured-route evidence");
        require(reduction_stats.structured_run.commits == 0 &&
                    reduction_stats.structured_run.emitted_rows == 0,
                "legacy reduction reported structured commit activity");
        require(reduction_stats.structured_incidence.shard_count == 0 &&
                    reduction_stats.structured_incidence.requested_worker_count == 0 &&
                    reduction_stats.structured_incidence.peak_worker_count == 0,
                "legacy reduction reported structured incidence activity");
    }
    require(pipeline_stats.special_q_processed > 0, "bounded probe did not process any special-Q");
    require(pipeline_stats.special_q_processed <= options.max_special_q,
            "bounded probe exceeded max_special_q");
    size_t hardware_workers = std::thread::hardware_concurrency();
    if (hardware_workers == 0) {
        hardware_workers = 4;
    }
    const size_t expected_worker_limit =
        std::min({hardware_workers, static_cast<size_t>(options.max_special_q_batch_workers),
                  static_cast<size_t>(pipeline.params().max_local_sieve_threads)});
    require(pipeline_stats.special_q_batch_worker_limit == expected_worker_limit,
            "Pipeline special-Q batch worker limit differs from the frozen effective cap");
    require(pipeline_stats.special_q_batch_count == (pipeline_stats.special_q_processed + 3) / 4,
            "Pipeline special-Q batch count differs from the fixed-width schedule");
    require(pipeline_stats.special_q_batch_peak_size ==
                std::min<size_t>(4, pipeline_stats.special_q_processed),
            "Pipeline special-Q peak batch size differs from the fixed-width schedule");
    require(pipeline_stats.special_q_batch_peak_workers ==
                std::min(pipeline_stats.special_q_batch_worker_limit,
                         pipeline_stats.special_q_batch_peak_size),
            "Pipeline special-Q peak workers differ from the effective schedule");
    require(pipeline_stats.local_sieve_thread_budget == pipeline.params().max_local_sieve_threads,
            "Pipeline local sieve thread budget differs from frozen params");
    require(pipeline_stats.special_q_batch_peak_assigned_threads ==
                pipeline_stats.local_sieve_thread_budget,
            "Pipeline did not assign the complete local sieve thread budget");
    size_t final_batch_size = pipeline_stats.special_q_processed % 4;
    if (final_batch_size == 0) {
        final_batch_size = std::min<size_t>(4, pipeline_stats.special_q_processed);
    }
    const size_t final_batch_workers =
        std::min(pipeline_stats.special_q_batch_worker_limit, final_batch_size);
    const size_t expected_peak_worker_threads =
        (pipeline_stats.local_sieve_thread_budget + final_batch_workers - 1) / final_batch_workers;
    require(pipeline_stats.special_q_worker_peak_sieve_threads == expected_peak_worker_threads,
            "Pipeline per-worker sieve thread assignment differs from the total budget");
    require(pipeline_stats.candidates_total > 0,
            "two-stage candidate batch observed no sieve candidates");
    require(pipeline_stats.candidate_batch_total_chunks > 0,
            "two-stage candidate batch processed no chunks");
    require(pipeline_stats.candidate_batch_peak_chunks > 0 &&
                pipeline_stats.candidate_batch_peak_chunks <=
                    pipeline_stats.candidate_batch_total_chunks,
            "candidate batch peak chunks differ from the processed chunk total");
    require(pipeline_stats.candidate_batch_peak_workers ==
                std::min(pipeline_stats.local_sieve_thread_budget,
                         pipeline_stats.candidate_batch_peak_chunks),
            "candidate batch worker topology differs from the frozen compute budget");
    require(pipeline_stats.candidate_batch_peak_candidates > 0 &&
                pipeline_stats.candidate_batch_peak_candidates <= pipeline_stats.candidates_total,
            "candidate batch peak candidates differ from the total candidate corpus");
    require(pipeline_stats.candidate_batch_rss_sample_candidates ==
                pipeline_stats.candidate_batch_peak_candidates,
            "candidate RSS sample does not identify the first maximum candidate batch");
    const bool candidate_after_generation_present =
        pipeline_stats.candidate_batch_after_generation_current_rss_bytes.has_value();
    const bool candidate_after_cofactor_present =
        pipeline_stats.candidate_batch_after_cofactor_current_rss_bytes.has_value();
    const bool candidate_after_release_present =
        pipeline_stats.candidate_batch_after_release_current_rss_bytes.has_value();
    require(candidate_after_generation_present == candidate_after_cofactor_present &&
                candidate_after_generation_present == candidate_after_release_present,
            "candidate RSS boundary sample is only partially populated");
    require(candidate_after_generation_present ==
                record.after_sieve_memory.current_rss_bytes.has_value(),
            "candidate RSS support differs from process current RSS support");
    require(pipeline_stats.timings.candidate_generation_s > 0.0 &&
                pipeline_stats.timings.candidate_cofactor_s > 0.0,
            "two-stage candidate batch timings were not recorded");
    validate_callback_boundary(evidence);

    const ArtifactPairState raw_after_sieve = inspect_artifact_pair(raw_base);
    require(raw_after_sieve.absent(), "raw OOC pair survived successful terminal handoff");
    require(!path_exists(raw_base + ".gnfs-collector-lease"),
            "raw OOC collector lease survived successful terminal handoff");
    record.raw_pair_removed = true;

    const std::optional<OOCCorpusArtifactScope> output_scope =
        reduction.relation_corpus().ooc_artifact_scope();
    std::optional<std::string> output_base;
    std::optional<std::string> output_cleanup_directory;
    if (options.strategy == ProbeStrategy::Structured) {
        require(output_scope.has_value(), "finalized OOC result has no artifact scope");
        require(output_scope->descriptor.format_version ==
                    gnfs::relation::OOCRelationWriter::FORMAT_VERSION_V3,
                "structured output is not paired OOC V3");
        require(output_scope->descriptor.store_id != 0, "structured output store identity is zero");
        require(output_scope->descriptor.count == static_cast<uint64_t>(reduction.size()),
                "structured output descriptor count differs from reduction size");
        require(!output_scope->cleanup_directory.empty(),
                "structured output has no exclusive cleanup lease");
        require(output_scope->base_path.find(raw_base + ".gnfs-structured-run-") == 0,
                "structured output is outside the production run namespace");
        require(inspect_artifact_pair(output_scope->base_path).complete(),
                "structured output artifact pair is not live");
        require(path_is_directory(output_scope->cleanup_directory),
                "structured output cleanup lease is not live");
        output_base = output_scope->base_path;
        output_cleanup_directory = output_scope->cleanup_directory;
        record.output_pair_observed = true;
        record.route_evidence = "production_direct_ooc";
    } else {
        require(!output_scope.has_value(),
                "legacy reduction unexpectedly returned an OOC output corpus");
        record.route_evidence = "production_legacy_ooc";
    }

    const size_t reduction_output_count = reduction.size();

    record.failure_stage = "matrix";
    phase_started = std::chrono::steady_clock::now();
    std::optional<Pipeline::MatrixResult> matrix_result;
    matrix_result.emplace(pipeline.build_matrix(std::move(reduction), factor_base, context));
    record.matrix_ms = elapsed_milliseconds(phase_started, std::chrono::steady_clock::now());
    record.after_matrix_memory = capture_memory();
    record.matrix_rows = matrix_result->matrix.num_rows();
    record.matrix_cols = matrix_result->matrix.num_cols();
    record.matrix_nonzeros = matrix_result->matrix.total_weight();
    record.matrix_signed_delta = signed_size_delta(record.matrix_rows, record.matrix_cols);
    record.structured_matrix_records = evidence.structured_matrix_records;
    record.sge_attempted = evidence.sge_attempted;
    record.solver_attempted =
        evidence.block_lanczos_attempted || evidence.block_wiedemann_attempted;
    record.sqrt_attempted = evidence.square_root_attempted;
    record.factorization_attempted = evidence.factor_extraction_attempted;

    require(matrix_result->dependencies.empty(),
            "matrix-build-only API unexpectedly returned dependencies");
    require(pipeline.stats().dependencies_found == 0,
            "matrix-build-only API reported solver dependencies");
    require(matrix_result->relation_count() == record.matrix_rows,
            "matrix result relation count differs from the final matrix row count");
    require(record.matrix_rows <= reduction_output_count,
            "matrix builder increased the reduction output row count");
    if (options.strategy == ProbeStrategy::Structured) {
        require(matrix_result->owns_relation_corpus(),
                "matrix result did not retain the structured relation corpus");
        require(matrix_result->relations.empty(),
                "structured matrix result unexpectedly materialized the legacy relation vector");
        require(matrix_result->structured_row_to_relation().size() == record.matrix_rows,
                "structured row mapping differs from matrix row count");
        std::vector<bool> observed_source_ordinals(reduction_output_count, false);
        record.matrix_row_mapping_identity = true;
        for (size_t row = 0; row < matrix_result->structured_row_to_relation().size(); ++row) {
            const size_t source_ordinal = matrix_result->structured_row_to_relation()[row];
            require(source_ordinal < reduction_output_count,
                    "structured matrix row mapping exceeds the source corpus");
            require(!observed_source_ordinals[source_ordinal],
                    "structured matrix row mapping contains a duplicate source ordinal");
            observed_source_ordinals[source_ordinal] = true;
            if (source_ordinal != row) {
                record.matrix_row_mapping_identity = false;
            }
        }
    } else {
        require(!matrix_result->owns_relation_corpus(),
                "legacy matrix result unexpectedly retained a structured corpus");
        require(matrix_result->structured_row_to_relation().empty(),
                "legacy matrix result unexpectedly exposed a structured row mapping");
        require(matrix_result->relations.size() == record.matrix_rows,
                "legacy matrix result did not retain the final in-memory relation vector");
        record.matrix_row_mapping_identity = true;
    }
    require(record.matrix_cols >= record.base_factor_columns,
            "matrix has fewer columns than its factor-base mapping");
    require(pipeline.stats().matrix_rows == record.matrix_rows &&
                pipeline.stats().matrix_cols == record.matrix_cols &&
                pipeline.stats().matrix_excess == *record.matrix_signed_delta,
            "Pipeline matrix stats differ from the returned matrix");
    if (options.strategy == ProbeStrategy::Structured) {
        require(evidence.structured_matrix_records == 1,
                "matrix build emitted an unexpected number of structured records");
        require(evidence.structured_matrix_record.find(
                    "generation=" + std::to_string(record.generation) + " ") != std::string::npos,
                "structured matrix record generation differs from the reduction");
        require(evidence.structured_matrix_record.find(
                    " row_column_delta=" + std::to_string(*record.matrix_signed_delta) + " ") !=
                    std::string::npos,
                "structured matrix record lost the signed row-column delta");
        require(evidence.structured_matrix_record.find(" matrix_build_wall_us=") !=
                    std::string::npos,
                "structured matrix record lost MatrixBuilder wall telemetry");
    } else {
        require(evidence.structured_matrix_records == 0,
                "legacy matrix build emitted structured-route evidence");
    }
    validate_callback_boundary(evidence);
    if (options.strategy == ProbeStrategy::Structured) {
        require(output_base.has_value() && output_cleanup_directory.has_value(),
                "structured output ownership paths were not captured");
        require(inspect_artifact_pair(*output_base).complete(),
                "matrix ownership did not retain the output OOC pair");
        require(path_is_directory(*output_cleanup_directory),
                "matrix ownership did not retain the output cleanup lease");
        record.output_pair_retained_by_matrix = true;
    }

    record.failure_stage = "cleanup";
    matrix_result.reset();
    if (options.strategy == ProbeStrategy::Structured) {
        require(inspect_artifact_pair(*output_base).absent(),
                "output OOC pair survived final matrix-owner destruction");
        require(!path_exists(*output_cleanup_directory),
                "output cleanup lease survived final matrix-owner destruction");
        record.output_pair_removed = true;
        record.output_lease_removed = true;
    }
    record.after_cleanup_memory = capture_memory();
    validate_memory_backends(record);

    record.failure_stage = "none";
    record.status = "pass";
}

} // namespace

int main(int argc, char** argv) {
    ExperimentRecord record;
    const auto started = std::chrono::steady_clock::now();
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--emit-contract-fixture") {
            emit_record(contract_fixture_record(), "GNFS_EXPERIMENT_FIXTURE_V2");
            return 0;
        }
        const CliOptions options = parse_cli(argc, argv);
        record.route = std::string(probe_strategy_name(options.strategy));
        record.max_special_q = options.max_special_q;
        record.max_special_q_batch_workers = options.max_special_q_batch_workers;
        record.max_local_sieve_threads_requested = options.max_local_sieve_threads.value_or(0);
        if (options.help) {
            std::cout << "Usage: test_structured_ooc_50d_probe "
                         "[--strategy legacy|structured] [--max-special-q N] "
                         "[--max-special-q-batch-workers W] "
                         "[--max-local-sieve-threads T] [--ooc-base PATH]  "
                         "# N in [1,UINT32_MAX], W in [1,4], T >= 1\n";
            return 0;
        }
        run_probe(options, record);
        record.total_ms = elapsed_milliseconds(started, std::chrono::steady_clock::now());
        emit_record(record);
        return 0;
    } catch (const std::exception& error) {
        record.total_ms = elapsed_milliseconds(started, std::chrono::steady_clock::now());
        record.error = sanitize_token(error.what());
        emit_record(record);
        std::cerr << "test_structured_ooc_50d_probe: " << error.what() << '\n';
        return 1;
    } catch (...) {
        record.total_ms = elapsed_milliseconds(started, std::chrono::steady_clock::now());
        record.error = "unknown_exception";
        emit_record(record);
        std::cerr << "test_structured_ooc_50d_probe: unknown exception\n";
        return 1;
    }
}
