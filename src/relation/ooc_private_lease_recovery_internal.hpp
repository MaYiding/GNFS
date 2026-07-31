#pragma once

/// @file ooc_private_lease_recovery_internal.hpp
/// @brief Source-private recovery bridge for one already-held BaseLock OFD.

#include <gnfs/relation/ooc_cleanup_transaction.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace gnfs::sieve::distributed_sieve_resume_detail {
class DistributedSievePrivateLeaseBaseLockAt;
}

namespace gnfs::relation::ooc_cleanup_detail {

class OOCPrivateLeaseRecoveryBuilderV1;

enum class OOCPreactiveLeaseRecoveryPhaseV1 : std::uint8_t {
    FinalDirectoryRawPair,
    StagingDirectoryRawPair,
    StagingDirectoryOwnerOnly,
    StagingDirectoryOwnerRemoved,
    DirectoryAbsentReservedAndOwned,
    DirectoryAbsentOwnedOnly,
    Count,
};

struct OOCPreactiveLeaseRecoveryFileExpectationV1 final {
    std::array<std::uint64_t, 3> identity{};
    std::uint64_t extent = 0;

    friend bool operator==(const OOCPreactiveLeaseRecoveryFileExpectationV1&,
                           const OOCPreactiveLeaseRecoveryFileExpectationV1&) = default;
};

/// Exact, authority-narrowing witness supplied by the WaveStore aggregate.
///
/// `owner_marker_identity` remains populated after the owner leaf is removed:
/// the exact OWNED record still binds that historical identity. Marker presence
/// and raw-pair presence are closed by `phase`; optional fields cannot add a
/// state outside that phase. This witness never grants path or lock authority.
struct OOCPreactiveLeaseRecoveryExpectationV1 final {
    OOCPreactiveLeaseRecoveryPhaseV1 phase = OOCPreactiveLeaseRecoveryPhaseV1::Count;
    std::array<std::uint64_t, 2> lease_id{};
    std::array<std::uint64_t, 3> directory_identity{};
    std::array<std::uint64_t, 3> owner_marker_identity{};
    std::array<std::uint64_t, 3> owned_marker_identity{};
    std::optional<std::array<std::uint64_t, 3>> reserved_marker_identity;
    std::optional<OOCPreactiveLeaseRecoveryFileExpectationV1> index;
    std::optional<OOCPreactiveLeaseRecoveryFileExpectationV1> data;

    friend bool operator==(const OOCPreactiveLeaseRecoveryExpectationV1&,
                           const OOCPreactiveLeaseRecoveryExpectationV1&) = default;
};

/// One-shot authority for duplicating the exact BaseLock open-file description
/// already held by WaveStore. It cannot open, flock, or identify a lock by path.
/// The source-private WaveStore BaseLock capability is its only mint.
class OOCPrivateLeaseRecoveryBorrowedBaseLockV1 final {
public:
    OOCPrivateLeaseRecoveryBorrowedBaseLockV1() = delete;
    OOCPrivateLeaseRecoveryBorrowedBaseLockV1(const OOCPrivateLeaseRecoveryBorrowedBaseLockV1&) =
        delete;
    OOCPrivateLeaseRecoveryBorrowedBaseLockV1&
    operator=(const OOCPrivateLeaseRecoveryBorrowedBaseLockV1&) = delete;

    OOCPrivateLeaseRecoveryBorrowedBaseLockV1(
        OOCPrivateLeaseRecoveryBorrowedBaseLockV1&& other) noexcept;
    OOCPrivateLeaseRecoveryBorrowedBaseLockV1&
    operator=(OOCPrivateLeaseRecoveryBorrowedBaseLockV1&&) = delete;
    ~OOCPrivateLeaseRecoveryBorrowedBaseLockV1() = default;

private:
    OOCPrivateLeaseRecoveryBorrowedBaseLockV1(int parent_descriptor, int lock_descriptor,
                                              std::string_view lock_leaf,
                                              std::array<std::uint64_t, 3> lock_identity,
                                              std::uint64_t creator_process_id) noexcept;

    [[nodiscard]] std::shared_ptr<BaseLock> consume(const OOCCleanupPaths& paths,
                                                    int retained_parent_descriptor);

    int parent_descriptor_ = -1;
    int lock_descriptor_ = -1;
    // The view is minted from the owning WaveStore BaseLockAt::leaf_ and is
    // consumed synchronously while that source capability remains alive.
    std::string_view lock_leaf_;
    std::array<std::uint64_t, 3> lock_identity_{};
    std::uint64_t creator_process_id_ = 0;
    bool consumed_ = false;

    friend class ::gnfs::sieve::distributed_sieve_resume_detail::
        DistributedSievePrivateLeaseBaseLockAt;
    friend OOCCleanupResult recover_private_lease_with_borrowed_base_lock_v1(
        const std::filesystem::path& base_path,
        OOCPrivateLeaseRecoveryBorrowedBaseLockV1&& borrowed,
        OOCPreactiveLeaseRecoveryExpectationV1 expectation,
        OOCPrivateLeaseTestHooks hooks) noexcept;
};

/// Run the ordinary preactive recovery state machine without acquiring another
/// BaseLock. The exact expectation is checked after relation action admission
/// and before its first mutation. Failure never widens generic public cleanup.
[[nodiscard]] OOCCleanupResult recover_private_lease_with_borrowed_base_lock_v1(
    const std::filesystem::path& base_path, OOCPrivateLeaseRecoveryBorrowedBaseLockV1&& borrowed,
    OOCPreactiveLeaseRecoveryExpectationV1 expectation,
    OOCPrivateLeaseTestHooks hooks = {}) noexcept;

} // namespace gnfs::relation::ooc_cleanup_detail
