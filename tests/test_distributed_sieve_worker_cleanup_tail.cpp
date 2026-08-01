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
#include <vector>

#if defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace cleanup_authority = gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail;
namespace commit_authority = gnfs::sieve::distributed_sieve_merge_commit_authority_detail;
namespace relation = gnfs::relation;
namespace sieve = gnfs::sieve;
namespace wave = gnfs::sieve::distributed_sieve_resume_detail;

using CommittedTail = cleanup_authority::DistributedSieveCommittedTailAdmissionV1;
using CleanupAdmission = cleanup_authority::DistributedSieveWorkerCleanupRootAdmissionV1;
using CleanupResult = cleanup_authority::DistributedSieveWorkerCleanupTailResultV1;

static_assert(std::is_final_v<CleanupAdmission>);
static_assert(!std::is_default_constructible_v<CleanupAdmission>);
static_assert(!std::is_copy_constructible_v<CleanupAdmission>);
static_assert(std::is_nothrow_move_constructible_v<CleanupAdmission>);
static_assert(std::is_final_v<CommittedTail>);
static_assert(!std::is_copy_constructible_v<CommittedTail>);
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

    const auto wave_probe = gnfs::test::run_child_process(
        test_executable,
        {"--probe-wave-lock", prepared.root().string(), digest_hex(prepared.manifest_digest())});
    CHECK(wave_probe.exited);
    CHECK(!wave_probe.signaled);
    CHECK(wave_probe.exit_code == WAVE_LOCK_BUSY_EXIT);
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
