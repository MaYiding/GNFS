#include "distributed_sieve_merge_writer_internal.hpp"

#include <gnfs/core/relation.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/relation/relation_corpus_sha256.hpp>
#include <gnfs/relation/relation_sequence_receipt.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#if !defined(_WIN32)
#include <cerrno>
#include <sys/stat.h>
#endif

namespace {

namespace merge = gnfs::sieve::distributed_sieve_merge_writer_detail;
namespace relation = gnfs::relation;
namespace sieve = gnfs::sieve;

using Digest = gnfs::util::Sha256Digest;
using MergePhase = merge::DistributedSieveMergeWriterPhaseV1;
using MergeResult = merge::DistributedSieveMergeWriterResultV1;
using MergeStatus = merge::DistributedSieveMergeWriterStatusV1;
using OOCReader = relation::OOCRelationReader;
using OOCWriter = relation::OOCRelationWriter;
using Relation = gnfs::core::Relation;
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
                    ("gnfs-merge-writer-" + std::to_string(tick) + "-" +
                     std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + "-" +
                     std::to_string(attempt));
            std::error_code error;
            if (!std::filesystem::create_directory(path_, error)) {
                if (error == std::errc::file_exists) {
                    continue;
                }
                throw std::filesystem::filesystem_error("create merge-writer fixture root", path_,
                                                        error);
            }
#if !defined(_WIN32)
            if (::chmod(path_.c_str(), 0700) != 0) {
                const int native_error = errno;
                std::filesystem::remove_all(path_, error);
                throw std::system_error(native_error, std::generic_category(),
                                        "chmod merge-writer fixture root");
            }
#endif
            path_ = std::filesystem::canonical(path_, error);
            if (error) {
                std::error_code ignored;
                (void)std::filesystem::remove_all(path_, ignored);
                throw std::filesystem::filesystem_error("canonicalize merge-writer fixture root",
                                                        path_, error);
            }
            return;
        }
        throw std::runtime_error("could not reserve merge-writer fixture root");
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

[[nodiscard]] constexpr std::uint64_t legacy_packed_key(const gnfs::core::ABPair& pair) noexcept {
    return static_cast<std::uint64_t>(pair.a) ^ (pair.b << 32U);
}

struct SemanticReceipts final {
    relation::RelationSequenceReceipt sequence;
    Digest corpus_sha256;
};

[[nodiscard]] SemanticReceipts semantic_receipts(std::span<const Relation> rows) {
    relation::RelationSequenceReceiptAccumulator sequence;
    relation::RelationCorpusSha256AccumulatorV1 corpus;
    for (const auto& row : rows) {
        sequence.append(row);
        CHECK(corpus.append(row));
    }
    const auto digest = corpus.finalize();
    CHECK(digest.has_value());
    CHECK(sequence.count() == rows.size());
    CHECK(corpus.count() == rows.size());
    return {
        .sequence = sequence.finish(),
        .corpus_sha256 = *digest,
    };
}

[[nodiscard]] sieve::RelationSequenceReceiptV1
protocol_sequence_receipt(const relation::RelationSequenceReceipt& receipt) noexcept {
    return {
        .relation_count = receipt.relation_count,
        .low = receipt.low,
        .high = receipt.high,
    };
}

struct InputCorpus final {
    std::filesystem::path base;
    relation::OOCSnapshotDescriptor descriptor;
    std::unique_ptr<OOCReader> reader;

    void open_reader() {
        CHECK(reader == nullptr);
        reader = std::make_unique<OOCReader>(base.string(), descriptor);
        CHECK(reader->valid());
    }
};

[[nodiscard]] InputCorpus make_input_corpus(const std::filesystem::path& root,
                                            std::string_view stem, std::span<const Relation> rows,
                                            bool open_reader = true) {
    InputCorpus corpus{
        .base = root / stem,
        .descriptor = {},
        .reader = nullptr,
    };
    {
        OOCWriter writer(corpus.base.string());
        for (std::size_t index = 0; index < rows.size(); ++index) {
            CHECK(writer.write(rows[index]) == index);
        }
        corpus.descriptor = writer.finalize();
    }
    CHECK(corpus.descriptor.count == rows.size());
    if (open_reader) {
        corpus.open_reader();
        CHECK(corpus.reader->count() == rows.size());
    }
    return corpus;
}

[[nodiscard]] sieve::TerminalChunkInputV1
handoff_input(std::uint32_t chunk_id, std::uint32_t sq_begin, std::uint32_t sq_end,
              std::span<const Relation> rows, std::uint8_t seed) {
    const auto receipts = semantic_receipts(rows);
    return {
        .chunk_id = chunk_id,
        .disposition = sieve::ChunkDispositionV1::handoff,
        .sq_begin = sq_begin,
        .sq_end = sq_end,
        .next_sq_index = sq_end,
        .processed_sq_count = 1,
        .completion_reason = rows.empty() ? sieve::WorkerCompletionReasonV1::zero_relations
                                          : sieve::WorkerCompletionReasonV1::range_exhausted,
        .durable_attempt_count = 1,
        .last_attempt_digest = digest_with_seed(seed),
        .lease_id = lease_id(static_cast<std::uint64_t>(seed) + 100U),
        .handoff_digest = digest_with_seed(static_cast<std::uint8_t>(seed + 1U)),
        .raw_relation_count = static_cast<std::uint64_t>(rows.size()),
        .sequence_receipt = protocol_sequence_receipt(receipts.sequence),
        .corpus_sha256 = receipts.corpus_sha256,
    };
}

[[nodiscard]] constexpr sieve::TerminalChunkInputV1 empty_input(std::uint32_t chunk_id,
                                                                std::uint32_t sq_index) noexcept {
    return {
        .chunk_id = chunk_id,
        .disposition = sieve::ChunkDispositionV1::empty,
        .sq_begin = sq_index,
        .sq_end = sq_index,
        .next_sq_index = sq_index,
        .processed_sq_count = 0,
        .completion_reason = sieve::WorkerCompletionReasonV1::zero_relations,
        .durable_attempt_count = 0,
        .last_attempt_digest = {},
        .lease_id = {},
        .handoff_digest = {},
        .raw_relation_count = 0,
        .sequence_receipt = {},
        .corpus_sha256 = {},
    };
}

struct ProtocolFixture final {
    sieve::WaveManifestV1 manifest;
    std::vector<sieve::TerminalChunkInputV1> inputs;
    sieve::MergeStartedV1 started;
};

[[nodiscard]] ProtocolFixture make_protocol_fixture(std::span<const Relation> chunk0,
                                                    std::span<const Relation> chunk1,
                                                    std::uint32_t merge_policy_version = 1) {
    ProtocolFixture fixture;
    fixture.manifest.wave_id = wave_id_with_seed(1);
    fixture.manifest.execution_contract_version = 1;
    fixture.manifest.executable_sha256 = digest_with_seed(2);
    fixture.manifest.work_sha256 = digest_with_seed(3);
    fixture.manifest.wave_root_identity = native_identity(UINT64_C(0xf000000000000000));
    fixture.manifest.permanent_lock_identity = native_identity(UINT64_C(0xe000000000000000));
    fixture.manifest.lock_semantics_version = 1;
    fixture.manifest.effective_sq_begin = 2;
    fixture.manifest.effective_sq_end = 4;
    fixture.manifest.worker_count = 3;
    fixture.manifest.chunks = {
        sieve::ChunkPlanV1{0, 2, 3, "chunk_0"},
        sieve::ChunkPlanV1{1, 3, 4, "chunk_1"},
        sieve::ChunkPlanV1{2, 4, 4, "chunk_2"},
    };
    fixture.manifest.sq_cap_per_worker = 10;
    fixture.manifest.relation_cap_per_worker = 100;
    fixture.manifest.max_worker_attempts = 2;
    fixture.manifest.max_merge_build_attempts = 2;
    fixture.manifest.max_consumption_attempts = 2;
    fixture.manifest.canonical_naming_version =
        sieve::DISTRIBUTED_SIEVE_CANONICAL_NAMING_VERSION_V1;
    fixture.manifest.retry_policy_version = 1;
    fixture.manifest.durable_start_consumes_ordinal = true;
    fixture.manifest.ooc_format_version = OOCWriter::FORMAT_VERSION;
    fixture.manifest.relation_serialization_version = 1;
    fixture.manifest.handoff_version = sieve::DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1;
    fixture.manifest.receipt_version = 1;
    fixture.manifest.digest_version = 1;
    fixture.manifest.merge_policy_version = merge_policy_version;
    fixture.manifest = seal_value(std::move(fixture.manifest));

    fixture.inputs = {
        handoff_input(0, 2, 3, chunk0, 10),
        handoff_input(1, 3, 4, chunk1, 20),
        empty_input(2, 4),
    };
    fixture.started.manifest_digest = fixture.manifest.self_digest;
    fixture.started.work_digest = fixture.manifest.work_sha256;
    fixture.started.ordered_inputs = fixture.inputs;
    fixture.started.merge_policy_version = fixture.manifest.merge_policy_version;
    fixture.started.merged_lease = lease_identity(UINT64_C(0xd000000000000000), "merged_attempt_0");
    fixture.started.merge_attempt_ordinal = 0;
    fixture.started.predecessor_digest = fixture.manifest.self_digest;
    fixture.started = seal_value(std::move(fixture.started));
    return fixture;
}

[[nodiscard]] MergeResult stream(const ProtocolFixture& fixture,
                                 std::span<const OOCReader* const> readers, OOCWriter& output) {
    const std::array starts{fixture.started};
    return merge::stream_distributed_sieve_merge_inputs_v1(fixture.manifest, starts, readers,
                                                           output);
}

void require_failure(const MergeResult& result, MergeStatus status, MergePhase phase,
                     bool artifacts_may_remain = false) {
    CHECK(!result);
    CHECK(!result.receipt.has_value());
    CHECK(result.diagnostic.status == status);
    CHECK(result.diagnostic.phase == phase);
    CHECK(result.artifacts_may_remain == artifacts_may_remain);
}

void require_success_receipt(const MergeResult& result, std::uint64_t input,
                             std::uint64_t duplicates, std::uint64_t output,
                             std::span<const sieve::PerChunkRetainedCountV1> retained,
                             bool artifacts_may_remain) {
    CHECK(result);
    CHECK(result.receipt.has_value());
    CHECK(result.diagnostic.status == MergeStatus::ready);
    CHECK(result.diagnostic.phase == MergePhase::complete);
    CHECK(result.diagnostic.protocol);
    CHECK(result.diagnostic.input_slot == merge::DISTRIBUTED_SIEVE_MERGE_WRITER_NO_INPUT_SLOT);
    CHECK(result.diagnostic.relation_ordinal ==
          merge::DISTRIBUTED_SIEVE_MERGE_WRITER_NO_RELATION_ORDINAL);
    CHECK(result.artifacts_may_remain == artifacts_may_remain);
    CHECK(result.receipt->input_relation_count == input);
    CHECK(result.receipt->duplicate_relation_count == duplicates);
    CHECK(result.receipt->output_relation_count == output);
    CHECK(result.receipt->per_chunk_retained_counts ==
          std::vector(retained.begin(), retained.end()));
}

struct FinalizedOutput final {
    merge::DistributedSieveMergeWriterReceiptV1 receipt;
    std::vector<Relation> rows;
    SemanticReceipts semantic;
};

[[nodiscard]] FinalizedOutput run_successful_merge(
    const std::filesystem::path& root, std::string_view stem, const ProtocolFixture& fixture,
    std::span<const OOCReader* const> readers, std::span<const Relation> expected,
    std::span<const sieve::PerChunkRetainedCountV1> retained, std::uint64_t duplicate_count) {
    OOCWriter output((root / stem).string());
    const auto result = stream(fixture, readers, output);
    require_success_receipt(result, expected.size() + duplicate_count, duplicate_count,
                            expected.size(), retained, !expected.empty());
    CHECK(output.state() == relation::OOCWriterState::Open);
    CHECK(output.count() == expected.size());

    const auto descriptor = output.finalize();
    CHECK(descriptor.count == expected.size());
    OOCReader reader((root / stem).string(), descriptor);
    CHECK(reader.valid());
    const auto rows = reader.read_all();
    CHECK(relation_vectors_equal(rows, expected));
    const auto semantic = semantic_receipts(rows);
    return {
        .receipt = *result.receipt,
        .rows = rows,
        .semantic = semantic,
    };
}

void test_manifest_order_first_ab_collision_receipts_and_determinism(
    const std::filesystem::path& root) {
    const Relation first_a = make_relation(11, 13, 10);
    const Relation duplicate_a = make_relation(11, 13, 90);
    const Relation collision_x = make_relation(0, 1, 20);
    const Relation collision_y =
        make_relation(static_cast<std::int64_t>(UINT64_C(3) << 32U), 2, 30);
    const Relation last_c = make_relation(-17, 19, 40);

    CHECK(first_a.ab() == duplicate_a.ab());
    CHECK(!relations_equal(first_a, duplicate_a));
    CHECK(collision_x.ab() != collision_y.ab());
    CHECK(legacy_packed_key(collision_x.ab()) == legacy_packed_key(collision_y.ab()));

    const std::array chunk0{first_a, collision_x};
    const std::array chunk1{duplicate_a, collision_y, last_c};
    const std::array expected{first_a, collision_x, collision_y, last_c};
    const std::array retained = {
        sieve::PerChunkRetainedCountV1{0, 2},
        sieve::PerChunkRetainedCountV1{1, 2},
        sieve::PerChunkRetainedCountV1{2, 0},
    };

    auto input0 = make_input_corpus(root, "main_input_0", chunk0);
    auto input1 = make_input_corpus(root, "main_input_1", chunk1);
    const auto fixture = make_protocol_fixture(chunk0, chunk1);
    const std::array<const OOCReader*, 3> readers{
        input0.reader.get(),
        input1.reader.get(),
        nullptr,
    };

    const auto first =
        run_successful_merge(root, "merged_first", fixture, readers, expected, retained, 1);
    const auto second =
        run_successful_merge(root, "merged_second", fixture, readers, expected, retained, 1);

    CHECK(relations_equal(first.rows.front(), first_a));
    CHECK(!relations_equal(first.rows.front(), duplicate_a));
    CHECK(relation_vectors_equal(first.rows, second.rows));
    CHECK(first.receipt.input_relation_count == second.receipt.input_relation_count);
    CHECK(first.receipt.duplicate_relation_count == second.receipt.duplicate_relation_count);
    CHECK(first.receipt.output_relation_count == second.receipt.output_relation_count);
    CHECK(first.receipt.per_chunk_retained_counts == second.receipt.per_chunk_retained_counts);
    CHECK(first.semantic.sequence == second.semantic.sequence);
    CHECK(first.semantic.corpus_sha256 == second.semantic.corpus_sha256);

    const auto expected_semantic = semantic_receipts(expected);
    CHECK(first.semantic.sequence == expected_semantic.sequence);
    CHECK(first.semantic.corpus_sha256 == expected_semantic.corpus_sha256);
}

void test_zero_row_handoffs_and_empty_reader_rule(const std::filesystem::path& root) {
    const std::vector<Relation> empty_rows;
    auto input0 = make_input_corpus(root, "zero_input_0", empty_rows);
    auto input1 = make_input_corpus(root, "zero_input_1", empty_rows);
    const auto fixture = make_protocol_fixture(empty_rows, empty_rows);
    const std::array<const OOCReader*, 3> readers{
        input0.reader.get(),
        input1.reader.get(),
        nullptr,
    };
    const std::array retained = {
        sieve::PerChunkRetainedCountV1{0, 0},
        sieve::PerChunkRetainedCountV1{1, 0},
        sieve::PerChunkRetainedCountV1{2, 0},
    };

    const auto completed = run_successful_merge(root, "merged_zero", fixture, readers,
                                                std::span<const Relation>{}, retained, 0);
    CHECK(completed.rows.empty());
    CHECK(completed.semantic.sequence == semantic_receipts(empty_rows).sequence);
    CHECK(completed.semantic.corpus_sha256 == semantic_receipts(empty_rows).corpus_sha256);

    {
        OOCWriter output((root / "empty_slot_has_reader").string());
        auto wrong = readers;
        wrong[2] = input0.reader.get();
        const auto result = stream(fixture, wrong, output);
        require_failure(result, MergeStatus::input_reader_invalid, MergePhase::input_validation);
        CHECK(result.diagnostic.input_slot == 2);
        CHECK(output.state() == relation::OOCWriterState::Open);
        CHECK(output.count() == 0);
    }
    {
        OOCWriter output((root / "handoff_slot_missing_reader").string());
        auto wrong = readers;
        wrong[0] = nullptr;
        const auto result = stream(fixture, wrong, output);
        require_failure(result, MergeStatus::input_reader_invalid, MergePhase::input_validation);
        CHECK(result.diagnostic.input_slot == 0);
        CHECK(output.state() == relation::OOCWriterState::Open);
        CHECK(output.count() == 0);
    }
}

void test_request_and_input_validation_failures(const std::filesystem::path& root) {
    const std::array chunk0{make_relation(11, 13, 10), make_relation(0, 1, 20)};
    const std::array chunk1{
        make_relation(11, 13, 90),
        make_relation(static_cast<std::int64_t>(UINT64_C(3) << 32U), 2, 30),
        make_relation(-17, 19, 40),
    };
    auto input0 = make_input_corpus(root, "validation_input_0", chunk0);
    auto input1 = make_input_corpus(root, "validation_input_1", chunk1);
    const auto fixture = make_protocol_fixture(chunk0, chunk1);
    const std::array<const OOCReader*, 3> readers{
        input0.reader.get(),
        input1.reader.get(),
        nullptr,
    };

    {
        OOCWriter output((root / "reader_span_size").string());
        const auto shortened =
            std::span<const OOCReader* const>(readers.data(), readers.size() - 1U);
        const auto result = stream(fixture, shortened, output);
        require_failure(result, MergeStatus::input_reader_invalid, MergePhase::input_validation);
        CHECK(output.state() == relation::OOCWriterState::Open);
        CHECK(output.count() == 0);
    }
    {
        OOCReader invalid_reader;
        auto invalid = readers;
        invalid[0] = &invalid_reader;
        OOCWriter output((root / "invalid_reader").string());
        const auto result = stream(fixture, invalid, output);
        require_failure(result, MergeStatus::input_reader_invalid, MergePhase::input_validation);
        CHECK(result.diagnostic.input_slot == 0);
        CHECK(output.count() == 0);
    }
    {
        const std::array wrong_rows{chunk0.front()};
        auto wrong_count = make_input_corpus(root, "wrong_count_input", wrong_rows);
        auto mismatched = readers;
        mismatched[0] = wrong_count.reader.get();
        OOCWriter output((root / "reader_count_mismatch").string());
        const auto result = stream(fixture, mismatched, output);
        require_failure(result, MergeStatus::input_corpus_count_mismatch,
                        MergePhase::input_validation);
        CHECK(result.diagnostic.input_slot == 0);
        CHECK(output.count() == 0);
    }
    {
        OOCWriter output((root / "nonempty_output").string());
        CHECK(output.write(make_relation(101, 103, 50)) == 0);
        const auto result = stream(fixture, readers, output);
        require_failure(result, MergeStatus::output_writer_invalid, MergePhase::request_validation,
                        true);
        CHECK(output.state() == relation::OOCWriterState::Open);
        CHECK(output.count() == 1);
        output.abort();
    }
    {
        OOCWriter output((root / "failed_output").string());
        output.abort();
        CHECK(output.state() == relation::OOCWriterState::Failed);
        CHECK(output.count() == 0);
        const auto result = stream(fixture, readers, output);
        require_failure(result, MergeStatus::output_writer_invalid, MergePhase::request_validation,
                        true);
        CHECK(output.state() == relation::OOCWriterState::Failed);
        CHECK(output.count() == 0);
    }
    {
        OOCWriter output((root / "empty_chain").string());
        const auto result = merge::stream_distributed_sieve_merge_inputs_v1(
            fixture.manifest, std::span<const sieve::MergeStartedV1>{}, readers, output);
        require_failure(result, MergeStatus::empty_merge_chain, MergePhase::request_validation);
        CHECK(output.count() == 0);
    }
    {
        auto invalid_started = fixture.started;
        invalid_started.predecessor_digest = digest_with_seed(99);
        invalid_started = reseal(std::move(invalid_started));
        const std::array invalid_chain{invalid_started};
        OOCWriter output((root / "invalid_chain").string());
        const auto result = merge::stream_distributed_sieve_merge_inputs_v1(
            fixture.manifest, invalid_chain, readers, output);
        require_failure(result, MergeStatus::merge_chain_invalid,
                        MergePhase::merge_chain_validation);
        CHECK(output.count() == 0);
    }
    {
        const auto unsupported = make_protocol_fixture(chunk0, chunk1, 2);
        OOCWriter output((root / "unsupported_policy").string());
        const auto result = stream(unsupported, readers, output);
        require_failure(result, MergeStatus::unsupported_merge_policy,
                        MergePhase::merge_chain_validation);
        CHECK(output.count() == 0);
    }

    const auto require_unsupported_semantic_version = [&](std::string_view output_stem,
                                                          auto mutate_manifest) {
        auto unsupported = fixture;
        mutate_manifest(unsupported.manifest);
        unsupported.manifest = reseal(std::move(unsupported.manifest));
        unsupported.started.manifest_digest = unsupported.manifest.self_digest;
        unsupported.started.predecessor_digest = unsupported.manifest.self_digest;
        unsupported.started = reseal(std::move(unsupported.started));

        OOCWriter output((root / output_stem).string());
        const auto result = stream(unsupported, readers, output);
        require_failure(result, MergeStatus::unsupported_semantic_version,
                        MergePhase::merge_chain_validation);
        CHECK(result.diagnostic.input_slot == merge::DISTRIBUTED_SIEVE_MERGE_WRITER_NO_INPUT_SLOT);
        CHECK(result.diagnostic.relation_ordinal ==
              merge::DISTRIBUTED_SIEVE_MERGE_WRITER_NO_RELATION_ORDINAL);
        CHECK(output.state() == relation::OOCWriterState::Open);
        CHECK(output.count() == 0);
    };

    require_unsupported_semantic_version(
        "unsupported_ooc_format",
        [](sieve::WaveManifestV1& manifest) { ++manifest.ooc_format_version; });
    require_unsupported_semantic_version(
        "unsupported_relation_serialization",
        [](sieve::WaveManifestV1& manifest) { ++manifest.relation_serialization_version; });
    require_unsupported_semantic_version(
        "unsupported_handoff_version",
        [](sieve::WaveManifestV1& manifest) { ++manifest.handoff_version; });
    require_unsupported_semantic_version(
        "unsupported_receipt_version",
        [](sieve::WaveManifestV1& manifest) { ++manifest.receipt_version; });
    require_unsupported_semantic_version(
        "unsupported_digest_version",
        [](sieve::WaveManifestV1& manifest) { ++manifest.digest_version; });
}

void require_receipt_mismatch(const MergeResult& result, std::size_t input_slot) {
    require_failure(result, MergeStatus::input_corpus_receipt_mismatch,
                    MergePhase::input_validation, true);
    CHECK(result.diagnostic.protocol.error ==
          sieve::DistributedSieveProtocolError::digest_mismatch);
    CHECK(result.diagnostic.protocol.element_index == input_slot);
    CHECK(result.diagnostic.input_slot == input_slot);
    CHECK(result.diagnostic.relation_ordinal ==
          merge::DISTRIBUTED_SIEVE_MERGE_WRITER_NO_RELATION_ORDINAL);
}

void test_input_receipt_binding_failures(const std::filesystem::path& root) {
    const std::array chunk0{
        make_relation(11, 13, 10),
        make_relation(17, 19, 20),
    };
    const std::array chunk1{
        make_relation(23, 29, 30),
        make_relation(31, 37, 40),
    };
    auto input0 = make_input_corpus(root, "receipt_input_0", chunk0);
    auto input1 = make_input_corpus(root, "receipt_input_1", chunk1);
    const auto fixture = make_protocol_fixture(chunk0, chunk1);
    const std::array<const OOCReader*, 3> readers{
        input0.reader.get(),
        input1.reader.get(),
        nullptr,
    };

    {
        const std::array<const OOCReader*, 3> swapped{
            input1.reader.get(),
            input0.reader.get(),
            nullptr,
        };
        OOCWriter output((root / "same_count_swapped_reader").string());
        const auto result = stream(fixture, swapped, output);
        require_receipt_mismatch(result, 0);
        CHECK(output.state() == relation::OOCWriterState::Open);
        CHECK(output.count() == chunk1.size());
        output.abort();
    }
    {
        auto tampered = fixture;
        tampered.started.ordered_inputs[0].corpus_sha256 = digest_with_seed(110);
        CHECK(tampered.started.ordered_inputs[0].corpus_sha256 !=
              fixture.started.ordered_inputs[0].corpus_sha256);
        tampered.started = reseal(std::move(tampered.started));
        OOCWriter output((root / "tampered_expected_corpus_digest").string());
        const auto result = stream(tampered, readers, output);
        require_receipt_mismatch(result, 0);
        CHECK(output.state() == relation::OOCWriterState::Open);
        CHECK(output.count() == chunk0.size());
        output.abort();
    }
    {
        auto tampered = fixture;
        tampered.started.ordered_inputs[0].sequence_receipt.low ^= UINT64_C(1);
        CHECK(tampered.started.ordered_inputs[0].sequence_receipt !=
              fixture.started.ordered_inputs[0].sequence_receipt);
        tampered.started = reseal(std::move(tampered.started));
        OOCWriter output((root / "tampered_expected_sequence_receipt").string());
        const auto result = stream(tampered, readers, output);
        require_receipt_mismatch(result, 0);
        CHECK(output.state() == relation::OOCWriterState::Open);
        CHECK(output.count() == chunk0.size());
        output.abort();
    }
}

[[nodiscard]] std::uint64_t read_u64(const std::filesystem::path& path, std::streamoff offset) {
    std::ifstream input(path, std::ios::binary);
    CHECK(static_cast<bool>(input));
    input.seekg(offset);
    std::uint64_t value = 0;
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    CHECK(static_cast<bool>(input));
    return value;
}

void overwrite_u32(const std::filesystem::path& path, std::streamoff offset, std::uint32_t value) {
    std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
    CHECK(static_cast<bool>(stream));
    stream.seekp(offset);
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    stream.flush();
    CHECK(static_cast<bool>(stream));
}

void test_input_read_failure_reports_written_residue(const std::filesystem::path& root) {
    const std::array chunk0{make_relation(11, 13, 10), make_relation(0, 1, 20)};
    const std::array chunk1{make_relation(-17, 19, 40)};
    auto input0 = make_input_corpus(root, "corrupt_input_0", chunk0, false);
    auto input1 = make_input_corpus(root, "corrupt_input_1", chunk1);
    const auto fixture = make_protocol_fixture(chunk0, chunk1);

    const auto index_path = input0.base.string() + ".relidx";
    const auto data_path = input0.base.string() + ".reldata";
    const auto second_offset =
        read_u64(index_path, static_cast<std::streamoff>(OOCWriter::INDEX_HEADER_BYTES +
                                                         sizeof(std::uint64_t)));
    overwrite_u32(
        data_path,
        static_cast<std::streamoff>(second_offset + sizeof(std::int64_t) + sizeof(std::uint64_t)),
        Relation::MAX_SERIALIZED_FACTORS + 1U);
    input0.open_reader();
    CHECK(input0.reader->count() == chunk0.size());

    const std::array<const OOCReader*, 3> readers{
        input0.reader.get(),
        input1.reader.get(),
        nullptr,
    };
    OOCWriter output((root / "input_read_failure_output").string());
    const auto result = stream(fixture, readers, output);
    require_failure(result, MergeStatus::input_read_failed, MergePhase::input_read, true);
    CHECK(result.diagnostic.input_slot == 0);
    CHECK(result.diagnostic.relation_ordinal == 1);
    CHECK(output.state() == relation::OOCWriterState::Open);
    CHECK(output.count() == 1);
    output.abort();
}

} // namespace

int main() {
    try {
        TempDirectory temp;
        test_manifest_order_first_ab_collision_receipts_and_determinism(temp.path());
        test_zero_row_handoffs_and_empty_reader_rule(temp.path());
        test_request_and_input_validation_failures(temp.path());
        test_input_receipt_binding_failures(temp.path());
        test_input_read_failure_reports_written_residue(temp.path());
        std::cout << "distributed sieve merge writer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
