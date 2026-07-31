#pragma once

// Source-private bridge for validating a canonical private handoff while the
// caller already owns the exact persistent BaseLock open-file description.

#include <gnfs/relation/ooc_cleanup_transaction.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace gnfs::sieve::distributed_sieve_resume_detail {
class DistributedSievePrivateLeaseBaseLockAt;
}

namespace gnfs::relation::ooc_cleanup_detail {

struct PrivateHandoffPublicationPrefixWitnessV1;
class PrivateHandoffPublicationValidatedPermitV1;

/// One-shot borrowed authority for the exact BaseLock already held by a
/// WaveStore attempt claim. The bridge duplicates the retained descriptor so
/// the adoption receipt shares the existing POSIX open-file description and
/// therefore never issues a competing flock against the same process.
///
/// The token exposes no descriptor or path accessor and can be minted only by
/// the source-private WaveStore BaseLock capability.
class OOCPrivateHandoffBorrowedBaseLockV1 final {
public:
    OOCPrivateHandoffBorrowedBaseLockV1() = delete;
    OOCPrivateHandoffBorrowedBaseLockV1(const OOCPrivateHandoffBorrowedBaseLockV1&) = delete;
    OOCPrivateHandoffBorrowedBaseLockV1&
    operator=(const OOCPrivateHandoffBorrowedBaseLockV1&) = delete;

    OOCPrivateHandoffBorrowedBaseLockV1(OOCPrivateHandoffBorrowedBaseLockV1&& other) noexcept;
    OOCPrivateHandoffBorrowedBaseLockV1& operator=(OOCPrivateHandoffBorrowedBaseLockV1&&) = delete;
    ~OOCPrivateHandoffBorrowedBaseLockV1() = default;

private:
    OOCPrivateHandoffBorrowedBaseLockV1(int parent_descriptor, int lock_descriptor,
                                        std::string_view lock_leaf,
                                        std::array<std::uint64_t, 3> lock_identity,
                                        std::uint64_t creator_process_id) noexcept;

    [[nodiscard]] std::shared_ptr<BaseLock> consume(const OOCCleanupPaths& paths,
                                                    AdoptionParentDirectoryHandle& parent);

    int parent_descriptor_ = -1;
    int lock_descriptor_ = -1;
    std::string_view lock_leaf_;
    std::array<std::uint64_t, 3> lock_identity_{};
    std::uint64_t creator_process_id_ = 0;
    bool consumed_ = false;

    friend class ::gnfs::sieve::distributed_sieve_resume_detail::
        DistributedSievePrivateLeaseBaseLockAt;
    friend OOCPrivateHandoffAdoptionResult adopt_private_handoff_with_borrowed_base_lock_v1(
        const std::filesystem::path& base_path, OOCPrivateHandoffBorrowedBaseLockV1&& borrowed,
        OOCPrivateHandoffAdoptionTestHooks hooks) noexcept;
};

/// Validate and adopt one exact canonical handoff without reacquiring the
/// already-held BaseLock. Success retains only a duplicated descriptor that
/// shares the caller's open-file description; destruction remains close-only.
[[nodiscard]] OOCPrivateHandoffAdoptionResult adopt_private_handoff_with_borrowed_base_lock_v1(
    const std::filesystem::path& base_path, OOCPrivateHandoffBorrowedBaseLockV1&& borrowed,
    OOCPrivateHandoffAdoptionTestHooks hooks = {}) noexcept;

/// One-shot authority carrying aliases into a consumed publication permit's
/// retained State. Both aliases share that State's control block: no BaseLock,
/// descriptor, open-file description, or action claim is recreated.
class OOCPrivateHandoffConsumedPublicationBaseLockV1 final {
public:
    OOCPrivateHandoffConsumedPublicationBaseLockV1() = delete;
    OOCPrivateHandoffConsumedPublicationBaseLockV1(
        const OOCPrivateHandoffConsumedPublicationBaseLockV1&) = delete;
    OOCPrivateHandoffConsumedPublicationBaseLockV1&
    operator=(const OOCPrivateHandoffConsumedPublicationBaseLockV1&) = delete;

    OOCPrivateHandoffConsumedPublicationBaseLockV1(
        OOCPrivateHandoffConsumedPublicationBaseLockV1&& other) noexcept;
    OOCPrivateHandoffConsumedPublicationBaseLockV1&
    operator=(OOCPrivateHandoffConsumedPublicationBaseLockV1&&) = delete;
    ~OOCPrivateHandoffConsumedPublicationBaseLockV1() = default;

private:
    OOCPrivateHandoffConsumedPublicationBaseLockV1(
        std::shared_ptr<BaseLock> live_lock,
        std::shared_ptr<const PrivateHandoffPublicationPrefixWitnessV1> terminal,
        std::uint64_t creator_process_id) noexcept;

    std::shared_ptr<BaseLock> live_lock_;
    std::shared_ptr<const PrivateHandoffPublicationPrefixWitnessV1> terminal_;
    std::uint64_t creator_process_id_ = 0;
    bool consumed_ = false;

    friend OOCPrivateHandoffAdoptionResult adopt_consumed_canonical_private_handoff_publication_v1(
        PrivateHandoffPublicationValidatedPermitV1&& permit,
        OOCPrivateHandoffAdoptionTestHooks hooks) noexcept;
    friend OOCPrivateHandoffAdoptionResult
    adopt_private_handoff_with_consumed_publication_base_lock_v1(
        const std::filesystem::path& base_path,
        OOCPrivateHandoffConsumedPublicationBaseLockV1&& authority,
        OOCPrivateHandoffAdoptionTestHooks hooks) noexcept;
};

/// Feed one consumed canonical publication authority into the common adoption
/// path. This bridge can move only the exact retained BaseLock alias.
[[nodiscard]] OOCPrivateHandoffAdoptionResult
adopt_private_handoff_with_consumed_publication_base_lock_v1(
    const std::filesystem::path& base_path,
    OOCPrivateHandoffConsumedPublicationBaseLockV1&& authority,
    OOCPrivateHandoffAdoptionTestHooks hooks = {}) noexcept;

} // namespace gnfs::relation::ooc_cleanup_detail
