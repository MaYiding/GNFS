// Fresh-process production SIQS probe for the sealed shadow-proof RSS holdout.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "fixtures/siqs_shadow_observe_rss_holdouts_v1.hpp"
#include "support/siqs_shadow_proof_rss_holdout_probe_protocol.hpp"

#include <gnfs/core/integer.hpp>
#include <gnfs/siqs/shadow_proof_observe.hpp>
#include <gnfs/siqs/siqs.hpp>
#include <gnfs/util/process_memory.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#endif

namespace {

using std::size_t;
using std::uint32_t;
using std::uint64_t;

using gnfs::core::Integer;
using gnfs::siqs::factor;
using gnfs::siqs::SIQS_SHADOW_PROOF_OBSERVE_ENV;
using gnfs::siqs::SIQSResult;
using gnfs::tests::fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_CORPUS_ID;
using gnfs::tests::fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_HIGH;
using gnfs::tests::fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_LOW;
using gnfs::tests::fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_FIXTURE_COUNT;
using gnfs::tests::fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_MAX_SECONDS;
using gnfs::tests::fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SEALED_BEFORE_MEASUREMENT;
using gnfs::tests::fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_USED_FOR_CALIBRATION;
using gnfs::tests::fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1;
using gnfs::tests::fixtures::SIQSShadowObserveRssHoldoutFixtureV1;
using gnfs::tests::support::emit_siqs_shadow_proof_rss_holdout_probe_record;
using gnfs::tests::support::parse_siqs_shadow_proof_rss_holdout_probe_options;
using gnfs::tests::support::SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_DIGITS;
using gnfs::tests::support::siqs_shadow_proof_rss_holdout_probe_environment_value;
using gnfs::tests::support::SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_ERROR_PREFIX;
using gnfs::tests::support::SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_MAX_SECONDS;
using gnfs::tests::support::siqs_shadow_proof_rss_holdout_probe_options_error_name;
using gnfs::tests::support::siqs_shadow_proof_rss_holdout_probe_record_error_name;
using gnfs::tests::support::SIQSShadowProofRssHoldoutProbeMode;
using gnfs::tests::support::SIQSShadowProofRssHoldoutProbeOptions;
using gnfs::tests::support::SIQSShadowProofRssHoldoutProbeRecord;
using gnfs::tests::support::SIQSShadowProofRssHoldoutProbeRecordError;
using gnfs::tests::support::validate_siqs_shadow_proof_rss_holdout_probe_record;
using gnfs::util::ProcessMemoryBackend;
using gnfs::util::ProcessMemorySnapshot;

#ifndef GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_BUILD_TYPE
#define GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_BUILD_TYPE "unknown"
#endif

constexpr std::string_view BUILD_TYPE = GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_BUILD_TYPE;
constexpr size_t ERROR_RECORD_CAPACITY = 1024;

#if defined(NDEBUG)
constexpr bool RELEASE_ASSERTIONS_DISABLED = true;
#else
constexpr bool RELEASE_ASSERTIONS_DISABLED = false;
#endif

static_assert(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SEALED_BEFORE_MEASUREMENT);
static_assert(!SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_USED_FOR_CALIBRATION);
static_assert(sizeof(size_t) <= sizeof(uint64_t));
static_assert(SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_ERROR_PREFIX.size() <=
              static_cast<size_t>(INT_MAX));
static_assert(SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_ERROR_PREFIX.size() + 2 <= ERROR_RECORD_CAPACITY);
static_assert(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_FIXTURE_COUNT == 8);
static_assert(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_MAX_SECONDS == 30);
static_assert(SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_MAX_SECONDS ==
              SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_MAX_SECONDS);
static_assert([]() constexpr {
    for (const auto& fixture : SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1) {
        if (fixture.modulus.size() != SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_DIGITS) {
            return false;
        }
    }
    return true;
}());
static_assert(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_CORPUS_ID ==
              gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_ID);
static_assert(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_LOW ==
              gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_LOW);
static_assert(SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_HIGH ==
              gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_HIGH);

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(std::string(message));
    }
}

void emit_error_record(std::string_view message) noexcept {
#if defined(_WIN32)
    std::array<char, ERROR_RECORD_CAPACITY> record{};
    size_t size = 0;
    const auto append_byte = [&record, &size](char byte) noexcept {
        if (size < record.size()) {
            record[size++] = byte;
        }
    };
    for (const char byte : SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_ERROR_PREFIX) {
        append_byte(byte);
    }
    append_byte(' ');
    for (const char character : message) {
        if (size + 1 >= record.size()) {
            break;
        }
        const auto byte = static_cast<unsigned char>(character);
        append_byte(byte >= 0x20U && byte <= 0x7eU ? static_cast<char>(byte) : '?');
    }
    append_byte('\n');

    const HANDLE error_handle = ::GetStdHandle(STD_ERROR_HANDLE);
    if (error_handle == nullptr || error_handle == INVALID_HANDLE_VALUE) {
        return;
    }
    size_t offset = 0;
    while (offset < size) {
        DWORD written = 0;
        const DWORD remaining = static_cast<DWORD>(size - offset);
        if (::WriteFile(error_handle, record.data() + offset, remaining, &written, nullptr) == 0 ||
            written == 0) {
            return;
        }
        offset += static_cast<size_t>(written);
    }
#else
    (void)std::fprintf(stderr, "%.*s ",
                       static_cast<int>(SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_ERROR_PREFIX.size()),
                       SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_ERROR_PREFIX.data());
    for (const char character : message) {
        const auto byte = static_cast<unsigned char>(character);
        (void)std::fputc(byte >= 0x20U && byte <= 0x7eU ? static_cast<int>(byte) : '?', stderr);
    }
    (void)std::fputc('\n', stderr);
    (void)std::fflush(stderr);
#endif
}

void write_stdout_record(std::string_view record) {
    require(!record.empty() && record.back() == '\n',
            "protocol emitter did not produce one LF-terminated record");
    const size_t written = std::fwrite(record.data(), 1, record.size(), stdout);
    require(written == record.size() && std::fflush(stdout) == 0 && std::ferror(stdout) == 0,
            "failed to commit the probe record to stdout");
}

void configure_standard_streams() {
#if defined(_WIN32)
    require(::_setmode(::_fileno(stderr), _O_BINARY) != -1, "failed to set stderr to binary mode");
    require(::_setmode(::_fileno(stdout), _O_BINARY) != -1, "failed to set stdout to binary mode");
#endif
}

void set_shadow_mode_environment(SIQSShadowProofRssHoldoutProbeMode mode) {
    const std::string_view value = siqs_shadow_proof_rss_holdout_probe_environment_value(mode);
    require(mode != SIQSShadowProofRssHoldoutProbeMode::unknown, "probe mode is unknown");
#if defined(_WIN32)
    if (::_putenv_s(SIQS_SHADOW_PROOF_OBSERVE_ENV, value.data()) != 0) {
        fail("failed to set GNFS_SIQS_SHADOW_PROOF");
    }
#else
    if (::setenv(SIQS_SHADOW_PROOF_OBSERVE_ENV, value.data(), 1) != 0) {
        fail("failed to set GNFS_SIQS_SHADOW_PROOF");
    }
#endif
}

[[nodiscard]] uint64_t elapsed_wall_ns(std::chrono::steady_clock::time_point before,
                                       std::chrono::steady_clock::time_point after) noexcept {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count();
    if (elapsed <= 0) {
        return 0;
    }
    using Elapsed = std::remove_cv_t<decltype(elapsed)>;
    using UnsignedElapsed = std::make_unsigned_t<Elapsed>;
    const auto unsigned_elapsed = static_cast<UnsignedElapsed>(elapsed);
    if constexpr (std::numeric_limits<UnsignedElapsed>::digits >
                  std::numeric_limits<uint64_t>::digits) {
        if (unsigned_elapsed > static_cast<UnsignedElapsed>(std::numeric_limits<uint64_t>::max())) {
            return std::numeric_limits<uint64_t>::max();
        }
    }
    return static_cast<uint64_t>(unsigned_elapsed);
}

struct PeakGrowth final {
    bool supported = false;
    uint64_t bytes = 0;
};

[[nodiscard]] PeakGrowth peak_growth(const ProcessMemorySnapshot& before,
                                     const ProcessMemorySnapshot& after) noexcept {
    if (!before.lifetime_peak_rss_bytes.has_value() || !after.lifetime_peak_rss_bytes.has_value() ||
        *after.lifetime_peak_rss_bytes < *before.lifetime_peak_rss_bytes) {
        return {};
    }
    return {true, *after.lifetime_peak_rss_bytes - *before.lifetime_peak_rss_bytes};
}

struct ProbeEvidence final {
    SIQSShadowProofRssHoldoutProbeOptions options;
    SIQSShadowObserveRssHoldoutFixtureV1 fixture;
    Integer factor;
    Integer cofactor;
    size_t relations_found = 0;
    size_t polynomials_used = 0;
    unsigned resolved_sieve_workers = 0;
    uint64_t factor_wall_ns = 0;
    ProcessMemorySnapshot before_memory;
    ProcessMemorySnapshot after_memory;
    PeakGrowth growth;
};

[[nodiscard]] const SIQSShadowObserveRssHoldoutFixtureV1& fixture_for_id(uint32_t fixture_id) {
    require(fixture_id > 0 && fixture_id <= SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1.size(),
            "fixture id is outside the sealed corpus");
    const auto& fixture = SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1[fixture_id - 1];
    require(fixture.id == fixture_id, "sealed fixture id does not match its corpus position");
    return fixture;
}

[[nodiscard]] ProbeEvidence run_probe(const SIQSShadowProofRssHoldoutProbeOptions& options) {
    require(BUILD_TYPE == "Release",
            "probe requires a Release build; observed " + std::string(BUILD_TYPE));
    require(RELEASE_ASSERTIONS_DISABLED, "probe requires NDEBUG to be defined");

    const SIQSShadowObserveRssHoldoutFixtureV1& fixture = fixture_for_id(options.fixture_id);
    const Integer modulus(std::string(fixture.modulus));
    const Integer expected_factor(std::string(fixture.factor_p));
    const Integer expected_cofactor(std::string(fixture.factor_q));
    require(expected_factor <= expected_cofactor,
            "sealed fixture factors are not in canonical order");
    require(expected_factor * expected_cofactor == modulus,
            "sealed fixture factors do not multiply to the modulus");

    set_shadow_mode_environment(options.mode);
    const ProcessMemorySnapshot before_memory = gnfs::util::process_memory_snapshot();
    require(before_memory.backend != ProcessMemoryBackend::Unsupported,
            "process RSS backend is unsupported");

    const auto wall_before = std::chrono::steady_clock::now();
    std::optional<SIQSResult> result =
        factor(modulus, SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_MAX_SECONDS, false);
    const auto wall_after = std::chrono::steady_clock::now();
    const ProcessMemorySnapshot after_memory = gnfs::util::process_memory_snapshot();

    require(after_memory.backend == before_memory.backend,
            "process RSS backend changed during the factor call");
    require(after_memory.lifetime_peak_rss_bytes.has_value(),
            "complete factor process peak RSS is unavailable");
    require(*after_memory.lifetime_peak_rss_bytes > 0, "complete factor process peak RSS is zero");
    if (before_memory.lifetime_peak_rss_bytes.has_value()) {
        require(*after_memory.lifetime_peak_rss_bytes >= *before_memory.lifetime_peak_rss_bytes,
                "complete factor process peak RSS moved backwards");
    }

    require(result.has_value(), "production SIQS did not factor the sealed fixture");
    require(result->relations_found > 0, "production SIQS returned zero relations");
    require(result->polynomials_used > 0, "production SIQS returned zero polynomials");
    require(result->resolved_sieve_workers > 0,
            "production SIQS returned zero resolved sieve workers");
    if (options.mode == SIQSShadowProofRssHoldoutProbeMode::observe) {
        require(result->shadow_proof_observe_record_committed,
                "production SIQS did not commit the observe record to stderr");
    } else {
        require(!result->shadow_proof_observe_record_committed,
                "disabled shadow proof reported an observe record on stderr");
    }

    Integer observed_factor = std::move(result->factor1);
    Integer observed_cofactor = std::move(result->factor2);
    if (observed_cofactor < observed_factor) {
        std::swap(observed_factor, observed_cofactor);
    }
    require(observed_factor == expected_factor && observed_cofactor == expected_cofactor,
            "production SIQS returned a noncanonical fixture factorization");
    require(observed_factor * observed_cofactor == modulus,
            "production SIQS factors do not multiply to the fixture modulus");

    const uint64_t wall_ns = elapsed_wall_ns(wall_before, wall_after);
    require(wall_ns > 0, "production SIQS wall time is not positive");
    return ProbeEvidence{options,
                         fixture,
                         std::move(observed_factor),
                         std::move(observed_cofactor),
                         result->relations_found,
                         result->polynomials_used,
                         result->resolved_sieve_workers,
                         wall_ns,
                         before_memory,
                         after_memory,
                         peak_growth(before_memory, after_memory)};
}

[[nodiscard]] uint64_t rss_bytes(const std::optional<uint64_t>& value) noexcept {
    return value.value_or(0);
}

[[nodiscard]] SIQSShadowProofRssHoldoutProbeRecord
make_record(const ProbeEvidence& evidence, const std::string& factor, const std::string& cofactor) {
    SIQSShadowProofRssHoldoutProbeRecord record;
    record.corpus_id = SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_CORPUS_ID;
    record.corpus_digest_low = SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_LOW;
    record.corpus_digest_high = SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_HIGH;
    record.sealed_before_measurement = SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_SEALED_BEFORE_MEASUREMENT;
    record.used_for_calibration = SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_USED_FOR_CALIBRATION;
    record.fixture_id = evidence.options.fixture_id;
    record.mode = evidence.options.mode;
    record.ordinal = evidence.options.ordinal;
    record.build_type = BUILD_TYPE;
    record.ndebug = RELEASE_ASSERTIONS_DISABLED;
    record.fresh_process = true;
    record.completed = true;
    record.environment_value =
        siqs_shadow_proof_rss_holdout_probe_environment_value(evidence.options.mode);
    record.digits = static_cast<uint32_t>(evidence.fixture.modulus.size());
    record.modulus = evidence.fixture.modulus;
    record.expected_factor = evidence.fixture.factor_p;
    record.expected_cofactor = evidence.fixture.factor_q;
    record.max_seconds = SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_MAX_SECONDS;
    record.factor = factor;
    record.cofactor = cofactor;
    record.relations_found = static_cast<uint64_t>(evidence.relations_found);
    record.polynomials_used = static_cast<uint64_t>(evidence.polynomials_used);
    record.resolved_production_sieve_workers = evidence.resolved_sieve_workers;
    record.factor_wall_ns = evidence.factor_wall_ns;
    record.rss_backend = gnfs::util::process_memory_backend_name(evidence.before_memory.backend);
    record.before_current_rss_supported = evidence.before_memory.current_rss_bytes.has_value();
    record.before_current_rss_bytes = rss_bytes(evidence.before_memory.current_rss_bytes);
    record.before_peak_rss_supported = evidence.before_memory.lifetime_peak_rss_bytes.has_value();
    record.before_peak_rss_bytes = rss_bytes(evidence.before_memory.lifetime_peak_rss_bytes);
    record.after_current_rss_supported = evidence.after_memory.current_rss_bytes.has_value();
    record.after_current_rss_bytes = rss_bytes(evidence.after_memory.current_rss_bytes);
    record.after_peak_rss_supported = evidence.after_memory.lifetime_peak_rss_bytes.has_value();
    record.after_peak_rss_bytes = rss_bytes(evidence.after_memory.lifetime_peak_rss_bytes);
    record.absolute_peak_rss_supported = true;
    record.absolute_peak_rss_bytes = *evidence.after_memory.lifetime_peak_rss_bytes;
    record.peak_growth_supported = evidence.growth.supported;
    record.peak_growth_bytes = evidence.growth.bytes;
    return record;
}

void emit_probe_record(const ProbeEvidence& evidence) {
    const std::string factor = evidence.factor.to_string();
    const std::string cofactor = evidence.cofactor.to_string();
    const SIQSShadowProofRssHoldoutProbeRecord record = make_record(evidence, factor, cofactor);
    const SIQSShadowProofRssHoldoutProbeRecordError record_error =
        validate_siqs_shadow_proof_rss_holdout_probe_record(record);
    if (record_error != SIQSShadowProofRssHoldoutProbeRecordError::none) {
        fail("probe record validation failed: " +
             std::string(siqs_shadow_proof_rss_holdout_probe_record_error_name(record_error)));
    }

    std::string output;
    require(emit_siqs_shadow_proof_rss_holdout_probe_record(record, output),
            "probe record emission failed");
    write_stdout_record(output);
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        configure_standard_streams();

        std::vector<std::string_view> arguments;
        if (argc > 1) {
            arguments.reserve(static_cast<size_t>(argc - 1));
        }
        for (int index = 1; index < argc; ++index) {
            require(argv[index] != nullptr, "null command-line argument");
            arguments.emplace_back(argv[index]);
        }

        const auto parsed = parse_siqs_shadow_proof_rss_holdout_probe_options(arguments);
        if (!parsed) {
            std::string message = "invalid command line: ";
            message.append(siqs_shadow_proof_rss_holdout_probe_options_error_name(parsed.error));
            if (!parsed.argument.empty()) {
                message.append(" argument=");
                message.append(parsed.argument);
            }
            fail(std::move(message));
        }

        const ProbeEvidence evidence = run_probe(parsed.options);
        emit_probe_record(evidence);
        return 0;
    } catch (const std::exception& error) {
        emit_error_record(error.what());
    } catch (...) {
        emit_error_record("unknown failure");
    }
    return 1;
}
