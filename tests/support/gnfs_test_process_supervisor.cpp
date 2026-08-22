// Test-runner process supervisor. This is intentionally a thin CLI over the
// cross-platform bounded-child transport; process containment remains owned by
// gnfs_util instead of being reimplemented in shell.

#include <gnfs/util/bounded_child_process.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>

#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <csignal>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using gnfs::util::BoundedChildProcessError;
using gnfs::util::BoundedChildProcessResult;
using gnfs::util::BoundedChildProcessSpec;
using gnfs::util::BoundedChildTerminationKind;

constexpr std::size_t DEFAULT_STREAM_LIMIT = 16 * 1024 * 1024;
constexpr int EXIT_USAGE = 64;
constexpr int EXIT_TIMEOUT = 124;
constexpr int EXIT_SUPERVISOR_FAILURE = 125;

enum class OutputMode : std::uint8_t {
    separate_streams,
    combined_stream,
    files,
};

struct Options final {
    std::chrono::milliseconds timeout{0};
    std::chrono::seconds heartbeat{0};
    std::size_t output_limit = DEFAULT_STREAM_LIMIT;
    OutputMode output_mode = OutputMode::separate_streams;
    std::filesystem::path stdout_file;
    std::filesystem::path stderr_file;
    std::filesystem::path executable;
    std::vector<std::string> arguments;
};

[[nodiscard]] bool parse_uint64(std::string_view text, std::uint64_t& value) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

#if defined(_WIN32)
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

[[nodiscard]] bool wide_to_utf8(std::wstring_view input, std::string& output) {
    if (input.empty()) {
        output.clear();
        return true;
    }
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int size =
        ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                              static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return false;
    }
    output.resize(static_cast<std::size_t>(size));
    return ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                                 static_cast<int>(input.size()), output.data(), size, nullptr,
                                 nullptr) == size;
}

[[nodiscard]] bool configure_binary_standard_streams() noexcept {
    return ::_setmode(::_fileno(stdout), _O_BINARY) >= 0 &&
           ::_setmode(::_fileno(stderr), _O_BINARY) >= 0;
}
#else
[[nodiscard]] constexpr bool configure_binary_standard_streams() noexcept {
    return true;
}
#endif

[[nodiscard]] std::optional<std::filesystem::path> path_from_utf8(std::string_view input) {
#if defined(_WIN32)
    std::wstring wide;
    if (!utf8_to_wide(input, wide)) {
        return std::nullopt;
    }
    return std::filesystem::path(std::move(wide));
#else
    return std::filesystem::path(input);
#endif
}

void print_usage(std::ostream& output, std::string_view program) {
    output << "usage: " << program
           << " --timeout-ms N [--heartbeat-seconds N] [--output-limit-bytes N]"
              " [--combined-output | --stdout-file PATH --stderr-file PATH]"
              " -- ABSOLUTE_EXECUTABLE [ARG ...]\n";
}

[[nodiscard]] bool parse_options(const std::vector<std::string>& argv, Options& options) {
    if (argv.empty()) {
        return false;
    }
    bool timeout_seen = false;
    bool combined_seen = false;
    bool stdout_file_seen = false;
    bool stderr_file_seen = false;
    std::size_t index = 1;
    for (; index < argv.size() && argv[index] != "--"; ++index) {
        const std::string_view option = argv[index];
        const auto require_value = [&]() -> const std::string* {
            if (index + 1 >= argv.size()) {
                return nullptr;
            }
            ++index;
            return &argv[index];
        };
        if (option == "--timeout-ms") {
            const std::string* value = require_value();
            std::uint64_t parsed = 0;
            if (value == nullptr || timeout_seen || !parse_uint64(*value, parsed) || parsed == 0 ||
                parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return false;
            }
            options.timeout = std::chrono::milliseconds(static_cast<std::int64_t>(parsed));
            timeout_seen = true;
        } else if (option == "--heartbeat-seconds") {
            const std::string* value = require_value();
            std::uint64_t parsed = 0;
            if (value == nullptr || !parse_uint64(*value, parsed) ||
                parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return false;
            }
            options.heartbeat = std::chrono::seconds(static_cast<std::int64_t>(parsed));
        } else if (option == "--output-limit-bytes") {
            const std::string* value = require_value();
            std::uint64_t parsed = 0;
            if (value == nullptr || !parse_uint64(*value, parsed) || parsed == 0 ||
                parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                return false;
            }
            options.output_limit = static_cast<std::size_t>(parsed);
        } else if (option == "--combined-output") {
            if (combined_seen) {
                return false;
            }
            combined_seen = true;
        } else if (option == "--stdout-file") {
            const std::string* value = require_value();
            if (value == nullptr || stdout_file_seen) {
                return false;
            }
            const auto path = path_from_utf8(*value);
            if (!path.has_value() || path->empty()) {
                return false;
            }
            options.stdout_file = *path;
            stdout_file_seen = true;
        } else if (option == "--stderr-file") {
            const std::string* value = require_value();
            if (value == nullptr || stderr_file_seen) {
                return false;
            }
            const auto path = path_from_utf8(*value);
            if (!path.has_value() || path->empty()) {
                return false;
            }
            options.stderr_file = *path;
            stderr_file_seen = true;
        } else {
            return false;
        }
    }

    if (!timeout_seen || index >= argv.size() || argv[index] != "--" || index + 1 >= argv.size() ||
        (combined_seen && (stdout_file_seen || stderr_file_seen)) ||
        stdout_file_seen != stderr_file_seen) {
        return false;
    }
    const auto executable = path_from_utf8(argv[index + 1]);
    if (!executable.has_value() || !executable->is_absolute()) {
        return false;
    }
    options.executable = *executable;
    options.arguments.assign(argv.begin() + static_cast<std::ptrdiff_t>(index + 2), argv.end());
    if (combined_seen) {
        options.output_mode = OutputMode::combined_stream;
    } else if (stdout_file_seen) {
        if (options.stdout_file.lexically_normal() == options.stderr_file.lexically_normal()) {
            return false;
        }
        options.output_mode = OutputMode::files;
    }

    const auto remaining = Clock::time_point::max() - Clock::now();
    if (options.timeout > std::chrono::duration_cast<std::chrono::milliseconds>(remaining)) {
        return false;
    }
    return true;
}

class Heartbeat final {
public:
    explicit Heartbeat(std::chrono::seconds interval) : interval_(interval) {
        if (interval_ <= std::chrono::seconds::zero()) {
            return;
        }
        try {
            worker_ = std::thread([this] { run(); });
        } catch (const std::system_error& error) {
            std::cerr << "gnfs test supervisor: heartbeat unavailable: " << error.what() << '\n';
        }
    }

    ~Heartbeat() {
        {
            std::lock_guard lock(mutex_);
            finished_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    Heartbeat(const Heartbeat&) = delete;
    Heartbeat& operator=(const Heartbeat&) = delete;

private:
    void run() {
        const auto started = Clock::now();
        std::unique_lock lock(mutex_);
        while (!condition_.wait_for(lock, interval_, [this] { return finished_; })) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count();
            lock.unlock();
            std::cerr << '[' << elapsed << "s]" << std::flush;
            lock.lock();
        }
    }

    std::chrono::seconds interval_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool finished_ = false;
    std::thread worker_;
};

#if defined(_WIN32)
volatile LONG requested_signal = 0;

BOOL WINAPI console_control_handler(DWORD control) {
    LONG signal_number = 0;
    switch (control) {
    case CTRL_C_EVENT:
        signal_number = SIGINT;
        break;
    case CTRL_BREAK_EVENT:
        signal_number = SIGTERM;
        break;
    default:
        return FALSE;
    }
    (void)::InterlockedCompareExchange(&requested_signal, signal_number, 0);
    return TRUE;
}

[[nodiscard]] bool cancellation_requested(void*) noexcept {
    return ::InterlockedCompareExchange(&requested_signal, 0, 0) != 0;
}

[[nodiscard]] int cancellation_signal() noexcept {
    return static_cast<int>(::InterlockedCompareExchange(&requested_signal, 0, 0));
}

class SignalCancellation final {
public:
    [[nodiscard]] bool install() noexcept {
        (void)::InterlockedExchange(&requested_signal, 0);
        installed_ = ::SetConsoleCtrlHandler(console_control_handler, TRUE) != 0;
        return installed_;
    }

    void uninstall() noexcept {
        if (installed_) {
            (void)::SetConsoleCtrlHandler(console_control_handler, FALSE);
            installed_ = false;
        }
    }

    ~SignalCancellation() {
        uninstall();
    }

private:
    bool installed_ = false;
};
#else
std::atomic<std::sig_atomic_t> requested_signal{0};
static_assert(decltype(requested_signal)::is_always_lock_free);

extern "C" void cancellation_signal_handler(int signal_number) {
    const int saved_errno = errno;
    std::sig_atomic_t expected = 0;
    (void)requested_signal.compare_exchange_strong(expected, signal_number,
                                                   std::memory_order_relaxed);
    errno = saved_errno;
}

[[nodiscard]] bool cancellation_requested(void*) noexcept {
    return requested_signal.load(std::memory_order_relaxed) != 0;
}

[[nodiscard]] int cancellation_signal() noexcept {
    return static_cast<int>(requested_signal.load(std::memory_order_relaxed));
}

class SignalCancellation final {
public:
    [[nodiscard]] bool install() noexcept {
        requested_signal.store(0, std::memory_order_relaxed);
        struct sigaction action{};
        action.sa_handler = cancellation_signal_handler;
        if (sigemptyset(&action.sa_mask) != 0 || sigaddset(&action.sa_mask, SIGHUP) != 0 ||
            sigaddset(&action.sa_mask, SIGINT) != 0 || sigaddset(&action.sa_mask, SIGTERM) != 0) {
            return false;
        }
        for (; installed_ < signals_.size(); ++installed_) {
            if (::sigaction(signals_[installed_], &action, &previous_[installed_]) != 0) {
                restore();
                return false;
            }
        }
        return true;
    }

    ~SignalCancellation() {
        restore();
    }

    void uninstall() noexcept {
        restore();
    }

private:
    void restore() noexcept {
        while (installed_ != 0) {
            --installed_;
            (void)::sigaction(signals_[installed_], &previous_[installed_], nullptr);
        }
    }

    const std::array<int, 3> signals_{SIGHUP, SIGINT, SIGTERM};
    std::array<struct sigaction, 3> previous_{};
    std::size_t installed_ = 0;
};
#endif

[[nodiscard]] bool truncate_file(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    return output.good();
}

[[nodiscard]] bool write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    return output.good();
}

[[nodiscard]] bool write_stream(std::ostream& output, std::string_view bytes) {
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    return output.good();
}

[[nodiscard]] bool emit_child_output(const Options& options,
                                     const BoundedChildProcessResult& result) {
    switch (options.output_mode) {
    case OutputMode::combined_stream:
        return write_stream(std::cout, result.stdout_bytes);
    case OutputMode::files:
        return write_file(options.stdout_file, result.stdout_bytes) &&
               write_file(options.stderr_file, result.stderr_bytes);
    case OutputMode::separate_streams:
        return write_stream(std::cout, result.stdout_bytes) &&
               write_stream(std::cerr, result.stderr_bytes);
    }
    return false;
}

void report_transport_failure(const BoundedChildProcessResult& result) {
    std::cerr << "gnfs test supervisor: transport="
              << gnfs::util::bounded_child_process_error_name(result.error)
              << " child_started=" << (result.child_started ? "true" : "false")
              << " cleanup_complete=" << (result.cleanup_complete ? "true" : "false");
    if (result.native_error) {
        std::cerr << " native_error=" << result.native_error.value() << ':'
                  << result.native_error.message();
    }
    if (result.cleanup_error) {
        std::cerr << " cleanup_error=" << result.cleanup_error.value() << ':'
                  << result.cleanup_error.message();
    }
    std::cerr << '\n';
}

[[nodiscard]] int map_exit_code(const BoundedChildProcessResult& result) {
    if (!result.cleanup_complete) {
        report_transport_failure(result);
        return EXIT_SUPERVISOR_FAILURE;
    }
    const int signal_number = cancellation_signal();
    if ((result.error == BoundedChildProcessError::none ||
         result.error == BoundedChildProcessError::normal_nonzero ||
         result.error == BoundedChildProcessError::signaled) &&
        signal_number > 0 && signal_number < 128) {
        return 128 + signal_number;
    }
    switch (result.error) {
    case BoundedChildProcessError::none:
        return result.succeeded() ? 0 : EXIT_SUPERVISOR_FAILURE;
    case BoundedChildProcessError::normal_nonzero:
        if (result.termination.kind == BoundedChildTerminationKind::exited &&
            result.termination.exit_code <= 255) {
            return static_cast<int>(result.termination.exit_code);
        }
        break;
    case BoundedChildProcessError::signaled:
        if (result.termination.kind == BoundedChildTerminationKind::signaled &&
            result.termination.signal > 0 && result.termination.signal < 128) {
            return 128 + result.termination.signal;
        }
        break;
    case BoundedChildProcessError::timeout:
        return EXIT_TIMEOUT;
    case BoundedChildProcessError::cancelled: {
        if (signal_number > 0 && signal_number < 128) {
            return 128 + signal_number;
        }
        break;
    }
    default:
        break;
    }
    report_transport_failure(result);
    return EXIT_SUPERVISOR_FAILURE;
}

[[nodiscard]] int supervisor_main(const std::vector<std::string>& argv) {
    if (!configure_binary_standard_streams()) {
        std::cerr << "gnfs test supervisor: could not configure binary output streams\n";
        return EXIT_SUPERVISOR_FAILURE;
    }
    Options options;
    if (!parse_options(argv, options)) {
        print_usage(std::cerr, argv.empty() ? "gnfs_test_process_supervisor" : argv.front());
        return EXIT_USAGE;
    }
    if (options.output_mode == OutputMode::files &&
        (!truncate_file(options.stdout_file) || !truncate_file(options.stderr_file))) {
        std::cerr << "gnfs test supervisor: could not initialize output files\n";
        return EXIT_SUPERVISOR_FAILURE;
    }

    SignalCancellation signals;
    if (!signals.install()) {
        std::cerr << "gnfs test supervisor: could not install cancellation handlers\n";
        return EXIT_SUPERVISOR_FAILURE;
    }

    BoundedChildProcessSpec spec;
    spec.executable = options.executable;
    spec.arguments = options.arguments;
    spec.inherit_parent_environment = true;
    spec.deadline = Clock::now() + options.timeout;
    spec.stdout_limit = options.output_limit;
    spec.stderr_limit =
        options.output_mode == OutputMode::combined_stream ? 0 : options.output_limit;
    spec.merge_stderr_into_stdout = options.output_mode == OutputMode::combined_stream;
    spec.cancellation_probe = cancellation_requested;
    spec.cancellation_context = nullptr;

    BoundedChildProcessResult result;
    {
        Heartbeat heartbeat(options.heartbeat);
        result = gnfs::util::run_bounded_child_process(spec);
    }
    const bool output_published = emit_child_output(options, result);
    signals.uninstall();
    if (!output_published) {
        std::cerr << "gnfs test supervisor: could not publish captured child output\n";
        return EXIT_SUPERVISOR_FAILURE;
    }
    return map_exit_code(result);
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> utf8_arguments;
    utf8_arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        std::string converted;
        if (argv[index] == nullptr || !wide_to_utf8(argv[index], converted)) {
            std::cerr << "gnfs test supervisor: argv is not valid Unicode\n";
            return EXIT_USAGE;
        }
        utf8_arguments.push_back(std::move(converted));
    }
    return supervisor_main(utf8_arguments);
}
#else
int main(int argc, char* argv[]) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        if (argv[index] == nullptr) {
            return EXIT_USAGE;
        }
        arguments.emplace_back(argv[index]);
    }
    return supervisor_main(arguments);
}
#endif
