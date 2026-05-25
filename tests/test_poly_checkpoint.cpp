#include "gnfs/polynomial/poly_checkpoint.hpp"
#include "gnfs/util/process.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

using namespace gnfs::polynomial;
using gnfs::core::Integer;
using gnfs::core::PolynomialContext;

static std::string tmp_ckpt_path(const char* label) {
    static int seq = 0;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/gnfs_test_poly_ckpt_%d_%d_%s",
                  gnfs::util::process_id(), ++seq, label);
    return std::string(buf);
}

struct CkptCleanup {
    std::string path;
    ~CkptCleanup() { if (!path.empty()) std::remove(path.c_str()); }
};

void test_roundtrip_small() {
    std::cout << "Testing roundtrip (small Integers)..." << std::endl;
    auto path = tmp_ckpt_path("rt_small");
    CkptCleanup c{path};

    PolyCheckpoint orig;
    orig.n = Integer("12345678901234567");
    orig.m = Integer("123456");
    orig.degree = 3;
    orig.f_coeffs.emplace_back(Integer(static_cast<int64_t>(-5)));
    orig.f_coeffs.emplace_back(Integer(static_cast<int64_t>(7)));
    orig.f_coeffs.emplace_back(Integer(static_cast<int64_t>(2)));
    orig.f_coeffs.emplace_back(Integer(static_cast<int64_t>(11)));
    orig.skewness = 1234.5;
    orig.murphy_e = -25.6;

    orig.save(path);
    auto loaded = PolyCheckpoint::load(path);

    assert(loaded.n == orig.n);
    assert(loaded.m == orig.m);
    assert(loaded.degree == orig.degree);
    assert(loaded.f_coeffs.size() == 4);
    assert(loaded.f_coeffs[0] == Integer(static_cast<int64_t>(-5)));
    assert(loaded.f_coeffs[1] == Integer(static_cast<int64_t>(7)));
    assert(loaded.f_coeffs[2] == Integer(static_cast<int64_t>(2)));
    assert(loaded.f_coeffs[3] == Integer(static_cast<int64_t>(11)));
    assert(loaded.skewness == orig.skewness);
    assert(loaded.murphy_e == orig.murphy_e);
    std::cout << "  Small roundtrip: PASS" << std::endl;
}

void test_roundtrip_large() {
    std::cout << "Testing roundtrip (multi-limb large Integers)..." << std::endl;
    auto path = tmp_ckpt_path("rt_large");
    CkptCleanup c{path};

    PolyCheckpoint orig;
    // 200-bit composite-ish N
    orig.n = Integer("1606938044258990275541962092341162602522202993782792835301376");
    orig.m = Integer("123456789012345678901234567890");
    orig.degree = 5;
    for (int i = 0; i < 6; ++i) {
        Integer c(Integer("1000000000000000000000") + Integer(static_cast<int64_t>(i * 17 - 30)));
        orig.f_coeffs.emplace_back(std::move(c));
    }
    orig.skewness = 1e7;
    orig.murphy_e = -42.123;

    orig.save(path);
    auto loaded = PolyCheckpoint::load(path);

    assert(loaded.n == orig.n);
    assert(loaded.m == orig.m);
    assert(loaded.degree == 5);
    assert(loaded.f_coeffs.size() == 6);
    for (size_t i = 0; i < 6; ++i) {
        assert(loaded.f_coeffs[i] == orig.f_coeffs[i]);
    }
    assert(loaded.skewness == orig.skewness);
    std::cout << "  Large roundtrip: PASS" << std::endl;
}

void test_negative_coefficients() {
    std::cout << "Testing negative + zero coefficients..." << std::endl;
    auto path = tmp_ckpt_path("neg_coeff");
    CkptCleanup c{path};

    PolyCheckpoint orig;
    orig.n = Integer("99991");
    orig.m = Integer("42");
    orig.degree = 4;
    // Mix: zero, large positive, large negative, small negative, small positive
    orig.f_coeffs.emplace_back(Integer(static_cast<int64_t>(0)));
    orig.f_coeffs.emplace_back(Integer("123456789012345"));
    orig.f_coeffs.emplace_back(Integer("-987654321098765"));
    orig.f_coeffs.emplace_back(Integer(static_cast<int64_t>(-1)));
    orig.f_coeffs.emplace_back(Integer(static_cast<int64_t>(1)));
    orig.skewness = 2.0;

    orig.save(path);
    auto loaded = PolyCheckpoint::load(path);

    assert(loaded.f_coeffs[0].is_zero());
    assert(loaded.f_coeffs[1] == Integer("123456789012345"));
    assert(loaded.f_coeffs[2] == Integer("-987654321098765"));
    assert(loaded.f_coeffs[3] == Integer(static_cast<int64_t>(-1)));
    assert(loaded.f_coeffs[4] == Integer(static_cast<int64_t>(1)));
    std::cout << "  Negative + zero coefficients: PASS" << std::endl;
}

void test_context_roundtrip() {
    std::cout << "Testing PolynomialContext to/from checkpoint..." << std::endl;
    auto path = tmp_ckpt_path("ctx_rt");
    CkptCleanup c{path};

    Integer n("314159265358979323846264338327950288419716939937510582097494459");
    std::vector<Integer> coeffs;
    coeffs.emplace_back(Integer(static_cast<int64_t>(-7)));
    coeffs.emplace_back(Integer(static_cast<int64_t>(11)));
    coeffs.emplace_back(Integer(static_cast<int64_t>(13)));
    coeffs.emplace_back(Integer(static_cast<int64_t>(1)));
    Integer m("271828182845904");
    PolynomialContext orig_ctx(Integer(n), std::move(coeffs), Integer(m), 3.14);

    PolyCheckpoint ck = PolyCheckpoint::from_context(orig_ctx, /*murphy_e=*/-30.5);
    ck.save(path);

    auto loaded_ck = PolyCheckpoint::load(path);
    PolynomialContext rebuilt = loaded_ck.to_context();

    assert(rebuilt.n() == orig_ctx.n());
    assert(rebuilt.m() == orig_ctx.m());
    assert(rebuilt.degree() == orig_ctx.degree());
    assert(rebuilt.skewness() == orig_ctx.skewness());
    for (uint32_t i = 0; i <= orig_ctx.degree(); ++i) {
        assert(rebuilt.coeff(i) == orig_ctx.coeff(i));
    }
    std::cout << "  Context roundtrip: PASS" << std::endl;
}

void test_load_for_validates_n() {
    std::cout << "Testing load_for N validation..." << std::endl;
    auto path = tmp_ckpt_path("load_for");
    CkptCleanup c{path};

    PolyCheckpoint orig;
    orig.n = Integer("999999999999");
    orig.m = Integer("123");
    orig.degree = 2;
    orig.f_coeffs.emplace_back(Integer(static_cast<int64_t>(1)));
    orig.f_coeffs.emplace_back(Integer(static_cast<int64_t>(2)));
    orig.f_coeffs.emplace_back(Integer(static_cast<int64_t>(3)));
    orig.save(path);

    // Correct N → ok
    auto ck = PolyCheckpoint::load_for(path, Integer("999999999999"));
    assert(ck.n == Integer("999999999999"));

    // Wrong N → throws
    bool threw = false;
    try {
        (void) PolyCheckpoint::load_for(path, Integer("999999999998"));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    std::cout << "  load_for N validation: PASS" << std::endl;
}

void test_exists_and_valid() {
    std::cout << "Testing exists_and_valid..." << std::endl;
    auto path = tmp_ckpt_path("valid");
    CkptCleanup c{path};

    assert(!PolyCheckpoint::exists_and_valid(path));

    PolyCheckpoint ck;
    ck.n = Integer("17");
    ck.m = Integer("2");
    ck.degree = 1;
    ck.f_coeffs.emplace_back(Integer(static_cast<int64_t>(-2)));
    ck.f_coeffs.emplace_back(Integer(static_cast<int64_t>(1)));
    ck.save(path);

    assert(PolyCheckpoint::exists_and_valid(path));
    std::cout << "  exists_and_valid: PASS" << std::endl;
}

void test_incomplete_magic_rejected() {
    std::cout << "Testing INCOMPLETE magic rejected..." << std::endl;
    auto path = tmp_ckpt_path("incomplete");
    CkptCleanup c{path};

    {
        std::ofstream out(path, std::ios::binary);
        uint64_t magic = PolyCheckpoint::MAGIC_INCOMPLETE;
        uint64_t version = PolyCheckpoint::VERSION;
        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.write(reinterpret_cast<const char*>(&version), 8);
        // Truncate intentionally (mid-save crash simulation)
    }

    assert(!PolyCheckpoint::exists_and_valid(path));
    bool threw = false;
    try {
        (void) PolyCheckpoint::load(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    std::cout << "  INCOMPLETE rejected: PASS" << std::endl;
}

void test_version_mismatch_rejected() {
    std::cout << "Testing version mismatch rejected..." << std::endl;
    auto path = tmp_ckpt_path("ver_bad");
    CkptCleanup c{path};

    {
        std::ofstream out(path, std::ios::binary);
        uint64_t magic = PolyCheckpoint::MAGIC;
        uint64_t version = 999;
        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.write(reinterpret_cast<const char*>(&version), 8);
    }
    bool threw = false;
    try { (void) PolyCheckpoint::load(path); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);
    std::cout << "  Version mismatch: PASS" << std::endl;
}

void test_corrupt_coeff_count_rejected() {
    std::cout << "Testing corrupt coeff_count rejected..." << std::endl;
    auto path = tmp_ckpt_path("coeff_corrupt");
    CkptCleanup c{path};

    {
        std::ofstream out(path, std::ios::binary);
        uint64_t magic = PolyCheckpoint::MAGIC;
        uint64_t version = PolyCheckpoint::VERSION;
        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.write(reinterpret_cast<const char*>(&version), 8);

        // Write N=0
        int32_t sgn = 0;
        uint32_t bc = 0;
        out.write(reinterpret_cast<const char*>(&sgn), 4);
        out.write(reinterpret_cast<const char*>(&bc), 4);
        // Write m=0
        out.write(reinterpret_cast<const char*>(&sgn), 4);
        out.write(reinterpret_cast<const char*>(&bc), 4);

        uint32_t degree = 5;
        out.write(reinterpret_cast<const char*>(&degree), 4);
        uint32_t coeff_count = 999;  // > 64 cap → corrupt
        out.write(reinterpret_cast<const char*>(&coeff_count), 4);
    }
    bool threw = false;
    try { (void) PolyCheckpoint::load(path); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);
    std::cout << "  Corrupt coeff_count: PASS" << std::endl;
}

void test_remove() {
    std::cout << "Testing remove..." << std::endl;
    auto path = tmp_ckpt_path("rm");
    PolyCheckpoint ck;
    ck.n = Integer("11"); ck.m = Integer("1"); ck.degree = 1;
    ck.f_coeffs.emplace_back(Integer(static_cast<int64_t>(0)));
    ck.f_coeffs.emplace_back(Integer(static_cast<int64_t>(1)));
    ck.save(path);
    assert(PolyCheckpoint::exists_and_valid(path));
    PolyCheckpoint::remove(path);
    assert(!PolyCheckpoint::exists_and_valid(path));
    std::cout << "  Remove: PASS" << std::endl;
}

void test_load_nonexistent() {
    std::cout << "Testing load nonexistent..." << std::endl;
    bool threw = false;
    try {
        (void) PolyCheckpoint::load("/tmp/nonexistent_xyz_gnfs_poly_xx_99999");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    assert(!PolyCheckpoint::exists_and_valid("/tmp/nonexistent_xyz_gnfs_poly_xx_99999"));
    std::cout << "  Nonexistent: PASS" << std::endl;
}

void test_allow_incomplete_force() {
    std::cout << "Testing allow_incomplete force-load..." << std::endl;
    auto path = tmp_ckpt_path("incomplete_force");
    CkptCleanup c{path};

    // Manually fabricate a fully-populated body with INCOMPLETE magic
    {
        std::ofstream out(path, std::ios::binary);
        uint64_t magic = PolyCheckpoint::MAGIC_INCOMPLETE;
        uint64_t version = PolyCheckpoint::VERSION;
        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.write(reinterpret_cast<const char*>(&version), 8);

        // N = 7 (positive, 1 byte)
        int32_t sgn = 1;
        uint32_t bc = 1;
        uint8_t byte = 7;
        out.write(reinterpret_cast<const char*>(&sgn), 4);
        out.write(reinterpret_cast<const char*>(&bc), 4);
        out.write(reinterpret_cast<const char*>(&byte), 1);

        // m = 3
        byte = 3;
        out.write(reinterpret_cast<const char*>(&sgn), 4);
        out.write(reinterpret_cast<const char*>(&bc), 4);
        out.write(reinterpret_cast<const char*>(&byte), 1);

        uint32_t degree = 1;
        uint32_t coeff_count = 2;
        out.write(reinterpret_cast<const char*>(&degree), 4);
        out.write(reinterpret_cast<const char*>(&coeff_count), 4);

        // coeff 0 = -2 (sgn=-1, 1 byte)
        sgn = -1; bc = 1; byte = 2;
        out.write(reinterpret_cast<const char*>(&sgn), 4);
        out.write(reinterpret_cast<const char*>(&bc), 4);
        out.write(reinterpret_cast<const char*>(&byte), 1);
        // coeff 1 = +1
        sgn = 1; bc = 1; byte = 1;
        out.write(reinterpret_cast<const char*>(&sgn), 4);
        out.write(reinterpret_cast<const char*>(&bc), 4);
        out.write(reinterpret_cast<const char*>(&byte), 1);

        double skew = 1.5, murphy = -10.0;
        out.write(reinterpret_cast<const char*>(&skew), 8);
        out.write(reinterpret_cast<const char*>(&murphy), 8);
    }

    // Strict load throws
    bool threw = false;
    try { (void) PolyCheckpoint::load(path); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);

    // Force load succeeds
    auto ck = PolyCheckpoint::load(path, /*allow_incomplete=*/true);
    assert(ck.n == Integer(static_cast<int64_t>(7)));
    assert(ck.m == Integer(static_cast<int64_t>(3)));
    assert(ck.degree == 1);
    assert(ck.f_coeffs.size() == 2);
    assert(ck.f_coeffs[0] == Integer(static_cast<int64_t>(-2)));
    assert(ck.f_coeffs[1] == Integer(static_cast<int64_t>(1)));
    std::cout << "  Force-load INCOMPLETE: PASS" << std::endl;
}

int main() {
    std::cout << "===== PolyCheckpoint Tests =====" << std::endl;
    test_roundtrip_small();
    test_roundtrip_large();
    test_negative_coefficients();
    test_context_roundtrip();
    test_load_for_validates_n();
    test_exists_and_valid();
    test_incomplete_magic_rejected();
    test_version_mismatch_rejected();
    test_corrupt_coeff_count_rejected();
    test_remove();
    test_load_nonexistent();
    test_allow_incomplete_force();
    std::cout << "\n===== All PolyCheckpoint tests PASSED =====" << std::endl;
    return 0;
}
