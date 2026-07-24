// Native boundary tests for the leased SIQS RSS campaign journal store.
//
// The production registry is intentionally empty. POSIX-only fixtures enter
// through the private deployment table so no test path or resolver is added to
// the public authority boundary.

#include <gnfs/siqs/shadow_proof_rss_campaign_journal_codec.hpp>
#include <gnfs/siqs/shadow_proof_rss_campaign_journal_store.hpp>

#include "shadow_proof_rss_campaign_journal_store_internal.hpp"
#include "shadow_proof_rss_campaign_slot_runner_internal.hpp"
#include "support/child_process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
namespace durable = gnfs::util::durable_immutable_file;
namespace store_detail = gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail;

using DeploymentEntry = store_detail::DeploymentEntry;
using LayoutError = SIQSShadowProofRssCampaignJournalLayoutError;
using StoreError = SIQSShadowProofRssCampaignJournalStoreError;
using StoreObject = SIQSShadowProofRssCampaignJournalStoreObject;
using SlotRunnerError = store_detail::SlotRunnerError;

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
static_assert(!std::is_default_constructible_v<SIQSShadowProofRssCampaignJournalActiveSlot>);
static_assert(!std::is_copy_constructible_v<SIQSShadowProofRssCampaignJournalActiveSlot>);
static_assert(!std::is_copy_assignable_v<SIQSShadowProofRssCampaignJournalActiveSlot>);
static_assert(std::is_nothrow_move_constructible_v<SIQSShadowProofRssCampaignJournalActiveSlot>);
static_assert(std::is_nothrow_move_assignable_v<SIQSShadowProofRssCampaignJournalActiveSlot>);
static_assert(!std::is_default_constructible_v<SIQSShadowProofRssCampaignJournalBeginSlotResult>);
static_assert(!std::is_copy_constructible_v<SIQSShadowProofRssCampaignJournalBeginSlotResult>);
static_assert(!std::is_copy_assignable_v<SIQSShadowProofRssCampaignJournalBeginSlotResult>);
static_assert(
    std::is_nothrow_move_constructible_v<SIQSShadowProofRssCampaignJournalBeginSlotResult>);
static_assert(std::is_nothrow_move_assignable_v<SIQSShadowProofRssCampaignJournalBeginSlotResult>);
using BeginSlotFunction = SIQSShadowProofRssCampaignJournalBeginSlotResult (
    SIQSShadowProofRssCampaignJournalSession::*)() && noexcept;
static_assert(std::same_as<decltype(&SIQSShadowProofRssCampaignJournalSession::begin_next_slot),
                           BeginSlotFunction>);
static_assert(!std::is_default_constructible_v<SIQSShadowProofRssCampaignJournalTaintResult>);
static_assert(!std::is_copy_constructible_v<SIQSShadowProofRssCampaignJournalTaintResult>);
static_assert(!std::is_copy_assignable_v<SIQSShadowProofRssCampaignJournalTaintResult>);
static_assert(std::is_nothrow_move_constructible_v<SIQSShadowProofRssCampaignJournalTaintResult>);
static_assert(std::is_nothrow_move_assignable_v<SIQSShadowProofRssCampaignJournalTaintResult>);
using ActiveSlotTaintFunction = SIQSShadowProofRssCampaignJournalTaintResult (
    SIQSShadowProofRssCampaignJournalActiveSlot::*)() && noexcept;
using PendingTaintFunction = SIQSShadowProofRssCampaignJournalTaintResult (
    SIQSShadowProofRssCampaignJournalSession::*)() && noexcept;
static_assert(std::same_as<decltype(&SIQSShadowProofRssCampaignJournalActiveSlot::taint),
                           ActiveSlotTaintFunction>);
static_assert(
    std::same_as<decltype(&SIQSShadowProofRssCampaignJournalSession::append_pending_taint),
                 PendingTaintFunction>);
using SlotRunFunction =
    store_detail::SlotRunnerResult (*)(SIQSShadowProofRssCampaignJournalActiveSlot&&) noexcept;
static_assert(std::same_as<decltype(&store_detail::SlotRunnerFactory::run), SlotRunFunction>);
static_assert(!std::is_default_constructible_v<store_detail::SlotRunnerResult>);
static_assert(!std::is_copy_constructible_v<store_detail::SlotRunnerResult>);
static_assert(!std::is_copy_assignable_v<store_detail::SlotRunnerResult>);
static_assert(std::is_nothrow_move_constructible_v<store_detail::SlotRunnerResult>);
static_assert(std::is_nothrow_move_assignable_v<store_detail::SlotRunnerResult>);
static_assert(!std::is_default_constructible_v<store_detail::SameChildExecutionReceipt>);
static_assert(!std::is_copy_constructible_v<store_detail::SameChildExecutionReceipt>);

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
        std::pair{StoreError::none, std::string_view{"none"}},
        std::pair{StoreError::preflight_rejected, std::string_view{"preflight_rejected"}},
        std::pair{StoreError::platform_unavailable, std::string_view{"platform_unavailable"}},
        std::pair{StoreError::binding_not_registered, std::string_view{"binding_not_registered"}},
        std::pair{StoreError::binding_ambiguous, std::string_view{"binding_ambiguous"}},
        std::pair{StoreError::registry_binding_mismatch,
                  std::string_view{"registry_binding_mismatch"}},
        std::pair{StoreError::base_open_failed, std::string_view{"base_open_failed"}},
        std::pair{StoreError::base_invalid, std::string_view{"base_invalid"}},
        std::pair{StoreError::root_open_failed, std::string_view{"root_open_failed"}},
        std::pair{StoreError::root_invalid, std::string_view{"root_invalid"}},
        std::pair{StoreError::artifact_root_open_failed,
                  std::string_view{"artifact_root_open_failed"}},
        std::pair{StoreError::artifact_root_invalid, std::string_view{"artifact_root_invalid"}},
        std::pair{StoreError::lock_open_failed, std::string_view{"lock_open_failed"}},
        std::pair{StoreError::lock_invalid, std::string_view{"lock_invalid"}},
        std::pair{StoreError::lock_busy, std::string_view{"lock_busy"}},
        std::pair{StoreError::lock_failed, std::string_view{"lock_failed"}},
        std::pair{StoreError::directory_open_failed, std::string_view{"directory_open_failed"}},
        std::pair{StoreError::directory_read_failed, std::string_view{"directory_read_failed"}},
        std::pair{StoreError::entry_metadata_failed, std::string_view{"entry_metadata_failed"}},
        std::pair{StoreError::entry_trust_invalid, std::string_view{"entry_trust_invalid"}},
        std::pair{StoreError::entry_open_failed, std::string_view{"entry_open_failed"}},
        std::pair{StoreError::entry_read_failed, std::string_view{"entry_read_failed"}},
        std::pair{StoreError::entry_identity_mismatch, std::string_view{"entry_identity_mismatch"}},
        std::pair{StoreError::entry_changed_during_read,
                  std::string_view{"entry_changed_during_read"}},
        std::pair{StoreError::snapshot_changed, std::string_view{"snapshot_changed"}},
        std::pair{StoreError::layout_invalid, std::string_view{"layout_invalid"}},
        std::pair{StoreError::artifact_layout_invalid, std::string_view{"artifact_layout_invalid"}},
        std::pair{StoreError::artifact_consistency_invalid,
                  std::string_view{"artifact_consistency_invalid"}},
        std::pair{StoreError::replay_rejected, std::string_view{"replay_rejected"}},
        std::pair{StoreError::session_inactive, std::string_view{"session_inactive"}},
        std::pair{StoreError::session_action_invalid, std::string_view{"session_action_invalid"}},
        std::pair{StoreError::journal_encode_failed, std::string_view{"journal_encode_failed"}},
        std::pair{StoreError::publication_conflict, std::string_view{"publication_conflict"}},
        std::pair{StoreError::publication_failed, std::string_view{"publication_failed"}},
        std::pair{StoreError::receipt_rejected, std::string_view{"receipt_rejected"}},
        std::pair{StoreError::commit_outcome_uncertain,
                  std::string_view{"commit_outcome_uncertain"}},
        std::pair{StoreError::resource_exhausted, std::string_view{"resource_exhausted"}},
        std::pair{StoreError::unexpected_failure, std::string_view{"unexpected_failure"}},
    };
    for (const auto& [error, expected_name] : known_errors) {
        CHECK(siqs_shadow_proof_rss_campaign_journal_store_error_name(error) == expected_name);
    }
    CHECK(siqs_shadow_proof_rss_campaign_journal_store_error_name(
              static_cast<StoreError>(UINT8_C(255))) == "unknown");

    constexpr std::array known_objects{
        std::pair{StoreObject::none, std::string_view{"none"}},
        std::pair{StoreObject::deployment_registry, std::string_view{"deployment_registry"}},
        std::pair{StoreObject::trusted_base, std::string_view{"trusted_base"}},
        std::pair{StoreObject::store_root, std::string_view{"store_root"}},
        std::pair{StoreObject::artifact_root, std::string_view{"artifact_root"}},
        std::pair{StoreObject::artifact, std::string_view{"artifact"}},
        std::pair{StoreObject::session_lock, std::string_view{"session_lock"}},
        std::pair{StoreObject::directory, std::string_view{"directory"}},
        std::pair{StoreObject::journal_header, std::string_view{"journal_header"}},
        std::pair{StoreObject::journal_record, std::string_view{"journal_record"}},
    };
    for (const auto& [object, expected_name] : known_objects) {
        CHECK(siqs_shadow_proof_rss_campaign_journal_store_object_name(object) == expected_name);
    }
    CHECK(siqs_shadow_proof_rss_campaign_journal_store_object_name(
              static_cast<StoreObject>(UINT8_C(255))) == "unknown");
}

#ifndef _WIN32

class TestPublicationOps final : public store_detail::PublicationOps {
public:
    enum class Action : uint8_t {
        fail_before_create,
        report_durable_without_file,
        publish_bytes_then_report_sync_failure,
    };

    TestPublicationOps(std::size_t target_call, Action action) noexcept
        : target_call_(target_call), action_(action) {}

    [[nodiscard]] durable::PublishResult
    publish_at(durable::NativeHandle parent_handle, const std::filesystem::path& leaf,
               std::span<const std::byte> bytes) noexcept override {
        ++publish_calls_;
        if (publish_calls_ == target_call_) {
            if (action_ == Action::report_durable_without_file) {
                return {durable::PublishStatus::durable, {}, static_cast<uint64_t>(bytes.size())};
            }
            if (action_ == Action::publish_bytes_then_report_sync_failure) {
                const auto publication = durable::publish_at(parent_handle, leaf, bytes);
                if (!publication.is_durable()) {
                    return publication;
                }
                return {durable::PublishStatus::parent_directory_sync_failed,
                        std::make_error_code(std::errc::io_error), publication.bytes_written()};
            }
            return {durable::PublishStatus::open_failed, std::make_error_code(std::errc::io_error),
                    0};
        }
        return durable::publish_at(parent_handle, leaf, bytes);
    }

    [[nodiscard]] durable::PublishResult
    confirm_durable_at(durable::NativeHandle parent_handle,
                       const std::filesystem::path& leaf) noexcept override {
        return durable::confirm_durable_at(parent_handle, leaf);
    }

    [[nodiscard]] std::size_t publish_calls() const noexcept {
        return publish_calls_;
    }

private:
    std::size_t target_call_ = 0;
    Action action_ = Action::fail_before_create;
    std::size_t publish_calls_ = 0;
};

class TestConfirmationOps final : public store_detail::PublicationOps {
public:
    enum class Action : uint8_t {
        fail_before_confirm,
        remove_and_report_durable,
    };

    TestConfirmationOps(std::size_t target_call, Action action) noexcept
        : target_call_(target_call), action_(action) {}

    [[nodiscard]] durable::PublishResult
    publish_at(durable::NativeHandle parent_handle, const std::filesystem::path& leaf,
               std::span<const std::byte> bytes) noexcept override {
        return durable::publish_at(parent_handle, leaf, bytes);
    }

    [[nodiscard]] durable::PublishResult
    confirm_durable_at(durable::NativeHandle parent_handle,
                       const std::filesystem::path& leaf) noexcept override {
        ++confirm_calls_;
        if (confirm_calls_ != target_call_) {
            return durable::confirm_durable_at(parent_handle, leaf);
        }
        if (action_ == Action::remove_and_report_durable) {
            if (::unlinkat(static_cast<int>(parent_handle), leaf.c_str(), 0) != 0) {
                return {durable::PublishStatus::open_failed,
                        std::error_code(errno, std::generic_category()), 0};
            }
            return {durable::PublishStatus::durable, {}, 0};
        }
        return {durable::PublishStatus::open_failed, std::make_error_code(std::errc::io_error), 0};
    }

    [[nodiscard]] std::size_t confirm_calls() const noexcept {
        return confirm_calls_;
    }

private:
    std::size_t target_call_ = 0;
    Action action_ = Action::fail_before_confirm;
    std::size_t confirm_calls_ = 0;
};

class CommitPublicationOps final : public store_detail::PublicationOps {
public:
    enum class LeafShape : uint8_t {
        exact,
        one_byte,
        different,
        exact_already_exists,
    };

    CommitPublicationOps(std::size_t target_publish_call, LeafShape leaf_shape,
                         bool fail_confirmation) noexcept
        : target_publish_call_(target_publish_call), leaf_shape_(leaf_shape),
          fail_confirmation_(fail_confirmation) {}

    [[nodiscard]] durable::PublishResult
    publish_at(durable::NativeHandle parent_handle, const std::filesystem::path& leaf,
               std::span<const std::byte> bytes) noexcept override {
        ++publish_calls_;
        if (publish_calls_ != target_publish_call_) {
            return durable::publish_at(parent_handle, leaf, bytes);
        }
        std::array<std::byte, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE>
            different_bytes{};
        std::span<const std::byte> selected = bytes;
        if (leaf_shape_ == LeafShape::one_byte) {
            selected = bytes.first(std::min<std::size_t>(1, bytes.size()));
        } else if (leaf_shape_ == LeafShape::different) {
            if (bytes.size() != different_bytes.size()) {
                return {durable::PublishStatus::file_ops_contract_violation, {}, 0};
            }
            std::copy(bytes.begin(), bytes.end(), different_bytes.begin());
            different_bytes.back() ^= std::byte{1};
            selected = different_bytes;
        }
        const auto publication = durable::publish_at(parent_handle, leaf, selected);
        if (!publication.is_durable()) {
            return publication;
        }
        if (leaf_shape_ == LeafShape::exact_already_exists) {
            return {durable::PublishStatus::already_exists,
                    std::make_error_code(std::errc::file_exists), 0};
        }
        return {durable::PublishStatus::parent_directory_sync_failed,
                std::make_error_code(std::errc::io_error), publication.bytes_written()};
    }

    [[nodiscard]] durable::PublishResult
    confirm_durable_at(durable::NativeHandle parent_handle,
                       const std::filesystem::path& leaf) noexcept override {
        ++confirm_calls_;
        if (fail_confirmation_ && confirm_calls_ == 1) {
            return {durable::PublishStatus::open_failed, std::make_error_code(std::errc::io_error),
                    0};
        }
        return durable::confirm_durable_at(parent_handle, leaf);
    }

    [[nodiscard]] std::size_t publish_calls() const noexcept {
        return publish_calls_;
    }

    [[nodiscard]] std::size_t confirm_calls() const noexcept {
        return confirm_calls_;
    }

private:
    std::size_t target_publish_call_ = 0;
    LeafShape leaf_shape_ = LeafShape::exact;
    bool fail_confirmation_ = false;
    std::size_t publish_calls_ = 0;
    std::size_t confirm_calls_ = 0;
};

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
        artifact_root_ =
            store_root_ / std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_DIRECTORY_LEAF);
        if (::mkdir(artifact_root_.c_str(), 0700) != 0) {
            throw std::system_error(errno, std::generic_category(), "mkdir artifact root");
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

    [[nodiscard]] const std::filesystem::path& artifact_root() const noexcept {
        return artifact_root_;
    }

    [[nodiscard]] std::filesystem::path store_leaf(std::string_view leaf) const {
        return store_root_ / std::string(leaf);
    }

    [[nodiscard]] std::filesystem::path artifact_leaf(std::string_view leaf) const {
        return artifact_root_ / std::string(leaf);
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

    void write_artifact_leaf(std::string_view leaf, std::span<const std::byte> bytes) const {
        write_file(artifact_leaf(leaf), bytes);
    }

    void write_artifact_leaf(std::string_view leaf, std::string_view bytes) const {
        write_file(artifact_leaf(leaf),
                   std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
    }

    [[nodiscard]] std::vector<std::byte> read_store_leaf(std::string_view leaf) const {
        return read_file(store_leaf(leaf));
    }

    [[nodiscard]] std::vector<std::byte> read_artifact_leaf(std::string_view leaf) const {
        return read_file(artifact_leaf(leaf));
    }

private:
    [[nodiscard]] static std::vector<std::byte> read_file(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        std::vector<std::byte> bytes;
        for (std::istreambuf_iterator<char> it(stream), end; it != end; ++it) {
            bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(*it)));
        }
        return bytes;
    }

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
    std::filesystem::path artifact_root_;
};

struct SyntheticChildren final {
    std::filesystem::path success;
    std::filesystem::path nonzero;
    std::filesystem::path malformed;
    std::filesystem::path overflow;
    std::filesystem::path hang;
};

[[nodiscard]] DeploymentEntry
make_runner_deployment(const TempStore& fixture, const std::filesystem::path& executable,
                       const std::filesystem::path& marker,
                       std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    auto deployment = make_deployment(fixture.trusted_base());
    deployment.holdout_probe.emplace(store_detail::ProbeExecutableBinding{
        .executable = std::filesystem::absolute(executable),
        .candidate_revision = std::string(make_policy().candidate_revision),
        .environment =
            {
                "GNFS_SIQS_RSS_SYNTHETIC_MARKER=" + marker.string(),
            },
        .timeout = timeout,
        .expected_owner = deployment.expected_owner,
    });
    return deployment;
}

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string bytes_as_string(const std::vector<std::byte>& bytes) {
    std::string result;
    result.reserve(bytes.size());
    for (const std::byte byte : bytes) {
        result.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    return result;
}

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

[[nodiscard]] SIQSShadowProofRssJournalCommitPayload
make_off_payload(std::string_view stdout_bytes, std::string_view stderr_bytes,
                 std::string_view joined_bytes) {
    const auto policy = make_policy();
    SIQSShadowProofRssJournalCommitPayload payload;
    payload.actual_operating_system = policy.operating_system;
    payload.actual_architecture = policy.architecture;
    payload.actual_memory_backend = policy.memory_backend;
    payload.actual_resolved_sieve_workers = policy.resolved_production_sieve_workers;
    payload.fresh_process = true;
    payload.completed = true;
    payload.factor_identity = SIQSShadowProofRssFactorIdentity::pass;
    payload.proof_evidence = SIQSShadowProofRssEvidence::not_applicable;
    payload.matrix_evidence = SIQSShadowProofRssEvidence::not_applicable;
    payload.absolute_peak_rss_bytes = UINT64_C(123456);
    payload.current_rss_bytes = UINT64_C(65432);
    payload.peak_growth_bytes = UINT64_C(1234);
    payload.wall_ns = UINT64_C(9999);
    payload.stdout_seal = seal_siqs_shadow_proof_rss_artifact(
        SIQSShadowProofRssArtifactKind::probe_stdout, stdout_bytes);
    payload.stderr_seal = seal_siqs_shadow_proof_rss_artifact(
        SIQSShadowProofRssArtifactKind::probe_stderr, stderr_bytes);
    payload.joined_sample_seal = seal_siqs_shadow_proof_rss_artifact(
        SIQSShadowProofRssArtifactKind::joined_gate_sample, joined_bytes);
    return payload;
}

[[nodiscard]] SIQSShadowProofRssCampaignJournalRecord
make_commit(const SIQSShadowProofRssCampaignJournalRecord& start,
            const SIQSShadowProofRssJournalCommitPayload& payload) {
    SIQSShadowProofRssCampaignJournalRecord commit;
    commit.schema_version = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION;
    commit.sequence_number = start.sequence_number + 1;
    commit.kind = SIQSShadowProofRssJournalRecordKind::slot_committed;
    commit.previous_record_digest = start.record_digest;
    commit.plan_digest = start.plan_digest;
    commit.slot_number = start.slot_number;
    commit.slot_digest = start.slot_digest;
    commit.commit_payload = payload;
    commit.record_digest = shadow_proof_rss_campaign_journal_detail::record_digest(commit);
    return commit;
}

void write_record(const TempStore& fixture, uint32_t leaf_sequence,
                  const SIQSShadowProofRssCampaignJournalRecord& record) {
    const auto leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(leaf_sequence);
    CHECK(leaf.has_value());
    const auto encoded = encode_siqs_shadow_proof_rss_campaign_journal_record(record);
    CHECK(encoded.bytes.has_value());
    fixture.write_store_leaf(leaf->view(), byte_span(*encoded.bytes));
}

[[nodiscard]] SIQSShadowProofRssCampaignArtifactLeaf
artifact_leaf(uint32_t slot_number, SIQSShadowProofRssArtifactKind kind) {
    const auto leaf = make_siqs_shadow_proof_rss_campaign_artifact_leaf(slot_number, kind);
    CHECK(leaf.has_value());
    return *leaf;
}

void write_artifact(const TempStore& fixture, uint32_t slot_number,
                    SIQSShadowProofRssArtifactKind kind, std::string_view bytes) {
    const auto leaf = artifact_leaf(slot_number, kind);
    fixture.write_artifact_leaf(leaf.view(), bytes);
}

void write_committed_off_slots(const TempStore& fixture, uint32_t slot_count,
                               std::string_view stdout_bytes, std::string_view stderr_bytes,
                               std::string_view joined_bytes) {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto header = write_canonical_header(fixture);
    std::vector<SIQSShadowProofRssCampaignJournalRecord> records;
    records.reserve(static_cast<std::size_t>(slot_count) * 2);
    for (uint32_t slot_number = 1; slot_number <= slot_count; ++slot_number) {
        const auto resume = resume_siqs_shadow_proof_rss_campaign_journal(
            &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header,
            std::span<const SIQSShadowProofRssCampaignJournalRecord>(records));
        CHECK(resume.status == SIQSShadowProofRssJournalStatus::ready);
        CHECK(resume.action == SIQSShadowProofRssJournalAction::append_slot_start);
        CHECK(resume.next_slot_number == slot_number);
        CHECK(resume.prepared_slot_start.has_value());
        const auto start = resume.prepared_slot_start->record();
        write_record(fixture, start.sequence_number, start);
        records.push_back(start);

        write_artifact(fixture, slot_number, SIQSShadowProofRssArtifactKind::probe_stdout,
                       stdout_bytes);
        write_artifact(fixture, slot_number, SIQSShadowProofRssArtifactKind::probe_stderr,
                       stderr_bytes);
        write_artifact(fixture, slot_number, SIQSShadowProofRssArtifactKind::joined_gate_sample,
                       joined_bytes);
        const auto commit =
            make_commit(start, make_off_payload(stdout_bytes, stderr_bytes, joined_bytes));
        write_record(fixture, commit.sequence_number, commit);
        records.push_back(commit);
    }
}

void expect_private_regular_leaf(const std::filesystem::path& path, std::size_t expected_size) {
    struct stat metadata{};
    CHECK(::lstat(path.c_str(), &metadata) == 0);
    CHECK(S_ISREG(metadata.st_mode));
    CHECK(static_cast<uint64_t>(metadata.st_uid) == static_cast<uint64_t>(::geteuid()));
    CHECK((metadata.st_mode & (S_IWGRP | S_IWOTH)) == 0);
    CHECK(metadata.st_nlink == 1);
    CHECK(metadata.st_size >= 0);
    CHECK(static_cast<std::size_t>(metadata.st_size) == expected_size);
}

void expect_begin_error(SIQSShadowProofRssCampaignJournalBeginSlotResult result, StoreError error,
                        StoreObject object) {
    CHECK(!static_cast<bool>(result));
    if (result.diagnostic().error != error || result.diagnostic().object != object) {
        std::cerr
            << "expected begin error "
            << siqs_shadow_proof_rss_campaign_journal_store_error_name(error) << '/'
            << siqs_shadow_proof_rss_campaign_journal_store_object_name(object) << ", got "
            << siqs_shadow_proof_rss_campaign_journal_store_error_name(result.diagnostic().error)
            << '/'
            << siqs_shadow_proof_rss_campaign_journal_store_object_name(result.diagnostic().object)
            << " native=" << result.diagnostic().native_error.value() << '\n';
    }
    CHECK(result.diagnostic().error == error);
    CHECK(result.diagnostic().object == object);
    CHECK(!std::move(result).take_active_slot().has_value());
    CHECK(!std::move(result).take_active_slot().has_value());
}

void expect_prepublication_namespace_change(
    SIQSShadowProofRssCampaignJournalBeginSlotResult result) {
    CHECK(!static_cast<bool>(result));
    const auto& diagnostic = result.diagnostic();
    CHECK(diagnostic.error == StoreError::snapshot_changed);
    CHECK(diagnostic.object == StoreObject::directory);
    CHECK(!diagnostic.publication_status.has_value());
    CHECK(diagnostic.publication_bytes_written == 0);
    CHECK(!diagnostic.last_durable_publication_object.has_value());
    CHECK(diagnostic.last_durable_publication_record_sequence ==
          SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE);
    CHECK(diagnostic.last_durable_publication_bytes_written == 0);
    CHECK(!std::move(result).take_active_slot().has_value());
    CHECK(!std::move(result).take_active_slot().has_value());
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

void test_active_slot_taint_is_durable_and_terminal() {
    TempStore fixture;
    auto session = take_successful_session(open_fixture(fixture));
    CHECK(session.has_value());
    auto begin = std::move(*session).begin_next_slot();
    CHECK(begin);
    auto active = std::move(begin).take_active_slot();
    CHECK(active.has_value());
    CHECK(active->active());

    auto taint = std::move(*active).taint();
    CHECK(!active->active());
    CHECK(static_cast<bool>(taint));
    CHECK(taint.diagnostic().error == StoreError::none);
    CHECK(taint.view().status == SIQSShadowProofRssJournalStatus::tainted);
    CHECK(taint.view().reason == SIQSShadowProofRssJournalReason::explicitly_tainted);
    CHECK(taint.view().action == SIQSShadowProofRssJournalAction::none);
    CHECK(taint.view().committed_slot_count == 0);
    CHECK(taint.view().next_slot_number == 1);

    const auto taint_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(2);
    CHECK(taint_leaf.has_value());
    const auto decoded = decode_siqs_shadow_proof_rss_campaign_journal_record(
        fixture.read_store_leaf(taint_leaf->view()));
    CHECK(decoded);
    CHECK(decoded.value->kind == SIQSShadowProofRssJournalRecordKind::campaign_tainted);
    CHECK(decoded.value->slot_number == 1);

    auto reopened = take_successful_session(open_fixture(fixture));
    CHECK(reopened.has_value());
    CHECK(reopened->view() == taint.view());
}

void test_reopened_dangling_start_appends_pending_taint() {
    TempStore fixture;
    const auto header = write_canonical_header(fixture);
    write_record(fixture, 1, canonical_start(header));
    auto session = take_successful_session(open_fixture(fixture));
    CHECK(session.has_value());
    CHECK(session->view().action == SIQSShadowProofRssJournalAction::append_taint);

    auto taint = std::move(*session).append_pending_taint();
    CHECK(!session->active());
    CHECK(static_cast<bool>(taint));
    CHECK(taint.view().reason == SIQSShadowProofRssJournalReason::explicitly_tainted);
    CHECK(taint.view().action == SIQSShadowProofRssJournalAction::none);

    auto reopened = take_successful_session(open_fixture(fixture));
    CHECK(reopened.has_value());
    CHECK(reopened->view() == taint.view());
}

void test_reopened_taint_confirms_dangling_chain_durability() {
    {
        TempStore fixture;
        TestPublicationOps ops(2,
                               TestPublicationOps::Action::publish_bytes_then_report_sync_failure);
        auto deployment = make_deployment(fixture.trusted_base());
        deployment.publication_ops = &ops;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto session = take_successful_session(open_private(&policy, &facts, deployment));
        CHECK(session.has_value());

        const auto begin = std::move(*session).begin_next_slot();
        CHECK(!static_cast<bool>(begin));
        CHECK(ops.publish_calls() == 2);
        CHECK(begin.diagnostic().error == StoreError::publication_failed);
        CHECK(begin.diagnostic().object == StoreObject::journal_record);
        CHECK(begin.diagnostic().publication_status ==
              durable::PublishStatus::parent_directory_sync_failed);
        CHECK(begin.diagnostic().publication_bytes_written ==
              SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE);
        const auto start_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(1);
        CHECK(start_leaf.has_value());
        CHECK(std::filesystem::exists(fixture.store_leaf(start_leaf->view())));

        auto reopened = take_successful_session(open_fixture(fixture));
        CHECK(reopened.has_value());
        CHECK(reopened->view().action == SIQSShadowProofRssJournalAction::append_taint);
        const auto recovered = std::move(*reopened).append_pending_taint();
        CHECK(static_cast<bool>(recovered));
        CHECK(recovered.view().reason == SIQSShadowProofRssJournalReason::explicitly_tainted);
    }
    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        write_record(fixture, 1, canonical_start(header));
        TestConfirmationOps ops(1, TestConfirmationOps::Action::fail_before_confirm);
        auto deployment = make_deployment(fixture.trusted_base());
        deployment.publication_ops = &ops;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto session = take_successful_session(open_private(&policy, &facts, deployment));
        CHECK(session.has_value());

        const auto taint = std::move(*session).append_pending_taint();
        CHECK(!static_cast<bool>(taint));
        CHECK(ops.confirm_calls() == 1);
        CHECK(taint.diagnostic().error == StoreError::publication_failed);
        CHECK(taint.diagnostic().object == StoreObject::journal_header);
        CHECK(taint.diagnostic().publication_status == durable::PublishStatus::open_failed);
        const auto taint_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(2);
        CHECK(taint_leaf.has_value());
        CHECK(!std::filesystem::exists(fixture.store_leaf(taint_leaf->view())));

        auto reopened = take_successful_session(open_fixture(fixture));
        CHECK(reopened.has_value());
        const auto recovered = std::move(*reopened).append_pending_taint();
        CHECK(static_cast<bool>(recovered));
    }
    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        write_record(fixture, 1, canonical_start(header));
        TestConfirmationOps ops(2, TestConfirmationOps::Action::fail_before_confirm);
        auto deployment = make_deployment(fixture.trusted_base());
        deployment.publication_ops = &ops;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto session = take_successful_session(open_private(&policy, &facts, deployment));
        CHECK(session.has_value());

        const auto taint = std::move(*session).append_pending_taint();
        CHECK(!static_cast<bool>(taint));
        CHECK(ops.confirm_calls() == 2);
        CHECK(taint.diagnostic().error == StoreError::publication_failed);
        CHECK(taint.diagnostic().object == StoreObject::journal_record);
        CHECK(taint.diagnostic().record_sequence == 1);
        CHECK(taint.diagnostic().publication_status == durable::PublishStatus::open_failed);
        const auto taint_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(2);
        CHECK(taint_leaf.has_value());
        CHECK(!std::filesystem::exists(fixture.store_leaf(taint_leaf->view())));

        auto reopened = take_successful_session(open_fixture(fixture));
        CHECK(reopened.has_value());
        const auto recovered = std::move(*reopened).append_pending_taint();
        CHECK(static_cast<bool>(recovered));
    }
    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        write_record(fixture, 1, canonical_start(header));
        TestConfirmationOps ops(2, TestConfirmationOps::Action::remove_and_report_durable);
        auto deployment = make_deployment(fixture.trusted_base());
        deployment.publication_ops = &ops;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto session = take_successful_session(open_private(&policy, &facts, deployment));
        CHECK(session.has_value());

        const auto taint = std::move(*session).append_pending_taint();
        CHECK(!static_cast<bool>(taint));
        CHECK(ops.confirm_calls() == 2);
        CHECK(taint.diagnostic().error == StoreError::snapshot_changed);
        CHECK(taint.diagnostic().publication_status == durable::PublishStatus::durable);
        const auto start_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(1);
        const auto taint_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(2);
        CHECK(start_leaf.has_value());
        CHECK(taint_leaf.has_value());
        CHECK(!std::filesystem::exists(fixture.store_leaf(start_leaf->view())));
        CHECK(!std::filesystem::exists(fixture.store_leaf(taint_leaf->view())));
    }
    {
        TempStore fixture;
        TestConfirmationOps ops(1, TestConfirmationOps::Action::fail_before_confirm);
        auto deployment = make_deployment(fixture.trusted_base());
        deployment.publication_ops = &ops;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto session = take_successful_session(open_private(&policy, &facts, deployment));
        CHECK(session.has_value());
        auto begin = std::move(*session).begin_next_slot();
        CHECK(begin);
        auto active = std::move(begin).take_active_slot();
        CHECK(active.has_value());

        const auto taint = std::move(*active).taint();
        CHECK(static_cast<bool>(taint));
        CHECK(ops.confirm_calls() == 0);
    }
}

void test_committed_prefix_durability_precedes_next_authority() {
    constexpr std::string_view stdout_bytes = "committed-off-stdout\n";
    constexpr std::string_view stderr_bytes;
    constexpr std::string_view joined_bytes = "committed-off-joined\n";
    struct ExpectedFailure final {
        StoreObject object = StoreObject::none;
        uint32_t record_sequence = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE;
        std::optional<SIQSShadowProofRssArtifactKind> artifact_kind;
    };
    const std::array expected_failures{
        ExpectedFailure{StoreObject::journal_header,
                        SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE, std::nullopt},
        ExpectedFailure{StoreObject::journal_record, 1, std::nullopt},
        ExpectedFailure{StoreObject::artifact,
                        SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE,
                        SIQSShadowProofRssArtifactKind::probe_stdout},
        ExpectedFailure{StoreObject::artifact,
                        SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE,
                        SIQSShadowProofRssArtifactKind::probe_stderr},
        ExpectedFailure{StoreObject::artifact,
                        SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE,
                        SIQSShadowProofRssArtifactKind::joined_gate_sample},
        ExpectedFailure{StoreObject::journal_record, 2, std::nullopt},
    };

    for (std::size_t index = 0; index < expected_failures.size(); ++index) {
        TempStore fixture;
        write_committed_off_slots(fixture, 1, stdout_bytes, stderr_bytes, joined_bytes);
        TestConfirmationOps ops(index + 1, TestConfirmationOps::Action::fail_before_confirm);
        auto deployment = make_deployment(fixture.trusted_base());
        deployment.publication_ops = &ops;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto session = take_successful_session(open_private(&policy, &facts, deployment));
        CHECK(session.has_value());

        auto begin = std::move(*session).begin_next_slot();
        CHECK(!static_cast<bool>(begin));
        CHECK(ops.confirm_calls() == index + 1);
        CHECK(begin.diagnostic().error == StoreError::publication_failed);
        CHECK(begin.diagnostic().object == expected_failures[index].object);
        CHECK(begin.diagnostic().record_sequence == expected_failures[index].record_sequence);
        CHECK(begin.diagnostic().artifact_slot_number ==
              (expected_failures[index].artifact_kind.has_value()
                   ? 1U
                   : SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_NO_SLOT));
        CHECK(begin.diagnostic().artifact_kind == expected_failures[index].artifact_kind);
        CHECK(begin.diagnostic().publication_status == durable::PublishStatus::open_failed);
        const auto next_start = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(3);
        CHECK(next_start.has_value());
        CHECK(!std::filesystem::exists(fixture.store_leaf(next_start->view())));
    }

    {
        TempStore fixture;
        write_committed_off_slots(fixture, 1, stdout_bytes, stderr_bytes, joined_bytes);
        TestConfirmationOps ops(100, TestConfirmationOps::Action::fail_before_confirm);
        auto deployment = make_deployment(fixture.trusted_base());
        deployment.publication_ops = &ops;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto session = take_successful_session(open_private(&policy, &facts, deployment));
        CHECK(session.has_value());

        auto begin = std::move(*session).begin_next_slot();
        CHECK(begin);
        CHECK(ops.confirm_calls() == expected_failures.size());
        auto active = std::move(begin).take_active_slot();
        CHECK(active.has_value());
        CHECK(active->slot_number() == 2);
        const auto taint = std::move(*active).taint();
        CHECK(static_cast<bool>(taint));
        CHECK(ops.confirm_calls() == expected_failures.size());
    }

    {
        TempStore fixture;
        write_committed_off_slots(fixture, 2, stdout_bytes, stderr_bytes, joined_bytes);
        TestConfirmationOps ops(7, TestConfirmationOps::Action::fail_before_confirm);
        auto deployment = make_deployment(fixture.trusted_base());
        deployment.publication_ops = &ops;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto session = take_successful_session(open_private(&policy, &facts, deployment));
        CHECK(session.has_value());

        auto begin = std::move(*session).begin_next_slot();
        CHECK(!static_cast<bool>(begin));
        CHECK(ops.confirm_calls() == 7);
        CHECK(begin.diagnostic().error == StoreError::publication_failed);
        CHECK(begin.diagnostic().object == StoreObject::journal_record);
        CHECK(begin.diagnostic().record_sequence == 3);
        const auto next_start = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(5);
        CHECK(next_start.has_value());
        CHECK(!std::filesystem::exists(fixture.store_leaf(next_start->view())));
    }

    {
        TempStore fixture;
        write_committed_off_slots(fixture, 1, stdout_bytes, stderr_bytes, joined_bytes);
        TestConfirmationOps ops(3, TestConfirmationOps::Action::remove_and_report_durable);
        auto deployment = make_deployment(fixture.trusted_base());
        deployment.publication_ops = &ops;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto session = take_successful_session(open_private(&policy, &facts, deployment));
        CHECK(session.has_value());

        auto begin = std::move(*session).begin_next_slot();
        CHECK(!static_cast<bool>(begin));
        CHECK(ops.confirm_calls() == 3);
        CHECK(begin.diagnostic().error == StoreError::snapshot_changed);
        CHECK(begin.diagnostic().object == StoreObject::artifact_root);
        CHECK(begin.diagnostic().publication_status == durable::PublishStatus::durable);
        const auto next_start = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(3);
        CHECK(next_start.has_value());
        CHECK(!std::filesystem::exists(fixture.store_leaf(next_start->view())));
    }

    {
        TempStore fixture;
        write_committed_off_slots(fixture, 1, stdout_bytes, stderr_bytes, joined_bytes);
        {
            auto session = take_successful_session(open_fixture(fixture));
            CHECK(session.has_value());
            auto begin = std::move(*session).begin_next_slot();
            CHECK(begin);
            auto active = std::move(begin).take_active_slot();
            CHECK(active.has_value());
        }

        TestConfirmationOps ops(7, TestConfirmationOps::Action::fail_before_confirm);
        auto deployment = make_deployment(fixture.trusted_base());
        deployment.publication_ops = &ops;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto reopened = take_successful_session(open_private(&policy, &facts, deployment));
        CHECK(reopened.has_value());
        CHECK(reopened->view().reason == SIQSShadowProofRssJournalReason::dangling_slot_start);

        const auto taint = std::move(*reopened).append_pending_taint();
        CHECK(!static_cast<bool>(taint));
        CHECK(ops.confirm_calls() == 7);
        CHECK(taint.diagnostic().error == StoreError::publication_failed);
        CHECK(taint.diagnostic().object == StoreObject::journal_record);
        CHECK(taint.diagnostic().record_sequence == 3);
        const auto taint_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(4);
        CHECK(taint_leaf.has_value());
        CHECK(!std::filesystem::exists(fixture.store_leaf(taint_leaf->view())));
    }
}

[[nodiscard]] std::optional<SIQSShadowProofRssCampaignJournalActiveSlot>
begin_runner_slot(const DeploymentEntry& deployment) {
    const auto policy = make_policy();
    const auto facts = make_facts();
    auto session = take_successful_session(open_private(&policy, &facts, deployment));
    CHECK(session.has_value());
    auto begin = std::move(*session).begin_next_slot();
    CHECK(begin);
    auto active = std::move(begin).take_active_slot();
    CHECK(active.has_value());
    CHECK(active->active());
    CHECK(active->slot_number() == 1);
    return active;
}

void expect_single_synthetic_launch(const std::filesystem::path& marker) {
    if (!std::filesystem::is_directory(marker)) {
        throw std::runtime_error("missing synthetic launch marker: " + marker.string());
    }
    CHECK(read_text_file(marker / "launch.txt") == "fixture_id=1 mode=off ordinal=1\n");
}

void expect_no_runner_artifacts(const TempStore& fixture) {
    for (const auto kind : {SIQSShadowProofRssArtifactKind::probe_stdout,
                            SIQSShadowProofRssArtifactKind::probe_stderr,
                            SIQSShadowProofRssArtifactKind::joined_gate_sample}) {
        CHECK(!std::filesystem::exists(fixture.artifact_leaf(artifact_leaf(1, kind).view())));
    }
}

void expect_explicit_taint_record(const TempStore& fixture) {
    const auto leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(2);
    CHECK(leaf.has_value());
    const auto decoded =
        decode_siqs_shadow_proof_rss_campaign_journal_record(fixture.read_store_leaf(leaf->view()));
    CHECK(decoded);
    CHECK(decoded.value->kind == SIQSShadowProofRssJournalRecordKind::campaign_tainted);
    CHECK(decoded.value->slot_number == 1);
}

void test_slot_runner_contract_and_missing_deployment() {
    constexpr std::array names{
        std::pair{SlotRunnerError::none, std::string_view{"none"}},
        std::pair{SlotRunnerError::platform_unavailable, std::string_view{"platform_unavailable"}},
        std::pair{SlotRunnerError::session_inactive, std::string_view{"session_inactive"}},
        std::pair{SlotRunnerError::deployment_unavailable,
                  std::string_view{"deployment_unavailable"}},
        std::pair{SlotRunnerError::deployment_invalid, std::string_view{"deployment_invalid"}},
        std::pair{SlotRunnerError::transport_failed, std::string_view{"transport_failed"}},
        std::pair{SlotRunnerError::stream_join_failed, std::string_view{"stream_join_failed"}},
        std::pair{SlotRunnerError::artifact_publication_failed,
                  std::string_view{"artifact_publication_failed"}},
        std::pair{SlotRunnerError::commit_failed, std::string_view{"commit_failed"}},
        std::pair{SlotRunnerError::commit_outcome_uncertain,
                  std::string_view{"commit_outcome_uncertain"}},
        std::pair{SlotRunnerError::taint_failed, std::string_view{"taint_failed"}},
        std::pair{SlotRunnerError::resource_exhausted, std::string_view{"resource_exhausted"}},
        std::pair{SlotRunnerError::unexpected_failure, std::string_view{"unexpected_failure"}},
    };
    for (const auto& [error, name] : names) {
        CHECK(store_detail::slot_runner_error_name(error) == name);
    }
    CHECK(store_detail::slot_runner_error_name(static_cast<SlotRunnerError>(UINT8_C(255))) ==
          "unknown");

    TempStore fixture;
    auto deployment = make_deployment(fixture.trusted_base());
    auto active = begin_runner_slot(deployment);
    auto result = store_detail::SlotRunnerFactory::run(std::move(*active));
    CHECK(!static_cast<bool>(result));
    CHECK(result.diagnostic().error == SlotRunnerError::deployment_unavailable);
    CHECK(result.diagnostic().taint_attempted);
    CHECK(result.diagnostic().taint_durable);
    CHECK(result.view().reason == SIQSShadowProofRssJournalReason::explicitly_tainted);
    expect_no_runner_artifacts(fixture);
    expect_explicit_taint_record(fixture);
}

void test_slot_runner_rejects_invalid_deployment(const std::filesystem::path& executable) {
    const auto run_invalid = [&](std::string_view marker_name, const auto& mutate) {
        TempStore fixture;
        const auto marker = fixture.base_leaf(marker_name);
        auto deployment = make_runner_deployment(fixture, executable, marker);
        mutate(*deployment.holdout_probe);
        auto active = begin_runner_slot(deployment);

        auto result = store_detail::SlotRunnerFactory::run(std::move(*active));
        CHECK(!static_cast<bool>(result));
        CHECK(result.diagnostic().error == SlotRunnerError::deployment_invalid);
        CHECK(result.diagnostic().store_diagnostic.error == StoreError::registry_binding_mismatch);
        CHECK(result.diagnostic().store_diagnostic.object == StoreObject::deployment_registry);
        CHECK(result.diagnostic().taint_attempted);
        CHECK(result.diagnostic().taint_durable);
        CHECK(!std::filesystem::exists(marker));
        expect_no_runner_artifacts(fixture);
        expect_explicit_taint_record(fixture);
    };

    run_invalid("runner-relative-executable-marker",
                [](auto& binding) { binding.executable = "relative-probe"; });
    run_invalid("runner-revision-mismatch-marker",
                [](auto& binding) { binding.candidate_revision = "different-revision"; });
    run_invalid("runner-zero-timeout-marker",
                [](auto& binding) { binding.timeout = std::chrono::milliseconds::zero(); });
}

void test_slot_runner_happy_off_commits_one_same_child(const std::filesystem::path& executable) {
    TempStore fixture;
    const auto marker = fixture.base_leaf("runner-happy-marker");
    auto deployment = make_runner_deployment(fixture, executable, marker);
    auto active = begin_runner_slot(deployment);

    auto result = store_detail::SlotRunnerFactory::run(std::move(*active));
    CHECK(static_cast<bool>(result));
    CHECK(result.diagnostic().error == SlotRunnerError::none);
    CHECK(result.view().status == SIQSShadowProofRssJournalStatus::ready);
    CHECK(result.view().reason == SIQSShadowProofRssJournalReason::ready);
    CHECK(result.view().action == SIQSShadowProofRssJournalAction::append_slot_start);
    CHECK(result.view().committed_slot_count == 1);
    CHECK(result.view().next_slot_number == 2);
    expect_single_synthetic_launch(marker);

    const auto stdout_leaf = artifact_leaf(1, SIQSShadowProofRssArtifactKind::probe_stdout);
    const auto stderr_leaf = artifact_leaf(1, SIQSShadowProofRssArtifactKind::probe_stderr);
    const auto joined_leaf = artifact_leaf(1, SIQSShadowProofRssArtifactKind::joined_gate_sample);
    const std::string stdout_bytes =
        bytes_as_string(fixture.read_artifact_leaf(stdout_leaf.view()));
    const std::string stderr_bytes =
        bytes_as_string(fixture.read_artifact_leaf(stderr_leaf.view()));
    const std::string joined_bytes =
        bytes_as_string(fixture.read_artifact_leaf(joined_leaf.view()));
    CHECK(!stdout_bytes.empty());
    CHECK(stderr_bytes.empty());
    CHECK(!joined_bytes.empty());

    const auto commit_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(2);
    CHECK(commit_leaf.has_value());
    const auto decoded = decode_siqs_shadow_proof_rss_campaign_journal_record(
        fixture.read_store_leaf(commit_leaf->view()));
    CHECK(decoded);
    CHECK(decoded.value->kind == SIQSShadowProofRssJournalRecordKind::slot_committed);
    const auto& payload = decoded.value->commit_payload;
    CHECK(payload.actual_operating_system == make_facts().operating_system);
    CHECK(payload.actual_architecture == make_facts().architecture);
    CHECK(payload.actual_memory_backend == make_facts().memory_backend);
    CHECK(payload.actual_resolved_sieve_workers == 4);
    CHECK(payload.fresh_process);
    CHECK(payload.completed);
    CHECK(payload.factor_identity == SIQSShadowProofRssFactorIdentity::pass);
    CHECK(payload.proof_evidence == SIQSShadowProofRssEvidence::not_applicable);
    CHECK(payload.matrix_evidence == SIQSShadowProofRssEvidence::not_applicable);
    CHECK(payload.absolute_peak_rss_bytes == UINT64_C(14000000));
    CHECK(!payload.observe_minus_off_peak_bytes.has_value());
    CHECK(payload.current_rss_bytes == UINT64_C(13000000));
    CHECK(payload.peak_growth_bytes == UINT64_C(2000000));
    CHECK(payload.wall_ns == UINT64_C(7000000));
    CHECK(payload.stdout_seal == seal_siqs_shadow_proof_rss_artifact(
                                     SIQSShadowProofRssArtifactKind::probe_stdout, stdout_bytes));
    CHECK(payload.stderr_seal == seal_siqs_shadow_proof_rss_artifact(
                                     SIQSShadowProofRssArtifactKind::probe_stderr, stderr_bytes));
    CHECK(payload.joined_sample_seal ==
          seal_siqs_shadow_proof_rss_artifact(SIQSShadowProofRssArtifactKind::joined_gate_sample,
                                              joined_bytes));

    auto continuation = std::move(result).take_session();
    CHECK(continuation.has_value());
    CHECK(continuation->active());
    CHECK(continuation->view().committed_slot_count == 1);
    continuation.reset();

    const auto policy = make_policy();
    const auto facts = make_facts();
    auto reopened = take_successful_session(open_private(&policy, &facts, deployment));
    CHECK(reopened.has_value());
    CHECK(reopened->view().committed_slot_count == 1);
    CHECK(reopened->view().next_slot_number == 2);
}

void run_slot_runner_failure_case(const std::filesystem::path& executable,
                                  std::string_view marker_name, SlotRunnerError expected_error,
                                  gnfs::util::BoundedChildProcessError expected_transport_error,
                                  std::chrono::milliseconds timeout = std::chrono::seconds(2),
                                  bool expect_child_marker = true) {
    TempStore fixture;
    const auto marker = fixture.base_leaf(marker_name);
    auto deployment = make_runner_deployment(fixture, executable, marker, timeout);
    auto active = begin_runner_slot(deployment);

    auto result = store_detail::SlotRunnerFactory::run(std::move(*active));
    CHECK(!static_cast<bool>(result));
    CHECK(result.diagnostic().error == expected_error);
    CHECK(result.diagnostic().transport_error == expected_transport_error);
    CHECK(result.diagnostic().child_started);
    CHECK(result.diagnostic().cleanup_complete);
    CHECK(result.diagnostic().taint_attempted);
    CHECK(result.diagnostic().taint_durable);
    CHECK(result.view().status == SIQSShadowProofRssJournalStatus::tainted);
    CHECK(result.view().reason == SIQSShadowProofRssJournalReason::explicitly_tainted);
    CHECK(result.view().action == SIQSShadowProofRssJournalAction::none);
    if (expect_child_marker) {
        expect_single_synthetic_launch(marker);
    }
    expect_no_runner_artifacts(fixture);
    expect_explicit_taint_record(fixture);

    const auto policy = make_policy();
    const auto facts = make_facts();
    auto reopened = take_successful_session(open_private(&policy, &facts, deployment));
    CHECK(reopened.has_value());
    CHECK(reopened->view() == result.view());
}

void test_slot_runner_execution_and_join_failures(const SyntheticChildren& children) {
    run_slot_runner_failure_case(children.nonzero, "runner-nonzero-marker",
                                 SlotRunnerError::transport_failed,
                                 gnfs::util::BoundedChildProcessError::normal_nonzero);
    run_slot_runner_failure_case(children.malformed, "runner-malformed-marker",
                                 SlotRunnerError::stream_join_failed,
                                 gnfs::util::BoundedChildProcessError::none);
    run_slot_runner_failure_case(children.overflow, "runner-overflow-marker",
                                 SlotRunnerError::transport_failed,
                                 gnfs::util::BoundedChildProcessError::overflow);
    run_slot_runner_failure_case(
        children.hang, "runner-timeout-marker", SlotRunnerError::transport_failed,
        gnfs::util::BoundedChildProcessError::timeout, std::chrono::milliseconds(150), false);
}

void test_slot_runner_artifact_prefix_failure_taints(const std::filesystem::path& executable) {
    TempStore fixture;
    const auto marker = fixture.base_leaf("runner-artifact-failure-marker");
    TestPublicationOps ops(4, TestPublicationOps::Action::fail_before_create);
    auto deployment = make_runner_deployment(fixture, executable, marker);
    deployment.publication_ops = &ops;
    auto active = begin_runner_slot(deployment);

    auto result = store_detail::SlotRunnerFactory::run(std::move(*active));
    CHECK(!static_cast<bool>(result));
    CHECK(result.diagnostic().error == SlotRunnerError::artifact_publication_failed);
    CHECK(result.diagnostic().store_diagnostic.error == StoreError::publication_failed);
    CHECK(result.diagnostic().store_diagnostic.object == StoreObject::artifact);
    CHECK(result.diagnostic().store_diagnostic.artifact_kind ==
          SIQSShadowProofRssArtifactKind::probe_stderr);
    CHECK(result.diagnostic().taint_durable);
    CHECK(ops.publish_calls() == 5);
    expect_single_synthetic_launch(marker);
    CHECK(std::filesystem::exists(fixture.artifact_leaf(
        artifact_leaf(1, SIQSShadowProofRssArtifactKind::probe_stdout).view())));
    CHECK(!std::filesystem::exists(fixture.artifact_leaf(
        artifact_leaf(1, SIQSShadowProofRssArtifactKind::probe_stderr).view())));
    CHECK(!std::filesystem::exists(fixture.artifact_leaf(
        artifact_leaf(1, SIQSShadowProofRssArtifactKind::joined_gate_sample).view())));
    expect_explicit_taint_record(fixture);
}

void test_slot_runner_commit_terminal_leaf_matrix(const std::filesystem::path& executable) {
    {
        TempStore fixture;
        const auto marker = fixture.base_leaf("runner-commit-absent-marker");
        TestPublicationOps ops(6, TestPublicationOps::Action::fail_before_create);
        auto deployment = make_runner_deployment(fixture, executable, marker);
        deployment.publication_ops = &ops;
        auto active = begin_runner_slot(deployment);

        auto result = store_detail::SlotRunnerFactory::run(std::move(*active));
        CHECK(!static_cast<bool>(result));
        CHECK(result.diagnostic().error == SlotRunnerError::commit_failed);
        CHECK(result.diagnostic().taint_durable);
        CHECK(ops.publish_calls() == 7);
        expect_explicit_taint_record(fixture);
    }
    {
        TempStore fixture;
        const auto marker = fixture.base_leaf("runner-commit-salvage-marker");
        CommitPublicationOps ops(6, CommitPublicationOps::LeafShape::exact, false);
        auto deployment = make_runner_deployment(fixture, executable, marker);
        deployment.publication_ops = &ops;
        auto active = begin_runner_slot(deployment);

        auto result = store_detail::SlotRunnerFactory::run(std::move(*active));
        CHECK(static_cast<bool>(result));
        CHECK(result.view().committed_slot_count == 1);
        CHECK(ops.publish_calls() == 6);
        CHECK(ops.confirm_calls() == 1);
    }
    {
        TempStore fixture;
        const auto marker = fixture.base_leaf("runner-commit-confirm-failure-marker");
        CommitPublicationOps ops(6, CommitPublicationOps::LeafShape::exact, true);
        auto deployment = make_runner_deployment(fixture, executable, marker);
        deployment.publication_ops = &ops;
        auto active = begin_runner_slot(deployment);

        auto result = store_detail::SlotRunnerFactory::run(std::move(*active));
        CHECK(!static_cast<bool>(result));
        CHECK(result.diagnostic().error == SlotRunnerError::commit_outcome_uncertain);
        CHECK(!result.diagnostic().taint_attempted);
        CHECK(!result.diagnostic().taint_durable);
        CHECK(ops.publish_calls() == 6);
        CHECK(ops.confirm_calls() == 1);
        const auto taint_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(2);
        CHECK(taint_leaf.has_value());
        const auto decoded = decode_siqs_shadow_proof_rss_campaign_journal_record(
            fixture.read_store_leaf(taint_leaf->view()));
        CHECK(decoded);
        CHECK(decoded.value->kind == SIQSShadowProofRssJournalRecordKind::slot_committed);

        auto normal_deployment = make_runner_deployment(fixture, executable, marker);
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto reopened = take_successful_session(open_private(&policy, &facts, normal_deployment));
        CHECK(reopened.has_value());
        CHECK(reopened->view().committed_slot_count == 1);
    }
    {
        TempStore fixture;
        const auto marker = fixture.base_leaf("runner-commit-partial-marker");
        CommitPublicationOps ops(6, CommitPublicationOps::LeafShape::one_byte, false);
        auto deployment = make_runner_deployment(fixture, executable, marker);
        deployment.publication_ops = &ops;
        auto active = begin_runner_slot(deployment);

        auto result = store_detail::SlotRunnerFactory::run(std::move(*active));
        CHECK(!static_cast<bool>(result));
        CHECK(result.diagnostic().error == SlotRunnerError::commit_outcome_uncertain);
        CHECK(!result.diagnostic().taint_attempted);
        CHECK(ops.publish_calls() == 6);
        CHECK(ops.confirm_calls() == 0);
        const auto commit_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(2);
        CHECK(commit_leaf.has_value());
        CHECK(std::filesystem::file_size(fixture.store_leaf(commit_leaf->view())) == 1);

        auto normal_deployment = make_runner_deployment(fixture, executable, marker);
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto reopened = open_private(&policy, &facts, normal_deployment);
        expect_layout_error(std::move(reopened), LayoutError::record_size_invalid,
                            StoreObject::journal_record, 2);
    }
    {
        TempStore fixture;
        const auto marker = fixture.base_leaf("runner-commit-different-marker");
        CommitPublicationOps ops(6, CommitPublicationOps::LeafShape::different, false);
        auto deployment = make_runner_deployment(fixture, executable, marker);
        deployment.publication_ops = &ops;
        auto active = begin_runner_slot(deployment);

        auto result = store_detail::SlotRunnerFactory::run(std::move(*active));
        CHECK(!static_cast<bool>(result));
        CHECK(result.diagnostic().error == SlotRunnerError::commit_outcome_uncertain);
        CHECK(!result.diagnostic().taint_attempted);
        CHECK(ops.publish_calls() == 6);
        CHECK(ops.confirm_calls() == 0);
        const auto commit_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(2);
        CHECK(commit_leaf.has_value());
        CHECK(std::filesystem::file_size(fixture.store_leaf(commit_leaf->view())) ==
              SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE);

        auto normal_deployment = make_runner_deployment(fixture, executable, marker);
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto reopened = open_private(&policy, &facts, normal_deployment);
        expect_layout_error(std::move(reopened), LayoutError::record_codec_invalid,
                            StoreObject::journal_record, 2);
    }
    {
        TempStore fixture;
        const auto marker = fixture.base_leaf("runner-commit-already-exists-marker");
        CommitPublicationOps ops(6, CommitPublicationOps::LeafShape::exact_already_exists, false);
        auto deployment = make_runner_deployment(fixture, executable, marker);
        deployment.publication_ops = &ops;
        auto active = begin_runner_slot(deployment);

        auto result = store_detail::SlotRunnerFactory::run(std::move(*active));
        CHECK(!static_cast<bool>(result));
        CHECK(result.diagnostic().error == SlotRunnerError::commit_outcome_uncertain);
        CHECK(result.diagnostic().store_diagnostic.publication_status ==
              durable::PublishStatus::already_exists);
        CHECK(!result.diagnostic().taint_attempted);
        CHECK(ops.publish_calls() == 6);
        CHECK(ops.confirm_calls() == 0);

        auto normal_deployment = make_runner_deployment(fixture, executable, marker);
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto reopened = take_successful_session(open_private(&policy, &facts, normal_deployment));
        CHECK(reopened.has_value());
        CHECK(reopened->view().committed_slot_count == 1);
    }
}

void test_taint_rejects_inactive_and_nonpending_sessions() {
    {
        TempStore fixture;
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        auto rejected = std::move(*session).append_pending_taint();
        CHECK(!static_cast<bool>(rejected));
        CHECK(rejected.diagnostic().error == StoreError::session_action_invalid);
        CHECK(rejected.diagnostic().journal_reason == SIQSShadowProofRssJournalReason::ready);
        auto inactive = std::move(*session).append_pending_taint();
        CHECK(!static_cast<bool>(inactive));
        CHECK(inactive.diagnostic().error == StoreError::session_inactive);
    }
    {
        TempStore fixture;
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        auto begin = std::move(*session).begin_next_slot();
        CHECK(begin);
        auto active = std::move(begin).take_active_slot();
        CHECK(active.has_value());
        auto moved(std::move(*active));
        CHECK(!active->active());
        auto rejected = std::move(*active).taint();
        CHECK(!static_cast<bool>(rejected));
        CHECK(rejected.diagnostic().error == StoreError::session_inactive);
        auto taint = std::move(moved).taint();
        CHECK(static_cast<bool>(taint));
    }
}

void test_begin_crash_recovers_only_through_pending_taint(const std::string& executable) {
    TempStore fixture;
    const auto crash = gnfs::test::run_child_process(
        executable, {"--begin-and-crash", fixture.trusted_base().string()});
    CHECK(crash.exited);
    CHECK(!crash.signaled);
    CHECK(crash.exit_code == 74);

    auto reopened = take_successful_session(open_fixture(fixture));
    CHECK(reopened.has_value());
    CHECK(reopened->view().status == SIQSShadowProofRssJournalStatus::tainted);
    CHECK(reopened->view().reason == SIQSShadowProofRssJournalReason::dangling_slot_start);
    CHECK(reopened->view().action == SIQSShadowProofRssJournalAction::append_taint);
    auto taint = std::move(*reopened).append_pending_taint();
    CHECK(static_cast<bool>(taint));
    CHECK(taint.view().reason == SIQSShadowProofRssJournalReason::explicitly_tainted);
}

void test_artifact_root_is_required_and_private() {
    {
        TempStore fixture;
        std::error_code error;
        CHECK(std::filesystem::remove(fixture.artifact_root(), error));
        CHECK(!error);
        expect_open_error(open_fixture(fixture), StoreError::artifact_root_open_failed,
                          StoreObject::artifact_root);
    }
    {
        TempStore fixture;
        CHECK(::chmod(fixture.artifact_root().c_str(), 0750) == 0);
        expect_open_error(open_fixture(fixture), StoreError::artifact_root_invalid,
                          StoreObject::artifact_root);
    }
    {
        TempStore fixture;
        CHECK(::chmod(fixture.artifact_root().c_str(), 01700) == 0);
        expect_open_error(open_fixture(fixture), StoreError::artifact_root_invalid,
                          StoreObject::artifact_root);
    }
    {
        TempStore fixture;
        fixture.write_artifact_leaf("unexpected.rssa", "sentinel");
        auto result = open_fixture(fixture);
        CHECK(result.diagnostic().artifact_layout.error ==
              SIQSShadowProofRssCampaignArtifactLayoutError::unknown_entry);
        expect_open_error(std::move(result), StoreError::artifact_layout_invalid,
                          StoreObject::artifact);
    }
}

void test_artifact_root_identity_and_generation_are_revalidated() {
    {
        TempStore fixture;
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        const auto displaced = fixture.store_root() / ".artifacts-displaced";
        std::error_code error;
        std::filesystem::rename(fixture.artifact_root(), displaced, error);
        CHECK(!error);
        CHECK(::mkdir(fixture.artifact_root().c_str(), 0700) == 0);

        auto begin = std::move(*session).begin_next_slot();
        CHECK(!static_cast<bool>(begin));
        CHECK(begin.diagnostic().error == StoreError::snapshot_changed);
        CHECK(begin.diagnostic().object == StoreObject::artifact_root);
        CHECK(!std::filesystem::exists(
            fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF)));
    }
    {
        TempStore fixture;
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        write_artifact(fixture, 1, SIQSShadowProofRssArtifactKind::probe_stdout, "out-of-band\n");

        auto begin = std::move(*session).begin_next_slot();
        CHECK(!static_cast<bool>(begin));
        CHECK(begin.diagnostic().error == StoreError::snapshot_changed);
        CHECK(begin.diagnostic().object == StoreObject::artifact_root);
        CHECK(!std::filesystem::exists(
            fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF)));
    }
}

void test_active_slot_actions_revalidate_authority_before_publication() {
    constexpr std::string_view stdout_bytes = "synthetic-stdout\n";
    constexpr std::string_view stderr_bytes;
    constexpr std::string_view joined_bytes = "synthetic-joined\n";

    {
        TempStore fixture;
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        auto begin = std::move(*session).begin_next_slot();
        CHECK(begin);
        auto active = std::move(begin).take_active_slot();
        CHECK(active.has_value());

        const auto displaced = fixture.store_root() / ".artifacts-displaced";
        std::error_code error;
        std::filesystem::rename(fixture.artifact_root(), displaced, error);
        CHECK(!error);
        CHECK(::mkdir(fixture.artifact_root().c_str(), 0700) == 0);

        const auto publication = store_detail::SessionFactory::publish_artifact_batch(
            *active, stdout_bytes, stderr_bytes, joined_bytes);
        CHECK(!static_cast<bool>(publication));
        CHECK(publication.diagnostic.error == StoreError::snapshot_changed);
        CHECK(publication.diagnostic.object == StoreObject::artifact_root);
        for (const auto kind : {SIQSShadowProofRssArtifactKind::probe_stdout,
                                SIQSShadowProofRssArtifactKind::probe_stderr,
                                SIQSShadowProofRssArtifactKind::joined_gate_sample}) {
            const auto leaf = artifact_leaf(1, kind);
            CHECK(!std::filesystem::exists(displaced / std::string(leaf.view())));
            CHECK(!std::filesystem::exists(fixture.artifact_leaf(leaf.view())));
        }
    }
    {
        TempStore fixture;
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        auto begin = std::move(*session).begin_next_slot();
        CHECK(begin);
        auto active = std::move(begin).take_active_slot();
        CHECK(active.has_value());
        fixture.write_artifact_leaf("unexpected.rssa", "sentinel");

        const auto taint = std::move(*active).taint();
        CHECK(!static_cast<bool>(taint));
        CHECK(taint.diagnostic().error == StoreError::snapshot_changed);
        CHECK(taint.diagnostic().object == StoreObject::artifact_root);
        const auto taint_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(2);
        CHECK(taint_leaf.has_value());
        CHECK(!std::filesystem::exists(fixture.store_leaf(taint_leaf->view())));
    }
    {
        TempStore fixture;
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        auto begin = std::move(*session).begin_next_slot();
        CHECK(begin);
        auto active = std::move(begin).take_active_slot();
        CHECK(active.has_value());

        const auto lock =
            fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF);
        std::error_code error;
        CHECK(std::filesystem::remove(lock, error));
        CHECK(!error);
        fixture.write_store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF, {});

        const auto taint = std::move(*active).taint();
        CHECK(!static_cast<bool>(taint));
        CHECK(taint.diagnostic().error == StoreError::lock_invalid);
        CHECK(taint.diagnostic().object == StoreObject::session_lock);
        const auto taint_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(2);
        CHECK(taint_leaf.has_value());
        CHECK(!std::filesystem::exists(fixture.store_leaf(taint_leaf->view())));
    }
}

void test_private_artifact_batch_publisher_preserves_closed_commit_boundary(
    const std::string& executable) {
    constexpr std::string_view stdout_bytes = "synthetic-stdout\n";
    constexpr std::string_view stderr_bytes;
    constexpr std::string_view joined_bytes = "synthetic-joined\n";

    TempStore fixture;
    auto session = take_successful_session(open_fixture(fixture));
    CHECK(session.has_value());
    auto begin = std::move(*session).begin_next_slot();
    CHECK(begin);
    auto active = std::move(begin).take_active_slot();
    CHECK(active.has_value());

    const auto publication = store_detail::SessionFactory::publish_artifact_batch(
        *active, stdout_bytes, stderr_bytes, joined_bytes);
    if (!publication) {
        std::cerr << "artifact batch failed: "
                  << siqs_shadow_proof_rss_campaign_journal_store_error_name(
                         publication.diagnostic.error)
                  << '/'
                  << siqs_shadow_proof_rss_campaign_journal_store_object_name(
                         publication.diagnostic.object)
                  << " native=" << publication.diagnostic.native_error.value() << " layout="
                  << siqs_shadow_proof_rss_campaign_artifact_layout_error_name(
                         publication.diagnostic.artifact_layout.error)
                  << " consistency="
                  << siqs_shadow_proof_rss_campaign_artifact_consistency_error_name(
                         publication.diagnostic.artifact_consistency.error)
                  << '\n';
    }
    CHECK(static_cast<bool>(publication));
    CHECK(publication.seals[0] == seal_siqs_shadow_proof_rss_artifact(
                                      SIQSShadowProofRssArtifactKind::probe_stdout, stdout_bytes));
    CHECK(publication.seals[1] == seal_siqs_shadow_proof_rss_artifact(
                                      SIQSShadowProofRssArtifactKind::probe_stderr, stderr_bytes));
    CHECK(publication.seals[2] ==
          seal_siqs_shadow_proof_rss_artifact(SIQSShadowProofRssArtifactKind::joined_gate_sample,
                                              joined_bytes));
    CHECK(active->active());
    CHECK(active->view().reason == SIQSShadowProofRssJournalReason::dangling_slot_start);
    const auto duplicate = store_detail::SessionFactory::publish_artifact_batch(
        *active, stdout_bytes, stderr_bytes, joined_bytes);
    CHECK(!static_cast<bool>(duplicate));
    CHECK(duplicate.diagnostic.error == StoreError::session_action_invalid);

    const std::array kinds{
        SIQSShadowProofRssArtifactKind::probe_stdout,
        SIQSShadowProofRssArtifactKind::probe_stderr,
        SIQSShadowProofRssArtifactKind::joined_gate_sample,
    };
    const std::array expected_bytes{stdout_bytes, stderr_bytes, joined_bytes};
    for (std::size_t index = 0; index < kinds.size(); ++index) {
        const auto leaf = artifact_leaf(1, kinds[index]);
        expect_private_regular_leaf(fixture.artifact_leaf(leaf.view()),
                                    expected_bytes[index].size());
        const auto expected_span = std::as_bytes(
            std::span<const char>(expected_bytes[index].data(), expected_bytes[index].size()));
        CHECK(fixture.read_artifact_leaf(leaf.view()) ==
              std::vector<std::byte>(expected_span.begin(), expected_span.end()));
    }

    const auto busy = gnfs::test::run_child_process(
        executable, {"--expect-lock-busy", fixture.trusted_base().string()});
    CHECK(busy.exited);
    CHECK(!busy.signaled);
    CHECK(busy.exit_code == 0);

    // Publishing durable artifacts deliberately does not create a public
    // commit authority. The only currently exposed closure is terminal taint.
    const auto taint = std::move(*active).taint();
    CHECK(static_cast<bool>(taint));
    auto reopened = take_successful_session(open_fixture(fixture));
    CHECK(reopened.has_value());
    CHECK(reopened->view().reason == SIQSShadowProofRssJournalReason::explicitly_tainted);
}

void test_artifact_batch_validates_mode_and_bounds_before_publication() {
    constexpr std::string_view stdout_bytes = "synthetic-stdout\n";
    constexpr std::string_view joined_bytes = "synthetic-joined\n";

    {
        TempStore fixture;
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        auto begin = std::move(*session).begin_next_slot();
        CHECK(begin);
        auto active = std::move(begin).take_active_slot();
        CHECK(active.has_value());

        const std::string oversized_stdout(
            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDOUT_MAX_BYTES + 1, 's');
        const std::string oversized_stderr(
            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDERR_MAX_BYTES + 1, 'e');
        const std::string oversized_joined(
            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_JOINED_MAX_BYTES + 1, 'j');
        const auto expect_rejected = [&](std::string_view probe_stdout,
                                         std::string_view probe_stderr, std::string_view joined) {
            const auto publication = store_detail::SessionFactory::publish_artifact_batch(
                *active, probe_stdout, probe_stderr, joined);
            CHECK(!static_cast<bool>(publication));
            CHECK(publication.diagnostic.error == StoreError::receipt_rejected);
            CHECK(publication.diagnostic.object == StoreObject::artifact);
            CHECK(!publication.diagnostic.last_durable_publication_object.has_value());
        };
        expect_rejected("", "", joined_bytes);
        expect_rejected(stdout_bytes, "off-mode-stderr\n", joined_bytes);
        expect_rejected(stdout_bytes, "", "");
        expect_rejected(oversized_stdout, "", joined_bytes);
        expect_rejected(stdout_bytes, oversized_stderr, joined_bytes);
        expect_rejected(stdout_bytes, "", oversized_joined);
        for (const auto kind : {SIQSShadowProofRssArtifactKind::probe_stdout,
                                SIQSShadowProofRssArtifactKind::probe_stderr,
                                SIQSShadowProofRssArtifactKind::joined_gate_sample}) {
            CHECK(!std::filesystem::exists(fixture.artifact_leaf(artifact_leaf(1, kind).view())));
        }
        const auto taint = std::move(*active).taint();
        CHECK(static_cast<bool>(taint));
    }
    {
        constexpr std::string_view off_stdout = "committed-off-stdout\n";
        constexpr std::string_view off_stderr;
        constexpr std::string_view off_joined = "committed-off-joined\n";
        constexpr std::string_view observe_stderr = "synthetic-observe-stderr\n";

        TempStore fixture;
        write_committed_off_slots(fixture, 3, off_stdout, off_stderr, off_joined);
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        CHECK(session->view().next_slot_number == 4);
        auto begin = std::move(*session).begin_next_slot();
        CHECK(begin);
        auto active = std::move(begin).take_active_slot();
        CHECK(active.has_value());

        const auto rejected = store_detail::SessionFactory::publish_artifact_batch(
            *active, stdout_bytes, "", joined_bytes);
        CHECK(!static_cast<bool>(rejected));
        CHECK(rejected.diagnostic.error == StoreError::receipt_rejected);
        for (const auto kind : {SIQSShadowProofRssArtifactKind::probe_stdout,
                                SIQSShadowProofRssArtifactKind::probe_stderr,
                                SIQSShadowProofRssArtifactKind::joined_gate_sample}) {
            CHECK(!std::filesystem::exists(fixture.artifact_leaf(artifact_leaf(4, kind).view())));
        }

        const auto publication = store_detail::SessionFactory::publish_artifact_batch(
            *active, stdout_bytes, observe_stderr, joined_bytes);
        CHECK(static_cast<bool>(publication));
        CHECK(publication.seals[1] ==
              seal_siqs_shadow_proof_rss_artifact(SIQSShadowProofRssArtifactKind::probe_stderr,
                                                  observe_stderr));
        const auto taint = std::move(*active).taint();
        CHECK(static_cast<bool>(taint));
    }
}

void test_artifact_publication_failures_preserve_prefix_and_trace() {
    constexpr std::string_view stdout_bytes = "synthetic-stdout\n";
    constexpr std::string_view stderr_bytes;
    constexpr std::string_view joined_bytes = "synthetic-joined\n";

    {
        TempStore fixture;
        TestPublicationOps ops(4, TestPublicationOps::Action::fail_before_create);
        auto deployment = make_deployment(fixture.trusted_base());
        deployment.publication_ops = &ops;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto session = take_successful_session(open_private(&policy, &facts, deployment));
        CHECK(session.has_value());
        auto begin = std::move(*session).begin_next_slot();
        CHECK(begin);
        auto active = std::move(begin).take_active_slot();
        CHECK(active.has_value());

        const auto publication = store_detail::SessionFactory::publish_artifact_batch(
            *active, stdout_bytes, stderr_bytes, joined_bytes);
        CHECK(!static_cast<bool>(publication));
        CHECK(ops.publish_calls() == 4);
        CHECK(publication.diagnostic.error == StoreError::publication_failed);
        CHECK(publication.diagnostic.object == StoreObject::artifact);
        CHECK(publication.diagnostic.artifact_slot_number == 1);
        CHECK(publication.diagnostic.artifact_kind == SIQSShadowProofRssArtifactKind::probe_stderr);
        CHECK(publication.diagnostic.last_durable_publication_object == StoreObject::artifact);
        CHECK(publication.diagnostic.last_durable_artifact_slot_number == 1);
        CHECK(publication.diagnostic.last_durable_artifact_kind ==
              SIQSShadowProofRssArtifactKind::probe_stdout);
        CHECK(publication.diagnostic.last_durable_publication_bytes_written == stdout_bytes.size());
        CHECK(std::filesystem::exists(fixture.artifact_leaf(
            artifact_leaf(1, SIQSShadowProofRssArtifactKind::probe_stdout).view())));
        CHECK(!std::filesystem::exists(fixture.artifact_leaf(
            artifact_leaf(1, SIQSShadowProofRssArtifactKind::probe_stderr).view())));
        const auto taint = std::move(*active).taint();
        CHECK(static_cast<bool>(taint));
    }
    {
        TempStore fixture;
        TestPublicationOps ops(3, TestPublicationOps::Action::report_durable_without_file);
        auto deployment = make_deployment(fixture.trusted_base());
        deployment.publication_ops = &ops;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto session = take_successful_session(open_private(&policy, &facts, deployment));
        CHECK(session.has_value());
        auto begin = std::move(*session).begin_next_slot();
        CHECK(begin);
        auto active = std::move(begin).take_active_slot();
        CHECK(active.has_value());

        const auto publication = store_detail::SessionFactory::publish_artifact_batch(
            *active, stdout_bytes, stderr_bytes, joined_bytes);
        CHECK(!static_cast<bool>(publication));
        CHECK(publication.diagnostic.error == StoreError::snapshot_changed);
        CHECK(publication.diagnostic.object == StoreObject::artifact);
        CHECK(publication.diagnostic.last_durable_artifact_kind ==
              SIQSShadowProofRssArtifactKind::probe_stdout);
        CHECK(!std::filesystem::exists(fixture.artifact_leaf(
            artifact_leaf(1, SIQSShadowProofRssArtifactKind::probe_stdout).view())));
        const auto taint = std::move(*active).taint();
        CHECK(static_cast<bool>(taint));
    }
    {
        TempStore fixture;
        {
            TestPublicationOps ops(5, TestPublicationOps::Action::fail_before_create);
            auto deployment = make_deployment(fixture.trusted_base());
            deployment.publication_ops = &ops;
            const auto policy = make_policy();
            const auto facts = make_facts();
            auto session = take_successful_session(open_private(&policy, &facts, deployment));
            CHECK(session.has_value());
            auto begin = std::move(*session).begin_next_slot();
            CHECK(begin);
            auto active = std::move(begin).take_active_slot();
            CHECK(active.has_value());

            const auto publication = store_detail::SessionFactory::publish_artifact_batch(
                *active, stdout_bytes, stderr_bytes, joined_bytes);
            CHECK(!static_cast<bool>(publication));
            CHECK(ops.publish_calls() == 5);
            CHECK(publication.diagnostic.error == StoreError::publication_failed);
            CHECK(publication.diagnostic.object == StoreObject::artifact);
            CHECK(publication.diagnostic.artifact_slot_number == 1);
            CHECK(publication.diagnostic.artifact_kind ==
                  SIQSShadowProofRssArtifactKind::joined_gate_sample);
            CHECK(publication.diagnostic.last_durable_artifact_slot_number == 1);
            CHECK(publication.diagnostic.last_durable_artifact_kind ==
                  SIQSShadowProofRssArtifactKind::probe_stderr);
            CHECK(publication.diagnostic.last_durable_publication_bytes_written == 0);
            CHECK(std::filesystem::exists(fixture.artifact_leaf(
                artifact_leaf(1, SIQSShadowProofRssArtifactKind::probe_stdout).view())));
            CHECK(std::filesystem::exists(fixture.artifact_leaf(
                artifact_leaf(1, SIQSShadowProofRssArtifactKind::probe_stderr).view())));
            CHECK(!std::filesystem::exists(fixture.artifact_leaf(
                artifact_leaf(1, SIQSShadowProofRssArtifactKind::joined_gate_sample).view())));
        }

        auto reopened = take_successful_session(open_fixture(fixture));
        CHECK(reopened.has_value());
        CHECK(reopened->view().reason == SIQSShadowProofRssJournalReason::dangling_slot_start);
        CHECK(reopened->view().action == SIQSShadowProofRssJournalAction::append_taint);
        const auto taint = std::move(*reopened).append_pending_taint();
        CHECK(static_cast<bool>(taint));
        CHECK(taint.view().reason == SIQSShadowProofRssJournalReason::explicitly_tainted);
    }
}

void test_artifact_batch_crash_recovers_only_to_taint(const std::string& executable) {
    TempStore fixture;
    const auto crash = gnfs::test::run_child_process(
        executable, {"--publish-artifacts-and-crash", fixture.trusted_base().string()});
    CHECK(crash.exited);
    CHECK(!crash.signaled);
    CHECK(crash.exit_code == 75);

    auto reopened = take_successful_session(open_fixture(fixture));
    CHECK(reopened.has_value());
    CHECK(reopened->view().reason == SIQSShadowProofRssJournalReason::dangling_slot_start);
    CHECK(reopened->view().action == SIQSShadowProofRssJournalAction::append_taint);
    const auto taint = std::move(*reopened).append_pending_taint();
    CHECK(static_cast<bool>(taint));
}

void test_reopen_closes_artifacts_against_journal() {
    constexpr std::string_view stdout_bytes = "synthetic-stdout-record\n";
    constexpr std::string_view stderr_bytes;
    constexpr std::string_view joined_bytes = "synthetic-joined-sample\n";

    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        const auto start = canonical_start(header);
        write_record(fixture, 1, start);
        write_artifact(fixture, 1, SIQSShadowProofRssArtifactKind::probe_stdout, stdout_bytes);
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        CHECK(session->view().action == SIQSShadowProofRssJournalAction::append_taint);
    }
    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        write_record(fixture, 1, canonical_start(header));
        write_artifact(fixture, 2, SIQSShadowProofRssArtifactKind::probe_stdout, stdout_bytes);
        auto result = open_fixture(fixture);
        CHECK(result.diagnostic().artifact_consistency.error ==
              SIQSShadowProofRssCampaignArtifactConsistencyError::unexpected_artifact);
        expect_open_error(std::move(result), StoreError::artifact_consistency_invalid,
                          StoreObject::artifact);
    }
    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        const auto start = canonical_start(header);
        write_record(fixture, 1, start);
        const auto payload = make_off_payload(stdout_bytes, stderr_bytes, joined_bytes);
        write_record(fixture, 2, make_commit(start, payload));
        write_artifact(fixture, 1, SIQSShadowProofRssArtifactKind::probe_stdout, stdout_bytes);
        write_artifact(fixture, 1, SIQSShadowProofRssArtifactKind::probe_stderr, stderr_bytes);
        write_artifact(fixture, 1, SIQSShadowProofRssArtifactKind::joined_gate_sample,
                       joined_bytes);
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        CHECK(session->view().status == SIQSShadowProofRssJournalStatus::ready);
        CHECK(session->view().committed_slot_count == 1);
        CHECK(session->view().next_slot_number == 2);
    }
    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        const auto start = canonical_start(header);
        write_record(fixture, 1, start);
        const auto payload = make_off_payload(stdout_bytes, stderr_bytes, joined_bytes);
        write_record(fixture, 2, make_commit(start, payload));
        write_artifact(fixture, 1, SIQSShadowProofRssArtifactKind::probe_stdout,
                       "tampered-stdout\n");
        write_artifact(fixture, 1, SIQSShadowProofRssArtifactKind::probe_stderr, stderr_bytes);
        write_artifact(fixture, 1, SIQSShadowProofRssArtifactKind::joined_gate_sample,
                       joined_bytes);
        auto result = open_fixture(fixture);
        CHECK(result.diagnostic().artifact_consistency.error ==
              SIQSShadowProofRssCampaignArtifactConsistencyError::committed_artifact_mismatch);
        expect_open_error(std::move(result), StoreError::artifact_consistency_invalid,
                          StoreObject::artifact);
    }
}

void test_artifact_leaf_trust_and_shape_fail_closed() {
    constexpr std::string_view stdout_bytes = "synthetic-stdout\n";
    const auto stdout_leaf = artifact_leaf(1, SIQSShadowProofRssArtifactKind::probe_stdout);
    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        write_record(fixture, 1, canonical_start(header));
        write_artifact(fixture, 1, SIQSShadowProofRssArtifactKind::probe_stdout, stdout_bytes);
        CHECK(::chmod(fixture.artifact_leaf(stdout_leaf.view()).c_str(), 0640) == 0);
        expect_open_error(open_fixture(fixture), StoreError::entry_trust_invalid,
                          StoreObject::artifact);
    }
    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        write_record(fixture, 1, canonical_start(header));
        write_artifact(fixture, 1, SIQSShadowProofRssArtifactKind::probe_stdout, stdout_bytes);
        CHECK(::chmod(fixture.artifact_leaf(stdout_leaf.view()).c_str(), 04600) == 0);
        expect_open_error(open_fixture(fixture), StoreError::entry_trust_invalid,
                          StoreObject::artifact);
    }
    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        write_record(fixture, 1, canonical_start(header));
        const auto target = fixture.base_leaf("artifact-symlink-target");
        fixture.write_base_leaf("artifact-symlink-target", {});
        CHECK(::symlink(target.c_str(), fixture.artifact_leaf(stdout_leaf.view()).c_str()) == 0);
        auto result = open_fixture(fixture);
        CHECK(result.diagnostic().artifact_layout.error ==
              SIQSShadowProofRssCampaignArtifactLayoutError::entry_not_regular_file);
        expect_open_error(std::move(result), StoreError::artifact_layout_invalid,
                          StoreObject::artifact);
    }
    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        write_record(fixture, 1, canonical_start(header));
        fixture.write_base_leaf(
            "artifact-hardlink-target",
            std::as_bytes(std::span<const char>(stdout_bytes.data(), stdout_bytes.size())));
        CHECK(::link(fixture.base_leaf("artifact-hardlink-target").c_str(),
                     fixture.artifact_leaf(stdout_leaf.view()).c_str()) == 0);
        auto result = open_fixture(fixture);
        CHECK(result.diagnostic().artifact_layout.error ==
              SIQSShadowProofRssCampaignArtifactLayoutError::link_count_invalid);
        expect_open_error(std::move(result), StoreError::artifact_layout_invalid,
                          StoreObject::artifact);
    }
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

void test_empty_store_begins_one_lease_bound_slot(const std::string& executable) {
    TempStore fixture;
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto expected_absent = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::absent, nullptr, {});
    CHECK(expected_absent.header_to_create.has_value());
    const auto expected_header = *expected_absent.header_to_create;
    const auto expected_start = canonical_start(expected_header);

    auto session = take_successful_session(open_fixture(fixture));
    CHECK(session.has_value());
    CHECK(session->view().action == SIQSShadowProofRssJournalAction::create_header);

    auto begin = std::move(*session).begin_next_slot();
    CHECK(!session->active());
    if (!begin) {
        std::cerr
            << "empty-store begin failed: "
            << siqs_shadow_proof_rss_campaign_journal_store_error_name(begin.diagnostic().error)
            << '/'
            << siqs_shadow_proof_rss_campaign_journal_store_object_name(begin.diagnostic().object)
            << " native=" << begin.diagnostic().native_error.value();
        if (begin.diagnostic().journal_reason.has_value()) {
            std::cerr << " journal_reason=" << static_cast<int>(*begin.diagnostic().journal_reason);
        }
        std::cerr << '\n';
    }
    CHECK(static_cast<bool>(begin));
    CHECK(begin.diagnostic().error == StoreError::none);
    auto active = std::move(begin).take_active_slot();
    CHECK(active.has_value());
    CHECK(active->active());
    CHECK(active->slot_number() == 1);
    CHECK(active->view().status == SIQSShadowProofRssJournalStatus::tainted);
    CHECK(active->view().reason == SIQSShadowProofRssJournalReason::dangling_slot_start);
    CHECK(active->view().action == SIQSShadowProofRssJournalAction::append_taint);
    CHECK(!static_cast<bool>(begin));

    const auto header_path = fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF);
    const auto record_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(1);
    CHECK(record_leaf.has_value());
    const auto record_path = fixture.store_leaf(record_leaf->view());
    expect_private_regular_leaf(header_path,
                                SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE);
    expect_private_regular_leaf(record_path,
                                SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE);

    const auto header_bytes =
        fixture.read_store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF);
    const auto decoded_header = decode_siqs_shadow_proof_rss_campaign_journal_header(header_bytes);
    CHECK(decoded_header);
    CHECK(decoded_header.value == expected_header);
    const auto record_bytes = fixture.read_store_leaf(record_leaf->view());
    const auto decoded_record = decode_siqs_shadow_proof_rss_campaign_journal_record(record_bytes);
    CHECK(decoded_record);
    CHECK(decoded_record.value == expected_start);

    const auto busy = gnfs::test::run_child_process(
        executable, {"--expect-lock-busy", fixture.trusted_base().string()});
    CHECK(busy.exited);
    CHECK(!busy.signaled);
    CHECK(busy.exit_code == 0);

    std::optional<SIQSShadowProofRssCampaignJournalActiveSlot> moved(std::move(*active));
    CHECK(!active->active());
    CHECK(moved->active());
    CHECK(moved->slot_number() == 1);
    active.reset();
    moved.reset();

    auto reopened = take_successful_session(open_fixture(fixture));
    CHECK(reopened.has_value());
    CHECK(reopened->view().status == SIQSShadowProofRssJournalStatus::tainted);
    CHECK(reopened->view().reason == SIQSShadowProofRssJournalReason::dangling_slot_start);
    CHECK(reopened->view().action == SIQSShadowProofRssJournalAction::append_taint);
}

void test_header_only_store_begins_slot_without_replacing_header() {
    TempStore fixture;
    const auto header = write_canonical_header(fixture);
    const auto original_header =
        fixture.read_store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF);
    auto session = take_successful_session(open_fixture(fixture));
    CHECK(session.has_value());
    CHECK(session->view().action == SIQSShadowProofRssJournalAction::append_slot_start);

    auto begin = std::move(*session).begin_next_slot();
    CHECK(begin);
    auto active = std::move(begin).take_active_slot();
    CHECK(active.has_value());
    CHECK(active->active());
    CHECK(active->slot_number() == 1);
    CHECK(fixture.read_store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF) ==
          original_header);

    const auto record_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(1);
    CHECK(record_leaf.has_value());
    const auto decoded = decode_siqs_shadow_proof_rss_campaign_journal_record(
        fixture.read_store_leaf(record_leaf->view()));
    CHECK(decoded);
    CHECK(decoded.value == canonical_start(header));
}

void test_store_publication_injection_preserves_durable_trace() {
    {
        TempStore fixture;
        TestPublicationOps ops(2, TestPublicationOps::Action::fail_before_create);
        auto deployment = make_deployment(fixture.trusted_base());
        deployment.publication_ops = &ops;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto session = take_successful_session(open_private(&policy, &facts, deployment));
        CHECK(session.has_value());
        auto begin = std::move(*session).begin_next_slot();
        CHECK(!static_cast<bool>(begin));
        CHECK(ops.publish_calls() == 2);
        CHECK(begin.diagnostic().error == StoreError::publication_failed);
        CHECK(begin.diagnostic().object == StoreObject::journal_record);
        CHECK(begin.diagnostic().publication_status == durable::PublishStatus::open_failed);
        CHECK(begin.diagnostic().publication_bytes_written == 0);
        CHECK(begin.diagnostic().last_durable_publication_object == StoreObject::journal_header);
        CHECK(begin.diagnostic().last_durable_publication_record_sequence ==
              SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE);
        CHECK(begin.diagnostic().last_durable_publication_bytes_written ==
              SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE);
        CHECK(std::filesystem::exists(
            fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF)));
        const auto start_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(1);
        CHECK(start_leaf.has_value());
        CHECK(!std::filesystem::exists(fixture.store_leaf(start_leaf->view())));
    }
    {
        TempStore fixture;
        TestPublicationOps ops(1, TestPublicationOps::Action::report_durable_without_file);
        auto deployment = make_deployment(fixture.trusted_base());
        deployment.publication_ops = &ops;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto session = take_successful_session(open_private(&policy, &facts, deployment));
        CHECK(session.has_value());
        auto begin = std::move(*session).begin_next_slot();
        CHECK(!static_cast<bool>(begin));
        CHECK(ops.publish_calls() == 1);
        CHECK(begin.diagnostic().error == StoreError::snapshot_changed);
        CHECK(begin.diagnostic().object == StoreObject::journal_header);
        CHECK(begin.diagnostic().last_durable_publication_object == StoreObject::journal_header);
        CHECK(begin.diagnostic().last_durable_publication_bytes_written ==
              SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE);
        CHECK(!std::filesystem::exists(
            fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF)));
    }
}

void test_taint_publication_injection_preserves_recovery() {
    {
        TempStore fixture;
        TestPublicationOps ops(3, TestPublicationOps::Action::fail_before_create);
        auto deployment = make_deployment(fixture.trusted_base());
        deployment.publication_ops = &ops;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto session = take_successful_session(open_private(&policy, &facts, deployment));
        CHECK(session.has_value());
        auto begin = std::move(*session).begin_next_slot();
        CHECK(begin);
        auto active = std::move(begin).take_active_slot();
        CHECK(active.has_value());

        const auto taint = std::move(*active).taint();
        CHECK(!static_cast<bool>(taint));
        CHECK(ops.publish_calls() == 3);
        CHECK(taint.diagnostic().error == StoreError::publication_failed);
        CHECK(taint.diagnostic().object == StoreObject::journal_record);
        CHECK(taint.diagnostic().record_sequence == 2);
        CHECK(taint.diagnostic().publication_status == durable::PublishStatus::open_failed);
        CHECK(taint.diagnostic().publication_bytes_written == 0);
        CHECK(!taint.diagnostic().last_durable_publication_object.has_value());
        const auto taint_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(2);
        CHECK(taint_leaf.has_value());
        CHECK(!std::filesystem::exists(fixture.store_leaf(taint_leaf->view())));

        auto reopened = take_successful_session(open_fixture(fixture));
        CHECK(reopened.has_value());
        CHECK(reopened->view().action == SIQSShadowProofRssJournalAction::append_taint);
        const auto recovered = std::move(*reopened).append_pending_taint();
        CHECK(static_cast<bool>(recovered));
        CHECK(recovered.view().reason == SIQSShadowProofRssJournalReason::explicitly_tainted);
    }
    {
        TempStore fixture;
        TestPublicationOps ops(3, TestPublicationOps::Action::report_durable_without_file);
        auto deployment = make_deployment(fixture.trusted_base());
        deployment.publication_ops = &ops;
        const auto policy = make_policy();
        const auto facts = make_facts();
        auto session = take_successful_session(open_private(&policy, &facts, deployment));
        CHECK(session.has_value());
        auto begin = std::move(*session).begin_next_slot();
        CHECK(begin);
        auto active = std::move(begin).take_active_slot();
        CHECK(active.has_value());

        const auto taint = std::move(*active).taint();
        CHECK(!static_cast<bool>(taint));
        CHECK(ops.publish_calls() == 3);
        CHECK(taint.diagnostic().error == StoreError::snapshot_changed);
        CHECK(taint.diagnostic().object == StoreObject::journal_record);
        CHECK(taint.diagnostic().last_durable_publication_object == StoreObject::journal_record);
        CHECK(taint.diagnostic().last_durable_publication_record_sequence == 2);
        CHECK(taint.diagnostic().last_durable_publication_bytes_written ==
              SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE);
        const auto taint_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(2);
        CHECK(taint_leaf.has_value());
        CHECK(!std::filesystem::exists(fixture.store_leaf(taint_leaf->view())));

        auto reopened = take_successful_session(open_fixture(fixture));
        CHECK(reopened.has_value());
        const auto recovered = std::move(*reopened).append_pending_taint();
        CHECK(static_cast<bool>(recovered));
        CHECK(recovered.view().reason == SIQSShadowProofRssJournalReason::explicitly_tainted);
    }
}

void test_begin_rejects_inactive_and_nonready_sessions() {
    {
        TempStore fixture;
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        auto first = std::move(*session).begin_next_slot();
        CHECK(first);
        auto inactive = std::move(*session).begin_next_slot();
        expect_begin_error(std::move(inactive), StoreError::session_inactive, StoreObject::none);
    }
    {
        TempStore fixture;
        const auto header = write_canonical_header(fixture);
        write_record(fixture, 1, canonical_start(header));
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        CHECK(session->view().status == SIQSShadowProofRssJournalStatus::tainted);
        auto begin = std::move(*session).begin_next_slot();
        CHECK(begin.diagnostic().journal_reason ==
              SIQSShadowProofRssJournalReason::dangling_slot_start);
        expect_begin_error(std::move(begin), StoreError::session_action_invalid, StoreObject::none);
    }
}

void test_begin_rejects_prepublication_namespace_changes() {
    {
        TempStore fixture;
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        fixture.write_store_leaf("unregistered-leaf", {});
        auto begin = std::move(*session).begin_next_slot();
        expect_prepublication_namespace_change(std::move(begin));
        CHECK(!std::filesystem::exists(
            fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF)));
    }
    {
        TempStore fixture;
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        std::array<std::byte, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE> sentinel{};
        sentinel.front() = std::byte{0x5a};
        fixture.write_store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF, sentinel);
        auto begin = std::move(*session).begin_next_slot();
        expect_prepublication_namespace_change(std::move(begin));
        CHECK(fixture.read_store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF) ==
              std::vector<std::byte>(sentinel.begin(), sentinel.end()));
    }
    {
        TempStore fixture;
        (void)write_canonical_header(fixture);
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        const auto record_leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(1);
        CHECK(record_leaf.has_value());
        std::array<std::byte, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE> sentinel{};
        sentinel.front() = std::byte{0xa5};
        fixture.write_store_leaf(record_leaf->view(), sentinel);
        auto begin = std::move(*session).begin_next_slot();
        expect_prepublication_namespace_change(std::move(begin));
        CHECK(fixture.read_store_leaf(record_leaf->view()) ==
              std::vector<std::byte>(sentinel.begin(), sentinel.end()));
    }
    {
        TempStore fixture;
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        const auto target = fixture.base_leaf("header-symlink-target");
        std::array<std::byte, 3> protected_bytes{std::byte{1}, std::byte{2}, std::byte{3}};
        fixture.write_base_leaf("header-symlink-target", protected_bytes);
        CHECK(::symlink(
                  target.c_str(),
                  fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF).c_str()) ==
              0);
        auto begin = std::move(*session).begin_next_slot();
        expect_prepublication_namespace_change(std::move(begin));
        std::ifstream stream(target, std::ios::binary);
        std::vector<std::byte> observed;
        for (std::istreambuf_iterator<char> it(stream), end; it != end; ++it) {
            observed.push_back(static_cast<std::byte>(static_cast<unsigned char>(*it)));
        }
        CHECK(observed == std::vector<std::byte>(protected_bytes.begin(), protected_bytes.end()));
    }
}

void test_begin_revalidates_root_and_lock_before_publication() {
    {
        TempStore fixture;
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        const auto displaced = fixture.base_leaf("displaced-store-root");
        std::error_code rename_error;
        std::filesystem::rename(fixture.store_root(), displaced, rename_error);
        CHECK(!rename_error);
        CHECK(::mkdir(fixture.store_root().c_str(), 0700) == 0);

        auto begin = std::move(*session).begin_next_slot();
        expect_begin_error(std::move(begin), StoreError::snapshot_changed, StoreObject::directory);
        CHECK(!std::filesystem::exists(
            displaced / std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF)));
        CHECK(!std::filesystem::exists(
            fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF)));
    }
    {
        TempStore fixture;
        auto session = take_successful_session(open_fixture(fixture));
        CHECK(session.has_value());
        const auto lock =
            fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF);
        CHECK(::unlink(lock.c_str()) == 0);
        fixture.write_store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF, {});

        auto begin = std::move(*session).begin_next_slot();
        expect_begin_error(std::move(begin), StoreError::lock_invalid, StoreObject::session_lock);
        CHECK(!std::filesystem::exists(
            fixture.store_leaf(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF)));
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
    if (mode == "--begin-and-crash") {
        if (!result) {
            return 72;
        }
        auto session = std::move(result).take_session();
        if (!session.has_value() || !session->active()) {
            return 72;
        }
        auto begin = std::move(*session).begin_next_slot();
        if (!begin) {
            return 72;
        }
        auto active = std::move(begin).take_active_slot();
        if (!active.has_value() || !active->active()) {
            return 72;
        }
        std::_Exit(74);
    }
    if (mode == "--publish-artifacts-and-crash") {
        if (!result) {
            return 72;
        }
        auto session = std::move(result).take_session();
        if (!session.has_value() || !session->active()) {
            return 72;
        }
        auto begin = std::move(*session).begin_next_slot();
        if (!begin) {
            return 72;
        }
        auto active = std::move(begin).take_active_slot();
        if (!active.has_value() || !active->active()) {
            return 72;
        }
        const auto publication = store_detail::SessionFactory::publish_artifact_batch(
            *active, "synthetic-stdout\n", "", "synthetic-joined\n");
        if (!publication) {
            return 72;
        }
        std::_Exit(75);
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
                      std::string_view(argv[1]) == "--open-and-crash" ||
                      std::string_view(argv[1]) == "--begin-and-crash" ||
                      std::string_view(argv[1]) == "--publish-artifacts-and-crash")) {
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
        CHECK(argc == 1 || argc == 6);
        const auto test_executable = std::filesystem::absolute(std::filesystem::path(argv[0]));
        const auto helper_directory = test_executable.parent_path();
        const SyntheticChildren children =
            argc == 6
                ? SyntheticChildren{
                      .success = argv[1],
                      .nonzero = argv[2],
                      .malformed = argv[3],
                      .overflow = argv[4],
                      .hang = argv[5],
                  }
                : SyntheticChildren{
                      .success =
                          helper_directory / "siqs_rss_campaign_synthetic_success",
                      .nonzero =
                          helper_directory / "siqs_rss_campaign_synthetic_nonzero",
                      .malformed =
                          helper_directory / "siqs_rss_campaign_synthetic_malformed",
                      .overflow =
                          helper_directory / "siqs_rss_campaign_synthetic_overflow",
                      .hang = helper_directory / "siqs_rss_campaign_synthetic_hang",
                  };
        const std::string executable = test_executable.string();
        test_trusted_base_component_walk_is_fail_closed();
        test_untrusted_owner_and_write_permissions_fail_closed();
        test_empty_store_lease_move_and_release(executable);
        test_crash_releases_persistent_lease(executable);
        test_active_slot_taint_is_durable_and_terminal();
        test_reopened_dangling_start_appends_pending_taint();
        test_reopened_taint_confirms_dangling_chain_durability();
        test_committed_prefix_durability_precedes_next_authority();
        test_slot_runner_contract_and_missing_deployment();
        test_slot_runner_rejects_invalid_deployment(children.success);
        test_slot_runner_happy_off_commits_one_same_child(children.success);
        test_slot_runner_execution_and_join_failures(children);
        test_slot_runner_artifact_prefix_failure_taints(children.success);
        test_slot_runner_commit_terminal_leaf_matrix(children.success);
        test_taint_rejects_inactive_and_nonpending_sessions();
        test_begin_crash_recovers_only_through_pending_taint(executable);
        test_artifact_root_is_required_and_private();
        test_artifact_root_identity_and_generation_are_revalidated();
        test_active_slot_actions_revalidate_authority_before_publication();
        test_private_artifact_batch_publisher_preserves_closed_commit_boundary(executable);
        test_artifact_batch_validates_mode_and_bounds_before_publication();
        test_artifact_publication_failures_preserve_prefix_and_trace();
        test_artifact_batch_crash_recovers_only_to_taint(executable);
        test_reopen_closes_artifacts_against_journal();
        test_artifact_leaf_trust_and_shape_fail_closed();
        test_unknown_case_and_temporary_leaves_fail_closed();
        test_invalid_session_lock_shapes();
        test_invalid_header_shapes_sizes_and_codec();
        test_record_gap_and_filename_wire_binding();
        test_valid_header_and_dangling_start_actions();
        test_empty_store_begins_one_lease_bound_slot(executable);
        test_header_only_store_begins_slot_without_replacing_header();
        test_store_publication_injection_preserves_durable_trace();
        test_taint_publication_injection_preserves_recovery();
        test_begin_rejects_inactive_and_nonready_sessions();
        test_begin_rejects_prepublication_namespace_changes();
        test_begin_revalidates_root_and_lock_before_publication();
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
