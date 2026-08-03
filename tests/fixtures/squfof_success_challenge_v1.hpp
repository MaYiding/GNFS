#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gnfs::tests::fixtures {

enum class SqufofSuccessChallengeCaller : uint8_t {
    normal_2lp = 1,
};

enum class SqufofSuccessChallengeProfile : uint8_t {
    close_balanced = 0,
    mildly_skewed = 1,
    moderately_skewed = 2,
};

struct SqufofSuccessChallengeProfileSpec {
    SqufofSuccessChallengeProfile profile;
    uint32_t p_min;
    uint32_t p_max_exclusive;
    uint32_t q_min;
    uint32_t q_max_exclusive;
    uint32_t gap_min;
    uint32_t gap_max_exclusive;
};

struct SqufofSuccessChallengeCase {
    SqufofSuccessChallengeCaller caller;
    SqufofSuccessChallengeProfile profile;
    uint64_t p;
    uint64_t q;
    uint64_t n;
    uint32_t budget;
};

inline constexpr std::string_view SQUFOF_SUCCESS_CHALLENGE_V1_NAME =
    "prospective_squfof_success_challenge_v1";
inline constexpr std::string_view SQUFOF_SUCCESS_CHALLENGE_V1_GENERATOR =
    "splitmix64-factor-balance-prime-pairs-v1";
inline constexpr uint64_t SQUFOF_SUCCESS_CHALLENGE_V1_SEED = UINT64_C(0x5355434345535331);
inline constexpr uint64_t SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_SEED_MIXER =
    UINT64_C(0xd1b54a32d192ed03);
inline constexpr uint32_t SQUFOF_SUCCESS_CHALLENGE_V1_BUDGET = UINT32_C(20000);
inline constexpr size_t SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_COUNT = 3;
inline constexpr size_t SQUFOF_SUCCESS_CHALLENGE_V1_CASES_PER_PROFILE = 64;
inline constexpr size_t SQUFOF_SUCCESS_CHALLENGE_V1_ROW_COUNT =
    SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_COUNT * SQUFOF_SUCCESS_CHALLENGE_V1_CASES_PER_PROFILE;

inline constexpr std::array<SqufofSuccessChallengeProfileSpec,
                            SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_COUNT>
    SQUFOF_SUCCESS_CHALLENGE_V1_PROFILES{{
        {SqufofSuccessChallengeProfile::close_balanced, UINT32_C(1) << 28,
         (UINT32_C(1) << 29) - (UINT32_C(1) << 22), UINT32_C(1) << 28, UINT32_C(1) << 29,
         UINT32_C(1) << 18, UINT32_C(1) << 22},
        {SqufofSuccessChallengeProfile::mildly_skewed, UINT32_C(1) << 27, UINT32_C(1) << 28,
         UINT32_C(1) << 29, UINT32_C(1) << 30, 0, 0},
        {SqufofSuccessChallengeProfile::moderately_skewed, UINT32_C(1) << 26, UINT32_C(1) << 27,
         UINT32_C(1) << 30, UINT32_C(1) << 31, 0, 0},
    }};

// Result-independent generation contract:
//
// 1. Each profile owns one SplitMix64 stream derived from the frozen seed and
//    profile ordinal. Every structural attempt consumes exactly two draws.
// 2. Draws map into the frozen factor intervals, become odd, and scan upward
//    for the first prime. Fixed intervals wrap; the close-balanced q scan does
//    not wrap and must retain the frozen gap bounds.
// 3. Reject only structural violations, duplicate products, or any factor used
//    earlier in the corpus. Never inspect a split, SQUFOF result, multiplier,
//    factorization outcome, or work count.
// 4. Emit 64 normal-2LP cases per profile at production budget 20000.
namespace squfof_success_challenge_v1_detail {

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

// The fixed factors are below 2^31. These Miller-Rabin bases are deterministic
// throughout that domain, and every modular product remains below 2^62.
[[nodiscard]] constexpr bool is_prime(uint32_t value) noexcept {
    if (value < 2) {
        return false;
    }
    constexpr std::array<uint32_t, 5> BASES{{2, 3, 5, 7, 11}};
    for (const uint32_t base : BASES) {
        if (value == base) {
            return true;
        }
        if (value % base == 0) {
            return false;
        }
    }

    uint64_t odd_part = static_cast<uint64_t>(value) - 1;
    unsigned power_of_two = 0;
    while ((odd_part & 1U) == 0) {
        odd_part >>= 1;
        ++power_of_two;
    }

    for (const uint32_t base : BASES) {
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

[[nodiscard]] constexpr uint32_t wrapped_next_prime(uint64_t draw, uint32_t minimum,
                                                    uint32_t maximum_exclusive) noexcept {
    const uint32_t width = maximum_exclusive - minimum;
    const uint32_t first =
        (minimum + static_cast<uint32_t>(draw % static_cast<uint64_t>(width))) | 1U;
    uint32_t candidate = first;
    do {
        if (is_prime(candidate)) {
            return candidate;
        }
        candidate += 2;
        if (candidate >= maximum_exclusive) {
            candidate = minimum | 1U;
        }
    } while (candidate != first);
    return 0;
}

[[nodiscard]] constexpr uint32_t close_q(uint32_t p, uint64_t draw,
                                         const SqufofSuccessChallengeProfileSpec& spec) noexcept {
    const uint32_t even_gap_count = (spec.gap_max_exclusive - spec.gap_min) / 2;
    const uint32_t gap =
        spec.gap_min + 2 * static_cast<uint32_t>(draw % static_cast<uint64_t>(even_gap_count));
    uint32_t candidate = p + gap;
    while (candidate < spec.q_max_exclusive && candidate - p < spec.gap_max_exclusive) {
        if (is_prime(candidate)) {
            return candidate;
        }
        candidate += 2;
    }
    return 0;
}

[[nodiscard]] constexpr uint64_t profile_seed(size_t profile_index) noexcept {
    return SQUFOF_SUCCESS_CHALLENGE_V1_SEED ^ (SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_SEED_MIXER *
                                               static_cast<uint64_t>(profile_index + 1));
}

struct GenerationResult {
    std::array<SqufofSuccessChallengeCase, SQUFOF_SUCCESS_CHALLENGE_V1_ROW_COUNT> rows{};
    std::array<size_t, SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_COUNT> attempts{};
};

[[nodiscard]] constexpr bool duplicates_previous(const GenerationResult& result, size_t row_count,
                                                 uint32_t p, uint32_t q, uint64_t n) noexcept {
    for (size_t previous = 0; previous < row_count; ++previous) {
        const auto& row = result.rows[previous];
        if (row.n == n || row.p == p || row.p == q || row.q == p || row.q == q) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline GenerationResult make_corpus() {
    GenerationResult result;
    size_t row_count = 0;
    for (size_t profile_index = 0; profile_index < SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_COUNT;
         ++profile_index) {
        const auto& spec = SQUFOF_SUCCESS_CHALLENGE_V1_PROFILES[profile_index];
        SplitMix64 generator(profile_seed(profile_index));
        size_t accepted = 0;
        while (accepted < SQUFOF_SUCCESS_CHALLENGE_V1_CASES_PER_PROFILE) {
            if (result.attempts[profile_index] == 1000000) {
                throw "SQUFOF success-challenge generator exhausted its structural attempt limit";
            }
            ++result.attempts[profile_index];
            const uint64_t p_draw = generator.next();
            const uint64_t q_draw = generator.next();
            const uint32_t p = wrapped_next_prime(p_draw, spec.p_min, spec.p_max_exclusive);
            const uint32_t q = spec.profile == SqufofSuccessChallengeProfile::close_balanced
                                   ? close_q(p, q_draw, spec)
                                   : wrapped_next_prime(q_draw, spec.q_min, spec.q_max_exclusive);
            if (p == 0 || q == 0 || p >= q || p >= (UINT32_C(1) << 31) ||
                q >= (UINT32_C(1) << 31)) {
                continue;
            }
            const uint64_t n = static_cast<uint64_t>(p) * q;
            if (n < (UINT64_C(1) << 50) || n >= (UINT64_C(1) << 62) ||
                duplicates_previous(result, row_count, p, q, n)) {
                continue;
            }
            result.rows[row_count] = {
                SqufofSuccessChallengeCaller::normal_2lp, spec.profile, p, q, n,
                SQUFOF_SUCCESS_CHALLENGE_V1_BUDGET};
            ++row_count;
            ++accepted;
        }
    }
    return result;
}

struct Digest {
    uint64_t low;
    uint64_t high;

    [[nodiscard]] constexpr bool operator==(const Digest&) const noexcept = default;
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

    constexpr void append_string(std::string_view value) noexcept {
        for (const char character : value) {
            append_byte(static_cast<uint8_t>(character));
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

[[nodiscard]] constexpr Digest
corpus_digest(const std::array<SqufofSuccessChallengeCase, SQUFOF_SUCCESS_CHALLENGE_V1_ROW_COUNT>&
                  corpus) noexcept {
    DigestBuilder builder;
    builder.append_string(SQUFOF_SUCCESS_CHALLENGE_V1_NAME);
    builder.append_byte(0);
    builder.append_string(SQUFOF_SUCCESS_CHALLENGE_V1_GENERATOR);
    builder.append_byte(0);
    builder.append_u64(SQUFOF_SUCCESS_CHALLENGE_V1_SEED);
    builder.append_u64(SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_SEED_MIXER);
    builder.append_u64(SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_COUNT);
    builder.append_u64(SQUFOF_SUCCESS_CHALLENGE_V1_CASES_PER_PROFILE);
    builder.append_u32(SQUFOF_SUCCESS_CHALLENGE_V1_BUDGET);
    for (const auto& spec : SQUFOF_SUCCESS_CHALLENGE_V1_PROFILES) {
        builder.append_byte(static_cast<uint8_t>(spec.profile));
        builder.append_u32(spec.p_min);
        builder.append_u32(spec.p_max_exclusive);
        builder.append_u32(spec.q_min);
        builder.append_u32(spec.q_max_exclusive);
        builder.append_u32(spec.gap_min);
        builder.append_u32(spec.gap_max_exclusive);
    }
    builder.append_u64(corpus.size());
    for (size_t row_index = 0; row_index < corpus.size(); ++row_index) {
        const auto& row = corpus[row_index];
        builder.append_u64(row_index);
        builder.append_byte(static_cast<uint8_t>(row.caller));
        builder.append_byte(static_cast<uint8_t>(row.profile));
        builder.append_u64(row.p);
        builder.append_u64(row.q);
        builder.append_u64(row.n);
        builder.append_u32(row.budget);
    }
    return builder.finish();
}

} // namespace squfof_success_challenge_v1_detail

inline const auto SQUFOF_SUCCESS_CHALLENGE_V1_GENERATION =
    squfof_success_challenge_v1_detail::make_corpus();
inline const auto& SQUFOF_SUCCESS_CHALLENGE_V1 = SQUFOF_SUCCESS_CHALLENGE_V1_GENERATION.rows;
inline const auto& SQUFOF_SUCCESS_CHALLENGE_V1_ATTEMPTS =
    SQUFOF_SUCCESS_CHALLENGE_V1_GENERATION.attempts;
inline constexpr uint64_t SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_LOW = UINT64_C(10783171939506602749);
inline constexpr uint64_t SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_HIGH = UINT64_C(9236118909252415409);

} // namespace gnfs::tests::fixtures
