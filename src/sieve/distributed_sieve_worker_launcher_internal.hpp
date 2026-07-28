#pragma once

// Source-private receipt-gated WaveStore worker launcher.
//
// This composition layer is the only consumer of a freshly published
// AttemptStartedV1 receipt. It binds the complete immutable work identity,
// builds an anonymous work-package capability, and launches the self-exec
// worker with the exact descriptor layout owned by the low-level transport.
// It does not authenticate the executable object or rehydrate worker-side
// writer authority; those remain later boundaries.

#include "distributed_sieve_wave_store_internal.hpp"
#include "distributed_sieve_worker_process_internal.hpp"
#include "distributed_sieve_worker_work_package_file_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gnfs::sieve::distributed_sieve_worker_launcher_detail {

inline constexpr std::size_t DISTRIBUTED_SIEVE_WORKER_LAUNCH_NO_SLOT =
    std::numeric_limits<std::size_t>::max();

enum class DistributedSieveWorkerLaunchPhaseV1 : std::uint8_t {
    none,
    request_validation,
    process_preparation,
    work_binding,
    initial_receipt_revalidation,
    attempt_directory_binding,
    work_package_creation,
    final_receipt_revalidation,
    work_package_revalidation,
    process_spawn,
};

enum class DistributedSieveWorkerLaunchDispositionV1 : std::uint8_t {
    failed_before_gate,
    armed_no_child,
    indeterminate,
    partial,
    all,
};

struct DistributedSieveWorkerLauncherTestHooksV1 final {
    using AfterInitialReceipts = void (*)(void* context) noexcept;
    using BeforeCarrier = void (*)(std::size_t slot, void* context) noexcept;
    using AfterCarrier = void (*)(std::size_t slot, void* context) noexcept;
    using BeforeSpawn = int (*)(std::size_t slot, void* context) noexcept;

    AfterInitialReceipts after_initial_receipts = nullptr;
    BeforeCarrier before_carrier = nullptr;
    AfterCarrier after_carrier = nullptr;
    BeforeSpawn before_spawn = nullptr;
    void* context = nullptr;
    /// Trusted test-only refusal of the fixed-capability close-all primitive.
    /// The launcher checks this before its receipt gate; the transport repeats
    /// the check independently if reached directly.
    bool force_fixed_capability_close_all_unavailable = false;
};

/// One sealed launch slot.
///
/// Construction irreversibly transfers a fresh start receipt into the request.
/// Arguments exclude argv[0]. Bootstrap bytes are never caller supplied; the
/// launcher derives them from the receipt's canonical AttemptStartedV1.
class DistributedSieveWorkerLaunchSlotV1 final {
public:
    DistributedSieveWorkerLaunchSlotV1(
        distributed_sieve_resume_detail::DistributedSieveWorkerAttemptStartReceipt&& receipt,
        std::vector<std::string> arguments) noexcept;

    DistributedSieveWorkerLaunchSlotV1(const DistributedSieveWorkerLaunchSlotV1&) = delete;
    DistributedSieveWorkerLaunchSlotV1&
    operator=(const DistributedSieveWorkerLaunchSlotV1&) = delete;
    DistributedSieveWorkerLaunchSlotV1(DistributedSieveWorkerLaunchSlotV1&&) noexcept = default;
    DistributedSieveWorkerLaunchSlotV1& operator=(DistributedSieveWorkerLaunchSlotV1&&) = delete;
    ~DistributedSieveWorkerLaunchSlotV1() = default;

private:
    distributed_sieve_resume_detail::DistributedSieveWorkerAttemptStartReceipt receipt_;
    std::vector<std::string> arguments_;

    friend class distributed_sieve_resume_detail::DistributedSieveWaveStore;
};

/// Complete fixed wave launch request.
///
/// The executable path and all argv strings are owned before the WaveStore
/// enters its one-shot receipt gate.
class DistributedSieveWorkerLaunchRequestV1 final {
public:
    DistributedSieveWorkerLaunchRequestV1(
        std::string executable_path, std::vector<DistributedSieveWorkerLaunchSlotV1> slots,
        DistributedSieveWorkerLauncherTestHooksV1 hooks = {}) noexcept;

    DistributedSieveWorkerLaunchRequestV1(const DistributedSieveWorkerLaunchRequestV1&) = delete;
    DistributedSieveWorkerLaunchRequestV1&
    operator=(const DistributedSieveWorkerLaunchRequestV1&) = delete;
    DistributedSieveWorkerLaunchRequestV1(DistributedSieveWorkerLaunchRequestV1&&) noexcept =
        default;
    DistributedSieveWorkerLaunchRequestV1&
    operator=(DistributedSieveWorkerLaunchRequestV1&&) = delete;
    ~DistributedSieveWorkerLaunchRequestV1() = default;

private:
    std::string executable_path_;
    std::vector<DistributedSieveWorkerLaunchSlotV1> slots_;
    DistributedSieveWorkerLauncherTestHooksV1 hooks_;

    friend class distributed_sieve_resume_detail::DistributedSieveWaveStore;
};

/// Inseparable parent-side ownership of one launched process and its exact
/// start receipt.
///
/// A terminal reap permits ordinary receipt destruction. Abandoning this
/// composite before terminal proof deliberately quarantines the receipt for
/// the remainder of the parent process so its BaseLock cannot be released
/// while the child may still run. Destruction never waits for or kills the
/// child. Member order is intentional: destruction closes the process report
/// endpoint before ordinarily releasing a terminal receipt.
class DistributedSieveLaunchedWorkerAttemptV1 final {
public:
    DistributedSieveLaunchedWorkerAttemptV1(const DistributedSieveLaunchedWorkerAttemptV1&) =
        delete;
    DistributedSieveLaunchedWorkerAttemptV1&
    operator=(const DistributedSieveLaunchedWorkerAttemptV1&) = delete;
    DistributedSieveLaunchedWorkerAttemptV1(DistributedSieveLaunchedWorkerAttemptV1&&) noexcept =
        default;
    DistributedSieveLaunchedWorkerAttemptV1&
    operator=(DistributedSieveLaunchedWorkerAttemptV1&&) = delete;
    ~DistributedSieveLaunchedWorkerAttemptV1() noexcept;

    [[nodiscard]] const AttemptStartedV1& record() const noexcept;
    /// Revalidate only the retained AttemptStartedV1 start authority and its
    /// WaveStore/BaseLock bindings. This is not worker completion, handoff,
    /// cleanup, or writer-authority validation.
    [[nodiscard]] distributed_sieve_resume_detail::DistributedSieveWaveStoreDiagnostic
    revalidate() const noexcept;
    [[nodiscard]] distributed_sieve_worker_process_detail::DistributedSieveWorkerProcessId
    process_id() const noexcept;
    [[nodiscard]] int report_descriptor() const noexcept;
    [[nodiscard]] distributed_sieve_worker_process_detail::DistributedSieveWorkerProcessWaitResult
    wait_terminal(
        distributed_sieve_worker_process_detail::DistributedSieveWorkerProcessWaitTestHooks hooks =
            {}) noexcept;
    [[nodiscard]] int release_report_descriptor() noexcept;

private:
    DistributedSieveLaunchedWorkerAttemptV1(
        std::unique_ptr<
            distributed_sieve_resume_detail::DistributedSieveWorkerAttemptStartReceipt>&& receipt,
        distributed_sieve_worker_process_detail::DistributedSieveWorkerProcess&& process) noexcept;

    std::unique_ptr<distributed_sieve_resume_detail::DistributedSieveWorkerAttemptStartReceipt>
        receipt_;
    distributed_sieve_worker_process_detail::DistributedSieveWorkerProcess process_;
    bool terminal_reaped_ = false;

    friend class distributed_sieve_resume_detail::DistributedSieveWaveStore;
};

struct DistributedSieveWorkerLaunchDiagnosticV1 final {
    DistributedSieveWorkerLaunchPhaseV1 phase = DistributedSieveWorkerLaunchPhaseV1::none;
    std::size_t slot = DISTRIBUTED_SIEVE_WORKER_LAUNCH_NO_SLOT;
    DistributedSieveProtocolStatus protocol;
    distributed_sieve_resume_detail::DistributedSieveWaveStoreDiagnostic wave_store;
    distributed_sieve_worker_work_package_file_detail::
        DistributedSieveWorkerWorkPackageFileDiagnostic carrier;
    distributed_sieve_worker_process_detail::DistributedSieveWorkerProcessTransportDiagnostic
        transport;
    bool reconciliation_required = false;
};

struct DistributedSieveWorkerLaunchChildResultV1 final {
    std::uint32_t chunk_id = 0;
    std::uint32_t attempt_ordinal = 0;
    util::Sha256Digest attempt_started_digest;
    std::optional<DistributedSieveLaunchedWorkerAttemptV1> worker;
    distributed_sieve_worker_process_detail::DistributedSieveWorkerProcessTransportDiagnostic
        transport;

    DistributedSieveWorkerLaunchChildResultV1() = default;
    DistributedSieveWorkerLaunchChildResultV1(const DistributedSieveWorkerLaunchChildResultV1&) =
        delete;
    DistributedSieveWorkerLaunchChildResultV1&
    operator=(const DistributedSieveWorkerLaunchChildResultV1&) = delete;
    DistributedSieveWorkerLaunchChildResultV1(
        DistributedSieveWorkerLaunchChildResultV1&&) noexcept = default;
    DistributedSieveWorkerLaunchChildResultV1&
    operator=(DistributedSieveWorkerLaunchChildResultV1&&) = delete;
    ~DistributedSieveWorkerLaunchChildResultV1() = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return worker.has_value() && transport.error ==
                                         distributed_sieve_worker_process_detail::
                                             DistributedSieveWorkerProcessTransportError::none;
    }
};

struct DistributedSieveWorkerLaunchBatchResultV1 final {
    std::vector<DistributedSieveWorkerLaunchChildResultV1> children;
    DistributedSieveWorkerLaunchDiagnosticV1 diagnostic;
    DistributedSieveWorkerLaunchDispositionV1 disposition =
        DistributedSieveWorkerLaunchDispositionV1::failed_before_gate;

    DistributedSieveWorkerLaunchBatchResultV1() = default;
    DistributedSieveWorkerLaunchBatchResultV1(const DistributedSieveWorkerLaunchBatchResultV1&) =
        delete;
    DistributedSieveWorkerLaunchBatchResultV1&
    operator=(const DistributedSieveWorkerLaunchBatchResultV1&) = delete;
    DistributedSieveWorkerLaunchBatchResultV1(
        DistributedSieveWorkerLaunchBatchResultV1&&) noexcept = default;
    DistributedSieveWorkerLaunchBatchResultV1&
    operator=(DistributedSieveWorkerLaunchBatchResultV1&&) = delete;
    ~DistributedSieveWorkerLaunchBatchResultV1() = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        if (disposition != DistributedSieveWorkerLaunchDispositionV1::all || children.empty()) {
            return false;
        }
        for (const auto& child : children) {
            if (!child) {
                return false;
            }
        }
        return true;
    }
};

} // namespace gnfs::sieve::distributed_sieve_worker_launcher_detail
