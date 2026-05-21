#pragma once

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <stdexcept>

// Windows port status: native mmap is unavailable; the project would need
// `CreateFileMapping` / `MapViewOfFile` from <windows.h>. To keep transitive
// header dependencies compiling on MSVC (so that the rest of the codebase
// builds), this header exposes the same MmapFile interface on Windows but
// every operation throws std::runtime_error at runtime. Callers that touch
// out-of-core (OOC) features will fail at run time with a clear message;
// the in-memory code paths remain fully usable.
#ifdef _WIN32
#define GNFS_MMAP_FILE_UNSUPPORTED 1
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gnfs::util {

#ifdef GNFS_MMAP_FILE_UNSUPPORTED

/// Stub MmapFile for Windows builds. Constructing with a real path throws
/// at runtime; the empty-construction overload is allowed so types
/// containing an MmapFile member can be default-constructed.
class MmapFile {
public:
    MmapFile() = default;
    explicit MmapFile(const std::string& /*path*/) {
        throw std::runtime_error(
            "MmapFile: memory-mapped files are not implemented on Windows. "
            "Recompile without OOC features or run on a POSIX platform.");
    }
    ~MmapFile() = default;
    MmapFile(MmapFile&&) noexcept = default;
    MmapFile& operator=(MmapFile&&) noexcept = default;
    MmapFile(const MmapFile&) = delete;
    MmapFile& operator=(const MmapFile&) = delete;

    void close() noexcept {}
    [[nodiscard]] const uint8_t* data() const noexcept { return nullptr; }
    [[nodiscard]] size_t size() const noexcept { return 0; }
    [[nodiscard]] bool is_open() const noexcept { return false; }

    template <typename T>
    [[nodiscard]] T read_at(size_t /*offset*/) const {
        throw std::runtime_error("MmapFile::read_at unavailable on Windows");
    }
    template <typename T>
    [[nodiscard]] const T* ptr_at(size_t /*offset*/) const {
        throw std::runtime_error("MmapFile::ptr_at unavailable on Windows");
    }
    void advise_random() const {}
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

#endif  // GNFS_MMAP_FILE_UNSUPPORTED

} // namespace gnfs::util
