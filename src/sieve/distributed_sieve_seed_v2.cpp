#include <gnfs/sieve/distributed_sieve_seed_v2.hpp>

#include "distributed_sieve_execution_policy_internal.hpp"

#include <gnfs/util/sha256.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace gnfs::sieve {
namespace {

using distributed_sieve_execution_policy_detail::distributed_sieve_execution_policy_descriptors_v1;
using distributed_sieve_execution_policy_detail::DistributedSieveExecutionPolicyClassificationV1;

constexpr std::string_view SEMANTIC_SEED_ROOT_DOMAIN =
    "GNFS-DISTRIBUTED-SIEVE-SEMANTIC-SEED-ROOT-V2";
constexpr std::uint32_t SEMANTIC_EXECUTION_SETTING_COUNT_V2 = 13;

[[nodiscard]] constexpr DistributedSieveProtocolStatus
failure(DistributedSieveProtocolError error) noexcept {
    return {error, DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX};
}

class SemanticSeedWriter final {
public:
    explicit SemanticSeedWriter(std::string_view domain) noexcept {
        constexpr std::array<std::byte, 1> separator{std::byte{0}};
        if (!accumulator_.update(domain) || !accumulator_.update(separator)) {
            failed_ = true;
        }
    }

    void put_u8(std::uint8_t value) noexcept {
        const std::array<std::byte, 1> bytes{static_cast<std::byte>(value)};
        put_bytes(bytes);
    }

    void put_u16(std::uint16_t value) noexcept {
        for (unsigned shift = 0; shift < 16; shift += 8) {
            put_u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_u32(std::uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            put_u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_u64(std::uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            put_u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_i64(std::int64_t value) noexcept {
        put_u64(std::bit_cast<std::uint64_t>(value));
    }

    void put_bool(bool value) noexcept {
        put_u8(value ? 1U : 0U);
    }

    void put_bytes(std::span<const std::byte> bytes) noexcept {
        if (!failed_ && !accumulator_.update(bytes)) {
            failed_ = true;
        }
    }

    void put_string(std::string_view value) noexcept {
        put_u32(static_cast<std::uint32_t>(value.size()));
        put_bytes(std::as_bytes(std::span(value.data(), value.size())));
    }

    [[nodiscard]] std::optional<util::Sha256Digest> finish() noexcept {
        if (failed_) {
            return std::nullopt;
        }
        return accumulator_.finalize();
    }

private:
    util::Sha256Accumulator accumulator_;
    bool failed_ = false;
};

void hash_effective_special_q_bounds(SemanticSeedWriter& writer,
                                     const SpecialQBoundsV1& bounds) noexcept {
    writer.put_u32(bounds.start_index);
    writer.put_u32(bounds.end_index);
    writer.put_u64(bounds.min_q);
    writer.put_u64(bounds.max_q);
}

void hash_polynomial(SemanticSeedWriter& writer,
                     const PolynomialWorkIdentityV1& polynomial) noexcept {
    writer.put_u8(0x01);
    writer.put_string(polynomial.n.decimal);
    writer.put_string(polynomial.m.decimal);
    writer.put_u32(polynomial.degree);

    const std::uint32_t live_coefficient_count = polynomial.degree + 1U;
    writer.put_u32(live_coefficient_count);
    for (std::uint32_t index = 0; index < live_coefficient_count; ++index) {
        writer.put_u32(index);
        writer.put_string(polynomial.coefficients[index].decimal);
    }
    writer.put_u64(polynomial.skewness_ieee754_bits);
}

void hash_factor_base(SemanticSeedWriter& writer,
                      const FactorBaseWorkIdentityV1& factor_base) noexcept {
    writer.put_u8(0x02);
    writer.put_u64(factor_base.rational_bound);
    writer.put_u64(factor_base.algebraic_bound);
    writer.put_u64(factor_base.large_prime_bound);
    writer.put_u32(factor_base.log_scale);

    writer.put_u32(static_cast<std::uint32_t>(factor_base.rational.size()));
    for (std::uint32_t index = 0; index < factor_base.rational.size(); ++index) {
        const auto& entry = factor_base.rational[index];
        writer.put_u32(index);
        writer.put_u64(entry.p);
        writer.put_u32(entry.log_p);
    }

    writer.put_u32(static_cast<std::uint32_t>(factor_base.algebraic.size()));
    for (std::uint32_t index = 0; index < factor_base.algebraic.size(); ++index) {
        const auto& entry = factor_base.algebraic[index];
        writer.put_u32(index);
        writer.put_u64(entry.p);
        writer.put_u64(entry.r);
        writer.put_u32(entry.log_p);
        writer.put_u32(entry.degree);
    }
    writer.put_u64(factor_base.sieve_algebraic_count);
}

void hash_sieve(SemanticSeedWriter& writer, const SieveParametersWorkIdentityV1& sieve,
                std::uint64_t factor_base_large_prime_bound) noexcept {
    const std::uint64_t effective_large_prime_bound =
        sieve.large_prime_bound == 0 ? factor_base_large_prime_bound : sieve.large_prime_bound;

    writer.put_u8(0x03);
    writer.put_u32(sieve.log_scale);
    writer.put_u16(sieve.rational_threshold);
    writer.put_u16(sieve.algebraic_threshold);
    writer.put_u64(effective_large_prime_bound);
    writer.put_bool(sieve.allow_2lp);
    writer.put_bool(sieve.allow_3lp);
}

void hash_region(SemanticSeedWriter& writer, const SieveRegionWorkIdentityV1& region) noexcept {
    writer.put_u8(0x04);
    writer.put_i64(region.i_min);
    writer.put_i64(region.i_max);
    writer.put_i64(region.j_min);
    writer.put_i64(region.j_max);
}

void hash_cofactor(SemanticSeedWriter& writer, const CofactorWorkIdentityV1& cofactor,
                   std::uint64_t factor_base_large_prime_bound) noexcept {
    const std::uint64_t effective_large_prime_bound =
        effective_distributed_sieve_cofactor_large_prime_bound_v2(cofactor.large_prime_bound,
                                                                  factor_base_large_prime_bound);
    const std::uint64_t effective_max_factorization_attempts =
        effective_distributed_sieve_max_factorization_attempts_v2(
            cofactor.max_factorization_attempts);

    writer.put_u8(0x05);
    writer.put_u64(effective_large_prime_bound);
    writer.put_bool(cofactor.allow_1lp);
    writer.put_bool(cofactor.allow_2lp);
    writer.put_bool(cofactor.allow_3lp);
    writer.put_u64(effective_max_factorization_attempts);
}

[[nodiscard]] bool
hash_semantic_execution_policy(SemanticSeedWriter& writer,
                               const DistributedSieveExecutionPolicyV1& policy) noexcept {
    const auto descriptors = distributed_sieve_execution_policy_descriptors_v1();
    std::uint32_t semantic_count = 0;
    for (const auto& descriptor : descriptors) {
        if (descriptor.key.has_value() &&
            descriptor.classification ==
                DistributedSieveExecutionPolicyClassificationV1::semantic) {
            ++semantic_count;
        }
    }
    if (semantic_count != SEMANTIC_EXECUTION_SETTING_COUNT_V2) {
        return false;
    }

    writer.put_u8(0x07);
    writer.put_u32(policy.schema_version);
    writer.put_u32(semantic_count);

    std::uint32_t semantic_ordinal = 0;
    for (const auto& descriptor : descriptors) {
        if (!descriptor.key.has_value() ||
            descriptor.classification !=
                DistributedSieveExecutionPolicyClassificationV1::semantic) {
            continue;
        }

        const std::uint16_t raw_key = static_cast<std::uint16_t>(*descriptor.key);
        if (raw_key == 0 || raw_key > policy.settings.size()) {
            return false;
        }
        const auto& setting = policy.settings[raw_key - 1U];
        if (setting.key != *descriptor.key || setting.kind != descriptor.kind) {
            return false;
        }

        std::uint64_t canonical_bits = setting.canonical_bits;
        if (setting.kind == ExecutionPolicyScalarKindV1::ieee754_binary64 &&
            std::bit_cast<double>(canonical_bits) == 0.0) {
            canonical_bits = std::bit_cast<std::uint64_t>(0.0);
        }

        writer.put_u32(semantic_ordinal++);
        writer.put_u16(raw_key);
        writer.put_u8(static_cast<std::uint8_t>(setting.kind));
        writer.put_u64(canonical_bits);
    }
    return semantic_ordinal == semantic_count;
}

[[nodiscard]] bool semantic_execution_policy_is_runtime_projectable_v2(
    const DistributedSieveExecutionPolicyV1& policy) noexcept {
    const auto setting = [&policy](ExecutionPolicyKeyV1 key) -> const ExecutionPolicySettingV1* {
        const std::uint16_t raw_key = static_cast<std::uint16_t>(key);
        if (raw_key == 0 || raw_key > policy.settings.size()) {
            return nullptr;
        }
        const auto& value = policy.settings[raw_key - 1U];
        return value.key == key ? &value : nullptr;
    };

    const auto* enabled = setting(ExecutionPolicyKeyV1::ecm_brent_suyama);
    const auto* degree = setting(ExecutionPolicyKeyV1::ecm_bs_degree);
    if (enabled == nullptr || degree == nullptr) {
        return false;
    }
    return (enabled->canonical_bits == 1U) == (degree->canonical_bits != 0);
}

void hash_semantic_contract_versions(SemanticSeedWriter& writer) noexcept {
    const auto& versions = DISTRIBUTED_SIEVE_SEMANTIC_CONTRACT_VERSIONS_V2;
    writer.put_u8(0x08);
    writer.put_u32(versions.special_q_enumeration);
    writer.put_u32(versions.lattice_candidate_generation);
    writer.put_u32(versions.candidate_collection_order);
    writer.put_u32(versions.cofactor_classification);
    writer.put_u32(versions.cofactor_input_digest);
}

void validate_cofactor_side(cofactor::CofactorSide side) {
    switch (side) {
    case cofactor::CofactorSide::rational:
    case cofactor::CofactorSide::algebraic:
        return;
    }
    throw std::invalid_argument("unknown distributed cofactor side");
}

[[nodiscard]] DeterministicRandomDomainV1
map_cofactor_random_domain(cofactor::CofactorRandomDomainV1 domain) {
    switch (domain) {
    case cofactor::CofactorRandomDomainV1::brent_pollard_rho:
        return DeterministicRandomDomainV1::pollard_rho;
    case cofactor::CofactorRandomDomainV1::ecm_curve_schedule:
        return DeterministicRandomDomainV1::ecm_curve;
    }
    throw std::invalid_argument("unknown distributed cofactor random domain");
}

} // namespace

DistributedSieveProtocolDigestResult
distributed_sieve_semantic_seed_root_v2(const DistributedSieveWorkIdentityV1& identity) noexcept {
    if (const auto status = validate_distributed_sieve_work_identity(identity); !status) {
        return {std::nullopt, status};
    }
    if (!semantic_execution_policy_is_runtime_projectable_v2(identity.execution_policy)) {
        return {std::nullopt, failure(DistributedSieveProtocolError::invalid_value)};
    }

    SemanticSeedWriter writer(SEMANTIC_SEED_ROOT_DOMAIN);
    writer.put_u32(DISTRIBUTED_SIEVE_SEMANTIC_SEED_ROOT_SCHEMA_VERSION_V2);
    hash_polynomial(writer, identity.polynomial);
    hash_factor_base(writer, identity.factor_base);
    hash_sieve(writer, identity.sieve, identity.factor_base.large_prime_bound);
    hash_region(writer, identity.region);
    hash_cofactor(writer, identity.cofactor, identity.factor_base.large_prime_bound);

    writer.put_u8(0x06);
    hash_effective_special_q_bounds(writer, identity.effective_sq_bounds);

    if (!hash_semantic_execution_policy(writer, identity.execution_policy)) {
        return {std::nullopt, failure(DistributedSieveProtocolError::digest_unavailable)};
    }
    hash_semantic_contract_versions(writer);

    auto digest = writer.finish();
    if (!digest.has_value()) {
        return {std::nullopt, failure(DistributedSieveProtocolError::digest_unavailable)};
    }
    return {digest, {}};
}

DistributedSieveCofactorSeedProviderV2::DistributedSieveCofactorSeedProviderV2(
    util::Sha256Digest semantic_seed_root) noexcept
    : semantic_seed_root_(semantic_seed_root) {}

cofactor::CofactorSeed256 DistributedSieveCofactorSeedProviderV2::seed_for(
    const cofactor::CofactorSeedRequestV1& request) const {
    validate_cofactor_side(request.side);
    const DeterministicRandomDomainV1 domain = map_cofactor_random_domain(request.domain);
    if (request.algorithm_identity == 0) {
        throw std::invalid_argument("distributed cofactor algorithm identity must be nonzero");
    }
    if (request.coordinates.special_q_index > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("distributed cofactor special-Q index exceeds uint32_t");
    }

    const DeterministicRandomSeedRequestV1 distributed_request{
        .work_digest = semantic_seed_root_,
        .domain = domain,
        .chunk_id = 0,
        .sq_index = static_cast<std::uint32_t>(request.coordinates.special_q_index),
        .candidate_ordinal = request.coordinates.candidate_ordinal,
        .algorithm_identity = request.algorithm_identity,
        .cofactor_input_digest = request.cofactor_digest,
    };
    const auto result = derive_distributed_sieve_deterministic_seed(distributed_request);
    if (!result) {
        throw std::logic_error("validated distributed cofactor seed derivation failed");
    }
    return cofactor::CofactorSeed256{.digest = *result.digest};
}

const util::Sha256Digest&
DistributedSieveCofactorSeedProviderV2::semantic_seed_root() const noexcept {
    return semantic_seed_root_;
}

} // namespace gnfs::sieve
