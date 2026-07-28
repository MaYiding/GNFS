#include <gnfs/sieve/distributed_sieve_protocol.hpp>

#include "distributed_sieve_execution_policy_internal.hpp"

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
    expect_bits(Key::adaptive_lattice_threshold, "0", binary64_bits(0.5));
    expect_bits(Key::adaptive_lattice_threshold, "101", binary64_bits(0.5));
    expect_bits(Key::adaptive_lattice_threshold, "nan", binary64_bits(0.5));
    expect_bits(Key::adaptive_lattice_threshold, "invalid", binary64_bits(0.5));

    expect_bits(Key::survival_threshold, "0", binary64_bits(0.0));
    expect_bits(Key::survival_threshold, "1", binary64_bits(1.0));
    expect_bits(Key::survival_threshold, "0.25suffix", binary64_bits(0.25));
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
        test_lattice_reduction_and_boolean_normalization();
        test_floating_and_adaptive_integer_normalization();
        test_ecm_enable_degree_invariant();
        test_cofactor_integer_normalization();
        test_modes_bucket_intent_and_no_tiny_semantics();
        test_tile_and_thread_boundaries();
        test_frozen_policy_rejects_host_bound_and_ecm_drift();
        test_capture_is_single_read_and_owned();
        test_diagnostics_are_noncanonical_and_consistency_is_closed();
        std::cout << "distributed sieve execution-policy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
