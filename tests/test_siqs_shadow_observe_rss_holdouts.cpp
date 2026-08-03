// Outcome-blind contract for the sealed SIQS shadow-observe RSS holdouts.
// This test deliberately does not include or call production SIQS or its probe.

#include "shadow_proof_rss_holdout_fixture_internal.hpp"

#include <gnfs/core/integer.hpp>

#include <gmp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

using std::size_t;
using std::uint32_t;
using std::uint64_t;

using gnfs::core::Integer;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::
    SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_CORPUS_ID;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::
    siqs_shadow_observe_rss_holdout_v1_digest;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::
    SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_DOMAIN;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::
    SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_HIGH;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::
    SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_LOW;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::
    SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_FIXTURE_COUNT;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::
    SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_MAX_SECONDS;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::
    SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_P_BASE;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::
    SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_P_STRIDE;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::
    SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_Q_BASE;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::
    SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_Q_STRIDE;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::
    SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SCHEMA_VERSION;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::
    SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SEALED_BEFORE_MEASUREMENT;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::
    SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SELECTION_PROTOCOL_ID;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::
    SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_USED_FOR_CALIBRATION;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::SIQSShadowObserveRssHoldoutDigestV1;
using gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::SIQSShadowObserveRssHoldoutFixtureV1;

static_assert(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SCHEMA_VERSION == 1);
static_assert(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_FIXTURE_COUNT == 8);
static_assert(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SEALED_BEFORE_MEASUREMENT);
static_assert(!SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_USED_FOR_CALIBRATION);
static_assert(noexcept(siqs_shadow_observe_rss_holdout_v1_digest()));

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

[[nodiscard]] Integer integer_from_decimal(std::string_view value) {
    return Integer(std::string(value));
}

[[nodiscard]] Integer next_prime_after(const Integer& seed) {
    Integer prime;
    mpz_nextprime(prime.get_mpz(), seed.get_mpz());
    return prime;
}

[[nodiscard]] bool digest_is_nonzero(const SIQSShadowObserveRssHoldoutDigestV1& digest) noexcept {
    return digest.low != 0 && digest.high != 0;
}

template <size_t Count>
[[nodiscard]] SIQSShadowObserveRssHoldoutDigestV1
digest_of(const std::array<SIQSShadowObserveRssHoldoutFixtureV1, Count>& fixtures) noexcept {
    return siqs_shadow_observe_rss_holdout_v1_digest(
        std::span<const SIQSShadowObserveRssHoldoutFixtureV1>(fixtures));
}

template <size_t Count>
void check_digest_changed(const std::array<SIQSShadowObserveRssHoldoutFixtureV1, Count>& fixtures,
                          const SIQSShadowObserveRssHoldoutDigestV1& baseline) {
    CHECK(!(digest_of(fixtures) == baseline));
}

void test_manifest_metadata() {
    CHECK(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_DOMAIN ==
          "gnfs.siqs.shadow_observe_rss_holdout.v1");
    CHECK(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SCHEMA_VERSION == 1);
    CHECK(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_CORPUS_ID == "siqs50_shadow_observe_rss_holdout_v1");
    CHECK(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SELECTION_PROTOCOL_ID ==
          "gmp_nextprime_decimal_stride_v1");
    CHECK(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SEALED_BEFORE_MEASUREMENT);
    CHECK(!SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_USED_FOR_CALIBRATION);
    CHECK(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_FIXTURE_COUNT == 8);
    CHECK(SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1.size() == 8);
    CHECK(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_MAX_SECONDS == 30);
    CHECK(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_P_BASE == "2100000000000000000000000");
    CHECK(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_P_STRIDE == "11000000000000000000000");
    CHECK(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_Q_BASE == "8100000000000000000000000");
    CHECK(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_Q_STRIDE == "17000000000000000000000");
}

void test_fixture_generation_and_identity() {
    const Integer p_base = integer_from_decimal(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_P_BASE);
    const Integer p_stride = integer_from_decimal(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_P_STRIDE);
    const Integer q_base = integer_from_decimal(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_Q_BASE);
    const Integer q_stride = integer_from_decimal(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_Q_STRIDE);

    std::set<uint32_t> ids;
    std::set<std::string_view> moduli;
    std::set<std::string_view> seeds;
    std::set<std::string_view> factors;

    for (size_t index = 0; index < SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1.size(); ++index) {
        const auto& fixture = SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1[index];
        CHECK(fixture.id == index + 1);
        CHECK(ids.insert(fixture.id).second);
        CHECK(moduli.insert(fixture.modulus).second);
        CHECK(seeds.insert(fixture.seed_p).second);
        CHECK(seeds.insert(fixture.seed_q).second);
        CHECK(factors.insert(fixture.factor_p).second);
        CHECK(factors.insert(fixture.factor_q).second);

        const Integer ordinal(static_cast<uint64_t>(index));
        const Integer expected_seed_p = p_base + p_stride * ordinal;
        const Integer expected_seed_q = q_base + q_stride * ordinal;
        const Integer seed_p = integer_from_decimal(fixture.seed_p);
        const Integer seed_q = integer_from_decimal(fixture.seed_q);
        CHECK(seed_p == expected_seed_p);
        CHECK(seed_q == expected_seed_q);
        CHECK(seed_p.to_string() == fixture.seed_p);
        CHECK(seed_q.to_string() == fixture.seed_q);

        const Integer factor_p = integer_from_decimal(fixture.factor_p);
        const Integer factor_q = integer_from_decimal(fixture.factor_q);
        const Integer modulus = integer_from_decimal(fixture.modulus);
        CHECK(next_prime_after(seed_p) == factor_p);
        CHECK(next_prime_after(seed_q) == factor_q);
        CHECK(factor_p.is_probable_prime(25) > 0);
        CHECK(factor_q.is_probable_prime(25) > 0);
        CHECK(factor_p > Integer(1));
        CHECK(factor_p <= factor_q);
        CHECK(factor_q < modulus);
        CHECK(factor_p * factor_q == modulus);
        CHECK(factor_p.num_digits(10) == 25);
        CHECK(factor_q.num_digits(10) == 25);
        CHECK(modulus.num_digits(10) == 50);
        CHECK(factor_p.to_string() == fixture.factor_p);
        CHECK(factor_q.to_string() == fixture.factor_q);
        CHECK(modulus.to_string() == fixture.modulus);
    }

    CHECK(ids.size() == SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_FIXTURE_COUNT);
    CHECK(moduli.size() == SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_FIXTURE_COUNT);
    CHECK(seeds.size() == 2 * SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_FIXTURE_COUNT);
    CHECK(factors.size() == 2 * SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_FIXTURE_COUNT);
}

void test_stable_digest_and_sensitivity() {
    const SIQSShadowObserveRssHoldoutDigestV1 baseline =
        siqs_shadow_observe_rss_holdout_v1_digest();
    CHECK(baseline.low == SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_LOW);
    CHECK(baseline.high == SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_HIGH);
    CHECK(baseline.low == UINT64_C(303806906129662515));
    CHECK(baseline.high == UINT64_C(18179245792498443738));
    CHECK(digest_is_nonzero(baseline));
    CHECK(digest_of(SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1) == baseline);
    CHECK(siqs_shadow_observe_rss_holdout_v1_digest() == baseline);

    auto changed = SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1;
    ++changed[0].id;
    check_digest_changed(changed, baseline);

    changed = SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1;
    changed[0].seed_p.remove_suffix(1);
    check_digest_changed(changed, baseline);

    changed = SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1;
    changed[0].seed_q.remove_suffix(1);
    check_digest_changed(changed, baseline);

    changed = SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1;
    changed[0].modulus.remove_suffix(1);
    check_digest_changed(changed, baseline);

    changed = SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1;
    changed[0].factor_p.remove_suffix(1);
    check_digest_changed(changed, baseline);

    changed = SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1;
    changed[0].factor_q.remove_suffix(1);
    check_digest_changed(changed, baseline);

    changed = SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1;
    std::swap(changed[0], changed[1]);
    check_digest_changed(changed, baseline);

    CHECK(!(siqs_shadow_observe_rss_holdout_v1_digest(
                std::span<const SIQSShadowObserveRssHoldoutFixtureV1>(
                    SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1.data(),
                    SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1.size() - 1)) == baseline));
}

} // namespace

int main() {
    test_manifest_metadata();
    test_fixture_generation_and_identity();
    test_stable_digest_and_sensitivity();

    std::cout << "SIQS shadow observe RSS holdouts: " << checks_passed << " passed, "
              << checks_failed << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
