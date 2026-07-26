#pragma once

// Source-private capability boundary for converting one adopted generic
// handoff into externally authorized cleanup. This file is not installed as
// public API and deliberately provides no production mint function.

#include <gnfs/relation/ooc_durable_handoff.hpp>
#include <gnfs/util/sha256.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>

namespace gnfs::sieve::distributed_sieve_resume_detail {

class DistributedSieveExternalCleanupAuthorizationState;
class DistributedSieveWaveStore;
[[nodiscard]] bool distributed_sieve_external_cleanup_authorization_state_owned_by_current_process(
    const DistributedSieveExternalCleanupAuthorizationState& state) noexcept;

} // namespace gnfs::sieve::distributed_sieve_resume_detail

namespace gnfs::relation::ooc_cleanup_detail {

/// Authority-free projection of the exact application and relation bindings
/// that a future wave-store mint must prove. Constructing or copying this value
/// does not grant cleanup, publication, adoption, or namespace authority.
struct OOCPrivateHandoffCleanupAuthorizationBinding final {
    std::filesystem::path base_path;
    util::Sha256Digest external_authorization_digest;
    util::Sha256Digest generic_handoff_self_digest;
    std::array<std::uint64_t, 2> lease_id{};
    util::durable_immutable_record::NativeIdentity directory_identity;
    util::durable_immutable_record::NativeIdentity owner_marker_identity;
    OOCPrivateHandoffPairDescriptorV1 pair;
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
        : binding_(std::move(other.binding_)),
          live_wave_authority_(std::move(other.live_wave_authority_)),
          spent_(std::exchange(other.spent_, true)) {}

    OOCPrivateHandoffCleanupAuthorizationReceipt&
    operator=(OOCPrivateHandoffCleanupAuthorizationReceipt&&) = delete;
    ~OOCPrivateHandoffCleanupAuthorizationReceipt() = default;

    [[nodiscard]] bool spent() const noexcept {
        return spent_ || !live_wave_authority_ ||
               !gnfs::sieve::distributed_sieve_resume_detail::
                   distributed_sieve_external_cleanup_authorization_state_owned_by_current_process(
                       *live_wave_authority_);
    }

private:
    OOCPrivateHandoffCleanupAuthorizationReceipt(
        OOCPrivateHandoffCleanupAuthorizationMintKey&&,
        OOCPrivateHandoffCleanupAuthorizationBinding binding,
        std::shared_ptr<const gnfs::sieve::distributed_sieve_resume_detail::
                            DistributedSieveExternalCleanupAuthorizationState>
            live_wave_authority) noexcept
        : binding_(std::move(binding)), live_wave_authority_(std::move(live_wave_authority)) {}

    void commit_spend() noexcept {
        spent_ = true;
    }

    OOCPrivateHandoffCleanupAuthorizationBinding binding_;
    std::shared_ptr<const gnfs::sieve::distributed_sieve_resume_detail::
                        DistributedSieveExternalCleanupAuthorizationState>
        live_wave_authority_;
    bool spent_ = false;

    friend class gnfs::sieve::distributed_sieve_resume_detail::DistributedSieveWaveStore;
};

} // namespace gnfs::relation::ooc_cleanup_detail
