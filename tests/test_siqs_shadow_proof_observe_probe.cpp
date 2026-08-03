// test_siqs_shadow_proof_observe_probe.cpp - fresh-process production SIQS observe probe

#include "fixtures/siqs_live_sieve_fixtures_v1.hpp"

#include <gnfs/core/integer.hpp>
#include <gnfs/siqs/shadow_proof_observe.hpp>
#include <gnfs/siqs/siqs.hpp>
#include <gnfs/util/process_memory.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <stdlib.h>
#endif

namespace {

using std::size_t;
using std::uint32_t;
using std::uint64_t;

using gnfs::core::Integer;
using gnfs::siqs::factor;
using gnfs::siqs::SIQS_SHADOW_PROOF_ENV;
using gnfs::siqs::SIQSResult;
using gnfs::tests::siqs_live_sieve_fixture_v1;
using gnfs::tests::SIQSLiveSieveFixtureV1;
using gnfs::util::ProcessMemoryBackend;
using gnfs::util::ProcessMemorySnapshot;

#ifndef GNFS_SIQS_SHADOW_PROOF_OBSERVE_PROBE_BUILD_TYPE
#define GNFS_SIQS_SHADOW_PROOF_OBSERVE_PROBE_BUILD_TYPE "unknown"
#endif

constexpr std::string_view BUILD_TYPE = GNFS_SIQS_SHADOW_PROOF_OBSERVE_PROBE_BUILD_TYPE;
#if defined(NDEBUG)
constexpr bool RELEASE_ASSERTIONS_DISABLED = true;
#else
constexpr bool RELEASE_ASSERTIONS_DISABLED = false;
#endif

constexpr uint32_t FIXTURE_BAND = 50;
constexpr size_t FACTOR_MAX_SECONDS = 30;
constexpr std::string_view PROFILE_ID = "siqs50_production_shadow_observe_v1";
constexpr std::string_view PROBE_PREFIX = "GNFS_SIQS_SHADOW_PROOF_OBSERVE_PROBE_V1";
constexpr std::string_view ERROR_PREFIX = "GNFS_SIQS_SHADOW_PROOF_OBSERVE_PROBE_ERROR_V1";

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(std::string(message));
    }
}

enum class ProbeMode : uint8_t {
    off,
    observe,
};

struct ProbeOptions final {
    ProbeMode mode = ProbeMode::off;
    uint32_t sample_ordinal = 0;
};

[[nodiscard]] constexpr std::string_view mode_name(ProbeMode mode) noexcept {
    switch (mode) {
    case ProbeMode::off:
        return "off";
    case ProbeMode::observe:
        return "observe";
    }
    return "unknown";
}

[[nodiscard]] constexpr const char* environment_value(ProbeMode mode) noexcept {
    return mode == ProbeMode::observe ? "observe" : "0";
}

[[nodiscard]] uint32_t parse_sample_ordinal(std::string_view text) {
    uint32_t value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [position, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || position != end || value > 3) {
        fail("--sample-ordinal must be an integer in 0..3");
    }
    return value;
}

[[nodiscard]] ProbeOptions parse_options(int argc, char** argv) {
    ProbeOptions options;
    bool have_mode = false;
    bool have_sample_ordinal = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--mode") {
            require(!have_mode, "--mode was provided more than once");
            require(index + 1 < argc, "--mode requires off or observe");
            const std::string_view value(argv[++index]);
            if (value == "off") {
                options.mode = ProbeMode::off;
            } else if (value == "observe") {
                options.mode = ProbeMode::observe;
            } else {
                fail("--mode must be exactly off or observe");
            }
            have_mode = true;
        } else if (argument == "--sample-ordinal") {
            require(!have_sample_ordinal, "--sample-ordinal was provided more than once");
            require(index + 1 < argc, "--sample-ordinal requires a value");
            options.sample_ordinal = parse_sample_ordinal(argv[++index]);
            have_sample_ordinal = true;
        } else {
            fail("unknown argument: " + std::string(argument));
        }
    }
    require(have_mode, "missing required --mode off|observe");
    require(have_sample_ordinal, "missing required --sample-ordinal 0..3");
    return options;
}

void set_shadow_mode_environment(ProbeMode mode) {
#if defined(_WIN32)
    if (::_putenv_s(SIQS_SHADOW_PROOF_ENV, environment_value(mode)) != 0) {
        fail("failed to set GNFS_SIQS_SHADOW_PROOF");
    }
#else
    if (::setenv(SIQS_SHADOW_PROOF_ENV, environment_value(mode), 1) != 0) {
        fail("failed to set GNFS_SIQS_SHADOW_PROOF");
    }
#endif
}

[[nodiscard]] constexpr const char* bool_name(bool value) noexcept {
    return value ? "true" : "false";
}

[[nodiscard]] constexpr uint64_t
optional_value_or_zero(const std::optional<uint64_t>& value) noexcept {
    return value.value_or(0);
}

[[nodiscard]] uint64_t elapsed_wall_ns(std::chrono::steady_clock::time_point before,
                                       std::chrono::steady_clock::time_point after) noexcept {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count();
    if (elapsed <= 0) {
        return 0;
    }
    using Elapsed = decltype(elapsed);
    if constexpr (std::numeric_limits<Elapsed>::max() > std::numeric_limits<uint64_t>::max()) {
        if (elapsed > static_cast<Elapsed>(std::numeric_limits<uint64_t>::max())) {
            return std::numeric_limits<uint64_t>::max();
        }
    }
    return static_cast<uint64_t>(elapsed);
}

struct PeakGrowth final {
    bool supported = false;
    uint64_t bytes = 0;
};

[[nodiscard]] PeakGrowth peak_growth(const ProcessMemorySnapshot& before,
                                     const ProcessMemorySnapshot& after) noexcept {
    if (before.backend == ProcessMemoryBackend::Unsupported || before.backend != after.backend ||
        !before.lifetime_peak_rss_bytes.has_value() || !after.lifetime_peak_rss_bytes.has_value() ||
        *after.lifetime_peak_rss_bytes < *before.lifetime_peak_rss_bytes) {
        return {};
    }
    return {true, *after.lifetime_peak_rss_bytes - *before.lifetime_peak_rss_bytes};
}

struct ProbeRecord final {
    ProbeOptions options;
    SIQSLiveSieveFixtureV1 fixture;
    Integer factor;
    Integer cofactor;
    size_t relations_found = 0;
    size_t polynomials_used = 0;
    uint64_t factor_wall_ns = 0;
    ProcessMemorySnapshot before_memory;
    ProcessMemorySnapshot after_memory;
    PeakGrowth growth;
};

[[nodiscard]] ProbeRecord run_probe(const ProbeOptions& options) {
    require(BUILD_TYPE == "Release",
            "probe requires a Release build; observed " + std::string(BUILD_TYPE));
    require(RELEASE_ASSERTIONS_DISABLED, "probe requires NDEBUG to be defined");

    const auto fixture_value = siqs_live_sieve_fixture_v1(FIXTURE_BAND);
    require(fixture_value.has_value(), "missing frozen 50-digit SIQS fixture");
    const SIQSLiveSieveFixtureV1 fixture = *fixture_value;
    const Integer modulus(std::string(fixture.modulus));
    const Integer expected_factor(std::string(fixture.factor_p));
    const Integer expected_cofactor(std::string(fixture.factor_q));
    require(expected_factor * expected_cofactor == modulus,
            "frozen SIQS fixture factors do not multiply to the modulus");

    set_shadow_mode_environment(options.mode);
    const ProcessMemorySnapshot before_memory = gnfs::util::process_memory_snapshot();
    const auto wall_before = std::chrono::steady_clock::now();
    std::optional<SIQSResult> result = factor(modulus, FACTOR_MAX_SECONDS, false);
    const auto wall_after = std::chrono::steady_clock::now();
    const ProcessMemorySnapshot after_memory = gnfs::util::process_memory_snapshot();

    require(result.has_value(), "production SIQS did not factor the frozen 50-digit fixture");
    Integer observed_factor = std::move(result->factor1);
    Integer observed_cofactor = std::move(result->factor2);
    if (observed_cofactor < observed_factor) {
        std::swap(observed_factor, observed_cofactor);
    }
    require(observed_factor == expected_factor && observed_cofactor == expected_cofactor,
            "production SIQS returned a noncanonical fixture factorization");
    require(observed_factor * observed_cofactor == modulus,
            "production SIQS factors do not multiply to the fixture modulus");
    require(result->relations_found > 0, "production SIQS returned zero relations");
    require(result->polynomials_used > 0, "production SIQS returned zero polynomials");

    const uint64_t wall_ns = elapsed_wall_ns(wall_before, wall_after);
    require(wall_ns > 0, "production SIQS wall time is not positive");
    return ProbeRecord{options,
                       fixture,
                       std::move(observed_factor),
                       std::move(observed_cofactor),
                       result->relations_found,
                       result->polynomials_used,
                       wall_ns,
                       before_memory,
                       after_memory,
                       peak_growth(before_memory, after_memory)};
}

void emit_probe_record(const ProbeRecord& record) {
    const std::string factor = record.factor.to_string();
    const std::string cofactor = record.cofactor.to_string();
    const std::string_view mode = mode_name(record.options.mode);
    const std::string_view before_backend =
        gnfs::util::process_memory_backend_name(record.before_memory.backend);
    const std::string_view after_backend =
        gnfs::util::process_memory_backend_name(record.after_memory.backend);

    const int emitted = std::fprintf(
        stdout,
        "%.*s schema_version=1 status=valid profile_id=%.*s build_type=%.*s ndebug=true"
        " scope=production_factor_fresh_process mode=%.*s env_value=%s"
        " sample_ordinal=%" PRIu32 " band=%" PRIu32 " digits=%" PRIu32
        " n=%.*s expected_factor=%.*s expected_cofactor=%.*s"
        " max_seconds=%zu factor_status=factor_found factor=%s cofactor=%s"
        " factor_identity=pass relations_found=%zu polynomials_used=%zu"
        " factor_wall_ns=%" PRIu64 " rss_scope=self_lifetime"
        " before_rss_backend=%.*s before_current_rss_supported=%s"
        " before_current_rss_bytes=%" PRIu64
        " before_peak_rss_supported=%s before_peak_rss_bytes=%" PRIu64
        " after_rss_backend=%.*s after_current_rss_supported=%s"
        " after_current_rss_bytes=%" PRIu64
        " after_peak_rss_supported=%s after_peak_rss_bytes=%" PRIu64
        " peak_growth_supported=%s peak_growth_bytes=%" PRIu64
        " route=legacy_result promotion=false\n",
        static_cast<int>(PROBE_PREFIX.size()), PROBE_PREFIX.data(),
        static_cast<int>(PROFILE_ID.size()), PROFILE_ID.data(), static_cast<int>(BUILD_TYPE.size()),
        BUILD_TYPE.data(), static_cast<int>(mode.size()), mode.data(),
        environment_value(record.options.mode), record.options.sample_ordinal, record.fixture.band,
        record.fixture.band, static_cast<int>(record.fixture.modulus.size()),
        record.fixture.modulus.data(), static_cast<int>(record.fixture.factor_p.size()),
        record.fixture.factor_p.data(), static_cast<int>(record.fixture.factor_q.size()),
        record.fixture.factor_q.data(), FACTOR_MAX_SECONDS, factor.c_str(), cofactor.c_str(),
        record.relations_found, record.polynomials_used, record.factor_wall_ns,
        static_cast<int>(before_backend.size()), before_backend.data(),
        bool_name(record.before_memory.current_rss_bytes.has_value()),
        optional_value_or_zero(record.before_memory.current_rss_bytes),
        bool_name(record.before_memory.lifetime_peak_rss_bytes.has_value()),
        optional_value_or_zero(record.before_memory.lifetime_peak_rss_bytes),
        static_cast<int>(after_backend.size()), after_backend.data(),
        bool_name(record.after_memory.current_rss_bytes.has_value()),
        optional_value_or_zero(record.after_memory.current_rss_bytes),
        bool_name(record.after_memory.lifetime_peak_rss_bytes.has_value()),
        optional_value_or_zero(record.after_memory.lifetime_peak_rss_bytes),
        bool_name(record.growth.supported), record.growth.bytes);
    require(emitted >= 0 && std::fflush(stdout) == 0, "failed to emit probe record");
}

} // namespace

int main(int argc, char** argv) {
    try {
        emit_probe_record(run_probe(parse_options(argc, argv)));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%.*s %s\n", static_cast<int>(ERROR_PREFIX.size()),
                     ERROR_PREFIX.data(), error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "%.*s unknown exception\n", static_cast<int>(ERROR_PREFIX.size()),
                     ERROR_PREFIX.data());
        return 1;
    }
}
