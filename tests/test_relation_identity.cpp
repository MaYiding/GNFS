// Regression tests for exact raw-relation (a,b) identity.
//
// Raw rows are deduplicated before source IDs are assigned. Structured rows
// must instead be identified by their complete source-ID combinations.

#include "gnfs/relation/relation_identity.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <unordered_set>
#include <vector>

using gnfs::core::ABPair;
using gnfs::core::ABPairHash;
using gnfs::core::Relation;
using gnfs::relation::relation_source_combination;
using gnfs::relation::RelationSourceCombination;
using gnfs::relation::RelationSourceCombinationHash;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition, message)                                                                  \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(condition)) {                                                                        \
            ++g_failures;                                                                          \
            std::cerr << "FAIL: " << message << " at " << __FILE__ << ':' << __LINE__ << '\n';     \
        }                                                                                          \
    } while (false)

// Reproduce the information loss of the removed key without performing the
// old signed left shift. Unsigned shifting is defined and has the same low
// 64-bit packing behavior for these inputs.
uint64_t legacy_packed_key(const ABPair& ab) noexcept {
    return static_cast<uint64_t>(ab.a) ^ (ab.b << 32U);
}

void test_old_packed_key_collision() {
    const ABPair first{0, 1};
    const ABPair second{static_cast<int64_t>(UINT64_C(3) << 32U), 2};

    CHECK(first != second, "collision fixture must contain different ABPair values");
    CHECK(legacy_packed_key(first) == legacy_packed_key(second),
          "fixture must collide under the removed packed key");

    std::unordered_set<ABPair, ABPairHash> exact;
    CHECK(exact.insert(first).second, "first exact pair must be retained");
    CHECK(exact.insert(second).second,
          "a distinct pair that shares the old packed key must be retained");
    CHECK(exact.size() == 2, "exact identity must retain both colliding pairs");
}

void test_negative_and_full_width_endpoints() {
    constexpr int64_t min_a = std::numeric_limits<int64_t>::min();
    constexpr int64_t max_a = std::numeric_limits<int64_t>::max();
    constexpr uint64_t max_b = std::numeric_limits<uint64_t>::max();

    const std::vector<ABPair> endpoints{
        ABPair{min_a, max_b},
        ABPair{max_a, max_b},
        ABPair{-1, max_b},
        ABPair{min_a, max_b - 1},
    };

    std::unordered_set<ABPair, ABPairHash> exact;
    for (const ABPair& ab : endpoints) {
        CHECK(exact.insert(ab).second,
              "negative/full-width endpoint must not alias another exact pair");
    }
    CHECK(exact.size() == endpoints.size(),
          "all negative/full-width endpoint pairs must remain distinct");
    CHECK(!exact.insert(endpoints.front()).second,
          "only a completely equal ABPair may be rejected as a duplicate");
}

void test_first_occurrence_order() {
    const ABPair old_collision_a{0, 1};
    const ABPair old_collision_b{static_cast<int64_t>(UINT64_C(3) << 32U), 2};
    const ABPair negative{-7, std::numeric_limits<uint64_t>::max()};
    const ABPair endpoint{std::numeric_limits<int64_t>::min(),
                          std::numeric_limits<uint64_t>::max()};

    const std::vector<ABPair> input{
        old_collision_a, negative, old_collision_b, old_collision_a, endpoint, negative,
    };

    std::unordered_set<ABPair, ABPairHash> seen;
    std::vector<ABPair> kept;
    for (const ABPair& ab : input) {
        if (seen.insert(ab).second) {
            kept.push_back(ab);
        }
    }

    const std::vector<ABPair> expected{
        old_collision_a,
        negative,
        old_collision_b,
        endpoint,
    };
    CHECK(kept == expected, "exact raw dedup must preserve the first occurrence and input order");
}

Relation materialized_relation(const ABPair& primary, std::initializer_list<ABPair> extras) {
    Relation relation(primary.a, primary.b);
    for (const auto& extra : extras) {
        relation.extra_ab_pairs.emplace_back(extra.a, extra.b);
    }
    return relation;
}

void test_shared_primary_distinct_combinations_survive() {
    const ABPair primary{11, 13};
    const ABPair left_source{17, 19};
    const ABPair right_source{23, 29};

    Relation left = materialized_relation(primary, {left_source});
    Relation right = materialized_relation(primary, {right_source});

    CHECK(left.ab() == right.ab(), "fixture must share the same materialized primary pair");

    std::unordered_set<RelationSourceCombination, RelationSourceCombinationHash> seen;
    CHECK(seen.insert(relation_source_combination(left)).second,
          "first structured source combination must be retained");
    CHECK(seen.insert(relation_source_combination(right)).second,
          "different source combinations sharing a primary pair must survive");
    CHECK(seen.size() == 2, "primary-pair equality must not collapse structured rows");
}

void test_same_source_set_is_order_and_primary_independent() {
    const ABPair first{31, 37};
    const ABPair second{41, 43};
    const ABPair third{47, 53};

    Relation lhs = materialized_relation(first, {second, third});
    Relation rhs = materialized_relation(third, {first, second});

    const auto lhs_identity = relation_source_combination(lhs);
    const auto rhs_identity = relation_source_combination(rhs);
    CHECK(lhs.ab() != rhs.ab(), "fixture must use a different primary materialization order");
    CHECK(lhs_identity == rhs_identity, "the same source set must have one canonical identity");

    std::unordered_set<RelationSourceCombination, RelationSourceCombinationHash> seen;
    CHECK(seen.insert(lhs_identity).second, "first materialization must be retained");
    CHECK(!seen.insert(rhs_identity).second,
          "an equivalent source set must be deduplicated regardless of order");
}

void test_nested_provenance_uses_gf2_normalization() {
    const ABPair first{59, 61};
    const ABPair second{67, 71};
    const ABPair third{73, 79};
    const ABPair fourth{83, 89};

    // This is the flattened representation produced when already-merged rows
    // are merged again: first and second each occur twice and must cancel.
    Relation nested = materialized_relation(first, {second, third, first, second, fourth});
    const auto identity = relation_source_combination(nested);
    const std::vector<ABPair> expected{third, fourth};
    CHECK(identity.sources == expected,
          "nested duplicate sources must cancel by symmetric difference");
}

void test_structured_identity_preserves_old_collision_pair() {
    const ABPair old_collision_a{0, 1};
    const ABPair old_collision_b{static_cast<int64_t>(UINT64_C(3) << 32U), 2};
    const ABPair shared_extra{97, 101};

    Relation lhs = materialized_relation(old_collision_a, {shared_extra});
    Relation rhs = materialized_relation(old_collision_b, {shared_extra});
    CHECK(legacy_packed_key(lhs.ab()) == legacy_packed_key(rhs.ab()),
          "structured fixture must reproduce the removed primary-key collision");
    CHECK(relation_source_combination(lhs) != relation_source_combination(rhs),
          "full-width structured identity must preserve both colliding sources");
}

} // namespace

int main() {
    test_old_packed_key_collision();
    test_negative_and_full_width_endpoints();
    test_first_occurrence_order();
    test_shared_primary_distinct_combinations_survive();
    test_same_source_set_is_order_and_primary_independent();
    test_nested_provenance_uses_gf2_normalization();
    test_structured_identity_preserves_old_collision_pair();

    if (g_failures != 0) {
        std::cerr << g_failures << " of " << g_checks << " checks failed\n";
        return 1;
    }
    std::cout << "All " << g_checks << " exact relation identity checks passed\n";
    return 0;
}
