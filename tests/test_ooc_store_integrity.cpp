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

using gnfs::core::ABPair;
using gnfs::core::PrimePower;
using gnfs::core::Relation;
using gnfs::relation::OOCRelationPrefixReader;
using gnfs::relation::OOCRelationReader;
using gnfs::relation::OOCRelationWriter;
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

void flip_to_incomplete(const std::string& base_path) {
    overwrite_u64(base_path + ".relidx", 0, OOCRelationWriter::MAGIC_INCOMPLETE);
}

void create_finalized_store(const std::string& base_path) {
    {
        OOCRelationWriter writer(base_path);
        CHECK(writer.write(make_relation(1, 2)) == 0);
        CHECK(writer.write(make_relation(3, 4)) == 1);
        CHECK(writer.finalize().count == 2);
    }
}

void create_valid_resume_store(const std::string& base_path) {
    create_finalized_store(base_path);
    flip_to_incomplete(base_path);
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

void expect_resume_rejected_without_mutation(const std::string& base_path) {
    const auto index_size = std::filesystem::file_size(base_path + ".relidx");
    const auto data_size = std::filesystem::file_size(base_path + ".reldata");

    bool rejected = false;
    try {
        OOCRelationWriter writer(base_path, true);
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
    create_valid_resume_store(path);

    OOCRelationWriter writer(path, true);
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
        create_valid_resume_store(path);
        const auto size = std::filesystem::file_size(path + ".relidx");
        CHECK(size > 0);
        std::filesystem::resize_file(path + ".relidx", size - 1);
        expect_resume_rejected_without_mutation(path);
    }
    {
        const std::string path = make_path("trailing_index");
        OOCArtifacts cleanup(path);
        create_valid_resume_store(path);
        std::ofstream index(path + ".relidx", std::ios::app | std::ios::binary);
        const uint64_t garbage = 0xBAD0FF5EULL;
        index.write(reinterpret_cast<const char*>(&garbage), sizeof(garbage));
        index.close();
        CHECK(static_cast<bool>(index));
        expect_resume_rejected_without_mutation(path);
    }
    {
        const std::string path = make_path("fake_count");
        OOCArtifacts cleanup(path);
        create_valid_resume_store(path);
        overwrite_u64(path + ".relidx", 8, 99);
        expect_resume_rejected_without_mutation(path);
    }
    {
        const std::string path = make_path("first_offset");
        OOCArtifacts cleanup(path);
        create_valid_resume_store(path);
        overwrite_u64(path + ".relidx", 16, 1);
        expect_resume_rejected_without_mutation(path);
    }
    {
        const std::string path = make_path("nonmonotonic_offset");
        OOCArtifacts cleanup(path);
        create_valid_resume_store(path);
        overwrite_u64(path + ".relidx", 24, 0);
        expect_resume_rejected_without_mutation(path);
    }
}

void test_resume_rejects_data_corruption() {
    {
        const std::string path = make_path("trailing_data");
        OOCArtifacts cleanup(path);
        create_valid_resume_store(path);
        std::ofstream data(path + ".reldata", std::ios::app | std::ios::binary);
        const uint8_t garbage = 0xA5;
        data.write(reinterpret_cast<const char*>(&garbage), sizeof(garbage));
        data.close();
        CHECK(static_cast<bool>(data));
        expect_resume_rejected_without_mutation(path);
    }
    {
        const std::string path = make_path("invalid_compact_record");
        OOCArtifacts cleanup(path);
        create_valid_resume_store(path);
        const uint32_t invalid_count = Relation::MAX_SERIALIZED_FACTORS + 1;
        overwrite_u32(path + ".reldata", 16, invalid_count);
        expect_resume_rejected_without_mutation(path);
    }
}

void test_finalized_reader_rejects_unreferenced_tail() {
    {
        const std::string path = make_path("finalized_low_count");
        OOCArtifacts cleanup(path);
        create_finalized_store(path);
        overwrite_u64(path + ".relidx", 8, 1);
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
        overwrite_u64(path + ".relidx", 16, 1);
        expect_finalized_reader_rejected(path);
    }
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
    test_finalized_reader_rejects_unreferenced_tail();
    test_prefix_reader_exact_extent_and_lease();
    test_failed_snapshot_transition();
    std::cout << "All OOC store integrity tests passed!\n";
    return 0;
}
