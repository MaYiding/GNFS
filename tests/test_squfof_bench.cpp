#include <gnfs/cofactor/squfof.hpp>

#include "fixtures/squfof_strategy_corpus_v1.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using gnfs::cofactor::SQUFOF;
using gnfs::tests::fixtures::FIXED_50D_SQUFOF_STRATEGY_V1;
using gnfs::tests::fixtures::FIXED_50D_SQUFOF_STRATEGY_V1_DIGEST_HIGH;
using gnfs::tests::fixtures::FIXED_50D_SQUFOF_STRATEGY_V1_DIGEST_LOW;
using gnfs::tests::fixtures::FIXED_50D_SQUFOF_STRATEGY_V1_NAME;
using gnfs::tests::fixtures::SqufofStrategyCase;

constexpr size_t DEFAULT_REPETITIONS = 3;
constexpr size_t MIN_REPETITIONS = 1;
constexpr size_t MAX_REPETITIONS = 9;
constexpr std::string_view BENCH_SCOPE = "fixed_50d_strategy_corpus";
constexpr std::string_view CLAIM_BOUNDARY = "whole_squfof_factor_call";
constexpr std::string_view TIMING_SCOPE = "factor_calls_plus_preallocated_result_store";

#ifndef GNFS_SQUFOF_BENCH_BUILD_TYPE
#define GNFS_SQUFOF_BENCH_BUILD_TYPE "unknown"
#endif

constexpr std::string_view BENCH_BUILD_TYPE = GNFS_SQUFOF_BENCH_BUILD_TYPE;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

struct Digest final {
    uint64_t low = 0;
    uint64_t high = 0;

    [[nodiscard]] bool operator==(const Digest&) const noexcept = default;
};

class DigestBuilder final {
public:
    DigestBuilder() = default;

    explicit DigestBuilder(std::string_view domain) {
        append_u64(static_cast<uint64_t>(domain.size()));
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

[[nodiscard]] uint64_t isqrt(uint64_t n) {
    if (n == 0) {
        return 0;
    }
    uint64_t root = static_cast<uint64_t>(std::sqrt(static_cast<double>(n)));
    root = std::min<uint64_t>(root, UINT32_MAX);
    while (root > 0 && root * root > n) {
        --root;
    }
    while (root < UINT32_MAX && (root + 1) * (root + 1) <= n) {
        ++root;
    }
    return root;
}

[[nodiscard]] Digest corpus_digest(std::span<const SqufofStrategyCase> corpus) {
    DigestBuilder builder;
    for (const char character : FIXED_50D_SQUFOF_STRATEGY_V1_NAME) {
        builder.append_byte(static_cast<uint8_t>(character));
    }
    builder.append_byte(0);
    builder.append_u64(static_cast<uint64_t>(corpus.size()));
    for (size_t index = 0; index < corpus.size(); ++index) {
        builder.append_u64(static_cast<uint64_t>(index));
        builder.append_u64(corpus[index].n);
        builder.append_u32(corpus[index].max_iterations);
    }
    return builder.finish();
}

template <typename Schedule> [[nodiscard]] Digest schedule_digest(const Schedule& schedule) {
    DigestBuilder builder("GNFS-SQUFOF-MULTIPLIER-SCHEDULE-V1");
    builder.append_u64(static_cast<uint64_t>(schedule.size()));
    for (size_t index = 0; index < schedule.size(); ++index) {
        builder.append_u64(static_cast<uint64_t>(index));
        builder.append_u64(schedule[index]);
    }
    return builder.finish();
}

struct ResultIdentity final {
    size_t successes = 0;
    size_t failures = 0;
    size_t invalid_factors = 0;
    Digest factor_digest;
    Digest success_digest;
    Digest failure_digest;
    std::string failure_bitmap_hex;

    [[nodiscard]] bool operator==(const ResultIdentity&) const noexcept = default;
};

[[nodiscard]] ResultIdentity result_identity(std::span<const SqufofStrategyCase> corpus,
                                             std::span<const uint64_t> factors) {
    require(corpus.size() == factors.size(), "result count differs from the strategy corpus");

    DigestBuilder factors_builder("GNFS-SQUFOF-FACTOR-RESULTS-V1");
    DigestBuilder successes_builder("GNFS-SQUFOF-SUCCESS-SET-V1");
    DigestBuilder failures_builder("GNFS-SQUFOF-FAILURE-SET-V1");
    factors_builder.append_u64(static_cast<uint64_t>(corpus.size()));

    ResultIdentity identity;
    identity.failure_bitmap_hex.reserve((corpus.size() + 3) / 4);
    constexpr std::string_view HEX_DIGITS = "0123456789abcdef";

    for (size_t group_start = 0; group_start < corpus.size(); group_start += 4) {
        unsigned nibble = 0;
        for (size_t offset = 0; offset < 4 && group_start + offset < corpus.size(); ++offset) {
            const size_t index = group_start + offset;
            const SqufofStrategyCase& test_case = corpus[index];
            const uint64_t factor = factors[index];
            const bool failed = factor == 1;
            const bool proper = factor > 1 && factor < test_case.n && test_case.n % factor == 0;

            factors_builder.append_u64(static_cast<uint64_t>(index));
            factors_builder.append_u64(test_case.n);
            factors_builder.append_u32(test_case.max_iterations);
            factors_builder.append_u64(factor);

            if (failed) {
                ++identity.failures;
                nibble |= 1u << offset;
                failures_builder.append_u64(static_cast<uint64_t>(index));
                failures_builder.append_u64(test_case.n);
                failures_builder.append_u32(test_case.max_iterations);
            } else if (proper) {
                ++identity.successes;
                successes_builder.append_u64(static_cast<uint64_t>(index));
                successes_builder.append_u64(test_case.n);
                successes_builder.append_u32(test_case.max_iterations);
            } else {
                ++identity.invalid_factors;
            }
        }
        identity.failure_bitmap_hex.push_back(HEX_DIGITS[nibble]);
    }

    successes_builder.append_u64(static_cast<uint64_t>(identity.successes));
    failures_builder.append_u64(static_cast<uint64_t>(identity.failures));
    identity.factor_digest = factors_builder.finish();
    identity.success_digest = successes_builder.finish();
    identity.failure_digest = failures_builder.finish();
    return identity;
}

void validate_corpus() {
    require(!FIXED_50D_SQUFOF_STRATEGY_V1.empty(), "strategy corpus is empty");
    for (size_t index = 0; index < FIXED_50D_SQUFOF_STRATEGY_V1.size(); ++index) {
        const SqufofStrategyCase& test_case = FIXED_50D_SQUFOF_STRATEGY_V1[index];
        require(test_case.n > 1,
                "strategy corpus contains n <= 1 at index " + std::to_string(index));
        require((test_case.n & 1) == 1,
                "strategy corpus contains an even n at index " + std::to_string(index));
        require(test_case.n < (UINT64_C(1) << 62),
                "strategy corpus exceeds the supported SQUFOF range at index " +
                    std::to_string(index));
        const uint64_t root = isqrt(test_case.n);
        require(root * root != test_case.n,
                "strategy corpus contains a square at index " + std::to_string(index));
        require(test_case.max_iterations > 0,
                "strategy corpus contains an automatic iteration budget at index " +
                    std::to_string(index));
    }

    const Digest observed = corpus_digest(FIXED_50D_SQUFOF_STRATEGY_V1);
    require(observed.low == FIXED_50D_SQUFOF_STRATEGY_V1_DIGEST_LOW &&
                observed.high == FIXED_50D_SQUFOF_STRATEGY_V1_DIGEST_HIGH,
            "strategy corpus digest differs from its frozen V1 identity");
}

void run_factor_pass(std::span<uint64_t> results) {
    require(results.size() == FIXED_50D_SQUFOF_STRATEGY_V1.size(),
            "factor result storage has the wrong size");
    for (size_t index = 0; index < FIXED_50D_SQUFOF_STRATEGY_V1.size(); ++index) {
        const SqufofStrategyCase& test_case = FIXED_50D_SQUFOF_STRATEGY_V1[index];
        results[index] = SQUFOF::factor(test_case.n, test_case.max_iterations);
    }
}

[[nodiscard]] uint64_t median_sample(std::vector<uint64_t> samples) {
    require(!samples.empty(), "cannot compute a median for no samples");
    std::sort(samples.begin(), samples.end());
    const size_t upper_index = samples.size() / 2;
    if (samples.size() % 2 != 0) {
        return samples[upper_index];
    }
    return samples[upper_index - 1] + (samples[upper_index] - samples[upper_index - 1]) / 2;
}

void emit_case_record(size_t repetition, uint64_t wall_ns, const Digest& corpus_id,
                      const Digest& schedule_id, const ResultIdentity& identity) {
    std::cout << "GNFS_SQUFOF_BENCH_CASE_V1" << " status=pass" << " scope=" << BENCH_SCOPE
              << " corpus=" << FIXED_50D_SQUFOF_STRATEGY_V1_NAME
              << " build_type=" << BENCH_BUILD_TYPE << " timing_scope=" << TIMING_SCOPE
              << " timing_asserted=false" << " diagnostics_timed=false"
              << " repetition=" << repetition << " cases=" << FIXED_50D_SQUFOF_STRATEGY_V1.size()
              << " calls=" << FIXED_50D_SQUFOF_STRATEGY_V1.size()
              << " successes=" << identity.successes << " failures=" << identity.failures
              << " invalid_factors=" << identity.invalid_factors << " wall_ns=" << wall_ns
              << " corpus_digest_low=" << corpus_id.low << " corpus_digest_high=" << corpus_id.high
              << " factor_digest_low=" << identity.factor_digest.low
              << " factor_digest_high=" << identity.factor_digest.high
              << " success_digest_low=" << identity.success_digest.low
              << " success_digest_high=" << identity.success_digest.high
              << " failure_digest_low=" << identity.failure_digest.low
              << " failure_digest_high=" << identity.failure_digest.high
              << " failure_bitmap_hex=" << identity.failure_bitmap_hex
              << " schedule_digest_low=" << schedule_id.low
              << " schedule_digest_high=" << schedule_id.high << '\n';
}

template <typename Schedule>
void emit_multiplier_records(const Schedule& schedule, const SQUFOF::Diagnostics& diagnostics,
                             const Digest& schedule_id) {
    require(diagnostics.slots.size() == schedule.size(),
            "diagnostics slot count differs from the multiplier schedule");
    for (size_t slot = 0; slot < schedule.size(); ++slot) {
        const auto& stats = diagnostics.slots[slot];
        std::cout << "GNFS_SQUFOF_BENCH_MULTIPLIER_V1" << " status=pass" << " scope=" << BENCH_SCOPE
                  << " corpus=" << FIXED_50D_SQUFOF_STRATEGY_V1_NAME
                  << " build_type=" << BENCH_BUILD_TYPE << " diagnostics_timed=false"
                  << " slot=" << slot << " multiplier=" << schedule[slot]
                  << " attempts=" << stats.attempts
                  << " forward_iterations=" << stats.forward_iterations
                  << " core_hits=" << stats.core_hits << " accepted_hits=" << stats.accepted_hits
                  << " overflow_skips=" << stats.overflow_skips
                  << " schedule_digest_low=" << schedule_id.low
                  << " schedule_digest_high=" << schedule_id.high << '\n';
    }
}

void emit_summary_record(size_t repetitions, std::span<const uint64_t> wall_samples,
                         size_t multiplier_count, const Digest& corpus_id,
                         const Digest& schedule_id, const ResultIdentity& identity) {
    require(wall_samples.size() == repetitions, "summary has an incomplete timing sample set");
    const auto [minimum, maximum] = std::minmax_element(wall_samples.begin(), wall_samples.end());

    std::cout << "GNFS_SQUFOF_BENCH_SUMMARY_V1" << " status=pass" << " scope=" << BENCH_SCOPE
              << " corpus=" << FIXED_50D_SQUFOF_STRATEGY_V1_NAME
              << " build_type=" << BENCH_BUILD_TYPE << " claim_boundary=" << CLAIM_BOUNDARY
              << " timing_scope=" << TIMING_SCOPE << " timing_asserted=false"
              << " diagnostics_timed=false" << " cases=" << FIXED_50D_SQUFOF_STRATEGY_V1.size()
              << " repetitions=" << repetitions
              << " measured_calls=" << FIXED_50D_SQUFOF_STRATEGY_V1.size() * repetitions
              << " successes_per_repetition=" << identity.successes
              << " failures_per_repetition=" << identity.failures
              << " invalid_factors=" << identity.invalid_factors << " wall_min_ns=" << *minimum
              << " wall_median_ns="
              << median_sample(std::vector<uint64_t>(wall_samples.begin(), wall_samples.end()))
              << " wall_max_ns=" << *maximum << " multiplier_count=" << multiplier_count
              << " corpus_digest_low=" << corpus_id.low << " corpus_digest_high=" << corpus_id.high
              << " factor_digest_low=" << identity.factor_digest.low
              << " factor_digest_high=" << identity.factor_digest.high
              << " success_digest_low=" << identity.success_digest.low
              << " success_digest_high=" << identity.success_digest.high
              << " failure_digest_low=" << identity.failure_digest.low
              << " failure_digest_high=" << identity.failure_digest.high
              << " failure_bitmap_hex=" << identity.failure_bitmap_hex
              << " schedule_digest_low=" << schedule_id.low
              << " schedule_digest_high=" << schedule_id.high << '\n';
}

void run_benchmark(size_t repetitions, std::string& failure_stage) {
    failure_stage = "build_contract";
    require(BENCH_BUILD_TYPE == "Release", "SQUFOF benchmark requires a Release build");

    failure_stage = "corpus";
    validate_corpus();
    const Digest corpus_id = corpus_digest(FIXED_50D_SQUFOF_STRATEGY_V1);
    const auto schedule = SQUFOF::multiplier_schedule();
    require(!schedule.empty(), "SQUFOF multiplier schedule is empty");
    const Digest schedule_id = schedule_digest(schedule);

    failure_stage = "warmup";
    std::vector<uint64_t> factors(FIXED_50D_SQUFOF_STRATEGY_V1.size());
    run_factor_pass(factors);
    const ResultIdentity reference = result_identity(FIXED_50D_SQUFOF_STRATEGY_V1, factors);
    require(reference.invalid_factors == 0, "warmup returned an invalid factor");

    failure_stage = "timed_factor_calls";
    std::vector<uint64_t> wall_samples;
    wall_samples.reserve(repetitions);
    for (size_t repetition = 1; repetition <= repetitions; ++repetition) {
        const auto started = std::chrono::steady_clock::now();
        run_factor_pass(factors);
        const auto finished = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();
        require(elapsed >= 0, "benchmark clock moved backwards");

        const ResultIdentity identity = result_identity(FIXED_50D_SQUFOF_STRATEGY_V1, factors);
        require(identity == reference, "factor identity changed across repetitions");
        wall_samples.push_back(static_cast<uint64_t>(elapsed));
        emit_case_record(repetition, wall_samples.back(), corpus_id, schedule_id, identity);
    }

    failure_stage = "diagnostics";
    SQUFOF::Diagnostics diagnostics;
    std::vector<uint64_t> diagnostic_factors(FIXED_50D_SQUFOF_STRATEGY_V1.size());
    for (size_t index = 0; index < FIXED_50D_SQUFOF_STRATEGY_V1.size(); ++index) {
        const SqufofStrategyCase& test_case = FIXED_50D_SQUFOF_STRATEGY_V1[index];
        diagnostic_factors[index] =
            SQUFOF::factor_with_diagnostics(test_case.n, test_case.max_iterations, diagnostics);
    }
    const ResultIdentity diagnostic_identity =
        result_identity(FIXED_50D_SQUFOF_STRATEGY_V1, diagnostic_factors);
    require(diagnostic_identity == reference,
            "diagnostics path changed the factor or failure identity");
    require(diagnostics.factor_calls == FIXED_50D_SQUFOF_STRATEGY_V1.size(),
            "diagnostics factor-call total differs from the corpus size");
    require(diagnostics.trivial_input_hits == 0 && diagnostics.even_fast_path_hits == 0 &&
                diagnostics.square_fast_path_hits == 0,
            "strategy corpus unexpectedly used a preprocessing fast path");

    uint64_t accepted_hits = 0;
    for (size_t slot = 0; slot < diagnostics.slots.size(); ++slot) {
        const auto& stats = diagnostics.slots[slot];
        require(stats.multiplier == schedule[slot],
                "diagnostics slot label differs from the production schedule");
        require(stats.core_hits >= stats.accepted_hits,
                "diagnostics accepted more hits than the core produced");
        require(stats.attempts >= stats.accepted_hits,
                "diagnostics accepted more hits than multiplier attempts");
        accepted_hits += stats.accepted_hits;
    }
    require(accepted_hits == reference.successes,
            "diagnostics accepted-hit total differs from successful calls");

    emit_multiplier_records(schedule, diagnostics, schedule_id);
    emit_summary_record(repetitions, wall_samples, schedule.size(), corpus_id, schedule_id,
                        reference);
    failure_stage = "none";
}

struct CliOptions final {
    size_t repetitions = DEFAULT_REPETITIONS;
    bool help = false;
};

[[nodiscard]] size_t parse_repetitions(std::string_view text) {
    uint64_t parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [position, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || position != end || parsed < MIN_REPETITIONS ||
        parsed > MAX_REPETITIONS) {
        throw std::invalid_argument("repetitions must be an integer in [1,9]");
    }
    return static_cast<size_t>(parsed);
}

[[nodiscard]] CliOptions parse_cli(int argc, char** argv) {
    if (argc == 1) {
        return {};
    }
    if (argc == 2) {
        const std::string_view argument(argv[1]);
        if (argument == "--help") {
            return CliOptions{.help = true};
        }
        return CliOptions{.repetitions = parse_repetitions(argument)};
    }
    throw std::invalid_argument("usage: test_squfof_bench [repetitions|--help]");
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

} // namespace

int main(int argc, char** argv) {
    std::string failure_stage = "cli";
    try {
        const CliOptions options = parse_cli(argc, argv);
        if (options.help) {
            std::cout << "Usage: test_squfof_bench [repetitions]\n"
                         "  repetitions: integer in [1,9], default 3\n";
            return 0;
        }
        run_benchmark(options.repetitions, failure_stage);
        return 0;
    } catch (const std::exception& error) {
        std::cout << "GNFS_SQUFOF_BENCH_SUMMARY_V1" << " status=fail" << " scope=" << BENCH_SCOPE
                  << " corpus=" << FIXED_50D_SQUFOF_STRATEGY_V1_NAME
                  << " build_type=" << BENCH_BUILD_TYPE << " claim_boundary=" << CLAIM_BOUNDARY
                  << " timing_scope=" << TIMING_SCOPE
                  << " failure_stage=" << sanitize_token(failure_stage)
                  << " error=" << sanitize_token(error.what()) << '\n';
        return 1;
    } catch (...) {
        std::cout << "GNFS_SQUFOF_BENCH_SUMMARY_V1" << " status=fail" << " scope=" << BENCH_SCOPE
                  << " corpus=" << FIXED_50D_SQUFOF_STRATEGY_V1_NAME
                  << " build_type=" << BENCH_BUILD_TYPE << " claim_boundary=" << CLAIM_BOUNDARY
                  << " timing_scope=" << TIMING_SCOPE
                  << " failure_stage=" << sanitize_token(failure_stage)
                  << " error=unknown_exception\n";
        return 1;
    }
}
