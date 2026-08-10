#include "gnfs/util/native_random_access_file.hpp"

#include "gnfs/util/owned_native_file.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace gnfs::util {
namespace {

[[nodiscard]] std::string operation_message(const char* operation, const char* detail) {
    return std::string("NativeRandomAccessFile::") + operation + ": " + detail;
}

void check_transfer_range(std::uint64_t offset, std::size_t size, const char* operation) {
    const auto size64 = static_cast<std::uint64_t>(size);
    if (size64 > (std::numeric_limits<std::uint64_t>::max)() - offset) {
        throw std::overflow_error(operation_message(operation, "range exceeds uint64_t"));
    }
    const std::uint64_t end = offset + size64;
#ifdef _WIN32
    constexpr auto native_max = static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)());
#else
    constexpr auto native_max = static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)());
#endif
    if (end > native_max) {
        throw std::overflow_error(operation_message(operation, "range exceeds native file offset"));
    }
}

#ifdef _WIN32

[[noreturn]] void throw_windows_error(const char* operation, DWORD error) {
    throw std::system_error(static_cast<int>(error), std::system_category(),
                            operation_message(operation, "native operation failed"));
}

class ScopedEvent final {
public:
    explicit ScopedEvent(const char* operation)
        : handle_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (handle_ == nullptr) {
            throw_windows_error(operation, ::GetLastError());
        }
    }

    ~ScopedEvent() {
        if (handle_ != nullptr) {
            (void)::CloseHandle(handle_);
        }
    }

    ScopedEvent(const ScopedEvent&) = delete;
    ScopedEvent& operator=(const ScopedEvent&) = delete;

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

private:
    HANDLE handle_ = nullptr;
};

[[nodiscard]] DWORD transfer_overlapped(HANDLE handle, std::uint64_t offset, void* buffer,
                                        DWORD size, bool write, const char* operation) {
    ScopedEvent event(operation);
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32U);
    overlapped.hEvent = event.get();

    const BOOL started = write ? ::WriteFile(handle, buffer, size, nullptr, &overlapped)
                               : ::ReadFile(handle, buffer, size, nullptr, &overlapped);
    if (!started) {
        const DWORD error = ::GetLastError();
        if (!write && error == ERROR_HANDLE_EOF) {
            return 0;
        }
        if (error != ERROR_IO_PENDING) {
            throw_windows_error(operation, error);
        }
    }

    DWORD transferred = 0;
    if (!::GetOverlappedResult(handle, &overlapped, &transferred, TRUE)) {
        const DWORD error = ::GetLastError();
        if (!write && error == ERROR_HANDLE_EOF) {
            return 0;
        }
        throw_windows_error(operation, error);
    }
    return transferred;
}

#else

[[noreturn]] void throw_posix_error(const char* operation, int error) {
    throw std::system_error(error, std::generic_category(),
                            operation_message(operation, "native operation failed"));
}

#endif

} // namespace

struct NativeRandomAccessFile::State final {
    explicit State(bool can_write) noexcept : writable(can_write) {}

    OwnedNativeFile file;
    bool writable = false;
};

NativeRandomAccessFile::NativeRandomAccessFile(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

NativeRandomAccessFile::NativeRandomAccessFile() noexcept = default;

NativeRandomAccessFile::~NativeRandomAccessFile() = default;

NativeRandomAccessFile::NativeRandomAccessFile(NativeRandomAccessFile&&) noexcept = default;

NativeRandomAccessFile&
NativeRandomAccessFile::operator=(NativeRandomAccessFile&&) noexcept = default;

NativeRandomAccessFile NativeRandomAccessFile::create_truncated(const std::filesystem::path& path) {
    auto state = std::make_unique<State>(true);
#ifdef _WIN32
    const HANDLE handle = ::CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw_windows_error("create_truncated", ::GetLastError());
    }
    auto file = OwnedNativeFile::adopt_ownership(handle);
#else
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        throw_posix_error("create_truncated", errno);
    }
    auto file = OwnedNativeFile::adopt_ownership(descriptor);
#endif
    state->file = std::move(file);
    return NativeRandomAccessFile(std::move(state));
}

NativeRandomAccessFile NativeRandomAccessFile::open_read_only(const std::filesystem::path& path) {
    auto state = std::make_unique<State>(false);
#ifdef _WIN32
    const HANDLE handle = ::CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw_windows_error("open_read_only", ::GetLastError());
    }
    auto file = OwnedNativeFile::adopt_ownership(handle);
#else
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        throw_posix_error("open_read_only", errno);
    }
    auto file = OwnedNativeFile::adopt_ownership(descriptor);
#endif
    state->file = std::move(file);
    return NativeRandomAccessFile(std::move(state));
}

bool NativeRandomAccessFile::is_open() const noexcept {
    return state_ != nullptr && state_->file.valid();
}

std::uint64_t NativeRandomAccessFile::size() const {
    if (!is_open()) {
        throw std::logic_error(operation_message("size", "file is closed"));
    }
#ifdef _WIN32
    LARGE_INTEGER result{};
    if (!::GetFileSizeEx(state_->file.handle_, &result)) {
        throw_windows_error("size", ::GetLastError());
    }
    if (result.QuadPart < 0) {
        throw std::runtime_error(operation_message("size", "negative native file size"));
    }
    return static_cast<std::uint64_t>(result.QuadPart);
#else
    struct stat result {};
    int status = -1;
    do {
        status = ::fstat(state_->file.handle_, &result);
    } while (status != 0 && errno == EINTR);
    if (status != 0) {
        throw_posix_error("size", errno);
    }
    if (result.st_size < 0) {
        throw std::runtime_error(operation_message("size", "negative native file size"));
    }
    return static_cast<std::uint64_t>(result.st_size);
#endif
}

void NativeRandomAccessFile::read_exact_at(std::uint64_t offset,
                                           std::span<std::byte> destination) const {
    if (!is_open()) {
        throw std::logic_error(operation_message("read_exact_at", "file is closed"));
    }
    check_transfer_range(offset, destination.size(), "read_exact_at");

    std::byte* cursor = destination.data();
    std::size_t remaining = destination.size();
    while (remaining != 0) {
#ifdef _WIN32
        const auto request = static_cast<DWORD>(std::min<std::size_t>(
            remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        const DWORD transferred = transfer_overlapped(state_->file.handle_, offset, cursor, request,
                                                      false, "read_exact_at");
        const auto progressed = static_cast<std::size_t>(transferred);
#else
        const auto request = std::min<std::size_t>(
            remaining, static_cast<std::size_t>((std::numeric_limits<ssize_t>::max)()));
        ssize_t transferred = -1;
        do {
            transferred =
                ::pread(state_->file.handle_, cursor, request, static_cast<off_t>(offset));
        } while (transferred < 0 && errno == EINTR);
        if (transferred < 0) {
            throw_posix_error("read_exact_at", errno);
        }
        const auto progressed = static_cast<std::size_t>(transferred);
#endif
        if (progressed == 0) {
            throw std::runtime_error(operation_message("read_exact_at", "short read"));
        }
        cursor += progressed;
        remaining -= progressed;
        offset += static_cast<std::uint64_t>(progressed);
    }
}

void NativeRandomAccessFile::write_exact_at(std::uint64_t offset,
                                            std::span<const std::byte> source) {
    if (!is_open()) {
        throw std::logic_error(operation_message("write_exact_at", "file is closed"));
    }
    if (!state_->writable) {
        throw std::logic_error(operation_message("write_exact_at", "file is read-only"));
    }
    check_transfer_range(offset, source.size(), "write_exact_at");

    const std::byte* cursor = source.data();
    std::size_t remaining = source.size();
    while (remaining != 0) {
#ifdef _WIN32
        const auto request = static_cast<DWORD>(std::min<std::size_t>(
            remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        const DWORD transferred =
            transfer_overlapped(state_->file.handle_, offset, const_cast<std::byte*>(cursor),
                                request, true, "write_exact_at");
        const auto progressed = static_cast<std::size_t>(transferred);
#else
        const auto request = std::min<std::size_t>(
            remaining, static_cast<std::size_t>((std::numeric_limits<ssize_t>::max)()));
        ssize_t transferred = -1;
        do {
            transferred =
                ::pwrite(state_->file.handle_, cursor, request, static_cast<off_t>(offset));
        } while (transferred < 0 && errno == EINTR);
        if (transferred < 0) {
            throw_posix_error("write_exact_at", errno);
        }
        const auto progressed = static_cast<std::size_t>(transferred);
#endif
        if (progressed == 0) {
            throw std::runtime_error(operation_message("write_exact_at", "zero-progress write"));
        }
        cursor += progressed;
        remaining -= progressed;
        offset += static_cast<std::uint64_t>(progressed);
    }
}

void NativeRandomAccessFile::sync() {
    if (!is_open()) {
        throw std::logic_error(operation_message("sync", "file is closed"));
    }
    if (!state_->writable) {
        throw std::logic_error(operation_message("sync", "file is read-only"));
    }
#ifdef _WIN32
    if (!::FlushFileBuffers(state_->file.handle_)) {
        throw_windows_error("sync", ::GetLastError());
    }
#else
    int status = -1;
    do {
#if defined(__APPLE__)
        status = ::fcntl(state_->file.handle_, F_FULLFSYNC);
#else
        status = ::fsync(state_->file.handle_);
#endif
    } while (status != 0 && errno == EINTR);
    if (status != 0) {
        throw_posix_error("sync", errno);
    }
#endif
}

void NativeRandomAccessFile::close() noexcept {
    state_.reset();
}

} // namespace gnfs::util
