#include <gnfs/cofactor/candidate_batch.hpp>
#include <gnfs/cofactor/cofactorizer.hpp>
#include <gnfs/core/integer.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/sieve/lattice_sieve.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using gnfs::cofactor::CandidateBatchOptions;
using gnfs::cofactor::CandidateBatchResult;
using gnfs::cofactor::Cofactorizer;
using gnfs::cofactor::CofactorizerConfig;
using gnfs::cofactor::verify_candidate_batch;
using gnfs::core::Integer;
using gnfs::core::PolynomialContext;
using gnfs::core::Relation;
using gnfs::factor_base::FactorBase;
using gnfs::factor_base::FactorBaseBuilder;
using gnfs::sieve::SieveCandidate;
using gnfs::sieve::SieveResult;
using gnfs::sieve::SpecialQ;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

struct Fixture {
    PolynomialContext ctx;
    FactorBase fb;
    CofactorizerConfig config;
};

[[nodiscard]] Fixture make_fixture() {
    std::vector<Integer> coefficients;
    coefficients.emplace_back(5);
    coefficients.emplace_back(5);
    coefficients.emplace_back(3);
    PolynomialContext ctx(Integer(143), std::move(coefficients), Integer(6));

    FactorBaseBuilder::Options options;
    options.rational_bound = 50;
    options.algebraic_bound = 50;
    options.special_q_bound = 200;
    options.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, options);
    require(fb.algebraic_count() > 12, "candidate batch fixture factor base is too short");
    require(fb.algebraic()[10].p == 71 && fb.algebraic()[10].r == 10,
            "candidate batch fixture lost special-Q 71/10 at index 10");
    require(fb.algebraic()[11].p == 71 && fb.algebraic()[11].r == 12,
            "candidate batch fixture lost special-Q 71/12 at index 11");
    require(fb.algebraic()[12].p == 73 && fb.algebraic()[12].r == 4,
            "candidate batch fixture lost special-Q 73/4 at index 12");

    CofactorizerConfig config;
    config.large_prime_bound = 50;
    return Fixture{std::move(ctx), std::move(fb), config};
}

[[nodiscard]] SieveCandidate candidate(int64_t a, uint64_t b) {
    return SieveCandidate{0, 0, a, b, 0};
}

[[nodiscard]] std::vector<SieveResult> make_sieve_results() {
    std::vector<SieveResult> results(3);
    results[0].special_q = SpecialQ{71, 10, 10};
    results[0].candidates = {candidate(-61, 1), candidate(10, 1),   candidate(-193, 2),
                             candidate(81, 1),  candidate(-254, 3), candidate(-112, 3)};
    results[1].special_q = SpecialQ{73, 4, 12};
    results[2].special_q = SpecialQ{71, 12, 11};
    results[2].candidates = {candidate(12, 1), candidate(83, 1), candidate(-23, 4),
                             candidate(-11, 5)};
    return results;
}

void require_valid_special_q_lattice_points(std::span<const SieveResult> sieve_results) {
    for (const auto& sieve_result : sieve_results) {
        const int64_t q = static_cast<int64_t>(sieve_result.special_q.q);
        const int64_t r = static_cast<int64_t>(sieve_result.special_q.r);
        for (const auto& sieve_candidate : sieve_result.candidates) {
            const int64_t residue =
                (sieve_candidate.a - static_cast<int64_t>(sieve_candidate.b) * r) % q;
            require(residue == 0, "candidate does not satisfy its special-Q lattice congruence");
        }
    }
}

void require_same_relation(const Relation& actual, const Relation& expected) {
    require(actual.a == expected.a && actual.b == expected.b, "relation (a,b) differs");
    require(actual.rational_factors == expected.rational_factors,
            "relation rational factors differ");
    require(actual.algebraic_factors == expected.algebraic_factors,
            "relation algebraic factors differ");
    require(actual.rational_large_prime == expected.rational_large_prime,
            "relation rational large primes differ");
    require(actual.algebraic_large_prime == expected.algebraic_large_prime,
            "relation algebraic large primes differ");
    require(actual.extra_ab_pairs == expected.extra_ab_pairs, "relation extra (a,b) pairs differ");
}

void require_same_batches(const std::vector<std::vector<Relation>>& actual,
                          const std::vector<std::vector<Relation>>& expected) {
    require(actual.size() == expected.size(), "relation batch outer shape differs");
    for (size_t special_q_index = 0; special_q_index < expected.size(); ++special_q_index) {
        require(actual[special_q_index].size() == expected[special_q_index].size(),
                "relation batch inner size differs");
        for (size_t relation_index = 0; relation_index < expected[special_q_index].size();
             ++relation_index) {
            require_same_relation(actual[special_q_index][relation_index],
                                  expected[special_q_index][relation_index]);
        }
    }
}

[[nodiscard]] std::vector<std::vector<Relation>>
serial_oracle(const Fixture& fixture, std::span<const SieveResult> sieve_results) {
    Cofactorizer cofactorizer(fixture.ctx, fixture.fb, fixture.config);
    std::vector<std::vector<Relation>> relations(sieve_results.size());
    for (size_t special_q_index = 0; special_q_index < sieve_results.size(); ++special_q_index) {
        const auto& sieve_result = sieve_results[special_q_index];
        for (const auto& sieve_candidate : sieve_result.candidates) {
            auto relation = cofactorizer.verify(sieve_candidate, sieve_result.special_q.q,
                                                sieve_result.special_q.r);
            if (relation) {
                relations[special_q_index].push_back(std::move(*relation));
            }
        }
    }
    return relations;
}

void test_special_q_forwarding_fixture(const Fixture& fixture) {
    Cofactorizer with_special_q(fixture.ctx, fixture.fb, fixture.config);
    auto relation = with_special_q.verify(candidate(10, 1), 71, 10);
    require(relation.has_value(), "special-Q fixture candidate must verify");
    require(std::any_of(relation->algebraic_large_prime.begin(),
                        relation->algebraic_large_prime.end(),
                        [](const auto& prime_power) { return prime_power.p == 71; }),
            "verified fixture relation must retain special-Q 71");

    Cofactorizer without_special_q(fixture.ctx, fixture.fb, fixture.config);
    require(!without_special_q.verify(candidate(10, 1)).has_value(),
            "fixture must fail when the batch executor drops special-Q metadata");
}

void test_worker_and_chunk_invariance(const Fixture& fixture) {
    const auto sieve_results = make_sieve_results();
    require_valid_special_q_lattice_points(sieve_results);
    const auto expected = serial_oracle(fixture, sieve_results);
    require(expected.size() == 3 && expected[0].size() == 3 && expected[1].empty() &&
                expected[2].size() == 3,
            "serial oracle fixture acceptance pattern changed");
    require(expected[0][0].a == 10 && expected[0][1].a == -193 && expected[0][2].a == -254 &&
                expected[2][0].a == 12 && expected[2][1].a == -23 && expected[2][2].a == -11,
            "serial oracle relation order changed");

    constexpr std::array<size_t, 4> chunk_sizes{1, 2, 3, 256};
    constexpr std::array<uint32_t, 4> worker_counts{1, 2, 4, 64};
    for (const size_t chunk_size : chunk_sizes) {
        const size_t expected_chunks =
            ((6 + chunk_size - 1) / chunk_size) + ((4 + chunk_size - 1) / chunk_size);
        for (const uint32_t workers : worker_counts) {
            CandidateBatchOptions options;
            options.max_candidates_per_chunk = chunk_size;
            options.max_workers = workers;
            const CandidateBatchResult result = verify_candidate_batch(
                fixture.ctx, fixture.fb, fixture.config, sieve_results, options);

            require(result.total_candidates == 10, "candidate batch total differs");
            require(result.planned_chunks == expected_chunks,
                    "candidate batch planned chunk count differs");
            require(result.workers_used == std::min<size_t>(workers, expected_chunks),
                    "candidate batch worker clamp differs");
            require_same_batches(result.relations_by_special_q, expected);
        }
    }
}

void test_empty_shapes_and_invalid_options(const Fixture& fixture) {
    CandidateBatchOptions options;
    options.max_workers = 4;
    const CandidateBatchResult no_results = verify_candidate_batch(
        fixture.ctx, fixture.fb, fixture.config, std::span<const SieveResult>{}, options);
    require(no_results.relations_by_special_q.empty() && no_results.total_candidates == 0 &&
                no_results.planned_chunks == 0 && no_results.workers_used == 0,
            "empty candidate batch result differs");

    std::vector<SieveResult> empty_special_qs(3);
    const CandidateBatchResult empty_shape =
        verify_candidate_batch(fixture.ctx, fixture.fb, fixture.config, empty_special_qs, options);
    require(empty_shape.relations_by_special_q.size() == 3 &&
                std::all_of(empty_shape.relations_by_special_q.begin(),
                            empty_shape.relations_by_special_q.end(),
                            [](const auto& relations) { return relations.empty(); }) &&
                empty_shape.total_candidates == 0 && empty_shape.planned_chunks == 0 &&
                empty_shape.workers_used == 0,
            "all-empty special-Q shape was not preserved");

    options.max_workers = 0;
    bool zero_workers_rejected = false;
    try {
        (void)verify_candidate_batch(fixture.ctx, fixture.fb, fixture.config, empty_special_qs,
                                     options);
    } catch (const std::invalid_argument&) {
        zero_workers_rejected = true;
    }
    require(zero_workers_rejected, "zero candidate batch workers must be rejected");

    options.max_workers = 1;
    options.max_candidates_per_chunk = 0;
    bool zero_chunk_rejected = false;
    try {
        (void)verify_candidate_batch(fixture.ctx, fixture.fb, fixture.config, empty_special_qs,
                                     options);
    } catch (const std::invalid_argument&) {
        zero_chunk_rejected = true;
    }
    require(zero_chunk_rejected, "zero candidate batch chunk size must be rejected");
}

} // namespace

int main() {
    try {
        const Fixture fixture = make_fixture();
        test_special_q_forwarding_fixture(fixture);
        test_worker_and_chunk_invariance(fixture);
        test_empty_shapes_and_invalid_options(fixture);
        std::cout << "All candidate batch tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Candidate batch test failed: " << error.what() << '\n';
        return 1;
    }
}
