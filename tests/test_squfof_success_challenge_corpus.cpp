// Outcome-blind contract for the prospective SQUFOF success challenge.
//
// This test intentionally does not include or call SQUFOF. It seals structural
// inputs and data splits before either frozen budget policy observes them.

#include "fixtures/squfof_success_challenge_v1.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_ATTEMPTS;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_BUDGET;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_CASES_PER_PROFILE;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_HIGH;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_LOW;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_GENERATOR;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_NAME;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_COUNT;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_SEED_MIXER;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_PROFILES;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_ROW_COUNT;
using gnfs::tests::fixtures::SQUFOF_SUCCESS_CHALLENGE_V1_SEED;
using gnfs::tests::fixtures::SqufofSuccessChallengeCaller;
using gnfs::tests::fixtures::SqufofSuccessChallengeProfile;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

// Trial division is intentionally independent of the fixture's Miller-Rabin
// generator. The factors are below 2^31, so this remains an instant test.
[[nodiscard]] bool is_prime_by_trial_division(uint64_t value) noexcept {
    if (value < 2) {
        return false;
    }
    if (value % 2 == 0) {
        return value == 2;
    }
    for (uint64_t divisor = 3; divisor <= value / divisor; divisor += 2) {
        if (value % divisor == 0) {
            return false;
        }
    }
    return true;
}

enum class DataSplit : uint8_t {
    train = 0,
    validation = 1,
    confirmation = 2,
};

[[nodiscard]] constexpr uint64_t stable_n_hash(uint64_t n) noexcept {
    n ^= n >> 30;
    n *= UINT64_C(0xbf58476d1ce4e5b9);
    n ^= n >> 27;
    n *= UINT64_C(0x94d049bb133111eb);
    n ^= n >> 31;
    return n;
}

struct Digest final {
    uint64_t low = 0;
    uint64_t high = 0;

    [[nodiscard]] bool operator==(const Digest&) const noexcept = default;
};

class DigestBuilder final {
public:
    explicit DigestBuilder(std::string_view domain) {
        append_u64(domain.size());
        for (const char character : domain) {
            append_byte(static_cast<uint8_t>(character));
        }
    }

    void append_byte(uint8_t value) noexcept {
        low_ ^= static_cast<uint64_t>(value);
        low_ *= UINT64_C(1099511628211);

        high_ ^= static_cast<uint64_t>(value) + byte_index_ * UINT64_C(0x9e3779b97f4a7c15);
        high_ = std::rotl(high_, 27);
        high_ *= UINT64_C(0x94d049bb133111eb);
        high_ += UINT64_C(0x2545f4914f6cdd1d);
        ++byte_index_;
    }

    void append_u32(uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    void append_u64(uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    [[nodiscard]] Digest finish() const noexcept {
        return {avalanche(low_ ^ byte_index_), avalanche(high_ ^ std::rotl(byte_index_, 17))};
    }

private:
    [[nodiscard]] static uint64_t avalanche(uint64_t value) noexcept {
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

using SplitAssignment = std::array<DataSplit, SQUFOF_SUCCESS_CHALLENGE_V1_ROW_COUNT>;

[[nodiscard]] SplitAssignment make_split_assignment() {
    SplitAssignment assignment{};
    constexpr std::array<DataSplit, 4> CYCLE{{
        DataSplit::train,
        DataSplit::train,
        DataSplit::validation,
        DataSplit::confirmation,
    }};

    for (size_t profile = 0; profile < SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_COUNT; ++profile) {
        std::array<size_t, SQUFOF_SUCCESS_CHALLENGE_V1_CASES_PER_PROFILE> indices{};
        for (size_t offset = 0; offset < indices.size(); ++offset) {
            indices[offset] = profile * indices.size() + offset;
        }
        std::sort(indices.begin(), indices.end(), [](size_t left, size_t right) {
            const uint64_t left_n = SQUFOF_SUCCESS_CHALLENGE_V1[left].n;
            const uint64_t right_n = SQUFOF_SUCCESS_CHALLENGE_V1[right].n;
            const uint64_t left_hash = stable_n_hash(left_n);
            const uint64_t right_hash = stable_n_hash(right_n);
            return left_hash != right_hash ? left_hash < right_hash : left_n < right_n;
        });
        for (size_t rank = 0; rank < indices.size(); ++rank) {
            assignment[indices[rank]] = CYCLE[rank % CYCLE.size()];
        }
    }
    return assignment;
}

[[nodiscard]] Digest split_digest(const SplitAssignment& assignment) {
    DigestBuilder builder("GNFS-SQUFOF-SUCCESS-CHALLENGE-SPLIT-V1");
    builder.append_u32(1);
    builder.append_u64(SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_LOW);
    builder.append_u64(SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_HIGH);
    builder.append_u64(SQUFOF_SUCCESS_CHALLENGE_V1.size());
    for (size_t row_index = 0; row_index < SQUFOF_SUCCESS_CHALLENGE_V1.size(); ++row_index) {
        const auto& row = SQUFOF_SUCCESS_CHALLENGE_V1[row_index];
        builder.append_u64(row_index);
        builder.append_u64(row.n);
        builder.append_byte(static_cast<uint8_t>(row.caller));
        builder.append_byte(static_cast<uint8_t>(row.profile));
        builder.append_u32(row.budget);
        builder.append_byte(static_cast<uint8_t>(assignment[row_index]));
    }
    return builder.finish();
}

[[nodiscard]] std::string sanitize_token(std::string_view input) {
    std::string output;
    output.reserve(std::min<size_t>(input.size(), 200));
    for (const char character : input) {
        const auto byte = static_cast<unsigned char>(character);
        const bool safe = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                          (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte == '.';
        output.push_back(safe ? static_cast<char>(byte) : '_');
        if (output.size() == 200) {
            break;
        }
    }
    return output.empty() ? "unknown" : output;
}

void validate_provenance() {
    require(SQUFOF_SUCCESS_CHALLENGE_V1_NAME == "prospective_squfof_success_challenge_v1" &&
                SQUFOF_SUCCESS_CHALLENGE_V1_GENERATOR == "splitmix64-factor-balance-prime-pairs-v1",
            "success-challenge generator provenance changed");
    require(SQUFOF_SUCCESS_CHALLENGE_V1_SEED == UINT64_C(0x5355434345535331) &&
                SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_SEED_MIXER == UINT64_C(0xd1b54a32d192ed03),
            "success-challenge seed contract changed");
    require(SQUFOF_SUCCESS_CHALLENGE_V1_BUDGET == 20000 &&
                static_cast<uint8_t>(SqufofSuccessChallengeCaller::normal_2lp) == 1,
            "success-challenge caller budget contract changed");
    require(SQUFOF_SUCCESS_CHALLENGE_V1_ATTEMPTS == std::array<size_t, 3>{{64, 64, 64}},
            "success-challenge generation started rejecting structural attempts");
    const auto observed = gnfs::tests::fixtures::squfof_success_challenge_v1_detail::corpus_digest(
        SQUFOF_SUCCESS_CHALLENGE_V1);
    require(observed.low == SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_LOW &&
                observed.high == SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_HIGH,
            "success-challenge corpus identity changed");
}

void validate_rows() {
    require(SQUFOF_SUCCESS_CHALLENGE_V1.size() == SQUFOF_SUCCESS_CHALLENGE_V1_ROW_COUNT,
            "success-challenge row count changed");
    std::array<size_t, SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_COUNT> profile_counts{};
    std::array<uint64_t, SQUFOF_SUCCESS_CHALLENGE_V1_ROW_COUNT> products{};
    std::array<uint64_t, SQUFOF_SUCCESS_CHALLENGE_V1_ROW_COUNT * 2> factors{};
    size_t product_count = 0;
    size_t factor_count = 0;

    for (size_t row_index = 0; row_index < SQUFOF_SUCCESS_CHALLENGE_V1.size(); ++row_index) {
        const auto& row = SQUFOF_SUCCESS_CHALLENGE_V1[row_index];
        const size_t expected_profile = row_index / SQUFOF_SUCCESS_CHALLENGE_V1_CASES_PER_PROFILE;
        require(static_cast<size_t>(row.profile) == expected_profile,
                "success-challenge profile block order changed");
        require(row.caller == SqufofSuccessChallengeCaller::normal_2lp &&
                    row.budget == SQUFOF_SUCCESS_CHALLENGE_V1_BUDGET,
                "success-challenge row changed caller or budget");
        require(row.p < row.q && row.p < (UINT64_C(1) << 31) && row.q < (UINT64_C(1) << 31) &&
                    row.p * row.q == row.n,
                "success-challenge factor metadata is invalid");
        require(row.n >= (UINT64_C(1) << 50) && row.n < (UINT64_C(1) << 62) &&
                    (std::bit_width(row.n) == 57 || std::bit_width(row.n) == 58),
                "success-challenge product left the frozen bit range");
        require(is_prime_by_trial_division(row.p) && is_prime_by_trial_division(row.q),
                "success-challenge contains non-prime factor metadata");

        const auto& spec = SQUFOF_SUCCESS_CHALLENGE_V1_PROFILES[expected_profile];
        require(row.p >= spec.p_min && row.p < spec.p_max_exclusive && row.q >= spec.q_min &&
                    row.q < spec.q_max_exclusive,
                "success-challenge factor left its profile interval");
        if (row.profile == SqufofSuccessChallengeProfile::close_balanced) {
            const uint64_t gap = row.q - row.p;
            require(gap >= spec.gap_min && gap < spec.gap_max_exclusive,
                    "close-balanced challenge row left its gap interval");
        } else {
            require(spec.gap_min == 0 && spec.gap_max_exclusive == 0,
                    "skewed profile unexpectedly acquired a gap contract");
        }

        require(std::find(products.begin(),
                          products.begin() + static_cast<std::ptrdiff_t>(product_count),
                          row.n) == products.begin() + static_cast<std::ptrdiff_t>(product_count),
                "success-challenge contains a duplicate product");
        require(std::find(factors.begin(),
                          factors.begin() + static_cast<std::ptrdiff_t>(factor_count),
                          row.p) == factors.begin() + static_cast<std::ptrdiff_t>(factor_count) &&
                    std::find(factors.begin(),
                              factors.begin() + static_cast<std::ptrdiff_t>(factor_count),
                              row.q) == factors.begin() + static_cast<std::ptrdiff_t>(factor_count),
                "success-challenge reuses a prime factor");
        products[product_count++] = row.n;
        factors[factor_count++] = row.p;
        factors[factor_count++] = row.q;
        ++profile_counts[expected_profile];
    }

    require(profile_counts == std::array<size_t, 3>{{64, 64, 64}} && product_count == 192 &&
                factor_count == 384,
            "success-challenge profile or uniqueness count changed");
}

[[nodiscard]] Digest validate_split() {
    const SplitAssignment assignment = make_split_assignment();
    std::array<size_t, 3> split_counts{};
    std::array<std::array<size_t, 3>, SQUFOF_SUCCESS_CHALLENGE_V1_PROFILE_COUNT>
        profile_split_counts{};
    for (size_t row = 0; row < SQUFOF_SUCCESS_CHALLENGE_V1.size(); ++row) {
        const size_t profile = static_cast<size_t>(SQUFOF_SUCCESS_CHALLENGE_V1[row].profile);
        const size_t split = static_cast<size_t>(assignment[row]);
        ++split_counts[split];
        ++profile_split_counts[profile][split];
    }
    require(split_counts == std::array<size_t, 3>{{96, 48, 48}},
            "success-challenge split balance changed");
    for (const auto& profile_counts : profile_split_counts) {
        require(profile_counts == std::array<size_t, 3>{{32, 16, 16}},
                "success-challenge profile split balance changed");
    }

    const Digest observed = split_digest(assignment);
    constexpr Digest EXPECTED{UINT64_C(5936611983363779581), UINT64_C(6396469101558652297)};
    if (!(observed == EXPECTED)) {
        std::cerr << "GNFS_SQUFOF_SUCCESS_CHALLENGE_SPLIT_OBSERVED_V1 low=" << observed.low
                  << " high=" << observed.high << '\n';
        fail("success-challenge split identity changed");
    }
    return observed;
}

void run_contract() {
    validate_provenance();
    validate_rows();
    const Digest split = validate_split();
    std::cout << "GNFS_SQUFOF_SUCCESS_CHALLENGE_CORPUS_SUMMARY_V1"
              << " status=pass schema=1 rows=" << SQUFOF_SUCCESS_CHALLENGE_V1.size()
              << " unique_inputs=192 unique_factors=384"
              << " train=96 validation=48 confirmation=48"
              << " corpus_digest_low=" << SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_LOW
              << " corpus_digest_high=" << SQUFOF_SUCCESS_CHALLENGE_V1_DIGEST_HIGH
              << " split_digest_low=" << split.low << " split_digest_high=" << split.high
              << " squfof_probed=false candidate_cap=10056 production_cap=20000\n";
}

} // namespace

int main() {
    try {
        run_contract();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GNFS_SQUFOF_SUCCESS_CHALLENGE_CORPUS_SUMMARY_V1 status=fail error="
                  << sanitize_token(error.what()) << '\n';
        return 1;
    } catch (...) {
        std::cerr << "GNFS_SQUFOF_SUCCESS_CHALLENGE_CORPUS_SUMMARY_V1"
                     " status=fail error=unknown_exception\n";
        return 1;
    }
}
