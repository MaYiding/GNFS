#include <gnfs/factor_base/builder.hpp>
#include <gnfs/polynomial/base_m.hpp>
#include <gnfs/sieve/distributed_sieve_protocol.hpp>

#include "distributed_sieve_bound_work_internal.hpp"
#include "distributed_sieve_cofactor_runtime_config_internal.hpp"
#include "distributed_sieve_execution_policy_internal.hpp"
#include "distributed_sieve_lattice_runtime_config_internal.hpp"
#include "support/scoped_environment_stderr.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            throw std::runtime_error(std::string("CHECK failed: " #condition " at ") + __FILE__ +  \
                                     ":" + std::to_string(__LINE__));                              \
        }                                                                                          \
    } while (false)

namespace {

namespace sieve = gnfs::sieve;
namespace policy_detail = gnfs::sieve::distributed_sieve_execution_policy_detail;

using Key = sieve::ExecutionPolicyKeyV1;
using Kind = sieve::ExecutionPolicyScalarKindV1;
using Classification = policy_detail::DistributedSieveExecutionPolicyClassificationV1;
using FrozenPolicy = policy_detail::DistributedSieveFrozenExecutionPolicyV1;
using Snapshot = policy_detail::DistributedSieveExecutionPolicyEnvironmentSnapshotV1;
using TernaryMode = policy_detail::DistributedSieveCanonicalTernaryModeV1;
using LatticeRuntimeConfig = policy_detail::DistributedSieveLatticeRuntimeConfigV1;
using CofactorRuntime = policy_detail::DistributedSieveCofactorRuntimeV2;
using BoundWork = policy_detail::DistributedSieveBoundWorkV1;
using WorkIdentity = sieve::DistributedSieveWorkIdentityV1;
using gnfs::tests::support::ScopedEnvironmentVariable;

struct ExpectedDescriptor final {
    Key key;
    std::string_view environment_name;
    Kind kind;
    Classification classification;
    std::uint64_t all_unset_bits;
};

constexpr std::uint64_t binary64_bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

constexpr std::array<ExpectedDescriptor, sieve::DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1>
    EXPECTED_DESCRIPTORS{{
        {Key::lattice_lll, "GNFS_LATTICE_LLL", Kind::closed_mode, Classification::semantic, 2},
        {Key::lattice_skew, "GNFS_LATTICE_SKEW", Kind::boolean, Classification::semantic, 0},
        {Key::adaptive_lattice, "GNFS_ADAPTIVE_LATTICE", Kind::boolean, Classification::semantic,
         0},
        {Key::adaptive_lattice_threshold, "GNFS_ADAPTIVE_LATTICE_THRESHOLD", Kind::ieee754_binary64,
         Classification::semantic, binary64_bits(0.5)},
        {Key::adaptive_lattice_max_retries, "GNFS_ADAPTIVE_LATTICE_MAX_RETRIES",
         Kind::unsigned_integer, Classification::semantic, 2},
        {Key::adaptive_lattice_seed, "GNFS_ADAPTIVE_LATTICE_SEED", Kind::unsigned_integer,
         Classification::semantic, 0},
        {Key::survival_filter, "GNFS_SURVIVAL_FILTER", Kind::boolean, Classification::semantic, 0},
        {Key::survival_threshold, "GNFS_SURVIVAL_THRESHOLD", Kind::ieee754_binary64,
         Classification::semantic, binary64_bits(0.0)},
        {Key::cofactor_brent, "GNFS_COFACTOR_BRENT", Kind::boolean, Classification::semantic, 0},
        {Key::ecm_brent_suyama, "GNFS_ECM_BRENT_SUYAMA", Kind::boolean, Classification::semantic,
         0},
        {Key::ecm_bs_degree, "GNFS_ECM_BS_DEGREE", Kind::unsigned_integer, Classification::semantic,
         0},
        {Key::ecm_sigma_pool_size, "GNFS_ECM_SIGMA_POOL_SIZE", Kind::unsigned_integer,
         Classification::semantic, 0},
        {Key::ecm_curve_pool, "GNFS_ECM_CURVE_POOL", Kind::unsigned_integer,
         Classification::semantic, 0},
        {Key::ecm_batch_inv, "GNFS_ECM_BATCH_INV", Kind::boolean, Classification::conservative, 0},
        {Key::cofactor_batch_size, "GNFS_COFACTOR_BATCH_SIZE", Kind::unsigned_integer,
         Classification::conservative, 1},
        {Key::brent_pollard_rho_threads, "GNFS_BRENT_POLLARD_RHO_THREADS", Kind::unsigned_integer,
         Classification::conservative, 1},
        {Key::ecm_b1_cache_size, "GNFS_ECM_B1_CACHE_SIZE", Kind::unsigned_integer,
         Classification::conservative, 0},
        {Key::ecm_stage1_parallel_threads, "GNFS_ECM_STAGE1_PARALLEL_THREADS",
         Kind::unsigned_integer, Classification::conservative, 1},
        {Key::ecm_stage2_parallel, "GNFS_ECM_STAGE2_PARALLEL", Kind::unsigned_integer,
         Classification::conservative, 1},
        {Key::cofactor_result_cache_size, "GNFS_COFACTOR_RESULT_CACHE_SIZE", Kind::unsigned_integer,
         Classification::conservative, 0},
        {Key::trial_div_simd, "GNFS_TRIAL_DIV_SIMD", Kind::closed_mode,
         Classification::conservative, 1},
        {Key::lattice_basis_parallel_threads, "GNFS_LATTICE_BASIS_PARALLEL_THREADS",
         Kind::unsigned_integer, Classification::conservative, 1},
        {Key::lattice_coords_simd, "GNFS_LATTICE_COORDS_SIMD", Kind::closed_mode,
         Classification::conservative, 1},
        {Key::sieve_apply_tile_threads, "GNFS_SIEVE_APPLY_TILE_THREADS", Kind::unsigned_integer,
         Classification::conservative, 1},
        {Key::bucket_prefetch, "GNFS_BUCKET_PREFETCH", Kind::closed_mode,
         Classification::conservative, 1},
        {Key::sieve_ecore_threads, "GNFS_SIEVE_ECORE_THREADS", Kind::unsigned_integer,
         Classification::conservative, 0},
        {Key::sieve_no_tiny_simd, "GNFS_SIEVE_NO_TINY_SIMD", Kind::boolean,
         Classification::conservative, 0},
        {Key::sieve_norm_tile_bits, "GNFS_SIEVE_NORM_TILE_BITS", Kind::unsigned_integer,
         Classification::conservative, 0},
        {Key::sieve_region_tile_bits, "GNFS_SIEVE_REGION_TILE_BITS", Kind::unsigned_integer,
         Classification::conservative, 0},
        {Key::sieve_saturated_sub_simd, "GNFS_SIEVE_SATURATED_SUB_SIMD", Kind::closed_mode,
         Classification::conservative, 1},
        {Key::sieve_count_above_threshold_simd, "GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD",
         Kind::closed_mode, Classification::conservative, 1},
    }};

constexpr std::size_t policy_index(Key key) noexcept {
    return static_cast<std::size_t>(static_cast<std::uint16_t>(key) - 1U);
}

Snapshot unset_snapshot(std::uint32_t hardware_concurrency = 8) {
    Snapshot snapshot;
    snapshot.hardware_concurrency = hardware_concurrency;
    return snapshot;
}

void set_raw(Snapshot& snapshot, Key key, std::string_view raw) {
    snapshot.canonical_values[policy_index(key)] = std::string(raw);
}

const sieve::ExecutionPolicySettingV1& setting(const FrozenPolicy& frozen, Key key) {
    const std::size_t index = policy_index(key);
    CHECK(index < frozen.canonical.settings.size());
    const auto& result = frozen.canonical.settings[index];
    CHECK(result.key == key);
    return result;
}

std::uint64_t bits(const FrozenPolicy& frozen, Key key) {
    return setting(frozen, key).canonical_bits;
}

void synchronize_host_bound_thread(FrozenPolicy& frozen, Key key, std::uint32_t value) {
    switch (key) {
    case Key::brent_pollard_rho_threads:
        frozen.cofactor.brent_pollard_rho_threads = value;
        break;
    case Key::ecm_stage1_parallel_threads:
        frozen.cofactor.ecm_stage1_parallel_threads = value;
        break;
    case Key::ecm_stage2_parallel:
        frozen.cofactor.ecm_stage2_parallel = value;
        break;
    case Key::lattice_basis_parallel_threads:
        frozen.sieve.lattice_basis_parallel_threads = value;
        break;
    case Key::sieve_apply_tile_threads:
        frozen.sieve.sieve_apply_tile_threads = value;
        break;
    case Key::sieve_ecore_threads:
        frozen.sieve.sieve_ecore_threads = value;
        break;
    default:
        throw std::runtime_error("not a host-bound execution-policy key");
    }

    auto& canonical = frozen.canonical.settings[policy_index(key)];
    CHECK(canonical.key == key);
    canonical.canonical_bits = value;
}

bool same_canonical_policy(const sieve::DistributedSieveExecutionPolicyV1& left,
                           const sieve::DistributedSieveExecutionPolicyV1& right) {
    if (left.schema_version != right.schema_version ||
        left.settings.size() != right.settings.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.settings.size(); ++index) {
        if (left.settings[index].key != right.settings[index].key ||
            left.settings[index].kind != right.settings[index].kind ||
            left.settings[index].canonical_bits != right.settings[index].canonical_bits) {
            return false;
        }
    }
    return true;
}

FrozenPolicy freeze_checked(const Snapshot& snapshot) {
    auto result = policy_detail::freeze_distributed_sieve_execution_policy_v1(snapshot);
    CHECK(result);
    CHECK(result.policy.has_value());
    CHECK(policy_detail::validate_distributed_sieve_frozen_execution_policy_v1(*result.policy));
    CHECK(sieve::validate_distributed_sieve_execution_policy(result.policy->canonical));

    const auto projected = policy_detail::project_distributed_sieve_execution_policy_v1(
        result.policy->sieve, result.policy->cofactor);
    CHECK(projected);
    CHECK(projected.policy.has_value());
    CHECK(same_canonical_policy(result.policy->canonical, *projected.policy));
    return std::move(*result.policy);
}

FrozenPolicy rehydrate_checked(const sieve::DistributedSieveExecutionPolicyV1& canonical) {
    auto result = policy_detail::rehydrate_distributed_sieve_execution_policy_v1(canonical);
    CHECK(result);
    CHECK(result.policy.has_value());
    CHECK(policy_detail::validate_distributed_sieve_frozen_execution_policy_v1(*result.policy));
    CHECK(!result.policy->diagnostics.cofactor_timing_enabled);
    CHECK(same_canonical_policy(result.policy->canonical, canonical));
    return std::move(*result.policy);
}

void expect_rehydrate_rejected(const sieve::DistributedSieveExecutionPolicyV1& canonical) {
    const auto result = policy_detail::rehydrate_distributed_sieve_execution_policy_v1(canonical);
    CHECK(!result);
    CHECK(!result.policy.has_value());
    CHECK(!result.status);
}

LatticeRuntimeConfig map_lattice_runtime_checked(const FrozenPolicy& frozen) {
    auto result = policy_detail::map_distributed_sieve_lattice_runtime_config_v1(frozen);
    CHECK(result);
    CHECK(result.config.has_value());
    return *result.config;
}

WorkIdentity make_runtime_identity(const FrozenPolicy& frozen) {
    WorkIdentity identity;
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
    identity.factor_base.algebraic = {{7, 1, 37, 1}, {11, 4, 55, 2}};
    identity.factor_base.sieve_algebraic_count = 2;

    identity.sieve = {16, 50, 51, 0, true, false};
    identity.region = {-100, 100, 1, 50};
    identity.cofactor = {0, true, true, false, 0};
    identity.original_sq_bounds = {0, 2, 0, std::numeric_limits<std::uint32_t>::max()};
    identity.effective_sq_bounds = {0, 2, 0, std::numeric_limits<std::uint32_t>::max()};
    identity.distributed.worker_count = 1;
    identity.distributed.chunks = {{0, 0, 2, "chunk_0"}};
    identity.distributed.max_worker_attempts = 1;
    identity.distributed.max_merge_build_attempts = 1;
    identity.distributed.max_consumption_attempts = 1;
    identity.execution_policy = frozen.canonical;
    identity.semantic_versions = policy_detail::DISTRIBUTED_SIEVE_BOUND_WORK_VERSIONS_V1;
    CHECK(sieve::validate_distributed_sieve_work_identity(identity));
    return identity;
}

CofactorRuntime map_cofactor_runtime_checked(const WorkIdentity& identity,
                                             const FrozenPolicy& frozen) {
    auto result = policy_detail::map_distributed_sieve_cofactor_runtime_v2(identity, frozen);
    CHECK(result);
    CHECK(result.runtime.has_value());
    return *result.runtime;
}

gnfs::core::PolynomialContext
make_live_polynomial(std::string_view n = "1000036000099", std::string_view m = "10001",
                     std::vector<std::string_view> coefficients = {"-5", "3", "1"},
                     double skewness = 1.25) {
    std::vector<gnfs::core::Integer> live_coefficients;
    live_coefficients.reserve(coefficients.size());
    for (const auto coefficient : coefficients) {
        live_coefficients.emplace_back(std::string(coefficient));
    }
    return gnfs::core::PolynomialContext{gnfs::core::Integer(std::string(n)),
                                         std::move(live_coefficients),
                                         gnfs::core::Integer(std::string(m)), skewness};
}

struct LiveFactorBaseSpec final {
    gnfs::core::FactorBaseParams params{100, 200, 10'000, 16};
    std::vector<gnfs::core::RationalPrime> rational{{2, 16}, {5, 25}};
    std::vector<gnfs::core::AlgebraicPrime> algebraic{{7, 1, 37, 1}, {11, 4, 55, 2}};
    std::size_t sieve_algebraic_count = 2;
};

gnfs::factor_base::FactorBase make_live_factor_base(const LiveFactorBaseSpec& spec = {}) {
    gnfs::factor_base::FactorBase factor_base(spec.params);
    factor_base.reserve(spec.rational.size(), spec.algebraic.size());
    for (const auto& entry : spec.rational) {
        factor_base.add_rational(entry.p, entry.log_p);
    }
    for (const auto& entry : spec.algebraic) {
        factor_base.add_algebraic(entry.p, entry.r, entry.log_p, entry.degree);
    }
    factor_base.set_sieve_algebraic_count(spec.sieve_algebraic_count);
    factor_base.build_index();
    return factor_base;
}

BoundWork bind_work_checked(const WorkIdentity& identity, const FrozenPolicy& frozen,
                            const gnfs::core::PolynomialContext& polynomial,
                            const gnfs::factor_base::FactorBase& factor_base) {
    auto result =
        policy_detail::bind_distributed_sieve_work_v1(identity, frozen, polynomial, factor_base);
    CHECK(result);
    CHECK(result.work.has_value());
    return std::move(*result.work);
}

void expect_work_binding_rejected(const WorkIdentity& identity, const FrozenPolicy& frozen,
                                  const gnfs::core::PolynomialContext& polynomial,
                                  const gnfs::factor_base::FactorBase& factor_base) {
    const auto result =
        policy_detail::bind_distributed_sieve_work_v1(identity, frozen, polynomial, factor_base);
    CHECK(!result);
    CHECK(!result.work.has_value());
    CHECK(!result.status);
}

bool same_lattice_runtime_config(const LatticeRuntimeConfig& lhs,
                                 const LatticeRuntimeConfig& rhs) noexcept {
    return lhs.sieve.lattice_basis.base_method == rhs.sieve.lattice_basis.base_method &&
           lhs.sieve.lattice_basis.skew_enabled == rhs.sieve.lattice_basis.skew_enabled &&
           lhs.sieve.adaptive_lattice.enabled == rhs.sieve.adaptive_lattice.enabled &&
           lhs.sieve.adaptive_lattice.density_threshold ==
               rhs.sieve.adaptive_lattice.density_threshold &&
           lhs.sieve.adaptive_lattice.max_retries == rhs.sieve.adaptive_lattice.max_retries &&
           lhs.sieve.adaptive_lattice.perturb_seed == rhs.sieve.adaptive_lattice.perturb_seed &&
           lhs.sieve.fallback_thread_count == rhs.sieve.fallback_thread_count &&
           lhs.sieve.ecore_thread_count == rhs.sieve.ecore_thread_count &&
           lhs.sieve.enable_tiny_simd == rhs.sieve.enable_tiny_simd &&
           lhs.sieve.enable_bucket_prefetch == rhs.sieve.enable_bucket_prefetch &&
           lhs.lattice_basis_parallel_threads == rhs.lattice_basis_parallel_threads;
}

bool same_sieve_result(const sieve::SieveResult& lhs, const sieve::SieveResult& rhs) noexcept {
    if (lhs.special_q.q != rhs.special_q.q || lhs.special_q.r != rhs.special_q.r ||
        lhs.special_q.index != rhs.special_q.index ||
        lhs.sieved_positions != rhs.sieved_positions || lhs.smooth_count != rhs.smooth_count ||
        lhs.candidates.size() != rhs.candidates.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.candidates.size(); ++index) {
        const auto& left = lhs.candidates[index];
        const auto& right = rhs.candidates[index];
        if (left.i != right.i || left.j != right.j || left.a != right.a || left.b != right.b ||
            left.residual != right.residual) {
            return false;
        }
    }
    return true;
}

void expect_bits(Key key, std::string_view raw, std::uint64_t expected,
                 std::uint32_t hardware_concurrency = 8, bool enable_ecm_brent_suyama = false) {
    auto snapshot = unset_snapshot(hardware_concurrency);
    if (enable_ecm_brent_suyama) {
        set_raw(snapshot, Key::ecm_brent_suyama, "1");
    }
    set_raw(snapshot, key, raw);
    const auto frozen = freeze_checked(snapshot);
    CHECK(bits(frozen, key) == expected);
}

void test_descriptor_inventory() {
    const auto descriptors = policy_detail::distributed_sieve_execution_policy_descriptors_v1();
    CHECK(EXPECTED_DESCRIPTORS.size() ==
          sieve::DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1);
    CHECK(descriptors.size() ==
          policy_detail::DISTRIBUTED_SIEVE_EXECUTION_POLICY_DESCRIPTOR_COUNT_V1);
    CHECK(descriptors.size() == EXPECTED_DESCRIPTORS.size() + 1U);

    std::array<bool, sieve::DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1> seen{};
    for (std::size_t index = 0; index < EXPECTED_DESCRIPTORS.size(); ++index) {
        const auto& expected = EXPECTED_DESCRIPTORS[index];
        const auto& actual = descriptors[index];
        CHECK(actual.key.has_value());
        CHECK(*actual.key == expected.key);
        CHECK(static_cast<std::uint16_t>(*actual.key) == index + 1U);
        CHECK(actual.environment_name == expected.environment_name);
        CHECK(actual.kind == expected.kind);
        CHECK(actual.classification == expected.classification);
        CHECK(actual.default_canonical_bits == expected.all_unset_bits);
        CHECK(!seen[policy_index(*actual.key)]);
        seen[policy_index(*actual.key)] = true;
    }
    CHECK(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }));

    const auto& diagnostic = descriptors.back();
    CHECK(!diagnostic.key.has_value());
    CHECK(diagnostic.environment_name == "GNFS_COFACTOR_TIMING_ENABLE");
    CHECK(diagnostic.kind == Kind::boolean);
    CHECK(diagnostic.classification == Classification::diagnostic);
    CHECK(diagnostic.default_canonical_bits == 0);
}

void test_all_unset_exact_policy() {
    const auto frozen = freeze_checked(unset_snapshot());
    CHECK(frozen.bound_hardware_concurrency == 8);
    CHECK(!frozen.diagnostics.cofactor_timing_enabled);
    CHECK(frozen.canonical.schema_version == sieve::DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1);
    CHECK(frozen.canonical.settings.size() == EXPECTED_DESCRIPTORS.size());

    for (std::size_t index = 0; index < EXPECTED_DESCRIPTORS.size(); ++index) {
        const auto& expected = EXPECTED_DESCRIPTORS[index];
        const auto& actual = frozen.canonical.settings[index];
        CHECK(actual.key == expected.key);
        CHECK(actual.kind == expected.kind);
        CHECK(actual.canonical_bits == expected.all_unset_bits);
    }
}

void test_rehydrate_decodes_every_canonical_setting() {
    struct RehydrateCase final {
        Key key;
        std::string_view raw;
        bool enable_ecm_brent_suyama = false;
    };

    constexpr std::array<RehydrateCase, sieve::DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1>
        CASES{{
            {Key::lattice_lll, "0"},
            {Key::lattice_skew, "1"},
            {Key::adaptive_lattice, "1"},
            {Key::adaptive_lattice_threshold, "0.75"},
            {Key::adaptive_lattice_max_retries, "5"},
            {Key::adaptive_lattice_seed, "42"},
            {Key::survival_filter, "1"},
            {Key::survival_threshold, "0.25"},
            {Key::cofactor_brent, "1"},
            {Key::ecm_brent_suyama, "1"},
            {Key::ecm_bs_degree, "30", true},
            {Key::ecm_sigma_pool_size, "8"},
            {Key::ecm_curve_pool, "8"},
            {Key::ecm_batch_inv, "1"},
            {Key::cofactor_batch_size, "16"},
            {Key::brent_pollard_rho_threads, "5"},
            {Key::ecm_b1_cache_size, "7"},
            {Key::ecm_stage1_parallel_threads, "5"},
            {Key::ecm_stage2_parallel, "5"},
            {Key::cofactor_result_cache_size, "64"},
            {Key::trial_div_simd, "0"},
            {Key::lattice_basis_parallel_threads, "5"},
            {Key::lattice_coords_simd, "0"},
            {Key::sieve_apply_tile_threads, "5"},
            {Key::bucket_prefetch, "0"},
            {Key::sieve_ecore_threads, "3"},
            {Key::sieve_no_tiny_simd, "1"},
            {Key::sieve_norm_tile_bits, "4"},
            {Key::sieve_region_tile_bits, "4"},
            {Key::sieve_saturated_sub_simd, "0"},
            {Key::sieve_count_above_threshold_simd, "0"},
        }};

    std::array<bool, sieve::DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1> seen{};
    for (const auto& test_case : CASES) {
        auto snapshot = unset_snapshot(8);
        snapshot.cofactor_timing = "1";
        if (test_case.enable_ecm_brent_suyama) {
            set_raw(snapshot, Key::ecm_brent_suyama, "1");
        }
        set_raw(snapshot, test_case.key, test_case.raw);

        const auto source = freeze_checked(snapshot);
        CHECK(source.diagnostics.cofactor_timing_enabled);
        CHECK(bits(source, test_case.key) !=
              EXPECTED_DESCRIPTORS[policy_index(test_case.key)].all_unset_bits);

        const auto rehydrated = rehydrate_checked(source.canonical);
        CHECK(rehydrated.sieve == source.sieve);
        CHECK(rehydrated.cofactor == source.cofactor);
        CHECK(!rehydrated.diagnostics.cofactor_timing_enabled);

        CHECK(!seen[policy_index(test_case.key)]);
        seen[policy_index(test_case.key)] = true;
    }
    CHECK(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }));
}

void test_rehydrate_derives_minimum_host_witness_without_ambient_inputs() {
    const auto source = freeze_checked(unset_snapshot(64));
    const auto baseline = rehydrate_checked(source.canonical);
    CHECK(baseline.bound_hardware_concurrency == 1);

    {
        auto canonical = source.canonical;
        canonical.settings[policy_index(Key::lattice_basis_parallel_threads)].canonical_bits = 5;
        const auto rehydrated = rehydrate_checked(canonical);
        CHECK(rehydrated.bound_hardware_concurrency == 3);
        CHECK(rehydrated.sieve.lattice_basis_parallel_threads == 5);
    }
    {
        auto canonical = source.canonical;
        canonical.settings[policy_index(Key::sieve_ecore_threads)].canonical_bits = 3;
        const auto rehydrated = rehydrate_checked(canonical);
        CHECK(rehydrated.bound_hardware_concurrency == 4);
        CHECK(rehydrated.sieve.sieve_ecore_threads == 3);
    }
    {
        auto canonical = source.canonical;
        canonical.settings[policy_index(Key::brent_pollard_rho_threads)].canonical_bits =
            std::numeric_limits<std::uint32_t>::max();
        const auto rehydrated = rehydrate_checked(canonical);
        CHECK(rehydrated.bound_hardware_concurrency == UINT32_C(1) << 31U);
        CHECK(rehydrated.cofactor.brent_pollard_rho_threads ==
              std::numeric_limits<std::uint32_t>::max());
    }
    {
        auto canonical = source.canonical;
        canonical.settings[policy_index(Key::brent_pollard_rho_threads)].canonical_bits =
            std::numeric_limits<std::uint32_t>::max();
        canonical.settings[policy_index(Key::sieve_ecore_threads)].canonical_bits =
            std::numeric_limits<std::uint32_t>::max() - 1U;
        const auto rehydrated = rehydrate_checked(canonical);
        CHECK(rehydrated.bound_hardware_concurrency == std::numeric_limits<std::uint32_t>::max());
    }

    ScopedEnvironmentVariable ambient_lll("GNFS_LATTICE_LLL", "0");
    ScopedEnvironmentVariable ambient_parallel_threads("GNFS_LATTICE_BASIS_PARALLEL_THREADS",
                                                       "999");
    ScopedEnvironmentVariable ambient_timing("GNFS_COFACTOR_TIMING_ENABLE", "1");
    const auto under_changed_environment = rehydrate_checked(source.canonical);
    CHECK(under_changed_environment.sieve == baseline.sieve);
    CHECK(under_changed_environment.cofactor == baseline.cofactor);
    CHECK(under_changed_environment.bound_hardware_concurrency ==
          baseline.bound_hardware_concurrency);
    CHECK(!under_changed_environment.diagnostics.cofactor_timing_enabled);
}

void test_rehydrate_rejects_invalid_canonical_shapes_and_combinations() {
    const auto source = freeze_checked(unset_snapshot(8));

    {
        auto canonical = source.canonical;
        ++canonical.schema_version;
        expect_rehydrate_rejected(canonical);
    }
    {
        auto canonical = source.canonical;
        canonical.settings.pop_back();
        expect_rehydrate_rejected(canonical);
    }
    {
        auto canonical = source.canonical;
        std::swap(canonical.settings[0], canonical.settings[1]);
        expect_rehydrate_rejected(canonical);
    }
    {
        auto canonical = source.canonical;
        canonical.settings[policy_index(Key::lattice_skew)].kind = Kind::unsigned_integer;
        expect_rehydrate_rejected(canonical);
    }
    {
        auto canonical = source.canonical;
        canonical.settings[policy_index(Key::lattice_skew)].canonical_bits = 2;
        expect_rehydrate_rejected(canonical);
    }
    {
        auto canonical = source.canonical;
        canonical.settings[policy_index(Key::ecm_bs_degree)].canonical_bits = 12;
        CHECK(sieve::validate_distributed_sieve_execution_policy(canonical));
        expect_rehydrate_rejected(canonical);
    }
    {
        auto canonical = source.canonical;
        canonical.settings[policy_index(Key::ecm_brent_suyama)].canonical_bits = 1;
        CHECK(sieve::validate_distributed_sieve_execution_policy(canonical));
        expect_rehydrate_rejected(canonical);
    }
    {
        auto canonical = source.canonical;
        canonical.settings[policy_index(Key::sieve_ecore_threads)].canonical_bits =
            std::numeric_limits<std::uint32_t>::max();
        CHECK(sieve::validate_distributed_sieve_execution_policy(canonical));
        expect_rehydrate_rejected(canonical);
    }
}

void test_lattice_reduction_and_boolean_normalization() {
    using LatticeReduction = policy_detail::DistributedSieveCanonicalLatticeReductionV1;
    static_assert(static_cast<std::uint8_t>(LatticeReduction::gauss) == 1);
    static_assert(static_cast<std::uint8_t>(LatticeReduction::lll) == 2);

    expect_bits(Key::lattice_lll, "0", 1);
    expect_bits(Key::lattice_lll, "gauss", 1);
    expect_bits(Key::lattice_lll, "GAUSS", 1);
    expect_bits(Key::lattice_lll, "1", 2);
    expect_bits(Key::lattice_lll, "lll", 2);
    expect_bits(Key::lattice_lll, "invalid", 2);

    for (const Key key : {Key::lattice_skew, Key::adaptive_lattice}) {
        expect_bits(key, "1", 1);
        expect_bits(key, "on", 1);
        expect_bits(key, "TRUE", 1);
        expect_bits(key, "0", 0);
        expect_bits(key, "yes", 0);
    }
    for (const Key key :
         {Key::survival_filter, Key::cofactor_brent, Key::ecm_brent_suyama, Key::ecm_batch_inv}) {
        expect_bits(key, "1", 1);
        expect_bits(key, "0", 0);
        expect_bits(key, "true", 0);
        expect_bits(key, " 1", 0);
    }
}

void test_floating_and_adaptive_integer_normalization() {
    expect_bits(Key::adaptive_lattice_threshold, "0.0001", binary64_bits(0.0001));
    expect_bits(Key::adaptive_lattice_threshold, "100", binary64_bits(100.0));
    expect_bits(Key::adaptive_lattice_threshold, "0.75suffix", binary64_bits(0.75));
    expect_bits(Key::adaptive_lattice_threshold, "+0.75suffix", binary64_bits(0.75));
    expect_bits(Key::adaptive_lattice_threshold, "0x1.8p+0suffix", binary64_bits(1.5));
    expect_bits(Key::adaptive_lattice_threshold, "0", binary64_bits(0.5));
    expect_bits(Key::adaptive_lattice_threshold, "101", binary64_bits(0.5));
    expect_bits(Key::adaptive_lattice_threshold, "nan", binary64_bits(0.5));
    expect_bits(Key::adaptive_lattice_threshold, "invalid", binary64_bits(0.5));

    expect_bits(Key::survival_threshold, "0", binary64_bits(0.0));
    expect_bits(Key::survival_threshold, "1", binary64_bits(1.0));
    expect_bits(Key::survival_threshold, "0.25suffix", binary64_bits(0.25));
    expect_bits(Key::survival_threshold, "0x1p-2suffix", binary64_bits(0.25));
    expect_bits(Key::survival_threshold, "-1", binary64_bits(0.0));
    expect_bits(Key::survival_threshold, "2", binary64_bits(0.0));
    expect_bits(Key::survival_threshold, "nan", binary64_bits(0.0));
    expect_bits(Key::survival_threshold, "invalid", binary64_bits(0.0));

    expect_bits(Key::adaptive_lattice_max_retries, "0", 0);
    expect_bits(Key::adaptive_lattice_max_retries, "16", 16);
    expect_bits(Key::adaptive_lattice_max_retries, "5suffix", 5);
    expect_bits(Key::adaptive_lattice_max_retries, "-1", 2);
    expect_bits(Key::adaptive_lattice_max_retries, "17", 2);
    expect_bits(Key::adaptive_lattice_max_retries, "invalid", 2);

    expect_bits(Key::adaptive_lattice_seed, "0", 0);
    expect_bits(Key::adaptive_lattice_seed, "42suffix", 42);
    expect_bits(Key::adaptive_lattice_seed, "18446744073709551615",
                std::numeric_limits<std::uint64_t>::max());
    expect_bits(Key::adaptive_lattice_seed, "18446744073709551616",
                std::numeric_limits<std::uint64_t>::max());
    expect_bits(Key::adaptive_lattice_seed, "invalid", 0);
}

void test_lattice_runtime_config_exact_mapping() {
    const auto all_unset = map_lattice_runtime_checked(freeze_checked(unset_snapshot()));
    CHECK(all_unset.sieve.lattice_basis.base_method == sieve::LatticeReductionMethod::LLL);
    CHECK(!all_unset.sieve.lattice_basis.skew_enabled);
    CHECK(!all_unset.sieve.adaptive_lattice.enabled);
    CHECK(all_unset.sieve.adaptive_lattice.density_threshold == 0.5);
    CHECK(all_unset.sieve.adaptive_lattice.max_retries == 2);
    CHECK(all_unset.sieve.adaptive_lattice.perturb_seed == 0);
    CHECK(all_unset.sieve.fallback_thread_count == 1);
    CHECK(all_unset.sieve.ecore_thread_count == 0);
    CHECK(all_unset.sieve.enable_tiny_simd);
    CHECK(all_unset.sieve.enable_bucket_prefetch == sieve::bucket_prefetch_supported());
    CHECK(all_unset.lattice_basis_parallel_threads == 1);

    auto snapshot = unset_snapshot(8);
    set_raw(snapshot, Key::sieve_ecore_threads, "3");
    set_raw(snapshot, Key::sieve_no_tiny_simd, "1");
    set_raw(snapshot, Key::bucket_prefetch, "0");
    auto mapped = map_lattice_runtime_checked(freeze_checked(snapshot));
    CHECK(mapped.sieve.fallback_thread_count == 1);
    CHECK(mapped.sieve.ecore_thread_count == 3);
    CHECK(!mapped.sieve.enable_tiny_simd);
    CHECK(!mapped.sieve.enable_bucket_prefetch);

    snapshot = unset_snapshot();
    set_raw(snapshot, Key::lattice_lll, "0");
    set_raw(snapshot, Key::lattice_skew, "1");
    mapped = map_lattice_runtime_checked(freeze_checked(snapshot));
    CHECK(mapped.sieve.lattice_basis.base_method == sieve::LatticeReductionMethod::Gauss);
    CHECK(mapped.sieve.lattice_basis.skew_enabled);

    snapshot = unset_snapshot();
    set_raw(snapshot, Key::lattice_lll, "1");
    set_raw(snapshot, Key::lattice_skew, "1");
    mapped = map_lattice_runtime_checked(freeze_checked(snapshot));
    CHECK(mapped.sieve.lattice_basis.base_method == sieve::LatticeReductionMethod::LLL);
    CHECK(mapped.sieve.lattice_basis.skew_enabled);

    snapshot = unset_snapshot(4);
    set_raw(snapshot, Key::adaptive_lattice, "1");
    set_raw(snapshot, Key::adaptive_lattice_threshold, "0.0001");
    set_raw(snapshot, Key::adaptive_lattice_max_retries, "0");
    set_raw(snapshot, Key::adaptive_lattice_seed, "0");
    set_raw(snapshot, Key::lattice_basis_parallel_threads, "1");
    mapped = map_lattice_runtime_checked(freeze_checked(snapshot));
    CHECK(mapped.sieve.adaptive_lattice.enabled);
    CHECK(mapped.sieve.adaptive_lattice.density_threshold == 0.0001);
    CHECK(mapped.sieve.adaptive_lattice.max_retries == 0);
    CHECK(mapped.sieve.adaptive_lattice.perturb_seed == 0);
    CHECK(mapped.lattice_basis_parallel_threads == 1);

    snapshot = unset_snapshot(4);
    set_raw(snapshot, Key::adaptive_lattice, "TRUE");
    set_raw(snapshot, Key::adaptive_lattice_threshold, "100");
    set_raw(snapshot, Key::adaptive_lattice_max_retries, "16");
    set_raw(snapshot, Key::adaptive_lattice_seed, "18446744073709551615");
    set_raw(snapshot, Key::lattice_basis_parallel_threads, "999");
    mapped = map_lattice_runtime_checked(freeze_checked(snapshot));
    CHECK(mapped.sieve.adaptive_lattice.enabled);
    CHECK(mapped.sieve.adaptive_lattice.density_threshold == 100.0);
    CHECK(mapped.sieve.adaptive_lattice.max_retries == 16);
    CHECK(mapped.sieve.adaptive_lattice.perturb_seed == std::numeric_limits<std::uint64_t>::max());
    CHECK(mapped.lattice_basis_parallel_threads == 8);
}

void test_lattice_runtime_config_rejects_invalid_frozen_object() {
    auto typed_drift = freeze_checked(unset_snapshot(4));
    typed_drift.sieve.lattice_skew = !typed_drift.sieve.lattice_skew;
    auto result = policy_detail::map_distributed_sieve_lattice_runtime_config_v1(typed_drift);
    CHECK(!result);
    CHECK(!result.config.has_value());

    auto canonical_drift = freeze_checked(unset_snapshot(4));
    canonical_drift.canonical.settings[policy_index(Key::adaptive_lattice_seed)].canonical_bits =
        42;
    result = policy_detail::map_distributed_sieve_lattice_runtime_config_v1(canonical_drift);
    CHECK(!result);
    CHECK(!result.config.has_value());

    auto host_bound_invalid = freeze_checked(unset_snapshot(4));
    synchronize_host_bound_thread(host_bound_invalid, Key::lattice_basis_parallel_threads, 9);
    CHECK(sieve::validate_distributed_sieve_execution_policy(host_bound_invalid.canonical));
    result = policy_detail::map_distributed_sieve_lattice_runtime_config_v1(host_bound_invalid);
    CHECK(!result);
    CHECK(!result.config.has_value());
}

void test_cofactor_runtime_exact_mapping_and_seed_root_binding() {
    const auto all_unset_policy = freeze_checked(unset_snapshot());
    const auto sentinel_identity = make_runtime_identity(all_unset_policy);
    const auto sentinel_runtime = map_cofactor_runtime_checked(sentinel_identity, all_unset_policy);
    const auto& sentinel_config = sentinel_runtime.cofactorizer;
    CHECK(sentinel_config.large_prime_bound == 10'000);
    CHECK(sentinel_config.allow_1lp);
    CHECK(sentinel_config.allow_2lp);
    CHECK(!sentinel_config.allow_3lp);
    CHECK(sentinel_config.max_factorization_attempts ==
          sieve::DISTRIBUTED_SIEVE_SEMANTIC_DEFAULT_MAX_FACTORIZATION_ATTEMPTS_V2);
    CHECK(!sentinel_config.seeded_brent_pollard_enabled);
    CHECK(sentinel_config.seeded_ecm_brent_suyama_degree == 0);

    const auto sentinel_root = sieve::distributed_sieve_semantic_seed_root_v2(sentinel_identity);
    CHECK(sentinel_root);
    CHECK(sentinel_runtime.seed_provider.semantic_seed_root() == *sentinel_root.digest);

    auto explicit_identity = sentinel_identity;
    explicit_identity.cofactor.large_prime_bound = explicit_identity.factor_base.large_prime_bound;
    explicit_identity.cofactor.max_factorization_attempts =
        sieve::DISTRIBUTED_SIEVE_SEMANTIC_DEFAULT_MAX_FACTORIZATION_ATTEMPTS_V2;
    const auto explicit_runtime = map_cofactor_runtime_checked(explicit_identity, all_unset_policy);
    CHECK(explicit_runtime.cofactorizer.large_prime_bound ==
          sentinel_runtime.cofactorizer.large_prime_bound);
    CHECK(explicit_runtime.cofactorizer.max_factorization_attempts ==
          sentinel_runtime.cofactorizer.max_factorization_attempts);
    CHECK(explicit_runtime.seed_provider.semantic_seed_root() ==
          sentinel_runtime.seed_provider.semantic_seed_root());

    auto enabled_snapshot = unset_snapshot();
    set_raw(enabled_snapshot, Key::cofactor_brent, "1");
    set_raw(enabled_snapshot, Key::ecm_brent_suyama, "1");
    set_raw(enabled_snapshot, Key::ecm_bs_degree, "12");
    const auto enabled_policy = freeze_checked(enabled_snapshot);
    auto enabled_identity = make_runtime_identity(enabled_policy);
    enabled_identity.cofactor.large_prime_bound = 20'000;
    enabled_identity.cofactor.allow_1lp = false;
    enabled_identity.cofactor.allow_2lp = false;
    enabled_identity.cofactor.allow_3lp = true;
    enabled_identity.cofactor.max_factorization_attempts = 50'000;
    const auto enabled_runtime = map_cofactor_runtime_checked(enabled_identity, enabled_policy);
    CHECK(enabled_runtime.cofactorizer.large_prime_bound == 20'000);
    CHECK(!enabled_runtime.cofactorizer.allow_1lp);
    CHECK(!enabled_runtime.cofactorizer.allow_2lp);
    CHECK(enabled_runtime.cofactorizer.allow_3lp);
    CHECK(enabled_runtime.cofactorizer.max_factorization_attempts == 50'000);
    CHECK(enabled_runtime.cofactorizer.seeded_brent_pollard_enabled);
    CHECK(enabled_runtime.cofactorizer.seeded_ecm_brent_suyama_degree == 12);
    CHECK(enabled_runtime.seed_provider.semantic_seed_root() !=
          sentinel_runtime.seed_provider.semantic_seed_root());
}

void test_cofactor_runtime_topology_and_conservative_policy_invariance() {
    const auto baseline_policy = freeze_checked(unset_snapshot());
    const auto baseline_identity = make_runtime_identity(baseline_policy);
    const auto baseline_runtime = map_cofactor_runtime_checked(baseline_identity, baseline_policy);

    auto topology_identity = baseline_identity;
    topology_identity.distributed.worker_count = 2;
    topology_identity.distributed.chunks = {
        {0, 0, 1, "left"},
        {1, 1, 2, "right"},
    };
    CHECK(sieve::validate_distributed_sieve_work_identity(topology_identity));
    const auto topology_runtime = map_cofactor_runtime_checked(topology_identity, baseline_policy);
    CHECK(topology_runtime.seed_provider.semantic_seed_root() ==
          baseline_runtime.seed_provider.semantic_seed_root());

    auto conservative_snapshot = unset_snapshot();
    set_raw(conservative_snapshot, Key::cofactor_batch_size, "64");
    const auto conservative_policy = freeze_checked(conservative_snapshot);
    const auto conservative_identity = make_runtime_identity(conservative_policy);
    const auto conservative_runtime =
        map_cofactor_runtime_checked(conservative_identity, conservative_policy);
    CHECK(conservative_runtime.seed_provider.semantic_seed_root() ==
          baseline_runtime.seed_provider.semantic_seed_root());

    gnfs::cofactor::CofactorSeedRequestV1 request;
    request.coordinates = {17, 23};
    request.side = gnfs::cofactor::CofactorSide::algebraic;
    request.domain = gnfs::cofactor::CofactorRandomDomainV1::ecm_curve_schedule;
    request.algorithm_identity = 1;
    CHECK(topology_runtime.seed_provider.seed_for(request) ==
          baseline_runtime.seed_provider.seed_for(request));
    CHECK(conservative_runtime.seed_provider.seed_for(request) ==
          baseline_runtime.seed_provider.seed_for(request));
}

void test_cofactor_runtime_rejects_identity_policy_split_brain() {
    const auto baseline_policy = freeze_checked(unset_snapshot(4));
    const auto baseline_identity = make_runtime_identity(baseline_policy);

    auto changed_snapshot = unset_snapshot(4);
    set_raw(changed_snapshot, Key::cofactor_brent, "1");
    const auto changed_policy = freeze_checked(changed_snapshot);
    const auto changed_identity = make_runtime_identity(changed_policy);

    auto result =
        policy_detail::map_distributed_sieve_cofactor_runtime_v2(changed_identity, baseline_policy);
    CHECK(!result);
    CHECK(!result.runtime.has_value());
    CHECK(result.status.error == sieve::DistributedSieveProtocolError::invalid_value);

    auto invalid_frozen = baseline_policy;
    invalid_frozen.cofactor.cofactor_brent = true;
    result =
        policy_detail::map_distributed_sieve_cofactor_runtime_v2(baseline_identity, invalid_frozen);
    CHECK(!result);
    CHECK(!result.runtime.has_value());

    auto invalid_identity = baseline_identity;
    invalid_identity.polynomial.n.decimal = "0";
    result =
        policy_detail::map_distributed_sieve_cofactor_runtime_v2(invalid_identity, baseline_policy);
    CHECK(!result);
    CHECK(!result.runtime.has_value());
    CHECK(result.status.error == sieve::DistributedSieveProtocolError::invalid_value);

    auto survival_snapshot = unset_snapshot(4);
    set_raw(survival_snapshot, Key::survival_filter, "1");
    set_raw(survival_snapshot, Key::survival_threshold, "0.25");
    const auto survival_policy = freeze_checked(survival_snapshot);
    const auto survival_identity = make_runtime_identity(survival_policy);
    result = policy_detail::map_distributed_sieve_cofactor_runtime_v2(survival_identity,
                                                                      survival_policy);
    CHECK(!result);
    CHECK(!result.runtime.has_value());
    CHECK(result.status.error == sieve::DistributedSieveProtocolError::invalid_value);

    for (const auto [key, raw] : std::array<std::pair<Key, std::string_view>, 2>{
             {{Key::ecm_sigma_pool_size, "16"}, {Key::ecm_curve_pool, "8"}}}) {
        auto unsupported_pool_snapshot = unset_snapshot(4);
        set_raw(unsupported_pool_snapshot, key, raw);
        const auto unsupported_pool_policy = freeze_checked(unsupported_pool_snapshot);
        const auto unsupported_pool_identity = make_runtime_identity(unsupported_pool_policy);
        CHECK(sieve::distributed_sieve_semantic_seed_root_v2(unsupported_pool_identity));
        result = policy_detail::map_distributed_sieve_cofactor_runtime_v2(unsupported_pool_identity,
                                                                          unsupported_pool_policy);
        CHECK(!result);
        CHECK(!result.runtime.has_value());
        CHECK(result.status.error == sieve::DistributedSieveProtocolError::invalid_value);
    }
}

void test_ecm_enable_degree_invariant() {
    for (const std::uint64_t degree :
         {UINT64_C(1), UINT64_C(2), UINT64_C(6), UINT64_C(12), UINT64_C(30)}) {
        expect_bits(Key::ecm_bs_degree, std::to_string(degree), degree, 8, true);
    }

    auto snapshot = unset_snapshot();
    set_raw(snapshot, Key::ecm_bs_degree, "30");
    auto frozen = freeze_checked(snapshot);
    CHECK(!frozen.cofactor.ecm_brent_suyama);
    CHECK(frozen.cofactor.ecm_bs_degree == 0);

    for (const std::string_view invalid_degree : {"", "0", "3", "31", "-1", "invalid"}) {
        snapshot = unset_snapshot();
        set_raw(snapshot, Key::ecm_brent_suyama, "1");
        set_raw(snapshot, Key::ecm_bs_degree, invalid_degree);
        frozen = freeze_checked(snapshot);
        CHECK(frozen.cofactor.ecm_brent_suyama);
        CHECK(frozen.cofactor.ecm_bs_degree == 12);
    }

    snapshot = unset_snapshot();
    set_raw(snapshot, Key::ecm_brent_suyama, "true");
    set_raw(snapshot, Key::ecm_bs_degree, "30");
    frozen = freeze_checked(snapshot);
    CHECK(!frozen.cofactor.ecm_brent_suyama);
    CHECK(frozen.cofactor.ecm_bs_degree == 0);
}

void test_cofactor_integer_normalization() {
    for (const auto& [raw, expected] : std::array<std::pair<std::string_view, std::uint64_t>, 7>{{
             {"1", 1},
             {"1024", 1024},
             {"1025", 1024},
             {"12suffix", 12},
             {"0", 0},
             {"-1", 0},
             {"invalid", 0},
         }}) {
        expect_bits(Key::ecm_sigma_pool_size, raw, expected);
    }
    for (const auto& [raw, expected] : std::array<std::pair<std::string_view, std::uint64_t>, 7>{{
             {"4", 4},
             {"1024", 1024},
             {"1025", 1024},
             {"3", 0},
             {"0", 0},
             {"-1", 1024},
             {"invalid", 0},
         }}) {
        expect_bits(Key::ecm_curve_pool, raw, expected);
    }
    for (const auto& [raw, expected] : std::array<std::pair<std::string_view, std::uint64_t>, 7>{{
             {"2", 2},
             {"4096", 4096},
             {"4097", 4096},
             {"1", 1},
             {"0", 1},
             {"-1", 4096},
             {"invalid", 1},
         }}) {
        expect_bits(Key::cofactor_batch_size, raw, expected);
    }
    for (const auto& [raw, expected] : std::array<std::pair<std::string_view, std::uint64_t>, 7>{{
             {"1", 1},
             {"32", 32},
             {"33", 32},
             {"12suffix", 12},
             {" 32", 0},
             {"-1", 0},
             {"invalid", 0},
         }}) {
        expect_bits(Key::ecm_b1_cache_size, raw, expected);
    }
    for (const auto& [raw, expected] : std::array<std::pair<std::string_view, std::uint64_t>, 7>{{
             {"1", 1},
             {"1048576", 1048576},
             {"1048577", 1048576},
             {"42suffix", 42},
             {" 42", 0},
             {"-1", 0},
             {"invalid", 0},
         }}) {
        expect_bits(Key::cofactor_result_cache_size, raw, expected);
    }
}

void test_modes_bucket_intent_and_no_tiny_semantics() {
    static_assert(static_cast<std::uint8_t>(TernaryMode::automatic) == 1);
    static_assert(static_cast<std::uint8_t>(TernaryMode::force_off) == 2);
    static_assert(static_cast<std::uint8_t>(TernaryMode::force_on) == 3);

    for (const Key key : {Key::trial_div_simd, Key::bucket_prefetch}) {
        expect_bits(key, "1", 3);
        expect_bits(key, "0", 2);
        expect_bits(key, "on", 1);
        expect_bits(key, "off", 1);
        expect_bits(key, "invalid", 1);
    }
    for (const Key key : {Key::lattice_coords_simd, Key::sieve_saturated_sub_simd,
                          Key::sieve_count_above_threshold_simd}) {
        expect_bits(key, "1", 3);
        expect_bits(key, "on", 3);
        expect_bits(key, "0", 2);
        expect_bits(key, "off", 2);
        expect_bits(key, "ON", 1);
        expect_bits(key, "invalid", 1);
    }

    auto snapshot = unset_snapshot();
    set_raw(snapshot, Key::bucket_prefetch, "1");
    auto frozen = freeze_checked(snapshot);
    CHECK(frozen.sieve.bucket_prefetch == TernaryMode::force_on);
    CHECK(bits(frozen, Key::bucket_prefetch) == 3);

    expect_bits(Key::sieve_no_tiny_simd, "0", 0);
    expect_bits(Key::sieve_no_tiny_simd, "1", 1);
    expect_bits(Key::sieve_no_tiny_simd, "off", 1);
    expect_bits(Key::sieve_no_tiny_simd, "00", 1);
    snapshot = unset_snapshot();
    frozen = freeze_checked(snapshot);
    CHECK(!frozen.sieve.sieve_no_tiny_simd);
    set_raw(snapshot, Key::sieve_no_tiny_simd, "1");
    frozen = freeze_checked(snapshot);
    CHECK(frozen.sieve.sieve_no_tiny_simd);
}

void test_tile_and_thread_boundaries() {
    for (const Key key : {Key::sieve_norm_tile_bits, Key::sieve_region_tile_bits}) {
        expect_bits(key, "1", 1);
        expect_bits(key, "8", 8);
        expect_bits(key, "9", 8);
        expect_bits(key, "0", 0);
        expect_bits(key, "-1", 0);
        expect_bits(key, "7suffix", 0);
        expect_bits(key, "invalid", 0);
    }

    for (const Key key : {Key::brent_pollard_rho_threads, Key::ecm_stage1_parallel_threads,
                          Key::ecm_stage2_parallel, Key::lattice_basis_parallel_threads,
                          Key::sieve_apply_tile_threads}) {
        expect_bits(key, "1", 1, 4);
        expect_bits(key, "8", 8, 4);
        expect_bits(key, "9", 8, 4);
        expect_bits(key, "7suffix", 7, 4);
        expect_bits(key, "0", 1, 4);
        expect_bits(key, "-1", 1, 4);
        expect_bits(key, "invalid", 1, 4);
    }

    expect_bits(Key::sieve_ecore_threads, "1", 1, 8);
    expect_bits(Key::sieve_ecore_threads, "7", 7, 8);
    expect_bits(Key::sieve_ecore_threads, "8", 7, 8);
    expect_bits(Key::sieve_ecore_threads, "9", 7, 8);
    expect_bits(Key::sieve_ecore_threads, "0", 0, 8);
    expect_bits(Key::sieve_ecore_threads, "-1", 0, 8);
    expect_bits(Key::sieve_ecore_threads, "invalid", 0, 8);
    expect_bits(Key::sieve_ecore_threads, "1", 0, 1);

    auto snapshot = unset_snapshot(0);
    for (const Key key : {Key::brent_pollard_rho_threads, Key::ecm_stage1_parallel_threads,
                          Key::ecm_stage2_parallel, Key::lattice_basis_parallel_threads,
                          Key::sieve_apply_tile_threads, Key::sieve_ecore_threads}) {
        set_raw(snapshot, key, "999");
    }
    auto frozen = freeze_checked(snapshot);
    CHECK(frozen.bound_hardware_concurrency == 4);
    CHECK(frozen.cofactor.brent_pollard_rho_threads == 8);
    CHECK(frozen.cofactor.ecm_stage1_parallel_threads == 8);
    CHECK(frozen.cofactor.ecm_stage2_parallel == 8);
    CHECK(frozen.sieve.lattice_basis_parallel_threads == 8);
    CHECK(frozen.sieve.sieve_apply_tile_threads == 8);
    CHECK(frozen.sieve.sieve_ecore_threads == 3);

    snapshot = unset_snapshot(3);
    for (const Key key : {Key::brent_pollard_rho_threads, Key::ecm_stage1_parallel_threads,
                          Key::ecm_stage2_parallel, Key::lattice_basis_parallel_threads,
                          Key::sieve_apply_tile_threads, Key::sieve_ecore_threads}) {
        set_raw(snapshot, key, "999");
    }
    frozen = freeze_checked(snapshot);
    CHECK(frozen.bound_hardware_concurrency == 3);
    CHECK(frozen.cofactor.brent_pollard_rho_threads == 6);
    CHECK(frozen.cofactor.ecm_stage1_parallel_threads == 6);
    CHECK(frozen.cofactor.ecm_stage2_parallel == 6);
    CHECK(frozen.sieve.lattice_basis_parallel_threads == 6);
    CHECK(frozen.sieve.sieve_apply_tile_threads == 6);
    CHECK(frozen.sieve.sieve_ecore_threads == 2);
}

void test_frozen_policy_rejects_host_bound_and_ecm_drift() {
    constexpr std::array<Key, 5> HOST_BOUND_THREAD_KEYS{
        Key::brent_pollard_rho_threads, Key::ecm_stage1_parallel_threads,
        Key::ecm_stage2_parallel,       Key::lattice_basis_parallel_threads,
        Key::sieve_apply_tile_threads,
    };

    auto zero_host = freeze_checked(unset_snapshot(4));
    zero_host.bound_hardware_concurrency = 0;
    CHECK(sieve::validate_distributed_sieve_execution_policy(zero_host.canonical));
    CHECK(!policy_detail::validate_distributed_sieve_frozen_execution_policy_v1(zero_host));

    for (const Key key : HOST_BOUND_THREAD_KEYS) {
        auto over_cap = freeze_checked(unset_snapshot(4));
        synchronize_host_bound_thread(over_cap, key, std::numeric_limits<std::uint32_t>::max());
        CHECK(sieve::validate_distributed_sieve_execution_policy(over_cap.canonical));
        CHECK(!policy_detail::validate_distributed_sieve_frozen_execution_policy_v1(over_cap));
    }

    auto ecore_over_cap = freeze_checked(unset_snapshot(4));
    synchronize_host_bound_thread(ecore_over_cap, Key::sieve_ecore_threads,
                                  std::numeric_limits<std::uint32_t>::max());
    CHECK(sieve::validate_distributed_sieve_execution_policy(ecore_over_cap.canonical));
    CHECK(!policy_detail::validate_distributed_sieve_frozen_execution_policy_v1(ecore_over_cap));

    auto legal_edges = freeze_checked(unset_snapshot(4));
    for (const Key key : HOST_BOUND_THREAD_KEYS) {
        synchronize_host_bound_thread(legal_edges, key, 8);
    }
    synchronize_host_bound_thread(legal_edges, Key::sieve_ecore_threads, 3);
    CHECK(policy_detail::validate_distributed_sieve_frozen_execution_policy_v1(legal_edges));

    auto overflow_safe_edges = freeze_checked(unset_snapshot(4));
    overflow_safe_edges.bound_hardware_concurrency = std::numeric_limits<std::uint32_t>::max();
    for (const Key key : HOST_BOUND_THREAD_KEYS) {
        synchronize_host_bound_thread(overflow_safe_edges, key,
                                      std::numeric_limits<std::uint32_t>::max());
    }
    synchronize_host_bound_thread(overflow_safe_edges, Key::sieve_ecore_threads,
                                  std::numeric_limits<std::uint32_t>::max() - 1U);
    CHECK(
        policy_detail::validate_distributed_sieve_frozen_execution_policy_v1(overflow_safe_edges));

    auto disabled_with_degree = freeze_checked(unset_snapshot(4));
    disabled_with_degree.cofactor.ecm_bs_degree = 30;
    disabled_with_degree.canonical.settings[policy_index(Key::ecm_bs_degree)].canonical_bits = 30;
    CHECK(sieve::validate_distributed_sieve_execution_policy(disabled_with_degree.canonical));
    CHECK(!policy_detail::validate_distributed_sieve_frozen_execution_policy_v1(
        disabled_with_degree));

    auto enabled_without_degree = freeze_checked(unset_snapshot(4));
    enabled_without_degree.cofactor.ecm_brent_suyama = true;
    enabled_without_degree.cofactor.ecm_bs_degree = 0;
    enabled_without_degree.canonical.settings[policy_index(Key::ecm_brent_suyama)].canonical_bits =
        1;
    enabled_without_degree.canonical.settings[policy_index(Key::ecm_bs_degree)].canonical_bits = 0;
    CHECK(sieve::validate_distributed_sieve_execution_policy(enabled_without_degree.canonical));
    CHECK(!policy_detail::validate_distributed_sieve_frozen_execution_policy_v1(
        enabled_without_degree));
}

struct CaptureFixture final {
    std::array<std::optional<std::string>,
               policy_detail::DISTRIBUTED_SIEVE_EXECUTION_POLICY_DESCRIPTOR_COUNT_V1>
        values;
    mutable std::array<std::size_t,
                       policy_detail::DISTRIBUTED_SIEVE_EXECUTION_POLICY_DESCRIPTOR_COUNT_V1>
        read_counts{};
    mutable std::vector<std::string> observed_names;
    mutable std::size_t unknown_reads = 0;
    mutable std::size_t hardware_reads = 0;
    std::uint32_t hardware_concurrency = 0;
};

std::optional<std::string> capture_environment_reader(const void* context, std::string_view name) {
    const auto& fixture = *static_cast<const CaptureFixture*>(context);
    for (std::size_t index = 0; index < EXPECTED_DESCRIPTORS.size(); ++index) {
        if (name == EXPECTED_DESCRIPTORS[index].environment_name) {
            ++fixture.read_counts[index];
            fixture.observed_names.emplace_back(name);
            return fixture.values[index];
        }
    }
    if (name == "GNFS_COFACTOR_TIMING_ENABLE") {
        const std::size_t index = EXPECTED_DESCRIPTORS.size();
        ++fixture.read_counts[index];
        fixture.observed_names.emplace_back(name);
        return fixture.values[index];
    }
    ++fixture.unknown_reads;
    return std::nullopt;
}

std::uint32_t capture_hardware_reader(const void* context) noexcept {
    const auto& fixture = *static_cast<const CaptureFixture*>(context);
    ++fixture.hardware_reads;
    return fixture.hardware_concurrency;
}

void test_capture_is_single_read_and_owned() {
    CaptureFixture fixture;
    fixture.hardware_concurrency = 3;
    fixture.values[policy_index(Key::lattice_lll)] = "0";
    fixture.values[policy_index(Key::survival_filter)] = "1";
    fixture.values[policy_index(Key::brent_pollard_rho_threads)] = "999";
    fixture.values[EXPECTED_DESCRIPTORS.size()] = "1";

    const policy_detail::DistributedSieveExecutionPolicyCaptureSourcesV1 sources{
        &fixture, capture_environment_reader, capture_hardware_reader};
    auto captured =
        policy_detail::capture_distributed_sieve_execution_policy_environment_v1(sources);
    CHECK(captured);
    CHECK(captured.snapshot.has_value());
    CHECK(fixture.unknown_reads == 0);
    CHECK(fixture.hardware_reads == 1);
    CHECK(std::all_of(fixture.read_counts.begin(), fixture.read_counts.end(),
                      [](std::size_t count) { return count == 1; }));
    CHECK(fixture.observed_names.size() == fixture.read_counts.size());
    for (std::size_t index = 0; index < EXPECTED_DESCRIPTORS.size(); ++index) {
        CHECK(fixture.observed_names[index] == EXPECTED_DESCRIPTORS[index].environment_name);
    }
    CHECK(fixture.observed_names.back() == "GNFS_COFACTOR_TIMING_ENABLE");

    fixture.hardware_concurrency = 99;
    fixture.values[policy_index(Key::lattice_lll)] = "1";
    fixture.values[policy_index(Key::survival_filter)] = "0";
    fixture.values[policy_index(Key::brent_pollard_rho_threads)] = "1";
    fixture.values[EXPECTED_DESCRIPTORS.size()] = "0";

    const auto frozen = freeze_checked(*captured.snapshot);
    CHECK(frozen.bound_hardware_concurrency == 3);
    CHECK(frozen.sieve.lattice_lll ==
          policy_detail::DistributedSieveCanonicalLatticeReductionV1::gauss);
    CHECK(frozen.cofactor.survival_filter);
    CHECK(frozen.cofactor.brent_pollard_rho_threads == 6);
    CHECK(frozen.diagnostics.cofactor_timing_enabled);
    CHECK(fixture.hardware_reads == 1);
    CHECK(std::all_of(fixture.read_counts.begin(), fixture.read_counts.end(),
                      [](std::size_t count) { return count == 1; }));
}

void test_lattice_runtime_mapping_uses_only_frozen_policy() {
    CaptureFixture fixture;
    fixture.hardware_concurrency = 4;
    fixture.values[policy_index(Key::lattice_lll)] = "1";
    fixture.values[policy_index(Key::lattice_skew)] = "1";
    fixture.values[policy_index(Key::adaptive_lattice)] = "1";
    fixture.values[policy_index(Key::adaptive_lattice_threshold)] = "100";
    fixture.values[policy_index(Key::adaptive_lattice_max_retries)] = "2";
    fixture.values[policy_index(Key::adaptive_lattice_seed)] = "0";
    fixture.values[policy_index(Key::lattice_basis_parallel_threads)] = "8";

    const policy_detail::DistributedSieveExecutionPolicyCaptureSourcesV1 sources{
        &fixture, capture_environment_reader, capture_hardware_reader};
    const auto captured =
        policy_detail::capture_distributed_sieve_execution_policy_environment_v1(sources);
    CHECK(captured);
    CHECK(captured.snapshot.has_value());

    const auto frozen = freeze_checked(*captured.snapshot);
    const auto before = map_lattice_runtime_checked(frozen);
    CHECK(before.sieve.lattice_basis.base_method == sieve::LatticeReductionMethod::LLL);
    CHECK(before.sieve.lattice_basis.skew_enabled);
    CHECK(before.sieve.adaptive_lattice.enabled);
    CHECK(before.sieve.adaptive_lattice.density_threshold == 100);
    CHECK(before.sieve.adaptive_lattice.max_retries == 2);
    CHECK(before.sieve.adaptive_lattice.perturb_seed == 0);
    CHECK(before.lattice_basis_parallel_threads == 8);

    const auto read_counts_after_capture = fixture.read_counts;
    const std::size_t hardware_reads_after_capture = fixture.hardware_reads;

    fixture.hardware_concurrency = 99;
    fixture.values[policy_index(Key::lattice_lll)] = "0";
    fixture.values[policy_index(Key::lattice_skew)] = "0";
    fixture.values[policy_index(Key::adaptive_lattice)] = "0";
    fixture.values[policy_index(Key::adaptive_lattice_threshold)] = "0.001";
    fixture.values[policy_index(Key::adaptive_lattice_max_retries)] = "0";
    fixture.values[policy_index(Key::adaptive_lattice_seed)] = "999";
    fixture.values[policy_index(Key::lattice_basis_parallel_threads)] = "1";

    ScopedEnvironmentVariable ambient_lll("GNFS_LATTICE_LLL", "0");
    ScopedEnvironmentVariable ambient_skew("GNFS_LATTICE_SKEW", "0");
    ScopedEnvironmentVariable ambient_adaptive("GNFS_ADAPTIVE_LATTICE", "0");
    ScopedEnvironmentVariable ambient_threshold("GNFS_ADAPTIVE_LATTICE_THRESHOLD", "0.001");
    ScopedEnvironmentVariable ambient_retries("GNFS_ADAPTIVE_LATTICE_MAX_RETRIES", "0");
    ScopedEnvironmentVariable ambient_seed("GNFS_ADAPTIVE_LATTICE_SEED", "999");
    ScopedEnvironmentVariable ambient_parallel_threads("GNFS_LATTICE_BASIS_PARALLEL_THREADS", "1");

    const auto after = map_lattice_runtime_checked(frozen);
    CHECK(same_lattice_runtime_config(before, after));
    CHECK(fixture.read_counts == read_counts_after_capture);
    CHECK(fixture.hardware_reads == hardware_reads_after_capture);

    const gnfs::core::Integer n("1000036000099");
    const auto polynomial = gnfs::polynomial::BaseMSelector::select(n, 3);
    CHECK(polynomial.success);
    const auto context = gnfs::polynomial::BaseMSelector::create_context(n, polynomial);
    CHECK(context.verify());

    gnfs::factor_base::FactorBaseBuilder::Options factor_base_options;
    factor_base_options.rational_bound = 3000;
    factor_base_options.algebraic_bound = 3000;
    factor_base_options.log_scale = 16;
    factor_base_options.parallel = false;
    const auto factor_base =
        gnfs::factor_base::FactorBaseBuilder::build(context, factor_base_options);

    sieve::SieveParams sieve_params;
    sieve_params.log_scale = 16;
    sieve_params.rational_threshold = 55;
    sieve_params.algebraic_threshold = 55;
    const sieve::SieveRegion region{-400, 399, 1, 80};

    sieve::SpecialQRange special_q_range;
    special_q_range.min_q = 1000;
    special_q_range.max_q = 3000;
    sieve::SpecialQGenerator generator(factor_base, special_q_range);
    std::vector<sieve::SpecialQ> special_qs;
    while (special_qs.size() < 8 && generator.has_next()) {
        const auto special_q = generator.next();
        CHECK(special_q.has_value());
        special_qs.push_back(*special_q);
    }
    CHECK(special_qs.size() == 8);

    sieve::LatticeSieve mapped_sieve(context, factor_base, sieve_params, after.sieve);
    mapped_sieve.set_region(region);
    sieve::AdaptiveBasisManager same_adaptive_policy(after.sieve.adaptive_lattice);
    sieve::LatticeSieve legacy_basis_sieve(context, factor_base, sieve_params);
    legacy_basis_sieve.set_region(region);
    legacy_basis_sieve.set_adaptive_manager(&same_adaptive_policy);

    bool observed_basis_sensitive_difference = false;
    std::size_t candidate_total = 0;
    for (const auto& special_q : special_qs) {
        const auto mapped_result = mapped_sieve.sieve_special_q(special_q);
        const auto legacy_result = legacy_basis_sieve.sieve_special_q(special_q);
        candidate_total += mapped_result.candidates.size();
        if (!same_sieve_result(mapped_result, legacy_result)) {
            observed_basis_sensitive_difference = true;
        }
    }
    CHECK(candidate_total > 0);
    CHECK(observed_basis_sensitive_difference);
    CHECK(mapped_sieve.adaptive_manager().config().enabled == after.sieve.adaptive_lattice.enabled);
    CHECK(mapped_sieve.adaptive_manager().config().density_threshold ==
          after.sieve.adaptive_lattice.density_threshold);
    CHECK(mapped_sieve.adaptive_manager().config().max_retries ==
          after.sieve.adaptive_lattice.max_retries);
    CHECK(mapped_sieve.adaptive_manager().config().perturb_seed ==
          after.sieve.adaptive_lattice.perturb_seed);
    CHECK(mapped_sieve.adaptive_manager().stats().snapshot().special_qs_processed ==
          special_qs.size());
}

void test_bound_work_derives_every_small_runtime_input() {
    const auto frozen = freeze_checked(unset_snapshot());
    auto identity = make_runtime_identity(frozen);
    identity.distributed.sq_cap_per_worker = 12;
    identity.distributed.relation_cap_per_worker = 34;
    identity.distributed.max_worker_attempts = 64;
    identity.distributed.max_merge_build_attempts = 63;
    identity.distributed.max_consumption_attempts = 62;
    identity.original_sq_bounds = {0, 2, 10, 11};
    identity.effective_sq_bounds = {1, 2, 0, std::numeric_limits<std::uint32_t>::max()};
    identity.distributed.chunks = {{0, 1, 2, "chunk_0"}};
    CHECK(sieve::validate_distributed_sieve_work_identity(identity));

    const auto polynomial = make_live_polynomial();
    const auto factor_base = make_live_factor_base();
    const auto bound = bind_work_checked(identity, frozen, polynomial, factor_base);

    const auto digest = sieve::distributed_sieve_work_digest(identity);
    CHECK(digest);
    CHECK(bound.work_digest == *digest.digest);
    CHECK(bound.sieve_parameters.log_scale == 16);
    CHECK(bound.sieve_parameters.rational_threshold == 50);
    CHECK(bound.sieve_parameters.algebraic_threshold == 51);
    CHECK(bound.sieve_parameters.large_prime_bound == 0);
    CHECK(bound.sieve_parameters.enable_2lp);
    CHECK(!bound.sieve_parameters.enable_3lp);
    CHECK(bound.sieve_region.i_min == -100);
    CHECK(bound.sieve_region.i_max == 100);
    CHECK(bound.sieve_region.j_min == 1);
    CHECK(bound.sieve_region.j_max == 50);

    CHECK(bound.cofactor.cofactorizer.large_prime_bound == 10'000);
    CHECK(bound.cofactor.cofactorizer.allow_1lp);
    CHECK(bound.cofactor.cofactorizer.allow_2lp);
    CHECK(!bound.cofactor.cofactorizer.allow_3lp);
    CHECK(bound.cofactor.cofactorizer.max_factorization_attempts ==
          sieve::DISTRIBUTED_SIEVE_SEMANTIC_DEFAULT_MAX_FACTORIZATION_ATTEMPTS_V2);
    const auto semantic_root = sieve::distributed_sieve_semantic_seed_root_v2(identity);
    CHECK(semantic_root);
    CHECK(bound.cofactor.seed_provider.semantic_seed_root() == *semantic_root.digest);
    CHECK(same_lattice_runtime_config(bound.lattice, map_lattice_runtime_checked(frozen)));

    CHECK(bound.original_sq_range.start_index == identity.original_sq_bounds.start_index);
    CHECK(bound.original_sq_range.end_index == identity.original_sq_bounds.end_index);
    CHECK(bound.original_sq_range.min_q == identity.original_sq_bounds.min_q);
    CHECK(bound.original_sq_range.max_q == identity.original_sq_bounds.max_q);
    CHECK(bound.effective_sq_range.start_index == identity.effective_sq_bounds.start_index);
    CHECK(bound.effective_sq_range.end_index == identity.effective_sq_bounds.end_index);
    CHECK(bound.effective_sq_range.min_q == 0);
    CHECK(bound.effective_sq_range.max_q == std::numeric_limits<std::uint32_t>::max());

    CHECK(bound.worker_count == 1);
    CHECK(bound.chunks == identity.distributed.chunks);
    CHECK(bound.sq_cap_per_worker == 12);
    CHECK(bound.relation_cap_per_worker == 34);
    CHECK(bound.max_worker_attempts == 64);
    CHECK(bound.max_merge_build_attempts == 63);
    CHECK(bound.max_consumption_attempts == 62);
    CHECK(bound.semantic_versions.relation_serialization_version == 1);
    CHECK(bound.semantic_versions.ooc_format_version ==
          gnfs::relation::OOCRelationStoreFormat::FORMAT_VERSION_V3);
    CHECK(bound.semantic_versions.merge_policy_version == 1);
    CHECK(same_canonical_policy(bound.frozen_policy.canonical, frozen.canonical));
}

void test_bound_work_enforces_lattice_sieve_region_bounds() {
    const auto frozen = freeze_checked(unset_snapshot());
    const auto polynomial = make_live_polynomial();
    const auto factor_base = make_live_factor_base();

    const auto expect_region_rejected = [&](const sieve::SieveRegionWorkIdentityV1& region) {
        auto identity = make_runtime_identity(frozen);
        identity.region = region;
        CHECK(sieve::validate_distributed_sieve_work_identity(identity));
        expect_work_binding_rejected(identity, frozen, polynomial, factor_base);
    };

    // CompactSmallPrime stores values in [0, p) for p < width. Thus 32768 is
    // the exact accepted threshold and 32769 is the first unsafe width.
    {
        auto identity = make_runtime_identity(frozen);
        identity.region = {-16'384, 16'383, 1, 1};
        CHECK(sieve::validate_distributed_sieve_work_identity(identity));
        const auto bound = bind_work_checked(identity, frozen, polynomial, factor_base);
        CHECK(bound.sieve_region.i_width() == 32'768);
        CHECK(bound.sieve_region.size() == 32'768);

        sieve::LatticeSieve lattice_sieve(polynomial, factor_base, bound.sieve_parameters,
                                          bound.lattice.sieve);
        lattice_sieve.set_region(bound.sieve_region);
        lattice_sieve.set_max_threads(1);
        const auto result = lattice_sieve.sieve_special_q({7, 1, 0});
        CHECK(result.sieved_positions == 32'768);
    }
    expect_region_rejected({-16'384, 16'384, 1, 1});

    // SieveRegion::j_height() and row offsets are int32_t.
    expect_region_rejected({0, 0, std::numeric_limits<std::int32_t>::min(), 0});

    // Inclusive row loops increment once after the last row, so INT32_MAX
    // cannot itself be a j endpoint even for a one-row region.
    expect_region_rejected(
        {0, 0, std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max()});

    // estimate_initial_log() forms j_min + j_max in int32_t.
    expect_region_rejected({0, 0, std::numeric_limits<std::int32_t>::max() - 2,
                            std::numeric_limits<std::int32_t>::max() - 1});
    expect_region_rejected({0, 0, std::numeric_limits<std::int32_t>::min(),
                            std::numeric_limits<std::int32_t>::min() + 1});

    // Exercise the widest/tallest representable product without allocating
    // it. On a platform whose vector limit is smaller, the same identity must
    // instead fail closed at bind time.
    constexpr std::int64_t maximum_height = std::numeric_limits<std::int32_t>::max();
    constexpr std::int64_t centered_j_min = -(maximum_height / 2) - 1;
    constexpr std::int64_t centered_j_max = centered_j_min + maximum_height - 1;
    auto maximum_area_identity = make_runtime_identity(frozen);
    maximum_area_identity.region = {-16'384, 16'383, centered_j_min, centered_j_max};
    CHECK(sieve::validate_distributed_sieve_work_identity(maximum_area_identity));

    constexpr std::uintmax_t maximum_region_area =
        std::uintmax_t{32'768} * static_cast<std::uintmax_t>(maximum_height);
    const auto maximum_vector_area =
        static_cast<std::uintmax_t>(std::vector<std::uint16_t>{}.max_size());
    if (maximum_region_area <= maximum_vector_area &&
        maximum_region_area <=
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        const auto bound =
            bind_work_checked(maximum_area_identity, frozen, polynomial, factor_base);
        CHECK(static_cast<std::uintmax_t>(bound.sieve_region.i_width()) *
                  static_cast<std::uintmax_t>(bound.sieve_region.j_height()) ==
              maximum_region_area);
    } else {
        expect_work_binding_rejected(maximum_area_identity, frozen, polynomial, factor_base);
    }

    // Where the platform allocation bound is reachable inside the region
    // dimension limits, cross it by exactly one row and require rejection.
    const std::uintmax_t first_oversized_height = maximum_vector_area / 32'768U + 1U;
    if (first_oversized_height <=
        static_cast<std::uintmax_t>(std::numeric_limits<std::int32_t>::max())) {
        const auto j_min = -static_cast<std::int64_t>(first_oversized_height / 2U);
        const auto j_max = j_min + static_cast<std::int64_t>(first_oversized_height) - 1;
        expect_region_rejected({-16'384, 16'383, j_min, j_max});
    }
}

void test_bound_work_rejects_every_live_polynomial_drift() {
    const auto frozen = freeze_checked(unset_snapshot());
    const auto identity = make_runtime_identity(frozen);
    const auto factor_base = make_live_factor_base();

    {
        const auto live = make_live_polynomial("1000036000101");
        expect_work_binding_rejected(identity, frozen, live, factor_base);
    }
    {
        const auto live = make_live_polynomial("1000036000099", "10003");
        expect_work_binding_rejected(identity, frozen, live, factor_base);
    }
    {
        const auto live = make_live_polynomial("1000036000099", "10001", {"-7", "3", "1"});
        expect_work_binding_rejected(identity, frozen, live, factor_base);
    }
    {
        const auto live = make_live_polynomial("1000036000099", "10001", {"-5", "3", "1", "1"});
        expect_work_binding_rejected(identity, frozen, live, factor_base);
    }
    {
        // PolynomialContext retains zero storage above its lowered live
        // degree. V1 binds that storage exactly even though the V2 seed root
        // deliberately normalizes it.
        const auto live = make_live_polynomial("1000036000099", "10001", {"-5", "3", "1", "0"});
        CHECK(live.degree() == identity.polynomial.degree);
        expect_work_binding_rejected(identity, frozen, live, factor_base);
    }
    {
        const auto live = make_live_polynomial("1000036000099", "10001", {"-5", "3", "1"}, 1.5);
        expect_work_binding_rejected(identity, frozen, live, factor_base);
    }
}

void test_bound_work_rejects_every_live_factor_base_drift() {
    const auto frozen = freeze_checked(unset_snapshot());
    const auto identity = make_runtime_identity(frozen);
    const auto polynomial = make_live_polynomial();

    const auto reject = [&](const LiveFactorBaseSpec& spec) {
        const auto live = make_live_factor_base(spec);
        expect_work_binding_rejected(identity, frozen, polynomial, live);
    };

    {
        LiveFactorBaseSpec spec;
        ++spec.params.rational_bound;
        reject(spec);
    }
    {
        LiveFactorBaseSpec spec;
        ++spec.params.algebraic_bound;
        reject(spec);
    }
    {
        LiveFactorBaseSpec spec;
        ++spec.params.large_prime_bound;
        reject(spec);
    }
    {
        LiveFactorBaseSpec spec;
        --spec.params.log_scale;
        reject(spec);
    }
    {
        LiveFactorBaseSpec spec;
        ++spec.rational[0].log_p;
        reject(spec);
    }
    {
        LiveFactorBaseSpec spec;
        std::swap(spec.rational[0], spec.rational[1]);
        reject(spec);
    }
    {
        LiveFactorBaseSpec spec;
        spec.rational.push_back({7, 44});
        reject(spec);
    }
    {
        LiveFactorBaseSpec spec;
        ++spec.algebraic[0].r;
        reject(spec);
    }
    {
        LiveFactorBaseSpec spec;
        ++spec.algebraic[0].log_p;
        reject(spec);
    }
    {
        LiveFactorBaseSpec spec;
        ++spec.algebraic[1].degree;
        reject(spec);
    }
    {
        LiveFactorBaseSpec spec;
        std::swap(spec.algebraic[0], spec.algebraic[1]);
        reject(spec);
    }
    {
        LiveFactorBaseSpec spec;
        spec.algebraic.push_back({211, 2, 61, 1});
        reject(spec);
    }
    {
        LiveFactorBaseSpec spec;
        spec.sieve_algebraic_count = 1;
        reject(spec);
    }

    // A raw zero prefix is legal in V1 only when every algebraic entry lies
    // beyond the algebraic bound. FactorBase interprets its raw zero as "all
    // entries", so the binder must reject instead of silently equating the two
    // representations.
    auto zero_prefix_identity = identity;
    zero_prefix_identity.factor_base.algebraic_bound = 5;
    zero_prefix_identity.factor_base.sieve_algebraic_count = 0;
    CHECK(sieve::validate_distributed_sieve_work_identity(zero_prefix_identity));
    LiveFactorBaseSpec zero_prefix_spec;
    zero_prefix_spec.params.algebraic_bound = 5;
    zero_prefix_spec.sieve_algebraic_count = 0;
    const auto zero_prefix_live = make_live_factor_base(zero_prefix_spec);
    CHECK(zero_prefix_live.sieve_algebraic_count() == zero_prefix_live.algebraic_count());
    expect_work_binding_rejected(zero_prefix_identity, frozen, polynomial, zero_prefix_live);
}

void test_bound_work_rejects_policy_version_and_mapper_drift() {
    const auto baseline_frozen = freeze_checked(unset_snapshot());
    const auto baseline_identity = make_runtime_identity(baseline_frozen);
    const auto polynomial = make_live_polynomial();
    const auto factor_base = make_live_factor_base();

    auto canonical_split_brain = baseline_identity;
    canonical_split_brain.execution_policy.settings[policy_index(Key::lattice_skew)]
        .canonical_bits = 1;
    CHECK(sieve::validate_distributed_sieve_work_identity(canonical_split_brain));
    expect_work_binding_rejected(canonical_split_brain, baseline_frozen, polynomial, factor_base);

    auto typed_split_brain = baseline_frozen;
    typed_split_brain.sieve.lattice_skew = true;
    expect_work_binding_rejected(baseline_identity, typed_split_brain, polynomial, factor_base);

    constexpr std::array version_members{
        &sieve::WorkSemanticVersionsV1::relation_serialization_version,
        &sieve::WorkSemanticVersionsV1::ooc_format_version,
        &sieve::WorkSemanticVersionsV1::digest_version,
        &sieve::WorkSemanticVersionsV1::handoff_version,
        &sieve::WorkSemanticVersionsV1::retry_policy_version,
        &sieve::WorkSemanticVersionsV1::chunking_version,
        &sieve::WorkSemanticVersionsV1::completion_version,
        &sieve::WorkSemanticVersionsV1::deduplication_version,
        &sieve::WorkSemanticVersionsV1::merge_policy_version,
    };
    for (const auto member : version_members) {
        auto changed = baseline_identity;
        ++(changed.semantic_versions.*member);
        CHECK(sieve::validate_distributed_sieve_work_identity(changed));
        expect_work_binding_rejected(changed, baseline_frozen, polynomial, factor_base);
    }

    {
        auto snapshot = unset_snapshot();
        set_raw(snapshot, Key::survival_filter, "1");
        set_raw(snapshot, Key::survival_threshold, "0.25");
        const auto frozen = freeze_checked(snapshot);
        const auto identity = make_runtime_identity(frozen);
        expect_work_binding_rejected(identity, frozen, polynomial, factor_base);
    }
    for (const auto key : {Key::ecm_sigma_pool_size, Key::ecm_curve_pool}) {
        auto snapshot = unset_snapshot();
        set_raw(snapshot, key, "8");
        const auto frozen = freeze_checked(snapshot);
        const auto identity = make_runtime_identity(frozen);
        expect_work_binding_rejected(identity, frozen, polynomial, factor_base);
    }

    // Conservative settings are retained in the bound object even before a
    // launch consumer exists; they are not normalized away or re-read.
    auto parallel_snapshot = unset_snapshot();
    set_raw(parallel_snapshot, Key::lattice_basis_parallel_threads, "4");
    const auto parallel_frozen = freeze_checked(parallel_snapshot);
    const auto parallel_identity = make_runtime_identity(parallel_frozen);
    const auto parallel =
        bind_work_checked(parallel_identity, parallel_frozen, polynomial, factor_base);
    CHECK(parallel.lattice.lattice_basis_parallel_threads == 4);
    CHECK(parallel.frozen_policy.sieve.lattice_basis_parallel_threads == 4);
}

void test_diagnostics_are_noncanonical_and_consistency_is_closed() {
    auto off_snapshot = unset_snapshot();
    off_snapshot.cofactor_timing = "0";
    auto on_snapshot = off_snapshot;
    on_snapshot.cofactor_timing = "1";

    const auto off = freeze_checked(off_snapshot);
    const auto on = freeze_checked(on_snapshot);
    CHECK(!off.diagnostics.cofactor_timing_enabled);
    CHECK(on.diagnostics.cofactor_timing_enabled);
    CHECK(same_canonical_policy(off.canonical, on.canonical));
    CHECK(off.sieve == on.sieve);
    CHECK(off.cofactor == on.cofactor);

    auto drifted = on;
    drifted.sieve.lattice_skew = !drifted.sieve.lattice_skew;
    CHECK(!policy_detail::validate_distributed_sieve_frozen_execution_policy_v1(drifted));
}

} // namespace

int main() {
    try {
        test_descriptor_inventory();
        test_all_unset_exact_policy();
        test_rehydrate_decodes_every_canonical_setting();
        test_rehydrate_derives_minimum_host_witness_without_ambient_inputs();
        test_rehydrate_rejects_invalid_canonical_shapes_and_combinations();
        test_lattice_reduction_and_boolean_normalization();
        test_floating_and_adaptive_integer_normalization();
        test_lattice_runtime_config_exact_mapping();
        test_lattice_runtime_config_rejects_invalid_frozen_object();
        test_cofactor_runtime_exact_mapping_and_seed_root_binding();
        test_cofactor_runtime_topology_and_conservative_policy_invariance();
        test_cofactor_runtime_rejects_identity_policy_split_brain();
        test_ecm_enable_degree_invariant();
        test_cofactor_integer_normalization();
        test_modes_bucket_intent_and_no_tiny_semantics();
        test_tile_and_thread_boundaries();
        test_frozen_policy_rejects_host_bound_and_ecm_drift();
        test_capture_is_single_read_and_owned();
        test_lattice_runtime_mapping_uses_only_frozen_policy();
        test_bound_work_derives_every_small_runtime_input();
        test_bound_work_enforces_lattice_sieve_region_bounds();
        test_bound_work_rejects_every_live_polynomial_drift();
        test_bound_work_rejects_every_live_factor_base_drift();
        test_bound_work_rejects_policy_version_and_mapper_drift();
        test_diagnostics_are_noncanonical_and_consistency_is_closed();
        std::cout << "distributed sieve execution-policy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
