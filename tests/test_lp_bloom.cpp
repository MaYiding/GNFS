// Unit tests for the LP key Bloom pre-screen helper
// (include/gnfs/relation/lp_bloom.hpp).
//
// Verifies:
//   1. ENV parsing matrix for GNFS_FILTER_LP_BLOOM_BITS (unset / "0" /
//      negative / non-numeric / empty all → 0; in-range stays as-is;
//      out-of-range clamps).
//   2. Bloom filter invariants: no false negatives, bounded false
//      positives, expected bits set per insert.
//   3. `count_unique_with_bloom` is bit-for-bit identical to the pure
//      `std::unordered_set` baseline across `bloom_bits` sweeps.
//   4. Edge cases (empty input, single key, all-duplicate input,
//      below-floor ctor throws).

#include "gnfs/relation/lp_bloom.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <vector>

using namespace gnfs::relation;

namespace {

void set_env_and_reload(const char* value) {
    if (value == nullptr) {
        ::unsetenv("GNFS_FILTER_LP_BLOOM_BITS");
    } else {
        ::setenv("GNFS_FILTER_LP_BLOOM_BITS", value, 1);
    }
    filter_lp_bloom_reset_env_cache_for_testing();
}

}  // namespace

// ---------- ENV parsing tests ----------

void test_env_unset_is_disabled() {
    std::cout << "Testing ENV unset → disabled..." << std::endl;
    set_env_and_reload(nullptr);
    assert(filter_lp_bloom_bits() == 0);
    assert(!filter_lp_bloom_enabled());
    std::cout << "  PASS" << std::endl;
}

void test_env_in_range_value() {
    std::cout << "Testing ENV in-range values..." << std::endl;
    set_env_and_reload("10");
    assert(filter_lp_bloom_bits() == 10);
    assert(filter_lp_bloom_enabled());

    set_env_and_reload("14");
    assert(filter_lp_bloom_bits() == 14);

    set_env_and_reload("16");
    assert(filter_lp_bloom_bits() == 16);
    assert(filter_lp_bloom_enabled());

    set_env_and_reload("22");
    assert(filter_lp_bloom_bits() == 22);

    set_env_and_reload("28");
    assert(filter_lp_bloom_bits() == 28);

    set_env_and_reload(nullptr);
    std::cout << "  PASS" << std::endl;
}

void test_env_invalid_strings_are_disabled() {
    std::cout << "Testing ENV invalid strings → disabled..." << std::endl;

    set_env_and_reload("garbage");
    assert(filter_lp_bloom_bits() == 0);

    set_env_and_reload("");
    assert(filter_lp_bloom_bits() == 0);

    set_env_and_reload("-5");
    assert(filter_lp_bloom_bits() == 0);

    set_env_and_reload("-0");
    assert(filter_lp_bloom_bits() == 0);

    // Strtol accepts a numeric prefix; documented behaviour is to take
    // the leading digits. "16abc" parses to 16 which is in-range.
    // Make sure that doesn't make the gate silently enable on noise:
    // pure non-numeric prefixes ("abc16") yield 0.
    set_env_and_reload("abc16");
    assert(filter_lp_bloom_bits() == 0);

    set_env_and_reload(nullptr);
    std::cout << "  PASS" << std::endl;
}

void test_env_clamping() {
    std::cout << "Testing ENV clamping at boundaries..." << std::endl;

    // Below the 10-bit floor → 0 (disabled).
    set_env_and_reload("5");
    assert(filter_lp_bloom_bits() == 0);

    set_env_and_reload("9");
    assert(filter_lp_bloom_bits() == 0);

    // At the floor.
    set_env_and_reload("10");
    assert(filter_lp_bloom_bits() == 10);

    // Above the 28-bit ceiling → 28.
    set_env_and_reload("29");
    assert(filter_lp_bloom_bits() == 28);

    set_env_and_reload("100");
    assert(filter_lp_bloom_bits() == 28);

    set_env_and_reload("9999999");
    assert(filter_lp_bloom_bits() == 28);

    set_env_and_reload(nullptr);
    std::cout << "  PASS" << std::endl;
}

// ---------- Bloom filter behaviour ----------

void test_bloom_basic_insert_and_contains() {
    std::cout << "Testing Bloom basic insert + contains..." << std::endl;
    BloomLPKeyFilter bloom(14);  // 16 KiB filter
    const std::uint64_t key = 0xDEADBEEFCAFEBABEULL;
    assert(!bloom.maybe_contains(key));
    bloom.insert(key);
    assert(bloom.maybe_contains(key));
    // Distinct key should usually not collide on all 4 hash slots.
    assert(!bloom.maybe_contains(key + 1));
    std::cout << "  PASS" << std::endl;
}

void test_bloom_no_false_negatives() {
    std::cout << "Testing Bloom: no false negatives across 10k inserts..."
              << std::endl;
    BloomLPKeyFilter bloom(18);  // 256 KiB filter — ~2.7% FP @ 10k
    std::mt19937_64 rng(0xC0FFEEC0FFEEULL);
    std::vector<std::uint64_t> keys;
    keys.reserve(10000);
    for (std::size_t i = 0; i < 10000; ++i) {
        keys.push_back(rng());
    }
    for (std::uint64_t k : keys) {
        bloom.insert(k);
    }
    // Every inserted key MUST report present. A single false negative
    // would prove the filter incorrect.
    for (std::uint64_t k : keys) {
        assert(bloom.maybe_contains(k));
    }
    std::cout << "  PASS" << std::endl;
}

void test_bloom_false_positive_rate_under_5pct() {
    std::cout << "Testing Bloom: FP rate < 5% for bits=20, n=10000..."
              << std::endl;
    BloomLPKeyFilter bloom(20);  // 1 MiB filter
    std::mt19937_64 rng(0xABCDEF0123456789ULL);

    // Insert 10000 distinct keys.
    std::unordered_set<std::uint64_t> inserted;
    inserted.reserve(10000);
    while (inserted.size() < 10000) {
        inserted.insert(rng());
    }
    for (std::uint64_t k : inserted) {
        bloom.insert(k);
    }

    // Query 10000 different keys that were never inserted; count how
    // many incorrectly report "maybe contains".
    std::size_t fp = 0;
    std::size_t queries = 0;
    while (queries < 10000) {
        const std::uint64_t q = rng();
        if (inserted.count(q) > 0) {
            continue;  // skip — would not be a FP, the key IS present.
        }
        ++queries;
        if (bloom.maybe_contains(q)) {
            ++fp;
        }
    }
    const double rate = static_cast<double>(fp) / static_cast<double>(queries);
    const double theory = bloom.estimated_fp_rate(inserted.size());
    std::cout << "    inserted=" << inserted.size() << " queries=" << queries
              << " fp=" << fp << " rate=" << rate
              << " (theoretical=" << theory << ")" << std::endl;
    assert(rate < 0.05);
    std::cout << "  PASS" << std::endl;
}

// ---------- count_unique_with_bloom parity ----------

void test_count_unique_parity_random_100k() {
    std::cout << "Testing count_unique_with_bloom parity (100k keys)..."
              << std::endl;
    std::mt19937_64 rng(0xBEEF1234ULL);
    std::vector<std::uint64_t> keys;
    keys.reserve(100000);
    // Mix of unique and duplicate keys — ~5% duplication rate to make
    // the unique-count exercise meaningful.
    for (std::size_t i = 0; i < 100000; ++i) {
        keys.push_back(rng() & 0xFFFFFFFFFFFFFULL);  // 52-bit range
    }
    // Synthesise some duplicates so the count is genuinely "unique".
    for (std::size_t i = 0; i < 5000; ++i) {
        keys.push_back(keys[i]);
    }

    // Baseline: bloom_bits == 0.
    const std::size_t baseline =
        count_unique_with_bloom(keys.begin(), keys.end(), 0);

    // Sweep across several bit widths.
    for (int bits : {10, 14, 18, 22}) {
        const std::size_t with_bloom =
            count_unique_with_bloom(keys.begin(), keys.end(), bits);
        if (with_bloom != baseline) {
            std::cerr << "[FAIL] bits=" << bits
                      << " baseline=" << baseline
                      << " with_bloom=" << with_bloom << std::endl;
        }
        assert(with_bloom == baseline);
    }
    std::cout << "    unique count=" << baseline << std::endl;
    std::cout << "  PASS" << std::endl;
}

// ---------- Edge cases ----------

void test_edge_empty_input() {
    std::cout << "Testing edge: empty input span..." << std::endl;
    std::vector<std::uint64_t> empty;
    assert(count_unique_with_bloom(empty.begin(), empty.end(), 0) == 0);
    assert(count_unique_with_bloom(empty.begin(), empty.end(), 16) == 0);
    assert(count_unique_with_bloom(empty.begin(), empty.end(), 22) == 0);
    std::cout << "  PASS" << std::endl;
}

void test_edge_single_key_and_all_same() {
    std::cout << "Testing edge: single key + all-duplicate span..." << std::endl;

    std::vector<std::uint64_t> single = {0x12345678};
    assert(count_unique_with_bloom(single.begin(), single.end(), 0) == 1);
    assert(count_unique_with_bloom(single.begin(), single.end(), 16) == 1);

    // 1000 copies of the same key → unique count is 1, regardless of path.
    std::vector<std::uint64_t> all_same(1000, 0xDEADBEEFULL);
    assert(count_unique_with_bloom(all_same.begin(), all_same.end(), 0) == 1);
    assert(count_unique_with_bloom(all_same.begin(), all_same.end(), 14) == 1);
    assert(count_unique_with_bloom(all_same.begin(), all_same.end(), 22) == 1);
    std::cout << "  PASS" << std::endl;
}

void test_ctor_rejects_below_floor_and_above_ceiling() {
    std::cout << "Testing ctor rejects bits < 10 and bits > 30..." << std::endl;
    bool threw = false;
    try {
        BloomLPKeyFilter bad(9);
        (void)bad;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    threw = false;
    try {
        BloomLPKeyFilter bad(0);
        (void)bad;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    threw = false;
    try {
        BloomLPKeyFilter bad(31);
        (void)bad;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    // Floor and ceiling values must construct successfully.
    BloomLPKeyFilter floor(10);
    assert(floor.bits() == 10);
    assert(floor.size_bytes() == 128);  // 2^10 / 8 = 128 bytes

    BloomLPKeyFilter ceiling(30);
    assert(ceiling.bits() == 30);
    assert(ceiling.size_bytes() == (1u << 30) / 8u);
    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== LP Bloom Pre-Screen Tests ===" << std::endl;

    test_env_unset_is_disabled();
    test_env_in_range_value();
    test_env_invalid_strings_are_disabled();
    test_env_clamping();

    test_bloom_basic_insert_and_contains();
    test_bloom_no_false_negatives();
    test_bloom_false_positive_rate_under_5pct();

    test_count_unique_parity_random_100k();

    test_edge_empty_input();
    test_edge_single_key_and_all_same();
    test_ctor_rejects_below_floor_and_above_ceiling();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
