// Unit tests for RelationFilter — singleton removal and filtering
#include "gnfs/relation/filter.hpp"

#include <cassert>
#include <iostream>

using namespace gnfs::relation;
using gnfs::core::Relation;
using gnfs::core::PrimePower;

// Helper: create a full relation (no large primes)
Relation make_full_relation(int64_t a, int64_t b) {
    Relation r(a, b);
    r.rational_factors = {0, 1, 2};
    r.algebraic_factors = {0, 1};
    return r;
}

// Helper: create a relation with one rational large prime
Relation make_1lp_relation(int64_t a, int64_t b, uint64_t lp) {
    Relation r(a, b);
    r.rational_factors = {0, 1};
    r.algebraic_factors = {0};
    r.rational_large_prime.push_back(PrimePower{lp, 0, 1});
    return r;
}

// Helper: create a relation with one algebraic large prime
Relation make_1alp_relation(int64_t a, int64_t b, uint64_t lp) {
    Relation r(a, b);
    r.rational_factors = {0, 1};
    r.algebraic_factors = {0};
    r.algebraic_large_prime.push_back(PrimePower{lp, 0, 1});
    return r;
}

void test_empty_input() {
    std::cout << "Testing empty input..." << std::endl;

    RelationFilter filter;
    std::vector<Relation> empty;
    auto result = filter.filter(std::move(empty));
    assert(result.empty());
    assert(filter.stats().input_relations == 0);
    assert(filter.stats().output_relations == 0);

    std::cout << "  PASS" << std::endl;
}

void test_no_large_primes() {
    std::cout << "Testing relations without large primes..." << std::endl;

    std::vector<Relation> rels;
    rels.push_back(make_full_relation(1, 1));
    rels.push_back(make_full_relation(2, 1));
    rels.push_back(make_full_relation(3, 1));

    RelationFilter filter;
    auto result = filter.filter(std::move(rels));

    // No large primes → no singletons → all preserved
    assert(result.size() == 3);
    assert(filter.stats().singletons_removed == 0);

    std::cout << "  PASS" << std::endl;
}

void test_singleton_removal() {
    std::cout << "Testing singleton removal..." << std::endl;

    std::vector<Relation> rels;
    // Two relations share LP=101
    rels.push_back(make_1lp_relation(1, 1, 101));
    rels.push_back(make_1lp_relation(2, 1, 101));
    // One relation with unique LP=103 (singleton → removed)
    rels.push_back(make_1lp_relation(3, 1, 103));
    // One full relation (no LP, always kept)
    rels.push_back(make_full_relation(4, 1));

    FilterConfig cfg;
    cfg.remove_singletons = true;
    cfg.max_passes = 10;
    RelationFilter filter(cfg);
    auto result = filter.filter(std::move(rels));

    // LP=103 is singleton → relation 3 removed
    assert(result.size() == 3);
    assert(filter.stats().singletons_removed >= 1);
    assert(filter.stats().input_relations == 4);
    assert(filter.stats().output_relations == 3);

    std::cout << "  PASS" << std::endl;
}

void test_multi_pass_filtering() {
    std::cout << "Testing multi-pass filtering..." << std::endl;

    std::vector<Relation> rels;
    // Rel 0: LP=101, LP=201 (algebraic)
    {
        Relation r(1, 1);
        r.rational_large_prime.push_back(PrimePower{101, 0, 1});
        r.algebraic_large_prime.push_back(PrimePower{201, 0, 1});
        rels.push_back(std::move(r));
    }
    // Rel 1: LP=101 (shares with rel 0)
    rels.push_back(make_1lp_relation(2, 1, 101));
    // Rel 2: LP=201 (algebraic, shares with rel 0)
    rels.push_back(make_1alp_relation(3, 1, 201));
    // Rel 3: LP=103 (singleton)
    rels.push_back(make_1lp_relation(4, 1, 103));

    FilterConfig cfg;
    cfg.remove_singletons = true;
    cfg.max_passes = 10;
    RelationFilter filter(cfg);
    auto result = filter.filter(std::move(rels));

    // Pass 1: LP=103 is singleton → rel 3 removed
    // Now LP=101 appears in rel 0 and rel 1 (2 times), LP=201 in rel 0 and rel 2 (2 times)
    // All surviving are non-singleton
    assert(result.size() == 3);

    std::cout << "  PASS (passes=" << filter.stats().passes << ")" << std::endl;
}

void test_cascading_singletons() {
    std::cout << "Testing cascading singleton removal..." << std::endl;

    std::vector<Relation> rels;
    // Chain: LP=101 shared by rel 0,1; LP=103 shared by rel 1,2
    // rel 0: LP=101
    rels.push_back(make_1lp_relation(1, 1, 101));
    // rel 1: LP=101 and LP=103
    {
        Relation r(2, 1);
        r.rational_large_prime.push_back(PrimePower{101, 0, 1});
        r.rational_large_prime.push_back(PrimePower{103, 0, 1});
        rels.push_back(std::move(r));
    }
    // rel 2: LP=103
    rels.push_back(make_1lp_relation(3, 1, 103));
    // rel 3: LP=107 (singleton → triggers cascade)
    rels.push_back(make_1lp_relation(4, 1, 107));

    FilterConfig cfg;
    cfg.remove_singletons = true;
    cfg.max_passes = 10;
    RelationFilter filter(cfg);
    auto result = filter.filter(std::move(rels));

    // Pass 1: LP=107 singleton → rel 3 removed
    //   After: LP=101 count=2, LP=103 count=2 → no more singletons
    // Result: 3 relations
    assert(result.size() == 3);
    assert(filter.stats().passes >= 1);

    std::cout << "  PASS" << std::endl;
}

void test_disable_singleton_removal() {
    std::cout << "Testing disabled singleton removal..." << std::endl;

    std::vector<Relation> rels;
    rels.push_back(make_1lp_relation(1, 1, 101));  // singleton
    rels.push_back(make_full_relation(2, 1));

    FilterConfig cfg;
    cfg.remove_singletons = false;
    RelationFilter filter(cfg);
    auto result = filter.filter(std::move(rels));

    assert(result.size() == 2);
    assert(filter.stats().singletons_removed == 0);

    std::cout << "  PASS" << std::endl;
}

void test_max_passes_limit() {
    std::cout << "Testing max_passes limit..." << std::endl;

    // Create a scenario where filtering could cascade many times
    std::vector<Relation> rels;
    for (int i = 0; i < 10; ++i) {
        rels.push_back(make_1lp_relation(i, 1, 100 + i));  // all singletons
    }

    FilterConfig cfg;
    cfg.remove_singletons = true;
    cfg.max_passes = 2;  // limit to 2 passes
    RelationFilter filter(cfg);
    auto result = filter.filter(std::move(rels));

    // All are singletons, should be removed in 1 pass
    assert(result.empty());
    assert(filter.stats().passes <= 2);

    std::cout << "  PASS" << std::endl;
}

void test_count_large_primes() {
    std::cout << "Testing count_large_primes..." << std::endl;

    std::vector<Relation> rels;
    rels.push_back(make_1lp_relation(1, 1, 101));
    rels.push_back(make_1lp_relation(2, 1, 101));
    rels.push_back(make_1lp_relation(3, 1, 103));
    rels.push_back(make_1alp_relation(4, 1, 101));  // algebraic, different from rational 101

    auto counts = RelationFilter::count_large_primes(rels);
    LargePrimeKey k101r{101, false};
    LargePrimeKey k103r{103, false};
    LargePrimeKey k101a{101, true};
    assert(counts[k101r] == 2);   // rational 101 appears twice
    assert(counts[k103r] == 1);   // rational 103 once
    assert(counts[k101a] == 1);   // algebraic 101 once

    std::cout << "  PASS" << std::endl;
}

void test_get_unique_large_primes() {
    std::cout << "Testing get_unique_large_primes..." << std::endl;

    std::vector<Relation> rels;
    rels.push_back(make_1lp_relation(1, 1, 101));
    rels.push_back(make_1lp_relation(2, 1, 101));
    rels.push_back(make_1lp_relation(3, 1, 103));

    auto unique = RelationFilter::get_unique_large_primes(rels);
    assert(unique.size() == 2);  // 101 and 103

    std::cout << "  PASS" << std::endl;
}

void test_separate_relations() {
    std::cout << "Testing separate_relations..." << std::endl;

    std::vector<Relation> rels;
    rels.push_back(make_full_relation(1, 1));
    rels.push_back(make_1lp_relation(2, 1, 101));
    rels.push_back(make_full_relation(3, 1));
    rels.push_back(make_1alp_relation(4, 1, 103));

    auto sep = separate_relations(std::move(rels));
    assert(sep.full.size() == 2);
    assert(sep.partial.size() == 2);

    std::cout << "  PASS" << std::endl;
}

void test_required_relations() {
    std::cout << "Testing required_relations..." << std::endl;

    // FB=1000, LP=500, excess=1.05 → 1575 + 1 = 1576
    size_t req = required_relations(1000, 500, 1.05);
    assert(req == 1576);

    // Zero large primes
    size_t req2 = required_relations(1000, 0, 1.05);
    assert(req2 == 1051);

    // has_enough_relations
    assert(has_enough_relations(1576, 1000, 500, 1.05));
    assert(!has_enough_relations(1575, 1000, 500, 1.05));

    std::cout << "  PASS" << std::endl;
}

void test_merger_count() {
    std::cout << "Testing PartialRelationMerger::count_mergeable_pairs..." << std::endl;

    std::vector<Relation> partials;
    partials.push_back(make_1lp_relation(1, 1, 101));
    partials.push_back(make_1lp_relation(2, 1, 101));
    partials.push_back(make_1lp_relation(3, 1, 101));

    size_t pairs = PartialRelationMerger::count_mergeable_pairs(partials);
    // 3 relations share LP=101 → C(3,2) = 3 pairs
    assert(pairs == 3);

    std::cout << "  PASS" << std::endl;
}

void test_reset_stats() {
    std::cout << "Testing reset_stats..." << std::endl;

    RelationFilter filter;
    std::vector<Relation> rels;
    rels.push_back(make_1lp_relation(1, 1, 101));
    auto _ = filter.filter(std::move(rels));
    (void)_;
    assert(filter.stats().input_relations > 0);

    filter.reset_stats();
    assert(filter.stats().input_relations == 0);
    assert(filter.stats().output_relations == 0);
    assert(filter.stats().singletons_removed == 0);
    assert(filter.stats().passes == 0);

    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== RelationFilter Unit Tests ===" << std::endl;

    test_empty_input();
    test_no_large_primes();
    test_singleton_removal();
    test_multi_pass_filtering();
    test_cascading_singletons();
    test_disable_singleton_removal();
    test_max_passes_limit();
    test_count_large_primes();
    test_get_unique_large_primes();
    test_separate_relations();
    test_required_relations();
    test_merger_count();
    test_reset_stats();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
