#include "distributed_sieve_wave_store_internal.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gnfs::sieve::distributed_sieve_resume_detail {
namespace {

[[nodiscard]] constexpr char decimal_digit(std::uint32_t value) noexcept {
    return static_cast<char>('0' + value);
}

} // namespace

std::optional<DistributedSieveWorkerAttemptNamesV1>
distributed_sieve_worker_attempt_names_v1(std::string_view chunk_relative_artifact_stem,
                                          std::uint32_t chunk_id, std::uint32_t attempt_ordinal) {
    if (chunk_id >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS ||
        attempt_ordinal >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS ||
        chunk_relative_artifact_stem.size() >
            DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES -
                DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size() -
                DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1) {
        return std::nullopt;
    }

    DistributedSieveWorkerAttemptNamesV1 names;
    names.relative_lease_stem.reserve(chunk_relative_artifact_stem.size() +
                                      DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size() +
                                      DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1);
    names.relative_lease_stem.append(chunk_relative_artifact_stem);
    names.relative_lease_stem.append(DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1);
    names.relative_lease_stem.push_back(decimal_digit(attempt_ordinal / 10U));
    names.relative_lease_stem.push_back(decimal_digit(attempt_ordinal % 10U));
    if (!distributed_sieve_worker_attempt_relative_stem_matches(
            chunk_relative_artifact_stem, attempt_ordinal, names.relative_lease_stem)) {
        return std::nullopt;
    }

    names.private_directory_leaf = names.relative_lease_stem;
    names.private_directory_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_DIRECTORY_SUFFIX);
    names.base_lock_leaf = names.relative_lease_stem;
    names.base_lock_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX);
    names.reserved_leaf = names.relative_lease_stem;
    names.reserved_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVED_SUFFIX);
    names.reserved_pending_leaf = names.relative_lease_stem;
    names.reserved_pending_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVED_PENDING_SUFFIX);
    names.owned_leaf = names.relative_lease_stem;
    names.owned_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNED_SUFFIX);
    names.owned_pending_leaf = names.relative_lease_stem;
    names.owned_pending_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNED_PENDING_SUFFIX);
    names.rollback_handoff_leaf = names.relative_lease_stem;
    names.rollback_handoff_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_HANDOFF_ROLLBACK_SUFFIX);

    names.canonical_record_leaf.reserve(
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PREFIX.size() +
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1 +
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_ORDINAL_SEPARATOR.size() +
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1);
    names.canonical_record_leaf.append(DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PREFIX);
    names.canonical_record_leaf.push_back(decimal_digit(chunk_id / 10U));
    names.canonical_record_leaf.push_back(decimal_digit(chunk_id % 10U));
    names.canonical_record_leaf.append(DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_ORDINAL_SEPARATOR);
    names.canonical_record_leaf.push_back(decimal_digit(attempt_ordinal / 10U));
    names.canonical_record_leaf.push_back(decimal_digit(attempt_ordinal % 10U));
    names.pending_record_leaf = names.canonical_record_leaf;
    names.pending_record_leaf.append(DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PENDING_SUFFIX);
    return names;
}

std::optional<DistributedSieveParsedWorkerAttemptLeafV1>
parse_distributed_sieve_worker_attempt_leaf_v1(std::string_view leaf) noexcept {
    bool pending = false;
    if (leaf.ends_with(DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PENDING_SUFFIX)) {
        pending = true;
        leaf.remove_suffix(DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PENDING_SUFFIX.size());
    }
    const std::size_t expected_size =
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PREFIX.size() +
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1 +
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_ORDINAL_SEPARATOR.size() +
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1;
    if (leaf.size() != expected_size ||
        !leaf.starts_with(DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PREFIX)) {
        return std::nullopt;
    }
    std::size_t cursor = DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PREFIX.size();
    const auto parse_two_digits = [&](std::uint32_t& output) noexcept {
        if (leaf[cursor] < '0' || leaf[cursor] > '9' || leaf[cursor + 1U] < '0' ||
            leaf[cursor + 1U] > '9') {
            return false;
        }
        output = static_cast<std::uint32_t>(leaf[cursor] - '0') * 10U +
                 static_cast<std::uint32_t>(leaf[cursor + 1U] - '0');
        cursor += DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1;
        return true;
    };

    DistributedSieveParsedWorkerAttemptLeafV1 parsed{.pending = pending};
    if (!parse_two_digits(parsed.chunk_id) ||
        leaf.substr(cursor, DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_ORDINAL_SEPARATOR.size()) !=
            DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_ORDINAL_SEPARATOR) {
        return std::nullopt;
    }
    cursor += DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_ORDINAL_SEPARATOR.size();
    if (!parse_two_digits(parsed.attempt_ordinal) || cursor != leaf.size() ||
        parsed.chunk_id >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS ||
        parsed.attempt_ordinal >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<DistributedSieveMergeGenerationNamesV1>
distributed_sieve_merge_generation_names_v1(std::uint32_t merge_attempt_ordinal) {
    if (merge_attempt_ordinal >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS) {
        return std::nullopt;
    }

    DistributedSieveMergeGenerationNamesV1 names;
    names.relative_lease_stem.reserve(DISTRIBUTED_SIEVE_MERGE_GENERATION_STEM_PREFIX_V1.size() +
                                      DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1);
    names.relative_lease_stem.append(DISTRIBUTED_SIEVE_MERGE_GENERATION_STEM_PREFIX_V1);
    names.relative_lease_stem.push_back(decimal_digit(merge_attempt_ordinal / 10U));
    names.relative_lease_stem.push_back(decimal_digit(merge_attempt_ordinal % 10U));

    names.private_directory_leaf = names.relative_lease_stem;
    names.private_directory_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_DIRECTORY_SUFFIX);
    names.base_lock_leaf = names.relative_lease_stem;
    names.base_lock_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX);
    names.reserved_leaf = names.relative_lease_stem;
    names.reserved_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVED_SUFFIX);
    names.reserved_pending_leaf = names.relative_lease_stem;
    names.reserved_pending_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVED_PENDING_SUFFIX);
    names.owned_leaf = names.relative_lease_stem;
    names.owned_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNED_SUFFIX);
    names.owned_pending_leaf = names.relative_lease_stem;
    names.owned_pending_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNED_PENDING_SUFFIX);
    names.rollback_handoff_leaf = names.relative_lease_stem;
    names.rollback_handoff_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_HANDOFF_ROLLBACK_SUFFIX);

    names.canonical_record_leaf.reserve(DISTRIBUTED_SIEVE_MERGE_STARTED_RECORD_PREFIX.size() +
                                        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1);
    names.canonical_record_leaf.append(DISTRIBUTED_SIEVE_MERGE_STARTED_RECORD_PREFIX);
    names.canonical_record_leaf.push_back(decimal_digit(merge_attempt_ordinal / 10U));
    names.canonical_record_leaf.push_back(decimal_digit(merge_attempt_ordinal % 10U));
    names.pending_record_leaf = names.canonical_record_leaf;
    names.pending_record_leaf.append(DISTRIBUTED_SIEVE_MERGE_STARTED_RECORD_PENDING_SUFFIX);
    return names;
}

std::optional<DistributedSieveParsedMergeStartedLeafV1>
parse_distributed_sieve_merge_started_leaf_v1(std::string_view leaf) noexcept {
    bool pending = false;
    if (leaf.ends_with(DISTRIBUTED_SIEVE_MERGE_STARTED_RECORD_PENDING_SUFFIX)) {
        pending = true;
        leaf.remove_suffix(DISTRIBUTED_SIEVE_MERGE_STARTED_RECORD_PENDING_SUFFIX.size());
    }
    const std::size_t expected_size = DISTRIBUTED_SIEVE_MERGE_STARTED_RECORD_PREFIX.size() +
                                      DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1;
    if (leaf.size() != expected_size ||
        !leaf.starts_with(DISTRIBUTED_SIEVE_MERGE_STARTED_RECORD_PREFIX)) {
        return std::nullopt;
    }
    const std::size_t digits_offset = DISTRIBUTED_SIEVE_MERGE_STARTED_RECORD_PREFIX.size();
    if (leaf[digits_offset] < '0' || leaf[digits_offset] > '9' || leaf[digits_offset + 1U] < '0' ||
        leaf[digits_offset + 1U] > '9') {
        return std::nullopt;
    }
    const auto ordinal = static_cast<std::uint32_t>(leaf[digits_offset] - '0') * 10U +
                         static_cast<std::uint32_t>(leaf[digits_offset + 1U] - '0');
    if (ordinal >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS) {
        return std::nullopt;
    }
    return DistributedSieveParsedMergeStartedLeafV1{
        .merge_attempt_ordinal = ordinal,
        .pending = pending,
    };
}

std::optional<DistributedSieveChunkTerminalFailureNamesV1>
distributed_sieve_chunk_terminal_failure_names_v1(std::uint32_t chunk_id) {
    if (chunk_id >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS) {
        return std::nullopt;
    }
    DistributedSieveChunkTerminalFailureNamesV1 names;
    names.canonical_record_leaf.reserve(
        DISTRIBUTED_SIEVE_CHUNK_TERMINAL_FAILURE_RECORD_PREFIX.size() +
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1);
    names.canonical_record_leaf.append(DISTRIBUTED_SIEVE_CHUNK_TERMINAL_FAILURE_RECORD_PREFIX);
    names.canonical_record_leaf.push_back(decimal_digit(chunk_id / 10U));
    names.canonical_record_leaf.push_back(decimal_digit(chunk_id % 10U));
    names.pending_record_leaf = names.canonical_record_leaf;
    names.pending_record_leaf.append(
        DISTRIBUTED_SIEVE_CHUNK_TERMINAL_FAILURE_RECORD_PENDING_SUFFIX);
    return names;
}

std::optional<DistributedSieveParsedChunkTerminalFailureLeafV1>
parse_distributed_sieve_chunk_terminal_failure_leaf_v1(std::string_view leaf) noexcept {
    bool pending = false;
    if (leaf.ends_with(DISTRIBUTED_SIEVE_CHUNK_TERMINAL_FAILURE_RECORD_PENDING_SUFFIX)) {
        pending = true;
        leaf.remove_suffix(DISTRIBUTED_SIEVE_CHUNK_TERMINAL_FAILURE_RECORD_PENDING_SUFFIX.size());
    }
    const std::size_t expected_size =
        DISTRIBUTED_SIEVE_CHUNK_TERMINAL_FAILURE_RECORD_PREFIX.size() +
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1;
    if (leaf.size() != expected_size ||
        !leaf.starts_with(DISTRIBUTED_SIEVE_CHUNK_TERMINAL_FAILURE_RECORD_PREFIX)) {
        return std::nullopt;
    }
    const std::size_t cursor = DISTRIBUTED_SIEVE_CHUNK_TERMINAL_FAILURE_RECORD_PREFIX.size();
    if (leaf[cursor] < '0' || leaf[cursor] > '9' || leaf[cursor + 1U] < '0' ||
        leaf[cursor + 1U] > '9') {
        return std::nullopt;
    }
    const std::uint32_t chunk_id = static_cast<std::uint32_t>(leaf[cursor] - '0') * 10U +
                                   static_cast<std::uint32_t>(leaf[cursor + 1U] - '0');
    if (chunk_id >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS) {
        return std::nullopt;
    }
    return DistributedSieveParsedChunkTerminalFailureLeafV1{
        .chunk_id = chunk_id,
        .pending = pending,
    };
}

DistributedSieveWaveMergeCommitNamesV1 distributed_sieve_wave_merge_commit_names_v1() {
    return {
        .canonical_record_leaf = std::string(DISTRIBUTED_SIEVE_WAVE_MERGE_COMMIT_RECORD_LEAF),
        .pending_record_leaf = std::string(DISTRIBUTED_SIEVE_WAVE_MERGE_COMMIT_RECORD_PENDING_LEAF),
    };
}

std::optional<DistributedSieveParsedWaveMergeCommitLeafV1>
parse_distributed_sieve_wave_merge_commit_leaf_v1(std::string_view leaf) noexcept {
    if (leaf == DISTRIBUTED_SIEVE_WAVE_MERGE_COMMIT_RECORD_LEAF) {
        return DistributedSieveParsedWaveMergeCommitLeafV1{.pending = false};
    }
    if (leaf == DISTRIBUTED_SIEVE_WAVE_MERGE_COMMIT_RECORD_PENDING_LEAF) {
        return DistributedSieveParsedWaveMergeCommitLeafV1{.pending = true};
    }
    return std::nullopt;
}

std::optional<DistributedSieveParsedCleanupRecordLeafV1>
parse_distributed_sieve_cleanup_record_leaf_v1(std::string_view leaf) noexcept {
    bool pending = false;
    if (leaf.ends_with(DISTRIBUTED_SIEVE_ROOT_RECORD_PENDING_SUFFIX)) {
        pending = true;
        leaf.remove_suffix(DISTRIBUTED_SIEVE_ROOT_RECORD_PENDING_SUFFIX.size());
    }

    const auto parse_worker = [&](std::string_view prefix,
                                  DistributedSieveCleanupRecordCoordinateKindV1 kind)
        -> std::optional<DistributedSieveParsedCleanupRecordLeafV1> {
        const std::size_t expected_size =
            prefix.size() + DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1;
        if (leaf.size() != expected_size || !leaf.starts_with(prefix)) {
            return std::nullopt;
        }
        const std::size_t cursor = prefix.size();
        if (leaf[cursor] < '0' || leaf[cursor] > '9' || leaf[cursor + 1U] < '0' ||
            leaf[cursor + 1U] > '9') {
            return std::nullopt;
        }
        const std::uint32_t ordinal = static_cast<std::uint32_t>(leaf[cursor] - '0') * 10U +
                                      static_cast<std::uint32_t>(leaf[cursor + 1U] - '0');
        if (ordinal >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS) {
            return std::nullopt;
        }
        return DistributedSieveParsedCleanupRecordLeafV1{
            .kind = kind,
            .manifest_order_ordinal = ordinal,
            .pending = pending,
        };
    };

    if (auto parsed =
            parse_worker(DISTRIBUTED_SIEVE_CLEANUP_AUTHORIZED_WORKER_RECORD_PREFIX,
                         DistributedSieveCleanupRecordCoordinateKindV1::authorized_worker);
        parsed.has_value()) {
        return parsed;
    }
    if (auto parsed = parse_worker(DISTRIBUTED_SIEVE_CLEANUP_COMPLETED_WORKER_RECORD_PREFIX,
                                   DistributedSieveCleanupRecordCoordinateKindV1::completed_worker);
        parsed.has_value()) {
        return parsed;
    }
    if (leaf == DISTRIBUTED_SIEVE_CLEANUP_AUTHORIZED_MERGED_RECORD_LEAF) {
        return DistributedSieveParsedCleanupRecordLeafV1{
            .kind = DistributedSieveCleanupRecordCoordinateKindV1::authorized_merged,
            .manifest_order_ordinal = std::nullopt,
            .pending = pending,
        };
    }
    if (leaf == DISTRIBUTED_SIEVE_CLEANUP_COMPLETED_MERGED_RECORD_LEAF) {
        return DistributedSieveParsedCleanupRecordLeafV1{
            .kind = DistributedSieveCleanupRecordCoordinateKindV1::completed_merged,
            .manifest_order_ordinal = std::nullopt,
            .pending = pending,
        };
    }
    return std::nullopt;
}

std::optional<DistributedSieveWorkerCleanupRecordNamesV1>
distributed_sieve_worker_cleanup_record_names_v1(std::uint32_t manifest_order_ordinal) {
    if (manifest_order_ordinal >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS) {
        return std::nullopt;
    }
    const std::array digits{
        decimal_digit(manifest_order_ordinal / 10U),
        decimal_digit(manifest_order_ordinal % 10U),
    };
    const auto make_canonical = [&](std::string_view prefix) {
        std::string leaf;
        leaf.reserve(prefix.size() + digits.size());
        leaf.append(prefix);
        leaf.append(digits.data(), digits.size());
        return leaf;
    };

    DistributedSieveWorkerCleanupRecordNamesV1 names;
    names.authorization_canonical_record_leaf =
        make_canonical(DISTRIBUTED_SIEVE_CLEANUP_AUTHORIZED_WORKER_RECORD_PREFIX);
    names.authorization_pending_record_leaf = names.authorization_canonical_record_leaf;
    names.authorization_pending_record_leaf.append(DISTRIBUTED_SIEVE_ROOT_RECORD_PENDING_SUFFIX);
    names.completion_canonical_record_leaf =
        make_canonical(DISTRIBUTED_SIEVE_CLEANUP_COMPLETED_WORKER_RECORD_PREFIX);
    names.completion_pending_record_leaf = names.completion_canonical_record_leaf;
    names.completion_pending_record_leaf.append(DISTRIBUTED_SIEVE_ROOT_RECORD_PENDING_SUFFIX);
    return names;
}

} // namespace gnfs::sieve::distributed_sieve_resume_detail
