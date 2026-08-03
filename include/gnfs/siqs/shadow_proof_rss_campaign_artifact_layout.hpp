#pragma once

/// @file shadow_proof_rss_campaign_artifact_layout.hpp
/// @brief Pure bounded layout and journal-closure validation for RSS artifacts.

#include <gnfs/siqs/shadow_proof_rss_campaign_journal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace gnfs::siqs {

inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_DIRECTORY_LEAF =
    ".artifacts-v1";
inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_PREFIX = "slot-";
inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDOUT_TOKEN = "stdout";
inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDERR_TOKEN = "stderr";
inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_JOINED_TOKEN = "joined";
inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_SUFFIX = ".rssa";
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_SLOT_DIGITS = 10;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACTS_PER_SLOT = 3;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_MAX_ENTRIES =
    SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT *
    SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACTS_PER_SLOT;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_LEAF_SIZE =
    SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_PREFIX.size() +
    SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_SLOT_DIGITS + 1 +
    SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDOUT_TOKEN.size() +
    SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_SUFFIX.size();
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDOUT_MAX_BYTES = 4096;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDERR_MAX_BYTES = 16 * 1024;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_JOINED_MAX_BYTES = 4096;
inline constexpr uint32_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_NO_SLOT = 0;

static_assert(SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT == 80);
static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_MAX_ENTRIES == 240);
static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_LEAF_SIZE == 27);
static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDOUT_TOKEN.size() ==
              SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDERR_TOKEN.size());
static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDOUT_TOKEN.size() ==
              SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_JOINED_TOKEN.size());

struct SIQSShadowProofRssCampaignArtifactAddress final {
    uint32_t slot_number = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_NO_SLOT;
    SIQSShadowProofRssArtifactKind kind = SIQSShadowProofRssArtifactKind::unknown;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssCampaignArtifactAddress&,
               const SIQSShadowProofRssCampaignArtifactAddress&) noexcept = default;
};

struct SIQSShadowProofRssCampaignArtifactLeaf final {
    std::array<char, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_LEAF_SIZE> bytes{};

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return {bytes.data(), bytes.size()};
    }

    [[nodiscard]] constexpr explicit operator std::string_view() const noexcept {
        return view();
    }

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssCampaignArtifactLeaf&,
               const SIQSShadowProofRssCampaignArtifactLeaf&) noexcept = default;
};

namespace shadow_proof_rss_campaign_artifact_layout_detail {

[[nodiscard]] constexpr std::optional<std::size_t>
kind_offset(SIQSShadowProofRssArtifactKind kind) noexcept {
    switch (kind) {
    case SIQSShadowProofRssArtifactKind::probe_stdout:
        return 0;
    case SIQSShadowProofRssArtifactKind::probe_stderr:
        return 1;
    case SIQSShadowProofRssArtifactKind::joined_gate_sample:
        return 2;
    case SIQSShadowProofRssArtifactKind::unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr SIQSShadowProofRssArtifactKind
kind_for_offset(std::size_t offset) noexcept {
    switch (offset) {
    case 0:
        return SIQSShadowProofRssArtifactKind::probe_stdout;
    case 1:
        return SIQSShadowProofRssArtifactKind::probe_stderr;
    case 2:
        return SIQSShadowProofRssArtifactKind::joined_gate_sample;
    default:
        return SIQSShadowProofRssArtifactKind::unknown;
    }
}

[[nodiscard]] constexpr std::string_view kind_token(SIQSShadowProofRssArtifactKind kind) noexcept {
    switch (kind) {
    case SIQSShadowProofRssArtifactKind::probe_stdout:
        return SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDOUT_TOKEN;
    case SIQSShadowProofRssArtifactKind::probe_stderr:
        return SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDERR_TOKEN;
    case SIQSShadowProofRssArtifactKind::joined_gate_sample:
        return SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_JOINED_TOKEN;
    case SIQSShadowProofRssArtifactKind::unknown:
        return {};
    }
    return {};
}

[[nodiscard]] constexpr std::optional<std::size_t>
artifact_index(uint32_t slot_number, SIQSShadowProofRssArtifactKind kind) noexcept {
    const auto offset = kind_offset(kind);
    if (slot_number == 0 || slot_number > SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT ||
        !offset.has_value()) {
        return std::nullopt;
    }
    return (static_cast<std::size_t>(slot_number) - 1) *
               SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACTS_PER_SLOT +
           *offset;
}

[[nodiscard]] constexpr SIQSShadowProofRssCampaignArtifactAddress
address_for_index(std::size_t index) noexcept {
    if (index >= SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_MAX_ENTRIES) {
        return {};
    }
    return {
        .slot_number =
            static_cast<uint32_t>(index / SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACTS_PER_SLOT + 1),
        .kind = kind_for_offset(index % SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACTS_PER_SLOT),
    };
}

[[nodiscard]] constexpr bool artifact_size_is_valid(SIQSShadowProofRssArtifactKind kind,
                                                    uint64_t observed_size,
                                                    std::size_t captured_size) noexcept {
    switch (kind) {
    case SIQSShadowProofRssArtifactKind::probe_stdout:
        return captured_size != 0 &&
               captured_size <= SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDOUT_MAX_BYTES &&
               observed_size == static_cast<uint64_t>(captured_size);
    case SIQSShadowProofRssArtifactKind::probe_stderr:
        return captured_size <= SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDERR_MAX_BYTES &&
               observed_size == static_cast<uint64_t>(captured_size);
    case SIQSShadowProofRssArtifactKind::joined_gate_sample:
        return captured_size != 0 &&
               captured_size <= SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_JOINED_MAX_BYTES &&
               observed_size == static_cast<uint64_t>(captured_size);
    case SIQSShadowProofRssArtifactKind::unknown:
        return false;
    }
    return false;
}

[[nodiscard]] constexpr bool seal_shape_is_valid(const SIQSShadowProofRssArtifactSeal& seal,
                                                 SIQSShadowProofRssArtifactKind kind) noexcept {
    const auto captured_size = static_cast<std::size_t>(seal.byte_count);
    return seal.committed && seal.kind == kind && seal.digest != SIQSShadowProofRssCorpusDigest{} &&
           static_cast<uint64_t>(captured_size) == seal.byte_count &&
           artifact_size_is_valid(kind, seal.byte_count, captured_size);
}

} // namespace shadow_proof_rss_campaign_artifact_layout_detail

[[nodiscard]] constexpr std::optional<SIQSShadowProofRssCampaignArtifactLeaf>
make_siqs_shadow_proof_rss_campaign_artifact_leaf(uint32_t slot_number,
                                                  SIQSShadowProofRssArtifactKind kind) noexcept {
    using namespace shadow_proof_rss_campaign_artifact_layout_detail;
    const auto offset = kind_offset(kind);
    if (slot_number == 0 || slot_number > SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT ||
        !offset.has_value()) {
        return std::nullopt;
    }

    SIQSShadowProofRssCampaignArtifactLeaf leaf;
    std::size_t output = 0;
    for (const char value : SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_PREFIX) {
        leaf.bytes[output++] = value;
    }
    std::size_t digit_output = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_PREFIX.size() +
                               SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_SLOT_DIGITS;
    uint32_t remaining = slot_number;
    for (std::size_t digit = 0; digit < SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_SLOT_DIGITS;
         ++digit) {
        leaf.bytes[--digit_output] = static_cast<char>('0' + (remaining % 10));
        remaining /= 10;
    }
    output = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_PREFIX.size() +
             SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_SLOT_DIGITS;
    leaf.bytes[output++] = '-';
    for (const char value : kind_token(kind)) {
        leaf.bytes[output++] = value;
    }
    for (const char value : SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_SUFFIX) {
        leaf.bytes[output++] = value;
    }
    return leaf;
}

[[nodiscard]] constexpr std::optional<SIQSShadowProofRssCampaignArtifactAddress>
parse_siqs_shadow_proof_rss_campaign_artifact_leaf(std::string_view leaf_name) noexcept {
    using namespace shadow_proof_rss_campaign_artifact_layout_detail;
    if (leaf_name.size() != SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_LEAF_SIZE ||
        !leaf_name.starts_with(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_PREFIX) ||
        !leaf_name.ends_with(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_SUFFIX)) {
        return std::nullopt;
    }

    uint64_t slot_number = 0;
    const std::size_t digit_begin = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_PREFIX.size();
    for (std::size_t digit = 0; digit < SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_SLOT_DIGITS;
         ++digit) {
        const char value = leaf_name[digit_begin + digit];
        if (value < '0' || value > '9') {
            return std::nullopt;
        }
        slot_number = slot_number * 10 + static_cast<uint64_t>(value - '0');
    }
    if (slot_number == 0 || slot_number > SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT) {
        return std::nullopt;
    }

    const std::size_t separator = digit_begin + SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_SLOT_DIGITS;
    if (leaf_name[separator] != '-') {
        return std::nullopt;
    }
    const std::size_t token_begin = separator + 1;
    const std::size_t token_size = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDOUT_TOKEN.size();
    const std::string_view token = leaf_name.substr(token_begin, token_size);
    SIQSShadowProofRssArtifactKind kind = SIQSShadowProofRssArtifactKind::unknown;
    if (token == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDOUT_TOKEN) {
        kind = SIQSShadowProofRssArtifactKind::probe_stdout;
    } else if (token == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDERR_TOKEN) {
        kind = SIQSShadowProofRssArtifactKind::probe_stderr;
    } else if (token == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_JOINED_TOKEN) {
        kind = SIQSShadowProofRssArtifactKind::joined_gate_sample;
    } else {
        return std::nullopt;
    }
    return SIQSShadowProofRssCampaignArtifactAddress{
        .slot_number = static_cast<uint32_t>(slot_number),
        .kind = kind,
    };
}

enum class SIQSShadowProofRssCampaignArtifactLayoutEntryKind : uint8_t {
    unknown,
    regular_file,
    directory,
    link_or_reparse_point,
    other,
};

struct SIQSShadowProofRssCampaignArtifactLayoutEntry final {
    std::string_view leaf_name;
    SIQSShadowProofRssCampaignArtifactLayoutEntryKind kind =
        SIQSShadowProofRssCampaignArtifactLayoutEntryKind::unknown;
    uint64_t link_count = 0;
    uint64_t observed_size = 0;
    std::string_view bytes;
};

enum class SIQSShadowProofRssCampaignArtifactLayoutError : uint8_t {
    none,
    too_many_entries,
    unknown_entry,
    duplicate_artifact,
    entry_not_regular_file,
    link_count_invalid,
    artifact_size_invalid,
};

[[nodiscard]] constexpr std::string_view siqs_shadow_proof_rss_campaign_artifact_layout_error_name(
    SIQSShadowProofRssCampaignArtifactLayoutError error) noexcept {
    switch (error) {
    case SIQSShadowProofRssCampaignArtifactLayoutError::none:
        return "none";
    case SIQSShadowProofRssCampaignArtifactLayoutError::too_many_entries:
        return "too_many_entries";
    case SIQSShadowProofRssCampaignArtifactLayoutError::unknown_entry:
        return "unknown_entry";
    case SIQSShadowProofRssCampaignArtifactLayoutError::duplicate_artifact:
        return "duplicate_artifact";
    case SIQSShadowProofRssCampaignArtifactLayoutError::entry_not_regular_file:
        return "entry_not_regular_file";
    case SIQSShadowProofRssCampaignArtifactLayoutError::link_count_invalid:
        return "link_count_invalid";
    case SIQSShadowProofRssCampaignArtifactLayoutError::artifact_size_invalid:
        return "artifact_size_invalid";
    }
    return "unknown";
}

struct SIQSShadowProofRssCampaignArtifactLayoutDiagnostic final {
    SIQSShadowProofRssCampaignArtifactLayoutError error =
        SIQSShadowProofRssCampaignArtifactLayoutError::none;
    uint32_t slot_number = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_NO_SLOT;
    SIQSShadowProofRssArtifactKind kind = SIQSShadowProofRssArtifactKind::unknown;
};

struct SIQSShadowProofRssCampaignArtifactLayoutSnapshot final {
    std::array<std::optional<SIQSShadowProofRssArtifactSeal>,
               SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_MAX_ENTRIES>
        seals{};
    std::size_t artifact_count = 0;

    [[nodiscard]] constexpr std::optional<SIQSShadowProofRssArtifactSeal>
    seal(uint32_t slot_number, SIQSShadowProofRssArtifactKind kind) const noexcept {
        const auto index =
            shadow_proof_rss_campaign_artifact_layout_detail::artifact_index(slot_number, kind);
        return index.has_value() ? seals[*index] : std::nullopt;
    }
};

struct SIQSShadowProofRssCampaignArtifactLayoutResult final {
    std::optional<SIQSShadowProofRssCampaignArtifactLayoutSnapshot> value;
    SIQSShadowProofRssCampaignArtifactLayoutDiagnostic diagnostic;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value.has_value() &&
               diagnostic.error == SIQSShadowProofRssCampaignArtifactLayoutError::none;
    }
};

namespace shadow_proof_rss_campaign_artifact_layout_detail {

using ArtifactLayoutError = SIQSShadowProofRssCampaignArtifactLayoutError;
using ArtifactLayoutResult = SIQSShadowProofRssCampaignArtifactLayoutResult;

[[nodiscard]] constexpr ArtifactLayoutResult
layout_failure(ArtifactLayoutError error,
               SIQSShadowProofRssCampaignArtifactAddress address = {}) noexcept {
    ArtifactLayoutResult result;
    result.diagnostic.error = error;
    result.diagnostic.slot_number = address.slot_number;
    result.diagnostic.kind = address.kind;
    return result;
}

} // namespace shadow_proof_rss_campaign_artifact_layout_detail

/// Validate one already-enumerated artifact directory. Error selection is
/// independent of enumeration order: capacity, naming, duplicates, kind,
/// links, then size are checked in that fixed order.
[[nodiscard]] constexpr SIQSShadowProofRssCampaignArtifactLayoutResult
inspect_siqs_shadow_proof_rss_campaign_artifact_layout(
    std::span<const SIQSShadowProofRssCampaignArtifactLayoutEntry> entries) noexcept {
    using namespace shadow_proof_rss_campaign_artifact_layout_detail;
    using Entry = SIQSShadowProofRssCampaignArtifactLayoutEntry;
    using EntryKind = SIQSShadowProofRssCampaignArtifactLayoutEntryKind;

    if (entries.size() > SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_MAX_ENTRIES) {
        return layout_failure(ArtifactLayoutError::too_many_entries);
    }

    std::array<const Entry*, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_MAX_ENTRIES> artifacts{};
    std::array<uint8_t, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_MAX_ENTRIES> counts{};
    bool unknown_entry = false;
    for (const Entry& entry : entries) {
        const auto address = parse_siqs_shadow_proof_rss_campaign_artifact_leaf(entry.leaf_name);
        if (!address.has_value()) {
            unknown_entry = true;
            continue;
        }
        const auto index = artifact_index(address->slot_number, address->kind);
        if (!index.has_value()) {
            unknown_entry = true;
            continue;
        }
        ++counts[*index];
        if (artifacts[*index] == nullptr) {
            artifacts[*index] = &entry;
        }
    }
    if (unknown_entry) {
        return layout_failure(ArtifactLayoutError::unknown_entry);
    }
    for (std::size_t index = 0; index < counts.size(); ++index) {
        if (counts[index] > 1) {
            return layout_failure(ArtifactLayoutError::duplicate_artifact,
                                  address_for_index(index));
        }
    }
    for (std::size_t index = 0; index < artifacts.size(); ++index) {
        if (artifacts[index] != nullptr && artifacts[index]->kind != EntryKind::regular_file) {
            return layout_failure(ArtifactLayoutError::entry_not_regular_file,
                                  address_for_index(index));
        }
    }
    for (std::size_t index = 0; index < artifacts.size(); ++index) {
        if (artifacts[index] != nullptr && artifacts[index]->link_count != 1) {
            return layout_failure(ArtifactLayoutError::link_count_invalid,
                                  address_for_index(index));
        }
    }
    for (std::size_t index = 0; index < artifacts.size(); ++index) {
        if (artifacts[index] == nullptr) {
            continue;
        }
        const auto address = address_for_index(index);
        if (!artifact_size_is_valid(address.kind, artifacts[index]->observed_size,
                                    artifacts[index]->bytes.size())) {
            return layout_failure(ArtifactLayoutError::artifact_size_invalid, address);
        }
    }

    SIQSShadowProofRssCampaignArtifactLayoutSnapshot snapshot;
    snapshot.artifact_count = entries.size();
    for (std::size_t index = 0; index < artifacts.size(); ++index) {
        if (artifacts[index] == nullptr) {
            continue;
        }
        const auto address = address_for_index(index);
        snapshot.seals[index] =
            seal_siqs_shadow_proof_rss_artifact(address.kind, artifacts[index]->bytes);
    }
    ArtifactLayoutResult result;
    result.value = snapshot;
    return result;
}

enum class SIQSShadowProofRssCampaignArtifactConsistencyError : uint8_t {
    none,
    resume_state_invalid,
    snapshot_invalid,
    committed_artifact_missing,
    committed_artifact_mismatch,
    unexpected_artifact,
};

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_rss_campaign_artifact_consistency_error_name(
    SIQSShadowProofRssCampaignArtifactConsistencyError error) noexcept {
    switch (error) {
    case SIQSShadowProofRssCampaignArtifactConsistencyError::none:
        return "none";
    case SIQSShadowProofRssCampaignArtifactConsistencyError::resume_state_invalid:
        return "resume_state_invalid";
    case SIQSShadowProofRssCampaignArtifactConsistencyError::snapshot_invalid:
        return "snapshot_invalid";
    case SIQSShadowProofRssCampaignArtifactConsistencyError::committed_artifact_missing:
        return "committed_artifact_missing";
    case SIQSShadowProofRssCampaignArtifactConsistencyError::committed_artifact_mismatch:
        return "committed_artifact_mismatch";
    case SIQSShadowProofRssCampaignArtifactConsistencyError::unexpected_artifact:
        return "unexpected_artifact";
    }
    return "unknown";
}

struct SIQSShadowProofRssCampaignArtifactConsistencyDiagnostic final {
    SIQSShadowProofRssCampaignArtifactConsistencyError error =
        SIQSShadowProofRssCampaignArtifactConsistencyError::none;
    uint32_t slot_number = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_NO_SLOT;
    SIQSShadowProofRssArtifactKind kind = SIQSShadowProofRssArtifactKind::unknown;
};

struct SIQSShadowProofRssCampaignArtifactConsistencyResult final {
    SIQSShadowProofRssCampaignArtifactConsistencyDiagnostic diagnostic;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return diagnostic.error == SIQSShadowProofRssCampaignArtifactConsistencyError::none;
    }
};

namespace shadow_proof_rss_campaign_artifact_layout_detail {

using ArtifactConsistencyError = SIQSShadowProofRssCampaignArtifactConsistencyError;
using ArtifactConsistencyResult = SIQSShadowProofRssCampaignArtifactConsistencyResult;

[[nodiscard]] constexpr ArtifactConsistencyResult
consistency_failure(ArtifactConsistencyError error,
                    SIQSShadowProofRssCampaignArtifactAddress address = {}) noexcept {
    ArtifactConsistencyResult result;
    result.diagnostic.error = error;
    result.diagnostic.slot_number = address.slot_number;
    result.diagnostic.kind = address.kind;
    return result;
}

[[nodiscard]] constexpr const SIQSShadowProofRssArtifactSeal&
payload_seal(const SIQSShadowProofRssJournalCommitPayload& payload,
             SIQSShadowProofRssArtifactKind kind) noexcept {
    switch (kind) {
    case SIQSShadowProofRssArtifactKind::probe_stdout:
        return payload.stdout_seal;
    case SIQSShadowProofRssArtifactKind::probe_stderr:
        return payload.stderr_seal;
    case SIQSShadowProofRssArtifactKind::joined_gate_sample:
        return payload.joined_sample_seal;
    case SIQSShadowProofRssArtifactKind::unknown:
        return payload.stdout_seal;
    }
    return payload.stdout_seal;
}

struct ResumeArtifactDomain final {
    bool valid = false;
    bool allow_current_orphans = false;
    uint32_t committed_slot_count = 0;
    uint32_t current_slot_number = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_NO_SLOT;
};

[[nodiscard]] constexpr ResumeArtifactDomain
resume_artifact_domain(const SIQSShadowProofRssCampaignJournalResume& resume) noexcept {
    const uint32_t slot_count =
        static_cast<uint32_t>(SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT);
    if (resume.committed_slot_count > slot_count) {
        return {};
    }
    if (resume.status == SIQSShadowProofRssJournalStatus::ready &&
        resume.reason == SIQSShadowProofRssJournalReason::ready) {
        if (resume.action == SIQSShadowProofRssJournalAction::create_header &&
            resume.committed_slot_count == 0 && resume.next_slot_number == 1) {
            return {true, false, 0, 1};
        }
        if (resume.action == SIQSShadowProofRssJournalAction::append_slot_start &&
            resume.committed_slot_count < slot_count &&
            resume.next_slot_number == resume.committed_slot_count + 1) {
            return {true, false, resume.committed_slot_count, resume.next_slot_number};
        }
        return {};
    }
    if (resume.status == SIQSShadowProofRssJournalStatus::complete &&
        resume.committed_slot_count == slot_count && resume.next_slot_number == 0) {
        const bool production_complete =
            resume.reason == SIQSShadowProofRssJournalReason::complete &&
            resume.action == SIQSShadowProofRssJournalAction::evaluate_gate;
        const bool synthetic_complete =
            resume.reason == SIQSShadowProofRssJournalReason::synthetic_complete &&
            resume.action == SIQSShadowProofRssJournalAction::none;
        if (production_complete || synthetic_complete) {
            return {true, false, slot_count, 0};
        }
    }
    if (resume.status == SIQSShadowProofRssJournalStatus::tainted &&
        resume.committed_slot_count < slot_count &&
        resume.next_slot_number == resume.committed_slot_count + 1) {
        const bool dangling =
            resume.reason == SIQSShadowProofRssJournalReason::dangling_slot_start &&
            resume.action == SIQSShadowProofRssJournalAction::append_taint;
        const bool explicit_taint =
            resume.reason == SIQSShadowProofRssJournalReason::explicitly_tainted &&
            resume.action == SIQSShadowProofRssJournalAction::none;
        if (dangling || explicit_taint) {
            return {true, true, resume.committed_slot_count, resume.next_slot_number};
        }
    }
    return {};
}

} // namespace shadow_proof_rss_campaign_artifact_layout_detail

/// Close a validated artifact snapshot against one already-validated journal
/// replay. Committed slots require all three exact seals. Ready and complete
/// states admit no future artifacts. A dangling or explicitly tainted current
/// slot may retain any subset of its three durable orphan artifacts.
[[nodiscard]] constexpr SIQSShadowProofRssCampaignArtifactConsistencyResult
validate_siqs_shadow_proof_rss_campaign_artifact_consistency(
    const SIQSShadowProofRssCampaignJournalResume& resume,
    const SIQSShadowProofRssCampaignArtifactLayoutSnapshot& snapshot) noexcept {
    using namespace shadow_proof_rss_campaign_artifact_layout_detail;

    std::size_t observed_count = 0;
    for (std::size_t index = 0; index < snapshot.seals.size(); ++index) {
        if (!snapshot.seals[index].has_value()) {
            continue;
        }
        ++observed_count;
        const auto address = address_for_index(index);
        if (!seal_shape_is_valid(*snapshot.seals[index], address.kind)) {
            return consistency_failure(ArtifactConsistencyError::snapshot_invalid, address);
        }
    }
    if (observed_count != snapshot.artifact_count ||
        observed_count > SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_MAX_ENTRIES) {
        return consistency_failure(ArtifactConsistencyError::snapshot_invalid);
    }

    const ResumeArtifactDomain domain = resume_artifact_domain(resume);
    if (!domain.valid) {
        return consistency_failure(ArtifactConsistencyError::resume_state_invalid);
    }

    for (uint32_t slot_number = 1; slot_number <= domain.committed_slot_count; ++slot_number) {
        const auto& payload = resume.committed_payloads[slot_number - 1];
        for (std::size_t offset = 0; offset < SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACTS_PER_SLOT;
             ++offset) {
            const auto kind = kind_for_offset(offset);
            const auto index = artifact_index(slot_number, kind);
            const SIQSShadowProofRssCampaignArtifactAddress address{slot_number, kind};
            if (!index.has_value() || !snapshot.seals[*index].has_value()) {
                return consistency_failure(ArtifactConsistencyError::committed_artifact_missing,
                                           address);
            }
            if (*snapshot.seals[*index] != payload_seal(payload, kind)) {
                return consistency_failure(ArtifactConsistencyError::committed_artifact_mismatch,
                                           address);
            }
        }
    }

    for (std::size_t index = 0; index < snapshot.seals.size(); ++index) {
        if (!snapshot.seals[index].has_value()) {
            continue;
        }
        const auto address = address_for_index(index);
        if (address.slot_number <= domain.committed_slot_count) {
            continue;
        }
        if (domain.allow_current_orphans && address.slot_number == domain.current_slot_number) {
            continue;
        }
        return consistency_failure(ArtifactConsistencyError::unexpected_artifact, address);
    }
    return {};
}

} // namespace gnfs::siqs
