#pragma once

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gnfs::util {

#ifdef _WIN32

/// RAII wrapper for read-only Windows file mappings.
class MmapFile {
public:
    MmapFile() = default;

    explicit MmapFile(const std::string& path) {
        file_ = ::CreateFileA(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("MmapFile: cannot open '" + path + "': " +
                                     last_error_message());
        }

        LARGE_INTEGER file_size{};
        if (!::GetFileSizeEx(file_, &file_size)) {
            close();
            throw std::runtime_error("MmapFile: GetFileSizeEx failed for '" + path + "': " +
                                     last_error_message());
        }
        if (file_size.QuadPart < 0 ||
            static_cast<unsigned long long>(file_size.QuadPart) >
                static_cast<unsigned long long>(SIZE_MAX)) {
            close();
            throw std::runtime_error("MmapFile: file too large for size_t: " + path);
        }
        size_ = static_cast<size_t>(file_size.QuadPart);

        if (size_ == 0) {
            return;
        }

        mapping_ = ::CreateFileMappingA(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping_ == nullptr) {
            close();
            throw std::runtime_error("MmapFile: CreateFileMapping failed for '" + path + "': " +
                                     last_error_message());
        }

        data_ = static_cast<const uint8_t*>(
            ::MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        if (data_ == nullptr) {
            close();
            throw std::runtime_error("MmapFile: MapViewOfFile failed for '" + path + "': " +
                                     last_error_message());
        }
    }

    ~MmapFile() { close(); }

    MmapFile(MmapFile&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)),
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

    [[nodiscard]] const uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] bool is_open() const noexcept {
        return file_ != INVALID_HANDLE_VALUE;
    }

    template <typename T>
    [[nodiscard]] T read_at(size_t offset) const {
        assert(offset + sizeof(T) <= size_);
        T val;
        std::memcpy(&val, data_ + offset, sizeof(T));
        return val;
    }

    template <typename T>
    [[nodiscard]] const T* ptr_at(size_t offset) const {
        assert(offset <= size_);
        return reinterpret_cast<const T*>(data_ + offset);
    }

    void advise_random() const {}

private:
    static std::string last_error_message() {
        DWORD error = ::GetLastError();
        if (error == 0) return "no error";

        char* buffer = nullptr;
        DWORD len = ::FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<char*>(&buffer), 0, nullptr);
        if (len == 0 || buffer == nullptr) {
            return "Windows error " + std::to_string(error);
        }

        std::string message(buffer, len);
        ::LocalFree(buffer);
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

#else  // POSIX implementation

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
    explicit MmapFile(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) {
            throw std::runtime_error("MmapFile: cannot open '" + path + "'");
        }

        struct stat st;
        if (::fstat(fd_, &st) < 0) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("MmapFile: fstat failed for '" + path + "'");
        }
        size_ = static_cast<size_t>(st.st_size);

        if (size_ == 0) {
            // Empty file: valid but no mapping needed
            data_ = nullptr;
            return;
        }

        data_ = static_cast<const uint8_t*>(
            ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0));
        if (data_ == MAP_FAILED) {
            data_ = nullptr;
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("MmapFile: mmap failed for '" + path + "'");
        }

        // Advise sequential access for relation scanning
        ::madvise(const_cast<uint8_t*>(data_), size_, MADV_SEQUENTIAL);
    }

    ~MmapFile() { close(); }

    // Move-only
    MmapFile(MmapFile&& other) noexcept
        : data_(other.data_), size_(other.size_), fd_(other.fd_) {
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
                std::fprintf(stderr, "[mmap_file] munmap failed: errno=%d size=%zu\n",
                             errno, size_);
            }
        }
        if (fd_ >= 0) {
            int rc = ::close(fd_);
            assert(rc == 0 && "MmapFile::close: close(fd) failed");
            if (rc != 0) {
                std::fprintf(stderr, "[mmap_file] close(fd=%d) failed: errno=%d\n",
                             fd_, errno);
            }
        }
        data_ = nullptr;
        size_ = 0;
        fd_ = -1;
    }

    [[nodiscard]] const uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

    /// Read a typed value at byte offset. No bounds check in release.
    template <typename T>
    [[nodiscard]] T read_at(size_t offset) const {
        assert(offset + sizeof(T) <= size_);
        T val;
        std::memcpy(&val, data_ + offset, sizeof(T));
        return val;
    }

    /// Get a pointer to a contiguous array of T at byte offset.
    template <typename T>
    [[nodiscard]] const T* ptr_at(size_t offset) const {
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
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    int fd_ = -1;
};

#endif  // _WIN32

} // namespace gnfs::util
