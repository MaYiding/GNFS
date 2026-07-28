#pragma once

/// @file distributed_sieve_seed_v2.hpp
/// @brief Topology-free semantic seed root projected from a valid V1 identity.

#include <gnfs/sieve/distributed_sieve_protocol.hpp>

#include <cstdint>

namespace gnfs::sieve {

inline constexpr std::uint32_t DISTRIBUTED_SIEVE_SEMANTIC_SEED_ROOT_SCHEMA_VERSION_V2 = 2;
inline constexpr std::uint64_t DISTRIBUTED_SIEVE_SEMANTIC_DEFAULT_MAX_FACTORIZATION_ATTEMPTS_V2 =
    10'000;

/// Frozen semantic contracts that directly shape candidate identity or
/// cofactor input before an algorithm-specific random schedule is selected.
///
/// This canonical object is hashed by distributed_sieve_semantic_seed_root_v2()
/// and is not supplied by callers. Increment exactly the affected field when
/// that contract changes.
struct DistributedSieveSemanticContractVersionsV2 final {
    std::uint32_t special_q_enumeration;
    std::uint32_t lattice_candidate_generation;
    std::uint32_t candidate_collection_order;
    std::uint32_t cofactor_classification;
    std::uint32_t cofactor_input_digest;

    [[nodiscard]] friend constexpr bool
    operator==(const DistributedSieveSemanticContractVersionsV2&,
               const DistributedSieveSemanticContractVersionsV2&) noexcept = default;
};

inline constexpr DistributedSieveSemanticContractVersionsV2
    DISTRIBUTED_SIEVE_SEMANTIC_CONTRACT_VERSIONS_V2{
        .special_q_enumeration = 1,
        .lattice_candidate_generation = 1,
        .candidate_collection_order = 1,
        .cofactor_classification = 1,
        .cofactor_input_digest = 1,
    };

/// Derive the stable root for per-candidate deterministic random requests.
///
/// V2 includes the live polynomial, factor base, sieve parameters, sieve
/// region, cofactor policy, effective special-Q bounds, and only execution
/// policy settings classified as semantic. It normalizes two V1 provenance or
/// storage details that do not change candidate semantics:
///   - polynomial coefficient storage above the live degree;
///   - caller-supplied original special-Q predicates that resolve to the same
///     effective special-Q index interval.
/// It also resolves V1 runtime-default sentinels before hashing:
///   - zero sieve/cofactor large-prime bounds use the factor-base bound;
///   - zero max_factorization_attempts uses the frozen V2 default above.
/// Semantic binary64 settings normalize both IEEE signed-zero encodings to
/// positive zero because their runtime comparisons are equivalent.
/// A later identity-to-runtime adapter must apply these exact effective-value
/// rules rather than independently interpreting the raw V1 fields.
///
/// V2 excludes the distributed policy, WorkSemanticVersionsV1, conservative
/// execution settings, and diagnostics. V1 distributed_sieve_work_digest()
/// continues to bind those fields and the exact V1 storage/provenance shape.
/// Their exclusion here means only that deterministic random streams do not
/// drift with execution topology or conservative tuning; it does not claim
/// bit-for-bit parity for the complete distributed pipeline. Individual random
/// schedule versions remain domain-separated by the later request's
/// algorithm_identity.
///
/// This function is a pure identity projection. It does not prove that runtime
/// construction consumes the same identity, that candidate coverage is
/// complete, or that every production RNG consumer uses the resulting root;
/// those integration closures are separate milestones.
///
/// Invalid V1 identities return their validation status unchanged.
[[nodiscard]] DistributedSieveProtocolDigestResult
distributed_sieve_semantic_seed_root_v2(const DistributedSieveWorkIdentityV1& identity) noexcept;

} // namespace gnfs::sieve
