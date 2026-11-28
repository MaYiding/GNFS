#include "gnfs/linalg/bl_checkpoint.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"
#include "support/test_check.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

#if defined(__linux__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GNFS_TEST_UNDER_ASAN 1
#endif
#endif
#if !defined(GNFS_TEST_UNDER_ASAN) && defined(__SANITIZE_ADDRESS__)
#define GNFS_TEST_UNDER_ASAN 1
#endif

using gnfs::linalg::BlockLanczosCheckpoint;

// Helper: temp file path that does not collide between cases
static std::string tmp_ckpt_path(const char* label) {
    static int seq = 0;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "gnfs_test_bl_ckpt_%d_%d_%s", gnfs::util::process_id(), ++seq,
                  label);
    return gnfs::util::temp_path(buf);
}

struct CkptCleanup {
    std::string path;
    ~CkptCleanup() {
        if (!path.empty())
            std::remove(path.c_str());
    }
};

static void require_save(const BlockLanczosCheckpoint& ck, const std::string& path) {
    if (!ck.save(path)) {
        std::cerr << "ERROR: failed to save checkpoint to " << path << std::endl;
        std::abort();
    }
}

static void append_u64_le(std::vector<uint8_t>& bytes, uint64_t value) {
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
}

static std::vector<uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    GNFS_TEST_CHECK(in);
    const auto end = in.tellg();
    GNFS_TEST_CHECK(end != std::ifstream::pos_type(-1));
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    in.seekg(0);
    if (!bytes.empty()) {
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        GNFS_TEST_CHECK(in.gcount() == static_cast<std::streamsize>(bytes.size()));
    }
    return bytes;
}

static void write_file_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    // trunc keeps the file exactly as long as the byte sequence.
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    GNFS_TEST_CHECK(out);
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    GNFS_TEST_CHECK(out);
}

static void append_digest(std::vector<uint8_t>& bytes, const gnfs::util::Sha256Digest& digest) {
    for (std::byte value : digest.bytes)
        bytes.push_back(std::to_integer<uint8_t>(value));
}

static void write_native_u64(std::ofstream& out, uint64_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

static void write_le_u64(std::ofstream& out, uint64_t value) {
    std::array<uint8_t, sizeof(uint64_t)> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>(value >> (i * 8));
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

// Write the fixed 136-byte V4 prefix of a hand-built fixture: magic, version,
// the six shape and cursor fields, the digest, the fingerprint, and the word
// count. The payload is left to the caller.
static void write_v4_header(std::ofstream& out, uint64_t rows, uint64_t cols, uint64_t wpr,
                            uint64_t pivot_row, uint64_t cur_col, uint64_t iteration,
                            uint64_t aug_word_count, const gnfs::util::Sha256Digest& digest,
                            const gnfs::util::Sha256Digest& fingerprint) {
    write_le_u64(out, BlockLanczosCheckpoint::MAGIC);
    write_le_u64(out, BlockLanczosCheckpoint::VERSION_V4);
    write_le_u64(out, rows);
    write_le_u64(out, cols);
    write_le_u64(out, wpr);
    write_le_u64(out, pivot_row);
    write_le_u64(out, cur_col);
    write_le_u64(out, iteration);
    out.write(reinterpret_cast<const char*>(digest.bytes.data()),
              static_cast<std::streamsize>(digest.bytes.size()));
    out.write(reinterpret_cast<const char*>(fingerprint.bytes.data()),
              static_cast<std::streamsize>(fingerprint.bytes.size()));
    write_le_u64(out, aug_word_count);
}

static gnfs::util::Sha256Digest test_fingerprint() {
    gnfs::util::Sha256Digest digest;
    digest.bytes.front() = std::byte{0x01};
    digest.bytes.back() = std::byte{0x80};
    return digest;
}

// Helper: rebuild a small deterministic aug payload for round-trip checks.
static BlockLanczosCheckpoint make_small_state() {
    BlockLanczosCheckpoint ck;
    ck.matrix_fingerprint = test_fingerprint();
    ck.rows = 8;
    ck.cols = 16;
    ck.aug_words_per_row = 1; // 8+16 = 24 cols → 1 word per row
    ck.pivot_row = 3;
    ck.cur_col = 11;
    ck.iteration = 5;
    ck.aug.resize(ck.rows * ck.aug_words_per_row);
    for (uint64_t i = 0; i < ck.aug.size(); ++i) {
        ck.aug[i] = 0xCAFE'BABE'0000'0000ULL ^ (i * 0x0011'2233'4455'6677ULL);
    }
    return ck;
}

void test_roundtrip_small() {
    std::cout << "Testing roundtrip (small state)..." << std::endl;
    auto path = tmp_ckpt_path("small");
    CkptCleanup cleanup{path};

    auto orig = make_small_state();
    require_save(orig, path);
    GNFS_TEST_CHECK(BlockLanczosCheckpoint::exists_and_valid(path));

    auto loaded_opt = BlockLanczosCheckpoint::load(path);
    GNFS_TEST_CHECK(loaded_opt.has_value());
    auto& loaded = *loaded_opt;

    GNFS_TEST_CHECK(loaded.format_version == BlockLanczosCheckpoint::VERSION_V4);
    GNFS_TEST_CHECK(loaded.rows == orig.rows);
    GNFS_TEST_CHECK(loaded.cols == orig.cols);
    GNFS_TEST_CHECK(loaded.aug_words_per_row == orig.aug_words_per_row);
    GNFS_TEST_CHECK(loaded.pivot_row == orig.pivot_row);
    GNFS_TEST_CHECK(loaded.cur_col == orig.cur_col);
    GNFS_TEST_CHECK(loaded.iteration == orig.iteration);
    GNFS_TEST_CHECK(loaded.aug == orig.aug);
    GNFS_TEST_CHECK(loaded.matrix_fingerprint == orig.matrix_fingerprint);

    // Independent SHA-256 oracle for the hashed range of this exact state:
    // version, six fields, fingerprint, word count, and the eight payload words.
    const auto expected_digest = gnfs::util::decode_sha256_hex(
        "6c6a70debe89f3f00d1b364914ef457a83c1e8cf868e8dff3cd41d3be512b5ae");
    GNFS_TEST_CHECK(expected_digest.has_value());
    const auto bytes = read_file_bytes(path);
    GNFS_TEST_CHECK(bytes.size() == 136 + 8 * orig.aug.size());
    const uint8_t* digest_begin = bytes.data() + BlockLanczosCheckpoint::DIGEST_OFFSET;
    const uint8_t* digest_end = bytes.data() + BlockLanczosCheckpoint::FINGERPRINT_OFFSET;
    const std::vector<uint8_t> stored_digest(digest_begin, digest_end);
    std::vector<uint8_t> expected_bytes;
    append_digest(expected_bytes, *expected_digest);
    GNFS_TEST_CHECK(stored_digest == expected_bytes);

    std::cout << "  small roundtrip: PASS" << std::endl;
}

void test_v4_little_endian_fixture() {
    std::cout << "Testing V4 little-endian byte fixture..." << std::endl;
    auto path = tmp_ckpt_path("v4_le_fixture");
    CkptCleanup cleanup{path};

    constexpr uint64_t rows = 1;
    constexpr uint64_t cols = 2;
    constexpr uint64_t wpr = 1;
    constexpr uint64_t pivot_row = 0;
    constexpr uint64_t cur_col = 1;
    constexpr uint64_t iteration = 7;
    constexpr uint64_t payload = 0x0123'4567'89AB'CDEFULL;
    const auto fingerprint = test_fingerprint();
    // Independent SHA-256 oracle over the hashed range: version, the six
    // fields, the fingerprint, the word count, and the payload word.
    const auto digest = gnfs::util::decode_sha256_hex(
        "3391fdd13b4d51c94677086215425e49f9cd15a2a0f1f0851aebcd380da3fa69");
    GNFS_TEST_CHECK(digest.has_value());

    BlockLanczosCheckpoint original;
    original.rows = rows;
    original.cols = cols;
    original.aug_words_per_row = wpr;
    original.pivot_row = pivot_row;
    original.cur_col = cur_col;
    original.iteration = iteration;
    original.aug = {payload};
    original.matrix_fingerprint = fingerprint;
    require_save(original, path);

    std::vector<uint8_t> expected;
    expected.reserve(144);
    append_u64_le(expected, BlockLanczosCheckpoint::MAGIC);
    append_u64_le(expected, BlockLanczosCheckpoint::VERSION);
    append_u64_le(expected, rows);
    append_u64_le(expected, cols);
    append_u64_le(expected, wpr);
    append_u64_le(expected, pivot_row);
    append_u64_le(expected, cur_col);
    append_u64_le(expected, iteration);
    append_digest(expected, *digest);
    append_digest(expected, fingerprint);
    append_u64_le(expected, 1);
    append_u64_le(expected, payload);
    // 136 fixed bytes plus one payload word.
    GNFS_TEST_CHECK(expected.size() == 144);
    GNFS_TEST_CHECK(read_file_bytes(path) == expected);

    // Decode the independent fixture, not just bytes emitted by save().
    write_file_bytes(path, expected);
    const auto loaded = BlockLanczosCheckpoint::load(path);
    GNFS_TEST_CHECK(loaded.has_value());
    GNFS_TEST_CHECK(loaded->format_version == BlockLanczosCheckpoint::VERSION_V4);
    GNFS_TEST_CHECK(loaded->matrix_fingerprint == original.matrix_fingerprint);
    GNFS_TEST_CHECK(loaded->rows == rows);
    GNFS_TEST_CHECK(loaded->cols == cols);
    GNFS_TEST_CHECK(loaded->aug_words_per_row == wpr);
    GNFS_TEST_CHECK(loaded->pivot_row == pivot_row);
    GNFS_TEST_CHECK(loaded->cur_col == cur_col);
    GNFS_TEST_CHECK(loaded->iteration == iteration);
    GNFS_TEST_CHECK(loaded->aug == std::vector<uint64_t>{payload});

    std::cout << "  V4 LE byte fixture + decode: PASS" << std::endl;
}

void test_legacy_compatibility(uint64_t version) {
    std::cout << "Testing legacy V" << version << " checkpoint compatibility..." << std::endl;
    auto path = tmp_ckpt_path("legacy");
    CkptCleanup cleanup{path};

    constexpr uint64_t rows = 2;
    constexpr uint64_t cols = 3;
    constexpr uint64_t wpr = 1;
    constexpr uint64_t pivot_row = 1;
    constexpr uint64_t cur_col = 4;
    constexpr uint64_t iteration = 9;
    constexpr uint64_t first_word = 0xAABB'CCDDEE11'2233ULL;
    constexpr uint64_t second_word = 0x4455'66778899'0001ULL;
    const bool bound = version == BlockLanczosCheckpoint::VERSION_V3;
    const auto fingerprint = test_fingerprint();

    // V3 folded the four little-endian words of the fingerprint digest into the
    // header checksum and stored the digest between header and word count.
    uint64_t fingerprint_fold = 0;
    if (bound) {
        for (size_t index = 0; index < fingerprint.bytes.size(); ++index) {
            fingerprint_fold ^=
                static_cast<uint64_t>(std::to_integer<uint8_t>(fingerprint.bytes[index]))
                << ((index % sizeof(uint64_t)) * 8);
        }
        GNFS_TEST_CHECK(fingerprint_fold == 0x8000000000000001ULL);
    }
    const uint64_t header_checksum =
        version ^ rows ^ cols ^ wpr ^ pivot_row ^ cur_col ^ iteration ^ fingerprint_fold;
    const uint64_t body_checksum = first_word ^ second_word;

    const auto write_u64 =
        version == BlockLanczosCheckpoint::VERSION_V1 ? write_native_u64 : write_le_u64;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    GNFS_TEST_CHECK(out);
    write_u64(out, BlockLanczosCheckpoint::MAGIC);
    write_u64(out, version);
    write_u64(out, rows);
    write_u64(out, cols);
    write_u64(out, wpr);
    write_u64(out, pivot_row);
    write_u64(out, cur_col);
    write_u64(out, iteration);
    write_u64(out, header_checksum);
    if (bound) {
        out.write(reinterpret_cast<const char*>(fingerprint.bytes.data()),
                  static_cast<std::streamsize>(fingerprint.bytes.size()));
        GNFS_TEST_CHECK(out);
    }
    write_u64(out, 2);
    write_u64(out, first_word);
    write_u64(out, second_word);
    write_u64(out, body_checksum);
    out.close();
    GNFS_TEST_CHECK(out);

    GNFS_TEST_CHECK(BlockLanczosCheckpoint::exists_and_valid(path));
    const auto loaded = BlockLanczosCheckpoint::load(path);
    GNFS_TEST_CHECK(loaded.has_value());
    // Legacy files stay decodable for diagnostics and report their version.
    GNFS_TEST_CHECK(loaded->format_version == version);
    GNFS_TEST_CHECK(loaded->matrix_fingerprint.has_value() == bound);
    if (bound)
        GNFS_TEST_CHECK(loaded->matrix_fingerprint == fingerprint);
    GNFS_TEST_CHECK(loaded->rows == rows);
    GNFS_TEST_CHECK(loaded->cols == cols);
    GNFS_TEST_CHECK(loaded->aug_words_per_row == wpr);
    GNFS_TEST_CHECK(loaded->pivot_row == pivot_row);
    GNFS_TEST_CHECK(loaded->cur_col == cur_col);
    GNFS_TEST_CHECK(loaded->iteration == iteration);
    const std::vector<uint64_t> expected_aug{first_word, second_word};
    GNFS_TEST_CHECK(loaded->aug == expected_aug);

    std::cout << "  Legacy decode compatibility: PASS" << std::endl;
}

void test_roundtrip_large() {
    std::cout << "Testing roundtrip (large 1 MB payload)..." << std::endl;
    auto path = tmp_ckpt_path("large");
    CkptCleanup cleanup{path};

    BlockLanczosCheckpoint orig;
    orig.matrix_fingerprint = test_fingerprint();
    orig.rows = 4096;
    orig.cols = 4096;
    orig.aug_words_per_row = 128; // 4096+4096 = 8192 bits / 64 = 128 words
    orig.pivot_row = 2000;
    orig.cur_col = 4096 + 1500;
    orig.iteration = 1234;
    orig.aug.resize(orig.rows * orig.aug_words_per_row);

    std::mt19937_64 rng(42);
    for (auto& w : orig.aug)
        w = rng();

    require_save(orig, path);
    auto loaded_opt = BlockLanczosCheckpoint::load(path);
    GNFS_TEST_CHECK(loaded_opt.has_value());
    auto& loaded = *loaded_opt;
    GNFS_TEST_CHECK(loaded.rows == orig.rows);
    GNFS_TEST_CHECK(loaded.aug == orig.aug);
    GNFS_TEST_CHECK(loaded.matrix_fingerprint == orig.matrix_fingerprint);

    std::cout << "  large roundtrip: PASS" << std::endl;
}

void test_empty_matrix() {
    std::cout << "Testing empty matrix (rows=cols=0)..." << std::endl;
    auto path = tmp_ckpt_path("empty");
    CkptCleanup cleanup{path};

    BlockLanczosCheckpoint orig; // zero-dimensional payload
    orig.matrix_fingerprint = test_fingerprint();
    require_save(orig, path);
    auto loaded_opt = BlockLanczosCheckpoint::load(path);
    GNFS_TEST_CHECK(loaded_opt.has_value());
    GNFS_TEST_CHECK(loaded_opt->rows == 0);
    GNFS_TEST_CHECK(loaded_opt->cols == 0);
    GNFS_TEST_CHECK(loaded_opt->aug.empty());

    std::cout << "  empty matrix: PASS" << std::endl;
}

void test_incomplete_magic_rejected() {
    std::cout << "Testing INCOMPLETE magic rejection..." << std::endl;
    auto path = tmp_ckpt_path("incomplete");
    CkptCleanup cleanup{path};

    // An interrupted save leaves a temporary file whose magic is still
    // INCOMPLETE. Only those eight bytes differ from a complete checkpoint.
    require_save(make_small_state(), path);
    auto bytes = read_file_bytes(path);
    std::fill(bytes.begin(), bytes.begin() + 8, 0);
    write_file_bytes(path, bytes);

    GNFS_TEST_CHECK(!BlockLanczosCheckpoint::exists_and_valid(path));
    auto loaded = BlockLanczosCheckpoint::load(path);
    GNFS_TEST_CHECK(!loaded.has_value());

    std::cout << "  INCOMPLETE rejection: PASS" << std::endl;
}

void test_corrupt_header_field_rejected() {
    std::cout << "Testing corrupt header field rejection..." << std::endl;
    auto path = tmp_ckpt_path("corrupt_header");
    CkptCleanup cleanup{path};

    auto orig = make_small_state();
    require_save(orig, path);

    // Flip one byte of `iteration` (byte 56). No cheap field check covers it;
    // only the digest can reject this file.
    {
        std::fstream fs(path, std::ios::binary | std::ios::in | std::ios::out);
        GNFS_TEST_CHECK(fs);
        fs.seekp(56);
        char byte = 0;
        fs.read(&byte, 1);
        GNFS_TEST_CHECK(fs.gcount() == 1);
        byte ^= 0x01;
        fs.seekp(56);
        fs.write(&byte, 1);
        GNFS_TEST_CHECK(fs);
    }

    auto loaded = BlockLanczosCheckpoint::load(path);
    GNFS_TEST_CHECK(!loaded.has_value());

    std::cout << "  corrupt header field rejection: PASS" << std::endl;
}

void test_corrupt_payload_rejected() {
    std::cout << "Testing corrupt payload rejection..." << std::endl;
    auto path = tmp_ckpt_path("corrupt_body");
    CkptCleanup cleanup{path};

    auto orig = make_small_state();
    require_save(orig, path);

    // Flip the very last byte, which belongs to the last payload word.
    {
        std::fstream fs(path, std::ios::binary | std::ios::in | std::ios::out);
        GNFS_TEST_CHECK(fs);
        fs.seekg(0, std::ios::end);
        auto sz = fs.tellg();
        fs.seekp(static_cast<std::streamoff>(sz) - 1);
        char byte = 0;
        fs.read(&byte, 1);
        byte ^= 0xFF;
        fs.seekp(static_cast<std::streamoff>(sz) - 1);
        fs.write(&byte, 1);
    }

    auto loaded = BlockLanczosCheckpoint::load(path);
    GNFS_TEST_CHECK(!loaded.has_value());

    std::cout << "  corrupt payload rejection: PASS" << std::endl;
}

void test_fingerprint_corruption_rejected() {
    const auto path = tmp_ckpt_path("fingerprint_corruption");
    CkptCleanup cleanup{path};
    require_save(make_small_state(), path);
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        GNFS_TEST_CHECK(file);
        file.seekg(BlockLanczosCheckpoint::FINGERPRINT_OFFSET);
        char first_byte = 0;
        file.read(&first_byte, 1);
        GNFS_TEST_CHECK(file.gcount() == 1);
        first_byte ^= 1;
        file.seekp(BlockLanczosCheckpoint::FINGERPRINT_OFFSET);
        file.write(&first_byte, 1);
        GNFS_TEST_CHECK(file);
    }
    GNFS_TEST_CHECK(!BlockLanczosCheckpoint::load(path));
}

void test_unbound_save_preserves_existing_file() {
    const auto path = tmp_ckpt_path("unbound_save");
    CkptCleanup cleanup{path};
    auto bound = make_small_state();
    require_save(bound, path);
    const auto original_bytes = read_file_bytes(path);
    bound.matrix_fingerprint.reset();
    GNFS_TEST_CHECK(!bound.save(path));
    GNFS_TEST_CHECK(read_file_bytes(path) == original_bytes);
}

void test_version_mismatch_rejected() {
    std::cout << "Testing version mismatch rejection..." << std::endl;
    auto path = tmp_ckpt_path("version_mismatch");
    CkptCleanup cleanup{path};

    {
        std::ofstream out(path, std::ios::binary);
        uint64_t magic = BlockLanczosCheckpoint::MAGIC;
        uint64_t version = 99999;
        write_le_u64(out, magic);
        write_le_u64(out, version);
    }

    auto loaded = BlockLanczosCheckpoint::load(path);
    GNFS_TEST_CHECK(!loaded.has_value());

    std::cout << "  version mismatch rejection: PASS" << std::endl;
}

void test_truncated_file_rejected() {
    std::cout << "Testing truncated file rejection..." << std::endl;
    auto path = tmp_ckpt_path("truncated");
    CkptCleanup cleanup{path};

    auto orig = make_small_state();
    require_save(orig, path);

    // Truncate file to half its size.
    {
        std::ifstream src(path, std::ios::binary | std::ios::ate);
        if (!src) {
            std::cerr << "ERROR: failed to open checkpoint for truncation" << std::endl;
            std::abort();
        }
        auto pos = src.tellg();
        if (pos == std::ifstream::pos_type(-1)) {
            std::cerr << "ERROR: failed to determine checkpoint size" << std::endl;
            std::abort();
        }
        auto sz = static_cast<size_t>(pos);
        src.close();
        std::ifstream rin(path, std::ios::binary);
        std::vector<char> buf(sz / 2);
        rin.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        rin.close();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    }

    auto loaded = BlockLanczosCheckpoint::load(path);
    GNFS_TEST_CHECK(!loaded.has_value());

    std::cout << "  truncated file rejection: PASS" << std::endl;
}

void test_load_nonexistent() {
    std::cout << "Testing load nonexistent file..." << std::endl;
    const std::string path = gnfs::util::temp_path("__nonexistent_bl_ckpt_xyz_12345");
    auto loaded = BlockLanczosCheckpoint::load(path);
    GNFS_TEST_CHECK(!loaded.has_value());
    GNFS_TEST_CHECK(!BlockLanczosCheckpoint::exists_and_valid(path));
    std::cout << "  nonexistent file: PASS" << std::endl;
}

void test_exists_and_valid_semantics() {
    std::cout << "Testing exists_and_valid semantics..." << std::endl;
    auto path = tmp_ckpt_path("exists");
    CkptCleanup cleanup{path};

    GNFS_TEST_CHECK(!BlockLanczosCheckpoint::exists_and_valid(path)); // doesn't exist
    auto orig = make_small_state();
    require_save(orig, path);
    GNFS_TEST_CHECK(BlockLanczosCheckpoint::exists_and_valid(path));

    std::cout << "  exists_and_valid semantics: PASS" << std::endl;
}

void test_remove() {
    std::cout << "Testing remove()..." << std::endl;
    auto path = tmp_ckpt_path("remove");

    auto orig = make_small_state();
    require_save(orig, path);
    GNFS_TEST_CHECK(BlockLanczosCheckpoint::exists_and_valid(path));

    BlockLanczosCheckpoint::remove(path);
    GNFS_TEST_CHECK(!BlockLanczosCheckpoint::exists_and_valid(path));

    std::cout << "  remove: PASS" << std::endl;
}

void test_interval_env_parser() {
    std::cout << "Testing GNFS_BL_CHECKPOINT_INTERVAL parser..." << std::endl;

    // Defensive: unset → default 50.
    ::unsetenv("GNFS_BL_CHECKPOINT_INTERVAL");
    GNFS_TEST_CHECK(gnfs::linalg::bl_checkpoint_interval() == 50);

    ::setenv("GNFS_BL_CHECKPOINT_INTERVAL", "10", 1);
    GNFS_TEST_CHECK(gnfs::linalg::bl_checkpoint_interval() == 10);

    ::setenv("GNFS_BL_CHECKPOINT_INTERVAL", "0", 1);
    GNFS_TEST_CHECK(gnfs::linalg::bl_checkpoint_interval() == 50); // 0 → default

    ::setenv("GNFS_BL_CHECKPOINT_INTERVAL", "9999999999", 1);
    GNFS_TEST_CHECK(gnfs::linalg::bl_checkpoint_interval() == 1'000'000); // clamped

    ::setenv("GNFS_BL_CHECKPOINT_INTERVAL", "notanumber", 1);
    GNFS_TEST_CHECK(gnfs::linalg::bl_checkpoint_interval() == 50);

    ::setenv("GNFS_BL_CHECKPOINT_INTERVAL", "", 1);
    GNFS_TEST_CHECK(gnfs::linalg::bl_checkpoint_interval() == 50);

    ::unsetenv("GNFS_BL_CHECKPOINT_INTERVAL");
    std::cout << "  interval parser: PASS" << std::endl;
}

void test_base_path_env_parser() {
    std::cout << "Testing GNFS_BL_CHECKPOINT base path parser..." << std::endl;
    ::unsetenv("GNFS_BL_CHECKPOINT");
    GNFS_TEST_CHECK(gnfs::linalg::bl_checkpoint_base_path().empty());
    GNFS_TEST_CHECK(gnfs::linalg::bl_checkpoint_full_path().empty());

    ::setenv("GNFS_BL_CHECKPOINT", "/tmp/foo_session", 1);
    GNFS_TEST_CHECK(gnfs::linalg::bl_checkpoint_base_path() == "/tmp/foo_session");
    GNFS_TEST_CHECK(gnfs::linalg::bl_checkpoint_full_path() == "/tmp/foo_session.bl_ckpt");

    ::unsetenv("GNFS_BL_CHECKPOINT");
    std::cout << "  base path parser: PASS" << std::endl;
}

void test_overwrite_existing() {
    std::cout << "Testing save() overwrites existing checkpoint..." << std::endl;
    auto path = tmp_ckpt_path("overwrite");
    CkptCleanup cleanup{path};

    auto first = make_small_state();
    require_save(first, path);

    BlockLanczosCheckpoint second = first;
    second.iteration = 999;
    second.pivot_row = 7;
    second.aug[0] = 0x1234'5678'9ABC'DEF0ULL;
    require_save(second, path);

    auto loaded = BlockLanczosCheckpoint::load(path);
    GNFS_TEST_CHECK(loaded.has_value());
    GNFS_TEST_CHECK(loaded->iteration == 999);
    GNFS_TEST_CHECK(loaded->pivot_row == 7);
    GNFS_TEST_CHECK(loaded->aug[0] == 0x1234'5678'9ABC'DEF0ULL);

    std::cout << "  overwrite existing: PASS" << std::endl;
}

void test_wpr_mismatch_rejected() {
    std::cout << "Testing wpr mismatch in payload rejected..." << std::endl;
    auto path = tmp_ckpt_path("wpr_mismatch");
    CkptCleanup cleanup{path};

    // Hand-craft a file: claim rows=4, wpr=2 → 8 words, but write 7 words. The
    // digest is a placeholder: the count check rejects the file before the
    // payload is hashed.
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        GNFS_TEST_CHECK(out);
        const gnfs::util::Sha256Digest placeholder;
        write_v4_header(out, 4, 100, 2, 0, 100, 0, 7, placeholder, placeholder);
        uint64_t dummy[7] = {};
        for (uint64_t word : dummy)
            write_le_u64(out, word);
        out.close();
        GNFS_TEST_CHECK(out);
    }

    auto loaded = BlockLanczosCheckpoint::load(path);
    GNFS_TEST_CHECK(!loaded.has_value());

    std::cout << "  wpr mismatch rejection: PASS" << std::endl;
}

void test_oversized_payload_rejected_before_allocation() {
    std::cout << "Testing oversized payload rejection..." << std::endl;
    auto path = tmp_ckpt_path("oversized_payload");
    CkptCleanup cleanup{path};

    constexpr uint64_t rows = 1;
    constexpr uint64_t cols = 1;
    constexpr uint64_t wpr = BlockLanczosCheckpoint::MAX_AUG_WORDS + 1;
    constexpr uint64_t pivot_row = 0;
    constexpr uint64_t cur_col = 1;
    constexpr uint64_t iteration = 0;

    // The loader rejects the count before checking the payload frame or
    // allocating a vector, so the fixture needs only its fixed header. The
    // digest is a placeholder for the same reason.
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        GNFS_TEST_CHECK(out);
        const gnfs::util::Sha256Digest placeholder;
        write_v4_header(out, rows, cols, wpr, pivot_row, cur_col, iteration, wpr, placeholder,
                        placeholder);
        out.close();
        GNFS_TEST_CHECK(out);
    }

    const auto loaded = BlockLanczosCheckpoint::load(path);
    GNFS_TEST_CHECK(!loaded.has_value());
    std::cout << "  oversized payload rejected before allocation: PASS" << std::endl;
}

void test_allocation_failure_returns_nullopt() {
#if !defined(__linux__)
    // Linux is the only CI platform where this test can reliably lower
    // RLIMIT_AS. macOS rejects that limit change, and Windows has no POSIX
    // equivalent; the portable oversized-count test above still runs there.
    std::cout << "Testing allocation failure handling... SKIP (requires Linux RLIMIT_AS)"
              << std::endl;
#else
#if defined(GNFS_TEST_UNDER_ASAN)
    // ASAN terminates oversized throwing operator new in its allocator before
    // libstdc++ can raise std::bad_alloc. The portable pre-allocation guard
    // above remains active; this resource-limit probe is meaningful only in an
    // unsanitized process where the loader can observe the C++ exception.
    std::cout << "Testing allocation failure handling... SKIP (ASAN allocator)" << std::endl;
    return;
#endif
    std::cout << "Testing allocation failure handling..." << std::endl;
    const auto require = [](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "ERROR: " << message << std::endl;
            std::abort();
        }
    };
    auto path = tmp_ckpt_path("allocation_failure");
    CkptCleanup cleanup{path};

    // Construct a wire-valid checkpoint at the 64 GiB packed-payload cap,
    // without materialising its sparse payload on disk. load() must reject the
    // allocation in the child and return nullopt rather than terminate. The
    // digest is a placeholder: the allocation happens before the payload is
    // hashed, so its content never matters for this probe.
    constexpr uint64_t max_aug_words = BlockLanczosCheckpoint::MAX_AUG_WORDS;
    // A V4 frame is the 136-byte header followed by the payload; no field
    // trails the payload.
    constexpr uint64_t payload_offset = BlockLanczosCheckpoint::PAYLOAD_OFFSET;
    constexpr uint64_t fixed_file_bytes = 136;
    constexpr uint64_t rows = 1;
    constexpr uint64_t cols = 1;
    constexpr uint64_t wpr = max_aug_words;
    constexpr uint64_t pivot_row = 0;
    constexpr uint64_t cur_col = 1;
    constexpr uint64_t iteration = 0;

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(out), "failed to create sparse checkpoint");
        const gnfs::util::Sha256Digest placeholder;
        write_v4_header(out, rows, cols, wpr, pivot_row, cur_col, iteration, max_aug_words,
                        placeholder, placeholder);

        // Leave the payload sparse and write only its last byte. The resulting
        // file size is exactly what load() expects.
        const uint64_t file_bytes = payload_offset + max_aug_words * 8ULL;
        require(file_bytes == fixed_file_bytes + max_aug_words * 8ULL,
                "sparse checkpoint size is inconsistent");
        require(file_bytes - 1 <= static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()),
                "checkpoint offset exceeds streamoff");
        out.seekp(static_cast<std::streamoff>(file_bytes - 1));
        require(static_cast<bool>(out), "failed to seek sparse checkpoint");
        const char last_byte = 0;
        out.write(&last_byte, 1);
        out.close();
        require(static_cast<bool>(out), "failed to finalize sparse checkpoint");
    }

    const pid_t child = ::fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        // Keep the test deterministic and avoid relying on host overcommit
        // policy: the 64 GiB vector allocation must fail before payload I/O.
        // macOS does not consistently enforce RLIMIT_AS for malloc-backed
        // allocations, so also cap the data segment. The alarm prevents an
        // unsupported resource limit from turning this instant test into an
        // unbounded sparse-file scan.
        constexpr rlim_t resource_cap = static_cast<rlim_t>(2ULL * 1024 * 1024 * 1024);
        bool resource_limit_applied = false;
        for (const int resource : {RLIMIT_AS, RLIMIT_DATA}) {
            struct rlimit limit {};
            if (::getrlimit(resource, &limit) != 0)
                continue;
            if (limit.rlim_cur <= resource_cap) {
                resource_limit_applied = true;
                continue;
            }
            if (limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur > resource_cap) {
                limit.rlim_cur = resource_cap;
                if (::setrlimit(resource, &limit) == 0)
                    resource_limit_applied = true;
            }
        }
        ::alarm(5);
        if (!resource_limit_applied)
            ::_exit(3);
        const auto loaded = BlockLanczosCheckpoint::load(path);
        ::alarm(0);
        ::_exit(loaded.has_value() ? 2 : 0);
    }

    int status = 0;
    require(::waitpid(child, &status, 0) == child, "waitpid failed");
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM) {
        std::cout << "  allocation failure handling: SKIP (RLIMIT_AS not enforced)" << std::endl;
        return;
    }
    require(WIFEXITED(status), "allocation-failure child terminated by signal");
    if (WEXITSTATUS(status) == 3) {
        std::cout << "  allocation failure handling: SKIP (RLIMIT_AS unavailable)" << std::endl;
        return;
    }
    require(WEXITSTATUS(status) == 0, "load did not return nullopt after bad_alloc");
    std::cout << "  allocation failure returns nullopt: PASS" << std::endl;
#endif
}

int main() {
    std::cout << "===== BlockLanczosCheckpoint Tests =====" << std::endl;

    test_roundtrip_small();
    test_v4_little_endian_fixture();
    test_legacy_compatibility(BlockLanczosCheckpoint::VERSION_V1);
    test_legacy_compatibility(BlockLanczosCheckpoint::VERSION_V2);
    test_legacy_compatibility(BlockLanczosCheckpoint::VERSION_V3);
    test_roundtrip_large();
    test_empty_matrix();
    test_incomplete_magic_rejected();
    test_corrupt_header_field_rejected();
    test_corrupt_payload_rejected();
    test_fingerprint_corruption_rejected();
    test_unbound_save_preserves_existing_file();
    test_version_mismatch_rejected();
    test_truncated_file_rejected();
    test_load_nonexistent();
    test_exists_and_valid_semantics();
    test_remove();
    test_interval_env_parser();
    test_base_path_env_parser();
    test_overwrite_existing();
    test_wpr_mismatch_rejected();
    test_oversized_payload_rejected_before_allocation();
    test_allocation_failure_returns_nullopt();

    std::cout << "\n===== All BlockLanczosCheckpoint tests PASSED =====" << std::endl;
    return 0;
}
