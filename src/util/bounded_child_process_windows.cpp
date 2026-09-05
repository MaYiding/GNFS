#include <gnfs/util/bounded_child_process.hpp>

#if defined(_WIN32)

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace gnfs::util {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto POST_EXIT_WRITER_GRACE = 200ms;
constexpr auto TERMINATION_CLEANUP_GRACE = 2s;
constexpr auto IO_POLL_QUANTUM = 10ms;
constexpr std::size_t STREAM_DRAIN_BUDGET = 64 * 1024;
#if defined(PROC_THREAD_ATTRIBUTE_JOB_LIST)
constexpr DWORD_PTR JOB_LIST_ATTRIBUTE = PROC_THREAD_ATTRIBUTE_JOB_LIST;
#else
// Win10's input-only ProcThreadAttributeValue(13, false, true, false).
// Some supported MinGW-w64 SDK revisions omit the symbolic constant.
constexpr DWORD_PTR JOB_LIST_ATTRIBUTE = 0x0002000D;
#endif

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE handle = nullptr) noexcept {
        if (*this) {
            (void)::CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

class AttributeList final {
public:
    AttributeList() = default;
    ~AttributeList() {
        if (list_ != nullptr) {
            ::DeleteProcThreadAttributeList(list_);
        }
    }

    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;

    [[nodiscard]] bool initialize(DWORD attribute_count) {
        SIZE_T bytes = 0;
        (void)::InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &bytes);
        if (bytes == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return false;
        }
        storage_.resize(bytes);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (::InitializeProcThreadAttributeList(list_, attribute_count, 0, &bytes) == 0) {
            list_ = nullptr;
            return false;
        }
        return true;
    }

    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() noexcept {
        return list_;
    }

private:
    std::vector<std::byte> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

struct CapturePipe final {
    UniqueHandle read_end;
    UniqueHandle write_end;
};

struct StreamState final {
    UniqueHandle* read_end = nullptr;
    std::string* bytes = nullptr;
    std::size_t limit = 0;
    bool* eof = nullptr;
    bool* overflow = nullptr;
    bool* failed = nullptr;
    DWORD native_error = ERROR_SUCCESS;
};

[[nodiscard]] bool has_embedded_nul(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] bool utf8_to_wide(std::string_view input, std::wstring& output) {
    if (input.empty()) {
        output.clear();
        return true;
    }
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                           static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) {
        return false;
    }
    output.resize(static_cast<std::size_t>(size));
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                 static_cast<int>(input.size()), output.data(), size) == size;
}

[[nodiscard]] bool same_environment_name(std::wstring_view left, std::wstring_view right) noexcept {
    if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                  static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] std::size_t environment_name_end(std::wstring_view entry) noexcept {
    if (entry.starts_with(L'=')) {
        const bool drive_letter = entry.size() >= 4 && ((entry[1] >= L'A' && entry[1] <= L'Z') ||
                                                        (entry[1] >= L'a' && entry[1] <= L'z'));
        return drive_letter && entry[2] == L':' && entry[3] == L'=' ? 3 : std::wstring_view::npos;
    }
    const std::size_t separator = entry.find(L'=');
    return separator == 0 ? std::wstring_view::npos : separator;
}

[[nodiscard]] bool validate_and_convert_environment(const std::vector<std::string>& input,
                                                    std::vector<std::wstring>& output) {
    output.clear();
    output.reserve(input.size());
    for (const auto& entry : input) {
        if (has_embedded_nul(entry)) {
            return false;
        }
        std::wstring converted;
        if (!utf8_to_wide(entry, converted) ||
            environment_name_end(converted) == std::wstring_view::npos) {
            return false;
        }
        output.push_back(std::move(converted));
    }
    std::vector<std::wstring_view> names;
    names.reserve(output.size());
    for (const auto& entry : output) {
        const std::size_t separator = environment_name_end(entry);
        names.emplace_back(entry.data(), separator);
    }
    for (std::size_t index = 0; index < names.size(); ++index) {
        for (std::size_t other = 0; other < index; ++other) {
            if (same_environment_name(names[index], names[other])) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool valid_and_convert_spec(const BoundedChildProcessSpec& spec,
                                          std::wstring& executable,
                                          std::vector<std::wstring>& arguments,
                                          std::vector<std::wstring>& environment) {
    if (spec.executable.empty() || !spec.executable.is_absolute() ||
        spec.deadline <= Clock::now() ||
        (spec.inherit_parent_environment && !spec.environment.empty()) ||
        (spec.merge_stderr_into_stdout && spec.stderr_limit != 0)) {
        return false;
    }
    executable = spec.executable.native();
    if (executable.find(L'\0') != std::wstring::npos) {
        return false;
    }
    arguments.clear();
    arguments.reserve(spec.arguments.size() + 1);
    arguments.push_back(executable);
    for (const auto& argument : spec.arguments) {
        if (has_embedded_nul(argument)) {
            return false;
        }
        std::wstring converted;
        if (!utf8_to_wide(argument, converted)) {
            return false;
        }
        arguments.push_back(std::move(converted));
    }
    if (spec.inherit_parent_environment) {
        environment.clear();
        return true;
    }
    return validate_and_convert_environment(spec.environment, environment);
}

[[nodiscard]] bool cancellation_requested(const BoundedChildProcessSpec& spec) noexcept {
    return spec.cancellation_probe != nullptr && spec.cancellation_probe(spec.cancellation_context);
}

void append_quoted_argument(std::wstring_view argument, std::wstring& command_line) {
    command_line.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            command_line.append(backslashes * 2 + 1, L'\\');
            command_line.push_back(L'"');
            backslashes = 0;
            continue;
        }
        command_line.append(backslashes, L'\\');
        backslashes = 0;
        command_line.push_back(character);
    }
    command_line.append(backslashes * 2, L'\\');
    command_line.push_back(L'"');
}

[[nodiscard]] std::vector<wchar_t> make_command_line(const std::vector<std::wstring>& arguments) {
    std::wstring value;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index != 0) {
            value.push_back(L' ');
        }
        append_quoted_argument(arguments[index], value);
    }
    std::vector<wchar_t> mutable_value(value.begin(), value.end());
    mutable_value.push_back(L'\0');
    return mutable_value;
}

[[nodiscard]] std::vector<wchar_t> make_environment_block(std::vector<std::wstring> environment) {
    std::sort(environment.begin(), environment.end(),
              [](const std::wstring& left, const std::wstring& right) {
                  const std::size_t left_separator = environment_name_end(left);
                  const std::size_t right_separator = environment_name_end(right);
                  return ::CompareStringOrdinal(left.data(), static_cast<int>(left_separator),
                                                right.data(), static_cast<int>(right_separator),
                                                TRUE) == CSTR_LESS_THAN;
              });
    std::vector<wchar_t> block;
    std::size_t size = 1;
    for (const auto& entry : environment) {
        size += entry.size() + 1;
    }
    if (environment.empty()) {
        ++size;
    }
    block.reserve(size);
    for (const auto& entry : environment) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (environment.empty()) {
        block.push_back(L'\0');
    }
    return block;
}

void set_primary_error(BoundedChildProcessResult& result, BoundedChildProcessError error,
                       DWORD native_error = ERROR_SUCCESS) noexcept {
    if (result.error != BoundedChildProcessError::none) {
        return;
    }
    result.error = error;
    if (native_error != ERROR_SUCCESS) {
        result.native_error =
            std::error_code(static_cast<int>(native_error), std::system_category());
    }
}

void set_cleanup_error(BoundedChildProcessResult& result, DWORD native_error) noexcept {
    if (native_error != ERROR_SUCCESS && !result.cleanup_error) {
        result.cleanup_error =
            std::error_code(static_cast<int>(native_error), std::system_category());
    }
    if (result.error == BoundedChildProcessError::none) {
        result.error = BoundedChildProcessError::cleanup_failed;
        result.native_error = result.cleanup_error;
    }
}

[[nodiscard]] bool make_capture_pipe(CapturePipe& pipe, DWORD& native_error) noexcept {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (::CreatePipe(&read_handle, &write_handle, &security, 0) == 0) {
        native_error = ::GetLastError();
        return false;
    }
    pipe.read_end.reset(read_handle);
    pipe.write_end.reset(write_handle);
    if (::SetHandleInformation(pipe.read_end.get(), HANDLE_FLAG_INHERIT, 0) == 0) {
        native_error = ::GetLastError();
        return false;
    }
    return true;
}

[[nodiscard]] bool is_pipe_eof_error(DWORD error) noexcept {
    return error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF ||
           error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA;
}

void record_pipe_eof(StreamState& state) noexcept {
    state.read_end->reset();
    *state.eof = true;
}

void record_pipe_failure(StreamState& state, DWORD error) noexcept {
    state.native_error = error;
    *state.failed = true;
    state.read_end->reset();
}

[[nodiscard]] bool drain_available(StreamState& state) {
    if (!*state.read_end) {
        return false;
    }

    std::array<char, 8192> buffer{};
    std::size_t budget = STREAM_DRAIN_BUDGET;
    while (budget != 0) {
        DWORD available = 0;
        if (::PeekNamedPipe(state.read_end->get(), nullptr, 0, nullptr, &available, nullptr) == 0) {
            const DWORD error = ::GetLastError();
            if (is_pipe_eof_error(error)) {
                record_pipe_eof(state);
            } else {
                record_pipe_failure(state, error);
            }
            return false;
        }
        if (available == 0) {
            return false;
        }

        // This process owns the only read handle. Bytes reported as available
        // cannot be consumed elsewhere, so reading no more than that count
        // keeps the synchronous ReadFile from waiting for a writer.
        const std::size_t request_size =
            std::min({budget, buffer.size(), static_cast<std::size_t>(available)});
        DWORD read_count = 0;
        if (::ReadFile(state.read_end->get(), buffer.data(), static_cast<DWORD>(request_size),
                       &read_count, nullptr) == 0) {
            const DWORD error = ::GetLastError();
            if (is_pipe_eof_error(error)) {
                record_pipe_eof(state);
            } else {
                record_pipe_failure(state, error);
            }
            return false;
        }
        if (read_count == 0) {
            record_pipe_eof(state);
            return false;
        }

        const std::size_t amount = static_cast<std::size_t>(read_count);
        const std::size_t capture_available = state.limit - state.bytes->size();
        const std::size_t retained = std::min(amount, capture_available);
        if (retained != 0) {
            state.bytes->append(buffer.data(), retained);
        }
        if (retained != amount) {
            *state.overflow = true;
        }
        budget -= amount;
    }
    // Revisit both streams and the deadlines before doing another bounded
    // drain round instead of sleeping while a pipe may still have backlog.
    return true;
}

[[nodiscard]] DWORD timeout_until(Clock::time_point deadline) noexcept {
    const auto now = Clock::now();
    if (deadline <= now) {
        return 0;
    }
    const auto remaining = deadline - now;
    const auto whole_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    std::uint64_t milliseconds = static_cast<std::uint64_t>(whole_ms.count());
    if (whole_ms < remaining) {
        ++milliseconds;
    }
    return static_cast<DWORD>(
        std::min<std::uint64_t>(milliseconds, static_cast<std::uint64_t>(INFINITE - 1)));
}

void terminate_job(HANDLE job, BoundedChildProcessResult& result) noexcept {
    if (::TerminateJobObject(job, ERROR_PROCESS_ABORTED) == 0) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_ACCESS_DENIED) {
            set_cleanup_error(result, error);
        }
    }
}

void terminate_direct_process(HANDLE process, BoundedChildProcessResult& result) noexcept {
    if (::TerminateProcess(process, ERROR_PROCESS_ABORTED) == 0) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_ACCESS_DENIED || ::WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
            set_cleanup_error(result, error);
        }
    }
}

[[nodiscard]] bool query_job_empty(HANDLE job, bool& empty, DWORD& native_error) noexcept {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (::QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting,
                                    sizeof(accounting), nullptr) == 0) {
        native_error = ::GetLastError();
        return false;
    }
    empty = accounting.ActiveProcesses == 0;
    return true;
}

void apply_stream_error(const StreamState& stdout_state, const StreamState& stderr_state,
                        BoundedChildProcessResult& result) noexcept {
    if (result.stdout_overflow || result.stderr_overflow) {
        set_primary_error(result, BoundedChildProcessError::overflow);
    } else if (result.stdout_read_failed || result.stderr_read_failed) {
        const DWORD error =
            result.stdout_read_failed ? stdout_state.native_error : stderr_state.native_error;
        set_primary_error(result, BoundedChildProcessError::read_failed, error);
    }
}

} // namespace

BoundedChildProcessResult run_bounded_child_process(const BoundedChildProcessSpec& spec) noexcept {
    BoundedChildProcessResult result;
    result.error = BoundedChildProcessError::none;

    try {
        std::wstring executable;
        std::vector<std::wstring> arguments;
        std::vector<std::wstring> environment;
        if (!valid_and_convert_spec(spec, executable, arguments, environment)) {
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
        auto command_line = make_command_line(arguments);
        std::vector<wchar_t> environment_block;
        void* child_environment = nullptr;
        if (!spec.inherit_parent_environment) {
            environment_block = make_environment_block(std::move(environment));
            child_environment = environment_block.data();
        }

        CapturePipe stdout_pipe;
        CapturePipe stderr_pipe;
        DWORD native_error = ERROR_SUCCESS;
        if (!make_capture_pipe(stdout_pipe, native_error) ||
            (!spec.merge_stderr_into_stdout && !make_capture_pipe(stderr_pipe, native_error))) {
            set_primary_error(result, BoundedChildProcessError::pipe_failed, native_error);
            result.cleanup_complete = true;
            return result;
        }
        result.stderr_eof = spec.merge_stderr_into_stdout;

        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        UniqueHandle null_input(::CreateFileW(L"NUL", GENERIC_READ,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!null_input) {
            set_primary_error(result, BoundedChildProcessError::pipe_failed, ::GetLastError());
            result.cleanup_complete = true;
            return result;
        }

        UniqueHandle job(::CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
        job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || ::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                              &job_limits, sizeof(job_limits)) == 0) {
            const DWORD error = ::GetLastError();
            set_primary_error(result, BoundedChildProcessError::spawn_failed, error);
            result.cleanup_complete = true;
            return result;
        }

        std::array<HANDLE, 3> inherited_handles{null_input.get(), stdout_pipe.write_end.get(),
                                                stderr_pipe.write_end.get()};
        const std::size_t inherited_handle_count = spec.merge_stderr_into_stdout ? 2 : 3;
        std::array<HANDLE, 1> child_jobs{job.get()};
        // Both attribute value arrays must outlive the attribute list.
        AttributeList attributes;
        if (!attributes.initialize(2)) {
            const DWORD error = ::GetLastError();
            set_primary_error(result, BoundedChildProcessError::spawn_failed, error);
            result.cleanup_complete = true;
            return result;
        }
        if (::UpdateProcThreadAttribute(
                attributes.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited_handles.data(),
                inherited_handle_count * sizeof(HANDLE), nullptr, nullptr) == 0) {
            const DWORD error = ::GetLastError();
            set_primary_error(result, BoundedChildProcessError::spawn_failed, error);
            result.cleanup_complete = true;
            return result;
        }
        if (::UpdateProcThreadAttribute(attributes.get(), 0, JOB_LIST_ATTRIBUTE, child_jobs.data(),
                                        child_jobs.size() * sizeof(HANDLE), nullptr,
                                        nullptr) == 0) {
            const DWORD error = ::GetLastError();
            set_primary_error(result,
                              error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_PARAMETER
                                  ? BoundedChildProcessError::platform_unavailable
                                  : BoundedChildProcessError::spawn_failed,
                              error);
            result.cleanup_complete = true;
            return result;
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = null_input.get();
        startup.StartupInfo.hStdOutput = stdout_pipe.write_end.get();
        startup.StartupInfo.hStdError = spec.merge_stderr_into_stdout ? stdout_pipe.write_end.get()
                                                                      : stderr_pipe.write_end.get();
        startup.lpAttributeList = attributes.get();

        if (cancellation_requested(spec)) {
            result.error = BoundedChildProcessError::cancelled;
            result.cleanup_complete = true;
            return result;
        }
        if (Clock::now() >= spec.deadline) {
            result.error = BoundedChildProcessError::timeout;
            result.cleanup_complete = true;
            return result;
        }

        PROCESS_INFORMATION process_information{};
        DWORD creation_flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
        if (!spec.inherit_parent_environment) {
            creation_flags |= CREATE_UNICODE_ENVIRONMENT;
        }
        if (::CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                             creation_flags, child_environment, nullptr, &startup.StartupInfo,
                             &process_information) == 0) {
            const DWORD error = ::GetLastError();
            set_primary_error(result, BoundedChildProcessError::spawn_failed, error);
            result.cleanup_complete = true;
            return result;
        }
        result.child_started = true;
        UniqueHandle process(process_information.hProcess);
        UniqueHandle primary_thread(process_information.hThread);

        // PROC_THREAD_ATTRIBUTE_JOB_LIST makes containment atomic with process
        // creation. There is no post-CreateProcess crash window in which the
        // suspended child exists outside the kill-on-close Job.
        constexpr bool process_in_job = true;

        if (process_in_job && result.error == BoundedChildProcessError::none &&
            cancellation_requested(spec)) {
            set_primary_error(result, BoundedChildProcessError::cancelled);
        } else if (process_in_job && result.error == BoundedChildProcessError::none &&
                   Clock::now() >= spec.deadline) {
            set_primary_error(result, BoundedChildProcessError::timeout);
        }

        stdout_pipe.write_end.reset();
        stderr_pipe.write_end.reset();
        null_input.reset();
        if (process_in_job && result.error == BoundedChildProcessError::none &&
            ::ResumeThread(primary_thread.get()) == static_cast<DWORD>(-1)) {
            const DWORD error = ::GetLastError();
            set_primary_error(result, BoundedChildProcessError::spawn_failed, error);
        }
        primary_thread.reset();

        StreamState stdout_state{&stdout_pipe.read_end,   &result.stdout_bytes,
                                 spec.stdout_limit,       &result.stdout_eof,
                                 &result.stdout_overflow, &result.stdout_read_failed};
        StreamState stderr_state{&stderr_pipe.read_end,   &result.stderr_bytes,
                                 spec.stderr_limit,       &result.stderr_eof,
                                 &result.stderr_overflow, &result.stderr_read_failed};

        bool process_finished = false;
        bool job_empty = !process_in_job;
        bool termination_requested = result.error != BoundedChildProcessError::none;
        bool termination_sent = false;
        Clock::time_point cleanup_deadline{};
        Clock::time_point writer_deadline{};

        const auto request_termination = [&](BoundedChildProcessError error) noexcept {
            set_primary_error(result, error);
            termination_requested = true;
            if (!termination_sent) {
                if (process_in_job) {
                    terminate_job(job.get(), result);
                } else {
                    terminate_direct_process(process.get(), result);
                }
                termination_sent = true;
                cleanup_deadline = Clock::now() + TERMINATION_CLEANUP_GRACE;
            }
        };

        if (termination_requested) {
            request_termination(result.error);
        }

        while (true) {
            const bool stdout_backlog = drain_available(stdout_state);
            const bool stderr_backlog = drain_available(stderr_state);
            const bool stream_backlog = stdout_backlog || stderr_backlog;
            apply_stream_error(stdout_state, stderr_state, result);

            if ((result.stdout_overflow || result.stderr_overflow || result.stdout_read_failed ||
                 result.stderr_read_failed) &&
                !termination_requested) {
                request_termination(result.stdout_overflow || result.stderr_overflow
                                        ? BoundedChildProcessError::overflow
                                        : BoundedChildProcessError::read_failed);
            }

            if (!process_finished) {
                const DWORD process_status = ::WaitForSingleObject(process.get(), 0);
                if (process_status == WAIT_OBJECT_0) {
                    process_finished = true;
                } else if (process_status == WAIT_FAILED) {
                    const DWORD error = ::GetLastError();
                    if (result.error == BoundedChildProcessError::none) {
                        set_primary_error(result, BoundedChildProcessError::wait_failed, error);
                    } else {
                        set_cleanup_error(result, error);
                    }
                    request_termination(BoundedChildProcessError::wait_failed);
                }
            }

            if (process_finished && !stdout_pipe.read_end && !stderr_pipe.read_end &&
                !termination_requested) {
                // Sweep descendants that closed both streams before the
                // direct child exited, then prove the Job is empty.
                request_termination(BoundedChildProcessError::none);
            }
            if (!termination_requested && cancellation_requested(spec)) {
                request_termination(BoundedChildProcessError::cancelled);
            }

            if (termination_requested && process_in_job) {
                DWORD job_query_error = ERROR_SUCCESS;
                if (!query_job_empty(job.get(), job_empty, job_query_error)) {
                    set_cleanup_error(result, job_query_error);
                }
            }
            if (process_finished && !stdout_pipe.read_end && !stderr_pipe.read_end && job_empty) {
                break;
            }

            const auto now = Clock::now();
            if (!process_finished && !termination_requested && now >= spec.deadline) {
                request_termination(BoundedChildProcessError::timeout);
            }
            if (process_finished && (stdout_pipe.read_end || stderr_pipe.read_end) &&
                !termination_requested) {
                if (writer_deadline == Clock::time_point{}) {
                    writer_deadline = now + POST_EXIT_WRITER_GRACE;
                } else if (now >= writer_deadline) {
                    request_termination(BoundedChildProcessError::descendant_writer_leak);
                }
            }

            if (termination_requested && cleanup_deadline != Clock::time_point{} &&
                now >= cleanup_deadline) {
                if (stdout_pipe.read_end || stderr_pipe.read_end) {
                    set_cleanup_error(result, ERROR_TIMEOUT);
                    stdout_pipe.read_end.reset();
                    stderr_pipe.read_end.reset();
                }
                if (!process_finished) {
                    set_cleanup_error(result, ERROR_TIMEOUT);
                }
                if (process_in_job && !job_empty) {
                    set_cleanup_error(result, ERROR_TIMEOUT);
                }
                break;
            }
            if (stream_backlog) {
                (void)::SwitchToThread();
                continue;
            }

            Clock::time_point next_deadline =
                termination_requested && cleanup_deadline != Clock::time_point{} ? cleanup_deadline
                                                                                 : spec.deadline;
            if (!termination_requested && writer_deadline != Clock::time_point{}) {
                next_deadline = std::min(next_deadline, writer_deadline);
            }
            const auto wake_deadline = std::min(next_deadline, now + IO_POLL_QUANTUM);
            const DWORD wait_timeout = timeout_until(wake_deadline);
            if (!process_finished) {
                const DWORD wait_status = ::WaitForSingleObject(process.get(), wait_timeout);
                if (wait_status == WAIT_OBJECT_0) {
                    process_finished = true;
                } else if (wait_status == WAIT_FAILED) {
                    const DWORD error = ::GetLastError();
                    if (result.error == BoundedChildProcessError::none) {
                        set_primary_error(result, BoundedChildProcessError::wait_failed, error);
                    } else {
                        set_cleanup_error(result, error);
                    }
                    request_termination(BoundedChildProcessError::wait_failed);
                }
            } else if (wait_timeout != 0) {
                ::Sleep(wait_timeout);
            } else {
                (void)::SwitchToThread();
            }
        }

        DWORD exit_code = STILL_ACTIVE;
        if (process_finished && ::GetExitCodeProcess(process.get(), &exit_code) != 0) {
            result.termination.kind = BoundedChildTerminationKind::exited;
            result.termination.exit_code = static_cast<std::uint32_t>(exit_code);
            if (result.error == BoundedChildProcessError::none && exit_code != 0) {
                result.error = BoundedChildProcessError::normal_nonzero;
            }
        } else if (process_finished) {
            const DWORD error = ::GetLastError();
            if (result.error == BoundedChildProcessError::none) {
                set_primary_error(result, BoundedChildProcessError::wait_failed, error);
            } else {
                set_cleanup_error(result, error);
            }
        }

        result.cleanup_complete = process_finished && result.stdout_eof && result.stderr_eof &&
                                  (!process_in_job || job_empty) && !result.cleanup_error;
        if (!result.cleanup_complete && result.error == BoundedChildProcessError::none) {
            result.error = BoundedChildProcessError::cleanup_failed;
        }
        return result;
    } catch (const std::bad_alloc&) {
        result.error = BoundedChildProcessError::resource_failure;
    } catch (...) {
        result.error = BoundedChildProcessError::unexpected_failure;
    }
    result.cleanup_complete = !result.child_started;
    return result;
}

} // namespace gnfs::util

#endif
