#pragma once

/// @file shadow_proof_observe_record_codec.hpp
/// @brief Strict parser for one production SIQS shadow-proof observe V1 record.

#include <gnfs/siqs/shadow_proof_observe.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>

namespace gnfs::siqs {

inline constexpr std::size_t SIQS_SHADOW_PROOF_OBSERVE_RECORD_MAX_BYTES =
    std::size_t{16} * std::size_t{1024};
inline constexpr std::size_t SIQS_SHADOW_PROOF_OBSERVE_RECORD_FIELD_COUNT = 95;
inline constexpr std::size_t SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_FIELD_INDEX =
    std::numeric_limits<std::size_t>::max();
inline constexpr std::size_t SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_ERROR_OFFSET =
    std::numeric_limits<std::size_t>::max();

enum class SIQSShadowProofObserveRecordError : uint8_t {
    none,
    record_too_large,
    invalid_framing,
    invalid_ascii,
    invalid_prefix,
    missing_field,
    invalid_field,
    duplicate_field,
    unknown_field,
    field_out_of_order,
    invalid_schema_version,
    invalid_fixed_value,
    invalid_boolean,
    invalid_unsigned_integer,
    unsigned_integer_out_of_range,
    invalid_terminal_status,
    invalid_stage,
    invalid_fallback_reason,
    invalid_memory_backend,
    invalid_memory_value,
    invalid_peak_growth,
};

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_observe_record_error_name(SIQSShadowProofObserveRecordError error) noexcept {
    switch (error) {
    case SIQSShadowProofObserveRecordError::none:
        return "none";
    case SIQSShadowProofObserveRecordError::record_too_large:
        return "record_too_large";
    case SIQSShadowProofObserveRecordError::invalid_framing:
        return "invalid_framing";
    case SIQSShadowProofObserveRecordError::invalid_ascii:
        return "invalid_ascii";
    case SIQSShadowProofObserveRecordError::invalid_prefix:
        return "invalid_prefix";
    case SIQSShadowProofObserveRecordError::missing_field:
        return "missing_field";
    case SIQSShadowProofObserveRecordError::invalid_field:
        return "invalid_field";
    case SIQSShadowProofObserveRecordError::duplicate_field:
        return "duplicate_field";
    case SIQSShadowProofObserveRecordError::unknown_field:
        return "unknown_field";
    case SIQSShadowProofObserveRecordError::field_out_of_order:
        return "field_out_of_order";
    case SIQSShadowProofObserveRecordError::invalid_schema_version:
        return "invalid_schema_version";
    case SIQSShadowProofObserveRecordError::invalid_fixed_value:
        return "invalid_fixed_value";
    case SIQSShadowProofObserveRecordError::invalid_boolean:
        return "invalid_boolean";
    case SIQSShadowProofObserveRecordError::invalid_unsigned_integer:
        return "invalid_unsigned_integer";
    case SIQSShadowProofObserveRecordError::unsigned_integer_out_of_range:
        return "unsigned_integer_out_of_range";
    case SIQSShadowProofObserveRecordError::invalid_terminal_status:
        return "invalid_terminal_status";
    case SIQSShadowProofObserveRecordError::invalid_stage:
        return "invalid_stage";
    case SIQSShadowProofObserveRecordError::invalid_fallback_reason:
        return "invalid_fallback_reason";
    case SIQSShadowProofObserveRecordError::invalid_memory_backend:
        return "invalid_memory_backend";
    case SIQSShadowProofObserveRecordError::invalid_memory_value:
        return "invalid_memory_value";
    case SIQSShadowProofObserveRecordError::invalid_peak_growth:
        return "invalid_peak_growth";
    }
    return "unknown";
}

struct SIQSShadowProofObserveRecordDiagnostic final {
    SIQSShadowProofObserveRecordError error = SIQSShadowProofObserveRecordError::none;
    std::size_t field_index = SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_FIELD_INDEX;
    std::size_t byte_offset = SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_ERROR_OFFSET;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofObserveRecordDiagnostic&,
               const SIQSShadowProofObserveRecordDiagnostic&) noexcept = default;
};

/// Successful results own every parsed value. Failed results contain no
/// record, so callers cannot accidentally consume partially decoded evidence.
struct SIQSShadowProofObserveRecordParseResult final {
    std::optional<SIQSShadowProofObserveRecord> record;
    SIQSShadowProofObserveRecordDiagnostic diagnostic;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return record.has_value() && diagnostic.error == SIQSShadowProofObserveRecordError::none;
    }
};

namespace shadow_proof_observe_record_codec_detail {

inline constexpr std::array<std::string_view, SIQS_SHADOW_PROOF_OBSERVE_RECORD_FIELD_COUNT>
    FIELD_NAMES{
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

static_assert(FIELD_NAMES.size() == SIQS_SHADOW_PROOF_OBSERVE_RECORD_FIELD_COUNT);

struct Field final {
    std::string_view value;
    std::size_t value_offset = 0;
};

[[nodiscard]] constexpr std::size_t field_name_index(std::string_view key) noexcept {
    for (std::size_t index = 0; index < FIELD_NAMES.size(); ++index) {
        if (FIELD_NAMES[index] == key) {
            return index;
        }
    }
    return FIELD_NAMES.size();
}

[[nodiscard]] constexpr SIQSShadowProofObserveRecordError
classify_unexpected_field(std::string_view key, std::size_t expected_index) noexcept {
    const std::size_t actual_index = field_name_index(key);
    if (actual_index == FIELD_NAMES.size()) {
        return SIQSShadowProofObserveRecordError::unknown_field;
    }
    if (actual_index < expected_index) {
        return SIQSShadowProofObserveRecordError::duplicate_field;
    }
    return SIQSShadowProofObserveRecordError::field_out_of_order;
}

[[nodiscard]] constexpr bool field_key_appears_at_or_after(std::string_view body,
                                                           std::size_t cursor,
                                                           std::string_view expected_key) noexcept {
    while (cursor < body.size()) {
        const std::size_t token_end = body.find(' ', cursor);
        const std::size_t bounded_end =
            token_end == std::string_view::npos ? body.size() : token_end;
        const std::string_view token = body.substr(cursor, bounded_end - cursor);
        const std::size_t separator = token.find('=');
        if (separator != std::string_view::npos && token.substr(0, separator) == expected_key) {
            return true;
        }
        cursor = bounded_end == body.size() ? body.size() : bounded_end + 1;
    }
    return false;
}

[[nodiscard]] constexpr SIQSShadowProofObserveRecordParseResult
failure(SIQSShadowProofObserveRecordError error,
        std::size_t field_index = SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_FIELD_INDEX,
        std::size_t byte_offset = SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_ERROR_OFFSET) noexcept {
    SIQSShadowProofObserveRecordParseResult result;
    result.diagnostic = {error, field_index, byte_offset};
    return result;
}

[[nodiscard]] constexpr SIQSShadowProofObserveRecordError
parse_canonical_u64(std::string_view value, uint64_t& parsed) noexcept {
    if (value.empty() || (value.size() > 1 && value.front() == '0')) {
        return SIQSShadowProofObserveRecordError::invalid_unsigned_integer;
    }
    for (const char digit : value) {
        if (digit < '0' || digit > '9') {
            return SIQSShadowProofObserveRecordError::invalid_unsigned_integer;
        }
    }
    uint64_t candidate = 0;
    const auto [end, status] =
        std::from_chars(value.data(), value.data() + value.size(), candidate);
    if (status == std::errc::result_out_of_range) {
        return SIQSShadowProofObserveRecordError::unsigned_integer_out_of_range;
    }
    if (status != std::errc{} || end != value.data() + value.size()) {
        return SIQSShadowProofObserveRecordError::invalid_unsigned_integer;
    }
    parsed = candidate;
    return SIQSShadowProofObserveRecordError::none;
}

[[nodiscard]] constexpr bool parse_terminal_status(std::string_view value,
                                                   SIQSShadowProofTerminalStatus& parsed) noexcept {
    if (value == "factor_found") {
        parsed = SIQSShadowProofTerminalStatus::factor_found;
    } else if (value == "no_factor") {
        parsed = SIQSShadowProofTerminalStatus::no_factor;
    } else if (value == "bounded_fallback") {
        parsed = SIQSShadowProofTerminalStatus::bounded_fallback;
    } else if (value == "invalid_input") {
        parsed = SIQSShadowProofTerminalStatus::invalid_input;
    } else if (value == "stage_failure") {
        parsed = SIQSShadowProofTerminalStatus::stage_failure;
    } else if (value == "resource_exhausted") {
        parsed = SIQSShadowProofTerminalStatus::resource_exhausted;
    } else if (value == "exception_failure") {
        parsed = SIQSShadowProofTerminalStatus::exception_failure;
    } else if (value == "internal_invariant_failure") {
        parsed = SIQSShadowProofTerminalStatus::internal_invariant_failure;
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] constexpr bool parse_stage(std::string_view value,
                                         SIQSShadowProofStage& parsed) noexcept {
    if (value == "not_started") {
        parsed = SIQSShadowProofStage::not_started;
    } else if (value == "input_validation") {
        parsed = SIQSShadowProofStage::input_validation;
    } else if (value == "payload_accounting") {
        parsed = SIQSShadowProofStage::payload_accounting;
    } else if (value == "adapter_preflight") {
        parsed = SIQSShadowProofStage::adapter_preflight;
    } else if (value == "graph_preflight") {
        parsed = SIQSShadowProofStage::graph_preflight;
    } else if (value == "assembly") {
        parsed = SIQSShadowProofStage::assembly;
    } else if (value == "matrix") {
        parsed = SIQSShadowProofStage::matrix;
    } else if (value == "dependency_verification") {
        parsed = SIQSShadowProofStage::dependency_verification;
    } else if (value == "factor_extraction") {
        parsed = SIQSShadowProofStage::factor_extraction;
    } else if (value == "complete") {
        parsed = SIQSShadowProofStage::complete;
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] constexpr bool parse_fallback_reason(std::string_view value,
                                                   SIQSShadowProofFallbackReason& parsed) noexcept {
    if (value == "none") {
        parsed = SIQSShadowProofFallbackReason::none;
    } else if (value == "raw_relation_limit") {
        parsed = SIQSShadowProofFallbackReason::raw_relation_limit;
    } else if (value == "raw_payload_limit") {
        parsed = SIQSShadowProofFallbackReason::raw_payload_limit;
    } else if (value == "graph_edge_limit") {
        parsed = SIQSShadowProofFallbackReason::graph_edge_limit;
    } else if (value == "graph_cycle_limit") {
        parsed = SIQSShadowProofFallbackReason::graph_cycle_limit;
    } else if (value == "graph_incidence_limit") {
        parsed = SIQSShadowProofFallbackReason::graph_incidence_limit;
    } else if (value == "row_candidate_limit") {
        parsed = SIQSShadowProofFallbackReason::row_candidate_limit;
    } else if (value == "pretrim_row_limit") {
        parsed = SIQSShadowProofFallbackReason::pretrim_row_limit;
    } else if (value == "insufficient_rows") {
        parsed = SIQSShadowProofFallbackReason::insufficient_rows;
    } else if (value == "matrix_resource_limit") {
        parsed = SIQSShadowProofFallbackReason::matrix_resource_limit;
    } else if (value == "matrix_backend_unavailable") {
        parsed = SIQSShadowProofFallbackReason::matrix_backend_unavailable;
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] constexpr bool parse_memory_backend(std::string_view value,
                                                  util::ProcessMemoryBackend& parsed) noexcept {
    if (value == "unsupported") {
        parsed = util::ProcessMemoryBackend::Unsupported;
    } else if (value == "darwin_getrusage") {
        parsed = util::ProcessMemoryBackend::DarwinGetrusage;
    } else if (value == "linux_getrusage") {
        parsed = util::ProcessMemoryBackend::LinuxGetrusage;
    } else if (value == "windows_psapi") {
        parsed = util::ProcessMemoryBackend::WindowsPsapi;
    } else {
        return false;
    }
    return true;
}

class FieldReader final {
public:
    explicit constexpr FieldReader(const std::array<Field, FIELD_NAMES.size()>& fields) noexcept
        : fields_(fields) {}

    [[nodiscard]] constexpr bool fixed(std::string_view expected) noexcept {
        if (fields_[index_].value != expected) {
            reject(SIQSShadowProofObserveRecordError::invalid_fixed_value);
            return false;
        }
        ++index_;
        return true;
    }

    [[nodiscard]] constexpr bool boolean(bool& parsed) noexcept {
        if (fields_[index_].value == "true") {
            parsed = true;
        } else if (fields_[index_].value == "false") {
            parsed = false;
        } else {
            reject(SIQSShadowProofObserveRecordError::invalid_boolean);
            return false;
        }
        ++index_;
        return true;
    }

    [[nodiscard]] constexpr bool u64(uint64_t& parsed) noexcept {
        const auto error = parse_canonical_u64(fields_[index_].value, parsed);
        if (error != SIQSShadowProofObserveRecordError::none) {
            reject(error);
            return false;
        }
        ++index_;
        return true;
    }

    [[nodiscard]] constexpr bool size(std::size_t& parsed) noexcept {
        uint64_t value = 0;
        const auto error = parse_canonical_u64(fields_[index_].value, value);
        if (error != SIQSShadowProofObserveRecordError::none) {
            reject(error);
            return false;
        }
        if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
            if (value > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
                reject(SIQSShadowProofObserveRecordError::unsigned_integer_out_of_range);
                return false;
            }
        }
        parsed = static_cast<std::size_t>(value);
        ++index_;
        return true;
    }

    [[nodiscard]] constexpr bool u32(uint32_t& parsed) noexcept {
        uint64_t value = 0;
        const auto error = parse_canonical_u64(fields_[index_].value, value);
        if (error != SIQSShadowProofObserveRecordError::none) {
            reject(error);
            return false;
        }
        if (value > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
            reject(SIQSShadowProofObserveRecordError::unsigned_integer_out_of_range);
            return false;
        }
        parsed = static_cast<uint32_t>(value);
        ++index_;
        return true;
    }

    [[nodiscard]] constexpr bool terminal(SIQSShadowProofTerminalStatus& parsed) noexcept {
        if (!parse_terminal_status(fields_[index_].value, parsed)) {
            reject(SIQSShadowProofObserveRecordError::invalid_terminal_status);
            return false;
        }
        ++index_;
        return true;
    }

    [[nodiscard]] constexpr bool stage(SIQSShadowProofStage& parsed) noexcept {
        if (!parse_stage(fields_[index_].value, parsed)) {
            reject(SIQSShadowProofObserveRecordError::invalid_stage);
            return false;
        }
        ++index_;
        return true;
    }

    [[nodiscard]] constexpr bool fallback(SIQSShadowProofFallbackReason& parsed) noexcept {
        if (!parse_fallback_reason(fields_[index_].value, parsed)) {
            reject(SIQSShadowProofObserveRecordError::invalid_fallback_reason);
            return false;
        }
        ++index_;
        return true;
    }

    [[nodiscard]] constexpr bool backend(util::ProcessMemoryBackend& parsed) noexcept {
        if (!parse_memory_backend(fields_[index_].value, parsed)) {
            reject(SIQSShadowProofObserveRecordError::invalid_memory_backend);
            return false;
        }
        ++index_;
        return true;
    }

    [[nodiscard]] constexpr SIQSShadowProofObserveRecordDiagnostic diagnostic() const noexcept {
        return diagnostic_;
    }

private:
    constexpr void reject(SIQSShadowProofObserveRecordError error) noexcept {
        diagnostic_ = {error, index_, fields_[index_].value_offset};
    }

    const std::array<Field, FIELD_NAMES.size()>& fields_;
    std::size_t index_ = 0;
    SIQSShadowProofObserveRecordDiagnostic diagnostic_;
};

[[nodiscard]] constexpr bool memory_value_is_consistent(bool supported, uint64_t bytes) noexcept {
    return supported ? bytes > 0 : bytes == 0;
}

} // namespace shadow_proof_observe_record_codec_detail

/// Parse exactly one bounded, printable-ASCII, LF-terminated observe V1 record.
/// This codec validates representation and RSS support/growth consistency
/// only. Holdout profile, proof, matrix, factor, and policy semantics belong to
/// the campaign stream join.
[[nodiscard]] inline SIQSShadowProofObserveRecordParseResult
parse_siqs_shadow_proof_observe_record(std::string_view bytes) noexcept {
    using namespace shadow_proof_observe_record_codec_detail;
    using Error = SIQSShadowProofObserveRecordError;

    if (bytes.size() > SIQS_SHADOW_PROOF_OBSERVE_RECORD_MAX_BYTES) {
        return failure(Error::record_too_large, SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_FIELD_INDEX,
                       SIQS_SHADOW_PROOF_OBSERVE_RECORD_MAX_BYTES);
    }
    if (bytes.empty()) {
        return failure(Error::invalid_framing);
    }
    if (bytes.back() != '\n') {
        return failure(Error::invalid_framing, SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_FIELD_INDEX,
                       bytes.size());
    }
    for (std::size_t index = 0; index + 1 < bytes.size(); ++index) {
        if (bytes[index] == '\n') {
            return failure(Error::invalid_framing, SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_FIELD_INDEX,
                           index);
        }
    }
    for (std::size_t index = 0; index + 1 < bytes.size(); ++index) {
        const auto byte = static_cast<unsigned char>(bytes[index]);
        if (byte < 0x20U || byte > 0x7eU) {
            return failure(Error::invalid_ascii, SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_FIELD_INDEX,
                           index);
        }
    }

    const std::size_t body_size = bytes.size() - 1;
    const std::string_view body = bytes.substr(0, body_size);
    if (body.empty()) {
        return failure(Error::invalid_framing);
    }
    if (body.front() == ' ') {
        return failure(Error::invalid_framing, SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_FIELD_INDEX, 0);
    }
    if (body.back() == ' ') {
        return failure(Error::invalid_framing, SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_FIELD_INDEX,
                       body.size() - 1);
    }
    if (const std::size_t doubled = body.find("  "); doubled != std::string_view::npos) {
        return failure(Error::invalid_framing, SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_FIELD_INDEX,
                       doubled + 1);
    }

    const std::string_view prefix = SIQS_SHADOW_PROOF_OBSERVE_PREFIX;
    if (!body.starts_with(prefix)) {
        std::size_t mismatch = 0;
        while (mismatch < body.size() && mismatch < prefix.size() &&
               body[mismatch] == prefix[mismatch]) {
            ++mismatch;
        }
        return failure(Error::invalid_prefix, SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_FIELD_INDEX,
                       mismatch);
    }
    if (body.size() == prefix.size()) {
        return failure(Error::missing_field, 0, body.size());
    }
    if (body.size() < prefix.size() + 1 || body[prefix.size()] != ' ') {
        return failure(Error::invalid_prefix, SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_FIELD_INDEX,
                       prefix.size());
    }

    std::array<Field, FIELD_NAMES.size()> fields{};
    std::size_t cursor = prefix.size() + 1;
    for (std::size_t field_index = 0; field_index < FIELD_NAMES.size(); ++field_index) {
        if (cursor >= body.size()) {
            return failure(Error::missing_field, field_index, body.size());
        }
        const std::size_t token_end = body.find(' ', cursor);
        const std::size_t bounded_end =
            token_end == std::string_view::npos ? body.size() : token_end;
        const std::string_view token = body.substr(cursor, bounded_end - cursor);
        const std::size_t separator = token.find('=');
        if (separator == std::string_view::npos || separator == 0 ||
            separator + 1 >= token.size() ||
            token.find('=', separator + 1) != std::string_view::npos) {
            return failure(Error::invalid_field, field_index, cursor);
        }
        const std::string_view key = token.substr(0, separator);
        if (key != FIELD_NAMES[field_index]) {
            Error error = classify_unexpected_field(key, field_index);
            const std::size_t following_cursor =
                bounded_end == body.size() ? body.size() : bounded_end + 1;
            if (error == Error::field_out_of_order &&
                !field_key_appears_at_or_after(body, following_cursor, FIELD_NAMES[field_index])) {
                error = Error::missing_field;
            }
            return failure(error, field_index, cursor);
        }
        fields[field_index] = {
            token.substr(separator + 1),
            cursor + separator + 1,
        };
        cursor = bounded_end == body.size() ? body.size() : bounded_end + 1;
    }
    if (cursor != body.size()) {
        const std::size_t token_end = body.find(' ', cursor);
        const std::size_t bounded_end =
            token_end == std::string_view::npos ? body.size() : token_end;
        const std::string_view token = body.substr(cursor, bounded_end - cursor);
        const std::size_t separator = token.find('=');
        if (separator == std::string_view::npos || separator == 0 ||
            separator + 1 >= token.size() ||
            token.find('=', separator + 1) != std::string_view::npos) {
            return failure(Error::invalid_field, FIELD_NAMES.size(), cursor);
        }
        const std::string_view key = token.substr(0, separator);
        const Error error = field_name_index(key) == FIELD_NAMES.size() ? Error::unknown_field
                                                                        : Error::duplicate_field;
        return failure(error, FIELD_NAMES.size(), cursor);
    }

    SIQSShadowProofObserveRecord record;
    FieldReader reader(fields);
    uint32_t schema_version = 0;
    if (!reader.u32(schema_version)) {
        SIQSShadowProofObserveRecordParseResult result;
        result.diagnostic = reader.diagnostic();
        return result;
    }
    if (schema_version != SIQS_SHADOW_PROOF_OBSERVE_SCHEMA_VERSION) {
        return failure(Error::invalid_schema_version, 0, fields[0].value_offset);
    }
    record.schema_version = schema_version;

    if (!reader.fixed("observe") || !reader.fixed("legacy_continue") ||
        !reader.fixed("self_lifetime") || !reader.boolean(record.proof_attempted) ||
        !reader.terminal(record.terminal_status) || !reader.stage(record.stage) ||
        !reader.fallback(record.fallback_reason) || !reader.boolean(record.factor_found) ||
        !reader.u64(record.observe_wall_ns) || !reader.size(record.raw_relations) ||
        !reader.boolean(record.raw_payload_supported) || !reader.size(record.raw_payload_bytes) ||
        !reader.size(record.factor_base_columns) || !reader.u64(record.large_prime_bound) ||
        !reader.size(record.raw_relation_cap) || !reader.size(record.raw_payload_cap_bytes) ||
        !reader.size(record.graph_edge_cap) || !reader.size(record.graph_cycle_cap) ||
        !reader.size(record.graph_incidence_cap) || !reader.size(record.row_candidate_cap) ||
        !reader.size(record.pretrim_row_cap) || !reader.size(record.minimum_row_excess) ||
        !reader.size(record.trim_excess_rows) || !reader.u32(record.assembly_workers) ||
        !reader.size(record.matrix_max_dependencies) || !reader.u32(record.matrix_workers) ||
        !reader.size(record.matrix_parallel_column_threshold) ||
        !reader.size(record.matrix_dense_bytes_cap) ||
        !reader.size(record.matrix_dense_variable_cap) ||
        !reader.size(record.adapter_input_relations) ||
        !reader.size(record.adapter_full_relations) ||
        !reader.size(record.adapter_accepted_one_lp) ||
        !reader.size(record.adapter_accepted_two_lp) ||
        !reader.size(record.adapter_rejected_relations) ||
        !reader.size(record.adapter_malformed_source_shape) ||
        !reader.size(record.adapter_unsupported_encoding) ||
        !reader.size(record.adapter_invalid_one_large_prime) ||
        !reader.size(record.adapter_invalid_two_large_prime_split) ||
        !reader.size(record.adapter_exact_duplicate) ||
        !reader.boolean(record.graph_evidence_supported) || !reader.size(record.graph_vertices) ||
        !reader.size(record.graph_edges) || !reader.size(record.graph_components) ||
        !reader.size(record.graph_cycles) || !reader.size(record.graph_cycle_incidences) ||
        !reader.size(record.graph_max_cycle_length) || !reader.size(record.row_candidate_upper) ||
        !reader.boolean(record.assembly_evidence_supported) ||
        !reader.size(record.assembly_pretrim_rows) || !reader.size(record.assembly_selected_rows) ||
        !reader.size(record.assembly_selected_full_rows) ||
        !reader.size(record.assembly_selected_cycle_rows) ||
        !reader.size(record.assembly_trimmed_rows) ||
        !reader.boolean(record.assembly_fingerprint_supported) ||
        !reader.u64(record.assembly_source_fingerprint_low) ||
        !reader.u64(record.assembly_source_fingerprint_high) ||
        !reader.u64(record.assembly_pretrim_fingerprint_low) ||
        !reader.u64(record.assembly_pretrim_fingerprint_high) ||
        !reader.u64(record.assembly_selected_fingerprint_low) ||
        !reader.u64(record.assembly_selected_fingerprint_high) ||
        !reader.boolean(record.projected_dense_bytes_supported) ||
        !reader.size(record.projected_dense_bytes) ||
        !reader.boolean(record.matrix_evidence_supported) || !reader.size(record.matrix_rows) ||
        !reader.size(record.matrix_columns) || !reader.size(record.minimum_nullity) ||
        !reader.size(record.dependencies_returned) || !reader.size(record.dependencies_examined) ||
        !reader.size(record.dependencies_verified) || !reader.size(record.no_factor_count) ||
        !reader.size(record.factor_found_count) || !reader.boolean(record.dependency_cap_reached) ||
        !reader.boolean(record.dependency_fingerprint_supported) ||
        !reader.u64(record.dependency_fingerprint_low) ||
        !reader.u64(record.dependency_fingerprint_high) ||
        !reader.boolean(record.first_failed_dependency_supported) ||
        !reader.size(record.first_failed_dependency) ||
        !reader.boolean(record.winning_dependency_supported) ||
        !reader.size(record.winning_dependency) ||
        !reader.boolean(record.winning_dependency_size_supported) ||
        !reader.size(record.winning_dependency_size)) {
        SIQSShadowProofObserveRecordParseResult result;
        result.diagnostic = reader.diagnostic();
        return result;
    }

    util::ProcessMemoryBackend before_backend = util::ProcessMemoryBackend::Unsupported;
    bool before_current_supported = false;
    uint64_t before_current_bytes = 0;
    bool before_peak_supported = false;
    uint64_t before_peak_bytes = 0;
    util::ProcessMemoryBackend after_backend = util::ProcessMemoryBackend::Unsupported;
    bool after_current_supported = false;
    uint64_t after_current_bytes = 0;
    bool after_peak_supported = false;
    uint64_t after_peak_bytes = 0;
    if (!reader.backend(before_backend) || !reader.boolean(before_current_supported) ||
        !reader.u64(before_current_bytes) || !reader.boolean(before_peak_supported) ||
        !reader.u64(before_peak_bytes) || !reader.backend(after_backend) ||
        !reader.boolean(after_current_supported) || !reader.u64(after_current_bytes) ||
        !reader.boolean(after_peak_supported) || !reader.u64(after_peak_bytes) ||
        !reader.boolean(record.peak_growth_supported) || !reader.u64(record.peak_growth_bytes)) {
        SIQSShadowProofObserveRecordParseResult result;
        result.diagnostic = reader.diagnostic();
        return result;
    }
    bool promotion = false;
    if (!reader.boolean(promotion)) {
        SIQSShadowProofObserveRecordParseResult result;
        result.diagnostic = reader.diagnostic();
        return result;
    }
    if (promotion) {
        return failure(Error::invalid_fixed_value, FIELD_NAMES.size() - 1,
                       fields.back().value_offset);
    }

    const auto invalid_memory = [&](std::size_t value_field_index) constexpr {
        return failure(Error::invalid_memory_value, value_field_index,
                       fields[value_field_index].value_offset);
    };
    if (!memory_value_is_consistent(before_current_supported, before_current_bytes)) {
        return invalid_memory(84);
    }
    if (!memory_value_is_consistent(before_peak_supported, before_peak_bytes)) {
        return invalid_memory(86);
    }
    if (before_backend == util::ProcessMemoryBackend::Unsupported &&
        (before_current_supported || before_peak_supported)) {
        return invalid_memory(before_current_supported ? 83 : 85);
    }
    if (!memory_value_is_consistent(after_current_supported, after_current_bytes)) {
        return invalid_memory(89);
    }
    if (!memory_value_is_consistent(after_peak_supported, after_peak_bytes)) {
        return invalid_memory(91);
    }
    if (after_backend == util::ProcessMemoryBackend::Unsupported &&
        (after_current_supported || after_peak_supported)) {
        return invalid_memory(after_current_supported ? 88 : 90);
    }

    const bool comparable_peaks = before_backend != util::ProcessMemoryBackend::Unsupported &&
                                  before_backend == after_backend && before_peak_supported &&
                                  after_peak_supported && after_peak_bytes >= before_peak_bytes;
    if (record.peak_growth_supported != comparable_peaks) {
        return failure(Error::invalid_peak_growth, 92, fields[92].value_offset);
    }
    const uint64_t expected_growth =
        comparable_peaks ? after_peak_bytes - before_peak_bytes : UINT64_C(0);
    if (record.peak_growth_bytes != expected_growth) {
        return failure(Error::invalid_peak_growth, 93, fields[93].value_offset);
    }

    record.before_memory.backend = before_backend;
    if (before_current_supported) {
        record.before_memory.current_rss_bytes = before_current_bytes;
    }
    if (before_peak_supported) {
        record.before_memory.lifetime_peak_rss_bytes = before_peak_bytes;
    }
    record.after_memory.backend = after_backend;
    if (after_current_supported) {
        record.after_memory.current_rss_bytes = after_current_bytes;
    }
    if (after_peak_supported) {
        record.after_memory.lifetime_peak_rss_bytes = after_peak_bytes;
    }

    SIQSShadowProofObserveRecordParseResult result;
    result.record = record;
    result.diagnostic = {
        Error::none,
        SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_FIELD_INDEX,
        SIQS_SHADOW_PROOF_OBSERVE_RECORD_NO_ERROR_OFFSET,
    };
    return result;
}

} // namespace gnfs::siqs
