#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <gnfs/util/bounded_child_process.hpp>

#include "authenticated_bounded_child_process_capability_internal.hpp"
#include "bounded_child_process_internal.hpp"

#if !defined(_WIN32)

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <limits>
#include <new>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <features.h>
#include <linux/prctl.h>
#include <pthread.h>
#include <sys/syscall.h>
#endif

extern char** environ;

namespace gnfs::util {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto POLL_QUANTUM = 20ms;
constexpr auto POST_EXIT_WRITER_GRACE = 200ms;
constexpr auto TERMINATION_CLEANUP_GRACE = 2s;
constexpr std::size_t STREAM_DRAIN_BUDGET = 64 * 1024;

struct ProcessGroupAbsenceResult final {
    bool absent = false;
    int native_error = 0;
};

[[nodiscard]] ProcessGroupAbsenceResult
wait_for_process_group_absence_until(pid_t process_group, Clock::time_point deadline) noexcept {
    while (true) {
        const int probe_status = ::kill(-process_group, 0);
        if (probe_status < 0 && errno == EINTR) {
            if (Clock::now() >= deadline) {
                return {false, ETIMEDOUT};
            }
            continue;
        }
        if (probe_status < 0 && errno == ESRCH) {
            return {true, 0};
        }
        if (probe_status < 0 && errno != EPERM) {
            return {false, errno};
        }

        const auto now = Clock::now();
        if (now >= deadline) {
            return {false, probe_status < 0 ? EPERM : ETIMEDOUT};
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int timeout = static_cast<int>(std::min(remaining, POLL_QUANTUM).count());
        const int poll_status = ::poll(nullptr, 0, timeout);
        if (poll_status < 0 && errno == EINTR) {
            continue;
        }
        if (poll_status < 0) {
            return {false, errno};
        }
    }
}

class UniqueFd final {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return fd_ >= 0;
    }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            // close() may already have released the descriptor when it reports
            // EINTR. Retrying can close an unrelated descriptor reused by a
            // concurrent launch, so RAII cleanup always issues one close.
            (void)::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

class SpawnFileActions final {
public:
    SpawnFileActions() noexcept : status_(::posix_spawn_file_actions_init(&actions_)) {}
    ~SpawnFileActions() {
        if (status_ == 0) {
            (void)::posix_spawn_file_actions_destroy(&actions_);
        }
    }

    SpawnFileActions(const SpawnFileActions&) = delete;
    SpawnFileActions& operator=(const SpawnFileActions&) = delete;

    [[nodiscard]] int status() const noexcept {
        return status_;
    }

    [[nodiscard]] posix_spawn_file_actions_t* get() noexcept {
        return &actions_;
    }

private:
    posix_spawn_file_actions_t actions_{};
    int status_ = 0;
};

class SpawnAttributes final {
public:
    SpawnAttributes() noexcept : status_(::posix_spawnattr_init(&attributes_)) {}
    ~SpawnAttributes() {
        if (status_ == 0) {
            (void)::posix_spawnattr_destroy(&attributes_);
        }
    }

    SpawnAttributes(const SpawnAttributes&) = delete;
    SpawnAttributes& operator=(const SpawnAttributes&) = delete;

    [[nodiscard]] int status() const noexcept {
        return status_;
    }

    [[nodiscard]] posix_spawnattr_t* get() noexcept {
        return &attributes_;
    }

private:
    posix_spawnattr_t attributes_{};
    int status_ = 0;
};

struct CapturePipe final {
    UniqueFd read_end;
    UniqueFd write_end;
};

[[nodiscard]] bool has_embedded_nul(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] bool valid_environment_entry(std::string_view entry) noexcept {
    const std::size_t separator = entry.find('=');
    return !has_embedded_nul(entry) && separator != std::string_view::npos && separator != 0;
}

[[nodiscard]] bool valid_spec(const BoundedChildProcessSpec& spec) noexcept {
    if (spec.executable.empty() || !spec.executable.is_absolute() ||
        spec.deadline <= Clock::now() ||
        (spec.inherit_parent_environment && !spec.environment.empty()) ||
        (spec.merge_stderr_into_stdout && spec.stderr_limit != 0)) {
        return false;
    }
    const std::string& executable = spec.executable.native();
    if (has_embedded_nul(executable)) {
        return false;
    }
    for (const auto& argument : spec.arguments) {
        if (has_embedded_nul(argument)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < spec.environment.size(); ++index) {
        if (!valid_environment_entry(spec.environment[index])) {
            return false;
        }
        const std::string_view left = spec.environment[index];
        const std::size_t left_separator = left.find('=');
        for (std::size_t other = 0; other < index; ++other) {
            const std::string_view right = spec.environment[other];
            const std::size_t right_separator = right.find('=');
            if (left.substr(0, left_separator) == right.substr(0, right_separator)) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool cancellation_requested(const BoundedChildProcessSpec& spec) noexcept {
    return spec.cancellation_probe != nullptr && spec.cancellation_probe(spec.cancellation_context);
}

void set_primary_error(BoundedChildProcessResult& result, BoundedChildProcessError error,
                       int native_error = 0) noexcept {
    if (result.error != BoundedChildProcessError::none) {
        return;
    }
    result.error = error;
    if (native_error != 0) {
        result.native_error = std::error_code(native_error, std::generic_category());
    }
}

void set_cleanup_error(BoundedChildProcessResult& result, int native_error) noexcept {
    if (native_error != 0 && !result.cleanup_error) {
        result.cleanup_error = std::error_code(native_error, std::generic_category());
    }
    if (result.error == BoundedChildProcessError::none) {
        result.error = BoundedChildProcessError::cleanup_failed;
        result.native_error = result.cleanup_error;
    }
}

#if !defined(__linux__)
[[nodiscard]] bool set_cloexec(int fd) noexcept {
    int flags = -1;
    do {
        flags = ::fcntl(fd, F_GETFD);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0) {
        return false;
    }
    int status = -1;
    do {
        status = ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    } while (status < 0 && errno == EINTR);
    return status == 0;
}
#endif

[[nodiscard]] bool move_above_standard_streams(UniqueFd& fd) noexcept {
    if (fd.get() >= 3) {
        return true;
    }
    int duplicate = -1;
    do {
        duplicate = ::fcntl(fd.get(), F_DUPFD_CLOEXEC, 3);
    } while (duplicate < 0 && errno == EINTR);
    if (duplicate < 0) {
        return false;
    }
    fd.reset(duplicate);
    return true;
}

[[nodiscard]] bool set_nonblocking_read(int fd) noexcept {
    int flags = -1;
    do {
        flags = ::fcntl(fd, F_GETFL);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0) {
        return false;
    }
    int status = -1;
    do {
        status = ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    } while (status < 0 && errno == EINTR);
    return status == 0;
}

[[nodiscard]] bool make_capture_pipe(CapturePipe& pipe, int& native_error) noexcept {
    int descriptors[2]{-1, -1};
#if defined(__linux__)
    if (::pipe2(descriptors, O_CLOEXEC) != 0) {
        native_error = errno;
        return false;
    }
#else
    if (::pipe(descriptors) != 0) {
        native_error = errno;
        return false;
    }
#endif
    pipe.read_end.reset(descriptors[0]);
    pipe.write_end.reset(descriptors[1]);
#if !defined(__linux__)
    if (!set_cloexec(pipe.read_end.get()) || !set_cloexec(pipe.write_end.get())) {
        native_error = errno;
        return false;
    }
#endif
    if (!move_above_standard_streams(pipe.read_end) ||
        !move_above_standard_streams(pipe.write_end) ||
        !set_nonblocking_read(pipe.read_end.get())) {
        native_error = errno;
        return false;
    }
    return true;
}

[[nodiscard]] int add_spawn_file_actions(SpawnFileActions& actions, const CapturePipe& stdout_pipe,
                                         const CapturePipe& stderr_pipe,
                                         bool merge_stderr_into_stdout) noexcept {
    int status =
        ::posix_spawn_file_actions_addopen(actions.get(), STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (status == 0) {
        status = ::posix_spawn_file_actions_adddup2(actions.get(), stdout_pipe.write_end.get(),
                                                    STDOUT_FILENO);
    }
    if (status == 0) {
        const int stderr_source =
            merge_stderr_into_stdout ? stdout_pipe.write_end.get() : stderr_pipe.write_end.get();
        status = ::posix_spawn_file_actions_adddup2(actions.get(), stderr_source, STDERR_FILENO);
    }
    for (const int fd : {stdout_pipe.read_end.get(), stdout_pipe.write_end.get(),
                         stderr_pipe.read_end.get(), stderr_pipe.write_end.get()}) {
        if (status == 0 && fd >= 0) {
            status = ::posix_spawn_file_actions_addclose(actions.get(), fd);
        }
    }
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
    if (status == 0) {
        status = ::posix_spawn_file_actions_addclosefrom_np(actions.get(), 3);
    }
#endif
#endif
    return status;
}

[[nodiscard]] int configure_spawn_attributes(SpawnAttributes& attributes) noexcept {
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
    int status = ::posix_spawnattr_setsigmask(attributes.get(), &empty_mask);
    if (status == 0) {
        status = ::posix_spawnattr_setsigdefault(attributes.get(), &default_signals);
    }
    if (status == 0) {
        status = ::posix_spawnattr_setpgroup(attributes.get(), 0);
    }
    short flags =
        static_cast<short>(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
#if defined(__APPLE__) && defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    flags = static_cast<short>(flags | POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    if (status == 0) {
        status = ::posix_spawnattr_setflags(attributes.get(), flags);
    }
    return status;
}

#if defined(__linux__)

struct DescriptorSpawnResult final {
    pid_t child = -1;
    int native_error = 0;
    BoundedChildProcessError error = BoundedChildProcessError::spawn_failed;
    bool cleanup_complete = true;
    int cleanup_error = 0;
};

#if GNFS_AUTHENTICATED_BOUNDED_CHILD_COMPILE_CAPABLE

enum class DescriptorPreExecStage : std::uint32_t {
    set_process_group = 1,
    reset_signals = 2,
    duplicate_stdio = 3,
    close_descriptors = 4,
    restore_signal_mask = 5,
    execveat = 6,
    arm_parent_death_signal = 7,
    verify_parent_liveness = 8,
};

struct DescriptorPreExecFailure final {
    DescriptorPreExecStage stage = DescriptorPreExecStage::set_process_group;
    int native_error = EIO;
};

static_assert(sizeof(DescriptorPreExecFailure) <= PIPE_BUF);

[[noreturn]] void report_pre_exec_failure(int diagnostic_fd, DescriptorPreExecStage stage,
                                          int native_error) noexcept {
    const DescriptorPreExecFailure failure{
        .stage = stage,
        .native_error = native_error != 0 ? native_error : EIO,
    };
    const auto* bytes = reinterpret_cast<const std::byte*>(&failure);
    std::size_t written = 0;
    ssize_t count = -1;
    while (written < sizeof(failure)) {
        do {
            count = ::write(diagnostic_fd, bytes + written, sizeof(failure) - written);
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            break;
        }
        written += static_cast<std::size_t>(count);
    }
    (void)count;
    ::_exit(127);
}

[[nodiscard]] bool mark_all_nonstandard_fds_cloexec() noexcept {
#if defined(SYS_close_range)
    constexpr unsigned int close_range_cloexec = 1U << 2U;
    return ::syscall(SYS_close_range, 3U, std::numeric_limits<unsigned int>::max(),
                     close_range_cloexec) == 0;
#else
    errno = ENOSYS;
    return false;
#endif
}

[[nodiscard]] int reset_descriptor_child_signals() noexcept {
    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    if (::sigemptyset(&default_action.sa_mask) != 0) {
        return errno;
    }
    for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
        if (signal_number == SIGKILL || signal_number == SIGSTOP) {
            continue;
        }
        if (::sigaction(signal_number, &default_action, nullptr) != 0 && errno != EINVAL) {
            return errno;
        }
    }
    return 0;
}

struct DescriptorCleanupResult final {
    bool complete = false;
    int native_error = 0;
};

[[nodiscard]] DescriptorCleanupResult kill_and_reap_descriptor_child(pid_t child) noexcept {
    if (child <= 1) {
        return {false, EPERM};
    }

    const auto cleanup_deadline = Clock::now() + TERMINATION_CLEANUP_GRACE;
    int retained_error = 0;
    if (::kill(-child, SIGKILL) != 0 && errno != ESRCH) {
        retained_error = errno;
    }
    if (::kill(child, SIGKILL) != 0 && errno != ESRCH && retained_error == 0) {
        retained_error = errno;
    }

    bool child_reaped = false;
    while (true) {
        int wait_status = 0;
        const pid_t waited = ::waitpid(child, &wait_status, WNOHANG);
        if (waited == child) {
            child_reaped = true;
            break;
        }
        if (waited < 0 && errno == EINTR) {
            if (Clock::now() >= cleanup_deadline) {
                return {false, ETIMEDOUT};
            }
            continue;
        }
        if (waited < 0) {
            return {false, errno != 0 ? errno : ECHILD};
        }
        if (Clock::now() >= cleanup_deadline) {
            return {false, ETIMEDOUT};
        }
        const int poll_status = ::poll(nullptr, 0, static_cast<int>(POLL_QUANTUM.count()));
        if (poll_status < 0 && errno == EINTR) {
            continue;
        }
        if (poll_status < 0) {
            return {false, errno};
        }
    }

    const auto group_receipt = wait_for_process_group_absence_until(child, cleanup_deadline);
    if (!group_receipt.absent && retained_error == 0) {
        retained_error = group_receipt.native_error;
    }
    return {child_reaped && group_receipt.absent && retained_error == 0, retained_error};
}

[[nodiscard]] constexpr bool descriptor_exec_is_platform_unavailable(int native_error) noexcept {
    return native_error == ENOSYS || native_error == EACCES || native_error == EPERM;
}

[[nodiscard]] constexpr bool
descriptor_parent_death_is_platform_unavailable(int native_error) noexcept {
    return native_error == ENOSYS || native_error == EINVAL || native_error == EACCES ||
           native_error == EPERM;
}

[[nodiscard]] constexpr bool
descriptor_stage_is_parent_death_setup(DescriptorPreExecStage stage) noexcept {
    return stage == DescriptorPreExecStage::arm_parent_death_signal ||
           stage == DescriptorPreExecStage::verify_parent_liveness;
}

#endif

[[nodiscard]] DescriptorSpawnResult
spawn_from_executable_fd(int executable_fd, const CapturePipe& stdout_pipe,
                         const CapturePipe& stderr_pipe, char* const* argv,
                         char* const* environment, const BoundedChildProcessSpec& spec) noexcept {
#if !GNFS_AUTHENTICATED_BOUNDED_CHILD_COMPILE_CAPABLE
    (void)executable_fd;
    (void)stdout_pipe;
    (void)stderr_pipe;
    (void)argv;
    (void)environment;
    (void)spec;
    return {-1, ENOTSUP, BoundedChildProcessError::platform_unavailable};
#else
    int descriptors[2]{-1, -1};
    if (::pipe2(descriptors, O_CLOEXEC) != 0) {
        return {-1, errno, BoundedChildProcessError::spawn_failed};
    }
    UniqueFd diagnostic_read(descriptors[0]);
    UniqueFd diagnostic_write(descriptors[1]);
    if (!move_above_standard_streams(diagnostic_read) ||
        !move_above_standard_streams(diagnostic_write)) {
        return {-1, errno, BoundedChildProcessError::spawn_failed};
    }

    int null_descriptor = -1;
    do {
        null_descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    } while (null_descriptor < 0 && errno == EINTR);
    if (null_descriptor < 0) {
        return {-1, errno, BoundedChildProcessError::spawn_failed};
    }
    UniqueFd null_fd(null_descriptor);
    if (!move_above_standard_streams(null_fd)) {
        return {-1, errno, BoundedChildProcessError::spawn_failed};
    }

    sigset_t blocked_signals;
    sigset_t previous_mask;
    if (::sigfillset(&blocked_signals) != 0) {
        return {-1, errno, BoundedChildProcessError::spawn_failed};
    }
    const int block_status = ::pthread_sigmask(SIG_SETMASK, &blocked_signals, &previous_mask);
    if (block_status != 0) {
        return {-1, block_status, BoundedChildProcessError::spawn_failed};
    }

    BoundedChildProcessError prevented_error = BoundedChildProcessError::none;
    int prevented_native_error = 0;
    if (cancellation_requested(spec)) {
        prevented_error = BoundedChildProcessError::cancelled;
        prevented_native_error = ECANCELED;
    } else if (Clock::now() >= spec.deadline) {
        prevented_error = BoundedChildProcessError::timeout;
        prevented_native_error = ETIMEDOUT;
    }
    if (prevented_error != BoundedChildProcessError::none) {
        const int restore_status = ::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
        return {-1, prevented_native_error, prevented_error, restore_status == 0, restore_status};
    }

    const pid_t expected_parent = ::getpid();
    const pid_t child = ::_Fork();
    if (child < 0) {
        const int fork_error = errno;
        (void)::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
        return {-1, fork_error, BoundedChildProcessError::spawn_failed};
    }
    if (child == 0) {
        if (::syscall(SYS_prctl, PR_SET_PDEATHSIG, SIGKILL, 0L, 0L, 0L) != 0) {
            report_pre_exec_failure(diagnostic_write.get(),
                                    DescriptorPreExecStage::arm_parent_death_signal, errno);
        }
        const long observed_parent = ::syscall(SYS_getppid);
        if (observed_parent < 0) {
            report_pre_exec_failure(diagnostic_write.get(),
                                    DescriptorPreExecStage::verify_parent_liveness, errno);
        }
        if (observed_parent != static_cast<long>(expected_parent)) {
            report_pre_exec_failure(diagnostic_write.get(),
                                    DescriptorPreExecStage::verify_parent_liveness, ECHILD);
        }
        (void)::close(diagnostic_read.get());
        if (::setpgid(0, 0) != 0) {
            report_pre_exec_failure(diagnostic_write.get(),
                                    DescriptorPreExecStage::set_process_group, errno);
        }
        if (const int signal_error = reset_descriptor_child_signals(); signal_error != 0) {
            report_pre_exec_failure(diagnostic_write.get(), DescriptorPreExecStage::reset_signals,
                                    signal_error);
        }
        const int stderr_source = spec.merge_stderr_into_stdout ? stdout_pipe.write_end.get()
                                                                : stderr_pipe.write_end.get();
        if (::dup2(null_fd.get(), STDIN_FILENO) < 0 ||
            ::dup2(stdout_pipe.write_end.get(), STDOUT_FILENO) < 0 ||
            ::dup2(stderr_source, STDERR_FILENO) < 0) {
            report_pre_exec_failure(diagnostic_write.get(), DescriptorPreExecStage::duplicate_stdio,
                                    errno);
        }
        if (!mark_all_nonstandard_fds_cloexec()) {
            report_pre_exec_failure(diagnostic_write.get(),
                                    DescriptorPreExecStage::close_descriptors, errno);
        }
        sigset_t empty_mask;
        if (::sigemptyset(&empty_mask) != 0 ||
            ::sigprocmask(SIG_SETMASK, &empty_mask, nullptr) != 0) {
            report_pre_exec_failure(diagnostic_write.get(),
                                    DescriptorPreExecStage::restore_signal_mask, errno);
        }

        (void)::syscall(SYS_execveat, executable_fd, "", argv, environment, AT_EMPTY_PATH);
        report_pre_exec_failure(diagnostic_write.get(), DescriptorPreExecStage::execveat, errno);
    }

    (void)::setpgid(child, child);
    const int restore_status = ::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
    if (restore_status != 0) {
        const auto cleanup = kill_and_reap_descriptor_child(child);
        return {-1, restore_status, BoundedChildProcessError::spawn_failed, cleanup.complete,
                cleanup.native_error};
    }

    diagnostic_write.reset();
    DescriptorPreExecFailure child_failure{};
    auto* bytes = reinterpret_cast<std::byte*>(&child_failure);
    std::size_t received = 0;
    bool handshake_timed_out = false;
    bool handshake_cancelled = false;
    bool diagnostic_eof = false;
    const auto consume_diagnostic_event = [&](const pollfd& descriptor, int poll_status) noexcept {
        if (poll_status < 0 || (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            child_failure.native_error = poll_status < 0 ? errno : EIO;
            received = sizeof(child_failure);
            return;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP)) == 0) {
            return;
        }
        const ssize_t count =
            ::read(diagnostic_read.get(), bytes + received, sizeof(child_failure) - received);
        if (count < 0 && errno == EINTR) {
            return;
        }
        if (count == 0) {
            diagnostic_eof = true;
        } else if (count < 0) {
            child_failure.native_error = errno;
            received = sizeof(child_failure);
        } else {
            received += static_cast<std::size_t>(count);
        }
    };
    while (received < sizeof(child_failure) && !diagnostic_eof) {
        // A closed diagnostic descriptor authenticates exec success, and a
        // complete failure record authenticates pre-exec failure. Consume
        // either already-published outcome before a concurrent cancel/deadline.
        pollfd descriptor{diagnostic_read.get(), POLLIN | POLLHUP, 0};
        int poll_status = ::poll(&descriptor, 1, 0);
        if (poll_status < 0 && errno == EINTR) {
            poll_status = 0;
        }
        if (poll_status != 0) {
            consume_diagnostic_event(descriptor, poll_status);
            continue;
        }
        if (cancellation_requested(spec)) {
            descriptor.revents = 0;
            poll_status = ::poll(&descriptor, 1, 0);
            if (poll_status < 0 && errno == EINTR) {
                continue;
            }
            if (poll_status != 0) {
                consume_diagnostic_event(descriptor, poll_status);
                continue;
            }
            handshake_cancelled = true;
            break;
        }
        const auto now = Clock::now();
        if (now >= spec.deadline) {
            descriptor.revents = 0;
            poll_status = ::poll(&descriptor, 1, 0);
            if (poll_status < 0 && errno == EINTR) {
                continue;
            }
            if (poll_status != 0) {
                consume_diagnostic_event(descriptor, poll_status);
                continue;
            }
            handshake_timed_out = true;
            break;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(spec.deadline - now);
        const auto bounded_remaining =
            std::max<std::chrono::milliseconds>(1ms, std::min(remaining, POLL_QUANTUM));
        descriptor.revents = 0;
        poll_status = ::poll(&descriptor, 1, static_cast<int>(bounded_remaining.count()));
        if (poll_status < 0 && errno == EINTR) {
            continue;
        }
        if (poll_status == 0) {
            continue;
        }
        consume_diagnostic_event(descriptor, poll_status);
    }
    diagnostic_read.reset();
    if (handshake_cancelled) {
        child_failure.native_error = ECANCELED;
    } else if (handshake_timed_out) {
        child_failure.native_error = ETIMEDOUT;
    } else if (received == 0) {
        return {child, 0, BoundedChildProcessError::none};
    }
    if ((!handshake_timed_out && !handshake_cancelled && received != sizeof(child_failure)) ||
        child_failure.native_error == 0) {
        child_failure.native_error = EIO;
    }

    const auto cleanup = kill_and_reap_descriptor_child(child);
    const auto error =
        handshake_cancelled   ? BoundedChildProcessError::cancelled
        : handshake_timed_out ? BoundedChildProcessError::timeout
        : child_failure.stage == DescriptorPreExecStage::execveat &&
                descriptor_exec_is_platform_unavailable(child_failure.native_error)
            ? BoundedChildProcessError::platform_unavailable
        : descriptor_stage_is_parent_death_setup(child_failure.stage) &&
                descriptor_parent_death_is_platform_unavailable(child_failure.native_error)
            ? BoundedChildProcessError::platform_unavailable
            : BoundedChildProcessError::spawn_failed;
    return {-1, child_failure.native_error, error, cleanup.complete, cleanup.native_error};
#endif
}

#endif

struct StreamState final {
    UniqueFd* fd = nullptr;
    std::string* bytes = nullptr;
    std::size_t limit = 0;
    bool* eof = nullptr;
    bool* overflow = nullptr;
    bool* read_failed = nullptr;
};

void drain_stream(StreamState state, BoundedChildProcessResult& result) noexcept {
    if (!*state.fd) {
        return;
    }
    std::array<char, 8192> buffer{};
    std::size_t drained = 0;
    while (drained < STREAM_DRAIN_BUDGET) {
        ssize_t count = -1;
        do {
            count = ::read(state.fd->get(), buffer.data(), buffer.size());
        } while (count < 0 && errno == EINTR);
        if (count > 0) {
            const std::size_t amount = static_cast<std::size_t>(count);
            drained += amount;
            const std::size_t available = state.limit - state.bytes->size();
            const std::size_t retained = std::min(amount, available);
            if (retained != 0) {
                state.bytes->append(buffer.data(), retained);
            }
            if (retained != amount) {
                *state.overflow = true;
                set_primary_error(result, BoundedChildProcessError::overflow);
                return;
            }
            continue;
        }
        if (count == 0) {
            state.fd->reset();
            *state.eof = true;
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        const int error = errno;
        *state.read_failed = true;
        state.fd->reset();
        set_primary_error(result, BoundedChildProcessError::read_failed, error);
        return;
    }
}

enum class ChildIdentityState : std::uint8_t {
    running,
    exited_waitable,
    unavailable,
    failed,
};

struct ChildIdentityObservation final {
    ChildIdentityState state = ChildIdentityState::failed;
    int native_error = 0;
};

[[nodiscard]] ChildIdentityObservation observe_child_without_reaping(pid_t child) noexcept {
    siginfo_t information{};
    int status = -1;
    do {
        status =
            ::waitid(P_PID, static_cast<id_t>(child), &information, WEXITED | WNOHANG | WNOWAIT);
    } while (status < 0 && errno == EINTR);
    if (status == 0) {
        if (information.si_pid == 0) {
            return {ChildIdentityState::running, 0};
        }
        if (information.si_pid == child) {
            return {ChildIdentityState::exited_waitable, 0};
        }
        return {ChildIdentityState::failed, ECHILD};
    }
    if (errno == ECHILD) {
        return {ChildIdentityState::unavailable, ECHILD};
    }
    return {ChildIdentityState::failed, errno};
}

[[nodiscard]] bool reap_without_blocking(pid_t child, int& status, bool& reaped,
                                         BoundedChildProcessResult& result) noexcept {
    if (reaped) {
        return true;
    }
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &status, WNOHANG);
    } while (waited < 0 && errno == EINTR);
    if (waited == child) {
        reaped = true;
        return true;
    }
    if (waited == 0) {
        return true;
    }
    set_primary_error(result, BoundedChildProcessError::wait_failed, errno);
    return false;
}

[[nodiscard]] bool query_process_group(pid_t process, pid_t& group, int& native_error) noexcept {
    do {
        group = ::getpgid(process);
    } while (group < 0 && errno == EINTR);
    if (group < 0) {
        native_error = errno;
        return false;
    }
    return true;
}

void kill_direct_child(pid_t child, BoundedChildProcessResult& result) noexcept {
    if (child <= 1) {
        set_cleanup_error(result, EPERM);
        return;
    }
    int status = -1;
    do {
        status = ::kill(child, SIGKILL);
    } while (status < 0 && errno == EINTR);
    if (status < 0 && errno != ESRCH) {
        set_cleanup_error(result, errno);
    }
}

void terminate_child_tree(pid_t child, pid_t verified_group, bool child_reaped,
                          bool& child_exited_waitable, bool& group_cleanup_needs_verification,
                          BoundedChildProcessResult& result) noexcept {
    const pid_t current_group = ::getpgrp();
    pid_t observed_group = -1;
    int observation_error = 0;
    bool child_identity_owned = false;
    bool child_group_unobservable = false;
    if (!child_reaped) {
        auto identity = observe_child_without_reaping(child);
        child_identity_owned = identity.state == ChildIdentityState::running ||
                               identity.state == ChildIdentityState::exited_waitable;
        child_exited_waitable = identity.state == ChildIdentityState::exited_waitable;
        observation_error = identity.native_error;
        if (identity.state == ChildIdentityState::running) {
            if (!query_process_group(child, observed_group, observation_error)) {
                const int group_query_error = observation_error;
                identity = observe_child_without_reaping(child);
                if (identity.state == ChildIdentityState::exited_waitable) {
                    child_identity_owned = true;
                    child_exited_waitable = true;
                    observation_error = 0;
                } else if (identity.state == ChildIdentityState::running &&
                           group_query_error == ESRCH) {
                    // The exact child is still owned and therefore reserves
                    // its PID. The successful POSIX_SPAWN_SETPGROUP contract
                    // makes -child safe even when Darwin transiently hides the
                    // group during a very short exit.
                    child_identity_owned = true;
                    child_exited_waitable = false;
                    child_group_unobservable = true;
                    observation_error = 0;
                } else {
                    // A live child whose group cannot be observed is not a
                    // safe signal target. In particular, ECHILD means an
                    // external reaper or SA_NOCLDWAIT released the PID.
                    child_identity_owned = false;
                    child_exited_waitable = false;
                    if (identity.native_error != 0) {
                        observation_error = identity.native_error;
                    }
                }
            }
        }
    }

    const auto scope = detail::select_posix_termination_scope(
        static_cast<std::int64_t>(child), static_cast<std::int64_t>(verified_group),
        static_cast<std::int64_t>(current_group), static_cast<std::int64_t>(observed_group),
        child_reaped, child_identity_owned, child_exited_waitable, child_group_unobservable);
    if (scope == detail::PosixTerminationScope::direct_child) {
        set_cleanup_error(result, observation_error != 0 ? observation_error : EPERM);
        kill_direct_child(child, result);
        return;
    }
    if (scope == detail::PosixTerminationScope::none) {
        set_cleanup_error(result, observation_error != 0 ? observation_error : EPERM);
        return;
    }

    group_cleanup_needs_verification = true;
    int status = -1;
    do {
        status = ::kill(-verified_group, SIGKILL);
    } while (status < 0 && errno == EINTR);
    if (status == 0) {
        // Delivery is not a completion receipt: non-child members can remain
        // observable briefly after accepting SIGKILL. Keep the leader's PID
        // reserved until waitpid(), then require that numeric group ID to
        // vanish. Reuse after reaping can only cause a fail-closed result:
        // subsequent probes use signal 0 and never target the reused group.
        return;
    }
    if (status < 0 && errno != ESRCH) {
        const int signal_error = errno;
        if (signal_error == EPERM) {
            // Darwin reports EPERM while a process group contains only a
            // just-exited leader. Defer the verdict until that exact PID is
            // reaped, then require the group to be absent. A genuinely live,
            // unsignalable descendant therefore remains a cleanup failure.
            if (!child_exited_waitable) {
                kill_direct_child(child, result);
            }
            return;
        }
        set_cleanup_error(result, signal_error);
        if (!child_exited_waitable) {
            kill_direct_child(child, result);
        }
    }
}

void record_termination(int status, BoundedChildProcessResult& result) noexcept {
    if (WIFEXITED(status)) {
        result.termination.kind = BoundedChildTerminationKind::exited;
        result.termination.exit_code = static_cast<std::uint32_t>(WEXITSTATUS(status));
        if (result.error == BoundedChildProcessError::none && result.termination.exit_code != 0) {
            result.error = BoundedChildProcessError::normal_nonzero;
        }
        return;
    }
    if (WIFSIGNALED(status)) {
        result.termination.kind = BoundedChildTerminationKind::signaled;
        result.termination.signal = WTERMSIG(status);
        if (result.error == BoundedChildProcessError::none) {
            result.error = BoundedChildProcessError::signaled;
        }
        return;
    }
    set_primary_error(result, BoundedChildProcessError::wait_failed);
}

[[nodiscard]] int bounded_poll_timeout(Clock::time_point now, Clock::time_point deadline) noexcept {
    if (deadline <= now) {
        return 0;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return static_cast<int>(std::min(remaining, POLL_QUANTUM).count());
}

[[nodiscard]] bool wait_for_process_group_absence(pid_t verified_group, Clock::time_point deadline,
                                                  BoundedChildProcessResult& result) noexcept {
    const auto receipt = wait_for_process_group_absence_until(verified_group, deadline);
    if (!receipt.absent) {
        set_cleanup_error(result, receipt.native_error);
    }
    return receipt.absent;
}

} // namespace

namespace {

BoundedChildProcessResult run_bounded_child_process_impl(const BoundedChildProcessSpec& spec,
                                                         int executable_fd,
                                                         std::string_view logical_argv0,
                                                         bool logical_argv0_is_explicit) noexcept {
    BoundedChildProcessResult result;
    result.error = BoundedChildProcessError::none;

    try {
        if (!valid_spec(spec) || executable_fd < -1 ||
            (logical_argv0_is_explicit &&
             (logical_argv0.empty() || has_embedded_nul(logical_argv0))) ||
            (executable_fd >= 0 && !logical_argv0_is_explicit)
#if defined(__linux__)
            || (executable_fd >= 0 && executable_fd < 3)
#else
            || executable_fd >= 0
#endif
        ) {
            result.error = BoundedChildProcessError::invalid_spec;
            result.cleanup_complete = true;
            return result;
        }
        if (cancellation_requested(spec)) {
            result.error = BoundedChildProcessError::cancelled;
            result.cleanup_complete = true;
            return result;
        }

        result.stdout_bytes.reserve(spec.stdout_limit);
        result.stderr_bytes.reserve(spec.stderr_limit);

        std::vector<std::string> argv_storage;
        argv_storage.reserve(spec.arguments.size() + 1);
        argv_storage.emplace_back(
            logical_argv0_is_explicit ? logical_argv0 : std::string_view(spec.executable.native()));
        argv_storage.insert(argv_storage.end(), spec.arguments.begin(), spec.arguments.end());
        std::vector<char*> argv;
        argv.reserve(argv_storage.size() + 1);
        for (auto& value : argv_storage) {
            argv.push_back(value.data());
        }
        argv.push_back(nullptr);

        std::vector<std::string> environment_storage = spec.environment;
        std::vector<char*> environment;
        environment.reserve(environment_storage.size() + 1);
        for (auto& value : environment_storage) {
            environment.push_back(value.data());
        }
        environment.push_back(nullptr);
        char* const* child_environment =
            spec.inherit_parent_environment ? ::environ : environment.data();

        CapturePipe stdout_pipe;
        CapturePipe stderr_pipe;
        int native_error = 0;
        if (!make_capture_pipe(stdout_pipe, native_error) ||
            (!spec.merge_stderr_into_stdout && !make_capture_pipe(stderr_pipe, native_error))) {
            set_primary_error(result, BoundedChildProcessError::pipe_failed, native_error);
            result.cleanup_complete = true;
            return result;
        }
        result.stderr_eof = spec.merge_stderr_into_stdout;

        const auto launch_prevented = [&]() noexcept {
            if (cancellation_requested(spec)) {
                result.error = BoundedChildProcessError::cancelled;
                result.cleanup_complete = true;
                return true;
            }
            if (Clock::now() >= spec.deadline) {
                result.error = BoundedChildProcessError::timeout;
                result.cleanup_complete = true;
                return true;
            }
            return false;
        };
        if (launch_prevented()) {
            return result;
        }

        const pid_t caller_process_group = ::getpgrp();
        pid_t child = -1;
        int status = 0;
        BoundedChildProcessError launch_error = BoundedChildProcessError::spawn_failed;
        bool launch_cleanup_complete = true;
        int launch_cleanup_error = 0;
#if defined(__linux__)
        if (executable_fd >= 0) {
            const DescriptorSpawnResult spawned = spawn_from_executable_fd(
                executable_fd, stdout_pipe, stderr_pipe, argv.data(), child_environment, spec);
            child = spawned.child;
            status = spawned.native_error;
            launch_error = spawned.error;
            launch_cleanup_complete = spawned.cleanup_complete;
            launch_cleanup_error = spawned.cleanup_error;
        } else
#endif
        {
            SpawnFileActions actions;
            SpawnAttributes attributes;
            status = actions.status();
            if (status == 0) {
                status = attributes.status();
            }
            if (status == 0) {
                status = add_spawn_file_actions(actions, stdout_pipe, stderr_pipe,
                                                spec.merge_stderr_into_stdout);
            }
            if (status == 0) {
                status = configure_spawn_attributes(attributes);
            }
            if (status == 0 && launch_prevented()) {
                return result;
            }
            if (status == 0) {
                status = ::posix_spawn(&child, spec.executable.c_str(), actions.get(),
                                       attributes.get(), argv.data(), child_environment);
            }
        }
        if (status != 0) {
            set_primary_error(result, launch_error, status);
            result.cleanup_complete = launch_cleanup_complete;
            if (launch_cleanup_error != 0) {
                set_cleanup_error(result, launch_cleanup_error);
            }
            return result;
        }
        result.child_started = true;
        stdout_pipe.write_end.reset();
        stderr_pipe.write_end.reset();

        bool reaped = false;
        int wait_status = 0;
        auto child_identity = observe_child_without_reaping(child);
        bool child_exited_waitable = child_identity.state == ChildIdentityState::exited_waitable;
        const bool child_identity_owned =
            child_identity.state == ChildIdentityState::running || child_exited_waitable;
        int group_validation_error = child_identity.native_error;
        bool group_verified = child > 1 && child != caller_process_group && child_identity_owned;
        pid_t spawned_group = group_verified ? child : -1;
        if (group_verified && child_exited_waitable) {
            // POSIX_SPAWN_SETPGROUP succeeded, and WNOWAIT keeps the exact
            // exited leader reserved until the group is cleaned.
            spawned_group = child;
            group_validation_error = 0;
        } else if (group_verified) {
            if (query_process_group(child, spawned_group, group_validation_error)) {
                group_verified = spawned_group == child && spawned_group != caller_process_group;
                if (!group_verified) {
                    group_validation_error = EPERM;
                }
            } else {
                const int group_query_error = group_validation_error;
                child_identity = observe_child_without_reaping(child);
                child_exited_waitable = child_identity.state == ChildIdentityState::exited_waitable;
                if (child_exited_waitable || (child_identity.state == ChildIdentityState::running &&
                                              group_query_error == ESRCH)) {
                    spawned_group = child;
                    group_validation_error = 0;
                } else {
                    group_verified = false;
                    if (child_identity.native_error != 0) {
                        group_validation_error = child_identity.native_error;
                    }
                }
            }
        }
        const pid_t verified_group = group_verified ? spawned_group : -1;
        if (!group_verified && group_validation_error == 0) {
            group_validation_error = EPERM;
        }

        bool termination_requested = false;
        bool termination_sent = false;
        bool group_cleanup_needs_verification = false;
        Clock::time_point cleanup_deadline{};
        Clock::time_point writer_deadline{};

        const auto signal_tree_once = [&]() noexcept {
            if (!termination_sent) {
                terminate_child_tree(child, verified_group, reaped, child_exited_waitable,
                                     group_cleanup_needs_verification, result);
                termination_sent = true;
                cleanup_deadline = Clock::now() + TERMINATION_CLEANUP_GRACE;
            }
        };

        const auto request_termination = [&](BoundedChildProcessError error) noexcept {
            set_primary_error(result, error);
            termination_requested = true;
            signal_tree_once();
        };

        StreamState stdout_state{&stdout_pipe.read_end,   &result.stdout_bytes,
                                 spec.stdout_limit,       &result.stdout_eof,
                                 &result.stdout_overflow, &result.stdout_read_failed};
        StreamState stderr_state{&stderr_pipe.read_end,   &result.stderr_bytes,
                                 spec.stderr_limit,       &result.stderr_eof,
                                 &result.stderr_overflow, &result.stderr_read_failed};

        if (!group_verified) {
            set_cleanup_error(result, group_validation_error);
            request_termination(BoundedChildProcessError::cleanup_failed);
        }

        while (true) {
            // Pipes are nonblocking. Observe already-published transport faults
            // and EOF before accepting a concurrent cancellation request.
            drain_stream(stdout_state, result);
            drain_stream(stderr_state, result);

            if (!reaped && !child_exited_waitable) {
                child_identity = observe_child_without_reaping(child);
                if (child_identity.state == ChildIdentityState::exited_waitable) {
                    child_exited_waitable = true;
                } else if (child_identity.state == ChildIdentityState::unavailable ||
                           child_identity.state == ChildIdentityState::failed) {
                    set_primary_error(result, BoundedChildProcessError::wait_failed,
                                      child_identity.native_error);
                    request_termination(BoundedChildProcessError::wait_failed);
                }
            }

            if (child_exited_waitable && !stdout_pipe.read_end && !stderr_pipe.read_end) {
                // Sweep the still-reserved process group before releasing the
                // leader PID. This also removes descendants that closed both
                // captured streams before outliving the direct child.
                signal_tree_once();
                const bool wait_ok = reap_without_blocking(child, wait_status, reaped, result);
                if (!wait_ok) {
                    request_termination(BoundedChildProcessError::wait_failed);
                }
                if (reaped) {
                    break;
                }
                child_exited_waitable = false;
            }

            if ((result.stdout_overflow || result.stderr_overflow || result.stdout_read_failed ||
                 result.stderr_read_failed) &&
                !termination_requested && !reaped) {
                request_termination(result.stdout_overflow || result.stderr_overflow
                                        ? BoundedChildProcessError::overflow
                                        : BoundedChildProcessError::read_failed);
            }
            if (!termination_requested && cancellation_requested(spec)) {
                request_termination(BoundedChildProcessError::cancelled);
            }

            const auto now = Clock::now();
            if (!child_exited_waitable && !reaped && !termination_requested &&
                now >= spec.deadline) {
                request_termination(BoundedChildProcessError::timeout);
            }
            if (child_exited_waitable && (!result.stdout_eof || !result.stderr_eof) &&
                !termination_requested) {
                if (writer_deadline == Clock::time_point{}) {
                    writer_deadline = now + POST_EXIT_WRITER_GRACE;
                } else if (now >= writer_deadline) {
                    request_termination(BoundedChildProcessError::descendant_writer_leak);
                }
            }

            if (termination_requested && cleanup_deadline != Clock::time_point{} &&
                now >= cleanup_deadline) {
                const bool streams_still_open = stdout_pipe.read_end || stderr_pipe.read_end;
                if (stdout_pipe.read_end) {
                    stdout_pipe.read_end.reset();
                }
                if (stderr_pipe.read_end) {
                    stderr_pipe.read_end.reset();
                }
                if (!reaped || streams_still_open) {
                    set_cleanup_error(result, ETIMEDOUT);
                }
                break;
            }

            std::array<pollfd, 2> poll_descriptors{{
                {stdout_pipe.read_end.get(), POLLIN | POLLHUP, 0},
                {stderr_pipe.read_end.get(), POLLIN | POLLHUP, 0},
            }};
            Clock::time_point next_deadline =
                termination_requested && cleanup_deadline != Clock::time_point{} ? cleanup_deadline
                                                                                 : spec.deadline;
            if (!termination_requested && writer_deadline != Clock::time_point{}) {
                next_deadline = std::min(next_deadline, writer_deadline);
            }
            const int timeout = bounded_poll_timeout(now, next_deadline);
            const int poll_status =
                ::poll(poll_descriptors.data(), poll_descriptors.size(), timeout);
            if (poll_status < 0 && errno == EINTR) {
                continue;
            }
            if (poll_status < 0) {
                const int error = errno;
                result.stdout_read_failed = true;
                result.stderr_read_failed = true;
                set_primary_error(result, BoundedChildProcessError::read_failed, error);
                request_termination(BoundedChildProcessError::read_failed);
                continue;
            }
            if (poll_descriptors[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
                if (poll_descriptors[0].revents & POLLNVAL) {
                    result.stdout_read_failed = true;
                    stdout_pipe.read_end.reset();
                    set_primary_error(result, BoundedChildProcessError::read_failed, EBADF);
                } else {
                    drain_stream(stdout_state, result);
                }
            }
            if (poll_descriptors[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
                if (poll_descriptors[1].revents & POLLNVAL) {
                    result.stderr_read_failed = true;
                    stderr_pipe.read_end.reset();
                    set_primary_error(result, BoundedChildProcessError::read_failed, EBADF);
                } else {
                    drain_stream(stderr_state, result);
                }
            }
        }

        if (!reaped) {
            pid_t waited = -1;
            do {
                waited = ::waitpid(child, &wait_status, WNOHANG);
            } while (waited < 0 && errno == EINTR);
            if (waited == child) {
                reaped = true;
            } else if (waited == 0) {
                set_cleanup_error(result, ETIMEDOUT);
            } else {
                set_cleanup_error(result, errno);
            }
        }
        if (reaped) {
            record_termination(wait_status, result);
        }
        if (reaped && group_cleanup_needs_verification &&
            wait_for_process_group_absence(verified_group, cleanup_deadline, result)) {
            group_cleanup_needs_verification = false;
        }

        result.cleanup_complete = reaped && result.stdout_eof && result.stderr_eof &&
                                  !stdout_pipe.read_end && !stderr_pipe.read_end &&
                                  !group_cleanup_needs_verification && !result.cleanup_error;
        if (!result.cleanup_complete && result.error == BoundedChildProcessError::none) {
            result.error = BoundedChildProcessError::cleanup_failed;
        }
        return result;
    } catch (const std::bad_alloc&) {
        result.error = BoundedChildProcessError::resource_failure;
        result.cleanup_complete = !result.child_started;
        return result;
    } catch (...) {
        result.error = BoundedChildProcessError::unexpected_failure;
        result.cleanup_complete = !result.child_started;
        return result;
    }
}

} // namespace

BoundedChildProcessResult run_bounded_child_process(const BoundedChildProcessSpec& spec) noexcept {
    return run_bounded_child_process_impl(spec, -1, {}, false);
}

namespace detail {

BoundedChildProcessResult run_bounded_child_process_with_argv0(const BoundedChildProcessSpec& spec,
                                                               std::string_view argv0) noexcept {
    return run_bounded_child_process_impl(spec, -1, argv0, true);
}

#if defined(__linux__)
BoundedChildProcessResult
run_bounded_child_process_from_executable_fd(const BoundedChildProcessSpec& spec, int executable_fd,
                                             std::string_view argv0) noexcept {
#if !GNFS_AUTHENTICATED_BOUNDED_CHILD_COMPILE_CAPABLE
    (void)spec;
    (void)executable_fd;
    (void)argv0;
    BoundedChildProcessResult result;
    result.error = BoundedChildProcessError::platform_unavailable;
    result.cleanup_complete = true;
    return result;
#else
    return run_bounded_child_process_impl(spec, executable_fd, argv0, true);
#endif
}
#endif

} // namespace detail

} // namespace gnfs::util

#endif
