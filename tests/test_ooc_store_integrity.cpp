#include "gnfs/core/relation.hpp"
#include "gnfs/relation/ooc_relation_store.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"

#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using gnfs::core::ABPair;
using gnfs::core::PrimePower;
using gnfs::core::Relation;
using gnfs::relation::OOCCleanupStatus;
using gnfs::relation::OOCCleanupTransaction;
using gnfs::relation::OOCRecoveryOutcome;
using gnfs::relation::OOCRelationPrefixReader;
using gnfs::relation::OOCRelationReader;
using gnfs::relation::OOCRelationWriter;
using gnfs::relation::OOCSnapshotDescriptor;
using gnfs::relation::OOCWriterState;
using gnfs::relation::RelationSequenceReceipt;
using gnfs::relation::RelationSequenceReceiptAccumulator;

static_assert(!std::is_constructible_v<OOCRelationWriter, std::string, OOCSnapshotDescriptor>);
static_assert(std::is_constructible_v<OOCRelationWriter, std::string, OOCSnapshotDescriptor,
                                      RelationSequenceReceipt>);

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
        try {
            const auto paths = OOCCleanupTransaction::paths_for(base_path_);
            std::error_code ignored;
            std::filesystem::remove(paths.intent_path, ignored);
            ignored.clear();
            std::filesystem::remove(paths.intent_pending_path, ignored);
            ignored.clear();
            std::filesystem::remove(paths.staged_path, ignored);
            ignored.clear();
            std::filesystem::remove(paths.staged_pending_path, ignored);
            ignored.clear();
            std::filesystem::remove(paths.quarantine_index_path, ignored);
            ignored.clear();
            std::filesystem::remove(paths.quarantine_data_path, ignored);
            ignored.clear();
            std::filesystem::remove(paths.lock_path, ignored);
        } catch (...) {
        }
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

Relation make_relation_with_extra(int64_t a, uint64_t b, int64_t extra_a, uint64_t extra_b) {
    Relation relation(a, b);
    relation.extra_ab_pairs.emplace_back(extra_a, extra_b);
    return relation;
}

RelationSequenceReceipt standard_sequence_receipt(uint64_t count) {
    RelationSequenceReceiptAccumulator sequence;
    for (uint64_t ordinal = 0; ordinal < count; ++ordinal) {
        sequence.append(make_relation(static_cast<int64_t>(2 * ordinal + 1), 2 * ordinal + 2));
    }
    return sequence.finish();
}

RelationSequenceReceipt minimal_sequence_receipt() {
    RelationSequenceReceiptAccumulator sequence;
    sequence.append(Relation(1, 2));
    return sequence.finish();
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
    CHECK(descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
    CHECK(descriptor.count == 2);
    CHECK(descriptor.data_end > OOCRelationWriter::DATA_HEADER_BYTES);
    return descriptor;
}

OOCSnapshotDescriptor create_recovery_store(const std::string& base_path, size_t count = 2) {
    OOCRelationWriter writer(base_path);
    for (size_t i = 0; i < count; ++i) {
        CHECK(writer.write(make_relation(static_cast<int64_t>(2 * i + 1), 2 * i + 2)) == i);
    }
    const auto descriptor = writer.checkpoint_prefix();
    CHECK(descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
    CHECK(descriptor.store_id != 0);
    CHECK(descriptor.generation == 1);
    CHECK(descriptor.data_end >= OOCRelationWriter::DATA_HEADER_BYTES);
    writer.fail_suspended_snapshot(); // Preserve INCOMPLETE exactly like process death.
    return descriptor;
}

void write_large_payload_records(OOCRelationWriter& writer) {
    Relation relation(1, 2);
    relation.rational_factors.assign(700'000, 3);
    for (int64_t ordinal = 0; ordinal < 4; ++ordinal) {
        relation.a = 2 * ordinal + 1;
        CHECK(writer.write(relation) == static_cast<size_t>(ordinal));
    }
}

void overwrite_with_oversized_first_record(const std::string& base_path,
                                           const OOCSnapshotDescriptor& descriptor) {
    CHECK(descriptor.count == 4);
    const uint64_t oversized_end = OOCRelationWriter::DATA_HEADER_BYTES +
                                   gnfs::relation::detail::MAX_COMPACT_RELATION_BYTES + 1;
    CHECK(oversized_end + 2 < descriptor.data_end);
    overwrite_u64(base_path + ".relidx", OOCRelationWriter::INDEX_HEADER_BYTES + sizeof(uint64_t),
                  oversized_end);
    overwrite_u64(base_path + ".relidx",
                  OOCRelationWriter::INDEX_HEADER_BYTES + 2 * sizeof(uint64_t), oversized_end + 1);
    overwrite_u64(base_path + ".relidx",
                  OOCRelationWriter::INDEX_HEADER_BYTES + 3 * sizeof(uint64_t), oversized_end + 2);
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

OOCSnapshotDescriptor downgrade_v3_pair_to_v2(const std::string& base_path,
                                              const OOCSnapshotDescriptor& v3_descriptor) {
    const std::string data_path = base_path + ".reldata";
    const std::string index_path = base_path + ".relidx";

    CHECK(v3_descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
    const uint64_t v3_magic = read_u64_at(index_path, 0);
    CHECK(v3_magic == OOCRelationWriter::MAGIC_V3_FINAL ||
          v3_magic == OOCRelationWriter::MAGIC_V3_INCOMPLETE);
    CHECK(read_u64_at(index_path, OOCRelationWriter::INDEX_FORMAT_VERSION_OFFSET) ==
          OOCRelationWriter::FORMAT_VERSION_V3);
    CHECK(read_u64_at(index_path, OOCRelationWriter::INDEX_STORE_ID_OFFSET) ==
          v3_descriptor.store_id);
    CHECK(std::filesystem::file_size(data_path) == v3_descriptor.data_end);
    CHECK(v3_descriptor.data_end >= OOCRelationWriter::DATA_HEADER_BYTES);
    CHECK(read_u64_at(data_path, 0) == OOCRelationWriter::MAGIC_V3_DATA);
    CHECK(read_u64_at(data_path, OOCRelationWriter::DATA_FORMAT_VERSION_OFFSET) ==
          OOCRelationWriter::FORMAT_VERSION_V3);
    CHECK(read_u64_at(data_path, OOCRelationWriter::DATA_STORE_ID_OFFSET) ==
          v3_descriptor.store_id);

    const auto v3_data = read_file_bytes(data_path);
    CHECK(v3_data.size() >= static_cast<size_t>(OOCRelationWriter::DATA_HEADER_BYTES));
    {
        std::ofstream output(data_path, std::ios::binary | std::ios::trunc);
        CHECK(static_cast<bool>(output));
        const size_t payload_size =
            v3_data.size() - static_cast<size_t>(OOCRelationWriter::DATA_HEADER_BYTES);
        if (payload_size != 0) {
            output.write(v3_data.data() + OOCRelationWriter::DATA_HEADER_BYTES,
                         static_cast<std::streamsize>(payload_size));
        }
        output.close();
        CHECK(static_cast<bool>(output));
    }

    for (uint64_t ordinal = 0; ordinal <= v3_descriptor.count; ++ordinal) {
        const auto offset_position = static_cast<std::streamoff>(
            OOCRelationWriter::INDEX_HEADER_BYTES + ordinal * sizeof(uint64_t));
        const uint64_t v3_offset = read_u64_at(index_path, offset_position);
        CHECK(v3_offset >= OOCRelationWriter::DATA_HEADER_BYTES);
        overwrite_u64(index_path, offset_position,
                      v3_offset - OOCRelationWriter::DATA_HEADER_BYTES);
    }
    overwrite_u64(index_path, 0,
                  v3_magic == OOCRelationWriter::MAGIC_V3_FINAL
                      ? OOCRelationWriter::MAGIC_V2_FINAL
                      : OOCRelationWriter::MAGIC_V2_INCOMPLETE);
    overwrite_u64(index_path, OOCRelationWriter::INDEX_FORMAT_VERSION_OFFSET,
                  OOCRelationWriter::FORMAT_VERSION_V2);

    auto v2_descriptor = v3_descriptor;
    v2_descriptor.format_version = OOCRelationWriter::FORMAT_VERSION_V2;
    v2_descriptor.data_end -= OOCRelationWriter::DATA_HEADER_BYTES;
    return v2_descriptor;
}

void check_v3_pair_layout(const std::string& base_path, const OOCSnapshotDescriptor& descriptor,
                          uint64_t expected_magic, uint64_t expected_persisted_count) {
    const std::string data_path = base_path + ".reldata";
    const std::string index_path = base_path + ".relidx";

    CHECK(descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
    CHECK(descriptor.store_id != 0);
    CHECK(descriptor.data_end >= OOCRelationWriter::DATA_HEADER_BYTES);
    CHECK(read_u64_at(index_path, 0) == expected_magic);
    CHECK(read_u64_at(index_path, OOCRelationWriter::INDEX_FORMAT_VERSION_OFFSET) ==
          OOCRelationWriter::FORMAT_VERSION_V3);
    CHECK(read_u64_at(index_path, OOCRelationWriter::INDEX_STORE_ID_OFFSET) == descriptor.store_id);
    CHECK(read_u64_at(index_path, OOCRelationWriter::INDEX_COUNT_OFFSET) ==
          expected_persisted_count);
    CHECK(read_u64_at(data_path, 0) == OOCRelationWriter::MAGIC_V3_DATA);
    CHECK(read_u64_at(data_path, OOCRelationWriter::DATA_FORMAT_VERSION_OFFSET) ==
          OOCRelationWriter::FORMAT_VERSION_V3);
    CHECK(read_u64_at(data_path, OOCRelationWriter::DATA_STORE_ID_OFFSET) == descriptor.store_id);
    CHECK(std::filesystem::file_size(index_path) ==
          OOCRelationWriter::index_size_for_count(descriptor.count));
    CHECK(std::filesystem::file_size(data_path) == descriptor.data_end);

    uint64_t previous = OOCRelationWriter::DATA_HEADER_BYTES;
    for (uint64_t ordinal = 0; ordinal <= descriptor.count; ++ordinal) {
        const auto offset_position = static_cast<std::streamoff>(
            OOCRelationWriter::INDEX_HEADER_BYTES + ordinal * sizeof(uint64_t));
        const uint64_t offset = read_u64_at(index_path, offset_position);
        CHECK(ordinal == 0 ? offset == previous : offset > previous);
        CHECK(offset <= descriptor.data_end);
        previous = offset;
    }
    CHECK(previous == descriptor.data_end);
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
    const auto index_bytes = read_file_bytes(base_path + ".relidx");
    const auto data_bytes = read_file_bytes(base_path + ".reldata");

    bool rejected = false;
    try {
        OOCRelationWriter writer(base_path, descriptor,
                                 standard_sequence_receipt(descriptor.count));
        (void)writer;
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(read_file_bytes(base_path + ".relidx") == index_bytes);
    CHECK(read_file_bytes(base_path + ".reldata") == data_bytes);
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

void test_persistence_rejects_zero_b() {
    Relation invalid(17, 0);

    std::ostringstream serialized(std::ios::binary);
    bool stream_write_rejected = false;
    try {
        invalid.serialize(serialized);
    } catch (const std::invalid_argument&) {
        stream_write_rejected = true;
    }
    CHECK(stream_write_rejected);
    CHECK(serialized.str().empty());

    Relation valid(17, 19);
    std::ostringstream valid_serialized(std::ios::binary);
    valid.serialize(valid_serialized);
    std::string forged = valid_serialized.str();
    CHECK(forged.size() >=
          sizeof(uint32_t) + sizeof(uint32_t) + sizeof(int64_t) + sizeof(uint64_t));
    for (size_t i = 16; i < 24; ++i) {
        forged[i] = '\0';
    }
    bool stream_read_rejected = false;
    try {
        std::istringstream input(forged, std::ios::binary);
        (void)Relation::deserialize(input);
    } catch (const std::runtime_error&) {
        stream_read_rejected = true;
    }
    CHECK(stream_read_rejected);

    Relation invalid_extra = make_relation_with_extra(17, 19, -5, 0);
    std::ostringstream rejected_extra_stream(std::ios::binary);
    bool extra_stream_write_rejected = false;
    try {
        invalid_extra.serialize(rejected_extra_stream);
    } catch (const std::invalid_argument&) {
        extra_stream_write_rejected = true;
    }
    CHECK(extra_stream_write_rejected);
    CHECK(rejected_extra_stream.str().empty());

    Relation valid_extra = make_relation_with_extra(17, 19, -5, 7);
    std::ostringstream valid_extra_serialized(std::ios::binary);
    valid_extra.serialize(valid_extra_serialized);
    std::string forged_extra = valid_extra_serialized.str();
    constexpr size_t stream_extra_b_offset = 2 * sizeof(uint32_t) + sizeof(int64_t) +
                                             sizeof(uint64_t) + 5 * sizeof(uint32_t) +
                                             sizeof(int64_t);
    CHECK(forged_extra.size() >= stream_extra_b_offset + sizeof(uint64_t));
    for (size_t i = stream_extra_b_offset; i < stream_extra_b_offset + sizeof(uint64_t); ++i) {
        forged_extra[i] = '\0';
    }
    bool extra_stream_read_rejected = false;
    try {
        std::istringstream input(forged_extra, std::ios::binary);
        (void)Relation::deserialize(input);
    } catch (const std::runtime_error&) {
        extra_stream_read_rejected = true;
    }
    CHECK(extra_stream_read_rejected);

    const std::string path = make_path("zero_b_writer");
    OOCArtifacts cleanup(path);
    OOCRelationWriter writer(path);
    bool ooc_write_rejected = false;
    try {
        (void)writer.write(invalid);
    } catch (const std::invalid_argument&) {
        ooc_write_rejected = true;
    }
    CHECK(ooc_write_rejected);
    CHECK(writer.state() == OOCWriterState::Open);
    CHECK(writer.count() == 0);
    bool extra_ooc_write_rejected = false;
    try {
        (void)writer.write(invalid_extra);
    } catch (const std::invalid_argument&) {
        extra_ooc_write_rejected = true;
    }
    CHECK(extra_ooc_write_rejected);
    CHECK(writer.state() == OOCWriterState::Open);
    CHECK(writer.count() == 0);
    CHECK(writer.write(valid) == 0);
    const auto descriptor = writer.finalize();

    overwrite_u64(path + ".reldata",
                  static_cast<std::streamoff>(OOCRelationWriter::DATA_HEADER_BYTES + 8), 0);
    bool ooc_read_rejected = false;
    try {
        OOCRelationReader reader(path, descriptor);
        (void)reader.read(0);
    } catch (const std::runtime_error&) {
        ooc_read_rejected = true;
    }
    CHECK(ooc_read_rejected);

    const std::string extra_path = make_path("zero_extra_b_reader");
    OOCArtifacts extra_cleanup(extra_path);
    OOCRelationWriter extra_writer(extra_path);
    CHECK(extra_writer.write(valid_extra) == 0);
    const auto extra_descriptor = extra_writer.finalize();
    constexpr size_t compact_extra_b_offset =
        sizeof(int64_t) + sizeof(uint64_t) + 5 * sizeof(uint32_t) + sizeof(int64_t);
    overwrite_u64(
        extra_path + ".reldata",
        static_cast<std::streamoff>(OOCRelationWriter::DATA_HEADER_BYTES + compact_extra_b_offset),
        0);
    bool extra_ooc_read_rejected = false;
    try {
        OOCRelationReader reader(extra_path, extra_descriptor);
        (void)reader.read(0);
    } catch (const std::runtime_error&) {
        extra_ooc_read_rejected = true;
    }
    CHECK(extra_ooc_read_rejected);
}

void test_v3_fresh_checkpoint_prefix_resume_and_finalize_layout() {
    const std::string path = make_path("v3_fresh_lifecycle");
    OOCArtifacts cleanup(path);
    OOCRelationWriter writer(path);
    CHECK(writer.state() == OOCWriterState::Open);
    CHECK(writer.count() == 0);

    const auto empty = writer.checkpoint_prefix();
    CHECK(empty.count == 0);
    CHECK(empty.data_end == OOCRelationWriter::DATA_HEADER_BYTES);
    check_v3_pair_layout(path, empty, OOCRelationWriter::MAGIC_V3_INCOMPLETE, 0);
    {
        OOCRelationPrefixReader prefix(path, empty, writer);
        CHECK(prefix.count() == 0);
    }

    writer.resume_append(empty);
    CHECK(writer.write(make_relation(1, 2)) == 0);
    CHECK(writer.write(make_relation(3, 4)) == 1);
    const auto nonempty = writer.checkpoint_prefix();
    CHECK(nonempty.generation == empty.generation + 1);
    CHECK(nonempty.count == 2);
    CHECK(nonempty.data_end > OOCRelationWriter::DATA_HEADER_BYTES);
    check_v3_pair_layout(path, nonempty, OOCRelationWriter::MAGIC_V3_INCOMPLETE, 0);
    {
        OOCRelationPrefixReader prefix(path, nonempty, writer);
        CHECK(prefix.count() == 2);
        CHECK(prefix.read(0).a == 1);
        CHECK(prefix.read(1).a == 3);
    }

    writer.resume_append(nonempty);
    CHECK(writer.write(make_relation(5, 6)) == 2);
    const auto finalized = writer.finalize();
    CHECK(finalized.generation == nonempty.generation + 1);
    CHECK(finalized.count == 3);
    check_v3_pair_layout(path, finalized, OOCRelationWriter::MAGIC_V3_FINAL, finalized.count);

    OOCRelationReader reader(path, finalized);
    CHECK(reader.count() == 3);
    CHECK(reader.read(0).a == 1);
    CHECK(reader.read(1).a == 3);
    CHECK(reader.read(2).a == 5);
}

void test_validated_resume_handoff_and_append() {
    const std::string path = make_path("valid_resume");
    OOCArtifacts cleanup(path);
    const auto descriptor = create_recovery_store(path);
    check_v3_pair_layout(path, descriptor, OOCRelationWriter::MAGIC_V3_INCOMPLETE, 0);

    OOCRelationWriter writer(path, descriptor, standard_sequence_receipt(descriptor.count));
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
    const auto finalized = writer.finalize();
    CHECK(finalized.count == 3);
    check_v3_pair_layout(path, finalized, OOCRelationWriter::MAGIC_V3_FINAL, finalized.count);

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
        overwrite_u64(path + ".relidx", OOCRelationWriter::INDEX_HEADER_BYTES,
                      OOCRelationWriter::DATA_HEADER_BYTES - 1);
        expect_resume_rejected_without_mutation(path, descriptor);
    }
    {
        const std::string path = make_path("nonmonotonic_offset");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_recovery_store(path);
        overwrite_u64(path + ".relidx", OOCRelationWriter::INDEX_HEADER_BYTES + sizeof(uint64_t),
                      OOCRelationWriter::DATA_HEADER_BYTES);
        expect_resume_rejected_without_mutation(path, descriptor);
    }
}

void test_resume_rejects_data_corruption() {
    {
        const std::string path = make_path("invalid_compact_record");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_recovery_store(path);
        const uint32_t invalid_count = Relation::MAX_SERIALIZED_FACTORS + 1;
        overwrite_u32(path + ".reldata",
                      static_cast<std::streamoff>(OOCRelationWriter::DATA_HEADER_BYTES + 16),
                      invalid_count);
        expect_resume_rejected_without_mutation(path, descriptor);
    }
    {
        const std::string path = make_path("oversized_compact_record");
        OOCArtifacts cleanup(path);
        auto descriptor = create_recovery_store(path, 1);
        const uint64_t oversized_end = OOCRelationWriter::DATA_HEADER_BYTES +
                                       gnfs::relation::detail::MAX_COMPACT_RELATION_BYTES + 1;
        std::filesystem::resize_file(path + ".reldata", oversized_end);
        overwrite_u64(path + ".relidx", OOCRelationWriter::INDEX_HEADER_BYTES + sizeof(uint64_t),
                      oversized_end);
        descriptor.data_end = oversized_end;
        const auto index_bytes = read_file_bytes(path + ".relidx");
        const auto data_bytes = read_file_bytes(path + ".reldata");
        bool rejected_at_size_gate = false;
        try {
            OOCRelationWriter writer(path, descriptor, standard_sequence_receipt(descriptor.count));
            (void)writer;
        } catch (const std::runtime_error& error) {
            rejected_at_size_gate =
                std::string(error.what()).find("record size exceeds persistence limit") !=
                std::string::npos;
        }
        CHECK(rejected_at_size_gate);
        CHECK(read_file_bytes(path + ".relidx") == index_bytes);
        CHECK(read_file_bytes(path + ".reldata") == data_bytes);
    }
    {
        const std::string path = make_path("zero_b_compact_record");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_recovery_store(path, 1);
        overwrite_u64(path + ".reldata",
                      static_cast<std::streamoff>(OOCRelationWriter::DATA_HEADER_BYTES + 8), 0);
        expect_resume_rejected_without_mutation(path, descriptor);
    }
    {
        const std::string path = make_path("zero_extra_b_compact_record");
        OOCArtifacts cleanup(path);
        const Relation relation = make_relation_with_extra(17, 19, -5, 7);
        OOCSnapshotDescriptor descriptor;
        {
            OOCRelationWriter writer(path);
            CHECK(writer.write(relation) == 0);
            descriptor = writer.checkpoint_prefix();
            writer.fail_suspended_snapshot();
        }
        constexpr size_t compact_extra_b_offset =
            sizeof(int64_t) + sizeof(uint64_t) + 5 * sizeof(uint32_t) + sizeof(int64_t);
        overwrite_u64(path + ".reldata",
                      static_cast<std::streamoff>(OOCRelationWriter::DATA_HEADER_BYTES +
                                                  compact_extra_b_offset),
                      0);
        expect_resume_rejected_without_mutation(path, descriptor);
    }
}

void test_resume_rejects_v3_data_header_corruption_without_mutation() {
    {
        const std::string path = make_path("resume_bad_v3_data_magic");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_recovery_store(path);
        overwrite_u64(path + ".reldata", 0, OOCRelationWriter::MAGIC_V3_DATA ^ uint64_t{1});
        expect_resume_rejected_without_mutation(path, descriptor);
    }
    {
        const std::string path = make_path("resume_bad_v3_data_version");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_recovery_store(path);
        overwrite_u64(path + ".reldata", OOCRelationWriter::DATA_FORMAT_VERSION_OFFSET,
                      OOCRelationWriter::FORMAT_VERSION_V2);
        expect_resume_rejected_without_mutation(path, descriptor);
    }
    {
        const std::string path = make_path("resume_bad_v3_data_store_id");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_recovery_store(path);
        overwrite_u64(path + ".reldata", OOCRelationWriter::DATA_STORE_ID_OFFSET,
                      descriptor.store_id ^ uint64_t{1});
        expect_resume_rejected_without_mutation(path, descriptor);
    }
    {
        const std::string path = make_path("resume_truncated_v3_data_header");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_recovery_store(path, 0);
        std::filesystem::resize_file(path + ".reldata", OOCRelationWriter::DATA_HEADER_BYTES - 1);
        expect_resume_rejected_without_mutation(path, descriptor);
    }
}

void test_resume_rejects_same_size_foreign_v3_data_without_mutation() {
    const std::string first_path = make_path("resume_foreign_v3_data_first");
    const std::string second_path = make_path("resume_foreign_v3_data_second");
    OOCArtifacts first_cleanup(first_path);
    OOCArtifacts second_cleanup(second_path);
    const auto first_descriptor = create_recovery_store(first_path);
    const auto second_descriptor = create_recovery_store(second_path);

    CHECK(first_descriptor.store_id != second_descriptor.store_id);
    CHECK(first_descriptor.count == second_descriptor.count);
    CHECK(first_descriptor.data_end == second_descriptor.data_end);
    CHECK(std::filesystem::file_size(first_path + ".reldata") ==
          std::filesystem::file_size(second_path + ".reldata"));
    CHECK(std::filesystem::copy_file(second_path + ".reldata", first_path + ".reldata",
                                     std::filesystem::copy_options::overwrite_existing));
    CHECK(read_u64_at(first_path + ".reldata", OOCRelationWriter::DATA_STORE_ID_OFFSET) ==
          second_descriptor.store_id);
    expect_resume_rejected_without_mutation(first_path, first_descriptor);
}

void test_active_prefix_and_resume_reject_same_size_foreign_v3_data() {
    const std::string owner_path = make_path("active_foreign_v3_data_owner");
    const std::string foreign_path = make_path("active_foreign_v3_data_source");
    OOCArtifacts owner_cleanup(owner_path);
    OOCArtifacts foreign_cleanup(foreign_path);

    OOCRelationWriter owner(owner_path);
    OOCRelationWriter foreign(foreign_path);
    CHECK(owner.write(make_relation(1, 2)) == 0);
    CHECK(owner.write(make_relation(3, 4)) == 1);
    CHECK(foreign.write(make_relation(1, 2)) == 0);
    CHECK(foreign.write(make_relation(3, 4)) == 1);
    const auto owner_descriptor = owner.checkpoint_prefix();
    const auto foreign_descriptor = foreign.checkpoint_prefix();
    CHECK(owner_descriptor.store_id != foreign_descriptor.store_id);
    CHECK(owner_descriptor.count == foreign_descriptor.count);
    CHECK(owner_descriptor.data_end == foreign_descriptor.data_end);

    CHECK(std::filesystem::copy_file(foreign_path + ".reldata", owner_path + ".reldata",
                                     std::filesystem::copy_options::overwrite_existing));
    const auto index_bytes = read_file_bytes(owner_path + ".relidx");
    const auto data_bytes = read_file_bytes(owner_path + ".reldata");

    bool prefix_rejected = false;
    try {
        OOCRelationPrefixReader reader(owner_path, owner_descriptor, owner);
        (void)reader;
    } catch (const std::runtime_error&) {
        prefix_rejected = true;
    }
    CHECK(prefix_rejected);
    CHECK(owner.state() == OOCWriterState::Suspended);
    CHECK(read_file_bytes(owner_path + ".relidx") == index_bytes);
    CHECK(read_file_bytes(owner_path + ".reldata") == data_bytes);

    bool resume_rejected = false;
    try {
        owner.resume_append(owner_descriptor);
    } catch (const std::runtime_error&) {
        resume_rejected = true;
    }
    CHECK(resume_rejected);
    CHECK(owner.state() == OOCWriterState::Failed);
    CHECK(read_file_bytes(owner_path + ".relidx") == index_bytes);
    CHECK(read_file_bytes(owner_path + ".reldata") == data_bytes);

    foreign.resume_append(foreign_descriptor);
    (void)foreign.finalize();
}

void test_suspended_finalize_rejects_duplicate_offset_without_mutation() {
    const std::string path = make_path("suspended_finalize_duplicate_offset");
    OOCArtifacts cleanup(path);
    OOCRelationWriter writer(path);
    CHECK(writer.write(make_relation(1, 2)) == 0);
    CHECK(writer.write(make_relation(3, 4)) == 1);
    const auto descriptor = writer.checkpoint_prefix();
    CHECK(descriptor.count == 2);

    overwrite_u64(path + ".relidx", OOCRelationWriter::INDEX_HEADER_BYTES + sizeof(uint64_t),
                  OOCRelationWriter::DATA_HEADER_BYTES);
    const auto index_bytes = read_file_bytes(path + ".relidx");
    const auto data_bytes = read_file_bytes(path + ".reldata");

    bool rejected = false;
    try {
        (void)writer.finalize();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(writer.state() == OOCWriterState::Failed);
    CHECK(read_file_bytes(path + ".relidx") == index_bytes);
    CHECK(read_file_bytes(path + ".reldata") == data_bytes);
}

void test_resume_rejects_v2_prefix_without_mutation() {
    const std::string path = make_path("resume_v2_prefix");
    OOCArtifacts cleanup(path);
    const auto v3_descriptor = create_recovery_store(path);
    const auto v2_descriptor = downgrade_v3_pair_to_v2(path, v3_descriptor);

    CHECK(v2_descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V2);
    CHECK(v2_descriptor.data_end + OOCRelationWriter::DATA_HEADER_BYTES == v3_descriptor.data_end);
    CHECK(read_u64_at(path + ".relidx", 0) == OOCRelationWriter::MAGIC_V2_INCOMPLETE);
    CHECK(read_u64_at(path + ".relidx", OOCRelationWriter::INDEX_FORMAT_VERSION_OFFSET) ==
          OOCRelationWriter::FORMAT_VERSION_V2);
    CHECK(std::filesystem::file_size(path + ".reldata") == v2_descriptor.data_end);
    expect_resume_rejected_without_mutation(path, v2_descriptor);
}

void test_paired_recovery_empty_prefix_and_generation() {
    const std::string path = make_path("empty_recovery");
    OOCArtifacts cleanup(path);
    const auto descriptor = create_recovery_store(path, 0);
    CHECK(descriptor.count == 0);
    CHECK(descriptor.data_end == OOCRelationWriter::DATA_HEADER_BYTES);
    check_v3_pair_layout(path, descriptor, OOCRelationWriter::MAGIC_V3_INCOMPLETE, 0);

    OOCRelationWriter writer(path, descriptor, standard_sequence_receipt(descriptor.count));
    CHECK(writer.recovery_outcome() == OOCRecoveryOutcome::AppendablePrefix);
    CHECK(writer.count() == 0);
    auto prefix = writer.take_validated_resume_prefix();
    CHECK(prefix.has_value());
    CHECK(prefix->count == 0);
    CHECK(prefix->seen.empty());

    const auto next = writer.checkpoint_prefix();
    CHECK(next.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
    CHECK(next.store_id == descriptor.store_id);
    CHECK(next.generation == descriptor.generation + 1);
    check_v3_pair_layout(path, next, OOCRelationWriter::MAGIC_V3_INCOMPLETE, 0);
    writer.resume_append(next);
    CHECK(writer.write(make_relation(1, 2)) == 0);
    const auto finalized = writer.finalize();
    CHECK(finalized.count == 1);
    check_v3_pair_layout(path, finalized, OOCRelationWriter::MAGIC_V3_FINAL, finalized.count);
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
        OOCRelationWriter writer(path, committed, standard_sequence_receipt(committed.count));
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

    OOCRelationWriter recovered(path, committed, standard_sequence_receipt(committed.count));
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
    OOCSnapshotDescriptor stale_committed;
    OOCSnapshotDescriptor committed;
    {
        OOCRelationWriter writer(path);
        CHECK(writer.write(make_relation(1, 2)) == 0);
        CHECK(writer.write(make_relation(3, 4)) == 1);
        stale_committed = writer.checkpoint_prefix();
        writer.resume_append(stale_committed);
        CHECK(writer.write(make_relation(5, 6)) == 2);
        committed = writer.checkpoint_prefix();
        writer.resume_append(committed);
        CHECK(writer.finalize().count == 3);
    }
    const auto index_size = std::filesystem::file_size(path + ".relidx");
    const auto data_size = std::filesystem::file_size(path + ".reldata");

    expect_resume_rejected_without_mutation(path, stale_committed);
    OOCRelationWriter recovered(path, committed, standard_sequence_receipt(committed.count));
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

    CHECK(read_u64_at(path + ".relidx", 0) == OOCRelationWriter::MAGIC_V3_INCOMPLETE);
    CHECK(read_u64_at(path + ".relidx", OOCRelationWriter::INDEX_FORMAT_VERSION_OFFSET) ==
          OOCRelationWriter::FORMAT_VERSION_V3);
    CHECK(read_u64_at(path + ".relidx", OOCRelationWriter::INDEX_STORE_ID_OFFSET) ==
          committed.store_id);
    CHECK(read_u64_at(path + ".relidx", OOCRelationWriter::INDEX_COUNT_OFFSET) == 2);
    CHECK(read_u64_at(path + ".reldata", 0) == OOCRelationWriter::MAGIC_V3_DATA);
    CHECK(read_u64_at(path + ".reldata", OOCRelationWriter::DATA_FORMAT_VERSION_OFFSET) ==
          OOCRelationWriter::FORMAT_VERSION_V3);
    CHECK(read_u64_at(path + ".reldata", OOCRelationWriter::DATA_STORE_ID_OFFSET) ==
          committed.store_id);

    OOCRelationWriter recovered(path, committed, standard_sequence_receipt(committed.count));
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
        overwrite_u64(path + ".relidx", OOCRelationWriter::INDEX_HEADER_BYTES,
                      OOCRelationWriter::DATA_HEADER_BYTES - 1);
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
    CHECK(descriptor.data_end == OOCRelationWriter::DATA_HEADER_BYTES);
    check_v3_pair_layout(path, descriptor, OOCRelationWriter::MAGIC_V3_FINAL, 0);

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
        const std::string path = make_path("expected_duplicate_offset");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_finalized_store(path);
        const std::streamoff second_offset =
            static_cast<std::streamoff>(OOCRelationWriter::INDEX_HEADER_BYTES + sizeof(uint64_t));
        overwrite_u64(path + ".relidx", second_offset, OOCRelationWriter::DATA_HEADER_BYTES);
        expect_descriptor_reader_rejected(path, descriptor);
    }
    {
        const std::string path = make_path("expected_incomplete_magic");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_finalized_store(path);
        overwrite_u64(path + ".relidx", 0, OOCRelationWriter::MAGIC_V3_INCOMPLETE);
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
        const auto descriptor = create_finalized_store(path);
        check_v3_pair_layout(path, descriptor, OOCRelationWriter::MAGIC_V3_FINAL, descriptor.count);

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
        OOCSnapshotDescriptor descriptor;
        {
            OOCRelationWriter writer(path);
            descriptor = writer.finalize();
        }

        CHECK(descriptor.count == 0);
        CHECK(descriptor.data_end == OOCRelationWriter::DATA_HEADER_BYTES);
        check_v3_pair_layout(path, descriptor, OOCRelationWriter::MAGIC_V3_FINAL, 0);

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

    const auto first_descriptor = create_finalized_store(first_path);
    const auto second_descriptor = create_finalized_store(second_path);
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
        const auto descriptor = create_finalized_store(path);
        overwrite_u64(path + ".reldata", 0, OOCRelationWriter::MAGIC_V3_DATA ^ uint64_t{1});
        expect_both_finalized_readers_rejected(path, descriptor);
    }
    {
        const std::string path = make_path("v3_bad_data_version");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_finalized_store(path);
        overwrite_u64(path + ".reldata", OOCRelationWriter::DATA_FORMAT_VERSION_OFFSET,
                      OOCRelationWriter::FORMAT_VERSION_V2);
        expect_both_finalized_readers_rejected(path, descriptor);
    }
    {
        const std::string path = make_path("v3_bad_data_store_id");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_finalized_store(path);
        overwrite_u64(path + ".reldata", OOCRelationWriter::DATA_STORE_ID_OFFSET,
                      descriptor.store_id ^ uint64_t{1});
        expect_both_finalized_readers_rejected(path, descriptor);
    }
    {
        const std::string path = make_path("v3_short_data_header");
        OOCArtifacts cleanup(path);
        OOCSnapshotDescriptor descriptor;
        {
            OOCRelationWriter writer(path);
            descriptor = writer.finalize();
        }
        std::filesystem::resize_file(path + ".reldata", OOCRelationWriter::DATA_HEADER_BYTES - 1);
        expect_both_finalized_readers_rejected(path, descriptor);
    }
}

void test_v1_v2_finalized_reader_compatibility_and_paired_rejection() {
    const std::string path = make_path("legacy_finalized_reader");
    OOCArtifacts cleanup(path);
    const auto v3_descriptor = create_finalized_store(path);
    const auto v2_descriptor = downgrade_v3_pair_to_v2(path, v3_descriptor);

    CHECK(read_u64_at(path + ".relidx", 0) == OOCRelationWriter::MAGIC_V2_FINAL);
    CHECK(read_u64_at(path + ".relidx", OOCRelationWriter::INDEX_FORMAT_VERSION_OFFSET) ==
          OOCRelationWriter::FORMAT_VERSION_V2);
    CHECK(read_u64_at(path + ".relidx",
                      static_cast<std::streamoff>(OOCRelationWriter::INDEX_HEADER_BYTES)) == 0);
    const auto v2_sentinel_position = static_cast<std::streamoff>(
        OOCRelationWriter::INDEX_HEADER_BYTES + v2_descriptor.count * sizeof(uint64_t));
    CHECK(read_u64_at(path + ".relidx", v2_sentinel_position) == v2_descriptor.data_end);
    CHECK(std::filesystem::file_size(path + ".reldata") == v2_descriptor.data_end);

    {
        OOCRelationReader v2_reader(path);
        CHECK(v2_reader.count() == 2);
        CHECK(v2_reader.read(0).a == 1);
        CHECK(v2_reader.read(1).a == 3);
    }
    {
        OOCRelationReader v2_bound_reader(path, v2_descriptor);
        CHECK(v2_bound_reader.count() == 2);
        CHECK(v2_bound_reader.read(0).a == 1);
        CHECK(v2_bound_reader.read(1).a == 3);
    }
    expect_resume_rejected_without_mutation(path, v2_descriptor);

    convert_finalized_v2_index_to_legacy_v1(path);

    OOCRelationReader v1_reader(path);
    CHECK(v1_reader.count() == 2);
    CHECK(v1_reader.read(0).a == 1);
    CHECK(v1_reader.read(1).a == 3);
    expect_descriptor_reader_rejected(path, v2_descriptor);
    expect_resume_rejected_without_mutation(path, v2_descriptor);
}

void test_empty_v1_v2_finalized_reader_compatibility() {
    const std::string path = make_path("legacy_empty_finalized_reader");
    OOCArtifacts cleanup(path);
    OOCSnapshotDescriptor v3_descriptor;
    {
        OOCRelationWriter writer(path);
        v3_descriptor = writer.finalize();
    }
    CHECK(v3_descriptor.count == 0);
    CHECK(v3_descriptor.data_end == OOCRelationWriter::DATA_HEADER_BYTES);

    const auto v2_descriptor = downgrade_v3_pair_to_v2(path, v3_descriptor);
    CHECK(v2_descriptor.count == 0);
    CHECK(v2_descriptor.data_end == 0);
    CHECK(std::filesystem::file_size(path + ".reldata") == 0);
    {
        OOCRelationReader ordinary(path);
        CHECK(ordinary.count() == 0);
        CHECK(ordinary.read_all().empty());
    }
    {
        OOCRelationReader bound(path, v2_descriptor);
        CHECK(bound.count() == 0);
        CHECK(bound.read_all().empty());
    }

    convert_finalized_v2_index_to_legacy_v1(path);
    OOCRelationReader v1_reader(path);
    CHECK(v1_reader.count() == 0);
    CHECK(v1_reader.read_all().empty());
    expect_descriptor_reader_rejected(path, v2_descriptor);
}

void test_finalized_reader_rejects_oversized_record_before_decode() {
    const std::string path = make_path("finalized_oversized_record");
    OOCArtifacts cleanup(path);
    OOCSnapshotDescriptor descriptor;
    {
        OOCRelationWriter writer(path);
        write_large_payload_records(writer);
        descriptor = writer.finalize();
    }
    overwrite_with_oversized_first_record(path, descriptor);

    OOCRelationReader reader(path);
    bool rejected_at_size_gate = false;
    try {
        (void)reader.read(0);
    } catch (const std::runtime_error& error) {
        rejected_at_size_gate =
            std::string(error.what()).find("record exceeds persistence limit") != std::string::npos;
    }
    CHECK(rejected_at_size_gate);
}

void test_finalized_reader_rejects_count_larger_than_data_extent() {
    const std::string path = make_path("finalized_count_exceeds_data_extent");
    OOCArtifacts cleanup(path);
    OOCSnapshotDescriptor descriptor;
    {
        OOCRelationWriter writer(path);
        CHECK(writer.write(make_relation(1, 2)) == 0);
        descriptor = writer.finalize();
    }

    const uint64_t shortened_end = OOCRelationWriter::DATA_HEADER_BYTES +
                                   gnfs::relation::detail::MIN_COMPACT_RELATION_BYTES - 1;
    CHECK(shortened_end < descriptor.data_end);
    std::filesystem::resize_file(path + ".reldata", shortened_end);
    overwrite_u64(path + ".relidx", OOCRelationWriter::INDEX_HEADER_BYTES + sizeof(uint64_t),
                  shortened_end);

    bool rejected_at_extent_gate = false;
    try {
        OOCRelationReader reader(path);
        (void)reader;
    } catch (const std::runtime_error& error) {
        rejected_at_extent_gate =
            std::string(error.what()).find("relation count exceeds data extent") !=
            std::string::npos;
    }
    CHECK(rejected_at_extent_gate);
}

void test_minimal_compact_record_extent_is_accepted() {
    const std::string finalized_path = make_path("finalized_minimal_compact_record");
    OOCArtifacts finalized_cleanup(finalized_path);
    OOCSnapshotDescriptor finalized_descriptor;
    {
        OOCRelationWriter writer(finalized_path);
        CHECK(writer.write(Relation(1, 2)) == 0);
        finalized_descriptor = writer.finalize();
    }
    CHECK(finalized_descriptor.data_end == OOCRelationWriter::DATA_HEADER_BYTES +
                                               gnfs::relation::detail::MIN_COMPACT_RELATION_BYTES);
    OOCRelationReader finalized_reader(finalized_path);
    CHECK(finalized_reader.read(0).a == 1);

    const std::string prefix_path = make_path("prefix_minimal_compact_record");
    OOCArtifacts prefix_cleanup(prefix_path);
    OOCRelationWriter prefix_writer(prefix_path);
    CHECK(prefix_writer.write(Relation(1, 2)) == 0);
    const auto prefix_descriptor = prefix_writer.checkpoint_prefix();
    CHECK(prefix_descriptor.data_end == OOCRelationWriter::DATA_HEADER_BYTES +
                                            gnfs::relation::detail::MIN_COMPACT_RELATION_BYTES);
    {
        OOCRelationPrefixReader prefix_reader(prefix_path, prefix_descriptor, prefix_writer);
        CHECK(prefix_reader.read(0).a == 1);
    }
    prefix_writer.fail_suspended_snapshot();

    const std::string recovery_path = make_path("recovery_minimal_compact_record");
    OOCArtifacts recovery_cleanup(recovery_path);
    OOCSnapshotDescriptor recovery_descriptor;
    {
        OOCRelationWriter writer(recovery_path);
        CHECK(writer.write(Relation(1, 2)) == 0);
        recovery_descriptor = writer.checkpoint_prefix();
        writer.fail_suspended_snapshot();
    }
    OOCRelationWriter recovered(recovery_path, recovery_descriptor, minimal_sequence_receipt());
    CHECK(recovered.count() == 1);
    recovered.abort();
}

void test_recovery_rejects_count_larger_than_data_extent() {
    const std::string path = make_path("recovery_count_exceeds_data_extent");
    OOCArtifacts cleanup(path);
    auto descriptor = create_recovery_store(path, 1);

    const uint64_t shortened_end = OOCRelationWriter::DATA_HEADER_BYTES +
                                   gnfs::relation::detail::MIN_COMPACT_RELATION_BYTES - 1;
    CHECK(shortened_end < descriptor.data_end);
    std::filesystem::resize_file(path + ".reldata", shortened_end);
    overwrite_u64(path + ".relidx", OOCRelationWriter::INDEX_HEADER_BYTES + sizeof(uint64_t),
                  shortened_end);
    descriptor.data_end = shortened_end;

    bool rejected_at_extent_gate = false;
    try {
        OOCRelationWriter writer(path, descriptor, standard_sequence_receipt(descriptor.count));
        (void)writer;
    } catch (const std::runtime_error& error) {
        rejected_at_extent_gate =
            std::string(error.what()).find("relation count exceeds data extent") !=
            std::string::npos;
    }
    CHECK(rejected_at_extent_gate);
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
        const std::string path = make_path("prefix_duplicate_offset");
        OOCArtifacts cleanup(path);
        OOCRelationWriter writer(path);
        CHECK(writer.write(make_relation(1, 2)) == 0);
        CHECK(writer.write(make_relation(3, 4)) == 1);
        const auto descriptor = writer.checkpoint_prefix();
        overwrite_u64(path + ".relidx", OOCRelationWriter::INDEX_HEADER_BYTES + sizeof(uint64_t),
                      OOCRelationWriter::DATA_HEADER_BYTES);

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
        const std::string path = make_path("prefix_oversized_record");
        OOCArtifacts cleanup(path);
        OOCRelationWriter writer(path);
        write_large_payload_records(writer);
        const auto descriptor = writer.checkpoint_prefix();
        overwrite_with_oversized_first_record(path, descriptor);

        {
            OOCRelationPrefixReader reader(path, descriptor, writer);
            bool rejected_at_size_gate = false;
            try {
                (void)reader.read(0);
            } catch (const std::runtime_error& error) {
                rejected_at_size_gate =
                    std::string(error.what()).find("record exceeds persistence limit") !=
                    std::string::npos;
            }
            CHECK(rejected_at_size_gate);
        }
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

void test_fresh_writer_reserves_pair_without_clobbering() {
    {
        const std::string path = make_path("fresh_existing_index");
        OOCArtifacts cleanup(path);
        const std::string sentinel = "existing-index-sentinel";
        {
            std::ofstream index(path + ".relidx", std::ios::binary);
            CHECK(static_cast<bool>(index));
            index.write(sentinel.data(), static_cast<std::streamsize>(sentinel.size()));
        }

        bool rejected = false;
        try {
            OOCRelationWriter writer(path);
        } catch (const std::system_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(!std::filesystem::exists(path + ".reldata"));
        std::ifstream index(path + ".relidx", std::ios::binary);
        const std::string persisted((std::istreambuf_iterator<char>(index)),
                                    std::istreambuf_iterator<char>());
        CHECK(persisted == sentinel);
    }
    {
        const std::string path = make_path("fresh_existing_data");
        OOCArtifacts cleanup(path);
        const std::string sentinel = "existing-data-sentinel";
        {
            std::ofstream data(path + ".reldata", std::ios::binary);
            CHECK(static_cast<bool>(data));
            data.write(sentinel.data(), static_cast<std::streamsize>(sentinel.size()));
        }

        bool rejected = false;
        try {
            OOCRelationWriter writer(path);
        } catch (const std::system_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(!std::filesystem::exists(path + ".relidx"));
        std::ifstream data(path + ".reldata", std::ios::binary);
        const std::string persisted((std::istreambuf_iterator<char>(data)),
                                    std::istreambuf_iterator<char>());
        CHECK(persisted == sentinel);
    }
}

void test_concurrent_fresh_writers_have_one_durable_winner() {
    const std::string path = make_path("concurrent_fresh");
    OOCArtifacts cleanup(path);
    std::barrier start(3);
    std::atomic<int> successes{0};
    std::atomic<int> collisions{0};
    std::atomic<int> unexpected_failures{0};

    auto attempt = [&](int64_t a) {
        start.arrive_and_wait();
        try {
            OOCRelationWriter writer(path);
            CHECK(writer.write(make_relation(a, static_cast<uint64_t>(a + 1))) == 0);
            CHECK(writer.finalize().count == 1);
            successes.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::system_error&) {
            collisions.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            unexpected_failures.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread first(attempt, 11);
    std::thread second(attempt, 13);
    start.arrive_and_wait();
    first.join();
    second.join();

    CHECK(successes.load(std::memory_order_relaxed) == 1);
    CHECK(collisions.load(std::memory_order_relaxed) == 1);
    CHECK(unexpected_failures.load(std::memory_order_relaxed) == 0);
    OOCRelationReader reader(path);
    CHECK(reader.count() == 1);
    const auto relation = reader.read(0);
    CHECK(relation.a == 11 || relation.a == 13);
}

void test_cleanup_receipt_is_fresh_only_and_transactional() {
    {
        const std::string path = make_path("fresh_cleanup_receipt");
        OOCArtifacts cleanup(path);
        OOCRelationWriter writer(path);
        CHECK(writer.write(make_relation(1, 2)) == 0);
        CHECK(writer.write(make_relation(3, 4)) == 1);
        CHECK(writer.finalize().count == 2);

        const auto result = writer.remove_owned_artifacts_noexcept();
        CHECK(result.status == OOCCleanupStatus::Completed);
        CHECK(result.transaction_terminal());
        CHECK(!std::filesystem::exists(path + ".relidx"));
        CHECK(!std::filesystem::exists(path + ".reldata"));

        const auto repeated = writer.remove_owned_artifacts_noexcept();
        CHECK(repeated.status == OOCCleanupStatus::Completed);
        CHECK(repeated.transaction_terminal());
    }

    {
        const std::string path = make_path("cleanup_waits_for_prefix_reader");
        OOCArtifacts cleanup(path);
        OOCRelationWriter writer(path);
        CHECK(writer.write(make_relation(1, 2)) == 0);
        const auto descriptor = writer.checkpoint_prefix();
        {
            OOCRelationPrefixReader reader(path, descriptor, writer);
            CHECK(reader.count() == 1);
            const auto busy = writer.remove_owned_artifacts_noexcept();
            CHECK(busy.status == OOCCleanupStatus::Busy);
            CHECK(!busy.transaction_terminal());
            CHECK(std::filesystem::exists(path + ".relidx"));
            CHECK(std::filesystem::exists(path + ".reldata"));
        }
        CHECK(writer.remove_owned_artifacts_noexcept().completed());
        CHECK(!std::filesystem::exists(path + ".relidx"));
        CHECK(!std::filesystem::exists(path + ".reldata"));
    }

    {
        const std::string path = make_path("recovery_has_no_cleanup_receipt");
        OOCArtifacts cleanup(path);
        const auto descriptor = create_recovery_store(path);
        OOCRelationWriter recovered(path, descriptor, standard_sequence_receipt(descriptor.count));
        CHECK(recovered.recovery_outcome() == OOCRecoveryOutcome::AppendablePrefix);

        const auto rejected = recovered.remove_owned_artifacts_noexcept();
        CHECK(rejected.status == OOCCleanupStatus::InvalidRequest);
        CHECK(!rejected.transaction_terminal());
        CHECK(std::filesystem::exists(path + ".relidx"));
        CHECK(std::filesystem::exists(path + ".reldata"));
        recovered.abort();
    }
}

} // namespace

int main() {
    std::cout << "=== OOC Store Integrity Tests ===\n";
    test_shared_persistence_limits();
    test_persistence_rejects_zero_b();
    test_v3_fresh_checkpoint_prefix_resume_and_finalize_layout();
    test_validated_resume_handoff_and_append();
    test_resume_rejects_index_shape_corruption();
    test_resume_rejects_data_corruption();
    test_resume_rejects_v3_data_header_corruption_without_mutation();
    test_resume_rejects_same_size_foreign_v3_data_without_mutation();
    test_active_prefix_and_resume_reject_same_size_foreign_v3_data();
    test_suspended_finalize_rejects_duplicate_offset_without_mutation();
    test_resume_rejects_v2_prefix_without_mutation();
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
    test_v1_v2_finalized_reader_compatibility_and_paired_rejection();
    test_empty_v1_v2_finalized_reader_compatibility();
    test_finalized_reader_rejects_oversized_record_before_decode();
    test_finalized_reader_rejects_count_larger_than_data_extent();
    test_minimal_compact_record_extent_is_accepted();
    test_recovery_rejects_count_larger_than_data_extent();
    test_prefix_reader_exact_extent_and_lease();
    test_failed_snapshot_transition();
    test_fresh_writer_reserves_pair_without_clobbering();
    test_concurrent_fresh_writers_have_one_durable_winner();
    test_cleanup_receipt_is_fresh_only_and_transactional();
    std::cout << "All OOC store integrity tests passed!\n";
    return 0;
}
