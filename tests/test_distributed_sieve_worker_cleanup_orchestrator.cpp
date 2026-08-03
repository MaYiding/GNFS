#include "distributed_sieve_merge_commit_authority_internal.hpp"
#include "distributed_sieve_wave_store_internal.hpp"
#include "distributed_sieve_worker_cleanup_authority_internal.hpp"
#include "distributed_sieve_worker_cleanup_orchestrator_internal.hpp"

#include "support/child_process.hpp"

#if defined(__APPLE__)
#include "support/distributed_sieve_wave_merge_commit_fixture.hpp"
#endif

#include <gnfs/core/relation.hpp>
#include <gnfs/relation/ooc_cleanup_transaction.hpp>
#include <gnfs/relation/relation_corpus.hpp>
#include <gnfs/sieve/distributed_sieve_protocol.hpp>
#include <gnfs/util/sha256.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
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
#include <fcntl.h>
#include <sys/file.h>
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
using OrchestrationContinuation =
    cleanup_authority::DistributedSieveWorkerCleanupOrchestrationContinuationV1;
using RetainedMergedResult = cleanup_authority::DistributedSieveWorkerCleanupRetainedMergedResultV1;
using OrchestrationResult = cleanup_authority::DistributedSieveWorkerCleanupOrchestrationResultV1;
using OrchestrationPhase = cleanup_authority::DistributedSieveWorkerCleanupOrchestrationPhaseV1;
using OrchestrationStatus = cleanup_authority::DistributedSieveWorkerCleanupOrchestrationStatusV1;
using OrchestrationDisposition =
    cleanup_authority::DistributedSieveWorkerCleanupOrchestrationDispositionV1;
using OrchestrationStage = cleanup_authority::DistributedSieveWorkerCleanupOrchestrationStageV1;
using DriveFunction =
    decltype(&cleanup_authority::drive_distributed_sieve_worker_cleanup_to_retained_merged_v1);
using ResumeFunction =
    decltype(&cleanup_authority::resume_distributed_sieve_worker_cleanup_to_retained_merged_v1);

template <typename Value>
concept HasRootAccessor = requires(Value& value) { value.root(); };

template <typename Value>
concept HasReaderAccessor = requires(Value& value) { value.reader(); };

template <typename Value>
concept HasPathAccessor = requires(Value& value) { value.path(); };

template <typename Value>
concept HasDescriptorAccessor = requires(Value& value) { value.descriptor(); };

template <typename Value>
concept HasReceiptAccessor = requires(Value& value) { value.receipt(); };

template <typename Value>
concept HasAdmissionAccessor = requires(Value& value) { value.admission(); };

template <typename Value>
concept HasAuthorizationAccessor = requires(Value& value) { value.authorization(); };

template <typename Value>
concept HasCompletionReadyAccessor = requires(Value& value) { value.completion_ready(); };

template <typename Value>
concept HasCompletionAccessor = requires(Value& value) { value.completion(); };

template <typename Value>
concept HasIntentConversionAccessor = requires(Value& value) { value.intent_conversion(); };

template <typename Value>
concept HasCleanupAccessor = requires(Value& value) { value.cleanup(); };

template <typename Value>
concept HasCleanupArm = requires(Value& value) { value.arm_ooc_cleanup(); };

template <typename Value>
concept HasAcknowledge = requires(Value& value) { value.acknowledge(); };

template <typename Value>
concept HasWorkerLaunch = requires(Value& value) { value.launch_worker(); };

template <typename Value>
concept HasMergedRelationsOnConstLvalue = requires(const Value& value) {
    { value.merged_relations() } -> std::same_as<const relation::ReadOnlyRelationCorpusView&>;
};

template <typename Value>
concept HasMergedRelationsOnRvalue =
    requires(Value&& value) { std::move(value).merged_relations(); };

static_assert(std::is_final_v<OrchestrationContinuation>);
static_assert(!std::is_default_constructible_v<OrchestrationContinuation>);
static_assert(!std::is_copy_constructible_v<OrchestrationContinuation>);
static_assert(std::is_nothrow_move_constructible_v<OrchestrationContinuation>);
static_assert(!std::is_move_assignable_v<OrchestrationContinuation>);
static_assert(std::is_final_v<RetainedMergedResult>);
static_assert(!std::is_default_constructible_v<RetainedMergedResult>);
static_assert(!std::is_copy_constructible_v<RetainedMergedResult>);
static_assert(std::is_nothrow_move_constructible_v<RetainedMergedResult>);
static_assert(!std::is_move_assignable_v<RetainedMergedResult>);
static_assert(!std::is_copy_constructible_v<OrchestrationResult>);
static_assert(std::is_nothrow_move_constructible_v<OrchestrationResult>);
static_assert(!std::is_move_assignable_v<OrchestrationResult>);
static_assert(!std::is_invocable_v<DriveFunction, CleanupAdmission&>);
static_assert(
    std::is_nothrow_invocable_r_v<OrchestrationResult, DriveFunction, CleanupAdmission&&>);
static_assert(!std::is_invocable_v<ResumeFunction, OrchestrationContinuation&>);
static_assert(std::is_nothrow_invocable_r_v<OrchestrationResult, ResumeFunction,
                                            OrchestrationContinuation&&>);
static_assert(HasMergedRelationsOnConstLvalue<RetainedMergedResult>);
static_assert(!HasMergedRelationsOnRvalue<RetainedMergedResult>);
static_assert(!HasRootAccessor<OrchestrationContinuation>);
static_assert(!HasRootAccessor<RetainedMergedResult>);
static_assert(!HasReaderAccessor<OrchestrationContinuation>);
static_assert(!HasReaderAccessor<RetainedMergedResult>);
static_assert(!HasPathAccessor<OrchestrationContinuation>);
static_assert(!HasPathAccessor<RetainedMergedResult>);
static_assert(!HasDescriptorAccessor<OrchestrationContinuation>);
static_assert(!HasDescriptorAccessor<RetainedMergedResult>);
static_assert(!HasReceiptAccessor<OrchestrationContinuation>);
static_assert(!HasReceiptAccessor<RetainedMergedResult>);
static_assert(!HasAdmissionAccessor<OrchestrationContinuation>);
static_assert(!HasAuthorizationAccessor<OrchestrationContinuation>);
static_assert(!HasCompletionReadyAccessor<OrchestrationContinuation>);
static_assert(!HasCompletionAccessor<OrchestrationContinuation>);
static_assert(!HasIntentConversionAccessor<OrchestrationContinuation>);
static_assert(!HasCleanupAccessor<RetainedMergedResult>);
static_assert(!HasCleanupArm<RetainedMergedResult>);
static_assert(!HasAcknowledge<RetainedMergedResult>);
static_assert(!HasWorkerLaunch<OrchestrationContinuation>);
static_assert(!HasWorkerLaunch<RetainedMergedResult>);

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
using AuthorizationPublicationFault = cleanup_authority::trusted_test::
    DistributedSieveWorkerCleanupAuthorizationPublicationFaultPointV1;
using AuthorizationPublicationHooks = cleanup_authority::trusted_test::
    DistributedSieveWorkerCleanupAuthorizationPublicationTestHooksV1;
using CompletionPublicationFault =
    cleanup_authority::trusted_test::DistributedSieveWorkerCleanupCompletionPublicationFaultPointV1;
using CompletionPublicationHooks =
    cleanup_authority::trusted_test::DistributedSieveWorkerCleanupCompletionPublicationTestHooksV1;
using IntentPublicationFault =
    relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentPublicationFaultPointV2;
using IntentPublicationHooks =
    relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentPublicationTestHooksV2;
using OrchestrationTestHooks =
    cleanup_authority::trusted_test::DistributedSieveWorkerCleanupOrchestrationTestHooksV1;

inline constexpr int BASE_LOCKS_FREE_EXIT = 81;
inline constexpr int BASE_LOCK_HELD_EXIT = 82;
inline constexpr int WAVE_LOCK_BUSY_EXIT = 83;
inline constexpr int ORCHESTRATOR_FORK_REJECTED_EXIT = 84;

std::string test_executable;

template <typename Fault> struct StopContext final {
    Fault target;
    bool invoked = false;
};

[[nodiscard]] bool stop_authorization_publication_after(AuthorizationPublicationFault point,
                                                        void* opaque) noexcept {
    auto& context = *static_cast<StopContext<AuthorizationPublicationFault>*>(opaque);
    if (point != context.target) {
        return false;
    }
    context.invoked = true;
    return true;
}

[[nodiscard]] bool stop_intent_publication_after(IntentPublicationFault point,
                                                 void* opaque) noexcept {
    auto& context = *static_cast<StopContext<IntentPublicationFault>*>(opaque);
    if (point != context.target) {
        return false;
    }
    context.invoked = true;
    return true;
}

[[nodiscard]] bool stop_completion_publication_after(CompletionPublicationFault point,
                                                     void* opaque) noexcept {
    auto& context = *static_cast<StopContext<CompletionPublicationFault>*>(opaque);
    if (point != context.target) {
        return false;
    }
    context.invoked = true;
    return true;
}

[[nodiscard]] std::string digest_hex(const Digest& digest) {
    const auto encoded = gnfs::util::encode_sha256_hex(digest);
    return std::string(encoded.data(), encoded.size());
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
        fail("publish WaveMergeCommit before orchestrator fixture", __LINE__);
    }
    CHECK(!committed.retryable_prepared.has_value());
    return std::move(*committed.committed_tail);
}

class CleanupLoopFixture final {
public:
    CleanupLoopFixture(std::string_view label, fixture::PreparedWaveChunkLayoutV1 layout)
        : prepared_(label, layout) {
        auto tail = commit_fresh(prepared_);
        auto transitioned =
            cleanup_authority::consume_distributed_sieve_committed_tail_for_worker_cleanup_v1(
                std::move(tail));
        if (!transitioned || !transitioned.admission.has_value()) {
            fail("consume committed tail for orchestrator fixture", __LINE__);
        }
        CHECK(!transitioned.retryable_tail.has_value());
        CHECK(transitioned.admission->valid());
        CHECK(transitioned.admission->cleanup_prefix().coordinates.empty());
        CHECK(transitioned.admission->reader().count() == prepared_.expected_rows().size());
        CHECK(fixture::relation_vectors_equal(transitioned.admission->reader().read_all(),
                                              prepared_.expected_rows()));
        admission_.emplace(std::move(*transitioned.admission));
    }

    CleanupLoopFixture(const CleanupLoopFixture&) = delete;
    CleanupLoopFixture& operator=(const CleanupLoopFixture&) = delete;

    [[nodiscard]] fixture::PreparedWaveFixture& prepared() noexcept {
        return prepared_;
    }

    [[nodiscard]] const fixture::PreparedWaveFixture& prepared() const noexcept {
        return prepared_;
    }

    [[nodiscard]] CleanupAdmission take_admission() {
        CHECK(admission_.has_value());
        CleanupAdmission admission(std::move(*admission_));
        admission_.reset();
        CHECK(admission.valid());
        return admission;
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

private:
    fixture::PreparedWaveFixture prepared_;
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

struct RootEntrySnapshot final {
    std::string name;
    std::filesystem::file_type type = std::filesystem::file_type::none;
    std::vector<std::byte> regular_file_bytes;

    [[nodiscard]] friend bool operator==(const RootEntrySnapshot&,
                                         const RootEntrySnapshot&) = default;
};

[[nodiscard]] std::vector<RootEntrySnapshot>
capture_root_inventory(const std::filesystem::path& root) {
    std::vector<RootEntrySnapshot> entries;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        std::error_code status_error;
        const auto status = entry.symlink_status(status_error);
        if (status_error) {
            throw std::filesystem::filesystem_error("inspect orchestrator root entry", entry.path(),
                                                    status_error);
        }
        RootEntrySnapshot snapshot{
            .name = entry.path().filename().string(),
            .type = status.type(),
        };
        if (snapshot.type == std::filesystem::file_type::regular) {
            snapshot.regular_file_bytes = fixture::read_file_bytes(entry.path());
        }
        entries.push_back(std::move(snapshot));
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& left, const auto& right) { return left.name < right.name; });
    return entries;
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

void require_all_base_locks_free(const CleanupLoopFixture& root) {
    for (const auto ordinal : root.nonempty_ordinals()) {
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

void require_wave_lock_busy(const CleanupLoopFixture& root) {
    const auto probe = gnfs::test::run_child_process(
        test_executable, {"--probe-wave-lock", root.prepared().root().string(),
                          digest_hex(root.prepared().manifest_digest())});
    CHECK(probe.exited);
    CHECK(!probe.signaled);
    CHECK(probe.exit_code == WAVE_LOCK_BUSY_EXIT);
}

[[nodiscard]] RetainedMergedResult take_retained(OrchestrationResult& result) {
    CHECK(result);
    CHECK(!result.retryable.has_value());
    CHECK(result.retained.has_value());
    CHECK(result.retained->valid());
    CHECK(result.diagnostic.phase == OrchestrationPhase::complete);
    CHECK(result.diagnostic.status == OrchestrationStatus::retained);
    CHECK(result.diagnostic.disposition == OrchestrationDisposition::retained);
    CHECK(!result.diagnostic.retry_stage.has_value());
    CHECK(result.diagnostic.transitions_completed <= result.diagnostic.transition_budget);
    CHECK(!result.diagnostic.native_error);
    RetainedMergedResult retained(std::move(*result.retained));
    result.retained.reset();
    CHECK(retained.valid());
    return retained;
}

[[nodiscard]] OrchestrationContinuation take_retryable(OrchestrationResult& result) {
    CHECK(result);
    CHECK(result.retryable.has_value());
    CHECK(!result.retained.has_value());
    CHECK(result.retryable->valid());
    CHECK(result.diagnostic.phase == OrchestrationPhase::complete);
    CHECK(result.diagnostic.status == OrchestrationStatus::retryable);
    CHECK(result.diagnostic.disposition == OrchestrationDisposition::retryable);
    CHECK(result.diagnostic.retry_stage == result.retryable->stage());
    CHECK(result.diagnostic.transitions_completed <= result.diagnostic.transition_budget);
    CHECK(!result.diagnostic.native_error);
    OrchestrationContinuation retryable(std::move(*result.retryable));
    result.retryable.reset();
    CHECK(retryable.valid());
    return retryable;
}

void require_cold_reopen(OrchestrationResult& result, OrchestrationPhase expected_phase) {
    CHECK(!result);
    CHECK(!result.retryable.has_value());
    CHECK(!result.retained.has_value());
    CHECK(result.diagnostic.phase == expected_phase);
    CHECK(result.diagnostic.status == OrchestrationStatus::child_cold_reopen_required);
    CHECK(result.diagnostic.disposition == OrchestrationDisposition::cold_reopen_required);
    CHECK(!result.diagnostic.retry_stage.has_value());
    CHECK(result.diagnostic.transitions_completed <= result.diagnostic.transition_budget);
}

void require_retained_relations(const RetainedMergedResult& retained,
                                const CleanupLoopFixture& root,
                                std::size_t expected_completed_workers) {
    CHECK(retained.valid());
    CHECK(retained.completed_worker_count() == expected_completed_workers);
    const auto& view = retained.merged_relations();
    const auto expected = root.prepared().expected_rows();
    CHECK(view.count() == expected.size());
    for (std::size_t ordinal = 0; ordinal < expected.size(); ++ordinal) {
        CHECK(fixture::relations_equal(view.read(ordinal), expected[ordinal]));
    }
}

void require_worker_private_namespaces_absent(const CleanupLoopFixture& root) {
    for (const auto ordinal : root.nonempty_ordinals()) {
        const auto paths = relation::OOCCleanupTransaction::paths_for(root.worker_base(ordinal));
        CHECK(std::filesystem::exists(paths.lock_path));
        CHECK(!std::filesystem::exists(paths.private_directory));
        CHECK(!std::filesystem::exists(paths.index_path));
        CHECK(!std::filesystem::exists(paths.data_path));
        CHECK(!std::filesystem::exists(paths.private_handoff_path));
        CHECK(!std::filesystem::exists(paths.intent_path));
        CHECK(!std::filesystem::exists(paths.intent_pending_path));
    }
}

void require_no_m5_records(const CleanupLoopFixture& root) {
    const auto nonempty_ordinals = root.nonempty_ordinals();
    std::size_t cleanup_record_count = 0;
    for (const auto& name : capture_root_entry_names(root.prepared().root())) {
        CHECK(name.find("WaveCompleted") == std::string::npos);
        CHECK(name.find("wave_completed") == std::string::npos);
        CHECK(name.find("wave-completed") == std::string::npos);
        CHECK(name.find("Consumption") == std::string::npos);
        CHECK(name.find("consumption") == std::string::npos);
        CHECK(name.find("consumption-started") == std::string::npos);
        CHECK(name.find("consumption-ack") == std::string::npos);
        CHECK(name.find("successor-prepared") == std::string::npos);
        CHECK(name.find("artifact-cleanup-authorized") == std::string::npos);
        CHECK(name.find("artifact-cleanup-completed") == std::string::npos);
        CHECK(name.find(".ACK") == std::string::npos);
        CHECK(name.find(".ack") == std::string::npos);
        const auto cleanup = wave::parse_distributed_sieve_cleanup_record_leaf_v1(name);
        if (!cleanup.has_value()) {
            continue;
        }
        CHECK(!cleanup->pending);
        CHECK(cleanup->manifest_order_ordinal.has_value());
        CHECK(cleanup->kind ==
                  wave::DistributedSieveCleanupRecordCoordinateKindV1::authorized_worker ||
              cleanup->kind ==
                  wave::DistributedSieveCleanupRecordCoordinateKindV1::completed_worker);
        CHECK(std::find(nonempty_ordinals.begin(), nonempty_ordinals.end(),
                        *cleanup->manifest_order_ordinal) != nonempty_ordinals.end());
        ++cleanup_record_count;
    }
    CHECK(cleanup_record_count == nonempty_ordinals.size() * 2U);
    const std::array merged_cleanup_leaves{
        wave::DISTRIBUTED_SIEVE_CLEANUP_AUTHORIZED_MERGED_RECORD_LEAF,
        wave::DISTRIBUTED_SIEVE_CLEANUP_COMPLETED_MERGED_RECORD_LEAF,
    };
    for (const auto leaf : merged_cleanup_leaves) {
        CHECK(!std::filesystem::exists(root.prepared().root() / leaf));
        CHECK(!std::filesystem::exists(
            root.prepared().root() /
            (std::string(leaf) + std::string(wave::DISTRIBUTED_SIEVE_ROOT_RECORD_PENDING_SUFFIX))));
    }
}

void test_orchestrator_routes_two_workers_across_empty_chunks() {
    CleanupLoopFixture root("worker-cleanup-orchestrator-two-worker",
                            fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
    const auto merged_snapshot = capture_merged_corpus(root.merged_base());

    auto result = cleanup_authority::drive_distributed_sieve_worker_cleanup_to_retained_merged_v1(
        root.take_admission());
    auto retained = take_retained(result);

    require_retained_relations(retained, root, 2U);
    require_worker_private_namespaces_absent(root);
    require_merged_corpus(root.merged_base(), merged_snapshot);
    require_all_base_locks_free(root);
    require_wave_lock_busy(root);
    require_no_m5_records(root);
}

void test_orchestrator_routes_three_workers_and_keeps_exact_relation_order() {
    CleanupLoopFixture root(
        "worker-cleanup-orchestrator-three-worker",
        fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_nonempty_empty);
    const auto merged_snapshot = capture_merged_corpus(root.merged_base());

    StopContext<AuthorizationPublicationFault> context{
        .target = AuthorizationPublicationFault::ColdBeforeAuthoritySpend,
    };
    auto interrupted = cleanup_authority::trusted_test::
        drive_distributed_sieve_worker_cleanup_to_retained_merged_v1_with_hooks(
            root.take_admission(), OrchestrationTestHooks{
                                       .authorization_publication =
                                           AuthorizationPublicationHooks{
                                               .stop_after = stop_authorization_publication_after,
                                               .context = &context,
                                           },
                                   });
    CHECK(context.invoked);
    auto retryable = take_retryable(interrupted);
    CHECK(retryable.stage() == OrchestrationStage::authorization_recovery);
    CHECK(interrupted.diagnostic.authorization_publication.has_value());
    require_merged_corpus(root.merged_base(), merged_snapshot);
    require_all_base_locks_free(root);
    require_wave_lock_busy(root);

    auto result = cleanup_authority::resume_distributed_sieve_worker_cleanup_to_retained_merged_v1(
        std::move(retryable));
    auto retained = take_retained(result);

    require_retained_relations(retained, root, 3U);
    require_worker_private_namespaces_absent(root);
    require_merged_corpus(root.merged_base(), merged_snapshot);
    require_all_base_locks_free(root);
    require_wave_lock_busy(root);
    require_no_m5_records(root);
}

void test_orchestrator_cold_terminal_is_zero_mutation() {
    CleanupLoopFixture root("worker-cleanup-orchestrator-cold-terminal",
                            fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
    const auto merged_snapshot = capture_merged_corpus(root.merged_base());
    {
        auto completed =
            cleanup_authority::drive_distributed_sieve_worker_cleanup_to_retained_merged_v1(
                root.take_admission());
        auto retained = take_retained(completed);
        require_retained_relations(retained, root, 2U);
    }

    root.cold_reopen("cold-open terminal worker-cleanup root");
    const auto inventory_before = capture_root_inventory(root.prepared().root());
    auto reopened = cleanup_authority::drive_distributed_sieve_worker_cleanup_to_retained_merged_v1(
        root.take_admission());
    auto retained = take_retained(reopened);

    require_retained_relations(retained, root, 2U);
    CHECK(capture_root_inventory(root.prepared().root()) == inventory_before);
    require_merged_corpus(root.merged_base(), merged_snapshot);
    require_all_base_locks_free(root);
    require_wave_lock_busy(root);
    require_no_m5_records(root);
}

void test_orchestrator_propagates_r3_retry_and_post_spend_cold() {
    {
        CleanupLoopFixture root("worker-cleanup-orchestrator-r3-advance-retry",
                                fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
        const auto merged_snapshot = capture_merged_corpus(root.merged_base());
        StopContext<AuthorizationPublicationFault> context{
            .target = AuthorizationPublicationFault::FreshBeforeAuthoritySpend,
        };

        auto interrupted = cleanup_authority::trusted_test::
            drive_distributed_sieve_worker_cleanup_to_retained_merged_v1_with_hooks(
                root.take_admission(),
                OrchestrationTestHooks{
                    .authorization_publication =
                        AuthorizationPublicationHooks{
                            .stop_after = stop_authorization_publication_after,
                            .context = &context,
                        },
                });

        CHECK(context.invoked);
        auto retryable = take_retryable(interrupted);
        CHECK(retryable.stage() == OrchestrationStage::authorization_advance_retry);
        CHECK(interrupted.diagnostic.authorization_publication.has_value());
        CHECK(interrupted.diagnostic.authorization_publication->last_fault_point ==
              AuthorizationPublicationFault::FreshBeforeAuthoritySpend);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);
        require_wave_lock_busy(root);

        auto resumed =
            cleanup_authority::resume_distributed_sieve_worker_cleanup_to_retained_merged_v1(
                std::move(retryable));
        auto retained = take_retained(resumed);
        require_retained_relations(retained, root, 2U);
        require_worker_private_namespaces_absent(root);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);
        require_wave_lock_busy(root);
        require_no_m5_records(root);
    }

    {
        CleanupLoopFixture root("worker-cleanup-orchestrator-r3-post-spend-cold",
                                fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
        const auto merged_snapshot = capture_merged_corpus(root.merged_base());
        StopContext<AuthorizationPublicationFault> context{
            .target = AuthorizationPublicationFault::FreshAfterAuthoritySpend,
        };

        auto interrupted = cleanup_authority::trusted_test::
            drive_distributed_sieve_worker_cleanup_to_retained_merged_v1_with_hooks(
                root.take_admission(),
                OrchestrationTestHooks{
                    .authorization_publication =
                        AuthorizationPublicationHooks{
                            .stop_after = stop_authorization_publication_after,
                            .context = &context,
                        },
                });

        CHECK(context.invoked);
        require_cold_reopen(interrupted, OrchestrationPhase::authorization_publication);
        CHECK(interrupted.diagnostic.authorization_publication.has_value());
        CHECK(interrupted.diagnostic.authorization_publication->last_fault_point ==
              AuthorizationPublicationFault::FreshAfterAuthoritySpend);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);

        root.cold_reopen("cold-reopen R3 post-spend orchestration root");
        auto replayed =
            cleanup_authority::drive_distributed_sieve_worker_cleanup_to_retained_merged_v1(
                root.take_admission());
        auto retained = take_retained(replayed);
        require_retained_relations(retained, root, 2U);
        require_worker_private_namespaces_absent(root);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);
        require_wave_lock_busy(root);
        require_no_m5_records(root);
    }
}

void test_orchestrator_propagates_r1_intent_retry_and_rejects_cross_process_resume() {
    CleanupLoopFixture root("worker-cleanup-orchestrator-r1-intent-retry",
                            fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
    const auto merged_snapshot = capture_merged_corpus(root.merged_base());
    StopContext<IntentPublicationFault> context{
        .target = IntentPublicationFault::ReaderViewsClosed,
    };

    auto interrupted = cleanup_authority::trusted_test::
        drive_distributed_sieve_worker_cleanup_to_retained_merged_v1_with_hooks(
            root.take_admission(), OrchestrationTestHooks{
                                       .intent_publication =
                                           IntentPublicationHooks{
                                               .stop_after = stop_intent_publication_after,
                                               .context = &context,
                                           },
                                   });

    CHECK(context.invoked);
    auto retryable = take_retryable(interrupted);
    CHECK(retryable.stage() == OrchestrationStage::intent_conversion_retry);
    CHECK(interrupted.diagnostic.completion_preparation.has_value());
    CHECK(interrupted.diagnostic.completion_preparation->intent_execute.status ==
          cleanup_authority::DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::
              capabilities_retained);
    require_merged_corpus(root.merged_base(), merged_snapshot);
    require_wave_lock_busy(root);

    const pid_t child = ::fork();
    if (child < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "fork R1 intent orchestration retry test");
    }
    if (child == 0) {
        auto rejected =
            cleanup_authority::resume_distributed_sieve_worker_cleanup_to_retained_merged_v1(
                std::move(retryable));
        const bool correct =
            !rejected && !rejected.retryable.has_value() && !rejected.retained.has_value() &&
            rejected.diagnostic.phase == OrchestrationPhase::input_validation &&
            rejected.diagnostic.status == OrchestrationStatus::invalid_input &&
            rejected.diagnostic.disposition == OrchestrationDisposition::cold_reopen_required;
        ::_exit(correct ? ORCHESTRATOR_FORK_REJECTED_EXIT : EXIT_FAILURE);
    }

    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "wait for R1 intent orchestration retry child");
    }
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == ORCHESTRATOR_FORK_REJECTED_EXIT);
    CHECK(retryable.valid());

    auto resumed = cleanup_authority::resume_distributed_sieve_worker_cleanup_to_retained_merged_v1(
        std::move(retryable));
    auto retained = take_retained(resumed);
    require_retained_relations(retained, root, 2U);
    require_worker_private_namespaces_absent(root);
    require_merged_corpus(root.merged_base(), merged_snapshot);
    require_all_base_locks_free(root);
    require_wave_lock_busy(root);
    require_no_m5_records(root);
}

void test_orchestrator_propagates_r2_retry_and_post_spend_cold() {
    {
        CleanupLoopFixture root("worker-cleanup-orchestrator-r2-fresh-retry",
                                fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
        const auto merged_snapshot = capture_merged_corpus(root.merged_base());
        StopContext<CompletionPublicationFault> context{
            .target = CompletionPublicationFault::FreshBeforeReceiptSpend,
        };

        auto interrupted = cleanup_authority::trusted_test::
            drive_distributed_sieve_worker_cleanup_to_retained_merged_v1_with_hooks(
                root.take_admission(), OrchestrationTestHooks{
                                           .completion_publication =
                                               CompletionPublicationHooks{
                                                   .stop_after = stop_completion_publication_after,
                                                   .context = &context,
                                               },
                                       });

        CHECK(context.invoked);
        auto retryable = take_retryable(interrupted);
        CHECK(retryable.stage() == OrchestrationStage::completion_publication_fresh_retry);
        CHECK(interrupted.diagnostic.completion_publication.has_value());
        CHECK(interrupted.diagnostic.completion_publication->last_fault_point ==
              CompletionPublicationFault::FreshBeforeReceiptSpend);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);
        require_wave_lock_busy(root);

        auto resumed =
            cleanup_authority::resume_distributed_sieve_worker_cleanup_to_retained_merged_v1(
                std::move(retryable));
        auto retained = take_retained(resumed);
        require_retained_relations(retained, root, 2U);
        require_worker_private_namespaces_absent(root);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);
        require_wave_lock_busy(root);
        require_no_m5_records(root);
    }

    {
        CleanupLoopFixture root("worker-cleanup-orchestrator-r2-post-spend-cold",
                                fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
        const auto merged_snapshot = capture_merged_corpus(root.merged_base());
        StopContext<CompletionPublicationFault> context{
            .target = CompletionPublicationFault::FreshAfterReceiptSpend,
        };

        auto interrupted = cleanup_authority::trusted_test::
            drive_distributed_sieve_worker_cleanup_to_retained_merged_v1_with_hooks(
                root.take_admission(), OrchestrationTestHooks{
                                           .completion_publication =
                                               CompletionPublicationHooks{
                                                   .stop_after = stop_completion_publication_after,
                                                   .context = &context,
                                               },
                                       });

        CHECK(context.invoked);
        require_cold_reopen(interrupted, OrchestrationPhase::completion_publication);
        CHECK(interrupted.diagnostic.completion_publication.has_value());
        CHECK(interrupted.diagnostic.completion_publication->last_fault_point ==
              CompletionPublicationFault::FreshAfterReceiptSpend);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);

        root.cold_reopen("cold-reopen R2 post-spend orchestration root");
        auto replayed =
            cleanup_authority::drive_distributed_sieve_worker_cleanup_to_retained_merged_v1(
                root.take_admission());
        auto retained = take_retained(replayed);
        require_retained_relations(retained, root, 2U);
        require_worker_private_namespaces_absent(root);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);
        require_wave_lock_busy(root);
        require_no_m5_records(root);
    }

    {
        CleanupLoopFixture root("worker-cleanup-orchestrator-r2-recovery-retry",
                                fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
        const auto merged_snapshot = capture_merged_corpus(root.merged_base());
        StopContext<CompletionPublicationFault> pending_context{
            .target = CompletionPublicationFault::PendingDurable,
        };

        auto pending = cleanup_authority::trusted_test::
            drive_distributed_sieve_worker_cleanup_to_retained_merged_v1_with_hooks(
                root.take_admission(), OrchestrationTestHooks{
                                           .completion_publication =
                                               CompletionPublicationHooks{
                                                   .stop_after = stop_completion_publication_after,
                                                   .context = &pending_context,
                                               },
                                       });

        CHECK(pending_context.invoked);
        require_cold_reopen(pending, OrchestrationPhase::completion_publication);
        CHECK(pending.diagnostic.completion_publication.has_value());
        CHECK(pending.diagnostic.completion_publication->last_fault_point ==
              CompletionPublicationFault::PendingDurable);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);

        root.cold_reopen("cold-reopen R2 pending completion for recovery retry");
        StopContext<CompletionPublicationFault> recovery_context{
            .target = CompletionPublicationFault::RecoveryBeforePublication,
        };
        auto recovery = cleanup_authority::trusted_test::
            drive_distributed_sieve_worker_cleanup_to_retained_merged_v1_with_hooks(
                root.take_admission(), OrchestrationTestHooks{
                                           .completion_publication =
                                               CompletionPublicationHooks{
                                                   .stop_after = stop_completion_publication_after,
                                                   .context = &recovery_context,
                                               },
                                       });

        CHECK(recovery_context.invoked);
        auto retryable = take_retryable(recovery);
        CHECK(retryable.stage() == OrchestrationStage::authorization_recovery);
        CHECK(recovery.diagnostic.completion_publication.has_value());
        CHECK(recovery.diagnostic.completion_publication->last_fault_point ==
              CompletionPublicationFault::RecoveryBeforePublication);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);
        require_wave_lock_busy(root);

        auto resumed =
            cleanup_authority::resume_distributed_sieve_worker_cleanup_to_retained_merged_v1(
                std::move(retryable));
        auto retained = take_retained(resumed);
        require_retained_relations(retained, root, 2U);
        require_worker_private_namespaces_absent(root);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_all_base_locks_free(root);
        require_wave_lock_busy(root);
        require_no_m5_records(root);
    }
}

void test_orchestrator_result_is_move_only_and_preserves_merged_bytes() {
    CleanupLoopFixture root("worker-cleanup-orchestrator-result-protection",
                            fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
    const auto merged_snapshot = capture_merged_corpus(root.merged_base());

    {
        auto result =
            cleanup_authority::drive_distributed_sieve_worker_cleanup_to_retained_merged_v1(
                root.take_admission());
        auto retained = take_retained(result);
        const auto* view_address = std::addressof(retained.merged_relations());
        const relation::ReadOnlyRelationCorpusView copied_view = retained.merged_relations();
        RetainedMergedResult moved(std::move(retained));
        CHECK(!retained.valid());
        CHECK(moved.valid());
        CHECK(std::addressof(moved.merged_relations()) == view_address);
        CHECK(copied_view.count() == root.prepared().expected_rows().size());
        for (std::size_t ordinal = 0; ordinal < copied_view.count(); ++ordinal) {
            CHECK(fixture::relations_equal(copied_view.read(ordinal),
                                           root.prepared().expected_rows()[ordinal]));
        }
        require_retained_relations(moved, root, 2U);
        require_merged_corpus(root.merged_base(), merged_snapshot);
        require_wave_lock_busy(root);
    }

    require_merged_corpus(root.merged_base(), merged_snapshot);
    auto reopened = wave::open_worker_cleanup_root_v1(root.prepared().root(),
                                                      root.prepared().manifest_digest());
    CHECK(reopened);
    CHECK(reopened.admission.has_value());
    CHECK(reopened.admission->valid());
    CHECK(reopened.admission->reader().count() == root.prepared().expected_rows().size());
}

void test_orchestrator_is_process_bound_and_preserves_authority_boundaries() {
    CleanupLoopFixture root("worker-cleanup-orchestrator-process-bound",
                            fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
    const auto merged_snapshot = capture_merged_corpus(root.merged_base());
    auto result = cleanup_authority::drive_distributed_sieve_worker_cleanup_to_retained_merged_v1(
        root.take_admission());
    auto retained = take_retained(result);

    require_retained_relations(retained, root, 2U);
    require_merged_corpus(root.merged_base(), merged_snapshot);
    require_worker_private_namespaces_absent(root);
    require_all_base_locks_free(root);
    require_wave_lock_busy(root);
    require_no_m5_records(root);

    const pid_t child = ::fork();
    if (child < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "fork worker-cleanup orchestrator process-bound test");
    }
    if (child == 0) {
        ::_exit(!retained.valid() ? ORCHESTRATOR_FORK_REJECTED_EXIT : EXIT_FAILURE);
    }

    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "wait for worker-cleanup orchestrator process-bound child");
    }
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == ORCHESTRATOR_FORK_REJECTED_EXIT);
    CHECK(retained.valid());
    require_retained_relations(retained, root, 2U);
    require_merged_corpus(root.merged_base(), merged_snapshot);
    require_all_base_locks_free(root);
    require_wave_lock_busy(root);
    require_no_m5_records(root);
}

#endif

void run_core_suite() {
#if defined(__APPLE__)
    test_orchestrator_routes_two_workers_across_empty_chunks();
    std::cout << "  two-worker route crosses empty chunks: PASS\n";
    test_orchestrator_routes_three_workers_and_keeps_exact_relation_order();
    std::cout << "  three-worker route preserves exact relation order: PASS\n";
    test_orchestrator_cold_terminal_is_zero_mutation();
    std::cout << "  cold terminal route is zero mutation: PASS\n";
#else
    throw TestFailure("core suite requires the macOS private-handoff runtime");
#endif
}

void run_fault_propagation_suite() {
#if defined(__APPLE__)
    test_orchestrator_propagates_r3_retry_and_post_spend_cold();
    std::cout << "  R3 retry and post-spend cold propagation: PASS\n";
    test_orchestrator_propagates_r1_intent_retry_and_rejects_cross_process_resume();
    std::cout << "  R1 intent retry and cross-process cold rejection: PASS\n";
    test_orchestrator_propagates_r2_retry_and_post_spend_cold();
    std::cout << "  R2 retry and post-spend cold propagation: PASS\n";
#else
    throw TestFailure("fault-propagation suite requires the macOS private-handoff runtime");
#endif
}

void run_protection_suite() {
#if defined(__APPLE__)
    test_orchestrator_result_is_move_only_and_preserves_merged_bytes();
    std::cout << "  move-only retained view preserves merged bytes: PASS\n";
    test_orchestrator_is_process_bound_and_preserves_authority_boundaries();
    std::cout << "  process, WaveLock, BaseLock, and M5 boundaries: PASS\n";
#else
    throw TestFailure("protection suite requires the macOS private-handoff runtime");
#endif
}

void run_platform_suite() {
#if defined(__APPLE__)
    throw TestFailure("platform suite is reserved for unsupported hosts");
#else
    static_assert(!std::is_constructible_v<CleanupAdmission, std::filesystem::path>);
    static_assert(!std::is_constructible_v<OrchestrationContinuation, CleanupAdmission&&>);
    static_assert(!std::is_constructible_v<RetainedMergedResult, CleanupAdmission&&>);
    std::cout << "  orchestrator remains unreachable without sealed cleanup authority: PASS\n";
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
            run_fault_propagation_suite();
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
            if (suite == "fault-propagation") {
                run_fault_propagation_suite();
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
                  << " [--suite core|fault-propagation|protection|platform]\n";
        return EXIT_FAILURE;
    } catch (const std::exception& failure) {
        std::cerr << "Distributed sieve worker-cleanup orchestrator tests FAILED: "
                  << failure.what() << '\n';
        return EXIT_FAILURE;
    }
}
