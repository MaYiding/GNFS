#pragma once

/// @file ooc_private_cleanup_action_permit_internal.hpp
/// @brief Source-private action permit for one retained cleanup-union witness.

#include "ooc_private_cleanup_union_internal.hpp"

#include <gnfs/relation/ooc_cleanup_transaction.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace gnfs::relation::ooc_cleanup_detail {

class PrivateCleanupActionPermit;
struct PrivateCleanupActionAdmission;
struct PrivateLeaseRemovalAdmission;

/// Exact current lease generation proven against one caller receipt before a
/// RemovePrivateLease permit may observe or reconcile C1.
struct PrivateLeaseRemovalGenerationProof final {
    std::array<std::uint64_t, 3> parent_identity{};
    std::array<std::uint64_t, 2> expected_lease_id{};
    std::array<std::uint64_t, 3> expected_directory_identity{};
    std::array<std::uint64_t, 3> expected_owner_identity{};
    std::array<std::uint64_t, 3> expected_owned_identity{};
    std::optional<LoadedPrivateLeaseMarker> owned;
    std::optional<LoadedPrivateLeaseMarker> reserved;
    std::optional<LoadedPrivateLeaseMarker> owned_pending;
    std::optional<LoadedPrivateLeaseMarker> reserved_pending;
    std::optional<std::array<std::uint64_t, 3>> final_directory_identity;
    std::optional<std::array<std::uint64_t, 3>> staging_directory_identity;
    bool owner_present = false;

    friend bool operator==(const PrivateLeaseRemovalGenerationProof&,
                           const PrivateLeaseRemovalGenerationProof&) = default;
};

/// A non-forgeable, move-only capability for one private-namespace action.
///
/// The implementation retains the BaseLock and the observation witnesses. It
/// is intentionally source-private: public records, paths, digests, raw test
/// observations, and ownership receipts cannot construct it.
class PrivateCleanupActionPermit final {
public:
    struct State;

    PrivateCleanupActionPermit() = delete;
    PrivateCleanupActionPermit(const PrivateCleanupActionPermit&) = delete;
    PrivateCleanupActionPermit& operator=(const PrivateCleanupActionPermit&) = delete;
    PrivateCleanupActionPermit(PrivateCleanupActionPermit&& other) noexcept;
    PrivateCleanupActionPermit& operator=(PrivateCleanupActionPermit&&) = delete;
    ~PrivateCleanupActionPermit();

private:
    explicit PrivateCleanupActionPermit(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;

    friend struct PrivateCleanupActionAdmission;
    friend PrivateCleanupActionAdmission
    admit_private_cleanup_action_locked(const OOCCleanupPaths& paths,
                                        std::shared_ptr<BaseLock> lock,
                                        PrivateNamespaceAction action);
    friend PrivateLeaseRemovalAdmission admit_private_lease_removal_locked(
        const OOCCleanupPaths& paths, std::shared_ptr<BaseLock> lock,
        const std::array<std::uint64_t, 2>& expected_lease_id,
        const std::array<std::uint64_t, 3>& expected_directory_identity,
        const std::array<std::uint64_t, 3>& expected_owner_identity,
        const std::array<std::uint64_t, 3>& expected_owned_identity);
    friend const BaseLock& begin_private_cleanup_action(PrivateCleanupActionPermit& permit,
                                                        const OOCCleanupPaths& paths,
                                                        PrivateNamespaceAction expected_action);
    friend void
    bind_private_lease_removal_generation(PrivateCleanupActionPermit& permit,
                                          const PrivateLeaseRemovalGenerationProof& proof);
    friend OOCPrivateHandoffInspectResult
    reconcile_private_handoff_from_permit(PrivateCleanupActionPermit& permit,
                                          PrivateNamespaceAction expected_action);
    friend OOCPrivateHandoffInspectResult
    inspect_private_handoff_from_permit(PrivateCleanupActionPermit& permit);
    friend void authorize_private_cleanup_mutation(PrivateCleanupMutationGate& gate,
                                                   const OOCCleanupPaths& paths,
                                                   const BaseLock& lock);
};

/// Source-private bridge from the public inline executor to the retained
/// RunLegacyCleanup permit. The first namespace mutation consumes this gate;
/// later mutations in the same executor are already covered by that transition.
class PrivateCleanupMutationGate final {
public:
    explicit PrivateCleanupMutationGate(PrivateCleanupActionPermit& permit) noexcept
        : permit_(&permit) {}

    [[nodiscard]] bool authorized() const noexcept {
        return state_ == State::Authorized;
    }

    PrivateCleanupMutationGate() = delete;
    PrivateCleanupMutationGate(const PrivateCleanupMutationGate&) = delete;
    PrivateCleanupMutationGate& operator=(const PrivateCleanupMutationGate&) = delete;
    PrivateCleanupMutationGate(PrivateCleanupMutationGate&&) = delete;
    PrivateCleanupMutationGate& operator=(PrivateCleanupMutationGate&&) = delete;

private:
    enum class State : std::uint8_t {
        Fresh,
        Authorized,
        Failed,
    };

    PrivateCleanupActionPermit* permit_ = nullptr;
    State state_ = State::Fresh;

    friend void authorize_private_cleanup_mutation(PrivateCleanupMutationGate& gate,
                                                   const OOCCleanupPaths& paths,
                                                   const BaseLock& lock);
};

/// Exactly one of `blocked` and `permit` is populated by the production
/// admission factory. A blocked admission never carries action authority.
struct PrivateCleanupActionAdmission final {
    std::optional<OOCCleanupResult> blocked;
    std::optional<PrivateCleanupActionPermit> permit;

    PrivateCleanupActionAdmission() = delete;
    PrivateCleanupActionAdmission(const PrivateCleanupActionAdmission&) = delete;
    PrivateCleanupActionAdmission& operator=(const PrivateCleanupActionAdmission&) = delete;
    PrivateCleanupActionAdmission(PrivateCleanupActionAdmission&&) noexcept = default;
    PrivateCleanupActionAdmission& operator=(PrivateCleanupActionAdmission&&) = delete;

private:
    explicit PrivateCleanupActionAdmission(OOCCleanupResult blocked_result)
        : blocked(std::move(blocked_result)) {}
    explicit PrivateCleanupActionAdmission(PrivateCleanupActionPermit&& granted_permit)
        : permit(std::move(granted_permit)) {}

    friend PrivateCleanupActionAdmission
    admit_private_cleanup_action_locked(const OOCCleanupPaths& paths,
                                        std::shared_ptr<BaseLock> lock,
                                        PrivateNamespaceAction action);
};

/// RemovePrivateLease admission keeps union-blocker precedence ahead of receipt
/// generation validation. An unblocked result carries both one permit and the
/// exact generation proof that must be bound to it.
struct PrivateLeaseRemovalAdmission final {
    std::optional<OOCCleanupResult> blocked;
    std::optional<PrivateCleanupActionPermit> permit;
    std::optional<PrivateLeaseRemovalGenerationProof> generation;

    PrivateLeaseRemovalAdmission() = delete;
    PrivateLeaseRemovalAdmission(const PrivateLeaseRemovalAdmission&) = delete;
    PrivateLeaseRemovalAdmission& operator=(const PrivateLeaseRemovalAdmission&) = delete;
    PrivateLeaseRemovalAdmission(PrivateLeaseRemovalAdmission&&) noexcept = default;
    PrivateLeaseRemovalAdmission& operator=(PrivateLeaseRemovalAdmission&&) = delete;

private:
    explicit PrivateLeaseRemovalAdmission(OOCCleanupResult blocked_result)
        : blocked(std::move(blocked_result)) {}
    PrivateLeaseRemovalAdmission(PrivateCleanupActionPermit&& granted_permit,
                                 PrivateLeaseRemovalGenerationProof generation_proof)
        : permit(std::move(granted_permit)), generation(std::move(generation_proof)) {}

    friend PrivateLeaseRemovalAdmission admit_private_lease_removal_locked(
        const OOCCleanupPaths& paths, std::shared_ptr<BaseLock> lock,
        const std::array<std::uint64_t, 2>& expected_lease_id,
        const std::array<std::uint64_t, 3>& expected_directory_identity,
        const std::array<std::uint64_t, 3>& expected_owner_identity,
        const std::array<std::uint64_t, 3>& expected_owned_identity);
};

/// Production-only mint. The test observer cannot provide or reconstruct the
/// retained state consumed here.
[[nodiscard]] PrivateCleanupActionAdmission
admit_private_cleanup_action_locked(const OOCCleanupPaths& paths, std::shared_ptr<BaseLock> lock,
                                    PrivateNamespaceAction action);

/// Remove-specific production mint. Union blockers are projected before the
/// read-only receipt-generation capture, preserving the existing status matrix.
[[nodiscard]] PrivateLeaseRemovalAdmission
admit_private_lease_removal_locked(const OOCCleanupPaths& paths, std::shared_ptr<BaseLock> lock,
                                   const std::array<std::uint64_t, 2>& expected_lease_id,
                                   const std::array<std::uint64_t, 3>& expected_directory_identity,
                                   const std::array<std::uint64_t, 3>& expected_owner_identity,
                                   const std::array<std::uint64_t, 3>& expected_owned_identity);

/// Commit one action consumption attempt and return its retained BaseLock.
/// Wrong-action, wrong-path, moved-from, fork-child, or repeated consumption is
/// rejected before a namespace probe or mutation.
[[nodiscard]] const BaseLock& begin_private_cleanup_action(PrivateCleanupActionPermit& permit,
                                                           const OOCCleanupPaths& paths,
                                                           PrivateNamespaceAction expected_action);

/// Prove that the currently locked lease generation is either the exact caller
/// generation or a fully absent terminal retry state. This is read-only and
/// runs only after RemovePrivateLease admission projects union blockers and
/// before that admission mints its permit.
[[nodiscard]] PrivateLeaseRemovalGenerationProof capture_private_lease_removal_generation_locked(
    const OOCCleanupPaths& paths, const BaseLock& lock,
    const std::array<std::uint64_t, 2>& expected_lease_id,
    const std::array<std::uint64_t, 3>& expected_directory_identity,
    const std::array<std::uint64_t, 3>& expected_owner_identity,
    const std::array<std::uint64_t, 3>& expected_owned_identity);

/// Bind the already-started RemovePrivateLease permit to the exact proof
/// captured before admission. Repeated, cross-action, or drifted bindings fail
/// closed before C1 mutation.
void bind_private_lease_removal_generation(PrivateCleanupActionPermit& permit,
                                           const PrivateLeaseRemovalGenerationProof& proof);

/// Revalidate and consume the C1 handoff portion of an already-started permit.
/// The expected action must match the mint and begin boundaries. This never
/// performs an independent handoff observation or constructs new authority
/// from current path state.
[[nodiscard]] OOCPrivateHandoffInspectResult
reconcile_private_handoff_from_permit(PrivateCleanupActionPermit& permit,
                                      PrivateNamespaceAction expected_action);

/// Revalidate and consume the C1 portion of an already-started
/// RunLegacyCleanup permit without publishing, converging, or removing either
/// handoff leaf.
[[nodiscard]] OOCPrivateHandoffInspectResult
inspect_private_handoff_from_permit(PrivateCleanupActionPermit& permit);

} // namespace gnfs::relation::ooc_cleanup_detail
