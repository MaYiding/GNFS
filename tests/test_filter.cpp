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
    LargePrimeKey k101r{101, 0, false};
    LargePrimeKey k103r{103, 0, false};
    LargePrimeKey k101a{101, 0, true};
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

void test_merger_merge() {
    std::cout << "Testing PartialRelationMerger::merge..." << std::endl;

    std::vector<Relation> partials;
    // Two 1LP relations sharing rational LP=101
    {
        Relation r1(5, 1);
        r1.rational_factors = {0, 1, 3};
        r1.algebraic_factors = {0, 2};
        r1.rational_large_prime.push_back(PrimePower{101, 0, 1});
        partials.push_back(std::move(r1));
    }
    {
        Relation r2(7, 2);
        r2.rational_factors = {1, 2};
        r2.algebraic_factors = {1};
        r2.rational_large_prime.push_back(PrimePower{101, 0, 1});
        partials.push_back(std::move(r2));
    }
    // A third 1LP with a different LP (singleton, can't merge)
    partials.push_back(make_1lp_relation(9, 3, 103));

    auto merged = PartialRelationMerger::merge(partials);

    // Should produce 1 merged relation (from the pair sharing LP=101)
    assert(merged.size() == 1);

    const auto& m = merged[0];
    // Primary (a,b) from first relation
    assert(m.a == 5 && m.b == 1);
    // Extra (a,b) from second relation
    assert(m.is_merged());
    assert(m.extra_ab_pairs.size() == 1);
    assert(m.extra_ab_pairs[0].first == 7);
    assert(m.extra_ab_pairs[0].second == 2);

    // Factors: concatenation of both
    assert(m.rational_factors.size() == 5);   // {0,1,3} + {1,2}
    assert(m.algebraic_factors.size() == 3);  // {0,2} + {1}

    // LP: shared LP=101 appears twice (preserved for rational_sqrt exponent computation)
    assert(m.rational_large_prime.size() == 2);
    assert(m.rational_large_prime[0].p == 101);
    assert(m.rational_large_prime[1].p == 101);
    // But effectively full (all LPs have even count)
    assert(PartialRelationMerger::is_effectively_full(m));

    std::cout << "  PASS" << std::endl;
}

void test_merger_algebraic_lp() {
    std::cout << "Testing merge with algebraic large primes..." << std::endl;

    std::vector<Relation> partials;
    // Two 1LP relations sharing algebraic LP=(107, root=3) — same prime ideal
    {
        Relation r1(11, 1);
        r1.rational_factors = {0};
        r1.algebraic_factors = {0, 1};
        r1.algebraic_large_prime.push_back(PrimePower{107, 3, 1});
        partials.push_back(std::move(r1));
    }
    {
        Relation r2(13, 2);
        r2.rational_factors = {1};
        r2.algebraic_factors = {2};
        r2.algebraic_large_prime.push_back(PrimePower{107, 3, 1});  // same root=3
        partials.push_back(std::move(r2));
    }

    auto merged = PartialRelationMerger::merge(partials);
    assert(merged.size() == 1);

    const auto& m = merged[0];
    assert(m.is_merged());
    // Shared algebraic LP=(107,3) appears twice (preserved for sqrt computation)
    assert(m.algebraic_large_prime.size() == 2);
    assert(m.algebraic_large_prime[0].p == 107);
    assert(m.algebraic_large_prime[1].p == 107);
    assert(PartialRelationMerger::is_effectively_full(m));

    // Different roots should NOT merge — they're different prime ideals
    {
        std::vector<Relation> partials2;
        Relation r3(15, 1);
        r3.algebraic_large_prime.push_back(PrimePower{109, 2, 1});
        partials2.push_back(std::move(r3));

        Relation r4(17, 2);
        r4.algebraic_large_prime.push_back(PrimePower{109, 5, 1});  // different root
        partials2.push_back(std::move(r4));

        auto merged2 = PartialRelationMerger::merge(partials2);
        assert(merged2.empty());  // no merge: different prime ideals
    }

    std::cout << "  PASS" << std::endl;
}

void test_merger_no_2lp() {
    std::cout << "Testing merge skips 2LP relations..." << std::endl;

    std::vector<Relation> partials;
    // A 2LP relation with LP=101 and LP=103
    {
        Relation r1(5, 1);
        r1.rational_large_prime.push_back(PrimePower{101, 0, 1});
        r1.rational_large_prime.push_back(PrimePower{103, 0, 1});
        partials.push_back(std::move(r1));
    }
    // A 1LP with LP=101 — should NOT merge with the 2LP
    partials.push_back(make_1lp_relation(7, 1, 101));

    auto merged = PartialRelationMerger::merge(partials);
    // No merge: rel1 is 2LP, not 1LP
    assert(merged.empty());

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

void test_merge_all_2lp() {
    std::cout << "Testing merge_all with 2LP relations..." << std::endl;

    // Create a triangle: 3 relations sharing LPs pairwise
    // R1: LP_A=101, LP_B=103  (2LP)
    // R2: LP_B=103, LP_C=107  (2LP)
    // R3: LP_A=101, LP_C=107  (2LP)
    // After merging R1+R2 → cancel LP_B=103, remaining {LP_A=101, LP_C=107}
    // Then merge result with R3 → cancel both LP_A and LP_C → full relation!

    std::vector<Relation> partials;
    {
        Relation r1(5, 1);
        r1.rational_factors = {0};
        r1.algebraic_factors = {0};
        r1.rational_large_prime.push_back(PrimePower{101, 0, 1});
        r1.rational_large_prime.push_back(PrimePower{103, 0, 1});
        partials.push_back(std::move(r1));
    }
    {
        Relation r2(7, 2);
        r2.rational_factors = {1};
        r2.algebraic_factors = {1};
        r2.rational_large_prime.push_back(PrimePower{103, 0, 1});
        r2.rational_large_prime.push_back(PrimePower{107, 0, 1});
        partials.push_back(std::move(r2));
    }
    {
        Relation r3(11, 3);
        r3.rational_factors = {2};
        r3.algebraic_factors = {2};
        r3.rational_large_prime.push_back(PrimePower{101, 0, 1});
        r3.rational_large_prime.push_back(PrimePower{107, 0, 1});
        partials.push_back(std::move(r3));
    }

    PartialRelationMerger::MergeStats stats;
    auto merged = PartialRelationMerger::merge_all(std::move(partials), 10, &stats);

    // Triangle of 3 2LP relations → 1 effectively-full merged relation (in 2 rounds)
    assert(merged.size() == 1);
    assert(PartialRelationMerger::is_effectively_full(merged[0]));
    assert(merged[0].is_merged());
    // Should have 3 ab pairs total (primary + 2 extra)
    assert(merged[0].extra_ab_pairs.size() == 2);
    assert(stats.input_2lp == 3);
    assert(stats.rounds >= 2);

    std::cout << "  PASS" << std::endl;
}

void test_merge_all_mixed() {
    std::cout << "Testing merge_all with mixed 1LP+2LP..." << std::endl;

    std::vector<Relation> partials;

    // 1LP pair sharing LP=101
    partials.push_back(make_1lp_relation(1, 1, 101));
    partials.push_back(make_1lp_relation(2, 1, 101));

    // 2LP relation with LP=103, LP=107
    {
        Relation r(5, 1);
        r.rational_large_prime.push_back(PrimePower{103, 0, 1});
        r.rational_large_prime.push_back(PrimePower{107, 0, 1});
        partials.push_back(std::move(r));
    }
    // Singleton 1LP with LP=109 (should be removed)
    partials.push_back(make_1lp_relation(9, 1, 109));

    PartialRelationMerger::MergeStats stats;
    auto merged = PartialRelationMerger::merge_all(std::move(partials), 10, &stats);

    // 1LP pair → 1 effectively-full relation
    // Singleton LP=109 removed
    // 2LP LP={103,107} has singletons → removed
    assert(merged.size() == 1);
    assert(PartialRelationMerger::is_effectively_full(merged[0]));
    assert(stats.input_1lp == 3);
    assert(stats.input_2lp == 1);

    std::cout << "  PASS" << std::endl;
}

void test_merge_all_chain() {
    std::cout << "Testing merge_all with 2LP chain..." << std::endl;

    // Chain: R1(A,B) — R2(B,C) — R3(C,D) + R4(A) + R5(D)
    // After processing:
    //   Weight-2 LPs: B (in R1,R2), C (in R2,R3)
    //   R1+R2 → remaining {A,D's neighbor...no wait}
    // Let me construct properly:
    // R1: LP_A=101, LP_B=103  (2LP)
    // R2: LP_B=103, LP_C=107  (2LP)
    // R3: LP_C=107  (1LP)
    // After round 1: merge R1+R2 → {LP_A=101} (1LP, since LP_B canceled and LP_C remained)
    //   Wait no: R2 has LP_B=103 and LP_C=107. R1 has LP_A=101 and LP_B=103.
    //   LP_B=103 appears in R1 and R2 → weight-2 → merge → cancel LP_B
    //   Result: {LP_A=101, LP_C=107} (2LP)
    //   LP_C=107 appears in result and R3 → weight-2 in round 2
    //   merge result + R3 → cancel LP_C → {LP_A=101} (1LP)
    //   LP_A=101 now singleton → removed in round 3
    //   So no full relation from this chain

    // Better: make it a cycle. Add R4(LP_A=101) as 1LP
    std::vector<Relation> partials;
    {
        Relation r1(1, 1);
        r1.rational_factors = {0};
        r1.rational_large_prime.push_back(PrimePower{101, 0, 1});
        r1.rational_large_prime.push_back(PrimePower{103, 0, 1});
        partials.push_back(std::move(r1));
    }
    {
        Relation r2(2, 1);
        r2.rational_factors = {1};
        r2.rational_large_prime.push_back(PrimePower{103, 0, 1});
        r2.rational_large_prime.push_back(PrimePower{107, 0, 1});
        partials.push_back(std::move(r2));
    }
    {
        // 1LP with LP_C=107
        Relation r3(3, 1);
        r3.rational_factors = {2};
        r3.rational_large_prime.push_back(PrimePower{107, 0, 1});
        partials.push_back(std::move(r3));
    }
    {
        // 1LP with LP_A=101 — closes the chain
        Relation r4(4, 1);
        r4.rational_factors = {3};
        r4.rational_large_prime.push_back(PrimePower{101, 0, 1});
        partials.push_back(std::move(r4));
    }

    PartialRelationMerger::MergeStats stats;
    auto merged = PartialRelationMerger::merge_all(std::move(partials), 10, &stats);

    // Should produce 1 effectively-full relation through multi-round merging
    assert(merged.size() == 1);
    assert(PartialRelationMerger::is_effectively_full(merged[0]));
    assert(merged[0].extra_ab_pairs.size() == 3);  // 4 relations merged

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
    test_merger_merge();
    test_merger_algebraic_lp();
    test_merger_no_2lp();
    test_merge_all_2lp();
    test_merge_all_mixed();
    test_merge_all_chain();
    test_reset_stats();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
