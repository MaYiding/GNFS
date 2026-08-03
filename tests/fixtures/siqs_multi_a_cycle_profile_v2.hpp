#pragma once

// Fixed 50-digit corpus and fail-closed plan goldens for the isolated
// multi-A SIQS cycle-density profile. Timings and memory are never golden.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gnfs::tests {

struct SIQSMultiAExpectedParamsV2 final {
    uint32_t factor_base_size;
    uint32_t sieve_half;
    uint32_t large_prime_multiplier;
    uint32_t a_factor_count;
    uint32_t sieve_error;
    uint32_t small_prime_cutoff;

    [[nodiscard]] friend constexpr bool operator==(const SIQSMultiAExpectedParamsV2&,
                                                   const SIQSMultiAExpectedParamsV2&) = default;
};

struct SIQSMultiAPlanGoldenV2 final {
    std::size_t a_count;
    uint64_t digest_low;
    uint64_t digest_high;
};

struct SIQSMultiACycleFixtureV2 final {
    std::string_view profile_id;
    uint32_t band;
    std::string_view modulus;
    std::string_view factor_p;
    std::string_view factor_q;
    SIQSMultiAExpectedParamsV2 params;
    uint32_t seed;
    std::size_t max_a_count;
    std::size_t b_slots_per_a;
    std::string_view expected_first_a;
    std::array<SIQSMultiAPlanGoldenV2, 4> prefix_plan_goldens;
};

inline constexpr SIQSMultiACycleFixtureV2 SIQS_MULTI_A_CYCLE_FIXTURE_V2{
    "siqs50_multi_a_64x32_v2",
    50,
    "18027426610499408447671494571938206274555088868093",
    "2041646378661656688438487",
    "8829847714527711737483339",
    {1600, 65536, 120, 6, 12, 25},
    42,
    64,
    32,
    "228011737959984857761",
    {{{1, UINT64_C(5242784113925592557), UINT64_C(7615844999108521601)},
      {4, UINT64_C(1278098051305323338), UINT64_C(10613159032701573247)},
      {16, UINT64_C(4812946197025571407), UINT64_C(9075223404998431144)},
      {64, UINT64_C(1765793428706751577), UINT64_C(7545837539168964381)}}},
};

} // namespace gnfs::tests
