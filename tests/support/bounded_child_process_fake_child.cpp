// Raw-I/O fake child for bounded_child_process integration tests. It contains
// no GNFS code and never opens a sealed holdout.

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

#if defined(_WIN32)
using NativeChar = wchar_t;
using NativeView = std::wstring_view;
#else
using NativeChar = char;
using NativeView = std::string_view;
#endif

[[nodiscard]] bool parse_size(NativeView text, std::size_t& value) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const NativeChar character : text) {
        if (character < static_cast<NativeChar>('0') || character > static_cast<NativeChar>('9')) {
            return false;
        }
        const std::uint64_t digit =
            static_cast<std::uint64_t>(character - static_cast<NativeChar>('0'));
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    if (parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    value = static_cast<std::size_t>(parsed);
    return true;
}

#if defined(_WIN32)

[[nodiscard]] bool write_all(HANDLE handle, const char* bytes, std::size_t size) noexcept {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < size) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(size - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (::WriteFile(handle, bytes + offset, chunk, &written, nullptr) == 0 || written == 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

[[nodiscard]] bool write_stdout(std::string_view bytes) noexcept {
    return write_all(::GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(), bytes.size());
}

[[nodiscard]] bool write_stderr(std::string_view bytes) noexcept {
    return write_all(::GetStdHandle(STD_ERROR_HANDLE), bytes.data(), bytes.size());
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

void close_stdout_stream() noexcept {
    const HANDLE handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    (void)::SetStdHandle(STD_OUTPUT_HANDLE, INVALID_HANDLE_VALUE);
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        (void)::CloseHandle(handle);
    }
}

void append_quoted_argument(std::wstring_view argument, std::wstring& command_line) {
    command_line.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            command_line.append(backslashes * 2 + 1, L'\\');
            command_line.push_back(L'"');
            backslashes = 0;
        } else {
            command_line.append(backslashes, L'\\');
            command_line.push_back(character);
            backslashes = 0;
        }
    }
    command_line.append(backslashes * 2, L'\\');
    command_line.push_back(L'"');
}

[[nodiscard]] bool spawn_writer_grandchild(std::wstring_view executable) {
    std::wstring command_line;
    append_quoted_argument(executable, command_line);
    command_line.append(L" \"--grandchild-sleep\"");
    command_line.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    const std::wstring application(executable);
    if (::CreateProcessW(application.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) == 0) {
        return false;
    }
    (void)::CloseHandle(process.hThread);
    (void)::CloseHandle(process.hProcess);
    return true;
}

#else

[[nodiscard]] bool write_all(int fd, const char* bytes, std::size_t size) noexcept {
    std::size_t offset = 0;
    while (offset < size) {
        ssize_t written = -1;
        do {
            written = ::write(fd, bytes + offset, size - offset);
        } while (written < 0 && errno == EINTR);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

[[nodiscard]] bool write_stdout(std::string_view bytes) noexcept {
    return write_all(STDOUT_FILENO, bytes.data(), bytes.size());
}

[[nodiscard]] bool write_stderr(std::string_view bytes) noexcept {
    return write_all(STDERR_FILENO, bytes.data(), bytes.size());
}

void close_stdout_stream() noexcept {
    while (::close(STDOUT_FILENO) < 0 && errno == EINTR) {
    }
}

[[nodiscard]] bool spawn_writer_grandchild(std::string_view) {
    const pid_t child = ::fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        std::this_thread::sleep_for(10s);
        std::_Exit(0);
    }
    return true;
}

#endif

[[nodiscard]] bool write_repeated(bool to_stdout, char byte, std::size_t size) noexcept {
    const std::string chunk(8192, byte);
    std::size_t remaining = size;
    while (remaining != 0) {
        const std::size_t amount = std::min(remaining, chunk.size());
        const bool written = to_stdout ? write_stdout(std::string_view(chunk.data(), amount))
                                       : write_stderr(std::string_view(chunk.data(), amount));
        if (!written) {
            return false;
        }
        remaining -= amount;
    }
    return true;
}

[[nodiscard]] bool write_interleaved(std::size_t total, std::size_t chunk_size) noexcept {
    if (chunk_size == 0) {
        return false;
    }
    const std::string stdout_chunk(chunk_size, 'O');
    const std::string stderr_chunk(chunk_size, 'E');
    std::size_t stdout_written = 0;
    std::size_t stderr_written = 0;
    while (stdout_written < total || stderr_written < total) {
        if (stdout_written < total) {
            const std::size_t amount = std::min(chunk_size, total - stdout_written);
            if (!write_stdout(std::string_view(stdout_chunk.data(), amount))) {
                return false;
            }
            stdout_written += amount;
        }
        if (stderr_written < total) {
            const std::size_t amount = std::min(chunk_size, total - stderr_written);
            if (!write_stderr(std::string_view(stderr_chunk.data(), amount))) {
                return false;
            }
            stderr_written += amount;
        }
    }
    return true;
}

[[nodiscard]] int fair_drain_contract() {
    constexpr std::size_t writer_count = 4;
    constexpr std::size_t chunks_per_writer = 128;
    constexpr std::size_t stderr_size = 256 * 1024;
    const std::string stdout_chunk(8192, 'F');
    std::atomic<bool> writers_ok{true};
    std::atomic<std::size_t> writers_started{0};
    std::atomic<bool> pressure_started{false};
    std::array<std::thread, writer_count> writers;
    for (auto& writer : writers) {
        writer = std::thread([&]() noexcept {
            if (!write_stdout(stdout_chunk)) {
                writers_ok.store(false, std::memory_order_release);
                return;
            }
            writers_started.fetch_add(1, std::memory_order_release);
            while (!pressure_started.load(std::memory_order_acquire) &&
                   writers_ok.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (!writers_ok.load(std::memory_order_acquire)) {
                return;
            }
            for (std::size_t chunk = 1; chunk < chunks_per_writer; ++chunk) {
                if (!write_stdout(stdout_chunk)) {
                    writers_ok.store(false, std::memory_order_release);
                    break;
                }
            }
        });
    }

    // Sanitizer scheduling can let the main thread finish stderr before any
    // writer runs. Start bounded concurrent pressure only after every writer
    // has put real data into stdout.
    while (writers_started.load(std::memory_order_acquire) != writer_count &&
           writers_ok.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    const bool all_writers_started =
        writers_started.load(std::memory_order_acquire) == writer_count;
    pressure_started.store(true, std::memory_order_release);
    const bool stderr_ok = all_writers_started && write_repeated(false, 'E', stderr_size);
    for (auto& writer : writers) {
        writer.join();
    }
    return stderr_ok && writers_ok.load(std::memory_order_relaxed) ? 0 : 66;
}

template <class Char> [[nodiscard]] int echo_contract(int argc, Char* argv[]) {
    std::string output = "argument_count=" + std::to_string(argc - 2) + "\n";
    for (int index = 2; index < argc; ++index) {
        std::string value;
#if defined(_WIN32)
        if (!wide_to_utf8(argv[index], value)) {
            return 65;
        }
#else
        value = argv[index];
#endif
        output.append("argument_");
        output.append(std::to_string(index - 2));
        output.push_back('=');
        output.append(value);
        output.push_back('\n');
    }

#if defined(_WIN32)
    const wchar_t* environment_value = ::_wgetenv(L"BCP_TEST_ENV");
    const wchar_t* parent_only = ::_wgetenv(L"BCP_PARENT_ONLY");
    std::string environment_utf8;
    if (environment_value != nullptr && !wide_to_utf8(environment_value, environment_utf8)) {
        return 65;
    }
#else
    const char* environment_value = std::getenv("BCP_TEST_ENV");
    const char* parent_only = std::getenv("BCP_PARENT_ONLY");
    const std::string environment_utf8 =
        environment_value == nullptr ? std::string{} : std::string(environment_value);
#endif
    output.append("environment=");
    output.append(environment_value == nullptr ? "<missing>" : environment_utf8);
    output.push_back('\n');
    output.append("parent_only=");
    output.append(parent_only == nullptr ? "<missing>\n" : "<present>\n");
    return write_stdout(output) ? 0 : 66;
}

#if defined(_WIN32)
[[nodiscard]] int environment_order_contract() {
    wchar_t* environment = ::GetEnvironmentStringsW();
    if (environment == nullptr) {
        return 65;
    }
    std::string output;
    bool valid = true;
    for (const wchar_t* cursor = environment; *cursor != L'\0';) {
        const std::wstring_view entry(cursor);
        if (entry.starts_with(L"BCP_SORT_")) {
            std::string converted;
            if (!wide_to_utf8(entry, converted)) {
                valid = false;
                break;
            }
            output.append(converted);
            output.push_back('\n');
        }
        cursor += entry.size() + 1;
    }
    (void)::FreeEnvironmentStringsW(environment);
    return valid && write_stdout(output) ? 0 : 66;
}
#endif

template <class Char> int fake_child_main(int argc, Char* argv[]) {
    if (argc < 2 || argv[1] == nullptr) {
        return 64;
    }
    const NativeView mode(argv[1]);
    if (mode == NativeView(
#if defined(_WIN32)
                    L"--grandchild-sleep"
#else
                    "--grandchild-sleep"
#endif
                    )) {
        std::this_thread::sleep_for(10s);
        return 0;
    }
    if (mode == NativeView(
#if defined(_WIN32)
                    L"--hang"
#else
                    "--hang"
#endif
                    )) {
        std::this_thread::sleep_for(10s);
        return 0;
    }
    if (mode == NativeView(
#if defined(_WIN32)
                    L"--nonzero"
#else
                    "--nonzero"
#endif
                    )) {
        (void)write_stdout("stdout-before-nonzero\n");
        (void)write_stderr("stderr-before-nonzero\n");
        return 23;
    }
    if (mode == NativeView(
#if defined(_WIN32)
                    L"--signal"
#else
                    "--signal"
#endif
                    )) {
#if defined(_WIN32)
        ::ExitProcess(UINT32_C(0xc0000409));
#else
        (void)::raise(SIGTERM);
#endif
        return 67;
    }
    if (mode == NativeView(
#if defined(_WIN32)
                    L"--close-stdout-hang"
#else
                    "--close-stdout-hang"
#endif
                    )) {
        close_stdout_stream();
        std::this_thread::sleep_for(10s);
        return 0;
    }
    if (mode == NativeView(
#if defined(_WIN32)
                    L"--descendant-writer"
#else
                    "--descendant-writer"
#endif
                    )) {
        return spawn_writer_grandchild(argv[0]) ? 0 : 68;
    }
    if (mode == NativeView(
#if defined(_WIN32)
                    L"--echo"
#else
                    "--echo"
#endif
                    )) {
        return echo_contract(argc, argv);
    }
    if (mode == NativeView(
#if defined(_WIN32)
                    L"--argv0"
#else
                    "--argv0"
#endif
                    )) {
        std::string argv0;
#if defined(_WIN32)
        if (!wide_to_utf8(argv[0], argv0)) {
            return 65;
        }
#else
        argv0 = argv[0];
#endif
        argv0.push_back('\n');
        return write_stdout(argv0) ? 0 : 66;
    }
#if !defined(_WIN32)
    if (mode == NativeView("--signal-dispositions")) {
        struct sigaction action {};
        for (const int signal_number : {SIGUSR1, SIGALRM, SIGCHLD}) {
            if (::sigaction(signal_number, nullptr, &action) != 0 || action.sa_handler != SIG_DFL) {
                return 70;
            }
        }
        return write_stdout("default\n") ? 0 : 71;
    }
#endif
#if defined(__linux__)
    if (mode == NativeView("--check-fd-closed")) {
        if (argc != 3) {
            return 64;
        }
        std::size_t descriptor = 0;
        if (!parse_size(NativeView(argv[2]), descriptor) ||
            descriptor > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return 64;
        }
        errno = 0;
        const int status = ::fcntl(static_cast<int>(descriptor), F_GETFD);
        return status < 0 && errno == EBADF && write_stdout("closed\n") ? 0 : 69;
    }
    if (mode == NativeView("--pid-ledger-hang")) {
        if (argc != 3) {
            return 64;
        }
        const pid_t pid = ::getpid();
        const pid_t parent = ::getppid();
        const pid_t group = ::getpgid(0);
        if (pid <= 1 || parent <= 1 || group <= 1) {
            return 72;
        }
        const std::string destination(argv[2]);
        const std::string pending = destination + ".tmp." + std::to_string(pid);
        int ledger = -1;
        do {
            ledger = ::open(pending.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        } while (ledger < 0 && errno == EINTR);
        if (ledger < 0) {
            return 73;
        }
        const std::string bytes = "pid=" + std::to_string(pid) +
                                  "\nppid=" + std::to_string(parent) +
                                  "\npgid=" + std::to_string(group) + "\n";
        bool published = write_all(ledger, bytes.data(), bytes.size()) && ::fsync(ledger) == 0;
        const int close_status = ::close(ledger);
        ledger = -1;
        published =
            published && close_status == 0 && ::rename(pending.c_str(), destination.c_str()) == 0;
        if (!published) {
            if (ledger >= 0) {
                (void)::close(ledger);
            }
            (void)::unlink(pending.c_str());
            return 74;
        }
        for (;;) {
            (void)::pause();
        }
    }
#endif
    if (mode == NativeView(
#if defined(_WIN32)
                    L"--flood-stdout"
#else
                    "--flood-stdout"
#endif
                    )) {
        const std::string chunk(8192, 'F');
        while (write_stdout(chunk)) {
        }
        return 66;
    }
    if (mode == NativeView(
#if defined(_WIN32)
                    L"--fair-drain"
#else
                    "--fair-drain"
#endif
                    )) {
        return fair_drain_contract();
    }
#if defined(_WIN32)
    if (mode == NativeView(L"--environment-order")) {
        return environment_order_contract();
    }
#endif

    if (argc != 4) {
        return 64;
    }
    std::size_t first = 0;
    std::size_t second = 0;
    if (!parse_size(NativeView(argv[2]), first) || !parse_size(NativeView(argv[3]), second)) {
        return 64;
    }
    if (mode == NativeView(
#if defined(_WIN32)
                    L"--write-sizes"
#else
                    "--write-sizes"
#endif
                    )) {
        return write_repeated(true, 'O', first) && write_repeated(false, 'E', second) ? 0 : 66;
    }
    if (mode == NativeView(
#if defined(_WIN32)
                    L"--stdout-first"
#else
                    "--stdout-first"
#endif
                    )) {
        return write_repeated(true, 'O', first) && write_repeated(false, 'E', second) ? 0 : 66;
    }
    if (mode == NativeView(
#if defined(_WIN32)
                    L"--stderr-first"
#else
                    "--stderr-first"
#endif
                    )) {
        return write_repeated(false, 'E', second) && write_repeated(true, 'O', first) ? 0 : 66;
    }
    if (mode == NativeView(
#if defined(_WIN32)
                    L"--interleaved"
#else
                    "--interleaved"
#endif
                    )) {
        return write_interleaved(first, second) ? 0 : 66;
    }
    return 64;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[]) {
    return fake_child_main(argc, argv);
}
#else
int main(int argc, char* argv[]) {
    return fake_child_main(argc, argv);
}
#endif
