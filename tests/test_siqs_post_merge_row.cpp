// test_siqs_post_merge_row.cpp - SIQS sparse-wide post-merge row contracts

#include <gnfs/core/integer.hpp>
#include <gnfs/siqs/congruence.hpp>
#include <gnfs/siqs/post_merge_row.hpp>
#include <gnfs/siqs/relation.hpp>
#include <gnfs/siqs/two_large_prime_materializer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <utility>
#include <vector>

namespace {

using std::size_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

using gnfs::core::Integer;
using gnfs::siqs::are_congruent_squares;
using gnfs::siqs::check_siqs_post_merge_row_identity;
using gnfs::siqs::make_cycle_post_merge_row;
using gnfs::siqs::make_full_post_merge_row;
using gnfs::siqs::materialize_two_large_prime_cycle;
using gnfs::siqs::MaterializedTwoLargePrimeCycle;
using gnfs::siqs::SIQSFactorPower;
using gnfs::siqs::SIQSPostMergeRow;
using gnfs::siqs::SIQSPostMergeRowResult;
using gnfs::siqs::SIQSPostMergeRowStatus;
using gnfs::siqs::SIQSRelation;
using gnfs::siqs::SIQSSourceId;
using gnfs::siqs::TwoLargePrimeCycleSource;
using gnfs::siqs::visit_siqs_post_merge_odd_columns;

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':' << __LINE__ << '\n';        \
        }                                                                                          \
    } while (false)

const Integer relation_modulus(91);
const std::vector<uint32_t> factor_base_primes{0, 2, 3, 5};

[[nodiscard]] SIQSRelation make_relation(int64_t value, bool negative,
                                         std::vector<uint8_t> exponents,
                                         std::vector<uint32_t> fb_indices = {}) {
    SIQSRelation relation;
    relation.value = Integer(value);
    relation.negative = negative;
    relation.exponents = std::move(exponents);
    if (fb_indices.empty()) {
        for (size_t i = 1; i < relation.exponents.size(); ++i) {
            if (relation.exponents[i] != 0) {
                fb_indices.push_back(static_cast<uint32_t>(i));
            }
        }
    }
    relation.fb_indices = std::move(fb_indices);
    relation.large_prime = 0;
    relation.large_prime2 = 0;
    relation.merge_lps.clear();
    return relation;
}

[[nodiscard]] MaterializedTwoLargePrimeCycle make_one_lp_cycle() {
    // 62^2 = -(2^505 * 3^2 * 5) * 29^2 (mod 91).
    return MaterializedTwoLargePrimeCycle{Integer(62), true, {0, 505, 2, 1}, {29}, {2, 9}};
}

[[nodiscard]] MaterializedTwoLargePrimeCycle make_two_lp_cycle() {
    // 38^2 = 2^26 * 3^4 * 5^2 * (11 * 29 * 41)^2 (mod 91).
    return MaterializedTwoLargePrimeCycle{
        Integer(38), false, {0, 26, 4, 2}, {11, 29, 41}, {1, 3, 7}};
}

void check_result(const SIQSPostMergeRowResult& result, SIQSPostMergeRowStatus expected_status) {
    CHECK(result.status() == expected_status);
    CHECK(result.row().has_value() == (expected_status == SIQSPostMergeRowStatus::valid));
    CHECK(result.is_valid() == (expected_status == SIQSPostMergeRowStatus::valid));
}

[[nodiscard]] bool same_row(const SIQSPostMergeRow& lhs, const SIQSPostMergeRow& rhs) {
    return lhs.x_modulus == rhs.x_modulus && lhs.q_negative == rhs.q_negative &&
           lhs.factor_powers == rhs.factor_powers &&
           lhs.large_prime_sqrt_factors == rhs.large_prime_sqrt_factors &&
           lhs.source_ids == rhs.source_ids;
}

void multiply_mod(Integer& product, const Integer& factor) {
    mpz_mul(product.get_mpz(), product.get_mpz(), factor.get_mpz());
    mpz_mod(product.get_mpz(), product.get_mpz(), relation_modulus.get_mpz());
}

void test_full_conversion_and_result_moves() {
    // 1^2 - 91 = -2 * 3^2 * 5. Input sparse indices may be unsorted.
    const SIQSRelation full = make_relation(1, true, {0, 1, 2, 1}, {3, 1, 2});
    const auto result =
        make_full_post_merge_row(full, SIQSSourceId{10}, factor_base_primes, relation_modulus);
    check_result(result, SIQSPostMergeRowStatus::valid);
    if (!result.row()) {
        return;
    }

    const std::vector<SIQSFactorPower> expected_powers{{1, 1}, {2, 2}, {3, 1}};
    const std::vector<SIQSSourceId> expected_sources{{10}};
    CHECK(result.row()->x_modulus == Integer(1));
    CHECK(result.row()->q_negative);
    CHECK(result.row()->factor_powers == expected_powers);
    CHECK(result.row()->large_prime_sqrt_factors.empty());
    CHECK(result.row()->source_ids == expected_sources);
    CHECK(check_siqs_post_merge_row_identity(*result.row(), factor_base_primes, relation_modulus) ==
          SIQSPostMergeRowStatus::valid);
    CHECK(full.value == Integer(1));
    CHECK(full.fb_indices == std::vector<uint32_t>({3, 1, 2}));

    SIQSRelation movable_full = full;
    const auto rvalue_result = make_full_post_merge_row(std::move(movable_full), SIQSSourceId{10},
                                                        factor_base_primes, relation_modulus);
    check_result(rvalue_result, SIQSPostMergeRowStatus::valid);
    if (rvalue_result.row()) {
        CHECK(same_row(*result.row(), *rvalue_result.row()));
    }

    std::vector<size_t> odd_columns;
    visit_siqs_post_merge_odd_columns(
        *result.row(), [&odd_columns](size_t column) { odd_columns.push_back(column); });
    CHECK(odd_columns == std::vector<size_t>({0, 1, 3}));

    SIQSPostMergeRow revisited = *result.row();
    revisited.q_negative = false;
    revisited.factor_powers.clear();
    CHECK(check_siqs_post_merge_row_identity(revisited, factor_base_primes, relation_modulus) ==
          SIQSPostMergeRowStatus::valid);
    odd_columns.clear();
    visit_siqs_post_merge_odd_columns(
        revisited, [&odd_columns](size_t column) { odd_columns.push_back(column); });
    CHECK(odd_columns.empty());

    SIQSPostMergeRowResult copied = result;
    check_result(copied, SIQSPostMergeRowStatus::valid);
    SIQSPostMergeRowResult moved = std::move(copied);
    check_result(moved, SIQSPostMergeRowStatus::valid);
    check_result(copied, SIQSPostMergeRowStatus::invalid_post_merge_row);

    SIQSRelation mismatch = make_relation(2, true, {0, 1, 2, 1});
    auto assignment_target =
        make_full_post_merge_row(mismatch, SIQSSourceId{99}, factor_base_primes, relation_modulus);
    check_result(assignment_target, SIQSPostMergeRowStatus::row_identity_mismatch);
    assignment_target = result;
    check_result(assignment_target, SIQSPostMergeRowStatus::valid);
    assignment_target = std::move(moved);
    check_result(assignment_target, SIQSPostMergeRowStatus::valid);
    check_result(moved, SIQSPostMergeRowStatus::invalid_post_merge_row);
}

void test_full_rvalue_and_invalid_sources() {
    // The rvalue path canonicalizes the value modulo 91 before moving it.
    auto moved_full = make_relation(92, true, {0, 1, 2, 1});
    const auto moved_result = make_full_post_merge_row(
        std::move(moved_full), SIQSSourceId{UINT64_MAX}, factor_base_primes, relation_modulus);
    check_result(moved_result, SIQSPostMergeRowStatus::valid);
    if (moved_result.row()) {
        CHECK(moved_result.row()->x_modulus == Integer(1));
        CHECK(moved_result.row()->source_ids ==
              std::vector<SIQSSourceId>({SIQSSourceId{UINT64_MAX}}));
    }

    auto negative_value = make_relation(-1, true, {0, 1, 2, 1});
    const auto negative_value_result = make_full_post_merge_row(
        std::move(negative_value), SIQSSourceId{11}, factor_base_primes, relation_modulus);
    check_result(negative_value_result, SIQSPostMergeRowStatus::valid);
    if (negative_value_result.row()) {
        CHECK(negative_value_result.row()->x_modulus == Integer(90));
    }

    auto bad = make_relation(1, true, {0, 1, 2, 1});
    bad.large_prime = 29;
    check_result(make_full_post_merge_row(bad, {1}, factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::invalid_source_relation);
    bad = make_relation(1, true, {0, 1, 2, 1});
    bad.large_prime2 = 1;
    check_result(make_full_post_merge_row(bad, {1}, factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::invalid_source_relation);
    bad = make_relation(1, true, {0, 1, 2, 1});
    bad.merge_lps = {29};
    check_result(make_full_post_merge_row(bad, {1}, factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::invalid_source_relation);
    bad = make_relation(1, true, {0, 1, 2});
    check_result(make_full_post_merge_row(bad, {1}, factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::invalid_source_relation);
    bad = make_relation(1, true, {1, 1, 2, 1}, {1, 2, 3});
    check_result(make_full_post_merge_row(bad, {1}, factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::invalid_source_relation);
    bad = make_relation(1, true, {0, 1, 2, 1}, {1, 1, 2, 3});
    check_result(make_full_post_merge_row(bad, {1}, factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::invalid_source_relation);
    bad = make_relation(1, true, {0, 1, 2, 1}, {1, 2});
    check_result(make_full_post_merge_row(bad, {1}, factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::invalid_source_relation);
    bad = make_relation(1, true, {0, 1, 2, 0}, {1, 2, 3});
    check_result(make_full_post_merge_row(bad, {1}, factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::invalid_source_relation);
    bad = make_relation(1, true, {0, 1, 2, 1}, {1, 2, 3, 4});
    check_result(make_full_post_merge_row(bad, {1}, factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::invalid_source_relation);

    check_result(make_full_post_merge_row(make_relation(1, true, {0, 1, 2, 1}), {1},
                                          factor_base_primes, Integer(1)),
                 SIQSPostMergeRowStatus::invalid_modulus);
    check_result(make_full_post_merge_row(make_relation(1, true, {0, 1, 2, 1}), {1},
                                          std::vector<uint32_t>{1, 2, 3, 5}, relation_modulus),
                 SIQSPostMergeRowStatus::invalid_factor_base);
}

void test_cycle_conversion_copy_and_move() {
    const auto one_lp = make_one_lp_cycle();
    const std::vector<SIQSSourceId> one_lp_ids{{900}, {200}};
    const auto one_lp_result =
        make_cycle_post_merge_row(one_lp, one_lp_ids, factor_base_primes, relation_modulus);
    check_result(one_lp_result, SIQSPostMergeRowStatus::valid);
    if (one_lp_result.row()) {
        CHECK(one_lp_result.row()->x_modulus == Integer(62));
        CHECK(one_lp_result.row()->q_negative);
        CHECK(one_lp_result.row()->factor_powers ==
              std::vector<SIQSFactorPower>({{1, 505}, {2, 2}, {3, 1}}));
        CHECK(one_lp_result.row()->large_prime_sqrt_factors == std::vector<uint64_t>({29}));
        CHECK(one_lp_result.row()->source_ids == std::vector<SIQSSourceId>({{200}, {900}}));
    }
    CHECK(one_lp.factor_base_exponents[1] == 505);
    CHECK(one_lp.large_prime_square_roots == std::vector<uint64_t>({29}));

    auto movable_one_lp = one_lp;
    const auto one_lp_rvalue_result = make_cycle_post_merge_row(
        std::move(movable_one_lp), one_lp_ids, factor_base_primes, relation_modulus);
    check_result(one_lp_rvalue_result, SIQSPostMergeRowStatus::valid);
    if (one_lp_result.row() && one_lp_rvalue_result.row()) {
        CHECK(same_row(*one_lp_result.row(), *one_lp_rvalue_result.row()));
    }

    auto two_lp = make_two_lp_cycle();
    const std::vector<SIQSSourceId> two_lp_ids{{700}, {100}, {300}};
    const auto two_lp_result = make_cycle_post_merge_row(std::move(two_lp), two_lp_ids,
                                                         factor_base_primes, relation_modulus);
    check_result(two_lp_result, SIQSPostMergeRowStatus::valid);
    if (two_lp_result.row()) {
        CHECK(two_lp_result.row()->x_modulus == Integer(38));
        CHECK(!two_lp_result.row()->q_negative);
        CHECK(two_lp_result.row()->factor_powers ==
              std::vector<SIQSFactorPower>({{1, 26}, {2, 4}, {3, 2}}));
        CHECK(two_lp_result.row()->large_prime_sqrt_factors == std::vector<uint64_t>({11, 29, 41}));
        CHECK(two_lp_result.row()->source_ids == std::vector<SIQSSourceId>({{100}, {300}, {700}}));
    }
}

void test_cycle_rejections() {
    const auto valid_cycle = make_one_lp_cycle();
    check_result(make_cycle_post_merge_row(valid_cycle, std::vector<SIQSSourceId>{{1}},
                                           factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::invalid_source_ids);
    check_result(make_cycle_post_merge_row(valid_cycle, std::vector<SIQSSourceId>{{1}, {2}, {3}},
                                           factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::invalid_source_ids);
    check_result(make_cycle_post_merge_row(valid_cycle, std::vector<SIQSSourceId>{{1}, {1}},
                                           factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::invalid_source_ids);
    check_result(make_cycle_post_merge_row(valid_cycle, std::vector<SIQSSourceId>{{2}, {1}},
                                           factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::valid);

    auto bad_cycle = valid_cycle;
    bad_cycle.value_modulus = Integer(63);
    check_result(make_cycle_post_merge_row(bad_cycle, std::vector<SIQSSourceId>{{1}, {2}},
                                           factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::row_identity_mismatch);
    bad_cycle = valid_cycle;
    bad_cycle.factor_base_exponents[1] = 504;
    check_result(make_cycle_post_merge_row(bad_cycle, std::vector<SIQSSourceId>{{1}, {2}},
                                           factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::row_identity_mismatch);
    bad_cycle = valid_cycle;
    bad_cycle.large_prime_square_roots = {31};
    check_result(make_cycle_post_merge_row(bad_cycle, std::vector<SIQSSourceId>{{1}, {2}},
                                           factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::row_identity_mismatch);
    bad_cycle = valid_cycle;
    bad_cycle.large_prime_square_roots.clear();
    check_result(make_cycle_post_merge_row(bad_cycle, std::vector<SIQSSourceId>{{1}, {2}},
                                           factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::invalid_materialized_cycle);
    bad_cycle = valid_cycle;
    bad_cycle.relation_indices = {9, 2};
    check_result(make_cycle_post_merge_row(bad_cycle, std::vector<SIQSSourceId>{{1}, {2}},
                                           factor_base_primes, relation_modulus),
                 SIQSPostMergeRowStatus::invalid_materialized_cycle);

    check_result(make_cycle_post_merge_row(valid_cycle, std::vector<SIQSSourceId>{{1}, {2}},
                                           factor_base_primes, Integer(0)),
                 SIQSPostMergeRowStatus::invalid_modulus);
    check_result(make_cycle_post_merge_row(valid_cycle, std::vector<SIQSSourceId>{{1}, {2}},
                                           std::vector<uint32_t>{0, 2, 2, 5}, relation_modulus),
                 SIQSPostMergeRowStatus::invalid_factor_base);
}

void test_generic_row_validation() {
    SIQSPostMergeRow unit{Integer(1), false, {}, {}, {{1}}};
    CHECK(check_siqs_post_merge_row_identity(unit, factor_base_primes, relation_modulus) ==
          SIQSPostMergeRowStatus::valid);

    // Repeated square-root factors are a multiset, not a set.
    SIQSPostMergeRow repeated_roots{Integer(9), false, {}, {11, 11}, {{1}}};
    CHECK(check_siqs_post_merge_row_identity(repeated_roots, factor_base_primes,
                                             relation_modulus) == SIQSPostMergeRowStatus::valid);

    const std::vector<TwoLargePrimeCycleSource> repeated_root_sources{
        {0, Integer(11), false, {0, 0, 0, 0}, 11, 11},
        {1, Integer(11), false, {0, 0, 0, 0}, 11, 11},
    };
    const std::vector<size_t> repeated_root_support{0, 1};
    const auto materialized_repeated_roots = materialize_two_large_prime_cycle(
        repeated_root_sources, repeated_root_support, relation_modulus);
    CHECK(materialized_repeated_roots.has_value());
    if (materialized_repeated_roots) {
        CHECK(materialized_repeated_roots->value_modulus == Integer(30));
        CHECK(materialized_repeated_roots->large_prime_square_roots ==
              std::vector<uint64_t>({11, 11}));
        const std::vector<SIQSSourceId> mapped_ids{{8}, {7}};
        const auto converted = make_cycle_post_merge_row(*materialized_repeated_roots, mapped_ids,
                                                         factor_base_primes, relation_modulus);
        check_result(converted, SIQSPostMergeRowStatus::valid);
        if (converted.row()) {
            CHECK(converted.row()->large_prime_sqrt_factors == std::vector<uint64_t>({11, 11}));
            CHECK(converted.row()->source_ids == std::vector<SIQSSourceId>({{7}, {8}}));
        }
    }

    auto bad = unit;
    bad.source_ids.clear();
    CHECK(check_siqs_post_merge_row_identity(bad, factor_base_primes, relation_modulus) ==
          SIQSPostMergeRowStatus::invalid_source_ids);
    bad = unit;
    bad.source_ids = {{2}, {1}};
    CHECK(check_siqs_post_merge_row_identity(bad, factor_base_primes, relation_modulus) ==
          SIQSPostMergeRowStatus::invalid_source_ids);
    bad = unit;
    bad.source_ids = {{1}, {1}};
    CHECK(check_siqs_post_merge_row_identity(bad, factor_base_primes, relation_modulus) ==
          SIQSPostMergeRowStatus::invalid_source_ids);

    bad = unit;
    bad.x_modulus = Integer(-1);
    CHECK(check_siqs_post_merge_row_identity(bad, factor_base_primes, relation_modulus) ==
          SIQSPostMergeRowStatus::invalid_post_merge_row);
    bad = unit;
    bad.x_modulus = relation_modulus;
    CHECK(check_siqs_post_merge_row_identity(bad, factor_base_primes, relation_modulus) ==
          SIQSPostMergeRowStatus::invalid_post_merge_row);
    bad = unit;
    bad.factor_powers = {{0, 2}};
    CHECK(check_siqs_post_merge_row_identity(bad, factor_base_primes, relation_modulus) ==
          SIQSPostMergeRowStatus::invalid_post_merge_row);
    bad = unit;
    bad.factor_powers = {{1, 0}};
    CHECK(check_siqs_post_merge_row_identity(bad, factor_base_primes, relation_modulus) ==
          SIQSPostMergeRowStatus::invalid_post_merge_row);
    bad = unit;
    bad.factor_powers = {{2, 2}, {1, 2}};
    CHECK(check_siqs_post_merge_row_identity(bad, factor_base_primes, relation_modulus) ==
          SIQSPostMergeRowStatus::invalid_post_merge_row);
    bad = unit;
    bad.factor_powers = {{1, 2}, {1, 4}};
    CHECK(check_siqs_post_merge_row_identity(bad, factor_base_primes, relation_modulus) ==
          SIQSPostMergeRowStatus::invalid_post_merge_row);
    bad = unit;
    bad.factor_powers = {{4, 2}};
    CHECK(check_siqs_post_merge_row_identity(bad, factor_base_primes, relation_modulus) ==
          SIQSPostMergeRowStatus::invalid_post_merge_row);
    bad = unit;
    bad.large_prime_sqrt_factors = {1};
    CHECK(check_siqs_post_merge_row_identity(bad, factor_base_primes, relation_modulus) ==
          SIQSPostMergeRowStatus::invalid_post_merge_row);
    bad = unit;
    bad.large_prime_sqrt_factors = {29, 11};
    CHECK(check_siqs_post_merge_row_identity(bad, factor_base_primes, relation_modulus) ==
          SIQSPostMergeRowStatus::invalid_post_merge_row);
    bad = unit;
    bad.x_modulus = Integer(2);
    CHECK(check_siqs_post_merge_row_identity(bad, factor_base_primes, relation_modulus) ==
          SIQSPostMergeRowStatus::row_identity_mismatch);

    CHECK(check_siqs_post_merge_row_identity(unit, factor_base_primes, Integer(-91)) ==
          SIQSPostMergeRowStatus::invalid_modulus);
    CHECK(check_siqs_post_merge_row_identity(unit, std::vector<uint32_t>{0, 3, 2, 5},
                                             relation_modulus) ==
          SIQSPostMergeRowStatus::invalid_factor_base);
}

void test_mixed_full_and_cycle_oracle() {
    const auto full = make_full_post_merge_row(make_relation(1, true, {0, 1, 2, 1}), {10},
                                               factor_base_primes, relation_modulus);
    const auto one_lp =
        make_cycle_post_merge_row(make_one_lp_cycle(), std::vector<SIQSSourceId>{{20}, {21}},
                                  factor_base_primes, relation_modulus);
    const auto two_lp =
        make_cycle_post_merge_row(make_two_lp_cycle(), std::vector<SIQSSourceId>{{30}, {31}, {32}},
                                  factor_base_primes, relation_modulus);
    CHECK(full.is_valid());
    CHECK(one_lp.is_valid());
    CHECK(two_lp.is_valid());
    if (!full.row() || !one_lp.row() || !two_lp.row()) {
        return;
    }

    const std::vector<const SIQSPostMergeRow*> dependency{&*full.row(), &*one_lp.row(),
                                                          &*two_lp.row()};
    std::vector<uint64_t> exponent_sums(factor_base_primes.size(), 0);
    std::vector<bool> parity(factor_base_primes.size(), false);
    Integer x_modulus(1);
    Integer y_modulus(1);

    for (const SIQSPostMergeRow* row : dependency) {
        multiply_mod(x_modulus, row->x_modulus);
        visit_siqs_post_merge_odd_columns(
            *row, [&parity](size_t column) { parity[column] = !parity[column]; });
        for (const SIQSFactorPower& power : row->factor_powers) {
            exponent_sums[power.factor_base_index] += power.exponent;
        }
        for (const uint64_t large_prime : row->large_prime_sqrt_factors) {
            multiply_mod(y_modulus, Integer(large_prime));
        }
    }

    CHECK(std::none_of(parity.begin(), parity.end(), [](bool value) { return value; }));
    CHECK(exponent_sums == std::vector<uint64_t>({0, 532, 8, 4}));
    for (size_t i = 1; i < factor_base_primes.size(); ++i) {
        const auto factor = gnfs::core::powmod(Integer(factor_base_primes[i]),
                                               Integer(exponent_sums[i] / 2), relation_modulus);
        multiply_mod(y_modulus, factor);
    }

    CHECK(x_modulus == Integer(81));
    CHECK(y_modulus == Integer(3));
    CHECK(are_congruent_squares(x_modulus, y_modulus, relation_modulus));
    CHECK(gnfs::core::gcd(x_modulus - y_modulus, relation_modulus) == Integer(13));
    CHECK(gnfs::core::gcd(x_modulus + y_modulus, relation_modulus) == Integer(7));
}

} // namespace

int main() {
    test_full_conversion_and_result_moves();
    test_full_rvalue_and_invalid_sources();
    test_cycle_conversion_copy_and_move();
    test_cycle_rejections();
    test_generic_row_validation();
    test_mixed_full_and_cycle_oracle();

    std::cout << "SIQS post-merge row: " << checks_passed << " checks passed, " << checks_failed
              << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
