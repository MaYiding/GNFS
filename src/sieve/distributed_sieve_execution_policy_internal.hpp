#pragma once

// Source-private frozen execution-policy boundary for durable distributed
// sieve workers. This file is intentionally not installed as public API.

#include <gnfs/sieve/distributed_sieve_protocol.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace gnfs::sieve::distributed_sieve_execution_policy_detail {

inline constexpr std::size_t DISTRIBUTED_SIEVE_EXECUTION_POLICY_DESCRIPTOR_COUNT_V1 =
    static_cast<std::size_t>(DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1) + 1U;

enum class DistributedSieveExecutionPolicyClassificationV1 : std::uint8_t {
    semantic = 1,
    conservative = 2,
    diagnostic = 3,
};

enum class DistributedSieveCanonicalLatticeReductionV1 : std::uint8_t {
    gauss = 1,
    lll = 2,
};

enum class DistributedSieveCanonicalTernaryModeV1 : std::uint8_t {
    automatic = 1,
    force_off = 2,
    force_on = 3,
};

struct DistributedSieveExecutionPolicyDescriptorV1 final {
    std::optional<ExecutionPolicyKeyV1> key;
    ExecutionPolicyScalarKindV1 kind = ExecutionPolicyScalarKindV1::boolean;
    DistributedSieveExecutionPolicyClassificationV1 classification =
        DistributedSieveExecutionPolicyClassificationV1::semantic;
    std::string_view environment_name;
    std::uint64_t default_canonical_bits = 0;

    [[nodiscard]] friend constexpr bool
    operator==(const DistributedSieveExecutionPolicyDescriptorV1&,
               const DistributedSieveExecutionPolicyDescriptorV1&) noexcept = default;
};

[[nodiscard]] std::span<const DistributedSieveExecutionPolicyDescriptorV1>
distributed_sieve_execution_policy_descriptors_v1() noexcept;

struct DistributedSieveExecutionPolicyEnvironmentSnapshotV1 final {
    std::array<std::optional<std::string>, DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1>
        canonical_values;
    std::optional<std::string> cofactor_timing;
    std::uint32_t hardware_concurrency = 0;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveExecutionPolicyEnvironmentSnapshotV1&,
               const DistributedSieveExecutionPolicyEnvironmentSnapshotV1&) = default;
};

using DistributedSieveExecutionPolicyEnvironmentReaderV1 =
    std::optional<std::string> (*)(const void* context, std::string_view name);
using DistributedSieveExecutionPolicyHardwareConcurrencyReaderV1 =
    std::uint32_t (*)(const void* context) noexcept;

struct DistributedSieveExecutionPolicyCaptureSourcesV1 final {
    const void* context = nullptr;
    DistributedSieveExecutionPolicyEnvironmentReaderV1 environment_reader = nullptr;
    DistributedSieveExecutionPolicyHardwareConcurrencyReaderV1 hardware_concurrency_reader =
        nullptr;
};

struct DistributedSieveExecutionPolicyCaptureResultV1 final {
    std::optional<DistributedSieveExecutionPolicyEnvironmentSnapshotV1> snapshot;
    DistributedSieveProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return snapshot.has_value() && static_cast<bool>(status);
    }
};

[[nodiscard]] DistributedSieveExecutionPolicyCaptureResultV1
capture_distributed_sieve_execution_policy_environment_v1(
    const DistributedSieveExecutionPolicyCaptureSourcesV1& sources) noexcept;

[[nodiscard]] DistributedSieveExecutionPolicyCaptureResultV1
capture_distributed_sieve_execution_policy_environment_v1() noexcept;

struct DistributedSieveFrozenSievePolicyV1 final {
    DistributedSieveCanonicalLatticeReductionV1 lattice_lll =
        DistributedSieveCanonicalLatticeReductionV1::lll;
    bool lattice_skew = false;
    bool adaptive_lattice = false;
    double adaptive_lattice_threshold = 0.5;
    std::uint32_t adaptive_lattice_max_retries = 2;
    std::uint64_t adaptive_lattice_seed = 0;
    std::uint32_t lattice_basis_parallel_threads = 1;
    DistributedSieveCanonicalTernaryModeV1 lattice_coords_simd =
        DistributedSieveCanonicalTernaryModeV1::automatic;
    std::uint32_t sieve_apply_tile_threads = 1;
    DistributedSieveCanonicalTernaryModeV1 bucket_prefetch =
        DistributedSieveCanonicalTernaryModeV1::automatic;
    std::uint32_t sieve_ecore_threads = 0;
    bool sieve_no_tiny_simd = false;
    std::uint32_t sieve_norm_tile_bits = 0;
    std::uint32_t sieve_region_tile_bits = 0;
    DistributedSieveCanonicalTernaryModeV1 sieve_saturated_sub_simd =
        DistributedSieveCanonicalTernaryModeV1::automatic;
    DistributedSieveCanonicalTernaryModeV1 sieve_count_above_threshold_simd =
        DistributedSieveCanonicalTernaryModeV1::automatic;

    [[nodiscard]] friend constexpr bool
    operator==(const DistributedSieveFrozenSievePolicyV1&,
               const DistributedSieveFrozenSievePolicyV1&) noexcept = default;
};

struct DistributedSieveFrozenCofactorPolicyV1 final {
    bool survival_filter = false;
    double survival_threshold = 0.0;
    bool cofactor_brent = false;
    bool ecm_brent_suyama = false;
    std::uint32_t ecm_bs_degree = 0;
    std::uint32_t ecm_sigma_pool_size = 0;
    std::uint32_t ecm_curve_pool = 0;
    bool ecm_batch_inv = false;
    std::uint32_t cofactor_batch_size = 1;
    std::uint32_t brent_pollard_rho_threads = 1;
    std::uint32_t ecm_b1_cache_size = 0;
    std::uint32_t ecm_stage1_parallel_threads = 1;
    std::uint32_t ecm_stage2_parallel = 1;
    std::uint32_t cofactor_result_cache_size = 0;
    DistributedSieveCanonicalTernaryModeV1 trial_div_simd =
        DistributedSieveCanonicalTernaryModeV1::automatic;

    [[nodiscard]] friend constexpr bool
    operator==(const DistributedSieveFrozenCofactorPolicyV1&,
               const DistributedSieveFrozenCofactorPolicyV1&) noexcept = default;
};

struct DistributedSieveFrozenDiagnosticsV1 final {
    bool cofactor_timing_enabled = false;

    [[nodiscard]] friend constexpr bool
    operator==(const DistributedSieveFrozenDiagnosticsV1&,
               const DistributedSieveFrozenDiagnosticsV1&) noexcept = default;
};

struct DistributedSieveFrozenExecutionPolicyV1 final {
    DistributedSieveExecutionPolicyV1 canonical;
    DistributedSieveFrozenSievePolicyV1 sieve;
    DistributedSieveFrozenCofactorPolicyV1 cofactor;
    DistributedSieveFrozenDiagnosticsV1 diagnostics;
    /// Effective host parallelism used to bind every canonical thread-count
    /// setting. A captured zero is normalized to four.
    std::uint32_t bound_hardware_concurrency = 4;
};

struct DistributedSieveExecutionPolicyProjectionResultV1 final {
    std::optional<DistributedSieveExecutionPolicyV1> policy;
    DistributedSieveProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return policy.has_value() && static_cast<bool>(status);
    }
};

[[nodiscard]] DistributedSieveExecutionPolicyProjectionResultV1
project_distributed_sieve_execution_policy_v1(
    const DistributedSieveFrozenSievePolicyV1& sieve,
    const DistributedSieveFrozenCofactorPolicyV1& cofactor) noexcept;

[[nodiscard]] DistributedSieveProtocolStatus validate_distributed_sieve_frozen_execution_policy_v1(
    const DistributedSieveFrozenExecutionPolicyV1& policy) noexcept;

struct DistributedSieveExecutionPolicyFreezeResultV1 final {
    std::optional<DistributedSieveFrozenExecutionPolicyV1> policy;
    DistributedSieveProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return policy.has_value() && static_cast<bool>(status);
    }
};

[[nodiscard]] DistributedSieveExecutionPolicyFreezeResultV1
freeze_distributed_sieve_execution_policy_v1(
    const DistributedSieveExecutionPolicyEnvironmentSnapshotV1& snapshot) noexcept;

} // namespace gnfs::sieve::distributed_sieve_execution_policy_detail
