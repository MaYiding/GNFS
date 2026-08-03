#pragma once

// Strict, allocation-free decoder for one sealed SIQS RSS holdout probe
// stdout record. The decoder borrows its input only for the duration of the
// call and returns an owning scalar projection.

#include "shadow_proof_rss_holdout_probe_protocol_internal.hpp"

#include <gnfs/util/process_memory.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace gnfs::siqs::shadow_proof_rss_holdout_detail {

inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_MAX_BYTES = 4096;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_FIELD_COUNT = 45;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD =
    std::numeric_limits<std::size_t>::max();
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_BYTE_OFFSET =
    std::numeric_limits<std::size_t>::max();

enum class SIQSShadowProofRssHoldoutProbeRecordCodecError : uint8_t {
    none,
    input_empty,
    input_too_large,
    final_lf_missing,
    record_count_invalid,
    byte_invalid,
    spacing_invalid,
    prefix_invalid,
    field_count_invalid,
    field_token_invalid,
    field_order_invalid,
    field_value_empty,
    u32_invalid,
    u64_invalid,
    boolean_invalid,
    mode_invalid,
    backend_invalid,
    semantic_invalid,
};

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_rss_holdout_probe_record_codec_error_name(
    SIQSShadowProofRssHoldoutProbeRecordCodecError error) noexcept {
    switch (error) {
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::none:
        return "none";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::input_empty:
        return "input_empty";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::input_too_large:
        return "input_too_large";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::final_lf_missing:
        return "final_lf_missing";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::record_count_invalid:
        return "record_count_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::byte_invalid:
        return "byte_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::spacing_invalid:
        return "spacing_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::prefix_invalid:
        return "prefix_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::field_count_invalid:
        return "field_count_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::field_token_invalid:
        return "field_token_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::field_order_invalid:
        return "field_order_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::field_value_empty:
        return "field_value_empty";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::u32_invalid:
        return "u32_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::u64_invalid:
        return "u64_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::boolean_invalid:
        return "boolean_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::mode_invalid:
        return "mode_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::backend_invalid:
        return "backend_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordCodecError::semantic_invalid:
        return "semantic_invalid";
    }
    return "unknown";
}

struct SIQSShadowProofRssHoldoutProbeRecordCodecDiagnostic final {
    SIQSShadowProofRssHoldoutProbeRecordCodecError error =
        SIQSShadowProofRssHoldoutProbeRecordCodecError::none;
    std::size_t field_index = SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD;
    std::size_t byte_offset = SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_BYTE_OFFSET;
};

struct SIQSShadowProofRssHoldoutProbeDecodedRecord final {
    uint32_t fixture_id = 0;
    SIQSShadowProofRssHoldoutProbeMode mode = SIQSShadowProofRssHoldoutProbeMode::unknown;
    uint32_t ordinal = 0;
    uint64_t relations_found = 0;
    uint64_t polynomials_used = 0;
    uint64_t resolved_production_sieve_workers = 0;
    uint64_t factor_wall_ns = 0;
    util::ProcessMemoryBackend memory_backend = util::ProcessMemoryBackend::Unsupported;
    util::ProcessMemorySnapshot before_memory;
    util::ProcessMemorySnapshot after_memory;
    uint64_t absolute_peak_rss_bytes = 0;
    std::optional<uint64_t> peak_growth_bytes;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssHoldoutProbeDecodedRecord& left,
               const SIQSShadowProofRssHoldoutProbeDecodedRecord& right) noexcept {
        return left.fixture_id == right.fixture_id && left.mode == right.mode &&
               left.ordinal == right.ordinal && left.relations_found == right.relations_found &&
               left.polynomials_used == right.polynomials_used &&
               left.resolved_production_sieve_workers == right.resolved_production_sieve_workers &&
               left.factor_wall_ns == right.factor_wall_ns &&
               left.memory_backend == right.memory_backend &&
               left.before_memory.backend == right.before_memory.backend &&
               left.before_memory.current_rss_bytes == right.before_memory.current_rss_bytes &&
               left.before_memory.lifetime_peak_rss_bytes ==
                   right.before_memory.lifetime_peak_rss_bytes &&
               left.after_memory.backend == right.after_memory.backend &&
               left.after_memory.current_rss_bytes == right.after_memory.current_rss_bytes &&
               left.after_memory.lifetime_peak_rss_bytes ==
                   right.after_memory.lifetime_peak_rss_bytes &&
               left.absolute_peak_rss_bytes == right.absolute_peak_rss_bytes &&
               left.peak_growth_bytes == right.peak_growth_bytes;
    }
};

namespace siqs_shadow_proof_rss_holdout_probe_record_codec_detail {
class CodecAccess;
} // namespace siqs_shadow_proof_rss_holdout_probe_record_codec_detail

class SIQSShadowProofRssHoldoutProbeRecordDecodeResult final {
public:
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return decoded_.has_value() &&
               diagnostic_.error == SIQSShadowProofRssHoldoutProbeRecordCodecError::none;
    }

    [[nodiscard]] constexpr const SIQSShadowProofRssHoldoutProbeDecodedRecord*
    decoded() const noexcept {
        return decoded_.has_value() ? &*decoded_ : nullptr;
    }

    [[nodiscard]] constexpr const SIQSShadowProofRssHoldoutProbeRecordCodecDiagnostic&
    diagnostic() const noexcept {
        return diagnostic_;
    }

private:
    explicit constexpr SIQSShadowProofRssHoldoutProbeRecordDecodeResult(
        SIQSShadowProofRssHoldoutProbeDecodedRecord decoded) noexcept
        : decoded_(std::move(decoded)) {}

    explicit constexpr SIQSShadowProofRssHoldoutProbeRecordDecodeResult(
        SIQSShadowProofRssHoldoutProbeRecordCodecDiagnostic diagnostic) noexcept
        : diagnostic_(diagnostic) {}

    std::optional<SIQSShadowProofRssHoldoutProbeDecodedRecord> decoded_;
    SIQSShadowProofRssHoldoutProbeRecordCodecDiagnostic diagnostic_;

    friend class siqs_shadow_proof_rss_holdout_probe_record_codec_detail::CodecAccess;
};

namespace siqs_shadow_proof_rss_holdout_probe_record_codec_detail {

enum Field : std::size_t {
    schema_version,
    status,
    corpus_id,
    corpus_digest_low,
    corpus_digest_high,
    sealed_before_measurement,
    used_for_calibration,
    fixture_id,
    mode,
    ordinal,
    build_type,
    ndebug,
    fresh_process,
    completed,
    scope,
    environment_value,
    digits,
    modulus,
    expected_factor,
    expected_cofactor,
    max_seconds,
    factor_status,
    factor,
    cofactor,
    factor_identity,
    relations_found,
    polynomials_used,
    resolved_production_sieve_workers,
    factor_wall_ns,
    rss_scope,
    rss_backend,
    before_current_rss_supported,
    before_current_rss_bytes,
    before_peak_rss_supported,
    before_peak_rss_bytes,
    after_current_rss_supported,
    after_current_rss_bytes,
    after_peak_rss_supported,
    after_peak_rss_bytes,
    absolute_peak_rss_supported,
    absolute_peak_rss_bytes,
    peak_growth_supported,
    peak_growth_bytes,
    route,
    promotion,
    field_count,
};

inline constexpr std::array<std::string_view, field_count> FIELD_NAMES{
    "schema_version",
    "status",
    "corpus_id",
    "corpus_digest_low",
    "corpus_digest_high",
    "sealed_before_measurement",
    "used_for_calibration",
    "fixture_id",
    "mode",
    "ordinal",
    "build_type",
    "ndebug",
    "fresh_process",
    "completed",
    "scope",
    "env_value",
    "digits",
    "n",
    "expected_factor",
    "expected_cofactor",
    "max_seconds",
    "factor_status",
    "factor",
    "cofactor",
    "factor_identity",
    "relations_found",
    "polynomials_used",
    "resolved_production_sieve_workers",
    "factor_wall_ns",
    "rss_scope",
    "rss_backend",
    "before_current_rss_supported",
    "before_current_rss_bytes",
    "before_peak_rss_supported",
    "before_peak_rss_bytes",
    "after_current_rss_supported",
    "after_current_rss_bytes",
    "after_peak_rss_supported",
    "after_peak_rss_bytes",
    "absolute_peak_rss_supported",
    "absolute_peak_rss_bytes",
    "peak_growth_supported",
    "peak_growth_bytes",
    "route",
    "promotion",
};

static_assert(field_count == SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_FIELD_COUNT);
static_assert(FIELD_NAMES.size() == SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_FIELD_COUNT);

class CodecAccess final {
public:
    [[nodiscard]] static constexpr SIQSShadowProofRssHoldoutProbeRecordDecodeResult
    success(SIQSShadowProofRssHoldoutProbeDecodedRecord decoded) noexcept {
        return SIQSShadowProofRssHoldoutProbeRecordDecodeResult(std::move(decoded));
    }

    [[nodiscard]] static constexpr SIQSShadowProofRssHoldoutProbeRecordDecodeResult
    failure(SIQSShadowProofRssHoldoutProbeRecordCodecError error,
            std::size_t field_index = SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD,
            std::size_t byte_offset =
                SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_BYTE_OFFSET) noexcept {
        return SIQSShadowProofRssHoldoutProbeRecordDecodeResult({error, field_index, byte_offset});
    }
};

struct ParsedFields final {
    std::array<std::string_view, field_count> values{};
    std::array<std::size_t, field_count> value_offsets{};
};

template <typename Unsigned>
[[nodiscard]] bool parse_canonical_unsigned(std::string_view text, Unsigned& value,
                                            std::size_t& error_offset) noexcept {
    static_assert(std::numeric_limits<Unsigned>::is_integer);
    static_assert(!std::numeric_limits<Unsigned>::is_signed);
    if (text.empty()) {
        error_offset = 0;
        return false;
    }
    if (text.size() > 1 && text.front() == '0') {
        error_offset = 1;
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] < '0' || text[index] > '9') {
            error_offset = index;
            return false;
        }
    }
    Unsigned parsed = 0;
    const auto [end, status_code] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (status_code != std::errc{} || end != text.data() + text.size()) {
        error_offset = 0;
        return false;
    }
    value = parsed;
    return true;
}

[[nodiscard]] constexpr bool parse_boolean(std::string_view text, bool& value) noexcept {
    if (text == "true") {
        value = true;
        return true;
    }
    if (text == "false") {
        value = false;
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool parse_mode(std::string_view text,
                                        SIQSShadowProofRssHoldoutProbeMode& value) noexcept {
    if (text == "off") {
        value = SIQSShadowProofRssHoldoutProbeMode::off;
        return true;
    }
    if (text == "observe") {
        value = SIQSShadowProofRssHoldoutProbeMode::observe;
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool parse_backend(std::string_view text,
                                           util::ProcessMemoryBackend& value) noexcept {
    if (text == "darwin_getrusage") {
        value = util::ProcessMemoryBackend::DarwinGetrusage;
        return true;
    }
    if (text == "linux_getrusage") {
        value = util::ProcessMemoryBackend::LinuxGetrusage;
        return true;
    }
    if (text == "windows_psapi") {
        value = util::ProcessMemoryBackend::WindowsPsapi;
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool support_value_is_consistent(bool supported, uint64_t value) noexcept {
    return supported ? value > 0 : value == 0;
}

[[nodiscard]] constexpr Field
semantic_error_field(SIQSShadowProofRssHoldoutProbeRecordError error,
                     const SIQSShadowProofRssHoldoutProbeRecord& record) noexcept {
    using Error = SIQSShadowProofRssHoldoutProbeRecordError;
    switch (error) {
    case Error::none:
        return field_count;
    case Error::schema_invalid:
        return record.schema_version != SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_SCHEMA_VERSION
                   ? schema_version
                   : status;
    case Error::corpus_binding_invalid:
        if (record.corpus_id != siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_ID) {
            return corpus_id;
        }
        if (record.corpus_digest_low != siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_LOW) {
            return corpus_digest_low;
        }
        if (record.corpus_digest_high != siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_HIGH) {
            return corpus_digest_high;
        }
        return !record.sealed_before_measurement ? sealed_before_measurement : used_for_calibration;
    case Error::fixture_invalid:
        return fixture_id;
    case Error::mode_or_ordinal_invalid: {
        if (record.mode != SIQSShadowProofRssHoldoutProbeMode::off &&
            record.mode != SIQSShadowProofRssHoldoutProbeMode::observe) {
            return mode;
        }
        const uint32_t repetitions = record.mode == SIQSShadowProofRssHoldoutProbeMode::off
                                         ? siqs::SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS
                                         : siqs::SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS;
        return record.ordinal == 0 || record.ordinal > repetitions ? ordinal : environment_value;
    }
    case Error::execution_contract_invalid:
        if (record.build_type != "Release") {
            return build_type;
        }
        if (!record.ndebug) {
            return ndebug;
        }
        if (!record.fresh_process) {
            return fresh_process;
        }
        if (!record.completed) {
            return completed;
        }
        if (record.scope != "production_factor_fresh_process") {
            return scope;
        }
        return max_seconds;
    case Error::fixture_identity_invalid:
        if (record.digits != SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_DIGITS) {
            return digits;
        }
        if (record.fixture_id > 0 &&
            record.fixture_id <= fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1.size()) {
            const auto& fixture =
                fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1[record.fixture_id - 1];
            if (record.modulus != fixture.modulus) {
                return modulus;
            }
            if (record.expected_factor != fixture.factor_p) {
                return expected_factor;
            }
            if (record.expected_cofactor != fixture.factor_q) {
                return expected_cofactor;
            }
        }
        return modulus;
    case Error::factor_identity_invalid:
        if (record.factor_status != "factor_found") {
            return factor_status;
        }
        if (record.factor != record.expected_factor) {
            return factor;
        }
        if (record.cofactor != record.expected_cofactor) {
            return cofactor;
        }
        return factor_identity;
    case Error::result_metrics_invalid:
        if (record.relations_found == 0) {
            return relations_found;
        }
        if (record.polynomials_used == 0) {
            return polynomials_used;
        }
        if (record.resolved_production_sieve_workers == 0) {
            return resolved_production_sieve_workers;
        }
        return factor_wall_ns;
    case Error::rss_backend_invalid:
        return record.rss_scope != "self_lifetime" ? rss_scope : rss_backend;
    case Error::rss_optional_value_invalid:
        if (!support_value_is_consistent(record.before_current_rss_supported,
                                         record.before_current_rss_bytes)) {
            return before_current_rss_supported;
        }
        if (!support_value_is_consistent(record.before_peak_rss_supported,
                                         record.before_peak_rss_bytes)) {
            return before_peak_rss_supported;
        }
        if (!support_value_is_consistent(record.after_current_rss_supported,
                                         record.after_current_rss_bytes)) {
            return after_current_rss_supported;
        }
        return after_peak_rss_supported;
    case Error::absolute_peak_invalid:
        if (!record.after_peak_rss_supported) {
            return after_peak_rss_supported;
        }
        if (!record.absolute_peak_rss_supported) {
            return absolute_peak_rss_supported;
        }
        return absolute_peak_rss_bytes;
    case Error::peak_growth_invalid:
        if (record.before_peak_rss_supported) {
            if (record.after_peak_rss_bytes < record.before_peak_rss_bytes) {
                return after_peak_rss_bytes;
            }
            return record.peak_growth_supported ? peak_growth_bytes : peak_growth_supported;
        }
        return record.peak_growth_supported ? peak_growth_supported : peak_growth_bytes;
    case Error::route_invalid:
        return record.route != "legacy_result" ? route : promotion;
    }
    return field_count;
}

[[nodiscard]] constexpr std::optional<uint64_t> optional_measurement(bool supported,
                                                                     uint64_t value) noexcept {
    return supported ? std::optional<uint64_t>(value) : std::nullopt;
}

} // namespace siqs_shadow_proof_rss_holdout_probe_record_codec_detail

[[nodiscard]] inline SIQSShadowProofRssHoldoutProbeRecordDecodeResult
decode_siqs_shadow_proof_rss_holdout_probe_record(std::string_view input) noexcept {
    using Error = SIQSShadowProofRssHoldoutProbeRecordCodecError;
    using namespace siqs_shadow_proof_rss_holdout_probe_record_codec_detail;

    if (input.empty()) {
        return CodecAccess::failure(Error::input_empty,
                                    SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD, 0);
    }
    if (input.size() > SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_MAX_BYTES) {
        return CodecAccess::failure(Error::input_too_large,
                                    SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD,
                                    SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_MAX_BYTES);
    }
    if (input.back() != '\n') {
        return CodecAccess::failure(Error::final_lf_missing,
                                    SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD,
                                    input.size());
    }
    for (std::size_t index = 0; index + 1 < input.size(); ++index) {
        if (input[index] == '\n') {
            return CodecAccess::failure(Error::record_count_invalid,
                                        SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD, index);
        }
        const auto byte = static_cast<unsigned char>(input[index]);
        if (byte < 0x20U || byte > 0x7eU) {
            return CodecAccess::failure(Error::byte_invalid,
                                        SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD, index);
        }
    }

    const std::string_view body = input.substr(0, input.size() - 1);
    if (!body.empty() && body.front() == ' ') {
        return CodecAccess::failure(Error::spacing_invalid,
                                    SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD, 0);
    }
    if (!body.empty() && body.back() == ' ') {
        return CodecAccess::failure(Error::spacing_invalid,
                                    SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD,
                                    body.size() - 1);
    }
    if (const std::size_t doubled = body.find("  "); doubled != std::string_view::npos) {
        return CodecAccess::failure(Error::spacing_invalid,
                                    SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD,
                                    doubled + 1);
    }

    std::array<std::string_view, field_count + 1> tokens{};
    std::array<std::size_t, field_count + 1> token_offsets{};
    std::size_t token_count = 0;
    std::size_t token_begin = 0;
    while (token_begin <= body.size()) {
        const std::size_t separator = body.find(' ', token_begin);
        const std::size_t token_end = separator == std::string_view::npos ? body.size() : separator;
        if (token_count >= tokens.size()) {
            return CodecAccess::failure(Error::field_count_invalid,
                                        SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD,
                                        token_begin);
        }
        tokens[token_count] = body.substr(token_begin, token_end - token_begin);
        token_offsets[token_count] = token_begin;
        ++token_count;
        if (separator == std::string_view::npos) {
            break;
        }
        token_begin = separator + 1;
    }
    if (token_count != tokens.size()) {
        return CodecAccess::failure(Error::field_count_invalid,
                                    SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD,
                                    body.size());
    }

    if (tokens[0] != SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_PREFIX) {
        const std::size_t common =
            std::min(tokens[0].size(), SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_PREFIX.size());
        std::size_t mismatch = 0;
        while (mismatch < common &&
               tokens[0][mismatch] == SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_PREFIX[mismatch]) {
            ++mismatch;
        }
        return CodecAccess::failure(Error::prefix_invalid,
                                    SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD, mismatch);
    }

    ParsedFields fields;
    for (std::size_t index = 0; index < field_count; ++index) {
        const std::string_view token = tokens[index + 1];
        const std::size_t token_offset = token_offsets[index + 1];
        const std::size_t separator = token.find('=');
        if (separator == std::string_view::npos ||
            token.find('=', separator + 1) != std::string_view::npos) {
            return CodecAccess::failure(Error::field_token_invalid, index, token_offset);
        }
        const std::string_view key = token.substr(0, separator);
        if (key != FIELD_NAMES[index]) {
            return CodecAccess::failure(Error::field_order_invalid, index, token_offset);
        }
        const std::string_view value = token.substr(separator + 1);
        if (value.empty()) {
            return CodecAccess::failure(Error::field_value_empty, index,
                                        token_offset + separator + 1);
        }
        fields.values[index] = value;
        fields.value_offsets[index] = token_offset + separator + 1;
    }

    SIQSShadowProofRssHoldoutProbeRecord record;
    SIQSShadowProofRssHoldoutProbeRecordCodecDiagnostic diagnostic;
    const auto parse_u32_field = [&](Field field, uint32_t& value) noexcept {
        std::size_t relative_offset = 0;
        if (!parse_canonical_unsigned(fields.values[field], value, relative_offset)) {
            diagnostic = {Error::u32_invalid, field, fields.value_offsets[field] + relative_offset};
            return false;
        }
        return true;
    };
    const auto parse_u64_field = [&](Field field, uint64_t& value) noexcept {
        std::size_t relative_offset = 0;
        if (!parse_canonical_unsigned(fields.values[field], value, relative_offset)) {
            diagnostic = {Error::u64_invalid, field, fields.value_offsets[field] + relative_offset};
            return false;
        }
        return true;
    };
    const auto parse_boolean_field = [&](Field field, bool& value) noexcept {
        if (!parse_boolean(fields.values[field], value)) {
            diagnostic = {Error::boolean_invalid, field, fields.value_offsets[field]};
            return false;
        }
        return true;
    };

    if (!parse_u32_field(schema_version, record.schema_version)) {
        return CodecAccess::failure(diagnostic.error, diagnostic.field_index,
                                    diagnostic.byte_offset);
    }
    record.status = fields.values[status];
    record.corpus_id = fields.values[corpus_id];
    if (!parse_u64_field(corpus_digest_low, record.corpus_digest_low) ||
        !parse_u64_field(corpus_digest_high, record.corpus_digest_high) ||
        !parse_boolean_field(sealed_before_measurement, record.sealed_before_measurement) ||
        !parse_boolean_field(used_for_calibration, record.used_for_calibration) ||
        !parse_u32_field(fixture_id, record.fixture_id)) {
        return CodecAccess::failure(diagnostic.error, diagnostic.field_index,
                                    diagnostic.byte_offset);
    }
    if (!parse_mode(fields.values[mode], record.mode)) {
        return CodecAccess::failure(Error::mode_invalid, mode, fields.value_offsets[mode]);
    }
    if (!parse_u32_field(ordinal, record.ordinal)) {
        return CodecAccess::failure(diagnostic.error, diagnostic.field_index,
                                    diagnostic.byte_offset);
    }
    record.build_type = fields.values[build_type];
    if (!parse_boolean_field(ndebug, record.ndebug) ||
        !parse_boolean_field(fresh_process, record.fresh_process) ||
        !parse_boolean_field(completed, record.completed)) {
        return CodecAccess::failure(diagnostic.error, diagnostic.field_index,
                                    diagnostic.byte_offset);
    }
    record.scope = fields.values[scope];
    record.environment_value = fields.values[environment_value];
    if (!parse_u32_field(digits, record.digits)) {
        return CodecAccess::failure(diagnostic.error, diagnostic.field_index,
                                    diagnostic.byte_offset);
    }
    record.modulus = fields.values[modulus];
    record.expected_factor = fields.values[expected_factor];
    record.expected_cofactor = fields.values[expected_cofactor];
    if (!parse_u32_field(max_seconds, record.max_seconds)) {
        return CodecAccess::failure(diagnostic.error, diagnostic.field_index,
                                    diagnostic.byte_offset);
    }
    record.factor_status = fields.values[factor_status];
    record.factor = fields.values[factor];
    record.cofactor = fields.values[cofactor];
    record.factor_identity = fields.values[factor_identity];
    if (!parse_u64_field(relations_found, record.relations_found) ||
        !parse_u64_field(polynomials_used, record.polynomials_used) ||
        !parse_u64_field(resolved_production_sieve_workers,
                         record.resolved_production_sieve_workers) ||
        !parse_u64_field(factor_wall_ns, record.factor_wall_ns)) {
        return CodecAccess::failure(diagnostic.error, diagnostic.field_index,
                                    diagnostic.byte_offset);
    }
    record.rss_scope = fields.values[rss_scope];
    util::ProcessMemoryBackend parsed_backend = util::ProcessMemoryBackend::Unsupported;
    if (!parse_backend(fields.values[rss_backend], parsed_backend)) {
        return CodecAccess::failure(Error::backend_invalid, rss_backend,
                                    fields.value_offsets[rss_backend]);
    }
    record.rss_backend = fields.values[rss_backend];
    if (!parse_boolean_field(before_current_rss_supported, record.before_current_rss_supported) ||
        !parse_u64_field(before_current_rss_bytes, record.before_current_rss_bytes) ||
        !parse_boolean_field(before_peak_rss_supported, record.before_peak_rss_supported) ||
        !parse_u64_field(before_peak_rss_bytes, record.before_peak_rss_bytes) ||
        !parse_boolean_field(after_current_rss_supported, record.after_current_rss_supported) ||
        !parse_u64_field(after_current_rss_bytes, record.after_current_rss_bytes) ||
        !parse_boolean_field(after_peak_rss_supported, record.after_peak_rss_supported) ||
        !parse_u64_field(after_peak_rss_bytes, record.after_peak_rss_bytes) ||
        !parse_boolean_field(absolute_peak_rss_supported, record.absolute_peak_rss_supported) ||
        !parse_u64_field(absolute_peak_rss_bytes, record.absolute_peak_rss_bytes) ||
        !parse_boolean_field(peak_growth_supported, record.peak_growth_supported) ||
        !parse_u64_field(peak_growth_bytes, record.peak_growth_bytes)) {
        return CodecAccess::failure(diagnostic.error, diagnostic.field_index,
                                    diagnostic.byte_offset);
    }
    record.route = fields.values[route];
    if (!parse_boolean_field(promotion, record.promotion)) {
        return CodecAccess::failure(diagnostic.error, diagnostic.field_index,
                                    diagnostic.byte_offset);
    }

    const SIQSShadowProofRssHoldoutProbeRecordError semantic_error =
        validate_siqs_shadow_proof_rss_holdout_probe_record(record);
    if (semantic_error != SIQSShadowProofRssHoldoutProbeRecordError::none) {
        const Field field = semantic_error_field(semantic_error, record);
        const std::size_t field_index =
            field == field_count ? SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD : field;
        const std::size_t byte_offset =
            field == field_count ? SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_BYTE_OFFSET
                                 : fields.value_offsets[field];
        return CodecAccess::failure(Error::semantic_invalid, field_index, byte_offset);
    }

    SIQSShadowProofRssHoldoutProbeDecodedRecord decoded;
    decoded.fixture_id = record.fixture_id;
    decoded.mode = record.mode;
    decoded.ordinal = record.ordinal;
    decoded.relations_found = record.relations_found;
    decoded.polynomials_used = record.polynomials_used;
    decoded.resolved_production_sieve_workers = record.resolved_production_sieve_workers;
    decoded.factor_wall_ns = record.factor_wall_ns;
    decoded.memory_backend = parsed_backend;
    decoded.before_memory = {
        parsed_backend,
        optional_measurement(record.before_current_rss_supported, record.before_current_rss_bytes),
        optional_measurement(record.before_peak_rss_supported, record.before_peak_rss_bytes),
    };
    decoded.after_memory = {
        parsed_backend,
        optional_measurement(record.after_current_rss_supported, record.after_current_rss_bytes),
        optional_measurement(record.after_peak_rss_supported, record.after_peak_rss_bytes),
    };
    decoded.absolute_peak_rss_bytes = record.absolute_peak_rss_bytes;
    decoded.peak_growth_bytes =
        optional_measurement(record.peak_growth_supported, record.peak_growth_bytes);
    return CodecAccess::success(std::move(decoded));
}

} // namespace gnfs::siqs::shadow_proof_rss_holdout_detail
