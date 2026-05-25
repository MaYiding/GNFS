#include "gnfs/sieve/sieve_checkpoint.hpp"
#include "gnfs/util/process.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

using namespace gnfs::sieve;

// Helper: temp file path
static std::string tmp_ckpt_path(const char* label) {
    static int seq = 0;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/gnfs_test_sieve_ckpt_%d_%d_%s",
                  gnfs::util::process_id(), ++seq, label);
    return std::string(buf);
}

struct CkptCleanup {
    std::string path;
    ~CkptCleanup() {
        if (!path.empty()) std::remove(path.c_str());
    }
};

void test_roundtrip() {
    std::cout << "Testing checkpoint roundtrip..." << std::endl;

    auto path = tmp_ckpt_path("roundtrip");
    CkptCleanup cleanup{path};

    SieveCheckpoint orig;
    orig.sq_count = 12345;
    orig.current_index = 6789;
    orig.round = 3;
    orig.batch_target = 1'500'000;
    orig.candidates_total = 9'876'543;
    orig.ooc_base_path = "/tmp/gnfs_test_relations_abc123";

    orig.save(path);

    auto loaded = SieveCheckpoint::load(path);
    assert(loaded.sq_count == orig.sq_count);
    assert(loaded.current_index == orig.current_index);
    assert(loaded.round == orig.round);
    assert(loaded.batch_target == orig.batch_target);
    assert(loaded.candidates_total == orig.candidates_total);
    assert(loaded.ooc_base_path == orig.ooc_base_path);

    std::cout << "  Roundtrip: PASS" << std::endl;
}

void test_empty_path() {
    std::cout << "Testing empty OOC path..." << std::endl;

    auto path = tmp_ckpt_path("empty_path");
    CkptCleanup cleanup{path};

    SieveCheckpoint orig;
    orig.sq_count = 42;
    orig.current_index = 7;
    orig.ooc_base_path = "";

    orig.save(path);

    auto loaded = SieveCheckpoint::load(path);
    assert(loaded.sq_count == 42);
    assert(loaded.current_index == 7);
    assert(loaded.ooc_base_path.empty());

    std::cout << "  Empty path: PASS" << std::endl;
}

void test_exists_and_valid() {
    std::cout << "Testing exists_and_valid..." << std::endl;

    auto path = tmp_ckpt_path("valid_check");
    CkptCleanup cleanup{path};

    // Doesn't exist yet
    assert(!SieveCheckpoint::exists_and_valid(path));

    SieveCheckpoint ck;
    ck.sq_count = 1;
    ck.ooc_base_path = "/tmp/x";
    ck.save(path);

    // After save: should be valid
    assert(SieveCheckpoint::exists_and_valid(path));

    std::cout << "  Exists check: PASS" << std::endl;
}

void test_incomplete_magic_rejected() {
    std::cout << "Testing INCOMPLETE magic rejection..." << std::endl;

    auto path = tmp_ckpt_path("incomplete");
    CkptCleanup cleanup{path};

    // Manually write INCOMPLETE header (simulate crash mid-save)
    {
        std::ofstream out(path, std::ios::binary);
        uint64_t magic = SieveCheckpoint::MAGIC_INCOMPLETE;
        uint64_t version = SieveCheckpoint::VERSION;
        uint64_t sq_count = 100;
        uint32_t current_index = 50;
        int32_t round = 1;
        uint64_t batch_target = 1000;
        uint64_t candidates_total = 5000;
        uint32_t path_len = 0;

        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.write(reinterpret_cast<const char*>(&version), 8);
        out.write(reinterpret_cast<const char*>(&sq_count), 8);
        out.write(reinterpret_cast<const char*>(&current_index), 4);
        out.write(reinterpret_cast<const char*>(&round), 4);
        out.write(reinterpret_cast<const char*>(&batch_target), 8);
        out.write(reinterpret_cast<const char*>(&candidates_total), 8);
        out.write(reinterpret_cast<const char*>(&path_len), 4);
    }

    // exists_and_valid 拒绝
    assert(!SieveCheckpoint::exists_and_valid(path));

    // 默认 load 抛
    bool threw = false;
    try {
        (void) SieveCheckpoint::load(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    // allow_incomplete=true 通过
    auto loaded = SieveCheckpoint::load(path, /*allow_incomplete=*/true);
    assert(loaded.sq_count == 100);
    assert(loaded.current_index == 50);

    std::cout << "  INCOMPLETE rejection + force-resume: PASS" << std::endl;
}

void test_corrupt_path_len_rejected() {
    std::cout << "Testing corrupt path_len rejection..." << std::endl;

    auto path = tmp_ckpt_path("corrupt_path");
    CkptCleanup cleanup{path};

    // Manually write valid magic + huge path_len (corrupt)
    {
        std::ofstream out(path, std::ios::binary);
        uint64_t magic = SieveCheckpoint::MAGIC;
        uint64_t version = SieveCheckpoint::VERSION;
        uint64_t sq_count = 0;
        uint32_t current_index = 0;
        int32_t round = 0;
        uint64_t batch_target = 0, candidates_total = 0;
        uint32_t path_len = 1'000'000'000;  // 1 GB — clearly corrupt

        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.write(reinterpret_cast<const char*>(&version), 8);
        out.write(reinterpret_cast<const char*>(&sq_count), 8);
        out.write(reinterpret_cast<const char*>(&current_index), 4);
        out.write(reinterpret_cast<const char*>(&round), 4);
        out.write(reinterpret_cast<const char*>(&batch_target), 8);
        out.write(reinterpret_cast<const char*>(&candidates_total), 8);
        out.write(reinterpret_cast<const char*>(&path_len), 4);
    }

    bool threw = false;
    try {
        (void) SieveCheckpoint::load(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "  Corrupt path_len rejection: PASS" << std::endl;
}

void test_version_mismatch_rejected() {
    std::cout << "Testing version mismatch rejection..." << std::endl;

    auto path = tmp_ckpt_path("version_mismatch");
    CkptCleanup cleanup{path};

    {
        std::ofstream out(path, std::ios::binary);
        uint64_t magic = SieveCheckpoint::MAGIC;
        uint64_t version = 999;  // future version

        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.write(reinterpret_cast<const char*>(&version), 8);
        // 不写其他, load 应在 version check 时 throw
    }

    bool threw = false;
    try {
        (void) SieveCheckpoint::load(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "  Version mismatch rejection: PASS" << std::endl;
}

void test_remove() {
    std::cout << "Testing checkpoint remove..." << std::endl;

    auto path = tmp_ckpt_path("remove");
    SieveCheckpoint ck;
    ck.save(path);
    assert(SieveCheckpoint::exists_and_valid(path));

    SieveCheckpoint::remove(path);
    assert(!SieveCheckpoint::exists_and_valid(path));

    std::cout << "  Remove: PASS" << std::endl;
}

void test_load_nonexistent() {
    std::cout << "Testing load nonexistent file..." << std::endl;

    bool threw = false;
    try {
        (void) SieveCheckpoint::load("/tmp/nonexistent_gnfs_ckpt_xyz_12345");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    // exists_and_valid returns false (does not throw)
    assert(!SieveCheckpoint::exists_and_valid("/tmp/nonexistent_gnfs_ckpt_xyz_12345"));

    std::cout << "  Nonexistent file: PASS" << std::endl;
}

void test_long_path() {
    std::cout << "Testing long OOC path..." << std::endl;

    auto path = tmp_ckpt_path("long_path");
    CkptCleanup cleanup{path};

    SieveCheckpoint orig;
    orig.sq_count = 100;
    orig.ooc_base_path = std::string(2048, 'x');  // 2 KB path

    orig.save(path);
    auto loaded = SieveCheckpoint::load(path);
    assert(loaded.ooc_base_path == orig.ooc_base_path);

    std::cout << "  Long path: PASS" << std::endl;
}

int main() {
    std::cout << "===== SieveCheckpoint Tests =====" << std::endl;

    test_roundtrip();
    test_empty_path();
    test_exists_and_valid();
    test_incomplete_magic_rejected();
    test_corrupt_path_len_rejected();
    test_version_mismatch_rejected();
    test_remove();
    test_load_nonexistent();
    test_long_path();

    std::cout << "\n===== All SieveCheckpoint tests PASSED =====" << std::endl;
    return 0;
}
