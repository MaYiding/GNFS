// Native boundary tests for the leased SIQS RSS campaign journal store.
//
// The production registry is intentionally empty. POSIX-only fixtures enter
// through the private deployment table so no test path or resolver is added to
// the public authority boundary.

#include <gnfs/siqs/shadow_proof_rss_campaign_journal_codec.hpp>
#include <gnfs/siqs/shadow_proof_rss_campaign_journal_store.hpp>

#include "shadow_proof_rss_campaign_journal_store_internal.hpp"
#include "support/child_process.hpp"

#include <array>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using namespace gnfs::siqs;
using gnfs::util::ProcessMemoryBackend;
namespace store_detail = gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail;

using DeploymentEntry = store_detail::DeploymentEntry;
using LayoutError = SIQSShadowProofRssCampaignJournalLayoutError;
using StoreError = SIQSShadowProofRssCampaignJournalStoreError;
using StoreObject = SIQSShadowProofRssCampaignJournalStoreObject;

using PublicOpenFunction = SIQSShadowProofRssCampaignJournalStoreOpenResult (*)(
    const SIQSShadowProofRssGatePolicy*, const SIQSShadowProofRssCampaignRuntimeFacts*) noexcept;

static_assert(std::same_as<decltype(&open_siqs_shadow_proof_rss_campaign_journal_session),
                           PublicOpenFunction>);
static_assert(!std::is_default_constructible_v<SIQSShadowProofRssCampaignJournalSession>);
static_assert(!std::is_copy_constructible_v<SIQSShadowProofRssCampaignJournalSession>);
static_assert(!std::is_copy_assignable_v<SIQSShadowProofRssCampaignJournalSession>);
static_assert(std::is_nothrow_move_constructible_v<SIQSShadowProofRssCampaignJournalSession>);
static_assert(std::is_nothrow_move_assignable_v<SIQSShadowProofRssCampaignJournalSession>);
static_assert(!std::is_default_constructible_v<SIQSShadowProofRssCampaignJournalStoreOpenResult>);
static_assert(!std::is_copy_constructible_v<SIQSShadowProofRssCampaignJournalStoreOpenResult>);
static_assert(!std::is_copy_assignable_v<SIQSShadowProofRssCampaignJournalStoreOpenResult>);
static_assert(
    std::is_nothrow_move_constructible_v<SIQSShadowProofRssCampaignJournalStoreOpenResult>);
static_assert(std::is_nothrow_move_assignable_v<SIQSShadowProofRssCampaignJournalStoreOpenResult>);

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

[[nodiscard]] constexpr SIQSShadowProofRssOperatingSystem host_operating_system() noexcept {
#if defined(_WIN32)
    return SIQSShadowProofRssOperatingSystem::windows;
#elif defined(__APPLE__)
    return SIQSShadowProofRssOperatingSystem::darwin;
#elif defined(__linux__)
    return SIQSShadowProofRssOperatingSystem::linux;
#else
    return SIQSShadowProofRssOperatingSystem::unknown;
#endif
}

[[nodiscard]] constexpr SIQSShadowProofRssArchitecture host_architecture() noexcept {
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    return SIQSShadowProofRssArchitecture::arm64;
#elif defined(__x86_64__) || defined(_M_X64)
    return SIQSShadowProofRssArchitecture::x86_64;
#else
    return SIQSShadowProofRssArchitecture::unknown;
#endif
}

[[nodiscard]] constexpr ProcessMemoryBackend host_memory_backend() noexcept {
#if defined(_WIN32)
    return ProcessMemoryBackend::WindowsPsapi;
#elif defined(__APPLE__)
    return ProcessMemoryBackend::DarwinGetrusage;
#elif defined(__linux__)
    return ProcessMemoryBackend::LinuxGetrusage;
#else
    return ProcessMemoryBackend::Unsupported;
#endif
}

[[nodiscard]] constexpr SIQSShadowProofRssGatePolicy make_policy() noexcept {
    SIQSShadowProofRssGatePolicy policy;
    policy.approved = true;
    policy.corpus_id = SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_ID;
    policy.corpus_digest = {SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_LOW,
                            SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_HIGH};
    policy.operating_system = host_operating_system();
    policy.architecture = host_architecture();
    policy.memory_backend = host_memory_backend();
    policy.resolved_production_sieve_workers = 4;
    policy.candidate_revision = "candidate-revision-1";
    policy.approval_id = "approval-ticket-1";
    policy.journal_store = {{UINT64_C(1010101010101010), UINT64_C(2020202020202020)},
                            {UINT64_C(1111222233334444), UINT64_C(5555666677778888)},
                            "rss-campaign-prod-v1"};
    policy.deployment_budget_bytes = UINT64_C(1000000000);
    policy.reserved_headroom_bytes = UINT64_C(100000000);
    return policy;
}

[[nodiscard]] constexpr SIQSShadowProofRssCampaignRuntimeFacts make_facts() noexcept {
    return {
        .operating_system = host_operating_system(),
        .architecture = host_architecture(),
        .memory_backend = host_memory_backend(),
        .resolved_production_sieve_workers = 4,
        .candidate_revision = "candidate-revision-1",
        .release_build = true,
        .ndebug = true,
    };
}

[[nodiscard]] DeploymentEntry make_deployment(const std::filesystem::path& trusted_base_path) {
    const auto policy = make_policy();
#ifndef _WIN32
    const uint64_t expected_owner = static_cast<uint64_t>(::geteuid());
#else
    constexpr uint64_t expected_owner = 0;
#endif
    return {
        .trusted_base_id = policy.journal_store.trusted_base_id,
        .store_id = policy.journal_store.store_id,
        .relative_locator = std::string(policy.journal_store.relative_locator),
        .trusted_base_path = trusted_base_path,
        .expected_owner = expected_owner,
    };
}

[[nodiscard]] SIQSShadowProofRssCampaignJournalStoreOpenResult
open_private(const SIQSShadowProofRssGatePolicy* policy,
             const SIQSShadowProofRssCampaignRuntimeFacts* facts,
             const DeploymentEntry& deployment) {
    return store_detail::SessionFactory::open_with_deployments(
        policy, facts, std::span<const DeploymentEntry>(&deployment, 1));
}

void expect_open_error(SIQSShadowProofRssCampaignJournalStoreOpenResult result, StoreError error,
                       StoreObject object) {
    CHECK(!static_cast<bool>(result));
    if (result.diagnostic().error != error || result.diagnostic().object != object) {
        std::cerr
            << "expected store error "
            << siqs_shadow_proof_rss_campaign_journal_store_error_name(error) << '/'
            << siqs_shadow_proof_rss_campaign_journal_store_object_name(object) << ", got "
            << siqs_shadow_proof_rss_campaign_journal_store_error_name(result.diagnostic().error)
            << '/'
            << siqs_shadow_proof_rss_campaign_journal_store_object_name(result.diagnostic().object)
            << " native=" << result.diagnostic().native_error.value() << '\n';
    }
    CHECK(result.diagnostic().error == error);
    CHECK(result.diagnostic().object == object);
    CHECK(!std::move(result).take_session().has_value());
    CHECK(!std::move(result).take_session().has_value());
}

[[maybe_unused]] void expect_layout_error(
    SIQSShadowProofRssCampaignJournalStoreOpenResult result, LayoutError layout_error,
    StoreObject object = StoreObject::directory,
    uint32_t record_sequence = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE) {
    CHECK(!static_cast<bool>(result));
    CHECK(result.diagnostic().error == StoreError::layout_invalid);
    CHECK(result.diagnostic().object == object);
    CHECK(result.diagnostic().layout.layout_error == layout_error);
    CHECK(result.diagnostic().layout.record_sequence == record_sequence);
    CHECK(!std::move(result).take_session().has_value());
}

[[maybe_unused, nodiscard]] std::optional<SIQSShadowProofRssCampaignJournalSession>
take_successful_session(SIQSShadowProofRssCampaignJournalStoreOpenResult result) {
    CHECK(static_cast<bool>(result));
    CHECK(result.diagnostic().error == StoreError::none);
    CHECK(result.diagnostic().object == StoreObject::none);
    auto session = std::move(result).take_session();
    CHECK(session.has_value());
    CHECK(session->active());
    return session;
}

void test_public_authority_and_preflight_boundaries() {
    const auto policy = make_policy();
    const auto facts = make_facts();

    auto production = open_siqs_shadow_proof_rss_campaign_journal_session(&policy, &facts);
    expect_open_error(std::move(production), StoreError::binding_not_registered,
                      StoreObject::deployment_registry);

    auto not_registered_entry = make_deployment("unused");
    ++not_registered_entry.trusted_base_id.low;
    auto not_registered = store_detail::SessionFactory::open_with_deployments(
        &policy, &facts, {&not_registered_entry, 1});
    expect_open_error(std::move(not_registered), StoreError::binding_not_registered,
                      StoreObject::deployment_registry);

    std::array<DeploymentEntry, 2> ambiguous_entries{
        make_deployment("unused-first"),
        make_deployment("unused-second"),
    };
    auto ambiguous =
        store_detail::SessionFactory::open_with_deployments(&policy, &facts, ambiguous_entries);
    expect_open_error(std::move(ambiguous), StoreError::binding_ambiguous,
                      StoreObject::deployment_registry);

    auto mismatched_store = make_deployment("unused");
    ++mismatched_store.store_id.high;
    auto mismatch = store_detail::SessionFactory::open_with_deployments(&policy, &facts,
                                                                        {&mismatched_store, 1});
    expect_open_error(std::move(mismatch), StoreError::registry_binding_mismatch,
                      StoreObject::deployment_registry);

    DeploymentEntry poisonous = make_deployment({});
    std::array preflight_poison{poisonous, poisonous};

    auto missing_policy =
        store_detail::SessionFactory::open_with_deployments(nullptr, &facts, preflight_poison);
    CHECK(missing_policy.diagnostic().journal_reason ==
          SIQSShadowProofRssJournalReason::policy_missing);
    expect_open_error(std::move(missing_policy), StoreError::preflight_rejected, StoreObject::none);

    auto unapproved_policy = policy;
    unapproved_policy.approved = false;
    auto unapproved = store_detail::SessionFactory::open_with_deployments(&unapproved_policy,
                                                                          &facts, preflight_poison);
    CHECK(unapproved.diagnostic().journal_reason ==
          SIQSShadowProofRssJournalReason::policy_not_approved);
    expect_open_error(std::move(unapproved), StoreError::preflight_rejected, StoreObject::none);

    auto invalid_facts = facts;
    invalid_facts.release_build = false;
    auto invalid = store_detail::SessionFactory::open_with_deployments(&policy, &invalid_facts,
                                                                       preflight_poison);
    CHECK(invalid.diagnostic().journal_reason ==
          SIQSShadowProofRssJournalReason::release_ndebug_required);
    expect_open_error(std::move(invalid), StoreError::preflight_rejected, StoreObject::none);
}

void test_diagnostic_name_contracts() {
    constexpr std::array known_errors{
        StoreError::none,
        StoreError::preflight_rejected,
        StoreError::platform_unavailable,
        StoreError::binding_not_registered,
        StoreError::binding_ambiguous,
        StoreError::registry_binding_mismatch,
        StoreError::base_open_failed,
        StoreError::base_invalid,
        StoreError::root_open_failed,
        StoreError::root_invalid,
        StoreError::lock_open_failed,
        StoreError::lock_invalid,
        StoreError::lock_busy,
        StoreError::lock_failed,
        StoreError::directory_open_failed,
        StoreError::directory_read_failed,
        StoreError::entry_metadata_failed,
        StoreError::entry_trust_invalid,
        StoreError::entry_open_failed,
        StoreError::entry_read_failed,
        StoreError::entry_identity_mismatch,
        StoreError::entry_changed_during_read,
        StoreError::snapshot_changed,
        StoreError::layout_invalid,
        StoreError::replay_rejected,
        StoreError::resource_exhausted,
        StoreError::unexpected_failure,
    };
    for (const auto error : known_errors) {
        const auto name = siqs_shadow_proof_rss_campaign_journal_store_error_name(error);
        CHECK(!name.empty());
        CHECK(name != "unknown");
    }
    CHECK(siqs_shadow_proof_rss_campaign_journal_store_error_name(
              static_cast<StoreError>(UINT8_C(255))) == "unknown");

    constexpr std::array known_objects{
        StoreObject::none,           StoreObject::deployment_registry, StoreObject::trusted_base,
        StoreObject::store_root,     StoreObject::session_lock,        StoreObject::directory,
        StoreObject::journal_header, StoreObject::journal_record,
    };
    for (const auto object : known_objects) {
        const auto name = siqs_shadow_proof_rss_campaign_journal_store_object_name(object);
        CHECK(!name.empty());
        CHECK(name != "unknown");
    }
    CHECK(siqs_shadow_proof_rss_campaign_journal_store_object_name(
              static_cast<StoreObject>(UINT8_C(255))) == "unknown");
}

#ifndef _WIN32

class TempStore final {
public:
    TempStore() {
        // Darwin's per-user temporary root has trusted ancestors; Linux /tmp
        // does not. Project runners and CTest use a trusted working directory
        // on non-Darwin POSIX hosts.
#if defined(__APPLE__)
        const auto temporary_root = std::filesystem::temp_directory_path();
#else
        const auto temporary_root = std::filesystem::current_path();
#endif
        std::string pattern = (temporary_root / ".gnfs-rss-journal-store-XXXXXX").string();
        std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
        mutable_pattern.push_back('\0');
        char* created = ::mkdtemp(mutable_pattern.data());
        if (created == nullptr) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp");
        }
        created_path_ = std::filesystem::path(created);

        std::error_code canonical_error;
        trusted_base_ = std::filesystem::weakly_canonical(created_path_, canonical_error);
        if (canonical_error) {
            throw std::system_error(canonical_error, "weakly_canonical");
        }
        if (::chmod(trusted_base_.c_str(), 0700) != 0) {
            throw std::system_error(errno, std::generic_category(), "chmod trusted base");
        }

        store_root_ = trusted_base_ / std::string(make_policy().journal_store.relative_locator);
        if (::mkdir(store_root_.c_str(), 0700) != 0) {
            throw std::system_error(errno, std::generic_category(), "mkdir store root");
        }
    }

    ~TempStore() {
        std::error_code ignored;
        if (!created_path_.empty()) {
            std::filesystem::remove_all(created_path_, ignored);
        }
    }

    TempStore(const TempStore&) = delete;
    TempStore& operator=(const TempStore&) = delete;
    TempStore(TempStore&&) = delete;
    TempStore& operator=(TempStore&&) = delete;

    [[nodiscard]] const std::filesystem::path& trusted_base() const noexcept {
        return trusted_base_;
    }

    [[nodiscard]] const std::filesystem::path& store_root() const noexcept {
        return store_root_;
    }

    [[nodiscard]] std::filesystem::path store_leaf(std::string_view leaf) const {
        return store_root_ / std::string(leaf);
    }

    [[nodiscard]] std::filesystem::path base_leaf(std::string_view leaf) const {
        return trusted_base_ / std::string(leaf);
    }

    void write_store_leaf(std::string_view leaf, std::span<const std::byte> bytes) const {
        write_file(store_leaf(leaf), bytes);
    }

    void write_base_leaf(std::string_view leaf, std::span<const std::byte> bytes) const {
        write_file(base_leaf(leaf), bytes);
    }

private:
    static void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
        const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (descriptor < 0) {
            throw std::system_error(errno, std::generic_category(), "open fixture file");
        }

        std::size_t written = 0;
        while (written < bytes.size()) {
            const auto count = ::write(descriptor, bytes.data() + written, bytes.size() - written);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                const int saved_errno = count < 0 ? errno : EIO;
                ::close(descriptor);
                throw std::system_error(saved_errno, std::generic_category(), "write fixture file");
            }
            written += static_cast<std::size_t>(count);
        }
        if (::fsync(descriptor) != 0) {
            const int saved_errno = errno;
            ::close(descriptor);
            throw std::system_error(saved_errno, std::generic_category(), "fsync fixture file");
        }
        if (::close(descriptor) != 0) {
            throw std::system_error(errno, std::generic_category(), "close fixture file");
        }
    }

    std::filesystem::path created_path_;
    std::filesystem::path trusted_base_;
    std::filesystem::path store_root_;
};

template <std::size_t Size>
[[nodiscard]] constexpr std::span<const std::byte>
byte_span(const std::array<std::byte, Size>& bytes) noexcept {
    return {bytes.data(), bytes.size()};
}

[[nodiscard]] SIQSShadowProofRssCampaignJournalHeader
write_canonical_header(const TempStore& fixture) {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto resume = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::absent, nullptr, {});
    CHECK(resume.status == SIQSShadowProofRssJournalStatus::ready);
    CHECK(resume.action == SIQSShadowProofRssJournalAction::create_header);
    CHECK(resume.header_to_create.has_value());
    const auto encoded =
        encode_siqs_shadow_proof_rss_campaign_journal_header(*resume.header_to_create);
    CHECK(encoded.bytes.has_value());
    fixture.write_store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF,
                             byte_span(*encoded.bytes));
    return *resume.header_to_create;
}

[[nodiscard]] SIQSShadowProofRssCampaignJournalRecord
canonical_start(const SIQSShadowProofRssCampaignJournalHeader& header) {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto resume = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, {});
    CHECK(resume.status == SIQSShadowProofRssJournalStatus::ready);
    CHECK(resume.action == SIQSShadowProofRssJournalAction::append_slot_start);
    CHECK(resume.prepared_slot_start.has_value());
    return resume.prepared_slot_start->record();
}

void write_record(const TempStore& fixture, uint32_t leaf_sequence,
                  const SIQSShadowProofRssCampaignJournalRecord& record) {
    const auto leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(leaf_sequence);
    CHECK(leaf.has_value());
    const auto encoded = encode_siqs_shadow_proof_rss_campaign_journal_record(record);
    CHECK(encoded.bytes.has_value());
    fixture.write_store_leaf(leaf->view(), byte_span(*encoded.bytes));
}

[[nodiscard]] SIQSShadowProofRssCampaignJournalStoreOpenResult
open_fixture(const TempStore& fixture) {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto deployment = make_deployment(fixture.trusted_base());
    return open_private(&policy, &facts, deployment);
}

[[nodiscard]] SIQSShadowProofRssCampaignJournalStoreOpenResult
open_with_base_path(const std::filesystem::path& trusted_base) {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto deployment = make_deployment(trusted_base);
    return open_private(&policy, &facts, deployment);
}

void test_trusted_base_component_walk_is_fail_closed() {
    TempStore fixture;
    const std::string canonical = fixture.trusted_base().string();
    CHECK(!canonical.empty());
    CHECK(canonical.front() == '/');

    for (const std::filesystem::path& invalid :
         {std::filesystem::path("relative/base"), std::filesystem::path("/." + canonical),
          std::filesystem::path("/.." + canonical), std::filesystem::path("/" + canonical),
          std::filesystem::path(canonical + "/")}) {
        expect_open_error(open_with_base_path(invalid), StoreError::base_invalid,
                          StoreObject::trusted_base);
    }

    const auto target = fixture.base_leaf("base-target");
    const auto nested = target / "nested";
    CHECK(::mkdir(target.c_str(), 0700) == 0);
    CHECK(::mkdir(nested.c_str(), 0700) == 0);
    const auto intermediate_link = fixture.base_leaf("base-link");
    CHECK(::symlink(target.c_str(), intermediate_link.c_str()) == 0);
    expect_open_error(open_with_base_path(intermediate_link / "nested"),
                      StoreError::base_open_failed, StoreObject::trusted_base);
}

void test_untrusted_owner_and_write_permissions_fail_closed() {
    {
        TempStore fixture;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto deployment = make_deployment(fixture.trusted_base());
        ++deployment.expected_owner;
        expect_open_error(open_private(&policy, &facts, deployment),
                          StoreError::registry_binding_mismatch, StoreObject::deployment_registry);
        CHECK(!std::filesystem::exists(
            fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF)));
    }
    {
        TempStore fixture;
        const auto intermediate = fixture.base_leaf("intermediate");
        const auto nested_base = intermediate / "trusted";
        CHECK(::mkdir(intermediate.c_str(), 0700) == 0);
        CHECK(::mkdir(nested_base.c_str(), 0700) == 0);
        const auto nested_root =
            nested_base / std::string(make_policy().journal_store.relative_locator);
        CHECK(::mkdir(nested_root.c_str(), 0700) == 0);
        CHECK(::chmod(intermediate.c_str(), 0770) == 0);
        expect_open_error(open_with_base_path(nested_base), StoreError::base_invalid,
                          StoreObject::trusted_base);
    }
    {
        TempStore fixture;
        CHECK(::chmod(fixture.trusted_base().c_str(), 0770) == 0);
        expect_open_error(open_fixture(fixture), StoreError::base_invalid,
                          StoreObject::trusted_base);
    }
    {
        TempStore fixture;
        CHECK(::chmod(fixture.store_root().c_str(), 0770) == 0);
        expect_open_error(open_fixture(fixture), StoreError::root_invalid, StoreObject::store_root);
    }
    {
        TempStore fixture;
        fixture.write_store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF, {});
        const auto lock =
            fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF);
        CHECK(::chmod(lock.c_str(), 0620) == 0);
        expect_open_error(open_fixture(fixture), StoreError::lock_invalid,
                          StoreObject::session_lock);
    }
    {
        TempStore fixture;
        (void)write_canonical_header(fixture);
        const auto header = fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF);
        CHECK(::chmod(header.c_str(), 0620) == 0);
        expect_open_error(open_fixture(fixture), StoreError::entry_trust_invalid,
                          StoreObject::journal_header);
    }
    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        write_record(fixture, 1, canonical_start(header));
        const auto record_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(1);
        CHECK(record_leaf.has_value());
        const auto record = fixture.store_leaf(record_leaf->view());
        CHECK(::chmod(record.c_str(), 0620) == 0);
        expect_open_error(open_fixture(fixture), StoreError::entry_trust_invalid,
                          StoreObject::journal_record);
    }
#if defined(__APPLE__)
    {
        TempStore fixture;
        const auto acl = gnfs::test::run_child_process(
            "/bin/chmod",
            {"+a", "everyone allow add_file,delete_child", fixture.store_root().string()});
        CHECK(acl.exited);
        CHECK(acl.exit_code == 0);
        expect_open_error(open_fixture(fixture), StoreError::root_invalid, StoreObject::store_root);
        CHECK(!std::filesystem::exists(
            fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF)));
    }
#endif
}

void test_empty_store_lease_move_and_release(const std::string& executable) {
    TempStore fixture;

    auto original_result = open_fixture(fixture);
    CHECK(static_cast<bool>(original_result));
    auto moved_result = std::move(original_result);
    CHECK(!static_cast<bool>(original_result));
    CHECK(static_cast<bool>(moved_result));
    CHECK(!std::move(original_result).take_session().has_value());

    auto first = std::move(moved_result).take_session();
    CHECK(!static_cast<bool>(moved_result));
    CHECK(!std::move(moved_result).take_session().has_value());
    CHECK(first.has_value());
    CHECK(first->active());
    const auto initial_view = first->view();
    CHECK(initial_view.status == SIQSShadowProofRssJournalStatus::ready);
    CHECK(initial_view.reason == SIQSShadowProofRssJournalReason::ready);
    CHECK(initial_view.action == SIQSShadowProofRssJournalAction::create_header);
    CHECK(initial_view.committed_slot_count == 0);
    CHECK(initial_view.next_slot_number == 1);

    const auto lock_path =
        fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF);
    CHECK(std::filesystem::is_regular_file(lock_path));
    CHECK(std::filesystem::file_size(lock_path) == 0);

    std::optional<SIQSShadowProofRssCampaignJournalSession> lease(std::move(*first));
    CHECK(!first->active());
    CHECK(lease->active());

    const auto busy = gnfs::test::run_child_process(
        executable, {"--expect-lock-busy", fixture.trusted_base().string()});
    CHECK(busy.exited);
    CHECK(!busy.signaled);
    CHECK(busy.exit_code == 0);

    first.reset();
    lease.reset();

    auto reopened = take_successful_session(open_fixture(fixture));
    CHECK(reopened.has_value());
    CHECK(reopened->view().action == SIQSShadowProofRssJournalAction::create_header);
}

void test_crash_releases_persistent_lease(const std::string& executable) {
    TempStore fixture;
    const auto crash = gnfs::test::run_child_process(
        executable, {"--open-and-crash", fixture.trusted_base().string()});
    CHECK(crash.exited);
    CHECK(!crash.signaled);
    CHECK(crash.exit_code == 73);

    const auto lock_path =
        fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF);
    CHECK(std::filesystem::is_regular_file(lock_path));
    CHECK(std::filesystem::file_size(lock_path) == 0);

    auto reopened = take_successful_session(open_fixture(fixture));
    CHECK(reopened.has_value());
    CHECK(reopened->view().action == SIQSShadowProofRssJournalAction::create_header);
}

void test_unknown_case_and_temporary_leaves_fail_closed() {
    for (const std::string_view leaf :
         {"unexpected.txt", "Campaign-header.rjhd", ".campaign-header.rjhd.tmp"}) {
        TempStore fixture;
        fixture.write_store_leaf(leaf, {});
        expect_layout_error(open_fixture(fixture), LayoutError::unknown_entry);
    }
}

void test_invalid_session_lock_shapes() {
    const std::array one_byte{std::byte{0x5a}};

    {
        TempStore fixture;
        fixture.write_store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF,
                                 one_byte);
        expect_open_error(open_fixture(fixture), StoreError::lock_invalid,
                          StoreObject::session_lock);
    }
    {
        TempStore fixture;
        const auto target = fixture.base_leaf("lock-symlink-target");
        fixture.write_base_leaf("lock-symlink-target", {});
        const auto link =
            fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF);
        CHECK(::symlink(target.c_str(), link.c_str()) == 0);
        expect_open_error(open_fixture(fixture), StoreError::lock_open_failed,
                          StoreObject::session_lock);
    }
    {
        TempStore fixture;
        const auto directory =
            fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF);
        CHECK(::mkdir(directory.c_str(), 0700) == 0);
        expect_open_error(open_fixture(fixture), StoreError::lock_open_failed,
                          StoreObject::session_lock);
    }
    {
        TempStore fixture;
        fixture.write_base_leaf("lock-hardlink-target", {});
        const auto target = fixture.base_leaf("lock-hardlink-target");
        const auto link =
            fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF);
        CHECK(::link(target.c_str(), link.c_str()) == 0);
        expect_open_error(open_fixture(fixture), StoreError::lock_invalid,
                          StoreObject::session_lock);
    }
}

void test_invalid_header_shapes_sizes_and_codec() {
    {
        TempStore fixture;
        const auto target = fixture.base_leaf("header-symlink-target");
        std::array<std::byte, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE> bytes{};
        fixture.write_base_leaf("header-symlink-target", bytes);
        const auto link = fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF);
        CHECK(::symlink(target.c_str(), link.c_str()) == 0);
        expect_layout_error(open_fixture(fixture), LayoutError::entry_not_regular_file,
                            StoreObject::journal_header);
    }
    {
        TempStore fixture;
        const auto directory =
            fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF);
        CHECK(::mkdir(directory.c_str(), 0700) == 0);
        expect_layout_error(open_fixture(fixture), LayoutError::entry_not_regular_file,
                            StoreObject::journal_header);
    }
    {
        TempStore fixture;
        std::array<std::byte, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE> bytes{};
        fixture.write_base_leaf("header-hardlink-target", bytes);
        const auto target = fixture.base_leaf("header-hardlink-target");
        const auto link = fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF);
        CHECK(::link(target.c_str(), link.c_str()) == 0);
        expect_layout_error(open_fixture(fixture), LayoutError::link_count_invalid,
                            StoreObject::journal_header);
    }
    for (const std::size_t size : {SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE - 1,
                                   SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE + 1}) {
        TempStore fixture;
        const std::vector<std::byte> bytes(size);
        fixture.write_store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF, bytes);
        expect_layout_error(open_fixture(fixture), LayoutError::header_size_invalid,
                            StoreObject::journal_header);
    }
    {
        TempStore fixture;
        std::array<std::byte, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE> bytes{};
        bytes[0] = std::byte{0xff};
        fixture.write_store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF, bytes);
        auto result = open_fixture(fixture);
        CHECK(result.diagnostic().layout.codec_error ==
              SIQSShadowProofRssCampaignJournalCodecError::invalid_magic);
        CHECK(result.diagnostic().layout.codec_error_offset == 0);
        expect_layout_error(std::move(result), LayoutError::header_codec_invalid,
                            StoreObject::journal_header);
    }
}

void test_record_gap_and_filename_wire_binding() {
    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        write_record(fixture, 2, canonical_start(header));
        expect_layout_error(open_fixture(fixture), LayoutError::record_gap,
                            StoreObject::journal_record, 1);
    }
    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        auto record = canonical_start(header);
        record.sequence_number = 2;
        record.record_digest = shadow_proof_rss_campaign_journal_detail::record_digest(record);
        write_record(fixture, 1, record);
        expect_layout_error(open_fixture(fixture), LayoutError::record_sequence_mismatch,
                            StoreObject::journal_record, 1);
    }
}

void test_valid_header_and_dangling_start_actions() {
    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        const auto view = session->view();
        CHECK(view.status == SIQSShadowProofRssJournalStatus::ready);
        CHECK(view.reason == SIQSShadowProofRssJournalReason::ready);
        CHECK(view.action == SIQSShadowProofRssJournalAction::append_slot_start);
        CHECK(view.committed_slot_count == 0);
        CHECK(view.next_slot_number == 1);
        CHECK(view.plan_digest == header.plan_digest);
    }
    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        write_record(fixture, 1, canonical_start(header));
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        const auto view = session->view();
        CHECK(view.status == SIQSShadowProofRssJournalStatus::tainted);
        CHECK(view.reason == SIQSShadowProofRssJournalReason::dangling_slot_start);
        CHECK(view.action == SIQSShadowProofRssJournalAction::append_taint);
        CHECK(view.committed_slot_count == 0);
        CHECK(view.next_slot_number == 1);
        CHECK(view.plan_digest == header.plan_digest);
    }
}

int run_child_mode(std::string_view mode, const std::filesystem::path& trusted_base) {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto deployment = make_deployment(trusted_base);
    auto result = open_private(&policy, &facts, deployment);

    if (mode == "--expect-lock-busy") {
        if (result || result.diagnostic().error != StoreError::lock_busy ||
            result.diagnostic().object != StoreObject::session_lock ||
            std::move(result).take_session().has_value()) {
            return 71;
        }
        return 0;
    }
    if (mode == "--open-and-crash") {
        if (!result) {
            return 72;
        }
        auto session = std::move(result).take_session();
        if (!session.has_value() || !session->active()) {
            return 72;
        }
        std::_Exit(73);
    }
    return 64;
}

#else

void test_windows_private_boundary_is_unavailable() {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto deployment = make_deployment("C:\\gnfs-test-deployment");
    expect_open_error(open_private(&policy, &facts, deployment), StoreError::platform_unavailable,
                      StoreObject::none);
}

#endif

} // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
    if (argc == 3 && (std::string_view(argv[1]) == "--expect-lock-busy" ||
                      std::string_view(argv[1]) == "--open-and-crash")) {
        return run_child_mode(argv[1], std::filesystem::path(argv[2]));
    }
#else
    (void)argc;
    (void)argv;
#endif

    try {
        test_public_authority_and_preflight_boundaries();
        test_diagnostic_name_contracts();
#ifndef _WIN32
        const std::string executable =
            std::filesystem::absolute(std::filesystem::path(argv[0])).string();
        test_trusted_base_component_walk_is_fail_closed();
        test_untrusted_owner_and_write_permissions_fail_closed();
        test_empty_store_lease_move_and_release(executable);
        test_crash_releases_persistent_lease(executable);
        test_unknown_case_and_temporary_leaves_fail_closed();
        test_invalid_session_lock_shapes();
        test_invalid_header_shapes_sizes_and_codec();
        test_record_gap_and_filename_wire_binding();
        test_valid_header_and_dangling_start_actions();
#else
        test_windows_private_boundary_is_unavailable();
#endif
        std::cout << "SIQS RSS campaign journal native store tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SIQS RSS campaign journal native store test failure: " << error.what()
                  << '\n';
        return 1;
    }
}
