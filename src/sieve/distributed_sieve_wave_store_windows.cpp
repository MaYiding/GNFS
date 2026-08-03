#include "distributed_sieve_wave_store_internal.hpp"

#if defined(_WIN32)

#include "distributed_sieve_worker_cleanup_authority_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace gnfs::sieve::distributed_sieve_resume_detail {
namespace {

namespace durable_record = util::durable_immutable_record;

[[nodiscard]] std::error_code invalid_argument_error() noexcept {
    return std::make_error_code(std::errc::invalid_argument);
}

[[nodiscard]] std::error_code protocol_error() noexcept {
    return std::make_error_code(std::errc::protocol_error);
}

[[nodiscard]] std::error_code unsupported_error() noexcept {
    return std::make_error_code(std::errc::operation_not_supported);
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
diagnostic(DistributedSieveWaveStoreStatus status, std::error_code native_error = {}) noexcept {
    DistributedSieveWaveStoreDiagnostic result;
    result.status = status;
    result.native_error = native_error;
    return result;
}

[[nodiscard]] DistributedSieveWaveStoreOpenResult
open_failure(DistributedSieveWaveStoreDiagnostic failure) noexcept {
    return {nullptr, std::nullopt, std::nullopt, std::move(failure)};
}

[[nodiscard]] DistributedSieveWorkerCleanupRootOpenResultV1
cleanup_root_open_failure(DistributedSieveWaveStoreDiagnostic failure) noexcept {
    return {std::nullopt, std::move(failure)};
}

[[nodiscard]] DistributedSieveWorkerCleanupRecordLeafLoadResultV1
cleanup_record_load_failure(DistributedSieveWaveStoreDiagnostic failure) noexcept {
    return {std::nullopt, std::move(failure)};
}

[[nodiscard]] constexpr bool nil_identity(const NativeIdentityV1& identity) noexcept {
    return identity.volume == 0 && identity.object == 0 && identity.generation == 0;
}

[[nodiscard]] constexpr bool nil_digest(const util::Sha256Digest& digest) noexcept {
    return digest == util::Sha256Digest{};
}

[[nodiscard]] bool valid_absolute_root(const std::filesystem::path& requested) {
    if (requested.empty() || !requested.is_absolute()) {
        return false;
    }

    const auto& native = requested.native();
    if (native.empty() ||
        std::find(native.begin(), native.end(),
                  static_cast<std::filesystem::path::value_type>(0)) != native.end()) {
        return false;
    }

    const auto is_separator = [](std::filesystem::path::value_type value) noexcept {
        return value == static_cast<std::filesystem::path::value_type>('/') ||
               value == static_cast<std::filesystem::path::value_type>('\\');
    };
    if (is_separator(native.back())) {
        return false;
    }
    for (std::size_t index = 1; index < native.size(); ++index) {
        if (is_separator(native[index]) && is_separator(native[index - 1])) {
            return false;
        }
    }
    if (requested.lexically_normal() != requested) {
        return false;
    }
    for (const auto& component : requested.relative_path()) {
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
    }
    const std::filesystem::path leaf = requested.filename();
    return !leaf.empty() && leaf != "." && leaf != "..";
}

[[nodiscard]] bool valid_manifest_draft(const WaveManifestV1& draft) noexcept {
    return nil_identity(draft.wave_root_identity) && nil_identity(draft.permanent_lock_identity) &&
           draft.lock_semantics_version == 0 && nil_digest(draft.self_digest);
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_manifest_draft_semantics(const WaveManifestV1& draft) {
    WaveManifestV1 candidate = draft;
    candidate.wave_root_identity = NativeIdentityV1{.object = 1};
    candidate.permanent_lock_identity = NativeIdentityV1{.object = 2};
    candidate.lock_semantics_version = DISTRIBUTED_SIEVE_WAVE_LOCK_SEMANTICS_VERSION_V1;
    candidate.self_digest = {};
    const DistributedSieveProtocolRecordV1 record(std::move(candidate));
    return validate_distributed_sieve_record(record, false);
}

[[nodiscard]] std::uint64_t current_process_id() noexcept {
    const int process_id = util::process_id();
    return process_id > 0 ? static_cast<std::uint64_t>(process_id) : 0;
}

[[nodiscard]] bool process_matches(std::uint64_t expected_process_id) noexcept {
    return expected_process_id != 0 && current_process_id() == expected_process_id;
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic process_mismatch() noexcept {
    return diagnostic(DistributedSieveWaveStoreStatus::invalid_request, invalid_argument_error());
}

[[nodiscard]] bool
valid_cleanup_exact_anchor(const DistributedSieveWorkerCleanupRootExactAnchorV1* expected_anchor,
                           const util::Sha256Digest& expected_manifest_digest) noexcept {
    if (expected_anchor == nullptr) {
        return true;
    }
    if (nil_identity(expected_anchor->wave_root_identity) ||
        nil_identity(expected_anchor->permanent_lock_identity) ||
        expected_anchor->wave_root_identity == expected_anchor->permanent_lock_identity ||
        expected_anchor->manifest_snapshot.identity == durable_record::NativeIdentity{} ||
        expected_anchor->manifest_snapshot.size == 0 || expected_anchor->manifest_bytes.empty() ||
        expected_anchor->manifest_snapshot.size !=
            static_cast<std::uint64_t>(expected_anchor->manifest_bytes.size()) ||
        nil_digest(expected_anchor->manifest_digest) ||
        expected_anchor->manifest_digest != expected_manifest_digest ||
        expected_anchor->merge_commit_snapshot.identity == durable_record::NativeIdentity{} ||
        expected_anchor->merge_commit_snapshot.size == 0 ||
        expected_anchor->merge_commit_bytes.empty() ||
        expected_anchor->merge_commit_snapshot.size !=
            static_cast<std::uint64_t>(expected_anchor->merge_commit_bytes.size()) ||
        nil_digest(expected_anchor->merge_commit_digest)) {
        return false;
    }

    auto decoded_manifest = decode_distributed_sieve_record(expected_anchor->manifest_bytes);
    auto decoded_commit = decode_distributed_sieve_record(expected_anchor->merge_commit_bytes);
    const auto* manifest =
        decoded_manifest && decoded_manifest.value.has_value()
            ? std::get_if<WaveManifestV1>(std::addressof(*decoded_manifest.value))
            : nullptr;
    const auto* commit = decoded_commit && decoded_commit.value.has_value()
                             ? std::get_if<WaveMergeCommitV1>(std::addressof(*decoded_commit.value))
                             : nullptr;
    return manifest != nullptr && commit != nullptr &&
           manifest->self_digest == expected_anchor->manifest_digest &&
           manifest->wave_root_identity == expected_anchor->wave_root_identity &&
           manifest->permanent_lock_identity == expected_anchor->permanent_lock_identity &&
           commit->manifest_digest == expected_anchor->manifest_digest &&
           commit->self_digest == expected_anchor->merge_commit_digest;
}

[[nodiscard]] bool exact_worker_cleanup_record_leaf(std::string_view leaf) noexcept {
    if (leaf.ends_with(DISTRIBUTED_SIEVE_ROOT_RECORD_PENDING_SUFFIX)) {
        leaf.remove_suffix(DISTRIBUTED_SIEVE_ROOT_RECORD_PENDING_SUFFIX.size());
    }

    const auto matches = [&](std::string_view prefix) noexcept {
        const std::size_t expected_size =
            prefix.size() + DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1;
        if (leaf.size() != expected_size || !leaf.starts_with(prefix)) {
            return false;
        }
        const std::size_t cursor = prefix.size();
        if (leaf[cursor] < '0' || leaf[cursor] > '9' || leaf[cursor + 1U] < '0' ||
            leaf[cursor + 1U] > '9') {
            return false;
        }
        const std::uint32_t ordinal = static_cast<std::uint32_t>(leaf[cursor] - '0') * 10U +
                                      static_cast<std::uint32_t>(leaf[cursor + 1U] - '0');
        return ordinal < DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS;
    };

    return matches(DISTRIBUTED_SIEVE_CLEANUP_AUTHORIZED_WORKER_RECORD_PREFIX) ||
           matches(DISTRIBUTED_SIEVE_CLEANUP_COMPLETED_WORKER_RECORD_PREFIX);
}

} // namespace

DistributedSieveWaveStore::~DistributedSieveWaveStore() = default;

DistributedSieveWaveStoreOpenResult
DistributedSieveWaveStore::create(const std::filesystem::path& absolute_root,
                                  WaveManifestV1 manifest_draft,
                                  DistributedSieveWaveStoreTestHooks hooks) noexcept {
    try {
        if (!valid_absolute_root(absolute_root) || !valid_manifest_draft(manifest_draft)) {
            return open_failure(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                           invalid_argument_error()));
        }
        const auto preflight = validate_manifest_draft_semantics(manifest_draft);
        if (!preflight) {
            auto failure =
                diagnostic(DistributedSieveWaveStoreStatus::invalid_request, protocol_error());
            failure.protocol_status = preflight;
            return open_failure(std::move(failure));
        }
        const std::uint64_t creator_process_id = current_process_id();
        if (!process_matches(creator_process_id)) {
            return open_failure(process_mismatch());
        }

        (void)hooks;
        return open_failure(
            diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error()));
    } catch (const std::bad_alloc&) {
        return open_failure(diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                       std::make_error_code(std::errc::not_enough_memory)));
    } catch (const std::filesystem::filesystem_error& error) {
        return open_failure(
            diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, error.code()));
    } catch (...) {
        return open_failure(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                       std::make_error_code(std::errc::io_error)));
    }
}

DistributedSieveWaveStoreOpenResult
DistributedSieveWaveStore::open(const std::filesystem::path& absolute_root,
                                const util::Sha256Digest& expected_manifest_digest,
                                DistributedSieveWaveStoreTestHooks hooks) noexcept {
    try {
        if (!valid_absolute_root(absolute_root) || nil_digest(expected_manifest_digest)) {
            return open_failure(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                           invalid_argument_error()));
        }
        const std::uint64_t creator_process_id = current_process_id();
        if (!process_matches(creator_process_id)) {
            return open_failure(process_mismatch());
        }

        (void)hooks;
        return open_failure(
            diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error()));
    } catch (const std::bad_alloc&) {
        return open_failure(diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                       std::make_error_code(std::errc::not_enough_memory)));
    } catch (const std::filesystem::filesystem_error& error) {
        return open_failure(
            diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, error.code()));
    } catch (...) {
        return open_failure(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                       std::make_error_code(std::errc::io_error)));
    }
}

DistributedSieveWorkerCleanupRecordLeafLoadResultV1
load_distributed_sieve_worker_cleanup_record_leaf_v1(int wave_root_fd, std::string_view leaf,
                                                     std::uint64_t creator_process_id) noexcept {
    if (!exact_worker_cleanup_record_leaf(leaf)) {
        return cleanup_record_load_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    }

    (void)wave_root_fd;
    (void)creator_process_id;
    return cleanup_record_load_failure(
        diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error()));
}

DistributedSieveWorkerCleanupRootAdmissionV1::DistributedSieveWorkerCleanupRootAdmissionV1(
    DistributedSieveWorkerCleanupRootAdmissionV1&&) noexcept = default;

DistributedSieveWorkerCleanupRootAdmissionV1&
DistributedSieveWorkerCleanupRootAdmissionV1::operator=(
    DistributedSieveWorkerCleanupRootAdmissionV1&&) noexcept = default;

DistributedSieveWorkerCleanupRootAdmissionV1::~DistributedSieveWorkerCleanupRootAdmissionV1() =
    default;

bool DistributedSieveWorkerCleanupRootAdmissionV1::valid() const noexcept {
    return false;
}

const WaveMergeCommitV1& DistributedSieveWorkerCleanupRootAdmissionV1::commit() const {
    throw std::logic_error("worker cleanup root admission is unavailable on this platform");
}

const DistributedSieveWorkerCleanupPrefixWitnessV1&
DistributedSieveWorkerCleanupRootAdmissionV1::cleanup_prefix() const {
    throw std::logic_error("worker cleanup root admission is unavailable on this platform");
}

const relation::OOCRelationReader& DistributedSieveWorkerCleanupRootAdmissionV1::reader() const {
    throw std::logic_error("worker cleanup root admission reader is unavailable on this platform");
}

DistributedSieveWorkerCleanupRootOpenResultV1 open_worker_cleanup_root_v1(
    const std::filesystem::path& absolute_root, const util::Sha256Digest& expected_manifest_digest,
    DistributedSieveWorkerCleanupRootOpenTestHooksV1 hooks,
    const DistributedSieveWorkerCleanupRootExactAnchorV1* expected_anchor) noexcept {
    try {
        if (!valid_absolute_root(absolute_root) || nil_digest(expected_manifest_digest) ||
            !valid_cleanup_exact_anchor(expected_anchor, expected_manifest_digest)) {
            return cleanup_root_open_failure(diagnostic(
                DistributedSieveWaveStoreStatus::invalid_request, invalid_argument_error()));
        }
        const std::uint64_t creator_process_id = current_process_id();
        if (!process_matches(creator_process_id)) {
            return cleanup_root_open_failure(process_mismatch());
        }

        (void)hooks;
        return cleanup_root_open_failure(
            diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error()));
    } catch (const std::bad_alloc&) {
        return cleanup_root_open_failure(
            diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                       std::make_error_code(std::errc::not_enough_memory)));
    } catch (const std::filesystem::filesystem_error& error) {
        return cleanup_root_open_failure(
            diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, error.code()));
    } catch (...) {
        return cleanup_root_open_failure(
            diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                       std::make_error_code(std::errc::io_error)));
    }
}

} // namespace gnfs::sieve::distributed_sieve_resume_detail

namespace gnfs::sieve::distributed_sieve_merge_writer_authority_detail {

bool DistributedSieveCommittedTailAdmissionV1::valid() const noexcept {
    return false;
}

distributed_sieve_worker_cleanup_authority_detail::DistributedSieveCommittedTailCleanupTransitionV1
DistributedSieveCommittedTailAdmissionV1::release_for_worker_cleanup_cold_open_v1() && noexcept {
    distributed_sieve_worker_cleanup_authority_detail::
        DistributedSieveCommittedTailCleanupTransitionV1 result;
    result.wave_store.status =
        distributed_sieve_resume_detail::DistributedSieveWaveStoreStatus::platform_unsupported;
    result.wave_store.native_error = std::make_error_code(std::errc::operation_not_supported);
    result.native_error = result.wave_store.native_error;
    return result;
}

} // namespace gnfs::sieve::distributed_sieve_merge_writer_authority_detail

#endif
