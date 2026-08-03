#pragma once

// Fixed balanced semiprimes and SIQS parameter expectations for the manual
// live-sieve evidence probe. These are mathematical fixtures, not timings.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace gnfs::tests {

struct SIQSLiveSieveExpectedParamsV1 final {
    uint32_t factor_base_size;
    uint32_t sieve_half;
    uint32_t large_prime_multiplier;
    uint32_t a_factor_count;
    uint32_t sieve_error;
    uint32_t small_prime_cutoff;

    [[nodiscard]] friend constexpr bool operator==(const SIQSLiveSieveExpectedParamsV1&,
                                                   const SIQSLiveSieveExpectedParamsV1&) = default;
};

struct SIQSLiveSieveFixtureV1 final {
    uint32_t band;
    std::string_view modulus;
    std::string_view factor_p;
    std::string_view factor_q;
    SIQSLiveSieveExpectedParamsV1 params;
    std::size_t b_slots;
    std::string_view expected_polynomial_a;
    uint64_t expected_plan_digest_low;
    uint64_t expected_plan_digest_high;
};

inline constexpr std::array<SIQSLiveSieveFixtureV1, 3> SIQS_LIVE_SIEVE_FIXTURES_V1{{
    {
        50,
        "18027426610499408447671494571938206274555088868093",
        "2041646378661656688438487",
        "8829847714527711737483339",
        {1600, 65536, 120, 6, 12, 25},
        8,
        "228011737959984857761",
        UINT64_C(11016941208907243574),
        UINT64_C(11284256310490571374),
    },
    {
        70,
        "5145428984470526568901146249885494368844810337544108407107738301118297",
        "66226967311440082180617125485836853",
        "77693863894952991166382493860662549",
        {15000, 131072, 120, 9, 14, 60},
        4,
        "947990294073379802108353785517",
        UINT64_C(3984091375373043499),
        UINT64_C(5485512563116088663),
    },
    {
        90,
        "12003456912051976816033729794086226190656927528624157901409932936929745008371849672309778"
        "1",
        "300012345678901234567890123456789012345679303",
        "400098765432109876543210987654321098765432227",
        {130000, 524288, 200, 11, 16, 100},
        4,
        "745118251777260751516704028809722753681",
        UINT64_C(11074722052958763298),
        UINT64_C(5003813898734258881),
    },
}};

[[nodiscard]] inline constexpr std::optional<SIQSLiveSieveFixtureV1>
siqs_live_sieve_fixture_v1(uint32_t band) noexcept {
    for (const auto& fixture : SIQS_LIVE_SIEVE_FIXTURES_V1) {
        if (fixture.band == band) {
            return fixture;
        }
    }
    return std::nullopt;
}

} // namespace gnfs::tests
