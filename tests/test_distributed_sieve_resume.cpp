#include <gnfs/relation/ooc_cleanup_transaction.hpp>
#include <gnfs/sieve/distributed_sieve_protocol.hpp>

#include "distributed_sieve_wave_store_internal.hpp"
#include "ooc_private_handoff_cleanup_authorization_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <membership.h>
#include <sys/acl.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif

namespace {

namespace sieve = gnfs::sieve;
namespace cleanup_detail = gnfs::relation::ooc_cleanup_detail;
namespace wave_detail = gnfs::sieve::distributed_sieve_resume_detail;
using Digest = gnfs::util::Sha256Digest;
using Record = sieve::DistributedSieveProtocolRecordV1;
using Status = sieve::DistributedSieveProtocolStatus;
using ExternalCleanupAuthorizationState =
    wave_detail::DistributedSieveExternalCleanupAuthorizationState;
using PrivateLeaseRootClaim = wave_detail::DistributedSievePrivateLeaseRootClaim;
using PrivateLeaseBaseLockAt = wave_detail::DistributedSievePrivateLeaseBaseLockAt;
using PrivateLeaseReservationReceipt = wave_detail::DistributedSievePrivateLeaseReservationReceipt;

static_assert(!noexcept(sieve::distributed_sieve_record_kind(std::declval<const Record&>())));
static_assert(
    std::is_default_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding>);
static_assert(
    std::is_copy_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding>);
static_assert(
    !std::is_default_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationMintKey>);
static_assert(
    !std::is_copy_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationMintKey>);
static_assert(
    !std::is_move_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationMintKey>);
static_assert(
    !std::is_move_assignable_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationMintKey>);
static_assert(
    !std::is_default_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt>);
static_assert(
    !std::is_copy_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt>);
static_assert(
    !std::is_copy_assignable_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt>);
static_assert(std::is_nothrow_move_constructible_v<
              cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt>);
static_assert(
    !std::is_move_assignable_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt>);
static_assert(
    !std::is_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt,
                             cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding>);
static_assert(!std::is_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt,
                                       std::shared_ptr<const ExternalCleanupAuthorizationState>>);
static_assert(
    !std::is_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt,
                             cleanup_detail::OOCPrivateHandoffCleanupAuthorizationMintKey&&,
                             cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding,
                             std::shared_ptr<const ExternalCleanupAuthorizationState>>);
static_assert(!std::is_constructible_v<
              cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt,
              cleanup_detail::OOCPrivateHandoffCleanupAuthorizationMintKey&&,
              cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding,
              std::shared_ptr<const ExternalCleanupAuthorizationState>, std::uint64_t>);
static_assert(!std::is_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt,
                                       sieve::ArtifactCleanupAuthorizedV1>);
static_assert(!std::is_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt,
                                       gnfs::relation::OOCPrivateHandoffRecordV1>);
static_assert(!std::is_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt,
                                       gnfs::relation::OOCPrivateHandoffAdoptionReceipt>);
static_assert(!std::is_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt,
                                       std::filesystem::path>);
static_assert(
    !std::is_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt, Digest>);
static_assert(!std::is_default_constructible_v<ExternalCleanupAuthorizationState>);
static_assert(!std::is_copy_constructible_v<ExternalCleanupAuthorizationState>);
static_assert(!std::is_copy_assignable_v<ExternalCleanupAuthorizationState>);
static_assert(!std::is_move_constructible_v<ExternalCleanupAuthorizationState>);
static_assert(!std::is_move_assignable_v<ExternalCleanupAuthorizationState>);
static_assert(!std::is_constructible_v<ExternalCleanupAuthorizationState,
                                       sieve::ArtifactCleanupAuthorizedV1>);
static_assert(!std::is_constructible_v<ExternalCleanupAuthorizationState,
                                       gnfs::relation::OOCPrivateHandoffRecordV1>);
static_assert(!std::is_constructible_v<ExternalCleanupAuthorizationState,
                                       gnfs::relation::OOCPrivateHandoffAdoptionReceipt>);
static_assert(
    !std::is_constructible_v<ExternalCleanupAuthorizationState,
                             cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding>);
static_assert(!std::is_constructible_v<ExternalCleanupAuthorizationState, std::filesystem::path>);
static_assert(!std::is_constructible_v<ExternalCleanupAuthorizationState, Digest>);
static_assert(
    !std::is_constructible_v<ExternalCleanupAuthorizationState, std::shared_ptr<const void>>);
static_assert(!std::is_constructible_v<ExternalCleanupAuthorizationState,
                                       std::shared_ptr<const void>, std::uint64_t>);
static_assert(!std::is_default_constructible_v<wave_detail::DistributedSieveWaveStore>);
static_assert(!std::is_copy_constructible_v<wave_detail::DistributedSieveWaveStore>);
static_assert(!std::is_copy_assignable_v<wave_detail::DistributedSieveWaveStore>);
static_assert(!std::is_move_constructible_v<wave_detail::DistributedSieveWaveStore>);
static_assert(!std::is_move_assignable_v<wave_detail::DistributedSieveWaveStore>);
static_assert(std::is_final_v<PrivateLeaseRootClaim>);
static_assert(!std::is_default_constructible_v<PrivateLeaseRootClaim>);
static_assert(!std::is_copy_constructible_v<PrivateLeaseRootClaim>);
static_assert(!std::is_copy_assignable_v<PrivateLeaseRootClaim>);
static_assert(!std::is_move_constructible_v<PrivateLeaseRootClaim>);
static_assert(!std::is_move_assignable_v<PrivateLeaseRootClaim>);
static_assert(
    !std::is_constructible_v<PrivateLeaseRootClaim, wave_detail::DistributedSieveWaveStore&>);
static_assert(!std::is_constructible_v<PrivateLeaseRootClaim, std::filesystem::path>);
static_assert(!std::is_constructible_v<PrivateLeaseRootClaim, Digest>);
static_assert(!std::is_constructible_v<PrivateLeaseRootClaim, std::shared_ptr<const void>>);
static_assert(!std::is_constructible_v<PrivateLeaseRootClaim, int>);
static_assert(std::is_final_v<PrivateLeaseBaseLockAt>);
static_assert(!std::is_default_constructible_v<PrivateLeaseBaseLockAt>);
static_assert(!std::is_copy_constructible_v<PrivateLeaseBaseLockAt>);
static_assert(!std::is_copy_assignable_v<PrivateLeaseBaseLockAt>);
static_assert(!std::is_move_constructible_v<PrivateLeaseBaseLockAt>);
static_assert(!std::is_move_assignable_v<PrivateLeaseBaseLockAt>);
static_assert(!std::is_constructible_v<PrivateLeaseBaseLockAt, std::filesystem::path>);
static_assert(!std::is_constructible_v<PrivateLeaseBaseLockAt, std::string>);
static_assert(!std::is_constructible_v<PrivateLeaseBaseLockAt, int>);
static_assert(std::is_final_v<PrivateLeaseReservationReceipt>);
static_assert(!std::is_default_constructible_v<PrivateLeaseReservationReceipt>);
static_assert(!std::is_copy_constructible_v<PrivateLeaseReservationReceipt>);
static_assert(!std::is_copy_assignable_v<PrivateLeaseReservationReceipt>);
static_assert(std::is_nothrow_move_constructible_v<PrivateLeaseReservationReceipt>);
static_assert(!std::is_move_assignable_v<PrivateLeaseReservationReceipt>);
static_assert(!std::is_constructible_v<PrivateLeaseReservationReceipt, PrivateLeaseRootClaim&&>);
static_assert(!std::is_constructible_v<PrivateLeaseReservationReceipt, PrivateLeaseBaseLockAt&&>);
static_assert(!std::is_constructible_v<PrivateLeaseReservationReceipt, std::filesystem::path>);
static_assert(!std::is_constructible_v<PrivateLeaseReservationReceipt, int>);

constexpr std::array WAVE_STORE_FAULT_POINTS{
    wave_detail::DistributedSieveWaveStoreFaultPoint::RootDurable,
    wave_detail::DistributedSieveWaveStoreFaultPoint::LockDurable,
    wave_detail::DistributedSieveWaveStoreFaultPoint::ManifestPendingDurable,
    wave_detail::DistributedSieveWaveStoreFaultPoint::ManifestCanonicalPromoted,
    wave_detail::DistributedSieveWaveStoreFaultPoint::ManifestCanonicalDurable,
};
static_assert(WAVE_STORE_FAULT_POINTS.size() ==
              static_cast<std::size_t>(wave_detail::DistributedSieveWaveStoreFaultPoint::Count));
static_assert([] {
    for (std::size_t index = 0; index < WAVE_STORE_FAULT_POINTS.size(); ++index) {
        if (static_cast<std::size_t>(WAVE_STORE_FAULT_POINTS[index]) != index) {
            return false;
        }
    }
    return true;
}());

constexpr std::array PRIVATE_LEASE_BASE_LOCK_SYNC_POINTS{
    wave_detail::DistributedSievePrivateLeaseBaseLockSyncPoint::TargetInitial,
    wave_detail::DistributedSievePrivateLeaseBaseLockSyncPoint::RootDirectory,
    wave_detail::DistributedSievePrivateLeaseBaseLockSyncPoint::TargetFinal,
};
static_assert(
    PRIVATE_LEASE_BASE_LOCK_SYNC_POINTS.size() ==
    static_cast<std::size_t>(wave_detail::DistributedSievePrivateLeaseBaseLockSyncPoint::Count));
static_assert([] {
    for (std::size_t index = 0; index < PRIVATE_LEASE_BASE_LOCK_SYNC_POINTS.size(); ++index) {
        if (static_cast<std::size_t>(PRIVATE_LEASE_BASE_LOCK_SYNC_POINTS[index]) != index) {
            return false;
        }
    }
    return true;
}());

struct PrivateLeaseReservationBoundaryContract final {
    wave_detail::DistributedSievePrivateLeaseReservationBoundary boundary;
    gnfs::relation::OOCPrivateLeaseFaultPoint relation_fault_point;
};

constexpr std::array PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS{
    PrivateLeaseReservationBoundaryContract{
        .boundary = wave_detail::DistributedSievePrivateLeaseReservationBoundary::PermitAcquired,
        .relation_fault_point =
            gnfs::relation::OOCPrivateLeaseFaultPoint::ReservationPermitAcquired,
    },
    PrivateLeaseReservationBoundaryContract{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable,
        .relation_fault_point = gnfs::relation::OOCPrivateLeaseFaultPoint::ReservedPendingDurable,
    },
    PrivateLeaseReservationBoundaryContract{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::ReservedCanonicalDurable,
        .relation_fault_point = gnfs::relation::OOCPrivateLeaseFaultPoint::ReservedDurable,
    },
    PrivateLeaseReservationBoundaryContract{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable,
        .relation_fault_point = gnfs::relation::OOCPrivateLeaseFaultPoint::StagingDirectoryDurable,
    },
    PrivateLeaseReservationBoundaryContract{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::OwnerPendingDurable,
        .relation_fault_point = gnfs::relation::OOCPrivateLeaseFaultPoint::OwnerPendingDurable,
    },
    PrivateLeaseReservationBoundaryContract{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::OwnerCanonicalDurable,
        .relation_fault_point = gnfs::relation::OOCPrivateLeaseFaultPoint::OwnerDurable,
    },
    PrivateLeaseReservationBoundaryContract{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::OwnedPendingDurable,
        .relation_fault_point = gnfs::relation::OOCPrivateLeaseFaultPoint::OwnedPendingDurable,
    },
    PrivateLeaseReservationBoundaryContract{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::OwnedCanonicalDurable,
        .relation_fault_point = gnfs::relation::OOCPrivateLeaseFaultPoint::OwnedDurable,
    },
    PrivateLeaseReservationBoundaryContract{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable,
        .relation_fault_point = gnfs::relation::OOCPrivateLeaseFaultPoint::FinalRenameDurable,
    },
};

static_assert(PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS.size() ==
              wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVATION_BOUNDARIES.size());
static_assert([] {
    for (std::size_t index = 0; index < PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS.size();
         ++index) {
        if (PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS[index].boundary !=
                wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVATION_BOUNDARIES[index] ||
            static_cast<std::size_t>(
                PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS[index].boundary) != index) {
            return false;
        }
    }
    return true;
}());

struct PrivateLeaseReservationSyncFailureSite final {
    wave_detail::DistributedSievePrivateLeaseReservationBoundary boundary;
    wave_detail::DistributedSievePrivateLeaseReservationSyncPoint point;
};

constexpr std::array PRIVATE_LEASE_RESERVATION_SYNC_FAILURE_SITES{
    PrivateLeaseReservationSyncFailureSite{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable,
        .point = wave_detail::DistributedSievePrivateLeaseReservationSyncPoint::MarkerFileInitial,
    },
    PrivateLeaseReservationSyncFailureSite{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable,
        .point = wave_detail::DistributedSievePrivateLeaseReservationSyncPoint::ParentDirectory,
    },
    PrivateLeaseReservationSyncFailureSite{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable,
        .point = wave_detail::DistributedSievePrivateLeaseReservationSyncPoint::MarkerFileFinal,
    },
    PrivateLeaseReservationSyncFailureSite{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::ReservedCanonicalDurable,
        .point = wave_detail::DistributedSievePrivateLeaseReservationSyncPoint::ParentDirectory,
    },
    PrivateLeaseReservationSyncFailureSite{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable,
        .point = wave_detail::DistributedSievePrivateLeaseReservationSyncPoint::StagingDirectory,
    },
    PrivateLeaseReservationSyncFailureSite{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable,
        .point = wave_detail::DistributedSievePrivateLeaseReservationSyncPoint::ParentDirectory,
    },
    PrivateLeaseReservationSyncFailureSite{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::OwnerPendingDurable,
        .point = wave_detail::DistributedSievePrivateLeaseReservationSyncPoint::MarkerFileInitial,
    },
    PrivateLeaseReservationSyncFailureSite{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::OwnerPendingDurable,
        .point = wave_detail::DistributedSievePrivateLeaseReservationSyncPoint::ParentDirectory,
    },
    PrivateLeaseReservationSyncFailureSite{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::OwnerPendingDurable,
        .point = wave_detail::DistributedSievePrivateLeaseReservationSyncPoint::MarkerFileFinal,
    },
    PrivateLeaseReservationSyncFailureSite{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::OwnerCanonicalDurable,
        .point = wave_detail::DistributedSievePrivateLeaseReservationSyncPoint::ParentDirectory,
    },
    PrivateLeaseReservationSyncFailureSite{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::OwnedPendingDurable,
        .point = wave_detail::DistributedSievePrivateLeaseReservationSyncPoint::MarkerFileInitial,
    },
    PrivateLeaseReservationSyncFailureSite{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::OwnedPendingDurable,
        .point = wave_detail::DistributedSievePrivateLeaseReservationSyncPoint::ParentDirectory,
    },
    PrivateLeaseReservationSyncFailureSite{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::OwnedPendingDurable,
        .point = wave_detail::DistributedSievePrivateLeaseReservationSyncPoint::MarkerFileFinal,
    },
    PrivateLeaseReservationSyncFailureSite{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::OwnedCanonicalDurable,
        .point = wave_detail::DistributedSievePrivateLeaseReservationSyncPoint::ParentDirectory,
    },
    PrivateLeaseReservationSyncFailureSite{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable,
        .point = wave_detail::DistributedSievePrivateLeaseReservationSyncPoint::ParentDirectory,
    },
};

static_assert(PRIVATE_LEASE_RESERVATION_SYNC_FAILURE_SITES.size() == 15U);

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string_view expression, int line, std::string_view detail = {}) {
    std::string message = "CHECK failed at line " + std::to_string(line) + ": ";
    message.append(expression);
    if (!detail.empty()) {
        message.append(" (");
        message.append(detail);
        message.push_back(')');
    }
    throw TestFailure(message);
}

void check(bool condition, std::string_view expression, int line) {
    if (!condition) {
        fail(expression, line);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void require_ok(const Status& status, std::string_view context) {
    if (!status) {
        fail(context, __LINE__, sieve::distributed_sieve_protocol_error_name(status.error));
    }
}

void require_failed(const Status& status, std::string_view context) {
    if (status || status.error == sieve::DistributedSieveProtocolError::none) {
        fail(context, __LINE__, "operation unexpectedly succeeded");
    }
}

[[nodiscard]] Digest digest_with_seed(uint8_t seed) {
    Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        const auto value = static_cast<uint8_t>(seed + static_cast<uint8_t>(index));
        digest.bytes[index] = static_cast<std::byte>(value);
    }
    return digest;
}

void perturb_digest(Digest& digest) {
    digest.bytes.front() ^= std::byte{0x80};
}

[[nodiscard]] sieve::WaveIdV1 wave_id_with_seed(uint8_t seed) {
    sieve::WaveIdV1 wave_id;
    for (std::size_t index = 0; index < wave_id.bytes.size(); ++index) {
        const auto value = static_cast<uint8_t>(seed + static_cast<uint8_t>(index));
        wave_id.bytes[index] = static_cast<std::byte>(value);
    }
    return wave_id;
}

[[nodiscard]] sieve::NativeIdentityV1 native_identity(uint64_t seed) {
    sieve::NativeIdentityV1 identity;
    identity.volume = seed;
    identity.object = seed + 1;
    identity.generation = seed + 2;
    return identity;
}

[[nodiscard]] constexpr sieve::LeaseIdV1 lease_id_with_seed(uint64_t seed) noexcept {
    return sieve::LeaseIdV1{{seed + 1, seed + 2}};
}

[[nodiscard]] sieve::LeaseIdentityV1 lease_identity(uint64_t seed, std::string stem) {
    sieve::LeaseIdentityV1 lease;
    lease.lease_id = lease_id_with_seed(seed);
    lease.owner_marker = native_identity(seed + 10);
    lease.directory = native_identity(seed + 20);
    lease.relative_stem = std::move(stem);
    return lease;
}

[[nodiscard]] sieve::CorpusArtifactV1 corpus_artifact(uint64_t seed, uint64_t relation_count) {
    sieve::CorpusArtifactV1 artifact;
    artifact.descriptor.format_version = 1;
    artifact.descriptor.store_id = seed + 1;
    artifact.descriptor.generation = seed + 2;
    artifact.descriptor.relation_count = relation_count;
    artifact.descriptor.data_end = 4096 + relation_count;
    artifact.index_file.identity = native_identity(seed + 30);
    artifact.index_file.extent = 128 + relation_count * 16;
    artifact.data_file.identity = native_identity(seed + 40);
    artifact.data_file.extent = artifact.descriptor.data_end;
    artifact.sequence_receipt.relation_count = relation_count;
    artifact.sequence_receipt.low = seed + 50;
    artifact.sequence_receipt.high = seed + 51;
    artifact.corpus_sha256 = digest_with_seed(static_cast<uint8_t>(seed));
    return artifact;
}

template <typename T> [[nodiscard]] T seal_value(T value) {
    Record record = std::move(value);
    require_ok(sieve::seal_distributed_sieve_record(record), "seal record");
    return std::get<T>(std::move(record));
}

template <typename T>
[[nodiscard]] Status validate_value(const T& value, bool verify_self_digest = true) {
    return sieve::validate_distributed_sieve_record(Record{value}, verify_self_digest);
}

template <typename T> [[nodiscard]] T reseal(T value) {
    value.self_digest = {};
    return seal_value(std::move(value));
}

template <typename T> void require_reseal_failed(T value, std::string_view context) {
    value.self_digest = {};
    Record record = std::move(value);
    require_failed(sieve::seal_distributed_sieve_record(record), context);
}

[[nodiscard]] std::vector<std::byte> encode_or_fail(const Record& record) {
    const auto encoded = sieve::encode_distributed_sieve_record(record);
    if (!encoded) {
        fail("encode_distributed_sieve_record", __LINE__,
             sieve::distributed_sieve_protocol_error_name(encoded.status.error));
    }
    return *encoded.bytes;
}

[[nodiscard]] Digest record_digest_or_fail(const Record& record) {
    const auto digest = sieve::distributed_sieve_record_digest(record);
    if (!digest) {
        fail("distributed_sieve_record_digest", __LINE__,
             sieve::distributed_sieve_protocol_error_name(digest.status.error));
    }
    return *digest.digest;
}

[[nodiscard]] const Digest& self_digest(const Record& record) {
    return std::visit([](const auto& value) -> const Digest& { return value.self_digest; }, record);
}

[[nodiscard]] constexpr uint64_t binary64_bits(double value) noexcept {
    return std::bit_cast<uint64_t>(value);
}

struct PolicySettingFixture final {
    sieve::ExecutionPolicyKeyV1 key;
    sieve::ExecutionPolicyScalarKindV1 kind;
    uint64_t baseline;
    uint64_t alternate;
};

constexpr std::array<PolicySettingFixture,
                     sieve::DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1>
    POLICY_SETTINGS = {{
        {sieve::ExecutionPolicyKeyV1::lattice_lll, sieve::ExecutionPolicyScalarKindV1::closed_mode,
         1, 2},
        {sieve::ExecutionPolicyKeyV1::lattice_skew, sieve::ExecutionPolicyScalarKindV1::boolean, 1,
         0},
        {sieve::ExecutionPolicyKeyV1::adaptive_lattice, sieve::ExecutionPolicyScalarKindV1::boolean,
         1, 0},
        {sieve::ExecutionPolicyKeyV1::adaptive_lattice_threshold,
         sieve::ExecutionPolicyScalarKindV1::ieee754_binary64, binary64_bits(0.5),
         binary64_bits(0.75)},
        {sieve::ExecutionPolicyKeyV1::adaptive_lattice_max_retries,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3},
        {sieve::ExecutionPolicyKeyV1::adaptive_lattice_seed,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 123, 124},
        {sieve::ExecutionPolicyKeyV1::survival_filter, sieve::ExecutionPolicyScalarKindV1::boolean,
         1, 0},
        {sieve::ExecutionPolicyKeyV1::survival_threshold,
         sieve::ExecutionPolicyScalarKindV1::ieee754_binary64, binary64_bits(0.125),
         binary64_bits(0.25)},
        {sieve::ExecutionPolicyKeyV1::cofactor_brent, sieve::ExecutionPolicyScalarKindV1::boolean,
         1, 0},
        {sieve::ExecutionPolicyKeyV1::ecm_brent_suyama, sieve::ExecutionPolicyScalarKindV1::boolean,
         1, 0},
        {sieve::ExecutionPolicyKeyV1::ecm_bs_degree,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 12, 30},
        {sieve::ExecutionPolicyKeyV1::ecm_sigma_pool_size,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 16, 32},
        {sieve::ExecutionPolicyKeyV1::ecm_curve_pool,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 8, 16},
        {sieve::ExecutionPolicyKeyV1::ecm_batch_inv, sieve::ExecutionPolicyScalarKindV1::boolean, 1,
         0},
        {sieve::ExecutionPolicyKeyV1::cofactor_batch_size,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 64, 128},
        {sieve::ExecutionPolicyKeyV1::brent_pollard_rho_threads,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3},
        {sieve::ExecutionPolicyKeyV1::ecm_b1_cache_size,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 16, 32},
        {sieve::ExecutionPolicyKeyV1::ecm_stage1_parallel_threads,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3},
        {sieve::ExecutionPolicyKeyV1::ecm_stage2_parallel,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 1, 2},
        {sieve::ExecutionPolicyKeyV1::cofactor_result_cache_size,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 128, 256},
        {sieve::ExecutionPolicyKeyV1::trial_div_simd,
         sieve::ExecutionPolicyScalarKindV1::closed_mode, 1, 2},
        {sieve::ExecutionPolicyKeyV1::lattice_basis_parallel_threads,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3},
        {sieve::ExecutionPolicyKeyV1::lattice_coords_simd,
         sieve::ExecutionPolicyScalarKindV1::closed_mode, 1, 2},
        {sieve::ExecutionPolicyKeyV1::sieve_apply_tile_threads,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3},
        {sieve::ExecutionPolicyKeyV1::bucket_prefetch,
         sieve::ExecutionPolicyScalarKindV1::closed_mode, 1, 2},
        {sieve::ExecutionPolicyKeyV1::sieve_ecore_threads,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3},
        {sieve::ExecutionPolicyKeyV1::sieve_no_tiny_simd,
         sieve::ExecutionPolicyScalarKindV1::boolean, 1, 0},
        {sieve::ExecutionPolicyKeyV1::sieve_norm_tile_bits,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 4, 5},
        {sieve::ExecutionPolicyKeyV1::sieve_region_tile_bits,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 4, 5},
        {sieve::ExecutionPolicyKeyV1::sieve_saturated_sub_simd,
         sieve::ExecutionPolicyScalarKindV1::closed_mode, 1, 2},
        {sieve::ExecutionPolicyKeyV1::sieve_count_above_threshold_simd,
         sieve::ExecutionPolicyScalarKindV1::closed_mode, 1, 2},
    }};

constexpr std::array<uint64_t, sieve::DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1>
    ALL_UNSET_POLICY_BITS = {
        2, 0, 0, binary64_bits(0.5),
        2, 0, 0, binary64_bits(0.0),
        0, 0, 0, 0,
        0, 0, 1, 1,
        0, 1, 1, 0,
        1, 1, 1, 1,
        1, 0, 0, 0,
        0, 1, 1,
};

static_assert(POLICY_SETTINGS.size() == sieve::DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1);
static_assert(ALL_UNSET_POLICY_BITS.size() == POLICY_SETTINGS.size());

[[nodiscard]] sieve::DistributedSieveExecutionPolicyV1 make_execution_policy() {
    sieve::DistributedSieveExecutionPolicyV1 policy;
    policy.settings.reserve(POLICY_SETTINGS.size());
    for (std::size_t index = 0; index < POLICY_SETTINGS.size(); ++index) {
        const auto& spec = POLICY_SETTINGS[index];
        CHECK(static_cast<uint16_t>(spec.key) == index + 1);
        policy.settings.push_back({spec.key, spec.kind, spec.baseline});
    }
    require_ok(sieve::validate_distributed_sieve_execution_policy(policy),
               "baseline execution policy");
    return policy;
}

[[nodiscard]] std::vector<sieve::ChunkPlanV1> make_work_chunks() {
    return {
        sieve::ChunkPlanV1{0, 2, 3, "chunk_0"},
        sieve::ChunkPlanV1{1, 3, 5, "chunk_1"},
    };
}

[[nodiscard]] sieve::DistributedSieveWorkIdentityV1 make_work_identity() {
    sieve::DistributedSieveWorkIdentityV1 identity;
    identity.polynomial.n.decimal = "1000036000099";
    identity.polynomial.m.decimal = "10001";
    identity.polynomial.degree = 2;
    identity.polynomial.coefficients = {
        sieve::CanonicalIntegerV1{"-5"},
        sieve::CanonicalIntegerV1{"3"},
        sieve::CanonicalIntegerV1{"1"},
    };
    identity.polynomial.skewness_ieee754_bits = binary64_bits(1.25);

    identity.factor_base.rational_bound = 100;
    identity.factor_base.algebraic_bound = 200;
    identity.factor_base.large_prime_bound = 10'000;
    identity.factor_base.log_scale = 16;
    identity.factor_base.rational = {{2, 16}, {5, 25}};
    identity.factor_base.algebraic = {
        {7, 1, 37, 1},   {11, 4, 55, 2},
        {211, 3, 61, 1}, {223, std::numeric_limits<uint32_t>::max(), 67, 1},
        {227, 5, 71, 1},
    };
    identity.factor_base.sieve_algebraic_count = 2;

    identity.sieve.log_scale = 16;
    identity.sieve.rational_threshold = 50;
    identity.sieve.algebraic_threshold = 51;
    identity.sieve.large_prime_bound = 10'000;
    identity.sieve.allow_2lp = true;
    identity.sieve.allow_3lp = false;

    identity.region.i_min = -100;
    identity.region.i_max = 100;
    identity.region.j_min = 1;
    identity.region.j_max = 50;

    identity.cofactor.large_prime_bound = 10'000;
    identity.cofactor.allow_1lp = true;
    identity.cofactor.allow_2lp = true;
    identity.cofactor.allow_3lp = true;
    identity.cofactor.max_factorization_attempts = 20;

    identity.original_sq_bounds = {0, 5, 100, 1000};
    identity.effective_sq_bounds = {2, 5, 0, std::numeric_limits<uint32_t>::max()};

    identity.distributed.worker_count = 2;
    identity.distributed.chunks = make_work_chunks();
    identity.distributed.sq_cap_per_worker = 10;
    identity.distributed.relation_cap_per_worker = 100;
    identity.distributed.max_worker_attempts = 2;
    identity.distributed.max_merge_build_attempts = 2;
    identity.distributed.max_consumption_attempts = 2;
    identity.execution_policy = make_execution_policy();

    identity.semantic_versions.relation_serialization_version = 1;
    identity.semantic_versions.ooc_format_version = 1;
    identity.semantic_versions.digest_version = 1;
    identity.semantic_versions.handoff_version = 1;
    identity.semantic_versions.retry_policy_version = 1;
    identity.semantic_versions.chunking_version = 1;
    identity.semantic_versions.completion_version = 1;
    identity.semantic_versions.deduplication_version = 1;
    identity.semantic_versions.merge_policy_version = 1;

    require_ok(sieve::validate_distributed_sieve_work_identity(identity), "baseline work identity");
    return identity;
}

[[nodiscard]] Digest work_digest_or_fail(const sieve::DistributedSieveWorkIdentityV1& identity) {
    const auto result = sieve::distributed_sieve_work_digest(identity);
    if (!result) {
        fail("distributed_sieve_work_digest", __LINE__,
             sieve::distributed_sieve_protocol_error_name(result.status.error));
    }
    return *result.digest;
}

[[nodiscard]] sieve::TerminalChunkInputV1 terminal_input(uint32_t chunk_id, uint32_t begin,
                                                         uint32_t end, uint8_t seed,
                                                         uint64_t relation_count) {
    sieve::TerminalChunkInputV1 input;
    input.chunk_id = chunk_id;
    input.disposition = sieve::ChunkDispositionV1::handoff;
    input.sq_begin = begin;
    input.sq_end = end;
    input.next_sq_index = end;
    input.processed_sq_count = static_cast<uint64_t>(end - begin);
    input.completion_reason = sieve::WorkerCompletionReasonV1::range_exhausted;
    input.durable_attempt_count = 1;
    input.last_attempt_digest = digest_with_seed(seed);
    input.lease_id = lease_id_with_seed(static_cast<uint64_t>(seed) + 100);
    input.handoff_digest = digest_with_seed(static_cast<uint8_t>(seed + 1));
    input.raw_relation_count = relation_count;
    input.sequence_receipt.relation_count = relation_count;
    input.sequence_receipt.low = static_cast<uint64_t>(seed) + 200;
    input.sequence_receipt.high = static_cast<uint64_t>(seed) + 201;
    input.corpus_sha256 = digest_with_seed(static_cast<uint8_t>(seed + 2));
    return input;
}

struct ProtocolFixture final {
    sieve::DistributedSieveWorkIdentityV1 work_identity = make_work_identity();
    Digest work_digest = work_digest_or_fail(work_identity);
    sieve::WaveManifestV1 manifest;
    sieve::AttemptStartedV1 attempt;
    sieve::AttemptStartedV1 failure_attempt_0;
    sieve::AttemptStartedV1 failure_attempt_1;
    sieve::ChunkTerminalFailureV1 terminal_failure;
    sieve::WorkerHandoffV1 handoff;
    sieve::WorkerHandoffV1 handoff_1;
    sieve::MergeStartedV1 merge_started;
    sieve::MergePreparedV1 merge_prepared;
    sieve::WaveMergeCommitV1 merge_commit;
    std::array<sieve::ArtifactCleanupAuthorizedV1, 3> cleanup_authorizations;
    std::array<sieve::ArtifactCleanupCompletedV1, 3> cleanup_completions;
    sieve::ConsumptionStartedV1 consumption_started;
    sieve::SuccessorPreparedV1 successor_prepared;
    sieve::WaveConsumptionAckV1 consumption_ack;
    sieve::WaveCompletedV1 completed;

    ProtocolFixture() {
        manifest.wave_id = wave_id_with_seed(1);
        manifest.execution_contract_version = 1;
        manifest.executable_sha256 = digest_with_seed(2);
        manifest.work_sha256 = work_digest;
        manifest.wave_root_identity = native_identity(10);
        manifest.permanent_lock_identity = native_identity(20);
        manifest.lock_semantics_version = 1;
        manifest.effective_sq_begin = 2;
        manifest.effective_sq_end = 5;
        manifest.worker_count = 2;
        manifest.chunks = {
            sieve::ChunkPlanV1{0, 2, 3, "chunk_0"},
            sieve::ChunkPlanV1{1, 3, 5, "chunk_1"},
        };
        manifest.sq_cap_per_worker = 10;
        manifest.relation_cap_per_worker = 100;
        manifest.max_worker_attempts = 2;
        manifest.max_merge_build_attempts = 2;
        manifest.max_consumption_attempts = 2;
        manifest.canonical_naming_version = 1;
        manifest.retry_policy_version = 1;
        manifest.durable_start_consumes_ordinal = true;
        manifest.ooc_format_version = 1;
        manifest.relation_serialization_version = 1;
        manifest.handoff_version = 1;
        manifest.receipt_version = 1;
        manifest.digest_version = 1;
        manifest.merge_policy_version = 1;
        manifest = seal_value(std::move(manifest));

        attempt.manifest_digest = manifest.self_digest;
        attempt.chunk_id = 0;
        attempt.sq_begin = 2;
        attempt.sq_end = 3;
        attempt.attempt_ordinal = 0;
        attempt.predecessor_digest = manifest.self_digest;
        attempt.lease = lease_identity(100, "chunk_0_attempt_00");
        attempt.retry_policy_version = 1;
        attempt = seal_value(std::move(attempt));

        failure_attempt_0.manifest_digest = manifest.self_digest;
        failure_attempt_0.chunk_id = 1;
        failure_attempt_0.sq_begin = 3;
        failure_attempt_0.sq_end = 5;
        failure_attempt_0.attempt_ordinal = 0;
        failure_attempt_0.predecessor_digest = manifest.self_digest;
        failure_attempt_0.lease = lease_identity(130, "chunk_1_attempt_00");
        failure_attempt_0.retry_policy_version = 1;
        failure_attempt_0 = seal_value(std::move(failure_attempt_0));

        failure_attempt_1 = failure_attempt_0;
        failure_attempt_1.attempt_ordinal = 1;
        failure_attempt_1.predecessor_digest = failure_attempt_0.self_digest;
        failure_attempt_1.lease = lease_identity(160, "chunk_1_attempt_01");
        failure_attempt_1 = reseal(std::move(failure_attempt_1));

        terminal_failure.manifest_digest = manifest.self_digest;
        terminal_failure.chunk_id = 1;
        terminal_failure.sq_begin = 3;
        terminal_failure.sq_end = 5;
        terminal_failure.exhausted_attempt_count = 2;
        terminal_failure.last_attempt_digest = failure_attempt_1.self_digest;
        const std::array failed_attempt_chain = {failure_attempt_0, failure_attempt_1};
        const auto failed_chain_digest = sieve::distributed_sieve_attempt_chain_digest(
            manifest.self_digest, failed_attempt_chain);
        if (!failed_chain_digest) {
            fail("distributed_sieve_attempt_chain_digest", __LINE__,
                 sieve::distributed_sieve_protocol_error_name(failed_chain_digest.status.error));
        }
        terminal_failure.predecessor_chain_digest = *failed_chain_digest.digest;
        terminal_failure.reason = sieve::ChunkTerminalFailureReasonV1::attempt_budget_exhausted;
        terminal_failure.wait_facts.kind = sieve::WorkerWaitFactKindV1::unavailable;
        terminal_failure.no_canonical_handoff_confirmed = true;
        terminal_failure.exact_attempt_lease_absent_confirmed = true;
        terminal_failure.next_sq_index = 3;
        terminal_failure.processed_sq_count = 0;
        terminal_failure = seal_value(std::move(terminal_failure));

        handoff.manifest_digest = manifest.self_digest;
        handoff.work_digest = work_digest;
        handoff.wave_id = manifest.wave_id;
        handoff.chunk_id = 0;
        handoff.sq_begin = 2;
        handoff.sq_end = 3;
        handoff.attempt_ordinal = 0;
        handoff.attempt_started_digest = attempt.self_digest;
        handoff.lease = attempt.lease;
        handoff.artifact = corpus_artifact(10, 3);
        handoff.processed_sq_count = 1;
        handoff.next_sq_index = 3;
        handoff.completion_reason = sieve::WorkerCompletionReasonV1::range_exhausted;
        handoff.relation_count = 3;
        handoff.cleanup_intent_absent = true;
        handoff = seal_value(std::move(handoff));

        handoff_1.manifest_digest = manifest.self_digest;
        handoff_1.work_digest = work_digest;
        handoff_1.wave_id = manifest.wave_id;
        handoff_1.chunk_id = 1;
        handoff_1.sq_begin = 3;
        handoff_1.sq_end = 5;
        handoff_1.attempt_ordinal = 0;
        handoff_1.attempt_started_digest = failure_attempt_0.self_digest;
        handoff_1.lease = failure_attempt_0.lease;
        handoff_1.artifact = corpus_artifact(1000, 4);
        handoff_1.processed_sq_count = 1;
        handoff_1.next_sq_index = 5;
        handoff_1.completion_reason = sieve::WorkerCompletionReasonV1::range_exhausted;
        handoff_1.relation_count = 4;
        handoff_1.cleanup_intent_absent = true;
        handoff_1 = seal_value(std::move(handoff_1));

        std::vector<sieve::TerminalChunkInputV1> inputs = {
            terminal_input(0, 2, 3, 30, 3),
            terminal_input(1, 3, 5, 40, 4),
        };
        inputs[0].last_attempt_digest = attempt.self_digest;
        inputs[0].lease_id = attempt.lease.lease_id;
        inputs[0].handoff_digest = handoff.self_digest;
        inputs[0].sequence_receipt = handoff.artifact.sequence_receipt;
        inputs[0].corpus_sha256 = handoff.artifact.corpus_sha256;
        inputs[1].last_attempt_digest = failure_attempt_0.self_digest;
        inputs[1].lease_id = failure_attempt_0.lease.lease_id;
        inputs[1].handoff_digest = handoff_1.self_digest;
        inputs[1].processed_sq_count = handoff_1.processed_sq_count;
        inputs[1].sequence_receipt = handoff_1.artifact.sequence_receipt;
        inputs[1].corpus_sha256 = handoff_1.artifact.corpus_sha256;

        merge_started.manifest_digest = manifest.self_digest;
        merge_started.work_digest = work_digest;
        merge_started.ordered_inputs = inputs;
        merge_started.merge_policy_version = 1;
        merge_started.merged_lease = lease_identity(200, "merged_attempt_0");
        merge_started.merge_attempt_ordinal = 0;
        merge_started.predecessor_digest = manifest.self_digest;
        merge_started = seal_value(std::move(merge_started));

        merge_prepared.manifest_digest = manifest.self_digest;
        merge_prepared.work_digest = work_digest;
        merge_prepared.merge_policy_version = 1;
        merge_prepared.merge_started_digest = merge_started.self_digest;
        merge_prepared.ordered_inputs = inputs;
        merge_prepared.input_relation_count = 7;
        merge_prepared.duplicate_relation_count = 1;
        merge_prepared.output_relation_count = 6;
        merge_prepared.per_chunk_retained_counts = {{0, 3}, {1, 3}};
        merge_prepared.merged_artifact = corpus_artifact(2000, 6);
        merge_prepared.merged_lease = merge_started.merged_lease;
        merge_prepared = seal_value(std::move(merge_prepared));

        merge_commit.manifest_digest = manifest.self_digest;
        merge_commit.work_digest = work_digest;
        merge_commit.chunks = {
            sieve::ChunkCommitSummaryV1{inputs[0], 3, {}},
            sieve::ChunkCommitSummaryV1{inputs[1], 3, {}},
        };
        merge_commit.merge_policy_version = 1;
        merge_commit.input_relation_count = 7;
        merge_commit.duplicate_relation_count = 1;
        merge_commit.output_relation_count = 6;
        merge_commit.merge_prepared_digest = merge_prepared.self_digest;
        merge_commit.merged_lease = merge_started.merged_lease;
        merge_commit.merged_artifact = merge_prepared.merged_artifact;
        merge_commit = seal_value(std::move(merge_commit));

        cleanup_authorizations[0].authorizer = sieve::CleanupAuthorizerKindV1::merge_commit_worker;
        cleanup_authorizations[0].manifest_digest = manifest.self_digest;
        cleanup_authorizations[0].authorizer_record_digest = merge_commit.self_digest;
        cleanup_authorizations[0].artifact_kind = sieve::CleanupArtifactKindV1::worker;
        cleanup_authorizations[0].manifest_order_ordinal = 0;
        cleanup_authorizations[0].lease = handoff.lease;
        cleanup_authorizations[0].handoff_digest = handoff.self_digest;
        cleanup_authorizations[0].artifact = handoff.artifact;
        cleanup_authorizations[0] = seal_value(std::move(cleanup_authorizations[0]));

        cleanup_authorizations[1] = cleanup_authorizations[0];
        cleanup_authorizations[1].manifest_order_ordinal = 1;
        cleanup_authorizations[1].lease = handoff_1.lease;
        cleanup_authorizations[1].handoff_digest = handoff_1.self_digest;
        cleanup_authorizations[1].artifact = handoff_1.artifact;
        cleanup_authorizations[1] = reseal(std::move(cleanup_authorizations[1]));

        cleanup_completions[0].authorization_digest = cleanup_authorizations[0].self_digest;
        cleanup_completions[0].cleanup_intent_identity = native_identity(300);
        cleanup_completions[0].parent_directory_durability_confirmed = true;
        cleanup_completions[0].expected_namespace_absent = true;
        cleanup_completions[0] = seal_value(std::move(cleanup_completions[0]));

        cleanup_completions[1] = cleanup_completions[0];
        cleanup_completions[1].authorization_digest = cleanup_authorizations[1].self_digest;
        cleanup_completions[1].cleanup_intent_identity = native_identity(310);
        cleanup_completions[1] = reseal(std::move(cleanup_completions[1]));

        consumption_started.merge_commit_digest = merge_commit.self_digest;
        consumption_started.manifest_digest = manifest.self_digest;
        consumption_started.consumer_kind =
            sieve::ConsumerKindV1::structured_reduction_relation_corpus;
        consumption_started.execution_contract_version = 1;
        consumption_started.successor_lease = lease_identity(400, "successor_attempt_0");
        consumption_started.successor_format_version = 1;
        consumption_started.consumption_attempt_ordinal = 0;
        consumption_started.predecessor_digest = manifest.self_digest;
        consumption_started = seal_value(std::move(consumption_started));

        successor_prepared.consumption_started_digest = consumption_started.self_digest;
        successor_prepared.successor_lease = consumption_started.successor_lease;
        successor_prepared.successor_artifact = corpus_artifact(3000, 6);
        successor_prepared.successor_semantic_digest = digest_with_seed(61);
        successor_prepared.input_relation_count = 6;
        successor_prepared.output_relation_count = 6;
        successor_prepared = seal_value(std::move(successor_prepared));

        consumption_ack.merge_commit_digest = merge_commit.self_digest;
        consumption_ack.consumer_kind = sieve::ConsumerKindV1::structured_reduction_relation_corpus;
        consumption_ack.consumption_started_digest = consumption_started.self_digest;
        consumption_ack.successor_prepared_digest = successor_prepared.self_digest;
        consumption_ack.successor_artifact = successor_prepared.successor_artifact;
        consumption_ack.successor_semantic_digest = successor_prepared.successor_semantic_digest;
        consumption_ack.successor_cleanup_authority_identity =
            consumption_started.successor_lease.owner_marker;
        consumption_ack = seal_value(std::move(consumption_ack));

        cleanup_authorizations[2].authorizer =
            sieve::CleanupAuthorizerKindV1::consumption_ack_merged;
        cleanup_authorizations[2].manifest_digest = manifest.self_digest;
        cleanup_authorizations[2].authorizer_record_digest = consumption_ack.self_digest;
        cleanup_authorizations[2].artifact_kind = sieve::CleanupArtifactKindV1::merged;
        cleanup_authorizations[2].manifest_order_ordinal = 0;
        cleanup_authorizations[2].lease = merge_commit.merged_lease;
        cleanup_authorizations[2].handoff_digest = merge_prepared.self_digest;
        cleanup_authorizations[2].artifact = merge_commit.merged_artifact;
        cleanup_authorizations[2] = seal_value(std::move(cleanup_authorizations[2]));

        cleanup_completions[2] = cleanup_completions[0];
        cleanup_completions[2].authorization_digest = cleanup_authorizations[2].self_digest;
        cleanup_completions[2].cleanup_intent_identity = native_identity(320);
        cleanup_completions[2] = reseal(std::move(cleanup_completions[2]));

        completed.wave_root_identity = manifest.wave_root_identity;
        completed.permanent_lock_identity = manifest.permanent_lock_identity;
        completed.manifest_digest = manifest.self_digest;
        completed.merge_commit_digest = merge_commit.self_digest;
        completed.consumption_ack_digest = consumption_ack.self_digest;
        completed.successor_prepared_digest = successor_prepared.self_digest;
        completed.chunks = merge_commit.chunks;
        completed.cleanup_confirmations = {
            {sieve::CleanupArtifactKindV1::worker, 0, cleanup_authorizations[0].self_digest,
             cleanup_completions[0].self_digest},
            {sieve::CleanupArtifactKindV1::worker, 1, cleanup_authorizations[1].self_digest,
             cleanup_completions[1].self_digest},
            {sieve::CleanupArtifactKindV1::merged, 0, cleanup_authorizations[2].self_digest,
             cleanup_completions[2].self_digest},
        };
        completed.successor_artifact = successor_prepared.successor_artifact;
        completed.successor_semantic_digest = successor_prepared.successor_semantic_digest;
        completed = seal_value(std::move(completed));
    }

    [[nodiscard]] std::vector<Record> all_records() const {
        return {
            manifest,
            attempt,
            terminal_failure,
            handoff,
            merge_started,
            merge_prepared,
            merge_commit,
            cleanup_authorizations[0],
            cleanup_completions[0],
            consumption_started,
            successor_prepared,
            consumption_ack,
            completed,
        };
    }
};

void mutate_digest_bound_field(Record& record) {
    std::visit(
        [](auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, sieve::WaveManifestV1>) {
                ++value.execution_contract_version;
            } else if constexpr (std::is_same_v<T, sieve::AttemptStartedV1>) {
                ++value.retry_policy_version;
            } else if constexpr (std::is_same_v<T, sieve::ChunkTerminalFailureV1>) {
                perturb_digest(value.predecessor_chain_digest);
            } else if constexpr (std::is_same_v<T, sieve::WorkerHandoffV1>) {
                perturb_digest(value.work_digest);
            } else if constexpr (std::is_same_v<T, sieve::MergeStartedV1>) {
                ++value.merge_policy_version;
            } else if constexpr (std::is_same_v<T, sieve::MergePreparedV1>) {
                ++value.merge_policy_version;
            } else if constexpr (std::is_same_v<T, sieve::WaveMergeCommitV1>) {
                ++value.merge_policy_version;
            } else if constexpr (std::is_same_v<T, sieve::ArtifactCleanupAuthorizedV1>) {
                perturb_digest(value.handoff_digest);
            } else if constexpr (std::is_same_v<T, sieve::ArtifactCleanupCompletedV1>) {
                perturb_digest(value.authorization_digest);
            } else if constexpr (std::is_same_v<T, sieve::ConsumptionStartedV1>) {
                ++value.execution_contract_version;
            } else if constexpr (std::is_same_v<T, sieve::SuccessorPreparedV1>) {
                perturb_digest(value.successor_semantic_digest);
            } else if constexpr (std::is_same_v<T, sieve::WaveConsumptionAckV1>) {
                perturb_digest(value.successor_semantic_digest);
            } else if constexpr (std::is_same_v<T, sieve::WaveCompletedV1>) {
                perturb_digest(value.successor_semantic_digest);
            }
        },
        record);
}

void test_closed_names_and_record_kinds() {
    constexpr std::array ERRORS = {
        sieve::DistributedSieveProtocolError::none,
        sieve::DistributedSieveProtocolError::input_too_large,
        sieve::DistributedSieveProtocolError::output_too_large,
        sieve::DistributedSieveProtocolError::truncated,
        sieve::DistributedSieveProtocolError::trailing_bytes,
        sieve::DistributedSieveProtocolError::invalid_magic,
        sieve::DistributedSieveProtocolError::unsupported_wire_version,
        sieve::DistributedSieveProtocolError::unsupported_schema_version,
        sieve::DistributedSieveProtocolError::declared_size_mismatch,
        sieve::DistributedSieveProtocolError::unknown_record_kind,
        sieve::DistributedSieveProtocolError::unknown_enum,
        sieve::DistributedSieveProtocolError::invalid_boolean,
        sieve::DistributedSieveProtocolError::invalid_value,
        sieve::DistributedSieveProtocolError::invalid_string,
        sieve::DistributedSieveProtocolError::collection_too_large,
        sieve::DistributedSieveProtocolError::duplicate_entry,
        sieve::DistributedSieveProtocolError::noncanonical_order,
        sieve::DistributedSieveProtocolError::range_gap,
        sieve::DistributedSieveProtocolError::range_overlap,
        sieve::DistributedSieveProtocolError::integer_out_of_range,
        sieve::DistributedSieveProtocolError::digest_mismatch,
        sieve::DistributedSieveProtocolError::digest_unavailable,
        sieve::DistributedSieveProtocolError::resource_exhausted,
        sieve::DistributedSieveProtocolError::record_type_mismatch,
    };
    static_assert(static_cast<uint8_t>(sieve::DistributedSieveProtocolError::record_type_mismatch) +
                      1U ==
                  ERRORS.size());
    for (std::size_t left = 0; left < ERRORS.size(); ++left) {
        const auto left_name = sieve::distributed_sieve_protocol_error_name(ERRORS[left]);
        CHECK(!left_name.empty());
        for (std::size_t right = left + 1; right < ERRORS.size(); ++right) {
            CHECK(left_name != sieve::distributed_sieve_protocol_error_name(ERRORS[right]));
        }
    }

    constexpr std::array KINDS = {
        sieve::DistributedSieveRecordKindV1::wave_manifest,
        sieve::DistributedSieveRecordKindV1::attempt_started,
        sieve::DistributedSieveRecordKindV1::chunk_terminal_failure,
        sieve::DistributedSieveRecordKindV1::worker_handoff,
        sieve::DistributedSieveRecordKindV1::merge_started,
        sieve::DistributedSieveRecordKindV1::merge_prepared,
        sieve::DistributedSieveRecordKindV1::wave_merge_commit,
        sieve::DistributedSieveRecordKindV1::artifact_cleanup_authorized,
        sieve::DistributedSieveRecordKindV1::artifact_cleanup_completed,
        sieve::DistributedSieveRecordKindV1::consumption_started,
        sieve::DistributedSieveRecordKindV1::successor_prepared,
        sieve::DistributedSieveRecordKindV1::wave_consumption_ack,
        sieve::DistributedSieveRecordKindV1::wave_completed,
    };
    static_assert(static_cast<uint16_t>(sieve::DistributedSieveRecordKindV1::wave_completed) ==
                  KINDS.size());
    ProtocolFixture fixture;
    const auto records = fixture.all_records();
    CHECK(records.size() == KINDS.size());
    for (std::size_t index = 0; index < KINDS.size(); ++index) {
        CHECK(static_cast<uint16_t>(KINDS[index]) == index + 1);
        CHECK(!sieve::distributed_sieve_record_kind_name(KINDS[index]).empty());
        CHECK(sieve::distributed_sieve_record_kind(records[index]) == KINDS[index]);
    }

    auto invalid_terminal = fixture.terminal_failure;
    invalid_terminal.reason = static_cast<sieve::ChunkTerminalFailureReasonV1>(0);
    require_failed(validate_value(invalid_terminal, false), "unknown terminal reason");
    invalid_terminal = fixture.terminal_failure;
    invalid_terminal.wait_facts.kind = static_cast<sieve::WorkerWaitFactKindV1>(0);
    require_failed(validate_value(invalid_terminal, false), "unknown wait fact");

    auto invalid_handoff = fixture.handoff;
    invalid_handoff.completion_reason = static_cast<sieve::WorkerCompletionReasonV1>(0);
    require_failed(validate_value(invalid_handoff, false), "unknown completion reason");

    auto invalid_merge = fixture.merge_started;
    invalid_merge.ordered_inputs.front().disposition = static_cast<sieve::ChunkDispositionV1>(0);
    require_failed(validate_value(invalid_merge, false), "unknown chunk disposition");

    auto invalid_commit = fixture.merge_commit;
    invalid_commit.chunks.front().diagnostic.kind =
        static_cast<sieve::NormalizedDiagnosticKindV1>(0);
    require_failed(validate_value(invalid_commit, false), "unknown diagnostic kind");
    constexpr std::array DIAGNOSTIC_KINDS = {
        sieve::NormalizedDiagnosticKindV1::none,
        sieve::NormalizedDiagnosticKindV1::recovered_handoff,
        sieve::NormalizedDiagnosticKindV1::retried_after_exit,
        sieve::NormalizedDiagnosticKindV1::retried_after_signal,
        sieve::NormalizedDiagnosticKindV1::retried_after_invalid_handoff,
    };
    for (const auto kind : DIAGNOSTIC_KINDS) {
        auto valid_commit = fixture.merge_commit;
        valid_commit.chunks.front().diagnostic.kind = kind;
        valid_commit.chunks.front().diagnostic.code =
            kind == sieve::NormalizedDiagnosticKindV1::retried_after_exit ||
                    kind == sieve::NormalizedDiagnosticKindV1::retried_after_signal
                ? 1U
                : 0U;
        require_ok(validate_value(valid_commit, false), "closed diagnostic kind");
    }

    auto invalid_cleanup = fixture.cleanup_authorizations[0];
    invalid_cleanup.authorizer = static_cast<sieve::CleanupAuthorizerKindV1>(0);
    require_failed(validate_value(invalid_cleanup, false), "unknown cleanup authorizer");
    invalid_cleanup = fixture.cleanup_authorizations[0];
    invalid_cleanup.artifact_kind = static_cast<sieve::CleanupArtifactKindV1>(0);
    require_failed(validate_value(invalid_cleanup, false), "unknown cleanup artifact kind");

    auto invalid_consumer = fixture.consumption_started;
    invalid_consumer.consumer_kind = static_cast<sieve::ConsumerKindV1>(0);
    require_failed(validate_value(invalid_consumer, false), "unknown consumer kind");
}

void test_all_record_round_trips_and_self_digests() {
    ProtocolFixture fixture;
    const auto records = fixture.all_records();

    for (const auto& record : records) {
        require_ok(sieve::validate_distributed_sieve_record(record), "sealed record validation");
        CHECK(record_digest_or_fail(record) == self_digest(record));

        const auto encoded = encode_or_fail(record);
        CHECK(!encoded.empty());
        CHECK(encoded.size() <= sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_RECORD_BYTES);

        const auto decoded = sieve::decode_distributed_sieve_record(encoded);
        CHECK(decoded);
        CHECK(decoded.value.has_value());
        CHECK(sieve::distributed_sieve_record_kind(*decoded.value) ==
              sieve::distributed_sieve_record_kind(record));
        CHECK(encode_or_fail(*decoded.value) == encoded);

        Record changed = record;
        mutate_digest_bound_field(changed);
        require_ok(sieve::validate_distributed_sieve_record(changed, false),
                   "semantic mutation without digest verification");
        const auto stale_digest_status = sieve::validate_distributed_sieve_record(changed, true);
        CHECK(!stale_digest_status);
        CHECK(stale_digest_status.error == sieve::DistributedSieveProtocolError::digest_mismatch);

        require_ok(sieve::seal_distributed_sieve_record(changed), "reseal semantic mutation");
        CHECK(self_digest(changed) != self_digest(record));
        require_ok(sieve::validate_distributed_sieve_record(changed), "resealed semantic mutation");
    }

    auto high_limb_drift = fixture.attempt;
    ++high_limb_drift.lease.lease_id.limbs[1];
    high_limb_drift = reseal(std::move(high_limb_drift));
    CHECK(high_limb_drift.self_digest != fixture.attempt.self_digest);
    const auto high_limb_round_trip =
        sieve::decode_distributed_sieve_record(encode_or_fail(Record{high_limb_drift}));
    CHECK(high_limb_round_trip);
    CHECK(std::get<sieve::AttemptStartedV1>(*high_limb_round_trip.value).lease.lease_id ==
          high_limb_drift.lease.lease_id);

    const std::array baseline_attempts = {fixture.attempt};
    const std::array high_limb_attempts = {high_limb_drift};
    const auto baseline_chain = sieve::distributed_sieve_attempt_chain_digest(
        fixture.manifest.self_digest, baseline_attempts);
    const auto high_limb_chain = sieve::distributed_sieve_attempt_chain_digest(
        fixture.manifest.self_digest, high_limb_attempts);
    CHECK(baseline_chain);
    CHECK(high_limb_chain);
    CHECK(*baseline_chain.digest != *high_limb_chain.digest);

    auto nil_lease = fixture.attempt;
    nil_lease.lease.lease_id = {};
    require_reseal_failed(std::move(nil_lease), "all-zero lease id is reserved");
}

void test_exact_framing_and_wire_tamper() {
    ProtocolFixture fixture;
    const auto records = fixture.all_records();

    for (const auto& record : records) {
        const auto encoded = encode_or_fail(record);
        for (std::size_t prefix_size = 0; prefix_size < encoded.size(); ++prefix_size) {
            const auto decoded = sieve::decode_distributed_sieve_record(
                std::span<const std::byte>(encoded).first(prefix_size));
            CHECK(!decoded);
            CHECK(decoded.status.error != sieve::DistributedSieveProtocolError::none);
        }

        auto trailing = encoded;
        trailing.push_back(std::byte{0x42});
        const auto trailing_result = sieve::decode_distributed_sieve_record(trailing);
        CHECK(!trailing_result);

        auto digest_tamper = encoded;
        digest_tamper.back() ^= std::byte{0x01};
        const auto digest_result = sieve::decode_distributed_sieve_record(digest_tamper);
        CHECK(!digest_result);
        CHECK(digest_result.status.error == sieve::DistributedSieveProtocolError::digest_mismatch);
    }

    auto lease_wire_order = fixture.attempt;
    lease_wire_order.lease.lease_id.limbs = {
        UINT64_C(0x0807060504030201),
        UINT64_C(0x1817161514131211),
    };
    lease_wire_order = reseal(std::move(lease_wire_order));
    const auto lease_wire = encode_or_fail(Record{lease_wire_order});
    constexpr std::size_t ATTEMPT_LEASE_ID_OFFSET = 24 + 32 + 4 + 4 + 4 + 4 + 32;
    constexpr std::array<std::byte, 16> EXPECTED_LEASE_ID_BYTES = {
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
        std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
        std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14},
        std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18},
    };
    CHECK(lease_wire.size() >= ATTEMPT_LEASE_ID_OFFSET + EXPECTED_LEASE_ID_BYTES.size());
    CHECK(std::equal(EXPECTED_LEASE_ID_BYTES.begin(), EXPECTED_LEASE_ID_BYTES.end(),
                     lease_wire.begin() + static_cast<std::ptrdiff_t>(ATTEMPT_LEASE_ID_OFFSET)));

    auto bad_magic = encode_or_fail(Record{fixture.manifest});
    bad_magic.front() ^= std::byte{0x80};
    const auto bad_magic_result = sieve::decode_distributed_sieve_record(bad_magic);
    CHECK(!bad_magic_result);
    CHECK(bad_magic_result.status.error == sieve::DistributedSieveProtocolError::invalid_magic);

    std::vector<std::byte> oversized(
        static_cast<std::size_t>(sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_RECORD_BYTES) + 1,
        std::byte{0});
    const auto oversized_result = sieve::decode_distributed_sieve_record(oversized);
    CHECK(!oversized_result);
    CHECK(oversized_result.status.error == sieve::DistributedSieveProtocolError::input_too_large);
}

void test_manifest_canonical_order_and_limits() {
    ProtocolFixture fixture;
    require_ok(validate_value(fixture.manifest, false), "baseline manifest");
    require_ok(sieve::validate_manifest_executable_identity(fixture.manifest,
                                                            fixture.manifest.executable_sha256),
               "manifest executable identity exact match");
    auto wrong_executable_digest = fixture.manifest.executable_sha256;
    perturb_digest(wrong_executable_digest);
    require_failed(
        sieve::validate_manifest_executable_identity(fixture.manifest, wrong_executable_digest),
        "manifest executable identity drift");

    auto zero_executable = fixture.manifest;
    zero_executable.executable_sha256 = {};
    zero_executable = reseal(std::move(zero_executable));
    require_ok(sieve::validate_manifest_executable_identity(zero_executable, {}),
               "all-zero executable digest remains an exact present identity");

    auto nil_wave_id = fixture.manifest;
    nil_wave_id.wave_id = {};
    require_failed(validate_value(nil_wave_id, false), "nil wave id rejected");

    auto maximum = fixture.manifest;
    maximum.effective_sq_begin = 100;
    maximum.effective_sq_end = 100 + sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS;
    maximum.worker_count = sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS;
    maximum.chunks.clear();
    for (uint32_t index = 0; index < sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS; ++index) {
        maximum.chunks.push_back(
            {index, 100 + index, 101 + index, "worker_" + std::to_string(index)});
    }
    maximum.max_worker_attempts = sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS;
    maximum.max_merge_build_attempts = sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS;
    maximum.max_consumption_attempts = sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS;
    require_ok(validate_value(maximum, false), "maximum manifest limits");

    auto too_many = maximum;
    too_many.worker_count = sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS + 1;
    too_many.effective_sq_end += 1;
    too_many.chunks.push_back({sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS,
                               maximum.effective_sq_end, too_many.effective_sq_end,
                               "worker_over_limit"});
    require_failed(validate_value(too_many, false), "manifest chunk limit");

    auto duplicate = fixture.manifest;
    duplicate.chunks[1].chunk_id = duplicate.chunks[0].chunk_id;
    require_failed(validate_value(duplicate, false), "duplicate chunk id");

    auto unordered = fixture.manifest;
    std::swap(unordered.chunks[0], unordered.chunks[1]);
    require_failed(validate_value(unordered, false), "unordered chunks");

    auto gap = fixture.manifest;
    ++gap.chunks[1].sq_begin;
    require_failed(validate_value(gap, false), "chunk range gap");

    auto overlap = fixture.manifest;
    --overlap.chunks[1].sq_begin;
    require_failed(validate_value(overlap, false), "chunk range overlap");

    auto wrong_count = fixture.manifest;
    ++wrong_count.worker_count;
    require_failed(validate_value(wrong_count, false), "worker/chunk count mismatch");

    auto bad_range = fixture.manifest;
    bad_range.chunks[0].sq_end = bad_range.chunks[0].sq_begin - 1;
    require_failed(validate_value(bad_range, false), "reversed chunk range");

    auto bad_stem = fixture.manifest;
    bad_stem.chunks[0].relative_artifact_stem.clear();
    require_failed(validate_value(bad_stem, false), "empty artifact stem");
    bad_stem = fixture.manifest;
    bad_stem.chunks[0].relative_artifact_stem.assign(
        static_cast<std::size_t>(sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES) + 1,
        'x');
    require_failed(validate_value(bad_stem, false), "artifact stem length limit");

    constexpr std::array<std::string_view, 8> NONPORTABLE_STEMS = {
        ".", "foo.", ".foo", "foo.bar", "CON", "nul", "COM1", "lPt9",
    };
    for (const auto stem : NONPORTABLE_STEMS) {
        auto bad_chunk_stem = fixture.manifest;
        bad_chunk_stem.chunks[0].relative_artifact_stem = stem;
        require_failed(validate_value(bad_chunk_stem, false),
                       "manifest rejects nonportable artifact stem");

        auto bad_lease_stem = fixture.attempt;
        bad_lease_stem.lease.relative_stem = stem;
        require_failed(validate_value(bad_lease_stem, false),
                       "lease rejects nonportable artifact stem");
    }

    constexpr std::array<std::string_view, 6> PORTABLE_STEMS = {
        "worker_0", "corpus", "foo", "foo-bar", "COM0", "COM10",
    };
    for (const auto stem : PORTABLE_STEMS) {
        auto valid_chunk_stem = fixture.manifest;
        valid_chunk_stem.chunks[0].relative_artifact_stem = stem;
        require_ok(validate_value(valid_chunk_stem, false), "manifest accepts portable stem");

        auto valid_lease_stem = fixture.attempt;
        valid_lease_stem.lease.relative_stem = stem;
        require_ok(validate_value(valid_lease_stem, false), "lease accepts portable stem");
    }

    auto case_folded_stems = fixture.manifest;
    case_folded_stems.chunks[0].relative_artifact_stem = "foo";
    case_folded_stems.chunks[1].relative_artifact_stem = "Foo";
    require_failed(validate_value(case_folded_stems, false),
                   "manifest chunk stems conflict under conservative ASCII case fold");

    auto zero_budget = fixture.manifest;
    zero_budget.max_worker_attempts = 0;
    require_failed(validate_value(zero_budget, false), "zero worker attempt budget");
    zero_budget = fixture.manifest;
    zero_budget.max_merge_build_attempts = 0;
    require_failed(validate_value(zero_budget, false), "zero merge attempt budget");
    zero_budget = fixture.manifest;
    zero_budget.max_consumption_attempts = 0;
    require_failed(validate_value(zero_budget, false), "zero consumption attempt budget");

    auto over_budget = fixture.manifest;
    over_budget.max_worker_attempts = sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS + 1;
    require_failed(validate_value(over_budget, false), "worker attempt budget limit");
    over_budget = fixture.manifest;
    over_budget.max_merge_build_attempts = sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS + 1;
    require_failed(validate_value(over_budget, false), "merge attempt budget limit");
    over_budget = fixture.manifest;
    over_budget.max_consumption_attempts = sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS + 1;
    require_failed(validate_value(over_budget, false), "consumption attempt budget limit");
}

void test_worker_attempt_naming_contract() {
    const auto two_digits = [](uint32_t value) {
        std::string digits(2, '0');
        digits[0] = static_cast<char>('0' + value / 10U);
        digits[1] = static_cast<char>('0' + value % 10U);
        return digits;
    };
    const auto ascii_casefold = [](std::string value) {
        for (char& character : value) {
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character + ('a' - 'A'));
            }
        }
        return value;
    };
    const auto require_unique = [](std::vector<std::string> values) {
        std::sort(values.begin(), values.end());
        CHECK(std::adjacent_find(values.begin(), values.end()) == values.end());
    };

    std::vector<std::string> stems;
    std::vector<std::string> namespace_leaves = {
        std::string(wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF),
        std::string(wave_detail::DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF),
        std::string(wave_detail::DISTRIBUTED_SIEVE_WAVE_MANIFEST_PENDING_LEAF),
    };
    stems.reserve(static_cast<std::size_t>(sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS) *
                  sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS);
    namespace_leaves.reserve(
        3U + 8U * static_cast<std::size_t>(sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS) *
                 sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS);

    for (uint32_t chunk_id = 0; chunk_id < sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS;
         ++chunk_id) {
        const std::string chunk_digits = two_digits(chunk_id);
        const std::string chunk_stem = "S" + chunk_digits;
        for (uint32_t ordinal = 0; ordinal < sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS;
             ++ordinal) {
            const std::string ordinal_digits = two_digits(ordinal);
            const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
                chunk_stem, chunk_id, ordinal);
            CHECK(names.has_value());
            CHECK(names->relative_lease_stem ==
                  chunk_stem + std::string(sieve::DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1) +
                      ordinal_digits);
            CHECK(names->private_directory_leaf ==
                  names->relative_lease_stem +
                      std::string(wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_DIRECTORY_SUFFIX));
            CHECK(names->base_lock_leaf ==
                  names->relative_lease_stem +
                      std::string(wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX));
            CHECK(names->reserved_leaf ==
                  names->relative_lease_stem +
                      std::string(wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVED_SUFFIX));
            CHECK(names->reserved_pending_leaf ==
                  names->relative_lease_stem +
                      std::string(
                          wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVED_PENDING_SUFFIX));
            CHECK(names->owned_leaf ==
                  names->relative_lease_stem +
                      std::string(wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNED_SUFFIX));
            CHECK(
                names->owned_pending_leaf ==
                names->relative_lease_stem +
                    std::string(wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNED_PENDING_SUFFIX));
            CHECK(names->canonical_record_leaf ==
                  std::string(wave_detail::DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PREFIX) +
                      chunk_digits +
                      std::string(
                          wave_detail::DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_ORDINAL_SEPARATOR) +
                      ordinal_digits);
            CHECK(names->pending_record_leaf ==
                  names->canonical_record_leaf +
                      std::string(
                          wave_detail::DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PENDING_SUFFIX));
            CHECK(names->canonical_record_leaf != names->pending_record_leaf);
            CHECK(names->relative_lease_stem.size() <=
                  sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES);
            CHECK(sieve::distributed_sieve_worker_attempt_relative_stem_matches(
                chunk_stem, ordinal, names->relative_lease_stem));

            const auto canonical = wave_detail::parse_distributed_sieve_worker_attempt_leaf_v1(
                names->canonical_record_leaf);
            const auto pending = wave_detail::parse_distributed_sieve_worker_attempt_leaf_v1(
                names->pending_record_leaf);
            CHECK(canonical.has_value());
            CHECK(pending.has_value());
            CHECK(canonical->chunk_id == chunk_id);
            CHECK(canonical->attempt_ordinal == ordinal);
            CHECK(!canonical->pending);
            CHECK(pending->chunk_id == chunk_id);
            CHECK(pending->attempt_ordinal == ordinal);
            CHECK(pending->pending);

            stems.push_back(names->relative_lease_stem);
            namespace_leaves.push_back(names->private_directory_leaf);
            namespace_leaves.push_back(names->base_lock_leaf);
            namespace_leaves.push_back(names->reserved_leaf);
            namespace_leaves.push_back(names->reserved_pending_leaf);
            namespace_leaves.push_back(names->owned_leaf);
            namespace_leaves.push_back(names->owned_pending_leaf);
            namespace_leaves.push_back(names->canonical_record_leaf);
            namespace_leaves.push_back(names->pending_record_leaf);
        }
    }

    require_unique(stems);
    require_unique(namespace_leaves);
    for (std::string& stem : stems) {
        stem = ascii_casefold(std::move(stem));
    }
    for (std::string& leaf : namespace_leaves) {
        leaf = ascii_casefold(std::move(leaf));
    }
    require_unique(std::move(stems));
    require_unique(std::move(namespace_leaves));

    for (uint32_t ordinal = 0; ordinal < sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS;
         ++ordinal) {
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1("S", 0, ordinal);
        CHECK(names.has_value());
        CHECK(names->relative_lease_stem == "S_attempt_" + two_digits(ordinal));
    }

    const auto lower_bound = wave_detail::distributed_sieve_worker_attempt_names_v1("S", 0, 0);
    const auto upper_bound = wave_detail::distributed_sieve_worker_attempt_names_v1("S", 63, 63);
    CHECK(lower_bound.has_value());
    CHECK(upper_bound.has_value());
    CHECK(lower_bound->private_directory_leaf == "S_attempt_00.gnfs-sink-lease");
    CHECK(lower_bound->base_lock_leaf == "S_attempt_00.gnfs-sink-lease.gnfs-ooc-cleanup-v1.lock");
    CHECK(lower_bound->reserved_leaf ==
          "S_attempt_00.gnfs-sink-lease.gnfs-private-lease-v1.reserved");
    CHECK(lower_bound->reserved_pending_leaf ==
          "S_attempt_00.gnfs-sink-lease.gnfs-private-lease-v1.reserved.pending");
    CHECK(lower_bound->owned_leaf == "S_attempt_00.gnfs-sink-lease.gnfs-private-lease-v1.owned");
    CHECK(lower_bound->owned_pending_leaf ==
          "S_attempt_00.gnfs-sink-lease.gnfs-private-lease-v1.owned.pending");
    CHECK(lower_bound->canonical_record_leaf == ".gnfs-wave-v1.attempt-c00-a00");
    CHECK(upper_bound->canonical_record_leaf == ".gnfs-wave-v1.attempt-c63-a63");
    CHECK(!wave_detail::distributed_sieve_worker_attempt_names_v1(
        "S", sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS, 0));
    CHECK(!wave_detail::distributed_sieve_worker_attempt_names_v1(
        "S", 0, sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS));
    CHECK(!sieve::distributed_sieve_worker_attempt_relative_stem_matches(
        "S", sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS, "S_attempt_64"));

    constexpr std::size_t ATTEMPT_STEM_SUFFIX_BYTES =
        sieve::DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size() +
        sieve::DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1;
    constexpr std::size_t MAX_CHUNK_STEM_BYTES =
        sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES - ATTEMPT_STEM_SUFFIX_BYTES;
    const std::string maximum_chunk_stem(MAX_CHUNK_STEM_BYTES, 's');
    const std::string too_long_chunk_stem(MAX_CHUNK_STEM_BYTES + 1U, 't');
    const auto maximum_names =
        wave_detail::distributed_sieve_worker_attempt_names_v1(maximum_chunk_stem, 63, 63);
    CHECK(maximum_names.has_value());
    CHECK(maximum_names->relative_lease_stem.size() ==
          sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES);
    CHECK(!wave_detail::distributed_sieve_worker_attempt_names_v1(too_long_chunk_stem, 0, 0));
    CHECK(!sieve::distributed_sieve_worker_attempt_relative_stem_matches(
        too_long_chunk_stem, 0, too_long_chunk_stem + "_attempt_00"));

    ProtocolFixture fixture;
    auto maximum_manifest = fixture.manifest;
    maximum_manifest.chunks[0].relative_artifact_stem = maximum_chunk_stem;
    require_ok(validate_value(maximum_manifest, false),
               "maximum representable nonempty worker-attempt stem");

    auto too_long_nonempty_manifest = fixture.manifest;
    too_long_nonempty_manifest.chunks[0].relative_artifact_stem = too_long_chunk_stem;
    require_failed(validate_value(too_long_nonempty_manifest, false),
                   "nonempty chunk stem must leave room for attempt suffix");

    auto long_empty_manifest = fixture.manifest;
    long_empty_manifest.effective_sq_end = long_empty_manifest.chunks[0].sq_end;
    long_empty_manifest.chunks[1].sq_begin = long_empty_manifest.chunks[0].sq_end;
    long_empty_manifest.chunks[1].sq_end = long_empty_manifest.chunks[0].sq_end;
    long_empty_manifest.chunks[1].relative_artifact_stem.assign(
        sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES, 'e');
    require_ok(validate_value(long_empty_manifest, false),
               "empty chunk may retain a maximum-length manifest stem");
    CHECK(!wave_detail::distributed_sieve_worker_attempt_names_v1(
        long_empty_manifest.chunks[1].relative_artifact_stem, 1, 0));
    CHECK(!sieve::distributed_sieve_worker_attempt_relative_stem_matches(
        long_empty_manifest.chunks[1].relative_artifact_stem, 0,
        long_empty_manifest.chunks[1].relative_artifact_stem + "_attempt_00"));

    auto empty_manifest = fixture.manifest;
    empty_manifest.effective_sq_end = empty_manifest.chunks[0].sq_end;
    empty_manifest.chunks[1].sq_begin = empty_manifest.chunks[0].sq_end;
    empty_manifest.chunks[1].sq_end = empty_manifest.chunks[0].sq_end;
    empty_manifest = reseal(std::move(empty_manifest));
    const auto empty_chunk_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        empty_manifest.chunks[1].relative_artifact_stem, 1, 0);
    CHECK(empty_chunk_names.has_value());
    auto empty_chunk_attempt = fixture.failure_attempt_0;
    empty_chunk_attempt.manifest_digest = empty_manifest.self_digest;
    empty_chunk_attempt.sq_begin = empty_manifest.chunks[1].sq_begin;
    empty_chunk_attempt.sq_end = empty_manifest.chunks[1].sq_end;
    empty_chunk_attempt.predecessor_digest = empty_manifest.self_digest;
    empty_chunk_attempt.lease.relative_stem = empty_chunk_names->relative_lease_stem;
    require_failed(validate_value(empty_chunk_attempt, false),
                   "record semantics reject a syntactically valid empty-chunk attempt");
    const std::span<const sieve::AttemptStartedV1> empty_chunk_attempts;
    require_failed(sieve::validate_worker_attempt_chain(empty_manifest, 1, empty_chunk_attempts,
                                                        nullptr, nullptr),
                   "empty chunk has no worker-attempt chain");

    constexpr std::array<std::string_view, 12> INVALID_BASE_STEMS = {
        "",        ".",        "..",  ".foo", "foo.", "foo.bar",
        "foo/bar", "foo\\bar", "CON", "nul",  "COM1", "lPt9",
    };
    for (const auto stem : INVALID_BASE_STEMS) {
        CHECK(!wave_detail::distributed_sieve_worker_attempt_names_v1(stem, 0, 0));
        CHECK(!sieve::distributed_sieve_worker_attempt_relative_stem_matches(
            stem, 0, std::string(stem) + "_attempt_00"));
    }

    constexpr std::array<std::string_view, 25> INVALID_RECORD_LEAVES = {
        "",
        ".gnfs-wave-v1.attempt-c7-a09",
        ".gnfs-wave-v1.attempt-c007-a09",
        ".gnfs-wave-v1.attempt-c07-a9",
        ".gnfs-wave-v1.attempt-c07-a009",
        ".GNFS-wave-v1.attempt-c07-a09",
        ".gnfs-WAVE-v1.attempt-c07-a09",
        ".gnfs-wave-v1.ATTEMPT-c07-a09",
        ".gnfs-wave-v1.attempt-C07-a09",
        ".gnfs-wave-v1.attempt-c07-A09",
        ".gnfs-wave-v1.attempt-c07_a09",
        ".gnfs-wave-v1.attempt-c07.a09",
        ".gnfs-wave-v1.attempt-c07-b09",
        ".gnfs-wave-v1.attempt-c+7-a09",
        ".gnfs-wave-v1.attempt-c-7-a09",
        ".gnfs-wave-v1.attempt-c07-a+9",
        ".gnfs-wave-v1.attempt-c07-a-9",
        ".gnfs-wave-v1.attempt-c07-a09.PENDING",
        ".gnfs-wave-v1.attempt-c07-a09.pending.pending",
        ".gnfs-wave-v1.attempt-c07-a09.tmp",
        ".gnfs-wave-v1.attempt-c07-a09.trailing",
        ".gnfs-wave-v1.attempt-c64-a00",
        ".gnfs-wave-v1.attempt-c00-a64",
        ".gnfs-wave-v1.attempt-c99-a99",
        "gnfs-wave-v1.attempt-c07-a09",
    };
    for (const auto leaf : INVALID_RECORD_LEAVES) {
        CHECK(!wave_detail::parse_distributed_sieve_worker_attempt_leaf_v1(leaf));
    }
    CHECK(!wave_detail::parse_distributed_sieve_worker_attempt_leaf_v1(
        wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF));
    CHECK(!wave_detail::parse_distributed_sieve_worker_attempt_leaf_v1(
        wave_detail::DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF));
    CHECK(!wave_detail::parse_distributed_sieve_worker_attempt_leaf_v1(
        wave_detail::DISTRIBUTED_SIEVE_WAVE_MANIFEST_PENDING_LEAF));
    std::string nul_terminated_alias = ".gnfs-wave-v1.attempt-c07-a09";
    nul_terminated_alias.push_back('\0');
    CHECK(!wave_detail::parse_distributed_sieve_worker_attempt_leaf_v1(nul_terminated_alias));

    const auto expected_names =
        wave_detail::distributed_sieve_worker_attempt_names_v1("chunk_0", 0, 0);
    CHECK(expected_names.has_value());
    CHECK(fixture.attempt.lease.relative_stem == expected_names->relative_lease_stem);
    const std::array baseline_attempts = {fixture.attempt};
    require_ok(sieve::validate_worker_attempt_chain(fixture.manifest, 0, baseline_attempts, nullptr,
                                                    nullptr),
               "worker chain accepts exact derived lease stem");

    const auto require_wrong_stem_rejected = [&](std::string wrong_stem) {
        auto attempt = fixture.attempt;
        attempt.lease.relative_stem = std::move(wrong_stem);
        attempt = reseal(std::move(attempt));
        const std::array attempts = {attempt};
        const auto status =
            sieve::validate_worker_attempt_chain(fixture.manifest, 0, attempts, nullptr, nullptr);
        CHECK(!status);
        CHECK(status.error == sieve::DistributedSieveProtocolError::noncanonical_order);
    };
    require_wrong_stem_rejected("chunk_0_attempt_0");
    require_wrong_stem_rejected("chunk_0_attempt_000");
    require_wrong_stem_rejected("Chunk_0_attempt_00");
    require_wrong_stem_rejected("chunk_0_Attempt_00");
    require_wrong_stem_rejected("chunk_0-attempt-00");
    require_wrong_stem_rejected("chunk_1_attempt_00");
    require_wrong_stem_rejected("chunk_0_attempt_01");
    require_wrong_stem_rejected("chunk_0_attempt_00_trailing");

    const std::array valid_retry_attempts = {fixture.failure_attempt_0, fixture.failure_attempt_1};
    require_ok(sieve::validate_worker_attempt_chain(fixture.manifest, 1, valid_retry_attempts,
                                                    nullptr, nullptr),
               "worker retry chain accepts per-ordinal derived stems");
    auto wrong_retry = fixture.failure_attempt_1;
    wrong_retry.lease.relative_stem = fixture.failure_attempt_0.lease.relative_stem;
    wrong_retry = reseal(std::move(wrong_retry));
    const std::array invalid_retry_attempts = {fixture.failure_attempt_0, wrong_retry};
    require_failed(sieve::validate_worker_attempt_chain(fixture.manifest, 1, invalid_retry_attempts,
                                                        nullptr, nullptr),
                   "worker retry chain rejects reused ordinal stem");

    auto wrong_naming_version = fixture.manifest;
    ++wrong_naming_version.canonical_naming_version;
    require_failed(validate_value(wrong_naming_version, false),
                   "manifest rejects unknown canonical naming version");
    wrong_naming_version.canonical_naming_version = 0;
    require_failed(validate_value(wrong_naming_version, false),
                   "manifest rejects zero canonical naming version");
}

void test_worker_completion_reason_closure() {
    ProtocolFixture fixture;

    auto sq_cap = fixture.handoff;
    sq_cap.artifact = corpus_artifact(5000, 0);
    sq_cap.sq_end = sq_cap.sq_begin + 3;
    sq_cap.processed_sq_count = 2;
    sq_cap.next_sq_index = sq_cap.sq_begin + 2;
    sq_cap.completion_reason = sieve::WorkerCompletionReasonV1::sq_cap;
    sq_cap.relation_count = 0;
    sq_cap = reseal(std::move(sq_cap));
    require_ok(validate_value(sq_cap), "partial SQ-cap zero-row handoff");
    const auto sq_cap_round_trip =
        sieve::decode_distributed_sieve_record(encode_or_fail(Record{sq_cap}));
    CHECK(sq_cap_round_trip);

    auto relation_cap = sq_cap;
    relation_cap.completion_reason = sieve::WorkerCompletionReasonV1::relation_cap;
    require_failed(validate_value(relation_cap, false),
                   "relation cap requires at least one relation");

    auto zero_relations = sq_cap;
    zero_relations.processed_sq_count =
        static_cast<uint64_t>(zero_relations.sq_end - zero_relations.sq_begin);
    zero_relations.next_sq_index = zero_relations.sq_end;
    zero_relations.completion_reason = sieve::WorkerCompletionReasonV1::zero_relations;
    zero_relations = reseal(std::move(zero_relations));
    require_ok(validate_value(zero_relations), "full-range zero-relation handoff");

    auto partial_zero = zero_relations;
    partial_zero.next_sq_index = partial_zero.sq_end - 1;
    partial_zero.processed_sq_count =
        static_cast<uint64_t>(partial_zero.next_sq_index - partial_zero.sq_begin);
    require_failed(validate_value(partial_zero, false),
                   "zero-relations reason requires full range");

    auto nonzero_zero_reason = zero_relations;
    nonzero_zero_reason.artifact = corpus_artifact(5001, 1);
    nonzero_zero_reason.relation_count = 1;
    require_failed(validate_value(nonzero_zero_reason, false),
                   "zero-relations reason requires zero rows");

    auto cap_manifest = fixture.manifest;
    cap_manifest.effective_sq_end = 15;
    cap_manifest.worker_count = 1;
    cap_manifest.chunks = {
        {0, cap_manifest.effective_sq_begin, cap_manifest.effective_sq_end, "cap_worker"}};
    cap_manifest = reseal(std::move(cap_manifest));

    auto cap_attempt = fixture.attempt;
    cap_attempt.manifest_digest = cap_manifest.self_digest;
    cap_attempt.sq_begin = cap_manifest.effective_sq_begin;
    cap_attempt.sq_end = cap_manifest.effective_sq_end;
    cap_attempt.predecessor_digest = cap_manifest.self_digest;
    cap_attempt.lease.relative_stem = "cap_worker_attempt_00";
    cap_attempt = reseal(std::move(cap_attempt));

    const auto projection_for = [](const sieve::WorkerHandoffV1& handoff) {
        sieve::TerminalChunkInputV1 projection;
        projection.chunk_id = handoff.chunk_id;
        projection.disposition = sieve::ChunkDispositionV1::handoff;
        projection.sq_begin = handoff.sq_begin;
        projection.sq_end = handoff.sq_end;
        projection.next_sq_index = handoff.next_sq_index;
        projection.processed_sq_count = handoff.processed_sq_count;
        projection.completion_reason = handoff.completion_reason;
        projection.durable_attempt_count = handoff.attempt_ordinal + 1;
        projection.last_attempt_digest = handoff.attempt_started_digest;
        projection.lease_id = handoff.lease.lease_id;
        projection.handoff_digest = handoff.self_digest;
        projection.raw_relation_count = handoff.relation_count;
        projection.sequence_receipt = handoff.artifact.sequence_receipt;
        projection.corpus_sha256 = handoff.artifact.corpus_sha256;
        return projection;
    };

    auto exact_sq_cap = fixture.handoff;
    exact_sq_cap.manifest_digest = cap_manifest.self_digest;
    exact_sq_cap.sq_begin = cap_manifest.effective_sq_begin;
    exact_sq_cap.sq_end = cap_manifest.effective_sq_end;
    exact_sq_cap.attempt_started_digest = cap_attempt.self_digest;
    exact_sq_cap.lease = cap_attempt.lease;
    exact_sq_cap.artifact = corpus_artifact(5100, 3);
    exact_sq_cap.processed_sq_count = cap_manifest.sq_cap_per_worker;
    exact_sq_cap.next_sq_index =
        exact_sq_cap.sq_begin + static_cast<uint32_t>(exact_sq_cap.processed_sq_count) + 1;
    exact_sq_cap.completion_reason = sieve::WorkerCompletionReasonV1::sq_cap;
    exact_sq_cap.relation_count = 3;
    exact_sq_cap = reseal(std::move(exact_sq_cap));
    const std::array cap_attempts = {cap_attempt};
    require_ok(sieve::validate_terminal_chunk_projection(
                   cap_manifest, 0, cap_attempts, &exact_sq_cap, projection_for(exact_sq_cap)),
               "SQ cap uses exact processed count despite a skipped index");

    auto short_of_sq_cap = exact_sq_cap;
    --short_of_sq_cap.processed_sq_count;
    short_of_sq_cap = reseal(std::move(short_of_sq_cap));
    require_failed(sieve::validate_terminal_chunk_projection(cap_manifest, 0, cap_attempts,
                                                             &short_of_sq_cap,
                                                             projection_for(short_of_sq_cap)),
                   "SQ-cap completion requires the exact configured processed count");

    auto relation_cap_after_sq_cap = exact_sq_cap;
    relation_cap_after_sq_cap.artifact =
        corpus_artifact(5200, cap_manifest.relation_cap_per_worker);
    relation_cap_after_sq_cap.completion_reason = sieve::WorkerCompletionReasonV1::relation_cap;
    relation_cap_after_sq_cap.relation_count = cap_manifest.relation_cap_per_worker;
    relation_cap_after_sq_cap = reseal(std::move(relation_cap_after_sq_cap));
    require_failed(sieve::validate_terminal_chunk_projection(
                       cap_manifest, 0, cap_attempts, &relation_cap_after_sq_cap,
                       projection_for(relation_cap_after_sq_cap)),
                   "SQ cap has priority when both completion caps are reached");

    auto relation_cap_before_sq_cap = relation_cap_after_sq_cap;
    --relation_cap_before_sq_cap.processed_sq_count;
    relation_cap_before_sq_cap = reseal(std::move(relation_cap_before_sq_cap));
    require_ok(sieve::validate_terminal_chunk_projection(
                   cap_manifest, 0, cap_attempts, &relation_cap_before_sq_cap,
                   projection_for(relation_cap_before_sq_cap)),
               "relation cap wins before the SQ cap is reached");
}

void test_terminal_failure_reason_normalization() {
    ProtocolFixture fixture;

    auto spawn_failed = fixture.terminal_failure;
    spawn_failed.reason = sieve::ChunkTerminalFailureReasonV1::spawn_failed;
    spawn_failed.wait_facts = {};
    spawn_failed = reseal(std::move(spawn_failed));
    require_ok(validate_value(spawn_failed), "spawn failure uses unavailable wait facts");

    require_ok(validate_value(fixture.terminal_failure),
               "attempt-budget exhaustion uses unavailable wait facts");

    auto invalid_handoff_without_wait = fixture.terminal_failure;
    invalid_handoff_without_wait.reason = sieve::ChunkTerminalFailureReasonV1::invalid_handoff;
    invalid_handoff_without_wait = reseal(std::move(invalid_handoff_without_wait));
    require_ok(validate_value(invalid_handoff_without_wait),
               "invalid handoff permits unavailable wait facts");

    auto invalid_handoff_after_clean_exit = invalid_handoff_without_wait;
    invalid_handoff_after_clean_exit.wait_facts.kind = sieve::WorkerWaitFactKindV1::exited;
    invalid_handoff_after_clean_exit.wait_facts.exit_code = 0;
    invalid_handoff_after_clean_exit = reseal(std::move(invalid_handoff_after_clean_exit));
    require_ok(validate_value(invalid_handoff_after_clean_exit),
               "invalid handoff permits clean worker exit");

    auto exited_unsuccessfully = fixture.terminal_failure;
    exited_unsuccessfully.reason = sieve::ChunkTerminalFailureReasonV1::exited_unsuccessfully;
    exited_unsuccessfully.wait_facts.kind = sieve::WorkerWaitFactKindV1::exited;
    exited_unsuccessfully.wait_facts.exit_code = 7;
    exited_unsuccessfully = reseal(std::move(exited_unsuccessfully));
    require_ok(validate_value(exited_unsuccessfully),
               "unsuccessful exit binds nonzero normalized exit code");

    auto clean_exit_is_not_unsuccessful = exited_unsuccessfully;
    clean_exit_is_not_unsuccessful.wait_facts.exit_code = 0;
    require_reseal_failed(std::move(clean_exit_is_not_unsuccessful),
                          "clean exit cannot be resealed as unsuccessful exit");

    auto spawn_with_exit = spawn_failed;
    spawn_with_exit.wait_facts.kind = sieve::WorkerWaitFactKindV1::exited;
    spawn_with_exit.wait_facts.exit_code = 0;
    require_reseal_failed(std::move(spawn_with_exit),
                          "spawn failure cannot be resealed with exit facts");

    auto budget_with_exit = fixture.terminal_failure;
    budget_with_exit.wait_facts.kind = sieve::WorkerWaitFactKindV1::exited;
    budget_with_exit.wait_facts.exit_code = 0;
    require_reseal_failed(std::move(budget_with_exit),
                          "attempt-budget exhaustion cannot be resealed with exit facts");

    auto invalid_handoff_with_failed_exit = invalid_handoff_without_wait;
    invalid_handoff_with_failed_exit.wait_facts.kind = sieve::WorkerWaitFactKindV1::exited;
    invalid_handoff_with_failed_exit.wait_facts.exit_code = 1;
    require_reseal_failed(std::move(invalid_handoff_with_failed_exit),
                          "invalid handoff cannot absorb unsuccessful exit");

    auto out_of_range_exit = exited_unsuccessfully;
    out_of_range_exit.wait_facts.exit_code = 256;
    require_reseal_failed(std::move(out_of_range_exit), "exit facts reject nonportable exit code");

    auto signaled = fixture.terminal_failure;
    signaled.reason = sieve::ChunkTerminalFailureReasonV1::signaled;
    signaled.wait_facts.kind = sieve::WorkerWaitFactKindV1::signaled;
    signaled.wait_facts.signal = 1;
    signaled = reseal(std::move(signaled));
    require_ok(validate_value(signaled), "signal one is portable");

    auto maximum_signal = signaled;
    maximum_signal.wait_facts.signal = 255;
    maximum_signal = reseal(std::move(maximum_signal));
    require_ok(validate_value(maximum_signal), "signal 255 is portable");

    auto zero_signal = signaled;
    zero_signal.wait_facts.signal = 0;
    require_reseal_failed(std::move(zero_signal), "signal zero is not a signaled wait fact");

    auto out_of_range_signal = signaled;
    out_of_range_signal.wait_facts.signal = 256;
    require_reseal_failed(std::move(out_of_range_signal),
                          "signal facts reject nonportable signal code");

    const auto require_diagnostic_ok = [&](sieve::NormalizedDiagnosticKindV1 kind, uint32_t code,
                                           std::string_view context) {
        auto commit = fixture.merge_commit;
        commit.chunks.front().diagnostic = {kind, code};
        commit = reseal(std::move(commit));
        require_ok(validate_value(commit), context);
    };
    const auto require_diagnostic_bad = [&](sieve::NormalizedDiagnosticKindV1 kind, uint32_t code,
                                            std::string_view context) {
        auto commit = fixture.merge_commit;
        commit.chunks.front().diagnostic = {kind, code};
        require_reseal_failed(std::move(commit), context);
    };

    for (const auto kind : {sieve::NormalizedDiagnosticKindV1::none,
                            sieve::NormalizedDiagnosticKindV1::recovered_handoff,
                            sieve::NormalizedDiagnosticKindV1::retried_after_invalid_handoff}) {
        require_diagnostic_ok(kind, 0, "non-process diagnostic has canonical zero code");
        require_diagnostic_bad(kind, 1, "non-process diagnostic rejects nonzero code");
    }
    for (const auto kind : {sieve::NormalizedDiagnosticKindV1::retried_after_exit,
                            sieve::NormalizedDiagnosticKindV1::retried_after_signal}) {
        require_diagnostic_ok(kind, 1, "process diagnostic accepts code one");
        require_diagnostic_ok(kind, 255, "process diagnostic accepts code 255");
        require_diagnostic_bad(kind, 0, "process diagnostic rejects zero code");
        require_diagnostic_bad(kind, 256, "process diagnostic rejects code above 255");
        require_diagnostic_bad(kind, std::numeric_limits<uint32_t>::max(),
                               "process diagnostic rejects uint32 maximum");
    }
}

void test_execution_policy_closed_inventory_and_field_drift() {
    auto identity = make_work_identity();
    const Digest baseline = work_digest_or_fail(identity);
    CHECK(identity.execution_policy.settings.size() == POLICY_SETTINGS.size());

    for (std::size_t index = 0; index < POLICY_SETTINGS.size(); ++index) {
        auto changed = identity;
        changed.execution_policy.settings[index].canonical_bits = POLICY_SETTINGS[index].alternate;
        require_ok(sieve::validate_distributed_sieve_execution_policy(changed.execution_policy),
                   "policy field alternate");
        require_ok(sieve::validate_distributed_sieve_work_identity(changed),
                   "work identity after policy field drift");
        CHECK(work_digest_or_fail(changed) != baseline);
    }

    auto disabled_degree = identity;
    disabled_degree.execution_policy.settings[10].canonical_bits = 0;
    require_ok(sieve::validate_distributed_sieve_execution_policy(disabled_degree.execution_policy),
               "ECM Brent-Suyama degree disabled boundary");
    CHECK(work_digest_or_fail(disabled_degree) != baseline);

    auto disabled_ecore = identity;
    disabled_ecore.execution_policy.settings[25].canonical_bits = 0;
    require_ok(sieve::validate_distributed_sieve_execution_policy(disabled_ecore.execution_policy),
               "sieve E-core threads disabled boundary");
    CHECK(work_digest_or_fail(disabled_ecore) != baseline);

    auto all_unset = identity;
    for (std::size_t index = 0; index < ALL_UNSET_POLICY_BITS.size(); ++index) {
        all_unset.execution_policy.settings[index].canonical_bits = ALL_UNSET_POLICY_BITS[index];
    }
    require_ok(sieve::validate_distributed_sieve_execution_policy(all_unset.execution_policy),
               "all-unset normalized execution policy");
    require_ok(sieve::validate_distributed_sieve_work_identity(all_unset),
               "work identity with all-unset policy");
    CHECK(work_digest_or_fail(all_unset) != baseline);

    auto invalid = identity.execution_policy;
    invalid.settings.pop_back();
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "missing policy field");

    invalid = identity.execution_policy;
    invalid.settings.push_back(invalid.settings.back());
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "extra policy field");

    invalid = identity.execution_policy;
    invalid.settings[1].key = invalid.settings[0].key;
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "duplicate policy key");

    invalid = identity.execution_policy;
    std::swap(invalid.settings[0], invalid.settings[1]);
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "noncanonical policy order");

    invalid = identity.execution_policy;
    invalid.settings[0].kind = sieve::ExecutionPolicyScalarKindV1::unsigned_integer;
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "wrong policy scalar kind");

    invalid = identity.execution_policy;
    invalid.settings[1].canonical_bits = 2;
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "noncanonical policy boolean");

    invalid = identity.execution_policy;
    invalid.settings[0].canonical_bits = 0;
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "noncanonical closed mode");

    invalid = identity.execution_policy;
    invalid.settings[3].canonical_bits = binary64_bits(std::numeric_limits<double>::infinity());
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "non-finite policy binary64");

    invalid = identity.execution_policy;
    invalid.schema_version = sieve::DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1 + 1;
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "unsupported policy schema");
}

struct WorkMutation final {
    std::string_view name;
    std::function<void(sieve::DistributedSieveWorkIdentityV1&)> apply;
    bool remains_valid = true;
};

void test_work_identity_field_drift_and_canonical_integers() {
    const auto baseline_identity = make_work_identity();
    const Digest baseline_digest = work_digest_or_fail(baseline_identity);

    const std::vector<WorkMutation> mutations = {
        {"polynomial n", [](auto& value) { value.polynomial.n.decimal = "1000036000101"; }},
        {"polynomial m", [](auto& value) { value.polynomial.m.decimal = "10003"; }},
        {"polynomial degree/count",
         [](auto& value) {
             value.polynomial.degree = 3;
             value.polynomial.coefficients.push_back({"2"});
         }},
        {"polynomial coefficient",
         [](auto& value) { value.polynomial.coefficients[0].decimal = "-7"; }},
        {"polynomial skew",
         [](auto& value) { value.polynomial.skewness_ieee754_bits = binary64_bits(1.5); }},
        {"rational bound", [](auto& value) { ++value.factor_base.rational_bound; }},
        {"algebraic bound", [](auto& value) { ++value.factor_base.algebraic_bound; }},
        {"factor-base LP bound", [](auto& value) { ++value.factor_base.large_prime_bound; }},
        {"factor-base log scale", [](auto& value) { ++value.factor_base.log_scale; }},
        {"rational prime", [](auto& value) { value.factor_base.rational[0].p = 3; }},
        {"rational log", [](auto& value) { ++value.factor_base.rational[0].log_p; }},
        {"rational entry count",
         [](auto& value) { value.factor_base.rational.push_back({7, 31}); }},
        {"algebraic prime", [](auto& value) { value.factor_base.algebraic[0].p = 5; }},
        {"algebraic root", [](auto& value) { ++value.factor_base.algebraic[0].r; }},
        {"algebraic log", [](auto& value) { ++value.factor_base.algebraic[0].log_p; }},
        {"algebraic degree", [](auto& value) { ++value.factor_base.algebraic[0].degree; }},
        {"algebraic entry count",
         [](auto& value) { value.factor_base.algebraic.push_back({229, 7, 73, 1}); }},
        {"sieve algebraic count",
         [](auto& value) {
             value.factor_base.sieve_algebraic_count = 1;
             value.factor_base.algebraic = {
                 {7, 1, 37, 1}, {211, 4, 55, 2}, {223, 3, 61, 1}, {227, 2, 67, 1}, {229, 5, 71, 1},
             };
         },
         false},
        {"sieve log scale", [](auto& value) { ++value.sieve.log_scale; }},
        {"rational threshold", [](auto& value) { ++value.sieve.rational_threshold; }},
        {"algebraic threshold", [](auto& value) { ++value.sieve.algebraic_threshold; }},
        {"sieve LP bound", [](auto& value) { ++value.sieve.large_prime_bound; }},
        {"allow 2LP", [](auto& value) { value.sieve.allow_2lp = false; }},
        {"allow 3LP", [](auto& value) { value.sieve.allow_3lp = true; }},
        {"region i min", [](auto& value) { --value.region.i_min; }},
        {"region i max", [](auto& value) { ++value.region.i_max; }},
        {"region j min", [](auto& value) { ++value.region.j_min; }},
        {"region j max", [](auto& value) { ++value.region.j_max; }},
        {"cofactor LP bound", [](auto& value) { ++value.cofactor.large_prime_bound; }},
        {"cofactor allow 1LP", [](auto& value) { value.cofactor.allow_1lp = false; }},
        {"cofactor allow 2LP", [](auto& value) { value.cofactor.allow_2lp = false; }},
        {"cofactor allow 3LP", [](auto& value) { value.cofactor.allow_3lp = false; }},
        {"cofactor attempts", [](auto& value) { ++value.cofactor.max_factorization_attempts; }},
        {"original SQ start", [](auto& value) { ++value.original_sq_bounds.start_index; }},
        {"original SQ end", [](auto& value) { ++value.original_sq_bounds.end_index; }},
        {"original min q", [](auto& value) { --value.original_sq_bounds.min_q; }},
        {"original max q", [](auto& value) { ++value.original_sq_bounds.max_q; }},
        {"effective SQ start",
         [](auto& value) {
             --value.effective_sq_bounds.start_index;
             --value.distributed.chunks.front().sq_begin;
         },
         false},
        {"effective SQ end",
         [](auto& value) {
             ++value.effective_sq_bounds.end_index;
             ++value.distributed.chunks.back().sq_end;
         },
         false},
        {"effective min q", [](auto& value) { ++value.effective_sq_bounds.min_q; }, false},
        {"effective max q", [](auto& value) { ++value.effective_sq_bounds.max_q; }, false},
        {"worker count/chunk table",
         [](auto& value) {
             value.distributed.worker_count = 3;
             value.distributed.chunks = {
                 {0, 2, 3, "chunk_0"},
                 {1, 3, 4, "chunk_1"},
                 {2, 4, 5, "chunk_2"},
             };
         }},
        {"chunk split",
         [](auto& value) {
             value.distributed.chunks[0].sq_end = 4;
             value.distributed.chunks[1].sq_begin = 4;
         }},
        {"chunk artifact stem",
         [](auto& value) { value.distributed.chunks[0].relative_artifact_stem = "chunk_zero"; }},
        {"SQ cap", [](auto& value) { ++value.distributed.sq_cap_per_worker; }},
        {"relation cap", [](auto& value) { ++value.distributed.relation_cap_per_worker; }},
        {"worker budget", [](auto& value) { ++value.distributed.max_worker_attempts; }},
        {"merge budget", [](auto& value) { ++value.distributed.max_merge_build_attempts; }},
        {"consumption budget", [](auto& value) { ++value.distributed.max_consumption_attempts; }},
        {"relation serialization version",
         [](auto& value) { ++value.semantic_versions.relation_serialization_version; }},
        {"OOC version", [](auto& value) { ++value.semantic_versions.ooc_format_version; }},
        {"digest version", [](auto& value) { ++value.semantic_versions.digest_version; }},
        {"handoff version", [](auto& value) { ++value.semantic_versions.handoff_version; }},
        {"retry version", [](auto& value) { ++value.semantic_versions.retry_policy_version; }},
        {"chunking version", [](auto& value) { ++value.semantic_versions.chunking_version; }},
        {"completion version", [](auto& value) { ++value.semantic_versions.completion_version; }},
        {"deduplication version",
         [](auto& value) { ++value.semantic_versions.deduplication_version; }},
        {"merge policy version",
         [](auto& value) { ++value.semantic_versions.merge_policy_version; }},
    };

    for (const auto& mutation : mutations) {
        auto changed = baseline_identity;
        mutation.apply(changed);
        const auto status = sieve::validate_distributed_sieve_work_identity(changed);
        if (!mutation.remains_valid) {
            require_failed(status, mutation.name);
            continue;
        }
        if (!status) {
            fail(mutation.name, __LINE__,
                 sieve::distributed_sieve_protocol_error_name(status.error));
        }
        CHECK(work_digest_or_fail(changed) != baseline_digest);
    }

    CHECK(baseline_identity.original_sq_bounds.start_index == 0);
    CHECK(baseline_identity.original_sq_bounds.end_index == 5);
    CHECK(baseline_identity.original_sq_bounds.min_q == 100);
    CHECK(baseline_identity.original_sq_bounds.max_q == 1000);
    CHECK(baseline_identity.effective_sq_bounds.start_index == 2);
    CHECK(baseline_identity.effective_sq_bounds.end_index == 5);
    CHECK(baseline_identity.effective_sq_bounds.min_q == 0);
    CHECK(baseline_identity.effective_sq_bounds.max_q == std::numeric_limits<uint32_t>::max());

    auto containment_only_narrowing = baseline_identity;
    containment_only_narrowing.effective_sq_bounds.start_index = 3;
    containment_only_narrowing.effective_sq_bounds.end_index = 4;
    containment_only_narrowing.distributed.chunks = {
        {0, 3, 4, "chunk_0"},
        {1, 4, 4, "chunk_1"},
    };
    require_failed(sieve::validate_distributed_sieve_work_identity(containment_only_narrowing),
                   "effective SQ range cannot be an arbitrary contained subset");

    auto wrong_effective_begin = baseline_identity;
    wrong_effective_begin.effective_sq_bounds.start_index = 1;
    wrong_effective_begin.distributed.chunks.front().sq_begin = 1;
    require_failed(sieve::validate_distributed_sieve_work_identity(wrong_effective_begin),
                   "effective SQ begin must be exactly recomputed");

    auto wrong_effective_end = baseline_identity;
    wrong_effective_end.effective_sq_bounds.end_index = 6;
    wrong_effective_end.distributed.chunks.back().sq_end = 6;
    require_failed(sieve::validate_distributed_sieve_work_identity(wrong_effective_end),
                   "effective SQ end must be exactly recomputed");

    auto effective_min_is_not_index_only = baseline_identity;
    effective_min_is_not_index_only.effective_sq_bounds.min_q = 100;
    require_failed(sieve::validate_distributed_sieve_work_identity(effective_min_is_not_index_only),
                   "effective SQ minimum must use the index-only zero sentinel");

    auto effective_max_is_not_index_only = baseline_identity;
    effective_max_is_not_index_only.effective_sq_bounds.max_q = 1000;
    require_failed(sieve::validate_distributed_sieve_work_identity(effective_max_is_not_index_only),
                   "effective SQ maximum must use the index-only uint32 sentinel");

    for (const std::string invalid_integer : {"", "+1", "01", "-0", " 1", "1 ", "--1", "1x"}) {
        auto invalid = baseline_identity;
        invalid.polynomial.n.decimal = invalid_integer;
        require_failed(sieve::validate_distributed_sieve_work_identity(invalid),
                       "noncanonical integer");
    }

    auto invalid = baseline_identity;
    invalid.polynomial.n.decimal.assign(
        static_cast<std::size_t>(sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CANONICAL_INTEGER_BYTES) + 1,
        '1');
    require_failed(sieve::validate_distributed_sieve_work_identity(invalid),
                   "canonical integer length limit");

    invalid = baseline_identity;
    invalid.polynomial.degree = 3;
    require_failed(sieve::validate_distributed_sieve_work_identity(invalid),
                   "polynomial degree/coefficient mismatch");

    auto zero_padded_polynomial = baseline_identity;
    zero_padded_polynomial.polynomial.coefficients.push_back({"0"});
    zero_padded_polynomial.polynomial.coefficients.push_back({"0"});
    require_ok(sieve::validate_distributed_sieve_work_identity(zero_padded_polynomial),
               "canonical zero-padded polynomial live shape");
    CHECK(work_digest_or_fail(zero_padded_polynomial) != baseline_digest);

    auto maximum_zero_padding = baseline_identity;
    maximum_zero_padding.polynomial.coefficients.resize(
        sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_COEFFICIENTS, sieve::CanonicalIntegerV1{"0"});
    require_ok(sieve::validate_distributed_sieve_work_identity(maximum_zero_padding),
               "maximum polynomial coefficient count");
    CHECK(work_digest_or_fail(maximum_zero_padding) != baseline_digest);

    auto nonzero_padding = zero_padded_polynomial;
    nonzero_padding.polynomial.coefficients.back().decimal = "1";
    require_failed(sieve::validate_distributed_sieve_work_identity(nonzero_padding),
                   "nonzero polynomial tail rejected");

    auto noncanonical_padding = zero_padded_polynomial;
    noncanonical_padding.polynomial.coefficients.back().decimal = "00";
    require_failed(sieve::validate_distributed_sieve_work_identity(noncanonical_padding),
                   "noncanonical polynomial zero tail rejected");

    auto insufficient_live_coefficients = baseline_identity;
    insufficient_live_coefficients.polynomial.coefficients.resize(
        insufficient_live_coefficients.polynomial.degree);
    require_failed(sieve::validate_distributed_sieve_work_identity(insufficient_live_coefficients),
                   "polynomial requires degree plus one live coefficients");

    auto zero_leading_coefficient = baseline_identity;
    zero_leading_coefficient.polynomial.coefficients[zero_leading_coefficient.polynomial.degree]
        .decimal = "0";
    require_failed(sieve::validate_distributed_sieve_work_identity(zero_leading_coefficient),
                   "polynomial leading live coefficient is nonzero");

    auto too_many_coefficients = maximum_zero_padding;
    too_many_coefficients.polynomial.coefficients.push_back({"0"});
    require_failed(sieve::validate_distributed_sieve_work_identity(too_many_coefficients),
                   "polynomial coefficient count limit");

    auto degree_zero_constant = baseline_identity;
    degree_zero_constant.polynomial.degree = 0;
    degree_zero_constant.polynomial.coefficients = {{"7"}, {"0"}};
    require_ok(sieve::validate_distributed_sieve_work_identity(degree_zero_constant),
               "nonzero degree-zero polynomial with zero padding");
    auto all_zero_constant = degree_zero_constant;
    all_zero_constant.polynomial.coefficients.front().decimal = "0";
    require_failed(sieve::validate_distributed_sieve_work_identity(all_zero_constant),
                   "all-zero degree-zero polynomial rejected");

    invalid = baseline_identity;
    std::swap(invalid.distributed.chunks[0], invalid.distributed.chunks[1]);
    require_failed(sieve::validate_distributed_sieve_work_identity(invalid),
                   "work chunk noncanonical order");

    invalid = baseline_identity;
    invalid.factor_base.rational[0].p = 1;
    require_failed(sieve::validate_distributed_sieve_work_identity(invalid),
                   "rational factor-base p=1 rejected");

    invalid = baseline_identity;
    invalid.factor_base.algebraic[0].p = 1;
    invalid.factor_base.algebraic[0].r = 0;
    require_failed(sieve::validate_distributed_sieve_work_identity(invalid),
                   "algebraic factor-base p=1 rejected");

    auto actual_factor_base_shape = baseline_identity;
    actual_factor_base_shape.factor_base.algebraic = {
        {7, 1, 37, 1},   {7, std::numeric_limits<uint32_t>::max(), 38, 1},
        {211, 4, 55, 2}, {223, 5, 61, 1},
        {227, 6, 67, 1},
    };
    actual_factor_base_shape.factor_base.sieve_algebraic_count = 2;
    require_ok(sieve::validate_distributed_sieve_work_identity(actual_factor_base_shape),
               "projective root and special-Q extension factor-base shape");
    CHECK(work_digest_or_fail(actual_factor_base_shape) != baseline_digest);

    auto out_of_bound_sieve_prefix = actual_factor_base_shape;
    out_of_bound_sieve_prefix.factor_base.algebraic = {
        {211, 1, 37, 1}, {211, std::numeric_limits<uint32_t>::max(), 38, 1},
        {223, 4, 55, 2}, {227, 5, 61, 1},
        {229, 6, 67, 1},
    };
    require_failed(sieve::validate_distributed_sieve_work_identity(out_of_bound_sieve_prefix),
                   "sieve algebraic prefix must stay within bound");

    auto noncanonical_factor_base_order = actual_factor_base_shape;
    std::swap(noncanonical_factor_base_order.factor_base.algebraic[0],
              noncanonical_factor_base_order.factor_base.algebraic[1]);
    require_failed(sieve::validate_distributed_sieve_work_identity(noncanonical_factor_base_order),
                   "full algebraic factor-base order remains canonical");

    auto sieve_bound_fallback = baseline_identity;
    sieve_bound_fallback.sieve.large_prime_bound = 0;
    require_ok(sieve::validate_distributed_sieve_work_identity(sieve_bound_fallback),
               "zero sieve large-prime bound fallback");
    CHECK(work_digest_or_fail(sieve_bound_fallback) != baseline_digest);

    auto cofactor_bound_fallback = baseline_identity;
    cofactor_bound_fallback.cofactor.large_prime_bound = 0;
    require_ok(sieve::validate_distributed_sieve_work_identity(cofactor_bound_fallback),
               "zero cofactor large-prime bound fallback");
    CHECK(work_digest_or_fail(cofactor_bound_fallback) != baseline_digest);

    auto default_factorization_attempts = baseline_identity;
    default_factorization_attempts.cofactor.max_factorization_attempts = 0;
    require_ok(sieve::validate_distributed_sieve_work_identity(default_factorization_attempts),
               "zero factorization attempts sentinel");
    CHECK(work_digest_or_fail(default_factorization_attempts) != baseline_digest);

    std::vector<Digest> sieve_flag_digests;
    for (uint32_t mask = 0; mask < 4; ++mask) {
        auto combination = baseline_identity;
        combination.sieve.allow_2lp = (mask & 1U) != 0;
        combination.sieve.allow_3lp = (mask & 2U) != 0;
        require_ok(sieve::validate_distributed_sieve_work_identity(combination),
                   "independent sieve LP flags");
        const Digest digest = work_digest_or_fail(combination);
        CHECK(std::find(sieve_flag_digests.begin(), sieve_flag_digests.end(), digest) ==
              sieve_flag_digests.end());
        sieve_flag_digests.push_back(digest);
    }

    std::vector<Digest> cofactor_flag_digests;
    for (uint32_t mask = 0; mask < 8; ++mask) {
        auto combination = baseline_identity;
        combination.cofactor.allow_1lp = (mask & 1U) != 0;
        combination.cofactor.allow_2lp = (mask & 2U) != 0;
        combination.cofactor.allow_3lp = (mask & 4U) != 0;
        require_ok(sieve::validate_distributed_sieve_work_identity(combination),
                   "independent cofactor LP flags");
        const Digest digest = work_digest_or_fail(combination);
        CHECK(std::find(cofactor_flag_digests.begin(), cofactor_flag_digests.end(), digest) ==
              cofactor_flag_digests.end());
        cofactor_flag_digests.push_back(digest);
    }
}

void test_manifest_work_identity_binding() {
    ProtocolFixture fixture;
    require_ok(sieve::validate_manifest_work_identity(fixture.manifest, fixture.work_identity),
               "manifest binds canonical work identity");

    auto wrong_work_digest = fixture.manifest;
    perturb_digest(wrong_work_digest.work_sha256);
    wrong_work_digest = reseal(std::move(wrong_work_digest));
    require_failed(sieve::validate_manifest_work_identity(wrong_work_digest, fixture.work_identity),
                   "manifest rejects wrong work digest");

    const std::vector<WorkMutation> denormalized_field_mutations = {
        {"manifest effective SQ begin",
         [](auto& value) {
             --value.effective_sq_bounds.start_index;
             --value.distributed.chunks.front().sq_begin;
         },
         false},
        {"manifest effective SQ end",
         [](auto& value) {
             ++value.effective_sq_bounds.end_index;
             ++value.distributed.chunks.back().sq_end;
         },
         false},
        {"manifest worker count and chunks",
         [](auto& value) {
             value.distributed.worker_count = 3;
             value.distributed.chunks = {
                 {0, 2, 3, "chunk_0"},
                 {1, 3, 4, "chunk_1"},
                 {2, 4, 5, "chunk_2"},
             };
         }},
        {"manifest chunk split",
         [](auto& value) {
             value.distributed.chunks[0].sq_end = 4;
             value.distributed.chunks[1].sq_begin = 4;
         }},
        {"manifest chunk artifact stem",
         [](auto& value) { value.distributed.chunks[0].relative_artifact_stem = "chunk_zero"; }},
        {"manifest SQ cap", [](auto& value) { ++value.distributed.sq_cap_per_worker; }},
        {"manifest relation cap", [](auto& value) { ++value.distributed.relation_cap_per_worker; }},
        {"manifest worker budget", [](auto& value) { ++value.distributed.max_worker_attempts; }},
        {"manifest merge budget",
         [](auto& value) { ++value.distributed.max_merge_build_attempts; }},
        {"manifest consumption budget",
         [](auto& value) { ++value.distributed.max_consumption_attempts; }},
        {"manifest OOC version", [](auto& value) { ++value.semantic_versions.ooc_format_version; }},
        {"manifest relation serialization version",
         [](auto& value) { ++value.semantic_versions.relation_serialization_version; }},
        {"manifest handoff version",
         [](auto& value) { ++value.semantic_versions.handoff_version; }},
        {"manifest digest version", [](auto& value) { ++value.semantic_versions.digest_version; }},
        {"manifest retry version",
         [](auto& value) { ++value.semantic_versions.retry_policy_version; }},
        {"manifest merge policy version",
         [](auto& value) { ++value.semantic_versions.merge_policy_version; }},
    };

    for (const auto& mutation : denormalized_field_mutations) {
        auto changed_identity = fixture.work_identity;
        mutation.apply(changed_identity);
        if (!mutation.remains_valid) {
            require_failed(sieve::validate_distributed_sieve_work_identity(changed_identity),
                           mutation.name);
            continue;
        }
        require_ok(sieve::validate_distributed_sieve_work_identity(changed_identity),
                   mutation.name);

        auto stale_manifest = fixture.manifest;
        stale_manifest.work_sha256 = work_digest_or_fail(changed_identity);
        stale_manifest = reseal(std::move(stale_manifest));
        const auto status =
            sieve::validate_manifest_work_identity(stale_manifest, changed_identity);
        if (status) {
            fail(mutation.name, __LINE__, "stale denormalized manifest field accepted");
        }
    }

    const std::vector<WorkMutation> digest_only_version_mutations = {
        {"chunking version", [](auto& value) { ++value.semantic_versions.chunking_version; }},
        {"completion version", [](auto& value) { ++value.semantic_versions.completion_version; }},
        {"deduplication version",
         [](auto& value) { ++value.semantic_versions.deduplication_version; }},
    };
    for (const auto& mutation : digest_only_version_mutations) {
        auto changed_identity = fixture.work_identity;
        mutation.apply(changed_identity);
        require_ok(sieve::validate_distributed_sieve_work_identity(changed_identity),
                   mutation.name);
        require_failed(sieve::validate_manifest_work_identity(fixture.manifest, changed_identity),
                       mutation.name);
    }
}

[[nodiscard]] Digest seed_or_fail(const sieve::DeterministicRandomSeedRequestV1& request) {
    const auto result = sieve::derive_distributed_sieve_deterministic_seed(request);
    if (!result) {
        fail("derive_distributed_sieve_deterministic_seed", __LINE__,
             sieve::distributed_sieve_protocol_error_name(result.status.error));
    }
    return *result.digest;
}

void test_deterministic_seed_domain_and_input_drift() {
    sieve::DeterministicRandomSeedRequestV1 request;
    request.work_digest = work_digest_or_fail(make_work_identity());
    request.domain = sieve::DeterministicRandomDomainV1::adaptive_lattice;
    request.chunk_id = 3;
    request.sq_index = 7;
    request.candidate_ordinal = 11;
    request.algorithm_identity = 13;
    request.cofactor_input_digest = digest_with_seed(90);

    const Digest baseline = seed_or_fail(request);
    CHECK(seed_or_fail(request) == baseline);

    constexpr std::array DOMAINS = {
        sieve::DeterministicRandomDomainV1::adaptive_lattice,
        sieve::DeterministicRandomDomainV1::ecm_sigma,
        sieve::DeterministicRandomDomainV1::ecm_curve,
        sieve::DeterministicRandomDomainV1::pollard_rho,
        sieve::DeterministicRandomDomainV1::cofactor_choice,
    };
    std::vector<Digest> domain_digests;
    for (const auto domain : DOMAINS) {
        auto changed = request;
        changed.domain = domain;
        const auto digest = seed_or_fail(changed);
        CHECK(std::find(domain_digests.begin(), domain_digests.end(), digest) ==
              domain_digests.end());
        domain_digests.push_back(digest);
    }

    auto expect_input_drift = [&](const auto& mutate) {
        auto changed = request;
        mutate(changed);
        CHECK(seed_or_fail(changed) != baseline);
    };
    expect_input_drift([](auto& value) { perturb_digest(value.work_digest); });
    expect_input_drift([](auto& value) { ++value.chunk_id; });
    expect_input_drift([](auto& value) { ++value.sq_index; });
    expect_input_drift([](auto& value) { ++value.candidate_ordinal; });
    expect_input_drift([](auto& value) { ++value.algorithm_identity; });
    expect_input_drift([](auto& value) { perturb_digest(value.cofactor_input_digest); });

    auto invalid = request;
    invalid.domain = static_cast<sieve::DeterministicRandomDomainV1>(0);
    const auto invalid_result = sieve::derive_distributed_sieve_deterministic_seed(invalid);
    CHECK(!invalid_result);
    CHECK(invalid_result.status.error == sieve::DistributedSieveProtocolError::unknown_enum);
}

void test_predecessor_and_dependency_closure() {
    ProtocolFixture fixture;

    const std::array worker_attempts = {fixture.attempt};
    require_ok(sieve::validate_worker_attempt_chain(fixture.manifest, 0, worker_attempts,
                                                    &fixture.handoff, nullptr),
               "valid worker handoff chain");

    const std::array<std::pair<std::string_view, sieve::NativeIdentityV1>, 2>
        manifest_control_identities = {{
            {"wave root", fixture.manifest.wave_root_identity},
            {"permanent lock", fixture.manifest.permanent_lock_identity},
        }};
    for (const auto& [control_name, control_identity] : manifest_control_identities) {
        for (const bool mutate_owner : {true, false}) {
            auto aliased_attempt = fixture.attempt;
            if (mutate_owner) {
                aliased_attempt.lease.owner_marker = control_identity;
            } else {
                aliased_attempt.lease.directory = control_identity;
            }
            aliased_attempt = reseal(std::move(aliased_attempt));
            const std::array aliased_attempts = {aliased_attempt};
            const std::string attempt_context = std::string("attempt lease ") +
                                                (mutate_owner ? "owner" : "directory") +
                                                " aliases " + std::string(control_name);
            require_failed(sieve::validate_worker_attempt_chain(fixture.manifest, 0,
                                                                aliased_attempts, nullptr, nullptr),
                           attempt_context);

            auto aliased_handoff = fixture.handoff;
            aliased_handoff.attempt_started_digest = aliased_attempt.self_digest;
            aliased_handoff.lease = aliased_attempt.lease;
            aliased_handoff = reseal(std::move(aliased_handoff));
            const std::string handoff_context = std::string("worker handoff lease ") +
                                                (mutate_owner ? "owner" : "directory") +
                                                " aliases " + std::string(control_name);
            require_failed(sieve::validate_worker_attempt_chain(
                               fixture.manifest, 0, aliased_attempts, &aliased_handoff, nullptr),
                           handoff_context);
        }
        for (const bool mutate_index : {true, false}) {
            auto aliased_handoff = fixture.handoff;
            if (mutate_index) {
                aliased_handoff.artifact.index_file.identity = control_identity;
            } else {
                aliased_handoff.artifact.data_file.identity = control_identity;
            }
            aliased_handoff = reseal(std::move(aliased_handoff));
            const std::string context = std::string("worker handoff artifact ") +
                                        (mutate_index ? "index" : "data") + " aliases " +
                                        std::string(control_name);
            require_failed(sieve::validate_worker_attempt_chain(
                               fixture.manifest, 0, worker_attempts, &aliased_handoff, nullptr),
                           context);
        }
    }

    auto wrong_worker_format = fixture.handoff;
    ++wrong_worker_format.artifact.descriptor.format_version;
    wrong_worker_format = reseal(std::move(wrong_worker_format));
    require_failed(sieve::validate_worker_attempt_chain(fixture.manifest, 0, worker_attempts,
                                                        &wrong_worker_format, nullptr),
                   "worker handoff format binds manifest OOC version");

    const std::array failed_attempts = {fixture.failure_attempt_0, fixture.failure_attempt_1};
    require_ok(sieve::validate_worker_attempt_chain(fixture.manifest, 1, failed_attempts, nullptr,
                                                    nullptr),
               "max-attempt crash prefix remains recoverable before lease cleanup");
    require_ok(sieve::validate_worker_attempt_chain(fixture.manifest, 1, failed_attempts, nullptr,
                                                    &fixture.terminal_failure),
               "terminalization is valid after exact lease cleanup");

    auto forbidden_attempt = fixture.failure_attempt_1;
    forbidden_attempt.attempt_ordinal = 2;
    forbidden_attempt.predecessor_digest = fixture.failure_attempt_1.self_digest;
    forbidden_attempt.lease = lease_identity(112, "chunk_1_attempt_02");
    forbidden_attempt = reseal(std::move(forbidden_attempt));
    const std::array attempts_beyond_budget = {
        fixture.failure_attempt_0,
        fixture.failure_attempt_1,
        forbidden_attempt,
    };
    require_failed(sieve::validate_worker_attempt_chain(fixture.manifest, 1, attempts_beyond_budget,
                                                        nullptr, nullptr),
                   "max-attempt crash prefix cannot start another attempt");

    require_failed(sieve::validate_worker_attempt_chain(fixture.manifest, 0, worker_attempts,
                                                        &fixture.handoff,
                                                        &fixture.terminal_failure),
                   "mutually exclusive worker terminals");

    auto wrong_attempt = fixture.attempt;
    wrong_attempt.attempt_ordinal = 1;
    wrong_attempt.predecessor_digest = fixture.attempt.self_digest;
    wrong_attempt = reseal(std::move(wrong_attempt));
    const std::array wrong_attempts = {wrong_attempt};
    require_failed(
        sieve::validate_worker_attempt_chain(fixture.manifest, 0, wrong_attempts, nullptr, nullptr),
        "worker ordinal gap");

    auto extra_attempt = fixture.attempt;
    extra_attempt.attempt_ordinal = 1;
    extra_attempt.predecessor_digest = fixture.attempt.self_digest;
    extra_attempt.lease = lease_identity(101, "chunk_0_attempt_01");
    extra_attempt = reseal(std::move(extra_attempt));
    const std::array attempts_after_terminal = {fixture.attempt, extra_attempt};
    require_failed(sieve::validate_worker_attempt_chain(
                       fixture.manifest, 0, attempts_after_terminal, &fixture.handoff, nullptr),
                   "worker handoff absorbs later attempt");

    const std::array merge_starts = {fixture.merge_started};
    require_ok(sieve::validate_merge_predecessor_chain(
                   fixture.manifest, merge_starts, &fixture.merge_prepared, &fixture.merge_commit),
               "valid merge chain");

    const std::array successful_attempts_1 = {fixture.failure_attempt_0};
    CHECK(fixture.work_identity.factor_base.algebraic[3].r == std::numeric_limits<uint32_t>::max());
    CHECK(fixture.handoff_1.processed_sq_count <
          static_cast<uint64_t>(fixture.handoff_1.next_sq_index - fixture.handoff_1.sq_begin));
    auto processed_beyond_index_span = fixture.handoff_1;
    processed_beyond_index_span.processed_sq_count =
        static_cast<uint64_t>(processed_beyond_index_span.next_sq_index -
                              processed_beyond_index_span.sq_begin) +
        1;
    require_reseal_failed(std::move(processed_beyond_index_span),
                          "processed special-Q count cannot exceed index advance");

    require_ok(sieve::validate_terminal_chunk_projection(fixture.manifest, 0, worker_attempts,
                                                         &fixture.handoff,
                                                         fixture.merge_started.ordered_inputs[0]),
               "chunk zero terminal projection binds exact handoff evidence");
    require_ok(sieve::validate_terminal_chunk_projection(fixture.manifest, 1, successful_attempts_1,
                                                         &fixture.handoff_1,
                                                         fixture.merge_started.ordered_inputs[1]),
               "chunk one terminal projection binds exact handoff evidence");

    auto high_limb_projection = fixture.merge_started.ordered_inputs[0];
    ++high_limb_projection.lease_id.limbs[1];
    require_failed(sieve::validate_terminal_chunk_projection(fixture.manifest, 0, worker_attempts,
                                                             &fixture.handoff,
                                                             high_limb_projection),
                   "terminal projection rejects lease high-limb drift");

    auto nil_handoff_projection = fixture.merge_started.ordered_inputs[0];
    nil_handoff_projection.lease_id = {};
    require_failed(sieve::validate_terminal_chunk_projection(fixture.manifest, 0, worker_attempts,
                                                             &fixture.handoff,
                                                             nil_handoff_projection),
                   "handoff projection rejects nil lease id");

    const std::array<sieve::ChunkTerminalEvidenceViewV1, 2> terminal_evidence = {{
        {std::span<const sieve::AttemptStartedV1>{worker_attempts}, &fixture.handoff},
        {std::span<const sieve::AttemptStartedV1>{successful_attempts_1}, &fixture.handoff_1},
    }};
    const std::span<const sieve::ConsumptionStartedV1> no_consumption_starts;
    require_ok(sieve::validate_merge_dependency_chain(fixture.manifest, terminal_evidence,
                                                      merge_starts, &fixture.merge_prepared,
                                                      &fixture.merge_commit),
               "valid merge dependency closure");
    require_ok(sieve::validate_artifact_cleanup_dependencies(
                   fixture.manifest, fixture.merge_commit, no_consumption_starts, nullptr, nullptr,
                   fixture.cleanup_authorizations[0], &fixture.handoff, nullptr),
               "validated merge evidence composes with worker cleanup mint");

    struct MergeAliasCase final {
        sieve::AttemptStartedV1 attempt_1;
        sieve::WorkerHandoffV1 handoff_1;
        sieve::MergeStartedV1 started;
        sieve::MergePreparedV1 prepared;
        sieve::WaveMergeCommitV1 commit;
    };
    using WorkerAliasMutation =
        std::function<void(sieve::AttemptStartedV1&, sieve::WorkerHandoffV1&)>;
    using MergeStartAliasMutation = std::function<void(sieve::MergeStartedV1&)>;
    using MergePreparedAliasMutation = std::function<void(sieve::MergePreparedV1&)>;
    const auto make_merge_alias_case = [&](const WorkerAliasMutation& mutate_worker,
                                           const MergeStartAliasMutation& mutate_start,
                                           const MergePreparedAliasMutation& mutate_prepared) {
        MergeAliasCase value;
        value.attempt_1 = fixture.failure_attempt_0;
        value.handoff_1 = fixture.handoff_1;
        if (mutate_worker) {
            mutate_worker(value.attempt_1, value.handoff_1);
        }
        value.attempt_1 = reseal(std::move(value.attempt_1));
        value.handoff_1.attempt_started_digest = value.attempt_1.self_digest;
        value.handoff_1.lease = value.attempt_1.lease;
        value.handoff_1 = reseal(std::move(value.handoff_1));

        auto projection = fixture.merge_started.ordered_inputs[1];
        projection.last_attempt_digest = value.attempt_1.self_digest;
        projection.lease_id = value.handoff_1.lease.lease_id;
        projection.handoff_digest = value.handoff_1.self_digest;
        projection.raw_relation_count = value.handoff_1.relation_count;
        projection.sequence_receipt = value.handoff_1.artifact.sequence_receipt;
        projection.corpus_sha256 = value.handoff_1.artifact.corpus_sha256;

        value.started = fixture.merge_started;
        value.started.ordered_inputs[1] = projection;
        if (mutate_start) {
            mutate_start(value.started);
        }
        value.started = reseal(std::move(value.started));

        value.prepared = fixture.merge_prepared;
        value.prepared.merge_started_digest = value.started.self_digest;
        value.prepared.ordered_inputs = value.started.ordered_inputs;
        value.prepared.merged_lease = value.started.merged_lease;
        if (mutate_prepared) {
            mutate_prepared(value.prepared);
        }
        value.prepared = reseal(std::move(value.prepared));

        value.commit = fixture.merge_commit;
        value.commit.chunks[1].input = projection;
        value.commit.merge_prepared_digest = value.prepared.self_digest;
        value.commit.merged_lease = value.prepared.merged_lease;
        value.commit.merged_artifact = value.prepared.merged_artifact;
        value.commit = reseal(std::move(value.commit));
        return value;
    };
    const auto validate_merge_alias_case = [&](const MergeAliasCase& value) {
        const std::array changed_attempts_1 = {value.attempt_1};
        const std::array<sieve::ChunkTerminalEvidenceViewV1, 2> changed_evidence = {{
            {std::span<const sieve::AttemptStartedV1>{worker_attempts}, &fixture.handoff},
            {std::span<const sieve::AttemptStartedV1>{changed_attempts_1}, &value.handoff_1},
        }};
        const std::array changed_starts = {value.started};
        return sieve::validate_merge_dependency_chain(
            fixture.manifest, changed_evidence, changed_starts, &value.prepared, &value.commit);
    };
    const auto require_merge_alias_rejected = [&](std::string_view name,
                                                  const MergeAliasCase& value) {
        const auto status = validate_merge_alias_case(value);
        if (status) {
            fail(name, __LINE__, "legal sequence accepted aliased identities");
        }
    };
    const auto require_merge_alias_accepted = [&](std::string_view name,
                                                  const MergeAliasCase& value) {
        const auto status = validate_merge_alias_case(value);
        if (!status) {
            fail(name, __LINE__, sieve::distributed_sieve_protocol_error_name(status.error));
        }
    };

    require_merge_alias_accepted("worker lease low limb reuse with distinct high limb",
                                 make_merge_alias_case(
                                     [&](auto& attempt_1, auto&) {
                                         attempt_1.lease.lease_id.limbs[0] =
                                             fixture.attempt.lease.lease_id.limbs[0];
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker exact 128-bit lease id reuse",
                                 make_merge_alias_case(
                                     [&](auto& attempt_1, auto&) {
                                         attempt_1.lease.lease_id = fixture.attempt.lease.lease_id;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker lease owner alias",
                                 make_merge_alias_case(
                                     [&](auto& attempt_1, auto&) {
                                         attempt_1.lease.owner_marker =
                                             fixture.attempt.lease.owner_marker;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker lease directory alias with distinct stem",
                                 make_merge_alias_case(
                                     [&](auto& attempt_1, auto&) {
                                         attempt_1.lease.directory =
                                             fixture.attempt.lease.directory;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker lease stem must match its chunk and ordinal",
                                 make_merge_alias_case(
                                     [&](auto& attempt_1, auto&) {
                                         attempt_1.lease.relative_stem =
                                             fixture.attempt.lease.relative_stem;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker lease owner-to-directory alias",
                                 make_merge_alias_case(
                                     [&](auto& attempt_1, auto&) {
                                         attempt_1.lease.owner_marker =
                                             fixture.attempt.lease.directory;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker lease directory-to-owner alias",
                                 make_merge_alias_case(
                                     [&](auto& attempt_1, auto&) {
                                         attempt_1.lease.directory =
                                             fixture.attempt.lease.owner_marker;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker OOC same store with different generation",
                                 make_merge_alias_case(
                                     [&](auto&, auto& handoff_1) {
                                         handoff_1.artifact.descriptor.store_id =
                                             fixture.handoff.artifact.descriptor.store_id;
                                     },
                                     {}, {}));
    require_merge_alias_accepted("worker OOC generation reuse",
                                 make_merge_alias_case(
                                     [&](auto&, auto& handoff_1) {
                                         handoff_1.artifact.descriptor.generation =
                                             fixture.handoff.artifact.descriptor.generation;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker OOC index identity alias",
                                 make_merge_alias_case(
                                     [&](auto&, auto& handoff_1) {
                                         handoff_1.artifact.index_file.identity =
                                             fixture.handoff.artifact.index_file.identity;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker OOC data identity alias",
                                 make_merge_alias_case(
                                     [&](auto&, auto& handoff_1) {
                                         handoff_1.artifact.data_file.identity =
                                             fixture.handoff.artifact.data_file.identity;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker OOC index-to-data identity alias",
                                 make_merge_alias_case(
                                     [&](auto&, auto& handoff_1) {
                                         handoff_1.artifact.index_file.identity =
                                             fixture.handoff.artifact.data_file.identity;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker OOC data-to-index identity alias",
                                 make_merge_alias_case(
                                     [&](auto&, auto& handoff_1) {
                                         handoff_1.artifact.data_file.identity =
                                             fixture.handoff.artifact.index_file.identity;
                                     },
                                     {}, {}));
    require_merge_alias_accepted(
        "worker-to-merge lease low limb reuse",
        make_merge_alias_case({},
                              [&](auto& started) {
                                  started.merged_lease.lease_id.limbs[0] =
                                      fixture.handoff.lease.lease_id.limbs[0];
                              },
                              {}));
    require_merge_alias_rejected("worker-to-merge exact 128-bit lease id reuse",
                                 make_merge_alias_case({},
                                                       [&](auto& started) {
                                                           started.merged_lease.lease_id =
                                                               fixture.handoff.lease.lease_id;
                                                       },
                                                       {}));
    require_merge_alias_rejected("worker-to-merge lease owner alias",
                                 make_merge_alias_case({},
                                                       [&](auto& started) {
                                                           started.merged_lease.owner_marker =
                                                               fixture.handoff.lease.owner_marker;
                                                       },
                                                       {}));
    require_merge_alias_rejected("worker-to-merge directory alias with distinct stem",
                                 make_merge_alias_case({},
                                                       [&](auto& started) {
                                                           started.merged_lease.directory =
                                                               fixture.handoff.lease.directory;
                                                       },
                                                       {}));
    require_merge_alias_rejected("worker-to-merge owner-to-directory alias",
                                 make_merge_alias_case({},
                                                       [&](auto& started) {
                                                           started.merged_lease.owner_marker =
                                                               fixture.handoff.lease.directory;
                                                       },
                                                       {}));
    require_merge_alias_rejected("worker-to-merge directory-to-owner alias",
                                 make_merge_alias_case({},
                                                       [&](auto& started) {
                                                           started.merged_lease.directory =
                                                               fixture.handoff.lease.owner_marker;
                                                       },
                                                       {}));
    require_merge_alias_rejected("worker-to-merge same store with different generation",
                                 make_merge_alias_case({}, {}, [&](auto& prepared) {
                                     prepared.merged_artifact.descriptor.store_id =
                                         fixture.handoff.artifact.descriptor.store_id;
                                 }));
    require_merge_alias_accepted("worker-to-merge OOC generation reuse",
                                 make_merge_alias_case({}, {}, [&](auto& prepared) {
                                     prepared.merged_artifact.descriptor.generation =
                                         fixture.handoff.artifact.descriptor.generation;
                                 }));
    require_merge_alias_rejected("worker-to-merge OOC index identity alias",
                                 make_merge_alias_case({}, {}, [&](auto& prepared) {
                                     prepared.merged_artifact.index_file.identity =
                                         fixture.handoff.artifact.index_file.identity;
                                 }));
    require_merge_alias_rejected("worker-to-merge OOC data identity alias",
                                 make_merge_alias_case({}, {}, [&](auto& prepared) {
                                     prepared.merged_artifact.data_file.identity =
                                         fixture.handoff.artifact.data_file.identity;
                                 }));
    require_merge_alias_rejected("worker-to-merge OOC index-to-data identity alias",
                                 make_merge_alias_case({}, {}, [&](auto& prepared) {
                                     prepared.merged_artifact.index_file.identity =
                                         fixture.handoff.artifact.data_file.identity;
                                 }));
    require_merge_alias_rejected("worker-to-merge OOC data-to-index identity alias",
                                 make_merge_alias_case({}, {}, [&](auto& prepared) {
                                     prepared.merged_artifact.data_file.identity =
                                         fixture.handoff.artifact.index_file.identity;
                                 }));

    for (const auto& [control_name, control_identity] : manifest_control_identities) {
        for (const bool mutate_owner : {true, false}) {
            const auto value =
                make_merge_alias_case({},
                                      [&](auto& started) {
                                          if (mutate_owner) {
                                              started.merged_lease.owner_marker = control_identity;
                                          } else {
                                              started.merged_lease.directory = control_identity;
                                          }
                                      },
                                      {});
            const std::string context = std::string("merged lease ") +
                                        (mutate_owner ? "owner" : "directory") + " aliases " +
                                        std::string(control_name);
            require_merge_alias_rejected(context, value);
        }
        for (const bool mutate_index : {true, false}) {
            const auto value = make_merge_alias_case({}, {}, [&](auto& prepared) {
                if (mutate_index) {
                    prepared.merged_artifact.index_file.identity = control_identity;
                } else {
                    prepared.merged_artifact.data_file.identity = control_identity;
                }
            });
            const std::string context = std::string("merged artifact ") +
                                        (mutate_index ? "index" : "data") + " aliases " +
                                        std::string(control_name);
            require_merge_alias_rejected(context, value);
        }
    }

    const auto worker_format_drift_case = make_merge_alias_case(
        [&](auto&, auto& handoff_1) { ++handoff_1.artifact.descriptor.format_version; }, {}, {});
    require_merge_alias_rejected("merge dependency rejects worker OOC format drift",
                                 worker_format_drift_case);

    const auto merged_format_drift_case = make_merge_alias_case(
        {}, {}, [&](auto& prepared) { ++prepared.merged_artifact.descriptor.format_version; });
    const std::array merged_format_starts = {merged_format_drift_case.started};
    require_failed(sieve::validate_merge_predecessor_chain(fixture.manifest, merged_format_starts,
                                                           &merged_format_drift_case.prepared,
                                                           &merged_format_drift_case.commit),
                   "merge prepared and commit format bind manifest OOC version");

    auto bundle_owner_as_index = fixture.handoff;
    bundle_owner_as_index.artifact.index_file.identity = bundle_owner_as_index.lease.owner_marker;
    require_reseal_failed(std::move(bundle_owner_as_index),
                          "worker bundle rejects lease owner as index identity");
    auto bundle_directory_as_data = fixture.handoff;
    bundle_directory_as_data.artifact.data_file.identity = bundle_directory_as_data.lease.directory;
    require_reseal_failed(std::move(bundle_directory_as_data),
                          "worker bundle rejects lease directory as data identity");
    auto bundle_owner_as_data = fixture.handoff;
    bundle_owner_as_data.artifact.data_file.identity = bundle_owner_as_data.lease.owner_marker;
    require_reseal_failed(std::move(bundle_owner_as_data),
                          "worker bundle rejects lease owner as data identity");
    auto bundle_directory_as_index = fixture.handoff;
    bundle_directory_as_index.artifact.index_file.identity =
        bundle_directory_as_index.lease.directory;
    require_reseal_failed(std::move(bundle_directory_as_index),
                          "worker bundle rejects lease directory as index identity");

    auto case_fold_attempt_0 = fixture.failure_attempt_0;
    case_fold_attempt_0.lease.lease_id = lease_id_with_seed(500);
    case_fold_attempt_0.lease.owner_marker = native_identity(500);
    case_fold_attempt_0.lease.directory = native_identity(550);
    case_fold_attempt_0.lease.relative_stem = "chunk_1_attempt_00";
    case_fold_attempt_0 = reseal(std::move(case_fold_attempt_0));
    auto case_fold_attempt_1 = fixture.failure_attempt_1;
    case_fold_attempt_1.predecessor_digest = case_fold_attempt_0.self_digest;
    case_fold_attempt_1.lease.lease_id = lease_id_with_seed(501);
    case_fold_attempt_1.lease.owner_marker = native_identity(510);
    case_fold_attempt_1.lease.directory = case_fold_attempt_0.lease.directory;
    case_fold_attempt_1.lease.relative_stem = "chunk_1_attempt_01";
    case_fold_attempt_1 = reseal(std::move(case_fold_attempt_1));
    const std::array case_fold_attempts = {case_fold_attempt_0, case_fold_attempt_1};
    require_failed(sieve::validate_worker_attempt_chain(fixture.manifest, 1, case_fold_attempts,
                                                        nullptr, nullptr),
                   "lease directory reuse conflicts even with distinct stems");

    auto fake_merge_started = fixture.merge_started;
    perturb_digest(fake_merge_started.ordered_inputs[0].handoff_digest);
    fake_merge_started = reseal(std::move(fake_merge_started));
    auto fake_merge_prepared = fixture.merge_prepared;
    fake_merge_prepared.merge_started_digest = fake_merge_started.self_digest;
    fake_merge_prepared.ordered_inputs = fake_merge_started.ordered_inputs;
    fake_merge_prepared = reseal(std::move(fake_merge_prepared));
    auto fake_merge_commit = fixture.merge_commit;
    fake_merge_commit.chunks[0].input = fake_merge_started.ordered_inputs[0];
    fake_merge_commit.merge_prepared_digest = fake_merge_prepared.self_digest;
    fake_merge_commit = reseal(std::move(fake_merge_commit));
    const std::array fake_merge_starts = {fake_merge_started};
    require_ok(sieve::validate_merge_predecessor_chain(fixture.manifest, fake_merge_starts,
                                                       &fake_merge_prepared, &fake_merge_commit),
               "internally resealed merge chain remains structurally valid");
    require_failed(sieve::validate_merge_dependency_chain(fixture.manifest, terminal_evidence,
                                                          fake_merge_starts, &fake_merge_prepared,
                                                          &fake_merge_commit),
                   "fully resealed fake merge projection lacks worker evidence");

    require_failed(sieve::validate_merge_predecessor_chain(fixture.manifest, merge_starts, nullptr,
                                                           &fixture.merge_commit),
                   "merge commit without prepared");

    auto wrong_prepared = fixture.merge_prepared;
    perturb_digest(wrong_prepared.merge_started_digest);
    wrong_prepared = reseal(std::move(wrong_prepared));
    require_failed(sieve::validate_merge_predecessor_chain(fixture.manifest, merge_starts,
                                                           &wrong_prepared, nullptr),
                   "prepared merge references wrong start");

    auto extra_merge_start = fixture.merge_started;
    extra_merge_start.merge_attempt_ordinal = 1;
    extra_merge_start.predecessor_digest = fixture.merge_started.self_digest;
    extra_merge_start.merged_lease = lease_identity(201, "merged_attempt_1");
    extra_merge_start = reseal(std::move(extra_merge_start));
    const std::array starts_after_prepared = {fixture.merge_started, extra_merge_start};
    require_failed(sieve::validate_merge_predecessor_chain(fixture.manifest, starts_after_prepared,
                                                           &fixture.merge_prepared,
                                                           &fixture.merge_commit),
                   "prepared merge must reference tail start");

    const std::array consumption_starts = {fixture.consumption_started};
    require_ok(sieve::validate_consumption_predecessor_chain(
                   fixture.manifest, fixture.merge_commit, consumption_starts,
                   &fixture.successor_prepared, &fixture.consumption_ack),
               "valid consumption chain");

    struct ConsumptionAliasCase final {
        sieve::ConsumptionStartedV1 started;
        sieve::SuccessorPreparedV1 successor;
        sieve::WaveConsumptionAckV1 ack;
    };
    const auto make_consumption_alias_case = [&](bool alias_lease, bool alias_artifact) {
        ConsumptionAliasCase value;
        value.started = fixture.consumption_started;
        if (alias_lease) {
            value.started.successor_lease = fixture.merge_commit.merged_lease;
        }
        value.started = reseal(std::move(value.started));

        value.successor = fixture.successor_prepared;
        value.successor.consumption_started_digest = value.started.self_digest;
        value.successor.successor_lease = value.started.successor_lease;
        if (alias_artifact) {
            value.successor.successor_artifact = fixture.merge_commit.merged_artifact;
        }
        value.successor = reseal(std::move(value.successor));

        value.ack = fixture.consumption_ack;
        value.ack.consumption_started_digest = value.started.self_digest;
        value.ack.successor_prepared_digest = value.successor.self_digest;
        value.ack.successor_artifact = value.successor.successor_artifact;
        value.ack.successor_semantic_digest = value.successor.successor_semantic_digest;
        value.ack.successor_cleanup_authority_identity = value.started.successor_lease.owner_marker;
        value.ack = reseal(std::move(value.ack));
        return value;
    };
    using ConsumptionStartMutation = std::function<void(sieve::ConsumptionStartedV1&)>;
    using SuccessorMutation = std::function<void(sieve::SuccessorPreparedV1&)>;
    const auto make_consumption_identity_case = [&](const ConsumptionStartMutation& mutate_start,
                                                    const SuccessorMutation& mutate_successor) {
        ConsumptionAliasCase value;
        value.started = fixture.consumption_started;
        if (mutate_start) {
            mutate_start(value.started);
        }
        value.started = reseal(std::move(value.started));

        value.successor = fixture.successor_prepared;
        value.successor.consumption_started_digest = value.started.self_digest;
        value.successor.successor_lease = value.started.successor_lease;
        if (mutate_successor) {
            mutate_successor(value.successor);
        }
        value.successor = reseal(std::move(value.successor));

        value.ack = fixture.consumption_ack;
        value.ack.consumption_started_digest = value.started.self_digest;
        value.ack.successor_prepared_digest = value.successor.self_digest;
        value.ack.successor_artifact = value.successor.successor_artifact;
        value.ack.successor_semantic_digest = value.successor.successor_semantic_digest;
        value.ack.successor_cleanup_authority_identity = value.started.successor_lease.owner_marker;
        value.ack = reseal(std::move(value.ack));
        return value;
    };
    const auto require_consumption_alias_rejected = [&](std::string_view name,
                                                        const ConsumptionAliasCase& value) {
        const std::array starts = {value.started};
        const auto status = sieve::validate_consumption_predecessor_chain(
            fixture.manifest, fixture.merge_commit, starts, &value.successor, &value.ack);
        if (status) {
            fail(name, __LINE__, "consumption chain accepted merged identity alias");
        }
    };
    require_consumption_alias_rejected("successor exact lease alias",
                                       make_consumption_alias_case(true, false));
    require_consumption_alias_rejected("successor exact artifact alias",
                                       make_consumption_alias_case(false, true));
    require_consumption_alias_rejected("successor exact lease and artifact alias",
                                       make_consumption_alias_case(true, true));

    auto successor_format_mismatch = fixture.successor_prepared;
    ++successor_format_mismatch.successor_artifact.descriptor.format_version;
    successor_format_mismatch = reseal(std::move(successor_format_mismatch));
    require_failed(sieve::validate_consumption_predecessor_chain(
                       fixture.manifest, fixture.merge_commit, consumption_starts,
                       &successor_format_mismatch, nullptr),
                   "successor artifact format binds consumption start");

    const auto matching_successor_format = make_consumption_identity_case(
        [](auto& started) { ++started.successor_format_version; },
        [](auto& successor) { ++successor.successor_artifact.descriptor.format_version; });
    const std::array matching_successor_starts = {matching_successor_format.started};
    require_ok(sieve::validate_consumption_predecessor_chain(
                   fixture.manifest, fixture.merge_commit, matching_successor_starts,
                   &matching_successor_format.successor, &matching_successor_format.ack),
               "successor format may advance when start binds the same version");

    for (const auto& [control_name, control_identity] : manifest_control_identities) {
        for (const bool mutate_owner : {true, false}) {
            const auto value = make_consumption_identity_case(
                [&](auto& started) {
                    if (mutate_owner) {
                        started.successor_lease.owner_marker = control_identity;
                    } else {
                        started.successor_lease.directory = control_identity;
                    }
                },
                {});
            const std::string context = std::string("successor lease ") +
                                        (mutate_owner ? "owner" : "directory") + " aliases " +
                                        std::string(control_name);
            require_consumption_alias_rejected(context, value);
        }
        for (const bool mutate_index : {true, false}) {
            const auto value = make_consumption_identity_case({}, [&](auto& successor) {
                if (mutate_index) {
                    successor.successor_artifact.index_file.identity = control_identity;
                } else {
                    successor.successor_artifact.data_file.identity = control_identity;
                }
            });
            const std::string context = std::string("successor artifact ") +
                                        (mutate_index ? "index" : "data") + " aliases " +
                                        std::string(control_name);
            require_consumption_alias_rejected(context, value);
        }
    }

    require_failed(sieve::validate_consumption_predecessor_chain(
                       fixture.manifest, fixture.merge_commit, consumption_starts, nullptr,
                       &fixture.consumption_ack),
                   "consumption ACK without prepared successor");

    auto wrong_successor = fixture.successor_prepared;
    perturb_digest(wrong_successor.consumption_started_digest);
    wrong_successor = reseal(std::move(wrong_successor));
    require_failed(
        sieve::validate_consumption_predecessor_chain(
            fixture.manifest, fixture.merge_commit, consumption_starts, &wrong_successor, nullptr),
        "successor references wrong consumption start");

    const auto validate_worker_authorization =
        [&](const sieve::WaveMergeCommitV1& commit,
            const sieve::ArtifactCleanupAuthorizedV1& authorization,
            const sieve::WorkerHandoffV1* worker_handoff) {
            return sieve::validate_artifact_cleanup_dependencies(
                fixture.manifest, commit, no_consumption_starts, nullptr, nullptr, authorization,
                worker_handoff, nullptr);
        };
    const auto validate_merged_authorization =
        [&](const sieve::WaveMergeCommitV1& commit,
            std::span<const sieve::ConsumptionStartedV1> starts,
            const sieve::SuccessorPreparedV1* successor, const sieve::WaveConsumptionAckV1* ack,
            const sieve::ArtifactCleanupAuthorizedV1& authorization,
            const sieve::MergePreparedV1* merge_prepared) {
            return sieve::validate_artifact_cleanup_dependencies(fixture.manifest, commit, starts,
                                                                 successor, ack, authorization,
                                                                 nullptr, merge_prepared);
        };

    require_ok(validate_worker_authorization(fixture.merge_commit,
                                             fixture.cleanup_authorizations[0], &fixture.handoff),
               "valid worker cleanup authorization closure");
    require_ok(validate_worker_authorization(fixture.merge_commit,
                                             fixture.cleanup_authorizations[1], &fixture.handoff_1),
               "valid second worker cleanup authorization closure");
    require_ok(validate_merged_authorization(fixture.merge_commit, consumption_starts,
                                             &fixture.successor_prepared, &fixture.consumption_ack,
                                             fixture.cleanup_authorizations[2],
                                             &fixture.merge_prepared),
               "valid merged cleanup authorization closure");
    require_failed(validate_worker_authorization(fixture.merge_commit,
                                                 fixture.cleanup_authorizations[0], nullptr),
                   "worker cleanup mint requires exact handoff");
    require_failed(validate_merged_authorization(
                       fixture.merge_commit, consumption_starts, &fixture.successor_prepared,
                       &fixture.consumption_ack, fixture.cleanup_authorizations[2], nullptr),
                   "merged cleanup mint requires exact prepared evidence");
    for (std::size_t index = 0; index < fixture.cleanup_authorizations.size(); ++index) {
        require_ok(sieve::validate_artifact_cleanup_completion_dependency(
                       fixture.cleanup_authorizations[index], fixture.cleanup_completions[index]),
                   "canonical cleanup recovery closure");
    }

    auto cleanup_bundle_alias = fixture.cleanup_authorizations[0];
    cleanup_bundle_alias.artifact.index_file.identity = cleanup_bundle_alias.lease.owner_marker;
    require_reseal_failed(std::move(cleanup_bundle_alias),
                          "cleanup authorization rejects bundle-native alias");

    auto cleanup_intent_alias = fixture.cleanup_completions[0];
    cleanup_intent_alias.cleanup_intent_identity =
        fixture.cleanup_authorizations[0].lease.owner_marker;
    cleanup_intent_alias = reseal(std::move(cleanup_intent_alias));
    require_failed(sieve::validate_artifact_cleanup_completion_dependency(
                       fixture.cleanup_authorizations[0], cleanup_intent_alias),
                   "cleanup recovery rejects intent alias with authorized lease");
    cleanup_intent_alias = fixture.cleanup_completions[0];
    cleanup_intent_alias.cleanup_intent_identity =
        fixture.cleanup_authorizations[0].artifact.index_file.identity;
    cleanup_intent_alias = reseal(std::move(cleanup_intent_alias));
    require_failed(sieve::validate_artifact_cleanup_completion_dependency(
                       fixture.cleanup_authorizations[0], cleanup_intent_alias),
                   "cleanup recovery rejects intent alias with authorized artifact");

    const auto make_worker_authorization_for_case = [&](const MergeAliasCase& value) {
        auto authorization = fixture.cleanup_authorizations[1];
        authorization.authorizer_record_digest = value.commit.self_digest;
        authorization.lease = value.handoff_1.lease;
        authorization.handoff_digest = value.handoff_1.self_digest;
        authorization.artifact = value.handoff_1.artifact;
        return reseal(std::move(authorization));
    };
    const auto root_aliased_worker_case = make_merge_alias_case(
        [&](auto& attempt_1, auto&) {
            attempt_1.lease.owner_marker = fixture.manifest.wave_root_identity;
        },
        {}, {});
    const auto root_aliased_worker_authorization =
        make_worker_authorization_for_case(root_aliased_worker_case);
    require_failed(validate_worker_authorization(root_aliased_worker_case.commit,
                                                 root_aliased_worker_authorization,
                                                 &root_aliased_worker_case.handoff_1),
                   "cleanup mint rejects worker lease alias with wave root");

    const auto lock_aliased_worker_case = make_merge_alias_case(
        [&](auto&, auto& handoff_1) {
            handoff_1.artifact.data_file.identity = fixture.manifest.permanent_lock_identity;
        },
        {}, {});
    const auto lock_aliased_worker_authorization =
        make_worker_authorization_for_case(lock_aliased_worker_case);
    require_failed(validate_worker_authorization(lock_aliased_worker_case.commit,
                                                 lock_aliased_worker_authorization,
                                                 &lock_aliased_worker_case.handoff_1),
                   "cleanup mint rejects worker artifact alias with permanent lock");

    const auto worker_format_drift_authorization =
        make_worker_authorization_for_case(worker_format_drift_case);
    require_failed(validate_worker_authorization(worker_format_drift_case.commit,
                                                 worker_format_drift_authorization,
                                                 &worker_format_drift_case.handoff_1),
                   "cleanup mint rejects fully resealed worker OOC format drift");

    auto wrong_worker_authorization = fixture.cleanup_authorizations[0];
    perturb_digest(wrong_worker_authorization.manifest_digest);
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup authorization binds manifest");

    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    perturb_digest(wrong_worker_authorization.authorizer_record_digest);
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup authorization binds commit authorizer");

    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    wrong_worker_authorization.authorizer = sieve::CleanupAuthorizerKindV1::consumption_ack_merged;
    wrong_worker_authorization.artifact_kind = sieve::CleanupArtifactKindV1::merged;
    wrong_worker_authorization.manifest_order_ordinal = 0;
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup rejects wrong authorizer kind");

    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    wrong_worker_authorization.manifest_order_ordinal = 1;
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup authorization binds manifest ordinal summary");

    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    ++wrong_worker_authorization.lease.lease_id.limbs[1];
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup authorization binds summary lease high limb");
    require_failed(sieve::validate_artifact_cleanup_completion_dependency(
                       wrong_worker_authorization, fixture.cleanup_completions[0]),
                   "cleanup recovery rejects lease high-limb authorization drift");

    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    ++wrong_worker_authorization.lease.owner_marker.generation;
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup authorization binds full handoff lease");

    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    perturb_digest(wrong_worker_authorization.handoff_digest);
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup authorization binds summary handoff");

    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    ++wrong_worker_authorization.artifact.descriptor.store_id;
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup authorization binds full handoff artifact");

    using CleanupAuthorizationMutation =
        std::pair<std::string_view, std::function<void(sieve::ArtifactCleanupAuthorizedV1&)>>;
    const std::vector<CleanupAuthorizationMutation> exact_target_mutations = {
        {"lease owner", [](auto& value) { value.lease.owner_marker = native_identity(900); }},
        {"lease directory", [](auto& value) { value.lease.directory = native_identity(910); }},
        {"lease stem", [](auto& value) { value.lease.relative_stem = "alternate_worker"; }},
        {"OOC format version", [](auto& value) { ++value.artifact.descriptor.format_version; }},
        {"OOC store id", [](auto& value) { ++value.artifact.descriptor.store_id; }},
        {"OOC generation", [](auto& value) { ++value.artifact.descriptor.generation; }},
        {"OOC relation count",
         [](auto& value) {
             ++value.artifact.descriptor.relation_count;
             ++value.artifact.sequence_receipt.relation_count;
         }},
        {"OOC data end",
         [](auto& value) {
             ++value.artifact.descriptor.data_end;
             ++value.artifact.data_file.extent;
         }},
        {"index file identity",
         [](auto& value) { value.artifact.index_file.identity = native_identity(920); }},
        {"index file extent", [](auto& value) { ++value.artifact.index_file.extent; }},
        {"data file identity",
         [](auto& value) { value.artifact.data_file.identity = native_identity(930); }},
        {"data file extent",
         [](auto& value) {
             value.artifact.data_file.extent += 2;
             value.artifact.descriptor.data_end += 2;
         }},
        {"sequence receipt low", [](auto& value) { ++value.artifact.sequence_receipt.low; }},
        {"sequence receipt high", [](auto& value) { ++value.artifact.sequence_receipt.high; }},
        {"corpus digest", [](auto& value) { perturb_digest(value.artifact.corpus_sha256); }},
    };
    for (const auto& [name, mutate] : exact_target_mutations) {
        auto changed = fixture.cleanup_authorizations[0];
        mutate(changed);
        changed = reseal(std::move(changed));
        const auto status =
            validate_worker_authorization(fixture.merge_commit, changed, &fixture.handoff);
        if (status) {
            fail(name, __LINE__, "resealed worker cleanup target drift accepted");
        }
    }

    auto laundered_worker_authorization = fixture.cleanup_authorizations[0];
    laundered_worker_authorization.lease.owner_marker = native_identity(700);
    laundered_worker_authorization.lease.directory = native_identity(710);
    laundered_worker_authorization.lease.relative_stem = "laundered_worker";
    laundered_worker_authorization.artifact.descriptor.format_version = 2;
    laundered_worker_authorization.artifact.descriptor.store_id = 701;
    laundered_worker_authorization.artifact.descriptor.generation = 702;
    laundered_worker_authorization.artifact.descriptor.data_end = 8192;
    laundered_worker_authorization.artifact.index_file.identity = native_identity(720);
    laundered_worker_authorization.artifact.index_file.extent = 1024;
    laundered_worker_authorization.artifact.data_file.identity = native_identity(730);
    laundered_worker_authorization.artifact.data_file.extent = 8192;
    laundered_worker_authorization = reseal(std::move(laundered_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit,
                                                 laundered_worker_authorization, nullptr),
                   "worker cleanup cannot launder unprojected lease and OOC identity");
    require_failed(validate_worker_authorization(fixture.merge_commit,
                                                 laundered_worker_authorization, &fixture.handoff),
                   "worker cleanup exact handoff rejects laundered lease and OOC identity");

    auto wrong_worker_commit = fixture.merge_commit;
    ++wrong_worker_commit.chunks[0].input.sequence_receipt.low;
    wrong_worker_commit = reseal(std::move(wrong_worker_commit));
    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    wrong_worker_authorization.authorizer_record_digest = wrong_worker_commit.self_digest;
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(wrong_worker_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup authorization binds commit summary");

    wrong_worker_commit = fixture.merge_commit;
    perturb_digest(wrong_worker_commit.work_digest);
    wrong_worker_commit = reseal(std::move(wrong_worker_commit));
    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    wrong_worker_authorization.authorizer_record_digest = wrong_worker_commit.self_digest;
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(wrong_worker_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup rejects fully resealed commit work drift");

    auto wrong_merged_authorization = fixture.cleanup_authorizations[2];
    perturb_digest(wrong_merged_authorization.manifest_digest);
    wrong_merged_authorization = reseal(std::move(wrong_merged_authorization));
    require_failed(
        validate_merged_authorization(fixture.merge_commit, consumption_starts,
                                      &fixture.successor_prepared, &fixture.consumption_ack,
                                      wrong_merged_authorization, &fixture.merge_prepared),
        "merged cleanup authorization binds manifest");

    wrong_merged_authorization = fixture.cleanup_authorizations[2];
    wrong_merged_authorization.authorizer = sieve::CleanupAuthorizerKindV1::merge_commit_worker;
    wrong_merged_authorization.artifact_kind = sieve::CleanupArtifactKindV1::worker;
    wrong_merged_authorization = reseal(std::move(wrong_merged_authorization));
    require_failed(
        validate_merged_authorization(fixture.merge_commit, consumption_starts,
                                      &fixture.successor_prepared, &fixture.consumption_ack,
                                      wrong_merged_authorization, &fixture.merge_prepared),
        "merged cleanup rejects wrong authorizer kind");

    wrong_merged_authorization = fixture.cleanup_authorizations[2];
    perturb_digest(wrong_merged_authorization.authorizer_record_digest);
    wrong_merged_authorization = reseal(std::move(wrong_merged_authorization));
    require_failed(
        validate_merged_authorization(fixture.merge_commit, consumption_starts,
                                      &fixture.successor_prepared, &fixture.consumption_ack,
                                      wrong_merged_authorization, &fixture.merge_prepared),
        "merged cleanup authorization binds ACK");

    wrong_merged_authorization = fixture.cleanup_authorizations[2];
    wrong_merged_authorization.manifest_order_ordinal = 1;
    require_failed(validate_value(wrong_merged_authorization, false),
                   "merged cleanup authorization requires canonical ordinal");

    wrong_merged_authorization = fixture.cleanup_authorizations[2];
    ++wrong_merged_authorization.lease.lease_id.limbs[1];
    wrong_merged_authorization = reseal(std::move(wrong_merged_authorization));
    require_failed(
        validate_merged_authorization(fixture.merge_commit, consumption_starts,
                                      &fixture.successor_prepared, &fixture.consumption_ack,
                                      wrong_merged_authorization, &fixture.merge_prepared),
        "merged cleanup authorization binds commit lease high limb");

    wrong_merged_authorization = fixture.cleanup_authorizations[2];
    perturb_digest(wrong_merged_authorization.handoff_digest);
    wrong_merged_authorization = reseal(std::move(wrong_merged_authorization));
    require_failed(
        validate_merged_authorization(fixture.merge_commit, consumption_starts,
                                      &fixture.successor_prepared, &fixture.consumption_ack,
                                      wrong_merged_authorization, &fixture.merge_prepared),
        "merged cleanup authorization binds prepared digest");

    wrong_merged_authorization = fixture.cleanup_authorizations[2];
    ++wrong_merged_authorization.artifact.descriptor.store_id;
    wrong_merged_authorization = reseal(std::move(wrong_merged_authorization));
    require_failed(
        validate_merged_authorization(fixture.merge_commit, consumption_starts,
                                      &fixture.successor_prepared, &fixture.consumption_ack,
                                      wrong_merged_authorization, &fixture.merge_prepared),
        "merged cleanup authorization binds commit artifact");

    auto wrong_ack = fixture.consumption_ack;
    perturb_digest(wrong_ack.merge_commit_digest);
    wrong_ack = reseal(std::move(wrong_ack));
    wrong_merged_authorization = fixture.cleanup_authorizations[2];
    wrong_merged_authorization.authorizer_record_digest = wrong_ack.self_digest;
    wrong_merged_authorization = reseal(std::move(wrong_merged_authorization));
    require_failed(validate_merged_authorization(
                       fixture.merge_commit, consumption_starts, &fixture.successor_prepared,
                       &wrong_ack, wrong_merged_authorization, &fixture.merge_prepared),
                   "merged cleanup authorization binds ACK to commit");

    auto wrong_merged_commit = fixture.merge_commit;
    perturb_digest(wrong_merged_commit.work_digest);
    wrong_merged_commit = reseal(std::move(wrong_merged_commit));
    auto wrong_consumption_started = fixture.consumption_started;
    wrong_consumption_started.merge_commit_digest = wrong_merged_commit.self_digest;
    wrong_consumption_started = reseal(std::move(wrong_consumption_started));
    const std::array wrong_consumption_starts = {wrong_consumption_started};
    auto wrong_successor_for_commit = fixture.successor_prepared;
    wrong_successor_for_commit.consumption_started_digest = wrong_consumption_started.self_digest;
    wrong_successor_for_commit = reseal(std::move(wrong_successor_for_commit));
    wrong_ack = fixture.consumption_ack;
    wrong_ack.merge_commit_digest = wrong_merged_commit.self_digest;
    wrong_ack.consumption_started_digest = wrong_consumption_started.self_digest;
    wrong_ack.successor_prepared_digest = wrong_successor_for_commit.self_digest;
    wrong_ack = reseal(std::move(wrong_ack));
    wrong_merged_authorization = fixture.cleanup_authorizations[2];
    wrong_merged_authorization.authorizer_record_digest = wrong_ack.self_digest;
    wrong_merged_authorization = reseal(std::move(wrong_merged_authorization));
    require_failed(validate_merged_authorization(
                       wrong_merged_commit, wrong_consumption_starts, &wrong_successor_for_commit,
                       &wrong_ack, wrong_merged_authorization, &fixture.merge_prepared),
                   "merged cleanup rejects fully resealed commit work drift");

    auto wrong_merge_prepared = fixture.merge_prepared;
    ++wrong_merge_prepared.merged_artifact.descriptor.store_id;
    wrong_merge_prepared = reseal(std::move(wrong_merge_prepared));
    require_failed(
        validate_merged_authorization(fixture.merge_commit, consumption_starts,
                                      &fixture.successor_prepared, &fixture.consumption_ack,
                                      fixture.cleanup_authorizations[2], &wrong_merge_prepared),
        "merged cleanup authorization binds prepared artifact");

    auto wrong_cleanup_completion = fixture.cleanup_completions[0];
    perturb_digest(wrong_cleanup_completion.authorization_digest);
    wrong_cleanup_completion = reseal(std::move(wrong_cleanup_completion));
    require_failed(sieve::validate_artifact_cleanup_completion_dependency(
                       fixture.cleanup_authorizations[0], wrong_cleanup_completion),
                   "cleanup completion binds authorization");

    require_ok(sieve::validate_wave_completion_dependencies(
                   fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                   fixture.consumption_ack, consumption_starts, fixture.cleanup_authorizations,
                   fixture.cleanup_completions, fixture.completed),
               "valid completion dependency closure");

    auto final_high_limb_authorizations = fixture.cleanup_authorizations;
    ++final_high_limb_authorizations[0].lease.lease_id.limbs[1];
    final_high_limb_authorizations[0] = reseal(std::move(final_high_limb_authorizations[0]));
    auto final_high_limb_completions = fixture.cleanup_completions;
    final_high_limb_completions[0].authorization_digest =
        final_high_limb_authorizations[0].self_digest;
    final_high_limb_completions[0] = reseal(std::move(final_high_limb_completions[0]));
    require_ok(sieve::validate_artifact_cleanup_completion_dependency(
                   final_high_limb_authorizations[0], final_high_limb_completions[0]),
               "rebound recovery record remains internally canonical");
    auto final_high_limb_completed = fixture.completed;
    final_high_limb_completed.cleanup_confirmations[0].authorization_digest =
        final_high_limb_authorizations[0].self_digest;
    final_high_limb_completed.cleanup_confirmations[0].completion_digest =
        final_high_limb_completions[0].self_digest;
    final_high_limb_completed = reseal(std::move(final_high_limb_completed));
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, final_high_limb_authorizations,
                       final_high_limb_completions, final_high_limb_completed),
                   "WaveCompleted rejects fully resealed lease high-limb drift");

    auto control_aliased_authorizations = fixture.cleanup_authorizations;
    control_aliased_authorizations[0].lease.owner_marker = fixture.manifest.wave_root_identity;
    control_aliased_authorizations[0] = reseal(std::move(control_aliased_authorizations[0]));
    auto control_aliased_completions = fixture.cleanup_completions;
    control_aliased_completions[0].authorization_digest =
        control_aliased_authorizations[0].self_digest;
    control_aliased_completions[0] = reseal(std::move(control_aliased_completions[0]));
    auto control_aliased_completed = fixture.completed;
    control_aliased_completed.cleanup_confirmations[0].authorization_digest =
        control_aliased_authorizations[0].self_digest;
    control_aliased_completed.cleanup_confirmations[0].completion_digest =
        control_aliased_completions[0].self_digest;
    control_aliased_completed = reseal(std::move(control_aliased_completed));
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, control_aliased_authorizations,
                       control_aliased_completions, control_aliased_completed),
                   "final closure rejects cleanup lease alias with wave root");

    auto control_aliased_intent_completions = fixture.cleanup_completions;
    control_aliased_intent_completions[0].cleanup_intent_identity =
        fixture.manifest.permanent_lock_identity;
    control_aliased_intent_completions[0] =
        reseal(std::move(control_aliased_intent_completions[0]));
    auto control_aliased_intent_completed = fixture.completed;
    control_aliased_intent_completed.cleanup_confirmations[0].completion_digest =
        control_aliased_intent_completions[0].self_digest;
    control_aliased_intent_completed = reseal(std::move(control_aliased_intent_completed));
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, fixture.cleanup_authorizations,
                       control_aliased_intent_completions, control_aliased_intent_completed),
                   "final closure rejects cleanup intent alias with permanent lock");

    auto format_drift_authorizations = fixture.cleanup_authorizations;
    ++format_drift_authorizations[0].artifact.descriptor.format_version;
    format_drift_authorizations[0] = reseal(std::move(format_drift_authorizations[0]));
    auto format_drift_completions = fixture.cleanup_completions;
    format_drift_completions[0].authorization_digest = format_drift_authorizations[0].self_digest;
    format_drift_completions[0] = reseal(std::move(format_drift_completions[0]));
    auto format_drift_completed = fixture.completed;
    format_drift_completed.cleanup_confirmations[0].authorization_digest =
        format_drift_authorizations[0].self_digest;
    format_drift_completed.cleanup_confirmations[0].completion_digest =
        format_drift_completions[0].self_digest;
    format_drift_completed = reseal(std::move(format_drift_completed));
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, format_drift_authorizations,
                       format_drift_completions, format_drift_completed),
                   "final closure rejects cleanup authorization OOC format laundering");

    auto wrong_completed = fixture.completed;
    perturb_digest(wrong_completed.consumption_ack_digest);
    wrong_completed = reseal(std::move(wrong_completed));
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, fixture.cleanup_authorizations,
                       fixture.cleanup_completions, wrong_completed),
                   "completed record references wrong ACK");

    const std::span<const sieve::ArtifactCleanupAuthorizedV1> missing_authorization(
        fixture.cleanup_authorizations.data(), fixture.cleanup_authorizations.size() - 1);
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, missing_authorization,
                       fixture.cleanup_completions, fixture.completed),
                   "completed record requires all cleanup authorizations");

    const std::span<const sieve::ArtifactCleanupCompletedV1> missing_cleanup(
        fixture.cleanup_completions.data(), fixture.cleanup_completions.size() - 1);
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, fixture.cleanup_authorizations,
                       missing_cleanup, fixture.completed),
                   "completed record requires all cleanup confirmations");

    auto forged_authorizations = fixture.cleanup_authorizations;
    perturb_digest(forged_authorizations[0].authorizer_record_digest);
    forged_authorizations[0] = reseal(std::move(forged_authorizations[0]));
    auto forged_completions = fixture.cleanup_completions;
    forged_completions[0].authorization_digest = forged_authorizations[0].self_digest;
    forged_completions[0] = reseal(std::move(forged_completions[0]));
    auto forged_completed = fixture.completed;
    forged_completed.cleanup_confirmations[0].authorization_digest =
        forged_authorizations[0].self_digest;
    forged_completed.cleanup_confirmations[0].completion_digest = forged_completions[0].self_digest;
    forged_completed = reseal(std::move(forged_completed));
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, forged_authorizations,
                       forged_completions, forged_completed),
                   "matching arbitrary cleanup digests do not establish authority");

    auto duplicate_authorizations = fixture.cleanup_authorizations;
    duplicate_authorizations[1] = duplicate_authorizations[0];
    auto duplicate_completions = fixture.cleanup_completions;
    duplicate_completions[1].authorization_digest = duplicate_authorizations[1].self_digest;
    duplicate_completions[1] = reseal(std::move(duplicate_completions[1]));
    auto duplicate_completed = fixture.completed;
    duplicate_completed.cleanup_confirmations[1].authorization_digest =
        duplicate_authorizations[1].self_digest;
    duplicate_completed.cleanup_confirmations[1].completion_digest =
        duplicate_completions[1].self_digest;
    duplicate_completed = reseal(std::move(duplicate_completed));
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, duplicate_authorizations,
                       duplicate_completions, duplicate_completed),
                   "duplicate worker zero authorization cannot cover worker one");

    auto swapped_authorizations = fixture.cleanup_authorizations;
    auto swapped_completions = fixture.cleanup_completions;
    std::swap(swapped_authorizations[0], swapped_authorizations[1]);
    std::swap(swapped_completions[0], swapped_completions[1]);
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, swapped_authorizations,
                       swapped_completions, fixture.completed),
                   "swapped cleanup arrays violate manifest-order coverage");

    auto laundered_successor = fixture.successor_prepared;
    laundered_successor.consumption_started_digest = digest_with_seed(180);
    laundered_successor.successor_lease = lease_identity(800, "laundered_successor");
    laundered_successor.successor_artifact = corpus_artifact(810, 6);
    laundered_successor.successor_semantic_digest = digest_with_seed(181);
    laundered_successor = reseal(std::move(laundered_successor));
    auto laundered_ack = fixture.consumption_ack;
    laundered_ack.consumption_started_digest = laundered_successor.consumption_started_digest;
    laundered_ack.successor_prepared_digest = laundered_successor.self_digest;
    laundered_ack.successor_artifact = laundered_successor.successor_artifact;
    laundered_ack.successor_semantic_digest = laundered_successor.successor_semantic_digest;
    laundered_ack.successor_cleanup_authority_identity =
        laundered_successor.successor_lease.owner_marker;
    laundered_ack = reseal(std::move(laundered_ack));
    auto laundered_authorizations = fixture.cleanup_authorizations;
    laundered_authorizations[2].authorizer_record_digest = laundered_ack.self_digest;
    laundered_authorizations[2] = reseal(std::move(laundered_authorizations[2]));
    auto laundered_completions = fixture.cleanup_completions;
    laundered_completions[2].authorization_digest = laundered_authorizations[2].self_digest;
    laundered_completions[2] = reseal(std::move(laundered_completions[2]));
    auto laundered_completed = fixture.completed;
    laundered_completed.consumption_ack_digest = laundered_ack.self_digest;
    laundered_completed.successor_prepared_digest = laundered_successor.self_digest;
    laundered_completed.cleanup_confirmations[2].authorization_digest =
        laundered_authorizations[2].self_digest;
    laundered_completed.cleanup_confirmations[2].completion_digest =
        laundered_completions[2].self_digest;
    laundered_completed.successor_artifact = laundered_successor.successor_artifact;
    laundered_completed.successor_semantic_digest = laundered_successor.successor_semantic_digest;
    laundered_completed = reseal(std::move(laundered_completed));
    require_failed(validate_merged_authorization(
                       fixture.merge_commit, consumption_starts, &laundered_successor,
                       &laundered_ack, laundered_authorizations[2], &fixture.merge_prepared),
                   "merged cleanup mint rejects ACK without canonical consumption start");
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, laundered_successor, laundered_ack,
                       consumption_starts, laundered_authorizations, laundered_completions,
                       laundered_completed),
                   "final closure rejects fully resealed ACK and successor laundering");
}

void test_empty_chunk_projection_and_merge() {
    ProtocolFixture fixture;

    auto manifest = fixture.manifest;
    manifest.effective_sq_begin = 2;
    manifest.effective_sq_end = 3;
    manifest.worker_count = 2;
    manifest.chunks = {
        {0, 2, 3, "chunk_nonempty"},
        {1, 3, 3, "chunk_empty"},
    };
    manifest = reseal(std::move(manifest));
    require_ok(validate_value(manifest), "workers may exceed nonempty special-Q ranges");

    auto attempt = fixture.attempt;
    attempt.manifest_digest = manifest.self_digest;
    attempt.sq_begin = 2;
    attempt.sq_end = 3;
    attempt.predecessor_digest = manifest.self_digest;
    attempt.lease.relative_stem = "chunk_nonempty_attempt_00";
    attempt = reseal(std::move(attempt));

    auto handoff = fixture.handoff;
    handoff.manifest_digest = manifest.self_digest;
    handoff.work_digest = manifest.work_sha256;
    handoff.wave_id = manifest.wave_id;
    handoff.sq_begin = 2;
    handoff.sq_end = 3;
    handoff.attempt_started_digest = attempt.self_digest;
    handoff.lease = attempt.lease;
    handoff.processed_sq_count = 1;
    handoff.next_sq_index = 3;
    handoff = reseal(std::move(handoff));

    sieve::TerminalChunkInputV1 nonempty_projection;
    nonempty_projection.chunk_id = 0;
    nonempty_projection.disposition = sieve::ChunkDispositionV1::handoff;
    nonempty_projection.sq_begin = 2;
    nonempty_projection.sq_end = 3;
    nonempty_projection.next_sq_index = 3;
    nonempty_projection.processed_sq_count = 1;
    nonempty_projection.completion_reason = sieve::WorkerCompletionReasonV1::range_exhausted;
    nonempty_projection.durable_attempt_count = 1;
    nonempty_projection.last_attempt_digest = attempt.self_digest;
    nonempty_projection.lease_id = handoff.lease.lease_id;
    nonempty_projection.handoff_digest = handoff.self_digest;
    nonempty_projection.raw_relation_count = handoff.relation_count;
    nonempty_projection.sequence_receipt = handoff.artifact.sequence_receipt;
    nonempty_projection.corpus_sha256 = handoff.artifact.corpus_sha256;

    sieve::TerminalChunkInputV1 empty_projection;
    empty_projection.chunk_id = 1;
    empty_projection.disposition = sieve::ChunkDispositionV1::empty;
    empty_projection.sq_begin = 3;
    empty_projection.sq_end = 3;
    empty_projection.next_sq_index = 3;
    empty_projection.completion_reason = sieve::WorkerCompletionReasonV1::zero_relations;

    CHECK(nonempty_projection.lease_id.limbs[0] != 0);
    CHECK(nonempty_projection.lease_id.limbs[1] != 0);
    CHECK(empty_projection.lease_id == sieve::LeaseIdV1{});

    const std::span<const sieve::AttemptStartedV1> no_attempts;
    require_ok(sieve::validate_terminal_chunk_projection(manifest, 1, no_attempts, nullptr,
                                                         empty_projection),
               "empty chunk has canonical zero-attempt projection");
    const std::array unexpected_attempts = {attempt};
    require_failed(sieve::validate_terminal_chunk_projection(manifest, 1, unexpected_attempts,
                                                             nullptr, empty_projection),
                   "empty chunk rejects attempts");
    require_failed(sieve::validate_terminal_chunk_projection(manifest, 1, no_attempts, &handoff,
                                                             empty_projection),
                   "empty chunk rejects handoff");

    using EmptyProjectionMutation =
        std::pair<std::string_view, std::function<void(sieve::TerminalChunkInputV1&)>>;
    const std::vector<EmptyProjectionMutation> invalid_empty_fields = {
        {"completion reason",
         [](auto& value) {
             value.completion_reason = sieve::WorkerCompletionReasonV1::range_exhausted;
         }},
        {"processed count", [](auto& value) { value.processed_sq_count = 1; }},
        {"attempt count", [](auto& value) { value.durable_attempt_count = 1; }},
        {"last attempt digest",
         [](auto& value) { value.last_attempt_digest = digest_with_seed(210); }},
        {"lease id", [](auto& value) { value.lease_id = lease_id_with_seed(1); }},
        {"handoff digest", [](auto& value) { value.handoff_digest = digest_with_seed(211); }},
        {"raw relation count",
         [](auto& value) {
             value.raw_relation_count = 1;
             value.sequence_receipt.relation_count = 1;
         }},
        {"sequence receipt", [](auto& value) { value.sequence_receipt.low = 1; }},
        {"corpus digest", [](auto& value) { value.corpus_sha256 = digest_with_seed(212); }},
    };
    for (const auto& [name, mutate] : invalid_empty_fields) {
        auto changed = empty_projection;
        mutate(changed);
        const auto status =
            sieve::validate_terminal_chunk_projection(manifest, 1, no_attempts, nullptr, changed);
        if (status) {
            fail(name, __LINE__, "empty chunk accepted nonzero terminal field");
        }
    }

    auto merge_started = fixture.merge_started;
    merge_started.manifest_digest = manifest.self_digest;
    merge_started.work_digest = manifest.work_sha256;
    merge_started.ordered_inputs = {nonempty_projection, empty_projection};
    merge_started.predecessor_digest = manifest.self_digest;
    merge_started = reseal(std::move(merge_started));

    auto merge_prepared = fixture.merge_prepared;
    merge_prepared.manifest_digest = manifest.self_digest;
    merge_prepared.work_digest = manifest.work_sha256;
    merge_prepared.merge_started_digest = merge_started.self_digest;
    merge_prepared.ordered_inputs = merge_started.ordered_inputs;
    merge_prepared.input_relation_count = handoff.relation_count;
    merge_prepared.duplicate_relation_count = 0;
    merge_prepared.output_relation_count = handoff.relation_count;
    merge_prepared.per_chunk_retained_counts = {
        {0, handoff.relation_count},
        {1, 0},
    };
    merge_prepared.merged_artifact = corpus_artifact(6000, handoff.relation_count);
    merge_prepared.merged_lease = merge_started.merged_lease;
    merge_prepared = reseal(std::move(merge_prepared));

    auto merge_commit = fixture.merge_commit;
    merge_commit.manifest_digest = manifest.self_digest;
    merge_commit.work_digest = manifest.work_sha256;
    merge_commit.chunks = {
        {nonempty_projection, handoff.relation_count, {}},
        {empty_projection, 0, {}},
    };
    merge_commit.input_relation_count = merge_prepared.input_relation_count;
    merge_commit.duplicate_relation_count = 0;
    merge_commit.output_relation_count = merge_prepared.output_relation_count;
    merge_commit.merge_prepared_digest = merge_prepared.self_digest;
    merge_commit.merged_lease = merge_prepared.merged_lease;
    merge_commit.merged_artifact = merge_prepared.merged_artifact;
    merge_commit = reseal(std::move(merge_commit));

    const std::array attempts = {attempt};
    const std::array<sieve::ChunkTerminalEvidenceViewV1, 2> terminal_evidence = {{
        {std::span<const sieve::AttemptStartedV1>{attempts}, &handoff},
        {no_attempts, nullptr},
    }};
    const std::array merge_starts = {merge_started};
    require_ok(sieve::validate_merge_dependency_chain(manifest, terminal_evidence, merge_starts,
                                                      &merge_prepared, &merge_commit),
               "merge dependency chain accepts canonical empty input");

    auto consumption_started = fixture.consumption_started;
    consumption_started.merge_commit_digest = merge_commit.self_digest;
    consumption_started.manifest_digest = manifest.self_digest;
    consumption_started.predecessor_digest = manifest.self_digest;
    consumption_started = reseal(std::move(consumption_started));

    auto successor_prepared = fixture.successor_prepared;
    successor_prepared.consumption_started_digest = consumption_started.self_digest;
    successor_prepared.successor_lease = consumption_started.successor_lease;
    successor_prepared.successor_artifact =
        corpus_artifact(7000, merge_commit.output_relation_count);
    successor_prepared.input_relation_count = merge_commit.output_relation_count;
    successor_prepared.output_relation_count = merge_commit.output_relation_count;
    successor_prepared = reseal(std::move(successor_prepared));

    auto consumption_ack = fixture.consumption_ack;
    consumption_ack.merge_commit_digest = merge_commit.self_digest;
    consumption_ack.consumption_started_digest = consumption_started.self_digest;
    consumption_ack.successor_prepared_digest = successor_prepared.self_digest;
    consumption_ack.successor_artifact = successor_prepared.successor_artifact;
    consumption_ack.successor_semantic_digest = successor_prepared.successor_semantic_digest;
    consumption_ack.successor_cleanup_authority_identity =
        consumption_started.successor_lease.owner_marker;
    consumption_ack = reseal(std::move(consumption_ack));

    const std::array consumption_starts = {consumption_started};
    require_ok(sieve::validate_consumption_predecessor_chain(manifest, merge_commit,
                                                             consumption_starts,
                                                             &successor_prepared, &consumption_ack),
               "consumption chain advances beyond a trailing empty chunk");

    auto worker_cleanup_authorization = fixture.cleanup_authorizations[0];
    worker_cleanup_authorization.manifest_digest = manifest.self_digest;
    worker_cleanup_authorization.authorizer_record_digest = merge_commit.self_digest;
    worker_cleanup_authorization.manifest_order_ordinal = 0;
    worker_cleanup_authorization.lease = handoff.lease;
    worker_cleanup_authorization.handoff_digest = handoff.self_digest;
    worker_cleanup_authorization.artifact = handoff.artifact;
    worker_cleanup_authorization = reseal(std::move(worker_cleanup_authorization));

    const std::span<const sieve::ConsumptionStartedV1> no_consumption_starts;
    require_ok(sieve::validate_artifact_cleanup_dependencies(
                   manifest, merge_commit, no_consumption_starts, nullptr, nullptr,
                   worker_cleanup_authorization, &handoff, nullptr),
               "only the nonempty worker receives worker cleanup authority");

    auto merged_cleanup_authorization = fixture.cleanup_authorizations[2];
    merged_cleanup_authorization.manifest_digest = manifest.self_digest;
    merged_cleanup_authorization.authorizer_record_digest = consumption_ack.self_digest;
    merged_cleanup_authorization.lease = merge_commit.merged_lease;
    merged_cleanup_authorization.handoff_digest = merge_prepared.self_digest;
    merged_cleanup_authorization.artifact = merge_commit.merged_artifact;
    merged_cleanup_authorization = reseal(std::move(merged_cleanup_authorization));
    require_ok(sieve::validate_artifact_cleanup_dependencies(
                   manifest, merge_commit, consumption_starts, &successor_prepared,
                   &consumption_ack, merged_cleanup_authorization, nullptr, &merge_prepared),
               "merged corpus receives cleanup authority after consumption");

    auto worker_cleanup_completion = fixture.cleanup_completions[0];
    worker_cleanup_completion.authorization_digest = worker_cleanup_authorization.self_digest;
    worker_cleanup_completion.cleanup_intent_identity = native_identity(9000);
    worker_cleanup_completion = reseal(std::move(worker_cleanup_completion));
    require_ok(sieve::validate_artifact_cleanup_completion_dependency(worker_cleanup_authorization,
                                                                      worker_cleanup_completion),
               "nonempty worker cleanup completes");

    auto merged_cleanup_completion = fixture.cleanup_completions[2];
    merged_cleanup_completion.authorization_digest = merged_cleanup_authorization.self_digest;
    merged_cleanup_completion.cleanup_intent_identity = native_identity(9010);
    merged_cleanup_completion = reseal(std::move(merged_cleanup_completion));
    require_ok(sieve::validate_artifact_cleanup_completion_dependency(merged_cleanup_authorization,
                                                                      merged_cleanup_completion),
               "merged cleanup completes");

    auto completed = fixture.completed;
    completed.wave_root_identity = manifest.wave_root_identity;
    completed.permanent_lock_identity = manifest.permanent_lock_identity;
    completed.manifest_digest = manifest.self_digest;
    completed.merge_commit_digest = merge_commit.self_digest;
    completed.consumption_ack_digest = consumption_ack.self_digest;
    completed.successor_prepared_digest = successor_prepared.self_digest;
    completed.chunks = merge_commit.chunks;
    completed.cleanup_confirmations = {
        {sieve::CleanupArtifactKindV1::worker, 0, worker_cleanup_authorization.self_digest,
         worker_cleanup_completion.self_digest},
        {sieve::CleanupArtifactKindV1::merged, 0, merged_cleanup_authorization.self_digest,
         merged_cleanup_completion.self_digest},
    };
    completed.successor_artifact = successor_prepared.successor_artifact;
    completed.successor_semantic_digest = successor_prepared.successor_semantic_digest;
    completed = reseal(std::move(completed));

    const std::array cleanup_authorizations = {
        worker_cleanup_authorization,
        merged_cleanup_authorization,
    };
    const std::array cleanup_completions = {
        worker_cleanup_completion,
        merged_cleanup_completion,
    };
    require_ok(sieve::validate_wave_completion_dependencies(
                   manifest, merge_commit, successor_prepared, consumption_ack, consumption_starts,
                   cleanup_authorizations, cleanup_completions, completed),
               "trailing empty chunk reaches WaveCompleted without cleanup authority");
}

class WaveStoreTempDirectory final {
public:
    WaveStoreTempDirectory() {
        static std::atomic<std::uint64_t> sequence{0};
        const auto tick =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::uint64_t attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("gnfs-distributed-wave-store-" + std::to_string(tick) + "-" +
                     std::to_string(sequence.fetch_add(1)) + "-" + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
#ifndef _WIN32
                if (::chmod(path_.c_str(), 0700) != 0) {
                    const int saved_errno = errno;
                    std::filesystem::remove_all(path_, error);
                    throw std::system_error(saved_errno, std::generic_category(),
                                            "chmod wave-store temp directory");
                }
#endif
                const auto canonical_path = std::filesystem::canonical(path_, error);
                if (error) {
                    std::error_code ignored;
                    (void)std::filesystem::remove_all(path_, ignored);
                    throw std::filesystem::filesystem_error(
                        "canonicalize wave-store temp directory", path_, error);
                }
                path_ = canonical_path;
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error("create wave-store temp directory", path_,
                                                        error);
            }
        }
        throw TestFailure("could not reserve a wave-store temporary directory");
    }

    WaveStoreTempDirectory(const WaveStoreTempDirectory&) = delete;
    WaveStoreTempDirectory& operator=(const WaveStoreTempDirectory&) = delete;

    ~WaveStoreTempDirectory() {
        std::error_code ignored;
        (void)std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::filesystem::path wave_lock_path(const std::filesystem::path& root) {
    return root / wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF;
}

[[nodiscard]] std::filesystem::path wave_manifest_path(const std::filesystem::path& root) {
    return root / wave_detail::DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF;
}

[[nodiscard]] std::filesystem::path wave_manifest_pending_path(const std::filesystem::path& root) {
    return root / wave_detail::DISTRIBUTED_SIEVE_WAVE_MANIFEST_PENDING_LEAF;
}

[[nodiscard]] bool entry_exists_no_follow(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return false;
    }
    if (error) {
        throw std::filesystem::filesystem_error("inspect wave-store leaf", path, error);
    }
    return status.type() != std::filesystem::file_type::not_found;
}

[[nodiscard]] std::vector<std::byte> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw TestFailure("cannot open wave-store file for reading: " + path.string());
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw TestFailure("cannot size wave-store file: " + path.string());
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) {
        throw TestFailure("cannot read complete wave-store file: " + path.string());
    }
    return bytes;
}

void write_file_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw TestFailure("cannot open wave-store file for writing: " + path.string());
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    if (!output) {
        throw TestFailure("cannot write complete wave-store file: " + path.string());
    }
}

void write_foreign_leaf(const std::filesystem::path& path) {
    constexpr std::array<std::byte, 7> bytes{
        std::byte{0x66}, std::byte{0x6f}, std::byte{0x72}, std::byte{0x65},
        std::byte{0x69}, std::byte{0x67}, std::byte{0x6e},
    };
    write_file_bytes(path, bytes);
#ifndef _WIN32
    if (::chmod(path.c_str(), 0600) != 0) {
        throw std::system_error(errno, std::generic_category(), "chmod foreign wave-store leaf");
    }
#endif
}

void write_empty_foreign_leaf(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw TestFailure("cannot create empty wave-store file: " + path.string());
    }
    output.close();
    if (!output) {
        throw TestFailure("cannot close empty wave-store file: " + path.string());
    }
#ifndef _WIN32
    if (::chmod(path.c_str(), 0600) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "chmod empty foreign wave-store leaf");
    }
#endif
}

[[nodiscard]] sieve::WaveManifestV1 wave_manifest_draft() {
    auto draft = ProtocolFixture{}.manifest;
    draft.wave_root_identity = {};
    draft.permanent_lock_identity = {};
    draft.lock_semantics_version = 0;
    draft.self_digest = {};
    return draft;
}

[[nodiscard]] std::string
wave_diagnostic_detail(const wave_detail::DistributedSieveWaveStoreDiagnostic& diagnostic) {
    std::string detail(wave_detail::distributed_sieve_wave_store_status_name(diagnostic.status));
    if (diagnostic.native_error) {
        detail.append(": ");
        detail.append(diagnostic.native_error.message());
    }
    return detail;
}

void require_wave_status(const wave_detail::DistributedSieveWaveStoreDiagnostic& diagnostic,
                         wave_detail::DistributedSieveWaveStoreStatus expected,
                         std::string_view context) {
    if (diagnostic.status != expected) {
        fail(context, __LINE__, wave_diagnostic_detail(diagnostic));
    }
}

[[nodiscard]] wave_detail::DistributedSieveWaveStore&
require_wave_ready(wave_detail::DistributedSieveWaveStoreOpenResult& result,
                   std::string_view context) {
    if (!result || result.store == nullptr) {
        fail(context, __LINE__, wave_diagnostic_detail(result.diagnostic));
    }
    return *result.store;
}

[[nodiscard]] PrivateLeaseRootClaim& require_private_lease_root_claim_ready(
    wave_detail::DistributedSievePrivateLeaseRootClaimResult& result, std::string_view context) {
    if (!result || result.claim == nullptr) {
        fail(context, __LINE__, wave_diagnostic_detail(result.diagnostic));
    }
    return *result.claim;
}

[[nodiscard]] Digest create_closed_wave(const std::filesystem::path& root) {
    auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    auto& store = require_wave_ready(created, "create closed wave fixture");
    const Digest digest = store.manifest_digest();
    created.store.reset();
    return digest;
}

[[nodiscard]] Digest manifest_digest_from_file(const std::filesystem::path& path) {
    const auto bytes = read_file_bytes(path);
    const auto decoded = sieve::decode_distributed_sieve_record(bytes);
    if (!decoded) {
        fail("decode wave manifest prefix", __LINE__,
             sieve::distributed_sieve_protocol_error_name(decoded.status.error));
    }
    const auto* manifest = std::get_if<sieve::WaveManifestV1>(&*decoded.value);
    if (manifest == nullptr) {
        fail("wave manifest prefix record kind", __LINE__);
    }
    return manifest->self_digest;
}

struct WaveFaultStopContext final {
    wave_detail::DistributedSieveWaveStoreFaultPoint target =
        wave_detail::DistributedSieveWaveStoreFaultPoint::RootDurable;
    std::array<bool, WAVE_STORE_FAULT_POINTS.size()> observed{};
};

[[nodiscard]] bool stop_at_wave_fault(wave_detail::DistributedSieveWaveStoreFaultPoint point,
                                      void* opaque) noexcept {
    auto& context = *static_cast<WaveFaultStopContext*>(opaque);
    const auto index = static_cast<std::size_t>(point);
    if (index < context.observed.size()) {
        context.observed[index] = true;
    }
    return point == context.target;
}

struct RelationPrivateLeaseStopContext final {
    gnfs::relation::OOCPrivateLeaseFaultPoint target =
        gnfs::relation::OOCPrivateLeaseFaultPoint::ReservationPermitAcquired;
    bool observed = false;
};

[[nodiscard]] bool
stop_at_relation_private_lease_fault(gnfs::relation::OOCPrivateLeaseFaultPoint point,
                                     void* opaque) noexcept {
    auto& context = *static_cast<RelationPrivateLeaseStopContext*>(opaque);
    if (point != context.target) {
        return false;
    }
    context.observed = true;
    return true;
}

struct WavePrivateLeaseProtocolStopContext final {
    wave_detail::DistributedSievePrivateLeaseReservationBoundary target =
        wave_detail::DistributedSievePrivateLeaseReservationBoundary::PermitAcquired;
    std::array<bool, wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVATION_BOUNDARIES.size()>
        observed{};
};

[[nodiscard]] bool stop_at_wave_private_lease_boundary(
    wave_detail::DistributedSievePrivateLeaseReservationBoundary boundary, void* opaque) noexcept {
    auto& context = *static_cast<WavePrivateLeaseProtocolStopContext*>(opaque);
    const auto index = static_cast<std::size_t>(boundary);
    if (index < context.observed.size()) {
        context.observed[index] = true;
    }
    return boundary == context.target;
}

void leave_relation_private_lease_reservation_prefix(
    const std::filesystem::path& base_path, gnfs::relation::OOCPrivateLeaseFaultPoint fault_point) {
    RelationPrivateLeaseStopContext context{
        .target = fault_point,
    };
    const auto reservation = gnfs::relation::OOCCleanupTransaction::reserve_private_lease(
        base_path, gnfs::relation::OOCPrivateLeaseTestHooks{
                       .stop_after = stop_at_relation_private_lease_fault,
                       .context = &context,
                   });
    CHECK(context.observed);
    CHECK(reservation.result.status == gnfs::relation::OOCCleanupStatus::Interrupted);
    CHECK(!reservation.ownership.has_value());
}

struct WaveReservationWitnessObservationContext final {
    wave_detail::DistributedSievePrivateLeaseReservationBoundary expected_boundary =
        wave_detail::DistributedSievePrivateLeaseReservationBoundary::PermitAcquired;
    std::string expected_base_lock_leaf;
    bool invoked = false;
    bool matched = false;
};

void observe_wave_reservation_witnesses(
    std::span<const wave_detail::DistributedSievePrivateLeaseReservationInventoryWitness> witnesses,
    void* opaque) noexcept {
    auto& context = *static_cast<WaveReservationWitnessObservationContext*>(opaque);
    context.invoked = true;
    if (witnesses.size() != 1U) {
        return;
    }
    const auto& witness = witnesses.front();
    const auto phase = static_cast<std::size_t>(context.expected_boundary);
    const bool expects_lease_id =
        context.expected_boundary !=
        wave_detail::DistributedSievePrivateLeaseReservationBoundary::PermitAcquired;
    const bool has_lease_id = witness.lease_id[0] != 0 || witness.lease_id[1] != 0;
    context.matched = witness.base_lock_leaf == context.expected_base_lock_leaf &&
                      witness.boundary == context.expected_boundary &&
                      has_lease_id == expects_lease_id &&
                      witness.reserved_marker_identity.has_value() == (phase >= 1U) &&
                      witness.directory_identity.has_value() == (phase >= 3U) &&
                      witness.owner_marker_identity.has_value() == (phase >= 4U) &&
                      witness.owned_marker_identity.has_value() == (phase >= 6U);
}

#if !defined(_WIN32)

struct WaveForkFaultContext final {
    wave_detail::DistributedSieveWaveStoreFaultPoint target =
        wave_detail::DistributedSieveWaveStoreFaultPoint::RootDurable;
    pid_t original_process = -1;
    pid_t child_process = -1;
    bool invoked = false;
};

[[nodiscard]] bool fork_at_wave_fault(wave_detail::DistributedSieveWaveStoreFaultPoint point,
                                      void* opaque) noexcept {
    auto& context = *static_cast<WaveForkFaultContext*>(opaque);
    if (point != context.target || context.invoked) {
        return false;
    }
    context.invoked = true;
    const pid_t child = ::fork();
    if (child == 0) {
        return false;
    }
    context.child_process = child;
    return true;
}

struct WaveUnknownFaultContext final {
    wave_detail::DistributedSieveWaveStoreFaultPoint target =
        wave_detail::DistributedSieveWaveStoreFaultPoint::RootDurable;
    std::filesystem::path unknown_leaf;
    bool inserted = false;
};

[[nodiscard]] bool
insert_unknown_at_wave_fault(wave_detail::DistributedSieveWaveStoreFaultPoint point,
                             void* opaque) noexcept {
    auto& context = *static_cast<WaveUnknownFaultContext*>(opaque);
    if (point != context.target || context.inserted) {
        return false;
    }
    const int descriptor =
        ::open(context.unknown_leaf.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }
    constexpr char byte = 'x';
    ssize_t count = -1;
    do {
        count = ::write(descriptor, &byte, 1);
    } while (count < 0 && errno == EINTR);
    const int saved_errno = errno;
    (void)::close(descriptor);
    errno = saved_errno;
    context.inserted = count == 1;
    return false;
}

struct WaveBaseLockReplacementContext final {
    std::filesystem::path canonical;
    std::filesystem::path displaced;
    int native_error = 0;
    bool invoked = false;
    bool replaced = false;
};

void replace_base_lock_after_first_inventory(void* opaque) noexcept {
    auto& context = *static_cast<WaveBaseLockReplacementContext*>(opaque);
    if (context.invoked) {
        return;
    }
    context.invoked = true;
    int renamed = -1;
    do {
        renamed = ::rename(context.canonical.c_str(), context.displaced.c_str());
    } while (renamed != 0 && errno == EINTR);
    if (renamed != 0) {
        context.native_error = errno;
        return;
    }

    int descriptor = -1;
    do {
        descriptor = ::open(context.canonical.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        context.native_error = errno;
        (void)::rename(context.displaced.c_str(), context.canonical.c_str());
        return;
    }
    if (::fchmod(descriptor, 0600) != 0) {
        context.native_error = errno;
        (void)::close(descriptor);
        return;
    }
    if (::close(descriptor) != 0) {
        context.native_error = errno;
        return;
    }
    context.replaced = true;
}

struct WaveSameBytesReplacementContext final {
    std::filesystem::path canonical;
    std::filesystem::path displaced;
    std::vector<std::byte> bytes;
    int native_error = 0;
    bool invoked = false;
    bool replaced = false;
};

void replace_marker_with_same_bytes_after_first_inventory(void* opaque) noexcept {
    auto& context = *static_cast<WaveSameBytesReplacementContext*>(opaque);
    if (context.invoked) {
        return;
    }
    context.invoked = true;
    int renamed = -1;
    do {
        renamed = ::rename(context.canonical.c_str(), context.displaced.c_str());
    } while (renamed != 0 && errno == EINTR);
    if (renamed != 0) {
        context.native_error = errno;
        return;
    }

    int descriptor = -1;
    do {
        descriptor = ::open(context.canonical.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        context.native_error = errno;
        return;
    }
    std::size_t offset = 0;
    while (offset < context.bytes.size()) {
        const ssize_t written =
            ::write(descriptor, context.bytes.data() + offset, context.bytes.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            context.native_error = errno;
            (void)::close(descriptor);
            return;
        }
        if (written == 0) {
            context.native_error = EIO;
            (void)::close(descriptor);
            return;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fchmod(descriptor, 0600) != 0) {
        context.native_error = errno;
        (void)::close(descriptor);
        return;
    }
    if (::close(descriptor) != 0) {
        context.native_error = errno;
        return;
    }
    context.replaced = true;
}

struct WaveRootReplacementContext final {
    std::filesystem::path canonical;
    std::filesystem::path displaced;
    int native_error = 0;
    bool invoked = false;
    bool replaced = false;
};

void replace_wave_root_after_attempt_phase(void* opaque) noexcept {
    auto& context = *static_cast<WaveRootReplacementContext*>(opaque);
    if (context.invoked) {
        return;
    }
    context.invoked = true;
    int renamed = -1;
    do {
        renamed = ::rename(context.canonical.c_str(), context.displaced.c_str());
    } while (renamed != 0 && errno == EINTR);
    if (renamed != 0) {
        context.native_error = errno;
        return;
    }

    int created = -1;
    do {
        created = ::mkdir(context.canonical.c_str(), 0700);
    } while (created != 0 && errno == EINTR);
    if (created != 0) {
        context.native_error = errno;
        return;
    }
    int chmod_result = -1;
    do {
        chmod_result = ::chmod(context.canonical.c_str(), 0700);
    } while (chmod_result != 0 && errno == EINTR);
    if (chmod_result != 0) {
        context.native_error = errno;
        return;
    }
    context.replaced = true;
}

struct AttemptPhaseForeignContext final {
    std::filesystem::path foreign;
    int native_error = 0;
    bool invoked = false;
    bool inserted = false;
};

void insert_foreign_after_attempt_phase(void* opaque) noexcept {
    auto& context = *static_cast<AttemptPhaseForeignContext*>(opaque);
    if (context.invoked) {
        return;
    }
    context.invoked = true;
    int descriptor = -1;
    do {
        descriptor = ::open(context.foreign.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        context.native_error = errno;
        return;
    }
    if (::fchmod(descriptor, 0600) != 0) {
        context.native_error = errno;
        (void)::close(descriptor);
        return;
    }
    if (::close(descriptor) != 0) {
        context.native_error = errno;
        return;
    }
    context.inserted = true;
}

struct PrivateLeaseProtocolForeignContext final {
    wave_detail::DistributedSievePrivateLeaseReservationBoundary successor =
        wave_detail::DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable;
    AttemptPhaseForeignContext foreign;
};

void insert_foreign_after_private_lease_predecessor(
    wave_detail::DistributedSievePrivateLeaseReservationBoundary successor, void* opaque) noexcept {
    auto& context = *static_cast<PrivateLeaseProtocolForeignContext*>(opaque);
    if (successor == context.successor) {
        insert_foreign_after_attempt_phase(&context.foreign);
    }
}

struct PrivateLeaseProtocolSyncFailureContext final {
    PrivateLeaseReservationSyncFailureSite target{
        .boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable,
        .point = wave_detail::DistributedSievePrivateLeaseReservationSyncPoint::MarkerFileInitial,
    };
    std::array<PrivateLeaseReservationSyncFailureSite,
               PRIVATE_LEASE_RESERVATION_SYNC_FAILURE_SITES.size()>
        observed{};
    std::size_t observed_count = 0;
    bool selected = false;
};

[[nodiscard]] bool fail_private_lease_reservation_sync(
    wave_detail::DistributedSievePrivateLeaseReservationBoundary boundary,
    wave_detail::DistributedSievePrivateLeaseReservationSyncPoint point, void* opaque) noexcept {
    auto& context = *static_cast<PrivateLeaseProtocolSyncFailureContext*>(opaque);
    if (context.observed_count < context.observed.size()) {
        context.observed[context.observed_count] = {
            .boundary = boundary,
            .point = point,
        };
    }
    ++context.observed_count;
    const bool selected = boundary == context.target.boundary && point == context.target.point;
    context.selected = context.selected || selected;
    return selected;
}

struct PrivateLeaseProtocolSyncRootReplacementContext final {
    PrivateLeaseProtocolSyncFailureContext sync;
    WaveRootReplacementContext root;
    wave_detail::DistributedSievePrivateLeaseReservationSyncFailureSite observed_site;
    bool after_failure_invoked = false;
};

[[nodiscard]] bool select_private_lease_sync_for_root_precedence(
    wave_detail::DistributedSievePrivateLeaseReservationBoundary boundary,
    wave_detail::DistributedSievePrivateLeaseReservationSyncPoint point, void* opaque) noexcept {
    auto& context = *static_cast<PrivateLeaseProtocolSyncRootReplacementContext*>(opaque);
    return fail_private_lease_reservation_sync(boundary, point, &context.sync);
}

void replace_root_after_injected_private_lease_sync_failure(
    wave_detail::DistributedSievePrivateLeaseReservationSyncFailureSite site,
    void* opaque) noexcept {
    auto& context = *static_cast<PrivateLeaseProtocolSyncRootReplacementContext*>(opaque);
    context.after_failure_invoked = true;
    context.observed_site = site;
    replace_wave_root_after_attempt_phase(&context.root);
}

struct PrivateLeaseSuccessorMarkerReplacementContext final {
    wave_detail::DistributedSievePrivateLeaseReservationBoundary target =
        wave_detail::DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable;
    std::filesystem::path canonical;
    std::filesystem::path displaced;
    int native_error = 0;
    bool invoked = false;
    bool replaced = false;
};

void replace_private_lease_successor_marker_after_first_validation(
    wave_detail::DistributedSievePrivateLeaseReservationBoundary successor, void* opaque) noexcept {
    auto& context = *static_cast<PrivateLeaseSuccessorMarkerReplacementContext*>(opaque);
    if (successor != context.target || context.invoked) {
        return;
    }
    context.invoked = true;
    std::array<std::byte, cleanup_detail::PRIVATE_LEASE_MARKER_BYTES> bytes{};
    int source = -1;
    do {
        source = ::open(context.canonical.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    } while (source < 0 && errno == EINTR);
    if (source < 0) {
        context.native_error = errno;
        return;
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        ssize_t count = -1;
        do {
            count = ::pread(source, bytes.data() + offset, bytes.size() - offset,
                            static_cast<off_t>(offset));
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            context.native_error = count < 0 ? errno : EIO;
            (void)::close(source);
            return;
        }
        offset += static_cast<std::size_t>(count);
    }
    std::byte extra{};
    ssize_t extra_count = -1;
    do {
        extra_count = ::pread(source, &extra, 1, static_cast<off_t>(bytes.size()));
    } while (extra_count < 0 && errno == EINTR);
    if (extra_count != 0) {
        context.native_error = extra_count < 0 ? errno : EFBIG;
        (void)::close(source);
        return;
    }
    if (::close(source) != 0) {
        context.native_error = errno;
        return;
    }

    int renamed = -1;
    do {
        renamed = ::rename(context.canonical.c_str(), context.displaced.c_str());
    } while (renamed != 0 && errno == EINTR);
    if (renamed != 0) {
        context.native_error = errno;
        return;
    }
    int replacement = -1;
    do {
        replacement = ::open(context.canonical.c_str(),
                             O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    } while (replacement < 0 && errno == EINTR);
    if (replacement < 0) {
        context.native_error = errno;
        return;
    }
    offset = 0;
    while (offset < bytes.size()) {
        ssize_t written = -1;
        do {
            written = ::write(replacement, bytes.data() + offset, bytes.size() - offset);
        } while (written < 0 && errno == EINTR);
        if (written <= 0) {
            context.native_error = written < 0 ? errno : EIO;
            (void)::close(replacement);
            return;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fchmod(replacement, 0600) != 0) {
        context.native_error = errno;
        (void)::close(replacement);
        return;
    }
    if (::close(replacement) != 0) {
        context.native_error = errno;
        return;
    }
    context.replaced = true;
}

struct PrivateLeaseSuccessorDirectoryReplacementContext final {
    wave_detail::DistributedSievePrivateLeaseReservationBoundary target =
        wave_detail::DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable;
    std::filesystem::path root;
    std::string relative_lease_stem;
    std::filesystem::path canonical;
    std::filesystem::path displaced;
    int native_error = 0;
    bool invoked = false;
    bool replaced = false;
};

void replace_private_lease_successor_directory_after_first_validation(
    wave_detail::DistributedSievePrivateLeaseReservationBoundary successor, void* opaque) noexcept {
    auto& context = *static_cast<PrivateLeaseSuccessorDirectoryReplacementContext*>(opaque);
    if (successor != context.target || context.invoked) {
        return;
    }
    context.invoked = true;
    if (context.canonical.empty()) {
        try {
            const std::string prefix =
                context.relative_lease_stem +
                std::string(wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG);
            for (const auto& entry : std::filesystem::directory_iterator(context.root)) {
                const std::string leaf = entry.path().filename().string();
                if (!leaf.starts_with(prefix)) {
                    continue;
                }
                if (!context.canonical.empty()) {
                    context.native_error = EEXIST;
                    return;
                }
                context.canonical = entry.path();
            }
        } catch (const std::filesystem::filesystem_error& failure) {
            context.native_error = failure.code().value();
            return;
        } catch (...) {
            context.native_error = EIO;
            return;
        }
    }
    if (context.canonical.empty()) {
        context.native_error = ENOENT;
        return;
    }
    int renamed = -1;
    do {
        renamed = ::rename(context.canonical.c_str(), context.displaced.c_str());
    } while (renamed != 0 && errno == EINTR);
    if (renamed != 0) {
        context.native_error = errno;
        return;
    }
    int created = -1;
    do {
        created = ::mkdir(context.canonical.c_str(), 0700);
    } while (created != 0 && errno == EINTR);
    if (created != 0) {
        context.native_error = errno;
        return;
    }
    int chmod_result = -1;
    do {
        chmod_result = ::chmod(context.canonical.c_str(), 0700);
    } while (chmod_result != 0 && errno == EINTR);
    if (chmod_result != 0) {
        context.native_error = errno;
        return;
    }
    context.replaced = true;
}

struct PrivateLeaseSuccessorRootReplacementContext final {
    wave_detail::DistributedSievePrivateLeaseReservationBoundary target =
        wave_detail::DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable;
    WaveRootReplacementContext root;
};

void replace_private_lease_root_after_first_successor_validation(
    wave_detail::DistributedSievePrivateLeaseReservationBoundary successor, void* opaque) noexcept {
    auto& context = *static_cast<PrivateLeaseSuccessorRootReplacementContext*>(opaque);
    if (successor == context.target) {
        replace_wave_root_after_attempt_phase(&context.root);
    }
}

struct BaseLockSyncFailureContext final {
    wave_detail::DistributedSievePrivateLeaseBaseLockSyncPoint target =
        wave_detail::DistributedSievePrivateLeaseBaseLockSyncPoint::TargetInitial;
    std::array<bool, PRIVATE_LEASE_BASE_LOCK_SYNC_POINTS.size()> observed{};
};

[[nodiscard]] bool
fail_before_base_lock_sync(wave_detail::DistributedSievePrivateLeaseBaseLockSyncPoint point,
                           void* opaque) noexcept {
    auto& context = *static_cast<BaseLockSyncFailureContext*>(opaque);
    const auto index = static_cast<std::size_t>(point);
    if (index < context.observed.size()) {
        context.observed[index] = true;
    }
    return point == context.target;
}

struct MixedAttemptFailureContext final {
    WaveBaseLockReplacementContext target;
    WaveBaseLockReplacementContext wave_lock;
    WaveRootReplacementContext root;
    AttemptPhaseForeignContext foreign;
    BaseLockSyncFailureContext sync;
};

void replace_attempt_target_and_root_after_lock(void* opaque) noexcept {
    auto& context = *static_cast<MixedAttemptFailureContext*>(opaque);
    replace_base_lock_after_first_inventory(&context.target);
    if (context.target.replaced) {
        replace_wave_root_after_attempt_phase(&context.root);
    }
}

void replace_attempt_target_and_wave_lock_after_lock(void* opaque) noexcept {
    auto& context = *static_cast<MixedAttemptFailureContext*>(opaque);
    replace_base_lock_after_first_inventory(&context.target);
    if (context.target.replaced) {
        replace_base_lock_after_first_inventory(&context.wave_lock);
    }
}

void replace_attempt_root_and_wave_lock_after_lock(void* opaque) noexcept {
    auto& context = *static_cast<MixedAttemptFailureContext*>(opaque);
    replace_base_lock_after_first_inventory(&context.wave_lock);
    if (context.wave_lock.replaced) {
        replace_wave_root_after_attempt_phase(&context.root);
    }
}

[[nodiscard]] bool replace_root_and_fail_before_base_lock_sync(
    wave_detail::DistributedSievePrivateLeaseBaseLockSyncPoint point, void* opaque) noexcept {
    auto& context = *static_cast<MixedAttemptFailureContext*>(opaque);
    const bool fail = fail_before_base_lock_sync(point, &context.sync);
    if (fail) {
        replace_wave_root_after_attempt_phase(&context.root);
    }
    return fail;
}

[[nodiscard]] bool replace_target_insert_foreign_and_fail_before_base_lock_sync(
    wave_detail::DistributedSievePrivateLeaseBaseLockSyncPoint point, void* opaque) noexcept {
    auto& context = *static_cast<MixedAttemptFailureContext*>(opaque);
    const bool fail = fail_before_base_lock_sync(point, &context.sync);
    if (fail) {
        replace_base_lock_after_first_inventory(&context.target);
        insert_foreign_after_attempt_phase(&context.foreign);
    }
    return fail;
}

[[nodiscard]] bool relation_base_lock_reports_busy(const std::filesystem::path& path) {
    try {
        cleanup_detail::BaseLock contender(path, false);
        return false;
    } catch (const cleanup_detail::Failure& failure) {
        return failure.status == gnfs::relation::OOCCleanupStatus::Busy;
    }
}

void require_strict_empty_base_lock(const std::filesystem::path& path, std::string_view context) {
    struct stat metadata{};
    if (::lstat(path.c_str(), &metadata) != 0) {
        throw std::system_error(errno, std::generic_category(), std::string(context));
    }
    CHECK(S_ISREG(metadata.st_mode));
    CHECK(metadata.st_nlink == 1);
    CHECK(metadata.st_size == 0);
    CHECK(static_cast<std::uint64_t>(metadata.st_uid) == static_cast<std::uint64_t>(::geteuid()));
    CHECK((metadata.st_mode & static_cast<mode_t>(07777)) == 0600);
}

void require_distinct_entry_identities(const std::filesystem::path& first,
                                       const std::filesystem::path& second,
                                       std::string_view context) {
    struct stat first_metadata{};
    struct stat second_metadata{};
    if (::lstat(first.c_str(), &first_metadata) != 0 ||
        ::lstat(second.c_str(), &second_metadata) != 0) {
        throw std::system_error(errno, std::generic_category(), std::string(context));
    }
    CHECK(first_metadata.st_dev == second_metadata.st_dev);
    CHECK(first_metadata.st_ino != second_metadata.st_ino);
}

struct WaveRootEntrySnapshot final {
    std::string leaf;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t link_count = 0;
    std::uint64_t size = 0;
    std::uint64_t owner = 0;
    std::uint64_t group = 0;
    std::uint64_t device_type = 0;
    mode_t mode = 0;
    std::vector<std::byte> bytes;
    std::string symlink_target;

    friend bool operator==(const WaveRootEntrySnapshot&, const WaveRootEntrySnapshot&) = default;
};

using WaveRootSnapshot = std::vector<WaveRootEntrySnapshot>;

[[nodiscard]] bool same_wave_snapshot_metadata(const struct stat& lhs,
                                               const struct stat& rhs) noexcept {
    return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino && lhs.st_nlink == rhs.st_nlink &&
           lhs.st_mode == rhs.st_mode && lhs.st_uid == rhs.st_uid && lhs.st_gid == rhs.st_gid &&
           lhs.st_size == rhs.st_size && lhs.st_rdev == rhs.st_rdev;
}

class WaveSnapshotFd final {
public:
    explicit WaveSnapshotFd(int descriptor) noexcept : descriptor_(descriptor) {}
    ~WaveSnapshotFd() {
        if (descriptor_ >= 0) {
            (void)::close(descriptor_);
        }
    }

    WaveSnapshotFd(const WaveSnapshotFd&) = delete;
    WaveSnapshotFd& operator=(const WaveSnapshotFd&) = delete;

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_ = -1;
};

class WaveSnapshotDirectory final {
public:
    explicit WaveSnapshotDirectory(DIR* directory) noexcept : directory_(directory) {}
    ~WaveSnapshotDirectory() {
        if (directory_ != nullptr) {
            (void)::closedir(directory_);
        }
    }

    WaveSnapshotDirectory(const WaveSnapshotDirectory&) = delete;
    WaveSnapshotDirectory& operator=(const WaveSnapshotDirectory&) = delete;

    [[nodiscard]] DIR* get() const noexcept {
        return directory_;
    }

private:
    DIR* directory_ = nullptr;
};

[[nodiscard]] WaveRootEntrySnapshot
wave_root_entry_snapshot_from_metadata(const struct stat& metadata, std::string leaf) {
    return {
        .leaf = std::move(leaf),
        .device = static_cast<std::uint64_t>(metadata.st_dev),
        .inode = static_cast<std::uint64_t>(metadata.st_ino),
        .link_count = static_cast<std::uint64_t>(metadata.st_nlink),
        .size = static_cast<std::uint64_t>(metadata.st_size),
        .owner = static_cast<std::uint64_t>(metadata.st_uid),
        .group = static_cast<std::uint64_t>(metadata.st_gid),
        .device_type = static_cast<std::uint64_t>(metadata.st_rdev),
        .mode = metadata.st_mode,
    };
}

[[nodiscard]] WaveRootEntrySnapshot capture_wave_root_entry_snapshot_at(int root_fd,
                                                                        std::string_view named_leaf,
                                                                        std::string snapshot_leaf) {
    if (named_leaf.empty() || named_leaf == "." || named_leaf == ".." ||
        named_leaf.find('/') != std::string_view::npos ||
        named_leaf.find('\0') != std::string_view::npos) {
        throw TestFailure("invalid anchored wave-store snapshot leaf");
    }
    const std::string native_leaf(named_leaf);
    struct stat named_before{};
    if (::fstatat(root_fd, native_leaf.c_str(), &named_before, AT_SYMLINK_NOFOLLOW) != 0) {
        throw std::system_error(errno, std::generic_category(), "snapshot wave-store namespace");
    }

    auto entry = wave_root_entry_snapshot_from_metadata(named_before, std::move(snapshot_leaf));
    if (S_ISREG(named_before.st_mode)) {
        int descriptor = -1;
        do {
            descriptor = ::openat(root_fd, native_leaf.c_str(),
                                  O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        } while (descriptor < 0 && errno == EINTR);
        if (descriptor < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "open wave-store snapshot leaf");
        }
        WaveSnapshotFd held(descriptor);

        struct stat held_before{};
        if (::fstat(held.get(), &held_before) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "stat wave-store snapshot leaf");
        }
        if (!S_ISREG(held_before.st_mode) ||
            !same_wave_snapshot_metadata(named_before, held_before) || held_before.st_size < 0 ||
            static_cast<std::uintmax_t>(held_before.st_size) >
                std::numeric_limits<std::size_t>::max()) {
            throw TestFailure("wave-store snapshot leaf identity changed before read: " +
                              native_leaf);
        }
        entry.bytes.resize(static_cast<std::size_t>(held_before.st_size));
        std::size_t offset = 0;
        while (offset < entry.bytes.size()) {
            const std::size_t request =
                std::min(entry.bytes.size() - offset,
                         static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            ssize_t count = -1;
            do {
                count = ::pread(held.get(), entry.bytes.data() + offset, request,
                                static_cast<off_t>(offset));
            } while (count < 0 && errno == EINTR);
            if (count <= 0) {
                throw TestFailure("cannot read complete wave-store snapshot leaf: " + native_leaf);
            }
            offset += static_cast<std::size_t>(count);
        }

        struct stat held_after{};
        struct stat named_after{};
        if (::fstat(held.get(), &held_after) != 0 ||
            ::fstatat(root_fd, native_leaf.c_str(), &named_after, AT_SYMLINK_NOFOLLOW) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "revalidate wave-store snapshot leaf");
        }
        if (!same_wave_snapshot_metadata(held_before, held_after) ||
            !same_wave_snapshot_metadata(held_after, named_after)) {
            throw TestFailure("wave-store snapshot leaf identity changed during read: " +
                              native_leaf);
        }
    } else if (S_ISLNK(named_before.st_mode)) {
        if (named_before.st_size < 0) {
            throw TestFailure("wave-store snapshot symlink has a negative size");
        }
        std::size_t capacity =
            std::max<std::size_t>(256, static_cast<std::size_t>(named_before.st_size) + 1U);
        std::vector<char> target(capacity);
        for (;;) {
            ssize_t count = -1;
            do {
                count = ::readlinkat(root_fd, native_leaf.c_str(), target.data(), target.size());
            } while (count < 0 && errno == EINTR);
            if (count < 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "read anchored wave-store snapshot symlink");
            }
            if (static_cast<std::size_t>(count) < target.size()) {
                entry.symlink_target.assign(target.data(), static_cast<std::size_t>(count));
                break;
            }
            if (target.size() > 1024U * 1024U) {
                throw TestFailure("wave-store snapshot symlink target exceeds test bound");
            }
            target.resize(target.size() * 2U);
        }
        struct stat named_after{};
        if (::fstatat(root_fd, native_leaf.c_str(), &named_after, AT_SYMLINK_NOFOLLOW) != 0 ||
            !same_wave_snapshot_metadata(named_before, named_after)) {
            throw TestFailure("wave-store snapshot symlink changed during read: " + native_leaf);
        }
    } else {
        struct stat named_after{};
        if (::fstatat(root_fd, native_leaf.c_str(), &named_after, AT_SYMLINK_NOFOLLOW) != 0 ||
            !same_wave_snapshot_metadata(named_before, named_after)) {
            throw TestFailure("wave-store snapshot entry changed during observation: " +
                              native_leaf);
        }
    }
    return entry;
}

[[nodiscard]] WaveRootEntrySnapshot
capture_wave_root_entry_snapshot(const std::filesystem::path& path, std::string snapshot_leaf) {
    const auto parent = path.parent_path();
    const auto named_leaf = path.filename().string();
    int parent_fd = -1;
    do {
        parent_fd =
            ::open(parent.c_str(), O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (parent_fd < 0 && errno == EINTR);
    if (parent_fd < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "open anchored wave-store snapshot parent");
    }
    WaveSnapshotFd held_parent(parent_fd);
    return capture_wave_root_entry_snapshot_at(held_parent.get(), named_leaf,
                                               std::move(snapshot_leaf));
}

[[nodiscard]] WaveRootSnapshot capture_wave_root_snapshot(const std::filesystem::path& root) {
    int root_fd = -1;
    do {
        root_fd =
            ::open(root.c_str(), O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (root_fd < 0 && errno == EINTR);
    if (root_fd < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "open anchored wave-store snapshot root");
    }
    WaveSnapshotFd held_root(root_fd);
    struct stat root_before{};
    if (::fstat(held_root.get(), &root_before) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "stat anchored wave-store snapshot root");
    }
    if (!S_ISDIR(root_before.st_mode)) {
        throw std::system_error(ENOTDIR, std::generic_category(),
                                "stat anchored wave-store snapshot root");
    }

    WaveRootSnapshot snapshot;
    auto root_entry = wave_root_entry_snapshot_from_metadata(root_before, ".");
    root_entry.link_count = 0;
    root_entry.size = 0;
    root_entry.device_type = 0;
    snapshot.push_back(std::move(root_entry));

    int enumeration_fd = -1;
    do {
#if defined(F_DUPFD_CLOEXEC)
        enumeration_fd = ::fcntl(held_root.get(), F_DUPFD_CLOEXEC, 0);
#else
        enumeration_fd = ::dup(held_root.get());
#endif
    } while (enumeration_fd < 0 && errno == EINTR);
    if (enumeration_fd < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "duplicate wave-store snapshot root");
    }
    DIR* raw_directory = ::fdopendir(enumeration_fd);
    if (raw_directory == nullptr) {
        const int saved_errno = errno;
        (void)::close(enumeration_fd);
        throw std::system_error(saved_errno, std::generic_category(),
                                "enumerate anchored wave-store snapshot root");
    }
    WaveSnapshotDirectory directory(raw_directory);
    std::vector<std::string> leaves;
    for (;;) {
        errno = 0;
        const dirent* entry = ::readdir(directory.get());
        if (entry == nullptr) {
            if (errno != 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "read anchored wave-store snapshot root");
            }
            break;
        }
        const std::string_view leaf(entry->d_name);
        if (leaf != "." && leaf != "..") {
            leaves.emplace_back(leaf);
        }
    }
    std::ranges::sort(leaves);
    for (const auto& leaf : leaves) {
        snapshot.push_back(capture_wave_root_entry_snapshot_at(held_root.get(), leaf, leaf));
    }

    struct stat root_after{};
    struct stat named_root_after{};
    if (::fstat(held_root.get(), &root_after) != 0 ||
        ::lstat(root.c_str(), &named_root_after) != 0 || root_before.st_dev != root_after.st_dev ||
        root_before.st_ino != root_after.st_ino || root_before.st_mode != root_after.st_mode ||
        root_before.st_uid != root_after.st_uid || root_before.st_gid != root_after.st_gid ||
        root_after.st_dev != named_root_after.st_dev ||
        root_after.st_ino != named_root_after.st_ino ||
        root_after.st_mode != named_root_after.st_mode ||
        root_after.st_uid != named_root_after.st_uid ||
        root_after.st_gid != named_root_after.st_gid) {
        throw TestFailure("wave-store snapshot root identity changed during observation");
    }
    std::ranges::sort(snapshot, {}, &WaveRootEntrySnapshot::leaf);
    return snapshot;
}

void erase_wave_root_snapshot_leaf(WaveRootSnapshot& snapshot, std::string_view leaf) {
    const auto found = std::ranges::find(snapshot, leaf, &WaveRootEntrySnapshot::leaf);
    if (found == snapshot.end()) {
        fail("snapshot contains expected wave-store leaf", __LINE__, leaf);
    }
    snapshot.erase(found);
}

[[nodiscard]] const WaveRootEntrySnapshot&
require_wave_root_snapshot_leaf(const WaveRootSnapshot& snapshot, std::string_view leaf) {
    const auto found = std::ranges::find(snapshot, leaf, &WaveRootEntrySnapshot::leaf);
    if (found == snapshot.end()) {
        fail("snapshot contains requested wave-store leaf", __LINE__, leaf);
    }
    return *found;
}

#endif

void check_wave_fault_prefix(const std::filesystem::path& root,
                             wave_detail::DistributedSieveWaveStoreFaultPoint point) {
    const bool root_present = entry_exists_no_follow(root);
    const bool lock_present = root_present && entry_exists_no_follow(wave_lock_path(root));
    const bool pending_present =
        root_present && entry_exists_no_follow(wave_manifest_pending_path(root));
    const bool canonical_present = root_present && entry_exists_no_follow(wave_manifest_path(root));

    CHECK(root_present);
    switch (point) {
    case wave_detail::DistributedSieveWaveStoreFaultPoint::RootDurable:
        CHECK(!lock_present);
        CHECK(!pending_present);
        CHECK(!canonical_present);
        return;
    case wave_detail::DistributedSieveWaveStoreFaultPoint::LockDurable:
        CHECK(lock_present);
        CHECK(!pending_present);
        CHECK(!canonical_present);
        return;
    case wave_detail::DistributedSieveWaveStoreFaultPoint::ManifestPendingDurable:
        CHECK(lock_present);
        CHECK(pending_present);
        CHECK(!canonical_present);
        return;
    case wave_detail::DistributedSieveWaveStoreFaultPoint::ManifestCanonicalPromoted:
    case wave_detail::DistributedSieveWaveStoreFaultPoint::ManifestCanonicalDurable:
        CHECK(lock_present);
        CHECK(!pending_present);
        CHECK(canonical_present);
        return;
    case wave_detail::DistributedSieveWaveStoreFaultPoint::Count:
        break;
    }
    fail("closed wave fault point", __LINE__);
}

void test_wave_store_create_open_revalidate_and_exact_manifest() {
    CHECK(wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF == ".gnfs-wave-v1.lock");
    CHECK(wave_detail::DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF == ".gnfs-wave-v1.manifest");
    CHECK(wave_detail::DISTRIBUTED_SIEVE_WAVE_MANIFEST_PENDING_LEAF ==
          ".gnfs-wave-v1.manifest.pending");

    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "create-open";
    const auto draft = wave_manifest_draft();
    CHECK(draft.wave_root_identity == sieve::NativeIdentityV1{});
    CHECK(draft.permanent_lock_identity == sieve::NativeIdentityV1{});
    CHECK(draft.lock_semantics_version == 0);
    CHECK(draft.self_digest == Digest{});

    auto created = wave_detail::DistributedSieveWaveStore::create(root, draft);
    auto& store = require_wave_ready(created, "create wave store");
    CHECK(store.absolute_root() == root);
    CHECK(store.manifest().wave_root_identity == store.wave_root_identity());
    CHECK(store.manifest().permanent_lock_identity == store.permanent_lock_identity());
    CHECK(store.manifest().lock_semantics_version ==
          wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_SEMANTICS_VERSION_V1);
    CHECK(store.manifest().self_digest == store.manifest_digest());
    CHECK(store.manifest_digest() != Digest{});

    auto independently_sealed = store.manifest();
    independently_sealed.self_digest = {};
    independently_sealed = seal_value(std::move(independently_sealed));
    CHECK(encode_or_fail(Record{independently_sealed}) == encode_or_fail(Record{store.manifest()}));
    CHECK(record_digest_or_fail(Record{store.manifest()}) == store.manifest_digest());

    const auto exact_manifest_bytes = encode_or_fail(Record{store.manifest()});
    CHECK(read_file_bytes(wave_manifest_path(root)) == exact_manifest_bytes);
    CHECK(store.manifest_snapshot().size == exact_manifest_bytes.size());
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "revalidate newly created wave");

    const Digest digest = store.manifest_digest();
    const auto root_identity = store.wave_root_identity();
    const auto lock_identity = store.permanent_lock_identity();
    const auto manifest_snapshot = store.manifest_snapshot();
    created.store.reset();

    auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
    auto& reopened = require_wave_ready(opened, "open exact wave store");
    CHECK(reopened.manifest_digest() == digest);
    CHECK(reopened.wave_root_identity() == root_identity);
    CHECK(reopened.permanent_lock_identity() == lock_identity);
    CHECK(reopened.manifest_snapshot() == manifest_snapshot);
    CHECK(read_file_bytes(wave_manifest_path(root)) == exact_manifest_bytes);
    require_wave_status(reopened.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "revalidate reopened wave");
}

void test_wave_store_rejects_non_draft_store_owned_fields() {
    WaveStoreTempDirectory temp;
    std::uint64_t index = 0;
    const auto reject = [&](auto mutate) {
        auto draft = wave_manifest_draft();
        mutate(draft);
        const auto root = temp.path() / ("forged-draft-" + std::to_string(index++));
        auto result = wave_detail::DistributedSieveWaveStore::create(root, std::move(draft));
        CHECK(!result);
        CHECK(result.store == nullptr);
        require_wave_status(result.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::invalid_request,
                            "reject invalid wave manifest draft");
        CHECK(!entry_exists_no_follow(root));
    };

    reject([](auto& draft) { draft.wave_root_identity = native_identity(8000); });
    reject([](auto& draft) { draft.permanent_lock_identity = native_identity(8010); });
    reject([](auto& draft) { draft.lock_semantics_version = 1; });
    reject([](auto& draft) { draft.self_digest = digest_with_seed(55); });
    reject([](auto& draft) { draft.worker_count = 0; });
}

void test_wave_store_rejects_zero_open_digest_without_observation() {
    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "zero-open-digest";
    auto opened = wave_detail::DistributedSieveWaveStore::open(root, Digest{});
    CHECK(!opened);
    CHECK(opened.store == nullptr);
    require_wave_status(opened.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::invalid_request,
                        "zero manifest digest is not open authority");
    CHECK(!entry_exists_no_follow(root));

#if !defined(_WIN32)
    const auto existing_root = temp.path() / "zero-open-digest-existing";
    (void)create_closed_wave(existing_root);
    const auto manifest_before = read_file_bytes(wave_manifest_path(existing_root));
    auto existing_opened = wave_detail::DistributedSieveWaveStore::open(existing_root, Digest{});
    CHECK(!existing_opened);
    CHECK(existing_opened.store == nullptr);
    require_wave_status(existing_opened.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::invalid_request,
                        "zero manifest digest does not inspect an existing wave");
    CHECK(read_file_bytes(wave_manifest_path(existing_root)) == manifest_before);
#endif
}

void test_wave_store_all_durable_prefixes_recover_exactly() {
    for (const auto point : WAVE_STORE_FAULT_POINTS) {
        WaveStoreTempDirectory temp;
        const auto root =
            temp.path() / ("fault-" + std::to_string(static_cast<std::size_t>(point)));
        WaveFaultStopContext context{.target = point};
        auto interrupted = wave_detail::DistributedSieveWaveStore::create(
            root, wave_manifest_draft(),
            wave_detail::DistributedSieveWaveStoreTestHooks{
                .stop_after = stop_at_wave_fault,
                .context = &context,
            });
        CHECK(!interrupted);
        CHECK(interrupted.store == nullptr);
        require_wave_status(interrupted.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::interrupted,
                            "fault point interrupts create");
        CHECK(interrupted.diagnostic.last_durable_fault_point == point);
        CHECK(context.observed[static_cast<std::size_t>(point)]);
        check_wave_fault_prefix(root, point);

        wave_detail::DistributedSieveWaveStoreOpenResult recovered =
            point == wave_detail::DistributedSieveWaveStoreFaultPoint::RootDurable ||
                    point == wave_detail::DistributedSieveWaveStoreFaultPoint::LockDurable
                ? wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft())
                : wave_detail::DistributedSieveWaveStore::open(
                      root, manifest_digest_from_file(
                                point == wave_detail::DistributedSieveWaveStoreFaultPoint::
                                             ManifestPendingDurable
                                    ? wave_manifest_pending_path(root)
                                    : wave_manifest_path(root)));
        auto& store = require_wave_ready(recovered, "recover durable create prefix");
        CHECK(entry_exists_no_follow(wave_lock_path(root)));
        CHECK(entry_exists_no_follow(wave_manifest_path(root)));
        CHECK(!entry_exists_no_follow(wave_manifest_pending_path(root)));
        const auto exact_bytes = encode_or_fail(Record{store.manifest()});
        CHECK(read_file_bytes(wave_manifest_path(root)) == exact_bytes);
        const Digest digest = store.manifest_digest();
        recovered.store.reset();

        auto reopened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        auto& exact = require_wave_ready(reopened, "open recovered durable prefix");
        CHECK(exact.manifest_digest() == digest);
        require_wave_status(exact.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "revalidate recovered durable prefix");
    }
}

#if !defined(_WIN32)

void test_wave_store_revalidates_after_root_and_lock_hooks() {
    const std::array points{
        wave_detail::DistributedSieveWaveStoreFaultPoint::RootDurable,
        wave_detail::DistributedSieveWaveStoreFaultPoint::LockDurable,
    };
    for (const auto point : points) {
        WaveStoreTempDirectory temp;
        const auto root =
            temp.path() / ("hook-drift-" + std::to_string(static_cast<std::size_t>(point)));
        const auto unknown = root / "unexpected.control";
        WaveUnknownFaultContext context{
            .target = point,
            .unknown_leaf = unknown,
        };
        auto result = wave_detail::DistributedSieveWaveStore::create(
            root, wave_manifest_draft(),
            wave_detail::DistributedSieveWaveStoreTestHooks{
                .stop_after = insert_unknown_at_wave_fault,
                .context = &context,
            });
        CHECK(context.inserted);
        CHECK(!result);
        CHECK(result.store == nullptr);
        require_wave_status(result.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "hook namespace drift blocks the next mutation");
        CHECK(entry_exists_no_follow(unknown));
        CHECK(!entry_exists_no_follow(wave_manifest_pending_path(root)));
        CHECK(!entry_exists_no_follow(wave_manifest_path(root)));
        CHECK(entry_exists_no_follow(wave_lock_path(root)) ==
              (point == wave_detail::DistributedSieveWaveStoreFaultPoint::LockDurable));
    }
}

#endif

void test_wave_store_unknown_leaf_and_wrong_digest_are_preserved() {
    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "unknown-leaf";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create unknown-leaf fixture");
        const Digest digest = store.manifest_digest();
        const auto manifest_before = read_file_bytes(wave_manifest_path(root));
        const auto unknown = root / "unexpected.control";
        write_foreign_leaf(unknown);
        const auto unknown_before = read_file_bytes(unknown);

        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "unknown leaf taints live wave");
        CHECK(read_file_bytes(wave_manifest_path(root)) == manifest_before);
        CHECK(read_file_bytes(unknown) == unknown_before);
        created.store.reset();

        auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        CHECK(!opened);
        require_wave_status(opened.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "unknown leaf taints reopened wave");
        CHECK(read_file_bytes(wave_manifest_path(root)) == manifest_before);
        CHECK(read_file_bytes(unknown) == unknown_before);
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "wrong-expected-digest";
        const Digest digest = create_closed_wave(root);
        const auto manifest_before = read_file_bytes(wave_manifest_path(root));
        auto wrong_digest = digest;
        perturb_digest(wrong_digest);
        auto opened = wave_detail::DistributedSieveWaveStore::open(root, wrong_digest);
        CHECK(!opened);
        require_wave_status(opened.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::manifest_conflict,
                            "wrong expected manifest digest");
        CHECK(read_file_bytes(wave_manifest_path(root)) == manifest_before);
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "wrong-on-disk-digest";
        const Digest digest = create_closed_wave(root);
        auto corrupted = read_file_bytes(wave_manifest_path(root));
        CHECK(!corrupted.empty());
        corrupted.back() ^= std::byte{0x40};
        write_file_bytes(wave_manifest_path(root), corrupted);
        auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        CHECK(!opened);
        require_wave_status(opened.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::manifest_invalid,
                            "wrong on-disk manifest digest");
        CHECK(read_file_bytes(wave_manifest_path(root)) == corrupted);
    }
}

#if !defined(_WIN32)

void require_chmod(const std::filesystem::path& path, mode_t mode, std::string_view context) {
    if (::chmod(path.c_str(), mode) != 0) {
        throw std::system_error(errno, std::generic_category(), std::string(context));
    }
}

void require_rename(const std::filesystem::path& source, const std::filesystem::path& target,
                    std::string_view context) {
    std::error_code error;
    std::filesystem::rename(source, target, error);
    if (error) {
        throw std::filesystem::filesystem_error(std::string(context), source, target, error);
    }
}

#if defined(__APPLE__)

void install_wave_extended_read_acl(int descriptor) {
    acl_t acl = ::acl_init(1);
    if (acl == nullptr) {
        throw TestFailure("cannot allocate wave-store test ACL");
    }

    acl_entry_t entry = nullptr;
    acl_permset_t permissions = nullptr;
    uuid_t owner_uuid{};
    const int membership_error = ::mbr_uid_to_uuid(::geteuid(), owner_uuid);
    const bool configured = membership_error == 0 && ::acl_create_entry(&acl, &entry) == 0 &&
                            ::acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0 &&
                            ::acl_set_qualifier(entry, owner_uuid) == 0 &&
                            ::acl_get_permset(entry, &permissions) == 0 &&
                            ::acl_clear_perms(permissions) == 0 &&
                            ::acl_add_perm(permissions, ACL_READ_DATA) == 0 &&
                            ::acl_set_permset(entry, permissions) == 0 && ::acl_valid(acl) == 0 &&
                            ::acl_set_fd_np(descriptor, acl, ACL_TYPE_EXTENDED) == 0;
    const int saved_errno = errno;
    (void)::acl_free(acl);
    if (!configured) {
        throw TestFailure("cannot install wave-store test ACL: " +
                          std::error_code(membership_error != 0 ? membership_error : saved_errno,
                                          std::generic_category())
                              .message());
    }
}

void install_wave_extended_read_acl(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "open wave-store ACL test target");
    }
    try {
        install_wave_extended_read_acl(descriptor);
    } catch (...) {
        (void)::close(descriptor);
        throw;
    }
    if (::close(descriptor) != 0) {
        throw std::system_error(errno, std::generic_category(), "close wave-store ACL test target");
    }
}

#elif defined(__linux__)

void install_wave_extended_read_acl(const std::filesystem::path& path) {
    constexpr std::uint16_t ACL_USER_OBJ = 0x01;
    constexpr std::uint16_t ACL_USER = 0x02;
    constexpr std::uint16_t ACL_GROUP_OBJ = 0x04;
    constexpr std::uint16_t ACL_MASK = 0x10;
    constexpr std::uint16_t ACL_OTHER = 0x20;
    constexpr std::uint32_t ACL_UNDEFINED_ID = 0xffffffffU;
    constexpr std::size_t HEADER_BYTES = 4;
    constexpr std::size_t ENTRY_BYTES = 8;
    std::array<std::byte, HEADER_BYTES + 5 * ENTRY_BYTES> acl{};
    struct stat metadata{};
    if (::lstat(path.c_str(), &metadata) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "inspect wave-store Linux ACL target");
    }
    const auto owner_permissions = static_cast<std::uint16_t>((metadata.st_mode >> 6U) & 07U);
    const auto group_permissions = static_cast<std::uint16_t>((metadata.st_mode >> 3U) & 07U);
    const auto other_permissions = static_cast<std::uint16_t>(metadata.st_mode & 07U);

    const auto put_u16_le = [&](std::size_t offset, std::uint16_t value) {
        acl[offset] = static_cast<std::byte>(value & 0xffU);
        acl[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    };
    const auto put_u32_le = [&](std::size_t offset, std::uint32_t value) {
        for (std::size_t index = 0; index < 4; ++index) {
            acl[offset + index] =
                static_cast<std::byte>((value >> static_cast<unsigned>(index * 8U)) & 0xffU);
        }
    };
    const auto put_entry = [&](std::size_t index, std::uint16_t tag, std::uint16_t permissions,
                               std::uint32_t id) {
        const std::size_t offset = HEADER_BYTES + index * ENTRY_BYTES;
        put_u16_le(offset, tag);
        put_u16_le(offset + 2, permissions);
        put_u32_le(offset + 4, id);
    };

    put_u32_le(0, 2);
    put_entry(0, ACL_USER_OBJ, owner_permissions, ACL_UNDEFINED_ID);
    put_entry(1, ACL_USER, 0, static_cast<std::uint32_t>(::geteuid()) ^ 1U);
    put_entry(2, ACL_GROUP_OBJ, group_permissions, ACL_UNDEFINED_ID);
    put_entry(3, ACL_MASK, group_permissions, ACL_UNDEFINED_ID);
    put_entry(4, ACL_OTHER, other_permissions, ACL_UNDEFINED_ID);
    if (::setxattr(path.c_str(), "system.posix_acl_access", acl.data(), acl.size(), 0) != 0) {
        throw std::system_error(errno, std::generic_category(), "install wave-store Linux ACL");
    }
}

#endif

void test_wave_store_rejects_noncanonical_and_intermediate_symlink_paths() {
    WaveStoreTempDirectory temp;
    const std::string base = temp.path().string();
    const std::array malformed_roots{
        std::filesystem::path(base + "//double-separator"),
        std::filesystem::path(base + "/./dot-component"),
        std::filesystem::path(base + "/missing/../dot-dot-component"),
        std::filesystem::path(base + "/trailing-separator/"),
    };
    for (const auto& root : malformed_roots) {
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        CHECK(!created);
        CHECK(created.store == nullptr);
        require_wave_status(created.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::invalid_request,
                            "reject noncanonical absolute wave path");
    }
    CHECK(!entry_exists_no_follow(temp.path() / "double-separator"));
    CHECK(!entry_exists_no_follow(temp.path() / "dot-component"));
    CHECK(!entry_exists_no_follow(temp.path() / "dot-dot-component"));
    CHECK(!entry_exists_no_follow(temp.path() / "trailing-separator"));

    const auto unsafe_parent = temp.path() / "unsafe-parent";
    std::error_code error;
    CHECK(std::filesystem::create_directory(unsafe_parent, error));
    CHECK(!error);
    require_chmod(unsafe_parent, 0777, "weaken wave parent mode");
    auto unsafe_create = wave_detail::DistributedSieveWaveStore::create(unsafe_parent / "wave",
                                                                        wave_manifest_draft());
    CHECK(!unsafe_create);
    require_wave_status(unsafe_create.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                        "reject writable direct parent before mutation");
    CHECK(!entry_exists_no_follow(unsafe_parent / "wave"));
    require_chmod(unsafe_parent, 0700, "restore wave parent mode");

    const auto real_parent = temp.path() / "real-parent";
    error.clear();
    CHECK(std::filesystem::create_directory(real_parent, error));
    CHECK(!error);
    require_chmod(real_parent, 0700, "chmod real wave parent");
    const auto alias = temp.path() / "parent-alias";
    CHECK(::symlink(real_parent.c_str(), alias.c_str()) == 0);

    auto aliased_create =
        wave_detail::DistributedSieveWaveStore::create(alias / "new-wave", wave_manifest_draft());
    CHECK(!aliased_create);
    require_wave_status(aliased_create.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                        "reject intermediate parent symlink on create");
    CHECK(!entry_exists_no_follow(real_parent / "new-wave"));

    const auto real_root = real_parent / "existing-wave";
    const Digest digest = create_closed_wave(real_root);
    const auto manifest_before = read_file_bytes(wave_manifest_path(real_root));
    auto aliased_open =
        wave_detail::DistributedSieveWaveStore::open(alias / "existing-wave", digest);
    CHECK(!aliased_open);
    require_wave_status(aliased_open.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                        "reject intermediate parent symlink on open");
    CHECK(read_file_bytes(wave_manifest_path(real_root)) == manifest_before);

    const auto stable_ancestor = temp.path() / "stable-ancestor";
    const auto held_parent = stable_ancestor / "held-parent";
    CHECK(std::filesystem::create_directories(held_parent, error));
    CHECK(!error);
    require_chmod(stable_ancestor, 0700, "chmod stable wave ancestor");
    require_chmod(held_parent, 0700, "chmod held wave parent");
    const auto held_root = held_parent / "held-wave";
    auto held = wave_detail::DistributedSieveWaveStore::create(held_root, wave_manifest_draft());
    auto& held_store = require_wave_ready(held, "create ancestor-replacement fixture");
    const auto held_manifest_before = read_file_bytes(wave_manifest_path(held_root));
    const auto original_ancestor = temp.path() / "stable-ancestor-original";
    require_rename(stable_ancestor, original_ancestor, "move held wave ancestor");
    CHECK(::symlink(original_ancestor.c_str(), stable_ancestor.c_str()) == 0);

    require_wave_status(held_store.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                        "reject intermediate ancestor symlink after acquisition");
    CHECK(read_file_bytes(wave_manifest_path(held_root)) == held_manifest_before);
}

void test_wave_store_root_and_lock_replacement() {
    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "root-replacement";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create root-replacement fixture");
        const auto original_root = temp.path() / "root-replacement-original";
        const auto manifest_before = read_file_bytes(wave_manifest_path(root));
        require_rename(root, original_root, "move original wave root");
        std::error_code error;
        CHECK(std::filesystem::create_directory(root, error));
        CHECK(!error);
        require_chmod(root, 0700, "chmod replacement wave root");

        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "revalidate replaced wave root");
        CHECK(std::filesystem::is_empty(root));
        CHECK(read_file_bytes(wave_manifest_path(original_root)) == manifest_before);
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "lock-replacement";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create lock-replacement fixture");
        const auto original_lock = temp.path() / "original-wave-lock";
        require_rename(wave_lock_path(root), original_lock, "move original wave lock");
        write_foreign_leaf(wave_lock_path(root));
        const auto replacement_before = read_file_bytes(wave_lock_path(root));

        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                            "revalidate replaced wave lock");
        CHECK(read_file_bytes(wave_lock_path(root)) == replacement_before);
        CHECK(entry_exists_no_follow(original_lock));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "live-lock-replacement";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create live-lock-replacement fixture");
        const Digest digest = store.manifest_digest();
        const auto manifest_before = read_file_bytes(wave_manifest_path(root));
        const auto original_lock = temp.path() / "live-original-wave-lock";
        require_rename(wave_lock_path(root), original_lock, "move live original wave lock");
        write_empty_foreign_leaf(wave_lock_path(root));

        auto replacement_opener = wave_detail::DistributedSieveWaveStore::open(root, digest);
        CHECK(!replacement_opener);
        require_wave_status(replacement_opener.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::manifest_conflict,
                            "replacement lock cannot satisfy manifest identity");
        CHECK(read_file_bytes(wave_manifest_path(root)) == manifest_before);
        CHECK(read_file_bytes(wave_lock_path(root)).empty());
        CHECK(!entry_exists_no_follow(wave_manifest_pending_path(root)));
        CHECK(entry_exists_no_follow(original_lock));
        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                            "old owner rejects replaced named lock");
    }
}

void test_wave_store_mode_symlink_and_hardlink_rejections() {
    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "root-mode";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create root-mode fixture");
        require_chmod(root, 0755, "weaken wave root mode");
        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "reject weakened wave root mode");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "lock-mode";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create lock-mode fixture");
        require_chmod(wave_lock_path(root), 0644, "weaken wave lock mode");
        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                            "reject weakened wave lock mode");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "manifest-mode";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create manifest-mode fixture");
        require_chmod(wave_manifest_path(root), 0644, "weaken manifest mode");
        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::manifest_invalid,
                            "reject weakened manifest mode");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "root-symlink";
        const Digest digest = create_closed_wave(root);
        const auto original_root = temp.path() / "root-symlink-original";
        require_rename(root, original_root, "move root before symlink replacement");
        CHECK(::symlink(original_root.c_str(), root.c_str()) == 0);
        auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        CHECK(!opened);
        require_wave_status(opened.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "reject wave root symlink");
        CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(root)));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "lock-symlink";
        const Digest digest = create_closed_wave(root);
        const auto original_lock = temp.path() / "lock-symlink-original";
        require_rename(wave_lock_path(root), original_lock, "move lock before symlink replacement");
        CHECK(::symlink(original_lock.c_str(), wave_lock_path(root).c_str()) == 0);
        auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        CHECK(!opened);
        require_wave_status(opened.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                            "reject wave lock symlink");
        CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(wave_lock_path(root))));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "manifest-symlink";
        const Digest digest = create_closed_wave(root);
        const auto original_manifest = temp.path() / "manifest-symlink-original";
        require_rename(wave_manifest_path(root), original_manifest,
                       "move manifest before symlink replacement");
        CHECK(::symlink(original_manifest.c_str(), wave_manifest_path(root).c_str()) == 0);
        auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        CHECK(!opened);
        require_wave_status(opened.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::manifest_invalid,
                            "reject manifest symlink");
        CHECK(
            std::filesystem::is_symlink(std::filesystem::symlink_status(wave_manifest_path(root))));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "lock-hardlink";
        const Digest digest = create_closed_wave(root);
        const auto original_lock = temp.path() / "lock-hardlink-original";
        require_rename(wave_lock_path(root), original_lock,
                       "move lock before hardlink replacement");
        CHECK(::link(original_lock.c_str(), wave_lock_path(root).c_str()) == 0);
        auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        CHECK(!opened);
        require_wave_status(opened.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                            "reject wave lock hardlink");
        CHECK(entry_exists_no_follow(original_lock));
        CHECK(entry_exists_no_follow(wave_lock_path(root)));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "manifest-hardlink";
        const Digest digest = create_closed_wave(root);
        const auto original_manifest = temp.path() / "manifest-hardlink-original";
        require_rename(wave_manifest_path(root), original_manifest,
                       "move manifest before hardlink replacement");
        CHECK(::link(original_manifest.c_str(), wave_manifest_path(root).c_str()) == 0);
        auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        CHECK(!opened);
        require_wave_status(opened.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::manifest_invalid,
                            "reject manifest hardlink");
        CHECK(entry_exists_no_follow(original_manifest));
        CHECK(entry_exists_no_follow(wave_manifest_path(root)));
    }

#if defined(__APPLE__) || defined(__linux__)
    {
        WaveStoreTempDirectory temp;
        install_wave_extended_read_acl(temp.path());
        const auto root = temp.path() / "parent-acl";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        CHECK(!created);
        require_wave_status(created.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "reject extended parent ACL before mutation");
        CHECK(!entry_exists_no_follow(root));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "root-acl";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create root-ACL fixture");
        install_wave_extended_read_acl(root);
        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "reject extended root ACL");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "lock-acl";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create lock-ACL fixture");
        install_wave_extended_read_acl(wave_lock_path(root));
        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                            "reject extended lock ACL");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "manifest-acl";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create manifest-ACL fixture");
        install_wave_extended_read_acl(wave_manifest_path(root));
        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::manifest_invalid,
                            "reject extended manifest ACL");
    }
#endif
}

[[nodiscard]] bool write_pipe_byte(int descriptor, char value) noexcept {
    ssize_t count = -1;
    do {
        count = ::write(descriptor, &value, 1);
    } while (count < 0 && errno == EINTR);
    return count == 1;
}

[[nodiscard]] bool read_pipe_byte(int descriptor, char& value) noexcept {
    ssize_t count = -1;
    do {
        count = ::read(descriptor, &value, 1);
    } while (count < 0 && errno == EINTR);
    return count == 1;
}

[[nodiscard]] bool wait_for_child(pid_t child, int& status) noexcept {
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == child;
}

void test_wave_store_fault_hooks_cannot_fork_store_authority() {
    for (const auto point : WAVE_STORE_FAULT_POINTS) {
        WaveStoreTempDirectory temp;
        const auto root =
            temp.path() / ("fork-fault-" + std::to_string(static_cast<std::size_t>(point)));
        WaveForkFaultContext context{
            .target = point,
            .original_process = ::getpid(),
        };
        auto result = wave_detail::DistributedSieveWaveStore::create(
            root, wave_manifest_draft(),
            wave_detail::DistributedSieveWaveStoreTestHooks{
                .stop_after = fork_at_wave_fault,
                .context = &context,
            });

        if (::getpid() != context.original_process) {
            const bool rejected = !result && result.store == nullptr &&
                                  result.diagnostic.status ==
                                      wave_detail::DistributedSieveWaveStoreStatus::invalid_request;
            ::_exit(rejected ? 0 : 91);
        }

        CHECK(context.invoked);
        CHECK(context.child_process > 0);
        CHECK(!result);
        CHECK(result.store == nullptr);
        require_wave_status(result.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::interrupted,
                            "parent interrupts after forking inside fault hook");

        int child_status = 0;
        CHECK(wait_for_child(context.child_process, child_status));
        CHECK(WIFEXITED(child_status));
        CHECK(WEXITSTATUS(child_status) == 0);
        check_wave_fault_prefix(root, point);
    }
}

void test_wave_store_deterministic_busy_with_fork_and_pipes() {
    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "busy";
    const Digest digest = create_closed_wave(root);
    int ready_pipe[2]{-1, -1};
    int release_pipe[2]{-1, -1};
    CHECK(::pipe(ready_pipe) == 0);
    CHECK(::pipe(release_pipe) == 0);
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)::close(ready_pipe[0]);
        (void)::close(release_pipe[1]);
        auto held = wave_detail::DistributedSieveWaveStore::open(root, digest);
        const char ready = held ? 'r' : 'f';
        const bool signalled = write_pipe_byte(ready_pipe[1], ready);
        char release = '\0';
        const bool released = read_pipe_byte(release_pipe[0], release);
        held.store.reset();
        ::_exit(signalled && released && release == 'x' && ready == 'r' ? 0 : 81);
    }

    (void)::close(ready_pipe[1]);
    (void)::close(release_pipe[0]);
    char ready = '\0';
    const bool received = read_pipe_byte(ready_pipe[0], ready);
    auto busy = wave_detail::DistributedSieveWaveStore::open(root, digest);
    const bool released = write_pipe_byte(release_pipe[1], 'x');
    (void)::close(ready_pipe[0]);
    (void)::close(release_pipe[1]);
    int child_status = 0;
    const bool waited = wait_for_child(child, child_status);

    CHECK(received);
    CHECK(ready == 'r');
    CHECK(!busy);
    require_wave_status(busy.diagnostic, wave_detail::DistributedSieveWaveStoreStatus::lock_busy,
                        "concurrent wave opener is busy");
    CHECK(released);
    CHECK(waited);
    CHECK(WIFEXITED(child_status));
    CHECK(WEXITSTATUS(child_status) == 0);

    auto reopened = wave_detail::DistributedSieveWaveStore::open(root, digest);
    (void)require_wave_ready(reopened, "wave opens after lock owner exits");
}

void test_wave_store_inherited_lock_is_process_bound_and_close_only() {
    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "child-close-only";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create child-close-only fixture");
        const Digest digest = store.manifest_digest();
        int ready_pipe[2]{-1, -1};
        int release_pipe[2]{-1, -1};
        CHECK(::pipe(ready_pipe) == 0);
        CHECK(::pipe(release_pipe) == 0);
        const pid_t child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            (void)::close(ready_pipe[0]);
            (void)::close(release_pipe[1]);
            const auto inherited = store.revalidate();
            const bool process_bound =
                inherited.status == wave_detail::DistributedSieveWaveStoreStatus::invalid_request;
            created.store.reset();
            const bool signalled = write_pipe_byte(ready_pipe[1], process_bound ? 'r' : 'f');
            char release = '\0';
            const bool released = read_pipe_byte(release_pipe[0], release);
            ::_exit(process_bound && signalled && released && release == 'x' ? 0 : 82);
        }

        (void)::close(ready_pipe[1]);
        (void)::close(release_pipe[0]);
        char ready = '\0';
        const bool received = read_pipe_byte(ready_pipe[0], ready);
        auto busy = wave_detail::DistributedSieveWaveStore::open(root, digest);
        const bool released = write_pipe_byte(release_pipe[1], 'x');
        (void)::close(ready_pipe[0]);
        (void)::close(release_pipe[1]);
        int child_status = 0;
        const bool waited = wait_for_child(child, child_status);

        CHECK(received);
        CHECK(ready == 'r');
        CHECK(!busy);
        require_wave_status(busy.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::lock_busy,
                            "fork copy close cannot unlock parent wave");
        CHECK(released);
        CHECK(waited);
        CHECK(WIFEXITED(child_status));
        CHECK(WEXITSTATUS(child_status) == 0);
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "parent wave remains valid after child close");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "child-retains-inherited-lock";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create inherited-lock fixture");
        const Digest digest = store.manifest_digest();
        int ready_pipe[2]{-1, -1};
        int release_pipe[2]{-1, -1};
        CHECK(::pipe(ready_pipe) == 0);
        CHECK(::pipe(release_pipe) == 0);
        const pid_t child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            (void)::close(ready_pipe[0]);
            (void)::close(release_pipe[1]);
            const auto inherited = store.revalidate();
            const bool process_bound =
                inherited.status == wave_detail::DistributedSieveWaveStoreStatus::invalid_request;
            const bool signalled = write_pipe_byte(ready_pipe[1], process_bound ? 'r' : 'f');
            char release = '\0';
            const bool released = read_pipe_byte(release_pipe[0], release);
            created.store.reset();
            ::_exit(process_bound && signalled && released && release == 'x' ? 0 : 83);
        }

        (void)::close(ready_pipe[1]);
        (void)::close(release_pipe[0]);
        char ready = '\0';
        const bool received = read_pipe_byte(ready_pipe[0], ready);
        created.store.reset();
        auto busy = wave_detail::DistributedSieveWaveStore::open(root, digest);
        const bool released = write_pipe_byte(release_pipe[1], 'x');
        (void)::close(ready_pipe[0]);
        (void)::close(release_pipe[1]);
        int child_status = 0;
        const bool waited = wait_for_child(child, child_status);

        CHECK(received);
        CHECK(ready == 'r');
        CHECK(!busy);
        require_wave_status(busy.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::lock_busy,
                            "inherited child descriptor retains wave lock");
        CHECK(released);
        CHECK(waited);
        CHECK(WIFEXITED(child_status));
        CHECK(WEXITSTATUS(child_status) == 0);

        auto reopened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        (void)require_wave_ready(reopened, "wave opens after inherited descriptor closes");
    }
}

void test_wave_store_private_lease_root_claim_traits_and_lifetime() {
    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "private-lease-root-claim";
    auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    auto& store = require_wave_ready(created, "create private-lease-root claim fixture");
    const Digest digest = store.manifest_digest();

    std::array<std::unique_ptr<PrivateLeaseRootClaim>, 2> claims;
    std::array<wave_detail::DistributedSieveWaveStoreDiagnostic, 2> diagnostics;
    std::barrier<> claim_phases(3);
    const auto claim_once = [&](std::size_t index) {
        claim_phases.arrive_and_wait();
        auto result = store.claim_private_lease_root();
        diagnostics[index] = result.diagnostic;
        claims[index] = std::move(result.claim);
        claim_phases.arrive_and_wait();
    };

    std::thread first(claim_once, 0);
    std::thread second(claim_once, 1);
    claim_phases.arrive_and_wait();
    claim_phases.arrive_and_wait();
    first.join();
    second.join();

    const std::size_t success_count = static_cast<std::size_t>(claims[0] != nullptr) +
                                      static_cast<std::size_t>(claims[1] != nullptr);
    CHECK(success_count == 1);
    const std::size_t winner = claims[0] != nullptr ? 0 : 1;
    const std::size_t loser = 1 - winner;
    require_wave_status(diagnostics[winner], wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "same-State private-lease-root claim winner");
    require_wave_status(diagnostics[loser],
                        wave_detail::DistributedSieveWaveStoreStatus::private_lease_root_busy,
                        "same-State private-lease-root claim loser");
    CHECK(claims[winner]->owned_by_current_process());
    require_wave_status(claims[winner]->revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "winning private-lease-root claim revalidates");

    claims[winner].reset();
    auto reacquired = store.claim_private_lease_root();
    auto& retained_claim = require_private_lease_root_claim_ready(
        reacquired, "reacquire released private-lease-root claim");
    require_wave_status(retained_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "reacquired private-lease-root claim revalidates");

    const auto independent_root = temp.path() / "independent-private-lease-root-claim";
    auto independent =
        wave_detail::DistributedSieveWaveStore::create(independent_root, wave_manifest_draft());
    auto& independent_store =
        require_wave_ready(independent, "create independent private-lease-root claim fixture");
    auto independent_claimed = independent_store.claim_private_lease_root();
    auto& independent_claim = require_private_lease_root_claim_ready(
        independent_claimed, "different wave root claims independently");
    require_wave_status(independent_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "different wave root claim revalidates while first is held");
    require_wave_status(retained_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "first wave root claim remains valid while second is held");

    created.store.reset();
    require_wave_status(retained_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "private-lease-root claim outlives store");
    auto busy = wave_detail::DistributedSieveWaveStore::open(root, digest);
    CHECK(!busy);
    CHECK(busy.store == nullptr);
    require_wave_status(busy.diagnostic, wave_detail::DistributedSieveWaveStoreStatus::lock_busy,
                        "retained private-lease-root claim keeps wave lock");

    reacquired.claim.reset();
    auto reopened = wave_detail::DistributedSieveWaveStore::open(root, digest);
    (void)require_wave_ready(reopened, "wave opens after private-lease-root claim release");
}

void test_wave_store_manifest_bound_base_locks_and_claim_inventory_split() {
    WaveStoreTempDirectory temp;
    const auto remove_entry = [](const std::filesystem::path& path) {
        std::error_code error;
        const bool removed = std::filesystem::remove(path, error);
        CHECK(removed);
        CHECK(!error);
    };

    const auto root = temp.path() / "manifest-bound-base-lock";
    auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    auto& initial_store = require_wave_ready(created, "create manifest-bound BaseLock fixture");
    const auto& first_chunk = initial_store.manifest().chunks.front();
    const auto first_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        first_chunk.relative_artifact_stem, first_chunk.chunk_id, 0);
    const auto second_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        first_chunk.relative_artifact_stem, first_chunk.chunk_id, 1);
    CHECK(first_names.has_value());
    CHECK(second_names.has_value());
    const auto valid_base_lock = root / first_names->base_lock_leaf;
    const auto metadata_base_lock = root / second_names->base_lock_leaf;
    const std::string base_lock_suffix(
        wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX);
    const auto relation_paths = gnfs::relation::OOCCleanupTransaction::paths_for(
        root / first_names->private_directory_leaf / "corpus");
    CHECK(relation_paths.private_directory == root / first_names->private_directory_leaf);
    CHECK(relation_paths.lock_path == valid_base_lock);
    auto production_base_lock =
        std::make_unique<cleanup_detail::BaseLock>(relation_paths.lock_path);
    CHECK(production_base_lock->matches(valid_base_lock));
    production_base_lock->require_stable();
    require_wave_status(initial_store.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "relation-derived BaseLock is accepted by a live store");
    const Digest digest = initial_store.manifest_digest();
    created.store.reset();

    const auto precedence_conflict = root / ("foreign_attempt_00" + base_lock_suffix);
    write_empty_foreign_leaf(precedence_conflict);
    auto mismatched_draft = wave_manifest_draft();
    ++mismatched_draft.relation_cap_per_worker;
    auto mismatched = wave_detail::DistributedSieveWaveStore::create(root, mismatched_draft);
    CHECK(!mismatched);
    CHECK(mismatched.store == nullptr);
    require_wave_status(mismatched.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::manifest_conflict,
                        "manifest mismatch wins over invalid BaseLock inventory");
    CHECK(entry_exists_no_follow(valid_base_lock));
    CHECK(entry_exists_no_follow(precedence_conflict));
    CHECK(!entry_exists_no_follow(wave_manifest_pending_path(root)));
    remove_entry(precedence_conflict);

    auto idempotent = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    auto& idempotent_store =
        require_wave_ready(idempotent, "idempotent create accepts manifest-bound BaseLock");
    require_wave_status(idempotent_store.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "idempotently created store revalidates manifest-bound BaseLock");
    idempotent.store.reset();

    auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
    auto& store = require_wave_ready(opened, "open wave with manifest-bound BaseLock");
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "opened store accepts manifest-bound BaseLock");

    const auto displaced_base_lock = temp.path() / "displaced-production-base-lock";
    WaveBaseLockReplacementContext replacement_context{
        .canonical = valid_base_lock,
        .displaced = displaced_base_lock,
    };
    const auto replaced_during_revalidation =
        store.revalidate(wave_detail::DistributedSieveWaveStoreInventoryTestHooks{
            .after_first_validation = replace_base_lock_after_first_inventory,
            .context = &replacement_context,
        });
    CHECK(replacement_context.invoked);
    CHECK(replacement_context.replaced);
    CHECK(replacement_context.native_error == 0);
    require_wave_status(replaced_during_revalidation,
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "full inventory rejects same-name BaseLock identity replacement");
    CHECK(!production_base_lock->stable_noexcept());
    CHECK(entry_exists_no_follow(valid_base_lock));
    CHECK(entry_exists_no_follow(displaced_base_lock));
    remove_entry(valid_base_lock);
    require_rename(displaced_base_lock, valid_base_lock, "restore production BaseLock identity");
    production_base_lock->require_stable();
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "restored BaseLock identity closes full inventory");

    const std::array invalid_manifest_members{
        root / ("foreign_attempt_00" + base_lock_suffix),
        root / ("chunk_0_attempt_02" + base_lock_suffix),
        root / ("chunk_0_attempt_0" + base_lock_suffix),
    };
    for (const auto& invalid_leaf : invalid_manifest_members) {
        write_empty_foreign_leaf(invalid_leaf);
        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "BaseLock lookalike is not admitted by suffix alone");
        remove_entry(invalid_leaf);
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "removing BaseLock lookalike restores closed inventory");
    }

    write_empty_foreign_leaf(metadata_base_lock);
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "second valid attempt BaseLock establishes metadata baseline");
    remove_entry(metadata_base_lock);

    write_foreign_leaf(metadata_base_lock);
    require_wave_status(store.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "nonempty manifest-bound BaseLock is rejected");
    remove_entry(metadata_base_lock);

    write_empty_foreign_leaf(metadata_base_lock);
    require_chmod(metadata_base_lock, 0644, "weaken manifest-bound BaseLock mode");
    require_wave_status(store.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "weak manifest-bound BaseLock mode is rejected");
    remove_entry(metadata_base_lock);

#if defined(__APPLE__) || defined(__linux__)
    write_empty_foreign_leaf(metadata_base_lock);
    install_wave_extended_read_acl(metadata_base_lock);
    require_wave_status(store.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "extended BaseLock ACL is rejected");
    remove_entry(metadata_base_lock);
#endif

    const auto link_target = temp.path() / "base-lock-link-target";
    write_empty_foreign_leaf(link_target);
    CHECK(::symlink(link_target.c_str(), metadata_base_lock.c_str()) == 0);
    require_wave_status(store.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "manifest-bound BaseLock symlink is rejected");
    remove_entry(metadata_base_lock);

    CHECK(::link(link_target.c_str(), metadata_base_lock.c_str()) == 0);
    require_wave_status(store.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "manifest-bound BaseLock hardlink is rejected");
    remove_entry(metadata_base_lock);
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "valid BaseLock remains after invalid metadata cases");

    auto claimed = store.claim_private_lease_root();
    auto& claim = require_private_lease_root_claim_ready(
        claimed, "claim private-lease root with manifest-bound BaseLock");
    const auto foreign = root / "unexpected.control";
    write_foreign_leaf(foreign);
    require_wave_status(claim.revalidate_authority(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "authority-only claim check ignores ordinary root inventory");
    require_wave_status(claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "full claim check rejects foreign root inventory");
    remove_entry(foreign);
    require_wave_status(claim.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "full claim check recovers after foreign inventory removal");

    write_foreign_leaf(wave_manifest_pending_path(root));
    require_wave_status(claim.revalidate_authority(),
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "authority-only claim check still rejects manifest pending");
    remove_entry(wave_manifest_pending_path(root));
    require_wave_status(claim.revalidate_authority(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "authority-only claim check recovers after pending removal");

    const auto recovery_root = temp.path() / "base-lock-pending-recovery";
    WaveFaultStopContext recovery_context{
        .target = wave_detail::DistributedSieveWaveStoreFaultPoint::ManifestPendingDurable,
    };
    auto interrupted = wave_detail::DistributedSieveWaveStore::create(
        recovery_root, wave_manifest_draft(),
        wave_detail::DistributedSieveWaveStoreTestHooks{
            .stop_after = stop_at_wave_fault,
            .context = &recovery_context,
        });
    CHECK(!interrupted);
    require_wave_status(interrupted.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::interrupted,
                        "leave manifest-pending prefix for BaseLock recovery");
    const auto recovery_names =
        wave_detail::distributed_sieve_worker_attempt_names_v1("chunk_0", 0, 0);
    CHECK(recovery_names.has_value());
    const auto recovery_base_lock = recovery_root / recovery_names->base_lock_leaf;
    write_empty_foreign_leaf(recovery_base_lock);
    const Digest recovery_digest =
        manifest_digest_from_file(wave_manifest_pending_path(recovery_root));
    auto recovered = wave_detail::DistributedSieveWaveStore::open(recovery_root, recovery_digest);
    auto& recovered_store =
        require_wave_ready(recovered, "recover manifest pending with bound BaseLock");
    CHECK(entry_exists_no_follow(recovery_base_lock));
    CHECK(!entry_exists_no_follow(wave_manifest_pending_path(recovery_root)));
    require_wave_status(recovered_store.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "recovered BaseLock inventory is fully closed");

    const auto entrypoint_root = temp.path() / "invalid-base-lock-entrypoints";
    const Digest entrypoint_digest = create_closed_wave(entrypoint_root);
    const auto invalid_entrypoint_leaf =
        entrypoint_root / ("foreign_attempt_00" + base_lock_suffix);
    write_empty_foreign_leaf(invalid_entrypoint_leaf);
    auto create_rejected =
        wave_detail::DistributedSieveWaveStore::create(entrypoint_root, wave_manifest_draft());
    CHECK(!create_rejected);
    require_wave_status(create_rejected.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "idempotent create rejects invalid BaseLock member");
    auto open_rejected =
        wave_detail::DistributedSieveWaveStore::open(entrypoint_root, entrypoint_digest);
    CHECK(!open_rejected);
    require_wave_status(open_rejected.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "open rejects invalid BaseLock member");
    CHECK(entry_exists_no_follow(invalid_entrypoint_leaf));
    CHECK(!entry_exists_no_follow(wave_manifest_pending_path(entrypoint_root)));

    const auto preclaim_root = temp.path() / "preclaim-foreign-inventory";
    auto preclaim_created =
        wave_detail::DistributedSieveWaveStore::create(preclaim_root, wave_manifest_draft());
    auto& preclaim_store =
        require_wave_ready(preclaim_created, "create preclaim foreign-inventory fixture");
    const auto preclaim_foreign = preclaim_root / "unexpected.control";
    write_foreign_leaf(preclaim_foreign);
    auto rejected = preclaim_store.claim_private_lease_root();
    CHECK(!rejected);
    CHECK(rejected.claim == nullptr);
    require_wave_status(rejected.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "initial claim mint requires closed full inventory");
    remove_entry(preclaim_foreign);
    auto retried = preclaim_store.claim_private_lease_root();
    auto& retry_claim = require_private_lease_root_claim_ready(
        retried, "failed initial inventory check releases the claim slot");
    require_wave_status(retry_claim.revalidate_authority(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "retried claim retains authority after inventory repair");
}

void test_wave_store_fresh_private_lease_reservation_protocol() {
    for (std::size_t index = 0;
         index < wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVATION_BOUNDARIES.size();
         ++index) {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / ("fresh-reservation-prefix-" + std::to_string(index));
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create fresh reservation prefix fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());

        auto claimed = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        (void)require_private_lease_root_claim_ready(claimed, "create fresh reservation BaseLock");
        WavePrivateLeaseProtocolStopContext context{
            .target = wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVATION_BOUNDARIES[index],
        };
        auto interrupted = wave_detail::reserve_worker_attempt_private_lease(
            std::move(claimed), wave_detail::DistributedSievePrivateLeaseProtocolTestHooks{
                                    .stop_after = stop_at_wave_private_lease_boundary,
                                    .context = &context,
                                });
        CHECK(!interrupted);
        CHECK(!interrupted.receipt.has_value());
        require_wave_status(interrupted.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::interrupted,
                            "fresh reservation stops at an exact durable prefix");
        CHECK(interrupted.diagnostic.last_private_lease_reservation_boundary == context.target);
        CHECK(claimed.claim == nullptr);
        for (std::size_t observed = 0; observed < context.observed.size(); ++observed) {
            CHECK(context.observed[observed] == (observed <= index));
        }
        CHECK(!entry_exists_no_follow(root / names->canonical_record_leaf));
        CHECK(!entry_exists_no_follow(root / names->pending_record_leaf));

        WaveReservationWitnessObservationContext observation{
            .expected_boundary = context.target,
            .expected_base_lock_leaf = names->base_lock_leaf,
        };
        require_wave_status(
            store.revalidate(wave_detail::DistributedSieveWaveStoreInventoryTestHooks{
                .observe_reservation_witnesses = observe_wave_reservation_witnesses,
                .context = &observation,
            }),
            wave_detail::DistributedSieveWaveStoreStatus::ready,
            "fresh reservation prefix is manifest-bound and closed");
        CHECK(observation.invoked);
        CHECK(observation.matched);

        const auto prefix_snapshot = capture_wave_root_snapshot(root);
        std::optional<std::pair<std::filesystem::path, WaveRootEntrySnapshot>>
            nested_owner_snapshot;
        if (index >= 4U) {
            const auto reserved_path =
                index == 1U ? root / names->reserved_pending_leaf : root / names->reserved_leaf;
            const auto reserved =
                cleanup_detail::parse_private_lease_marker(read_file_bytes(reserved_path));
            const auto directory =
                index == 8U
                    ? root / names->private_directory_leaf
                    : root /
                          (names->relative_lease_stem +
                           std::string(wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG) +
                           cleanup_detail::private_lease_id_hex(reserved.lease_id));
            const auto owner_leaf =
                index == 4U
                    ? std::string(wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_PENDING_LEAF)
                    : std::string(wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF);
            const auto owner = directory / owner_leaf;
            nested_owner_snapshot.emplace(owner,
                                          capture_wave_root_entry_snapshot(owner, owner_leaf));
        }
        const auto require_prefix_unchanged = [&] {
            CHECK(capture_wave_root_snapshot(root) == prefix_snapshot);
            if (nested_owner_snapshot.has_value()) {
                CHECK(capture_wave_root_entry_snapshot(nested_owner_snapshot->first,
                                                       nested_owner_snapshot->second.leaf) ==
                      nested_owner_snapshot->second);
            }
        };

        auto generic = store.claim_private_lease_root();
        auto& generic_claim = require_private_lease_root_claim_ready(
            generic, "interrupted reservation releases same-State root slot");
        require_wave_status(generic_claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "generic root claim sees exact interrupted prefix");
        generic.claim.reset();

        auto reopened = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        (void)require_private_lease_root_claim_ready(
            reopened, "interrupted reservation releases target flock");
        if (index == 0U) {
            reopened.claim.reset();
            auto create_only_rejected =
                store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
            CHECK(!create_only_rejected);
            CHECK(create_only_rejected.claim == nullptr);
            require_wave_status(create_only_rejected.diagnostic,
                                wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                                "create-only retry never falls back to open-existing");
        } else {
            auto fresh_rejected =
                wave_detail::reserve_worker_attempt_private_lease(std::move(reopened));
            CHECK(!fresh_rejected);
            CHECK(!fresh_rejected.receipt.has_value());
            require_wave_status(fresh_rejected.diagnostic,
                                wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                                "fresh reservation never continues an existing prefix");
            if (index == 1U) {
                auto root_again = store.claim_private_lease_root();
                (void)require_private_lease_root_claim_ready(
                    root_again, "existing-prefix rejection releases same-State root slot");
                root_again.claim.reset();
                auto target_again = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
                (void)require_private_lease_root_claim_ready(
                    target_again, "existing-prefix rejection releases target flock");
                target_again.claim.reset();
            }
        }
        require_prefix_unchanged();
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "fresh-reservation-clean-open";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create clean-open fresh reservation fixture");
        const auto& chunk = store.manifest().chunks.front();

        auto created_claim = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        WavePrivateLeaseProtocolStopContext stop_at_p0{
            .target = wave_detail::DistributedSievePrivateLeaseReservationBoundary::PermitAcquired,
        };
        auto base_lock_only = wave_detail::reserve_worker_attempt_private_lease(
            std::move(created_claim), wave_detail::DistributedSievePrivateLeaseProtocolTestHooks{
                                          .stop_after = stop_at_wave_private_lease_boundary,
                                          .context = &stop_at_p0,
                                      });
        CHECK(!base_lock_only);
        require_wave_status(base_lock_only.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::interrupted,
                            "leave an explicit BaseLock-only P0");

        auto opened_claim = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        auto completed = wave_detail::reserve_worker_attempt_private_lease(std::move(opened_claim));
        CHECK(completed);
        CHECK(completed.receipt.has_value());
        require_wave_status(completed.receipt->revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "fresh reservation may start from an explicitly opened clean P0");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "fresh-reservation-predecessor-drift";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create reservation predecessor-drift fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        const auto foreign = root / "reservation-predecessor.foreign";
        PrivateLeaseProtocolForeignContext context{
            .foreign =
                AttemptPhaseForeignContext{
                    .foreign = foreign,
                },
        };

        auto claimed = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        auto rejected = wave_detail::reserve_worker_attempt_private_lease(
            std::move(claimed),
            wave_detail::DistributedSievePrivateLeaseProtocolTestHooks{
                .after_predecessor_validation = insert_foreign_after_private_lease_predecessor,
                .context = &context,
            });
        CHECK(context.foreign.invoked);
        CHECK(context.foreign.inserted);
        CHECK(context.foreign.native_error == 0);
        CHECK(!rejected);
        CHECK(!rejected.receipt.has_value());
        require_wave_status(rejected.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "predecessor drift blocks the first reservation mutation");
        CHECK(entry_exists_no_follow(foreign));
        CHECK(read_file_bytes(foreign).empty());
        CHECK(!entry_exists_no_follow(root / names->reserved_pending_leaf));
        CHECK(!entry_exists_no_follow(root / names->reserved_leaf));

        std::error_code remove_error;
        CHECK(std::filesystem::remove(foreign, remove_error));
        CHECK(!remove_error);
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "removing predecessor drift restores the exact P0");
    }

    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "fresh-reservation-complete";
    auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    auto& store = require_wave_ready(created, "create complete fresh reservation fixture");
    const auto& chunk = store.manifest().chunks.front();
    const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        chunk.relative_artifact_stem, chunk.chunk_id, 0);
    CHECK(names.has_value());
    const Digest manifest_digest = store.manifest_digest();

    auto claimed = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
    auto reserved = wave_detail::reserve_worker_attempt_private_lease(std::move(claimed));
    CHECK(reserved);
    CHECK(reserved.receipt.has_value());
    CHECK(claimed.claim == nullptr);
    CHECK(reserved.receipt->owned_by_current_process());
    CHECK(reserved.receipt->relative_lease_stem() == names->relative_lease_stem);
    CHECK((reserved.receipt->lease_id() != std::array<std::uint64_t, 2>{}));
    CHECK(reserved.receipt->directory_identity() != sieve::NativeIdentityV1{});
    CHECK(reserved.receipt->owner_marker_identity() != sieve::NativeIdentityV1{});
    CHECK(reserved.receipt->owned_marker_identity() != sieve::NativeIdentityV1{});
    require_wave_status(reserved.receipt->revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "completed reservation receipt observes its exact P8 snapshot");
    CHECK(!entry_exists_no_follow(root / names->canonical_record_leaf));
    CHECK(!entry_exists_no_follow(root / names->pending_record_leaf));

    WaveReservationWitnessObservationContext final_observation{
        .expected_boundary =
            wave_detail::DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable,
        .expected_base_lock_leaf = names->base_lock_leaf,
    };
    require_wave_status(store.revalidate(wave_detail::DistributedSieveWaveStoreInventoryTestHooks{
                            .observe_reservation_witnesses = observe_wave_reservation_witnesses,
                            .context = &final_observation,
                        }),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "completed fresh reservation is a closed P8 prefix");
    CHECK(final_observation.invoked);
    CHECK(final_observation.matched);

    auto generic = store.claim_private_lease_root();
    (void)require_private_lease_root_claim_ready(
        generic, "snapshot receipt retains no same-State root claim");
    generic.claim.reset();
    auto target = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
    (void)require_private_lease_root_claim_ready(target,
                                                 "snapshot receipt retains no target flock");
    target.claim.reset();

    auto second_claim = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 1);
    auto second_reserved =
        wave_detail::reserve_worker_attempt_private_lease(std::move(second_claim));
    CHECK(second_reserved);
    CHECK(second_reserved.receipt.has_value());
    require_wave_status(reserved.receipt->revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "first receipt permits and ignores an unrelated valid attempt");
    require_wave_status(second_reserved.receipt->revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "second receipt independently observes its exact P8 snapshot");

    int ready_pipe[2]{-1, -1};
    CHECK(::pipe(ready_pipe) == 0);
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)::close(ready_pipe[0]);
        const bool rejected = !reserved.receipt->owned_by_current_process() &&
                              reserved.receipt->revalidate().status ==
                                  wave_detail::DistributedSieveWaveStoreStatus::invalid_request;
        second_reserved.receipt.reset();
        reserved.receipt.reset();
        created.store.reset();
        const bool signalled = write_pipe_byte(ready_pipe[1], rejected ? 'r' : 'f');
        (void)::close(ready_pipe[1]);
        ::_exit(rejected && signalled ? 0 : 85);
    }
    (void)::close(ready_pipe[1]);
    char child_result = '\0';
    const bool child_reported = read_pipe_byte(ready_pipe[0], child_result);
    (void)::close(ready_pipe[0]);
    int child_status = 0;
    const bool child_waited = wait_for_child(child, child_status);
    CHECK(child_reported);
    CHECK(child_result == 'r');
    CHECK(child_waited);
    CHECK(WIFEXITED(child_status));
    CHECK(WEXITSTATUS(child_status) == 0);
    require_wave_status(reserved.receipt->revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "child receipt reset cannot release parent WaveStore authority");

    const auto completed_snapshot = capture_wave_root_snapshot(root);
    second_reserved.receipt.reset();
    created.store.reset();
    require_wave_status(reserved.receipt->revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "snapshot receipt retains WaveStore authority after facade destruction");
    reserved.receipt.reset();
    CHECK(capture_wave_root_snapshot(root) == completed_snapshot);

    auto reopened = wave_detail::DistributedSieveWaveStore::open(root, manifest_digest);
    auto& reopened_store = require_wave_ready(reopened, "reopen completed fresh reservation");
    require_wave_status(reopened_store.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "receipt destruction does not mutate the P8 namespace");
}

void test_wave_store_fresh_private_lease_reservation_sync_failures() {
    for (std::size_t index = 0; index < PRIVATE_LEASE_RESERVATION_SYNC_FAILURE_SITES.size();
         ++index) {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / ("fresh-reservation-sync-" + std::to_string(index));
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create reservation sync-failure fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());

        PrivateLeaseProtocolSyncFailureContext context{
            .target = PRIVATE_LEASE_RESERVATION_SYNC_FAILURE_SITES[index],
        };
        auto claimed = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        auto failed = wave_detail::reserve_worker_attempt_private_lease(
            std::move(claimed), wave_detail::DistributedSievePrivateLeaseProtocolTestHooks{
                                    .fail_before_sync = fail_private_lease_reservation_sync,
                                    .context = &context,
                                });
        CHECK(!failed);
        CHECK(!failed.receipt.has_value());
        CHECK(claimed.claim == nullptr);
        require_wave_status(failed.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::durability_failed,
                            "deterministic reservation sync failure");
        const wave_detail::DistributedSievePrivateLeaseReservationSyncFailureSite expected_site{
            .boundary = context.target.boundary,
            .point = context.target.point,
        };
        CHECK(failed.diagnostic.failed_private_lease_reservation_sync_site == expected_site);
        CHECK(context.selected);
        CHECK(context.observed_count == index + 1U);
        for (std::size_t observed = 0; observed < context.observed_count; ++observed) {
            CHECK(context.observed[observed].boundary ==
                  PRIVATE_LEASE_RESERVATION_SYNC_FAILURE_SITES[observed].boundary);
            CHECK(context.observed[observed].point ==
                  PRIVATE_LEASE_RESERVATION_SYNC_FAILURE_SITES[observed].point);
        }
        CHECK(!entry_exists_no_follow(root / names->canonical_record_leaf));
        CHECK(!entry_exists_no_follow(root / names->pending_record_leaf));

        WaveReservationWitnessObservationContext observation{
            .expected_boundary = context.target.boundary,
            .expected_base_lock_leaf = names->base_lock_leaf,
        };
        require_wave_status(
            store.revalidate(wave_detail::DistributedSieveWaveStoreInventoryTestHooks{
                .observe_reservation_witnesses = observe_wave_reservation_witnesses,
                .context = &observation,
            }),
            wave_detail::DistributedSieveWaveStoreStatus::ready,
            "sync failure leaves the exact visible successor prefix");
        CHECK(observation.invoked);
        CHECK(observation.matched);

        auto generic = store.claim_private_lease_root();
        (void)require_private_lease_root_claim_ready(generic,
                                                     "sync failure releases same-State root slot");
        generic.claim.reset();
        auto target = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        (void)require_private_lease_root_claim_ready(target, "sync failure releases target flock");
        target.claim.reset();

        const auto preserved = capture_wave_root_snapshot(root);
        auto create_only_retry = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        CHECK(!create_only_retry);
        CHECK(create_only_retry.claim == nullptr);
        require_wave_status(create_only_retry.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "sync failure never falls back to fresh creation");
        CHECK(capture_wave_root_snapshot(root) == preserved);
    }

    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "fresh-reservation-sync-root-precedence";
    auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    auto& store = require_wave_ready(created, "create sync/root precedence fixture");
    const auto& chunk = store.manifest().chunks.front();
    const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        chunk.relative_artifact_stem, chunk.chunk_id, 0);
    CHECK(names.has_value());
    PrivateLeaseProtocolSyncRootReplacementContext context{
        .sync =
            PrivateLeaseProtocolSyncFailureContext{
                .target = PRIVATE_LEASE_RESERVATION_SYNC_FAILURE_SITES.front(),
            },
        .root =
            WaveRootReplacementContext{
                .canonical = root,
                .displaced = temp.path() / "original-wave-root-before-sync",
            },
    };
    auto claimed = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
    auto rejected = wave_detail::reserve_worker_attempt_private_lease(
        std::move(claimed),
        wave_detail::DistributedSievePrivateLeaseProtocolTestHooks{
            .fail_before_sync = select_private_lease_sync_for_root_precedence,
            .after_injected_sync_failure = replace_root_after_injected_private_lease_sync_failure,
            .context = &context,
        });
    CHECK(!rejected);
    CHECK(!rejected.receipt.has_value());
    require_wave_status(rejected.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                        "root replacement outranks a selected sync failure");
    CHECK(!rejected.diagnostic.failed_private_lease_reservation_sync_site.has_value());
    CHECK(context.sync.selected);
    CHECK(context.after_failure_invoked);
    CHECK(context.observed_site.boundary == context.sync.target.boundary);
    CHECK(context.observed_site.point == context.sync.target.point);
    CHECK(context.root.invoked);
    CHECK(context.root.replaced);
    CHECK(context.root.native_error == 0);
    CHECK(!entry_exists_no_follow(root / names->reserved_pending_leaf));
    CHECK(entry_exists_no_follow(context.root.displaced / names->reserved_pending_leaf));
}

void test_wave_store_fresh_private_lease_successor_replacements() {
    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "fresh-reservation-marker-replacement";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create fresh marker-replacement fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        PrivateLeaseSuccessorMarkerReplacementContext context{
            .canonical = root / names->reserved_pending_leaf,
            .displaced = temp.path() / "original-reserved-pending",
        };

        auto claimed = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        auto rejected = wave_detail::reserve_worker_attempt_private_lease(
            std::move(claimed),
            wave_detail::DistributedSievePrivateLeaseProtocolTestHooks{
                .after_first_successor_validation =
                    replace_private_lease_successor_marker_after_first_validation,
                .context = &context,
            });
        CHECK(!rejected);
        CHECK(!rejected.receipt.has_value());
        require_wave_status(rejected.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "same-byte P1 successor replacement is rejected");
        CHECK(context.invoked);
        CHECK(context.replaced);
        CHECK(context.native_error == 0);
        CHECK(entry_exists_no_follow(context.canonical));
        CHECK(entry_exists_no_follow(context.displaced));
        require_distinct_entry_identities(context.canonical, context.displaced,
                                          "P1 replacement must use a distinct inode");
        CHECK(!entry_exists_no_follow(root / names->reserved_leaf));

        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "replacement P1 remains classifier-valid but transaction-distinct");
        std::error_code remove_error;
        CHECK(std::filesystem::remove(context.canonical, remove_error));
        CHECK(!remove_error);
        require_rename(context.displaced, context.canonical, "restore original P1 marker identity");
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "restoring original P1 identity closes the prefix");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "fresh-reservation-directory-replacement";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create fresh directory-replacement fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        PrivateLeaseSuccessorDirectoryReplacementContext context{
            .root = root,
            .relative_lease_stem = names->relative_lease_stem,
            .displaced = temp.path() / "original-staging-directory",
        };

        auto claimed = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        auto rejected = wave_detail::reserve_worker_attempt_private_lease(
            std::move(claimed),
            wave_detail::DistributedSievePrivateLeaseProtocolTestHooks{
                .after_first_successor_validation =
                    replace_private_lease_successor_directory_after_first_validation,
                .context = &context,
            });
        CHECK(!rejected);
        CHECK(!rejected.receipt.has_value());
        require_wave_status(rejected.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "same-name P3 successor directory replacement is rejected");
        CHECK(context.invoked);
        CHECK(context.replaced);
        CHECK(context.native_error == 0);
        CHECK(entry_exists_no_follow(context.canonical));
        CHECK(entry_exists_no_follow(context.displaced));
        require_distinct_entry_identities(context.canonical, context.displaced,
                                          "P3 replacement must use a distinct inode");
        CHECK(!entry_exists_no_follow(
            context.canonical / wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_PENDING_LEAF));
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "replacement P3 remains classifier-valid but transaction-distinct");

        std::error_code remove_error;
        CHECK(std::filesystem::remove(context.canonical, remove_error));
        CHECK(!remove_error);
        require_rename(context.displaced, context.canonical,
                       "restore original P3 directory identity");
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "restoring original P3 identity closes the prefix");
    }
}

void test_wave_store_private_lease_receipt_rejects_replacement() {
    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "reservation-receipt-owned-replacement";
    auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    auto& store = require_wave_ready(created, "create receipt replacement fixture");
    const auto& chunk = store.manifest().chunks.front();
    const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        chunk.relative_artifact_stem, chunk.chunk_id, 0);
    CHECK(names.has_value());

    auto claimed = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
    auto reserved = wave_detail::reserve_worker_attempt_private_lease(std::move(claimed));
    CHECK(reserved);
    CHECK(reserved.receipt.has_value());

    const auto owned = root / names->owned_leaf;
    const auto displaced = temp.path() / "original-owned-marker";
    WaveSameBytesReplacementContext context{
        .canonical = owned,
        .displaced = displaced,
        .bytes = read_file_bytes(owned),
    };
    require_wave_status(reserved.receipt->revalidate(
                            wave_detail::DistributedSievePrivateLeaseReservationReceiptTestHooks{
                                .after_first_target_validation =
                                    replace_marker_with_same_bytes_after_first_inventory,
                                .context = &context,
                            }),
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "receipt rejects same-byte OWNED replacement between target observations");
    CHECK(context.invoked);
    CHECK(context.replaced);
    CHECK(context.native_error == 0);
    require_distinct_entry_identities(owned, displaced,
                                      "P8 OWNED replacement must use a distinct inode");
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "same-byte OWNED replacement remains classifier-valid");

    std::error_code remove_error;
    CHECK(std::filesystem::remove(owned, remove_error));
    CHECK(!remove_error);
    require_rename(displaced, owned, "restore receipt-bound OWNED identity");
    require_wave_status(reserved.receipt->revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "receipt accepts the restored exact P8 witness");
}

void test_wave_store_private_lease_validation_hook_authority_sandwich() {
    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "successor-hook-root-replacement";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create successor-hook authority fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        PrivateLeaseSuccessorRootReplacementContext context{
            .root =
                WaveRootReplacementContext{
                    .canonical = root,
                    .displaced = temp.path() / "original-root-after-first-successor",
                },
        };

        auto claimed = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        auto rejected = wave_detail::reserve_worker_attempt_private_lease(
            std::move(claimed), wave_detail::DistributedSievePrivateLeaseProtocolTestHooks{
                                    .after_first_successor_validation =
                                        replace_private_lease_root_after_first_successor_validation,
                                    .context = &context,
                                });
        CHECK(!rejected);
        CHECK(!rejected.receipt.has_value());
        require_wave_status(rejected.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "successor hook root replacement outranks P1 acceptance");
        CHECK(context.root.invoked);
        CHECK(context.root.replaced);
        CHECK(context.root.native_error == 0);
        CHECK(!entry_exists_no_follow(root / names->reserved_pending_leaf));
        CHECK(entry_exists_no_follow(context.root.displaced / names->reserved_pending_leaf));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "receipt-hook-root-replacement";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create receipt-hook authority fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        auto claimed = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        auto reserved = wave_detail::reserve_worker_attempt_private_lease(std::move(claimed));
        CHECK(reserved);
        CHECK(reserved.receipt.has_value());
        WaveRootReplacementContext context{
            .canonical = root,
            .displaced = temp.path() / "original-root-after-first-receipt-target",
        };

        require_wave_status(
            reserved.receipt->revalidate(
                wave_detail::DistributedSievePrivateLeaseReservationReceiptTestHooks{
                    .after_first_target_validation = replace_wave_root_after_attempt_phase,
                    .context = &context,
                }),
            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
            "receipt hook root replacement outranks target confirmation");
        CHECK(context.invoked);
        CHECK(context.replaced);
        CHECK(context.native_error == 0);
        CHECK(!entry_exists_no_follow(root / names->owned_leaf));
        CHECK(entry_exists_no_follow(context.displaced / names->owned_leaf));
    }
}

void test_wave_store_classifies_all_private_lease_reservation_prefixes() {
    for (std::size_t index = 0; index < PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS.size();
         ++index) {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / ("reservation-prefix-" + std::to_string(index));
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create reservation-prefix fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());

        leave_relation_private_lease_reservation_prefix(
            root / names->private_directory_leaf / "corpus",
            PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS[index].relation_fault_point);
        CHECK(!entry_exists_no_follow(root / names->canonical_record_leaf));
        CHECK(!entry_exists_no_follow(root / names->pending_record_leaf));
        const auto prefix_snapshot = capture_wave_root_snapshot(root);
        std::optional<std::filesystem::path> reservation_directory;
        std::optional<std::pair<std::filesystem::path, WaveRootEntrySnapshot>>
            nested_owner_snapshot;
        if (index >= 3U) {
            const auto reserved = cleanup_detail::parse_private_lease_marker(
                read_file_bytes(root / names->reserved_leaf));
            reservation_directory =
                index == 8U
                    ? root / names->private_directory_leaf
                    : root /
                          (names->relative_lease_stem +
                           std::string(wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG) +
                           cleanup_detail::private_lease_id_hex(reserved.lease_id));
        }
        if (index >= 4U) {
            const auto owner_leaf =
                index == 4U
                    ? std::string(wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_PENDING_LEAF)
                    : std::string(wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF);
            const auto owner_path = *reservation_directory / owner_leaf;
            nested_owner_snapshot.emplace(owner_path,
                                          capture_wave_root_entry_snapshot(owner_path, owner_leaf));
        }
        const auto require_exact_prefix_snapshot = [&] {
            CHECK(capture_wave_root_snapshot(root) == prefix_snapshot);
            if (nested_owner_snapshot.has_value()) {
                CHECK(capture_wave_root_entry_snapshot(nested_owner_snapshot->first,
                                                       nested_owner_snapshot->second.leaf) ==
                      nested_owner_snapshot->second);
            }
            if (index == 3U) {
                CHECK(!entry_exists_no_follow(
                    *reservation_directory /
                    wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF));
                CHECK(!entry_exists_no_follow(
                    *reservation_directory /
                    wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_PENDING_LEAF));
            }
        };
        WaveReservationWitnessObservationContext observation{
            .expected_boundary = PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS[index].boundary,
            .expected_base_lock_leaf = names->base_lock_leaf,
        };
        require_wave_status(
            store.revalidate(wave_detail::DistributedSieveWaveStoreInventoryTestHooks{
                .observe_reservation_witnesses = observe_wave_reservation_witnesses,
                .context = &observation,
            }),
            wave_detail::DistributedSieveWaveStoreStatus::ready,
            "reservation prefix is a closed manifest-bound inventory");
        CHECK(observation.invoked);
        CHECK(observation.matched);
        require_exact_prefix_snapshot();

        auto root_claimed = store.claim_private_lease_root();
        auto& root_claim = require_private_lease_root_claim_ready(
            root_claimed, "reservation prefix admits a generic root claim");
        require_wave_status(root_claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "generic root claim retains reservation prefix witness");
        root_claimed.claim.reset();
        require_exact_prefix_snapshot();

        auto attempt_claimed = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        auto& attempt_claim = require_private_lease_root_claim_ready(
            attempt_claimed, "reservation prefix admits its exact attempt claim");
        require_wave_status(attempt_claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "attempt claim retains reservation prefix witness");
        const Digest digest = store.manifest_digest();
        attempt_claimed.claim.reset();
        require_exact_prefix_snapshot();
        created.store.reset();

        auto reopened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        auto& reopened_store =
            require_wave_ready(reopened, "reopen wave at exact reservation prefix");
        require_wave_status(reopened_store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "reopened wave retains exact reservation prefix");
        require_exact_prefix_snapshot();
    }
}

void test_wave_store_rejects_dual_private_lease_marker_states() {
    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "dual-private-lease-markers";
    auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    auto& store = require_wave_ready(created, "create dual-marker fixture");
    const auto& chunk = store.manifest().chunks.front();
    const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        chunk.relative_artifact_stem, chunk.chunk_id, 0);
    CHECK(names.has_value());
    const auto base_path = root / names->private_directory_leaf / "corpus";
    const auto paths = gnfs::relation::OOCCleanupTransaction::paths_for(base_path);

    leave_relation_private_lease_reservation_prefix(
        base_path, gnfs::relation::OOCPrivateLeaseFaultPoint::OwnedDurable);
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "owned-canonical reservation prefix is initially valid");
    const auto reserved_record =
        cleanup_detail::parse_private_lease_marker(read_file_bytes(paths.lease_reserved_path));
    const auto staging_leaf =
        names->relative_lease_stem +
        std::string(wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG) +
        cleanup_detail::private_lease_id_hex(reserved_record.lease_id);
    const auto staging = root / staging_leaf;

    const auto duplicate_and_reject = [&](const std::filesystem::path& canonical,
                                          const std::filesystem::path& pending,
                                          std::string_view context) {
        write_file_bytes(pending, read_file_bytes(canonical));
        require_chmod(pending, 0600, "chmod duplicate private-lease marker");
        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            context);
        std::error_code error;
        CHECK(std::filesystem::remove(pending, error));
        CHECK(!error);
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "removing dual marker restores the exact reservation prefix");
    };

    duplicate_and_reject(paths.lease_reserved_path, paths.lease_reserved_pending_path,
                         "canonical plus pending RESERVED is rejected");
    duplicate_and_reject(staging / wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF,
                         staging / wave_detail::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_PENDING_LEAF,
                         "canonical plus pending OWNER is rejected");
    duplicate_and_reject(paths.lease_owned_path, paths.lease_owned_pending_path,
                         "canonical plus pending OWNED is rejected");
}

void test_wave_store_private_lease_witness_rejects_same_bytes_replacement() {
    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "same-bytes-private-lease-replacement";
    auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    auto& store = require_wave_ready(created, "create same-bytes marker fixture");
    const auto& chunk = store.manifest().chunks.front();
    const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        chunk.relative_artifact_stem, chunk.chunk_id, 0);
    CHECK(names.has_value());
    const auto base_path = root / names->private_directory_leaf / "corpus";
    const auto paths = gnfs::relation::OOCCleanupTransaction::paths_for(base_path);
    leave_relation_private_lease_reservation_prefix(
        base_path, gnfs::relation::OOCPrivateLeaseFaultPoint::ReservedDurable);
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "reserved marker establishes the initial witness");

    const auto displaced_between_observations = temp.path() / "reserved-between-observations";
    WaveSameBytesReplacementContext between_observations{
        .canonical = paths.lease_reserved_path,
        .displaced = displaced_between_observations,
        .bytes = read_file_bytes(paths.lease_reserved_path),
    };
    require_wave_status(
        store.revalidate(wave_detail::DistributedSieveWaveStoreInventoryTestHooks{
            .after_first_validation = replace_marker_with_same_bytes_after_first_inventory,
            .context = &between_observations,
        }),
        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
        "same-byte marker replacement between inventory observations is rejected");
    CHECK(between_observations.invoked);
    CHECK(between_observations.replaced);
    CHECK(between_observations.native_error == 0);
    std::error_code error;
    CHECK(std::filesystem::remove(paths.lease_reserved_path, error));
    CHECK(!error);
    require_rename(displaced_between_observations, paths.lease_reserved_path,
                   "restore original reserved marker after inventory replacement");
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "restoring marker identity closes the wave inventory");

    auto claimed = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
    auto& claim =
        require_private_lease_root_claim_ready(claimed, "bind exact reserved-marker witness");
    const auto displaced_after_acquisition = temp.path() / "reserved-after-acquisition";
    WaveSameBytesReplacementContext after_acquisition{
        .canonical = paths.lease_reserved_path,
        .displaced = displaced_after_acquisition,
        .bytes = read_file_bytes(paths.lease_reserved_path),
    };
    replace_marker_with_same_bytes_after_first_inventory(&after_acquisition);
    CHECK(after_acquisition.invoked);
    CHECK(after_acquisition.replaced);
    CHECK(after_acquisition.native_error == 0);
    require_wave_status(claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "bound attempt claim rejects same-byte marker replacement");
    error.clear();
    CHECK(std::filesystem::remove(paths.lease_reserved_path, error));
    CHECK(!error);
    require_rename(displaced_after_acquisition, paths.lease_reserved_path,
                   "restore original reserved marker after claim replacement");
    require_wave_status(claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "invalidated bound attempt claim remains fail closed after repair");
    claimed.claim.reset();

    auto reopened = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
    auto& reopened_claim = require_private_lease_root_claim_ready(
        reopened, "reopen exact restored reservation prefix");
    require_wave_status(reopened_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "new attempt claim accepts restored original marker identity");
}

void test_wave_store_manifest_pending_rejects_valid_reservation_before_repair() {
    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "pending-manifest-fake-reservation";
    WaveFaultStopContext context{
        .target = wave_detail::DistributedSieveWaveStoreFaultPoint::ManifestPendingDurable,
    };
    auto interrupted = wave_detail::DistributedSieveWaveStore::create(
        root, wave_manifest_draft(),
        wave_detail::DistributedSieveWaveStoreTestHooks{
            .stop_after = stop_at_wave_fault,
            .context = &context,
        });
    CHECK(!interrupted);
    require_wave_status(interrupted.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::interrupted,
                        "leave pending manifest before fake reservation");
    const auto draft = wave_manifest_draft();
    const auto& chunk = draft.chunks.front();
    const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        chunk.relative_artifact_stem, chunk.chunk_id, 0);
    CHECK(names.has_value());
    const auto base_path = root / names->private_directory_leaf / "corpus";
    const auto paths = gnfs::relation::OOCCleanupTransaction::paths_for(base_path);
    leave_relation_private_lease_reservation_prefix(
        base_path, gnfs::relation::OOCPrivateLeaseFaultPoint::ReservedPendingDurable);
    CHECK(entry_exists_no_follow(paths.lock_path));
    CHECK(entry_exists_no_follow(paths.lease_reserved_pending_path));
    const Digest digest = manifest_digest_from_file(wave_manifest_pending_path(root));
    const auto before = capture_wave_root_snapshot(root);

    auto wrong_digest = digest;
    perturb_digest(wrong_digest);
    auto wrong_open = wave_detail::DistributedSieveWaveStore::open(root, wrong_digest);
    CHECK(!wrong_open);
    CHECK(wrong_open.store == nullptr);
    require_wave_status(wrong_open.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::manifest_conflict,
                        "manifest authority precedes valid pre-manifest reservation on open");
    CHECK(capture_wave_root_snapshot(root) == before);

    auto mismatched_draft = wave_manifest_draft();
    ++mismatched_draft.relation_cap_per_worker;
    auto wrong_create =
        wave_detail::DistributedSieveWaveStore::create(root, std::move(mismatched_draft));
    CHECK(!wrong_create);
    CHECK(wrong_create.store == nullptr);
    require_wave_status(wrong_create.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::manifest_conflict,
                        "manifest authority precedes valid pre-manifest reservation on create");
    CHECK(capture_wave_root_snapshot(root) == before);

    auto rejected = wave_detail::DistributedSieveWaveStore::open(root, digest);
    CHECK(!rejected);
    CHECK(rejected.store == nullptr);
    require_wave_status(rejected.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "pre-manifest valid reservation is rejected before publication repair");
    CHECK(capture_wave_root_snapshot(root) == before);
    CHECK(entry_exists_no_follow(wave_manifest_pending_path(root)));
    CHECK(!entry_exists_no_follow(wave_manifest_path(root)));

    auto create_rejected =
        wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    CHECK(!create_rejected);
    CHECK(create_rejected.store == nullptr);
    require_wave_status(create_rejected.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "idempotent create rejects valid pre-manifest reservation");
    CHECK(capture_wave_root_snapshot(root) == before);
    CHECK(entry_exists_no_follow(wave_manifest_pending_path(root)));
    CHECK(!entry_exists_no_follow(wave_manifest_path(root)));

    std::error_code error;
    CHECK(std::filesystem::remove(paths.lease_reserved_pending_path, error));
    CHECK(!error);
    auto recovered = wave_detail::DistributedSieveWaveStore::open(root, digest);
    auto& recovered_store =
        require_wave_ready(recovered, "recover pending manifest after reservation removal");
    CHECK(!entry_exists_no_follow(wave_manifest_pending_path(root)));
    CHECK(entry_exists_no_follow(wave_manifest_path(root)));
    CHECK(entry_exists_no_follow(paths.lock_path));
    require_wave_status(recovered_store.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "recovered manifest retains the BaseLock-only attempt prefix");
}

void test_wave_store_attempt_base_lock_create_recover_and_phase_contract() {
    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "attempt-base-lock-phase";
    auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    auto& store = require_wave_ready(created, "create attempt BaseLock phase fixture");
    const auto& first_chunk = store.manifest().chunks.at(0);
    const auto& second_chunk = store.manifest().chunks.at(1);
    const auto first_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        first_chunk.relative_artifact_stem, first_chunk.chunk_id, 0);
    const auto second_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        second_chunk.relative_artifact_stem, second_chunk.chunk_id, 0);
    CHECK(first_names.has_value());
    CHECK(second_names.has_value());
    const auto first_lock = root / first_names->base_lock_leaf;
    const auto second_lock = root / second_names->base_lock_leaf;
    const auto pristine_namespace = capture_wave_root_snapshot(root);

    auto missing = store.open_worker_attempt_private_lease_root(first_chunk.chunk_id, 0);
    CHECK(!missing);
    CHECK(missing.claim == nullptr);
    require_wave_status(missing.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "open-existing attempt BaseLock never creates a missing leaf");
    CHECK(!entry_exists_no_follow(first_lock));
    CHECK(capture_wave_root_snapshot(root) == pristine_namespace);

    auto unknown_chunk = store.create_worker_attempt_private_lease_root(
        sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS - 1U, 0);
    CHECK(!unknown_chunk);
    CHECK(unknown_chunk.claim == nullptr);
    require_wave_status(unknown_chunk.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::invalid_request,
                        "unknown attempt chunk rejected before observation");
    auto exhausted_ordinal = store.create_worker_attempt_private_lease_root(
        first_chunk.chunk_id, store.manifest().max_worker_attempts);
    CHECK(!exhausted_ordinal);
    CHECK(exhausted_ordinal.claim == nullptr);
    require_wave_status(exhausted_ordinal.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::invalid_request,
                        "out-of-manifest attempt ordinal rejected before observation");
    CHECK(!entry_exists_no_follow(first_lock));
    CHECK(!entry_exists_no_follow(second_lock));
    CHECK(capture_wave_root_snapshot(root) == pristine_namespace);

    const auto empty_root = temp.path() / "attempt-base-lock-empty-chunk";
    auto empty_draft = wave_manifest_draft();
    empty_draft.effective_sq_end = empty_draft.chunks.front().sq_end;
    empty_draft.chunks.back().sq_begin = empty_draft.chunks.front().sq_end;
    empty_draft.chunks.back().sq_end = empty_draft.chunks.front().sq_end;
    auto empty_created =
        wave_detail::DistributedSieveWaveStore::create(empty_root, std::move(empty_draft));
    auto& empty_store =
        require_wave_ready(empty_created, "create empty-chunk attempt BaseLock fixture");
    const auto& empty_chunk = empty_store.manifest().chunks.back();
    const auto empty_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        empty_chunk.relative_artifact_stem, empty_chunk.chunk_id, 0);
    CHECK(empty_names.has_value());
    const auto empty_namespace = capture_wave_root_snapshot(empty_root);
    auto empty_rejected =
        empty_store.create_worker_attempt_private_lease_root(empty_chunk.chunk_id, 0);
    CHECK(!empty_rejected);
    CHECK(empty_rejected.claim == nullptr);
    require_wave_status(empty_rejected.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::invalid_request,
                        "empty manifest chunk cannot reserve a worker BaseLock");
    CHECK(!entry_exists_no_follow(empty_root / empty_names->base_lock_leaf));
    CHECK(capture_wave_root_snapshot(empty_root) == empty_namespace);

    auto fresh = store.create_worker_attempt_private_lease_root(first_chunk.chunk_id, 0);
    auto& fresh_claim =
        require_private_lease_root_claim_ready(fresh, "create fresh attempt BaseLockAt");
    CHECK(entry_exists_no_follow(first_lock));
    CHECK(!entry_exists_no_follow(root / first_names->private_directory_leaf));
    require_strict_empty_base_lock(first_lock, "strict fresh attempt BaseLock metadata");
    const auto relation_paths = gnfs::relation::OOCCleanupTransaction::paths_for(
        root / first_names->private_directory_leaf / "corpus");
    CHECK(relation_paths.lock_path == first_lock);
    CHECK(relation_paths.private_directory == root / first_names->private_directory_leaf);
    require_wave_status(fresh_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "fresh attempt BaseLock claim revalidates");
    require_wave_status(fresh_claim.revalidate_authority(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "fresh attempt BaseLock authority revalidates");
    CHECK(relation_base_lock_reports_busy(first_lock));
    const auto first_claimed_namespace = capture_wave_root_snapshot(root);

    auto invalid_while_busy = store.create_worker_attempt_private_lease_root(
        sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS - 1U, 0);
    CHECK(!invalid_while_busy);
    CHECK(invalid_while_busy.claim == nullptr);
    require_wave_status(invalid_while_busy.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::invalid_request,
                        "invalid coordinate precedes same-State root contention");
    CHECK(capture_wave_root_snapshot(root) == first_claimed_namespace);

    auto same_state = store.create_worker_attempt_private_lease_root(second_chunk.chunk_id, 0);
    CHECK(!same_state);
    CHECK(same_state.claim == nullptr);
    require_wave_status(same_state.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::private_lease_root_busy,
                        "attempt transactions serialize one same-State root action");
    CHECK(!entry_exists_no_follow(second_lock));
    CHECK(capture_wave_root_snapshot(root) == first_claimed_namespace);

    fresh.claim.reset();
    const auto first_released_namespace = capture_wave_root_snapshot(root);
    auto duplicate_fresh = store.create_worker_attempt_private_lease_root(first_chunk.chunk_id, 0);
    CHECK(!duplicate_fresh);
    CHECK(duplicate_fresh.claim == nullptr);
    require_wave_status(duplicate_fresh.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "fresh entry point never degrades to open-existing");
    CHECK(capture_wave_root_snapshot(root) == first_released_namespace);

    auto recovered = store.open_worker_attempt_private_lease_root(first_chunk.chunk_id, 0);
    auto& recovered_claim =
        require_private_lease_root_claim_ready(recovered, "open existing attempt BaseLockAt");
    require_wave_status(recovered_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "recovered attempt BaseLock claim revalidates");
    CHECK(relation_base_lock_reports_busy(first_lock));
    recovered.claim.reset();
    CHECK(capture_wave_root_snapshot(root) == first_released_namespace);

    std::unique_ptr<cleanup_detail::BaseLock> external_holder;
    try {
        external_holder = std::make_unique<cleanup_detail::BaseLock>(first_lock, false);
    } catch (const cleanup_detail::Failure& failure) {
        fail("hold existing attempt BaseLock externally", __LINE__,
             std::to_string(static_cast<int>(failure.status)));
    }
    const auto externally_held_namespace = capture_wave_root_snapshot(root);
    auto fresh_while_target_busy =
        store.create_worker_attempt_private_lease_root(first_chunk.chunk_id, 0);
    CHECK(!fresh_while_target_busy);
    CHECK(fresh_while_target_busy.claim == nullptr);
    require_wave_status(fresh_while_target_busy.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "fresh-existing conflict precedes target-lock contention");
    CHECK(capture_wave_root_snapshot(root) == externally_held_namespace);
    auto busy = store.open_worker_attempt_private_lease_root(first_chunk.chunk_id, 0);
    CHECK(!busy);
    CHECK(busy.claim == nullptr);
    require_wave_status(busy.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::private_lease_lock_busy,
                        "existing attempt BaseLock reports target-lock contention");
    CHECK(capture_wave_root_snapshot(root) == externally_held_namespace);
    external_holder.reset();
    auto retry = store.open_worker_attempt_private_lease_root(first_chunk.chunk_id, 0);
    auto& retry_claim =
        require_private_lease_root_claim_ready(retry, "retry existing attempt BaseLock");
    require_wave_status(retry_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "retry after target-lock release revalidates");
    retry.claim.reset();

    const auto before_between_phase_foreign = capture_wave_root_snapshot(root);
    const auto between_phase_foreign = root / "between-phase.unexpected-control";
    AttemptPhaseForeignContext foreign_context{
        .foreign = between_phase_foreign,
    };
    auto between_phase_foreign_rejected = store.create_worker_attempt_private_lease_root(
        second_chunk.chunk_id, 0,
        wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
            .after_initial_phase_validation = insert_foreign_after_attempt_phase,
            .context = &foreign_context,
        });
    CHECK(foreign_context.invoked);
    CHECK(foreign_context.inserted);
    CHECK(foreign_context.native_error == 0);
    CHECK(!between_phase_foreign_rejected);
    CHECK(between_phase_foreign_rejected.claim == nullptr);
    require_wave_status(between_phase_foreign_rejected.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "foreign leaf inserted between phase witnesses is rejected");
    CHECK(!entry_exists_no_follow(second_lock));
    auto after_between_phase_foreign = capture_wave_root_snapshot(root);
    erase_wave_root_snapshot_leaf(after_between_phase_foreign,
                                  between_phase_foreign.filename().string());
    CHECK(after_between_phase_foreign == before_between_phase_foreign);
    CHECK(read_file_bytes(between_phase_foreign).empty());
    require_strict_empty_base_lock(between_phase_foreign,
                                   "between-phase foreign leaf remains exact and strict");
    std::error_code remove_error;
    CHECK(std::filesystem::remove(between_phase_foreign, remove_error));
    CHECK(!remove_error);

    const auto between_phase_displaced = temp.path() / "between-phase-existing-attempt-base-lock";
    const auto before_between_phase_replacement = capture_wave_root_snapshot(root);
    WaveBaseLockReplacementContext between_phase_lock_context{
        .canonical = first_lock,
        .displaced = between_phase_displaced,
    };
    auto between_phase_replacement_rejected = store.create_worker_attempt_private_lease_root(
        second_chunk.chunk_id, 0,
        wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
            .after_initial_phase_validation = replace_base_lock_after_first_inventory,
            .context = &between_phase_lock_context,
        });
    CHECK(between_phase_lock_context.invoked);
    CHECK(between_phase_lock_context.replaced);
    CHECK(between_phase_lock_context.native_error == 0);
    CHECK(!between_phase_replacement_rejected);
    CHECK(between_phase_replacement_rejected.claim == nullptr);
    require_wave_status(between_phase_replacement_rejected.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "non-target BaseLock identity drift between witnesses is rejected");
    CHECK(!entry_exists_no_follow(second_lock));
    auto before_without_first_lock = before_between_phase_replacement;
    auto after_without_first_lock = capture_wave_root_snapshot(root);
    erase_wave_root_snapshot_leaf(before_without_first_lock, first_names->base_lock_leaf);
    erase_wave_root_snapshot_leaf(after_without_first_lock, first_names->base_lock_leaf);
    CHECK(after_without_first_lock == before_without_first_lock);
    require_strict_empty_base_lock(first_lock,
                                   "between-phase replacement BaseLock remains preserved");
    require_strict_empty_base_lock(between_phase_displaced,
                                   "between-phase displaced BaseLock remains preserved");
    CHECK(std::filesystem::remove(first_lock, remove_error));
    CHECK(!remove_error);
    require_rename(between_phase_displaced, first_lock,
                   "restore between-phase non-target BaseLock");
    CHECK(capture_wave_root_snapshot(root) == before_between_phase_replacement);

    const auto foreign = root / "unexpected.attempt-control";
    write_foreign_leaf(foreign);
    const auto namespace_with_foreign = capture_wave_root_snapshot(root);
    auto foreign_rejected =
        store.create_worker_attempt_private_lease_root(second_chunk.chunk_id, 0);
    CHECK(!foreign_rejected);
    CHECK(foreign_rejected.claim == nullptr);
    require_wave_status(foreign_rejected.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "fresh attempt requires a closed pre-mutation phase");
    CHECK(!entry_exists_no_follow(second_lock));
    CHECK(capture_wave_root_snapshot(root) == namespace_with_foreign);
    remove_error.clear();
    CHECK(std::filesystem::remove(foreign, remove_error));
    CHECK(!remove_error);

    auto second_fresh = store.create_worker_attempt_private_lease_root(second_chunk.chunk_id, 0);
    auto& second_claim = require_private_lease_root_claim_ready(
        second_fresh, "fresh attempt succeeds after foreign phase repair");
    require_wave_status(second_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "second manifest-bound attempt BaseLock revalidates");
    require_strict_empty_base_lock(second_lock, "strict second attempt BaseLock metadata");
}

void test_wave_store_attempt_base_lock_durability_prefixes() {
    WaveStoreTempDirectory temp;
    for (std::size_t failure_index = 0; failure_index < PRIVATE_LEASE_BASE_LOCK_SYNC_POINTS.size();
         ++failure_index) {
        const auto root = temp.path() / ("attempt-base-lock-sync-" + std::to_string(failure_index));
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create attempt BaseLock durability fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        const auto target = root / names->base_lock_leaf;
        const auto before = capture_wave_root_snapshot(root);

        BaseLockSyncFailureContext context{
            .target = PRIVATE_LEASE_BASE_LOCK_SYNC_POINTS[failure_index],
        };
        auto interrupted = store.create_worker_attempt_private_lease_root(
            chunk.chunk_id, 0,
            wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
                .fail_before_sync = fail_before_base_lock_sync,
                .context = &context,
            });
        CHECK(!interrupted);
        CHECK(interrupted.claim == nullptr);
        require_wave_status(interrupted.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::durability_failed,
                            "deterministic attempt BaseLock durability interruption");
        CHECK(interrupted.diagnostic.failed_private_lease_base_lock_sync_point == context.target);
        for (std::size_t observed_index = 0;
             observed_index < PRIVATE_LEASE_BASE_LOCK_SYNC_POINTS.size(); ++observed_index) {
            CHECK(context.observed[observed_index] == (observed_index <= failure_index));
        }

        CHECK(entry_exists_no_follow(target));
        require_strict_empty_base_lock(target,
                                       "interrupted attempt BaseLock prefix remains strict");
        CHECK(!relation_base_lock_reports_busy(target));
        auto after_interruption = capture_wave_root_snapshot(root);
        erase_wave_root_snapshot_leaf(after_interruption, names->base_lock_leaf);
        CHECK(after_interruption == before);
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "interrupted BaseLock prefix remains closed inventory");

        const auto preserved_prefix = capture_wave_root_snapshot(root);
        auto fresh_retry = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        CHECK(!fresh_retry);
        CHECK(fresh_retry.claim == nullptr);
        require_wave_status(fresh_retry.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "durability interruption never retries as fresh creation");
        CHECK(capture_wave_root_snapshot(root) == preserved_prefix);

        std::unique_ptr<cleanup_detail::BaseLock> external_holder;
        try {
            external_holder = std::make_unique<cleanup_detail::BaseLock>(target, false);
        } catch (const cleanup_detail::Failure& failure) {
            fail("hold interrupted BaseLock after failed transaction", __LINE__,
                 std::to_string(static_cast<int>(failure.status)));
        }
        auto externally_contended = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        CHECK(!externally_contended);
        CHECK(externally_contended.claim == nullptr);
        require_wave_status(
            externally_contended.diagnostic,
            wave_detail::DistributedSieveWaveStoreStatus::private_lease_lock_busy,
            "durability failure releases root slot but external target lock is observed");
        CHECK(capture_wave_root_snapshot(root) == preserved_prefix);
        external_holder.reset();

        auto recovered = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        auto& claim = require_private_lease_root_claim_ready(
            recovered, "explicit recovery completes interrupted BaseLock durability");
        require_wave_status(claim.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "recovered interrupted BaseLock claim revalidates");
        require_wave_status(claim.revalidate_authority(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "recovered interrupted BaseLock authority revalidates");
        CHECK(relation_base_lock_reports_busy(target));
        recovered.claim.reset();
        CHECK(capture_wave_root_snapshot(root) == preserved_prefix);
        CHECK(!relation_base_lock_reports_busy(target));
    }
}

void test_wave_store_attempt_base_lock_state_scope_concurrency() {
    WaveStoreTempDirectory temp;
    const auto shared_root = temp.path() / "attempt-base-lock-shared-state";
    auto shared_created =
        wave_detail::DistributedSieveWaveStore::create(shared_root, wave_manifest_draft());
    auto& shared_store =
        require_wave_ready(shared_created, "create same-State attempt concurrency fixture");
    const auto& first_chunk = shared_store.manifest().chunks.at(0);
    const auto& second_chunk = shared_store.manifest().chunks.at(1);
    const std::array chunk_ids{first_chunk.chunk_id, second_chunk.chunk_id};
    const auto first_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        first_chunk.relative_artifact_stem, first_chunk.chunk_id, 0);
    const auto second_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        second_chunk.relative_artifact_stem, second_chunk.chunk_id, 0);
    CHECK(first_names.has_value());
    CHECK(second_names.has_value());
    const std::array targets{
        shared_root / first_names->base_lock_leaf,
        shared_root / second_names->base_lock_leaf,
    };
    const auto shared_baseline = capture_wave_root_snapshot(shared_root);

    std::array<std::unique_ptr<PrivateLeaseRootClaim>, 2> shared_claims;
    std::array<wave_detail::DistributedSieveWaveStoreDiagnostic, 2> shared_diagnostics;
    std::barrier<> shared_phases(3);
    const auto claim_shared_attempt = [&](std::size_t index) {
        shared_phases.arrive_and_wait();
        auto result = shared_store.create_worker_attempt_private_lease_root(chunk_ids[index], 0);
        shared_diagnostics[index] = result.diagnostic;
        shared_claims[index] = std::move(result.claim);
        shared_phases.arrive_and_wait();
    };

    std::thread first_shared(claim_shared_attempt, 0);
    std::thread second_shared(claim_shared_attempt, 1);
    shared_phases.arrive_and_wait();
    shared_phases.arrive_and_wait();
    first_shared.join();
    second_shared.join();

    const std::size_t shared_success_count = static_cast<std::size_t>(shared_claims[0] != nullptr) +
                                             static_cast<std::size_t>(shared_claims[1] != nullptr);
    CHECK(shared_success_count == 1);
    const std::size_t winner = shared_claims[0] != nullptr ? 0 : 1;
    const std::size_t loser = 1U - winner;
    require_wave_status(shared_diagnostics[winner],
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "same-State attempt concurrency winner");
    require_wave_status(shared_diagnostics[loser],
                        wave_detail::DistributedSieveWaveStoreStatus::private_lease_root_busy,
                        "same-State attempt concurrency loser");
    CHECK(entry_exists_no_follow(targets[winner]));
    CHECK(!entry_exists_no_follow(targets[loser]));
    require_strict_empty_base_lock(targets[winner],
                                   "same-State concurrency winner BaseLock is strict");
    CHECK(relation_base_lock_reports_busy(targets[winner]));
    require_wave_status(shared_claims[winner]->revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "same-State concurrency winner revalidates");
    require_wave_status(shared_claims[winner]->revalidate_authority(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "same-State concurrency winner authority revalidates");
    auto shared_after = capture_wave_root_snapshot(shared_root);
    erase_wave_root_snapshot_leaf(shared_after, winner == 0 ? first_names->base_lock_leaf
                                                            : second_names->base_lock_leaf);
    CHECK(shared_after == shared_baseline);

    shared_claims[winner].reset();
    auto loser_retry = shared_store.create_worker_attempt_private_lease_root(chunk_ids[loser], 0);
    auto& loser_claim = require_private_lease_root_claim_ready(
        loser_retry, "same-State concurrency loser retries after release");
    require_wave_status(loser_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "retried same-State attempt claim revalidates");
    CHECK(entry_exists_no_follow(targets[loser]));

    const std::array independent_roots{
        temp.path() / "attempt-base-lock-independent-a",
        temp.path() / "attempt-base-lock-independent-b",
    };
    auto independent_first =
        wave_detail::DistributedSieveWaveStore::create(independent_roots[0], wave_manifest_draft());
    auto independent_second =
        wave_detail::DistributedSieveWaveStore::create(independent_roots[1], wave_manifest_draft());
    auto& independent_first_store =
        require_wave_ready(independent_first, "create first independent-State fixture");
    auto& independent_second_store =
        require_wave_ready(independent_second, "create second independent-State fixture");
    const std::array<wave_detail::DistributedSieveWaveStore*, 2> independent_stores{
        &independent_first_store,
        &independent_second_store,
    };
    const auto independent_chunk_id = independent_first_store.manifest().chunks.front().chunk_id;
    const auto independent_first_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        independent_first_store.manifest().chunks.front().relative_artifact_stem,
        independent_chunk_id, 0);
    const auto independent_second_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        independent_second_store.manifest().chunks.front().relative_artifact_stem,
        independent_chunk_id, 0);
    CHECK(independent_first_names.has_value());
    CHECK(independent_second_names.has_value());
    const std::array independent_targets{
        independent_roots[0] / independent_first_names->base_lock_leaf,
        independent_roots[1] / independent_second_names->base_lock_leaf,
    };
    const std::array independent_baselines{
        capture_wave_root_snapshot(independent_roots[0]),
        capture_wave_root_snapshot(independent_roots[1]),
    };
    std::array<std::unique_ptr<PrivateLeaseRootClaim>, 2> independent_claims;
    std::array<wave_detail::DistributedSieveWaveStoreDiagnostic, 2> independent_diagnostics;
    std::barrier<> independent_phases(3);
    const auto claim_independent_attempt = [&](std::size_t index) {
        independent_phases.arrive_and_wait();
        auto result = independent_stores[index]->create_worker_attempt_private_lease_root(
            independent_chunk_id, 0);
        independent_diagnostics[index] = result.diagnostic;
        independent_claims[index] = std::move(result.claim);
        independent_phases.arrive_and_wait();
    };

    std::thread first_independent(claim_independent_attempt, 0);
    std::thread second_independent(claim_independent_attempt, 1);
    independent_phases.arrive_and_wait();
    independent_phases.arrive_and_wait();
    first_independent.join();
    second_independent.join();

    CHECK(independent_claims[0] != nullptr);
    CHECK(independent_claims[1] != nullptr);
    require_wave_status(independent_diagnostics[0],
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "first independent-State attempt succeeds concurrently");
    require_wave_status(independent_diagnostics[1],
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "second independent-State attempt succeeds concurrently");
    require_wave_status(independent_claims[0]->revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "first independent-State claim revalidates");
    require_wave_status(independent_claims[1]->revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "second independent-State claim revalidates");
    require_wave_status(independent_claims[0]->revalidate_authority(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "first independent-State authority revalidates");
    require_wave_status(independent_claims[1]->revalidate_authority(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "second independent-State authority revalidates");
    for (std::size_t index = 0; index < independent_targets.size(); ++index) {
        require_strict_empty_base_lock(independent_targets[index],
                                       "independent-State concurrent BaseLock is strict");
        CHECK(relation_base_lock_reports_busy(independent_targets[index]));
        auto observed = capture_wave_root_snapshot(independent_roots[index]);
        erase_wave_root_snapshot_leaf(observed, index == 0
                                                    ? independent_first_names->base_lock_leaf
                                                    : independent_second_names->base_lock_leaf);
        CHECK(observed == independent_baselines[index]);
    }

    independent_claims[0].reset();
    independent_claims[1].reset();
    auto independent_first_recovered =
        independent_first_store.open_worker_attempt_private_lease_root(independent_chunk_id, 0);
    auto independent_second_recovered =
        independent_second_store.open_worker_attempt_private_lease_root(independent_chunk_id, 0);
    (void)require_private_lease_root_claim_ready(
        independent_first_recovered,
        "first independent-State target reopens after concurrent release");
    (void)require_private_lease_root_claim_ready(
        independent_second_recovered,
        "second independent-State target reopens after concurrent release");
}

void test_wave_store_attempt_base_lock_bound_claim_exact_inventory() {
    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "attempt-bound-claim-added-lock";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create bound-claim added-lock fixture");
        const auto& target_chunk = store.manifest().chunks.at(0);
        const auto& added_chunk = store.manifest().chunks.at(1);
        const auto target_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            target_chunk.relative_artifact_stem, target_chunk.chunk_id, 0);
        const auto added_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            added_chunk.relative_artifact_stem, added_chunk.chunk_id, 0);
        CHECK(target_names.has_value());
        CHECK(added_names.has_value());
        const auto target = root / target_names->base_lock_leaf;
        const auto added = root / added_names->base_lock_leaf;

        auto bound = store.create_worker_attempt_private_lease_root(target_chunk.chunk_id, 0);
        auto& claim =
            require_private_lease_root_claim_ready(bound, "create exact-inventory bound claim");
        const auto exact_successor = capture_wave_root_snapshot(root);
        write_empty_foreign_leaf(added);
        require_strict_empty_base_lock(added, "otherwise valid added attempt BaseLock is strict");
        auto added_namespace = capture_wave_root_snapshot(root);
        erase_wave_root_snapshot_leaf(added_namespace, added_names->base_lock_leaf);
        CHECK(added_namespace == exact_successor);
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "generic WaveStore accepts added manifest-valid attempt BaseLock");
        require_wave_status(claim.revalidate_authority(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "bound authority-only check deliberately ignores added inventory");
        require_wave_status(claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "bound claim rejects added manifest-valid attempt BaseLock");
        CHECK(relation_base_lock_reports_busy(target));

        std::error_code remove_error;
        CHECK(std::filesystem::remove(added, remove_error));
        CHECK(!remove_error);
        CHECK(capture_wave_root_snapshot(root) == exact_successor);
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "generic store recovers after added BaseLock removal");
        require_wave_status(claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "bound target stays sticky-invalid after exact inventory repair");
        bound.claim.reset();

        auto recovered = store.open_worker_attempt_private_lease_root(target_chunk.chunk_id, 0);
        auto& recovered_claim = require_private_lease_root_claim_ready(
            recovered, "explicitly reopen target after added-inventory failure");
        require_wave_status(recovered_claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "new claim accepts repaired exact inventory");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "attempt-bound-claim-nontarget-split";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create bound-claim non-target fixture");
        const auto& non_target_chunk = store.manifest().chunks.at(0);
        const auto& target_chunk = store.manifest().chunks.at(1);
        const auto non_target_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            non_target_chunk.relative_artifact_stem, non_target_chunk.chunk_id, 0);
        const auto target_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            target_chunk.relative_artifact_stem, target_chunk.chunk_id, 0);
        CHECK(non_target_names.has_value());
        CHECK(target_names.has_value());
        const auto non_target = root / non_target_names->base_lock_leaf;
        const auto target = root / target_names->base_lock_leaf;

        auto non_target_created =
            store.create_worker_attempt_private_lease_root(non_target_chunk.chunk_id, 0);
        (void)require_private_lease_root_claim_ready(non_target_created,
                                                     "create non-target exact-inventory BaseLock");
        non_target_created.claim.reset();
        auto bound = store.create_worker_attempt_private_lease_root(target_chunk.chunk_id, 0);
        auto& claim = require_private_lease_root_claim_ready(
            bound, "create target claim with pre-existing non-target BaseLock");
        const auto exact_successor = capture_wave_root_snapshot(root);
        const auto original_non_target =
            require_wave_root_snapshot_leaf(exact_successor, non_target_names->base_lock_leaf);
        const auto displaced = temp.path() / "attempt-bound-claim-original-nontarget";
        WaveBaseLockReplacementContext context{
            .canonical = non_target,
            .displaced = displaced,
        };
        replace_base_lock_after_first_inventory(&context);
        CHECK(context.invoked);
        CHECK(context.replaced);
        CHECK(context.native_error == 0);
        require_strict_empty_base_lock(non_target,
                                       "bound-claim non-target replacement remains strict");
        require_strict_empty_base_lock(displaced,
                                       "bound-claim displaced non-target remains strict");
        CHECK(capture_wave_root_entry_snapshot(displaced, non_target_names->base_lock_leaf) ==
              original_non_target);
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "generic WaveStore accepts stable non-target replacement");
        require_wave_status(claim.revalidate_authority(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "bound authority-only check accepts unchanged own target");
        require_wave_status(claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "bound claim rejects non-target BaseLock identity replacement");
        CHECK(relation_base_lock_reports_busy(target));
        CHECK(!relation_base_lock_reports_busy(non_target));
        CHECK(!relation_base_lock_reports_busy(displaced));

        std::error_code remove_error;
        CHECK(std::filesystem::remove(non_target, remove_error));
        CHECK(!remove_error);
        require_rename(displaced, non_target, "restore bound-claim exact non-target identity");
        CHECK(capture_wave_root_snapshot(root) == exact_successor);
        require_wave_status(claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "bound target stays sticky-invalid after non-target identity repair");
        bound.claim.reset();

        auto recovered = store.open_worker_attempt_private_lease_root(target_chunk.chunk_id, 0);
        auto& recovered_claim = require_private_lease_root_claim_ready(
            recovered, "explicitly reopen target after non-target repair");
        require_wave_status(recovered_claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "new target claim accepts restored exact non-target identity");
    }
}

void test_wave_store_attempt_base_lock_fork_binding_and_close_only_lifetime() {
    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "attempt-base-lock-fork";
    auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    auto& store = require_wave_ready(created, "create attempt BaseLock fork fixture");
    const auto& chunk = store.manifest().chunks.front();
    const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        chunk.relative_artifact_stem, chunk.chunk_id, 0);
    CHECK(names.has_value());
    const auto target = root / names->base_lock_leaf;
    auto claimed = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
    auto& claim =
        require_private_lease_root_claim_ready(claimed, "create attempt BaseLock before fork");

    int ready_pipe[2]{-1, -1};
    int release_pipe[2]{-1, -1};
    CHECK(::pipe(ready_pipe) == 0);
    CHECK(::pipe(release_pipe) == 0);
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)::close(ready_pipe[0]);
        (void)::close(release_pipe[1]);
        const auto inherited_full = claim.revalidate();
        const auto inherited_authority = claim.revalidate_authority();
        auto inherited_open = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        const bool rejected = !claim.owned_by_current_process() &&
                              inherited_full.status ==
                                  wave_detail::DistributedSieveWaveStoreStatus::invalid_request &&
                              inherited_authority.status ==
                                  wave_detail::DistributedSieveWaveStoreStatus::invalid_request &&
                              !inherited_open && inherited_open.claim == nullptr &&
                              inherited_open.diagnostic.status ==
                                  wave_detail::DistributedSieveWaveStoreStatus::invalid_request;
        claimed.claim.reset();
        const bool signalled = write_pipe_byte(ready_pipe[1], rejected ? 'r' : 'f');
        char release = '\0';
        const bool released = read_pipe_byte(release_pipe[0], release);
        ::_exit(rejected && signalled && released && release == 'x' ? 0 : 85);
    }

    (void)::close(ready_pipe[1]);
    (void)::close(release_pipe[0]);
    char ready = '\0';
    const bool received = read_pipe_byte(ready_pipe[0], ready);
    CHECK(received);
    CHECK(ready == 'r');
    CHECK(relation_base_lock_reports_busy(target));
    require_wave_status(claim.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "parent attempt BaseLock survives child close");

    const bool released = write_pipe_byte(release_pipe[1], 'x');
    (void)::close(ready_pipe[0]);
    (void)::close(release_pipe[1]);
    int child_status = 0;
    const bool waited = wait_for_child(child, child_status);
    CHECK(released);
    CHECK(waited);
    CHECK(WIFEXITED(child_status));
    CHECK(WEXITSTATUS(child_status) == 0);
    CHECK(relation_base_lock_reports_busy(target));
    require_wave_status(claim.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "parent attempt BaseLock survives child exit");

    claimed.claim.reset();
    auto reopened = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
    auto& reopened_claim =
        require_private_lease_root_claim_ready(reopened, "reopen after final parent close");
    require_wave_status(reopened_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "reopened attempt BaseLock revalidates after fork");
}

void test_wave_store_attempt_base_lock_pre_mutation_authority_replacement() {
    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "attempt-authority-replacement";
    auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    auto& store = require_wave_ready(created, "create attempt authority-replacement fixture");
    const auto& chunk = store.manifest().chunks.front();
    const auto& post_lock_chunk = store.manifest().chunks.at(1);
    const auto first_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        chunk.relative_artifact_stem, chunk.chunk_id, 0);
    const auto second_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        chunk.relative_artifact_stem, chunk.chunk_id, 1);
    const auto post_root_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        post_lock_chunk.relative_artifact_stem, post_lock_chunk.chunk_id, 0);
    const auto post_wave_lock_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        post_lock_chunk.relative_artifact_stem, post_lock_chunk.chunk_id, 1);
    CHECK(first_names.has_value());
    CHECK(second_names.has_value());
    CHECK(post_root_names.has_value());
    CHECK(post_wave_lock_names.has_value());

    const auto original_root = temp.path() / "attempt-authority-original-root";
    const auto before_root_replacement = capture_wave_root_snapshot(root);
    WaveRootReplacementContext root_context{
        .canonical = root,
        .displaced = original_root,
    };
    auto root_rejected = store.create_worker_attempt_private_lease_root(
        chunk.chunk_id, 0,
        wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
            .after_initial_phase_validation = replace_wave_root_after_attempt_phase,
            .context = &root_context,
        });
    CHECK(root_context.invoked);
    CHECK(root_context.replaced);
    CHECK(root_context.native_error == 0);
    CHECK(!root_rejected);
    CHECK(root_rejected.claim == nullptr);
    require_wave_status(root_rejected.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                        "root identity replacement rejected before BaseLock mutation");
    CHECK(!entry_exists_no_follow(root / first_names->base_lock_leaf));
    CHECK(!entry_exists_no_follow(original_root / first_names->base_lock_leaf));
    CHECK(capture_wave_root_snapshot(root).size() == 1);
    CHECK(capture_wave_root_snapshot(original_root) == before_root_replacement);

    const auto observed_replacement = temp.path() / "attempt-authority-replacement-root";
    require_rename(root, observed_replacement, "preserve replacement attempt root");
    require_rename(original_root, root, "restore original attempt root");
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "store revalidates after attempt-root restoration");
    auto first_created = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
    (void)require_private_lease_root_claim_ready(first_created,
                                                 "fresh BaseLock succeeds after root restoration");
    first_created.claim.reset();

    const auto original_wave_lock = temp.path() / "attempt-authority-original-wave-lock";
    const auto before_wave_lock_replacement = capture_wave_root_snapshot(root);
    WaveBaseLockReplacementContext lock_context{
        .canonical = wave_lock_path(root),
        .displaced = original_wave_lock,
    };
    auto lock_rejected = store.create_worker_attempt_private_lease_root(
        chunk.chunk_id, 1,
        wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
            .after_initial_phase_validation = replace_base_lock_after_first_inventory,
            .context = &lock_context,
        });
    CHECK(lock_context.invoked);
    CHECK(lock_context.replaced);
    CHECK(lock_context.native_error == 0);
    CHECK(!lock_rejected);
    CHECK(lock_rejected.claim == nullptr);
    require_wave_status(lock_rejected.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                        "wave-lock identity replacement rejected before BaseLock mutation");
    CHECK(!entry_exists_no_follow(root / second_names->base_lock_leaf));
    auto before_without_wave_lock = before_wave_lock_replacement;
    auto after_without_wave_lock = capture_wave_root_snapshot(root);
    erase_wave_root_snapshot_leaf(before_without_wave_lock,
                                  wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF);
    erase_wave_root_snapshot_leaf(after_without_wave_lock,
                                  wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF);
    CHECK(after_without_wave_lock == before_without_wave_lock);
    require_strict_empty_base_lock(wave_lock_path(root),
                                   "replacement permanent wave lock remains strict");
    require_strict_empty_base_lock(original_wave_lock,
                                   "displaced permanent wave lock remains strict");

    std::error_code remove_error;
    CHECK(std::filesystem::remove(wave_lock_path(root), remove_error));
    CHECK(!remove_error);
    require_rename(original_wave_lock, wave_lock_path(root), "restore original attempt wave lock");
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "store revalidates after attempt wave-lock restoration");
    auto second_created = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 1);
    auto& second_claim = require_private_lease_root_claim_ready(
        second_created, "fresh BaseLock succeeds after wave-lock restoration");
    require_wave_status(second_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "post-restoration attempt claim revalidates");
    second_created.claim.reset();

    const auto post_root_target = root / post_root_names->base_lock_leaf;
    const auto post_root_displaced = temp.path() / "attempt-authority-post-lock-original-root";
    const auto before_post_root_replacement = capture_wave_root_snapshot(root);
    WaveRootReplacementContext post_root_context{
        .canonical = root,
        .displaced = post_root_displaced,
    };
    auto post_root_rejected = store.create_worker_attempt_private_lease_root(
        post_lock_chunk.chunk_id, 0,
        wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
            .after_target_lock_acquired = replace_wave_root_after_attempt_phase,
            .context = &post_root_context,
        });
    CHECK(post_root_context.invoked);
    CHECK(post_root_context.replaced);
    CHECK(post_root_context.native_error == 0);
    CHECK(!post_root_rejected);
    CHECK(post_root_rejected.claim == nullptr);
    require_wave_status(post_root_rejected.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                        "post-lock root replacement precedes durability and is preserved");
    CHECK(capture_wave_root_snapshot(root).size() == 1);
    CHECK(entry_exists_no_follow(post_root_displaced / post_root_names->base_lock_leaf));
    require_strict_empty_base_lock(post_root_displaced / post_root_names->base_lock_leaf,
                                   "post-lock root replacement preserves created BaseLock prefix");
    auto displaced_post_root_snapshot = capture_wave_root_snapshot(post_root_displaced);
    erase_wave_root_snapshot_leaf(displaced_post_root_snapshot, post_root_names->base_lock_leaf);
    CHECK(displaced_post_root_snapshot == before_post_root_replacement);

    const auto observed_post_root_replacement =
        temp.path() / "attempt-authority-post-lock-replacement-root";
    require_rename(root, observed_post_root_replacement, "preserve post-lock replacement root");
    require_rename(post_root_displaced, root, "restore post-lock original root");
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "store accepts preserved BaseLock prefix after root restoration");
    auto post_root_recovered =
        store.open_worker_attempt_private_lease_root(post_lock_chunk.chunk_id, 0);
    auto& post_root_claim = require_private_lease_root_claim_ready(
        post_root_recovered, "explicitly recover post-lock root-loss prefix");
    require_wave_status(post_root_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "post-lock root-loss recovery revalidates");
    CHECK(relation_base_lock_reports_busy(post_root_target));
    post_root_recovered.claim.reset();

    const auto before_open_root_replacement = capture_wave_root_snapshot(root);
    const auto open_root_displaced = temp.path() / "attempt-authority-open-original-root";
    WaveRootReplacementContext open_root_context{
        .canonical = root,
        .displaced = open_root_displaced,
    };
    auto open_root_rejected = store.open_worker_attempt_private_lease_root(
        post_lock_chunk.chunk_id, 0,
        wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
            .after_target_lock_acquired = replace_wave_root_after_attempt_phase,
            .context = &open_root_context,
        });
    CHECK(open_root_context.invoked);
    CHECK(open_root_context.replaced);
    CHECK(open_root_context.native_error == 0);
    CHECK(!open_root_rejected);
    CHECK(open_root_rejected.claim == nullptr);
    require_wave_status(open_root_rejected.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                        "open-existing root replacement is rejected without target mutation");
    CHECK(capture_wave_root_snapshot(root).size() == 1);
    CHECK(capture_wave_root_snapshot(open_root_displaced) == before_open_root_replacement);
    CHECK(!relation_base_lock_reports_busy(open_root_displaced / post_root_names->base_lock_leaf));

    const auto observed_open_root_replacement =
        temp.path() / "attempt-authority-open-replacement-root";
    require_rename(root, observed_open_root_replacement, "preserve open-existing replacement root");
    require_rename(open_root_displaced, root, "restore open-existing original root");
    CHECK(capture_wave_root_snapshot(root) == before_open_root_replacement);
    auto open_root_retry =
        store.open_worker_attempt_private_lease_root(post_lock_chunk.chunk_id, 0);
    auto& open_root_retry_claim = require_private_lease_root_claim_ready(
        open_root_retry, "retry recovery after open-existing root restoration");
    require_wave_status(open_root_retry_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "open-existing root-restoration retry revalidates");
    open_root_retry.claim.reset();

    const auto post_wave_lock_target = root / post_wave_lock_names->base_lock_leaf;
    const auto post_wave_lock_displaced =
        temp.path() / "attempt-authority-post-lock-original-wave-lock";
    const auto before_post_wave_lock_replacement = capture_wave_root_snapshot(root);
    WaveBaseLockReplacementContext post_wave_lock_context{
        .canonical = wave_lock_path(root),
        .displaced = post_wave_lock_displaced,
    };
    auto post_wave_lock_rejected = store.create_worker_attempt_private_lease_root(
        post_lock_chunk.chunk_id, 1,
        wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
            .after_target_lock_acquired = replace_base_lock_after_first_inventory,
            .context = &post_wave_lock_context,
        });
    CHECK(post_wave_lock_context.invoked);
    CHECK(post_wave_lock_context.replaced);
    CHECK(post_wave_lock_context.native_error == 0);
    CHECK(!post_wave_lock_rejected);
    CHECK(post_wave_lock_rejected.claim == nullptr);
    require_wave_status(post_wave_lock_rejected.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                        "post-lock wave-lock replacement precedes durability and is preserved");
    CHECK(entry_exists_no_follow(post_wave_lock_target));
    require_strict_empty_base_lock(
        post_wave_lock_target, "post-lock wave-lock replacement preserves created BaseLock prefix");
    auto before_post_wave_lock_without_lock = before_post_wave_lock_replacement;
    auto after_post_wave_lock_without_lock = capture_wave_root_snapshot(root);
    erase_wave_root_snapshot_leaf(before_post_wave_lock_without_lock,
                                  wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF);
    erase_wave_root_snapshot_leaf(after_post_wave_lock_without_lock,
                                  wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF);
    erase_wave_root_snapshot_leaf(after_post_wave_lock_without_lock,
                                  post_wave_lock_names->base_lock_leaf);
    CHECK(after_post_wave_lock_without_lock == before_post_wave_lock_without_lock);
    require_strict_empty_base_lock(wave_lock_path(root),
                                   "post-lock replacement wave lock remains strict");
    require_strict_empty_base_lock(post_wave_lock_displaced,
                                   "post-lock displaced wave lock remains strict");

    remove_error.clear();
    CHECK(std::filesystem::remove(wave_lock_path(root), remove_error));
    CHECK(!remove_error);
    require_rename(post_wave_lock_displaced, wave_lock_path(root),
                   "restore post-lock original wave lock");
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "store accepts preserved BaseLock prefix after wave-lock restoration");
    auto post_wave_lock_recovered =
        store.open_worker_attempt_private_lease_root(post_lock_chunk.chunk_id, 1);
    auto& post_wave_lock_claim = require_private_lease_root_claim_ready(
        post_wave_lock_recovered, "explicitly recover post-lock wave-lock-loss prefix");
    require_wave_status(post_wave_lock_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "post-lock wave-lock-loss recovery revalidates");
    CHECK(relation_base_lock_reports_busy(post_wave_lock_target));
    post_wave_lock_recovered.claim.reset();

    const auto before_open_wave_lock_replacement = capture_wave_root_snapshot(root);
    const auto open_wave_lock_displaced = temp.path() / "attempt-authority-open-original-wave-lock";
    WaveBaseLockReplacementContext open_wave_lock_context{
        .canonical = wave_lock_path(root),
        .displaced = open_wave_lock_displaced,
    };
    auto open_wave_lock_rejected = store.open_worker_attempt_private_lease_root(
        post_lock_chunk.chunk_id, 1,
        wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
            .after_target_lock_acquired = replace_base_lock_after_first_inventory,
            .context = &open_wave_lock_context,
        });
    CHECK(open_wave_lock_context.invoked);
    CHECK(open_wave_lock_context.replaced);
    CHECK(open_wave_lock_context.native_error == 0);
    CHECK(!open_wave_lock_rejected);
    CHECK(open_wave_lock_rejected.claim == nullptr);
    require_wave_status(open_wave_lock_rejected.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                        "open-existing wave-lock replacement is rejected without target mutation");
    auto before_open_without_wave_lock = before_open_wave_lock_replacement;
    auto after_open_without_wave_lock = capture_wave_root_snapshot(root);
    erase_wave_root_snapshot_leaf(before_open_without_wave_lock,
                                  wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF);
    erase_wave_root_snapshot_leaf(after_open_without_wave_lock,
                                  wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF);
    CHECK(after_open_without_wave_lock == before_open_without_wave_lock);
    require_strict_empty_base_lock(wave_lock_path(root),
                                   "open-existing replacement wave lock remains strict");
    require_strict_empty_base_lock(open_wave_lock_displaced,
                                   "open-existing displaced wave lock remains strict");
    CHECK(!relation_base_lock_reports_busy(post_wave_lock_target));

    remove_error.clear();
    CHECK(std::filesystem::remove(wave_lock_path(root), remove_error));
    CHECK(!remove_error);
    require_rename(open_wave_lock_displaced, wave_lock_path(root),
                   "restore open-existing original wave lock");
    CHECK(capture_wave_root_snapshot(root) == before_open_wave_lock_replacement);
    auto open_wave_lock_retry =
        store.open_worker_attempt_private_lease_root(post_lock_chunk.chunk_id, 1);
    auto& open_wave_lock_retry_claim = require_private_lease_root_claim_ready(
        open_wave_lock_retry, "retry recovery after open-existing wave-lock restoration");
    require_wave_status(open_wave_lock_retry_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "open-existing wave-lock-restoration retry revalidates");
}

void test_wave_store_attempt_base_lock_mixed_failure_precedence() {
    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "mixed-target-root";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create target-root precedence fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        const auto target = root / names->base_lock_leaf;
        MixedAttemptFailureContext context{
            .target =
                {
                    .canonical = target,
                    .displaced = temp.path() / "mixed-target-root-original-target",
                },
            .root =
                {
                    .canonical = root,
                    .displaced = temp.path() / "mixed-target-root-original-root",
                },
        };

        auto rejected = store.create_worker_attempt_private_lease_root(
            chunk.chunk_id, 0,
            wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
                .after_target_lock_acquired = replace_attempt_target_and_root_after_lock,
                .context = &context,
            });
        CHECK(!rejected);
        CHECK(rejected.claim == nullptr);
        require_wave_status(rejected.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "root authority outranks simultaneous target identity loss");
        CHECK(context.target.invoked);
        CHECK(context.target.replaced);
        CHECK(context.target.native_error == 0);
        CHECK(context.root.invoked);
        CHECK(context.root.replaced);
        CHECK(context.root.native_error == 0);
        CHECK(capture_wave_root_snapshot(root).size() == 1);
        CHECK(!entry_exists_no_follow(target));
        require_strict_empty_base_lock(context.root.displaced / names->base_lock_leaf,
                                       "target replacement remains in displaced original root");
        require_strict_empty_base_lock(context.target.displaced,
                                       "locked target remains outside displaced original root");
        CHECK(!relation_base_lock_reports_busy(context.target.displaced));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "mixed-target-wave-lock";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create target-wave-lock precedence fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        MixedAttemptFailureContext context{
            .target =
                {
                    .canonical = root / names->base_lock_leaf,
                    .displaced = temp.path() / "mixed-target-wave-lock-original-target",
                },
            .wave_lock =
                {
                    .canonical = wave_lock_path(root),
                    .displaced = temp.path() / "mixed-target-wave-lock-original-wave-lock",
                },
        };

        auto rejected = store.create_worker_attempt_private_lease_root(
            chunk.chunk_id, 0,
            wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
                .after_target_lock_acquired = replace_attempt_target_and_wave_lock_after_lock,
                .context = &context,
            });
        CHECK(!rejected);
        CHECK(rejected.claim == nullptr);
        require_wave_status(rejected.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                            "wave-lock authority outranks simultaneous target identity loss");
        CHECK(context.target.invoked);
        CHECK(context.target.replaced);
        CHECK(context.target.native_error == 0);
        CHECK(context.wave_lock.invoked);
        CHECK(context.wave_lock.replaced);
        CHECK(context.wave_lock.native_error == 0);
        require_strict_empty_base_lock(root / names->base_lock_leaf,
                                       "mixed target-wave replacement target remains preserved");
        require_strict_empty_base_lock(context.target.displaced,
                                       "mixed target-wave displaced target remains preserved");
        require_strict_empty_base_lock(wave_lock_path(root),
                                       "mixed target-wave replacement wave lock remains preserved");
        require_strict_empty_base_lock(context.wave_lock.displaced,
                                       "mixed target-wave displaced wave lock remains preserved");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "mixed-root-wave-lock";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create root-wave-lock precedence fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        MixedAttemptFailureContext context{
            .wave_lock =
                {
                    .canonical = wave_lock_path(root),
                    .displaced = temp.path() / "mixed-root-wave-original-wave-lock",
                },
            .root =
                {
                    .canonical = root,
                    .displaced = temp.path() / "mixed-root-wave-original-root",
                },
        };

        auto rejected = store.create_worker_attempt_private_lease_root(
            chunk.chunk_id, 0,
            wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
                .after_target_lock_acquired = replace_attempt_root_and_wave_lock_after_lock,
                .context = &context,
            });
        CHECK(!rejected);
        CHECK(rejected.claim == nullptr);
        require_wave_status(rejected.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "root authority outranks simultaneous wave-lock loss");
        CHECK(context.wave_lock.invoked);
        CHECK(context.wave_lock.replaced);
        CHECK(context.wave_lock.native_error == 0);
        CHECK(context.root.invoked);
        CHECK(context.root.replaced);
        CHECK(context.root.native_error == 0);
        CHECK(capture_wave_root_snapshot(root).size() == 1);
        require_strict_empty_base_lock(context.root.displaced / names->base_lock_leaf,
                                       "root-wave replacement preserves created target prefix");
        require_strict_empty_base_lock(context.root.displaced /
                                           wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF,
                                       "root-wave replacement preserves replacement wave lock");
        require_strict_empty_base_lock(context.wave_lock.displaced,
                                       "root-wave replacement preserves original wave lock");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "mixed-root-durability";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create root-durability precedence fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        MixedAttemptFailureContext context{
            .root =
                {
                    .canonical = root,
                    .displaced = temp.path() / "mixed-root-durability-original-root",
                },
            .sync =
                {
                    .target =
                        wave_detail::DistributedSievePrivateLeaseBaseLockSyncPoint::TargetInitial,
                },
        };

        auto rejected = store.create_worker_attempt_private_lease_root(
            chunk.chunk_id, 0,
            wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
                .fail_before_sync = replace_root_and_fail_before_base_lock_sync,
                .context = &context,
            });
        CHECK(!rejected);
        CHECK(rejected.claim == nullptr);
        require_wave_status(rejected.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "root loss outranks simultaneous durability interruption");
        CHECK(!rejected.diagnostic.failed_private_lease_base_lock_sync_point.has_value());
        CHECK(context.sync.observed.front());
        CHECK(context.root.invoked);
        CHECK(context.root.replaced);
        CHECK(context.root.native_error == 0);
        CHECK(capture_wave_root_snapshot(root).size() == 1);
        require_strict_empty_base_lock(context.root.displaced / names->base_lock_leaf,
                                       "root-durability failure preserves target prefix");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "mixed-target-foreign-durability";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store =
            require_wave_ready(created, "create target-foreign-durability precedence fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        MixedAttemptFailureContext context{
            .target =
                {
                    .canonical = root / names->base_lock_leaf,
                    .displaced = temp.path() / "mixed-target-foreign-durability-original-target",
                },
            .foreign =
                {
                    .foreign = root / "mixed.unexpected-control",
                },
            .sync =
                {
                    .target =
                        wave_detail::DistributedSievePrivateLeaseBaseLockSyncPoint::TargetInitial,
                },
        };

        auto rejected = store.create_worker_attempt_private_lease_root(
            chunk.chunk_id, 0,
            wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
                .fail_before_sync = replace_target_insert_foreign_and_fail_before_base_lock_sync,
                .context = &context,
            });
        CHECK(!rejected);
        CHECK(rejected.claim == nullptr);
        require_wave_status(
            rejected.diagnostic, wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
            "target and inventory drift outrank simultaneous durability interruption");
        CHECK(!rejected.diagnostic.failed_private_lease_base_lock_sync_point.has_value());
        CHECK(context.sync.observed.front());
        CHECK(context.target.invoked);
        CHECK(context.target.replaced);
        CHECK(context.target.native_error == 0);
        CHECK(context.foreign.invoked);
        CHECK(context.foreign.inserted);
        CHECK(context.foreign.native_error == 0);
        require_strict_empty_base_lock(
            root / names->base_lock_leaf,
            "target-foreign-durability replacement target remains preserved");
        require_strict_empty_base_lock(
            context.target.displaced,
            "target-foreign-durability displaced target remains preserved");
        require_strict_empty_base_lock(context.foreign.foreign,
                                       "target-foreign-durability foreign leaf remains preserved");
        CHECK(!relation_base_lock_reports_busy(context.target.displaced));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "mixed-returned-claim";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create returned-claim precedence fixture");
        const auto& first_chunk = store.manifest().chunks.at(0);
        const auto& second_chunk = store.manifest().chunks.at(1);
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            first_chunk.relative_artifact_stem, first_chunk.chunk_id, 0);
        CHECK(names.has_value());
        auto claimed = store.create_worker_attempt_private_lease_root(first_chunk.chunk_id, 0);
        auto& claim = require_private_lease_root_claim_ready(
            claimed, "create returned claim before mixed replacement");
        MixedAttemptFailureContext context{
            .target =
                {
                    .canonical = root / names->base_lock_leaf,
                    .displaced = temp.path() / "mixed-returned-claim-original-target",
                },
            .root =
                {
                    .canonical = root,
                    .displaced = temp.path() / "mixed-returned-claim-original-root",
                },
        };
        replace_attempt_target_and_root_after_lock(&context);
        CHECK(context.target.replaced);
        CHECK(context.root.replaced);

        auto competing = store.create_worker_attempt_private_lease_root(second_chunk.chunk_id, 0);
        CHECK(!competing);
        CHECK(competing.claim == nullptr);
        require_wave_status(competing.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::private_lease_root_busy,
                            "same-State root arbitration outranks non-owner authority observation");
        require_wave_status(claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "returned claim reports root before simultaneous target loss");
        require_wave_status(
            claim.revalidate_authority(),
            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
            "returned claim authority reports root before simultaneous target loss");
        CHECK(relation_base_lock_reports_busy(context.target.displaced));

        const auto observed_replacement = temp.path() / "mixed-returned-claim-replacement-root";
        require_rename(root, observed_replacement,
                       "preserve mixed returned-claim replacement root");
        require_rename(context.root.displaced, root, "restore mixed returned-claim original root");
        std::error_code remove_error;
        CHECK(std::filesystem::remove(root / names->base_lock_leaf, remove_error));
        CHECK(!remove_error);
        require_rename(context.target.displaced, root / names->base_lock_leaf,
                       "restore mixed returned-claim original target");
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "store revalidates after mixed returned-claim restoration");
        require_wave_status(
            claim.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
            "returned target capability remains sticky-invalid after authority repair");
        claimed.claim.reset();

        auto recovered = store.open_worker_attempt_private_lease_root(first_chunk.chunk_id, 0);
        auto& recovered_claim = require_private_lease_root_claim_ready(
            recovered, "new claim explicitly recovers mixed returned-claim target");
        require_wave_status(recovered_claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "new mixed-recovery claim revalidates");
    }
}

void test_wave_store_attempt_base_lock_authority_sandwich() {
    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "factory-root-sandwich";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create factory root-sandwich fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        WaveRootReplacementContext context{
            .canonical = root,
            .displaced = temp.path() / "factory-root-sandwich-original",
        };
        auto rejected = store.create_worker_attempt_private_lease_root(
            chunk.chunk_id, 0,
            wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
                .after_target_revalidation = replace_wave_root_after_attempt_phase,
                .context = &context,
            });
        CHECK(context.invoked);
        CHECK(context.replaced);
        CHECK(context.native_error == 0);
        CHECK(!rejected);
        CHECK(rejected.claim == nullptr);
        require_wave_status(rejected.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "factory rechecks root authority after target revalidation");
        CHECK(capture_wave_root_snapshot(root).size() == 1);
        require_strict_empty_base_lock(context.displaced / names->base_lock_leaf,
                                       "factory root-sandwich preserves created target prefix");
        CHECK(!relation_base_lock_reports_busy(context.displaced / names->base_lock_leaf));

        const auto replacement = temp.path() / "factory-root-sandwich-replacement";
        require_rename(root, replacement, "preserve factory root-sandwich replacement");
        require_rename(context.displaced, root, "restore factory root-sandwich original");
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "store accepts restored factory root-sandwich namespace");
        auto recovered = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        auto& recovered_claim = require_private_lease_root_claim_ready(
            recovered, "explicitly recover factory root-sandwich target");
        require_wave_status(recovered_claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "factory root-sandwich recovery revalidates");
        CHECK(relation_base_lock_reports_busy(root / names->base_lock_leaf));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "factory-wave-lock-sandwich";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create factory wave-lock-sandwich fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        WaveBaseLockReplacementContext context{
            .canonical = wave_lock_path(root),
            .displaced = temp.path() / "factory-wave-lock-sandwich-original",
        };
        auto rejected = store.create_worker_attempt_private_lease_root(
            chunk.chunk_id, 0,
            wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
                .after_target_revalidation = replace_base_lock_after_first_inventory,
                .context = &context,
            });
        CHECK(context.invoked);
        CHECK(context.replaced);
        CHECK(context.native_error == 0);
        CHECK(!rejected);
        CHECK(rejected.claim == nullptr);
        require_wave_status(rejected.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                            "factory rechecks wave-lock authority after target revalidation");
        require_strict_empty_base_lock(
            root / names->base_lock_leaf,
            "factory wave-lock-sandwich preserves created target prefix");
        CHECK(!relation_base_lock_reports_busy(root / names->base_lock_leaf));
        require_strict_empty_base_lock(wave_lock_path(root),
                                       "factory wave-lock-sandwich preserves replacement lock");
        require_strict_empty_base_lock(context.displaced,
                                       "factory wave-lock-sandwich preserves original lock");

        std::error_code remove_error;
        CHECK(std::filesystem::remove(wave_lock_path(root), remove_error));
        CHECK(!remove_error);
        require_rename(context.displaced, wave_lock_path(root),
                       "restore factory wave-lock-sandwich original");
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "store accepts restored factory wave-lock-sandwich namespace");
        auto recovered = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        auto& recovered_claim = require_private_lease_root_claim_ready(
            recovered, "explicitly recover factory wave-lock-sandwich target");
        require_wave_status(recovered_claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "factory wave-lock-sandwich recovery revalidates");
        CHECK(relation_base_lock_reports_busy(root / names->base_lock_leaf));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "claim-root-sandwich";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create claim root-sandwich fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        auto bound = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        auto& claim = require_private_lease_root_claim_ready(
            bound, "create bound claim for root-sandwich revalidation");
        WaveRootReplacementContext context{
            .canonical = root,
            .displaced = temp.path() / "claim-root-sandwich-original",
        };
        require_wave_status(
            claim.revalidate(wave_detail::DistributedSievePrivateLeaseRootClaimTestHooks{
                .after_first_authority_validation = replace_wave_root_after_attempt_phase,
                .context = &context,
            }),
            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
            "bound claim rechecks root authority after target revalidation");
        CHECK(context.invoked);
        CHECK(context.replaced);
        CHECK(context.native_error == 0);
        CHECK(relation_base_lock_reports_busy(context.displaced / names->base_lock_leaf));

        const auto replacement = temp.path() / "claim-root-sandwich-replacement";
        require_rename(root, replacement, "preserve claim root-sandwich replacement");
        require_rename(context.displaced, root, "restore claim root-sandwich original");
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "store accepts restored claim root-sandwich namespace");
        require_wave_status(claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "root-sandwich failure sticky-invalidates returned target");
        bound.claim.reset();
        auto recovered = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        auto& recovered_claim = require_private_lease_root_claim_ready(
            recovered, "explicitly recover claim root-sandwich target");
        require_wave_status(recovered_claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "claim root-sandwich recovery revalidates");
        CHECK(relation_base_lock_reports_busy(root / names->base_lock_leaf));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "claim-wave-lock-sandwich";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create claim wave-lock-sandwich fixture");
        const auto& chunk = store.manifest().chunks.front();
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        const auto target = root / names->base_lock_leaf;
        auto bound = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        auto& claim = require_private_lease_root_claim_ready(
            bound, "create bound claim for wave-lock-sandwich revalidation");
        WaveBaseLockReplacementContext context{
            .canonical = wave_lock_path(root),
            .displaced = temp.path() / "claim-wave-lock-sandwich-original",
        };
        require_wave_status(
            claim.revalidate(wave_detail::DistributedSievePrivateLeaseRootClaimTestHooks{
                .after_first_authority_validation = replace_base_lock_after_first_inventory,
                .context = &context,
            }),
            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
            "bound claim rechecks wave-lock authority after target revalidation");
        CHECK(context.invoked);
        CHECK(context.replaced);
        CHECK(context.native_error == 0);
        CHECK(relation_base_lock_reports_busy(target));

        std::error_code remove_error;
        CHECK(std::filesystem::remove(wave_lock_path(root), remove_error));
        CHECK(!remove_error);
        require_rename(context.displaced, wave_lock_path(root),
                       "restore claim wave-lock-sandwich original");
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "store accepts restored claim wave-lock-sandwich namespace");
        require_wave_status(claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "wave-lock-sandwich failure sticky-invalidates returned target");
        bound.claim.reset();
        auto recovered = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
        auto& recovered_claim = require_private_lease_root_claim_ready(
            recovered, "explicitly recover claim wave-lock-sandwich target");
        require_wave_status(recovered_claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "claim wave-lock-sandwich recovery revalidates");
        CHECK(relation_base_lock_reports_busy(target));
    }
}

void test_wave_store_attempt_base_lock_identity_split_is_sticky() {
    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "attempt-base-lock-identity-split";
    auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    auto& store = require_wave_ready(created, "create attempt BaseLock split fixture");
    const auto& chunk = store.manifest().chunks.front();
    const auto first_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        chunk.relative_artifact_stem, chunk.chunk_id, 0);
    const auto second_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        chunk.relative_artifact_stem, chunk.chunk_id, 1);
    CHECK(first_names.has_value());
    CHECK(second_names.has_value());
    const auto first_target = root / first_names->base_lock_leaf;
    const auto first_displaced = temp.path() / "attempt-base-lock-split-during-create";

    WaveBaseLockReplacementContext create_context{
        .canonical = first_target,
        .displaced = first_displaced,
    };
    auto split_during_create = store.create_worker_attempt_private_lease_root(
        chunk.chunk_id, 0,
        wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
            .after_target_lock_acquired = replace_base_lock_after_first_inventory,
            .context = &create_context,
        });
    CHECK(create_context.invoked);
    CHECK(create_context.replaced);
    CHECK(create_context.native_error == 0);
    CHECK(!split_during_create);
    CHECK(split_during_create.claim == nullptr);
    require_wave_status(split_during_create.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "same-name target split during acquire is rejected");
    CHECK(entry_exists_no_follow(first_target));
    CHECK(entry_exists_no_follow(first_displaced));
    require_strict_empty_base_lock(first_target, "replacement BaseLock remains preserved");
    require_strict_empty_base_lock(first_displaced, "locked original BaseLock remains preserved");
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "canonical replacement remains a valid closed inventory member");

    std::error_code remove_error;
    CHECK(std::filesystem::remove(first_target, remove_error));
    CHECK(!remove_error);
    require_rename(first_displaced, first_target, "restore split-during-create BaseLock");
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "restored original BaseLock closes inventory");
    auto first_recovered = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
    (void)require_private_lease_root_claim_ready(first_recovered,
                                                 "explicit recovery reopens restored BaseLock");
    first_recovered.claim.reset();

    const auto recovery_baseline = capture_wave_root_snapshot(root);
    const auto original_recovery_entry =
        require_wave_root_snapshot_leaf(recovery_baseline, first_names->base_lock_leaf);
    const auto recovery_displaced = temp.path() / "attempt-base-lock-split-during-recovery";
    WaveBaseLockReplacementContext recovery_context{
        .canonical = first_target,
        .displaced = recovery_displaced,
    };
    auto split_during_recovery = store.open_worker_attempt_private_lease_root(
        chunk.chunk_id, 0,
        wave_detail::DistributedSievePrivateLeaseBaseLockTestHooks{
            .after_target_lock_acquired = replace_base_lock_after_first_inventory,
            .context = &recovery_context,
        });
    CHECK(recovery_context.invoked);
    CHECK(recovery_context.replaced);
    CHECK(recovery_context.native_error == 0);
    CHECK(!split_during_recovery);
    CHECK(split_during_recovery.claim == nullptr);
    require_wave_status(split_during_recovery.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "same-name target split during recovery is rejected");
    CHECK(entry_exists_no_follow(first_target));
    CHECK(entry_exists_no_follow(recovery_displaced));
    require_strict_empty_base_lock(first_target, "recovery replacement BaseLock remains preserved");
    require_strict_empty_base_lock(recovery_displaced,
                                   "recovery displaced BaseLock remains preserved");
    const auto recovery_replacement_entry =
        capture_wave_root_entry_snapshot(first_target, first_names->base_lock_leaf);
    const auto displaced_recovery_entry =
        capture_wave_root_entry_snapshot(recovery_displaced, first_names->base_lock_leaf);
    CHECK(displaced_recovery_entry == original_recovery_entry);
    CHECK(recovery_replacement_entry.device != displaced_recovery_entry.device ||
          recovery_replacement_entry.inode != displaced_recovery_entry.inode);
    auto recovery_baseline_without_target = recovery_baseline;
    auto recovery_after_without_target = capture_wave_root_snapshot(root);
    erase_wave_root_snapshot_leaf(recovery_baseline_without_target, first_names->base_lock_leaf);
    erase_wave_root_snapshot_leaf(recovery_after_without_target, first_names->base_lock_leaf);
    CHECK(recovery_after_without_target == recovery_baseline_without_target);
    CHECK(!relation_base_lock_reports_busy(recovery_displaced));
    CHECK(!relation_base_lock_reports_busy(first_target));
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "recovery replacement remains valid closed inventory");

    const auto recovery_replacement_namespace = capture_wave_root_snapshot(root);
    auto recovery_create_fallback =
        store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
    CHECK(!recovery_create_fallback);
    CHECK(recovery_create_fallback.claim == nullptr);
    require_wave_status(recovery_create_fallback.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "failed recovery never falls back to fresh creation");
    CHECK(capture_wave_root_snapshot(root) == recovery_replacement_namespace);

    remove_error.clear();
    CHECK(std::filesystem::remove(first_target, remove_error));
    CHECK(!remove_error);
    require_rename(recovery_displaced, first_target, "restore split-during-recovery BaseLock");
    CHECK(capture_wave_root_snapshot(root) == recovery_baseline);
    auto recovery_retry = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 0);
    auto& recovery_retry_claim = require_private_lease_root_claim_ready(
        recovery_retry, "explicit retry reopens restored recovery BaseLock");
    require_wave_status(recovery_retry_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "retried recovery claim revalidates");
    require_wave_status(recovery_retry_claim.revalidate_authority(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "retried recovery authority revalidates");
    CHECK(relation_base_lock_reports_busy(first_target));
    recovery_retry.claim.reset();

    const auto second_target = root / second_names->base_lock_leaf;
    const auto second_displaced = temp.path() / "attempt-base-lock-split-after-acquire";
    auto second_created = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 1);
    auto& second_claim = require_private_lease_root_claim_ready(
        second_created, "create BaseLock before post-acquire identity split");
    WaveBaseLockReplacementContext live_context{
        .canonical = second_target,
        .displaced = second_displaced,
    };
    replace_base_lock_after_first_inventory(&live_context);
    CHECK(live_context.invoked);
    CHECK(live_context.replaced);
    CHECK(live_context.native_error == 0);
    CHECK(entry_exists_no_follow(second_target));
    CHECK(entry_exists_no_follow(second_displaced));
    require_wave_status(second_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "live claim rejects same-name target identity split");
    CHECK(relation_base_lock_reports_busy(second_displaced));

    remove_error.clear();
    CHECK(std::filesystem::remove(second_target, remove_error));
    CHECK(!remove_error);
    require_rename(second_displaced, second_target, "restore live split BaseLock name");
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "store independently accepts restored BaseLock identity");
    require_wave_status(second_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                        "observed target identity split keeps capability sticky-invalid");
    auto contended = store.claim_private_lease_root();
    CHECK(!contended);
    CHECK(contended.claim == nullptr);
    require_wave_status(contended.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::private_lease_root_busy,
                        "sticky-invalid transaction retains root slot until destruction");

    second_created.claim.reset();
    auto second_recovered = store.open_worker_attempt_private_lease_root(chunk.chunk_id, 1);
    auto& recovered_claim = require_private_lease_root_claim_ready(
        second_recovered, "new transaction reopens restored sticky-invalid target");
    require_wave_status(recovered_claim.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "new target capability is valid after explicit recovery");
}

void test_wave_store_private_lease_root_claim_process_and_namespace_binding() {
    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "private-lease-root-fork";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create private-lease-root fork fixture");
        auto claimed = store.claim_private_lease_root();
        auto& claim =
            require_private_lease_root_claim_ready(claimed, "claim private-lease-root before fork");

        int ready_pipe[2]{-1, -1};
        int release_pipe[2]{-1, -1};
        CHECK(::pipe(ready_pipe) == 0);
        CHECK(::pipe(release_pipe) == 0);
        const pid_t child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            (void)::close(ready_pipe[0]);
            (void)::close(release_pipe[1]);
            const auto inherited_authority = claim.revalidate_authority();
            const auto inherited_claim = claim.revalidate();
            auto inherited_store_claim = store.claim_private_lease_root();
            const bool rejected =
                !claim.owned_by_current_process() &&
                inherited_authority.status ==
                    wave_detail::DistributedSieveWaveStoreStatus::invalid_request &&
                inherited_claim.status ==
                    wave_detail::DistributedSieveWaveStoreStatus::invalid_request &&
                !inherited_store_claim && inherited_store_claim.claim == nullptr &&
                inherited_store_claim.diagnostic.status ==
                    wave_detail::DistributedSieveWaveStoreStatus::invalid_request;
            claimed.claim.reset();
            const bool signalled = write_pipe_byte(ready_pipe[1], rejected ? 'r' : 'f');
            char release = '\0';
            const bool released = read_pipe_byte(release_pipe[0], release);
            ::_exit(rejected && signalled && released && release == 'x' ? 0 : 84);
        }

        (void)::close(ready_pipe[1]);
        (void)::close(release_pipe[0]);
        char ready = '\0';
        const bool received = read_pipe_byte(ready_pipe[0], ready);
        require_wave_status(claim.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "parent claim remains valid while child is alive");
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "parent store remains valid while child is alive");
        const bool released = write_pipe_byte(release_pipe[1], 'x');
        (void)::close(ready_pipe[0]);
        (void)::close(release_pipe[1]);
        int child_status = 0;
        const bool waited = wait_for_child(child, child_status);

        CHECK(received);
        CHECK(ready == 'r');
        CHECK(released);
        CHECK(waited);
        CHECK(WIFEXITED(child_status));
        CHECK(WEXITSTATUS(child_status) == 0);
        require_wave_status(claim.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "parent claim remains valid after child exits");
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "parent store remains valid after child exits");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "private-lease-root-replacement";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create private-lease-root replacement fixture");
        auto claimed = store.claim_private_lease_root();
        auto& claim =
            require_private_lease_root_claim_ready(claimed, "claim before wave-root replacement");

        const auto original_root = temp.path() / "private-lease-root-original";
        require_rename(root, original_root, "move claimed wave root");
        std::error_code error;
        CHECK(std::filesystem::create_directory(root, error));
        CHECK(!error);
        require_chmod(root, 0700, "chmod replacement claimed wave root");
        const auto sentinel = root / "replacement-sentinel";
        write_foreign_leaf(sentinel);
        const auto sentinel_before = read_file_bytes(sentinel);

        require_wave_status(claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "claim rejects replaced wave root");
        auto contended = store.claim_private_lease_root();
        CHECK(!contended);
        CHECK(contended.claim == nullptr);
        require_wave_status(contended.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::private_lease_root_busy,
                            "held claim hides transitional wave-root namespace");
        claimed.claim.reset();
        auto rejected_after_release = store.claim_private_lease_root();
        CHECK(!rejected_after_release);
        CHECK(rejected_after_release.claim == nullptr);
        require_wave_status(rejected_after_release.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "released claim exposes replaced wave root");
        CHECK(read_file_bytes(sentinel) == sentinel_before);
        CHECK(!entry_exists_no_follow(wave_lock_path(root)));
        CHECK(!entry_exists_no_follow(wave_manifest_path(root)));
        CHECK(!entry_exists_no_follow(wave_manifest_pending_path(root)));

        const auto displaced_replacement = temp.path() / "private-lease-root-replacement-observed";
        require_rename(root, displaced_replacement, "preserve replacement wave root");
        require_rename(original_root, root, "restore claimed wave root");
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "store revalidates after wave-root restoration");
        CHECK(read_file_bytes(displaced_replacement / sentinel.filename()) == sentinel_before);

        auto reacquired = store.claim_private_lease_root();
        auto& restored_claim = require_private_lease_root_claim_ready(
            reacquired, "claim succeeds after wave-root restoration");
        require_wave_status(restored_claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "new claim revalidates after wave-root restoration");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "private-lease-lock-replacement";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create private-lease lock-replacement fixture");
        auto claimed = store.claim_private_lease_root();
        auto& claim =
            require_private_lease_root_claim_ready(claimed, "claim before wave-lock replacement");

        const auto original_lock = temp.path() / "private-lease-original-lock";
        require_rename(wave_lock_path(root), original_lock, "move claimed wave lock");
        write_foreign_leaf(wave_lock_path(root));
        const auto replacement_before = read_file_bytes(wave_lock_path(root));

        require_wave_status(claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                            "claim rejects replaced wave lock");
        auto contended = store.claim_private_lease_root();
        CHECK(!contended);
        CHECK(contended.claim == nullptr);
        require_wave_status(contended.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::private_lease_root_busy,
                            "held claim hides transitional wave-lock namespace");
        claimed.claim.reset();
        auto rejected_after_release = store.claim_private_lease_root();
        CHECK(!rejected_after_release);
        CHECK(rejected_after_release.claim == nullptr);
        require_wave_status(rejected_after_release.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                            "released claim exposes replaced wave lock");
        CHECK(read_file_bytes(wave_lock_path(root)) == replacement_before);

        const auto displaced_replacement = temp.path() / "private-lease-lock-replacement-observed";
        require_rename(wave_lock_path(root), displaced_replacement,
                       "preserve replacement wave lock");
        require_rename(original_lock, wave_lock_path(root), "restore claimed wave lock");
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "store revalidates after wave-lock restoration");
        CHECK(read_file_bytes(displaced_replacement) == replacement_before);

        auto reacquired = store.claim_private_lease_root();
        auto& restored_claim = require_private_lease_root_claim_ready(
            reacquired, "claim succeeds after wave-lock restoration");
        require_wave_status(restored_claim.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "new claim revalidates after wave-lock restoration");
    }
}

#else

void test_wave_store_platform_fail_closed() {
    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "unsupported";
    auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    CHECK(!created);
    CHECK(created.store == nullptr);
    require_wave_status(created.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::platform_unsupported,
                        "unsupported platform rejects wave-store creation");
    CHECK(!entry_exists_no_follow(root));
}

#endif

using TestFunction = void (*)();

void run_core_suite() {
    const std::array<std::pair<std::string_view, TestFunction>, 13> tests = {{
        {"closed names and kinds", test_closed_names_and_record_kinds},
        {"record round trips and self digests", test_all_record_round_trips_and_self_digests},
        {"exact framing and tamper", test_exact_framing_and_wire_tamper},
        {"manifest canonical order and limits", test_manifest_canonical_order_and_limits},
        {"worker attempt naming contract", test_worker_attempt_naming_contract},
        {"worker completion reason closure", test_worker_completion_reason_closure},
        {"terminal failure normalization", test_terminal_failure_reason_normalization},
        {"execution-policy field drift", test_execution_policy_closed_inventory_and_field_drift},
        {"work-identity field drift", test_work_identity_field_drift_and_canonical_integers},
        {"manifest/work identity binding", test_manifest_work_identity_binding},
        {"deterministic seed drift", test_deterministic_seed_domain_and_input_drift},
        {"predecessor and dependency closure", test_predecessor_and_dependency_closure},
        {"empty chunk projection and merge", test_empty_chunk_projection_and_merge},
    }};

    std::cout << "===== Distributed Sieve Resume Core Tests =====\n";
    for (const auto& [name, function] : tests) {
        function();
        std::cout << "  " << name << ": PASS\n";
    }
    std::cout << "===== Distributed Sieve Resume Core Tests PASSED =====\n";
}

void run_wave_store_suite() {
#if !defined(_WIN32)
    const std::array<std::pair<std::string_view, TestFunction>, 33> tests = {{
        {"create, open, revalidate, and exact manifest",
         test_wave_store_create_open_revalidate_and_exact_manifest},
        {"store-owned draft fields", test_wave_store_rejects_non_draft_store_owned_fields},
        {"zero open digest", test_wave_store_rejects_zero_open_digest_without_observation},
        {"canonical no-follow paths",
         test_wave_store_rejects_noncanonical_and_intermediate_symlink_paths},
        {"all durable fault prefixes", test_wave_store_all_durable_prefixes_recover_exactly},
        {"post-hook namespace revalidation", test_wave_store_revalidates_after_root_and_lock_hooks},
        {"fault-hook fork authority", test_wave_store_fault_hooks_cannot_fork_store_authority},
        {"unknown leaf and wrong digest",
         test_wave_store_unknown_leaf_and_wrong_digest_are_preserved},
        {"root and lock replacement", test_wave_store_root_and_lock_replacement},
        {"mode, symlink, and hardlink", test_wave_store_mode_symlink_and_hardlink_rejections},
        {"deterministic busy", test_wave_store_deterministic_busy_with_fork_and_pipes},
        {"inherited lock", test_wave_store_inherited_lock_is_process_bound_and_close_only},
        {"typed claim lifetime and exclusion",
         test_wave_store_private_lease_root_claim_traits_and_lifetime},
        {"manifest-bound BaseLocks and claim inventory split",
         test_wave_store_manifest_bound_base_locks_and_claim_inventory_split},
        {"fresh private-lease reservation protocol",
         test_wave_store_fresh_private_lease_reservation_protocol},
        {"fresh private-lease reservation sync failures",
         test_wave_store_fresh_private_lease_reservation_sync_failures},
        {"fresh private-lease successor replacements",
         test_wave_store_fresh_private_lease_successor_replacements},
        {"private-lease receipt replacement",
         test_wave_store_private_lease_receipt_rejects_replacement},
        {"private-lease validation-hook authority sandwich",
         test_wave_store_private_lease_validation_hook_authority_sandwich},
        {"all private-lease reservation prefixes",
         test_wave_store_classifies_all_private_lease_reservation_prefixes},
        {"dual private-lease marker states",
         test_wave_store_rejects_dual_private_lease_marker_states},
        {"same-byte private-lease marker replacement",
         test_wave_store_private_lease_witness_rejects_same_bytes_replacement},
        {"pending manifest rejects valid reservation",
         test_wave_store_manifest_pending_rejects_valid_reservation_before_repair},
        {"attempt BaseLock create, recover, and phase contract",
         test_wave_store_attempt_base_lock_create_recover_and_phase_contract},
        {"attempt BaseLock durability prefixes",
         test_wave_store_attempt_base_lock_durability_prefixes},
        {"attempt BaseLock State-scope concurrency",
         test_wave_store_attempt_base_lock_state_scope_concurrency},
        {"attempt BaseLock bound-claim exact inventory",
         test_wave_store_attempt_base_lock_bound_claim_exact_inventory},
        {"attempt BaseLock fork and close-only lifetime",
         test_wave_store_attempt_base_lock_fork_binding_and_close_only_lifetime},
        {"attempt BaseLock pre-mutation authority replacement",
         test_wave_store_attempt_base_lock_pre_mutation_authority_replacement},
        {"attempt BaseLock mixed failure precedence",
         test_wave_store_attempt_base_lock_mixed_failure_precedence},
        {"attempt BaseLock authority sandwich",
         test_wave_store_attempt_base_lock_authority_sandwich},
        {"attempt BaseLock sticky identity split",
         test_wave_store_attempt_base_lock_identity_split_is_sticky},
        {"typed claim process and namespace binding",
         test_wave_store_private_lease_root_claim_process_and_namespace_binding},
    }};
#else
    const std::array<std::pair<std::string_view, TestFunction>, 2> tests = {{
        {"zero open digest", test_wave_store_rejects_zero_open_digest_without_observation},
        {"unsupported platform fails closed", test_wave_store_platform_fail_closed},
    }};
#endif

    std::cout << "===== Distributed Sieve Wave Store Tests =====\n";
    for (const auto& [name, function] : tests) {
        function();
        std::cout << "  " << name << ": PASS\n";
    }
    std::cout << "===== Distributed Sieve Wave Store Tests PASSED =====\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            run_core_suite();
            run_wave_store_suite();
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--suite" &&
            std::string_view(argv[2]) == "core") {
            run_core_suite();
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--suite" &&
            std::string_view(argv[2]) == "wave-store") {
            run_wave_store_suite();
            return 0;
        }

        std::cerr << "usage: " << argv[0] << " [--suite core|wave-store]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Distributed sieve resume test failure: " << error.what() << '\n';
        return 1;
    }
}
