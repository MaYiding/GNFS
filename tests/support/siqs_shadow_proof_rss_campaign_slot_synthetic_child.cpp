// Synthetic-only child for the private RSS campaign slot runner. It never
// calls factor(), opens a holdout campaign store, or launches another process.

#include "shadow_proof_rss_holdout_fixture_internal.hpp"
#include "shadow_proof_rss_holdout_probe_protocol_internal.hpp"
#include "shadow_proof_rss_holdout_stream_join_internal.hpp"

#include <gnfs/siqs/shadow_matrix.hpp>
#include <gnfs/siqs/shadow_proof_observe.hpp>
#include <gnfs/siqs/shadow_proof_runner.hpp>
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

[[nodiscard]] bool
emit_valid_observe_record(const support::SIQSShadowProofRssHoldoutProbeOptions& options) {
    if (options.mode != support::SIQSShadowProofRssHoldoutProbeMode::observe) {
        return true;
    }
    if (options.fixture_id == 0 ||
        options.fixture_id > support::SIQS_SHADOW_PROOF_RSS_HOLDOUT_LARGE_PRIME_BOUNDS.size()) {
        return false;
    }

    constexpr std::size_t matrix_rows = support::SIQS_SHADOW_PROOF_RSS_HOLDOUT_SELECTED_ROWS;
    constexpr std::size_t matrix_columns =
        support::SIQS_SHADOW_PROOF_RSS_HOLDOUT_FACTOR_BASE_COLUMNS;
    constexpr auto projected_dense_bytes =
        gnfs::siqs::checked_siqs_shadow_dense_matrix_bytes(matrix_rows, matrix_columns);
    static_assert(projected_dense_bytes.has_value());

    const gnfs::siqs::SIQSShadowProofOptions defaults{};
    gnfs::siqs::SIQSShadowProofObserveRecord record;
    record.proof_attempted = true;
    record.terminal_status = gnfs::siqs::SIQSShadowProofTerminalStatus::factor_found;
    record.stage = gnfs::siqs::SIQSShadowProofStage::factor_extraction;
    record.fallback_reason = gnfs::siqs::SIQSShadowProofFallbackReason::none;
    record.factor_found = true;
    record.observe_wall_ns = UINT64_C(6000000);
    record.raw_relations = 8457;
    record.raw_payload_supported = true;
    record.raw_payload_bytes = 14151664;
    record.factor_base_columns = matrix_columns;
    record.large_prime_bound =
        support::SIQS_SHADOW_PROOF_RSS_HOLDOUT_LARGE_PRIME_BOUNDS[options.fixture_id - 1];
    record.raw_relation_cap = defaults.limits.max_raw_relations;
    record.raw_payload_cap_bytes = defaults.limits.max_raw_payload_bytes;
    record.graph_edge_cap = defaults.limits.graph.max_edges;
    record.graph_cycle_cap = defaults.limits.graph.max_cycles;
    record.graph_incidence_cap = defaults.limits.graph.max_cycle_incidences;
    record.row_candidate_cap = defaults.limits.max_row_candidates;
    record.pretrim_row_cap = defaults.limits.max_pretrim_rows;
    record.minimum_row_excess = defaults.limits.minimum_row_excess;
    record.trim_excess_rows = defaults.assembly.trim_excess_rows;
    record.assembly_workers = defaults.assembly.materialization_workers;
    record.matrix_max_dependencies = defaults.matrix.max_dependencies;
    record.matrix_workers = defaults.matrix.elimination_workers;
    record.matrix_parallel_column_threshold = defaults.matrix.parallel_column_threshold;
    record.matrix_dense_bytes_cap = defaults.matrix.max_dense_matrix_bytes;
    record.matrix_dense_variable_cap = defaults.matrix.max_dense_variable_count;
    record.adapter_input_relations = 8457;
    record.adapter_full_relations = 1243;
    record.adapter_accepted_one_lp = 7214;
    record.graph_evidence_supported = true;
    record.graph_vertices = 6641;
    record.graph_edges = 7214;
    record.graph_components = 1;
    record.graph_cycles = 574;
    record.graph_cycle_incidences = 1148;
    record.graph_max_cycle_length = 2;
    record.row_candidate_upper = 1817;
    record.assembly_evidence_supported = true;
    record.assembly_pretrim_rows = 1816;
    record.assembly_selected_rows = matrix_rows;
    record.assembly_selected_full_rows = 1242;
    record.assembly_selected_cycle_rows = 459;
    record.assembly_trimmed_rows = 115;
    record.assembly_fingerprint_supported = true;
    record.assembly_source_fingerprint_low = 11;
    record.assembly_source_fingerprint_high = 12;
    record.assembly_pretrim_fingerprint_low = 13;
    record.assembly_pretrim_fingerprint_high = 14;
    record.assembly_selected_fingerprint_low = 15;
    record.assembly_selected_fingerprint_high = 16;
    record.projected_dense_bytes_supported = true;
    record.projected_dense_bytes = *projected_dense_bytes;
    record.matrix_evidence_supported = true;
    record.matrix_rows = matrix_rows;
    record.matrix_columns = matrix_columns;
    record.minimum_nullity = matrix_rows - matrix_columns;
    record.dependencies_returned = defaults.matrix.max_dependencies;
    record.dependencies_examined = 1;
    record.dependencies_verified = 1;
    record.factor_found_count = 1;
    record.dependency_cap_reached = true;
    record.dependency_fingerprint_supported = true;
    record.dependency_fingerprint_low = 17;
    record.dependency_fingerprint_high = 18;
    record.winning_dependency_supported = true;
    record.winning_dependency = 0;
    record.winning_dependency_size_supported = true;
    record.winning_dependency_size = 671;
    record.before_memory = {
        host_memory_backend(),
        UINT64_C(11000000),
        UINT64_C(12000000),
    };
    record.after_memory = {
        host_memory_backend(),
        UINT64_C(13000000),
        UINT64_C(14000000),
    };
    record.peak_growth_supported = true;
    record.peak_growth_bytes = UINT64_C(2000000);
    return gnfs::siqs::emit_siqs_shadow_proof_observe_record(stderr, record);
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
    if (!emit_valid_observe_record(parsed.options)) {
        return 95;
    }
    if constexpr (BEHAVIOR == Behavior::valid_nonzero) {
        return 23;
    }
    return 0;
}
