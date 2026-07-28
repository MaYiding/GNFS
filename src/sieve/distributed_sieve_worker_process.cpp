#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "distributed_sieve_worker_process_internal.hpp"

#include <array>
#include <cerrno>
#include <new>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <features.h>
#endif
#endif

namespace gnfs::sieve::distributed_sieve_worker_process_detail {
namespace {

inline constexpr int LAST_STANDARD_DESCRIPTOR = 2;
inline constexpr std::size_t FIXED_CAPABILITY_COUNT = 4U;

using FixedCapabilityDescriptorArray = std::array<int, FIXED_CAPABILITY_COUNT>;

inline constexpr FixedCapabilityDescriptorArray FIXED_CAPABILITY_TARGETS{
    DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR,
    DISTRIBUTED_SIEVE_WORKER_CHILD_PERMANENT_WAVE_STORE_LOCK_DESCRIPTOR,
    DISTRIBUTED_SIEVE_WORKER_CHILD_ATTEMPT_BASE_LOCK_DESCRIPTOR,
    DISTRIBUTED_SIEVE_WORKER_CHILD_WORK_PACKAGE_READER_DESCRIPTOR,
};

[[nodiscard]] constexpr FixedCapabilityDescriptorArray
capability_sources(const DistributedSieveWorkerProcessFixedCapabilitySourcesV1& sources) noexcept {
    return {
        sources.wave_root_directory_descriptor,
        sources.permanent_wave_store_lock_descriptor,
        sources.attempt_base_lock_descriptor,
        sources.work_package_reader_descriptor,
    };
}

void close_descriptor(int descriptor) noexcept {
#if !defined(_WIN32)
    if (descriptor >= 0) {
        (void)::close(descriptor);
    }
#else
    (void)descriptor;
#endif
}

[[nodiscard]] bool contains_nul(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] bool is_absolute_executable_path(std::string_view path) noexcept {
#if defined(_WIN32)
    const bool drive_absolute =
        path.size() >= 3U &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/');
    const bool unc_absolute = path.size() >= 2U && (path[0] == '\\' || path[0] == '/') &&
                              (path[1] == '\\' || path[1] == '/');
    return drive_absolute || unc_absolute;
#else
    return !path.empty() && path.front() == '/';
#endif
}

[[nodiscard]] DistributedSieveWorkerProcessBatchPrepareResult
prepare_failure(DistributedSieveWorkerProcessTransportError error, int native_error) noexcept {
    return {
        .batch = std::nullopt,
        .diagnostic =
            {
                .error = error,
                .native_error = native_error,
            },
    };
}

[[nodiscard]] DistributedSieveWorkerProcessBatchLaunchResult
launch_failure(DistributedSieveWorkerProcessTransportError error, int native_error) noexcept {
    return {
        .children = {},
        .diagnostic =
            {
                .error = error,
                .native_error = native_error,
            },
        .spawn_loop_entered = false,
        .child_set_complete = false,
    };
}

#if !defined(_WIN32)
struct DecodedWaitStatus final {
    bool terminal = false;
    bool success = false;
    int exit_status = -1;
    int signal = 0;
};

[[nodiscard]] DecodedWaitStatus decode_wait_status(int wait_status) noexcept {
    if (WIFEXITED(wait_status)) {
        const int status = WEXITSTATUS(wait_status);
        return {
            .terminal = true,
            .success = status == 0,
            .exit_status = status,
            .signal = 0,
        };
    }
    if (WIFSIGNALED(wait_status)) {
        return {
            .terminal = true,
            .success = false,
            .exit_status = -1,
            .signal = WTERMSIG(wait_status),
        };
    }
    return {};
}

[[nodiscard]] int duplicate_cloexec_at_or_above(int descriptor, int minimum) noexcept {
    int duplicate = -1;
    do {
        duplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, minimum);
    } while (duplicate < 0 && errno == EINTR);
    return duplicate;
}

#if !defined(__linux__)
[[nodiscard]] bool set_close_on_exec(int descriptor) noexcept {
    int flags = -1;
    do {
        flags = ::fcntl(descriptor, F_GETFD);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0) {
        return false;
    }

    int result = -1;
    do {
        result = ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}
#endif

[[nodiscard]] bool move_to_unmapped_descriptor_range(int& descriptor) noexcept {
    if (descriptor >= DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR) {
        return true;
    }
    const int duplicate = duplicate_cloexec_at_or_above(
        descriptor, DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR);
    if (duplicate < 0) {
        return false;
    }
    close_descriptor(descriptor);
    descriptor = duplicate;
    return true;
}

[[nodiscard]] bool make_cloexec_pipe(int (&descriptors)[2]) noexcept {
#if defined(__linux__)
    if (::pipe2(descriptors, O_CLOEXEC) != 0) {
        return false;
    }
#else
    if (::pipe(descriptors) != 0) {
        return false;
    }
    if (!set_close_on_exec(descriptors[0]) || !set_close_on_exec(descriptors[1])) {
        const int native_error = errno;
        close_descriptor(descriptors[0]);
        close_descriptor(descriptors[1]);
        descriptors[0] = -1;
        descriptors[1] = -1;
        errno = native_error;
        return false;
    }
#endif
    if (!move_to_unmapped_descriptor_range(descriptors[0]) ||
        !move_to_unmapped_descriptor_range(descriptors[1])) {
        const int native_error = errno;
        close_descriptor(descriptors[0]);
        close_descriptor(descriptors[1]);
        descriptors[0] = -1;
        descriptors[1] = -1;
        errno = native_error;
        return false;
    }
    return true;
}

[[nodiscard]] bool write_bootstrap_frame(int descriptor,
                                         std::span<const std::byte> frame) noexcept {
    ssize_t written = -1;
    do {
        written = ::write(descriptor, frame.data(), frame.size());
    } while (written < 0 && errno == EINTR);
    if (written < 0) {
        return false;
    }
    if (static_cast<std::size_t>(written) != frame.size()) {
        errno = EIO;
        return false;
    }
    return true;
}

[[nodiscard]] bool is_first_external_descriptor(std::span<const int> descriptors,
                                                std::size_t index) noexcept {
    for (std::size_t prior = 0; prior < index; ++prior) {
        if (descriptors[prior] == descriptors[index]) {
            return false;
        }
    }
    return true;
}

struct StagedFixedCapabilitySlot final {
    FixedCapabilityDescriptorArray descriptors{-1, -1, -1, -1};
};

struct StagedFixedCapabilityBatch final {
    std::vector<StagedFixedCapabilitySlot> slots;

    StagedFixedCapabilityBatch() = default;
    StagedFixedCapabilityBatch(const StagedFixedCapabilityBatch&) = delete;
    StagedFixedCapabilityBatch& operator=(const StagedFixedCapabilityBatch&) = delete;

    ~StagedFixedCapabilityBatch() noexcept {
        for (auto& slot : slots) {
            for (int& descriptor : slot.descriptors) {
                close_descriptor(std::exchange(descriptor, -1));
            }
        }
    }
};

[[nodiscard]] int configure_spawn_attributes(posix_spawnattr_t& attributes) noexcept {
    sigset_t empty_mask;
    sigset_t default_signals;
    if (sigemptyset(&empty_mask) != 0 || sigemptyset(&default_signals) != 0) {
        return errno;
    }
    for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
        if (signal_number == SIGKILL || signal_number == SIGSTOP) {
            continue;
        }
        if (sigaddset(&default_signals, signal_number) != 0 && errno != EINVAL) {
            return errno;
        }
    }

    int error = ::posix_spawnattr_setsigmask(&attributes, &empty_mask);
    if (error == 0) {
        error = ::posix_spawnattr_setsigdefault(&attributes, &default_signals);
    }
    if (error == 0) {
        error = ::posix_spawnattr_setpgroup(&attributes, 0);
    }
    short flags =
        static_cast<short>(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
#if defined(__APPLE__) && defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    flags = static_cast<short>(flags | POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    if (error == 0) {
        error = ::posix_spawnattr_setflags(&attributes, flags);
    }
    return error;
}

#endif

} // namespace

bool distributed_sieve_worker_process_fixed_capability_close_all_supported() noexcept {
#if defined(__APPLE__) && defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    return true;
#elif defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

DistributedSieveWorkerProcess::DistributedSieveWorkerProcess(
    DistributedSieveWorkerProcessId process_id, int report_descriptor) noexcept
    : process_id_(process_id), report_descriptor_(report_descriptor) {}

DistributedSieveWorkerProcess::DistributedSieveWorkerProcess(
    DistributedSieveWorkerProcess&& other) noexcept
    : process_id_(std::exchange(other.process_id_, -1)),
      report_descriptor_(std::exchange(other.report_descriptor_, -1)),
      wait_result_(std::move(other.wait_result_)) {
    other.wait_result_.reset();
}

DistributedSieveWorkerProcess::~DistributedSieveWorkerProcess() noexcept {
    close_descriptor(std::exchange(report_descriptor_, -1));
}

DistributedSieveWorkerProcessId DistributedSieveWorkerProcess::process_id() const noexcept {
    return process_id_;
}

int DistributedSieveWorkerProcess::report_descriptor() const noexcept {
    return report_descriptor_;
}

int DistributedSieveWorkerProcess::release_report_descriptor() noexcept {
    if (!wait_result_.has_value() || !wait_result_->reaped) {
        return -1;
    }
    return std::exchange(report_descriptor_, -1);
}

DistributedSieveWorkerProcessWaitResult DistributedSieveWorkerProcess::wait_terminal(
    DistributedSieveWorkerProcessWaitTestHooks hooks) noexcept {
    if (wait_result_.has_value()) {
        return *wait_result_;
    }
#if defined(_WIN32)
    (void)hooks;
    wait_result_.emplace(DistributedSieveWorkerProcessWaitResult{
        .reaped = false,
        .success = false,
        .exit_status = -1,
        .signal = 0,
        .native_error = ENOTSUP,
    });
    return *wait_result_;
#else
    if (process_id_ <= 0) {
        wait_result_.emplace(DistributedSieveWorkerProcessWaitResult{
            .reaped = false,
            .success = false,
            .exit_status = -1,
            .signal = 0,
            .native_error = EINVAL,
        });
        return *wait_result_;
    }

    const auto observe = [&](int* wait_status) noexcept {
        return hooks.wait != nullptr ? hooks.wait(process_id_, wait_status, 0, hooks.context)
                                     : static_cast<DistributedSieveWorkerProcessId>(
                                           ::waitpid(process_id_, wait_status, 0));
    };
    int wait_status = 0;
    DistributedSieveWorkerProcessId observed = -1;
    do {
        observed = observe(&wait_status);
    } while (observed < 0 && errno == EINTR);

    if (observed < 0) {
        wait_result_.emplace(DistributedSieveWorkerProcessWaitResult{
            .reaped = false,
            .success = false,
            .exit_status = -1,
            .signal = 0,
            .native_error = errno,
        });
        return *wait_result_;
    }
    if (observed != process_id_) {
        wait_result_.emplace(DistributedSieveWorkerProcessWaitResult{
            .reaped = false,
            .success = false,
            .exit_status = -1,
            .signal = 0,
            .native_error = ECHILD,
        });
        return *wait_result_;
    }

    const auto decoded = decode_wait_status(wait_status);
    if (!decoded.terminal) {
        wait_result_.emplace(DistributedSieveWorkerProcessWaitResult{});
        return *wait_result_;
    }

    wait_result_.emplace(DistributedSieveWorkerProcessWaitResult{
        .reaped = true,
        .success = decoded.success,
        .exit_status = decoded.exit_status,
        .signal = decoded.signal,
        .native_error = 0,
    });
    return *wait_result_;
#endif
}

DistributedSieveWorkerProcessBatch::DistributedSieveWorkerProcessBatch(
    std::vector<SpawnSlot> slots) noexcept
    : slots_(std::move(slots)) {}

DistributedSieveWorkerProcessBatch::DistributedSieveWorkerProcessBatch(
    DistributedSieveWorkerProcessBatch&& other) noexcept
    : slots_(std::move(other.slots_)) {
    other.slots_.clear();
}

DistributedSieveWorkerProcessBatch::~DistributedSieveWorkerProcessBatch() noexcept {
    for (auto& slot : slots_) {
        close_descriptor(std::exchange(slot.parent_bootstrap_read_descriptor, -1));
        close_descriptor(std::exchange(slot.parent_report_read_descriptor, -1));
        close_descriptor(std::exchange(slot.child_report_write_descriptor, -1));
    }
}

std::size_t DistributedSieveWorkerProcessBatch::size() const noexcept {
    return slots_.size();
}

DistributedSieveWorkerProcessBatchPrepareResult prepare_distributed_sieve_worker_process_batch(
    std::span<const DistributedSieveWorkerProcessSpawnSpec> children) noexcept {
    if (children.empty()) {
        return prepare_failure(DistributedSieveWorkerProcessTransportError::invalid_request,
                               EINVAL);
    }
    for (const auto& child : children) {
        if (child.executable_path.empty() || !is_absolute_executable_path(child.executable_path) ||
            contains_nul(child.executable_path) || child.bootstrap_frame.empty()) {
            return prepare_failure(DistributedSieveWorkerProcessTransportError::invalid_request,
                                   EINVAL);
        }
        if (child.bootstrap_frame.size() > DISTRIBUTED_SIEVE_WORKER_BOOTSTRAP_FRAME_LIMIT) {
            return prepare_failure(DistributedSieveWorkerProcessTransportError::invalid_request,
                                   E2BIG);
        }
        for (const auto argument : child.arguments) {
            if (contains_nul(argument)) {
                return prepare_failure(DistributedSieveWorkerProcessTransportError::invalid_request,
                                       EINVAL);
            }
        }
    }

#if defined(_WIN32)
    return prepare_failure(DistributedSieveWorkerProcessTransportError::platform_unavailable,
                           ENOTSUP);
#else
    std::vector<DistributedSieveWorkerProcessBatch::SpawnSlot> slots;
    try {
        slots.resize(children.size());
        for (std::size_t index = 0; index < children.size(); ++index) {
            auto& slot = slots[index];
            const auto& child = children[index];
            slot.executable_path.assign(child.executable_path);
            slot.arguments.resize(child.arguments.size());
            for (std::size_t argument_index = 0; argument_index < child.arguments.size();
                 ++argument_index) {
                slot.arguments[argument_index].assign(child.arguments[argument_index]);
            }
            slot.argument_vector.reserve(slot.arguments.size() + 2U);
            slot.argument_vector.push_back(slot.executable_path.data());
            for (auto& argument : slot.arguments) {
                slot.argument_vector.push_back(argument.data());
            }
            slot.argument_vector.push_back(nullptr);
        }
    } catch (const std::bad_alloc&) {
        return prepare_failure(DistributedSieveWorkerProcessTransportError::resource_exhausted,
                               ENOMEM);
    } catch (...) {
        return prepare_failure(DistributedSieveWorkerProcessTransportError::resource_exhausted,
                               ENOMEM);
    }

    const auto close_slots = [&slots]() noexcept {
        for (auto& slot : slots) {
            close_descriptor(std::exchange(slot.parent_bootstrap_read_descriptor, -1));
            close_descriptor(std::exchange(slot.parent_report_read_descriptor, -1));
            close_descriptor(std::exchange(slot.child_report_write_descriptor, -1));
        }
    };

    for (std::size_t index = 0; index < children.size(); ++index) {
        auto& slot = slots[index];
        int bootstrap_descriptors[2]{-1, -1};
        if (!make_cloexec_pipe(bootstrap_descriptors)) {
            const int native_error = errno;
            close_slots();
            return prepare_failure(DistributedSieveWorkerProcessTransportError::pipe_failed,
                                   native_error);
        }
        slot.parent_bootstrap_read_descriptor = bootstrap_descriptors[0];
        int bootstrap_write_descriptor = bootstrap_descriptors[1];

        int report_descriptors[2]{-1, -1};
        if (!make_cloexec_pipe(report_descriptors)) {
            const int native_error = errno;
            close_descriptor(std::exchange(bootstrap_write_descriptor, -1));
            close_slots();
            return prepare_failure(DistributedSieveWorkerProcessTransportError::pipe_failed,
                                   native_error);
        }
        slot.parent_report_read_descriptor = report_descriptors[0];
        slot.child_report_write_descriptor = report_descriptors[1];

        if (!write_bootstrap_frame(bootstrap_write_descriptor, children[index].bootstrap_frame)) {
            const int native_error = errno;
            close_descriptor(std::exchange(bootstrap_write_descriptor, -1));
            close_slots();
            return prepare_failure(
                DistributedSieveWorkerProcessTransportError::bootstrap_write_failed, native_error);
        }
        close_descriptor(std::exchange(bootstrap_write_descriptor, -1));
    }

    DistributedSieveWorkerProcessBatchPrepareResult result;
    DistributedSieveWorkerProcessBatch batch(std::move(slots));
    result.batch.emplace(std::move(batch));
    return result;
#endif
}

DistributedSieveWorkerProcessBatchLaunchResult DistributedSieveWorkerProcessBatch::launch_impl(
    std::span<const DistributedSieveWorkerProcessFixedCapabilitySourcesV1> capabilities,
    bool capability_mode, std::span<const int> close_in_every_child,
    DistributedSieveWorkerProcessSpawnTestHooks hooks) && noexcept {
    auto consumed = std::move(*this);
    if (consumed.slots_.empty()) {
        return launch_failure(DistributedSieveWorkerProcessTransportError::invalid_request, EINVAL);
    }
    if ((capability_mode && capabilities.size() != consumed.slots_.size()) ||
        (!capability_mode && !capabilities.empty())) {
        return launch_failure(DistributedSieveWorkerProcessTransportError::invalid_request, EINVAL);
    }
    for (const auto& slot : consumed.slots_) {
        if (slot.parent_bootstrap_read_descriptor <
                DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR ||
            slot.parent_report_read_descriptor <
                DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR ||
            slot.child_report_write_descriptor <
                DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR ||
            slot.executable_path.empty() || slot.argument_vector.size() < 2U ||
            slot.argument_vector.back() != nullptr) {
            return launch_failure(DistributedSieveWorkerProcessTransportError::invalid_request,
                                  EINVAL);
        }
    }
    const auto is_live_batch_descriptor = [&consumed](int descriptor) noexcept {
        for (const auto& slot : consumed.slots_) {
            if (slot.parent_bootstrap_read_descriptor == descriptor ||
                slot.parent_report_read_descriptor == descriptor ||
                slot.child_report_write_descriptor == descriptor) {
                return true;
            }
        }
        return false;
    };
    bool authority_uses_standard_error = false;
    if (capability_mode) {
        for (const auto& capability : capabilities) {
            const auto sources = capability_sources(capability);
            for (std::size_t role = 0; role < sources.size(); ++role) {
                const int source = sources[role];
                if (source < 0 || is_live_batch_descriptor(source)) {
                    return launch_failure(
                        DistributedSieveWorkerProcessTransportError::invalid_request, EINVAL);
                }
                for (std::size_t prior = 0; prior < role; ++prior) {
                    if (sources[prior] == source) {
                        return launch_failure(
                            DistributedSieveWorkerProcessTransportError::invalid_request, EINVAL);
                    }
                }
                authority_uses_standard_error =
                    authority_uses_standard_error || source == LAST_STANDARD_DESCRIPTOR;
            }
        }
    }
    for (const int descriptor : close_in_every_child) {
        if (descriptor >= 0 && descriptor <= LAST_STANDARD_DESCRIPTOR) {
            return launch_failure(DistributedSieveWorkerProcessTransportError::invalid_request,
                                  EINVAL);
        }
    }
    if (capability_mode &&
        (hooks.force_fixed_capability_close_all_unavailable ||
         !distributed_sieve_worker_process_fixed_capability_close_all_supported())) {
        return launch_failure(DistributedSieveWorkerProcessTransportError::platform_unavailable,
                              ENOTSUP);
    }

    std::vector<DistributedSieveWorkerProcessLaunchResult> results;
    try {
        results.resize(consumed.slots_.size());
    } catch (const std::bad_alloc&) {
        return launch_failure(DistributedSieveWorkerProcessTransportError::resource_exhausted,
                              ENOMEM);
    } catch (...) {
        return launch_failure(DistributedSieveWorkerProcessTransportError::resource_exhausted,
                              ENOMEM);
    }

    const auto fail_all_slots = [&results](DistributedSieveWorkerProcessTransportError error,
                                           int native_error) noexcept {
        for (auto& result : results) {
            result.diagnostic = {
                .error = error,
                .native_error = native_error,
            };
        }
        return DistributedSieveWorkerProcessBatchLaunchResult{
            .children = std::move(results),
            .diagnostic = {},
            .spawn_loop_entered = false,
            .child_set_complete = true,
        };
    };

#if defined(_WIN32)
    (void)hooks;
    return fail_all_slots(DistributedSieveWorkerProcessTransportError::platform_unavailable,
                          ENOTSUP);
#else
    StagedFixedCapabilityBatch staged_capabilities;
    if (capability_mode) {
        try {
            staged_capabilities.slots.resize(capabilities.size());
        } catch (const std::bad_alloc&) {
            return fail_all_slots(DistributedSieveWorkerProcessTransportError::resource_exhausted,
                                  ENOMEM);
        } catch (...) {
            return fail_all_slots(DistributedSieveWorkerProcessTransportError::resource_exhausted,
                                  ENOMEM);
        }
        for (std::size_t slot_index = 0; slot_index < capabilities.size(); ++slot_index) {
            const auto sources = capability_sources(capabilities[slot_index]);
            auto& staged = staged_capabilities.slots[slot_index].descriptors;
            for (std::size_t role = 0; role < sources.size(); ++role) {
                staged[role] = duplicate_cloexec_at_or_above(
                    sources[role], DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR);
                if (staged[role] < 0) {
                    return fail_all_slots(DistributedSieveWorkerProcessTransportError::spawn_failed,
                                          errno);
                }
            }
        }
    }

    int standard_error_snapshot = -1;
    if (!authority_uses_standard_error) {
        standard_error_snapshot = duplicate_cloexec_at_or_above(
            STDERR_FILENO, DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR);
        if (standard_error_snapshot < 0 && errno != EBADF) {
            return fail_all_slots(DistributedSieveWorkerProcessTransportError::spawn_failed, errno);
        }
    }

    char* empty_environment[]{nullptr};
    const auto is_staging_descriptor = [&staged_capabilities](int descriptor) noexcept {
        for (const auto& slot : staged_capabilities.slots) {
            for (const int staged : slot.descriptors) {
                if (staged == descriptor) {
                    return true;
                }
            }
        }
        return false;
    };
    const auto is_authority_source_descriptor = [capability_mode,
                                                 capabilities](int descriptor) noexcept {
        if (!capability_mode) {
            return false;
        }
        for (const auto& capability : capabilities) {
            for (const int source : capability_sources(capability)) {
                if (source == descriptor) {
                    return true;
                }
            }
        }
        return false;
    };
    const auto add_file_actions = [&consumed, capabilities, capability_mode, close_in_every_child,
                                   standard_error_snapshot, &is_authority_source_descriptor,
                                   &is_live_batch_descriptor, &is_staging_descriptor,
                                   &staged_capabilities](posix_spawn_file_actions_t& actions,
                                                         std::size_t own_index) noexcept {
        const auto& own = consumed.slots_[own_index];
        int error = ::posix_spawn_file_actions_adddup2(
            &actions, own.parent_bootstrap_read_descriptor, STDIN_FILENO);
        if (error != 0) {
            return error;
        }
        error = ::posix_spawn_file_actions_adddup2(&actions, own.child_report_write_descriptor,
                                                   STDOUT_FILENO);
        if (error != 0) {
            return error;
        }
        if (standard_error_snapshot >= 0) {
            error = ::posix_spawn_file_actions_adddup2(&actions, standard_error_snapshot,
                                                       STDERR_FILENO);
            if (error != 0) {
                return error;
            }
        }

        for (const auto& slot : consumed.slots_) {
            const int batch_descriptors[]{
                slot.parent_bootstrap_read_descriptor,
                slot.parent_report_read_descriptor,
                slot.child_report_write_descriptor,
            };
            for (const int descriptor : batch_descriptors) {
                if (descriptor < 0) {
                    continue;
                }
                error = ::posix_spawn_file_actions_addclose(&actions, descriptor);
                if (error != 0) {
                    return error;
                }
            }
        }
        if (standard_error_snapshot >= 0) {
            error = ::posix_spawn_file_actions_addclose(&actions, standard_error_snapshot);
            if (error != 0) {
                return error;
            }
        }

        for (std::size_t close_index = 0; close_index < close_in_every_child.size();
             ++close_index) {
            const int descriptor = close_in_every_child[close_index];
            if (descriptor < 0 ||
                !is_first_external_descriptor(close_in_every_child, close_index) ||
                is_live_batch_descriptor(descriptor) || descriptor == standard_error_snapshot ||
                is_staging_descriptor(descriptor) || is_authority_source_descriptor(descriptor)) {
                continue;
            }
            error = ::posix_spawn_file_actions_addclose(&actions, descriptor);
            if (error != 0) {
                return error;
            }
        }

        if (capability_mode) {
            const auto is_first_authority_source = [capabilities](std::size_t slot_index,
                                                                  std::size_t role_index) noexcept {
                const int source = capability_sources(capabilities[slot_index])[role_index];
                for (std::size_t prior_slot = 0; prior_slot <= slot_index; ++prior_slot) {
                    const auto prior_sources = capability_sources(capabilities[prior_slot]);
                    const std::size_t role_limit =
                        prior_slot == slot_index ? role_index : prior_sources.size();
                    for (std::size_t prior_role = 0; prior_role < role_limit; ++prior_role) {
                        if (prior_sources[prior_role] == source) {
                            return false;
                        }
                    }
                }
                return true;
            };
            for (std::size_t slot_index = 0; slot_index < capabilities.size(); ++slot_index) {
                const auto sources = capability_sources(capabilities[slot_index]);
                for (std::size_t role = 0; role < sources.size(); ++role) {
                    const int source = sources[role];
                    if (source == STDIN_FILENO || source == STDOUT_FILENO ||
                        !is_first_authority_source(slot_index, role)) {
                        continue;
                    }
                    error = ::posix_spawn_file_actions_addclose(&actions, source);
                    if (error != 0) {
                        return error;
                    }
                }
            }

            const auto& own_staged = staged_capabilities.slots[own_index].descriptors;
            for (std::size_t role = 0; role < own_staged.size(); ++role) {
                error = ::posix_spawn_file_actions_adddup2(&actions, own_staged[role],
                                                           FIXED_CAPABILITY_TARGETS[role]);
                if (error != 0) {
                    return error;
                }
            }
            for (const int descriptor : own_staged) {
                error = ::posix_spawn_file_actions_addclose(&actions, descriptor);
                if (error != 0) {
                    return error;
                }
            }
        }

#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
        if (error == 0) {
            const int first_unmapped =
                capability_mode ? DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR
                                : DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR;
            error = ::posix_spawn_file_actions_addclosefrom_np(&actions, first_unmapped);
        }
#endif
#endif
        return error;
    };

    posix_spawnattr_t attributes{};
    int attribute_error = ::posix_spawnattr_init(&attributes);
    const bool attributes_initialized = attribute_error == 0;
    if (attribute_error == 0) {
        attribute_error = configure_spawn_attributes(attributes);
    }
    if (attribute_error != 0) {
        if (attributes_initialized) {
            (void)::posix_spawnattr_destroy(&attributes);
        }
        close_descriptor(std::exchange(standard_error_snapshot, -1));
        return fail_all_slots(DistributedSieveWorkerProcessTransportError::spawn_failed,
                              attribute_error);
    }

    // From this point the complete fixed result set is allocated and every
    // slot is attempted exactly once; no global return can bypass the loop.
    for (std::size_t index = 0; index < consumed.slots_.size(); ++index) {
        auto& own = consumed.slots_[index];
        int spawn_error =
            hooks.before_spawn != nullptr ? hooks.before_spawn(index, hooks.context) : 0;

        posix_spawn_file_actions_t actions{};
        bool actions_initialized = false;
        if (spawn_error == 0) {
            spawn_error = ::posix_spawn_file_actions_init(&actions);
            actions_initialized = spawn_error == 0;
        }
        if (spawn_error == 0) {
            spawn_error = add_file_actions(actions, index);
        }

        pid_t child = -1;
        if (spawn_error == 0) {
            spawn_error = ::posix_spawn(&child, own.executable_path.c_str(), &actions, &attributes,
                                        own.argument_vector.data(), empty_environment);
        }
        if (actions_initialized) {
            (void)::posix_spawn_file_actions_destroy(&actions);
        }

        close_descriptor(std::exchange(own.parent_bootstrap_read_descriptor, -1));
        close_descriptor(std::exchange(own.child_report_write_descriptor, -1));
        if (spawn_error != 0) {
            close_descriptor(std::exchange(own.parent_report_read_descriptor, -1));
            results[index].diagnostic = {
                .error = DistributedSieveWorkerProcessTransportError::spawn_failed,
                .native_error = spawn_error,
            };
            continue;
        }

        DistributedSieveWorkerProcess process(child,
                                              std::exchange(own.parent_report_read_descriptor, -1));
        results[index].process.emplace(std::move(process));
    }
    (void)::posix_spawnattr_destroy(&attributes);
    close_descriptor(std::exchange(standard_error_snapshot, -1));

    return {
        .children = std::move(results),
        .diagnostic = {},
        .spawn_loop_entered = true,
        .child_set_complete = true,
    };
#endif
}

DistributedSieveWorkerProcessBatchLaunchResult spawn_distributed_sieve_worker_process_batch(
    DistributedSieveWorkerProcessBatch&& batch, std::span<const int> close_in_every_child,
    DistributedSieveWorkerProcessSpawnTestHooks hooks) noexcept {
    return std::move(batch).launch_impl({}, false, close_in_every_child, hooks);
}

DistributedSieveWorkerProcessBatchLaunchResult
spawn_distributed_sieve_worker_process_batch_with_capabilities(
    DistributedSieveWorkerProcessBatch&& batch,
    std::span<const DistributedSieveWorkerProcessFixedCapabilitySourcesV1> capabilities,
    std::span<const int> close_in_every_child,
    DistributedSieveWorkerProcessSpawnTestHooks hooks) noexcept {
    return std::move(batch).launch_impl(capabilities, true, close_in_every_child, hooks);
}

} // namespace gnfs::sieve::distributed_sieve_worker_process_detail
