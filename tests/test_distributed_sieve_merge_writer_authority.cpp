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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
    struct stat metadata {};
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

[[nodiscard]] std::uint64_t read_file_u64(const std::filesystem::path& path, std::uint64_t offset) {
    CHECK(offset <= static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::filesystem::filesystem_error("open merge-writer-authority fixture integer", path,
                                                std::make_error_code(std::errc::io_error));
    }
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::uint64_t value = 0;
    input.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(value)));
    if (!input) {
        throw TestFailure("cannot read merge-writer-authority fixture integer");
    }
    return value;
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

enum class RawMergeWriterResidueShapeV1 : std::uint8_t {
    empty_incomplete,
    partial_incomplete,
    complete_incomplete,
    finalized_without_handoff,
};

[[nodiscard]] std::string_view
raw_merge_writer_residue_label(RawMergeWriterResidueShapeV1 shape) noexcept {
    switch (shape) {
    case RawMergeWriterResidueShapeV1::empty_incomplete:
        return "raw-empty-incomplete";
    case RawMergeWriterResidueShapeV1::partial_incomplete:
        return "raw-partial-incomplete";
    case RawMergeWriterResidueShapeV1::complete_incomplete:
        return "raw-complete-incomplete";
    case RawMergeWriterResidueShapeV1::finalized_without_handoff:
        return "raw-finalized-without-handoff";
    }
    return "raw-unknown";
}

[[nodiscard]] std::array<std::vector<Relation>, 2>
raw_merge_writer_worker_rows(RawMergeWriterResidueShapeV1 shape) {
    if (shape == RawMergeWriterResidueShapeV1::empty_incomplete) {
        return {};
    }
    return nonempty_worker_rows();
}

[[nodiscard]] constexpr std::uint64_t
raw_merge_writer_persisted_relation_count(RawMergeWriterResidueShapeV1 shape) noexcept {
    switch (shape) {
    case RawMergeWriterResidueShapeV1::empty_incomplete:
        return 0;
    case RawMergeWriterResidueShapeV1::partial_incomplete:
        return 1;
    case RawMergeWriterResidueShapeV1::complete_incomplete:
    case RawMergeWriterResidueShapeV1::finalized_without_handoff:
        return 4;
    }
    return 0;
}

struct StopAfterFirstOutputWriteContextV1 final {
    std::size_t input_slot = std::numeric_limits<std::size_t>::max();
    std::uint64_t relation_ordinal = std::numeric_limits<std::uint64_t>::max();
    bool invoked = false;
};

[[nodiscard]] bool stop_after_first_output_write(std::size_t input_slot,
                                                 std::uint64_t relation_ordinal,
                                                 void* opaque) noexcept {
    auto& context = *static_cast<StopAfterFirstOutputWriteContextV1*>(opaque);
    if (context.invoked) {
        return false;
    }
    context.input_slot = input_slot;
    context.relation_ordinal = relation_ordinal;
    context.invoked = true;
    return true;
}

[[nodiscard]] bool stop_after_payload_build_before_handoff(void* opaque) noexcept {
    auto& invoked = *static_cast<bool*>(opaque);
    invoked = true;
    return true;
}

struct RawMergeWriterResidueStateV1 final {
    Digest manifest_digest;
    sieve::MergeStartedV1 started;
    wave::DistributedSieveMergeGenerationNamesV1 names;
    relation::OOCCleanupPaths paths;
    std::uint64_t persisted_relation_count = 0;
    bool finalized = false;
};

[[nodiscard]] RawMergeWriterResidueStateV1
materialize_raw_merge_writer_residue(MergeAuthorityFixture& fixture,
                                     RawMergeWriterResidueShapeV1 shape) {
    auto worker_result = fixture.take_worker_result();
    CHECK(worker_result.store != nullptr);
    const auto manifest_digest = worker_result.store->manifest_digest();
    auto admission = begin_merge_generation(std::move(worker_result));
    const auto started = admission.started_receipt()->record();
    const auto names =
        wave::distributed_sieve_merge_generation_names_v1(started.merge_attempt_ordinal);
    CHECK(names.has_value());
    const auto paths = relation::OOCCleanupTransaction::paths_for(
        fixture.root() / names->private_directory_leaf / "corpus");

    if (shape == RawMergeWriterResidueShapeV1::partial_incomplete) {
        StopAfterFirstOutputWriteContextV1 stop;
        auto interrupted =
            authority::trusted_test::consume_distributed_sieve_merge_generation_v1_with_hooks(
                std::move(admission),
                WriterAdoptionTestHooks{
                    .stream_hooks =
                        {
                            .stop_after_output_write = stop_after_first_output_write,
                            .context = &stop,
                        },
                });
        CHECK(stop.invoked);
        CHECK(stop.input_slot == 0U);
        CHECK(stop.relation_ordinal == 0U);
        CHECK(!interrupted);
        CHECK(!interrupted.authority.has_value());
        CHECK(interrupted.diagnostic.phase ==
              authority::DistributedSieveMergeWriterAuthorityPhaseV1::streaming);
        CHECK(interrupted.diagnostic.status ==
              authority::DistributedSieveMergeWriterAuthorityStatusV1::stream_failed);
        CHECK(interrupted.diagnostic.stream.phase ==
              gnfs::sieve::distributed_sieve_merge_writer_detail::
                  DistributedSieveMergeWriterPhaseV1::output_write);
        CHECK(interrupted.diagnostic.stream.status ==
              gnfs::sieve::distributed_sieve_merge_writer_detail::
                  DistributedSieveMergeWriterStatusV1::output_write_failed);
        CHECK(interrupted.diagnostic.reconciliation_required);
    } else {
        auto adopted = consume_merge_generation(std::move(admission));
        CHECK(adopted.authority.has_value());
        if (shape == RawMergeWriterResidueShapeV1::finalized_without_handoff) {
            bool stopped = false;
            auto interrupted =
                authority::trusted_test::publish_distributed_sieve_merge_prepared_v1_with_hooks(
                    std::move(*adopted.authority),
                    authority::trusted_test::DistributedSieveMergePreparedPublicationTestHooksV1{
                        .private_handoff_hooks = {},
                        .stop_after_payload_build_before_handoff =
                            stop_after_payload_build_before_handoff,
                        .payload_build_context = &stopped,
                    });
            CHECK(stopped);
            CHECK(!interrupted);
            CHECK(!interrupted.admission.has_value());
            CHECK(interrupted.diagnostic.phase ==
                  authority::DistributedSieveMergeWriterAuthorityPhaseV1::payload_build);
            CHECK(interrupted.diagnostic.status ==
                  authority::DistributedSieveMergeWriterAuthorityStatusV1::payload_build_failed);
            CHECK(interrupted.diagnostic.reconciliation_required);
            CHECK(!adopted.authority->valid());
        } else {
            adopted.authority.reset();
        }
    }

    return {
        .manifest_digest = manifest_digest,
        .started = started,
        .names = *names,
        .paths = paths,
        .persisted_relation_count = raw_merge_writer_persisted_relation_count(shape),
        .finalized = shape == RawMergeWriterResidueShapeV1::finalized_without_handoff,
    };
}

class RawMergeWriterResidueFixtureV1 final {
public:
    explicit RawMergeWriterResidueFixtureV1(RawMergeWriterResidueShapeV1 shape)
        : source_(raw_merge_writer_residue_label(shape), raw_merge_writer_worker_rows(shape)),
          shape_(shape), state_(materialize_raw_merge_writer_residue(source_, shape)) {}

    RawMergeWriterResidueFixtureV1(const RawMergeWriterResidueFixtureV1&) = delete;
    RawMergeWriterResidueFixtureV1& operator=(const RawMergeWriterResidueFixtureV1&) = delete;

    [[nodiscard]] RawMergeWriterResidueShapeV1 shape() const noexcept {
        return shape_;
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return source_.root();
    }

    [[nodiscard]] const Digest& manifest_digest() const noexcept {
        return state_.manifest_digest;
    }

    [[nodiscard]] const sieve::MergeStartedV1& started() const noexcept {
        return state_.started;
    }

    [[nodiscard]] const wave::DistributedSieveMergeGenerationNamesV1& names() const noexcept {
        return state_.names;
    }

    [[nodiscard]] const relation::OOCCleanupPaths& paths() const noexcept {
        return state_.paths;
    }

    [[nodiscard]] std::uint64_t persisted_relation_count() const noexcept {
        return state_.persisted_relation_count;
    }

    [[nodiscard]] bool finalized() const noexcept {
        return state_.finalized;
    }

private:
    MergeAuthorityFixture source_;
    RawMergeWriterResidueShapeV1 shape_;
    RawMergeWriterResidueStateV1 state_;
};

void require_raw_merge_writer_residue_shape(const RawMergeWriterResidueFixtureV1& fixture) {
    const auto& paths = fixture.paths();
    CHECK(fixture.manifest_digest() != Digest{});
    CHECK(fixture.started().merge_attempt_ordinal == 0U);
    CHECK(std::filesystem::exists(fixture.root() / fixture.names().base_lock_leaf));
    CHECK(std::filesystem::exists(fixture.root() / fixture.names().canonical_record_leaf));
    CHECK(!std::filesystem::exists(fixture.root() / fixture.names().pending_record_leaf));
    CHECK(std::filesystem::exists(paths.lease_reserved_path));
    CHECK(std::filesystem::exists(paths.lease_owned_path));
    CHECK(std::filesystem::exists(paths.private_directory));
    CHECK(std::filesystem::exists(paths.index_path));
    CHECK(std::filesystem::exists(paths.data_path));
    CHECK(!std::filesystem::exists(paths.private_handoff_path));
    CHECK(!std::filesystem::exists(paths.private_handoff_pending_path));
    CHECK(!std::filesystem::exists(paths.private_handoff_rollback_path));

    const std::uint64_t expected_magic = fixture.finalized()
                                             ? relation::OOCRelationWriter::MAGIC_V3_FINAL
                                             : relation::OOCRelationWriter::MAGIC_V3_INCOMPLETE;
    CHECK(read_file_u64(paths.index_path, 0) == expected_magic);
    const std::uint64_t expected_header_count =
        fixture.finalized() ? fixture.persisted_relation_count() : 0U;
    CHECK(read_file_u64(paths.index_path, relation::OOCRelationWriter::INDEX_COUNT_OFFSET) ==
          expected_header_count);

    const std::uint64_t expected_index_extent =
        relation::OOCRelationWriter::INDEX_HEADER_BYTES +
        fixture.persisted_relation_count() * sizeof(std::uint64_t) +
        (fixture.finalized() ? relation::OOCRelationWriter::INDEX_SENTINEL_BYTES : 0U);
    CHECK(file_extent(paths.index_path) == expected_index_extent);
    if (fixture.persisted_relation_count() == 0U) {
        CHECK(file_extent(paths.data_path) == relation::OOCRelationWriter::DATA_HEADER_BYTES);
    } else {
        CHECK(file_extent(paths.data_path) > relation::OOCRelationWriter::DATA_HEADER_BYTES);
    }
}

void require_raw_merge_writer_recovered_to_p0(const RawMergeWriterResidueFixtureV1& fixture) {
    const auto& paths = fixture.paths();
    CHECK(std::filesystem::exists(fixture.root() / fixture.names().base_lock_leaf));
    CHECK(std::filesystem::exists(fixture.root() / fixture.names().canonical_record_leaf));
    CHECK(!std::filesystem::exists(fixture.root() / fixture.names().pending_record_leaf));
    CHECK(!std::filesystem::exists(fixture.root() / fixture.names().reserved_leaf));
    CHECK(!std::filesystem::exists(fixture.root() / fixture.names().reserved_pending_leaf));
    CHECK(!std::filesystem::exists(fixture.root() / fixture.names().owned_leaf));
    CHECK(!std::filesystem::exists(fixture.root() / fixture.names().owned_pending_leaf));
    CHECK(!std::filesystem::exists(fixture.root() / fixture.names().rollback_handoff_leaf));
    CHECK(!std::filesystem::exists(paths.private_directory));
    CHECK(!std::filesystem::exists(paths.index_path));
    CHECK(!std::filesystem::exists(paths.data_path));
    CHECK(!std::filesystem::exists(paths.private_handoff_path));
    CHECK(!std::filesystem::exists(paths.private_handoff_pending_path));
    CHECK(!std::filesystem::exists(paths.private_handoff_rollback_path));
}

void require_no_next_merge_generation(const RawMergeWriterResidueFixtureV1& fixture) {
    const auto next_names = wave::distributed_sieve_merge_generation_names_v1(
        fixture.started().merge_attempt_ordinal + 1U);
    CHECK(next_names.has_value());
    CHECK(!std::filesystem::exists(fixture.root() / next_names->base_lock_leaf));
    CHECK(!std::filesystem::exists(fixture.root() / next_names->private_directory_leaf));
    CHECK(!std::filesystem::exists(fixture.root() / next_names->canonical_record_leaf));
    CHECK(!std::filesystem::exists(fixture.root() / next_names->pending_record_leaf));
}

struct RawMergeWriterStableFactsV1 final {
    std::vector<std::byte> canonical_record_bytes;
    sieve::NativeIdentityV1 canonical_record_identity;
};

[[nodiscard]] RawMergeWriterStableFactsV1
capture_raw_merge_writer_stable_facts(const RawMergeWriterResidueFixtureV1& fixture) {
    const auto canonical_path = fixture.root() / fixture.names().canonical_record_leaf;
    auto canonical_record_bytes = read_file_bytes(canonical_path);
    CHECK(canonical_record_bytes == encode_record(Record{fixture.started()}));
    return {
        .canonical_record_bytes = std::move(canonical_record_bytes),
        .canonical_record_identity = native_identity(canonical_path),
    };
}

void require_canonical_merge_started_unchanged(const RawMergeWriterResidueFixtureV1& fixture,
                                               const RawMergeWriterStableFactsV1& expected) {
    const auto canonical_path = fixture.root() / fixture.names().canonical_record_leaf;
    CHECK(read_file_bytes(canonical_path) == expected.canonical_record_bytes);
    CHECK(native_identity(canonical_path) == expected.canonical_record_identity);
}

void require_worker_inputs_observable(const RawMergeWriterResidueFixtureV1& fixture,
                                      const wave::DistributedSieveWaveStore& store) {
    const auto observed = store.observe_worker_chunks_v1();
    if (!observed) {
        fail("observe raw-recovery worker inputs", __LINE__,
             wave_diagnostic_detail(observed.diagnostic));
    }
    const auto& inputs = fixture.started().ordered_inputs;
    CHECK(observed.chunks.size() == inputs.size());
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const auto& input = inputs[index];
        const auto& chunk = observed.chunks[index];
        CHECK(chunk.chunk.chunk_id == input.chunk_id);
        CHECK(chunk.chunk.sq_begin == input.sq_begin);
        CHECK(chunk.chunk.sq_end == input.sq_end);
        CHECK(!chunk.terminal_failure.has_value());

        if (input.disposition == sieve::ChunkDispositionV1::empty) {
            CHECK(chunk.state == wave::DistributedSieveWorkerChunkDurableStateV1::empty);
            CHECK(!chunk.latest_attempt.has_value());
            CHECK(!chunk.handoff.has_value());
            continue;
        }

        CHECK(input.disposition == sieve::ChunkDispositionV1::handoff);
        CHECK(chunk.state == wave::DistributedSieveWorkerChunkDurableStateV1::handoff);
        CHECK(chunk.latest_attempt.has_value());
        CHECK(chunk.handoff.has_value());
        const auto& attempt = *chunk.latest_attempt;
        const auto& handoff = *chunk.handoff;
        CHECK(input.durable_attempt_count > 0U);
        CHECK(attempt.manifest_digest == fixture.manifest_digest());
        CHECK(attempt.chunk_id == input.chunk_id);
        CHECK(attempt.sq_begin == input.sq_begin);
        CHECK(attempt.sq_end == input.sq_end);
        CHECK(attempt.attempt_ordinal == input.durable_attempt_count - 1U);
        CHECK(attempt.self_digest == input.last_attempt_digest);
        CHECK(attempt.lease.lease_id == input.lease_id);
        CHECK(handoff.manifest_digest == fixture.manifest_digest());
        CHECK(handoff.work_digest == fixture.started().work_digest);
        CHECK(handoff.chunk_id == input.chunk_id);
        CHECK(handoff.sq_begin == input.sq_begin);
        CHECK(handoff.sq_end == input.sq_end);
        CHECK(handoff.attempt_ordinal == attempt.attempt_ordinal);
        CHECK(handoff.attempt_started_digest == attempt.self_digest);
        CHECK(handoff.lease.lease_id == input.lease_id);
        CHECK(handoff.next_sq_index == input.next_sq_index);
        CHECK(handoff.processed_sq_count == input.processed_sq_count);
        CHECK(handoff.completion_reason == input.completion_reason);
        CHECK(handoff.relation_count == input.raw_relation_count);
        CHECK(handoff.artifact.descriptor.relation_count == input.raw_relation_count);
        CHECK(handoff.artifact.sequence_receipt == input.sequence_receipt);
        CHECK(handoff.artifact.corpus_sha256 == input.corpus_sha256);
        CHECK(handoff.self_digest == input.handoff_digest);
        CHECK(handoff.cleanup_intent_absent);
    }
}

void require_raw_merge_writer_stable_facts(const RawMergeWriterResidueFixtureV1& fixture,
                                           const wave::DistributedSieveWaveStore& store,
                                           const RawMergeWriterStableFactsV1& expected) {
    require_canonical_merge_started_unchanged(fixture, expected);
    require_worker_inputs_observable(fixture, store);
    require_no_next_merge_generation(fixture);
}

void test_raw_merge_writer_residue_recovery_shapes() {
    constexpr std::array shapes{
        RawMergeWriterResidueShapeV1::empty_incomplete,
        RawMergeWriterResidueShapeV1::partial_incomplete,
        RawMergeWriterResidueShapeV1::complete_incomplete,
        RawMergeWriterResidueShapeV1::finalized_without_handoff,
    };
    for (const auto shape : shapes) {
        RawMergeWriterResidueFixtureV1 fixture(shape);
        CHECK(fixture.shape() == shape);
        require_raw_merge_writer_residue_shape(fixture);

        const auto stable_facts = capture_raw_merge_writer_stable_facts(fixture);
        const auto index_bytes = read_file_bytes(fixture.paths().index_path);
        const auto data_bytes = read_file_bytes(fixture.paths().data_path);
        const auto index_identity = native_identity(fixture.paths().index_path);
        const auto data_identity = native_identity(fixture.paths().data_path);

        auto reopened =
            wave::DistributedSieveWaveStore::open(fixture.root(), fixture.manifest_digest());
        CHECK(reopened);
        CHECK(reopened.store != nullptr);
        CHECK(!reopened.prepared_admission.has_value());
        require_wave_status(reopened.diagnostic, wave::DistributedSieveWaveStoreStatus::ready,
                            "cold open observes raw merge-writer residue");

        // Cold open is deliberately read-only.  It retains an exact observation
        // for the later prepare/reconcile authority path.
        require_raw_merge_writer_residue_shape(fixture);
        require_raw_merge_writer_stable_facts(fixture, *reopened.store, stable_facts);
        CHECK(read_file_bytes(fixture.paths().index_path) == index_bytes);
        CHECK(read_file_bytes(fixture.paths().data_path) == data_bytes);
        CHECK(native_identity(fixture.paths().index_path) == index_identity);
        CHECK(native_identity(fixture.paths().data_path) == data_identity);
        require_no_next_merge_generation(fixture);

        const auto cursor = wave::prepare_distributed_sieve_merge_generation_v1(*reopened.store);
        if (!cursor) {
            fail("prepare reconciles raw merge-writer residue", __LINE__,
                 wave_diagnostic_detail(cursor.diagnostic));
        }
        CHECK(*cursor.merge_attempt_ordinal == fixture.started().merge_attempt_ordinal + 1U);
        require_raw_merge_writer_recovered_to_p0(fixture);
        require_raw_merge_writer_stable_facts(fixture, *reopened.store, stable_facts);

        const auto repeated = wave::prepare_distributed_sieve_merge_generation_v1(*reopened.store);
        if (!repeated) {
            fail("repeated prepare remains idempotent after raw recovery", __LINE__,
                 wave_diagnostic_detail(repeated.diagnostic));
        }
        CHECK(repeated.merge_attempt_ordinal == cursor.merge_attempt_ordinal);
        require_raw_merge_writer_recovered_to_p0(fixture);
        require_raw_merge_writer_stable_facts(fixture, *reopened.store, stable_facts);

        reopened.store.reset();
        auto idempotent =
            wave::DistributedSieveWaveStore::open(fixture.root(), fixture.manifest_digest());
        CHECK(idempotent);
        CHECK(idempotent.store != nullptr);
        CHECK(!idempotent.prepared_admission.has_value());
        require_wave_status(idempotent.diagnostic, wave::DistributedSieveWaveStoreStatus::ready,
                            "reopen after raw merge-writer recovery");
        const auto reopened_cursor =
            wave::prepare_distributed_sieve_merge_generation_v1(*idempotent.store);
        if (!reopened_cursor) {
            fail("prepare after raw-recovery reopen remains idempotent", __LINE__,
                 wave_diagnostic_detail(reopened_cursor.diagnostic));
        }
        CHECK(reopened_cursor.merge_attempt_ordinal == cursor.merge_attempt_ordinal);
        require_raw_merge_writer_recovered_to_p0(fixture);
        require_raw_merge_writer_stable_facts(fixture, *idempotent.store, stable_facts);
    }
}

enum class RawRecoveryNamespaceEntryKindV1 : std::uint8_t {
    directory,
    regular_file,
};

struct RawRecoveryNamespaceEntrySnapshotV1 final {
    std::filesystem::path relative_path;
    RawRecoveryNamespaceEntryKindV1 kind = RawRecoveryNamespaceEntryKindV1::regular_file;
    sieve::NativeIdentityV1 identity;
    std::uint64_t extent = 0;
    std::uint64_t link_count = 0;
    std::uint32_t mode = 0;
    std::vector<std::byte> bytes;

    [[nodiscard]] friend bool operator==(const RawRecoveryNamespaceEntrySnapshotV1&,
                                         const RawRecoveryNamespaceEntrySnapshotV1&) = default;
};

struct RawRecoveryNamespaceSnapshotV1 final {
    sieve::NativeIdentityV1 root_identity;
    std::vector<RawRecoveryNamespaceEntrySnapshotV1> entries;

    [[nodiscard]] friend bool operator==(const RawRecoveryNamespaceSnapshotV1&,
                                         const RawRecoveryNamespaceSnapshotV1&) = default;
};

[[nodiscard]] RawRecoveryNamespaceSnapshotV1
capture_raw_recovery_namespace_snapshot(const RawMergeWriterResidueFixtureV1& fixture) {
    RawRecoveryNamespaceSnapshotV1 snapshot{
        .root_identity = native_identity(fixture.root()),
    };
    for (const auto& entry : std::filesystem::recursive_directory_iterator(fixture.root())) {
        struct stat metadata {};
        if (::lstat(entry.path().c_str(), &metadata) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "inspect raw-recovery namespace snapshot entry");
        }
        const bool directory = S_ISDIR(metadata.st_mode);
        const bool regular_file = S_ISREG(metadata.st_mode);
        if (!directory && !regular_file) {
            fail("raw-recovery namespace contains only directories and regular files", __LINE__);
        }
        if (regular_file && metadata.st_size < 0) {
            fail("raw-recovery namespace metadata is nonnegative", __LINE__);
        }
        auto relative_path = entry.path().lexically_relative(fixture.root());
        CHECK(!relative_path.empty());
        snapshot.entries.push_back({
            .relative_path = std::move(relative_path),
            .kind = directory ? RawRecoveryNamespaceEntryKindV1::directory
                              : RawRecoveryNamespaceEntryKindV1::regular_file,
            .identity = native_identity(entry.path()),
            .extent = regular_file ? static_cast<std::uint64_t>(metadata.st_size) : 0U,
            .link_count = static_cast<std::uint64_t>(metadata.st_nlink),
            .mode = static_cast<std::uint32_t>(metadata.st_mode),
            .bytes = regular_file ? read_file_bytes(entry.path()) : std::vector<std::byte>{},
        });
        if (regular_file) {
            CHECK(snapshot.entries.back().bytes.size() == snapshot.entries.back().extent);
        }
    }
    std::sort(snapshot.entries.begin(), snapshot.entries.end(),
              [](const RawRecoveryNamespaceEntrySnapshotV1& left,
                 const RawRecoveryNamespaceEntrySnapshotV1& right) {
                  return left.relative_path.generic_string() < right.relative_path.generic_string();
              });
    return snapshot;
}

void require_raw_recovery_fault_shape(
    const RawMergeWriterResidueFixtureV1& fixture,
    wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1 point) {
    const auto& paths = fixture.paths();
    const auto staging_directory =
        cleanup::private_lease_staging_path(paths, fixture.started().merged_lease.lease_id.limbs);
    const auto staging_owner = cleanup::private_lease_owner_path(staging_directory);
    const auto staging_owner_pending = cleanup::private_lease_owner_pending_path(staging_directory);
    const auto staging_index = staging_directory / paths.index_path.filename();
    const auto staging_data = staging_directory / paths.data_path.filename();
    const auto staging_handoff = staging_directory / paths.private_handoff_path.filename();
    const auto staging_handoff_pending =
        staging_directory / paths.private_handoff_pending_path.filename();
    const auto staging_quarantine_index =
        staging_directory / paths.quarantine_index_path.filename();
    const auto staging_quarantine_data = staging_directory / paths.quarantine_data_path.filename();

    bool final_directory_present = false;
    bool staging_directory_present = false;
    bool owner_present = false;
    bool index_present = false;
    bool data_present = false;
    bool reserved_present = false;
    bool owned_present = false;
    switch (point) {
    case wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::RecoveryPermitAcquired:
        final_directory_present = true;
        owner_present = true;
        index_present = true;
        data_present = true;
        reserved_present = true;
        owned_present = true;
        break;
    case wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::
        PreactiveDirectoryQuarantinedDurable:
        staging_directory_present = true;
        owner_present = true;
        index_present = true;
        data_present = true;
        reserved_present = true;
        owned_present = true;
        break;
    case wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::PreactiveDataRemovedDurable:
        staging_directory_present = true;
        owner_present = true;
        index_present = true;
        reserved_present = true;
        owned_present = true;
        break;
    case wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::PreactiveIndexRemovedDurable:
        staging_directory_present = true;
        owner_present = true;
        reserved_present = true;
        owned_present = true;
        break;
    case wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::OwnerRemovedDurable:
        staging_directory_present = true;
        reserved_present = true;
        owned_present = true;
        break;
    case wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::FinalDirectoryRemovedDurable:
        reserved_present = true;
        owned_present = true;
        break;
    case wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::ReservedRemovedDurable:
        owned_present = true;
        break;
    case wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::OwnedRemovedDurable:
        break;
    case wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::Count:
        fail("raw-recovery fault point is concrete", __LINE__);
    }

    CHECK(std::filesystem::is_directory(paths.private_directory) == final_directory_present);
    CHECK(std::filesystem::is_directory(staging_directory) == staging_directory_present);
    CHECK(std::filesystem::exists(paths.lease_reserved_path) == reserved_present);
    CHECK(std::filesystem::exists(paths.lease_owned_path) == owned_present);
    CHECK(!std::filesystem::exists(paths.lease_reserved_pending_path));
    CHECK(!std::filesystem::exists(paths.lease_owned_pending_path));
    CHECK(std::filesystem::exists(fixture.root() / fixture.names().base_lock_leaf));
    CHECK(std::filesystem::exists(fixture.root() / fixture.names().canonical_record_leaf));
    CHECK(!std::filesystem::exists(fixture.root() / fixture.names().pending_record_leaf));
    CHECK(!std::filesystem::exists(fixture.root() / fixture.names().rollback_handoff_leaf));

    const auto active_directory =
        final_directory_present ? paths.private_directory : staging_directory;
    const auto active_owner = cleanup::private_lease_owner_path(active_directory);
    const auto active_owner_pending = cleanup::private_lease_owner_pending_path(active_directory);
    const auto active_index = active_directory / paths.index_path.filename();
    const auto active_data = active_directory / paths.data_path.filename();
    CHECK(std::filesystem::exists(active_owner) == owner_present);
    CHECK(!std::filesystem::exists(active_owner_pending));
    CHECK(std::filesystem::exists(active_index) == index_present);
    CHECK(std::filesystem::exists(active_data) == data_present);

    CHECK(!std::filesystem::exists(paths.private_handoff_path));
    CHECK(!std::filesystem::exists(paths.private_handoff_pending_path));
    CHECK(!std::filesystem::exists(paths.private_handoff_rollback_path));
    CHECK(!std::filesystem::exists(paths.quarantine_index_path));
    CHECK(!std::filesystem::exists(paths.quarantine_data_path));
    CHECK(!std::filesystem::exists(staging_owner_pending));
    CHECK(!std::filesystem::exists(staging_handoff));
    CHECK(!std::filesystem::exists(staging_handoff_pending));
    CHECK(!std::filesystem::exists(staging_quarantine_index));
    CHECK(!std::filesystem::exists(staging_quarantine_data));
    if (!staging_directory_present) {
        CHECK(!std::filesystem::exists(staging_owner));
        CHECK(!std::filesystem::exists(staging_index));
        CHECK(!std::filesystem::exists(staging_data));
    }
    require_no_next_merge_generation(fixture);
}

inline constexpr std::array RAW_MERGE_WRITER_RECOVERY_FAULT_POINTS_V1{
    wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::RecoveryPermitAcquired,
    wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::PreactiveDirectoryQuarantinedDurable,
    wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::PreactiveDataRemovedDurable,
    wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::PreactiveIndexRemovedDurable,
    wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::OwnerRemovedDurable,
    wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::FinalDirectoryRemovedDurable,
    wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::ReservedRemovedDurable,
    wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::OwnedRemovedDurable,
};

static_assert(
    RAW_MERGE_WRITER_RECOVERY_FAULT_POINTS_V1.size() ==
    static_cast<std::size_t>(wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::Count));

struct RawMergeWriterRecoveryStopContextV1 final {
    wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1 target =
        wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1::Count;
    bool invoked = false;
};

[[nodiscard]] bool stop_after_raw_merge_writer_recovery_fault_point(
    wave::DistributedSieveMergeRawWriterRecoveryFaultPointV1 point, void* opaque) noexcept {
    auto& context = *static_cast<RawMergeWriterRecoveryStopContextV1*>(opaque);
    if (point != context.target) {
        return false;
    }
    context.invoked = true;
    return true;
}

void test_raw_merge_writer_recovery_fault_prefixes_retry() {
    for (const auto point : RAW_MERGE_WRITER_RECOVERY_FAULT_POINTS_V1) {
        RawMergeWriterResidueFixtureV1 fixture(RawMergeWriterResidueShapeV1::complete_incomplete);
        require_raw_merge_writer_residue_shape(fixture);
        const auto stable_facts = capture_raw_merge_writer_stable_facts(fixture);

        auto opened =
            wave::DistributedSieveWaveStore::open(fixture.root(), fixture.manifest_digest());
        CHECK(opened);
        CHECK(opened.store != nullptr);
        CHECK(!opened.prepared_admission.has_value());
        require_wave_status(opened.diagnostic, wave::DistributedSieveWaveStoreStatus::ready,
                            "open raw merge-writer fault-prefix fixture");
        require_raw_merge_writer_stable_facts(fixture, *opened.store, stable_facts);

        RawMergeWriterRecoveryStopContextV1 stop{.target = point};
        const auto interrupted = wave::reconcile_merge_started_generation_v1(
            *opened.store, fixture.started().merge_attempt_ordinal,
            wave::DistributedSieveMergeStartedReconcileTestHooksV1{
                .raw_recovery_stop_after = stop_after_raw_merge_writer_recovery_fault_point,
                .context = &stop,
            });
        CHECK(stop.invoked);
        CHECK(!interrupted);
        CHECK(!interrupted.reconciled.has_value());
        require_wave_status(interrupted.diagnostic,
                            wave::DistributedSieveWaveStoreStatus::interrupted,
                            "interrupt raw merge-writer recovery at durable fault point");
        CHECK(interrupted.diagnostic.last_merge_raw_writer_recovery_fault_point.has_value());
        CHECK(*interrupted.diagnostic.last_merge_raw_writer_recovery_fault_point == point);
        require_canonical_merge_started_unchanged(fixture, stable_facts);
        require_raw_recovery_fault_shape(fixture, point);
        const auto interrupted_namespace = capture_raw_recovery_namespace_snapshot(fixture);

        opened.store.reset();
        auto reopened =
            wave::DistributedSieveWaveStore::open(fixture.root(), fixture.manifest_digest());
        CHECK(reopened);
        CHECK(reopened.store != nullptr);
        CHECK(!reopened.prepared_admission.has_value());
        require_wave_status(reopened.diagnostic, wave::DistributedSieveWaveStoreStatus::ready,
                            "cold reopen interrupted raw merge-writer recovery prefix");
        require_raw_merge_writer_stable_facts(fixture, *reopened.store, stable_facts);
        require_raw_recovery_fault_shape(fixture, point);
        CHECK(capture_raw_recovery_namespace_snapshot(fixture) == interrupted_namespace);

        const auto cursor = wave::prepare_distributed_sieve_merge_generation_v1(*reopened.store);
        if (!cursor) {
            fail("retry raw merge-writer recovery prefix to P0", __LINE__,
                 wave_diagnostic_detail(cursor.diagnostic));
        }
        CHECK(cursor.merge_attempt_ordinal == fixture.started().merge_attempt_ordinal + 1U);
        require_raw_merge_writer_recovered_to_p0(fixture);
        require_raw_merge_writer_stable_facts(fixture, *reopened.store, stable_facts);
    }
}

struct SameByteRawLeafReplacementV1 final {
    std::vector<std::byte> bytes;
    sieve::NativeIdentityV1 original_identity;
    sieve::NativeIdentityV1 replacement_identity;
};

[[nodiscard]] SameByteRawLeafReplacementV1
replace_raw_leaf_with_same_bytes(const std::filesystem::path& path,
                                 const std::filesystem::path& displaced_path) {
    CHECK(!std::filesystem::exists(displaced_path));
    const auto bytes = read_file_bytes(path);
    const auto original_identity = native_identity(path);
    struct stat metadata {};
    CHECK(::lstat(path.c_str(), &metadata) == 0);
    CHECK(S_ISREG(metadata.st_mode));
    CHECK(::rename(path.c_str(), displaced_path.c_str()) == 0);

    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                            static_cast<mode_t>(metadata.st_mode & 0777));
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "create same-byte raw merge-writer replacement");
    }
    UniqueFd replacement(descriptor);
    int chmod_result = -1;
    do {
        chmod_result = ::fchmod(replacement.get(), static_cast<mode_t>(metadata.st_mode & 0777));
    } while (chmod_result < 0 && errno == EINTR);
    CHECK(chmod_result == 0);
    CHECK(write_exact_fd(replacement.get(), bytes.data(), bytes.size()));
    int sync_result = -1;
    do {
        sync_result = ::fsync(replacement.get());
    } while (sync_result < 0 && errno == EINTR);
    CHECK(sync_result == 0);

    const auto replacement_identity = native_identity(path);
    CHECK(replacement_identity != original_identity);
    CHECK(native_identity(displaced_path) == original_identity);
    CHECK(read_file_bytes(path) == bytes);
    CHECK(read_file_bytes(displaced_path) == bytes);
    return {
        .bytes = bytes,
        .original_identity = original_identity,
        .replacement_identity = replacement_identity,
    };
}

void test_raw_merge_writer_same_byte_replacement_fails_closed() {
    constexpr std::array replace_index_cases{true, false};
    for (const bool replace_index : replace_index_cases) {
        RawMergeWriterResidueFixtureV1 fixture(RawMergeWriterResidueShapeV1::complete_incomplete);
        require_raw_merge_writer_residue_shape(fixture);
        const auto stable_facts = capture_raw_merge_writer_stable_facts(fixture);
        const auto index_bytes = read_file_bytes(fixture.paths().index_path);
        const auto data_bytes = read_file_bytes(fixture.paths().data_path);
        const auto index_identity = native_identity(fixture.paths().index_path);
        const auto data_identity = native_identity(fixture.paths().data_path);

        auto opened =
            wave::DistributedSieveWaveStore::open(fixture.root(), fixture.manifest_digest());
        CHECK(opened);
        CHECK(opened.store != nullptr);
        CHECK(!opened.prepared_admission.has_value());
        require_raw_merge_writer_stable_facts(fixture, *opened.store, stable_facts);

        const auto& target_path =
            replace_index ? fixture.paths().index_path : fixture.paths().data_path;
        const auto& other_path =
            replace_index ? fixture.paths().data_path : fixture.paths().index_path;
        const auto displaced_path =
            fixture.root().parent_path() /
            (replace_index ? "raw-index-original.displaced" : "raw-data-original.displaced");
        const auto replaced = replace_raw_leaf_with_same_bytes(target_path, displaced_path);

        const auto cursor = wave::prepare_distributed_sieve_merge_generation_v1(*opened.store);
        CHECK(!cursor);
        CHECK(!cursor.merge_attempt_ordinal.has_value());
        require_wave_status(cursor.diagnostic,
                            wave::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "same-byte raw leaf replacement conflicts with observed identity");

        require_canonical_merge_started_unchanged(fixture, stable_facts);
        require_no_next_merge_generation(fixture);
        require_raw_merge_writer_residue_shape(fixture);
        CHECK(std::filesystem::exists(displaced_path));
        CHECK(native_identity(target_path) == replaced.replacement_identity);
        CHECK(native_identity(displaced_path) == replaced.original_identity);
        CHECK(read_file_bytes(target_path) == replaced.bytes);
        CHECK(read_file_bytes(displaced_path) == replaced.bytes);
        if (replace_index) {
            CHECK(replaced.bytes == index_bytes);
            CHECK(replaced.original_identity == index_identity);
            CHECK(native_identity(other_path) == data_identity);
            CHECK(read_file_bytes(other_path) == data_bytes);
        } else {
            CHECK(replaced.bytes == data_bytes);
            CHECK(replaced.original_identity == data_identity);
            CHECK(native_identity(other_path) == index_identity);
            CHECK(read_file_bytes(other_path) == index_bytes);
        }
    }
}

void test_raw_merge_writer_competing_cold_open_is_busy() {
    RawMergeWriterResidueFixtureV1 fixture(RawMergeWriterResidueShapeV1::complete_incomplete);
    require_raw_merge_writer_residue_shape(fixture);
    const auto stable_facts = capture_raw_merge_writer_stable_facts(fixture);
    const auto index_bytes = read_file_bytes(fixture.paths().index_path);
    const auto data_bytes = read_file_bytes(fixture.paths().data_path);
    const auto index_identity = native_identity(fixture.paths().index_path);
    const auto data_identity = native_identity(fixture.paths().data_path);

    auto baseline =
        wave::DistributedSieveWaveStore::open(fixture.root(), fixture.manifest_digest());
    CHECK(baseline);
    CHECK(baseline.store != nullptr);
    require_raw_merge_writer_stable_facts(fixture, *baseline.store, stable_facts);
    baseline.store.reset();

    std::array<int, 2> ready_pipe{-1, -1};
    std::array<int, 2> release_pipe{-1, -1};
    CHECK(::pipe(ready_pipe.data()) == 0);
    CHECK(::pipe(release_pipe.data()) == 0);
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)::close(ready_pipe[0]);
        (void)::close(release_pipe[1]);
        auto held =
            wave::DistributedSieveWaveStore::open(fixture.root(), fixture.manifest_digest());
        const char ready =
            held && held.store != nullptr && !held.prepared_admission.has_value() ? 'r' : 'f';
        const bool signalled = write_exact_fd(ready_pipe[1], &ready, sizeof(ready));
        char release = '\0';
        const bool released = read_exact_fd(release_pipe[0], &release, sizeof(release));
        held.store.reset();
        (void)::close(ready_pipe[1]);
        (void)::close(release_pipe[0]);
        ::_exit(signalled && released && ready == 'r' && release == 'x' ? EXIT_SUCCESS
                                                                        : EXIT_FAILURE);
    }

    (void)::close(ready_pipe[1]);
    (void)::close(release_pipe[0]);
    char ready = '\0';
    const bool received = read_exact_fd(ready_pipe[0], &ready, sizeof(ready));
    auto busy = wave::DistributedSieveWaveStore::open(fixture.root(), fixture.manifest_digest());
    const char release = 'x';
    const bool released = write_exact_fd(release_pipe[1], &release, sizeof(release));
    (void)::close(ready_pipe[0]);
    (void)::close(release_pipe[1]);
    int child_status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &child_status, 0);
    } while (waited < 0 && errno == EINTR);

    CHECK(received);
    CHECK(ready == 'r');
    CHECK(!busy);
    CHECK(busy.store == nullptr);
    CHECK(!busy.prepared_admission.has_value());
    require_wave_status(busy.diagnostic, wave::DistributedSieveWaveStoreStatus::lock_busy,
                        "competing process cannot cold-open raw merge-writer residue");
    CHECK(released);
    CHECK(waited == child);
    CHECK(WIFEXITED(child_status));
    CHECK(WEXITSTATUS(child_status) == EXIT_SUCCESS);
    require_canonical_merge_started_unchanged(fixture, stable_facts);
    require_no_next_merge_generation(fixture);
    require_raw_merge_writer_residue_shape(fixture);
    CHECK(read_file_bytes(fixture.paths().index_path) == index_bytes);
    CHECK(read_file_bytes(fixture.paths().data_path) == data_bytes);
    CHECK(native_identity(fixture.paths().index_path) == index_identity);
    CHECK(native_identity(fixture.paths().data_path) == data_identity);

    auto reopened =
        wave::DistributedSieveWaveStore::open(fixture.root(), fixture.manifest_digest());
    CHECK(reopened);
    CHECK(reopened.store != nullptr);
    require_raw_merge_writer_stable_facts(fixture, *reopened.store, stable_facts);
    const auto cursor = wave::prepare_distributed_sieve_merge_generation_v1(*reopened.store);
    if (!cursor) {
        fail("prepare raw merge-writer residue after competing process exits", __LINE__,
             wave_diagnostic_detail(cursor.diagnostic));
    }
    CHECK(cursor.merge_attempt_ordinal == fixture.started().merge_attempt_ordinal + 1U);
    require_raw_merge_writer_recovered_to_p0(fixture);
    require_raw_merge_writer_stable_facts(fixture, *reopened.store, stable_facts);
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

    struct stat metadata {};
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
    const bool reopened_store_branch = reopened.store != nullptr;
    const bool reopened_prepared_branch = reopened.prepared_admission.has_value();
    CHECK(reopened_store_branch != reopened_prepared_branch);
    require_wave_status(reopened.diagnostic, wave::DistributedSieveWaveStoreStatus::ready,
                        "cold open reconciles real typed MergePrepared prefix");
    if (pending_only) {
        CHECK(reopened.store != nullptr);
        CHECK(!reopened.prepared_admission.has_value());
    } else {
        CHECK(reopened.store == nullptr);
        CHECK(reopened.prepared_admission.has_value());
        CHECK(reopened.prepared_admission->valid());
        CHECK(encode_record(Record{reopened.prepared_admission->record()}) ==
              encode_record(Record{prepared}));
    }
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

    if (pending_only) {
        const auto cursor = wave::prepare_distributed_sieve_merge_generation_v1(*reopened.store);
        CHECK(cursor);
        CHECK(cursor.merge_attempt_ordinal == next_ordinal);
        require_wave_status(cursor.diagnostic, wave::DistributedSieveWaveStoreStatus::ready,
                            "rolled-back MergePrepared permits the next merge ordinal");
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
        test_raw_merge_writer_residue_recovery_shapes();
        test_raw_merge_writer_recovery_fault_prefixes_retry();
        test_raw_merge_writer_same_byte_replacement_fails_closed();
        test_raw_merge_writer_competing_cold_open_is_busy();
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
