#include "gnfs/core/relation.hpp"
#include "gnfs/relation/ooc_relation_store.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using gnfs::core::ABPair;
using gnfs::core::PrimePower;
using gnfs::core::Relation;
using gnfs::relation::OOCRecoveryOutcome;
using gnfs::relation::OOCRelationPrefixReader;
using gnfs::relation::OOCRelationReader;
using gnfs::relation::OOCRelationWriter;
using gnfs::relation::OOCSnapshotDescriptor;
using gnfs::relation::OOCWriterState;

[[noreturn]] static void check_failed(const char* expression, int line) {
    throw std::runtime_error(std::string("CHECK failed at line ") + std::to_string(line) + ": " +
                             expression);
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition))                                                                          \
            check_failed(#condition, __LINE__);                                                    \
    } while (false)

namespace {

class OOCArtifacts {
public:
    explicit OOCArtifacts(std::string base_path) : base_path_(std::move(base_path)) {}

    ~OOCArtifacts() {
        std::remove((base_path_ + ".reldata").c_str());
        std::remove((base_path_ + ".relidx").c_str());
    }

private:
    std::string base_path_;
};

std::string make_path(const std::string& suffix) {
    return gnfs::util::temp_path("gnfs_ooc_integrity_" + std::to_string(gnfs::util::process_id()) +
                                 "_" + suffix);
}

Relation make_relation(int64_t a, uint64_t b) {
    Relation relation(a, b);
    relation.rational_factors.push_back(static_cast<uint32_t>(100 + static_cast<uint64_t>(a)));
    return relation;
}

Relation make_large_prime_relation(size_t count) {
    Relation relation(1, 2);
    relation.rational_large_prime.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        relation.rational_large_prime.push_back(PrimePower{1009 + static_cast<uint64_t>(i), 0, 1});
    }
    return relation;
}

void overwrite_u64(const std::string& path, std::streamoff offset, uint64_t value) {
    std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
    CHECK(static_cast<bool>(stream));
    stream.seekp(offset);
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    stream.flush();
    CHECK(static_cast<bool>(stream));
}

void overwrite_u32(const std::string& path, std::streamoff offset, uint32_t value) {
    std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
    CHECK(static_cast<bool>(stream));
    stream.seekp(offset);
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    stream.flush();
    CHECK(static_cast<bool>(stream));
}

uint64_t read_u64_at(const std::string& path, std::streamoff offset) {
    std::ifstream stream(path, std::ios::binary);
    CHECK(static_cast<bool>(stream));
    stream.seekg(offset);
    uint64_t value = 0;
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    CHECK(static_cast<bool>(stream));
    return value;
}

OOCSnapshotDescriptor create_finalized_store(const std::string& base_path) {
    OOCRelationWriter writer(base_path);
    CHECK(writer.write(make_relation(1, 2)) == 0);
    CHECK(writer.write(make_relation(3, 4)) == 1);
    const auto descriptor = writer.finalize();
    CHECK(descriptor.count == 2);
    return descriptor;
}

OOCSnapshotDescriptor create_recovery_store(const std::string& base_path, size_t count = 2) {
    OOCRelationWriter writer(base_path);
    for (size_t i = 0; i < count; ++i) {
        CHECK(writer.write(make_relation(static_cast<int64_t>(2 * i + 1), 2 * i + 2)) == i);
    }
    const auto descriptor = writer.checkpoint_prefix();
    CHECK(descriptor.format_version == OOCRelationWriter::FORMAT_VERSION);
    CHECK(descriptor.store_id != 0);
    CHECK(descriptor.generation == 1);
    writer.fail_suspended_snapshot(); // Preserve INCOMPLETE exactly like process death.
    return descriptor;
}

std::vector<char> read_file_bytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    CHECK(static_cast<bool>(input));
    const auto end = input.tellg();
    CHECK(end >= 0);

    std::vector<char> bytes(static_cast<size_t>(end));
    input.seekg(0);
    if (!bytes.empty()) {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        CHECK(static_cast<bool>(input));
    }
    return bytes;
}

OOCSnapshotDescriptor upgrade_finalized_v2_pair_to_v3(const std::string& base_path,
                                                      const OOCSnapshotDescriptor& v2_descriptor) {
    const std::string data_path = base_path + ".reldata";
    const std::string index_path = base_path + ".relidx";

    CHECK(v2_descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V2);
    CHECK(read_u64_at(index_path, 0) == OOCRelationWriter::MAGIC_V2_FINAL);
    CHECK(read_u64_at(index_path, OOCRelationWriter::INDEX_FORMAT_VERSION_OFFSET) ==
          OOCRelationWriter::FORMAT_VERSION_V2);
    CHECK(read_u64_at(index_path, OOCRelationWriter::INDEX_STORE_ID_OFFSET) ==
          v2_descriptor.store_id);
    CHECK(read_u64_at(index_path, OOCRelationWriter::INDEX_COUNT_OFFSET) == v2_descriptor.count);
    CHECK(std::filesystem::file_size(data_path) == v2_descriptor.data_end);

    const auto v2_data = read_file_bytes(data_path);
    {
        std::ofstream output(data_path, std::ios::binary | std::ios::trunc);
        CHECK(static_cast<bool>(output));
        const uint64_t data_magic = OOCRelationWriter::MAGIC_V3_DATA;
        const uint64_t format_version = OOCRelationWriter::FORMAT_VERSION_V3;
        output.write(reinterpret_cast<const char*>(&data_magic), sizeof(data_magic));
        output.write(reinterpret_cast<const char*>(&format_version), sizeof(format_version));
        output.write(reinterpret_cast<const char*>(&v2_descriptor.store_id),
                     sizeof(v2_descriptor.store_id));
        if (!v2_data.empty()) {
            output.write(v2_data.data(), static_cast<std::streamsize>(v2_data.size()));
        }
        output.close();
        CHECK(static_cast<bool>(output));
    }

    for (uint64_t ordinal = 0; ordinal <= v2_descriptor.count; ++ordinal) {
        const auto offset_position = static_cast<std::streamoff>(
            OOCRelationWriter::INDEX_HEADER_BYTES + ordinal * sizeof(uint64_t));
        const uint64_t v2_offset = read_u64_at(index_path, offset_position);
        overwrite_u64(index_path, offset_position,
                      v2_offset + OOCRelationWriter::DATA_HEADER_BYTES);
    }
    overwrite_u64(index_path, 0, OOCRelationWriter::MAGIC_V3_FINAL);
    overwrite_u64(index_path, OOCRelationWriter::INDEX_FORMAT_VERSION_OFFSET,
                  OOCRelationWriter::FORMAT_VERSION_V3);

    auto v3_descriptor = v2_descriptor;
    v3_descriptor.format_version = OOCRelationWriter::FORMAT_VERSION_V3;
    v3_descriptor.data_end += OOCRelationWriter::DATA_HEADER_BYTES;
    return v3_descriptor;
}

void append_u64(const std::string& path, uint64_t value) {
    std::ofstream stream(path, std::ios::app | std::ios::binary);
    CHECK(static_cast<bool>(stream));
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    stream.close();
    CHECK(static_cast<bool>(stream));
}

void append_byte(const std::string& path, uint8_t value) {
    std::ofstream stream(path, std::ios::app | std::ios::binary);
    CHECK(static_cast<bool>(stream));
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    stream.close();
    CHECK(static_cast<bool>(stream));
}

void convert_finalized_v2_index_to_legacy_v1(const std::string& base_path) {
    const std::string index_path = base_path + ".relidx";
    std::ifstream input(index_path, std::ios::binary | std::ios::ate);
    CHECK(static_cast<bool>(input));
    const auto end = input.tellg();
    CHECK(end >= static_cast<std::streamoff>(OOCRelationWriter::INDEX_HEADER_BYTES));
    const size_t index_size = static_cast<size_t>(end);

    uint64_t count = 0;
    input.seekg(static_cast<std::streamoff>(OOCRelationWriter::INDEX_COUNT_OFFSET));
    input.read(reinterpret_cast<char*>(&count), sizeof(count));
    CHECK(static_cast<bool>(input));

    const size_t offsets_size =
        index_size - static_cast<size_t>(OOCRelationWriter::INDEX_HEADER_BYTES);
    std::vector<char> offsets(offsets_size);
    input.seekg(static_cast<std::streamoff>(OOCRelationWriter::INDEX_HEADER_BYTES));
    input.read(offsets.data(), static_cast<std::streamsize>(offsets.size()));
    CHECK(static_cast<bool>(input));
    input.close();

    std::ofstream output(index_path, std::ios::binary | std::ios::trunc);
    CHECK(static_cast<bool>(output));
    const uint64_t magic = OOCRelationWriter::MAGIC_V1_FINAL;
    output.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    output.write(reinterpret_cast<const char*>(&count), sizeof(count));
    output.write(offsets.data(), static_cast<std::streamsize>(offsets.size()));
    output.close();
    CHECK(static_cast<bool>(output));
}

void expect_finalized_reader_rejected(const std::string& base_path) {
    bool rejected = false;
    try {
        OOCRelationReader reader(base_path);
        (void)reader;
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
}

void expect_descriptor_reader_rejected(const std::string& base_path,
                                       const OOCSnapshotDescriptor& descriptor) {
    bool rejected = false;
    try {
        OOCRelationReader reader(base_path, descriptor);
        (void)reader;
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
}

void expect_both_finalized_readers_rejected(const std::string& base_path,
                                            const OOCSnapshotDescriptor& descriptor) {
    expect_finalized_reader_rejected(base_path);
    expect_descriptor_reader_rejected(base_path, descriptor);
}

void expect_resume_rejected_without_mutation(const std::string& base_path,
                                             const OOCSnapshotDescriptor& descriptor) {
    const auto index_size = std::filesystem::file_size(base_path + ".relidx");
    const auto data_size = std::filesystem::file_size(base_path + ".reldata");

    bool rejected = false;
    try {
        OOCRelationWriter writer(base_path, descriptor);
        (void)writer;
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(std::filesystem::file_size(base_path + ".relidx") == index_size);
    CHECK(std::filesystem::file_size(base_path + ".reldata") == data_size);
}

void test_shared_persistence_limits() {
    const size_t limit = Relation::MAX_SERIALIZED_LARGE_PRIMES;
    Relation at_limit = make_large_prime_relation(limit);

    std::ostringstream serialized(std::ios::binary);
    at_limit.serialize(serialized);
    CHECK(!serialized.str().empty());
    std::istringstream input(serialized.str(), std::ios::binary);
    const Relation stream_roundtrip = Relation::deserialize(input);
    CHECK(stream_roundtrip.rational_large_prime.size() == limit);

    Relation oversized = make_large_prime_relation(limit + 1);
    std::ostringstream rejected_stream(std::ios::binary);
    bool stream_rejected = false;
    try {
        oversized.serialize(rejected_stream);
    } catch (const std::length_error&) {
        stream_rejected = true;
    }
    CHECK(stream_rejected);
    CHECK(rejected_stream.str().empty());

    const std::string path = make_path("writer_limits");
    OOCArtifacts cleanup(path);
    OOCRelationWriter writer(path);

    bool first_rejected = false;
    try {
        (void)writer.write(oversized);
    } catch (const std::length_error&) {
        first_rejected = true;
    }
    CHECK(first_rejected);
    CHECK(writer.state() == OOCWriterState::Open);
    CHECK(writer.count() == 0);

    CHECK(writer.write(at_limit) == 0);
    CHECK(writer.count() == 1);

    bool second_rejected = false;
    try {
        (void)writer.write(oversized);
    } catch (const std::length_error&) {
        second_rejected = true;
    }
    CHECK(second_rejected);
    CHECK(writer.state() == OOCWriterState::Open);
    CHECK(writer.count() == 1);

    CHECK(writer.write(make_relation(3, 4)) == 1);
    CHECK(writer.finalize().count == 2);

    OOCRelationReader reader(path);
    CHECK(reader.count() == 2);
    CHECK(reader.read(0).rational_large_prime.size() == limit);
    CHECK(reader.read(1).a == 3);
}

void test_validated_resume_handoff_and_append() {
    const std::string path = make_path("valid_resume");
    OOCArtifacts cleanup(path);
    const auto descriptor = create_recovery_store(path);

    OOCRelationWriter writer(path, descriptor);
    CHECK(writer.state() == OOCWriterState::Open);
    CHECK(writer.count() == 2);

    auto prefix = writer.take_validated_resume_prefix();
    CHECK(prefix.has_value());
    CHECK(prefix->count == 2);
    CHECK(prefix->data_end == std::filesystem::file_size(path + ".reldata"));
    CHECK(prefix->seen.size() == 2);
    CHECK(prefix->seen.contains(ABPair{1, 2}));
    CHECK(prefix->seen.contains(ABPair{3, 4}));
    CHECK(prefix->full_relations == 2);
    CHECK(prefix->partial_1lp == 0);
    CHECK(prefix->partial_2lp == 0);
    CHECK(!writer.take_validated_resume_prefix().has_value());

    CHECK(writer.write(make_relation(5, 6)) == 2);
    CHECK(writer.finalize().count == 3);

    OOCRelationReader reader(path);
    CHECK(reader.count() == 3);
    CHECK(reader.read(0).a == 1);
    CHECK(reader.read(1).a == 3);
    CHECK(reader.read(2).a == 5);
}

void test_resume_rejects_index_shape_corruption() {
    {
        const std::string path = make_path("truncated_index");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_recovery_store(path);
        const auto size = std::filesystem::file_size(path + ".relidx");
        CHECK(size > 0);
        std::filesystem::resize_file(path + ".relidx", size - 1);
        expect_resume_rejected_without_mutation(path, descriptor);
    }
    {
        const std::string path = make_path("store_header_mismatch");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_recovery_store(path);
        overwrite_u64(path + ".relidx", OOCRelationWriter::INDEX_STORE_ID_OFFSET,
                      descriptor.store_id + 1);
        expect_resume_rejected_without_mutation(path, descriptor);
    }
    {
        const std::string path = make_path("first_offset");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_recovery_store(path);
        overwrite_u64(path + ".relidx", OOCRelationWriter::INDEX_HEADER_BYTES, 1);
        expect_resume_rejected_without_mutation(path, descriptor);
    }
    {
        const std::string path = make_path("nonmonotonic_offset");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_recovery_store(path);
        overwrite_u64(path + ".relidx", OOCRelationWriter::INDEX_HEADER_BYTES + sizeof(uint64_t),
                      0);
        expect_resume_rejected_without_mutation(path, descriptor);
    }
}

void test_resume_rejects_data_corruption() {
    {
        const std::string path = make_path("invalid_compact_record");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_recovery_store(path);
        const uint32_t invalid_count = Relation::MAX_SERIALIZED_FACTORS + 1;
        overwrite_u32(path + ".reldata", 16, invalid_count);
        expect_resume_rejected_without_mutation(path, descriptor);
    }
}

void test_paired_recovery_empty_prefix_and_generation() {
    const std::string path = make_path("empty_recovery");
    OOCArtifacts cleanup(path);
    const auto descriptor = create_recovery_store(path, 0);
    CHECK(descriptor.count == 0);
    CHECK(descriptor.data_end == 0);

    OOCRelationWriter writer(path, descriptor);
    CHECK(writer.recovery_outcome() == OOCRecoveryOutcome::AppendablePrefix);
    CHECK(writer.count() == 0);
    auto prefix = writer.take_validated_resume_prefix();
    CHECK(prefix.has_value());
    CHECK(prefix->count == 0);
    CHECK(prefix->seen.empty());

    const auto next = writer.checkpoint_prefix();
    CHECK(next.format_version == OOCRelationWriter::FORMAT_VERSION);
    CHECK(next.store_id == descriptor.store_id);
    CHECK(next.generation == descriptor.generation + 1);
    writer.resume_append(next);
    CHECK(writer.write(make_relation(1, 2)) == 0);
    CHECK(writer.finalize().count == 1);
}

void test_paired_recovery_rejects_descriptor_identity_and_generation() {
    const std::string path = make_path("descriptor_identity");
    OOCArtifacts cleanup(path);
    const auto descriptor = create_recovery_store(path);

    auto foreign = descriptor;
    ++foreign.store_id;
    expect_resume_rejected_without_mutation(path, foreign);

    auto legacy = descriptor;
    legacy.format_version = 1;
    expect_resume_rejected_without_mutation(path, legacy);

    auto zero_generation = descriptor;
    zero_generation.generation = 0;
    expect_resume_rejected_without_mutation(path, zero_generation);

    bool bare_rejected = false;
    try {
        OOCRelationWriter writer(path, true);
        (void)writer;
    } catch (const std::invalid_argument&) {
        bare_rejected = true;
    }
    CHECK(bare_rejected);

    const std::string legacy_path = make_path("legacy_incomplete_magic");
    OOCArtifacts legacy_cleanup(legacy_path);
    const auto legacy_descriptor = create_recovery_store(legacy_path);
    overwrite_u64(legacy_path + ".relidx", 0, OOCRelationWriter::MAGIC_INCOMPLETE_V1);
    expect_resume_rejected_without_mutation(legacy_path, legacy_descriptor);
}

void test_paired_recovery_rolls_back_uncommitted_tails() {
    const std::string path = make_path("tail_rollback");
    OOCArtifacts cleanup(path);
    const auto committed = create_recovery_store(path);

    {
        OOCRelationWriter writer(path, committed);
        CHECK(writer.write(make_relation(5, 6)) == 2);
        CHECK(writer.write(make_relation(7, 8)) == 3);
        const auto later = writer.checkpoint_prefix();
        CHECK(later.generation == committed.generation + 1);
        CHECK(later.count == 4);
        writer.fail_suspended_snapshot();
    }
    CHECK(std::filesystem::file_size(path + ".relidx") >
          OOCRelationWriter::index_size_for_count(committed.count));
    CHECK(std::filesystem::file_size(path + ".reldata") > committed.data_end);

    OOCRelationWriter recovered(path, committed);
    CHECK(recovered.recovery_outcome() == OOCRecoveryOutcome::AppendablePrefix);
    CHECK(recovered.count() == committed.count);
    CHECK(std::filesystem::file_size(path + ".relidx") ==
          OOCRelationWriter::index_size_for_count(committed.count));
    CHECK(std::filesystem::file_size(path + ".reldata") == committed.data_end);
    auto prefix = recovered.take_validated_resume_prefix();
    CHECK(prefix.has_value());
    CHECK(prefix->seen.size() == 2);
    CHECK(!prefix->seen.contains(ABPair{5, 6}));

    CHECK(recovered.write(make_relation(9, 10)) == 2);
    CHECK(recovered.finalize().count == 3);
    OOCRelationReader reader(path);
    CHECK(reader.count() == 3);
    CHECK(reader.read(2).a == 9);
}

void test_finalized_corpus_recovery_is_read_only() {
    const std::string path = make_path("finalized_recovery");
    OOCArtifacts cleanup(path);
    OOCSnapshotDescriptor committed;
    {
        OOCRelationWriter writer(path);
        CHECK(writer.write(make_relation(1, 2)) == 0);
        CHECK(writer.write(make_relation(3, 4)) == 1);
        committed = writer.checkpoint_prefix();
        writer.resume_append(committed);
        CHECK(writer.write(make_relation(5, 6)) == 2);
        CHECK(writer.finalize().count == 3);
    }
    const auto index_size = std::filesystem::file_size(path + ".relidx");
    const auto data_size = std::filesystem::file_size(path + ".reldata");

    OOCRelationWriter recovered(path, committed);
    CHECK(recovered.recovery_outcome() == OOCRecoveryOutcome::FinalizedCorpus);
    CHECK(recovered.state() == OOCWriterState::Finalized);
    CHECK(recovered.count() == 3);
    auto prefix = recovered.take_validated_resume_prefix();
    CHECK(prefix.has_value());
    CHECK(prefix->count == 3);
    CHECK(prefix->seen.size() == 3);
    CHECK(std::filesystem::file_size(path + ".relidx") == index_size);
    CHECK(std::filesystem::file_size(path + ".reldata") == data_size);

    auto mismatched_prefix = committed;
    ++mismatched_prefix.data_end;
    expect_resume_rejected_without_mutation(path, mismatched_prefix);
    auto future_prefix = committed;
    future_prefix.count = 4;
    expect_resume_rejected_without_mutation(path, future_prefix);
    auto foreign_store = committed;
    ++foreign_store.store_id;
    expect_resume_rejected_without_mutation(path, foreign_store);

    bool append_rejected = false;
    try {
        (void)recovered.write(make_relation(7, 8));
    } catch (const std::logic_error&) {
        append_rejected = true;
    }
    CHECK(append_rejected);
}

void fail_after_finalize_metadata(OOCRelationWriter::FinalizeStage stage) {
    if (stage == OOCRelationWriter::FinalizeStage::MetadataDurable) {
        throw std::runtime_error("injected finalize interruption");
    }
}

void test_interrupted_finalize_preserves_paired_recovery() {
    const std::string path = make_path("interrupted_finalize");
    OOCArtifacts cleanup(path);
    OOCSnapshotDescriptor committed;
    {
        OOCRelationWriter writer(path);
        CHECK(writer.write(make_relation(1, 2)) == 0);
        committed = writer.checkpoint_prefix();
        writer.resume_append(committed);
        CHECK(writer.write(make_relation(3, 4)) == 1);

        bool interrupted = false;
        try {
            (void)writer.finalize(fail_after_finalize_metadata);
        } catch (const std::runtime_error&) {
            interrupted = true;
        }
        CHECK(interrupted);
        CHECK(writer.state() == OOCWriterState::Failed);
    }

    CHECK(read_u64_at(path + ".relidx", 0) == OOCRelationWriter::MAGIC_INCOMPLETE);
    CHECK(read_u64_at(path + ".relidx", OOCRelationWriter::INDEX_FORMAT_VERSION_OFFSET) ==
          OOCRelationWriter::FORMAT_VERSION);
    CHECK(read_u64_at(path + ".relidx", OOCRelationWriter::INDEX_STORE_ID_OFFSET) ==
          committed.store_id);
    CHECK(read_u64_at(path + ".relidx", OOCRelationWriter::INDEX_COUNT_OFFSET) == 2);

    OOCRelationWriter recovered(path, committed);
    CHECK(recovered.recovery_outcome() == OOCRecoveryOutcome::AppendablePrefix);
    CHECK(recovered.count() == 1);
    CHECK(read_u64_at(path + ".relidx", OOCRelationWriter::INDEX_COUNT_OFFSET) == 0);
    CHECK(recovered.write(make_relation(5, 6)) == 1);
    CHECK(recovered.finalize().count == 2);

    OOCRelationReader reader(path);
    CHECK(reader.count() == 2);
    CHECK(reader.read(0).a == 1);
    CHECK(reader.read(1).a == 5);
}

void test_finalized_reader_rejects_unreferenced_tail() {
    {
        const std::string path = make_path("finalized_low_count");
        OOCArtifacts cleanup(path);
        create_finalized_store(path);
        overwrite_u64(path + ".relidx", OOCRelationWriter::INDEX_COUNT_OFFSET, 1);
        expect_finalized_reader_rejected(path);
    }
    {
        const std::string path = make_path("finalized_trailing_index");
        OOCArtifacts cleanup(path);
        create_finalized_store(path);
        append_u64(path + ".relidx", 0xBAD0FF5EULL);
        expect_finalized_reader_rejected(path);
    }
    {
        const std::string path = make_path("finalized_trailing_data");
        OOCArtifacts cleanup(path);
        create_finalized_store(path);
        append_byte(path + ".reldata", 0xA5);
        expect_finalized_reader_rejected(path);
    }
    {
        const std::string path = make_path("finalized_first_offset");
        OOCArtifacts cleanup(path);
        create_finalized_store(path);
        overwrite_u64(path + ".relidx", OOCRelationWriter::INDEX_HEADER_BYTES, 1);
        expect_finalized_reader_rejected(path);
    }
}

void test_finalized_reader_expected_descriptor_binding() {
    const std::string path = make_path("finalized_expected_descriptor");
    OOCArtifacts cleanup(path);
    const auto descriptor = create_finalized_store(path);

    OOCRelationReader exact(path, descriptor);
    CHECK(exact.count() == descriptor.count);
    CHECK(exact.read(0).a == 1);
    CHECK(exact.read(1).a == 3);

    auto foreign_store = descriptor;
    ++foreign_store.store_id;
    expect_descriptor_reader_rejected(path, foreign_store);

    auto zero_store_id = descriptor;
    zero_store_id.store_id = 0;
    expect_descriptor_reader_rejected(path, zero_store_id);

    auto wrong_count = descriptor;
    ++wrong_count.count;
    expect_descriptor_reader_rejected(path, wrong_count);

    auto wrong_data_end = descriptor;
    ++wrong_data_end.data_end;
    expect_descriptor_reader_rejected(path, wrong_data_end);

    auto wrong_format = descriptor;
    ++wrong_format.format_version;
    expect_descriptor_reader_rejected(path, wrong_format);

    auto zero_generation = descriptor;
    zero_generation.generation = 0;
    expect_descriptor_reader_rejected(path, zero_generation);
}

void test_finalized_reader_expected_descriptor_empty_store() {
    const std::string path = make_path("finalized_expected_empty");
    OOCArtifacts cleanup(path);
    OOCRelationWriter writer(path);
    const auto descriptor = writer.finalize();
    CHECK(descriptor.count == 0);
    CHECK(descriptor.data_end == 0);

    OOCRelationReader reader(path, descriptor);
    CHECK(reader.count() == 0);
    CHECK(reader.read_all().empty());
}

void test_finalized_reader_expected_descriptor_rejects_corruption() {
    {
        const std::string path = make_path("expected_trailing_index");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_finalized_store(path);
        append_u64(path + ".relidx", 0xBAD0FF5EULL);
        expect_descriptor_reader_rejected(path, descriptor);
    }
    {
        const std::string path = make_path("expected_trailing_data");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_finalized_store(path);
        append_byte(path + ".reldata", 0xA5);
        expect_descriptor_reader_rejected(path, descriptor);
    }
    {
        const std::string path = make_path("expected_bad_sentinel");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_finalized_store(path);
        const std::streamoff sentinel_offset = static_cast<std::streamoff>(
            OOCRelationWriter::INDEX_HEADER_BYTES + descriptor.count * sizeof(uint64_t));
        overwrite_u64(path + ".relidx", sentinel_offset, descriptor.data_end - 1);
        expect_descriptor_reader_rejected(path, descriptor);
    }
    {
        const std::string path = make_path("expected_non_monotonic_offset");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_finalized_store(path);
        const std::streamoff second_offset =
            static_cast<std::streamoff>(OOCRelationWriter::INDEX_HEADER_BYTES + sizeof(uint64_t));
        overwrite_u64(path + ".relidx", second_offset, 0);
        expect_descriptor_reader_rejected(path, descriptor);
    }
    {
        const std::string path = make_path("expected_incomplete_magic");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_finalized_store(path);
        overwrite_u64(path + ".relidx", 0, OOCRelationWriter::MAGIC_INCOMPLETE);
        expect_descriptor_reader_rejected(path, descriptor);
    }
    {
        const std::string path = make_path("expected_bad_version");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_finalized_store(path);
        overwrite_u64(path + ".relidx", OOCRelationWriter::INDEX_FORMAT_VERSION_OFFSET,
                      descriptor.format_version + 1);
        expect_descriptor_reader_rejected(path, descriptor);
    }
}

void test_v3_finalized_reader_roundtrip_and_physical_extents() {
    {
        const std::string path = make_path("v3_finalized_nonempty");
        OOCArtifacts cleanup(path);
        const auto v2_descriptor = create_finalized_store(path);
        const auto descriptor = upgrade_finalized_v2_pair_to_v3(path, v2_descriptor);

        CHECK(descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
        CHECK(descriptor.data_end == v2_descriptor.data_end + OOCRelationWriter::DATA_HEADER_BYTES);
        CHECK(read_u64_at(path + ".relidx", 0) == OOCRelationWriter::MAGIC_V3_FINAL);
        CHECK(read_u64_at(path + ".relidx", OOCRelationWriter::INDEX_FORMAT_VERSION_OFFSET) ==
              OOCRelationWriter::FORMAT_VERSION_V3);
        CHECK(read_u64_at(path + ".relidx", OOCRelationWriter::INDEX_STORE_ID_OFFSET) ==
              descriptor.store_id);
        CHECK(read_u64_at(path + ".reldata", 0) == OOCRelationWriter::MAGIC_V3_DATA);
        CHECK(read_u64_at(path + ".reldata", OOCRelationWriter::DATA_FORMAT_VERSION_OFFSET) ==
              OOCRelationWriter::FORMAT_VERSION_V3);
        CHECK(read_u64_at(path + ".reldata", OOCRelationWriter::DATA_STORE_ID_OFFSET) ==
              descriptor.store_id);

        const auto first_offset = read_u64_at(
            path + ".relidx", static_cast<std::streamoff>(OOCRelationWriter::INDEX_HEADER_BYTES));
        const auto sentinel_offset = static_cast<std::streamoff>(
            OOCRelationWriter::INDEX_HEADER_BYTES + descriptor.count * sizeof(uint64_t));
        CHECK(first_offset == OOCRelationWriter::DATA_HEADER_BYTES);
        CHECK(read_u64_at(path + ".relidx", sentinel_offset) == descriptor.data_end);
        CHECK(std::filesystem::file_size(path + ".reldata") == descriptor.data_end);

        OOCRelationReader ordinary(path);
        CHECK(ordinary.count() == 2);
        CHECK(ordinary.read(0).a == 1);
        CHECK(ordinary.read(1).a == 3);

        OOCRelationReader bound(path, descriptor);
        CHECK(bound.count() == 2);
        CHECK(bound.read(0).a == 1);
        CHECK(bound.read(1).a == 3);
    }
    {
        const std::string path = make_path("v3_finalized_empty");
        OOCArtifacts cleanup(path);
        OOCSnapshotDescriptor v2_descriptor;
        {
            OOCRelationWriter writer(path);
            v2_descriptor = writer.finalize();
        }
        CHECK(v2_descriptor.count == 0);
        CHECK(v2_descriptor.data_end == 0);
        const auto descriptor = upgrade_finalized_v2_pair_to_v3(path, v2_descriptor);

        CHECK(descriptor.count == 0);
        CHECK(descriptor.data_end == OOCRelationWriter::DATA_HEADER_BYTES);
        CHECK(std::filesystem::file_size(path + ".reldata") ==
              OOCRelationWriter::DATA_HEADER_BYTES);
        CHECK(read_u64_at(path + ".relidx",
                          static_cast<std::streamoff>(OOCRelationWriter::INDEX_HEADER_BYTES)) ==
              descriptor.data_end);

        OOCRelationReader ordinary(path);
        CHECK(ordinary.count() == 0);
        CHECK(ordinary.read_all().empty());
        OOCRelationReader bound(path, descriptor);
        CHECK(bound.count() == 0);
        CHECK(bound.read_all().empty());
    }
}

void test_v3_finalized_reader_rejects_same_size_foreign_data_swap() {
    const std::string first_path = make_path("v3_foreign_data_first");
    const std::string second_path = make_path("v3_foreign_data_second");
    OOCArtifacts first_cleanup(first_path);
    OOCArtifacts second_cleanup(second_path);

    const auto first_descriptor =
        upgrade_finalized_v2_pair_to_v3(first_path, create_finalized_store(first_path));
    const auto second_descriptor =
        upgrade_finalized_v2_pair_to_v3(second_path, create_finalized_store(second_path));
    CHECK(first_descriptor.store_id != second_descriptor.store_id);
    CHECK(first_descriptor.count == second_descriptor.count);
    CHECK(std::filesystem::file_size(first_path + ".reldata") ==
          std::filesystem::file_size(second_path + ".reldata"));

    CHECK(std::filesystem::copy_file(second_path + ".reldata", first_path + ".reldata",
                                     std::filesystem::copy_options::overwrite_existing));
    CHECK(std::filesystem::file_size(first_path + ".reldata") == first_descriptor.data_end);
    CHECK(read_u64_at(first_path + ".reldata", OOCRelationWriter::DATA_STORE_ID_OFFSET) ==
          second_descriptor.store_id);
    expect_both_finalized_readers_rejected(first_path, first_descriptor);

    OOCRelationReader source_reader(second_path, second_descriptor);
    CHECK(source_reader.count() == second_descriptor.count);
}

void test_v3_finalized_reader_rejects_data_header_corruption() {
    {
        const std::string path = make_path("v3_bad_data_magic");
        OOCArtifacts cleanup(path);
        const auto descriptor = upgrade_finalized_v2_pair_to_v3(path, create_finalized_store(path));
        overwrite_u64(path + ".reldata", 0, OOCRelationWriter::MAGIC_V3_DATA ^ uint64_t{1});
        expect_both_finalized_readers_rejected(path, descriptor);
    }
    {
        const std::string path = make_path("v3_bad_data_version");
        OOCArtifacts cleanup(path);
        const auto descriptor = upgrade_finalized_v2_pair_to_v3(path, create_finalized_store(path));
        overwrite_u64(path + ".reldata", OOCRelationWriter::DATA_FORMAT_VERSION_OFFSET,
                      OOCRelationWriter::FORMAT_VERSION_V2);
        expect_both_finalized_readers_rejected(path, descriptor);
    }
    {
        const std::string path = make_path("v3_bad_data_store_id");
        OOCArtifacts cleanup(path);
        const auto descriptor = upgrade_finalized_v2_pair_to_v3(path, create_finalized_store(path));
        overwrite_u64(path + ".reldata", OOCRelationWriter::DATA_STORE_ID_OFFSET,
                      descriptor.store_id ^ uint64_t{1});
        expect_both_finalized_readers_rejected(path, descriptor);
    }
    {
        const std::string path = make_path("v3_short_data_header");
        OOCArtifacts cleanup(path);
        OOCSnapshotDescriptor v2_descriptor;
        {
            OOCRelationWriter writer(path);
            v2_descriptor = writer.finalize();
        }
        const auto descriptor = upgrade_finalized_v2_pair_to_v3(path, v2_descriptor);
        std::filesystem::resize_file(path + ".reldata", OOCRelationWriter::DATA_HEADER_BYTES - 1);
        expect_both_finalized_readers_rejected(path, descriptor);
    }
}

void test_legacy_finalized_reader_compatibility_and_paired_rejection() {
    const std::string path = make_path("legacy_finalized_reader");
    OOCArtifacts cleanup(path);
    const auto descriptor = create_finalized_store(path);
    convert_finalized_v2_index_to_legacy_v1(path);

    OOCRelationReader reader(path);
    CHECK(reader.count() == 2);
    CHECK(reader.read(0).a == 1);
    CHECK(reader.read(1).a == 3);
    expect_descriptor_reader_rejected(path, descriptor);

    expect_resume_rejected_without_mutation(path, descriptor);
}

void test_prefix_reader_exact_extent_and_lease() {
    {
        const std::string path = make_path("prefix_trailing_index");
        OOCArtifacts cleanup(path);
        OOCRelationWriter writer(path);
        CHECK(writer.write(make_relation(1, 2)) == 0);
        const auto descriptor = writer.checkpoint_prefix();
        append_u64(path + ".relidx", 0xBAD0FF5EULL);

        bool rejected = false;
        try {
            OOCRelationPrefixReader reader(path, descriptor, writer);
            (void)reader;
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(writer.state() == OOCWriterState::Suspended);
        writer.fail_suspended_snapshot();
    }
    {
        const std::string path = make_path("prefix_trailing_data");
        OOCArtifacts cleanup(path);
        OOCRelationWriter writer(path);
        CHECK(writer.write(make_relation(1, 2)) == 0);
        const auto descriptor = writer.checkpoint_prefix();
        append_byte(path + ".reldata", 0xA5);

        bool rejected = false;
        try {
            OOCRelationPrefixReader reader(path, descriptor, writer);
            (void)reader;
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(writer.state() == OOCWriterState::Suspended);
        writer.fail_suspended_snapshot();
    }
    {
        const std::string path = make_path("prefix_active_reader");
        OOCArtifacts cleanup(path);
        OOCRelationWriter writer(path);
        CHECK(writer.write(make_relation(1, 2)) == 0);
        const auto descriptor = writer.checkpoint_prefix();
        {
            OOCRelationPrefixReader reader(path, descriptor, writer);
            CHECK(reader.count() == 1);

            bool resume_rejected = false;
            try {
                writer.resume_append(descriptor);
            } catch (const std::logic_error&) {
                resume_rejected = true;
            }
            CHECK(resume_rejected);
            CHECK(writer.state() == OOCWriterState::Suspended);

            bool finalize_rejected = false;
            try {
                (void)writer.finalize();
            } catch (const std::logic_error&) {
                finalize_rejected = true;
            }
            CHECK(finalize_rejected);
            CHECK(writer.state() == OOCWriterState::Suspended);
        }
        writer.resume_append(descriptor);
        CHECK(writer.write(make_relation(3, 4)) == 1);
        CHECK(writer.finalize().count == 2);
    }
}

void test_failed_snapshot_transition() {
    {
        const std::string path = make_path("snapshot_fail");
        OOCArtifacts cleanup(path);
        OOCRelationWriter writer(path);
        CHECK(writer.write(make_relation(1, 2)) == 0);
        const auto descriptor = writer.checkpoint_prefix();
        CHECK(descriptor.count == 1);
        CHECK(writer.state() == OOCWriterState::Suspended);

        writer.fail_suspended_snapshot();
        CHECK(writer.state() == OOCWriterState::Failed);

        bool write_rejected = false;
        try {
            (void)writer.write(make_relation(3, 4));
        } catch (const std::logic_error&) {
            write_rejected = true;
        }
        CHECK(write_rejected);

        bool finalize_rejected = false;
        try {
            (void)writer.finalize();
        } catch (const std::runtime_error&) {
            finalize_rejected = true;
        }
        CHECK(finalize_rejected);
    }
    {
        const std::string path = make_path("snapshot_fail_wrong_state");
        OOCArtifacts cleanup(path);
        OOCRelationWriter writer(path);
        bool rejected = false;
        try {
            writer.fail_suspended_snapshot();
        } catch (const std::logic_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(writer.state() == OOCWriterState::Open);
        CHECK(writer.finalize().count == 0);
    }
}

} // namespace

int main() {
    std::cout << "=== OOC Store Integrity Tests ===\n";
    test_shared_persistence_limits();
    test_validated_resume_handoff_and_append();
    test_resume_rejects_index_shape_corruption();
    test_resume_rejects_data_corruption();
    test_paired_recovery_empty_prefix_and_generation();
    test_paired_recovery_rejects_descriptor_identity_and_generation();
    test_paired_recovery_rolls_back_uncommitted_tails();
    test_finalized_corpus_recovery_is_read_only();
    test_interrupted_finalize_preserves_paired_recovery();
    test_finalized_reader_rejects_unreferenced_tail();
    test_finalized_reader_expected_descriptor_binding();
    test_finalized_reader_expected_descriptor_empty_store();
    test_finalized_reader_expected_descriptor_rejects_corruption();
    test_v3_finalized_reader_roundtrip_and_physical_extents();
    test_v3_finalized_reader_rejects_same_size_foreign_data_swap();
    test_v3_finalized_reader_rejects_data_header_corruption();
    test_legacy_finalized_reader_compatibility_and_paired_rejection();
    test_prefix_reader_exact_extent_and_lease();
    test_failed_snapshot_transition();
    std::cout << "All OOC store integrity tests passed!\n";
    return 0;
}
