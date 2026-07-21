#include "gnfs/core/params.hpp"
#include "gnfs/core/polynomial_context.hpp"
#include "gnfs/core/relation.hpp"
#include "gnfs/factor_base/factor_base.hpp"
#include "gnfs/relation/ooc_relation_store.hpp"
#include "gnfs/sieve/sieve_checkpoint.hpp"
#include "gnfs/sieve/sieve_run_identity.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"
#include "support/child_process.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using gnfs::relation::OOCRelationWriter;
using gnfs::sieve::SieveCheckpoint;
using gnfs::sieve::SieveRunIdentity;

namespace {

[[noreturn]] void fail_check(const char* expression, const char* file, int line) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": CHECK failed: " + expression);
}

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fail_check(#expression, __FILE__, __LINE__);                                           \
        }                                                                                          \
    } while (false)

template <typename Function> void check_throws(Function&& function) {
    bool threw = false;
    try {
        std::forward<Function>(function)();
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

std::string checkpoint_path(const char* label) {
    static unsigned sequence = 0;
    return gnfs::util::temp_path("gnfs_test_sieve_ckpt_" +
                                 std::to_string(gnfs::util::process_id()) + "_" +
                                 std::to_string(++sequence) + "_" + label);
}

struct CheckpointCleanup {
    std::string path;

    ~CheckpointCleanup() {
        if (!path.empty()) {
            SieveCheckpoint::remove(path);
        }
    }
};

struct PairedArtifactsCleanup {
    std::string base_path;

    ~PairedArtifactsCleanup() {
        if (!base_path.empty()) {
            SieveCheckpoint::remove(base_path + ".sieve_ckpt");
            std::remove((base_path + ".reldata").c_str());
            std::remove((base_path + ".relidx").c_str());
        }
    }
};

SieveCheckpoint make_checkpoint(uint64_t generation = 7) {
    SieveCheckpoint checkpoint;
    checkpoint.sq_count = 12'345 + generation;
    checkpoint.current_index = 6'789;
    checkpoint.round = 3;
    checkpoint.batch_target = 1'500'000;
    checkpoint.candidates_total = 9'876'543;
    checkpoint.run_n = "1000036000099";
    checkpoint.run_fingerprint_lo = 0x44e2'83f1'9a57'c60bULL;
    checkpoint.run_fingerprint_hi = 0xc317'ba25'008d'71e4ULL;
    checkpoint.ooc_format_version = OOCRelationWriter::FORMAT_VERSION_V3;
    checkpoint.ooc_store_id = 0x1234'5678'9abc'def0ULL;
    checkpoint.ooc_generation = generation;
    checkpoint.ooc_relation_count = 41;
    checkpoint.ooc_data_end = 8'192;
    checkpoint.ooc_base_path = "portable-relation-store";
    return checkpoint;
}

gnfs::core::Relation make_ooc_relation(int64_t a) {
    gnfs::core::Relation relation(a, static_cast<uint64_t>(a + 1));
    relation.rational_factors.push_back(static_cast<uint32_t>(100 + a));
    return relation;
}

SieveCheckpoint make_paired_checkpoint(const gnfs::relation::OOCSnapshotDescriptor& descriptor,
                                       const std::string& base_path) {
    auto checkpoint = make_checkpoint(descriptor.generation);
    checkpoint.ooc_format_version = descriptor.format_version;
    checkpoint.ooc_store_id = descriptor.store_id;
    checkpoint.ooc_generation = descriptor.generation;
    checkpoint.ooc_relation_count = descriptor.count;
    checkpoint.ooc_data_end = descriptor.data_end;
    checkpoint.ooc_base_path = base_path;
    return checkpoint;
}

gnfs::relation::OOCSnapshotDescriptor descriptor_from(const SieveCheckpoint& checkpoint) {
    return gnfs::relation::OOCSnapshotDescriptor{
        .format_version = checkpoint.ooc_format_version,
        .store_id = checkpoint.ooc_store_id,
        .generation = checkpoint.ooc_generation,
        .count = checkpoint.ooc_relation_count,
        .data_end = checkpoint.ooc_data_end,
    };
}

void check_equal(const SieveCheckpoint& actual, const SieveCheckpoint& expected) {
    CHECK(actual == expected);
    CHECK(actual.sq_count == expected.sq_count);
    CHECK(actual.current_index == expected.current_index);
    CHECK(actual.round == expected.round);
    CHECK(actual.batch_target == expected.batch_target);
    CHECK(actual.candidates_total == expected.candidates_total);
    CHECK(actual.run_n == expected.run_n);
    CHECK(actual.run_fingerprint_lo == expected.run_fingerprint_lo);
    CHECK(actual.run_fingerprint_hi == expected.run_fingerprint_hi);
    CHECK(actual.ooc_format_version == expected.ooc_format_version);
    CHECK(actual.ooc_store_id == expected.ooc_store_id);
    CHECK(actual.ooc_generation == expected.ooc_generation);
    CHECK(actual.ooc_relation_count == expected.ooc_relation_count);
    CHECK(actual.ooc_data_end == expected.ooc_data_end);
    CHECK(actual.ooc_base_path == expected.ooc_base_path);
}

gnfs::core::GNFSParams make_identity_params() {
    gnfs::core::GNFSParams params;
    params.bits = 41;
    params.digits = 13;
    params.degree = 2;
    params.rational_bound = 100;
    params.algebraic_bound = 200;
    params.large_prime_bound = 10'000;
    params.large_prime_bits = 14;
    params.log_scale = 16;
    params.sieve_i_min = -128;
    params.sieve_i_max = 127;
    params.sieve_j_min = 1;
    params.sieve_j_max = 64;
    params.rational_threshold = 72;
    params.algebraic_threshold = 88;
    params.special_q_min = 211;
    params.special_q_max = 997;
    params.max_special_q = 400;
    params.target_excess = 123;
    return params;
}

struct RunIdentityFixture {
    std::string n = "1000036000099";
    std::string m = "10001";
    int coefficient_zero = -5;
    double skewness = 1.25;
    gnfs::core::FactorBaseParams fb_params{100, 200, 10'000, 16};
    bool reverse_rational_order = false;
    uint32_t rational_log_delta = 0;
    uint32_t algebraic_root_delta = 0;
    size_t sieve_algebraic_count = 2;
    gnfs::core::GNFSParams params = make_identity_params();
};

SieveRunIdentity identity_from(const RunIdentityFixture& fixture) {
    std::vector<gnfs::core::Integer> coefficients;
    coefficients.emplace_back(static_cast<int64_t>(fixture.coefficient_zero));
    coefficients.emplace_back(static_cast<int64_t>(3));
    coefficients.emplace_back(static_cast<int64_t>(1));
    gnfs::core::PolynomialContext context(gnfs::core::Integer(fixture.n), std::move(coefficients),
                                          gnfs::core::Integer(fixture.m), fixture.skewness);

    gnfs::factor_base::FactorBase factor_base(fixture.fb_params);
    if (fixture.reverse_rational_order) {
        factor_base.add_rational(3, 25);
        factor_base.add_rational(2, 16 + fixture.rational_log_delta);
    } else {
        factor_base.add_rational(2, 16 + fixture.rational_log_delta);
        factor_base.add_rational(3, 25);
    }
    factor_base.add_algebraic(5, 1 + fixture.algebraic_root_delta, 37, 1);
    factor_base.add_algebraic(7, 2, 44, 1);
    factor_base.add_algebraic(11, 4, 55, 2);
    factor_base.set_sieve_algebraic_count(fixture.sieve_algebraic_count);

    return gnfs::sieve::make_sieve_run_identity(context, factor_base, fixture.params);
}

std::vector<uint8_t> read_bytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    CHECK(input.good());
    const auto end = input.tellg();
    CHECK(end >= 0);
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    CHECK(input.good());
    return bytes;
}

void write_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    CHECK(output.good());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.flush();
    CHECK(output.good());
}

void write_u64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    CHECK(offset <= bytes.size());
    CHECK(8 <= bytes.size() - offset);
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes[offset++] = static_cast<uint8_t>((value >> shift) & 0xffULL);
    }
}

void write_u32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    CHECK(offset <= bytes.size());
    CHECK(4 <= bytes.size() - offset);
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes[offset++] = static_cast<uint8_t>((value >> shift) & 0xffU);
    }
}

uint64_t checksum(const uint8_t* data, size_t size) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

void refresh_checksum(std::vector<uint8_t>& bytes) {
    CHECK(bytes.size() >= 16);
    const size_t checksum_offset = bytes.size() - 8;
    write_u64(bytes, checksum_offset, checksum(bytes.data() + 8, checksum_offset - 8));
}

void test_v2_round_trip() {
    const auto path = checkpoint_path("round_trip");
    CheckpointCleanup cleanup{path};

    const auto original = make_checkpoint();
    original.save(path);

    CHECK(SieveCheckpoint::exists(path));
    CHECK(SieveCheckpoint::exists_and_valid(path));
    check_equal(SieveCheckpoint::load(path), original);
    CHECK(!SieveCheckpoint::exists(SieveCheckpoint::temporary_path(path)));
}

void test_run_identity_is_stable_and_matches_checkpoint() {
    const auto first = identity_from(RunIdentityFixture{});
    const auto second = identity_from(RunIdentityFixture{});

    CHECK(first == second);
    CHECK(first.run_n == "1000036000099");
    CHECK(first.fingerprint_lo != 0);
    CHECK(first.fingerprint_hi != 0);

    auto checkpoint = make_checkpoint();
    checkpoint.run_n = first.run_n;
    checkpoint.run_fingerprint_lo = first.fingerprint_lo;
    checkpoint.run_fingerprint_hi = first.fingerprint_hi;
    CHECK(checkpoint.matches_run_identity(first));

    auto mismatch = first;
    mismatch.fingerprint_hi ^= 1;
    CHECK(!checkpoint.matches_run_identity(mismatch));
    mismatch = first;
    mismatch.run_n.push_back('7');
    CHECK(!checkpoint.matches_run_identity(mismatch));
}

void test_run_identity_mutations_change_fingerprint() {
    const auto baseline = identity_from(RunIdentityFixture{});

    auto expect_changed = [&](const auto& mutate) {
        RunIdentityFixture fixture;
        mutate(fixture);
        const auto changed = identity_from(fixture);
        CHECK(changed != baseline);
        CHECK(changed.fingerprint_lo != 0);
        CHECK(changed.fingerprint_hi != 0);
    };

    expect_changed([](auto& fixture) { fixture.n = "1000036000101"; });
    expect_changed([](auto& fixture) { fixture.m = "10003"; });
    expect_changed([](auto& fixture) { ++fixture.coefficient_zero; });
    expect_changed([](auto& fixture) { fixture.skewness = 1.5; });
    expect_changed([](auto& fixture) { ++fixture.fb_params.rational_bound; });
    expect_changed([](auto& fixture) { ++fixture.rational_log_delta; });
    expect_changed([](auto& fixture) { fixture.reverse_rational_order = true; });
    expect_changed([](auto& fixture) { ++fixture.algebraic_root_delta; });
    expect_changed([](auto& fixture) { fixture.sieve_algebraic_count = 1; });

    auto expect_param_changed = [&](const auto& mutate) {
        expect_changed([&](RunIdentityFixture& fixture) { mutate(fixture.params); });
    };
    expect_param_changed([](auto& params) { ++params.bits; });
    expect_param_changed([](auto& params) { ++params.digits; });
    expect_param_changed([](auto& params) { ++params.degree; });
    expect_param_changed([](auto& params) { ++params.rational_bound; });
    expect_param_changed([](auto& params) { ++params.algebraic_bound; });
    expect_param_changed([](auto& params) { ++params.large_prime_bound; });
    expect_param_changed([](auto& params) { ++params.large_prime_bits; });
    expect_param_changed([](auto& params) { ++params.log_scale; });
    expect_param_changed([](auto& params) { --params.sieve_i_min; });
    expect_param_changed([](auto& params) { ++params.sieve_i_max; });
    expect_param_changed([](auto& params) { ++params.sieve_j_min; });
    expect_param_changed([](auto& params) { ++params.sieve_j_max; });
    expect_param_changed([](auto& params) { ++params.rational_threshold; });
    expect_param_changed([](auto& params) { ++params.algebraic_threshold; });
    expect_param_changed([](auto& params) { ++params.special_q_min; });
    expect_param_changed([](auto& params) { ++params.special_q_max; });
    expect_param_changed([](auto& params) { ++params.max_special_q; });
    expect_param_changed([](auto& params) { ++params.target_excess; });
}

void test_empty_committed_prefix_round_trip() {
    const auto path = checkpoint_path("empty_prefix");
    CheckpointCleanup cleanup{path};

    auto original = make_checkpoint();
    original.ooc_relation_count = 0;
    original.ooc_data_end = OOCRelationWriter::DATA_HEADER_BYTES;
    original.save(path);

    check_equal(SieveCheckpoint::load(path), original);
}

void test_long_portable_path_round_trip() {
    const auto path = checkpoint_path("long_path");
    CheckpointCleanup cleanup{path};

    auto original = make_checkpoint();
    original.ooc_base_path = std::string(2'048, 'x');
    original.save(path);
    check_equal(SieveCheckpoint::load(path), original);
}

void test_invalid_state_rejected_before_write() {
    const auto path = checkpoint_path("invalid_state");
    CheckpointCleanup cleanup{path};

    auto expect_invalid = [&](const std::function<void(SieveCheckpoint&)>& mutate) {
        auto checkpoint = make_checkpoint();
        mutate(checkpoint);
        check_throws([&] { checkpoint.save(path); });
        CHECK(!SieveCheckpoint::exists(path));
        CHECK(!SieveCheckpoint::exists(SieveCheckpoint::temporary_path(path)));
    };

    expect_invalid([](auto& checkpoint) { checkpoint.round = -1; });
    expect_invalid([](auto& checkpoint) { checkpoint.batch_target = 0; });
    expect_invalid([](auto& checkpoint) { checkpoint.run_n.clear(); });
    expect_invalid([](auto& checkpoint) { checkpoint.run_n = "012345"; });
    expect_invalid([](auto& checkpoint) { checkpoint.run_n = "12x345"; });
    expect_invalid([](auto& checkpoint) { checkpoint.run_n = "0"; });
    expect_invalid([](auto& checkpoint) { checkpoint.run_fingerprint_lo = 0; });
    expect_invalid([](auto& checkpoint) { checkpoint.run_fingerprint_hi = 0; });
    expect_invalid([](auto& checkpoint) {
        checkpoint.run_n.assign(static_cast<size_t>(SieveCheckpoint::MAX_RUN_N_LENGTH) + 1, '7');
    });
    expect_invalid([](auto& checkpoint) { checkpoint.ooc_format_version = 0; });
    expect_invalid([](auto& checkpoint) {
        checkpoint.ooc_format_version = OOCRelationWriter::FORMAT_VERSION_V2;
    });
    expect_invalid([](auto& checkpoint) { checkpoint.ooc_store_id = 0; });
    expect_invalid([](auto& checkpoint) { checkpoint.ooc_generation = 0; });
    expect_invalid([](auto& checkpoint) {
        checkpoint.ooc_relation_count = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
    });
    expect_invalid([](auto& checkpoint) {
        checkpoint.ooc_relation_count = std::numeric_limits<uint64_t>::max();
    });
    expect_invalid([](auto& checkpoint) { checkpoint.ooc_relation_count = 0; });
    expect_invalid([](auto& checkpoint) { checkpoint.ooc_data_end = 0; });
    expect_invalid(
        [](auto& checkpoint) { checkpoint.ooc_data_end = std::numeric_limits<uint64_t>::max(); });
    expect_invalid(
        [](auto& checkpoint) { checkpoint.ooc_data_end = OOCRelationWriter::DATA_HEADER_BYTES; });
    expect_invalid([](auto& checkpoint) { checkpoint.ooc_base_path.clear(); });
    expect_invalid([](auto& checkpoint) {
        checkpoint.ooc_base_path.assign(static_cast<size_t>(SieveCheckpoint::MAX_PATH_LENGTH) + 1,
                                        'x');
    });
    expect_invalid(
        [](auto& checkpoint) { checkpoint.ooc_base_path = std::string("bad\0path", 8); });
}

void test_incomplete_magic_rejected() {
    const auto path = checkpoint_path("incomplete");
    CheckpointCleanup cleanup{path};
    make_checkpoint().save(path);

    auto bytes = read_bytes(path);
    write_u64(bytes, 0, SieveCheckpoint::MAGIC_INCOMPLETE);
    write_bytes(path, bytes);

    CHECK(SieveCheckpoint::exists(path));
    CHECK(!SieveCheckpoint::exists_and_valid(path));
    check_throws([&] { (void)SieveCheckpoint::load(path); });
}

void test_v1_rejected_even_with_valid_checksum() {
    const auto path = checkpoint_path("v1");
    CheckpointCleanup cleanup{path};
    make_checkpoint().save(path);

    auto bytes = read_bytes(path);
    write_u64(bytes, 8, 1);
    refresh_checksum(bytes);
    write_bytes(path, bytes);

    check_throws([&] { (void)SieveCheckpoint::load(path); });
    CHECK(!SieveCheckpoint::exists_and_valid(path));
}

void test_legacy_v2_ooc_rejected_even_with_valid_checksum() {
    const auto path = checkpoint_path("legacy_ooc_v2");
    CheckpointCleanup cleanup{path};
    make_checkpoint().save(path);

    auto bytes = read_bytes(path);
    write_u64(bytes, SieveCheckpoint::WIRE_OOC_FORMAT_VERSION_OFFSET,
              OOCRelationWriter::FORMAT_VERSION_V2);
    refresh_checksum(bytes);
    write_bytes(path, bytes);

    check_throws([&] { (void)SieveCheckpoint::load(path); });
    CHECK(!SieveCheckpoint::exists_and_valid(path));
}

void test_checksum_corruption_rejected() {
    const auto path = checkpoint_path("checksum");
    CheckpointCleanup cleanup{path};
    make_checkpoint().save(path);

    auto bytes = read_bytes(path);
    bytes[SieveCheckpoint::WIRE_RUN_FINGERPRINT_LO_OFFSET] ^= 0x80U;
    write_bytes(path, bytes);
    check_throws([&] { (void)SieveCheckpoint::load(path); });
}

void test_truncation_rejected() {
    const auto path = checkpoint_path("truncated");
    CheckpointCleanup cleanup{path};
    make_checkpoint().save(path);

    auto bytes = read_bytes(path);
    bytes.resize(bytes.size() - 1);
    write_bytes(path, bytes);
    check_throws([&] { (void)SieveCheckpoint::load(path); });
}

void test_checksummed_trailing_byte_rejected() {
    const auto path = checkpoint_path("trailing");
    CheckpointCleanup cleanup{path};
    make_checkpoint().save(path);

    auto bytes = read_bytes(path);
    bytes.insert(bytes.end() - 8, 0x42U);
    refresh_checksum(bytes);
    write_bytes(path, bytes);
    check_throws([&] { (void)SieveCheckpoint::load(path); });
}

void test_checksummed_malformed_fields_rejected() {
    const auto path = checkpoint_path("malformed");
    CheckpointCleanup cleanup{path};
    make_checkpoint().save(path);

    // The format exports offsets so these mutations stay synchronized with the
    // parser's fixed prefix while preserving exact byte-level coverage.
    auto bytes = read_bytes(path);
    write_u64(bytes, SieveCheckpoint::WIRE_RUN_FINGERPRINT_LO_OFFSET, 0);
    refresh_checksum(bytes);
    write_bytes(path, bytes);
    check_throws([&] { (void)SieveCheckpoint::load(path); });

    make_checkpoint().save(path);
    bytes = read_bytes(path);
    write_u64(bytes, SieveCheckpoint::WIRE_OOC_GENERATION_OFFSET, 0);
    refresh_checksum(bytes);
    write_bytes(path, bytes);
    check_throws([&] { (void)SieveCheckpoint::load(path); });

    make_checkpoint().save(path);
    bytes = read_bytes(path);
    write_u32(bytes, SieveCheckpoint::WIRE_RUN_N_LENGTH_OFFSET,
              SieveCheckpoint::MAX_RUN_N_LENGTH + 1);
    refresh_checksum(bytes);
    write_bytes(path, bytes);
    check_throws([&] { (void)SieveCheckpoint::load(path); });

    make_checkpoint().save(path);
    bytes = read_bytes(path);
    write_u32(bytes, SieveCheckpoint::WIRE_OOC_PATH_LENGTH_OFFSET,
              SieveCheckpoint::MAX_PATH_LENGTH + 1);
    refresh_checksum(bytes);
    write_bytes(path, bytes);
    check_throws([&] { (void)SieveCheckpoint::load(path); });

    make_checkpoint().save(path);
    bytes = read_bytes(path);
    bytes[SieveCheckpoint::WIRE_STRINGS_OFFSET] = 0;
    refresh_checksum(bytes);
    write_bytes(path, bytes);
    check_throws([&] { (void)SieveCheckpoint::load(path); });

    make_checkpoint().save(path);
    bytes = read_bytes(path);
    bytes[SieveCheckpoint::WIRE_STRINGS_OFFSET + make_checkpoint().run_n.size()] = 0;
    refresh_checksum(bytes);
    write_bytes(path, bytes);
    check_throws([&] { (void)SieveCheckpoint::load(path); });
}

SieveCheckpoint::SaveStage failure_stage = SieveCheckpoint::SaveStage::Published;

void throwing_hook(SieveCheckpoint::SaveStage stage) {
    if (stage == failure_stage) {
        throw std::runtime_error("injected pre-publication failure");
    }
}

void test_prepublication_failure_preserves_previous_checkpoint() {
    const auto path = checkpoint_path("atomic_failure");
    CheckpointCleanup cleanup{path};

    const auto previous = make_checkpoint(1);
    const auto replacement = make_checkpoint(2);
    previous.save(path);

    for (const auto stage : {SieveCheckpoint::SaveStage::PayloadFlushed,
                             SieveCheckpoint::SaveStage::TemporaryFileCompleted}) {
        failure_stage = stage;
        check_throws([&] { replacement.save(path, throwing_hook); });
        check_equal(SieveCheckpoint::load(path), previous);
        CHECK(!SieveCheckpoint::exists(SieveCheckpoint::temporary_path(path)));
    }
}

std::vector<SieveCheckpoint::SaveStage> observed_stages;

void recording_hook(SieveCheckpoint::SaveStage stage) {
    observed_stages.push_back(stage);
}

void test_successful_publication_replaces_checkpoint() {
    const auto path = checkpoint_path("atomic_success");
    CheckpointCleanup cleanup{path};

    make_checkpoint(1).save(path);
    const auto replacement = make_checkpoint(2);
    observed_stages.clear();
    replacement.save(path, recording_hook);

    CHECK(observed_stages.size() == 3);
    CHECK(observed_stages[0] == SieveCheckpoint::SaveStage::PayloadFlushed);
    CHECK(observed_stages[1] == SieveCheckpoint::SaveStage::TemporaryFileCompleted);
    CHECK(observed_stages[2] == SieveCheckpoint::SaveStage::Published);
    check_equal(SieveCheckpoint::load(path), replacement);
    CHECK(!SieveCheckpoint::exists(SieveCheckpoint::temporary_path(path)));
}

SieveCheckpoint::SaveStage crash_stage = SieveCheckpoint::SaveStage::Published;

void crash_hook(SieveCheckpoint::SaveStage stage) {
    if (stage == crash_stage) {
        const int code = 80 + static_cast<int>(stage);
        std::_Exit(code);
    }
}

int run_crash_child(const std::string& stage_name, const std::string& path) {
    if (stage_name == "payload") {
        crash_stage = SieveCheckpoint::SaveStage::PayloadFlushed;
    } else if (stage_name == "complete") {
        crash_stage = SieveCheckpoint::SaveStage::TemporaryFileCompleted;
    } else if (stage_name == "published") {
        crash_stage = SieveCheckpoint::SaveStage::Published;
    } else {
        return 64;
    }

    make_checkpoint(2).save(path, crash_hook);
    return 65;
}

void check_child_exit(const gnfs::test::ChildProcessResult& result,
                      SieveCheckpoint::SaveStage stage) {
    CHECK(result.exited);
    CHECK(!result.signaled);
    CHECK(result.exit_code == 80 + static_cast<int>(stage));
}

void test_process_crash_stage_boundaries(const std::string& executable) {
    const auto path = checkpoint_path("process_crash");
    CheckpointCleanup cleanup{path};
    const auto previous = make_checkpoint(1);
    const auto replacement = make_checkpoint(2);

    previous.save(path);
    auto result = gnfs::test::run_child_process(executable, {"--crash-save", "payload", path});
    check_child_exit(result, SieveCheckpoint::SaveStage::PayloadFlushed);
    check_equal(SieveCheckpoint::load(path), previous);
    CHECK(SieveCheckpoint::exists(SieveCheckpoint::temporary_path(path)));
    CHECK(!SieveCheckpoint::exists_and_valid(SieveCheckpoint::temporary_path(path)));
    SieveCheckpoint::remove(SieveCheckpoint::temporary_path(path));

    result = gnfs::test::run_child_process(executable, {"--crash-save", "complete", path});
    check_child_exit(result, SieveCheckpoint::SaveStage::TemporaryFileCompleted);
    check_equal(SieveCheckpoint::load(path), previous);
    CHECK(SieveCheckpoint::exists_and_valid(SieveCheckpoint::temporary_path(path)));
    check_equal(SieveCheckpoint::load(SieveCheckpoint::temporary_path(path)), replacement);
    SieveCheckpoint::remove(SieveCheckpoint::temporary_path(path));

    result = gnfs::test::run_child_process(executable, {"--crash-save", "published", path});
    check_child_exit(result, SieveCheckpoint::SaveStage::Published);
    check_equal(SieveCheckpoint::load(path), replacement);
    CHECK(!SieveCheckpoint::exists(SieveCheckpoint::temporary_path(path)));
}

enum class PairedCrashPoint : int {
    PrefixBeforeCheckpoint = 100,
    CheckpointTemporary = 101,
    CheckpointPublished = 102,
    ResumedWithNewPrefix = 103,
    FinalizeMetadataDurable = 104,
    FinalizedBeforeRemoval = 105,
};

int paired_checkpoint_exit_code = 0;

void paired_checkpoint_crash_hook(SieveCheckpoint::SaveStage stage) {
    if (stage == SieveCheckpoint::SaveStage::PayloadFlushed) {
        std::_Exit(paired_checkpoint_exit_code);
    }
}

void paired_finalize_crash_hook(gnfs::relation::OOCRelationWriter::FinalizeStage stage) {
    if (stage == gnfs::relation::OOCRelationWriter::FinalizeStage::MetadataDurable) {
        std::_Exit(static_cast<int>(PairedCrashPoint::FinalizeMetadataDurable));
    }
}

int run_paired_crash_child(const std::string& point_name, const std::string& base_path) {
    using gnfs::relation::OOCRelationWriter;

    const std::string checkpoint_path = base_path + ".sieve_ckpt";
    OOCRelationWriter writer(base_path);
    CHECK(writer.write(make_ooc_relation(1)) == 0);
    const auto first = writer.checkpoint_prefix();
    make_paired_checkpoint(first, base_path).save(checkpoint_path);
    writer.resume_append(first);
    CHECK(writer.write(make_ooc_relation(3)) == 1);

    if (point_name == "finalize-metadata") {
        (void)writer.finalize(paired_finalize_crash_hook);
        return 65;
    }

    if (point_name == "finalized") {
        CHECK(writer.finalize().count == 2);
        std::_Exit(static_cast<int>(PairedCrashPoint::FinalizedBeforeRemoval));
    }

    const auto second = writer.checkpoint_prefix();
    CHECK(second.generation == first.generation + 1);
    if (point_name == "prefix") {
        std::_Exit(static_cast<int>(PairedCrashPoint::PrefixBeforeCheckpoint));
    }

    const auto second_checkpoint = make_paired_checkpoint(second, base_path);
    if (point_name == "temporary") {
        paired_checkpoint_exit_code = static_cast<int>(PairedCrashPoint::CheckpointTemporary);
        second_checkpoint.save(checkpoint_path, paired_checkpoint_crash_hook);
        return 65;
    }

    second_checkpoint.save(checkpoint_path);
    if (point_name == "published") {
        std::_Exit(static_cast<int>(PairedCrashPoint::CheckpointPublished));
    }

    if (point_name == "resumed-tail") {
        writer.resume_append(second);
        CHECK(writer.write(make_ooc_relation(5)) == 2);
        const auto unpaired_third = writer.checkpoint_prefix();
        CHECK(unpaired_third.generation == second.generation + 1);
        std::_Exit(static_cast<int>(PairedCrashPoint::ResumedWithNewPrefix));
    }

    return 64;
}

void check_paired_child_exit(const gnfs::test::ChildProcessResult& result, PairedCrashPoint point) {
    CHECK(result.exited);
    CHECK(!result.signaled);
    CHECK(result.exit_code == static_cast<int>(point));
}

void check_final_relation_sequence(const std::string& base_path,
                                   const std::vector<int64_t>& expected) {
    gnfs::relation::OOCRelationReader reader(base_path);
    CHECK(reader.count() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        CHECK(reader.read(i).a == expected[i]);
    }
}

void run_paired_recovery_case(const std::string& executable, const char* child_mode,
                              PairedCrashPoint crash_point, uint64_t expected_committed_count) {
    const std::string base_path = checkpoint_path(child_mode) + "_ooc";
    PairedArtifactsCleanup cleanup{base_path};
    SieveCheckpoint::remove(base_path + ".sieve_ckpt");
    std::remove((base_path + ".reldata").c_str());
    std::remove((base_path + ".relidx").c_str());

    const auto result =
        gnfs::test::run_child_process(executable, {"--crash-ooc", child_mode, base_path});
    check_paired_child_exit(result, crash_point);

    const auto checkpoint = SieveCheckpoint::load(base_path + ".sieve_ckpt");
    CHECK(checkpoint.ooc_relation_count == expected_committed_count);
    const auto descriptor = descriptor_from(checkpoint);

    gnfs::relation::OOCRelationWriter recovered(base_path, descriptor);
    CHECK(recovered.recovery_outcome() == gnfs::relation::OOCRecoveryOutcome::AppendablePrefix);
    CHECK(recovered.count() == expected_committed_count);
    CHECK(std::filesystem::file_size(base_path + ".relidx") ==
          gnfs::relation::OOCRelationWriter::index_size_for_count(expected_committed_count));
    CHECK(std::filesystem::file_size(base_path + ".reldata") == checkpoint.ooc_data_end);

    auto prefix = recovered.take_validated_resume_prefix();
    CHECK(prefix.has_value());
    CHECK(prefix->count == expected_committed_count);
    CHECK(prefix->seen.size() == expected_committed_count);

    CHECK(recovered.write(make_ooc_relation(9)) == expected_committed_count);
    CHECK(recovered.finalize().count == expected_committed_count + 1);
    if (expected_committed_count == 1) {
        check_final_relation_sequence(base_path, {1, 9});
    } else {
        check_final_relation_sequence(base_path, {1, 3, 9});
    }
}

void test_paired_ooc_process_crash_boundaries(const std::string& executable) {
    run_paired_recovery_case(executable, "prefix", PairedCrashPoint::PrefixBeforeCheckpoint, 1);
    run_paired_recovery_case(executable, "temporary", PairedCrashPoint::CheckpointTemporary, 1);
    run_paired_recovery_case(executable, "published", PairedCrashPoint::CheckpointPublished, 2);
    run_paired_recovery_case(executable, "resumed-tail", PairedCrashPoint::ResumedWithNewPrefix, 2);
    run_paired_recovery_case(executable, "finalize-metadata",
                             PairedCrashPoint::FinalizeMetadataDurable, 1);

    const std::string base_path = checkpoint_path("finalized") + "_ooc";
    PairedArtifactsCleanup cleanup{base_path};
    const auto result =
        gnfs::test::run_child_process(executable, {"--crash-ooc", "finalized", base_path});
    check_paired_child_exit(result, PairedCrashPoint::FinalizedBeforeRemoval);

    const auto checkpoint = SieveCheckpoint::load(base_path + ".sieve_ckpt");
    CHECK(checkpoint.ooc_relation_count == 1);
    gnfs::relation::OOCRelationWriter recovered(base_path, descriptor_from(checkpoint));
    CHECK(recovered.recovery_outcome() == gnfs::relation::OOCRecoveryOutcome::FinalizedCorpus);
    CHECK(recovered.state() == gnfs::relation::OOCWriterState::Finalized);
    CHECK(recovered.count() == 2);
    check_final_relation_sequence(base_path, {1, 3});
    check_throws([&] { (void)recovered.write(make_ooc_relation(9)); });
}

void test_remove_cleans_official_and_temporary() {
    const auto path = checkpoint_path("remove");
    CheckpointCleanup cleanup{path};
    make_checkpoint().save(path);

    {
        std::ofstream temporary(SieveCheckpoint::temporary_path(path));
        CHECK(temporary.good());
        temporary << "left by crash";
    }
    CHECK(SieveCheckpoint::exists(path));
    CHECK(SieveCheckpoint::exists(SieveCheckpoint::temporary_path(path)));

    SieveCheckpoint::remove(path);
    CHECK(!SieveCheckpoint::exists(path));
    CHECK(!SieveCheckpoint::exists(SieveCheckpoint::temporary_path(path)));
}

void test_nonexistent_load() {
    const auto path = checkpoint_path("nonexistent");
    SieveCheckpoint::remove(path);
    CHECK(!SieveCheckpoint::exists(path));
    CHECK(!SieveCheckpoint::exists_and_valid(path));
    check_throws([&] { (void)SieveCheckpoint::load(path); });
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--crash-save") {
        return run_crash_child(argv[2], argv[3]);
    }
    if (argc == 4 && std::string(argv[1]) == "--crash-ooc") {
        return run_paired_crash_child(argv[2], argv[3]);
    }

    try {
        const std::string executable =
            std::filesystem::absolute(std::filesystem::path(argv[0])).string();
        const std::vector<std::pair<const char*, std::function<void()>>> tests = {
            {"V2 round-trip", test_v2_round_trip},
            {"run identity stability", test_run_identity_is_stable_and_matches_checkpoint},
            {"run identity mutations", test_run_identity_mutations_change_fingerprint},
            {"empty committed prefix", test_empty_committed_prefix_round_trip},
            {"long portable path", test_long_portable_path_round_trip},
            {"invalid states", test_invalid_state_rejected_before_write},
            {"incomplete magic", test_incomplete_magic_rejected},
            {"V1 rejection", test_v1_rejected_even_with_valid_checksum},
            {"legacy OOC V2 rejection", test_legacy_v2_ooc_rejected_even_with_valid_checksum},
            {"checksum corruption", test_checksum_corruption_rejected},
            {"truncation", test_truncation_rejected},
            {"exact trailing bytes", test_checksummed_trailing_byte_rejected},
            {"malformed fields", test_checksummed_malformed_fields_rejected},
            {"atomic pre-publication failure",
             test_prepublication_failure_preserves_previous_checkpoint},
            {"atomic publication", test_successful_publication_replaces_checkpoint},
            {"process crash stages", [&] { test_process_crash_stage_boundaries(executable); }},
            {"paired OOC process crash stages",
             [&] { test_paired_ooc_process_crash_boundaries(executable); }},
            {"remove", test_remove_cleans_official_and_temporary},
            {"nonexistent", test_nonexistent_load},
        };

        std::cout << "===== SieveCheckpoint V2 Tests =====\n";
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "  " << name << ": PASS\n";
        }
        std::cout << "===== All SieveCheckpoint V2 tests PASSED =====\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SieveCheckpoint test failure: " << error.what() << '\n';
        return 1;
    }
}
