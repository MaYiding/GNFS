#include "distributed_sieve_cofactor_runtime_config_internal.hpp"

#include <cstddef>

namespace gnfs::sieve::distributed_sieve_execution_policy_detail {
namespace {

[[nodiscard]] constexpr DistributedSieveProtocolStatus
runtime_config_failure(DistributedSieveProtocolError error) noexcept {
    return {error, DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX};
}

[[nodiscard]] bool same_canonical_policy(const DistributedSieveExecutionPolicyV1& left,
                                         const DistributedSieveExecutionPolicyV1& right) noexcept {
    if (left.schema_version != right.schema_version ||
        left.settings.size() != right.settings.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.settings.size(); ++index) {
        const auto& left_setting = left.settings[index];
        const auto& right_setting = right.settings[index];
        if (left_setting.key != right_setting.key || left_setting.kind != right_setting.kind ||
            left_setting.canonical_bits != right_setting.canonical_bits) {
            return false;
        }
    }
    return true;
}

} // namespace

DistributedSieveCofactorRuntimeMapResultV2 map_distributed_sieve_cofactor_runtime_v2(
    const DistributedSieveWorkIdentityV1& identity,
    const DistributedSieveFrozenExecutionPolicyV1& frozen_policy) noexcept {
    if (const auto status = validate_distributed_sieve_work_identity(identity); !status) {
        return {std::nullopt, status};
    }
    if (const auto status = validate_distributed_sieve_frozen_execution_policy_v1(frozen_policy);
        !status) {
        return {std::nullopt, status};
    }
    if (!same_canonical_policy(identity.execution_policy, frozen_policy.canonical)) {
        return {std::nullopt, runtime_config_failure(DistributedSieveProtocolError::invalid_value)};
    }
    if (frozen_policy.cofactor.survival_filter && frozen_policy.cofactor.survival_threshold > 0.0) {
        return {std::nullopt, runtime_config_failure(DistributedSieveProtocolError::invalid_value)};
    }
    if (frozen_policy.cofactor.ecm_sigma_pool_size != 0 ||
        frozen_policy.cofactor.ecm_curve_pool != 0) {
        return {std::nullopt, runtime_config_failure(DistributedSieveProtocolError::invalid_value)};
    }

    const auto semantic_root = distributed_sieve_semantic_seed_root_v2(identity);
    if (!semantic_root) {
        return {std::nullopt, semantic_root.status};
    }

    cofactor::CofactorizerConfig config;
    config.large_prime_bound = effective_distributed_sieve_cofactor_large_prime_bound_v2(
        identity.cofactor.large_prime_bound, identity.factor_base.large_prime_bound);
    config.allow_1lp = identity.cofactor.allow_1lp;
    config.allow_2lp = identity.cofactor.allow_2lp;
    config.allow_3lp = identity.cofactor.allow_3lp;
    config.max_factorization_attempts = effective_distributed_sieve_max_factorization_attempts_v2(
        identity.cofactor.max_factorization_attempts);
    config.seeded_brent_pollard_enabled = frozen_policy.cofactor.cofactor_brent;
    config.seeded_ecm_brent_suyama_degree =
        frozen_policy.cofactor.ecm_brent_suyama ? frozen_policy.cofactor.ecm_bs_degree : 0;

    return {
        DistributedSieveCofactorRuntimeV2{
            .cofactorizer = config,
            .seed_provider = DistributedSieveCofactorSeedProviderV2{*semantic_root.digest},
        },
        {},
    };
}

} // namespace gnfs::sieve::distributed_sieve_execution_policy_detail
