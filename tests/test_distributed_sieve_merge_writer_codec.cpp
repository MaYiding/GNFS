#include "distributed_sieve_merge_writer_codec_internal.hpp"

#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/relation/relation_corpus_sha256.hpp>
#include <gnfs/relation/relation_sequence_receipt.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace codec = gnfs::sieve::distributed_sieve_merge_writer_codec_detail;
namespace relation = gnfs::relation;
namespace sieve = gnfs::sieve;

using BuildPhase = codec::DistributedSieveMergePreparedPayloadBuildPhaseV1;
using BuildStatus = codec::DistributedSieveMergePreparedPayloadBuildStatusV1;
using BuildResult = codec::DistributedSieveMergePreparedPayloadBuildResultV1;
using Digest = gnfs::util::Sha256Digest;
using Record = sieve::DistributedSieveProtocolRecordV1;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            throw std::runtime_error(std::string("CHECK failed: " #condition " at ") + __FILE__ +  \
                                     ":" + std::to_string(__LINE__));                              \
        }                                                                                          \
    } while (false)

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0};
        const auto tick =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::uint64_t attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("gnfs-merge-writer-codec-" + std::to_string(tick) + "-" +
                     std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + "-" +
                     std::to_string(attempt));
            std::error_code error;
            if (!std::filesystem::create_directory(path_, error)) {
                if (error == std::errc::file_exists) {
                    continue;
                }
                throw std::filesystem::filesystem_error("create merge-codec fixture root", path_,
                                                        error);
            }
#if !defined(_WIN32)
            if (::chmod(path_.c_str(), 0700) != 0) {
                const int native_error = errno;
                std::filesystem::remove_all(path_, error);
                throw std::system_error(native_error, std::generic_category(),
                                        "chmod merge-codec fixture root");
            }
#endif
            path_ = std::filesystem::canonical(path_, error);
            if (error) {
                std::error_code ignored;
                (void)std::filesystem::remove_all(path_, ignored);
                throw std::filesystem::filesystem_error("canonicalize merge-codec fixture root",
                                                        path_, error);
            }
            return;
        }
        throw std::runtime_error("could not reserve merge-codec fixture root");
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

[[nodiscard]] constexpr sieve::NativeIdentityV1 native_identity(std::uint64_t seed) noexcept {
    return {
        .volume = seed,
        .object = seed + 1U,
        .generation = seed + 2U,
    };
}

[[nodiscard]] constexpr sieve::LeaseIdV1 lease_id(std::uint64_t seed) noexcept {
    return {{seed + 1U, seed + 2U}};
}

[[nodiscard]] sieve::LeaseIdentityV1 lease_identity(std::uint64_t seed, std::string stem) {
    return {
        .lease_id = lease_id(seed),
        .owner_marker = native_identity(seed + 10U),
        .directory = native_identity(seed + 20U),
        .relative_stem = std::move(stem),
    };
}

template <typename Value> [[nodiscard]] Value seal_value(Value value) {
    Record record(std::move(value));
    const auto status = sieve::seal_distributed_sieve_record(record);
    CHECK(status);
    return std::get<Value>(std::move(record));
}

template <typename Value> [[nodiscard]] Value reseal(Value value) {
    value.self_digest = {};
    return seal_value(std::move(value));
}

struct NativeFileFacts final {
    std::array<std::uint64_t, 3> identity{};
    std::uint64_t extent = 0;
};

[[nodiscard]] NativeFileFacts capture_native_file_facts(const std::filesystem::path& path) {
    const auto extent = std::filesystem::file_size(path);
#if defined(_WIN32)
    const HANDLE handle = ::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                "open finalized corpus file");
    }
    BY_HANDLE_FILE_INFORMATION information{};
    FILE_ID_INFO file_id{};
    const bool inspected =
        ::GetFileInformationByHandle(handle, &information) != 0 &&
        ::GetFileInformationByHandleEx(handle, FileIdInfo, &file_id, sizeof(file_id)) != 0 &&
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
        information.nNumberOfLinks == 1;
    const DWORD inspect_error =
        inspected ? ERROR_SUCCESS
                  : (::GetLastError() == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : ::GetLastError());
    std::uint64_t low = 0;
    std::uint64_t high = 0;
    if (inspected) {
        static_assert(sizeof(file_id.FileId.Identifier) == sizeof(low) + sizeof(high));
        std::memcpy(&low, file_id.FileId.Identifier, sizeof(low));
        std::memcpy(&high, file_id.FileId.Identifier + sizeof(low), sizeof(high));
    }
    const DWORD close_error = ::CloseHandle(handle) ? ERROR_SUCCESS : ::GetLastError();
    if (!inspected) {
        throw std::system_error(static_cast<int>(inspect_error), std::system_category(),
                                "inspect finalized corpus file");
    }
    if (close_error != ERROR_SUCCESS) {
        throw std::system_error(static_cast<int>(close_error), std::system_category(),
                                "close finalized corpus file");
    }
    return {
        .identity =
            {
                static_cast<std::uint64_t>(file_id.VolumeSerialNumber),
                low,
                high,
            },
        .extent = extent,
    };
#else
    struct stat information {};
    if (::lstat(path.c_str(), &information) != 0) {
        throw std::system_error(errno, std::generic_category(), "inspect finalized corpus file");
    }
    CHECK(S_ISREG(information.st_mode));
    CHECK(information.st_nlink == 1);
    return {
        .identity =
            {
                static_cast<std::uint64_t>(information.st_dev),
                static_cast<std::uint64_t>(information.st_ino),
                0,
            },
        .extent = extent,
    };
#endif
}

[[nodiscard]] relation::OOCFinalizedCorpusEvidenceV1
make_real_empty_corpus_evidence(const std::filesystem::path& root) {
    const auto base = root / "merged_corpus";
    relation::OOCRelationWriter writer(base.string());
    const auto descriptor = writer.finalize();
    CHECK(descriptor.count == 0);
    CHECK(descriptor.format_version == relation::OOCRelationWriter::FORMAT_VERSION);

    {
        relation::OOCRelationReader reader(base.string(), descriptor);
        CHECK(reader.valid());
        CHECK(reader.count() == 0);
    }

    const auto index = capture_native_file_facts(base.string() + ".relidx");
    const auto data = capture_native_file_facts(base.string() + ".reldata");
    relation::RelationSequenceReceiptAccumulator sequence;
    relation::RelationCorpusSha256AccumulatorV1 corpus;
    const auto corpus_sha256 = corpus.finalize();
    CHECK(corpus_sha256.has_value());
    CHECK(corpus.count() == 0);

    relation::OOCFinalizedCorpusEvidenceV1 evidence{
        .descriptor = descriptor,
        .index_file =
            {
                .identity = index.identity,
                .extent = index.extent,
            },
        .data_file =
            {
                .identity = data.identity,
                .extent = data.extent,
            },
        .sequence_receipt = sequence.finish(),
        .corpus_sha256 = *corpus_sha256,
    };
    CHECK(evidence.index_file.extent == relation::OOCRelationWriter::index_size_for_count(0));
    CHECK(evidence.data_file.extent == relation::OOCRelationWriter::DATA_HEADER_BYTES);

    const auto cleanup = writer.remove_owned_artifacts_noexcept();
    CHECK(cleanup.completed());
    return evidence;
}

[[nodiscard]] sieve::TerminalChunkInputV1
terminal_input(std::uint32_t chunk_id, std::uint32_t sq_begin, std::uint32_t sq_end,
               std::uint64_t raw_relation_count, std::uint8_t seed) {
    return {
        .chunk_id = chunk_id,
        .disposition = sieve::ChunkDispositionV1::handoff,
        .sq_begin = sq_begin,
        .sq_end = sq_end,
        .next_sq_index = sq_end,
        .processed_sq_count = static_cast<std::uint64_t>(sq_end - sq_begin),
        .completion_reason = sieve::WorkerCompletionReasonV1::range_exhausted,
        .durable_attempt_count = 1,
        .last_attempt_digest = digest_with_seed(seed),
        .lease_id = lease_id(static_cast<std::uint64_t>(seed) + 100U),
        .handoff_digest = digest_with_seed(static_cast<std::uint8_t>(seed + 1U)),
        .raw_relation_count = raw_relation_count,
        .sequence_receipt =
            {
                .relation_count = raw_relation_count,
                .low = static_cast<std::uint64_t>(seed) + 200U,
                .high = static_cast<std::uint64_t>(seed) + 201U,
            },
        .corpus_sha256 = digest_with_seed(static_cast<std::uint8_t>(seed + 2U)),
    };
}

struct Fixture final {
    sieve::WaveManifestV1 manifest;
    std::vector<sieve::TerminalChunkInputV1> inputs;
    sieve::MergeStartedV1 started;

    Fixture() {
        manifest.wave_id = wave_id_with_seed(1);
        manifest.execution_contract_version = 1;
        manifest.executable_sha256 = digest_with_seed(2);
        manifest.work_sha256 = digest_with_seed(3);
        manifest.wave_root_identity = native_identity(UINT64_C(0xf000000000000000));
        manifest.permanent_lock_identity = native_identity(UINT64_C(0xe000000000000000));
        manifest.lock_semantics_version = 1;
        manifest.effective_sq_begin = 2;
        manifest.effective_sq_end = 5;
        manifest.worker_count = 2;
        manifest.chunks = {
            sieve::ChunkPlanV1{0, 2, 3, "chunk_0"},
            sieve::ChunkPlanV1{1, 3, 5, "chunk_1"},
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
        manifest = seal_value(std::move(manifest));

        inputs = {
            terminal_input(0, 2, 3, 1, 10),
            terminal_input(1, 3, 5, 2, 20),
        };
        started.manifest_digest = manifest.self_digest;
        started.work_digest = manifest.work_sha256;
        started.ordered_inputs = inputs;
        started.merge_policy_version = manifest.merge_policy_version;
        started.merged_lease = lease_identity(UINT64_C(0xd000000000000000), "merged_attempt_0");
        started.merge_attempt_ordinal = 0;
        started.predecessor_digest = manifest.self_digest;
        started = seal_value(std::move(started));
    }
};

[[nodiscard]] bool prepared_records_equal(const sieve::MergePreparedV1& left,
                                          const sieve::MergePreparedV1& right) noexcept {
    return left.manifest_digest == right.manifest_digest && left.work_digest == right.work_digest &&
           left.merge_policy_version == right.merge_policy_version &&
           left.merge_started_digest == right.merge_started_digest &&
           left.ordered_inputs == right.ordered_inputs &&
           left.input_relation_count == right.input_relation_count &&
           left.duplicate_relation_count == right.duplicate_relation_count &&
           left.output_relation_count == right.output_relation_count &&
           left.per_chunk_retained_counts == right.per_chunk_retained_counts &&
           left.merged_artifact == right.merged_artifact &&
           left.merged_lease == right.merged_lease && left.self_digest == right.self_digest;
}

[[nodiscard]] BuildResult
build(const Fixture& fixture, std::span<const sieve::MergeStartedV1> starts,
      std::uint64_t input_relation_count, std::uint64_t duplicate_relation_count,
      std::uint64_t output_relation_count, std::span<const sieve::PerChunkRetainedCountV1> retained,
      const relation::OOCFinalizedCorpusEvidenceV1& evidence) {
    return codec::build_distributed_sieve_merge_prepared_payload_v1(
        fixture.manifest, starts, input_relation_count, duplicate_relation_count,
        output_relation_count, retained, evidence);
}

void require_status(const BuildResult& result, BuildStatus status, BuildPhase phase) {
    CHECK(!result);
    CHECK(!result.prepared.has_value());
    CHECK(result.diagnostic.status == status);
    CHECK(result.diagnostic.phase == phase);
    CHECK(!result.diagnostic.protocol);
}

void test_valid_empty_corpus_round_trip_and_manifest_order(
    const Fixture& fixture, const relation::OOCFinalizedCorpusEvidenceV1& evidence) {
    const std::array starts{fixture.started};
    const std::array retained = {
        sieve::PerChunkRetainedCountV1{0, 0},
        sieve::PerChunkRetainedCountV1{1, 0},
    };
    const auto result = build(fixture, starts, 3, 3, 0, retained, evidence);
    CHECK(result);
    CHECK(result.prepared.has_value());
    CHECK(result.prepared->opaque_payload.size() <= 64U * 1024U);
    CHECK(result.prepared->record.input_relation_count == 3);
    CHECK(result.prepared->record.duplicate_relation_count == 3);
    CHECK(result.prepared->record.output_relation_count == 0);
    CHECK(result.prepared->record.per_chunk_retained_counts ==
          std::vector(retained.begin(), retained.end()));
    CHECK(result.prepared->record.merged_artifact.descriptor.relation_count == 0);

    const auto decoded = sieve::decode_distributed_sieve_record(result.prepared->opaque_payload);
    CHECK(decoded);
    CHECK(decoded.value.has_value());
    const auto* decoded_prepared = std::get_if<sieve::MergePreparedV1>(&*decoded.value);
    CHECK(decoded_prepared != nullptr);
    CHECK(prepared_records_equal(*decoded_prepared, result.prepared->record));
    CHECK(sieve::validate_distributed_sieve_record(*decoded.value, true));
    CHECK(sieve::validate_merge_predecessor_chain(fixture.manifest, starts, decoded_prepared,
                                                  nullptr));
}

void test_count_failures(const Fixture& fixture,
                         const relation::OOCFinalizedCorpusEvidenceV1& evidence) {
    const std::array starts{fixture.started};
    const std::array retained = {
        sieve::PerChunkRetainedCountV1{0, 0},
        sieve::PerChunkRetainedCountV1{1, 0},
    };
    const auto conservation_failure = build(fixture, starts, 3, 2, 2, retained, evidence);
    require_status(conservation_failure, BuildStatus::count_conservation_invalid,
                   BuildPhase::count_validation);

    auto overflow_started = fixture.started;
    overflow_started.ordered_inputs[0].raw_relation_count =
        std::numeric_limits<std::uint64_t>::max();
    overflow_started.ordered_inputs[0].sequence_receipt.relation_count =
        std::numeric_limits<std::uint64_t>::max();
    overflow_started.ordered_inputs[1].raw_relation_count = 1;
    overflow_started.ordered_inputs[1].sequence_receipt.relation_count = 1;
    overflow_started = reseal(std::move(overflow_started));
    const std::array overflow_starts{overflow_started};
    const auto overflow = build(fixture, overflow_starts, std::numeric_limits<std::uint64_t>::max(),
                                std::numeric_limits<std::uint64_t>::max(), 0, retained, evidence);
    require_status(overflow, BuildStatus::count_overflow, BuildPhase::count_validation);
}

void test_per_chunk_failures(const Fixture& fixture,
                             const relation::OOCFinalizedCorpusEvidenceV1& evidence) {
    const std::array starts{fixture.started};
    const std::array wrong_order = {
        sieve::PerChunkRetainedCountV1{1, 0},
        sieve::PerChunkRetainedCountV1{0, 0},
    };
    const auto order_failure = build(fixture, starts, 3, 3, 0, wrong_order, evidence);
    require_status(order_failure, BuildStatus::per_chunk_counts_invalid,
                   BuildPhase::count_validation);
    CHECK(order_failure.diagnostic.protocol.error ==
          sieve::DistributedSieveProtocolError::noncanonical_order);
    CHECK(order_failure.diagnostic.protocol.element_index == 0);

    const std::array retained_too_large = {
        sieve::PerChunkRetainedCountV1{0, 2},
        sieve::PerChunkRetainedCountV1{1, 0},
    };
    const auto retained_failure = build(fixture, starts, 3, 1, 2, retained_too_large, evidence);
    require_status(retained_failure, BuildStatus::per_chunk_counts_invalid,
                   BuildPhase::count_validation);
    CHECK(retained_failure.diagnostic.protocol.element_index == 0);
}

void test_evidence_failures(const Fixture& fixture,
                            const relation::OOCFinalizedCorpusEvidenceV1& evidence) {
    const std::array starts{fixture.started};
    const std::array retained = {
        sieve::PerChunkRetainedCountV1{0, 0},
        sieve::PerChunkRetainedCountV1{1, 0},
    };
    const auto require_evidence_rejected = [&](auto mutated) {
        const auto result = build(fixture, starts, 3, 3, 0, retained, mutated);
        require_status(result, BuildStatus::corpus_evidence_invalid,
                       BuildPhase::evidence_projection);
    };

    auto count_mismatch = evidence;
    count_mismatch.descriptor.count = 1;
    require_evidence_rejected(count_mismatch);

    auto format_mismatch = evidence;
    ++format_mismatch.descriptor.format_version;
    require_evidence_rejected(format_mismatch);

    auto index_extent_mismatch = evidence;
    ++index_extent_mismatch.index_file.extent;
    require_evidence_rejected(index_extent_mismatch);

    auto data_extent_mismatch = evidence;
    ++data_extent_mismatch.data_file.extent;
    require_evidence_rejected(data_extent_mismatch);

    auto native_identity_mismatch = evidence;
    native_identity_mismatch.index_file.identity = native_identity_mismatch.data_file.identity;
    require_evidence_rejected(native_identity_mismatch);
}

void test_merge_chain_failures(const Fixture& fixture,
                               const relation::OOCFinalizedCorpusEvidenceV1& evidence) {
    const std::array retained = {
        sieve::PerChunkRetainedCountV1{0, 0},
        sieve::PerChunkRetainedCountV1{1, 0},
    };
    const auto empty =
        build(fixture, std::span<const sieve::MergeStartedV1>{}, 3, 3, 0, retained, evidence);
    require_status(empty, BuildStatus::empty_merge_chain, BuildPhase::request_validation);

    auto bad_started = fixture.started;
    bad_started.predecessor_digest = digest_with_seed(90);
    bad_started = reseal(std::move(bad_started));
    const std::array bad_chain{bad_started};
    const auto wrong = build(fixture, bad_chain, 3, 3, 0, retained, evidence);
    require_status(wrong, BuildStatus::merge_chain_invalid, BuildPhase::merge_chain_validation);
}

} // namespace

int main() {
    try {
        TempDirectory temp;
        const auto evidence = make_real_empty_corpus_evidence(temp.path());
        const Fixture fixture;
        test_valid_empty_corpus_round_trip_and_manifest_order(fixture, evidence);
        test_count_failures(fixture, evidence);
        test_per_chunk_failures(fixture, evidence);
        test_evidence_failures(fixture, evidence);
        test_merge_chain_failures(fixture, evidence);
        std::cout << "distributed sieve merge writer codec tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
