// Cross-platform contracts for the chunk-compressed Krylov sequence store.

#include "gnfs/linalg/krylov_sequence_compressed.hpp"

#include "gnfs/linalg/krylov_compress.hpp"
#include "gnfs/util/native_random_access_file.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"
#include "support/test_check.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace {

using gnfs::linalg::KrylovCompressor;
using gnfs::linalg::KrylovSequenceCompressed;
using gnfs::util::NativeRandomAccessFile;

static_assert(!std::is_copy_constructible_v<KrylovSequenceCompressed>);
static_assert(!std::is_copy_assignable_v<KrylovSequenceCompressed>);
static_assert(std::is_nothrow_move_constructible_v<KrylovSequenceCompressed>);
static_assert(!std::is_move_assignable_v<KrylovSequenceCompressed>);

constexpr std::uint64_t kCanonicalLength = 5;
constexpr std::uint64_t kCanonicalChunkBlocks = 2;
constexpr std::uint64_t kIndexEntrySize = 16;
constexpr std::uint64_t kFlagOffset = 16;
constexpr std::uint64_t kLengthOffset = 24;
constexpr std::uint64_t kEntrySizeOffset = 32;
constexpr std::uint64_t kChunkBlocksOffset = 40;
constexpr std::uint64_t kChunkCountOffset = 48;
constexpr std::uint64_t kIndexOffset = 56;

using Record = std::array<std::uint8_t, 32>;

struct WireChunk final {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

struct WireLayout final {
    std::uint64_t length = 0;
    std::uint64_t entry_size = 0;
    std::uint64_t chunk_blocks = 0;
    std::uint64_t chunk_count = 0;
    std::uint64_t index_offset = 0;
    std::vector<WireChunk> chunks;
};

class TempDirectory final {
public:
    TempDirectory() {
        const std::string name = "gnfs_krylov_compression_" +
                                 std::to_string(gnfs::util::process_id()) + "_" +
                                 std::to_string(++sequence_);
        path_ = gnfs::util::temp_directory_path() / std::filesystem::path(name);
        std::error_code error;
        const bool created = std::filesystem::create_directory(path_, error);
        GNFS_TEST_CHECK(created);
        GNFS_TEST_CHECK(!error);
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
    inline static unsigned sequence_ = 0;
    std::filesystem::path path_;
};

[[nodiscard]] std::string utf8_string(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::u8string utf8 = path.u8string();
    std::string result;
    result.reserve(utf8.size());
    for (const char8_t byte : utf8) {
        result.push_back(static_cast<char>(byte));
    }
    return result;
#else
    return path.native();
#endif
}

void store_u64_le(std::byte* destination, std::uint64_t value) noexcept {
    for (unsigned byte = 0; byte < 8; ++byte) {
        destination[byte] = static_cast<std::byte>((value >> (byte * 8U)) & 0xFFU);
    }
}

[[nodiscard]] std::uint64_t load_u64_le(const std::byte* source) noexcept {
    std::uint64_t value = 0;
    for (unsigned byte = 0; byte < 8; ++byte) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(source[byte]))
                 << (byte * 8U);
    }
    return value;
}

template <typename Exception, typename Operation>
[[nodiscard]] std::string expect_exact_exception(Operation&& operation) {
    bool caught = false;
    std::string message;
    try {
        std::forward<Operation>(operation)();
    } catch (const Exception& error) {
        caught = true;
        GNFS_TEST_CHECK(typeid(error) == typeid(Exception));
        message = error.what();
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("unexpected exception type ") + typeid(error).name() +
                                 ": " + error.what());
    } catch (...) {
        GNFS_TEST_CHECK(false);
    }
    GNFS_TEST_CHECK(caught);
    return message;
}

[[nodiscard]] std::uint64_t read_u64(const std::filesystem::path& path, std::uint64_t offset) {
    auto file = NativeRandomAccessFile::open_read_only(path);
    std::array<std::byte, 8> encoded{};
    file.read_exact_at(offset, encoded);
    return load_u64_le(encoded.data());
}

void patch_u64(const std::filesystem::path& path, std::uint64_t offset, std::uint64_t value) {
    std::array<std::byte, 8> encoded{};
    store_u64_le(encoded.data(), value);
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    GNFS_TEST_CHECK(file.is_open());
    file.seekp(static_cast<std::streamoff>(offset));
    GNFS_TEST_CHECK(file.good());
    file.write(reinterpret_cast<const char*>(encoded.data()),
               static_cast<std::streamsize>(encoded.size()));
    GNFS_TEST_CHECK(file.good());
}

[[nodiscard]] std::uint8_t read_byte(const std::filesystem::path& path, std::uint64_t offset) {
    auto file = NativeRandomAccessFile::open_read_only(path);
    std::array<std::byte, 1> encoded{};
    file.read_exact_at(offset, encoded);
    return std::to_integer<std::uint8_t>(encoded[0]);
}

void patch_byte(const std::filesystem::path& path, std::uint64_t offset, std::uint8_t value) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    GNFS_TEST_CHECK(file.is_open());
    file.seekp(static_cast<std::streamoff>(offset));
    GNFS_TEST_CHECK(file.good());
    const char encoded = static_cast<char>(value);
    file.write(&encoded, 1);
    GNFS_TEST_CHECK(file.good());
}

void resize_test_file(const std::filesystem::path& path, std::uint64_t size) {
    std::error_code error;
    std::filesystem::resize_file(path, size, error);
    GNFS_TEST_CHECK(!error);
}

[[nodiscard]] std::vector<std::byte> read_file_bytes(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    GNFS_TEST_CHECK(size <= static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()));
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    auto file = NativeRandomAccessFile::open_read_only(path);
    file.read_exact_at(0, bytes);
    return bytes;
}

void write_sentinel(const std::filesystem::path& path) {
    constexpr std::string_view sentinel = "preexisting-compressed-sentinel";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    GNFS_TEST_CHECK(output.is_open());
    output.write(sentinel.data(), static_cast<std::streamsize>(sentinel.size()));
    GNFS_TEST_CHECK(output.good());
}

[[nodiscard]] WireLayout parse_layout(const std::filesystem::path& path) {
    WireLayout layout;
    layout.length = read_u64(path, kLengthOffset);
    layout.entry_size = read_u64(path, kEntrySizeOffset);
    layout.chunk_blocks = read_u64(path, kChunkBlocksOffset);
    layout.chunk_count = read_u64(path, kChunkCountOffset);
    layout.index_offset = read_u64(path, kIndexOffset);
    layout.chunks.reserve(static_cast<std::size_t>(layout.chunk_count));
    for (std::uint64_t chunk = 0; chunk < layout.chunk_count; ++chunk) {
        const std::uint64_t entry_offset = layout.index_offset + chunk * kIndexEntrySize;
        layout.chunks.push_back({read_u64(path, entry_offset), read_u64(path, entry_offset + 8)});
    }
    return layout;
}

[[nodiscard]] std::uint64_t codec_bound(std::uint64_t size) noexcept {
    return KrylovCompressor::HEADER_BYTES + size + (size + 127U) / 128U;
}

[[nodiscard]] std::array<Record, kCanonicalLength> make_records() {
    std::array<Record, kCanonicalLength> records{};
    for (std::size_t record = 0; record < records.size(); ++record) {
        for (std::size_t byte = 0; byte < records[record].size(); ++byte) {
            records[record][byte] = static_cast<std::uint8_t>(17U * record + 29U * byte + 3U);
        }
    }
    records.back().fill(0);
    for (std::size_t byte = 8; byte < records.back().size(); ++byte) {
        records.back()[byte] = static_cast<std::uint8_t>(byte - 7U);
    }
    return records;
}

class SuiteFixture final {
public:
    SuiteFixture()
        : canonical_path_(directory_.path() /
                          std::filesystem::path(u8"canonical_\u538b\u7f29_\u5e8f\u5217.kryz")),
          canonical_utf8_(utf8_string(canonical_path_)), records_(make_records()) {
        KrylovSequenceCompressed writer(canonical_utf8_.c_str(), kCanonicalLength, sizeof(Record),
                                        kCanonicalChunkBlocks, 64);
        GNFS_TEST_CHECK(writer.path() == canonical_utf8_);
        GNFS_TEST_CHECK(writer.filesystem_path() == canonical_path_);
        incomplete_flag_seen_ = read_u64(canonical_path_, kFlagOffset) == 1;
        for (std::uint64_t index = 0; index < kCanonicalLength; ++index) {
            writer.write_entry(index, records_[static_cast<std::size_t>(index)]);
        }
        writer.close();
        complete_flag_seen_ = read_u64(canonical_path_, kFlagOffset) == 0;
        layout_ = parse_layout(canonical_path_);
    }

    [[nodiscard]] std::filesystem::path clone(std::string_view label) const {
        const auto destination = directory_.path() / (std::string(label) + ".kryz");
        std::error_code error;
        const bool copied = std::filesystem::copy_file(
            canonical_path_, destination, std::filesystem::copy_options::overwrite_existing, error);
        GNFS_TEST_CHECK(copied);
        GNFS_TEST_CHECK(!error);
        return destination;
    }

    [[nodiscard]] const std::filesystem::path& directory() const noexcept {
        return directory_.path();
    }

    [[nodiscard]] const std::filesystem::path& canonical_path() const noexcept {
        return canonical_path_;
    }

    [[nodiscard]] const std::string& canonical_utf8() const noexcept {
        return canonical_utf8_;
    }

    [[nodiscard]] const std::array<Record, kCanonicalLength>& records() const noexcept {
        return records_;
    }

    [[nodiscard]] const WireLayout& layout() const noexcept {
        return layout_;
    }

    [[nodiscard]] bool incomplete_flag_seen() const noexcept {
        return incomplete_flag_seen_;
    }

    [[nodiscard]] bool complete_flag_seen() const noexcept {
        return complete_flag_seen_;
    }

private:
    TempDirectory directory_;
    std::filesystem::path canonical_path_;
    std::string canonical_utf8_;
    std::array<Record, kCanonicalLength> records_{};
    WireLayout layout_;
    bool incomplete_flag_seen_ = false;
    bool complete_flag_seen_ = false;
};

void expect_corrupt_file(const std::filesystem::path& path) {
    (void)expect_exact_exception<std::runtime_error>(
        [&] { (void)KrylovSequenceCompressed::open_readonly(path); });
}

void test_state_traits_and_construction_validation(const SuiteFixture& fixture) {
    std::cout << "Testing state traits and construction validation...\n";
    KrylovSequenceCompressed closed;
    GNFS_TEST_CHECK(!closed.is_open());
    closed.close();
    closed.remove_file();

    (void)expect_exact_exception<std::invalid_argument>([] {
        (void)KrylovSequenceCompressed(static_cast<const char*>(nullptr), 1, sizeof(Record));
    });
    (void)expect_exact_exception<std::invalid_argument>(
        [] { (void)KrylovSequenceCompressed::open_readonly(static_cast<const char*>(nullptr)); });

    const auto zero_path = fixture.directory() / "zero.kryz";
    (void)expect_exact_exception<std::invalid_argument>(
        [&] { (void)KrylovSequenceCompressed(zero_path, 0, sizeof(Record)); });
    GNFS_TEST_CHECK(!std::filesystem::exists(zero_path));

    const auto overflow_path = fixture.directory() / "overflow.kryz";
    write_sentinel(overflow_path);
    const auto sentinel = read_file_bytes(overflow_path);
    (void)expect_exact_exception<std::overflow_error>([&] {
        (void)KrylovSequenceCompressed(overflow_path, (std::numeric_limits<std::uint64_t>::max)(),
                                       2, 1);
    });
    GNFS_TEST_CHECK(read_file_bytes(overflow_path) == sentinel);

    const auto missing = fixture.directory() / "missing.kryz";
    const std::string message = expect_exact_exception<std::system_error>(
        [&] { (void)KrylovSequenceCompressed::open_readonly(missing); });
    GNFS_TEST_CHECK(message.find("open_read_only") != std::string::npos);

    const auto raii_path = fixture.directory() / "raii_complete.kryz";
    {
        KrylovSequenceCompressed writer(raii_path, 1, sizeof(Record), 1);
        writer.write_entry(0, fixture.records()[0]);
    }
    auto raii_reader = KrylovSequenceCompressed::open_readonly(raii_path);
    GNFS_TEST_CHECK(raii_reader.read_entry<Record>(0) == fixture.records()[0]);
    raii_reader.remove_file();

    const auto underfilled_raii_path = fixture.directory() / "raii_underfilled.kryz";
    {
        KrylovSequenceCompressed writer(underfilled_raii_path, 2, sizeof(Record), 1);
        writer.write_entry(0, fixture.records()[0]);
    }
    GNFS_TEST_CHECK(read_u64(underfilled_raii_path, kFlagOffset) == 1);
    expect_corrupt_file(underfilled_raii_path);
    std::error_code remove_error;
    GNFS_TEST_CHECK(std::filesystem::remove(underfilled_raii_path, remove_error));
    GNFS_TEST_CHECK(!remove_error);
}

void test_copy_roundtrip_chunk_boundaries_and_completion(const SuiteFixture& fixture) {
    std::cout << "Testing copy roundtrip, chunk boundaries, and completion...\n";
    GNFS_TEST_CHECK(fixture.incomplete_flag_seen());
    GNFS_TEST_CHECK(fixture.complete_flag_seen());

    auto reader = KrylovSequenceCompressed::open_readonly(fixture.canonical_utf8());
    GNFS_TEST_CHECK(reader.length() == kCanonicalLength);
    GNFS_TEST_CHECK(reader.entry_size() == sizeof(Record));
    GNFS_TEST_CHECK(reader.chunk_blocks() == kCanonicalChunkBlocks);
    GNFS_TEST_CHECK(reader.chunk_count() == 3);
    GNFS_TEST_CHECK(reader.total_uncompressed_bytes() == kCanonicalLength * sizeof(Record));

    constexpr std::array<std::uint64_t, kCanonicalLength> order{1, 2, 4, 0, 3};
    for (const std::uint64_t index : order) {
        const Record loaded = reader.read_entry<Record>(index);
        GNFS_TEST_CHECK(loaded == fixture.records()[static_cast<std::size_t>(index)]);
    }

    const WireLayout& layout = fixture.layout();
    GNFS_TEST_CHECK(layout.length == kCanonicalLength);
    GNFS_TEST_CHECK(layout.entry_size == sizeof(Record));
    GNFS_TEST_CHECK(layout.chunk_blocks == kCanonicalChunkBlocks);
    GNFS_TEST_CHECK(layout.chunk_count == 3);
    GNFS_TEST_CHECK(layout.chunks.size() == 3);
    for (std::size_t chunk = 0; chunk < layout.chunks.size(); ++chunk) {
        const std::uint64_t entries = chunk + 1 == layout.chunks.size() ? 1 : 2;
        GNFS_TEST_CHECK(layout.chunks[chunk].size >= KrylovCompressor::HEADER_BYTES);
        GNFS_TEST_CHECK(layout.chunks[chunk].size <= codec_bound(entries * sizeof(Record)));
    }
    GNFS_TEST_CHECK(std::filesystem::file_size(fixture.canonical_path()) ==
                    layout.index_offset + layout.chunk_count * kIndexEntrySize);
}

void test_write_preflight_and_mode_failures(const SuiteFixture& fixture) {
    std::cout << "Testing write preflight and mode failures...\n";
    const auto path = fixture.directory() / "preflight.kryz";
    KrylovSequenceCompressed writer(path, 3, sizeof(Record), 1);
    const auto initial_size = std::filesystem::file_size(path);
    std::array<std::byte, sizeof(Record) - 1> short_entry{};
    (void)expect_exact_exception<std::invalid_argument>(
        [&] { writer.write_entry(0, short_entry); });
    GNFS_TEST_CHECK(std::filesystem::file_size(path) == initial_size);

    const std::array<std::uint8_t, 16> wrong_typed{};
    (void)expect_exact_exception<std::invalid_argument>(
        [&] { writer.write_entry(0, wrong_typed); });
    GNFS_TEST_CHECK(std::filesystem::file_size(path) == initial_size);

    (void)expect_exact_exception<std::out_of_range>(
        [&] { writer.write_entry(3, fixture.records()[0]); });
    (void)expect_exact_exception<std::logic_error>(
        [&] { writer.write_entry(1, fixture.records()[1]); });
    GNFS_TEST_CHECK(std::filesystem::file_size(path) == initial_size);
    writer.write_entry(0, fixture.records()[0]);

    Record destination{};
    (void)expect_exact_exception<std::logic_error>(
        [&] { writer.read_entry(0, std::as_writable_bytes(std::span<Record>(&destination, 1))); });
    writer.remove_file();
    GNFS_TEST_CHECK(!std::filesystem::exists(path));

    auto reader = KrylovSequenceCompressed::open_readonly(fixture.canonical_path());
    (void)expect_exact_exception<std::logic_error>(
        [&] { reader.write_entry(0, fixture.records()[0]); });
    reader.close();
    (void)expect_exact_exception<std::logic_error>(
        [&] { reader.read_entry(0, std::as_writable_bytes(std::span<Record>(&destination, 1))); });
}

void test_exact_length_and_failed_writer_terminal(const SuiteFixture& fixture) {
    std::cout << "Testing exact-length commit and failed writer terminal state...\n";
    const auto path = fixture.directory() / "underfilled.kryz";
    KrylovSequenceCompressed writer(path, 3, sizeof(Record), 2);
    writer.write_entry(0, fixture.records()[0]);
    writer.write_entry(1, fixture.records()[1]);
    GNFS_TEST_CHECK(read_u64(path, kFlagOffset) == 1);

    (void)expect_exact_exception<std::logic_error>([&] { writer.close(); });
    GNFS_TEST_CHECK(!writer.is_open());
    GNFS_TEST_CHECK(read_u64(path, kFlagOffset) == 1);
    (void)expect_exact_exception<std::logic_error>(
        [&] { writer.write_entry(2, fixture.records()[2]); });
    writer.close();
    expect_corrupt_file(path);
    writer.remove_file();
    GNFS_TEST_CHECK(!std::filesystem::exists(path));
}

void test_read_copy_failure_and_failed_reader_terminal(const SuiteFixture& fixture) {
    std::cout << "Testing failed read copy guarantee and terminal state...\n";
    const auto path = fixture.clone("late_codec_failure");
    const WireChunk& last = fixture.layout().chunks.back();
    GNFS_TEST_CHECK((read_byte(path, last.offset + KrylovCompressor::HEADER_BYTES) & 0x80U) != 0);
    GNFS_TEST_CHECK(read_byte(path, last.offset + KrylovCompressor::HEADER_BYTES + 2) == 23U);
    patch_byte(path, last.offset + KrylovCompressor::HEADER_BYTES + 2, 0x7FU);

    auto reader = KrylovSequenceCompressed::open_readonly(path);
    Record destination{};
    destination.fill(0xA5U);
    const Record sentinel = destination;
    (void)expect_exact_exception<std::runtime_error>(
        [&] { reader.read_entry(4, std::as_writable_bytes(std::span<Record>(&destination, 1))); });
    GNFS_TEST_CHECK(destination == sentinel);
    GNFS_TEST_CHECK(!reader.is_open());
    (void)expect_exact_exception<std::logic_error>(
        [&] { reader.read_entry(0, std::as_writable_bytes(std::span<Record>(&destination, 1))); });
    reader.remove_file();
}

void test_header_corruption_table(const SuiteFixture& fixture) {
    std::cout << "Testing hostile header validation table...\n";
    using Mutator = std::function<void(const std::filesystem::path&)>;
    const std::vector<std::pair<std::string, Mutator>> cases{
        {"short_header", [](const auto& path) { resize_test_file(path, 63); }},
        {"bad_magic", [](const auto& path) { patch_u64(path, 0, 0); }},
        {"bad_version", [](const auto& path) { patch_u64(path, 8, 2); }},
        {"incomplete", [](const auto& path) { patch_u64(path, kFlagOffset, 1); }},
        {"zero_length", [](const auto& path) { patch_u64(path, kLengthOffset, 0); }},
        {"zero_entry", [](const auto& path) { patch_u64(path, kEntrySizeOffset, 0); }},
        {"zero_chunk", [](const auto& path) { patch_u64(path, kChunkBlocksOffset, 0); }},
        {"bad_count", [](const auto& path) { patch_u64(path, kChunkCountOffset, 4); }},
        {"dimension_overflow",
         [](const auto& path) {
             patch_u64(path, kLengthOffset, (std::numeric_limits<std::uint64_t>::max)());
             patch_u64(path, kEntrySizeOffset, 2);
             patch_u64(path, kChunkBlocksOffset, 1);
         }},
    };

    for (const auto& [label, mutate] : cases) {
        const auto path = fixture.clone(label);
        mutate(path);
        expect_corrupt_file(path);
    }
}

void test_index_and_file_corruption_table(const SuiteFixture& fixture) {
    std::cout << "Testing hostile index and file-extent validation table...\n";
    const WireLayout& layout = fixture.layout();
    const WireChunk first = layout.chunks.front();
    const WireChunk second = layout.chunks[1];
    const WireChunk last = layout.chunks.back();
    const std::uint64_t last_size_offset =
        layout.index_offset + (layout.chunk_count - 1) * kIndexEntrySize + 8;
    const std::uint64_t original_file_size = std::filesystem::file_size(fixture.canonical_path());

    using Mutator = std::function<void(const std::filesystem::path&)>;
    const std::vector<std::pair<std::string, Mutator>> cases{
        {"index_in_header", [](const auto& path) { patch_u64(path, kIndexOffset, 63); }},
        {"index_overflow",
         [](const auto& path) {
             patch_u64(path, kIndexOffset, (std::numeric_limits<std::uint64_t>::max)());
         }},
        {"truncated_index",
         [=](const auto& path) { resize_test_file(path, original_file_size - 1); }},
        {"trailing_bytes",
         [=](const auto& path) { resize_test_file(path, original_file_size + 1); }},
        {"payload_in_header", [=](const auto& path) { patch_u64(path, layout.index_offset, 63); }},
        {"payload_gap", [=](const auto& path) { patch_u64(path, layout.index_offset, 65); }},
        {"payload_overlap",
         [=](const auto& path) {
             patch_u64(path, layout.index_offset + kIndexEntrySize, first.offset);
         }},
        {"payload_too_short", [=](const auto& path) { patch_u64(path, last_size_offset, 15); }},
        {"payload_too_large",
         [=](const auto& path) {
             patch_u64(path, last_size_offset, codec_bound(sizeof(Record)) + 1);
         }},
        {"payload_extent_gap",
         [=](const auto& path) {
             GNFS_TEST_CHECK(last.size > KrylovCompressor::HEADER_BYTES);
             patch_u64(path, last_size_offset, last.size - 1);
         }},
    };

    GNFS_TEST_CHECK(second.offset == first.offset + first.size);
    for (const auto& [label, mutate] : cases) {
        const auto path = fixture.clone(label);
        mutate(path);
        expect_corrupt_file(path);
    }
}

void test_cache_eviction_and_copy_lifetime(const SuiteFixture& fixture) {
    std::cout << "Testing deterministic cache eviction and copy lifetime...\n";
    auto reader = KrylovSequenceCompressed::open_readonly(fixture.canonical_path(), 64);
    const Record saved = reader.read_entry<Record>(0);
    GNFS_TEST_CHECK(reader.read_entry<Record>(2) == fixture.records()[2]);
    GNFS_TEST_CHECK(reader.read_entry<Record>(0) == fixture.records()[0]);
    GNFS_TEST_CHECK(reader.read_entry<Record>(1) == fixture.records()[1]);
    GNFS_TEST_CHECK(saved == fixture.records()[0]);
    GNFS_TEST_CHECK(reader.cache_misses() == 3);
    GNFS_TEST_CHECK(reader.cache_hits() == 1);
}

void test_unicode_path_and_remove(const SuiteFixture& fixture) {
    std::cout << "Testing Unicode path ownership and removal...\n";
    const auto path = fixture.directory() /
                      std::filesystem::path(u8"\u53e6\u4e00\u4e2a_\u538b\u7f29_\u6587\u4ef6.kryz");
    std::error_code error;
    const bool copied = std::filesystem::copy_file(
        fixture.canonical_path(), path, std::filesystem::copy_options::overwrite_existing, error);
    GNFS_TEST_CHECK(copied);
    GNFS_TEST_CHECK(!error);

    auto reader = KrylovSequenceCompressed::open_readonly(path);
    GNFS_TEST_CHECK(reader.filesystem_path() == path);
    GNFS_TEST_CHECK(reader.path() == utf8_string(path));
    GNFS_TEST_CHECK(reader.read_entry<Record>(4) == fixture.records()[4]);
    reader.remove_file();
    GNFS_TEST_CHECK(!reader.is_open());
    GNFS_TEST_CHECK(!std::filesystem::exists(path));
    reader.remove_file();
}

void test_move_ownership(const SuiteFixture& fixture) {
    std::cout << "Testing move ownership for writer and reader...\n";
    const auto path = fixture.directory() / "move_owned.kryz";
    KrylovSequenceCompressed source(path, 3, sizeof(Record), 2);
    source.write_entry(0, fixture.records()[0]);
    KrylovSequenceCompressed writer(std::move(source));
    GNFS_TEST_CHECK(!source.is_open());
    source.remove_file();
    GNFS_TEST_CHECK(std::filesystem::exists(path));
    writer.write_entry(1, fixture.records()[1]);
    writer.write_entry(2, fixture.records()[2]);
    writer.close();

    auto reader_source = KrylovSequenceCompressed::open_readonly(path, 64);
    GNFS_TEST_CHECK(reader_source.read_entry<Record>(0) == fixture.records()[0]);
    KrylovSequenceCompressed reader(std::move(reader_source));
    GNFS_TEST_CHECK(!reader_source.is_open());
    reader_source.remove_file();
    GNFS_TEST_CHECK(std::filesystem::exists(path));
    GNFS_TEST_CHECK(reader.filesystem_path() == path);
    GNFS_TEST_CHECK(reader.cache_misses() == 1);
    GNFS_TEST_CHECK(reader.read_entry<Record>(1) == fixture.records()[1]);
    reader.remove_file();
    GNFS_TEST_CHECK(!std::filesystem::exists(path));
}

} // namespace

int main() {
    try {
        std::cout << "===== KrylovSequenceCompressed Tests =====\n";
        const SuiteFixture fixture;
        test_state_traits_and_construction_validation(fixture);
        test_copy_roundtrip_chunk_boundaries_and_completion(fixture);
        test_write_preflight_and_mode_failures(fixture);
        test_exact_length_and_failed_writer_terminal(fixture);
        test_read_copy_failure_and_failed_reader_terminal(fixture);
        test_header_corruption_table(fixture);
        test_index_and_file_corruption_table(fixture);
        test_cache_eviction_and_copy_lifetime(fixture);
        test_unicode_path_and_remove(fixture);
        test_move_ownership(fixture);
        std::cout << "===== All KrylovSequenceCompressed tests PASSED =====\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "KrylovSequenceCompressed tests FAILED: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "KrylovSequenceCompressed tests FAILED: unknown exception\n";
        return 1;
    }
}
