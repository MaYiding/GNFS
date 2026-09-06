// Unit tests for gnfs::util::MmapFile.
//
// Directly tests the RAII mmap wrapper: open, read, advise, move, destroy.
// Previously only indirectly tested through KrylovSequenceMmap +
// OOCRelationStore. Failure modes around empty files, missing files,
// large multi-page mappings, and move semantics are now isolated.

#include "gnfs/util/mmap_file.hpp"
#include "gnfs/util/owned_native_file.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace gnfs::util;

namespace {

[[noreturn]] void fail_check(const char* expression, int line) {
    throw std::runtime_error(std::string("CHECK failed at line ") + std::to_string(line) + ": " +
                             expression);
}

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fail_check(#expression, __LINE__);                                                     \
        }                                                                                          \
    } while (false)

// Generate a unique temp path per test invocation (PID + counter).
std::string make_temp_path(const char* tag) {
    static int counter = 0;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "gnfs_test_mmap_%d_%d_%s.bin", gnfs::util::process_id(),
                  counter++, tag);
    return gnfs::util::temp_path(buf);
}

void write_bytes(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    out.close();
}

// RAII guard: removes a file when the guard leaves scope.
struct FileGuard {
    std::string path;
    explicit FileGuard(std::string p) : path(std::move(p)) {}
    ~FileGuard() {
        std::remove(path.c_str());
    }
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
};

struct FilesystemPathGuard {
    std::filesystem::path path;

    explicit FilesystemPathGuard(std::filesystem::path value) : path(std::move(value)) {}

    ~FilesystemPathGuard() {
        std::error_code error;
        (void)std::filesystem::remove(path, error);
    }

    FilesystemPathGuard(const FilesystemPathGuard&) = delete;
    FilesystemPathGuard& operator=(const FilesystemPathGuard&) = delete;
};

std::string utf8_path_string(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    std::string result;
    result.reserve(utf8.size());
    for (const char8_t byte : utf8) {
        result.push_back(static_cast<char>(byte));
    }
    return result;
}

using TestNativeHandle = OwnedNativeFile::NativeHandle;

TestNativeHandle open_native_read_only(const std::string& path) {
#ifdef _WIN32
    const std::filesystem::path filesystem_path(path);
    HANDLE handle =
        ::CreateFileW(filesystem_path.c_str(), GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE);
    return handle;
#else
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDONLY);
    } while (descriptor < 0 && errno == EINTR);
    CHECK(descriptor >= 0);
    return descriptor;
#endif
}

TestNativeHandle open_native_write_only(const std::string& path) {
#ifdef _WIN32
    const std::filesystem::path filesystem_path(path);
    HANDLE handle =
        ::CreateFileW(filesystem_path.c_str(), GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE);
    return handle;
#else
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_WRONLY);
    } while (descriptor < 0 && errno == EINTR);
    CHECK(descriptor >= 0);
    return descriptor;
#endif
}

OwnedNativeFile open_owned_read_only(const std::string& path) {
    return OwnedNativeFile::adopt_ownership(open_native_read_only(path));
}

void check_native_handle_open(TestNativeHandle handle) {
#ifdef _WIN32
    DWORD flags = 0;
    CHECK(::GetHandleInformation(handle, &flags) != 0);
#else
    int result = -1;
    do {
        result = ::fcntl(handle, F_GETFD);
    } while (result < 0 && errno == EINTR);
    CHECK(result >= 0);
#endif
}

void check_native_handle_closed(TestNativeHandle handle) {
#ifdef _WIN32
    DWORD flags = 0;
    ::SetLastError(ERROR_SUCCESS);
    CHECK(::GetHandleInformation(handle, &flags) == 0);
    CHECK(::GetLastError() == ERROR_INVALID_HANDLE);
#else
    errno = 0;
    CHECK(::fcntl(handle, F_GETFD) == -1);
    CHECK(errno == EBADF);
#endif
}

#ifndef _WIN32
int find_fd_for_path(const std::string& path) {
    struct stat expected {};
    CHECK(::stat(path.c_str(), &expected) == 0);
    for (int descriptor = 0; descriptor < 1024; ++descriptor) {
        struct stat observed {};
        if (::fstat(descriptor, &observed) == 0 && observed.st_dev == expected.st_dev &&
            observed.st_ino == expected.st_ino) {
            return descriptor;
        }
    }
    return -1;
}
#endif

void replace_file(const std::string& source, const std::string& destination) {
#ifdef _WIN32
    const std::filesystem::path source_path(source);
    const std::filesystem::path destination_path(destination);
    if (::ReplaceFileW(destination_path.c_str(), source_path.c_str(), nullptr, 0, nullptr,
                       nullptr) == 0) {
        const DWORD error = ::GetLastError();
        throw std::system_error(std::error_code(static_cast<int>(error), std::system_category()),
                                "cannot replace test file");
    }
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    CHECK(!error);
#endif
}

static_assert(!std::is_copy_constructible_v<OwnedNativeFile>);
static_assert(!std::is_copy_assignable_v<OwnedNativeFile>);
static_assert(std::is_nothrow_move_constructible_v<OwnedNativeFile>);
static_assert(std::is_nothrow_move_assignable_v<OwnedNativeFile>);

} // namespace

void test_basic_open_and_read() {
    std::cout << "Testing MmapFile basic open and read..." << std::endl;

    std::string path = make_temp_path("basic");
    FileGuard guard(path);

    std::vector<uint8_t> payload = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    write_bytes(path, payload);

    MmapFile mf(path);
    assert(mf.is_open());
    assert(mf.size() == payload.size());
    assert(mf.data() != nullptr);

    // Byte-by-byte readback
    for (size_t i = 0; i < payload.size(); ++i) {
        assert(mf.data()[i] == payload[i]);
    }

    std::cout << "  basic open/read: PASS" << std::endl;
}

void test_empty_file() {
    std::cout << "Testing MmapFile empty file (size==0 valid path)..." << std::endl;

    std::string path = make_temp_path("empty");
    FileGuard guard(path);

    write_bytes(path, {}); // create empty file

    MmapFile mf(path);
    assert(mf.is_open()); // fd is open
    assert(mf.size() == 0);
    assert(mf.data() == nullptr); // no mapping for empty file

#ifndef _WIN32
    const int descriptor = find_fd_for_path(path);
    CHECK(descriptor >= 0);
    int descriptor_flags = -1;
    do {
        descriptor_flags = ::fcntl(descriptor, F_GETFD);
    } while (descriptor_flags < 0 && errno == EINTR);
    CHECK(descriptor_flags >= 0);
    CHECK((descriptor_flags & FD_CLOEXEC) != 0);
#endif

    std::cout << "  empty file: PASS" << std::endl;
}

void test_nonexistent_file_throws() {
    std::cout << "Testing MmapFile nonexistent file throws..." << std::endl;

    std::string path =
        gnfs::util::temp_path("gnfs_test_mmap_nonexistent_XXXXX_definitely_not_there.bin");
    // Make sure it doesn't exist
    std::remove(path.c_str());

    bool threw = false;
    try {
        MmapFile mf(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    (void)threw; // appease -Wunused-but-set-variable when NDEBUG defined

    std::cout << "  nonexistent throws: PASS" << std::endl;
}

void test_read_at_typed_values() {
    std::cout << "Testing MmapFile read_at typed values..." << std::endl;

    std::string path = make_temp_path("typed");
    FileGuard guard(path);

    // Layout: uint32_t header | int64_t value | uint16_t array of 3 elements
    std::vector<uint8_t> buf;
    auto append_u32 = [&buf](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            buf.push_back(static_cast<uint8_t>(v >> (i * 8)));
    };
    auto append_i64 = [&buf](int64_t v) {
        uint64_t u = static_cast<uint64_t>(v);
        for (int i = 0; i < 8; ++i)
            buf.push_back(static_cast<uint8_t>(u >> (i * 8)));
    };
    auto append_u16 = [&buf](uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v));
        buf.push_back(static_cast<uint8_t>(v >> 8));
    };

    append_u32(0xDEADBEEF);
    append_i64(-9223372036854775000ll); // near INT64_MIN
    append_u16(100);
    append_u16(200);
    append_u16(300);

    write_bytes(path, buf);

    MmapFile mf(path);
    assert(mf.size() == buf.size());

    [[maybe_unused]] uint32_t header = mf.read_at<uint32_t>(0);
    assert(header == 0xDEADBEEF);

    [[maybe_unused]] int64_t val = mf.read_at<int64_t>(4);
    assert(val == -9223372036854775000ll);

    [[maybe_unused]] uint16_t a = mf.read_at<uint16_t>(12);
    [[maybe_unused]] uint16_t b = mf.read_at<uint16_t>(14);
    [[maybe_unused]] uint16_t c = mf.read_at<uint16_t>(16);
    assert(a == 100);
    assert(b == 200);
    assert(c == 300);

    std::cout << "  read_at typed values: PASS" << std::endl;
}

void test_read_and_ptr_at_reject_out_of_bounds_offsets() {
    std::cout << "Testing MmapFile bounds checks avoid offset wraparound..." << std::endl;

    std::string path = make_temp_path("bounds");
    FileGuard guard(path);
    write_bytes(path, {0x11, 0x22, 0x33, 0x44});

    MmapFile mf(path);
    const size_t max_offset = (std::numeric_limits<size_t>::max)();

    bool read_threw = false;
    try {
        (void)mf.read_at<uint32_t>(max_offset);
    } catch (const std::out_of_range&) {
        read_threw = true;
    }
    CHECK(read_threw);

    bool short_read_threw = false;
    try {
        (void)mf.read_at<uint32_t>(1);
    } catch (const std::out_of_range&) {
        short_read_threw = true;
    }
    CHECK(short_read_threw);

    bool ptr_threw = false;
    try {
        (void)mf.ptr_at<uint8_t>(max_offset);
    } catch (const std::out_of_range&) {
        ptr_threw = true;
    }
    CHECK(ptr_threw);

    std::cout << "  out-of-bounds read/ptr offsets: PASS" << std::endl;
}

void test_ptr_at() {
    std::cout << "Testing MmapFile ptr_at offset..." << std::endl;

    std::string path = make_temp_path("ptr_at");
    FileGuard guard(path);

    std::vector<uint8_t> buf;
    // 4 uint32_t values at offsets 0, 4, 8, 12
    for (uint32_t v : {0x12345678u, 0xABCDEF01u, 0xDEADBEEFu, 0xCAFEBABEu}) {
        for (int i = 0; i < 4; ++i)
            buf.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    write_bytes(path, buf);

    MmapFile mf(path);
    [[maybe_unused]] const uint32_t* arr = mf.ptr_at<uint32_t>(0);
    assert(arr[0] == 0x12345678u);
    assert(arr[1] == 0xABCDEF01u);
    assert(arr[2] == 0xDEADBEEFu);
    assert(arr[3] == 0xCAFEBABEu);

    // Offset variant
    [[maybe_unused]] const uint32_t* arr_offset = mf.ptr_at<uint32_t>(8);
    assert(arr_offset[0] == 0xDEADBEEFu);
    assert(arr_offset[1] == 0xCAFEBABEu);

    std::cout << "  ptr_at: PASS" << std::endl;
}

void test_advise_random() {
    std::cout << "Testing MmapFile advise_random doesn't break access..." << std::endl;

    std::string path = make_temp_path("advise");
    FileGuard guard(path);

    std::vector<uint8_t> buf(64, 0);
    for (size_t i = 0; i < buf.size(); ++i)
        buf[i] = static_cast<uint8_t>(i);
    write_bytes(path, buf);

    MmapFile mf(path);
    mf.advise_random(); // switch hint

    // Data still readable
    for (size_t i = 0; i < buf.size(); ++i) {
        assert(mf.data()[i] == static_cast<uint8_t>(i));
    }

    std::cout << "  advise_random: PASS" << std::endl;
}

void test_move_semantics() {
    std::cout << "Testing MmapFile move semantics..." << std::endl;

    std::string path = make_temp_path("move");
    FileGuard guard(path);

    std::vector<uint8_t> buf = {0xCA, 0xFE, 0xBA, 0xBE};
    write_bytes(path, buf);

    MmapFile original(path);
    [[maybe_unused]] const uint8_t* orig_data = original.data();
    [[maybe_unused]] size_t orig_size = original.size();
    assert(orig_data != nullptr);
    assert(orig_size == 4);

    // Move constructor
    MmapFile moved(std::move(original));
    assert(moved.data() == orig_data);
    assert(moved.size() == orig_size);
    assert(moved.is_open());

    assert(original.data() == nullptr);
    assert(original.size() == 0);
    assert(!original.is_open());

    // Data readable from moved
    assert(moved.data()[0] == 0xCA);
    assert(moved.data()[3] == 0xBE);

    // Move assignment
    std::string path2 = make_temp_path("move2");
    FileGuard guard2(path2);
    write_bytes(path2, {0xDE, 0xAD});

    MmapFile other(path2);
    other = std::move(moved);
    assert(other.data() == orig_data);
    assert(other.size() == 4);
    assert(other.data()[0] == 0xCA);

    assert(moved.data() == nullptr);
    assert(!moved.is_open());

    std::cout << "  move semantics: PASS" << std::endl;
}

void test_close_idempotent() {
    std::cout << "Testing MmapFile close idempotent..." << std::endl;

    std::string path = make_temp_path("close");
    FileGuard guard(path);

    write_bytes(path, {0x42});

    MmapFile mf(path);
    assert(mf.is_open());

    mf.close();
    assert(!mf.is_open());
    assert(mf.data() == nullptr);
    assert(mf.size() == 0);

    // Second close should be no-op
    mf.close();
    assert(!mf.is_open());

    std::cout << "  close idempotent: PASS" << std::endl;
}

void test_large_multi_page() {
    std::cout << "Testing MmapFile multi-page roundtrip (256 KiB)..." << std::endl;

    std::string path = make_temp_path("large");
    FileGuard guard(path);

    constexpr size_t SIZE = 256 * 1024; // 256 KiB, span many pages
    std::vector<uint8_t> buf(SIZE);
    std::mt19937 rng(0xBEEFCAFE);
    for (size_t i = 0; i < SIZE; ++i) {
        buf[i] = static_cast<uint8_t>(rng() & 0xFF);
    }
    write_bytes(path, buf);

    MmapFile mf(path);
    assert(mf.size() == SIZE);

    // Spot-check: header, middle, tail
    assert(mf.data()[0] == buf[0]);
    assert(mf.data()[SIZE / 2] == buf[SIZE / 2]);
    assert(mf.data()[SIZE - 1] == buf[SIZE - 1]);

    // Full content match
    assert(std::memcmp(mf.data(), buf.data(), SIZE) == 0);

    // Switch advise + re-verify (random access patterns)
    mf.advise_random();
    assert(std::memcmp(mf.data(), buf.data(), SIZE) == 0);

    std::cout << "  large multi-page: PASS" << std::endl;
}

void test_default_constructed_state() {
    std::cout << "Testing MmapFile default-constructed state..." << std::endl;

    MmapFile mf;
    assert(!mf.is_open());
    assert(mf.data() == nullptr);
    assert(mf.size() == 0);

    // close() on default-constructed is safe
    mf.close();
    assert(!mf.is_open());

    std::cout << "  default constructed: PASS" << std::endl;
}

void test_owned_native_file_exact_handle_read() {
    std::cout << "Testing MmapFile consumes and reads the exact owned handle..." << std::endl;

    std::string path = make_temp_path("owned_exact");
    FileGuard guard(path);
    const std::vector<uint8_t> payload = {0x10, 0x20, 0x30, 0x40, 0x50};
    write_bytes(path, payload);

    OwnedNativeFile owned = open_owned_read_only(path);
    CHECK(owned.valid());

    MmapFile mapped(std::move(owned));
    CHECK(!owned.valid());
    CHECK(mapped.is_open());
    CHECK(mapped.size() == payload.size());
    CHECK(mapped.data() != nullptr);
    CHECK(std::memcmp(mapped.data(), payload.data(), payload.size()) == 0);

    std::cout << "  exact owned handle: PASS" << std::endl;
}

void test_owned_native_file_survives_path_replacement() {
    std::cout << "Testing owned-handle mapping ignores later path replacement..." << std::endl;

    std::string path = make_temp_path("owned_replaced");
    std::string replacement_path = make_temp_path("owned_replacement_source");
    FileGuard guard(path);
    FileGuard replacement_guard(replacement_path);

    const std::vector<uint8_t> original = {0xAA, 0xBB, 0xCC, 0xDD};
    const std::vector<uint8_t> replacement = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    write_bytes(path, original);

    OwnedNativeFile owned = open_owned_read_only(path);
    CHECK(owned.valid());

    write_bytes(replacement_path, replacement);
    replace_file(replacement_path, path);

    MmapFile mapped_owned(std::move(owned));
    CHECK(mapped_owned.size() == original.size());
    CHECK(std::memcmp(mapped_owned.data(), original.data(), original.size()) == 0);

    MmapFile mapped_path(path);
    CHECK(mapped_path.size() == replacement.size());
    CHECK(std::memcmp(mapped_path.data(), replacement.data(), replacement.size()) == 0);

    std::cout << "  path replacement isolation: PASS" << std::endl;
}

void test_owned_native_empty_file() {
    std::cout << "Testing empty owned native file..." << std::endl;

    std::string path = make_temp_path("owned_empty");
    FileGuard guard(path);
    write_bytes(path, {});

    OwnedNativeFile owned = open_owned_read_only(path);
    MmapFile mapped(std::move(owned));

    CHECK(!owned.valid());
    CHECK(mapped.is_open());
    CHECK(mapped.size() == 0);
    CHECK(mapped.data() == nullptr);

    std::cout << "  empty owned native file: PASS" << std::endl;
}

void test_owned_native_file_move_and_close() {
    std::cout << "Testing OwnedNativeFile move and close semantics..." << std::endl;

    std::string first_path = make_temp_path("owned_move_first");
    std::string second_path = make_temp_path("owned_move_second");
    FileGuard first_guard(first_path);
    FileGuard second_guard(second_path);
    write_bytes(first_path, {0x11});
    write_bytes(second_path, {0x22});

    const TestNativeHandle original_handle = open_native_read_only(first_path);
    OwnedNativeFile original = OwnedNativeFile::adopt_ownership(original_handle);
    OwnedNativeFile moved(std::move(original));
    CHECK(!original.valid());
    CHECK(moved.valid());
    check_native_handle_open(original_handle);

    const TestNativeHandle overwritten_handle = open_native_read_only(second_path);
    OwnedNativeFile assigned = OwnedNativeFile::adopt_ownership(overwritten_handle);
    CHECK(assigned.valid());
    assigned = std::move(moved);
    CHECK(!moved.valid());
    CHECK(assigned.valid());
    check_native_handle_closed(overwritten_handle);
    check_native_handle_open(original_handle);

    assigned.close();
    CHECK(!assigned.valid());
    check_native_handle_closed(original_handle);
    assigned.close();
    CHECK(!assigned.valid());

    std::cout << "  owned move/close: PASS" << std::endl;
}

void test_invalid_owned_native_file_rejected() {
    std::cout << "Testing invalid owned native file is rejected..." << std::endl;

    OwnedNativeFile invalid;
    CHECK(!invalid.valid());

    bool threw = false;
    try {
        MmapFile mapped(std::move(invalid));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(!invalid.valid());

    bool adopt_threw = false;
    try {
#ifdef _WIN32
        (void)OwnedNativeFile::adopt_ownership(INVALID_HANDLE_VALUE);
#else
        (void)OwnedNativeFile::adopt_ownership(-1);
#endif
    } catch (const std::invalid_argument&) {
        adopt_threw = true;
    }
    CHECK(adopt_threw);

    std::cout << "  invalid owned handle: PASS" << std::endl;
}

void test_owned_native_file_mapping_failure_closes_temporary_handle() {
    std::cout << "Testing failed owned-handle mapping closes temporary ownership..." << std::endl;

    std::string path = make_temp_path("owned_mapping_failure");
    FileGuard guard(path);
    write_bytes(path, {0x42});

    const TestNativeHandle write_only_handle = open_native_write_only(path);
    check_native_handle_open(write_only_handle);

    bool threw = false;
    try {
        MmapFile mapped(OwnedNativeFile::adopt_ownership(write_only_handle));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    check_native_handle_closed(write_only_handle);

    const TestNativeHandle retained_handle = open_native_write_only(path);
    OwnedNativeFile retained = OwnedNativeFile::adopt_ownership(retained_handle);
    threw = false;
    try {
        MmapFile mapped(std::move(retained));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(retained.valid());
    check_native_handle_open(retained_handle);
    retained.close();
    check_native_handle_closed(retained_handle);

    std::cout << "  mapping failure closes temporary handle: PASS" << std::endl;
}

void test_utf8_path() {
    std::cout << "Testing UTF-8 path mapping..." << std::endl;

    const auto path = gnfs::util::temp_directory_path() /
                      std::filesystem::path(std::u8string(u8"gnfs_test_mmap_\u6d4b\u8bd5.bin"));
    FilesystemPathGuard guard(path);
    const std::vector<uint8_t> payload = {0xA1, 0xB2, 0xC3, 0xD4};

    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        CHECK(output.is_open());
        output.write(reinterpret_cast<const char*>(payload.data()),
                     static_cast<std::streamsize>(payload.size()));
        CHECK(output.good());
    }

    MmapFile mapped(utf8_path_string(path));
    CHECK(mapped.is_open());
    CHECK(mapped.size() == payload.size());
    CHECK(std::memcmp(mapped.data(), payload.data(), payload.size()) == 0);

    std::cout << "  UTF-8 path mapping: PASS" << std::endl;
}

int main() {
    std::cout << "=== util/mmap_file.hpp tests ===" << std::endl;

    test_owned_native_file_exact_handle_read();
    test_owned_native_file_survives_path_replacement();
    test_owned_native_empty_file();
    test_owned_native_file_move_and_close();
    test_invalid_owned_native_file_rejected();
    test_owned_native_file_mapping_failure_closes_temporary_handle();
    test_read_and_ptr_at_reject_out_of_bounds_offsets();

#ifdef _WIN32
    test_default_constructed_state();
    test_utf8_path();
    return 0;
#else
    test_default_constructed_state();
    test_basic_open_and_read();
    test_empty_file();
    test_nonexistent_file_throws();
    test_read_at_typed_values();
    test_ptr_at();
    test_advise_random();
    test_move_semantics();
    test_close_idempotent();
    test_large_multi_page();
    test_utf8_path();

    std::cout << "\n=== All util/mmap_file.hpp tests PASSED ===" << std::endl;
    return 0;
#endif
}
