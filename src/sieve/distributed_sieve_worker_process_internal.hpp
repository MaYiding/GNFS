#pragma once

// Source-private self-exec process transport foundation for sieve workers.
//
// This layer encapsulates a posix_spawn()/waitpid() sequence but is not yet
// integrated into the legacy distributed sieve. The generic entry grants no
// WaveStore, lease, path, relation-writer, or retry authority. The exact-role
// entry only transports caller-borrowed descriptors; a receipt-gated higher
// layer must authenticate and retain those capabilities. A prepared batch owns
// a per-slot bootstrap-input channel and report-output channel. Every bounded
// bootstrap frame is written and every parent bootstrap writer is closed
// before the first child is spawned.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <limits.h>
#include <sys/types.h>
#endif

namespace gnfs::sieve::distributed_sieve_worker_process_detail {

#if defined(_WIN32)
using DistributedSieveWorkerProcessId = std::int64_t;
inline constexpr std::size_t DISTRIBUTED_SIEVE_WORKER_BOOTSTRAP_FRAME_LIMIT = 4096U;
#else
using DistributedSieveWorkerProcessId = pid_t;
inline constexpr std::size_t DISTRIBUTED_SIEVE_WORKER_BOOTSTRAP_FRAME_LIMIT =
    PIPE_BUF < 4096 ? static_cast<std::size_t>(PIPE_BUF) : 4096U;
#endif

inline constexpr int DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR = 3;
inline constexpr int DISTRIBUTED_SIEVE_WORKER_CHILD_PERMANENT_WAVE_STORE_LOCK_DESCRIPTOR = 4;
inline constexpr int DISTRIBUTED_SIEVE_WORKER_CHILD_ATTEMPT_BASE_LOCK_DESCRIPTOR = 5;
inline constexpr int DISTRIBUTED_SIEVE_WORKER_CHILD_WORK_PACKAGE_READER_DESCRIPTOR = 6;
inline constexpr int DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR = 7;

/// Whether the fixed-capability transport can close every unmapped descriptor.
///
/// This is true only when the platform supplies an atomic spawn-time close-all
/// primitive: POSIX_SPAWN_CLOEXEC_DEFAULT on Apple or
/// posix_spawn_file_actions_addclosefrom_np() on glibc 2.34 and newer. Callers
/// may use this query before preparing receipt-gated launch state. The
/// capability spawn entry also enforces it independently.
[[nodiscard]] bool distributed_sieve_worker_process_fixed_capability_close_all_supported() noexcept;

class DistributedSieveWorkerProcessBatch;
struct DistributedSieveWorkerProcessBatchPrepareResult;
struct DistributedSieveWorkerProcessSpawnSpec;
struct DistributedSieveWorkerProcessFixedCapabilitySourcesV1;
struct DistributedSieveWorkerProcessLaunchResult;
struct DistributedSieveWorkerProcessBatchLaunchResult;
struct DistributedSieveWorkerProcessSpawnTestHooks;

enum class DistributedSieveWorkerProcessTransportError : std::uint8_t {
    none,
    invalid_request,
    platform_unavailable,
    resource_exhausted,
    pipe_failed,
    bootstrap_write_failed,
    spawn_failed,
};

[[nodiscard]] constexpr std::string_view distributed_sieve_worker_process_transport_error_name(
    DistributedSieveWorkerProcessTransportError error) noexcept {
    switch (error) {
    case DistributedSieveWorkerProcessTransportError::none:
        return "none";
    case DistributedSieveWorkerProcessTransportError::invalid_request:
        return "invalid_request";
    case DistributedSieveWorkerProcessTransportError::platform_unavailable:
        return "platform_unavailable";
    case DistributedSieveWorkerProcessTransportError::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveWorkerProcessTransportError::pipe_failed:
        return "pipe_failed";
    case DistributedSieveWorkerProcessTransportError::bootstrap_write_failed:
        return "bootstrap_write_failed";
    case DistributedSieveWorkerProcessTransportError::spawn_failed:
        return "spawn_failed";
    }
    return "unknown";
}

struct DistributedSieveWorkerProcessTransportDiagnostic final {
    DistributedSieveWorkerProcessTransportError error =
        DistributedSieveWorkerProcessTransportError::none;
    int native_error = 0;
};

/// Parent-side terminal observation.
///
/// `reaped` is true only for WIFEXITED or WIFSIGNALED. A stopped, continued,
/// mismatched, or failed wait remains uncertain and cannot authorize cleanup
/// or retry. The first non-EINTR observation is sticky, including uncertainty,
/// so a later wait cannot launder an already-uncertain lifecycle boundary.
struct DistributedSieveWorkerProcessWaitResult final {
    bool reaped = false;
    bool success = false;
    int exit_status = -1;
    int signal = 0;
    int native_error = 0;

    [[nodiscard]] friend constexpr bool
    operator==(const DistributedSieveWorkerProcessWaitResult&,
               const DistributedSieveWorkerProcessWaitResult&) noexcept = default;
};

struct DistributedSieveWorkerProcessWaitTestHooks final {
    using Wait = DistributedSieveWorkerProcessId (*)(DistributedSieveWorkerProcessId process_id,
                                                     int* wait_status, int options,
                                                     void* context) noexcept;

    Wait wait = nullptr;
    void* context = nullptr;
};

class DistributedSieveWorkerProcess final {
public:
    DistributedSieveWorkerProcess() = delete;
    DistributedSieveWorkerProcess(const DistributedSieveWorkerProcess&) = delete;
    DistributedSieveWorkerProcess& operator=(const DistributedSieveWorkerProcess&) = delete;
    DistributedSieveWorkerProcess(DistributedSieveWorkerProcess&& other) noexcept;
    DistributedSieveWorkerProcess& operator=(DistributedSieveWorkerProcess&&) = delete;

    /// Close the still-owned report descriptor. Destruction never kills or
    /// reaps the child; callers must establish and retain the wait result.
    ~DistributedSieveWorkerProcess() noexcept;

    [[nodiscard]] DistributedSieveWorkerProcessId process_id() const noexcept;
    [[nodiscard]] int report_descriptor() const noexcept;

    /// Transfer the report descriptor to a specialized frame decoder only
    /// after a terminal reap. Before that boundary this returns -1 and retains
    /// ownership.
    [[nodiscard]] int release_report_descriptor() noexcept;

    /// Wait for the direct child, retrying EINTR.
    ///
    /// The first non-EINTR result is cached. Only an exited/signaled result
    /// confirms reap; every other cached result permanently suppresses cleanup
    /// and retry for this token.
    [[nodiscard]] DistributedSieveWorkerProcessWaitResult
    wait_terminal(DistributedSieveWorkerProcessWaitTestHooks hooks = {}) noexcept;

private:
    DistributedSieveWorkerProcess(DistributedSieveWorkerProcessId process_id,
                                  int report_descriptor) noexcept;

    DistributedSieveWorkerProcessId process_id_ = -1;
    int report_descriptor_ = -1;
    std::optional<DistributedSieveWorkerProcessWaitResult> wait_result_;

    friend class DistributedSieveWorkerProcessBatch;
    friend struct DistributedSieveWorkerProcessBatchLaunchResult;
    friend DistributedSieveWorkerProcessBatchLaunchResult
    spawn_distributed_sieve_worker_process_batch(
        DistributedSieveWorkerProcessBatch&& batch, std::span<const int> close_in_every_child,
        DistributedSieveWorkerProcessSpawnTestHooks hooks) noexcept;
    friend DistributedSieveWorkerProcessBatchLaunchResult
    spawn_distributed_sieve_worker_process_batch_with_capabilities(
        DistributedSieveWorkerProcessBatch&& batch,
        std::span<const DistributedSieveWorkerProcessFixedCapabilitySourcesV1> capabilities,
        std::span<const int> close_in_every_child,
        DistributedSieveWorkerProcessSpawnTestHooks hooks) noexcept;
};

/// One self-exec slot.
///
/// `arguments` excludes argv[0], which the transport supplies from
/// `executable_path`. The executable path must be absolute, non-empty, and
/// contain no NUL; each argument must contain no NUL. All strings and the
/// bootstrap bytes are copied or written during preparation; the caller need
/// not retain the spans after prepare returns.
struct DistributedSieveWorkerProcessSpawnSpec final {
    std::string_view executable_path;
    std::span<const std::string_view> arguments;
    std::span<const std::byte> bootstrap_frame;
};

/// Borrowed authority sources for one fixed child slot.
///
/// The four roles and child targets are protocol constants, not caller choices.
/// The caller must keep every source open until the authority-aware spawn call
/// returns. The transport duplicates the complete batch before the first spawn
/// and never closes these parent-side sources.
struct DistributedSieveWorkerProcessFixedCapabilitySourcesV1 final {
    int wave_root_directory_descriptor = -1;
    int permanent_wave_store_lock_descriptor = -1;
    int attempt_base_lock_descriptor = -1;
    int work_package_reader_descriptor = -1;
};

/// All bootstrap/report channels and owned argv storage for one future wave.
///
/// The type is move-only and exposes no pipe endpoint. Destruction closes
/// every still-owned descriptor. The generic and authority-aware spawn
/// functions are its only consumers.
class DistributedSieveWorkerProcessBatch final {
public:
    DistributedSieveWorkerProcessBatch() = delete;
    DistributedSieveWorkerProcessBatch(const DistributedSieveWorkerProcessBatch&) = delete;
    DistributedSieveWorkerProcessBatch&
    operator=(const DistributedSieveWorkerProcessBatch&) = delete;
    DistributedSieveWorkerProcessBatch(DistributedSieveWorkerProcessBatch&& other) noexcept;
    DistributedSieveWorkerProcessBatch& operator=(DistributedSieveWorkerProcessBatch&&) = delete;
    ~DistributedSieveWorkerProcessBatch() noexcept;

    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct SpawnSlot final {
        int parent_bootstrap_read_descriptor = -1;
        int parent_report_read_descriptor = -1;
        int child_report_write_descriptor = -1;
        std::string executable_path;
        std::vector<std::string> arguments;
        std::vector<char*> argument_vector;
    };

    explicit DistributedSieveWorkerProcessBatch(std::vector<SpawnSlot> slots) noexcept;
    [[nodiscard]] DistributedSieveWorkerProcessBatchLaunchResult
    launch_impl(std::span<const DistributedSieveWorkerProcessFixedCapabilitySourcesV1> capabilities,
                bool capability_mode, std::span<const int> close_in_every_child,
                DistributedSieveWorkerProcessSpawnTestHooks hooks) && noexcept;

    std::vector<SpawnSlot> slots_;

    friend struct DistributedSieveWorkerProcessBatchPrepareResult;
    friend DistributedSieveWorkerProcessBatchPrepareResult
    prepare_distributed_sieve_worker_process_batch(
        std::span<const DistributedSieveWorkerProcessSpawnSpec> children) noexcept;
    friend DistributedSieveWorkerProcessBatchLaunchResult
    spawn_distributed_sieve_worker_process_batch(
        DistributedSieveWorkerProcessBatch&& batch, std::span<const int> close_in_every_child,
        DistributedSieveWorkerProcessSpawnTestHooks hooks) noexcept;
    friend DistributedSieveWorkerProcessBatchLaunchResult
    spawn_distributed_sieve_worker_process_batch_with_capabilities(
        DistributedSieveWorkerProcessBatch&& batch,
        std::span<const DistributedSieveWorkerProcessFixedCapabilitySourcesV1> capabilities,
        std::span<const int> close_in_every_child,
        DistributedSieveWorkerProcessSpawnTestHooks hooks) noexcept;
};

struct DistributedSieveWorkerProcessBatchPrepareResult final {
    std::optional<DistributedSieveWorkerProcessBatch> batch;
    DistributedSieveWorkerProcessTransportDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return batch.has_value() &&
               diagnostic.error == DistributedSieveWorkerProcessTransportError::none;
    }
};

/// Test-only per-slot failure injection.
///
/// Return zero from `before_spawn` to execute the real posix_spawn(). Return a
/// positive errno value to fail that slot without invoking posix_spawn().
/// `force_fixed_capability_close_all_unavailable` exercises the platform
/// refusal before the first per-slot hook.
struct DistributedSieveWorkerProcessSpawnTestHooks final {
    using BeforeSpawn = int (*)(std::size_t slot, void* context) noexcept;

    BeforeSpawn before_spawn = nullptr;
    void* context = nullptr;
    bool force_fixed_capability_close_all_unavailable = false;
};

struct DistributedSieveWorkerProcessLaunchResult final {
    std::optional<DistributedSieveWorkerProcess> process;
    DistributedSieveWorkerProcessTransportDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return process.has_value() &&
               diagnostic.error == DistributedSieveWorkerProcessTransportError::none;
    }
};

struct DistributedSieveWorkerProcessBatchLaunchResult final {
    std::vector<DistributedSieveWorkerProcessLaunchResult> children;
    DistributedSieveWorkerProcessTransportDiagnostic diagnostic;
    /// True only after control reaches the fixed per-slot spawn loop.
    bool spawn_loop_entered = false;
    /// True only when `children` is the complete fixed per-slot result set.
    ///
    /// A false value with an empty child set, a non-none global diagnostic,
    /// and `spawn_loop_entered == false` is a closed global pre-spawn failure,
    /// not an assertion that a partial child set is complete.
    bool child_set_complete = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        if (!spawn_loop_entered || !child_set_complete ||
            diagnostic.error != DistributedSieveWorkerProcessTransportError::none ||
            children.empty()) {
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

/// Validate and prepare one complete self-exec wave.
///
/// The child list and every bootstrap frame must be non-empty. Each frame is
/// bounded by `DISTRIBUTED_SIEVE_WORKER_BOOTSTRAP_FRAME_LIMIT`, which is never
/// greater than either PIPE_BUF or 4096 on POSIX. Validation and owned argv
/// allocation finish before any pipe is opened. Every frame is then written
/// to an empty per-slot pipe and every bootstrap writer is closed before this
/// function returns.
[[nodiscard]] DistributedSieveWorkerProcessBatchPrepareResult
prepare_distributed_sieve_worker_process_batch(
    std::span<const DistributedSieveWorkerProcessSpawnSpec> children) noexcept;

/// Consume one prepared batch and perform one posix_spawn() per fixed slot.
///
/// Each child maps its bootstrap reader to stdin and report writer to stdout.
/// The parent's stderr open/closed state is sampled once at launch; when open,
/// a private duplicate maps the same open file description to child stderr.
/// Each child starts a new process group with an empty signal mask, default
/// signal dispositions, and an explicitly empty environment. Ordered file
/// actions close the complete live batch endpoint inventory and every declared
/// foreign descriptor after installing the standard-stream mappings.
///
/// Apple uses POSIX_SPAWN_CLOEXEC_DEFAULT and glibc 2.34 or newer additionally
/// closes every descriptor from 3 upward. Other POSIX implementations guarantee
/// only the batch and explicitly declared close inventory; callers there must
/// declare any ambient non-CLOEXEC descriptor that must not cross exec. A
/// per-slot spawn failure closes that slot and the remaining fixed slots are
/// attempted exactly once.
[[nodiscard]] DistributedSieveWorkerProcessBatchLaunchResult
spawn_distributed_sieve_worker_process_batch(
    DistributedSieveWorkerProcessBatch&& batch, std::span<const int> close_in_every_child = {},
    DistributedSieveWorkerProcessSpawnTestHooks hooks = {}) noexcept;

/// Consume one prepared batch with four exact fixed capabilities per child.
///
/// The capability count must equal the prepared slot count. Every source is
/// borrowed and may use any non-negative descriptor, including 0 through 6.
/// Within one slot the four sources must be distinct, and no source may alias a
/// live bootstrap/report endpoint. Before the first spawn, the transport
/// duplicates the complete source batch at descriptors 7 or greater with
/// close-on-exec. A failure in that preflight starts no child.
///
/// Each child receives the wave-root directory at descriptor 3, the permanent
/// WaveStore lock at 4, the attempt BaseLock at 5, and the anonymous immutable
/// work-package reader at 6. Standard input, output, and error retain the
/// generic contract, except that an authority source occupying parent
/// descriptor 2 makes child standard error explicitly closed rather than
/// leaking that authority as a diagnostic stream. Platforms without a reliable
/// spawn-time close-all primitive fail with `platform_unavailable` before any
/// child is spawned; the explicit close inventory is not an authority-mode
/// fallback.
[[nodiscard]] DistributedSieveWorkerProcessBatchLaunchResult
spawn_distributed_sieve_worker_process_batch_with_capabilities(
    DistributedSieveWorkerProcessBatch&& batch,
    std::span<const DistributedSieveWorkerProcessFixedCapabilitySourcesV1> capabilities,
    std::span<const int> close_in_every_child = {},
    DistributedSieveWorkerProcessSpawnTestHooks hooks = {}) noexcept;

} // namespace gnfs::sieve::distributed_sieve_worker_process_detail
