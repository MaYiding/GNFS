#pragma once

// Source-private reconstruction of the immutable live objects consumed by one
// durable distributed-sieve worker. The boundary is pure with respect to the
// process and filesystem: all values come from the authenticated work identity.

#include "distributed_sieve_bound_work_internal.hpp"

#include <gnfs/core/polynomial_context.hpp>
#include <gnfs/factor_base/factor_base.hpp>
#include <gnfs/sieve/distributed_sieve_protocol.hpp>

#include <optional>

namespace gnfs::sieve::distributed_sieve_worker_execution_detail {

struct DistributedSieveWorkerRuntimeV1 final {
    core::PolynomialContext polynomial;
    factor_base::FactorBase factor_base;
    distributed_sieve_execution_policy_detail::DistributedSieveBoundWorkV1 bound_work;
};

struct DistributedSieveWorkerRuntimeResultV1 final {
    std::optional<DistributedSieveWorkerRuntimeV1> runtime;
    DistributedSieveProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return runtime.has_value() && static_cast<bool>(status);
    }
};

/// Reconstruct and bind the exact polynomial, factor base, and frozen runtime
/// policy encoded by `identity`. No environment, hardware, clock, randomness,
/// process, or artifact namespace is consulted.
[[nodiscard]] DistributedSieveWorkerRuntimeResultV1 rehydrate_distributed_sieve_worker_runtime_v1(
    const DistributedSieveWorkIdentityV1& identity) noexcept;

} // namespace gnfs::sieve::distributed_sieve_worker_execution_detail
