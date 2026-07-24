// Pure codec contracts for the sealed SIQS RSS holdout probe stdout record.
// This test does not launch a process, capture RSS, or call factor().

#include "shadow_proof_rss_holdout_probe_record_codec_internal.hpp"

#include <gnfs/util/process_memory.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using gnfs::siqs::shadow_proof_rss_holdout_detail::
    decode_siqs_shadow_proof_rss_holdout_probe_record;
using gnfs::siqs::shadow_proof_rss_holdout_detail::emit_siqs_shadow_proof_rss_holdout_probe_record;
using gnfs::siqs::shadow_proof_rss_holdout_detail::
    siqs_shadow_proof_rss_holdout_probe_record_codec_error_name;
using gnfs::siqs::shadow_proof_rss_holdout_detail::
    SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_FIELD_COUNT;
using gnfs::siqs::shadow_proof_rss_holdout_detail::
    SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_MAX_BYTES;
using gnfs::siqs::shadow_proof_rss_holdout_detail::
    SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD;
using gnfs::siqs::shadow_proof_rss_holdout_detail::SIQSShadowProofRssHoldoutProbeDecodedRecord;
using gnfs::siqs::shadow_proof_rss_holdout_detail::SIQSShadowProofRssHoldoutProbeMode;
using gnfs::siqs::shadow_proof_rss_holdout_detail::SIQSShadowProofRssHoldoutProbeRecord;
using gnfs::siqs::shadow_proof_rss_holdout_detail::SIQSShadowProofRssHoldoutProbeRecordCodecError;
using gnfs::util::ProcessMemoryBackend;

static_assert(SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_MAX_BYTES == 4096);
static_assert(SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_FIELD_COUNT == 45);
static_assert(
    std::is_same_v<decltype(SIQSShadowProofRssHoldoutProbeDecodedRecord::fixture_id), uint32_t>);
static_assert(std::is_same_v<decltype(SIQSShadowProofRssHoldoutProbeDecodedRecord::mode),
                             SIQSShadowProofRssHoldoutProbeMode>);
static_assert(
    std::is_same_v<decltype(SIQSShadowProofRssHoldoutProbeDecodedRecord::peak_growth_bytes),
                   std::optional<uint64_t>>);

constexpr std::size_t FIELD_SCHEMA_VERSION = 0;
constexpr std::size_t FIELD_STATUS = 1;
constexpr std::size_t FIELD_CORPUS_ID = 2;
constexpr std::size_t FIELD_CORPUS_DIGEST_LOW = 3;
constexpr std::size_t FIELD_SEALED_BEFORE_MEASUREMENT = 5;
constexpr std::size_t FIELD_USED_FOR_CALIBRATION = 6;
constexpr std::size_t FIELD_FIXTURE_ID = 7;
constexpr std::size_t FIELD_MODE = 8;
constexpr std::size_t FIELD_ORDINAL = 9;
constexpr std::size_t FIELD_BUILD_TYPE = 10;
constexpr std::size_t FIELD_NDEBUG = 11;
constexpr std::size_t FIELD_FRESH_PROCESS = 12;
constexpr std::size_t FIELD_COMPLETED = 13;
constexpr std::size_t FIELD_SCOPE = 14;
constexpr std::size_t FIELD_ENV_VALUE = 15;
constexpr std::size_t FIELD_DIGITS = 16;
constexpr std::size_t FIELD_MODULUS = 17;
constexpr std::size_t FIELD_EXPECTED_FACTOR = 18;
constexpr std::size_t FIELD_MAX_SECONDS = 20;
constexpr std::size_t FIELD_FACTOR_STATUS = 21;
constexpr std::size_t FIELD_FACTOR = 22;
constexpr std::size_t FIELD_FACTOR_IDENTITY = 24;
constexpr std::size_t FIELD_RELATIONS_FOUND = 25;
constexpr std::size_t FIELD_POLYNOMIALS_USED = 26;
constexpr std::size_t FIELD_WORKERS = 27;
constexpr std::size_t FIELD_FACTOR_WALL_NS = 28;
constexpr std::size_t FIELD_RSS_SCOPE = 29;
constexpr std::size_t FIELD_RSS_BACKEND = 30;
constexpr std::size_t FIELD_BEFORE_CURRENT_SUPPORTED = 31;
constexpr std::size_t FIELD_BEFORE_CURRENT_BYTES = 32;
constexpr std::size_t FIELD_BEFORE_PEAK_SUPPORTED = 33;
constexpr std::size_t FIELD_BEFORE_PEAK_BYTES = 34;
constexpr std::size_t FIELD_AFTER_CURRENT_SUPPORTED = 35;
constexpr std::size_t FIELD_AFTER_PEAK_SUPPORTED = 37;
constexpr std::size_t FIELD_AFTER_PEAK_BYTES = 38;
constexpr std::size_t FIELD_ABSOLUTE_PEAK_SUPPORTED = 39;
constexpr std::size_t FIELD_ABSOLUTE_PEAK_BYTES = 40;
constexpr std::size_t FIELD_PEAK_GROWTH_SUPPORTED = 41;
constexpr std::size_t FIELD_PEAK_GROWTH_BYTES = 42;
constexpr std::size_t FIELD_ROUTE = 43;
constexpr std::size_t FIELD_PROMOTION = 44;

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

[[nodiscard]] SIQSShadowProofRssHoldoutProbeRecord
make_record(uint32_t fixture_id = 8,
            SIQSShadowProofRssHoldoutProbeMode mode = SIQSShadowProofRssHoldoutProbeMode::observe,
            uint32_t ordinal = 7,
            ProcessMemoryBackend backend = ProcessMemoryBackend::LinuxGetrusage) {
    const auto& fixture = gnfs::siqs::shadow_proof_rss_holdout_fixture_detail::
        SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1[fixture_id - 1];
    SIQSShadowProofRssHoldoutProbeRecord record;
    record.fixture_id = fixture_id;
    record.mode = mode;
    record.ordinal = ordinal;
    record.environment_value = gnfs::siqs::shadow_proof_rss_holdout_detail::
        siqs_shadow_proof_rss_holdout_probe_environment_value(mode);
    record.digits =
        gnfs::siqs::shadow_proof_rss_holdout_detail::SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_DIGITS;
    record.modulus = fixture.modulus;
    record.expected_factor = fixture.factor_p;
    record.expected_cofactor = fixture.factor_q;
    record.factor = fixture.factor_p;
    record.cofactor = fixture.factor_q;
    record.relations_found = UINT64_C(1700) + fixture_id;
    record.polynomials_used = UINT64_C(9) + ordinal;
    record.resolved_production_sieve_workers = 4;
    record.factor_wall_ns = UINT64_C(123456789) + fixture_id + ordinal;
    record.rss_backend = gnfs::util::process_memory_backend_name(backend);
    record.before_current_rss_supported = true;
    record.before_current_rss_bytes = 100;
    record.before_peak_rss_supported = true;
    record.before_peak_rss_bytes = 200;
    record.after_current_rss_supported = true;
    record.after_current_rss_bytes = 150;
    record.after_peak_rss_supported = true;
    record.after_peak_rss_bytes = 250;
    record.absolute_peak_rss_supported = true;
    record.absolute_peak_rss_bytes = 250;
    record.peak_growth_supported = true;
    record.peak_growth_bytes = 50;
    return record;
}

[[nodiscard]] std::string emit_record(const SIQSShadowProofRssHoldoutProbeRecord& record) {
    std::string output;
    CHECK(emit_siqs_shadow_proof_rss_holdout_probe_record(record, output));
    return output;
}

[[nodiscard]] std::string replace_field_value(std::string record, std::string_view key,
                                              std::string_view replacement) {
    const std::string marker = " " + std::string(key) + "=";
    const std::size_t marker_offset = record.find(marker);
    CHECK(marker_offset != std::string::npos);
    if (marker_offset == std::string::npos) {
        return record;
    }
    const std::size_t value_begin = marker_offset + marker.size();
    const std::size_t value_end = record.find_first_of(" \n", value_begin);
    CHECK(value_end != std::string::npos);
    if (value_end == std::string::npos) {
        return record;
    }
    record.replace(value_begin, value_end - value_begin, replacement);
    return record;
}

[[nodiscard]] std::string rename_field(std::string record, std::string_view key,
                                       std::string_view replacement) {
    const std::string marker = " " + std::string(key) + "=";
    const std::size_t marker_offset = record.find(marker);
    CHECK(marker_offset != std::string::npos);
    if (marker_offset != std::string::npos) {
        record.replace(marker_offset + 1, key.size(), replacement);
    }
    return record;
}

[[nodiscard]] std::string erase_field(std::string record, std::string_view key) {
    const std::string marker = " " + std::string(key) + "=";
    const std::size_t marker_offset = record.find(marker);
    CHECK(marker_offset != std::string::npos);
    if (marker_offset == std::string::npos) {
        return record;
    }
    const std::size_t field_end = record.find_first_of(" \n", marker_offset + marker.size());
    CHECK(field_end != std::string::npos);
    if (field_end != std::string::npos) {
        record.erase(marker_offset, field_end - marker_offset);
    }
    return record;
}

[[nodiscard]] std::string swap_adjacent_fields(std::string record, std::string_view first_key,
                                               std::string_view second_key) {
    const std::string first_marker = " " + std::string(first_key) + "=";
    const std::string second_marker = " " + std::string(second_key) + "=";
    const std::size_t first_begin = record.find(first_marker);
    const std::size_t second_begin = record.find(second_marker);
    CHECK(first_begin != std::string::npos);
    CHECK(second_begin != std::string::npos);
    if (first_begin == std::string::npos || second_begin == std::string::npos ||
        second_begin <= first_begin) {
        return record;
    }
    const std::size_t first_token_begin = first_begin + 1;
    const std::size_t first_token_end = second_begin;
    const std::size_t second_token_begin = second_begin + 1;
    const std::size_t second_token_end =
        record.find_first_of(" \n", second_begin + second_marker.size());
    CHECK(second_token_end != std::string::npos);
    if (second_token_end == std::string::npos) {
        return record;
    }
    const std::string first = record.substr(first_token_begin, first_token_end - first_token_begin);
    const std::string second =
        record.substr(second_token_begin, second_token_end - second_token_begin);
    record.replace(first_token_begin, second_token_end - first_token_begin, second + " " + first);
    return record;
}

void expect_error(std::string_view input, SIQSShadowProofRssHoldoutProbeRecordCodecError error,
                  std::optional<std::size_t> expected_field = std::nullopt) {
    const auto decoded = decode_siqs_shadow_proof_rss_holdout_probe_record(input);
    CHECK(!decoded);
    CHECK(decoded.decoded() == nullptr);
    CHECK(decoded.diagnostic().error == error);
    if (expected_field.has_value()) {
        CHECK(decoded.diagnostic().field_index == *expected_field);
        CHECK(decoded.diagnostic().byte_offset < input.size());
    }
}

void expect_semantic_error(std::string input, std::size_t expected_field) {
    expect_error(input, SIQSShadowProofRssHoldoutProbeRecordCodecError::semantic_invalid,
                 expected_field);
}

void test_error_names_are_closed() {
    using Error = SIQSShadowProofRssHoldoutProbeRecordCodecError;
    constexpr std::array names{
        std::pair{Error::none, std::string_view("none")},
        std::pair{Error::input_empty, std::string_view("input_empty")},
        std::pair{Error::input_too_large, std::string_view("input_too_large")},
        std::pair{Error::final_lf_missing, std::string_view("final_lf_missing")},
        std::pair{Error::record_count_invalid, std::string_view("record_count_invalid")},
        std::pair{Error::byte_invalid, std::string_view("byte_invalid")},
        std::pair{Error::spacing_invalid, std::string_view("spacing_invalid")},
        std::pair{Error::prefix_invalid, std::string_view("prefix_invalid")},
        std::pair{Error::field_count_invalid, std::string_view("field_count_invalid")},
        std::pair{Error::field_token_invalid, std::string_view("field_token_invalid")},
        std::pair{Error::field_order_invalid, std::string_view("field_order_invalid")},
        std::pair{Error::field_value_empty, std::string_view("field_value_empty")},
        std::pair{Error::u32_invalid, std::string_view("u32_invalid")},
        std::pair{Error::u64_invalid, std::string_view("u64_invalid")},
        std::pair{Error::boolean_invalid, std::string_view("boolean_invalid")},
        std::pair{Error::mode_invalid, std::string_view("mode_invalid")},
        std::pair{Error::backend_invalid, std::string_view("backend_invalid")},
        std::pair{Error::semantic_invalid, std::string_view("semantic_invalid")},
    };
    for (const auto& [error, name] : names) {
        CHECK(siqs_shadow_proof_rss_holdout_probe_record_codec_error_name(error) == name);
    }
    CHECK(siqs_shadow_proof_rss_holdout_probe_record_codec_error_name(static_cast<Error>(255)) ==
          "unknown");
}

void test_emitter_round_trip_and_all_slots() {
    constexpr std::array backends{
        ProcessMemoryBackend::DarwinGetrusage,
        ProcessMemoryBackend::LinuxGetrusage,
        ProcessMemoryBackend::WindowsPsapi,
    };
    std::size_t slots = 0;
    for (uint32_t fixture_id = 1;
         fixture_id <= gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT; ++fixture_id) {
        for (const SIQSShadowProofRssHoldoutProbeMode mode :
             {SIQSShadowProofRssHoldoutProbeMode::off,
              SIQSShadowProofRssHoldoutProbeMode::observe}) {
            const uint32_t repetitions =
                mode == SIQSShadowProofRssHoldoutProbeMode::off
                    ? gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS
                    : gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS;
            for (uint32_t ordinal = 1; ordinal <= repetitions; ++ordinal) {
                const ProcessMemoryBackend backend = backends[slots % backends.size()];
                const auto source = make_record(fixture_id, mode, ordinal, backend);
                const std::string wire = emit_record(source);
                CHECK(wire.size() <= SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_MAX_BYTES);
                CHECK(std::count(wire.begin(), wire.end(), '\n') == 1);
                CHECK(wire.back() == '\n');

                const auto result = decode_siqs_shadow_proof_rss_holdout_probe_record(wire);
                CHECK(result);
                CHECK(result.diagnostic().error ==
                      SIQSShadowProofRssHoldoutProbeRecordCodecError::none);
                CHECK(result.diagnostic().field_index ==
                      SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_NO_FIELD);
                const auto* decoded = result.decoded();
                CHECK(decoded != nullptr);
                if (decoded != nullptr) {
                    CHECK(decoded->fixture_id == fixture_id);
                    CHECK(decoded->mode == mode);
                    CHECK(decoded->ordinal == ordinal);
                    CHECK(decoded->relations_found == source.relations_found);
                    CHECK(decoded->polynomials_used == source.polynomials_used);
                    CHECK(decoded->resolved_production_sieve_workers ==
                          source.resolved_production_sieve_workers);
                    CHECK(decoded->factor_wall_ns == source.factor_wall_ns);
                    CHECK(decoded->memory_backend == backend);
                    CHECK(decoded->before_memory.backend == backend);
                    CHECK(decoded->before_memory.current_rss_bytes == 100);
                    CHECK(decoded->before_memory.lifetime_peak_rss_bytes == 200);
                    CHECK(decoded->after_memory.backend == backend);
                    CHECK(decoded->after_memory.current_rss_bytes == 150);
                    CHECK(decoded->after_memory.lifetime_peak_rss_bytes == 250);
                    CHECK(decoded->absolute_peak_rss_bytes == 250);
                    CHECK(decoded->peak_growth_bytes == 50);
                }
                ++slots;
            }
        }
    }
    CHECK(slots == 80);
}

void test_valid_optional_rss_shapes() {
    auto record = make_record();
    record.before_current_rss_supported = false;
    record.before_current_rss_bytes = 0;
    record.after_current_rss_supported = false;
    record.after_current_rss_bytes = 0;
    record.before_peak_rss_supported = false;
    record.before_peak_rss_bytes = 0;
    record.peak_growth_supported = false;
    record.peak_growth_bytes = 0;
    const auto optional_result =
        decode_siqs_shadow_proof_rss_holdout_probe_record(emit_record(record));
    CHECK(optional_result);
    CHECK(optional_result.decoded() != nullptr);
    if (const auto* decoded = optional_result.decoded(); decoded != nullptr) {
        CHECK(!decoded->before_memory.current_rss_bytes.has_value());
        CHECK(!decoded->before_memory.lifetime_peak_rss_bytes.has_value());
        CHECK(!decoded->after_memory.current_rss_bytes.has_value());
        CHECK(decoded->after_memory.lifetime_peak_rss_bytes == 250);
        CHECK(!decoded->peak_growth_bytes.has_value());
    }

    record = make_record();
    record.before_peak_rss_bytes = 250;
    record.peak_growth_bytes = 0;
    const auto zero_growth_result =
        decode_siqs_shadow_proof_rss_holdout_probe_record(emit_record(record));
    CHECK(zero_growth_result);
    CHECK(zero_growth_result.decoded() != nullptr);
    if (const auto* decoded = zero_growth_result.decoded(); decoded != nullptr) {
        CHECK(decoded->peak_growth_bytes.has_value());
        CHECK(decoded->peak_growth_bytes == 0);
    }
}

void test_framing_and_schema_failures() {
    using Error = SIQSShadowProofRssHoldoutProbeRecordCodecError;
    const std::string valid = emit_record(make_record());

    expect_error("", Error::input_empty);
    expect_error(std::string(SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_MAX_BYTES + 1, 'x'),
                 Error::input_too_large);
    expect_error(std::string(SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_RECORD_MAX_BYTES - 1, 'x') + "\n",
                 Error::field_count_invalid);

    std::string mutation = valid;
    mutation.pop_back();
    expect_error(mutation, Error::final_lf_missing);
    expect_error(valid + "\n", Error::record_count_invalid);
    mutation = valid;
    mutation.insert(mutation.size() / 2, 1, '\n');
    expect_error(mutation, Error::record_count_invalid);
    mutation = valid;
    mutation.insert(mutation.size() / 2, 1, '\r');
    expect_error(mutation, Error::byte_invalid);
    mutation = valid;
    mutation[mutation.size() / 2] = '\0';
    expect_error(mutation, Error::byte_invalid);
    mutation = valid;
    mutation[mutation.size() / 2] = static_cast<char>(0x80);
    expect_error(mutation, Error::byte_invalid);

    expect_error(" " + valid, Error::spacing_invalid);
    mutation = valid;
    mutation.insert(mutation.size() - 1, 1, ' ');
    expect_error(mutation, Error::spacing_invalid);
    mutation = valid;
    mutation.insert(mutation.find(' ') + 1, 1, ' ');
    expect_error(mutation, Error::spacing_invalid);

    mutation = valid;
    mutation.front() = 'X';
    expect_error(mutation, Error::prefix_invalid);
    expect_error(erase_field(valid, "status"), Error::field_count_invalid);
    mutation = valid;
    mutation.insert(mutation.size() - 1, " extra=1");
    expect_error(mutation, Error::field_count_invalid);
    expect_error(rename_field(valid, "status", "state"), Error::field_order_invalid, FIELD_STATUS);
    expect_error(swap_adjacent_fields(valid, "status", "corpus_id"), Error::field_order_invalid,
                 FIELD_STATUS);

    mutation = valid;
    const std::size_t status_equals =
        mutation.find(" status=") + std::string_view(" status").size();
    mutation.erase(status_equals, 1);
    expect_error(mutation, Error::field_token_invalid, FIELD_STATUS);
    expect_error(replace_field_value(valid, "status", "=valid"), Error::field_token_invalid,
                 FIELD_STATUS);
    expect_error(replace_field_value(valid, "status", ""), Error::field_value_empty, FIELD_STATUS);
}

void test_canonical_scalar_and_closed_enum_failures() {
    using Error = SIQSShadowProofRssHoldoutProbeRecordCodecError;
    const std::string valid = emit_record(make_record());

    expect_error(replace_field_value(valid, "schema_version", "01"), Error::u32_invalid,
                 FIELD_SCHEMA_VERSION);
    expect_error(replace_field_value(valid, "fixture_id", "4294967296"), Error::u32_invalid,
                 FIELD_FIXTURE_ID);
    expect_error(replace_field_value(valid, "ordinal", "+7"), Error::u32_invalid, FIELD_ORDINAL);
    expect_error(replace_field_value(valid, "relations_found", "01708"), Error::u64_invalid,
                 FIELD_RELATIONS_FOUND);
    expect_error(replace_field_value(valid, "polynomials_used", "-1"), Error::u64_invalid,
                 FIELD_POLYNOMIALS_USED);
    expect_error(replace_field_value(valid, "factor_wall_ns", "18446744073709551616"),
                 Error::u64_invalid, FIELD_FACTOR_WALL_NS);
    expect_error(replace_field_value(valid, "ndebug", "True"), Error::boolean_invalid,
                 FIELD_NDEBUG);
    expect_error(replace_field_value(valid, "promotion", "0"), Error::boolean_invalid,
                 FIELD_PROMOTION);
    expect_error(replace_field_value(valid, "mode", "shadow"), Error::mode_invalid, FIELD_MODE);
    expect_error(replace_field_value(valid, "rss_backend", "unsupported"), Error::backend_invalid,
                 FIELD_RSS_BACKEND);
    expect_error(replace_field_value(valid, "rss_backend", "linux_procfs"), Error::backend_invalid,
                 FIELD_RSS_BACKEND);
}

void test_all_semantic_validation_categories() {
    const std::string valid = emit_record(make_record());

    expect_semantic_error(replace_field_value(valid, "schema_version", "2"), FIELD_SCHEMA_VERSION);
    expect_semantic_error(replace_field_value(valid, "status", "invalid"), FIELD_STATUS);
    expect_semantic_error(replace_field_value(valid, "corpus_id", "other"), FIELD_CORPUS_ID);
    expect_semantic_error(replace_field_value(valid, "corpus_digest_low", "1"),
                          FIELD_CORPUS_DIGEST_LOW);
    expect_semantic_error(replace_field_value(valid, "sealed_before_measurement", "false"),
                          FIELD_SEALED_BEFORE_MEASUREMENT);
    expect_semantic_error(replace_field_value(valid, "used_for_calibration", "true"),
                          FIELD_USED_FOR_CALIBRATION);
    expect_semantic_error(replace_field_value(valid, "fixture_id", "9"), FIELD_FIXTURE_ID);
    expect_semantic_error(replace_field_value(valid, "ordinal", "8"), FIELD_ORDINAL);
    expect_semantic_error(replace_field_value(valid, "env_value", "0"), FIELD_ENV_VALUE);

    expect_semantic_error(replace_field_value(valid, "build_type", "Debug"), FIELD_BUILD_TYPE);
    expect_semantic_error(replace_field_value(valid, "ndebug", "false"), FIELD_NDEBUG);
    expect_semantic_error(replace_field_value(valid, "fresh_process", "false"),
                          FIELD_FRESH_PROCESS);
    expect_semantic_error(replace_field_value(valid, "completed", "false"), FIELD_COMPLETED);
    expect_semantic_error(replace_field_value(valid, "scope", "whole_process"), FIELD_SCOPE);
    expect_semantic_error(replace_field_value(valid, "max_seconds", "119"), FIELD_MAX_SECONDS);

    expect_semantic_error(replace_field_value(valid, "digits", "49"), FIELD_DIGITS);
    expect_semantic_error(replace_field_value(valid, "n", "1"), FIELD_MODULUS);
    expect_semantic_error(replace_field_value(valid, "expected_factor", "3"),
                          FIELD_EXPECTED_FACTOR);
    expect_semantic_error(replace_field_value(valid, "factor_status", "no_factor"),
                          FIELD_FACTOR_STATUS);
    expect_semantic_error(replace_field_value(valid, "factor", "3"), FIELD_FACTOR);
    expect_semantic_error(replace_field_value(valid, "factor_identity", "fail"),
                          FIELD_FACTOR_IDENTITY);

    expect_semantic_error(replace_field_value(valid, "relations_found", "0"),
                          FIELD_RELATIONS_FOUND);
    expect_semantic_error(replace_field_value(valid, "polynomials_used", "0"),
                          FIELD_POLYNOMIALS_USED);
    expect_semantic_error(replace_field_value(valid, "resolved_production_sieve_workers", "0"),
                          FIELD_WORKERS);
    expect_semantic_error(replace_field_value(valid, "factor_wall_ns", "0"), FIELD_FACTOR_WALL_NS);
    expect_semantic_error(replace_field_value(valid, "rss_scope", "children"), FIELD_RSS_SCOPE);

    expect_semantic_error(replace_field_value(valid, "before_current_rss_bytes", "0"),
                          FIELD_BEFORE_CURRENT_SUPPORTED);
    expect_semantic_error(
        replace_field_value(replace_field_value(valid, "before_current_rss_supported", "false"),
                            "before_current_rss_bytes", "100"),
        FIELD_BEFORE_CURRENT_SUPPORTED);
    expect_semantic_error(replace_field_value(valid, "before_peak_rss_bytes", "0"),
                          FIELD_BEFORE_PEAK_SUPPORTED);
    expect_semantic_error(replace_field_value(valid, "after_current_rss_supported", "false"),
                          FIELD_AFTER_CURRENT_SUPPORTED);
    expect_semantic_error(replace_field_value(valid, "after_peak_rss_supported", "false"),
                          FIELD_AFTER_PEAK_SUPPORTED);

    expect_semantic_error(replace_field_value(valid, "absolute_peak_rss_supported", "false"),
                          FIELD_ABSOLUTE_PEAK_SUPPORTED);
    expect_semantic_error(replace_field_value(valid, "absolute_peak_rss_bytes", "249"),
                          FIELD_ABSOLUTE_PEAK_BYTES);
    expect_semantic_error(
        replace_field_value(replace_field_value(valid, "after_peak_rss_supported", "false"),
                            "after_peak_rss_bytes", "0"),
        FIELD_AFTER_PEAK_SUPPORTED);
    expect_semantic_error(
        replace_field_value(replace_field_value(valid, "after_peak_rss_bytes", "199"),
                            "absolute_peak_rss_bytes", "199"),
        FIELD_AFTER_PEAK_BYTES);
    expect_semantic_error(replace_field_value(valid, "peak_growth_supported", "false"),
                          FIELD_PEAK_GROWTH_SUPPORTED);
    expect_semantic_error(replace_field_value(valid, "peak_growth_bytes", "49"),
                          FIELD_PEAK_GROWTH_BYTES);
    expect_semantic_error(
        replace_field_value(replace_field_value(valid, "before_peak_rss_supported", "false"),
                            "before_peak_rss_bytes", "0"),
        FIELD_PEAK_GROWTH_SUPPORTED);
    expect_semantic_error(replace_field_value(valid, "route", "proof"), FIELD_ROUTE);
    expect_semantic_error(replace_field_value(valid, "promotion", "true"), FIELD_PROMOTION);

    (void)FIELD_BEFORE_CURRENT_BYTES;
    (void)FIELD_BEFORE_PEAK_BYTES;
    (void)FIELD_AFTER_PEAK_BYTES;
}

} // namespace

int main() {
    test_error_names_are_closed();
    test_emitter_round_trip_and_all_slots();
    test_valid_optional_rss_shapes();
    test_framing_and_schema_failures();
    test_canonical_scalar_and_closed_enum_failures();
    test_all_semantic_validation_categories();

    std::cout << "SIQS RSS holdout probe record codec checks passed: " << checks_passed << '\n';
    if (checks_failed != 0) {
        std::cerr << "SIQS RSS holdout probe record codec checks failed: " << checks_failed << '\n';
        return 1;
    }
    return 0;
}
