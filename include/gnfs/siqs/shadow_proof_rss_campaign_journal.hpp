#pragma once

/// @file shadow_proof_rss_campaign_journal.hpp
/// @brief Pure, write-once replay contract for an approved SIQS RSS campaign.

#include <gnfs/siqs/shadow_proof_rss_campaign.hpp>
#include <gnfs/siqs/shadow_proof_rss_probe_execution_identity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace gnfs::siqs {

namespace shadow_proof_rss_campaign_journal_store_detail {
class SessionCore;
}

inline constexpr uint32_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION = 3;

enum class SIQSShadowProofRssJournalPresence : uint8_t {
    absent,
    present,
};

enum class SIQSShadowProofRssJournalStatus : uint8_t {
    blocked,
    invalid,
    ready,
    tainted,
    complete,
};

enum class SIQSShadowProofRssJournalReason : uint8_t {
    policy_missing,
    policy_not_approved,
    policy_budget_missing,
    policy_headroom_missing,
    policy_budget_not_above_headroom,
    policy_binding_invalid,
    runtime_facts_missing,
    runtime_facts_invalid,
    runtime_facts_mismatch,
    release_ndebug_required,
    journal_presence_invalid,
    absent_journal_has_state,
    present_journal_missing_header,
    header_invalid,
    record_invalid,
    record_order_invalid,
    committed_sample_invalid,
    dangling_slot_start,
    explicitly_tainted,
    synthetic_complete,
    ready,
    complete,
};

enum class SIQSShadowProofRssJournalAction : uint8_t {
    none,
    create_header,
    append_slot_start,
    append_taint,
    evaluate_gate,
};

enum class SIQSShadowProofRssJournalRecordKind : uint8_t {
    unknown,
    slot_started,
    slot_committed,
    campaign_tainted,
};

enum class SIQSShadowProofRssArtifactKind : uint8_t {
    unknown,
    probe_stdout,
    probe_stderr,
    joined_gate_sample,
};

/// Deployment classification claimed by the campaign runtime facts and later
/// cross-checked against the private deployment row for every launched slot.
/// This tag is not executable-image authentication. Only
/// `production_holdout` data can reach the data-level gate reconstruction;
/// `synthetic_test` exists solely for private transaction tests.
enum class SIQSShadowProofRssProbeKind : uint8_t {
    unknown,
    synthetic_test,
    production_holdout,
};

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_rss_probe_kind_name(SIQSShadowProofRssProbeKind value) noexcept {
    switch (value) {
    case SIQSShadowProofRssProbeKind::unknown:
        return "unknown";
    case SIQSShadowProofRssProbeKind::synthetic_test:
        return "synthetic_test";
    case SIQSShadowProofRssProbeKind::production_holdout:
        return "production_holdout";
    }
    return "unknown";
}

/// Values observed by the future launcher before it is allowed to open a
/// journal. The probe kind is a caller claim that the private store/runner
/// boundary must cross-check against its deployment row. The candidate
/// revision is a borrowed audit token. The execution identity is also a
/// deployment claim until a platform launch boundary authenticates the exact
/// executable object.
struct SIQSShadowProofRssCampaignRuntimeFacts final {
    SIQSShadowProofRssOperatingSystem operating_system = SIQSShadowProofRssOperatingSystem::unknown;
    SIQSShadowProofRssArchitecture architecture = SIQSShadowProofRssArchitecture::unknown;
    util::ProcessMemoryBackend memory_backend = util::ProcessMemoryBackend::Unsupported;
    std::size_t resolved_production_sieve_workers = 0;
    SIQSShadowProofRssProbeKind probe_kind = SIQSShadowProofRssProbeKind::unknown;
    std::string_view candidate_revision;
    SIQSShadowProofRssProbeExecutionIdentity probe_execution_identity;
    bool release_build = false;
    bool ndebug = false;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssCampaignRuntimeFacts&,
               const SIQSShadowProofRssCampaignRuntimeFacts&) noexcept = default;
};

struct SIQSShadowProofRssArtifactSeal final {
    bool committed = false;
    SIQSShadowProofRssArtifactKind kind = SIQSShadowProofRssArtifactKind::unknown;
    uint64_t byte_count = 0;
    SIQSShadowProofRssCorpusDigest digest;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssArtifactSeal&,
               const SIQSShadowProofRssArtifactSeal&) noexcept = default;
};

/// Owning, string-free sample state stored by a commit. Policy strings are
/// reconstructed from the validated policy only after replay succeeds.
struct SIQSShadowProofRssJournalCommitPayload final {
    SIQSShadowProofRssOperatingSystem actual_operating_system =
        SIQSShadowProofRssOperatingSystem::unknown;
    SIQSShadowProofRssArchitecture actual_architecture = SIQSShadowProofRssArchitecture::unknown;
    util::ProcessMemoryBackend actual_memory_backend = util::ProcessMemoryBackend::Unsupported;
    std::size_t actual_resolved_sieve_workers = 0;
    SIQSShadowProofRssProbeKind deployment_probe_kind = SIQSShadowProofRssProbeKind::unknown;
    bool fresh_process = false;
    bool completed = false;
    SIQSShadowProofRssFactorIdentity factor_identity = SIQSShadowProofRssFactorIdentity::unknown;
    SIQSShadowProofRssEvidence proof_evidence = SIQSShadowProofRssEvidence::unknown;
    SIQSShadowProofRssEvidence matrix_evidence = SIQSShadowProofRssEvidence::unknown;
    std::optional<uint64_t> absolute_peak_rss_bytes;
    std::optional<int64_t> observe_minus_off_peak_bytes;
    std::optional<uint64_t> current_rss_bytes;
    std::optional<uint64_t> peak_growth_bytes;
    std::optional<uint64_t> wall_ns;
    SIQSShadowProofRssArtifactSeal stdout_seal;
    SIQSShadowProofRssArtifactSeal stderr_seal;
    SIQSShadowProofRssArtifactSeal joined_sample_seal;
    SIQSShadowProofRssProbeExecutionIdentity probe_execution_identity;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssJournalCommitPayload&,
               const SIQSShadowProofRssJournalCommitPayload&) noexcept = default;
};

struct SIQSShadowProofRssCampaignJournalHeader final {
    uint32_t schema_version = 0;
    SIQSShadowProofRssProbeKind probe_kind = SIQSShadowProofRssProbeKind::unknown;
    SIQSShadowProofRssCorpusDigest policy_binding_digest;
    SIQSShadowProofRssCorpusDigest runtime_facts_digest;
    SIQSShadowProofRssCorpusDigest plan_digest;
    uint32_t slot_count = 0;
    uint32_t max_concurrency = 0;
    SIQSShadowProofRssProbeExecutionIdentity probe_execution_identity;
    SIQSShadowProofRssCorpusDigest header_digest;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssCampaignJournalHeader&,
               const SIQSShadowProofRssCampaignJournalHeader&) noexcept = default;
};

struct SIQSShadowProofRssCampaignJournalRecord final {
    uint32_t schema_version = 0;
    uint32_t sequence_number = 0;
    SIQSShadowProofRssJournalRecordKind kind = SIQSShadowProofRssJournalRecordKind::unknown;
    SIQSShadowProofRssCorpusDigest previous_record_digest;
    SIQSShadowProofRssCorpusDigest plan_digest;
    uint32_t slot_number = 0;
    SIQSShadowProofRssCorpusDigest slot_digest;
    SIQSShadowProofRssJournalCommitPayload commit_payload;
    SIQSShadowProofRssCorpusDigest record_digest;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssCampaignJournalRecord&,
               const SIQSShadowProofRssCampaignJournalRecord&) noexcept = default;
};

/// Move-only proof that the storage layer durably published one exact journal
/// record. Ordinary callers cannot construct this capability. A receipt binds
/// the complete typed record, including its plan, sequence, and digest, so a
/// stale or mismatched receipt cannot authorize a process launch.
class SIQSShadowProofRssDurableRecordReceipt final {
public:
    SIQSShadowProofRssDurableRecordReceipt() = delete;
    SIQSShadowProofRssDurableRecordReceipt(const SIQSShadowProofRssDurableRecordReceipt&) = delete;
    SIQSShadowProofRssDurableRecordReceipt&
    operator=(const SIQSShadowProofRssDurableRecordReceipt&) = delete;

    constexpr SIQSShadowProofRssDurableRecordReceipt(
        SIQSShadowProofRssDurableRecordReceipt&& other) noexcept
        : record_(other.record_), active_(std::exchange(other.active_, false)) {}

    SIQSShadowProofRssDurableRecordReceipt&
    operator=(SIQSShadowProofRssDurableRecordReceipt&&) = delete;

    [[nodiscard]] constexpr const SIQSShadowProofRssCampaignJournalRecord& record() const noexcept {
        return record_;
    }

    [[nodiscard]] constexpr bool active() const noexcept {
        return active_;
    }

private:
    explicit constexpr SIQSShadowProofRssDurableRecordReceipt(
        SIQSShadowProofRssCampaignJournalRecord record) noexcept
        : record_(record), active_(true) {}

    SIQSShadowProofRssCampaignJournalRecord record_;
    bool active_ = false;

    friend constexpr std::optional<class SIQSShadowProofRssLaunchPermit>
    acknowledge_siqs_shadow_proof_rss_durable_slot_start(
        class SIQSShadowProofRssPreparedSlotStart&&,
        SIQSShadowProofRssDurableRecordReceipt&&) noexcept;
    friend class shadow_proof_rss_campaign_journal_store_detail::SessionCore;
};

struct SIQSShadowProofRssCampaignJournalResume;

[[nodiscard]] constexpr SIQSShadowProofRssCampaignJournalResume
resume_siqs_shadow_proof_rss_campaign_journal(
    const SIQSShadowProofRssGatePolicy*, const SIQSShadowProofRssCampaignRuntimeFacts*,
    SIQSShadowProofRssJournalPresence, const SIQSShadowProofRssCampaignJournalHeader*,
    std::span<const SIQSShadowProofRssCampaignJournalRecord>) noexcept;

/// Move-only capability created only by a successful replay. The future
/// storage layer may inspect the proposed record, but must consume this value
/// when it acknowledges that the exact bytes are durable.
class SIQSShadowProofRssPreparedSlotStart final {
public:
    SIQSShadowProofRssPreparedSlotStart() = delete;
    SIQSShadowProofRssPreparedSlotStart(const SIQSShadowProofRssPreparedSlotStart&) = delete;
    SIQSShadowProofRssPreparedSlotStart&
    operator=(const SIQSShadowProofRssPreparedSlotStart&) = delete;

    constexpr SIQSShadowProofRssPreparedSlotStart(
        SIQSShadowProofRssPreparedSlotStart&& other) noexcept
        : record_(other.record_), active_(std::exchange(other.active_, false)) {}

    // Unlike durable receipts and launch permits, this is a replaceable,
    // pre-publication replay proposal. Refresh may intentionally replace it
    // with an equivalent proposal before any execution authority exists.
    constexpr SIQSShadowProofRssPreparedSlotStart&
    operator=(SIQSShadowProofRssPreparedSlotStart&& other) noexcept {
        if (this != &other) {
            record_ = other.record_;
            active_ = std::exchange(other.active_, false);
        }
        return *this;
    }

    [[nodiscard]] constexpr const SIQSShadowProofRssCampaignJournalRecord& record() const noexcept {
        return record_;
    }

    [[nodiscard]] constexpr bool active() const noexcept {
        return active_;
    }

private:
    explicit constexpr SIQSShadowProofRssPreparedSlotStart(
        SIQSShadowProofRssCampaignJournalRecord record) noexcept
        : record_(record), active_(true) {}

    SIQSShadowProofRssCampaignJournalRecord record_;
    bool active_ = false;

    friend constexpr SIQSShadowProofRssCampaignJournalResume
    resume_siqs_shadow_proof_rss_campaign_journal(
        const SIQSShadowProofRssGatePolicy*, const SIQSShadowProofRssCampaignRuntimeFacts*,
        SIQSShadowProofRssJournalPresence, const SIQSShadowProofRssCampaignJournalHeader*,
        std::span<const SIQSShadowProofRssCampaignJournalRecord>) noexcept;

    friend constexpr std::optional<class SIQSShadowProofRssLaunchPermit>
    acknowledge_siqs_shadow_proof_rss_durable_slot_start(
        SIQSShadowProofRssPreparedSlotStart&&, SIQSShadowProofRssDurableRecordReceipt&&) noexcept;
};

struct SIQSShadowProofRssCampaignJournalResume final {
    SIQSShadowProofRssJournalStatus status = SIQSShadowProofRssJournalStatus::invalid;
    SIQSShadowProofRssJournalReason reason = SIQSShadowProofRssJournalReason::record_invalid;
    SIQSShadowProofRssJournalAction action = SIQSShadowProofRssJournalAction::none;
    uint32_t committed_slot_count = 0;
    uint32_t next_slot_number = 0;
    SIQSShadowProofRssCorpusDigest plan_digest;
    std::optional<SIQSShadowProofRssCampaignJournalHeader> header_to_create;
    std::optional<SIQSShadowProofRssPreparedSlotStart> prepared_slot_start;
    std::optional<SIQSShadowProofRssCampaignJournalRecord> taint_to_append;
    std::array<SIQSShadowProofRssJournalCommitPayload,
               SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT>
        committed_payloads{};
};

class SIQSShadowProofRssLaunchPermit final {
public:
    SIQSShadowProofRssLaunchPermit() = delete;
    SIQSShadowProofRssLaunchPermit(const SIQSShadowProofRssLaunchPermit&) = delete;
    SIQSShadowProofRssLaunchPermit& operator=(const SIQSShadowProofRssLaunchPermit&) = delete;
    constexpr SIQSShadowProofRssLaunchPermit(SIQSShadowProofRssLaunchPermit&& other) noexcept
        : durable_start_record_(other.durable_start_record_),
          active_(std::exchange(other.active_, false)) {}

    SIQSShadowProofRssLaunchPermit& operator=(SIQSShadowProofRssLaunchPermit&&) = delete;

    [[nodiscard]] constexpr const SIQSShadowProofRssCampaignJournalRecord&
    durable_start_record() const noexcept {
        return durable_start_record_;
    }

    [[nodiscard]] constexpr bool active() const noexcept {
        return active_;
    }

private:
    explicit constexpr SIQSShadowProofRssLaunchPermit(
        SIQSShadowProofRssCampaignJournalRecord durable_start_record) noexcept
        : durable_start_record_(durable_start_record), active_(true) {}

    SIQSShadowProofRssCampaignJournalRecord durable_start_record_;
    bool active_ = false;

    friend constexpr std::optional<SIQSShadowProofRssLaunchPermit>
    acknowledge_siqs_shadow_proof_rss_durable_slot_start(
        SIQSShadowProofRssPreparedSlotStart&&, SIQSShadowProofRssDurableRecordReceipt&&) noexcept;

    friend constexpr std::optional<SIQSShadowProofRssCampaignJournalRecord>
    make_siqs_shadow_proof_rss_slot_commit(SIQSShadowProofRssLaunchPermit&&,
                                           const SIQSShadowProofRssGatePolicy*,
                                           const SIQSShadowProofRssJournalCommitPayload&) noexcept;
};

namespace shadow_proof_rss_campaign_journal_detail {

class DigestBuilder final {
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

    constexpr void append_i64(int64_t value) noexcept {
        append_u64(static_cast<uint64_t>(value));
    }

    constexpr void append_string(std::string_view value) noexcept {
        append_u64(static_cast<uint64_t>(value.size()));
        for (const char character : value) {
            append_byte(static_cast<uint8_t>(static_cast<unsigned char>(character)));
        }
    }

    constexpr void append_digest(SIQSShadowProofRssCorpusDigest value) noexcept {
        append_u64(value.low);
        append_u64(value.high);
    }

    constexpr void append_sha256_digest(const util::Sha256Digest& value) noexcept {
        for (const std::byte byte : value.bytes) {
            append_byte(std::to_integer<uint8_t>(byte));
        }
    }

    constexpr void append_probe_execution_identity(
        const SIQSShadowProofRssProbeExecutionIdentity& identity) noexcept {
        append_sha256_digest(identity.executable_sha256);
        append_sha256_digest(identity.execution_contract_sha256);
    }

    template <typename T>
    constexpr void append_optional_unsigned(const std::optional<T>& value) noexcept {
        append_bool(value.has_value());
        append_u64(value.has_value() ? static_cast<uint64_t>(*value) : 0);
    }

    constexpr void append_optional_signed(const std::optional<int64_t>& value) noexcept {
        append_bool(value.has_value());
        append_i64(value.value_or(0));
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

[[nodiscard]] constexpr bool digest_is_nonzero(SIQSShadowProofRssCorpusDigest digest) noexcept {
    return digest.low != 0 || digest.high != 0;
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

[[nodiscard]] constexpr bool
runtime_facts_are_valid(const SIQSShadowProofRssCampaignRuntimeFacts& facts) noexcept {
    const bool operating_system_known =
        facts.operating_system == SIQSShadowProofRssOperatingSystem::darwin ||
        facts.operating_system == SIQSShadowProofRssOperatingSystem::linux ||
        facts.operating_system == SIQSShadowProofRssOperatingSystem::windows;
    const bool architecture_known = facts.architecture == SIQSShadowProofRssArchitecture::x86_64 ||
                                    facts.architecture == SIQSShadowProofRssArchitecture::arm64;
    const bool backend_matches =
        (facts.operating_system == SIQSShadowProofRssOperatingSystem::darwin &&
         facts.memory_backend == util::ProcessMemoryBackend::DarwinGetrusage) ||
        (facts.operating_system == SIQSShadowProofRssOperatingSystem::linux &&
         facts.memory_backend == util::ProcessMemoryBackend::LinuxGetrusage) ||
        (facts.operating_system == SIQSShadowProofRssOperatingSystem::windows &&
         facts.memory_backend == util::ProcessMemoryBackend::WindowsPsapi);
    const bool probe_kind_known =
        facts.probe_kind == SIQSShadowProofRssProbeKind::synthetic_test ||
        facts.probe_kind == SIQSShadowProofRssProbeKind::production_holdout;
    return operating_system_known && architecture_known && backend_matches && probe_kind_known &&
           facts.resolved_production_sieve_workers != 0 && safe_token(facts.candidate_revision) &&
           siqs_shadow_proof_rss_probe_execution_identity_is_valid(facts.probe_execution_identity);
}

[[nodiscard]] constexpr bool
runtime_facts_match_policy(const SIQSShadowProofRssCampaignRuntimeFacts& facts,
                           const SIQSShadowProofRssGatePolicy& policy) noexcept {
    return facts.operating_system == policy.operating_system &&
           facts.architecture == policy.architecture &&
           facts.memory_backend == policy.memory_backend &&
           facts.resolved_production_sieve_workers == policy.resolved_production_sieve_workers &&
           facts.candidate_revision == policy.candidate_revision &&
           facts.probe_execution_identity == policy.probe_execution_identity;
}

[[nodiscard]] constexpr SIQSShadowProofRssCorpusDigest
runtime_facts_digest(const SIQSShadowProofRssCampaignRuntimeFacts& facts) noexcept {
    DigestBuilder builder;
    builder.append_string("gnfs.siqs.shadow_proof_rss_campaign.runtime_facts.v3");
    builder.append_u64(static_cast<uint64_t>(facts.operating_system));
    builder.append_u64(static_cast<uint64_t>(facts.architecture));
    builder.append_u64(static_cast<uint64_t>(facts.memory_backend));
    builder.append_u64(static_cast<uint64_t>(facts.resolved_production_sieve_workers));
    builder.append_u64(static_cast<uint64_t>(facts.probe_kind));
    builder.append_string(facts.candidate_revision);
    builder.append_probe_execution_identity(facts.probe_execution_identity);
    builder.append_bool(facts.release_build);
    builder.append_bool(facts.ndebug);
    return builder.finish();
}

[[nodiscard]] constexpr SIQSShadowProofRssCorpusDigest
plan_digest(const SIQSShadowProofRssCampaignPlan& plan,
            SIQSShadowProofRssCorpusDigest policy_digest,
            SIQSShadowProofRssCorpusDigest facts_digest) noexcept {
    DigestBuilder builder;
    builder.append_string("gnfs.siqs.shadow_proof_rss_campaign.plan.v3");
    builder.append_digest(policy_digest);
    builder.append_digest(facts_digest);
    builder.append_u64(static_cast<uint64_t>(plan.slot_count));
    builder.append_u64(static_cast<uint64_t>(plan.max_concurrency));
    for (std::size_t index = 0; index < plan.slot_count; ++index) {
        const auto& slot = plan.slots[index];
        builder.append_u64(slot.slot_number);
        builder.append_u64(slot.fixture_id);
        builder.append_u64(static_cast<uint64_t>(slot.mode));
        builder.append_u64(slot.ordinal);
        builder.append_digest(slot.policy_binding_digest);
        builder.append_probe_execution_identity(slot.probe_execution_identity);
    }
    return builder.finish();
}

[[nodiscard]] constexpr SIQSShadowProofRssCorpusDigest
slot_digest(SIQSShadowProofRssCorpusDigest plan_digest_value,
            const SIQSShadowProofRssCampaignSlot& slot) noexcept {
    DigestBuilder builder;
    builder.append_string("gnfs.siqs.shadow_proof_rss_campaign.slot.v3");
    builder.append_digest(plan_digest_value);
    builder.append_u64(slot.slot_number);
    builder.append_u64(slot.fixture_id);
    builder.append_u64(static_cast<uint64_t>(slot.mode));
    builder.append_u64(slot.ordinal);
    builder.append_digest(slot.policy_binding_digest);
    builder.append_probe_execution_identity(slot.probe_execution_identity);
    return builder.finish();
}

[[nodiscard]] constexpr SIQSShadowProofRssCorpusDigest
header_digest(const SIQSShadowProofRssCampaignJournalHeader& header) noexcept {
    DigestBuilder builder;
    builder.append_string("gnfs.siqs.shadow_proof_rss_campaign.journal_header.v3");
    builder.append_u64(header.schema_version);
    builder.append_u64(static_cast<uint64_t>(header.probe_kind));
    builder.append_digest(header.policy_binding_digest);
    builder.append_digest(header.runtime_facts_digest);
    builder.append_digest(header.plan_digest);
    builder.append_u64(header.slot_count);
    builder.append_u64(header.max_concurrency);
    builder.append_probe_execution_identity(header.probe_execution_identity);
    return builder.finish();
}

constexpr void append_artifact_seal(DigestBuilder& builder,
                                    const SIQSShadowProofRssArtifactSeal& seal) noexcept {
    builder.append_bool(seal.committed);
    builder.append_u64(static_cast<uint64_t>(seal.kind));
    builder.append_u64(seal.byte_count);
    builder.append_digest(seal.digest);
}

constexpr void
append_commit_payload(DigestBuilder& builder,
                      const SIQSShadowProofRssJournalCommitPayload& payload) noexcept {
    builder.append_u64(static_cast<uint64_t>(payload.actual_operating_system));
    builder.append_u64(static_cast<uint64_t>(payload.actual_architecture));
    builder.append_u64(static_cast<uint64_t>(payload.actual_memory_backend));
    builder.append_u64(static_cast<uint64_t>(payload.actual_resolved_sieve_workers));
    builder.append_u64(static_cast<uint64_t>(payload.deployment_probe_kind));
    builder.append_bool(payload.fresh_process);
    builder.append_bool(payload.completed);
    builder.append_u64(static_cast<uint64_t>(payload.factor_identity));
    builder.append_u64(static_cast<uint64_t>(payload.proof_evidence));
    builder.append_u64(static_cast<uint64_t>(payload.matrix_evidence));
    builder.append_optional_unsigned(payload.absolute_peak_rss_bytes);
    builder.append_optional_signed(payload.observe_minus_off_peak_bytes);
    builder.append_optional_unsigned(payload.current_rss_bytes);
    builder.append_optional_unsigned(payload.peak_growth_bytes);
    builder.append_optional_unsigned(payload.wall_ns);
    append_artifact_seal(builder, payload.stdout_seal);
    append_artifact_seal(builder, payload.stderr_seal);
    append_artifact_seal(builder, payload.joined_sample_seal);
    builder.append_probe_execution_identity(payload.probe_execution_identity);
}

[[nodiscard]] constexpr SIQSShadowProofRssCorpusDigest
record_digest(const SIQSShadowProofRssCampaignJournalRecord& record) noexcept {
    DigestBuilder builder;
    builder.append_string("gnfs.siqs.shadow_proof_rss_campaign.journal_record.v3");
    builder.append_u64(record.schema_version);
    builder.append_u64(record.sequence_number);
    builder.append_u64(static_cast<uint64_t>(record.kind));
    builder.append_digest(record.previous_record_digest);
    builder.append_digest(record.plan_digest);
    builder.append_u64(record.slot_number);
    builder.append_digest(record.slot_digest);
    append_commit_payload(builder, record.commit_payload);
    return builder.finish();
}

[[nodiscard]] constexpr SIQSShadowProofRssCorpusDigest
artifact_digest(SIQSShadowProofRssArtifactKind kind, std::string_view bytes) noexcept {
    DigestBuilder builder;
    builder.append_string("gnfs.siqs.shadow_proof_rss_campaign.artifact.v1");
    builder.append_u64(static_cast<uint64_t>(kind));
    builder.append_string(bytes);
    return builder.finish();
}

[[nodiscard]] constexpr bool artifact_seal_is_valid(const SIQSShadowProofRssArtifactSeal& seal,
                                                    SIQSShadowProofRssArtifactKind kind,
                                                    bool must_be_nonempty) noexcept {
    return seal.committed && seal.kind == kind && digest_is_nonzero(seal.digest) &&
           (!must_be_nonempty || seal.byte_count != 0);
}

[[nodiscard]] constexpr bool
commit_payload_is_valid(const SIQSShadowProofRssJournalCommitPayload& payload,
                        const SIQSShadowProofRssCampaignSlot& slot,
                        const SIQSShadowProofRssGatePolicy& policy) noexcept {
    if (payload.actual_operating_system != policy.operating_system ||
        payload.actual_architecture != policy.architecture ||
        payload.actual_memory_backend != policy.memory_backend ||
        payload.actual_resolved_sieve_workers != policy.resolved_production_sieve_workers ||
        payload.probe_execution_identity != slot.probe_execution_identity ||
        payload.probe_execution_identity != policy.probe_execution_identity ||
        !siqs_shadow_proof_rss_probe_execution_identity_is_valid(
            payload.probe_execution_identity) ||
        (payload.deployment_probe_kind != SIQSShadowProofRssProbeKind::synthetic_test &&
         payload.deployment_probe_kind != SIQSShadowProofRssProbeKind::production_holdout) ||
        !payload.fresh_process || !payload.completed ||
        payload.factor_identity != SIQSShadowProofRssFactorIdentity::pass ||
        !payload.absolute_peak_rss_bytes.has_value() || *payload.absolute_peak_rss_bytes == 0 ||
        !artifact_seal_is_valid(payload.stdout_seal, SIQSShadowProofRssArtifactKind::probe_stdout,
                                true) ||
        !artifact_seal_is_valid(payload.stderr_seal, SIQSShadowProofRssArtifactKind::probe_stderr,
                                slot.mode == SIQSShadowProofRssSampleMode::observe) ||
        !artifact_seal_is_valid(payload.joined_sample_seal,
                                SIQSShadowProofRssArtifactKind::joined_gate_sample, true)) {
        return false;
    }
    if (slot.mode == SIQSShadowProofRssSampleMode::off) {
        return payload.stderr_seal.byte_count == 0 &&
               payload.stderr_seal.digest ==
                   artifact_digest(SIQSShadowProofRssArtifactKind::probe_stderr, {}) &&
               payload.proof_evidence == SIQSShadowProofRssEvidence::not_applicable &&
               payload.matrix_evidence == SIQSShadowProofRssEvidence::not_applicable;
    }
    return slot.mode == SIQSShadowProofRssSampleMode::observe &&
           payload.stderr_seal.byte_count != 0 &&
           payload.proof_evidence == SIQSShadowProofRssEvidence::pass &&
           payload.matrix_evidence == SIQSShadowProofRssEvidence::pass;
}

[[nodiscard]] constexpr SIQSShadowProofRssCampaignJournalHeader
make_header(const SIQSShadowProofRssCampaignPlan& plan,
            SIQSShadowProofRssCorpusDigest policy_digest,
            SIQSShadowProofRssCorpusDigest facts_digest, SIQSShadowProofRssProbeKind probe_kind,
            const SIQSShadowProofRssProbeExecutionIdentity& probe_execution_identity) noexcept {
    SIQSShadowProofRssCampaignJournalHeader header;
    header.schema_version = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION;
    header.probe_kind = probe_kind;
    header.policy_binding_digest = policy_digest;
    header.runtime_facts_digest = facts_digest;
    header.plan_digest = plan_digest(plan, policy_digest, facts_digest);
    header.slot_count = static_cast<uint32_t>(plan.slot_count);
    header.max_concurrency = static_cast<uint32_t>(plan.max_concurrency);
    header.probe_execution_identity = probe_execution_identity;
    header.header_digest = header_digest(header);
    return header;
}

[[nodiscard]] constexpr SIQSShadowProofRssCampaignJournalRecord
make_start_record(const SIQSShadowProofRssCampaignJournalHeader& header,
                  const SIQSShadowProofRssCampaignSlot& slot, uint32_t sequence_number,
                  SIQSShadowProofRssCorpusDigest previous_digest) noexcept {
    SIQSShadowProofRssCampaignJournalRecord record;
    record.schema_version = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION;
    record.sequence_number = sequence_number;
    record.kind = SIQSShadowProofRssJournalRecordKind::slot_started;
    record.previous_record_digest = previous_digest;
    record.plan_digest = header.plan_digest;
    record.slot_number = slot.slot_number;
    record.slot_digest = slot_digest(header.plan_digest, slot);
    record.record_digest = record_digest(record);
    return record;
}

[[nodiscard]] constexpr SIQSShadowProofRssCampaignJournalRecord
make_taint_record(const SIQSShadowProofRssCampaignJournalRecord& start) noexcept {
    SIQSShadowProofRssCampaignJournalRecord record;
    record.schema_version = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION;
    record.sequence_number = start.sequence_number + 1;
    record.kind = SIQSShadowProofRssJournalRecordKind::campaign_tainted;
    record.previous_record_digest = start.record_digest;
    record.plan_digest = start.plan_digest;
    record.slot_number = start.slot_number;
    record.slot_digest = start.slot_digest;
    record.record_digest = record_digest(record);
    return record;
}

[[nodiscard]] constexpr SIQSShadowProofRssCampaignJournalResume resume_result(
    SIQSShadowProofRssJournalStatus status, SIQSShadowProofRssJournalReason reason,
    SIQSShadowProofRssJournalAction action = SIQSShadowProofRssJournalAction::none) noexcept {
    SIQSShadowProofRssCampaignJournalResume result;
    result.status = status;
    result.reason = reason;
    result.action = action;
    return result;
}

[[nodiscard]] constexpr SIQSShadowProofRssCampaignJournalResume
policy_rejection(SIQSShadowProofRssGateReason reason) noexcept {
    switch (reason) {
    case SIQSShadowProofRssGateReason::policy_missing:
        return resume_result(SIQSShadowProofRssJournalStatus::blocked,
                             SIQSShadowProofRssJournalReason::policy_missing);
    case SIQSShadowProofRssGateReason::policy_not_approved:
        return resume_result(SIQSShadowProofRssJournalStatus::blocked,
                             SIQSShadowProofRssJournalReason::policy_not_approved);
    case SIQSShadowProofRssGateReason::policy_budget_missing:
        return resume_result(SIQSShadowProofRssJournalStatus::blocked,
                             SIQSShadowProofRssJournalReason::policy_budget_missing);
    case SIQSShadowProofRssGateReason::policy_headroom_missing:
        return resume_result(SIQSShadowProofRssJournalStatus::blocked,
                             SIQSShadowProofRssJournalReason::policy_headroom_missing);
    case SIQSShadowProofRssGateReason::policy_budget_not_above_headroom:
        return resume_result(SIQSShadowProofRssJournalStatus::invalid,
                             SIQSShadowProofRssJournalReason::policy_budget_not_above_headroom);
    default:
        return resume_result(SIQSShadowProofRssJournalStatus::invalid,
                             SIQSShadowProofRssJournalReason::policy_binding_invalid);
    }
}

} // namespace shadow_proof_rss_campaign_journal_detail

/// Stable non-cryptographic seal for bytes already committed by a future
/// storage layer. It is an accidental-corruption check, not authentication.
[[nodiscard]] constexpr SIQSShadowProofRssArtifactSeal
seal_siqs_shadow_proof_rss_artifact(SIQSShadowProofRssArtifactKind kind,
                                    std::string_view bytes) noexcept {
    return {true, kind, static_cast<uint64_t>(bytes.size()),
            shadow_proof_rss_campaign_journal_detail::artifact_digest(kind, bytes)};
}

[[nodiscard]] constexpr SIQSShadowProofRssCampaignJournalResume
resume_siqs_shadow_proof_rss_campaign_journal(
    const SIQSShadowProofRssGatePolicy* policy,
    const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts,
    SIQSShadowProofRssJournalPresence presence,
    const SIQSShadowProofRssCampaignJournalHeader* header,
    std::span<const SIQSShadowProofRssCampaignJournalRecord> records) noexcept {
    using namespace shadow_proof_rss_campaign_journal_detail;

    if (const auto policy_error = siqs_shadow_proof_rss_gate_policy_error(policy)) {
        return policy_rejection(*policy_error);
    }
    if (runtime_facts == nullptr) {
        return resume_result(SIQSShadowProofRssJournalStatus::invalid,
                             SIQSShadowProofRssJournalReason::runtime_facts_missing);
    }
    if (!runtime_facts_are_valid(*runtime_facts)) {
        return resume_result(SIQSShadowProofRssJournalStatus::invalid,
                             SIQSShadowProofRssJournalReason::runtime_facts_invalid);
    }
    if (!runtime_facts_match_policy(*runtime_facts, *policy)) {
        return resume_result(SIQSShadowProofRssJournalStatus::invalid,
                             SIQSShadowProofRssJournalReason::runtime_facts_mismatch);
    }
    if (!runtime_facts->release_build || !runtime_facts->ndebug) {
        return resume_result(SIQSShadowProofRssJournalStatus::invalid,
                             SIQSShadowProofRssJournalReason::release_ndebug_required);
    }
    if (presence != SIQSShadowProofRssJournalPresence::absent &&
        presence != SIQSShadowProofRssJournalPresence::present) {
        return resume_result(SIQSShadowProofRssJournalStatus::invalid,
                             SIQSShadowProofRssJournalReason::journal_presence_invalid);
    }

    const auto plan = make_siqs_shadow_proof_rss_campaign_plan(policy);
    if (plan.status != SIQSShadowProofRssCampaignPlanStatus::ready ||
        plan.slot_count != SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT) {
        return resume_result(SIQSShadowProofRssJournalStatus::invalid,
                             SIQSShadowProofRssJournalReason::policy_binding_invalid);
    }
    const auto policy_digest = siqs_shadow_proof_rss_policy_binding_digest(*policy);
    const auto facts_digest = runtime_facts_digest(*runtime_facts);
    const auto expected_header =
        make_header(plan, policy_digest, facts_digest, runtime_facts->probe_kind,
                    runtime_facts->probe_execution_identity);

    if (presence == SIQSShadowProofRssJournalPresence::absent) {
        if (header != nullptr || !records.empty()) {
            return resume_result(SIQSShadowProofRssJournalStatus::invalid,
                                 SIQSShadowProofRssJournalReason::absent_journal_has_state);
        }
        auto result = resume_result(SIQSShadowProofRssJournalStatus::ready,
                                    SIQSShadowProofRssJournalReason::ready,
                                    SIQSShadowProofRssJournalAction::create_header);
        result.next_slot_number = 1;
        result.plan_digest = expected_header.plan_digest;
        result.header_to_create = expected_header;
        return result;
    }
    if (header == nullptr) {
        return resume_result(SIQSShadowProofRssJournalStatus::invalid,
                             SIQSShadowProofRssJournalReason::present_journal_missing_header);
    }
    if (*header != expected_header) {
        return resume_result(SIQSShadowProofRssJournalStatus::invalid,
                             SIQSShadowProofRssJournalReason::header_invalid);
    }

    SIQSShadowProofRssCampaignJournalResume result = resume_result(
        SIQSShadowProofRssJournalStatus::ready, SIQSShadowProofRssJournalReason::ready,
        SIQSShadowProofRssJournalAction::append_slot_start);
    result.plan_digest = header->plan_digest;
    SIQSShadowProofRssCorpusDigest previous_digest = header->header_digest;
    uint32_t expected_sequence = 1;
    std::size_t slot_index = 0;
    bool expecting_start = true;

    const auto replay_failure = [&](SIQSShadowProofRssJournalReason reason) constexpr {
        if (expecting_start) {
            return resume_result(SIQSShadowProofRssJournalStatus::invalid, reason);
        }
        result.status = SIQSShadowProofRssJournalStatus::tainted;
        result.reason = reason;
        result.action = SIQSShadowProofRssJournalAction::none;
        result.committed_slot_count = static_cast<uint32_t>(slot_index);
        result.next_slot_number = plan.slots[slot_index].slot_number;
        return std::move(result);
    };

    for (std::size_t record_index = 0; record_index < records.size(); ++record_index) {
        const auto& record = records[record_index];
        if (record.schema_version != SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION ||
            record.sequence_number != expected_sequence ||
            record.previous_record_digest != previous_digest ||
            record.plan_digest != header->plan_digest ||
            record.record_digest != record_digest(record) || slot_index >= plan.slot_count) {
            return replay_failure(SIQSShadowProofRssJournalReason::record_invalid);
        }
        const auto& slot = plan.slots[slot_index];
        if (record.slot_number != slot.slot_number ||
            record.slot_digest != slot_digest(header->plan_digest, slot)) {
            return replay_failure(SIQSShadowProofRssJournalReason::record_order_invalid);
        }
        if (record.kind == SIQSShadowProofRssJournalRecordKind::campaign_tainted) {
            if (expecting_start || record_index + 1 != records.size()) {
                return replay_failure(SIQSShadowProofRssJournalReason::record_order_invalid);
            }
            if (record.commit_payload != SIQSShadowProofRssJournalCommitPayload{}) {
                return replay_failure(SIQSShadowProofRssJournalReason::record_invalid);
            }
            result.status = SIQSShadowProofRssJournalStatus::tainted;
            result.reason = SIQSShadowProofRssJournalReason::explicitly_tainted;
            result.action = SIQSShadowProofRssJournalAction::none;
            result.committed_slot_count = static_cast<uint32_t>(slot_index);
            result.next_slot_number = slot.slot_number;
            return result;
        }
        if (expecting_start) {
            if (record.kind != SIQSShadowProofRssJournalRecordKind::slot_started ||
                record.commit_payload != SIQSShadowProofRssJournalCommitPayload{}) {
                return replay_failure(SIQSShadowProofRssJournalReason::record_order_invalid);
            }
            expecting_start = false;
        } else {
            if (record.kind != SIQSShadowProofRssJournalRecordKind::slot_committed) {
                return replay_failure(SIQSShadowProofRssJournalReason::record_order_invalid);
            }
            if (!commit_payload_is_valid(record.commit_payload, slot, *policy)) {
                return replay_failure(SIQSShadowProofRssJournalReason::committed_sample_invalid);
            }
            if (record.commit_payload.deployment_probe_kind != header->probe_kind ||
                record.commit_payload.probe_execution_identity !=
                    header->probe_execution_identity) {
                return replay_failure(SIQSShadowProofRssJournalReason::committed_sample_invalid);
            }
            result.committed_payloads[slot_index] = record.commit_payload;
            ++slot_index;
            expecting_start = true;
        }
        previous_digest = record.record_digest;
        ++expected_sequence;
    }

    result.committed_slot_count = static_cast<uint32_t>(slot_index);
    if (!expecting_start) {
        result.status = SIQSShadowProofRssJournalStatus::tainted;
        result.reason = SIQSShadowProofRssJournalReason::dangling_slot_start;
        result.action = SIQSShadowProofRssJournalAction::append_taint;
        result.next_slot_number = plan.slots[slot_index].slot_number;
        result.taint_to_append = make_taint_record(records.back());
        return result;
    }
    if (slot_index == plan.slot_count) {
        result.status = SIQSShadowProofRssJournalStatus::complete;
        const bool production_probe =
            runtime_facts->probe_kind == SIQSShadowProofRssProbeKind::production_holdout;
        result.reason = production_probe ? SIQSShadowProofRssJournalReason::complete
                                         : SIQSShadowProofRssJournalReason::synthetic_complete;
        result.action = production_probe ? SIQSShadowProofRssJournalAction::evaluate_gate
                                         : SIQSShadowProofRssJournalAction::none;
        result.next_slot_number = 0;
        return result;
    }

    result.next_slot_number = plan.slots[slot_index].slot_number;
    result.prepared_slot_start = SIQSShadowProofRssPreparedSlotStart(
        make_start_record(*header, plan.slots[slot_index], expected_sequence, previous_digest));
    return result;
}

/// The caller must invoke this function only after the exact proposed start
/// record has reached the storage layer's durable-commit boundary.
[[nodiscard]] constexpr std::optional<SIQSShadowProofRssLaunchPermit>
acknowledge_siqs_shadow_proof_rss_durable_slot_start(
    SIQSShadowProofRssPreparedSlotStart&& prepared,
    SIQSShadowProofRssDurableRecordReceipt&& receipt) noexcept {
    const bool matches = prepared.active_ && receipt.active_ && receipt.record_ == prepared.record_;
    prepared.active_ = false;
    receipt.active_ = false;
    if (!matches) {
        return std::nullopt;
    }
    return SIQSShadowProofRssLaunchPermit(receipt.record_);
}

[[nodiscard]] constexpr std::optional<SIQSShadowProofRssCampaignJournalRecord>
make_siqs_shadow_proof_rss_slot_commit(
    SIQSShadowProofRssLaunchPermit&& permit, const SIQSShadowProofRssGatePolicy* policy,
    const SIQSShadowProofRssJournalCommitPayload& payload) noexcept {
    using namespace shadow_proof_rss_campaign_journal_detail;
    if (!permit.active_) {
        return std::nullopt;
    }
    permit.active_ = false;
    if (policy == nullptr || siqs_shadow_proof_rss_gate_policy_error(policy).has_value()) {
        return std::nullopt;
    }
    const auto plan = make_siqs_shadow_proof_rss_campaign_plan(policy);
    const auto& start = permit.durable_start_record();
    if (start.schema_version != SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION ||
        start.kind != SIQSShadowProofRssJournalRecordKind::slot_started ||
        start.commit_payload != SIQSShadowProofRssJournalCommitPayload{} ||
        start.record_digest != record_digest(start) || start.slot_number == 0 ||
        start.slot_number > plan.slot_count) {
        return std::nullopt;
    }
    const auto& slot = plan.slots[start.slot_number - 1];
    if (start.slot_digest != slot_digest(start.plan_digest, slot) ||
        !commit_payload_is_valid(payload, slot, *policy)) {
        return std::nullopt;
    }
    SIQSShadowProofRssCampaignJournalRecord commit;
    commit.schema_version = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION;
    commit.sequence_number = start.sequence_number + 1;
    commit.kind = SIQSShadowProofRssJournalRecordKind::slot_committed;
    commit.previous_record_digest = start.record_digest;
    commit.plan_digest = start.plan_digest;
    commit.slot_number = start.slot_number;
    commit.slot_digest = start.slot_digest;
    commit.commit_payload = payload;
    commit.record_digest = record_digest(commit);
    return commit;
}

[[nodiscard]] constexpr SIQSShadowProofRssGateSample reconstruct_siqs_shadow_proof_rss_gate_sample(
    const SIQSShadowProofRssGatePolicy& policy, const SIQSShadowProofRssCampaignSlot& slot,
    const SIQSShadowProofRssJournalCommitPayload& payload) noexcept {
    SIQSShadowProofRssGateSample sample;
    sample.policy_approved = policy.approved;
    sample.corpus_id = policy.corpus_id;
    sample.corpus_digest = policy.corpus_digest;
    sample.operating_system = payload.actual_operating_system;
    sample.architecture = payload.actual_architecture;
    sample.memory_backend = payload.actual_memory_backend;
    sample.resolved_production_sieve_workers = payload.actual_resolved_sieve_workers;
    sample.candidate_revision = policy.candidate_revision;
    sample.probe_execution_identity = slot.probe_execution_identity;
    sample.approval_id = policy.approval_id;
    sample.journal_store = policy.journal_store;
    sample.deployment_budget_bytes = policy.deployment_budget_bytes;
    sample.reserved_headroom_bytes = policy.reserved_headroom_bytes;
    sample.fixture_id = slot.fixture_id;
    sample.mode = slot.mode;
    sample.ordinal = slot.ordinal;
    sample.fresh_process = payload.fresh_process;
    sample.completed = payload.completed;
    sample.factor_identity = payload.factor_identity;
    sample.proof_evidence = payload.proof_evidence;
    sample.matrix_evidence = payload.matrix_evidence;
    sample.absolute_peak_rss_bytes = payload.absolute_peak_rss_bytes;
    sample.observe_minus_off_peak_bytes = payload.observe_minus_off_peak_bytes;
    sample.current_rss_bytes = payload.current_rss_bytes;
    sample.peak_growth_bytes = payload.peak_growth_bytes;
    sample.wall_ns = payload.wall_ns;
    return sample;
}

/// Reconstruct data from a fully consistent production-classified journal.
/// The inputs are ordinary forgeable values, so success proves internal replay
/// consistency only. Executable authenticity and permission to evaluate a
/// production gate must come from a later authority-holding session boundary.
[[nodiscard]] inline std::optional<
    std::array<SIQSShadowProofRssGateSample, SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT>>
reconstruct_siqs_shadow_proof_rss_gate_samples(
    const SIQSShadowProofRssGatePolicy* policy,
    const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts,
    const SIQSShadowProofRssCampaignJournalHeader* header,
    std::span<const SIQSShadowProofRssCampaignJournalRecord> records) noexcept {
    using namespace shadow_proof_rss_campaign_journal_detail;
    if (policy == nullptr || runtime_facts == nullptr ||
        siqs_shadow_proof_rss_gate_policy_error(policy).has_value() ||
        !runtime_facts_are_valid(*runtime_facts) ||
        !runtime_facts_match_policy(*runtime_facts, *policy) || !runtime_facts->release_build ||
        !runtime_facts->ndebug ||
        runtime_facts->probe_kind != SIQSShadowProofRssProbeKind::production_holdout ||
        header == nullptr ||
        header->probe_kind != SIQSShadowProofRssProbeKind::production_holdout) {
        return std::nullopt;
    }
    const auto resume = resume_siqs_shadow_proof_rss_campaign_journal(
        policy, runtime_facts, SIQSShadowProofRssJournalPresence::present, header, records);
    if (resume.status != SIQSShadowProofRssJournalStatus::complete ||
        resume.reason != SIQSShadowProofRssJournalReason::complete ||
        resume.action != SIQSShadowProofRssJournalAction::evaluate_gate ||
        resume.committed_slot_count != SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT) {
        return std::nullopt;
    }
    const auto plan = make_siqs_shadow_proof_rss_campaign_plan(policy);
    const auto expected_plan_digest =
        plan_digest(plan, siqs_shadow_proof_rss_policy_binding_digest(*policy),
                    runtime_facts_digest(*runtime_facts));
    if (plan.status != SIQSShadowProofRssCampaignPlanStatus::ready ||
        resume.plan_digest != expected_plan_digest) {
        return std::nullopt;
    }
    std::array<SIQSShadowProofRssGateSample, SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT>
        samples{};
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if (!shadow_proof_rss_campaign_journal_detail::commit_payload_is_valid(
                resume.committed_payloads[index], plan.slots[index], *policy)) {
            return std::nullopt;
        }
        samples[index] = reconstruct_siqs_shadow_proof_rss_gate_sample(
            *policy, plan.slots[index], resume.committed_payloads[index]);
    }
    return samples;
}

} // namespace gnfs::siqs
