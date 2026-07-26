#pragma once

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gnfs::util {

class MmapFile;

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
};

#ifdef _WIN32

/// RAII wrapper for read-only Windows file mappings.
class MmapFile {
public:
    MmapFile() = default;

    explicit MmapFile(const std::string& path) : MmapFile(open_read_only(path), path) {}

    /// Consume and map an already-open file without reopening any path.
    /// Ownership transfers only after mapping succeeds.
    explicit MmapFile(OwnedNativeFile&& file)
        : MmapFile(std::move(file), std::string("owned native file")) {}

    ~MmapFile() {
        close();
    }

    MmapFile(MmapFile&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)),
          mapping_(std::exchange(other.mapping_, nullptr)),
          file_(std::exchange(other.file_, INVALID_HANDLE_VALUE)) {}

    MmapFile& operator=(MmapFile&& other) noexcept {
        if (this != &other) {
            close();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            mapping_ = std::exchange(other.mapping_, nullptr);
            file_ = std::exchange(other.file_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    MmapFile(const MmapFile&) = delete;
    MmapFile& operator=(const MmapFile&) = delete;

    void close() noexcept {
        if (data_ != nullptr) {
            if (!::UnmapViewOfFile(data_)) {
                std::fprintf(stderr, "[mmap_file] UnmapViewOfFile failed: error=%lu\n",
                             static_cast<unsigned long>(::GetLastError()));
            }
        }
        if (mapping_ != nullptr) {
            if (!::CloseHandle(mapping_)) {
                std::fprintf(stderr, "[mmap_file] CloseHandle(mapping) failed: error=%lu\n",
                             static_cast<unsigned long>(::GetLastError()));
            }
        }
        if (file_ != INVALID_HANDLE_VALUE) {
            if (!::CloseHandle(file_)) {
                std::fprintf(stderr, "[mmap_file] CloseHandle(file) failed: error=%lu\n",
                             static_cast<unsigned long>(::GetLastError()));
            }
        }
        data_ = nullptr;
        size_ = 0;
        mapping_ = nullptr;
        file_ = INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] const uint8_t* data() const noexcept {
        return data_;
    }
    [[nodiscard]] size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] bool is_open() const noexcept {
        return file_ != INVALID_HANDLE_VALUE;
    }

    template <typename T> [[nodiscard]] T read_at(size_t offset) const {
        assert(offset + sizeof(T) <= size_);
        T val;
        std::memcpy(&val, data_ + offset, sizeof(T));
        return val;
    }

    template <typename T> [[nodiscard]] const T* ptr_at(size_t offset) const {
        assert(offset <= size_);
        return reinterpret_cast<const T*>(data_ + offset);
    }

    void advise_random() const {}

private:
    class PendingMapping final {
    public:
        PendingMapping() = default;
        PendingMapping(const PendingMapping&) = delete;
        PendingMapping& operator=(const PendingMapping&) = delete;

        ~PendingMapping() {
            if (data != nullptr && !::UnmapViewOfFile(data)) {
                std::fprintf(stderr, "[mmap_file] UnmapViewOfFile(pending) failed: error=%lu\n",
                             static_cast<unsigned long>(::GetLastError()));
            }
            if (mapping != nullptr && !::CloseHandle(mapping)) {
                std::fprintf(stderr, "[mmap_file] CloseHandle(pending mapping) failed: error=%lu\n",
                             static_cast<unsigned long>(::GetLastError()));
            }
        }

        const uint8_t* data = nullptr;
        HANDLE mapping = nullptr;
    };

    [[nodiscard]] static OwnedNativeFile open_read_only(const std::string& path) {
        const std::filesystem::path filesystem_path(path);
        HANDLE file = ::CreateFileW(filesystem_path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = ::GetLastError();
            throw std::runtime_error("MmapFile: cannot open '" + path +
                                     "': " + last_error_message(error));
        }
        return OwnedNativeFile::adopt_ownership(file);
    }

    MmapFile(OwnedNativeFile&& file, const std::string& source) {
        if (!file.valid()) {
            throw std::invalid_argument("MmapFile: cannot map an invalid owned native file");
        }

        LARGE_INTEGER file_size{};
        if (!::GetFileSizeEx(file.handle_, &file_size)) {
            const DWORD error = ::GetLastError();
            throw std::runtime_error("MmapFile: GetFileSizeEx failed for '" + source +
                                     "': " + last_error_message(error));
        }
        if (file_size.QuadPart < 0 || static_cast<unsigned long long>(file_size.QuadPart) >
                                          static_cast<unsigned long long>(SIZE_MAX)) {
            throw std::runtime_error("MmapFile: file too large for size_t: " + source);
        }
        const size_t mapped_size = static_cast<size_t>(file_size.QuadPart);

        if (mapped_size == 0) {
            file_ = file.take_native_handle();
            return;
        }

        PendingMapping pending;
        pending.mapping = ::CreateFileMappingW(file.handle_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (pending.mapping == nullptr) {
            const DWORD error = ::GetLastError();
            throw std::runtime_error("MmapFile: CreateFileMapping failed for '" + source +
                                     "': " + last_error_message(error));
        }

        pending.data =
            static_cast<const uint8_t*>(::MapViewOfFile(pending.mapping, FILE_MAP_READ, 0, 0, 0));
        if (pending.data == nullptr) {
            const DWORD error = ::GetLastError();
            throw std::runtime_error("MmapFile: MapViewOfFile failed for '" + source +
                                     "': " + last_error_message(error));
        }

        file_ = file.take_native_handle();
        size_ = mapped_size;
        mapping_ = std::exchange(pending.mapping, nullptr);
        data_ = std::exchange(pending.data, nullptr);
    }

    static std::string last_error_message(DWORD error) {
        if (error == 0)
            return "no error";

        char buffer[1024]{};
        DWORD len = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     buffer, static_cast<DWORD>(sizeof(buffer)), nullptr);
        if (len == 0) {
            return "Windows error " + std::to_string(error);
        }

        std::string message(buffer, len);
        while (!message.empty() &&
               (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
            message.pop_back();
        }
        return message + " (" + std::to_string(error) + ")";
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    HANDLE mapping_ = nullptr;
    HANDLE file_ = INVALID_HANDLE_VALUE;
};

#else // POSIX implementation

/// RAII wrapper for memory-mapped files (read-only).
///
/// Maps a file into the process address space for zero-copy access.
/// Suitable for large relation files and CSR matrix data where
/// sequential/random reads dominate and writes are not needed.
///
/// On close, the mapping is automatically unmapped and the fd closed.
class MmapFile {
public:
    MmapFile() = default;

    /// Open and map a file read-only. Throws on failure.
    explicit MmapFile(const std::string& path) : MmapFile(open_read_only(path), path) {}

    /// Consume and map an already-open file without reopening any path.
    /// Ownership transfers only after mapping succeeds.
    explicit MmapFile(OwnedNativeFile&& file)
        : MmapFile(std::move(file), std::string("owned native file")) {}

    ~MmapFile() {
        close();
    }

    // Move-only
    MmapFile(MmapFile&& other) noexcept : data_(other.data_), size_(other.size_), fd_(other.fd_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.fd_ = -1;
    }
    MmapFile& operator=(MmapFile&& other) noexcept {
        if (this != &other) {
            close();
            data_ = other.data_;
            size_ = other.size_;
            fd_ = other.fd_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.fd_ = -1;
        }
        return *this;
    }
    MmapFile(const MmapFile&) = delete;
    MmapFile& operator=(const MmapFile&) = delete;

    void close() noexcept {
        if (data_ && size_ > 0) {
            // munmap can fail with EINVAL (already unmapped, bad addr) — bug, not
            // recoverable here. Assert in Debug; log + continue in Release so
            // close() stays noexcept (called from dtor).
            int rc = ::munmap(const_cast<uint8_t*>(data_), size_);
            assert(rc == 0 && "MmapFile::close: munmap failed");
            if (rc != 0) {
                // Best-effort warning; can't throw from noexcept.
                std::fprintf(stderr, "[mmap_file] munmap failed: errno=%d size=%zu\n", errno,
                             size_);
            }
        }
        if (fd_ >= 0) {
            int rc = ::close(fd_);
            assert(rc == 0 && "MmapFile::close: close(fd) failed");
            if (rc != 0) {
                std::fprintf(stderr, "[mmap_file] close(fd=%d) failed: errno=%d\n", fd_, errno);
            }
        }
        data_ = nullptr;
        size_ = 0;
        fd_ = -1;
    }

    [[nodiscard]] const uint8_t* data() const noexcept {
        return data_;
    }
    [[nodiscard]] size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] bool is_open() const noexcept {
        return fd_ >= 0;
    }

    /// Read a typed value at byte offset. No bounds check in release.
    template <typename T> [[nodiscard]] T read_at(size_t offset) const {
        assert(offset + sizeof(T) <= size_);
        T val;
        std::memcpy(&val, data_ + offset, sizeof(T));
        return val;
    }

    /// Get a pointer to a contiguous array of T at byte offset.
    template <typename T> [[nodiscard]] const T* ptr_at(size_t offset) const {
        assert(offset <= size_);
        return reinterpret_cast<const T*>(data_ + offset);
    }

    /// Switch madvise hint (e.g., MADV_RANDOM for random access patterns)
    void advise_random() const {
        if (data_ && size_ > 0) {
            ::madvise(const_cast<uint8_t*>(data_), size_, MADV_RANDOM);
        }
    }

private:
    [[nodiscard]] static OwnedNativeFile open_read_only(const std::string& path) {
        const int descriptor = ::open(path.c_str(), O_RDONLY);
        if (descriptor < 0) {
            throw std::runtime_error("MmapFile: cannot open '" + path + "'");
        }
        return OwnedNativeFile::adopt_ownership(descriptor);
    }

    MmapFile(OwnedNativeFile&& file, const std::string& source) {
        if (!file.valid()) {
            throw std::invalid_argument("MmapFile: cannot map an invalid owned native file");
        }

        struct stat metadata{};
        if (::fstat(file.handle_, &metadata) < 0) {
            throw std::runtime_error("MmapFile: fstat failed for '" + source + "'");
        }
        if (metadata.st_size < 0 ||
            static_cast<std::uintmax_t>(metadata.st_size) > static_cast<std::uintmax_t>(SIZE_MAX)) {
            throw std::runtime_error("MmapFile: file too large for size_t: " + source);
        }
        const size_t mapped_size = static_cast<size_t>(metadata.st_size);

        if (mapped_size == 0) {
            // Empty file: valid but no mapping needed
            fd_ = file.take_native_handle();
            return;
        }

        const auto* mapped_data = static_cast<const uint8_t*>(
            ::mmap(nullptr, mapped_size, PROT_READ, MAP_PRIVATE, file.handle_, 0));
        if (mapped_data == MAP_FAILED) {
            throw std::runtime_error("MmapFile: mmap failed for '" + source + "'");
        }

        // Advise sequential access for relation scanning
        ::madvise(const_cast<uint8_t*>(mapped_data), mapped_size, MADV_SEQUENTIAL);

        fd_ = file.take_native_handle();
        size_ = mapped_size;
        data_ = mapped_data;
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    int fd_ = -1;
};

#endif // _WIN32

} // namespace gnfs::util
