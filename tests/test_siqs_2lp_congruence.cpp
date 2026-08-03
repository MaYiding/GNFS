// test_siqs_2lp_congruence.cpp - SIQS 2LP modular-congruence contracts

#include <gnfs/core/integer.hpp>
#include <gnfs/siqs/congruence.hpp>
#include <gnfs/siqs/siqs.hpp>
#include <gnfs/siqs/two_large_prime_adapter.hpp>
#include <gnfs/siqs/two_large_prime_congruence.hpp>
#include <gnfs/siqs/two_large_prime_graph.hpp>
#include <gnfs/siqs/two_large_prime_materializer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
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
using gnfs::siqs::build_two_large_prime_cycle_basis;
using gnfs::siqs::check_materialized_two_large_prime_identity;
using gnfs::siqs::FBPrime;
using gnfs::siqs::materialize_two_large_prime_cycle;
using gnfs::siqs::MaterializedTwoLargePrimeCycle;
using gnfs::siqs::prepare_two_large_prime_corpus;
using gnfs::siqs::SIQSRelation;
using gnfs::siqs::try_extract;
using gnfs::siqs::TwoLargePrimeCongruenceStatus;
using gnfs::siqs::TwoLargePrimeCycleSource;
using gnfs::siqs::TwoLargePrimeDependencyResult;
using gnfs::siqs::TwoLargePrimeEdge;
using gnfs::siqs::verify_materialized_two_large_prime_dependency;

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

const Integer relation_modulus(101);
const std::vector<uint32_t> factor_base_primes{0, 2, 3, 5, 7};

[[nodiscard]] SIQSRelation make_relation(int64_t value, std::vector<uint8_t> exponents,
                                         uint64_t large_prime, uint64_t large_prime2,
                                         bool negative = false) {
    SIQSRelation relation;
    relation.value = Integer(value);
    relation.exponents = std::move(exponents);
    for (size_t i = 0; i < relation.exponents.size(); ++i) {
        if (relation.exponents[i] != 0) {
            relation.fb_indices.push_back(static_cast<uint32_t>(i));
        }
    }
    relation.large_prime = large_prime;
    relation.large_prime2 = large_prime2;
    relation.negative = negative;
    return relation;
}

[[nodiscard]] MaterializedTwoLargePrimeCycle make_row(int64_t value_modulus, bool negative,
                                                      std::vector<uint32_t> exponents,
                                                      std::vector<uint64_t> roots,
                                                      std::vector<size_t> relation_indices) {
    return MaterializedTwoLargePrimeCycle{
        Integer(value_modulus),      negative, std::move(exponents), std::move(roots),
        std::move(relation_indices),
    };
}

struct OracleSplitter {
    [[nodiscard]] std::pair<uint64_t, uint64_t> operator()(uint64_t cofactor) const noexcept {
        switch (cofactor) {
        case 169:
            return {13, 13};
        case 221:
            return {13, 17};
        case 247:
            return {13, 19};
        case 323:
            return {17, 19};
        default:
            return {0, 0};
        }
    }
};

[[nodiscard]] std::optional<MaterializedTwoLargePrimeCycle>
prepare_single_cycle(const std::vector<SIQSRelation>& relations, uint64_t large_prime_bound) {
    const auto corpus = prepare_two_large_prime_corpus(
        std::span<const SIQSRelation>(relations.data(), relations.size()),
        factor_base_primes.size(), large_prime_bound, OracleSplitter{});
    if (!corpus) {
        return std::nullopt;
    }

    const auto basis = build_two_large_prime_cycle_basis(
        std::span<const TwoLargePrimeEdge>(corpus->edges.data(), corpus->edges.size()));
    if (!basis || basis->cycles.size() != 1) {
        return std::nullopt;
    }

    const auto& cycle = basis->cycles.front();
    return materialize_two_large_prime_cycle(
        std::span<const TwoLargePrimeCycleSource>(corpus->sources.data(), corpus->sources.size()),
        std::span<const size_t>(cycle.data(), cycle.size()), relation_modulus);
}

void check_valid_identity(const MaterializedTwoLargePrimeCycle& row) {
    CHECK(check_materialized_two_large_prime_identity(
              row, std::span<const uint32_t>(factor_base_primes.data(), factor_base_primes.size()),
              relation_modulus) == TwoLargePrimeCongruenceStatus::valid);
}

void check_result(const TwoLargePrimeDependencyResult& result,
                  TwoLargePrimeCongruenceStatus expected_status) {
    CHECK(result.status() == expected_status);
    CHECK(result.verified().has_value() ==
          (result.status() == TwoLargePrimeCongruenceStatus::valid));
    CHECK(result.is_valid() == (result.status() == TwoLargePrimeCongruenceStatus::valid));
}

[[nodiscard]] TwoLargePrimeDependencyResult
verify_dependency(const std::vector<MaterializedTwoLargePrimeCycle>& rows,
                  const std::vector<size_t>& dependency,
                  const std::vector<uint32_t>& primes = factor_base_primes,
                  const Integer& modulus = relation_modulus) {
    return verify_materialized_two_large_prime_dependency(
        std::span<const MaterializedTwoLargePrimeCycle>(rows.data(), rows.size()),
        std::span<const size_t>(dependency.data(), dependency.size()),
        std::span<const uint32_t>(primes.data(), primes.size()), modulus);
}

void test_generic_square_congruence() {
    CHECK(are_congruent_squares(Integer(87), Integer(14), relation_modulus));
    CHECK(are_congruent_squares(Integer(-14), Integer(14), relation_modulus));
    CHECK(are_congruent_squares(Integer(188), Integer(14), relation_modulus));
    CHECK(!are_congruent_squares(Integer(87), Integer(15), relation_modulus));
    CHECK(!are_congruent_squares(Integer(1), Integer(1), Integer(1)));
    CHECK(!are_congruent_squares(Integer(1), Integer(1), Integer(0)));
    CHECK(!are_congruent_squares(Integer(1), Integer(1), Integer(-101)));
}

[[nodiscard]] MaterializedTwoLargePrimeCycle test_one_lp_parallel_identity() {
    // 26^2 - 101 = 5^2 * 23
    // 49^2 - 101 = 2^2 * 5^2 * 23
    const std::vector<SIQSRelation> relations{
        make_relation(26, {0, 0, 0, 2, 0}, 23, 0),
        make_relation(49, {0, 2, 0, 2, 0}, 23, 0),
    };
    const auto row = prepare_single_cycle(relations, 23);
    CHECK(row.has_value());
    if (!row) {
        return make_row(0, false, {0, 0, 0, 0, 0}, {2}, {0});
    }

    CHECK(row->value_modulus == Integer(62));
    CHECK(!row->negative);
    const std::vector<uint32_t> expected_exponents{0, 2, 0, 4, 0};
    const std::vector<uint64_t> expected_roots{23};
    const std::vector<size_t> expected_indices{0, 1};
    CHECK(row->factor_base_exponents == expected_exponents);
    CHECK(row->large_prime_square_roots == expected_roots);
    CHECK(row->relation_indices == expected_indices);
    check_valid_identity(*row);
    return *row;
}

void test_two_lp_triangle_identity() {
    // Each exact cofactor is carried in the raw sentinel encoding and split at
    // the adapter boundary.
    const std::vector<SIQSRelation> relations{
        // 149^2 - 101 = 2^2 * 5^2 * 13 * 17
        make_relation(149, {0, 2, 0, 2, 0}, 221, 1),
        // 33^2 - 101 = 2^2 * 13 * 19
        make_relation(33, {0, 2, 0, 0, 0}, 247, 1),
        // 81^2 - 101 = 2^2 * 5 * 17 * 19
        make_relation(81, {0, 2, 0, 1, 0}, 323, 1),
    };
    const auto row = prepare_single_cycle(relations, 23);
    CHECK(row.has_value());
    if (!row) {
        return;
    }

    CHECK(row->value_modulus == Integer(34));
    CHECK(!row->negative);
    const std::vector<uint32_t> expected_exponents{0, 6, 0, 3, 0};
    const std::vector<uint64_t> expected_roots{13, 17, 19};
    const std::vector<size_t> expected_indices{0, 1, 2};
    CHECK(row->factor_base_exponents == expected_exponents);
    CHECK(row->large_prime_square_roots == expected_roots);
    CHECK(row->relation_indices == expected_indices);
    check_valid_identity(*row);
}

[[nodiscard]] MaterializedTwoLargePrimeCycle test_square_self_loop_identity() {
    // 59^2 - 101 = 2^2 * 5 * 13^2. The odd factor-base
    // exponent is valid for a materialized row.
    const std::vector<SIQSRelation> relations{
        make_relation(59, {0, 2, 0, 1, 0}, 169, 1),
    };
    const auto row = prepare_single_cycle(relations, 23);
    CHECK(row.has_value());
    if (!row) {
        return make_row(0, false, {0, 0, 0, 0, 0}, {2}, {0});
    }

    CHECK(row->value_modulus == Integer(59));
    CHECK(!row->negative);
    const std::vector<uint32_t> expected_exponents{0, 2, 0, 1, 0};
    const std::vector<uint64_t> expected_roots{13};
    CHECK(row->factor_base_exponents == expected_exponents);
    CHECK(row->large_prime_square_roots == expected_roots);
    check_valid_identity(*row);

    return *row;
}

[[nodiscard]] MaterializedTwoLargePrimeCycle test_negative_identity() {
    // 6^2 - 101 = -5 * 13
    // 19^2 - 101 = 2^2 * 5 * 13
    const std::vector<SIQSRelation> relations{
        make_relation(6, {0, 0, 0, 1, 0}, 13, 0, true),
        make_relation(19, {0, 2, 0, 1, 0}, 13, 0, false),
    };
    const auto row = prepare_single_cycle(relations, 23);
    CHECK(row.has_value());
    if (!row) {
        return make_row(0, false, {0, 0, 0, 0, 0}, {2}, {0});
    }

    CHECK(row->value_modulus == Integer(13));
    CHECK(row->negative);
    const std::vector<uint32_t> expected_exponents{0, 2, 0, 2, 0};
    const std::vector<uint64_t> expected_roots{13};
    CHECK(row->factor_base_exponents == expected_exponents);
    CHECK(row->large_prime_square_roots == expected_roots);
    check_valid_identity(*row);

    auto wrong_sign = *row;
    wrong_sign.negative = false;
    CHECK(check_materialized_two_large_prime_identity(wrong_sign, factor_base_primes,
                                                      relation_modulus) ==
          TwoLargePrimeCongruenceStatus::row_identity_mismatch);
    return *row;
}

void test_wide_exponent_identity() {
    // 2^101 * 7 == 2^201 * 7 == 14 (mod 101), whose roots are 32 and 69.
    const std::vector<SIQSRelation> relations{
        make_relation(32, {0, 101, 0, 0, 0}, 7, 0),
        make_relation(69, {0, 201, 0, 0, 0}, 7, 0),
    };
    const auto row = prepare_single_cycle(relations, 23);
    CHECK(row.has_value());
    if (!row) {
        return;
    }

    CHECK(row->value_modulus == Integer(87));
    const std::vector<uint32_t> expected_exponents{0, 302, 0, 0, 0};
    const std::vector<uint64_t> expected_roots{7};
    CHECK(row->factor_base_exponents == expected_exponents);
    CHECK(row->large_prime_square_roots == expected_roots);
    check_valid_identity(*row);
}

void test_maximum_even_exponent_identity() {
    const auto row =
        make_row(37, false, {0, std::numeric_limits<uint32_t>::max() - 1U, 0, 0, 0}, {7}, {0});
    // 2^(UINT32_MAX-1) * 7^2 == 37^2 == 56 (mod 101).
    check_valid_identity(row);

    auto second_row = row;
    second_row.relation_indices = {1};
    const std::vector<MaterializedTwoLargePrimeCycle> rows{row, second_row};
    const auto result = verify_dependency(rows, {0, 1});
    check_result(result, TwoLargePrimeCongruenceStatus::valid);
    if (result.verified()) {
        // The aggregate exponent and its half both exceed UINT32_MAX.
        CHECK(result.verified()->x_modulus == Integer(56));
        CHECK(result.verified()->y_modulus == Integer(56));
    }
}

[[nodiscard]] MaterializedTwoLargePrimeCycle test_uint64_large_prime_identity() {
    constexpr uint64_t large_prime = UINT64_C(18446744073709551557);
    // large_prime == 20 (mod 101); 11 and 90 are its square roots.
    const std::vector<SIQSRelation> relations{
        make_relation(11, {0, 0, 0, 0, 0}, large_prime, 0),
        make_relation(90, {0, 0, 0, 0, 0}, large_prime, 0),
    };
    const auto row = prepare_single_cycle(relations, large_prime);
    CHECK(row.has_value());
    if (!row) {
        return make_row(0, false, {0, 0, 0, 0, 0}, {2}, {0});
    }

    CHECK(row->value_modulus == Integer(81));
    const std::vector<uint64_t> expected_roots{large_prime};
    CHECK(row->large_prime_square_roots == expected_roots);
    check_valid_identity(*row);
    return *row;
}

void test_identity_mismatches(const MaterializedTwoLargePrimeCycle& self_loop) {
    auto bad_value = self_loop;
    bad_value.value_modulus = Integer(60);
    CHECK(check_materialized_two_large_prime_identity(bad_value, factor_base_primes,
                                                      relation_modulus) ==
          TwoLargePrimeCongruenceStatus::row_identity_mismatch);

    auto bad_exponent = self_loop;
    bad_exponent.factor_base_exponents[3] = 2;
    CHECK(check_materialized_two_large_prime_identity(bad_exponent, factor_base_primes,
                                                      relation_modulus) ==
          TwoLargePrimeCongruenceStatus::row_identity_mismatch);

    auto bad_large_prime = self_loop;
    bad_large_prime.large_prime_square_roots[0] = 17;
    CHECK(check_materialized_two_large_prime_identity(bad_large_prime, factor_base_primes,
                                                      relation_modulus) ==
          TwoLargePrimeCongruenceStatus::row_identity_mismatch);

    const std::vector<uint32_t> wrong_but_well_formed_factor_base{0, 2, 3, 7, 11};
    CHECK(check_materialized_two_large_prime_identity(self_loop, wrong_but_well_formed_factor_base,
                                                      relation_modulus) ==
          TwoLargePrimeCongruenceStatus::row_identity_mismatch);
}

void test_identity_malformed_boundaries(const MaterializedTwoLargePrimeCycle& valid_row) {
    for (const int modulus : {-101, 0, 1}) {
        CHECK(check_materialized_two_large_prime_identity(valid_row, factor_base_primes,
                                                          Integer(modulus)) ==
              TwoLargePrimeCongruenceStatus::invalid_modulus);
    }

    const std::vector<std::vector<uint32_t>> invalid_factor_bases{
        {}, {1, 2, 3}, {0, 1, 3}, {0, 2, 2}, {0, 3, 2},
    };
    for (const auto& primes : invalid_factor_bases) {
        CHECK(check_materialized_two_large_prime_identity(valid_row, primes, relation_modulus) ==
              TwoLargePrimeCongruenceStatus::invalid_factor_base);
    }

    auto negative_value = valid_row;
    negative_value.value_modulus = Integer(-1);
    CHECK(check_materialized_two_large_prime_identity(negative_value, factor_base_primes,
                                                      relation_modulus) ==
          TwoLargePrimeCongruenceStatus::invalid_materialized_cycle);

    auto unreduced_value = valid_row;
    unreduced_value.value_modulus = relation_modulus;
    CHECK(check_materialized_two_large_prime_identity(unreduced_value, factor_base_primes,
                                                      relation_modulus) ==
          TwoLargePrimeCongruenceStatus::invalid_materialized_cycle);

    auto wrong_width = valid_row;
    wrong_width.factor_base_exponents.pop_back();
    CHECK(check_materialized_two_large_prime_identity(wrong_width, factor_base_primes,
                                                      relation_modulus) ==
          TwoLargePrimeCongruenceStatus::invalid_materialized_cycle);

    auto sign_sentinel_exponent = valid_row;
    sign_sentinel_exponent.factor_base_exponents[0] = 1;
    CHECK(check_materialized_two_large_prime_identity(sign_sentinel_exponent, factor_base_primes,
                                                      relation_modulus) ==
          TwoLargePrimeCongruenceStatus::invalid_materialized_cycle);

    auto no_roots = valid_row;
    no_roots.large_prime_square_roots.clear();
    CHECK(check_materialized_two_large_prime_identity(no_roots, factor_base_primes,
                                                      relation_modulus) ==
          TwoLargePrimeCongruenceStatus::invalid_materialized_cycle);

    auto invalid_root = valid_row;
    invalid_root.large_prime_square_roots = {1};
    CHECK(check_materialized_two_large_prime_identity(invalid_root, factor_base_primes,
                                                      relation_modulus) ==
          TwoLargePrimeCongruenceStatus::invalid_materialized_cycle);

    auto unsorted_roots = valid_row;
    unsorted_roots.large_prime_square_roots = {23, 13};
    CHECK(check_materialized_two_large_prime_identity(unsorted_roots, factor_base_primes,
                                                      relation_modulus) ==
          TwoLargePrimeCongruenceStatus::invalid_materialized_cycle);

    auto no_relation_ids = valid_row;
    no_relation_ids.relation_indices.clear();
    CHECK(check_materialized_two_large_prime_identity(no_relation_ids, factor_base_primes,
                                                      relation_modulus) ==
          TwoLargePrimeCongruenceStatus::invalid_materialized_cycle);

    auto unsorted_relation_ids = valid_row;
    unsorted_relation_ids.relation_indices = {1, 0};
    CHECK(check_materialized_two_large_prime_identity(unsorted_relation_ids, factor_base_primes,
                                                      relation_modulus) ==
          TwoLargePrimeCongruenceStatus::invalid_materialized_cycle);

    auto duplicate_relation_ids = valid_row;
    duplicate_relation_ids.relation_indices = {0, 0};
    CHECK(check_materialized_two_large_prime_identity(duplicate_relation_ids, factor_base_primes,
                                                      relation_modulus) ==
          TwoLargePrimeCongruenceStatus::invalid_materialized_cycle);

    // Repeated LP roots are canonical and represent a degree-four vertex.
    const auto repeated_root_row = make_row(68, false, {0, 0, 0, 0, 0}, {13, 13}, {0, 1});
    check_valid_identity(repeated_root_row);
    const std::vector<MaterializedTwoLargePrimeCycle> repeated_root_rows{repeated_root_row};
    const auto repeated_root_dependency = verify_dependency(repeated_root_rows, {0});
    check_result(repeated_root_dependency, TwoLargePrimeCongruenceStatus::valid);
    if (repeated_root_dependency.verified()) {
        CHECK(repeated_root_dependency.verified()->x_modulus == Integer(68));
        CHECK(repeated_root_dependency.verified()->y_modulus == Integer(68));
    }
}

void test_dependencies(const MaterializedTwoLargePrimeCycle& square_row,
                       const MaterializedTwoLargePrimeCycle& odd_exponent_row,
                       const MaterializedTwoLargePrimeCycle& negative_row,
                       const MaterializedTwoLargePrimeCycle& uint64_lp_row) {
    {
        const std::vector<MaterializedTwoLargePrimeCycle> rows{square_row};
        const auto result = verify_dependency(rows, {0});
        check_result(result, TwoLargePrimeCongruenceStatus::valid);
        if (result.verified()) {
            CHECK(result.verified()->x_modulus == Integer(62));
            CHECK(result.verified()->y_modulus == Integer(39));
            CHECK(are_congruent_squares(result.verified()->x_modulus, result.verified()->y_modulus,
                                        relation_modulus));
        }
    }

    {
        auto second_row = odd_exponent_row;
        second_row.relation_indices = {1};
        const std::vector<MaterializedTwoLargePrimeCycle> rows{odd_exponent_row, second_row};
        const auto result = verify_dependency(rows, {1, 0});
        check_result(result, TwoLargePrimeCongruenceStatus::valid);
        if (result.verified()) {
            CHECK(result.verified()->x_modulus == Integer(47));
            CHECK(result.verified()->y_modulus == Integer(47));
        }
    }

    {
        const std::vector<MaterializedTwoLargePrimeCycle> rows{negative_row};
        check_result(verify_dependency(rows, {0}),
                     TwoLargePrimeCongruenceStatus::dependency_not_square);
    }

    {
        auto second_negative_row = negative_row;
        second_negative_row.relation_indices = {2, 3};
        const std::vector<MaterializedTwoLargePrimeCycle> rows{negative_row, second_negative_row};
        const auto result = verify_dependency(rows, {0, 1});
        check_result(result, TwoLargePrimeCongruenceStatus::valid);
        if (result.verified()) {
            CHECK(result.verified()->x_modulus == Integer(68));
            CHECK(result.verified()->y_modulus == Integer(33));
        }
    }

    {
        const std::vector<MaterializedTwoLargePrimeCycle> rows{odd_exponent_row};
        check_result(verify_dependency(rows, {0}),
                     TwoLargePrimeCongruenceStatus::dependency_not_square);
    }

    {
        const std::vector<MaterializedTwoLargePrimeCycle> rows{square_row};
        check_result(verify_dependency(rows, {}),
                     TwoLargePrimeCongruenceStatus::invalid_dependency);
        check_result(verify_dependency(rows, {1}),
                     TwoLargePrimeCongruenceStatus::invalid_dependency);
        check_result(verify_dependency(rows, {0, 0}),
                     TwoLargePrimeCongruenceStatus::invalid_dependency);
    }

    {
        auto bad_row = square_row;
        bad_row.value_modulus = Integer(60);
        const std::vector<MaterializedTwoLargePrimeCycle> rows{bad_row};
        check_result(verify_dependency(rows, {0}),
                     TwoLargePrimeCongruenceStatus::row_identity_mismatch);
    }

    {
        const std::vector<MaterializedTwoLargePrimeCycle> rows{square_row};
        check_result(verify_dependency(rows, {0}, factor_base_primes, Integer(1)),
                     TwoLargePrimeCongruenceStatus::invalid_modulus);
        const std::vector<uint32_t> invalid_primes{0, 2, 2};
        check_result(verify_dependency(rows, {0}, invalid_primes),
                     TwoLargePrimeCongruenceStatus::invalid_factor_base);
    }

    {
        const std::vector<MaterializedTwoLargePrimeCycle> rows{uint64_lp_row};
        const auto result = verify_dependency(rows, {0});
        check_result(result, TwoLargePrimeCongruenceStatus::valid);
        if (result.verified()) {
            CHECK(result.verified()->x_modulus == Integer(81));
            CHECK(result.verified()->y_modulus == Integer(20));
        }
    }
}

void test_try_extract_congruence_gate() {
    const Integer modulus(15);
    const std::vector<FBPrime> factor_base{
        {0, 0, 0},
        {2, 1, 1},
    };
    const std::vector<size_t> dependency{0};

    {
        const std::vector<SIQSRelation> relations{
            make_relation(4, {0, 0}, 0, 0),
        };
        const auto factors = try_extract(modulus, modulus, relations, dependency, factor_base);
        CHECK(factors.has_value());
        if (factors) {
            CHECK(factors->first == Integer(3));
            CHECK(factors->second == Integer(5));
        }
    }

    {
        // gcd(5 + 1, 15) is 3, but 5^2 != 1^2 (mod 15). The complete
        // congruence gate must reject this dependency before either GCD.
        const std::vector<SIQSRelation> relations{
            make_relation(5, {0, 0}, 0, 0),
        };
        CHECK(!try_extract(modulus, modulus, relations, dependency, factor_base).has_value());
    }

    {
        // Exercise the production merge_lps conversion beyond Windows LLP64's
        // 32-bit unsigned long. With M=L^2-1, 1^2 == L^2 (mod M), and the
        // two non-trivial factors are exactly L-1 and L+1.
        constexpr uint64_t large_prime =
            static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 16U;
        const Integer large_prime_integer(large_prime);
        const Integer wide_modulus = large_prime_integer * large_prime_integer - Integer(1);
        const std::vector<FBPrime> sign_only_factor_base{{0, 0, 0}};
        SIQSRelation relation = make_relation(1, {0}, 0, 0);
        relation.merge_lps = {large_prime};
        const std::vector<SIQSRelation> relations{relation};

        const auto factors =
            try_extract(wide_modulus, wide_modulus, relations, dependency, sign_only_factor_base);
        CHECK(factors.has_value());
        if (factors) {
            CHECK(factors->first == Integer(large_prime - 1));
            CHECK(factors->second == Integer(large_prime + 1));
        }
    }
}

} // namespace

int main() {
    test_generic_square_congruence();
    const auto parallel_row = test_one_lp_parallel_identity();
    test_two_lp_triangle_identity();
    const auto self_loop_row = test_square_self_loop_identity();
    const auto negative_row = test_negative_identity();
    test_wide_exponent_identity();
    test_maximum_even_exponent_identity();
    const auto uint64_lp_row = test_uint64_large_prime_identity();
    test_identity_mismatches(self_loop_row);
    test_identity_malformed_boundaries(self_loop_row);
    test_dependencies(parallel_row, self_loop_row, negative_row, uint64_lp_row);
    test_try_extract_congruence_gate();

    std::cout << checks_passed << " checks passed, " << checks_failed << " checks failed\n";
    return checks_failed == 0 ? 0 : 1;
}
