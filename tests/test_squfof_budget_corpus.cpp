// Outcome-blind contract for the prospective SQUFOF budget corpus.
//
// This test intentionally does not include or call SQUFOF. It freezes corpus
// provenance and data splits before any budget candidate is evaluated.

#include "fixtures/squfof_budget_corpus_v1.hpp"

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

using gnfs::tests::fixtures::SQUFOF_BUDGET_CORPUS_V1;
using gnfs::tests::fixtures::SQUFOF_BUDGET_CORPUS_V1_DIGEST_HIGH;
using gnfs::tests::fixtures::SQUFOF_BUDGET_CORPUS_V1_DIGEST_LOW;
using gnfs::tests::fixtures::SQUFOF_BUDGET_CORPUS_V1_ROW_COUNT;
using gnfs::tests::fixtures::SQUFOF_BUDGET_CORPUS_V1_UNIQUE_CASE_COUNT;
using gnfs::tests::fixtures::SQUFOF_BUDGET_CORPUS_V1_UNIQUE_CASES_PER_BAND;
using gnfs::tests::fixtures::SqufofBudgetBitBand;
using gnfs::tests::fixtures::SqufofBudgetCallerPath;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

[[nodiscard]] bool is_prime_by_trial_division(uint32_t value) noexcept {
    if (value < 2) {
        return false;
    }
    if (value % 2 == 0) {
        return value == 2;
    }
    for (uint32_t divisor = 3; static_cast<uint64_t>(divisor) * divisor <= value; divisor += 2) {
        if (value % divisor == 0) {
            return false;
        }
    }
    return true;
}

enum class DataSplit : uint8_t {
    train = 0,
    validation = 1,
    holdout = 2,
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

using SplitAssignment = std::array<DataSplit, SQUFOF_BUDGET_CORPUS_V1_ROW_COUNT>;

[[nodiscard]] SplitAssignment make_split_assignment() {
    SplitAssignment assignment{};
    constexpr std::array<DataSplit, 4> CYCLE{{
        DataSplit::train,
        DataSplit::train,
        DataSplit::validation,
        DataSplit::holdout,
    }};

    for (size_t band_index = 0; band_index < 3; ++band_index) {
        std::array<size_t, SQUFOF_BUDGET_CORPUS_V1_UNIQUE_CASES_PER_BAND> unique_indices{};
        for (size_t offset = 0; offset < unique_indices.size(); ++offset) {
            unique_indices[offset] =
                band_index * SQUFOF_BUDGET_CORPUS_V1_UNIQUE_CASES_PER_BAND + offset;
        }
        std::sort(unique_indices.begin(), unique_indices.end(), [](size_t left, size_t right) {
            const uint64_t left_n = SQUFOF_BUDGET_CORPUS_V1[left * 2].n;
            const uint64_t right_n = SQUFOF_BUDGET_CORPUS_V1[right * 2].n;
            const uint64_t left_hash = stable_n_hash(left_n);
            const uint64_t right_hash = stable_n_hash(right_n);
            return left_hash != right_hash ? left_hash < right_hash : left_n < right_n;
        });

        for (size_t rank = 0; rank < unique_indices.size(); ++rank) {
            const size_t first_row = unique_indices[rank] * 2;
            assignment[first_row] = CYCLE[rank % CYCLE.size()];
            assignment[first_row + 1] = assignment[first_row];
        }
    }
    return assignment;
}

[[nodiscard]] Digest split_digest(const SplitAssignment& assignment) {
    DigestBuilder builder("GNFS-SQUFOF-BUDGET-CORPUS-SPLIT-V1");
    builder.append_u32(1);
    builder.append_u64(SQUFOF_BUDGET_CORPUS_V1_DIGEST_LOW);
    builder.append_u64(SQUFOF_BUDGET_CORPUS_V1_DIGEST_HIGH);
    builder.append_u64(SQUFOF_BUDGET_CORPUS_V1.size());
    for (size_t row = 0; row < SQUFOF_BUDGET_CORPUS_V1.size(); ++row) {
        const auto& test_case = SQUFOF_BUDGET_CORPUS_V1[row];
        builder.append_u64(row);
        builder.append_u64(test_case.n);
        builder.append_byte(static_cast<uint8_t>(test_case.caller_path));
        builder.append_byte(static_cast<uint8_t>(test_case.bit_band));
        builder.append_u32(test_case.budget);
        builder.append_byte(static_cast<uint8_t>(assignment[row]));
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

void run_contract() {
    require(SQUFOF_BUDGET_CORPUS_V1.size() == SQUFOF_BUDGET_CORPUS_V1_ROW_COUNT,
            "prospective corpus row count changed");

    std::array<size_t, 3> unique_by_band{};
    std::array<size_t, 2> rows_by_path{};
    for (size_t unique_index = 0; unique_index < SQUFOF_BUDGET_CORPUS_V1_UNIQUE_CASE_COUNT;
         ++unique_index) {
        const size_t first_row = unique_index * 2;
        const auto& three_lp = SQUFOF_BUDGET_CORPUS_V1[first_row];
        const auto& normal_2lp = SQUFOF_BUDGET_CORPUS_V1[first_row + 1];
        require(is_prime_by_trial_division(static_cast<uint32_t>(three_lp.p)) &&
                    is_prime_by_trial_division(static_cast<uint32_t>(three_lp.q)),
                "prospective corpus contains non-prime factor metadata");
        require(three_lp.p < three_lp.q && three_lp.p * three_lp.q == three_lp.n,
                "prospective corpus factor metadata is not canonical");
        require(three_lp.n == normal_2lp.n && three_lp.p == normal_2lp.p &&
                    three_lp.q == normal_2lp.q && three_lp.bit_band == normal_2lp.bit_band,
                "caller-path rows do not share one immutable input");
        require(three_lp.caller_path == SqufofBudgetCallerPath::three_lp &&
                    normal_2lp.caller_path == SqufofBudgetCallerPath::normal_2lp,
                "caller-path pair order changed");
        ++unique_by_band[static_cast<size_t>(three_lp.bit_band)];
        ++rows_by_path[static_cast<size_t>(three_lp.caller_path)];
        ++rows_by_path[static_cast<size_t>(normal_2lp.caller_path)];
    }
    require(unique_by_band == std::array<size_t, 3>{{32, 32, 32}},
            "prospective corpus band balance changed");
    require(rows_by_path == std::array<size_t, 2>{{96, 96}},
            "prospective corpus caller-path balance changed");

    const SplitAssignment assignment = make_split_assignment();
    std::array<size_t, 3> unique_by_split{};
    std::array<std::array<size_t, 3>, 3> unique_by_split_and_band{};
    for (size_t unique_index = 0; unique_index < SQUFOF_BUDGET_CORPUS_V1_UNIQUE_CASE_COUNT;
         ++unique_index) {
        const size_t first_row = unique_index * 2;
        require(assignment[first_row] == assignment[first_row + 1],
                "same n leaked across prospective corpus splits");
        const size_t split = static_cast<size_t>(assignment[first_row]);
        const size_t band = static_cast<size_t>(SQUFOF_BUDGET_CORPUS_V1[first_row].bit_band);
        ++unique_by_split[split];
        ++unique_by_split_and_band[split][band];
    }
    require(unique_by_split == std::array<size_t, 3>{{48, 24, 24}},
            "prospective corpus split balance changed");
    require(unique_by_split_and_band[0] == std::array<size_t, 3>{{16, 16, 16}} &&
                unique_by_split_and_band[1] == std::array<size_t, 3>{{8, 8, 8}} &&
                unique_by_split_and_band[2] == std::array<size_t, 3>{{8, 8, 8}},
            "prospective corpus split-by-band balance changed");

    const Digest observed_split_digest = split_digest(assignment);
    constexpr Digest EXPECTED_SPLIT_DIGEST{UINT64_C(17722147925989565997),
                                           UINT64_C(4435973663510799258)};
    if (!(observed_split_digest == EXPECTED_SPLIT_DIGEST)) {
        std::cerr << "GNFS_SQUFOF_BUDGET_CORPUS_SPLIT_OBSERVED_V1 low=" << observed_split_digest.low
                  << " high=" << observed_split_digest.high << '\n';
        fail("prospective corpus split identity changed");
    }

    std::cout << "GNFS_SQUFOF_BUDGET_CORPUS_SUMMARY_V1"
              << " status=pass schema=1 rows=" << SQUFOF_BUDGET_CORPUS_V1.size()
              << " unique_inputs=" << SQUFOF_BUDGET_CORPUS_V1_UNIQUE_CASE_COUNT
              << " train_unique=" << unique_by_split[0]
              << " validation_unique=" << unique_by_split[1]
              << " holdout_unique=" << unique_by_split[2]
              << " corpus_digest_low=" << SQUFOF_BUDGET_CORPUS_V1_DIGEST_LOW
              << " corpus_digest_high=" << SQUFOF_BUDGET_CORPUS_V1_DIGEST_HIGH
              << " split_digest_low=" << observed_split_digest.low
              << " split_digest_high=" << observed_split_digest.high << '\n';
}

} // namespace

int main() {
    try {
        run_contract();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GNFS_SQUFOF_BUDGET_CORPUS_SUMMARY_V1 status=fail error="
                  << sanitize_token(error.what()) << '\n';
        return 1;
    } catch (...) {
        std::cerr << "GNFS_SQUFOF_BUDGET_CORPUS_SUMMARY_V1"
                     " status=fail error=unknown_exception\n";
        return 1;
    }
}
