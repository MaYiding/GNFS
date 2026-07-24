// Pure contracts for the sealed SIQS RSS holdout probe protocol. This test uses
// only the constexpr fixture manifest; it never calls factor(), captures RSS,
// or launches a process.

#include "shadow_proof_rss_holdout_probe_protocol_internal.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

using std::uint32_t;
using std::uint64_t;

using gnfs::siqs::shadow_proof_rss_holdout_detail::emit_siqs_shadow_proof_rss_holdout_probe_record;
using gnfs::siqs::shadow_proof_rss_holdout_detail::
    parse_siqs_shadow_proof_rss_holdout_probe_options;
using gnfs::siqs::shadow_proof_rss_holdout_detail::SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_DIGITS;
using gnfs::siqs::shadow_proof_rss_holdout_detail::SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_ERROR_PREFIX;
using gnfs::siqs::shadow_proof_rss_holdout_detail::siqs_shadow_proof_rss_holdout_probe_mode_name;
using gnfs::siqs::shadow_proof_rss_holdout_detail::
    siqs_shadow_proof_rss_holdout_probe_options_error_name;
using gnfs::siqs::shadow_proof_rss_holdout_detail::SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_PREFIX;
using gnfs::siqs::shadow_proof_rss_holdout_detail::
    siqs_shadow_proof_rss_holdout_probe_record_error_name;
using gnfs::siqs::shadow_proof_rss_holdout_detail::
    SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_SCHEMA_VERSION;
using gnfs::siqs::shadow_proof_rss_holdout_detail::SIQSShadowProofRssHoldoutProbeMode;
using gnfs::siqs::shadow_proof_rss_holdout_detail::SIQSShadowProofRssHoldoutProbeOptionsError;
using gnfs::siqs::shadow_proof_rss_holdout_detail::SIQSShadowProofRssHoldoutProbeOptionsParseResult;
using gnfs::siqs::shadow_proof_rss_holdout_detail::SIQSShadowProofRssHoldoutProbeRecord;
using gnfs::siqs::shadow_proof_rss_holdout_detail::SIQSShadowProofRssHoldoutProbeRecordError;
using gnfs::siqs::shadow_proof_rss_holdout_detail::
    validate_siqs_shadow_proof_rss_holdout_probe_record;

static_assert(SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_SCHEMA_VERSION == 1);
static_assert(SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_DIGITS == 50);
static_assert(SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_PREFIX ==
              "GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_V1");
static_assert(SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_ERROR_PREFIX ==
              "GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_ERROR_V1");

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':' << __LINE__ << '\n';        \
        }                                                                                          \
    } while (false)

[[nodiscard]] SIQSShadowProofRssHoldoutProbeOptionsParseResult
parse_args(std::initializer_list<std::string_view> arguments) {
    return parse_siqs_shadow_proof_rss_holdout_probe_options(
        std::span<const std::string_view>(arguments.begin(), arguments.size()));
}

void expect_options_error(std::initializer_list<std::string_view> arguments,
                          SIQSShadowProofRssHoldoutProbeOptionsError error,
                          std::string_view expected_argument) {
    const auto result = parse_args(arguments);
    CHECK(!result);
    CHECK(result.error == error);
    CHECK(result.argument == expected_argument);
}

[[nodiscard]] SIQSShadowProofRssHoldoutProbeRecord make_record() {
    const auto& fixture =
        gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1
            .back();
    SIQSShadowProofRssHoldoutProbeRecord record;
    record.fixture_id = fixture.id;
    record.mode = SIQSShadowProofRssHoldoutProbeMode::observe;
    record.ordinal = 7;
    record.environment_value = "observe";
    record.digits = 50;
    record.modulus = fixture.modulus;
    record.expected_factor = fixture.factor_p;
    record.expected_cofactor = fixture.factor_q;
    record.factor = record.expected_factor;
    record.cofactor = record.expected_cofactor;
    record.relations_found = 1701;
    record.polynomials_used = 9;
    record.resolved_production_sieve_workers = 4;
    record.factor_wall_ns = UINT64_C(123456789);
    record.rss_backend = "linux_getrusage";
    record.before_current_rss_supported = true;
    record.before_current_rss_bytes = UINT64_C(100);
    record.before_peak_rss_supported = true;
    record.before_peak_rss_bytes = UINT64_C(200);
    record.after_current_rss_supported = true;
    record.after_current_rss_bytes = UINT64_C(150);
    record.after_peak_rss_supported = true;
    record.after_peak_rss_bytes = UINT64_C(250);
    record.absolute_peak_rss_supported = true;
    record.absolute_peak_rss_bytes = UINT64_C(250);
    record.peak_growth_supported = true;
    record.peak_growth_bytes = UINT64_C(50);
    return record;
}

void expect_record_error(const SIQSShadowProofRssHoldoutProbeRecord& record,
                         SIQSShadowProofRssHoldoutProbeRecordError error) {
    CHECK(validate_siqs_shadow_proof_rss_holdout_probe_record(record) == error);
    std::string output = "sentinel";
    CHECK(!emit_siqs_shadow_proof_rss_holdout_probe_record(record, output));
    CHECK(output == "sentinel");
}

void test_names_and_cli_boundaries() {
    CHECK(siqs_shadow_proof_rss_holdout_probe_mode_name(
              SIQSShadowProofRssHoldoutProbeMode::unknown) == "unknown");
    CHECK(siqs_shadow_proof_rss_holdout_probe_mode_name(SIQSShadowProofRssHoldoutProbeMode::off) ==
          "off");
    CHECK(siqs_shadow_proof_rss_holdout_probe_mode_name(
              SIQSShadowProofRssHoldoutProbeMode::observe) == "observe");
    CHECK(siqs_shadow_proof_rss_holdout_probe_mode_name(
              static_cast<SIQSShadowProofRssHoldoutProbeMode>(255)) == "unknown");

    constexpr std::array option_errors{
        std::pair{SIQSShadowProofRssHoldoutProbeOptionsError::none, std::string_view("none")},
        std::pair{SIQSShadowProofRssHoldoutProbeOptionsError::unknown_argument,
                  std::string_view("unknown_argument")},
        std::pair{SIQSShadowProofRssHoldoutProbeOptionsError::duplicate_argument,
                  std::string_view("duplicate_argument")},
        std::pair{SIQSShadowProofRssHoldoutProbeOptionsError::argument_value_missing,
                  std::string_view("argument_value_missing")},
        std::pair{SIQSShadowProofRssHoldoutProbeOptionsError::required_argument_missing,
                  std::string_view("required_argument_missing")},
        std::pair{SIQSShadowProofRssHoldoutProbeOptionsError::fixture_id_invalid,
                  std::string_view("fixture_id_invalid")},
        std::pair{SIQSShadowProofRssHoldoutProbeOptionsError::mode_invalid,
                  std::string_view("mode_invalid")},
        std::pair{SIQSShadowProofRssHoldoutProbeOptionsError::ordinal_invalid,
                  std::string_view("ordinal_invalid")},
    };
    for (const auto& [error, name] : option_errors) {
        CHECK(siqs_shadow_proof_rss_holdout_probe_options_error_name(error) == name);
    }
    CHECK(siqs_shadow_proof_rss_holdout_probe_options_error_name(
              static_cast<SIQSShadowProofRssHoldoutProbeOptionsError>(255)) == "unknown");

    for (const std::string_view fixture : {"1", "8"}) {
        for (const std::string_view ordinal : {"1", "3"}) {
            const auto parsed =
                parse_args({"--fixture-id", fixture, "--mode", "off", "--ordinal", ordinal});
            CHECK(parsed);
            CHECK(parsed.argument.empty());
            CHECK(parsed.options.mode == SIQSShadowProofRssHoldoutProbeMode::off);
        }
    }
    for (const std::string_view ordinal : {"1", "7"}) {
        const auto parsed =
            parse_args({"--ordinal", ordinal, "--mode", "observe", "--fixture-id", "8"});
        CHECK(parsed);
        CHECK(parsed.argument.empty());
        CHECK(parsed.options.fixture_id == 8);
        CHECK(parsed.options.ordinal == static_cast<uint32_t>(ordinal.front() - '0'));
    }

    expect_options_error({}, SIQSShadowProofRssHoldoutProbeOptionsError::required_argument_missing,
                         "--fixture-id");
    expect_options_error({"--fixture-id"},
                         SIQSShadowProofRssHoldoutProbeOptionsError::argument_value_missing,
                         "--fixture-id");
    expect_options_error(
        {"--mode"}, SIQSShadowProofRssHoldoutProbeOptionsError::argument_value_missing, "--mode");
    expect_options_error({"--ordinal"},
                         SIQSShadowProofRssHoldoutProbeOptionsError::argument_value_missing,
                         "--ordinal");
    expect_options_error({"--fixture-id", "1", "--mode", "off"},
                         SIQSShadowProofRssHoldoutProbeOptionsError::required_argument_missing,
                         "--ordinal");
    expect_options_error({"--fixture-id", "1", "--ordinal", "1"},
                         SIQSShadowProofRssHoldoutProbeOptionsError::required_argument_missing,
                         "--mode");
    expect_options_error({"--sample-ordinal", "1"},
                         SIQSShadowProofRssHoldoutProbeOptionsError::unknown_argument,
                         "--sample-ordinal");
    expect_options_error(
        {"positional"}, SIQSShadowProofRssHoldoutProbeOptionsError::unknown_argument, "positional");
    expect_options_error(
        {"--fixture-id", "1", "--fixture-id", "2", "--mode", "off", "--ordinal", "1"},
        SIQSShadowProofRssHoldoutProbeOptionsError::duplicate_argument, "--fixture-id");
    expect_options_error(
        {"--fixture-id", "1", "--mode", "off", "--mode", "observe", "--ordinal", "1"},
        SIQSShadowProofRssHoldoutProbeOptionsError::duplicate_argument, "--mode");
    expect_options_error({"--fixture-id", "1", "--mode", "off", "--ordinal", "1", "--ordinal", "2"},
                         SIQSShadowProofRssHoldoutProbeOptionsError::duplicate_argument,
                         "--ordinal");

    for (const std::string_view value : {"0", "9", "01", "+1", "-1", "4294967296"}) {
        expect_options_error({"--fixture-id", value, "--mode", "off", "--ordinal", "1"},
                             SIQSShadowProofRssHoldoutProbeOptionsError::fixture_id_invalid, value);
    }
    for (const std::string_view value : {"Off", "OBSERVE", "unknown", "0"}) {
        expect_options_error({"--fixture-id", "1", "--mode", value, "--ordinal", "1"},
                             SIQSShadowProofRssHoldoutProbeOptionsError::mode_invalid, value);
    }
    for (const std::string_view value : {"0", "4", "01", "+1", "-1", "4294967296"}) {
        expect_options_error({"--fixture-id", "1", "--mode", "off", "--ordinal", value},
                             SIQSShadowProofRssHoldoutProbeOptionsError::ordinal_invalid, value);
    }
    for (const std::string_view value : {"0", "8", "01", "+1", "-1", "4294967296"}) {
        expect_options_error({"--fixture-id", "1", "--mode", "observe", "--ordinal", value},
                             SIQSShadowProofRssHoldoutProbeOptionsError::ordinal_invalid, value);
    }
}

void test_record_names_and_golden_emission() {
    constexpr std::array record_errors{
        std::pair{SIQSShadowProofRssHoldoutProbeRecordError::none, std::string_view("none")},
        std::pair{SIQSShadowProofRssHoldoutProbeRecordError::schema_invalid,
                  std::string_view("schema_invalid")},
        std::pair{SIQSShadowProofRssHoldoutProbeRecordError::corpus_binding_invalid,
                  std::string_view("corpus_binding_invalid")},
        std::pair{SIQSShadowProofRssHoldoutProbeRecordError::fixture_invalid,
                  std::string_view("fixture_invalid")},
        std::pair{SIQSShadowProofRssHoldoutProbeRecordError::mode_or_ordinal_invalid,
                  std::string_view("mode_or_ordinal_invalid")},
        std::pair{SIQSShadowProofRssHoldoutProbeRecordError::execution_contract_invalid,
                  std::string_view("execution_contract_invalid")},
        std::pair{SIQSShadowProofRssHoldoutProbeRecordError::fixture_identity_invalid,
                  std::string_view("fixture_identity_invalid")},
        std::pair{SIQSShadowProofRssHoldoutProbeRecordError::factor_identity_invalid,
                  std::string_view("factor_identity_invalid")},
        std::pair{SIQSShadowProofRssHoldoutProbeRecordError::result_metrics_invalid,
                  std::string_view("result_metrics_invalid")},
        std::pair{SIQSShadowProofRssHoldoutProbeRecordError::rss_backend_invalid,
                  std::string_view("rss_backend_invalid")},
        std::pair{SIQSShadowProofRssHoldoutProbeRecordError::rss_optional_value_invalid,
                  std::string_view("rss_optional_value_invalid")},
        std::pair{SIQSShadowProofRssHoldoutProbeRecordError::absolute_peak_invalid,
                  std::string_view("absolute_peak_invalid")},
        std::pair{SIQSShadowProofRssHoldoutProbeRecordError::peak_growth_invalid,
                  std::string_view("peak_growth_invalid")},
        std::pair{SIQSShadowProofRssHoldoutProbeRecordError::route_invalid,
                  std::string_view("route_invalid")},
    };
    for (const auto& [error, name] : record_errors) {
        CHECK(siqs_shadow_proof_rss_holdout_probe_record_error_name(error) == name);
    }
    CHECK(siqs_shadow_proof_rss_holdout_probe_record_error_name(
              static_cast<SIQSShadowProofRssHoldoutProbeRecordError>(255)) == "unknown");

    const auto record = make_record();
    CHECK(validate_siqs_shadow_proof_rss_holdout_probe_record(record) ==
          SIQSShadowProofRssHoldoutProbeRecordError::none);
    std::string emitted = "old";
    CHECK(emit_siqs_shadow_proof_rss_holdout_probe_record(record, emitted));
    const std::string expected =
        "GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_V1 schema_version=1 status=valid"
        " corpus_id=siqs50_shadow_observe_rss_holdout_v1"
        " corpus_digest_low=303806906129662515"
        " corpus_digest_high=18179245792498443738"
        " sealed_before_measurement=true used_for_calibration=false fixture_id=8"
        " mode=observe ordinal=7 build_type=Release ndebug=true fresh_process=true"
        " completed=true scope=production_factor_fresh_process env_value=observe digits=50"
        " n=17892763000000000000000537926000000000000000002491"
        " expected_factor=2177000000000000000000053"
        " expected_cofactor=8219000000000000000000047 max_seconds=30"
        " factor_status=factor_found factor=2177000000000000000000053"
        " cofactor=8219000000000000000000047 factor_identity=pass relations_found=1701"
        " polynomials_used=9 resolved_production_sieve_workers=4 factor_wall_ns=123456789"
        " rss_scope=self_lifetime rss_backend=linux_getrusage"
        " before_current_rss_supported=true before_current_rss_bytes=100"
        " before_peak_rss_supported=true before_peak_rss_bytes=200"
        " after_current_rss_supported=true after_current_rss_bytes=150"
        " after_peak_rss_supported=true after_peak_rss_bytes=250"
        " absolute_peak_rss_supported=true absolute_peak_rss_bytes=250"
        " peak_growth_supported=true peak_growth_bytes=50 route=legacy_result"
        " promotion=false\n";
    CHECK(emitted == expected);
    CHECK(emitted.find('\r') == std::string::npos);
    CHECK(emitted.find("proof_evidence=") == std::string::npos);
    CHECK(emitted.find("matrix_evidence=") == std::string::npos);
    CHECK(emitted.find("candidate_revision=") == std::string::npos);
    CHECK(emitted.find("approval_id=") == std::string::npos);
}

void test_record_rejections() {
    const auto valid = make_record();
    auto record = valid;
    record.schema_version = 2;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::schema_invalid);
    record = valid;
    record.status = "completed";
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::schema_invalid);

    record = valid;
    record.corpus_id = "wrong";
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::corpus_binding_invalid);
    record = valid;
    ++record.corpus_digest_low;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::corpus_binding_invalid);
    record = valid;
    ++record.corpus_digest_high;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::corpus_binding_invalid);
    record = valid;
    record.sealed_before_measurement = false;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::corpus_binding_invalid);
    record = valid;
    record.used_for_calibration = true;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::corpus_binding_invalid);

    for (const uint32_t fixture_id : {0U, 9U}) {
        record = valid;
        record.fixture_id = fixture_id;
        expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::fixture_invalid);
    }
    record = valid;
    record.fixture_id = 7;
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::fixture_identity_invalid);
    record = valid;
    record.mode = SIQSShadowProofRssHoldoutProbeMode::unknown;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::mode_or_ordinal_invalid);
    record = valid;
    record.ordinal = 0;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::mode_or_ordinal_invalid);
    record = valid;
    record.ordinal = 8;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::mode_or_ordinal_invalid);
    record = valid;
    record.environment_value = "0";
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::mode_or_ordinal_invalid);
    record = valid;
    record.mode = SIQSShadowProofRssHoldoutProbeMode::off;
    record.ordinal = 4;
    record.environment_value = "0";
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::mode_or_ordinal_invalid);

    record = valid;
    record.build_type = "Debug";
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::execution_contract_invalid);
    record = valid;
    record.ndebug = false;
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::execution_contract_invalid);
    record = valid;
    record.fresh_process = false;
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::execution_contract_invalid);
    record = valid;
    record.completed = false;
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::execution_contract_invalid);
    record = valid;
    record.scope = "batch";
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::execution_contract_invalid);
    record = valid;
    ++record.max_seconds;
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::execution_contract_invalid);

    record = valid;
    --record.digits;
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::fixture_identity_invalid);
    record = valid;
    record.digits = 49;
    record.modulus = "7010000000000000000000654600000000000000000005129";
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::fixture_identity_invalid);
    record = valid;
    record.modulus = "01";
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::fixture_identity_invalid);
    record = valid;
    record.modulus =
        gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1
            .front()
            .modulus;
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::fixture_identity_invalid);
    record = valid;
    record.expected_factor = "factor";
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::fixture_identity_invalid);
    record = valid;
    std::swap(record.expected_factor, record.expected_cofactor);
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::fixture_identity_invalid);

    record = valid;
    record.factor_status = "failed";
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::factor_identity_invalid);
    record = valid;
    record.factor = "2";
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::factor_identity_invalid);
    record = valid;
    record.cofactor = "3";
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::factor_identity_invalid);
    record = valid;
    record.factor_identity = "not_checked";
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::factor_identity_invalid);

    record = valid;
    record.relations_found = 0;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::result_metrics_invalid);
    record = valid;
    record.polynomials_used = 0;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::result_metrics_invalid);
    record = valid;
    record.resolved_production_sieve_workers = 0;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::result_metrics_invalid);
    record = valid;
    record.factor_wall_ns = 0;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::result_metrics_invalid);

    record = valid;
    record.rss_scope = "current";
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::rss_backend_invalid);
    for (const std::string_view backend : {"unsupported", "DarwinGetrusage", ""}) {
        record = valid;
        record.rss_backend = backend;
        expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::rss_backend_invalid);
    }

    record = valid;
    record.before_current_rss_supported = false;
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::rss_optional_value_invalid);
    record = valid;
    record.before_peak_rss_bytes = 0;
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::rss_optional_value_invalid);
    record = valid;
    record.after_current_rss_supported = false;
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::rss_optional_value_invalid);
    record = valid;
    record.after_peak_rss_bytes = 0;
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutProbeRecordError::rss_optional_value_invalid);

    record = valid;
    record.absolute_peak_rss_supported = false;
    record.absolute_peak_rss_bytes = 0;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::absolute_peak_invalid);
    record = valid;
    ++record.absolute_peak_rss_bytes;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::absolute_peak_invalid);

    record = valid;
    ++record.peak_growth_bytes;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::peak_growth_invalid);
    record = valid;
    record.after_peak_rss_bytes = record.before_peak_rss_bytes - 1;
    record.absolute_peak_rss_bytes = record.after_peak_rss_bytes;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::peak_growth_invalid);
    record = valid;
    record.before_peak_rss_supported = false;
    record.before_peak_rss_bytes = 0;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::peak_growth_invalid);

    record = valid;
    record.route = "shadow_return";
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::route_invalid);
    record = valid;
    record.promotion = true;
    expect_record_error(record, SIQSShadowProofRssHoldoutProbeRecordError::route_invalid);
}

void test_valid_platform_and_optional_variants() {
    for (const std::string_view backend :
         {"darwin_getrusage", "linux_getrusage", "windows_psapi"}) {
        auto record = make_record();
        record.rss_backend = backend;
        CHECK(validate_siqs_shadow_proof_rss_holdout_probe_record(record) ==
              SIQSShadowProofRssHoldoutProbeRecordError::none);
    }

    auto record = make_record();
    record.mode = SIQSShadowProofRssHoldoutProbeMode::off;
    record.ordinal = 3;
    record.environment_value = "0";
    record.before_current_rss_supported = false;
    record.before_current_rss_bytes = 0;
    record.before_peak_rss_supported = false;
    record.before_peak_rss_bytes = 0;
    record.after_current_rss_supported = false;
    record.after_current_rss_bytes = 0;
    record.peak_growth_supported = false;
    record.peak_growth_bytes = 0;
    CHECK(validate_siqs_shadow_proof_rss_holdout_probe_record(record) ==
          SIQSShadowProofRssHoldoutProbeRecordError::none);

    record = make_record();
    record.before_current_rss_bytes += 1;
    record.after_current_rss_bytes += 9;
    CHECK(validate_siqs_shadow_proof_rss_holdout_probe_record(record) ==
          SIQSShadowProofRssHoldoutProbeRecordError::none);

    record = make_record();
    constexpr uint64_t max_u64 = std::numeric_limits<uint64_t>::max();
    record.relations_found = max_u64;
    record.polynomials_used = max_u64;
    record.resolved_production_sieve_workers = max_u64;
    record.factor_wall_ns = max_u64;
    record.before_current_rss_bytes = max_u64;
    record.before_peak_rss_bytes = max_u64;
    record.after_current_rss_bytes = max_u64;
    record.after_peak_rss_bytes = max_u64;
    record.absolute_peak_rss_bytes = max_u64;
    record.peak_growth_bytes = 0;
    CHECK(validate_siqs_shadow_proof_rss_holdout_probe_record(record) ==
          SIQSShadowProofRssHoldoutProbeRecordError::none);
    std::string max_output;
    CHECK(emit_siqs_shadow_proof_rss_holdout_probe_record(record, max_output));
    CHECK(max_output.find(" relations_found=18446744073709551615") != std::string::npos);
    CHECK(max_output.find(" peak_growth_supported=true peak_growth_bytes=0") != std::string::npos);
}

} // namespace

int main() {
    test_names_and_cli_boundaries();
    test_record_names_and_golden_emission();
    test_record_rejections();
    test_valid_platform_and_optional_variants();

    std::cout << "SIQS shadow proof RSS holdout probe contract: " << checks_passed << " passed, "
              << checks_failed << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
