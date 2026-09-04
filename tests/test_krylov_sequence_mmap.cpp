#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "gnfs/linalg/krylov_sequence_mmap.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"
#include "support/test_check.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <istream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

using namespace gnfs::linalg;

namespace {

std::string unique_path(std::string_view label) {
    static std::uint64_t sequence = 0;
    return gnfs::util::temp_path("gnfs_test_krylov_" + std::to_string(gnfs::util::process_id()) +
                                 "_" + std::to_string(++sequence) + "_" + std::string(label) +
                                 ".kry");
}

std::filesystem::path native_path(const std::string& path) {
    return gnfs::linalg::detail::krylov_native_path_from_string(path);
}

std::string unique_utf8_path() {
    static std::uint64_t sequence = 0;
    const std::string filename = "gnfs_test_krylov_" + std::to_string(gnfs::util::process_id()) +
                                 "_" + std::to_string(++sequence) +
                                 "_utf8_\xE4\xB8\xAD\xE6\x96\x87.kry";
    return gnfs::linalg::detail::krylov_cached_path_string(gnfs::util::temp_directory_path() /
                                                           native_path(filename));
}

class PathCleanup {
public:
    explicit PathCleanup(std::string path) : path_(std::move(path)) {}

    ~PathCleanup() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove(native_path(path_), error);
        }
    }

    PathCleanup(const PathCleanup&) = delete;
    PathCleanup& operator=(const PathCleanup&) = delete;

private:
    std::string path_;
};

struct DenseGF2_64x64_Mock {
    std::array<std::uint64_t, 64> rows{};

    bool operator==(const DenseGF2_64x64_Mock&) const = default;
};

static_assert(sizeof(DenseGF2_64x64_Mock) == 512);
static_assert(alignof(DenseGF2_64x64_Mock) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<DenseGF2_64x64_Mock>);

bool path_exists(const std::string& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(native_path(path), error);
    GNFS_TEST_CHECK(!error);
    return exists;
}

std::uintmax_t checked_file_size(const std::string& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(native_path(path), error);
    GNFS_TEST_CHECK(!error);
    return size;
}

void remove_existing_file(const std::string& path) {
    std::error_code error;
    const bool removed = std::filesystem::remove(native_path(path), error);
    GNFS_TEST_CHECK(!error);
    GNFS_TEST_CHECK(removed);
}

template <typename T> T read_trivial(std::istream& input) {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    constexpr auto byte_count = static_cast<std::streamsize>(sizeof(T));
    input.read(reinterpret_cast<char*>(&value), byte_count);
    GNFS_TEST_CHECK(input.gcount() == byte_count);
    GNFS_TEST_CHECK(input.good());
    return value;
}

template <typename T> T read_trivial_at(std::ifstream& input, std::uint64_t offset) {
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    GNFS_TEST_CHECK(input.good());
    return read_trivial<T>(input);
}

void overwrite_u64(const std::string& path, std::streamoff offset, std::uint64_t value) {
    std::fstream output(native_path(path), std::ios::binary | std::ios::in | std::ios::out);
    GNFS_TEST_CHECK(output.is_open());
    output.seekp(offset, std::ios::beg);
    GNFS_TEST_CHECK(output.good());
    output.write(reinterpret_cast<const char*>(&value),
                 static_cast<std::streamsize>(sizeof(value)));
    output.flush();
    GNFS_TEST_CHECK(output.good());
}

template <typename Exception, typename Callable> bool throws_as(Callable&& callable) {
    try {
        std::forward<Callable>(callable)();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

template <typename Exception, typename Callable>
bool throws_with_message(Callable&& callable, std::string_view expected_message) {
    try {
        std::forward<Callable>(callable)();
    } catch (const Exception& error) {
        return std::string_view(error.what()).find(expected_message) != std::string_view::npos;
    }
    return false;
}

template <typename Exception, typename Callable>
bool throws_with_exact_message(Callable&& callable, std::string_view expected_message) {
    try {
        std::forward<Callable>(callable)();
    } catch (const Exception& error) {
        return std::string_view(error.what()) == expected_message;
    }
    return false;
}

#ifdef _WIN32

class ScopedWindowsHandle {
public:
    explicit ScopedWindowsHandle(HANDLE handle) : handle_(handle) {}

    ~ScopedWindowsHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
    }

    ScopedWindowsHandle(const ScopedWindowsHandle&) = delete;
    ScopedWindowsHandle& operator=(const ScopedWindowsHandle&) = delete;

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

struct ExclusiveOpenResult {
    bool opened = false;
    DWORD error = ERROR_SUCCESS;
};

ExclusiveOpenResult try_exclusive_open(const std::string& path) {
    const auto filesystem_path = native_path(path);
    const HANDLE handle = ::CreateFileW(filesystem_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        return {false, error};
    }
    ScopedWindowsHandle guard(handle);
    return {true, ERROR_SUCCESS};
}

#endif

void test_basic_typed_access() {
    std::cout << "Testing typed access (DenseGF2_64x64-shaped)...\n";

    const auto path = unique_path("typed");
    PathCleanup cleanup(path);
    constexpr std::uint64_t length = 100;
    constexpr std::uint64_t entry_size = sizeof(DenseGF2_64x64_Mock);

    KrylovSequenceMmap sequence(path, length, entry_size);
    GNFS_TEST_CHECK(sequence.is_open());
    GNFS_TEST_CHECK(sequence.length() == length);
    GNFS_TEST_CHECK(sequence.entry_size() == entry_size);
    GNFS_TEST_CHECK(sequence.path() == path);

    for (std::uint64_t k = 0; k < length; ++k) {
        auto& matrix = *sequence.at<DenseGF2_64x64_Mock>(k);
        for (std::size_t row = 0; row < matrix.rows.size(); ++row) {
            matrix.rows[row] = (k << 32U) | static_cast<std::uint64_t>(row);
        }
    }

    sequence.advise_random();
    const auto& const_sequence = sequence;
    for (std::uint64_t k = 0; k < length; ++k) {
        const auto& matrix = *const_sequence.at<DenseGF2_64x64_Mock>(k);
        for (std::size_t row = 0; row < matrix.rows.size(); ++row) {
            GNFS_TEST_CHECK(matrix.rows[row] == ((k << 32U) | static_cast<std::uint64_t>(row)));
        }
    }

    std::cout << "  Typed mutable/const access: PASS\n";
}

void test_persistent_roundtrip() {
    std::cout << "Testing persistent roundtrip...\n";

    const auto path = unique_path("persistent");
    PathCleanup cleanup(path);
    constexpr std::uint64_t length = 256;
    constexpr std::uint64_t entry_size = sizeof(DenseGF2_64x64_Mock);
    constexpr std::uint64_t expected_file_size =
        static_cast<std::uint64_t>(KrylovSequenceMmap::HEADER_SIZE) + length * entry_size;

    std::array<DenseGF2_64x64_Mock, 256> golden{};
    std::mt19937_64 random(12345);

    {
        KrylovSequenceMmap sequence(path, length, entry_size);
        GNFS_TEST_CHECK(sequence.length() == length);
        GNFS_TEST_CHECK(sequence.entry_size() == entry_size);

        for (std::uint64_t k = 0; k < length; ++k) {
            auto& matrix = *sequence.at<DenseGF2_64x64_Mock>(k);
            for (auto& row : matrix.rows) {
                row = random();
            }
            golden[static_cast<std::size_t>(k)] = matrix;
        }
        sequence.msync();
    }

    GNFS_TEST_CHECK(checked_file_size(path) == static_cast<std::uintmax_t>(expected_file_size));

    std::ifstream input(native_path(path), std::ios::binary);
    GNFS_TEST_CHECK(input.is_open());
    const auto header = read_trivial<std::array<std::uint64_t, 4>>(input);
    GNFS_TEST_CHECK(header[0] == KrylovSequenceMmap::MAGIC);
    GNFS_TEST_CHECK(header[1] == KrylovSequenceMmap::VERSION);
    GNFS_TEST_CHECK(header[2] == length);
    GNFS_TEST_CHECK(header[3] == entry_size);

    for (std::uint64_t k = 0; k < length; ++k) {
        const auto loaded = read_trivial<DenseGF2_64x64_Mock>(input);
        GNFS_TEST_CHECK(loaded == golden[static_cast<std::size_t>(k)]);
    }

    std::cout << "  Header and 256 entries persisted: PASS\n";
}

void test_validate_header() {
    std::cout << "Testing validate_header()...\n";

    const auto path = unique_path("validate");
    PathCleanup cleanup(path);

    {
        KrylovSequenceMmap sequence(path, 10, 64);
    }
    KrylovSequenceMmap::validate_header(path);

    overwrite_u64(path, 0, 0xDEADBEEFCAFEBABEULL);
    GNFS_TEST_CHECK(throws_with_message<std::runtime_error>(
        [&] { KrylovSequenceMmap::validate_header(path); }, "bad magic"));

    {
        KrylovSequenceMmap sequence(path, 10, 64);
    }
    overwrite_u64(path, static_cast<std::streamoff>(sizeof(std::uint64_t)),
                  KrylovSequenceMmap::VERSION + 1);
    GNFS_TEST_CHECK(throws_with_message<std::runtime_error>(
        [&] { KrylovSequenceMmap::validate_header(path); }, "version mismatch"));

    {
        std::ofstream output(native_path(path), std::ios::binary | std::ios::trunc);
        GNFS_TEST_CHECK(output.is_open());
        const std::uint64_t magic = KrylovSequenceMmap::MAGIC;
        output.write(reinterpret_cast<const char*>(&magic),
                     static_cast<std::streamsize>(sizeof(magic)));
        GNFS_TEST_CHECK(output.good());
    }
    GNFS_TEST_CHECK(throws_with_message<std::runtime_error>(
        [&] { KrylovSequenceMmap::validate_header(path); }, "short read"));

    std::cout << "  Valid, bad-magic, bad-version, and short headers: PASS\n";
}

void test_raw_byte_access() {
    std::cout << "Testing raw_at() byte access...\n";

    const auto path = unique_path("raw");
    PathCleanup cleanup(path);
    constexpr std::uint64_t length = 16;
    constexpr std::uint64_t entry_size = 17;

    KrylovSequenceMmap sequence(path, length, entry_size);
    for (std::uint64_t k = 0; k < length; ++k) {
        auto* bytes = sequence.raw_at(k);
        for (std::uint64_t index = 0; index < entry_size; ++index) {
            bytes[index] = static_cast<std::uint8_t>((k * 31U + index) & 0xFFU);
        }
    }

    const auto& const_sequence = sequence;
    for (std::uint64_t k = 0; k < length; ++k) {
        const auto* bytes = const_sequence.raw_at(k);
        for (std::uint64_t index = 0; index < entry_size; ++index) {
            const auto expected = static_cast<std::uint8_t>((k * 31U + index) & 0xFFU);
            GNFS_TEST_CHECK(bytes[index] == expected);
        }
    }

    std::cout << "  Mutable/const raw byte access: PASS\n";
}

void test_utf8_path() {
    std::cout << "Testing UTF-8 path handling...\n";

    const auto path = unique_utf8_path();
    PathCleanup cleanup(path);
    KrylovSequenceMmap sequence(path, 4, sizeof(std::uint64_t));
    GNFS_TEST_CHECK(sequence.is_open());
    GNFS_TEST_CHECK(sequence.path() == path);
    GNFS_TEST_CHECK(path_exists(path));

    sequence.raw_at(0)[0] = 0xA5U;
    sequence.msync();
    KrylovSequenceMmap::validate_header(path);
    sequence.remove_file();
    GNFS_TEST_CHECK(!path_exists(path));

    std::cout << "  UTF-8 create/validate/remove roundtrip: PASS\n";
}

void test_large_sequence() {
    std::cout << "Testing large sequence (2 MiB)...\n";

    const auto path = unique_path("large");
    PathCleanup cleanup(path);
    constexpr std::uint64_t length = 4096;
    constexpr std::uint64_t entry_size = sizeof(DenseGF2_64x64_Mock);
    constexpr std::uint64_t expected_file_size =
        static_cast<std::uint64_t>(KrylovSequenceMmap::HEADER_SIZE) + length * entry_size;
    constexpr std::array<std::uint64_t, 4> sample_positions{0, 1, length / 2, length - 1};

    static_assert(length * entry_size == 2U * 1024U * 1024U);

    {
        KrylovSequenceMmap sequence(path, length, entry_size);
        for (std::uint64_t k = 0; k < length; ++k) {
            auto& matrix = *sequence.at<DenseGF2_64x64_Mock>(k);
            for (std::size_t row = 0; row < matrix.rows.size(); ++row) {
                matrix.rows[row] = (k << 32U) | static_cast<std::uint64_t>(row);
            }
        }
        sequence.msync();
    }

    GNFS_TEST_CHECK(checked_file_size(path) == static_cast<std::uintmax_t>(expected_file_size));
    std::ifstream input(native_path(path), std::ios::binary);
    GNFS_TEST_CHECK(input.is_open());

    for (const auto k : sample_positions) {
        const std::uint64_t offset =
            static_cast<std::uint64_t>(KrylovSequenceMmap::HEADER_SIZE) + k * entry_size;
        const auto loaded = read_trivial_at<DenseGF2_64x64_Mock>(input, offset);
        GNFS_TEST_CHECK(loaded.rows.front() == (k << 32U));
        GNFS_TEST_CHECK(loaded.rows.back() == ((k << 32U) | 63U));
    }

    std::cout << "  File size, seek/read, and sampled entries: PASS\n";
}

void test_invalid_args() {
    std::cout << "Testing invalid construction arguments...\n";

    const auto zero_length_path = unique_path("zero_length");
    PathCleanup zero_length_cleanup(zero_length_path);
    GNFS_TEST_CHECK(!path_exists(zero_length_path));
    const bool zero_length_threw = throws_as<std::invalid_argument>(
        [&] { (void)KrylovSequenceMmap(zero_length_path, 0, 64); });
    GNFS_TEST_CHECK(!path_exists(zero_length_path));
    GNFS_TEST_CHECK(zero_length_threw);

    const auto zero_entry_path = unique_path("zero_entry");
    PathCleanup zero_entry_cleanup(zero_entry_path);
    GNFS_TEST_CHECK(!path_exists(zero_entry_path));
    const bool zero_entry_threw =
        throws_as<std::invalid_argument>([&] { (void)KrylovSequenceMmap(zero_entry_path, 10, 0); });
    GNFS_TEST_CHECK(!path_exists(zero_entry_path));
    GNFS_TEST_CHECK(zero_entry_threw);

    constexpr auto max_uint64 = (std::numeric_limits<std::uint64_t>::max)();
    constexpr std::uint64_t overflow_entry_size = 2;
    constexpr std::uint64_t overflow_length =
        (max_uint64 - static_cast<std::uint64_t>(KrylovSequenceMmap::HEADER_SIZE)) /
            overflow_entry_size +
        1;
    const auto overflow_path = unique_path("size_overflow");
    PathCleanup overflow_cleanup(overflow_path);
    GNFS_TEST_CHECK(!path_exists(overflow_path));
    const bool overflow_threw = throws_with_exact_message<std::overflow_error>(
        [&] { (void)KrylovSequenceMmap(overflow_path, overflow_length, overflow_entry_size); },
        "KrylovSequenceMmap: file size overflow");
    GNFS_TEST_CHECK(!path_exists(overflow_path));
    GNFS_TEST_CHECK(overflow_threw);

    using NativeFileOffset = detail::krylov_native_file_offset_t;
    constexpr auto native_max =
        static_cast<std::uint64_t>((std::numeric_limits<NativeFileOffset>::max)());
    static_assert(native_max >= KrylovSequenceMmap::HEADER_SIZE);
    static_assert(native_max < max_uint64);
    constexpr std::uint64_t native_overflow_length =
        native_max - static_cast<std::uint64_t>(KrylovSequenceMmap::HEADER_SIZE) + 1;
    const auto native_overflow_path = unique_path("native_offset_overflow");
    PathCleanup native_overflow_cleanup(native_overflow_path);
    GNFS_TEST_CHECK(!path_exists(native_overflow_path));
    const bool native_overflow_threw = throws_with_exact_message<std::overflow_error>(
        [&] { (void)KrylovSequenceMmap(native_overflow_path, native_overflow_length, 1); },
        "KrylovSequenceMmap: file too large for native file offset");
    GNFS_TEST_CHECK(!path_exists(native_overflow_path));
    GNFS_TEST_CHECK(native_overflow_threw);

    const auto truncation_path = unique_path("preflight_preserves_existing");
    PathCleanup truncation_cleanup(truncation_path);
    constexpr std::string_view sentinel = "GNFS Krylov size preflight";
    {
        std::ofstream output(native_path(truncation_path), std::ios::binary | std::ios::trunc);
        GNFS_TEST_CHECK(output.is_open());
        output.write(sentinel.data(), static_cast<std::streamsize>(sentinel.size()));
        GNFS_TEST_CHECK(output.good());
    }
    GNFS_TEST_CHECK(path_exists(truncation_path));
    const bool truncation_guard_threw = throws_with_exact_message<std::overflow_error>(
        [&] { (void)KrylovSequenceMmap(truncation_path, native_overflow_length, 1); },
        "KrylovSequenceMmap: file too large for native file offset");
    GNFS_TEST_CHECK(truncation_guard_threw);
    GNFS_TEST_CHECK(path_exists(truncation_path));
    GNFS_TEST_CHECK(checked_file_size(truncation_path) == sentinel.size());
    std::ifstream preserved_input(native_path(truncation_path), std::ios::binary);
    GNFS_TEST_CHECK(preserved_input.is_open());
    std::string preserved(sentinel.size(), '\0');
    preserved_input.read(preserved.data(), static_cast<std::streamsize>(preserved.size()));
    GNFS_TEST_CHECK(preserved_input.gcount() == static_cast<std::streamsize>(preserved.size()));
    GNFS_TEST_CHECK(std::string_view(preserved) == sentinel);

    std::cout << "  Invalid sizes fail before file creation or truncation: PASS\n";
}

void test_move_semantics() {
    std::cout << "Testing move semantics and old-resource release...\n";

    const auto source_path = unique_path("move_source");
    PathCleanup source_cleanup(source_path);
    KrylovSequenceMmap source(source_path, 10, 64);
    source.raw_at(3)[0] = 0xA5U;
    source.msync();
    GNFS_TEST_CHECK(source.is_open());

    KrylovSequenceMmap moved(std::move(source));
    GNFS_TEST_CHECK(!source.is_open());
    GNFS_TEST_CHECK(moved.is_open());
    GNFS_TEST_CHECK(moved.length() == 10);
    GNFS_TEST_CHECK(moved.entry_size() == 64);
    GNFS_TEST_CHECK(moved.path() == source_path);
    GNFS_TEST_CHECK(moved.raw_at(3)[0] == 0xA5U);

    const auto destination_path = unique_path("move_destination");
    PathCleanup destination_cleanup(destination_path);
    KrylovSequenceMmap destination(destination_path, 5, 32);
    destination.raw_at(1)[0] = 0x5AU;
    GNFS_TEST_CHECK(path_exists(destination_path));

#ifdef _WIN32
    const auto locked = try_exclusive_open(destination_path);
    GNFS_TEST_CHECK(!locked.opened);
    GNFS_TEST_CHECK(locked.error == ERROR_SHARING_VIOLATION);
#endif

    destination = std::move(moved);
    GNFS_TEST_CHECK(!moved.is_open());
    GNFS_TEST_CHECK(destination.is_open());
    GNFS_TEST_CHECK(destination.length() == 10);
    GNFS_TEST_CHECK(destination.entry_size() == 64);
    GNFS_TEST_CHECK(destination.path() == source_path);
    GNFS_TEST_CHECK(destination.raw_at(3)[0] == 0xA5U);

#ifdef _WIN32
    const auto released = try_exclusive_open(destination_path);
    GNFS_TEST_CHECK(released.opened);
    GNFS_TEST_CHECK(released.error == ERROR_SUCCESS);
#endif

    remove_existing_file(destination_path);
    GNFS_TEST_CHECK(!path_exists(destination_path));

    std::cout << "  Move construction/assignment and old target release: PASS\n";
}

void test_remove_file() {
    std::cout << "Testing remove_file()...\n";

    const auto path = unique_path("remove");
    PathCleanup cleanup(path);
    {
        KrylovSequenceMmap sequence(path, 4, 64);
        GNFS_TEST_CHECK(path_exists(path));
        sequence.remove_file();
        GNFS_TEST_CHECK(!path_exists(path));
#ifdef _WIN32
        GNFS_TEST_CHECK(!sequence.is_open());
#else
        GNFS_TEST_CHECK(sequence.is_open());
        sequence.raw_at(2)[0] = 0xC3U;
        GNFS_TEST_CHECK(sequence.raw_at(2)[0] == 0xC3U);
#endif
        sequence.remove_file();
        GNFS_TEST_CHECK(!path_exists(path));
    }

    std::cout << "  Path removal and idempotent retry: PASS\n";
}

} // namespace

int main() {
    try {
        std::cout << "===== KrylovSequenceMmap Tests =====\n";

        test_basic_typed_access();
        test_persistent_roundtrip();
        test_validate_header();
        test_raw_byte_access();
        test_utf8_path();
        test_large_sequence();
        test_invalid_args();
        test_move_semantics();
        test_remove_file();

        std::cout << "===== All KrylovSequenceMmap tests PASSED =====\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "KrylovSequenceMmap tests FAILED: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "KrylovSequenceMmap tests FAILED: unknown exception\n";
        return 1;
    }
}
