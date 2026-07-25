#pragma once

#include "gnfs/relation/ooc_relation_format.hpp"
#include "gnfs/util/durable_immutable_file.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/sha256.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <stdio.h>
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace gnfs::relation {

class OOCRelationWriter;
class OOCCleanupTransaction;
namespace ooc_cleanup_detail {
class BaseLock;
}

/// Last durable cleanup boundary reached by an OOC cleanup transaction.
enum class OOCCleanupStage : std::uint8_t {
    None,
    IntentDurable,
    IndexQuarantined,
    PairQuarantined,
    DeleteAuthorized,
    DataRemoved,
    IndexRemoved,
    IntentRemoved,
    Completed,
};

enum class OOCCleanupStatus : std::uint8_t {
    Completed,
    NoTransaction,
    Interrupted,
    InvalidRequest,
    SourcePairInvalid,
    IntentCorrupt,
    IntentConflict,
    ForeignReplacementPreserved,
    NamespaceConflict,
    RecoveryRequired,
    Busy,
    IoFailure,
    DurabilityFailure,
    PlatformUnsupported,
    UnexpectedFailure,
};

struct OOCCleanupResult final {
    OOCCleanupStatus status = OOCCleanupStatus::UnexpectedFailure;
    OOCCleanupStage stage = OOCCleanupStage::None;
    std::error_code native_error;

    [[nodiscard]] bool completed() const noexcept {
        return status == OOCCleanupStatus::Completed;
    }

    /// No further canonical cleanup transaction is currently actionable.
    /// This does not prove the pair namespace is empty: NoTransaction is also
    /// returned when unrelated live artifacts exist without an intent.
    [[nodiscard]] bool transaction_terminal() const noexcept {
        return status == OOCCleanupStatus::Completed || status == OOCCleanupStatus::NoTransaction;
    }

    [[nodiscard]] bool retryable() const noexcept {
        return status == OOCCleanupStatus::Interrupted || status == OOCCleanupStatus::Busy ||
               status == OOCCleanupStatus::RecoveryRequired ||
               status == OOCCleanupStatus::IoFailure ||
               status == OOCCleanupStatus::DurabilityFailure;
    }
};

struct OOCExactCleanupExpectation final {
    std::uint64_t index_magic = 0;
    std::uint64_t persisted_count = 0;
    std::uint64_t index_size = 0;
    std::uint64_t data_size = 0;

    friend bool operator==(const OOCExactCleanupExpectation&,
                           const OOCExactCleanupExpectation&) = default;
};

/// Move-only authority to start cleanup for one pair created by a trusted
/// owner. The stable native identities are captured at ownership acquisition;
/// store_id correlates the two V3 headers but never grants authority alone.
///
/// No public path/store-id factory exists. Only a fresh writer issues this
/// receipt while it still owns the exact O_EXCL-created pair. Structural
/// recovery descriptors and sequence digests never recreate it. Once a
/// canonical durable intent exists, begin_or_resume() consumes the receipt and
/// later processes resume from that intent alone.
class OOCCleanupOwnershipReceipt final {
public:
    OOCCleanupOwnershipReceipt(const OOCCleanupOwnershipReceipt&) = delete;
    OOCCleanupOwnershipReceipt& operator=(const OOCCleanupOwnershipReceipt&) = delete;

    OOCCleanupOwnershipReceipt(OOCCleanupOwnershipReceipt&& other) noexcept
        : base_path_(std::move(other.base_path_)), store_id_(other.store_id_),
          index_identity_(other.index_identity_), data_identity_(other.data_identity_),
          spent_(other.spent_) {
        other.store_id_ = 0;
        other.index_identity_ = {};
        other.data_identity_ = {};
        other.spent_ = true;
    }

    OOCCleanupOwnershipReceipt& operator=(OOCCleanupOwnershipReceipt&&) = delete;

    [[nodiscard]] bool spent() const noexcept {
        return spent_;
    }

private:
    struct NativeIdentity final {
        std::uint64_t first = 0;
        std::uint64_t second = 0;
        std::uint64_t third = 0;

        friend bool operator==(const NativeIdentity&, const NativeIdentity&) = default;
    };

    OOCCleanupOwnershipReceipt(std::filesystem::path base_path, std::uint64_t store_id,
                               NativeIdentity index_identity, NativeIdentity data_identity) noexcept
        : base_path_(std::move(base_path)), store_id_(store_id), index_identity_(index_identity),
          data_identity_(data_identity) {}

    std::filesystem::path base_path_;
    std::uint64_t store_id_ = 0;
    NativeIdentity index_identity_;
    NativeIdentity data_identity_;
    bool spent_ = false;

    friend class OOCRelationWriter;
    friend class OOCCleanupTransaction;
};

/// Move-only authority to remove one RelationSink lease directory created
/// under the matching persistent external lock. The receipt alone never
/// authorizes pair deletion. Before activation, its exact durable
/// RESERVED/OWNED chain may roll back only the fresh pair inside that owned
/// directory; after RESERVED is consumed, pair cleanup again requires the
/// separate fresh-writer receipt above.
class OOCPrivateLeaseOwnershipReceipt final {
public:
    OOCPrivateLeaseOwnershipReceipt(const OOCPrivateLeaseOwnershipReceipt&) = delete;
    OOCPrivateLeaseOwnershipReceipt& operator=(const OOCPrivateLeaseOwnershipReceipt&) = delete;

    OOCPrivateLeaseOwnershipReceipt(OOCPrivateLeaseOwnershipReceipt&& other) noexcept
        : base_path_(std::move(other.base_path_)),
          private_directory_(std::move(other.private_directory_)),
          lock_path_(std::move(other.lock_path_)), directory_identity_(other.directory_identity_),
          lease_id_(other.lease_id_), owner_identity_(other.owner_identity_),
          owned_identity_(other.owned_identity_), live_lock_(std::move(other.live_lock_)),
          owner_process_id_(other.owner_process_id_), active_(other.active_), spent_(other.spent_) {
        other.base_path_.clear();
        other.private_directory_.clear();
        other.lock_path_.clear();
        other.directory_identity_ = {};
        other.lease_id_ = {};
        other.owner_identity_ = {};
        other.owned_identity_ = {};
        other.owner_process_id_ = 0;
        other.active_ = false;
        other.spent_ = true;
    }

    OOCPrivateLeaseOwnershipReceipt& operator=(OOCPrivateLeaseOwnershipReceipt&&) = delete;

    [[nodiscard]] bool spent() const noexcept {
        return spent_;
    }

    [[nodiscard]] const std::filesystem::path& base_path() const noexcept {
        return base_path_;
    }

    [[nodiscard]] const std::filesystem::path& private_directory() const noexcept {
        return private_directory_;
    }

private:
    OOCPrivateLeaseOwnershipReceipt(std::filesystem::path base_path,
                                    std::filesystem::path private_directory,
                                    std::filesystem::path lock_path,
                                    std::array<std::uint64_t, 3> directory_identity,
                                    std::array<std::uint64_t, 2> lease_id,
                                    std::array<std::uint64_t, 3> owner_identity,
                                    std::array<std::uint64_t, 3> owned_identity,
                                    std::shared_ptr<ooc_cleanup_detail::BaseLock> live_lock,
                                    std::uint64_t owner_process_id) noexcept
        : base_path_(std::move(base_path)), private_directory_(std::move(private_directory)),
          lock_path_(std::move(lock_path)), directory_identity_(directory_identity),
          lease_id_(lease_id), owner_identity_(owner_identity), owned_identity_(owned_identity),
          live_lock_(std::move(live_lock)), owner_process_id_(owner_process_id) {}

    std::filesystem::path base_path_;
    std::filesystem::path private_directory_;
    std::filesystem::path lock_path_;
    std::array<std::uint64_t, 3> directory_identity_{};
    std::array<std::uint64_t, 2> lease_id_{};
    std::array<std::uint64_t, 3> owner_identity_{};
    std::array<std::uint64_t, 3> owned_identity_{};
    std::shared_ptr<ooc_cleanup_detail::BaseLock> live_lock_;
    std::uint64_t owner_process_id_ = 0;
    bool active_ = false;
    bool spent_ = false;

    friend class OOCRelationWriter;
    friend class OOCCleanupTransaction;
};

struct OOCPrivateLeaseReservation final {
    OOCCleanupResult result;
    std::optional<OOCPrivateLeaseOwnershipReceipt> ownership;

    [[nodiscard]] bool completed() const noexcept {
        return result.completed() && ownership.has_value() && !ownership->spent();
    }
};

/// Optional narrowing expectation for resume(). It cannot authorize a new
/// intent; begin_or_resume() derives path and store identity from a receipt.
struct OOCCleanupRequest final {
    std::filesystem::path base_path;
    std::uint64_t store_id = 0;
    std::optional<OOCExactCleanupExpectation> exact;
};

/// Trusted test-only interruption boundaries. Each point is emitted only after
/// the corresponding namespace mutation and parent-directory sync completed.
enum class OOCCleanupFaultPoint : std::uint8_t {
    IntentDurable,
    FirstRenameDurable,
    SecondRenameDurable,
    DeleteAuthorizedDurable,
    FirstUnlinkDurable,
    SecondUnlinkDurable,
    IntentRemovedDurable,
};

/// Trusted test-only boundaries for the no-authority pending publication
/// protocol. The existing OOCCleanupFaultPoint values remain the authoritative
/// post-canonical boundaries.
enum class OOCCleanupPublishFaultPoint : std::uint8_t {
    IntentPendingDurable,
    StagedPendingDurable,
};

/// Trusted test-only failures injected immediately before a namespace
/// operation or its following durability barrier.
enum class OOCCleanupTestOperation : std::uint8_t {
    IndexRename,
    IndexRenameParentSync,
    DataRename,
    DataRenameParentSync,
    DataUnlink,
    DataUnlinkParentSync,
    IndexUnlink,
    IndexUnlinkParentSync,
    IntentUnlink,
    IntentUnlinkParentSync,
    StagedUnlink,
    StagedUnlinkParentSync,
};

struct OOCCleanupTestHooks final {
    using StopAfter = bool (*)(OOCCleanupFaultPoint point, void* context) noexcept;
    using StopAfterPublish = bool (*)(OOCCleanupPublishFaultPoint point, void* context) noexcept;
    using FailBeforeOperation = bool (*)(OOCCleanupTestOperation operation, void* context) noexcept;

    StopAfter stop_after = nullptr;
    StopAfterPublish stop_after_publish = nullptr;
    FailBeforeOperation fail_before_operation = nullptr;
    void* context = nullptr;
};

/// Trusted test-only boundaries for the private lease protocol. Values with a
/// `Durable` suffix are emitted only after the corresponding namespace
/// mutation and durability barrier. Fresh-writer reservation values cover
/// volatile process-crash windows before lease activation.
enum class OOCPrivateLeaseFaultPoint : std::uint8_t {
    ReservedPendingDurable,
    ReservedDurable,
    StagingDirectoryDurable,
    OwnerPendingDurable,
    OwnerDurable,
    OwnedPendingDurable,
    OwnedDurable,
    FinalRenameDurable,
    FreshIndexReserved,
    FreshDataReserved,
    FreshHeadersValidated,
    FreshPairOwnershipCaptured,
    PreactiveDirectoryQuarantinedDurable,
    PreactiveDataRemovedDurable,
    PreactiveIndexRemovedDurable,
    ReservedRemovedDurable,
    OwnerRemovedDurable,
    FinalDirectoryRemovedDurable,
    OwnedRemovedDurable,
};

struct OOCPrivateLeaseTestHooks final {
    using StopAfter = bool (*)(OOCPrivateLeaseFaultPoint point, void* context) noexcept;

    StopAfter stop_after = nullptr;
    void* context = nullptr;
};

struct OOCCleanupPaths final {
    std::filesystem::path base_path;
    std::filesystem::path private_directory;
    std::filesystem::path index_path;
    std::filesystem::path data_path;
    std::filesystem::path intent_path;
    std::filesystem::path intent_pending_path;
    std::filesystem::path staged_path;
    std::filesystem::path staged_pending_path;
    std::filesystem::path lock_path;
    std::filesystem::path lease_reserved_path;
    std::filesystem::path lease_reserved_pending_path;
    std::filesystem::path lease_owned_path;
    std::filesystem::path lease_owned_pending_path;
    std::filesystem::path quarantine_index_path;
    std::filesystem::path quarantine_data_path;
};

namespace ooc_cleanup_detail {

inline constexpr std::uint64_t INTENT_MAGIC = 0x474E46534F434954ULL; // 'GNFSOCIT'
inline constexpr std::uint64_t STAGED_MAGIC = 0x474E46534F435354ULL; // 'GNFSOCST'
inline constexpr std::uint64_t INTENT_VERSION = 1;
inline constexpr std::size_t MARKER_FIELD_COUNT = 20;
inline constexpr std::size_t MARKER_PAYLOAD_BYTES = MARKER_FIELD_COUNT * sizeof(std::uint64_t);
inline constexpr std::size_t MARKER_BYTES = MARKER_PAYLOAD_BYTES + util::SHA256_DIGEST_BYTES;
inline constexpr std::uint64_t PRIVATE_LEASE_MAGIC = 0x474E46534C534531ULL; // 'GNFSLSE1'
inline constexpr std::uint64_t PRIVATE_LEASE_VERSION = 1;
inline constexpr std::size_t PRIVATE_LEASE_FIELD_COUNT = 27;
inline constexpr std::size_t PRIVATE_LEASE_PAYLOAD_BYTES =
    PRIVATE_LEASE_FIELD_COUNT * sizeof(std::uint64_t);
inline constexpr std::size_t PRIVATE_LEASE_MARKER_BYTES =
    PRIVATE_LEASE_PAYLOAD_BYTES + util::SHA256_DIGEST_BYTES;

#ifdef _WIN32
inline constexpr std::uint64_t PLATFORM_ID = 2;
#else
inline constexpr std::uint64_t PLATFORM_ID = 1;
#endif

struct Failure final {
    OOCCleanupStatus status;
    OOCCleanupStage stage;
    std::error_code error;
};

[[noreturn]] inline void fail(OOCCleanupStatus status, OOCCleanupStage stage,
                              std::error_code error = {}) {
    throw Failure{status, stage, error};
}

[[nodiscard]] inline std::error_code invalid_argument_error() noexcept {
    return std::make_error_code(std::errc::invalid_argument);
}

[[nodiscard]] inline std::error_code protocol_error() noexcept {
    return std::make_error_code(std::errc::protocol_error);
}

[[nodiscard]] inline bool path_contains_nul(const std::filesystem::path& path) noexcept {
    const auto& native = path.native();
    return native.find(std::filesystem::path::value_type{}) !=
           std::filesystem::path::string_type::npos;
}

[[nodiscard]] inline bool
path_leaf_uses_reserved_cleanup_suffix(const std::filesystem::path& leaf) noexcept {
    using Character = std::filesystem::path::value_type;
    const auto& native = leaf.native();
    const auto reserved = std::filesystem::path(".gnfs-ooc-cleanup-v1").native();
    if (native.size() < reserved.size()) {
        return false;
    }
    const auto fold_ascii = [](Character value) noexcept {
        const Character upper_a = static_cast<Character>('A');
        const Character upper_z = static_cast<Character>('Z');
        const Character case_delta = static_cast<Character>('a' - 'A');
        return value >= upper_a && value <= upper_z ? static_cast<Character>(value + case_delta)
                                                    : value;
    };
    const std::size_t offset = native.size() - reserved.size();
    for (std::size_t index = 0; index < reserved.size(); ++index) {
        if (fold_ascii(native[offset + index]) != fold_ascii(reserved[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool path_leaf_equals_ascii(const std::filesystem::path& leaf,
                                                 const char* expected) noexcept {
    using Character = std::filesystem::path::value_type;
    const auto& native = leaf.native();
    const auto expected_native = std::filesystem::path(expected).native();
    if (native.size() != expected_native.size()) {
        return false;
    }
    const auto fold_ascii = [](Character value) noexcept {
        const Character upper_a = static_cast<Character>('A');
        const Character upper_z = static_cast<Character>('Z');
        const Character case_delta = static_cast<Character>('a' - 'A');
        return value >= upper_a && value <= upper_z ? static_cast<Character>(value + case_delta)
                                                    : value;
    };
    for (std::size_t index = 0; index < native.size(); ++index) {
        if (fold_ascii(native[index]) != fold_ascii(expected_native[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool path_leaf_equals(const std::filesystem::path& left,
                                           const std::filesystem::path& right) noexcept {
    using Character = std::filesystem::path::value_type;
    const auto& left_native = left.native();
    const auto& right_native = right.native();
    if (left_native.size() != right_native.size()) {
        return false;
    }
    const auto fold_ascii = [](Character value) noexcept {
        const Character upper_a = static_cast<Character>('A');
        const Character upper_z = static_cast<Character>('Z');
        const Character case_delta = static_cast<Character>('a' - 'A');
        return value >= upper_a && value <= upper_z ? static_cast<Character>(value + case_delta)
                                                    : value;
    };
    for (std::size_t index = 0; index < left_native.size(); ++index) {
        if (fold_ascii(left_native[index]) != fold_ascii(right_native[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool path_leaf_ends_with_ascii(const std::filesystem::path& leaf,
                                                    const char* suffix) noexcept {
    using Character = std::filesystem::path::value_type;
    const auto& native = leaf.native();
    const auto suffix_native = std::filesystem::path(suffix).native();
    if (native.size() < suffix_native.size()) {
        return false;
    }
    const auto fold_ascii = [](Character value) noexcept {
        const Character upper_a = static_cast<Character>('A');
        const Character upper_z = static_cast<Character>('Z');
        const Character case_delta = static_cast<Character>('a' - 'A');
        return value >= upper_a && value <= upper_z ? static_cast<Character>(value + case_delta)
                                                    : value;
    };
    const std::size_t offset = native.size() - suffix_native.size();
    for (std::size_t index = 0; index < suffix_native.size(); ++index) {
        if (fold_ascii(native[offset + index]) != fold_ascii(suffix_native[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline std::filesystem::path append_leaf_suffix(const std::filesystem::path& parent,
                                                              const std::filesystem::path& leaf,
                                                              const char* suffix) {
    auto name = leaf.native();
    name.append(std::filesystem::path(suffix).native());
    return parent / std::filesystem::path(std::move(name));
}

[[nodiscard]] inline OOCCleanupPaths freeze_paths(const std::filesystem::path& requested) {
    if (requested.empty() || path_contains_nul(requested)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }

    std::error_code error;
    auto absolute = std::filesystem::absolute(requested, error);
    if (error) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, error);
    }
    absolute = absolute.lexically_normal();
    const auto leaf = absolute.filename();
    if (absolute.empty() || absolute == absolute.root_path() || leaf.empty() || leaf == "." ||
        leaf == ".." || path_contains_nul(leaf) || path_leaf_uses_reserved_cleanup_suffix(leaf)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }

    const auto requested_parent = absolute.parent_path();
    const auto requested_parent_leaf = requested_parent.filename();
    const bool private_sink_layout =
        path_leaf_equals_ascii(leaf, "corpus") &&
        path_leaf_ends_with_ascii(requested_parent_leaf, ".gnfs-sink-lease");
    const auto parent_to_freeze =
        private_sink_layout ? requested_parent.parent_path() : requested_parent;
    auto frozen_parent = std::filesystem::weakly_canonical(parent_to_freeze, error);
    if (error || frozen_parent.empty() || !frozen_parent.is_absolute()) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
             error ? error : invalid_argument_error());
    }
    frozen_parent = frozen_parent.lexically_normal();

    std::filesystem::path artifact_parent = frozen_parent;
    std::filesystem::path private_directory;
    std::filesystem::path lock_parent = frozen_parent;
    std::filesystem::path lock_leaf = leaf;
    if (private_sink_layout) {
        if (requested_parent_leaf.empty() || requested_parent_leaf == "." ||
            requested_parent_leaf == ".." || path_contains_nul(requested_parent_leaf) ||
            path_leaf_uses_reserved_cleanup_suffix(requested_parent_leaf)) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        artifact_parent = frozen_parent / requested_parent_leaf;
        private_directory = artifact_parent;
        lock_leaf = requested_parent_leaf;
    }

    const auto base = artifact_parent / leaf;
    return OOCCleanupPaths{
        .base_path = base,
        .private_directory = private_directory,
        .index_path = append_leaf_suffix(artifact_parent, leaf, ".relidx"),
        .data_path = append_leaf_suffix(artifact_parent, leaf, ".reldata"),
        .intent_path = append_leaf_suffix(artifact_parent, leaf, ".gnfs-ooc-cleanup-v1.intent"),
        .intent_pending_path =
            append_leaf_suffix(artifact_parent, leaf, ".gnfs-ooc-cleanup-v1.intent.pending"),
        .staged_path = append_leaf_suffix(artifact_parent, leaf, ".gnfs-ooc-cleanup-v1.staged"),
        .staged_pending_path =
            append_leaf_suffix(artifact_parent, leaf, ".gnfs-ooc-cleanup-v1.staged.pending"),
        .lock_path = append_leaf_suffix(lock_parent, lock_leaf, ".gnfs-ooc-cleanup-v1.lock"),
        .lease_reserved_path =
            append_leaf_suffix(lock_parent, lock_leaf, ".gnfs-private-lease-v1.reserved"),
        .lease_reserved_pending_path =
            append_leaf_suffix(lock_parent, lock_leaf, ".gnfs-private-lease-v1.reserved.pending"),
        .lease_owned_path =
            append_leaf_suffix(lock_parent, lock_leaf, ".gnfs-private-lease-v1.owned"),
        .lease_owned_pending_path =
            append_leaf_suffix(lock_parent, lock_leaf, ".gnfs-private-lease-v1.owned.pending"),
        .quarantine_index_path =
            append_leaf_suffix(artifact_parent, leaf, ".gnfs-ooc-cleanup-v1.relidx"),
        .quarantine_data_path =
            append_leaf_suffix(artifact_parent, leaf, ".gnfs-ooc-cleanup-v1.reldata"),
    };
}

[[nodiscard]] inline std::array<std::uint64_t, 2> allocate_private_lease_id() noexcept {
    static std::atomic<std::uint64_t> sequence{1};
    auto mix = [](std::uint64_t value) noexcept {
        value += 0x9E3779B97F4A7C15ULL;
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    };

    std::uint64_t seed = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
#ifdef _WIN32
    seed ^= static_cast<std::uint64_t>(::GetCurrentProcessId()) << 32U;
#else
    seed ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(::getpid())) << 32U;
#endif
    seed ^= sequence.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t entropy = 0;
    try {
        std::random_device random;
        seed ^= static_cast<std::uint64_t>(random()) << 32U;
        seed ^= static_cast<std::uint64_t>(random());
        entropy ^= static_cast<std::uint64_t>(random()) << 32U;
        entropy ^= static_cast<std::uint64_t>(random());
    } catch (...) {
        entropy = seed ^ 0xD1B54A32D192ED03ULL;
    }
    std::array<std::uint64_t, 2> lease_id{
        mix(seed),
        mix(entropy ^ seed ^ 0x94D049BB133111EBULL),
    };
    if (lease_id[0] == 0 && lease_id[1] == 0) {
        lease_id[1] = 1;
    }
    return lease_id;
}

[[nodiscard]] inline std::string
private_lease_id_hex(const std::array<std::uint64_t, 2>& lease_id) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string encoded(32, '0');
    for (std::size_t word = 0; word < lease_id.size(); ++word) {
        for (std::size_t digit = 0; digit < 16; ++digit) {
            const auto shift = static_cast<unsigned>((15 - digit) * 4);
            encoded[word * 16 + digit] =
                digits[static_cast<std::size_t>((lease_id[word] >> shift) & 0x0fU)];
        }
    }
    return encoded;
}

[[nodiscard]] inline std::filesystem::path
private_lease_staging_path(const OOCCleanupPaths& paths,
                           const std::array<std::uint64_t, 2>& lease_id) {
    auto leaf = paths.private_directory.filename().native();
    leaf.append(std::filesystem::path(".gnfs-private-lease-v1.stage-").native());
    leaf.append(std::filesystem::path(private_lease_id_hex(lease_id)).native());
    return paths.private_directory.parent_path() / std::filesystem::path(std::move(leaf));
}

[[nodiscard]] inline std::filesystem::path
private_lease_owner_path(const std::filesystem::path& directory) {
    return directory / ".gnfs-private-lease-v1.owner";
}

[[nodiscard]] inline std::filesystem::path
private_lease_owner_pending_path(const std::filesystem::path& directory) {
    return directory / ".gnfs-private-lease-v1.owner.pending";
}

struct FileIdentity final {
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::uint64_t third = 0;
    std::uint64_t size = 0;

    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

struct OwnershipProof final {
    std::filesystem::path base_path;
    std::uint64_t store_id = 0;
    std::array<std::uint64_t, 3> index_identity{};
    std::array<std::uint64_t, 3> data_identity{};
};

[[nodiscard]] inline std::array<std::uint64_t, 3>
stable_identity(const FileIdentity& identity) noexcept {
    return {identity.first, identity.second, identity.third};
}

enum class InspectKind : std::uint8_t {
    Missing,
    Present,
    Rejected,
    Error,
};

struct InspectResult final {
    InspectKind kind = InspectKind::Error;
    FileIdentity identity;
    std::vector<std::byte> bytes;
    std::error_code error;
};

#ifdef _WIN32

[[nodiscard]] inline std::error_code windows_error(DWORD value) noexcept {
    return {static_cast<int>(value), std::system_category()};
}

[[nodiscard]] inline std::optional<FileIdentity>
windows_identity(HANDLE handle, const BY_HANDLE_FILE_INFORMATION& info) noexcept {
    FILE_ID_INFO file_id{};
    if (!::GetFileInformationByHandleEx(handle, FileIdInfo, &file_id, sizeof(file_id))) {
        return std::nullopt;
    }

    const auto size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) |
                      static_cast<std::uint64_t>(info.nFileSizeLow);
    std::uint64_t file_id_low = 0;
    std::uint64_t file_id_high = 0;
    static_assert(sizeof(file_id.FileId.Identifier) == sizeof(file_id_low) + sizeof(file_id_high));
    std::memcpy(&file_id_low, file_id.FileId.Identifier, sizeof(file_id_low));
    std::memcpy(&file_id_high, file_id.FileId.Identifier + sizeof(file_id_low),
                sizeof(file_id_high));
    return FileIdentity{
        .first = static_cast<std::uint64_t>(file_id.VolumeSerialNumber),
        .second = file_id_low,
        .third = file_id_high,
        .size = size,
    };
}

[[nodiscard]] inline bool
windows_regular_single_link(const BY_HANDLE_FILE_INFORMATION& info) noexcept {
    return (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
           (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 && info.nNumberOfLinks == 1;
}

[[nodiscard]] inline InspectResult inspect_file(const std::filesystem::path& path,
                                                std::size_t bytes_to_read,
                                                bool require_exact_size) {
    const HANDLE handle = ::CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return InspectResult{
                .kind = InspectKind::Missing,
                .identity = {},
                .bytes = {},
                .error = {},
            };
        }
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = windows_error(code),
        };
    }

    BY_HANDLE_FILE_INFORMATION before{};
    if (!::GetFileInformationByHandle(handle, &before)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = windows_error(code),
        };
    }
    const auto before_identity = windows_identity(handle, before);
    if (!before_identity || !windows_regular_single_link(before) ||
        before_identity->size < bytes_to_read ||
        (require_exact_size && before_identity->size != bytes_to_read)) {
        (void)::CloseHandle(handle);
        return InspectResult{
            .kind = InspectKind::Rejected,
            .identity = {},
            .bytes = {},
            .error = {},
        };
    }

    std::vector<std::byte> bytes(bytes_to_read);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD request = static_cast<DWORD>(
            (std::min)(bytes.size() - offset,
                       static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD read = 0;
        if (!::ReadFile(handle, bytes.data() + offset, request, &read, nullptr)) {
            const DWORD code = ::GetLastError();
            (void)::CloseHandle(handle);
            return InspectResult{
                .kind = InspectKind::Error,
                .identity = {},
                .bytes = {},
                .error = windows_error(code),
            };
        }
        if (read == 0) {
            (void)::CloseHandle(handle);
            return InspectResult{
                .kind = InspectKind::Rejected,
                .identity = {},
                .bytes = {},
                .error = {},
            };
        }
        offset += static_cast<std::size_t>(read);
    }

    BY_HANDLE_FILE_INFORMATION after{};
    if (!::GetFileInformationByHandle(handle, &after)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = windows_error(code),
        };
    }
    const auto after_identity = windows_identity(handle, after);
    if (!after_identity) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = windows_error(code),
        };
    }
    if (!::CloseHandle(handle)) {
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = windows_error(::GetLastError()),
        };
    }
    if (!windows_regular_single_link(after) || *after_identity != *before_identity) {
        return InspectResult{
            .kind = InspectKind::Rejected,
            .identity = {},
            .bytes = {},
            .error = {},
        };
    }
    return InspectResult{
        .kind = InspectKind::Present,
        .identity = *before_identity,
        .bytes = std::move(bytes),
        .error = {},
    };
}

#else

[[nodiscard]] inline std::error_code posix_error(int value) noexcept {
    return {value, std::generic_category()};
}

[[nodiscard]] inline FileIdentity posix_identity(const struct stat& info) noexcept {
    return FileIdentity{
        .first = static_cast<std::uint64_t>(info.st_dev),
        .second = static_cast<std::uint64_t>(info.st_ino),
        .third = 0,
        .size = static_cast<std::uint64_t>(info.st_size),
    };
}

[[nodiscard]] inline bool posix_regular_single_link(const struct stat& info) noexcept {
    return S_ISREG(info.st_mode) && info.st_nlink == 1 && info.st_size >= 0;
}

[[nodiscard]] inline InspectResult inspect_file(const std::filesystem::path& path,
                                                std::size_t bytes_to_read,
                                                bool require_exact_size) {
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        const int saved_errno = errno;
        if (saved_errno == ENOENT) {
            return InspectResult{
                .kind = InspectKind::Missing,
                .identity = {},
                .bytes = {},
                .error = {},
            };
        }
        if (saved_errno == ELOOP) {
            return InspectResult{
                .kind = InspectKind::Rejected,
                .identity = {},
                .bytes = {},
                .error = {},
            };
        }
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = posix_error(saved_errno),
        };
    }

    struct stat before{};
    if (::fstat(descriptor, &before) != 0) {
        const int saved_errno = errno;
        (void)::close(descriptor);
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = posix_error(saved_errno),
        };
    }
    const FileIdentity before_identity = posix_identity(before);
    if (!posix_regular_single_link(before) || before_identity.size < bytes_to_read ||
        (require_exact_size && before_identity.size != bytes_to_read)) {
        (void)::close(descriptor);
        return InspectResult{
            .kind = InspectKind::Rejected,
            .identity = {},
            .bytes = {},
            .error = {},
        };
    }

    std::vector<std::byte> bytes(bytes_to_read);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::pread(descriptor, bytes.data() + offset, bytes.size() - offset,
                                      static_cast<off_t>(offset));
        if (count < 0) {
            const int saved_errno = errno;
            if (saved_errno == EINTR) {
                continue;
            }
            (void)::close(descriptor);
            return InspectResult{
                .kind = InspectKind::Error,
                .identity = {},
                .bytes = {},
                .error = posix_error(saved_errno),
            };
        }
        if (count == 0) {
            (void)::close(descriptor);
            return InspectResult{
                .kind = InspectKind::Rejected,
                .identity = {},
                .bytes = {},
                .error = {},
            };
        }
        offset += static_cast<std::size_t>(count);
    }

    struct stat after{};
    if (::fstat(descriptor, &after) != 0) {
        const int saved_errno = errno;
        (void)::close(descriptor);
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = posix_error(saved_errno),
        };
    }
    if (::close(descriptor) != 0) {
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = posix_error(errno),
        };
    }
    if (!posix_regular_single_link(after) || posix_identity(after) != before_identity) {
        return InspectResult{
            .kind = InspectKind::Rejected,
            .identity = {},
            .bytes = {},
            .error = {},
        };
    }
    return InspectResult{
        .kind = InspectKind::Present,
        .identity = before_identity,
        .bytes = std::move(bytes),
        .error = {},
    };
}

#endif

[[nodiscard]] inline std::uint64_t read_native_u64(std::span<const std::byte> bytes,
                                                   std::size_t offset) {
    if (offset > bytes.size() || sizeof(std::uint64_t) > bytes.size() - offset) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

struct ArtifactFingerprint final {
    FileIdentity identity;
    std::uint64_t header_magic = 0;
    std::uint64_t header_version = 0;
    std::uint64_t header_store_id = 0;
    std::uint64_t header_count = 0;

    friend bool operator==(const ArtifactFingerprint&, const ArtifactFingerprint&) = default;
};

enum class ArtifactKind : std::uint8_t {
    Index,
    Data,
};

[[nodiscard]] inline std::size_t artifact_header_bytes(ArtifactKind kind) noexcept {
    return kind == ArtifactKind::Index ? OOCRelationStoreFormat::INDEX_HEADER_BYTES
                                       : OOCRelationStoreFormat::DATA_HEADER_BYTES;
}

[[nodiscard]] inline std::optional<ArtifactFingerprint>
artifact_fingerprint(const InspectResult& inspected, ArtifactKind kind,
                     std::uint64_t expected_store_id) {
    if (inspected.kind != InspectKind::Present ||
        inspected.bytes.size() < OOCRelationStoreFormat::DATA_HEADER_BYTES) {
        return std::nullopt;
    }

    const auto magic = read_native_u64(inspected.bytes, 0);
    const auto version = read_native_u64(inspected.bytes, 8);
    const auto store_id = read_native_u64(inspected.bytes, 16);
    const bool valid_magic = kind == ArtifactKind::Index
                                 ? magic == OOCRelationStoreFormat::MAGIC_V3_FINAL ||
                                       magic == OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE
                                 : magic == OOCRelationStoreFormat::MAGIC_V3_DATA;
    const std::uint64_t minimum_size = kind == ArtifactKind::Index
                                           ? OOCRelationStoreFormat::INDEX_HEADER_BYTES
                                           : OOCRelationStoreFormat::DATA_HEADER_BYTES;
    if (!valid_magic || version != OOCRelationStoreFormat::FORMAT_VERSION_V3 || store_id == 0 ||
        store_id != expected_store_id || inspected.identity.size < minimum_size) {
        return std::nullopt;
    }

    return ArtifactFingerprint{
        .identity = inspected.identity,
        .header_magic = magic,
        .header_version = version,
        .header_store_id = store_id,
        .header_count =
            kind == ArtifactKind::Index
                ? read_native_u64(inspected.bytes, OOCRelationStoreFormat::INDEX_COUNT_OFFSET)
                : 0,
    };
}

struct IntentRecord final {
    std::uint64_t platform_id = PLATFORM_ID;
    std::uint64_t store_id = 0;
    ArtifactFingerprint index;
    ArtifactFingerprint data;

    friend bool operator==(const IntentRecord&, const IntentRecord&) = default;
};

enum class PrivateLeasePhase : std::uint64_t {
    Reserved = 1,
    Owner = 2,
    Owned = 3,
};

enum class PrivateLeaseCapability : std::uint64_t {
    RollbackStagingOnly = 1,
    RemoveOwnerAndEmptyLease = 2,
    RollbackPreactivePairAndLease = 3,
};

struct PrivateLeaseRecord final {
    std::uint64_t platform_id = PLATFORM_ID;
    PrivateLeasePhase phase = PrivateLeasePhase::Reserved;
    PrivateLeaseCapability capability = PrivateLeaseCapability::RollbackStagingOnly;
    std::array<std::uint64_t, 2> lease_id{};
    std::array<std::uint64_t, 4> base_path_digest{};
    std::array<std::uint64_t, 3> parent_identity{};
    std::array<std::uint64_t, 3> lock_identity{};
    std::array<std::uint64_t, 3> directory_identity{};
    std::array<std::uint64_t, 4> reserved_digest{};
    std::array<std::uint64_t, 3> owner_identity{};

    friend bool operator==(const PrivateLeaseRecord&, const PrivateLeaseRecord&) = default;
};

inline void append_u64(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

[[nodiscard]] inline std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t& cursor);

template <std::size_t Size>
inline void append_u64_array(std::vector<std::byte>& bytes,
                             const std::array<std::uint64_t, Size>& values) {
    for (const auto value : values) {
        append_u64(bytes, value);
    }
}

template <std::size_t Size>
[[nodiscard]] inline std::array<std::uint64_t, Size>
read_u64_array(std::span<const std::byte> bytes, std::size_t& cursor) {
    std::array<std::uint64_t, Size> values{};
    for (auto& value : values) {
        value = read_u64(bytes, cursor);
    }
    return values;
}

[[nodiscard]] inline std::array<std::uint64_t, 4>
digest_words(const util::Sha256Digest& digest) noexcept {
    std::array<std::uint64_t, 4> words{};
    for (std::size_t word = 0; word < words.size(); ++word) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            const auto index = word * sizeof(std::uint64_t) + shift / 8;
            words[word] |=
                static_cast<std::uint64_t>(std::to_integer<unsigned char>(digest.bytes[index]))
                << shift;
        }
    }
    return words;
}

[[nodiscard]] inline std::array<std::uint64_t, 4> digest_words(std::span<const std::byte> bytes) {
    const auto digest = util::sha256(bytes);
    if (!digest) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    return digest_words(*digest);
}

[[nodiscard]] inline std::array<std::uint64_t, 4>
frozen_path_digest(const std::filesystem::path& path) {
    const auto& native = path.native();
    const auto characters =
        std::span<const std::filesystem::path::value_type>(native.data(), native.size());
    return digest_words(std::as_bytes(characters));
}

[[nodiscard]] inline bool all_zero(std::span<const std::uint64_t> values) noexcept {
    return std::all_of(values.begin(), values.end(),
                       [](std::uint64_t value) noexcept { return value == 0; });
}

[[nodiscard]] inline bool
private_lease_record_shape_valid(const PrivateLeaseRecord& record) noexcept {
    if (record.platform_id != PLATFORM_ID || all_zero(record.lease_id) ||
        all_zero(record.base_path_digest) || all_zero(record.parent_identity) ||
        all_zero(record.lock_identity)) {
        return false;
    }
    switch (record.phase) {
    case PrivateLeasePhase::Reserved:
        return record.capability == PrivateLeaseCapability::RollbackStagingOnly &&
               all_zero(record.directory_identity) && all_zero(record.reserved_digest) &&
               all_zero(record.owner_identity);
    case PrivateLeasePhase::Owner:
        return record.capability == PrivateLeaseCapability::RemoveOwnerAndEmptyLease &&
               !all_zero(record.directory_identity) && !all_zero(record.reserved_digest) &&
               all_zero(record.owner_identity);
    case PrivateLeasePhase::Owned:
        return (record.capability == PrivateLeaseCapability::RemoveOwnerAndEmptyLease ||
                record.capability == PrivateLeaseCapability::RollbackPreactivePairAndLease) &&
               !all_zero(record.directory_identity) && !all_zero(record.reserved_digest) &&
               !all_zero(record.owner_identity);
    }
    return false;
}

[[nodiscard]] inline std::vector<std::byte>
serialize_private_lease_marker(const PrivateLeaseRecord& record) {
    if (!private_lease_record_shape_valid(record)) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    std::vector<std::byte> bytes;
    bytes.reserve(PRIVATE_LEASE_MARKER_BYTES);
    append_u64(bytes, PRIVATE_LEASE_MAGIC);
    append_u64(bytes, PRIVATE_LEASE_VERSION);
    append_u64(bytes, record.platform_id);
    append_u64(bytes, static_cast<std::uint64_t>(record.phase));
    append_u64(bytes, static_cast<std::uint64_t>(record.capability));
    append_u64_array(bytes, record.lease_id);
    append_u64_array(bytes, record.base_path_digest);
    append_u64_array(bytes, record.parent_identity);
    append_u64_array(bytes, record.lock_identity);
    append_u64_array(bytes, record.directory_identity);
    append_u64_array(bytes, record.reserved_digest);
    append_u64_array(bytes, record.owner_identity);
    if (bytes.size() != PRIVATE_LEASE_PAYLOAD_BYTES) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    const auto digest = util::sha256(bytes);
    if (!digest) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    bytes.insert(bytes.end(), digest->bytes.begin(), digest->bytes.end());
    return bytes;
}

[[nodiscard]] inline PrivateLeaseRecord
parse_private_lease_marker(std::span<const std::byte> bytes) {
    if (bytes.size() != PRIVATE_LEASE_MARKER_BYTES) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    const auto digest = util::sha256(bytes.first(PRIVATE_LEASE_PAYLOAD_BYTES));
    if (!digest ||
        !std::equal(digest->bytes.begin(), digest->bytes.end(),
                    bytes.begin() + static_cast<std::ptrdiff_t>(PRIVATE_LEASE_PAYLOAD_BYTES))) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    std::size_t cursor = 0;
    if (read_u64(bytes, cursor) != PRIVATE_LEASE_MAGIC ||
        read_u64(bytes, cursor) != PRIVATE_LEASE_VERSION) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    PrivateLeaseRecord record;
    record.platform_id = read_u64(bytes, cursor);
    record.phase = static_cast<PrivateLeasePhase>(read_u64(bytes, cursor));
    record.capability = static_cast<PrivateLeaseCapability>(read_u64(bytes, cursor));
    record.lease_id = read_u64_array<2>(bytes, cursor);
    record.base_path_digest = read_u64_array<4>(bytes, cursor);
    record.parent_identity = read_u64_array<3>(bytes, cursor);
    record.lock_identity = read_u64_array<3>(bytes, cursor);
    record.directory_identity = read_u64_array<3>(bytes, cursor);
    record.reserved_digest = read_u64_array<4>(bytes, cursor);
    record.owner_identity = read_u64_array<3>(bytes, cursor);
    if (cursor != PRIVATE_LEASE_PAYLOAD_BYTES || !private_lease_record_shape_valid(record)) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    return record;
}

[[nodiscard]] inline std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t& cursor) {
    if (cursor > bytes.size() || sizeof(std::uint64_t) > bytes.size() - cursor) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[cursor++]))
                 << shift;
    }
    return value;
}

inline void append_fingerprint(std::vector<std::byte>& bytes,
                               const ArtifactFingerprint& fingerprint) {
    append_u64(bytes, fingerprint.identity.first);
    append_u64(bytes, fingerprint.identity.second);
    append_u64(bytes, fingerprint.identity.third);
    append_u64(bytes, fingerprint.identity.size);
    append_u64(bytes, fingerprint.header_magic);
    append_u64(bytes, fingerprint.header_version);
    append_u64(bytes, fingerprint.header_store_id);
    append_u64(bytes, fingerprint.header_count);
}

[[nodiscard]] inline ArtifactFingerprint read_fingerprint(std::span<const std::byte> bytes,
                                                          std::size_t& cursor) {
    ArtifactFingerprint fingerprint;
    fingerprint.identity.first = read_u64(bytes, cursor);
    fingerprint.identity.second = read_u64(bytes, cursor);
    fingerprint.identity.third = read_u64(bytes, cursor);
    fingerprint.identity.size = read_u64(bytes, cursor);
    fingerprint.header_magic = read_u64(bytes, cursor);
    fingerprint.header_version = read_u64(bytes, cursor);
    fingerprint.header_store_id = read_u64(bytes, cursor);
    fingerprint.header_count = read_u64(bytes, cursor);
    return fingerprint;
}

[[nodiscard]] inline std::vector<std::byte> serialize_marker(const IntentRecord& intent,
                                                             std::uint64_t marker_magic) {
    std::vector<std::byte> bytes;
    bytes.reserve(MARKER_BYTES);
    append_u64(bytes, marker_magic);
    append_u64(bytes, INTENT_VERSION);
    append_u64(bytes, intent.platform_id);
    append_u64(bytes, intent.store_id);
    append_fingerprint(bytes, intent.index);
    append_fingerprint(bytes, intent.data);
    if (bytes.size() != MARKER_PAYLOAD_BYTES) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    const auto digest = util::sha256(bytes);
    if (!digest) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    bytes.insert(bytes.end(), digest->bytes.begin(), digest->bytes.end());
    return bytes;
}

[[nodiscard]] inline IntentRecord parse_marker(std::span<const std::byte> bytes,
                                               std::uint64_t expected_magic) {
    if (bytes.size() != MARKER_BYTES) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    const auto digest = util::sha256(bytes.first(MARKER_PAYLOAD_BYTES));
    if (!digest || !std::equal(digest->bytes.begin(), digest->bytes.end(),
                               bytes.begin() + static_cast<std::ptrdiff_t>(MARKER_PAYLOAD_BYTES))) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }

    std::size_t cursor = 0;
    if (read_u64(bytes, cursor) != expected_magic || read_u64(bytes, cursor) != INTENT_VERSION) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }

    IntentRecord intent;
    intent.platform_id = read_u64(bytes, cursor);
    intent.store_id = read_u64(bytes, cursor);
    intent.index = read_fingerprint(bytes, cursor);
    intent.data = read_fingerprint(bytes, cursor);
    if (cursor != MARKER_PAYLOAD_BYTES || intent.platform_id != PLATFORM_ID ||
        intent.store_id == 0 || intent.index.header_store_id != intent.store_id ||
        intent.data.header_store_id != intent.store_id ||
        intent.index.header_version != OOCRelationStoreFormat::FORMAT_VERSION_V3 ||
        intent.data.header_version != OOCRelationStoreFormat::FORMAT_VERSION_V3 ||
        (intent.index.header_magic != OOCRelationStoreFormat::MAGIC_V3_FINAL &&
         intent.index.header_magic != OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE) ||
        intent.data.header_magic != OOCRelationStoreFormat::MAGIC_V3_DATA ||
        intent.data.header_count != 0 ||
        intent.index.identity.size < OOCRelationStoreFormat::INDEX_HEADER_BYTES ||
        intent.data.identity.size < OOCRelationStoreFormat::DATA_HEADER_BYTES ||
        intent.index.identity == intent.data.identity) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    if (intent.index.header_magic == OOCRelationStoreFormat::MAGIC_V3_FINAL) {
        constexpr std::uint64_t offset_bytes = sizeof(std::uint64_t);
        if (intent.index.header_count >
            (std::numeric_limits<std::uint64_t>::max() -
             OOCRelationStoreFormat::INDEX_HEADER_BYTES - offset_bytes) /
                offset_bytes) {
            fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
        }
        const std::uint64_t expected_index_size = OOCRelationStoreFormat::INDEX_HEADER_BYTES +
                                                  (intent.index.header_count + 1) * offset_bytes;
        if (intent.index.identity.size != expected_index_size) {
            fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
        }
    }
    return intent;
}

inline void sync_parent_directory(const std::filesystem::path& parent, OOCCleanupStage stage) {
#ifdef _WIN32
    const HANDLE directory = ::CreateFileW(
        parent.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(::GetLastError()));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!::GetFileInformationByHandle(directory, &information) ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        const DWORD code =
            ::GetLastError() == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : ::GetLastError();
        (void)::CloseHandle(directory);
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(code));
    }
    if (!::FlushFileBuffers(directory)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(directory);
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(code));
    }
    if (!::CloseHandle(directory)) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(::GetLastError()));
    }
#else
    int descriptor = -1;
    do {
        descriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(errno));
    }
    int result = -1;
    do {
#if defined(__APPLE__)
        result = ::fcntl(descriptor, F_FULLFSYNC);
#else
        result = ::fsync(descriptor);
#endif
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        const int saved_errno = errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(saved_errno));
    }
    if (::close(descriptor) != 0) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(errno));
    }
#endif
}

class BaseLock final {
public:
    BaseLock(const BaseLock&) = delete;
    BaseLock& operator=(const BaseLock&) = delete;
    BaseLock(BaseLock&&) = delete;
    BaseLock& operator=(BaseLock&&) = delete;

    explicit BaseLock(const std::filesystem::path& path, bool allow_create = true) : path_(path) {
#ifdef _WIN32
        ::SetLastError(ERROR_SUCCESS);
        handle_ = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                allow_create ? OPEN_ALWAYS : OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                                    FILE_FLAG_WRITE_THROUGH,
                                nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
                fail(OOCCleanupStatus::Busy, OOCCleanupStage::None, windows_error(code));
            }
            if (!allow_create && (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                     windows_error(code));
            }
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(code));
        }
        const bool created = allow_create && ::GetLastError() != ERROR_ALREADY_EXISTS;
        BY_HANDLE_FILE_INFORMATION information{};
        std::optional<FileIdentity> held_identity;
        if (!::GetFileInformationByHandle(handle_, &information) ||
            !windows_regular_single_link(information) ||
            !(held_identity = windows_identity(handle_, information))) {
            const DWORD code =
                ::GetLastError() == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : ::GetLastError();
            release_noexcept();
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, windows_error(code));
        }
        identity_ = stable_identity(*held_identity);
        if (created) {
            if (!::FlushFileBuffers(handle_)) {
                const DWORD code = ::GetLastError();
                release_noexcept();
                fail(OOCCleanupStatus::DurabilityFailure, OOCCleanupStage::None,
                     windows_error(code));
            }
            try {
                sync_parent_directory(path.parent_path(), OOCCleanupStage::None);
            } catch (...) {
                release_noexcept();
                throw;
            }
            if (!::FlushFileBuffers(handle_)) {
                const DWORD code = ::GetLastError();
                release_noexcept();
                fail(OOCCleanupStatus::DurabilityFailure, OOCCleanupStage::None,
                     windows_error(code));
            }
        }
#else
        bool created = false;
        if (allow_create) {
            do {
                descriptor_ =
                    ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            } while (descriptor_ < 0 && errno == EINTR);
            if (descriptor_ >= 0) {
                created = true;
            } else if (errno == EEXIST) {
                do {
                    descriptor_ = ::open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
                } while (descriptor_ < 0 && errno == EINTR);
            }
        } else {
            do {
                descriptor_ = ::open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
            } while (descriptor_ < 0 && errno == EINTR);
        }
        if (descriptor_ < 0) {
            const int saved_errno = errno;
            if (!allow_create && saved_errno == ENOENT) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                     posix_error(saved_errno));
            }
            fail(saved_errno == ELOOP ? OOCCleanupStatus::NamespaceConflict
                                      : OOCCleanupStatus::IoFailure,
                 OOCCleanupStage::None, posix_error(saved_errno));
        }

        struct stat held{};
        struct stat named{};
        if (::fstat(descriptor_, &held) != 0 || ::lstat(path.c_str(), &named) != 0 ||
            !posix_regular_single_link(held) || !S_ISREG(named.st_mode) || named.st_nlink != 1 ||
            held.st_dev != named.st_dev || held.st_ino != named.st_ino) {
            const int saved_errno = errno == 0 ? EACCES : errno;
            release_noexcept();
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                 posix_error(saved_errno));
        }
        identity_ = stable_identity(posix_identity(held));
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const int saved_errno = errno;
            release_noexcept();
            if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
                fail(OOCCleanupStatus::Busy, OOCCleanupStage::None, posix_error(saved_errno));
            }
            if (saved_errno == ENOTSUP || saved_errno == EOPNOTSUPP) {
                fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None,
                     posix_error(saved_errno));
            }
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(saved_errno));
        }
        struct stat locked_name{};
        if (::lstat(path.c_str(), &locked_name) != 0 || locked_name.st_dev != held.st_dev ||
            locked_name.st_ino != held.st_ino || !S_ISREG(locked_name.st_mode) ||
            locked_name.st_nlink != 1) {
            const int saved_errno = errno == 0 ? EACCES : errno;
            release_noexcept();
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                 posix_error(saved_errno));
        }
        if (created) {
            const auto sync_held_lock = [&]() noexcept {
                int result = -1;
                do {
#if defined(__APPLE__)
                    result = ::fcntl(descriptor_, F_FULLFSYNC);
#else
                    result = ::fsync(descriptor_);
#endif
                } while (result != 0 && errno == EINTR);
                return result;
            };
            if (sync_held_lock() != 0) {
                const int saved_errno = errno;
                release_noexcept();
                fail(OOCCleanupStatus::DurabilityFailure, OOCCleanupStage::None,
                     posix_error(saved_errno));
            }
            try {
                sync_parent_directory(path.parent_path(), OOCCleanupStage::None);
            } catch (...) {
                release_noexcept();
                throw;
            }
            if (sync_held_lock() != 0) {
                const int saved_errno = errno;
                release_noexcept();
                fail(OOCCleanupStatus::DurabilityFailure, OOCCleanupStage::None,
                     posix_error(saved_errno));
            }
        }
#endif
    }

    [[nodiscard]] const std::array<std::uint64_t, 3>& identity() const noexcept {
        return identity_;
    }

    [[nodiscard]] bool matches(const std::filesystem::path& path) const noexcept {
        return path_ == path;
    }

    ~BaseLock() {
        release_noexcept();
    }

private:
    void release_noexcept() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            (void)::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (descriptor_ >= 0) {
            // close() releases an ordinary in-process flock when this is the
            // last reference. After fork(), however, the child owns a
            // duplicate descriptor for the same open-file description.
            // Explicit LOCK_UN in either process would unlock both copies and
            // let recovery race a still-running worker; close-only keeps the
            // lock until the final inherited descriptor is gone.
            (void)::close(descriptor_);
            descriptor_ = -1;
        }
#endif
    }

#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
    std::filesystem::path path_;
    std::array<std::uint64_t, 3> identity_{};
};

inline void confirm_file_durable(const std::filesystem::path& path, const FileIdentity& expected,
                                 const std::filesystem::path& parent, OOCCleanupStage stage) {
#ifdef _WIN32
    const HANDLE handle = ::CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(::GetLastError()));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    const bool inspected = ::GetFileInformationByHandle(handle, &information) != FALSE;
    const auto identity = inspected ? windows_identity(handle, information) : std::nullopt;
    if (!inspected || !identity || !windows_regular_single_link(information) ||
        *identity != expected) {
        const DWORD code =
            ::GetLastError() == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::IntentCorrupt, stage, windows_error(code));
    }
    if (!::FlushFileBuffers(handle)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(code));
    }
    try {
        sync_parent_directory(parent, stage);
    } catch (...) {
        (void)::CloseHandle(handle);
        throw;
    }
    if (!::FlushFileBuffers(handle)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(code));
    }
    if (!::CloseHandle(handle)) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(::GetLastError()));
    }
#else
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(errno));
    }
    struct stat information{};
    if (::fstat(descriptor, &information) != 0 || !posix_regular_single_link(information) ||
        posix_identity(information) != expected) {
        const int saved_errno = errno == 0 ? EACCES : errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::IntentCorrupt, stage, posix_error(saved_errno));
    }
    const auto sync_held_file = [&]() noexcept {
        int result = -1;
        do {
#if defined(__APPLE__)
            result = ::fcntl(descriptor, F_FULLFSYNC);
#else
            result = ::fsync(descriptor);
#endif
        } while (result != 0 && errno == EINTR);
        return result;
    };
    if (sync_held_file() != 0) {
        const int saved_errno = errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(saved_errno));
    }
    try {
        sync_parent_directory(parent, stage);
    } catch (...) {
        (void)::close(descriptor);
        throw;
    }
    if (sync_held_file() != 0) {
        const int saved_errno = errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(saved_errno));
    }
    if (::close(descriptor) != 0) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(errno));
    }
#endif
}

enum class RenameResult : std::uint8_t {
    Succeeded,
    DestinationExists,
    Unsupported,
    Failed,
};

struct RenameOutcome final {
    RenameResult result = RenameResult::Failed;
    std::error_code error;
};

[[nodiscard]] inline RenameOutcome
rename_no_replace(const std::filesystem::path& source,
                  const std::filesystem::path& destination) noexcept {
#ifdef _WIN32
    if (::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return RenameOutcome{.result = RenameResult::Succeeded, .error = {}};
    }
    const DWORD code = ::GetLastError();
    if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
        return RenameOutcome{
            .result = RenameResult::DestinationExists,
            .error = windows_error(code),
        };
    }
    return RenameOutcome{.result = RenameResult::Failed, .error = windows_error(code)};
#elif defined(__APPLE__)
    if (::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0) {
        return RenameOutcome{.result = RenameResult::Succeeded, .error = {}};
    }
    const int saved_errno = errno;
    if (saved_errno == EEXIST) {
        return RenameOutcome{
            .result = RenameResult::DestinationExists,
            .error = posix_error(saved_errno),
        };
    }
    return RenameOutcome{.result = RenameResult::Failed, .error = posix_error(saved_errno)};
#elif defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned int rename_noreplace = 1U;
    if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                  rename_noreplace) == 0) {
        return RenameOutcome{.result = RenameResult::Succeeded, .error = {}};
    }
    const int saved_errno = errno;
    if (saved_errno == EEXIST) {
        return RenameOutcome{
            .result = RenameResult::DestinationExists,
            .error = posix_error(saved_errno),
        };
    }
    if (saved_errno == ENOSYS || saved_errno == EINVAL || saved_errno == EOPNOTSUPP) {
        return RenameOutcome{.result = RenameResult::Unsupported,
                             .error = posix_error(saved_errno)};
    }
    return RenameOutcome{.result = RenameResult::Failed, .error = posix_error(saved_errno)};
#else
    (void)source;
    (void)destination;
    return RenameOutcome{
        .result = RenameResult::Unsupported,
        .error = std::make_error_code(std::errc::operation_not_supported),
    };
#endif
}

inline void remove_file(const std::filesystem::path& path, OOCCleanupStage stage) {
#ifdef _WIN32
    if (!::DeleteFileW(path.c_str())) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return;
        }
        fail(OOCCleanupStatus::IoFailure, stage, windows_error(code));
    }
#else
    if (::unlink(path.c_str()) != 0) {
        const int saved_errno = errno;
        if (saved_errno == ENOENT) {
            return;
        }
        fail(OOCCleanupStatus::IoFailure, stage, posix_error(saved_errno));
    }
#endif
}

[[nodiscard]] inline InspectResult inspect_pending_file(const std::filesystem::path& path,
                                                        std::size_t maximum_bytes = MARKER_BYTES) {
    const auto metadata = inspect_file(path, 0, false);
    if (metadata.kind != InspectKind::Present) {
        return metadata;
    }
    if (metadata.identity.size > maximum_bytes) {
        return InspectResult{
            .kind = InspectKind::Rejected,
            .identity = {},
            .bytes = {},
            .error = {},
        };
    }

    auto inspected = inspect_file(path, static_cast<std::size_t>(metadata.identity.size), true);
    if (inspected.kind == InspectKind::Present && inspected.identity != metadata.identity) {
        return InspectResult{
            .kind = InspectKind::Rejected,
            .identity = {},
            .bytes = {},
            .error = {},
        };
    }
    return inspected;
}

[[nodiscard]] inline bool marker_bytes_equal(std::span<const std::byte> actual,
                                             std::span<const std::byte> expected) noexcept {
    return actual.size() == expected.size() &&
           std::equal(actual.begin(), actual.end(), expected.begin());
}

[[nodiscard]] inline std::optional<IntentRecord> try_parse_marker(std::span<const std::byte> bytes,
                                                                  std::uint64_t expected_magic) {
    try {
        return parse_marker(bytes, expected_magic);
    } catch (const Failure& failure) {
        if (failure.status != OOCCleanupStatus::IntentCorrupt) {
            throw;
        }
        return std::nullopt;
    }
}

[[nodiscard]] inline bool same_native_file(const FileIdentity& actual,
                                           const FileIdentity& expected) noexcept {
    return actual.first == expected.first && actual.second == expected.second &&
           actual.third == expected.third;
}

inline void rewrite_pending_durable(const std::filesystem::path& path,
                                    const FileIdentity& expected_identity,
                                    std::span<const std::byte> bytes, OOCCleanupStage stage) {
#ifdef _WIN32
    const HANDLE handle =
        ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        fail(OOCCleanupStatus::IoFailure, stage, windows_error(::GetLastError()));
    }

    BY_HANDLE_FILE_INFORMATION before{};
    if (!::GetFileInformationByHandle(handle, &before) || !windows_regular_single_link(before)) {
        const DWORD code =
            ::GetLastError() == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, windows_error(code));
    }
    const auto before_identity = windows_identity(handle, before);
    if (!before_identity || !same_native_file(*before_identity, expected_identity)) {
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    LARGE_INTEGER start{};
    if (!::SetFilePointerEx(handle, start, nullptr, FILE_BEGIN)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::IoFailure, stage, windows_error(code));
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD requested = static_cast<DWORD>(
            (std::min)(bytes.size() - offset,
                       static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!::WriteFile(handle, bytes.data() + offset, requested, &written, nullptr)) {
            const DWORD code = ::GetLastError();
            (void)::CloseHandle(handle);
            fail(OOCCleanupStatus::IoFailure, stage, windows_error(code));
        }
        if (written == 0) {
            (void)::CloseHandle(handle);
            fail(OOCCleanupStatus::IoFailure, stage, std::make_error_code(std::errc::io_error));
        }
        offset += static_cast<std::size_t>(written);
    }
    if (!::SetEndOfFile(handle)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::IoFailure, stage, windows_error(code));
    }
    if (!::FlushFileBuffers(handle)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(code));
    }

    BY_HANDLE_FILE_INFORMATION after{};
    if (!::GetFileInformationByHandle(handle, &after) || !windows_regular_single_link(after)) {
        const DWORD code =
            ::GetLastError() == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, windows_error(code));
    }
    const auto after_identity = windows_identity(handle, after);
    if (!after_identity || !same_native_file(*after_identity, expected_identity) ||
        after_identity->size != bytes.size()) {
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    if (!::CloseHandle(handle)) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(::GetLastError()));
    }
#else
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        const int saved_errno = errno;
        fail(saved_errno == ELOOP ? OOCCleanupStatus::ForeignReplacementPreserved
                                  : OOCCleanupStatus::IoFailure,
             stage, posix_error(saved_errno));
    }

    struct stat before{};
    if (::fstat(descriptor, &before) != 0 || !posix_regular_single_link(before)) {
        const int saved_errno = errno == 0 ? EACCES : errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, posix_error(saved_errno));
    }
    if (!same_native_file(posix_identity(before), expected_identity)) {
        (void)::close(descriptor);
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::pwrite(descriptor, bytes.data() + offset, bytes.size() - offset,
                                         static_cast<off_t>(offset));
        if (written < 0) {
            const int saved_errno = errno;
            if (saved_errno == EINTR) {
                continue;
            }
            (void)::close(descriptor);
            fail(OOCCleanupStatus::IoFailure, stage, posix_error(saved_errno));
        }
        if (written == 0) {
            (void)::close(descriptor);
            fail(OOCCleanupStatus::IoFailure, stage, std::make_error_code(std::errc::io_error));
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::ftruncate(descriptor, static_cast<off_t>(bytes.size())) != 0) {
        const int saved_errno = errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::IoFailure, stage, posix_error(saved_errno));
    }

#if defined(__APPLE__)
    int sync_result = -1;
    do {
        sync_result = ::fcntl(descriptor, F_FULLFSYNC);
    } while (sync_result != 0 && errno == EINTR);
#else
    int sync_result = -1;
    do {
        sync_result = ::fsync(descriptor);
    } while (sync_result != 0 && errno == EINTR);
#endif
    if (sync_result != 0) {
        const int saved_errno = errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(saved_errno));
    }

    struct stat after{};
    if (::fstat(descriptor, &after) != 0 || !posix_regular_single_link(after)) {
        const int saved_errno = errno == 0 ? EACCES : errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, posix_error(saved_errno));
    }
    const auto after_identity = posix_identity(after);
    if (!same_native_file(after_identity, expected_identity) ||
        after_identity.size != bytes.size()) {
        (void)::close(descriptor);
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    if (::close(descriptor) != 0) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(errno));
    }
#endif

    sync_parent_directory(path.parent_path(), stage);
    const auto persisted = inspect_file(path, bytes.size(), true);
    if (persisted.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, persisted.error);
    }
    if (persisted.kind != InspectKind::Present || !marker_bytes_equal(persisted.bytes, bytes)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
}

[[nodiscard]] inline IntentRecord load_marker(const std::filesystem::path& path,
                                              std::uint64_t expected_magic,
                                              FileIdentity* identity = nullptr) {
    auto inspected = inspect_file(path, MARKER_BYTES, true);
    if (inspected.kind == InspectKind::Missing) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None,
             std::make_error_code(std::errc::no_such_file_or_directory));
    }
    if (inspected.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (identity != nullptr) {
        *identity = inspected.identity;
    }
    return parse_marker(inspected.bytes, expected_magic);
}

inline void confirm_existing_marker(const std::filesystem::path& path, const IntentRecord& expected,
                                    std::uint64_t expected_magic, OOCCleanupStage stage) {
    FileIdentity identity;
    const IntentRecord before = load_marker(path, expected_magic, &identity);
    if (before != expected) {
        fail(OOCCleanupStatus::IntentConflict, stage, protocol_error());
    }
    confirm_file_durable(path, identity, path.parent_path(), stage);
    const IntentRecord after = load_marker(path, expected_magic);
    if (after != expected) {
        fail(OOCCleanupStatus::IntentConflict, stage, protocol_error());
    }
}

inline void revalidate_marker(const std::filesystem::path& path, const IntentRecord& expected,
                              std::uint64_t expected_magic, OOCCleanupStage stage) {
    const IntentRecord current = load_marker(path, expected_magic);
    if (current != expected) {
        fail(OOCCleanupStatus::IntentConflict, stage, protocol_error());
    }
}

[[nodiscard]] inline IntentRecord load_intent(const OOCCleanupPaths& paths,
                                              FileIdentity* identity = nullptr) {
    return load_marker(paths.intent_path, INTENT_MAGIC, identity);
}

[[nodiscard]] inline IntentRecord load_staged(const OOCCleanupPaths& paths,
                                              FileIdentity* identity = nullptr) {
    return load_marker(paths.staged_path, STAGED_MAGIC, identity);
}

inline void confirm_existing_intent(const OOCCleanupPaths& paths, const IntentRecord& expected,
                                    OOCCleanupStage stage) {
    confirm_existing_marker(paths.intent_path, expected, INTENT_MAGIC, stage);
}

inline void confirm_existing_staged(const OOCCleanupPaths& paths, const IntentRecord& expected,
                                    OOCCleanupStage stage) {
    confirm_existing_marker(paths.staged_path, expected, STAGED_MAGIC, stage);
}

inline void revalidate_intent(const OOCCleanupPaths& paths, const IntentRecord& expected,
                              OOCCleanupStage stage) {
    revalidate_marker(paths.intent_path, expected, INTENT_MAGIC, stage);
}

inline void revalidate_staged(const OOCCleanupPaths& paths, const IntentRecord& expected,
                              OOCCleanupStage stage) {
    revalidate_marker(paths.staged_path, expected, STAGED_MAGIC, stage);
}

enum class ArtifactLocation : std::uint8_t {
    Missing,
    Live,
    Quarantine,
};

[[nodiscard]] inline ArtifactLocation locate_artifact(const std::filesystem::path& live_path,
                                                      const std::filesystem::path& quarantine_path,
                                                      ArtifactKind kind,
                                                      const ArtifactFingerprint& expected,
                                                      OOCCleanupStage stage) {
    auto live = inspect_file(live_path, artifact_header_bytes(kind), false);
    auto quarantine = inspect_file(quarantine_path, artifact_header_bytes(kind), false);

    if (live.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, live.error);
    }
    if (quarantine.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, quarantine.error);
    }
    if (live.kind == InspectKind::Rejected || quarantine.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    if (live.kind == InspectKind::Present && quarantine.kind == InspectKind::Present) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage,
             std::make_error_code(std::errc::file_exists));
    }
    if (live.kind == InspectKind::Present) {
        const auto actual = artifact_fingerprint(live, kind, expected.header_store_id);
        if (!actual || *actual != expected) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
        }
        return ArtifactLocation::Live;
    }
    if (quarantine.kind == InspectKind::Present) {
        const auto actual = artifact_fingerprint(quarantine, kind, expected.header_store_id);
        if (!actual || *actual != expected) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
        }
        return ArtifactLocation::Quarantine;
    }
    return ArtifactLocation::Missing;
}

struct NamespaceState final {
    ArtifactLocation index = ArtifactLocation::Missing;
    ArtifactLocation data = ArtifactLocation::Missing;
};

[[nodiscard]] inline NamespaceState audit_namespace(const OOCCleanupPaths& paths,
                                                    const IntentRecord& intent,
                                                    OOCCleanupStage stage, bool allow_missing) {
    revalidate_intent(paths, intent, stage);
    const NamespaceState state{
        .index = locate_artifact(paths.index_path, paths.quarantine_index_path, ArtifactKind::Index,
                                 intent.index, stage),
        .data = locate_artifact(paths.data_path, paths.quarantine_data_path, ArtifactKind::Data,
                                intent.data, stage),
    };
    if (!allow_missing &&
        (state.index == ArtifactLocation::Missing || state.data == ArtifactLocation::Missing)) {
        fail(OOCCleanupStatus::NamespaceConflict, stage, protocol_error());
    }
    return state;
}

[[nodiscard]] inline bool should_interrupt(const OOCCleanupTestHooks& hooks,
                                           OOCCleanupFaultPoint point) noexcept {
    return hooks.stop_after != nullptr && hooks.stop_after(point, hooks.context);
}

[[nodiscard]] inline bool should_interrupt_publish(const OOCCleanupTestHooks& hooks,
                                                   OOCCleanupPublishFaultPoint point) noexcept {
    return hooks.stop_after_publish != nullptr && hooks.stop_after_publish(point, hooks.context);
}

[[nodiscard]] inline bool should_fail_operation(const OOCCleanupTestHooks& hooks,
                                                OOCCleanupTestOperation operation) noexcept {
    return hooks.fail_before_operation != nullptr &&
           hooks.fail_before_operation(operation, hooks.context);
}

inline void inject_operation_failure(const OOCCleanupTestHooks& hooks,
                                     OOCCleanupTestOperation operation, OOCCleanupStatus status,
                                     OOCCleanupStage stage) {
    if (should_fail_operation(hooks, operation)) {
        fail(status, stage, std::make_error_code(std::errc::io_error));
    }
}

[[nodiscard]] inline OOCCleanupResult interrupted(OOCCleanupStage stage) noexcept {
    return OOCCleanupResult{
        .status = OOCCleanupStatus::Interrupted,
        .stage = stage,
        .native_error = {},
    };
}

inline void quarantine_one(const std::filesystem::path& live,
                           const std::filesystem::path& quarantine, OOCCleanupStage prior_stage) {
    const RenameOutcome renamed = rename_no_replace(live, quarantine);
    switch (renamed.result) {
    case RenameResult::Succeeded:
        return;
    case RenameResult::DestinationExists:
        fail(OOCCleanupStatus::NamespaceConflict, prior_stage, renamed.error);
    case RenameResult::Unsupported:
        fail(OOCCleanupStatus::PlatformUnsupported, prior_stage, renamed.error);
    case RenameResult::Failed:
        fail(OOCCleanupStatus::IoFailure, prior_stage, renamed.error);
    }
    fail(OOCCleanupStatus::UnexpectedFailure, prior_stage, protocol_error());
}

[[nodiscard]] inline bool path_exists_or_rejected(const std::filesystem::path& path,
                                                  OOCCleanupStage stage) {
    const auto inspected = inspect_file(path, 0, false);
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, inspected.error);
    }
    return inspected.kind != InspectKind::Missing;
}

[[nodiscard]] inline IntentRecord capture_source_pair(const OOCCleanupPaths& paths,
                                                      std::uint64_t store_id) {
    auto index = inspect_file(paths.index_path, OOCRelationStoreFormat::INDEX_HEADER_BYTES, false);
    auto data = inspect_file(paths.data_path, OOCRelationStoreFormat::DATA_HEADER_BYTES, false);
    if (index.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, index.error);
    }
    if (data.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, data.error);
    }
    if (index.kind != InspectKind::Present || data.kind != InspectKind::Present) {
        fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None, protocol_error());
    }
    const auto index_fingerprint = artifact_fingerprint(index, ArtifactKind::Index, store_id);
    const auto data_fingerprint = artifact_fingerprint(data, ArtifactKind::Data, store_id);
    if (!index_fingerprint || !data_fingerprint ||
        index_fingerprint->identity == data_fingerprint->identity) {
        fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None, protocol_error());
    }
    return IntentRecord{
        .platform_id = PLATFORM_ID,
        .store_id = store_id,
        .index = *index_fingerprint,
        .data = *data_fingerprint,
    };
}

inline void require_source_pair_unchanged(const OOCCleanupPaths& paths,
                                          const IntentRecord& intent) {
    const auto current = capture_source_pair(paths, intent.store_id);
    if (current != intent) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
}

[[nodiscard]] inline bool
expectation_is_well_formed(const OOCExactCleanupExpectation& exact) noexcept {
    if ((exact.index_magic != OOCRelationStoreFormat::MAGIC_V3_FINAL &&
         exact.index_magic != OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE) ||
        exact.index_size < OOCRelationStoreFormat::INDEX_HEADER_BYTES ||
        exact.data_size < OOCRelationStoreFormat::DATA_HEADER_BYTES) {
        return false;
    }
    if (exact.index_magic == OOCRelationStoreFormat::MAGIC_V3_FINAL) {
        constexpr std::uint64_t offset_bytes = sizeof(std::uint64_t);
        if (exact.persisted_count > (std::numeric_limits<std::uint64_t>::max() -
                                     OOCRelationStoreFormat::INDEX_HEADER_BYTES - offset_bytes) /
                                        offset_bytes) {
            return false;
        }
        return exact.index_size == OOCRelationStoreFormat::INDEX_HEADER_BYTES +
                                       (exact.persisted_count + 1) * offset_bytes;
    }
    return true;
}

[[nodiscard]] inline bool request_matches_intent(const OOCCleanupRequest& request,
                                                 const IntentRecord& intent) noexcept {
    if (request.store_id == 0 || request.store_id != intent.store_id) {
        return false;
    }
    if (!request.exact) {
        return intent.index.header_magic == OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE;
    }
    return expectation_is_well_formed(*request.exact) &&
           request.exact->index_magic == intent.index.header_magic &&
           request.exact->persisted_count == intent.index.header_count &&
           request.exact->index_size == intent.index.identity.size &&
           request.exact->data_size == intent.data.identity.size;
}

[[nodiscard]] inline bool ownership_proof_matches(const OwnershipProof& proof,
                                                  const OOCCleanupPaths& paths,
                                                  const IntentRecord& intent) noexcept {
    return proof.base_path == paths.base_path && proof.store_id == intent.store_id &&
           proof.index_identity == stable_identity(intent.index.identity) &&
           proof.data_identity == stable_identity(intent.data.identity);
}

[[nodiscard]] inline OOCCleanupStatus
publish_failure_status(util::durable_immutable_file::PublishStatus status) noexcept {
    using PublishStatus = util::durable_immutable_file::PublishStatus;
    switch (status) {
    case PublishStatus::invalid_path:
        return OOCCleanupStatus::InvalidRequest;
    case PublishStatus::file_sync_failed:
    case PublishStatus::parent_directory_sync_failed:
    case PublishStatus::parent_directory_close_failed:
        return OOCCleanupStatus::DurabilityFailure;
    case PublishStatus::already_exists:
        return OOCCleanupStatus::IntentConflict;
    case PublishStatus::input_too_large:
    case PublishStatus::file_ops_contract_violation:
        return OOCCleanupStatus::UnexpectedFailure;
    case PublishStatus::parent_directory_open_failed:
    case PublishStatus::open_failed:
    case PublishStatus::write_failed:
    case PublishStatus::zero_write_progress:
    case PublishStatus::close_failed:
        return OOCCleanupStatus::IoFailure;
    case PublishStatus::durable:
        return OOCCleanupStatus::Completed;
    case PublishStatus::unexpected_failure:
        return OOCCleanupStatus::UnexpectedFailure;
    }
    return OOCCleanupStatus::UnexpectedFailure;
}

[[nodiscard]] inline bool should_interrupt_private_lease(const OOCPrivateLeaseTestHooks& hooks,
                                                         OOCPrivateLeaseFaultPoint point) noexcept {
    return hooks.stop_after != nullptr && hooks.stop_after(point, hooks.context);
}

[[nodiscard]] inline InspectResult inspect_private_lease_marker(const std::filesystem::path& path) {
    return inspect_file(path, PRIVATE_LEASE_MARKER_BYTES, true);
}

[[nodiscard]] inline PrivateLeaseRecord
load_private_lease_marker(const std::filesystem::path& path, FileIdentity* identity = nullptr) {
    const auto inspected = inspect_private_lease_marker(path);
    if (inspected.kind == InspectKind::Missing) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None,
             std::make_error_code(std::errc::no_such_file_or_directory));
    }
    if (inspected.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (identity != nullptr) {
        *identity = inspected.identity;
    }
    return parse_private_lease_marker(inspected.bytes);
}

inline void
validate_private_lease_record_context(const PrivateLeaseRecord& record,
                                      const OOCCleanupPaths& paths,
                                      const std::array<std::uint64_t, 3>& parent_identity,
                                      const std::array<std::uint64_t, 3>& lock_identity) {
    if (record.base_path_digest != frozen_path_digest(paths.base_path) ||
        record.parent_identity != parent_identity || record.lock_identity != lock_identity) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
}

inline void confirm_private_lease_marker(
    const std::filesystem::path& path, const PrivateLeaseRecord& expected,
    std::optional<std::array<std::uint64_t, 3>> expected_identity = std::nullopt) {
    FileIdentity identity;
    const auto before = load_private_lease_marker(path, &identity);
    if (before != expected ||
        (expected_identity && stable_identity(identity) != *expected_identity)) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    confirm_file_durable(path, identity, path.parent_path(), OOCCleanupStage::None);
    FileIdentity after_identity;
    const auto after = load_private_lease_marker(path, &after_identity);
    if (after != expected || !same_native_file(after_identity, identity)) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
}

struct PrivateLeasePublication final {
    std::array<std::uint64_t, 3> identity{};
    bool interrupted = false;
};

[[nodiscard]] inline PrivateLeasePublication publish_private_lease_marker_durable(
    const std::filesystem::path& canonical_path, const std::filesystem::path& pending_path,
    const PrivateLeaseRecord& record, const OOCPrivateLeaseTestHooks& hooks,
    OOCPrivateLeaseFaultPoint pending_fault_point) {
    const auto expected = serialize_private_lease_marker(record);
    auto canonical = inspect_private_lease_marker(canonical_path);
    if (canonical.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, canonical.error);
    }
    if (canonical.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    if (canonical.kind == InspectKind::Present) {
        if (!marker_bytes_equal(canonical.bytes, expected)) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
        confirm_private_lease_marker(canonical_path, record);
        const auto pending = inspect_pending_file(pending_path, PRIVATE_LEASE_MARKER_BYTES);
        if (pending.kind == InspectKind::Error) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, pending.error);
        }
        if (pending.kind == InspectKind::Rejected ||
            (pending.kind == InspectKind::Present &&
             !marker_bytes_equal(pending.bytes, expected))) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        if (pending.kind == InspectKind::Present) {
            remove_file(pending_path, OOCCleanupStage::None);
            sync_parent_directory(pending_path.parent_path(), OOCCleanupStage::None);
            confirm_private_lease_marker(canonical_path, record);
        }
        return PrivateLeasePublication{
            .identity = stable_identity(canonical.identity),
            .interrupted = false,
        };
    }

    auto pending = inspect_pending_file(pending_path, PRIVATE_LEASE_MARKER_BYTES);
    if (pending.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, pending.error);
    }
    if (pending.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    bool newly_durable = false;
    if (pending.kind == InspectKind::Missing) {
        const auto published = util::durable_immutable_file::publish(pending_path, expected);
        if (!published.is_durable()) {
            pending = inspect_pending_file(pending_path, PRIVATE_LEASE_MARKER_BYTES);
            if (pending.kind != InspectKind::Present ||
                !marker_bytes_equal(pending.bytes, expected)) {
                fail(publish_failure_status(published.status()), OOCCleanupStage::None,
                     published.native_error());
            }
        }
        newly_durable = true;
    } else if (!marker_bytes_equal(pending.bytes, expected)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    FileIdentity pending_identity;
    const auto ready = load_private_lease_marker(pending_path, &pending_identity);
    if (ready != record) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    confirm_private_lease_marker(pending_path, record);
    if (newly_durable && should_interrupt_private_lease(hooks, pending_fault_point)) {
        return PrivateLeasePublication{
            .identity = stable_identity(pending_identity),
            .interrupted = true,
        };
    }

    const auto renamed = rename_no_replace(pending_path, canonical_path);
    switch (renamed.result) {
    case RenameResult::Succeeded:
        sync_parent_directory(canonical_path.parent_path(), OOCCleanupStage::None);
        break;
    case RenameResult::DestinationExists:
        confirm_private_lease_marker(canonical_path, record);
        break;
    case RenameResult::Unsupported:
        fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None, renamed.error);
    case RenameResult::Failed:
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, renamed.error);
    }

    FileIdentity canonical_identity;
    const auto persisted = load_private_lease_marker(canonical_path, &canonical_identity);
    if (persisted != record || !same_native_file(canonical_identity, pending_identity)) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    confirm_private_lease_marker(canonical_path, record);
    return PrivateLeasePublication{
        .identity = stable_identity(canonical_identity),
        .interrupted = false,
    };
}

inline void remove_private_lease_marker_durable(
    const std::filesystem::path& path, const PrivateLeaseRecord& expected,
    std::optional<std::array<std::uint64_t, 3>> expected_identity = std::nullopt) {
    const auto inspected = inspect_private_lease_marker(path);
    if (inspected.kind == InspectKind::Missing) {
        sync_parent_directory(path.parent_path(), OOCCleanupStage::None);
        return;
    }
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (inspected.kind == InspectKind::Rejected ||
        parse_private_lease_marker(inspected.bytes) != expected ||
        (expected_identity && stable_identity(inspected.identity) != *expected_identity)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    remove_file(path, OOCCleanupStage::None);
    sync_parent_directory(path.parent_path(), OOCCleanupStage::None);
    const auto absent = inspect_file(path, 0, false);
    if (absent.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, absent.error);
    }
    if (absent.kind != InspectKind::Missing) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
}

[[nodiscard]] inline bool marker_is_valid(std::span<const std::byte> bytes,
                                          std::uint64_t marker_magic) {
    return try_parse_marker(bytes, marker_magic).has_value();
}

[[nodiscard]] inline bool
pending_is_recognizable_owned(std::span<const std::byte> pending,
                              std::span<const std::byte> expected) noexcept {
    constexpr std::size_t ownership_prefix_bytes = 4 * sizeof(std::uint64_t);
    return pending.size() >= ownership_prefix_bytes && pending.size() <= expected.size() &&
           std::equal(pending.begin(),
                      pending.begin() + static_cast<std::ptrdiff_t>(ownership_prefix_bytes),
                      expected.begin());
}

struct PendingDurableResult final {
    FileIdentity identity;
    bool newly_durable = false;
};

[[nodiscard]] inline PendingDurableResult
confirm_pending_durable(const std::filesystem::path& pending_path, const IntentRecord& intent,
                        std::uint64_t marker_magic, OOCCleanupStage stage, bool newly_durable) {
    FileIdentity identity;
    const IntentRecord persisted = load_marker(pending_path, marker_magic, &identity);
    if (persisted != intent) {
        fail(OOCCleanupStatus::IntentConflict, stage, protocol_error());
    }
    confirm_existing_marker(pending_path, intent, marker_magic, stage);
    return PendingDurableResult{
        .identity = identity,
        .newly_durable = newly_durable,
    };
}

[[nodiscard]] inline PendingDurableResult
ensure_pending_durable(const std::filesystem::path& pending_path, const IntentRecord& intent,
                       std::uint64_t marker_magic, OOCCleanupStage stage) {
    const auto expected = serialize_marker(intent, marker_magic);
    auto pending = inspect_pending_file(pending_path);
    if (pending.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, pending.error);
    }
    if (pending.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    if (pending.kind == InspectKind::Missing) {
        const auto published = util::durable_immutable_file::publish(pending_path, expected);
        if (!published.is_durable()) {
            pending = inspect_pending_file(pending_path);
            if (pending.kind != InspectKind::Present ||
                !marker_bytes_equal(pending.bytes, expected)) {
                fail(publish_failure_status(published.status()), stage, published.native_error());
            }
        }
        return confirm_pending_durable(pending_path, intent, marker_magic, stage, true);
    }

    if (marker_bytes_equal(pending.bytes, expected)) {
        return confirm_pending_durable(pending_path, intent, marker_magic, stage, false);
    }

    // A complete, valid marker for another transaction is foreign even when it
    // happens to share the same reserved pending leaf.
    if (pending.bytes.size() == MARKER_BYTES && (marker_is_valid(pending.bytes, INTENT_MAGIC) ||
                                                 marker_is_valid(pending.bytes, STAGED_MAGIC))) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    // The trusted-parent contract makes a regular single-link malformed
    // pending leaf an abandoned no-authority write. Rewrite that same inode;
    // never unlink it, and never follow or rewrite a hardlink/reparse point.
    rewrite_pending_durable(pending_path, pending.identity, expected, stage);
    return confirm_pending_durable(pending_path, intent, marker_magic, stage, true);
}

inline void reclaim_pending_for_canonical(const std::filesystem::path& canonical_path,
                                          const std::filesystem::path& pending_path,
                                          const IntentRecord& intent, std::uint64_t marker_magic,
                                          OOCCleanupStage stage) {
    const auto expected = serialize_marker(intent, marker_magic);
    auto pending = inspect_pending_file(pending_path);
    if (pending.kind == InspectKind::Missing) {
        return;
    }
    if (pending.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, pending.error);
    }
    if (pending.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    if (pending.bytes.size() == MARKER_BYTES && !marker_bytes_equal(pending.bytes, expected) &&
        (marker_is_valid(pending.bytes, INTENT_MAGIC) ||
         marker_is_valid(pending.bytes, STAGED_MAGIC))) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    if (!marker_bytes_equal(pending.bytes, expected) &&
        !pending_is_recognizable_owned(pending.bytes, expected)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    revalidate_marker(canonical_path, intent, marker_magic, stage);
    const auto rechecked = inspect_pending_file(pending_path);
    if (rechecked.kind != InspectKind::Present || rechecked.identity != pending.identity ||
        !marker_bytes_equal(rechecked.bytes, pending.bytes)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    remove_file(pending_path, stage);
    sync_parent_directory(pending_path.parent_path(), stage);
    revalidate_marker(canonical_path, intent, marker_magic, stage);
}

[[nodiscard]] inline bool publish_marker_durable(const std::filesystem::path& canonical_path,
                                                 const std::filesystem::path& pending_path,
                                                 const IntentRecord& intent,
                                                 std::uint64_t marker_magic, OOCCleanupStage stage,
                                                 const OOCCleanupTestHooks& hooks,
                                                 OOCCleanupPublishFaultPoint pending_fault_point) {
    const auto expected = serialize_marker(intent, marker_magic);
    const auto canonical = inspect_file(canonical_path, MARKER_BYTES, true);
    if (canonical.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, canonical.error);
    }
    if (canonical.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, stage, protocol_error());
    }
    if (canonical.kind == InspectKind::Present) {
        if (!marker_bytes_equal(canonical.bytes, expected)) {
            fail(OOCCleanupStatus::IntentConflict, stage, protocol_error());
        }
        confirm_existing_marker(canonical_path, intent, marker_magic, stage);
        reclaim_pending_for_canonical(canonical_path, pending_path, intent, marker_magic, stage);
        return false;
    }

    const auto pending = ensure_pending_durable(pending_path, intent, marker_magic, stage);
    if (pending.newly_durable && should_interrupt_publish(hooks, pending_fault_point)) {
        return true;
    }

    FileIdentity before_rename;
    const IntentRecord ready = load_marker(pending_path, marker_magic, &before_rename);
    if (ready != intent || before_rename != pending.identity) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    const RenameOutcome renamed = rename_no_replace(pending_path, canonical_path);
    switch (renamed.result) {
    case RenameResult::Succeeded:
        sync_parent_directory(canonical_path.parent_path(), stage);
        break;
    case RenameResult::DestinationExists:
        confirm_existing_marker(canonical_path, intent, marker_magic, stage);
        reclaim_pending_for_canonical(canonical_path, pending_path, intent, marker_magic, stage);
        return false;
    case RenameResult::Unsupported:
        fail(OOCCleanupStatus::PlatformUnsupported, stage, renamed.error);
    case RenameResult::Failed:
        fail(OOCCleanupStatus::IoFailure, stage, renamed.error);
    }

    FileIdentity canonical_identity;
    const IntentRecord persisted = load_marker(canonical_path, marker_magic, &canonical_identity);
    if (persisted != intent || !same_native_file(canonical_identity, before_rename)) {
        fail(OOCCleanupStatus::IntentConflict, stage, protocol_error());
    }
    confirm_existing_marker(canonical_path, intent, marker_magic, stage);
    return false;
}

[[nodiscard]] inline OOCCleanupResult finish_staged_only_tail(const OOCCleanupPaths& paths,
                                                              const IntentRecord& staged,
                                                              const OOCCleanupTestHooks& hooks) {
    constexpr OOCCleanupStage stage = OOCCleanupStage::IntentRemoved;
    revalidate_staged(paths, staged, stage);
    reclaim_pending_for_canonical(paths.staged_path, paths.staged_pending_path, staged,
                                  STAGED_MAGIC, stage);
    // Intent has already been durably consumed, so this marker has no delete
    // authority. Original live names may now belong to a newer store and are
    // deliberately ignored; only an unfinished quarantine tail blocks cleanup.
    if (path_exists_or_rejected(paths.quarantine_index_path, stage) ||
        path_exists_or_rejected(paths.quarantine_data_path, stage)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    inject_operation_failure(hooks, OOCCleanupTestOperation::StagedUnlink,
                             OOCCleanupStatus::IoFailure, stage);
    remove_file(paths.staged_path, stage);
    inject_operation_failure(hooks, OOCCleanupTestOperation::StagedUnlinkParentSync,
                             OOCCleanupStatus::DurabilityFailure, stage);
    sync_parent_directory(paths.staged_path.parent_path(), stage);
    const auto final_staged = inspect_file(paths.staged_path, 0, false);
    if (final_staged.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, final_staged.error);
    }
    if (final_staged.kind != InspectKind::Missing) {
        fail(OOCCleanupStatus::IntentConflict, stage, protocol_error());
    }
    return OOCCleanupResult{
        .status = OOCCleanupStatus::Completed,
        .stage = OOCCleanupStage::Completed,
        .native_error = {},
    };
}

[[nodiscard]] inline OOCCleanupResult advance_transaction(const OOCCleanupPaths& paths,
                                                          const IntentRecord& intent,
                                                          bool staged_exists,
                                                          const OOCCleanupTestHooks& hooks) {
    OOCCleanupStage stage = OOCCleanupStage::IntentDurable;

    // Confirm any namespace state recovered after a prior uncertain barrier
    // before allowing the next transition to consume it.
    sync_parent_directory(paths.intent_path.parent_path(), stage);

    if (!staged_exists) {
        // Before delete authorization exists, every owned leaf must remain
        // visible at exactly one of its live/quarantine names.
        auto state = audit_namespace(paths, intent, stage, false);
        const bool valid_pre_staged_state =
            (state.index == ArtifactLocation::Live && state.data == ArtifactLocation::Live) ||
            (state.index == ArtifactLocation::Quarantine && state.data == ArtifactLocation::Live) ||
            (state.index == ArtifactLocation::Quarantine &&
             state.data == ArtifactLocation::Quarantine);
        if (!valid_pre_staged_state) {
            fail(OOCCleanupStatus::NamespaceConflict, stage, protocol_error());
        }
        if (state.index == ArtifactLocation::Live) {
            inject_operation_failure(hooks, OOCCleanupTestOperation::IndexRename,
                                     OOCCleanupStatus::IoFailure, stage);
            quarantine_one(paths.index_path, paths.quarantine_index_path, stage);
            inject_operation_failure(hooks, OOCCleanupTestOperation::IndexRenameParentSync,
                                     OOCCleanupStatus::DurabilityFailure, stage);
            sync_parent_directory(paths.intent_path.parent_path(), stage);
            state = audit_namespace(paths, intent, stage, false);
            if (state.index != ArtifactLocation::Quarantine) {
                fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
            }
            stage = OOCCleanupStage::IndexQuarantined;
            if (should_interrupt(hooks, OOCCleanupFaultPoint::FirstRenameDurable)) {
                return interrupted(stage);
            }
        } else if (state.index != ArtifactLocation::Quarantine) {
            fail(OOCCleanupStatus::NamespaceConflict, stage, protocol_error());
        } else {
            stage = OOCCleanupStage::IndexQuarantined;
        }

        state = audit_namespace(paths, intent, stage, false);
        if (state.data == ArtifactLocation::Live) {
            inject_operation_failure(hooks, OOCCleanupTestOperation::DataRename,
                                     OOCCleanupStatus::IoFailure, stage);
            quarantine_one(paths.data_path, paths.quarantine_data_path, stage);
            inject_operation_failure(hooks, OOCCleanupTestOperation::DataRenameParentSync,
                                     OOCCleanupStatus::DurabilityFailure, stage);
            sync_parent_directory(paths.intent_path.parent_path(), stage);
            state = audit_namespace(paths, intent, stage, false);
            if (state.index != ArtifactLocation::Quarantine ||
                state.data != ArtifactLocation::Quarantine) {
                fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
            }
            stage = OOCCleanupStage::PairQuarantined;
            if (should_interrupt(hooks, OOCCleanupFaultPoint::SecondRenameDurable)) {
                return interrupted(stage);
            }
        } else if (state.data != ArtifactLocation::Quarantine) {
            fail(OOCCleanupStatus::NamespaceConflict, stage, protocol_error());
        } else {
            stage = OOCCleanupStage::PairQuarantined;
        }

        state = audit_namespace(paths, intent, stage, false);
        if (state.index != ArtifactLocation::Quarantine ||
            state.data != ArtifactLocation::Quarantine) {
            fail(OOCCleanupStatus::NamespaceConflict, stage, protocol_error());
        }
        if (publish_marker_durable(paths.staged_path, paths.staged_pending_path, intent,
                                   STAGED_MAGIC, stage, hooks,
                                   OOCCleanupPublishFaultPoint::StagedPendingDurable)) {
            return interrupted(stage);
        }
        confirm_existing_staged(paths, intent, stage);
        stage = OOCCleanupStage::DeleteAuthorized;
        if (should_interrupt(hooks, OOCCleanupFaultPoint::DeleteAuthorizedDurable)) {
            return interrupted(stage);
        }
    } else {
        confirm_existing_staged(paths, intent, OOCCleanupStage::DeleteAuthorized);
        reclaim_pending_for_canonical(paths.staged_path, paths.staged_pending_path, intent,
                                      STAGED_MAGIC, OOCCleanupStage::DeleteAuthorized);
        stage = OOCCleanupStage::DeleteAuthorized;
    }

    revalidate_staged(paths, intent, stage);
    auto state = audit_namespace(paths, intent, stage, true);
    if (state.index == ArtifactLocation::Live || state.data == ArtifactLocation::Live) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    if (state.data == ArtifactLocation::Quarantine) {
        revalidate_staged(paths, intent, stage);
        inject_operation_failure(hooks, OOCCleanupTestOperation::DataUnlink,
                                 OOCCleanupStatus::IoFailure, stage);
        remove_file(paths.quarantine_data_path, stage);
        inject_operation_failure(hooks, OOCCleanupTestOperation::DataUnlinkParentSync,
                                 OOCCleanupStatus::DurabilityFailure, stage);
        sync_parent_directory(paths.intent_path.parent_path(), stage);
        state = audit_namespace(paths, intent, stage, true);
        revalidate_staged(paths, intent, stage);
        if (state.data != ArtifactLocation::Missing) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
        }
        stage = OOCCleanupStage::DataRemoved;
        if (should_interrupt(hooks, OOCCleanupFaultPoint::FirstUnlinkDurable)) {
            return interrupted(stage);
        }
    } else {
        stage = OOCCleanupStage::DataRemoved;
    }

    state = audit_namespace(paths, intent, stage, true);
    revalidate_staged(paths, intent, stage);
    if (state.index == ArtifactLocation::Live || state.data == ArtifactLocation::Live) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    if (state.index == ArtifactLocation::Quarantine) {
        inject_operation_failure(hooks, OOCCleanupTestOperation::IndexUnlink,
                                 OOCCleanupStatus::IoFailure, stage);
        remove_file(paths.quarantine_index_path, stage);
        inject_operation_failure(hooks, OOCCleanupTestOperation::IndexUnlinkParentSync,
                                 OOCCleanupStatus::DurabilityFailure, stage);
        sync_parent_directory(paths.intent_path.parent_path(), stage);
        state = audit_namespace(paths, intent, stage, true);
        revalidate_staged(paths, intent, stage);
        if (state.index != ArtifactLocation::Missing || state.data != ArtifactLocation::Missing) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
        }
        stage = OOCCleanupStage::IndexRemoved;
        if (should_interrupt(hooks, OOCCleanupFaultPoint::SecondUnlinkDurable)) {
            return interrupted(stage);
        }
    } else {
        stage = OOCCleanupStage::IndexRemoved;
    }

    state = audit_namespace(paths, intent, stage, true);
    revalidate_staged(paths, intent, stage);
    if (state.index != ArtifactLocation::Missing || state.data != ArtifactLocation::Missing) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    // Consuming intent closes delete authority. If the process exits after this
    // barrier, staged-only recovery may remove only the staged marker.
    inject_operation_failure(hooks, OOCCleanupTestOperation::IntentUnlink,
                             OOCCleanupStatus::IoFailure, stage);
    remove_file(paths.intent_path, stage);
    inject_operation_failure(hooks, OOCCleanupTestOperation::IntentUnlinkParentSync,
                             OOCCleanupStatus::DurabilityFailure, stage);
    sync_parent_directory(paths.intent_path.parent_path(), stage);
    const auto final_intent = inspect_file(paths.intent_path, 0, false);
    if (final_intent.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, final_intent.error);
    }
    if (final_intent.kind != InspectKind::Missing) {
        fail(OOCCleanupStatus::IntentConflict, stage, protocol_error());
    }
    stage = OOCCleanupStage::IntentRemoved;
    if (should_interrupt(hooks, OOCCleanupFaultPoint::IntentRemovedDurable)) {
        return interrupted(stage);
    }
    return finish_staged_only_tail(paths, intent, hooks);
}

[[nodiscard]] inline OOCCleanupResult
run_transaction_locked(const OOCCleanupPaths& paths, const BaseLock& held_lock,
                       const OOCCleanupRequest* request, bool allow_begin,
                       const OwnershipProof* ownership_proof, bool* consume_receipt,
                       const OOCCleanupTestHooks& hooks, bool publish_intent_only = false) {
    if (!held_lock.matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    if ((allow_begin &&
         (request == nullptr || ownership_proof == nullptr || consume_receipt == nullptr)) ||
        (!allow_begin && ownership_proof != nullptr)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }

    auto intent_inspection = inspect_file(paths.intent_path, MARKER_BYTES, true);
    if (intent_inspection.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, intent_inspection.error);
    }
    if (intent_inspection.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    auto staged_inspection = inspect_file(paths.staged_path, MARKER_BYTES, true);
    if (staged_inspection.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, staged_inspection.error);
    }
    if (staged_inspection.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }

    IntentRecord intent;
    if (intent_inspection.kind == InspectKind::Missing) {
        if (staged_inspection.kind == InspectKind::Present) {
            const IntentRecord staged = parse_marker(staged_inspection.bytes, STAGED_MAGIC);
            const bool tail_matches_request =
                request != nullptr && request_matches_intent(*request, staged);
            confirm_existing_staged(paths, staged, OOCCleanupStage::IntentRemoved);
            const auto tail_result = finish_staged_only_tail(paths, staged, hooks);
            if (!allow_begin || request == nullptr) {
                if (request != nullptr && !tail_matches_request) {
                    return OOCCleanupResult{
                        .status = OOCCleanupStatus::NoTransaction,
                        .stage = OOCCleanupStage::None,
                        .native_error = {},
                    };
                }
                return tail_result;
            }
            staged_inspection = InspectResult{
                .kind = InspectKind::Missing,
                .identity = {},
                .bytes = {},
                .error = {},
            };
        }

        const bool quarantine_exists =
            path_exists_or_rejected(paths.quarantine_index_path, OOCCleanupStage::None) ||
            path_exists_or_rejected(paths.quarantine_data_path, OOCCleanupStage::None);
        if (quarantine_exists) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        const bool intent_pending_exists =
            path_exists_or_rejected(paths.intent_pending_path, OOCCleanupStage::None);
        if (!allow_begin) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::NoTransaction,
                .stage = OOCCleanupStage::None,
                .native_error = {},
            };
        }
        if (request == nullptr || request->store_id == 0 ||
            (request->exact && !expectation_is_well_formed(*request->exact))) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }

        const bool index_exists = path_exists_or_rejected(paths.index_path, OOCCleanupStage::None);
        const bool data_exists = path_exists_or_rejected(paths.data_path, OOCCleanupStage::None);
        if (!index_exists && !data_exists) {
            if (intent_pending_exists) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
            }
            *consume_receipt = true;
            return OOCCleanupResult{
                .status = OOCCleanupStatus::Completed,
                .stage = OOCCleanupStage::Completed,
                .native_error = {},
            };
        }

        intent = capture_source_pair(paths, request->store_id);
        if (!request_matches_intent(*request, intent)) {
            fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None, protocol_error());
        }
        if (!ownership_proof_matches(*ownership_proof, paths, intent)) {
            fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None, protocol_error());
        }
        require_source_pair_unchanged(paths, intent);
        if (publish_marker_durable(paths.intent_path, paths.intent_pending_path, intent,
                                   INTENT_MAGIC, OOCCleanupStage::None, hooks,
                                   OOCCleanupPublishFaultPoint::IntentPendingDurable)) {
            return interrupted(OOCCleanupStage::None);
        }
        *consume_receipt = true;
        if (should_interrupt(hooks, OOCCleanupFaultPoint::IntentDurable)) {
            return interrupted(OOCCleanupStage::IntentDurable);
        }
    } else {
        intent = parse_marker(intent_inspection.bytes, INTENT_MAGIC);
        if (request != nullptr && !request_matches_intent(*request, intent)) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
        if (ownership_proof != nullptr &&
            !ownership_proof_matches(*ownership_proof, paths, intent)) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
        confirm_existing_intent(paths, intent, OOCCleanupStage::IntentDurable);
        if (consume_receipt != nullptr) {
            *consume_receipt = true;
        }
        reclaim_pending_for_canonical(paths.intent_path, paths.intent_pending_path, intent,
                                      INTENT_MAGIC, OOCCleanupStage::IntentDurable);
    }

    if (publish_intent_only) {
        if (!allow_begin || staged_inspection.kind != InspectKind::Missing) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::IntentDurable,
                 protocol_error());
        }
        const auto state = audit_namespace(paths, intent, OOCCleanupStage::IntentDurable, false);
        if (state.index != ArtifactLocation::Live || state.data != ArtifactLocation::Live) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::IntentDurable,
                 protocol_error());
        }
        return OOCCleanupResult{
            .status = OOCCleanupStatus::Completed,
            .stage = OOCCleanupStage::IntentDurable,
            .native_error = {},
        };
    }

    bool staged_exists = staged_inspection.kind == InspectKind::Present;
    if (staged_exists) {
        const IntentRecord staged = parse_marker(staged_inspection.bytes, STAGED_MAGIC);
        if (staged != intent) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::DeleteAuthorized,
                 protocol_error());
        }
        confirm_existing_staged(paths, intent, OOCCleanupStage::DeleteAuthorized);
    }
    return advance_transaction(paths, intent, staged_exists, hooks);
}

[[nodiscard]] inline OOCCleanupResult
run_transaction(const std::filesystem::path& requested_base, const OOCCleanupRequest* request,
                bool allow_begin, const OwnershipProof* ownership_proof, bool* consume_receipt,
                const OOCCleanupTestHooks& hooks) {
    const OOCCleanupPaths paths = freeze_paths(requested_base);
    BaseLock lock(paths.lock_path, paths.private_directory.empty());
    return run_transaction_locked(paths, lock, request, allow_begin, ownership_proof,
                                  consume_receipt, hooks, false);
}

/// Require the complete pair namespace to be empty while the caller holds the
/// matching BaseLock. The persistent regular lock leaf itself is intentionally
/// outside this set.
inline void require_pair_namespace_reusable_locked(const OOCCleanupPaths& paths) {
    const std::array<const std::filesystem::path*, 8> leaves{
        &paths.index_path,
        &paths.data_path,
        &paths.intent_path,
        &paths.intent_pending_path,
        &paths.staged_path,
        &paths.staged_pending_path,
        &paths.quarantine_index_path,
        &paths.quarantine_data_path,
    };
    for (const auto* leaf : leaves) {
        const auto inspected = inspect_file(*leaf, 0, false);
        if (inspected.kind == InspectKind::Error) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
        }
        if (inspected.kind != InspectKind::Missing) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
    }
}

[[nodiscard]] inline std::array<std::uint64_t, 3>
capture_directory_identity_locked(const std::filesystem::path& directory_path) {
#ifdef _WIN32
    const HANDLE handle = ::CreateFileW(
        directory_path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(::GetLastError()));
    }

    BY_HANDLE_FILE_INFORMATION information{};
    const bool inspected = ::GetFileInformationByHandle(handle, &information) != FALSE;
    const auto identity = inspected ? windows_identity(handle, information) : std::nullopt;
    if (!inspected || !identity || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        const DWORD code =
            ::GetLastError() == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, windows_error(code));
    }
    if (!::CloseHandle(handle)) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(::GetLastError()));
    }
    return stable_identity(*identity);
#else
    int descriptor = -1;
    do {
        descriptor =
            ::open(directory_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        fail(errno == ELOOP ? OOCCleanupStatus::NamespaceConflict : OOCCleanupStatus::IoFailure,
             OOCCleanupStage::None, posix_error(errno));
    }

    struct stat held{};
    struct stat named{};
    if (::fstat(descriptor, &held) != 0 || ::lstat(directory_path.c_str(), &named) != 0 ||
        !S_ISDIR(held.st_mode) || !S_ISDIR(named.st_mode) || held.st_dev != named.st_dev ||
        held.st_ino != named.st_ino) {
        const int saved_errno = errno == 0 ? EACCES : errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, posix_error(saved_errno));
    }
    const auto identity = stable_identity(posix_identity(held));
    if (::close(descriptor) != 0) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
    }
    return identity;
#endif
}

[[nodiscard]] inline std::optional<std::array<std::uint64_t, 3>>
inspect_directory_identity_locked(const std::filesystem::path& directory_path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(directory_path, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            return std::nullopt;
        }
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, error);
    }
    if (status.type() == std::filesystem::file_type::not_found) {
        return std::nullopt;
    }
    if (!std::filesystem::is_directory(status)) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    return capture_directory_identity_locked(directory_path);
}

struct PrivateLeaseDirectoryEntries final {
    bool owner = false;
    bool owner_pending = false;
};

struct PrivateLeasePreactiveEntries final {
    bool owner = false;
    bool index = false;
    bool data = false;
};

[[nodiscard]] inline PrivateLeaseDirectoryEntries
inspect_private_lease_control_entries(const std::filesystem::path& directory_path) {
    PrivateLeaseDirectoryEntries entries;
    std::error_code error;
    std::filesystem::directory_iterator cursor(directory_path, error);
    if (error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, error);
    }
    for (const auto& entry : cursor) {
        const auto leaf = entry.path().filename();
        if (path_leaf_equals_ascii(leaf, ".gnfs-private-lease-v1.owner")) {
            if (entries.owner) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
            }
            entries.owner = true;
            continue;
        }
        if (path_leaf_equals_ascii(leaf, ".gnfs-private-lease-v1.owner.pending")) {
            if (entries.owner_pending) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
            }
            entries.owner_pending = true;
            continue;
        }
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    return entries;
}

[[nodiscard]] inline PrivateLeasePreactiveEntries
inspect_private_lease_preactive_entries(const std::filesystem::path& directory_path,
                                        const OOCCleanupPaths& paths) {
    PrivateLeasePreactiveEntries entries;
    std::error_code error;
    std::filesystem::directory_iterator cursor(directory_path, error);
    if (error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, error);
    }
    for (const auto& entry : cursor) {
        const auto leaf = entry.path().filename();
        if (path_leaf_equals_ascii(leaf, ".gnfs-private-lease-v1.owner")) {
            if (entries.owner) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
            }
            entries.owner = true;
            continue;
        }
        if (path_leaf_equals(leaf, paths.index_path.filename())) {
            if (entries.index) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
            }
            entries.index = true;
            continue;
        }
        if (path_leaf_equals(leaf, paths.data_path.filename())) {
            if (entries.data) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
            }
            entries.data = true;
            continue;
        }
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    return entries;
}

/// Reject foreign children before a canonical pair transaction performs its
/// first rename. The transaction itself validates the type, identity, and
/// contents of every allowed protocol leaf; this directory-wide pass closes
/// the earlier gap where an unknown sibling was discovered only after the
/// owned pair had already been deleted.
inline void inspect_private_lease_transaction_entries(const std::filesystem::path& directory_path,
                                                      const OOCCleanupPaths& paths) {
    std::array<bool, 9> seen{};
    const std::array<const std::filesystem::path*, 8> pair_leaves{
        &paths.index_path,
        &paths.data_path,
        &paths.intent_path,
        &paths.intent_pending_path,
        &paths.staged_path,
        &paths.staged_pending_path,
        &paths.quarantine_index_path,
        &paths.quarantine_data_path,
    };

    std::error_code error;
    std::filesystem::directory_iterator cursor(directory_path, error);
    if (error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, error);
    }
    for (const auto& entry : cursor) {
        const auto leaf = entry.path().filename();
        size_t slot = seen.size();
        if (path_leaf_equals_ascii(leaf, ".gnfs-private-lease-v1.owner")) {
            slot = 0;
        } else {
            for (size_t index = 0; index < pair_leaves.size(); ++index) {
                if (path_leaf_equals(leaf, pair_leaves[index]->filename())) {
                    slot = index + 1;
                    break;
                }
            }
        }
        if (slot >= seen.size() || seen[slot]) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        seen[slot] = true;
    }
}

inline void create_directory_durable_locked(const std::filesystem::path& directory_path) {
    if (inspect_directory_identity_locked(directory_path)) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    std::error_code error;
    const bool created = std::filesystem::create_directory(directory_path, error);
    if (error) {
        fail(error == std::errc::file_exists ? OOCCleanupStatus::NamespaceConflict
                                             : OOCCleanupStatus::IoFailure,
             OOCCleanupStage::None, error);
    }
    if (!created) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    sync_parent_directory(directory_path.parent_path(), OOCCleanupStage::None);
    if (!inspect_directory_identity_locked(directory_path)) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
}

inline void
remove_empty_directory_durable_locked(const std::filesystem::path& directory_path,
                                      const std::array<std::uint64_t, 3>& expected_identity) {
    const auto current = inspect_directory_identity_locked(directory_path);
    if (!current) {
        sync_parent_directory(directory_path.parent_path(), OOCCleanupStage::None);
        return;
    }
    if (*current != expected_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    std::error_code error;
    std::filesystem::directory_iterator cursor(directory_path, error);
    if (error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, error);
    }
    if (cursor != std::filesystem::directory_iterator{}) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    const bool removed = std::filesystem::remove(directory_path, error);
    if (error) {
        fail(error == std::errc::directory_not_empty ? OOCCleanupStatus::NamespaceConflict
                                                     : OOCCleanupStatus::IoFailure,
             OOCCleanupStage::None, error);
    }
    if (!removed) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    sync_parent_directory(directory_path.parent_path(), OOCCleanupStage::None);
    if (inspect_directory_identity_locked(directory_path)) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
}

[[nodiscard]] inline PrivateLeaseRecord
make_private_lease_reserved_record(const OOCCleanupPaths& paths,
                                   const std::array<std::uint64_t, 2>& lease_id,
                                   const std::array<std::uint64_t, 3>& parent_identity,
                                   const std::array<std::uint64_t, 3>& lock_identity) {
    return PrivateLeaseRecord{
        .platform_id = PLATFORM_ID,
        .phase = PrivateLeasePhase::Reserved,
        .capability = PrivateLeaseCapability::RollbackStagingOnly,
        .lease_id = lease_id,
        .base_path_digest = frozen_path_digest(paths.base_path),
        .parent_identity = parent_identity,
        .lock_identity = lock_identity,
    };
}

[[nodiscard]] inline PrivateLeaseRecord
make_private_lease_owner_record(const PrivateLeaseRecord& reserved,
                                const std::array<std::uint64_t, 3>& directory_identity) {
    auto owner = reserved;
    owner.phase = PrivateLeasePhase::Owner;
    owner.capability = PrivateLeaseCapability::RemoveOwnerAndEmptyLease;
    owner.directory_identity = directory_identity;
    owner.reserved_digest = digest_words(serialize_private_lease_marker(reserved));
    return owner;
}

[[nodiscard]] inline PrivateLeaseRecord
make_private_lease_owned_record(const PrivateLeaseRecord& owner,
                                const std::array<std::uint64_t, 3>& owner_identity) {
    auto owned = owner;
    owned.phase = PrivateLeasePhase::Owned;
    owned.capability = PrivateLeaseCapability::RollbackPreactivePairAndLease;
    owned.owner_identity = owner_identity;
    return owned;
}

inline void validate_private_lease_record_chain(const PrivateLeaseRecord& reserved,
                                                const PrivateLeaseRecord& owned) {
    if (reserved.phase != PrivateLeasePhase::Reserved || owned.phase != PrivateLeasePhase::Owned ||
        owned.lease_id != reserved.lease_id ||
        owned.base_path_digest != reserved.base_path_digest ||
        owned.parent_identity != reserved.parent_identity ||
        owned.lock_identity != reserved.lock_identity ||
        owned.reserved_digest != digest_words(serialize_private_lease_marker(reserved))) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
}

[[nodiscard]] inline PrivateLeaseRecord owner_record_for(const PrivateLeaseRecord& owned) {
    auto owner = owned;
    owner.phase = PrivateLeasePhase::Owner;
    owner.capability = PrivateLeaseCapability::RemoveOwnerAndEmptyLease;
    owner.owner_identity = {};
    return owner;
}

inline void remove_owner_marker_durable_locked(const std::filesystem::path& directory_path,
                                               const PrivateLeaseRecord& owned,
                                               bool allow_missing) {
    const auto owner_path = private_lease_owner_path(directory_path);
    const auto expected_owner = owner_record_for(owned);
    const auto owner = inspect_private_lease_marker(owner_path);
    if (owner.kind == InspectKind::Missing) {
        if (!allow_missing) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
    } else {
        if (owner.kind == InspectKind::Error) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, owner.error);
        }
        if (owner.kind == InspectKind::Rejected ||
            parse_private_lease_marker(owner.bytes) != expected_owner ||
            stable_identity(owner.identity) != owned.owner_identity) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        remove_file(owner_path, OOCCleanupStage::None);
        sync_parent_directory(directory_path, OOCCleanupStage::None);
    }
    const auto pending_path = private_lease_owner_pending_path(directory_path);
    const auto pending = inspect_pending_file(pending_path, PRIVATE_LEASE_MARKER_BYTES);
    if (pending.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, pending.error);
    }
    if (pending.kind == InspectKind::Rejected ||
        (pending.kind == InspectKind::Present &&
         !marker_bytes_equal(pending.bytes, serialize_private_lease_marker(expected_owner)))) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    if (pending.kind == InspectKind::Present) {
        remove_file(pending_path, OOCCleanupStage::None);
        sync_parent_directory(directory_path, OOCCleanupStage::None);
    }
}

struct LoadedPrivateLeaseMarker final {
    PrivateLeaseRecord record;
    std::array<std::uint64_t, 3> identity{};
};

[[nodiscard]] inline std::optional<LoadedPrivateLeaseMarker>
load_optional_private_lease_marker(const std::filesystem::path& path) {
    const auto inspected = inspect_private_lease_marker(path);
    if (inspected.kind == InspectKind::Missing) {
        return std::nullopt;
    }
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (inspected.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    return LoadedPrivateLeaseMarker{
        .record = parse_private_lease_marker(inspected.bytes),
        .identity = stable_identity(inspected.identity),
    };
}

inline void remove_matching_private_lease_pending(const std::filesystem::path& pending_path,
                                                  const PrivateLeaseRecord& expected) {
    const auto pending = inspect_pending_file(pending_path, PRIVATE_LEASE_MARKER_BYTES);
    if (pending.kind == InspectKind::Missing) {
        return;
    }
    if (pending.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, pending.error);
    }
    if (pending.kind == InspectKind::Rejected ||
        !marker_bytes_equal(pending.bytes, serialize_private_lease_marker(expected))) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    remove_file(pending_path, OOCCleanupStage::None);
    sync_parent_directory(pending_path.parent_path(), OOCCleanupStage::None);
}

[[nodiscard]] inline OOCCleanupResult private_lease_completed() noexcept {
    return OOCCleanupResult{
        .status = OOCCleanupStatus::Completed,
        .stage = OOCCleanupStage::Completed,
        .native_error = {},
    };
}

[[nodiscard]] inline OOCCleanupResult private_lease_no_transaction() noexcept {
    return OOCCleanupResult{
        .status = OOCCleanupStatus::NoTransaction,
        .stage = OOCCleanupStage::None,
        .native_error = {},
    };
}

[[nodiscard]] inline OOCCleanupResult private_lease_interrupted() noexcept {
    return OOCCleanupResult{
        .status = OOCCleanupStatus::Interrupted,
        .stage = OOCCleanupStage::None,
        .native_error = {},
    };
}

inline void validate_private_lease_owner_at(const std::filesystem::path& directory_path,
                                            const PrivateLeaseRecord& owned) {
    if (capture_directory_identity_locked(directory_path) != owned.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    confirm_private_lease_marker(private_lease_owner_path(directory_path), owner_record_for(owned),
                                 owned.owner_identity);
}

[[nodiscard]] inline bool remove_preactive_pair_leaf_durable_locked(
    const std::filesystem::path& path, const std::filesystem::path& directory_path,
    OOCPrivateLeaseFaultPoint fault_point, const OOCPrivateLeaseTestHooks& hooks) {
    const auto before = inspect_file(path, 0, false);
    if (before.kind == InspectKind::Missing) {
        sync_parent_directory(directory_path, OOCCleanupStage::None);
        return false;
    }
    if (before.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, before.error);
    }
    if (before.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    // Establish and then recheck the exact regular single-link target before
    // the path-based unlink. The surrounding directory identity and external
    // BaseLock remain fixed for the entire operation.
    confirm_file_durable(path, before.identity, directory_path, OOCCleanupStage::None);
    const auto confirmed = inspect_file(path, 0, false);
    if (confirmed.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, confirmed.error);
    }
    if (confirmed.kind != InspectKind::Present ||
        !same_native_file(confirmed.identity, before.identity)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    remove_file(path, OOCCleanupStage::None);
    sync_parent_directory(directory_path, OOCCleanupStage::None);
    const auto after = inspect_file(path, 0, false);
    if (after.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, after.error);
    }
    if (after.kind != InspectKind::Missing) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    return should_interrupt_private_lease(hooks, fault_point);
}

inline void validate_preactive_pair_leaf_before_quarantine(const std::filesystem::path& path,
                                                           bool expected_present) {
    const auto inspected = inspect_file(path, 0, false);
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (expected_present) {
        if (inspected.kind == InspectKind::Rejected) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        if (inspected.kind != InspectKind::Present) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
    } else if (inspected.kind != InspectKind::Missing) {
        fail(inspected.kind == InspectKind::Rejected ? OOCCleanupStatus::ForeignReplacementPreserved
                                                     : OOCCleanupStatus::NamespaceConflict,
             OOCCleanupStage::None, protocol_error());
    }
}

/// Roll back a fresh pair only while the exact owned lease still has its
/// canonical RESERVED predecessor. The fixed directory is first moved back to
/// its generation-specific staging name, making the directory identity itself
/// the deletion capability. Only the owner marker and the two expected pair
/// leaves are accepted inside that directory.
[[nodiscard]] inline OOCCleanupResult
rollback_owned_preactive_pair_locked(const OOCCleanupPaths& paths, const PrivateLeaseRecord& owned,
                                     const OOCPrivateLeaseTestHooks& hooks) {
    if (owned.capability != PrivateLeaseCapability::RollbackPreactivePairAndLease) {
        fail(OOCCleanupStatus::RecoveryRequired, OOCCleanupStage::None, protocol_error());
    }

    const auto staging_path = private_lease_staging_path(paths, owned.lease_id);
    auto staging_identity = inspect_directory_identity_locked(staging_path);
    auto final_identity = inspect_directory_identity_locked(paths.private_directory);
    if (staging_identity && final_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (staging_identity && *staging_identity != owned.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (final_identity && *final_identity != owned.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }

    if (final_identity) {
        const auto entries =
            inspect_private_lease_preactive_entries(paths.private_directory, paths);
        if (!entries.owner) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
        validate_private_lease_owner_at(paths.private_directory, owned);
        validate_preactive_pair_leaf_before_quarantine(paths.index_path, entries.index);
        validate_preactive_pair_leaf_before_quarantine(paths.data_path, entries.data);

        const auto renamed = rename_no_replace(paths.private_directory, staging_path);
        switch (renamed.result) {
        case RenameResult::Succeeded:
            sync_parent_directory(staging_path.parent_path(), OOCCleanupStage::None);
            break;
        case RenameResult::DestinationExists:
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, renamed.error);
        case RenameResult::Unsupported:
            fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None, renamed.error);
        case RenameResult::Failed:
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, renamed.error);
        }
        if (inspect_directory_identity_locked(paths.private_directory)) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        staging_identity = inspect_directory_identity_locked(staging_path);
        if (!staging_identity || *staging_identity != owned.directory_identity) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        if (should_interrupt_private_lease(
                hooks, OOCPrivateLeaseFaultPoint::PreactiveDirectoryQuarantinedDurable)) {
            return private_lease_interrupted();
        }
    }

    if (staging_identity) {
        const auto entries = inspect_private_lease_preactive_entries(staging_path, paths);
        if (entries.owner) {
            validate_private_lease_owner_at(staging_path, owned);
        } else if (entries.index || entries.data) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }

        const auto staged_data = staging_path / paths.data_path.filename();
        if (entries.data && remove_preactive_pair_leaf_durable_locked(
                                staged_data, staging_path,
                                OOCPrivateLeaseFaultPoint::PreactiveDataRemovedDurable, hooks)) {
            return private_lease_interrupted();
        }
        const auto staged_index = staging_path / paths.index_path.filename();
        if (entries.index && remove_preactive_pair_leaf_durable_locked(
                                 staged_index, staging_path,
                                 OOCPrivateLeaseFaultPoint::PreactiveIndexRemovedDurable, hooks)) {
            return private_lease_interrupted();
        }

        remove_owner_marker_durable_locked(staging_path, owned, true);
        if (should_interrupt_private_lease(hooks, OOCPrivateLeaseFaultPoint::OwnerRemovedDurable)) {
            return private_lease_interrupted();
        }
        remove_empty_directory_durable_locked(staging_path, owned.directory_identity);
        if (should_interrupt_private_lease(
                hooks, OOCPrivateLeaseFaultPoint::FinalDirectoryRemovedDurable)) {
            return private_lease_interrupted();
        }
    } else {
        sync_parent_directory(paths.private_directory.parent_path(), OOCCleanupStage::None);
    }
    return private_lease_completed();
}

inline void rollback_reserved_staging_locked(const OOCCleanupPaths& paths, const BaseLock& lock,
                                             const std::array<std::uint64_t, 3>& parent_identity,
                                             const LoadedPrivateLeaseMarker& loaded_reserved) {
    const auto& reserved = loaded_reserved.record;
    validate_private_lease_record_context(reserved, paths, parent_identity, lock.identity());
    if (reserved.phase != PrivateLeasePhase::Reserved) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    confirm_private_lease_marker(paths.lease_reserved_path, reserved, loaded_reserved.identity);

    // RESERVED has authority over only its unpredictable staging leaf. It
    // never permits touching the fixed lease directory.
    if (inspect_directory_identity_locked(paths.private_directory)) {
        fail(OOCCleanupStatus::RecoveryRequired, OOCCleanupStage::None, protocol_error());
    }

    const auto staging_path = private_lease_staging_path(paths, reserved.lease_id);
    const auto staging_identity = inspect_directory_identity_locked(staging_path);
    auto owned_pending = load_optional_private_lease_marker(paths.lease_owned_pending_path);
    if (!staging_identity) {
        if (owned_pending) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
    } else {
        const auto entries = inspect_private_lease_control_entries(staging_path);
        const auto expected_owner = make_private_lease_owner_record(reserved, *staging_identity);
        std::optional<std::array<std::uint64_t, 3>> owner_identity;
        if (entries.owner) {
            FileIdentity identity;
            const auto owner =
                load_private_lease_marker(private_lease_owner_path(staging_path), &identity);
            if (owner != expected_owner) {
                fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
            }
            owner_identity = stable_identity(identity);
        }
        if (owned_pending) {
            if (!owner_identity) {
                fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
            }
            const auto expected_owned =
                make_private_lease_owned_record(expected_owner, *owner_identity);
            validate_private_lease_record_context(owned_pending->record, paths, parent_identity,
                                                  lock.identity());
            validate_private_lease_record_chain(reserved, owned_pending->record);
            if (owned_pending->record != expected_owned) {
                fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
            }
            remove_matching_private_lease_pending(paths.lease_owned_pending_path, expected_owned);
        }
        if (entries.owner_pending) {
            remove_matching_private_lease_pending(private_lease_owner_pending_path(staging_path),
                                                  expected_owner);
        }
        if (entries.owner) {
            const auto owner_path = private_lease_owner_path(staging_path);
            confirm_private_lease_marker(owner_path, expected_owner, *owner_identity);
            remove_file(owner_path, OOCCleanupStage::None);
            sync_parent_directory(staging_path, OOCCleanupStage::None);
        }
        remove_empty_directory_durable_locked(staging_path, *staging_identity);
    }

    remove_matching_private_lease_pending(paths.lease_reserved_pending_path, reserved);
    remove_private_lease_marker_durable(paths.lease_reserved_path, reserved,
                                        loaded_reserved.identity);
}

/// A crash while publishing a deferred worker handoff may leave only the
/// no-authority pending intent. RESERVED still authorizes rollback of the
/// exact preactivation directory, but the pending leaf must first prove that
/// it describes the current pair and then be durably discarded. Malformed,
/// linked, or foreign pending leaves remain fail-closed.
inline void discard_matching_preactive_intent_pending_locked(const OOCCleanupPaths& paths) {
    const auto canonical = inspect_file(paths.intent_path, MARKER_BYTES, true);
    if (canonical.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, canonical.error);
    }
    if (canonical.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    if (canonical.kind == InspectKind::Present) {
        return;
    }

    FileIdentity pending_identity;
    const auto pending = inspect_file(paths.intent_pending_path, MARKER_BYTES, true);
    if (pending.kind == InspectKind::Missing) {
        return;
    }
    if (pending.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, pending.error);
    }
    if (pending.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    const IntentRecord expected = parse_marker(pending.bytes, INTENT_MAGIC);
    pending_identity = pending.identity;
    confirm_existing_marker(paths.intent_pending_path, expected, INTENT_MAGIC,
                            OOCCleanupStage::None);
    const IntentRecord source = capture_source_pair(paths, expected.store_id);
    if (source != expected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    require_source_pair_unchanged(paths, expected);

    const auto rechecked = inspect_file(paths.intent_pending_path, MARKER_BYTES, true);
    if (rechecked.kind != InspectKind::Present || rechecked.identity != pending_identity ||
        !marker_bytes_equal(rechecked.bytes, pending.bytes)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    remove_file(paths.intent_pending_path, OOCCleanupStage::None);
    sync_parent_directory(paths.intent_pending_path.parent_path(), OOCCleanupStage::None);
    const auto absent = inspect_file(paths.intent_pending_path, 0, false);
    if (absent.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, absent.error);
    }
    if (absent.kind != InspectKind::Missing) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    require_source_pair_unchanged(paths, expected);
}

[[nodiscard]] inline OOCCleanupResult
recover_owned_private_lease_locked(const OOCCleanupPaths& paths, const BaseLock& lock,
                                   const std::array<std::uint64_t, 3>& parent_identity,
                                   const LoadedPrivateLeaseMarker& loaded_owned,
                                   const std::optional<LoadedPrivateLeaseMarker>& loaded_reserved,
                                   const OOCPrivateLeaseTestHooks& hooks) {
    const auto& owned = loaded_owned.record;
    validate_private_lease_record_context(owned, paths, parent_identity, lock.identity());
    if (owned.phase != PrivateLeasePhase::Owned) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    confirm_private_lease_marker(paths.lease_owned_path, owned, loaded_owned.identity);
    remove_matching_private_lease_pending(paths.lease_owned_pending_path, owned);

    if (loaded_reserved) {
        validate_private_lease_record_context(loaded_reserved->record, paths, parent_identity,
                                              lock.identity());
        validate_private_lease_record_chain(loaded_reserved->record, owned);
        confirm_private_lease_marker(paths.lease_reserved_path, loaded_reserved->record,
                                     loaded_reserved->identity);
        remove_matching_private_lease_pending(paths.lease_reserved_pending_path,
                                              loaded_reserved->record);
    }

    const bool preactive_pair_rollback =
        loaded_reserved &&
        owned.capability == PrivateLeaseCapability::RollbackPreactivePairAndLease;
    if (preactive_pair_rollback) {
        if (inspect_directory_identity_locked(paths.private_directory)) {
            inspect_private_lease_transaction_entries(paths.private_directory, paths);
        }
        discard_matching_preactive_intent_pending_locked(paths);
    }

    const auto staging_path = private_lease_staging_path(paths, owned.lease_id);
    const auto staging_identity = inspect_directory_identity_locked(staging_path);
    const auto final_identity = inspect_directory_identity_locked(paths.private_directory);
    if (staging_identity && final_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (staging_identity && *staging_identity != owned.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (final_identity && *final_identity != owned.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }

    if (staging_identity) {
        if (!loaded_reserved) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
        if (preactive_pair_rollback) {
            const auto rolled_back = rollback_owned_preactive_pair_locked(paths, owned, hooks);
            if (!rolled_back.completed()) {
                return rolled_back;
            }
        } else {
            const auto entries = inspect_private_lease_control_entries(staging_path);
            if (!entries.owner) {
                fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
            }
            validate_private_lease_owner_at(staging_path, owned);
            remove_owner_marker_durable_locked(staging_path, owned, false);
            remove_empty_directory_durable_locked(staging_path, owned.directory_identity);
        }
    } else if (final_identity) {
        const auto owner_path = private_lease_owner_path(paths.private_directory);
        const auto owner_inspection = inspect_private_lease_marker(owner_path);
        if (owner_inspection.kind == InspectKind::Error) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, owner_inspection.error);
        }
        if (owner_inspection.kind == InspectKind::Rejected) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        const bool owner_present = owner_inspection.kind == InspectKind::Present;
        if (owner_present) {
            validate_private_lease_owner_at(paths.private_directory, owned);
            inspect_private_lease_transaction_entries(paths.private_directory, paths);
            const auto pair_result =
                run_transaction_locked(paths, lock, nullptr, false, nullptr, nullptr, {});
            if (!pair_result.transaction_terminal()) {
                return pair_result;
            }
        }
        if (preactive_pair_rollback) {
            const auto rolled_back = rollback_owned_preactive_pair_locked(paths, owned, hooks);
            if (!rolled_back.completed()) {
                return rolled_back;
            }
        } else {
            try {
                require_pair_namespace_reusable_locked(paths);
            } catch (const Failure& failure) {
                if (failure.status == OOCCleanupStatus::NamespaceConflict) {
                    return OOCCleanupResult{
                        .status = OOCCleanupStatus::RecoveryRequired,
                        .stage = OOCCleanupStage::None,
                        .native_error = failure.error,
                    };
                }
                throw;
            }

            const auto entries = inspect_private_lease_control_entries(paths.private_directory);
            if (entries.owner != owner_present) {
                fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
            }
            remove_owner_marker_durable_locked(paths.private_directory, owned, true);
            if (should_interrupt_private_lease(hooks,
                                               OOCPrivateLeaseFaultPoint::OwnerRemovedDurable)) {
                return private_lease_interrupted();
            }
            remove_empty_directory_durable_locked(paths.private_directory,
                                                  owned.directory_identity);
            if (should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::FinalDirectoryRemovedDurable)) {
                return private_lease_interrupted();
            }
        }
    } else {
        // Cover rmdir-visible-but-not-yet-confirmed recovery. The parent sync
        // establishes the absence before external authority is consumed.
        sync_parent_directory(paths.private_directory.parent_path(), OOCCleanupStage::None);
    }

    if (loaded_reserved) {
        remove_private_lease_marker_durable(paths.lease_reserved_path, loaded_reserved->record,
                                            loaded_reserved->identity);
        if (should_interrupt_private_lease(hooks,
                                           OOCPrivateLeaseFaultPoint::ReservedRemovedDurable)) {
            return private_lease_interrupted();
        }
    }
    remove_private_lease_marker_durable(paths.lease_owned_path, owned, loaded_owned.identity);
    if (should_interrupt_private_lease(hooks, OOCPrivateLeaseFaultPoint::OwnedRemovedDurable)) {
        return private_lease_interrupted();
    }
    return private_lease_completed();
}

[[nodiscard]] inline OOCCleanupResult
recover_private_lease_locked(const OOCCleanupPaths& paths, const BaseLock& lock,
                             const OOCPrivateLeaseTestHooks& hooks = {}) {
    if (paths.private_directory.empty()) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    if (!lock.matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    const auto parent = paths.private_directory.parent_path();
    sync_parent_directory(parent, OOCCleanupStage::None);
    const auto parent_identity = capture_directory_identity_locked(parent);

    auto reserved = load_optional_private_lease_marker(paths.lease_reserved_path);
    if (!reserved) {
        const auto pending = load_optional_private_lease_marker(paths.lease_reserved_pending_path);
        if (pending) {
            validate_private_lease_record_context(pending->record, paths, parent_identity,
                                                  lock.identity());
            if (pending->record.phase != PrivateLeasePhase::Reserved) {
                fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
            }

            // A pending leaf is never a deletion capability and recovery must
            // not promote it into one. The pre-canonical crash boundary occurs
            // before mkdir, so only a completely directory-free state may
            // discard this exact no-authority publication leaf.
            const auto staging_path = private_lease_staging_path(paths, pending->record.lease_id);
            if (inspect_directory_identity_locked(staging_path) ||
                inspect_directory_identity_locked(paths.private_directory) ||
                load_optional_private_lease_marker(paths.lease_owned_path) ||
                load_optional_private_lease_marker(paths.lease_owned_pending_path)) {
                return OOCCleanupResult{
                    .status = OOCCleanupStatus::RecoveryRequired,
                    .stage = OOCCleanupStage::None,
                    .native_error = protocol_error(),
                };
            }
            remove_matching_private_lease_pending(paths.lease_reserved_pending_path,
                                                  pending->record);
            return private_lease_completed();
        }
    }

    auto owned = load_optional_private_lease_marker(paths.lease_owned_path);
    if (owned) {
        return recover_owned_private_lease_locked(paths, lock, parent_identity, *owned, reserved,
                                                  hooks);
    }

    if (reserved) {
        rollback_reserved_staging_locked(paths, lock, parent_identity, *reserved);
        return private_lease_completed();
    }

    const auto owned_pending = load_optional_private_lease_marker(paths.lease_owned_pending_path);
    if (owned_pending || inspect_directory_identity_locked(paths.private_directory)) {
        fail(OOCCleanupStatus::RecoveryRequired, OOCCleanupStage::None, protocol_error());
    }
    return private_lease_no_transaction();
}

} // namespace ooc_cleanup_detail

/// Recoverable cleanup for one closed V3 `.relidx`/`.reldata` pair.
///
/// An unspent move-only ownership receipt is the only authority that can
/// create an intent. The immutable SHA-256-protected intent records native
/// file identities, exact sizes, the complete V3 index header, and paired data
/// ownership. Live
/// files move to same-directory quarantine names with no-replace rename. A
/// marker is first made durable in its deterministic pending leaf and then
/// atomically renamed to its canonical name; pending leaves never authorize
/// deletion. A second immutable staged marker is published only after both
/// quarantined leaves revalidate; no unlink is authorized without both
/// canonical markers. Data and index are then unlinked, intent is durably
/// consumed to close delete authority, and staged is removed as a no-authority
/// completion tail.
///
/// The caller must close all writers, mappings, and reader handles first. A
/// per-base cross-process lock serializes cooperating callers. Parent namespace
/// control remains a trusted boundary; this prototype does not promise
/// non-interference against a malicious same-user process.
class OOCCleanupTransaction final {
public:
    [[nodiscard]] static OOCCleanupResult
    begin_or_resume(OOCCleanupOwnershipReceipt& ownership,
                    std::optional<OOCExactCleanupExpectation> exact = std::nullopt,
                    OOCCleanupTestHooks hooks = {}) noexcept {
        if (ownership.spent_ || ownership.base_path_.empty() || ownership.store_id_ == 0) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::InvalidRequest,
                .stage = OOCCleanupStage::None,
                .native_error = ooc_cleanup_detail::invalid_argument_error(),
            };
        }
        const OOCCleanupRequest request{
            .base_path = ownership.base_path_,
            .store_id = ownership.store_id_,
            .exact = std::move(exact),
        };
        const ooc_cleanup_detail::OwnershipProof proof{
            .base_path = ownership.base_path_,
            .store_id = ownership.store_id_,
            .index_identity =
                {
                    ownership.index_identity_.first,
                    ownership.index_identity_.second,
                    ownership.index_identity_.third,
                },
            .data_identity =
                {
                    ownership.data_identity_.first,
                    ownership.data_identity_.second,
                    ownership.data_identity_.third,
                },
        };
        bool consume_receipt = false;
        const auto result = invoke([&] {
            return ooc_cleanup_detail::run_transaction(request.base_path, &request, true, &proof,
                                                       &consume_receipt, hooks);
        });
        if (consume_receipt) {
            ownership.spent_ = true;
        }
        return result;
    }

    [[nodiscard]] static OOCCleanupResult resume(const OOCCleanupRequest& request,
                                                 OOCCleanupTestHooks hooks = {}) noexcept {
        return invoke([&] {
            return ooc_cleanup_detail::run_transaction(request.base_path, &request, false, nullptr,
                                                       nullptr, hooks);
        });
    }

    [[nodiscard]] static OOCCleanupResult resume(const std::filesystem::path& base_path,
                                                 OOCCleanupTestHooks hooks = {}) noexcept {
        return invoke([&] {
            return ooc_cleanup_detail::run_transaction(base_path, nullptr, false, nullptr, nullptr,
                                                       hooks);
        });
    }

    [[nodiscard]] static OOCCleanupPaths paths_for(const std::filesystem::path& base_path) {
        return ooc_cleanup_detail::freeze_paths(base_path);
    }

    /// Reserve one fresh RelationSink private directory. The directory name is
    /// derived from the recognized `<requested>.gnfs-sink-lease/corpus`
    /// layout, and its lock is a persistent sibling outside the removable
    /// directory. The returned receipt retains that same lock until a fresh
    /// writer has created both O_EXCL artifacts and activates the lease.
    [[nodiscard]] static OOCPrivateLeaseReservation
    reserve_private_lease(const std::filesystem::path& base_path,
                          OOCPrivateLeaseTestHooks hooks = {}) noexcept {
        std::optional<OOCPrivateLeaseOwnershipReceipt> ownership;
        const auto result = invoke([&] {
            const auto paths = ooc_cleanup_detail::freeze_paths(base_path);
            if (paths.private_directory.empty()) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                         ooc_cleanup_detail::invalid_argument_error());
            }

            // A foreign fixed-directory collision must not cause even the
            // persistent lock leaf to be created. Crash recovery always has a
            // lock that was durably published before protocol directories.
            const auto initial_lock = ooc_cleanup_detail::inspect_file(paths.lock_path, 0, false);
            if (initial_lock.kind == ooc_cleanup_detail::InspectKind::Error) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None,
                                         initial_lock.error);
            }
            if (initial_lock.kind == ooc_cleanup_detail::InspectKind::Missing) {
                const bool foreign_protocol_state =
                    ooc_cleanup_detail::inspect_directory_identity_locked(paths.private_directory)
                        .has_value() ||
                    ooc_cleanup_detail::load_optional_private_lease_marker(
                        paths.lease_reserved_path)
                        .has_value() ||
                    ooc_cleanup_detail::load_optional_private_lease_marker(
                        paths.lease_reserved_pending_path)
                        .has_value() ||
                    ooc_cleanup_detail::load_optional_private_lease_marker(paths.lease_owned_path)
                        .has_value() ||
                    ooc_cleanup_detail::load_optional_private_lease_marker(
                        paths.lease_owned_pending_path)
                        .has_value();
                if (foreign_protocol_state) {
                    ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict,
                                             OOCCleanupStage::None,
                                             ooc_cleanup_detail::protocol_error());
                }
            }
            if (initial_lock.kind == ooc_cleanup_detail::InspectKind::Rejected) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }

            auto live_lock = std::make_shared<ooc_cleanup_detail::BaseLock>(paths.lock_path);
            const auto recovered =
                ooc_cleanup_detail::recover_private_lease_locked(paths, *live_lock);
            if (!recovered.transaction_terminal()) {
                return recovered;
            }
            ooc_cleanup_detail::require_pair_namespace_reusable_locked(paths);

            const auto parent = paths.private_directory.parent_path();
            ooc_cleanup_detail::sync_parent_directory(parent, OOCCleanupStage::None);
            if (ooc_cleanup_detail::inspect_directory_identity_locked(paths.private_directory) ||
                ooc_cleanup_detail::load_optional_private_lease_marker(paths.lease_reserved_path) ||
                ooc_cleanup_detail::load_optional_private_lease_marker(paths.lease_owned_path) ||
                ooc_cleanup_detail::load_optional_private_lease_marker(
                    paths.lease_reserved_pending_path) ||
                ooc_cleanup_detail::load_optional_private_lease_marker(
                    paths.lease_owned_pending_path)) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }

            const auto parent_identity =
                ooc_cleanup_detail::capture_directory_identity_locked(parent);
            const auto lease_id = ooc_cleanup_detail::allocate_private_lease_id();
            const auto staging_path =
                ooc_cleanup_detail::private_lease_staging_path(paths, lease_id);
            if (ooc_cleanup_detail::inspect_directory_identity_locked(staging_path)) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            const auto reserved = ooc_cleanup_detail::make_private_lease_reserved_record(
                paths, lease_id, parent_identity, live_lock->identity());
            const auto reserved_publication =
                ooc_cleanup_detail::publish_private_lease_marker_durable(
                    paths.lease_reserved_path, paths.lease_reserved_pending_path, reserved, hooks,
                    OOCPrivateLeaseFaultPoint::ReservedPendingDurable);
            if (reserved_publication.interrupted) {
                return ooc_cleanup_detail::private_lease_interrupted();
            }
            if (ooc_cleanup_detail::should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::ReservedDurable)) {
                return ooc_cleanup_detail::private_lease_interrupted();
            }
            ooc_cleanup_detail::create_directory_durable_locked(staging_path);
            const auto directory_identity =
                ooc_cleanup_detail::capture_directory_identity_locked(staging_path);
            if (ooc_cleanup_detail::should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::StagingDirectoryDurable)) {
                return ooc_cleanup_detail::private_lease_interrupted();
            }

            const auto owner =
                ooc_cleanup_detail::make_private_lease_owner_record(reserved, directory_identity);
            const auto owner_publication = ooc_cleanup_detail::publish_private_lease_marker_durable(
                ooc_cleanup_detail::private_lease_owner_path(staging_path),
                ooc_cleanup_detail::private_lease_owner_pending_path(staging_path), owner, hooks,
                OOCPrivateLeaseFaultPoint::OwnerPendingDurable);
            if (owner_publication.interrupted) {
                return ooc_cleanup_detail::private_lease_interrupted();
            }
            if (ooc_cleanup_detail::should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::OwnerDurable)) {
                return ooc_cleanup_detail::private_lease_interrupted();
            }

            const auto owned = ooc_cleanup_detail::make_private_lease_owned_record(
                owner, owner_publication.identity);
            const auto owned_publication = ooc_cleanup_detail::publish_private_lease_marker_durable(
                paths.lease_owned_path, paths.lease_owned_pending_path, owned, hooks,
                OOCPrivateLeaseFaultPoint::OwnedPendingDurable);
            if (owned_publication.interrupted) {
                return ooc_cleanup_detail::private_lease_interrupted();
            }
            if (ooc_cleanup_detail::should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::OwnedDurable)) {
                return ooc_cleanup_detail::private_lease_interrupted();
            }

            const auto renamed =
                ooc_cleanup_detail::rename_no_replace(staging_path, paths.private_directory);
            switch (renamed.result) {
            case ooc_cleanup_detail::RenameResult::Succeeded:
                ooc_cleanup_detail::sync_parent_directory(parent, OOCCleanupStage::None);
                break;
            case ooc_cleanup_detail::RenameResult::DestinationExists:
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         renamed.error);
            case ooc_cleanup_detail::RenameResult::Unsupported:
                ooc_cleanup_detail::fail(OOCCleanupStatus::PlatformUnsupported,
                                         OOCCleanupStage::None, renamed.error);
            case ooc_cleanup_detail::RenameResult::Failed:
                ooc_cleanup_detail::fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None,
                                         renamed.error);
            }
            if (ooc_cleanup_detail::inspect_directory_identity_locked(staging_path) ||
                ooc_cleanup_detail::capture_directory_identity_locked(paths.private_directory) !=
                    directory_identity) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            ooc_cleanup_detail::validate_private_lease_owner_at(paths.private_directory, owned);
            if (ooc_cleanup_detail::should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::FinalRenameDurable)) {
                return ooc_cleanup_detail::private_lease_interrupted();
            }

            OOCPrivateLeaseOwnershipReceipt candidate(
                paths.base_path, paths.private_directory, paths.lock_path, directory_identity,
                lease_id, owner_publication.identity, owned_publication.identity,
                std::move(live_lock), static_cast<std::uint64_t>(gnfs::util::process_id()));
            static_assert(std::is_nothrow_move_constructible_v<OOCPrivateLeaseOwnershipReceipt>);
            ownership.emplace(std::move(candidate));
            return ooc_cleanup_detail::private_lease_completed();
        });
        return OOCPrivateLeaseReservation{
            .result = result,
            .ownership = std::move(ownership),
        };
    }

    /// Remove one exact private lease after its pair namespace is fully empty.
    /// The external lock remains permanently in place, so directory reuse
    /// cannot create a second simultaneously valid lock inode.
    [[nodiscard]] static OOCCleanupResult
    remove_private_lease(OOCPrivateLeaseOwnershipReceipt& ownership,
                         OOCPrivateLeaseTestHooks hooks = {}) noexcept {
        if (ownership.spent_ || ownership.base_path_.empty() ||
            ownership.private_directory_.empty() || ownership.lock_path_.empty() ||
            ownership.owner_process_id_ != static_cast<std::uint64_t>(gnfs::util::process_id())) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::InvalidRequest,
                .stage = OOCCleanupStage::None,
                .native_error = ooc_cleanup_detail::invalid_argument_error(),
            };
        }

        const auto result = invoke([&] {
            const auto paths = ooc_cleanup_detail::freeze_paths(ownership.base_path_);
            if (paths.private_directory.empty() || paths.base_path != ownership.base_path_ ||
                paths.private_directory != ownership.private_directory_ ||
                paths.lock_path != ownership.lock_path_) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                         ooc_cleanup_detail::invalid_argument_error());
            }
            std::shared_ptr<ooc_cleanup_detail::BaseLock> held_lock = ownership.live_lock_;
            if (!held_lock) {
                held_lock = std::make_shared<ooc_cleanup_detail::BaseLock>(paths.lock_path, false);
            }
            if (!held_lock->matches(paths.lock_path)) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                         ooc_cleanup_detail::invalid_argument_error());
            }
            const auto parent_identity = ooc_cleanup_detail::capture_directory_identity_locked(
                paths.private_directory.parent_path());

            auto loaded_owned =
                ooc_cleanup_detail::load_optional_private_lease_marker(paths.lease_owned_path);
            const auto current_directory =
                ooc_cleanup_detail::inspect_directory_identity_locked(paths.private_directory);
            if (!loaded_owned) {
                if (current_directory || ooc_cleanup_detail::load_optional_private_lease_marker(
                                             paths.lease_reserved_path)) {
                    ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict,
                                             OOCCleanupStage::None,
                                             ooc_cleanup_detail::protocol_error());
                }
                ooc_cleanup_detail::sync_parent_directory(paths.private_directory.parent_path(),
                                                          OOCCleanupStage::None);
                return ooc_cleanup_detail::private_lease_completed();
            }
            if (loaded_owned->record.lease_id != ownership.lease_id_ ||
                loaded_owned->record.directory_identity != ownership.directory_identity_ ||
                loaded_owned->record.owner_identity != ownership.owner_identity_ ||
                loaded_owned->identity != ownership.owned_identity_) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::ForeignReplacementPreserved,
                                         OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            ooc_cleanup_detail::validate_private_lease_record_context(
                loaded_owned->record, paths, parent_identity, held_lock->identity());

            auto loaded_reserved =
                ooc_cleanup_detail::load_optional_private_lease_marker(paths.lease_reserved_path);
            if (loaded_reserved) {
                ooc_cleanup_detail::validate_private_lease_record_context(
                    loaded_reserved->record, paths, parent_identity, held_lock->identity());
                ooc_cleanup_detail::validate_private_lease_record_chain(loaded_reserved->record,
                                                                        loaded_owned->record);
            }

            return ooc_cleanup_detail::recover_owned_private_lease_locked(
                paths, *held_lock, parent_identity, *loaded_owned, loaded_reserved, hooks);
        });
        if (result.completed()) {
            ownership.spent_ = true;
            ownership.live_lock_.reset();
        }
        return result;
    }

    /// Recover a crash-left lease only through durable protocol authority.
    /// A new-capability lease with RESERVED still present may quarantine and
    /// roll back its exact preactivation directory. Active live pairs, for
    /// which activation already consumed RESERVED, remain preserved without a
    /// canonical pair cleanup intent.
    [[nodiscard]] static OOCCleanupResult
    recover_private_lease(const std::filesystem::path& base_path,
                          OOCPrivateLeaseTestHooks hooks = {}) noexcept {
        return invoke([&] {
            const auto paths = ooc_cleanup_detail::freeze_paths(base_path);
            if (paths.private_directory.empty()) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                         ooc_cleanup_detail::invalid_argument_error());
            }
            const auto lock_inspection =
                ooc_cleanup_detail::inspect_file(paths.lock_path, 0, false);
            if (lock_inspection.kind == ooc_cleanup_detail::InspectKind::Error) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None,
                                         lock_inspection.error);
            }
            if (lock_inspection.kind == ooc_cleanup_detail::InspectKind::Missing) {
                if (!ooc_cleanup_detail::inspect_directory_identity_locked(
                        paths.private_directory) &&
                    !ooc_cleanup_detail::load_optional_private_lease_marker(
                        paths.lease_reserved_path) &&
                    !ooc_cleanup_detail::load_optional_private_lease_marker(
                        paths.lease_reserved_pending_path) &&
                    !ooc_cleanup_detail::load_optional_private_lease_marker(
                        paths.lease_owned_path) &&
                    !ooc_cleanup_detail::load_optional_private_lease_marker(
                        paths.lease_owned_pending_path)) {
                    return ooc_cleanup_detail::private_lease_no_transaction();
                }
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            if (lock_inspection.kind != ooc_cleanup_detail::InspectKind::Present) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            ooc_cleanup_detail::BaseLock lock(paths.lock_path, false);
            return ooc_cleanup_detail::recover_private_lease_locked(paths, lock, hooks);
        });
    }

    /// Confirm that no live, quarantined, pending, or canonical cleanup leaf
    /// remains for this pair. The persistent regular lock leaf is deliberately
    /// retained and is not part of the pair-reuse decision.
    [[nodiscard]] static OOCCleanupResult
    confirm_pair_namespace_reusable(const std::filesystem::path& base_path) noexcept {
        return invoke([&] {
            const auto paths = ooc_cleanup_detail::freeze_paths(base_path);
            ooc_cleanup_detail::BaseLock lock(paths.lock_path, paths.private_directory.empty());
            ooc_cleanup_detail::require_pair_namespace_reusable_locked(paths);
            return OOCCleanupResult{
                .status = OOCCleanupStatus::Completed,
                .stage = OOCCleanupStage::Completed,
                .native_error = {},
            };
        });
    }

private:
    /// Publish the exact fresh pair's canonical cleanup intent while retaining
    /// the private lease's RESERVED predecessor and held BaseLock. This is the
    /// narrow child-to-parent handoff used by deferred fork workers: success
    /// consumes pair ownership but deliberately does not quarantine or delete
    /// either artifact.
    [[nodiscard]] static OOCCleanupResult publish_private_lease_cleanup_handoff(
        OOCCleanupOwnershipReceipt& pair_ownership, OOCPrivateLeaseOwnershipReceipt& lease,
        const OOCExactCleanupExpectation& exact, OOCCleanupTestHooks hooks = {}) noexcept {
        if (pair_ownership.spent_ || pair_ownership.store_id_ == 0 || lease.spent_ ||
            lease.active_ || !lease.live_lock_ ||
            !ooc_cleanup_detail::expectation_is_well_formed(exact)) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::InvalidRequest,
                .stage = OOCCleanupStage::None,
                .native_error = ooc_cleanup_detail::invalid_argument_error(),
            };
        }

        const OOCCleanupRequest request{
            .base_path = pair_ownership.base_path_,
            .store_id = pair_ownership.store_id_,
            .exact = exact,
        };
        const ooc_cleanup_detail::OwnershipProof proof{
            .base_path = pair_ownership.base_path_,
            .store_id = pair_ownership.store_id_,
            .index_identity =
                {
                    pair_ownership.index_identity_.first,
                    pair_ownership.index_identity_.second,
                    pair_ownership.index_identity_.third,
                },
            .data_identity =
                {
                    pair_ownership.data_identity_.first,
                    pair_ownership.data_identity_.second,
                    pair_ownership.data_identity_.third,
                },
        };

        bool consume_receipt = false;
        const auto result = invoke([&] {
            const auto paths = ooc_cleanup_detail::freeze_paths(lease.base_path_);
            if (paths.base_path != lease.base_path_ ||
                paths.private_directory != lease.private_directory_ ||
                paths.lock_path != lease.lock_path_ ||
                pair_ownership.base_path_ != lease.base_path_) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                         ooc_cleanup_detail::invalid_argument_error());
            }

            auto& lock = *lease.live_lock_;
            const auto parent_identity = ooc_cleanup_detail::capture_directory_identity_locked(
                paths.private_directory.parent_path());
            auto reserved =
                ooc_cleanup_detail::load_optional_private_lease_marker(paths.lease_reserved_path);
            auto owned =
                ooc_cleanup_detail::load_optional_private_lease_marker(paths.lease_owned_path);
            if (!reserved || !owned || reserved->record.lease_id != lease.lease_id_ ||
                owned->record.lease_id != lease.lease_id_ ||
                owned->record.directory_identity != lease.directory_identity_ ||
                owned->record.owner_identity != lease.owner_identity_ ||
                owned->identity != lease.owned_identity_) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            ooc_cleanup_detail::validate_private_lease_record_context(
                reserved->record, paths, parent_identity, lock.identity());
            ooc_cleanup_detail::validate_private_lease_record_context(
                owned->record, paths, parent_identity, lock.identity());
            ooc_cleanup_detail::validate_private_lease_record_chain(reserved->record,
                                                                    owned->record);
            ooc_cleanup_detail::validate_private_lease_owner_at(paths.private_directory,
                                                                owned->record);
            if (ooc_cleanup_detail::inspect_directory_identity_locked(
                    ooc_cleanup_detail::private_lease_staging_path(paths, lease.lease_id_))) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }

            return ooc_cleanup_detail::run_transaction_locked(paths, lock, &request, true, &proof,
                                                              &consume_receipt, hooks, true);
        });
        if (consume_receipt) {
            pair_ownership.spent_ = true;
        }
        return result;
    }

    [[nodiscard]] static OOCCleanupResult
    activate_private_lease_for_fresh_writer(OOCPrivateLeaseOwnershipReceipt& lease,
                                            const OOCCleanupOwnershipReceipt& pair_ownership,
                                            OOCPrivateLeaseTestHooks hooks = {}) noexcept {
        if (lease.spent_ || lease.active_ || !lease.live_lock_ || pair_ownership.spent_ ||
            pair_ownership.store_id_ == 0 ||
            lease.owner_process_id_ != static_cast<std::uint64_t>(gnfs::util::process_id())) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::InvalidRequest,
                .stage = OOCCleanupStage::None,
                .native_error = ooc_cleanup_detail::invalid_argument_error(),
            };
        }
        const auto result = invoke([&] {
            const auto paths = ooc_cleanup_detail::freeze_paths(lease.base_path_);
            if (paths.base_path != lease.base_path_ ||
                paths.private_directory != lease.private_directory_ ||
                paths.lock_path != lease.lock_path_ ||
                pair_ownership.base_path_ != lease.base_path_) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                         ooc_cleanup_detail::invalid_argument_error());
            }
            auto& lock = *lease.live_lock_;
            const auto parent_identity = ooc_cleanup_detail::capture_directory_identity_locked(
                paths.private_directory.parent_path());
            auto reserved =
                ooc_cleanup_detail::load_optional_private_lease_marker(paths.lease_reserved_path);
            auto owned =
                ooc_cleanup_detail::load_optional_private_lease_marker(paths.lease_owned_path);
            if (!reserved || !owned || reserved->record.lease_id != lease.lease_id_ ||
                owned->record.lease_id != lease.lease_id_ ||
                owned->record.directory_identity != lease.directory_identity_ ||
                owned->record.owner_identity != lease.owner_identity_ ||
                owned->identity != lease.owned_identity_) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            ooc_cleanup_detail::validate_private_lease_record_context(
                reserved->record, paths, parent_identity, lock.identity());
            ooc_cleanup_detail::validate_private_lease_record_context(
                owned->record, paths, parent_identity, lock.identity());
            ooc_cleanup_detail::validate_private_lease_record_chain(reserved->record,
                                                                    owned->record);
            ooc_cleanup_detail::validate_private_lease_owner_at(paths.private_directory,
                                                                owned->record);
            if (ooc_cleanup_detail::inspect_directory_identity_locked(
                    ooc_cleanup_detail::private_lease_staging_path(paths, lease.lease_id_))) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }

            const ooc_cleanup_detail::OwnershipProof proof{
                .base_path = pair_ownership.base_path_,
                .store_id = pair_ownership.store_id_,
                .index_identity =
                    {
                        pair_ownership.index_identity_.first,
                        pair_ownership.index_identity_.second,
                        pair_ownership.index_identity_.third,
                    },
                .data_identity =
                    {
                        pair_ownership.data_identity_.first,
                        pair_ownership.data_identity_.second,
                        pair_ownership.data_identity_.third,
                    },
            };
            const auto pair =
                ooc_cleanup_detail::capture_source_pair(paths, pair_ownership.store_id_);
            if (!ooc_cleanup_detail::ownership_proof_matches(proof, paths, pair)) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            ooc_cleanup_detail::require_source_pair_unchanged(paths, pair);
            const std::array<const std::filesystem::path*, 6> no_authority_leaves{
                &paths.intent_path,           &paths.intent_pending_path,
                &paths.staged_path,           &paths.staged_pending_path,
                &paths.quarantine_index_path, &paths.quarantine_data_path,
            };
            for (const auto* leaf : no_authority_leaves) {
                const auto inspected = ooc_cleanup_detail::inspect_file(*leaf, 0, false);
                if (inspected.kind == ooc_cleanup_detail::InspectKind::Error) {
                    ooc_cleanup_detail::fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None,
                                             inspected.error);
                }
                if (inspected.kind != ooc_cleanup_detail::InspectKind::Missing) {
                    ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict,
                                             OOCCleanupStage::None,
                                             ooc_cleanup_detail::protocol_error());
                }
            }

            ooc_cleanup_detail::remove_matching_private_lease_pending(
                paths.lease_reserved_pending_path, reserved->record);
            ooc_cleanup_detail::remove_private_lease_marker_durable(
                paths.lease_reserved_path, reserved->record, reserved->identity);
            if (ooc_cleanup_detail::should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::ReservedRemovedDurable)) {
                return ooc_cleanup_detail::private_lease_interrupted();
            }
            return ooc_cleanup_detail::private_lease_completed();
        });
        if (result.completed()) {
            lease.active_ = true;
            lease.live_lock_.reset();
        }
        return result;
    }

    [[nodiscard]] static OOCCleanupOwnershipReceipt
    capture_fresh_ownership_receipt(const std::filesystem::path& base_path, std::uint64_t store_id,
                                    const std::array<std::uint64_t, 3>& expected_index_identity,
                                    const std::array<std::uint64_t, 3>& expected_data_identity) {
        const auto paths = ooc_cleanup_detail::freeze_paths(base_path);
        const auto source = ooc_cleanup_detail::capture_source_pair(paths, store_id);
        ooc_cleanup_detail::require_source_pair_unchanged(paths, source);
        const auto index_identity = ooc_cleanup_detail::stable_identity(source.index.identity);
        const auto data_identity = ooc_cleanup_detail::stable_identity(source.data.identity);
        if (index_identity != expected_index_identity || data_identity != expected_data_identity ||
            index_identity == data_identity) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None,
                                     ooc_cleanup_detail::protocol_error());
        }
        return OOCCleanupOwnershipReceipt(paths.base_path, store_id,
                                          OOCCleanupOwnershipReceipt::NativeIdentity{
                                              .first = source.index.identity.first,
                                              .second = source.index.identity.second,
                                              .third = source.index.identity.third,
                                          },
                                          OOCCleanupOwnershipReceipt::NativeIdentity{
                                              .first = source.data.identity.first,
                                              .second = source.data.identity.second,
                                              .third = source.data.identity.third,
                                          });
    }

    template <typename Operation>
    [[nodiscard]] static OOCCleanupResult invoke(Operation&& operation) noexcept {
        try {
            return std::forward<Operation>(operation)();
        } catch (const ooc_cleanup_detail::Failure& failure) {
            return OOCCleanupResult{
                .status = failure.status,
                .stage = failure.stage,
                .native_error = failure.error,
            };
        } catch (const std::filesystem::filesystem_error& error) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::IoFailure,
                .stage = OOCCleanupStage::None,
                .native_error = error.code(),
            };
        } catch (const std::system_error& error) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::IoFailure,
                .stage = OOCCleanupStage::None,
                .native_error = error.code(),
            };
        } catch (...) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::UnexpectedFailure,
                .stage = OOCCleanupStage::None,
                .native_error = {},
            };
        }
    }

    friend class OOCRelationWriter;
};

} // namespace gnfs::relation
