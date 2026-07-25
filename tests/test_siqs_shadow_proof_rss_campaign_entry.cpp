// Fail-closed contract tests for the source-private production composition.
// No test deployment row, executable, session, or controller callback is
// injected: the default production registry must remain the only path.

#include "shadow_proof_rss_campaign_entry_internal.hpp"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace gnfs::siqs;
namespace entry_detail = gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail;
using EntryOutcome = entry_detail::ProductionCampaignEntryOutcome;
using EntryResult = entry_detail::ProductionCampaignEntryResult;
using StoreError = SIQSShadowProofRssCampaignJournalStoreError;
using StoreObject = SIQSShadowProofRssCampaignJournalStoreObject;

using EntryFunction = EntryResult (*)(const SIQSShadowProofRssGatePolicy*,
                                      const SIQSShadowProofRssCampaignRuntimeFacts*) noexcept;

static_assert(std::same_as<decltype(&entry_detail::run_siqs_shadow_proof_rss_production_campaign),
                           EntryFunction>);
static_assert(!std::is_default_constructible_v<EntryResult>);
static_assert(std::is_copy_constructible_v<EntryResult>);
static_assert(std::is_copy_assignable_v<EntryResult>);
static_assert(std::is_nothrow_move_constructible_v<EntryResult>);
static_assert(std::is_nothrow_move_assignable_v<EntryResult>);
static_assert(!std::is_convertible_v<EntryResult, bool>);
static_assert(!std::is_constructible_v<bool, EntryResult&>);
static_assert(!std::is_constructible_v<bool, const EntryResult&>);
static_assert(!std::is_constructible_v<bool, EntryResult&&>);
static_assert(!std::is_convertible_v<EntryResult, SIQSShadowProofRssCampaignJournalSession>);
static_assert(
    !std::is_convertible_v<EntryResult, SIQSShadowProofRssCampaignJournalStoreOpenResult>);
static_assert(!std::is_constructible_v<EntryResult, entry_detail::SerialCampaignResult>);
static_assert(!std::is_constructible_v<EntryResult, SIQSShadowProofRssCampaignJournalSession>);

template <typename T>
concept CastableToSession =
    requires(T value) { static_cast<SIQSShadowProofRssCampaignJournalSession>(std::move(value)); };

template <typename T>
concept CastableToOpenResult = requires(T value) {
    static_cast<SIQSShadowProofRssCampaignJournalStoreOpenResult>(std::move(value));
};

template <typename T>
concept HasTakeSession = requires(T value) { std::move(value).take_session(); };

template <typename T>
concept HasRetry = requires(T value) { value.retry(); };

template <typename T>
concept HasResume = requires(T value) { value.resume(); };

template <typename T>
concept HasReopen = requires(T value) { value.reopen(); };

template <typename T>
concept HasAppendPendingTaint = requires(T value) { std::move(value).append_pending_taint(); };

template <typename T>
concept HasGateOutcome = requires(const T value) { value.gate_outcome(); };

static_assert(!CastableToSession<EntryResult>);
static_assert(!CastableToOpenResult<EntryResult>);
static_assert(!HasTakeSession<EntryResult>);
static_assert(!HasRetry<EntryResult>);
static_assert(!HasResume<EntryResult>);
static_assert(!HasReopen<EntryResult>);
static_assert(!HasAppendPendingTaint<EntryResult>);
static_assert(!HasGateOutcome<EntryResult>);

static_assert(entry_detail::production_campaign_entry_detail::project_serial_outcome(
                  entry_detail::SerialCampaignOutcome::production_complete_gate_required) ==
              EntryOutcome::production_complete_gate_required);
static_assert(entry_detail::production_campaign_entry_detail::project_serial_outcome(
                  entry_detail::SerialCampaignOutcome::durably_tainted) ==
              EntryOutcome::durably_tainted);
static_assert(entry_detail::production_campaign_entry_detail::project_serial_outcome(
                  entry_detail::SerialCampaignOutcome::synthetic_complete) ==
              EntryOutcome::reconcile_required);
static_assert(entry_detail::production_campaign_entry_detail::project_serial_outcome(
                  entry_detail::SerialCampaignOutcome::stopped) ==
              EntryOutcome::reconcile_required);
static_assert(entry_detail::production_campaign_entry_detail::project_serial_outcome(
                  entry_detail::SerialCampaignOutcome::reconcile_required) ==
              EntryOutcome::reconcile_required);

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

[[nodiscard]] constexpr gnfs::util::ProcessMemoryBackend host_memory_backend() noexcept {
#if defined(_WIN32)
    return gnfs::util::ProcessMemoryBackend::WindowsPsapi;
#elif defined(__APPLE__)
    return gnfs::util::ProcessMemoryBackend::DarwinGetrusage;
#elif defined(__linux__)
    return gnfs::util::ProcessMemoryBackend::LinuxGetrusage;
#else
    return gnfs::util::ProcessMemoryBackend::Unsupported;
#endif
}

[[nodiscard]] constexpr SIQSShadowProofRssProbeExecutionIdentity claim_identity() noexcept {
    SIQSShadowProofRssProbeExecutionIdentity identity;
    for (std::size_t index = 0; index < identity.executable_sha256.bytes.size(); ++index) {
        identity.executable_sha256.bytes[index] = static_cast<std::byte>(index + 1);
        identity.execution_contract_sha256.bytes[index] = static_cast<std::byte>(index + 33);
    }
    return identity;
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
    policy.candidate_revision = "entry-candidate-revision-1";
    policy.probe_execution_identity = claim_identity();
    policy.approval_id = "entry-approval-ticket-1";
    policy.journal_store = {{UINT64_C(0x1010101010101010), UINT64_C(0x2020202020202020)},
                            {UINT64_C(0x3030303030303030), UINT64_C(0x4040404040404040)},
                            "rss-campaign-entry-no-side-effects-v1"};
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
        .probe_kind = SIQSShadowProofRssProbeKind::production_holdout,
        .candidate_revision = "entry-candidate-revision-1",
        .probe_execution_identity = claim_identity(),
        .release_build = true,
        .ndebug = true,
    };
}

class TempWorkspace final {
public:
    TempWorkspace() : original_(std::filesystem::current_path()) {
        const auto seed =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto base = std::filesystem::temp_directory_path();
        for (std::uint32_t attempt = 0; attempt < 256; ++attempt) {
            root_ =
                base / ("gnfs-siqs-entry-" + std::to_string(seed) + "-" + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(root_, error)) {
                std::filesystem::current_path(root_);
                return;
            }
        }
        throw std::runtime_error("unable to create temporary entry workspace");
    }

    ~TempWorkspace() {
        std::error_code ignored;
        std::filesystem::current_path(original_, ignored);
        std::filesystem::remove_all(root_, ignored);
    }

    TempWorkspace(const TempWorkspace&) = delete;
    TempWorkspace& operator=(const TempWorkspace&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

private:
    std::filesystem::path original_;
    std::filesystem::path root_;
};

[[nodiscard]] std::vector<std::string> snapshot_tree(const std::filesystem::path& root) {
    std::vector<std::string> snapshot;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        std::string value = std::filesystem::relative(entry.path(), root).generic_string();
        if (entry.is_regular_file()) {
            value += ":" + std::to_string(entry.file_size());
        } else if (entry.is_directory()) {
            value += "/";
        } else {
            value += ":other";
        }
        snapshot.push_back(std::move(value));
    }
    std::sort(snapshot.begin(), snapshot.end());
    return snapshot;
}

void check_rejection(const EntryResult& result, EntryOutcome outcome, StoreError store_error,
                     StoreObject store_object,
                     std::optional<SIQSShadowProofRssJournalReason> journal_reason) {
    CHECK(result.outcome() == outcome);
    CHECK(result.store_diagnostic().error == store_error);
    CHECK(result.store_diagnostic().object == store_object);
    CHECK(result.store_diagnostic().journal_reason == journal_reason);
    CHECK(!result.controller_result().has_value());
}

void test_names_are_closed() {
    CHECK(entry_detail::production_campaign_entry_outcome_name(EntryOutcome::preflight_rejected) ==
          "preflight_rejected");
    CHECK(entry_detail::production_campaign_entry_outcome_name(
              EntryOutcome::production_classification_rejected) ==
          "production_classification_rejected");
    CHECK(entry_detail::production_campaign_entry_outcome_name(EntryOutcome::open_rejected) ==
          "open_rejected");
    CHECK(entry_detail::production_campaign_entry_outcome_name(
              EntryOutcome::production_complete_gate_required) ==
          "production_complete_gate_required");
    CHECK(entry_detail::production_campaign_entry_outcome_name(EntryOutcome::durably_tainted) ==
          "durably_tainted");
    CHECK(entry_detail::production_campaign_entry_outcome_name(EntryOutcome::reconcile_required) ==
          "reconcile_required");
    CHECK(entry_detail::production_campaign_entry_outcome_name(
              static_cast<EntryOutcome>(UINT8_C(255))) == "unknown");
}

void test_preflight_and_classification_rejections() {
    auto facts = make_facts();
    check_rejection(entry_detail::run_siqs_shadow_proof_rss_production_campaign(nullptr, &facts),
                    EntryOutcome::preflight_rejected, StoreError::preflight_rejected,
                    StoreObject::none, SIQSShadowProofRssJournalReason::policy_missing);

    auto policy = make_policy();
    policy.approved = false;
    check_rejection(entry_detail::run_siqs_shadow_proof_rss_production_campaign(&policy, nullptr),
                    EntryOutcome::preflight_rejected, StoreError::preflight_rejected,
                    StoreObject::none, SIQSShadowProofRssJournalReason::policy_not_approved);

    policy = make_policy();
    check_rejection(entry_detail::run_siqs_shadow_proof_rss_production_campaign(&policy, nullptr),
                    EntryOutcome::preflight_rejected, StoreError::preflight_rejected,
                    StoreObject::none, SIQSShadowProofRssJournalReason::runtime_facts_missing);

    facts = make_facts();
    facts.release_build = false;
    check_rejection(entry_detail::run_siqs_shadow_proof_rss_production_campaign(&policy, &facts),
                    EntryOutcome::preflight_rejected, StoreError::preflight_rejected,
                    StoreObject::none, SIQSShadowProofRssJournalReason::release_ndebug_required);

    facts = make_facts();
    facts.ndebug = false;
    check_rejection(entry_detail::run_siqs_shadow_proof_rss_production_campaign(&policy, &facts),
                    EntryOutcome::preflight_rejected, StoreError::preflight_rejected,
                    StoreObject::none, SIQSShadowProofRssJournalReason::release_ndebug_required);

    facts = make_facts();
    facts.candidate_revision = "mismatched-candidate";
    check_rejection(entry_detail::run_siqs_shadow_proof_rss_production_campaign(&policy, &facts),
                    EntryOutcome::preflight_rejected, StoreError::preflight_rejected,
                    StoreObject::none, SIQSShadowProofRssJournalReason::runtime_facts_mismatch);

    facts = make_facts();
    facts.probe_execution_identity.executable_sha256.bytes[0] = std::byte{0x7f};
    check_rejection(entry_detail::run_siqs_shadow_proof_rss_production_campaign(&policy, &facts),
                    EntryOutcome::preflight_rejected, StoreError::preflight_rejected,
                    StoreObject::none, SIQSShadowProofRssJournalReason::runtime_facts_mismatch);
}

void test_default_registry_rejects_without_side_effects() {
    TempWorkspace workspace;
    const auto before = snapshot_tree(workspace.root());
    const auto policy = make_policy();
    const auto facts = make_facts();

    auto synthetic_facts = facts;
    synthetic_facts.probe_kind = SIQSShadowProofRssProbeKind::synthetic_test;
    const auto classified =
        entry_detail::run_siqs_shadow_proof_rss_production_campaign(&policy, &synthetic_facts);
    check_rejection(classified, EntryOutcome::production_classification_rejected, StoreError::none,
                    StoreObject::none, std::nullopt);
    CHECK(snapshot_tree(workspace.root()) == before);

    const auto first = entry_detail::run_siqs_shadow_proof_rss_production_campaign(&policy, &facts);
    check_rejection(first, EntryOutcome::open_rejected, StoreError::binding_not_registered,
                    StoreObject::deployment_registry, std::nullopt);

    const auto second =
        entry_detail::run_siqs_shadow_proof_rss_production_campaign(&policy, &facts);
    check_rejection(second, EntryOutcome::open_rejected, StoreError::binding_not_registered,
                    StoreObject::deployment_registry, std::nullopt);

    CHECK(snapshot_tree(workspace.root()) == before);
    const auto claimed_root = workspace.root() / std::string(policy.journal_store.relative_locator);
    CHECK(!std::filesystem::exists(claimed_root));
    CHECK(!std::filesystem::exists(claimed_root / "launch.txt"));
    CHECK(!std::filesystem::exists(claimed_root / "launches.txt"));
}

} // namespace

int main() {
    try {
        test_names_are_closed();
        test_preflight_and_classification_rejections();
        test_default_registry_rejects_without_side_effects();
        std::cout << "SIQS shadow-proof RSS campaign entry tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
