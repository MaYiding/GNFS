#include "gnfs/linalg/bl_checkpoint.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using gnfs::linalg::BlockLanczosCheckpoint;

// Helper: temp file path that does not collide between cases
static std::string tmp_ckpt_path(const char* label) {
    static int seq = 0;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "gnfs_test_bl_ckpt_%d_%d_%s",
                  gnfs::util::process_id(), ++seq, label);
    return gnfs::util::temp_path(buf);
}

struct CkptCleanup {
    std::string path;
    ~CkptCleanup() {
        if (!path.empty()) std::remove(path.c_str());
    }
};

static void require_save(const BlockLanczosCheckpoint& ck,
                         const std::string& path) {
    if (!ck.save(path)) {
        std::cerr << "ERROR: failed to save checkpoint to " << path
                  << std::endl;
        std::abort();
    }
}

// Helper: rebuild a small deterministic aug payload for round-trip checks.
static BlockLanczosCheckpoint make_small_state() {
    BlockLanczosCheckpoint ck;
    ck.rows = 8;
    ck.cols = 16;
    ck.aug_words_per_row = 1;  // 8+16 = 24 cols → 1 word per row
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

void test_roundtrip_large() {
    std::cout << "Testing roundtrip (large 1 MB payload)..." << std::endl;
    auto path = tmp_ckpt_path("large");
    CkptCleanup cleanup{path};

    BlockLanczosCheckpoint orig;
    orig.rows = 4096;
    orig.cols = 4096;
    orig.aug_words_per_row = 128;  // 4096+4096 = 8192 bits / 64 = 128 words
    orig.pivot_row = 2000;
    orig.cur_col = 4096 + 1500;
    orig.iteration = 1234;
    orig.aug.resize(orig.rows * orig.aug_words_per_row);

    std::mt19937_64 rng(42);
    for (auto& w : orig.aug) w = rng();

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

    BlockLanczosCheckpoint orig;  // all zero
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
        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.write(reinterpret_cast<const char*>(&version), 8);
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
        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.write(reinterpret_cast<const char*>(&version), 8);
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
            std::cerr << "ERROR: failed to open checkpoint for truncation"
                      << std::endl;
            std::abort();
        }
        auto pos = src.tellg();
        if (pos == std::ifstream::pos_type(-1)) {
            std::cerr << "ERROR: failed to determine checkpoint size"
                      << std::endl;
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

    assert(!BlockLanczosCheckpoint::exists_and_valid(path));  // doesn't exist
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
    assert(gnfs::linalg::bl_checkpoint_interval() == 50);  // 0 → default

    ::setenv("GNFS_BL_CHECKPOINT_INTERVAL", "9999999999", 1);
    assert(gnfs::linalg::bl_checkpoint_interval() == 1'000'000);  // clamped

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
    assert(gnfs::linalg::bl_checkpoint_full_path()
           == "/tmp/foo_session.bl_ckpt");

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
        uint64_t header_csum = version ^ rows ^ cols ^ wpr ^ pivot_row
                              ^ cur_col ^ iteration;
        uint64_t aug_word_count = 7;  // mismatch: rows*wpr = 8
        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.write(reinterpret_cast<const char*>(&version), 8);
        out.write(reinterpret_cast<const char*>(&rows), 8);
        out.write(reinterpret_cast<const char*>(&cols), 8);
        out.write(reinterpret_cast<const char*>(&wpr), 8);
        out.write(reinterpret_cast<const char*>(&pivot_row), 8);
        out.write(reinterpret_cast<const char*>(&cur_col), 8);
        out.write(reinterpret_cast<const char*>(&iteration), 8);
        out.write(reinterpret_cast<const char*>(&header_csum), 8);
        out.write(reinterpret_cast<const char*>(&aug_word_count), 8);
        uint64_t dummy[7] = {};
        out.write(reinterpret_cast<const char*>(dummy), 8 * 7);
        uint64_t bcsum = 0;
        out.write(reinterpret_cast<const char*>(&bcsum), 8);
    }

    auto loaded = BlockLanczosCheckpoint::load(path);
    assert(!loaded.has_value());

    std::cout << "  wpr mismatch rejection: PASS" << std::endl;
}

int main() {
    std::cout << "===== BlockLanczosCheckpoint Tests =====" << std::endl;

    test_roundtrip_small();
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

    std::cout << "\n===== All BlockLanczosCheckpoint tests PASSED ====="
              << std::endl;
    return 0;
}
