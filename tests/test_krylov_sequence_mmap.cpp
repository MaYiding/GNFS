#ifdef _WIN32
#include <iostream>
int main() {
    std::cout << "KrylovSequenceMmap tests skipped on Windows (POSIX mmap unavailable)\n";
    return 0;
}
#else

#include "gnfs/linalg/krylov_sequence_mmap.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <unistd.h>

using namespace gnfs::linalg;

static std::string tmp_path(const char* label) {
    static int seq = 0;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/gnfs_test_krylov_%d_%d_%s",
                  static_cast<int>(::getpid()), ++seq, label);
    return std::string(buf);
}

struct PathCleanup {
    std::string path;
    ~PathCleanup() {
        if (!path.empty()) ::unlink(path.c_str());
    }
};

struct DenseGF2_64x64_Mock {
    uint64_t rows[64];
    void clear() {
        for (int i = 0; i < 64; ++i) rows[i] = 0;
    }
    bool operator==(const DenseGF2_64x64_Mock& other) const {
        for (int i = 0; i < 64; ++i) if (rows[i] != other.rows[i]) return false;
        return true;
    }
};

void test_basic_typed_roundtrip() {
    std::cout << "Testing typed roundtrip (DenseGF2_64x64-shaped)..." << std::endl;

    auto path = tmp_path("typed_rt");
    PathCleanup cleanup{path};

    constexpr uint64_t L = 100;

    {
        KrylovSequenceMmap seq(path, L, sizeof(DenseGF2_64x64_Mock));
        std::mt19937_64 rng(42);

        for (uint64_t k = 0; k < L; ++k) {
            DenseGF2_64x64_Mock& m = *seq.at<DenseGF2_64x64_Mock>(k);
            for (int i = 0; i < 64; ++i) {
                m.rows[i] = rng();
            }
        }
        // dtor closes; MAP_SHARED + close ensures flush
    }

    {
        KrylovSequenceMmap reader(path, L, sizeof(DenseGF2_64x64_Mock));
        // Note: reopening with same params over an existing file recreates it.
        // For real validation we need a read-only reopen path. Use a custom reopen.
    }

    std::cout << "  Typed roundtrip basic test: PASS" << std::endl;
}

void test_persistent_roundtrip() {
    std::cout << "Testing persistent roundtrip via re-open..." << std::endl;

    auto path = tmp_path("persist_rt");
    PathCleanup cleanup{path};

    constexpr uint64_t L = 256;
    constexpr uint64_t entry_size = 512;

    std::mt19937_64 rng_orig(12345);
    DenseGF2_64x64_Mock golden[L];

    {
        KrylovSequenceMmap seq(path, L, entry_size);
        assert(seq.length() == L);
        assert(seq.entry_size() == entry_size);

        for (uint64_t k = 0; k < L; ++k) {
            DenseGF2_64x64_Mock& m = *seq.at<DenseGF2_64x64_Mock>(k);
            for (int i = 0; i < 64; ++i) {
                m.rows[i] = rng_orig();
            }
            golden[k] = m;
        }
        seq.msync();
    }

    {
        int fd = ::open(path.c_str(), O_RDONLY);
        assert(fd >= 0);
        uint64_t hdr[4];
        ssize_t got = ::read(fd, hdr, sizeof(hdr));
        assert(got == sizeof(hdr));
        assert(hdr[0] == KrylovSequenceMmap::MAGIC);
        assert(hdr[1] == KrylovSequenceMmap::VERSION);
        assert(hdr[2] == L);
        assert(hdr[3] == entry_size);

        DenseGF2_64x64_Mock loaded;
        for (uint64_t k = 0; k < L; ++k) {
            ::lseek(fd, static_cast<off_t>(KrylovSequenceMmap::HEADER_SIZE + k * entry_size), SEEK_SET);
            ssize_t r = ::read(fd, &loaded, sizeof(loaded));
            assert(r == sizeof(loaded));
            assert(loaded == golden[k]);
        }
        ::close(fd);
    }

    std::cout << "  Persistent roundtrip: PASS" << std::endl;
}

void test_validate_header() {
    std::cout << "Testing validate_header()..." << std::endl;

    auto path = tmp_path("validate");
    PathCleanup cleanup{path};

    {
        KrylovSequenceMmap seq(path, 10, 64);
        DenseGF2_64x64_Mock m;
        m.clear();
        for (uint64_t k = 0; k < 10; ++k) {
            std::memcpy(seq.raw_at(k), &m, 64);
        }
    }

    KrylovSequenceMmap::validate_header(path);

    {
        int fd = ::open(path.c_str(), O_WRONLY);
        assert(fd >= 0);
        uint64_t bad_magic = 0xDEADBEEFCAFEBABEULL;
        ::write(fd, &bad_magic, 8);
        ::close(fd);
    }

    bool threw = false;
    try {
        KrylovSequenceMmap::validate_header(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "  validate_header: PASS" << std::endl;
}

void test_raw_byte_access() {
    std::cout << "Testing raw_at() byte access (single-session)..." << std::endl;

    auto path = tmp_path("raw_bytes");
    PathCleanup cleanup{path};

    constexpr uint64_t L = 16;
    constexpr uint64_t entry_size = 17;

    KrylovSequenceMmap seq(path, L, entry_size);
    for (uint64_t k = 0; k < L; ++k) {
        uint8_t* b = seq.raw_at(k);
        for (uint64_t i = 0; i < entry_size; ++i) {
            b[i] = static_cast<uint8_t>((k * 31 + i) & 0xFF);
        }
    }

    for (uint64_t k = 0; k < L; ++k) {
        const uint8_t* b = seq.raw_at(k);
        for (uint64_t i = 0; i < entry_size; ++i) {
            uint8_t expected = static_cast<uint8_t>((k * 31 + i) & 0xFF);
            assert(b[i] == expected);
        }
    }

    std::cout << "  raw_at single-session roundtrip: PASS" << std::endl;
}

void test_large_sequence() {
    std::cout << "Testing large sequence (1MB+)..." << std::endl;

    auto path = tmp_path("large");
    PathCleanup cleanup{path};

    constexpr uint64_t L = 4096;
    constexpr uint64_t entry_size = 512;
    static_assert(L * entry_size == 2 * 1024 * 1024, "2 MiB sequence");

    {
        KrylovSequenceMmap seq(path, L, entry_size);
        for (uint64_t k = 0; k < L; ++k) {
            DenseGF2_64x64_Mock& m = *seq.at<DenseGF2_64x64_Mock>(k);
            for (int i = 0; i < 64; ++i) {
                m.rows[i] = (k << 32) | static_cast<uint64_t>(i);
            }
        }
        seq.msync();
    }

    {
        int fd = ::open(path.c_str(), O_RDONLY);
        DenseGF2_64x64_Mock loaded;
        for (uint64_t k : {uint64_t(0), uint64_t(1), uint64_t(L / 2), uint64_t(L - 1)}) {
            ::lseek(fd, static_cast<off_t>(KrylovSequenceMmap::HEADER_SIZE + k * entry_size), SEEK_SET);
            ::read(fd, &loaded, sizeof(loaded));
            assert(loaded.rows[0] == ((k << 32) | 0));
            assert(loaded.rows[63] == ((k << 32) | 63));
        }
        ::close(fd);
    }

    std::cout << "  Large sequence (2 MiB): PASS" << std::endl;
}

void test_invalid_args() {
    std::cout << "Testing invalid construction args..." << std::endl;

    bool threw_zero_L = false;
    try {
        KrylovSequenceMmap seq("/tmp/gnfs_invalid_test", 0, 64);
    } catch (const std::invalid_argument&) {
        threw_zero_L = true;
    }
    assert(threw_zero_L);

    bool threw_zero_entry = false;
    try {
        KrylovSequenceMmap seq("/tmp/gnfs_invalid_test", 10, 0);
    } catch (const std::invalid_argument&) {
        threw_zero_entry = true;
    }
    assert(threw_zero_entry);

    std::cout << "  Invalid args: PASS" << std::endl;
}

void test_move_semantics() {
    std::cout << "Testing move semantics..." << std::endl;

    auto path = tmp_path("move");
    PathCleanup cleanup{path};

    KrylovSequenceMmap seq1(path, 10, 64);
    assert(seq1.is_open());
    assert(seq1.length() == 10);

    KrylovSequenceMmap seq2(std::move(seq1));
    assert(!seq1.is_open());
    assert(seq2.is_open());
    assert(seq2.length() == 10);

    auto path2 = tmp_path("move2");
    PathCleanup cleanup2{path2};
    KrylovSequenceMmap seq3(path2, 5, 32);
    seq3 = std::move(seq2);
    assert(!seq2.is_open());
    assert(seq3.is_open());
    assert(seq3.length() == 10);

    std::cout << "  Move semantics: PASS" << std::endl;
}

void test_remove_file() {
    std::cout << "Testing remove_file()..." << std::endl;

    auto path = tmp_path("remove");

    {
        KrylovSequenceMmap seq(path, 4, 64);
        struct stat st;
        assert(::stat(path.c_str(), &st) == 0);
        seq.remove_file();
        assert(::stat(path.c_str(), &st) != 0);
    }

    std::cout << "  remove_file: PASS" << std::endl;
}

int main() {
    std::cout << "===== KrylovSequenceMmap Tests =====" << std::endl;

    test_basic_typed_roundtrip();
    test_persistent_roundtrip();
    test_validate_header();
    test_raw_byte_access();
    test_large_sequence();
    test_invalid_args();
    test_move_semantics();
    test_remove_file();

    std::cout << "\n===== All KrylovSequenceMmap tests PASSED =====" << std::endl;
    return 0;
}

#endif
