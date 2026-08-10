#include "gnfs/util/native_random_access_file.hpp"

#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"
#include "support/test_check.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace {

using gnfs::util::NativeRandomAccessFile;

static_assert(!std::is_copy_constructible_v<NativeRandomAccessFile>);
static_assert(!std::is_copy_assignable_v<NativeRandomAccessFile>);
static_assert(std::is_nothrow_move_constructible_v<NativeRandomAccessFile>);
static_assert(std::is_nothrow_move_assignable_v<NativeRandomAccessFile>);
static_assert(noexcept(std::declval<NativeRandomAccessFile&>().close()));

class TempDirectory final {
public:
    TempDirectory() {
        path_ = gnfs::util::temp_path("gnfs_native_random_access_" +
                                      std::to_string(gnfs::util::process_id()) + "_" +
                                      std::to_string(++sequence_));
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

template <typename Exception, typename Operation>
std::string capture_exception(Operation&& operation) {
    bool threw = false;
    std::string message;
    try {
        std::forward<Operation>(operation)();
    } catch (const Exception& error) {
        threw = true;
        message = error.what();
    }
    GNFS_TEST_CHECK(threw);
    return message;
}

void write_sentinel(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    GNFS_TEST_CHECK(output.is_open());
    constexpr std::string_view sentinel = "preexisting-sentinel";
    output.write(sentinel.data(), static_cast<std::streamsize>(sentinel.size()));
    GNFS_TEST_CHECK(output.good());
}

void test_default_and_closed_state() {
    std::cout << "Testing default and closed state..." << std::endl;
    NativeRandomAccessFile file;
    GNFS_TEST_CHECK(!file.is_open());
    file.close();
    file.close();

    std::array<std::byte, 1> byte{};
    GNFS_TEST_CHECK(capture_exception<std::logic_error>([&] { (void)file.size(); }) ==
                    "NativeRandomAccessFile::size: file is closed");
    GNFS_TEST_CHECK(capture_exception<std::logic_error>([&] { file.read_exact_at(0, byte); }) ==
                    "NativeRandomAccessFile::read_exact_at: file is closed");
    GNFS_TEST_CHECK(capture_exception<std::logic_error>([&] { file.write_exact_at(0, byte); }) ==
                    "NativeRandomAccessFile::write_exact_at: file is closed");
    GNFS_TEST_CHECK(capture_exception<std::logic_error>([&] { file.sync(); }) ==
                    "NativeRandomAccessFile::sync: file is closed");
}

void test_positioned_roundtrip_and_truncation() {
    std::cout << "Testing positioned roundtrip and truncation..." << std::endl;
    TempDirectory workspace;
    const auto path = workspace.path() / "roundtrip.bin";
    write_sentinel(path);

    auto writer = NativeRandomAccessFile::create_truncated(path);
    GNFS_TEST_CHECK(writer.is_open());
    GNFS_TEST_CHECK(writer.size() == 0);

    const std::array<std::byte, 4> head{std::byte{0x10}, std::byte{0x11}, std::byte{0x12},
                                        std::byte{0x13}};
    const std::array<std::byte, 4> middle{std::byte{0x20}, std::byte{0x21}, std::byte{0x22},
                                          std::byte{0x23}};
    const std::array<std::byte, 4> tail{std::byte{0x30}, std::byte{0x31}, std::byte{0x32},
                                        std::byte{0x33}};
    const std::array<std::byte, 12> expected{
        head[0],   head[1],   head[2], head[3], middle[0], middle[1],
        middle[2], middle[3], tail[0], tail[1], tail[2],   tail[3],
    };

    writer.write_exact_at(8, tail);
    writer.write_exact_at(4, middle);
    writer.write_exact_at(0, head);
    GNFS_TEST_CHECK(writer.size() == expected.size());

    std::array<std::byte, 12> writer_readback{};
    writer.read_exact_at(0, writer_readback);
    GNFS_TEST_CHECK(writer_readback == expected);
    writer.sync();
    writer.close();
    GNFS_TEST_CHECK(!writer.is_open());

    auto reader = NativeRandomAccessFile::open_read_only(path);
    GNFS_TEST_CHECK(reader.size() == expected.size());
    std::array<std::byte, 12> full_readback{};
    reader.read_exact_at(0, full_readback);
    GNFS_TEST_CHECK(full_readback == expected);
    std::array<std::byte, 4> middle_readback{};
    reader.read_exact_at(4, middle_readback);
    GNFS_TEST_CHECK(middle_readback == middle);

    GNFS_TEST_CHECK(capture_exception<std::logic_error>([&] { reader.write_exact_at(0, head); }) ==
                    "NativeRandomAccessFile::write_exact_at: file is read-only");
    GNFS_TEST_CHECK(capture_exception<std::logic_error>([&] { reader.sync(); }) ==
                    "NativeRandomAccessFile::sync: file is read-only");
}

void test_short_reads_and_range_checks() {
    std::cout << "Testing short reads and range checks..." << std::endl;
    TempDirectory workspace;
    const auto path = workspace.path() / "boundaries.bin";
    const std::array<std::byte, 4> content{std::byte{0xA0}, std::byte{0xA1}, std::byte{0xA2},
                                           std::byte{0xA3}};

    auto file = NativeRandomAccessFile::create_truncated(path);
    file.write_exact_at(0, content);

    std::array<std::byte, 1> one{};
    GNFS_TEST_CHECK(capture_exception<std::runtime_error>([&] {
                        file.read_exact_at(content.size(), one);
                    }) == "NativeRandomAccessFile::read_exact_at: short read");

    std::array<std::byte, 2> across_eof{};
    GNFS_TEST_CHECK(capture_exception<std::runtime_error>([&] {
                        file.read_exact_at(content.size() - 1, across_eof);
                    }) == "NativeRandomAccessFile::read_exact_at: short read");

    // The low 32 bits address content[1], so dropping OffsetHigh would make
    // this read succeed instead of reporting EOF.
    constexpr std::uint64_t high_offset = (std::uint64_t{1} << 32U) + 1U;
    GNFS_TEST_CHECK(capture_exception<std::runtime_error>([&] {
                        file.read_exact_at(high_offset, one);
                    }) == "NativeRandomAccessFile::read_exact_at: short read");

    GNFS_TEST_CHECK(capture_exception<std::overflow_error>([&] {
                        file.write_exact_at((std::numeric_limits<std::uint64_t>::max)(), one);
                    }) == "NativeRandomAccessFile::write_exact_at: range exceeds uint64_t");

    constexpr auto native_max =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    GNFS_TEST_CHECK(
        capture_exception<std::overflow_error>([&] { file.write_exact_at(native_max, one); }) ==
        "NativeRandomAccessFile::write_exact_at: range exceeds native file offset");

    const std::array<std::byte, 2> read_sentinel{std::byte{0x55}, std::byte{0x66}};
    auto uint64_overflow_read = read_sentinel;
    GNFS_TEST_CHECK(capture_exception<std::overflow_error>([&] {
                        file.read_exact_at((std::numeric_limits<std::uint64_t>::max)(),
                                           uint64_overflow_read);
                    }) == "NativeRandomAccessFile::read_exact_at: range exceeds uint64_t");
    GNFS_TEST_CHECK(uint64_overflow_read == read_sentinel);

    std::array<std::byte, 1> native_overflow_read{std::byte{0x77}};
    GNFS_TEST_CHECK(capture_exception<std::overflow_error>(
                        [&] { file.read_exact_at(native_max, native_overflow_read); }) ==
                    "NativeRandomAccessFile::read_exact_at: range exceeds native file offset");
    GNFS_TEST_CHECK(native_overflow_read[0] == std::byte{0x77});

    GNFS_TEST_CHECK(file.size() == content.size());
    std::array<std::byte, 4> preserved{};
    file.read_exact_at(0, preserved);
    GNFS_TEST_CHECK(preserved == content);

    const auto missing = workspace.path() / "missing.bin";
    const std::string missing_message = capture_exception<std::system_error>(
        [&] { (void)NativeRandomAccessFile::open_read_only(missing); });
    GNFS_TEST_CHECK(missing_message.starts_with(
        "NativeRandomAccessFile::open_read_only: native operation failed"));
}

void test_move_semantics() {
    std::cout << "Testing move semantics..." << std::endl;
    TempDirectory workspace;
    const auto source_path = workspace.path() / "source.bin";
    const auto destination_path = workspace.path() / "destination.bin";
    const std::array<std::byte, 3> content{std::byte{0x41}, std::byte{0x42}, std::byte{0x43}};

    auto source = NativeRandomAccessFile::create_truncated(source_path);
    source.write_exact_at(0, content);
    NativeRandomAccessFile moved(std::move(source));
    GNFS_TEST_CHECK(!source.is_open());
    GNFS_TEST_CHECK(moved.is_open());

    auto assigned = NativeRandomAccessFile::create_truncated(destination_path);
    assigned = std::move(moved);
    GNFS_TEST_CHECK(!moved.is_open());
    GNFS_TEST_CHECK(assigned.is_open());
    GNFS_TEST_CHECK(assigned.size() == content.size());
    assigned.sync();
    assigned.close();

    auto reader = NativeRandomAccessFile::open_read_only(source_path);
    std::array<std::byte, 3> readback{};
    reader.read_exact_at(0, readback);
    GNFS_TEST_CHECK(readback == content);
}

void test_unicode_path() {
    std::cout << "Testing Unicode path..." << std::endl;
    TempDirectory workspace;
    const auto path = workspace.path() /
                      std::filesystem::path(u8"gnfs_random_access_\u6d4b\u8bd5_\u6570\u636e.bin");
    const std::array<std::byte, 4> content{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                           std::byte{0xEF}};

    auto writer = NativeRandomAccessFile::create_truncated(path);
    writer.write_exact_at(0, content);
    writer.sync();
    writer.close();

    auto reader = NativeRandomAccessFile::open_read_only(path);
    std::array<std::byte, 4> readback{};
    reader.read_exact_at(0, readback);
    GNFS_TEST_CHECK(readback == content);
}

} // namespace

int main() {
    try {
        std::cout << "===== NativeRandomAccessFile Tests =====" << std::endl;
        test_default_and_closed_state();
        test_positioned_roundtrip_and_truncation();
        test_short_reads_and_range_checks();
        test_move_semantics();
        test_unicode_path();
        std::cout << "===== All NativeRandomAccessFile tests PASSED =====" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NativeRandomAccessFile tests FAILED: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "NativeRandomAccessFile tests FAILED: unknown exception\n";
        return 1;
    }
}
