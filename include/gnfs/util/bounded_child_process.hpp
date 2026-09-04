#pragma once

/// @file bounded_child_process.hpp
/// @brief Shell-free child-process transport with bounded stdout and stderr.
///
/// This utility transports bytes and termination facts only. It does not grant
/// campaign launch, persistence, or commit authority.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace gnfs::util {

enum class BoundedChildProcessError : std::uint8_t {
    none,
    invalid_spec,
    pipe_failed,
    spawn_failed,
    platform_unavailable,
    read_failed,
    overflow,
    timeout,
    cancelled,
    descendant_writer_leak,
    wait_failed,
    cleanup_failed,
    normal_nonzero,
    signaled,
    resource_failure,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view
bounded_child_process_error_name(BoundedChildProcessError error) noexcept {
    switch (error) {
    case BoundedChildProcessError::none:
        return "none";
    case BoundedChildProcessError::invalid_spec:
        return "invalid_spec";
    case BoundedChildProcessError::pipe_failed:
        return "pipe_failed";
    case BoundedChildProcessError::spawn_failed:
        return "spawn_failed";
    case BoundedChildProcessError::platform_unavailable:
        return "platform_unavailable";
    case BoundedChildProcessError::read_failed:
        return "read_failed";
    case BoundedChildProcessError::overflow:
        return "overflow";
    case BoundedChildProcessError::timeout:
        return "timeout";
    case BoundedChildProcessError::cancelled:
        return "cancelled";
    case BoundedChildProcessError::descendant_writer_leak:
        return "descendant_writer_leak";
    case BoundedChildProcessError::wait_failed:
        return "wait_failed";
    case BoundedChildProcessError::cleanup_failed:
        return "cleanup_failed";
    case BoundedChildProcessError::normal_nonzero:
        return "normal_nonzero";
    case BoundedChildProcessError::signaled:
        return "signaled";
    case BoundedChildProcessError::resource_failure:
        return "resource_failure";
    case BoundedChildProcessError::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

enum class BoundedChildTerminationKind : std::uint8_t {
    unknown,
    exited,
    signaled,
};

struct BoundedChildTermination final {
    BoundedChildTerminationKind kind = BoundedChildTerminationKind::unknown;
    std::uint32_t exit_code = 0;
    int signal = 0;
};

using BoundedChildCancellationProbe = bool (*)(void* context) noexcept;

/// Arguments exclude argv[0]; the transport inserts the absolute executable
/// path. Argument and environment strings are strict UTF-8 on Windows and raw
/// non-NUL bytes on POSIX. `environment` is the complete child environment, not
/// a delta; on Windows it may include drive-current-directory entries such as
/// `=C:=C:\work`. Set `inherit_parent_environment` only when the ambient parent
/// environment is intentionally part of the launch contract; it is mutually
/// exclusive with a non-empty `environment`. When `merge_stderr_into_stdout` is
/// set, both child streams share the stdout capture in kernel write order,
/// `stderr_limit` must be zero, and a completed result has empty `stderr_bytes`
/// with `stderr_eof` set. The optional cancellation probe may be called
/// repeatedly from the launching thread and must remain valid until this
/// function returns. Already-observable capture faults take precedence over a
/// concurrent cancellation, which in turn takes precedence over the deadline.
/// The child receives only the configured standard streams; nonstandard parent
/// descriptors or handles are not inherited, and their parent-side flags and
/// identities remain unchanged.
/// A containment failure is always reported through `cleanup_error` and
/// `cleanup_complete` without replacing an earlier primary `error`.
struct BoundedChildProcessSpec final {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::vector<std::string> environment;
    std::chrono::steady_clock::time_point deadline;
    std::size_t stdout_limit = 0;
    std::size_t stderr_limit = 0;
    bool inherit_parent_environment = false;
    bool merge_stderr_into_stdout = false;
    BoundedChildCancellationProbe cancellation_probe = nullptr;
    void* cancellation_context = nullptr;
};

struct BoundedChildProcessResult final {
    BoundedChildProcessError error = BoundedChildProcessError::unexpected_failure;
    BoundedChildTermination termination;
    std::string stdout_bytes;
    std::string stderr_bytes;
    bool child_started = false;
    bool stdout_eof = false;
    bool stderr_eof = false;
    bool stdout_overflow = false;
    bool stderr_overflow = false;
    bool stdout_read_failed = false;
    bool stderr_read_failed = false;
    bool cleanup_complete = false;
    std::error_code native_error;
    std::error_code cleanup_error;

    [[nodiscard]] bool succeeded() const noexcept {
        return error == BoundedChildProcessError::none && child_started && stdout_eof &&
               stderr_eof && cleanup_complete &&
               termination.kind == BoundedChildTerminationKind::exited &&
               termination.exit_code == 0;
    }
};

#if !defined(_WIN32)
namespace detail {

enum class PosixTerminationScope : std::uint8_t {
    none,
    direct_child,
    process_group,
};

/// Pure safety seam shared by the POSIX transport and its tests. A process
/// group may be targeted only when the spawn-time group was verified and the
/// numeric target cannot denote the caller's current group. While the direct
/// child is live, its current group must still match that verified group. An
/// exited child authorizes the group only while `waitid(..., WNOWAIT)` proves
/// that the exact child remains waitable and therefore cannot be reused. A
/// transiently unobservable group is accepted only while the same ownership
/// check proves the live child still reserves its spawn-time group identifier.
[[nodiscard]] constexpr PosixTerminationScope
select_posix_termination_scope(std::int64_t child, std::int64_t verified_group,
                               std::int64_t current_group, std::int64_t observed_child_group,
                               bool child_reaped, bool child_identity_owned,
                               bool child_exited_waitable, bool child_group_unobservable) noexcept {
    if (child <= 1 || child_reaped || !child_identity_owned) {
        return PosixTerminationScope::none;
    }
    const bool safe_group = verified_group > 1 && verified_group == child &&
                            verified_group != current_group &&
                            (child_exited_waitable || child_group_unobservable ||
                             observed_child_group == verified_group);
    if (safe_group) {
        return PosixTerminationScope::process_group;
    }
    return PosixTerminationScope::direct_child;
}

} // namespace detail
#endif

/// Launch without a command shell and capture both byte streams concurrently.
/// All failures are returned as data; the function never throws. POSIX callers
/// must retain exclusive responsibility for reaping the spawned child for the
/// duration of this call; arbitrary concurrent waitpid/waitid calls cannot be
/// made race-free with portable PID-only process APIs.
[[nodiscard]] BoundedChildProcessResult
run_bounded_child_process(const BoundedChildProcessSpec& spec) noexcept;

} // namespace gnfs::util
