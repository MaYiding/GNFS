#include <gnfs/linalg/relation_source.hpp>
#include <gnfs/relation/collector.hpp>
#include <gnfs/relation/relation_corpus.hpp>
#include <gnfs/util/process.hpp>
#include <gnfs/util/temp_path.hpp>

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
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
using gnfs::relation::CollectorConfig;
using gnfs::relation::materialize_selected;
using gnfs::relation::OOCCleanupPolicy;
using gnfs::relation::OOCCleanupTransaction;
using gnfs::relation::OOCRelationReader;
using gnfs::relation::OOCRelationWriter;
using gnfs::relation::OOCSnapshotDescriptor;
using gnfs::relation::ReadOnlyRelationCorpusView;
using gnfs::relation::RelationCollector;
using gnfs::relation::RelationCorpus;
using gnfs::relation::RelationSelection;
using gnfs::relation::RelationStorageKind;
using gnfs::util::OwnedNativeFile;

static_assert(!std::is_copy_constructible_v<RelationCorpus>);
static_assert(!std::is_copy_assignable_v<RelationCorpus>);
static_assert(std::is_nothrow_move_constructible_v<RelationCorpus>);
static_assert(std::is_nothrow_move_assignable_v<RelationCorpus>);
static_assert(gnfs::linalg::RelationSource<RelationCorpus>);

struct ReadOnlyViewVisitorProbe final {
    void operator()(const Relation&, size_t) const {}
};

template <typename View>
concept HasReadOnlyViewSurface =
    requires(const View& view, size_t ordinal, ReadOnlyViewVisitorProbe visitor) {
        { view.count() } -> std::same_as<size_t>;
        { view.read(ordinal) } -> std::same_as<Relation>;
        view.for_each(visitor);
    };

template <typename View>
concept ExposesCleanupArm = requires(View& view) { view.arm_ooc_cleanup(); };

template <typename View>
concept ExposesArtifactScope = requires(const View& view) { view.ooc_artifact_scope(); };

template <typename View>
concept ExposesBasePathMethod = requires(const View& view) { view.base_path(); };

template <typename View>
concept ExposesBasePathField = requires(const View& view) { view.base_path; };

template <typename View>
concept ExposesNativeHandle = requires(const View& view) { view.native_handle(); };

template <typename View>
concept ExposesIndexHandle = requires(const View& view) { view.index_handle(); };

template <typename View>
concept ExposesDataHandle = requires(const View& view) { view.data_handle(); };

template <typename View>
concept ExposesHasCleanupReceipt = requires(View& view) { view.has_cleanup_ownership_receipt(); };

template <typename View>
concept ExposesTakeCleanupReceipt = requires(View& view) { view.take_cleanup_ownership_receipt(); };

static_assert(HasReadOnlyViewSurface<ReadOnlyRelationCorpusView>);
static_assert(gnfs::linalg::RelationSource<ReadOnlyRelationCorpusView>);
static_assert(std::same_as<gnfs::linalg::OOCRelationSource, ReadOnlyRelationCorpusView>);
static_assert(!std::is_default_constructible_v<ReadOnlyRelationCorpusView>);
static_assert(std::is_constructible_v<ReadOnlyRelationCorpusView, OOCRelationReader&>);
static_assert(std::is_constructible_v<ReadOnlyRelationCorpusView, const OOCRelationReader&>);
static_assert(!std::is_nothrow_constructible_v<ReadOnlyRelationCorpusView, OOCRelationReader&>);
static_assert(!std::is_constructible_v<ReadOnlyRelationCorpusView, OOCRelationReader&&>);
static_assert(!std::is_constructible_v<ReadOnlyRelationCorpusView, const OOCRelationReader&&>);
static_assert(!std::is_constructible_v<ReadOnlyRelationCorpusView, const std::string&>);
static_assert(!std::is_constructible_v<ReadOnlyRelationCorpusView, const std::filesystem::path&>);
static_assert(!std::is_constructible_v<ReadOnlyRelationCorpusView, const OOCSnapshotDescriptor&>);
static_assert(!std::is_constructible_v<ReadOnlyRelationCorpusView, const std::string&,
                                       const OOCSnapshotDescriptor&>);
static_assert(!std::is_constructible_v<ReadOnlyRelationCorpusView, OwnedNativeFile&&,
                                       OwnedNativeFile&&, const OOCSnapshotDescriptor&>);
static_assert(std::is_copy_constructible_v<ReadOnlyRelationCorpusView>);
static_assert(!ExposesCleanupArm<ReadOnlyRelationCorpusView>);
static_assert(!ExposesArtifactScope<ReadOnlyRelationCorpusView>);
static_assert(!ExposesBasePathMethod<ReadOnlyRelationCorpusView>);
static_assert(!ExposesBasePathField<ReadOnlyRelationCorpusView>);
static_assert(!ExposesNativeHandle<ReadOnlyRelationCorpusView>);
static_assert(!ExposesIndexHandle<ReadOnlyRelationCorpusView>);
static_assert(!ExposesDataHandle<ReadOnlyRelationCorpusView>);
static_assert(!ExposesHasCleanupReceipt<ReadOnlyRelationCorpusView>);
static_assert(!ExposesTakeCleanupReceipt<ReadOnlyRelationCorpusView>);

[[noreturn]] void fail(const char* expression, int line) {
    throw std::runtime_error(std::string("CHECK failed at line ") + std::to_string(line) + ": " +
                             expression);
}

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fail(#expression, __LINE__);                                                           \
        }                                                                                          \
    } while (false)

template <typename Exception, typename Callable> void expect_throws(Callable&& callable) {
    bool caught = false;
    try {
        std::forward<Callable>(callable)();
    } catch (const Exception&) {
        caught = true;
    }
    CHECK(caught);
}

Relation make_relation(int64_t a, uint64_t b, uint32_t seed) {
    Relation relation(a, b);
    relation.rational_factors = {seed, static_cast<uint32_t>(seed + 2)};
    relation.algebraic_factors = {static_cast<uint32_t>(seed + 1), static_cast<uint32_t>(seed + 3)};
    relation.rational_large_prime = {
        {static_cast<uint64_t>(1009 + seed), 0, static_cast<uint8_t>(1 + seed % 2)}};
    relation.algebraic_large_prime = {
        {static_cast<uint64_t>(2003 + seed), static_cast<uint64_t>(17 + seed), 1}};
    relation.extra_ab_pairs = {
        {static_cast<int64_t>(a - 10), b + 1},
        {static_cast<int64_t>(a + 20), b + 2},
    };
    return relation;
}

std::vector<Relation> make_relations(size_t count) {
    std::vector<Relation> relations;
    relations.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        relations.push_back(make_relation(static_cast<int64_t>(31 + i),
                                          static_cast<uint64_t>(7 + i),
                                          static_cast<uint32_t>(10 + i)));
    }
    return relations;
}

bool relations_equal(const Relation& lhs, const Relation& rhs) {
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.rational_factors == rhs.rational_factors &&
           lhs.algebraic_factors == rhs.algebraic_factors &&
           lhs.rational_large_prime == rhs.rational_large_prime &&
           lhs.algebraic_large_prime == rhs.algebraic_large_prime &&
           lhs.extra_ab_pairs == rhs.extra_ab_pairs;
}

std::string unique_base(std::string_view label) {
    static std::atomic<uint64_t> sequence{0};
    const uint64_t suffix = sequence.fetch_add(1, std::memory_order_relaxed);
    return gnfs::util::temp_path("gnfs_relation_corpus_" + std::string(label) + "_" +
                                 std::to_string(gnfs::util::process_id()) + "_" +
                                 std::to_string(suffix));
}

struct TestArtifactCleanup final {
    explicit TestArtifactCleanup(std::string base_path) : base(std::move(base_path)) {}

    ~TestArtifactCleanup() {
        std::error_code ignored;
        try {
            const auto paths = OOCCleanupTransaction::paths_for(base);
            for (const auto* path :
                 {&paths.index_path, &paths.data_path, &paths.intent_path,
                  &paths.intent_pending_path, &paths.staged_path, &paths.staged_pending_path,
                  &paths.quarantine_index_path, &paths.quarantine_data_path, &paths.lock_path}) {
                ignored.clear();
                std::filesystem::remove(*path, ignored);
            }
        } catch (...) {
            std::filesystem::remove(base + ".relidx", ignored);
            ignored.clear();
            std::filesystem::remove(base + ".reldata", ignored);
        }
    }

    std::string base;
};

struct TestPathCleanup final {
    explicit TestPathCleanup(std::filesystem::path entry_path) : path(std::move(entry_path)) {}

    ~TestPathCleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

bool artifacts_exist(const std::string& base_path) {
    return std::filesystem::exists(base_path + ".relidx") &&
           std::filesystem::exists(base_path + ".reldata");
}

OOCSnapshotDescriptor write_finalized_store(const std::string& base_path,
                                            const std::vector<Relation>& relations) {
    OOCRelationWriter writer(base_path);
    for (const auto& relation : relations) {
        (void)writer.write(relation);
    }
    const auto descriptor = writer.finalize();
    CHECK(descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
    CHECK(descriptor.data_end >= OOCRelationWriter::DATA_HEADER_BYTES);
    return descriptor;
}

[[nodiscard]] std::unique_ptr<OOCRelationWriter>
make_finalized_writer(const std::string& base_path, const std::vector<Relation>& relations) {
    auto writer = std::make_unique<OOCRelationWriter>(base_path);
    for (const auto& relation : relations) {
        (void)writer->write(relation);
    }
    const auto descriptor = writer->finalize();
    CHECK(descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
    CHECK(descriptor.count == relations.size());
    return writer;
}

RelationCorpus collect_owned_corpus(uint64_t logical_generation, const std::string& base_path,
                                    const std::vector<Relation>& relations) {
    CollectorConfig config;
    config.ooc_enabled = true;
    config.ooc_base_path = base_path;
    config.use_pool = false;

    RelationCollector collector(config);
    for (const auto& relation : relations) {
        CHECK(collector.add(Relation(relation)));
    }
    return collector.handoff_ooc_corpus(logical_generation, OOCCleanupPolicy::RemoveArtifacts);
}

void test_read_only_ooc_view_nonempty_and_empty() {
    TestArtifactCleanup populated_artifacts(unique_base("readonly_view_populated"));
    const auto expected = make_relations(3);
    const auto populated_descriptor = write_finalized_store(populated_artifacts.base, expected);
    OOCRelationReader populated_reader(populated_artifacts.base, populated_descriptor);
    ReadOnlyRelationCorpusView populated_view(populated_reader);

    CHECK(populated_view.count() == expected.size());
    CHECK(relations_equal(populated_view.read(0), expected[0]));
    CHECK(relations_equal(populated_view.read(2), expected[2]));
    expect_throws<std::out_of_range>([&] { (void)populated_view.read(populated_view.count()); });

    std::vector<size_t> visited_ordinals;
    std::vector<Relation> visited_relations;
    populated_view.for_each([&](const Relation& relation, size_t ordinal) {
        visited_ordinals.push_back(ordinal);
        visited_relations.push_back(relation);
    });
    CHECK(visited_ordinals == std::vector<size_t>({0, 1, 2}));
    CHECK(visited_relations.size() == expected.size());
    for (size_t ordinal = 0; ordinal < expected.size(); ++ordinal) {
        CHECK(relations_equal(visited_relations[ordinal], expected[ordinal]));
    }

    TestArtifactCleanup empty_artifacts(unique_base("readonly_view_empty"));
    const auto empty_descriptor = write_finalized_store(empty_artifacts.base, {});
    OOCRelationReader empty_reader(empty_artifacts.base, empty_descriptor);
    ReadOnlyRelationCorpusView empty_view(empty_reader);

    CHECK(empty_view.count() == 0);
    size_t empty_visits = 0;
    empty_view.for_each([&](const Relation&, size_t) { ++empty_visits; });
    CHECK(empty_visits == 0);
    expect_throws<std::out_of_range>([&] { (void)empty_view.read(0); });
}

void test_read_only_ooc_view_for_each_exception_short_circuits() {
    struct StopVisit final {};

    TestArtifactCleanup artifacts(unique_base("readonly_view_short_circuit"));
    const auto expected = make_relations(4);
    const auto descriptor = write_finalized_store(artifacts.base, expected);
    OOCRelationReader reader(artifacts.base, descriptor);
    ReadOnlyRelationCorpusView view(reader);

    std::vector<size_t> visited;
    bool rows_match = true;
    expect_throws<StopVisit>([&] {
        view.for_each([&](const Relation& relation, size_t ordinal) {
            visited.push_back(ordinal);
            rows_match = rows_match && relations_equal(relation, expected[ordinal]);
            if (ordinal == 1) {
                throw StopVisit{};
            }
        });
    });

    CHECK(rows_match);
    CHECK(visited == std::vector<size_t>({0, 1}));
    CHECK(relations_equal(view.read(2), expected[2]));
}

void test_read_only_ooc_view_borrowed_lifetime_has_no_cleanup_side_effects() {
    TestArtifactCleanup artifacts(unique_base("readonly_view_borrowed_lifetime"));
    const auto expected = make_relations(2);
    const auto descriptor = write_finalized_store(artifacts.base, expected);

    {
        OOCRelationReader reader(artifacts.base, descriptor);
        {
            ReadOnlyRelationCorpusView view(reader);
            const ReadOnlyRelationCorpusView copied_view = view;
            CHECK(copied_view.count() == expected.size());
            CHECK(relations_equal(copied_view.read(1), expected[1]));
            CHECK(artifacts_exist(artifacts.base));
        }

        CHECK(reader.count() == expected.size());
        CHECK(relations_equal(reader.read(0), expected[0]));
        CHECK(artifacts_exist(artifacts.base));
    }

    CHECK(artifacts_exist(artifacts.base));
}

void test_read_only_ooc_view_rejects_invalid_and_detects_reader_move() {
    OOCRelationReader default_reader;
    CHECK(!default_reader.valid());
    expect_throws<std::invalid_argument>([&] { (void)ReadOnlyRelationCorpusView(default_reader); });

    TestArtifactCleanup artifacts(unique_base("readonly_view_reader_move"));
    const auto expected = make_relations(2);
    const auto descriptor = write_finalized_store(artifacts.base, expected);
    OOCRelationReader reader(artifacts.base, descriptor);
    ReadOnlyRelationCorpusView view(reader);

    OOCRelationReader moved_reader(std::move(reader));
    CHECK(!reader.valid());
    CHECK(moved_reader.valid());
    expect_throws<std::invalid_argument>([&] { (void)ReadOnlyRelationCorpusView(reader); });
    expect_throws<std::logic_error>([&] { (void)view.count(); });
    expect_throws<std::logic_error>([&] { (void)view.read(0); });

    size_t visits = 0;
    expect_throws<std::logic_error>(
        [&] { view.for_each([&](const Relation&, size_t) { ++visits; }); });
    CHECK(visits == 0);

    ReadOnlyRelationCorpusView moved_reader_view(moved_reader);
    CHECK(moved_reader_view.count() == expected.size());
    CHECK(relations_equal(moved_reader_view.read(1), expected[1]));
    CHECK(artifacts_exist(artifacts.base));

    TestArtifactCleanup replacement_artifacts(unique_base("readonly_view_rebound_target"));
    const auto replacement_expected = make_relations(3);
    const auto replacement_descriptor =
        write_finalized_store(replacement_artifacts.base, replacement_expected);
    OOCRelationReader replacement_reader(replacement_artifacts.base, replacement_descriptor);

    moved_reader = std::move(replacement_reader);
    CHECK(!replacement_reader.valid());
    CHECK(moved_reader.valid());
    expect_throws<std::logic_error>([&] { (void)moved_reader_view.count(); });
    expect_throws<std::logic_error>([&] { (void)moved_reader_view.read(0); });

    size_t rebound_visits = 0;
    expect_throws<std::logic_error>(
        [&] { moved_reader_view.for_each([&](const Relation&, size_t) { ++rebound_visits; }); });
    CHECK(rebound_visits == 0);

    ReadOnlyRelationCorpusView rebound_view(moved_reader);
    CHECK(rebound_view.count() == replacement_expected.size());
    CHECK(relations_equal(rebound_view.read(2), replacement_expected[2]));
    CHECK(artifacts_exist(replacement_artifacts.base));
}

void test_in_memory_move_generation_and_bounds() {
    const auto expected = make_relations(3);
    auto corpus = RelationCorpus::from_in_memory(101, expected);

    CHECK(corpus.valid());
    CHECK(corpus.logical_generation() == 101);
    CHECK(corpus.storage_kind() == RelationStorageKind::InMemory);
    CHECK(corpus.count() == expected.size());
    CHECK(!corpus.empty());
    CHECK(!corpus.ooc_artifact_scope().has_value());
    CHECK(relations_equal(corpus.read(1), expected[1]));
    const Relation* borrowed = corpus.try_borrow_in_memory(1);
    CHECK(borrowed != nullptr);
    CHECK(relations_equal(*borrowed, expected[1]));
    expect_throws<std::out_of_range>([&] { (void)corpus.read(expected.size()); });
    expect_throws<std::out_of_range>([&] { (void)corpus.try_borrow_in_memory(expected.size()); });

    auto moved = std::move(corpus);
    CHECK(!corpus.valid());
    CHECK(moved.valid());
    CHECK(relations_equal(moved.read(2), expected[2]));
    expect_throws<std::logic_error>([&] { (void)corpus.count(); });

    auto assigned = RelationCorpus::from_in_memory(202, make_relations(1));
    assigned = std::move(moved);
    CHECK(!moved.valid());
    CHECK(assigned.logical_generation() == 101);
    CHECK(assigned.count() == expected.size());

    expect_throws<std::invalid_argument>([] { (void)RelationCorpus::from_in_memory(0, {}); });
}

void test_freeze_ooc_path_does_not_follow_namespace_leaf() {
    const std::filesystem::path namespace_leaf(unique_base("freeze_leaf"));
    const std::filesystem::path symlink_target(unique_base("freeze_target"));
    TestPathCleanup namespace_cleanup(namespace_leaf);
    TestPathCleanup target_cleanup(symlink_target);
    CHECK(std::filesystem::create_directory(symlink_target));

    std::error_code symlink_error;
    std::filesystem::create_directory_symlink(symlink_target, namespace_leaf, symlink_error);
    if (symlink_error) {
        // Unprivileged Windows environments may not permit symlink creation.
        return;
    }

    const auto absolute_leaf = std::filesystem::absolute(namespace_leaf).lexically_normal();
    const std::string expected =
        (std::filesystem::weakly_canonical(absolute_leaf.parent_path()) / absolute_leaf.filename())
            .lexically_normal()
            .string();
    const std::string frozen =
        gnfs::relation::relation_corpus_detail::freeze_ooc_path(namespace_leaf.string());
    CHECK(frozen == expected);
    CHECK(frozen != std::filesystem::weakly_canonical(namespace_leaf).string());
}

void test_finalized_ooc_roundtrip_and_preserve_lifetime() {
    TestArtifactCleanup artifacts(unique_base("roundtrip"));
    const auto expected = make_relations(4);
    const auto descriptor = write_finalized_store(artifacts.base, expected);

    {
        auto corpus = RelationCorpus::from_finalized_ooc(9'001, artifacts.base, descriptor,
                                                         OOCCleanupPolicy::Preserve);
        CHECK(corpus.storage_kind() == RelationStorageKind::FinalizedOOC);
        CHECK(corpus.logical_generation() == 9'001);
        CHECK(corpus.logical_generation() != descriptor.generation);
        CHECK(corpus.count() == expected.size());
        const auto scope = corpus.ooc_artifact_scope();
        CHECK(scope.has_value());
        CHECK(scope->base_path == std::filesystem::weakly_canonical(artifacts.base).string());
        CHECK(scope->descriptor == descriptor);
        CHECK(scope->cleanup_directory.empty());
        CHECK(corpus.try_borrow_in_memory(0) == nullptr);
        for (size_t i = 0; i < expected.size(); ++i) {
            CHECK(relations_equal(corpus.read(i), expected[i]));
        }
        expect_throws<std::out_of_range>([&] { (void)corpus.read(expected.size()); });
        expect_throws<std::out_of_range>(
            [&] { (void)corpus.try_borrow_in_memory(expected.size()); });

        auto moved = std::move(corpus);
        CHECK(!corpus.valid());
        CHECK(moved.count() == expected.size());

        const auto selection = RelationSelection::from_ordinals(moved, {2, 0, 2});
        const auto selected = materialize_selected(moved, selection);
        CHECK(selected.size() == 2);
        CHECK(relations_equal(selected[0], expected[2]));
        CHECK(relations_equal(selected[1], expected[0]));
    }

    CHECK(artifacts_exist(artifacts.base));
}

void test_finalized_ooc_cleanup_and_move_assignment() {
    TestArtifactCleanup first_artifacts(unique_base("cleanup_first"));
    TestArtifactCleanup second_artifacts(unique_base("cleanup_second"));
    const auto first_relations = make_relations(2);
    const auto second_relations = make_relations(3);
    auto first_writer = make_finalized_writer(first_artifacts.base, first_relations);
    auto second_writer = make_finalized_writer(second_artifacts.base, second_relations);

    {
        auto first = RelationCorpus::from_owned_finalized_ooc(301, *first_writer,
                                                              OOCCleanupPolicy::RemoveArtifacts);
        auto second = RelationCorpus::from_owned_finalized_ooc(302, *second_writer,
                                                               OOCCleanupPolicy::RemoveArtifacts);
        first_writer.reset();
        second_writer.reset();

        CHECK(artifacts_exist(first_artifacts.base));
        CHECK(artifacts_exist(second_artifacts.base));

        // Destroying the previous state during move assignment must close its
        // mmap before cleanup. This is the Windows-sensitive ownership path.
        first = std::move(second);
        CHECK(!second.valid());
        CHECK(!std::filesystem::exists(first_artifacts.base + ".relidx"));
        CHECK(!std::filesystem::exists(first_artifacts.base + ".reldata"));
        CHECK(artifacts_exist(second_artifacts.base));
        CHECK(first.logical_generation() == 302);
        CHECK(first.count() == second_relations.size());
    }

    CHECK(!std::filesystem::exists(second_artifacts.base + ".relidx"));
    CHECK(!std::filesystem::exists(second_artifacts.base + ".reldata"));
}

void test_ooc_adoption_fails_closed() {
    TestArtifactCleanup artifacts(unique_base("fail_closed"));
    const auto expected = make_relations(2);
    const auto descriptor = write_finalized_store(artifacts.base, expected);

    expect_throws<std::invalid_argument>([&] {
        (void)RelationCorpus::from_finalized_ooc(401, artifacts.base, descriptor,
                                                 OOCCleanupPolicy::RemoveArtifacts);
    });
    {
        auto reopened = RelationCorpus::from_finalized_ooc(401, artifacts.base, descriptor,
                                                           OOCCleanupPolicy::Preserve);
        CHECK(!reopened.arm_ooc_cleanup());
    }
    CHECK(artifacts_exist(artifacts.base));

    expect_throws<std::invalid_argument>(
        [&] { (void)RelationCorpus::from_finalized_ooc(401, "", descriptor); });
    expect_throws<std::invalid_argument>([&] {
        (void)RelationCorpus::from_finalized_ooc(401, std::string("invalid\0path", 12), descriptor);
    });
    CHECK(artifacts_exist(artifacts.base));

    expect_throws<std::invalid_argument>(
        [&] { (void)RelationCorpus::from_finalized_ooc(0, artifacts.base, descriptor); });
    CHECK(artifacts_exist(artifacts.base));

    auto zero_descriptor_generation = descriptor;
    zero_descriptor_generation.generation = 0;
    expect_throws<std::invalid_argument>([&] {
        (void)RelationCorpus::from_finalized_ooc(401, artifacts.base, zero_descriptor_generation);
    });
    CHECK(artifacts_exist(artifacts.base));

    auto legacy_v2 = descriptor;
    legacy_v2.format_version = OOCRelationWriter::FORMAT_VERSION_V2;
    expect_throws<std::invalid_argument>(
        [&] { (void)RelationCorpus::from_finalized_ooc(401, artifacts.base, legacy_v2); });
    CHECK(artifacts_exist(artifacts.base));

    auto foreign_store = descriptor;
    ++foreign_store.store_id;
    expect_throws<std::runtime_error>(
        [&] { (void)RelationCorpus::from_finalized_ooc(401, artifacts.base, foreign_store); });
    CHECK(artifacts_exist(artifacts.base));

    auto wrong_count = descriptor;
    ++wrong_count.count;
    expect_throws<std::runtime_error>(
        [&] { (void)RelationCorpus::from_finalized_ooc(401, artifacts.base, wrong_count); });
    CHECK(artifacts_exist(artifacts.base));
}

void test_owned_adoption_failure_retains_receipt_for_retry() {
    TestArtifactCleanup artifacts(unique_base("owned_adoption_retry"));
    const std::string foreign_root = unique_base("foreign_private_lease");
    const std::string foreign_store =
        (std::filesystem::path(foreign_root + ".gnfs-sink-lease") / "corpus").string();
    const auto foreign_paths = OOCCleanupTransaction::paths_for(foreign_store);
    TestPathCleanup foreign_directory(foreign_paths.private_directory);
    TestPathCleanup foreign_lock(foreign_paths.lock_path);
    auto foreign_reservation = OOCCleanupTransaction::reserve_private_lease(foreign_store);
    CHECK(foreign_reservation.completed());

    const auto expected = make_relations(2);
    auto writer = make_finalized_writer(artifacts.base, expected);

    expect_throws<std::invalid_argument>([&] {
        (void)RelationCorpus::from_owned_finalized_ooc(
            450, *writer, OOCCleanupPolicy::RemoveArtifacts, &*foreign_reservation.ownership);
    });
    CHECK(writer->has_cleanup_ownership_receipt());
    CHECK(artifacts_exist(artifacts.base));
    CHECK(OOCCleanupTransaction::remove_private_lease(*foreign_reservation.ownership).completed());

    {
        auto corpus =
            RelationCorpus::from_owned_finalized_ooc(450, *writer, OOCCleanupPolicy::Preserve);
        CHECK(!writer->has_cleanup_ownership_receipt());
        CHECK(corpus.count() == expected.size());
        CHECK(corpus.arm_ooc_cleanup());
        writer.reset();
    }
    CHECK(!std::filesystem::exists(artifacts.base + ".relidx"));
    CHECK(!std::filesystem::exists(artifacts.base + ".reldata"));
}

void test_selection_stable_dedup_order_and_identity() {
    const auto expected = make_relations(5);
    auto corpus = RelationCorpus::from_in_memory(501, expected);
    const auto selection = RelationSelection::from_ordinals(corpus, {3, 1, 3, 4, 1, 0});

    const std::vector<size_t> expected_ordinals{3, 1, 4, 0};
    CHECK(selection.logical_generation() == 501);
    CHECK(selection.source_count() == expected.size());
    CHECK(selection.ordinals() == expected_ordinals);
    CHECK(selection.count() == expected_ordinals.size());
    CHECK(selection.source_ordinal(2) == 4);
    CHECK(relations_equal(selection.read(corpus, 0), expected[3]));
    expect_throws<std::out_of_range>([&] { (void)selection.source_ordinal(selection.count()); });
    expect_throws<std::out_of_range>(
        [&] { (void)RelationSelection::from_ordinals(corpus, {0, expected.size()}); });

    // Moving the corpus handle preserves the opaque instance identity.
    auto moved = std::move(corpus);
    const auto materialized = materialize_selected(moved, selection);
    CHECK(materialized.size() == expected_ordinals.size());
    for (size_t i = 0; i < expected_ordinals.size(); ++i) {
        CHECK(relations_equal(materialized[i], expected[expected_ordinals[i]]));
    }

    auto wrong_generation = RelationCorpus::from_in_memory(502, expected);
    expect_throws<std::invalid_argument>(
        [&] { (void)materialize_selected(wrong_generation, selection); });

    auto wrong_count = RelationCorpus::from_in_memory(501, make_relations(4));
    expect_throws<std::invalid_argument>(
        [&] { (void)materialize_selected(wrong_count, selection); });

    auto same_generation_and_count = RelationCorpus::from_in_memory(501, expected);
    expect_throws<std::invalid_argument>(
        [&] { (void)materialize_selected(same_generation_and_count, selection); });

    const auto empty_selection = RelationSelection::from_ordinals(moved, {});
    CHECK(empty_selection.empty());
    CHECK(materialize_selected(moved, empty_selection).empty());
}

void test_selection_xor_parity_canonical_order_and_identity() {
    const auto expected = make_relations(8);
    auto corpus = RelationCorpus::from_in_memory(551, expected);

    const auto selection =
        RelationSelection::from_xor_ordinals(corpus, {7, 3, 5, 1, 7, 3, 5, 5, 1, 1});
    const std::vector<size_t> expected_ordinals{1, 5};
    CHECK(selection.ordinals() == expected_ordinals);

    const auto all_cancelled = RelationSelection::from_xor_ordinals(corpus, {6, 2, 6, 0, 2, 0});
    CHECK(all_cancelled.empty());
    CHECK(RelationSelection::from_xor_ordinals(corpus, {}).empty());
    expect_throws<std::out_of_range>(
        [&] { (void)RelationSelection::from_xor_ordinals(corpus, {0, expected.size()}); });

    // Selections stay bound to the corpus state across a handle move.
    auto moved = std::move(corpus);
    const auto materialized = materialize_selected(moved, selection);
    CHECK(materialized.size() == expected_ordinals.size());
    for (size_t i = 0; i < expected_ordinals.size(); ++i) {
        CHECK(relations_equal(materialized[i], expected[expected_ordinals[i]]));
    }

    auto foreign = RelationCorpus::from_in_memory(551, expected);
    expect_throws<std::invalid_argument>([&] { (void)materialize_selected(foreign, selection); });
    expect_throws<std::logic_error>(
        [&] { (void)RelationSelection::from_xor_ordinals(corpus, {0}); });
}

void test_deterministic_sample_boundaries_fixture_and_identity() {
    const auto expected = make_relations(10);
    auto corpus = RelationCorpus::from_in_memory(552, expected);

    const auto empty = RelationSelection::deterministic_sample(corpus, 0, 42);
    CHECK(empty.empty());

    const auto full = RelationSelection::deterministic_sample(corpus, expected.size(), 42);
    const std::vector<size_t> all_ordinals{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(full.ordinals() == all_ordinals);

    // Exact fixture for the repository-owned SplitMix64 + unbiased reservoir
    // algorithm. This must not change with the host standard library.
    const auto sample = RelationSelection::deterministic_sample(corpus, 4, 42);
    const std::vector<size_t> expected_sample{4, 5, 6, 9};
    CHECK(sample.ordinals() == expected_sample);

    const auto repeated = RelationSelection::deterministic_sample(corpus, 4, 42);
    CHECK(repeated.ordinals() == sample.ordinals());
    CHECK(sample.count() == 4);
    for (size_t i = 1; i < sample.count(); ++i) {
        CHECK(sample.source_ordinal(i - 1) < sample.source_ordinal(i));
    }

    expect_throws<std::invalid_argument>(
        [&] { (void)RelationSelection::deterministic_sample(corpus, expected.size() + 1, 42); });

    auto moved = std::move(corpus);
    const auto materialized = materialize_selected(moved, sample);
    CHECK(materialized.size() == expected_sample.size());
    for (size_t i = 0; i < expected_sample.size(); ++i) {
        CHECK(relations_equal(materialized[i], expected[expected_sample[i]]));
    }

    auto foreign = RelationCorpus::from_in_memory(552, expected);
    expect_throws<std::invalid_argument>([&] { (void)materialize_selected(foreign, sample); });
    expect_throws<std::logic_error>(
        [&] { (void)RelationSelection::deterministic_sample(corpus, 1, 42); });
}

void test_selection_rejects_distinct_vector_and_ooc_instances() {
    TestArtifactCleanup first_artifacts(unique_base("selection_identity_ooc_first"));
    TestArtifactCleanup second_artifacts(unique_base("selection_identity_ooc_second"));
    const auto expected = make_relations(3);
    const auto first_descriptor = write_finalized_store(first_artifacts.base, expected);

    auto second_expected = expected;
    second_expected[0].a += 200;
    const auto second_descriptor = write_finalized_store(second_artifacts.base, second_expected);
    CHECK(second_descriptor.store_id != first_descriptor.store_id);
    CHECK(second_descriptor.count == first_descriptor.count);

    auto vector_corpus = RelationCorpus::from_in_memory(601, expected);
    auto vector_selection = RelationSelection::from_ordinals(vector_corpus, {2, 0});

    auto different_vector_relations = expected;
    different_vector_relations[0].a += 100;
    auto different_vector = RelationCorpus::from_in_memory(601, different_vector_relations);
    expect_throws<std::invalid_argument>(
        [&] { (void)materialize_selected(different_vector, vector_selection); });

    auto first_ooc =
        RelationCorpus::from_finalized_ooc(601, first_artifacts.base, first_descriptor);
    expect_throws<std::invalid_argument>(
        [&] { (void)materialize_selected(first_ooc, vector_selection); });

    const auto ooc_selection = RelationSelection::from_ordinals(first_ooc, {1, 2});
    expect_throws<std::invalid_argument>(
        [&] { (void)materialize_selected(vector_corpus, ooc_selection); });

    // A separate OOC store with the same logical generation and relation count
    // is still a distinct corpus instance.
    auto second_ooc =
        RelationCorpus::from_finalized_ooc(601, second_artifacts.base, second_descriptor);
    CHECK(second_ooc.logical_generation() == first_ooc.logical_generation());
    CHECK(second_ooc.count() == first_ooc.count());
    expect_throws<std::invalid_argument>(
        [&] { (void)materialize_selected(second_ooc, ooc_selection); });

    // Reopening the exact same finalized descriptor creates a new corpus
    // instance and must not inherit selections from the earlier handle.
    auto reopened_ooc =
        RelationCorpus::from_finalized_ooc(601, first_artifacts.base, first_descriptor);
    expect_throws<std::invalid_argument>(
        [&] { (void)materialize_selected(reopened_ooc, ooc_selection); });

    auto moved_ooc = std::move(first_ooc);
    const auto selected_after_move = materialize_selected(moved_ooc, ooc_selection);
    CHECK(selected_after_move.size() == 2);
    CHECK(relations_equal(selected_after_move[0], expected[1]));
    CHECK(relations_equal(selected_after_move[1], expected[2]));
}

void test_collector_handoff_and_independent_cleanup() {
    TestArtifactCleanup first_artifacts(unique_base("collector_handoff_first"));
    TestArtifactCleanup second_artifacts(unique_base("collector_handoff_second"));

    std::vector<Relation> first_relations;
    std::vector<Relation> second_relations;
    for (uint32_t i = 0; i < 2; ++i) {
        first_relations.push_back(make_relation(static_cast<int64_t>(41 + 2 * i), 1, 30 + i));
    }
    for (uint32_t i = 0; i < 3; ++i) {
        second_relations.push_back(make_relation(static_cast<int64_t>(61 + 2 * i), 1, 40 + i));
    }

    {
        auto first = collect_owned_corpus(701, first_artifacts.base, first_relations);
        CHECK(artifacts_exist(first_artifacts.base));
        CHECK(first.count() == first_relations.size());
        for (size_t i = 0; i < first_relations.size(); ++i) {
            CHECK(relations_equal(first.read(i), first_relations[i]));
        }

        const auto first_selection = RelationSelection::from_ordinals(first, {1, 0, 1});
        CHECK(relations_equal(first_selection.read(first, 0), first_relations[1]));
        const auto selected = materialize_selected(first, first_selection);
        CHECK(selected.size() == 2);
        CHECK(relations_equal(selected[0], first_relations[1]));
        CHECK(relations_equal(selected[1], first_relations[0]));

        {
            auto second = collect_owned_corpus(702, second_artifacts.base, second_relations);
            CHECK(second.count() == second_relations.size());
            CHECK(artifacts_exist(first_artifacts.base));
            CHECK(artifacts_exist(second_artifacts.base));
        }

        CHECK(artifacts_exist(first_artifacts.base));
        CHECK(!std::filesystem::exists(second_artifacts.base + ".relidx"));
        CHECK(!std::filesystem::exists(second_artifacts.base + ".reldata"));
    }

    CHECK(!std::filesystem::exists(first_artifacts.base + ".relidx"));
    CHECK(!std::filesystem::exists(first_artifacts.base + ".reldata"));
    CHECK(!std::filesystem::exists(second_artifacts.base + ".relidx"));
    CHECK(!std::filesystem::exists(second_artifacts.base + ".reldata"));
}

} // namespace

int main() {
    try {
        test_read_only_ooc_view_nonempty_and_empty();
        test_read_only_ooc_view_for_each_exception_short_circuits();
        test_read_only_ooc_view_borrowed_lifetime_has_no_cleanup_side_effects();
        test_read_only_ooc_view_rejects_invalid_and_detects_reader_move();
        test_in_memory_move_generation_and_bounds();
        test_freeze_ooc_path_does_not_follow_namespace_leaf();
        test_finalized_ooc_roundtrip_and_preserve_lifetime();
        test_finalized_ooc_cleanup_and_move_assignment();
        test_ooc_adoption_fails_closed();
        test_owned_adoption_failure_retains_receipt_for_retry();
        test_selection_stable_dedup_order_and_identity();
        test_selection_xor_parity_canonical_order_and_identity();
        test_deterministic_sample_boundaries_fixture_and_identity();
        test_selection_rejects_distinct_vector_and_ooc_instances();
        test_collector_handoff_and_independent_cleanup();
        std::cout << "All relation corpus tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "relation corpus test failure: " << error.what() << '\n';
        return 1;
    }
}
