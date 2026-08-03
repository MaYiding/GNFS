#include "gnfs/linalg/relation_source.hpp"
#include "gnfs/relation/relation_sink.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using gnfs::core::Relation;
using gnfs::linalg::OOCRelationSource;
using gnfs::linalg::RelationSelectionSource;
using gnfs::linalg::VectorRelationSource;
using gnfs::relation::OOCCleanupPolicy;
using gnfs::relation::OOCCleanupTransaction;
using gnfs::relation::OOCRelationReader;
using gnfs::relation::OOCRelationWriter;
using gnfs::relation::OOCWriterState;
using gnfs::relation::RelationCorpus;
using gnfs::relation::RelationSink;
using gnfs::relation::RelationSinkState;
using gnfs::relation::RelationStorageKind;

static_assert(!std::is_copy_constructible_v<RelationSink>);
static_assert(!std::is_copy_assignable_v<RelationSink>);
static_assert(std::is_move_constructible_v<RelationSink>);
static_assert(std::is_move_assignable_v<RelationSink>);
static_assert(gnfs::relation::RelationSource<RelationCorpus>);
static_assert(gnfs::relation::RelationSource<VectorRelationSource>);
static_assert(gnfs::relation::RelationSource<OOCRelationSource>);
static_assert(gnfs::relation::RelationSource<RelationSelectionSource>);
static_assert(gnfs::linalg::RelationSource<RelationCorpus>);
static_assert(gnfs::linalg::RelationSource<VectorRelationSource>);
static_assert(gnfs::linalg::RelationSource<OOCRelationSource>);
static_assert(gnfs::linalg::RelationSource<RelationSelectionSource>);

size_t checks = 0;

[[noreturn]] void fail(const char* expression, int line) {
    throw std::runtime_error(std::string("CHECK failed at line ") + std::to_string(line) + ": " +
                             expression);
}

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(expression)) {                                                                       \
            fail(#expression, __LINE__);                                                           \
        }                                                                                          \
    } while (false)

template <typename Callable> void expect_failure(Callable&& callable) {
    bool caught = false;
    try {
        std::forward<Callable>(callable)();
    } catch (const std::exception&) {
        caught = true;
    }
    CHECK(caught);
}

[[nodiscard]] Relation make_relation(int64_t a, uint64_t b, uint32_t seed) {
    Relation relation(a, b);
    relation.rational_factors = {seed, static_cast<uint32_t>(seed + 2)};
    relation.algebraic_factors = {static_cast<uint32_t>(seed + 1), static_cast<uint32_t>(seed + 3)};
    relation.rational_large_prime = {
        {static_cast<uint64_t>(1009 + seed), 0, static_cast<uint8_t>(1 + seed % 2)}};
    relation.algebraic_large_prime = {
        {static_cast<uint64_t>(2003 + seed), static_cast<uint64_t>(17 + seed), 1}};
    relation.extra_ab_pairs = {{a - 10, b + 1}, {a + 20, b + 2}};
    return relation;
}

[[nodiscard]] std::vector<Relation> make_relations(size_t count) {
    std::vector<Relation> relations;
    relations.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        relations.push_back(make_relation(static_cast<int64_t>(31 + i),
                                          static_cast<uint64_t>(7 + i),
                                          static_cast<uint32_t>(10 + i)));
    }
    return relations;
}

[[nodiscard]] bool relation_equal(const Relation& lhs, const Relation& rhs) {
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.rational_factors == rhs.rational_factors &&
           lhs.algebraic_factors == rhs.algebraic_factors &&
           lhs.rational_large_prime == rhs.rational_large_prime &&
           lhs.algebraic_large_prime == rhs.algebraic_large_prime &&
           lhs.extra_ab_pairs == rhs.extra_ab_pairs;
}

void check_corpus(const RelationCorpus& corpus, uint64_t generation,
                  const std::vector<Relation>& expected) {
    CHECK(corpus.logical_generation() == generation);
    CHECK(corpus.count() == expected.size());
    CHECK(corpus.empty() == expected.empty());
    for (size_t ordinal = 0; ordinal < expected.size(); ++ordinal) {
        CHECK(relation_equal(corpus.read(ordinal), expected[ordinal]));
    }
}

[[nodiscard]] std::string unique_base(std::string_view label) {
    static std::atomic<uint64_t> sequence{0};
    const uint64_t suffix = sequence.fetch_add(1, std::memory_order_relaxed);
    return gnfs::util::temp_path("gnfs_relation_sink_" + std::string(label) + "_" +
                                 std::to_string(gnfs::util::process_id()) + "_" +
                                 std::to_string(suffix));
}

struct ArtifactCleanup final {
    explicit ArtifactCleanup(std::string artifact_base) : base(std::move(artifact_base)) {}

    ~ArtifactCleanup() {
        std::error_code ignored;
        std::filesystem::remove(base + ".relidx", ignored);
        ignored.clear();
        std::filesystem::remove(base + ".reldata", ignored);
        ignored.clear();
        std::filesystem::remove(base + ".gnfs-ooc-cleanup-v1.lock", ignored);
        ignored.clear();
        std::filesystem::remove_all(base + ".gnfs-sink-lease", ignored);
        ignored.clear();
        try {
            const auto sink_paths = OOCCleanupTransaction::paths_for(
                std::filesystem::path(base + ".gnfs-sink-lease") / "corpus");
            std::filesystem::remove(sink_paths.lock_path, ignored);
        } catch (...) {
        }
    }

    std::string base;
};

[[nodiscard]] bool artifacts_exist(const std::string& base) {
    return std::filesystem::exists(base + ".relidx") && std::filesystem::exists(base + ".reldata");
}

[[nodiscard]] bool artifacts_absent(const std::string& base) {
    return !std::filesystem::exists(base + ".relidx") &&
           !std::filesystem::exists(base + ".reldata");
}

[[nodiscard]] std::string sink_store_base(const std::string& base) {
    return (std::filesystem::path(base + ".gnfs-sink-lease") / "corpus").string();
}

[[nodiscard]] bool sink_artifacts_exist(const std::string& base) {
    return artifacts_exist(sink_store_base(base));
}

[[nodiscard]] bool sink_artifacts_absent(const std::string& base) {
    return artifacts_absent(sink_store_base(base));
}

[[nodiscard]] bool lease_absent(const std::string& base) {
    return !std::filesystem::exists(base + ".gnfs-sink-lease");
}

[[nodiscard]] bool lease_exists(const std::string& base) {
    return std::filesystem::is_directory(base + ".gnfs-sink-lease");
}

[[nodiscard]] bool sink_lock_exists(const std::string& base) {
    return std::filesystem::is_regular_file(
        OOCCleanupTransaction::paths_for(sink_store_base(base)).lock_path);
}

void write_file(const std::string& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create relation-sink test artifact: " + path);
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("cannot write relation-sink test artifact: " + path);
    }
}

[[nodiscard]] std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read relation-sink test artifact: " + path);
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void test_in_memory_empty_and_ordered_nonempty() {
    {
        auto sink = RelationSink::in_memory(101);
        CHECK(sink.logical_generation() == 101);
        CHECK(sink.count() == 0);
        CHECK(sink.state() == RelationSinkState::Open);

        auto corpus = sink.finalize();
        CHECK(sink.state() == RelationSinkState::Finalized);
        CHECK(corpus.storage_kind() == RelationStorageKind::InMemory);
        check_corpus(corpus, 101, {});
    }

    const auto expected = make_relations(4);
    std::optional<RelationCorpus> result;
    {
        auto sink = RelationSink::in_memory(102);
        for (size_t ordinal = 0; ordinal < expected.size(); ++ordinal) {
            CHECK(sink.append(Relation(expected[ordinal])) == ordinal);
        }
        CHECK(sink.count() == expected.size());
        result.emplace(sink.finalize());
        CHECK(sink.state() == RelationSinkState::Finalized);
    }

    CHECK(result.has_value());
    CHECK(result->storage_kind() == RelationStorageKind::InMemory);
    check_corpus(*result, 102, expected);
}

void test_out_of_core_empty_and_descriptor_backed_lifetime() {
    {
        ArtifactCleanup artifacts(unique_base("empty_ooc"));
        auto sink = RelationSink::out_of_core(201, artifacts.base, OOCCleanupPolicy::Preserve);
        {
            auto corpus = sink.finalize();

            CHECK(sink.state() == RelationSinkState::Finalized);
            CHECK(corpus.storage_kind() == RelationStorageKind::FinalizedOOC);
            CHECK(sink_artifacts_exist(artifacts.base));
            CHECK(lease_exists(artifacts.base));
            CHECK(sink_lock_exists(artifacts.base));
            const auto scope = corpus.ooc_artifact_scope();
            CHECK(scope.has_value());
            CHECK(scope->cleanup_directory ==
                  RelationSink::lease_root_for(artifacts.base).string());
            check_corpus(corpus, 201, {});
        }
        CHECK(sink_artifacts_exist(artifacts.base));
        CHECK(lease_exists(artifacts.base));
        CHECK(sink_lock_exists(artifacts.base));
    }

    ArtifactCleanup artifacts(unique_base("nonempty_ooc"));
    const auto expected = make_relations(5);
    std::optional<RelationCorpus> result;
    {
        auto original =
            RelationSink::out_of_core(202, artifacts.base, OOCCleanupPolicy::RemoveArtifacts);
        CHECK(original.append(Relation(expected.front())) == 0);
        auto sink = std::move(original);
        CHECK(sink.logical_generation() == 202);
        CHECK(sink.count() == 1);
        CHECK(sink.state() == RelationSinkState::Open);
        for (size_t ordinal = 1; ordinal < expected.size(); ++ordinal) {
            CHECK(sink.append(Relation(expected[ordinal])) == ordinal);
        }
        result.emplace(sink.finalize());
        CHECK(sink.state() == RelationSinkState::Finalized);
    }

    CHECK(result.has_value());
    CHECK(result->storage_kind() == RelationStorageKind::FinalizedOOC);
    CHECK(sink_artifacts_exist(artifacts.base));
    CHECK(lease_exists(artifacts.base));
    check_corpus(*result, 202, expected);

    result.reset();
    CHECK(sink_artifacts_absent(artifacts.base));
    CHECK(lease_absent(artifacts.base));
    CHECK(sink_lock_exists(artifacts.base));
}

void test_preserve_can_arm_private_lease_and_reuse_one_lock_domain() {
    ArtifactCleanup artifacts(unique_base("preserve_arm_reuse"));
    const auto paths = OOCCleanupTransaction::paths_for(sink_store_base(artifacts.base));

    {
        auto sink = RelationSink::out_of_core(203, artifacts.base, OOCCleanupPolicy::Preserve);
        CHECK(sink.append(make_relation(70, 3, 70)) == 0);
        auto corpus = sink.finalize();
        CHECK(corpus.arm_ooc_cleanup());
        CHECK(sink_artifacts_exist(artifacts.base));
        CHECK(lease_exists(artifacts.base));
        CHECK(sink_lock_exists(artifacts.base));
    }
    CHECK(sink_artifacts_absent(artifacts.base));
    CHECK(lease_absent(artifacts.base));
    CHECK(sink_lock_exists(artifacts.base));

    {
        auto sink =
            RelationSink::out_of_core(204, artifacts.base, OOCCleanupPolicy::RemoveArtifacts);
        CHECK(sink.append(make_relation(71, 3, 71)) == 0);
        auto corpus = sink.finalize();
        CHECK(sink_artifacts_exist(artifacts.base));
        CHECK(lease_exists(artifacts.base));
        CHECK(paths.lock_path ==
              OOCCleanupTransaction::paths_for(sink_store_base(artifacts.base)).lock_path);
    }
    CHECK(sink_artifacts_absent(artifacts.base));
    CHECK(lease_absent(artifacts.base));
    CHECK(sink_lock_exists(artifacts.base));
}

void test_abort_never_finalizes_and_cleans_ooc_artifacts() {
    {
        auto sink = RelationSink::in_memory(301);
        CHECK(sink.append(make_relation(1, 1, 1)) == 0);
        sink.abort();
        CHECK(sink.state() == RelationSinkState::Aborted);
        expect_failure([&] { (void)sink.append(make_relation(2, 1, 2)); });
        expect_failure([&] { (void)sink.finalize(); });
    }

    ArtifactCleanup artifacts(unique_base("abort_ooc"));
    {
        auto sink = RelationSink::out_of_core(302, artifacts.base, OOCCleanupPolicy::Preserve);
        CHECK(sink.append(make_relation(3, 1, 3)) == 0);
        CHECK(sink.append(make_relation(4, 1, 4)) == 1);
        sink.abort();
        CHECK(sink.state() == RelationSinkState::Aborted);
        CHECK(sink_artifacts_absent(artifacts.base));
        CHECK(lease_absent(artifacts.base));
        expect_failure([&] { (void)sink.finalize(); });
    }
    CHECK(sink_artifacts_absent(artifacts.base));
}

void test_open_sink_destructor_aborts_without_publishing() {
    ArtifactCleanup artifacts(unique_base("destructor_abort"));
    {
        auto sink =
            RelationSink::out_of_core(303, artifacts.base, OOCCleanupPolicy::RemoveArtifacts);
        (void)sink.append(make_relation(5, 1, 5));
        CHECK(sink_artifacts_exist(artifacts.base));
    }
    CHECK(sink_artifacts_absent(artifacts.base));
    CHECK(lease_absent(artifacts.base));
}

void throw_after_final_magic_is_durable(OOCRelationWriter::FinalizeStage stage) {
    if (stage == OOCRelationWriter::FinalizeStage::FinalMagicDurable) {
        throw std::runtime_error("injected post-commit observer failure");
    }
}

void test_post_commit_hook_failure_keeps_writer_finalized() {
    ArtifactCleanup artifacts(unique_base("post_commit_hook"));
    OOCRelationWriter writer(artifacts.base);
    const Relation expected = make_relation(6, 1, 6);
    (void)writer.write(expected);

    expect_failure([&] { (void)writer.finalize(throw_after_final_magic_is_durable); });
    CHECK(writer.state() == OOCWriterState::Finalized);

    const auto descriptor = writer.finalize();
    CHECK(descriptor.count == 1);
    OOCRelationReader reader(artifacts.base, descriptor);
    CHECK(reader.count() == 1);
    CHECK(relation_equal(reader.read(0), expected));
}

void test_writer_abort_is_idempotent_and_never_finalizes() {
    ArtifactCleanup artifacts(unique_base("writer_abort"));
    {
        OOCRelationWriter writer(artifacts.base);
        (void)writer.write(make_relation(6, 1, 6));
        writer.abort();
        writer.abort();
        CHECK(writer.state() == OOCWriterState::Failed);
    }
    CHECK(artifacts_exist(artifacts.base));
    expect_failure([&] {
        OOCRelationReader reader(artifacts.base);
        (void)reader;
    });
}

void test_existing_ooc_artifacts_are_rejected_without_mutation() {
    ArtifactCleanup artifacts(unique_base("existing_ooc"));
    const std::string index_bytes("existing-index\0payload", 22);
    const std::string data_bytes("existing-data\0payload", 21);
    write_file(artifacts.base + ".relidx", index_bytes);
    write_file(artifacts.base + ".reldata", data_bytes);

    expect_failure([&] {
        auto sink =
            RelationSink::out_of_core(401, artifacts.base, OOCCleanupPolicy::RemoveArtifacts);
        (void)sink;
    });

    CHECK(read_file(artifacts.base + ".relidx") == index_bytes);
    CHECK(read_file(artifacts.base + ".reldata") == data_bytes);
    CHECK(lease_absent(artifacts.base));
}

void test_partial_artifact_and_lease_collisions_are_rejected() {
    for (const std::string& suffix : {std::string(".relidx"), std::string(".reldata")}) {
        ArtifactCleanup artifacts(unique_base("partial_existing"));
        const std::string bytes("existing-partial\0payload", 24);
        write_file(artifacts.base + suffix, bytes);

        expect_failure([&] {
            auto sink =
                RelationSink::out_of_core(403, artifacts.base, OOCCleanupPolicy::RemoveArtifacts);
            (void)sink;
        });

        CHECK(read_file(artifacts.base + suffix) == bytes);
        CHECK(lease_absent(artifacts.base));
    }

    ArtifactCleanup leased(unique_base("existing_lease"));
    std::error_code ec;
    CHECK(std::filesystem::create_directory(leased.base + ".gnfs-sink-lease", ec));
    CHECK(!ec);
    expect_failure([&] {
        auto sink = RelationSink::out_of_core(404, leased.base, OOCCleanupPolicy::RemoveArtifacts);
        (void)sink;
    });
    CHECK(lease_exists(leased.base));
    CHECK(sink_artifacts_absent(leased.base));
    CHECK(!sink_lock_exists(leased.base));
}

void test_dangling_symlink_is_rejected_without_following_it() {
    ArtifactCleanup artifacts(unique_base("dangling_symlink"));
    const std::filesystem::path link_path(artifacts.base + ".relidx");
    const std::filesystem::path missing_target(artifacts.base + ".missing-target");
    std::error_code ec;
    std::filesystem::create_symlink(missing_target, link_path, ec);
    if (ec) {
        std::cout << "Skipping dangling-symlink check: " << ec.message() << '\n';
        return;
    }

    expect_failure([&] {
        auto sink =
            RelationSink::out_of_core(402, artifacts.base, OOCCleanupPolicy::RemoveArtifacts);
        (void)sink;
    });
    CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(link_path)));
    CHECK(lease_absent(artifacts.base));
}

void test_append_validation_failure_aborts_transaction() {
    Relation oversized = make_relation(8, 1, 8);
    oversized.rational_large_prime.resize(
        static_cast<size_t>(Relation::MAX_SERIALIZED_LARGE_PRIMES) + 1);

    auto memory_sink = RelationSink::in_memory(405);
    expect_failure([&] { (void)memory_sink.append(oversized); });
    CHECK(memory_sink.state() == RelationSinkState::Aborted);
    CHECK(memory_sink.count() == 0);
    expect_failure([&] { (void)memory_sink.finalize(); });

    ArtifactCleanup artifacts(unique_base("append_validation"));
    auto ooc_sink =
        RelationSink::out_of_core(406, artifacts.base, OOCCleanupPolicy::RemoveArtifacts);
    CHECK(sink_artifacts_exist(artifacts.base));
    expect_failure([&] { (void)ooc_sink.append(oversized); });
    CHECK(ooc_sink.state() == RelationSinkState::Aborted);
    CHECK(ooc_sink.count() == 0);
    CHECK(sink_artifacts_absent(artifacts.base));
    CHECK(lease_absent(artifacts.base));
}

void test_move_repeated_finalize_and_append_after_finalize() {
    const auto expected = make_relations(2);
    auto original = RelationSink::in_memory(501);
    CHECK(original.append(Relation(expected[0])) == 0);

    auto moved = std::move(original);
    CHECK(moved.logical_generation() == 501);
    CHECK(moved.count() == 1);
    CHECK(moved.state() == RelationSinkState::Open);
    CHECK(moved.append(Relation(expected[1])) == 1);

    auto corpus = moved.finalize();
    CHECK(moved.state() == RelationSinkState::Finalized);
    check_corpus(corpus, 501, expected);
    expect_failure([&] { (void)moved.finalize(); });
    expect_failure([&] { (void)moved.append(make_relation(99, 1, 99)); });

    auto assigned = RelationSink::in_memory(502);
    auto source = RelationSink::in_memory(503);
    CHECK(source.append(make_relation(7, 1, 7)) == 0);
    assigned = std::move(source);
    CHECK(assigned.logical_generation() == 503);
    CHECK(assigned.count() == 1);
    CHECK(assigned.state() == RelationSinkState::Open);
    auto assigned_corpus = assigned.finalize();
    CHECK(assigned_corpus.logical_generation() == 503);
    CHECK(assigned_corpus.count() == 1);
}

void test_ooc_move_assignment_rolls_back_old_target() {
    ArtifactCleanup old_artifacts(unique_base("move_assign_old"));
    ArtifactCleanup new_artifacts(unique_base("move_assign_new"));
    auto target =
        RelationSink::out_of_core(601, old_artifacts.base, OOCCleanupPolicy::RemoveArtifacts);
    auto source =
        RelationSink::out_of_core(602, new_artifacts.base, OOCCleanupPolicy::RemoveArtifacts);
    (void)target.append(make_relation(9, 1, 9));
    const Relation expected = make_relation(10, 1, 10);
    (void)source.append(expected);
    CHECK(sink_artifacts_exist(old_artifacts.base));
    CHECK(sink_artifacts_exist(new_artifacts.base));

    target = std::move(source);
    CHECK(target.logical_generation() == 602);
    CHECK(target.count() == 1);
    CHECK(target.state() == RelationSinkState::Open);
    CHECK(sink_artifacts_absent(old_artifacts.base));
    CHECK(lease_absent(old_artifacts.base));
    CHECK(sink_artifacts_exist(new_artifacts.base));

    {
        auto corpus = target.finalize();
        CHECK(corpus.storage_kind() == RelationStorageKind::FinalizedOOC);
        CHECK(relation_equal(corpus.read(0), expected));
    }
    CHECK(sink_artifacts_absent(new_artifacts.base));
    CHECK(lease_absent(new_artifacts.base));
}

} // namespace

int main() {
    try {
        test_in_memory_empty_and_ordered_nonempty();
        test_out_of_core_empty_and_descriptor_backed_lifetime();
        test_preserve_can_arm_private_lease_and_reuse_one_lock_domain();
        test_abort_never_finalizes_and_cleans_ooc_artifacts();
        test_open_sink_destructor_aborts_without_publishing();
        test_post_commit_hook_failure_keeps_writer_finalized();
        test_writer_abort_is_idempotent_and_never_finalizes();
        test_existing_ooc_artifacts_are_rejected_without_mutation();
        test_partial_artifact_and_lease_collisions_are_rejected();
        test_dangling_symlink_is_rejected_without_following_it();
        test_append_validation_failure_aborts_transaction();
        test_move_repeated_finalize_and_append_after_finalize();
        test_ooc_move_assignment_rolls_back_old_target();
    } catch (const std::exception& error) {
        std::cerr << "relation sink test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "All " << checks << " relation sink checks passed\n";
    return 0;
}
