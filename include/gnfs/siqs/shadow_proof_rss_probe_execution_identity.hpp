#pragma once

/// @file shadow_proof_rss_probe_execution_identity.hpp
/// @brief Cryptographic identity for one SIQS RSS probe execution contract.

#include <gnfs/util/sha256.hpp>

#include <cstddef>
#include <cstdint>

namespace gnfs::siqs {

inline constexpr std::uint32_t SIQS_SHADOW_PROOF_RSS_PROBE_EXECUTION_CONTRACT_SCHEMA_VERSION = 1;

/// Stable content identity for the executable bytes and the approved,
/// versioned launch contract represented here. Filesystem paths are deployment
/// locators and are intentionally not part of this identity.
struct SIQSShadowProofRssProbeExecutionIdentity final {
    util::Sha256Digest executable_sha256;
    util::Sha256Digest execution_contract_sha256;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssProbeExecutionIdentity&,
               const SIQSShadowProofRssProbeExecutionIdentity&) noexcept = default;
};

[[nodiscard]] constexpr bool siqs_shadow_proof_rss_probe_execution_identity_is_valid(
    const SIQSShadowProofRssProbeExecutionIdentity& identity) noexcept {
    bool executable_nonzero = false;
    for (const std::byte value : identity.executable_sha256.bytes) {
        executable_nonzero = executable_nonzero || value != std::byte{0};
    }

    bool contract_nonzero = false;
    for (const std::byte value : identity.execution_contract_sha256.bytes) {
        contract_nonzero = contract_nonzero || value != std::byte{0};
    }
    return executable_nonzero && contract_nonzero;
}

} // namespace gnfs::siqs
