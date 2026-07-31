#include "ooc_private_cleanup_action_permit_internal.hpp"
#include "ooc_private_handoff_adoption_internal.hpp"
#include "ooc_private_lease_recovery_internal.hpp"
#include "ooc_private_lease_reservation_protocol_internal.hpp"

#include <gnfs/relation/ooc_authorized_cleanup_intent.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <dirent.h>
#endif

namespace gnfs::relation {

class ooc_cleanup_detail::OOCPrivateLeaseRecoveryBuilderV1 final {
public:
    template <typename Operation>
    [[nodiscard]] static OOCCleanupResult invoke(Operation&& operation) noexcept {
        return OOCCleanupTransaction::invoke(std::forward<Operation>(operation));
    }
};

} // namespace gnfs::relation

namespace gnfs::relation::ooc_cleanup_detail {

OOCPrivateLeaseRecoveryBorrowedBaseLockV1::OOCPrivateLeaseRecoveryBorrowedBaseLockV1(
    int parent_descriptor, int lock_descriptor, std::string_view lock_leaf,
    std::array<std::uint64_t, 3> lock_identity, std::uint64_t creator_process_id) noexcept
    : parent_descriptor_(parent_descriptor), lock_descriptor_(lock_descriptor),
      lock_leaf_(lock_leaf), lock_identity_(lock_identity),
      creator_process_id_(creator_process_id) {}

OOCPrivateLeaseRecoveryBorrowedBaseLockV1::OOCPrivateLeaseRecoveryBorrowedBaseLockV1(
    OOCPrivateLeaseRecoveryBorrowedBaseLockV1&& other) noexcept
    : parent_descriptor_(std::exchange(other.parent_descriptor_, -1)),
      lock_descriptor_(std::exchange(other.lock_descriptor_, -1)),
      lock_leaf_(std::exchange(other.lock_leaf_, {})), lock_identity_(other.lock_identity_),
      creator_process_id_(std::exchange(other.creator_process_id_, 0)),
      consumed_(std::exchange(other.consumed_, true)) {}

static_assert(!std::is_default_constructible_v<OOCPrivateLeaseRecoveryBorrowedBaseLockV1>);
static_assert(!std::is_copy_constructible_v<OOCPrivateLeaseRecoveryBorrowedBaseLockV1>);
static_assert(!std::is_copy_assignable_v<OOCPrivateLeaseRecoveryBorrowedBaseLockV1>);
static_assert(std::is_nothrow_move_constructible_v<OOCPrivateLeaseRecoveryBorrowedBaseLockV1>);
static_assert(!std::is_move_assignable_v<OOCPrivateLeaseRecoveryBorrowedBaseLockV1>);

namespace {

#if !defined(_WIN32)

class OOCPrivateLeaseRecoveryParentHandleV1 final {
public:
    OOCPrivateLeaseRecoveryParentHandleV1(const OOCPrivateLeaseRecoveryParentHandleV1&) = delete;
    OOCPrivateLeaseRecoveryParentHandleV1&
    operator=(const OOCPrivateLeaseRecoveryParentHandleV1&) = delete;
    OOCPrivateLeaseRecoveryParentHandleV1(OOCPrivateLeaseRecoveryParentHandleV1&&) = delete;
    OOCPrivateLeaseRecoveryParentHandleV1&
    operator=(OOCPrivateLeaseRecoveryParentHandleV1&&) = delete;

    OOCPrivateLeaseRecoveryParentHandleV1(int source_descriptor, const std::filesystem::path& path)
        : path_(path) {
        if (source_descriptor < 0) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        do {
            descriptor_ = ::fcntl(source_descriptor, F_DUPFD_CLOEXEC, 0);
        } while (descriptor_ < 0 && errno == EINTR);
        if (descriptor_ < 0) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
        }
        try {
            require_initial_binding();
        } catch (...) {
            if (descriptor_ >= 0) {
                (void)::close(descriptor_);
                descriptor_ = -1;
            }
            throw;
        }
    }

    ~OOCPrivateLeaseRecoveryParentHandleV1() {
        if (descriptor_ >= 0) {
            (void)::close(descriptor_);
        }
    }

    [[nodiscard]] int descriptor() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] const std::array<std::uint64_t, 3>& identity() const noexcept {
        return identity_;
    }

    void require_stable() const {
        struct stat held{};
        struct stat named{};
        if (::fstat(descriptor_, &held) != 0 || ::lstat(path_.c_str(), &named) != 0) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                 posix_error(errno == 0 ? EACCES : errno));
        }
        if (!valid_parent(held) || !valid_parent(named) ||
            stable_identity(posix_identity(held)) != identity_ ||
            stable_identity(posix_identity(named)) != identity_ || held.st_dev != named.st_dev ||
            held.st_ino != named.st_ino) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
    }

private:
    [[nodiscard]] static bool valid_parent(const struct stat& metadata) noexcept {
        return S_ISDIR(metadata.st_mode) &&
               (metadata.st_mode & static_cast<mode_t>(07777)) == static_cast<mode_t>(0700) &&
               metadata.st_uid == ::geteuid();
    }

    void require_initial_binding() {
        struct stat held{};
        struct stat named{};
        if (::fstat(descriptor_, &held) != 0 || ::lstat(path_.c_str(), &named) != 0 ||
            !valid_parent(held) || !valid_parent(named) || held.st_dev != named.st_dev ||
            held.st_ino != named.st_ino) {
            const int saved_errno = errno == 0 ? EACCES : errno;
            (void)::close(descriptor_);
            descriptor_ = -1;
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                 posix_error(saved_errno));
        }
        identity_ = stable_identity(posix_identity(held));
        require_stable();
    }

    int descriptor_ = -1;
    std::filesystem::path path_;
    std::array<std::uint64_t, 3> identity_{};
};

#endif

[[nodiscard]] bool all_zero_words(const std::array<std::uint64_t, 2>& values) noexcept {
    return values[0] == 0 && values[1] == 0;
}

template <std::size_t Size>
[[nodiscard]] bool all_zero_words(const std::array<std::uint64_t, Size>& values) noexcept {
    return std::all_of(values.begin(), values.end(),
                       [](std::uint64_t value) { return value == 0; });
}

void require_recovery_file_expectation(
    const std::filesystem::path& path,
    const std::optional<OOCPreactiveLeaseRecoveryFileExpectationV1>& expected) {
    const auto inspected = inspect_file(path, 0, false);
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (!expected) {
        if (inspected.kind != InspectKind::Missing) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        return;
    }
    if (inspected.kind != InspectKind::Present ||
        stable_identity(inspected.identity) != expected->identity ||
        inspected.identity.size != expected->extent) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
#if !defined(_WIN32)
    struct stat named{};
    if (::lstat(path.c_str(), &named) != 0 || !S_ISREG(named.st_mode) || named.st_nlink != 1 ||
        named.st_size < 0 || stable_identity(posix_identity(named)) != expected->identity ||
        static_cast<std::uint64_t>(named.st_size) != expected->extent ||
        static_cast<std::uint64_t>(named.st_uid) != static_cast<std::uint64_t>(::geteuid()) ||
        (named.st_mode & static_cast<mode_t>(07777)) != static_cast<mode_t>(0600)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             errno == 0 ? protocol_error() : posix_error(errno));
    }
#endif
    const auto confirmed = inspect_file(path, 0, false);
    if (confirmed.kind != InspectKind::Present || confirmed.identity != inspected.identity) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
}

void require_preactive_recovery_expectation(const OOCCleanupPaths& paths, const BaseLock& lock,
                                            const OOCPreactiveLeaseRecoveryExpectationV1& expected
#if !defined(_WIN32)
                                            ,
                                            const OOCPrivateLeaseRecoveryParentHandleV1& parent
#endif
) {
    const bool raw_phase =
        expected.phase == OOCPreactiveLeaseRecoveryPhaseV1::FinalDirectoryRawPair ||
        expected.phase == OOCPreactiveLeaseRecoveryPhaseV1::StagingDirectoryRawPair;
    const bool reserved_absent =
        expected.phase == OOCPreactiveLeaseRecoveryPhaseV1::DirectoryAbsentOwnedOnly;
    if (expected.phase == OOCPreactiveLeaseRecoveryPhaseV1::Count ||
        all_zero_words(expected.lease_id) || all_zero_words(expected.directory_identity) ||
        all_zero_words(expected.owner_marker_identity) ||
        all_zero_words(expected.owned_marker_identity) ||
        (expected.reserved_marker_identity.has_value() == reserved_absent) ||
        (raw_phase ? !expected.index.has_value()
                   : expected.index.has_value() || expected.data.has_value()) ||
        (expected.data.has_value() && !expected.index.has_value()) ||
        (expected.index && all_zero_words(expected.index->identity)) ||
        (expected.data && all_zero_words(expected.data->identity))) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }

#if !defined(_WIN32)
    parent.require_stable();
    if (parent.identity() !=
        capture_directory_identity_locked(paths.private_directory.parent_path())) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
#endif
    lock.require_stable();
    auto generation = capture_private_lease_removal_generation_locked(
        paths, lock, expected.lease_id, expected.directory_identity, expected.owner_marker_identity,
        expected.owned_marker_identity);
    if (!generation.owned || generation.owned_pending || generation.reserved_pending ||
        generation.owned->record.capability !=
            PrivateLeaseCapability::RollbackPreactivePairAndLease ||
        generation.owned->identity != expected.owned_marker_identity ||
        generation.owned->record.owner_identity != expected.owner_marker_identity ||
        generation.owned->record.directory_identity != expected.directory_identity ||
        generation.owned->record.lease_id != expected.lease_id ||
        generation.reserved.has_value() != expected.reserved_marker_identity.has_value() ||
        (generation.reserved &&
         generation.reserved->identity != *expected.reserved_marker_identity)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    const bool final_present = generation.final_directory_identity.has_value();
    const bool staging_present = generation.staging_directory_identity.has_value();
    const bool directory_absent = !final_present && !staging_present;
    const bool expect_final =
        expected.phase == OOCPreactiveLeaseRecoveryPhaseV1::FinalDirectoryRawPair;
    const bool expect_staging =
        expected.phase == OOCPreactiveLeaseRecoveryPhaseV1::StagingDirectoryRawPair ||
        expected.phase == OOCPreactiveLeaseRecoveryPhaseV1::StagingDirectoryOwnerOnly ||
        expected.phase == OOCPreactiveLeaseRecoveryPhaseV1::StagingDirectoryOwnerRemoved;
    const bool expect_owner =
        expected.phase == OOCPreactiveLeaseRecoveryPhaseV1::FinalDirectoryRawPair ||
        expected.phase == OOCPreactiveLeaseRecoveryPhaseV1::StagingDirectoryRawPair ||
        expected.phase == OOCPreactiveLeaseRecoveryPhaseV1::StagingDirectoryOwnerOnly;
    if (final_present != expect_final || staging_present != expect_staging ||
        directory_absent != (!expect_final && !expect_staging) ||
        generation.owner_present != expect_owner ||
        (final_present && *generation.final_directory_identity != expected.directory_identity) ||
        (staging_present &&
         *generation.staging_directory_identity != expected.directory_identity)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    if (expect_final || expect_staging) {
        const auto active_directory = expect_final
                                          ? paths.private_directory
                                          : private_lease_staging_path(paths, expected.lease_id);
        const auto entries = inspect_private_lease_preactive_entries(active_directory, paths);
        if (entries.owner != expect_owner || entries.index != expected.index.has_value() ||
            entries.data != expected.data.has_value() || (entries.data && !entries.index)) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        require_recovery_file_expectation(active_directory / paths.index_path.filename(),
                                          expected.index);
        require_recovery_file_expectation(active_directory / paths.data_path.filename(),
                                          expected.data);
    } else {
        require_recovery_file_expectation(paths.index_path, std::nullopt);
        require_recovery_file_expectation(paths.data_path, std::nullopt);
    }
    lock.require_stable();
#if !defined(_WIN32)
    parent.require_stable();
#endif
}

struct BorrowedRecoveryHookContextV1 final {
    const OOCCleanupPaths* paths = nullptr;
    const BaseLock* lock = nullptr;
    const OOCPreactiveLeaseRecoveryExpectationV1* expectation = nullptr;
    OOCPrivateLeaseTestHooks user_hooks;
#if !defined(_WIN32)
    const OOCPrivateLeaseRecoveryParentHandleV1* parent = nullptr;
#endif
    bool expectation_checked = false;
    std::optional<OOCCleanupResult> expectation_failure;
};

void record_borrowed_recovery_expectation_failure(BorrowedRecoveryHookContextV1& context,
                                                  OOCCleanupStatus status,
                                                  std::error_code error = {}) noexcept {
    context.expectation_failure = OOCCleanupResult{
        .status = status,
        .stage = OOCCleanupStage::None,
        .native_error = error ? error : protocol_error(),
    };
}

[[nodiscard]] bool
validate_borrowed_recovery_expectation_noexcept(BorrowedRecoveryHookContextV1& context) noexcept {
    try {
        if (context.paths == nullptr || context.lock == nullptr || context.expectation == nullptr
#if !defined(_WIN32)
            || context.parent == nullptr
#endif
        ) {
            record_borrowed_recovery_expectation_failure(context, OOCCleanupStatus::InvalidRequest,
                                                         invalid_argument_error());
            return false;
        }
        require_preactive_recovery_expectation(*context.paths, *context.lock, *context.expectation
#if !defined(_WIN32)
                                               ,
                                               *context.parent
#endif
        );
        return true;
    } catch (const Failure& failure) {
        record_borrowed_recovery_expectation_failure(context, failure.status, failure.error);
    } catch (const std::filesystem::filesystem_error& error) {
        record_borrowed_recovery_expectation_failure(context, OOCCleanupStatus::IoFailure,
                                                     error.code());
    } catch (const std::system_error& error) {
        record_borrowed_recovery_expectation_failure(context, OOCCleanupStatus::IoFailure,
                                                     error.code());
    } catch (...) {
        record_borrowed_recovery_expectation_failure(context, OOCCleanupStatus::UnexpectedFailure);
    }
    return false;
}

[[nodiscard]] bool borrowed_recovery_stop_after(OOCPrivateLeaseFaultPoint point,
                                                void* opaque) noexcept {
    if (opaque == nullptr) {
        return true;
    }
    auto& context = *static_cast<BorrowedRecoveryHookContextV1*>(opaque);
    if (point == OOCPrivateLeaseFaultPoint::RecoveryPermitAcquired) {
        if (context.expectation_checked) {
            record_borrowed_recovery_expectation_failure(context, OOCCleanupStatus::InvalidRequest,
                                                         invalid_argument_error());
            return true;
        }
        context.expectation_checked = true;
        if (!validate_borrowed_recovery_expectation_noexcept(context)) {
            return true;
        }
        const bool interrupted = context.user_hooks.stop_after != nullptr &&
                                 context.user_hooks.stop_after(point, context.user_hooks.context);
        if (!validate_borrowed_recovery_expectation_noexcept(context)) {
            return true;
        }
        return interrupted;
    }
#if !defined(_WIN32)
    try {
        if (context.parent == nullptr) {
            record_borrowed_recovery_expectation_failure(context, OOCCleanupStatus::InvalidRequest,
                                                         invalid_argument_error());
            return true;
        }
        context.parent->require_stable();
    } catch (const Failure& failure) {
        record_borrowed_recovery_expectation_failure(context, failure.status, failure.error);
        return true;
    } catch (...) {
        record_borrowed_recovery_expectation_failure(context, OOCCleanupStatus::UnexpectedFailure);
        return true;
    }
#endif
    const bool interrupted = context.user_hooks.stop_after != nullptr &&
                             context.user_hooks.stop_after(point, context.user_hooks.context);
#if !defined(_WIN32)
    try {
        context.parent->require_stable();
    } catch (const Failure& failure) {
        record_borrowed_recovery_expectation_failure(context, failure.status, failure.error);
        return true;
    } catch (...) {
        record_borrowed_recovery_expectation_failure(context, OOCCleanupStatus::UnexpectedFailure);
        return true;
    }
#endif
    return interrupted;
}

} // namespace

std::shared_ptr<BaseLock>
OOCPrivateLeaseRecoveryBorrowedBaseLockV1::consume(const OOCCleanupPaths& paths,
                                                   int retained_parent_descriptor) {
#if defined(_WIN32)
    (void)paths;
    (void)retained_parent_descriptor;
    fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None,
         std::make_error_code(std::errc::operation_not_supported));
#else
    if (consumed_ || parent_descriptor_ < 0 || lock_descriptor_ < 0 ||
        retained_parent_descriptor < 0 || lock_leaf_.empty() || creator_process_id_ == 0 ||
        creator_process_id_ != static_cast<std::uint64_t>(gnfs::util::process_id())) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    consumed_ = true;
    const auto expected_leaf = paths.lock_path.filename().string();
    if (paths.private_directory.empty() || lock_leaf_ != expected_leaf) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }

    const auto valid_parent = [](const struct stat& metadata) noexcept {
        return S_ISDIR(metadata.st_mode) &&
               (metadata.st_mode & static_cast<mode_t>(07777)) == static_cast<mode_t>(0700) &&
               metadata.st_uid == ::geteuid();
    };
    const auto valid_lock = [](const struct stat& metadata) noexcept {
        return S_ISREG(metadata.st_mode) && metadata.st_nlink == 1 &&
               (metadata.st_mode & static_cast<mode_t>(07777)) == static_cast<mode_t>(0600) &&
               metadata.st_uid == ::geteuid();
    };
    struct stat source_parent{};
    struct stat retained_parent{};
    struct stat held_lock{};
    struct stat named_lock{};
    if (::fstat(parent_descriptor_, &source_parent) != 0 ||
        ::fstat(retained_parent_descriptor, &retained_parent) != 0 ||
        ::fstat(lock_descriptor_, &held_lock) != 0 ||
        ::fstatat(retained_parent_descriptor, expected_leaf.c_str(), &named_lock,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
             posix_error(errno == 0 ? EACCES : errno));
    }
    const auto source_parent_identity = stable_identity(posix_identity(source_parent));
    const auto retained_parent_identity = stable_identity(posix_identity(retained_parent));
    const auto held_lock_identity = stable_identity(posix_identity(held_lock));
    const auto named_lock_identity = stable_identity(posix_identity(named_lock));
    if (!valid_parent(source_parent) || !valid_parent(retained_parent) || !valid_lock(held_lock) ||
        !valid_lock(named_lock) || source_parent_identity != retained_parent_identity ||
        held_lock_identity != lock_identity_ || named_lock_identity != lock_identity_ ||
        held_lock.st_dev != named_lock.st_dev || held_lock.st_ino != named_lock.st_ino) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }

    int duplicated = -1;
    do {
        duplicated = ::fcntl(lock_descriptor_, F_DUPFD_CLOEXEC, 0);
    } while (duplicated < 0 && errno == EINTR);
    if (duplicated < 0) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
    }
    try {
        auto adopted = std::unique_ptr<BaseLock>(
            new BaseLock(paths.lock_path, duplicated, retained_parent_descriptor, expected_leaf,
                         retained_parent_identity, lock_identity_,
                         BaseLock::AdoptBorrowedLockedOpenFileDescription{}));
        duplicated = -1;
        return std::shared_ptr<BaseLock>(std::move(adopted));
    } catch (...) {
        if (duplicated >= 0) {
            (void)::close(duplicated);
        }
        throw;
    }
#endif
}

static_assert(static_cast<std::size_t>(PrivateCleanupMarkerSlot::Count) == 4);
static_assert(static_cast<std::size_t>(PrivateHandoffLeafSlot::Count) == 2);
static_assert(static_cast<std::uint8_t>(PrivateCleanupMarkerObservationKind::Count) == 7);
static_assert(static_cast<std::uint8_t>(PrivateHandoffLeafObservationKind::Count) == 5);
static_assert(static_cast<std::uint8_t>(PrivateCleanupUnionBlock::Count) == 6);
static_assert(static_cast<std::uint8_t>(PrivateNamespaceAction::Count) == 11);
static_assert(static_cast<std::uint8_t>(PrivateNamespaceActionDisposition::Count) == 5);

PrivateCleanupUnionClassification
classify_private_cleanup_union(const PrivateCleanupUnionRawObservation& observation) noexcept {
    PrivateCleanupUnionClassification result;
    bool marker_corrupt = false;
    bool foreign = observation.namespace_foreign;

    for (std::size_t slot = 0; slot < observation.cleanup_markers.size(); ++slot) {
        const auto marker = observation.cleanup_markers[slot];
        switch (marker) {
        case PrivateCleanupMarkerObservationKind::Missing:
            break;
        case PrivateCleanupMarkerObservationKind::LegacyV1:
            result.has_legacy_v1 = true;
            break;
        case PrivateCleanupMarkerObservationKind::LegacyPendingCandidate:
            if (slot != static_cast<std::size_t>(PrivateCleanupMarkerSlot::IntentPending) &&
                slot != static_cast<std::size_t>(PrivateCleanupMarkerSlot::StagedPending)) {
                foreign = true;
            }
            break;
        case PrivateCleanupMarkerObservationKind::AuthorizedV2:
            result.has_v2_record = true;
            break;
        case PrivateCleanupMarkerObservationKind::WrongRoleV2:
            result.has_v2_record = true;
            marker_corrupt = true;
            break;
        case PrivateCleanupMarkerObservationKind::Malformed:
            marker_corrupt = true;
            break;
        case PrivateCleanupMarkerObservationKind::Foreign:
        case PrivateCleanupMarkerObservationKind::Count:
            foreign = true;
            break;
        default:
            foreign = true;
            break;
        }
    }

    for (const auto handoff : observation.handoff_markers) {
        switch (handoff) {
        case PrivateHandoffLeafObservationKind::Missing:
            break;
        case PrivateHandoffLeafObservationKind::Exact:
            result.has_handoff = true;
            break;
        case PrivateHandoffLeafObservationKind::Unsupported:
            result.handoff_unsupported = true;
            break;
        case PrivateHandoffLeafObservationKind::Malformed:
        case PrivateHandoffLeafObservationKind::Foreign:
        case PrivateHandoffLeafObservationKind::Count:
            foreign = true;
            break;
        default:
            foreign = true;
            break;
        }
    }

    if (foreign) {
        result.block = PrivateCleanupUnionBlock::Foreign;
    } else if (marker_corrupt) {
        result.block = PrivateCleanupUnionBlock::MarkerCorrupt;
    } else if (result.has_v2_record) {
        result.block = PrivateCleanupUnionBlock::AuthorizedV2Present;
    } else if (result.handoff_unsupported) {
        result.block = PrivateCleanupUnionBlock::HandoffUnsupported;
    } else if (result.has_legacy_v1 && result.has_handoff) {
        result.block = PrivateCleanupUnionBlock::MixedLegacyAuthorities;
    }
    return result;
}

PrivateNamespaceActionDecision
decide_private_namespace_action(const PrivateCleanupUnionRawObservation& observation,
                                PrivateNamespaceAction action) noexcept {
    const auto classification = classify_private_cleanup_union(observation);
    switch (action) {
    case PrivateNamespaceAction::InspectHandoff:
    case PrivateNamespaceAction::AdoptHandoff:
    case PrivateNamespaceAction::RunLegacyCleanup:
    case PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff:
    case PrivateNamespaceAction::RecoverPrivateLease:
    case PrivateNamespaceAction::RemovePrivateLease:
    case PrivateNamespaceAction::ReservePrivateLease:
    case PrivateNamespaceAction::ConfirmPairReusable:
    case PrivateNamespaceAction::PublishPrivateHandoff:
    case PrivateNamespaceAction::ValidateFreshWriter:
    case PrivateNamespaceAction::ActivateFreshLease:
        break;
    case PrivateNamespaceAction::Count:
        return {
            .classification =
                {
                    .block = PrivateCleanupUnionBlock::Foreign,
                },
            .action = action,
            .disposition = PrivateNamespaceActionDisposition::RejectForeignPreserved,
        };
    default:
        return {
            .classification =
                {
                    .block = PrivateCleanupUnionBlock::Foreign,
                },
            .action = action,
            .disposition = PrivateNamespaceActionDisposition::RejectForeignPreserved,
        };
    }

    PrivateNamespaceActionDisposition disposition =
        PrivateNamespaceActionDisposition::RejectForeignPreserved;
    switch (classification.block) {
    case PrivateCleanupUnionBlock::None:
        disposition = PrivateNamespaceActionDisposition::DelegateExistingRuntime;
        break;
    case PrivateCleanupUnionBlock::Foreign:
        disposition = PrivateNamespaceActionDisposition::RejectForeignPreserved;
        break;
    case PrivateCleanupUnionBlock::MarkerCorrupt:
        disposition = PrivateNamespaceActionDisposition::RejectIntentCorrupt;
        break;
    case PrivateCleanupUnionBlock::AuthorizedV2Present:
    case PrivateCleanupUnionBlock::HandoffUnsupported:
        disposition = PrivateNamespaceActionDisposition::RejectPlatformUnsupported;
        break;
    case PrivateCleanupUnionBlock::MixedLegacyAuthorities:
        disposition = PrivateNamespaceActionDisposition::RejectNamespaceConflict;
        break;
    case PrivateCleanupUnionBlock::Count:
        disposition = PrivateNamespaceActionDisposition::RejectForeignPreserved;
        break;
    }
    return {
        .classification = classification,
        .action = action,
        .disposition = disposition,
    };
}

PrivateHandoffLeafObservationKind
observe_platform_limited_handoff_leaf(const std::filesystem::path& path) {
#ifdef _WIN32
    const HANDLE handle = ::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return PrivateHandoffLeafObservationKind::Missing;
        }
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(code));
    }

    BY_HANDLE_FILE_INFORMATION before{};
    if (!::GetFileInformationByHandle(handle, &before)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(code));
    }
    const auto before_identity = windows_identity(handle, before);
    if (!before_identity) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(code));
    }

    BY_HANDLE_FILE_INFORMATION after{};
    if (!::GetFileInformationByHandle(handle, &after)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(code));
    }
    const auto after_identity = windows_identity(handle, after);
    if (!after_identity) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(code));
    }
    if (!::CloseHandle(handle)) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(::GetLastError()));
    }
    if (!windows_regular_single_link(before) || !windows_regular_single_link(after) ||
        *before_identity != *after_identity) {
        return PrivateHandoffLeafObservationKind::Foreign;
    }
    return PrivateHandoffLeafObservationKind::Unsupported;
#else
    struct stat named{};
    int result = -1;
    do {
        result = ::lstat(path.c_str(), &named);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        const int saved_errno = errno;
        if (saved_errno == ENOENT) {
            return PrivateHandoffLeafObservationKind::Missing;
        }
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(saved_errno));
    }
    if (!posix_regular_single_link(named) ||
        static_cast<std::uint64_t>(named.st_uid) != static_cast<std::uint64_t>(::geteuid()) ||
        (named.st_mode & static_cast<mode_t>(07777)) != (S_IRUSR | S_IWUSR)) {
        return PrivateHandoffLeafObservationKind::Foreign;
    }

    const auto inspected = inspect_file(path, 0, false);
    switch (inspected.kind) {
    case InspectKind::Present:
        return inspected.identity == posix_identity(named)
                   ? PrivateHandoffLeafObservationKind::Unsupported
                   : PrivateHandoffLeafObservationKind::Foreign;
    case InspectKind::Missing:
    case InspectKind::Rejected:
        return PrivateHandoffLeafObservationKind::Foreign;
    case InspectKind::Error:
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    return PrivateHandoffLeafObservationKind::Foreign;
#endif
}

namespace {

[[nodiscard]] constexpr OOCAuthorizedCleanupMarkerKindV2
opposite_v2_marker_kind(OOCAuthorizedCleanupMarkerKindV2 expected) noexcept {
    return expected == OOCAuthorizedCleanupMarkerKindV2::intent
               ? OOCAuthorizedCleanupMarkerKindV2::staged
               : OOCAuthorizedCleanupMarkerKindV2::intent;
}

[[nodiscard]] constexpr std::uint64_t opposite_v1_marker_magic(std::uint64_t expected) noexcept {
    return expected == INTENT_MAGIC ? STAGED_MAGIC : INTENT_MAGIC;
}

[[nodiscard]] bool has_v2_magic_crash_prefix(std::span<const std::byte> bytes) noexcept {
    if (bytes.empty()) {
        return false;
    }
    const auto compared = (std::min)(bytes.size(), OOC_AUTHORIZED_CLEANUP_INTENT_MAGIC_V2.size());
    for (std::size_t index = 0; index < compared; ++index) {
        const auto expected = static_cast<std::byte>(
            static_cast<unsigned char>(OOC_AUTHORIZED_CLEANUP_INTENT_MAGIC_V2[index]));
        if (bytes[index] != expected) {
            return false;
        }
    }
    return true;
}

inline void reject_v2_decoder_infrastructure_failure(OOCAuthorizedCleanupIntentProtocolCode code) {
    switch (code) {
    case OOCAuthorizedCleanupIntentProtocolCode::resource_exhausted:
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None,
             std::make_error_code(std::errc::not_enough_memory));
    case OOCAuthorizedCleanupIntentProtocolCode::digest_unavailable:
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None,
             std::make_error_code(std::errc::operation_not_supported));
    default:
        return;
    }
}

[[nodiscard]] PrivateCleanupMarkerObservationKind
decode_cleanup_marker_inspection(const InspectResult& inspected, bool pending,
                                 std::uint64_t expected_v1_magic,
                                 OOCAuthorizedCleanupMarkerKindV2 expected_v2_kind) {
    switch (inspected.kind) {
    case InspectKind::Missing:
        return PrivateCleanupMarkerObservationKind::Missing;
    case InspectKind::Error:
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    case InspectKind::Rejected:
        return pending ? PrivateCleanupMarkerObservationKind::Foreign
                       : PrivateCleanupMarkerObservationKind::Malformed;
    case InspectKind::Present:
        break;
    }

    if (inspected.bytes.size() == MARKER_BYTES) {
        if (marker_is_valid(inspected.bytes, expected_v1_magic)) {
            return PrivateCleanupMarkerObservationKind::LegacyV1;
        }
        if (pending &&
            marker_is_valid(inspected.bytes, opposite_v1_marker_magic(expected_v1_magic))) {
            return PrivateCleanupMarkerObservationKind::Foreign;
        }
        if (!pending || has_v2_magic_crash_prefix(inspected.bytes)) {
            return PrivateCleanupMarkerObservationKind::Malformed;
        }
        return PrivateCleanupMarkerObservationKind::LegacyPendingCandidate;
    }

    if (inspected.bytes.size() == OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2) {
        const auto expected =
            decode_ooc_authorized_cleanup_intent(inspected.bytes, expected_v2_kind);
        if (expected) {
            return PrivateCleanupMarkerObservationKind::AuthorizedV2;
        }
        reject_v2_decoder_infrastructure_failure(expected.status.code);
        if (expected.status.code ==
            OOCAuthorizedCleanupIntentProtocolCode::unexpected_marker_kind) {
            const auto opposite = decode_ooc_authorized_cleanup_intent(
                inspected.bytes, opposite_v2_marker_kind(expected_v2_kind));
            if (opposite) {
                return PrivateCleanupMarkerObservationKind::WrongRoleV2;
            }
            reject_v2_decoder_infrastructure_failure(opposite.status.code);
        }
        // A V2-sized pending leaf exceeded the legacy pending bound before
        // this preflight existed, so an invalid record retains the established
        // foreign-replacement result. Canonical corruption remains
        // IntentCorrupt.
        return !pending || has_v2_magic_crash_prefix(inspected.bytes)
                   ? PrivateCleanupMarkerObservationKind::Malformed
                   : PrivateCleanupMarkerObservationKind::Foreign;
    }

    if (!pending) {
        return PrivateCleanupMarkerObservationKind::Malformed;
    }
    if (has_v2_magic_crash_prefix(inspected.bytes)) {
        return PrivateCleanupMarkerObservationKind::Malformed;
    }
    // A bounded V1 pending leaf is deliberately opaque here. The established
    // V1 code must compare it with the context-bound expected record before it
    // can decide whether to rewrite, reclaim, or preserve that exact inode.
    return inspected.bytes.size() <= MARKER_BYTES
               ? PrivateCleanupMarkerObservationKind::LegacyPendingCandidate
               : PrivateCleanupMarkerObservationKind::Foreign;
}

#if !defined(__APPLE__)
[[nodiscard]] PrivateCleanupMarkerObservationKind
decode_cleanup_marker_leaf(const std::filesystem::path& path, bool pending,
                           std::uint64_t expected_v1_magic,
                           OOCAuthorizedCleanupMarkerKindV2 expected_v2_kind) {
    return decode_cleanup_marker_inspection(
        inspect_pending_file(path, OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2), pending,
        expected_v1_magic, expected_v2_kind);
}
#endif

#if defined(__APPLE__)

enum class PrivateCleanupUnionDirectoryEntry : std::size_t {
    Owner,
    Index,
    Data,
    Handoff,
    HandoffPending,
    Intent,
    IntentPending,
    Staged,
    StagedPending,
    QuarantineIndex,
    QuarantineData,
    Count,
};

struct PrivateCleanupUnionLeafMetadata final {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t mode = 0;
    std::uint64_t owner = 0;
    std::uint64_t group = 0;
    std::uint64_t link_count = 0;
    std::int64_t size = 0;
    std::int64_t modified_seconds = 0;
    std::int64_t modified_nanoseconds = 0;
    std::int64_t changed_seconds = 0;
    std::int64_t changed_nanoseconds = 0;

    friend bool operator==(const PrivateCleanupUnionLeafMetadata&,
                           const PrivateCleanupUnionLeafMetadata&) = default;
};

struct PrivateCleanupUnionDirectoryInventory final {
    bool foreign = false;
    std::array<std::optional<PrivateCleanupUnionLeafMetadata>,
               static_cast<std::size_t>(PrivateCleanupUnionDirectoryEntry::Count)>
        leaves{};

    friend bool operator==(const PrivateCleanupUnionDirectoryInventory&,
                           const PrivateCleanupUnionDirectoryInventory&) = default;
};

[[nodiscard]] PrivateCleanupUnionLeafMetadata
private_cleanup_union_leaf_metadata(const struct stat& metadata) noexcept {
    const auto modified = metadata.st_mtimespec;
    const auto changed = metadata.st_ctimespec;
    return {
        .device = static_cast<std::uint64_t>(metadata.st_dev),
        .inode = static_cast<std::uint64_t>(metadata.st_ino),
        .mode = static_cast<std::uint64_t>(metadata.st_mode),
        .owner = static_cast<std::uint64_t>(metadata.st_uid),
        .group = static_cast<std::uint64_t>(metadata.st_gid),
        .link_count = static_cast<std::uint64_t>(metadata.st_nlink),
        .size = static_cast<std::int64_t>(metadata.st_size),
        .modified_seconds = static_cast<std::int64_t>(modified.tv_sec),
        .modified_nanoseconds = static_cast<std::int64_t>(modified.tv_nsec),
        .changed_seconds = static_cast<std::int64_t>(changed.tv_sec),
        .changed_nanoseconds = static_cast<std::int64_t>(changed.tv_nsec),
    };
}

[[nodiscard]] bool private_cleanup_union_leaf_has_file_policy(
    const PrivateCleanupUnionLeafMetadata& metadata) noexcept {
    return (metadata.mode & static_cast<std::uint64_t>(S_IFMT)) ==
               static_cast<std::uint64_t>(S_IFREG) &&
           metadata.link_count == 1 && metadata.owner == static_cast<std::uint64_t>(::geteuid()) &&
           (metadata.mode & static_cast<std::uint64_t>(07777)) ==
               static_cast<std::uint64_t>(S_IRUSR | S_IWUSR) &&
           metadata.size >= 0;
}

[[nodiscard]] std::array<std::filesystem::path,
                         static_cast<std::size_t>(PrivateCleanupUnionDirectoryEntry::Count)>
private_cleanup_union_allowed_leaves(const OOCCleanupPaths& paths) {
    return {
        std::filesystem::path(".gnfs-private-lease-v1.owner"),
        paths.index_path.filename(),
        paths.data_path.filename(),
        paths.private_handoff_path.filename(),
        paths.private_handoff_pending_path.filename(),
        paths.intent_path.filename(),
        paths.intent_pending_path.filename(),
        paths.staged_path.filename(),
        paths.staged_pending_path.filename(),
        paths.quarantine_index_path.filename(),
        paths.quarantine_data_path.filename(),
    };
}

[[nodiscard]] PrivateCleanupUnionDirectoryInventory
scan_private_cleanup_union_directory(const OOCCleanupPaths& paths,
                                     const PrivateDirectoryHandle& directory) {
    directory.require_stable();
    const int held_descriptor = static_cast<int>(directory.native_handle());
    int scan_descriptor = -1;
    do {
        scan_descriptor =
            ::openat(held_descriptor, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (scan_descriptor < 0 && errno == EINTR);
    if (scan_descriptor < 0) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
    }

    struct stat scan_metadata{};
    if (::fstat(scan_descriptor, &scan_metadata) != 0 ||
        stable_identity(posix_identity(scan_metadata)) != directory.identity()) {
        const int saved_errno = errno == 0 ? EACCES : errno;
        (void)::close(scan_descriptor);
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, posix_error(saved_errno));
    }

    DIR* raw_stream = ::fdopendir(scan_descriptor);
    if (raw_stream == nullptr) {
        const int saved_errno = errno;
        (void)::close(scan_descriptor);
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(saved_errno));
    }
    std::unique_ptr<DIR, decltype(&::closedir)> stream(raw_stream, &::closedir);
    const auto allowed = private_cleanup_union_allowed_leaves(paths);

    PrivateCleanupUnionDirectoryInventory inventory;
    errno = 0;
    while (const auto* entry = ::readdir(stream.get())) {
        const std::filesystem::path leaf(entry->d_name);
        if (leaf == std::filesystem::path(".") || leaf == std::filesystem::path("..")) {
            errno = 0;
            continue;
        }

        std::size_t slot = allowed.size();
        for (std::size_t index = 0; index < allowed.size(); ++index) {
            if (path_leaf_equals(leaf, allowed[index])) {
                slot = index;
                break;
            }
        }
        if (slot == allowed.size() || leaf.native() != allowed[slot].native() ||
            inventory.leaves[slot]) {
            inventory.foreign = true;
            errno = 0;
            continue;
        }

        struct stat metadata{};
        int inspected = -1;
        do {
            inspected = ::fstatat(held_descriptor, leaf.c_str(), &metadata, AT_SYMLINK_NOFOLLOW);
        } while (inspected != 0 && errno == EINTR);
        if (inspected != 0) {
            if (errno == ENOENT) {
                inventory.foreign = true;
                errno = 0;
                continue;
            }
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
        }
        inventory.leaves[slot] = private_cleanup_union_leaf_metadata(metadata);
        errno = 0;
    }
    if (errno != 0) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
    }

    struct stat final_metadata{};
    if (::fstat(::dirfd(stream.get()), &final_metadata) != 0 ||
        stable_identity(posix_identity(final_metadata)) != directory.identity()) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
             errno == 0 ? protocol_error() : posix_error(errno));
    }
    DIR* close_stream = stream.release();
    if (::closedir(close_stream) != 0) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
    }
    directory.require_stable();
    return inventory;
}

struct PrivateCleanupLeafWitness final {
    InspectResult inspection;
    std::optional<util::durable_immutable_record::RecordSnapshot> snapshot;
};

[[nodiscard]] PrivateCleanupLeafWitness
inspect_relative_cleanup_leaf(const PrivateDirectoryHandle& directory,
                              const std::filesystem::path& leaf, std::size_t maximum_bytes) {
    using namespace util::durable_immutable_record;
    const auto read = read_bounded_at(directory.native_handle(), leaf, 0, maximum_bytes);
    switch (read.state()) {
    case BoundedReadState::missing:
        return {
            .inspection =
                {
                    .kind = InspectKind::Missing,
                    .identity = {},
                    .bytes = {},
                    .error = {},
                },
        };
    case BoundedReadState::exact:
        if (!read.bytes() || !read.snapshot()) {
            fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
        }
        return {
            .inspection =
                {
                    .kind = InspectKind::Present,
                    .identity =
                        {
                            .first = read.snapshot()->identity.first,
                            .second = read.snapshot()->identity.second,
                            .third = read.snapshot()->identity.third,
                            .size = read.snapshot()->size,
                        },
                    .bytes = *read.bytes(),
                    .error = {},
                },
            .snapshot = *read.snapshot(),
        };
    case BoundedReadState::rejected:
        return {
            .inspection =
                {
                    .kind = InspectKind::Rejected,
                    .identity = {},
                    .bytes = {},
                    .error = read.native_error(),
                },
        };
    case BoundedReadState::interrupted:
    case BoundedReadState::failed:
        return {
            .inspection =
                {
                    .kind = InspectKind::Error,
                    .identity = {},
                    .bytes = {},
                    .error = read.native_error(),
                },
        };
    case BoundedReadState::unsupported:
        fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None, read.native_error());
    }
    fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
}

[[nodiscard]] bool private_cleanup_union_inspection_matches_inventory(
    const InspectResult& inspection,
    const std::optional<PrivateCleanupUnionLeafMetadata>& metadata) noexcept {
    switch (inspection.kind) {
    case InspectKind::Missing:
        return !metadata;
    case InspectKind::Present:
        return metadata && metadata->device == inspection.identity.first &&
               metadata->inode == inspection.identity.second && metadata->size >= 0 &&
               static_cast<std::uint64_t>(metadata->size) == inspection.identity.size;
    case InspectKind::Rejected:
        return metadata.has_value();
    case InspectKind::Error:
        return false;
    }
    return false;
}

[[nodiscard]] PrivateCleanupMarkerObservationKind decode_relative_cleanup_marker_leaf(
    const PrivateDirectoryHandle& directory, const std::filesystem::path& leaf, bool pending,
    std::uint64_t expected_v1_magic, OOCAuthorizedCleanupMarkerKindV2 expected_v2_kind,
    const std::optional<PrivateCleanupUnionLeafMetadata>& inventory_metadata,
    bool& inventory_mismatch, std::optional<PrivateCleanupLeafWitness>& retained) {
    auto inspected =
        inspect_relative_cleanup_leaf(directory, leaf, OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2);
    inventory_mismatch = inventory_mismatch || !private_cleanup_union_inspection_matches_inventory(
                                                   inspected.inspection, inventory_metadata);
    const auto decoded = decode_cleanup_marker_inspection(inspected.inspection, pending,
                                                          expected_v1_magic, expected_v2_kind);
    retained = std::move(inspected);
    return decoded;
}

[[nodiscard]] bool private_handoff_leaf_matches_inventory(
    const LoadedPrivateHandoffLeaf& leaf,
    const std::optional<PrivateCleanupUnionLeafMetadata>& metadata) noexcept {
    switch (leaf.state) {
    case PrivateHandoffLeafState::Missing:
        return !metadata;
    case PrivateHandoffLeafState::Exact:
        return metadata && leaf.snapshot && metadata->device == leaf.snapshot->identity.first &&
               metadata->inode == leaf.snapshot->identity.second && metadata->size >= 0 &&
               static_cast<std::uint64_t>(metadata->size) == leaf.snapshot->size;
    case PrivateHandoffLeafState::Rejected:
        return metadata.has_value();
    }
    return false;
}

[[nodiscard]] PrivateHandoffLeafObservationKind
project_tainted_handoff_leaf(const PrivateHandoffLeafClassification& classified) noexcept {
    return classified.inspection.result.status == OOCCleanupStatus::ForeignReplacementPreserved
               ? PrivateHandoffLeafObservationKind::Foreign
               : PrivateHandoffLeafObservationKind::Malformed;
}

enum class StrictHandoffObservationBlocker : std::uint8_t {
    None,
    Unsupported,
    Foreign,
};

[[nodiscard]] StrictHandoffObservationBlocker
strict_handoff_observation_blocker(const PrivateCleanupUnionRawObservation& raw) noexcept {
    if (raw.namespace_foreign) {
        return StrictHandoffObservationBlocker::Foreign;
    }
    StrictHandoffObservationBlocker blocker = StrictHandoffObservationBlocker::None;
    for (const auto marker : raw.handoff_markers) {
        switch (marker) {
        case PrivateHandoffLeafObservationKind::Missing:
        case PrivateHandoffLeafObservationKind::Exact:
            break;
        case PrivateHandoffLeafObservationKind::Unsupported:
            if (blocker == StrictHandoffObservationBlocker::None) {
                blocker = StrictHandoffObservationBlocker::Unsupported;
            }
            break;
        case PrivateHandoffLeafObservationKind::Malformed:
        case PrivateHandoffLeafObservationKind::Foreign:
        case PrivateHandoffLeafObservationKind::Count:
            return StrictHandoffObservationBlocker::Foreign;
        }
    }
    return blocker;
}

[[nodiscard]] PrivateHandoffLeafObservationKind
strict_handoff_blocker_fallback(StrictHandoffObservationBlocker blocker) noexcept {
    return blocker == StrictHandoffObservationBlocker::Foreign
               ? PrivateHandoffLeafObservationKind::Foreign
               : PrivateHandoffLeafObservationKind::Unsupported;
}

[[nodiscard]] std::optional<PrivateHandoffLeafClassification>
project_strict_handoff_leaves(const OOCCleanupPaths& paths, const BaseLock& lock,
                              const std::array<std::uint64_t, 3>& directory_identity,
                              const std::optional<LoadedPrivateHandoffLeaf>& canonical,
                              const std::optional<LoadedPrivateHandoffLeaf>& pending,
                              PrivateCleanupUnionRawObservation& raw,
                              std::optional<LoadedPrivateLeaseMarker>& reserved_witness,
                              std::optional<LoadedPrivateLeaseMarker>& owned_witness) {
    const auto canonical_slot = static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical);
    const auto pending_slot = static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending);
    if (!canonical && !pending) {
        return std::nullopt;
    }

    const LoadedPrivateHandoffLeaf missing{
        .state = PrivateHandoffLeafState::Missing,
    };
    const auto& canonical_leaf = canonical ? *canonical : missing;
    const auto& pending_leaf = pending ? *pending : missing;
    if (canonical_leaf.state == PrivateHandoffLeafState::Exact &&
        pending_leaf.state == PrivateHandoffLeafState::Exact &&
        canonical_leaf.bytes != pending_leaf.bytes) {
        raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Malformed;
    }
    const auto existing_blocker = strict_handoff_observation_blocker(raw);
    std::optional<PrivateHandoffLeafClassification> classified;
    try {
        classified =
            classify_private_handoff_leaves_locked(paths, lock, directory_identity, canonical_leaf,
                                                   pending_leaf, &reserved_witness, &owned_witness);
    } catch (const Failure&) {
        if (existing_blocker == StrictHandoffObservationBlocker::None) {
            throw;
        }
        const auto fallback = strict_handoff_blocker_fallback(existing_blocker);
        if (canonical_leaf.state == PrivateHandoffLeafState::Exact &&
            raw.handoff_markers[canonical_slot] == PrivateHandoffLeafObservationKind::Missing) {
            raw.handoff_markers[canonical_slot] = fallback;
        }
        if (pending_leaf.state == PrivateHandoffLeafState::Exact &&
            raw.handoff_markers[pending_slot] == PrivateHandoffLeafObservationKind::Missing) {
            raw.handoff_markers[pending_slot] = fallback;
        }
        return std::nullopt;
    }
    switch (classified->inspection.state) {
    case OOCPrivateHandoffState::None:
        if (canonical_leaf.state == PrivateHandoffLeafState::Exact) {
            raw.handoff_markers[canonical_slot] = PrivateHandoffLeafObservationKind::Foreign;
        }
        if (pending_leaf.state == PrivateHandoffLeafState::Exact) {
            raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Foreign;
        }
        return classified;
    case OOCPrivateHandoffState::Canonical:
        if (canonical_leaf.state != PrivateHandoffLeafState::Exact) {
            raw.handoff_markers[canonical_slot] = PrivateHandoffLeafObservationKind::Foreign;
            if (pending_leaf.state == PrivateHandoffLeafState::Exact) {
                raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Foreign;
            }
            return classified;
        }
        raw.handoff_markers[canonical_slot] = PrivateHandoffLeafObservationKind::Exact;
        if (pending_leaf.state == PrivateHandoffLeafState::Exact) {
            raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Exact;
        }
        return classified;
    case OOCPrivateHandoffState::PendingOnly:
        if (pending_leaf.state != PrivateHandoffLeafState::Exact) {
            raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Foreign;
            if (canonical_leaf.state == PrivateHandoffLeafState::Exact) {
                raw.handoff_markers[canonical_slot] = PrivateHandoffLeafObservationKind::Foreign;
            }
            return classified;
        }
        raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Exact;
        return classified;
    case OOCPrivateHandoffState::TaintedPreserved:
        break;
    }

    const bool canonical_exact = canonical_leaf.state == PrivateHandoffLeafState::Exact;
    const bool pending_exact = pending_leaf.state == PrivateHandoffLeafState::Exact;
    if (canonical_exact && pending_exact) {
        if (classified->canonical_context_valid) {
            raw.handoff_markers[canonical_slot] = PrivateHandoffLeafObservationKind::Exact;
            raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Malformed;
            return classified;
        }
        raw.handoff_markers[canonical_slot] = project_tainted_handoff_leaf(*classified);
        raw.handoff_markers[pending_slot] = canonical_leaf.bytes == pending_leaf.bytes
                                                ? project_tainted_handoff_leaf(*classified)
                                                : PrivateHandoffLeafObservationKind::Malformed;
        try {
            const auto pending_only = classify_private_handoff_leaves_locked(
                paths, lock, directory_identity, missing, pending_leaf);
            if (pending_only.inspection.state == OOCPrivateHandoffState::PendingOnly &&
                pending_only.pending_is_preactive) {
                raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Exact;
            }
        } catch (const Failure&) {
            // The aggregate is already terminal. Companion refinement is
            // diagnostic only and may not replace the established blocker.
        }
        return classified;
    }
    if (canonical_exact) {
        raw.handoff_markers[canonical_slot] = project_tainted_handoff_leaf(*classified);
    }
    if (pending_exact) {
        raw.handoff_markers[pending_slot] = project_tainted_handoff_leaf(*classified);
    }
    return classified;
}

#endif

struct PrivateCleanupUnionObservationWitness final {
    PrivateCleanupUnionRawObservation raw;
#if defined(__APPLE__)
    std::unique_ptr<PrivateDirectoryHandle> directory;
    std::optional<PrivateCleanupUnionDirectoryInventory> before_inventory;
    std::optional<PrivateCleanupUnionDirectoryInventory> after_inventory;
    std::array<std::optional<PrivateCleanupLeafWitness>,
               static_cast<std::size_t>(PrivateCleanupMarkerSlot::Count)>
        cleanup_leaves;
    std::optional<LoadedPrivateHandoffLeaf> canonical_handoff;
    std::optional<LoadedPrivateHandoffLeaf> pending_handoff;
    std::optional<PrivateHandoffLeafClassification> handoff_classification;
    std::optional<LoadedPrivateLeaseMarker> reserved_marker;
    std::optional<LoadedPrivateLeaseMarker> owned_marker;
#endif
};

[[nodiscard]] PrivateCleanupUnionObservationWitness
observe_private_cleanup_union_locked(const OOCCleanupPaths& paths, const BaseLock& lock,
                                     PrivateCleanupUnionObservationTestHooks hooks = {}) {
    if (!lock.matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
#if !defined(__APPLE__)
    if (hooks.observe != nullptr) {
        fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None,
             std::make_error_code(std::errc::operation_not_supported));
    }
#endif
    lock.require_stable();

    auto observation = invoke_with_stable_base_lock(lock, [&] {
        PrivateCleanupUnionObservationWitness witness;
        auto& raw = witness.raw;
        if (!paths.private_handoff_rollback_path.empty()) {
            const auto rollback = inspect_file(paths.private_handoff_rollback_path, 0, false);
            if (rollback.kind == InspectKind::Error) {
                fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, rollback.error);
            }
            if (rollback.kind != InspectKind::Missing) {
                // Only the typed resume state machine may consume the durable
                // rollback capability. Every legacy/adoption/lease action
                // treats even an exact tombstone as a closed namespace.
                raw.namespace_foreign = true;
                return witness;
            }
        }
#if defined(__APPLE__)
        const auto directory_identity = inspect_directory_identity_locked(paths.private_directory);
        if (!directory_identity) {
            return witness;
        }

        witness.directory = std::make_unique<PrivateDirectoryHandle>(paths.private_directory);
        auto& directory = *witness.directory;
        if (directory.identity() != *directory_identity) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        // Enumerating names and no-follow metadata grants no record authority.
        // Do it before the private-mode check so a concrete foreign leaf keeps
        // the established ForeignReplacementPreserved precedence. No leaf
        // bytes are read until the directory policy has passed.
        witness.before_inventory = scan_private_cleanup_union_directory(paths, directory);
        const auto& before = *witness.before_inventory;
        if (before.foreign) {
            raw.namespace_foreign = true;
            return witness;
        }
        directory.require_private_policy();
        if (hooks.observe != nullptr) {
            hooks.observe(PrivateCleanupUnionObservationPoint::InitialInventoryComplete,
                          hooks.context);
        }
        const auto metadata = [&](PrivateCleanupUnionDirectoryEntry entry)
            -> const std::optional<PrivateCleanupUnionLeafMetadata>& {
            return before.leaves[static_cast<std::size_t>(entry)];
        };
        bool inventory_mismatch = false;
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::Intent)] =
            decode_relative_cleanup_marker_leaf(
                directory, paths.intent_path.filename(), false, INTENT_MAGIC,
                OOCAuthorizedCleanupMarkerKindV2::intent,
                metadata(PrivateCleanupUnionDirectoryEntry::Intent), inventory_mismatch,
                witness.cleanup_leaves[static_cast<std::size_t>(PrivateCleanupMarkerSlot::Intent)]);
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::IntentPending)] =
            decode_relative_cleanup_marker_leaf(
                directory, paths.intent_pending_path.filename(), true, INTENT_MAGIC,
                OOCAuthorizedCleanupMarkerKindV2::intent,
                metadata(PrivateCleanupUnionDirectoryEntry::IntentPending), inventory_mismatch,
                witness.cleanup_leaves[static_cast<std::size_t>(
                    PrivateCleanupMarkerSlot::IntentPending)]);
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::Staged)] =
            decode_relative_cleanup_marker_leaf(
                directory, paths.staged_path.filename(), false, STAGED_MAGIC,
                OOCAuthorizedCleanupMarkerKindV2::staged,
                metadata(PrivateCleanupUnionDirectoryEntry::Staged), inventory_mismatch,
                witness.cleanup_leaves[static_cast<std::size_t>(PrivateCleanupMarkerSlot::Staged)]);
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::StagedPending)] =
            decode_relative_cleanup_marker_leaf(
                directory, paths.staged_pending_path.filename(), true, STAGED_MAGIC,
                OOCAuthorizedCleanupMarkerKindV2::staged,
                metadata(PrivateCleanupUnionDirectoryEntry::StagedPending), inventory_mismatch,
                witness.cleanup_leaves[static_cast<std::size_t>(
                    PrivateCleanupMarkerSlot::StagedPending)]);
        raw.namespace_foreign = inventory_mismatch;

        const auto handoff_metadata = metadata(PrivateCleanupUnionDirectoryEntry::Handoff);
        const auto handoff_pending_metadata =
            metadata(PrivateCleanupUnionDirectoryEntry::HandoffPending);
        const auto canonical_slot = static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical);
        const auto pending_slot = static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending);
        if (handoff_metadata && !private_cleanup_union_leaf_has_file_policy(*handoff_metadata)) {
            raw.handoff_markers[canonical_slot] = PrivateHandoffLeafObservationKind::Foreign;
        }
        if (handoff_pending_metadata &&
            !private_cleanup_union_leaf_has_file_policy(*handoff_pending_metadata)) {
            raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Foreign;
        }
        const auto read_handoff_leaf =
            [&](const std::optional<PrivateCleanupUnionLeafMetadata>& leaf_metadata,
                const std::filesystem::path& leaf,
                PrivateHandoffLeafSlot slot) -> std::optional<LoadedPrivateHandoffLeaf> {
            const auto raw_slot = static_cast<std::size_t>(slot);
            if (raw.handoff_markers[raw_slot] == PrivateHandoffLeafObservationKind::Foreign) {
                return std::nullopt;
            }
            try {
                auto loaded = read_private_handoff_leaf(directory.native_handle(), leaf);
                const bool matches_inventory =
                    private_handoff_leaf_matches_inventory(loaded, leaf_metadata);
                inventory_mismatch = inventory_mismatch || !matches_inventory;
                raw.namespace_foreign = raw.namespace_foreign || !matches_inventory;
                if (loaded.state == PrivateHandoffLeafState::Rejected) {
                    raw.handoff_markers[raw_slot] = PrivateHandoffLeafObservationKind::Foreign;
                    return std::nullopt;
                }
                return loaded;
            } catch (const Failure& failure) {
                if (failure.status == OOCCleanupStatus::PlatformUnsupported) {
                    raw.handoff_markers[raw_slot] =
                        leaf_metadata ? PrivateHandoffLeafObservationKind::Unsupported
                                      : PrivateHandoffLeafObservationKind::Missing;
                    return std::nullopt;
                }
                const auto blocker = strict_handoff_observation_blocker(raw);
                if (blocker != StrictHandoffObservationBlocker::None) {
                    raw.handoff_markers[raw_slot] = strict_handoff_blocker_fallback(blocker);
                    return std::nullopt;
                }
                throw;
            }
        };
        witness.canonical_handoff =
            read_handoff_leaf(handoff_metadata, paths.private_handoff_path.filename(),
                              PrivateHandoffLeafSlot::Canonical);
        witness.pending_handoff = read_handoff_leaf(handoff_pending_metadata,
                                                    paths.private_handoff_pending_path.filename(),
                                                    PrivateHandoffLeafSlot::Pending);
        witness.handoff_classification = project_strict_handoff_leaves(
            paths, lock, directory.identity(), witness.canonical_handoff, witness.pending_handoff,
            raw, witness.reserved_marker, witness.owned_marker);
        if (hooks.observe != nullptr) {
            hooks.observe(PrivateCleanupUnionObservationPoint::LeafReadsComplete, hooks.context);
        }
        witness.after_inventory = scan_private_cleanup_union_directory(paths, directory);
        const auto& after = *witness.after_inventory;
        raw.namespace_foreign =
            raw.namespace_foreign || inventory_mismatch || after.foreign || before != after;
        directory.require_stable();
        directory.require_private_policy();
        return witness;
#else
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::Intent)] =
            decode_cleanup_marker_leaf(paths.intent_path, false, INTENT_MAGIC,
                                       OOCAuthorizedCleanupMarkerKindV2::intent);
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::IntentPending)] =
            decode_cleanup_marker_leaf(paths.intent_pending_path, true, INTENT_MAGIC,
                                       OOCAuthorizedCleanupMarkerKindV2::intent);
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::Staged)] =
            decode_cleanup_marker_leaf(paths.staged_path, false, STAGED_MAGIC,
                                       OOCAuthorizedCleanupMarkerKindV2::staged);
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::StagedPending)] =
            decode_cleanup_marker_leaf(paths.staged_pending_path, true, STAGED_MAGIC,
                                       OOCAuthorizedCleanupMarkerKindV2::staged);
        if (inspect_directory_identity_locked(paths.private_directory)) {
            const auto entries = inspect_private_handoff_directory_entries(paths);
            raw.namespace_foreign = !entries.valid;
        }
        if (raw.namespace_foreign) {
            return witness;
        }
        raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical)] =
            observe_platform_limited_handoff_leaf(paths.private_handoff_path);
        raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending)] =
            observe_platform_limited_handoff_leaf(paths.private_handoff_pending_path);
        return witness;
#endif
    });
    lock.require_stable();
    return observation;
}

[[nodiscard]] std::optional<OOCCleanupResult>
private_cleanup_union_blocked_result(const PrivateNamespaceActionDecision& decision) {
    switch (decision.disposition) {
    case PrivateNamespaceActionDisposition::DelegateExistingRuntime:
        return std::nullopt;
    case PrivateNamespaceActionDisposition::RejectForeignPreserved:
        return OOCCleanupResult{
            .status = OOCCleanupStatus::ForeignReplacementPreserved,
            .stage = OOCCleanupStage::None,
            .native_error = protocol_error(),
        };
    case PrivateNamespaceActionDisposition::RejectIntentCorrupt:
        return OOCCleanupResult{
            .status = OOCCleanupStatus::IntentCorrupt,
            .stage = OOCCleanupStage::None,
            .native_error = protocol_error(),
        };
    case PrivateNamespaceActionDisposition::RejectPlatformUnsupported:
        return OOCCleanupResult{
            .status = OOCCleanupStatus::PlatformUnsupported,
            .stage = OOCCleanupStage::None,
            .native_error = std::make_error_code(std::errc::operation_not_supported),
        };
    case PrivateNamespaceActionDisposition::RejectNamespaceConflict:
        return OOCCleanupResult{
            .status = OOCCleanupStatus::NamespaceConflict,
            .stage = OOCCleanupStage::None,
            .native_error = protocol_error(),
        };
    case PrivateNamespaceActionDisposition::Count:
        break;
    }
    return OOCCleanupResult{
        .status = OOCCleanupStatus::ForeignReplacementPreserved,
        .stage = OOCCleanupStage::None,
        .native_error = protocol_error(),
    };
}

[[nodiscard]] std::optional<OOCCleanupResult>
project_private_cleanup_union_preflight(const OOCCleanupPaths& paths, const BaseLock& lock,
                                        PrivateNamespaceAction action) {
    if (paths.private_directory.empty()) {
        return std::nullopt;
    }
    const auto observation = observe_private_cleanup_union_locked(paths, lock);
    const auto decision = decide_private_namespace_action(observation.raw, action);
    lock.require_stable();
    return private_cleanup_union_blocked_result(decision);
}

} // namespace

enum class PrivateHandoffConsumptionState : std::uint8_t {
    Fresh,
    LegacyObserved,
    LegacyMutationAuthorized,
    PublicationObserved,
    PublicationMutationAuthorized,
    LifecycleObserved,
    RecoverConsumed,
    RemoveConsumed,
    Failed,
};

bool try_claim_private_cleanup_action(BaseLock& lock) noexcept {
    bool expected = false;
    return lock.private_cleanup_action_claimed_.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
}

void release_private_cleanup_action(BaseLock& lock) noexcept {
    lock.private_cleanup_action_claimed_.store(false, std::memory_order_release);
}

struct PrivateCleanupActionPermit::State final {
    State(OOCCleanupPaths frozen_paths, std::shared_ptr<BaseLock> retained_lock,
          PrivateNamespaceActionDecision retained_decision,
          PrivateCleanupUnionObservationWitness retained_witness)
        : paths(std::move(frozen_paths)), lock(std::move(retained_lock)),
          decision(retained_decision),
          creator_process_id(static_cast<std::uint64_t>(gnfs::util::process_id())),
          witness(std::move(retained_witness)) {}

    ~State() {
        if (lock) {
            release_private_cleanup_action(*lock);
        }
    }

    OOCCleanupPaths paths;
    std::shared_ptr<BaseLock> lock;
    PrivateNamespaceActionDecision decision;
    std::uint64_t creator_process_id = 0;
    PrivateCleanupUnionObservationWitness witness;
    std::optional<PrivateCleanupUnionObservationWitness> publication_phase_witness;
    std::optional<PrivateLeaseRemovalGenerationProof> removal_generation;
    std::optional<PrivateLeaseRemovalGenerationProof> publication_generation;
    std::optional<IntentRecord> publication_intent;
    std::optional<FileIdentity> publication_original_canonical_identity;
    std::optional<PrivateCleanupUnionObservationWitness> lifecycle_phase_witness;
    PrivateLeaseReservationPermitPhase reservation_phase =
        PrivateLeaseReservationPermitPhase::Fresh;
    std::optional<PrivateLeaseRecord> reservation_reserved;
    std::optional<PrivateLeaseRecord> reservation_owner;
    std::optional<PrivateLeaseRecord> reservation_owned;
    std::optional<std::array<std::uint64_t, 3>> reservation_parent_identity;
    std::optional<std::array<std::uint64_t, 3>> reservation_reserved_pending_identity;
    std::optional<std::array<std::uint64_t, 3>> reservation_reserved_identity;
    std::optional<std::array<std::uint64_t, 3>> reservation_owner_pending_identity;
    std::optional<std::array<std::uint64_t, 3>> reservation_owner_identity;
    std::optional<std::array<std::uint64_t, 3>> reservation_owned_pending_identity;
    std::optional<std::array<std::uint64_t, 3>> reservation_owned_identity;
    std::optional<std::array<std::uint64_t, 3>> reservation_directory_identity;
    std::filesystem::path reservation_staging_path;
#ifndef _WIN32
    std::unique_ptr<PrivateDirectoryHandle> reservation_directory;
#endif
    PrivateFreshWriterPermitPhase fresh_writer_phase = PrivateFreshWriterPermitPhase::Fresh;
    std::optional<PrivateLeaseRemovalGenerationProof> fresh_writer_generation;
    std::optional<std::array<std::uint64_t, 3>> fresh_index_identity;
    std::optional<std::array<std::uint64_t, 3>> fresh_data_identity;
    std::optional<IntentRecord> fresh_pair;
    std::uint64_t fresh_store_id = 0;
    PrivateLeaseActivationPermitPhase activation_phase = PrivateLeaseActivationPermitPhase::Fresh;
    std::optional<PrivateLeaseRemovalGenerationProof> activation_generation;
    std::optional<OwnershipProof> activation_pair_ownership;
    std::optional<IntentRecord> activation_pair;
    bool removal_generation_binding_attempted = false;
    bool action_started = false;
    PrivateHandoffConsumptionState handoff_state = PrivateHandoffConsumptionState::Fresh;
};

PrivateCleanupActionPermit::PrivateCleanupActionPermit(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

PrivateCleanupActionPermit::PrivateCleanupActionPermit(PrivateCleanupActionPermit&& other) noexcept
    : state_(std::move(other.state_)) {}

PrivateCleanupActionPermit::~PrivateCleanupActionPermit() = default;

static_assert(!std::is_default_constructible_v<PrivateCleanupActionPermit>);
static_assert(!std::is_copy_constructible_v<PrivateCleanupActionPermit>);
static_assert(!std::is_copy_assignable_v<PrivateCleanupActionPermit>);
static_assert(std::is_nothrow_move_constructible_v<PrivateCleanupActionPermit>);
static_assert(!std::is_move_assignable_v<PrivateCleanupActionPermit>);

namespace {

class PrivateCleanupActionClaimGuard final {
public:
    explicit PrivateCleanupActionClaimGuard(BaseLock& lock) noexcept
        : lock_(&lock), acquired_(try_claim_private_cleanup_action(lock)) {}

    PrivateCleanupActionClaimGuard(const PrivateCleanupActionClaimGuard&) = delete;
    PrivateCleanupActionClaimGuard& operator=(const PrivateCleanupActionClaimGuard&) = delete;

    ~PrivateCleanupActionClaimGuard() {
        if (acquired_ && lock_ != nullptr) {
            release_private_cleanup_action(*lock_);
        }
    }

    [[nodiscard]] bool acquired() const noexcept {
        return acquired_;
    }

    void transfer_to_permit() noexcept {
        acquired_ = false;
        lock_ = nullptr;
    }

private:
    BaseLock* lock_ = nullptr;
    bool acquired_ = false;
};

[[nodiscard]] OOCCleanupResult private_cleanup_action_busy() noexcept {
    return OOCCleanupResult{
        .status = OOCCleanupStatus::Busy,
        .stage = OOCCleanupStage::None,
        .native_error = std::make_error_code(std::errc::device_or_resource_busy),
    };
}

[[nodiscard]] bool private_cleanup_paths_equal(const OOCCleanupPaths& lhs,
                                               const OOCCleanupPaths& rhs) noexcept {
    return lhs.base_path == rhs.base_path && lhs.private_directory == rhs.private_directory &&
           lhs.index_path == rhs.index_path && lhs.data_path == rhs.data_path &&
           lhs.intent_path == rhs.intent_path &&
           lhs.intent_pending_path == rhs.intent_pending_path &&
           lhs.staged_path == rhs.staged_path &&
           lhs.staged_pending_path == rhs.staged_pending_path && lhs.lock_path == rhs.lock_path &&
           lhs.lease_reserved_path == rhs.lease_reserved_path &&
           lhs.lease_reserved_pending_path == rhs.lease_reserved_pending_path &&
           lhs.lease_owned_path == rhs.lease_owned_path &&
           lhs.lease_owned_pending_path == rhs.lease_owned_pending_path &&
           lhs.private_handoff_path == rhs.private_handoff_path &&
           lhs.private_handoff_pending_path == rhs.private_handoff_pending_path &&
           lhs.private_handoff_rollback_path == rhs.private_handoff_rollback_path &&
           lhs.quarantine_index_path == rhs.quarantine_index_path &&
           lhs.quarantine_data_path == rhs.quarantine_data_path;
}

void require_private_lease_removal_generation_unchanged(PrivateCleanupActionPermit::State& state) {
    if (!state.removal_generation || !state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    const auto& expected = *state.removal_generation;
    const auto current = capture_private_lease_removal_generation_locked(
        state.paths, *state.lock, expected.expected_lease_id, expected.expected_directory_identity,
        expected.expected_owner_identity, expected.expected_owned_identity);
    if (current != expected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
}

#if defined(__APPLE__)
[[nodiscard]] bool cleanup_leaf_witness_equal(const PrivateCleanupLeafWitness& lhs,
                                              const PrivateCleanupLeafWitness& rhs) noexcept {
    return lhs.inspection.kind == rhs.inspection.kind &&
           lhs.inspection.identity == rhs.inspection.identity &&
           lhs.inspection.bytes == rhs.inspection.bytes && lhs.snapshot == rhs.snapshot;
}

[[nodiscard]] bool handoff_leaf_witness_equal(const LoadedPrivateHandoffLeaf& lhs,
                                              const LoadedPrivateHandoffLeaf& rhs) noexcept {
    return lhs.state == rhs.state && lhs.bytes == rhs.bytes && lhs.record == rhs.record &&
           lhs.snapshot == rhs.snapshot;
}

[[nodiscard]] bool
lease_marker_witness_equal(const std::optional<LoadedPrivateLeaseMarker>& lhs,
                           const std::optional<LoadedPrivateLeaseMarker>& rhs) noexcept {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    return !lhs || (lhs->record == rhs->record && lhs->identity == rhs->identity);
}

[[noreturn]] void fail_private_cleanup_witness_replacement() {
    fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None, protocol_error());
}

void require_private_cleanup_witness_unchanged(const OOCCleanupPaths& paths, const BaseLock& lock,
                                               PrivateCleanupUnionObservationWitness& witness) {
    if (!witness.directory) {
        if (inspect_directory_identity_locked(paths.private_directory)) {
            fail_private_cleanup_witness_replacement();
        }
        return;
    }
    if (!witness.before_inventory || !witness.after_inventory ||
        *witness.before_inventory != *witness.after_inventory) {
        fail_private_cleanup_witness_replacement();
    }

    witness.directory->require_stable();
    witness.directory->require_private_policy();
    const auto before_reads = scan_private_cleanup_union_directory(paths, *witness.directory);
    if (before_reads != *witness.after_inventory) {
        fail_private_cleanup_witness_replacement();
    }

    const std::array<std::filesystem::path,
                     static_cast<std::size_t>(PrivateCleanupMarkerSlot::Count)>
        cleanup_leaves{
            paths.intent_path.filename(),
            paths.intent_pending_path.filename(),
            paths.staged_path.filename(),
            paths.staged_pending_path.filename(),
        };
    for (std::size_t slot = 0; slot < cleanup_leaves.size(); ++slot) {
        if (!witness.cleanup_leaves[slot]) {
            fail_private_cleanup_witness_replacement();
        }
        const auto current = inspect_relative_cleanup_leaf(
            *witness.directory, cleanup_leaves[slot], OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2);
        if (current.inspection.kind == InspectKind::Error) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, current.inspection.error);
        }
        if (!cleanup_leaf_witness_equal(current, *witness.cleanup_leaves[slot])) {
            fail_private_cleanup_witness_replacement();
        }
    }

    const auto require_handoff_leaf_unchanged =
        [&](const std::optional<LoadedPrivateHandoffLeaf>& retained,
            const std::filesystem::path& leaf) {
            if (!retained) {
                fail_private_cleanup_witness_replacement();
            }
            const auto current =
                read_private_handoff_leaf(witness.directory->native_handle(), leaf);
            if (!handoff_leaf_witness_equal(current, *retained)) {
                fail_private_cleanup_witness_replacement();
            }
        };
    require_handoff_leaf_unchanged(witness.canonical_handoff,
                                   paths.private_handoff_path.filename());
    require_handoff_leaf_unchanged(witness.pending_handoff,
                                   paths.private_handoff_pending_path.filename());

    const bool handoff_context_was_observed =
        (witness.canonical_handoff &&
         witness.canonical_handoff->state == PrivateHandoffLeafState::Exact) ||
        (witness.pending_handoff &&
         witness.pending_handoff->state == PrivateHandoffLeafState::Exact);
    if (handoff_context_was_observed) {
        const auto current_reserved = load_optional_private_lease_marker(paths.lease_reserved_path);
        const auto current_owned = load_optional_private_lease_marker(paths.lease_owned_path);
        if (!lease_marker_witness_equal(current_reserved, witness.reserved_marker) ||
            !lease_marker_witness_equal(current_owned, witness.owned_marker)) {
            fail_private_cleanup_witness_replacement();
        }
    }

    const auto after_reads = scan_private_cleanup_union_directory(paths, *witness.directory);
    if (after_reads != *witness.after_inventory) {
        fail_private_cleanup_witness_replacement();
    }
    witness.directory->require_stable();
    witness.directory->require_private_policy();
    lock.require_stable();
}
#else
void require_private_cleanup_witness_unchanged(const OOCCleanupPaths& paths, const BaseLock& lock,
                                               PrivateCleanupUnionObservationWitness& witness) {
    const auto current = observe_private_cleanup_union_locked(paths, lock);
    if (current.raw != witness.raw) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
}
#endif

void require_private_cleanup_action_witness_unchanged(PrivateCleanupActionPermit::State& state) {
    if (!state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    require_private_cleanup_witness_unchanged(state.paths, *state.lock, state.witness);
}

} // namespace

PrivateLeaseRemovalGenerationProof capture_private_lease_removal_generation_locked(
    const OOCCleanupPaths& paths, const BaseLock& lock,
    const std::array<std::uint64_t, 2>& expected_lease_id,
    const std::array<std::uint64_t, 3>& expected_directory_identity,
    const std::array<std::uint64_t, 3>& expected_owner_identity,
    const std::array<std::uint64_t, 3>& expected_owned_identity) {
    if (paths.private_directory.empty() || !lock.matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    lock.require_stable();

    PrivateLeaseRemovalGenerationProof proof{
        .parent_identity = capture_directory_identity_locked(paths.private_directory.parent_path()),
        .expected_lease_id = expected_lease_id,
        .expected_directory_identity = expected_directory_identity,
        .expected_owner_identity = expected_owner_identity,
        .expected_owned_identity = expected_owned_identity,
    };
    proof.owned = load_optional_private_lease_marker(paths.lease_owned_path);
    proof.final_directory_identity = inspect_directory_identity_locked(paths.private_directory);

    if (!proof.owned) {
        proof.reserved = load_optional_private_lease_marker(paths.lease_reserved_path);
        proof.reserved_pending =
            load_optional_private_lease_marker(paths.lease_reserved_pending_path);
        proof.owned_pending = load_optional_private_lease_marker(paths.lease_owned_pending_path);
        proof.staging_directory_identity =
            inspect_directory_identity_locked(private_lease_staging_path(paths, expected_lease_id));
        if (proof.reserved || proof.reserved_pending || proof.owned_pending ||
            proof.final_directory_identity || proof.staging_directory_identity) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        lock.require_stable();
        return proof;
    }

    if (proof.owned->record.lease_id != expected_lease_id ||
        proof.owned->record.directory_identity != expected_directory_identity ||
        proof.owned->record.owner_identity != expected_owner_identity ||
        proof.owned->identity != expected_owned_identity) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    proof.owned_pending = load_optional_private_lease_marker(paths.lease_owned_pending_path);
    if (proof.owned_pending) {
        if (proof.owned_pending->record != proof.owned->record) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
    }
    validate_private_lease_record_context(proof.owned->record, paths, proof.parent_identity,
                                          lock.identity());
    if (proof.owned->record.phase != PrivateLeasePhase::Owned) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    proof.reserved = load_optional_private_lease_marker(paths.lease_reserved_path);
    if (proof.reserved) {
        validate_private_lease_record_context(proof.reserved->record, paths, proof.parent_identity,
                                              lock.identity());
        validate_private_lease_record_chain(proof.reserved->record, proof.owned->record);
    }
    proof.reserved_pending = load_optional_private_lease_marker(paths.lease_reserved_pending_path);
    if (proof.reserved_pending) {
        if (!proof.reserved || proof.reserved_pending->record != proof.reserved->record) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
    }

    const auto staging_path = private_lease_staging_path(paths, proof.owned->record.lease_id);
    proof.staging_directory_identity = inspect_directory_identity_locked(staging_path);
    if (proof.staging_directory_identity && proof.final_directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (proof.staging_directory_identity &&
        *proof.staging_directory_identity != proof.owned->record.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (proof.final_directory_identity &&
        *proof.final_directory_identity != proof.owned->record.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }

    const auto owner_directory =
        proof.final_directory_identity
            ? std::optional<std::filesystem::path>(paths.private_directory)
            : (proof.staging_directory_identity ? std::optional<std::filesystem::path>(staging_path)
                                                : std::nullopt);
    if (owner_directory) {
        const auto owner = inspect_private_lease_marker(private_lease_owner_path(*owner_directory));
        if (owner.kind == InspectKind::Error) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, owner.error);
        }
        if (owner.kind == InspectKind::Rejected) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        proof.owner_present = owner.kind == InspectKind::Present;
        if (proof.owner_present) {
            const auto owner_record = parse_private_lease_marker(owner.bytes);
            if (owner_record != owner_record_for(proof.owned->record) ||
                stable_identity(owner.identity) != proof.owned->record.owner_identity) {
                fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                     protocol_error());
            }
        }
    }

    if (proof.staging_directory_identity) {
        const bool preactive_pair_rollback =
            proof.reserved &&
            proof.owned->record.capability == PrivateLeaseCapability::RollbackPreactivePairAndLease;
        const bool owner_in_inventory =
            preactive_pair_rollback
                ? inspect_private_lease_preactive_entries(staging_path, paths).owner
                : inspect_private_lease_control_entries(staging_path).owner;
        if (owner_in_inventory != proof.owner_present) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
    }

    lock.require_stable();
    return proof;
}

namespace {

void require_private_lease_cleanup_handoff_generation_shape(
    const OOCCleanupPaths& paths, const PrivateLeaseRemovalGenerationProof& proof) {
    if (!proof.owned || !proof.reserved || !proof.final_directory_identity ||
        proof.staging_directory_identity || !proof.owner_present ||
        proof.owned->record.lease_id != proof.expected_lease_id ||
        proof.reserved->record.lease_id != proof.expected_lease_id ||
        proof.owned->record.directory_identity != proof.expected_directory_identity ||
        proof.owned->record.owner_identity != proof.expected_owner_identity ||
        proof.owned->identity != proof.expected_owned_identity ||
        *proof.final_directory_identity != proof.expected_directory_identity) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    inspect_private_lease_transaction_entries(paths.private_directory, paths);
}

[[nodiscard]] PrivateLeaseRemovalGenerationProof
capture_private_lease_cleanup_handoff_generation_locked(
    const OOCCleanupPaths& paths, const BaseLock& lock,
    const std::array<std::uint64_t, 2>& expected_lease_id,
    const std::array<std::uint64_t, 3>& expected_directory_identity,
    const std::array<std::uint64_t, 3>& expected_owner_identity,
    const std::array<std::uint64_t, 3>& expected_owned_identity) {
    auto proof = capture_private_lease_removal_generation_locked(
        paths, lock, expected_lease_id, expected_directory_identity, expected_owner_identity,
        expected_owned_identity);
    require_private_lease_cleanup_handoff_generation_shape(paths, proof);
    lock.require_stable();
    return proof;
}

} // namespace

namespace {

struct RetainedPrivateHandoffPublicationPrefixV1 final {
    PrivateHandoffPublicationPrefixWitnessV1 witness;
    std::optional<PrivateHandoffObservationWitness> handoff;
    std::unique_ptr<PrivateDirectoryHandle> rollback_parent;
    std::optional<LoadedPrivateHandoffLeaf> rollback;
    PrivateLeaseRemovalGenerationProof generation;
    std::optional<LoadedPrivateLeaseMarker> owner;
    bool rollback_index_present = false;
    bool rollback_data_present = false;
};

struct PrivateHandoffPublicationPrefixCaptureV1 final {
    OOCCleanupResult result;
    std::optional<RetainedPrivateHandoffPublicationPrefixV1> retained;
};

[[nodiscard]] std::array<std::uint64_t, 3>
resume_identity_words(const util::durable_immutable_record::NativeIdentity& identity) noexcept {
    return {identity.first, identity.second, identity.third};
}

[[nodiscard]] PrivateHandoffPublicationLeaseMarkerWitnessV1
resume_marker_witness(const LoadedPrivateLeaseMarker& marker) {
    return {
        .record = marker.record,
        .identity = marker.identity,
    };
}

[[nodiscard]] OOCCleanupResult resume_failure_result(const Failure& failure) noexcept {
    return {
        .status = failure.status,
        .stage = failure.stage,
        .native_error = failure.error,
    };
}

[[nodiscard]] OOCCleanupResult resume_unexpected_result(std::error_code error = {}) noexcept {
    return {
        .status = OOCCleanupStatus::UnexpectedFailure,
        .stage = OOCCleanupStage::None,
        .native_error = error,
    };
}

[[nodiscard]] PrivateHandoffPublicationResumeResultV1
resume_failed(OOCCleanupResult result,
              std::optional<PrivateHandoffPublicationPrefixWitnessV1> expected = std::nullopt) {
    return {
        .result = result,
        .disposition = PrivateHandoffPublicationResumeDispositionV1::Failed,
        .expected_prefix = std::move(expected),
        .terminal_prefix = std::nullopt,
    };
}

[[nodiscard]] OOCCleanupResult resume_foreign_replacement() noexcept {
    return {
        .status = OOCCleanupStatus::ForeignReplacementPreserved,
        .stage = OOCCleanupStage::None,
        .native_error = protocol_error(),
    };
}

[[nodiscard]] OOCCleanupResult resume_interrupted_result() noexcept {
    return {
        .status = OOCCleanupStatus::Interrupted,
        .stage = OOCCleanupStage::None,
        .native_error = {},
    };
}

[[nodiscard]] bool canonical_publication_terminal_shape_valid(
    const PrivateHandoffPublicationPrefixWitnessV1& terminal) {
    if (terminal.state != PrivateHandoffPublicationPrefixStateV1::Canonical ||
        !terminal.canonical_snapshot || terminal.pending_snapshot || terminal.rollback_snapshot ||
        !terminal.owner || !terminal.owned || terminal.reserved ||
        !private_lease_record_shape_valid(terminal.owner->record) ||
        !private_lease_record_shape_valid(terminal.owned->record)) {
        return false;
    }

    const auto& owner = *terminal.owner;
    const auto& owned = *terminal.owned;
    return terminal.record.lock_identity == handoff_native_identity(terminal.lock_identity) &&
           terminal.record.directory_identity ==
               handoff_native_identity(terminal.directory_identity) &&
           terminal.record.owner_marker_identity == handoff_native_identity(owner.identity) &&
           terminal.record.owned_marker_identity == handoff_native_identity(owned.identity) &&
           terminal.record.lease_id == owned.record.lease_id &&
           owner.record == owner_record_for(owned.record) &&
           owned.record.phase == PrivateLeasePhase::Owned &&
           owned.record.capability == PrivateLeaseCapability::RollbackPreactivePairAndLease &&
           owned.record.parent_identity == terminal.parent_identity &&
           owned.record.lock_identity == terminal.lock_identity &&
           owned.record.directory_identity == terminal.directory_identity &&
           owned.record.owner_identity == owner.identity;
}

[[nodiscard]] OOCPrivateHandoffAdoptionResult
consumed_publication_adoption_failure(OOCCleanupStatus status,
                                      std::error_code error = {}) noexcept {
    if (!error) {
        error = protocol_error();
    }
    return {
        .result =
            {
                .status = status,
                .stage = OOCCleanupStage::None,
                .native_error = error,
            },
        .state = OOCPrivateHandoffState::TaintedPreserved,
        .adoption = std::nullopt,
    };
}

[[nodiscard]] PrivateHandoffPublicationReaderAdoptionResultV1
consumed_publication_reader_adoption_failure(OOCCleanupStatus status,
                                             std::error_code error = {}) noexcept {
    if (!error) {
        error = protocol_error();
    }
    return PrivateHandoffPublicationReaderAdoptionResultV1(
        OOCCleanupResult{
            .status = status,
            .stage = OOCCleanupStage::None,
            .native_error = error,
        },
        OOCPrivateHandoffState::TaintedPreserved, nullptr);
}

#if defined(__APPLE__)
[[nodiscard]] PrivateHandoffPublicationPrefixCaptureV1
capture_private_handoff_publication_prefix_v1_locked(
    const OOCCleanupPaths& paths, const BaseLock& lock,
    const std::array<std::uint64_t, 3>& expected_directory_identity);

void sync_resume_directory_handle(PrivateDirectoryHandle& directory) {
    int result = -1;
    do {
        result = ::fcntl(static_cast<int>(directory.native_handle()), F_FULLFSYNC);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        fail(OOCCleanupStatus::DurabilityFailure, OOCCleanupStage::None, posix_error(errno));
    }
    directory.require_stable();
}

struct PrivateHandoffLeaseRecoveryObservationAdapterV1 final {
    const PrivateHandoffPublicationResumeTestHooksV1* outer = nullptr;
    const OOCCleanupPaths* paths = nullptr;
    const BaseLock* lock = nullptr;
    const std::array<std::uint64_t, 3>* expected_directory_identity = nullptr;
    const PrivateHandoffPublicationPrefixWitnessV1* initial = nullptr;
    std::optional<OOCCleanupResult> exact_failure;
    bool unknown_point = false;
};

[[nodiscard]] std::optional<PrivateHandoffPublicationResumeObservationPointV1>
map_private_handoff_lease_recovery_observation(OOCPrivateLeaseFaultPoint point) noexcept {
    using OuterPoint = PrivateHandoffPublicationResumeObservationPointV1;
    switch (point) {
    case OOCPrivateLeaseFaultPoint::PreactiveDirectoryQuarantinedDurable:
        return OuterPoint::AfterPendingRollbackPreactiveDirectoryQuarantinedDurable;
    case OOCPrivateLeaseFaultPoint::PreactiveDataRemovedDurable:
        return OuterPoint::AfterPendingRollbackPreactiveDataRemovedDurable;
    case OOCPrivateLeaseFaultPoint::PreactiveIndexRemovedDurable:
        return OuterPoint::AfterPendingRollbackPreactiveIndexRemovedDurable;
    case OOCPrivateLeaseFaultPoint::OwnerRemovedDurable:
        return OuterPoint::AfterPendingRollbackOwnerRemovedDurable;
    case OOCPrivateLeaseFaultPoint::FinalDirectoryRemovedDurable:
        return OuterPoint::AfterPendingRollbackLeaseDirectoryRemovedDurable;
    case OOCPrivateLeaseFaultPoint::ReservedRemovedDurable:
        return OuterPoint::AfterPendingRollbackReservedRemovedDurable;
    case OOCPrivateLeaseFaultPoint::OwnedRemovedDurable:
        return OuterPoint::AfterPendingRollbackOwnedRemovedDurable;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] bool private_handoff_lease_recovery_stage_matches(
    OOCPrivateLeaseFaultPoint point,
    const RetainedPrivateHandoffPublicationPrefixV1& current) noexcept {
    const bool final_directory = current.generation.final_directory_identity.has_value();
    const bool staging_directory = current.generation.staging_directory_identity.has_value();
    const bool owner = current.witness.owner.has_value();
    const bool owned = current.witness.owned.has_value();
    const bool reserved = current.witness.reserved.has_value();
    const bool index = current.rollback_index_present;
    const bool data = current.rollback_data_present;
    switch (point) {
    case OOCPrivateLeaseFaultPoint::PreactiveDirectoryQuarantinedDurable:
        return !final_directory && staging_directory && owner && owned && reserved && index && data;
    case OOCPrivateLeaseFaultPoint::PreactiveDataRemovedDurable:
        return !final_directory && staging_directory && owner && owned && reserved && index &&
               !data;
    case OOCPrivateLeaseFaultPoint::PreactiveIndexRemovedDurable:
        return !final_directory && staging_directory && owner && owned && reserved && !index &&
               !data;
    case OOCPrivateLeaseFaultPoint::OwnerRemovedDurable:
        return !final_directory && staging_directory && !owner && owned && reserved && !index &&
               !data;
    case OOCPrivateLeaseFaultPoint::FinalDirectoryRemovedDurable:
        return !final_directory && !staging_directory && !owner && owned && reserved && !index &&
               !data;
    case OOCPrivateLeaseFaultPoint::ReservedRemovedDurable:
        return !final_directory && !staging_directory && !owner && owned && !reserved && !index &&
               !data;
    case OOCPrivateLeaseFaultPoint::OwnedRemovedDurable:
        return !final_directory && !staging_directory && !owner && !owned && !reserved && !index &&
               !data;
    default:
        return false;
    }
}

[[nodiscard]] bool observe_private_handoff_lease_recovery(OOCPrivateLeaseFaultPoint point,
                                                          void* opaque) noexcept {
    auto& adapter = *static_cast<PrivateHandoffLeaseRecoveryObservationAdapterV1*>(opaque);
    const auto mapped = map_private_handoff_lease_recovery_observation(point);
    if (!mapped) {
        // A new inner mutation boundary must be deliberately exposed through
        // the outer authority bridge before relation resume may proceed.
        adapter.unknown_point = true;
        return true;
    }
    if (adapter.outer != nullptr && adapter.outer->stop_after != nullptr &&
        adapter.outer->stop_after(*mapped, adapter.outer->context)) {
        return true;
    }
    try {
        if (adapter.paths == nullptr || adapter.lock == nullptr ||
            adapter.expected_directory_identity == nullptr || adapter.initial == nullptr) {
            adapter.exact_failure = resume_unexpected_result(protocol_error());
            return true;
        }
        auto current = capture_private_handoff_publication_prefix_v1_locked(
            *adapter.paths, *adapter.lock, *adapter.expected_directory_identity);
        const auto& initial = *adapter.initial;
        const auto remaining_marker_matches_initial =
            [](const std::optional<PrivateHandoffPublicationLeaseMarkerWitnessV1>& observed,
               const std::optional<PrivateHandoffPublicationLeaseMarkerWitnessV1>& expected) {
                return !observed || (expected && *observed == *expected);
            };
        if (!current.retained ||
            initial.state != PrivateHandoffPublicationPrefixStateV1::PendingRollback ||
            initial.canonical_snapshot || initial.pending_snapshot || !initial.rollback_snapshot ||
            current.retained->witness.state !=
                PrivateHandoffPublicationPrefixStateV1::PendingRollback ||
            current.retained->witness.record != initial.record ||
            current.retained->witness.canonical_snapshot ||
            current.retained->witness.pending_snapshot ||
            !current.retained->witness.rollback_snapshot ||
            current.retained->witness.rollback_snapshot != initial.rollback_snapshot ||
            current.retained->witness.parent_identity != initial.parent_identity ||
            current.retained->witness.lock_identity != initial.lock_identity ||
            current.retained->witness.directory_identity != initial.directory_identity ||
            !remaining_marker_matches_initial(current.retained->witness.owner, initial.owner) ||
            !remaining_marker_matches_initial(current.retained->witness.owned, initial.owned) ||
            !remaining_marker_matches_initial(current.retained->witness.reserved,
                                              initial.reserved) ||
            !private_handoff_lease_recovery_stage_matches(point, *current.retained)) {
            adapter.exact_failure =
                current.retained ? resume_foreign_replacement() : current.result;
            if (adapter.exact_failure->status == OOCCleanupStatus::NoTransaction) {
                adapter.exact_failure = resume_foreign_replacement();
            }
            return true;
        }
        adapter.lock->require_stable();
        return false;
    } catch (const Failure& failure) {
        adapter.exact_failure = resume_failure_result(failure);
    } catch (const std::bad_alloc&) {
        adapter.exact_failure =
            resume_unexpected_result(std::make_error_code(std::errc::not_enough_memory));
    } catch (const std::system_error& error) {
        adapter.exact_failure = resume_unexpected_result(error.code());
    } catch (...) {
        adapter.exact_failure = resume_unexpected_result();
    }
    return true;
}

/// Consume only the exact RESERVED/OWNED preactive generation already bound
/// to a retained PendingRollback handoff witness.
///
/// The public/private-lease recovery executor must keep its generic cleanup
/// union preflight: deferred cleanup intents can legitimately coexist with a
/// normal preactive lease. A durable handoff rollback tombstone intentionally
/// fails that preflight, so the typed resume path uses this narrower executor.
/// Its preactive directory scanner accepts only the exact pair and owner
/// leaves; cleanup markers and every foreign child fail before the first
/// rename or unlink.
[[nodiscard]] OOCCleanupResult recover_private_handoff_rollback_generation_locked(
    const OOCCleanupPaths& paths, const BaseLock& lock,
    const std::array<std::uint64_t, 3>& parent_identity,
    const LoadedPrivateLeaseMarker& loaded_owned,
    const std::optional<LoadedPrivateLeaseMarker>& loaded_reserved,
    const OOCPrivateLeaseTestHooks& hooks) {
    lock.require_stable();
    const auto& owned = loaded_owned.record;
    validate_private_lease_record_context(owned, paths, parent_identity, lock.identity());
    if (owned.phase != PrivateLeasePhase::Owned ||
        owned.capability != PrivateLeaseCapability::RollbackPreactivePairAndLease) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    confirm_private_lease_marker(paths.lease_owned_path, owned, loaded_owned.identity);

    if (loaded_reserved) {
        validate_private_lease_record_context(loaded_reserved->record, paths, parent_identity,
                                              lock.identity());
        validate_private_lease_record_chain(loaded_reserved->record, owned);
        confirm_private_lease_marker(paths.lease_reserved_path, loaded_reserved->record,
                                     loaded_reserved->identity);
    }

    const auto staging_path = private_lease_staging_path(paths, owned.lease_id);
    const auto staging_identity = inspect_directory_identity_locked(staging_path);
    const auto final_identity = inspect_directory_identity_locked(paths.private_directory);
    if ((staging_identity && final_identity) ||
        (staging_identity && *staging_identity != owned.directory_identity) ||
        (final_identity && *final_identity != owned.directory_identity) ||
        (!loaded_reserved && (staging_identity || final_identity))) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }

    if (staging_identity || final_identity) {
        const auto rolled_back = rollback_owned_preactive_pair_locked(paths, owned, lock, hooks);
        if (!rolled_back.completed()) {
            return rolled_back;
        }
    } else {
        invoke_with_stable_base_lock(lock, [&] {
            sync_parent_directory(paths.private_directory.parent_path(), OOCCleanupStage::None);
        });
    }

    if (loaded_reserved) {
        invoke_with_stable_base_lock(lock, [&] {
            remove_private_lease_marker_durable(paths.lease_reserved_path, loaded_reserved->record,
                                                loaded_reserved->identity);
        });
        if (invoke_with_stable_base_lock(lock, [&] {
                return should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::ReservedRemovedDurable);
            })) {
            return private_lease_interrupted();
        }
    }
    invoke_with_stable_base_lock(lock, [&] {
        remove_private_lease_marker_durable(paths.lease_owned_path, owned, loaded_owned.identity);
    });
    if (invoke_with_stable_base_lock(lock, [&] {
            return should_interrupt_private_lease(hooks,
                                                  OOCPrivateLeaseFaultPoint::OwnedRemovedDurable);
        })) {
        return private_lease_interrupted();
    }
    lock.require_stable();
    return private_lease_completed();
}

void move_exact_pending_to_rollback(
    PrivateDirectoryHandle& source, PrivateDirectoryHandle& destination,
    const std::filesystem::path& source_leaf, const std::filesystem::path& destination_leaf,
    const util::durable_immutable_record::RecordSnapshot& expected) {
    struct stat metadata{};
    int result = -1;
    do {
        result = ::fstatat(static_cast<int>(source.native_handle()), source_leaf.c_str(), &metadata,
                           AT_SYMLINK_NOFOLLOW);
    } while (result != 0 && errno == EINTR);
    if (result != 0 || !S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 ||
        metadata.st_size < 0 || static_cast<std::uint64_t>(metadata.st_size) != expected.size ||
        static_cast<std::uint64_t>(metadata.st_uid) != static_cast<std::uint64_t>(::geteuid()) ||
        (metadata.st_mode & static_cast<mode_t>(07777)) != (S_IRUSR | S_IWUSR) ||
        util::durable_immutable_record::NativeIdentity{
            .first = static_cast<std::uint64_t>(metadata.st_dev),
            .second = static_cast<std::uint64_t>(metadata.st_ino),
            .third = 0,
        } != expected.identity) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    do {
        result = ::fstatat(static_cast<int>(destination.native_handle()), destination_leaf.c_str(),
                           &metadata, AT_SYMLINK_NOFOLLOW);
    } while (result != 0 && errno == EINTR);
    if (result == 0 || errno != ENOENT) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             result == 0 ? protocol_error() : posix_error(errno));
    }

    // Apple-only crash contract: rename(2) explicitly guarantees that an
    // instance of `new` always exists even if the system crashes in the middle
    // of the operation, and renameatx_np is the descriptor-relative variant
    // used here with RENAME_EXCL. After a successful live call, the atomic
    // namespace transition exposes this exact inode only at the rollback name.
    // We sync the source directory first, making the old-name removal durable;
    // Apple's `new`-always-exists guarantee then closes the apparent window
    // before the destination-directory sync. The second barrier separately
    // makes the new-name insertion durable. Only both barriers together are
    // evidence for a crash-durable armed tombstone. This ordering and claim
    // must not be generalized to non-Apple rename implementations.
    do {
        result = ::renameatx_np(static_cast<int>(source.native_handle()), source_leaf.c_str(),
                                static_cast<int>(destination.native_handle()),
                                destination_leaf.c_str(), RENAME_EXCL);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        const int saved_errno = errno;
        const auto error = posix_error(saved_errno);
        auto status = OOCCleanupStatus::IoFailure;
        if (saved_errno == EEXIST) {
            status = OOCCleanupStatus::ForeignReplacementPreserved;
        } else if (saved_errno == ENOTSUP || saved_errno == EOPNOTSUPP) {
            status = OOCCleanupStatus::PlatformUnsupported;
        }
        fail(status, OOCCleanupStage::None, error);
    }

    const auto moved = read_private_handoff_leaf(destination.native_handle(), destination_leaf);
    if (moved.state != PrivateHandoffLeafState::Exact || !moved.snapshot ||
        *moved.snapshot != expected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    source.require_stable();
    destination.require_stable();
}

[[nodiscard]] bool exact_remaining_handoff_pair_leaf(const std::filesystem::path& path,
                                                     const auto& expected) {
    const auto inspected = inspect_file(path, 0, false);
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (inspected.kind == InspectKind::Missing) {
        return false;
    }
    if (inspected.kind != InspectKind::Present ||
        handoff_native_identity(inspected.identity) != expected.identity ||
        inspected.identity.size != expected.extent) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    return true;
}

[[nodiscard]] PrivateHandoffPublicationPrefixCaptureV1
capture_private_handoff_publication_rollback_v1_locked(
    const OOCCleanupPaths& paths, const BaseLock& lock,
    const std::array<std::uint64_t, 3>& expected_directory_identity,
    std::unique_ptr<PrivateDirectoryHandle> rollback_parent, LoadedPrivateHandoffLeaf rollback) {
    if (!rollback.record || !rollback.snapshot ||
        rollback.state != PrivateHandoffLeafState::Exact) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    const auto& record = *rollback.record;
    if (record.lock_identity != handoff_native_identity(lock.identity()) ||
        record.directory_identity != handoff_native_identity(expected_directory_identity)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    const auto owner_identity = resume_identity_words(record.owner_marker_identity);
    const auto owned_identity = resume_identity_words(record.owned_marker_identity);
    auto generation = capture_private_lease_removal_generation_locked(
        paths, lock, record.lease_id, expected_directory_identity, owner_identity, owned_identity);
    if (generation.owned_pending || generation.reserved_pending) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    std::optional<LoadedPrivateLeaseMarker> owner;
    bool index_present = false;
    bool data_present = false;
    if (generation.owned) {
        if (generation.owned->record.capability !=
            PrivateLeaseCapability::RollbackPreactivePairAndLease) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        if (!generation.reserved &&
            (generation.final_directory_identity || generation.staging_directory_identity ||
             generation.owner_present)) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }

        const auto staging_path = private_lease_staging_path(paths, record.lease_id);
        const auto active_directory =
            generation.final_directory_identity
                ? std::optional<std::filesystem::path>(paths.private_directory)
                : (generation.staging_directory_identity
                       ? std::optional<std::filesystem::path>(staging_path)
                       : std::nullopt);
        if (active_directory) {
            const auto entries = inspect_private_lease_preactive_entries(*active_directory, paths);
            index_present = exact_remaining_handoff_pair_leaf(
                *active_directory / paths.index_path.filename(), record.index);
            data_present = exact_remaining_handoff_pair_leaf(
                *active_directory / paths.data_path.filename(), record.data);
            if (entries.index != index_present || entries.data != data_present ||
                (data_present && !index_present) ||
                ((index_present || data_present) && !entries.owner) ||
                entries.owner != generation.owner_present) {
                fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                     protocol_error());
            }
            if (generation.owner_present) {
                owner =
                    load_optional_private_lease_marker(private_lease_owner_path(*active_directory));
                if (!owner || owner->identity != owner_identity ||
                    owner->record != owner_record_for(generation.owned->record)) {
                    fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                         protocol_error());
                }
            }
        }
    } else if (generation.reserved || generation.final_directory_identity ||
               generation.staging_directory_identity || generation.owner_present) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    PrivateHandoffPublicationPrefixWitnessV1 witness{
        .state = PrivateHandoffPublicationPrefixStateV1::PendingRollback,
        .record = record,
        .canonical_snapshot = std::nullopt,
        .pending_snapshot = std::nullopt,
        .rollback_snapshot = rollback.snapshot,
        .parent_identity = generation.parent_identity,
        .lock_identity = lock.identity(),
        .directory_identity = expected_directory_identity,
        .owner = owner ? std::optional(resume_marker_witness(*owner)) : std::nullopt,
        .owned = generation.owned ? std::optional(resume_marker_witness(*generation.owned))
                                  : std::nullopt,
        .reserved = generation.reserved
                        ? std::optional<PrivateHandoffPublicationLeaseMarkerWitnessV1>(
                              resume_marker_witness(*generation.reserved))
                        : std::nullopt,
    };
    rollback_parent->require_stable();
    if (rollback_parent->identity() != generation.parent_identity) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    lock.require_stable();
    return {
        .result =
            {
                .status = OOCCleanupStatus::RecoveryRequired,
                .stage = OOCCleanupStage::None,
                .native_error = protocol_error(),
            },
        .retained =
            RetainedPrivateHandoffPublicationPrefixV1{
                .witness = std::move(witness),
                .handoff = std::nullopt,
                .rollback_parent = std::move(rollback_parent),
                .rollback = std::move(rollback),
                .generation = std::move(generation),
                .owner = std::move(owner),
                .rollback_index_present = index_present,
                .rollback_data_present = data_present,
            },
    };
}

[[nodiscard]] PrivateHandoffPublicationPrefixCaptureV1
capture_private_handoff_publication_prefix_v1_locked(
    const OOCCleanupPaths& paths, const BaseLock& lock,
    const std::array<std::uint64_t, 3>& expected_directory_identity) {
    lock.require_stable();
    auto rollback_parent =
        std::make_unique<PrivateDirectoryHandle>(paths.private_handoff_rollback_path.parent_path());
    auto rollback = read_private_handoff_leaf(rollback_parent->native_handle(),
                                              paths.private_handoff_rollback_path.filename());
    rollback_parent->require_stable();
    auto handoff = observe_private_handoff_locked(paths, lock);
    if (rollback.state == PrivateHandoffLeafState::Rejected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    if (rollback.state == PrivateHandoffLeafState::Exact) {
        if (handoff.inspection.state != OOCPrivateHandoffState::None) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        return capture_private_handoff_publication_rollback_v1_locked(
            paths, lock, expected_directory_identity, std::move(rollback_parent),
            std::move(rollback));
    }
    if (handoff.inspection.state == OOCPrivateHandoffState::None ||
        handoff.inspection.state == OOCPrivateHandoffState::TaintedPreserved) {
        return {
            .result = handoff.inspection.result,
            .retained = std::nullopt,
        };
    }
    if (!handoff.directory || handoff.directory->identity() != expected_directory_identity) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    const bool pending_only = handoff.inspection.state == OOCPrivateHandoffState::PendingOnly;
    const LoadedPrivateHandoffLeaf* canonical = handoff.canonical ? &*handoff.canonical : nullptr;
    const LoadedPrivateHandoffLeaf* pending = handoff.pending ? &*handoff.pending : nullptr;
    const LoadedPrivateHandoffLeaf* authoritative = pending_only ? pending : canonical;
    if (authoritative == nullptr || authoritative->state != PrivateHandoffLeafState::Exact ||
        !authoritative->record || !authoritative->snapshot ||
        (pending_only && (!handoff.pending_is_preactive || canonical != nullptr)) ||
        (!pending_only && canonical == nullptr)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    const auto& record = *authoritative->record;
    const auto owner_identity = resume_identity_words(record.owner_marker_identity);
    const auto owned_identity = resume_identity_words(record.owned_marker_identity);
    auto generation = capture_private_lease_removal_generation_locked(
        paths, lock, record.lease_id, expected_directory_identity, owner_identity, owned_identity);
    if (!generation.owned || generation.owned_pending || generation.reserved_pending ||
        !generation.final_directory_identity || generation.staging_directory_identity ||
        !generation.owner_present ||
        *generation.final_directory_identity != expected_directory_identity ||
        generation.owned->record.capability !=
            PrivateLeaseCapability::RollbackPreactivePairAndLease ||
        (pending_only && !generation.reserved)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    const auto loaded_owner =
        load_optional_private_lease_marker(private_lease_owner_path(paths.private_directory));
    if (!loaded_owner || loaded_owner->identity != owner_identity ||
        loaded_owner->record != owner_record_for(generation.owned->record) ||
        rollback_parent->identity() != generation.parent_identity ||
        !handoff_record_matches_context(record, paths, lock, expected_directory_identity,
                                        generation.reserved, generation.owned, pending_only)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    const bool identical_dual = !pending_only && pending != nullptr;
    PrivateHandoffPublicationPrefixWitnessV1 witness{
        .state = pending_only     ? PrivateHandoffPublicationPrefixStateV1::PendingOnly
                 : identical_dual ? PrivateHandoffPublicationPrefixStateV1::IdenticalDual
                                  : PrivateHandoffPublicationPrefixStateV1::Canonical,
        .record = record,
        .canonical_snapshot = canonical != nullptr ? canonical->snapshot : std::nullopt,
        .pending_snapshot = pending != nullptr ? pending->snapshot : std::nullopt,
        .rollback_snapshot = std::nullopt,
        .parent_identity = generation.parent_identity,
        .lock_identity = lock.identity(),
        .directory_identity = expected_directory_identity,
        .owner = resume_marker_witness(*loaded_owner),
        .owned = resume_marker_witness(*generation.owned),
        .reserved = generation.reserved
                        ? std::optional<PrivateHandoffPublicationLeaseMarkerWitnessV1>(
                              resume_marker_witness(*generation.reserved))
                        : std::nullopt,
    };

    if ((pending_only && (witness.canonical_snapshot || !witness.pending_snapshot)) ||
        (!pending_only && !witness.canonical_snapshot) ||
        (!pending_only && identical_dual != witness.pending_snapshot.has_value()) ||
        record.lock_identity != handoff_native_identity(witness.lock_identity) ||
        record.directory_identity != handoff_native_identity(witness.directory_identity)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    handoff.directory->require_stable();
    handoff.directory->require_private_policy();
    lock.require_stable();
    return {
        .result = handoff.inspection.result,
        .retained =
            RetainedPrivateHandoffPublicationPrefixV1{
                .witness = std::move(witness),
                .handoff = std::move(handoff),
                .rollback_parent = std::move(rollback_parent),
                .rollback = std::nullopt,
                .generation = std::move(generation),
                .owner = *loaded_owner,
                .rollback_index_present = false,
                .rollback_data_present = false,
            },
    };
}
#endif

} // namespace

struct PrivateHandoffPublicationObservedPermitV1::State final {
    enum class Phase : std::uint8_t {
        Observed,
        Validated,
        ConsumedNonTerminal,
        ConsumedCanonical,
    };

    State(OOCCleanupPaths frozen_paths, std::array<std::uint64_t, 3> expected_directory,
          RetainedPrivateHandoffPublicationPrefixV1 retained_prefix)
        : paths(std::move(frozen_paths)), expected_directory_identity(expected_directory),
          creator_process_id(static_cast<std::uint64_t>(gnfs::util::process_id())),
          prefix(std::move(retained_prefix)) {}

    ~State() {
        if (lock && creator_process_id == static_cast<std::uint64_t>(gnfs::util::process_id())) {
            release_private_cleanup_action(*lock);
        }
    }

    OOCCleanupPaths paths;
    std::unique_ptr<BaseLock> lock;
    std::array<std::uint64_t, 3> expected_directory_identity{};
    std::uint64_t creator_process_id = 0;
    Phase phase = Phase::Observed;
    RetainedPrivateHandoffPublicationPrefixV1 prefix;
    std::optional<PrivateHandoffPublicationPrefixWitnessV1> canonical_terminal;
};

namespace {

static_assert(std::is_nothrow_move_constructible_v<PrivateHandoffPublicationPrefixWitnessV1>);
static_assert(std::is_nothrow_move_constructible_v<PrivateHandoffPublicationResumeResultV1>);

[[nodiscard]] PrivateHandoffPublicationResumeResultV1
commit_canonical_publication_terminal(PrivateHandoffPublicationObservedPermitV1::State& state,
                                      PrivateHandoffPublicationResumeResultV1 completed) {
    const bool canonical_disposition =
        completed.disposition == PrivateHandoffPublicationResumeDispositionV1::CanonicalTerminal ||
        completed.disposition == PrivateHandoffPublicationResumeDispositionV1::CanonicalConverged;
    if (state.phase !=
            PrivateHandoffPublicationObservedPermitV1::State::Phase::ConsumedNonTerminal ||
        state.canonical_terminal || !canonical_disposition || !completed.expected_prefix ||
        !completed.terminal_prefix || completed.result.status != OOCCleanupStatus::HandoffPresent ||
        completed.result.stage != OOCCleanupStage::None || completed.result.native_error ||
        !canonical_publication_terminal_shape_valid(*completed.terminal_prefix)) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }

    auto retained_terminal = *completed.terminal_prefix;
    state.canonical_terminal.emplace(std::move(retained_terminal));
    state.phase = PrivateHandoffPublicationObservedPermitV1::State::Phase::ConsumedCanonical;
    return completed;
}

} // namespace

PrivateHandoffPublicationTypedValidatorV1::PrivateHandoffPublicationTypedValidatorV1(
    Validate validate, void* context) noexcept
    : validate_(validate), context_(context),
      creator_process_id_(static_cast<std::uint64_t>(gnfs::util::process_id())) {}

PrivateHandoffPublicationTypedValidatorV1::PrivateHandoffPublicationTypedValidatorV1(
    PrivateHandoffPublicationTypedValidatorV1&& other) noexcept
    : validate_(std::exchange(other.validate_, nullptr)),
      context_(std::exchange(other.context_, nullptr)),
      creator_process_id_(std::exchange(other.creator_process_id_, 0)) {}

PrivateHandoffPublicationAdoptionRevalidatorV1::PrivateHandoffPublicationAdoptionRevalidatorV1(
    Validate validate, void* context) noexcept
    : validate_(validate), context_(context),
      creator_process_id_(static_cast<std::uint64_t>(gnfs::util::process_id())) {}

PrivateHandoffPublicationAdoptionRevalidatorV1::PrivateHandoffPublicationAdoptionRevalidatorV1(
    PrivateHandoffPublicationAdoptionRevalidatorV1&& other) noexcept
    : validate_(std::exchange(other.validate_, nullptr)),
      context_(std::exchange(other.context_, nullptr)),
      creator_process_id_(std::exchange(other.creator_process_id_, 0)) {}

PrivateHandoffPublicationObservedPermitV1::PrivateHandoffPublicationObservedPermitV1(
    std::shared_ptr<State> state) noexcept
    : state_(std::move(state)) {}

PrivateHandoffPublicationObservedPermitV1::PrivateHandoffPublicationObservedPermitV1(
    PrivateHandoffPublicationObservedPermitV1&& other) noexcept
    : state_(std::move(other.state_)) {}

PrivateHandoffPublicationObservedPermitV1::~PrivateHandoffPublicationObservedPermitV1() = default;

bool PrivateHandoffPublicationObservedPermitV1::valid() const noexcept {
    return state_ && state_->lock && state_->phase == State::Phase::Observed &&
           state_->creator_process_id == static_cast<std::uint64_t>(gnfs::util::process_id());
}

const PrivateHandoffPublicationPrefixWitnessV1*
PrivateHandoffPublicationObservedPermitV1::witness() const noexcept {
    return valid() ? &state_->prefix.witness : nullptr;
}

PrivateHandoffPublicationValidatedPermitV1::PrivateHandoffPublicationValidatedPermitV1(
    std::shared_ptr<PrivateHandoffPublicationObservedPermitV1::State> state) noexcept
    : state_(std::move(state)) {}

PrivateHandoffPublicationValidatedPermitV1::PrivateHandoffPublicationValidatedPermitV1(
    PrivateHandoffPublicationValidatedPermitV1&& other) noexcept
    : state_(std::move(other.state_)) {}

PrivateHandoffPublicationValidatedPermitV1::~PrivateHandoffPublicationValidatedPermitV1() = default;

bool PrivateHandoffPublicationValidatedPermitV1::valid() const noexcept {
    return state_ && state_->lock &&
           state_->phase == PrivateHandoffPublicationObservedPermitV1::State::Phase::Validated &&
           state_->creator_process_id == static_cast<std::uint64_t>(gnfs::util::process_id());
}

bool PrivateHandoffPublicationValidatedPermitV1::held() const noexcept {
    return state_ && state_->lock &&
           state_->creator_process_id == static_cast<std::uint64_t>(gnfs::util::process_id());
}

PrivateHandoffPublicationReaderAdoptionResultV1::PrivateHandoffPublicationReaderAdoptionResultV1(
    OOCCleanupResult cleanup_result, OOCPrivateHandoffState handoff_state,
    std::unique_ptr<OOCPrivateHandoffReader> adopted_reader) noexcept
    : result(cleanup_result), state(handoff_state), reader(std::move(adopted_reader)) {}

PrivateHandoffPublicationReaderAdoptionResultV1::PrivateHandoffPublicationReaderAdoptionResultV1(
    PrivateHandoffPublicationReaderAdoptionResultV1&& other) noexcept
    : result(other.result), state(other.state), reader(std::move(other.reader)) {}

PrivateHandoffPublicationReaderAdoptionResultV1::
    ~PrivateHandoffPublicationReaderAdoptionResultV1() = default;

bool PrivateHandoffPublicationReaderAdoptionResultV1::adopted() const noexcept {
    return result.status == OOCCleanupStatus::HandoffPresent &&
           state == OOCPrivateHandoffState::Canonical && reader && reader->valid();
}

static_assert(!std::is_default_constructible_v<PrivateHandoffPublicationTypedValidatorV1>);
static_assert(!std::is_copy_constructible_v<PrivateHandoffPublicationTypedValidatorV1>);
static_assert(!std::is_copy_assignable_v<PrivateHandoffPublicationTypedValidatorV1>);
static_assert(std::is_nothrow_move_constructible_v<PrivateHandoffPublicationTypedValidatorV1>);
static_assert(!std::is_move_assignable_v<PrivateHandoffPublicationTypedValidatorV1>);
static_assert(!std::is_default_constructible_v<PrivateHandoffPublicationAdoptionRevalidatorV1>);
static_assert(!std::is_copy_constructible_v<PrivateHandoffPublicationAdoptionRevalidatorV1>);
static_assert(!std::is_copy_assignable_v<PrivateHandoffPublicationAdoptionRevalidatorV1>);
static_assert(std::is_nothrow_move_constructible_v<PrivateHandoffPublicationAdoptionRevalidatorV1>);
static_assert(!std::is_move_assignable_v<PrivateHandoffPublicationAdoptionRevalidatorV1>);
static_assert(!std::is_default_constructible_v<PrivateHandoffPublicationObservedPermitV1>);
static_assert(!std::is_copy_constructible_v<PrivateHandoffPublicationObservedPermitV1>);
static_assert(!std::is_copy_assignable_v<PrivateHandoffPublicationObservedPermitV1>);
static_assert(std::is_nothrow_move_constructible_v<PrivateHandoffPublicationObservedPermitV1>);
static_assert(!std::is_move_assignable_v<PrivateHandoffPublicationObservedPermitV1>);
static_assert(!std::is_default_constructible_v<PrivateHandoffPublicationValidatedPermitV1>);
static_assert(!std::is_copy_constructible_v<PrivateHandoffPublicationValidatedPermitV1>);
static_assert(!std::is_copy_assignable_v<PrivateHandoffPublicationValidatedPermitV1>);
static_assert(std::is_nothrow_move_constructible_v<PrivateHandoffPublicationValidatedPermitV1>);
static_assert(!std::is_move_assignable_v<PrivateHandoffPublicationValidatedPermitV1>);
static_assert(!std::is_constructible_v<PrivateHandoffPublicationValidatedPermitV1,
                                       PrivateHandoffPublicationObservedPermitV1&&>);
static_assert(!std::is_copy_constructible_v<PrivateHandoffPublicationReaderAdoptionResultV1>);
static_assert(
    std::is_nothrow_move_constructible_v<PrivateHandoffPublicationReaderAdoptionResultV1>);
static_assert(!std::is_move_assignable_v<PrivateHandoffPublicationReaderAdoptionResultV1>);

PrivateHandoffPublicationResumeAdmissionV1 acquire_private_handoff_publication_resume_v1(
    const OOCCleanupPaths& paths,
    const std::array<std::uint64_t, 3>& expected_directory_identity) noexcept {
    try {
        const auto expected_rollback_path =
            paths.private_directory.empty()
                ? std::filesystem::path{}
                : append_leaf_suffix(paths.private_directory.parent_path(),
                                     paths.private_directory.filename(),
                                     ".gnfs-ooc-private-handoff-v1.rollback");
        if (paths.private_directory.empty() || paths.lock_path.empty() ||
            paths.private_handoff_rollback_path.empty() ||
            paths.private_handoff_rollback_path != expected_rollback_path ||
            paths.private_handoff_rollback_path.parent_path() !=
                paths.private_directory.parent_path()) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
#if !defined(__APPLE__)
        (void)expected_directory_identity;
        return {
            .result =
                {
                    .status = OOCCleanupStatus::PlatformUnsupported,
                    .stage = OOCCleanupStage::None,
                    .native_error = std::make_error_code(std::errc::operation_not_supported),
                },
            .observed = std::nullopt,
        };
#else
        auto lock = std::make_unique<BaseLock>(paths.lock_path, false);
        PrivateCleanupActionClaimGuard claim(*lock);
        if (!claim.acquired()) {
            return {
                .result = private_cleanup_action_busy(),
                .observed = std::nullopt,
            };
        }
        lock->require_stable();
        auto captured = capture_private_handoff_publication_prefix_v1_locked(
            paths, *lock, expected_directory_identity);
        if (!captured.retained) {
            return {
                .result = captured.result,
                .observed = std::nullopt,
            };
        }
        auto state = std::make_shared<PrivateHandoffPublicationObservedPermitV1::State>(
            paths, expected_directory_identity, std::move(*captured.retained));
        // All allocation, path copying, and retained-prefix construction
        // finishes before ownership of the BaseLock changes. The following
        // shared_ptr move and guard disarm are noexcept, so the guard can never
        // retain a pointer to a destroyed lock.
        state->lock = std::move(lock);
        claim.transfer_to_permit();
        return {
            .result = captured.result,
            .observed = PrivateHandoffPublicationObservedPermitV1(std::move(state)),
        };
#endif
    } catch (const Failure& failure) {
        return {
            .result = resume_failure_result(failure),
            .observed = std::nullopt,
        };
    } catch (const std::bad_alloc&) {
        return {
            .result = resume_unexpected_result(std::make_error_code(std::errc::not_enough_memory)),
            .observed = std::nullopt,
        };
    } catch (const std::system_error& error) {
        return {
            .result = resume_unexpected_result(error.code()),
            .observed = std::nullopt,
        };
    } catch (...) {
        return {
            .result = resume_unexpected_result(),
            .observed = std::nullopt,
        };
    }
}

PrivateHandoffPublicationResumeValidationV1 validate_private_handoff_publication_resume_v1(
    PrivateHandoffPublicationObservedPermitV1&& observed,
    PrivateHandoffPublicationTypedValidatorV1&& validator) noexcept {
    auto state = std::move(observed.state_);
    const auto typed_validate = std::exchange(validator.validate_, nullptr);
    void* const typed_context = std::exchange(validator.context_, nullptr);
    const auto typed_creator_process_id = std::exchange(validator.creator_process_id_, 0);
    try {
        if (!state || !state->lock ||
            state->phase != PrivateHandoffPublicationObservedPermitV1::State::Phase::Observed ||
            state->creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
            typed_validate == nullptr ||
            typed_creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id())) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        auto& lock = *state->lock;
        lock.require_stable();
#if defined(__APPLE__)
        auto current = capture_private_handoff_publication_prefix_v1_locked(
            state->paths, lock, state->expected_directory_identity);
        if (!current.retained) {
            return {
                .result = current.result,
                .permit = std::nullopt,
            };
        }
        if (current.retained->witness != state->prefix.witness) {
            return {
                .result = resume_foreign_replacement(),
                .permit = std::nullopt,
            };
        }
        const auto typed_witness = current.retained->witness;
        const bool typed_valid = invoke_with_stable_base_lock(
            lock, [&] { return typed_validate(typed_witness, typed_context); });
        if (!typed_valid) {
            return {
                .result = resume_foreign_replacement(),
                .permit = std::nullopt,
            };
        }
        auto after_typed = capture_private_handoff_publication_prefix_v1_locked(
            state->paths, lock, state->expected_directory_identity);
        if (!after_typed.retained || after_typed.retained->witness != typed_witness) {
            return {
                .result = after_typed.retained ? resume_foreign_replacement() : after_typed.result,
                .permit = std::nullopt,
            };
        }
        state->prefix = std::move(*after_typed.retained);
        state->phase = PrivateHandoffPublicationObservedPermitV1::State::Phase::Validated;
        return {
            .result = after_typed.result,
            .permit = PrivateHandoffPublicationValidatedPermitV1(std::move(state)),
        };
#else
        (void)validator;
        (void)typed_context;
        return {
            .result =
                {
                    .status = OOCCleanupStatus::PlatformUnsupported,
                    .stage = OOCCleanupStage::None,
                    .native_error = std::make_error_code(std::errc::operation_not_supported),
                },
            .permit = std::nullopt,
        };
#endif
    } catch (const Failure& failure) {
        return {
            .result = resume_failure_result(failure),
            .permit = std::nullopt,
        };
    } catch (const std::bad_alloc&) {
        return {
            .result = resume_unexpected_result(std::make_error_code(std::errc::not_enough_memory)),
            .permit = std::nullopt,
        };
    } catch (const std::system_error& error) {
        return {
            .result = resume_unexpected_result(error.code()),
            .permit = std::nullopt,
        };
    } catch (...) {
        return {
            .result = resume_unexpected_result(),
            .permit = std::nullopt,
        };
    }
}

PrivateHandoffPublicationResumeRevalidationV1 revalidate_private_handoff_publication_resume_v1(
    const PrivateHandoffPublicationValidatedPermitV1& permit) noexcept {
    try {
        if (!permit.state_ || !permit.state_->lock ||
            permit.state_->phase !=
                PrivateHandoffPublicationObservedPermitV1::State::Phase::Validated ||
            permit.state_->creator_process_id !=
                static_cast<std::uint64_t>(gnfs::util::process_id())) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
#if !defined(__APPLE__)
        return {
            .result =
                {
                    .status = OOCCleanupStatus::PlatformUnsupported,
                    .stage = OOCCleanupStage::None,
                    .native_error = std::make_error_code(std::errc::operation_not_supported),
                },
            .witness = std::nullopt,
        };
#else
        auto& lock = *permit.state_->lock;
        lock.require_stable();
        auto current = capture_private_handoff_publication_prefix_v1_locked(
            permit.state_->paths, lock, permit.state_->expected_directory_identity);
        if (!current.retained || current.retained->witness != permit.state_->prefix.witness) {
            return {
                .result = current.retained ? resume_foreign_replacement() : current.result,
                .witness = std::nullopt,
            };
        }
        return {
            .result = current.result,
            .witness = current.retained->witness,
        };
#endif
    } catch (const Failure& failure) {
        return {
            .result = resume_failure_result(failure),
            .witness = std::nullopt,
        };
    } catch (const std::bad_alloc&) {
        return {
            .result = resume_unexpected_result(std::make_error_code(std::errc::not_enough_memory)),
            .witness = std::nullopt,
        };
    } catch (const std::system_error& error) {
        return {
            .result = resume_unexpected_result(error.code()),
            .witness = std::nullopt,
        };
    } catch (...) {
        return {
            .result = resume_unexpected_result(),
            .witness = std::nullopt,
        };
    }
}

PrivateHandoffPublicationResumeResultV1 reconcile_private_handoff_publication_for_resume_v1(
    PrivateHandoffPublicationValidatedPermitV1& permit,
    PrivateHandoffPublicationResumeTestHooksV1 hooks) noexcept {
    auto* state = permit.state_.get();
    std::optional<PrivateHandoffPublicationPrefixWitnessV1> expected;
    try {
        if (!state || !state->lock ||
            state->phase != PrivateHandoffPublicationObservedPermitV1::State::Phase::Validated ||
            state->creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
            ((hooks.stop_after == nullptr && hooks.fail_before == nullptr) &&
             hooks.context != nullptr)) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        state->phase = PrivateHandoffPublicationObservedPermitV1::State::Phase::ConsumedNonTerminal;
        state->canonical_terminal.reset();
        expected = state->prefix.witness;
        auto& lock = *state->lock;
        lock.require_stable();
#if defined(__APPLE__)
        auto before = capture_private_handoff_publication_prefix_v1_locked(
            state->paths, lock, state->expected_directory_identity);
        if (!before.retained) {
            return resume_failed(before.result, expected);
        }
        if (before.retained->witness != *expected) {
            return resume_failed(resume_foreign_replacement(), expected);
        }

        if (hooks.stop_after != nullptr) {
            const bool interrupted = invoke_with_stable_base_lock(lock, [&] {
                return hooks.stop_after(
                    PrivateHandoffPublicationResumeObservationPointV1::AfterExpectedPrefixValidated,
                    hooks.context);
            });
            if (interrupted) {
                return resume_failed(resume_interrupted_result(), expected);
            }
        }

        auto current = capture_private_handoff_publication_prefix_v1_locked(
            state->paths, lock, state->expected_directory_identity);
        if (!current.retained) {
            return resume_failed(current.result, expected);
        }
        if (current.retained->witness != *expected) {
            return resume_failed(resume_foreign_replacement(), expected);
        }

        auto& retained = *current.retained;
        auto prefix_state = retained.witness.state;
        if (prefix_state == PrivateHandoffPublicationPrefixStateV1::PendingOnly) {
            if (!retained.handoff || !retained.handoff->directory || !retained.handoff->pending ||
                !retained.handoff->pending->snapshot || !retained.rollback_parent ||
                !retained.generation.owned || !retained.generation.reserved) {
                fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                     protocol_error());
            }
            invoke_with_stable_base_lock(lock, [&] {
                move_exact_pending_to_rollback(
                    *retained.handoff->directory, *retained.rollback_parent,
                    state->paths.private_handoff_pending_path.filename(),
                    state->paths.private_handoff_rollback_path.filename(),
                    *retained.handoff->pending->snapshot);
            });

            if (hooks.fail_before != nullptr && invoke_with_stable_base_lock(lock, [&] {
                    return hooks.fail_before(PrivateHandoffPublicationResumeObservationPointV1::
                                                 BeforePendingRollbackSourceDirectorySync,
                                             hooks.context);
                })) {
                return resume_failed(
                    OOCCleanupResult{
                        .status = OOCCleanupStatus::DurabilityFailure,
                        .stage = OOCCleanupStage::None,
                        .native_error = std::make_error_code(std::errc::io_error),
                    },
                    expected);
            }
            invoke_with_stable_base_lock(
                lock, [&] { sync_resume_directory_handle(*retained.handoff->directory); });
            retained.handoff->directory->require_private_policy();
            if (hooks.stop_after != nullptr) {
                const bool interrupted = invoke_with_stable_base_lock(lock, [&] {
                    return hooks.stop_after(PrivateHandoffPublicationResumeObservationPointV1::
                                                AfterPendingRollbackSourceDirectoryDurable,
                                            hooks.context);
                });
                if (interrupted) {
                    return resume_failed(resume_interrupted_result(), expected);
                }
            }

            if (hooks.fail_before != nullptr && invoke_with_stable_base_lock(lock, [&] {
                    return hooks.fail_before(PrivateHandoffPublicationResumeObservationPointV1::
                                                 BeforePendingRollbackDestinationDirectorySync,
                                             hooks.context);
                })) {
                return resume_failed(
                    OOCCleanupResult{
                        .status = OOCCleanupStatus::DurabilityFailure,
                        .stage = OOCCleanupStage::None,
                        .native_error = std::make_error_code(std::errc::io_error),
                    },
                    expected);
            }
            invoke_with_stable_base_lock(
                lock, [&] { sync_resume_directory_handle(*retained.rollback_parent); });
            if (hooks.stop_after != nullptr) {
                const bool interrupted = invoke_with_stable_base_lock(lock, [&] {
                    return hooks.stop_after(PrivateHandoffPublicationResumeObservationPointV1::
                                                AfterPendingRollbackDestinationDirectoryDurable,
                                            hooks.context);
                });
                if (interrupted) {
                    return resume_failed(resume_interrupted_result(), expected);
                }
            }

            auto armed = capture_private_handoff_publication_prefix_v1_locked(
                state->paths, lock, state->expected_directory_identity);
            auto expected_rollback = *expected;
            expected_rollback.state = PrivateHandoffPublicationPrefixStateV1::PendingRollback;
            expected_rollback.rollback_snapshot = expected_rollback.pending_snapshot;
            expected_rollback.pending_snapshot.reset();
            if (!armed.retained || armed.retained->witness != expected_rollback) {
                return resume_failed(armed.retained ? resume_foreign_replacement() : armed.result,
                                     expected);
            }
            current = std::move(armed);
            prefix_state = PrivateHandoffPublicationPrefixStateV1::PendingRollback;
        }

        if (prefix_state == PrivateHandoffPublicationPrefixStateV1::PendingRollback) {
            auto& rollback_retained = *current.retained;
            if (!rollback_retained.rollback_parent || !rollback_retained.rollback ||
                !rollback_retained.rollback->snapshot ||
                !rollback_retained.witness.rollback_snapshot) {
                fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                     protocol_error());
            }
            OOCCleanupResult recovered = private_lease_completed();
            if (rollback_retained.generation.owned) {
                PrivateHandoffLeaseRecoveryObservationAdapterV1 lease_adapter{
                    .outer = &hooks,
                    .paths = &state->paths,
                    .lock = &lock,
                    .expected_directory_identity = &state->expected_directory_identity,
                    .initial = &rollback_retained.witness,
                };
                recovered = recover_private_handoff_rollback_generation_locked(
                    state->paths, lock, rollback_retained.generation.parent_identity,
                    *rollback_retained.generation.owned, rollback_retained.generation.reserved,
                    OOCPrivateLeaseTestHooks{
                        .stop_after = observe_private_handoff_lease_recovery,
                        .context = &lease_adapter,
                    });
                if (lease_adapter.exact_failure) {
                    return resume_failed(*lease_adapter.exact_failure, expected);
                }
                if (lease_adapter.unknown_point) {
                    return resume_failed(resume_unexpected_result(protocol_error()), expected);
                }
                if (!recovered.completed()) {
                    return resume_failed(recovered, expected);
                }
            }
            auto terminal_rollback = capture_private_handoff_publication_prefix_v1_locked(
                state->paths, lock, state->expected_directory_identity);
            if (!terminal_rollback.retained ||
                terminal_rollback.retained->witness.state !=
                    PrivateHandoffPublicationPrefixStateV1::PendingRollback ||
                terminal_rollback.retained->witness.record != rollback_retained.witness.record ||
                terminal_rollback.retained->witness.owned ||
                terminal_rollback.retained->witness.reserved ||
                terminal_rollback.retained->witness.owner ||
                !terminal_rollback.retained->rollback_parent ||
                !terminal_rollback.retained->rollback ||
                !terminal_rollback.retained->rollback->snapshot) {
                return resume_failed(terminal_rollback.retained ? resume_foreign_replacement()
                                                                : terminal_rollback.result,
                                     expected);
            }
            if (hooks.stop_after != nullptr) {
                const bool interrupted = invoke_with_stable_base_lock(lock, [&] {
                    return hooks.stop_after(PrivateHandoffPublicationResumeObservationPointV1::
                                                BeforePendingRollbackTombstoneRemovalValidated,
                                            hooks.context);
                });
                if (interrupted) {
                    return resume_failed(resume_interrupted_result(), expected);
                }
            }
            auto removal_ready = capture_private_handoff_publication_prefix_v1_locked(
                state->paths, lock, state->expected_directory_identity);
            if (!removal_ready.retained ||
                removal_ready.retained->witness != terminal_rollback.retained->witness ||
                !removal_ready.retained->rollback_parent || !removal_ready.retained->rollback ||
                !removal_ready.retained->rollback->snapshot) {
                return resume_failed(removal_ready.retained ? resume_foreign_replacement()
                                                            : removal_ready.result,
                                     expected);
            }
            invoke_with_stable_base_lock(lock, [&] {
                remove_exact_private_handoff_pending(
                    *removal_ready.retained->rollback_parent,
                    *removal_ready.retained->rollback->snapshot,
                    state->paths.private_handoff_rollback_path.filename());
            });
            if (hooks.stop_after != nullptr) {
                const bool interrupted = invoke_with_stable_base_lock(lock, [&] {
                    return hooks.stop_after(PrivateHandoffPublicationResumeObservationPointV1::
                                                AfterPendingRollbackTombstoneRemovedDurable,
                                            hooks.context);
                });
                if (interrupted) {
                    return resume_failed(resume_interrupted_result(), expected);
                }
            }
            auto absent = capture_private_handoff_publication_prefix_v1_locked(
                state->paths, lock, state->expected_directory_identity);
            if (absent.retained || absent.result.status != OOCCleanupStatus::NoTransaction ||
                absent.result.stage != OOCCleanupStage::None || absent.result.native_error) {
                return resume_failed(absent.retained ? resume_foreign_replacement() : absent.result,
                                     expected);
            }
            return {
                .result = recovered,
                .disposition = PrivateHandoffPublicationResumeDispositionV1::PendingRolledBack,
                .expected_prefix = std::move(expected),
                .terminal_prefix = std::nullopt,
            };
        }

        if (prefix_state != PrivateHandoffPublicationPrefixStateV1::Canonical &&
            prefix_state != PrivateHandoffPublicationPrefixStateV1::IdenticalDual) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        const bool has_duplicate =
            prefix_state == PrivateHandoffPublicationPrefixStateV1::IdenticalDual;
        const bool has_reserved = retained.generation.reserved.has_value();
        if (!has_duplicate && !has_reserved) {
            if (!retained.handoff) {
                fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                     protocol_error());
            }
            return commit_canonical_publication_terminal(
                *state,
                PrivateHandoffPublicationResumeResultV1{
                    .result = retained.handoff->inspection.result,
                    .disposition = PrivateHandoffPublicationResumeDispositionV1::CanonicalTerminal,
                    .expected_prefix = expected,
                    .terminal_prefix = expected,
                });
        }
        if (!retained.handoff || !retained.handoff->directory || !retained.handoff->canonical ||
            !retained.handoff->canonical->snapshot || !retained.witness.canonical_snapshot) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }

        const auto confirmed = invoke_with_stable_base_lock(lock, [&] {
            return util::durable_immutable_record::publish_at(
                retained.handoff->directory->native_handle(),
                state->paths.private_handoff_pending_path.filename(),
                state->paths.private_handoff_path.filename(), retained.handoff->canonical->bytes);
        });
        if (!confirmed.is_durable() || !confirmed.canonical_snapshot()) {
            const auto status =
                confirmed.status() ==
                        util::durable_immutable_record::RecordPublishStatus::platform_unsupported
                    ? OOCCleanupStatus::PlatformUnsupported
                    : OOCCleanupStatus::DurabilityFailure;
            return resume_failed(
                OOCCleanupResult{
                    .status = status,
                    .stage = OOCCleanupStage::None,
                    .native_error = confirmed.native_error(),
                },
                expected);
        }
        if (*confirmed.canonical_snapshot() != *retained.witness.canonical_snapshot) {
            return resume_failed(resume_foreign_replacement(), expected);
        }
        retained.handoff->directory->require_stable();
        retained.handoff->directory->require_private_policy();

        auto expected_canonical = *expected;
        expected_canonical.state = PrivateHandoffPublicationPrefixStateV1::Canonical;
        expected_canonical.pending_snapshot.reset();
        expected_canonical.canonical_snapshot = confirmed.canonical_snapshot();
        if (hooks.stop_after != nullptr) {
            const bool interrupted = invoke_with_stable_base_lock(lock, [&] {
                return hooks.stop_after(PrivateHandoffPublicationResumeObservationPointV1::
                                            AfterCanonicalConfirmedDurable,
                                        hooks.context);
            });
            if (interrupted) {
                return resume_failed(resume_interrupted_result(), expected);
            }
        }

        retained.handoff->directory->require_stable();
        retained.handoff->directory->require_private_policy();
        auto post_canonical = capture_private_handoff_publication_prefix_v1_locked(
            state->paths, lock, state->expected_directory_identity);
        if (!post_canonical.retained) {
            return resume_failed(post_canonical.result, expected);
        }
        if (post_canonical.retained->witness != expected_canonical) {
            return resume_failed(resume_foreign_replacement(), expected);
        }

        if (post_canonical.retained->generation.reserved) {
            invoke_with_stable_base_lock(lock, [&] {
                remove_private_lease_marker_durable(
                    state->paths.lease_reserved_path,
                    post_canonical.retained->generation.reserved->record,
                    post_canonical.retained->generation.reserved->identity);
            });
            if (hooks.stop_after != nullptr) {
                const bool interrupted = invoke_with_stable_base_lock(lock, [&] {
                    return hooks.stop_after(PrivateHandoffPublicationResumeObservationPointV1::
                                                AfterReservedRevokedDurable,
                                            hooks.context);
                });
                if (interrupted) {
                    return resume_failed(resume_interrupted_result(), expected);
                }
            }
        }

        auto terminal = capture_private_handoff_publication_prefix_v1_locked(
            state->paths, lock, state->expected_directory_identity);
        if (!terminal.retained) {
            return resume_failed(terminal.result, expected);
        }
        auto expected_terminal = expected_canonical;
        expected_terminal.reserved.reset();
        if (terminal.retained->witness != expected_terminal) {
            return resume_failed(resume_foreign_replacement(), expected);
        }
        return commit_canonical_publication_terminal(
            *state,
            PrivateHandoffPublicationResumeResultV1{
                .result = terminal.result,
                .disposition = PrivateHandoffPublicationResumeDispositionV1::CanonicalConverged,
                .expected_prefix = std::move(expected),
                .terminal_prefix = std::move(terminal.retained->witness),
            });
#else
        (void)hooks;
        return resume_failed(
            OOCCleanupResult{
                .status = OOCCleanupStatus::PlatformUnsupported,
                .stage = OOCCleanupStage::None,
                .native_error = std::make_error_code(std::errc::operation_not_supported),
            },
            expected);
#endif
    } catch (const Failure& failure) {
        return resume_failed(resume_failure_result(failure), std::move(expected));
    } catch (const std::bad_alloc&) {
        return resume_failed(
            resume_unexpected_result(std::make_error_code(std::errc::not_enough_memory)),
            std::move(expected));
    } catch (const std::system_error& error) {
        return resume_failed(resume_unexpected_result(error.code()), std::move(expected));
    } catch (...) {
        return resume_failed(resume_unexpected_result(), std::move(expected));
    }
}

OOCPrivateHandoffAdoptionResult adopt_consumed_canonical_private_handoff_publication_v1(
    PrivateHandoffPublicationValidatedPermitV1&& permit,
    OOCPrivateHandoffAdoptionTestHooks hooks) noexcept {
    auto state = std::move(permit.state_);
    try {
        if (!state || !state->lock ||
            state->phase !=
                PrivateHandoffPublicationObservedPermitV1::State::Phase::ConsumedCanonical ||
            state->creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id())) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        if (!state->canonical_terminal ||
            !canonical_publication_terminal_shape_valid(*state->canonical_terminal) ||
            state->expected_directory_identity != state->canonical_terminal->directory_identity ||
            state->lock->identity() != state->canonical_terminal->lock_identity) {
            fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
        }

        auto owner = std::move(state);
        auto live_lock = std::shared_ptr<BaseLock>(owner, owner->lock.get());
        auto terminal = std::shared_ptr<const PrivateHandoffPublicationPrefixWitnessV1>(
            owner, std::addressof(*owner->canonical_terminal));
        OOCPrivateHandoffConsumedPublicationBaseLockV1 authority(
            std::move(live_lock), std::move(terminal), owner->creator_process_id);
        return adopt_private_handoff_with_consumed_publication_base_lock_v1(
            owner->paths.base_path, std::move(authority), hooks);
    } catch (const Failure& failure) {
        return consumed_publication_adoption_failure(failure.status, failure.error);
    } catch (const std::bad_alloc&) {
        return consumed_publication_adoption_failure(
            OOCCleanupStatus::UnexpectedFailure,
            std::make_error_code(std::errc::not_enough_memory));
    } catch (const std::system_error& error) {
        return consumed_publication_adoption_failure(OOCCleanupStatus::UnexpectedFailure,
                                                     error.code());
    } catch (...) {
        return consumed_publication_adoption_failure(OOCCleanupStatus::UnexpectedFailure);
    }
}

PrivateHandoffPublicationReaderAdoptionResultV1 adopt_consumed_canonical_private_handoff_reader_v1(
    PrivateHandoffPublicationValidatedPermitV1& permit,
    PrivateHandoffPublicationAdoptionRevalidatorV1&& revalidator,
    OOCPrivateHandoffAdoptionTestHooks hooks) noexcept {
    auto retained_revalidator = std::move(revalidator);
    auto owner = permit.state_;
    try {
        const auto current_process_id = static_cast<std::uint64_t>(gnfs::util::process_id());
        if (!owner || !owner->lock ||
            owner->phase !=
                PrivateHandoffPublicationObservedPermitV1::State::Phase::ConsumedCanonical ||
            owner->creator_process_id != current_process_id ||
            retained_revalidator.validate_ == nullptr ||
            retained_revalidator.creator_process_id_ != current_process_id) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        if (!owner->canonical_terminal ||
            !canonical_publication_terminal_shape_valid(*owner->canonical_terminal) ||
            owner->expected_directory_identity != owner->canonical_terminal->directory_identity ||
            owner->lock->identity() != owner->canonical_terminal->lock_identity) {
            fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
        }

        auto live_lock = std::shared_ptr<BaseLock>(owner, owner->lock.get());
        auto terminal = std::shared_ptr<const PrivateHandoffPublicationPrefixWitnessV1>(
            owner, std::addressof(*owner->canonical_terminal));
        OOCPrivateHandoffConsumedPublicationBaseLockV1 authority(
            std::move(live_lock), std::move(terminal), owner->creator_process_id);
        auto adopted = adopt_private_handoff_with_consumed_publication_base_lock_v1(
            owner->paths.base_path, std::move(authority), retained_revalidator, hooks);
        if (!adopted.adopted() || !adopted.adoption) {
            return PrivateHandoffPublicationReaderAdoptionResultV1(adopted.result, adopted.state,
                                                                   nullptr);
        }

        auto reader = std::make_unique<OOCPrivateHandoffReader>(std::move(*adopted.adoption));
        if (!reader->valid()) {
            fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
        }

#if !defined(__APPLE__)
        return consumed_publication_reader_adoption_failure(
            OOCCleanupStatus::PlatformUnsupported,
            std::make_error_code(std::errc::operation_not_supported));
#else
        const auto require_terminal = [&] {
            owner->lock->require_stable();
            auto current = capture_private_handoff_publication_prefix_v1_locked(
                owner->paths, *owner->lock, owner->expected_directory_identity);
            if (!current.retained || current.retained->witness != *owner->canonical_terminal) {
                const auto result =
                    current.retained ? resume_foreign_replacement() : current.result;
                fail(result.status, OOCCleanupStage::None,
                     result.native_error ? result.native_error : protocol_error());
            }
        };
        require_terminal();
        const bool aggregate_valid = invoke_with_stable_base_lock(*owner->lock, [&] {
            return retained_revalidator.validate_(reader.get(), retained_revalidator.context_);
        });
        if (!aggregate_valid) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        require_terminal();
        if (!reader->valid()) {
            fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
        }

        permit.state_.reset();
        return PrivateHandoffPublicationReaderAdoptionResultV1(adopted.result, adopted.state,
                                                               std::move(reader));
#endif
    } catch (const Failure& failure) {
        return consumed_publication_reader_adoption_failure(failure.status, failure.error);
    } catch (const std::bad_alloc&) {
        return consumed_publication_reader_adoption_failure(
            OOCCleanupStatus::UnexpectedFailure,
            std::make_error_code(std::errc::not_enough_memory));
    } catch (const std::system_error& error) {
        return consumed_publication_reader_adoption_failure(OOCCleanupStatus::UnexpectedFailure,
                                                            error.code());
    } catch (...) {
        return consumed_publication_reader_adoption_failure(OOCCleanupStatus::UnexpectedFailure);
    }
}

PrivateCleanupActionAdmission admit_private_cleanup_action_locked(const OOCCleanupPaths& paths,
                                                                  std::shared_ptr<BaseLock> lock,
                                                                  PrivateNamespaceAction action) {
    if (paths.private_directory.empty() || !lock || !lock->matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    PrivateCleanupActionClaimGuard claim(*lock);
    if (!claim.acquired()) {
        return PrivateCleanupActionAdmission(private_cleanup_action_busy());
    }
    lock->require_stable();
    auto witness = observe_private_cleanup_union_locked(paths, *lock);
    const auto decision = decide_private_namespace_action(witness.raw, action);
    lock->require_stable();
    if (const auto blocked = private_cleanup_union_blocked_result(decision)) {
        return PrivateCleanupActionAdmission(*blocked);
    }
    auto state =
        std::unique_ptr<PrivateCleanupActionPermit::State>(new PrivateCleanupActionPermit::State(
            paths, std::move(lock), decision, std::move(witness)));
    claim.transfer_to_permit();
    return PrivateCleanupActionAdmission(PrivateCleanupActionPermit(std::move(state)));
}

PrivateCleanupActionAdmission admit_private_lease_cleanup_handoff_locked(
    const OOCCleanupPaths& paths, std::shared_ptr<BaseLock> lock, const OOCCleanupRequest& request,
    const OwnershipProof& ownership_proof, const std::array<std::uint64_t, 2>& expected_lease_id,
    const std::array<std::uint64_t, 3>& expected_directory_identity,
    const std::array<std::uint64_t, 3>& expected_owner_identity,
    const std::array<std::uint64_t, 3>& expected_owned_identity) {
    if (paths.private_directory.empty() || !lock || !lock->matches(paths.lock_path) ||
        request.base_path != paths.base_path || request.store_id == 0 || !request.exact ||
        !expectation_is_well_formed(*request.exact) ||
        ownership_proof.base_path != paths.base_path ||
        ownership_proof.store_id != request.store_id) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    PrivateCleanupActionClaimGuard claim(*lock);
    if (!claim.acquired()) {
        return PrivateCleanupActionAdmission(private_cleanup_action_busy());
    }
    lock->require_stable();
    auto witness = observe_private_cleanup_union_locked(paths, *lock);
    const auto decision = decide_private_namespace_action(
        witness.raw, PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff);
    lock->require_stable();
    if (const auto blocked = private_cleanup_union_blocked_result(decision)) {
        return PrivateCleanupActionAdmission(*blocked);
    }

    auto generation = capture_private_lease_cleanup_handoff_generation_locked(
        paths, *lock, expected_lease_id, expected_directory_identity, expected_owner_identity,
        expected_owned_identity);
    const auto intent = capture_source_pair(paths, request.store_id);
    if (!request_matches_intent(request, intent) ||
        !ownership_proof_matches(ownership_proof, paths, intent)) {
        fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None, protocol_error());
    }
    require_source_pair_unchanged(paths, intent);
    std::optional<FileIdentity> original_canonical_identity;
    const auto original_canonical = inspect_file(paths.intent_path, MARKER_BYTES, true);
    if (original_canonical.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, original_canonical.error);
    }
    if (original_canonical.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    if (original_canonical.kind == InspectKind::Present) {
        if (parse_marker(original_canonical.bytes, INTENT_MAGIC) != intent) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
        original_canonical_identity = original_canonical.identity;
    }
    lock->require_stable();

    auto state =
        std::unique_ptr<PrivateCleanupActionPermit::State>(new PrivateCleanupActionPermit::State(
            paths, std::move(lock), decision, std::move(witness)));
    claim.transfer_to_permit();
    state->publication_generation = std::move(generation);
    state->publication_intent = intent;
    state->publication_original_canonical_identity = original_canonical_identity;
    return PrivateCleanupActionAdmission(PrivateCleanupActionPermit(std::move(state)));
}

PrivateLeaseRemovalAdmission
admit_private_lease_removal_locked(const OOCCleanupPaths& paths, std::shared_ptr<BaseLock> lock,
                                   const std::array<std::uint64_t, 2>& expected_lease_id,
                                   const std::array<std::uint64_t, 3>& expected_directory_identity,
                                   const std::array<std::uint64_t, 3>& expected_owner_identity,
                                   const std::array<std::uint64_t, 3>& expected_owned_identity) {
    if (paths.private_directory.empty() || !lock || !lock->matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    PrivateCleanupActionClaimGuard claim(*lock);
    if (!claim.acquired()) {
        return PrivateLeaseRemovalAdmission(private_cleanup_action_busy());
    }
    lock->require_stable();
    auto witness = observe_private_cleanup_union_locked(paths, *lock);
    const auto decision =
        decide_private_namespace_action(witness.raw, PrivateNamespaceAction::RemovePrivateLease);
    lock->require_stable();
    if (const auto blocked = private_cleanup_union_blocked_result(decision)) {
        return PrivateLeaseRemovalAdmission(*blocked);
    }

    auto generation = capture_private_lease_removal_generation_locked(
        paths, *lock, expected_lease_id, expected_directory_identity, expected_owner_identity,
        expected_owned_identity);
    lock->require_stable();
    auto state =
        std::unique_ptr<PrivateCleanupActionPermit::State>(new PrivateCleanupActionPermit::State(
            paths, std::move(lock), decision, std::move(witness)));
    claim.transfer_to_permit();
    return PrivateLeaseRemovalAdmission(PrivateCleanupActionPermit(std::move(state)),
                                        std::move(generation));
}

const BaseLock& begin_private_cleanup_action(PrivateCleanupActionPermit& permit,
                                             const OOCCleanupPaths& paths,
                                             PrivateNamespaceAction expected_action) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    if (state.action_started) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.action_started = true;
    if (state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        state.decision.action != expected_action ||
        state.decision.action == PrivateNamespaceAction::Count ||
        !decision_delegates_existing_runtime(state.decision) ||
        !private_cleanup_paths_equal(state.paths, paths) || !state.lock ||
        !state.lock->matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.lock->require_stable();
    return *state.lock;
}

void bind_private_lease_removal_generation(PrivateCleanupActionPermit& permit,
                                           const PrivateLeaseRemovalGenerationProof& proof) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    if (state.removal_generation_binding_attempted) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.removal_generation_binding_attempted = true;
    const auto prior_handoff_state = state.handoff_state;
    state.handoff_state = PrivateHandoffConsumptionState::Failed;
    if (!state.action_started || prior_handoff_state != PrivateHandoffConsumptionState::Fresh ||
        state.decision.action != PrivateNamespaceAction::RemovePrivateLease ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    const auto current = capture_private_lease_removal_generation_locked(
        state.paths, *state.lock, proof.expected_lease_id, proof.expected_directory_identity,
        proof.expected_owner_identity, proof.expected_owned_identity);
    if (current != proof) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    const bool retained_handoff = std::any_of(
        state.witness.raw.handoff_markers.begin(), state.witness.raw.handoff_markers.end(),
        [](const auto marker) { return marker == PrivateHandoffLeafObservationKind::Exact; });
    if (retained_handoff && (!proof.owned || !proof.owner_present)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    state.removal_generation = proof;
    state.handoff_state = PrivateHandoffConsumptionState::Fresh;
}

OOCPrivateHandoffInspectResult
reconcile_private_handoff_from_permit(PrivateCleanupActionPermit& permit,
                                      PrivateNamespaceAction expected_action) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    if (!state.action_started || state.handoff_state != PrivateHandoffConsumptionState::Fresh) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.handoff_state = PrivateHandoffConsumptionState::Failed;
    if (state.decision.action != expected_action ||
        (expected_action != PrivateNamespaceAction::RecoverPrivateLease &&
         expected_action != PrivateNamespaceAction::RemovePrivateLease) ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.lock->require_stable();
    if (expected_action == PrivateNamespaceAction::RemovePrivateLease) {
        require_private_lease_removal_generation_unchanged(state);
    }
    require_private_cleanup_action_witness_unchanged(state);
    state.handoff_state = expected_action == PrivateNamespaceAction::RecoverPrivateLease
                              ? PrivateHandoffConsumptionState::RecoverConsumed
                              : PrivateHandoffConsumptionState::RemoveConsumed;

#if defined(__APPLE__)
    auto& witness = state.witness;
    if (!witness.handoff_classification) {
        return handoff_none();
    }
    const auto& classified = *witness.handoff_classification;
    if (classified.inspection.state == OOCPrivateHandoffState::Canonical) {
        if (!witness.directory || !witness.canonical_handoff ||
            !witness.canonical_handoff->record || !witness.canonical_handoff->snapshot) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        const auto confirmed = invoke_with_stable_base_lock(*state.lock, [&] {
            return util::durable_immutable_record::publish_at(
                witness.directory->native_handle(),
                state.paths.private_handoff_pending_path.filename(),
                state.paths.private_handoff_path.filename(), witness.canonical_handoff->bytes);
        });
        if (!confirmed.is_durable() || !confirmed.canonical_snapshot()) {
            const auto status =
                confirmed.status() ==
                        util::durable_immutable_record::RecordPublishStatus::platform_unsupported
                    ? OOCCleanupStatus::PlatformUnsupported
                    : OOCCleanupStatus::DurabilityFailure;
            return handoff_failure(status, OOCPrivateHandoffState::TaintedPreserved,
                                   confirmed.native_error());
        }
        witness.directory->require_stable();
        witness.directory->require_private_policy();
        state.lock->require_stable();
        return handoff_present(*witness.canonical_handoff->record, *confirmed.canonical_snapshot());
    }

    if (classified.inspection.state == OOCPrivateHandoffState::PendingOnly &&
        classified.pending_is_preactive) {
        if (!witness.directory || !witness.pending_handoff || !witness.pending_handoff->snapshot) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        invoke_with_stable_base_lock(*state.lock, [&] {
            remove_exact_private_handoff_pending(
                *witness.directory, *witness.pending_handoff->snapshot,
                state.paths.private_handoff_pending_path.filename());
        });
        witness.directory->require_private_policy();
        state.lock->require_stable();
        return handoff_none();
    }
    return classified.inspection;
#else
    return handoff_none();
#endif
}

OOCPrivateHandoffInspectResult
inspect_private_handoff_from_permit(PrivateCleanupActionPermit& permit) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    if (!state.action_started || state.handoff_state != PrivateHandoffConsumptionState::Fresh) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.handoff_state = PrivateHandoffConsumptionState::Failed;
    if (state.decision.action != PrivateNamespaceAction::RunLegacyCleanup ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.lock->require_stable();
    require_private_cleanup_action_witness_unchanged(state);

#if defined(__APPLE__)
    if (state.witness.handoff_classification) {
        const auto inspection = state.witness.handoff_classification->inspection;
        if (inspection.state != OOCPrivateHandoffState::None) {
            return inspection;
        }
    }
#endif
    state.handoff_state = PrivateHandoffConsumptionState::LegacyObserved;
    return handoff_none();
}

OOCPrivateHandoffInspectResult
inspect_private_handoff_for_action_from_permit(PrivateCleanupActionPermit& permit,
                                               PrivateNamespaceAction expected_action) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    if (!state.action_started || state.handoff_state != PrivateHandoffConsumptionState::Fresh) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.handoff_state = PrivateHandoffConsumptionState::Failed;
    const bool lifecycle_action = expected_action == PrivateNamespaceAction::ReservePrivateLease ||
                                  expected_action == PrivateNamespaceAction::ValidateFreshWriter ||
                                  expected_action == PrivateNamespaceAction::ActivateFreshLease;
    if (!lifecycle_action || state.decision.action != expected_action ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.lock->require_stable();
    require_private_cleanup_action_witness_unchanged(state);

#if defined(__APPLE__)
    if (state.witness.handoff_classification) {
        const auto inspection = state.witness.handoff_classification->inspection;
        if (inspection.state != OOCPrivateHandoffState::None) {
            return inspection;
        }
    }
#endif
    state.handoff_state = PrivateHandoffConsumptionState::LifecycleObserved;
    return handoff_none();
}

namespace {

[[nodiscard]] bool
private_lifecycle_union_is_clear(const PrivateCleanupUnionRawObservation& raw) noexcept {
    return raw == PrivateCleanupUnionRawObservation{};
}

void require_private_lifecycle_common(PrivateCleanupActionPermit::State& state,
                                      PrivateNamespaceAction expected_action) {
    if (!state.action_started ||
        state.handoff_state != PrivateHandoffConsumptionState::LifecycleObserved ||
        state.decision.action != expected_action ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock || !state.lock->matches(state.paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.lock->require_stable();
}

void require_private_lifecycle_phase_witness(PrivateCleanupActionPermit::State& state) {
    if (!state.lifecycle_phase_witness || !state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    require_private_cleanup_witness_unchanged(state.paths, *state.lock,
                                              *state.lifecycle_phase_witness);
    state.lock->require_stable();
}

void record_private_lifecycle_successor(PrivateCleanupActionPermit::State& state) {
    if (!state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto successor = observe_private_cleanup_union_locked(state.paths, *state.lock);
    if (!private_lifecycle_union_is_clear(successor.raw)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    state.lock->require_stable();
    state.lifecycle_phase_witness = std::move(successor);
}

[[nodiscard]] LoadedPrivateLeaseMarker require_private_lease_marker_exact(
    const std::filesystem::path& path, const PrivateLeaseRecord& record,
    std::optional<std::array<std::uint64_t, 3>> identity = std::nullopt) {
    const auto loaded = load_optional_private_lease_marker(path);
    if (!loaded || loaded->record != record || (identity && loaded->identity != *identity)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    return *loaded;
}

void require_private_lease_marker_missing(const std::filesystem::path& path) {
    if (load_optional_private_lease_marker(path)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
}

void require_private_artifact_identity(const std::filesystem::path& path,
                                       const std::array<std::uint64_t, 3>& identity,
                                       std::optional<std::uint64_t> expected_size = std::nullopt) {
    const auto inspected = inspect_file(path, 0, false);
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (inspected.kind != InspectKind::Present || stable_identity(inspected.identity) != identity ||
        (expected_size && inspected.identity.size != *expected_size)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
}

void require_private_lease_generation_shape(const OOCCleanupPaths& paths,
                                            const PrivateLeaseRemovalGenerationProof& proof,
                                            bool require_reserved) {
    if (!proof.owned || !proof.final_directory_identity || proof.staging_directory_identity ||
        !proof.owner_present || proof.owned_pending || proof.reserved_pending ||
        proof.owned->record.lease_id != proof.expected_lease_id ||
        proof.owned->record.directory_identity != proof.expected_directory_identity ||
        proof.owned->record.owner_identity != proof.expected_owner_identity ||
        proof.owned->identity != proof.expected_owned_identity ||
        *proof.final_directory_identity != proof.expected_directory_identity ||
        (require_reserved && !proof.reserved) || (!require_reserved && proof.reserved)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    const auto entries = inspect_private_lease_preactive_entries(paths.private_directory, paths);
    if (!entries.owner) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
}

void require_private_reservation_authority_chain(PrivateCleanupActionPermit::State& state,
                                                 PrivateLeaseReservationPermitPhase phase) {
    const auto require_saved_marker =
        [&](const std::filesystem::path& canonical_path, const std::filesystem::path& pending_path,
            const std::optional<PrivateLeaseRecord>& record,
            const std::optional<std::array<std::uint64_t, 3>>& canonical_identity,
            const std::optional<std::array<std::uint64_t, 3>>& pending_identity,
            bool canonical_present, bool pending_present) {
            if (!record) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            if (canonical_present) {
                if (!canonical_identity) {
                    fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                         invalid_argument_error());
                }
                (void)require_private_lease_marker_exact(canonical_path, *record,
                                                         *canonical_identity);
            } else {
                require_private_lease_marker_missing(canonical_path);
            }
            if (pending_present) {
                if (!pending_identity) {
                    fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                         invalid_argument_error());
                }
                (void)require_private_lease_marker_exact(pending_path, *record, *pending_identity);
            } else {
                require_private_lease_marker_missing(pending_path);
            }
        };

    const bool reserved_pending =
        phase == PrivateLeaseReservationPermitPhase::ReservedPending ||
        phase == PrivateLeaseReservationPermitPhase::ReservedCanonicalAuthorized;
    const bool reserved_canonical =
        phase >= PrivateLeaseReservationPermitPhase::ReservedCanonical &&
        phase < PrivateLeaseReservationPermitPhase::Failed;
    require_saved_marker(state.paths.lease_reserved_path, state.paths.lease_reserved_pending_path,
                         state.reservation_reserved, state.reservation_reserved_identity,
                         state.reservation_reserved_pending_identity, reserved_canonical,
                         reserved_pending);

    const bool owner_started =
        phase >= PrivateLeaseReservationPermitPhase::OwnerPendingAuthorized &&
        phase < PrivateLeaseReservationPermitPhase::Failed;
    if (owner_started) {
        const bool owner_pending =
            phase == PrivateLeaseReservationPermitPhase::OwnerPending ||
            phase == PrivateLeaseReservationPermitPhase::OwnerCanonicalAuthorized;
        const bool owner_canonical = phase >= PrivateLeaseReservationPermitPhase::OwnerCanonical &&
                                     phase < PrivateLeaseReservationPermitPhase::Failed;
        const auto owner_directory = phase >= PrivateLeaseReservationPermitPhase::FinalDirectory
                                         ? state.paths.private_directory
                                         : state.reservation_staging_path;
        require_saved_marker(private_lease_owner_path(owner_directory),
                             private_lease_owner_pending_path(owner_directory),
                             state.reservation_owner, state.reservation_owner_identity,
                             state.reservation_owner_pending_identity, owner_canonical,
                             owner_pending);
    }

    const bool owned_started =
        phase >= PrivateLeaseReservationPermitPhase::OwnedPendingAuthorized &&
        phase < PrivateLeaseReservationPermitPhase::Failed;
    if (owned_started) {
        const bool owned_pending =
            phase == PrivateLeaseReservationPermitPhase::OwnedPending ||
            phase == PrivateLeaseReservationPermitPhase::OwnedCanonicalAuthorized;
        const bool owned_canonical = phase >= PrivateLeaseReservationPermitPhase::OwnedCanonical &&
                                     phase < PrivateLeaseReservationPermitPhase::Failed;
        require_saved_marker(state.paths.lease_owned_path, state.paths.lease_owned_pending_path,
                             state.reservation_owned, state.reservation_owned_identity,
                             state.reservation_owned_pending_identity, owned_canonical,
                             owned_pending);
    }
}

void require_private_reservation_phase_shape(PrivateCleanupActionPermit::State& state,
                                             PrivateLeaseReservationPermitPhase phase,
                                             bool require_prior_witness = true) {
    require_private_lifecycle_common(state, PrivateNamespaceAction::ReservePrivateLease);
    if (state.reservation_phase != phase || !state.reservation_reserved ||
        !state.reservation_parent_identity || state.reservation_staging_path.empty()) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    if (require_prior_witness) {
        require_private_lifecycle_phase_witness(state);
    }
    const bool staging_present = phase >= PrivateLeaseReservationPermitPhase::StagingDirectory &&
                                 phase < PrivateLeaseReservationPermitPhase::FinalDirectory;
    const bool final_present = phase >= PrivateLeaseReservationPermitPhase::FinalDirectory &&
                               phase < PrivateLeaseReservationPermitPhase::Failed;
    if (capture_directory_identity_locked(state.paths.private_directory.parent_path()) !=
            *state.reservation_parent_identity ||
        inspect_directory_identity_locked(state.reservation_staging_path) !=
            (staging_present ? state.reservation_directory_identity : std::nullopt) ||
        inspect_directory_identity_locked(state.paths.private_directory) !=
            (final_present ? state.reservation_directory_identity : std::nullopt)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    require_private_reservation_authority_chain(state, phase);
    if (staging_present) {
        const auto entries = inspect_private_lease_control_entries(state.reservation_staging_path);
        const bool expect_owner_pending =
            phase == PrivateLeaseReservationPermitPhase::OwnerPending ||
            phase == PrivateLeaseReservationPermitPhase::OwnerCanonicalAuthorized;
        const bool expect_owner = phase >= PrivateLeaseReservationPermitPhase::OwnerCanonical;
        if (entries.owner != expect_owner || entries.owner_pending != expect_owner_pending) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
    }
    if (final_present) {
        const auto entries = inspect_private_lease_control_entries(state.paths.private_directory);
        if (!entries.owner || entries.owner_pending) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
    }
    state.lock->require_stable();
}

enum class PrivateReservationMarkerRole : std::uint8_t {
    Reserved,
    Owner,
    Owned,
};

struct PrivateReservationMarkerBinding final {
    PrivateReservationMarkerRole role;
    PrivateLeaseReservationPermitPhase prior_phase;
    PrivateLeaseReservationPermitPhase pending_authorized_phase;
    PrivateLeaseReservationPermitPhase pending_phase;
    PrivateLeaseReservationPermitPhase canonical_authorized_phase;
    PrivateLeaseReservationPermitPhase canonical_phase;
    std::optional<PrivateLeaseRecord>* record = nullptr;
    std::optional<std::array<std::uint64_t, 3>>* pending_identity = nullptr;
    std::optional<std::array<std::uint64_t, 3>>* canonical_identity = nullptr;
};

[[nodiscard]] PrivateReservationMarkerBinding
reservation_marker_binding(PrivateCleanupActionPermit::State& state,
                           const std::filesystem::path& canonical_path,
                           const std::filesystem::path& pending_path) {
    if (canonical_path == state.paths.lease_reserved_path &&
        pending_path == state.paths.lease_reserved_pending_path) {
        return PrivateReservationMarkerBinding{
            .role = PrivateReservationMarkerRole::Reserved,
            .prior_phase = PrivateLeaseReservationPermitPhase::Empty,
            .pending_authorized_phase =
                PrivateLeaseReservationPermitPhase::ReservedPendingAuthorized,
            .pending_phase = PrivateLeaseReservationPermitPhase::ReservedPending,
            .canonical_authorized_phase =
                PrivateLeaseReservationPermitPhase::ReservedCanonicalAuthorized,
            .canonical_phase = PrivateLeaseReservationPermitPhase::ReservedCanonical,
            .record = &state.reservation_reserved,
            .pending_identity = &state.reservation_reserved_pending_identity,
            .canonical_identity = &state.reservation_reserved_identity,
        };
    }
    if (canonical_path == private_lease_owner_path(state.reservation_staging_path) &&
        pending_path == private_lease_owner_pending_path(state.reservation_staging_path)) {
        return PrivateReservationMarkerBinding{
            .role = PrivateReservationMarkerRole::Owner,
            .prior_phase = PrivateLeaseReservationPermitPhase::StagingDirectory,
            .pending_authorized_phase = PrivateLeaseReservationPermitPhase::OwnerPendingAuthorized,
            .pending_phase = PrivateLeaseReservationPermitPhase::OwnerPending,
            .canonical_authorized_phase =
                PrivateLeaseReservationPermitPhase::OwnerCanonicalAuthorized,
            .canonical_phase = PrivateLeaseReservationPermitPhase::OwnerCanonical,
            .record = &state.reservation_owner,
            .pending_identity = &state.reservation_owner_pending_identity,
            .canonical_identity = &state.reservation_owner_identity,
        };
    }
    if (canonical_path == state.paths.lease_owned_path &&
        pending_path == state.paths.lease_owned_pending_path) {
        return PrivateReservationMarkerBinding{
            .role = PrivateReservationMarkerRole::Owned,
            .prior_phase = PrivateLeaseReservationPermitPhase::OwnerCanonical,
            .pending_authorized_phase = PrivateLeaseReservationPermitPhase::OwnedPendingAuthorized,
            .pending_phase = PrivateLeaseReservationPermitPhase::OwnedPending,
            .canonical_authorized_phase =
                PrivateLeaseReservationPermitPhase::OwnedCanonicalAuthorized,
            .canonical_phase = PrivateLeaseReservationPermitPhase::OwnedCanonical,
            .record = &state.reservation_owned,
            .pending_identity = &state.reservation_owned_pending_identity,
            .canonical_identity = &state.reservation_owned_identity,
        };
    }
    fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
}

void validate_reservation_marker_record(PrivateCleanupActionPermit::State& state,
                                        PrivateReservationMarkerBinding binding,
                                        const PrivateLeaseRecord& record) {
    switch (binding.role) {
    case PrivateReservationMarkerRole::Reserved:
        if (!state.reservation_reserved || record != *state.reservation_reserved) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        return;
    case PrivateReservationMarkerRole::Owner:
        if (!state.reservation_reserved || !state.reservation_directory_identity ||
            record != make_private_lease_owner_record(*state.reservation_reserved,
                                                      *state.reservation_directory_identity)) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        return;
    case PrivateReservationMarkerRole::Owned:
        if (!state.reservation_owner || !state.reservation_owner_identity ||
            record != make_private_lease_owned_record(*state.reservation_owner,
                                                      *state.reservation_owner_identity)) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        return;
    }
    fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
}

void require_reservation_marker_parent_stable(PrivateCleanupActionPermit::State& state,
                                              PrivateReservationMarkerRole role) {
#ifndef _WIN32
    if ((role == PrivateReservationMarkerRole::Owner) &&
        (!state.reservation_directory || !state.reservation_directory_identity ||
         state.reservation_directory->identity() != *state.reservation_directory_identity)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    if (role == PrivateReservationMarkerRole::Owner) {
        state.reservation_directory->require_stable();
        state.reservation_directory->require_private_policy();
    }
#else
    (void)state;
    (void)role;
#endif
}

void require_fresh_writer_generation(PrivateCleanupActionPermit::State& state,
                                     PrivateFreshWriterPermitPhase phase,
                                     bool require_prior_witness = true) {
    require_private_lifecycle_common(state, PrivateNamespaceAction::ValidateFreshWriter);
    if (state.fresh_writer_phase != phase || !state.fresh_writer_generation || !state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    if (require_prior_witness) {
        require_private_lifecycle_phase_witness(state);
    }
    const auto& expected = *state.fresh_writer_generation;
    const auto current = capture_private_lease_removal_generation_locked(
        state.paths, *state.lock, expected.expected_lease_id, expected.expected_directory_identity,
        expected.expected_owner_identity, expected.expected_owned_identity);
    if (current != expected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    require_private_lease_generation_shape(state.paths, current, true);

    const auto entries =
        inspect_private_lease_preactive_entries(state.paths.private_directory, state.paths);
    const bool expect_index = phase >= PrivateFreshWriterPermitPhase::IndexReserved;
    const bool expect_data = phase >= PrivateFreshWriterPermitPhase::PairReserved;
    if (entries.owner != true || entries.index != expect_index || entries.data != expect_data) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    if (expect_index) {
        if (!state.fresh_index_identity) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        const auto expected_size = phase >= PrivateFreshWriterPermitPhase::HeadersExact
                                       ? OOCRelationStoreFormat::INDEX_HEADER_BYTES
                                       : 0;
        require_private_artifact_identity(state.paths.index_path, *state.fresh_index_identity,
                                          expected_size);
    }
    if (expect_data) {
        if (!state.fresh_data_identity || state.fresh_data_identity == state.fresh_index_identity) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        const auto expected_size = phase >= PrivateFreshWriterPermitPhase::HeadersExact
                                       ? OOCRelationStoreFormat::DATA_HEADER_BYTES
                                       : 0;
        require_private_artifact_identity(state.paths.data_path, *state.fresh_data_identity,
                                          expected_size);
    }
    if (phase >= PrivateFreshWriterPermitPhase::HeadersExact) {
        if (!state.fresh_pair || state.fresh_store_id == 0) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        require_source_pair_unchanged(state.paths, *state.fresh_pair);
    }
    state.lock->require_stable();
}

void require_activation_generation(PrivateCleanupActionPermit::State& state,
                                   bool reserved_present) {
    require_private_lifecycle_common(state, PrivateNamespaceAction::ActivateFreshLease);
    if (!state.activation_generation || !state.activation_pair ||
        !state.activation_pair_ownership || !state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    require_private_lifecycle_phase_witness(state);
    const auto& expected = *state.activation_generation;
    auto current = capture_private_lease_removal_generation_locked(
        state.paths, *state.lock, expected.expected_lease_id, expected.expected_directory_identity,
        expected.expected_owner_identity, expected.expected_owned_identity);
    require_private_lease_generation_shape(state.paths, current, reserved_present);
    if (reserved_present && current != expected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    if (!ownership_proof_matches(*state.activation_pair_ownership, state.paths,
                                 *state.activation_pair)) {
        fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None, protocol_error());
    }
    require_source_pair_unchanged(state.paths, *state.activation_pair);
    state.lock->require_stable();
}

void require_private_lease_cleanup_handoff_bindings_unchanged(
    PrivateCleanupActionPermit::State& state) {
    if (!state.publication_generation || !state.publication_intent || !state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }

    try {
        const auto current_generation = capture_private_lease_cleanup_handoff_generation_locked(
            state.paths, *state.lock, state.publication_generation->expected_lease_id,
            state.publication_generation->expected_directory_identity,
            state.publication_generation->expected_owner_identity,
            state.publication_generation->expected_owned_identity);
        if (current_generation != *state.publication_generation) {
            fail_private_cleanup_witness_replacement();
        }
        require_source_pair_unchanged(state.paths, *state.publication_intent);
    } catch (const Failure& failure) {
        if (failure.status == OOCCleanupStatus::IoFailure ||
            failure.status == OOCCleanupStatus::UnexpectedFailure) {
            throw;
        }
        fail_private_cleanup_witness_replacement();
    }
    state.lock->require_stable();
}

#if defined(__APPLE__)
[[nodiscard]] bool
optional_cleanup_leaf_witness_equal(const std::optional<PrivateCleanupLeafWitness>& lhs,
                                    const std::optional<PrivateCleanupLeafWitness>& rhs) noexcept {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    return !lhs || cleanup_leaf_witness_equal(*lhs, *rhs);
}

[[nodiscard]] bool
optional_handoff_leaf_witness_equal(const std::optional<LoadedPrivateHandoffLeaf>& lhs,
                                    const std::optional<LoadedPrivateHandoffLeaf>& rhs) noexcept {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    return !lhs || handoff_leaf_witness_equal(*lhs, *rhs);
}

[[nodiscard]] bool publication_inventory_equal_except_intent(
    const std::optional<PrivateCleanupUnionDirectoryInventory>& lhs,
    const std::optional<PrivateCleanupUnionDirectoryInventory>& rhs) noexcept {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    if (!lhs) {
        return true;
    }
    if (lhs->foreign != rhs->foreign) {
        return false;
    }
    for (std::size_t slot = 0; slot < lhs->leaves.size(); ++slot) {
        if (slot == static_cast<std::size_t>(PrivateCleanupUnionDirectoryEntry::Intent) ||
            slot == static_cast<std::size_t>(PrivateCleanupUnionDirectoryEntry::IntentPending)) {
            continue;
        }
        if (lhs->leaves[slot] != rhs->leaves[slot]) {
            return false;
        }
    }
    return true;
}
#endif

void require_publication_non_intent_witness_unchanged(
    PrivateCleanupActionPermit::State& state,
    const PrivateCleanupUnionObservationWitness& current) {
    const auto& original = state.witness;
    const auto staged_slot = static_cast<std::size_t>(PrivateCleanupMarkerSlot::Staged);
    const auto staged_pending_slot =
        static_cast<std::size_t>(PrivateCleanupMarkerSlot::StagedPending);
    if (current.raw.namespace_foreign != original.raw.namespace_foreign ||
        current.raw.cleanup_markers[staged_slot] != original.raw.cleanup_markers[staged_slot] ||
        current.raw.cleanup_markers[staged_pending_slot] !=
            original.raw.cleanup_markers[staged_pending_slot] ||
        current.raw.handoff_markers != original.raw.handoff_markers) {
        fail_private_cleanup_witness_replacement();
    }

#if defined(__APPLE__)
    if (current.directory == nullptr || original.directory == nullptr ||
        current.directory->identity() != original.directory->identity() ||
        !current.before_inventory || !current.after_inventory ||
        *current.before_inventory != *current.after_inventory || !original.before_inventory ||
        !original.after_inventory || *original.before_inventory != *original.after_inventory ||
        !publication_inventory_equal_except_intent(current.before_inventory,
                                                   original.before_inventory) ||
        !publication_inventory_equal_except_intent(current.after_inventory,
                                                   original.after_inventory) ||
        !optional_cleanup_leaf_witness_equal(current.cleanup_leaves[staged_slot],
                                             original.cleanup_leaves[staged_slot]) ||
        !optional_cleanup_leaf_witness_equal(current.cleanup_leaves[staged_pending_slot],
                                             original.cleanup_leaves[staged_pending_slot]) ||
        !optional_handoff_leaf_witness_equal(current.canonical_handoff,
                                             original.canonical_handoff) ||
        !optional_handoff_leaf_witness_equal(current.pending_handoff, original.pending_handoff) ||
        !lease_marker_witness_equal(current.reserved_marker, original.reserved_marker) ||
        !lease_marker_witness_equal(current.owned_marker, original.owned_marker)) {
        fail_private_cleanup_witness_replacement();
    }
#endif
    state.lock->require_stable();
}

[[nodiscard]] InspectResult
require_exact_publication_marker(const std::filesystem::path& path, bool pending,
                                 const IntentRecord& expected,
                                 std::optional<FileIdentity> expected_identity = std::nullopt) {
    auto inspected = inspect_file(path, MARKER_BYTES, pending);
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (inspected.kind != InspectKind::Present ||
        parse_marker(inspected.bytes, INTENT_MAGIC) != expected ||
        (expected_identity && inspected.identity != *expected_identity)) {
        fail_private_cleanup_witness_replacement();
    }
    return inspected;
}

void require_publication_marker_missing(const std::filesystem::path& path) {
    const auto inspected = inspect_file(path, 0, false);
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (inspected.kind != InspectKind::Missing) {
        fail_private_cleanup_witness_replacement();
    }
}

void require_publication_non_intent_names_missing(const OOCCleanupPaths& paths) {
    require_publication_marker_missing(paths.staged_path);
    require_publication_marker_missing(paths.staged_pending_path);
    require_publication_marker_missing(paths.quarantine_index_path);
    require_publication_marker_missing(paths.quarantine_data_path);
}

void require_publication_original_witness(PrivateCleanupActionPermit::State& state) {
    require_private_cleanup_action_witness_unchanged(state);
    require_publication_non_intent_names_missing(state.paths);
    require_private_lease_cleanup_handoff_bindings_unchanged(state);
}

void capture_publication_phase_witness(PrivateCleanupActionPermit::State& state) {
    if (!state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto current = observe_private_cleanup_union_locked(state.paths, *state.lock);
    require_publication_non_intent_witness_unchanged(state, current);
    require_publication_non_intent_names_missing(state.paths);
    require_private_lease_cleanup_handoff_bindings_unchanged(state);
    state.publication_phase_witness = std::move(current);
}

void require_publication_phase_witness_unchanged(PrivateCleanupActionPermit::State& state) {
    if (!state.lock || !state.publication_phase_witness) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    require_private_cleanup_witness_unchanged(state.paths, *state.lock,
                                              *state.publication_phase_witness);
    require_publication_non_intent_names_missing(state.paths);
    require_private_lease_cleanup_handoff_bindings_unchanged(state);
}

void require_publication_pending_owned_or_missing(const OOCCleanupPaths& paths,
                                                  const IntentRecord& intent) {
    const auto expected = serialize_marker(intent, INTENT_MAGIC);
    const auto pending = inspect_pending_file(paths.intent_pending_path);
    if (pending.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, pending.error);
    }
    if (pending.kind == InspectKind::Missing) {
        return;
    }
    if (pending.kind != InspectKind::Present ||
        (!marker_bytes_equal(pending.bytes, expected) &&
         !pending_is_recognizable_owned(pending.bytes, expected))) {
        fail_private_cleanup_witness_replacement();
    }
}

} // namespace

void initialize_private_lease_reservation_permit(PrivateCleanupActionPermit& permit,
                                                 const PrivateLeaseRecord& reserved) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    const auto prior = state.reservation_phase;
    state.reservation_phase = PrivateLeaseReservationPermitPhase::Failed;
    try {
        if (prior != PrivateLeaseReservationPermitPhase::Fresh) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        require_private_lifecycle_common(state, PrivateNamespaceAction::ReservePrivateLease);
        require_private_cleanup_action_witness_unchanged(state);
        if (!private_lifecycle_union_is_clear(state.witness.raw)) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        require_pair_namespace_reusable_locked(state.paths);
        require_private_lease_marker_missing(state.paths.lease_reserved_path);
        require_private_lease_marker_missing(state.paths.lease_reserved_pending_path);
        require_private_lease_marker_missing(state.paths.lease_owned_path);
        require_private_lease_marker_missing(state.paths.lease_owned_pending_path);
        if (inspect_directory_identity_locked(state.paths.private_directory)) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }

        const auto parent_identity =
            capture_directory_identity_locked(state.paths.private_directory.parent_path());
        validate_private_lease_record_context(reserved, state.paths, parent_identity,
                                              state.lock->identity());
        const auto staging_path = private_lease_staging_path(state.paths, reserved.lease_id);
        if (inspect_directory_identity_locked(staging_path)) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        state.reservation_reserved = reserved;
        state.reservation_parent_identity = parent_identity;
        state.reservation_staging_path = staging_path;
        state.reservation_phase = PrivateLeaseReservationPermitPhase::Empty;
        record_private_lifecycle_successor(state);
    } catch (...) {
        state.reservation_phase = PrivateLeaseReservationPermitPhase::Failed;
        throw;
    }
}

void advance_private_lease_reservation_marker(PrivateCleanupActionPermit& permit,
                                              PrivateLeaseMarkerPublicationPoint point,
                                              const std::filesystem::path& canonical_path,
                                              const std::filesystem::path& pending_path,
                                              const PrivateLeaseRecord& record,
                                              const FileIdentity* identity) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    const auto prior = state.reservation_phase;
    state.reservation_phase = PrivateLeaseReservationPermitPhase::Failed;
    try {
        state.reservation_phase = prior;
        require_private_lifecycle_common(state, PrivateNamespaceAction::ReservePrivateLease);
        auto binding = reservation_marker_binding(state, canonical_path, pending_path);
        validate_reservation_marker_record(state, binding, record);
        require_reservation_marker_parent_stable(state, binding.role);

        switch (point) {
        case PrivateLeaseMarkerPublicationPoint::BeforePendingPreparation:
            require_private_reservation_phase_shape(state, binding.prior_phase);
            require_private_lease_marker_missing(canonical_path);
            require_private_lease_marker_missing(pending_path);
            if (binding.role != PrivateReservationMarkerRole::Reserved) {
                *binding.record = record;
            }
            state.reservation_phase = binding.pending_authorized_phase;
            require_private_reservation_phase_shape(state, binding.pending_authorized_phase);
            return;
        case PrivateLeaseMarkerPublicationPoint::PendingDurable: {
            if (identity == nullptr) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            require_private_lifecycle_common(state, PrivateNamespaceAction::ReservePrivateLease);
            if (state.reservation_phase != binding.pending_authorized_phase) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            const auto stable = stable_identity(*identity);
            require_private_lease_marker_missing(canonical_path);
            (void)require_private_lease_marker_exact(pending_path, record, stable);
            if (binding.role != PrivateReservationMarkerRole::Reserved) {
                *binding.record = record;
            }
            *binding.pending_identity = stable;
            state.reservation_phase = binding.pending_phase;
            require_private_reservation_phase_shape(state, binding.pending_phase, false);
            record_private_lifecycle_successor(state);
            return;
        }
        case PrivateLeaseMarkerPublicationPoint::BeforeCanonicalRename:
            require_private_reservation_phase_shape(state, binding.pending_phase);
            if (!*binding.pending_identity) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            require_private_lease_marker_missing(canonical_path);
            (void)require_private_lease_marker_exact(pending_path, record,
                                                     **binding.pending_identity);
            state.reservation_phase = binding.canonical_authorized_phase;
            require_private_reservation_phase_shape(state, binding.canonical_authorized_phase);
            return;
        case PrivateLeaseMarkerPublicationPoint::CanonicalDurable: {
            if (identity == nullptr || !*binding.pending_identity) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            require_private_lifecycle_common(state, PrivateNamespaceAction::ReservePrivateLease);
            if (state.reservation_phase != binding.canonical_authorized_phase) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            const auto stable = stable_identity(*identity);
            if (stable != **binding.pending_identity) {
                fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                     protocol_error());
            }
            require_private_lease_marker_missing(pending_path);
            (void)require_private_lease_marker_exact(canonical_path, record, stable);
            *binding.canonical_identity = stable;
            state.reservation_phase = binding.canonical_phase;
            require_private_reservation_phase_shape(state, binding.canonical_phase, false);
            record_private_lifecycle_successor(state);
            return;
        }
        case PrivateLeaseMarkerPublicationPoint::BeforeDuplicatePendingRemoval:
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        case PrivateLeaseMarkerPublicationPoint::Complete:
            require_private_reservation_phase_shape(state, binding.canonical_phase);
            if (!*binding.canonical_identity) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            require_private_lease_marker_missing(pending_path);
            (void)require_private_lease_marker_exact(canonical_path, record,
                                                     **binding.canonical_identity);
            return;
        }
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    } catch (...) {
        state.reservation_phase = PrivateLeaseReservationPermitPhase::Failed;
        throw;
    }
}

void record_private_lease_reservation_directory_successor(
    PrivateCleanupActionPermit& permit, const std::filesystem::path& directory_path,
    const std::array<std::uint64_t, 3>& identity, bool final_directory) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    const auto prior = state.reservation_phase;
    state.reservation_phase = PrivateLeaseReservationPermitPhase::Failed;
    try {
        state.reservation_phase = prior;
        const auto expected_phase =
            final_directory ? PrivateLeaseReservationPermitPhase::FinalRenameAuthorized
                            : PrivateLeaseReservationPermitPhase::StagingDirectoryAuthorized;
        require_private_lifecycle_common(state, PrivateNamespaceAction::ReservePrivateLease);
        if (state.reservation_phase != expected_phase) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        if (directory_path !=
            (final_directory ? state.paths.private_directory : state.reservation_staging_path)) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        if (capture_directory_identity_locked(directory_path) != identity) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        if (!final_directory) {
            const auto entries = inspect_private_lease_control_entries(directory_path);
            if (entries.owner || entries.owner_pending ||
                inspect_directory_identity_locked(state.paths.private_directory)) {
                fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                     protocol_error());
            }
            state.reservation_directory_identity = identity;
#ifndef _WIN32
            state.reservation_directory = std::make_unique<PrivateDirectoryHandle>(directory_path);
            if (state.reservation_directory->identity() != identity) {
                fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                     protocol_error());
            }
            state.reservation_directory->require_private_policy();
#endif
            state.reservation_phase = PrivateLeaseReservationPermitPhase::StagingDirectory;
        } else {
            if (inspect_directory_identity_locked(state.reservation_staging_path)) {
                fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                     protocol_error());
            }
#ifndef _WIN32
            if (!state.reservation_directory) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            state.reservation_directory->rebind_after_rename(state.reservation_staging_path,
                                                             state.paths.private_directory);
#endif
            if (!state.reservation_owned || !state.reservation_owner ||
                !state.reservation_owner_identity || !state.reservation_owned_identity) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            const auto entries = inspect_private_lease_control_entries(directory_path);
            if (!entries.owner || entries.owner_pending) {
                fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                     protocol_error());
            }
            (void)require_private_lease_marker_exact(private_lease_owner_path(directory_path),
                                                     *state.reservation_owner,
                                                     *state.reservation_owner_identity);
            state.reservation_phase = PrivateLeaseReservationPermitPhase::FinalDirectory;
        }
        require_private_reservation_phase_shape(state, state.reservation_phase, false);
        record_private_lifecycle_successor(state);
    } catch (...) {
        state.reservation_phase = PrivateLeaseReservationPermitPhase::Failed;
        throw;
    }
}

void authorize_private_lease_reservation_staging_directory(PrivateCleanupActionPermit& permit) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    const auto prior = state.reservation_phase;
    state.reservation_phase = PrivateLeaseReservationPermitPhase::Failed;
    try {
        state.reservation_phase = prior;
        require_private_reservation_phase_shape(
            state, PrivateLeaseReservationPermitPhase::ReservedCanonical);
        if (inspect_directory_identity_locked(state.reservation_staging_path) ||
            inspect_directory_identity_locked(state.paths.private_directory)) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        state.reservation_phase = PrivateLeaseReservationPermitPhase::StagingDirectoryAuthorized;
        require_private_reservation_phase_shape(
            state, PrivateLeaseReservationPermitPhase::StagingDirectoryAuthorized);
    } catch (...) {
        state.reservation_phase = PrivateLeaseReservationPermitPhase::Failed;
        throw;
    }
}

void authorize_private_lease_reservation_final_rename(PrivateCleanupActionPermit& permit) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    const auto prior = state.reservation_phase;
    state.reservation_phase = PrivateLeaseReservationPermitPhase::Failed;
    try {
        state.reservation_phase = prior;
        require_private_reservation_phase_shape(state,
                                                PrivateLeaseReservationPermitPhase::OwnedCanonical);
        if (!state.reservation_directory_identity || !state.reservation_owner ||
            !state.reservation_owner_identity || !state.reservation_owned ||
            !state.reservation_owned_identity ||
            inspect_directory_identity_locked(state.paths.private_directory) ||
            capture_directory_identity_locked(state.reservation_staging_path) !=
                *state.reservation_directory_identity) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        const auto entries = inspect_private_lease_control_entries(state.reservation_staging_path);
        if (!entries.owner || entries.owner_pending) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        (void)require_private_lease_marker_exact(
            private_lease_owner_path(state.reservation_staging_path), *state.reservation_owner,
            *state.reservation_owner_identity);
        (void)require_private_lease_marker_exact(state.paths.lease_owned_path,
                                                 *state.reservation_owned,
                                                 *state.reservation_owned_identity);
        require_private_lease_marker_missing(state.paths.lease_owned_pending_path);
#ifndef _WIN32
        if (!state.reservation_directory) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        state.reservation_directory->require_stable();
        state.reservation_directory->require_private_policy();
#endif
        state.reservation_phase = PrivateLeaseReservationPermitPhase::FinalRenameAuthorized;
        record_private_lifecycle_successor(state);
    } catch (...) {
        state.reservation_phase = PrivateLeaseReservationPermitPhase::Failed;
        throw;
    }
}

void complete_private_lease_reservation_permit(PrivateCleanupActionPermit& permit) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    const auto prior = state.reservation_phase;
    state.reservation_phase = PrivateLeaseReservationPermitPhase::Failed;
    try {
        state.reservation_phase = prior;
        require_private_reservation_phase_shape(state,
                                                PrivateLeaseReservationPermitPhase::FinalDirectory);
        if (!state.reservation_reserved || !state.reservation_reserved_identity ||
            !state.reservation_owned || !state.reservation_owned_identity ||
            !state.reservation_owner || !state.reservation_owner_identity ||
            !state.reservation_directory_identity ||
            capture_directory_identity_locked(state.paths.private_directory) !=
                *state.reservation_directory_identity) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        (void)require_private_lease_marker_exact(state.paths.lease_reserved_path,
                                                 *state.reservation_reserved,
                                                 *state.reservation_reserved_identity);
        (void)require_private_lease_marker_exact(state.paths.lease_owned_path,
                                                 *state.reservation_owned,
                                                 *state.reservation_owned_identity);
        (void)require_private_lease_marker_exact(
            private_lease_owner_path(state.paths.private_directory), *state.reservation_owner,
            *state.reservation_owner_identity);
        require_private_lease_marker_missing(state.paths.lease_reserved_pending_path);
        require_private_lease_marker_missing(state.paths.lease_owned_pending_path);
        state.reservation_phase = PrivateLeaseReservationPermitPhase::ReceiptCommitted;
    } catch (...) {
        state.reservation_phase = PrivateLeaseReservationPermitPhase::Failed;
        throw;
    }
}

void initialize_private_fresh_writer_permit(
    PrivateCleanupActionPermit& permit, const std::array<std::uint64_t, 2>& expected_lease_id,
    const std::array<std::uint64_t, 3>& expected_directory_identity,
    const std::array<std::uint64_t, 3>& expected_owner_identity,
    const std::array<std::uint64_t, 3>& expected_owned_identity, bool deferred_mode,
    std::uint64_t receipt_owner_process_id) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    const auto prior = state.fresh_writer_phase;
    state.fresh_writer_phase = PrivateFreshWriterPermitPhase::Failed;
    try {
        if (prior != PrivateFreshWriterPermitPhase::Fresh ||
            (receipt_owner_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) &&
             !deferred_mode)) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        require_private_lifecycle_common(state, PrivateNamespaceAction::ValidateFreshWriter);
        require_private_cleanup_action_witness_unchanged(state);
        if (!private_lifecycle_union_is_clear(state.witness.raw)) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        auto generation = capture_private_lease_removal_generation_locked(
            state.paths, *state.lock, expected_lease_id, expected_directory_identity,
            expected_owner_identity, expected_owned_identity);
        require_private_lease_generation_shape(state.paths, generation, true);
        const auto entries =
            inspect_private_lease_preactive_entries(state.paths.private_directory, state.paths);
        if (!entries.owner || entries.index || entries.data) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        require_pair_namespace_reusable_locked(state.paths);
        state.fresh_writer_generation = std::move(generation);
        state.fresh_writer_phase = PrivateFreshWriterPermitPhase::LeaseOnly;
        record_private_lifecycle_successor(state);
    } catch (...) {
        state.fresh_writer_phase = PrivateFreshWriterPermitPhase::Failed;
        throw;
    }
}

void advance_private_fresh_writer_permit(PrivateCleanupActionPermit& permit,
                                         PrivateFreshWriterPermitBoundary boundary,
                                         std::optional<std::array<std::uint64_t, 3>> index_identity,
                                         std::optional<std::array<std::uint64_t, 3>> data_identity,
                                         std::uint64_t store_id) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    const auto prior = state.fresh_writer_phase;
    state.fresh_writer_phase = PrivateFreshWriterPermitPhase::Failed;
    try {
        state.fresh_writer_phase = prior;
        switch (boundary) {
        case PrivateFreshWriterPermitBoundary::BeforeIndexReservation:
            require_fresh_writer_generation(state, PrivateFreshWriterPermitPhase::LeaseOnly);
            state.fresh_writer_phase = PrivateFreshWriterPermitPhase::IndexReservationAuthorized;
            require_fresh_writer_generation(
                state, PrivateFreshWriterPermitPhase::IndexReservationAuthorized);
            return;
        case PrivateFreshWriterPermitBoundary::IndexReserved:
            if (!index_identity || data_identity || store_id != 0) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            require_private_lifecycle_common(state, PrivateNamespaceAction::ValidateFreshWriter);
            if (prior != PrivateFreshWriterPermitPhase::IndexReservationAuthorized) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            state.fresh_index_identity = *index_identity;
            state.fresh_writer_phase = PrivateFreshWriterPermitPhase::IndexReserved;
            require_fresh_writer_generation(state, PrivateFreshWriterPermitPhase::IndexReserved,
                                            false);
            record_private_lifecycle_successor(state);
            return;
        case PrivateFreshWriterPermitBoundary::BeforeDataReservation:
            require_fresh_writer_generation(state, PrivateFreshWriterPermitPhase::IndexReserved);
            state.fresh_writer_phase = PrivateFreshWriterPermitPhase::DataReservationAuthorized;
            require_fresh_writer_generation(
                state, PrivateFreshWriterPermitPhase::DataReservationAuthorized);
            return;
        case PrivateFreshWriterPermitBoundary::DataReserved:
            if (!index_identity || !data_identity || *index_identity == *data_identity ||
                store_id != 0 || state.fresh_index_identity != index_identity ||
                prior != PrivateFreshWriterPermitPhase::DataReservationAuthorized) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            state.fresh_data_identity = *data_identity;
            state.fresh_writer_phase = PrivateFreshWriterPermitPhase::PairReserved;
            require_fresh_writer_generation(state, PrivateFreshWriterPermitPhase::PairReserved,
                                            false);
            record_private_lifecycle_successor(state);
            return;
        case PrivateFreshWriterPermitBoundary::BeforeHeaderWrite:
            require_fresh_writer_generation(state, PrivateFreshWriterPermitPhase::PairReserved);
            state.fresh_writer_phase = PrivateFreshWriterPermitPhase::HeaderWriteAuthorized;
            require_fresh_writer_generation(state,
                                            PrivateFreshWriterPermitPhase::HeaderWriteAuthorized);
            return;
        case PrivateFreshWriterPermitBoundary::HeadersValidated: {
            if (!index_identity || !data_identity || store_id == 0 ||
                state.fresh_index_identity != index_identity ||
                state.fresh_data_identity != data_identity ||
                prior != PrivateFreshWriterPermitPhase::HeaderWriteAuthorized) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            auto pair = capture_source_pair(state.paths, store_id);
            if (stable_identity(pair.index.identity) != *index_identity ||
                stable_identity(pair.data.identity) != *data_identity) {
                fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                     protocol_error());
            }
            state.fresh_store_id = store_id;
            state.fresh_pair = std::move(pair);
            state.fresh_writer_phase = PrivateFreshWriterPermitPhase::HeadersExact;
            require_fresh_writer_generation(state, PrivateFreshWriterPermitPhase::HeadersExact,
                                            false);
            record_private_lifecycle_successor(state);
            return;
        }
        case PrivateFreshWriterPermitBoundary::PairOwnershipCaptured:
            if (!index_identity || !data_identity || store_id == 0 ||
                state.fresh_index_identity != index_identity ||
                state.fresh_data_identity != data_identity || state.fresh_store_id != store_id ||
                prior != PrivateFreshWriterPermitPhase::HeadersExact) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            state.fresh_writer_phase = PrivateFreshWriterPermitPhase::PairReceiptCandidate;
            require_fresh_writer_generation(state,
                                            PrivateFreshWriterPermitPhase::PairReceiptCandidate);
            record_private_lifecycle_successor(state);
            return;
        case PrivateFreshWriterPermitBoundary::Complete:
            if (index_identity || data_identity || store_id != 0) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            require_fresh_writer_generation(state,
                                            PrivateFreshWriterPermitPhase::PairReceiptCandidate);
            state.fresh_writer_phase = PrivateFreshWriterPermitPhase::Complete;
            return;
        }
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    } catch (...) {
        state.fresh_writer_phase = PrivateFreshWriterPermitPhase::Failed;
        throw;
    }
}

bool private_fresh_writer_permit_allows_rollback(
    PrivateCleanupActionPermit& permit, std::optional<std::array<std::uint64_t, 3>> index_identity,
    std::optional<std::array<std::uint64_t, 3>> data_identity) noexcept {
    try {
        if (!permit.state_) {
            return false;
        }
        auto& state = *permit.state_;
        if (state.fresh_writer_phase == PrivateFreshWriterPermitPhase::Fresh ||
            state.fresh_writer_phase == PrivateFreshWriterPermitPhase::Failed ||
            state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
            state.fresh_index_identity != index_identity ||
            state.fresh_data_identity != data_identity) {
            return false;
        }
        require_fresh_writer_generation(state, state.fresh_writer_phase);
        // This is a one-shot authorization for the caller's existing
        // same-identity reservation handles. A repeated rollback query must
        // fail closed.
        state.fresh_writer_phase = PrivateFreshWriterPermitPhase::Failed;
        return true;
    } catch (...) {
        if (permit.state_) {
            permit.state_->fresh_writer_phase = PrivateFreshWriterPermitPhase::Failed;
        }
        return false;
    }
}

void initialize_private_lease_activation_permit(
    PrivateCleanupActionPermit& permit, const std::array<std::uint64_t, 2>& expected_lease_id,
    const std::array<std::uint64_t, 3>& expected_directory_identity,
    const std::array<std::uint64_t, 3>& expected_owner_identity,
    const std::array<std::uint64_t, 3>& expected_owned_identity,
    const OwnershipProof& pair_ownership) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    const auto prior = state.activation_phase;
    state.activation_phase = PrivateLeaseActivationPermitPhase::Failed;
    try {
        if (prior != PrivateLeaseActivationPermitPhase::Fresh ||
            pair_ownership.base_path != state.paths.base_path || pair_ownership.store_id == 0) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        require_private_lifecycle_common(state, PrivateNamespaceAction::ActivateFreshLease);
        require_private_cleanup_action_witness_unchanged(state);
        if (!private_lifecycle_union_is_clear(state.witness.raw)) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        auto generation = capture_private_lease_removal_generation_locked(
            state.paths, *state.lock, expected_lease_id, expected_directory_identity,
            expected_owner_identity, expected_owned_identity);
        require_private_lease_generation_shape(state.paths, generation, true);
        auto pair = capture_source_pair(state.paths, pair_ownership.store_id);
        if (!ownership_proof_matches(pair_ownership, state.paths, pair)) {
            fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None, protocol_error());
        }
        require_source_pair_unchanged(state.paths, pair);
        state.activation_generation = std::move(generation);
        state.activation_pair_ownership = pair_ownership;
        state.activation_pair = std::move(pair);
        state.activation_phase = PrivateLeaseActivationPermitPhase::ExactPreactive;
        record_private_lifecycle_successor(state);
    } catch (...) {
        state.activation_phase = PrivateLeaseActivationPermitPhase::Failed;
        throw;
    }
}

void advance_private_lease_activation_permit(PrivateCleanupActionPermit& permit,
                                             PrivateLeaseActivationPermitBoundary boundary) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    const auto prior = state.activation_phase;
    state.activation_phase = PrivateLeaseActivationPermitPhase::Failed;
    try {
        state.activation_phase = prior;
        switch (boundary) {
        case PrivateLeaseActivationPermitBoundary::BeforeReservedRemoval:
            if (prior != PrivateLeaseActivationPermitPhase::ExactPreactive) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            require_activation_generation(state, true);
            state.activation_phase = PrivateLeaseActivationPermitPhase::ReservedRemovalAuthorized;
            return;
        case PrivateLeaseActivationPermitBoundary::Complete:
            if (prior != PrivateLeaseActivationPermitPhase::ReservedRevokedCommitted) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            require_activation_generation(state, false);
            state.activation_phase = PrivateLeaseActivationPermitPhase::Complete;
            return;
        }
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    } catch (...) {
        state.activation_phase = PrivateLeaseActivationPermitPhase::Failed;
        throw;
    }
}

bool commit_private_lease_activation_permit_noexcept(PrivateCleanupActionPermit& permit) noexcept {
    if (!permit.state_) {
        return false;
    }
    auto& state = *permit.state_;
    if (state.activation_phase != PrivateLeaseActivationPermitPhase::ReservedRemovalAuthorized ||
        !state.action_started ||
        state.handoff_state != PrivateHandoffConsumptionState::LifecycleObserved ||
        state.decision.action != PrivateNamespaceAction::ActivateFreshLease ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock) {
        state.activation_phase = PrivateLeaseActivationPermitPhase::Failed;
        return false;
    }
    state.activation_phase = PrivateLeaseActivationPermitPhase::ReservedRevokedCommitted;
    return true;
}

LoadedPrivateLeaseMarker
private_lease_activation_reserved_removal_proof(PrivateCleanupActionPermit& permit) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    if (state.activation_phase != PrivateLeaseActivationPermitPhase::ReservedRemovalAuthorized ||
        !state.activation_generation || !state.activation_generation->reserved) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    require_activation_generation(state, true);
    return *state.activation_generation->reserved;
}

OOCPrivateHandoffInspectResult
inspect_private_lease_cleanup_handoff_from_permit(PrivateCleanupActionPermit& permit) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    if (!state.action_started || state.handoff_state != PrivateHandoffConsumptionState::Fresh) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.handoff_state = PrivateHandoffConsumptionState::Failed;
    if (state.decision.action != PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock || !state.publication_generation || !state.publication_intent) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.lock->require_stable();
    require_publication_original_witness(state);

#if defined(__APPLE__)
    if (state.witness.handoff_classification) {
        const auto inspection = state.witness.handoff_classification->inspection;
        if (inspection.state != OOCPrivateHandoffState::None) {
            return inspection;
        }
    }
#endif
    state.handoff_state = PrivateHandoffConsumptionState::PublicationObserved;
    return handoff_none();
}

void authorize_private_cleanup_mutation(PrivateCleanupMutationGate& gate,
                                        const OOCCleanupPaths& paths, const BaseLock& lock,
                                        PrivateCleanupMutationBoundary boundary) {
    if (gate.state_ == PrivateCleanupMutationGate::State::Failed) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    const auto prior_gate_state = gate.state_;
    gate.state_ = PrivateCleanupMutationGate::State::Failed;
    if (gate.permit_ == nullptr || !gate.permit_->state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *gate.permit_->state_;
    const auto prior_handoff_state = state.handoff_state;
    state.handoff_state = PrivateHandoffConsumptionState::Failed;
    if (!state.action_started ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock || state.lock.get() != &lock || !lock.matches(paths.lock_path) ||
        !private_cleanup_paths_equal(state.paths, paths)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    lock.require_stable();

    if (state.decision.action == PrivateNamespaceAction::RunLegacyCleanup) {
        const bool already_authorized =
            prior_gate_state == PrivateCleanupMutationGate::State::LegacyAuthorized;
        if ((prior_gate_state != PrivateCleanupMutationGate::State::Fresh && !already_authorized) ||
            prior_handoff_state != (already_authorized
                                        ? PrivateHandoffConsumptionState::LegacyMutationAuthorized
                                        : PrivateHandoffConsumptionState::LegacyObserved)) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        if (!already_authorized) {
            require_private_cleanup_action_witness_unchanged(state);
        }
        state.handoff_state = PrivateHandoffConsumptionState::LegacyMutationAuthorized;
        gate.state_ = PrivateCleanupMutationGate::State::LegacyAuthorized;
        return;
    }

    if (state.decision.action != PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff ||
        (prior_handoff_state != PrivateHandoffConsumptionState::PublicationObserved &&
         prior_handoff_state != PrivateHandoffConsumptionState::PublicationMutationAuthorized)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }

    switch (boundary) {
    case PrivateCleanupMutationBoundary::PendingPreparation:
        if (prior_gate_state != PrivateCleanupMutationGate::State::Fresh) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        require_publication_original_witness(state);
        require_publication_marker_missing(paths.intent_path);
        gate.state_ = PrivateCleanupMutationGate::State::PublicationPendingPreparationAuthorized;
        break;
    case PrivateCleanupMutationBoundary::CanonicalRename:
        if (prior_gate_state != PrivateCleanupMutationGate::State::PublicationPendingDurable ||
            !gate.publication_pending_identity_ || !state.publication_intent) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        require_publication_phase_witness_unchanged(state);
        require_publication_marker_missing(paths.intent_path);
        (void)require_exact_publication_marker(paths.intent_pending_path, true,
                                               *state.publication_intent,
                                               gate.publication_pending_identity_);
        gate.state_ = PrivateCleanupMutationGate::State::PublicationCanonicalRenameAuthorized;
        break;
    case PrivateCleanupMutationBoundary::PendingRemoval:
        if (prior_gate_state != PrivateCleanupMutationGate::State::PublicationReceiptCommitted ||
            !gate.publication_receipt_committed_ || !gate.publication_canonical_identity_ ||
            !state.publication_intent) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        require_publication_phase_witness_unchanged(state);
        (void)require_exact_publication_marker(paths.intent_path, false, *state.publication_intent,
                                               gate.publication_canonical_identity_);
        gate.state_ = PrivateCleanupMutationGate::State::PublicationPendingRemovalAuthorized;
        break;
    case PrivateCleanupMutationBoundary::PublicationComplete:
        if ((prior_gate_state != PrivateCleanupMutationGate::State::PublicationReceiptCommitted &&
             prior_gate_state !=
                 PrivateCleanupMutationGate::State::PublicationPendingRemovalAuthorized) ||
            !gate.publication_receipt_committed_ || !gate.publication_canonical_identity_ ||
            !state.publication_intent) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        capture_publication_phase_witness(state);
        (void)require_exact_publication_marker(paths.intent_path, false, *state.publication_intent,
                                               gate.publication_canonical_identity_);
        require_publication_marker_missing(paths.intent_pending_path);
        gate.publication_pending_identity_.reset();
        gate.state_ = PrivateCleanupMutationGate::State::PublicationCompleted;
        break;
    case PrivateCleanupMutationBoundary::Generic:
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.handoff_state = PrivateHandoffConsumptionState::PublicationMutationAuthorized;
}

void record_private_cleanup_pending_successor(PrivateCleanupMutationGate& gate,
                                              const OOCCleanupPaths& paths, const BaseLock& lock,
                                              const std::filesystem::path& pending_path,
                                              const FileIdentity& identity) {
    if (gate.state_ == PrivateCleanupMutationGate::State::Failed || gate.permit_ == nullptr ||
        !gate.permit_->state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *gate.permit_->state_;
    if (state.decision.action == PrivateNamespaceAction::RunLegacyCleanup) {
        return;
    }

    const auto prior_gate_state = gate.state_;
    gate.state_ = PrivateCleanupMutationGate::State::Failed;
    const auto prior_handoff_state = state.handoff_state;
    state.handoff_state = PrivateHandoffConsumptionState::Failed;
    if (!state.action_started ||
        state.decision.action != PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff ||
        (prior_handoff_state != PrivateHandoffConsumptionState::PublicationObserved &&
         prior_handoff_state != PrivateHandoffConsumptionState::PublicationMutationAuthorized) ||
        (prior_gate_state != PrivateCleanupMutationGate::State::Fresh &&
         prior_gate_state !=
             PrivateCleanupMutationGate::State::PublicationPendingPreparationAuthorized) ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock || state.lock.get() != &lock || !lock.matches(paths.lock_path) ||
        !private_cleanup_paths_equal(state.paths, paths) ||
        pending_path != paths.intent_pending_path || !state.publication_intent) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    lock.require_stable();
    if (prior_gate_state == PrivateCleanupMutationGate::State::Fresh) {
        require_publication_original_witness(state);
    }
    require_publication_marker_missing(paths.intent_path);
    const auto pending = require_exact_publication_marker(paths.intent_pending_path, true,
                                                          *state.publication_intent, identity);
    capture_publication_phase_witness(state);
    gate.publication_pending_identity_ = pending.identity;
    gate.state_ = PrivateCleanupMutationGate::State::PublicationPendingDurable;
    state.handoff_state = PrivateHandoffConsumptionState::PublicationMutationAuthorized;
}

void commit_private_cleanup_canonical(PrivateCleanupMutationGate& gate,
                                      const OOCCleanupPaths& paths, const BaseLock& lock,
                                      const FileIdentity& durable_identity,
                                      bool renamed_from_pending, bool pending_must_remain) {
    if (gate.permit_ == nullptr || !gate.permit_->state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *gate.permit_->state_;
    const auto prior_gate_state = gate.state_;
    const auto prior_handoff_state = state.handoff_state;

    // The caller sets the escrowed receipt's sticky commit bit before entering
    // this bridge. Mirror that fact before any successor observation: a later
    // union, lease, pair, or pending-cleanup failure must never revive it.
    gate.publication_receipt_committed_ = true;
    gate.publication_canonical_identity_ = durable_identity;
    gate.state_ = PrivateCleanupMutationGate::State::Failed;
    state.handoff_state = PrivateHandoffConsumptionState::Failed;

    const bool original_canonical = prior_gate_state == PrivateCleanupMutationGate::State::Fresh;
    const bool promoted_pending =
        prior_gate_state == PrivateCleanupMutationGate::State::PublicationCanonicalRenameAuthorized;
    if (!state.action_started ||
        state.decision.action != PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock || state.lock.get() != &lock || !lock.matches(paths.lock_path) ||
        !private_cleanup_paths_equal(state.paths, paths) || !state.publication_intent ||
        (prior_handoff_state != PrivateHandoffConsumptionState::PublicationObserved &&
         prior_handoff_state != PrivateHandoffConsumptionState::PublicationMutationAuthorized) ||
        (!original_canonical && !promoted_pending) ||
        (renamed_from_pending && (!promoted_pending || pending_must_remain)) ||
        (pending_must_remain && (!promoted_pending || renamed_from_pending)) ||
        (promoted_pending && !renamed_from_pending && !pending_must_remain)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }

    if (original_canonical) {
        if (renamed_from_pending || pending_must_remain ||
            !state.publication_original_canonical_identity ||
            durable_identity != *state.publication_original_canonical_identity) {
            fail_private_cleanup_witness_replacement();
        }
    } else if (!gate.publication_pending_identity_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    } else if (renamed_from_pending &&
               !same_native_file(durable_identity, *gate.publication_pending_identity_)) {
        fail_private_cleanup_witness_replacement();
    }

    if (renamed_from_pending) {
        require_publication_marker_missing(paths.intent_pending_path);
    } else if (pending_must_remain) {
        (void)require_exact_publication_marker(paths.intent_pending_path, true,
                                               *state.publication_intent,
                                               gate.publication_pending_identity_);
    } else {
        require_publication_pending_owned_or_missing(paths, *state.publication_intent);
    }

    capture_publication_phase_witness(state);
    gate.state_ = PrivateCleanupMutationGate::State::PublicationReceiptCommitted;
    state.handoff_state = PrivateHandoffConsumptionState::PublicationMutationAuthorized;
}

OOCCleanupResult recover_private_lease_locked(const OOCCleanupPaths& paths,
                                              std::shared_ptr<BaseLock> lock,
                                              const OOCPrivateLeaseTestHooks& hooks) {
    if (paths.private_directory.empty() || !lock || !lock->matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }

    auto admission = admit_private_cleanup_action_locked(
        paths, std::move(lock), PrivateNamespaceAction::RecoverPrivateLease);
    if (admission.blocked) {
        return *admission.blocked;
    }
    if (!admission.permit) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    auto permit = std::move(*admission.permit);
    admission.permit.reset();
    const auto& held_lock =
        begin_private_cleanup_action(permit, paths, PrivateNamespaceAction::RecoverPrivateLease);
    if (invoke_with_stable_base_lock(held_lock, [&] {
            return should_interrupt_private_lease(
                hooks, OOCPrivateLeaseFaultPoint::RecoveryPermitAcquired);
        })) {
        return private_lease_interrupted();
    }

    const auto handoff =
        reconcile_private_handoff_from_permit(permit, PrivateNamespaceAction::RecoverPrivateLease);
    if (handoff.state != OOCPrivateHandoffState::None) {
        return handoff.result;
    }
    const auto parent = paths.private_directory.parent_path();
    sync_parent_directory(parent, OOCCleanupStage::None);
    const auto parent_identity = capture_directory_identity_locked(parent);

    auto reserved = load_optional_private_lease_marker(paths.lease_reserved_path);
    if (!reserved) {
        const auto pending = load_optional_private_lease_marker(paths.lease_reserved_pending_path);
        if (pending) {
            validate_private_lease_record_context(pending->record, paths, parent_identity,
                                                  held_lock.identity());
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
            invoke_with_stable_base_lock(held_lock, [&] {
                remove_matching_private_lease_pending(paths.lease_reserved_pending_path,
                                                      pending->record);
            });
            return private_lease_completed();
        }
    }

    auto owned = load_optional_private_lease_marker(paths.lease_owned_path);
    if (owned) {
        return recover_owned_private_lease_locked(paths, held_lock, parent_identity, *owned,
                                                  reserved, hooks);
    }

    if (reserved) {
        rollback_reserved_staging_locked(paths, held_lock, parent_identity, *reserved);
        held_lock.require_stable();
        return private_lease_completed();
    }

    const auto owned_pending = load_optional_private_lease_marker(paths.lease_owned_pending_path);
    if (owned_pending || inspect_directory_identity_locked(paths.private_directory)) {
        fail(OOCCleanupStatus::RecoveryRequired, OOCCleanupStage::None, protocol_error());
    }
    held_lock.require_stable();
    return private_lease_no_transaction();
}

OOCCleanupResult recover_private_lease_with_borrowed_base_lock_v1(
    const std::filesystem::path& base_path, OOCPrivateLeaseRecoveryBorrowedBaseLockV1&& borrowed,
    OOCPreactiveLeaseRecoveryExpectationV1 expectation, OOCPrivateLeaseTestHooks hooks) noexcept {
    return OOCPrivateLeaseRecoveryBuilderV1::invoke([&] {
#if defined(_WIN32)
        (void)base_path;
        (void)borrowed;
        (void)expectation;
        (void)hooks;
        fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None,
             std::make_error_code(std::errc::operation_not_supported));
#else
        const auto paths = freeze_paths(base_path);
        if (paths.private_directory.empty()) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        // Retain the WaveStore root from the token itself. The path below is
        // used only to prove that this inherited directory capability remains
        // the named parent; it is never opened to acquire cleanup authority.
        OOCPrivateLeaseRecoveryParentHandleV1 parent(borrowed.parent_descriptor_,
                                                     paths.lock_path.parent_path());
        auto lock = borrowed.consume(paths, parent.descriptor());
        if (!lock || !lock->matches(paths.lock_path)) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        BorrowedRecoveryHookContextV1 context{
            .paths = &paths,
            .lock = lock.get(),
            .expectation = &expectation,
            .user_hooks = hooks,
            .parent = &parent,
        };
        const auto recovered =
            recover_private_lease_locked(paths, lock,
                                         OOCPrivateLeaseTestHooks{
                                             .stop_after = borrowed_recovery_stop_after,
                                             .context = &context,
                                         });
        parent.require_stable();
        lock->require_stable();
        if (context.expectation_failure) {
            return *context.expectation_failure;
        }
        if (!context.expectation_checked &&
            (recovered.completed() || recovered.status == OOCCleanupStatus::Interrupted)) {
            fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
        }
        return recovered;
#endif
    });
}

PrivateCleanupUnionRawObservation
observe_private_cleanup_union_for_test(const std::filesystem::path& base_path,
                                       PrivateCleanupUnionObservationTestHooks hooks) {
    const auto paths = freeze_paths(base_path);
    if (paths.private_directory.empty()) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    BaseLock lock(paths.lock_path, false);
    return observe_private_cleanup_union_locked(paths, lock, hooks).raw;
}

OOCCleanupResult run_transaction(const std::filesystem::path& requested_base,
                                 const OOCCleanupRequest* request, bool allow_begin,
                                 const OwnershipProof* ownership_proof, bool* consume_receipt,
                                 const OOCCleanupTestHooks& hooks) {
    const OOCCleanupPaths paths = freeze_paths(requested_base);
    if (paths.private_directory.empty()) {
        BaseLock lock(paths.lock_path, true);
        return run_transaction_locked(paths, lock, request, allow_begin, ownership_proof,
                                      consume_receipt, hooks, false);
    }

    auto lock = std::make_shared<BaseLock>(paths.lock_path, false);
    auto admission = admit_private_cleanup_action_locked(paths, std::move(lock),
                                                         PrivateNamespaceAction::RunLegacyCleanup);
    if (admission.blocked) {
        return *admission.blocked;
    }
    if (!admission.permit) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    auto permit = std::move(*admission.permit);
    admission.permit.reset();
    const auto& held_lock =
        begin_private_cleanup_action(permit, paths, PrivateNamespaceAction::RunLegacyCleanup);
    if (invoke_with_stable_base_lock(held_lock, [&] {
            return should_interrupt(hooks, OOCCleanupFaultPoint::LegacyCleanupPermitAcquired);
        })) {
        return interrupted(OOCCleanupStage::None);
    }

    const auto handoff = inspect_private_handoff_from_permit(permit);
    if (handoff.state != OOCPrivateHandoffState::None) {
        return handoff.result;
    }
    PrivateCleanupMutationGate mutation_gate(permit);
    return run_transaction_locked(paths, held_lock, request, allow_begin, ownership_proof,
                                  consume_receipt, hooks, false, &mutation_gate);
}

std::optional<OOCCleanupResult> preflight_private_cleanup_union_for_transaction_locked(
    const OOCCleanupPaths& paths, const BaseLock& lock, bool publish_intent_only) {
    return project_private_cleanup_union_preflight(
        paths, lock,
        publish_intent_only ? PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff
                            : PrivateNamespaceAction::RunLegacyCleanup);
}

} // namespace gnfs::relation::ooc_cleanup_detail

namespace gnfs::relation {

OOCCleanupTransaction::PrivateLeaseActionAdmission
OOCCleanupTransaction::admit_private_lease_reservation_action(
    const OOCCleanupPaths& paths, std::shared_ptr<ooc_cleanup_detail::BaseLock> lock) noexcept {
    std::shared_ptr<ooc_cleanup_detail::PrivateCleanupActionPermit> retained;
    const auto result = invoke([&] {
        if (paths.private_directory.empty() || !lock || !lock->matches(paths.lock_path)) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        auto admission = ooc_cleanup_detail::admit_private_cleanup_action_locked(
            paths, lock, ooc_cleanup_detail::PrivateNamespaceAction::ReservePrivateLease);
        if (admission.blocked) {
            return *admission.blocked;
        }
        if (!admission.permit) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None,
                                     ooc_cleanup_detail::protocol_error());
        }
        auto permit = std::move(*admission.permit);
        admission.permit.reset();
        (void)ooc_cleanup_detail::begin_private_cleanup_action(
            permit, paths, ooc_cleanup_detail::PrivateNamespaceAction::ReservePrivateLease);
        const auto handoff = ooc_cleanup_detail::inspect_private_handoff_for_action_from_permit(
            permit, ooc_cleanup_detail::PrivateNamespaceAction::ReservePrivateLease);
        if (handoff.state != OOCPrivateHandoffState::None) {
            return handoff.result;
        }
        retained =
            std::make_shared<ooc_cleanup_detail::PrivateCleanupActionPermit>(std::move(permit));
        return ooc_cleanup_detail::private_lease_completed();
    });
    return PrivateLeaseActionAdmission(result, std::move(retained));
}

OOCCleanupResult OOCCleanupTransaction::initialize_private_lease_reservation_action(
    ooc_cleanup_detail::PrivateCleanupActionPermit& permit, const OOCCleanupPaths& paths,
    const ooc_cleanup_detail::BaseLock& lock,
    const ooc_cleanup_detail::PrivateLeaseRecord& reserved) noexcept {
    return invoke([&] {
        if (!lock.matches(paths.lock_path)) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        ooc_cleanup_detail::initialize_private_lease_reservation_permit(permit, reserved);
        lock.require_stable();
        return ooc_cleanup_detail::private_lease_completed();
    });
}

void OOCCleanupTransaction::private_lease_reservation_marker_transition(
    ooc_cleanup_detail::PrivateLeaseMarkerPublicationPoint point,
    const std::filesystem::path& canonical_path, const std::filesystem::path& pending_path,
    const ooc_cleanup_detail::PrivateLeaseRecord& record,
    const ooc_cleanup_detail::FileIdentity* identity, void* context) {
    auto* guard = static_cast<PrivateLeaseMarkerGuardContext*>(context);
    if (guard == nullptr || guard->permit == nullptr || guard->paths == nullptr ||
        guard->lock == nullptr || !guard->lock->matches(guard->paths->lock_path)) {
        ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                 ooc_cleanup_detail::invalid_argument_error());
    }
    ooc_cleanup_detail::advance_private_lease_reservation_marker(
        *guard->permit, point, canonical_path, pending_path, record, identity);
    guard->lock->require_stable();
}

OOCCleanupResult OOCCleanupTransaction::record_private_lease_reservation_directory(
    ooc_cleanup_detail::PrivateCleanupActionPermit& permit, const OOCCleanupPaths& paths,
    const ooc_cleanup_detail::BaseLock& lock, const std::filesystem::path& directory_path,
    const std::array<std::uint64_t, 3>& identity, bool final_directory) noexcept {
    return invoke([&] {
        if (!lock.matches(paths.lock_path)) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        ooc_cleanup_detail::record_private_lease_reservation_directory_successor(
            permit, directory_path, identity, final_directory);
        lock.require_stable();
        return ooc_cleanup_detail::private_lease_completed();
    });
}

OOCCleanupResult OOCCleanupTransaction::authorize_private_lease_reservation_staging_directory(
    ooc_cleanup_detail::PrivateCleanupActionPermit& permit, const OOCCleanupPaths& paths,
    const ooc_cleanup_detail::BaseLock& lock) noexcept {
    return invoke([&] {
        if (!lock.matches(paths.lock_path)) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        ooc_cleanup_detail::authorize_private_lease_reservation_staging_directory(permit);
        lock.require_stable();
        return ooc_cleanup_detail::private_lease_completed();
    });
}

OOCCleanupResult OOCCleanupTransaction::authorize_private_lease_reservation_final_rename(
    ooc_cleanup_detail::PrivateCleanupActionPermit& permit, const OOCCleanupPaths& paths,
    const ooc_cleanup_detail::BaseLock& lock) noexcept {
    return invoke([&] {
        if (!lock.matches(paths.lock_path)) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        ooc_cleanup_detail::authorize_private_lease_reservation_final_rename(permit);
        lock.require_stable();
        return ooc_cleanup_detail::private_lease_completed();
    });
}

OOCCleanupResult OOCCleanupTransaction::complete_private_lease_reservation_action(
    ooc_cleanup_detail::PrivateCleanupActionPermit& permit, const OOCCleanupPaths& paths,
    const ooc_cleanup_detail::BaseLock& lock) noexcept {
    return invoke([&] {
        if (!lock.matches(paths.lock_path)) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        ooc_cleanup_detail::complete_private_lease_reservation_permit(permit);
        lock.require_stable();
        return ooc_cleanup_detail::private_lease_completed();
    });
}

} // namespace gnfs::relation

namespace gnfs::relation::ooc_cleanup_detail {

class PathPrivateLeaseReservationTarget final {
public:
    PathPrivateLeaseReservationTarget(const OOCCleanupPaths& paths, BaseLock& lock,
                                      PrivateCleanupActionPermit& permit,
                                      const PrivateLeaseRecord& reserved,
                                      const std::filesystem::path& staging_path,
                                      const OOCPrivateLeaseTestHooks& hooks) noexcept
        : paths_(paths), lock_(lock), permit_(permit), reserved_(reserved),
          staging_path_(staging_path), hooks_(hooks) {}

    [[nodiscard]] bool checkpoint(PrivateLeaseReservationBoundary boundary) {
        const auto fault_point = [&] {
            switch (boundary) {
            case PrivateLeaseReservationBoundary::PermitAcquired:
                return OOCPrivateLeaseFaultPoint::ReservationPermitAcquired;
            case PrivateLeaseReservationBoundary::ReservedCanonicalDurable:
                return OOCPrivateLeaseFaultPoint::ReservedDurable;
            case PrivateLeaseReservationBoundary::StagingDirectoryDurable:
                return OOCPrivateLeaseFaultPoint::StagingDirectoryDurable;
            case PrivateLeaseReservationBoundary::OwnerCanonicalDurable:
                return OOCPrivateLeaseFaultPoint::OwnerDurable;
            case PrivateLeaseReservationBoundary::OwnedCanonicalDurable:
                return OOCPrivateLeaseFaultPoint::OwnedDurable;
            case PrivateLeaseReservationBoundary::FinalDirectoryDurable:
                return OOCPrivateLeaseFaultPoint::FinalRenameDurable;
            case PrivateLeaseReservationBoundary::ReservedPendingDurable:
            case PrivateLeaseReservationBoundary::OwnerPendingDurable:
            case PrivateLeaseReservationBoundary::OwnedPendingDurable:
            case PrivateLeaseReservationBoundary::Count:
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }();
        return invoke_with_stable_base_lock(
            lock_, [&] { return should_interrupt_private_lease(hooks_, fault_point); });
    }

    [[nodiscard]] bool publish_marker(PrivateLeaseReservationMarkerRole role,
                                      PrivateLeaseReservationBoundary pending_boundary) {
        switch (role) {
        case PrivateLeaseReservationMarkerRole::Reserved:
            require_pending_boundary(pending_boundary,
                                     PrivateLeaseReservationBoundary::ReservedPendingDurable);
            return publish_reserved();
        case PrivateLeaseReservationMarkerRole::Owner:
            require_pending_boundary(pending_boundary,
                                     PrivateLeaseReservationBoundary::OwnerPendingDurable);
            return publish_owner();
        case PrivateLeaseReservationMarkerRole::Owned:
            require_pending_boundary(pending_boundary,
                                     PrivateLeaseReservationBoundary::OwnedPendingDurable);
            return publish_owned();
        case PrivateLeaseReservationMarkerRole::Count:
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }

    void create_staging_directory() {
        if (directory_identity_) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        require_completed(
            OOCCleanupTransaction::authorize_private_lease_reservation_staging_directory(
                permit_, paths_, lock_));
        invoke_with_stable_base_lock(lock_,
                                     [&] { create_directory_durable_locked(staging_path_); });
        directory_identity_ = capture_directory_identity_locked(staging_path_);
        require_completed(OOCCleanupTransaction::record_private_lease_reservation_directory(
            permit_, paths_, lock_, staging_path_, *directory_identity_, false));
    }

    void promote_final_directory() {
        if (!directory_identity_ || !owner_ || !owner_identity_ || !owned_ || !owned_identity_) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        require_completed(OOCCleanupTransaction::authorize_private_lease_reservation_final_rename(
            permit_, paths_, lock_));
        const auto renamed = invoke_with_stable_base_lock(
            lock_, [&] { return rename_no_replace(staging_path_, paths_.private_directory); });
        switch (renamed.result) {
        case RenameResult::Succeeded:
            invoke_with_stable_base_lock(lock_, [&] {
                sync_parent_directory(paths_.private_directory.parent_path(),
                                      OOCCleanupStage::None);
            });
            break;
        case RenameResult::DestinationExists:
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, renamed.error);
        case RenameResult::Unsupported:
            fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None, renamed.error);
        case RenameResult::Failed:
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, renamed.error);
        }
        if (inspect_directory_identity_locked(staging_path_) ||
            capture_directory_identity_locked(paths_.private_directory) != *directory_identity_) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        validate_private_lease_owner_at(paths_.private_directory, *owned_);
        require_completed(OOCCleanupTransaction::record_private_lease_reservation_directory(
            permit_, paths_, lock_, paths_.private_directory, *directory_identity_, true));
    }

    void complete() {
        require_completed(OOCCleanupTransaction::complete_private_lease_reservation_action(
            permit_, paths_, lock_));
    }

    [[nodiscard]] const std::array<std::uint64_t, 3>& directory_identity() const {
        return require_identity(directory_identity_);
    }

    [[nodiscard]] const std::array<std::uint64_t, 3>& owner_identity() const {
        return require_identity(owner_identity_);
    }

    [[nodiscard]] const std::array<std::uint64_t, 3>& owned_identity() const {
        return require_identity(owned_identity_);
    }

private:
    static void require_pending_boundary(PrivateLeaseReservationBoundary actual,
                                         PrivateLeaseReservationBoundary expected) {
        if (actual != expected) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
    }

    static void require_completed(const OOCCleanupResult& result) {
        if (!result.completed()) {
            fail(result.status, result.stage, result.native_error);
        }
    }

    [[nodiscard]] static const std::array<std::uint64_t, 3>&
    require_identity(const std::optional<std::array<std::uint64_t, 3>>& identity) {
        if (!identity) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        return *identity;
    }

    [[nodiscard]] PrivateLeaseMarkerPublicationGuard
    marker_guard(OOCCleanupTransaction::PrivateLeaseMarkerGuardContext& context) const noexcept {
        context = {
            .permit = &permit_,
            .paths = &paths_,
            .lock = &lock_,
        };
        return {
            .transition = &OOCCleanupTransaction::private_lease_reservation_marker_transition,
            .context = &context,
        };
    }

    [[nodiscard]] bool publish_reserved() {
        OOCCleanupTransaction::PrivateLeaseMarkerGuardContext context;
        const auto guard = marker_guard(context);
        const auto publication = publish_private_lease_marker_durable(
            paths_.lease_reserved_path, paths_.lease_reserved_pending_path, reserved_, lock_,
            hooks_, OOCPrivateLeaseFaultPoint::ReservedPendingDurable, &guard);
        return publication.interrupted;
    }

    [[nodiscard]] bool publish_owner() {
        if (!directory_identity_ || owner_ || owner_identity_) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        auto owner = make_private_lease_owner_record(reserved_, *directory_identity_);
        OOCCleanupTransaction::PrivateLeaseMarkerGuardContext context;
        const auto guard = marker_guard(context);
        const auto publication = publish_private_lease_marker_durable(
            private_lease_owner_path(staging_path_),
            private_lease_owner_pending_path(staging_path_), owner, lock_, hooks_,
            OOCPrivateLeaseFaultPoint::OwnerPendingDurable, &guard);
        if (!publication.interrupted) {
            owner_ = std::move(owner);
            owner_identity_ = publication.identity;
        }
        return publication.interrupted;
    }

    [[nodiscard]] bool publish_owned() {
        if (!owner_ || !owner_identity_ || owned_ || owned_identity_) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        auto owned = make_private_lease_owned_record(*owner_, *owner_identity_);
        OOCCleanupTransaction::PrivateLeaseMarkerGuardContext context;
        const auto guard = marker_guard(context);
        const auto publication = publish_private_lease_marker_durable(
            paths_.lease_owned_path, paths_.lease_owned_pending_path, owned, lock_, hooks_,
            OOCPrivateLeaseFaultPoint::OwnedPendingDurable, &guard);
        if (!publication.interrupted) {
            owned_ = std::move(owned);
            owned_identity_ = publication.identity;
        }
        return publication.interrupted;
    }

    const OOCCleanupPaths& paths_;
    BaseLock& lock_;
    PrivateCleanupActionPermit& permit_;
    const PrivateLeaseRecord& reserved_;
    const std::filesystem::path& staging_path_;
    const OOCPrivateLeaseTestHooks& hooks_;
    std::optional<std::array<std::uint64_t, 3>> directory_identity_;
    std::optional<PrivateLeaseRecord> owner_;
    std::optional<std::array<std::uint64_t, 3>> owner_identity_;
    std::optional<PrivateLeaseRecord> owned_;
    std::optional<std::array<std::uint64_t, 3>> owned_identity_;
};

} // namespace gnfs::relation::ooc_cleanup_detail

namespace gnfs::relation {

OOCPrivateLeaseReservation
OOCCleanupTransaction::reserve_private_lease(const std::filesystem::path& base_path,
                                             OOCPrivateLeaseTestHooks hooks) noexcept {
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
        if (initial_lock.kind == ooc_cleanup_detail::InspectKind::Missing &&
            ooc_cleanup_detail::private_protocol_artifact_exists_without_lock(paths)) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                     ooc_cleanup_detail::protocol_error());
        }
        if (initial_lock.kind == ooc_cleanup_detail::InspectKind::Rejected) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                     ooc_cleanup_detail::protocol_error());
        }

        auto live_lock = std::make_shared<ooc_cleanup_detail::BaseLock>(paths.lock_path);
        const auto recovered = ooc_cleanup_detail::recover_private_lease_locked(paths, live_lock);
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

        const auto parent_identity = ooc_cleanup_detail::capture_directory_identity_locked(parent);
        const auto lease_id = ooc_cleanup_detail::allocate_private_lease_id();
        const auto staging_path = ooc_cleanup_detail::private_lease_staging_path(paths, lease_id);
        if (ooc_cleanup_detail::inspect_directory_identity_locked(staging_path)) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                     ooc_cleanup_detail::protocol_error());
        }
        const auto reserved = ooc_cleanup_detail::make_private_lease_reserved_record(
            paths, lease_id, parent_identity, live_lock->identity());
        auto reservation_admission = admit_private_lease_reservation_action(paths, live_lock);
        if (!reservation_admission.admitted()) {
            return reservation_admission.result;
        }
        auto& reservation_permit = *reservation_admission.permit;
        const auto initialized = initialize_private_lease_reservation_action(
            reservation_permit, paths, *live_lock, reserved);
        if (!initialized.completed()) {
            return initialized;
        }

        ooc_cleanup_detail::PathPrivateLeaseReservationTarget target(
            paths, *live_lock, reservation_permit, reserved, staging_path, hooks);
        if (ooc_cleanup_detail::run_private_lease_reservation_protocol(target) ==
            ooc_cleanup_detail::PrivateLeaseReservationRunResult::Interrupted) {
            return ooc_cleanup_detail::private_lease_interrupted();
        }

        live_lock->require_stable();
        const auto directory_identity = target.directory_identity();
        const auto owner_identity = target.owner_identity();
        const auto owned_identity = target.owned_identity();
        OOCPrivateLeaseOwnershipReceipt candidate(
            paths.base_path, paths.private_directory, paths.lock_path, directory_identity, lease_id,
            owner_identity, owned_identity, std::move(live_lock),
            static_cast<std::uint64_t>(gnfs::util::process_id()));
        static_assert(std::is_nothrow_move_constructible_v<OOCPrivateLeaseOwnershipReceipt>);
        ownership.emplace(std::move(candidate));
        return ooc_cleanup_detail::private_lease_completed();
    });
    return OOCPrivateLeaseReservation{
        .result = result,
        .ownership = std::move(ownership),
    };
}

OOCCleanupTransaction::PrivateLeaseActionAdmission
OOCCleanupTransaction::admit_private_fresh_writer_action(
    const OOCPrivateLeaseOwnershipReceipt& lease, bool deferred_mode) noexcept {
    std::shared_ptr<ooc_cleanup_detail::PrivateCleanupActionPermit> retained;
    const auto result = invoke([&] {
        if (lease.spent_ || lease.active_ || !lease.live_lock_ || lease.base_path_.empty() ||
            lease.private_directory_.empty() || lease.lock_path_.empty()) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        const auto paths = ooc_cleanup_detail::freeze_paths(lease.base_path_);
        if (paths.base_path != lease.base_path_ ||
            paths.private_directory != lease.private_directory_ ||
            paths.lock_path != lease.lock_path_ || !lease.live_lock_->matches(paths.lock_path)) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        auto admission = ooc_cleanup_detail::admit_private_cleanup_action_locked(
            paths, lease.live_lock_,
            ooc_cleanup_detail::PrivateNamespaceAction::ValidateFreshWriter);
        if (admission.blocked) {
            return *admission.blocked;
        }
        if (!admission.permit) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None,
                                     ooc_cleanup_detail::protocol_error());
        }
        auto permit = std::move(*admission.permit);
        admission.permit.reset();
        (void)ooc_cleanup_detail::begin_private_cleanup_action(
            permit, paths, ooc_cleanup_detail::PrivateNamespaceAction::ValidateFreshWriter);
        const auto handoff = ooc_cleanup_detail::inspect_private_handoff_for_action_from_permit(
            permit, ooc_cleanup_detail::PrivateNamespaceAction::ValidateFreshWriter);
        if (handoff.state != OOCPrivateHandoffState::None) {
            return handoff.result;
        }
        ooc_cleanup_detail::initialize_private_fresh_writer_permit(
            permit, lease.lease_id_, lease.directory_identity_, lease.owner_identity_,
            lease.owned_identity_, deferred_mode, lease.owner_process_id_);
        retained =
            std::make_shared<ooc_cleanup_detail::PrivateCleanupActionPermit>(std::move(permit));
        return ooc_cleanup_detail::private_lease_completed();
    });
    return PrivateLeaseActionAdmission(result, std::move(retained));
}

OOCCleanupResult OOCCleanupTransaction::advance_private_fresh_writer_action(
    ooc_cleanup_detail::PrivateCleanupActionPermit& permit,
    const OOCPrivateLeaseOwnershipReceipt& lease, PrivateFreshWriterBoundary boundary,
    std::optional<std::array<std::uint64_t, 3>> index_identity,
    std::optional<std::array<std::uint64_t, 3>> data_identity, std::uint64_t store_id) noexcept {
    return invoke([&] {
        if (!lease.live_lock_) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        ooc_cleanup_detail::PrivateFreshWriterPermitBoundary internal_boundary;
        switch (boundary) {
        case PrivateFreshWriterBoundary::BeforeIndexReservation:
            internal_boundary =
                ooc_cleanup_detail::PrivateFreshWriterPermitBoundary::BeforeIndexReservation;
            break;
        case PrivateFreshWriterBoundary::IndexReserved:
            internal_boundary = ooc_cleanup_detail::PrivateFreshWriterPermitBoundary::IndexReserved;
            break;
        case PrivateFreshWriterBoundary::BeforeDataReservation:
            internal_boundary =
                ooc_cleanup_detail::PrivateFreshWriterPermitBoundary::BeforeDataReservation;
            break;
        case PrivateFreshWriterBoundary::DataReserved:
            internal_boundary = ooc_cleanup_detail::PrivateFreshWriterPermitBoundary::DataReserved;
            break;
        case PrivateFreshWriterBoundary::BeforeHeaderWrite:
            internal_boundary =
                ooc_cleanup_detail::PrivateFreshWriterPermitBoundary::BeforeHeaderWrite;
            break;
        case PrivateFreshWriterBoundary::HeadersValidated:
            internal_boundary =
                ooc_cleanup_detail::PrivateFreshWriterPermitBoundary::HeadersValidated;
            break;
        case PrivateFreshWriterBoundary::PairOwnershipCaptured:
            internal_boundary =
                ooc_cleanup_detail::PrivateFreshWriterPermitBoundary::PairOwnershipCaptured;
            break;
        case PrivateFreshWriterBoundary::Complete:
            internal_boundary = ooc_cleanup_detail::PrivateFreshWriterPermitBoundary::Complete;
            break;
        default:
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        ooc_cleanup_detail::advance_private_fresh_writer_permit(
            permit, internal_boundary, index_identity, data_identity, store_id);
        lease.live_lock_->require_stable();
        return ooc_cleanup_detail::private_lease_completed();
    });
}

bool OOCCleanupTransaction::private_fresh_writer_rollback_allowed(
    ooc_cleanup_detail::PrivateCleanupActionPermit& permit,
    const OOCPrivateLeaseOwnershipReceipt& lease,
    std::optional<std::array<std::uint64_t, 3>> index_identity,
    std::optional<std::array<std::uint64_t, 3>> data_identity) noexcept {
    return lease.live_lock_ && ooc_cleanup_detail::private_fresh_writer_permit_allows_rollback(
                                   permit, index_identity, data_identity);
}

OOCCleanupTransaction::PrivateLeaseActionAdmission
OOCCleanupTransaction::admit_private_lease_activation_action(
    const OOCPrivateLeaseOwnershipReceipt& lease,
    const OOCCleanupOwnershipReceipt& pair_ownership) noexcept {
    std::shared_ptr<ooc_cleanup_detail::PrivateCleanupActionPermit> retained;
    const auto result = invoke([&] {
        if (lease.spent_ || lease.active_ || !lease.live_lock_ || pair_ownership.spent_ ||
            pair_ownership.store_id_ == 0 ||
            lease.owner_process_id_ != static_cast<std::uint64_t>(gnfs::util::process_id())) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        const auto paths = ooc_cleanup_detail::freeze_paths(lease.base_path_);
        if (paths.base_path != lease.base_path_ ||
            paths.private_directory != lease.private_directory_ ||
            paths.lock_path != lease.lock_path_ || pair_ownership.base_path_ != lease.base_path_ ||
            !lease.live_lock_->matches(paths.lock_path)) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        auto admission = ooc_cleanup_detail::admit_private_cleanup_action_locked(
            paths, lease.live_lock_,
            ooc_cleanup_detail::PrivateNamespaceAction::ActivateFreshLease);
        if (admission.blocked) {
            return *admission.blocked;
        }
        if (!admission.permit) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None,
                                     ooc_cleanup_detail::protocol_error());
        }
        auto permit = std::move(*admission.permit);
        admission.permit.reset();
        (void)ooc_cleanup_detail::begin_private_cleanup_action(
            permit, paths, ooc_cleanup_detail::PrivateNamespaceAction::ActivateFreshLease);
        const auto handoff = ooc_cleanup_detail::inspect_private_handoff_for_action_from_permit(
            permit, ooc_cleanup_detail::PrivateNamespaceAction::ActivateFreshLease);
        if (handoff.state != OOCPrivateHandoffState::None) {
            return handoff.result;
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
        ooc_cleanup_detail::initialize_private_lease_activation_permit(
            permit, lease.lease_id_, lease.directory_identity_, lease.owner_identity_,
            lease.owned_identity_, proof);
        retained =
            std::make_shared<ooc_cleanup_detail::PrivateCleanupActionPermit>(std::move(permit));
        return ooc_cleanup_detail::private_lease_completed();
    });
    return PrivateLeaseActionAdmission(result, std::move(retained));
}

OOCCleanupResult OOCCleanupTransaction::advance_private_lease_activation_action(
    ooc_cleanup_detail::PrivateCleanupActionPermit& permit,
    const OOCPrivateLeaseOwnershipReceipt& lease, const OOCCleanupOwnershipReceipt& pair_ownership,
    PrivateLeaseActivationBoundary boundary) noexcept {
    return invoke([&] {
        if ((boundary == PrivateLeaseActivationBoundary::BeforeReservedRemoval &&
             !lease.live_lock_) ||
            pair_ownership.spent_) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        ooc_cleanup_detail::PrivateLeaseActivationPermitBoundary internal_boundary;
        switch (boundary) {
        case PrivateLeaseActivationBoundary::BeforeReservedRemoval:
            internal_boundary =
                ooc_cleanup_detail::PrivateLeaseActivationPermitBoundary::BeforeReservedRemoval;
            break;
        case PrivateLeaseActivationBoundary::Complete:
            internal_boundary = ooc_cleanup_detail::PrivateLeaseActivationPermitBoundary::Complete;
            break;
        default:
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        ooc_cleanup_detail::advance_private_lease_activation_permit(permit, internal_boundary);
        if (lease.live_lock_) {
            lease.live_lock_->require_stable();
        }
        return ooc_cleanup_detail::private_lease_completed();
    });
}

bool OOCCleanupTransaction::commit_private_lease_activation_noexcept(
    ooc_cleanup_detail::PrivateCleanupActionPermit& permit) noexcept {
    return ooc_cleanup_detail::commit_private_lease_activation_permit_noexcept(permit);
}

OOCCleanupResult OOCCleanupTransaction::commit_private_lease_activation_action(
    ooc_cleanup_detail::PrivateCleanupActionPermit& permit, OOCPrivateLeaseOwnershipReceipt& lease,
    OOCPrivateLeaseTestHooks hooks) noexcept {
    return invoke([&] {
        if (!lease.live_lock_) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        auto retained_lock = lease.live_lock_;
        const auto removal =
            ooc_cleanup_detail::private_lease_activation_reserved_removal_proof(permit);
        bool stop_after_commit = false;
        bool permit_commit_ok = false;
        ooc_cleanup_detail::invoke_with_stable_base_lock(*retained_lock, [&] {
            ooc_cleanup_detail::remove_private_lease_marker_durable_impl(
                ooc_cleanup_detail::freeze_paths(lease.base_path_).lease_reserved_path,
                removal.record, removal.identity, [&]() noexcept {
                    permit_commit_ok = commit_private_lease_activation_noexcept(permit);
                    lease.active_ = true;
                    lease.live_lock_.reset();
                    stop_after_commit = ooc_cleanup_detail::should_interrupt_private_lease(
                        hooks, OOCPrivateLeaseFaultPoint::ReservedRemovedDurable);
                });
        });
        if (stop_after_commit) {
            return ooc_cleanup_detail::private_lease_interrupted();
        }
        if (!permit_commit_ok) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::ForeignReplacementPreserved,
                                     OOCCleanupStage::None, ooc_cleanup_detail::protocol_error());
        }
        return ooc_cleanup_detail::private_lease_completed();
    });
}

OOCCleanupResult OOCCleanupTransaction::publish_private_lease_cleanup_handoff(
    OOCCleanupOwnershipReceipt& pair_ownership, OOCPrivateLeaseOwnershipReceipt& lease,
    const OOCExactCleanupExpectation& exact, OOCCleanupTestHooks hooks) noexcept {
    if (pair_ownership.spent_ || pair_ownership.store_id_ == 0 || lease.spent_ || lease.active_ ||
        !lease.live_lock_ || !ooc_cleanup_detail::expectation_is_well_formed(exact)) {
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
    const ooc_cleanup_detail::OwnershipProof ownership_proof{
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
    const auto lease_base_path = lease.base_path_;
    const auto lease_private_directory = lease.private_directory_;
    const auto lease_lock_path = lease.lock_path_;
    const auto expected_lease_id = lease.lease_id_;
    const auto expected_directory_identity = lease.directory_identity_;
    const auto expected_owner_identity = lease.owner_identity_;
    const auto expected_owned_identity = lease.owned_identity_;
    auto retained_lock = lease.live_lock_;

    // The caller escrows this receipt outside the writer before entering here.
    // Mark it unavailable during every test callback; only a pre-canonical
    // return may roll this in-memory attempt back to fresh.
    pair_ownership.spent_ = true;
    bool consume_receipt = false;
    const auto result = invoke([&] {
        const auto paths = ooc_cleanup_detail::freeze_paths(lease_base_path);
        if (paths.base_path != lease_base_path ||
            paths.private_directory != lease_private_directory ||
            paths.lock_path != lease_lock_path || request.base_path != lease_base_path ||
            !retained_lock || !retained_lock->matches(paths.lock_path)) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }

        auto admission = ooc_cleanup_detail::admit_private_lease_cleanup_handoff_locked(
            paths, retained_lock, request, ownership_proof, expected_lease_id,
            expected_directory_identity, expected_owner_identity, expected_owned_identity);
        if (admission.blocked) {
            return *admission.blocked;
        }
        if (!admission.permit) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None,
                                     ooc_cleanup_detail::protocol_error());
        }
        auto permit = std::move(*admission.permit);
        admission.permit.reset();
        const auto& lock = ooc_cleanup_detail::begin_private_cleanup_action(
            permit, paths,
            ooc_cleanup_detail::PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff);
        if (ooc_cleanup_detail::invoke_with_stable_base_lock(lock, [&] {
                return ooc_cleanup_detail::should_interrupt(
                    hooks, OOCCleanupFaultPoint::PrivateLeaseCleanupHandoffPermitAcquired);
            })) {
            return ooc_cleanup_detail::interrupted(OOCCleanupStage::None);
        }

        const auto handoff =
            ooc_cleanup_detail::inspect_private_lease_cleanup_handoff_from_permit(permit);
        if (handoff.state != OOCPrivateHandoffState::None) {
            return handoff.result;
        }
        ooc_cleanup_detail::PrivateCleanupMutationGate mutation_gate(permit);
        return ooc_cleanup_detail::run_transaction_locked(paths, lock, &request, true,
                                                          &ownership_proof, &consume_receipt, hooks,
                                                          true, &mutation_gate);
    });
    pair_ownership.spent_ = consume_receipt;
    return result;
}

OOCCleanupResult
OOCCleanupTransaction::remove_private_lease(OOCPrivateLeaseOwnershipReceipt& ownership,
                                            OOCPrivateLeaseTestHooks hooks) noexcept {
    if (ownership.spent_ || ownership.base_path_.empty() || ownership.private_directory_.empty() ||
        ownership.lock_path_.empty() ||
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

        auto admission = ooc_cleanup_detail::admit_private_lease_removal_locked(
            paths, held_lock, ownership.lease_id_, ownership.directory_identity_,
            ownership.owner_identity_, ownership.owned_identity_);
        if (admission.blocked) {
            return *admission.blocked;
        }
        if (!admission.permit || !admission.generation) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None,
                                     ooc_cleanup_detail::protocol_error());
        }
        auto generation = std::move(*admission.generation);
        admission.generation.reset();
        auto permit = std::move(*admission.permit);
        admission.permit.reset();
        const auto& retained_lock = ooc_cleanup_detail::begin_private_cleanup_action(
            permit, paths, ooc_cleanup_detail::PrivateNamespaceAction::RemovePrivateLease);
        ooc_cleanup_detail::bind_private_lease_removal_generation(permit, generation);
        if (ooc_cleanup_detail::invoke_with_stable_base_lock(retained_lock, [&] {
                return ooc_cleanup_detail::should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::RemovalPermitAcquired);
            })) {
            return ooc_cleanup_detail::private_lease_interrupted();
        }

        const auto handoff = ooc_cleanup_detail::reconcile_private_handoff_from_permit(
            permit, ooc_cleanup_detail::PrivateNamespaceAction::RemovePrivateLease);
        if (handoff.state != OOCPrivateHandoffState::None) {
            return handoff.result;
        }
        if (!generation.owned) {
            ooc_cleanup_detail::invoke_with_stable_base_lock(retained_lock, [&] {
                ooc_cleanup_detail::sync_parent_directory(paths.private_directory.parent_path(),
                                                          OOCCleanupStage::None);
            });
            return ooc_cleanup_detail::private_lease_completed();
        }

        return ooc_cleanup_detail::recover_owned_private_lease_locked(
            paths, retained_lock, generation.parent_identity, *generation.owned,
            generation.reserved, hooks);
    });
    if (result.completed()) {
        ownership.spent_ = true;
        ownership.live_lock_.reset();
    }
    return result;
}

} // namespace gnfs::relation
