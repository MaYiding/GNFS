#pragma once

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <stdexcept>

#ifdef _WIN32
#error "MmapFile: Windows not supported (use CreateFileMapping)"
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gnfs::util {

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

} // namespace gnfs::util
