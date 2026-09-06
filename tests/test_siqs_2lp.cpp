// test_siqs_2lp.cpp - strict SIQS two-large-prime normalization contracts

#include <gnfs/siqs/siqs.hpp>
#include <gnfs/siqs/two_large_prime.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using gnfs::core::Integer;
using gnfs::siqs::classify_siqs_residual;
using gnfs::siqs::FBPrime;
using gnfs::siqs::nonnegative_mpz_to_uint64_checked;
using gnfs::siqs::normalize_two_large_prime;
using gnfs::siqs::sieve_polynomial;
using gnfs::siqs::SIQSLiveSieveCaptureController;
using gnfs::siqs::SIQSLiveSieveCaptureStopReason;
using gnfs::siqs::SIQSLiveSieveRelationKind;
using gnfs::siqs::SIQSPoly;
using gnfs::siqs::SIQSRelation;
using gnfs::siqs::SIQSShadowTwoLargePrimeCaptureConfig;
using gnfs::siqs::SIQSShadowTwoLargePrimeCaptureSink;
using gnfs::siqs::SIQSShadowTwoLargePrimeCaptureSnapshot;
using gnfs::siqs::split_cofactor_64;
using gnfs::siqs::TwoLargePrimeFactors;

static_assert(
    std::is_same_v<decltype(std::declval<const SIQSShadowTwoLargePrimeCaptureSink&>().snapshot()),
                   SIQSShadowTwoLargePrimeCaptureSnapshot>);
static_assert(!noexcept(SIQSShadowTwoLargePrimeCaptureSink(SIQSShadowTwoLargePrimeCaptureConfig{
    10'000, {1, 1}})));

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

void check_factors(const std::optional<TwoLargePrimeFactors>& result, uint64_t expected_p,
                   uint64_t expected_q) {
    CHECK(result.has_value());
    if (result) {
        CHECK(result->p == expected_p);
        CHECK(result->q == expected_q);
    }
}

void test_existing_splitter_accepts_exact_semiprimes() {
    check_factors(normalize_two_large_prime(7 * 13, 13, split_cofactor_64(7 * 13)), 7, 13);

    constexpr uint64_t repeated_prime = 97;
    constexpr uint64_t square = repeated_prime * repeated_prime;
    check_factors(normalize_two_large_prime(square, repeated_prime, split_cofactor_64(square)),
                  repeated_prime, repeated_prime);
}

void test_candidate_order_is_canonicalized() {
    check_factors(normalize_two_large_prime(7 * 13, 13, {13, 7}), 7, 13);
}

void test_non_semiprimes_are_rejected() {
    CHECK(!normalize_two_large_prime(0, 100, {0, 0}));
    CHECK(!normalize_two_large_prime(1, 100, {0, 0}));

    constexpr uint64_t prime = 97;
    CHECK(!normalize_two_large_prime(prime, prime, split_cofactor_64(prime)));

    // split_cofactor_64(30) returns an exact factor pair, but one side is
    // composite. A three-prime cofactor must not cross the 2LP boundary.
    CHECK(!normalize_two_large_prime(2 * 3 * 5, 15, split_cofactor_64(2 * 3 * 5)));

    // This composite is a strong pseudoprime for several small bases. The
    // project-wide deterministic uint64 primality predicate must reject it.
    constexpr uint64_t pseudoprime = UINT64_C(341550071728321);
    constexpr uint64_t pseudoprime_product = 2 * pseudoprime;
    CHECK(!normalize_two_large_prime(pseudoprime_product, pseudoprime, {2, pseudoprime}));
}

void test_bounds_and_exact_product_are_required() {
    CHECK(!normalize_two_large_prime(7 * 13, 12, {7, 13}));
    CHECK(!normalize_two_large_prime(7 * 13, 13, {7, 11}));
    CHECK(!normalize_two_large_prime(7 * 13, 13, {0, 0}));
    CHECK(!normalize_two_large_prime(7 * 13, 91, {1, 91}));
    CHECK(!normalize_two_large_prime(4 * 9, 9, {4, 9}));

    // A forged pair whose multiplication would wrap must still fail closed.
    CHECK(!normalize_two_large_prime(15, std::numeric_limits<uint64_t>::max(),
                                     {std::numeric_limits<uint64_t>::max(), 15}));
}

void test_residual_classification_is_exact_and_deterministic() {
    CHECK(!classify_siqs_residual(0, 100, 10'000).has_value());
    CHECK(!classify_siqs_residual(1, 100, 10'000).has_value());

    const auto one_lp = classify_siqs_residual(97, 100, 10'000);
    CHECK(one_lp.has_value());
    if (one_lp) {
        CHECK(one_lp->kind == SIQSLiveSieveRelationKind::one_lp);
        CHECK(one_lp->large_prime == 97);
        CHECK(one_lp->large_prime2 == 0);
    }

    CHECK(!classify_siqs_residual(7 * 13, 100, 0).has_value());
    const auto two_lp = classify_siqs_residual(7 * 13, 100, 10'000);
    CHECK(two_lp.has_value());
    if (two_lp) {
        CHECK(two_lp->kind == SIQSLiveSieveRelationKind::two_lp_candidate);
        CHECK(two_lp->large_prime == 7 * 13);
        CHECK(two_lp->large_prime2 == 1);
    }

    CHECK(!classify_siqs_residual(101, 100, 10'000).has_value());
    CHECK(!classify_siqs_residual(101 * 103, 100, 10'000).has_value());

    // This strong base-2 pseudoprime was missed by the legacy capture-only
    // probable-prime filter. Deterministic uint64 primality retains it as a
    // raw composite candidate; strict split normalization rejects it later if
    // it is not exactly a product of two bounded primes.
    constexpr uint64_t pseudoprime = UINT64_C(341550071728321);
    const auto pseudoprime_candidate = classify_siqs_residual(pseudoprime, 100, pseudoprime);
    CHECK(pseudoprime_candidate.has_value());
    if (pseudoprime_candidate) {
        CHECK(pseudoprime_candidate->kind == SIQSLiveSieveRelationKind::two_lp_candidate);
    }
}

void test_nonnegative_mpz_to_uint64_checked() {
    const Integer negative("-1");
    CHECK(!nonnegative_mpz_to_uint64_checked(negative.get_mpz()).has_value());

    const Integer zero(0);
    const auto zero_value = nonnegative_mpz_to_uint64_checked(zero.get_mpz());
    CHECK(zero_value.has_value());
    CHECK(zero_value.value_or(1) == 0);

    constexpr uint64_t above_u32_expected = UINT64_C(4294967311);
    const Integer above_u32("4294967311");
    const auto above_u32_value = nonnegative_mpz_to_uint64_checked(above_u32.get_mpz());
    CHECK(above_u32_value.has_value());
    CHECK(above_u32_value.value_or(0) == above_u32_expected);

    const Integer uint64_max("18446744073709551615");
    const auto uint64_max_value = nonnegative_mpz_to_uint64_checked(uint64_max.get_mpz());
    CHECK(uint64_max_value.has_value());
    CHECK(uint64_max_value.value_or(0) == std::numeric_limits<uint64_t>::max());

    const Integer above_uint64("18446744073709551616");
    CHECK(!nonnegative_mpz_to_uint64_checked(above_uint64.get_mpz()).has_value());
}

std::vector<SIQSRelation> collect_gmp_fallback_relations(
    const char* cofactor_text, SIQSLiveSieveCaptureController* live_capture = nullptr,
    uint64_t two_large_prime_bound = std::numeric_limits<uint64_t>::max(),
    SIQSShadowTwoLargePrimeCaptureSink* shadow_two_lp_capture = nullptr) {
    const Integer cofactor(cofactor_text);
    Integer q_value;
    mpz_mul_2exp(q_value.get_mpz(), cofactor.get_mpz(), 100);

    // With A=1 and B=q_value, the x=0 candidate has
    // Q(x)=B^2-N=q_value. Its >127-bit size forces the GMP fallback even
    // on platforms with native __int128 support. The x=-1 candidate leaves
    // the >64-bit residual q_value-1 and is rejected.
    Integer modulus;
    mpz_mul(modulus.get_mpz(), q_value.get_mpz(), q_value.get_mpz());
    mpz_sub(modulus.get_mpz(), modulus.get_mpz(), q_value.get_mpz());

    SIQSPoly poly;
    mpz_set_ui(poly.A.get_mpz(), 1);
    mpz_set(poly.B.get_mpz(), q_value.get_mpz());
    const uint32_t no_solution = std::numeric_limits<uint32_t>::max();
    poly.solns.assign(2, {no_solution, no_solution});

    const std::vector<FBPrime> factor_base = {
        {0, 0, 0},
        {2, 1, 1},
    };
    std::vector<SIQSRelation> relations;
    std::mutex relations_mutex;
    std::vector<uint8_t> sieve_buffer;
    std::vector<uint8_t> exponent_buffer(factor_base.size(), 0);

    sieve_polynomial(poly, modulus, factor_base, 1, 0, 3, std::numeric_limits<uint32_t>::max(),
                     two_large_prime_bound, relations, relations_mutex, sieve_buffer,
                     exponent_buffer, live_capture, shadow_two_lp_capture);
    return relations;
}

std::vector<SIQSRelation> collect_native_relations(
    const char* cofactor_text, SIQSLiveSieveCaptureController* live_capture = nullptr,
    uint64_t two_large_prime_bound = 10'000, std::vector<uint8_t>* reusable_exponents = nullptr,
    SIQSShadowTwoLargePrimeCaptureSink* shadow_two_lp_capture = nullptr) {
    const Integer cofactor(cofactor_text);
    Integer q_value;
    mpz_mul_2exp(q_value.get_mpz(), cofactor.get_mpz(), 20);

    Integer modulus;
    mpz_mul(modulus.get_mpz(), q_value.get_mpz(), q_value.get_mpz());
    mpz_sub(modulus.get_mpz(), modulus.get_mpz(), q_value.get_mpz());

    SIQSPoly poly;
    mpz_set_ui(poly.A.get_mpz(), 1);
    mpz_set(poly.B.get_mpz(), q_value.get_mpz());
    const uint32_t no_solution = std::numeric_limits<uint32_t>::max();
    poly.solns.assign(2, {no_solution, no_solution});

    const std::vector<FBPrime> factor_base = {
        {0, 0, 0},
        {2, 1, 1},
    };
    std::vector<SIQSRelation> relations;
    std::mutex relations_mutex;
    std::vector<uint8_t> sieve_buffer;
    std::vector<uint8_t> local_exponents(factor_base.size(), 0);
    std::vector<uint8_t>& exponent_buffer =
        reusable_exponents != nullptr ? *reusable_exponents : local_exponents;
    if (exponent_buffer.size() != factor_base.size()) {
        exponent_buffer.assign(factor_base.size(), 0);
    }

    sieve_polynomial(poly, modulus, factor_base, 1, 0, 3, 100, two_large_prime_bound, relations,
                     relations_mutex, sieve_buffer, exponent_buffer, live_capture,
                     shadow_two_lp_capture);
    return relations;
}

std::vector<SIQSRelation>
collect_native_relations_with_seven(const char* cofactor_text,
                                    SIQSShadowTwoLargePrimeCaptureSink* shadow_two_lp_capture) {
    const Integer cofactor(cofactor_text);
    Integer q_value;
    mpz_mul_2exp(q_value.get_mpz(), cofactor.get_mpz(), 20);

    Integer modulus;
    mpz_mul(modulus.get_mpz(), q_value.get_mpz(), q_value.get_mpz());
    mpz_sub(modulus.get_mpz(), modulus.get_mpz(), q_value.get_mpz());

    SIQSPoly poly;
    mpz_set_ui(poly.A.get_mpz(), 1);
    mpz_set(poly.B.get_mpz(), q_value.get_mpz());
    const uint32_t no_solution = std::numeric_limits<uint32_t>::max();
    poly.solns.assign(3, {no_solution, no_solution});

    // The extra factor-base prime removes 7 from 10402 = 2 * 7 * 743,
    // leaving the prime residual 743. The adjacent 10403 = 101 * 103
    // remains an unresolved composite candidate.
    const std::vector<FBPrime> factor_base = {
        {0, 0, 0},
        {2, 1, 1},
        {7, 0, 3},
    };
    std::vector<SIQSRelation> relations;
    std::mutex relations_mutex;
    std::vector<uint8_t> sieve_buffer;
    std::vector<uint8_t> exponent_buffer(factor_base.size(), 0);

    sieve_polynomial(poly, modulus, factor_base, 1, 0, 3, 100, 0, relations, relations_mutex,
                     sieve_buffer, exponent_buffer, nullptr, shadow_two_lp_capture);
    return relations;
}

[[nodiscard]] bool same_relation(const SIQSRelation& lhs, const SIQSRelation& rhs) {
    return lhs.value == rhs.value && lhs.negative == rhs.negative &&
           lhs.large_prime == rhs.large_prime && lhs.large_prime2 == rhs.large_prime2 &&
           lhs.exponents == rhs.exponents && lhs.fb_indices == rhs.fb_indices &&
           lhs.merge_lps == rhs.merge_lps;
}

[[nodiscard]] SIQSRelation make_raw_two_lp_relation(uint64_t cofactor = 91) {
    SIQSRelation relation;
    relation.value = Integer(1);
    relation.large_prime = cofactor;
    relation.large_prime2 = 1;
    relation.negative = false;
    return relation;
}

void test_shadow_sink_factory_exceptions_roll_back_for_retry() {
    const gnfs::siqs::SIQSLiveSieveRelationPayloadShape payload{1, 0, 0, 0};
    SIQSShadowTwoLargePrimeCaptureSink factory_failure(
        SIQSShadowTwoLargePrimeCaptureConfig{10'000, {2, 2}});

    bool runtime_error_seen = false;
    try {
        (void)factory_failure.try_capture(91, payload, []() -> SIQSRelation {
            throw std::runtime_error("injected shadow relation factory failure");
        });
    } catch (const std::runtime_error&) {
        runtime_error_seen = true;
    }
    CHECK(runtime_error_seen);
    CHECK(!factory_failure.stopped());
    CHECK(factory_failure.relations().empty());
    CHECK(factory_failure.snapshot().captured_relations == 0);
    CHECK(factory_failure.snapshot().captured_payload_bytes == 0);
    CHECK(factory_failure.try_capture(91, payload, [] { return make_raw_two_lp_relation(); }));
    CHECK(factory_failure.relations().size() == 1);
    CHECK(factory_failure.snapshot().captured_relations == 1);

    SIQSShadowTwoLargePrimeCaptureSink factory_bad_alloc(
        SIQSShadowTwoLargePrimeCaptureConfig{10'000, {1, 1}});
    bool factory_bad_alloc_seen = false;
    try {
        (void)factory_bad_alloc.try_capture(91, payload,
                                            []() -> SIQSRelation { throw std::bad_alloc(); });
    } catch (const std::bad_alloc&) {
        factory_bad_alloc_seen = true;
    }
    CHECK(factory_bad_alloc_seen);
    CHECK(!factory_bad_alloc.stopped());
    CHECK(factory_bad_alloc.relations().empty());
    CHECK(factory_bad_alloc.snapshot().captured_relations == 0);
    CHECK(factory_bad_alloc.try_capture(91, payload, [] { return make_raw_two_lp_relation(); }));
    CHECK(factory_bad_alloc.relations().size() == 1);
    CHECK(factory_bad_alloc.stop_reason() == SIQSLiveSieveCaptureStopReason::relation_limit);
}

void test_shadow_sink_rejects_malformed_factory_output_and_can_retry() {
    const gnfs::siqs::SIQSLiveSieveRelationPayloadShape payload{1, 0, 0, 0};
    SIQSShadowTwoLargePrimeCaptureSink sink(SIQSShadowTwoLargePrimeCaptureConfig{100, {8, 100}});

    const auto expect_logic_error = [&](auto&& factory) {
        bool logic_error_seen = false;
        try {
            (void)sink.try_capture(91, payload, std::forward<decltype(factory)>(factory));
        } catch (const std::logic_error&) {
            logic_error_seen = true;
        }
        CHECK(logic_error_seen);
        CHECK(!sink.stopped());
        CHECK(sink.relations().empty());
        CHECK(sink.snapshot().captured_relations == 0);
        CHECK(sink.snapshot().captured_payload_bytes == 0);
    };

    expect_logic_error([] {
        SIQSRelation relation = make_raw_two_lp_relation(1);
        return relation;
    });
    expect_logic_error([] { return make_raw_two_lp_relation(101); });
    expect_logic_error([] { return make_raw_two_lp_relation(77); });
    expect_logic_error([] {
        SIQSRelation relation = make_raw_two_lp_relation();
        relation.large_prime2 = 0;
        return relation;
    });
    expect_logic_error([] {
        SIQSRelation relation = make_raw_two_lp_relation();
        relation.merge_lps.push_back(7);
        return relation;
    });
    expect_logic_error([] {
        SIQSRelation relation = make_raw_two_lp_relation();
        relation.exponents.push_back(1);
        return relation;
    });

    gnfs::siqs::SIQSLiveSieveRelationPayloadShape aliased_payload = payload;
    bool alias_mutation_rejected = false;
    try {
        (void)sink.try_capture(91, aliased_payload, [&aliased_payload] {
            aliased_payload.factor_base_exponent_count = 1;
            SIQSRelation relation = make_raw_two_lp_relation();
            relation.exponents.push_back(1);
            return relation;
        });
    } catch (const std::logic_error&) {
        alias_mutation_rejected = true;
    }
    CHECK(alias_mutation_rejected);
    CHECK(aliased_payload.factor_base_exponent_count == 1);
    CHECK(!sink.stopped());
    CHECK(sink.relations().empty());
    CHECK(sink.snapshot().captured_relations == 0);
    CHECK(sink.snapshot().captured_payload_bytes == 0);

    CHECK(sink.try_capture(91, payload, [] { return make_raw_two_lp_relation(); }));
    CHECK(sink.relations().size() == 1);
    CHECK(sink.snapshot().captured_relations == 1);
    CHECK(sink.snapshot().observed_two_lp_candidates == 8);
}

void test_shadow_sink_validates_config_before_reserving() {
    SIQSShadowTwoLargePrimeCaptureSink too_small_bound(
        SIQSShadowTwoLargePrimeCaptureConfig{3, {std::numeric_limits<std::size_t>::max(), 1}});
    CHECK(too_small_bound.stopped());
    CHECK(too_small_bound.stop_reason() == SIQSLiveSieveCaptureStopReason::invalid_limits);

    SIQSShadowTwoLargePrimeCaptureSink zero_relation_cap(
        SIQSShadowTwoLargePrimeCaptureConfig{4, {0, 1}});
    CHECK(zero_relation_cap.stopped());
    CHECK(zero_relation_cap.stop_reason() == SIQSLiveSieveCaptureStopReason::invalid_limits);

    SIQSShadowTwoLargePrimeCaptureSink zero_payload_cap(
        SIQSShadowTwoLargePrimeCaptureConfig{4, {std::numeric_limits<std::size_t>::max(), 0}});
    CHECK(zero_payload_cap.stopped());
    CHECK(zero_payload_cap.stop_reason() == SIQSLiveSieveCaptureStopReason::invalid_limits);

    bool length_error_seen = false;
    try {
        SIQSShadowTwoLargePrimeCaptureSink impossible_reserve(
            SIQSShadowTwoLargePrimeCaptureConfig{4, {std::numeric_limits<std::size_t>::max(), 1}});
        (void)impossible_reserve;
    } catch (const std::length_error&) {
        length_error_seen = true;
    }
    CHECK(length_error_seen);
}

void test_shadow_sink_exact_caps_are_terminal_only_for_the_sink() {
    const gnfs::siqs::SIQSLiveSieveRelationPayloadShape payload{1, 0, 0, 0};

    SIQSShadowTwoLargePrimeCaptureSink relation_limited(
        SIQSShadowTwoLargePrimeCaptureConfig{10'000, {1, std::numeric_limits<std::size_t>::max()}});
    CHECK(relation_limited.try_capture(91, payload, [] { return make_raw_two_lp_relation(); }));
    CHECK(relation_limited.stop_reason() == SIQSLiveSieveCaptureStopReason::relation_limit);
    CHECK(relation_limited.relations().size() == 1);
    bool stopped_factory_called = false;
    CHECK(!relation_limited.try_capture(91, payload, [&] {
        stopped_factory_called = true;
        return make_raw_two_lp_relation();
    }));
    CHECK(!stopped_factory_called);
    CHECK(relation_limited.relations().size() == 1);

    SIQSShadowTwoLargePrimeCaptureSink payload_limited(
        SIQSShadowTwoLargePrimeCaptureConfig{10'000, {2, 1}});
    CHECK(payload_limited.try_capture(91, payload, [] { return make_raw_two_lp_relation(); }));
    CHECK(payload_limited.stop_reason() == SIQSLiveSieveCaptureStopReason::payload_limit);
    CHECK(payload_limited.relations().size() == 1);
    CHECK(payload_limited.snapshot().captured_payload_bytes == 1);

    SIQSShadowTwoLargePrimeCaptureSink payload_rejected(
        SIQSShadowTwoLargePrimeCaptureConfig{10'000, {2, 1}});
    bool overcap_factory_called = false;
    CHECK(!payload_rejected.try_capture(91, {2, 0, 0, 0}, [&] {
        overcap_factory_called = true;
        return make_raw_two_lp_relation();
    }));
    CHECK(!overcap_factory_called);
    CHECK(payload_rejected.stop_reason() == SIQSLiveSieveCaptureStopReason::payload_limit);
    CHECK(payload_rejected.relations().empty());

    SIQSShadowTwoLargePrimeCaptureSink disabled(SIQSShadowTwoLargePrimeCaptureConfig{0, {1, 1}});
    CHECK(disabled.stopped());
    CHECK(disabled.stop_reason() == SIQSLiveSieveCaptureStopReason::invalid_limits);
}

void test_llp64_gmp_fallback_classification() {
    // These values sit just above UINT32_MAX. On Windows LLP64, converting
    // either through unsigned long truncates it to the parenthesized value.
    // Prime 4294967311 (15) must be rejected as a 2LP composite candidate.
    CHECK(collect_gmp_fallback_relations("4294967311").empty());

    // Composite 4294967299 = 7 * 613566757 (3) must be retained for exact
    // 2LP normalization rather than misclassified from its low 32 bits.
    const auto relations = collect_gmp_fallback_relations("4294967299");
    CHECK(relations.size() == 1);
    for (const SIQSRelation& relation : relations) {
        CHECK(relation.large_prime == UINT64_C(4294967299));
        CHECK(relation.large_prime2 == 1);
        CHECK(relation.exponents.size() == 2);
        if (relation.exponents.size() == 2) {
            CHECK(relation.exponents[1] == 100);
        }
    }
}

void test_gmp_capture_is_bounded_before_dense_relation_allocation() {
    constexpr const char* composite = "4294967299";
    const auto baseline = collect_gmp_fallback_relations(composite);
    CHECK(baseline.size() == 1);

    SIQSLiveSieveCaptureController roomy_capture({10, std::numeric_limits<std::size_t>::max()});
    const auto captured = collect_gmp_fallback_relations(composite, &roomy_capture);
    CHECK(captured.size() == baseline.size());
    if (captured.size() == baseline.size() && !captured.empty()) {
        CHECK(same_relation(captured.front(), baseline.front()));
    }
    const auto& roomy = roomy_capture.snapshot();
    CHECK(!roomy_capture.stopped());
    CHECK(roomy.captured_relations == captured.size());
    CHECK(roomy.observed_two_lp_candidates == 1);
    CHECK(roomy.threshold_candidates == roomy.unrepresentable_residuals + roomy.rejected_residuals +
                                            roomy.observed_full_relations +
                                            roomy.observed_one_lp_relations +
                                            roomy.observed_two_lp_candidates);

    SIQSLiveSieveCaptureController relation_limited({1, std::numeric_limits<std::size_t>::max()});
    const auto final_relation = collect_gmp_fallback_relations(composite, &relation_limited);
    CHECK(final_relation.size() == 1);
    CHECK(relation_limited.stop_reason() == SIQSLiveSieveCaptureStopReason::relation_limit);
    CHECK(relation_limited.snapshot().captured_relations == 1);

    SIQSLiveSieveCaptureController payload_limited({10, 1});
    const auto rejected_before_allocation =
        collect_gmp_fallback_relations(composite, &payload_limited);
    CHECK(rejected_before_allocation.empty());
    CHECK(payload_limited.stop_reason() == SIQSLiveSieveCaptureStopReason::payload_limit);
    CHECK(payload_limited.snapshot().observed_two_lp_candidates == 1);
    CHECK(payload_limited.snapshot().captured_relations == 0);
    CHECK(payload_limited.snapshot().captured_payload_bytes == 0);
}

void test_capture_observation_preserves_one_lp_only_output() {
    for (const char* residual : {"1", "97", "4294967299"}) {
        const auto baseline = collect_gmp_fallback_relations(residual, nullptr, 0);
        SIQSLiveSieveCaptureController observer({10, std::numeric_limits<std::size_t>::max()});
        const auto observed = collect_gmp_fallback_relations(residual, &observer, 0);

        CHECK(observed.size() == baseline.size());
        if (observed.size() == baseline.size()) {
            for (std::size_t i = 0; i < observed.size(); ++i) {
                CHECK(same_relation(observed[i], baseline[i]));
            }
        }
    }
}

void test_native_capture_matches_null_path_and_cleans_reusable_exponents() {
#if defined(__SIZEOF_INT128__)
    for (const char* residual : {"1", "97", "91"}) {
        const auto baseline = collect_native_relations(residual);
        SIQSLiveSieveCaptureController observer({10, std::numeric_limits<std::size_t>::max()});
        const auto observed = collect_native_relations(residual, &observer);
        CHECK(observed.size() == baseline.size());
        if (observed.size() == baseline.size()) {
            for (std::size_t i = 0; i < observed.size(); ++i) {
                CHECK(same_relation(observed[i], baseline[i]));
            }
        }
    }

    std::vector<uint8_t> exponent_buffer(2, 0);
    SIQSLiveSieveCaptureController exact_limit({1, std::numeric_limits<std::size_t>::max()});
    const auto first = collect_native_relations("91", &exact_limit, 10'000, &exponent_buffer);
    CHECK(first.size() == 1);
    CHECK(exact_limit.stop_reason() == SIQSLiveSieveCaptureStopReason::relation_limit);
    CHECK(exponent_buffer == std::vector<uint8_t>({0, 0}));

    SIQSLiveSieveCaptureController replay({1, std::numeric_limits<std::size_t>::max()});
    const auto second = collect_native_relations("91", &replay, 10'000, &exponent_buffer);
    CHECK(second.size() == 1);
    if (!second.empty()) {
        CHECK(second.front().exponents.size() == 2);
        if (second.front().exponents.size() == 2) {
            CHECK(second.front().exponents[1] == 20);
        }
    }
    CHECK(exponent_buffer == std::vector<uint8_t>({0, 0}));

    SIQSLiveSieveCaptureController payload_limit({10, 1});
    CHECK(collect_native_relations("91", &payload_limit, 10'000, &exponent_buffer).empty());
    CHECK(payload_limit.stop_reason() == SIQSLiveSieveCaptureStopReason::payload_limit);
    CHECK(exponent_buffer == std::vector<uint8_t>({0, 0}));

    SIQSLiveSieveCaptureController unrepresentable({10, std::numeric_limits<std::size_t>::max()});
    const auto too_wide = collect_native_relations("18446744073709551617", &unrepresentable,
                                                   std::numeric_limits<uint64_t>::max());
    CHECK(too_wide.empty());
    CHECK(unrepresentable.snapshot().unrepresentable_residuals > 0);
    CHECK(unrepresentable.snapshot().captured_relations == 0);
#endif
}

void test_shadow_capture_is_supplemental_on_gmp_fallback() {
    SIQSShadowTwoLargePrimeCaptureSink shadow(SIQSShadowTwoLargePrimeCaptureConfig{
        10'000, {10, std::numeric_limits<std::size_t>::max()}});

    for (const char* residual : {"1", "97", "91", "10403"}) {
        const auto baseline = collect_gmp_fallback_relations(residual, nullptr, 0);
        const auto observed = collect_gmp_fallback_relations(residual, nullptr, 0, &shadow);
        CHECK(observed.size() == baseline.size());
        if (observed.size() == baseline.size()) {
            for (std::size_t i = 0; i < observed.size(); ++i) {
                CHECK(same_relation(observed[i], baseline[i]));
            }
        }
    }

    CHECK(!shadow.stopped());
    CHECK(shadow.relations().size() == 1);
    if (shadow.relations().size() == 1) {
        const SIQSRelation& captured = shadow.relations().front();
        CHECK(captured.large_prime == 91);
        CHECK(captured.large_prime2 == 1);
        CHECK(captured.exponents.size() == 2);
        if (captured.exponents.size() == 2) {
            CHECK(captured.exponents[1] == 100);
        }
    }
    CHECK(shadow.snapshot().observed_two_lp_candidates == 1);
    CHECK(shadow.snapshot().captured_relations == 1);
}

void test_gmp_shadow_capture_preserves_exact_llp64_cofactor() {
    constexpr uint64_t cofactor = UINT64_C(4294967299);
    SIQSShadowTwoLargePrimeCaptureSink shadow(SIQSShadowTwoLargePrimeCaptureConfig{
        cofactor, {1, std::numeric_limits<std::size_t>::max()}});

    const auto production = collect_gmp_fallback_relations("4294967299", nullptr, 0, &shadow);
    CHECK(production.empty());
    CHECK(shadow.relations().size() == 1);
    CHECK(shadow.stop_reason() == SIQSLiveSieveCaptureStopReason::relation_limit);
    if (shadow.relations().size() == 1) {
        const SIQSRelation& captured = shadow.relations().front();
        CHECK(captured.large_prime == cofactor);
        CHECK(captured.large_prime2 == 1);
        CHECK(captured.exponents.size() == 2);
        if (captured.exponents.size() == 2) {
            CHECK(captured.exponents[1] == 100);
        }
    }
}

void test_legacy_admission_wins_without_shadow_duplication() {
    SIQSShadowTwoLargePrimeCaptureSink shadow(SIQSShadowTwoLargePrimeCaptureConfig{
        10'000, {10, std::numeric_limits<std::size_t>::max()}});
    const auto legacy = collect_gmp_fallback_relations("91", nullptr, 10'000, &shadow);
    CHECK(legacy.size() == 1);
    if (legacy.size() == 1) {
        CHECK(legacy.front().large_prime == 91);
        CHECK(legacy.front().large_prime2 == 1);
    }
    CHECK(shadow.relations().empty());
    CHECK(shadow.snapshot().observed_two_lp_candidates == 0);
}

void test_stopped_shadow_sink_does_not_stop_later_legacy_admission() {
    SIQSShadowTwoLargePrimeCaptureSink shadow(
        SIQSShadowTwoLargePrimeCaptureConfig{10'000, {1, std::numeric_limits<std::size_t>::max()}});
    CHECK(collect_gmp_fallback_relations("91", nullptr, 0, &shadow).empty());
    CHECK(shadow.stop_reason() == SIQSLiveSieveCaptureStopReason::relation_limit);
    CHECK(shadow.relations().size() == 1);

    const auto later_one_lp = collect_gmp_fallback_relations("97", nullptr, 0, &shadow);
    CHECK(later_one_lp.size() == 1);
    if (later_one_lp.size() == 1) {
        CHECK(later_one_lp.front().large_prime == 97);
        CHECK(later_one_lp.front().large_prime2 == 0);
    }
    CHECK(shadow.relations().size() == 1);
}

void test_native_shadow_capture_cleans_reusable_exponents() {
#if defined(__SIZEOF_INT128__)
    SIQSShadowTwoLargePrimeCaptureSink roomy_shadow(SIQSShadowTwoLargePrimeCaptureConfig{
        10'000, {10, std::numeric_limits<std::size_t>::max()}});
    for (const char* residual : {"1", "97", "101", "91", "10403"}) {
        const auto baseline = collect_native_relations(residual, nullptr, 0);
        const auto observed =
            collect_native_relations(residual, nullptr, 0, nullptr, &roomy_shadow);
        CHECK(observed.size() == baseline.size());
        if (observed.size() == baseline.size()) {
            for (std::size_t i = 0; i < observed.size(); ++i) {
                CHECK(same_relation(observed[i], baseline[i]));
            }
        }
    }
    CHECK(roomy_shadow.relations().size() == 1);
    if (roomy_shadow.relations().size() == 1) {
        CHECK(roomy_shadow.relations().front().large_prime == 91);
        CHECK(roomy_shadow.relations().front().large_prime2 == 1);
    }

    std::vector<uint8_t> exponent_buffer(2, 0);
    SIQSShadowTwoLargePrimeCaptureSink shadow(
        SIQSShadowTwoLargePrimeCaptureConfig{10'000, {1, std::numeric_limits<std::size_t>::max()}});

    const auto production = collect_native_relations("91", nullptr, 0, &exponent_buffer, &shadow);
    CHECK(production.empty());
    CHECK(shadow.relations().size() == 1);
    CHECK(shadow.stop_reason() == SIQSLiveSieveCaptureStopReason::relation_limit);
    CHECK(exponent_buffer == std::vector<uint8_t>({0, 0}));
    if (shadow.relations().size() == 1) {
        const SIQSRelation& captured = shadow.relations().front();
        CHECK(captured.large_prime == 91);
        CHECK(captured.large_prime2 == 1);
        CHECK(captured.exponents.size() == 2);
        if (captured.exponents.size() == 2) {
            CHECK(captured.exponents[1] == 20);
        }
    }

    const auto later_one_lp = collect_native_relations("97", nullptr, 0, &exponent_buffer, &shadow);
    CHECK(later_one_lp.size() == 1);
    CHECK(exponent_buffer == std::vector<uint8_t>({0, 0}));
#endif
}

void test_native_shadow_capture_honors_exact_cofactor_bound() {
#if defined(__SIZEOF_INT128__)
    SIQSShadowTwoLargePrimeCaptureSink below_exact(
        SIQSShadowTwoLargePrimeCaptureConfig{10'402, {1, std::numeric_limits<std::size_t>::max()}});
    CHECK(collect_native_relations_with_seven("10403", &below_exact).empty());
    CHECK(below_exact.relations().empty());
    CHECK(!below_exact.stopped());

    SIQSShadowTwoLargePrimeCaptureSink shadow(
        SIQSShadowTwoLargePrimeCaptureConfig{10'403, {2, std::numeric_limits<std::size_t>::max()}});

    CHECK(collect_native_relations_with_seven("10402", &shadow).empty());
    CHECK(shadow.relations().empty());
    CHECK(collect_native_relations_with_seven("10403", &shadow).empty());
    CHECK(shadow.relations().size() == 1);
    if (shadow.relations().size() == 1) {
        CHECK(shadow.relations().front().large_prime == 10'403);
        CHECK(shadow.relations().front().large_prime2 == 1);
    }
#endif
}

void test_shadow_limit_does_not_stop_same_polynomial_legacy_relation() {
    const Integer modulus(1934);
    SIQSPoly poly;
    poly.A = Integer(1);
    poly.B = Integer(-44);
    const uint32_t no_solution = std::numeric_limits<uint32_t>::max();
    poly.solns.assign(2, {no_solution, no_solution});

    const std::vector<FBPrime> factor_base = {
        {0, 0, 0},
        {2, 1, 1},
    };
    std::vector<SIQSRelation> production;
    std::mutex production_mutex;
    std::vector<uint8_t> sieve_buffer;
    std::vector<uint8_t> exponent_buffer(factor_base.size(), 0);
    SIQSShadowTwoLargePrimeCaptureSink shadow(
        SIQSShadowTwoLargePrimeCaptureConfig{91, {1, std::numeric_limits<std::size_t>::max()}});

    // x=-1 yields Q(x)=91 and fills the shadow sink. x=0 then yields
    // Q(x)=2, which the factor base reduces to a legacy full relation.
    sieve_polynomial(poly, modulus, factor_base, 1, 0, 3, 2, 0, production, production_mutex,
                     sieve_buffer, exponent_buffer, nullptr, &shadow);

    CHECK(shadow.stop_reason() == SIQSLiveSieveCaptureStopReason::relation_limit);
    CHECK(shadow.relations().size() == 1);
    if (shadow.relations().size() == 1) {
        CHECK(shadow.relations().front().large_prime == 91);
        CHECK(shadow.relations().front().large_prime2 == 1);
    }
    CHECK(production.size() == 1);
    if (production.size() == 1) {
        CHECK(production.front().value == Integer(-44));
        CHECK(production.front().large_prime == 0);
        CHECK(production.front().large_prime2 == 0);
        CHECK(production.front().exponents.size() == 2);
        if (production.front().exponents.size() == 2) {
            CHECK(production.front().exponents[1] == 1);
        }
    }
    CHECK(exponent_buffer == std::vector<uint8_t>({0, 0}));
}

void test_sieve_score_saturates_without_losing_smooth_relation() {
    constexpr uint32_t sieve_half = 1;
    constexpr uint8_t threshold = 100;
    const std::vector<uint32_t> primes = {
        2,   3,   5,   7,   11,  13,  17,  19,  23,  29,  31,  37,  41,  43,  47,  53,  59,
        61,  67,  71,  73,  79,  83,  89,  97,  101, 103, 107, 109, 113, 127, 131, 137, 139,
        149, 151, 157, 163, 167, 173, 179, 181, 191, 193, 197, 199, 211, 223, 227, 229, 233,
        239, 241, 251, 257, 263, 269, 271, 277, 281, 283, 293, 307, 311, 313, 317, 331, 337,
        347, 349, 353, 359, 367, 373, 379, 383, 389, 397, 401, 409, 419};

    Integer base;
    mpz_ui_pow_ui(base.get_mpz(), 10, 100);
    Integer factor_base_product(1);
    for (uint32_t prime : primes) {
        factor_base_product *= static_cast<int64_t>(prime);
    }
    const Integer modulus = base * base - factor_base_product;

    SIQSPoly poly;
    poly.A = Integer(1);
    poly.B = base;
    poly.solns.assign(primes.size() + 1, {sieve_half, sieve_half});

    std::vector<FBPrime> factor_base;
    factor_base.reserve(primes.size() + 1);
    factor_base.push_back({0, 0, 0});
    for (uint32_t prime : primes) {
        uint8_t logp = 0;
        for (uint32_t value = prime; value > 1; value >>= 1) {
            ++logp;
        }
        factor_base.push_back(
            {prime, static_cast<uint32_t>(mpz_fdiv_ui(base.get_mpz(), prime)), logp});
    }

    std::vector<SIQSRelation> relations;
    std::mutex relations_mutex;
    std::vector<uint8_t> sieve_buffer;
    std::vector<uint8_t> exponent_buffer(factor_base.size());
    sieve_polynomial(poly, modulus, factor_base, sieve_half, threshold, 0,
                     std::numeric_limits<uint64_t>::max(), 0, relations, relations_mutex,
                     sieve_buffer, exponent_buffer);

    uint32_t expected_score = 0;
    for (size_t index = 1; index < factor_base.size(); ++index) {
        expected_score += factor_base[index].logp;
    }

    CHECK(expected_score > std::numeric_limits<uint8_t>::max());
    CHECK(sieve_buffer[sieve_half] == std::numeric_limits<uint8_t>::max());
    CHECK(relations.size() == 1);
    if (relations.size() == 1) {
        CHECK(relations.front().value == base);
        CHECK(relations.front().large_prime == 0);
        CHECK(relations.front().large_prime2 == 0);
        CHECK(relations.front().exponents.size() == factor_base.size());
        if (relations.front().exponents.size() == factor_base.size()) {
            for (size_t index = 1; index < factor_base.size(); ++index) {
                CHECK(relations.front().exponents[index] == 1);
            }
        }
    }
}

} // namespace

int main() {
    test_existing_splitter_accepts_exact_semiprimes();
    test_candidate_order_is_canonicalized();
    test_non_semiprimes_are_rejected();
    test_bounds_and_exact_product_are_required();
    test_residual_classification_is_exact_and_deterministic();
    test_nonnegative_mpz_to_uint64_checked();
    test_shadow_sink_factory_exceptions_roll_back_for_retry();
    test_shadow_sink_rejects_malformed_factory_output_and_can_retry();
    test_shadow_sink_validates_config_before_reserving();
    test_shadow_sink_exact_caps_are_terminal_only_for_the_sink();
    test_llp64_gmp_fallback_classification();
    test_gmp_capture_is_bounded_before_dense_relation_allocation();
    test_capture_observation_preserves_one_lp_only_output();
    test_native_capture_matches_null_path_and_cleans_reusable_exponents();
    test_shadow_capture_is_supplemental_on_gmp_fallback();
    test_gmp_shadow_capture_preserves_exact_llp64_cofactor();
    test_legacy_admission_wins_without_shadow_duplication();
    test_stopped_shadow_sink_does_not_stop_later_legacy_admission();
    test_native_shadow_capture_cleans_reusable_exponents();
    test_native_shadow_capture_honors_exact_cofactor_bound();
    test_shadow_limit_does_not_stop_same_polynomial_legacy_relation();
    test_sieve_score_saturates_without_losing_smooth_relation();

    std::cout << checks_passed << " checks passed, " << checks_failed << " checks failed\n";
    return checks_failed == 0 ? 0 : 1;
}
