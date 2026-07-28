#pragma once

// Source-private binding from one validated distributed-sieve identity and
// its exact frozen execution policy to the explicit seeded cofactor runtime.

#include "distributed_sieve_execution_policy_internal.hpp"

#include <gnfs/cofactor/cofactorizer.hpp>
#include <gnfs/sieve/distributed_sieve_seed_v2.hpp>

#include <optional>

namespace gnfs::sieve::distributed_sieve_execution_policy_detail {

struct DistributedSieveCofactorRuntimeV2 final {
    cofactor::CofactorizerConfig cofactorizer;
    DistributedSieveCofactorSeedProviderV2 seed_provider;
};

struct DistributedSieveCofactorRuntimeMapResultV2 final {
    std::optional<DistributedSieveCofactorRuntimeV2> runtime;
    DistributedSieveProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return runtime.has_value() && static_cast<bool>(status);
    }
};

/// Map only the explicit seeded-cofactor seams available in CofactorizerConfig.
///
/// The identity must be valid, the frozen policy must be internally valid, and
/// its canonical projection must exactly match identity.execution_policy.
/// Identity-level zero sentinels are resolved exactly as the V2 semantic root
/// resolves them. The returned provider is rooted in that same projection.
[[nodiscard]] DistributedSieveCofactorRuntimeMapResultV2 map_distributed_sieve_cofactor_runtime_v2(
    const DistributedSieveWorkIdentityV1& identity,
    const DistributedSieveFrozenExecutionPolicyV1& frozen_policy) noexcept;

} // namespace gnfs::sieve::distributed_sieve_execution_policy_detail
