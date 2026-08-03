#include "shadow_proof_rss_holdout_probe_protocol_internal.hpp"
#include "shadow_proof_rss_probe_execution_identity_internal.hpp"

#include <gnfs/siqs/shadow_proof_rss_probe_execution_identity.hpp>
#include <gnfs/util/sha256.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace identity_detail = gnfs::siqs::shadow_proof_rss_probe_execution_identity_detail;

using gnfs::siqs::siqs_shadow_proof_rss_probe_execution_identity_is_valid;
using gnfs::siqs::SIQSShadowProofRssArchitecture;
using gnfs::siqs::SIQSShadowProofRssOperatingSystem;
using gnfs::siqs::SIQSShadowProofRssProbeExecutionIdentity;
using gnfs::siqs::SIQSShadowProofRssProbeKind;
using gnfs::util::decode_sha256_hex;
using gnfs::util::encode_sha256_hex;
using gnfs::util::ProcessMemoryBackend;
using gnfs::util::sha256;
using gnfs::util::Sha256Accumulator;
using gnfs::util::Sha256Digest;
using identity_detail::make_siqs_shadow_proof_rss_probe_execution_identity;
using identity_detail::ProbeExecutableLaunchProfile;
using identity_detail::ProbeExecutionContractInput;

constexpr std::string_view EXECUTABLE_SHA256 =
    "7cd5084a925acabf1d46b47291a92effd2cbf7eef3a5ed354f626b1878fa114b";
constexpr std::string_view EXECUTION_CONTRACT_SHA256 =
    "f159f66eb3aeb601aa5ac4bb0fc839efcaa0f074fca651001fe54d508ad2546a";

static_assert(gnfs::siqs::SIQS_SHADOW_PROOF_RSS_PROBE_EXECUTION_CONTRACT_SCHEMA_VERSION == 2);
static_assert(sizeof(SIQSShadowProofRssProbeExecutionIdentity) == 64);
static_assert(std::is_trivially_copyable_v<SIQSShadowProofRssProbeExecutionIdentity>);
static_assert(identity_detail::SIQS_SHADOW_PROOF_RSS_PROBE_TIMEOUT_MIN_MS == 1);
static_assert(identity_detail::SIQS_SHADOW_PROOF_RSS_PROBE_TIMEOUT_MAX_MS == 60'000);
static_assert(identity_detail::SIQS_SHADOW_PROOF_RSS_PROBE_STDOUT_CAP_BYTES == 4096);
static_assert(identity_detail::SIQS_SHADOW_PROOF_RSS_PROBE_OFF_STDERR_CAP_BYTES == 0);
static_assert(identity_detail::SIQS_SHADOW_PROOF_RSS_PROBE_OBSERVE_STDERR_CAP_BYTES == 16 * 1024);
static_assert(identity_detail::SIQS_SHADOW_PROOF_RSS_PROBE_JOINED_CAP_BYTES == 4096);
static_assert(identity_detail::canonical_detail::transport_contract_version(
                  ProbeExecutableLaunchProfile::synthetic_path_spawn_v1) ==
              std::optional<std::uint64_t>{1});
static_assert(identity_detail::canonical_detail::transport_contract_version(
                  ProbeExecutableLaunchProfile::linux_sealed_memfd_execveat_v1) ==
              std::optional<std::uint64_t>{2});
static_assert(identity_detail::canonical_detail::transport_contract_version(
                  ProbeExecutableLaunchProfile::darwin_hardened_suspended_v1) ==
              std::optional<std::uint64_t>{1});
static_assert(!identity_detail::canonical_detail::transport_contract_version(
                   ProbeExecutableLaunchProfile::unknown)
                   .has_value());
static_assert(
    identity_detail::SIQS_SHADOW_PROOF_RSS_PROBE_STDOUT_SCHEMA ==
    gnfs::siqs::shadow_proof_rss_holdout_detail::SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_PREFIX);
static_assert(identity_detail::SIQS_SHADOW_PROOF_RSS_PROBE_STDOUT_SCHEMA_VERSION ==
              gnfs::siqs::shadow_proof_rss_holdout_detail::
                  SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_SCHEMA_VERSION);
static_assert(noexcept(siqs_shadow_proof_rss_probe_execution_identity_is_valid(
    SIQSShadowProofRssProbeExecutionIdentity{})));
static_assert(noexcept(make_siqs_shadow_proof_rss_probe_execution_identity(
    std::declval<const ProbeExecutionContractInput&>())));

int checks_passed = 0;
int checks_failed = 0;

void expect(bool condition, const char* expression, int line) {
    if (condition) {
        ++checks_passed;
        return;
    }
    ++checks_failed;
    std::cerr << "FAIL: " << expression << " at " << __FILE__ << ':' << line << '\n';
}

#define EXPECT(condition) expect(static_cast<bool>(condition), #condition, __LINE__)

[[nodiscard]] std::string_view hex_view(const gnfs::util::Sha256Hex& encoded) noexcept {
    return {encoded.data(), encoded.size()};
}

[[nodiscard]] Sha256Digest required_digest(std::string_view encoded) {
    const auto digest = decode_sha256_hex(encoded);
    EXPECT(digest.has_value());
    return digest.value_or(Sha256Digest{});
}

struct Fixture final {
    std::vector<std::string> environment = {
        "GNFS_SIQS_SHADOW_PROOF=observe",
        "LANG=C",
        "TZ=UTC",
        "ZZ_TOKEN=alpha=beta",
    };
    ProbeExecutionContractInput input;

    Fixture() {
        input.executable_sha256 = required_digest(EXECUTABLE_SHA256);
        input.probe_kind = SIQSShadowProofRssProbeKind::production_holdout;
        input.launch_profile = ProbeExecutableLaunchProfile::linux_sealed_memfd_execveat_v1;
        input.candidate_revision = "candidate-revision-2026-07-25";
        input.operating_system = SIQSShadowProofRssOperatingSystem::linux;
        input.architecture = SIQSShadowProofRssArchitecture::x86_64;
        input.memory_backend = ProcessMemoryBackend::LinuxGetrusage;
        input.resolved_production_sieve_workers = 4;
        input.release_build = true;
        input.ndebug = true;
        input.timeout_ms = 35'000;
        input.expected_owner = 1000;
        bind_environment();
    }

    void bind_environment() noexcept {
        input.environment = std::span<const std::string>(environment);
    }
};

[[nodiscard]] std::optional<SIQSShadowProofRssProbeExecutionIdentity>
identity_for(Fixture& fixture) noexcept {
    fixture.bind_environment();
    return make_siqs_shadow_proof_rss_probe_execution_identity(fixture.input);
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::byte>(value >> shift));
    }
}

void append_string(std::vector<std::byte>& output, std::string_view value) {
    append_u64(output, static_cast<std::uint64_t>(value.size()));
    const auto bytes = std::as_bytes(std::span<const char>(value.data(), value.size()));
    output.insert(output.end(), bytes.begin(), bytes.end());
}

[[nodiscard]] std::vector<std::byte> independent_golden_preimage() {
    std::vector<std::byte> output;
    output.reserve(900);
    append_string(output, "gnfs.siqs.shadow_proof_rss.probe_execution_contract.sha256.v2");
    append_u64(output, 2);
    const Sha256Digest executable = required_digest(EXECUTABLE_SHA256);
    output.insert(output.end(), executable.bytes.begin(), executable.bytes.end());
    append_u64(output, 2);
    append_u64(output, 2);
    append_string(output, "gnfs-siqs-rss-holdout-probe");
    append_string(output, "candidate-revision-2026-07-25");
    append_u64(output, 2);
    append_u64(output, 1);
    append_u64(output, 2);
    append_u64(output, 4);
    append_u64(output, 1);
    append_u64(output, 1);

    constexpr std::array<std::string_view, 4> ENVIRONMENT = {
        "GNFS_SIQS_SHADOW_PROOF=observe",
        "LANG=C",
        "TZ=UTC",
        "ZZ_TOKEN=alpha=beta",
    };
    append_u64(output, ENVIRONMENT.size());
    for (const std::string_view entry : ENVIRONMENT) {
        append_string(output, entry);
    }

    append_u64(output, 35'000);
    append_u64(output, 1000);
    append_string(output, "gnfs.siqs.shadow_proof_rss.argv_template.v1");
    append_u64(output, 1);
    constexpr std::array<std::string_view, 6> ARGUMENTS = {
        "--fixture-id", "{fixture_id_u32_decimal}", "--mode", "{off|observe}",
        "--ordinal",    "{ordinal_u32_decimal}",
    };
    append_u64(output, ARGUMENTS.size());
    for (const std::string_view argument : ARGUMENTS) {
        append_string(output, argument);
    }

    append_u64(output, 4096);
    append_u64(output, 0);
    append_u64(output, 16 * 1024);
    append_u64(output, 4096);
    append_string(output, "GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_V1");
    append_u64(output, 1);
    append_string(output, "GNFS_SIQS_SHADOW_PROOF_OBSERVE_V1");
    append_u64(output, 1);
    append_string(output, "GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_JOINED_DRAFT_V3");
    append_u64(output, 3);
    append_string(
        output, "gnfs.util.authenticated_bounded_child_process.linux_memfd_execveat_pdeathsig.v2");
    append_u64(output, 2);
    append_u64(output, 1);
    append_u64(output, 1);
    append_u64(output, 1);
    return output;
}

template <typename Mutator>
void expect_valid_change(const SIQSShadowProofRssProbeExecutionIdentity& baseline,
                         Mutator&& mutate) {
    Fixture changed;
    mutate(changed);
    const auto identity = identity_for(changed);
    EXPECT(identity.has_value());
    if (identity.has_value()) {
        EXPECT(identity->execution_contract_sha256 != baseline.execution_contract_sha256);
    }
}

void test_golden_identity_and_split_hash() {
    Fixture fixture;
    const auto identity = identity_for(fixture);
    EXPECT(identity.has_value());
    if (!identity.has_value()) {
        return;
    }
    EXPECT(siqs_shadow_proof_rss_probe_execution_identity_is_valid(*identity));
    EXPECT(hex_view(encode_sha256_hex(identity->executable_sha256)) == EXECUTABLE_SHA256);
    EXPECT(hex_view(encode_sha256_hex(identity->execution_contract_sha256)) ==
           EXECUTION_CONTRACT_SHA256);

    const auto decoded_contract = decode_sha256_hex(EXECUTION_CONTRACT_SHA256);
    EXPECT(decoded_contract.has_value());
    EXPECT(decoded_contract == std::optional(identity->execution_contract_sha256));

    const std::vector<std::byte> preimage = independent_golden_preimage();
    EXPECT(preimage.size() == 887);
    const auto one_shot = sha256(std::span<const std::byte>(preimage));
    EXPECT(one_shot.has_value());
    EXPECT(one_shot == decoded_contract);

    Sha256Accumulator split;
    constexpr std::array<std::size_t, 8> CHUNKS = {1, 7, 31, 64, 3, 127, 11, 256};
    std::size_t offset = 0;
    std::size_t chunk_index = 0;
    while (offset < preimage.size()) {
        const std::size_t count =
            std::min(CHUNKS[chunk_index % CHUNKS.size()], preimage.size() - offset);
        EXPECT(split.update(std::span<const std::byte>(preimage).subspan(offset, count)));
        offset += count;
        ++chunk_index;
    }
    EXPECT(split.finalize() == decoded_contract);
}

void test_every_input_field_is_bound() {
    Fixture fixture;
    const auto baseline = identity_for(fixture);
    EXPECT(baseline.has_value());
    if (!baseline.has_value()) {
        return;
    }

    expect_valid_change(*baseline, [](Fixture& changed) {
        changed.input.executable_sha256.bytes[0] ^= std::byte{1};
    });
    expect_valid_change(*baseline, [](Fixture& changed) {
        changed.input.probe_kind = SIQSShadowProofRssProbeKind::synthetic_test;
    });
    expect_valid_change(*baseline, [](Fixture& changed) {
        changed.input.probe_kind = SIQSShadowProofRssProbeKind::synthetic_test;
        changed.input.launch_profile = ProbeExecutableLaunchProfile::synthetic_path_spawn_v1;
    });
    Fixture synthetic_sealed;
    synthetic_sealed.input.probe_kind = SIQSShadowProofRssProbeKind::synthetic_test;
    const auto synthetic_sealed_identity = identity_for(synthetic_sealed);
    EXPECT(synthetic_sealed_identity.has_value());
    synthetic_sealed.input.launch_profile = ProbeExecutableLaunchProfile::synthetic_path_spawn_v1;
    const auto synthetic_path_identity = identity_for(synthetic_sealed);
    EXPECT(synthetic_path_identity.has_value());
    if (synthetic_sealed_identity.has_value() && synthetic_path_identity.has_value()) {
        EXPECT(synthetic_sealed_identity->execution_contract_sha256 !=
               synthetic_path_identity->execution_contract_sha256);
    }
    expect_valid_change(*baseline, [](Fixture& changed) {
        changed.input.candidate_revision = "candidate-revision-2026-07-26";
    });
    expect_valid_change(*baseline, [](Fixture& changed) {
        changed.input.operating_system = SIQSShadowProofRssOperatingSystem::darwin;
        changed.input.memory_backend = ProcessMemoryBackend::DarwinGetrusage;
        changed.input.launch_profile = ProbeExecutableLaunchProfile::darwin_hardened_suspended_v1;
    });
    expect_valid_change(*baseline, [](Fixture& changed) {
        changed.input.architecture = SIQSShadowProofRssArchitecture::arm64;
    });
    expect_valid_change(
        *baseline, [](Fixture& changed) { changed.input.resolved_production_sieve_workers = 5; });
    expect_valid_change(*baseline, [](Fixture& changed) { changed.input.release_build = false; });
    expect_valid_change(*baseline, [](Fixture& changed) { changed.input.ndebug = false; });
    expect_valid_change(*baseline,
                        [](Fixture& changed) { changed.environment[1] = "LANG=C.UTF-8"; });
    expect_valid_change(*baseline, [](Fixture& changed) { changed.input.timeout_ms = 35'001; });
    expect_valid_change(*baseline, [](Fixture& changed) { changed.input.expected_owner = 1001; });
}

void test_identity_and_scalar_rejections() {
    const SIQSShadowProofRssProbeExecutionIdentity zero{};
    EXPECT(!siqs_shadow_proof_rss_probe_execution_identity_is_valid(zero));

    Fixture fixture;
    const auto baseline = identity_for(fixture);
    EXPECT(baseline.has_value());
    if (baseline.has_value()) {
        auto zero_executable = *baseline;
        zero_executable.executable_sha256 = {};
        EXPECT(!siqs_shadow_proof_rss_probe_execution_identity_is_valid(zero_executable));
        auto zero_contract = *baseline;
        zero_contract.execution_contract_sha256 = {};
        EXPECT(!siqs_shadow_proof_rss_probe_execution_identity_is_valid(zero_contract));
    }

    fixture.input.executable_sha256 = {};
    EXPECT(!identity_for(fixture).has_value());

    Fixture unknown_kind;
    unknown_kind.input.probe_kind = SIQSShadowProofRssProbeKind::unknown;
    EXPECT(!identity_for(unknown_kind).has_value());
    Fixture unknown_profile;
    unknown_profile.input.launch_profile = ProbeExecutableLaunchProfile::unknown;
    EXPECT(!identity_for(unknown_profile).has_value());
    Fixture production_path;
    production_path.input.launch_profile = ProbeExecutableLaunchProfile::synthetic_path_spawn_v1;
    EXPECT(!identity_for(production_path).has_value());
    Fixture linux_darwin_profile;
    linux_darwin_profile.input.launch_profile =
        ProbeExecutableLaunchProfile::darwin_hardened_suspended_v1;
    EXPECT(!identity_for(linux_darwin_profile).has_value());
    Fixture unknown_os;
    unknown_os.input.operating_system = SIQSShadowProofRssOperatingSystem::unknown;
    EXPECT(!identity_for(unknown_os).has_value());
    Fixture unknown_arch;
    unknown_arch.input.architecture = SIQSShadowProofRssArchitecture::unknown;
    EXPECT(!identity_for(unknown_arch).has_value());
    Fixture unsupported_backend;
    unsupported_backend.input.memory_backend = ProcessMemoryBackend::Unsupported;
    EXPECT(!identity_for(unsupported_backend).has_value());
    Fixture mismatched_backend;
    mismatched_backend.input.memory_backend = ProcessMemoryBackend::DarwinGetrusage;
    EXPECT(!identity_for(mismatched_backend).has_value());
    Fixture zero_workers;
    zero_workers.input.resolved_production_sieve_workers = 0;
    EXPECT(!identity_for(zero_workers).has_value());
}

void test_token_validation() {
    Fixture empty;
    empty.input.candidate_revision = {};
    EXPECT(!identity_for(empty).has_value());

    Fixture too_long;
    const std::string long_token(gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_MAX_TOKEN_BYTES + 1, 'a');
    too_long.input.candidate_revision = long_token;
    EXPECT(!identity_for(too_long).has_value());

    Fixture whitespace;
    whitespace.input.candidate_revision = "candidate revision";
    EXPECT(!identity_for(whitespace).has_value());
    Fixture equals;
    equals.input.candidate_revision = "candidate=revision";
    EXPECT(!identity_for(equals).has_value());
    Fixture embedded_nul;
    const std::string nul_token("candidate\0revision", 18);
    embedded_nul.input.candidate_revision = nul_token;
    EXPECT(!identity_for(embedded_nul).has_value());
}

void test_environment_validation() {
    Fixture unsorted;
    std::swap(unsorted.environment[0], unsorted.environment[1]);
    EXPECT(!identity_for(unsorted).has_value());

    Fixture duplicate;
    duplicate.environment = {"LANG=C", "LANG=C.UTF-8"};
    EXPECT(!identity_for(duplicate).has_value());

    Fixture missing_separator;
    missing_separator.environment = {"LANG"};
    EXPECT(!identity_for(missing_separator).has_value());
    Fixture empty_name;
    empty_name.environment = {"=value"};
    EXPECT(!identity_for(empty_name).has_value());
    Fixture digit_first;
    digit_first.environment = {"1LANG=C"};
    EXPECT(!identity_for(digit_first).has_value());
    Fixture punctuation;
    punctuation.environment = {"BAD-NAME=value"};
    EXPECT(!identity_for(punctuation).has_value());
    Fixture embedded_nul;
    embedded_nul.environment = {std::string("LANG=C\0hidden", 13)};
    EXPECT(!identity_for(embedded_nul).has_value());
    Fixture loader_preload;
    loader_preload.environment = {"LD_PRELOAD=/tmp/unapproved.so"};
    EXPECT(!identity_for(loader_preload).has_value());
    Fixture loader_tunables;
    loader_tunables.environment = {"GLIBC_TUNABLES=glibc.malloc.check=3"};
    EXPECT(!identity_for(loader_tunables).has_value());
    Fixture darwin_loader;
    darwin_loader.input.operating_system = SIQSShadowProofRssOperatingSystem::darwin;
    darwin_loader.input.memory_backend = ProcessMemoryBackend::DarwinGetrusage;
    darwin_loader.input.launch_profile = ProbeExecutableLaunchProfile::darwin_hardened_suspended_v1;
    darwin_loader.environment = {"DYLD_INSERT_LIBRARIES=/tmp/unapproved.dylib"};
    EXPECT(!identity_for(darwin_loader).has_value());

    Fixture empty_environment;
    empty_environment.environment.clear();
    EXPECT(identity_for(empty_environment).has_value());
}

void test_timeout_boundaries() {
    Fixture below;
    below.input.timeout_ms = 0;
    EXPECT(!identity_for(below).has_value());

    Fixture minimum;
    minimum.input.timeout_ms = identity_detail::SIQS_SHADOW_PROOF_RSS_PROBE_TIMEOUT_MIN_MS;
    EXPECT(identity_for(minimum).has_value());

    Fixture maximum;
    maximum.input.timeout_ms = identity_detail::SIQS_SHADOW_PROOF_RSS_PROBE_TIMEOUT_MAX_MS;
    EXPECT(identity_for(maximum).has_value());

    Fixture above;
    above.input.timeout_ms = identity_detail::SIQS_SHADOW_PROOF_RSS_PROBE_TIMEOUT_MAX_MS + 1;
    EXPECT(!identity_for(above).has_value());
}

} // namespace

int main() {
    test_golden_identity_and_split_hash();
    test_every_input_field_is_bound();
    test_identity_and_scalar_rejections();
    test_token_validation();
    test_environment_validation();
    test_timeout_boundaries();

    std::cout << "SIQS probe execution identity checks: " << checks_passed << " passed, "
              << checks_failed << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
