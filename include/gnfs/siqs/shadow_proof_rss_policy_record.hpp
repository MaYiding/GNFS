#pragma once

/// @file shadow_proof_rss_policy_record.hpp
/// @brief Pure canonical record codec for SIQS shadow-proof RSS policies.

#include <gnfs/siqs/shadow_proof_rss_gate.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace gnfs::siqs {

inline constexpr char SIQS_SHADOW_PROOF_RSS_POLICY_RECORD_PREFIX[] =
    "GNFS_SIQS_SHADOW_PROOF_RSS_POLICY_V1";
inline constexpr uint32_t SIQS_SHADOW_PROOF_RSS_POLICY_RECORD_SCHEMA_VERSION = 1;

/// Parse failures are syntax and representation failures only. Approval,
/// corpus identity, backend/OS compatibility, and budget semantics remain the
/// responsibility of `evaluate_siqs_shadow_proof_rss_gate`.
enum class SIQSShadowProofRssPolicyRecordError : uint8_t {
    none,
    invalid_framing,
    invalid_ascii,
    invalid_prefix,
    missing_field,
    invalid_field,
    duplicate_field,
    unknown_field,
    field_out_of_order,
    invalid_schema_version,
    invalid_boolean,
    invalid_unsigned_integer,
    unsigned_integer_out_of_range,
    invalid_operating_system,
    invalid_architecture,
    invalid_memory_backend,
    resource_failure,
};

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_rss_policy_record_error_name(SIQSShadowProofRssPolicyRecordError error) noexcept {
    switch (error) {
    case SIQSShadowProofRssPolicyRecordError::none:
        return "none";
    case SIQSShadowProofRssPolicyRecordError::invalid_framing:
        return "invalid_framing";
    case SIQSShadowProofRssPolicyRecordError::invalid_ascii:
        return "invalid_ascii";
    case SIQSShadowProofRssPolicyRecordError::invalid_prefix:
        return "invalid_prefix";
    case SIQSShadowProofRssPolicyRecordError::missing_field:
        return "missing_field";
    case SIQSShadowProofRssPolicyRecordError::invalid_field:
        return "invalid_field";
    case SIQSShadowProofRssPolicyRecordError::duplicate_field:
        return "duplicate_field";
    case SIQSShadowProofRssPolicyRecordError::unknown_field:
        return "unknown_field";
    case SIQSShadowProofRssPolicyRecordError::field_out_of_order:
        return "field_out_of_order";
    case SIQSShadowProofRssPolicyRecordError::invalid_schema_version:
        return "invalid_schema_version";
    case SIQSShadowProofRssPolicyRecordError::invalid_boolean:
        return "invalid_boolean";
    case SIQSShadowProofRssPolicyRecordError::invalid_unsigned_integer:
        return "invalid_unsigned_integer";
    case SIQSShadowProofRssPolicyRecordError::unsigned_integer_out_of_range:
        return "unsigned_integer_out_of_range";
    case SIQSShadowProofRssPolicyRecordError::invalid_operating_system:
        return "invalid_operating_system";
    case SIQSShadowProofRssPolicyRecordError::invalid_architecture:
        return "invalid_architecture";
    case SIQSShadowProofRssPolicyRecordError::invalid_memory_backend:
        return "invalid_memory_backend";
    case SIQSShadowProofRssPolicyRecordError::resource_failure:
        return "resource_failure";
    }
    return "unknown";
}

/// Owning representation of one parsed policy record. The returned gate policy
/// borrows the three string fields. It must not outlive this value, and moving
/// or modifying this record invalidates the view.
struct SIQSShadowProofRssPolicyRecord final {
    bool approved = false;
    std::string corpus_id;
    SIQSShadowProofRssCorpusDigest corpus_digest;
    SIQSShadowProofRssOperatingSystem operating_system = SIQSShadowProofRssOperatingSystem::unknown;
    SIQSShadowProofRssArchitecture architecture = SIQSShadowProofRssArchitecture::unknown;
    util::ProcessMemoryBackend memory_backend = util::ProcessMemoryBackend::Unsupported;
    std::size_t resolved_production_sieve_workers = 0;
    std::string candidate_revision;
    std::string approval_id;
    uint64_t deployment_budget_bytes = 0;
    uint64_t reserved_headroom_bytes = 0;

    [[nodiscard]] SIQSShadowProofRssGatePolicy as_gate_policy() const& noexcept {
        return {approved,
                corpus_id,
                corpus_digest,
                operating_system,
                architecture,
                memory_backend,
                resolved_production_sieve_workers,
                candidate_revision,
                approval_id,
                deployment_budget_bytes,
                reserved_headroom_bytes};
    }

    [[nodiscard]] SIQSShadowProofRssGatePolicy as_gate_policy() && = delete;
    [[nodiscard]] SIQSShadowProofRssGatePolicy as_gate_policy() const&& = delete;
};

struct SIQSShadowProofRssPolicyRecordParseResult final {
    SIQSShadowProofRssPolicyRecord record;
    SIQSShadowProofRssPolicyRecordError error = SIQSShadowProofRssPolicyRecordError::none;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return error == SIQSShadowProofRssPolicyRecordError::none;
    }
};

namespace shadow_proof_rss_policy_record_detail {

inline constexpr std::array<std::string_view, 13> FIELD_NAMES{
    "schema_version",
    "approved",
    "corpus_id",
    "corpus_digest_low",
    "corpus_digest_high",
    "operating_system",
    "architecture",
    "memory_backend",
    "resolved_production_sieve_workers",
    "candidate_revision",
    "approval_id",
    "deployment_budget_bytes",
    "reserved_headroom_bytes",
};

struct Field final {
    std::string_view key;
    std::string_view value;
};

[[nodiscard]] constexpr bool record_token_is_valid(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x21U || byte > 0x7eU || byte == static_cast<unsigned char>('=')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr std::size_t field_name_index(std::string_view key) noexcept {
    for (std::size_t index = 0; index < FIELD_NAMES.size(); ++index) {
        if (FIELD_NAMES[index] == key) {
            return index;
        }
    }
    return FIELD_NAMES.size();
}

[[nodiscard]] constexpr SIQSShadowProofRssPolicyRecordError
classify_unexpected_field(std::string_view key, std::size_t expected_index) noexcept {
    const std::size_t actual_index = field_name_index(key);
    if (actual_index == FIELD_NAMES.size()) {
        return SIQSShadowProofRssPolicyRecordError::unknown_field;
    }
    if (actual_index < expected_index) {
        return SIQSShadowProofRssPolicyRecordError::duplicate_field;
    }
    return SIQSShadowProofRssPolicyRecordError::field_out_of_order;
}

[[nodiscard]] constexpr SIQSShadowProofRssPolicyRecordError split_field(std::string_view token,
                                                                        Field& field) noexcept {
    const std::size_t separator = token.find('=');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= token.size() ||
        token.find('=', separator + 1) != std::string_view::npos) {
        return SIQSShadowProofRssPolicyRecordError::invalid_field;
    }
    field = {token.substr(0, separator), token.substr(separator + 1)};
    return record_token_is_valid(field.value) ? SIQSShadowProofRssPolicyRecordError::none
                                              : SIQSShadowProofRssPolicyRecordError::invalid_field;
}

[[nodiscard]] constexpr SIQSShadowProofRssPolicyRecordError
parse_canonical_uint64(std::string_view value, uint64_t& parsed) noexcept {
    if (value.empty() || (value.size() > 1 && value.front() == '0')) {
        return SIQSShadowProofRssPolicyRecordError::invalid_unsigned_integer;
    }
    for (const char digit : value) {
        if (digit < '0' || digit > '9') {
            return SIQSShadowProofRssPolicyRecordError::invalid_unsigned_integer;
        }
    }
    uint64_t candidate = 0;
    const auto [end, status] =
        std::from_chars(value.data(), value.data() + value.size(), candidate);
    if (status == std::errc::result_out_of_range) {
        return SIQSShadowProofRssPolicyRecordError::unsigned_integer_out_of_range;
    }
    if (status != std::errc{} || end != value.data() + value.size()) {
        return SIQSShadowProofRssPolicyRecordError::invalid_unsigned_integer;
    }
    parsed = candidate;
    return SIQSShadowProofRssPolicyRecordError::none;
}

[[nodiscard]] constexpr bool
parse_operating_system(std::string_view value, SIQSShadowProofRssOperatingSystem& parsed) noexcept {
    if (value == "darwin") {
        parsed = SIQSShadowProofRssOperatingSystem::darwin;
        return true;
    }
    if (value == "linux") {
        parsed = SIQSShadowProofRssOperatingSystem::linux;
        return true;
    }
    if (value == "windows") {
        parsed = SIQSShadowProofRssOperatingSystem::windows;
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool parse_architecture(std::string_view value,
                                                SIQSShadowProofRssArchitecture& parsed) noexcept {
    if (value == "x86_64") {
        parsed = SIQSShadowProofRssArchitecture::x86_64;
        return true;
    }
    if (value == "arm64") {
        parsed = SIQSShadowProofRssArchitecture::arm64;
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool parse_memory_backend(std::string_view value,
                                                  util::ProcessMemoryBackend& parsed) noexcept {
    if (value == "darwin_getrusage") {
        parsed = util::ProcessMemoryBackend::DarwinGetrusage;
        return true;
    }
    if (value == "linux_getrusage") {
        parsed = util::ProcessMemoryBackend::LinuxGetrusage;
        return true;
    }
    if (value == "windows_psapi") {
        parsed = util::ProcessMemoryBackend::WindowsPsapi;
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool
known_record_operating_system(SIQSShadowProofRssOperatingSystem value) noexcept {
    return value == SIQSShadowProofRssOperatingSystem::darwin ||
           value == SIQSShadowProofRssOperatingSystem::linux ||
           value == SIQSShadowProofRssOperatingSystem::windows;
}

[[nodiscard]] constexpr bool
known_record_architecture(SIQSShadowProofRssArchitecture value) noexcept {
    return value == SIQSShadowProofRssArchitecture::x86_64 ||
           value == SIQSShadowProofRssArchitecture::arm64;
}

[[nodiscard]] constexpr bool known_record_backend(util::ProcessMemoryBackend value) noexcept {
    return value == util::ProcessMemoryBackend::DarwinGetrusage ||
           value == util::ProcessMemoryBackend::LinuxGetrusage ||
           value == util::ProcessMemoryBackend::WindowsPsapi;
}

inline void append_field(std::string& output, std::string_view key, std::string_view value) {
    output.push_back(' ');
    output.append(key);
    output.push_back('=');
    output.append(value);
}

inline void append_uint64_field(std::string& output, std::string_view key, uint64_t value) {
    char buffer[std::numeric_limits<uint64_t>::digits10 + 2]{};
    const auto [end, status] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (status != std::errc{}) {
        throw std::bad_alloc();
    }
    append_field(output, key, std::string_view(buffer, static_cast<std::size_t>(end - buffer)));
}

} // namespace shadow_proof_rss_policy_record_detail

/// Parse exactly one printable-ASCII, LF-terminated policy record. The function
/// reads only the supplied bytes and returns an owning result. It never reads a
/// file, host property, environment variable, or holdout fixture.
[[nodiscard]] inline SIQSShadowProofRssPolicyRecordParseResult
parse_siqs_shadow_proof_rss_policy_record(std::string_view bytes) noexcept {
    using namespace shadow_proof_rss_policy_record_detail;
    using Error = SIQSShadowProofRssPolicyRecordError;

    SIQSShadowProofRssPolicyRecordParseResult result;
    if (bytes.empty() || bytes.back() != '\n') {
        result.error = Error::invalid_framing;
        return result;
    }
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto byte = static_cast<unsigned char>(bytes[index]);
        if (index + 1 == bytes.size()) {
            if (byte != static_cast<unsigned char>('\n')) {
                result.error = Error::invalid_framing;
                return result;
            }
        } else if (byte < 0x20U || byte > 0x7eU) {
            result.error = Error::invalid_ascii;
            return result;
        }
    }

    std::string_view body = bytes.substr(0, bytes.size() - 1);
    if (body.empty() || body.front() == ' ' || body.back() == ' ' ||
        body.find("  ") != std::string_view::npos) {
        result.error = Error::invalid_framing;
        return result;
    }

    const std::size_t prefix_end = body.find(' ');
    if (prefix_end == std::string_view::npos ||
        body.substr(0, prefix_end) != SIQS_SHADOW_PROOF_RSS_POLICY_RECORD_PREFIX) {
        result.error = Error::invalid_prefix;
        return result;
    }
    body.remove_prefix(prefix_end + 1);

    std::array<std::string_view, FIELD_NAMES.size()> values{};
    for (std::size_t field_index = 0; field_index < FIELD_NAMES.size(); ++field_index) {
        if (body.empty()) {
            result.error = Error::missing_field;
            return result;
        }
        const std::size_t token_end = body.find(' ');
        const std::string_view token = body.substr(0, token_end);
        Field field;
        if (const Error error = split_field(token, field); error != Error::none) {
            result.error = error;
            return result;
        }
        if (field.key != FIELD_NAMES[field_index]) {
            result.error = classify_unexpected_field(field.key, field_index);
            return result;
        }
        values[field_index] = field.value;
        if (token_end == std::string_view::npos) {
            body = {};
        } else {
            body.remove_prefix(token_end + 1);
        }
    }
    if (!body.empty()) {
        const std::size_t token_end = body.find(' ');
        Field field;
        if (const Error error = split_field(body.substr(0, token_end), field);
            error != Error::none) {
            result.error = error;
        } else {
            result.error = field_name_index(field.key) == FIELD_NAMES.size()
                               ? Error::unknown_field
                               : Error::duplicate_field;
        }
        return result;
    }

    SIQSShadowProofRssPolicyRecord record;
    uint64_t schema_version = 0;
    if (const Error error = parse_canonical_uint64(values[0], schema_version);
        error != Error::none ||
        schema_version != SIQS_SHADOW_PROOF_RSS_POLICY_RECORD_SCHEMA_VERSION) {
        result.error = error == Error::none ? Error::invalid_schema_version : error;
        return result;
    }
    if (values[1] == "true") {
        record.approved = true;
    } else if (values[1] == "false") {
        record.approved = false;
    } else {
        result.error = Error::invalid_boolean;
        return result;
    }

    uint64_t workers = 0;
    if (const Error error = parse_canonical_uint64(values[3], record.corpus_digest.low);
        error != Error::none) {
        result.error = error;
        return result;
    }
    if (const Error error = parse_canonical_uint64(values[4], record.corpus_digest.high);
        error != Error::none) {
        result.error = error;
        return result;
    }
    if (!parse_operating_system(values[5], record.operating_system)) {
        result.error = Error::invalid_operating_system;
        return result;
    }
    if (!parse_architecture(values[6], record.architecture)) {
        result.error = Error::invalid_architecture;
        return result;
    }
    if (!parse_memory_backend(values[7], record.memory_backend)) {
        result.error = Error::invalid_memory_backend;
        return result;
    }
    if (const Error error = parse_canonical_uint64(values[8], workers); error != Error::none) {
        result.error = error;
        return result;
    }
    if (workers > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
        result.error = Error::unsigned_integer_out_of_range;
        return result;
    }
    record.resolved_production_sieve_workers = static_cast<std::size_t>(workers);
    if (const Error error = parse_canonical_uint64(values[11], record.deployment_budget_bytes);
        error != Error::none) {
        result.error = error;
        return result;
    }
    if (const Error error = parse_canonical_uint64(values[12], record.reserved_headroom_bytes);
        error != Error::none) {
        result.error = error;
        return result;
    }

    try {
        record.corpus_id.assign(values[2]);
        record.candidate_revision.assign(values[9]);
        record.approval_id.assign(values[10]);
        result.record = std::move(record);
    } catch (...) {
        result.error = Error::resource_failure;
    }
    return result;
}

/// Format one syntactically representable gate policy as the canonical record.
/// On failure `output` is unchanged. This function does not evaluate approval
/// or compute the policy-binding digest; those remain gate responsibilities.
[[nodiscard]] inline bool
emit_siqs_shadow_proof_rss_policy_record(const SIQSShadowProofRssGatePolicy& policy,
                                         std::string& output) noexcept {
    using namespace shadow_proof_rss_policy_record_detail;

    if (!record_token_is_valid(policy.corpus_id) ||
        !record_token_is_valid(policy.candidate_revision) ||
        !record_token_is_valid(policy.approval_id) ||
        !known_record_operating_system(policy.operating_system) ||
        !known_record_architecture(policy.architecture) ||
        !known_record_backend(policy.memory_backend) ||
        !policy.deployment_budget_bytes.has_value() ||
        !policy.reserved_headroom_bytes.has_value()) {
        return false;
    }
    if constexpr (sizeof(std::size_t) > sizeof(uint64_t)) {
        if (policy.resolved_production_sieve_workers >
            static_cast<std::size_t>(std::numeric_limits<uint64_t>::max())) {
            return false;
        }
    }

    try {
        std::string candidate;
        candidate.reserve(512);
        candidate.append(SIQS_SHADOW_PROOF_RSS_POLICY_RECORD_PREFIX);
        append_uint64_field(candidate, FIELD_NAMES[0],
                            SIQS_SHADOW_PROOF_RSS_POLICY_RECORD_SCHEMA_VERSION);
        append_field(candidate, FIELD_NAMES[1], policy.approved ? "true" : "false");
        append_field(candidate, FIELD_NAMES[2], policy.corpus_id);
        append_uint64_field(candidate, FIELD_NAMES[3], policy.corpus_digest.low);
        append_uint64_field(candidate, FIELD_NAMES[4], policy.corpus_digest.high);
        append_field(candidate, FIELD_NAMES[5],
                     siqs_shadow_proof_rss_operating_system_name(policy.operating_system));
        append_field(candidate, FIELD_NAMES[6],
                     siqs_shadow_proof_rss_architecture_name(policy.architecture));
        append_field(candidate, FIELD_NAMES[7],
                     util::process_memory_backend_name(policy.memory_backend));
        append_uint64_field(candidate, FIELD_NAMES[8],
                            static_cast<uint64_t>(policy.resolved_production_sieve_workers));
        append_field(candidate, FIELD_NAMES[9], policy.candidate_revision);
        append_field(candidate, FIELD_NAMES[10], policy.approval_id);
        append_uint64_field(candidate, FIELD_NAMES[11], *policy.deployment_budget_bytes);
        append_uint64_field(candidate, FIELD_NAMES[12], *policy.reserved_headroom_bytes);
        candidate.push_back('\n');
        output.swap(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace gnfs::siqs
