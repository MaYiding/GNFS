// Pure synthetic-byte tests for the production SIQS shadow-proof observe V1
// record codec. This test does not run SIQS or read process memory.

#include <gnfs/siqs/shadow_proof_observe_record_codec.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using std::size_t;
using std::uint64_t;

using gnfs::siqs::emit_siqs_shadow_proof_observe_record;
using gnfs::siqs::parse_siqs_shadow_proof_observe_record;
using gnfs::siqs::SIQS_SHADOW_PROOF_OBSERVE_PREFIX;
using gnfs::siqs::siqs_shadow_proof_observe_record_error_name;
using gnfs::siqs::SIQS_SHADOW_PROOF_OBSERVE_RECORD_FIELD_COUNT;
using gnfs::siqs::SIQS_SHADOW_PROOF_OBSERVE_RECORD_MAX_BYTES;
using gnfs::siqs::SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_ERROR_OFFSET;
using gnfs::siqs::SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_FIELD_INDEX;
using gnfs::siqs::SIQSShadowProofFallbackReason;
using gnfs::siqs::SIQSShadowProofObserveRecord;
using gnfs::siqs::SIQSShadowProofObserveRecordError;
using gnfs::siqs::SIQSShadowProofStage;
using gnfs::siqs::SIQSShadowProofTerminalStatus;
using gnfs::util::ProcessMemoryBackend;

static_assert(noexcept(parse_siqs_shadow_proof_observe_record(std::string_view{})));
static_assert(std::is_nothrow_copy_constructible_v<SIQSShadowProofObserveRecord>);
static_assert(SIQS_SHADOW_PROOF_OBSERVE_RECORD_MAX_BYTES == 16'384);
static_assert(SIQS_SHADOW_PROOF_OBSERVE_RECORD_FIELD_COUNT == 95);

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

[[nodiscard]] SIQSShadowProofObserveRecord make_record() {
    SIQSShadowProofObserveRecord record;
    record.proof_attempted = true;
    record.terminal_status = SIQSShadowProofTerminalStatus::factor_found;
    record.stage = SIQSShadowProofStage::factor_extraction;
    record.fallback_reason = SIQSShadowProofFallbackReason::none;
    record.factor_found = true;
    record.observe_wall_ns = 123;

    record.raw_relations = 101;
    record.raw_payload_supported = true;
    record.raw_payload_bytes = 2'020;
    record.factor_base_columns = 303;
    record.large_prime_bound = 404;

    record.raw_relation_cap = 505;
    record.raw_payload_cap_bytes = 606;
    record.graph_edge_cap = 707;
    record.graph_cycle_cap = 808;
    record.graph_incidence_cap = 909;
    record.row_candidate_cap = 1'010;
    record.pretrim_row_cap = 1'111;
    record.minimum_row_excess = 12;
    record.trim_excess_rows = 13;
    record.assembly_workers = 14;
    record.matrix_max_dependencies = 15;
    record.matrix_workers = 16;
    record.matrix_parallel_column_threshold = 1'717;
    record.matrix_dense_bytes_cap = 1'818;
    record.matrix_dense_variable_cap = 1'919;

    record.adapter_input_relations = 20;
    record.adapter_full_relations = 21;
    record.adapter_accepted_one_lp = 22;
    record.adapter_accepted_two_lp = 23;
    record.adapter_rejected_relations = 24;
    record.adapter_malformed_source_shape = 25;
    record.adapter_unsupported_encoding = 26;
    record.adapter_invalid_one_large_prime = 27;
    record.adapter_invalid_two_large_prime_split = 28;
    record.adapter_exact_duplicate = 29;

    record.graph_evidence_supported = true;
    record.graph_vertices = 30;
    record.graph_edges = 31;
    record.graph_components = 32;
    record.graph_cycles = 33;
    record.graph_cycle_incidences = 34;
    record.graph_max_cycle_length = 35;
    record.row_candidate_upper = 36;

    record.assembly_evidence_supported = true;
    record.assembly_pretrim_rows = 37;
    record.assembly_selected_rows = 38;
    record.assembly_selected_full_rows = 39;
    record.assembly_selected_cycle_rows = 40;
    record.assembly_trimmed_rows = 41;
    record.assembly_fingerprint_supported = true;
    record.assembly_source_fingerprint_low = 42;
    record.assembly_source_fingerprint_high = 43;
    record.assembly_pretrim_fingerprint_low = 44;
    record.assembly_pretrim_fingerprint_high = 45;
    record.assembly_selected_fingerprint_low = 46;
    record.assembly_selected_fingerprint_high = 47;

    record.projected_dense_bytes_supported = true;
    record.projected_dense_bytes = 48;
    record.matrix_evidence_supported = true;
    record.matrix_rows = 49;
    record.matrix_columns = 50;
    record.minimum_nullity = 51;

    record.dependencies_returned = 52;
    record.dependencies_examined = 53;
    record.dependencies_verified = 54;
    record.no_factor_count = 55;
    record.factor_found_count = 56;
    record.dependency_cap_reached = true;
    record.dependency_fingerprint_supported = true;
    record.dependency_fingerprint_low = 57;
    record.dependency_fingerprint_high = 58;
    record.first_failed_dependency_supported = false;
    record.first_failed_dependency = 0;
    record.winning_dependency_supported = true;
    record.winning_dependency = 59;
    record.winning_dependency_size_supported = true;
    record.winning_dependency_size = 60;

    record.before_memory = {ProcessMemoryBackend::LinuxGetrusage, UINT64_C(100), UINT64_C(1'000)};
    record.after_memory = {ProcessMemoryBackend::LinuxGetrusage, UINT64_C(200), UINT64_C(1'600)};
    record.peak_growth_supported = true;
    record.peak_growth_bytes = 600;
    return record;
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
    std::array<char, SIQS_SHADOW_PROOF_OBSERVE_RECORD_MAX_BYTES + 1> bytes{};
    const size_t size = std::fread(bytes.data(), 1, bytes.size(), file);
    CHECK(std::ferror(file) == 0);
    CHECK(std::fclose(file) == 0);
    return {bytes.data(), size};
}

[[nodiscard]] std::string replace_once(std::string input, std::string_view from,
                                       std::string_view to) {
    const size_t position = input.find(from);
    CHECK(position != std::string::npos);
    if (position != std::string::npos) {
        input.replace(position, from.size(), to);
    }
    return input;
}

[[nodiscard]] std::string replace_field_value(std::string input, std::string_view field,
                                              std::string_view replacement) {
    const std::string needle = " " + std::string(field) + "=";
    const size_t field_position = input.find(needle);
    CHECK(field_position != std::string::npos);
    if (field_position == std::string::npos) {
        return input;
    }
    const size_t value_position = field_position + needle.size();
    size_t value_end = input.find(' ', value_position);
    if (value_end == std::string::npos) {
        value_end = input.size() - 1;
    }
    input.replace(value_position, value_end - value_position, replacement);
    return input;
}

[[nodiscard]] std::string remove_field(std::string input, std::string_view field) {
    const std::string needle = " " + std::string(field) + "=";
    const size_t field_position = input.find(needle);
    CHECK(field_position != std::string::npos);
    if (field_position == std::string::npos) {
        return input;
    }
    size_t field_end = input.find(' ', field_position + 1);
    if (field_end == std::string::npos) {
        field_end = input.size() - 1;
    }
    input.erase(field_position, field_end - field_position);
    return input;
}

gnfs::siqs::SIQSShadowProofObserveRecordParseResult
expect_error(std::string_view bytes, SIQSShadowProofObserveRecordError expected) {
    const auto parsed = parse_siqs_shadow_proof_observe_record(bytes);
    CHECK(!parsed);
    CHECK(!parsed.record.has_value());
    CHECK(parsed.diagnostic.error == expected);
    return parsed;
}

void expect_valid(std::string_view bytes) {
    const auto parsed = parse_siqs_shadow_proof_observe_record(bytes);
    CHECK(parsed);
    CHECK(parsed.record.has_value());
    CHECK(parsed.diagnostic.error == SIQSShadowProofObserveRecordError::none);
    CHECK(parsed.diagnostic.field_index == SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_FIELD_INDEX);
    CHECK(parsed.diagnostic.byte_offset == SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_ERROR_OFFSET);
}

void test_error_names_are_closed() {
    constexpr std::array errors{
        std::pair{SIQSShadowProofObserveRecordError::none, std::string_view("none")},
        std::pair{SIQSShadowProofObserveRecordError::record_too_large,
                  std::string_view("record_too_large")},
        std::pair{SIQSShadowProofObserveRecordError::invalid_framing,
                  std::string_view("invalid_framing")},
        std::pair{SIQSShadowProofObserveRecordError::invalid_ascii,
                  std::string_view("invalid_ascii")},
        std::pair{SIQSShadowProofObserveRecordError::invalid_prefix,
                  std::string_view("invalid_prefix")},
        std::pair{SIQSShadowProofObserveRecordError::missing_field,
                  std::string_view("missing_field")},
        std::pair{SIQSShadowProofObserveRecordError::invalid_field,
                  std::string_view("invalid_field")},
        std::pair{SIQSShadowProofObserveRecordError::duplicate_field,
                  std::string_view("duplicate_field")},
        std::pair{SIQSShadowProofObserveRecordError::unknown_field,
                  std::string_view("unknown_field")},
        std::pair{SIQSShadowProofObserveRecordError::field_out_of_order,
                  std::string_view("field_out_of_order")},
        std::pair{SIQSShadowProofObserveRecordError::invalid_schema_version,
                  std::string_view("invalid_schema_version")},
        std::pair{SIQSShadowProofObserveRecordError::invalid_fixed_value,
                  std::string_view("invalid_fixed_value")},
        std::pair{SIQSShadowProofObserveRecordError::invalid_boolean,
                  std::string_view("invalid_boolean")},
        std::pair{SIQSShadowProofObserveRecordError::invalid_unsigned_integer,
                  std::string_view("invalid_unsigned_integer")},
        std::pair{SIQSShadowProofObserveRecordError::unsigned_integer_out_of_range,
                  std::string_view("unsigned_integer_out_of_range")},
        std::pair{SIQSShadowProofObserveRecordError::invalid_terminal_status,
                  std::string_view("invalid_terminal_status")},
        std::pair{SIQSShadowProofObserveRecordError::invalid_stage,
                  std::string_view("invalid_stage")},
        std::pair{SIQSShadowProofObserveRecordError::invalid_fallback_reason,
                  std::string_view("invalid_fallback_reason")},
        std::pair{SIQSShadowProofObserveRecordError::invalid_memory_backend,
                  std::string_view("invalid_memory_backend")},
        std::pair{SIQSShadowProofObserveRecordError::invalid_memory_value,
                  std::string_view("invalid_memory_value")},
        std::pair{SIQSShadowProofObserveRecordError::invalid_peak_growth,
                  std::string_view("invalid_peak_growth")},
    };
    for (const auto& [error, name] : errors) {
        CHECK(siqs_shadow_proof_observe_record_error_name(error) == name);
    }
    CHECK(siqs_shadow_proof_observe_record_error_name(
              static_cast<SIQSShadowProofObserveRecordError>(255)) == "unknown");
}

void test_emitter_roundtrip_is_owning() {
    std::string emitted = emit_to_string(make_record());
    CHECK(emitted.starts_with(SIQS_SHADOW_PROOF_OBSERVE_PREFIX));
    CHECK(emitted.ends_with(" promotion=false\n"));
    CHECK(emitted.size() <= SIQS_SHADOW_PROOF_OBSERVE_RECORD_MAX_BYTES);

    const auto parsed = parse_siqs_shadow_proof_observe_record(emitted);
    CHECK(parsed);
    CHECK(parsed.record.has_value());
    CHECK(parsed.record->schema_version == 1);
    CHECK(parsed.record->terminal_status == SIQSShadowProofTerminalStatus::factor_found);
    CHECK(parsed.record->stage == SIQSShadowProofStage::factor_extraction);
    CHECK(parsed.record->fallback_reason == SIQSShadowProofFallbackReason::none);
    CHECK(parsed.record->before_memory.backend == ProcessMemoryBackend::LinuxGetrusage);
    CHECK(parsed.record->before_memory.current_rss_bytes == UINT64_C(100));
    CHECK(parsed.record->after_memory.lifetime_peak_rss_bytes == UINT64_C(1'600));
    CHECK(parsed.record->peak_growth_supported);
    CHECK(parsed.record->peak_growth_bytes == UINT64_C(600));
    CHECK(emit_to_string(*parsed.record) == emitted);

    emitted.assign("destroyed");
    CHECK(parsed.record->raw_relations == 101);
    CHECK(parsed.record->assembly_source_fingerprint_high == 43);
}

void test_framing_and_ascii_rejections() {
    const std::string canonical = emit_to_string(make_record());
    expect_error({}, SIQSShadowProofObserveRecordError::invalid_framing);
    expect_error("\n", SIQSShadowProofObserveRecordError::invalid_framing);
    expect_error(canonical.substr(0, canonical.size() - 1),
                 SIQSShadowProofObserveRecordError::invalid_framing);
    expect_error(canonical + "\n", SIQSShadowProofObserveRecordError::invalid_framing);
    expect_error(replace_once(canonical, "\n", "\r\n"),
                 SIQSShadowProofObserveRecordError::invalid_ascii);
    expect_error(" " + canonical, SIQSShadowProofObserveRecordError::invalid_framing);
    expect_error(replace_once(canonical, "\n", " \n"),
                 SIQSShadowProofObserveRecordError::invalid_framing);
    expect_error(replace_once(canonical, " schema_version", "  schema_version"),
                 SIQSShadowProofObserveRecordError::invalid_framing);
    expect_error(replace_once(canonical, " mode", std::string_view("\0 mode", 6)),
                 SIQSShadowProofObserveRecordError::invalid_ascii);
    expect_error(replace_once(canonical, " mode", "\tmode"),
                 SIQSShadowProofObserveRecordError::invalid_ascii);
    expect_error(std::string("\xef\xbb\xbf") + canonical,
                 SIQSShadowProofObserveRecordError::invalid_ascii);

    std::string oversized(SIQS_SHADOW_PROOF_OBSERVE_RECORD_MAX_BYTES + 1, 'A');
    oversized.back() = '\n';
    const auto oversized_result =
        expect_error(oversized, SIQSShadowProofObserveRecordError::record_too_large);
    CHECK(oversized_result.diagnostic.byte_offset == SIQS_SHADOW_PROOF_OBSERVE_RECORD_MAX_BYTES);
}

void test_prefix_field_shape_order_and_cardinality() {
    const std::string canonical = emit_to_string(make_record());
    expect_error(replace_once(canonical, SIQS_SHADOW_PROOF_OBSERVE_PREFIX,
                              "GNFS_SIQS_SHADOW_PROOF_OBSERVE_V2"),
                 SIQSShadowProofObserveRecordError::invalid_prefix);
    expect_error(std::string(SIQS_SHADOW_PROOF_OBSERVE_PREFIX) + "\n",
                 SIQSShadowProofObserveRecordError::missing_field);
    expect_error(remove_field(canonical, "promotion"),
                 SIQSShadowProofObserveRecordError::missing_field);
    const std::string missing_middle = remove_field(canonical, "matrix_workers");
    const auto missing_middle_result =
        expect_error(missing_middle, SIQSShadowProofObserveRecordError::missing_field);
    CHECK(missing_middle_result.diagnostic.field_index == 26);
    CHECK(missing_middle_result.diagnostic.byte_offset ==
          missing_middle.find("matrix_parallel_column_threshold="));
    expect_error(replace_once(canonical, " mode=observe", " mode"),
                 SIQSShadowProofObserveRecordError::invalid_field);
    expect_error(replace_once(canonical, " mode=observe", " mode="),
                 SIQSShadowProofObserveRecordError::invalid_field);
    expect_error(replace_once(canonical, " mode=observe", " mode=observe=again"),
                 SIQSShadowProofObserveRecordError::invalid_field);
    expect_error(replace_once(canonical, " proof_attempted=true",
                              " proof_attempted=true proof_attempted=true"),
                 SIQSShadowProofObserveRecordError::duplicate_field);
    expect_error(replace_once(canonical, " proof_attempted=true", " unknown=true"),
                 SIQSShadowProofObserveRecordError::unknown_field);
    expect_error(replace_once(canonical, " mode=observe route=legacy_continue",
                              " route=legacy_continue mode=observe"),
                 SIQSShadowProofObserveRecordError::field_out_of_order);
    expect_error(replace_once(canonical, "\n", " mode=observe\n"),
                 SIQSShadowProofObserveRecordError::duplicate_field);
    expect_error(replace_once(canonical, "\n", " extra=1\n"),
                 SIQSShadowProofObserveRecordError::unknown_field);
}

void test_canonical_scalars_and_enum_closure() {
    const std::string canonical = emit_to_string(make_record());
    const auto wrong_schema =
        expect_error(replace_field_value(canonical, "schema_version", "2"),
                     SIQSShadowProofObserveRecordError::invalid_schema_version);
    CHECK(wrong_schema.diagnostic.field_index == 0);
    CHECK(wrong_schema.diagnostic.byte_offset ==
          canonical.find("schema_version=") + std::string_view("schema_version=").size());

    for (const std::string_view value : {"01", "+1", "-1", "1.0", "one"}) {
        expect_error(replace_field_value(canonical, "observe_wall_ns", value),
                     SIQSShadowProofObserveRecordError::invalid_unsigned_integer);
    }
    expect_error(replace_field_value(canonical, "observe_wall_ns", "18446744073709551616"),
                 SIQSShadowProofObserveRecordError::unsigned_integer_out_of_range);
    expect_error(replace_field_value(canonical, "assembly_workers", "4294967296"),
                 SIQSShadowProofObserveRecordError::unsigned_integer_out_of_range);
    expect_error(replace_field_value(canonical, "proof_attempted", "TRUE"),
                 SIQSShadowProofObserveRecordError::invalid_boolean);
    expect_error(replace_field_value(canonical, "mode", "off"),
                 SIQSShadowProofObserveRecordError::invalid_fixed_value);
    expect_error(replace_field_value(canonical, "route", "shadow_return"),
                 SIQSShadowProofObserveRecordError::invalid_fixed_value);
    expect_error(replace_field_value(canonical, "rss_scope", "current"),
                 SIQSShadowProofObserveRecordError::invalid_fixed_value);
    expect_error(replace_field_value(canonical, "promotion", "true"),
                 SIQSShadowProofObserveRecordError::invalid_fixed_value);
    expect_error(replace_field_value(canonical, "terminal", "unknown"),
                 SIQSShadowProofObserveRecordError::invalid_terminal_status);
    expect_error(replace_field_value(canonical, "stage", "unknown"),
                 SIQSShadowProofObserveRecordError::invalid_stage);
    expect_error(replace_field_value(canonical, "fallback", "unknown"),
                 SIQSShadowProofObserveRecordError::invalid_fallback_reason);

    constexpr std::array terminals{
        "factor_found",  "no_factor",          "bounded_fallback",  "invalid_input",
        "stage_failure", "resource_exhausted", "exception_failure", "internal_invariant_failure",
    };
    for (const std::string_view value : terminals) {
        expect_valid(replace_field_value(canonical, "terminal", value));
    }
    constexpr std::array stages{
        "not_started",
        "input_validation",
        "payload_accounting",
        "adapter_preflight",
        "graph_preflight",
        "assembly",
        "matrix",
        "dependency_verification",
        "factor_extraction",
        "complete",
    };
    for (const std::string_view value : stages) {
        expect_valid(replace_field_value(canonical, "stage", value));
    }
    constexpr std::array fallbacks{
        "none",
        "raw_relation_limit",
        "raw_payload_limit",
        "graph_edge_limit",
        "graph_cycle_limit",
        "graph_incidence_limit",
        "row_candidate_limit",
        "pretrim_row_limit",
        "insufficient_rows",
        "matrix_resource_limit",
        "matrix_backend_unavailable",
    };
    for (const std::string_view value : fallbacks) {
        expect_valid(replace_field_value(canonical, "fallback", value));
    }

    expect_valid(replace_field_value(canonical, "observe_wall_ns", "18446744073709551615"));
    const std::string max_size = std::to_string(std::numeric_limits<size_t>::max());
    expect_valid(replace_field_value(canonical, "raw_relations", max_size));
    if constexpr (sizeof(size_t) < sizeof(uint64_t)) {
        expect_error(replace_field_value(canonical, "raw_relations", "4294967296"),
                     SIQSShadowProofObserveRecordError::unsigned_integer_out_of_range);
    } else {
        expect_valid(replace_field_value(canonical, "raw_relations", "4294967296"));
    }
}

void test_rss_diagnostic_indices_and_offsets() {
    const std::string canonical = emit_to_string(make_record());
    const auto check_rss_error = [&](std::string_view key, std::string_view replacement,
                                     SIQSShadowProofObserveRecordError error,
                                     std::size_t expected_index) {
        const std::string changed = replace_field_value(canonical, key, replacement);
        const auto result = expect_error(changed, error);
        CHECK(result.diagnostic.field_index == expected_index);
        CHECK(result.diagnostic.byte_offset ==
              changed.find(std::string(key) + "=") + key.size() + 1);
    };
    check_rss_error("before_current_rss_bytes", "0",
                    SIQSShadowProofObserveRecordError::invalid_memory_value, 84);
    check_rss_error("before_peak_rss_bytes", "0",
                    SIQSShadowProofObserveRecordError::invalid_memory_value, 86);
    check_rss_error("after_current_rss_bytes", "0",
                    SIQSShadowProofObserveRecordError::invalid_memory_value, 89);
    check_rss_error("after_peak_rss_bytes", "0",
                    SIQSShadowProofObserveRecordError::invalid_memory_value, 91);
    check_rss_error("peak_growth_supported", "false",
                    SIQSShadowProofObserveRecordError::invalid_peak_growth, 92);
    check_rss_error("peak_growth_bytes", "601",
                    SIQSShadowProofObserveRecordError::invalid_peak_growth, 93);
}

void test_memory_backend_support_and_growth_contract() {
    const std::string canonical = emit_to_string(make_record());
    expect_error(replace_field_value(canonical, "before_rss_backend", "linux"),
                 SIQSShadowProofObserveRecordError::invalid_memory_backend);
    expect_error(replace_field_value(canonical, "before_current_rss_supported", "false"),
                 SIQSShadowProofObserveRecordError::invalid_memory_value);
    expect_error(replace_field_value(canonical, "before_current_rss_bytes", "0"),
                 SIQSShadowProofObserveRecordError::invalid_memory_value);
    expect_error(replace_field_value(canonical, "after_peak_rss_supported", "false"),
                 SIQSShadowProofObserveRecordError::invalid_memory_value);

    std::string unsupported_before =
        replace_field_value(canonical, "before_rss_backend", "unsupported");
    expect_error(unsupported_before, SIQSShadowProofObserveRecordError::invalid_memory_value);

    expect_error(replace_field_value(canonical, "peak_growth_supported", "false"),
                 SIQSShadowProofObserveRecordError::invalid_peak_growth);
    expect_error(replace_field_value(canonical, "peak_growth_bytes", "601"),
                 SIQSShadowProofObserveRecordError::invalid_peak_growth);

    auto unsupported = make_record();
    unsupported.before_memory = {};
    unsupported.after_memory = {};
    unsupported.peak_growth_supported = false;
    unsupported.peak_growth_bytes = 0;
    const std::string unsupported_bytes = emit_to_string(unsupported);
    const auto parsed_unsupported = parse_siqs_shadow_proof_observe_record(unsupported_bytes);
    CHECK(parsed_unsupported);
    CHECK(parsed_unsupported.record->before_memory.backend == ProcessMemoryBackend::Unsupported);
    CHECK(!parsed_unsupported.record->before_memory.current_rss_bytes.has_value());
    CHECK(!parsed_unsupported.record->after_memory.lifetime_peak_rss_bytes.has_value());

    auto optional_current = make_record();
    optional_current.before_memory.current_rss_bytes.reset();
    optional_current.after_memory.current_rss_bytes.reset();
    expect_valid(emit_to_string(optional_current));

    auto missing_before_peak = make_record();
    missing_before_peak.before_memory.lifetime_peak_rss_bytes.reset();
    missing_before_peak.peak_growth_supported = false;
    missing_before_peak.peak_growth_bytes = 0;
    expect_valid(emit_to_string(missing_before_peak));

    auto decreasing = make_record();
    decreasing.before_memory.lifetime_peak_rss_bytes = UINT64_C(2'000);
    decreasing.after_memory.lifetime_peak_rss_bytes = UINT64_C(1'600);
    decreasing.peak_growth_supported = false;
    decreasing.peak_growth_bytes = 0;
    expect_valid(emit_to_string(decreasing));

    auto equal_peaks = make_record();
    equal_peaks.before_memory.lifetime_peak_rss_bytes = UINT64_C(1'600);
    equal_peaks.after_memory.lifetime_peak_rss_bytes = UINT64_C(1'600);
    equal_peaks.peak_growth_supported = true;
    equal_peaks.peak_growth_bytes = 0;
    expect_valid(emit_to_string(equal_peaks));

    auto backend_mismatch = make_record();
    backend_mismatch.after_memory.backend = ProcessMemoryBackend::DarwinGetrusage;
    backend_mismatch.peak_growth_supported = false;
    backend_mismatch.peak_growth_bytes = 0;
    expect_valid(emit_to_string(backend_mismatch));

    for (const ProcessMemoryBackend backend :
         {ProcessMemoryBackend::DarwinGetrusage, ProcessMemoryBackend::LinuxGetrusage,
          ProcessMemoryBackend::WindowsPsapi}) {
        auto supported = make_record();
        supported.before_memory.backend = backend;
        supported.after_memory.backend = backend;
        expect_valid(emit_to_string(supported));
    }
}

} // namespace

int main() {
    test_error_names_are_closed();
    test_emitter_roundtrip_is_owning();
    test_framing_and_ascii_rejections();
    test_prefix_field_shape_order_and_cardinality();
    test_canonical_scalars_and_enum_closure();
    test_rss_diagnostic_indices_and_offsets();
    test_memory_backend_support_and_growth_contract();

    std::cout << "SIQS shadow proof observe record codec: " << checks_passed << " passed, "
              << checks_failed << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
