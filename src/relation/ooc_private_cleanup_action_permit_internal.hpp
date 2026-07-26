#pragma once

/// @file ooc_private_cleanup_action_permit_internal.hpp
/// @brief Source-private action permit for one retained cleanup-union witness.

#include "ooc_private_cleanup_union_internal.hpp"

#include <gnfs/relation/ooc_cleanup_transaction.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace gnfs::relation::ooc_cleanup_detail {

class PrivateCleanupActionPermit;
struct PrivateCleanupActionAdmission;

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
    friend const BaseLock& begin_private_cleanup_action(PrivateCleanupActionPermit& permit,
                                                        const OOCCleanupPaths& paths,
                                                        PrivateNamespaceAction expected_action);
    friend OOCPrivateHandoffInspectResult
    reconcile_recovery_handoff_from_permit(PrivateCleanupActionPermit& permit);
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

/// Production-only mint. The test observer cannot provide or reconstruct the
/// retained state consumed here.
[[nodiscard]] PrivateCleanupActionAdmission
admit_private_cleanup_action_locked(const OOCCleanupPaths& paths, std::shared_ptr<BaseLock> lock,
                                    PrivateNamespaceAction action);

/// Commit one action consumption attempt and return its retained BaseLock.
/// Wrong-action, wrong-path, moved-from, fork-child, or repeated consumption is
/// rejected before a namespace probe or mutation.
[[nodiscard]] const BaseLock& begin_private_cleanup_action(PrivateCleanupActionPermit& permit,
                                                           const OOCCleanupPaths& paths,
                                                           PrivateNamespaceAction expected_action);

/// Revalidate and consume the C1 handoff portion of an already-started
/// RecoverPrivateLease permit. This never performs an independent handoff
/// observation or constructs new authority from current path state.
[[nodiscard]] OOCPrivateHandoffInspectResult
reconcile_recovery_handoff_from_permit(PrivateCleanupActionPermit& permit);

} // namespace gnfs::relation::ooc_cleanup_detail
