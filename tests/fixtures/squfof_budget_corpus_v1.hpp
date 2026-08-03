#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gnfs::tests::fixtures {

enum class SqufofBudgetCallerPath : uint8_t {
    three_lp = 0,
    normal_2lp = 1,
};

enum class SqufofBudgetBitBand : uint8_t {
    low = 0,
    mid = 1,
    high = 2,
};

struct SqufofBudgetCase {
    SqufofBudgetCallerPath caller_path;
    SqufofBudgetBitBand bit_band;
    uint64_t p;
    uint64_t q;
    uint64_t n;
    uint32_t budget;
};

inline constexpr std::string_view SQUFOF_BUDGET_CORPUS_V1_NAME =
    "prospective_squfof_budget_corpus_v1";
inline constexpr std::string_view SQUFOF_BUDGET_CORPUS_V1_GENERATOR = "splitmix64-prime-pairs-v1";
inline constexpr uint64_t SQUFOF_BUDGET_CORPUS_V1_SEED = UINT64_C(0x4255444745545631);
inline constexpr size_t SQUFOF_BUDGET_CORPUS_V1_UNIQUE_CASES_PER_BAND = 32;
inline constexpr size_t SQUFOF_BUDGET_CORPUS_V1_PATHS_PER_CASE = 2;
inline constexpr size_t SQUFOF_BUDGET_CORPUS_V1_BAND_COUNT = 3;
inline constexpr size_t SQUFOF_BUDGET_CORPUS_V1_UNIQUE_CASE_COUNT =
    SQUFOF_BUDGET_CORPUS_V1_UNIQUE_CASES_PER_BAND * SQUFOF_BUDGET_CORPUS_V1_BAND_COUNT;
inline constexpr size_t SQUFOF_BUDGET_CORPUS_V1_ROW_COUNT =
    SQUFOF_BUDGET_CORPUS_V1_UNIQUE_CASE_COUNT * SQUFOF_BUDGET_CORPUS_V1_PATHS_PER_CASE;

// Prospective, result-independent generation contract:
//
// 1. Initialize one SplitMix64 stream per band from the frozen seed and band
//    ordinal.
// 2. Map each draw into the band's half-open factor interval, make it odd,
//    and scan upward (wrapping within that interval) for the first prime.
// 3. Form canonical distinct-prime pairs p < q. Reject only duplicate p*q
//    values within the corpus; never call SQUFOF or inspect factor outcomes.
// 4. Emit adjacent three_lp and normal_2lp rows for each p*q using the
//    production caller budgets frozen below.
//
// The factor intervals intentionally place products well inside the caller
// thresholds: low < 2^40, mid in [2^40, 2^50), and high in [2^50, 2^62).
struct SqufofBudgetBandSpec {
    SqufofBudgetBitBand band;
    uint32_t factor_min;
    uint32_t factor_max_exclusive;
    uint32_t three_lp_budget;
    uint32_t normal_2lp_budget;
};

inline constexpr std::array<SqufofBudgetBandSpec, SQUFOF_BUDGET_CORPUS_V1_BAND_COUNT>
    SQUFOF_BUDGET_CORPUS_V1_BANDS{{
        {SqufofBudgetBitBand::low, UINT32_C(1) << 18, UINT32_C(1) << 19, UINT32_C(1000),
         UINT32_C(2000)},
        {SqufofBudgetBitBand::mid, UINT32_C(1) << 22, UINT32_C(1) << 23, UINT32_C(2000),
         UINT32_C(5000)},
        {SqufofBudgetBitBand::high, UINT32_C(1) << 29, UINT32_C(1) << 30, UINT32_C(5000),
         UINT32_C(20000)},
    }};

namespace squfof_budget_corpus_v1_detail {

class SplitMix64 {
public:
    explicit constexpr SplitMix64(uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] constexpr uint64_t next() noexcept {
        uint64_t value = (state_ += UINT64_C(0x9e3779b97f4a7c15));
        value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
        return value ^ (value >> 31);
    }

private:
    uint64_t state_;
};

[[nodiscard]] constexpr uint64_t modular_power(uint64_t base, uint64_t exponent,
                                               uint64_t modulus) noexcept {
    uint64_t result = 1;
    while (exponent != 0) {
        if ((exponent & 1U) != 0) {
            result = (result * base) % modulus;
        }
        base = (base * base) % modulus;
        exponent >>= 1;
    }
    return result;
}

// All generated factors are below 2^30. Strong probable-prime tests for
// bases 2, 3, 5, 7, and 11 are deterministic throughout this bounded domain.
[[nodiscard]] constexpr bool is_prime(uint32_t value) noexcept {
    if (value < 2) {
        return false;
    }
    constexpr std::array<uint32_t, 5> SMALL_BASES{{2, 3, 5, 7, 11}};
    for (const uint32_t prime : SMALL_BASES) {
        if (value == prime) {
            return true;
        }
        if (value % prime == 0) {
            return false;
        }
    }

    uint64_t odd_part = static_cast<uint64_t>(value) - 1;
    unsigned power_of_two = 0;
    while ((odd_part & 1U) == 0) {
        odd_part >>= 1;
        ++power_of_two;
    }

    for (const uint32_t base : SMALL_BASES) {
        uint64_t witness = modular_power(base, odd_part, value);
        if (witness == 1 || witness == static_cast<uint64_t>(value) - 1) {
            continue;
        }

        bool probably_prime = false;
        for (unsigned iteration = 1; iteration < power_of_two; ++iteration) {
            witness = (witness * witness) % value;
            if (witness == static_cast<uint64_t>(value) - 1) {
                probably_prime = true;
                break;
            }
        }
        if (!probably_prime) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr uint32_t next_prime(uint64_t draw,
                                            const SqufofBudgetBandSpec& spec) noexcept {
    const uint32_t width = spec.factor_max_exclusive - spec.factor_min;
    uint32_t candidate = spec.factor_min + static_cast<uint32_t>(draw % width);
    candidate |= 1U;

    while (true) {
        if (is_prime(candidate)) {
            return candidate;
        }
        candidate += 2;
        if (candidate >= spec.factor_max_exclusive) {
            candidate = spec.factor_min | 1U;
        }
    }
}

[[nodiscard]] constexpr uint64_t band_seed(size_t band_index) noexcept {
    return SQUFOF_BUDGET_CORPUS_V1_SEED ^
           (UINT64_C(0xd1b54a32d192ed03) * static_cast<uint64_t>(band_index + 1));
}

[[nodiscard]] consteval std::array<SqufofBudgetCase, SQUFOF_BUDGET_CORPUS_V1_ROW_COUNT>
make_corpus() noexcept {
    std::array<SqufofBudgetCase, SQUFOF_BUDGET_CORPUS_V1_ROW_COUNT> corpus{};
    size_t unique_count = 0;

    for (size_t band_index = 0; band_index < SQUFOF_BUDGET_CORPUS_V1_BANDS.size(); ++band_index) {
        const auto& spec = SQUFOF_BUDGET_CORPUS_V1_BANDS[band_index];
        SplitMix64 generator(band_seed(band_index));

        for (size_t case_in_band = 0; case_in_band < SQUFOF_BUDGET_CORPUS_V1_UNIQUE_CASES_PER_BAND;
             ++case_in_band) {
            uint32_t p = 0;
            uint32_t q = 0;
            uint64_t n = 0;
            bool duplicate = false;
            do {
                p = next_prime(generator.next(), spec);
                do {
                    q = next_prime(generator.next(), spec);
                } while (q == p);
                if (q < p) {
                    const uint32_t temporary = p;
                    p = q;
                    q = temporary;
                }
                n = static_cast<uint64_t>(p) * q;

                duplicate = false;
                for (size_t previous = 0; previous < unique_count; ++previous) {
                    if (corpus[previous * SQUFOF_BUDGET_CORPUS_V1_PATHS_PER_CASE].n == n) {
                        duplicate = true;
                        break;
                    }
                }
            } while (duplicate);

            const size_t row = unique_count * SQUFOF_BUDGET_CORPUS_V1_PATHS_PER_CASE;
            corpus[row] = {
                SqufofBudgetCallerPath::three_lp, spec.band, p, q, n, spec.three_lp_budget};
            corpus[row + 1] = {
                SqufofBudgetCallerPath::normal_2lp, spec.band, p, q, n, spec.normal_2lp_budget};
            ++unique_count;
        }
    }
    return corpus;
}

[[nodiscard]] constexpr const SqufofBudgetBandSpec& band_spec(SqufofBudgetBitBand band) noexcept {
    return SQUFOF_BUDGET_CORPUS_V1_BANDS[static_cast<size_t>(band)];
}

[[nodiscard]] constexpr bool n_is_in_band(uint64_t n, SqufofBudgetBitBand band) noexcept {
    switch (band) {
    case SqufofBudgetBitBand::low:
        return n < (UINT64_C(1) << 40);
    case SqufofBudgetBitBand::mid:
        return n >= (UINT64_C(1) << 40) && n < (UINT64_C(1) << 50);
    case SqufofBudgetBitBand::high:
        return n >= (UINT64_C(1) << 50) && n < (UINT64_C(1) << 62);
    }
    return false;
}

struct Digest {
    uint64_t low;
    uint64_t high;
};

class DigestBuilder {
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

    [[nodiscard]] constexpr Digest finish() const noexcept {
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

} // namespace squfof_budget_corpus_v1_detail

inline constexpr auto SQUFOF_BUDGET_CORPUS_V1 = squfof_budget_corpus_v1_detail::make_corpus();

namespace squfof_budget_corpus_v1_detail {

[[nodiscard]] constexpr Digest compute_digest() noexcept {
    DigestBuilder builder;
    for (const char value : SQUFOF_BUDGET_CORPUS_V1_NAME) {
        builder.append_byte(static_cast<uint8_t>(value));
    }
    builder.append_byte(0);
    for (const char value : SQUFOF_BUDGET_CORPUS_V1_GENERATOR) {
        builder.append_byte(static_cast<uint8_t>(value));
    }
    builder.append_byte(0);
    builder.append_u64(SQUFOF_BUDGET_CORPUS_V1_SEED);
    builder.append_u64(SQUFOF_BUDGET_CORPUS_V1.size());
    for (size_t index = 0; index < SQUFOF_BUDGET_CORPUS_V1.size(); ++index) {
        const auto& test_case = SQUFOF_BUDGET_CORPUS_V1[index];
        builder.append_u64(index);
        builder.append_byte(static_cast<uint8_t>(test_case.caller_path));
        builder.append_byte(static_cast<uint8_t>(test_case.bit_band));
        builder.append_u64(test_case.p);
        builder.append_u64(test_case.q);
        builder.append_u64(test_case.n);
        builder.append_u32(test_case.budget);
    }
    return builder.finish();
}

[[nodiscard]] constexpr bool validate_corpus() noexcept {
    if (SQUFOF_BUDGET_CORPUS_V1.size() != SQUFOF_BUDGET_CORPUS_V1_ROW_COUNT) {
        return false;
    }

    for (size_t unique_index = 0; unique_index < SQUFOF_BUDGET_CORPUS_V1_UNIQUE_CASE_COUNT;
         ++unique_index) {
        const size_t row = unique_index * SQUFOF_BUDGET_CORPUS_V1_PATHS_PER_CASE;
        const auto& three_lp = SQUFOF_BUDGET_CORPUS_V1[row];
        const auto& normal_2lp = SQUFOF_BUDGET_CORPUS_V1[row + 1];
        const auto& spec = band_spec(three_lp.bit_band);

        if (three_lp.caller_path != SqufofBudgetCallerPath::three_lp ||
            normal_2lp.caller_path != SqufofBudgetCallerPath::normal_2lp ||
            normal_2lp.bit_band != three_lp.bit_band || normal_2lp.p != three_lp.p ||
            normal_2lp.q != three_lp.q || normal_2lp.n != three_lp.n ||
            three_lp.budget != spec.three_lp_budget ||
            normal_2lp.budget != spec.normal_2lp_budget) {
            return false;
        }
        if (three_lp.p >= three_lp.q || three_lp.p < spec.factor_min ||
            three_lp.q >= spec.factor_max_exclusive ||
            !is_prime(static_cast<uint32_t>(three_lp.p)) ||
            !is_prime(static_cast<uint32_t>(three_lp.q)) || three_lp.p * three_lp.q != three_lp.n ||
            !n_is_in_band(three_lp.n, three_lp.bit_band)) {
            return false;
        }

        const size_t expected_band_index =
            unique_index / SQUFOF_BUDGET_CORPUS_V1_UNIQUE_CASES_PER_BAND;
        if (static_cast<size_t>(three_lp.bit_band) != expected_band_index) {
            return false;
        }
        for (size_t previous = 0; previous < unique_index; ++previous) {
            if (SQUFOF_BUDGET_CORPUS_V1[previous * SQUFOF_BUDGET_CORPUS_V1_PATHS_PER_CASE].n ==
                three_lp.n) {
                return false;
            }
        }
    }
    return true;
}

} // namespace squfof_budget_corpus_v1_detail

inline constexpr auto SQUFOF_BUDGET_CORPUS_V1_COMPUTED_DIGEST =
    squfof_budget_corpus_v1_detail::compute_digest();

// Digest encoding: corpus name and generator name (each zero-terminated),
// seed, row count, then each row's little-endian index, caller path, bit band,
// p, q, n, and caller budget. The two lanes use the stable fixture mixer.
inline constexpr uint64_t SQUFOF_BUDGET_CORPUS_V1_DIGEST_LOW = UINT64_C(16007979797267497993);
inline constexpr uint64_t SQUFOF_BUDGET_CORPUS_V1_DIGEST_HIGH = UINT64_C(6430637409354473680);

static_assert(SQUFOF_BUDGET_CORPUS_V1_ROW_COUNT == 192);
static_assert(squfof_budget_corpus_v1_detail::validate_corpus());
static_assert(SQUFOF_BUDGET_CORPUS_V1_COMPUTED_DIGEST.low == SQUFOF_BUDGET_CORPUS_V1_DIGEST_LOW);
static_assert(SQUFOF_BUDGET_CORPUS_V1_COMPUTED_DIGEST.high == SQUFOF_BUDGET_CORPUS_V1_DIGEST_HIGH);

} // namespace gnfs::tests::fixtures
