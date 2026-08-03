#include "distributed_sieve_bound_work_internal.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace gnfs::sieve::distributed_sieve_execution_policy_detail {
namespace {

[[nodiscard]] constexpr DistributedSieveProtocolStatus
binding_failure(std::uint32_t element_index = DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX) noexcept {
    return {DistributedSieveProtocolError::invalid_value, DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET,
            element_index};
}

[[nodiscard]] bool
lattice_sieve_region_is_runtime_safe(const SieveRegionWorkIdentityV1& region) noexcept {
    using WideUnsigned = std::uintmax_t;

    // The protocol validator runs before this worker-runtime check. Repeat the
    // endpoint/order facts here so the widened subtractions below remain safe
    // if this helper is ever reused independently.
    if (region.i_min < std::numeric_limits<std::int32_t>::min() ||
        region.i_max > std::numeric_limits<std::int32_t>::max() ||
        region.j_min < std::numeric_limits<std::int32_t>::min() ||
        region.j_max > std::numeric_limits<std::int32_t>::max() || region.i_max < region.i_min ||
        region.j_max < region.j_min) {
        return false;
    }

    const auto width = static_cast<WideUnsigned>(region.i_max - region.i_min) + WideUnsigned{1};
    const auto height = static_cast<WideUnsigned>(region.j_max - region.j_min) + WideUnsigned{1};

    // Row-major CompactSmallPrime stores values in [0, p) as int16_t for
    // every p < width. Width 32768 is therefore the exact inclusive boundary:
    // the largest possible stored value is still INT16_MAX.
    constexpr WideUnsigned maximum_compact_width =
        static_cast<WideUnsigned>(std::numeric_limits<std::int16_t>::max()) + WideUnsigned{1};
    if (width == 0 || width > maximum_compact_width || height == 0 ||
        height > static_cast<WideUnsigned>(std::numeric_limits<std::int32_t>::max())) {
        return false;
    }

    // LatticeSieve uses inclusive int32_t row loops and computes
    // j_min + height - 1. The final increment and that intermediate both need
    // one representable value above j_max.
    if (region.j_max == std::numeric_limits<std::int32_t>::max()) {
        return false;
    }

    // estimate_initial_log() currently forms this midpoint sum in int32_t.
    const std::int64_t j_midpoint_sum = region.j_min + region.j_max;
    if (j_midpoint_sum < std::numeric_limits<std::int32_t>::min() ||
        j_midpoint_sum > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }

    // Width and height are now tightly bounded, so their product fits
    // uintmax_t. Check both the platform index type and the exact container
    // used by LatticeSieve::set_region() before SieveRegion::size() is called.
    const WideUnsigned area = width * height;
    if (area > static_cast<WideUnsigned>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    return static_cast<std::size_t>(area) <= std::vector<std::uint16_t>{}.max_size();
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

[[nodiscard]] constexpr bool same_semantic_versions(const WorkSemanticVersionsV1& left,
                                                    const WorkSemanticVersionsV1& right) noexcept {
    return left.relation_serialization_version == right.relation_serialization_version &&
           left.ooc_format_version == right.ooc_format_version &&
           left.digest_version == right.digest_version &&
           left.handoff_version == right.handoff_version &&
           left.retry_policy_version == right.retry_policy_version &&
           left.chunking_version == right.chunking_version &&
           left.completion_version == right.completion_version &&
           left.deduplication_version == right.deduplication_version &&
           left.merge_policy_version == right.merge_policy_version;
}

[[nodiscard]] bool polynomial_matches(const PolynomialWorkIdentityV1& identity,
                                      const core::PolynomialContext& live,
                                      DistributedSieveProtocolStatus& status) {
    if (identity.n.decimal != live.n().to_string() || identity.m.decimal != live.m().to_string() ||
        identity.degree != live.degree() ||
        identity.coefficients.size() != live.coefficients().size() ||
        identity.skewness_ieee754_bits != std::bit_cast<std::uint64_t>(live.skewness())) {
        status = binding_failure();
        return false;
    }
    for (std::size_t index = 0; index < identity.coefficients.size(); ++index) {
        if (identity.coefficients[index].decimal != live.coefficients()[index].to_string()) {
            status = binding_failure(static_cast<std::uint32_t>(index));
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool factor_base_matches(const FactorBaseWorkIdentityV1& identity,
                                       const factor_base::FactorBase& live,
                                       DistributedSieveProtocolStatus& status) noexcept {
    const auto& params = live.params();
    if (identity.rational_bound != params.rational_bound ||
        identity.algebraic_bound != params.algebraic_bound ||
        identity.large_prime_bound != params.large_prime_bound ||
        identity.log_scale != params.log_scale ||
        identity.sieve_algebraic_count != live.sieve_algebraic_count() ||
        identity.rational.size() != live.rational().size() ||
        identity.algebraic.size() != live.algebraic().size()) {
        status = binding_failure();
        return false;
    }

    const auto rational = live.rational();
    for (std::size_t index = 0; index < identity.rational.size(); ++index) {
        if (identity.rational[index].p != rational[index].p ||
            identity.rational[index].log_p != rational[index].log_p) {
            status = binding_failure(static_cast<std::uint32_t>(index));
            return false;
        }
    }

    const auto algebraic = live.algebraic();
    for (std::size_t index = 0; index < identity.algebraic.size(); ++index) {
        const auto& expected = identity.algebraic[index];
        const auto& actual = algebraic[index];
        if (expected.p != actual.p || expected.r != actual.r || expected.log_p != actual.log_p ||
            expected.degree != actual.degree) {
            status = binding_failure(static_cast<std::uint32_t>(index));
            return false;
        }
    }
    return true;
}

[[nodiscard]] SpecialQRange project_sq_range(const SpecialQBoundsV1& bounds) noexcept {
    return SpecialQRange{
        .min_q = static_cast<std::uint32_t>(bounds.min_q),
        .max_q = static_cast<std::uint32_t>(bounds.max_q),
        .start_index = bounds.start_index,
        .end_index = bounds.end_index,
    };
}

} // namespace

DistributedSieveBoundWorkResultV1
bind_distributed_sieve_work_v1(const DistributedSieveWorkIdentityV1& identity,
                               const DistributedSieveFrozenExecutionPolicyV1& frozen_policy,
                               const core::PolynomialContext& polynomial,
                               const factor_base::FactorBase& factor_base) noexcept {
    if (const auto status = validate_distributed_sieve_work_identity(identity); !status) {
        return {std::nullopt, status};
    }
    if (const auto status = validate_distributed_sieve_frozen_execution_policy_v1(frozen_policy);
        !status) {
        return {std::nullopt, status};
    }
    if (!same_canonical_policy(identity.execution_policy, frozen_policy.canonical)) {
        return {std::nullopt, binding_failure()};
    }
    if (!same_semantic_versions(identity.semantic_versions,
                                DISTRIBUTED_SIEVE_BOUND_WORK_VERSIONS_V1)) {
        return {std::nullopt, binding_failure()};
    }
    if (!lattice_sieve_region_is_runtime_safe(identity.region)) {
        return {std::nullopt, binding_failure()};
    }

    DistributedSieveProtocolStatus live_status;
    try {
        if (!polynomial_matches(identity.polynomial, polynomial, live_status) ||
            !factor_base_matches(identity.factor_base, factor_base, live_status)) {
            return {std::nullopt, live_status};
        }

        auto lattice = map_distributed_sieve_lattice_runtime_config_v1(frozen_policy);
        if (!lattice) {
            return {std::nullopt, lattice.status};
        }
        auto cofactor = map_distributed_sieve_cofactor_runtime_v2(identity, frozen_policy);
        if (!cofactor) {
            return {std::nullopt, cofactor.status};
        }
        auto work_digest = distributed_sieve_work_digest(identity);
        if (!work_digest) {
            return {std::nullopt, work_digest.status};
        }

        SieveParams sieve_parameters{
            .log_scale = static_cast<std::uint8_t>(identity.sieve.log_scale),
            .rational_threshold = identity.sieve.rational_threshold,
            .algebraic_threshold = identity.sieve.algebraic_threshold,
            .large_prime_bound = static_cast<std::uint32_t>(identity.sieve.large_prime_bound),
            .enable_2lp = identity.sieve.allow_2lp,
            .enable_3lp = identity.sieve.allow_3lp,
        };
        SieveRegion sieve_region{
            .i_min = static_cast<std::int32_t>(identity.region.i_min),
            .i_max = static_cast<std::int32_t>(identity.region.i_max),
            .j_min = static_cast<std::int32_t>(identity.region.j_min),
            .j_max = static_cast<std::int32_t>(identity.region.j_max),
        };

        return {
            DistributedSieveBoundWorkV1{
                .work_digest = *work_digest.digest,
                .sieve_parameters = sieve_parameters,
                .sieve_region = sieve_region,
                .cofactor = std::move(*cofactor.runtime),
                .lattice = *lattice.config,
                .original_sq_range = project_sq_range(identity.original_sq_bounds),
                .effective_sq_range = project_sq_range(identity.effective_sq_bounds),
                .worker_count = identity.distributed.worker_count,
                .chunks = identity.distributed.chunks,
                .sq_cap_per_worker = identity.distributed.sq_cap_per_worker,
                .relation_cap_per_worker = identity.distributed.relation_cap_per_worker,
                .max_worker_attempts = identity.distributed.max_worker_attempts,
                .max_merge_build_attempts = identity.distributed.max_merge_build_attempts,
                .max_consumption_attempts = identity.distributed.max_consumption_attempts,
                .semantic_versions = identity.semantic_versions,
                .frozen_policy = frozen_policy,
            },
            {},
        };
    } catch (const std::bad_alloc&) {
        return {std::nullopt,
                {DistributedSieveProtocolError::resource_exhausted,
                 DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX}};
    } catch (...) {
        return {std::nullopt, binding_failure()};
    }
}

} // namespace gnfs::sieve::distributed_sieve_execution_policy_detail
