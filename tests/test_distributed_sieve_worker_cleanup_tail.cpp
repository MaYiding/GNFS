#include "distributed_sieve_merge_commit_authority_internal.hpp"
#include "distributed_sieve_wave_store_internal.hpp"
#include "distributed_sieve_worker_cleanup_authority_internal.hpp"
#include "ooc_private_handoff_cleanup_authorization_internal.hpp"

#include "support/child_process.hpp"

#if defined(__APPLE__)
#include "support/distributed_sieve_wave_merge_commit_fixture.hpp"
#endif

#include <gnfs/core/relation.hpp>
#include <gnfs/relation/ooc_cleanup_transaction.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/sieve/distributed_sieve_protocol.hpp>
#include <gnfs/util/durable_immutable_record.hpp>
#include <gnfs/util/sha256.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#if defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace cleanup_authority = gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail;
namespace commit_authority = gnfs::sieve::distributed_sieve_merge_commit_authority_detail;
namespace durable_record = gnfs::util::durable_immutable_record;
namespace relation = gnfs::relation;
namespace sieve = gnfs::sieve;
namespace wave = gnfs::sieve::distributed_sieve_resume_detail;

using CommittedTail = cleanup_authority::DistributedSieveCommittedTailAdmissionV1;
using CleanupAdmission = cleanup_authority::DistributedSieveWorkerCleanupRootAdmissionV1;
using CleanupResult = cleanup_authority::DistributedSieveWorkerCleanupTailResultV1;
using CleanupReceiptMinted = cleanup_authority::DistributedSieveWorkerCleanupReceiptMintedV1;
using CleanupReceiptMintResult =
    cleanup_authority::DistributedSieveWorkerCleanupReceiptMintResultV1;
using CleanupConversionCapsule =
    cleanup_authority::DistributedSieveWorkerCleanupIntentConversionCapsuleV1;
using CleanupConversionPrepareResult =
    cleanup_authority::DistributedSieveWorkerCleanupIntentConversionPrepareResultV1;
using CleanupConversionExecuteResult =
    cleanup_authority::DistributedSieveWorkerCleanupIntentConversionExecuteResultV1;
using CleanupConversionPrepareFunction =
    decltype(&cleanup_authority::prepare_distributed_sieve_worker_cleanup_intent_conversion_v1);
using CleanupConversionExecuteFunction =
    decltype(&cleanup_authority::execute_distributed_sieve_worker_cleanup_intent_conversion_v1);
using CleanupCompletionReadyCapsule =
    cleanup_authority::DistributedSieveWorkerCleanupCompletionReadyCapsuleV1;
using CleanupCompletionPreparationResult =
    cleanup_authority::DistributedSieveWorkerCleanupCompletionPreparationResultV1;
using CleanupCompletionDriveFunction =
    decltype(&cleanup_authority::trusted_test::
                 drive_distributed_sieve_worker_cleanup_to_completion_ready_v1_with_root);
using CleanupCompletionPublishedContinuation =
    cleanup_authority::DistributedSieveWorkerCleanupCompletionPublishedContinuationV1;
using CleanupCompletionPublicationResult =
    cleanup_authority::DistributedSieveWorkerCleanupCompletionPublicationResultV1;
using CleanupCompletionPublishFunction =
    decltype(&cleanup_authority::publish_distributed_sieve_worker_cleanup_completion_v1);
using CleanupCompletionReconcileFunction =
    decltype(&cleanup_authority::reconcile_distributed_sieve_worker_cleanup_completion_v1);
using CleanupCompletionPublicationFault =
    cleanup_authority::trusted_test::DistributedSieveWorkerCleanupCompletionPublicationFaultPointV1;
using CleanupCompletionPublicationHooks =
    cleanup_authority::trusted_test::DistributedSieveWorkerCleanupCompletionPublicationTestHooksV1;

static_assert(std::is_final_v<CleanupAdmission>);
static_assert(!std::is_default_constructible_v<CleanupAdmission>);
static_assert(!std::is_copy_constructible_v<CleanupAdmission>);
static_assert(std::is_nothrow_move_constructible_v<CleanupAdmission>);
static_assert(std::is_final_v<CommittedTail>);
static_assert(!std::is_copy_constructible_v<CommittedTail>);
static_assert(!std::is_default_constructible_v<CleanupReceiptMinted>);
static_assert(!std::is_copy_constructible_v<CleanupReceiptMinted>);
static_assert(std::is_nothrow_move_constructible_v<CleanupReceiptMinted>);
static_assert(!std::is_copy_constructible_v<CleanupReceiptMintResult>);
static_assert(std::is_nothrow_move_constructible_v<CleanupReceiptMintResult>);
static_assert(std::is_final_v<CleanupConversionCapsule>);
static_assert(!std::is_default_constructible_v<CleanupConversionCapsule>);
static_assert(!std::is_copy_constructible_v<CleanupConversionCapsule>);
static_assert(std::is_nothrow_move_constructible_v<CleanupConversionCapsule>);
static_assert(!std::is_copy_constructible_v<CleanupConversionPrepareResult>);
static_assert(std::is_nothrow_move_constructible_v<CleanupConversionPrepareResult>);
static_assert(!std::is_copy_constructible_v<CleanupConversionExecuteResult>);
static_assert(std::is_nothrow_move_constructible_v<CleanupConversionExecuteResult>);
static_assert(!std::is_invocable_v<CleanupConversionPrepareFunction, CleanupAdmission&>);
static_assert(std::is_nothrow_invocable_r_v<CleanupConversionPrepareResult,
                                            CleanupConversionPrepareFunction, CleanupAdmission&&>);
static_assert(!std::is_invocable_v<CleanupConversionExecuteFunction, CleanupConversionCapsule&>);
static_assert(
    std::is_nothrow_invocable_r_v<CleanupConversionExecuteResult, CleanupConversionExecuteFunction,
                                  CleanupConversionCapsule&&>);
static_assert(std::is_final_v<CleanupCompletionReadyCapsule>);
static_assert(!std::is_default_constructible_v<CleanupCompletionReadyCapsule>);
static_assert(!std::is_copy_constructible_v<CleanupCompletionReadyCapsule>);
static_assert(std::is_nothrow_move_constructible_v<CleanupCompletionReadyCapsule>);
static_assert(!std::is_copy_constructible_v<CleanupCompletionPreparationResult>);
static_assert(std::is_nothrow_move_constructible_v<CleanupCompletionPreparationResult>);
static_assert(!std::is_invocable_v<CleanupCompletionDriveFunction, CleanupAdmission&>);
static_assert(std::is_nothrow_invocable_r_v<CleanupCompletionPreparationResult,
                                            CleanupCompletionDriveFunction, CleanupAdmission&&>);
static_assert(std::is_final_v<CleanupCompletionPublishedContinuation>);
static_assert(!std::is_default_constructible_v<CleanupCompletionPublishedContinuation>);
static_assert(!std::is_copy_constructible_v<CleanupCompletionPublishedContinuation>);
static_assert(std::is_nothrow_move_constructible_v<CleanupCompletionPublishedContinuation>);
static_assert(!std::is_copy_constructible_v<CleanupCompletionPublicationResult>);
static_assert(std::is_nothrow_move_constructible_v<CleanupCompletionPublicationResult>);
static_assert(
    !std::is_invocable_v<CleanupCompletionPublishFunction, CleanupCompletionReadyCapsule&>);
static_assert(std::is_nothrow_invocable_r_v<CleanupCompletionPublicationResult,
                                            CleanupCompletionPublishFunction,
                                            CleanupCompletionReadyCapsule&&>);
static_assert(!std::is_invocable_v<CleanupCompletionReconcileFunction, CleanupAdmission&>);
static_assert(
    std::is_nothrow_invocable_r_v<CleanupCompletionPublicationResult,
                                  CleanupCompletionReconcileFunction, CleanupAdmission&&>);
static_assert(
    noexcept(cleanup_authority::mint_distributed_sieve_worker_cleanup_authorization_receipt_v1(
        std::declval<CleanupAdmission&>())));
static_assert(
    noexcept(cleanup_authority::prepare_distributed_sieve_worker_cleanup_intent_conversion_v1(
        std::declval<CleanupAdmission&&>())));
static_assert(
    noexcept(cleanup_authority::execute_distributed_sieve_worker_cleanup_intent_conversion_v1(
        std::declval<CleanupConversionCapsule&&>())));
static_assert(
    noexcept(cleanup_authority::consume_distributed_sieve_committed_tail_for_worker_cleanup_v1(
        std::declval<CommittedTail&&>())));
static_assert(
    std::is_same_v<
        decltype(cleanup_authority::consume_distributed_sieve_committed_tail_for_worker_cleanup_v1(
            std::declval<CommittedTail&&>())),
        CleanupResult>);

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
    throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view expression, int line) {
    if (!condition) {
        fail(expression, line);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

#if defined(__APPLE__)

namespace fixture = gnfs::test::distributed_sieve_wave_merge_commit_fixture;

using Digest = gnfs::util::Sha256Digest;
using Relation = gnfs::core::Relation;
using WorkerSnapshot = fixture::WorkerArtifactSnapshot;

inline constexpr int BASE_LOCKS_FREE_EXIT = 71;
inline constexpr int BASE_LOCK_HELD_EXIT = 72;
inline constexpr int WAVE_LOCK_BUSY_EXIT = 73;
inline constexpr int FORK_REJECTED_EXIT = 74;
inline constexpr int COMPLETION_READY_FORK_REJECTED_EXIT = 75;
inline constexpr int COMPLETION_PUBLICATION_FORK_REJECTED_EXIT = 76;

std::string test_executable;

[[nodiscard]] std::string digest_hex(const Digest& digest) {
    const auto encoded = gnfs::util::encode_sha256_hex(digest);
    return std::string(encoded.data(), encoded.size());
}

void write_all(int descriptor, std::span<const std::byte> bytes) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto result = ::write(descriptor, bytes.data() + written, bytes.size() - written);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            throw std::system_error(result < 0 ? errno : EIO, std::generic_category(),
                                    "write cleanup-tail replacement");
        }
        written += static_cast<std::size_t>(result);
    }
}

void sync_directory(const std::filesystem::path& directory) {
    int descriptor = -1;
    do {
        descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "open cleanup-tail directory for sync");
    }
    const int sync_result = ::fsync(descriptor);
    const int sync_error = errno;
    (void)::close(descriptor);
    if (sync_result != 0) {
        throw std::system_error(sync_error, std::generic_category(), "sync cleanup-tail directory");
    }
}

void replace_file_with_same_bytes(const std::filesystem::path& canonical,
                                  const std::filesystem::path& saved,
                                  std::span<const std::byte> bytes) {
    if (::rename(canonical.c_str(), saved.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(), "save cleanup-tail anchored leaf");
    }
    int descriptor = -1;
    do {
        descriptor =
            ::open(canonical.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "create cleanup-tail replacement leaf");
    }
    try {
        if (::fchmod(descriptor, 0600) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "chmod cleanup-tail replacement leaf");
        }
        write_all(descriptor, bytes);
        if (::fsync(descriptor) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "sync cleanup-tail replacement leaf");
        }
    } catch (...) {
        (void)::close(descriptor);
        throw;
    }
    if (::close(descriptor) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "close cleanup-tail replacement leaf");
    }
    sync_directory(canonical.parent_path());
}

void write_immutable_test_leaf(const std::filesystem::path& path,
                               std::span<const std::byte> bytes) {
    int descriptor = -1;
    do {
        descriptor =
            ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "create cleanup-tail immutable leaf");
    }
    try {
        if (::fchmod(descriptor, 0600) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "chmod cleanup-tail immutable leaf");
        }
        write_all(descriptor, bytes);
        if (::fsync(descriptor) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "sync cleanup-tail immutable leaf");
        }
    } catch (...) {
        (void)::close(descriptor);
        throw;
    }
    if (::close(descriptor) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "close cleanup-tail immutable leaf");
    }
    sync_directory(path.parent_path());
}

void restore_replaced_file(const std::filesystem::path& canonical,
                           const std::filesystem::path& saved) {
    std::error_code remove_error;
    if (!std::filesystem::remove(canonical, remove_error) || remove_error) {
        throw std::filesystem::filesystem_error("remove cleanup-tail replacement leaf", canonical,
                                                remove_error);
    }
    if (::rename(saved.c_str(), canonical.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "restore cleanup-tail anchored leaf");
    }
    sync_directory(canonical.parent_path());
}

enum class GapReplacementKind : std::uint8_t {
    root,
    wave_lock,
    manifest,
    merge_commit,
};

[[nodiscard]] std::string_view gap_replacement_label(GapReplacementKind kind) noexcept {
    switch (kind) {
    case GapReplacementKind::root:
        return "root";
    case GapReplacementKind::wave_lock:
        return "wave-lock";
    case GapReplacementKind::manifest:
        return "manifest";
    case GapReplacementKind::merge_commit:
        return "merge-commit";
    }
    return "unknown";
}

[[nodiscard]] wave::DistributedSieveWaveStoreStatus
expected_gap_replacement_status(GapReplacementKind kind) noexcept {
    switch (kind) {
    case GapReplacementKind::root:
    case GapReplacementKind::wave_lock:
        return wave::DistributedSieveWaveStoreStatus::manifest_conflict;
    case GapReplacementKind::manifest:
    case GapReplacementKind::merge_commit:
        return wave::DistributedSieveWaveStoreStatus::namespace_conflict;
    }
    return wave::DistributedSieveWaveStoreStatus::unexpected_failure;
}

[[nodiscard]] std::filesystem::path gap_replacement_target(const std::filesystem::path& root,
                                                           GapReplacementKind kind) {
    switch (kind) {
    case GapReplacementKind::root:
        return root;
    case GapReplacementKind::wave_lock:
        return root / std::string(wave::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF);
    case GapReplacementKind::manifest:
        return root / std::string(wave::DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF);
    case GapReplacementKind::merge_commit:
        return root / std::string(wave::DISTRIBUTED_SIEVE_WAVE_MERGE_COMMIT_RECORD_LEAF);
    }
    fail("known generation-gap replacement kind", __LINE__);
}

struct GapReplacementContext final {
    GapReplacementKind kind = GapReplacementKind::merge_commit;
    std::filesystem::path root;
    std::filesystem::path canonical;
    std::filesystem::path saved;
    std::vector<std::byte> bytes;
    sieve::NativeIdentityV1 original_identity;
    sieve::NativeIdentityV1 replacement_identity;
    bool invoked = false;
    bool replacement_present = false;
    std::exception_ptr failure;
};

void replace_generation_gap_anchor(void* opaque) noexcept {
    auto& context = *static_cast<GapReplacementContext*>(opaque);
    context.invoked = true;
    try {
        if (context.kind == GapReplacementKind::root) {
            if (::rename(context.canonical.c_str(), context.saved.c_str()) != 0) {
                throw std::system_error(errno, std::generic_category(), "save cleanup-tail root");
            }
            std::filesystem::copy(context.saved, context.canonical,
                                  std::filesystem::copy_options::recursive);
            if (::chmod(context.canonical.c_str(), 0700) != 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "chmod cleanup-tail replacement root");
            }
            sync_directory(context.canonical);
            sync_directory(context.canonical.parent_path());
        } else {
            replace_file_with_same_bytes(context.canonical, context.saved, context.bytes);
        }
        context.replacement_identity = fixture::native_identity(context.canonical);
        context.replacement_present = true;
    } catch (...) {
        context.failure = std::current_exception();
    }
}

struct InjectedPreSpendFailureContext final {
    bool invoked = false;
};

[[nodiscard]] bool inject_pre_spend_failure(void* opaque) noexcept {
    auto& context = *static_cast<InjectedPreSpendFailureContext*>(opaque);
    context.invoked = true;
    return true;
}

[[nodiscard]] bool replace_before_final_tail_revalidation(void* opaque) noexcept {
    replace_generation_gap_anchor(opaque);
    return false;
}

void restore_generation_gap_anchor(GapReplacementContext& context) {
    if (context.kind == GapReplacementKind::root) {
        std::error_code remove_error;
        (void)std::filesystem::remove_all(context.canonical, remove_error);
        if (remove_error) {
            throw std::filesystem::filesystem_error("remove cleanup-tail replacement root",
                                                    context.canonical, remove_error);
        }
        if (::rename(context.saved.c_str(), context.canonical.c_str()) != 0) {
            throw std::system_error(errno, std::generic_category(), "restore cleanup-tail root");
        }
        sync_directory(context.canonical.parent_path());
    } else {
        std::error_code remove_error;
        if (!std::filesystem::remove(context.canonical, remove_error) || remove_error) {
            throw std::filesystem::filesystem_error("remove cleanup-tail replacement leaf",
                                                    context.canonical, remove_error);
        }
        if (::rename(context.saved.c_str(), context.canonical.c_str()) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "restore cleanup-tail anchored leaf");
        }
        sync_directory(context.root);
    }
    context.replacement_present = false;
}

[[nodiscard]] std::string
wave_diagnostic_detail(const wave::DistributedSieveWaveStoreDiagnostic& diagnostic) {
    std::string detail(wave::distributed_sieve_wave_store_status_name(diagnostic.status));
    if (diagnostic.native_error) {
        detail.append(": ");
        detail.append(diagnostic.native_error.message());
    }
    return detail;
}

[[nodiscard]] std::string cleanup_diagnostic_detail(
    const cleanup_authority::DistributedSieveWorkerCleanupTailDiagnosticV1& diagnostic) {
    std::string detail(
        cleanup_authority::distributed_sieve_worker_cleanup_tail_status_name(diagnostic.status));
    detail.append(" phase=");
    detail.append(std::to_string(static_cast<unsigned>(diagnostic.phase)));
    if (diagnostic.native_error) {
        detail.append(": ");
        detail.append(diagnostic.native_error.message());
    }
    if (diagnostic.wave_store.status != wave::DistributedSieveWaveStoreStatus::ready) {
        detail.append(" wave=");
        detail.append(wave_diagnostic_detail(diagnostic.wave_store));
    }
    return detail;
}

[[nodiscard]] CommittedTail commit_fresh(fixture::PreparedWaveFixture& prepared) {
    auto publication = prepared.prepare_fresh();
    CHECK(publication.admission.has_value());
    auto committed = commit_authority::consume_distributed_sieve_merge_prepared_v1(
        std::move(*publication.admission));
    if (!committed || !committed.committed_tail.has_value()) {
        fail("publish WaveMergeCommit before cleanup-tail transition", __LINE__);
    }
    CHECK(!committed.retryable_prepared.has_value());
    return std::move(*committed.committed_tail);
}

struct WorkerCleanupSource final {
    sieve::WorkerHandoffV1 handoff;
    sieve::NativeIdentityV1 base_lock_identity;
    sieve::NativeIdentityV1 owned_marker_identity;
    Digest private_handoff_digest;
    sieve::NativeFileExtentV1 private_handoff_record;
};

[[nodiscard]] WorkerCleanupSource worker_cleanup_source(const WorkerSnapshot& snapshot) {
    const auto decoded_private =
        relation::decode_ooc_private_handoff_record(snapshot.leaves[3].bytes);
    CHECK(decoded_private);
    const auto decoded_payload =
        sieve::decode_distributed_sieve_record(decoded_private.value->opaque_payload);
    CHECK(decoded_payload);
    const auto* handoff = std::get_if<sieve::WorkerHandoffV1>(&*decoded_payload.value);
    CHECK(handoff != nullptr);
    return {
        .handoff = *handoff,
        .base_lock_identity = snapshot.leaves[0].identity,
        .owned_marker_identity = snapshot.leaves[1].identity,
        .private_handoff_digest = decoded_private.value->self_digest,
        .private_handoff_record =
            {
                .identity = snapshot.leaves[3].identity,
                .extent = static_cast<std::uint64_t>(snapshot.leaves[3].bytes.size()),
            },
    };
}

[[nodiscard]] sieve::ArtifactCleanupAuthorizedV1 make_worker_cleanup_authorization(
    const sieve::WaveManifestV1& manifest, const sieve::WaveMergeCommitV1& commit,
    const WorkerCleanupSource& source, std::uint32_t manifest_order_ordinal) {
    return fixture::seal_value(sieve::ArtifactCleanupAuthorizedV1{
        .authorizer = sieve::CleanupAuthorizerKindV1::merge_commit_worker,
        .manifest_digest = manifest.self_digest,
        .authorizer_record_digest = commit.self_digest,
        .artifact_kind = sieve::CleanupArtifactKindV1::worker,
        .manifest_order_ordinal = manifest_order_ordinal,
        .lease = source.handoff.lease,
        .base_lock_identity = source.base_lock_identity,
        .owned_marker_identity = source.owned_marker_identity,
        .handoff_digest = source.handoff.self_digest,
        .private_handoff_digest = source.private_handoff_digest,
        .private_handoff_record = source.private_handoff_record,
        .artifact = source.handoff.artifact,
    });
}

[[nodiscard]] sieve::ArtifactCleanupCompletedV1
make_worker_cleanup_completion(const sieve::ArtifactCleanupAuthorizedV1& authorization) {
    return fixture::seal_value(sieve::ArtifactCleanupCompletedV1{
        .authorization_digest = authorization.self_digest,
        .cleanup_intent_identity = std::nullopt,
        .parent_directory_durability_confirmed = true,
        .expected_namespace_absent = true,
    });
}

void remove_worker_private_namespace(const std::filesystem::path& root,
                                     const WorkerSnapshot& snapshot) {
    for (std::size_t index = 2; index < snapshot.leaves.size(); ++index) {
        std::error_code error;
        if (!std::filesystem::remove(snapshot.leaves[index].path, error) || error) {
            throw std::filesystem::filesystem_error("remove cleanup-tail worker private leaf",
                                                    snapshot.leaves[index].path, error);
        }
    }
    const auto private_directory = snapshot.leaves[2].path.parent_path();
    std::error_code directory_error;
    if (!std::filesystem::remove(private_directory, directory_error) || directory_error) {
        throw std::filesystem::filesystem_error("remove cleanup-tail worker private directory",
                                                private_directory, directory_error);
    }
    std::error_code owned_error;
    if (!std::filesystem::remove(snapshot.leaves[1].path, owned_error) || owned_error) {
        throw std::filesystem::filesystem_error("remove cleanup-tail worker OWNED marker",
                                                snapshot.leaves[1].path, owned_error);
    }
    sync_directory(root);
}

[[nodiscard]] std::string receipt_mint_diagnostic_detail(
    const cleanup_authority::DistributedSieveWorkerCleanupReceiptMintDiagnosticV1& diagnostic) {
    std::string detail(cleanup_authority::distributed_sieve_worker_cleanup_receipt_mint_status_name(
        diagnostic.status));
    detail.append(" phase=");
    detail.append(std::to_string(static_cast<unsigned>(diagnostic.phase)));
    if (diagnostic.native_error) {
        detail.append(": ");
        detail.append(diagnostic.native_error.message());
    }
    if (diagnostic.wave_store.status != wave::DistributedSieveWaveStoreStatus::ready) {
        detail.append(" wave=");
        detail.append(wave_diagnostic_detail(diagnostic.wave_store));
    }
    return detail;
}

class CanonicalWorkerCleanupRoot final {
public:
    explicit CanonicalWorkerCleanupRoot(std::string_view label, std::uint32_t active_ordinal = 0)
        : prepared_(label), active_ordinal_(active_ordinal) {
        CHECK(active_ordinal_ < worker_snapshots_.size());
        {
            auto tail = commit_fresh(prepared_);
            for (std::uint32_t ordinal = 0; ordinal <= active_ordinal_; ++ordinal) {
                worker_snapshots_[ordinal].emplace(prepared_.worker_snapshot(ordinal));
                const auto source = worker_cleanup_source(*worker_snapshots_[ordinal]);
                authorizations_[ordinal].emplace(make_worker_cleanup_authorization(
                    prepared_.manifest(), tail.record(), source, ordinal));
                const auto names = wave::distributed_sieve_worker_cleanup_record_names_v1(ordinal);
                CHECK(names.has_value());
                names_[ordinal].emplace(*names);
                authorization_bytes_[ordinal] = fixture::encode_record(
                    sieve::DistributedSieveProtocolRecordV1{*authorizations_[ordinal]});
                completions_[ordinal].emplace(
                    make_worker_cleanup_completion(*authorizations_[ordinal]));
                completion_bytes_[ordinal] = fixture::encode_record(
                    sieve::DistributedSieveProtocolRecordV1{*completions_[ordinal]});
            }
        }

        for (std::uint32_t ordinal = 0; ordinal < active_ordinal_; ++ordinal) {
            fixture::publish_canonical_record(prepared_.root(),
                                              names_[ordinal]->authorization_pending_record_leaf,
                                              names_[ordinal]->authorization_canonical_record_leaf,
                                              authorization_bytes_[ordinal]);
            remove_worker_private_namespace(prepared_.root(), *worker_snapshots_[ordinal]);
            fixture::publish_canonical_record(
                prepared_.root(), names_[ordinal]->completion_pending_record_leaf,
                names_[ordinal]->completion_canonical_record_leaf, completion_bytes_[ordinal]);
        }
        fixture::publish_canonical_record(
            prepared_.root(), names_[active_ordinal_]->authorization_pending_record_leaf,
            names_[active_ordinal_]->authorization_canonical_record_leaf,
            authorization_bytes_[active_ordinal_]);
        open();
    }

    CanonicalWorkerCleanupRoot(const CanonicalWorkerCleanupRoot&) = delete;
    CanonicalWorkerCleanupRoot& operator=(const CanonicalWorkerCleanupRoot&) = delete;

    [[nodiscard]] fixture::PreparedWaveFixture& prepared() noexcept {
        return prepared_;
    }

    [[nodiscard]] CleanupAdmission& admission() {
        CHECK(admission_.has_value());
        return *admission_;
    }

    [[nodiscard]] CleanupAdmission take_admission() {
        CHECK(admission_.has_value());
        CleanupAdmission admission(std::move(*admission_));
        admission_.reset();
        CHECK(admission.valid());
        return admission;
    }

    [[nodiscard]] std::uint32_t active_ordinal() const noexcept {
        return active_ordinal_;
    }

    [[nodiscard]] const std::vector<std::byte>& authorization_bytes() const noexcept {
        return authorization_bytes_[active_ordinal_];
    }

    [[nodiscard]] std::filesystem::path authorization_path() const {
        return authorization_path(active_ordinal_);
    }

    [[nodiscard]] std::filesystem::path authorization_path(std::uint32_t ordinal) const {
        CHECK(ordinal <= active_ordinal_);
        return prepared_.root() / names_[ordinal]->authorization_canonical_record_leaf;
    }

    [[nodiscard]] std::filesystem::path authorization_pending_path() const {
        return prepared_.root() / names_[active_ordinal_]->authorization_pending_record_leaf;
    }

    [[nodiscard]] const std::vector<std::byte>& completion_bytes(std::uint32_t ordinal) const {
        CHECK(ordinal <= active_ordinal_);
        return completion_bytes_[ordinal];
    }

    [[nodiscard]] std::filesystem::path completion_path(std::uint32_t ordinal) const {
        CHECK(ordinal <= active_ordinal_);
        return prepared_.root() / names_[ordinal]->completion_canonical_record_leaf;
    }

    [[nodiscard]] std::filesystem::path completion_pending_path(std::uint32_t ordinal) const {
        CHECK(ordinal <= active_ordinal_);
        return prepared_.root() / names_[ordinal]->completion_pending_record_leaf;
    }

    [[nodiscard]] std::filesystem::path worker_base() const {
        const auto& chunk = prepared_.manifest().chunks[active_ordinal_];
        const auto attempt_names = wave::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(attempt_names.has_value());
        return prepared_.root() / attempt_names->private_directory_leaf / "corpus";
    }

    void release_and_cold_reopen() {
        admission_.reset();
        open();
    }

private:
    void open() {
        auto opened =
            wave::open_worker_cleanup_root_v1(prepared_.root(), prepared_.manifest_digest());
        if (!opened || !opened.admission.has_value()) {
            fail("open canonical worker-cleanup root", __LINE__,
                 wave_diagnostic_detail(opened.diagnostic));
        }
        CHECK(opened.admission->valid());
        const auto& prefix = opened.admission->cleanup_prefix();
        CHECK(prefix.completed_worker_count == active_ordinal_);
        CHECK(prefix.frontier_manifest_order_ordinal ==
              std::optional<std::uint32_t>{active_ordinal_});
        CHECK(prefix.active_manifest_order_ordinal ==
              std::optional<std::uint32_t>{active_ordinal_});
        CHECK(prefix.coordinates.size() == static_cast<std::size_t>(active_ordinal_) + 1U);
        CHECK(prefix.coordinates.back().state ==
              wave::DistributedSieveWorkerCleanupPrefixStateV1::authorization_canonical_only);
        admission_.emplace(std::move(*opened.admission));
    }

    fixture::PreparedWaveFixture prepared_;
    std::uint32_t active_ordinal_ = 0;
    std::array<std::optional<WorkerSnapshot>, 2> worker_snapshots_;
    std::array<std::optional<sieve::ArtifactCleanupAuthorizedV1>, 2> authorizations_;
    std::array<std::optional<sieve::ArtifactCleanupCompletedV1>, 2> completions_;
    std::array<std::optional<wave::DistributedSieveWorkerCleanupRecordNamesV1>, 2> names_;
    std::array<std::vector<std::byte>, 2> authorization_bytes_;
    std::array<std::vector<std::byte>, 2> completion_bytes_;
    std::optional<CleanupAdmission> admission_;
};

[[nodiscard]] CleanupReceiptMintResult mint_cleanup_receipt(CanonicalWorkerCleanupRoot& root) {
    auto minted = cleanup_authority::mint_distributed_sieve_worker_cleanup_authorization_receipt_v1(
        root.admission());
    if (!minted || !minted.minted.has_value()) {
        fail("mint worker-cleanup authorization receipt", __LINE__,
             receipt_mint_diagnostic_detail(minted.diagnostic));
    }
    CHECK(minted.minted->manifest_order_ordinal == root.active_ordinal());
    return minted;
}

[[nodiscard]] CleanupReceiptMintResult mint_cleanup_receipt(CleanupAdmission& admission,
                                                            std::uint32_t expected_ordinal) {
    auto minted = cleanup_authority::mint_distributed_sieve_worker_cleanup_authorization_receipt_v1(
        admission);
    if (!minted || !minted.minted.has_value()) {
        fail("mint worker-cleanup authorization receipt from continuation", __LINE__,
             receipt_mint_diagnostic_detail(minted.diagnostic));
    }
    CHECK(minted.minted->manifest_order_ordinal == expected_ordinal);
    return minted;
}

[[nodiscard]] CleanupConversionPrepareResult
prepare_cleanup_conversion_capsule(CanonicalWorkerCleanupRoot& root) {
    auto prepared =
        cleanup_authority::prepare_distributed_sieve_worker_cleanup_intent_conversion_v1(
            root.take_admission());
    CHECK(!prepared.retryable_root.has_value());
    CHECK(prepared.capsule.has_value());
    CHECK(prepared.diagnostic.phase ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionPreparePhaseV1::complete);
    CHECK(prepared.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1::ready);
    CHECK(prepared.capsule->valid());
    CHECK(prepared.capsule->manifest_order_ordinal() == root.active_ordinal());
    return prepared;
}

struct CleanupConversionStopContext final {
    relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentPublicationFaultPointV2 target =
        relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentPublicationFaultPointV2::
            ReaderViewsClosed;
    bool invoked = false;
};

[[nodiscard]] bool stop_cleanup_conversion_after(
    relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentPublicationFaultPointV2 point,
    void* opaque) noexcept {
    auto& context = *static_cast<CleanupConversionStopContext*>(opaque);
    if (point != context.target) {
        return false;
    }
    context.invoked = true;
    return true;
}

[[nodiscard]] wave::DistributedSieveWorkerCleanupRecordNamesV1
active_cleanup_record_names(const CanonicalWorkerCleanupRoot& root) {
    const auto names =
        wave::distributed_sieve_worker_cleanup_record_names_v1(root.active_ordinal());
    CHECK(names.has_value());
    return *names;
}

void require_active_cleanup_completion_absent(CanonicalWorkerCleanupRoot& root) {
    const auto names = active_cleanup_record_names(root);
    CHECK(!std::filesystem::exists(root.prepared().root() / names.completion_pending_record_leaf));
    CHECK(
        !std::filesystem::exists(root.prepared().root() / names.completion_canonical_record_leaf));
}

void require_worker_cleanup_namespace_absent(const std::filesystem::path& base) {
    const auto paths = relation::OOCCleanupTransaction::paths_for(base);
    CHECK(std::filesystem::exists(paths.lock_path));
    const std::array absent_paths{
        paths.private_directory,
        paths.index_path,
        paths.data_path,
        paths.intent_path,
        paths.intent_pending_path,
        paths.staged_path,
        paths.staged_pending_path,
        paths.lease_reserved_path,
        paths.lease_reserved_pending_path,
        paths.lease_owned_path,
        paths.lease_owned_pending_path,
        paths.private_handoff_path,
        paths.private_handoff_pending_path,
        paths.private_handoff_rollback_path,
        paths.quarantine_index_path,
        paths.quarantine_data_path,
    };
    for (const auto& path : absent_paths) {
        CHECK(!std::filesystem::exists(path));
    }
}

[[nodiscard]] CleanupAdmission publish_canonical_cleanup_intent(CanonicalWorkerCleanupRoot& root) {
    auto prepared = prepare_cleanup_conversion_capsule(root);
    auto published =
        cleanup_authority::execute_distributed_sieve_worker_cleanup_intent_conversion_v1(
            std::move(*prepared.capsule));
    CHECK(published);
    CHECK(!published.retryable_capsule.has_value());
    CHECK(published.root_continuation.has_value());
    CHECK(published.publication.intent_published());
    CHECK(published.root_continuation->valid());
    return std::move(*published.root_continuation);
}

void materialize_pending_cleanup_intent_and_cold_reopen(CanonicalWorkerCleanupRoot& root) {
    const auto paths = relation::OOCCleanupTransaction::paths_for(root.worker_base());
    auto prepared = prepare_cleanup_conversion_capsule(root);
    CleanupConversionStopContext context{
        .target = relation::ooc_cleanup_detail::
            OOCPrivateHandoffCleanupIntentPublicationFaultPointV2::IntentPendingDurable,
    };
    auto interrupted = cleanup_authority::trusted_test::
        execute_distributed_sieve_worker_cleanup_intent_conversion_v1_with_hooks(
            std::move(*prepared.capsule),
            relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentPublicationTestHooksV2{
                .stop_after = stop_cleanup_conversion_after,
                .context = &context,
            });
    CHECK(context.invoked);
    CHECK(interrupted);
    CHECK(interrupted.retryable_capsule.has_value());
    CHECK(!interrupted.root_continuation.has_value());
    CHECK(interrupted.publication.capabilities_retained());
    CHECK(!std::filesystem::exists(paths.intent_path));
    CHECK(std::filesystem::exists(paths.intent_pending_path));

    interrupted.retryable_capsule.reset();
    root.release_and_cold_reopen();
}

[[nodiscard]] CleanupAdmission
publish_canonical_cleanup_intent_with_duplicate_pending(CanonicalWorkerCleanupRoot& root) {
    auto continuation = publish_canonical_cleanup_intent(root);
    const auto paths = relation::OOCCleanupTransaction::paths_for(root.worker_base());
    CHECK(std::filesystem::exists(paths.intent_path));
    CHECK(!std::filesystem::exists(paths.intent_pending_path));
    write_immutable_test_leaf(paths.intent_pending_path,
                              fixture::read_file_bytes(paths.intent_path));
    CHECK(std::filesystem::exists(paths.intent_pending_path));
    return continuation;
}

[[nodiscard]] CleanupCompletionPreparationResult
drive_cleanup_to_completion_ready(CleanupAdmission&& admission, std::uint32_t expected_ordinal) {
    auto driven = cleanup_authority::trusted_test::
        drive_distributed_sieve_worker_cleanup_to_completion_ready_v1_with_root(
            std::move(admission));
    CHECK(driven);
    CHECK(!driven.retryable_root.has_value());
    CHECK(!driven.retryable_intent_conversion.has_value());
    CHECK(driven.completion_ready.has_value());
    CHECK(driven.diagnostic.phase ==
          cleanup_authority::DistributedSieveWorkerCleanupCompletionPreparationPhaseV1::complete);
    CHECK(driven.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupCompletionPreparationStatusV1::ready);
    CHECK(driven.diagnostic.disposition ==
          cleanup_authority::DistributedSieveWorkerCleanupCompletionPreparationDispositionV1::
              completion_ready);
    CHECK(!driven.diagnostic.native_error);
    CHECK(driven.diagnostic.cleanup_disposition ==
          std::optional{relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupResumeDispositionV2::
                            NamespaceAbsent});
    CHECK(driven.completion_ready->valid());
    CHECK(driven.completion_ready->manifest_order_ordinal() == expected_ordinal);
    return driven;
}

enum class InjectedCompletionPrefix : std::uint8_t {
    pending_only,
    canonical_only,
    identical_dual,
};

struct InjectedCompletionSnapshots final {
    std::optional<fixture::LeafSnapshot> canonical;
    std::optional<fixture::LeafSnapshot> pending;
};

[[nodiscard]] InjectedCompletionSnapshots
inject_active_completion_prefix(CanonicalWorkerCleanupRoot& root, InjectedCompletionPrefix shape) {
    const auto ordinal = root.active_ordinal();
    const auto& bytes = root.completion_bytes(ordinal);
    const auto canonical = root.completion_path(ordinal);
    const auto pending = root.completion_pending_path(ordinal);
    if (shape != InjectedCompletionPrefix::pending_only) {
        fixture::publish_canonical_record(root.prepared().root(), pending.filename().string(),
                                          canonical.filename().string(), bytes);
    }
    if (shape != InjectedCompletionPrefix::canonical_only) {
        write_immutable_test_leaf(pending, bytes);
    }
    return {
        .canonical = std::filesystem::exists(canonical)
                         ? std::optional{fixture::snapshot_leaf(canonical)}
                         : std::nullopt,
        .pending = std::filesystem::exists(pending) ? std::optional{fixture::snapshot_leaf(pending)}
                                                    : std::nullopt,
    };
}

[[nodiscard]] CleanupAdmission open_cleanup_root(const fixture::PreparedWaveFixture& prepared,
                                                 std::string_view context) {
    auto opened = wave::open_worker_cleanup_root_v1(prepared.root(), prepared.manifest_digest());
    if (!opened || !opened.admission.has_value()) {
        fail(context, __LINE__, wave_diagnostic_detail(opened.diagnostic));
    }
    CHECK(opened.admission->valid());
    return std::move(*opened.admission);
}

[[nodiscard]] std::vector<std::string> capture_root_entry_names(const std::filesystem::path& root) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

[[nodiscard]] sieve::ArtifactCleanupCompletedV1
read_cleanup_completion(const std::filesystem::path& path) {
    const auto decoded = sieve::decode_distributed_sieve_record(fixture::read_file_bytes(path));
    CHECK(decoded);
    CHECK(decoded.value.has_value());
    const auto* completion = std::get_if<sieve::ArtifactCleanupCompletedV1>(&*decoded.value);
    CHECK(completion != nullptr);
    return *completion;
}

void require_completion_publication_success(
    CleanupCompletionPublicationResult& result, std::uint32_t expected_ordinal,
    durable_record::RecordPublishDisposition expected_publication_disposition) {
    using Phase = cleanup_authority::DistributedSieveWorkerCleanupCompletionPublicationPhaseV1;
    using Status = cleanup_authority::DistributedSieveWorkerCleanupCompletionPublicationStatusV1;
    using Disposition =
        cleanup_authority::DistributedSieveWorkerCleanupCompletionPublicationDispositionV1;

    CHECK(result);
    CHECK(!result.retryable_completion_ready.has_value());
    CHECK(!result.retryable_recovery_root.has_value());
    CHECK(result.published_continuation.has_value());
    CHECK(result.published_continuation->valid());
    CHECK(result.published_continuation->manifest_order_ordinal() == expected_ordinal);
    CHECK(result.diagnostic.phase == Phase::complete);
    CHECK(result.diagnostic.status == Status::published);
    CHECK(result.diagnostic.disposition == Disposition::completion_published);
    CHECK(result.diagnostic.manifest_order_ordinal == expected_ordinal);
    CHECK(result.diagnostic.publication_status == durable_record::RecordPublishStatus::durable);
    CHECK(result.diagnostic.publication_disposition == expected_publication_disposition);
    CHECK(result.diagnostic.authority_spent);
    CHECK(result.diagnostic.publication_started);
    CHECK(!result.diagnostic.native_error);
}

void require_completed_cleanup_prefix(const CleanupAdmission& admission,
                                      std::size_t expected_completed,
                                      std::optional<std::uint32_t> expected_frontier) {
    const auto& prefix = admission.cleanup_prefix();
    CHECK(prefix.completed_worker_count == expected_completed);
    CHECK(prefix.frontier_manifest_order_ordinal == expected_frontier);
    CHECK(!prefix.active_manifest_order_ordinal.has_value());
    CHECK(prefix.coordinates.size() == expected_completed);
    for (const auto& coordinate : prefix.coordinates) {
        CHECK(coordinate.state == wave::DistributedSieveWorkerCleanupPrefixStateV1::completed);
        CHECK(coordinate.authorization_canonical_snapshot.has_value());
        CHECK(!coordinate.authorization_pending_snapshot.has_value());
        CHECK(coordinate.completion.has_value());
        CHECK(coordinate.completion_canonical_snapshot.has_value());
        CHECK(!coordinate.completion_pending_snapshot.has_value());
    }
}

struct CompletionPublicationStopContext final {
    CleanupCompletionPublicationFault target = CleanupCompletionPublicationFault::PendingDurable;
    std::array<bool, static_cast<std::size_t>(CleanupCompletionPublicationFault::Count)> observed{};
};

[[nodiscard]] bool stop_completion_publication_after(CleanupCompletionPublicationFault point,
                                                     void* opaque) noexcept {
    auto& context = *static_cast<CompletionPublicationStopContext*>(opaque);
    const auto index = static_cast<std::size_t>(point);
    if (index < context.observed.size()) {
        context.observed[index] = true;
    }
    return point == context.target;
}

enum class CompletionSuccessorMutation : std::uint8_t {
    replace_canonical,
    add_identical_pending,
};

struct CompletionSuccessorMutationContext final {
    CompletionSuccessorMutation mutation = CompletionSuccessorMutation::replace_canonical;
    std::filesystem::path canonical;
    std::filesystem::path pending;
    std::filesystem::path saved;
    bool invoked = false;
    std::exception_ptr failure;
};

[[nodiscard]] bool mutate_completion_successor(CleanupCompletionPublicationFault point,
                                               void* opaque) noexcept {
    auto& context = *static_cast<CompletionSuccessorMutationContext*>(opaque);
    if (point != CleanupCompletionPublicationFault::AfterFirstSuccessorObservation) {
        return false;
    }
    context.invoked = true;
    try {
        const auto bytes = fixture::read_file_bytes(context.canonical);
        if (context.mutation == CompletionSuccessorMutation::replace_canonical) {
            replace_file_with_same_bytes(context.canonical, context.saved, bytes);
        } else {
            write_immutable_test_leaf(context.pending, bytes);
        }
    } catch (...) {
        context.failure = std::current_exception();
    }
    return false;
}

[[nodiscard]] CommittedTail cold_open_tail(const std::filesystem::path& root,
                                           const Digest& manifest_digest) {
    auto opened = wave::DistributedSieveWaveStore::open(root, manifest_digest);
    if (!opened || !opened.committed_tail_admission.has_value()) {
        fail("cold-open committed tail", __LINE__, wave_diagnostic_detail(opened.diagnostic));
    }
    CHECK(opened.store == nullptr);
    CHECK(!opened.prepared_admission.has_value());
    return std::move(*opened.committed_tail_admission);
}

[[nodiscard]] std::array<WorkerSnapshot, 2>
capture_worker_snapshots(const fixture::PreparedWaveFixture& prepared) {
    return {prepared.worker_snapshot(0), prepared.worker_snapshot(1)};
}

void require_worker_snapshots(const fixture::PreparedWaveFixture& prepared,
                              const std::array<WorkerSnapshot, 2>& expected) {
    CHECK(prepared.worker_snapshot(0) == expected[0]);
    CHECK(prepared.worker_snapshot(1) == expected[1]);
}

[[nodiscard]] std::array<std::filesystem::path, 3>
private_handoff_bases(const fixture::PreparedWaveFixture& prepared) {
    std::array<std::filesystem::path, 3> bases;
    for (std::size_t index = 0; index < 2U; ++index) {
        const auto& chunk = prepared.manifest().chunks[index];
        const auto names = wave::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        bases[index] = prepared.root() / names->private_directory_leaf / "corpus";
    }
    const auto merge_names = wave::distributed_sieve_merge_generation_names_v1(
        prepared.merge_started().merge_attempt_ordinal);
    CHECK(merge_names.has_value());
    bases[2] = prepared.root() / merge_names->private_directory_leaf / "corpus";
    return bases;
}

struct MergedCorpusSnapshot final {
    std::array<fixture::LeafSnapshot, 3> leaves;
};

[[nodiscard]] MergedCorpusSnapshot capture_merged_corpus(const std::filesystem::path& merged_base) {
    const auto paths = relation::OOCCleanupTransaction::paths_for(merged_base);
    return {
        .leaves =
            {
                fixture::snapshot_leaf(paths.private_handoff_path),
                fixture::snapshot_leaf(paths.index_path),
                fixture::snapshot_leaf(paths.data_path),
            },
    };
}

void require_merged_corpus(const std::filesystem::path& merged_base,
                           const MergedCorpusSnapshot& expected) {
    const auto actual = capture_merged_corpus(merged_base);
    CHECK(actual.leaves == expected.leaves);
}

void require_no_cleanup_records(const std::filesystem::path& root) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        CHECK(
            !wave::parse_distributed_sieve_cleanup_record_leaf_v1(entry.path().filename().string())
                 .has_value());
    }
}

[[nodiscard]] bool can_adopt_private_handoff(const std::filesystem::path& base) {
    auto adopted = relation::OOCCleanupTransaction::adopt_private_handoff(base);
    return adopted.adopted() && adopted.adoption.has_value() && !adopted.adoption->spent();
}

[[nodiscard]] int private_handoff_lock_probe_child(std::span<const std::filesystem::path> bases) {
    for (const auto& base : bases) {
        if (!can_adopt_private_handoff(base)) {
            return BASE_LOCK_HELD_EXIT;
        }
    }
    return BASE_LOCKS_FREE_EXIT;
}

[[nodiscard]] int direct_base_lock_probe_child(const std::filesystem::path& lock_path) noexcept {
    int descriptor = -1;
    do {
        descriptor = ::open(lock_path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return EXIT_FAILURE;
    }

    int locked = -1;
    do {
        locked = ::flock(descriptor, LOCK_EX | LOCK_NB);
    } while (locked != 0 && errno == EINTR);
    const int lock_error = errno;
    (void)::close(descriptor);
    if (locked == 0) {
        return BASE_LOCKS_FREE_EXIT;
    }
    return lock_error == EWOULDBLOCK ? BASE_LOCK_HELD_EXIT : EXIT_FAILURE;
}

[[nodiscard]] int wave_lock_probe_child(const std::filesystem::path& root,
                                        const Digest& manifest_digest) {
    auto opened = wave::DistributedSieveWaveStore::open(root, manifest_digest);
    const bool busy = !opened && opened.store == nullptr &&
                      !opened.prepared_admission.has_value() &&
                      !opened.committed_tail_admission.has_value() &&
                      opened.diagnostic.status == wave::DistributedSieveWaveStoreStatus::lock_busy;
    return busy ? WAVE_LOCK_BUSY_EXIT : EXIT_FAILURE;
}

[[nodiscard]] gnfs::test::ChildProcessResult
run_base_lock_probe(const std::array<std::filesystem::path, 3>& bases) {
    return gnfs::test::run_child_process(test_executable, {"--probe-base-locks", bases[0].string(),
                                                           bases[1].string(), bases[2].string()});
}

[[nodiscard]] gnfs::test::ChildProcessResult
run_direct_base_lock_probe(const std::filesystem::path& base) {
    const auto lock_path = relation::OOCCleanupTransaction::paths_for(base).lock_path;
    return gnfs::test::run_child_process(test_executable,
                                         {"--probe-base-lock-file", lock_path.string()});
}

[[nodiscard]] std::size_t count_open_descriptors_for_path(const std::filesystem::path& path) {
    struct stat expected {};
    int inspected = -1;
    do {
        inspected = ::stat(path.c_str(), &expected);
    } while (inspected != 0 && errno == EINTR);
    if (inspected != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "stat worker BaseLock for descriptor count");
    }
    const int descriptor_limit = ::getdtablesize();
    CHECK(descriptor_limit > 0);
    std::size_t count = 0;
    for (int descriptor = 0; descriptor < descriptor_limit; ++descriptor) {
        struct stat actual {};
        int result = -1;
        do {
            result = ::fstat(descriptor, &actual);
        } while (result != 0 && errno == EINTR);
        if (result == 0 && actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino) {
            ++count;
        }
    }
    return count;
}

void require_exact_active_worker_base_lock(const std::array<std::filesystem::path, 3>& bases,
                                           std::uint32_t active_ordinal) {
    CHECK(active_ordinal < 2U);
    for (std::size_t ordinal = 0; ordinal < bases.size(); ++ordinal) {
        const auto probe = run_direct_base_lock_probe(bases[ordinal]);
        CHECK(probe.exited);
        CHECK(!probe.signaled);
        CHECK(probe.exit_code ==
              (ordinal == active_ordinal ? BASE_LOCK_HELD_EXIT : BASE_LOCKS_FREE_EXIT));
        const auto lock_path = relation::OOCCleanupTransaction::paths_for(bases[ordinal]).lock_path;
        CHECK(count_open_descriptors_for_path(lock_path) == (ordinal == active_ordinal ? 1U : 0U));
    }
}

void require_wave_lock_busy(const fixture::PreparedWaveFixture& prepared) {
    const auto wave_probe = gnfs::test::run_child_process(
        test_executable,
        {"--probe-wave-lock", prepared.root().string(), digest_hex(prepared.manifest_digest())});
    CHECK(wave_probe.exited);
    CHECK(!wave_probe.signaled);
    CHECK(wave_probe.exit_code == WAVE_LOCK_BUSY_EXIT);
}

void require_all_base_locks_free(const std::array<std::filesystem::path, 3>& bases) {
    for (const auto& base : bases) {
        const auto probe = run_direct_base_lock_probe(base);
        CHECK(probe.exited);
        CHECK(!probe.signaled);
        CHECK(probe.exit_code == BASE_LOCKS_FREE_EXIT);
        const auto lock_path = relation::OOCCleanupTransaction::paths_for(base).lock_path;
        CHECK(count_open_descriptors_for_path(lock_path) == 0U);
    }
}

void require_cleanup_admission(
    CleanupResult& transitioned, const sieve::WaveMergeCommitV1& expected_commit,
    std::span<const Relation> expected_rows,
    const cleanup_authority::DistributedSieveWorkerCleanupTailPhaseV1 expected_phase =
        cleanup_authority::DistributedSieveWorkerCleanupTailPhaseV1::complete) {
    if (!transitioned || !transitioned.admission.has_value()) {
        fail("consume committed tail for worker cleanup", __LINE__,
             cleanup_diagnostic_detail(transitioned.diagnostic));
    }
    CHECK(!transitioned.retryable_tail.has_value());
    CHECK(transitioned.diagnostic.phase == expected_phase);
    CHECK(transitioned.diagnostic.tail_spent);
    CHECK(!transitioned.diagnostic.cold_reopen_required);
    CHECK(transitioned.admission->valid());
    CHECK(transitioned.admission->commit().self_digest == expected_commit.self_digest);
    CHECK(transitioned.admission->cleanup_prefix().coordinates.empty());
    CHECK(transitioned.admission->reader().count() == expected_rows.size());
    CHECK(fixture::relation_vectors_equal(transitioned.admission->reader().read_all(),
                                          expected_rows));
}

void require_free_base_locks_and_busy_new_wave_lock(
    const fixture::PreparedWaveFixture& prepared,
    const std::array<std::filesystem::path, 3>& bases) {
    const auto base_probe = run_base_lock_probe(bases);
    CHECK(base_probe.exited);
    CHECK(!base_probe.signaled);
    CHECK(base_probe.exit_code == BASE_LOCKS_FREE_EXIT);

    require_wave_lock_busy(prepared);
}

void test_fresh_tail_crosses_one_lock_generation_without_mutation() {
    fixture::PreparedWaveFixture prepared("worker-cleanup-tail-fresh");
    auto tail = commit_fresh(prepared);
    CHECK(tail.valid());
    const auto expected_commit = tail.record();
    const auto worker_snapshots = capture_worker_snapshots(prepared);
    const auto bases = private_handoff_bases(prepared);
    const auto merged_snapshot = capture_merged_corpus(bases[2]);
    require_no_cleanup_records(prepared.root());

    const auto held_probe = run_base_lock_probe(bases);
    CHECK(held_probe.exited);
    CHECK(!held_probe.signaled);
    CHECK(held_probe.exit_code == BASE_LOCK_HELD_EXIT);

    auto transitioned =
        cleanup_authority::consume_distributed_sieve_committed_tail_for_worker_cleanup_v1(
            std::move(tail));
    CHECK(!tail.valid());
    require_cleanup_admission(transitioned, expected_commit, prepared.expected_rows());
    require_worker_snapshots(prepared, worker_snapshots);
    require_merged_corpus(bases[2], merged_snapshot);
    require_no_cleanup_records(prepared.root());
    require_free_base_locks_and_busy_new_wave_lock(prepared, bases);

    auto replay = cleanup_authority::consume_distributed_sieve_committed_tail_for_worker_cleanup_v1(
        std::move(tail));
    CHECK(!replay);
    CHECK(!replay.retryable_tail.has_value());
    CHECK(!replay.admission.has_value());
    CHECK(!replay.diagnostic.tail_spent);
    CHECK(!replay.diagnostic.cold_reopen_required);
    CHECK(replay.diagnostic.phase ==
          cleanup_authority::DistributedSieveWorkerCleanupTailPhaseV1::admission_validation);
    CHECK(replay.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupTailStatusV1::invalid_admission);
    require_worker_snapshots(prepared, worker_snapshots);
    require_merged_corpus(bases[2], merged_snapshot);
    require_no_cleanup_records(prepared.root());

    transitioned.admission.reset();
    auto reopened = cold_open_tail(prepared.root(), prepared.manifest_digest());
    CHECK(reopened.valid());
}

void test_cold_tail_crosses_same_lock_generation_boundary() {
    fixture::PreparedWaveFixture prepared("worker-cleanup-tail-cold");
    auto fresh_tail = commit_fresh(prepared);
    const auto expected_commit = fresh_tail.record();
    const auto worker_snapshots = capture_worker_snapshots(prepared);
    const auto bases = private_handoff_bases(prepared);
    const auto merged_snapshot = capture_merged_corpus(bases[2]);
    {
        std::optional<CommittedTail> held;
        held.emplace(std::move(fresh_tail));
        held.reset();
    }
    auto cold_tail = cold_open_tail(prepared.root(), prepared.manifest_digest());
    CHECK(cold_tail.valid());

    auto transitioned =
        cleanup_authority::consume_distributed_sieve_committed_tail_for_worker_cleanup_v1(
            std::move(cold_tail));
    CHECK(!cold_tail.valid());
    require_cleanup_admission(transitioned, expected_commit, prepared.expected_rows());
    require_worker_snapshots(prepared, worker_snapshots);
    require_merged_corpus(bases[2], merged_snapshot);
    require_no_cleanup_records(prepared.root());
    require_free_base_locks_and_busy_new_wave_lock(prepared, bases);
}

void test_pre_spend_failure_returns_the_unique_retryable_tail() {
    fixture::PreparedWaveFixture prepared("worker-cleanup-tail-pre-spend-retry");
    auto tail = commit_fresh(prepared);
    const auto expected_commit = tail.record();
    const auto worker_snapshots = capture_worker_snapshots(prepared);
    const auto bases = private_handoff_bases(prepared);
    const auto merged_snapshot = capture_merged_corpus(bases[2]);
    InjectedPreSpendFailureContext injected;

    auto interrupted = cleanup_authority::trusted_test::
        consume_distributed_sieve_committed_tail_for_worker_cleanup_v1_with_hooks(
            std::move(tail),
            cleanup_authority::trusted_test::DistributedSieveWorkerCleanupTailTestHooksV1{
                .before_final_tail_revalidation = inject_pre_spend_failure,
                .context = &injected,
            });
    CHECK(injected.invoked);
    CHECK(!tail.valid());
    CHECK(!interrupted);
    CHECK(interrupted.retryable_tail.has_value());
    CHECK(interrupted.retryable_tail->valid());
    CHECK(!interrupted.admission.has_value());
    CHECK(interrupted.diagnostic.phase ==
          cleanup_authority::DistributedSieveWorkerCleanupTailPhaseV1::root_snapshot);
    CHECK(interrupted.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupTailStatusV1::root_snapshot_failed);
    CHECK(interrupted.diagnostic.wave_store.status ==
          wave::DistributedSieveWaveStoreStatus::interrupted);
    CHECK(!interrupted.diagnostic.tail_spent);
    CHECK(!interrupted.diagnostic.cold_reopen_required);
    require_worker_snapshots(prepared, worker_snapshots);
    require_merged_corpus(bases[2], merged_snapshot);
    require_no_cleanup_records(prepared.root());

    auto retried =
        cleanup_authority::consume_distributed_sieve_committed_tail_for_worker_cleanup_v1(
            std::move(*interrupted.retryable_tail));
    require_cleanup_admission(retried, expected_commit, prepared.expected_rows());
}

void test_failed_final_revalidation_spends_the_invalid_tail() {
    fixture::PreparedWaveFixture prepared("worker-cleanup-tail-final-invalid");
    auto tail = commit_fresh(prepared);
    CHECK(tail.valid());
    const auto worker_snapshots = capture_worker_snapshots(prepared);
    const auto bases = private_handoff_bases(prepared);
    const auto merged_snapshot = capture_merged_corpus(bases[2]);
    const auto canonical =
        prepared.root() / std::string(wave::DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF);
    GapReplacementContext replacement{
        .kind = GapReplacementKind::manifest,
        .root = prepared.root(),
        .canonical = canonical,
        .saved = prepared.root().parent_path() /
                 (prepared.root().filename().string() + ".saved-final-revalidation"),
        .bytes = fixture::read_file_bytes(canonical),
        .original_identity = fixture::native_identity(canonical),
    };

    auto rejected = cleanup_authority::trusted_test::
        consume_distributed_sieve_committed_tail_for_worker_cleanup_v1_with_hooks(
            std::move(tail),
            cleanup_authority::trusted_test::DistributedSieveWorkerCleanupTailTestHooksV1{
                .before_final_tail_revalidation = replace_before_final_tail_revalidation,
                .context = &replacement,
            });
    CHECK(replacement.invoked);
    if (replacement.failure != nullptr) {
        if (replacement.replacement_present) {
            restore_generation_gap_anchor(replacement);
        }
        std::rethrow_exception(replacement.failure);
    }
    CHECK(replacement.replacement_present);
    CHECK(replacement.replacement_identity != replacement.original_identity);
    CHECK(!tail.valid());
    CHECK(!rejected);
    CHECK(!rejected.retryable_tail.has_value());
    CHECK(!rejected.admission.has_value());
    CHECK(rejected.diagnostic.phase ==
          cleanup_authority::DistributedSieveWorkerCleanupTailPhaseV1::old_epoch_release);
    CHECK(rejected.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupTailStatusV1::root_snapshot_failed);
    CHECK(rejected.diagnostic.wave_store.status ==
          wave::DistributedSieveWaveStoreStatus::manifest_conflict);
    CHECK(rejected.diagnostic.tail_spent);
    CHECK(rejected.diagnostic.cold_reopen_required);

    restore_generation_gap_anchor(replacement);
    require_worker_snapshots(prepared, worker_snapshots);
    require_merged_corpus(bases[2], merged_snapshot);
    require_no_cleanup_records(prepared.root());
    auto reopened = cold_open_tail(prepared.root(), prepared.manifest_digest());
    CHECK(reopened.valid());
}

void test_exact_anchor_rejects_generation_gap_replacements() {
    constexpr std::array replacement_kinds{
        GapReplacementKind::root,
        GapReplacementKind::wave_lock,
        GapReplacementKind::manifest,
        GapReplacementKind::merge_commit,
    };
    for (const auto kind : replacement_kinds) {
        fixture::PreparedWaveFixture prepared("worker-cleanup-tail-replace-" +
                                              std::string(gap_replacement_label(kind)));
        auto tail = commit_fresh(prepared);
        CHECK(tail.valid());
        const auto worker_snapshots = capture_worker_snapshots(prepared);
        const auto bases = private_handoff_bases(prepared);
        const auto merged_snapshot = capture_merged_corpus(bases[2]);
        const auto canonical = gap_replacement_target(prepared.root(), kind);
        const auto root_manifest_bytes = fixture::read_file_bytes(
            prepared.root() / std::string(wave::DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF));
        const auto root_commit_bytes = fixture::read_file_bytes(
            prepared.root() / std::string(wave::DISTRIBUTED_SIEVE_WAVE_MERGE_COMMIT_RECORD_LEAF));
        GapReplacementContext replacement{
            .kind = kind,
            .root = prepared.root(),
            .canonical = canonical,
            .saved = prepared.root().parent_path() /
                     (prepared.root().filename().string() + ".saved-gap-" +
                      std::string(gap_replacement_label(kind))),
            .bytes = kind == GapReplacementKind::root ? std::vector<std::byte>{}
                                                      : fixture::read_file_bytes(canonical),
            .original_identity = fixture::native_identity(canonical),
        };

        auto transitioned = cleanup_authority::trusted_test::
            consume_distributed_sieve_committed_tail_for_worker_cleanup_v1_with_hooks(
                std::move(tail),
                cleanup_authority::trusted_test::DistributedSieveWorkerCleanupTailTestHooksV1{
                    .after_old_epoch_release = replace_generation_gap_anchor,
                    .context = &replacement,
                });
        CHECK(replacement.invoked);
        if (replacement.failure != nullptr) {
            if (replacement.replacement_present) {
                restore_generation_gap_anchor(replacement);
            }
            std::rethrow_exception(replacement.failure);
        }
        CHECK(replacement.replacement_present);
        CHECK(replacement.replacement_identity != replacement.original_identity);
        if (kind == GapReplacementKind::root) {
            CHECK(fixture::read_file_bytes(
                      prepared.root() / std::string(wave::DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF)) ==
                  root_manifest_bytes);
            CHECK(fixture::read_file_bytes(
                      prepared.root() /
                      std::string(wave::DISTRIBUTED_SIEVE_WAVE_MERGE_COMMIT_RECORD_LEAF)) ==
                  root_commit_bytes);
        }
        CHECK(!tail.valid());
        CHECK(!transitioned);
        CHECK(!transitioned.retryable_tail.has_value());
        CHECK(!transitioned.admission.has_value());
        CHECK(transitioned.diagnostic.phase ==
              cleanup_authority::DistributedSieveWorkerCleanupTailPhaseV1::cleanup_root_open);
        CHECK(
            transitioned.diagnostic.status ==
            cleanup_authority::DistributedSieveWorkerCleanupTailStatusV1::cleanup_root_open_failed);
        CHECK(transitioned.diagnostic.tail_spent);
        CHECK(transitioned.diagnostic.cold_reopen_required);
        CHECK(transitioned.diagnostic.wave_store.status == expected_gap_replacement_status(kind));

        restore_generation_gap_anchor(replacement);
        require_worker_snapshots(prepared, worker_snapshots);
        require_merged_corpus(bases[2], merged_snapshot);
        require_no_cleanup_records(prepared.root());
        auto recovered = cold_open_tail(prepared.root(), prepared.manifest_digest());
        CHECK(recovered.valid());
    }
}

void test_forked_tail_is_rejected_and_parent_remains_authoritative() {
    fixture::PreparedWaveFixture prepared("worker-cleanup-tail-fork");
    auto tail = commit_fresh(prepared);
    CHECK(tail.valid());
    const auto expected_commit = tail.record();
    const auto worker_snapshots = capture_worker_snapshots(prepared);
    const auto bases = private_handoff_bases(prepared);
    const auto merged_snapshot = capture_merged_corpus(bases[2]);

    const pid_t child = ::fork();
    if (child < 0) {
        throw std::system_error(errno, std::generic_category(), "fork cleanup-tail test");
    }
    if (child == 0) {
        auto rejected =
            cleanup_authority::consume_distributed_sieve_committed_tail_for_worker_cleanup_v1(
                std::move(tail));
        const bool correct =
            !rejected && !rejected.retryable_tail.has_value() && !rejected.admission.has_value() &&
            !rejected.diagnostic.tail_spent && !rejected.diagnostic.cold_reopen_required &&
            rejected.diagnostic.phase ==
                cleanup_authority::DistributedSieveWorkerCleanupTailPhaseV1::admission_validation &&
            rejected.diagnostic.status ==
                cleanup_authority::DistributedSieveWorkerCleanupTailStatusV1::process_mismatch;
        ::_exit(correct ? FORK_REJECTED_EXIT : EXIT_FAILURE);
    }
    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        throw std::system_error(errno, std::generic_category(), "wait for cleanup-tail child");
    }
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == FORK_REJECTED_EXIT);
    CHECK(tail.valid());
    require_worker_snapshots(prepared, worker_snapshots);
    require_merged_corpus(bases[2], merged_snapshot);
    require_no_cleanup_records(prepared.root());

    auto transitioned =
        cleanup_authority::consume_distributed_sieve_committed_tail_for_worker_cleanup_v1(
            std::move(tail));
    require_cleanup_admission(transitioned, expected_commit, prepared.expected_rows());
}

void test_cleanup_receipt_requires_one_canonical_active_frontier() {
    fixture::PreparedWaveFixture prepared("worker-cleanup-receipt-no-authorization");
    auto tail = commit_fresh(prepared);
    const auto expected_commit = tail.record();
    auto transitioned =
        cleanup_authority::consume_distributed_sieve_committed_tail_for_worker_cleanup_v1(
            std::move(tail));
    require_cleanup_admission(transitioned, expected_commit, prepared.expected_rows());

    auto rejected =
        cleanup_authority::mint_distributed_sieve_worker_cleanup_authorization_receipt_v1(
            *transitioned.admission);
    CHECK(!rejected);
    CHECK(!rejected.minted.has_value());
    CHECK(rejected.diagnostic.phase ==
          cleanup_authority::DistributedSieveWorkerCleanupReceiptMintPhaseV1::admission_validation);
    CHECK(rejected.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupReceiptMintStatusV1::
              authorization_not_canonical);
    CHECK(!rejected.diagnostic.cold_reopen_required);
}

void test_cleanup_receipt_single_live_move_release_base_lock_and_fork() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-receipt-lifecycle");
    auto first = mint_cleanup_receipt(root);
    CHECK(!first.minted->receipt.spent());

    auto duplicate =
        cleanup_authority::mint_distributed_sieve_worker_cleanup_authorization_receipt_v1(
            root.admission());
    CHECK(!duplicate);
    CHECK(!duplicate.minted.has_value());
    CHECK(duplicate.diagnostic.phase ==
          cleanup_authority::DistributedSieveWorkerCleanupReceiptMintPhaseV1::live_claim);
    CHECK(
        duplicate.diagnostic.status ==
        cleanup_authority::DistributedSieveWorkerCleanupReceiptMintStatusV1::receipt_already_live);
    CHECK(!duplicate.diagnostic.cold_reopen_required);
    CHECK(!first.minted->receipt.spent());

    std::optional<CleanupReceiptMinted> moved_owner;
    moved_owner.emplace(std::move(*first.minted));
    first.minted.reset();
    auto still_claimed =
        cleanup_authority::mint_distributed_sieve_worker_cleanup_authorization_receipt_v1(
            root.admission());
    CHECK(!still_claimed);
    CHECK(
        still_claimed.diagnostic.status ==
        cleanup_authority::DistributedSieveWorkerCleanupReceiptMintStatusV1::receipt_already_live);

    {
        auto adopted = relation::OOCCleanupTransaction::adopt_private_handoff(root.worker_base());
        CHECK(adopted.adopted());
        CHECK(adopted.adoption.has_value());
        relation::OOCPrivateHandoffReader reader(std::move(*adopted.adoption));
        CHECK(reader.valid());
        CHECK(!moved_owner->receipt.spent());

        const pid_t child = ::fork();
        if (child < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "fork worker-cleanup receipt test");
        }
        if (child == 0) {
            ::_exit(moved_owner->receipt.spent() ? FORK_REJECTED_EXIT : EXIT_FAILURE);
        }
        int status = 0;
        pid_t waited = -1;
        do {
            waited = ::waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "wait for worker-cleanup receipt child");
        }
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == FORK_REJECTED_EXIT);
        CHECK(!moved_owner->receipt.spent());
    }

    moved_owner.reset();
    auto reminted = mint_cleanup_receipt(root);
    CHECK(!reminted.minted->receipt.spent());
}

void test_cleanup_receipt_spend_retains_claim_and_ignores_private_relation_changes() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-receipt-private-change");
    auto minted = mint_cleanup_receipt(root);
    auto adopted = relation::OOCCleanupTransaction::adopt_private_handoff(root.worker_base());
    CHECK(adopted.adopted());
    CHECK(adopted.adoption.has_value());
    relation::OOCPrivateHandoffReader reader(std::move(*adopted.adoption));
    CHECK(reader.valid());
    CHECK(!minted.minted->receipt.spent());

    const auto paths = relation::OOCCleanupTransaction::paths_for(root.worker_base());
    const auto converted =
        relation::ooc_cleanup_detail::convert_authorized_private_handoff_to_cleanup_intent_v2(
            std::move(reader), std::move(minted.minted->receipt));
    CHECK(converted.intent_published());
    CHECK(converted.capabilities_spent());
    CHECK(std::filesystem::exists(paths.intent_path));
    CHECK(minted.minted->receipt.spent());

    auto blocked =
        cleanup_authority::mint_distributed_sieve_worker_cleanup_authorization_receipt_v1(
            root.admission());
    CHECK(!blocked);
    CHECK(
        blocked.diagnostic.status ==
        cleanup_authority::DistributedSieveWorkerCleanupReceiptMintStatusV1::receipt_already_live);

    minted.minted.reset();
    auto reminted = mint_cleanup_receipt(root);
    CHECK(!reminted.minted->receipt.spent());
}

void test_cleanup_receipt_sticky_invalidates_same_byte_authorization_replacement() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-receipt-replacement");
    auto minted = mint_cleanup_receipt(root);
    const auto canonical = root.authorization_path();
    const auto saved = root.prepared().root().parent_path() /
                       (root.prepared().root().filename().string() + ".saved-live-authorization");
    const auto original_identity = fixture::native_identity(canonical);
    replace_file_with_same_bytes(canonical, saved, root.authorization_bytes());
    CHECK(fixture::native_identity(canonical) != original_identity);
    CHECK(minted.minted->receipt.spent());

    restore_replaced_file(canonical, saved);
    CHECK(fixture::native_identity(canonical) == original_identity);
    CHECK(root.admission().valid());
    CHECK(minted.minted->receipt.spent());
    auto rejected =
        cleanup_authority::mint_distributed_sieve_worker_cleanup_authorization_receipt_v1(
            root.admission());
    CHECK(!rejected);
    CHECK(rejected.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupReceiptMintStatusV1::
              root_authority_invalid);
    CHECK(rejected.diagnostic.cold_reopen_required);

    minted.minted.reset();
    root.release_and_cold_reopen();
    auto recovered = mint_cleanup_receipt(root);
    CHECK(!recovered.minted->receipt.spent());
}

void test_cleanup_receipt_anchors_the_complete_completed_prefix() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-receipt-complete-prefix", 1);
    auto minted = mint_cleanup_receipt(root);
    CHECK(minted.minted->manifest_order_ordinal == 1U);
    const auto canonical = root.completion_path(0);
    const auto saved = root.prepared().root().parent_path() /
                       (root.prepared().root().filename().string() + ".saved-prior-completion");
    const auto original_identity = fixture::native_identity(canonical);
    replace_file_with_same_bytes(canonical, saved, root.completion_bytes(0));
    CHECK(fixture::native_identity(canonical) != original_identity);
    CHECK(minted.minted->receipt.spent());

    restore_replaced_file(canonical, saved);
    CHECK(fixture::native_identity(canonical) == original_identity);
    CHECK(root.admission().valid());
    CHECK(minted.minted->receipt.spent());
}

void test_cleanup_receipt_sticky_invalidates_added_cleanup_leaf() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-receipt-added-leaf");
    auto minted = mint_cleanup_receipt(root);
    const auto pending = root.authorization_pending_path();
    write_immutable_test_leaf(pending, root.authorization_bytes());
    CHECK(minted.minted->receipt.spent());

    std::error_code remove_error;
    CHECK(std::filesystem::remove(pending, remove_error));
    CHECK(!remove_error);
    sync_directory(root.prepared().root());
    CHECK(root.admission().valid());
    CHECK(minted.minted->receipt.spent());
    auto rejected =
        cleanup_authority::mint_distributed_sieve_worker_cleanup_authorization_receipt_v1(
            root.admission());
    CHECK(!rejected);
    CHECK(rejected.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupReceiptMintStatusV1::
              root_authority_invalid);
    CHECK(rejected.diagnostic.cold_reopen_required);

    minted.minted.reset();
    root.release_and_cold_reopen();
    write_immutable_test_leaf(pending, root.authorization_bytes());
    auto directly_revalidated =
        cleanup_authority::mint_distributed_sieve_worker_cleanup_authorization_receipt_v1(
            root.admission());
    CHECK(!directly_revalidated);
    CHECK(directly_revalidated.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupReceiptMintStatusV1::
              root_authority_invalid);
    CHECK(directly_revalidated.diagnostic.wave_store.status ==
          wave::DistributedSieveWaveStoreStatus::namespace_conflict);
    CHECK(directly_revalidated.diagnostic.wave_store.native_error);
    CHECK(directly_revalidated.diagnostic.native_error ==
          directly_revalidated.diagnostic.wave_store.native_error);
    std::error_code final_remove_error;
    CHECK(std::filesystem::remove(pending, final_remove_error));
    CHECK(!final_remove_error);
    sync_directory(root.prepared().root());
}

void test_cleanup_capsule_prepare_returns_retryable_root_while_receipt_is_live() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-capsule-live-receipt");
    auto competing = mint_cleanup_receipt(root);

    auto blocked = cleanup_authority::prepare_distributed_sieve_worker_cleanup_intent_conversion_v1(
        root.take_admission());
    CHECK(!blocked.capsule.has_value());
    CHECK(blocked.retryable_root.has_value());
    CHECK(blocked.retryable_root->valid());
    CHECK(blocked.diagnostic.phase ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionPreparePhaseV1::
              receipt_mint);
    CHECK(blocked.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1::
              receipt_mint_failed);
    CHECK(
        blocked.diagnostic.receipt_mint.status ==
        cleanup_authority::DistributedSieveWorkerCleanupReceiptMintStatusV1::receipt_already_live);

    competing.minted.reset();
    auto retried = cleanup_authority::prepare_distributed_sieve_worker_cleanup_intent_conversion_v1(
        std::move(*blocked.retryable_root));
    CHECK(!retried.retryable_root.has_value());
    CHECK(retried.capsule.has_value());
    CHECK(retried.diagnostic.phase ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionPreparePhaseV1::complete);
    CHECK(retried.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1::ready);
    CHECK(retried.capsule->valid());
}

void test_cleanup_capsule_selects_active_ordinal_and_retains_one_base_lock_fd() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-capsule-active-ordinal", 1);
    const auto bases = private_handoff_bases(root.prepared());
    auto prepared = prepare_cleanup_conversion_capsule(root);

    CHECK(prepared.capsule->manifest_order_ordinal() == 1U);
    require_exact_active_worker_base_lock(bases, 1U);
    require_wave_lock_busy(root.prepared());
    CHECK(prepared.capsule->valid());
}

void test_cleanup_capsule_reader_views_closed_retries_in_place() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-capsule-reader-views-closed");
    const auto bases = private_handoff_bases(root.prepared());
    auto prepared = prepare_cleanup_conversion_capsule(root);
    CleanupConversionStopContext context{
        .target = relation::ooc_cleanup_detail::
            OOCPrivateHandoffCleanupIntentPublicationFaultPointV2::ReaderViewsClosed,
    };

    auto interrupted = cleanup_authority::trusted_test::
        execute_distributed_sieve_worker_cleanup_intent_conversion_v1_with_hooks(
            std::move(*prepared.capsule),
            relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentPublicationTestHooksV2{
                .stop_after = stop_cleanup_conversion_after,
                .context = &context,
            });
    CHECK(context.invoked);
    CHECK(interrupted);
    CHECK(interrupted.retryable_capsule.has_value());
    CHECK(!interrupted.root_continuation.has_value());
    CHECK(interrupted.publication.capabilities_retained());
    CHECK(interrupted.publication.result.status == relation::OOCCleanupStatus::Interrupted);
    CHECK(interrupted.publication.result.stage == relation::OOCCleanupStage::None);
    CHECK(!interrupted.publication.result.native_error);
    CHECK(interrupted.retryable_capsule->valid());
    CHECK(interrupted.diagnostic.phase ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionExecutePhaseV1::complete);
    CHECK(interrupted.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::
              capabilities_retained);
    CHECK(interrupted.diagnostic.capsule_retained);
    CHECK(!interrupted.diagnostic.root_continuation_retained);
    CHECK(!interrupted.diagnostic.cold_reopen_required);
    require_exact_active_worker_base_lock(bases, 0U);
    require_wave_lock_busy(root.prepared());

    auto resumed = cleanup_authority::execute_distributed_sieve_worker_cleanup_intent_conversion_v1(
        std::move(*interrupted.retryable_capsule));
    CHECK(resumed);
    CHECK(!resumed.retryable_capsule.has_value());
    CHECK(resumed.root_continuation.has_value());
    CHECK(resumed.publication.intent_published());
    CHECK(resumed.publication.result.status == relation::OOCCleanupStatus::RecoveryRequired);
    CHECK(resumed.publication.result.stage == relation::OOCCleanupStage::IntentDurable);
    CHECK(resumed.publication.result.native_error ==
          std::make_error_code(std::errc::protocol_error));
    CHECK(resumed.diagnostic.phase ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionExecutePhaseV1::complete);
    CHECK(resumed.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::
              intent_published);
    CHECK(!resumed.diagnostic.capsule_retained);
    CHECK(resumed.diagnostic.root_continuation_retained);
    CHECK(!resumed.diagnostic.cold_reopen_required);
    require_all_base_locks_free(bases);
    require_wave_lock_busy(root.prepared());

    auto reminted = mint_cleanup_receipt(*resumed.root_continuation, 0U);
    CHECK(!reminted.minted->receipt.spent());
    require_all_base_locks_free(bases);
    require_wave_lock_busy(root.prepared());
}

void test_cleanup_capsule_intent_publication_returns_root_and_releases_base_lock() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-capsule-intent-published");
    const auto bases = private_handoff_bases(root.prepared());
    const auto paths = relation::OOCCleanupTransaction::paths_for(bases[0]);
    auto prepared = prepare_cleanup_conversion_capsule(root);

    auto published =
        cleanup_authority::execute_distributed_sieve_worker_cleanup_intent_conversion_v1(
            std::move(*prepared.capsule));
    CHECK(published);
    CHECK(!published.retryable_capsule.has_value());
    CHECK(published.root_continuation.has_value());
    CHECK(published.root_continuation->valid());
    CHECK(published.publication.intent_published());
    CHECK(published.publication.result.status == relation::OOCCleanupStatus::RecoveryRequired);
    CHECK(published.publication.result.stage == relation::OOCCleanupStage::IntentDurable);
    CHECK(published.publication.result.native_error ==
          std::make_error_code(std::errc::protocol_error));
    CHECK(published.diagnostic.phase ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionExecutePhaseV1::complete);
    CHECK(published.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::
              intent_published);
    CHECK(!published.diagnostic.capsule_retained);
    CHECK(published.diagnostic.root_continuation_retained);
    CHECK(!published.diagnostic.cold_reopen_required);
    CHECK(std::filesystem::exists(paths.intent_path));
    CHECK(!std::filesystem::exists(paths.intent_pending_path));
    require_all_base_locks_free(bases);
    require_wave_lock_busy(root.prepared());

    auto reminted = mint_cleanup_receipt(*published.root_continuation, 0U);
    CHECK(!reminted.minted->receipt.spent());
    require_all_base_locks_free(bases);
    require_wave_lock_busy(root.prepared());
}

void test_cleanup_capsule_canonical_promotion_returns_reconciliation_root() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-capsule-canonical-promoted");
    const auto bases = private_handoff_bases(root.prepared());
    const auto paths = relation::OOCCleanupTransaction::paths_for(bases[0]);
    auto prepared = prepare_cleanup_conversion_capsule(root);
    CleanupConversionStopContext context{
        .target = relation::ooc_cleanup_detail::
            OOCPrivateHandoffCleanupIntentPublicationFaultPointV2::IntentCanonicalPromoted,
    };

    auto interrupted = cleanup_authority::trusted_test::
        execute_distributed_sieve_worker_cleanup_intent_conversion_v1_with_hooks(
            std::move(*prepared.capsule),
            relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentPublicationTestHooksV2{
                .stop_after = stop_cleanup_conversion_after,
                .context = &context,
            });
    CHECK(context.invoked);
    CHECK(interrupted);
    CHECK(!interrupted.retryable_capsule.has_value());
    CHECK(interrupted.root_continuation.has_value());
    CHECK(interrupted.root_continuation->valid());
    CHECK(interrupted.publication.canonical_reconciliation_required());
    CHECK(interrupted.publication.result.status == relation::OOCCleanupStatus::Interrupted);
    CHECK(interrupted.publication.result.stage == relation::OOCCleanupStage::None);
    CHECK(interrupted.publication.result.native_error ==
          std::make_error_code(std::errc::operation_canceled));
    CHECK(interrupted.diagnostic.phase ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionExecutePhaseV1::complete);
    CHECK(interrupted.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::
              canonical_reconciliation_required);
    CHECK(!interrupted.diagnostic.capsule_retained);
    CHECK(interrupted.diagnostic.root_continuation_retained);
    CHECK(!interrupted.diagnostic.cold_reopen_required);
    CHECK(std::filesystem::exists(paths.intent_path));
    CHECK(!std::filesystem::exists(paths.intent_pending_path));
    require_all_base_locks_free(bases);
    require_wave_lock_busy(root.prepared());

    auto reminted = mint_cleanup_receipt(*interrupted.root_continuation, 0U);
    CHECK(!reminted.minted->receipt.spent());
    CHECK(std::filesystem::exists(paths.intent_path));
    CHECK(!std::filesystem::exists(paths.intent_pending_path));
    require_all_base_locks_free(bases);
    require_wave_lock_busy(root.prepared());
}

void test_forked_cleanup_capsule_destruction_preserves_parent_authority() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-capsule-fork");
    const auto bases = private_handoff_bases(root.prepared());
    auto prepared = prepare_cleanup_conversion_capsule(root);

    const pid_t child = ::fork();
    if (child < 0) {
        throw std::system_error(errno, std::generic_category(), "fork worker-cleanup capsule test");
    }
    if (child == 0) {
        bool rejected = false;
        {
            auto result =
                cleanup_authority::execute_distributed_sieve_worker_cleanup_intent_conversion_v1(
                    std::move(*prepared.capsule));
            rejected =
                !result && !result.retryable_capsule.has_value() &&
                !result.root_continuation.has_value() &&
                result.diagnostic.phase ==
                    cleanup_authority::DistributedSieveWorkerCleanupIntentConversionExecutePhaseV1::
                        capsule_validation &&
                result.diagnostic.status ==
                    cleanup_authority::
                        DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::
                            process_mismatch &&
                result.diagnostic.native_error ==
                    std::make_error_code(std::errc::no_such_process) &&
                !result.diagnostic.capsule_retained &&
                !result.diagnostic.root_continuation_retained &&
                result.diagnostic.cold_reopen_required &&
                result.publication.result.status == relation::OOCCleanupStatus::UnexpectedFailure &&
                result.publication.result.stage == relation::OOCCleanupStage::None &&
                !result.publication.result.native_error &&
                result.publication.disposition ==
                    relation::ooc_cleanup_detail::
                        OOCPrivateHandoffCleanupIntentPublicationDispositionV2::Failed &&
                !result.publication.evidence.has_value() &&
                !result.publication.intent_published() &&
                !result.publication.capabilities_retained() &&
                !result.publication.canonical_reconciliation_required() &&
                !result.publication.capabilities_spent();
        }
        prepared.capsule.reset();
        ::_exit(rejected ? FORK_REJECTED_EXIT : EXIT_FAILURE);
    }
    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "wait for worker-cleanup capsule child");
    }
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == FORK_REJECTED_EXIT);
    CHECK(prepared.capsule->valid());
    require_exact_active_worker_base_lock(bases, 0U);
    require_wave_lock_busy(root.prepared());

    auto published =
        cleanup_authority::execute_distributed_sieve_worker_cleanup_intent_conversion_v1(
            std::move(*prepared.capsule));
    CHECK(published);
    CHECK(published.root_continuation.has_value());
    CHECK(published.publication.intent_published());
    CHECK(published.publication.result.status == relation::OOCCleanupStatus::RecoveryRequired);
    CHECK(published.publication.result.stage == relation::OOCCleanupStage::IntentDurable);
    CHECK(published.publication.result.native_error ==
          std::make_error_code(std::errc::protocol_error));
    CHECK(published.diagnostic.phase ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionExecutePhaseV1::complete);
    CHECK(published.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::
              intent_published);
    CHECK(!published.diagnostic.capsule_retained);
    CHECK(published.diagnostic.root_continuation_retained);
    CHECK(!published.diagnostic.cold_reopen_required);
}

void test_cleanup_relation_route_live_unconverted_reaches_completion_ready() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-route-live");
    const auto bases = private_handoff_bases(root.prepared());
    const auto merged_snapshot = capture_merged_corpus(bases[2]);
    const auto authorization_snapshot = fixture::snapshot_leaf(root.authorization_path());
    require_active_cleanup_completion_absent(root);

    auto driven = drive_cleanup_to_completion_ready(root.take_admission(), 0U);

    CHECK(driven.completion_ready->valid());
    CHECK(
        driven.diagnostic.observed_prefix ==
        std::optional{
            relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupPrefixStateV2::LiveUnconverted});
    require_worker_cleanup_namespace_absent(bases[0]);
    CHECK(fixture::snapshot_leaf(root.authorization_path()) == authorization_snapshot);
    require_active_cleanup_completion_absent(root);
    require_merged_corpus(bases[2], merged_snapshot);
    require_all_base_locks_free(bases);
    require_wave_lock_busy(root.prepared());
}

void test_cleanup_relation_route_busy_observer_returns_retryable_root() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-route-busy-observer");
    const auto bases = private_handoff_bases(root.prepared());
    const auto worker_snapshots = capture_worker_snapshots(root.prepared());
    const auto merged_snapshot = capture_merged_corpus(bases[2]);
    const auto authorization_snapshot = fixture::snapshot_leaf(root.authorization_path());
    const auto paths = relation::OOCCleanupTransaction::paths_for(bases[0]);
    require_active_cleanup_completion_absent(root);

    auto adopted = relation::OOCCleanupTransaction::adopt_private_handoff(bases[0]);
    CHECK(adopted.adopted());
    CHECK(adopted.adoption.has_value());
    std::optional<relation::OOCPrivateHandoffReader> competing_reader;
    competing_reader.emplace(std::move(*adopted.adoption));
    CHECK(competing_reader->valid());

    auto blocked = cleanup_authority::trusted_test::
        drive_distributed_sieve_worker_cleanup_to_completion_ready_v1_with_root(
            root.take_admission());

    CHECK(blocked);
    CHECK(blocked.retryable_root.has_value());
    CHECK(!blocked.retryable_intent_conversion.has_value());
    CHECK(!blocked.completion_ready.has_value());
    CHECK(blocked.retryable_root->valid());
    CHECK(blocked.diagnostic.disposition ==
          cleanup_authority::DistributedSieveWorkerCleanupCompletionPreparationDispositionV1::
              retryable_root);
    CHECK(blocked.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupCompletionPreparationStatusV1::
              retryable_root);
    CHECK(blocked.diagnostic.phase ==
          cleanup_authority::DistributedSieveWorkerCleanupCompletionPreparationPhaseV1::
              relation_prefix_observation);
    CHECK(blocked.diagnostic.relation.status == relation::OOCCleanupStatus::Busy);
    CHECK(!blocked.diagnostic.observed_prefix.has_value());
    CHECK(competing_reader->valid());
    CHECK(!std::filesystem::exists(paths.intent_path));
    CHECK(!std::filesystem::exists(paths.intent_pending_path));
    CHECK(!std::filesystem::exists(paths.staged_path));
    CHECK(!std::filesystem::exists(paths.staged_pending_path));
    require_worker_snapshots(root.prepared(), worker_snapshots);
    CHECK(fixture::snapshot_leaf(root.authorization_path()) == authorization_snapshot);
    require_active_cleanup_completion_absent(root);
    require_merged_corpus(bases[2], merged_snapshot);
    require_exact_active_worker_base_lock(bases, 0U);
    require_wave_lock_busy(root.prepared());

    competing_reader.reset();
    require_all_base_locks_free(bases);
    auto completed = drive_cleanup_to_completion_ready(std::move(*blocked.retryable_root), 0U);

    CHECK(completed.completion_ready->valid());
    CHECK(
        completed.diagnostic.observed_prefix ==
        std::optional{
            relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupPrefixStateV2::LiveUnconverted});
    require_worker_cleanup_namespace_absent(bases[0]);
    CHECK(fixture::snapshot_leaf(root.authorization_path()) == authorization_snapshot);
    require_active_cleanup_completion_absent(root);
    require_merged_corpus(bases[2], merged_snapshot);
    require_all_base_locks_free(bases);
    require_wave_lock_busy(root.prepared());
}

void test_cleanup_relation_route_existing_canonical_intent_reaches_completion_ready() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-route-canonical");
    const auto bases = private_handoff_bases(root.prepared());
    const auto merged_snapshot = capture_merged_corpus(bases[2]);
    const auto authorization_snapshot = fixture::snapshot_leaf(root.authorization_path());
    const auto paths = relation::OOCCleanupTransaction::paths_for(bases[0]);
    auto continuation = publish_canonical_cleanup_intent(root);
    CHECK(std::filesystem::exists(paths.intent_path));
    CHECK(!std::filesystem::exists(paths.intent_pending_path));

    auto driven = drive_cleanup_to_completion_ready(std::move(continuation), 0U);

    CHECK(driven.completion_ready->valid());
    CHECK(driven.diagnostic.observed_prefix ==
          std::optional{relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupPrefixStateV2::
                            IntentCanonicalWithHandoff});
    require_worker_cleanup_namespace_absent(bases[0]);
    CHECK(fixture::snapshot_leaf(root.authorization_path()) == authorization_snapshot);
    require_active_cleanup_completion_absent(root);
    require_merged_corpus(bases[2], merged_snapshot);
    require_all_base_locks_free(bases);
    require_wave_lock_busy(root.prepared());
}

void test_cleanup_relation_route_pending_and_duplicate_pending_reach_completion_ready() {
    {
        CanonicalWorkerCleanupRoot root("worker-cleanup-route-pending");
        const auto bases = private_handoff_bases(root.prepared());
        const auto merged_snapshot = capture_merged_corpus(bases[2]);
        const auto authorization_snapshot = fixture::snapshot_leaf(root.authorization_path());
        const auto paths = relation::OOCCleanupTransaction::paths_for(bases[0]);
        materialize_pending_cleanup_intent_and_cold_reopen(root);
        CHECK(!std::filesystem::exists(paths.intent_path));
        CHECK(std::filesystem::exists(paths.intent_pending_path));

        auto driven = drive_cleanup_to_completion_ready(root.take_admission(), 0U);

        CHECK(driven.completion_ready->valid());
        CHECK(driven.diagnostic.observed_prefix ==
              std::optional{relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupPrefixStateV2::
                                IntentPendingOnly});
        require_worker_cleanup_namespace_absent(bases[0]);
        CHECK(fixture::snapshot_leaf(root.authorization_path()) == authorization_snapshot);
        require_active_cleanup_completion_absent(root);
        require_merged_corpus(bases[2], merged_snapshot);
        require_all_base_locks_free(bases);
        require_wave_lock_busy(root.prepared());
    }

    {
        CanonicalWorkerCleanupRoot root("worker-cleanup-route-duplicate-pending");
        const auto bases = private_handoff_bases(root.prepared());
        const auto merged_snapshot = capture_merged_corpus(bases[2]);
        const auto authorization_snapshot = fixture::snapshot_leaf(root.authorization_path());
        const auto paths = relation::OOCCleanupTransaction::paths_for(bases[0]);
        auto continuation = publish_canonical_cleanup_intent_with_duplicate_pending(root);
        CHECK(fixture::read_file_bytes(paths.intent_path) ==
              fixture::read_file_bytes(paths.intent_pending_path));
        CHECK(fixture::snapshot_leaf(paths.intent_path).identity !=
              fixture::snapshot_leaf(paths.intent_pending_path).identity);

        auto driven = drive_cleanup_to_completion_ready(std::move(continuation), 0U);

        CHECK(driven.completion_ready->valid());
        CHECK(driven.diagnostic.observed_prefix ==
              std::optional{relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupPrefixStateV2::
                                IntentCanonicalAndPending});
        require_worker_cleanup_namespace_absent(bases[0]);
        CHECK(fixture::snapshot_leaf(root.authorization_path()) == authorization_snapshot);
        require_active_cleanup_completion_absent(root);
        require_merged_corpus(bases[2], merged_snapshot);
        require_all_base_locks_free(bases);
        require_wave_lock_busy(root.prepared());
    }
}

void test_cleanup_relation_route_markerless_absence_reaches_completion_ready() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-route-markerless-absent");
    const auto bases = private_handoff_bases(root.prepared());
    const auto merged_snapshot = capture_merged_corpus(bases[2]);
    const auto authorization_snapshot = fixture::snapshot_leaf(root.authorization_path());
    const auto worker_snapshot = root.prepared().worker_snapshot(0);
    remove_worker_private_namespace(root.prepared().root(), worker_snapshot);
    require_worker_cleanup_namespace_absent(bases[0]);
    require_active_cleanup_completion_absent(root);
    root.release_and_cold_reopen();

    auto driven = drive_cleanup_to_completion_ready(root.take_admission(), 0U);

    CHECK(driven.completion_ready->valid());
    CHECK(
        driven.diagnostic.observed_prefix ==
        std::optional{relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupPrefixStateV2::Absent});
    require_worker_cleanup_namespace_absent(bases[0]);
    CHECK(fixture::snapshot_leaf(root.authorization_path()) == authorization_snapshot);
    require_active_cleanup_completion_absent(root);
    require_merged_corpus(bases[2], merged_snapshot);
    require_all_base_locks_free(bases);
    require_wave_lock_busy(root.prepared());
}

void test_cleanup_relation_route_active_ordinal_retains_prefix_and_merged_corpus() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-route-active-ordinal", 1);
    const auto bases = private_handoff_bases(root.prepared());
    const auto merged_snapshot = capture_merged_corpus(bases[2]);
    const auto authorization_snapshot = fixture::snapshot_leaf(root.authorization_path());
    const auto prior_completion_snapshot = fixture::snapshot_leaf(root.completion_path(0));
    require_active_cleanup_completion_absent(root);

    auto driven = drive_cleanup_to_completion_ready(root.take_admission(), 1U);

    CHECK(driven.completion_ready->valid());
    CHECK(
        driven.diagnostic.observed_prefix ==
        std::optional{
            relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupPrefixStateV2::LiveUnconverted});
    CHECK(driven.completion_ready->manifest_order_ordinal() == 1U);
    require_worker_cleanup_namespace_absent(bases[0]);
    require_worker_cleanup_namespace_absent(bases[1]);
    CHECK(fixture::snapshot_leaf(root.authorization_path()) == authorization_snapshot);
    CHECK(fixture::snapshot_leaf(root.completion_path(0)) == prior_completion_snapshot);
    require_active_cleanup_completion_absent(root);
    require_merged_corpus(bases[2], merged_snapshot);
    require_all_base_locks_free(bases);
    require_wave_lock_busy(root.prepared());
}

void test_completion_ready_capsule_is_process_bound_across_fork() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-route-completion-ready-fork");
    const auto bases = private_handoff_bases(root.prepared());
    auto driven = drive_cleanup_to_completion_ready(root.take_admission(), 0U);
    CHECK(driven.completion_ready->valid());

    const pid_t child = ::fork();
    if (child < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "fork worker-cleanup completion-ready capsule test");
    }
    if (child == 0) {
        ::_exit(!driven.completion_ready->valid() ? COMPLETION_READY_FORK_REJECTED_EXIT
                                                  : EXIT_FAILURE);
    }

    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "wait for worker-cleanup completion-ready capsule child");
    }
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == COMPLETION_READY_FORK_REJECTED_EXIT);
    CHECK(driven.completion_ready->valid());
    require_worker_cleanup_namespace_absent(bases[0]);
    require_all_base_locks_free(bases);
    require_wave_lock_busy(root.prepared());
}

void test_completion_publication_creates_and_advances_one_frontier() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-completion-publish-fresh");
    const auto bases = private_handoff_bases(root.prepared());
    const auto merged_snapshot = capture_merged_corpus(bases[2]);
    const auto authorization_snapshot = fixture::snapshot_leaf(root.authorization_path());
    const auto next_names = wave::distributed_sieve_worker_cleanup_record_names_v1(1U);
    CHECK(next_names.has_value());
    auto driven = drive_cleanup_to_completion_ready(root.take_admission(), 0U);
    const auto root_entries_before = capture_root_entry_names(root.prepared().root());
    require_active_cleanup_completion_absent(root);
    require_wave_lock_busy(root.prepared());

    auto published = cleanup_authority::publish_distributed_sieve_worker_cleanup_completion_v1(
        std::move(*driven.completion_ready));

    require_completion_publication_success(published, 0U,
                                           durable_record::RecordPublishDisposition::created);
    CHECK(std::filesystem::exists(root.completion_path(0U)));
    CHECK(!std::filesystem::exists(root.completion_pending_path(0U)));
    const auto completion = read_cleanup_completion(root.completion_path(0U));
    CHECK(completion.self_digest == published.published_continuation->completion_digest());
    CHECK(fixture::snapshot_leaf(root.authorization_path()) == authorization_snapshot);
    CHECK(!std::filesystem::exists(root.prepared().root() /
                                   next_names->authorization_canonical_record_leaf));
    CHECK(!std::filesystem::exists(root.prepared().root() /
                                   next_names->authorization_pending_record_leaf));
    CHECK(!std::filesystem::exists(root.prepared().root() /
                                   next_names->completion_canonical_record_leaf));
    CHECK(!std::filesystem::exists(root.prepared().root() /
                                   next_names->completion_pending_record_leaf));
    auto expected_root_entries = root_entries_before;
    expected_root_entries.push_back(root.completion_path(0U).filename().string());
    std::sort(expected_root_entries.begin(), expected_root_entries.end());
    CHECK(capture_root_entry_names(root.prepared().root()) == expected_root_entries);
    require_merged_corpus(bases[2], merged_snapshot);
    require_all_base_locks_free(bases);
    require_wave_lock_busy(root.prepared());

    published.published_continuation.reset();
    auto reopened = open_cleanup_root(root.prepared(), "cold-open fresh completion successor");
    require_completed_cleanup_prefix(reopened, 1U, 1U);
    CHECK(reopened.reader().count() == root.prepared().expected_rows().size());
    auto remint =
        cleanup_authority::mint_distributed_sieve_worker_cleanup_authorization_receipt_v1(reopened);
    CHECK(!remint);
    CHECK(!remint.minted.has_value());
    CHECK(remint.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupReceiptMintStatusV1::
              authorization_not_canonical);
    require_wave_lock_busy(root.prepared());
}

void test_completion_publication_recovers_pending_canonical_and_identical_dual() {
    struct Case final {
        InjectedCompletionPrefix shape;
        std::string_view label;
        durable_record::RecordPublishDisposition expected_disposition;
    };
    constexpr std::array cases{
        Case{InjectedCompletionPrefix::pending_only, "pending",
             durable_record::RecordPublishDisposition::recovered_pending},
        Case{InjectedCompletionPrefix::canonical_only, "canonical",
             durable_record::RecordPublishDisposition::confirmed_existing},
        Case{InjectedCompletionPrefix::identical_dual, "dual",
             durable_record::RecordPublishDisposition::confirmed_existing},
    };

    for (const auto& test_case : cases) {
        CanonicalWorkerCleanupRoot root(std::string("worker-cleanup-completion-recover-") +
                                        std::string(test_case.label));
        const auto bases = private_handoff_bases(root.prepared());
        const auto merged_snapshot = capture_merged_corpus(bases[2]);
        const auto authorization_snapshot = fixture::snapshot_leaf(root.authorization_path());
        const auto worker_snapshot = root.prepared().worker_snapshot(0U);
        {
            auto released = root.take_admission();
            CHECK(released.valid());
        }
        remove_worker_private_namespace(root.prepared().root(), worker_snapshot);
        require_worker_cleanup_namespace_absent(bases[0]);
        const auto injected = inject_active_completion_prefix(root, test_case.shape);
        CHECK(injected.pending.has_value() ==
              (test_case.shape != InjectedCompletionPrefix::canonical_only));
        CHECK(injected.canonical.has_value() ==
              (test_case.shape != InjectedCompletionPrefix::pending_only));
        auto recovery_root =
            open_cleanup_root(root.prepared(), "cold-open injected completion prefix");

        auto recovered =
            cleanup_authority::reconcile_distributed_sieve_worker_cleanup_completion_v1(
                std::move(recovery_root));

        require_completion_publication_success(recovered, 0U, test_case.expected_disposition);
        const auto final_canonical = fixture::snapshot_leaf(root.completion_path(0U));
        CHECK(!std::filesystem::exists(root.completion_pending_path(0U)));
        if (test_case.shape == InjectedCompletionPrefix::pending_only) {
            CHECK(final_canonical.identity == injected.pending->identity);
            CHECK(final_canonical.bytes == injected.pending->bytes);
        } else {
            CHECK(final_canonical == *injected.canonical);
        }
        const auto completion = read_cleanup_completion(root.completion_path(0U));
        CHECK(completion.self_digest == recovered.published_continuation->completion_digest());
        CHECK(fixture::snapshot_leaf(root.authorization_path()) == authorization_snapshot);
        require_merged_corpus(bases[2], merged_snapshot);
        require_all_base_locks_free(bases);
        require_wave_lock_busy(root.prepared());

        recovered.published_continuation.reset();
        auto reopened = open_cleanup_root(root.prepared(), "cold-open recovered completion");
        require_completed_cleanup_prefix(reopened, 1U, 1U);
        CHECK(reopened.reader().count() == root.prepared().expected_rows().size());
    }
}

void test_completion_publication_fault_prefixes_cold_replay() {
    constexpr std::array fresh_faults{
        CleanupCompletionPublicationFault::PendingDurable,
        CleanupCompletionPublicationFault::CanonicalPromoted,
        CleanupCompletionPublicationFault::CanonicalDurable,
    };
    using Status = cleanup_authority::DistributedSieveWorkerCleanupCompletionPublicationStatusV1;
    using Disposition =
        cleanup_authority::DistributedSieveWorkerCleanupCompletionPublicationDispositionV1;

    {
        CanonicalWorkerCleanupRoot root("worker-cleanup-completion-before-spend");
        const auto bases = private_handoff_bases(root.prepared());
        const auto merged_snapshot = capture_merged_corpus(bases[2]);
        const auto authorization_snapshot = fixture::snapshot_leaf(root.authorization_path());
        auto driven = drive_cleanup_to_completion_ready(root.take_admission(), 0U);
        CompletionPublicationStopContext context{
            .target = CleanupCompletionPublicationFault::FreshBeforeReceiptSpend,
        };

        auto retryable = cleanup_authority::trusted_test::
            publish_distributed_sieve_worker_cleanup_completion_v1_with_hooks(
                std::move(*driven.completion_ready),
                CleanupCompletionPublicationHooks{
                    .stop_after = stop_completion_publication_after,
                    .context = &context,
                });

        CHECK(retryable);
        CHECK(retryable.retryable_completion_ready.has_value());
        CHECK(retryable.retryable_completion_ready->valid());
        CHECK(!retryable.retryable_recovery_root.has_value());
        CHECK(!retryable.published_continuation.has_value());
        CHECK(retryable.diagnostic.status == Status::retryable_completion_ready);
        CHECK(retryable.diagnostic.disposition == Disposition::retryable_completion_ready);
        CHECK(retryable.diagnostic.last_fault_point ==
              CleanupCompletionPublicationFault::FreshBeforeReceiptSpend);
        CHECK(!retryable.diagnostic.authority_spent);
        CHECK(!retryable.diagnostic.publication_started);
        CHECK(!retryable.diagnostic.publication_status.has_value());
        CHECK(!retryable.diagnostic.publication_disposition.has_value());
        require_active_cleanup_completion_absent(root);
        CHECK(fixture::snapshot_leaf(root.authorization_path()) == authorization_snapshot);
        require_merged_corpus(bases[2], merged_snapshot);
        require_all_base_locks_free(bases);
        require_wave_lock_busy(root.prepared());

        auto published = cleanup_authority::publish_distributed_sieve_worker_cleanup_completion_v1(
            std::move(*retryable.retryable_completion_ready));
        require_completion_publication_success(published, 0U,
                                               durable_record::RecordPublishDisposition::created);
        require_merged_corpus(bases[2], merged_snapshot);
    }

    {
        CanonicalWorkerCleanupRoot root("worker-cleanup-completion-after-spend");
        const auto bases = private_handoff_bases(root.prepared());
        const auto merged_snapshot = capture_merged_corpus(bases[2]);
        const auto authorization_snapshot = fixture::snapshot_leaf(root.authorization_path());
        auto driven = drive_cleanup_to_completion_ready(root.take_admission(), 0U);
        CompletionPublicationStopContext context{
            .target = CleanupCompletionPublicationFault::FreshAfterReceiptSpend,
        };

        auto interrupted = cleanup_authority::trusted_test::
            publish_distributed_sieve_worker_cleanup_completion_v1_with_hooks(
                std::move(*driven.completion_ready),
                CleanupCompletionPublicationHooks{
                    .stop_after = stop_completion_publication_after,
                    .context = &context,
                });

        CHECK(!interrupted);
        CHECK(!interrupted.retryable_completion_ready.has_value());
        CHECK(!interrupted.retryable_recovery_root.has_value());
        CHECK(!interrupted.published_continuation.has_value());
        CHECK(interrupted.diagnostic.status == Status::test_interrupted);
        CHECK(interrupted.diagnostic.disposition == Disposition::cold_reopen_required);
        CHECK(interrupted.diagnostic.last_fault_point ==
              CleanupCompletionPublicationFault::FreshAfterReceiptSpend);
        CHECK(interrupted.diagnostic.authority_spent);
        CHECK(!interrupted.diagnostic.publication_started);
        CHECK(!interrupted.diagnostic.publication_status.has_value());
        CHECK(!interrupted.diagnostic.publication_disposition.has_value());
        require_active_cleanup_completion_absent(root);
        CHECK(fixture::snapshot_leaf(root.authorization_path()) == authorization_snapshot);
        require_merged_corpus(bases[2], merged_snapshot);
        require_all_base_locks_free(bases);

        const auto root_entries_before_reconcile = capture_root_entry_names(root.prepared().root());
        auto reopened = open_cleanup_root(root.prepared(), "cold-open post-spend A-only root");
        CompletionPublicationStopContext recovery_context{
            .target = CleanupCompletionPublicationFault::RecoveryBeforePublication,
        };
        auto rejected = cleanup_authority::trusted_test::
            reconcile_distributed_sieve_worker_cleanup_completion_v1_with_hooks(
                std::move(reopened), CleanupCompletionPublicationHooks{
                                         .stop_after = stop_completion_publication_after,
                                         .context = &recovery_context,
                                     });
        CHECK(rejected);
        CHECK(!rejected.retryable_completion_ready.has_value());
        CHECK(rejected.retryable_recovery_root.has_value());
        CHECK(rejected.retryable_recovery_root->valid());
        CHECK(!rejected.published_continuation.has_value());
        CHECK(rejected.diagnostic.status == Status::completion_target_missing);
        CHECK(rejected.diagnostic.disposition == Disposition::retryable_recovery_root);
        CHECK(!rejected.diagnostic.authority_spent);
        CHECK(!rejected.diagnostic.publication_started);
        CHECK(!rejected.diagnostic.publication_status.has_value());
        CHECK(!rejected.diagnostic.publication_disposition.has_value());
        CHECK(!rejected.diagnostic.last_fault_point.has_value());
        CHECK(std::none_of(recovery_context.observed.begin(), recovery_context.observed.end(),
                           [](bool observed) { return observed; }));
        CHECK(capture_root_entry_names(root.prepared().root()) == root_entries_before_reconcile);
        require_active_cleanup_completion_absent(root);
        CHECK(fixture::snapshot_leaf(root.authorization_path()) == authorization_snapshot);
        require_merged_corpus(bases[2], merged_snapshot);
        require_all_base_locks_free(bases);
        require_wave_lock_busy(root.prepared());

        auto redriven =
            drive_cleanup_to_completion_ready(std::move(*rejected.retryable_recovery_root), 0U);
        CHECK(redriven.diagnostic.observed_prefix ==
              std::optional{
                  relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupPrefixStateV2::Absent});
        auto published = cleanup_authority::publish_distributed_sieve_worker_cleanup_completion_v1(
            std::move(*redriven.completion_ready));
        require_completion_publication_success(published, 0U,
                                               durable_record::RecordPublishDisposition::created);
        require_merged_corpus(bases[2], merged_snapshot);
    }

    for (const auto fault : fresh_faults) {
        CanonicalWorkerCleanupRoot root("worker-cleanup-completion-fault-" +
                                        std::to_string(static_cast<std::size_t>(fault)));
        const auto bases = private_handoff_bases(root.prepared());
        const auto merged_snapshot = capture_merged_corpus(bases[2]);
        auto driven = drive_cleanup_to_completion_ready(root.take_admission(), 0U);
        CompletionPublicationStopContext context{.target = fault};

        auto interrupted = cleanup_authority::trusted_test::
            publish_distributed_sieve_worker_cleanup_completion_v1_with_hooks(
                std::move(*driven.completion_ready),
                CleanupCompletionPublicationHooks{
                    .stop_after = stop_completion_publication_after,
                    .context = &context,
                });

        CHECK(!interrupted);
        CHECK(!interrupted.retryable_completion_ready.has_value());
        CHECK(!interrupted.retryable_recovery_root.has_value());
        CHECK(!interrupted.published_continuation.has_value());
        CHECK(interrupted.diagnostic.status == Status::test_interrupted);
        CHECK(interrupted.diagnostic.disposition == Disposition::cold_reopen_required);
        CHECK(interrupted.diagnostic.publication_status ==
              durable_record::RecordPublishStatus::interrupted);
        CHECK(interrupted.diagnostic.publication_disposition ==
              durable_record::RecordPublishDisposition::created);
        CHECK(interrupted.diagnostic.last_fault_point == fault);
        CHECK(interrupted.diagnostic.authority_spent);
        CHECK(interrupted.diagnostic.publication_started);
        CHECK(context.observed[static_cast<std::size_t>(fault)]);
        const bool pending_only = fault == CleanupCompletionPublicationFault::PendingDurable;
        CHECK(std::filesystem::exists(root.completion_pending_path(0U)) == pending_only);
        CHECK(std::filesystem::exists(root.completion_path(0U)) == !pending_only);
        require_merged_corpus(bases[2], merged_snapshot);
        require_all_base_locks_free(bases);

        auto recovery_root =
            open_cleanup_root(root.prepared(), "cold-open durable completion fault prefix");
        auto recovered =
            cleanup_authority::reconcile_distributed_sieve_worker_cleanup_completion_v1(
                std::move(recovery_root));
        require_completion_publication_success(
            recovered, 0U,
            pending_only ? durable_record::RecordPublishDisposition::recovered_pending
                         : durable_record::RecordPublishDisposition::confirmed_existing);
        CHECK(!std::filesystem::exists(root.completion_pending_path(0U)));
        require_merged_corpus(bases[2], merged_snapshot);
    }

    CanonicalWorkerCleanupRoot dual_root("worker-cleanup-completion-fault-dual");
    const auto dual_bases = private_handoff_bases(dual_root.prepared());
    const auto dual_merged_snapshot = capture_merged_corpus(dual_bases[2]);
    const auto worker_snapshot = dual_root.prepared().worker_snapshot(0U);
    {
        auto released = dual_root.take_admission();
        CHECK(released.valid());
    }
    remove_worker_private_namespace(dual_root.prepared().root(), worker_snapshot);
    const auto injected =
        inject_active_completion_prefix(dual_root, InjectedCompletionPrefix::identical_dual);
    auto recovery_root = open_cleanup_root(dual_root.prepared(), "cold-open dual fault prefix");
    CompletionPublicationStopContext context{
        .target = CleanupCompletionPublicationFault::CanonicalDurable,
    };
    auto interrupted = cleanup_authority::trusted_test::
        reconcile_distributed_sieve_worker_cleanup_completion_v1_with_hooks(
            std::move(recovery_root), CleanupCompletionPublicationHooks{
                                          .stop_after = stop_completion_publication_after,
                                          .context = &context,
                                      });
    CHECK(!interrupted);
    CHECK(interrupted.diagnostic.status == Status::test_interrupted);
    CHECK(interrupted.diagnostic.disposition == Disposition::cold_reopen_required);
    CHECK(interrupted.diagnostic.publication_status ==
          durable_record::RecordPublishStatus::interrupted);
    CHECK(interrupted.diagnostic.publication_disposition ==
          durable_record::RecordPublishDisposition::confirmed_existing);
    CHECK(std::filesystem::exists(dual_root.completion_path(0U)));
    CHECK(std::filesystem::exists(dual_root.completion_pending_path(0U)));
    CHECK(fixture::snapshot_leaf(dual_root.completion_path(0U)) == *injected.canonical);
    CHECK(fixture::snapshot_leaf(dual_root.completion_pending_path(0U)) == *injected.pending);

    auto replay_root = open_cleanup_root(dual_root.prepared(), "cold-replay dual fault prefix");
    auto replayed = cleanup_authority::reconcile_distributed_sieve_worker_cleanup_completion_v1(
        std::move(replay_root));
    require_completion_publication_success(
        replayed, 0U, durable_record::RecordPublishDisposition::confirmed_existing);
    CHECK(!std::filesystem::exists(dual_root.completion_pending_path(0U)));
    require_merged_corpus(dual_bases[2], dual_merged_snapshot);
}

void test_completion_publication_rejects_replacement_and_successor_drift() {
    constexpr std::array mutations{
        CompletionSuccessorMutation::replace_canonical,
        CompletionSuccessorMutation::add_identical_pending,
    };
    using Status = cleanup_authority::DistributedSieveWorkerCleanupCompletionPublicationStatusV1;
    using Disposition =
        cleanup_authority::DistributedSieveWorkerCleanupCompletionPublicationDispositionV1;

    for (const auto mutation : mutations) {
        CanonicalWorkerCleanupRoot root("worker-cleanup-completion-successor-mutation-" +
                                        std::to_string(static_cast<std::size_t>(mutation)));
        const auto bases = private_handoff_bases(root.prepared());
        const auto merged_snapshot = capture_merged_corpus(bases[2]);
        const auto authorization_snapshot = fixture::snapshot_leaf(root.authorization_path());
        auto driven = drive_cleanup_to_completion_ready(root.take_admission(), 0U);
        CompletionSuccessorMutationContext context{
            .mutation = mutation,
            .canonical = root.completion_path(0U),
            .pending = root.completion_pending_path(0U),
            .saved = root.prepared().root().parent_path() /
                     (root.prepared().root().filename().string() + "-saved-completion"),
        };

        auto rejected = cleanup_authority::trusted_test::
            publish_distributed_sieve_worker_cleanup_completion_v1_with_hooks(
                std::move(*driven.completion_ready), CleanupCompletionPublicationHooks{
                                                         .stop_after = mutate_completion_successor,
                                                         .context = &context,
                                                     });

        CHECK(context.invoked);
        CHECK(!context.failure);
        CHECK(!rejected);
        CHECK(!rejected.retryable_completion_ready.has_value());
        CHECK(!rejected.retryable_recovery_root.has_value());
        CHECK(!rejected.published_continuation.has_value());
        CHECK(rejected.diagnostic.status == Status::successor_mismatch);
        CHECK(rejected.diagnostic.disposition == Disposition::cold_reopen_required);
        CHECK(rejected.diagnostic.publication_status ==
              durable_record::RecordPublishStatus::durable);
        CHECK(rejected.diagnostic.publication_disposition ==
              durable_record::RecordPublishDisposition::created);
        CHECK(rejected.diagnostic.authority_spent);
        CHECK(rejected.diagnostic.publication_started);
        CHECK(fixture::snapshot_leaf(root.authorization_path()) == authorization_snapshot);
        require_merged_corpus(bases[2], merged_snapshot);
        require_all_base_locks_free(bases);
        if (mutation == CompletionSuccessorMutation::replace_canonical) {
            CHECK(std::filesystem::exists(context.saved));
            CHECK(fixture::read_file_bytes(context.saved) ==
                  fixture::read_file_bytes(context.canonical));
            CHECK(fixture::native_identity(context.saved) !=
                  fixture::native_identity(context.canonical));
            CHECK(!std::filesystem::exists(context.pending));
        } else {
            CHECK(std::filesystem::exists(context.pending));
            CHECK(fixture::read_file_bytes(context.pending) ==
                  fixture::read_file_bytes(context.canonical));
            CHECK(fixture::native_identity(context.pending) !=
                  fixture::native_identity(context.canonical));
        }

        auto recovery_root =
            open_cleanup_root(root.prepared(), "cold-open stable successor mutation");
        auto recovered =
            cleanup_authority::reconcile_distributed_sieve_worker_cleanup_completion_v1(
                std::move(recovery_root));
        require_completion_publication_success(
            recovered, 0U, durable_record::RecordPublishDisposition::confirmed_existing);
        CHECK(!std::filesystem::exists(context.pending));
        require_merged_corpus(bases[2], merged_snapshot);
    }
}

void test_completion_publication_is_process_bound_across_fork() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-completion-publisher-fork");
    const auto bases = private_handoff_bases(root.prepared());
    const auto merged_snapshot = capture_merged_corpus(bases[2]);
    const auto authorization_snapshot = fixture::snapshot_leaf(root.authorization_path());
    auto driven = drive_cleanup_to_completion_ready(root.take_admission(), 0U);
    CHECK(driven.completion_ready->valid());
    require_active_cleanup_completion_absent(root);

    const pid_t child = ::fork();
    if (child < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "fork worker-cleanup completion publisher test");
    }
    if (child == 0) {
        using Phase = cleanup_authority::DistributedSieveWorkerCleanupCompletionPublicationPhaseV1;
        using Status =
            cleanup_authority::DistributedSieveWorkerCleanupCompletionPublicationStatusV1;
        using Disposition =
            cleanup_authority::DistributedSieveWorkerCleanupCompletionPublicationDispositionV1;
        auto rejected = cleanup_authority::publish_distributed_sieve_worker_cleanup_completion_v1(
            std::move(*driven.completion_ready));
        const bool correct =
            !rejected && !rejected.retryable_completion_ready.has_value() &&
            !rejected.retryable_recovery_root.has_value() &&
            !rejected.published_continuation.has_value() &&
            rejected.diagnostic.phase == Phase::input_validation &&
            rejected.diagnostic.status == Status::process_mismatch &&
            rejected.diagnostic.disposition == Disposition::cold_reopen_required &&
            rejected.diagnostic.native_error == std::make_error_code(std::errc::no_such_process) &&
            !rejected.diagnostic.authority_spent && !rejected.diagnostic.publication_started &&
            !rejected.diagnostic.publication_status.has_value() &&
            !rejected.diagnostic.publication_disposition.has_value() &&
            !std::filesystem::exists(root.completion_path(0U)) &&
            !std::filesystem::exists(root.completion_pending_path(0U));
        driven.completion_ready.reset();
        ::_exit(correct ? COMPLETION_PUBLICATION_FORK_REJECTED_EXIT : EXIT_FAILURE);
    }

    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "wait for worker-cleanup completion publisher child");
    }
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == COMPLETION_PUBLICATION_FORK_REJECTED_EXIT);
    CHECK(driven.completion_ready->valid());
    require_active_cleanup_completion_absent(root);
    CHECK(fixture::snapshot_leaf(root.authorization_path()) == authorization_snapshot);
    require_merged_corpus(bases[2], merged_snapshot);
    require_all_base_locks_free(bases);
    require_wave_lock_busy(root.prepared());

    auto published = cleanup_authority::publish_distributed_sieve_worker_cleanup_completion_v1(
        std::move(*driven.completion_ready));
    require_completion_publication_success(published, 0U,
                                           durable_record::RecordPublishDisposition::created);
    require_merged_corpus(bases[2], merged_snapshot);
}

void test_completion_publication_nonzero_last_worker_stops_before_m5() {
    CanonicalWorkerCleanupRoot root("worker-cleanup-completion-last-worker", 1U);
    const auto bases = private_handoff_bases(root.prepared());
    const auto merged_snapshot = capture_merged_corpus(bases[2]);
    const auto authorization_0_snapshot = fixture::snapshot_leaf(root.authorization_path(0U));
    const auto completion_0_snapshot = fixture::snapshot_leaf(root.completion_path(0U));
    const auto authorization_1_snapshot = fixture::snapshot_leaf(root.authorization_path(1U));
    auto driven = drive_cleanup_to_completion_ready(root.take_admission(), 1U);
    const auto root_entries_before = capture_root_entry_names(root.prepared().root());

    auto published = cleanup_authority::publish_distributed_sieve_worker_cleanup_completion_v1(
        std::move(*driven.completion_ready));

    require_completion_publication_success(published, 1U,
                                           durable_record::RecordPublishDisposition::created);
    CHECK(fixture::snapshot_leaf(root.authorization_path(0U)) == authorization_0_snapshot);
    CHECK(fixture::snapshot_leaf(root.completion_path(0U)) == completion_0_snapshot);
    CHECK(fixture::snapshot_leaf(root.authorization_path(1U)) == authorization_1_snapshot);
    CHECK(std::filesystem::exists(root.completion_path(1U)));
    CHECK(!std::filesystem::exists(root.completion_pending_path(1U)));
    const auto completion = read_cleanup_completion(root.completion_path(1U));
    CHECK(completion.self_digest == published.published_continuation->completion_digest());
    auto expected_root_entries = root_entries_before;
    expected_root_entries.push_back(root.completion_path(1U).filename().string());
    std::sort(expected_root_entries.begin(), expected_root_entries.end());
    CHECK(capture_root_entry_names(root.prepared().root()) == expected_root_entries);
    require_worker_cleanup_namespace_absent(bases[0]);
    require_worker_cleanup_namespace_absent(bases[1]);
    require_merged_corpus(bases[2], merged_snapshot);
    require_all_base_locks_free(bases);
    require_wave_lock_busy(root.prepared());

    published.published_continuation.reset();
    auto reopened = open_cleanup_root(root.prepared(), "cold-open last worker completion");
    require_completed_cleanup_prefix(reopened, 2U, std::nullopt);
    CHECK(reopened.reader().count() == root.prepared().expected_rows().size());
    require_merged_corpus(bases[2], merged_snapshot);
    require_wave_lock_busy(root.prepared());
}

void run_apple_tests() {
    test_fresh_tail_crosses_one_lock_generation_without_mutation();
    std::cout << "  fresh tail lock-generation bridge and moved replay: PASS\n";
    test_cold_tail_crosses_same_lock_generation_boundary();
    std::cout << "  cold tail lock-generation bridge: PASS\n";
    test_pre_spend_failure_returns_the_unique_retryable_tail();
    std::cout << "  pre-spend failure returns unique retryable tail: PASS\n";
    test_failed_final_revalidation_spends_the_invalid_tail();
    std::cout << "  failed final revalidation spends invalid tail: PASS\n";
    test_exact_anchor_rejects_generation_gap_replacements();
    std::cout << "  root, lock, manifest, and commit generation-gap replacements: PASS\n";
    test_forked_tail_is_rejected_and_parent_remains_authoritative();
    std::cout << "  fork-invalid tail and parent continuity: PASS\n";
    test_cleanup_receipt_requires_one_canonical_active_frontier();
    std::cout << "  cleanup receipt requires canonical active frontier: PASS\n";
    test_cleanup_receipt_single_live_move_release_base_lock_and_fork();
    std::cout << "  cleanup receipt single-live lifecycle, BaseLock, and fork: PASS\n";
    test_cleanup_receipt_spend_retains_claim_and_ignores_private_relation_changes();
    std::cout << "  cleanup receipt spent claim and private relation changes: PASS\n";
    test_cleanup_receipt_sticky_invalidates_same_byte_authorization_replacement();
    std::cout << "  cleanup receipt same-byte replacement sticky invalidation: PASS\n";
    test_cleanup_receipt_anchors_the_complete_completed_prefix();
    std::cout << "  cleanup receipt complete prefix anchoring: PASS\n";
    test_cleanup_receipt_sticky_invalidates_added_cleanup_leaf();
    std::cout << "  cleanup receipt added root leaf sticky invalidation: PASS\n";
    test_cleanup_capsule_prepare_returns_retryable_root_while_receipt_is_live();
    std::cout << "  cleanup capsule retryable root after competing receipt: PASS\n";
    test_cleanup_capsule_selects_active_ordinal_and_retains_one_base_lock_fd();
    std::cout << "  cleanup capsule exact active ordinal and one BaseLock fd: PASS\n";
    test_cleanup_capsule_reader_views_closed_retries_in_place();
    std::cout << "  cleanup capsule ReaderViewsClosed retry in place: PASS\n";
    test_cleanup_capsule_intent_publication_returns_root_and_releases_base_lock();
    std::cout << "  cleanup capsule intent publication root continuation: PASS\n";
    test_cleanup_capsule_canonical_promotion_returns_reconciliation_root();
    std::cout << "  cleanup capsule canonical promotion reconciliation: PASS\n";
    test_forked_cleanup_capsule_destruction_preserves_parent_authority();
    std::cout << "  fork-invalid cleanup capsule preserves parent authority: PASS\n";
    test_cleanup_relation_route_live_unconverted_reaches_completion_ready();
    std::cout << "  cleanup relation route live prefix to completion-ready: PASS\n";
    test_cleanup_relation_route_busy_observer_returns_retryable_root();
    std::cout << "  cleanup relation route Busy retryable root: PASS\n";
    test_cleanup_relation_route_existing_canonical_intent_reaches_completion_ready();
    std::cout << "  cleanup relation route canonical intent to completion-ready: PASS\n";
    test_cleanup_relation_route_pending_and_duplicate_pending_reach_completion_ready();
    std::cout << "  cleanup relation route pending and duplicate pending: PASS\n";
    test_cleanup_relation_route_markerless_absence_reaches_completion_ready();
    std::cout << "  cleanup relation route markerless absence: PASS\n";
    test_cleanup_relation_route_active_ordinal_retains_prefix_and_merged_corpus();
    std::cout << "  cleanup relation route active ordinal and merged retention: PASS\n";
    test_completion_ready_capsule_is_process_bound_across_fork();
    std::cout << "  cleanup completion-ready capsule fork boundary: PASS\n";
    test_completion_publication_creates_and_advances_one_frontier();
    std::cout << "  cleanup completion publication advances one frontier: PASS\n";
    test_completion_publication_recovers_pending_canonical_and_identical_dual();
    std::cout << "  cleanup completion P/C/CP recovery: PASS\n";
    test_completion_publication_fault_prefixes_cold_replay();
    std::cout << "  cleanup completion durable fault replay: PASS\n";
    test_completion_publication_rejects_replacement_and_successor_drift();
    std::cout << "  cleanup completion successor replacement and drift rejection: PASS\n";
    test_completion_publication_is_process_bound_across_fork();
    std::cout << "  cleanup completion publisher fork boundary: PASS\n";
    test_completion_publication_nonzero_last_worker_stops_before_m5();
    std::cout << "  cleanup completion last worker stops before M5: PASS\n";
}

#endif

void run_platform_tests() {
#if defined(__APPLE__)
    throw TestFailure("platform suite is reserved for unsupported hosts");
#else
    static_assert(!std::is_default_constructible_v<CommittedTail>);
    static_assert(!std::is_default_constructible_v<CleanupAdmission>);
    CHECK(cleanup_authority::distributed_sieve_worker_cleanup_tail_root_snapshot_status(
              wave::DistributedSieveWaveStoreStatus::platform_unsupported) ==
          cleanup_authority::DistributedSieveWorkerCleanupTailStatusV1::platform_unsupported);
    std::cout << "  cleanup-tail authority remains source-private and typed: PASS\n";
#endif
}

#undef CHECK

} // namespace

int main(int argc, char** argv) {
    try {
#if defined(__APPLE__)
        std::error_code executable_error;
        const auto absolute_executable = std::filesystem::absolute(argv[0], executable_error);
        if (executable_error) {
            throw std::filesystem::filesystem_error("resolve test executable", argv[0],
                                                    executable_error);
        }
        const auto canonical_executable =
            std::filesystem::canonical(absolute_executable, executable_error);
        if (executable_error) {
            throw std::filesystem::filesystem_error("canonicalize test executable",
                                                    absolute_executable, executable_error);
        }
        test_executable = canonical_executable.string();

        if (argc == 5 && std::string_view(argv[1]) == "--probe-base-locks") {
            const std::array bases{
                std::filesystem::path(argv[2]),
                std::filesystem::path(argv[3]),
                std::filesystem::path(argv[4]),
            };
            return private_handoff_lock_probe_child(bases);
        }
        if (argc == 4 && std::string_view(argv[1]) == "--probe-wave-lock") {
            const auto digest = gnfs::util::decode_sha256_hex(argv[3]);
            if (!digest.has_value()) {
                return EXIT_FAILURE;
            }
            return wave_lock_probe_child(std::filesystem::path(argv[2]), *digest);
        }
        if (argc == 3 && std::string_view(argv[1]) == "--probe-base-lock-file") {
            return direct_base_lock_probe_child(std::filesystem::path(argv[2]));
        }
#endif

        if (argc == 1) {
#if defined(__APPLE__)
            run_apple_tests();
#else
            run_platform_tests();
#endif
            return EXIT_SUCCESS;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--suite" &&
            std::string_view(argv[2]) == "platform") {
            run_platform_tests();
            return EXIT_SUCCESS;
        }
        std::cerr << "usage: " << argv[0] << " [--suite platform]\n";
        return EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::cerr << "test_distributed_sieve_worker_cleanup_tail: FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
