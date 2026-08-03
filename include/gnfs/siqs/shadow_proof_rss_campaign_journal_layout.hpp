#pragma once

/// @file shadow_proof_rss_campaign_journal_layout.hpp
/// @brief Pure bounded layout validation for one SIQS RSS campaign journal.

#include <gnfs/siqs/shadow_proof_rss_campaign_journal_codec.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace gnfs::siqs {

inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF =
    ".session.lock";
inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF =
    "campaign-header.rjhd";
inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_PREFIX = "record-";
inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_SUFFIX = ".rjrc";
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_DIGITS = 10;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_LEAF_SIZE =
    SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_PREFIX.size() +
    SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_DIGITS +
    SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_SUFFIX.size();
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS =
    2 * SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_ENTRIES =
    2 + SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS;
inline constexpr uint32_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE = 0;

static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS == 160);
static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_ENTRIES == 162);
static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_LEAF_SIZE == 22);

enum class SIQSShadowProofRssCampaignJournalLayoutEntryKind : uint8_t {
    unknown,
    regular_file,
    directory,
    link_or_reparse_point,
    other,
};

/// One already-enumerated directory leaf. The inspector borrows all views only
/// for the duration of the call and never stores them in its result.
struct SIQSShadowProofRssCampaignJournalLayoutEntry final {
    std::string_view leaf_name;
    SIQSShadowProofRssCampaignJournalLayoutEntryKind kind =
        SIQSShadowProofRssCampaignJournalLayoutEntryKind::unknown;
    uint64_t link_count = 0;
    uint64_t observed_size = 0;
    std::span<const std::byte> bytes;
};

enum class SIQSShadowProofRssCampaignJournalLayoutError : uint8_t {
    none,
    too_many_entries,
    unknown_entry,
    duplicate_session_lock,
    duplicate_header,
    duplicate_record,
    session_lock_missing,
    entry_not_regular_file,
    link_count_invalid,
    session_lock_not_empty,
    header_size_invalid,
    record_size_invalid,
    record_without_header,
    record_gap,
    header_codec_invalid,
    record_codec_invalid,
    record_sequence_mismatch,
};

[[nodiscard]] constexpr std::string_view siqs_shadow_proof_rss_campaign_journal_layout_error_name(
    SIQSShadowProofRssCampaignJournalLayoutError error) noexcept {
    switch (error) {
    case SIQSShadowProofRssCampaignJournalLayoutError::none:
        return "none";
    case SIQSShadowProofRssCampaignJournalLayoutError::too_many_entries:
        return "too_many_entries";
    case SIQSShadowProofRssCampaignJournalLayoutError::unknown_entry:
        return "unknown_entry";
    case SIQSShadowProofRssCampaignJournalLayoutError::duplicate_session_lock:
        return "duplicate_session_lock";
    case SIQSShadowProofRssCampaignJournalLayoutError::duplicate_header:
        return "duplicate_header";
    case SIQSShadowProofRssCampaignJournalLayoutError::duplicate_record:
        return "duplicate_record";
    case SIQSShadowProofRssCampaignJournalLayoutError::session_lock_missing:
        return "session_lock_missing";
    case SIQSShadowProofRssCampaignJournalLayoutError::entry_not_regular_file:
        return "entry_not_regular_file";
    case SIQSShadowProofRssCampaignJournalLayoutError::link_count_invalid:
        return "link_count_invalid";
    case SIQSShadowProofRssCampaignJournalLayoutError::session_lock_not_empty:
        return "session_lock_not_empty";
    case SIQSShadowProofRssCampaignJournalLayoutError::header_size_invalid:
        return "header_size_invalid";
    case SIQSShadowProofRssCampaignJournalLayoutError::record_size_invalid:
        return "record_size_invalid";
    case SIQSShadowProofRssCampaignJournalLayoutError::record_without_header:
        return "record_without_header";
    case SIQSShadowProofRssCampaignJournalLayoutError::record_gap:
        return "record_gap";
    case SIQSShadowProofRssCampaignJournalLayoutError::header_codec_invalid:
        return "header_codec_invalid";
    case SIQSShadowProofRssCampaignJournalLayoutError::record_codec_invalid:
        return "record_codec_invalid";
    case SIQSShadowProofRssCampaignJournalLayoutError::record_sequence_mismatch:
        return "record_sequence_mismatch";
    }
    return "unknown";
}

struct SIQSShadowProofRssCampaignJournalRecordLeaf final {
    std::array<char, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_LEAF_SIZE> bytes{};

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return {bytes.data(), bytes.size()};
    }

    [[nodiscard]] constexpr explicit operator std::string_view() const noexcept {
        return view();
    }

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssCampaignJournalRecordLeaf&,
               const SIQSShadowProofRssCampaignJournalRecordLeaf&) noexcept = default;
};

[[nodiscard]] constexpr std::optional<SIQSShadowProofRssCampaignJournalRecordLeaf>
make_siqs_shadow_proof_rss_campaign_journal_record_leaf(uint32_t sequence_number) noexcept {
    if (sequence_number == 0 ||
        sequence_number > SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS) {
        return std::nullopt;
    }

    SIQSShadowProofRssCampaignJournalRecordLeaf leaf;
    std::size_t output = 0;
    for (const char value : SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_PREFIX) {
        leaf.bytes[output++] = value;
    }
    std::size_t digit_output = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_PREFIX.size() +
                               SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_DIGITS;
    uint32_t remaining = sequence_number;
    for (std::size_t digit = 0; digit < SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_DIGITS;
         ++digit) {
        leaf.bytes[--digit_output] = static_cast<char>('0' + (remaining % 10));
        remaining /= 10;
    }
    output = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_PREFIX.size() +
             SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_DIGITS;
    for (const char value : SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_SUFFIX) {
        leaf.bytes[output++] = value;
    }
    return leaf;
}

[[nodiscard]] constexpr std::optional<uint32_t>
parse_siqs_shadow_proof_rss_campaign_journal_record_leaf(std::string_view leaf_name) noexcept {
    if (leaf_name.size() != SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_LEAF_SIZE ||
        !leaf_name.starts_with(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_PREFIX) ||
        !leaf_name.ends_with(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_SUFFIX)) {
        return std::nullopt;
    }

    uint64_t sequence_number = 0;
    const std::size_t digit_begin = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_PREFIX.size();
    for (std::size_t digit = 0; digit < SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_DIGITS;
         ++digit) {
        const char value = leaf_name[digit_begin + digit];
        if (value < '0' || value > '9') {
            return std::nullopt;
        }
        sequence_number = sequence_number * 10 + static_cast<uint64_t>(value - '0');
    }
    if (sequence_number == 0 ||
        sequence_number > SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(sequence_number);
}

struct SIQSShadowProofRssCampaignJournalLayoutSnapshot final {
    SIQSShadowProofRssJournalPresence presence = SIQSShadowProofRssJournalPresence::absent;
    std::optional<SIQSShadowProofRssCampaignJournalHeader> header;
    std::array<SIQSShadowProofRssCampaignJournalRecord,
               SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS>
        records{};
    std::size_t record_count = 0;

    [[nodiscard]] constexpr std::span<const SIQSShadowProofRssCampaignJournalRecord>
    record_span() const noexcept {
        // This pure snapshot is intentionally forgeable and carries no storage
        // authority. Keep even a caller-mutated aggregate from creating an
        // out-of-bounds span; the native leased store must still reject rather
        // than trust caller-supplied snapshots.
        const std::size_t bounded_count =
            record_count <= records.size() ? record_count : records.size();
        return {records.data(), bounded_count};
    }
};

struct SIQSShadowProofRssCampaignJournalLayoutDiagnostic final {
    SIQSShadowProofRssCampaignJournalLayoutError layout_error =
        SIQSShadowProofRssCampaignJournalLayoutError::none;
    SIQSShadowProofRssCampaignJournalCodecError codec_error =
        SIQSShadowProofRssCampaignJournalCodecError::none;
    std::size_t codec_error_offset = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET;
    uint32_t record_sequence = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE;
};

struct SIQSShadowProofRssCampaignJournalLayoutResult final {
    std::optional<SIQSShadowProofRssCampaignJournalLayoutSnapshot> value;
    SIQSShadowProofRssCampaignJournalLayoutDiagnostic diagnostic;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value.has_value() &&
               diagnostic.layout_error == SIQSShadowProofRssCampaignJournalLayoutError::none &&
               diagnostic.codec_error == SIQSShadowProofRssCampaignJournalCodecError::none;
    }
};

namespace shadow_proof_rss_campaign_journal_layout_detail {

using Entry = SIQSShadowProofRssCampaignJournalLayoutEntry;
using Error = SIQSShadowProofRssCampaignJournalLayoutError;
using Result = SIQSShadowProofRssCampaignJournalLayoutResult;

[[nodiscard]] constexpr Result
failure(Error layout_error,
        uint32_t record_sequence = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE,
        SIQSShadowProofRssCampaignJournalCodecError codec_error =
            SIQSShadowProofRssCampaignJournalCodecError::none,
        std::size_t codec_error_offset =
            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET) noexcept {
    Result result;
    result.diagnostic.layout_error = layout_error;
    result.diagnostic.codec_error = codec_error;
    result.diagnostic.codec_error_offset = codec_error_offset;
    result.diagnostic.record_sequence = record_sequence;
    return result;
}

[[nodiscard]] constexpr uint32_t record_sequence_for_index(std::size_t index) noexcept {
    return static_cast<uint32_t>(index + 1);
}

} // namespace shadow_proof_rss_campaign_journal_layout_detail

/// Validate and decode one bounded directory snapshot. Validation uses fixed
/// phases, so entry enumeration order cannot change the reported error
/// category. Successful records are returned in sequence order. A failed
/// result owns no partial decoded value and no borrowed entry view.
[[nodiscard]] constexpr SIQSShadowProofRssCampaignJournalLayoutResult
inspect_siqs_shadow_proof_rss_campaign_journal_layout(
    std::span<const SIQSShadowProofRssCampaignJournalLayoutEntry> entries) noexcept {
    using namespace shadow_proof_rss_campaign_journal_layout_detail;

    if (entries.size() > SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_ENTRIES) {
        return failure(Error::too_many_entries);
    }

    const Entry* session_lock = nullptr;
    const Entry* header = nullptr;
    std::array<const Entry*, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS> records{};
    std::size_t session_lock_count = 0;
    std::size_t header_count = 0;
    std::array<uint8_t, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS> record_counts{};
    bool unknown_entry = false;

    // Phase 1: classify every leaf without accepting aliases or temporary
    // names. Do not fail during enumeration; that would make category priority
    // depend on enumeration order.
    for (const Entry& entry : entries) {
        if (entry.leaf_name == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF) {
            ++session_lock_count;
            if (session_lock == nullptr) {
                session_lock = &entry;
            }
            continue;
        }
        if (entry.leaf_name == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF) {
            ++header_count;
            if (header == nullptr) {
                header = &entry;
            }
            continue;
        }
        const auto sequence =
            parse_siqs_shadow_proof_rss_campaign_journal_record_leaf(entry.leaf_name);
        if (!sequence.has_value()) {
            unknown_entry = true;
            continue;
        }
        const std::size_t index = static_cast<std::size_t>(*sequence - 1);
        ++record_counts[index];
        if (records[index] == nullptr) {
            records[index] = &entry;
        }
    }

    // Phase 2: reject naming and multiplicity failures in a fixed order.
    if (unknown_entry) {
        return failure(Error::unknown_entry);
    }
    if (session_lock_count > 1) {
        return failure(Error::duplicate_session_lock);
    }
    if (header_count > 1) {
        return failure(Error::duplicate_header);
    }
    for (std::size_t index = 0; index < record_counts.size(); ++index) {
        if (record_counts[index] > 1) {
            return failure(Error::duplicate_record, record_sequence_for_index(index));
        }
    }
    if (session_lock == nullptr) {
        return failure(Error::session_lock_missing);
    }

    // Phase 3: validate object type for all allowed leaves before any
    // leaf-specific metadata. The order is lock, header, then record sequence.
    if (session_lock->kind != SIQSShadowProofRssCampaignJournalLayoutEntryKind::regular_file) {
        return failure(Error::entry_not_regular_file);
    }
    if (header != nullptr &&
        header->kind != SIQSShadowProofRssCampaignJournalLayoutEntryKind::regular_file) {
        return failure(Error::entry_not_regular_file);
    }
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (records[index] != nullptr &&
            records[index]->kind !=
                SIQSShadowProofRssCampaignJournalLayoutEntryKind::regular_file) {
            return failure(Error::entry_not_regular_file, record_sequence_for_index(index));
        }
    }

    // Phase 4: every allowed object must be a single-link file.
    if (session_lock->link_count != 1) {
        return failure(Error::link_count_invalid);
    }
    if (header != nullptr && header->link_count != 1) {
        return failure(Error::link_count_invalid);
    }
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (records[index] != nullptr && records[index]->link_count != 1) {
            return failure(Error::link_count_invalid, record_sequence_for_index(index));
        }
    }

    // Phase 5: exact observed and captured byte sizes exclude partial reads.
    if (session_lock->observed_size != 0 || !session_lock->bytes.empty()) {
        return failure(Error::session_lock_not_empty);
    }
    if (header != nullptr &&
        (header->observed_size != SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE ||
         header->bytes.size() != SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE)) {
        return failure(Error::header_size_invalid);
    }
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (records[index] != nullptr &&
            (records[index]->observed_size !=
                 SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE ||
             records[index]->bytes.size() !=
                 SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE)) {
            return failure(Error::record_size_invalid, record_sequence_for_index(index));
        }
    }

    // Phase 6: establish structural presence and a contiguous record prefix.
    std::size_t highest_record_index = 0;
    bool has_record = false;
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (records[index] != nullptr) {
            has_record = true;
            highest_record_index = index;
        }
    }
    if (has_record && header == nullptr) {
        return failure(Error::record_without_header);
    }
    if (has_record) {
        for (std::size_t index = 0; index <= highest_record_index; ++index) {
            if (records[index] == nullptr) {
                return failure(Error::record_gap, record_sequence_for_index(index));
            }
        }
    }

    SIQSShadowProofRssCampaignJournalLayoutSnapshot snapshot;
    if (header == nullptr) {
        snapshot.presence = SIQSShadowProofRssJournalPresence::absent;
        Result result;
        result.value = snapshot;
        return result;
    }
    snapshot.presence = SIQSShadowProofRssJournalPresence::present;

    // Phase 7: decode the header, then every record in canonical sequence
    // order. Complete all record decoding before comparing filename and wire
    // sequence values, keeping codec failures at a fixed higher priority.
    const auto decoded_header = decode_siqs_shadow_proof_rss_campaign_journal_header(header->bytes);
    if (!decoded_header) {
        return failure(Error::header_codec_invalid,
                       SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE,
                       decoded_header.error, decoded_header.error_offset);
    }
    snapshot.header = *decoded_header.value;
    snapshot.record_count = has_record ? highest_record_index + 1 : 0;

    for (std::size_t index = 0; index < snapshot.record_count; ++index) {
        const auto decoded_record =
            decode_siqs_shadow_proof_rss_campaign_journal_record(records[index]->bytes);
        if (!decoded_record) {
            return failure(Error::record_codec_invalid, record_sequence_for_index(index),
                           decoded_record.error, decoded_record.error_offset);
        }
        snapshot.records[index] = *decoded_record.value;
    }
    for (std::size_t index = 0; index < snapshot.record_count; ++index) {
        if (snapshot.records[index].sequence_number != record_sequence_for_index(index)) {
            return failure(Error::record_sequence_mismatch, record_sequence_for_index(index));
        }
    }

    Result result;
    result.value = snapshot;
    return result;
}

} // namespace gnfs::siqs
