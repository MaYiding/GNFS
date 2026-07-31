#include "distributed_sieve_merge_commit_authority_internal.hpp"
#include "distributed_sieve_merge_prepared_admission_internal.hpp"
#include "distributed_sieve_wave_store_internal.hpp"

#include "support/child_process.hpp"

#if defined(__APPLE__)
#include "support/distributed_sieve_wave_merge_commit_fixture.hpp"
#endif

#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/sieve/distributed_sieve_protocol.hpp>
#include <gnfs/util/sha256.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
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
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace commit_authority = gnfs::sieve::distributed_sieve_merge_commit_authority_detail;
namespace writer_authority = gnfs::sieve::distributed_sieve_merge_writer_authority_detail;
namespace relation = gnfs::relation;
namespace sieve = gnfs::sieve;
namespace wave = gnfs::sieve::distributed_sieve_resume_detail;

using CommitResult = commit_authority::DistributedSieveWaveMergeCommitResultV1;
using CommittedTail = commit_authority::DistributedSieveCommittedTailAdmissionV1;
using Digest = gnfs::util::Sha256Digest;
using PreparedAdmission = writer_authority::DistributedSieveMergePreparedAdmissionV1;

template <typename T>
concept HasCleanupMember = requires(T& value) { value.cleanup(); };

template <typename T>
concept HasWriterMember = requires(T& value) { value.writer(); };

static_assert(std::is_final_v<PreparedAdmission>);
static_assert(!std::is_default_constructible_v<PreparedAdmission>);
static_assert(!std::is_copy_constructible_v<PreparedAdmission>);
static_assert(std::is_nothrow_move_constructible_v<PreparedAdmission>);
static_assert(!std::is_constructible_v<PreparedAdmission, sieve::MergePreparedV1>);
static_assert(std::is_final_v<CommittedTail>);
static_assert(!std::is_default_constructible_v<CommittedTail>);
static_assert(!std::is_copy_constructible_v<CommittedTail>);
static_assert(std::is_nothrow_move_constructible_v<CommittedTail>);
static_assert(!HasCleanupMember<CommittedTail>);
static_assert(!HasWriterMember<CommittedTail>);
static_assert(noexcept(commit_authority::consume_distributed_sieve_merge_prepared_v1(
    std::declval<PreparedAdmission&&>())));
static_assert(std::is_same_v<decltype(commit_authority::consume_distributed_sieve_merge_prepared_v1(
                                 std::declval<PreparedAdmission&&>())),
                             CommitResult>);
static_assert(
    noexcept(commit_authority::trusted_test::consume_distributed_sieve_merge_prepared_v1_with_hooks(
        std::declval<PreparedAdmission&&>(),
        std::declval<wave::DistributedSieveWaveMergeCommitTestHooksV1>())));

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

[[nodiscard]] std::string
wave_diagnostic_detail(const wave::DistributedSieveWaveStoreDiagnostic& diagnostic) {
    std::string detail(wave::distributed_sieve_wave_store_status_name(diagnostic.status));
    if (diagnostic.native_error) {
        detail.append(": ");
        detail.append(diagnostic.native_error.message());
    }
    return detail;
}

[[nodiscard]] std::string commit_diagnostic_detail(
    const commit_authority::DistributedSieveWaveMergeCommitDiagnosticV1& diagnostic) {
    std::string detail(
        commit_authority::distributed_sieve_wave_merge_commit_status_name(diagnostic.status));
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

[[nodiscard]] std::string digest_hex(const Digest& digest) {
    const auto encoded = gnfs::util::encode_sha256_hex(digest);
    return std::string(encoded.data(), encoded.size());
}

#if defined(__APPLE__)

namespace fixture = gnfs::test::distributed_sieve_wave_merge_commit_fixture;

using WorkerSnapshot = fixture::WorkerArtifactSnapshot;

inline constexpr int CRASH_CHILD_EXIT_BASE = 100;
inline constexpr int BUSY_CHILD_EXIT = 77;

std::string test_executable;

[[nodiscard]] std::filesystem::path commit_path(const std::filesystem::path& root) {
    return root / std::string(wave::DISTRIBUTED_SIEVE_WAVE_MERGE_COMMIT_RECORD_LEAF);
}

[[nodiscard]] std::filesystem::path commit_pending_path(const std::filesystem::path& root) {
    return root / std::string(wave::DISTRIBUTED_SIEVE_WAVE_MERGE_COMMIT_RECORD_PENDING_LEAF);
}

enum class PredecessorRecordKind : std::uint8_t {
    WorkerAttempt,
    MergeStarted,
};

inline constexpr std::array PREDECESSOR_RECORD_KINDS{
    PredecessorRecordKind::WorkerAttempt,
    PredecessorRecordKind::MergeStarted,
};

[[nodiscard]] std::string_view predecessor_record_label(PredecessorRecordKind kind) noexcept {
    switch (kind) {
    case PredecessorRecordKind::WorkerAttempt:
        return "worker-attempt";
    case PredecessorRecordKind::MergeStarted:
        return "merge-started";
    }
    return "unknown";
}

[[nodiscard]] std::filesystem::path
predecessor_record_path(const fixture::PreparedWaveFixture& prepared, PredecessorRecordKind kind) {
    switch (kind) {
    case PredecessorRecordKind::WorkerAttempt: {
        const auto& chunk = prepared.manifest().chunks.front();
        const auto names = wave::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        CHECK(names.has_value());
        return prepared.root() / names->canonical_record_leaf;
    }
    case PredecessorRecordKind::MergeStarted: {
        const auto names = wave::distributed_sieve_merge_generation_names_v1(
            prepared.merge_started().merge_attempt_ordinal);
        CHECK(names.has_value());
        return prepared.root() / names->canonical_record_leaf;
    }
    }
    fail("known predecessor record kind", __LINE__);
}

[[nodiscard]] std::filesystem::path
displaced_predecessor_record_path(const fixture::PreparedWaveFixture& prepared,
                                  PredecessorRecordKind kind, std::string_view phase) {
    return prepared.root().parent_path() /
           (prepared.root().filename().string() + ".displaced-" +
            std::string(predecessor_record_label(kind)) + "-" + std::string(phase));
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

[[nodiscard]] sieve::WaveMergeCommitV1
expected_commit_from_prepared(const sieve::MergePreparedV1& prepared) {
    CHECK(prepared.ordered_inputs.size() == prepared.per_chunk_retained_counts.size());
    sieve::WaveMergeCommitV1 commit;
    commit.manifest_digest = prepared.manifest_digest;
    commit.work_digest = prepared.work_digest;
    commit.chunks.reserve(prepared.ordered_inputs.size());
    for (std::size_t index = 0; index < prepared.ordered_inputs.size(); ++index) {
        CHECK(prepared.per_chunk_retained_counts[index].chunk_id ==
              prepared.ordered_inputs[index].chunk_id);
        commit.chunks.push_back({
            .input = prepared.ordered_inputs[index],
            .retained_relation_count =
                prepared.per_chunk_retained_counts[index].retained_relation_count,
            .diagnostic = {},
        });
    }
    commit.merge_policy_version = prepared.merge_policy_version;
    commit.input_relation_count = prepared.input_relation_count;
    commit.duplicate_relation_count = prepared.duplicate_relation_count;
    commit.output_relation_count = prepared.output_relation_count;
    commit.merge_prepared_digest = prepared.self_digest;
    commit.merged_lease = prepared.merged_lease;
    commit.merged_artifact = prepared.merged_artifact;
    return fixture::seal_value(std::move(commit));
}

[[nodiscard]] relation::OOCSnapshotDescriptor
relation_descriptor(const sieve::CorpusArtifactV1& artifact) noexcept {
    return {
        .format_version = artifact.descriptor.format_version,
        .store_id = artifact.descriptor.store_id,
        .generation = artifact.descriptor.generation,
        .count = artifact.descriptor.relation_count,
        .data_end = artifact.descriptor.data_end,
    };
}

void require_merged_corpus(const fixture::PreparedWaveFixture& prepared,
                           const sieve::WaveMergeCommitV1& commit) {
    const auto names = wave::distributed_sieve_merge_generation_names_v1(
        prepared.merge_started().merge_attempt_ordinal);
    CHECK(names.has_value());
    const auto base = prepared.root() / names->private_directory_leaf / "corpus";
    relation::OOCRelationReader reader(base.string(), relation_descriptor(commit.merged_artifact));
    CHECK(reader.valid());
    CHECK(reader.count() == prepared.expected_rows().size());
    const auto rows = reader.read_all();
    CHECK(fixture::relation_vectors_equal(rows, prepared.expected_rows()));
}

[[nodiscard]] std::vector<std::byte> require_committed_tail(
    const fixture::PreparedWaveFixture& prepared, const sieve::MergePreparedV1& prepared_record,
    const std::array<WorkerSnapshot, 2>& worker_snapshots, const CommittedTail& tail) {
    CHECK(tail.valid());
    const auto expected = expected_commit_from_prepared(prepared_record);
    const std::array merge_starts{prepared.merge_started()};
    const auto dependency = sieve::validate_merge_predecessor_chain(
        prepared.manifest(), merge_starts, &prepared_record, &tail.record());
    if (!dependency) {
        fail("validate committed predecessor chain", __LINE__,
             sieve::distributed_sieve_protocol_error_name(dependency.error));
    }
    const auto expected_bytes = fixture::encode_record(sieve::DistributedSieveProtocolRecordV1{
        expected,
    });
    const auto actual_bytes = fixture::encode_record(sieve::DistributedSieveProtocolRecordV1{
        tail.record(),
    });
    CHECK(actual_bytes == expected_bytes);
    CHECK(fixture::read_file_bytes(commit_path(prepared.root())) == expected_bytes);
    CHECK(!std::filesystem::exists(commit_pending_path(prepared.root())));
    CHECK(tail.canonical_snapshot().size == expected_bytes.size());
    require_merged_corpus(prepared, tail.record());
    require_worker_snapshots(prepared, worker_snapshots);
    return actual_bytes;
}

[[nodiscard]] CommittedTail take_committed_tail(CommitResult result, std::string_view context) {
    if (!result || !result.committed_tail.has_value()) {
        fail(context, __LINE__, commit_diagnostic_detail(result.diagnostic));
    }
    CHECK(!result.retryable_prepared.has_value());
    return std::move(*result.committed_tail);
}

[[nodiscard]] CommittedTail open_or_finish_commit(const std::filesystem::path& root,
                                                  const Digest& manifest_digest) {
    auto opened = wave::DistributedSieveWaveStore::open(root, manifest_digest);
    if (!opened) {
        fail("cold-open WaveMergeCommit tail", __LINE__, wave_diagnostic_detail(opened.diagnostic));
    }
    CHECK(opened.store == nullptr);
    const bool prepared = opened.prepared_admission.has_value();
    const bool committed = opened.committed_tail_admission.has_value();
    CHECK(prepared != committed);
    if (committed) {
        return std::move(*opened.committed_tail_admission);
    }
    auto result = commit_authority::consume_distributed_sieve_merge_prepared_v1(
        std::move(*opened.prepared_admission));
    return take_committed_tail(std::move(result), "finish recovered WaveMergeCommit");
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
                                    "write replacement commit");
        }
        written += static_cast<std::size_t>(result);
    }
}

void replace_leaf_with_same_bytes(const std::filesystem::path& root,
                                  const std::filesystem::path& canonical,
                                  const std::filesystem::path& saved,
                                  std::span<const std::byte> bytes) {
    if (::rename(canonical.c_str(), saved.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(), "save canonical merge commit");
    }
    int descriptor = -1;
    do {
        descriptor =
            ::open(canonical.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "create replacement merge commit");
    }
    try {
        write_all(descriptor, bytes);
        if (::fsync(descriptor) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "sync replacement merge commit");
        }
    } catch (...) {
        (void)::close(descriptor);
        throw;
    }
    if (::close(descriptor) != 0) {
        throw std::system_error(errno, std::generic_category(), "close replacement merge commit");
    }
    int root_descriptor = -1;
    do {
        root_descriptor = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (root_descriptor < 0 && errno == EINTR);
    if (root_descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "open replacement commit root");
    }
    const int sync_result = ::fsync(root_descriptor);
    const int sync_error = errno;
    (void)::close(root_descriptor);
    if (sync_result != 0) {
        throw std::system_error(sync_error, std::generic_category(),
                                "sync replacement commit root");
    }
}

void replace_commit_with_same_bytes(const std::filesystem::path& root,
                                    const std::filesystem::path& saved,
                                    std::span<const std::byte> bytes) {
    replace_leaf_with_same_bytes(root, commit_path(root), saved, bytes);
}

struct SameByteReplacementContext final {
    std::filesystem::path root;
    std::filesystem::path canonical;
    std::filesystem::path saved;
    std::vector<std::byte> bytes;
    bool invoked = false;
    std::exception_ptr failure;
};

void replace_leaf_at_boundary(void* opaque) noexcept {
    auto& context = *static_cast<SameByteReplacementContext*>(opaque);
    context.invoked = true;
    try {
        replace_leaf_with_same_bytes(context.root, context.canonical, context.saved, context.bytes);
    } catch (...) {
        context.failure = std::current_exception();
    }
}

struct CommitFaultContext final {
    wave::DistributedSieveWaveMergeCommitFaultPointV1 target =
        wave::DistributedSieveWaveMergeCommitFaultPointV1::PendingDurable;
    bool terminate_process = false;
    int process_exit_code = EXIT_FAILURE;
    bool invoked = false;
};

[[nodiscard]] bool stop_commit_at(wave::DistributedSieveWaveMergeCommitFaultPointV1 point,
                                  void* opaque) noexcept {
    auto& context = *static_cast<CommitFaultContext*>(opaque);
    if (point != context.target) {
        return false;
    }
    context.invoked = true;
    if (context.terminate_process) {
        ::_exit(context.process_exit_code);
    }
    return true;
}

[[nodiscard]] int crash_child(std::uint32_t point_index, const std::filesystem::path& root,
                              const Digest& manifest_digest) {
    if (point_index >=
        static_cast<std::uint32_t>(wave::DistributedSieveWaveMergeCommitFaultPointV1::Count)) {
        return EXIT_FAILURE;
    }
    auto opened = wave::DistributedSieveWaveStore::open(root, manifest_digest);
    if (!opened || opened.store != nullptr || !opened.prepared_admission.has_value() ||
        opened.committed_tail_admission.has_value()) {
        return EXIT_FAILURE;
    }
    CommitFaultContext context{
        .target = static_cast<wave::DistributedSieveWaveMergeCommitFaultPointV1>(point_index),
        .terminate_process = true,
        .process_exit_code = CRASH_CHILD_EXIT_BASE + static_cast<int>(point_index),
    };
    (void)commit_authority::trusted_test::consume_distributed_sieve_merge_prepared_v1_with_hooks(
        std::move(*opened.prepared_admission), wave::DistributedSieveWaveMergeCommitTestHooksV1{
                                                   .stop_after = stop_commit_at,
                                                   .context = &context,
                                               });
    ::_exit(EXIT_FAILURE);
}

[[nodiscard]] int busy_probe_child(const std::filesystem::path& root,
                                   const Digest& manifest_digest) {
    auto opened = wave::DistributedSieveWaveStore::open(root, manifest_digest);
    const bool busy = !opened && opened.store == nullptr &&
                      !opened.prepared_admission.has_value() &&
                      !opened.committed_tail_admission.has_value() &&
                      opened.diagnostic.status == wave::DistributedSieveWaveStoreStatus::lock_busy;
    return busy ? BUSY_CHILD_EXIT : EXIT_FAILURE;
}

void test_fresh_commit_and_cold_reopen_are_byte_exact() {
    fixture::PreparedWaveFixture prepared("fresh-byte-parity");
    auto publication = prepared.prepare_fresh();
    CHECK(publication.admission.has_value());
    const auto prepared_record = publication.admission->record();
    const auto worker_snapshots = capture_worker_snapshots(prepared);

    auto result = commit_authority::consume_distributed_sieve_merge_prepared_v1(
        std::move(*publication.admission));
    CHECK(!publication.admission->valid());
    auto tail = take_committed_tail(std::move(result), "consume fresh prepared admission");
    const auto fresh_bytes =
        require_committed_tail(prepared, prepared_record, worker_snapshots, tail);

    std::optional<CommittedTail> held(std::move(tail));
    held.reset();
    auto reopened =
        wave::DistributedSieveWaveStore::open(prepared.root(), prepared.manifest_digest());
    if (!reopened || !reopened.committed_tail_admission.has_value()) {
        fail("cold-open existing WaveMergeCommit", __LINE__,
             wave_diagnostic_detail(reopened.diagnostic));
    }
    CHECK(reopened.store == nullptr);
    CHECK(!reopened.prepared_admission.has_value());
    const auto cold_bytes = require_committed_tail(prepared, prepared_record, worker_snapshots,
                                                   *reopened.committed_tail_admission);
    CHECK(cold_bytes == fresh_bytes);
}

void test_cold_prepared_admission_commits_without_worker_cleanup() {
    fixture::PreparedWaveFixture prepared("cold-prepared");
    auto publication = prepared.prepare_fresh();
    CHECK(publication.admission.has_value());
    const auto prepared_record = publication.admission->record();
    const auto worker_snapshots = capture_worker_snapshots(prepared);
    publication.admission.reset();

    auto opened =
        wave::DistributedSieveWaveStore::open(prepared.root(), prepared.manifest_digest());
    if (!opened || !opened.prepared_admission.has_value()) {
        fail("cold-open canonical MergePrepared", __LINE__,
             wave_diagnostic_detail(opened.diagnostic));
    }
    CHECK(opened.store == nullptr);
    CHECK(!opened.committed_tail_admission.has_value());
    auto result = commit_authority::consume_distributed_sieve_merge_prepared_v1(
        std::move(*opened.prepared_admission));
    auto tail = take_committed_tail(std::move(result), "consume cold prepared admission");
    (void)require_committed_tail(prepared, prepared_record, worker_snapshots, tail);
}

void test_moved_admission_replay_is_rejected_without_mutation() {
    fixture::PreparedWaveFixture prepared("moved-replay");
    auto publication = prepared.prepare_fresh();
    CHECK(publication.admission.has_value());
    const auto prepared_record = publication.admission->record();
    const auto worker_snapshots = capture_worker_snapshots(prepared);

    auto committed = commit_authority::consume_distributed_sieve_merge_prepared_v1(
        std::move(*publication.admission));
    auto tail = take_committed_tail(std::move(committed), "commit before replay");
    const auto commit_bytes =
        require_committed_tail(prepared, prepared_record, worker_snapshots, tail);
    const auto commit_identity = fixture::native_identity(commit_path(prepared.root()));

    auto replay = commit_authority::consume_distributed_sieve_merge_prepared_v1(
        std::move(*publication.admission));
    CHECK(!replay);
    CHECK(!replay.retryable_prepared.has_value());
    CHECK(!replay.committed_tail.has_value());
    CHECK(replay.diagnostic.phase ==
          commit_authority::DistributedSieveWaveMergeCommitPhaseV1::admission_validation);
    CHECK(replay.diagnostic.status ==
          commit_authority::DistributedSieveWaveMergeCommitStatusV1::invalid_admission);
    CHECK(!replay.diagnostic.admission_spent);
    CHECK(!replay.diagnostic.reconciliation_required);
    CHECK(fixture::read_file_bytes(commit_path(prepared.root())) == commit_bytes);
    CHECK(fixture::native_identity(commit_path(prepared.root())) == commit_identity);
    require_worker_snapshots(prepared, worker_snapshots);
}

void test_commit_fault_prefixes_return_interrupted_and_recover_in_process() {
    constexpr std::array fault_points{
        wave::DistributedSieveWaveMergeCommitFaultPointV1::PendingDurable,
        wave::DistributedSieveWaveMergeCommitFaultPointV1::CanonicalPromoted,
        wave::DistributedSieveWaveMergeCommitFaultPointV1::CanonicalDurable,
    };
    for (std::size_t index = 0; index < fault_points.size(); ++index) {
        fixture::PreparedWaveFixture prepared("interrupted-" + std::to_string(index));
        auto publication = prepared.prepare_fresh();
        CHECK(publication.admission.has_value());
        const auto prepared_record = publication.admission->record();
        const auto worker_snapshots = capture_worker_snapshots(prepared);
        CommitFaultContext context{.target = fault_points[index]};

        auto interrupted =
            commit_authority::trusted_test::consume_distributed_sieve_merge_prepared_v1_with_hooks(
                std::move(*publication.admission), wave::DistributedSieveWaveMergeCommitTestHooksV1{
                                                       .stop_after = stop_commit_at,
                                                       .context = &context,
                                                   });
        CHECK(context.invoked);
        CHECK(!interrupted);
        CHECK(!interrupted.retryable_prepared.has_value());
        CHECK(!interrupted.committed_tail.has_value());
        CHECK(interrupted.diagnostic.phase ==
              commit_authority::DistributedSieveWaveMergeCommitPhaseV1::publication);
        CHECK(interrupted.diagnostic.status ==
              commit_authority::DistributedSieveWaveMergeCommitStatusV1::publication_failed);
        CHECK(interrupted.diagnostic.wave_store.status ==
              wave::DistributedSieveWaveStoreStatus::interrupted);
        CHECK(interrupted.diagnostic.wave_store.last_wave_merge_commit_fault_point.has_value());
        CHECK(*interrupted.diagnostic.wave_store.last_wave_merge_commit_fault_point ==
              fault_points[index]);
        CHECK(interrupted.diagnostic.admission_spent);
        CHECK(interrupted.diagnostic.reconciliation_required);

        auto tail = open_or_finish_commit(prepared.root(), prepared.manifest_digest());
        (void)require_committed_tail(prepared, prepared_record, worker_snapshots, tail);
    }
}

void test_commit_fault_prefixes_recover_after_process_death() {
    constexpr std::array fault_points{
        wave::DistributedSieveWaveMergeCommitFaultPointV1::PendingDurable,
        wave::DistributedSieveWaveMergeCommitFaultPointV1::CanonicalPromoted,
        wave::DistributedSieveWaveMergeCommitFaultPointV1::CanonicalDurable,
    };
    for (std::size_t index = 0; index < fault_points.size(); ++index) {
        fixture::PreparedWaveFixture prepared("crash-" + std::to_string(index));
        auto publication = prepared.prepare_fresh();
        CHECK(publication.admission.has_value());
        const auto prepared_record = publication.admission->record();
        const auto worker_snapshots = capture_worker_snapshots(prepared);
        publication.admission.reset();

        const auto child = gnfs::test::run_child_process(
            test_executable,
            {"--crash-commit", std::to_string(static_cast<std::uint32_t>(fault_points[index])),
             prepared.root().string(), digest_hex(prepared.manifest_digest())});
        CHECK(child.exited);
        CHECK(!child.signaled);
        CHECK(child.exit_code == CRASH_CHILD_EXIT_BASE + static_cast<int>(index));

        auto tail = open_or_finish_commit(prepared.root(), prepared.manifest_digest());
        (void)require_committed_tail(prepared, prepared_record, worker_snapshots, tail);
    }
}

void test_same_byte_commit_replacement_fails_closed() {
    fixture::PreparedWaveFixture prepared("commit-replacement");
    auto publication = prepared.prepare_fresh();
    CHECK(publication.admission.has_value());
    const auto prepared_record = publication.admission->record();
    const auto worker_snapshots = capture_worker_snapshots(prepared);
    const auto canonical_bytes = fixture::encode_record(sieve::DistributedSieveProtocolRecordV1{
        expected_commit_from_prepared(prepared_record),
    });
    SameByteReplacementContext context{
        .root = prepared.root(),
        .canonical = commit_path(prepared.root()),
        .saved = prepared.root().parent_path() /
                 (prepared.root().filename().string() + ".displaced-merge-commit"),
        .bytes = canonical_bytes,
    };
    auto result =
        commit_authority::trusted_test::consume_distributed_sieve_merge_prepared_v1_with_hooks(
            std::move(*publication.admission),
            wave::DistributedSieveWaveMergeCommitTestHooksV1{
                .after_first_successor_validation = replace_leaf_at_boundary,
                .context = &context,
            });
    CHECK(context.invoked);
    if (context.failure) {
        std::rethrow_exception(context.failure);
    }
    CHECK(!result);
    CHECK(!result.retryable_prepared.has_value());
    CHECK(!result.committed_tail.has_value());
    CHECK(result.diagnostic.phase ==
          commit_authority::DistributedSieveWaveMergeCommitPhaseV1::canonical_revalidation);
    CHECK(result.diagnostic.status ==
          commit_authority::DistributedSieveWaveMergeCommitStatusV1::publication_failed);
    CHECK(result.diagnostic.wave_store.status ==
          wave::DistributedSieveWaveStoreStatus::namespace_conflict);
    CHECK(result.diagnostic.admission_spent);
    CHECK(result.diagnostic.reconciliation_required);

    const auto replacement_identity = fixture::native_identity(commit_path(prepared.root()));
    const auto displaced_identity = fixture::native_identity(context.saved);
    CHECK(replacement_identity != displaced_identity);
    CHECK(fixture::read_file_bytes(commit_path(prepared.root())) == canonical_bytes);
    CHECK(fixture::read_file_bytes(context.saved) == canonical_bytes);
    CHECK(fixture::native_identity(commit_path(prepared.root())) == replacement_identity);
    CHECK(fixture::native_identity(context.saved) == displaced_identity);
    require_worker_snapshots(prepared, worker_snapshots);
}

void test_predecessor_replacements_before_publication_fail_closed() {
    for (const auto kind : PREDECESSOR_RECORD_KINDS) {
        fixture::PreparedWaveFixture prepared("predecessor-before-" +
                                              std::string(predecessor_record_label(kind)));
        auto publication = prepared.prepare_fresh();
        CHECK(publication.admission.has_value());
        const auto prepared_record = publication.admission->record();
        const auto worker_snapshots = capture_worker_snapshots(prepared);
        const auto canonical = predecessor_record_path(prepared, kind);
        const auto bytes = fixture::read_file_bytes(canonical);
        SameByteReplacementContext context{
            .root = prepared.root(),
            .canonical = canonical,
            .saved = displaced_predecessor_record_path(prepared, kind, "before-publication"),
            .bytes = bytes,
        };

        auto result =
            commit_authority::trusted_test::consume_distributed_sieve_merge_prepared_v1_with_hooks(
                std::move(*publication.admission),
                wave::DistributedSieveWaveMergeCommitTestHooksV1{
                    .before_record_publication = replace_leaf_at_boundary,
                    .context = &context,
                });
        CHECK(context.invoked);
        if (context.failure) {
            std::rethrow_exception(context.failure);
        }
        CHECK(!result);
        CHECK(result.retryable_prepared.has_value());
        CHECK(!result.committed_tail.has_value());
        CHECK(result.diagnostic.wave_store.status ==
              wave::DistributedSieveWaveStoreStatus::namespace_conflict);
        CHECK(!result.diagnostic.admission_spent);
        CHECK(!result.diagnostic.reconciliation_required);
        CHECK(!std::filesystem::exists(commit_path(prepared.root())));
        CHECK(!std::filesystem::exists(commit_pending_path(prepared.root())));
        CHECK(fixture::read_file_bytes(canonical) == bytes);
        CHECK(fixture::read_file_bytes(context.saved) == bytes);
        CHECK(fixture::native_identity(canonical) != fixture::native_identity(context.saved));
        require_worker_snapshots(prepared, worker_snapshots);

        result.retryable_prepared.reset();
        auto tail = open_or_finish_commit(prepared.root(), prepared.manifest_digest());
        (void)require_committed_tail(prepared, prepared_record, worker_snapshots, tail);
    }
}

void test_predecessor_replacements_after_first_successor_fail_closed() {
    for (const auto kind : PREDECESSOR_RECORD_KINDS) {
        fixture::PreparedWaveFixture prepared("predecessor-successor-" +
                                              std::string(predecessor_record_label(kind)));
        auto publication = prepared.prepare_fresh();
        CHECK(publication.admission.has_value());
        const auto prepared_record = publication.admission->record();
        const auto worker_snapshots = capture_worker_snapshots(prepared);
        const auto canonical = predecessor_record_path(prepared, kind);
        const auto bytes = fixture::read_file_bytes(canonical);
        SameByteReplacementContext context{
            .root = prepared.root(),
            .canonical = canonical,
            .saved = displaced_predecessor_record_path(prepared, kind, "after-successor"),
            .bytes = bytes,
        };

        auto result =
            commit_authority::trusted_test::consume_distributed_sieve_merge_prepared_v1_with_hooks(
                std::move(*publication.admission),
                wave::DistributedSieveWaveMergeCommitTestHooksV1{
                    .after_first_successor_validation = replace_leaf_at_boundary,
                    .context = &context,
                });
        CHECK(context.invoked);
        if (context.failure) {
            std::rethrow_exception(context.failure);
        }
        CHECK(!result);
        CHECK(!result.retryable_prepared.has_value());
        CHECK(!result.committed_tail.has_value());
        CHECK(result.diagnostic.phase ==
              commit_authority::DistributedSieveWaveMergeCommitPhaseV1::canonical_revalidation);
        CHECK(result.diagnostic.status ==
              commit_authority::DistributedSieveWaveMergeCommitStatusV1::publication_failed);
        CHECK(result.diagnostic.wave_store.status ==
              wave::DistributedSieveWaveStoreStatus::namespace_conflict);
        CHECK(result.diagnostic.admission_spent);
        CHECK(result.diagnostic.reconciliation_required);
        CHECK(fixture::read_file_bytes(canonical) == bytes);
        CHECK(fixture::read_file_bytes(context.saved) == bytes);
        CHECK(fixture::native_identity(canonical) != fixture::native_identity(context.saved));
        require_worker_snapshots(prepared, worker_snapshots);

        auto tail = open_or_finish_commit(prepared.root(), prepared.manifest_digest());
        (void)require_committed_tail(prepared, prepared_record, worker_snapshots, tail);
    }
}

void test_committed_tail_detects_late_same_byte_replacement() {
    fixture::PreparedWaveFixture prepared("tail-replacement");
    auto publication = prepared.prepare_fresh();
    CHECK(publication.admission.has_value());
    const auto prepared_record = publication.admission->record();
    const auto worker_snapshots = capture_worker_snapshots(prepared);
    auto result = commit_authority::consume_distributed_sieve_merge_prepared_v1(
        std::move(*publication.admission));
    auto tail = take_committed_tail(std::move(result), "commit before tail replacement");
    const auto canonical_bytes =
        require_committed_tail(prepared, prepared_record, worker_snapshots, tail);
    const auto displaced = prepared.root().parent_path() /
                           (prepared.root().filename().string() + ".displaced-tail-commit");

    replace_commit_with_same_bytes(prepared.root(), displaced, canonical_bytes);

    CHECK(!tail.valid());
    CHECK(fixture::read_file_bytes(commit_path(prepared.root())) == canonical_bytes);
    CHECK(fixture::read_file_bytes(displaced) == canonical_bytes);
    CHECK(fixture::native_identity(commit_path(prepared.root())) !=
          fixture::native_identity(displaced));
    require_worker_snapshots(prepared, worker_snapshots);
}

void test_committed_tail_detects_late_predecessor_replacements() {
    for (const auto kind : PREDECESSOR_RECORD_KINDS) {
        fixture::PreparedWaveFixture prepared("tail-predecessor-" +
                                              std::string(predecessor_record_label(kind)));
        auto publication = prepared.prepare_fresh();
        CHECK(publication.admission.has_value());
        const auto prepared_record = publication.admission->record();
        const auto worker_snapshots = capture_worker_snapshots(prepared);
        auto result = commit_authority::consume_distributed_sieve_merge_prepared_v1(
            std::move(*publication.admission));
        auto tail = take_committed_tail(std::move(result), "commit before predecessor replacement");
        (void)require_committed_tail(prepared, prepared_record, worker_snapshots, tail);
        const auto canonical = predecessor_record_path(prepared, kind);
        const auto bytes = fixture::read_file_bytes(canonical);
        const auto displaced = displaced_predecessor_record_path(prepared, kind, "after-tail");

        replace_leaf_with_same_bytes(prepared.root(), canonical, displaced, bytes);

        CHECK(!tail.valid());
        CHECK(fixture::read_file_bytes(canonical) == bytes);
        CHECK(fixture::read_file_bytes(displaced) == bytes);
        CHECK(fixture::native_identity(canonical) != fixture::native_identity(displaced));
        require_worker_snapshots(prepared, worker_snapshots);
    }
}

void test_committed_tail_retains_wave_lock_until_release() {
    fixture::PreparedWaveFixture prepared("wave-lock-busy");
    auto publication = prepared.prepare_fresh();
    CHECK(publication.admission.has_value());
    const auto prepared_record = publication.admission->record();
    const auto worker_snapshots = capture_worker_snapshots(prepared);
    auto result = commit_authority::consume_distributed_sieve_merge_prepared_v1(
        std::move(*publication.admission));
    auto tail = take_committed_tail(std::move(result), "commit before Busy probe");
    const auto commit_bytes =
        require_committed_tail(prepared, prepared_record, worker_snapshots, tail);
    std::optional<CommittedTail> held(std::move(tail));

    const auto child =
        gnfs::test::run_child_process(test_executable, {"--probe-busy", prepared.root().string(),
                                                        digest_hex(prepared.manifest_digest())});
    CHECK(child.exited);
    CHECK(!child.signaled);
    CHECK(child.exit_code == BUSY_CHILD_EXIT);
    CHECK(fixture::read_file_bytes(commit_path(prepared.root())) == commit_bytes);
    require_worker_snapshots(prepared, worker_snapshots);

    held.reset();
    auto reopened =
        wave::DistributedSieveWaveStore::open(prepared.root(), prepared.manifest_digest());
    if (!reopened || !reopened.committed_tail_admission.has_value()) {
        fail("reopen committed tail after Busy owner exits", __LINE__,
             wave_diagnostic_detail(reopened.diagnostic));
    }
    (void)require_committed_tail(prepared, prepared_record, worker_snapshots,
                                 *reopened.committed_tail_admission);
}

#endif

void run_core_suite() {
#if defined(__APPLE__)
    test_fresh_commit_and_cold_reopen_are_byte_exact();
    std::cout << "  fresh commit and cold reopen byte parity: PASS\n";
    test_cold_prepared_admission_commits_without_worker_cleanup();
    std::cout << "  cold prepared admission and three-slot worker preservation: PASS\n";
    test_moved_admission_replay_is_rejected_without_mutation();
    std::cout << "  moved admission replay: PASS\n";
#else
    throw TestFailure("core suite requires the macOS production consumer boundary");
#endif
}

void run_commit_crash_suite() {
#if defined(__APPLE__)
    test_commit_fault_prefixes_return_interrupted_and_recover_in_process();
    std::cout << "  pending, promoted, and durable interrupted-return recovery: PASS\n";
    test_commit_fault_prefixes_recover_after_process_death();
    std::cout << "  pending, promoted, and durable process-death recovery: PASS\n";
#else
    throw TestFailure("commit-crash suite requires the macOS production consumer boundary");
#endif
}

void run_protection_suite() {
#if defined(__APPLE__)
    test_same_byte_commit_replacement_fails_closed();
    std::cout << "  same-byte commit replacement: PASS\n";
    test_predecessor_replacements_before_publication_fail_closed();
    std::cout << "  same-byte predecessor replacement before publication: PASS\n";
    test_predecessor_replacements_after_first_successor_fail_closed();
    std::cout << "  same-byte predecessor replacement after successor validation: PASS\n";
    test_committed_tail_detects_late_same_byte_replacement();
    std::cout << "  committed-tail late same-byte replacement: PASS\n";
    test_committed_tail_detects_late_predecessor_replacements();
    std::cout << "  committed-tail late same-byte predecessor replacement: PASS\n";
    test_committed_tail_retains_wave_lock_until_release();
    std::cout << "  committed-tail WaveLock Busy: PASS\n";
#else
    throw TestFailure("protection suite requires the macOS production consumer boundary");
#endif
}

void run_platform_suite() {
#if defined(__APPLE__)
    throw TestFailure("platform suite is reserved for unsupported hosts");
#else
    static_assert(!std::is_default_constructible_v<PreparedAdmission>);
    static_assert(!std::is_constructible_v<PreparedAdmission, std::filesystem::path>);
    static_assert(!std::is_default_constructible_v<CommittedTail>);
    static_assert(!HasCleanupMember<CommittedTail>);
    static std::atomic<std::uint64_t> sequence{0};
    const auto candidate = std::filesystem::temp_directory_path() /
                           ("gnfs-wave-merge-commit-platform-unreachable-" +
                            std::to_string(static_cast<std::uint64_t>(
                                std::chrono::steady_clock::now().time_since_epoch().count())) +
                            "-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    CHECK(!std::filesystem::exists(candidate));
    std::cout << "  commit authority remains unreachable without a production admission: PASS\n";
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

        if (argc == 5 && std::string_view(argv[1]) == "--crash-commit") {
            const auto point = static_cast<std::uint32_t>(std::stoul(argv[2]));
            const auto digest = gnfs::util::decode_sha256_hex(argv[4]);
            if (!digest.has_value()) {
                return EXIT_FAILURE;
            }
            return crash_child(point, std::filesystem::path(argv[3]), *digest);
        }
        if (argc == 4 && std::string_view(argv[1]) == "--probe-busy") {
            const auto digest = gnfs::util::decode_sha256_hex(argv[3]);
            if (!digest.has_value()) {
                return EXIT_FAILURE;
            }
            return busy_probe_child(std::filesystem::path(argv[2]), *digest);
        }
#endif

        if (argc == 1) {
#if defined(__APPLE__)
            run_core_suite();
            run_commit_crash_suite();
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
            if (suite == "commit-crash") {
                run_commit_crash_suite();
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
        std::cerr << "usage: " << argv[0] << " [--suite core|commit-crash|protection|platform]\n";
        return EXIT_FAILURE;
    } catch (const std::exception& failure) {
        std::cerr << "Distributed sieve WaveMergeCommit tests FAILED: " << failure.what() << '\n';
        return EXIT_FAILURE;
    }
}
