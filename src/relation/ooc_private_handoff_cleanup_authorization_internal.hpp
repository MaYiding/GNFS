#pragma once

// Source-private capability boundary for converting one adopted generic
// handoff into externally authorized cleanup. This file is not installed as
// public API and deliberately provides no production mint function.

#include <gnfs/relation/ooc_authorized_cleanup_intent.hpp>
#include <gnfs/relation/ooc_cleanup_transaction.hpp>
#include <gnfs/util/process.hpp>
#include <gnfs/util/sha256.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>

namespace gnfs::sieve::distributed_sieve_resume_detail {

class DistributedSieveExternalCleanupAuthorizationState;
class DistributedSieveWaveStore;
[[nodiscard]] bool distributed_sieve_external_cleanup_authorization_state_owned_by_current_process(
    const DistributedSieveExternalCleanupAuthorizationState& state) noexcept;

} // namespace gnfs::sieve::distributed_sieve_resume_detail

namespace gnfs::relation::ooc_cleanup_detail {

class OOCPrivateHandoffCleanupAuthorizationTestAuthorityV2;
class OOCPrivateHandoffCleanupIntentConversionExecutorV2;
class OOCPrivateHandoffCleanupIntentPublicationTestKeyV2;
struct OOCPrivateHandoffCleanupIntentPublicationResultV2;
struct OOCPrivateHandoffCleanupIntentPublicationTestHooksV2;

/// Data-only trusted-test liveness anchor. It carries no mint or cleanup
/// authority; the receipt constructor remains private to the exact test
/// authority friend and always installs the fixed validator below.
struct OOCPrivateHandoffCleanupAuthorizationTestLivenessV2 final {
    std::uint64_t creator_process_id = 0;
    std::shared_ptr<std::atomic_bool> live;
};

/// Authority-free projection of the exact application and relation bindings
/// that a future wave-store mint must prove. Constructing or copying this value
/// does not grant cleanup, publication, adoption, or namespace authority.
struct OOCPrivateHandoffCleanupAuthorizationBinding final {
    std::filesystem::path base_path;
    util::Sha256Digest external_authorization_digest;
    util::Sha256Digest generic_handoff_self_digest;
    std::array<std::uint64_t, 2> lease_id{};
    util::durable_immutable_record::NativeIdentity parent_directory_identity;
    util::durable_immutable_record::NativeIdentity lock_identity;
    util::durable_immutable_record::NativeIdentity directory_identity;
    util::durable_immutable_record::NativeIdentity owner_marker_identity;
    util::durable_immutable_record::NativeIdentity owned_marker_identity;
    OOCPrivateHandoffPairDescriptorV1 pair;
    OOCPrivateHandoffArtifactBindingV1 handoff;
    OOCPrivateHandoffArtifactBindingV1 index;
    OOCPrivateHandoffArtifactBindingV1 data;

    [[nodiscard]] friend bool
    operator==(const OOCPrivateHandoffCleanupAuthorizationBinding&,
               const OOCPrivateHandoffCleanupAuthorizationBinding&) = default;
};

/// Unforgeable constructor token. Only the source-private wave store may create
/// one after it holds the wave lock and confirms the canonical external
/// authorization record.
class OOCPrivateHandoffCleanupAuthorizationMintKey final {
public:
    OOCPrivateHandoffCleanupAuthorizationMintKey(
        const OOCPrivateHandoffCleanupAuthorizationMintKey&) = delete;
    OOCPrivateHandoffCleanupAuthorizationMintKey&
    operator=(const OOCPrivateHandoffCleanupAuthorizationMintKey&) = delete;
    OOCPrivateHandoffCleanupAuthorizationMintKey(OOCPrivateHandoffCleanupAuthorizationMintKey&&) =
        delete;
    OOCPrivateHandoffCleanupAuthorizationMintKey&
    operator=(OOCPrivateHandoffCleanupAuthorizationMintKey&&) = delete;
    ~OOCPrivateHandoffCleanupAuthorizationMintKey() = default;

private:
    OOCPrivateHandoffCleanupAuthorizationMintKey() noexcept = default;

    friend class gnfs::sieve::distributed_sieve_resume_detail::DistributedSieveWaveStore;
};

/// Current-process application authority for one exact generic handoff.
///
/// The opaque lifetime must retain the wave lock and canonical external-record
/// binding. The value remains unusable without a separately acquired matching
/// adoption receipt. Neither this type nor its pure binding exposes a public
/// factory, path accessor, native handle, or namespace operation.
class OOCPrivateHandoffCleanupAuthorizationReceipt final {
public:
    OOCPrivateHandoffCleanupAuthorizationReceipt() = delete;
    OOCPrivateHandoffCleanupAuthorizationReceipt(
        const OOCPrivateHandoffCleanupAuthorizationReceipt&) = delete;
    OOCPrivateHandoffCleanupAuthorizationReceipt&
    operator=(const OOCPrivateHandoffCleanupAuthorizationReceipt&) = delete;

    OOCPrivateHandoffCleanupAuthorizationReceipt(
        OOCPrivateHandoffCleanupAuthorizationReceipt&& other) noexcept
        : binding_(std::move(other.binding_)), live_authority_(std::move(other.live_authority_)),
          validate_live_authority_(std::exchange(other.validate_live_authority_, nullptr)),
          spent_(std::exchange(other.spent_, true)) {}

    OOCPrivateHandoffCleanupAuthorizationReceipt&
    operator=(OOCPrivateHandoffCleanupAuthorizationReceipt&&) = delete;
    ~OOCPrivateHandoffCleanupAuthorizationReceipt() = default;

    [[nodiscard]] bool spent() const noexcept {
        return spent_ || !live_authority_ || validate_live_authority_ == nullptr ||
               !validate_live_authority_(live_authority_.get());
    }

private:
    using ValidateLiveAuthority = bool (*)(const void* authority) noexcept;

    OOCPrivateHandoffCleanupAuthorizationReceipt(
        OOCPrivateHandoffCleanupAuthorizationMintKey&&,
        OOCPrivateHandoffCleanupAuthorizationBinding binding,
        std::shared_ptr<const gnfs::sieve::distributed_sieve_resume_detail::
                            DistributedSieveExternalCleanupAuthorizationState>
            live_wave_authority) noexcept
        : binding_(std::move(binding)), live_authority_(std::move(live_wave_authority)),
          validate_live_authority_([](const void* authority) noexcept {
              if (authority == nullptr) {
                  return false;
              }
              return gnfs::sieve::distributed_sieve_resume_detail::
                  distributed_sieve_external_cleanup_authorization_state_owned_by_current_process(
                      *static_cast<const gnfs::sieve::distributed_sieve_resume_detail::
                                       DistributedSieveExternalCleanupAuthorizationState*>(
                          authority));
          }) {}

    OOCPrivateHandoffCleanupAuthorizationReceipt(
        OOCPrivateHandoffCleanupAuthorizationBinding binding,
        std::shared_ptr<const OOCPrivateHandoffCleanupAuthorizationTestLivenessV2>
            live_authority) noexcept
        : binding_(std::move(binding)), live_authority_(std::move(live_authority)),
          validate_live_authority_([](const void* authority) noexcept {
              const auto* state =
                  static_cast<const OOCPrivateHandoffCleanupAuthorizationTestLivenessV2*>(
                      authority);
              return state != nullptr && state->creator_process_id != 0 && state->live &&
                     state->creator_process_id ==
                         static_cast<std::uint64_t>(gnfs::util::process_id()) &&
                     state->live->load(std::memory_order_acquire);
          }) {}

    void commit_spend() noexcept {
        spent_ = true;
    }

    OOCPrivateHandoffCleanupAuthorizationBinding binding_;
    std::shared_ptr<const void> live_authority_;
    ValidateLiveAuthority validate_live_authority_ = nullptr;
    bool spent_ = false;

    friend class gnfs::sieve::distributed_sieve_resume_detail::DistributedSieveWaveStore;
    friend class OOCPrivateHandoffCleanupAuthorizationTestAuthorityV2;
    friend class OOCPrivateHandoffCleanupIntentConversionExecutorV2;
};

/// Unforgeable token preventing production/internal callers from arming the
/// partial-publication fault seam.
class OOCPrivateHandoffCleanupIntentPublicationTestKeyV2 final {
public:
    OOCPrivateHandoffCleanupIntentPublicationTestKeyV2(
        const OOCPrivateHandoffCleanupIntentPublicationTestKeyV2&) = delete;
    OOCPrivateHandoffCleanupIntentPublicationTestKeyV2&
    operator=(const OOCPrivateHandoffCleanupIntentPublicationTestKeyV2&) = delete;
    OOCPrivateHandoffCleanupIntentPublicationTestKeyV2(
        OOCPrivateHandoffCleanupIntentPublicationTestKeyV2&&) = delete;
    OOCPrivateHandoffCleanupIntentPublicationTestKeyV2&
    operator=(OOCPrivateHandoffCleanupIntentPublicationTestKeyV2&&) = delete;

private:
    OOCPrivateHandoffCleanupIntentPublicationTestKeyV2() noexcept = default;

    friend class OOCPrivateHandoffCleanupAuthorizationTestAuthorityV2;
};

enum class OOCPrivateHandoffCleanupIntentPublicationFaultPointV2 : std::uint8_t {
    ReaderViewsClosed,
    BindingRevalidated,
    IntentPendingDurable,
    IntentCanonicalPromoted,
    IntentCanonicalDurable,
    CanonicalBindingRevalidated,
    Count,
};

struct OOCPrivateHandoffCleanupIntentPublicationTestHooksV2 final {
    using StopAfter = bool (*)(OOCPrivateHandoffCleanupIntentPublicationFaultPointV2 point,
                               void* context) noexcept;

    StopAfter stop_after = nullptr;
    void* context = nullptr;
};

enum class OOCPrivateHandoffCleanupIntentPublicationDispositionV2 : std::uint8_t {
    Failed,
    CapabilitiesRetained,
    CanonicalReconciliationRequired,
    IntentPublished,
};

/// Durable evidence for the T2a authority-conversion boundary. This proves
/// only canonical intent publication. It deliberately does not claim artifact,
/// handoff, lease, or namespace cleanup completion.
struct OOCPrivateHandoffCleanupIntentPublicationEvidenceV2 final {
    util::Sha256Digest external_authorization_digest;
    util::durable_immutable_record::RecordSnapshot intent_snapshot;
    util::durable_immutable_record::NativeIdentity parent_directory_identity;
};

struct OOCPrivateHandoffCleanupIntentPublicationResultV2 final {
    OOCCleanupResult result;
    OOCPrivateHandoffCleanupIntentPublicationDispositionV2 disposition =
        OOCPrivateHandoffCleanupIntentPublicationDispositionV2::Failed;
    std::optional<OOCPrivateHandoffCleanupIntentPublicationEvidenceV2> evidence;

    [[nodiscard]] bool intent_published() const noexcept {
        return disposition ==
                   OOCPrivateHandoffCleanupIntentPublicationDispositionV2::IntentPublished &&
               evidence.has_value();
    }

    [[nodiscard]] bool capabilities_retained() const noexcept {
        return disposition ==
               OOCPrivateHandoffCleanupIntentPublicationDispositionV2::CapabilitiesRetained;
    }

    [[nodiscard]] bool canonical_reconciliation_required() const noexcept {
        return disposition == OOCPrivateHandoffCleanupIntentPublicationDispositionV2::
                                  CanonicalReconciliationRequired;
    }

    [[nodiscard]] bool capabilities_spent() const noexcept {
        return intent_published() || canonical_reconciliation_required();
    }
};

/// Convert the exact same-handle reader and one matching application receipt
/// into canonical V2 cleanup intent. Once canonical promotion may be visible,
/// both live capabilities are spent and any uncertain durability prefix is
/// reconciliation-only. This T2a seam performs no artifact or lease deletion.
[[nodiscard]] OOCPrivateHandoffCleanupIntentPublicationResultV2
convert_authorized_private_handoff_to_cleanup_intent_v2(
    OOCPrivateHandoffReader&& reader,
    OOCPrivateHandoffCleanupAuthorizationReceipt&& authorization) noexcept;

/// Same conversion with deterministic partial-publication stops. Only the
/// exact trusted-test friend can construct the first argument.
[[nodiscard]] OOCPrivateHandoffCleanupIntentPublicationResultV2
convert_authorized_private_handoff_to_cleanup_intent_v2_for_trusted_test(
    OOCPrivateHandoffCleanupIntentPublicationTestKeyV2&&, OOCPrivateHandoffReader&& reader,
    OOCPrivateHandoffCleanupAuthorizationReceipt&& authorization,
    OOCPrivateHandoffCleanupIntentPublicationTestHooksV2 hooks) noexcept;

} // namespace gnfs::relation::ooc_cleanup_detail
