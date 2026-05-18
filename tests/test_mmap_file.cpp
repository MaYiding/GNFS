// Unit tests for gnfs::util::MmapFile.
//
// Directly tests the RAII mmap wrapper: open, read, advise, move, destroy.
// Previously only indirectly tested through KrylovSequenceMmap +
// OOCRelationStore. Failure modes around empty files, missing files,
// large multi-page mappings, and move semantics are now isolated.

#include "gnfs/util/mmap_file.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace gnfs::util;

namespace {

// Generate a unique temp path per test invocation (PID + counter).
std::string make_temp_path(const char* tag) {
    static int counter = 0;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/gnfs_test_mmap_%d_%d_%s.bin",
                  static_cast<int>(getpid()), counter++, tag);
    return std::string(buf);
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
    ~FileGuard() { std::remove(path.c_str()); }
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
};

}  // namespace

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

    write_bytes(path, {});  // create empty file

    MmapFile mf(path);
    assert(mf.is_open());   // fd is open
    assert(mf.size() == 0);
    assert(mf.data() == nullptr);  // no mapping for empty file

    std::cout << "  empty file: PASS" << std::endl;
}

void test_nonexistent_file_throws() {
    std::cout << "Testing MmapFile nonexistent file throws..." << std::endl;

    std::string path = "/tmp/gnfs_test_mmap_nonexistent_XXXXX_definitely_not_there.bin";
    // Make sure it doesn't exist
    std::remove(path.c_str());

    bool threw = false;
    try {
        MmapFile mf(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    (void)threw;  // appease -Wunused-but-set-variable when NDEBUG defined

    std::cout << "  nonexistent throws: PASS" << std::endl;
}

void test_read_at_typed_values() {
    std::cout << "Testing MmapFile read_at typed values..." << std::endl;

    std::string path = make_temp_path("typed");
    FileGuard guard(path);

    // Layout: uint32_t header | int64_t value | uint16_t array of 3 elements
    std::vector<uint8_t> buf;
    auto append_u32 = [&buf](uint32_t v) {
        for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>(v >> (i * 8)));
    };
    auto append_i64 = [&buf](int64_t v) {
        uint64_t u = static_cast<uint64_t>(v);
        for (int i = 0; i < 8; ++i) buf.push_back(static_cast<uint8_t>(u >> (i * 8)));
    };
    auto append_u16 = [&buf](uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v));
        buf.push_back(static_cast<uint8_t>(v >> 8));
    };

    append_u32(0xDEADBEEF);
    append_i64(-9223372036854775000ll);  // near INT64_MIN
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

void test_ptr_at() {
    std::cout << "Testing MmapFile ptr_at offset..." << std::endl;

    std::string path = make_temp_path("ptr_at");
    FileGuard guard(path);

    std::vector<uint8_t> buf;
    // 4 uint32_t values at offsets 0, 4, 8, 12
    for (uint32_t v : {0x12345678u, 0xABCDEF01u, 0xDEADBEEFu, 0xCAFEBABEu}) {
        for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>(v >> (i * 8)));
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
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(i);
    write_bytes(path, buf);

    MmapFile mf(path);
    mf.advise_random();  // switch hint

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

    constexpr size_t SIZE = 256 * 1024;  // 256 KiB, span many pages
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

int main() {
    std::cout << "=== util/mmap_file.hpp tests ===" << std::endl;

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

    std::cout << "\n=== All util/mmap_file.hpp tests PASSED ===" << std::endl;
    return 0;
}
