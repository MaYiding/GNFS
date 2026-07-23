#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "bounded_child_process.hpp"

#if !defined(_WIN32)

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
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

namespace gnfs::test {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto POLL_QUANTUM = 20ms;
constexpr auto POST_EXIT_WRITER_GRACE = 200ms;
constexpr auto TERMINATION_CLEANUP_GRACE = 2s;
constexpr std::size_t STREAM_DRAIN_BUDGET = 64 * 1024;

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
        spec.deadline <= Clock::now()) {
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
                                         const CapturePipe& stderr_pipe) noexcept {
    int status =
        ::posix_spawn_file_actions_addopen(actions.get(), STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (status == 0) {
        status = ::posix_spawn_file_actions_adddup2(actions.get(), stdout_pipe.write_end.get(),
                                                    STDOUT_FILENO);
    }
    if (status == 0) {
        status = ::posix_spawn_file_actions_adddup2(actions.get(), stderr_pipe.write_end.get(),
                                                    STDERR_FILENO);
    }
    for (const int fd : {stdout_pipe.read_end.get(), stdout_pipe.write_end.get(),
                         stderr_pipe.read_end.get(), stderr_pipe.write_end.get()}) {
        if (status == 0) {
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
    if (sigemptyset(&empty_mask) != 0 || sigemptyset(&default_signals) != 0 ||
        sigaddset(&default_signals, SIGPIPE) != 0 || sigaddset(&default_signals, SIGINT) != 0 ||
        sigaddset(&default_signals, SIGTERM) != 0 || sigaddset(&default_signals, SIGHUP) != 0 ||
        sigaddset(&default_signals, SIGQUIT) != 0) {
        return errno;
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

    int status = -1;
    do {
        status = ::kill(-verified_group, SIGKILL);
    } while (status < 0 && errno == EINTR);
    if (status < 0 && errno != ESRCH) {
        const int signal_error = errno;
        if (signal_error == EPERM) {
            // Darwin reports EPERM while a process group contains only a
            // just-exited leader. Defer the verdict until that exact PID is
            // reaped, then require the group to be absent. A genuinely live,
            // unsignalable descendant therefore remains a cleanup failure.
            group_cleanup_needs_verification = true;
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

} // namespace

BoundedChildProcessResult run_bounded_child_process(const BoundedChildProcessSpec& spec) noexcept {
    BoundedChildProcessResult result;
    result.error = BoundedChildProcessError::none;

    try {
        if (!valid_spec(spec)) {
            result.error = BoundedChildProcessError::invalid_spec;
            result.cleanup_complete = true;
            return result;
        }

        result.stdout_bytes.reserve(spec.stdout_limit);
        result.stderr_bytes.reserve(spec.stderr_limit);

        std::vector<std::string> argv_storage;
        argv_storage.reserve(spec.arguments.size() + 1);
        argv_storage.push_back(spec.executable.native());
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

        CapturePipe stdout_pipe;
        CapturePipe stderr_pipe;
        int native_error = 0;
        if (!make_capture_pipe(stdout_pipe, native_error) ||
            !make_capture_pipe(stderr_pipe, native_error)) {
            set_primary_error(result, BoundedChildProcessError::pipe_failed, native_error);
            result.cleanup_complete = true;
            return result;
        }

        SpawnFileActions actions;
        SpawnAttributes attributes;
        int status = actions.status();
        if (status == 0) {
            status = attributes.status();
        }
        if (status == 0) {
            status = add_spawn_file_actions(actions, stdout_pipe, stderr_pipe);
        }
        if (status == 0) {
            status = configure_spawn_attributes(attributes);
        }
        if (status != 0) {
            set_primary_error(result, BoundedChildProcessError::spawn_failed, status);
            result.cleanup_complete = true;
            return result;
        }

        const pid_t caller_process_group = ::getpgrp();
        pid_t child = -1;
        status = ::posix_spawn(&child, argv_storage.front().c_str(), actions.get(),
                               attributes.get(), argv.data(), environment.data());
        if (status != 0) {
            set_primary_error(result, BoundedChildProcessError::spawn_failed, status);
            result.cleanup_complete = true;
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
            int poll_status = -1;
            do {
                poll_status = ::poll(poll_descriptors.data(), poll_descriptors.size(), timeout);
            } while (poll_status < 0 && errno == EINTR);
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
        if (group_cleanup_needs_verification) {
            int probe_status = -1;
            do {
                probe_status = ::kill(-verified_group, 0);
            } while (probe_status < 0 && errno == EINTR);
            if (probe_status == 0 || errno != ESRCH) {
                set_cleanup_error(result, probe_status == 0 ? EPERM : errno);
            }
        }

        result.cleanup_complete = reaped && result.stdout_eof && result.stderr_eof &&
                                  !stdout_pipe.read_end && !stderr_pipe.read_end &&
                                  !result.cleanup_error;
        if (!result.cleanup_complete && result.error == BoundedChildProcessError::none) {
            result.error = BoundedChildProcessError::cleanup_failed;
        }
        return result;
    } catch (const std::bad_alloc&) {
        result.error = BoundedChildProcessError::resource_failure;
        return result;
    } catch (...) {
        result.error = BoundedChildProcessError::unexpected_failure;
        return result;
    }
}

} // namespace gnfs::test

#endif
