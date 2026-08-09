// Unit tests for KrylovCompressor (byte-RLE + XOR-delta primitive).
//
// Tests the compressor in isolation, before integration with the mmap layer.
// Roundtrip correctness over GF(2) Krylov data is non-negotiable: any byte
// mismatch breaks BW dependency extraction, so these tests require exact
// equality after decompress.

#include "gnfs/linalg/krylov_compress.hpp"
#include "support/test_check.hpp"

#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using gnfs::linalg::KrylovCompressor;

namespace {

void check_roundtrip(const std::vector<uint8_t>& src, size_t block_stride,
                     const std::string& label) {
    auto compressed = KrylovCompressor::compress_chunk(src.data(), src.size(), block_stride);

    // Header check
    GNFS_TEST_CHECK(compressed.size() >= KrylovCompressor::HEADER_BYTES);
    uint64_t reported =
        KrylovCompressor::peek_uncompressed_size(compressed.data(), compressed.size());
    GNFS_TEST_CHECK(reported == src.size());

    // Decompress
    std::vector<uint8_t> dst(src.size(), 0xFE); // poison to detect under-write
    bool ok = KrylovCompressor::decompress_chunk(compressed.data(), compressed.size(), dst.data(),
                                                 dst.size(), block_stride);
    GNFS_TEST_CHECK(ok);
    GNFS_TEST_CHECK(dst.size() == src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        if (dst[i] != src[i]) {
            std::cerr << label << ": mismatch at byte " << i
                      << " expected=" << static_cast<int>(src[i])
                      << " got=" << static_cast<int>(dst[i]) << std::endl;
            GNFS_TEST_CHECK(false);
        }
    }

    double ratio = src.empty()
                       ? 0.0
                       : static_cast<double>(compressed.size()) / static_cast<double>(src.size());
    std::cout << "  " << label << ": " << src.size() << " B -> " << compressed.size()
              << " B (ratio=" << ratio << ")" << std::endl;
}

} // namespace

void test_empty_roundtrip() {
    std::cout << "Testing empty input..." << std::endl;
    std::vector<uint8_t> src;
    auto compressed = KrylovCompressor::compress_chunk(src.data(), 0, 0);
    GNFS_TEST_CHECK(compressed.size() == KrylovCompressor::HEADER_BYTES);
    std::vector<uint8_t> dst;
    bool ok =
        KrylovCompressor::decompress_chunk(compressed.data(), compressed.size(), dst.data(), 0, 0);
    GNFS_TEST_CHECK(ok);
    std::cout << "  empty: PASS" << std::endl;
}

void test_single_byte_roundtrip() {
    std::cout << "Testing single byte..." << std::endl;
    std::vector<uint8_t> src = {0x42};
    check_roundtrip(src, 0, "single byte");
}

void test_16_byte_roundtrip() {
    std::cout << "Testing 16 byte buffer..." << std::endl;
    std::vector<uint8_t> src(16);
    for (size_t i = 0; i < 16; ++i)
        src[i] = static_cast<uint8_t>(i * 17 + 5);
    check_roundtrip(src, 0, "16 B mixed");
}

void test_4kb_random_roundtrip() {
    std::cout << "Testing 4 KB pseudo-random buffer..." << std::endl;
    std::vector<uint8_t> src(4096);
    std::mt19937_64 rng(0xABCD1234);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<uint8_t>(rng());
    check_roundtrip(src, 0, "4 KB random");
}

void test_all_zero_high_ratio() {
    std::cout << "Testing 32 KB all-zero (should compress ~1000×)..." << std::endl;
    std::vector<uint8_t> src(32768, 0);
    auto compressed = KrylovCompressor::compress_chunk(src.data(), src.size(), 0);
    std::vector<uint8_t> dst(src.size());
    GNFS_TEST_CHECK(KrylovCompressor::decompress_chunk(compressed.data(), compressed.size(),
                                                       dst.data(), dst.size(), 0));
    for (size_t i = 0; i < src.size(); ++i)
        GNFS_TEST_CHECK(dst[i] == 0);

    double ratio = static_cast<double>(compressed.size()) / static_cast<double>(src.size());
    std::cout << "  32 KB zeros: " << src.size() << " B -> " << compressed.size()
              << " B (ratio=" << ratio << ")" << std::endl;
    GNFS_TEST_CHECK(ratio < 0.05); // < 5% — strongly compressible
}

void test_all_ones_roundtrip() {
    std::cout << "Testing 8 KB all-0xFF..." << std::endl;
    std::vector<uint8_t> src(8192, 0xFF);
    check_roundtrip(src, 0, "8 KB 0xFF");
}

void test_sparse_input_roundtrip() {
    std::cout << "Testing 16 KB sparse (90% zeros)..." << std::endl;
    std::vector<uint8_t> src(16384, 0);
    std::mt19937_64 rng(0xDEAD);
    // 10% non-zero
    for (size_t i = 0; i < src.size() / 10; ++i) {
        size_t pos = rng() % src.size();
        src[pos] = static_cast<uint8_t>(rng() | 1);
    }
    auto compressed = KrylovCompressor::compress_chunk(src.data(), src.size(), 0);
    std::vector<uint8_t> dst(src.size());
    GNFS_TEST_CHECK(KrylovCompressor::decompress_chunk(compressed.data(), compressed.size(),
                                                       dst.data(), dst.size(), 0));
    for (size_t i = 0; i < src.size(); ++i)
        GNFS_TEST_CHECK(dst[i] == src[i]);

    double ratio = static_cast<double>(compressed.size()) / static_cast<double>(src.size());
    std::cout << "  16 KB sparse: " << src.size() << " B -> " << compressed.size()
              << " B (ratio=" << ratio << ")" << std::endl;
    // Sparse should compress to well under 50%
    GNFS_TEST_CHECK(ratio < 0.5);
}

void test_incompressible_no_blowup() {
    std::cout << "Testing 4 KB high-entropy (incompressible)..." << std::endl;
    std::vector<uint8_t> src(4096);
    // Truly random data
    std::mt19937_64 rng(0xCAFEBABE);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<uint8_t>(rng());

    auto compressed = KrylovCompressor::compress_chunk(src.data(), src.size(), 0);
    std::vector<uint8_t> dst(src.size());
    GNFS_TEST_CHECK(KrylovCompressor::decompress_chunk(compressed.data(), compressed.size(),
                                                       dst.data(), dst.size(), 0));
    for (size_t i = 0; i < src.size(); ++i)
        GNFS_TEST_CHECK(dst[i] == src[i]);

    // Worst case overhead: header (16) + 1 length byte per 128 bytes of literal
    // = 16 + 32 = 48 bytes overhead over 4096; ratio < 1.015
    double ratio = static_cast<double>(compressed.size()) / static_cast<double>(src.size());
    std::cout << "  4 KB random: " << src.size() << " B -> " << compressed.size()
              << " B (ratio=" << ratio << ")" << std::endl;
    GNFS_TEST_CHECK(ratio < 1.02);
}

void test_delta_mode_roundtrip_sparse() {
    std::cout << "Testing delta-mode with sparse block sequence..." << std::endl;
    // Simulate Krylov pattern: 16 blocks, 512 B each, near-identical with
    // ~5 bytes diff per block.
    constexpr size_t BLOCK_STRIDE = 512;
    constexpr size_t N_BLOCKS = 16;
    std::vector<uint8_t> src(BLOCK_STRIDE * N_BLOCKS, 0);
    // First block: random fill
    std::mt19937_64 rng(0x11223344);
    for (size_t i = 0; i < BLOCK_STRIDE; ++i)
        src[i] = static_cast<uint8_t>(rng());
    // Subsequent blocks: copy previous + 5 random byte flips
    for (size_t blk = 1; blk < N_BLOCKS; ++blk) {
        const size_t off = blk * BLOCK_STRIDE;
        std::memcpy(src.data() + off, src.data() + off - BLOCK_STRIDE, BLOCK_STRIDE);
        for (int k = 0; k < 5; ++k) {
            size_t pos = off + (rng() % BLOCK_STRIDE);
            src[pos] = static_cast<uint8_t>(rng());
        }
    }

    auto compressed = KrylovCompressor::compress_chunk(src.data(), src.size(), BLOCK_STRIDE);
    std::vector<uint8_t> dst(src.size());
    bool ok = KrylovCompressor::decompress_chunk(compressed.data(), compressed.size(), dst.data(),
                                                 dst.size(), BLOCK_STRIDE);
    GNFS_TEST_CHECK(ok);
    for (size_t i = 0; i < src.size(); ++i) {
        if (dst[i] != src[i]) {
            std::cerr << "delta mismatch byte " << i << std::endl;
            GNFS_TEST_CHECK(false);
        }
    }

    double ratio = static_cast<double>(compressed.size()) / static_cast<double>(src.size());
    std::cout << "  delta sparse: " << src.size() << " B -> " << compressed.size()
              << " B (ratio=" << ratio << ")" << std::endl;
    // Delta mode should crush sparse difference to << 0.5
    GNFS_TEST_CHECK(ratio < 0.5);
}

void test_invalid_magic_rejected() {
    std::cout << "Testing decompress rejects bad magic..." << std::endl;
    std::vector<uint8_t> src(64);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<uint8_t>(i);
    auto compressed = KrylovCompressor::compress_chunk(src.data(), src.size(), 0);

    // Corrupt magic
    compressed[0] ^= 0xFF;
    std::vector<uint8_t> dst(src.size());
    bool ok = KrylovCompressor::decompress_chunk(compressed.data(), compressed.size(), dst.data(),
                                                 dst.size(), 0);
    GNFS_TEST_CHECK(!ok);
    std::cout << "  invalid magic: rejected (PASS)" << std::endl;
}

void test_size_mismatch_rejected() {
    std::cout << "Testing decompress rejects size mismatch..." << std::endl;
    std::vector<uint8_t> src(100);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<uint8_t>(i);
    auto compressed = KrylovCompressor::compress_chunk(src.data(), src.size(), 0);

    // Tell decompressor wrong size
    std::vector<uint8_t> dst(50);
    bool ok = KrylovCompressor::decompress_chunk(compressed.data(), compressed.size(), dst.data(),
                                                 dst.size(), 0);
    GNFS_TEST_CHECK(!ok);
    std::cout << "  size mismatch: rejected (PASS)" << std::endl;
}

void test_delta_flag_mismatch_rejected() {
    std::cout << "Testing decompress rejects delta flag mismatch..." << std::endl;
    std::vector<uint8_t> src(256);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<uint8_t>(i);

    // Compressed without delta
    auto raw_compressed = KrylovCompressor::compress_chunk(src.data(), src.size(), 0);
    std::vector<uint8_t> dst(src.size());
    // Decompress with delta=64 should fail
    bool ok = KrylovCompressor::decompress_chunk(raw_compressed.data(), raw_compressed.size(),
                                                 dst.data(), dst.size(), 64);
    GNFS_TEST_CHECK(!ok);

    // Compressed with delta=64
    auto delta_compressed = KrylovCompressor::compress_chunk(src.data(), src.size(), 64);
    // Decompress with delta=0 should fail
    ok = KrylovCompressor::decompress_chunk(delta_compressed.data(), delta_compressed.size(),
                                            dst.data(), dst.size(), 0);
    GNFS_TEST_CHECK(!ok);

    std::cout << "  delta flag mismatch: rejected (PASS)" << std::endl;
}

void test_truncated_payload_rejected() {
    std::cout << "Testing decompress rejects truncated payload..." << std::endl;
    std::vector<uint8_t> src(1024);
    std::mt19937_64 rng(0x99);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<uint8_t>(rng());
    auto compressed = KrylovCompressor::compress_chunk(src.data(), src.size(), 0);

    // Cut off last few bytes
    std::vector<uint8_t> dst(src.size());
    bool ok = KrylovCompressor::decompress_chunk(compressed.data(), compressed.size() - 5,
                                                 dst.data(), dst.size(), 0);
    GNFS_TEST_CHECK(!ok);

    std::cout << "  truncated: rejected (PASS)" << std::endl;
}

void test_repeated_runs() {
    std::cout << "Testing long repeated runs..." << std::endl;
    // 10000 bytes with many same-value runs
    std::vector<uint8_t> src(10000);
    for (size_t i = 0; i < src.size(); ++i) {
        // 200-byte runs of incrementing values
        src[i] = static_cast<uint8_t>(i / 200);
    }
    check_roundtrip(src, 0, "200-byte runs");
}

void test_chunk_sized_block_grid() {
    std::cout << "Testing 32 KB grid of 512 B blocks..." << std::endl;
    constexpr size_t BLOCK_STRIDE = 512;
    constexpr size_t N_BLOCKS = 64; // 32 KB total
    std::vector<uint8_t> src(BLOCK_STRIDE * N_BLOCKS);
    std::mt19937_64 rng(0xBEEF1234);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<uint8_t>(rng());

    auto compressed = KrylovCompressor::compress_chunk(src.data(), src.size(), BLOCK_STRIDE);
    std::vector<uint8_t> dst(src.size());
    bool ok = KrylovCompressor::decompress_chunk(compressed.data(), compressed.size(), dst.data(),
                                                 dst.size(), BLOCK_STRIDE);
    GNFS_TEST_CHECK(ok);
    for (size_t i = 0; i < src.size(); ++i)
        GNFS_TEST_CHECK(dst[i] == src[i]);

    double ratio = static_cast<double>(compressed.size()) / static_cast<double>(src.size());
    std::cout << "  32 KB grid: " << src.size() << " B -> " << compressed.size()
              << " B (ratio=" << ratio << ")" << std::endl;
}

int main() {
    try {
        std::cout << "===== KrylovCompressor Tests =====" << std::endl;

        test_empty_roundtrip();
        test_single_byte_roundtrip();
        test_16_byte_roundtrip();
        test_4kb_random_roundtrip();
        test_all_zero_high_ratio();
        test_all_ones_roundtrip();
        test_sparse_input_roundtrip();
        test_incompressible_no_blowup();
        test_delta_mode_roundtrip_sparse();
        test_invalid_magic_rejected();
        test_size_mismatch_rejected();
        test_delta_flag_mismatch_rejected();
        test_truncated_payload_rejected();
        test_repeated_runs();
        test_chunk_sized_block_grid();

        std::cout << "\n===== All KrylovCompressor tests PASSED =====" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "KrylovCompressor tests FAILED: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "KrylovCompressor tests FAILED: unknown exception\n";
        return 1;
    }
}
