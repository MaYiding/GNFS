#pragma once

/// @file shadow_proof_rss_gate.hpp
/// @brief Pure, fail-closed RSS evidence gate for SIQS shadow-proof review.

#include <gnfs/util/process_memory.hpp>

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string_view>

namespace gnfs::siqs {

inline constexpr char SIQS_SHADOW_PROOF_RSS_GATE_RECORD_PREFIX[] =
    "GNFS_SIQS_SHADOW_PROOF_RSS_GATE_V1";
inline constexpr uint32_t SIQS_SHADOW_PROOF_RSS_GATE_SCHEMA_VERSION = 1;
inline constexpr uint32_t SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT = 8;
inline constexpr uint32_t SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS = 3;
inline constexpr uint32_t SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS = 7;
inline constexpr uint32_t SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT =
    SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT *
    (SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS + SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS);
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_GATE_MAX_TOKEN_BYTES = 128;
inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_ID =
    "siqs50_shadow_observe_rss_holdout_v1";
inline constexpr uint64_t SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_LOW =
    UINT64_C(303806906129662515);
inline constexpr uint64_t SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_HIGH =
    UINT64_C(18179245792498443738);

enum class SIQSShadowProofRssOperatingSystem : uint8_t {
    unknown,
    darwin,
    linux,
    windows,
};

enum class SIQSShadowProofRssArchitecture : uint8_t {
    unknown,
    x86_64,
    arm64,
};

enum class SIQSShadowProofRssSampleMode : uint8_t {
    unknown,
    off,
    observe,
};

enum class SIQSShadowProofRssEvidence : uint8_t {
    unknown,
    not_applicable,
    pass,
    fail,
};

enum class SIQSShadowProofRssFactorIdentity : uint8_t {
    unknown,
    pass,
    fail,
    not_checked,
};

enum class SIQSShadowProofRssGateStatus : uint8_t {
    blocked,
    invalid,
    limit_exceeded,
    manual_review_candidate,
};

enum class SIQSShadowProofRssGateReason : uint8_t {
    policy_missing,
    policy_not_approved,
    policy_budget_missing,
    policy_headroom_missing,
    policy_budget_not_above_headroom,
    policy_binding_invalid,
    sample_count_invalid,
    sample_enum_invalid,
    sample_binding_mismatch,
    sample_fixture_out_of_range,
    sample_ordinal_out_of_range,
    sample_duplicate,
    sample_missing,
    sample_execution_invalid,
    sample_factor_identity_invalid,
    observe_evidence_invalid,
    observe_peak_missing,
    observe_peak_zero,
    observe_peak_over_limit,
    all_observe_peaks_within_limit,
    internal_failure,
};

struct SIQSShadowProofRssCorpusDigest final {
    uint64_t low = 0;
    uint64_t high = 0;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssCorpusDigest&,
               const SIQSShadowProofRssCorpusDigest&) noexcept = default;
};

/// Deployment-owned, per-platform approval. String fields are audit tokens,
/// not free-form text. Evaluation rejects empty, non-ASCII, whitespace,
/// control-character, '=' and over-length values.
struct SIQSShadowProofRssGatePolicy final {
    bool approved = false;
    std::string_view corpus_id;
    SIQSShadowProofRssCorpusDigest corpus_digest;
    SIQSShadowProofRssOperatingSystem operating_system = SIQSShadowProofRssOperatingSystem::unknown;
    SIQSShadowProofRssArchitecture architecture = SIQSShadowProofRssArchitecture::unknown;
    util::ProcessMemoryBackend memory_backend = util::ProcessMemoryBackend::Unsupported;
    std::size_t resolved_production_sieve_workers = 0;
    std::string_view candidate_revision;
    std::string_view approval_id;
    std::optional<uint64_t> deployment_budget_bytes;
    std::optional<uint64_t> reserved_headroom_bytes;
};

/// One already-collected fresh-process sample. This type contains data only;
/// the gate never launches a process, reads live RSS, parses configuration, or
/// calls SIQS. Diagnostic quantities are retained for audit output but never
/// participate in the verdict.
struct SIQSShadowProofRssGateSample final {
    bool policy_approved = false;
    std::string_view corpus_id;
    SIQSShadowProofRssCorpusDigest corpus_digest;
    SIQSShadowProofRssOperatingSystem operating_system = SIQSShadowProofRssOperatingSystem::unknown;
    SIQSShadowProofRssArchitecture architecture = SIQSShadowProofRssArchitecture::unknown;
    util::ProcessMemoryBackend memory_backend = util::ProcessMemoryBackend::Unsupported;
    std::size_t resolved_production_sieve_workers = 0;
    std::string_view candidate_revision;
    std::string_view approval_id;
    std::optional<uint64_t> deployment_budget_bytes;
    std::optional<uint64_t> reserved_headroom_bytes;

    uint32_t fixture_id = 0;
    SIQSShadowProofRssSampleMode mode = SIQSShadowProofRssSampleMode::unknown;
    uint32_t ordinal = 0;
    bool fresh_process = false;
    bool completed = false;
    SIQSShadowProofRssFactorIdentity factor_identity = SIQSShadowProofRssFactorIdentity::unknown;
    SIQSShadowProofRssEvidence proof_evidence = SIQSShadowProofRssEvidence::unknown;
    SIQSShadowProofRssEvidence matrix_evidence = SIQSShadowProofRssEvidence::unknown;
    std::optional<uint64_t> absolute_peak_rss_bytes;

    // Diagnostics only. Mutating any of these fields cannot change a verdict.
    std::optional<int64_t> observe_minus_off_peak_bytes;
    std::optional<uint64_t> current_rss_bytes;
    std::optional<uint64_t> peak_growth_bytes;
    std::optional<uint64_t> wall_ns;
};

struct SIQSShadowProofRssGateOutcome final {
    SIQSShadowProofRssGateStatus status = SIQSShadowProofRssGateStatus::invalid;
    SIQSShadowProofRssGateReason reason = SIQSShadowProofRssGateReason::internal_failure;
    std::size_t total_sample_count = 0;
    uint32_t valid_off_sample_count = 0;
    uint32_t valid_observe_sample_count = 0;
    uint64_t rss_limit_bytes = 0;
    uint64_t max_observe_peak_rss_bytes = 0;
    SIQSShadowProofRssCorpusDigest policy_binding_digest;
    bool shadow_outcome_routed = false;
    bool promotion = false;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssGateOutcome&,
               const SIQSShadowProofRssGateOutcome&) noexcept = default;
};

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_rss_operating_system_name(SIQSShadowProofRssOperatingSystem value) noexcept {
    switch (value) {
    case SIQSShadowProofRssOperatingSystem::unknown:
        return "unknown";
    case SIQSShadowProofRssOperatingSystem::darwin:
        return "darwin";
    case SIQSShadowProofRssOperatingSystem::linux:
        return "linux";
    case SIQSShadowProofRssOperatingSystem::windows:
        return "windows";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_rss_architecture_name(SIQSShadowProofRssArchitecture value) noexcept {
    switch (value) {
    case SIQSShadowProofRssArchitecture::unknown:
        return "unknown";
    case SIQSShadowProofRssArchitecture::x86_64:
        return "x86_64";
    case SIQSShadowProofRssArchitecture::arm64:
        return "arm64";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_rss_sample_mode_name(SIQSShadowProofRssSampleMode value) noexcept {
    switch (value) {
    case SIQSShadowProofRssSampleMode::unknown:
        return "unknown";
    case SIQSShadowProofRssSampleMode::off:
        return "off";
    case SIQSShadowProofRssSampleMode::observe:
        return "observe";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_rss_evidence_name(SIQSShadowProofRssEvidence value) noexcept {
    switch (value) {
    case SIQSShadowProofRssEvidence::unknown:
        return "unknown";
    case SIQSShadowProofRssEvidence::not_applicable:
        return "not_applicable";
    case SIQSShadowProofRssEvidence::pass:
        return "pass";
    case SIQSShadowProofRssEvidence::fail:
        return "fail";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_rss_factor_identity_name(SIQSShadowProofRssFactorIdentity value) noexcept {
    switch (value) {
    case SIQSShadowProofRssFactorIdentity::unknown:
        return "unknown";
    case SIQSShadowProofRssFactorIdentity::pass:
        return "pass";
    case SIQSShadowProofRssFactorIdentity::fail:
        return "fail";
    case SIQSShadowProofRssFactorIdentity::not_checked:
        return "not_checked";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_rss_gate_status_name(SIQSShadowProofRssGateStatus value) noexcept {
    switch (value) {
    case SIQSShadowProofRssGateStatus::blocked:
        return "blocked";
    case SIQSShadowProofRssGateStatus::invalid:
        return "invalid";
    case SIQSShadowProofRssGateStatus::limit_exceeded:
        return "limit_exceeded";
    case SIQSShadowProofRssGateStatus::manual_review_candidate:
        return "manual_review_candidate";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_rss_gate_reason_name(SIQSShadowProofRssGateReason value) noexcept {
    switch (value) {
    case SIQSShadowProofRssGateReason::policy_missing:
        return "policy_missing";
    case SIQSShadowProofRssGateReason::policy_not_approved:
        return "policy_not_approved";
    case SIQSShadowProofRssGateReason::policy_budget_missing:
        return "policy_budget_missing";
    case SIQSShadowProofRssGateReason::policy_headroom_missing:
        return "policy_headroom_missing";
    case SIQSShadowProofRssGateReason::policy_budget_not_above_headroom:
        return "policy_budget_not_above_headroom";
    case SIQSShadowProofRssGateReason::policy_binding_invalid:
        return "policy_binding_invalid";
    case SIQSShadowProofRssGateReason::sample_count_invalid:
        return "sample_count_invalid";
    case SIQSShadowProofRssGateReason::sample_enum_invalid:
        return "sample_enum_invalid";
    case SIQSShadowProofRssGateReason::sample_binding_mismatch:
        return "sample_binding_mismatch";
    case SIQSShadowProofRssGateReason::sample_fixture_out_of_range:
        return "sample_fixture_out_of_range";
    case SIQSShadowProofRssGateReason::sample_ordinal_out_of_range:
        return "sample_ordinal_out_of_range";
    case SIQSShadowProofRssGateReason::sample_duplicate:
        return "sample_duplicate";
    case SIQSShadowProofRssGateReason::sample_missing:
        return "sample_missing";
    case SIQSShadowProofRssGateReason::sample_execution_invalid:
        return "sample_execution_invalid";
    case SIQSShadowProofRssGateReason::sample_factor_identity_invalid:
        return "sample_factor_identity_invalid";
    case SIQSShadowProofRssGateReason::observe_evidence_invalid:
        return "observe_evidence_invalid";
    case SIQSShadowProofRssGateReason::observe_peak_missing:
        return "observe_peak_missing";
    case SIQSShadowProofRssGateReason::observe_peak_zero:
        return "observe_peak_zero";
    case SIQSShadowProofRssGateReason::observe_peak_over_limit:
        return "observe_peak_over_limit";
    case SIQSShadowProofRssGateReason::all_observe_peaks_within_limit:
        return "all_observe_peaks_within_limit";
    case SIQSShadowProofRssGateReason::internal_failure:
        return "internal_failure";
    }
    return "unknown";
}

namespace shadow_proof_rss_gate_detail {

[[nodiscard]] constexpr bool
known_operating_system(SIQSShadowProofRssOperatingSystem value) noexcept {
    return value == SIQSShadowProofRssOperatingSystem::darwin ||
           value == SIQSShadowProofRssOperatingSystem::linux ||
           value == SIQSShadowProofRssOperatingSystem::windows;
}

[[nodiscard]] constexpr bool known_architecture(SIQSShadowProofRssArchitecture value) noexcept {
    return value == SIQSShadowProofRssArchitecture::x86_64 ||
           value == SIQSShadowProofRssArchitecture::arm64;
}

[[nodiscard]] constexpr bool known_mode(SIQSShadowProofRssSampleMode value) noexcept {
    return value == SIQSShadowProofRssSampleMode::off ||
           value == SIQSShadowProofRssSampleMode::observe;
}

[[nodiscard]] constexpr bool known_evidence(SIQSShadowProofRssEvidence value) noexcept {
    return value == SIQSShadowProofRssEvidence::not_applicable ||
           value == SIQSShadowProofRssEvidence::pass || value == SIQSShadowProofRssEvidence::fail;
}

[[nodiscard]] constexpr bool
known_factor_identity(SIQSShadowProofRssFactorIdentity value) noexcept {
    return value == SIQSShadowProofRssFactorIdentity::pass ||
           value == SIQSShadowProofRssFactorIdentity::fail ||
           value == SIQSShadowProofRssFactorIdentity::not_checked;
}

[[nodiscard]] constexpr bool known_backend(util::ProcessMemoryBackend value) noexcept {
    return value == util::ProcessMemoryBackend::Unsupported ||
           value == util::ProcessMemoryBackend::DarwinGetrusage ||
           value == util::ProcessMemoryBackend::LinuxGetrusage ||
           value == util::ProcessMemoryBackend::WindowsPsapi;
}

[[nodiscard]] constexpr bool
backend_matches_operating_system(SIQSShadowProofRssOperatingSystem operating_system,
                                 util::ProcessMemoryBackend backend) noexcept {
    switch (operating_system) {
    case SIQSShadowProofRssOperatingSystem::darwin:
        return backend == util::ProcessMemoryBackend::DarwinGetrusage;
    case SIQSShadowProofRssOperatingSystem::linux:
        return backend == util::ProcessMemoryBackend::LinuxGetrusage;
    case SIQSShadowProofRssOperatingSystem::windows:
        return backend == util::ProcessMemoryBackend::WindowsPsapi;
    case SIQSShadowProofRssOperatingSystem::unknown:
        return false;
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

/// Stable, non-cryptographic fingerprint that closes a terminal outcome over
/// every approved policy field without retaining caller-owned string views.
class PolicyBindingDigestBuilder final {
public:
    constexpr void append_byte(uint8_t value) noexcept {
        low_ ^= static_cast<uint64_t>(value);
        low_ *= UINT64_C(1099511628211);
        high_ ^= static_cast<uint64_t>(value) + byte_index_ * UINT64_C(0x9e3779b97f4a7c15);
        high_ *= UINT64_C(0x94d049bb133111eb);
        high_ ^= high_ >> 29;
        ++byte_index_;
    }

    constexpr void append_bool(bool value) noexcept {
        append_byte(static_cast<uint8_t>(value ? 1 : 0));
    }

    constexpr void append_u64(uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    constexpr void append_string(std::string_view value) noexcept {
        append_u64(static_cast<uint64_t>(value.size()));
        for (const char character : value) {
            append_byte(static_cast<uint8_t>(static_cast<unsigned char>(character)));
        }
    }

    [[nodiscard]] constexpr SIQSShadowProofRssCorpusDigest finish() const noexcept {
        return {low_ ^ (byte_index_ * UINT64_C(0xbf58476d1ce4e5b9)),
                high_ ^ (byte_index_ * UINT64_C(0x94d049bb133111eb))};
    }

private:
    uint64_t low_ = UINT64_C(14695981039346656037);
    uint64_t high_ = UINT64_C(0x243f6a8885a308d3);
    uint64_t byte_index_ = 0;
};

[[nodiscard]] constexpr SIQSShadowProofRssCorpusDigest
policy_binding_digest(const SIQSShadowProofRssGatePolicy& policy) noexcept {
    PolicyBindingDigestBuilder builder;
    builder.append_string("gnfs.siqs.shadow_proof_rss_gate.policy.v1");
    builder.append_bool(policy.approved);
    builder.append_string(policy.corpus_id);
    builder.append_u64(policy.corpus_digest.low);
    builder.append_u64(policy.corpus_digest.high);
    builder.append_string(siqs_shadow_proof_rss_operating_system_name(policy.operating_system));
    builder.append_string(siqs_shadow_proof_rss_architecture_name(policy.architecture));
    builder.append_string(util::process_memory_backend_name(policy.memory_backend));
    builder.append_u64(static_cast<uint64_t>(policy.resolved_production_sieve_workers));
    builder.append_string(policy.candidate_revision);
    builder.append_string(policy.approval_id);
    builder.append_bool(policy.deployment_budget_bytes.has_value());
    builder.append_u64(policy.deployment_budget_bytes.value_or(0));
    builder.append_bool(policy.reserved_headroom_bytes.has_value());
    builder.append_u64(policy.reserved_headroom_bytes.value_or(0));
    return builder.finish();
}

[[nodiscard]] constexpr bool
policy_binding_is_valid(const SIQSShadowProofRssGatePolicy& policy) noexcept {
    return policy.corpus_id == SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_ID &&
           policy.corpus_digest.low == SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_LOW &&
           policy.corpus_digest.high == SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_HIGH &&
           known_operating_system(policy.operating_system) &&
           known_architecture(policy.architecture) && known_backend(policy.memory_backend) &&
           policy.memory_backend != util::ProcessMemoryBackend::Unsupported &&
           backend_matches_operating_system(policy.operating_system, policy.memory_backend) &&
           policy.resolved_production_sieve_workers > 0 && safe_token(policy.candidate_revision) &&
           safe_token(policy.approval_id);
}

[[nodiscard]] constexpr bool
sample_binding_matches_policy(const SIQSShadowProofRssGateSample& sample,
                              const SIQSShadowProofRssGatePolicy& policy) noexcept {
    return sample.policy_approved == policy.approved && sample.corpus_id == policy.corpus_id &&
           sample.corpus_digest == policy.corpus_digest &&
           sample.operating_system == policy.operating_system &&
           sample.architecture == policy.architecture &&
           sample.memory_backend == policy.memory_backend &&
           sample.resolved_production_sieve_workers == policy.resolved_production_sieve_workers &&
           sample.candidate_revision == policy.candidate_revision &&
           sample.approval_id == policy.approval_id &&
           sample.deployment_budget_bytes == policy.deployment_budget_bytes &&
           sample.reserved_headroom_bytes == policy.reserved_headroom_bytes;
}

[[nodiscard]] constexpr SIQSShadowProofRssGateOutcome
outcome(SIQSShadowProofRssGateStatus status, SIQSShadowProofRssGateReason reason,
        std::size_t total_sample_count, uint64_t rss_limit_bytes = 0,
        uint32_t valid_off_sample_count = 0, uint32_t valid_observe_sample_count = 0,
        uint64_t max_observe_peak_rss_bytes = 0,
        SIQSShadowProofRssCorpusDigest policy_digest = {}) noexcept {
    return {status,
            reason,
            total_sample_count,
            valid_off_sample_count,
            valid_observe_sample_count,
            rss_limit_bytes,
            max_observe_peak_rss_bytes,
            policy_digest,
            false,
            false};
}

[[nodiscard]] constexpr bool known_status(SIQSShadowProofRssGateStatus value) noexcept {
    switch (value) {
    case SIQSShadowProofRssGateStatus::blocked:
    case SIQSShadowProofRssGateStatus::invalid:
    case SIQSShadowProofRssGateStatus::limit_exceeded:
    case SIQSShadowProofRssGateStatus::manual_review_candidate:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool known_reason(SIQSShadowProofRssGateReason value) noexcept {
    return siqs_shadow_proof_rss_gate_reason_name(value) != "unknown";
}

[[nodiscard]] constexpr bool
outcome_is_consistent(const SIQSShadowProofRssGateOutcome& value) noexcept {
    if (!known_status(value.status) || !known_reason(value.reason) || value.shadow_outcome_routed ||
        value.promotion ||
        value.total_sample_count != SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT ||
        value.valid_off_sample_count !=
            SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT * SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS ||
        value.valid_observe_sample_count != SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT *
                                                SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS ||
        value.rss_limit_bytes == 0 || value.max_observe_peak_rss_bytes == 0) {
        return false;
    }
    if (value.status == SIQSShadowProofRssGateStatus::manual_review_candidate) {
        return value.reason == SIQSShadowProofRssGateReason::all_observe_peaks_within_limit &&
               value.max_observe_peak_rss_bytes <= value.rss_limit_bytes;
    }
    if (value.status == SIQSShadowProofRssGateStatus::limit_exceeded) {
        return value.reason == SIQSShadowProofRssGateReason::observe_peak_over_limit &&
               value.max_observe_peak_rss_bytes > value.rss_limit_bytes;
    }
    return false;
}

} // namespace shadow_proof_rss_gate_detail

/// Return the first fail-closed policy error, or `std::nullopt` when the
/// complete policy is approved and structurally valid. This is the shared
/// policy boundary for evidence evaluation and pre-execution campaign
/// planning; neither caller may weaken or reorder it independently.
[[nodiscard]] constexpr std::optional<SIQSShadowProofRssGateReason>
siqs_shadow_proof_rss_gate_policy_error(const SIQSShadowProofRssGatePolicy* policy) noexcept {
    if (policy == nullptr) {
        return SIQSShadowProofRssGateReason::policy_missing;
    }
    if (!policy->approved) {
        return SIQSShadowProofRssGateReason::policy_not_approved;
    }
    if (!policy->deployment_budget_bytes.has_value()) {
        return SIQSShadowProofRssGateReason::policy_budget_missing;
    }
    if (!policy->reserved_headroom_bytes.has_value()) {
        return SIQSShadowProofRssGateReason::policy_headroom_missing;
    }
    if (*policy->deployment_budget_bytes <= *policy->reserved_headroom_bytes) {
        return SIQSShadowProofRssGateReason::policy_budget_not_above_headroom;
    }
    if (!shadow_proof_rss_gate_detail::policy_binding_is_valid(*policy)) {
        return SIQSShadowProofRssGateReason::policy_binding_invalid;
    }
    return std::nullopt;
}

/// Stable, non-cryptographic identity checksum over every policy field. The
/// policy must remain alive only for the duration of this call; no string view
/// is retained.
[[nodiscard]] constexpr SIQSShadowProofRssCorpusDigest
siqs_shadow_proof_rss_policy_binding_digest(const SIQSShadowProofRssGatePolicy& policy) noexcept {
    return shadow_proof_rss_gate_detail::policy_binding_digest(policy);
}

/// Evaluate already-collected evidence only. A null policy represents the
/// absence of an approved per-platform policy. Structural validation always
/// completes before the limit comparison, so malformed evidence cannot be
/// reported as a simple threshold failure.
[[nodiscard]] inline SIQSShadowProofRssGateOutcome evaluate_siqs_shadow_proof_rss_gate(
    const SIQSShadowProofRssGatePolicy* policy,
    std::span<const SIQSShadowProofRssGateSample> samples) noexcept {
    using namespace shadow_proof_rss_gate_detail;

    const std::size_t sample_count = samples.size();
    if (const auto policy_error = siqs_shadow_proof_rss_gate_policy_error(policy)) {
        const bool blocked = *policy_error == SIQSShadowProofRssGateReason::policy_missing ||
                             *policy_error == SIQSShadowProofRssGateReason::policy_not_approved ||
                             *policy_error == SIQSShadowProofRssGateReason::policy_budget_missing ||
                             *policy_error == SIQSShadowProofRssGateReason::policy_headroom_missing;
        uint64_t policy_rss_limit_bytes = 0;
        if (*policy_error == SIQSShadowProofRssGateReason::policy_binding_invalid) {
            policy_rss_limit_bytes =
                *policy->deployment_budget_bytes - *policy->reserved_headroom_bytes;
        }
        return outcome(blocked ? SIQSShadowProofRssGateStatus::blocked
                               : SIQSShadowProofRssGateStatus::invalid,
                       *policy_error, sample_count, policy_rss_limit_bytes);
    }
    const uint64_t rss_limit_bytes =
        *policy->deployment_budget_bytes - *policy->reserved_headroom_bytes;
    if (sample_count != SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT) {
        return outcome(SIQSShadowProofRssGateStatus::invalid,
                       SIQSShadowProofRssGateReason::sample_count_invalid, sample_count,
                       rss_limit_bytes);
    }

    std::array<bool, SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT> occupied{};
    uint32_t off_count = 0;
    uint32_t observe_count = 0;
    uint64_t max_observe_peak = 0;

    for (const auto& sample : samples) {
        if (!known_mode(sample.mode) || !known_evidence(sample.proof_evidence) ||
            !known_evidence(sample.matrix_evidence) ||
            !known_factor_identity(sample.factor_identity) ||
            !known_operating_system(sample.operating_system) ||
            !known_architecture(sample.architecture) || !known_backend(sample.memory_backend)) {
            return outcome(SIQSShadowProofRssGateStatus::invalid,
                           SIQSShadowProofRssGateReason::sample_enum_invalid, sample_count,
                           rss_limit_bytes, off_count, observe_count, max_observe_peak);
        }
        if (!sample_binding_matches_policy(sample, *policy)) {
            return outcome(SIQSShadowProofRssGateStatus::invalid,
                           SIQSShadowProofRssGateReason::sample_binding_mismatch, sample_count,
                           rss_limit_bytes, off_count, observe_count, max_observe_peak);
        }
        if (sample.fixture_id == 0 ||
            sample.fixture_id > SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT) {
            return outcome(SIQSShadowProofRssGateStatus::invalid,
                           SIQSShadowProofRssGateReason::sample_fixture_out_of_range, sample_count,
                           rss_limit_bytes, off_count, observe_count, max_observe_peak);
        }

        const uint32_t repetition_count = sample.mode == SIQSShadowProofRssSampleMode::off
                                              ? SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS
                                              : SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS;
        if (sample.ordinal == 0 || sample.ordinal > repetition_count) {
            return outcome(SIQSShadowProofRssGateStatus::invalid,
                           SIQSShadowProofRssGateReason::sample_ordinal_out_of_range, sample_count,
                           rss_limit_bytes, off_count, observe_count, max_observe_peak);
        }

        const std::size_t fixture_offset = static_cast<std::size_t>(sample.fixture_id - 1) *
                                           (SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS +
                                            SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS);
        const std::size_t mode_offset = sample.mode == SIQSShadowProofRssSampleMode::off
                                            ? 0
                                            : SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS;
        const std::size_t slot = fixture_offset + mode_offset + sample.ordinal - 1;
        if (occupied[slot]) {
            return outcome(SIQSShadowProofRssGateStatus::invalid,
                           SIQSShadowProofRssGateReason::sample_duplicate, sample_count,
                           rss_limit_bytes, off_count, observe_count, max_observe_peak);
        }
        occupied[slot] = true;

        if (!sample.fresh_process || !sample.completed) {
            return outcome(SIQSShadowProofRssGateStatus::invalid,
                           SIQSShadowProofRssGateReason::sample_execution_invalid, sample_count,
                           rss_limit_bytes, off_count, observe_count, max_observe_peak);
        }
        if (sample.factor_identity != SIQSShadowProofRssFactorIdentity::pass) {
            return outcome(SIQSShadowProofRssGateStatus::invalid,
                           SIQSShadowProofRssGateReason::sample_factor_identity_invalid,
                           sample_count, rss_limit_bytes, off_count, observe_count,
                           max_observe_peak);
        }

        if (sample.mode == SIQSShadowProofRssSampleMode::off) {
            ++off_count;
            continue;
        }

        if (sample.proof_evidence != SIQSShadowProofRssEvidence::pass ||
            sample.matrix_evidence != SIQSShadowProofRssEvidence::pass) {
            return outcome(SIQSShadowProofRssGateStatus::invalid,
                           SIQSShadowProofRssGateReason::observe_evidence_invalid, sample_count,
                           rss_limit_bytes, off_count, observe_count, max_observe_peak);
        }
        if (!sample.absolute_peak_rss_bytes.has_value()) {
            return outcome(SIQSShadowProofRssGateStatus::invalid,
                           SIQSShadowProofRssGateReason::observe_peak_missing, sample_count,
                           rss_limit_bytes, off_count, observe_count, max_observe_peak);
        }
        if (*sample.absolute_peak_rss_bytes == 0) {
            return outcome(SIQSShadowProofRssGateStatus::invalid,
                           SIQSShadowProofRssGateReason::observe_peak_zero, sample_count,
                           rss_limit_bytes, off_count, observe_count, max_observe_peak);
        }
        if (*sample.absolute_peak_rss_bytes > max_observe_peak) {
            max_observe_peak = *sample.absolute_peak_rss_bytes;
        }
        ++observe_count;
    }

    for (const bool present : occupied) {
        if (!present) {
            return outcome(SIQSShadowProofRssGateStatus::invalid,
                           SIQSShadowProofRssGateReason::sample_missing, sample_count,
                           rss_limit_bytes, off_count, observe_count, max_observe_peak);
        }
    }
    if (off_count !=
            SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT * SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS ||
        observe_count != SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT *
                             SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS) {
        return outcome(SIQSShadowProofRssGateStatus::invalid,
                       SIQSShadowProofRssGateReason::sample_missing, sample_count, rss_limit_bytes,
                       off_count, observe_count, max_observe_peak);
    }

    if (max_observe_peak > rss_limit_bytes) {
        return outcome(SIQSShadowProofRssGateStatus::limit_exceeded,
                       SIQSShadowProofRssGateReason::observe_peak_over_limit, sample_count,
                       rss_limit_bytes, off_count, observe_count, max_observe_peak,
                       policy_binding_digest(*policy));
    }
    return outcome(SIQSShadowProofRssGateStatus::manual_review_candidate,
                   SIQSShadowProofRssGateReason::all_observe_peaks_within_limit, sample_count,
                   rss_limit_bytes, off_count, observe_count, max_observe_peak,
                   policy_binding_digest(*policy));
}

/// Emit one closed audit line only after recomputing and matching the supplied
/// outcome. Success is a write-and-flush commit point for this record only; it
/// never authorizes routing or promotion. Any exception or I/O failure returns
/// false.
[[nodiscard]] inline bool emit_siqs_shadow_proof_rss_gate_outcome(
    std::FILE* output, const SIQSShadowProofRssGatePolicy* policy,
    std::span<const SIQSShadowProofRssGateSample> samples,
    const SIQSShadowProofRssGateOutcome& supplied_outcome) noexcept {
    if (output == nullptr || policy == nullptr || std::ferror(output) != 0) {
        return false;
    }
    try {
        const SIQSShadowProofRssGateOutcome verified =
            evaluate_siqs_shadow_proof_rss_gate(policy, samples);
        if (verified != supplied_outcome ||
            !shadow_proof_rss_gate_detail::outcome_is_consistent(verified) ||
            !shadow_proof_rss_gate_detail::policy_binding_is_valid(*policy)) {
            return false;
        }

        const std::string_view status = siqs_shadow_proof_rss_gate_status_name(verified.status);
        const std::string_view reason = siqs_shadow_proof_rss_gate_reason_name(verified.reason);
        const std::string_view operating_system =
            siqs_shadow_proof_rss_operating_system_name(policy->operating_system);
        const std::string_view architecture =
            siqs_shadow_proof_rss_architecture_name(policy->architecture);
        const std::string_view backend = util::process_memory_backend_name(policy->memory_backend);

        const int written = std::fprintf(
            output,
            "%s schema_version=%" PRIu32
            " status=%.*s reason=%.*s corpus_id=%.*s digest_low=%" PRIu64 " digest_high=%" PRIu64
            " operating_system=%.*s architecture=%.*s memory_backend=%.*s"
            " resolved_production_sieve_workers=%zu candidate_revision=%.*s"
            " approval_id=%.*s deployment_budget_bytes=%" PRIu64 " reserved_headroom_bytes=%" PRIu64
            " rss_limit_bytes=%" PRIu64 " total_samples=%zu off_samples=%" PRIu32
            " observe_samples=%" PRIu32 " max_observe_peak_rss_bytes=%" PRIu64
            " policy_binding_digest_low=%" PRIu64 " policy_binding_digest_high=%" PRIu64
            " gate_quantity=observe_absolute_process_peak_rss"
            " shadow_outcome_routed=false promotion=false\n",
            SIQS_SHADOW_PROOF_RSS_GATE_RECORD_PREFIX, SIQS_SHADOW_PROOF_RSS_GATE_SCHEMA_VERSION,
            static_cast<int>(status.size()), status.data(), static_cast<int>(reason.size()),
            reason.data(), static_cast<int>(policy->corpus_id.size()), policy->corpus_id.data(),
            policy->corpus_digest.low, policy->corpus_digest.high,
            static_cast<int>(operating_system.size()), operating_system.data(),
            static_cast<int>(architecture.size()), architecture.data(),
            static_cast<int>(backend.size()), backend.data(),
            policy->resolved_production_sieve_workers,
            static_cast<int>(policy->candidate_revision.size()), policy->candidate_revision.data(),
            static_cast<int>(policy->approval_id.size()), policy->approval_id.data(),
            *policy->deployment_budget_bytes, *policy->reserved_headroom_bytes,
            verified.rss_limit_bytes, verified.total_sample_count, verified.valid_off_sample_count,
            verified.valid_observe_sample_count, verified.max_observe_peak_rss_bytes,
            verified.policy_binding_digest.low, verified.policy_binding_digest.high);
        if (written < 0 || std::fflush(output) != 0) {
            return false;
        }
        return std::ferror(output) == 0;
    } catch (...) {
        return false;
    }
}

} // namespace gnfs::siqs
