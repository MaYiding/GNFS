#include "distributed_sieve_merge_commit_authority_internal.hpp"
#include "distributed_sieve_wave_result_internal.hpp"
#include "distributed_sieve_worker_cleanup_authority_internal.hpp"
#include "distributed_sieve_worker_cleanup_orchestrator_internal.hpp"

#include "support/child_process.hpp"

#if defined(__APPLE__)
#include "support/distributed_sieve_wave_merge_commit_fixture.hpp"
#endif

#include <gnfs/relation/relation_corpus.hpp>
#include <gnfs/sieve/distributed_sieve.hpp>
#include <gnfs/sieve/distributed_sieve_protocol.hpp>
#include <gnfs/util/sha256.hpp>

#include <algorithm>
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
#include <variant>
#include <vector>

#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace cleanup = gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail;
namespace commit = gnfs::sieve::distributed_sieve_merge_commit_authority_detail;
namespace relation = gnfs::relation;
namespace result_detail = gnfs::sieve::distributed_sieve_result_detail;
namespace sieve = gnfs::sieve;
namespace wave = gnfs::sieve::distributed_sieve_resume_detail;

using WaveResultCleanupAdmission = cleanup::DistributedSieveWorkerCleanupRootAdmissionV1;
using RetainedMergedResult = result_detail::RetainedMergedResultV1;
using PromotionResult = result_detail::DistributedSieveWaveResultPromotionResultV1;
using PromotionPhase = result_detail::DistributedSieveWaveResultPromotionPhaseV1;
using PromotionStatus = result_detail::DistributedSieveWaveResultPromotionStatusV1;
using PromotionDisposition = result_detail::DistributedSieveWaveResultPromotionDispositionV1;
using PromotionFaultPoint =
    result_detail::trusted_test::DistributedSieveWaveResultPromotionFaultPointV1;
using PromotionHooks = result_detail::trusted_test::DistributedSieveWaveResultPromotionTestHooksV1;
using WaveResult = sieve::DistributedSieveWaveResult;

template <typename Value>
concept HasMergedRelationsOnConstLvalue = requires(const Value& value) {
    { value.merged_relations() } -> std::same_as<const relation::ReadOnlyRelationCorpusView&>;
};

template <typename Value>
concept HasMergedRelationsOnRvalue =
    requires(Value&& value) { std::move(value).merged_relations(); };

template <typename Value>
concept HasChunksOnConstLvalue = requires(const Value& value) {
    { value.chunks() } -> std::same_as<std::span<const sieve::ChunkCommitSummaryV1>>;
};

template <typename Value>
concept HasChunksOnRvalue = requires(Value&& value) { std::move(value).chunks(); };

template <typename Value>
concept HasManifestDigestOnRvalue = requires(Value&& value) { std::move(value).manifest_digest(); };

template <typename Value>
concept HasMergeCommitDigestOnRvalue =
    requires(Value&& value) { std::move(value).merge_commit_digest(); };

template <typename Value>
concept HasRootAccessor = requires(Value& value) { value.root(); };

template <typename Value>
concept HasPathAccessor = requires(Value& value) { value.path(); };

template <typename Value>
concept HasDescriptorAccessor = requires(Value& value) { value.descriptor(); };

template <typename Value>
concept HasReceiptAccessor = requires(Value& value) { value.receipt(); };

template <typename Value>
concept HasCleanup = requires(Value& value) { value.cleanup(); };

template <typename Value>
concept HasCleanupArm = requires(Value& value) { value.arm_ooc_cleanup(); };

template <typename Value>
concept HasAcknowledge = requires(Value& value) { value.acknowledge(); };

template <typename Value>
concept HasBeginConsumption = requires(Value& value) { value.begin_consumption(); };

template <typename Value>
concept HasWorkerLaunch = requires(Value& value) { value.launch_worker(); };

static_assert(std::is_final_v<WaveResult>);
static_assert(!std::is_default_constructible_v<WaveResult>);
static_assert(!std::is_copy_constructible_v<WaveResult>);
static_assert(!std::is_copy_assignable_v<WaveResult>);
static_assert(std::is_nothrow_move_constructible_v<WaveResult>);
static_assert(!std::is_move_assignable_v<WaveResult>);
static_assert(std::is_nothrow_destructible_v<WaveResult>);
static_assert(!std::is_constructible_v<WaveResult, RetainedMergedResult&&>);
static_assert(!std::is_constructible_v<WaveResult, relation::ReadOnlyRelationCorpusView>);
static_assert(!std::is_copy_constructible_v<PromotionResult>);
static_assert(std::is_nothrow_move_constructible_v<PromotionResult>);
static_assert(!std::is_move_assignable_v<PromotionResult>);
static_assert(HasMergedRelationsOnConstLvalue<WaveResult>);
static_assert(!HasMergedRelationsOnRvalue<WaveResult>);
static_assert(HasChunksOnConstLvalue<WaveResult>);
static_assert(!HasChunksOnRvalue<WaveResult>);
static_assert(!HasManifestDigestOnRvalue<WaveResult>);
static_assert(!HasMergeCommitDigestOnRvalue<WaveResult>);
static_assert(!HasRootAccessor<WaveResult>);
static_assert(!HasPathAccessor<WaveResult>);
static_assert(!HasDescriptorAccessor<WaveResult>);
static_assert(!HasReceiptAccessor<WaveResult>);
static_assert(!HasCleanup<WaveResult>);
static_assert(!HasCleanupArm<WaveResult>);
static_assert(!HasAcknowledge<WaveResult>);
static_assert(!HasBeginConsumption<WaveResult>);
static_assert(!HasWorkerLaunch<WaveResult>);

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

using WaveResultBooleanMember = bool (WaveResult::*)() const noexcept;
using WaveResultCountMember = std::size_t (WaveResult::*)() const noexcept;
using WaveResultDigestMember = const gnfs::util::Sha256Digest& (WaveResult::*)() const&;
using WaveResultChunksMember =
    std::span<const sieve::ChunkCommitSummaryV1> (WaveResult::*)() const&;
using WaveResultRelationsMember =
    const relation::ReadOnlyRelationCorpusView& (WaveResult::*)() const&;
using WaveResultMoveConstructFunction = void (*)(void*, WaveResult&&) noexcept;
using WaveResultDestroyFunction = void (*)(WaveResult*) noexcept;

void move_construct_wave_result_link_probe(void* storage, WaveResult&& value) noexcept {
    std::construct_at(static_cast<WaveResult*>(storage), std::move(value));
}

void destroy_wave_result_link_probe(WaveResult* value) noexcept {
    if (value != nullptr) {
        std::destroy_at(value);
    }
}

[[maybe_unused]] WaveResultBooleanMember volatile wave_result_valid_link_probe = &WaveResult::valid;
[[maybe_unused]] WaveResultBooleanMember volatile wave_result_bool_link_probe =
    &WaveResult::operator bool;
[[maybe_unused]] WaveResultCountMember volatile wave_result_relation_count_link_probe =
    &WaveResult::relation_count;
[[maybe_unused]] WaveResultCountMember volatile wave_result_worker_count_link_probe =
    &WaveResult::completed_worker_count;
[[maybe_unused]] WaveResultDigestMember volatile wave_result_manifest_link_probe =
    static_cast<WaveResultDigestMember>(&WaveResult::manifest_digest);
[[maybe_unused]] WaveResultDigestMember volatile wave_result_merge_commit_link_probe =
    static_cast<WaveResultDigestMember>(&WaveResult::merge_commit_digest);
[[maybe_unused]] WaveResultChunksMember volatile wave_result_chunks_link_probe =
    static_cast<WaveResultChunksMember>(&WaveResult::chunks);
[[maybe_unused]] WaveResultRelationsMember volatile wave_result_relations_link_probe =
    static_cast<WaveResultRelationsMember>(&WaveResult::merged_relations);
[[maybe_unused]] WaveResultMoveConstructFunction volatile wave_result_move_construct_link_probe =
    &move_construct_wave_result_link_probe;
[[maybe_unused]] WaveResultDestroyFunction volatile wave_result_destroy_link_probe =
    &destroy_wave_result_link_probe;

#if defined(__APPLE__)

namespace fixture = gnfs::test::distributed_sieve_wave_merge_commit_fixture;

using CommittedTail = cleanup::DistributedSieveCommittedTailAdmissionV1;
using Digest = gnfs::util::Sha256Digest;

inline constexpr int WAVE_LOCK_BUSY_EXIT = 91;
inline constexpr int PUBLIC_RESULT_FORK_REJECTED_EXIT = 92;

std::string test_executable;

struct AllocationFaultState final {
    std::size_t calls = 0;
};

[[nodiscard]] bool stop_before_state_allocation(PromotionFaultPoint point, void* context) noexcept {
    auto& state = *static_cast<AllocationFaultState*>(context);
    ++state.calls;
    return point == PromotionFaultPoint::before_state_allocation;
}

[[nodiscard]] std::string digest_hex(const Digest& digest) {
    const auto encoded = gnfs::util::encode_sha256_hex(digest);
    return std::string(encoded.data(), encoded.size());
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

[[nodiscard]] CommittedTail commit_fresh(fixture::PreparedWaveFixture& prepared) {
    auto publication = prepared.prepare_fresh();
    CHECK(publication.admission.has_value());
    auto committed =
        commit::consume_distributed_sieve_merge_prepared_v1(std::move(*publication.admission));
    if (!committed || !committed.committed_tail.has_value()) {
        fail("publish WaveMergeCommit before public result fixture", __LINE__);
    }
    CHECK(!committed.retryable_prepared.has_value());
    return std::move(*committed.committed_tail);
}

class WaveResultFixture final {
public:
    WaveResultFixture(std::string_view label, fixture::PreparedWaveChunkLayoutV1 layout)
        : prepared_(label, layout) {
        auto tail = commit_fresh(prepared_);
        auto transitioned = cleanup::consume_distributed_sieve_committed_tail_for_worker_cleanup_v1(
            std::move(tail));
        if (!transitioned || !transitioned.admission.has_value()) {
            fail("consume committed tail for public result fixture", __LINE__);
        }
        CHECK(!transitioned.retryable_tail.has_value());
        admission_.emplace(std::move(*transitioned.admission));
    }

    WaveResultFixture(const WaveResultFixture&) = delete;
    WaveResultFixture& operator=(const WaveResultFixture&) = delete;

    [[nodiscard]] fixture::PreparedWaveFixture& prepared() noexcept {
        return prepared_;
    }

    [[nodiscard]] const fixture::PreparedWaveFixture& prepared() const noexcept {
        return prepared_;
    }

    [[nodiscard]] RetainedMergedResult drive_to_retained() {
        CHECK(admission_.has_value());
        auto driven = cleanup::drive_distributed_sieve_worker_cleanup_to_retained_merged_v1(
            std::move(*admission_));
        admission_.reset();
        CHECK(driven);
        CHECK(!driven.retryable.has_value());
        CHECK(driven.retained.has_value());
        RetainedMergedResult retained(std::move(*driven.retained));
        driven.retained.reset();
        CHECK(retained.valid());
        return retained;
    }

private:
    fixture::PreparedWaveFixture prepared_;
    std::optional<WaveResultCleanupAdmission> admission_;
};

struct RootEntrySnapshot final {
    std::string name;
    std::filesystem::file_type type = std::filesystem::file_type::none;
    std::vector<std::byte> bytes;

    [[nodiscard]] friend bool operator==(const RootEntrySnapshot&,
                                         const RootEntrySnapshot&) = default;
};

[[nodiscard]] std::vector<RootEntrySnapshot>
capture_root_inventory(const std::filesystem::path& root) {
    std::vector<RootEntrySnapshot> entries;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        std::error_code error;
        const auto status = entry.symlink_status(error);
        if (error) {
            throw std::filesystem::filesystem_error("inspect wave-result root", entry.path(),
                                                    error);
        }
        RootEntrySnapshot snapshot{
            .name = entry.path().filename().string(),
            .type = status.type(),
        };
        if (snapshot.type == std::filesystem::file_type::regular) {
            snapshot.bytes = fixture::read_file_bytes(entry.path());
        }
        entries.push_back(std::move(snapshot));
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& left, const auto& right) { return left.name < right.name; });
    return entries;
}

[[nodiscard]] sieve::WaveMergeCommitV1 load_merge_commit(const std::filesystem::path& root) {
    const auto bytes =
        fixture::read_file_bytes(root / wave::DISTRIBUTED_SIEVE_WAVE_MERGE_COMMIT_RECORD_LEAF);
    auto decoded = sieve::decode_distributed_sieve_record(bytes);
    CHECK(decoded);
    CHECK(decoded.value.has_value());
    const auto* committed = std::get_if<sieve::WaveMergeCommitV1>(&*decoded.value);
    CHECK(committed != nullptr);
    return *committed;
}

[[nodiscard]] WaveResult take_promoted(PromotionResult& promotion) {
    CHECK(promotion);
    CHECK(!promotion.retryable.has_value());
    CHECK(promotion.promoted.has_value());
    CHECK(promotion.promoted->valid());
    CHECK(promotion.diagnostic.phase == PromotionPhase::complete);
    CHECK(promotion.diagnostic.status == PromotionStatus::promoted);
    CHECK(promotion.diagnostic.disposition == PromotionDisposition::promoted);
    CHECK(!promotion.diagnostic.native_error);
    WaveResult result(std::move(*promotion.promoted));
    promotion.promoted.reset();
    CHECK(result.valid());
    return result;
}

void require_exact_relations(const WaveResult& result,
                             const fixture::PreparedWaveFixture& prepared) {
    const auto expected = prepared.expected_rows();
    CHECK(result.relation_count() == expected.size());
    CHECK(result.merged_relations().count() == expected.size());
    for (std::size_t ordinal = 0; ordinal < expected.size(); ++ordinal) {
        CHECK(fixture::relations_equal(result.merged_relations().read(ordinal), expected[ordinal]));
    }
}

void require_public_result_wave_lock_busy(const WaveResultFixture& fixture_root) {
    const auto probe = gnfs::test::run_child_process(
        test_executable, {"--probe-wave-lock", fixture_root.prepared().root().string(),
                          digest_hex(fixture_root.prepared().manifest_digest())});
    CHECK(probe.exited);
    CHECK(!probe.signaled);
    CHECK(probe.exit_code == WAVE_LOCK_BUSY_EXIT);
}

void require_no_m5_mutation_records(const std::filesystem::path& root) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const std::string name = entry.path().filename().string();
        CHECK(name.find("consumption") == std::string::npos);
        CHECK(name.find("successor") == std::string::npos);
        CHECK(name.find("wave-completed") == std::string::npos);
        CHECK(name.find("wave_completed") == std::string::npos);
        CHECK(name != wave::DISTRIBUTED_SIEVE_CLEANUP_AUTHORIZED_MERGED_RECORD_LEAF);
        CHECK(name != wave::DISTRIBUTED_SIEVE_CLEANUP_COMPLETED_MERGED_RECORD_LEAF);
    }
}

void test_wave_result_promotes_exact_commit_projection() {
    WaveResultFixture fixture_root(
        "distributed-sieve-wave-result-projection",
        fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_nonempty_empty);
    auto retained = fixture_root.drive_to_retained();
    const auto committed = load_merge_commit(fixture_root.prepared().root());
    const auto inventory_before = capture_root_inventory(fixture_root.prepared().root());

    auto promotion = result_detail::promote_distributed_sieve_wave_result_v1(std::move(retained));
    auto result = take_promoted(promotion);

    CHECK(!retained.valid());
    CHECK(result.manifest_digest() == fixture_root.prepared().manifest_digest());
    CHECK(result.merge_commit_digest() == committed.self_digest);
    CHECK(result.chunks().size() == committed.chunks.size());
    CHECK(std::equal(result.chunks().begin(), result.chunks().end(), committed.chunks.begin()));
    CHECK(result.completed_worker_count() == 3U);
    require_exact_relations(result, fixture_root.prepared());
    CHECK(capture_root_inventory(fixture_root.prepared().root()) == inventory_before);
    require_no_m5_mutation_records(fixture_root.prepared().root());
}

void test_wave_result_rejects_spent_r4_authority() {
    WaveResultFixture fixture_root(
        "distributed-sieve-wave-result-spent",
        fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
    auto retained = fixture_root.drive_to_retained();
    auto promotion = result_detail::promote_distributed_sieve_wave_result_v1(std::move(retained));
    auto result = take_promoted(promotion);

    auto rejected = result_detail::promote_distributed_sieve_wave_result_v1(std::move(retained));
    CHECK(!rejected);
    CHECK(!rejected.retryable.has_value());
    CHECK(!rejected.promoted.has_value());
    CHECK(rejected.diagnostic.phase == PromotionPhase::input_validation);
    CHECK(rejected.diagnostic.status == PromotionStatus::invalid_input);
    CHECK(rejected.diagnostic.disposition == PromotionDisposition::cold_reopen_required);
    CHECK(rejected.diagnostic.native_error == std::make_error_code(std::errc::invalid_argument));
    CHECK(result.valid());
    require_exact_relations(result, fixture_root.prepared());
}

void test_wave_result_returns_retryable_owner_before_spend() {
    WaveResultFixture fixture_root(
        "distributed-sieve-wave-result-allocation-retry",
        fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
    auto retained = fixture_root.drive_to_retained();
    const auto inventory_before = capture_root_inventory(fixture_root.prepared().root());
    AllocationFaultState fault_state;
    const PromotionHooks hooks{
        .stop_before = stop_before_state_allocation,
        .context = &fault_state,
    };

    auto retryable =
        result_detail::trusted_test::promote_distributed_sieve_wave_result_v1_with_hooks(
            std::move(retained), hooks);
    CHECK(!retryable);
    CHECK(!retained.valid());
    CHECK(retryable.retryable.has_value());
    CHECK(retryable.retryable->valid());
    CHECK(!retryable.promoted.has_value());
    CHECK(retryable.diagnostic.phase == PromotionPhase::state_allocation);
    CHECK(retryable.diagnostic.status == PromotionStatus::resource_exhausted);
    CHECK(retryable.diagnostic.disposition == PromotionDisposition::retryable);
    CHECK(retryable.diagnostic.native_error == std::make_error_code(std::errc::not_enough_memory));
    CHECK(fault_state.calls == 1U);
    CHECK(capture_root_inventory(fixture_root.prepared().root()) == inventory_before);

    auto promotion =
        result_detail::promote_distributed_sieve_wave_result_v1(std::move(*retryable.retryable));
    retryable.retryable.reset();
    auto result = take_promoted(promotion);
    require_exact_relations(result, fixture_root.prepared());
    CHECK(capture_root_inventory(fixture_root.prepared().root()) == inventory_before);
}

void test_wave_result_move_preserves_borrowed_view_and_spans() {
    WaveResultFixture fixture_root(
        "distributed-sieve-wave-result-move",
        fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
    auto retained = fixture_root.drive_to_retained();
    auto promotion = result_detail::promote_distributed_sieve_wave_result_v1(std::move(retained));
    auto result = take_promoted(promotion);

    std::optional<WaveResult> moved_owner;
    const auto* view_address = std::addressof(result.merged_relations());
    const auto copied_view = result.merged_relations();
    const auto chunks = result.chunks();
    const auto* chunk_address = chunks.data();
    const auto& manifest_digest = result.manifest_digest();
    const auto* manifest_digest_address = std::addressof(manifest_digest);
    const auto& merge_commit_digest = result.merge_commit_digest();
    const auto* merge_commit_digest_address = std::addressof(merge_commit_digest);

    moved_owner.emplace(std::move(result));
    const auto& moved = *moved_owner;
    CHECK(!result.valid());
    CHECK(result.relation_count() == 0U);
    CHECK(result.completed_worker_count() == 0U);
    CHECK(moved.valid());
    CHECK(std::addressof(moved.merged_relations()) == view_address);
    CHECK(moved.chunks().data() == chunk_address);
    CHECK(std::addressof(moved.manifest_digest()) == manifest_digest_address);
    CHECK(std::addressof(moved.merge_commit_digest()) == merge_commit_digest_address);
    CHECK(moved.manifest_digest() == manifest_digest);
    CHECK(moved.merge_commit_digest() == merge_commit_digest);
    CHECK(copied_view.count() == moved.relation_count());
    require_exact_relations(moved, fixture_root.prepared());
}

void test_wave_result_retains_wave_lock_and_exposes_no_mutation() {
    WaveResultFixture fixture_root(
        "distributed-sieve-wave-result-authority",
        fixture::PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty);
    auto retained = fixture_root.drive_to_retained();
    auto promotion = result_detail::promote_distributed_sieve_wave_result_v1(std::move(retained));
    auto result = take_promoted(promotion);

    require_public_result_wave_lock_busy(fixture_root);
    require_no_m5_mutation_records(fixture_root.prepared().root());

    const pid_t child = ::fork();
    if (child < 0) {
        throw std::system_error(errno, std::generic_category(), "fork public result test");
    }
    if (child == 0) {
        ::_exit(result.valid() ? EXIT_FAILURE : PUBLIC_RESULT_FORK_REJECTED_EXIT);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "wait public result test");
        }
    }
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == PUBLIC_RESULT_FORK_REJECTED_EXIT);
    CHECK(result.valid());
    require_exact_relations(result, fixture_root.prepared());
}

#endif

void run_core_suite() {
#if defined(__APPLE__)
    test_wave_result_promotes_exact_commit_projection();
    std::cout << "  exact public commit projection: PASS\n";
    test_wave_result_rejects_spent_r4_authority();
    std::cout << "  spent R4 authority rejection: PASS\n";
    test_wave_result_returns_retryable_owner_before_spend();
    std::cout << "  pre-spend allocation retry: PASS\n";
#else
    throw TestFailure("core suite requires the macOS private-handoff runtime");
#endif
}

void run_protection_suite() {
#if defined(__APPLE__)
    test_wave_result_move_preserves_borrowed_view_and_spans();
    std::cout << "  move-stable borrowed view and summaries: PASS\n";
    test_wave_result_retains_wave_lock_and_exposes_no_mutation();
    std::cout << "  process, WaveLock, and no-mutation boundaries: PASS\n";
#else
    throw TestFailure("protection suite requires the macOS private-handoff runtime");
#endif
}

void run_platform_suite() {
#if defined(__APPLE__)
    throw TestFailure("platform suite is reserved for unsupported hosts");
#else
    static_assert(!std::is_constructible_v<WaveResult, std::filesystem::path>);
    static_assert(
        !std::is_constructible_v<WaveResult, std::span<const sieve::ChunkCommitSummaryV1>>);
    CHECK(wave_result_valid_link_probe != nullptr);
    CHECK(wave_result_bool_link_probe != nullptr);
    CHECK(wave_result_relation_count_link_probe != nullptr);
    CHECK(wave_result_worker_count_link_probe != nullptr);
    CHECK(wave_result_manifest_link_probe != nullptr);
    CHECK(wave_result_merge_commit_link_probe != nullptr);
    CHECK(wave_result_chunks_link_probe != nullptr);
    CHECK(wave_result_relations_link_probe != nullptr);
    CHECK(wave_result_move_construct_link_probe != nullptr);
    CHECK(wave_result_destroy_link_probe != nullptr);
    std::cout << "  public result remains unreachable without sealed R4 authority: PASS\n";
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
#endif

        if (argc == 1) {
#if defined(__APPLE__)
            run_core_suite();
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
            if (suite == "protection") {
                run_protection_suite();
                return EXIT_SUCCESS;
            }
            if (suite == "platform") {
                run_platform_suite();
                return EXIT_SUCCESS;
            }
        }
        std::cerr << "usage: " << argv[0] << " [--suite core|protection|platform]\n";
        return EXIT_FAILURE;
    } catch (const std::exception& failure) {
        std::cerr << "Distributed sieve wave-result tests FAILED: " << failure.what() << '\n';
        return EXIT_FAILURE;
    }
}
