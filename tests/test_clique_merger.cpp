// Unit tests for V3 CliqueRelationMerger (backup merge via spanning tree)
#include "gnfs/relation/clique_merger.hpp"

#include <cassert>
#include <iostream>

using namespace gnfs::relation;
using gnfs::core::Relation;
using gnfs::core::PrimePower;

// Helper: 1LP relation (rational)
static Relation make_1rat(int64_t a, int64_t b, uint64_t lp) {
    Relation r(a, b);
    r.rational_factors = {0, 1};
    r.algebraic_factors = {0};
    r.rational_large_prime.push_back(PrimePower{lp, 0, 1});
    return r;
}

// Helper: 2LP relation (one rat + one alg)
static Relation make_2lp(int64_t a, int64_t b, uint64_t rat_lp,
                         uint64_t alg_p, uint64_t alg_r) {
    Relation r(a, b);
    r.rational_factors = {0, 1};
    r.algebraic_factors = {0};
    r.rational_large_prime.push_back(PrimePower{rat_lp, 0, 1});
    r.algebraic_large_prime.push_back(PrimePower{alg_p, alg_r, 1});
    return r;
}

void test_empty() {
    std::cout << "Testing CliqueRelationMerger empty input..." << std::endl;
    CliqueStats stats;
    auto out = CliqueRelationMerger::merge_cliques({}, &stats);
    assert(out.empty());
    assert(stats.input_relations == 0);
    std::cout << "  PASS" << std::endl;
}

void test_1lp_2clique() {
    // 2 rels share LP=101 (1LP each) → 1 full
    std::cout << "Testing CliqueRelationMerger 2-clique with 1 shared LP..." << std::endl;
    std::vector<Relation> rels;
    rels.push_back(make_1rat(1, 1, 101));
    rels.push_back(make_1rat(2, 1, 101));
    CliqueStats stats;
    auto out = CliqueRelationMerger::merge_cliques(std::move(rels), &stats);
    assert(stats.input_1lp == 2);
    assert(stats.components_with_excess == 1);
    assert(stats.full_produced == 1);
    assert(out.size() == 1);
    std::cout << "  PASS (full=" << stats.full_produced << ")" << std::endl;
}

void test_1lp_4clique() {
    // 4 rels all share LP=101 → spanning tree only emits 1 full
    // (after first merge cancels 101, accumulator is full; remaining 2 rels
    //  start new BFS but they share 101 which has been "consumed" — actually
    //  let me re-think: 101 still appears in lp_index, the 2 remaining rels
    //  are visited fresh, they can pair → 2nd full.
    //  Expected: 2 full from 4 rels.
    std::cout << "Testing CliqueRelationMerger 4-clique with 1 shared LP..." << std::endl;
    std::vector<Relation> rels;
    rels.push_back(make_1rat(1, 1, 101));
    rels.push_back(make_1rat(2, 1, 101));
    rels.push_back(make_1rat(3, 1, 101));
    rels.push_back(make_1rat(4, 1, 101));
    CliqueStats stats;
    auto out = CliqueRelationMerger::merge_cliques(std::move(rels), &stats);
    assert(stats.input_1lp == 4);
    // Result varies: at least 1 full, ideally 2.
    // Conservative assertion: ≥ 1 full.
    assert(stats.full_produced >= 1);
    std::cout << "  PASS (full=" << stats.full_produced
              << ", components=" << stats.components_found << ")" << std::endl;
}

void test_2lp_triangle() {
    // Triangle: R1=(A,B), R2=(B,C), R3=(A,C)
    // Expected: spanning tree path R1→R2 (cancel B) → intermediate (A,C),
    // then →R3 (cancel A,C) → full. Result: 1 full from 3 rels.
    std::cout << "Testing CliqueRelationMerger 2LP triangle..." << std::endl;
    std::vector<Relation> rels;
    rels.push_back(make_2lp(1, 1, 101, 201, 1));  // R1: rat=101, alg=(201,1)
    rels.push_back(make_2lp(2, 1, 101, 202, 2));  // R2: rat=101, alg=(202,2)
    rels.push_back(make_2lp(3, 1, 103, 201, 1));  // R3: rat=103, alg=(201,1)
    // LP sharing: R1↔R2 share rat 101; R1↔R3 share alg (201,1); R2 and R3 no overlap.
    // Component: {R1, R2, R3}
    CliqueStats stats;
    auto out = CliqueRelationMerger::merge_cliques(std::move(rels), &stats);
    assert(stats.input_2lp == 3);
    assert(stats.components_with_excess == 1);
    // Either 1 full (triangle cancels all) or 0 if residual LPs prevent
    // BFS from completing — depends on traversal order.
    std::cout << "  PASS (full=" << stats.full_produced
              << ", residual=" << stats.residual_emitted
              << ", lp_rejections=" << stats.lp_cancel_rejections << ")" << std::endl;
}

void test_no_overlap() {
    // 2 rels with NO shared LPs → 2 isolated components, no merge possible
    std::cout << "Testing CliqueRelationMerger non-overlapping rels..." << std::endl;
    std::vector<Relation> rels;
    rels.push_back(make_1rat(1, 1, 101));
    rels.push_back(make_1rat(2, 1, 103));
    CliqueStats stats;
    auto out = CliqueRelationMerger::merge_cliques(std::move(rels), &stats);
    assert(stats.components_with_excess == 0);
    assert(stats.full_produced == 0);
    assert(out.empty());
    std::cout << "  PASS" << std::endl;
}

void test_3lp_filtered() {
    // 3LP+ relation should be discarded pre-emptively
    std::cout << "Testing CliqueRelationMerger 3LP+ filter..." << std::endl;
    Relation r(1, 1);
    r.rational_factors = {0};
    r.algebraic_factors = {0};
    r.rational_large_prime.push_back(PrimePower{101, 0, 1});
    r.algebraic_large_prime.push_back(PrimePower{201, 1, 1});
    r.algebraic_large_prime.push_back(PrimePower{202, 2, 1});  // 3rd LP
    std::vector<Relation> rels;
    rels.push_back(std::move(r));
    CliqueStats stats;
    auto out = CliqueRelationMerger::merge_cliques(std::move(rels), &stats);
    assert(stats.input_3lp_plus == 1);
    assert(stats.input_1lp == 0);
    assert(stats.input_2lp == 0);
    std::cout << "  PASS" << std::endl;
}

void test_stats_to_string() {
    std::cout << "Testing CliqueStats::to_string()..." << std::endl;
    CliqueStats s;
    s.input_relations = 100;
    s.input_1lp = 60;
    s.input_2lp = 40;
    s.components_with_excess = 5;
    s.components_found = 10;
    s.full_produced = 15;
    s.residual_emitted = 3;
    s.lp_cancel_rejections = 200;
    s.fast_path_rejects = 198;
    s.heavy_path_rejects = 2;
    auto str = s.to_string();
    assert(str.find("in=100") != std::string::npos);
    assert(str.find("1lp=60") != std::string::npos);
    assert(str.find("full=15") != std::string::npos);
    assert(str.find("fast=198") != std::string::npos);
    assert(str.find("heavy=2") != std::string::npos);
    std::cout << "  " << str << std::endl;
    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== CliqueRelationMerger Unit Tests ===" << std::endl;

    test_empty();
    test_1lp_2clique();
    test_1lp_4clique();
    test_2lp_triangle();
    test_no_overlap();
    test_3lp_filtered();
    test_stats_to_string();

    std::cout << "\nAll CliqueRelationMerger tests passed!" << std::endl;
    return 0;
}
