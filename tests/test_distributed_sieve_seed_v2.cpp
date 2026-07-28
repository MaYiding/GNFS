// test_distributed_sieve_seed_v2.cpp - topology-free semantic seed-root contracts

#include <gnfs/sieve/distributed_sieve_seed_v2.hpp>
#include <gnfs/util/sha256.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace sieve = gnfs::sieve;

using Digest = gnfs::util::Sha256Digest;
using Identity = sieve::DistributedSieveWorkIdentityV1;
using SemanticVersions = sieve::DistributedSieveSemanticContractVersionsV2;
using SeedProvider = sieve::DistributedSieveCofactorSeedProviderV2;
using gnfs::cofactor::CofactorRandomDomainV1;
using gnfs::cofactor::CofactorSeedRequestV1;
using gnfs::cofactor::CofactorSide;

[[noreturn]] void fail(std::string_view message, const char* file, int line) {
    throw std::runtime_error(std::string(message) + " at " + file + ":" + std::to_string(line));
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fail("CHECK failed: " #condition, __FILE__, __LINE__);                                 \
        }                                                                                          \
    } while (false)

void require_named(bool condition, std::string_view name, std::string_view expectation) {
    if (!condition) {
        throw std::runtime_error(std::string(name) + ": " + std::string(expectation));
    }
}

constexpr std::string_view SEMANTIC_ROOT_DOMAIN = "GNFS-DISTRIBUTED-SIEVE-SEMANTIC-SEED-ROOT-V2";
constexpr std::string_view EXPECTED_V2_ROOT_HEX =
    "b04139191154fcc7cfce98956623a0957b356ef84436e422cdf39b8fbdb0e64d";
constexpr std::string_view EXPECTED_V1_WORK_DIGEST_HEX =
    "25026e9a43b5f9dd13dae332b949267a4f880c4857178384306645411fbd309a";

// Frozen with an independent Python struct.pack("<...") writer and hashlib.sha256,
// not with the production V2 writer. This is the complete 659-byte preimage.
constexpr std::string_view EXPECTED_V2_PREIMAGE_HEX =
    "474e46532d44495354524942555445442d53494556452d53454d414e5449432d534545442d524f4f542d5632000200"
    "00"
    "00010d00000031303030303336303030303939050000003130303031020000000300000000000000020000002d3501"
    "00"
    "00000100000033020000000100000031000000000000f43f026400000000000000c800000000000000102700000000"
    "00"
    "0010000000020000000000000002000000000000001000000001000000050000000000000019000000050000000000"
    "00"
    "00070000000000000001000000000000002500000001000000010000000b0000000000000004000000000000003700"
    "00"
    "000200000002000000d30000000000000003000000000000003d0000000100000003000000df00000000000000ffff"
    "ff"
    "ff00000000430000000100000004000000e30000000000000005000000000000004700000001000000020000000000"
    "00"
    "0003100000003200330010270000000000000100049cffffffffffffff640000000000000001000000000000003200"
    "00"
    "000000000005102700000000000001010114000000000000000602000000050000000000000000000000ffffffff00"
    "00"
    "000007010000000d000000000000000100050100000000000000010000000200010100000000000000020000000300"
    "01"
    "010000000000000003000000040004000000000000e03f040000000500020200000000000000050000000600027b00"
    "00"
    "000000000006000000070001010000000000000007000000080004000000000000c03f080000000900010100000000"
    "00"
    "0000090000000a000101000000000000000a0000000b00020c000000000000000b0000000c00021000000000000000"
    "0c"
    "0000000d00020800000000000000080100000001000000010000000100000001000000";

[[nodiscard]] constexpr std::uint64_t binary64_bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] constexpr std::size_t policy_index(sieve::ExecutionPolicyKeyV1 key) noexcept {
    return static_cast<std::size_t>(static_cast<std::uint16_t>(key) - 1U);
}

struct PolicySpec final {
    sieve::ExecutionPolicyKeyV1 key;
    sieve::ExecutionPolicyScalarKindV1 kind;
    std::uint64_t baseline;
    std::uint64_t alternate;
    bool semantic;
};

constexpr std::array<PolicySpec, sieve::DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1>
    POLICY_SPECS{{
        {sieve::ExecutionPolicyKeyV1::lattice_lll, sieve::ExecutionPolicyScalarKindV1::closed_mode,
         1, 2, true},
        {sieve::ExecutionPolicyKeyV1::lattice_skew, sieve::ExecutionPolicyScalarKindV1::boolean, 1,
         0, true},
        {sieve::ExecutionPolicyKeyV1::adaptive_lattice, sieve::ExecutionPolicyScalarKindV1::boolean,
         1, 0, true},
        {sieve::ExecutionPolicyKeyV1::adaptive_lattice_threshold,
         sieve::ExecutionPolicyScalarKindV1::ieee754_binary64, binary64_bits(0.5),
         binary64_bits(0.75), true},
        {sieve::ExecutionPolicyKeyV1::adaptive_lattice_max_retries,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3, true},
        {sieve::ExecutionPolicyKeyV1::adaptive_lattice_seed,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 123, 124, true},
        {sieve::ExecutionPolicyKeyV1::survival_filter, sieve::ExecutionPolicyScalarKindV1::boolean,
         1, 0, true},
        {sieve::ExecutionPolicyKeyV1::survival_threshold,
         sieve::ExecutionPolicyScalarKindV1::ieee754_binary64, binary64_bits(0.125),
         binary64_bits(0.25), true},
        {sieve::ExecutionPolicyKeyV1::cofactor_brent, sieve::ExecutionPolicyScalarKindV1::boolean,
         1, 0, true},
        {sieve::ExecutionPolicyKeyV1::ecm_brent_suyama, sieve::ExecutionPolicyScalarKindV1::boolean,
         1, 0, true},
        {sieve::ExecutionPolicyKeyV1::ecm_bs_degree,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 12, 30, true},
        {sieve::ExecutionPolicyKeyV1::ecm_sigma_pool_size,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 16, 32, true},
        {sieve::ExecutionPolicyKeyV1::ecm_curve_pool,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 8, 16, true},
        {sieve::ExecutionPolicyKeyV1::ecm_batch_inv, sieve::ExecutionPolicyScalarKindV1::boolean, 1,
         0, false},
        {sieve::ExecutionPolicyKeyV1::cofactor_batch_size,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 64, 128, false},
        {sieve::ExecutionPolicyKeyV1::brent_pollard_rho_threads,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3, false},
        {sieve::ExecutionPolicyKeyV1::ecm_b1_cache_size,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 16, 32, false},
        {sieve::ExecutionPolicyKeyV1::ecm_stage1_parallel_threads,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3, false},
        {sieve::ExecutionPolicyKeyV1::ecm_stage2_parallel,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 1, 2, false},
        {sieve::ExecutionPolicyKeyV1::cofactor_result_cache_size,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 128, 256, false},
        {sieve::ExecutionPolicyKeyV1::trial_div_simd,
         sieve::ExecutionPolicyScalarKindV1::closed_mode, 1, 2, false},
        {sieve::ExecutionPolicyKeyV1::lattice_basis_parallel_threads,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3, false},
        {sieve::ExecutionPolicyKeyV1::lattice_coords_simd,
         sieve::ExecutionPolicyScalarKindV1::closed_mode, 1, 2, false},
        {sieve::ExecutionPolicyKeyV1::sieve_apply_tile_threads,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3, false},
        {sieve::ExecutionPolicyKeyV1::bucket_prefetch,
         sieve::ExecutionPolicyScalarKindV1::closed_mode, 1, 2, false},
        {sieve::ExecutionPolicyKeyV1::sieve_ecore_threads,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3, false},
        {sieve::ExecutionPolicyKeyV1::sieve_no_tiny_simd,
         sieve::ExecutionPolicyScalarKindV1::boolean, 1, 0, false},
        {sieve::ExecutionPolicyKeyV1::sieve_norm_tile_bits,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 4, 5, false},
        {sieve::ExecutionPolicyKeyV1::sieve_region_tile_bits,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 4, 5, false},
        {sieve::ExecutionPolicyKeyV1::sieve_saturated_sub_simd,
         sieve::ExecutionPolicyScalarKindV1::closed_mode, 1, 2, false},
        {sieve::ExecutionPolicyKeyV1::sieve_count_above_threshold_simd,
         sieve::ExecutionPolicyScalarKindV1::closed_mode, 1, 2, false},
    }};

[[nodiscard]] sieve::DistributedSieveExecutionPolicyV1 make_execution_policy() {
    sieve::DistributedSieveExecutionPolicyV1 policy;
    policy.settings.reserve(POLICY_SPECS.size());
    for (const PolicySpec& spec : POLICY_SPECS) {
        policy.settings.push_back({spec.key, spec.kind, spec.baseline});
    }
    return policy;
}

[[nodiscard]] Identity make_identity() {
    Identity identity;
    identity.polynomial.n.decimal = "1000036000099";
    identity.polynomial.m.decimal = "10001";
    identity.polynomial.degree = 2;
    identity.polynomial.coefficients = {{"-5"}, {"3"}, {"1"}};
    identity.polynomial.skewness_ieee754_bits = binary64_bits(1.25);

    identity.factor_base.rational_bound = 100;
    identity.factor_base.algebraic_bound = 200;
    identity.factor_base.large_prime_bound = 10'000;
    identity.factor_base.log_scale = 16;
    identity.factor_base.rational = {{2, 16}, {5, 25}};
    identity.factor_base.algebraic = {
        {7, 1, 37, 1},   {11, 4, 55, 2},
        {211, 3, 61, 1}, {223, std::numeric_limits<std::uint32_t>::max(), 67, 1},
        {227, 5, 71, 1},
    };
    identity.factor_base.sieve_algebraic_count = 2;

    identity.sieve = {16, 50, 51, 10'000, true, false};
    identity.region = {-100, 100, 1, 50};
    identity.cofactor = {10'000, true, true, true, 20};
    identity.original_sq_bounds = {0, 5, 100, 1000};
    identity.effective_sq_bounds = {2, 5, 0, std::numeric_limits<std::uint32_t>::max()};

    identity.distributed.worker_count = 2;
    identity.distributed.chunks = {
        {0, 2, 3, "chunk_0"},
        {1, 3, 5, "chunk_1"},
    };
    identity.distributed.sq_cap_per_worker = 10;
    identity.distributed.relation_cap_per_worker = 100;
    identity.distributed.max_worker_attempts = 2;
    identity.distributed.max_merge_build_attempts = 2;
    identity.distributed.max_consumption_attempts = 2;
    identity.execution_policy = make_execution_policy();
    identity.semantic_versions = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    return identity;
}

class OracleWriter final {
public:
    explicit OracleWriter(std::string_view domain) {
        put_ascii(domain);
        put_u8(0);
    }

    void put_u8(std::uint8_t value) {
        bytes_.push_back(static_cast<std::byte>(value));
    }

    void put_u16(std::uint16_t value) {
        for (unsigned shift = 0; shift < 16; shift += 8) {
            put_u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            put_u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_u64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            put_u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_i64(std::int64_t value) {
        put_u64(std::bit_cast<std::uint64_t>(value));
    }

    void put_bool(bool value) {
        put_u8(value ? 1U : 0U);
    }

    void put_string(std::string_view value) {
        put_u32(static_cast<std::uint32_t>(value.size()));
        put_ascii(value);
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept {
        return bytes_;
    }

private:
    void put_ascii(std::string_view value) {
        for (const char character : value) {
            bytes_.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
        }
    }

    std::vector<std::byte> bytes_;
};

void oracle_hash_polynomial(OracleWriter& writer,
                            const sieve::PolynomialWorkIdentityV1& polynomial) {
    writer.put_u8(0x01);
    writer.put_string(polynomial.n.decimal);
    writer.put_string(polynomial.m.decimal);
    writer.put_u32(polynomial.degree);
    const std::uint32_t live_count = polynomial.degree + 1U;
    writer.put_u32(live_count);
    for (std::uint32_t index = 0; index < live_count; ++index) {
        writer.put_u32(index);
        writer.put_string(polynomial.coefficients[index].decimal);
    }
    writer.put_u64(polynomial.skewness_ieee754_bits);
}

void oracle_hash_factor_base(OracleWriter& writer,
                             const sieve::FactorBaseWorkIdentityV1& factor_base) {
    writer.put_u8(0x02);
    writer.put_u64(factor_base.rational_bound);
    writer.put_u64(factor_base.algebraic_bound);
    writer.put_u64(factor_base.large_prime_bound);
    writer.put_u32(factor_base.log_scale);
    writer.put_u32(static_cast<std::uint32_t>(factor_base.rational.size()));
    for (std::uint32_t index = 0; index < factor_base.rational.size(); ++index) {
        writer.put_u32(index);
        writer.put_u64(factor_base.rational[index].p);
        writer.put_u32(factor_base.rational[index].log_p);
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

void oracle_hash_sieve(OracleWriter& writer, const Identity& identity) {
    const std::uint64_t large_prime_bound = identity.sieve.large_prime_bound == 0
                                                ? identity.factor_base.large_prime_bound
                                                : identity.sieve.large_prime_bound;
    writer.put_u8(0x03);
    writer.put_u32(identity.sieve.log_scale);
    writer.put_u16(identity.sieve.rational_threshold);
    writer.put_u16(identity.sieve.algebraic_threshold);
    writer.put_u64(large_prime_bound);
    writer.put_bool(identity.sieve.allow_2lp);
    writer.put_bool(identity.sieve.allow_3lp);
}

void oracle_hash_region(OracleWriter& writer, const sieve::SieveRegionWorkIdentityV1& region) {
    writer.put_u8(0x04);
    writer.put_i64(region.i_min);
    writer.put_i64(region.i_max);
    writer.put_i64(region.j_min);
    writer.put_i64(region.j_max);
}

void oracle_hash_cofactor(OracleWriter& writer, const Identity& identity) {
    const std::uint64_t large_prime_bound = identity.cofactor.large_prime_bound == 0
                                                ? identity.factor_base.large_prime_bound
                                                : identity.cofactor.large_prime_bound;
    const std::uint64_t attempts =
        identity.cofactor.max_factorization_attempts == 0
            ? sieve::DISTRIBUTED_SIEVE_SEMANTIC_DEFAULT_MAX_FACTORIZATION_ATTEMPTS_V2
            : identity.cofactor.max_factorization_attempts;
    writer.put_u8(0x05);
    writer.put_u64(large_prime_bound);
    writer.put_bool(identity.cofactor.allow_1lp);
    writer.put_bool(identity.cofactor.allow_2lp);
    writer.put_bool(identity.cofactor.allow_3lp);
    writer.put_u64(attempts);
}

[[nodiscard]] std::vector<std::byte>
oracle_preimage(const Identity& identity, std::uint32_t root_schema_version,
                const SemanticVersions& semantic_versions,
                std::string_view domain = SEMANTIC_ROOT_DOMAIN) {
    OracleWriter writer(domain);
    writer.put_u32(root_schema_version);
    oracle_hash_polynomial(writer, identity.polynomial);
    oracle_hash_factor_base(writer, identity.factor_base);
    oracle_hash_sieve(writer, identity);
    oracle_hash_region(writer, identity.region);
    oracle_hash_cofactor(writer, identity);

    writer.put_u8(0x06);
    writer.put_u32(identity.effective_sq_bounds.start_index);
    writer.put_u32(identity.effective_sq_bounds.end_index);
    writer.put_u64(identity.effective_sq_bounds.min_q);
    writer.put_u64(identity.effective_sq_bounds.max_q);

    writer.put_u8(0x07);
    writer.put_u32(identity.execution_policy.schema_version);
    writer.put_u32(13);
    std::uint32_t semantic_ordinal = 0;
    for (std::size_t index = 0; index < POLICY_SPECS.size(); ++index) {
        if (!POLICY_SPECS[index].semantic) {
            continue;
        }
        const auto& setting = identity.execution_policy.settings[index];
        std::uint64_t canonical_bits = setting.canonical_bits;
        if (setting.kind == sieve::ExecutionPolicyScalarKindV1::ieee754_binary64 &&
            std::bit_cast<double>(canonical_bits) == 0.0) {
            canonical_bits = binary64_bits(0.0);
        }
        writer.put_u32(semantic_ordinal++);
        writer.put_u16(static_cast<std::uint16_t>(setting.key));
        writer.put_u8(static_cast<std::uint8_t>(setting.kind));
        writer.put_u64(canonical_bits);
    }
    CHECK(semantic_ordinal == 13);

    writer.put_u8(0x08);
    writer.put_u32(semantic_versions.special_q_enumeration);
    writer.put_u32(semantic_versions.lattice_candidate_generation);
    writer.put_u32(semantic_versions.candidate_collection_order);
    writer.put_u32(semantic_versions.cofactor_classification);
    writer.put_u32(semantic_versions.cofactor_input_digest);
    return writer.bytes();
}

[[nodiscard]] std::string bytes_hex(std::span<const std::byte> bytes) {
    constexpr char HEX[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(bytes.size() * 2);
    for (const std::byte byte : bytes) {
        const auto value = std::to_integer<std::uint8_t>(byte);
        encoded.push_back(HEX[value >> 4U]);
        encoded.push_back(HEX[value & 0x0fU]);
    }
    return encoded;
}

[[nodiscard]] std::string digest_hex(const Digest& digest) {
    const auto encoded = gnfs::util::encode_sha256_hex(digest);
    return {encoded.begin(), encoded.end()};
}

[[nodiscard]] Digest hash_bytes(const std::vector<std::byte>& bytes) {
    const auto digest = gnfs::util::sha256(std::span<const std::byte>(bytes.data(), bytes.size()));
    CHECK(digest.has_value());
    return *digest;
}

[[nodiscard]] Digest v1_digest_or_fail(const Identity& identity) {
    const auto result = sieve::distributed_sieve_work_digest(identity);
    CHECK(result);
    return *result.digest;
}

[[nodiscard]] Digest v2_root_or_fail(const Identity& identity) {
    const auto result = sieve::distributed_sieve_semantic_seed_root_v2(identity);
    CHECK(result);
    return *result.digest;
}

[[nodiscard]] CofactorSeedRequestV1 make_seed_request() {
    CofactorSeedRequestV1 request;
    request.coordinates.special_q_index = UINT32_C(0x01020304);
    request.coordinates.candidate_ordinal = UINT64_C(0x1112131415161718);
    request.side = CofactorSide::algebraic;
    for (std::size_t index = 0; index < request.cofactor_digest.bytes.size(); ++index) {
        request.cofactor_digest.bytes[index] =
            static_cast<std::byte>(static_cast<std::uint8_t>(0xa0U + index));
    }
    request.domain = CofactorRandomDomainV1::ecm_curve_schedule;
    request.algorithm_identity = UINT32_C(0x0a0b0c0d);
    return request;
}

[[nodiscard]] sieve::DeterministicRandomDomainV1
expected_distributed_domain(CofactorRandomDomainV1 domain) {
    switch (domain) {
    case CofactorRandomDomainV1::brent_pollard_rho:
        return sieve::DeterministicRandomDomainV1::pollard_rho;
    case CofactorRandomDomainV1::ecm_curve_schedule:
        return sieve::DeterministicRandomDomainV1::ecm_curve;
    }
    throw std::invalid_argument("unknown expected cofactor domain");
}

[[nodiscard]] gnfs::cofactor::CofactorSeed256
expected_distributed_seed(const Digest& semantic_root, const CofactorSeedRequestV1& request) {
    CHECK(request.coordinates.special_q_index <= std::numeric_limits<std::uint32_t>::max());
    const sieve::DeterministicRandomSeedRequestV1 distributed_request{
        .work_digest = semantic_root,
        .domain = expected_distributed_domain(request.domain),
        .chunk_id = 0,
        .sq_index = static_cast<std::uint32_t>(request.coordinates.special_q_index),
        .candidate_ordinal = request.coordinates.candidate_ordinal,
        .algorithm_identity = request.algorithm_identity,
        .cofactor_input_digest = request.cofactor_digest,
    };
    const auto result = sieve::derive_distributed_sieve_deterministic_seed(distributed_request);
    CHECK(result);
    return gnfs::cofactor::CofactorSeed256{.digest = *result.digest};
}

struct Baseline final {
    Identity identity;
    Digest v1_digest;
    Digest v2_root;
};

[[nodiscard]] Baseline make_baseline() {
    Identity identity = make_identity();
    CHECK(sieve::validate_distributed_sieve_work_identity(identity));
    return {identity, v1_digest_or_fail(identity), v2_root_or_fail(identity)};
}

struct Mutation final {
    std::string_view name;
    std::function<void(Identity&)> apply;
};

void require_projection(const Baseline& baseline, const Mutation& mutation, bool expect_v2_change) {
    Identity changed = baseline.identity;
    mutation.apply(changed);
    require_named(static_cast<bool>(sieve::validate_distributed_sieve_work_identity(changed)),
                  mutation.name, "mutation must remain a valid V1 identity");
    require_named(v1_digest_or_fail(changed) != baseline.v1_digest, mutation.name,
                  "V1 work digest must retain exact field sensitivity");
    const bool v2_changed = v2_root_or_fail(changed) != baseline.v2_root;
    require_named(v2_changed == expect_v2_change, mutation.name,
                  expect_v2_change ? "V2 root must change" : "V2 root must remain stable");
}

void test_independent_byte_and_digest_goldens() {
    const Baseline baseline = make_baseline();
    const auto preimage = oracle_preimage(
        baseline.identity, sieve::DISTRIBUTED_SIEVE_SEMANTIC_SEED_ROOT_SCHEMA_VERSION_V2,
        sieve::DISTRIBUTED_SIEVE_SEMANTIC_CONTRACT_VERSIONS_V2);

    CHECK(preimage.size() == 659);
    CHECK(bytes_hex(preimage) == EXPECTED_V2_PREIMAGE_HEX);
    CHECK(digest_hex(hash_bytes(preimage)) == EXPECTED_V2_ROOT_HEX);
    CHECK(digest_hex(baseline.v2_root) == EXPECTED_V2_ROOT_HEX);
    CHECK(digest_hex(baseline.v1_digest) == EXPECTED_V1_WORK_DIGEST_HEX);

    CHECK(sieve::DISTRIBUTED_SIEVE_SEMANTIC_SEED_ROOT_SCHEMA_VERSION_V2 == 2);
    const auto next_schema = oracle_preimage(
        baseline.identity, sieve::DISTRIBUTED_SIEVE_SEMANTIC_SEED_ROOT_SCHEMA_VERSION_V2 + 1U,
        sieve::DISTRIBUTED_SIEVE_SEMANTIC_CONTRACT_VERSIONS_V2);
    CHECK(hash_bytes(next_schema) != baseline.v2_root);

    const std::array<std::uint32_t SemanticVersions::*, 5> version_fields{
        &SemanticVersions::special_q_enumeration,
        &SemanticVersions::lattice_candidate_generation,
        &SemanticVersions::candidate_collection_order,
        &SemanticVersions::cofactor_classification,
        &SemanticVersions::cofactor_input_digest,
    };
    for (const auto field : version_fields) {
        SemanticVersions changed = sieve::DISTRIBUTED_SIEVE_SEMANTIC_CONTRACT_VERSIONS_V2;
        ++(changed.*field);
        CHECK(hash_bytes(oracle_preimage(
                  baseline.identity, sieve::DISTRIBUTED_SIEVE_SEMANTIC_SEED_ROOT_SCHEMA_VERSION_V2,
                  changed)) != baseline.v2_root);
    }
}

void test_cofactor_seed_provider_exact_projection_and_field_separation() {
    const Baseline baseline = make_baseline();
    const CofactorSeedRequestV1 request = make_seed_request();
    const SeedProvider provider(baseline.v2_root);
    const auto expected_seed = expected_distributed_seed(baseline.v2_root, request);
    const auto actual_seed = provider.seed_for(request);

    CHECK(provider.semantic_seed_root() == baseline.v2_root);
    CHECK(actual_seed == expected_seed);
    CHECK(provider.seed_for(request) == actual_seed);

    std::vector<CofactorSeedRequestV1> changed_requests;
    auto changed = request;
    ++changed.coordinates.special_q_index;
    changed_requests.push_back(changed);
    changed = request;
    ++changed.coordinates.candidate_ordinal;
    changed_requests.push_back(changed);
    changed = request;
    changed.cofactor_digest.bytes[0] ^= std::byte{1};
    changed_requests.push_back(changed);
    changed = request;
    changed.domain = CofactorRandomDomainV1::brent_pollard_rho;
    changed_requests.push_back(changed);
    changed = request;
    ++changed.algorithm_identity;
    changed_requests.push_back(changed);

    for (const CofactorSeedRequestV1& changed_request : changed_requests) {
        CHECK(provider.seed_for(changed_request) != actual_seed);
    }

    Digest changed_root = baseline.v2_root;
    changed_root.bytes.back() ^= std::byte{1};
    CHECK(SeedProvider(changed_root).seed_for(request) != actual_seed);
    const auto zero_root_expected = expected_distributed_seed(Digest{}, request);
    CHECK(SeedProvider(Digest{}).seed_for(request) == zero_root_expected);

    auto brent_request = request;
    brent_request.domain = CofactorRandomDomainV1::brent_pollard_rho;
    CHECK(provider.seed_for(brent_request) ==
          expected_distributed_seed(baseline.v2_root, brent_request));
    CHECK(provider.seed_for(brent_request) != actual_seed);

    auto boundary_request = request;
    boundary_request.coordinates.special_q_index = std::numeric_limits<std::uint32_t>::max();
    boundary_request.coordinates.candidate_ordinal = std::numeric_limits<std::uint64_t>::max();
    boundary_request.algorithm_identity = std::numeric_limits<std::uint32_t>::max();
    boundary_request.cofactor_digest = {};
    CHECK(provider.seed_for(boundary_request) ==
          expected_distributed_seed(baseline.v2_root, boundary_request));
}

void test_cofactor_seed_provider_rejects_invalid_requests() {
    const SeedProvider provider(make_baseline().v2_root);
    const CofactorSeedRequestV1 baseline = make_seed_request();
    const std::array<CofactorSeedRequestV1, 3> invalid_requests{
        [&] {
            auto value = baseline;
            value.side = static_cast<CofactorSide>(0xff);
            return value;
        }(),
        [&] {
            auto value = baseline;
            value.domain = static_cast<CofactorRandomDomainV1>(0xff);
            return value;
        }(),
        [&] {
            auto value = baseline;
            value.algorithm_identity = 0;
            return value;
        }(),
    };

    for (const auto& request : invalid_requests) {
        bool caught = false;
        try {
            (void)provider.seed_for(request);
        } catch (const std::invalid_argument&) {
            caught = true;
        }
        CHECK(caught);
    }

    auto oversized_special_q = baseline;
    oversized_special_q.coordinates.special_q_index =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;
    bool caught_overflow = false;
    try {
        (void)provider.seed_for(oversized_special_q);
    } catch (const std::overflow_error&) {
        caught_overflow = true;
    }
    CHECK(caught_overflow);
}

void test_runtime_unprojectable_ecm_policy_fails_closed() {
    const Identity baseline = make_baseline().identity;
    std::array<Identity, 2> invalid_runtime_policies{baseline, baseline};
    invalid_runtime_policies[0]
        .execution_policy.settings[policy_index(sieve::ExecutionPolicyKeyV1::ecm_brent_suyama)]
        .canonical_bits = 0;
    invalid_runtime_policies[1]
        .execution_policy.settings[policy_index(sieve::ExecutionPolicyKeyV1::ecm_bs_degree)]
        .canonical_bits = 0;

    for (const Identity& identity : invalid_runtime_policies) {
        CHECK(sieve::validate_distributed_sieve_work_identity(identity));
        const auto result = sieve::distributed_sieve_semantic_seed_root_v2(identity);
        CHECK(!result);
        CHECK(!result.digest.has_value());
        CHECK(result.status.error == sieve::DistributedSieveProtocolError::invalid_value);
    }
}

void test_topology_provenance_and_storage_exclusions() {
    const Baseline baseline = make_baseline();
    const std::vector<Mutation> mutations{
        {"canonical trailing coefficient storage",
         [](auto& value) { value.polynomial.coefficients.push_back({"0"}); }},
        {"original SQ start with same effective interval",
         [](auto& value) { value.original_sq_bounds.start_index = 1; }},
        {"original SQ end with same effective interval",
         [](auto& value) { value.original_sq_bounds.end_index = 6; }},
        {"original min q with same effective interval",
         [](auto& value) { value.original_sq_bounds.min_q = 99; }},
        {"original max q with same effective interval",
         [](auto& value) { value.original_sq_bounds.max_q = 999; }},
        {"single-worker topology",
         [](auto& value) {
             value.distributed.worker_count = 1;
             value.distributed.chunks = {{0, 2, 5, "solo"}};
         }},
        {"three-worker topology",
         [](auto& value) {
             value.distributed.worker_count = 3;
             value.distributed.chunks = {
                 {0, 2, 3, "chunk_0"},
                 {1, 3, 4, "chunk_1"},
                 {2, 4, 5, "chunk_2"},
             };
         }},
        {"chunk range split",
         [](auto& value) {
             value.distributed.chunks[0].sq_end = 4;
             value.distributed.chunks[1].sq_begin = 4;
         }},
        {"chunk artifact stem",
         [](auto& value) { value.distributed.chunks[0].relative_artifact_stem = "chunk_zero"; }},
        {"worker artifact assignment",
         [](auto& value) {
             std::swap(value.distributed.chunks[0].relative_artifact_stem,
                       value.distributed.chunks[1].relative_artifact_stem);
         }},
        {"SQ cap", [](auto& value) { ++value.distributed.sq_cap_per_worker; }},
        {"relation cap", [](auto& value) { ++value.distributed.relation_cap_per_worker; }},
        {"worker attempt budget", [](auto& value) { ++value.distributed.max_worker_attempts; }},
        {"merge attempt budget", [](auto& value) { ++value.distributed.max_merge_build_attempts; }},
        {"consumption attempt budget",
         [](auto& value) { ++value.distributed.max_consumption_attempts; }},
        {"V1 relation serialization version",
         [](auto& value) { ++value.semantic_versions.relation_serialization_version; }},
        {"V1 OOC version", [](auto& value) { ++value.semantic_versions.ooc_format_version; }},
        {"V1 digest version", [](auto& value) { ++value.semantic_versions.digest_version; }},
        {"V1 handoff version", [](auto& value) { ++value.semantic_versions.handoff_version; }},
        {"V1 retry version", [](auto& value) { ++value.semantic_versions.retry_policy_version; }},
        {"V1 chunking version", [](auto& value) { ++value.semantic_versions.chunking_version; }},
        {"V1 completion version",
         [](auto& value) { ++value.semantic_versions.completion_version; }},
        {"V1 deduplication version",
         [](auto& value) { ++value.semantic_versions.deduplication_version; }},
        {"V1 merge-policy version",
         [](auto& value) { ++value.semantic_versions.merge_policy_version; }},
    };

    for (const Mutation& mutation : mutations) {
        require_projection(baseline, mutation, false);
    }

    // Equality here is only an identity projection contract. The differing V1
    // digests above deliberately prove that it is not a claim of pipeline
    // result parity, complete candidate coverage, or RNG-consumer closure.
}

void test_execution_policy_classification_matrix() {
    const Baseline baseline = make_baseline();
    std::size_t semantic_count = 0;
    std::size_t conservative_count = 0;

    for (std::size_t index = 0; index < POLICY_SPECS.size(); ++index) {
        const PolicySpec& spec = POLICY_SPECS[index];
        const Mutation mutation{
            spec.semantic ? "semantic execution setting" : "conservative execution setting",
            [index, key = spec.key, alternate = spec.alternate](auto& value) {
                value.execution_policy.settings[index].canonical_bits = alternate;
                if (key == sieve::ExecutionPolicyKeyV1::ecm_brent_suyama && alternate == 0) {
                    value.execution_policy
                        .settings[policy_index(sieve::ExecutionPolicyKeyV1::ecm_bs_degree)]
                        .canonical_bits = 0;
                }
            },
        };
        require_projection(baseline, mutation, spec.semantic);
        semantic_count += spec.semantic ? 1U : 0U;
        conservative_count += spec.semantic ? 0U : 1U;
    }

    CHECK(semantic_count == 13);
    CHECK(conservative_count == 18);
}

void test_survival_threshold_signed_zero_normalization() {
    constexpr std::size_t survival_threshold_index = static_cast<std::size_t>(
        static_cast<std::uint16_t>(sieve::ExecutionPolicyKeyV1::survival_threshold) - 1U);
    constexpr std::uint64_t positive_zero_bits = binary64_bits(0.0);
    constexpr std::uint64_t negative_zero_bits = binary64_bits(-0.0);
    static_assert(positive_zero_bits != negative_zero_bits);

    Identity positive_zero = make_identity();
    positive_zero.execution_policy.settings[survival_threshold_index].canonical_bits =
        positive_zero_bits;
    Identity negative_zero = positive_zero;
    negative_zero.execution_policy.settings[survival_threshold_index].canonical_bits =
        negative_zero_bits;

    CHECK(sieve::validate_distributed_sieve_work_identity(positive_zero));
    CHECK(sieve::validate_distributed_sieve_work_identity(negative_zero));
    CHECK(v1_digest_or_fail(positive_zero) != v1_digest_or_fail(negative_zero));
    CHECK(v2_root_or_fail(positive_zero) == v2_root_or_fail(negative_zero));
    CHECK(hash_bytes(oracle_preimage(positive_zero,
                                     sieve::DISTRIBUTED_SIEVE_SEMANTIC_SEED_ROOT_SCHEMA_VERSION_V2,
                                     sieve::DISTRIBUTED_SIEVE_SEMANTIC_CONTRACT_VERSIONS_V2)) ==
          v2_root_or_fail(positive_zero));
    CHECK(hash_bytes(oracle_preimage(negative_zero,
                                     sieve::DISTRIBUTED_SIEVE_SEMANTIC_SEED_ROOT_SCHEMA_VERSION_V2,
                                     sieve::DISTRIBUTED_SIEVE_SEMANTIC_CONTRACT_VERSIONS_V2)) ==
          v2_root_or_fail(negative_zero));

    Identity positive_threshold = positive_zero;
    positive_threshold.execution_policy.settings[survival_threshold_index].canonical_bits =
        binary64_bits(0.25);
    CHECK(sieve::validate_distributed_sieve_work_identity(positive_threshold));
    CHECK(v2_root_or_fail(positive_threshold) != v2_root_or_fail(positive_zero));
}

void test_effective_default_sentinels() {
    const Baseline baseline = make_baseline();
    CHECK(sieve::DISTRIBUTED_SIEVE_SEMANTIC_DEFAULT_MAX_FACTORIZATION_ATTEMPTS_V2 == 10'000);
    require_projection(
        baseline, {"zero sieve LP bound", [](auto& value) { value.sieve.large_prime_bound = 0; }},
        false);
    require_projection(
        baseline,
        {"zero cofactor LP bound", [](auto& value) { value.cofactor.large_prime_bound = 0; }},
        false);

    Identity explicit_default = baseline.identity;
    explicit_default.cofactor.max_factorization_attempts =
        sieve::DISTRIBUTED_SIEVE_SEMANTIC_DEFAULT_MAX_FACTORIZATION_ATTEMPTS_V2;
    Identity sentinel = explicit_default;
    sentinel.cofactor.max_factorization_attempts = 0;
    CHECK(sieve::validate_distributed_sieve_work_identity(explicit_default));
    CHECK(sieve::validate_distributed_sieve_work_identity(sentinel));
    CHECK(v1_digest_or_fail(explicit_default) != v1_digest_or_fail(sentinel));
    CHECK(v2_root_or_fail(explicit_default) == v2_root_or_fail(sentinel));
    CHECK(hash_bytes(oracle_preimage(explicit_default,
                                     sieve::DISTRIBUTED_SIEVE_SEMANTIC_SEED_ROOT_SCHEMA_VERSION_V2,
                                     sieve::DISTRIBUTED_SIEVE_SEMANTIC_CONTRACT_VERSIONS_V2)) ==
          v2_root_or_fail(explicit_default));
    CHECK(hash_bytes(oracle_preimage(sentinel,
                                     sieve::DISTRIBUTED_SIEVE_SEMANTIC_SEED_ROOT_SCHEMA_VERSION_V2,
                                     sieve::DISTRIBUTED_SIEVE_SEMANTIC_CONTRACT_VERSIONS_V2)) ==
          v2_root_or_fail(sentinel));
}

void test_included_semantic_field_matrix() {
    const Baseline baseline = make_baseline();
    const std::vector<Mutation> mutations{
        {"N", [](auto& value) { value.polynomial.n.decimal = "1000036000101"; }},
        {"polynomial m", [](auto& value) { value.polynomial.m.decimal = "10003"; }},
        {"polynomial degree and live count",
         [](auto& value) {
             value.polynomial.degree = 3;
             value.polynomial.coefficients.push_back({"2"});
         }},
        {"active coefficient 0",
         [](auto& value) { value.polynomial.coefficients[0].decimal = "-7"; }},
        {"active coefficient 1",
         [](auto& value) { value.polynomial.coefficients[1].decimal = "4"; }},
        {"active coefficient 2",
         [](auto& value) { value.polynomial.coefficients[2].decimal = "2"; }},
        {"polynomial skew",
         [](auto& value) { value.polynomial.skewness_ieee754_bits = binary64_bits(1.5); }},
        {"rational factor-base bound", [](auto& value) { ++value.factor_base.rational_bound; }},
        {"algebraic factor-base bound", [](auto& value) { ++value.factor_base.algebraic_bound; }},
        {"factor-base large-prime bound",
         [](auto& value) { ++value.factor_base.large_prime_bound; }},
        {"factor-base log scale", [](auto& value) { ++value.factor_base.log_scale; }},
        {"rational factor-base prime", [](auto& value) { value.factor_base.rational[0].p = 3; }},
        {"rational factor-base log", [](auto& value) { ++value.factor_base.rational[0].log_p; }},
        {"rational factor-base count",
         [](auto& value) { value.factor_base.rational.push_back({7, 31}); }},
        {"algebraic factor-base prime", [](auto& value) { value.factor_base.algebraic[0].p = 5; }},
        {"algebraic factor-base root", [](auto& value) { ++value.factor_base.algebraic[0].r; }},
        {"algebraic factor-base log", [](auto& value) { ++value.factor_base.algebraic[0].log_p; }},
        {"algebraic factor-base degree",
         [](auto& value) { ++value.factor_base.algebraic[0].degree; }},
        {"algebraic factor-base count",
         [](auto& value) { value.factor_base.algebraic.push_back({229, 7, 73, 1}); }},
        {"sieve algebraic prefix count",
         [](auto& value) {
             value.factor_base.algebraic = {
                 {7, 1, 37, 1},
                 {211, 3, 61, 1},
                 {223, std::numeric_limits<std::uint32_t>::max(), 67, 1},
                 {227, 5, 71, 1},
                 {229, 7, 73, 1},
             };
             value.factor_base.sieve_algebraic_count = 1;
             value.effective_sq_bounds.start_index = 1;
             value.distributed.chunks = {
                 {0, 1, 3, "chunk_0"},
                 {1, 3, 5, "chunk_1"},
             };
         }},
        {"sieve log scale", [](auto& value) { ++value.sieve.log_scale; }},
        {"sieve rational threshold", [](auto& value) { ++value.sieve.rational_threshold; }},
        {"sieve algebraic threshold", [](auto& value) { ++value.sieve.algebraic_threshold; }},
        {"sieve large-prime bound", [](auto& value) { ++value.sieve.large_prime_bound; }},
        {"sieve allow 2LP", [](auto& value) { value.sieve.allow_2lp = false; }},
        {"sieve allow 3LP", [](auto& value) { value.sieve.allow_3lp = true; }},
        {"region i min", [](auto& value) { --value.region.i_min; }},
        {"region i max", [](auto& value) { ++value.region.i_max; }},
        {"region j min", [](auto& value) { ++value.region.j_min; }},
        {"region j max", [](auto& value) { ++value.region.j_max; }},
        {"cofactor large-prime bound", [](auto& value) { ++value.cofactor.large_prime_bound; }},
        {"cofactor allow 1LP", [](auto& value) { value.cofactor.allow_1lp = false; }},
        {"cofactor allow 2LP", [](auto& value) { value.cofactor.allow_2lp = false; }},
        {"cofactor allow 3LP", [](auto& value) { value.cofactor.allow_3lp = false; }},
        {"cofactor attempts", [](auto& value) { ++value.cofactor.max_factorization_attempts; }},
        {"effective SQ begin",
         [](auto& value) {
             value.original_sq_bounds.min_q = 212;
             value.effective_sq_bounds.start_index = 3;
             value.distributed.chunks = {
                 {0, 3, 4, "chunk_0"},
                 {1, 4, 5, "chunk_1"},
             };
         }},
        {"effective SQ end",
         [](auto& value) {
             value.original_sq_bounds.max_q = 223;
             value.effective_sq_bounds.end_index = 4;
             value.distributed.chunks = {
                 {0, 2, 3, "chunk_0"},
                 {1, 3, 4, "chunk_1"},
             };
         }},
    };

    for (const Mutation& mutation : mutations) {
        require_projection(baseline, mutation, true);
    }
}

void test_invalid_identity_fails_closed_with_v1_status() {
    const Identity baseline = make_identity();
    const std::vector<Mutation> invalid_mutations{
        {"noncanonical N", [](auto& value) { value.polynomial.n.decimal = "01"; }},
        {"noncanonical live coefficient",
         [](auto& value) { value.polynomial.coefficients[0].decimal = "00"; }},
        {"chunk range gap", [](auto& value) { value.distributed.chunks[0].sq_end = 2; }},
        {"missing execution setting",
         [](auto& value) { value.execution_policy.settings.pop_back(); }},
        {"zero excluded V1 semantic version",
         [](auto& value) { value.semantic_versions.digest_version = 0; }},
        {"noncanonical effective min q", [](auto& value) { value.effective_sq_bounds.min_q = 1; }},
        {"noncanonical effective max q",
         [](auto& value) { value.effective_sq_bounds.max_q = 1000; }},
    };

    for (const Mutation& mutation : invalid_mutations) {
        Identity invalid = baseline;
        mutation.apply(invalid);
        const auto v1_status = sieve::validate_distributed_sieve_work_identity(invalid);
        const auto v2_result = sieve::distributed_sieve_semantic_seed_root_v2(invalid);
        require_named(!static_cast<bool>(v1_status), mutation.name,
                      "fixture mutation must be invalid");
        require_named(!static_cast<bool>(v2_result), mutation.name,
                      "V2 projection must reject invalid V1 identity");
        require_named(!v2_result.digest.has_value(), mutation.name,
                      "invalid identity must not produce a root");
        require_named(v2_result.status.error == v1_status.error &&
                          v2_result.status.byte_offset == v1_status.byte_offset &&
                          v2_result.status.element_index == v1_status.element_index,
                      mutation.name, "V2 must preserve the exact V1 validation status");
    }
}

} // namespace

int main() {
    try {
        test_independent_byte_and_digest_goldens();
        test_cofactor_seed_provider_exact_projection_and_field_separation();
        test_cofactor_seed_provider_rejects_invalid_requests();
        test_runtime_unprojectable_ecm_policy_fails_closed();
        test_topology_provenance_and_storage_exclusions();
        test_execution_policy_classification_matrix();
        test_survival_threshold_signed_zero_normalization();
        test_effective_default_sentinels();
        test_included_semantic_field_matrix();
        test_invalid_identity_fails_closed_with_v1_status();
    } catch (const std::exception& error) {
        std::cerr << "test_distributed_sieve_seed_v2: " << error.what() << '\n';
        return 1;
    }

    std::cout << "Distributed sieve semantic seed-root V2 contract tests passed\n";
    return 0;
}
