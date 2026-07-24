// Synthetic-only child for the private RSS campaign slot runner. It never
// calls factor(), opens a holdout campaign store, or launches another process.

#include "shadow_proof_rss_holdout_fixture_internal.hpp"
#include "shadow_proof_rss_holdout_probe_protocol_internal.hpp"
#include "shadow_proof_rss_holdout_stream_join_internal.hpp"

#include <gnfs/util/process_memory.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef GNFS_SIQS_RSS_SYNTHETIC_BEHAVIOR
#define GNFS_SIQS_RSS_SYNTHETIC_BEHAVIOR 0
#endif

namespace {

namespace fixtures = gnfs::siqs::shadow_proof_rss_holdout_fixture_detail;
namespace support = gnfs::siqs::shadow_proof_rss_holdout_detail;

enum class Behavior : int {
    success = 0,
    valid_nonzero = 1,
    malformed_success = 2,
    stdout_overflow = 3,
    hang = 4,
};

constexpr Behavior BEHAVIOR = static_cast<Behavior>(GNFS_SIQS_RSS_SYNTHETIC_BEHAVIOR);

[[nodiscard]] constexpr gnfs::util::ProcessMemoryBackend host_memory_backend() noexcept {
#if defined(_WIN32)
    return gnfs::util::ProcessMemoryBackend::WindowsPsapi;
#elif defined(__APPLE__)
    return gnfs::util::ProcessMemoryBackend::DarwinGetrusage;
#elif defined(__linux__)
    return gnfs::util::ProcessMemoryBackend::LinuxGetrusage;
#else
    return gnfs::util::ProcessMemoryBackend::Unsupported;
#endif
}

[[nodiscard]] bool
write_launch_marker(const support::SIQSShadowProofRssHoldoutProbeOptions& options) {
    const char* marker_value = std::getenv("GNFS_SIQS_RSS_SYNTHETIC_MARKER");
    if (marker_value == nullptr || *marker_value == '\0' ||
        std::getenv("GNFS_SIQS_SHADOW_PROOF") != nullptr) {
        return false;
    }
    const std::filesystem::path marker(marker_value);
    std::error_code error;
    if (!std::filesystem::create_directory(marker, error) || error) {
        return false;
    }
    std::ofstream output(marker / "launch.txt", std::ios::binary | std::ios::out);
    output << "fixture_id=" << options.fixture_id
           << " mode=" << support::siqs_shadow_proof_rss_holdout_probe_mode_name(options.mode)
           << " ordinal=" << options.ordinal << '\n';
    output.flush();
    return static_cast<bool>(output);
}

[[nodiscard]] std::string
make_valid_record(const support::SIQSShadowProofRssHoldoutProbeOptions& options) {
    if (options.fixture_id == 0 ||
        options.fixture_id > fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1.size()) {
        return {};
    }
    const auto& fixture = fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1[options.fixture_id - 1];
    support::SIQSShadowProofRssHoldoutProbeRecord record;
    record.fixture_id = options.fixture_id;
    record.mode = options.mode;
    record.ordinal = options.ordinal;
    record.environment_value =
        support::siqs_shadow_proof_rss_holdout_probe_environment_value(options.mode);
    record.digits = static_cast<uint32_t>(fixture.modulus.size());
    record.modulus = fixture.modulus;
    record.expected_factor = fixture.factor_p;
    record.expected_cofactor = fixture.factor_q;
    record.factor = fixture.factor_p;
    record.cofactor = fixture.factor_q;
    record.relations_found = support::SIQS_SHADOW_PROOF_RSS_HOLDOUT_SELECTED_ROWS;
    record.polynomials_used = 17;
    record.resolved_production_sieve_workers = 4;
    record.factor_wall_ns = UINT64_C(7000000);
    record.rss_backend = gnfs::util::process_memory_backend_name(host_memory_backend());
    record.before_current_rss_supported = true;
    record.before_current_rss_bytes = UINT64_C(11000000);
    record.before_peak_rss_supported = true;
    record.before_peak_rss_bytes = UINT64_C(12000000);
    record.after_current_rss_supported = true;
    record.after_current_rss_bytes = UINT64_C(13000000);
    record.after_peak_rss_supported = true;
    record.after_peak_rss_bytes = UINT64_C(14000000);
    record.absolute_peak_rss_supported = true;
    record.absolute_peak_rss_bytes = UINT64_C(14000000);
    record.peak_growth_supported = true;
    record.peak_growth_bytes = UINT64_C(2000000);

    std::string output;
    if (!support::emit_siqs_shadow_proof_rss_holdout_probe_record(record, output)) {
        return {};
    }
    return output;
}

[[nodiscard]] bool write_stdout(std::string_view bytes) {
    return std::fwrite(bytes.data(), 1, bytes.size(), stdout) == bytes.size() &&
           std::fflush(stdout) == 0 && std::ferror(stdout) == 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> arguments;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) {
            return 90;
        }
        arguments.emplace_back(argv[index]);
    }
    const auto parsed = support::parse_siqs_shadow_proof_rss_holdout_probe_options(arguments);
    if (!parsed || !write_launch_marker(parsed.options)) {
        return 91;
    }

    if constexpr (BEHAVIOR == Behavior::hang) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        return 0;
    }
    if constexpr (BEHAVIOR == Behavior::stdout_overflow) {
        return write_stdout(std::string(4097, 'X')) ? 0 : 92;
    }
    if constexpr (BEHAVIOR == Behavior::malformed_success) {
        return write_stdout("malformed\n") ? 0 : 93;
    }

    const std::string record = make_valid_record(parsed.options);
    if (record.empty() || !write_stdout(record)) {
        return 94;
    }
    if constexpr (BEHAVIOR == Behavior::valid_nonzero) {
        return 23;
    }
    return 0;
}
