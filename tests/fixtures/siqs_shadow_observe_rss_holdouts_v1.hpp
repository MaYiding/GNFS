#pragma once

// Sealed, outcome-blind 50-digit SIQS RSS holdouts. The corpus was frozen
// before any production factorization, probe, timing, or RSS measurement was
// run against it.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace gnfs::tests::fixtures {

struct SIQSShadowObserveRssHoldoutFixtureV1 final {
    uint32_t id;
    std::string_view seed_p;
    std::string_view seed_q;
    std::string_view modulus;
    std::string_view factor_p;
    std::string_view factor_q;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowObserveRssHoldoutFixtureV1&,
               const SIQSShadowObserveRssHoldoutFixtureV1&) noexcept = default;
};

struct SIQSShadowObserveRssHoldoutDigestV1 final {
    uint64_t low;
    uint64_t high;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowObserveRssHoldoutDigestV1&,
               const SIQSShadowObserveRssHoldoutDigestV1&) noexcept = default;
};

inline constexpr std::string_view SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_DOMAIN =
    "gnfs.siqs.shadow_observe_rss_holdout.v1";
inline constexpr uint32_t SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SCHEMA_VERSION = 1;
inline constexpr std::string_view SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_CORPUS_ID =
    "siqs50_shadow_observe_rss_holdout_v1";
inline constexpr std::string_view SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SELECTION_PROTOCOL_ID =
    "gmp_nextprime_decimal_stride_v1";
inline constexpr bool SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SEALED_BEFORE_MEASUREMENT = true;
inline constexpr bool SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_USED_FOR_CALIBRATION = false;
inline constexpr std::size_t SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_FIXTURE_COUNT = 8;
inline constexpr uint32_t SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_MAX_SECONDS = 30;

inline constexpr std::string_view SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_P_BASE =
    "2100000000000000000000000";
inline constexpr std::string_view SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_P_STRIDE =
    "11000000000000000000000";
inline constexpr std::string_view SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_Q_BASE =
    "8100000000000000000000000";
inline constexpr std::string_view SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_Q_STRIDE =
    "17000000000000000000000";

// For zero-based row index i, seed_p = p_base + i * p_stride and
// seed_q = q_base + i * q_stride. GMP mpz_nextprime(seed) selects the first
// probable prime strictly greater than each decimal seed.
inline constexpr std::array<SIQSShadowObserveRssHoldoutFixtureV1,
                            SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_FIXTURE_COUNT>
    SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1{{
        {1, "2100000000000000000000000", "8100000000000000000000000",
         "17010000000000000000000654600000000000000000005129", "2100000000000000000000023",
         "8100000000000000000000223"},
        {2, "2111000000000000000000000", "8117000000000000000000000",
         "17134987000000000000000769062000000000000000006831", "2111000000000000000000069",
         "8117000000000000000000099"},
        {3, "2122000000000000000000000", "8134000000000000000000000",
         "17260348000000000000000188500000000000000000000507", "2122000000000000000000013",
         "8134000000000000000000039"},
        {4, "2133000000000000000000000", "8151000000000000000000000",
         "17386083000000000000001579848000000000000000008869", "2133000000000000000000181",
         "8151000000000000000000049"},
        {5, "2144000000000000000000000", "8168000000000000000000000",
         "17512192000000000000000379912000000000000000000861", "2144000000000000000000041",
         "8168000000000000000000021"},
        {6, "2155000000000000000000000", "8185000000000000000000000",
         "17638675000000000000000092230000000000000000000039", "2155000000000000000000001",
         "8185000000000000000000039"},
        {7, "2166000000000000000000000", "8202000000000000000000000",
         "17765532000000000000000238464000000000000000000529", "2166000000000000000000023",
         "8202000000000000000000023"},
        {8, "2177000000000000000000000", "8219000000000000000000000",
         "17892763000000000000000537926000000000000000002491", "2177000000000000000000053",
         "8219000000000000000000047"},
    }};

namespace siqs_shadow_observe_rss_holdout_v1_detail {

// Stable two-lane 128-bit digest. Unsigned arithmetic wraps modulo 2^64.
// Integers use little-endian bytes, booleans use one 0/1 byte, and strings use
// a little-endian uint64 byte length followed by their bytes. The manifest is
// serialized in this order: domain, schema, corpus ID, selection protocol,
// base/stride parameters, flags, fixture count, max_seconds, then every fixture
// field in array order. The independently seeded lanes are finalized with the
// same SplitMix64 avalanche but never feed into one another.
class DigestBuilder final {
public:
    constexpr void append_byte(uint8_t value) noexcept {
        low_ ^= static_cast<uint64_t>(value);
        low_ *= UINT64_C(1099511628211);

        high_ ^= static_cast<uint64_t>(value) + byte_index_ * UINT64_C(0x9e3779b97f4a7c15);
        high_ = std::rotl(high_, 27);
        high_ *= UINT64_C(0x94d049bb133111eb);
        high_ += UINT64_C(0x2545f4914f6cdd1d);
        ++byte_index_;
    }

    constexpr void append_bool(bool value) noexcept {
        append_byte(static_cast<uint8_t>(value ? 1 : 0));
    }

    constexpr void append_u32(uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    constexpr void append_u64(uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    constexpr void append_string(std::string_view value) noexcept {
        append_u64(static_cast<uint64_t>(value.size()));
        for (const char character : value) {
            append_byte(static_cast<uint8_t>(static_cast<unsigned char>(character)));
        }
    }

    [[nodiscard]] constexpr SIQSShadowObserveRssHoldoutDigestV1 finish() const noexcept {
        return {avalanche(low_ ^ byte_index_), avalanche(high_ ^ std::rotl(byte_index_, 17))};
    }

private:
    [[nodiscard]] static constexpr uint64_t avalanche(uint64_t value) noexcept {
        value ^= value >> 30;
        value *= UINT64_C(0xbf58476d1ce4e5b9);
        value ^= value >> 27;
        value *= UINT64_C(0x94d049bb133111eb);
        value ^= value >> 31;
        return value;
    }

    uint64_t low_ = UINT64_C(14695981039346656037);
    uint64_t high_ = UINT64_C(0x243f6a8885a308d3);
    uint64_t byte_index_ = 0;
};

} // namespace siqs_shadow_observe_rss_holdout_v1_detail

[[nodiscard]] constexpr SIQSShadowObserveRssHoldoutDigestV1
siqs_shadow_observe_rss_holdout_v1_digest(
    std::span<const SIQSShadowObserveRssHoldoutFixtureV1> fixtures) noexcept {
    siqs_shadow_observe_rss_holdout_v1_detail::DigestBuilder builder;
    builder.append_string(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_DOMAIN);
    builder.append_u32(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SCHEMA_VERSION);
    builder.append_string(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_CORPUS_ID);
    builder.append_string(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SELECTION_PROTOCOL_ID);
    builder.append_string(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_P_BASE);
    builder.append_string(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_P_STRIDE);
    builder.append_string(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_Q_BASE);
    builder.append_string(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_Q_STRIDE);
    builder.append_bool(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SEALED_BEFORE_MEASUREMENT);
    builder.append_bool(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_USED_FOR_CALIBRATION);
    builder.append_u64(static_cast<uint64_t>(fixtures.size()));
    builder.append_u32(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_MAX_SECONDS);
    for (const auto& fixture : fixtures) {
        builder.append_u32(fixture.id);
        builder.append_string(fixture.seed_p);
        builder.append_string(fixture.seed_q);
        builder.append_string(fixture.modulus);
        builder.append_string(fixture.factor_p);
        builder.append_string(fixture.factor_q);
    }
    return builder.finish();
}

[[nodiscard]] constexpr SIQSShadowObserveRssHoldoutDigestV1
siqs_shadow_observe_rss_holdout_v1_digest() noexcept {
    return siqs_shadow_observe_rss_holdout_v1_digest(
        std::span<const SIQSShadowObserveRssHoldoutFixtureV1>(SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1));
}

inline constexpr uint64_t SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_LOW =
    UINT64_C(303806906129662515);
inline constexpr uint64_t SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_HIGH =
    UINT64_C(18179245792498443738);

static_assert(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SEALED_BEFORE_MEASUREMENT);
static_assert(!SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_USED_FOR_CALIBRATION);
static_assert(SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1.size() ==
              SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_FIXTURE_COUNT);
static_assert(siqs_shadow_observe_rss_holdout_v1_digest().low ==
              SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_LOW);
static_assert(siqs_shadow_observe_rss_holdout_v1_digest().high ==
              SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_HIGH);

} // namespace gnfs::tests::fixtures
