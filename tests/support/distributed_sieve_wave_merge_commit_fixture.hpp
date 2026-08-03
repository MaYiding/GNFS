#pragma once

#if !defined(__APPLE__)
#error "The real WaveMergeCommit fixture requires the macOS private-handoff runtime"
#endif

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
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace gnfs::test::distributed_sieve_wave_merge_commit_fixture {

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
using PreparedAdmission = authority::DistributedSieveMergePreparedAdmissionV1;
using PreparedResult = authority::DistributedSieveMergePreparedResultV1;
using WriterAdoptionResult = authority::DistributedSieveMergeWriterAdoptionResultV1;

class FixtureFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] inline void fail(std::string_view expression, int line, std::string_view detail = {}) {
    std::string message = "fixture check failed at line " + std::to_string(line) + ": ";
    message.append(expression);
    if (!detail.empty()) {
        message.append(" (");
        message.append(detail);
        message.push_back(')');
    }
    throw FixtureFailure(std::move(message));
}

inline void check(bool condition, std::string_view expression, int line) {
    if (!condition) {
        fail(expression, line);
    }
}

#define GNFS_WAVE_COMMIT_FIXTURE_CHECK(expression)                                                 \
    ::gnfs::test::distributed_sieve_wave_merge_commit_fixture::check(                              \
        static_cast<bool>(expression), #expression, __LINE__)

[[nodiscard]] inline std::string
wave_diagnostic_detail(const wave::DistributedSieveWaveStoreDiagnostic& diagnostic) {
    std::string detail(wave::distributed_sieve_wave_store_status_name(diagnostic.status));
    if (diagnostic.native_error) {
        detail.append(": ");
        detail.append(diagnostic.native_error.message());
    }
    return detail;
}

[[nodiscard]] inline std::string authority_diagnostic_detail(
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

template <typename Value> [[nodiscard]] inline Value seal_value(Value value) {
    Record record(std::move(value));
    const auto status = sieve::seal_distributed_sieve_record(record);
    if (!status) {
        fail("seal distributed-sieve fixture record", __LINE__,
             sieve::distributed_sieve_protocol_error_name(status.error));
    }
    return std::get<Value>(std::move(record));
}

[[nodiscard]] inline std::vector<std::byte> encode_record(const Record& record) {
    const auto encoded = sieve::encode_distributed_sieve_record(record);
    if (!encoded || !encoded.bytes.has_value()) {
        fail("encode distributed-sieve fixture record", __LINE__,
             sieve::distributed_sieve_protocol_error_name(encoded.status.error));
    }
    return *encoded.bytes;
}

[[nodiscard]] inline Digest digest_with_seed(std::uint8_t seed) noexcept {
    Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        digest.bytes[index] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + index + 1U));
    }
    return digest;
}

[[nodiscard]] inline sieve::WaveIdV1 wave_id_with_seed(std::uint8_t seed) noexcept {
    sieve::WaveIdV1 wave_id;
    for (std::size_t index = 0; index < wave_id.bytes.size(); ++index) {
        wave_id.bytes[index] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + index + 1U));
    }
    return wave_id;
}

[[nodiscard]] inline Relation make_relation(std::int64_t a, std::uint64_t b, std::uint32_t seed) {
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

[[nodiscard]] inline bool relations_equal(const Relation& left, const Relation& right) noexcept {
    return left.a == right.a && left.b == right.b &&
           left.rational_factors == right.rational_factors &&
           left.algebraic_factors == right.algebraic_factors &&
           left.rational_large_prime == right.rational_large_prime &&
           left.algebraic_large_prime == right.algebraic_large_prime &&
           left.extra_ab_pairs == right.extra_ab_pairs;
}

[[nodiscard]] inline bool relation_vectors_equal(std::span<const Relation> left,
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
                    ("gnfs-wave-merge-commit-" + std::to_string(tick) + "-" +
                     std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + "-" +
                     std::to_string(attempt));
            std::error_code error;
            if (!std::filesystem::create_directory(path_, error)) {
                if (error == std::errc::file_exists) {
                    continue;
                }
                throw std::filesystem::filesystem_error("create WaveMergeCommit fixture root",
                                                        path_, error);
            }
            if (::chmod(path_.c_str(), 0700) != 0) {
                const int native_error = errno;
                std::filesystem::remove_all(path_, error);
                throw std::system_error(native_error, std::generic_category(),
                                        "chmod WaveMergeCommit fixture root");
            }
            path_ = std::filesystem::canonical(path_, error);
            if (error) {
                std::error_code ignored;
                (void)std::filesystem::remove_all(path_, ignored);
                throw std::filesystem::filesystem_error("canonicalize WaveMergeCommit fixture root",
                                                        path_, error);
            }
            return;
        }
        throw FixtureFailure("could not reserve WaveMergeCommit fixture root");
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

[[nodiscard]] inline sieve::NativeIdentityV1 native_identity(const std::filesystem::path& path) {
    struct stat metadata {};
    if (::lstat(path.c_str(), &metadata) != 0) {
        throw std::filesystem::filesystem_error("inspect WaveMergeCommit fixture entry", path,
                                                std::error_code(errno, std::generic_category()));
    }
    return {
        .volume = static_cast<std::uint64_t>(metadata.st_dev),
        .object = static_cast<std::uint64_t>(metadata.st_ino),
        .generation = 0,
    };
}

[[nodiscard]] inline std::uint64_t file_extent(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    GNFS_WAVE_COMMIT_FIXTURE_CHECK(size <= std::numeric_limits<std::uint64_t>::max());
    return static_cast<std::uint64_t>(size);
}

[[nodiscard]] inline std::vector<std::byte> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::filesystem::filesystem_error("open WaveMergeCommit fixture record", path,
                                                std::make_error_code(std::errc::io_error));
    }
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) {
        throw FixtureFailure("cannot size WaveMergeCommit fixture record");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) {
        throw FixtureFailure("cannot read WaveMergeCommit fixture record");
    }
    return bytes;
}

inline void publish_canonical_record(const std::filesystem::path& root,
                                     std::string_view pending_leaf, std::string_view canonical_leaf,
                                     std::span<const std::byte> bytes) {
    int descriptor = -1;
    do {
        descriptor = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "open WaveMergeCommit fixture wave root");
    }
    UniqueFd held_root(descriptor);
    const auto published = durable_record::publish_at(
        static_cast<durable_record::NativeHandle>(held_root.get()),
        std::filesystem::path{pending_leaf}, std::filesystem::path{canonical_leaf}, bytes);
    GNFS_WAVE_COMMIT_FIXTURE_CHECK(published.is_durable());
    GNFS_WAVE_COMMIT_FIXTURE_CHECK(published.canonical_snapshot().has_value());
}

enum class PreparedWaveChunkLayoutV1 : std::uint8_t {
    nonempty_nonempty_empty,
    nonempty_empty_nonempty_empty,
    nonempty_empty_nonempty_nonempty_empty,
};

[[nodiscard]] inline std::vector<std::pair<std::uint32_t, std::uint32_t>>
prepared_wave_chunk_ranges(PreparedWaveChunkLayoutV1 layout) {
    switch (layout) {
    case PreparedWaveChunkLayoutV1::nonempty_nonempty_empty:
        return {{2, 3}, {3, 4}, {4, 4}};
    case PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_empty:
        return {{2, 3}, {3, 3}, {3, 4}, {4, 4}};
    case PreparedWaveChunkLayoutV1::nonempty_empty_nonempty_nonempty_empty:
        return {{2, 3}, {3, 3}, {3, 4}, {4, 5}, {5, 5}};
    }
    throw FixtureFailure("unknown prepared-wave chunk layout");
}

[[nodiscard]] inline sieve::WaveManifestV1 manifest_draft(PreparedWaveChunkLayoutV1 layout) {
    sieve::WaveManifestV1 manifest;
    manifest.wave_id = wave_id_with_seed(1);
    manifest.execution_contract_version = 1;
    manifest.executable_sha256 = digest_with_seed(2);
    manifest.work_sha256 = digest_with_seed(3);
    const auto ranges = prepared_wave_chunk_ranges(layout);
    GNFS_WAVE_COMMIT_FIXTURE_CHECK(!ranges.empty());
    manifest.effective_sq_begin = ranges.front().first;
    manifest.effective_sq_end = ranges.back().second;
    manifest.worker_count = static_cast<std::uint32_t>(ranges.size());
    manifest.chunks.reserve(ranges.size());
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(
            index <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
        manifest.chunks.push_back(sieve::ChunkPlanV1{
            static_cast<std::uint32_t>(index),
            ranges[index].first,
            ranges[index].second,
            "commit_chunk_" + std::to_string(index),
        });
    }
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

[[nodiscard]] inline sieve::WaveManifestV1 manifest_draft() {
    return manifest_draft(PreparedWaveChunkLayoutV1::nonempty_nonempty_empty);
}

[[nodiscard]] inline std::vector<std::vector<Relation>>
worker_rows_for_layout(PreparedWaveChunkLayoutV1 layout) {
    const auto manifest = manifest_draft(layout);
    std::vector<std::vector<Relation>> rows(manifest.chunks.size());
    GNFS_WAVE_COMMIT_FIXTURE_CHECK(!manifest.chunks.empty());
    GNFS_WAVE_COMMIT_FIXTURE_CHECK(manifest.chunks.front().sq_begin <
                                   manifest.chunks.front().sq_end);
    rows.front() = {
        make_relation(11, 13, 10),
        make_relation(-41, 43, 40),
    };
    return rows;
}

[[nodiscard]] inline sieve::LeaseIdentityV1
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

[[nodiscard]] inline sieve::AttemptStartedV1 attempt_started(const sieve::WaveManifestV1& manifest,
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

[[nodiscard]] inline sieve::CorpusArtifactV1
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

[[nodiscard]] inline relation::OOCPrivateHandoffPairDescriptorV1
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

inline void publish_worker_handoff(wave::DistributedSieveWaveStore& store,
                                   const std::filesystem::path& root,
                                   const sieve::ChunkPlanV1& chunk,
                                   std::span<const Relation> rows) {
    auto created = store.create_worker_attempt_private_lease_root(chunk.chunk_id, 0);
    if (!created || created.claim == nullptr) {
        fail("create worker fixture P0", __LINE__, wave_diagnostic_detail(created.diagnostic));
    }
    created.claim.reset();

    const auto names = wave::distributed_sieve_worker_attempt_names_v1(chunk.relative_artifact_stem,
                                                                       chunk.chunk_id, 0);
    GNFS_WAVE_COMMIT_FIXTURE_CHECK(names.has_value());
    const auto base = root / names->private_directory_leaf / "corpus";
    const auto paths = relation::OOCCleanupTransaction::paths_for(base);
    auto reservation = relation::OOCCleanupTransaction::reserve_private_lease(base);
    GNFS_WAVE_COMMIT_FIXTURE_CHECK(reservation.completed());
    GNFS_WAVE_COMMIT_FIXTURE_CHECK(reservation.ownership.has_value());

    const auto attempt = attempt_started(store.manifest(), chunk, lease_identity(paths, *names));
    const auto attempt_bytes = encode_record(Record{attempt});
    publish_canonical_record(root, names->pending_record_leaf, names->canonical_record_leaf,
                             attempt_bytes);

    relation::OOCRelationWriter writer(
        base.string(), std::move(*reservation.ownership),
        relation::OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
    relation::RelationSequenceReceiptAccumulator sequence_accumulator;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(writer.write(rows[index]) == index);
        sequence_accumulator.append(rows[index]);
    }
    const auto descriptor = writer.finalize();
    auto pair_ownership = writer.take_cleanup_ownership_receipt();
    auto lease_ownership = writer.take_deferred_private_lease_ownership();
    const auto sequence = sequence_accumulator.finish();
    const auto corpus = relation::relation_corpus_sha256_v1(rows);
    GNFS_WAVE_COMMIT_FIXTURE_CHECK(corpus.has_value());
    GNFS_WAVE_COMMIT_FIXTURE_CHECK(descriptor.count == rows.size());
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
    GNFS_WAVE_COMMIT_FIXTURE_CHECK(published.canonical());
}

struct LeafSnapshot final {
    std::filesystem::path path;
    sieve::NativeIdentityV1 identity;
    std::vector<std::byte> bytes;

    [[nodiscard]] friend bool operator==(const LeafSnapshot&, const LeafSnapshot&) = default;
};

struct WorkerArtifactSnapshot final {
    sieve::NativeIdentityV1 private_directory_identity;
    std::optional<LeafSnapshot> reserved;
    std::array<LeafSnapshot, 6> leaves;

    [[nodiscard]] friend bool operator==(const WorkerArtifactSnapshot&,
                                         const WorkerArtifactSnapshot&) = default;
};

[[nodiscard]] inline LeafSnapshot snapshot_leaf(const std::filesystem::path& path) {
    return {
        .path = path,
        .identity = native_identity(path),
        .bytes = read_file_bytes(path),
    };
}

class PreparedWaveFixture final {
public:
    explicit PreparedWaveFixture(std::string_view label)
        : PreparedWaveFixture(label, PreparedWaveChunkLayoutV1::nonempty_nonempty_empty) {}

    PreparedWaveFixture(std::string_view label, PreparedWaveChunkLayoutV1 layout)
        : root_(temp_.path() / std::string(label)), worker_rows_(worker_rows_for_layout(layout)),
          opened_(wave::DistributedSieveWaveStore::create(root_, manifest_draft(layout))) {
        if (!opened_ || opened_.store == nullptr) {
            fail("create WaveMergeCommit WaveStore", __LINE__,
                 wave_diagnostic_detail(opened_.diagnostic));
        }
    }

    PreparedWaveFixture(const PreparedWaveFixture&) = delete;
    PreparedWaveFixture& operator=(const PreparedWaveFixture&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

    [[nodiscard]] Digest manifest_digest() const noexcept {
        return manifest_.has_value() ? manifest_->self_digest : opened_.store->manifest_digest();
    }

    [[nodiscard]] const sieve::WaveManifestV1& manifest() const {
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(manifest_.has_value());
        return *manifest_;
    }

    [[nodiscard]] const sieve::MergeStartedV1& merge_started() const {
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(merge_started_.has_value());
        return *merge_started_;
    }

    [[nodiscard]] std::span<const Relation> expected_rows() const noexcept {
        return worker_rows_[0];
    }

    [[nodiscard]] PreparedResult prepare_fresh() {
        auto worker_result = take_worker_result();
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(worker_result.store != nullptr);
        manifest_ = worker_result.store->manifest();

        auto merge_admission = coordinator::begin_or_resume_distributed_sieve_merge_generation_v1(
            std::move(worker_result));
        if (!merge_admission) {
            fail("admit complete worker result for merge", __LINE__,
                 wave_diagnostic_detail(merge_admission.diagnostic().wave_store));
        }
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(merge_admission.started_receipt() != nullptr);
        merge_started_ = merge_admission.started_receipt()->record();
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(merge_started_->ordered_inputs.size() ==
                                       worker_rows_.size());
        for (std::size_t index = 0; index < manifest_->chunks.size(); ++index) {
            const auto& chunk = manifest_->chunks[index];
            const auto& input = merge_started_->ordered_inputs[index];
            if (chunk.sq_begin == chunk.sq_end) {
                GNFS_WAVE_COMMIT_FIXTURE_CHECK(input.disposition ==
                                               sieve::ChunkDispositionV1::empty);
                continue;
            }
            GNFS_WAVE_COMMIT_FIXTURE_CHECK(input.disposition == sieve::ChunkDispositionV1::handoff);
            GNFS_WAVE_COMMIT_FIXTURE_CHECK(input.raw_relation_count == worker_rows_[index].size());
            GNFS_WAVE_COMMIT_FIXTURE_CHECK(
                input.completion_reason ==
                (worker_rows_[index].empty() ? sieve::WorkerCompletionReasonV1::zero_relations
                                             : sieve::WorkerCompletionReasonV1::range_exhausted));
        }

        auto adopted =
            authority::consume_distributed_sieve_merge_generation_v1(std::move(merge_admission));
        if (!adopted || !adopted.authority.has_value()) {
            fail("consume admitted merge generation", __LINE__,
                 authority_diagnostic_detail(adopted.diagnostic));
        }
        auto published =
            authority::publish_distributed_sieve_merge_prepared_v1(std::move(*adopted.authority));
        if (!published || !published.admission.has_value()) {
            fail("publish typed MergePrepared", __LINE__,
                 authority_diagnostic_detail(published.diagnostic));
        }
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(published.admission->valid());
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(published.admission->record().input_relation_count ==
                                       expected_rows().size());
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(published.admission->record().duplicate_relation_count ==
                                       0U);
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(published.admission->record().output_relation_count ==
                                       expected_rows().size());
        return published;
    }

    [[nodiscard]] WorkerArtifactSnapshot worker_snapshot(std::size_t index) const {
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(manifest_.has_value());
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(index < manifest_->chunks.size());
        const auto& chunk = manifest_->chunks[index];
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(chunk.sq_begin < chunk.sq_end);
        const auto names = wave::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, 0);
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(names.has_value());
        const auto base = root_ / names->private_directory_leaf / "corpus";
        const auto paths = relation::OOCCleanupTransaction::paths_for(base);
        const auto owner_path = cleanup::private_lease_owner_path(paths.private_directory);
        std::optional<LeafSnapshot> reserved;
        if (std::filesystem::exists(paths.lease_reserved_path)) {
            reserved.emplace(snapshot_leaf(paths.lease_reserved_path));
        }
        return {
            .private_directory_identity = native_identity(paths.private_directory),
            .reserved = std::move(reserved),
            .leaves =
                {
                    snapshot_leaf(paths.lock_path),
                    snapshot_leaf(paths.lease_owned_path),
                    snapshot_leaf(owner_path),
                    snapshot_leaf(paths.private_handoff_path),
                    snapshot_leaf(paths.index_path),
                    snapshot_leaf(paths.data_path),
                },
        };
    }

private:
    [[nodiscard]] CoordinatorResult take_worker_result() {
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(opened_.store != nullptr);
        auto& store = *opened_.store;
        std::vector<std::optional<wave::DistributedSieveAdoptedWorkerChunkV1>> adopted(
            store.manifest().chunks.size());
        for (std::size_t index = 0; index < adopted.size(); ++index) {
            const auto& chunk = store.manifest().chunks[index];
            if (chunk.sq_begin == chunk.sq_end) {
                continue;
            }
            publish_worker_handoff(store, root_, store.manifest().chunks[index],
                                   worker_rows_[index]);
        }
        for (std::size_t index = 0; index < adopted.size(); ++index) {
            const auto& chunk = store.manifest().chunks[index];
            if (chunk.sq_begin == chunk.sq_end) {
                continue;
            }
            auto result = store.adopt_worker_handoff_v1(store.manifest().chunks[index].chunk_id);
            if (!result || !result.adopted.has_value()) {
                fail("adopt real worker handoff", __LINE__,
                     wave_diagnostic_detail(result.diagnostic));
            }
            GNFS_WAVE_COMMIT_FIXTURE_CHECK(result.adopted->valid());
            GNFS_WAVE_COMMIT_FIXTURE_CHECK(result.adopted->reader().count() ==
                                           worker_rows_[index].size());
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
            if (!adopted[index].has_value()) {
                GNFS_WAVE_COMMIT_FIXTURE_CHECK(coordinated.chunk.sq_begin ==
                                               coordinated.chunk.sq_end);
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
        GNFS_WAVE_COMMIT_FIXTURE_CHECK(result);
        return result;
    }

    TempDirectory temp_;
    std::filesystem::path root_;
    std::vector<std::vector<Relation>> worker_rows_;
    wave::DistributedSieveWaveStoreOpenResult opened_;
    std::optional<sieve::WaveManifestV1> manifest_;
    std::optional<sieve::MergeStartedV1> merge_started_;
};

#undef GNFS_WAVE_COMMIT_FIXTURE_CHECK

} // namespace gnfs::test::distributed_sieve_wave_merge_commit_fixture
