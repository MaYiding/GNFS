// Unit tests for Phase 0 dedup-sort radix-sort path
// (include/gnfs/relation/radix_sort.hpp + collector.hpp dispatch).
//
// Verifies bit-for-bit equivalence between the legacy std::sort path
// (GNFS_FILTER_RADIX_SORT=0 / unset) and the radix-sort path
// (GNFS_FILTER_RADIX_SORT=1). After dedup the relation set must be
// IDENTICAL — same relations in same order — between the two paths.
// Stability of the radix sort is required so that duplicate (a, b)
// keys preserve insertion order and filter_duplicates picks the same
// representative either way.

#include "gnfs/relation/collector.hpp"
#include "gnfs/relation/radix_sort.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

using namespace gnfs::relation;
using gnfs::core::PrimePower;
using gnfs::core::Relation;

namespace {

// Helper: small marker payload so we can tell "which copy" of a
// duplicate (a, b) pair was kept. We tag the rational_factors[0] with
// a unique counter for each relation pushed; if dedup is stable we
// should see the first-inserted relation's tag survive.
Relation make_relation(int64_t a, uint64_t b, uint32_t tag) {
    Relation r(a, b);
    r.rational_factors = {tag};
    r.algebraic_factors = {tag};
    return r;
}

// Force re-resolve the ENV cache so back-to-back tests with different
// ENV settings observe the right path.
void set_env_and_reload(const char* value) {
    if (value == nullptr) {
        ::unsetenv("GNFS_FILTER_RADIX_SORT");
    } else {
        ::setenv("GNFS_FILTER_RADIX_SORT", value, 1);
    }
    filter_radix_sort_reset_env_cache_for_testing();
}

// Deep equality for the fields the radix sort permutes around. We only
// compare (a, b) and the marker tag stored in rational_factors[0] /
// algebraic_factors[0]: full Relation equality is overkill since the
// radix sort never touches the inner fields.
bool relations_equal(const Relation& l, const Relation& r) {
    if (l.a != r.a || l.b != r.b) return false;
    if (l.rational_factors.size() != r.rational_factors.size()) return false;
    if (l.algebraic_factors.size() != r.algebraic_factors.size()) return false;
    for (size_t i = 0; i < l.rational_factors.size(); ++i) {
        if (l.rational_factors[i] != r.rational_factors[i]) return false;
    }
    for (size_t i = 0; i < l.algebraic_factors.size(); ++i) {
        if (l.algebraic_factors[i] != r.algebraic_factors[i]) return false;
    }
    return true;
}

bool vectors_equal(const std::vector<Relation>& lhs,
                   const std::vector<Relation>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!relations_equal(lhs[i], rhs[i])) return false;
    }
    return true;
}

// Run `seed_relations` once with std::sort path and once with radix path,
// then assert that the sorted-then-deduplicated results match.
void run_parity(const std::vector<Relation>& seed_relations,
                const char* label) {
    auto clone = [&]() {
        std::vector<Relation> out;
        out.reserve(seed_relations.size());
        for (const auto& r : seed_relations) {
            Relation copy = r;  // PrimePower / vector deep copy
            out.push_back(std::move(copy));
        }
        return out;
    };

    set_env_and_reload(nullptr);
    assert(!filter_radix_sort_enabled());
    auto std_sorted = clone();
    sort_relations(std_sorted);
    auto std_dedup = filter_duplicates(std::move(std_sorted));

    set_env_and_reload("1");
    assert(filter_radix_sort_enabled());
    auto radix_sorted = clone();
    sort_relations(radix_sorted);
    auto radix_dedup = filter_duplicates(std::move(radix_sorted));

    // Reset ENV so subsequent tests start from a clean default.
    set_env_and_reload(nullptr);

    if (!vectors_equal(std_dedup, radix_dedup)) {
        std::cerr << "[FAIL] " << label
                  << " — std vs radix dedup result differs.\n"
                  << "  std size=" << std_dedup.size()
                  << ", radix size=" << radix_dedup.size() << std::endl;
        for (size_t i = 0; i < std::min(std_dedup.size(), radix_dedup.size()); ++i) {
            std::cerr << "    [" << i << "] std=("
                      << std_dedup[i].a << "," << std_dedup[i].b << ")"
                      << " radix=(" << radix_dedup[i].a << "," << radix_dedup[i].b << ")\n";
        }
    }
    assert(vectors_equal(std_dedup, radix_dedup));
}

}  // namespace

void test_empty_input() {
    std::cout << "Testing empty input..." << std::endl;
    std::vector<Relation> rels;
    run_parity(rels, "empty");
    std::cout << "  PASS" << std::endl;
}

void test_single_relation() {
    std::cout << "Testing single relation..." << std::endl;
    std::vector<Relation> rels;
    rels.push_back(make_relation(42, 7, 0));
    run_parity(rels, "single");
    std::cout << "  PASS" << std::endl;
}

void test_already_sorted() {
    std::cout << "Testing already-sorted input (10 relations)..." << std::endl;
    std::vector<Relation> rels;
    // sort key is (b, a) lex — generate in (b ascending, a ascending) order.
    uint32_t tag = 0;
    for (uint64_t b = 1; b <= 5; ++b) {
        for (int64_t a = -2; a <= -1; ++a) {  // two per b
            rels.push_back(make_relation(a, b, tag++));
        }
    }
    assert(rels.size() == 10);
    run_parity(rels, "already_sorted");
    std::cout << "  PASS" << std::endl;
}

void test_reverse_sorted() {
    std::cout << "Testing reverse-sorted input (10 relations)..." << std::endl;
    std::vector<Relation> rels;
    uint32_t tag = 0;
    // Reverse: large b first, descending a within each.
    for (uint64_t b = 5; b >= 1; --b) {
        for (int64_t a = 10; a >= 9; --a) {
            rels.push_back(make_relation(a, b, tag++));
        }
    }
    assert(rels.size() == 10);
    run_parity(rels, "reverse_sorted");
    std::cout << "  PASS" << std::endl;
}

void test_random_100() {
    std::cout << "Testing random input (100 relations, fixed seed)..." << std::endl;
    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_int_distribution<int64_t> a_dist(-1000, 1000);
    std::uniform_int_distribution<uint64_t> b_dist(1, 200);

    std::vector<Relation> rels;
    rels.reserve(100);
    for (uint32_t i = 0; i < 100; ++i) {
        rels.push_back(make_relation(a_dist(rng), b_dist(rng), i));
    }
    run_parity(rels, "random_100");
    std::cout << "  PASS" << std::endl;
}

void test_random_10000() {
    std::cout << "Testing random input (10000 relations, fixed seed)..." << std::endl;
    std::mt19937_64 rng(0xDEADBEEFCAFEBABEULL);
    // Wider ranges to cross multiple byte boundaries in both keys.
    std::uniform_int_distribution<int64_t> a_dist(
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max());
    std::uniform_int_distribution<uint64_t> b_dist(1, 1ULL << 40);

    std::vector<Relation> rels;
    rels.reserve(10000);
    for (uint32_t i = 0; i < 10000; ++i) {
        rels.push_back(make_relation(a_dist(rng), b_dist(rng), i));
    }
    run_parity(rels, "random_10000");
    std::cout << "  PASS" << std::endl;
}

void test_duplicates_dedup_stable() {
    std::cout << "Testing duplicate (a, b) collapses to one (stable)..." << std::endl;
    std::vector<Relation> rels;
    // 5 copies of (1, 2) with distinct tags; insertion order tag 0..4.
    for (uint32_t i = 0; i < 5; ++i) {
        rels.push_back(make_relation(1, 2, i));
    }
    // Two unrelated relations to make sure the dedup leaves more than
    // just the duplicate cluster behind.
    rels.push_back(make_relation(-3, 4, 100));
    rels.push_back(make_relation(0, 1, 101));

    // Run parity on the seed list (asserts vectors_equal between paths).
    run_parity(rels, "duplicates");

    // Additionally pin down the exact dedup result for both paths:
    // filter_duplicates keeps the FIRST occurrence per (a, b) key in
    // the post-sort order. Because the sort is stable, that first copy
    // is the one with the lowest insertion tag.
    auto clone = [&]() {
        std::vector<Relation> out;
        for (const auto& r : rels) {
            Relation c = r;
            out.push_back(std::move(c));
        }
        return out;
    };

    set_env_and_reload("1");
    auto sorted_radix = clone();
    sort_relations(sorted_radix);
    auto dedup_radix = filter_duplicates(std::move(sorted_radix));
    set_env_and_reload(nullptr);

    // Expect 3 distinct (a, b) keys after dedup: (0,1), (1,2), (-3,4).
    assert(dedup_radix.size() == 3);
    // The (1, 2) cluster must survive with tag=0 (first insertion),
    // proving the radix sort is stable and dedup picks the same
    // representative as the std::sort path.
    bool found_tag0_for_1_2 = false;
    for (const auto& r : dedup_radix) {
        if (r.a == 1 && r.b == 2) {
            assert(!r.rational_factors.empty());
            assert(r.rational_factors[0] == 0);
            found_tag0_for_1_2 = true;
        }
    }
    assert(found_tag0_for_1_2);
    std::cout << "  PASS" << std::endl;
}

void test_env_parsing() {
    std::cout << "Testing ENV parsing matrix..." << std::endl;

    set_env_and_reload(nullptr);
    assert(!filter_radix_sort_enabled());

    set_env_and_reload("0");
    assert(!filter_radix_sort_enabled());

    set_env_and_reload("1");
    assert(filter_radix_sort_enabled());

    set_env_and_reload("garbage");
    assert(!filter_radix_sort_enabled());

    set_env_and_reload("2");
    assert(!filter_radix_sort_enabled());

    set_env_and_reload("true");
    assert(!filter_radix_sort_enabled());

    // Empty string is treated as "not 1" → OFF.
    set_env_and_reload("");
    assert(!filter_radix_sort_enabled());

    // Restore default for the rest of the suite.
    set_env_and_reload(nullptr);
    assert(!filter_radix_sort_enabled());

    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== Phase 0 Radix-Sort Parity Tests ===" << std::endl;

    test_empty_input();
    test_single_relation();
    test_already_sorted();
    test_reverse_sorted();
    test_random_100();
    test_random_10000();
    test_duplicates_dedup_stable();
    test_env_parsing();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
