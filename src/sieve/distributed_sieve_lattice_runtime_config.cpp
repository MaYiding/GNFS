#include "distributed_sieve_lattice_runtime_config_internal.hpp"

namespace gnfs::sieve::distributed_sieve_execution_policy_detail {
namespace {

[[nodiscard]] constexpr DistributedSieveProtocolStatus
runtime_config_failure(DistributedSieveProtocolError error) noexcept {
    return {error, DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX};
}

} // namespace

DistributedSieveLatticeRuntimeConfigMapResultV1 map_distributed_sieve_lattice_runtime_config_v1(
    const DistributedSieveFrozenExecutionPolicyV1& policy) noexcept {
    const auto validation = validate_distributed_sieve_frozen_execution_policy_v1(policy);
    if (!validation) {
        return {std::nullopt, validation};
    }

    LatticeReductionMethod base_method;
    switch (policy.sieve.lattice_lll) {
    case DistributedSieveCanonicalLatticeReductionV1::gauss:
        base_method = LatticeReductionMethod::Gauss;
        break;
    case DistributedSieveCanonicalLatticeReductionV1::lll:
        base_method = LatticeReductionMethod::LLL;
        break;
    default:
        return {std::nullopt, runtime_config_failure(DistributedSieveProtocolError::unknown_enum)};
    }

    DistributedSieveLatticeRuntimeConfigV1 config;
    config.sieve.lattice_basis.base_method = base_method;
    config.sieve.lattice_basis.skew_enabled = policy.sieve.lattice_skew;
    config.sieve.adaptive_lattice.enabled = policy.sieve.adaptive_lattice;
    config.sieve.adaptive_lattice.density_threshold = policy.sieve.adaptive_lattice_threshold;
    config.sieve.adaptive_lattice.max_retries =
        static_cast<int>(policy.sieve.adaptive_lattice_max_retries);
    config.sieve.adaptive_lattice.perturb_seed = policy.sieve.adaptive_lattice_seed;
    config.lattice_basis_parallel_threads = policy.sieve.lattice_basis_parallel_threads;
    return {config, {}};
}

} // namespace gnfs::sieve::distributed_sieve_execution_policy_detail
