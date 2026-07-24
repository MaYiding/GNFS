#pragma once

// Private canonicalization contract for the SIQS RSS probe execution identity.
// The preimage contains no filesystem path. All integers are little-endian
// u64, all strings are u64 byte length followed by their raw bytes, and the
// executable SHA-256 is appended as 32 raw bytes.

#include <gnfs/siqs/shadow_proof_rss_campaign_artifact_layout.hpp>
#include <gnfs/siqs/shadow_proof_rss_probe_execution_identity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace gnfs::siqs::shadow_proof_rss_probe_execution_identity_detail {

inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_PROBE_EXECUTION_CONTRACT_DOMAIN =
    "gnfs.siqs.shadow_proof_rss.probe_execution_contract.sha256.v1";
inline constexpr std::uint64_t SIQS_SHADOW_PROOF_RSS_PROBE_TIMEOUT_MIN_MS = 1;
inline constexpr std::uint64_t SIQS_SHADOW_PROOF_RSS_PROBE_TIMEOUT_MAX_MS = 60'000;

inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_PROBE_ARGUMENT_TEMPLATE_ID =
    "gnfs.siqs.shadow_proof_rss.argv_template.v1";
inline constexpr std::uint64_t SIQS_SHADOW_PROOF_RSS_PROBE_ARGUMENT_TEMPLATE_VERSION = 1;
inline constexpr std::array<std::string_view, 6> SIQS_SHADOW_PROOF_RSS_PROBE_ARGUMENT_TEMPLATE = {
    "--fixture-id", "{fixture_id_u32_decimal}", "--mode", "{off|observe}",
    "--ordinal",    "{ordinal_u32_decimal}",
};

inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_PROBE_STDOUT_SCHEMA =
    "GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_V1";
inline constexpr std::uint64_t SIQS_SHADOW_PROOF_RSS_PROBE_STDOUT_SCHEMA_VERSION = 1;
inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_PROBE_OBSERVE_STDERR_SCHEMA =
    "GNFS_SIQS_SHADOW_PROOF_OBSERVE_V1";
inline constexpr std::uint64_t SIQS_SHADOW_PROOF_RSS_PROBE_OBSERVE_STDERR_SCHEMA_VERSION = 1;
inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_PROBE_JOINED_SCHEMA =
    "GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_JOINED_DRAFT_V3";
inline constexpr std::uint64_t SIQS_SHADOW_PROOF_RSS_PROBE_JOINED_SCHEMA_VERSION = 3;

inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_PROBE_TRANSPORT_CONTRACT_ID =
    "gnfs.util.bounded_child_process.transport.v1";
inline constexpr std::uint64_t SIQS_SHADOW_PROOF_RSS_PROBE_TRANSPORT_CONTRACT_VERSION = 1;

inline constexpr std::uint64_t SIQS_SHADOW_PROOF_RSS_PROBE_STDOUT_CAP_BYTES =
    SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDOUT_MAX_BYTES;
inline constexpr std::uint64_t SIQS_SHADOW_PROOF_RSS_PROBE_OFF_STDERR_CAP_BYTES = 0;
inline constexpr std::uint64_t SIQS_SHADOW_PROOF_RSS_PROBE_OBSERVE_STDERR_CAP_BYTES =
    SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDERR_MAX_BYTES;
inline constexpr std::uint64_t SIQS_SHADOW_PROOF_RSS_PROBE_JOINED_CAP_BYTES =
    SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_JOINED_MAX_BYTES;

struct ProbeExecutionContractInput final {
    util::Sha256Digest executable_sha256;
    SIQSShadowProofRssProbeKind probe_kind = SIQSShadowProofRssProbeKind::unknown;
    std::string_view candidate_revision;
    SIQSShadowProofRssOperatingSystem operating_system = SIQSShadowProofRssOperatingSystem::unknown;
    SIQSShadowProofRssArchitecture architecture = SIQSShadowProofRssArchitecture::unknown;
    util::ProcessMemoryBackend memory_backend = util::ProcessMemoryBackend::Unsupported;
    std::size_t resolved_production_sieve_workers = 0;
    bool release_build = false;
    bool ndebug = false;
    std::span<const std::string> environment;
    std::uint64_t timeout_ms = 0;
    std::uint64_t expected_owner = 0;
};

namespace canonical_detail {

static_assert(std::numeric_limits<std::size_t>::digits <=
              std::numeric_limits<std::uint64_t>::digits);

[[nodiscard]] constexpr bool digest_is_nonzero(const util::Sha256Digest& digest) noexcept {
    for (const std::byte value : digest.bytes) {
        if (value != std::byte{0}) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr bool safe_token(std::string_view value) noexcept {
    if (value.empty() || value.size() > SIQS_SHADOW_PROOF_RSS_GATE_MAX_TOKEN_BYTES) {
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

[[nodiscard]] constexpr bool valid_environment_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    const auto is_alpha = [](unsigned char value) constexpr {
        return (value >= static_cast<unsigned char>('A') &&
                value <= static_cast<unsigned char>('Z')) ||
               (value >= static_cast<unsigned char>('a') &&
                value <= static_cast<unsigned char>('z'));
    };
    const auto is_digit = [](unsigned char value) constexpr {
        return value >= static_cast<unsigned char>('0') && value <= static_cast<unsigned char>('9');
    };

    const auto first = static_cast<unsigned char>(name.front());
    if (!is_alpha(first) && first != static_cast<unsigned char>('_')) {
        return false;
    }
    for (const char character : name.substr(1)) {
        const auto byte = static_cast<unsigned char>(character);
        if (!is_alpha(byte) && !is_digit(byte) && byte != static_cast<unsigned char>('_')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr std::optional<std::string_view>
environment_name(std::string_view entry) noexcept {
    if (entry.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t separator = entry.find('=');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view name = entry.substr(0, separator);
    if (!valid_environment_name(name)) {
        return std::nullopt;
    }
    return name;
}

[[nodiscard]] inline bool
environment_is_canonical(std::span<const std::string> environment) noexcept {
    std::string_view previous_name;
    for (const std::string& owned_entry : environment) {
        const auto name = environment_name(owned_entry);
        if (!name.has_value() || (!previous_name.empty() && !(previous_name < *name))) {
            return false;
        }
        previous_name = *name;
    }
    return true;
}

[[nodiscard]] constexpr std::optional<std::uint64_t>
probe_kind_tag(SIQSShadowProofRssProbeKind value) noexcept {
    switch (value) {
    case SIQSShadowProofRssProbeKind::synthetic_test:
        return 1;
    case SIQSShadowProofRssProbeKind::production_holdout:
        return 2;
    case SIQSShadowProofRssProbeKind::unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<std::uint64_t>
operating_system_tag(SIQSShadowProofRssOperatingSystem value) noexcept {
    switch (value) {
    case SIQSShadowProofRssOperatingSystem::darwin:
        return 1;
    case SIQSShadowProofRssOperatingSystem::linux:
        return 2;
    case SIQSShadowProofRssOperatingSystem::windows:
        return 3;
    case SIQSShadowProofRssOperatingSystem::unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<std::uint64_t>
architecture_tag(SIQSShadowProofRssArchitecture value) noexcept {
    switch (value) {
    case SIQSShadowProofRssArchitecture::x86_64:
        return 1;
    case SIQSShadowProofRssArchitecture::arm64:
        return 2;
    case SIQSShadowProofRssArchitecture::unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<std::uint64_t>
memory_backend_tag(util::ProcessMemoryBackend value) noexcept {
    switch (value) {
    case util::ProcessMemoryBackend::DarwinGetrusage:
        return 1;
    case util::ProcessMemoryBackend::LinuxGetrusage:
        return 2;
    case util::ProcessMemoryBackend::WindowsPsapi:
        return 3;
    case util::ProcessMemoryBackend::Unsupported:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool
backend_matches_operating_system(SIQSShadowProofRssOperatingSystem operating_system,
                                 util::ProcessMemoryBackend memory_backend) noexcept {
    return (operating_system == SIQSShadowProofRssOperatingSystem::darwin &&
            memory_backend == util::ProcessMemoryBackend::DarwinGetrusage) ||
           (operating_system == SIQSShadowProofRssOperatingSystem::linux &&
            memory_backend == util::ProcessMemoryBackend::LinuxGetrusage) ||
           (operating_system == SIQSShadowProofRssOperatingSystem::windows &&
            memory_backend == util::ProcessMemoryBackend::WindowsPsapi);
}

class CanonicalWriter final {
public:
    [[nodiscard]] bool append_u64(std::uint64_t value) noexcept {
        std::array<std::byte, 8> encoded{};
        for (unsigned shift = 0; shift < 64; shift += 8) {
            encoded[shift / 8] = static_cast<std::byte>(value >> shift);
        }
        return accumulator_.update(encoded);
    }

    [[nodiscard]] bool append_string(std::string_view value) noexcept {
        return append_u64(static_cast<std::uint64_t>(value.size())) && accumulator_.update(value);
    }

    [[nodiscard]] bool append_digest(const util::Sha256Digest& digest) noexcept {
        return accumulator_.update(digest.bytes);
    }

    [[nodiscard]] std::optional<util::Sha256Digest> finish() noexcept {
        return accumulator_.finalize();
    }

private:
    util::Sha256Accumulator accumulator_;
};

} // namespace canonical_detail

/// Canonical preimage order:
/// domain, contract schema, raw executable digest, probe kind, revision,
/// OS/architecture/backend/workers, release/NDEBUG, sorted complete
/// environment, timeout/owner, argv template ID/version/items, capture caps,
/// output schemas, then transport ID/version and the
/// shell-free/fresh-process/process-group-cleanup flags.
[[nodiscard]] inline std::optional<SIQSShadowProofRssProbeExecutionIdentity>
make_siqs_shadow_proof_rss_probe_execution_identity(
    const ProbeExecutionContractInput& input) noexcept {
    using namespace canonical_detail;

    const auto probe_tag = probe_kind_tag(input.probe_kind);
    const auto os_tag = operating_system_tag(input.operating_system);
    const auto arch_tag = architecture_tag(input.architecture);
    const auto backend_tag = memory_backend_tag(input.memory_backend);
    if (!digest_is_nonzero(input.executable_sha256) || !probe_tag.has_value() ||
        !safe_token(input.candidate_revision) || !os_tag.has_value() || !arch_tag.has_value() ||
        !backend_tag.has_value() ||
        !backend_matches_operating_system(input.operating_system, input.memory_backend) ||
        input.resolved_production_sieve_workers == 0 ||
        !environment_is_canonical(input.environment) ||
        input.timeout_ms < SIQS_SHADOW_PROOF_RSS_PROBE_TIMEOUT_MIN_MS ||
        input.timeout_ms > SIQS_SHADOW_PROOF_RSS_PROBE_TIMEOUT_MAX_MS) {
        return std::nullopt;
    }

    CanonicalWriter writer;
    if (!writer.append_string(SIQS_SHADOW_PROOF_RSS_PROBE_EXECUTION_CONTRACT_DOMAIN) ||
        !writer.append_u64(SIQS_SHADOW_PROOF_RSS_PROBE_EXECUTION_CONTRACT_SCHEMA_VERSION) ||
        !writer.append_digest(input.executable_sha256) || !writer.append_u64(*probe_tag) ||
        !writer.append_string(input.candidate_revision) || !writer.append_u64(*os_tag) ||
        !writer.append_u64(*arch_tag) || !writer.append_u64(*backend_tag) ||
        !writer.append_u64(static_cast<std::uint64_t>(input.resolved_production_sieve_workers)) ||
        !writer.append_u64(input.release_build ? 1 : 0) ||
        !writer.append_u64(input.ndebug ? 1 : 0) ||
        !writer.append_u64(static_cast<std::uint64_t>(input.environment.size()))) {
        return std::nullopt;
    }
    for (const std::string& entry : input.environment) {
        if (!writer.append_string(entry)) {
            return std::nullopt;
        }
    }
    if (!writer.append_u64(input.timeout_ms) || !writer.append_u64(input.expected_owner) ||
        !writer.append_string(SIQS_SHADOW_PROOF_RSS_PROBE_ARGUMENT_TEMPLATE_ID) ||
        !writer.append_u64(SIQS_SHADOW_PROOF_RSS_PROBE_ARGUMENT_TEMPLATE_VERSION) ||
        !writer.append_u64(SIQS_SHADOW_PROOF_RSS_PROBE_ARGUMENT_TEMPLATE.size())) {
        return std::nullopt;
    }
    for (const std::string_view argument : SIQS_SHADOW_PROOF_RSS_PROBE_ARGUMENT_TEMPLATE) {
        if (!writer.append_string(argument)) {
            return std::nullopt;
        }
    }
    if (!writer.append_u64(SIQS_SHADOW_PROOF_RSS_PROBE_STDOUT_CAP_BYTES) ||
        !writer.append_u64(SIQS_SHADOW_PROOF_RSS_PROBE_OFF_STDERR_CAP_BYTES) ||
        !writer.append_u64(SIQS_SHADOW_PROOF_RSS_PROBE_OBSERVE_STDERR_CAP_BYTES) ||
        !writer.append_u64(SIQS_SHADOW_PROOF_RSS_PROBE_JOINED_CAP_BYTES) ||
        !writer.append_string(SIQS_SHADOW_PROOF_RSS_PROBE_STDOUT_SCHEMA) ||
        !writer.append_u64(SIQS_SHADOW_PROOF_RSS_PROBE_STDOUT_SCHEMA_VERSION) ||
        !writer.append_string(SIQS_SHADOW_PROOF_RSS_PROBE_OBSERVE_STDERR_SCHEMA) ||
        !writer.append_u64(SIQS_SHADOW_PROOF_RSS_PROBE_OBSERVE_STDERR_SCHEMA_VERSION) ||
        !writer.append_string(SIQS_SHADOW_PROOF_RSS_PROBE_JOINED_SCHEMA) ||
        !writer.append_u64(SIQS_SHADOW_PROOF_RSS_PROBE_JOINED_SCHEMA_VERSION) ||
        !writer.append_string(SIQS_SHADOW_PROOF_RSS_PROBE_TRANSPORT_CONTRACT_ID) ||
        !writer.append_u64(SIQS_SHADOW_PROOF_RSS_PROBE_TRANSPORT_CONTRACT_VERSION) ||
        !writer.append_u64(1) || // shell-free
        !writer.append_u64(1) || // fresh process
        !writer.append_u64(1)) { // process-group descendant cleanup
        return std::nullopt;
    }

    const auto contract_sha256 = writer.finish();
    if (!contract_sha256.has_value()) {
        return std::nullopt;
    }
    SIQSShadowProofRssProbeExecutionIdentity identity{
        .executable_sha256 = input.executable_sha256,
        .execution_contract_sha256 = *contract_sha256,
    };
    if (!siqs_shadow_proof_rss_probe_execution_identity_is_valid(identity)) {
        return std::nullopt;
    }
    return identity;
}

} // namespace gnfs::siqs::shadow_proof_rss_probe_execution_identity_detail
