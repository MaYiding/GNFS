#include <iostream>

#if defined(_WIN32)

#include "distributed_sieve_wave_store_internal.hpp"
#include "distributed_sieve_worker_cleanup_authority_internal.hpp"

#include <gnfs/relation/ooc_relation_format.hpp>
#include <gnfs/sieve/distributed_sieve_protocol.hpp>
#include <gnfs/util/process.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

namespace relation = gnfs::relation;
namespace sieve = gnfs::sieve;
namespace wave = gnfs::sieve::distributed_sieve_resume_detail;
namespace cleanup = gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail;
namespace merge_tail = gnfs::sieve::distributed_sieve_merge_writer_authority_detail;
using Digest = gnfs::util::Sha256Digest;

using TailReleaseFunction = cleanup::DistributedSieveCommittedTailCleanupTransitionV1 (
    merge_tail::DistributedSieveCommittedTailAdmissionV1::*)() && noexcept;

volatile TailReleaseFunction tail_release_link_probe =
    &merge_tail::DistributedSieveCommittedTailAdmissionV1::release_for_worker_cleanup_cold_open_v1;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            throw std::runtime_error(std::string("CHECK failed: " #condition " at ") + __FILE__ +  \
                                     ":" + std::to_string(__LINE__));                              \
        }                                                                                          \
    } while (false)

[[nodiscard]] Digest digest_with_seed(std::uint8_t seed) noexcept {
    Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        digest.bytes[index] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + index + 1U));
    }
    return digest;
}

[[nodiscard]] sieve::WaveIdV1 wave_id_with_seed(std::uint8_t seed) noexcept {
    sieve::WaveIdV1 wave_id;
    for (std::size_t index = 0; index < wave_id.bytes.size(); ++index) {
        wave_id.bytes[index] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + index + 1U));
    }
    return wave_id;
}

[[nodiscard]] sieve::WaveManifestV1 valid_manifest_draft() {
    return {
        .wave_id = wave_id_with_seed(1),
        .execution_contract_version = 1,
        .executable_sha256 = digest_with_seed(2),
        .work_sha256 = digest_with_seed(3),
        .wave_root_identity = {},
        .permanent_lock_identity = {},
        .lock_semantics_version = 0,
        .effective_sq_begin = 2,
        .effective_sq_end = 5,
        .worker_count = 2,
        .chunks =
            {
                sieve::ChunkPlanV1{0, 2, 3, "chunk_0"},
                sieve::ChunkPlanV1{1, 3, 5, "chunk_1"},
            },
        .sq_cap_per_worker = 10,
        .relation_cap_per_worker = 100,
        .max_worker_attempts = 2,
        .max_merge_build_attempts = 2,
        .max_consumption_attempts = 2,
        .canonical_naming_version = sieve::DISTRIBUTED_SIEVE_CANONICAL_NAMING_VERSION_V1,
        .retry_policy_version = 1,
        .durable_start_consumes_ordinal = true,
        .ooc_format_version = relation::OOCRelationStoreFormat::FORMAT_VERSION_V3,
        .relation_serialization_version = 1,
        .handoff_version = sieve::DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1,
        .receipt_version = 1,
        .digest_version = 1,
        .merge_policy_version = 1,
        .self_digest = {},
    };
}

[[nodiscard]] bool stop_store_hook(wave::DistributedSieveWaveStoreFaultPoint,
                                   void* context) noexcept {
    ++*static_cast<unsigned*>(context);
    return true;
}

void cleanup_root_hook(void* context) noexcept {
    ++*static_cast<unsigned*>(context);
}

void check_unsupported(const wave::DistributedSieveWaveStoreOpenResult& result) {
    CHECK(result.diagnostic.status == wave::DistributedSieveWaveStoreStatus::platform_unsupported);
    CHECK(result.diagnostic.native_error ==
          std::make_error_code(std::errc::operation_not_supported));
    CHECK(result.store == nullptr);
    CHECK(!result.prepared_admission.has_value());
    CHECK(!result.committed_tail_admission.has_value());
    CHECK(!static_cast<bool>(result));
}

void check_invalid(const wave::DistributedSieveWaveStoreOpenResult& result) {
    CHECK(result.diagnostic.status == wave::DistributedSieveWaveStoreStatus::invalid_request);
    CHECK(result.diagnostic.native_error == std::make_error_code(std::errc::invalid_argument));
    CHECK(result.store == nullptr);
    CHECK(!result.prepared_admission.has_value());
    CHECK(!result.committed_tail_admission.has_value());
    CHECK(!static_cast<bool>(result));
}

[[nodiscard]] bool path_exists_without_error(const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    CHECK(!error);
    return exists;
}

void test_platform_neutral_naming_linkage() {
    const TailReleaseFunction tail_release = tail_release_link_probe;
    CHECK(tail_release != nullptr);

    const auto worker = wave::distributed_sieve_worker_attempt_names_v1("chunk_3", 3, 4);
    CHECK(worker.has_value());
    CHECK(worker->relative_lease_stem == "chunk_3_attempt_04");
    CHECK(worker->private_directory_leaf == "chunk_3_attempt_04.gnfs-sink-lease");
    CHECK(worker->canonical_record_leaf == ".gnfs-wave-v1.attempt-c03-a04");
    CHECK(worker->pending_record_leaf == ".gnfs-wave-v1.attempt-c03-a04.pending");
    CHECK(!wave::distributed_sieve_worker_attempt_names_v1("chunk_3", 64, 4).has_value());

    const auto parsed_worker =
        wave::parse_distributed_sieve_worker_attempt_leaf_v1(worker->pending_record_leaf);
    CHECK(parsed_worker.has_value());
    CHECK(parsed_worker->chunk_id == 3);
    CHECK(parsed_worker->attempt_ordinal == 4);
    CHECK(parsed_worker->pending);
    CHECK(!wave::parse_distributed_sieve_worker_attempt_leaf_v1(
               ".gnfs-wave-v1.attempt-c3-a04.pending")
               .has_value());

    const auto merge = wave::distributed_sieve_merge_generation_names_v1(5);
    CHECK(merge.has_value());
    CHECK(merge->relative_lease_stem == "gnfs-wave-v1-merge-a05");
    CHECK(merge->canonical_record_leaf == ".gnfs-wave-v1.merge-start-a05");
    const auto parsed_merge =
        wave::parse_distributed_sieve_merge_started_leaf_v1(merge->pending_record_leaf);
    CHECK(parsed_merge.has_value());
    CHECK(parsed_merge->merge_attempt_ordinal == 5);
    CHECK(parsed_merge->pending);

    const auto terminal = wave::distributed_sieve_chunk_terminal_failure_names_v1(6);
    CHECK(terminal.has_value());
    CHECK(terminal->canonical_record_leaf == ".gnfs-wave-v1.chunk-terminal-failure-c06");
    const auto parsed_terminal =
        wave::parse_distributed_sieve_chunk_terminal_failure_leaf_v1(terminal->pending_record_leaf);
    CHECK(parsed_terminal.has_value());
    CHECK(parsed_terminal->chunk_id == 6);
    CHECK(parsed_terminal->pending);

    const auto commit = wave::distributed_sieve_wave_merge_commit_names_v1();
    CHECK(commit.canonical_record_leaf == ".gnfs-wave-v1.merge-commit");
    CHECK(commit.pending_record_leaf == ".gnfs-wave-v1.merge-commit.pending");
    const auto parsed_commit =
        wave::parse_distributed_sieve_wave_merge_commit_leaf_v1(commit.pending_record_leaf);
    CHECK(parsed_commit.has_value());
    CHECK(parsed_commit->pending);

    const auto cleanup = wave::distributed_sieve_worker_cleanup_record_names_v1(7);
    CHECK(cleanup.has_value());
    CHECK(cleanup->authorization_canonical_record_leaf ==
          ".gnfs-wave-v1.cleanup-authorized-worker-c07");
    CHECK(cleanup->completion_pending_record_leaf ==
          ".gnfs-wave-v1.cleanup-completed-worker-c07.pending");
    const auto parsed_worker_cleanup = wave::parse_distributed_sieve_cleanup_record_leaf_v1(
        cleanup->completion_pending_record_leaf);
    CHECK(parsed_worker_cleanup.has_value());
    CHECK(parsed_worker_cleanup->kind ==
          wave::DistributedSieveCleanupRecordCoordinateKindV1::completed_worker);
    CHECK(parsed_worker_cleanup->manifest_order_ordinal == 7);
    CHECK(parsed_worker_cleanup->pending);
    const auto parsed_merged_cleanup = wave::parse_distributed_sieve_cleanup_record_leaf_v1(
        ".gnfs-wave-v1.cleanup-authorized-merged.pending");
    CHECK(parsed_merged_cleanup.has_value());
    CHECK(parsed_merged_cleanup->kind ==
          wave::DistributedSieveCleanupRecordCoordinateKindV1::authorized_merged);
    CHECK(!parsed_merged_cleanup->manifest_order_ordinal.has_value());
    CHECK(parsed_merged_cleanup->pending);
}

void test_wave_store_stub() {
    std::error_code error;
    const auto temporary_root = std::filesystem::temp_directory_path(error);
    CHECK(!error);
    const auto absent_root =
        temporary_root /
        ("gnfs-wave-store-windows-stub-must-not-exist-" +
         std::to_string(static_cast<unsigned long long>(gnfs::util::process_id())));
    CHECK(!path_exists_without_error(absent_root));

    unsigned store_hook_calls = 0;
    const wave::DistributedSieveWaveStoreTestHooks store_hooks{
        .stop_after = stop_store_hook,
        .merge_prepared_resume = {},
        .worker_handoff_resume = {},
        .wave_merge_commit = {},
        .context = &store_hook_calls,
    };

    auto created =
        wave::DistributedSieveWaveStore::create(absent_root, valid_manifest_draft(), store_hooks);
    check_unsupported(created);
    CHECK(store_hook_calls == 0);
    CHECK(!path_exists_without_error(absent_root));

    const Digest manifest_digest = digest_with_seed(4);
    auto opened = wave::DistributedSieveWaveStore::open(absent_root, manifest_digest, store_hooks);
    check_unsupported(opened);
    CHECK(store_hook_calls == 0);
    CHECK(!path_exists_without_error(absent_root));

    auto invalid_path = wave::DistributedSieveWaveStore::create(
        std::filesystem::path{"relative-wave-root"}, valid_manifest_draft(), store_hooks);
    check_invalid(invalid_path);
    CHECK(store_hook_calls == 0);

    auto invalid_draft_value = valid_manifest_draft();
    invalid_draft_value.wave_root_identity.object = 1;
    auto invalid_draft = wave::DistributedSieveWaveStore::create(
        absent_root, std::move(invalid_draft_value), store_hooks);
    check_invalid(invalid_draft);
    CHECK(store_hook_calls == 0);

    auto invalid_digest = wave::DistributedSieveWaveStore::open(absent_root, Digest{}, store_hooks);
    check_invalid(invalid_digest);
    CHECK(store_hook_calls == 0);
    CHECK(!path_exists_without_error(absent_root));
}

void test_cleanup_stubs() {
    std::error_code error;
    const auto temporary_root = std::filesystem::temp_directory_path(error);
    CHECK(!error);
    const auto absent_root =
        temporary_root /
        ("gnfs-wave-cleanup-windows-stub-must-not-exist-" +
         std::to_string(static_cast<unsigned long long>(gnfs::util::process_id())));
    CHECK(!path_exists_without_error(absent_root));

    constexpr std::string_view cleanup_leaf = ".gnfs-wave-v1.cleanup-authorized-worker-c00";
    const auto loaded =
        wave::load_distributed_sieve_worker_cleanup_record_leaf_v1(-1, cleanup_leaf, 0);
    CHECK(!loaded.witness.has_value());
    CHECK(loaded.diagnostic.status == wave::DistributedSieveWaveStoreStatus::platform_unsupported);
    CHECK(loaded.diagnostic.native_error ==
          std::make_error_code(std::errc::operation_not_supported));
    CHECK(!static_cast<bool>(loaded));

    const auto malformed = wave::load_distributed_sieve_worker_cleanup_record_leaf_v1(
        -1, ".gnfs-wave-v1.cleanup-authorized-worker-c0", 0);
    CHECK(!malformed.witness.has_value());
    CHECK(malformed.diagnostic.status == wave::DistributedSieveWaveStoreStatus::namespace_conflict);
    CHECK(malformed.diagnostic.native_error == std::make_error_code(std::errc::protocol_error));
    CHECK(!static_cast<bool>(malformed));

    unsigned cleanup_hook_calls = 0;
    const wave::DistributedSieveWorkerCleanupRootOpenTestHooksV1 cleanup_hooks{
        .after_first_observation = cleanup_root_hook,
        .context = &cleanup_hook_calls,
    };
    const Digest manifest_digest = digest_with_seed(5);
    auto opened =
        wave::open_worker_cleanup_root_v1(absent_root, manifest_digest, cleanup_hooks, nullptr);
    CHECK(!opened.admission.has_value());
    CHECK(opened.diagnostic.status == wave::DistributedSieveWaveStoreStatus::platform_unsupported);
    CHECK(opened.diagnostic.native_error ==
          std::make_error_code(std::errc::operation_not_supported));
    CHECK(!static_cast<bool>(opened));
    CHECK(cleanup_hook_calls == 0);
    CHECK(!path_exists_without_error(absent_root));

    auto invalid = wave::open_worker_cleanup_root_v1(std::filesystem::path{"relative-cleanup-root"},
                                                     manifest_digest, cleanup_hooks, nullptr);
    CHECK(!invalid.admission.has_value());
    CHECK(invalid.diagnostic.status == wave::DistributedSieveWaveStoreStatus::invalid_request);
    CHECK(invalid.diagnostic.native_error == std::make_error_code(std::errc::invalid_argument));
    CHECK(!static_cast<bool>(invalid));
    CHECK(cleanup_hook_calls == 0);
    CHECK(!path_exists_without_error(absent_root));
}

} // namespace

#endif

int main() {
#if !defined(_WIN32)
    std::cout << "SKIP: distributed-sieve WaveStore Windows stub contract requires WIN32\n";
    return 77;
#else
    try {
        test_platform_neutral_naming_linkage();
        test_wave_store_stub();
        test_cleanup_stubs();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "distributed-sieve WaveStore Windows stub tests passed\n";
    return 0;
#endif
}
