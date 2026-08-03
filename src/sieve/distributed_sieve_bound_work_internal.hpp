#pragma once

// Source-private, side-effect-free binding of one validated distributed-sieve
// identity to the heavyweight live polynomial/factor-base objects.
//
// This is deliberately not worker launch authority. In particular, the value
// returned here cannot reserve an OOC lease, publish AttemptStartedV1, fork a
// worker, or derive an artifact path. A later WaveStore launcher must perform
// this binding immediately before consuming a fresh attempt-start receipt.

#include "distributed_sieve_cofactor_runtime_config_internal.hpp"
#include "distributed_sieve_lattice_runtime_config_internal.hpp"

#include <gnfs/core/polynomial_context.hpp>
#include <gnfs/factor_base/factor_base.hpp>
#include <gnfs/relation/ooc_relation_format.hpp>
#include <gnfs/sieve/distributed_sieve_protocol.hpp>
#include <gnfs/sieve/special_q.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace gnfs::sieve::distributed_sieve_execution_policy_detail {

/// Runtime-contract versions implemented by the first bound-work adapter.
///
/// Most fields are semantic contract ordinals. `ooc_format_version` is the
/// actual descriptor/wire-format value because protocol handoff and merge
/// validation compare it directly with `OOCDescriptorV1::format_version`.
/// A future behavior change must increment the corresponding identity field
/// and add a new adapter rather than silently accepting it here.
inline constexpr WorkSemanticVersionsV1 DISTRIBUTED_SIEVE_BOUND_WORK_VERSIONS_V1{
    .relation_serialization_version = 1,
    .ooc_format_version =
        static_cast<std::uint32_t>(relation::OOCRelationStoreFormat::FORMAT_VERSION_V3),
    .digest_version = 1,
    .handoff_version = 1,
    .retry_policy_version = 1,
    .chunking_version = 1,
    .completion_version = 1,
    .deduplication_version = 1,
    .merge_policy_version = 1,
};

/// Immutable small-value projection of one work identity.
///
/// `polynomial` and `factor_base` are intentionally absent: callers retain
/// their live objects, and the eventual receipt-gated launcher must rerun the
/// binding immediately before fork. Every other worker input is derived here
/// from the identity instead of being accepted as a redundant caller value.
struct DistributedSieveBoundWorkV1 final {
    util::Sha256Digest work_digest;
    SieveParams sieve_parameters;
    SieveRegion sieve_region;
    DistributedSieveCofactorRuntimeV2 cofactor;
    DistributedSieveLatticeRuntimeConfigV1 lattice;
    SpecialQRange original_sq_range;
    SpecialQRange effective_sq_range;
    std::uint32_t worker_count = 0;
    std::vector<ChunkPlanV1> chunks;
    std::uint64_t sq_cap_per_worker = 0;
    std::uint64_t relation_cap_per_worker = 0;
    std::uint32_t max_worker_attempts = 0;
    std::uint32_t max_merge_build_attempts = 0;
    std::uint32_t max_consumption_attempts = 0;
    WorkSemanticVersionsV1 semantic_versions;
    DistributedSieveFrozenExecutionPolicyV1 frozen_policy;
};

struct DistributedSieveBoundWorkResultV1 final {
    std::optional<DistributedSieveBoundWorkV1> work;
    DistributedSieveProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return work.has_value() && static_cast<bool>(status);
    }
};

/// Bind a complete identity and frozen policy to the exact live objects.
///
/// The function is pure with respect to process and filesystem state: it reads
/// no environment, clock, hardware, process, randomness, or artifact
/// namespace. On success, every small worker input is derived from `identity`.
/// On failure it returns the first protocol/mismatch status and no partial
/// runtime object.
[[nodiscard]] DistributedSieveBoundWorkResultV1
bind_distributed_sieve_work_v1(const DistributedSieveWorkIdentityV1& identity,
                               const DistributedSieveFrozenExecutionPolicyV1& frozen_policy,
                               const core::PolynomialContext& polynomial,
                               const factor_base::FactorBase& factor_base) noexcept;

} // namespace gnfs::sieve::distributed_sieve_execution_policy_detail
