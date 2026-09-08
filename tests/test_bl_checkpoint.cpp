#include "gnfs/linalg/bl_checkpoint.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"

#include <array>
#include <cassert>
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
    assert(in);
    const auto end = in.tellg();
    assert(end != std::ifstream::pos_type(-1));
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    in.seekg(0);
    if (!bytes.empty()) {
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        assert(in.gcount() == static_cast<std::streamsize>(bytes.size()));
    }
    return bytes;
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

// Helper: rebuild a small deterministic aug payload for round-trip checks.
static BlockLanczosCheckpoint make_small_state() {
    BlockLanczosCheckpoint ck;
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
    assert(BlockLanczosCheckpoint::exists_and_valid(path));

    auto loaded_opt = BlockLanczosCheckpoint::load(path);
    assert(loaded_opt.has_value());
    auto& loaded = *loaded_opt;

    assert(loaded.rows == orig.rows);
    assert(loaded.cols == orig.cols);
    assert(loaded.aug_words_per_row == orig.aug_words_per_row);
    assert(loaded.pivot_row == orig.pivot_row);
    assert(loaded.cur_col == orig.cur_col);
    assert(loaded.iteration == orig.iteration);
    assert(loaded.aug == orig.aug);

    std::cout << "  small roundtrip: PASS" << std::endl;
}

void test_v2_little_endian_fixture() {
    std::cout << "Testing V2 little-endian byte fixture..." << std::endl;
    auto path = tmp_ckpt_path("v2_le_fixture");
    CkptCleanup cleanup{path};

    constexpr uint64_t rows = 1;
    constexpr uint64_t cols = 2;
    constexpr uint64_t wpr = 1;
    constexpr uint64_t pivot_row = 0;
    constexpr uint64_t cur_col = 1;
    constexpr uint64_t iteration = 7;
    constexpr uint64_t payload = 0x0123'4567'89AB'CDEFULL;
    constexpr uint64_t header_checksum =
        BlockLanczosCheckpoint::VERSION_V2 ^ rows ^ cols ^ wpr ^ pivot_row ^ cur_col ^ iteration;

    BlockLanczosCheckpoint original;
    original.rows = rows;
    original.cols = cols;
    original.aug_words_per_row = wpr;
    original.pivot_row = pivot_row;
    original.cur_col = cur_col;
    original.iteration = iteration;
    original.aug = {payload};
    require_save(original, path);

    std::vector<uint8_t> expected;
    expected.reserve(12 * sizeof(uint64_t));
    append_u64_le(expected, BlockLanczosCheckpoint::MAGIC);
    append_u64_le(expected, BlockLanczosCheckpoint::VERSION_V2);
    append_u64_le(expected, rows);
    append_u64_le(expected, cols);
    append_u64_le(expected, wpr);
    append_u64_le(expected, pivot_row);
    append_u64_le(expected, cur_col);
    append_u64_le(expected, iteration);
    append_u64_le(expected, header_checksum);
    append_u64_le(expected, 1);
    append_u64_le(expected, payload);
    append_u64_le(expected, payload);
    assert(read_file_bytes(path) == expected);

    // Decode the independent fixture, not just bytes emitted by save().
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        assert(out);
        out.write(reinterpret_cast<const char*>(expected.data()),
                  static_cast<std::streamsize>(expected.size()));
    }
    const auto loaded = BlockLanczosCheckpoint::load(path);
    assert(loaded.has_value());
    assert(loaded->rows == rows);
    assert(loaded->cols == cols);
    assert(loaded->aug_words_per_row == wpr);
    assert(loaded->pivot_row == pivot_row);
    assert(loaded->cur_col == cur_col);
    assert(loaded->iteration == iteration);
    assert(loaded->aug == std::vector<uint64_t>{payload});

    std::cout << "  V2 LE byte fixture + decode: PASS" << std::endl;
}

void test_v1_native_compatibility() {
    std::cout << "Testing V1 native-endian compatibility..." << std::endl;
    auto path = tmp_ckpt_path("v1_native");
    CkptCleanup cleanup{path};

    constexpr uint64_t rows = 2;
    constexpr uint64_t cols = 3;
    constexpr uint64_t wpr = 1;
    constexpr uint64_t pivot_row = 1;
    constexpr uint64_t cur_col = 4;
    constexpr uint64_t iteration = 9;
    constexpr uint64_t first_word = 0xAABB'CCDDEE11'2233ULL;
    constexpr uint64_t second_word = 0x4455'66778899'0001ULL;
    const uint64_t header_checksum =
        BlockLanczosCheckpoint::VERSION_V1 ^ rows ^ cols ^ wpr ^ pivot_row ^ cur_col ^ iteration;
    const uint64_t body_checksum = first_word ^ second_word;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out);
    write_native_u64(out, BlockLanczosCheckpoint::MAGIC);
    write_native_u64(out, BlockLanczosCheckpoint::VERSION_V1);
    write_native_u64(out, rows);
    write_native_u64(out, cols);
    write_native_u64(out, wpr);
    write_native_u64(out, pivot_row);
    write_native_u64(out, cur_col);
    write_native_u64(out, iteration);
    write_native_u64(out, header_checksum);
    write_native_u64(out, 2);
    write_native_u64(out, first_word);
    write_native_u64(out, second_word);
    write_native_u64(out, body_checksum);
    out.close();

    assert(BlockLanczosCheckpoint::exists_and_valid(path));
    const auto loaded = BlockLanczosCheckpoint::load(path);
    assert(loaded.has_value());
    assert(loaded->rows == rows);
    assert(loaded->cols == cols);
    assert(loaded->aug_words_per_row == wpr);
    assert(loaded->pivot_row == pivot_row);
    assert(loaded->cur_col == cur_col);
    assert(loaded->iteration == iteration);
    const std::vector<uint64_t> expected_aug{first_word, second_word};
    assert(loaded->aug == expected_aug);

    std::cout << "  V1 native compatibility: PASS" << std::endl;
}

void test_roundtrip_large() {
    std::cout << "Testing roundtrip (large 1 MB payload)..." << std::endl;
    auto path = tmp_ckpt_path("large");
    CkptCleanup cleanup{path};

    BlockLanczosCheckpoint orig;
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
    assert(loaded_opt.has_value());
    auto& loaded = *loaded_opt;
    assert(loaded.rows == orig.rows);
    assert(loaded.aug == orig.aug);

    std::cout << "  large roundtrip: PASS" << std::endl;
}

void test_empty_matrix() {
    std::cout << "Testing empty matrix (rows=cols=0)..." << std::endl;
    auto path = tmp_ckpt_path("empty");
    CkptCleanup cleanup{path};

    BlockLanczosCheckpoint orig; // all zero
    require_save(orig, path);
    auto loaded_opt = BlockLanczosCheckpoint::load(path);
    assert(loaded_opt.has_value());
    assert(loaded_opt->rows == 0);
    assert(loaded_opt->cols == 0);
    assert(loaded_opt->aug.empty());

    std::cout << "  empty matrix: PASS" << std::endl;
}

void test_incomplete_magic_rejected() {
    std::cout << "Testing INCOMPLETE magic rejection..." << std::endl;
    auto path = tmp_ckpt_path("incomplete");
    CkptCleanup cleanup{path};

    // Hand-roll a file where save() crashed before flipping magic.
    {
        std::ofstream out(path, std::ios::binary);
        uint64_t magic = BlockLanczosCheckpoint::MAGIC_INCOMPLETE;
        uint64_t version = BlockLanczosCheckpoint::VERSION;
        write_le_u64(out, magic);
        write_le_u64(out, version);
        // Pad rest with zeros to keep readers from underflowing
        char zero[8 * 8] = {};
        out.write(zero, sizeof(zero));
    }

    assert(!BlockLanczosCheckpoint::exists_and_valid(path));
    auto loaded = BlockLanczosCheckpoint::load(path);
    assert(!loaded.has_value());

    std::cout << "  INCOMPLETE rejection: PASS" << std::endl;
}

void test_corrupt_header_checksum_rejected() {
    std::cout << "Testing corrupt header checksum rejection..." << std::endl;
    auto path = tmp_ckpt_path("corrupt_header");
    CkptCleanup cleanup{path};

    auto orig = make_small_state();
    require_save(orig, path);

    // Flip one byte inside the header (e.g. byte 24 = start of `rows` field).
    {
        std::fstream fs(path, std::ios::binary | std::ios::in | std::ios::out);
        assert(fs);
        fs.seekp(24);
        char byte = 0;
        fs.read(&byte, 1);
        byte ^= 0x01;
        fs.seekp(24);
        fs.write(&byte, 1);
    }

    auto loaded = BlockLanczosCheckpoint::load(path);
    assert(!loaded.has_value());

    std::cout << "  corrupt header checksum rejection: PASS" << std::endl;
}

void test_corrupt_body_checksum_rejected() {
    std::cout << "Testing corrupt body checksum rejection..." << std::endl;
    auto path = tmp_ckpt_path("corrupt_body");
    CkptCleanup cleanup{path};

    auto orig = make_small_state();
    require_save(orig, path);

    // Flip the very last byte (which is part of body_csum).
    {
        std::fstream fs(path, std::ios::binary | std::ios::in | std::ios::out);
        assert(fs);
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
    assert(!loaded.has_value());

    std::cout << "  corrupt body checksum rejection: PASS" << std::endl;
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
    assert(!loaded.has_value());

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
    assert(!loaded.has_value());

    std::cout << "  truncated file rejection: PASS" << std::endl;
}

void test_load_nonexistent() {
    std::cout << "Testing load nonexistent file..." << std::endl;
    const std::string path = gnfs::util::temp_path("__nonexistent_bl_ckpt_xyz_12345");
    auto loaded = BlockLanczosCheckpoint::load(path);
    assert(!loaded.has_value());
    assert(!BlockLanczosCheckpoint::exists_and_valid(path));
    std::cout << "  nonexistent file: PASS" << std::endl;
}

void test_exists_and_valid_semantics() {
    std::cout << "Testing exists_and_valid semantics..." << std::endl;
    auto path = tmp_ckpt_path("exists");
    CkptCleanup cleanup{path};

    assert(!BlockLanczosCheckpoint::exists_and_valid(path)); // doesn't exist
    auto orig = make_small_state();
    require_save(orig, path);
    assert(BlockLanczosCheckpoint::exists_and_valid(path));

    std::cout << "  exists_and_valid semantics: PASS" << std::endl;
}

void test_remove() {
    std::cout << "Testing remove()..." << std::endl;
    auto path = tmp_ckpt_path("remove");

    auto orig = make_small_state();
    require_save(orig, path);
    assert(BlockLanczosCheckpoint::exists_and_valid(path));

    BlockLanczosCheckpoint::remove(path);
    assert(!BlockLanczosCheckpoint::exists_and_valid(path));

    std::cout << "  remove: PASS" << std::endl;
}

void test_interval_env_parser() {
    std::cout << "Testing GNFS_BL_CHECKPOINT_INTERVAL parser..." << std::endl;

    // Defensive: unset → default 50.
    ::unsetenv("GNFS_BL_CHECKPOINT_INTERVAL");
    assert(gnfs::linalg::bl_checkpoint_interval() == 50);

    ::setenv("GNFS_BL_CHECKPOINT_INTERVAL", "10", 1);
    assert(gnfs::linalg::bl_checkpoint_interval() == 10);

    ::setenv("GNFS_BL_CHECKPOINT_INTERVAL", "0", 1);
    assert(gnfs::linalg::bl_checkpoint_interval() == 50); // 0 → default

    ::setenv("GNFS_BL_CHECKPOINT_INTERVAL", "9999999999", 1);
    assert(gnfs::linalg::bl_checkpoint_interval() == 1'000'000); // clamped

    ::setenv("GNFS_BL_CHECKPOINT_INTERVAL", "notanumber", 1);
    assert(gnfs::linalg::bl_checkpoint_interval() == 50);

    ::setenv("GNFS_BL_CHECKPOINT_INTERVAL", "", 1);
    assert(gnfs::linalg::bl_checkpoint_interval() == 50);

    ::unsetenv("GNFS_BL_CHECKPOINT_INTERVAL");
    std::cout << "  interval parser: PASS" << std::endl;
}

void test_base_path_env_parser() {
    std::cout << "Testing GNFS_BL_CHECKPOINT base path parser..." << std::endl;
    ::unsetenv("GNFS_BL_CHECKPOINT");
    assert(gnfs::linalg::bl_checkpoint_base_path().empty());
    assert(gnfs::linalg::bl_checkpoint_full_path().empty());

    ::setenv("GNFS_BL_CHECKPOINT", "/tmp/foo_session", 1);
    assert(gnfs::linalg::bl_checkpoint_base_path() == "/tmp/foo_session");
    assert(gnfs::linalg::bl_checkpoint_full_path() == "/tmp/foo_session.bl_ckpt");

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
    assert(loaded.has_value());
    assert(loaded->iteration == 999);
    assert(loaded->pivot_row == 7);
    assert(loaded->aug[0] == 0x1234'5678'9ABC'DEF0ULL);

    std::cout << "  overwrite existing: PASS" << std::endl;
}

void test_wpr_mismatch_rejected() {
    std::cout << "Testing wpr mismatch in payload rejected..." << std::endl;
    auto path = tmp_ckpt_path("wpr_mismatch");
    CkptCleanup cleanup{path};

    // Hand-craft a file: claim rows=4, wpr=2 → 8 words, but write 7 words.
    {
        std::ofstream out(path, std::ios::binary);
        uint64_t magic = BlockLanczosCheckpoint::MAGIC;
        uint64_t version = BlockLanczosCheckpoint::VERSION;
        uint64_t rows = 4, cols = 100, wpr = 2, pivot_row = 0;
        uint64_t cur_col = 100, iteration = 0;
        uint64_t header_csum = version ^ rows ^ cols ^ wpr ^ pivot_row ^ cur_col ^ iteration;
        uint64_t aug_word_count = 7; // mismatch: rows*wpr = 8
        write_le_u64(out, magic);
        write_le_u64(out, version);
        write_le_u64(out, rows);
        write_le_u64(out, cols);
        write_le_u64(out, wpr);
        write_le_u64(out, pivot_row);
        write_le_u64(out, cur_col);
        write_le_u64(out, iteration);
        write_le_u64(out, header_csum);
        write_le_u64(out, aug_word_count);
        uint64_t dummy[7] = {};
        for (uint64_t word : dummy)
            write_le_u64(out, word);
        uint64_t bcsum = 0;
        write_le_u64(out, bcsum);
    }

    auto loaded = BlockLanczosCheckpoint::load(path);
    assert(!loaded.has_value());

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
    constexpr uint64_t header_checksum =
        BlockLanczosCheckpoint::VERSION_V2 ^ rows ^ cols ^ wpr ^ pivot_row ^ cur_col ^ iteration;

    // The loader rejects the count before checking the payload frame or
    // allocating a vector, so the fixture needs only its fixed header.
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out);
    write_le_u64(out, BlockLanczosCheckpoint::MAGIC);
    write_le_u64(out, BlockLanczosCheckpoint::VERSION_V2);
    write_le_u64(out, rows);
    write_le_u64(out, cols);
    write_le_u64(out, wpr);
    write_le_u64(out, pivot_row);
    write_le_u64(out, cur_col);
    write_le_u64(out, iteration);
    write_le_u64(out, header_checksum);
    write_le_u64(out, wpr);
    out.close();
    assert(out);

    const auto loaded = BlockLanczosCheckpoint::load(path);
    assert(!loaded.has_value());
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
    // allocation in the child and return nullopt rather than terminate.
    constexpr uint64_t max_aug_words = BlockLanczosCheckpoint::MAX_AUG_WORDS;
    // The first ten u64 fields (through aug_word_count) occupy 80 bytes;
    // the trailing body checksum makes the fixed non-payload size 88 bytes.
    constexpr uint64_t payload_offset = 10ULL * 8ULL;
    constexpr uint64_t fixed_file_bytes = 11ULL * 8ULL;
    constexpr uint64_t rows = 1;
    constexpr uint64_t cols = 1;
    constexpr uint64_t wpr = max_aug_words;
    constexpr uint64_t pivot_row = 0;
    constexpr uint64_t cur_col = 1;
    constexpr uint64_t iteration = 0;
    constexpr uint64_t header_checksum =
        BlockLanczosCheckpoint::VERSION_V2 ^ rows ^ cols ^ wpr ^ pivot_row ^ cur_col ^ iteration;

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(out), "failed to create sparse checkpoint");
        write_le_u64(out, BlockLanczosCheckpoint::MAGIC);
        write_le_u64(out, BlockLanczosCheckpoint::VERSION_V2);
        write_le_u64(out, rows);
        write_le_u64(out, cols);
        write_le_u64(out, wpr);
        write_le_u64(out, pivot_row);
        write_le_u64(out, cur_col);
        write_le_u64(out, iteration);
        write_le_u64(out, header_checksum);
        write_le_u64(out, max_aug_words);

        // Leave the payload sparse and write only its trailing checksum. The
        // resulting file size is exactly what load() expects.
        const uint64_t checksum_offset = payload_offset + max_aug_words * 8ULL;
        require(checksum_offset + sizeof(uint64_t) == fixed_file_bytes + max_aug_words * 8ULL,
                "sparse checkpoint checksum offset is inconsistent");
        require(checksum_offset <=
                    static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()),
                "checkpoint offset exceeds streamoff");
        out.seekp(static_cast<std::streamoff>(checksum_offset));
        require(static_cast<bool>(out), "failed to seek sparse checkpoint");
        write_le_u64(out, 0);
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
    test_v2_little_endian_fixture();
    test_v1_native_compatibility();
    test_roundtrip_large();
    test_empty_matrix();
    test_incomplete_magic_rejected();
    test_corrupt_header_checksum_rejected();
    test_corrupt_body_checksum_rejected();
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
