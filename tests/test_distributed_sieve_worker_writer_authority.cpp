// Reuse the worker-entry test's real P8 self-exec fixture in the same
// translation unit. Renaming its main keeps this test focused on the next
// single-use transition: authenticated entry -> exact relation-writer authority.
#define main gnfs_embedded_distributed_sieve_worker_entry_test_main
#include "test_distributed_sieve_worker_entry.cpp"
#undef main

#include "distributed_sieve_worker_writer_internal.hpp"

#include <gnfs/core/relation.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>

#include <cstdlib>
#include <fstream>
#include <map>
#include <set>

namespace {

using WriterAuthority = entry::DistributedSieveWorkerWriterAuthorityV1;
using WriterResult = entry::DistributedSieveWorkerWriterAdoptionResultV1;
using WriterPhase = entry::DistributedSieveWorkerWriterPhaseV1;
using WriterRollback = entry::DistributedSieveWorkerWriterRollbackV1;
using WriterStatus = entry::DistributedSieveWorkerWriterStatusV1;
using Relation = gnfs::core::Relation;

static_assert(!std::is_default_constructible_v<WriterAuthority>);
static_assert(!std::is_copy_constructible_v<WriterAuthority>);
static_assert(!std::is_copy_assignable_v<WriterAuthority>);
static_assert(std::is_nothrow_move_constructible_v<WriterAuthority>);
static_assert(!std::is_move_assignable_v<WriterAuthority>);
static_assert(!std::is_constructible_v<WriterAuthority, std::filesystem::path>);
static_assert(!std::is_constructible_v<WriterAuthority, std::string>);
static_assert(!std::is_constructible_v<WriterAuthority, int>);
static_assert(!std::is_copy_constructible_v<WriterResult>);
static_assert(std::is_nothrow_move_constructible_v<WriterResult>);
static_assert(noexcept(entry::consume_distributed_sieve_worker_writer_v1(
    std::declval<entry::DistributedSieveWorkerEntryV1&&>())));
static_assert(noexcept(std::declval<const WriterAuthority&>().valid()));
static_assert(noexcept(std::declval<const WriterAuthority&>().finalized()));
static_assert(noexcept(std::declval<const WriterAuthority&>().count()));
static_assert(noexcept(std::declval<const WriterAuthority&>().record()));
static_assert(noexcept(std::declval<const WriterAuthority&>().manifest()));
static_assert(noexcept(std::declval<const WriterAuthority&>().identity()));
static_assert(noexcept(std::declval<const WriterAuthority&>().chunk()));
static_assert(noexcept(std::declval<const WriterAuthority&>().witness()));

[[nodiscard]] Relation make_writer_relation(std::int64_t a, std::uint64_t b, std::uint32_t seed) {
    Relation relation(a, b);
    relation.rational_factors = {seed, static_cast<std::uint32_t>(seed + 2)};
    relation.algebraic_factors = {
        static_cast<std::uint32_t>(seed + 1),
        static_cast<std::uint32_t>(seed + 3),
    };
    relation.rational_large_prime = {
        {static_cast<std::uint64_t>(1009 + seed), 0, static_cast<std::uint8_t>(1 + seed % 2)},
    };
    relation.algebraic_large_prime = {
        {static_cast<std::uint64_t>(2003 + seed), static_cast<std::uint64_t>(17 + seed), 1},
    };
    relation.extra_ab_pairs = {{a - 10, b + 1}, {a + 20, b + 2}};
    return relation;
}

[[nodiscard]] bool writer_relation_equal(const Relation& left, const Relation& right) noexcept {
    return left.a == right.a && left.b == right.b &&
           left.rational_factors == right.rational_factors &&
           left.algebraic_factors == right.algebraic_factors &&
           left.rational_large_prime == right.rational_large_prime &&
           left.algebraic_large_prime == right.algebraic_large_prime &&
           left.extra_ab_pairs == right.extra_ab_pairs;
}

template <typename Callable>
[[nodiscard]] bool rejects_writer_mutation(Callable&& callable) noexcept {
    try {
        std::forward<Callable>(callable)();
    } catch (const std::logic_error&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

template <typename Callable>
[[nodiscard]] bool rejects_writer_runtime_failure(Callable&& callable) noexcept {
    try {
        std::forward<Callable>(callable)();
    } catch (const std::runtime_error&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

enum class WriterChildScenario : std::uint32_t {
    happy = 1,
    reserved_replacement_sandwich,
    private_directory_replacement_sandwich,
    base_lock_replacement_sandwich,
    wrong_base_path_digest,
    foreign_staging_residue,
    post_authority_base_lock_replacement,
    post_authority_reserved_replacement,
    post_authority_private_directory_replacement,
    construction_clean_rollback,
    construction_foreign_index_residue,
};

inline constexpr std::string_view WRITER_CHILD_ARGUMENT = "--worker-writer-authority-child";
inline constexpr std::uint32_t WRITER_CHILD_REPORT_MAGIC = 0x47575731U;

inline constexpr std::uint32_t WRITER_FLAG_ENTRY_ADOPTED = 1U << 0U;
inline constexpr std::uint32_t WRITER_FLAG_FIXED_FDS_CLOSED = 1U << 1U;
inline constexpr std::uint32_t WRITER_FLAG_AUTHORITY_READY = 1U << 2U;
inline constexpr std::uint32_t WRITER_FLAG_BINDINGS_VALID = 1U << 3U;
inline constexpr std::uint32_t WRITER_FLAG_SECOND_CONSUME_REJECTED = 1U << 4U;
inline constexpr std::uint32_t WRITER_FLAG_FORK_WRITE_REJECTED = 1U << 5U;
inline constexpr std::uint32_t WRITER_FLAG_FORK_FINALIZE_REJECTED = 1U << 6U;
inline constexpr std::uint32_t WRITER_FLAG_PARENT_UNCHANGED_AFTER_FORK = 1U << 7U;
inline constexpr std::uint32_t WRITER_FLAG_WROTE_EXACT_PAIR = 1U << 8U;
inline constexpr std::uint32_t WRITER_FLAG_FINALIZED = 1U << 9U;
inline constexpr std::uint32_t WRITER_FLAG_POST_FINALIZE_REJECTED = 1U << 10U;
inline constexpr std::uint32_t WRITER_FLAG_MUTATION_INVOKED = 1U << 11U;
inline constexpr std::uint32_t WRITER_FLAG_MUTATION_SUCCEEDED = 1U << 12U;
inline constexpr std::uint32_t WRITER_FLAG_FORK_NORMAL_RETURN = 1U << 13U;
inline constexpr std::uint32_t WRITER_FLAG_POST_AUTHORITY_WRITE_REJECTED = 1U << 14U;
inline constexpr std::uint32_t WRITER_FLAG_POST_AUTHORITY_INVALIDATED = 1U << 15U;
inline constexpr std::uint32_t WRITER_FLAG_POST_AUTHORITY_FINALIZE_REJECTED = 1U << 16U;
inline constexpr std::uint32_t WRITER_FLAG_CONSTRUCTION_HOOK_INVOKED = 1U << 17U;
inline constexpr std::uint32_t WRITER_FLAG_RECONCILIATION_REQUIRED = 1U << 18U;

inline constexpr std::uint32_t WRITER_HAPPY_FLAGS =
    WRITER_FLAG_ENTRY_ADOPTED | WRITER_FLAG_FIXED_FDS_CLOSED | WRITER_FLAG_AUTHORITY_READY |
    WRITER_FLAG_BINDINGS_VALID | WRITER_FLAG_SECOND_CONSUME_REJECTED |
    WRITER_FLAG_FORK_WRITE_REJECTED | WRITER_FLAG_FORK_FINALIZE_REJECTED |
    WRITER_FLAG_PARENT_UNCHANGED_AFTER_FORK | WRITER_FLAG_WROTE_EXACT_PAIR | WRITER_FLAG_FINALIZED |
    WRITER_FLAG_POST_FINALIZE_REJECTED | WRITER_FLAG_FORK_NORMAL_RETURN;

struct WriterChildReport final {
    std::uint32_t magic = WRITER_CHILD_REPORT_MAGIC;
    std::uint32_t scenario = 0;
    std::uint32_t flags = 0;
    std::uint32_t entry_status = 0;
    std::uint32_t entry_phase = 0;
    std::uint32_t writer_status = 0;
    std::uint32_t writer_phase = 0;
    std::uint32_t writer_rollback = 0;
    std::uint32_t second_status = 0;
    std::uint32_t second_phase = 0;
    std::uint32_t chunk_id = 0;
    std::uint32_t attempt_ordinal = 0;
    std::uint64_t count_after_write = 0;
    std::int32_t mutation_error = 0;
    std::int32_t writer_native_error = 0;
    std::int32_t rollback_native_error = 0;
    Digest attempt_digest;
    Digest manifest_digest;
    Digest work_digest;
};

static_assert(std::is_trivially_copyable_v<WriterChildReport>);

[[nodiscard]] std::optional<WriterChildScenario>
parse_writer_child_scenario(std::string_view raw) noexcept {
    std::uint32_t value = 0;
    if (raw.empty()) {
        return std::nullopt;
    }
    for (const char character : raw) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        value = value * 10U + static_cast<std::uint32_t>(character - '0');
    }
    if (value < static_cast<std::uint32_t>(WriterChildScenario::happy) ||
        value >
            static_cast<std::uint32_t>(WriterChildScenario::construction_foreign_index_residue)) {
        return std::nullopt;
    }
    return static_cast<WriterChildScenario>(value);
}

#if !defined(_WIN32)

[[nodiscard]] constexpr bool is_post_authority_drift(WriterChildScenario scenario) noexcept {
    return scenario == WriterChildScenario::post_authority_base_lock_replacement ||
           scenario == WriterChildScenario::post_authority_reserved_replacement ||
           scenario == WriterChildScenario::post_authority_private_directory_replacement;
}

[[nodiscard]] constexpr bool is_construction_failure(WriterChildScenario scenario) noexcept {
    return scenario == WriterChildScenario::construction_clean_rollback ||
           scenario == WriterChildScenario::construction_foreign_index_residue;
}

struct NamespaceNode final {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    mode_t mode = 0;
    std::vector<std::byte> bytes;
};

using NamespaceSnapshot = std::map<std::string, NamespaceNode>;

[[nodiscard]] std::vector<std::byte> read_snapshot_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    CHECK(stream.is_open());
    stream.seekg(0, std::ios::end);
    const auto end = stream.tellg();
    CHECK(end >= 0);
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        CHECK(stream.good());
    }
    return bytes;
}

[[nodiscard]] NamespaceSnapshot snapshot_namespace(const std::filesystem::path& root) {
    NamespaceSnapshot result;
    for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
        struct stat metadata{};
        CHECK(::lstat(item.path().c_str(), &metadata) == 0);
        NamespaceNode node{
            .device = static_cast<std::uint64_t>(metadata.st_dev),
            .inode = static_cast<std::uint64_t>(metadata.st_ino),
            .mode = metadata.st_mode,
        };
        if (S_ISREG(metadata.st_mode)) {
            node.bytes = read_snapshot_file(item.path());
        }
        const auto relative = std::filesystem::relative(item.path(), root).generic_string();
        CHECK(result.emplace(relative, std::move(node)).second);
    }
    return result;
}

[[nodiscard]] bool same_baseline_node(const NamespaceNode& left,
                                      const NamespaceNode& right) noexcept {
    if (left.device != right.device || left.inode != right.inode || left.mode != right.mode) {
        return false;
    }
    if (S_ISREG(left.mode)) {
        return left.bytes == right.bytes;
    }
    return true;
}

void require_exact_happy_namespace_delta(const WorkerEntryFixture& fixture,
                                         const NamespaceSnapshot& baseline) {
    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    const auto after = snapshot_namespace(fixture.root);

    for (const auto& [path, expected] : baseline) {
        const auto found = after.find(path);
        CHECK(found != after.end());
        CHECK(same_baseline_node(expected, found->second));
    }

    std::set<std::string> additions;
    for (const auto& [path, node] : after) {
        (void)node;
        if (!baseline.contains(path)) {
            additions.insert(path);
        }
    }
    const std::string prefix = names->private_directory_leaf + "/";
    const std::set<std::string> expected{
        prefix + "corpus.relidx",
        prefix + "corpus.reldata",
    };
    CHECK(additions == expected);
}

void require_no_handoff_or_cleanup_publication(const WorkerEntryFixture& fixture) {
    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    for (const auto& item : std::filesystem::recursive_directory_iterator(fixture.root)) {
        const std::string leaf = item.path().filename().string();
        CHECK(leaf.find("handoff") == std::string::npos);
        if (leaf.find("cleanup") != std::string::npos && leaf != names->base_lock_leaf) {
            fail("writer authority publishes no cleanup artifact", __LINE__, leaf);
        }
    }
}

void require_unfinalized_corpus(const std::filesystem::path& base) {
    CHECK(std::filesystem::is_regular_file(base.string() + ".relidx"));
    CHECK(std::filesystem::is_regular_file(base.string() + ".reldata"));
    bool rejected = false;
    try {
        gnfs::relation::OOCRelationReader reader(base.string());
        (void)reader;
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
}

void require_distinct_regular_inodes(const std::filesystem::path& left,
                                     const std::filesystem::path& right) {
    struct stat left_metadata{};
    struct stat right_metadata{};
    CHECK(::lstat(left.c_str(), &left_metadata) == 0);
    CHECK(::lstat(right.c_str(), &right_metadata) == 0);
    CHECK(S_ISREG(left_metadata.st_mode));
    CHECK(S_ISREG(right_metadata.st_mode));
    CHECK(left_metadata.st_dev != right_metadata.st_dev ||
          left_metadata.st_ino != right_metadata.st_ino);
}

void require_distinct_owner_directories(const std::filesystem::path& left,
                                        const std::filesystem::path& right) {
    struct stat left_metadata{};
    struct stat right_metadata{};
    CHECK(::lstat(left.c_str(), &left_metadata) == 0);
    CHECK(::lstat(right.c_str(), &right_metadata) == 0);
    CHECK(S_ISDIR(left_metadata.st_mode));
    CHECK(S_ISDIR(right_metadata.st_mode));
    CHECK((left_metadata.st_mode & static_cast<mode_t>(07777)) == 0700);
    CHECK((right_metadata.st_mode & static_cast<mode_t>(07777)) == 0700);
    CHECK(left_metadata.st_dev != right_metadata.st_dev ||
          left_metadata.st_ino != right_metadata.st_ino);
}

void configure_writer_replacement(WriterChildScenario scenario,
                                  const wave::DistributedSieveWorkerAttemptNamesV1& names,
                                  ReplacementHookContext& replacement) {
    switch (scenario) {
    case WriterChildScenario::reserved_replacement_sandwich:
        replacement.leaf = names.reserved_leaf;
        replacement.displaced_leaf = ".gnfs-test-writer-displaced-reserved";
        break;
    case WriterChildScenario::private_directory_replacement_sandwich:
        replacement.kind = ReplacementHookContext::Kind::private_directory_replacement;
        replacement.leaf = names.private_directory_leaf;
        replacement.displaced_leaf = ".gnfs-test-writer-displaced-private-directory";
        break;
    case WriterChildScenario::base_lock_replacement_sandwich:
        replacement.leaf = names.base_lock_leaf;
        replacement.displaced_leaf = ".gnfs-test-writer-displaced-base-lock";
        break;
    case WriterChildScenario::happy:
    case WriterChildScenario::wrong_base_path_digest:
    case WriterChildScenario::foreign_staging_residue:
    case WriterChildScenario::post_authority_base_lock_replacement:
    case WriterChildScenario::post_authority_reserved_replacement:
    case WriterChildScenario::post_authority_private_directory_replacement:
    case WriterChildScenario::construction_clean_rollback:
    case WriterChildScenario::construction_foreign_index_residue:
        break;
    }
}

void configure_post_authority_replacement(WriterChildScenario scenario,
                                          const wave::DistributedSieveWorkerAttemptNamesV1& names,
                                          ReplacementHookContext& replacement) {
    switch (scenario) {
    case WriterChildScenario::post_authority_base_lock_replacement:
        replacement.leaf = names.base_lock_leaf;
        replacement.displaced_leaf = ".gnfs-test-writer-post-displaced-base-lock";
        break;
    case WriterChildScenario::post_authority_reserved_replacement:
        replacement.leaf = names.reserved_leaf;
        replacement.displaced_leaf = ".gnfs-test-writer-post-displaced-reserved";
        break;
    case WriterChildScenario::post_authority_private_directory_replacement:
        replacement.kind = ReplacementHookContext::Kind::private_directory_replacement;
        replacement.leaf = names.private_directory_leaf;
        replacement.displaced_leaf = ".gnfs-test-writer-post-displaced-private-directory";
        break;
    default:
        fail("post-authority replacement scenario", __LINE__);
    }
}

struct ConstructionFailureHookContext final {
    bool replace_index = false;
    int root_descriptor = -1;
    std::string private_directory_leaf;
    bool invoked = false;
    bool replaced = false;
    int error = 0;
};

[[nodiscard]] bool stop_after_fresh_index_reserved(gnfs::relation::OOCPrivateLeaseFaultPoint point,
                                                   void* opaque) noexcept {
    auto& context = *static_cast<ConstructionFailureHookContext*>(opaque);
    if (point != gnfs::relation::OOCPrivateLeaseFaultPoint::FreshIndexReserved) {
        return false;
    }
    context.invoked = true;
    if (!context.replace_index) {
        return true;
    }

    int directory = -1;
    do {
        directory = ::openat(context.root_descriptor, context.private_directory_leaf.c_str(),
                             O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (directory < 0 && errno == EINTR);
    if (directory < 0) {
        context.error = errno;
        return true;
    }

    context.error = replace_regular_at_same_bytes(directory, "corpus.relidx",
                                                  ".gnfs-test-writer-owned-index-displaced");
    context.replaced = context.error == 0;
    if (::close(directory) != 0 && context.error == 0) {
        context.error = errno;
        context.replaced = false;
    }
    return true;
}

[[nodiscard]] int fork_inherited_authority_probe(WriterResult& converted,
                                                 const Relation& relation) {
    auto& authority = *converted.writer;
    const bool invalid = !authority.valid() && !authority.finalized() && authority.count() == 0;
    const bool write_rejected = rejects_writer_mutation([&] { (void)authority.write(relation); });
    const bool finalize_rejected = rejects_writer_mutation([&] { authority.finalize(); });

    // Exercise the inherited authority destructor before a normal return from
    // main. This intentionally does not use _exit(): buffered native streams
    // in the forked copy must not flush or finalize the parent's corpus.
    converted.writer.reset();
    return invalid && write_rejected && finalize_rejected ? EXIT_SUCCESS : EXIT_FAILURE;
}

[[nodiscard]] int run_writer_child(WriterChildScenario scenario) noexcept {
    WriterChildReport report;
    report.scenario = static_cast<std::uint32_t>(scenario);
    try {
        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        if (!names.has_value()) {
            (void)write_exact(STDOUT_FILENO, &report, sizeof(report));
            return 90;
        }

        int root_descriptor = -1;
        do {
            root_descriptor = ::fcntl(
                process::DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR, F_DUPFD_CLOEXEC,
                process::DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR);
        } while (root_descriptor < 0 && errno == EINTR);
        if (root_descriptor < 0) {
            report.mutation_error = errno;
            (void)write_exact(STDOUT_FILENO, &report, sizeof(report));
            return 91;
        }

        auto adopted = entry::adopt_distributed_sieve_worker_entry_v1();
        report.entry_status = static_cast<std::uint32_t>(adopted.diagnostic.status);
        report.entry_phase = static_cast<std::uint32_t>(adopted.diagnostic.phase);
        if (all_fixed_descriptors_are_closed() && descriptor_is_closed(STDIN_FILENO)) {
            report.flags |= WRITER_FLAG_FIXED_FDS_CLOSED;
        }
        if (!adopted) {
            (void)::close(root_descriptor);
            (void)write_exact(STDOUT_FILENO, &report, sizeof(report));
            return 0;
        }
        report.flags |= WRITER_FLAG_ENTRY_ADOPTED;

        ReplacementHookContext replacement;
        configure_writer_replacement(scenario, *names, replacement);
        replacement.root_descriptor = root_descriptor;
        ConstructionFailureHookContext construction{
            .replace_index = scenario == WriterChildScenario::construction_foreign_index_residue,
            .root_descriptor = root_descriptor,
            .private_directory_leaf = names->private_directory_leaf,
        };

        if (scenario == WriterChildScenario::wrong_base_path_digest) {
            report.flags |= WRITER_FLAG_MUTATION_INVOKED;
            report.mutation_error =
                rewrite_private_lease_with_wrong_base_path_digest(root_descriptor, *names);
            if (report.mutation_error == 0) {
                report.flags |= WRITER_FLAG_MUTATION_SUCCEEDED;
            }
        } else if (scenario == WriterChildScenario::foreign_staging_residue) {
            report.flags |= WRITER_FLAG_MUTATION_INVOKED;
            std::string staging_leaf;
            report.mutation_error =
                create_foreign_staging_residue(root_descriptor, *names, staging_leaf);
            if (report.mutation_error == 0) {
                report.flags |= WRITER_FLAG_MUTATION_SUCCEEDED;
            }
        }

        entry::trusted_test::DistributedSieveWorkerWriterTestHooksV1 conversion_hooks;
        if (!replacement.leaf.empty()) {
            conversion_hooks.after_first_validation = replace_after_first_validation;
            conversion_hooks.context = &replacement;
        }
        if (is_construction_failure(scenario)) {
            conversion_hooks.private_lease_hooks = {
                .stop_after = stop_after_fresh_index_reserved,
                .context = &construction,
            };
        }
        const bool use_conversion_hooks =
            !replacement.leaf.empty() || is_construction_failure(scenario);
        WriterResult converted =
            use_conversion_hooks
                ? entry::trusted_test::consume_distributed_sieve_worker_writer_v1_with_hooks(
                      std::move(*adopted.entry), conversion_hooks)
                : entry::consume_distributed_sieve_worker_writer_v1(std::move(*adopted.entry));

        if (!replacement.leaf.empty()) {
            report.flags |= WRITER_FLAG_MUTATION_INVOKED;
            report.mutation_error = replacement.error;
            if (replacement.invoked && replacement.replaced && replacement.error == 0) {
                report.flags |= WRITER_FLAG_MUTATION_SUCCEEDED;
            }
        }
        if (is_construction_failure(scenario)) {
            report.flags |= WRITER_FLAG_MUTATION_INVOKED;
            report.mutation_error = construction.error;
            if (construction.invoked) {
                report.flags |= WRITER_FLAG_CONSTRUCTION_HOOK_INVOKED;
            }
            if (construction.invoked && construction.error == 0 &&
                (!construction.replace_index || construction.replaced)) {
                report.flags |= WRITER_FLAG_MUTATION_SUCCEEDED;
            }
        }
        if (!is_post_authority_drift(scenario)) {
            if (::close(root_descriptor) != 0 && report.mutation_error == 0) {
                report.mutation_error = errno;
            }
            root_descriptor = -1;
        }

        report.writer_status = static_cast<std::uint32_t>(converted.diagnostic.status);
        report.writer_phase = static_cast<std::uint32_t>(converted.diagnostic.phase);
        report.writer_rollback = static_cast<std::uint32_t>(converted.diagnostic.rollback);
        report.writer_native_error = converted.diagnostic.native_error;
        report.rollback_native_error = converted.diagnostic.rollback_native_error;
        if (converted.diagnostic.reconciliation_required()) {
            report.flags |= WRITER_FLAG_RECONCILIATION_REQUIRED;
        }

        const auto second =
            entry::consume_distributed_sieve_worker_writer_v1(std::move(*adopted.entry));
        report.second_status = static_cast<std::uint32_t>(second.diagnostic.status);
        report.second_phase = static_cast<std::uint32_t>(second.diagnostic.phase);
        if (!second && second.diagnostic.status == WriterStatus::already_consumed &&
            second.diagnostic.phase == WriterPhase::single_use_gate) {
            report.flags |= WRITER_FLAG_SECOND_CONSUME_REJECTED;
        }

        if (converted) {
            report.flags |= WRITER_FLAG_AUTHORITY_READY;
            auto& authority = *converted.writer;
            report.chunk_id = authority.record().chunk_id;
            report.attempt_ordinal = authority.record().attempt_ordinal;
            report.attempt_digest = authority.record().self_digest;
            report.manifest_digest = authority.manifest().self_digest;
            const auto digest = sieve::distributed_sieve_work_digest(authority.identity());
            if (digest && digest.digest.has_value()) {
                report.work_digest = *digest.digest;
            }
            if (authority.valid() && !authority.finalized() && authority.count() == 0 &&
                authority.record().manifest_digest == authority.manifest().self_digest &&
                authority.chunk().chunk_id == authority.record().chunk_id &&
                authority.chunk().sq_begin == authority.record().sq_begin &&
                authority.chunk().sq_end == authority.record().sq_end && digest &&
                digest.digest.has_value() && *digest.digest == authority.manifest().work_sha256 &&
                authority.witness().work_sha256 == authority.manifest().work_sha256) {
                report.flags |= WRITER_FLAG_BINDINGS_VALID;
            }

            const Relation first_relation = make_writer_relation(31, 7, 10);
            const Relation second_relation = make_writer_relation(32, 8, 11);
            if (is_post_authority_drift(scenario)) {
                ReplacementHookContext post_authority;
                configure_post_authority_replacement(scenario, *names, post_authority);
                post_authority.root_descriptor = root_descriptor;
                replace_after_first_validation(&post_authority);
                report.flags |= WRITER_FLAG_MUTATION_INVOKED;
                report.mutation_error = post_authority.error;
                if (post_authority.invoked && post_authority.replaced &&
                    post_authority.error == 0) {
                    report.flags |= WRITER_FLAG_MUTATION_SUCCEEDED;
                }

                if (scenario == WriterChildScenario::post_authority_private_directory_replacement) {
                    if (rejects_writer_runtime_failure([&] { authority.finalize(); })) {
                        report.flags |= WRITER_FLAG_POST_AUTHORITY_FINALIZE_REJECTED;
                    }
                    if (!authority.valid() && !authority.finalized() && authority.count() == 0) {
                        report.flags |= WRITER_FLAG_POST_AUTHORITY_INVALIDATED;
                    }
                    if (rejects_writer_mutation([&] { (void)authority.write(first_relation); })) {
                        report.flags |= WRITER_FLAG_POST_AUTHORITY_WRITE_REJECTED;
                    }
                } else {
                    if (rejects_writer_runtime_failure(
                            [&] { (void)authority.write(first_relation); })) {
                        report.flags |= WRITER_FLAG_POST_AUTHORITY_WRITE_REJECTED;
                    }
                    if (!authority.valid() && !authority.finalized() && authority.count() == 0) {
                        report.flags |= WRITER_FLAG_POST_AUTHORITY_INVALIDATED;
                    }
                    if (rejects_writer_mutation([&] { authority.finalize(); })) {
                        report.flags |= WRITER_FLAG_POST_AUTHORITY_FINALIZE_REJECTED;
                    }
                }
            } else if (scenario == WriterChildScenario::happy) {
                // Keep one relation buffered across fork. A child-side normal
                // return must purge, rather than flush, this inherited 1 MB
                // stdio buffer before the parent appends the second relation.
                const bool first_written =
                    authority.write(first_relation) == 0 && authority.count() == 1;
                const pid_t forked = ::fork();
                if (forked == 0) {
                    return fork_inherited_authority_probe(converted, first_relation);
                }
                if (forked < 0) {
                    report.mutation_error = errno;
                } else {
                    int status = 0;
                    pid_t waited = -1;
                    do {
                        waited = ::waitpid(forked, &status, 0);
                    } while (waited < 0 && errno == EINTR);
                    if (waited == forked && WIFEXITED(status) &&
                        WEXITSTATUS(status) == EXIT_SUCCESS) {
                        report.flags |= WRITER_FLAG_FORK_WRITE_REJECTED |
                                        WRITER_FLAG_FORK_FINALIZE_REJECTED |
                                        WRITER_FLAG_FORK_NORMAL_RETURN;
                    }
                }

                if (first_written && authority.valid() && !authority.finalized() &&
                    authority.count() == 1) {
                    report.flags |= WRITER_FLAG_PARENT_UNCHANGED_AFTER_FORK;
                }
                if (first_written && authority.write(second_relation) == 1 &&
                    authority.count() == 2) {
                    report.flags |= WRITER_FLAG_WROTE_EXACT_PAIR;
                    report.count_after_write = static_cast<std::uint64_t>(authority.count());
                }
                authority.finalize();
                if (authority.valid() && authority.finalized() && authority.count() == 2) {
                    report.flags |= WRITER_FLAG_FINALIZED;
                }
                const bool write_rejected =
                    rejects_writer_mutation([&] { (void)authority.write(first_relation); });
                const bool finalize_rejected =
                    rejects_writer_mutation([&] { authority.finalize(); });
                if (write_rejected && finalize_rejected) {
                    report.flags |= WRITER_FLAG_POST_FINALIZE_REJECTED;
                }
            }
        }

        if (root_descriptor >= 0) {
            if (::close(root_descriptor) != 0 && report.mutation_error == 0) {
                report.mutation_error = errno;
            }
            root_descriptor = -1;
        }
        if (!write_exact(STDOUT_FILENO, &report, sizeof(report))) {
            return 92;
        }
        return 0;
    } catch (...) {
        (void)write_exact(STDOUT_FILENO, &report, sizeof(report));
        return 93;
    }
}

[[nodiscard]] WriterChildReport read_writer_child_report(int descriptor) {
    WriterChildReport report;
    CHECK(read_exact(descriptor, &report, sizeof(report)));
    std::byte trailing{};
    ssize_t received = -1;
    do {
        received = ::read(descriptor, &trailing, 1);
    } while (received < 0 && errno == EINTR);
    CHECK(received == 0);
    CHECK(report.magic == WRITER_CHILD_REPORT_MAGIC);
    return report;
}

#endif

struct LaunchedWriterCaseResult final {
    WriterChildReport report;
    sieve::AttemptStartedV1 record;
#if !defined(_WIN32)
    NamespaceSnapshot baseline;
#endif
};

[[nodiscard]] LaunchedWriterCaseResult launch_writer_case(WorkerEntryFixture& fixture,
                                                          const std::filesystem::path& executable,
                                                          WriterChildScenario scenario) {
#if defined(_WIN32)
    (void)fixture;
    (void)executable;
    (void)scenario;
    throw TestFailure("worker-writer self-exec fixture is unavailable on Windows");
#else
    auto receipt = fixture.start_receipt();
    const auto record = receipt.record();
    auto baseline = snapshot_namespace(fixture.root);
    std::vector<launcher::DistributedSieveWorkerLaunchSlotV1> slots;
    slots.emplace_back(std::move(receipt), std::vector<std::string>{
                                               std::string(WRITER_CHILD_ARGUMENT),
                                               std::to_string(static_cast<std::uint32_t>(scenario)),
                                           });
    launcher::DistributedSieveWorkerLaunchRequestV1 request(executable.string(), std::move(slots));
    auto launched = fixture.store().launch_worker_process_batch_v1(
        std::move(request), fixture.identity, fixture.frozen, fixture.polynomial,
        fixture.factor_base);
    CHECK(launched);
    CHECK(launched.children.size() == 1);
    CHECK(launched.children[0]);
    CHECK(launched.children[0].worker.has_value());
    auto& worker = *launched.children[0].worker;
    const auto waited = worker.wait_terminal();
    CHECK(waited.reaped);
    CHECK(waited.success);
    CHECK(waited.exit_status == 0);
    const int report_descriptor = worker.release_report_descriptor();
    CHECK(report_descriptor >= 0);
    const auto report = read_writer_child_report(report_descriptor);
    CHECK(::close(report_descriptor) == 0);
    CHECK(report.scenario == static_cast<std::uint32_t>(scenario));
    return {
        .report = report,
        .record = record,
        .baseline = std::move(baseline),
    };
#endif
}

void require_writer_entry_was_adopted(const WriterChildReport& report) {
    CHECK((report.flags & WRITER_FLAG_ENTRY_ADOPTED) != 0U);
    CHECK((report.flags & WRITER_FLAG_FIXED_FDS_CLOSED) != 0U);
    CHECK(report.entry_status ==
          static_cast<std::uint32_t>(entry::DistributedSieveWorkerEntryStatusV1::ready));
}

void require_writer_rejection(const WriterChildReport& report, WriterPhase phase,
                              WriterStatus status) {
    require_writer_entry_was_adopted(report);
    CHECK((report.flags & WRITER_FLAG_MUTATION_INVOKED) != 0U);
    CHECK((report.flags & WRITER_FLAG_MUTATION_SUCCEEDED) != 0U);
    CHECK((report.flags & WRITER_FLAG_AUTHORITY_READY) == 0U);
    CHECK((report.flags & WRITER_FLAG_SECOND_CONSUME_REJECTED) != 0U);
    CHECK(report.mutation_error == 0);
    CHECK(report.writer_phase == static_cast<std::uint32_t>(phase));
    CHECK(report.writer_status == static_cast<std::uint32_t>(status));
    CHECK(report.second_phase == static_cast<std::uint32_t>(WriterPhase::single_use_gate));
    CHECK(report.second_status == static_cast<std::uint32_t>(WriterStatus::already_consumed));
}

void require_post_authority_drift_rejection(const WriterChildReport& report) {
    require_writer_entry_was_adopted(report);
    CHECK((report.flags & WRITER_FLAG_AUTHORITY_READY) != 0U);
    CHECK((report.flags & WRITER_FLAG_BINDINGS_VALID) != 0U);
    CHECK((report.flags & WRITER_FLAG_SECOND_CONSUME_REJECTED) != 0U);
    CHECK((report.flags & WRITER_FLAG_MUTATION_INVOKED) != 0U);
    CHECK((report.flags & WRITER_FLAG_MUTATION_SUCCEEDED) != 0U);
    CHECK((report.flags & WRITER_FLAG_POST_AUTHORITY_WRITE_REJECTED) != 0U);
    CHECK((report.flags & WRITER_FLAG_POST_AUTHORITY_INVALIDATED) != 0U);
    CHECK((report.flags & WRITER_FLAG_POST_AUTHORITY_FINALIZE_REJECTED) != 0U);
    CHECK((report.flags & WRITER_FLAG_FINALIZED) == 0U);
    CHECK((report.flags & WRITER_FLAG_RECONCILIATION_REQUIRED) == 0U);
    CHECK(report.mutation_error == 0);
    CHECK(report.writer_phase == static_cast<std::uint32_t>(WriterPhase::writer_creation));
    CHECK(report.writer_status == static_cast<std::uint32_t>(WriterStatus::ready));
    CHECK(report.writer_rollback == static_cast<std::uint32_t>(WriterRollback::not_applicable));
    CHECK(report.count_after_write == 0);
}

void require_construction_failure(const WriterChildReport& report, WriterRollback rollback,
                                  bool reconciliation_required) {
    require_writer_entry_was_adopted(report);
    CHECK((report.flags & WRITER_FLAG_AUTHORITY_READY) == 0U);
    CHECK((report.flags & WRITER_FLAG_SECOND_CONSUME_REJECTED) != 0U);
    CHECK((report.flags & WRITER_FLAG_MUTATION_INVOKED) != 0U);
    CHECK((report.flags & WRITER_FLAG_MUTATION_SUCCEEDED) != 0U);
    CHECK((report.flags & WRITER_FLAG_CONSTRUCTION_HOOK_INVOKED) != 0U);
    CHECK(((report.flags & WRITER_FLAG_RECONCILIATION_REQUIRED) != 0U) == reconciliation_required);
    CHECK(report.mutation_error == 0);
    CHECK(report.writer_phase == static_cast<std::uint32_t>(WriterPhase::writer_creation));
    CHECK(report.writer_status == static_cast<std::uint32_t>(WriterStatus::writer_failed));
    CHECK(report.writer_rollback == static_cast<std::uint32_t>(rollback));
    CHECK(report.writer_native_error == ECANCELED);
    CHECK(report.second_phase == static_cast<std::uint32_t>(WriterPhase::single_use_gate));
    CHECK(report.second_status == static_cast<std::uint32_t>(WriterStatus::already_consumed));
}

void test_writer_authority_happy_path(const std::filesystem::path& executable) {
    WorkerEntryFixture fixture("writer-happy");
    const auto result = launch_writer_case(fixture, executable, WriterChildScenario::happy);
    require_writer_entry_was_adopted(result.report);
    CHECK(result.report.writer_phase == static_cast<std::uint32_t>(WriterPhase::writer_creation));
    CHECK(result.report.writer_status == static_cast<std::uint32_t>(WriterStatus::ready));
    CHECK((result.report.flags & WRITER_HAPPY_FLAGS) == WRITER_HAPPY_FLAGS);
    CHECK(result.report.count_after_write == 2);
    CHECK(result.report.chunk_id == 0);
    CHECK(result.report.attempt_ordinal == 0);
    CHECK(result.report.attempt_digest == result.record.self_digest);
    CHECK(result.report.manifest_digest == fixture.store().manifest_digest());
    CHECK(result.report.work_digest == work_digest(fixture.identity));

    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    const auto base = fixture.root / names->private_directory_leaf / "corpus";
    gnfs::relation::OOCRelationReader reader(base.string());
    CHECK(reader.count() == 2);
    CHECK(writer_relation_equal(reader.read(0), make_writer_relation(31, 7, 10)));
    CHECK(writer_relation_equal(reader.read(1), make_writer_relation(32, 8, 11)));
    require_exact_happy_namespace_delta(fixture, result.baseline);
    require_no_handoff_or_cleanup_publication(fixture);
}

void test_writer_replacement_sandwiches(const std::filesystem::path& executable) {
    struct Case final {
        WriterChildScenario scenario;
        WriterStatus status;
        std::string_view displaced_leaf;
    };
    constexpr std::array cases{
        Case{
            WriterChildScenario::reserved_replacement_sandwich,
            WriterStatus::private_lease_invalid,
            ".gnfs-test-writer-displaced-reserved",
        },
        Case{
            WriterChildScenario::private_directory_replacement_sandwich,
            WriterStatus::private_lease_invalid,
            ".gnfs-test-writer-displaced-private-directory",
        },
        Case{
            WriterChildScenario::base_lock_replacement_sandwich,
            WriterStatus::lock_invalid,
            ".gnfs-test-writer-displaced-base-lock",
        },
    };

    for (const auto& test_case : cases) {
        WorkerEntryFixture fixture("writer-sandwich-" +
                                   std::to_string(static_cast<std::uint32_t>(test_case.scenario)));
        const auto result = launch_writer_case(fixture, executable, test_case.scenario);
        require_writer_rejection(result.report, WriterPhase::final_revalidation, test_case.status);
        CHECK(std::filesystem::exists(fixture.root / test_case.displaced_leaf));

        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        switch (test_case.scenario) {
        case WriterChildScenario::reserved_replacement_sandwich:
            CHECK(std::filesystem::is_regular_file(fixture.root / names->reserved_leaf));
            break;
        case WriterChildScenario::private_directory_replacement_sandwich:
            CHECK(std::filesystem::is_directory(fixture.root / names->private_directory_leaf));
            CHECK(std::filesystem::is_empty(fixture.root / names->private_directory_leaf));
            CHECK(std::filesystem::exists(
                fixture.root / test_case.displaced_leaf /
                std::string(wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF)));
            break;
        case WriterChildScenario::base_lock_replacement_sandwich:
            CHECK(std::filesystem::is_regular_file(fixture.root / names->base_lock_leaf));
            break;
        default:
            fail("writer replacement scenario", __LINE__);
        }
        require_no_worker_outputs(fixture);
        require_no_handoff_or_cleanup_publication(fixture);
    }
}

void test_writer_wrong_consistent_base_digest(const std::filesystem::path& executable) {
    WorkerEntryFixture fixture("writer-wrong-base-digest");
    const auto result =
        launch_writer_case(fixture, executable, WriterChildScenario::wrong_base_path_digest);
    require_writer_rejection(result.report, WriterPhase::entry_revalidation,
                             WriterStatus::private_lease_invalid);

    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    CHECK(std::filesystem::exists(fixture.root / names->reserved_leaf));
    CHECK(std::filesystem::exists(fixture.root / names->owned_leaf));
    CHECK(std::filesystem::exists(fixture.root / names->private_directory_leaf /
                                  std::string(wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF)));
    require_no_worker_outputs(fixture);
    require_no_handoff_or_cleanup_publication(fixture);
}

void test_writer_foreign_staging_residue(const std::filesystem::path& executable) {
    WorkerEntryFixture fixture("writer-foreign-staging");
    const auto result =
        launch_writer_case(fixture, executable, WriterChildScenario::foreign_staging_residue);
    require_writer_rejection(result.report, WriterPhase::entry_revalidation,
                             WriterStatus::private_lease_invalid);

    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    const auto staging = foreign_staging_leaf_for(*names, result.record);
    CHECK(std::filesystem::is_directory(fixture.root / staging));
    require_no_worker_outputs(fixture);
    require_no_handoff_or_cleanup_publication(fixture);
}

void test_post_authority_namespace_drift(const std::filesystem::path& executable) {
    enum class Target : std::uint8_t {
        base_lock,
        reserved,
        private_directory,
    };
    struct Case final {
        WriterChildScenario scenario;
        std::string_view displaced_leaf;
        Target target = Target::base_lock;
    };
    constexpr std::array cases{
        Case{
            WriterChildScenario::post_authority_base_lock_replacement,
            ".gnfs-test-writer-post-displaced-base-lock",
            Target::base_lock,
        },
        Case{
            WriterChildScenario::post_authority_reserved_replacement,
            ".gnfs-test-writer-post-displaced-reserved",
            Target::reserved,
        },
        Case{
            WriterChildScenario::post_authority_private_directory_replacement,
            ".gnfs-test-writer-post-displaced-private-directory",
            Target::private_directory,
        },
    };

    for (const auto& test_case : cases) {
        WorkerEntryFixture fixture("writer-post-authority-" +
                                   std::to_string(static_cast<std::uint32_t>(test_case.scenario)));
        const auto result = launch_writer_case(fixture, executable, test_case.scenario);
        require_post_authority_drift_rejection(result.report);

        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        const auto displaced = fixture.root / test_case.displaced_leaf;
        if (test_case.target == Target::private_directory) {
            const auto named = fixture.root / names->private_directory_leaf;
            require_distinct_owner_directories(named, displaced);
            CHECK(std::filesystem::is_empty(named));
            CHECK(std::filesystem::is_regular_file(
                displaced / std::string(wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF)));
            require_unfinalized_corpus(displaced / "corpus");
        } else {
            const auto canonical =
                fixture.root / (test_case.target == Target::base_lock ? names->base_lock_leaf
                                                                      : names->reserved_leaf);
            require_distinct_regular_inodes(canonical, displaced);
            const auto base = fixture.root / names->private_directory_leaf / "corpus";
            require_unfinalized_corpus(base);
        }
        require_no_handoff_or_cleanup_publication(fixture);
    }
}

void test_exact_writer_construction_rollback_diagnostics(const std::filesystem::path& executable) {
    {
        WorkerEntryFixture fixture("writer-construction-clean");
        const auto result = launch_writer_case(fixture, executable,
                                               WriterChildScenario::construction_clean_rollback);
        require_construction_failure(result.report, WriterRollback::clean, false);
        CHECK(result.report.rollback_native_error == 0);

        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        const auto base = fixture.root / names->private_directory_leaf / "corpus";
        CHECK(!std::filesystem::exists(base.string() + ".relidx"));
        CHECK(!std::filesystem::exists(base.string() + ".reldata"));
        require_no_worker_outputs(fixture);
        require_no_handoff_or_cleanup_publication(fixture);
    }

    {
        WorkerEntryFixture fixture("writer-construction-foreign-residue");
        const auto result = launch_writer_case(
            fixture, executable, WriterChildScenario::construction_foreign_index_residue);
        require_construction_failure(result.report, WriterRollback::named_residue_may_remain, true);
        CHECK(result.report.rollback_native_error != 0);

        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        const auto directory = fixture.root / names->private_directory_leaf;
        const auto foreign_index = directory / "corpus.relidx";
        const auto displaced_owned = directory / ".gnfs-test-writer-owned-index-displaced";
        require_distinct_regular_inodes(foreign_index, displaced_owned);
        CHECK(!std::filesystem::exists(directory / "corpus.reldata"));
        require_no_handoff_or_cleanup_publication(fixture);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        (void)argc;
        (void)argv;
        const auto unsupported = entry::adopt_distributed_sieve_worker_entry_v1();
        CHECK(!unsupported);
        CHECK(unsupported.diagnostic.phase ==
              entry::DistributedSieveWorkerEntryPhaseV1::platform_gate);
        CHECK(unsupported.diagnostic.status ==
              entry::DistributedSieveWorkerEntryStatusV1::platform_unsupported);
        std::cout << "distributed sieve worker-writer authority explicitly "
                     "unsupported on this platform\n";
        return 0;
#else
        if (argc == 3 && std::string_view(argv[1]) == WRITER_CHILD_ARGUMENT) {
            const auto scenario = parse_writer_child_scenario(argv[2]);
            return scenario.has_value() ? run_writer_child(*scenario) : 94;
        }

        CHECK(argc >= 1);
        const auto executable = self_executable_path(argv[0]);
        test_writer_authority_happy_path(executable);
        test_writer_replacement_sandwiches(executable);
        test_writer_wrong_consistent_base_digest(executable);
        test_writer_foreign_staging_residue(executable);
        test_post_authority_namespace_drift(executable);
        test_exact_writer_construction_rollback_diagnostics(executable);
        std::cout << "distributed sieve worker-writer authority tests passed\n";
        return 0;
#endif
    } catch (const std::exception& error) {
        std::cerr << "distributed sieve worker-writer authority test failed: " << error.what()
                  << '\n';
        return 1;
    }
}
