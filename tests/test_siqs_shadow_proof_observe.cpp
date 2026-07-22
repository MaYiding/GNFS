// test_siqs_shadow_proof_observe.cpp - fixed observe telemetry contracts

#include <gnfs/core/integer.hpp>
#include <gnfs/siqs/relation.hpp>
#include <gnfs/siqs/shadow_proof_observe.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using std::int64_t;
using std::size_t;
using std::uint32_t;
using std::uint64_t;

using gnfs::core::Integer;
using gnfs::siqs::emit_siqs_shadow_proof_observe_record;
using gnfs::siqs::make_siqs_shadow_proof_observe_record_from_result;
using gnfs::siqs::make_siqs_shadow_proof_observe_setup_failure;
using gnfs::siqs::observe_siqs_shadow_proof;
using gnfs::siqs::parse_siqs_shadow_proof_observe_mode;
using gnfs::siqs::run_siqs_shadow_proof;
using gnfs::siqs::siqs_shadow_proof_fallback_reason_name;
using gnfs::siqs::siqs_shadow_proof_observe_mode_name;
using gnfs::siqs::siqs_shadow_proof_stage_name;
using gnfs::siqs::siqs_shadow_proof_terminal_status_name;
using gnfs::siqs::SIQSRelation;
using gnfs::siqs::SIQSShadowProofFallbackReason;
using gnfs::siqs::SIQSShadowProofObserveMode;
using gnfs::siqs::SIQSShadowProofObserveRecord;
using gnfs::siqs::SIQSShadowProofObserveSetupFailure;
using gnfs::siqs::SIQSShadowProofOptions;
using gnfs::siqs::SIQSShadowProofResult;
using gnfs::siqs::SIQSShadowProofStage;
using gnfs::siqs::SIQSShadowProofTerminalStatus;
using gnfs::util::ProcessMemoryBackend;
using gnfs::util::ProcessMemorySnapshot;

static_assert(std::is_nothrow_copy_constructible_v<SIQSShadowProofObserveRecord>);
static_assert(std::is_nothrow_move_constructible_v<SIQSShadowProofObserveRecord>);
static_assert(noexcept(emit_siqs_shadow_proof_observe_record(
    static_cast<std::FILE*>(nullptr), std::declval<const SIQSShadowProofObserveRecord&>())));
static_assert(siqs_shadow_proof_terminal_status_name(SIQSShadowProofTerminalStatus::factor_found) ==
              "factor_found");
static_assert(siqs_shadow_proof_stage_name(SIQSShadowProofStage::not_started) == "not_started");
static_assert(siqs_shadow_proof_fallback_reason_name(SIQSShadowProofFallbackReason::none) ==
              "none");

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':' << __LINE__ << '\n';        \
        }                                                                                          \
    } while (false)

[[nodiscard]] SIQSRelation make_sign_only_full(int64_t value) {
    SIQSRelation relation{};
    relation.value = Integer(value);
    relation.exponents = {0};
    return relation;
}

[[nodiscard]] std::vector<SIQSRelation> make_factor_corpus() {
    return {make_sign_only_full(27), make_sign_only_full(1)};
}

[[nodiscard]] std::vector<SIQSRelation> make_no_factor_corpus() {
    return {make_sign_only_full(90), make_sign_only_full(1)};
}

struct NoSplit {
    [[nodiscard]] std::pair<uint64_t, uint64_t> operator()(uint64_t) const noexcept {
        return {0, 0};
    }
};

[[nodiscard]] SIQSShadowProofOptions make_distinct_options() {
    SIQSShadowProofOptions options;
    options.limits.max_raw_relations = 101;
    options.limits.max_raw_payload_bytes = 2'020;
    options.limits.graph.max_edges = 303;
    options.limits.graph.max_cycles = 304;
    options.limits.graph.max_cycle_incidences = 305;
    options.limits.max_row_candidates = 306;
    options.limits.max_pretrim_rows = 307;
    options.limits.minimum_row_excess = 1;
    options.assembly.trim_excess_rows = 9;
    options.assembly.materialization_workers = 1;
    options.matrix.max_dependencies = 8;
    options.matrix.elimination_workers = 1;
    options.matrix.parallel_column_threshold = 123;
    options.matrix.max_dense_matrix_bytes = 9'999;
    options.matrix.max_dense_variable_count = 88;
    return options;
}

[[nodiscard]] SIQSShadowProofResult run_corpus(const std::vector<SIQSRelation>& relations,
                                               const SIQSShadowProofOptions& options) {
    const std::array<uint32_t, 1> factor_base{0};
    NoSplit splitter;
    return run_siqs_shadow_proof(std::span<const SIQSRelation>(relations.data(), relations.size()),
                                 std::span<const uint32_t>(factor_base.data(), factor_base.size()),
                                 Integer(91), Integer(91), 47, splitter, options);
}

[[nodiscard]] ProcessMemorySnapshot memory(ProcessMemoryBackend backend,
                                           std::optional<uint64_t> current,
                                           std::optional<uint64_t> peak) {
    return ProcessMemorySnapshot{backend, current, peak};
}

void test_exact_parser() {
    CHECK(parse_siqs_shadow_proof_observe_mode(nullptr) == SIQSShadowProofObserveMode::off);
    CHECK(parse_siqs_shadow_proof_observe_mode("0") == SIQSShadowProofObserveMode::off);
    CHECK(parse_siqs_shadow_proof_observe_mode("observe") == SIQSShadowProofObserveMode::observe);
    CHECK(siqs_shadow_proof_observe_mode_name(SIQSShadowProofObserveMode::off) == "off");
    CHECK(siqs_shadow_proof_observe_mode_name(SIQSShadowProofObserveMode::observe) == "observe");
    CHECK(siqs_shadow_proof_observe_mode_name(static_cast<SIQSShadowProofObserveMode>(255)) ==
          "unknown");

    constexpr std::array invalid_values{"",         "off",      "1",  "Observe", "OBSERVE",
                                        " observe", "observe ", "0 ", "00",      "\nobserve"};
    for (const char* value : invalid_values) {
        try {
            (void)parse_siqs_shadow_proof_observe_mode(value);
            CHECK(false);
        } catch (const std::invalid_argument& error) {
            CHECK(std::string_view(error.what()) ==
                  "GNFS_SIQS_SHADOW_PROOF must be unset or exactly one of: 0, observe");
        } catch (...) {
            CHECK(false);
        }
    }
}

void test_all_enum_names() {
    constexpr std::array terminals{
        std::pair{SIQSShadowProofTerminalStatus::factor_found, std::string_view("factor_found")},
        std::pair{SIQSShadowProofTerminalStatus::no_factor, std::string_view("no_factor")},
        std::pair{SIQSShadowProofTerminalStatus::bounded_fallback,
                  std::string_view("bounded_fallback")},
        std::pair{SIQSShadowProofTerminalStatus::invalid_input, std::string_view("invalid_input")},
        std::pair{SIQSShadowProofTerminalStatus::stage_failure, std::string_view("stage_failure")},
        std::pair{SIQSShadowProofTerminalStatus::resource_exhausted,
                  std::string_view("resource_exhausted")},
        std::pair{SIQSShadowProofTerminalStatus::exception_failure,
                  std::string_view("exception_failure")},
        std::pair{SIQSShadowProofTerminalStatus::internal_invariant_failure,
                  std::string_view("internal_invariant_failure")},
    };
    for (const auto& [value, name] : terminals) {
        CHECK(siqs_shadow_proof_terminal_status_name(value) == name);
    }
    CHECK(siqs_shadow_proof_terminal_status_name(static_cast<SIQSShadowProofTerminalStatus>(255)) ==
          "unknown");

    constexpr std::array stages{
        std::pair{SIQSShadowProofStage::not_started, std::string_view("not_started")},
        std::pair{SIQSShadowProofStage::input_validation, std::string_view("input_validation")},
        std::pair{SIQSShadowProofStage::payload_accounting, std::string_view("payload_accounting")},
        std::pair{SIQSShadowProofStage::adapter_preflight, std::string_view("adapter_preflight")},
        std::pair{SIQSShadowProofStage::graph_preflight, std::string_view("graph_preflight")},
        std::pair{SIQSShadowProofStage::assembly, std::string_view("assembly")},
        std::pair{SIQSShadowProofStage::matrix, std::string_view("matrix")},
        std::pair{SIQSShadowProofStage::dependency_verification,
                  std::string_view("dependency_verification")},
        std::pair{SIQSShadowProofStage::factor_extraction, std::string_view("factor_extraction")},
        std::pair{SIQSShadowProofStage::complete, std::string_view("complete")},
    };
    for (const auto& [value, name] : stages) {
        CHECK(siqs_shadow_proof_stage_name(value) == name);
    }
    CHECK(siqs_shadow_proof_stage_name(static_cast<SIQSShadowProofStage>(255)) == "unknown");

    constexpr std::array fallbacks{
        std::pair{SIQSShadowProofFallbackReason::none, std::string_view("none")},
        std::pair{SIQSShadowProofFallbackReason::raw_relation_limit,
                  std::string_view("raw_relation_limit")},
        std::pair{SIQSShadowProofFallbackReason::raw_payload_limit,
                  std::string_view("raw_payload_limit")},
        std::pair{SIQSShadowProofFallbackReason::graph_edge_limit,
                  std::string_view("graph_edge_limit")},
        std::pair{SIQSShadowProofFallbackReason::graph_cycle_limit,
                  std::string_view("graph_cycle_limit")},
        std::pair{SIQSShadowProofFallbackReason::graph_incidence_limit,
                  std::string_view("graph_incidence_limit")},
        std::pair{SIQSShadowProofFallbackReason::row_candidate_limit,
                  std::string_view("row_candidate_limit")},
        std::pair{SIQSShadowProofFallbackReason::pretrim_row_limit,
                  std::string_view("pretrim_row_limit")},
        std::pair{SIQSShadowProofFallbackReason::insufficient_rows,
                  std::string_view("insufficient_rows")},
        std::pair{SIQSShadowProofFallbackReason::matrix_resource_limit,
                  std::string_view("matrix_resource_limit")},
        std::pair{SIQSShadowProofFallbackReason::matrix_backend_unavailable,
                  std::string_view("matrix_backend_unavailable")},
    };
    for (const auto& [value, name] : fallbacks) {
        CHECK(siqs_shadow_proof_fallback_reason_name(value) == name);
    }
    CHECK(siqs_shadow_proof_fallback_reason_name(static_cast<SIQSShadowProofFallbackReason>(255)) ==
          "unknown");
}

void check_profile(const SIQSShadowProofObserveRecord& record,
                   const SIQSShadowProofOptions& options) {
    CHECK(record.raw_relation_cap == options.limits.max_raw_relations);
    CHECK(record.raw_payload_cap_bytes == options.limits.max_raw_payload_bytes);
    CHECK(record.graph_edge_cap == options.limits.graph.max_edges);
    CHECK(record.graph_cycle_cap == options.limits.graph.max_cycles);
    CHECK(record.graph_incidence_cap == options.limits.graph.max_cycle_incidences);
    CHECK(record.row_candidate_cap == options.limits.max_row_candidates);
    CHECK(record.pretrim_row_cap == options.limits.max_pretrim_rows);
    CHECK(record.minimum_row_excess == options.limits.minimum_row_excess);
    CHECK(record.trim_excess_rows == options.assembly.trim_excess_rows);
    CHECK(record.assembly_workers == options.assembly.materialization_workers);
    CHECK(record.matrix_max_dependencies == options.matrix.max_dependencies);
    CHECK(record.matrix_workers == options.matrix.elimination_workers);
    CHECK(record.matrix_parallel_column_threshold == options.matrix.parallel_column_threshold);
    CHECK(record.matrix_dense_bytes_cap == options.matrix.max_dense_matrix_bytes);
    CHECK(record.matrix_dense_variable_cap == options.matrix.max_dense_variable_count);
}

void test_real_factor_and_no_factor_records() {
    const SIQSShadowProofOptions options = make_distinct_options();
    const auto before = memory(ProcessMemoryBackend::LinuxGetrusage, 100, 1'000);
    const auto after = memory(ProcessMemoryBackend::LinuxGetrusage, 200, 1'600);

    const SIQSShadowProofResult factor_result = run_corpus(make_factor_corpus(), options);
    const auto factor =
        make_siqs_shadow_proof_observe_record_from_result(factor_result, before, after);
    CHECK(factor.schema_version == 1);
    CHECK(factor.proof_attempted);
    CHECK(factor.terminal_status == SIQSShadowProofTerminalStatus::factor_found);
    CHECK(factor.stage == SIQSShadowProofStage::factor_extraction);
    CHECK(factor.fallback_reason == SIQSShadowProofFallbackReason::none);
    CHECK(factor.factor_found);
    CHECK(factor.raw_relations == 2);
    CHECK(factor.raw_payload_supported);
    CHECK(factor.raw_payload_bytes > 0);
    CHECK(factor.factor_base_columns == 1);
    CHECK(factor.large_prime_bound == 47);
    check_profile(factor, options);
    CHECK(factor.graph_evidence_supported);
    CHECK(factor.assembly_evidence_supported);
    CHECK(factor.assembly_fingerprint_supported);
    CHECK(factor.matrix_evidence_supported);
    CHECK(factor.projected_dense_bytes_supported);
    CHECK(factor.matrix_rows == 2);
    CHECK(factor.matrix_columns == 1);
    CHECK(factor.minimum_nullity == 1);
    CHECK(factor.dependencies_returned == 2);
    CHECK(factor.dependencies_examined == 2);
    CHECK(factor.dependencies_verified == 2);
    CHECK(factor.no_factor_count == 1);
    CHECK(factor.factor_found_count == 1);
    CHECK(!factor.dependency_cap_reached);
    CHECK(factor.dependency_fingerprint_supported);
    CHECK(!factor.first_failed_dependency_supported);
    CHECK(factor.first_failed_dependency == 0);
    CHECK(factor.winning_dependency_supported);
    CHECK(factor.winning_dependency == 1);
    CHECK(factor.winning_dependency_size_supported);
    CHECK(factor.winning_dependency_size == 1);
    CHECK(factor.peak_growth_supported);
    CHECK(factor.peak_growth_bytes == 600);

    const SIQSShadowProofResult no_factor_result = run_corpus(make_no_factor_corpus(), options);
    const auto no_factor =
        make_siqs_shadow_proof_observe_record_from_result(no_factor_result, before, after);
    CHECK(no_factor.proof_attempted);
    CHECK(no_factor.terminal_status == SIQSShadowProofTerminalStatus::no_factor);
    CHECK(no_factor.stage == SIQSShadowProofStage::complete);
    CHECK(!no_factor.factor_found);
    CHECK(no_factor.dependencies_returned == 2);
    CHECK(no_factor.dependencies_examined == 2);
    CHECK(no_factor.dependencies_verified == 2);
    CHECK(no_factor.no_factor_count == 2);
    CHECK(no_factor.factor_found_count == 0);
    CHECK(no_factor.dependency_fingerprint_supported);
    CHECK(!no_factor.winning_dependency_supported);
    CHECK(no_factor.winning_dependency == 0);
    CHECK(!no_factor.winning_dependency_size_supported);
    CHECK(no_factor.winning_dependency_size == 0);
}

void test_peak_growth_contract() {
    const SIQSShadowProofOptions options = make_distinct_options();
    const SIQSShadowProofResult result = run_corpus(make_no_factor_corpus(), options);

    const auto unsupported = make_siqs_shadow_proof_observe_record_from_result(
        result, memory(ProcessMemoryBackend::Unsupported, 1, 10),
        memory(ProcessMemoryBackend::Unsupported, 2, 20));
    CHECK(!unsupported.peak_growth_supported);
    CHECK(unsupported.peak_growth_bytes == 0);

    const auto mismatch = make_siqs_shadow_proof_observe_record_from_result(
        result, memory(ProcessMemoryBackend::LinuxGetrusage, 1, 10),
        memory(ProcessMemoryBackend::DarwinGetrusage, 2, 20));
    CHECK(!mismatch.peak_growth_supported);
    CHECK(mismatch.peak_growth_bytes == 0);

    const auto missing = make_siqs_shadow_proof_observe_record_from_result(
        result, memory(ProcessMemoryBackend::LinuxGetrusage, 1, std::nullopt),
        memory(ProcessMemoryBackend::LinuxGetrusage, 2, 20));
    CHECK(!missing.peak_growth_supported);
    CHECK(missing.peak_growth_bytes == 0);

    const auto decreasing = make_siqs_shadow_proof_observe_record_from_result(
        result, memory(ProcessMemoryBackend::LinuxGetrusage, 1, 20),
        memory(ProcessMemoryBackend::LinuxGetrusage, 2, 10));
    CHECK(!decreasing.peak_growth_supported);
    CHECK(decreasing.peak_growth_bytes == 0);

    const auto equal = make_siqs_shadow_proof_observe_record_from_result(
        result, memory(ProcessMemoryBackend::LinuxGetrusage, 1, 20),
        memory(ProcessMemoryBackend::LinuxGetrusage, 2, 20));
    CHECK(equal.peak_growth_supported);
    CHECK(equal.peak_growth_bytes == 0);
}

struct SnapshotSequence {
    ProcessMemorySnapshot before;
    ProcessMemorySnapshot after;
    size_t calls = 0;

    [[nodiscard]] ProcessMemorySnapshot operator()() noexcept {
        return calls++ == 0 ? before : after;
    }
};

struct ThrowingSnapshotProvider {
    size_t calls = 0;

    [[nodiscard]] ProcessMemorySnapshot operator()() {
        ++calls;
        throw std::runtime_error("injected snapshot failure");
    }
};

void check_setup_record(const SIQSShadowProofObserveRecord& record,
                        SIQSShadowProofTerminalStatus status,
                        const SIQSShadowProofOptions& options) {
    CHECK(!record.proof_attempted);
    CHECK(record.terminal_status == status);
    CHECK(record.stage == SIQSShadowProofStage::not_started);
    CHECK(record.fallback_reason == SIQSShadowProofFallbackReason::none);
    CHECK(!record.factor_found);
    CHECK(record.raw_relations == 17);
    CHECK(!record.raw_payload_supported);
    CHECK(record.raw_payload_bytes == 0);
    CHECK(record.factor_base_columns == 5);
    CHECK(record.large_prime_bound == 47);
    check_profile(record, options);
    CHECK(!record.graph_evidence_supported);
    CHECK(!record.assembly_evidence_supported);
    CHECK(!record.assembly_fingerprint_supported);
    CHECK(!record.projected_dense_bytes_supported);
    CHECK(!record.matrix_evidence_supported);
    CHECK(record.dependencies_returned == 0);
    CHECK(record.dependencies_examined == 0);
    CHECK(record.dependencies_verified == 0);
    CHECK(!record.dependency_fingerprint_supported);
    CHECK(!record.first_failed_dependency_supported);
    CHECK(!record.winning_dependency_supported);
}

void test_setup_failures_and_no_throw_boundary() {
    const SIQSShadowProofOptions options = make_distinct_options();
    const auto before = memory(ProcessMemoryBackend::LinuxGetrusage, 100, 1'000);
    const auto after = memory(ProcessMemoryBackend::LinuxGetrusage, 200, 1'600);

    const auto direct_resource = make_siqs_shadow_proof_observe_setup_failure(
        SIQSShadowProofObserveSetupFailure::resource_exhausted, 17, 5, 47, options, before, after);
    check_setup_record(direct_resource, SIQSShadowProofTerminalStatus::resource_exhausted, options);
    CHECK(direct_resource.peak_growth_supported);
    CHECK(direct_resource.peak_growth_bytes == 600);

    const auto direct_exception = make_siqs_shadow_proof_observe_setup_failure(
        SIQSShadowProofObserveSetupFailure::exception_failure, 17, 5, 47, options, before, after);
    check_setup_record(direct_exception, SIQSShadowProofTerminalStatus::exception_failure, options);

    const SIQSShadowProofResult factor_result = run_corpus(make_factor_corpus(), options);
    SnapshotSequence success_snapshots{before, after};
    size_t operation_calls = 0;
    const auto success = observe_siqs_shadow_proof(
        2, 1, 47, options,
        [&]() {
            ++operation_calls;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return factor_result;
        },
        success_snapshots);
    CHECK(success.proof_attempted);
    CHECK(success.terminal_status == SIQSShadowProofTerminalStatus::factor_found);
    CHECK(operation_calls == 1);
    CHECK(success_snapshots.calls == 2);
    CHECK(success.observe_wall_ns > 0);

    SnapshotSequence resource_snapshots{before, after};
    const auto resource = observe_siqs_shadow_proof(
        17, 5, 47, options, []() -> SIQSShadowProofResult { throw std::bad_alloc(); },
        resource_snapshots);
    check_setup_record(resource, SIQSShadowProofTerminalStatus::resource_exhausted, options);
    CHECK(resource_snapshots.calls == 2);
    CHECK(resource.peak_growth_supported);

    SnapshotSequence exception_snapshots{before, after};
    const auto exception = observe_siqs_shadow_proof(
        17, 5, 47, options, []() -> SIQSShadowProofResult { throw std::runtime_error("injected"); },
        exception_snapshots);
    check_setup_record(exception, SIQSShadowProofTerminalStatus::exception_failure, options);
    CHECK(exception_snapshots.calls == 2);

    ThrowingSnapshotProvider throwing_success_snapshots;
    operation_calls = 0;
    const auto success_without_rss = observe_siqs_shadow_proof(
        2, 1, 47, options,
        [&]() {
            ++operation_calls;
            return factor_result;
        },
        throwing_success_snapshots);
    CHECK(success_without_rss.proof_attempted);
    CHECK(success_without_rss.terminal_status == SIQSShadowProofTerminalStatus::factor_found);
    CHECK(operation_calls == 1);
    CHECK(throwing_success_snapshots.calls == 2);
    CHECK(success_without_rss.before_memory.backend == ProcessMemoryBackend::Unsupported);
    CHECK(success_without_rss.after_memory.backend == ProcessMemoryBackend::Unsupported);
    CHECK(!success_without_rss.peak_growth_supported);

    ThrowingSnapshotProvider throwing_failure_snapshots;
    const auto failure_without_rss = observe_siqs_shadow_proof(
        17, 5, 47, options,
        []() -> SIQSShadowProofResult { throw std::runtime_error("injected operation failure"); },
        throwing_failure_snapshots);
    check_setup_record(failure_without_rss, SIQSShadowProofTerminalStatus::exception_failure,
                       options);
    CHECK(throwing_failure_snapshots.calls == 2);
    CHECK(failure_without_rss.before_memory.backend == ProcessMemoryBackend::Unsupported);
    CHECK(failure_without_rss.after_memory.backend == ProcessMemoryBackend::Unsupported);
}

[[nodiscard]] std::string emit_to_string(const SIQSShadowProofObserveRecord& record) {
    std::FILE* file = std::tmpfile();
    CHECK(file != nullptr);
    if (file == nullptr) {
        return {};
    }
    CHECK(emit_siqs_shadow_proof_observe_record(file, record));
    CHECK(std::fflush(file) == 0);
    CHECK(std::fseek(file, 0, SEEK_SET) == 0);
    std::array<char, 16'384> buffer{};
    const size_t bytes = std::fread(buffer.data(), 1, buffer.size(), file);
    CHECK(std::ferror(file) == 0);
    CHECK(std::fclose(file) == 0);
    return std::string(buffer.data(), bytes);
}

void test_fixed_emitter_schema() {
    const SIQSShadowProofOptions options = make_distinct_options();
    const SIQSShadowProofResult result = run_corpus(make_factor_corpus(), options);
    auto record = make_siqs_shadow_proof_observe_record_from_result(
        result, memory(ProcessMemoryBackend::LinuxGetrusage, 100, 1'000),
        memory(ProcessMemoryBackend::LinuxGetrusage, 200, 1'600));
    record.observe_wall_ns = 123;

    CHECK(!emit_siqs_shadow_proof_observe_record(nullptr, record));
    const std::string line = emit_to_string(record);
    constexpr std::string_view prefix =
        "GNFS_SIQS_SHADOW_PROOF_OBSERVE_V1 schema_version=1 mode=observe"
        " route=legacy_continue rss_scope=self_lifetime proof_attempted=true"
        " terminal=factor_found stage=factor_extraction fallback=none factor_found=true"
        " observe_wall_ns=123 ";
    CHECK(line.starts_with(prefix));
    CHECK(line.ends_with(" peak_growth_supported=true peak_growth_bytes=600 promotion=false\n"));
    CHECK(std::count(line.begin(), line.end(), '\n') == 1);
    CHECK(line.find(" before_rss_backend=linux_getrusage before_current_rss_supported=true"
                    " before_current_rss_bytes=100 before_peak_rss_supported=true"
                    " before_peak_rss_bytes=1000") != std::string::npos);
    CHECK(line.find(" after_rss_backend=linux_getrusage after_current_rss_supported=true"
                    " after_current_rss_bytes=200 after_peak_rss_supported=true"
                    " after_peak_rss_bytes=1600") != std::string::npos);

    constexpr std::string_view fields[] = {
        "schema_version",
        "mode",
        "route",
        "rss_scope",
        "proof_attempted",
        "terminal",
        "stage",
        "fallback",
        "factor_found",
        "observe_wall_ns",
        "raw_relations",
        "raw_payload_supported",
        "raw_payload_bytes",
        "factor_base_columns",
        "large_prime_bound",
        "raw_relation_cap",
        "raw_payload_cap_bytes",
        "graph_edge_cap",
        "graph_cycle_cap",
        "graph_incidence_cap",
        "row_candidate_cap",
        "pretrim_row_cap",
        "minimum_row_excess",
        "trim_excess_rows",
        "assembly_workers",
        "matrix_max_dependencies",
        "matrix_workers",
        "matrix_parallel_column_threshold",
        "matrix_dense_bytes_cap",
        "matrix_dense_variable_cap",
        "adapter_input_relations",
        "adapter_full_relations",
        "adapter_accepted_one_lp",
        "adapter_accepted_two_lp",
        "adapter_rejected_relations",
        "adapter_malformed_source_shape",
        "adapter_unsupported_encoding",
        "adapter_invalid_one_large_prime",
        "adapter_invalid_two_large_prime_split",
        "adapter_exact_duplicate",
        "graph_evidence_supported",
        "graph_vertices",
        "graph_edges",
        "graph_components",
        "graph_cycles",
        "graph_cycle_incidences",
        "graph_max_cycle_length",
        "row_candidate_upper",
        "assembly_evidence_supported",
        "assembly_pretrim_rows",
        "assembly_selected_rows",
        "assembly_selected_full_rows",
        "assembly_selected_cycle_rows",
        "assembly_trimmed_rows",
        "assembly_fingerprint_supported",
        "assembly_source_fingerprint_low",
        "assembly_source_fingerprint_high",
        "assembly_pretrim_fingerprint_low",
        "assembly_pretrim_fingerprint_high",
        "assembly_selected_fingerprint_low",
        "assembly_selected_fingerprint_high",
        "projected_dense_bytes_supported",
        "projected_dense_bytes",
        "matrix_evidence_supported",
        "matrix_rows",
        "matrix_columns",
        "minimum_nullity",
        "dependencies_returned",
        "dependencies_examined",
        "dependencies_verified",
        "no_factor_count",
        "factor_found_count",
        "dependency_cap_reached",
        "dependency_fingerprint_supported",
        "dependency_fingerprint_low",
        "dependency_fingerprint_high",
        "first_failed_dependency_supported",
        "first_failed_dependency",
        "winning_dependency_supported",
        "winning_dependency",
        "winning_dependency_size_supported",
        "winning_dependency_size",
        "before_rss_backend",
        "before_current_rss_supported",
        "before_current_rss_bytes",
        "before_peak_rss_supported",
        "before_peak_rss_bytes",
        "after_rss_backend",
        "after_current_rss_supported",
        "after_current_rss_bytes",
        "after_peak_rss_supported",
        "after_peak_rss_bytes",
        "peak_growth_supported",
        "peak_growth_bytes",
        "promotion",
    };
    CHECK(static_cast<size_t>(std::count(line.begin(), line.end(), ' ')) == std::size(fields));
    for (const std::string_view field : fields) {
        const std::string needle = " " + std::string(field) + "=";
        const size_t first = line.find(needle);
        CHECK(first != std::string::npos);
        CHECK(first == std::string::npos || line.find(needle, first + 1) == std::string::npos);
    }
}

} // namespace

int main() {
    test_exact_parser();
    test_all_enum_names();
    test_real_factor_and_no_factor_records();
    test_peak_growth_contract();
    test_setup_failures_and_no_throw_boundary();
    test_fixed_emitter_schema();

    std::cout << "SIQS shadow proof observe: " << checks_passed << " passed, " << checks_failed
              << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
