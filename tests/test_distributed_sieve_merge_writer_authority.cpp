#include "distributed_sieve_merge_coordinator.hpp"
#include "distributed_sieve_merge_writer_authority_internal.hpp"
#include "distributed_sieve_wave_store_internal.hpp"

#include <gnfs/core/relation.hpp>
#include <gnfs/relation/ooc_cleanup_transaction.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/relation/relation_corpus_sha256.hpp>
#include <gnfs/relation/relation_sequence_receipt.hpp>
#include <gnfs/sieve/distributed_sieve_protocol.hpp>
#include <gnfs/util/durable_immutable_record.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace authority = gnfs::sieve::distributed_sieve_merge_writer_authority_detail;
namespace cleanup = gnfs::relation::ooc_cleanup_detail;
namespace coordinator = gnfs::sieve::distributed_sieve_merge_coordinator_detail;
namespace durable_record = gnfs::util::durable_immutable_record;
namespace relation = gnfs::relation;
namespace sieve = gnfs::sieve;
namespace wave = gnfs::sieve::distributed_sieve_resume_detail;
namespace worker = gnfs::sieve::distributed_sieve_worker_coordinator_detail;

using CoordinatorResult = worker::DistributedSieveWorkerCoordinatorResultV1;
using Digest = gnfs::util::Sha256Digest;
using Relation = gnfs::core::Relation;
using Record = sieve::DistributedSieveProtocolRecordV1;
using MergeWriterAuthority = authority::DistributedSieveMergeWriterAuthorityV1;
using PreparedAdmission = authority::DistributedSieveMergePreparedAdmissionV1;
using WriterAdoptionResult = authority::DistributedSieveMergeWriterAdoptionResultV1;
using PreparedResult = authority::DistributedSieveMergePreparedResultV1;
using WriterAdoptionTestHooks =
    authority::trusted_test::DistributedSieveMergeWriterAdoptionTestHooksV1;

template <typename T>
concept HasMutableWriterMember = requires(T& value) {
    { value.writer() } -> std::same_as<relation::OOCRelationWriter&>;
};

template <typename T>
concept HasWriterTakeMember = requires(T& value) { value.take_writer(); };

template <typename T>
concept HasCleanupReceiptMember = requires(T& value) { value.cleanup_receipt(); };

template <typename T>
concept HasLeaseOwnershipMember = requires(T& value) { value.lease_ownership(); };

static_assert(std::is_final_v<MergeWriterAuthority>);
static_assert(!std::is_default_constructible_v<MergeWriterAuthority>);
static_assert(!std::is_copy_constructible_v<MergeWriterAuthority>);
static_assert(!std::is_copy_assignable_v<MergeWriterAuthority>);
static_assert(std::is_nothrow_move_constructible_v<MergeWriterAuthority>);
static_assert(!std::is_move_assignable_v<MergeWriterAuthority>);
static_assert(!std::is_constructible_v<MergeWriterAuthority, std::filesystem::path>);
static_assert(!HasMutableWriterMember<MergeWriterAuthority>);
static_assert(!HasWriterTakeMember<MergeWriterAuthority>);
static_assert(!HasCleanupReceiptMember<MergeWriterAuthority>);
static_assert(!HasLeaseOwnershipMember<MergeWriterAuthority>);

static_assert(std::is_final_v<PreparedAdmission>);
static_assert(!std::is_default_constructible_v<PreparedAdmission>);
static_assert(!std::is_copy_constructible_v<PreparedAdmission>);
static_assert(!std::is_copy_assignable_v<PreparedAdmission>);
static_assert(std::is_nothrow_move_constructible_v<PreparedAdmission>);
static_assert(!std::is_move_assignable_v<PreparedAdmission>);
static_assert(!std::is_constructible_v<PreparedAdmission, sieve::MergePreparedV1>);
static_assert(!std::is_constructible_v<PreparedAdmission, std::filesystem::path>);
static_assert(!HasMutableWriterMember<PreparedAdmission>);
static_assert(!HasWriterTakeMember<PreparedAdmission>);
static_assert(!HasCleanupReceiptMember<PreparedAdmission>);
static_assert(!HasLeaseOwnershipMember<PreparedAdmission>);
static_assert(std::is_same_v<decltype(std::declval<const PreparedAdmission&>().valid()), bool>);
static_assert(std::is_same_v<decltype(std::declval<const PreparedAdmission&>().record()),
                             const sieve::MergePreparedV1&>);

static_assert(noexcept(authority::consume_distributed_sieve_merge_generation_v1(
    std::declval<coordinator::DistributedSieveMergeGenerationAdmissionV1&&>())));
static_assert(noexcept(authority::publish_distributed_sieve_merge_prepared_v1(
    std::declval<MergeWriterAuthority&&>())));
static_assert(
    std::is_same_v<decltype(authority::consume_distributed_sieve_merge_generation_v1(
                       std::declval<coordinator::DistributedSieveMergeGenerationAdmissionV1&&>())),
                   WriterAdoptionResult>);
static_assert(std::is_same_v<decltype(authority::publish_distributed_sieve_merge_prepared_v1(
                                 std::declval<MergeWriterAuthority&&>())),
                             PreparedResult>);
static_assert(
    noexcept(authority::trusted_test::consume_distributed_sieve_merge_generation_v1_with_hooks(
        std::declval<coordinator::DistributedSieveMergeGenerationAdmissionV1&&>(),
        std::declval<WriterAdoptionTestHooks>())));
static_assert(
    std::is_same_v<
        decltype(authority::trusted_test::consume_distributed_sieve_merge_generation_v1_with_hooks(
            std::declval<coordinator::DistributedSieveMergeGenerationAdmissionV1&&>(),
            std::declval<WriterAdoptionTestHooks>())),
        WriterAdoptionResult>);

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

[[nodiscard]] std::string authority_diagnostic_detail(
    const authority::DistributedSieveMergeWriterAuthorityDiagnosticV1& diagnostic) {
    std::string detail(
        authority::distributed_sieve_merge_writer_authority_status_name(diagnostic.status));
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

void require_wave_status(const wave::DistributedSieveWaveStoreDiagnostic& diagnostic,
                         wave::DistributedSieveWaveStoreStatus expected, std::string_view context) {
    if (diagnostic.status != expected) {
        fail(context, __LINE__, wave_diagnostic_detail(diagnostic));
    }
}

template <typename Value> [[nodiscard]] Value seal_value(Value value) {
    Record record(std::move(value));
    const auto status = sieve::seal_distributed_sieve_record(record);
    if (!status) {
        fail("seal distributed-sieve fixture record", __LINE__,
             sieve::distributed_sieve_protocol_error_name(status.error));
    }
    return std::get<Value>(std::move(record));
}

[[nodiscard]] std::vector<std::byte> encode_record(const Record& record) {
    const auto encoded = sieve::encode_distributed_sieve_record(record);
    if (!encoded || !encoded.bytes.has_value()) {
        fail("encode distributed-sieve fixture record", __LINE__,
             sieve::distributed_sieve_protocol_error_name(encoded.status.error));
    }
    return *encoded.bytes;
}

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

[[nodiscard]] Relation make_relation(std::int64_t a, std::uint64_t b, std::uint32_t seed) {
    Relation value(a, b);
    value.rational_factors = {seed, static_cast<std::uint32_t>(seed + 2U)};
    value.algebraic_factors = {
        static_cast<std::uint32_t>(seed + 1U),
        static_cast<std::uint32_t>(seed + 3U),
    };
    value.rational_large_prime = {
        {static_cast<std::uint64_t>(1009U + seed), 0, static_cast<std::uint8_t>(1U + seed % 2U)},
    };
    value.algebraic_large_prime = {
        {static_cast<std::uint64_t>(2003U + seed), static_cast<std::uint64_t>(17U + seed), 1},
    };
    value.extra_ab_pairs = {{a - 10, b + 1U}, {a + 20, b + 2U}};
    return value;
}

[[nodiscard]] bool relations_equal(const Relation& left, const Relation& right) noexcept {
    return left.a == right.a && left.b == right.b &&
           left.rational_factors == right.rational_factors &&
           left.algebraic_factors == right.algebraic_factors &&
           left.rational_large_prime == right.rational_large_prime &&
           left.algebraic_large_prime == right.algebraic_large_prime &&
           left.extra_ab_pairs == right.extra_ab_pairs;
}

[[nodiscard]] bool relation_vectors_equal(std::span<const Relation> left,
                                          std::span<const Relation> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!relations_equal(left[index], right[index])) {
            return false;
        }
    }
    return true;
}

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0};
        const auto tick =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::uint64_t attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("gnfs-merge-writer-authority-" + std::to_string(tick) + "-" +
                     std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + "-" +
                     std::to_string(attempt));
            std::error_code error;
            if (!std::filesystem::create_directory(path_, error)) {
                if (error == std::errc::file_exists) {
                    continue;
                }
                throw std::filesystem::filesystem_error(
                    "create merge-writer-authority fixture root", path_, error);
            }
#if !defined(_WIN32)
            if (::chmod(path_.c_str(), 0700) != 0) {
                const int native_error = errno;
                std::filesystem::remove_all(path_, error);
                throw std::system_error(native_error, std::generic_category(),
                                        "chmod merge-writer-authority fixture root");
            }
#endif
            path_ = std::filesystem::canonical(path_, error);
            if (error) {
                std::error_code ignored;
                (void)std::filesystem::remove_all(path_, ignored);
                throw std::filesystem::filesystem_error(
                    "canonicalize merge-writer-authority fixture root", path_, error);
            }
            return;
        }
        throw TestFailure("could not reserve merge-writer-authority fixture root");
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    ~TempDirectory() {
        std::error_code ignored;
        (void)std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

#if defined(__APPLE__)

class UniqueFd final {
public:
    explicit UniqueFd(int descriptor = -1) noexcept : descriptor_(descriptor) {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    UniqueFd& operator=(UniqueFd&&) = delete;
    ~UniqueFd() {
        if (descriptor_ >= 0) {
            (void)::close(descriptor_);
        }
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_ = -1;
};

[[nodiscard]] bool write_exact_fd(int descriptor, const void* data, std::size_t size) noexcept {
    const auto* cursor = static_cast<const std::byte*>(data);
    std::size_t written = 0;
    while (written < size) {
        const auto result = ::write(descriptor, cursor + written, size - written);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

[[nodiscard]] bool read_exact_fd(int descriptor, void* data, std::size_t size) noexcept {
    auto* cursor = static_cast<std::byte*>(data);
    std::size_t read = 0;
    while (read < size) {
        const auto result = ::read(descriptor, cursor + read, size - read);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        read += static_cast<std::size_t>(result);
    }
    return true;
}

[[nodiscard]] sieve::NativeIdentityV1 native_identity(const std::filesystem::path& path) {
    struct stat metadata{};
    if (::lstat(path.c_str(), &metadata) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "inspect merge-writer-authority fixture entry");
    }
    return {
        .volume = static_cast<std::uint64_t>(metadata.st_dev),
        .object = static_cast<std::uint64_t>(metadata.st_ino),
        .generation = 0,
    };
}

[[nodiscard]] std::uint64_t file_extent(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    CHECK(size <= std::numeric_limits<std::uint64_t>::max());
    return static_cast<std::uint64_t>(size);
}

[[nodiscard]] std::vector<std::byte> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::filesystem::filesystem_error("open merge-writer-authority fixture record", path,
                                                std::make_error_code(std::errc::io_error));
    }
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) {
        throw TestFailure("cannot size merge-writer-authority fixture record");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) {
        throw TestFailure("cannot read merge-writer-authority fixture record");
    }
    return bytes;
}

void publish_canonical_record(const std::filesystem::path& root, std::string_view pending_leaf,
                              std::string_view canonical_leaf, std::span<const std::byte> bytes) {
    int descriptor = -1;
    do {
        descriptor = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "open merge-writer-authority wave root");
    }
    UniqueFd held_root(descriptor);
    const auto published = durable_record::publish_at(
        static_cast<durable_record::NativeHandle>(held_root.get()),
        std::filesystem::path{pending_leaf}, std::filesystem::path{canonical_leaf}, bytes);
    CHECK(published.is_durable());
    CHECK(published.canonical_snapshot().has_value());
}

[[nodiscard]] sieve::WaveManifestV1 manifest_draft() {
    sieve::WaveManifestV1 manifest;
    manifest.wave_id = wave_id_with_seed(1);
    manifest.execution_contract_version = 1;
    manifest.executable_sha256 = digest_with_seed(2);
    manifest.work_sha256 = digest_with_seed(3);
    manifest.effective_sq_begin = 2;
    manifest.effective_sq_end = 4;
    manifest.worker_count = 3;
    manifest.chunks = {
        sieve::ChunkPlanV1{0, 2, 3, "authority_chunk_0"},
        sieve::ChunkPlanV1{1, 3, 4, "authority_chunk_1"},
        sieve::ChunkPlanV1{2, 4, 4, "authority_chunk_2"},
    };
    manifest.sq_cap_per_worker = 10;
    manifest.relation_cap_per_worker = 100;
    manifest.max_worker_attempts = 2;
    manifest.max_merge_build_attempts = 2;
    manifest.max_consumption_attempts = 2;
    manifest.canonical_naming_version = sieve::DISTRIBUTED_SIEVE_CANONICAL_NAMING_VERSION_V1;
    manifest.retry_policy_version = 1;
    manifest.durable_start_consumes_ordinal = true;
    manifest.ooc_format_version = relation::OOCRelationWriter::FORMAT_VERSION;
    manifest.relation_serialization_version = 1;
    manifest.handoff_version = sieve::DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1;
    manifest.receipt_version = 1;
    manifest.digest_version = 1;
    manifest.merge_policy_version = 1;
    return manifest;
}

[[nodiscard]] sieve::LeaseIdentityV1
lease_identity(const relation::OOCCleanupPaths& paths,
               const wave::DistributedSieveWorkerAttemptNamesV1& names) {
    const auto reserved =
        cleanup::parse_private_lease_marker(read_file_bytes(paths.lease_reserved_path));
    return {
        .lease_id = {.limbs = reserved.lease_id},
        .owner_marker = native_identity(cleanup::private_lease_owner_path(paths.private_directory)),
        .directory = native_identity(paths.private_directory),
        .relative_stem = names.relative_lease_stem,
    };
}

[[nodiscard]] sieve::AttemptStartedV1 attempt_started(const sieve::WaveManifestV1& manifest,
                                                      const sieve::ChunkPlanV1& chunk,
                                                      sieve::LeaseIdentityV1 lease) {
    sieve::AttemptStartedV1 attempt;
    attempt.manifest_digest = manifest.self_digest;
    attempt.chunk_id = chunk.chunk_id;
    attempt.sq_begin = chunk.sq_begin;
    attempt.sq_end = chunk.sq_end;
    attempt.attempt_ordinal = 0;
    attempt.predecessor_digest = manifest.self_digest;
    attempt.lease = std::move(lease);
    attempt.retry_policy_version = manifest.retry_policy_version;
    return seal_value(std::move(attempt));
}

[[nodiscard]] sieve::CorpusArtifactV1
artifact_descriptor(const relation::OOCSnapshotDescriptor& descriptor,
                    const relation::OOCCleanupPaths& paths,
                    const relation::RelationSequenceReceipt& sequence, const Digest& corpus) {
    return {
        .descriptor =
            {
                .format_version = descriptor.format_version,
                .store_id = descriptor.store_id,
                .generation = descriptor.generation,
                .relation_count = descriptor.count,
                .data_end = descriptor.data_end,
            },
        .index_file =
            {
                .identity = native_identity(paths.index_path),
                .extent = file_extent(paths.index_path),
            },
        .data_file =
            {
                .identity = native_identity(paths.data_path),
                .extent = file_extent(paths.data_path),
            },
        .sequence_receipt =
            {
                .relation_count = sequence.relation_count,
                .low = sequence.low,
                .high = sequence.high,
            },
        .corpus_sha256 = corpus,
    };
}

[[nodiscard]] gnfs::relation::OOCPrivateHandoffPairDescriptorV1
private_handoff_pair(const relation::OOCSnapshotDescriptor& descriptor,
                     const sieve::CorpusArtifactV1& artifact) noexcept {
    return {
        .format_version = descriptor.format_version,
        .store_id = descriptor.store_id,
        .generation = descriptor.generation,
        .count = descriptor.count,
        .index_extent = artifact.index_file.extent,
        .data_extent = artifact.data_file.extent,
    };
}

void publish_worker_handoff(wave::DistributedSieveWaveStore& store,
                            const std::filesystem::path& root, const sieve::ChunkPlanV1& chunk,
                            std::span<const Relation> rows) {
    auto created = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
    if (!created || created.claim == nullptr) {
        fail("create worker fixture P0", __LINE__, wave_diagnostic_detail(created.diagnostic));
    }
    created.claim.reset();

    const auto names = wave::distributed_sieve_worker_attempt_names_v1(chunk.relative_artifact_stem,
                                                                       chunk.chunk_id, 0);
    CHECK(names.has_value());
    const auto base = root / names->private_directory_leaf / "corpus";
    const auto paths = relation::OOCCleanupTransaction::paths_for(base);
    auto reservation = relation::OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    CHECK(reservation.ownership.has_value());

    const auto attempt = attempt_started(store.manifest(), chunk, lease_identity(paths, *names));
    const auto attempt_bytes = encode_record(Record{attempt});
    publish_canonical_record(root, names->pending_record_leaf, names->canonical_record_leaf,
                             attempt_bytes);

    relation::OOCRelationWriter writer(
        base.string(), std::move(*reservation.ownership),
        relation::OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
    relation::RelationSequenceReceiptAccumulator sequence_accumulator;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        CHECK(writer.write(rows[index]) == index);
        sequence_accumulator.append(rows[index]);
    }
    const auto descriptor = writer.finalize();
    auto pair_ownership = writer.take_cleanup_ownership_receipt();
    auto lease_ownership = writer.take_deferred_private_lease_ownership();
    const auto sequence = sequence_accumulator.finish();
    const auto corpus = relation::relation_corpus_sha256_v1(rows);
    CHECK(corpus.has_value());
    CHECK(descriptor.count == rows.size());
    const auto artifact = artifact_descriptor(descriptor, paths, sequence, *corpus);

    sieve::WorkerHandoffV1 handoff{
        .manifest_digest = store.manifest_digest(),
        .work_digest = store.manifest().work_sha256,
        .wave_id = store.manifest().wave_id,
        .chunk_id = chunk.chunk_id,
        .sq_begin = chunk.sq_begin,
        .sq_end = chunk.sq_end,
        .attempt_ordinal = 0,
        .attempt_started_digest = attempt.self_digest,
        .lease = attempt.lease,
        .artifact = artifact,
        .processed_sq_count = static_cast<std::uint64_t>(chunk.sq_end - chunk.sq_begin),
        .next_sq_index = chunk.sq_end,
        .completion_reason = rows.empty() ? sieve::WorkerCompletionReasonV1::zero_relations
                                          : sieve::WorkerCompletionReasonV1::range_exhausted,
        .relation_count = descriptor.count,
        .cleanup_intent_absent = true,
    };
    handoff = seal_value(std::move(handoff));
    const auto handoff_payload = encode_record(Record{handoff});
    const auto published = relation::OOCCleanupTransaction::publish_private_handoff(
        pair_ownership, lease_ownership, private_handoff_pair(descriptor, artifact),
        static_cast<std::uint32_t>(sieve::DistributedSieveRecordKindV1::worker_handoff),
        store.manifest().handoff_version, handoff_payload);
    CHECK(published.canonical());
}

class MergeAuthorityFixture final {
public:
    MergeAuthorityFixture(std::string_view label, std::array<std::vector<Relation>, 2> rows)
        : root_(temp_.path() / std::string(label)), rows_(std::move(rows)),
          opened_(wave::DistributedSieveWaveStore::create(root_, manifest_draft())) {
        if (!opened_ || opened_.store == nullptr) {
            fail("create merge-writer-authority WaveStore", __LINE__,
                 wave_diagnostic_detail(opened_.diagnostic));
        }
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

    [[nodiscard]] const std::array<std::vector<Relation>, 2>& rows() const noexcept {
        return rows_;
    }

    [[nodiscard]] Digest manifest_digest() const noexcept {
        CHECK(opened_.store != nullptr);
        return opened_.store->manifest_digest();
    }

    [[nodiscard]] CoordinatorResult take_worker_result() {
        CHECK(opened_.store != nullptr);
        auto& store = *opened_.store;
        std::array<std::optional<wave::DistributedSieveAdoptedWorkerChunkV1>, 2> adopted;
        for (std::size_t index = 0; index < adopted.size(); ++index) {
            publish_worker_handoff(store, root_, store.manifest().chunks[index], rows_[index]);
        }
        for (std::size_t index = 0; index < adopted.size(); ++index) {
            auto result = store.adopt_worker_handoff_v1(store.manifest().chunks[index].chunk_id);
            if (!result || !result.adopted.has_value()) {
                fail("adopt real worker handoff", __LINE__,
                     wave_diagnostic_detail(result.diagnostic));
            }
            CHECK(result.adopted->valid());
            CHECK(result.adopted->reader().count() == rows_[index].size());
            adopted[index].emplace(std::move(*result.adopted));
        }
        auto claimed = store.claim_worker_coordinator_v1();
        if (!claimed || claimed.claim == nullptr) {
            fail("claim worker coordinator for merge fixture", __LINE__,
                 wave_diagnostic_detail(claimed.diagnostic));
        }

        CoordinatorResult result;
        result.chunks.reserve(store.manifest().chunks.size());
        for (std::size_t index = 0; index < store.manifest().chunks.size(); ++index) {
            result.chunks.emplace_back();
            auto& coordinated = result.chunks.back();
            coordinated.chunk = store.manifest().chunks[index];
            if (index == adopted.size()) {
                CHECK(coordinated.chunk.sq_begin == coordinated.chunk.sq_end);
                coordinated.disposition =
                    worker::DistributedSieveWorkerCoordinationDispositionV1::empty;
            } else {
                coordinated.disposition =
                    worker::DistributedSieveWorkerCoordinationDispositionV1::adopted;
                coordinated.adopted.emplace(std::move(*adopted[index]));
            }
        }
        result.diagnostic.status = worker::DistributedSieveWorkerCoordinatorStatusV1::succeeded;
        result.diagnostic.wave_store.status = wave::DistributedSieveWaveStoreStatus::ready;
        result.coordinator_claim = std::move(claimed.claim);
        result.store = std::move(opened_.store);
        CHECK(result);
        return result;
    }

private:
    TempDirectory temp_;
    std::filesystem::path root_;
    std::array<std::vector<Relation>, 2> rows_;
    wave::DistributedSieveWaveStoreOpenResult opened_;
};

[[nodiscard]] constexpr std::uint64_t legacy_packed_key(const gnfs::core::ABPair& pair) noexcept {
    return static_cast<std::uint64_t>(pair.a) ^ (pair.b << 32U);
}

[[nodiscard]] std::array<std::vector<Relation>, 2> nonempty_worker_rows() {
    const auto first = make_relation(11, 13, 10);
    const auto duplicate = make_relation(11, 13, 90);
    const auto collision_x = make_relation(0, 1, 20);
    const auto collision_y = make_relation(static_cast<std::int64_t>(UINT64_C(3) << 32U), 2, 30);
    const auto tail = make_relation(-41, 43, 40);
    CHECK(legacy_packed_key(collision_x.ab()) == legacy_packed_key(collision_y.ab()));
    CHECK(collision_x.ab() != collision_y.ab());
    CHECK(!relations_equal(first, duplicate));
    return {
        std::vector<Relation>{first, collision_x},
        std::vector<Relation>{duplicate, collision_y, tail},
    };
}

[[nodiscard]] std::vector<Relation>
expected_nonempty_merged_rows(const std::array<std::vector<Relation>, 2>& rows) {
    CHECK(rows[0].size() == 2U);
    CHECK(rows[1].size() == 3U);
    return {
        rows[0][0],
        rows[0][1],
        rows[1][1],
        rows[1][2],
    };
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

void require_prepared_record(const sieve::WaveManifestV1& manifest,
                             const sieve::MergeStartedV1& started,
                             const sieve::MergePreparedV1& prepared,
                             const std::filesystem::path& root,
                             std::span<const Relation> expected_rows,
                             std::span<const sieve::PerChunkRetainedCountV1> expected_retained,
                             std::uint64_t expected_input_count,
                             std::uint64_t expected_duplicate_count) {
    const auto validated = sieve::validate_distributed_sieve_record(Record{prepared}, false);
    if (!validated) {
        fail("validate published MergePrepared", __LINE__,
             sieve::distributed_sieve_protocol_error_name(validated.error));
    }
    const std::array starts{started};
    const auto chain =
        sieve::validate_merge_predecessor_chain(manifest, starts, &prepared, nullptr);
    if (!chain) {
        fail("validate published MergePrepared predecessor chain", __LINE__,
             sieve::distributed_sieve_protocol_error_name(chain.error));
    }

    CHECK(prepared.manifest_digest == manifest.self_digest);
    CHECK(prepared.work_digest == manifest.work_sha256);
    CHECK(prepared.merge_policy_version == manifest.merge_policy_version);
    CHECK(prepared.merge_started_digest == started.self_digest);
    CHECK(prepared.ordered_inputs == started.ordered_inputs);
    CHECK(prepared.input_relation_count == expected_input_count);
    CHECK(prepared.duplicate_relation_count == expected_duplicate_count);
    CHECK(prepared.output_relation_count == expected_rows.size());
    CHECK(prepared.per_chunk_retained_counts ==
          std::vector(expected_retained.begin(), expected_retained.end()));
    CHECK(prepared.merged_lease == started.merged_lease);
    CHECK(prepared.merged_artifact.descriptor.format_version ==
          relation::OOCRelationWriter::FORMAT_VERSION);
    CHECK(prepared.merged_artifact.descriptor.store_id != 0U);
    CHECK(prepared.merged_artifact.descriptor.generation == 1U);
    CHECK(prepared.merged_artifact.descriptor.relation_count == expected_rows.size());
    CHECK(prepared.merged_artifact.descriptor.data_end >=
          relation::OOCRelationWriter::DATA_HEADER_BYTES);
    CHECK(prepared.merged_artifact.sequence_receipt.relation_count == expected_rows.size());

    const auto names =
        wave::distributed_sieve_merge_generation_names_v1(started.merge_attempt_ordinal);
    CHECK(names.has_value());
    const auto base = root / names->private_directory_leaf / "corpus";
    const auto paths = relation::OOCCleanupTransaction::paths_for(base);
    CHECK(prepared.merged_artifact.index_file.identity == native_identity(paths.index_path));
    CHECK(prepared.merged_artifact.data_file.identity == native_identity(paths.data_path));
    CHECK(prepared.merged_artifact.index_file.extent == file_extent(paths.index_path));
    CHECK(prepared.merged_artifact.data_file.extent == file_extent(paths.data_path));
    CHECK(prepared.merged_artifact.index_file.extent ==
          relation::OOCRelationWriter::index_size_for_count(expected_rows.size()));
    CHECK(prepared.merged_artifact.data_file.extent ==
          prepared.merged_artifact.descriptor.data_end);

    relation::OOCRelationReader reader(base.string(),
                                       relation_descriptor(prepared.merged_artifact));
    CHECK(reader.valid());
    CHECK(reader.count() == expected_rows.size());
    const auto actual_rows = reader.read_all();
    CHECK(relation_vectors_equal(actual_rows, expected_rows));
    const auto sequence = relation::relation_sequence_receipt(actual_rows);
    const auto corpus = relation::relation_corpus_sha256_v1(actual_rows);
    CHECK(corpus.has_value());
    CHECK(prepared.merged_artifact.sequence_receipt.relation_count == sequence.relation_count);
    CHECK(prepared.merged_artifact.sequence_receipt.low == sequence.low);
    CHECK(prepared.merged_artifact.sequence_receipt.high == sequence.high);
    CHECK(prepared.merged_artifact.corpus_sha256 == *corpus);
}

[[nodiscard]] sieve::MergePreparedV1
read_typed_merge_prepared_handoff(const std::filesystem::path& path,
                                  std::uint32_t handoff_version) {
    const auto outer = relation::decode_ooc_private_handoff_record(read_file_bytes(path));
    CHECK(outer);
    CHECK(outer.value.has_value());
    CHECK(outer.value->payload_kind ==
          static_cast<std::uint32_t>(sieve::DistributedSieveRecordKindV1::merge_prepared));
    CHECK(outer.value->payload_version == handoff_version);
    const auto decoded = sieve::decode_distributed_sieve_record(outer.value->opaque_payload);
    CHECK(decoded);
    CHECK(decoded.value.has_value());
    const auto* prepared = std::get_if<sieve::MergePreparedV1>(&*decoded.value);
    CHECK(prepared != nullptr);
    return *prepared;
}

[[nodiscard]] coordinator::DistributedSieveMergeGenerationAdmissionV1
begin_merge_generation(CoordinatorResult result) {
    auto admission =
        coordinator::begin_or_resume_distributed_sieve_merge_generation_v1(std::move(result));
    if (!admission) {
        fail("admit complete worker result for merge", __LINE__,
             wave_diagnostic_detail(admission.diagnostic().wave_store));
    }
    CHECK(admission.started_receipt() != nullptr);
    CHECK(admission.started_receipt()->owned_by_current_process());
    require_wave_status(admission.started_receipt()->revalidate(),
                        wave::DistributedSieveWaveStoreStatus::ready,
                        "revalidate admitted MergeStarted receipt");
    return admission;
}

[[nodiscard]] WriterAdoptionResult
consume_merge_generation(coordinator::DistributedSieveMergeGenerationAdmissionV1&& admission) {
    auto adopted = authority::consume_distributed_sieve_merge_generation_v1(std::move(admission));
    if (!adopted || !adopted.authority.has_value()) {
        fail("consume admitted merge generation", __LINE__,
             authority_diagnostic_detail(adopted.diagnostic));
    }
    CHECK(adopted.authority->valid());
    CHECK(adopted.diagnostic.phase ==
          authority::DistributedSieveMergeWriterAuthorityPhaseV1::complete);
    CHECK(adopted.diagnostic.status ==
          authority::DistributedSieveMergeWriterAuthorityStatusV1::ready);
    CHECK(!adopted.diagnostic.reconciliation_required);
    return adopted;
}

[[nodiscard]] PreparedResult publish_merge_prepared(WriterAdoptionResult adopted) {
    CHECK(adopted.authority.has_value());
    auto published =
        authority::publish_distributed_sieve_merge_prepared_v1(std::move(*adopted.authority));
    if (!published || !published.admission.has_value()) {
        fail("publish typed MergePrepared", __LINE__,
             authority_diagnostic_detail(published.diagnostic));
    }
    CHECK(published.admission->valid());
    CHECK(published.diagnostic.phase ==
          authority::DistributedSieveMergeWriterAuthorityPhaseV1::complete);
    CHECK(published.diagnostic.status ==
          authority::DistributedSieveMergeWriterAuthorityStatusV1::ready);
    CHECK(!published.diagnostic.reconciliation_required);
    CHECK(!adopted.authority->valid());
    return published;
}

void test_real_nonempty_merge_end_to_end() {
    MergeAuthorityFixture fixture("nonempty", nonempty_worker_rows());
    auto worker_result = fixture.take_worker_result();
    CHECK(worker_result.store != nullptr);
    const auto manifest = worker_result.store->manifest();
    const auto expected_rows = expected_nonempty_merged_rows(fixture.rows());

    auto admission = begin_merge_generation(std::move(worker_result));
    const auto started = admission.started_receipt()->record();
    CHECK(started.ordered_inputs.size() == manifest.chunks.size());
    CHECK(started.ordered_inputs[0].raw_relation_count == 2U);
    CHECK(started.ordered_inputs[1].raw_relation_count == 3U);
    CHECK(started.ordered_inputs[2].disposition == sieve::ChunkDispositionV1::empty);

    auto adopted = consume_merge_generation(std::move(admission));
    CHECK(!admission);
    const auto reused =
        authority::consume_distributed_sieve_merge_generation_v1(std::move(admission));
    CHECK(!reused);
    CHECK(!reused.authority.has_value());
    CHECK(reused.diagnostic.phase ==
          authority::DistributedSieveMergeWriterAuthorityPhaseV1::admission_validation);
    CHECK(reused.diagnostic.status ==
          authority::DistributedSieveMergeWriterAuthorityStatusV1::invalid_admission);
    CHECK(!reused.diagnostic.reconciliation_required);

    auto published = publish_merge_prepared(std::move(adopted));
    const std::array expected_retained{
        sieve::PerChunkRetainedCountV1{.chunk_id = 0, .retained_relation_count = 2},
        sieve::PerChunkRetainedCountV1{.chunk_id = 1, .retained_relation_count = 2},
        sieve::PerChunkRetainedCountV1{.chunk_id = 2, .retained_relation_count = 0},
    };
    require_prepared_record(manifest, started, published.admission->record(), fixture.root(),
                            expected_rows, expected_retained, 5, 1);
    CHECK(relations_equal(expected_rows.front(), fixture.rows()[0][0]));
    CHECK(!relations_equal(expected_rows.front(), fixture.rows()[1][0]));
}

struct ForkBatchContext final {
    std::filesystem::path data_path;
    int report_read_descriptor = -1;
    int report_write_descriptor = -1;
    pid_t child_process_id = -1;
    int fork_error = 0;
    int stat_error = 0;
    std::uint64_t disk_data_extent = std::numeric_limits<std::uint64_t>::max();
    std::size_t hook_input_slot = std::numeric_limits<std::size_t>::max();
    std::uint64_t hook_relation_ordinal = std::numeric_limits<std::uint64_t>::max();
    bool invoked = false;
    bool in_child = false;
};

void fork_after_first_output_write(std::size_t input_slot, std::uint64_t relation_ordinal,
                                   void* opaque) noexcept {
    auto& context = *static_cast<ForkBatchContext*>(opaque);
    if (context.invoked) {
        return;
    }
    context.invoked = true;
    context.hook_input_slot = input_slot;
    context.hook_relation_ordinal = relation_ordinal;

    struct stat metadata{};
    if (::lstat(context.data_path.c_str(), &metadata) != 0) {
        context.stat_error = errno;
    } else if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0) {
        context.stat_error = EINVAL;
    } else {
        context.disk_data_extent = static_cast<std::uint64_t>(metadata.st_size);
    }

    const pid_t child = ::fork();
    if (child < 0) {
        context.fork_error = errno;
        return;
    }
    if (child == 0) {
        context.in_child = true;
        context.child_process_id = 0;
        if (context.report_read_descriptor >= 0) {
            (void)::close(context.report_read_descriptor);
            context.report_read_descriptor = -1;
        }
        return;
    }
    context.child_process_id = child;
}

inline constexpr std::uint32_t FORK_CHILD_REPORT_MAGIC = UINT32_C(0x474E4653);
inline constexpr std::uint32_t FORK_CHILD_RESULT_REJECTED = UINT32_C(1) << 0U;
inline constexpr std::uint32_t FORK_CHILD_NO_AUTHORITY = UINT32_C(1) << 1U;
inline constexpr std::uint32_t FORK_CHILD_RECONCILIATION_REQUIRED = UINT32_C(1) << 2U;
inline constexpr std::uint32_t FORK_CHILD_AUTHORITY_STREAMING = UINT32_C(1) << 3U;
inline constexpr std::uint32_t FORK_CHILD_AUTHORITY_STREAM_FAILED = UINT32_C(1) << 4U;
inline constexpr std::uint32_t FORK_CHILD_STREAM_OUTPUT_WRITE = UINT32_C(1) << 5U;
inline constexpr std::uint32_t FORK_CHILD_STREAM_OUTPUT_WRITE_FAILED = UINT32_C(1) << 6U;
inline constexpr std::uint32_t FORK_CHILD_SECOND_ROW_REJECTED = UINT32_C(1) << 7U;
inline constexpr std::uint32_t FORK_CHILD_EXPECTED_FLAGS =
    FORK_CHILD_RESULT_REJECTED | FORK_CHILD_NO_AUTHORITY | FORK_CHILD_RECONCILIATION_REQUIRED |
    FORK_CHILD_AUTHORITY_STREAMING | FORK_CHILD_AUTHORITY_STREAM_FAILED |
    FORK_CHILD_STREAM_OUTPUT_WRITE | FORK_CHILD_STREAM_OUTPUT_WRITE_FAILED |
    FORK_CHILD_SECOND_ROW_REJECTED;

struct ForkChildReport final {
    std::uint32_t magic = FORK_CHILD_REPORT_MAGIC;
    std::uint32_t flags = 0;
};

[[noreturn]] void finish_fork_child(WriterAdoptionResult& adopted,
                                    const ForkBatchContext& context) noexcept {
    ForkChildReport report;
    if (!adopted) {
        report.flags |= FORK_CHILD_RESULT_REJECTED;
    }
    if (!adopted.authority.has_value()) {
        report.flags |= FORK_CHILD_NO_AUTHORITY;
    }
    if (adopted.diagnostic.reconciliation_required) {
        report.flags |= FORK_CHILD_RECONCILIATION_REQUIRED;
    }
    if (adopted.diagnostic.phase ==
        authority::DistributedSieveMergeWriterAuthorityPhaseV1::streaming) {
        report.flags |= FORK_CHILD_AUTHORITY_STREAMING;
    }
    if (adopted.diagnostic.status ==
        authority::DistributedSieveMergeWriterAuthorityStatusV1::stream_failed) {
        report.flags |= FORK_CHILD_AUTHORITY_STREAM_FAILED;
    }
    if (adopted.diagnostic.stream.phase == gnfs::sieve::distributed_sieve_merge_writer_detail::
                                               DistributedSieveMergeWriterPhaseV1::output_write) {
        report.flags |= FORK_CHILD_STREAM_OUTPUT_WRITE;
    }
    if (adopted.diagnostic.stream.status ==
        gnfs::sieve::distributed_sieve_merge_writer_detail::DistributedSieveMergeWriterStatusV1::
            output_write_failed) {
        report.flags |= FORK_CHILD_STREAM_OUTPUT_WRITE_FAILED;
    }
    if (adopted.diagnostic.stream.input_slot == 0U &&
        adopted.diagnostic.stream.relation_ordinal == 1U) {
        report.flags |= FORK_CHILD_SECOND_ROW_REJECTED;
    }

    // Force any unexpectedly returned authority through its process-aware
    // destructor before reporting. The normal failure path has already run
    // write fail-close, the exact-batch guard destructor, and authority abort.
    adopted.authority.reset();
    const bool reported = context.report_write_descriptor >= 0 &&
                          write_exact_fd(context.report_write_descriptor, &report, sizeof(report));
    const bool closed =
        context.report_write_descriptor < 0 || ::close(context.report_write_descriptor) == 0;
    ::_exit(reported && closed ? EXIT_SUCCESS : EXIT_FAILURE);
}

void test_fork_child_exact_batch_does_not_flush_parent() {
    MergeAuthorityFixture fixture("fork-batch-no-flush", nonempty_worker_rows());
    auto worker_result = fixture.take_worker_result();
    CHECK(worker_result.store != nullptr);
    const auto manifest = worker_result.store->manifest();
    const auto expected_rows = expected_nonempty_merged_rows(fixture.rows());
    auto admission = begin_merge_generation(std::move(worker_result));
    const auto started = admission.started_receipt()->record();

    const auto names =
        wave::distributed_sieve_merge_generation_names_v1(started.merge_attempt_ordinal);
    CHECK(names.has_value());
    const auto paths = relation::OOCCleanupTransaction::paths_for(
        fixture.root() / names->private_directory_leaf / "corpus");

    std::array<int, 2> report_pipe{-1, -1};
    CHECK(::pipe(report_pipe.data()) == 0);
    ForkBatchContext context{
        .data_path = paths.data_path,
        .report_read_descriptor = report_pipe[0],
        .report_write_descriptor = report_pipe[1],
    };
    auto adopted =
        authority::trusted_test::consume_distributed_sieve_merge_generation_v1_with_hooks(
            std::move(admission), WriterAdoptionTestHooks{
                                      .stream_hooks =
                                          {
                                              .after_output_write = fork_after_first_output_write,
                                              .context = &context,
                                          },
                                  });
    if (context.in_child) {
        finish_fork_child(adopted, context);
    }

    CHECK(context.invoked);
    CHECK(context.hook_input_slot == 0U);
    CHECK(context.hook_relation_ordinal == 0U);
    CHECK(context.stat_error == 0);
    CHECK(context.disk_data_extent == relation::OOCRelationWriter::DATA_HEADER_BYTES);
    CHECK(context.fork_error == 0);
    CHECK(context.child_process_id > 0);
    CHECK(::close(context.report_write_descriptor) == 0);
    context.report_write_descriptor = -1;

    ForkChildReport child_report;
    const bool report_complete =
        read_exact_fd(context.report_read_descriptor, &child_report, sizeof(child_report));
    const bool report_read_closed = ::close(context.report_read_descriptor) == 0;
    context.report_read_descriptor = -1;

    int child_status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(context.child_process_id, &child_status, 0);
    } while (waited < 0 && errno == EINTR);
    CHECK(waited == context.child_process_id);
    CHECK(report_complete);
    CHECK(report_read_closed);
    CHECK(WIFEXITED(child_status));
    CHECK(WEXITSTATUS(child_status) == EXIT_SUCCESS);
    CHECK(child_report.magic == FORK_CHILD_REPORT_MAGIC);
    CHECK((child_report.flags & FORK_CHILD_EXPECTED_FLAGS) == FORK_CHILD_EXPECTED_FLAGS);

    if (!adopted || !adopted.authority.has_value()) {
        fail("parent consumes merge after fork-child batch rejection", __LINE__,
             authority_diagnostic_detail(adopted.diagnostic));
    }
    CHECK(adopted.authority->valid());
    auto published = publish_merge_prepared(std::move(adopted));
    const std::array expected_retained{
        sieve::PerChunkRetainedCountV1{.chunk_id = 0, .retained_relation_count = 2},
        sieve::PerChunkRetainedCountV1{.chunk_id = 1, .retained_relation_count = 2},
        sieve::PerChunkRetainedCountV1{.chunk_id = 2, .retained_relation_count = 0},
    };
    require_prepared_record(manifest, started, published.admission->record(), fixture.root(),
                            expected_rows, expected_retained, 5, 1);
}

void test_real_zero_row_merge_end_to_end() {
    MergeAuthorityFixture fixture("zero-row", std::array<std::vector<Relation>, 2>{
                                                  std::vector<Relation>{},
                                                  std::vector<Relation>{},
                                              });
    auto worker_result = fixture.take_worker_result();
    CHECK(worker_result.store != nullptr);
    const auto manifest = worker_result.store->manifest();
    auto admission = begin_merge_generation(std::move(worker_result));
    const auto started = admission.started_receipt()->record();
    CHECK(started.ordered_inputs.size() == 3U);
    CHECK(started.ordered_inputs[0].disposition == sieve::ChunkDispositionV1::handoff);
    CHECK(started.ordered_inputs[0].raw_relation_count == 0U);
    CHECK(started.ordered_inputs[0].completion_reason ==
          sieve::WorkerCompletionReasonV1::zero_relations);
    CHECK(started.ordered_inputs[1].disposition == sieve::ChunkDispositionV1::handoff);
    CHECK(started.ordered_inputs[1].raw_relation_count == 0U);
    CHECK(started.ordered_inputs[2].disposition == sieve::ChunkDispositionV1::empty);

    auto published = publish_merge_prepared(consume_merge_generation(std::move(admission)));
    const std::array expected_retained{
        sieve::PerChunkRetainedCountV1{.chunk_id = 0, .retained_relation_count = 0},
        sieve::PerChunkRetainedCountV1{.chunk_id = 1, .retained_relation_count = 0},
        sieve::PerChunkRetainedCountV1{.chunk_id = 2, .retained_relation_count = 0},
    };
    const std::array<Relation, 0> expected_rows{};
    require_prepared_record(manifest, started, published.admission->record(), fixture.root(),
                            expected_rows, expected_retained, 0, 0);
}

void test_invalid_admission_fails_closed() {
    CoordinatorResult invalid_worker_result;
    auto admission = coordinator::begin_or_resume_distributed_sieve_merge_generation_v1(
        std::move(invalid_worker_result));
    CHECK(!admission);
    CHECK(admission.started_receipt() == nullptr);
    auto adopted = authority::consume_distributed_sieve_merge_generation_v1(std::move(admission));
    CHECK(!adopted);
    CHECK(!adopted.authority.has_value());
    CHECK(adopted.diagnostic.phase ==
          authority::DistributedSieveMergeWriterAuthorityPhaseV1::admission_validation);
    CHECK(adopted.diagnostic.status ==
          authority::DistributedSieveMergeWriterAuthorityStatusV1::invalid_admission);
    CHECK(!adopted.diagnostic.reconciliation_required);
}

[[nodiscard]] bool stop_private_handoff_at(relation::OOCPrivateHandoffFaultPoint point,
                                           void* opaque) noexcept {
    return point == *static_cast<const relation::OOCPrivateHandoffFaultPoint*>(opaque);
}

enum class MergePreparedPublicationPrefixShape : std::uint8_t {
    pending_only,
    canonical_only,
    identical_dual,
};

void test_real_typed_publication_prefix(MergePreparedPublicationPrefixShape shape,
                                        std::string_view label) {
    MergeAuthorityFixture fixture(label, nonempty_worker_rows());
    auto worker_result = fixture.take_worker_result();
    CHECK(worker_result.store != nullptr);
    const auto manifest = worker_result.store->manifest();
    const auto manifest_digest = worker_result.store->manifest_digest();
    auto admission = begin_merge_generation(std::move(worker_result));
    const auto started = admission.started_receipt()->record();
    auto adopted = consume_merge_generation(std::move(admission));
    CHECK(adopted.authority.has_value());

    const bool pending_only = shape == MergePreparedPublicationPrefixShape::pending_only;
    const bool identical_dual = shape == MergePreparedPublicationPrefixShape::identical_dual;
    auto target = pending_only ? relation::OOCPrivateHandoffFaultPoint::PendingDurable
                               : relation::OOCPrivateHandoffFaultPoint::CanonicalPromoted;
    auto interrupted =
        authority::trusted_test::publish_distributed_sieve_merge_prepared_v1_with_hooks(
            std::move(*adopted.authority),
            authority::trusted_test::DistributedSieveMergePreparedPublicationTestHooksV1{
                .private_handoff_hooks =
                    {
                        .stop_after = stop_private_handoff_at,
                        .context = &target,
                    },
            });
    CHECK(!interrupted);
    CHECK(!interrupted.admission.has_value());
    CHECK(interrupted.diagnostic.phase ==
          authority::DistributedSieveMergeWriterAuthorityPhaseV1::handoff_publication);
    CHECK(interrupted.diagnostic.status ==
          authority::DistributedSieveMergeWriterAuthorityStatusV1::handoff_publication_failed);
    CHECK(interrupted.diagnostic.reconciliation_required);
    CHECK(!adopted.authority->valid());

    const auto names =
        wave::distributed_sieve_merge_generation_names_v1(started.merge_attempt_ordinal);
    CHECK(names.has_value());
    const auto paths = relation::OOCCleanupTransaction::paths_for(
        fixture.root() / names->private_directory_leaf / "corpus");
    if (identical_dual) {
        constexpr std::string_view fixture_pending =
            ".gnfs-wave-v1.test-merge-prepared-identical.pending";
        publish_canonical_record(paths.private_directory, fixture_pending,
                                 paths.private_handoff_pending_path.filename().string(),
                                 read_file_bytes(paths.private_handoff_path));
        CHECK(native_identity(paths.private_handoff_path) !=
              native_identity(paths.private_handoff_pending_path));
    }
    CHECK(std::filesystem::exists(paths.private_handoff_path) == !pending_only);
    CHECK(std::filesystem::exists(paths.private_handoff_pending_path) ==
          (pending_only || identical_dual));
    const auto visible =
        pending_only ? paths.private_handoff_pending_path : paths.private_handoff_path;
    const auto visible_bytes = read_file_bytes(visible);
    const auto prepared = read_typed_merge_prepared_handoff(visible, manifest.handoff_version);
    const auto expected_rows = expected_nonempty_merged_rows(fixture.rows());
    const std::array expected_retained{
        sieve::PerChunkRetainedCountV1{.chunk_id = 0, .retained_relation_count = 2},
        sieve::PerChunkRetainedCountV1{.chunk_id = 1, .retained_relation_count = 2},
        sieve::PerChunkRetainedCountV1{.chunk_id = 2, .retained_relation_count = 0},
    };
    require_prepared_record(manifest, started, prepared, fixture.root(), expected_rows,
                            expected_retained, 5, 1);

    const auto next_ordinal = started.merge_attempt_ordinal + 1U;
    const auto next_names = wave::distributed_sieve_merge_generation_names_v1(next_ordinal);
    CHECK(next_names.has_value());
    const auto require_no_next_merge = [&] {
        CHECK(!std::filesystem::exists(fixture.root() / next_names->base_lock_leaf));
        CHECK(!std::filesystem::exists(fixture.root() / next_names->private_directory_leaf));
        CHECK(!std::filesystem::exists(fixture.root() / next_names->canonical_record_leaf));
        CHECK(!std::filesystem::exists(fixture.root() / next_names->pending_record_leaf));
    };
    require_no_next_merge();

    auto reopened = wave::DistributedSieveWaveStore::open(fixture.root(), manifest_digest);
    CHECK(reopened);
    CHECK(reopened.store != nullptr);
    require_wave_status(reopened.diagnostic, wave::DistributedSieveWaveStoreStatus::ready,
                        "cold open reconciles real typed MergePrepared prefix");
    CHECK(!std::filesystem::exists(fixture.root() / names->reserved_leaf));
    CHECK(!std::filesystem::exists(fixture.root() / names->reserved_pending_leaf));
    CHECK(!std::filesystem::exists(fixture.root() / names->owned_pending_leaf));
    CHECK(!std::filesystem::exists(fixture.root() / names->rollback_handoff_leaf));
    CHECK(std::filesystem::exists(fixture.root() / names->canonical_record_leaf));
    CHECK(!std::filesystem::exists(fixture.root() / names->pending_record_leaf));
    if (pending_only) {
        CHECK(!std::filesystem::exists(fixture.root() / names->owned_leaf));
        CHECK(!std::filesystem::exists(paths.private_directory));
        CHECK(!std::filesystem::exists(paths.private_handoff_path));
        CHECK(!std::filesystem::exists(paths.private_handoff_pending_path));
    } else {
        CHECK(std::filesystem::exists(fixture.root() / names->owned_leaf));
        CHECK(std::filesystem::exists(paths.private_directory));
        CHECK(std::filesystem::exists(paths.private_handoff_path));
        CHECK(!std::filesystem::exists(paths.private_handoff_pending_path));
        CHECK(read_file_bytes(paths.private_handoff_path) == visible_bytes);
    }
    require_no_next_merge();

    const auto cursor = wave::prepare_distributed_sieve_merge_generation_v1(*reopened.store);
    if (pending_only) {
        CHECK(cursor);
        CHECK(cursor.merge_attempt_ordinal == next_ordinal);
        require_wave_status(cursor.diagnostic, wave::DistributedSieveWaveStoreStatus::ready,
                            "rolled-back MergePrepared permits the next merge ordinal");
    } else {
        CHECK(!cursor);
        CHECK(!cursor.merge_attempt_ordinal.has_value());
        require_wave_status(cursor.diagnostic,
                            wave::DistributedSieveWaveStoreStatus::reconciliation_required,
                            "canonical MergePrepared does not start a duplicate merge");
    }
    require_no_next_merge();
}

void test_real_typed_publication_prefixes() {
    test_real_typed_publication_prefix(MergePreparedPublicationPrefixShape::pending_only,
                                       "prepared-pending");
    test_real_typed_publication_prefix(MergePreparedPublicationPrefixShape::canonical_only,
                                       "prepared-canonical-promoted");
    test_real_typed_publication_prefix(MergePreparedPublicationPrefixShape::identical_dual,
                                       "prepared-identical-dual");
}

#endif

} // namespace

int main() {
#if !defined(__APPLE__)
    std::cout << "Distributed sieve merge writer authority tests skipped: "
                 "private handoff publication requires macOS\n";
    return 0;
#else
    try {
        test_real_nonempty_merge_end_to_end();
        test_fork_child_exact_batch_does_not_flush_parent();
        test_real_zero_row_merge_end_to_end();
        test_invalid_admission_fails_closed();
        test_real_typed_publication_prefixes();
        std::cout << "Distributed sieve merge writer authority tests PASSED\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Distributed sieve merge writer authority tests FAILED: " << failure.what()
                  << '\n';
        return 1;
    }
#endif
}
