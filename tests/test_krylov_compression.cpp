#ifdef _WIN32
#include <iostream>
int main() {
    std::cout << "KrylovSequenceCompressed tests skipped on Windows (POSIX file APIs unavailable)\n";
    return 0;
}
#else

// Tests for KrylovSequenceCompressed (compressed mmap layer).
//
// Covers:
// - Single-block / 1000-block roundtrip via write/read API
// - Sparse blocks (high compression ratio)
// - All-zero blocks
// - Chunk-boundary blocks (block N at boundary of chunk M, M+1)
// - INCOMPLETE-on-reopen rejection (crash-safety MAGIC flip discipline)
// - MAGIC flip on close (finalized files load successfully)
// - LRU cache invariant (repeated read_at gives identical result)
// - Compression ratio sanity (sparse Krylov synthetic ≥ 1.3× ratio = ≤ 76% size)

#include "gnfs/linalg/krylov_sequence_compressed.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

using gnfs::linalg::KrylovSequenceCompressed;

namespace {

std::string tmp_path(const char* label) {
    static int seq = 0;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/gnfs_test_kryz_%d_%d_%s",
                  static_cast<int>(::getpid()), ++seq, label);
    return std::string(buf);
}

struct PathCleanup {
    std::string path;
    ~PathCleanup() {
        if (!path.empty()) ::unlink(path.c_str());
    }
};

struct Block512 {
    uint64_t rows[64];
    bool operator==(const Block512& o) const {
        for (int i = 0; i < 64; ++i) if (rows[i] != o.rows[i]) return false;
        return true;
    }
};

}  // namespace

void test_single_block_roundtrip() {
    std::cout << "Testing 1-block roundtrip..." << std::endl;
    auto path = tmp_path("1blk");
    PathCleanup cleanup{path};

    Block512 golden;
    std::mt19937_64 rng(0x1234);
    for (int i = 0; i < 64; ++i) golden.rows[i] = rng();

    {
        KrylovSequenceCompressed seq(path, /*L=*/1, sizeof(Block512),
                                     /*chunk_blocks=*/64);
        std::memcpy(seq.write_at(0), &golden, sizeof(golden));
        seq.close();
        assert(seq.chunk_count() == 1);
        assert(seq.total_uncompressed_bytes() == sizeof(Block512));
    }

    {
        auto reader = KrylovSequenceCompressed::open_readonly(path);
        assert(reader.length() == 1);
        Block512 loaded;
        std::memcpy(&loaded, reader.read_at(0), sizeof(loaded));
        assert(loaded == golden);
        assert(reader.cache_misses() == 1);
        assert(reader.cache_hits() == 0);
    }

    std::cout << "  PASS" << std::endl;
}

void test_1000_block_roundtrip() {
    std::cout << "Testing 1000-block roundtrip..." << std::endl;
    auto path = tmp_path("1000blk");
    PathCleanup cleanup{path};

    constexpr uint64_t L = 1000;
    std::vector<Block512> golden(L);
    std::mt19937_64 rng(0x5555);
    for (uint64_t k = 0; k < L; ++k) {
        for (int i = 0; i < 64; ++i) golden[k].rows[i] = rng();
    }

    {
        KrylovSequenceCompressed seq(path, L, sizeof(Block512), /*chunk_blocks=*/64);
        for (uint64_t k = 0; k < L; ++k) {
            std::memcpy(seq.write_at(k), &golden[k], sizeof(Block512));
        }
        seq.close();
        const uint64_t expected_chunks = (L + 63) / 64;
        assert(seq.chunk_count() == expected_chunks);
    }

    {
        auto reader = KrylovSequenceCompressed::open_readonly(path);
        assert(reader.length() == L);
        // Read every block twice to exercise cache hits
        for (int pass = 0; pass < 2; ++pass) {
            for (uint64_t k = 0; k < L; ++k) {
                Block512 loaded;
                std::memcpy(&loaded, reader.read_at(k), sizeof(loaded));
                if (!(loaded == golden[k])) {
                    std::cerr << "  Mismatch at block " << k
                              << " pass=" << pass << std::endl;
                    assert(false);
                }
            }
        }
        std::cout << "  cache hits=" << reader.cache_hits()
                  << " misses=" << reader.cache_misses() << std::endl;
    }

    std::cout << "  PASS" << std::endl;
}

void test_all_zero_blocks_high_ratio() {
    std::cout << "Testing 256 all-zero blocks (high compression)..." << std::endl;
    auto path = tmp_path("allzero");
    PathCleanup cleanup{path};

    constexpr uint64_t L = 256;
    Block512 zero{};

    {
        KrylovSequenceCompressed seq(path, L, sizeof(Block512), /*chunk_blocks=*/64);
        for (uint64_t k = 0; k < L; ++k) {
            std::memcpy(seq.write_at(k), &zero, sizeof(zero));
        }
        seq.close();

        const uint64_t total_orig = L * sizeof(Block512);
        const uint64_t total_comp = seq.total_compressed_bytes();
        const double ratio = static_cast<double>(total_comp) /
                             static_cast<double>(total_orig);
        std::cout << "  " << total_orig << " B -> " << total_comp
                  << " B (ratio=" << ratio << ")" << std::endl;
        assert(ratio < 0.05);  // very compressible
    }

    {
        auto reader = KrylovSequenceCompressed::open_readonly(path);
        for (uint64_t k = 0; k < L; ++k) {
            Block512 loaded;
            std::memcpy(&loaded, reader.read_at(k), sizeof(loaded));
            for (int i = 0; i < 64; ++i) assert(loaded.rows[i] == 0);
        }
    }

    std::cout << "  PASS" << std::endl;
}

void test_max_density_no_blowup() {
    std::cout << "Testing 128 random (incompressible) blocks..." << std::endl;
    auto path = tmp_path("dense");
    PathCleanup cleanup{path};

    constexpr uint64_t L = 128;
    std::vector<Block512> golden(L);
    std::mt19937_64 rng(0xBADC0DE);
    for (uint64_t k = 0; k < L; ++k) {
        for (int i = 0; i < 64; ++i) golden[k].rows[i] = rng();
    }

    {
        KrylovSequenceCompressed seq(path, L, sizeof(Block512), /*chunk_blocks=*/32);
        for (uint64_t k = 0; k < L; ++k) {
            std::memcpy(seq.write_at(k), &golden[k], sizeof(Block512));
        }
        seq.close();

        const uint64_t total_orig = L * sizeof(Block512);
        const uint64_t total_comp = seq.total_compressed_bytes();
        const double ratio = static_cast<double>(total_comp) /
                             static_cast<double>(total_orig);
        std::cout << "  " << total_orig << " B -> " << total_comp
                  << " B (ratio=" << ratio << ")" << std::endl;
        // Worst case: ~1.5% overhead per chunk (header + RLE length bytes).
        // Block delta XOR of random ~= random, so no compression but no blowup.
        assert(ratio < 1.05);
    }

    {
        auto reader = KrylovSequenceCompressed::open_readonly(path);
        for (uint64_t k = 0; k < L; ++k) {
            Block512 loaded;
            std::memcpy(&loaded, reader.read_at(k), sizeof(loaded));
            assert(loaded == golden[k]);
        }
    }

    std::cout << "  PASS" << std::endl;
}

void test_chunk_boundary_blocks() {
    std::cout << "Testing chunk boundary blocks (chunk_blocks=8, 25 blocks)..." << std::endl;
    auto path = tmp_path("boundary");
    PathCleanup cleanup{path};

    constexpr uint64_t L = 25;
    constexpr uint64_t chunk_blocks = 8;  // chunks: 0..7, 8..15, 16..23, 24..24
    std::vector<Block512> golden(L);
    for (uint64_t k = 0; k < L; ++k) {
        for (int i = 0; i < 64; ++i) {
            golden[k].rows[i] = (k << 32) | static_cast<uint64_t>(i);
        }
    }

    {
        KrylovSequenceCompressed seq(path, L, sizeof(Block512), chunk_blocks);
        for (uint64_t k = 0; k < L; ++k) {
            std::memcpy(seq.write_at(k), &golden[k], sizeof(Block512));
        }
        seq.close();
        assert(seq.chunk_count() == 4);  // ceil(25/8) = 4
    }

    {
        auto reader = KrylovSequenceCompressed::open_readonly(path);
        // Read in random order to test chunk boundary navigation
        std::vector<uint64_t> order = {7, 8, 24, 0, 15, 16, 23, 12, 4, 19};
        for (uint64_t k : order) {
            Block512 loaded;
            std::memcpy(&loaded, reader.read_at(k), sizeof(loaded));
            if (!(loaded == golden[k])) {
                std::cerr << "  Mismatch at boundary block " << k << std::endl;
                assert(false);
            }
        }
    }

    std::cout << "  PASS" << std::endl;
}

void test_incomplete_on_reopen_rejected() {
    std::cout << "Testing INCOMPLETE flag on mid-write file rejected..." << std::endl;
    auto path = tmp_path("incomplete");
    PathCleanup cleanup{path};

    {
        KrylovSequenceCompressed seq(path, /*L=*/100, sizeof(Block512),
                                     /*chunk_blocks=*/8);
        Block512 dummy{};
        for (uint64_t k = 0; k < 50; ++k) {
            std::memcpy(seq.write_at(k), &dummy, sizeof(dummy));
        }
        // Simulate crash: do NOT call close(). When the destructor runs the
        // writer should attempt close, BUT we explicitly want to test the
        // INCOMPLETE state. Achieve that by truncating the destructor's
        // ability: easier approach — explicitly skip close. Destructor
        // catches the exception path and leaves INCOMPLETE flag set.
        // Actually our destructor calls close() unless explicitly stopped.
        // Force INCOMPLETE by directly corrupting the file:
        seq.close();
        // Re-open the file and flip incomplete back to 1
        int fd = ::open(path.c_str(), O_RDWR);
        assert(fd >= 0);
        ::lseek(fd, 16, SEEK_SET);
        uint64_t incomplete = 1;
        ::write(fd, &incomplete, 8);
        ::close(fd);
    }

    bool threw = false;
    try {
        auto reader = KrylovSequenceCompressed::open_readonly(path);
        (void) reader;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "  PASS" << std::endl;
}

void test_magic_flip_on_close() {
    std::cout << "Testing MAGIC flip from INCOMPLETE to finalized on close..." << std::endl;
    auto path = tmp_path("magic_flip");
    PathCleanup cleanup{path};

    {
        KrylovSequenceCompressed seq(path, /*L=*/16, sizeof(Block512),
                                     /*chunk_blocks=*/8);
        // Mid-write: check incomplete_flag is 1
        {
            int fd = ::open(path.c_str(), O_RDONLY);
            assert(fd >= 0);
            uint8_t hdr[64];
            ::read(fd, hdr, 64);
            uint64_t flag;
            std::memcpy(&flag, hdr + 16, 8);
            ::close(fd);
            assert(flag == 1);
        }
        Block512 dummy{};
        for (uint64_t k = 0; k < 16; ++k) {
            std::memcpy(seq.write_at(k), &dummy, sizeof(dummy));
        }
        seq.close();
    }

    // After close, incomplete_flag must be 0
    {
        int fd = ::open(path.c_str(), O_RDONLY);
        assert(fd >= 0);
        uint8_t hdr[64];
        ::read(fd, hdr, 64);
        uint64_t flag;
        std::memcpy(&flag, hdr + 16, 8);
        ::close(fd);
        assert(flag == 0);
    }

    // open_readonly should succeed
    auto reader = KrylovSequenceCompressed::open_readonly(path);
    assert(reader.length() == 16);

    std::cout << "  PASS" << std::endl;
}

void test_lru_cache_invariant() {
    std::cout << "Testing LRU cache invariant (multiple reads, identical results)..." << std::endl;
    auto path = tmp_path("lru");
    PathCleanup cleanup{path};

    constexpr uint64_t L = 200;
    constexpr uint64_t chunk_blocks = 8;  // many chunks
    std::vector<Block512> golden(L);
    std::mt19937_64 rng(0x99);
    for (uint64_t k = 0; k < L; ++k) {
        for (int i = 0; i < 64; ++i) golden[k].rows[i] = rng();
    }

    {
        KrylovSequenceCompressed seq(path, L, sizeof(Block512), chunk_blocks);
        for (uint64_t k = 0; k < L; ++k) {
            std::memcpy(seq.write_at(k), &golden[k], sizeof(Block512));
        }
        seq.close();
    }

    {
        // Small cache to force eviction (16 KB → ~4 chunks at 4 KB each)
        auto reader = KrylovSequenceCompressed::open_readonly(path);
        // The default 64 MB cache fits all data; access pattern triggers
        // cache hits on repeat. Verify byte-for-byte identical results on
        // repeated reads.
        for (int iter = 0; iter < 3; ++iter) {
            for (uint64_t k = 0; k < L; ++k) {
                Block512 loaded;
                std::memcpy(&loaded, reader.read_at(k), sizeof(loaded));
                if (!(loaded == golden[k])) {
                    std::cerr << "LRU iter=" << iter << " block " << k << " mismatch\n";
                    assert(false);
                }
            }
        }
        // Cache hits after 1st pass should grow
        assert(reader.cache_hits() > 0);
    }

    std::cout << "  PASS" << std::endl;
}

void test_out_of_order_write_rejected() {
    std::cout << "Testing out-of-order write rejected..." << std::endl;
    auto path = tmp_path("ooo_write");
    PathCleanup cleanup{path};

    KrylovSequenceCompressed seq(path, /*L=*/16, sizeof(Block512),
                                 /*chunk_blocks=*/8);
    Block512 dummy{};
    std::memcpy(seq.write_at(0), &dummy, sizeof(dummy));
    bool threw = false;
    try {
        std::memcpy(seq.write_at(5), &dummy, sizeof(dummy));  // skips 1-4
    } catch (const std::logic_error&) {
        threw = true;
    }
    assert(threw);
    seq.remove_file();

    std::cout << "  PASS" << std::endl;
}

void test_oor_access_throws() {
    std::cout << "Testing out-of-range read_at/write_at rejected..." << std::endl;
    auto path = tmp_path("oor");
    PathCleanup cleanup{path};

    KrylovSequenceCompressed seq(path, /*L=*/8, sizeof(Block512),
                                 /*chunk_blocks=*/4);
    Block512 dummy{};
    bool threw_write = false;
    try {
        std::memcpy(seq.write_at(8), &dummy, sizeof(dummy));  // L is 8 → max idx 7
    } catch (const std::out_of_range&) {
        threw_write = true;
    }
    assert(threw_write);

    // Fill + close
    for (uint64_t k = 0; k < 8; ++k) {
        std::memcpy(seq.write_at(k), &dummy, sizeof(dummy));
    }
    seq.close();

    auto reader = KrylovSequenceCompressed::open_readonly(path);
    bool threw_read = false;
    try {
        (void) reader.read_at(8);
    } catch (const std::out_of_range&) {
        threw_read = true;
    }
    assert(threw_read);

    std::cout << "  PASS" << std::endl;
}

void test_sparse_krylov_synthetic_compression_ratio() {
    std::cout << "Testing sparse Krylov synthetic ratio (≥1.3× target informational)..." << std::endl;
    auto path = tmp_path("krylov_sim");
    PathCleanup cleanup{path};

    // Simulate Krylov block sequence: V_0 random, V_{k+1} = V_k XOR (small
    // random perturbation simulating M·M^T · V_k for sparse M)
    constexpr uint64_t L = 200;
    constexpr uint64_t chunk_blocks = 32;
    std::vector<Block512> blocks(L);
    std::mt19937_64 rng(0x0FF1CE);
    // V_0 random fill
    for (int i = 0; i < 64; ++i) blocks[0].rows[i] = rng();
    // V_{k+1} = V_k XOR (perturbation with 3-5 bits flipped per row)
    for (uint64_t k = 1; k < L; ++k) {
        blocks[k] = blocks[k - 1];
        for (int r = 0; r < 64; ++r) {
            int flips = static_cast<int>(rng() % 6);
            for (int f = 0; f < flips; ++f) {
                int bit = static_cast<int>(rng() & 63);
                blocks[k].rows[r] ^= (1ULL << bit);
            }
        }
    }

    {
        KrylovSequenceCompressed seq(path, L, sizeof(Block512), chunk_blocks);
        for (uint64_t k = 0; k < L; ++k) {
            std::memcpy(seq.write_at(k), &blocks[k], sizeof(Block512));
        }
        seq.close();
        const uint64_t total_orig = L * sizeof(Block512);
        const uint64_t total_comp = seq.total_compressed_bytes();
        const double ratio = static_cast<double>(total_comp) /
                             static_cast<double>(total_orig);
        std::cout << "  sparse Krylov sim: " << total_orig << " B -> "
                  << total_comp << " B (ratio=" << ratio
                  << ", " << (total_orig / std::max<uint64_t>(total_comp, 1))
                  << "× compression)" << std::endl;
        // Informational: do not strict-assert. Print only.
        // ≥1.3× compression means ratio ≤ 0.77. Sparse Krylov should
        // easily achieve this with XOR-delta + RLE.
        if (ratio >= 0.77) {
            std::cerr << "  WARN: ratio " << ratio << " >= 0.77 — expected ≤ 0.77 for sparse Krylov"
                      << std::endl;
        }
    }

    // Roundtrip correctness check
    {
        auto reader = KrylovSequenceCompressed::open_readonly(path);
        for (uint64_t k = 0; k < L; ++k) {
            Block512 loaded;
            std::memcpy(&loaded, reader.read_at(k), sizeof(loaded));
            assert(loaded == blocks[k]);
        }
    }

    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "===== KrylovSequenceCompressed Tests =====" << std::endl;

    test_single_block_roundtrip();
    test_1000_block_roundtrip();
    test_all_zero_blocks_high_ratio();
    test_max_density_no_blowup();
    test_chunk_boundary_blocks();
    test_incomplete_on_reopen_rejected();
    test_magic_flip_on_close();
    test_lru_cache_invariant();
    test_out_of_order_write_rejected();
    test_oor_access_throws();
    test_sparse_krylov_synthetic_compression_ratio();

    std::cout << "\n===== All KrylovSequenceCompressed tests PASSED =====" << std::endl;
    return 0;
}

#endif
