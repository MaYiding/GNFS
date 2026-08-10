#pragma once

#include <cerrno>
#include <cstdio>
#include <stdexcept>
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
#include <unistd.h>
#endif

namespace gnfs::util {

class MmapFile;
class NativeRandomAccessFile;

/// Move-only ownership of one already-open native file handle.
///
/// adopt_ownership() transfers close responsibility into this object. There is
/// deliberately no release() or raw-handle accessor: the only supported
/// transfer is into another OwnedNativeFile or a consuming MmapFile
/// constructor.
class OwnedNativeFile final {
public:
#ifdef _WIN32
    using NativeHandle = HANDLE;
#else
    using NativeHandle = int;
#endif

    OwnedNativeFile() noexcept = default;

    [[nodiscard]] static OwnedNativeFile adopt_ownership(NativeHandle handle) {
        if (!native_handle_valid(handle)) {
            throw std::invalid_argument("OwnedNativeFile: cannot adopt an invalid native handle");
        }
        return OwnedNativeFile(handle);
    }

    ~OwnedNativeFile() {
        close();
    }

    OwnedNativeFile(const OwnedNativeFile&) = delete;
    OwnedNativeFile& operator=(const OwnedNativeFile&) = delete;

    OwnedNativeFile(OwnedNativeFile&& other) noexcept
        : handle_(std::exchange(other.handle_, invalid_native_handle())) {}

    OwnedNativeFile& operator=(OwnedNativeFile&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, invalid_native_handle());
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept {
        return native_handle_valid(handle_);
    }

    void close() noexcept {
        if (!valid()) {
            return;
        }
#ifdef _WIN32
        if (!::CloseHandle(handle_)) {
            std::fprintf(stderr, "[mmap_file] CloseHandle(owned file) failed: error=%lu\n",
                         static_cast<unsigned long>(::GetLastError()));
        }
#else
        if (::close(handle_) != 0) {
            std::fprintf(stderr, "[mmap_file] close(owned fd=%d) failed: errno=%d\n", handle_,
                         errno);
        }
#endif
        handle_ = invalid_native_handle();
    }

private:
    explicit OwnedNativeFile(NativeHandle handle) noexcept : handle_(handle) {}

    [[nodiscard]] static NativeHandle invalid_native_handle() noexcept {
#ifdef _WIN32
        return INVALID_HANDLE_VALUE;
#else
        return -1;
#endif
    }

    [[nodiscard]] static bool native_handle_valid(NativeHandle handle) noexcept {
#ifdef _WIN32
        return handle != nullptr && handle != INVALID_HANDLE_VALUE;
#else
        return handle >= 0;
#endif
    }

    [[nodiscard]] NativeHandle take_native_handle() noexcept {
        return std::exchange(handle_, invalid_native_handle());
    }

    NativeHandle handle_ = invalid_native_handle();

    friend class MmapFile;
    friend class NativeRandomAccessFile;
};

} // namespace gnfs::util
