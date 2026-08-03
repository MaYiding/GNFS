#include "distributed_sieve_merge_commit_authority_internal.hpp"
#include "distributed_sieve_wave_store_internal.hpp"
#include "distributed_sieve_worker_cleanup_authority_internal.hpp"

#include "support/child_process.hpp"

#if defined(__APPLE__)
#include "support/distributed_sieve_wave_merge_commit_fixture.hpp"
#endif

#include <gnfs/core/relation.hpp>
#include <gnfs/relation/ooc_cleanup_transaction.hpp>
#include <gnfs/sieve/distributed_sieve_protocol.hpp>
#include <gnfs/util/durable_immutable_record.hpp>
#include <gnfs/util/sha256.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
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
using CleanupCompletionPreparationResult =
    cleanup_authority::DistributedSieveWorkerCleanupCompletionPreparationResultV1;
using CleanupCompletionDriveFunction =
    decltype(&cleanup_authority::drive_distributed_sieve_worker_cleanup_to_completion_ready_v1);
using CleanupCompletionPublicationResult =
    cleanup_authority::DistributedSieveWorkerCleanupCompletionPublicationResultV1;
using CleanupCompletionPublishedContinuation =
    cleanup_authority::DistributedSieveWorkerCleanupCompletionPublishedContinuationV1;
using CleanupAuthorizationPublishedContinuation =
    cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublishedContinuationV1;
using CleanupAllWorkersCompletedContinuation =
    cleanup_authority::DistributedSieveWorkerCleanupAllWorkersCompletedContinuationV1;
using CleanupAuthorizationPublicationResult =
    cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationResultV1;
using CleanupAuthorizationAdvanceFunction =
    decltype(&cleanup_authority::advance_distributed_sieve_worker_cleanup_authorization_v1);
using CleanupAuthorizationResumeFunction =
    decltype(&cleanup_authority::resume_distributed_sieve_worker_cleanup_authorization_v1);
using CleanupAuthorizationPublicationFault = cleanup_authority::trusted_test::
    DistributedSieveWorkerCleanupAuthorizationPublicationFaultPointV1;
using CleanupAuthorizationPublicationHooks = cleanup_authority::trusted_test::
    DistributedSieveWorkerCleanupAuthorizationPublicationTestHooksV1;

static_assert(std::is_final_v<CleanupAdmission>);
static_assert(!std::is_default_constructible_v<CleanupAdmission>);
static_assert(!std::is_copy_constructible_v<CleanupAdmission>);
static_assert(std::is_nothrow_move_constructible_v<CleanupAdmission>);
static_assert(std::is_final_v<CleanupCompletionPublishedContinuation>);
static_assert(!std::is_default_constructible_v<CleanupCompletionPublishedContinuation>);
static_assert(!std::is_copy_constructible_v<CleanupCompletionPublishedContinuation>);
static_assert(std::is_nothrow_move_constructible_v<CleanupCompletionPublishedContinuation>);
static_assert(std::is_final_v<CleanupAuthorizationPublishedContinuation>);
static_assert(!std::is_default_constructible_v<CleanupAuthorizationPublishedContinuation>);
static_assert(!std::is_copy_constructible_v<CleanupAuthorizationPublishedContinuation>);
static_assert(std::is_nothrow_move_constructible_v<CleanupAuthorizationPublishedContinuation>);
static_assert(!std::is_invocable_v<CleanupCompletionDriveFunction,
                                   CleanupAuthorizationPublishedContinuation&>);
static_assert(std::is_nothrow_invocable_r_v<CleanupCompletionPreparationResult,
                                            CleanupCompletionDriveFunction,
                                            CleanupAuthorizationPublishedContinuation&&>);
static_assert(std::is_final_v<CleanupAllWorkersCompletedContinuation>);
static_assert(!std::is_default_constructible_v<CleanupAllWorkersCompletedContinuation>);
static_assert(!std::is_copy_constructible_v<CleanupAllWorkersCompletedContinuation>);
static_assert(std::is_nothrow_move_constructible_v<CleanupAllWorkersCompletedContinuation>);
static_assert(!std::is_copy_constructible_v<CleanupAuthorizationPublicationResult>);
static_assert(std::is_nothrow_move_constructible_v<CleanupAuthorizationPublicationResult>);
static_assert(!std::is_invocable_v<CleanupAuthorizationAdvanceFunction,
                                   CleanupCompletionPublishedContinuation&>);
static_assert(std::is_nothrow_invocable_r_v<CleanupAuthorizationPublicationResult,
                                            CleanupAuthorizationAdvanceFunction,
                                            CleanupCompletionPublishedContinuation&&>);
static_assert(!std::is_invocable_v<CleanupAuthorizationResumeFunction, CleanupAdmission&>);
static_assert(
    std::is_nothrow_invocable_r_v<CleanupAuthorizationPublicationResult,
                                  CleanupAuthorizationResumeFunction, CleanupAdmission&&>);

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

inline constexpr int BASE_LOCKS_FREE_EXIT = 81;
inline constexpr int BASE_LOCK_HELD_EXIT = 82;
inline constexpr int WAVE_LOCK_BUSY_EXIT = 83;
inline constexpr int AUTHORIZATION_PUBLISHER_FORK_REJECTED_EXIT = 84;

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
                                    "write authorization-publisher fixture leaf");
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
                                "open authorization-publisher directory for sync");
    }
    int synced = -1;
    do {
        synced = ::fsync(descriptor);
    } while (synced != 0 && errno == EINTR);
    const int sync_error = errno;
    (void)::close(descriptor);
    if (synced != 0) {
        throw std::system_error(sync_error, std::generic_category(),
                                "sync authorization-publisher directory");
    }
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
                                "create authorization-publisher fixture leaf");
    }
    try {
        write_all(descriptor, bytes);
        int synced = -1;
        do {
            synced = ::fsync(descriptor);
        } while (synced != 0 && errno == EINTR);
        if (synced != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "sync authorization-publisher fixture leaf");
        }
    } catch (...) {
        (void)::close(descriptor);
        throw;
    }
    (void)::close(descriptor);
    sync_directory(path.parent_path());
}

void replace_file_with_same_bytes(const std::filesystem::path& canonical,
                                  const std::filesystem::path& saved,
                                  std::span<const std::byte> bytes) {
    std::error_code rename_error;
    std::filesystem::rename(canonical, saved, rename_error);
    if (rename_error) {
        throw std::filesystem::filesystem_error("displace authorization-publisher fixture leaf",
                                                canonical, saved, rename_error);
    }
    write_immutable_test_leaf(canonical, bytes);
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

[[nodiscard]] CommittedTail commit_fresh(fixture::PreparedWaveFixture& prepared) {
    auto publication = prepared.prepare_fresh();
    CHECK(publication.admission.has_value());
    auto committed = commit_authority::consume_distributed_sieve_merge_prepared_v1(
        std::move(*publication.admission));
    if (!committed || !committed.committed_tail.has_value()) {
        fail("publish WaveMergeCommit before authorization-publisher fixture", __LINE__);
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
            throw std::filesystem::filesystem_error(
                "remove authorization-publisher worker private leaf", snapshot.leaves[index].path,
                error);
        }
    }
    const auto private_directory = snapshot.leaves[2].path.parent_path();
    std::error_code directory_error;
    if (!std::filesystem::remove(private_directory, directory_error) || directory_error) {
        throw std::filesystem::filesystem_error(
            "remove authorization-publisher worker private directory", private_directory,
            directory_error);
    }
    std::error_code owned_error;
    if (!std::filesystem::remove(snapshot.leaves[1].path, owned_error) || owned_error) {
        throw std::filesystem::filesystem_error(
            "remove authorization-publisher worker OWNED marker", snapshot.leaves[1].path,
            owned_error);
    }
    sync_directory(root);
}

enum class InjectedAuthorizationPrefixV1 : std::uint8_t {
    pending_only,
    canonical_only,
    identical_dual,
};

enum class InjectedCompletionPrefixV1 : std::uint8_t {
    pending_only,
    identical_dual,
};

class CleanupWaveFixture final {
public:
    CleanupWaveFixture(std::string_view label, fixture::PreparedWaveChunkLayoutV1 layout)
        : prepared_(label, layout) {
        auto tail = commit_fresh(prepared_);
        commit_ = tail.record();

        const auto chunk_count = prepared_.manifest().chunks.size();
        worker_snapshots_.resize(chunk_count);
        authorizations_.resize(chunk_count);
        completions_.resize(chunk_count);
        names_.resize(chunk_count);
        authorization_bytes_.resize(chunk_count);
        completion_bytes_.resize(chunk_count);
        for (std::size_t index = 0; index < chunk_count; ++index) {
            CHECK(index <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
            const auto ordinal = static_cast<std::uint32_t>(index);
            names_[index] = wave::distributed_sieve_worker_cleanup_record_names_v1(ordinal);
            CHECK(names_[index].has_value());
            const auto& chunk = prepared_.manifest().chunks[index];
            if (chunk.sq_begin == chunk.sq_end) {
                continue;
            }
            worker_snapshots_[index].emplace(prepared_.worker_snapshot(index));
            const auto source = worker_cleanup_source(*worker_snapshots_[index]);
            authorizations_[index].emplace(
                make_worker_cleanup_authorization(prepared_.manifest(), commit_, source, ordinal));
            completions_[index].emplace(make_worker_cleanup_completion(*authorizations_[index]));
            authorization_bytes_[index] = fixture::encode_record(
                sieve::DistributedSieveProtocolRecordV1{*authorizations_[index]});
            completion_bytes_[index] = fixture::encode_record(
                sieve::DistributedSieveProtocolRecordV1{*completions_[index]});
        }

        auto transitioned =
            cleanup_authority::consume_distributed_sieve_committed_tail_for_worker_cleanup_v1(
                std::move(tail));
        if (!transitioned || !transitioned.admission.has_value()) {
            fail("consume committed tail for authorization-publisher fixture", __LINE__);
        }
        CHECK(!transitioned.retryable_tail.has_value());
        CHECK(transitioned.admission->valid());
        CHECK(transitioned.admission->cleanup_prefix().coordinates.empty());
        CHECK(transitioned.admission->reader().count() == prepared_.expected_rows().size());
        CHECK(fixture::relation_vectors_equal(transitioned.admission->reader().read_all(),
                                              prepared_.expected_rows()));
        admission_.emplace(std::move(*transitioned.admission));
    }

    CleanupWaveFixture(const CleanupWaveFixture&) = delete;
    CleanupWaveFixture& operator=(const CleanupWaveFixture&) = delete;

    [[nodiscard]] fixture::PreparedWaveFixture& prepared() noexcept {
        return prepared_;
    }

    [[nodiscard]] const fixture::PreparedWaveFixture& prepared() const noexcept {
        return prepared_;
    }

    [[nodiscard]] const sieve::WaveMergeCommitV1& commit() const noexcept {
        return commit_;
    }

    [[nodiscard]] std::vector<std::uint32_t> nonempty_ordinals() const {
        std::vector<std::uint32_t> ordinals;
        for (std::size_t index = 0; index < prepared_.manifest().chunks.size(); ++index) {
            const auto& chunk = prepared_.manifest().chunks[index];
            if (chunk.sq_begin < chunk.sq_end) {
                ordinals.push_back(static_cast<std::uint32_t>(index));
            }
        }
        return ordinals;
    }

    [[nodiscard]] CleanupAdmission take_admission() {
        CHECK(admission_.has_value());
        CleanupAdmission admission(std::move(*admission_));
        admission_.reset();
        CHECK(admission.valid());
        return admission;
    }

    [[nodiscard]] CleanupAdmission& admission() {
        CHECK(admission_.has_value());
        return *admission_;
    }

    void set_admission(CleanupAdmission&& admission) {
        CHECK(!admission_.has_value());
        CHECK(admission.valid());
        admission_.emplace(std::move(admission));
    }

    void release_admission() noexcept {
        admission_.reset();
    }

    void cold_reopen(std::string_view context) {
        release_admission();
        auto opened =
            wave::open_worker_cleanup_root_v1(prepared_.root(), prepared_.manifest_digest());
        if (!opened || !opened.admission.has_value()) {
            fail(context, __LINE__, wave_diagnostic_detail(opened.diagnostic));
        }
        CHECK(opened.admission->valid());
        admission_.emplace(std::move(*opened.admission));
    }

    [[nodiscard]] const WorkerSnapshot& worker_snapshot(std::uint32_t ordinal) const {
        CHECK(ordinal < worker_snapshots_.size());
        CHECK(worker_snapshots_[ordinal].has_value());
        return *worker_snapshots_[ordinal];
    }

    [[nodiscard]] const sieve::ArtifactCleanupAuthorizedV1&
    authorization(std::uint32_t ordinal) const {
        CHECK(ordinal < authorizations_.size());
        CHECK(authorizations_[ordinal].has_value());
        return *authorizations_[ordinal];
    }

    [[nodiscard]] const std::vector<std::byte>& authorization_bytes(std::uint32_t ordinal) const {
        CHECK(ordinal < authorization_bytes_.size());
        CHECK(!authorization_bytes_[ordinal].empty());
        return authorization_bytes_[ordinal];
    }

    [[nodiscard]] const std::vector<std::byte>& completion_bytes(std::uint32_t ordinal) const {
        CHECK(ordinal < completion_bytes_.size());
        CHECK(!completion_bytes_[ordinal].empty());
        return completion_bytes_[ordinal];
    }

    [[nodiscard]] const wave::DistributedSieveWorkerCleanupRecordNamesV1&
    cleanup_names(std::uint32_t ordinal) const {
        CHECK(ordinal < names_.size());
        CHECK(names_[ordinal].has_value());
        return *names_[ordinal];
    }

    [[nodiscard]] std::filesystem::path authorization_canonical_path(std::uint32_t ordinal) const {
        return prepared_.root() / cleanup_names(ordinal).authorization_canonical_record_leaf;
    }

    [[nodiscard]] std::filesystem::path authorization_pending_path(std::uint32_t ordinal) const {
        return prepared_.root() / cleanup_names(ordinal).authorization_pending_record_leaf;
    }

    [[nodiscard]] std::filesystem::path completion_canonical_path(std::uint32_t ordinal) const {
        return prepared_.root() / cleanup_names(ordinal).completion_canonical_record_leaf;
    }

    [[nodiscard]] std::filesystem::path completion_pending_path(std::uint32_t ordinal) const {
        return prepared_.root() / cleanup_names(ordinal).completion_pending_record_leaf;
    }

    [[nodiscard]] std::filesystem::path worker_base(std::uint32_t ordinal) const {
        CHECK(ordinal < prepared_.manifest().chunks.size());
        const auto& chunk = prepared_.manifest().chunks[ordinal];
        CHECK(chunk.sq_begin < chunk.sq_end);
        const auto attempt_names = wave::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(attempt_names.has_value());
        return prepared_.root() / attempt_names->private_directory_leaf / "corpus";
    }

    [[nodiscard]] std::filesystem::path merged_base() const {
        const auto merge_names = wave::distributed_sieve_merge_generation_names_v1(
            prepared_.merge_started().merge_attempt_ordinal);
        CHECK(merge_names.has_value());
        return prepared_.root() / merge_names->private_directory_leaf / "corpus";
    }

    void inject_authorization(std::uint32_t ordinal, InjectedAuthorizationPrefixV1 shape) {
        release_admission();
        const auto& names = cleanup_names(ordinal);
        const auto& bytes = authorization_bytes(ordinal);
        if (shape != InjectedAuthorizationPrefixV1::pending_only) {
            fixture::publish_canonical_record(prepared_.root(),
                                              names.authorization_pending_record_leaf,
                                              names.authorization_canonical_record_leaf, bytes);
        }
        if (shape != InjectedAuthorizationPrefixV1::canonical_only) {
            write_immutable_test_leaf(authorization_pending_path(ordinal), bytes);
        }
    }

    void inject_completion(std::uint32_t ordinal, InjectedCompletionPrefixV1 shape) {
        release_admission();
        if (!std::filesystem::exists(authorization_canonical_path(ordinal))) {
            const auto& names = cleanup_names(ordinal);
            fixture::publish_canonical_record(
                prepared_.root(), names.authorization_pending_record_leaf,
                names.authorization_canonical_record_leaf, authorization_bytes(ordinal));
        }
        if (std::filesystem::exists(worker_snapshot(ordinal).leaves[1].path)) {
            remove_worker_private_namespace(prepared_.root(), worker_snapshot(ordinal));
        }
        const auto& names = cleanup_names(ordinal);
        const auto& bytes = completion_bytes(ordinal);
        if (shape == InjectedCompletionPrefixV1::identical_dual) {
            fixture::publish_canonical_record(prepared_.root(),
                                              names.completion_pending_record_leaf,
                                              names.completion_canonical_record_leaf, bytes);
        }
        write_immutable_test_leaf(completion_pending_path(ordinal), bytes);
    }

private:
    fixture::PreparedWaveFixture prepared_;
    sieve::WaveMergeCommitV1 commit_;
    std::vector<std::optional<WorkerSnapshot>> worker_snapshots_;
    std::vector<std::optional<sieve::ArtifactCleanupAuthorizedV1>> authorizations_;
    std::vector<std::optional<sieve::ArtifactCleanupCompletedV1>> completions_;
    std::vector<std::optional<wave::DistributedSieveWorkerCleanupRecordNamesV1>> names_;
    std::vector<std::vector<std::byte>> authorization_bytes_;
    std::vector<std::vector<std::byte>> completion_bytes_;
    std::optional<CleanupAdmission> admission_;
};

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
    CHECK(capture_merged_corpus(merged_base).leaves == expected.leaves);
}

[[nodiscard]] std::vector<std::string> capture_root_entry_names(const std::filesystem::path& root) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

struct CleanupCanonicalPairSnapshot final {
    fixture::LeafSnapshot authorization;
    fixture::LeafSnapshot completion;

    [[nodiscard]] friend bool operator==(const CleanupCanonicalPairSnapshot&,
                                         const CleanupCanonicalPairSnapshot&) = default;
};

[[nodiscard]] CleanupCanonicalPairSnapshot
capture_cleanup_canonical_pair(const CleanupWaveFixture& root, std::uint32_t ordinal) {
    return {
        .authorization = fixture::snapshot_leaf(root.authorization_canonical_path(ordinal)),
        .completion = fixture::snapshot_leaf(root.completion_canonical_path(ordinal)),
    };
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
run_direct_base_lock_probe(const std::filesystem::path& base) {
    const auto lock_path = relation::OOCCleanupTransaction::paths_for(base).lock_path;
    return gnfs::test::run_child_process(test_executable,
                                         {"--probe-base-lock-file", lock_path.string()});
}

void require_all_base_locks_free(const CleanupWaveFixture& root) {
    auto ordinals = root.nonempty_ordinals();
    for (const auto ordinal : ordinals) {
        const auto probe = run_direct_base_lock_probe(root.worker_base(ordinal));
        CHECK(probe.exited);
        CHECK(!probe.signaled);
        CHECK(probe.exit_code == BASE_LOCKS_FREE_EXIT);
    }
    const auto merged_probe = run_direct_base_lock_probe(root.merged_base());
    CHECK(merged_probe.exited);
    CHECK(!merged_probe.signaled);
    CHECK(merged_probe.exit_code == BASE_LOCKS_FREE_EXIT);
}

void require_wave_lock_busy(const CleanupWaveFixture& root) {
    const auto probe = gnfs::test::run_child_process(
        test_executable, {"--probe-wave-lock", root.prepared().root().string(),
                          digest_hex(root.prepared().manifest_digest())});
    CHECK(probe.exited);
    CHECK(!probe.signaled);
    CHECK(probe.exit_code == WAVE_LOCK_BUSY_EXIT);
}

[[nodiscard]] sieve::ArtifactCleanupAuthorizedV1
read_cleanup_authorization(const std::filesystem::path& path) {
    const auto decoded = sieve::decode_distributed_sieve_record(fixture::read_file_bytes(path));
    CHECK(decoded);
    CHECK(decoded.value.has_value());
    const auto* authorization = std::get_if<sieve::ArtifactCleanupAuthorizedV1>(&*decoded.value);
    CHECK(authorization != nullptr);
    return *authorization;
}

void require_authorization_publication_success(
    CleanupAuthorizationPublicationResult& result, const CleanupWaveFixture& root,
    std::uint32_t expected_ordinal,
    cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationTargetShapeV1
        expected_shape,
    durable_record::RecordPublishDisposition expected_publication_disposition) {
    using Phase = cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationPhaseV1;
    using Status = cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationStatusV1;
    using Disposition =
        cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationDispositionV1;

    CHECK(result);
    CHECK(!result.retryable_completion_continuation.has_value());
    CHECK(!result.retryable_recovery_root.has_value());
    CHECK(!result.completion_reconciliation_root.has_value());
    CHECK(result.authorization_published.has_value());
    CHECK(!result.all_workers_completed.has_value());
    CHECK(result.authorization_published->valid());
    CHECK(result.authorization_published->manifest_order_ordinal() == expected_ordinal);
    CHECK(result.diagnostic.phase == Phase::complete);
    CHECK(result.diagnostic.status == Status::authorization_published);
    CHECK(result.diagnostic.disposition == Disposition::authorization_published);
    CHECK(result.diagnostic.manifest_order_ordinal == expected_ordinal);
    CHECK(result.diagnostic.target_shape == expected_shape);
    CHECK(result.diagnostic.publication_status == durable_record::RecordPublishStatus::durable);
    CHECK(result.diagnostic.publication_disposition == expected_publication_disposition);
    CHECK(result.diagnostic.authority_spent);
    CHECK(result.diagnostic.publication_started);
    CHECK(!result.diagnostic.native_error);
    CHECK(std::filesystem::exists(root.authorization_canonical_path(expected_ordinal)));
    CHECK(!std::filesystem::exists(root.authorization_pending_path(expected_ordinal)));
    CHECK(fixture::read_file_bytes(root.authorization_canonical_path(expected_ordinal)) ==
          root.authorization_bytes(expected_ordinal));
    const auto authorization =
        read_cleanup_authorization(root.authorization_canonical_path(expected_ordinal));
    CHECK(authorization.self_digest == result.authorization_published->authorization_digest());
    CHECK(authorization.self_digest == root.authorization(expected_ordinal).self_digest);
}

[[nodiscard]] CleanupAuthorizationPublishedContinuation
take_authorization_publication(CleanupAuthorizationPublicationResult& result) {
    CHECK(result.authorization_published.has_value());
    CleanupAuthorizationPublishedContinuation continuation(
        std::move(*result.authorization_published));
    result.authorization_published.reset();
    CHECK(continuation.valid());
    return continuation;
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

[[nodiscard]] CleanupCompletionPublishedContinuation
complete_authorized_worker(CleanupAuthorizationPublishedContinuation&& authorization,
                           std::uint32_t expected_ordinal) {
    auto prepared =
        cleanup_authority::drive_distributed_sieve_worker_cleanup_to_completion_ready_v1(
            std::move(authorization));
    CHECK(prepared);
    CHECK(!prepared.retryable_root.has_value());
    CHECK(!prepared.retryable_intent_conversion.has_value());
    CHECK(prepared.completion_ready.has_value());
    CHECK(prepared.completion_ready->valid());
    CHECK(prepared.completion_ready->manifest_order_ordinal() == expected_ordinal);
    CHECK(prepared.diagnostic.phase ==
          cleanup_authority::DistributedSieveWorkerCleanupCompletionPreparationPhaseV1::complete);
    CHECK(prepared.diagnostic.status ==
          cleanup_authority::DistributedSieveWorkerCleanupCompletionPreparationStatusV1::ready);
    CHECK(prepared.diagnostic.disposition ==
          cleanup_authority::DistributedSieveWorkerCleanupCompletionPreparationDispositionV1::
              completion_ready);

    auto published = cleanup_authority::publish_distributed_sieve_worker_cleanup_completion_v1(
        std::move(*prepared.completion_ready));
    require_completion_publication_success(published, expected_ordinal,
                                           durable_record::RecordPublishDisposition::created);
    CleanupCompletionPublishedContinuation continuation(
        std::move(*published.published_continuation));
    published.published_continuation.reset();
    CHECK(continuation.valid());
    return continuation;
}

void require_cleanup_coordinate_records_absent(const CleanupWaveFixture& root,
                                               std::uint32_t ordinal) {
    CHECK(!std::filesystem::exists(root.authorization_pending_path(ordinal)));
    CHECK(!std::filesystem::exists(root.authorization_canonical_path(ordinal)));
    CHECK(!std::filesystem::exists(root.completion_pending_path(ordinal)));
    CHECK(!std::filesystem::exists(root.completion_canonical_path(ordinal)));
}

void require_terminal_success(CleanupAuthorizationPublicationResult& result,
                              std::size_t expected_completed_workers) {
    using Phase = cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationPhaseV1;
    using Status = cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationStatusV1;
    using Disposition =
        cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationDispositionV1;
    using TargetShape =
        cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationTargetShapeV1;

    CHECK(result);
    CHECK(!result.retryable_completion_continuation.has_value());
    CHECK(!result.retryable_recovery_root.has_value());
    CHECK(!result.completion_reconciliation_root.has_value());
    CHECK(!result.authorization_published.has_value());
    CHECK(result.all_workers_completed.has_value());
    CHECK(result.all_workers_completed->valid());
    CHECK(result.all_workers_completed->completed_worker_count() == expected_completed_workers);
    CHECK(result.diagnostic.phase == Phase::complete);
    CHECK(result.diagnostic.status == Status::all_workers_completed);
    CHECK(result.diagnostic.disposition == Disposition::all_workers_completed);
    CHECK(!result.diagnostic.manifest_order_ordinal.has_value());
    CHECK(result.diagnostic.target_shape == TargetShape::all_workers_completed);
    CHECK(!result.diagnostic.publication_status.has_value());
    CHECK(!result.diagnostic.publication_disposition.has_value());
    CHECK(result.diagnostic.authority_spent);
    CHECK(!result.diagnostic.publication_started);
    CHECK(!result.diagnostic.native_error);
}

struct AuthorizationPublicationStopContext final {
    CleanupAuthorizationPublicationFault target =
        CleanupAuthorizationPublicationFault::PendingDurable;
    std::array<bool, static_cast<std::size_t>(CleanupAuthorizationPublicationFault::Count)>
        observed{};
};

[[nodiscard]] bool stop_authorization_publication_after(CleanupAuthorizationPublicationFault point,
                                                        void* opaque) noexcept {
    auto& context = *static_cast<AuthorizationPublicationStopContext*>(opaque);
    const auto index = static_cast<std::size_t>(point);
    if (index < context.observed.size()) {
        context.observed[index] = true;
    }
    return point == context.target;
}

enum class AuthorizationPublicationMutationV1 : std::uint8_t {
    inject_pending_after_baseline,
    replace_canonical_after_successor,
    add_pending_after_successor,
};

struct AuthorizationPublicationMutationContext final {
    AuthorizationPublicationMutationV1 mutation =
        AuthorizationPublicationMutationV1::inject_pending_after_baseline;
    std::filesystem::path canonical;
    std::filesystem::path pending;
    std::filesystem::path saved;
    std::vector<std::byte> expected_bytes;
    bool invoked = false;
    std::exception_ptr failure;
};

[[nodiscard]] bool mutate_authorization_publication(CleanupAuthorizationPublicationFault point,
                                                    void* opaque) noexcept {
    auto& context = *static_cast<AuthorizationPublicationMutationContext*>(opaque);
    const bool inject_pending =
        context.mutation == AuthorizationPublicationMutationV1::inject_pending_after_baseline &&
        point == CleanupAuthorizationPublicationFault::AfterFirstBaselineObservation;
    const bool replace_canonical =
        context.mutation == AuthorizationPublicationMutationV1::replace_canonical_after_successor &&
        point == CleanupAuthorizationPublicationFault::AfterFirstSuccessorObservation;
    const bool add_pending =
        context.mutation == AuthorizationPublicationMutationV1::add_pending_after_successor &&
        point == CleanupAuthorizationPublicationFault::AfterFirstSuccessorObservation;
    if (!inject_pending && !replace_canonical && !add_pending) {
        return false;
    }
    context.invoked = true;
    try {
        if (inject_pending) {
            write_immutable_test_leaf(context.pending, context.expected_bytes);
        } else if (replace_canonical) {
            replace_file_with_same_bytes(context.canonical, context.saved,
                                         fixture::read_file_bytes(context.canonical));
        } else {
            write_immutable_test_leaf(context.pending, fixture::read_file_bytes(context.canonical));
        }
    } catch (...) {
        context.failure = std::current_exception();
    }
    return false;
}

#endif

void test_authorization_publisher_cold_initial_and_fresh_next_cross_empty_chunks() {
#if defined(__APPLE__)
    using TargetShape =
        cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationTargetShapeV1;

    CleanupWaveFixture root("worker-cleanup-authorization-cold-fresh-empty",
                            fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
    const auto merged_snapshot = capture_merged_corpus(root.merged_base());
    const auto first_worker_snapshot = root.prepared().worker_snapshot(0U);
    const auto second_worker_snapshot = root.prepared().worker_snapshot(2U);
    const auto initial_entries = capture_root_entry_names(root.prepared().root());

    auto initial = cleanup_authority::resume_distributed_sieve_worker_cleanup_authorization_v1(
        root.take_admission());
    require_authorization_publication_success(initial, root, 0U, TargetShape::absent,
                                              durable_record::RecordPublishDisposition::created);
    auto expected_initial_entries = initial_entries;
    expected_initial_entries.push_back(root.authorization_canonical_path(0U).filename().string());
    std::sort(expected_initial_entries.begin(), expected_initial_entries.end());
    CHECK(capture_root_entry_names(root.prepared().root()) == expected_initial_entries);
    CHECK(root.prepared().worker_snapshot(0U) == first_worker_snapshot);
    CHECK(root.prepared().worker_snapshot(2U) == second_worker_snapshot);
    require_cleanup_coordinate_records_absent(root, 1U);
    require_cleanup_coordinate_records_absent(root, 2U);

    auto first_authorization = take_authorization_publication(initial);
    auto first_completion = complete_authorized_worker(std::move(first_authorization), 0U);
    CHECK(std::filesystem::exists(root.completion_canonical_path(0U)));
    const auto before_fresh = capture_root_entry_names(root.prepared().root());

    auto next = cleanup_authority::advance_distributed_sieve_worker_cleanup_authorization_v1(
        std::move(first_completion));
    require_authorization_publication_success(next, root, 2U, TargetShape::absent,
                                              durable_record::RecordPublishDisposition::created);
    auto expected_fresh_entries = before_fresh;
    expected_fresh_entries.push_back(root.authorization_canonical_path(2U).filename().string());
    std::sort(expected_fresh_entries.begin(), expected_fresh_entries.end());
    CHECK(capture_root_entry_names(root.prepared().root()) == expected_fresh_entries);
    CHECK(root.prepared().worker_snapshot(2U) == second_worker_snapshot);
    require_cleanup_coordinate_records_absent(root, 1U);
    require_cleanup_coordinate_records_absent(root, 3U);
    require_merged_corpus(root.merged_base(), merged_snapshot);
    require_all_base_locks_free(root);
    require_wave_lock_busy(root);
#else
    throw TestFailure("authorization publisher requires the macOS private-handoff runtime");
#endif
}

void test_authorization_publisher_recovers_pending_canonical_and_identical_dual() {
#if defined(__APPLE__)
    using TargetShape =
        cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationTargetShapeV1;

    struct Case final {
        InjectedAuthorizationPrefixV1 shape;
        std::string_view label;
        TargetShape expected_shape;
        durable_record::RecordPublishDisposition expected_disposition;
    };
    constexpr std::array cases{
        Case{InjectedAuthorizationPrefixV1::pending_only, "pending",
             TargetShape::authorization_pending_only,
             durable_record::RecordPublishDisposition::recovered_pending},
        Case{InjectedAuthorizationPrefixV1::canonical_only, "canonical",
             TargetShape::authorization_canonical_only,
             durable_record::RecordPublishDisposition::confirmed_existing},
        Case{InjectedAuthorizationPrefixV1::identical_dual, "dual",
             TargetShape::authorization_identical_dual,
             durable_record::RecordPublishDisposition::confirmed_existing},
    };

    for (const auto& test_case : cases) {
        CleanupWaveFixture root("worker-cleanup-authorization-recover-" +
                                    std::string(test_case.label),
                                fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
        const auto merged_snapshot = capture_merged_corpus(root.merged_base());
        const auto worker_snapshot = root.prepared().worker_snapshot(0U);
        root.inject_authorization(0U, test_case.shape);
        const auto canonical_before =
            std::filesystem::exists(root.authorization_canonical_path(0U))
                ? std::optional{fixture::snapshot_leaf(root.authorization_canonical_path(0U))}
                : std::nullopt;
        const auto pending_before =
            std::filesystem::exists(root.authorization_pending_path(0U))
                ? std::optional{fixture::snapshot_leaf(root.authorization_pending_path(0U))}
                : std::nullopt;
        root.cold_reopen("cold-open injected authorization prefix");

        auto recovered =
            cleanup_authority::resume_distributed_sieve_worker_cleanup_authorization_v1(
                root.take_admission());
        require_authorization_publication_success(recovered, root, 0U, test_case.expected_shape,
                                                  test_case.expected_disposition);
        const auto canonical_after = fixture::snapshot_leaf(root.authorization_canonical_path(0U));
        if (test_case.shape == InjectedAuthorizationPrefixV1::pending_only) {
            CHECK(pending_before.has_value());
            CHECK(canonical_after.identity == pending_before->identity);
            CHECK(canonical_after.bytes == pending_before->bytes);
        } else {
            CHECK(canonical_before.has_value());
            CHECK(canonical_after == *canonical_before);
        }
        CHECK(!std::filesystem::exists(root.authorization_pending_path(0U)));
        CHECK(root.prepared().worker_snapshot(0U) == worker_snapshot);
        require_cleanup_coordinate_records_absent(root, 1U);
        require_cleanup_coordinate_records_absent(root, 2U);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);
        require_wave_lock_busy(root);
    }
#else
    throw TestFailure("authorization publisher requires the macOS private-handoff runtime");
#endif
}

void test_authorization_publisher_routes_three_workers_and_returns_terminal_view() {
#if defined(__APPLE__)
    using TargetShape =
        cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationTargetShapeV1;

    CleanupWaveFixture root(
        "worker-cleanup-authorization-three-worker-route",
        fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_nonempty_empty);
    const auto merged_snapshot = capture_merged_corpus(root.merged_base());

    auto first = cleanup_authority::resume_distributed_sieve_worker_cleanup_authorization_v1(
        root.take_admission());
    require_authorization_publication_success(first, root, 0U, TargetShape::absent,
                                              durable_record::RecordPublishDisposition::created);
    require_cleanup_coordinate_records_absent(root, 2U);
    require_cleanup_coordinate_records_absent(root, 3U);
    auto first_completion = complete_authorized_worker(take_authorization_publication(first), 0U);
    const auto first_pair = capture_cleanup_canonical_pair(root, 0U);

    auto second = cleanup_authority::advance_distributed_sieve_worker_cleanup_authorization_v1(
        std::move(first_completion));
    require_authorization_publication_success(second, root, 2U, TargetShape::absent,
                                              durable_record::RecordPublishDisposition::created);
    CHECK(capture_cleanup_canonical_pair(root, 0U) == first_pair);
    require_cleanup_coordinate_records_absent(root, 3U);
    auto second_completion = complete_authorized_worker(take_authorization_publication(second), 2U);
    const auto second_pair = capture_cleanup_canonical_pair(root, 2U);

    auto third = cleanup_authority::advance_distributed_sieve_worker_cleanup_authorization_v1(
        std::move(second_completion));
    require_authorization_publication_success(third, root, 3U, TargetShape::absent,
                                              durable_record::RecordPublishDisposition::created);
    CHECK(capture_cleanup_canonical_pair(root, 0U) == first_pair);
    CHECK(capture_cleanup_canonical_pair(root, 2U) == second_pair);
    auto third_completion = complete_authorized_worker(take_authorization_publication(third), 3U);
    const auto third_pair = capture_cleanup_canonical_pair(root, 3U);
    const auto before_terminal = capture_root_entry_names(root.prepared().root());

    auto terminal = cleanup_authority::advance_distributed_sieve_worker_cleanup_authorization_v1(
        std::move(third_completion));
    require_terminal_success(terminal, 3U);
    CHECK(capture_root_entry_names(root.prepared().root()) == before_terminal);
    CHECK(capture_cleanup_canonical_pair(root, 0U) == first_pair);
    CHECK(capture_cleanup_canonical_pair(root, 2U) == second_pair);
    CHECK(capture_cleanup_canonical_pair(root, 3U) == third_pair);
    require_cleanup_coordinate_records_absent(root, 1U);
    require_cleanup_coordinate_records_absent(root, 4U);
    for (const auto ordinal : root.nonempty_ordinals()) {
        CHECK(std::filesystem::exists(root.authorization_canonical_path(ordinal)));
        CHECK(std::filesystem::exists(root.completion_canonical_path(ordinal)));
        CHECK(!std::filesystem::exists(root.authorization_pending_path(ordinal)));
        CHECK(!std::filesystem::exists(root.completion_pending_path(ordinal)));
    }
    require_merged_corpus(root.merged_base(), merged_snapshot);
    require_all_base_locks_free(root);
    require_wave_lock_busy(root);

    CleanupWaveFixture cold_root(
        "worker-cleanup-authorization-three-worker-cold-terminal",
        fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_nonempty_empty);
    const auto cold_merged_snapshot = capture_merged_corpus(cold_root.merged_base());
    auto cold_first = cleanup_authority::resume_distributed_sieve_worker_cleanup_authorization_v1(
        cold_root.take_admission());
    require_authorization_publication_success(cold_first, cold_root, 0U, TargetShape::absent,
                                              durable_record::RecordPublishDisposition::created);
    require_cleanup_coordinate_records_absent(cold_root, 2U);
    require_cleanup_coordinate_records_absent(cold_root, 3U);
    auto cold_first_completion =
        complete_authorized_worker(take_authorization_publication(cold_first), 0U);
    auto cold_second = cleanup_authority::advance_distributed_sieve_worker_cleanup_authorization_v1(
        std::move(cold_first_completion));
    require_authorization_publication_success(cold_second, cold_root, 2U, TargetShape::absent,
                                              durable_record::RecordPublishDisposition::created);
    require_cleanup_coordinate_records_absent(cold_root, 3U);
    auto cold_second_completion =
        complete_authorized_worker(take_authorization_publication(cold_second), 2U);
    auto cold_third = cleanup_authority::advance_distributed_sieve_worker_cleanup_authorization_v1(
        std::move(cold_second_completion));
    require_authorization_publication_success(cold_third, cold_root, 3U, TargetShape::absent,
                                              durable_record::RecordPublishDisposition::created);
    {
        auto released = complete_authorized_worker(take_authorization_publication(cold_third), 3U);
        CHECK(released.valid());
    }
    cold_root.cold_reopen("cold-open all-workers-completed authorization root");
    const auto cold_entries_before = capture_root_entry_names(cold_root.prepared().root());
    const auto cold_first_pair = capture_cleanup_canonical_pair(cold_root, 0U);
    const auto cold_second_pair = capture_cleanup_canonical_pair(cold_root, 2U);
    const auto cold_third_pair = capture_cleanup_canonical_pair(cold_root, 3U);
    auto cold_terminal =
        cleanup_authority::resume_distributed_sieve_worker_cleanup_authorization_v1(
            cold_root.take_admission());
    require_terminal_success(cold_terminal, 3U);
    CHECK(capture_root_entry_names(cold_root.prepared().root()) == cold_entries_before);
    CHECK(capture_cleanup_canonical_pair(cold_root, 0U) == cold_first_pair);
    CHECK(capture_cleanup_canonical_pair(cold_root, 2U) == cold_second_pair);
    CHECK(capture_cleanup_canonical_pair(cold_root, 3U) == cold_third_pair);
    require_merged_corpus(cold_root.merged_base(), cold_merged_snapshot);
    require_all_base_locks_free(cold_root);
    require_wave_lock_busy(cold_root);
#else
    throw TestFailure("authorization publisher requires the macOS private-handoff runtime");
#endif
}

void test_authorization_publisher_fault_prefixes_cold_replay() {
#if defined(__APPLE__)
    using Phase = cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationPhaseV1;
    using Status = cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationStatusV1;
    using Disposition =
        cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationDispositionV1;
    using TargetShape =
        cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationTargetShapeV1;

    constexpr std::array faults{
        CleanupAuthorizationPublicationFault::PendingDurable,
        CleanupAuthorizationPublicationFault::CanonicalPromoted,
        CleanupAuthorizationPublicationFault::CanonicalDurable,
    };

    {
        CleanupWaveFixture root("worker-cleanup-authorization-cold-before-spend",
                                fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
        const auto merged_snapshot = capture_merged_corpus(root.merged_base());
        const auto entries_before = capture_root_entry_names(root.prepared().root());
        AuthorizationPublicationStopContext context{
            .target = CleanupAuthorizationPublicationFault::ColdBeforeAuthoritySpend,
        };

        auto retryable = cleanup_authority::trusted_test::
            resume_distributed_sieve_worker_cleanup_authorization_v1_with_hooks(
                root.take_admission(), CleanupAuthorizationPublicationHooks{
                                           .stop_after = stop_authorization_publication_after,
                                           .context = &context,
                                       });

        CHECK(retryable);
        CHECK(!retryable.retryable_completion_continuation.has_value());
        CHECK(retryable.retryable_recovery_root.has_value());
        CHECK(retryable.retryable_recovery_root->valid());
        CHECK(!retryable.completion_reconciliation_root.has_value());
        CHECK(!retryable.authorization_published.has_value());
        CHECK(!retryable.all_workers_completed.has_value());
        CHECK(retryable.diagnostic.phase == Phase::authority_spend);
        CHECK(retryable.diagnostic.status == Status::retryable_recovery_root);
        CHECK(retryable.diagnostic.disposition == Disposition::retryable_recovery_root);
        CHECK(retryable.diagnostic.manifest_order_ordinal == 0U);
        CHECK(retryable.diagnostic.target_shape == TargetShape::absent);
        CHECK(retryable.diagnostic.last_fault_point ==
              CleanupAuthorizationPublicationFault::ColdBeforeAuthoritySpend);
        CHECK(!retryable.diagnostic.authority_spent);
        CHECK(!retryable.diagnostic.publication_started);
        CHECK(!retryable.diagnostic.publication_status.has_value());
        CHECK(!retryable.diagnostic.publication_disposition.has_value());
        CHECK(capture_root_entry_names(root.prepared().root()) == entries_before);
        require_cleanup_coordinate_records_absent(root, 0U);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);
        require_wave_lock_busy(root);

        auto published =
            cleanup_authority::resume_distributed_sieve_worker_cleanup_authorization_v1(
                std::move(*retryable.retryable_recovery_root));
        require_authorization_publication_success(
            published, root, 0U, TargetShape::absent,
            durable_record::RecordPublishDisposition::created);
        require_merged_corpus(root.merged_base(), merged_snapshot);
    }

    {
        CleanupWaveFixture root("worker-cleanup-authorization-fresh-before-spend",
                                fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
        const auto merged_snapshot = capture_merged_corpus(root.merged_base());
        auto first = cleanup_authority::resume_distributed_sieve_worker_cleanup_authorization_v1(
            root.take_admission());
        require_authorization_publication_success(
            first, root, 0U, TargetShape::absent,
            durable_record::RecordPublishDisposition::created);
        auto completion = complete_authorized_worker(take_authorization_publication(first), 0U);
        const auto entries_before = capture_root_entry_names(root.prepared().root());
        AuthorizationPublicationStopContext context{
            .target = CleanupAuthorizationPublicationFault::FreshBeforeAuthoritySpend,
        };

        auto retryable = cleanup_authority::trusted_test::
            advance_distributed_sieve_worker_cleanup_authorization_v1_with_hooks(
                std::move(completion), CleanupAuthorizationPublicationHooks{
                                           .stop_after = stop_authorization_publication_after,
                                           .context = &context,
                                       });

        CHECK(retryable);
        CHECK(retryable.retryable_completion_continuation.has_value());
        CHECK(retryable.retryable_completion_continuation->valid());
        CHECK(!retryable.retryable_recovery_root.has_value());
        CHECK(!retryable.completion_reconciliation_root.has_value());
        CHECK(!retryable.authorization_published.has_value());
        CHECK(!retryable.all_workers_completed.has_value());
        CHECK(retryable.diagnostic.phase == Phase::authority_spend);
        CHECK(retryable.diagnostic.status == Status::retryable_completion_continuation);
        CHECK(retryable.diagnostic.disposition == Disposition::retryable_completion_continuation);
        CHECK(retryable.diagnostic.manifest_order_ordinal == 2U);
        CHECK(retryable.diagnostic.target_shape == TargetShape::absent);
        CHECK(retryable.diagnostic.last_fault_point ==
              CleanupAuthorizationPublicationFault::FreshBeforeAuthoritySpend);
        CHECK(!retryable.diagnostic.authority_spent);
        CHECK(!retryable.diagnostic.publication_started);
        CHECK(!retryable.diagnostic.publication_status.has_value());
        CHECK(!retryable.diagnostic.publication_disposition.has_value());
        CHECK(capture_root_entry_names(root.prepared().root()) == entries_before);
        require_cleanup_coordinate_records_absent(root, 2U);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);
        require_wave_lock_busy(root);

        auto published =
            cleanup_authority::advance_distributed_sieve_worker_cleanup_authorization_v1(
                std::move(*retryable.retryable_completion_continuation));
        require_authorization_publication_success(
            published, root, 2U, TargetShape::absent,
            durable_record::RecordPublishDisposition::created);
        require_merged_corpus(root.merged_base(), merged_snapshot);
    }

    {
        CleanupWaveFixture root("worker-cleanup-authorization-fresh-after-spend",
                                fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
        const auto merged_snapshot = capture_merged_corpus(root.merged_base());
        auto first = cleanup_authority::resume_distributed_sieve_worker_cleanup_authorization_v1(
            root.take_admission());
        require_authorization_publication_success(
            first, root, 0U, TargetShape::absent,
            durable_record::RecordPublishDisposition::created);
        auto completion = complete_authorized_worker(take_authorization_publication(first), 0U);
        const auto entries_before = capture_root_entry_names(root.prepared().root());
        AuthorizationPublicationStopContext context{
            .target = CleanupAuthorizationPublicationFault::FreshAfterAuthoritySpend,
        };

        auto interrupted = cleanup_authority::trusted_test::
            advance_distributed_sieve_worker_cleanup_authorization_v1_with_hooks(
                std::move(completion), CleanupAuthorizationPublicationHooks{
                                           .stop_after = stop_authorization_publication_after,
                                           .context = &context,
                                       });

        CHECK(!interrupted);
        CHECK(!interrupted.retryable_completion_continuation.has_value());
        CHECK(!interrupted.retryable_recovery_root.has_value());
        CHECK(!interrupted.completion_reconciliation_root.has_value());
        CHECK(!interrupted.authorization_published.has_value());
        CHECK(!interrupted.all_workers_completed.has_value());
        CHECK(interrupted.diagnostic.phase == Phase::authority_spend);
        CHECK(interrupted.diagnostic.status == Status::test_interrupted);
        CHECK(interrupted.diagnostic.disposition == Disposition::cold_reopen_required);
        CHECK(interrupted.diagnostic.manifest_order_ordinal == 2U);
        CHECK(interrupted.diagnostic.target_shape == TargetShape::absent);
        CHECK(interrupted.diagnostic.last_fault_point ==
              CleanupAuthorizationPublicationFault::FreshAfterAuthoritySpend);
        CHECK(interrupted.diagnostic.authority_spent);
        CHECK(!interrupted.diagnostic.publication_started);
        CHECK(!interrupted.diagnostic.publication_status.has_value());
        CHECK(!interrupted.diagnostic.publication_disposition.has_value());
        CHECK(capture_root_entry_names(root.prepared().root()) == entries_before);
        require_cleanup_coordinate_records_absent(root, 2U);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);

        root.cold_reopen("cold-create next authorization after fresh spend");
        auto published =
            cleanup_authority::resume_distributed_sieve_worker_cleanup_authorization_v1(
                root.take_admission());
        require_authorization_publication_success(
            published, root, 2U, TargetShape::absent,
            durable_record::RecordPublishDisposition::created);
        require_merged_corpus(root.merged_base(), merged_snapshot);
    }

    for (const auto fault : faults) {
        CleanupWaveFixture root("worker-cleanup-authorization-fault-" +
                                    std::to_string(static_cast<std::size_t>(fault)),
                                fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
        const auto merged_snapshot = capture_merged_corpus(root.merged_base());
        AuthorizationPublicationStopContext context{.target = fault};

        auto interrupted = cleanup_authority::trusted_test::
            resume_distributed_sieve_worker_cleanup_authorization_v1_with_hooks(
                root.take_admission(), CleanupAuthorizationPublicationHooks{
                                           .stop_after = stop_authorization_publication_after,
                                           .context = &context,
                                       });

        CHECK(!interrupted);
        CHECK(!interrupted.retryable_completion_continuation.has_value());
        CHECK(!interrupted.retryable_recovery_root.has_value());
        CHECK(!interrupted.completion_reconciliation_root.has_value());
        CHECK(!interrupted.authorization_published.has_value());
        CHECK(!interrupted.all_workers_completed.has_value());
        CHECK(interrupted.diagnostic.status == Status::test_interrupted);
        CHECK(interrupted.diagnostic.disposition == Disposition::cold_reopen_required);
        CHECK(interrupted.diagnostic.manifest_order_ordinal == 0U);
        CHECK(interrupted.diagnostic.target_shape == TargetShape::absent);
        CHECK(interrupted.diagnostic.publication_status ==
              durable_record::RecordPublishStatus::interrupted);
        CHECK(interrupted.diagnostic.publication_disposition ==
              durable_record::RecordPublishDisposition::created);
        CHECK(interrupted.diagnostic.last_fault_point == fault);
        CHECK(interrupted.diagnostic.authority_spent);
        CHECK(interrupted.diagnostic.publication_started);
        CHECK(context.observed[static_cast<std::size_t>(fault)]);
        const bool pending_only = fault == CleanupAuthorizationPublicationFault::PendingDurable;
        CHECK(std::filesystem::exists(root.authorization_pending_path(0U)) == pending_only);
        CHECK(std::filesystem::exists(root.authorization_canonical_path(0U)) == !pending_only);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);

        root.cold_reopen("cold-replay durable authorization prefix");
        auto replayed = cleanup_authority::resume_distributed_sieve_worker_cleanup_authorization_v1(
            root.take_admission());
        require_authorization_publication_success(
            replayed, root, 0U,
            pending_only ? TargetShape::authorization_pending_only
                         : TargetShape::authorization_canonical_only,
            pending_only ? durable_record::RecordPublishDisposition::recovered_pending
                         : durable_record::RecordPublishDisposition::confirmed_existing);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);
        require_wave_lock_busy(root);
    }
#else
    throw TestFailure("authorization publisher requires the macOS private-handoff runtime");
#endif
}

void test_authorization_publisher_completion_prefixes_are_zero_mutation() {
#if defined(__APPLE__)
    using Phase = cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationPhaseV1;
    using Status = cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationStatusV1;
    using Disposition =
        cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationDispositionV1;
    using TargetShape =
        cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationTargetShapeV1;

    struct Case final {
        InjectedCompletionPrefixV1 shape;
        std::string_view label;
        TargetShape expected_shape;
        durable_record::RecordPublishDisposition expected_completion_disposition;
    };
    constexpr std::array cases{
        Case{InjectedCompletionPrefixV1::pending_only, "pending",
             TargetShape::completion_pending_only,
             durable_record::RecordPublishDisposition::recovered_pending},
        Case{InjectedCompletionPrefixV1::identical_dual, "dual",
             TargetShape::completion_identical_dual,
             durable_record::RecordPublishDisposition::confirmed_existing},
    };

    for (const auto& test_case : cases) {
        CleanupWaveFixture root("worker-cleanup-authorization-completion-route-" +
                                    std::string(test_case.label),
                                fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
        const auto merged_snapshot = capture_merged_corpus(root.merged_base());
        root.inject_completion(0U, test_case.shape);
        root.cold_reopen("cold-open completion reconciliation route");
        const auto entries_before = capture_root_entry_names(root.prepared().root());
        const auto authorization_before =
            fixture::snapshot_leaf(root.authorization_canonical_path(0U));
        const auto completion_pending_before =
            fixture::snapshot_leaf(root.completion_pending_path(0U));
        const auto completion_canonical_before =
            std::filesystem::exists(root.completion_canonical_path(0U))
                ? std::optional{fixture::snapshot_leaf(root.completion_canonical_path(0U))}
                : std::nullopt;

        auto routed = cleanup_authority::resume_distributed_sieve_worker_cleanup_authorization_v1(
            root.take_admission());

        CHECK(routed);
        CHECK(!routed.retryable_completion_continuation.has_value());
        CHECK(!routed.retryable_recovery_root.has_value());
        CHECK(routed.completion_reconciliation_root.has_value());
        CHECK(!routed.authorization_published.has_value());
        CHECK(!routed.all_workers_completed.has_value());
        CHECK(routed.completion_reconciliation_root->valid());
        const auto& routed_prefix = routed.completion_reconciliation_root->cleanup_prefix();
        CHECK(routed_prefix.completed_worker_count == 0U);
        CHECK(routed_prefix.frontier_manifest_order_ordinal == 0U);
        CHECK(routed_prefix.active_manifest_order_ordinal == 0U);
        CHECK(routed_prefix.coordinates.size() == 1U);
        CHECK(routed_prefix.coordinates.front().manifest_order_ordinal == 0U);
        CHECK(routed_prefix.coordinates.front().state ==
              (test_case.shape == InjectedCompletionPrefixV1::pending_only
                   ? wave::DistributedSieveWorkerCleanupPrefixStateV1::completion_pending_only
                   : wave::DistributedSieveWorkerCleanupPrefixStateV1::completion_identical_dual));
        CHECK(routed.completion_reconciliation_root->reader().count() ==
              root.prepared().expected_rows().size());
        CHECK(fixture::relation_vectors_equal(
            routed.completion_reconciliation_root->reader().read_all(),
            root.prepared().expected_rows()));
        CHECK(routed.diagnostic.phase == Phase::complete);
        CHECK(routed.diagnostic.status == Status::completion_reconciliation_required);
        CHECK(routed.diagnostic.disposition == Disposition::completion_reconciliation_required);
        CHECK(routed.diagnostic.manifest_order_ordinal == 0U);
        CHECK(routed.diagnostic.target_shape == test_case.expected_shape);
        CHECK(!routed.diagnostic.publication_status.has_value());
        CHECK(!routed.diagnostic.publication_disposition.has_value());
        CHECK(!routed.diagnostic.authority_spent);
        CHECK(!routed.diagnostic.publication_started);
        CHECK(!routed.diagnostic.native_error);
        CHECK(capture_root_entry_names(root.prepared().root()) == entries_before);
        CHECK(fixture::snapshot_leaf(root.authorization_canonical_path(0U)) ==
              authorization_before);
        CHECK(fixture::snapshot_leaf(root.completion_pending_path(0U)) ==
              completion_pending_before);
        if (completion_canonical_before.has_value()) {
            CHECK(fixture::snapshot_leaf(root.completion_canonical_path(0U)) ==
                  *completion_canonical_before);
        } else {
            CHECK(!std::filesystem::exists(root.completion_canonical_path(0U)));
        }
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);
        require_wave_lock_busy(root);

        auto completion =
            cleanup_authority::reconcile_distributed_sieve_worker_cleanup_completion_v1(
                std::move(*routed.completion_reconciliation_root));
        require_completion_publication_success(completion, 0U,
                                               test_case.expected_completion_disposition);
        auto next = cleanup_authority::advance_distributed_sieve_worker_cleanup_authorization_v1(
            std::move(*completion.published_continuation));
        require_authorization_publication_success(
            next, root, 2U, TargetShape::absent, durable_record::RecordPublishDisposition::created);
        require_merged_corpus(root.merged_base(), merged_snapshot);
    }
#else
    throw TestFailure("authorization publisher requires the macOS private-handoff runtime");
#endif
}

void test_authorization_publisher_rejects_replacement_and_successor_drift() {
#if defined(__APPLE__)
    using Status = cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationStatusV1;
    using Disposition =
        cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationDispositionV1;
    using TargetShape =
        cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationTargetShapeV1;

    {
        CleanupWaveFixture root("worker-cleanup-authorization-baseline-drift",
                                fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
        const auto merged_snapshot = capture_merged_corpus(root.merged_base());
        AuthorizationPublicationMutationContext context{
            .mutation = AuthorizationPublicationMutationV1::inject_pending_after_baseline,
            .canonical = root.authorization_canonical_path(0U),
            .pending = root.authorization_pending_path(0U),
            .saved = {},
            .expected_bytes = root.authorization_bytes(0U),
        };

        auto rejected = cleanup_authority::trusted_test::
            resume_distributed_sieve_worker_cleanup_authorization_v1_with_hooks(
                root.take_admission(), CleanupAuthorizationPublicationHooks{
                                           .stop_after = mutate_authorization_publication,
                                           .context = &context,
                                       });

        CHECK(context.invoked);
        CHECK(!context.failure);
        CHECK(!rejected);
        CHECK(!rejected.retryable_completion_continuation.has_value());
        CHECK(!rejected.retryable_recovery_root.has_value());
        CHECK(!rejected.completion_reconciliation_root.has_value());
        CHECK(!rejected.authorization_published.has_value());
        CHECK(!rejected.all_workers_completed.has_value());
        CHECK(rejected.diagnostic.status == Status::baseline_changed);
        CHECK(rejected.diagnostic.disposition == Disposition::cold_reopen_required);
        CHECK(rejected.diagnostic.target_shape == TargetShape::absent);
        CHECK(!rejected.diagnostic.authority_spent);
        CHECK(!rejected.diagnostic.publication_started);
        CHECK(std::filesystem::exists(context.pending));
        CHECK(!std::filesystem::exists(context.canonical));
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);

        root.cold_reopen("cold-replay baseline authorization drift");
        auto recovered =
            cleanup_authority::resume_distributed_sieve_worker_cleanup_authorization_v1(
                root.take_admission());
        require_authorization_publication_success(
            recovered, root, 0U, TargetShape::authorization_pending_only,
            durable_record::RecordPublishDisposition::recovered_pending);
        require_merged_corpus(root.merged_base(), merged_snapshot);
    }

    constexpr std::array successor_mutations{
        AuthorizationPublicationMutationV1::replace_canonical_after_successor,
        AuthorizationPublicationMutationV1::add_pending_after_successor,
    };
    for (const auto mutation : successor_mutations) {
        CleanupWaveFixture root("worker-cleanup-authorization-successor-drift-" +
                                    std::to_string(static_cast<std::size_t>(mutation)),
                                fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
        const auto merged_snapshot = capture_merged_corpus(root.merged_base());
        AuthorizationPublicationMutationContext context{
            .mutation = mutation,
            .canonical = root.authorization_canonical_path(0U),
            .pending = root.authorization_pending_path(0U),
            .saved = root.prepared().root().parent_path() /
                     (root.prepared().root().filename().string() + "-saved-authorization"),
            .expected_bytes = root.authorization_bytes(0U),
        };

        auto rejected = cleanup_authority::trusted_test::
            resume_distributed_sieve_worker_cleanup_authorization_v1_with_hooks(
                root.take_admission(), CleanupAuthorizationPublicationHooks{
                                           .stop_after = mutate_authorization_publication,
                                           .context = &context,
                                       });

        CHECK(context.invoked);
        CHECK(!context.failure);
        CHECK(!rejected);
        CHECK(!rejected.retryable_completion_continuation.has_value());
        CHECK(!rejected.retryable_recovery_root.has_value());
        CHECK(!rejected.completion_reconciliation_root.has_value());
        CHECK(!rejected.authorization_published.has_value());
        CHECK(!rejected.all_workers_completed.has_value());
        CHECK(rejected.diagnostic.status == Status::successor_mismatch);
        CHECK(rejected.diagnostic.disposition == Disposition::cold_reopen_required);
        CHECK(rejected.diagnostic.target_shape == TargetShape::absent);
        CHECK(rejected.diagnostic.publication_status ==
              durable_record::RecordPublishStatus::durable);
        CHECK(rejected.diagnostic.publication_disposition ==
              durable_record::RecordPublishDisposition::created);
        CHECK(rejected.diagnostic.authority_spent);
        CHECK(rejected.diagnostic.publication_started);
        CHECK(std::filesystem::exists(context.canonical));
        CHECK(fixture::read_file_bytes(context.canonical) == context.expected_bytes);
        if (mutation == AuthorizationPublicationMutationV1::replace_canonical_after_successor) {
            CHECK(std::filesystem::exists(context.saved));
            CHECK(fixture::read_file_bytes(context.saved) == context.expected_bytes);
            CHECK(fixture::native_identity(context.saved) !=
                  fixture::native_identity(context.canonical));
            CHECK(!std::filesystem::exists(context.pending));
        } else {
            CHECK(std::filesystem::exists(context.pending));
            CHECK(fixture::read_file_bytes(context.pending) == context.expected_bytes);
        }
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);

        root.cold_reopen("cold-replay successor authorization drift");
        auto recovered =
            cleanup_authority::resume_distributed_sieve_worker_cleanup_authorization_v1(
                root.take_admission());
        require_authorization_publication_success(
            recovered, root, 0U,
            mutation == AuthorizationPublicationMutationV1::add_pending_after_successor
                ? TargetShape::authorization_identical_dual
                : TargetShape::authorization_canonical_only,
            durable_record::RecordPublishDisposition::confirmed_existing);
        require_merged_corpus(root.merged_base(), merged_snapshot);
    }
#else
    throw TestFailure("authorization publisher requires the macOS private-handoff runtime");
#endif
}

void test_authorization_publisher_is_process_bound_and_preserves_lock_boundaries() {
#if defined(__APPLE__)
    using Phase = cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationPhaseV1;
    using Status = cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationStatusV1;
    using Disposition =
        cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationDispositionV1;
    using TargetShape =
        cleanup_authority::DistributedSieveWorkerCleanupAuthorizationPublicationTargetShapeV1;

    CleanupWaveFixture root("worker-cleanup-authorization-publisher-fork",
                            fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
    const auto merged_snapshot = capture_merged_corpus(root.merged_base());
    auto first = cleanup_authority::resume_distributed_sieve_worker_cleanup_authorization_v1(
        root.take_admission());
    require_authorization_publication_success(first, root, 0U, TargetShape::absent,
                                              durable_record::RecordPublishDisposition::created);
    auto completion = complete_authorized_worker(take_authorization_publication(first), 0U);
    CHECK(completion.valid());
    require_cleanup_coordinate_records_absent(root, 2U);
    require_all_base_locks_free(root);
    require_wave_lock_busy(root);

    CleanupCompletionPublishedContinuation moved_completion(std::move(completion));
    CHECK(moved_completion.valid());
    const pid_t child = ::fork();
    if (child < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "fork authorization-publisher process-bound test");
    }
    if (child == 0) {
        auto rejected =
            cleanup_authority::advance_distributed_sieve_worker_cleanup_authorization_v1(
                std::move(moved_completion));
        const bool correct =
            !rejected && !rejected.retryable_completion_continuation.has_value() &&
            !rejected.retryable_recovery_root.has_value() &&
            !rejected.completion_reconciliation_root.has_value() &&
            !rejected.authorization_published.has_value() &&
            !rejected.all_workers_completed.has_value() &&
            rejected.diagnostic.phase == Phase::input_validation &&
            rejected.diagnostic.status == Status::process_mismatch &&
            rejected.diagnostic.disposition == Disposition::cold_reopen_required &&
            rejected.diagnostic.native_error == std::make_error_code(std::errc::no_such_process) &&
            !rejected.diagnostic.authority_spent && !rejected.diagnostic.publication_started &&
            !std::filesystem::exists(root.authorization_pending_path(2U)) &&
            !std::filesystem::exists(root.authorization_canonical_path(2U));
        ::_exit(correct ? AUTHORIZATION_PUBLISHER_FORK_REJECTED_EXIT : EXIT_FAILURE);
    }

    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "wait for authorization-publisher process-bound child");
    }
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == AUTHORIZATION_PUBLISHER_FORK_REJECTED_EXIT);
    CHECK(moved_completion.valid());
    require_cleanup_coordinate_records_absent(root, 2U);
    require_merged_corpus(root.merged_base(), merged_snapshot);
    require_all_base_locks_free(root);
    require_wave_lock_busy(root);

    auto next = cleanup_authority::advance_distributed_sieve_worker_cleanup_authorization_v1(
        std::move(moved_completion));
    require_authorization_publication_success(next, root, 2U, TargetShape::absent,
                                              durable_record::RecordPublishDisposition::created);
    require_merged_corpus(root.merged_base(), merged_snapshot);
    require_all_base_locks_free(root);
    require_wave_lock_busy(root);
#else
    throw TestFailure("authorization publisher requires the macOS private-handoff runtime");
#endif
}

void run_core_suite() {
#if defined(__APPLE__)
    test_authorization_publisher_cold_initial_and_fresh_next_cross_empty_chunks();
    std::cout << "  cold initial and fresh next authorization cross empty chunks: PASS\n";
    test_authorization_publisher_recovers_pending_canonical_and_identical_dual();
    std::cout << "  authorization pending/canonical/identical-dual recovery: PASS\n";
    test_authorization_publisher_routes_three_workers_and_returns_terminal_view();
    std::cout << "  three-worker route and terminal continuation: PASS\n";
#else
    throw TestFailure("core suite requires the macOS private-handoff runtime");
#endif
}

void run_publication_crash_suite() {
#if defined(__APPLE__)
    test_authorization_publisher_fault_prefixes_cold_replay();
    std::cout << "  authorization durable fault prefixes and cold replay: PASS\n";
    test_authorization_publisher_completion_prefixes_are_zero_mutation();
    std::cout << "  completion pending/identical-dual zero-mutation routing: PASS\n";
#else
    throw TestFailure("publication-crash suite requires the macOS private-handoff runtime");
#endif
}

void run_protection_suite() {
#if defined(__APPLE__)
    test_authorization_publisher_rejects_replacement_and_successor_drift();
    std::cout << "  authorization replacement and successor drift rejection: PASS\n";
    test_authorization_publisher_is_process_bound_and_preserves_lock_boundaries();
    std::cout << "  process boundary, WaveLock, and released BaseLocks: PASS\n";
#else
    throw TestFailure("protection suite requires the macOS private-handoff runtime");
#endif
}

void run_platform_suite() {
#if defined(__APPLE__)
    throw TestFailure("platform suite is reserved for unsupported hosts");
#else
    static_assert(!std::is_constructible_v<CleanupAdmission, std::filesystem::path>);
    static_assert(
        !std::is_constructible_v<CleanupCompletionPublishedContinuation, CleanupAdmission&&>);
    std::cout << "  authorization publisher remains unreachable without a sealed input: PASS\n";
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
            run_core_suite();
            run_publication_crash_suite();
            run_protection_suite();
#else
            run_platform_suite();
#endif
            return EXIT_SUCCESS;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--suite") {
            const std::string_view suite = argv[2];
            if (suite == "core") {
                run_core_suite();
                return EXIT_SUCCESS;
            }
            if (suite == "publication-crash") {
                run_publication_crash_suite();
                return EXIT_SUCCESS;
            }
            if (suite == "protection") {
                run_protection_suite();
                return EXIT_SUCCESS;
            }
            if (suite == "platform") {
                run_platform_suite();
                return EXIT_SUCCESS;
            }
        }
        std::cerr << "usage: " << argv[0]
                  << " [--suite core|publication-crash|protection|platform]\n";
        return EXIT_FAILURE;
    } catch (const std::exception& failure) {
        std::cerr << "Distributed sieve worker-cleanup authorization publisher tests FAILED: "
                  << failure.what() << '\n';
        return EXIT_FAILURE;
    }
}
