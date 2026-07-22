// Unit tests for RelationFilter — singleton removal and filtering
#include "gnfs/relation/filter.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

using namespace gnfs::relation;
using gnfs::core::Relation;
using gnfs::core::PrimePower;

void require_test(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "  FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// Helper: create a full relation (no large primes)
Relation make_full_relation(int64_t a, int64_t b) {
    Relation r(a, static_cast<uint64_t>(b));
    r.rational_factors = {0, 1, 2};
    r.algebraic_factors = {0, 1};
    return r;
}

// Helper: create a relation with one rational large prime
Relation make_1lp_relation(int64_t a, int64_t b, uint64_t lp) {
    Relation r(a, static_cast<uint64_t>(b));
    r.rational_factors = {0, 1};
    r.algebraic_factors = {0};
    r.rational_large_prime.push_back(PrimePower{lp, 0, 1});
    return r;
}

// Helper: create a relation with one algebraic large prime
Relation make_1alp_relation(int64_t a, int64_t b, uint64_t lp) {
    Relation r(a, static_cast<uint64_t>(b));
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
        rels.push_back(make_1lp_relation(i, 1, static_cast<uint64_t>(100 + i)));  // all singletons
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

void test_reuse_max_passes_is_per_call() {
    std::cout << "Testing max_passes on reused filter..." << std::endl;

    FilterConfig cfg;
    cfg.remove_singletons = true;
    cfg.max_passes = 1;
    RelationFilter filter(cfg);

    std::vector<Relation> first_input;
    first_input.push_back(make_1lp_relation(1, 1, 101));
    auto first_result = filter.filter(std::move(first_input));
    require_test(first_result.empty(), "first call must remove its singleton");
    require_test(filter.stats() ==
                     FilterStats{
                         .input_relations = 1,
                         .output_relations = 0,
                         .singletons_removed = 1,
                         .duplicates_removed = 0,
                         .passes = 1,
                     },
                 "first call must report one-pass local statistics");

    std::vector<Relation> second_input;
    second_input.push_back(make_1lp_relation(2, 1, 103));
    auto second_result = filter.filter(std::move(second_input));
    require_test(second_result.empty(),
                 "second call must receive its own max_passes budget");
    require_test(filter.stats() ==
                     FilterStats{
                         .input_relations = 1,
                         .output_relations = 0,
                         .singletons_removed = 1,
                         .duplicates_removed = 0,
                         .passes = 1,
                     },
                 "second call statistics must replace the first call statistics");

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

void test_effective_column_excess_boundaries() {
    std::cout << "Testing effective-column excess boundaries..." << std::endl;

    require_test(!has_effective_column_excess(0, 0, 0),
                 "zero rows must not exceed zero columns");
    require_test(!has_effective_column_excess(10, 6, 4),
                 "rows equal to effective columns are not excess");
    require_test(has_effective_column_excess(11, 6, 4),
                 "one row beyond effective columns must be excess");

    constexpr size_t MAX_SIZE = std::numeric_limits<size_t>::max();
    require_test(effective_column_count(6, 4) == 10,
                 "effective columns must include the LP columns");
    require_test(effective_column_count(MAX_SIZE - 1, 2) == MAX_SIZE,
                 "overflowing effective columns must saturate");
    require_test(has_effective_column_excess(MAX_SIZE, MAX_SIZE - 2, 1),
                 "representable near-limit sum must preserve strict excess");
    require_test(!has_effective_column_excess(MAX_SIZE, MAX_SIZE - 1, 1),
                 "rows equal to a near-limit sum are not excess");
    require_test(!has_effective_column_excess(MAX_SIZE, MAX_SIZE - 1, 2),
                 "overflowing effective-column sum cannot have representable excess rows");
    require_test(!has_effective_column_excess(MAX_SIZE, MAX_SIZE, 1),
                 "maximal base columns plus LP columns must fail closed");

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

void test_count_unique_lp_keys() {
    std::cout << "Testing count_unique_lp_keys..." << std::endl;
    // Empty
    {
        std::vector<Relation> empty;
        assert(count_unique_lp_keys(empty) == 0);
    }
    // 1 rel with 1 rat LP
    {
        std::vector<Relation> rels;
        rels.push_back(make_1lp_relation(1, 1, 12345));
        assert(count_unique_lp_keys(rels) == 1);  // 1 rat LP
    }
    // 2 rels share same rat LP (even exponent → cancels)
    {
        std::vector<Relation> rels;
        rels.push_back(make_1lp_relation(1, 1, 99));
        rels.push_back(make_1lp_relation(2, 1, 99));  // same LP
        // 2 rels independently have lp=99 with e=1 each. NOT same relation.
        // count_unique_lp_keys 在 per-rel 内累加 (per matrix_builder convention),
        // 所以 rel1 has e=1 (奇), rel2 has e=1 (奇), 两个 unique key = same key = 1.
        assert(count_unique_lp_keys(rels) == 1);
    }
    // 1 rel with even exponent LP (cancels)
    {
        Relation r(1, 1);
        r.rational_factors = {0};
        r.algebraic_factors = {0};
        r.rational_large_prime.push_back(PrimePower{99, 0, 2});  // e=2 even
        std::vector<Relation> rels;
        rels.push_back(std::move(r));
        assert(count_unique_lp_keys(rels) == 0);  // even-exp → not counted
    }
    // alg LP differs by (p, r) — same p, different r → 2 columns
    {
        Relation r1(1, 1), r2(2, 1);
        r1.rational_factors = {0}; r1.algebraic_factors = {0};
        r2.rational_factors = {0}; r2.algebraic_factors = {0};
        r1.algebraic_large_prime.push_back(PrimePower{77, 3, 1});  // (p=77, r=3)
        r2.algebraic_large_prime.push_back(PrimePower{77, 5, 1});  // (p=77, r=5)
        std::vector<Relation> rels;
        rels.push_back(std::move(r1));
        rels.push_back(std::move(r2));
        assert(count_unique_lp_keys(rels) == 2);  // 2 distinct ideals
    }
    // Same alg ideal twice in 1 rel (e=2 even → cancels)
    {
        Relation r(1, 1);
        r.rational_factors = {0}; r.algebraic_factors = {0};
        r.algebraic_large_prime.push_back(PrimePower{77, 3, 1});
        r.algebraic_large_prime.push_back(PrimePower{77, 3, 1});  // duplicate
        std::vector<Relation> rels;
        rels.push_back(std::move(r));
        assert(count_unique_lp_keys(rels) == 0);  // even-exp cancels
    }
    // Regression test for V2 bug: 5% guess vs ~64% actual
    // Simulates 50d distribution: many rels each with multiple distinct LP keys.
    // Verifies count_unique_lp_keys returns accurate count (not 5% guess).
    {
        std::vector<Relation> rels;
        for (uint64_t i = 1; i <= 100; ++i) {
            Relation r(static_cast<int64_t>(i), 1);
            r.rational_factors = {0};
            r.algebraic_factors = {0};
            // Each rel has 2 unique alg LP ideals, all different across rels
            r.algebraic_large_prime.push_back(PrimePower{1000 + i, 1, 1});
            r.algebraic_large_prime.push_back(PrimePower{2000 + i, 2, 1});
            rels.push_back(std::move(r));
        }
        // 100 rels × 2 unique LP each = 200 distinct (p,r) keys
        assert(count_unique_lp_keys(rels) == 200);
        // 5% guess would say 5; bug-induced under-estimate of 40×.
        assert(rels.size() / 20 == 5);  // confirm old buggy estimate
    }
    std::cout << "  PASS" << std::endl;
}

void test_count_lp_key_weights() {
    std::cout << "Testing count_lp_key_weights..." << std::endl;

    // Empty input → all zero
    {
        std::vector<Relation> empty;
        auto h = count_lp_key_weights(empty);
        assert(h.unique_keys == 0);
        assert(h.weight_1 == 0);
        assert(h.weight_2 == 0);
        assert(h.weight_3 == 0);
        assert(h.weight_4plus == 0);
    }

    // Single rel with 1 rat LP → 1 key, weight=1
    {
        std::vector<Relation> rels;
        rels.push_back(make_1lp_relation(1, 1, 100));
        auto h = count_lp_key_weights(rels);
        assert(h.unique_keys == 1);
        assert(h.weight_1 == 1);
        assert(h.weight_2 == 0);
    }

    // 2 rels share same rat LP → 1 key, weight=2 (V0 sweet spot)
    {
        std::vector<Relation> rels;
        rels.push_back(make_1lp_relation(1, 1, 100));
        rels.push_back(make_1lp_relation(2, 1, 100));
        auto h = count_lp_key_weights(rels);
        assert(h.unique_keys == 1);
        assert(h.weight_1 == 0);
        assert(h.weight_2 == 1);
    }

    // 3 rels share LP → weight=3 (chain territory)
    {
        std::vector<Relation> rels;
        for (int64_t a = 1; a <= 3; ++a) {
            rels.push_back(make_1lp_relation(a, 1, 100));
        }
        auto h = count_lp_key_weights(rels);
        assert(h.unique_keys == 1);
        assert(h.weight_3 == 1);
    }

    // 5 rels share LP → weight≥4 bucket
    {
        std::vector<Relation> rels;
        for (int64_t a = 1; a <= 5; ++a) {
            rels.push_back(make_1lp_relation(a, 1, 100));
        }
        auto h = count_lp_key_weights(rels);
        assert(h.unique_keys == 1);
        assert(h.weight_4plus == 1);
    }

    // Mixed: rat LP and alg LP are SEPARATE key spaces
    // 1 rel with rat lp=100, 1 rel with alg lp=100 → 2 distinct keys both weight=1
    {
        std::vector<Relation> rels;
        rels.push_back(make_1lp_relation(1, 1, 100));   // rat lp=100
        rels.push_back(make_1alp_relation(2, 1, 100));  // alg (p=100, r=0)
        auto h = count_lp_key_weights(rels);
        assert(h.unique_keys == 2);
        assert(h.weight_1 == 2);  // both singleton
    }

    // Algebraic LP distinguishes by (p, r) — same p different r → 2 keys
    {
        Relation r1(1, 1), r2(2, 1);
        r1.rational_factors = {0}; r1.algebraic_factors = {0};
        r2.rational_factors = {0}; r2.algebraic_factors = {0};
        r1.algebraic_large_prime.push_back(PrimePower{77, 3, 1});  // (77, 3)
        r2.algebraic_large_prime.push_back(PrimePower{77, 5, 1});  // (77, 5)
        std::vector<Relation> rels;
        rels.push_back(std::move(r1));
        rels.push_back(std::move(r2));
        auto h = count_lp_key_weights(rels);
        assert(h.unique_keys == 2);
        assert(h.weight_1 == 2);
    }

    // Within one rel, repeated LP key still counts as 1 occurrence for the
    // histogram (de-dup within relation). So 1 rel with 3 copies of same LP
    // still yields weight=1 (not weight=3 for the same key in the same rel).
    {
        Relation r(1, 1);
        r.rational_factors = {0}; r.algebraic_factors = {0};
        r.rational_large_prime.push_back(PrimePower{99, 0, 1});
        r.rational_large_prime.push_back(PrimePower{99, 0, 1});
        r.rational_large_prime.push_back(PrimePower{99, 0, 1});
        std::vector<Relation> rels;
        rels.push_back(std::move(r));
        auto h = count_lp_key_weights(rels);
        assert(h.unique_keys == 1);
        assert(h.weight_1 == 1);  // de-dup within rel
    }

    // Regression: per-relation LP count > 8 must NOT inflate weight via
    // duplicate-LP-in-same-rel. V3 chain-merged partials commonly carry
    // many residual LPs from accumulated chain. The 8-slot stack de-dup
    // overflow path must hand off to unordered_set (not silently miss).
    {
        Relation r(1, 1);
        r.rational_factors = {0}; r.algebraic_factors = {0};
        // 12 distinct rational LPs, plus one of them (say p=1005) repeated
        // 4 more times. Without overflow handling, the duplicate occurrences
        // would inflate the count from 1 to 5 for p=1005 (BUG: 5 in a single
        // relation → weight=5 → weight_4plus, instead of weight=1).
        for (uint64_t p = 1000; p < 1012; ++p) {
            r.rational_large_prime.push_back(PrimePower{p, 0, 1});
        }
        // Repeat p=1005 four more times (now 13+4 = 17 LP entries, still
        // only 12 distinct keys).
        for (int i = 0; i < 4; ++i) {
            r.rational_large_prime.push_back(PrimePower{1005, 0, 1});
        }
        std::vector<Relation> rels;
        rels.push_back(std::move(r));
        auto h = count_lp_key_weights(rels);
        // 12 unique keys, ALL weight=1 (single-rel singletons).
        assert(h.unique_keys == 12);
        assert(h.weight_1 == 12);
        assert(h.weight_2 == 0);
        assert(h.weight_3 == 0);
        assert(h.weight_4plus == 0);
    }

    // 50d-like scenario: many distinct LP keys, most weight=1, some weight=2
    {
        std::vector<Relation> rels;
        // 100 rels each with a unique rat LP → 100 singletons (weight=1)
        for (uint64_t i = 1; i <= 100; ++i) {
            rels.push_back(make_1lp_relation(static_cast<int64_t>(i), 1, 1000 + i));
        }
        // 50 pairs sharing alg LP → 50 weight=2 keys
        for (uint64_t i = 1; i <= 50; ++i) {
            Relation a(static_cast<int64_t>(1000 + i), 1);
            a.rational_factors = {0}; a.algebraic_factors = {0};
            a.algebraic_large_prime.push_back(PrimePower{i + 10000, 0, 1});
            Relation b(static_cast<int64_t>(2000 + i), 1);
            b.rational_factors = {0}; b.algebraic_factors = {0};
            b.algebraic_large_prime.push_back(PrimePower{i + 10000, 0, 1});
            rels.push_back(std::move(a));
            rels.push_back(std::move(b));
        }
        auto h = count_lp_key_weights(rels);
        assert(h.unique_keys == 150);
        assert(h.weight_1 == 100);
        assert(h.weight_2 == 50);
        assert(h.weight_3 == 0);
        assert(h.weight_4plus == 0);
    }

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
    test_reuse_max_passes_is_per_call();
    test_count_large_primes();
    test_get_unique_large_primes();
    test_separate_relations();
    test_required_relations();
    test_effective_column_excess_boundaries();
    test_merger_count();
    test_merger_merge();
    test_merger_algebraic_lp();
    test_merger_no_2lp();
    test_merge_all_2lp();
    test_merge_all_mixed();
    test_merge_all_chain();
    test_reset_stats();
    test_count_unique_lp_keys();
    test_count_lp_key_weights();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
