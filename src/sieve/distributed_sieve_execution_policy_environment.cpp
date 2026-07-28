#include "distributed_sieve_execution_policy_internal.hpp"

#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace gnfs::sieve::distributed_sieve_execution_policy_detail {
namespace {

[[nodiscard]] std::optional<std::string> owned_environment_value(const char* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

[[nodiscard]] constexpr DistributedSieveProtocolStatus resource_exhausted() noexcept {
    return {DistributedSieveProtocolError::resource_exhausted, DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET,
            DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX};
}

} // namespace

DistributedSieveExecutionPolicyCaptureResultV1
capture_distributed_sieve_execution_policy_environment_v1() noexcept {
    try {
        DistributedSieveExecutionPolicyEnvironmentSnapshotV1 snapshot;
        snapshot.canonical_values = {
            owned_environment_value(std::getenv("GNFS_LATTICE_LLL")),
            owned_environment_value(std::getenv("GNFS_LATTICE_SKEW")),
            owned_environment_value(std::getenv("GNFS_ADAPTIVE_LATTICE")),
            owned_environment_value(std::getenv("GNFS_ADAPTIVE_LATTICE_THRESHOLD")),
            owned_environment_value(std::getenv("GNFS_ADAPTIVE_LATTICE_MAX_RETRIES")),
            owned_environment_value(std::getenv("GNFS_ADAPTIVE_LATTICE_SEED")),
            owned_environment_value(std::getenv("GNFS_SURVIVAL_FILTER")),
            owned_environment_value(std::getenv("GNFS_SURVIVAL_THRESHOLD")),
            owned_environment_value(std::getenv("GNFS_COFACTOR_BRENT")),
            owned_environment_value(std::getenv("GNFS_ECM_BRENT_SUYAMA")),
            owned_environment_value(std::getenv("GNFS_ECM_BS_DEGREE")),
            owned_environment_value(std::getenv("GNFS_ECM_SIGMA_POOL_SIZE")),
            owned_environment_value(std::getenv("GNFS_ECM_CURVE_POOL")),
            owned_environment_value(std::getenv("GNFS_ECM_BATCH_INV")),
            owned_environment_value(std::getenv("GNFS_COFACTOR_BATCH_SIZE")),
            owned_environment_value(std::getenv("GNFS_BRENT_POLLARD_RHO_THREADS")),
            owned_environment_value(std::getenv("GNFS_ECM_B1_CACHE_SIZE")),
            owned_environment_value(std::getenv("GNFS_ECM_STAGE1_PARALLEL_THREADS")),
            owned_environment_value(std::getenv("GNFS_ECM_STAGE2_PARALLEL")),
            owned_environment_value(std::getenv("GNFS_COFACTOR_RESULT_CACHE_SIZE")),
            owned_environment_value(std::getenv("GNFS_TRIAL_DIV_SIMD")),
            owned_environment_value(std::getenv("GNFS_LATTICE_BASIS_PARALLEL_THREADS")),
            owned_environment_value(std::getenv("GNFS_LATTICE_COORDS_SIMD")),
            owned_environment_value(std::getenv("GNFS_SIEVE_APPLY_TILE_THREADS")),
            owned_environment_value(std::getenv("GNFS_BUCKET_PREFETCH")),
            owned_environment_value(std::getenv("GNFS_SIEVE_ECORE_THREADS")),
            owned_environment_value(std::getenv("GNFS_SIEVE_NO_TINY_SIMD")),
            owned_environment_value(std::getenv("GNFS_SIEVE_NORM_TILE_BITS")),
            owned_environment_value(std::getenv("GNFS_SIEVE_REGION_TILE_BITS")),
            owned_environment_value(std::getenv("GNFS_SIEVE_SATURATED_SUB_SIMD")),
            owned_environment_value(std::getenv("GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD")),
        };
        snapshot.cofactor_timing =
            owned_environment_value(std::getenv("GNFS_COFACTOR_TIMING_ENABLE"));
        snapshot.hardware_concurrency = std::thread::hardware_concurrency();
        return {std::move(snapshot), {}};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, resource_exhausted()};
    } catch (...) {
        return {std::nullopt, resource_exhausted()};
    }
}

} // namespace gnfs::sieve::distributed_sieve_execution_policy_detail
