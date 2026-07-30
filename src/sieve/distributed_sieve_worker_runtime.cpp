#include "distributed_sieve_worker_runtime_internal.hpp"

#include "distributed_sieve_execution_policy_internal.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace gnfs::sieve::distributed_sieve_worker_execution_detail {
namespace {

[[nodiscard]] constexpr DistributedSieveProtocolStatus
runtime_failure(DistributedSieveProtocolError error = DistributedSieveProtocolError::invalid_value,
                std::uint32_t element_index = DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX) noexcept {
    return {error, DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, element_index};
}

[[nodiscard]] core::PolynomialContext
rehydrate_polynomial(const PolynomialWorkIdentityV1& identity) {
    std::vector<core::Integer> coefficients;
    coefficients.reserve(identity.coefficients.size());
    for (const auto& coefficient : identity.coefficients) {
        coefficients.emplace_back(coefficient.decimal);
    }
    return {
        core::Integer(identity.n.decimal),
        std::move(coefficients),
        core::Integer(identity.m.decimal),
        std::bit_cast<double>(identity.skewness_ieee754_bits),
    };
}

[[nodiscard]] factor_base::FactorBase
rehydrate_factor_base(const FactorBaseWorkIdentityV1& identity) {
    core::FactorBaseParams parameters{
        static_cast<std::uint32_t>(identity.rational_bound),
        static_cast<std::uint32_t>(identity.algebraic_bound),
        identity.large_prime_bound,
        static_cast<std::uint8_t>(identity.log_scale),
    };
    factor_base::FactorBase factor_base(parameters);
    factor_base.reserve(identity.rational.size(), identity.algebraic.size());
    for (const auto& entry : identity.rational) {
        factor_base.add_rational(static_cast<std::uint32_t>(entry.p), entry.log_p);
    }
    for (const auto& entry : identity.algebraic) {
        factor_base.add_algebraic(static_cast<std::uint32_t>(entry.p),
                                  static_cast<std::uint32_t>(entry.r), entry.log_p,
                                  static_cast<std::uint8_t>(entry.degree));
    }
    factor_base.set_sieve_algebraic_count(static_cast<std::size_t>(identity.sieve_algebraic_count));
    factor_base.build_index();
    return factor_base;
}

} // namespace

DistributedSieveWorkerRuntimeResultV1 rehydrate_distributed_sieve_worker_runtime_v1(
    const DistributedSieveWorkIdentityV1& identity) noexcept {
    using namespace distributed_sieve_execution_policy_detail;

    if (const auto status = validate_distributed_sieve_work_identity(identity); !status) {
        return {std::nullopt, status};
    }

    try {
        auto frozen = rehydrate_distributed_sieve_execution_policy_v1(identity.execution_policy);
        if (!frozen || !frozen.policy.has_value()) {
            return {std::nullopt, frozen.status};
        }

        auto polynomial = rehydrate_polynomial(identity.polynomial);
        auto factor_base = rehydrate_factor_base(identity.factor_base);
        auto bound =
            bind_distributed_sieve_work_v1(identity, *frozen.policy, polynomial, factor_base);
        if (!bound || !bound.work.has_value()) {
            return {std::nullopt, bound.status};
        }

        return {
            DistributedSieveWorkerRuntimeV1{
                .polynomial = std::move(polynomial),
                .factor_base = std::move(factor_base),
                .bound_work = std::move(*bound.work),
            },
            {},
        };
    } catch (const std::bad_alloc&) {
        return {std::nullopt, runtime_failure(DistributedSieveProtocolError::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, runtime_failure()};
    }
}

} // namespace gnfs::sieve::distributed_sieve_worker_execution_detail
