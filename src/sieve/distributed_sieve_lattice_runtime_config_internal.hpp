#pragma once

// Source-private mapping from a validated frozen distributed-sieve policy to
// the low-level lattice runtime configuration. This file is not public API.

#include "distributed_sieve_execution_policy_internal.hpp"

#include <gnfs/sieve/lattice_sieve.hpp>

#include <cstdint>
#include <optional>

namespace gnfs::sieve::distributed_sieve_execution_policy_detail {

struct DistributedSieveLatticeRuntimeConfigV1 final {
    LatticeSieveExecutionConfig sieve;
    std::uint32_t lattice_basis_parallel_threads = 1;
};

struct DistributedSieveLatticeRuntimeConfigMapResultV1 final {
    std::optional<DistributedSieveLatticeRuntimeConfigV1> config;
    DistributedSieveProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return config.has_value() && static_cast<bool>(status);
    }
};

[[nodiscard]] DistributedSieveLatticeRuntimeConfigMapResultV1
map_distributed_sieve_lattice_runtime_config_v1(
    const DistributedSieveFrozenExecutionPolicyV1& policy) noexcept;

} // namespace gnfs::sieve::distributed_sieve_execution_policy_detail
