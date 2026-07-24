// Pure synthetic-byte tests for the canonical SIQS RSS policy record codec.
// This test never reads policy files, host state, environment variables,
// sealed fixtures, process memory, or production SIQS paths.

#include <gnfs/siqs/shadow_proof_rss_policy_record.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace {

using std::uint64_t;

using gnfs::siqs::emit_siqs_shadow_proof_rss_policy_record;
using gnfs::siqs::parse_siqs_shadow_proof_rss_policy_record;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_HIGH;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_LOW;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_ID;
using gnfs::siqs::siqs_shadow_proof_rss_policy_binding_digest;
using gnfs::siqs::siqs_shadow_proof_rss_policy_record_error_name;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_POLICY_RECORD_PREFIX;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_POLICY_RECORD_SCHEMA_VERSION;
using gnfs::siqs::SIQSShadowProofRssArchitecture;
using gnfs::siqs::SIQSShadowProofRssGatePolicy;
using gnfs::siqs::SIQSShadowProofRssOperatingSystem;
using gnfs::siqs::SIQSShadowProofRssPolicyRecord;
using gnfs::siqs::SIQSShadowProofRssPolicyRecordError;
using gnfs::siqs::SIQSShadowProofRssProbeExecutionIdentity;
using gnfs::util::ProcessMemoryBackend;

template <typename T>
concept CanBorrowGatePolicy = requires(T&& value) { std::forward<T>(value).as_gate_policy(); };

static_assert(CanBorrowGatePolicy<SIQSShadowProofRssPolicyRecord&>);
static_assert(CanBorrowGatePolicy<const SIQSShadowProofRssPolicyRecord&>);
static_assert(!CanBorrowGatePolicy<SIQSShadowProofRssPolicyRecord>);
static_assert(!CanBorrowGatePolicy<const SIQSShadowProofRssPolicyRecord>);

int checks_passed = 0;
int checks_failed = 0;

constexpr std::string_view TEST_EXECUTABLE_SHA256 =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
constexpr std::string_view TEST_EXECUTION_CONTRACT_SHA256 =
    "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f";

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':' << __LINE__ << '\n';        \
        }                                                                                          \
    } while (false)

[[nodiscard]] constexpr SIQSShadowProofRssProbeExecutionIdentity
test_probe_execution_identity() noexcept {
    SIQSShadowProofRssProbeExecutionIdentity identity;
    for (std::size_t index = 0; index < identity.executable_sha256.bytes.size(); ++index) {
        identity.executable_sha256.bytes[index] = static_cast<std::byte>(index);
        identity.execution_contract_sha256.bytes[index] = static_cast<std::byte>(index + 32);
    }
    return identity;
}

[[nodiscard]] SIQSShadowProofRssGatePolicy make_policy() {
    SIQSShadowProofRssGatePolicy policy;
    policy.approved = true;
    policy.corpus_id = "synthetic-corpus-v7";
    policy.corpus_digest = {UINT64_C(123456789), UINT64_C(987654321)};
    policy.operating_system = SIQSShadowProofRssOperatingSystem::darwin;
    policy.architecture = SIQSShadowProofRssArchitecture::arm64;
    policy.memory_backend = ProcessMemoryBackend::DarwinGetrusage;
    policy.resolved_production_sieve_workers = 4;
    policy.candidate_revision = "synthetic-revision-abc123";
    policy.probe_execution_identity = test_probe_execution_identity();
    policy.approval_id = "synthetic-approval-42";
    policy.journal_store = {{UINT64_C(1111111122222222), UINT64_C(3333333344444444)},
                            {UINT64_C(1234123412341234), UINT64_C(5678567856785678)},
                            "synthetic-rss-store-v1"};
    policy.deployment_budget_bytes = UINT64_C(4294967296);
    policy.reserved_headroom_bytes = UINT64_C(536870912);
    return policy;
}

[[nodiscard]] std::string canonical_record() {
    return "GNFS_SIQS_SHADOW_PROOF_RSS_POLICY_V3 schema_version=3 approved=true"
           " corpus_id=synthetic-corpus-v7 corpus_digest_low=123456789"
           " corpus_digest_high=987654321 operating_system=darwin architecture=arm64"
           " memory_backend=darwin_getrusage resolved_production_sieve_workers=4"
           " candidate_revision=synthetic-revision-abc123"
           " probe_executable_sha256="
           "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
           " probe_execution_contract_sha256="
           "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
           " approval_id=synthetic-approval-42"
           " journal_trusted_base_id_low=1111111122222222"
           " journal_trusted_base_id_high=3333333344444444"
           " journal_store_id_low=1234123412341234"
           " journal_store_id_high=5678567856785678"
           " journal_store_locator=synthetic-rss-store-v1 deployment_budget_bytes=4294967296"
           " reserved_headroom_bytes=536870912\n";
}

[[nodiscard]] std::string replace_once(std::string input, std::string_view from,
                                       std::string_view to) {
    const std::size_t position = input.find(from);
    CHECK(position != std::string::npos);
    if (position != std::string::npos) {
        input.replace(position, from.size(), to);
    }
    return input;
}

void expect_error(std::string_view bytes, SIQSShadowProofRssPolicyRecordError expected) {
    const auto parsed = parse_siqs_shadow_proof_rss_policy_record(bytes);
    CHECK(!parsed);
    CHECK(parsed.error == expected);
}

void expect_emit_rejected(const SIQSShadowProofRssGatePolicy& policy) {
    std::string output = "sentinel";
    CHECK(!emit_siqs_shadow_proof_rss_policy_record(policy, output));
    CHECK(output == "sentinel");
}

void test_constants_and_error_names() {
    CHECK(std::string_view(SIQS_SHADOW_PROOF_RSS_POLICY_RECORD_PREFIX) ==
          "GNFS_SIQS_SHADOW_PROOF_RSS_POLICY_V3");
    CHECK(SIQS_SHADOW_PROOF_RSS_POLICY_RECORD_SCHEMA_VERSION == 3);

    constexpr std::array errors{
        std::pair{SIQSShadowProofRssPolicyRecordError::none, std::string_view("none")},
        std::pair{SIQSShadowProofRssPolicyRecordError::invalid_framing,
                  std::string_view("invalid_framing")},
        std::pair{SIQSShadowProofRssPolicyRecordError::invalid_ascii,
                  std::string_view("invalid_ascii")},
        std::pair{SIQSShadowProofRssPolicyRecordError::invalid_prefix,
                  std::string_view("invalid_prefix")},
        std::pair{SIQSShadowProofRssPolicyRecordError::missing_field,
                  std::string_view("missing_field")},
        std::pair{SIQSShadowProofRssPolicyRecordError::invalid_field,
                  std::string_view("invalid_field")},
        std::pair{SIQSShadowProofRssPolicyRecordError::duplicate_field,
                  std::string_view("duplicate_field")},
        std::pair{SIQSShadowProofRssPolicyRecordError::unknown_field,
                  std::string_view("unknown_field")},
        std::pair{SIQSShadowProofRssPolicyRecordError::field_out_of_order,
                  std::string_view("field_out_of_order")},
        std::pair{SIQSShadowProofRssPolicyRecordError::invalid_schema_version,
                  std::string_view("invalid_schema_version")},
        std::pair{SIQSShadowProofRssPolicyRecordError::invalid_boolean,
                  std::string_view("invalid_boolean")},
        std::pair{SIQSShadowProofRssPolicyRecordError::invalid_unsigned_integer,
                  std::string_view("invalid_unsigned_integer")},
        std::pair{SIQSShadowProofRssPolicyRecordError::unsigned_integer_out_of_range,
                  std::string_view("unsigned_integer_out_of_range")},
        std::pair{SIQSShadowProofRssPolicyRecordError::invalid_operating_system,
                  std::string_view("invalid_operating_system")},
        std::pair{SIQSShadowProofRssPolicyRecordError::invalid_architecture,
                  std::string_view("invalid_architecture")},
        std::pair{SIQSShadowProofRssPolicyRecordError::invalid_memory_backend,
                  std::string_view("invalid_memory_backend")},
        std::pair{SIQSShadowProofRssPolicyRecordError::invalid_sha256,
                  std::string_view("invalid_sha256")},
        std::pair{SIQSShadowProofRssPolicyRecordError::resource_failure,
                  std::string_view("resource_failure")},
    };
    for (const auto& [error, name] : errors) {
        CHECK(siqs_shadow_proof_rss_policy_record_error_name(error) == name);
    }
    CHECK(siqs_shadow_proof_rss_policy_record_error_name(
              static_cast<SIQSShadowProofRssPolicyRecordError>(255)) == "unknown");
}

void test_exact_emit_parse_and_gate_mapping() {
    const auto source = make_policy();
    std::string emitted = "old-value";
    CHECK(emit_siqs_shadow_proof_rss_policy_record(source, emitted));
    CHECK(emitted == canonical_record());
    CHECK(emitted.back() == '\n');
    CHECK(emitted.find('\r') == std::string::npos);

    const auto parsed = parse_siqs_shadow_proof_rss_policy_record(emitted);
    CHECK(parsed);
    CHECK(parsed.error == SIQSShadowProofRssPolicyRecordError::none);
    const auto mapped = parsed.record.as_gate_policy();
    CHECK(mapped.approved == source.approved);
    CHECK(mapped.corpus_id == source.corpus_id);
    CHECK(mapped.corpus_digest == source.corpus_digest);
    CHECK(mapped.operating_system == source.operating_system);
    CHECK(mapped.architecture == source.architecture);
    CHECK(mapped.memory_backend == source.memory_backend);
    CHECK(mapped.resolved_production_sieve_workers == source.resolved_production_sieve_workers);
    CHECK(mapped.candidate_revision == source.candidate_revision);
    CHECK(mapped.probe_execution_identity == source.probe_execution_identity);
    CHECK(mapped.approval_id == source.approval_id);
    CHECK(mapped.journal_store == source.journal_store);
    CHECK(mapped.deployment_budget_bytes == source.deployment_budget_bytes);
    CHECK(mapped.reserved_headroom_bytes == source.reserved_headroom_bytes);
    CHECK(siqs_shadow_proof_rss_policy_binding_digest(mapped) ==
          siqs_shadow_proof_rss_policy_binding_digest(source));

    std::string reemitted;
    CHECK(emit_siqs_shadow_proof_rss_policy_record(mapped, reemitted));
    CHECK(reemitted == emitted);
}

void test_closed_platform_values_and_semantic_separation() {
    auto policy = make_policy();
    for (const auto [operating_system, architecture, backend] :
         std::array{std::tuple{SIQSShadowProofRssOperatingSystem::darwin,
                               SIQSShadowProofRssArchitecture::arm64,
                               ProcessMemoryBackend::DarwinGetrusage},
                    std::tuple{SIQSShadowProofRssOperatingSystem::linux,
                               SIQSShadowProofRssArchitecture::x86_64,
                               ProcessMemoryBackend::LinuxGetrusage},
                    std::tuple{SIQSShadowProofRssOperatingSystem::windows,
                               SIQSShadowProofRssArchitecture::arm64,
                               ProcessMemoryBackend::WindowsPsapi}}) {
        policy.operating_system = operating_system;
        policy.architecture = architecture;
        policy.memory_backend = backend;
        std::string emitted;
        CHECK(emit_siqs_shadow_proof_rss_policy_record(policy, emitted));
        const auto parsed = parse_siqs_shadow_proof_rss_policy_record(emitted);
        CHECK(parsed);
        CHECK(parsed.record.operating_system == operating_system);
        CHECK(parsed.record.architecture == architecture);
        CHECK(parsed.record.memory_backend == backend);
    }

    // These values are syntactically canonical but semantically unsuitable for
    // an approved gate. The codec preserves them; the existing gate rejects or
    // blocks them later instead of duplicating policy semantics here.
    policy = make_policy();
    policy.approved = false;
    policy.operating_system = SIQSShadowProofRssOperatingSystem::darwin;
    policy.memory_backend = ProcessMemoryBackend::LinuxGetrusage;
    policy.resolved_production_sieve_workers = 0;
    policy.journal_store = {{}, {}, "syntactic/path"};
    policy.deployment_budget_bytes = 0;
    policy.reserved_headroom_bytes = std::numeric_limits<uint64_t>::max();
    std::string emitted;
    CHECK(emit_siqs_shadow_proof_rss_policy_record(policy, emitted));
    const auto parsed = parse_siqs_shadow_proof_rss_policy_record(emitted);
    CHECK(parsed);
    CHECK(!parsed.record.approved);
    CHECK(parsed.record.memory_backend == ProcessMemoryBackend::LinuxGetrusage);
    CHECK(parsed.record.resolved_production_sieve_workers == 0);
    CHECK(parsed.record.journal_trusted_base_id == gnfs::siqs::SIQSShadowProofRssCorpusDigest{});
    CHECK(parsed.record.journal_store_id == gnfs::siqs::SIQSShadowProofRssCorpusDigest{});
    CHECK(parsed.record.journal_store_locator == "syntactic/path");
    const auto invalid_store_policy = parsed.record.as_gate_policy();
    CHECK(gnfs::siqs::siqs_shadow_proof_rss_gate_policy_error(&invalid_store_policy) ==
          gnfs::siqs::SIQSShadowProofRssGateReason::policy_not_approved);
    CHECK(parsed.record.deployment_budget_bytes == 0);
    CHECK(parsed.record.reserved_headroom_bytes == std::numeric_limits<uint64_t>::max());

    policy = make_policy();
    policy.journal_store.relative_locator = "MixedCase";
    emitted.clear();
    CHECK(emit_siqs_shadow_proof_rss_policy_record(policy, emitted));
    const auto parsed_noncanonical_locator = parse_siqs_shadow_proof_rss_policy_record(emitted);
    CHECK(parsed_noncanonical_locator);
    const auto noncanonical_locator_policy = parsed_noncanonical_locator.record.as_gate_policy();
    CHECK(gnfs::siqs::siqs_shadow_proof_rss_gate_policy_error(&noncanonical_locator_policy) ==
          gnfs::siqs::SIQSShadowProofRssGateReason::policy_binding_invalid);

    auto gate_valid_policy = make_policy();
    gate_valid_policy.corpus_id = SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_ID;
    gate_valid_policy.corpus_digest = {SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_LOW,
                                       SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_HIGH};
    CHECK(!gnfs::siqs::siqs_shadow_proof_rss_gate_policy_error(&gate_valid_policy).has_value());
    const auto expect_zero_digest_rejected_by_gate = [&](bool executable_digest) {
        auto changed = gate_valid_policy;
        if (executable_digest) {
            changed.probe_execution_identity.executable_sha256 = {};
        } else {
            changed.probe_execution_identity.execution_contract_sha256 = {};
        }
        std::string zero_digest_record;
        CHECK(emit_siqs_shadow_proof_rss_policy_record(changed, zero_digest_record));
        const auto zero_digest_parsed =
            parse_siqs_shadow_proof_rss_policy_record(zero_digest_record);
        CHECK(zero_digest_parsed);
        if (zero_digest_parsed) {
            const auto zero_digest_policy = zero_digest_parsed.record.as_gate_policy();
            CHECK(gnfs::siqs::siqs_shadow_proof_rss_gate_policy_error(&zero_digest_policy) ==
                  gnfs::siqs::SIQSShadowProofRssGateReason::policy_binding_invalid);
        }
    };
    expect_zero_digest_rejected_by_gate(true);
    expect_zero_digest_rejected_by_gate(false);
}

void test_framing_and_ascii_rejections() {
    const std::string canonical = canonical_record();
    expect_error("", SIQSShadowProofRssPolicyRecordError::invalid_framing);
    expect_error(std::string_view(canonical).substr(0, canonical.size() - 1),
                 SIQSShadowProofRssPolicyRecordError::invalid_framing);
    expect_error("\n", SIQSShadowProofRssPolicyRecordError::invalid_framing);
    expect_error(" " + canonical, SIQSShadowProofRssPolicyRecordError::invalid_framing);
    expect_error(replace_once(canonical, " schema_version", "  schema_version"),
                 SIQSShadowProofRssPolicyRecordError::invalid_framing);
    expect_error(replace_once(canonical, "\n", " \n"),
                 SIQSShadowProofRssPolicyRecordError::invalid_framing);
    expect_error(canonical + "\n", SIQSShadowProofRssPolicyRecordError::invalid_ascii);
    expect_error(replace_once(canonical, "\n", "\r\n"),
                 SIQSShadowProofRssPolicyRecordError::invalid_ascii);
    expect_error(std::string("\xef\xbb\xbf") + canonical,
                 SIQSShadowProofRssPolicyRecordError::invalid_ascii);
    expect_error(replace_once(canonical, " approved", std::string_view("\0 approved", 10)),
                 SIQSShadowProofRssPolicyRecordError::invalid_ascii);
    expect_error(replace_once(canonical, " approved", "\tapproved"),
                 SIQSShadowProofRssPolicyRecordError::invalid_ascii);
    expect_error(
        replace_once(canonical, "synthetic-corpus-v7", std::string_view("synthetic-\x80", 11)),
        SIQSShadowProofRssPolicyRecordError::invalid_ascii);
    expect_error(replace_once(canonical, SIQS_SHADOW_PROOF_RSS_POLICY_RECORD_PREFIX,
                              "GNFS_SIQS_SHADOW_PROOF_RSS_POLICY_V2"),
                 SIQSShadowProofRssPolicyRecordError::invalid_prefix);
    expect_error(replace_once(canonical, SIQS_SHADOW_PROOF_RSS_POLICY_RECORD_PREFIX,
                              "GNFS_SIQS_SHADOW_PROOF_RSS_POLICY_V1"),
                 SIQSShadowProofRssPolicyRecordError::invalid_prefix);
}

void test_field_shape_order_and_cardinality_rejections() {
    const std::string canonical = canonical_record();
    expect_error(replace_once(canonical, " approved=true", " enabled=true"),
                 SIQSShadowProofRssPolicyRecordError::unknown_field);
    expect_error(replace_once(canonical, " approved=true", " corpus_id=true"),
                 SIQSShadowProofRssPolicyRecordError::field_out_of_order);
    expect_error(replace_once(canonical, " approval_id=synthetic-approval-42",
                              " candidate_revision=synthetic-approval-42"),
                 SIQSShadowProofRssPolicyRecordError::duplicate_field);
    expect_error(
        replace_once(canonical, " probe_execution_contract_sha256=", " probe_executable_sha256="),
        SIQSShadowProofRssPolicyRecordError::duplicate_field);
    const std::string executable_field =
        " probe_executable_sha256=" + std::string(TEST_EXECUTABLE_SHA256);
    const std::string contract_field =
        " probe_execution_contract_sha256=" + std::string(TEST_EXECUTION_CONTRACT_SHA256);
    expect_error(replace_once(canonical, executable_field + contract_field,
                              contract_field + executable_field),
                 SIQSShadowProofRssPolicyRecordError::field_out_of_order);
    expect_error(replace_once(canonical, " approved=true", " approved"),
                 SIQSShadowProofRssPolicyRecordError::invalid_field);
    expect_error(replace_once(canonical, " approval_id=synthetic-approval-42", " approval_id="),
                 SIQSShadowProofRssPolicyRecordError::invalid_field);
    expect_error(replace_once(canonical, " candidate_revision=synthetic-revision-abc123",
                              " candidate_revision=synthetic=revision"),
                 SIQSShadowProofRssPolicyRecordError::invalid_field);
    expect_error(replace_once(canonical, " reserved_headroom_bytes=536870912", ""),
                 SIQSShadowProofRssPolicyRecordError::missing_field);
    expect_error(replace_once(canonical, "\n", " extra_field=1\n"),
                 SIQSShadowProofRssPolicyRecordError::unknown_field);
    expect_error(replace_once(canonical, "\n", " approved=true\n"),
                 SIQSShadowProofRssPolicyRecordError::duplicate_field);
}

void test_canonical_scalar_rejections() {
    const std::string canonical = canonical_record();
    expect_error(replace_once(canonical, "schema_version=3", "schema_version=4"),
                 SIQSShadowProofRssPolicyRecordError::invalid_schema_version);
    expect_error(replace_once(canonical, "schema_version=3", "schema_version=2"),
                 SIQSShadowProofRssPolicyRecordError::invalid_schema_version);
    for (const auto [field, canonical_value] :
         std::array{std::pair{std::string_view("probe_executable_sha256"), TEST_EXECUTABLE_SHA256},
                    std::pair{std::string_view("probe_execution_contract_sha256"),
                              TEST_EXECUTION_CONTRACT_SHA256}}) {
        const std::string assignment = std::string(field) + "=" + std::string(canonical_value);
        expect_error(
            replace_once(canonical, assignment,
                         std::string(field) + "=" + std::string(canonical_value.substr(0, 63))),
            SIQSShadowProofRssPolicyRecordError::invalid_sha256);
        expect_error(replace_once(canonical, assignment,
                                  std::string(field) + "=" + std::string(canonical_value) + "0"),
                     SIQSShadowProofRssPolicyRecordError::invalid_sha256);

        std::string uppercase(canonical_value);
        const std::size_t lowercase_hex = uppercase.find_first_of("abcdef");
        CHECK(lowercase_hex != std::string::npos);
        if (lowercase_hex != std::string::npos) {
            uppercase[lowercase_hex] = 'A';
        }
        expect_error(replace_once(canonical, assignment, std::string(field) + "=" + uppercase),
                     SIQSShadowProofRssPolicyRecordError::invalid_sha256);

        std::string non_hex(canonical_value);
        non_hex[0] = 'g';
        expect_error(replace_once(canonical, assignment, std::string(field) + "=" + non_hex),
                     SIQSShadowProofRssPolicyRecordError::invalid_sha256);
    }
    for (const std::string_view value : {"01", "+1", "-1", "1.0", "one"}) {
        expect_error(replace_once(canonical, "corpus_digest_low=123456789",
                                  std::string("corpus_digest_low=") + std::string(value)),
                     SIQSShadowProofRssPolicyRecordError::invalid_unsigned_integer);
    }
    expect_error(replace_once(canonical, "corpus_digest_low=123456789",
                              "corpus_digest_low=18446744073709551616"),
                 SIQSShadowProofRssPolicyRecordError::unsigned_integer_out_of_range);
    for (const auto [field, value] :
         std::array{std::pair{std::string_view("journal_trusted_base_id_low"),
                              std::string_view("1111111122222222")},
                    std::pair{std::string_view("journal_trusted_base_id_high"),
                              std::string_view("3333333344444444")},
                    std::pair{std::string_view("journal_store_id_low"),
                              std::string_view("1234123412341234")},
                    std::pair{std::string_view("journal_store_id_high"),
                              std::string_view("5678567856785678")}}) {
        const std::string assignment = std::string(field) + "=" + std::string(value);
        expect_error(replace_once(canonical, assignment, std::string(field) + "=01"),
                     SIQSShadowProofRssPolicyRecordError::invalid_unsigned_integer);
        expect_error(
            replace_once(canonical, assignment, std::string(field) + "=18446744073709551616"),
            SIQSShadowProofRssPolicyRecordError::unsigned_integer_out_of_range);
    }
    for (const std::string_view value : {"True", "FALSE", "1", "yes"}) {
        expect_error(
            replace_once(canonical, "approved=true", std::string("approved=") + std::string(value)),
            SIQSShadowProofRssPolicyRecordError::invalid_boolean);
    }
    expect_error(replace_once(canonical, "operating_system=darwin", "operating_system=unknown"),
                 SIQSShadowProofRssPolicyRecordError::invalid_operating_system);
    expect_error(replace_once(canonical, "operating_system=darwin", "operating_system=Darwin"),
                 SIQSShadowProofRssPolicyRecordError::invalid_operating_system);
    expect_error(replace_once(canonical, "architecture=arm64", "architecture=aarch64"),
                 SIQSShadowProofRssPolicyRecordError::invalid_architecture);
    expect_error(
        replace_once(canonical, "memory_backend=darwin_getrusage", "memory_backend=unsupported"),
        SIQSShadowProofRssPolicyRecordError::invalid_memory_backend);
    expect_error(replace_once(canonical, "memory_backend=darwin_getrusage",
                              "memory_backend=windows_psapi_v2"),
                 SIQSShadowProofRssPolicyRecordError::invalid_memory_backend);

    if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
        expect_error(replace_once(canonical, "resolved_production_sieve_workers=4",
                                  "resolved_production_sieve_workers=4294967296"),
                     SIQSShadowProofRssPolicyRecordError::unsigned_integer_out_of_range);
    }
}

void test_emitter_rejections_are_transactional() {
    auto policy = make_policy();
    policy.corpus_id = "";
    expect_emit_rejected(policy);
    policy = make_policy();
    policy.corpus_id = "two words";
    expect_emit_rejected(policy);
    policy = make_policy();
    policy.corpus_id = "a=b";
    expect_emit_rejected(policy);
    policy = make_policy();
    policy.candidate_revision = "line\nbreak";
    expect_emit_rejected(policy);
    policy = make_policy();
    const std::string non_ascii("bad\x80", 4);
    policy.approval_id = non_ascii;
    expect_emit_rejected(policy);
    policy = make_policy();
    policy.journal_store.relative_locator = "two words";
    expect_emit_rejected(policy);
    policy = make_policy();
    policy.journal_store.relative_locator = "a=b";
    expect_emit_rejected(policy);

    policy = make_policy();
    policy.operating_system = SIQSShadowProofRssOperatingSystem::unknown;
    expect_emit_rejected(policy);
    policy = make_policy();
    policy.operating_system = static_cast<SIQSShadowProofRssOperatingSystem>(255);
    expect_emit_rejected(policy);
    policy = make_policy();
    policy.architecture = SIQSShadowProofRssArchitecture::unknown;
    expect_emit_rejected(policy);
    policy = make_policy();
    policy.memory_backend = ProcessMemoryBackend::Unsupported;
    expect_emit_rejected(policy);
    policy = make_policy();
    policy.memory_backend = static_cast<ProcessMemoryBackend>(255);
    expect_emit_rejected(policy);

    policy = make_policy();
    policy.deployment_budget_bytes.reset();
    expect_emit_rejected(policy);
    policy = make_policy();
    policy.reserved_headroom_bytes.reset();
    expect_emit_rejected(policy);
}

} // namespace

int main() {
    test_constants_and_error_names();
    test_exact_emit_parse_and_gate_mapping();
    test_closed_platform_values_and_semantic_separation();
    test_framing_and_ascii_rejections();
    test_field_shape_order_and_cardinality_rejections();
    test_canonical_scalar_rejections();
    test_emitter_rejections_are_transactional();

    std::cout << "SIQS shadow proof RSS policy record: " << checks_passed << " passed, "
              << checks_failed << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
