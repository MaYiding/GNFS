#pragma once

// Pure command-line and record contracts for the sealed SIQS RSS holdout
// probe. This header binds records to the constexpr fixture manifest but
// performs no I/O, factoring, or memory capture.

#include "shadow_proof_rss_holdout_fixture_internal.hpp"

#include <gnfs/siqs/shadow_proof_rss_gate.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace gnfs::siqs::shadow_proof_rss_holdout_detail {

namespace fixtures = gnfs::siqs::shadow_proof_rss_holdout_fixture_detail;

inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_PREFIX =
    "GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_V1";
inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_ERROR_PREFIX =
    "GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_ERROR_V1";
inline constexpr uint32_t SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_SCHEMA_VERSION = 1;
inline constexpr uint32_t SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_MAX_SECONDS = 30;
inline constexpr uint32_t SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_DIGITS = 50;

static_assert(fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_FIXTURE_COUNT ==
              siqs::SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT);
static_assert(fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_MAX_SECONDS ==
              SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_MAX_SECONDS);
static_assert(fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SEALED_BEFORE_MEASUREMENT);
static_assert(!fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_USED_FOR_CALIBRATION);
static_assert(fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_CORPUS_ID ==
              siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_ID);
static_assert(fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_LOW ==
              siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_LOW);
static_assert(fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_HIGH ==
              siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_HIGH);
static_assert([]() constexpr {
    for (const auto& fixture : fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1) {
        if (fixture.modulus.size() != SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_DIGITS) {
            return false;
        }
    }
    return true;
}());

enum class SIQSShadowProofRssHoldoutProbeMode : uint8_t {
    unknown,
    off,
    observe,
};

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_rss_holdout_probe_mode_name(SIQSShadowProofRssHoldoutProbeMode mode) noexcept {
    switch (mode) {
    case SIQSShadowProofRssHoldoutProbeMode::unknown:
        return "unknown";
    case SIQSShadowProofRssHoldoutProbeMode::off:
        return "off";
    case SIQSShadowProofRssHoldoutProbeMode::observe:
        return "observe";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view siqs_shadow_proof_rss_holdout_probe_environment_value(
    SIQSShadowProofRssHoldoutProbeMode mode) noexcept {
    switch (mode) {
    case SIQSShadowProofRssHoldoutProbeMode::off:
        return "0";
    case SIQSShadowProofRssHoldoutProbeMode::observe:
        return "observe";
    case SIQSShadowProofRssHoldoutProbeMode::unknown:
        return "unknown";
    }
    return "unknown";
}

struct SIQSShadowProofRssHoldoutProbeOptions final {
    uint32_t fixture_id = 0;
    SIQSShadowProofRssHoldoutProbeMode mode = SIQSShadowProofRssHoldoutProbeMode::unknown;
    uint32_t ordinal = 0;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssHoldoutProbeOptions&,
               const SIQSShadowProofRssHoldoutProbeOptions&) noexcept = default;
};

enum class SIQSShadowProofRssHoldoutProbeOptionsError : uint8_t {
    none,
    unknown_argument,
    duplicate_argument,
    argument_value_missing,
    required_argument_missing,
    fixture_id_invalid,
    mode_invalid,
    ordinal_invalid,
};

[[nodiscard]] constexpr std::string_view siqs_shadow_proof_rss_holdout_probe_options_error_name(
    SIQSShadowProofRssHoldoutProbeOptionsError error) noexcept {
    switch (error) {
    case SIQSShadowProofRssHoldoutProbeOptionsError::none:
        return "none";
    case SIQSShadowProofRssHoldoutProbeOptionsError::unknown_argument:
        return "unknown_argument";
    case SIQSShadowProofRssHoldoutProbeOptionsError::duplicate_argument:
        return "duplicate_argument";
    case SIQSShadowProofRssHoldoutProbeOptionsError::argument_value_missing:
        return "argument_value_missing";
    case SIQSShadowProofRssHoldoutProbeOptionsError::required_argument_missing:
        return "required_argument_missing";
    case SIQSShadowProofRssHoldoutProbeOptionsError::fixture_id_invalid:
        return "fixture_id_invalid";
    case SIQSShadowProofRssHoldoutProbeOptionsError::mode_invalid:
        return "mode_invalid";
    case SIQSShadowProofRssHoldoutProbeOptionsError::ordinal_invalid:
        return "ordinal_invalid";
    }
    return "unknown";
}

struct SIQSShadowProofRssHoldoutProbeOptionsParseResult final {
    SIQSShadowProofRssHoldoutProbeOptions options;
    SIQSShadowProofRssHoldoutProbeOptionsError error =
        SIQSShadowProofRssHoldoutProbeOptionsError::none;
    std::string_view argument;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return error == SIQSShadowProofRssHoldoutProbeOptionsError::none;
    }
};

namespace siqs_shadow_proof_rss_holdout_probe_protocol_detail {

[[nodiscard]] inline bool parse_canonical_u32(std::string_view text, uint32_t& value) noexcept {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) {
        return false;
    }
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
    }
    uint32_t parsed = 0;
    const auto [end, status] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (status != std::errc{} || end != text.data() + text.size()) {
        return false;
    }
    value = parsed;
    return true;
}

[[nodiscard]] constexpr bool canonical_positive_decimal(std::string_view value) noexcept {
    if (value.empty() || value.front() == '0') {
        return false;
    }
    for (const char character : value) {
        if (character < '0' || character > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool canonical_decimal_less_equal(std::string_view left,
                                                          std::string_view right) noexcept {
    return left.size() < right.size() || (left.size() == right.size() && left <= right);
}

[[nodiscard]] constexpr bool supported_rss_backend(std::string_view value) noexcept {
    return value == "darwin_getrusage" || value == "linux_getrusage" || value == "windows_psapi";
}

[[nodiscard]] constexpr bool supported_value_is_consistent(bool supported,
                                                           uint64_t bytes) noexcept {
    return supported ? bytes > 0 : bytes == 0;
}

inline void append_field(std::string& output, std::string_view key, std::string_view value) {
    output.push_back(' ');
    output.append(key);
    output.push_back('=');
    output.append(value);
}

inline void append_u64_field(std::string& output, std::string_view key, uint64_t value) {
    char buffer[std::numeric_limits<uint64_t>::digits10 + 2]{};
    const auto [end, status] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (status != std::errc{}) {
        throw std::bad_alloc();
    }
    append_field(output, key, std::string_view(buffer, static_cast<std::size_t>(end - buffer)));
}

[[nodiscard]] constexpr std::string_view bool_name(bool value) noexcept {
    return value ? "true" : "false";
}

} // namespace siqs_shadow_proof_rss_holdout_probe_protocol_detail

[[nodiscard]] inline SIQSShadowProofRssHoldoutProbeOptionsParseResult
parse_siqs_shadow_proof_rss_holdout_probe_options(
    std::span<const std::string_view> arguments) noexcept {
    using Error = SIQSShadowProofRssHoldoutProbeOptionsError;
    using namespace siqs_shadow_proof_rss_holdout_probe_protocol_detail;

    SIQSShadowProofRssHoldoutProbeOptionsParseResult result;
    bool have_fixture_id = false;
    bool have_mode = false;
    bool have_ordinal = false;
    std::string_view ordinal_value;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];
        bool* present = nullptr;
        if (argument == "--fixture-id") {
            present = &have_fixture_id;
        } else if (argument == "--mode") {
            present = &have_mode;
        } else if (argument == "--ordinal") {
            present = &have_ordinal;
        } else {
            result.error = Error::unknown_argument;
            result.argument = argument;
            return result;
        }
        if (*present) {
            result.error = Error::duplicate_argument;
            result.argument = argument;
            return result;
        }
        if (index + 1 >= arguments.size()) {
            result.error = Error::argument_value_missing;
            result.argument = argument;
            return result;
        }
        *present = true;
        const std::string_view value = arguments[++index];
        if (argument == "--fixture-id") {
            if (!parse_canonical_u32(value, result.options.fixture_id) ||
                result.options.fixture_id == 0 ||
                result.options.fixture_id > siqs::SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT) {
                result.error = Error::fixture_id_invalid;
                result.argument = value;
                return result;
            }
        } else if (argument == "--mode") {
            if (value == "off") {
                result.options.mode = SIQSShadowProofRssHoldoutProbeMode::off;
            } else if (value == "observe") {
                result.options.mode = SIQSShadowProofRssHoldoutProbeMode::observe;
            } else {
                result.error = Error::mode_invalid;
                result.argument = value;
                return result;
            }
        } else if (!parse_canonical_u32(value, result.options.ordinal)) {
            result.error = Error::ordinal_invalid;
            result.argument = value;
            return result;
        } else {
            ordinal_value = value;
        }
    }

    if (!have_fixture_id || !have_mode || !have_ordinal) {
        result.error = Error::required_argument_missing;
        result.argument = !have_fixture_id ? "--fixture-id" : !have_mode ? "--mode" : "--ordinal";
        return result;
    }
    const uint32_t repetitions = result.options.mode == SIQSShadowProofRssHoldoutProbeMode::off
                                     ? siqs::SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS
                                     : siqs::SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS;
    if (result.options.ordinal == 0 || result.options.ordinal > repetitions) {
        result.error = Error::ordinal_invalid;
        result.argument = ordinal_value;
    }
    return result;
}

struct SIQSShadowProofRssHoldoutProbeRecord final {
    uint32_t schema_version = SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_SCHEMA_VERSION;
    std::string_view status = "valid";
    std::string_view corpus_id = siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_ID;
    uint64_t corpus_digest_low = siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_LOW;
    uint64_t corpus_digest_high = siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_HIGH;
    bool sealed_before_measurement = true;
    bool used_for_calibration = false;
    uint32_t fixture_id = 0;
    SIQSShadowProofRssHoldoutProbeMode mode = SIQSShadowProofRssHoldoutProbeMode::unknown;
    uint32_t ordinal = 0;
    std::string_view build_type = "Release";
    bool ndebug = true;
    bool fresh_process = true;
    bool completed = true;
    std::string_view scope = "production_factor_fresh_process";
    std::string_view environment_value;
    uint32_t digits = 0;
    std::string_view modulus;
    std::string_view expected_factor;
    std::string_view expected_cofactor;
    uint32_t max_seconds = SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_MAX_SECONDS;
    std::string_view factor_status = "factor_found";
    std::string_view factor;
    std::string_view cofactor;
    std::string_view factor_identity = "pass";
    uint64_t relations_found = 0;
    uint64_t polynomials_used = 0;
    uint64_t resolved_production_sieve_workers = 0;
    uint64_t factor_wall_ns = 0;
    std::string_view rss_scope = "self_lifetime";
    std::string_view rss_backend;
    bool before_current_rss_supported = false;
    uint64_t before_current_rss_bytes = 0;
    bool before_peak_rss_supported = false;
    uint64_t before_peak_rss_bytes = 0;
    bool after_current_rss_supported = false;
    uint64_t after_current_rss_bytes = 0;
    bool after_peak_rss_supported = false;
    uint64_t after_peak_rss_bytes = 0;
    bool absolute_peak_rss_supported = false;
    uint64_t absolute_peak_rss_bytes = 0;
    bool peak_growth_supported = false;
    uint64_t peak_growth_bytes = 0;
    std::string_view route = "legacy_result";
    bool promotion = false;
};

enum class SIQSShadowProofRssHoldoutProbeRecordError : uint8_t {
    none,
    schema_invalid,
    corpus_binding_invalid,
    fixture_invalid,
    mode_or_ordinal_invalid,
    execution_contract_invalid,
    fixture_identity_invalid,
    factor_identity_invalid,
    result_metrics_invalid,
    rss_backend_invalid,
    rss_optional_value_invalid,
    absolute_peak_invalid,
    peak_growth_invalid,
    route_invalid,
};

[[nodiscard]] constexpr std::string_view siqs_shadow_proof_rss_holdout_probe_record_error_name(
    SIQSShadowProofRssHoldoutProbeRecordError error) noexcept {
    switch (error) {
    case SIQSShadowProofRssHoldoutProbeRecordError::none:
        return "none";
    case SIQSShadowProofRssHoldoutProbeRecordError::schema_invalid:
        return "schema_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordError::corpus_binding_invalid:
        return "corpus_binding_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordError::fixture_invalid:
        return "fixture_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordError::mode_or_ordinal_invalid:
        return "mode_or_ordinal_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordError::execution_contract_invalid:
        return "execution_contract_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordError::fixture_identity_invalid:
        return "fixture_identity_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordError::factor_identity_invalid:
        return "factor_identity_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordError::result_metrics_invalid:
        return "result_metrics_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordError::rss_backend_invalid:
        return "rss_backend_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordError::rss_optional_value_invalid:
        return "rss_optional_value_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordError::absolute_peak_invalid:
        return "absolute_peak_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordError::peak_growth_invalid:
        return "peak_growth_invalid";
    case SIQSShadowProofRssHoldoutProbeRecordError::route_invalid:
        return "route_invalid";
    }
    return "unknown";
}

[[nodiscard]] constexpr SIQSShadowProofRssHoldoutProbeRecordError
validate_siqs_shadow_proof_rss_holdout_probe_record(
    const SIQSShadowProofRssHoldoutProbeRecord& record) noexcept {
    using Error = SIQSShadowProofRssHoldoutProbeRecordError;
    using namespace siqs_shadow_proof_rss_holdout_probe_protocol_detail;

    if (record.schema_version != SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_SCHEMA_VERSION ||
        record.status != "valid") {
        return Error::schema_invalid;
    }
    if (record.corpus_id != siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_ID ||
        record.corpus_digest_low != siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_LOW ||
        record.corpus_digest_high != siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_HIGH ||
        !record.sealed_before_measurement || record.used_for_calibration) {
        return Error::corpus_binding_invalid;
    }
    if (record.fixture_id == 0 ||
        record.fixture_id > siqs::SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT) {
        return Error::fixture_invalid;
    }
    const std::size_t fixture_index = static_cast<std::size_t>(record.fixture_id - 1);
    const auto& sealed_fixture = fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1[fixture_index];
    if (sealed_fixture.id != record.fixture_id) {
        return Error::fixture_invalid;
    }
    const uint32_t repetitions = record.mode == SIQSShadowProofRssHoldoutProbeMode::off
                                     ? siqs::SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS
                                 : record.mode == SIQSShadowProofRssHoldoutProbeMode::observe
                                     ? siqs::SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS
                                     : 0;
    if (repetitions == 0 || record.ordinal == 0 || record.ordinal > repetitions ||
        record.environment_value !=
            siqs_shadow_proof_rss_holdout_probe_environment_value(record.mode)) {
        return Error::mode_or_ordinal_invalid;
    }
    if (record.build_type != "Release" || !record.ndebug || !record.fresh_process ||
        !record.completed || record.scope != "production_factor_fresh_process" ||
        record.max_seconds != SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_MAX_SECONDS) {
        return Error::execution_contract_invalid;
    }
    if (record.digits != SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_DIGITS ||
        !canonical_positive_decimal(record.modulus) || record.modulus.size() != record.digits ||
        !canonical_positive_decimal(record.expected_factor) ||
        !canonical_positive_decimal(record.expected_cofactor) ||
        !canonical_decimal_less_equal(record.expected_factor, record.expected_cofactor) ||
        record.modulus != sealed_fixture.modulus ||
        record.expected_factor != sealed_fixture.factor_p ||
        record.expected_cofactor != sealed_fixture.factor_q) {
        return Error::fixture_identity_invalid;
    }
    if (record.factor_status != "factor_found" || record.factor_identity != "pass" ||
        record.factor != record.expected_factor || record.cofactor != record.expected_cofactor) {
        return Error::factor_identity_invalid;
    }
    if (record.relations_found == 0 || record.polynomials_used == 0 ||
        record.resolved_production_sieve_workers == 0 || record.factor_wall_ns == 0) {
        return Error::result_metrics_invalid;
    }
    if (record.rss_scope != "self_lifetime" || !supported_rss_backend(record.rss_backend)) {
        return Error::rss_backend_invalid;
    }
    if (!supported_value_is_consistent(record.before_current_rss_supported,
                                       record.before_current_rss_bytes) ||
        !supported_value_is_consistent(record.before_peak_rss_supported,
                                       record.before_peak_rss_bytes) ||
        !supported_value_is_consistent(record.after_current_rss_supported,
                                       record.after_current_rss_bytes) ||
        !supported_value_is_consistent(record.after_peak_rss_supported,
                                       record.after_peak_rss_bytes)) {
        return Error::rss_optional_value_invalid;
    }
    if (!record.after_peak_rss_supported || !record.absolute_peak_rss_supported ||
        record.absolute_peak_rss_bytes == 0 ||
        record.absolute_peak_rss_bytes != record.after_peak_rss_bytes) {
        return Error::absolute_peak_invalid;
    }
    if (record.before_peak_rss_supported) {
        if (record.after_peak_rss_bytes < record.before_peak_rss_bytes ||
            !record.peak_growth_supported ||
            record.peak_growth_bytes !=
                record.after_peak_rss_bytes - record.before_peak_rss_bytes) {
            return Error::peak_growth_invalid;
        }
    } else if (record.peak_growth_supported || record.peak_growth_bytes != 0) {
        return Error::peak_growth_invalid;
    }
    if (record.route != "legacy_result" || record.promotion) {
        return Error::route_invalid;
    }
    return Error::none;
}

[[nodiscard]] inline bool
emit_siqs_shadow_proof_rss_holdout_probe_record(const SIQSShadowProofRssHoldoutProbeRecord& record,
                                                std::string& output) noexcept {
    using namespace siqs_shadow_proof_rss_holdout_probe_protocol_detail;
    if (validate_siqs_shadow_proof_rss_holdout_probe_record(record) !=
        SIQSShadowProofRssHoldoutProbeRecordError::none) {
        return false;
    }

    try {
        std::string candidate;
        candidate.reserve(2048);
        candidate.append(SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_PREFIX);
        append_u64_field(candidate, "schema_version", record.schema_version);
        append_field(candidate, "status", record.status);
        append_field(candidate, "corpus_id", record.corpus_id);
        append_u64_field(candidate, "corpus_digest_low", record.corpus_digest_low);
        append_u64_field(candidate, "corpus_digest_high", record.corpus_digest_high);
        append_field(candidate, "sealed_before_measurement",
                     bool_name(record.sealed_before_measurement));
        append_field(candidate, "used_for_calibration", bool_name(record.used_for_calibration));
        append_u64_field(candidate, "fixture_id", record.fixture_id);
        append_field(candidate, "mode", siqs_shadow_proof_rss_holdout_probe_mode_name(record.mode));
        append_u64_field(candidate, "ordinal", record.ordinal);
        append_field(candidate, "build_type", record.build_type);
        append_field(candidate, "ndebug", bool_name(record.ndebug));
        append_field(candidate, "fresh_process", bool_name(record.fresh_process));
        append_field(candidate, "completed", bool_name(record.completed));
        append_field(candidate, "scope", record.scope);
        append_field(candidate, "env_value", record.environment_value);
        append_u64_field(candidate, "digits", record.digits);
        append_field(candidate, "n", record.modulus);
        append_field(candidate, "expected_factor", record.expected_factor);
        append_field(candidate, "expected_cofactor", record.expected_cofactor);
        append_u64_field(candidate, "max_seconds", record.max_seconds);
        append_field(candidate, "factor_status", record.factor_status);
        append_field(candidate, "factor", record.factor);
        append_field(candidate, "cofactor", record.cofactor);
        append_field(candidate, "factor_identity", record.factor_identity);
        append_u64_field(candidate, "relations_found", record.relations_found);
        append_u64_field(candidate, "polynomials_used", record.polynomials_used);
        append_u64_field(candidate, "resolved_production_sieve_workers",
                         record.resolved_production_sieve_workers);
        append_u64_field(candidate, "factor_wall_ns", record.factor_wall_ns);
        append_field(candidate, "rss_scope", record.rss_scope);
        append_field(candidate, "rss_backend", record.rss_backend);
        append_field(candidate, "before_current_rss_supported",
                     bool_name(record.before_current_rss_supported));
        append_u64_field(candidate, "before_current_rss_bytes", record.before_current_rss_bytes);
        append_field(candidate, "before_peak_rss_supported",
                     bool_name(record.before_peak_rss_supported));
        append_u64_field(candidate, "before_peak_rss_bytes", record.before_peak_rss_bytes);
        append_field(candidate, "after_current_rss_supported",
                     bool_name(record.after_current_rss_supported));
        append_u64_field(candidate, "after_current_rss_bytes", record.after_current_rss_bytes);
        append_field(candidate, "after_peak_rss_supported",
                     bool_name(record.after_peak_rss_supported));
        append_u64_field(candidate, "after_peak_rss_bytes", record.after_peak_rss_bytes);
        append_field(candidate, "absolute_peak_rss_supported",
                     bool_name(record.absolute_peak_rss_supported));
        append_u64_field(candidate, "absolute_peak_rss_bytes", record.absolute_peak_rss_bytes);
        append_field(candidate, "peak_growth_supported", bool_name(record.peak_growth_supported));
        append_u64_field(candidate, "peak_growth_bytes", record.peak_growth_bytes);
        append_field(candidate, "route", record.route);
        append_field(candidate, "promotion", bool_name(record.promotion));
        candidate.push_back('\n');
        output.swap(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace gnfs::siqs::shadow_proof_rss_holdout_detail
